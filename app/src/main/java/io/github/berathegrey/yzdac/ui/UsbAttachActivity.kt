package io.github.berathegrey.yzdac.ui

import android.app.Activity
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import io.github.berathegrey.yzdac.Prefs
import io.github.berathegrey.yzdac.player.PlayerService
import io.github.berathegrey.yzdac.usb.DacManager

/**
 * Invisible activity started by Android when a USB audio device is plugged in
 * (USB_DEVICE_ATTACHED). If the user ticked "always open with YZ DAC", the USB
 * permission is granted implicitly and the hardware volume fix is applied
 * without any UI, like usbDacVolumeAndroid's auto-apply + auto-exit.
 */
class UsbAttachActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val device = usbDevice(intent)
        val prefs = Prefs(this)
        if (device == null || !DacManager.isAudioDevice(device)) {
            finish()
            return
        }
        if (!prefs.autoApplyOnAttach || !prefs.warningAccepted) {
            // First run (or auto-apply disabled): show the full UI instead.
            startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            finish()
            return
        }
        if (PlayerService.isRunning) {
            finish()
            return
        }
        val app = applicationContext
        DacManager.requestPermission(this, device) { granted ->
            if (!granted) {
                Toast.makeText(app, "YZ DAC: USB izni verilmedi", Toast.LENGTH_SHORT).show()
                return@requestPermission
            }
            DacManager.worker.execute {
                val msg = DacManager.open(app, device).fold(
                    onSuccess = { s ->
                        val r = DacManager.applyConfiguredVolume(app, s)
                        if (r.ok) "YZ DAC: donanım sesi ayarlandı\n" + r.summary else "YZ DAC: " + r.summary
                    },
                    onFailure = { "YZ DAC: " + it.message },
                )
                runOnUiThreadSafe(app, msg)
            }
        }
        finish()
    }

    private fun runOnUiThreadSafe(app: android.content.Context, msg: String) {
        android.os.Handler(android.os.Looper.getMainLooper()).post { Toast.makeText(app, msg, Toast.LENGTH_LONG).show() }
    }

    companion object {
        fun usbDevice(intent: Intent?): UsbDevice? {
            if (intent == null) return null
            return if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
    }
}
