package io.github.berathegrey.yzdac.usb

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import java.io.Closeable
import java.nio.ByteBuffer
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

/**
 * An opened USB audio device. Wraps the UsbDeviceConnection's file descriptor
 * with the native libusb driver. Thread-safe; [close] waits for running calls.
 */
class DacSession private constructor(
    val device: UsbDevice,
    private val connection: UsbDeviceConnection,
    private var handle: Long,
) : Closeable {

    private val lock = ReentrantReadWriteLock()

    @Volatile
    var info: DeviceInfo = DeviceInfo.parse(NativeBridge.nativeDescribe(handle))
        private set

    val isOpen get() = handle != 0L
    val displayName: String
        get() = listOfNotNull(device.manufacturerName, device.productName).joinToString(" ").ifBlank { device.deviceName }

    private inline fun <T> withHandle(default: T, block: (Long) -> T): T = lock.read {
        val h = handle
        if (h == 0L) default else block(h)
    }

    fun refreshInfo(): DeviceInfo = withHandle(info) { h ->
        DeviceInfo.parse(NativeBridge.nativeDescribe(h)).also { info = it }
    }

    fun readVolume(reattachMode: Int): List<VolumeChannel> = withHandle(emptyList()) { h ->
        val o = org.json.JSONObject(NativeBridge.nativeReadVolume(h, reattachMode))
        VolumeChannel.list(o.optJSONArray("channels")).also { refreshInfo() }
    }

    fun applyMax(reattachMode: Int): VolumeApplyResult = applyRaw(true, 0, false, reattachMode)

    fun applyDb(db: Float, allowBeyondMax: Boolean, reattachMode: Int): VolumeApplyResult =
        applyRaw(false, dbToDb256(db), allowBeyondMax, reattachMode)

    private fun applyRaw(toMax: Boolean, target: Int, beyond: Boolean, mode: Int): VolumeApplyResult =
        withHandle(VolumeApplyResult(false, "Cihaz kapalı", emptyList())) { h ->
            VolumeApplyResult.parse(NativeBridge.nativeApplyVolume(h, toMax, target, beyond, mode)).also { refreshInfo() }
        }

    fun probeAboveMax(limitDb: Int, keepHighest: Boolean, reattachMode: Int): ProbeResult =
        withHandle(ProbeResult(false, "Cihaz kapalı", emptyList())) { h ->
            ProbeResult.parse(NativeBridge.nativeProbeAboveMax(h, limitDb, keepHighest, reattachMode)).also { refreshInfo() }
        }

    fun startStream(rate: Int, channels: Int, bits: Int, forcedRate: Int): StreamConfig =
        withHandle(StreamConfig.parse("""{"ok":false,"error":"Cihaz kapalı"}""")) { h ->
            StreamConfig.parse(NativeBridge.nativeStartStream(h, rate, channels, bits, forcedRate))
        }

    /** Blocks while the DAC buffer is full. Returns frames consumed or -1 if the stream is gone. */
    fun write(buffer: ByteBuffer, offset: Int, size: Int, encoding: Int, channels: Int, rate: Int): Int =
        withHandle(-1) { h -> NativeBridge.nativeWrite(h, buffer, offset, size, encoding, channels, rate) }

    fun flush() = withHandle(Unit) { h -> NativeBridge.nativeFlush(h) }

    fun stopStream() = withHandle(Unit) { h -> NativeBridge.nativeStopStream(h) }

    fun stats(): StreamStats = withHandle(StreamStats.parse("{}")) { h -> StreamStats.parse(NativeBridge.nativeStats(h)) }

    fun setDigitalGain(gain: Float, softClip: Boolean) = withHandle(Unit) { h ->
        NativeBridge.nativeSetDigitalGain(h, gain, softClip)
    }

    override fun close() {
        // Wake a writer blocked in nativeWrite before taking the exclusive lock.
        val h = handle
        if (h != 0L) NativeBridge.nativeStopStream(h)
        lock.write {
            if (handle != 0L) {
                NativeBridge.nativeClose(handle)
                handle = 0L
            }
        }
        connection.close()
    }

    companion object {
        fun open(manager: UsbManager, device: UsbDevice): Result<DacSession> {
            val connection = manager.openDevice(device)
                ?: return Result.failure(IllegalStateException("USB bağlantısı açılamadı (izin yok mu?)"))
            val raw = connection.rawDescriptors
            if (raw == null) {
                connection.close()
                return Result.failure(IllegalStateException("USB descriptor'ları okunamadı"))
            }
            val handle = NativeBridge.nativeOpen(connection.fileDescriptor, raw)
            if (handle == 0L) {
                val err = NativeBridge.nativeLastOpenError()
                connection.close()
                return Result.failure(IllegalStateException(err.ifBlank { "Native sürücü açılamadı" }))
            }
            return Result.success(DacSession(device, connection, handle))
        }
    }
}
