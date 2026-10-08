package io.github.berathegrey.yzdac.player

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.hardware.usb.UsbManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.VolumeProvider
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import io.github.berathegrey.yzdac.Prefs
import io.github.berathegrey.yzdac.R
import io.github.berathegrey.yzdac.ui.MainActivity
import io.github.berathegrey.yzdac.ui.UsbAttachActivity
import io.github.berathegrey.yzdac.usb.DacManager
import io.github.berathegrey.yzdac.usb.DacSession
import io.github.berathegrey.yzdac.usb.StreamConfig
import io.github.berathegrey.yzdac.usb.VolumeCurve
import io.github.berathegrey.yzdac.usb.db256ToDb
import kotlin.math.pow

/**
 * Exclusive-mode player, the same idea as USB Audio Player PRO: Android's
 * audio stack is bypassed completely and decoded PCM is streamed to the DAC
 * by our own USB driver. While it runs, the DAC is detached from Android
 * (system sounds fall back to the phone speaker).
 *
 * Volume is the DAC's own hardware attenuator, so the digital signal stays
 * bit-perfect; the hardware volume keys drive it through a remote
 * MediaSession VolumeProvider.
 */
class PlayerService : Service() {

    data class State(
        val playing: Boolean = false,
        val paused: Boolean = false,
        val title: String = "",
        val index: Int = 0,
        val count: Int = 0,
        val positionMs: Long = 0,
        val durationMs: Long = 0,
        val config: StreamConfig? = null,
        val volumeStep: Int = 0,
        val volumeSteps: Int = VolumeCurve.DEFAULT_STEPS,
        val volumeDb: Float = 0f,
        val error: String? = null,
    )

    private lateinit var prefs: Prefs
    private lateinit var mediaSession: MediaSession
    private lateinit var audioManager: AudioManager
    private var focusRequest: AudioFocusRequest? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var volumeProvider: VolumeProvider? = null
    @Volatile private var curve = VolumeCurve(-60f, 0f)

    @Volatile private var playlist: List<Uri> = emptyList()
    @Volatile private var index = 0
    private var decodeThread: Thread? = null
    private val control = TrackDecoder.Control()

    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            val d = UsbAttachActivity.usbDevice(intent)
            val s = DacManager.session
            if (s == null || d == null || s.device.deviceName == d.deviceName) {
                update { it.copy(error = "DAC çıkarıldı") }
                stopPlayback()
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        instance = this
        createChannel()
        mediaSession = MediaSession(this, "YzDac").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() = setPaused(false)
                override fun onPause() = setPaused(true)
                override fun onStop() = stopPlayback()
                override fun onSkipToNext() = skip(1)
                override fun onSkipToPrevious() = skip(-1)
                override fun onSeekTo(pos: Long) = seekTo(pos)
            })
            isActive = true
        }
        val filter = IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(detachReceiver, filter, Context.RECEIVER_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(detachReceiver, filter)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        when (intent?.action) {
            ACTION_PLAY -> {
                val uris = intent.getStringArrayListExtra(EXTRA_URIS)?.map(Uri::parse).orEmpty()
                if (uris.isNotEmpty()) startPlaylist(uris, intent.getIntExtra(EXTRA_INDEX, 0))
            }
            ACTION_TOGGLE -> setPaused(!state.paused)
            ACTION_NEXT -> skip(1)
            ACTION_PREV -> skip(-1)
            ACTION_STOP -> stopPlayback()
        }
        if (decodeThread == null && intent?.action != ACTION_PLAY) stopSelf()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopDecoder()
        runCatching { unregisterReceiver(detachReceiver) }
        mediaSession.release()
        abandonFocus()
        releaseWakeLock()
        instance = null
        isRunning = false
        publish(State())
        super.onDestroy()
    }

    // ---- playback control --------------------------------------------------

    private fun startPlaylist(uris: List<Uri>, start: Int) {
        val session = DacManager.session
        if (session == null) {
            publish(State(error = "Önce DAC'ı bağlayıp izin verin"))
            stopSelf()
            return
        }
        stopDecoder()
        playlist = uris
        index = start.coerceIn(0, uris.size - 1)
        requestFocus()
        acquireWakeLock()
        isRunning = true
        control.reset()
        val thread = Thread({ decodeLoop(session) }, "yzdac-decoder")
        thread.priority = Thread.MAX_PRIORITY
        decodeThread = thread
        thread.start()
    }

    fun setPaused(paused: Boolean) {
        if (decodeThread == null) return
        control.paused = paused
        update { it.copy(paused = paused) }
    }

    fun skip(delta: Int) {
        if (decodeThread == null) return
        control.skip(delta)
    }

    fun seekTo(ms: Long) {
        control.seekUs = ms * 1000
    }

    fun stopPlayback() {
        stopDecoder()
        stopSelf()
    }

    private fun stopDecoder() {
        control.stop = true
        control.paused = false
        val t = decodeThread
        decodeThread = null
        DacManager.session?.stopStream()  // also wakes a blocked write
        t?.join(3000)
        isRunning = false
    }

    private fun decodeLoop(session: DacSession) {
        var lastConfig: StreamConfig? = null
        volumeReady = false
        try {
            while (!control.stop && index in playlist.indices) {
                val uri = playlist[index]
                val title = displayName(uri)
                update { it.copy(playing = true, paused = control.paused, title = title, index = index, count = playlist.size, error = null) }
                updateMetadata(title)
                val decoder = TrackDecoder(this, uri, session, control, prefs.forcedRate,
                    onConfig = { cfg ->
                        lastConfig = cfg
                        update { it.copy(config = cfg) }
                        if (cfg.ok) onStreamReady(session, cfg)
                    },
                    onProgress = { pos, dur ->
                        update { it.copy(positionMs = pos, durationMs = dur) }
                        updatePlaybackState()
                    })
                val result = decoder.run()
                when (result) {
                    is TrackDecoder.Result.Error -> {
                        update { it.copy(error = "$title: ${result.message}") }
                        if (result.fatal) break
                        index++
                    }
                    TrackDecoder.Result.Finished -> index++
                    is TrackDecoder.Result.Skipped -> index = (index + result.delta).coerceAtLeast(0)
                    TrackDecoder.Result.Stopped -> break
                }
                control.clearSkip()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "decoder crashed", t)
            update { it.copy(error = t.message) }
        } finally {
            session.stopStream()  // hands the DAC back to Android
            update { it.copy(playing = false, paused = false, config = lastConfig) }
            mediaSession.setPlaybackState(PlaybackState.Builder().setState(PlaybackState.STATE_STOPPED, 0, 0f).build())
            if (!control.stop) android.os.Handler(mainLooper).post { stopPlayback() }
        }
    }

    // ---- volume -------------------------------------------------------------

    /** True once the hardware volume curve is known and the stream owns the DAC. */
    @Volatile private var volumeReady = false

    /**
     * Called from the decoder thread right after the stream started, before
     * any audio is queued. Reads the DAC's real range, applies the remembered
     * level (first time: 20 dB below max, for hearing safety) and hands the
     * hardware volume keys over to the DAC through a remote VolumeProvider.
     */
    private fun onStreamReady(session: DacSession, cfg: StreamConfig) {
        applyGain(session, cfg)
        if (volumeReady) return
        val ch = session.refreshInfo().volume.firstOrNull { it.rangeValid }
        curve = if (ch != null) VolumeCurve(db256ToDb(ch.min), db256ToDb(ch.max)) else VolumeCurve(-60f, 0f)
        var db = prefs.exclusiveVolumeDb
        if (db.isNaN()) db = curve.maxDb - 20f
        val step = curve.stepForDb(db)
        session.applyDb(curve.dbForStep(step), false, prefs.reattachMode)
        volumeReady = true
        update { it.copy(volumeStep = step, volumeSteps = curve.steps, volumeDb = curve.dbForStep(step)) }
        android.os.Handler(mainLooper).post {
            val vp = object : VolumeProvider(VOLUME_CONTROL_ABSOLUTE, curve.steps, step) {
                override fun onSetVolumeTo(volume: Int) = setVolumeStep(volume)
                override fun onAdjustVolume(direction: Int) = setVolumeStep(currentVolume + direction)
            }
            volumeProvider = vp
            mediaSession.setPlaybackToRemote(vp)
        }
    }

    fun setVolumeStep(step: Int) {
        if (!volumeReady) return
        val s = step.coerceIn(0, curve.steps)
        val db = curve.dbForStep(s)
        prefs.exclusiveVolumeDb = db
        volumeProvider?.currentVolume = s
        update { it.copy(volumeStep = s, volumeDb = db) }
        val session = DacManager.session ?: return
        DacManager.worker.execute { session.applyDb(db, false, prefs.reattachMode) }
    }

    fun setPreGain(db: Float) {
        prefs.preGainDb = db
        val session = DacManager.session ?: return
        state.config?.let { applyGain(session, it) }
    }

    private fun applyGain(session: DacSession, cfg: StreamConfig) {
        val db = prefs.preGainDb
        // Pre-gain (loudness boost for quiet recordings) uses a soft clipper;
        // 0 dB keeps the path bit-perfect.
        session.setDigitalGain(10f.pow(db / 20f), db > 0f)
        val shown = if (cfg.ok && db > 0f) cfg.copy(bitPerfect = false) else cfg
        update { it.copy(config = shown) }
    }

    // ---- system integration -------------------------------------------------

    private fun requestFocus() {
        val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
            )
            .setOnAudioFocusChangeListener { change ->
                if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) setPaused(true)
            }
            .build()
        focusRequest = req
        audioManager.requestAudioFocus(req)
    }

    private fun abandonFocus() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "yzdac:playback").apply {
            setReferenceCounted(false)
            acquire(6 * 60 * 60 * 1000L)
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun displayName(uri: Uri): String {
        runCatching {
            contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) return c.getString(0)
            }
        }
        return uri.lastPathSegment ?: uri.toString()
    }

    private fun updateMetadata(title: String) {
        mediaSession.setMetadata(
            MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "YZ DAC exclusive")
                .build(),
        )
        updatePlaybackState()
        notifyNotification()
    }

    private var lastStateUpdate = 0L

    private fun updatePlaybackState() {
        val now = System.currentTimeMillis()
        if (now - lastStateUpdate < 1000) return
        lastStateUpdate = now
        val st = if (state.paused) PlaybackState.STATE_PAUSED else PlaybackState.STATE_PLAYING
        mediaSession.setPlaybackState(
            PlaybackState.Builder()
                .setActions(
                    PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
                        PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or
                        PlaybackState.ACTION_STOP or PlaybackState.ACTION_SEEK_TO,
                )
                .setState(st, state.positionMs, if (state.paused) 0f else 1f)
                .build(),
        )
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Exclusive çalma", NotificationManager.IMPORTANCE_LOW))
    }

    private fun servicePending(action: String, code: Int): PendingIntent =
        PendingIntent.getService(this, code, Intent(this, PlayerService::class.java).setAction(action), PendingIntent.FLAG_IMMUTABLE)

    private fun action(icon: Int, title: String, action: String, code: Int): Notification.Action =
        Notification.Action.Builder(Icon.createWithResource(this, icon), title, servicePending(action, code)).build()

    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val toggle = if (state.paused) action(android.R.drawable.ic_media_play, "Çal", ACTION_TOGGLE, 2)
        else action(android.R.drawable.ic_media_pause, "Duraklat", ACTION_TOGGLE, 2)
        val b = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_dac)
            .setContentTitle(state.title.ifEmpty { "YZ DAC" })
            .setContentText(state.config?.summary ?: "Exclusive USB modu")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(action(android.R.drawable.ic_media_previous, "Önceki", ACTION_PREV, 1))
            .addAction(toggle)
            .addAction(action(android.R.drawable.ic_media_next, "Sonraki", ACTION_NEXT, 3))
            .addAction(action(android.R.drawable.ic_menu_close_clear_cancel, "Durdur", ACTION_STOP, 4))
            .setStyle(Notification.MediaStyle().setMediaSession(mediaSession.sessionToken).setShowActionsInCompactView(0, 1, 2))
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        return b.build()
    }

    private fun startForegroundCompat() {
        val n = buildNotification()
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        else startForeground(NOTIFICATION_ID, n)
    }

    private var lastNotified = ""

    private fun notifyNotification() {
        val key = "${state.title}|${state.paused}|${state.config?.summary}"
        if (key == lastNotified) return
        lastNotified = key
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification())
    }

    private val stateLock = Any()

    private fun update(f: (State) -> State) {
        synchronized(stateLock) { state = f(state) }
        changed()
    }

    private fun publish(s: State) {
        synchronized(stateLock) { state = s }
        changed()
    }

    private fun changed() {
        DacManager.notifyChanged()
        if (::mediaSession.isInitialized) android.os.Handler(mainLooper).post { notifyNotification() }
    }

    companion object {
        private const val TAG = "YzDac"
        private const val CHANNEL = "playback"
        private const val NOTIFICATION_ID = 42
        const val ACTION_PLAY = "io.github.berathegrey.yzdac.PLAY"
        const val ACTION_TOGGLE = "io.github.berathegrey.yzdac.TOGGLE"
        const val ACTION_NEXT = "io.github.berathegrey.yzdac.NEXT"
        const val ACTION_PREV = "io.github.berathegrey.yzdac.PREV"
        const val ACTION_STOP = "io.github.berathegrey.yzdac.STOP"
        const val EXTRA_URIS = "uris"
        const val EXTRA_INDEX = "index"

        @Volatile var instance: PlayerService? = null
            private set
        @Volatile var isRunning = false
            private set
        @Volatile var state = State()
            private set

        fun play(ctx: Context, uris: List<Uri>, index: Int = 0) {
            val i = Intent(ctx, PlayerService::class.java).setAction(ACTION_PLAY)
                .putStringArrayListExtra(EXTRA_URIS, ArrayList(uris.map(Uri::toString)))
                .putExtra(EXTRA_INDEX, index)
            ctx.startForegroundService(i)
        }

        fun command(ctx: Context, action: String) {
            if (instance == null) return
            ctx.startService(Intent(ctx, PlayerService::class.java).setAction(action))
        }
    }
}
