package io.github.berathegrey.yzdac.usb

import java.nio.ByteBuffer

/**
 * JNI surface of libyzdac (app/src/main/cpp). Every call that returns a String
 * returns a JSON object; see [DacSession] for the typed wrappers.
 */
object NativeBridge {
    init {
        System.loadLibrary("yzdac")
    }

    @JvmStatic external fun nativeOpen(fd: Int, rawDescriptors: ByteArray): Long
    @JvmStatic external fun nativeLastOpenError(): String
    @JvmStatic external fun nativeClose(handle: Long)
    @JvmStatic external fun nativeDescribe(handle: Long): String
    @JvmStatic external fun nativeReadVolume(handle: Long, reattachMode: Int): String
    @JvmStatic external fun nativeApplyVolume(
        handle: Long, toMax: Boolean, targetDb256: Int, allowBeyondMax: Boolean, reattachMode: Int,
    ): String
    @JvmStatic external fun nativeProbeAboveMax(handle: Long, limitDb: Int, keepHighest: Boolean, reattachMode: Int): String
    @JvmStatic external fun nativeStartStream(handle: Long, rate: Int, channels: Int, bits: Int, forcedRate: Int): String
    @JvmStatic external fun nativeWrite(
        handle: Long, buffer: ByteBuffer, offset: Int, size: Int, encoding: Int, channels: Int, rate: Int,
    ): Int
    @JvmStatic external fun nativeFlush(handle: Long)
    @JvmStatic external fun nativeStopStream(handle: Long)
    @JvmStatic external fun nativeStats(handle: Long): String
    @JvmStatic external fun nativeSetDigitalGain(handle: Long, gain: Float, softClip: Boolean)
}

/** Must match dsp::InputEncoding in dsp/pcm.h. */
object PcmEncoding {
    const val PCM16 = 0
    const val FLOAT = 1
    const val PCM24_PACKED = 2
    const val PCM32 = 3
    const val PCM8 = 4
}

/** Must match engine::ReattachMode in engine/dac_device.h. */
object ReattachMode {
    const val ATTACH_DRIVER = 0
    const val USB_RESET = 1
}
