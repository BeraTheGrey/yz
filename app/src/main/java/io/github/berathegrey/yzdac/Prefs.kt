package io.github.berathegrey.yzdac

import android.content.Context
import android.content.SharedPreferences

class Prefs(context: Context) {
    private val p: SharedPreferences = context.applicationContext.getSharedPreferences("yzdac", Context.MODE_PRIVATE)

    /** Apply the hardware volume automatically every time the DAC is plugged in. */
    var autoApplyOnAttach: Boolean
        get() = p.getBoolean("autoApply", true)
        set(v) = p.edit().putBoolean("autoApply", v).apply()

    /** true: device maximum (GET_MAX); false: [customDb]. */
    var useMax: Boolean
        get() = p.getBoolean("useMax", true)
        set(v) = p.edit().putBoolean("useMax", v).apply()

    var customDb: Float
        get() = p.getFloat("customDb", 0f)
        set(v) = p.edit().putFloat("customDb", v).apply()

    var allowBeyondMax: Boolean
        get() = p.getBoolean("allowBeyondMax", false)
        set(v) = p.edit().putBoolean("allowBeyondMax", v).apply()

    var reattachMode: Int
        get() = p.getInt("reattachMode", 0)
        set(v) = p.edit().putInt("reattachMode", v).apply()

    /** Hardware volume used by the exclusive player, as dB below/at the device max. */
    var exclusiveVolumeDb: Float
        get() = p.getFloat("exclusiveDb", Float.NaN)
        set(v) = p.edit().putFloat("exclusiveDb", v).apply()

    var preGainDb: Float
        get() = p.getFloat("preGainDb", 0f)
        set(v) = p.edit().putFloat("preGainDb", v).apply()

    var forcedRate: Int
        get() = p.getInt("forcedRate", 0)
        set(v) = p.edit().putInt("forcedRate", v).apply()

    var warningAccepted: Boolean
        get() = p.getBoolean("warningAccepted", false)
        set(v) = p.edit().putBoolean("warningAccepted", v).apply()

    var lastResult: String
        get() = p.getString("lastResult", "") ?: ""
        set(v) = p.edit().putString("lastResult", v).apply()
}
