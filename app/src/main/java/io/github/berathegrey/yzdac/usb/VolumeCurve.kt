package io.github.berathegrey.yzdac.usb

/** UAC volume values are signed 1/256 dB. */
fun db256ToDb(v: Int): Float = v / 256f
fun dbToDb256(db: Float): Int = Math.round(db * 256f)

/**
 * Maps volume-key / slider steps to hardware dB values, 1 dB per step below
 * the device maximum (like a hi-fi attenuator). Step 0 is the device minimum
 * (quietest). Pure Kotlin so it can be unit-tested on the JVM.
 */
class VolumeCurve(val minDb: Float, val maxDb: Float, val steps: Int = DEFAULT_STEPS) {
    init {
        require(steps > 0)
    }

    fun dbForStep(step: Int): Float {
        val s = step.coerceIn(0, steps)
        if (s == 0) return minDb
        return (maxDb - (steps - s)).coerceIn(minDb, maxDb)
    }

    fun stepForDb(db: Float): Int {
        if (db <= minDb) return 0
        val s = steps - Math.round(maxDb - db)
        return s.coerceIn(1, steps)
    }

    companion object {
        const val DEFAULT_STEPS = 60
    }
}
