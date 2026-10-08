package io.github.berathegrey.yzdac.ui

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import io.github.berathegrey.yzdac.Prefs
import io.github.berathegrey.yzdac.player.PlayerService
import io.github.berathegrey.yzdac.usb.DacManager
import io.github.berathegrey.yzdac.usb.DacSession
import io.github.berathegrey.yzdac.usb.ReattachMode
import io.github.berathegrey.yzdac.usb.VolumeCurve
import io.github.berathegrey.yzdac.usb.db256ToDb
import io.github.berathegrey.yzdac.usb.formatRate
import kotlin.math.roundToInt

/** Programmatic UI (no AndroidX / layout XML) to keep the app dependency-free. */
class MainActivity : Activity() {

    private lateinit var prefs: Prefs
    private val main = Handler(Looper.getMainLooper())

    private lateinit var status: TextView
    private lateinit var result: TextView
    private lateinit var customLabel: TextView
    private lateinit var customSeek: SeekBar
    private lateinit var nowPlaying: TextView
    private lateinit var pathInfo: TextView
    private lateinit var stats: TextView
    private lateinit var volLabel: TextView
    private lateinit var volSeek: SeekBar
    private lateinit var gainLabel: TextView
    private lateinit var gainSeek: SeekBar
    private lateinit var playPause: Button
    private lateinit var deviceDump: TextView

    private var busy = false
    private var userDraggingVolume = false
    private var customMinDb = -60f
    private var customMaxDb = 0f

    private val listener: () -> Unit = { refresh() }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> DacManager.onDetached(UsbAttachActivity.usbDevice(intent))
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> main.postDelayed({ connect(auto = true) }, 500)
            }
        }
    }

    private val ticker = object : Runnable {
        override fun run() {
            refreshPlayer()
            main.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        setContentView(buildUi())
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(usbReceiver, filter)

        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)

        if (!prefs.warningAccepted) showWarning() else connect(auto = true)
    }

    override fun onResume() {
        super.onResume()
        DacManager.addListener(listener)
        main.post(ticker)
        refresh()
    }

    override fun onPause() {
        DacManager.removeListener(listener)
        main.removeCallbacks(ticker)
        super.onPause()
    }

    override fun onDestroy() {
        runCatching { unregisterReceiver(usbReceiver) }
        super.onDestroy()
    }

    // ---- actions -----------------------------------------------------------

    private fun showWarning() {
        AlertDialog.Builder(this)
            .setTitle("İşitme güvenliği")
            .setMessage(
                "Bu uygulama DAC'ın donanım ses kısıtlamasını kaldırır ve exclusive modda Android'in " +
                    "\"yüksek ses uyarısı\" ile \"ses dozu (CSD)\" sınırlamalarını devre dışı bırakır.\n\n" +
                    "Yüksek ses kalıcı işitme kaybına yol açabilir. Kulaklığı takmadan önce sesi kısın, " +
                    "hassas (IEM) kulaklıklarda dikkatli olun.",
            )
            .setCancelable(false)
            .setPositiveButton("Anladım") { _, _ ->
                prefs.warningAccepted = true
                connect(auto = true)
            }
            .setNegativeButton("Çık") { _, _ -> finish() }
            .show()
    }

    private fun connect(auto: Boolean) {
        val existing = DacManager.session
        if (existing != null && existing.isOpen) {
            refresh()
            return
        }
        val devices = DacManager.audioDevices(this)
        if (devices.isEmpty()) {
            if (!auto) toastResult("USB ses cihazı bulunamadı. Dongle'ı takın.")
            refresh()
            return
        }
        val device = intent?.let { UsbAttachActivity.usbDevice(it) }?.takeIf { d -> devices.any { it.deviceName == d.deviceName } }
            ?: devices.first()
        DacManager.requestPermission(this, device) { granted ->
            if (!granted) {
                toastResult("USB izni reddedildi")
                return@requestPermission
            }
            runBusy("Bağlanıyor…") {
                DacManager.open(this, device).fold(
                    onSuccess = { "Bağlandı: ${it.displayName}" },
                    onFailure = { "Açılamadı: ${it.message}" },
                )
            }
        }
    }

    private fun withSession(block: (DacSession) -> String) {
        val s = DacManager.session
        if (s == null) {
            connect(auto = false)
            return
        }
        runBusy("Çalışıyor…") { block(s) }
    }

    private fun runBusy(label: String, work: () -> String) {
        if (busy) return
        busy = true
        result.text = label
        DacManager.worker.execute {
            val msg = runCatching(work).getOrElse { "Hata: ${it.message}" }
            main.post {
                busy = false
                toastResult(msg)
                refresh()
            }
        }
    }

    private fun toastResult(msg: String) {
        result.text = msg
        prefs.lastResult = msg
    }

    private fun applyMax() = withSession { s ->
        prefs.useMax = true
        val r = DacManager.applyConfiguredVolume(this, s)
        (if (r.ok) "✔ Donanım sesi maksimumda\n" else "✘ ") + r.summary
    }

    private fun applyCustom() = withSession { s ->
        prefs.useMax = false
        val r = DacManager.applyConfiguredVolume(this, s)
        (if (r.ok) "✔ Uygulandı\n" else "✘ ") + r.summary
    }

    private fun readVolume() = withSession { s ->
        val ch = s.readVolume(prefs.reattachMode)
        if (ch.isEmpty()) "Okunabilir ses kontrolü yok"
        else ch.joinToString("\n") {
            if (it.rangeValid) "%s: şu an %.2f dB (aralık %.2f … %.2f dB, adım %.3f)".format(
                it.label, db256ToDb(it.cur), db256ToDb(it.min), db256ToDb(it.max), db256ToDb(it.res),
            ) else "${it.label}: yalnızca mute"
        }
    }

    private fun probe() {
        AlertDialog.Builder(this)
            .setTitle("Maksimum ötesi testi (deneysel)")
            .setMessage(
                "Cihazın bildirdiği maksimumun üstündeki değerler 1 dB adımlarla (en fazla +12 dB) denenir ve " +
                    "cihazın gerçekten kabul ettiği değer okunur. Çoğu DAC (Apple dongle dahil) bunu reddeder; " +
                    "bu durumda hiçbir şey değişmez. Test sırasında müziği durdurun ve kulaklığı çıkarın.",
            )
            .setPositiveButton("Test et") { _, _ ->
                withSession { s ->
                    val r = s.probeAboveMax(12, prefs.allowBeyondMax, prefs.reattachMode)
                    r.summary + if (r.anyAboveMax) {
                        if (prefs.allowBeyondMax) "\nEn yüksek kabul edilen değer uygulandı."
                        else "\nUygulamak için \"maksimum üstüne izin ver\" seçeneğini açıp tekrar test edin."
                    } else "\nCihaz rapor ettiği maksimumun üstünü kabul etmiyor (donanım/firmware sınırı)."
                }
            }
            .setNegativeButton("Vazgeç", null)
            .show()
    }

    private val pickAudio = 7

    private fun pickFiles() {
        if (DacManager.session == null) {
            connect(auto = false)
            return
        }
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "audio/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        @Suppress("DEPRECATION")
        startActivityForResult(i, pickAudio)
    }

    @Deprecated("Activity result API without AndroidX")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pickAudio || resultCode != RESULT_OK || data == null) return
        val uris = mutableListOf<Uri>()
        data.clipData?.let { cd -> for (k in 0 until cd.itemCount) uris.add(cd.getItemAt(k).uri) }
        if (uris.isEmpty()) data.data?.let(uris::add)
        uris.forEach { runCatching { contentResolver.takePersistableUriPermission(it, Intent.FLAG_GRANT_READ_URI_PERMISSION) } }
        if (uris.isNotEmpty()) PlayerService.play(this, uris)
    }

    // ---- UI ----------------------------------------------------------------

    private fun dp(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun text(s: String = "", size: Float = 14f, bold: Boolean = false, mono: Boolean = false) = TextView(this).apply {
        text = s
        textSize = size
        if (bold) setTypeface(typeface, Typeface.BOLD)
        if (mono) typeface = Typeface.MONOSPACE
        setPadding(0, dp(4), 0, dp(4))
        setTextIsSelectable(mono)
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        isAllCaps = false
        setOnClickListener { onClick() }
    }

    private fun row(vararg views: View) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        views.forEach { addView(it, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)) }
    }

    private fun LinearLayout.section(title: String) {
        addView(View(context).apply { setBackgroundColor(0x33888888) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)).apply {
            topMargin = dp(16)
            bottomMargin = dp(8)
        })
        addView(text(title, 18f, bold = true))
    }

    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(32))
        }
        root.addView(text("YZ USB DAC", 24f, bold = true))
        status = text("", 14f)
        root.addView(status)
        root.addView(button("DAC'a bağlan / yenile") { connect(auto = false) })

        // --- system-wide mode
        root.section("1 · Sistem geneli mod (Spotify, YouTube… tüm uygulamalar)")
        root.addView(text(
            "Android, USB DAC'ın donanım ses seviyesini ayarlamaz; Apple dongle düşük bir varsayılanla açılır. " +
                "Bu buton DAC'ın kendi ses kontrolünü (UAC Feature Unit) maksimuma çeker ve cihazı Android'e geri verir. " +
                "Ses seviyesini sonra telefonun ses tuşlarıyla normal şekilde ayarlarsınız.", 13f,
        ))
        root.addView(button("Donanım sesini MAKSİMUMA ayarla") { applyMax() })
        root.addView(Switch(this).apply {
            text = "DAC takıldığında otomatik uygula"
            isChecked = prefs.autoApplyOnAttach
            setOnCheckedChangeListener { _, c -> prefs.autoApplyOnAttach = c }
        })
        root.addView(text("İpucu: Dongle'ı takınca çıkan pencerede \"YZ DAC\" + \"Her zaman\" seçerseniz izin sorulmadan otomatik uygulanır.", 12f))

        customLabel = text("Özel seviye: 0.0 dB")
        root.addView(customLabel)
        customSeek = SeekBar(this).apply {
            max = 600
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    val db = customMinDb + (customMaxDb - customMinDb) * p / 600f
                    customLabel.text = "Özel seviye: %.1f dB".format(db)
                    if (fromUser) prefs.customDb = (db * 2).roundToInt() / 2f
                }
                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            })
        }
        root.addView(customSeek)
        root.addView(row(button("Özel seviyeyi uygula") { applyCustom() }, button("Mevcut değeri oku") { readVolume() }))

        root.addView(text("USB sürücüsünü geri verme yöntemi", 13f, bold = true))
        val rg = RadioGroup(this)
        val rbAttach = RadioButton(this).apply { text = "Sürücüyü yeniden bağla (önerilen)"; id = View.generateViewId() }
        val rbReset = RadioButton(this).apply { text = "USB reset (usbDacVolumeAndroid yöntemi)"; id = View.generateViewId() }
        rg.addView(rbAttach)
        rg.addView(rbReset)
        rg.check(if (prefs.reattachMode == ReattachMode.USB_RESET) rbReset.id else rbAttach.id)
        rg.setOnCheckedChangeListener { _, id -> prefs.reattachMode = if (id == rbReset.id) ReattachMode.USB_RESET else ReattachMode.ATTACH_DRIVER }
        root.addView(rg)

        root.addView(Switch(this).apply {
            text = "Rapor edilen maksimumun üstüne izin ver (deneysel)"
            isChecked = prefs.allowBeyondMax
            setOnCheckedChangeListener { _, c -> prefs.allowBeyondMax = c }
        })
        root.addView(button("Maksimum ötesini test et (deneysel)") { probe() })

        result = text(prefs.lastResult, 13f, mono = true)
        root.addView(result)

        // --- exclusive player
        root.section("2 · Exclusive bit-perfect oynatıcı (UAPP modu)")
        root.addView(text(
            "Android ses sistemi tamamen atlanır: mikser, 48 kHz'e zorunlu yeniden örnekleme, ses eğrileri, " +
                "AB \"güvenli ses\" uyarısı ve ses dozu (CSD) sınırlaması devre dışıdır. Ses doğrudan kendi USB " +
                "sürücümüzle DAC'a gönderilir; ses seviyesi DAC'ın donanım zayıflatıcısıyla ayarlanır (sinyal bit-perfect kalır). " +
                "Çalarken diğer uygulamaların sesi telefon hoparlöründen çıkar.", 13f,
        ))
        root.addView(button("Müzik dosyası seç ve çal") { pickFiles() })
        nowPlaying = text("", 15f, bold = true)
        pathInfo = text("", 13f, mono = true)
        stats = text("", 12f, mono = true)
        root.addView(nowPlaying)
        root.addView(pathInfo)
        root.addView(stats)
        playPause = button("Duraklat") { PlayerService.command(this, PlayerService.ACTION_TOGGLE) }
        root.addView(row(
            button("⏮") { PlayerService.command(this, PlayerService.ACTION_PREV) },
            playPause,
            button("⏹") { PlayerService.command(this, PlayerService.ACTION_STOP) },
            button("⏭") { PlayerService.command(this, PlayerService.ACTION_NEXT) },
        ))
        volLabel = text("Donanım sesi: –")
        root.addView(volLabel)
        volSeek = SeekBar(this).apply {
            max = VolumeCurve.DEFAULT_STEPS
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    if (fromUser) PlayerService.instance?.setVolumeStep(p)
                }
                override fun onStartTrackingTouch(sb: SeekBar) { userDraggingVolume = true }
                override fun onStopTrackingTouch(sb: SeekBar) { userDraggingVolume = false }
            })
        }
        root.addView(volSeek)
        gainLabel = text("")
        root.addView(gainLabel)
        gainSeek = SeekBar(this).apply {
            max = 24  // 0 … +12 dB in 0.5 dB steps
            progress = (prefs.preGainDb * 2).roundToInt()
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    updateGainLabel(p / 2f)
                    if (fromUser) {
                        prefs.preGainDb = p / 2f
                        PlayerService.instance?.setPreGain(p / 2f)
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            })
        }
        updateGainLabel(prefs.preGainDb)
        root.addView(gainSeek)

        root.addView(text("DAC örnekleme hızı", 13f, bold = true))
        val rates = listOf(0, 44100, 48000, 88200, 96000, 176400, 192000)
        root.addView(Spinner(this).apply {
            adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item,
                rates.map { if (it == 0) "Otomatik (kaynakla aynı, bit-perfect)" else "${formatRate(it)} kHz'e zorla" })
            setSelection(rates.indexOf(prefs.forcedRate).coerceAtLeast(0))
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) { prefs.forcedRate = rates[pos] }
                override fun onNothingSelected(p: AdapterView<*>?) {}
            }
        })

        // --- EU info
        root.section("3 · AB (EU) sürümü hakkında")
        root.addView(text(
            "Apple USB-C → 3.5 mm adaptörün AB sürümü (A2155) çıkışı yaklaşık 0.5 Vrms ile, ABD sürümü (A2049) " +
                "yaklaşık 1 Vrms ile sınırlıdır. Bu fark adaptörün içindeki firmware/analog katmandadır; hiçbir Android " +
                "uygulaması (UAPP dahil) bu analog tavanı yükseltemez.\n\n" +
                "Bu uygulamanın kaldırdıkları: (1) Android'in DAC donanım sesini düşük bırakması, (2) Android'in AB " +
                "güvenli ses/CSD sınırlamaları (exclusive mod), (3) 48 kHz zorunlu yeniden örnekleme ve 16-bit kesme.\n" +
                "Ek olarak: \"Ön kazanç\" sessiz kayıtları yumuşak sınırlayıcıyla yükseltir (bit-perfect değildir), " +
                "\"Maksimum ötesi testi\" cihazın bildirdiğinin üstünde bir değer kabul edip etmediğini dener.", 13f,
        ))

        root.section("4 · Cihaz bilgisi")
        deviceDump = text("", 11f, mono = true)
        root.addView(deviceDump)

        return ScrollView(this).apply {
            isFillViewport = true
            addView(root)
        }
    }

    private fun updateGainLabel(db: Float) {
        gainLabel.text = if (db <= 0f) "Ön kazanç: kapalı (bit-perfect)" else "Ön kazanç: +%.1f dB (yumuşak sınırlayıcı açık)".format(db)
    }

    private fun refresh() {
        if (!::status.isInitialized) return
        val s = DacManager.session
        if (s == null || !s.isOpen) {
            status.text = "DAC bağlı değil"
            deviceDump.text = ""
        } else {
            val i = s.info
            status.text = buildString {
                append(s.displayName)
                append("  (%04x:%04x, UAC%d, %s)".format(i.vendorId, i.productId, i.uacVersion, if (i.highSpeed) "High-Speed" else "Full-Speed"))
                if (i.isAppleDongle) append("\nApple USB-C → 3.5 mm adaptör algılandı")
            }
            val vol = i.volume.firstOrNull { it.rangeValid }
            if (vol != null) {
                customMinDb = db256ToDb(vol.min)
                customMaxDb = db256ToDb(vol.max)
            }
            val span = (customMaxDb - customMinDb).takeIf { it > 0f } ?: 1f
            customSeek.progress = (((prefs.customDb.coerceIn(customMinDb, customMaxDb)) - customMinDb) / span * 600).roundToInt()
            deviceDump.text = buildString {
                append("bcdDevice %04x · feature unit: %s\n".format(i.bcdDevice, i.featureUnits.joinToString()))
                i.alts.forEach { append(if (it.playback) "▶ " else "● ").append(it.summary).append('\n') }
                i.volume.forEach {
                    if (it.rangeValid) append("%s: %.2f dB [%.2f … %.2f]\n".format(it.label, db256ToDb(it.cur), db256ToDb(it.min), db256ToDb(it.max)))
                }
            }
        }
        refreshPlayer()
    }

    private fun refreshPlayer() {
        if (!::nowPlaying.isInitialized) return
        val st = PlayerService.state
        val running = PlayerService.isRunning && st.playing
        nowPlaying.text = when {
            running -> "♪ ${st.title}  (${st.index + 1}/${st.count})  ${fmt(st.positionMs)} / ${fmt(st.durationMs)}"
            st.error != null -> "Durdu: ${st.error}"
            else -> "Çalmıyor"
        }
        pathInfo.text = st.config?.summary ?: ""
        stats.text = if (running) DacManager.session?.stats()?.summary ?: "" else ""
        playPause.text = if (st.paused) "▶" else "⏸"
        volLabel.text = if (running) "Donanım sesi: %.1f dB  (ses tuşları da çalışır)".format(st.volumeDb) else "Donanım sesi: çalarken ayarlanır"
        if (!userDraggingVolume) {
            volSeek.max = st.volumeSteps
            volSeek.progress = st.volumeStep
        }
        volSeek.isEnabled = running
    }

    private fun fmt(ms: Long): String {
        val s = ms / 1000
        return "%d:%02d".format(s / 60, s % 60)
    }
}
