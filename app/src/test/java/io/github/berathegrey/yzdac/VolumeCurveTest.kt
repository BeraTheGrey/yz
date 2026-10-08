package io.github.berathegrey.yzdac

import io.github.berathegrey.yzdac.usb.VolumeCurve
import io.github.berathegrey.yzdac.usb.db256ToDb
import io.github.berathegrey.yzdac.usb.dbToDb256
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class VolumeCurveTest {
    @Test
    fun appleLikeRangeUsesOneDbSteps() {
        val c = VolumeCurve(-64f, 0f)
        assertEquals(0f, c.dbForStep(60), 0f)
        assertEquals(-1f, c.dbForStep(59), 0f)
        assertEquals(-59f, c.dbForStep(1), 0f)
        assertEquals(-64f, c.dbForStep(0), 0f)  // step 0 = quietest the DAC can do
        assertEquals(60, c.stepForDb(0f))
        assertEquals(40, c.stepForDb(-20f))
        assertEquals(0, c.stepForDb(-64f))
        assertEquals(1, c.stepForDb(-62f))
    }

    @Test
    fun narrowRangeIsClamped() {
        val c = VolumeCurve(-20f, -6f)
        assertEquals(-6f, c.dbForStep(60), 0f)
        assertEquals(-20f, c.dbForStep(10), 0f)
        assertEquals(46, c.stepForDb(-19.5f))
        for (s in 0..60) {
            val db = c.dbForStep(s)
            assertTrue(db in -20f..-6f)
        }
    }

    @Test
    fun outOfRangeStepsAreClamped() {
        val c = VolumeCurve(-60f, 0f)
        assertEquals(0f, c.dbForStep(1000), 0f)
        assertEquals(-60f, c.dbForStep(-5), 0f)
        assertEquals(60, c.stepForDb(12f))
    }

    @Test
    fun db256Conversion() {
        assertEquals(256, dbToDb256(1f))
        assertEquals(-0x2000, dbToDb256(-32f))
        assertEquals(-6.5f, db256ToDb(-1664), 0f)
    }
}
