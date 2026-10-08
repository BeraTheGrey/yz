package io.github.berathegrey.yzdac.player

import android.content.Context
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.Build
import android.util.Log
import io.github.berathegrey.yzdac.usb.DacSession
import io.github.berathegrey.yzdac.usb.PcmEncoding
import io.github.berathegrey.yzdac.usb.StreamConfig

/**
 * Decodes one track with the platform codecs (FLAC, WAV, ALAC, MP3, AAC,
 * Opus, Vorbis…) and pushes PCM straight to the native USB stream. The
 * decoder asks for float output so 24-bit sources are not truncated to 16 bits
 * by Android; float32 represents 24-bit samples exactly.
 */
class TrackDecoder(
    private val context: Context,
    private val uri: Uri,
    private val session: DacSession,
    private val control: Control,
    private val forcedRate: Int,
    private val onConfig: (StreamConfig) -> Unit,
    private val onProgress: (positionMs: Long, durationMs: Long) -> Unit,
) {
    class Control {
        @Volatile var stop = false
        @Volatile var paused = false
        @Volatile var seekUs = -1L
        @Volatile private var skipDelta = 0

        fun skip(delta: Int) {
            skipDelta = delta
        }

        fun takeSkip(): Int = skipDelta
        fun clearSkip() {
            skipDelta = 0
        }

        fun reset() {
            stop = false
            paused = false
            seekUs = -1
            skipDelta = 0
        }
    }

    sealed class Result {
        data object Finished : Result()
        data object Stopped : Result()
        data class Skipped(val delta: Int) : Result()
        data class Error(val message: String, val fatal: Boolean) : Result()
    }

    fun run(): Result {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            extractor.setDataSource(context, uri, null)
            val track = (0 until extractor.trackCount).firstOrNull {
                extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: return Result.Error("Ses izi bulunamadı", false)
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            val mime = format.getString(MediaFormat.KEY_MIME)!!
            val durationUs = if (format.containsKey(MediaFormat.KEY_DURATION)) format.getLong(MediaFormat.KEY_DURATION) else 0L
            val declaredBits = when {
                format.containsKey("bits-per-sample") -> format.getInteger("bits-per-sample")
                format.containsKey("bit-width") -> format.getInteger("bit-width")
                else -> 0
            }
            codec = createCodec(mime, format)
            return pump(extractor, codec, durationUs, declaredBits)
        } catch (e: Exception) {
            Log.w("YzDac", "decode failed: $uri", e)
            return Result.Error(e.message ?: e.javaClass.simpleName, false)
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            runCatching { extractor.release() }
        }
    }

    private fun createCodec(mime: String, format: MediaFormat): MediaCodec {
        val c = MediaCodec.createDecoderByType(mime)
        try {
            val f = MediaFormat(format).apply { setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT) }
            c.configure(f, null, null, 0)
        } catch (e: Exception) {
            // Some decoders refuse float output; fall back to their default (16-bit).
            c.reset()
            c.configure(format, null, null, 0)
        }
        c.start()
        return c
    }

    private fun pump(extractor: MediaExtractor, codec: MediaCodec, durationUs: Long, declaredBits: Int): Result {
        val info = MediaCodec.BufferInfo()
        var inputDone = false
        var rate = 0
        var channels = 0
        var encoding = PcmEncoding.PCM16
        var streamReady = false
        var lastProgress = 0L

        while (true) {
            if (control.stop) return Result.Stopped
            control.takeSkip().let { if (it != 0) return Result.Skipped(it) }
            val seek = control.seekUs
            if (seek >= 0) {
                control.seekUs = -1
                extractor.seekTo(seek, MediaExtractor.SEEK_TO_CLOSEST_SYNC)
                codec.flush()
                session.flush()
                inputDone = false
            }
            if (control.paused) {
                Thread.sleep(20)
                continue
            }

            if (!inputDone) {
                val inIdx = codec.dequeueInputBuffer(5_000)
                if (inIdx >= 0) {
                    val buf = codec.getInputBuffer(inIdx)!!
                    val n = extractor.readSampleData(buf, 0)
                    if (n < 0) {
                        codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        inputDone = true
                    } else {
                        codec.queueInputBuffer(inIdx, 0, n, extractor.sampleTime, 0)
                        extractor.advance()
                    }
                }
            }

            val outIdx = codec.dequeueOutputBuffer(info, 5_000)
            when {
                outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val of = codec.outputFormat
                    rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    encoding = encodingOf(of)
                    val cfg = session.startStream(rate, channels, sourceBits(encoding, declaredBits), forcedRate)
                    onConfig(cfg)
                    if (!cfg.ok) return Result.Error(cfg.error ?: "Akış başlatılamadı", true)
                    streamReady = true
                }
                outIdx >= 0 -> {
                    if (!streamReady) {
                        // Some decoders deliver output without announcing the format first.
                        val of = codec.getOutputFormat(outIdx)
                        rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                        encoding = encodingOf(of)
                        val cfg = session.startStream(rate, channels, sourceBits(encoding, declaredBits), forcedRate)
                        onConfig(cfg)
                        if (!cfg.ok) {
                            codec.releaseOutputBuffer(outIdx, false)
                            return Result.Error(cfg.error ?: "Akış başlatılamadı", true)
                        }
                        streamReady = true
                    }
                    if (info.size > 0) {
                        val buf = codec.getOutputBuffer(outIdx)!!
                        val written = session.write(buf, info.offset, info.size, encoding, channels, rate)
                        if (written < 0) {
                            codec.releaseOutputBuffer(outIdx, false)
                            return if (control.stop) Result.Stopped else Result.Error("USB akışı kesildi (DAC çıkarıldı mı?)", true)
                        }
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    val now = System.currentTimeMillis()
                    if (now - lastProgress > 250) {
                        lastProgress = now
                        onProgress(info.presentationTimeUs / 1000, durationUs / 1000)
                    }
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return Result.Finished
                }
            }
        }
    }

    private fun encodingOf(f: MediaFormat): Int {
        val e = if (f.containsKey(MediaFormat.KEY_PCM_ENCODING)) f.getInteger(MediaFormat.KEY_PCM_ENCODING)
        else AudioFormat.ENCODING_PCM_16BIT
        return when {
            e == AudioFormat.ENCODING_PCM_FLOAT -> PcmEncoding.FLOAT
            e == AudioFormat.ENCODING_PCM_8BIT -> PcmEncoding.PCM8
            Build.VERSION.SDK_INT >= 31 && e == AudioFormat.ENCODING_PCM_24BIT_PACKED -> PcmEncoding.PCM24_PACKED
            Build.VERSION.SDK_INT >= 31 && e == AudioFormat.ENCODING_PCM_32BIT -> PcmEncoding.PCM32
            else -> PcmEncoding.PCM16
        }
    }

    private fun sourceBits(encoding: Int, declared: Int): Int = when {
        declared in 8..32 -> declared
        encoding == PcmEncoding.PCM8 -> 8
        encoding == PcmEncoding.PCM16 -> 16
        encoding == PcmEncoding.PCM32 -> 32
        else -> 24
    }
}
