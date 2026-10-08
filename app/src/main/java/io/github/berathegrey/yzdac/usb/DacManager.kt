package io.github.berathegrey.yzdac.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import io.github.berathegrey.yzdac.Prefs
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.Executors

/**
 * Process-wide owner of the currently opened DAC. The activity, the attach
 * handler and the playback service all go through here so the device is only
 * opened once.
 */
object DacManager {
    private const val TAG = "YzDac"
    private const val ACTION_USB_PERMISSION = "io.github.berathegrey.yzdac.USB_PERMISSION"

    /** Single worker for USB control operations; they block for up to ~1 s. */
    val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "yzdac-usb").apply { isDaemon = true } }
    private val main = Handler(Looper.getMainLooper())

    @Volatile
    var session: DacSession? = null
        private set

    private val listeners = CopyOnWriteArraySet<() -> Unit>()

    fun addListener(l: () -> Unit) = listeners.add(l)
    fun removeListener(l: () -> Unit) = listeners.remove(l)
    fun notifyChanged() = main.post { listeners.forEach { it() } }

    fun usbManager(ctx: Context) = ctx.getSystemService(Context.USB_SERVICE) as UsbManager

    fun isAudioDevice(d: UsbDevice): Boolean = (0 until d.interfaceCount).any {
        val i = d.getInterface(it)
        i.interfaceClass == UsbConstants.USB_CLASS_AUDIO && i.interfaceSubclass == 1
    }

    fun audioDevices(ctx: Context): List<UsbDevice> =
        usbManager(ctx).deviceList.values.filter(::isAudioDevice)
            .sortedByDescending { it.vendorId == DeviceInfo.APPLE_VID && it.productId == DeviceInfo.APPLE_DONGLE_PID }

    fun requestPermission(ctx: Context, device: UsbDevice, onResult: (Boolean) -> Unit) {
        val manager = usbManager(ctx)
        if (manager.hasPermission(device)) {
            onResult(true)
            return
        }
        val app = ctx.applicationContext
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, intent: Intent) {
                if (intent.action != ACTION_USB_PERMISSION) return
                runCatching { app.unregisterReceiver(this) }
                onResult(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) || manager.hasPermission(device))
            }
        }
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") app.registerReceiver(receiver, filter)
        // Mutable so the system can attach EXTRA_DEVICE / EXTRA_PERMISSION_GRANTED;
        // explicit (package set) as required for mutable PendingIntents on API 34+.
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val pi = PendingIntent.getBroadcast(app, 0, Intent(ACTION_USB_PERMISSION).setPackage(app.packageName), flags)
        manager.requestPermission(device, pi)
    }

    /** Opens (or reuses) the session for [device]. Must be called off the main thread. */
    @Synchronized
    fun open(ctx: Context, device: UsbDevice): Result<DacSession> {
        session?.let {
            if (it.isOpen && it.device.deviceName == device.deviceName) return Result.success(it)
            it.close()
            session = null
        }
        return DacSession.open(usbManager(ctx), device).onSuccess {
            session = it
            Log.i(TAG, "opened ${it.displayName}: ${it.info}")
            notifyChanged()
        }
    }

    @Synchronized
    fun onDetached(device: UsbDevice?) {
        val s = session ?: return
        if (device == null || s.device.deviceName == device.deviceName) {
            runCatching { s.close() }
            session = null
            notifyChanged()
        }
    }

    /**
     * System-wide mode: sets the DAC's hardware volume (to its max by default)
     * and hands the device back to Android, so every app benefits. Other
     * players are paused briefly through transient audio focus because the
     * sound card disappears for about a second.
     */
    fun applyConfiguredVolume(ctx: Context, s: DacSession): VolumeApplyResult {
        val prefs = Prefs(ctx)
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val focus = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            .build()
        val gotFocus = am.requestAudioFocus(focus) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        try {
            val result = if (prefs.useMax) s.applyMax(prefs.reattachMode)
            else s.applyDb(prefs.customDb, prefs.allowBeyondMax, prefs.reattachMode)
            prefs.lastResult = result.summary
            notifyChanged()
            return result
        } finally {
            if (gotFocus) {
                // Give Android time to re-create the USB sound card before players resume.
                main.postDelayed({ am.abandonAudioFocusRequest(focus) }, 1500)
            }
        }
    }
}
