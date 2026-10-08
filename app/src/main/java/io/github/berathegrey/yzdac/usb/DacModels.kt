package io.github.berathegrey.yzdac.usb

import org.json.JSONArray
import org.json.JSONObject

data class VolumeChannel(
    val unit: Int,
    val channel: Int,
    val rangeValid: Boolean,
    val min: Int,
    val max: Int,
    val res: Int,
    val cur: Int,
    val hasMute: Boolean,
) {
    val hasVolume get() = rangeValid
    val label get() = if (channel == 0) "FU$unit master" else "FU$unit kanal $channel"

    companion object {
        fun from(o: JSONObject) = VolumeChannel(
            unit = o.optInt("unit"),
            channel = o.optInt("channel"),
            rangeValid = o.optBoolean("rangeValid"),
            min = o.optInt("min"),
            max = o.optInt("max"),
            res = o.optInt("res", 1),
            cur = o.optInt("cur"),
            hasMute = o.optBoolean("hasMute"),
        )

        fun list(a: JSONArray?): List<VolumeChannel> =
            if (a == null) emptyList() else (0 until a.length()).map { from(a.getJSONObject(it)) }
    }
}

data class AltInfo(
    val iface: Int,
    val alt: Int,
    val playback: Boolean,
    val channels: Int,
    val subslot: Int,
    val bits: Int,
    val sync: String,
    val feedback: Boolean,
    val maxPacket: Int,
    val rates: List<Int>,
) {
    val summary: String
        get() {
            val r = if (rates.isEmpty()) "?" else rates.joinToString("/") { formatRate(it) }
            return "if$iface alt$alt: ${channels}ch ${bits}bit (${subslot}B) $r kHz, $sync" +
                (if (feedback) "+fb" else "") + ", ${maxPacket}B"
        }
}

data class DeviceInfo(
    val vendorId: Int,
    val productId: Int,
    val bcdDevice: Int,
    val uacVersion: Int,
    val highSpeed: Boolean,
    val featureUnits: List<Int>,
    val alts: List<AltInfo>,
    val volume: List<VolumeChannel>,
) {
    val isAppleDongle get() = vendorId == APPLE_VID && productId == APPLE_DONGLE_PID
    val playbackAlts get() = alts.filter { it.playback }

    companion object {
        const val APPLE_VID = 0x05AC
        const val APPLE_DONGLE_PID = 0x110A

        fun parse(json: String): DeviceInfo {
            val o = JSONObject(json)
            val fus = o.optJSONArray("featureUnits")
            val alts = o.optJSONArray("alts")
            return DeviceInfo(
                vendorId = o.optInt("vendorId"),
                productId = o.optInt("productId"),
                bcdDevice = o.optInt("bcdDevice"),
                uacVersion = o.optInt("uacVersion", 1),
                highSpeed = o.optBoolean("highSpeed"),
                featureUnits = if (fus == null) emptyList() else (0 until fus.length()).map { fus.getInt(it) },
                alts = if (alts == null) emptyList() else (0 until alts.length()).map {
                    val a = alts.getJSONObject(it)
                    val rates = a.optJSONArray("rates")
                    AltInfo(
                        iface = a.optInt("interface"),
                        alt = a.optInt("alt"),
                        playback = a.optBoolean("playback"),
                        channels = a.optInt("channels"),
                        subslot = a.optInt("subslot"),
                        bits = a.optInt("bits"),
                        sync = a.optString("sync"),
                        feedback = a.optBoolean("feedback"),
                        maxPacket = a.optInt("maxPacket"),
                        rates = if (rates == null) emptyList() else (0 until rates.length()).map { i -> rates.getInt(i) },
                    )
                },
                volume = VolumeChannel.list(o.optJSONArray("volume")),
            )
        }
    }
}

data class ChannelResult(
    val unit: Int,
    val channel: Int,
    val min: Int,
    val max: Int,
    val requested: Int,
    val ok: Boolean,
    val readBack: Int?,
)

data class VolumeApplyResult(val ok: Boolean, val error: String?, val channels: List<ChannelResult>) {
    val summary: String
        get() = if (channels.isEmpty()) error ?: "Sonuç yok" else channels.joinToString("\n") {
            val rb = it.readBack?.let { v -> "%.2f dB".format(db256ToDb(v)) } ?: "okunamadı"
            "FU${it.unit}/k${it.channel}: istek %.2f dB → cihaz $rb (aralık %.1f…%.1f dB)%s".format(
                db256ToDb(it.requested), db256ToDb(it.min), db256ToDb(it.max), if (it.ok) "" else " HATA",
            )
        }

    companion object {
        fun parse(json: String): VolumeApplyResult {
            val o = JSONObject(json)
            val arr = o.optJSONArray("results")
            val list = if (arr == null) emptyList() else (0 until arr.length()).map {
                val r = arr.getJSONObject(it)
                ChannelResult(
                    unit = r.optInt("unit"),
                    channel = r.optInt("channel"),
                    min = r.optInt("min"),
                    max = r.optInt("max"),
                    requested = r.optInt("requested"),
                    ok = r.optBoolean("ok"),
                    readBack = if (r.optBoolean("readBackValid")) r.optInt("readBack") else null,
                )
            }
            return VolumeApplyResult(o.optBoolean("ok"), o.optString("error").ifEmpty { null }, list)
        }
    }
}

data class ProbeChannel(val unit: Int, val channel: Int, val max: Int, val highestAccepted: Int, val aboveMax: Boolean)

data class ProbeResult(val ok: Boolean, val error: String?, val channels: List<ProbeChannel>) {
    val anyAboveMax get() = channels.any { it.aboveMax }
    val summary: String
        get() = if (channels.isEmpty()) error ?: "Sonuç yok" else channels.joinToString("\n") {
            "FU${it.unit}/k${it.channel}: rapor edilen max %.2f dB, cihazın kabul ettiği en yüksek %.2f dB%s".format(
                db256ToDb(it.max), db256ToDb(it.highestAccepted), if (it.aboveMax) "  ← max üstü kabul edildi" else "",
            )
        }

    companion object {
        fun parse(json: String): ProbeResult {
            val o = JSONObject(json)
            val arr = o.optJSONArray("results")
            val list = if (arr == null) emptyList() else (0 until arr.length()).map {
                val r = arr.getJSONObject(it)
                ProbeChannel(r.optInt("unit"), r.optInt("channel"), r.optInt("max"), r.optInt("highestAccepted"), r.optBoolean("aboveMax"))
            }
            return ProbeResult(o.optBoolean("ok"), o.optString("error").ifEmpty { null }, list)
        }
    }
}

data class StreamConfig(
    val ok: Boolean,
    val error: String?,
    val sourceRate: Int,
    val deviceRate: Int,
    val sourceBits: Int,
    val deviceBits: Int,
    val subslot: Int,
    val channels: Int,
    val sync: String,
    val feedback: Boolean,
    val resampling: Boolean,
    val dither: Boolean,
    val bitPerfect: Boolean,
) {
    val summary: String
        get() = if (!ok) "Hata: $error" else buildString {
            append("${formatRate(sourceRate)} kHz / ${sourceBits}bit → DAC ${formatRate(deviceRate)} kHz / ${deviceBits}bit")
            append(" (${subslot}B, ${channels}ch, $sync${if (feedback) "+fb" else ""})")
            if (resampling) append(" · SRC")
            if (dither) append(" · dither")
            append(if (bitPerfect) " · BIT-PERFECT" else " · işlenmiş")
        }

    companion object {
        fun parse(json: String): StreamConfig {
            val o = JSONObject(json)
            return StreamConfig(
                ok = o.optBoolean("ok"),
                error = o.optString("error").ifEmpty { null },
                sourceRate = o.optInt("sourceRate"),
                deviceRate = o.optInt("deviceRate"),
                sourceBits = o.optInt("sourceBits"),
                deviceBits = o.optInt("deviceBits"),
                subslot = o.optInt("subslot"),
                channels = o.optInt("channels"),
                sync = o.optString("sync"),
                feedback = o.optBoolean("feedback"),
                resampling = o.optBoolean("resampling"),
                dither = o.optBoolean("dither"),
                bitPerfect = o.optBoolean("bitPerfect"),
            )
        }
    }
}

data class StreamStats(
    val running: Boolean,
    val rate: Int,
    val underruns: Long,
    val errors: Long,
    val bufferedFrames: Int,
    val bufferCapacityFrames: Int,
    val feedback: Double,
    val nominal: Double,
) {
    val summary: String
        get() = if (!running) "Akış yok" else
            "Tampon %d/%d · underrun %d · hata %d · fb %.4f (nominal %.4f)".format(
                bufferedFrames, bufferCapacityFrames, underruns, errors, feedback, nominal,
            )

    companion object {
        fun parse(json: String): StreamStats {
            val o = JSONObject(json)
            return StreamStats(
                running = o.optBoolean("running"),
                rate = o.optInt("rate"),
                underruns = o.optLong("underruns"),
                errors = o.optLong("errors"),
                bufferedFrames = o.optInt("bufferedFrames"),
                bufferCapacityFrames = o.optInt("bufferCapacityFrames"),
                feedback = o.optDouble("feedbackFramesPerPacket", 0.0),
                nominal = o.optDouble("nominalFramesPerPacket", 0.0),
            )
        }
    }
}

fun formatRate(hz: Int): String =
    if (hz % 1000 == 0) (hz / 1000).toString() else "%.1f".format(hz / 1000f)
