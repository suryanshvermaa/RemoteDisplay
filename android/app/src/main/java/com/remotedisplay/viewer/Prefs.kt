package com.remotedisplay.viewer

import android.content.Context
import android.util.Base64
import com.remotedisplay.viewer.core.DeviceIdentity

// What the app remembers: this phone's permanent device key and the computers it connected to.
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("remotedisplay", Context.MODE_PRIVATE)

    data class Recent(val address: String, val key: String)

    // The same key every time, so a PC that chose "Always allow this device" recognises this phone.
    fun identity(): DeviceIdentity {
        val priv = sp.getString("id_priv", null)
        val pub = sp.getString("id_pub", null)
        if (priv != null && pub != null) {
            DeviceIdentity.load(Base64.decode(priv, Base64.NO_WRAP), Base64.decode(pub, Base64.NO_WRAP))?.let { return it }
        }
        val id = DeviceIdentity.generate()
        sp.edit()
            .putString("id_priv", Base64.encodeToString(id.encodedPrivate(), Base64.NO_WRAP))
            .putString("id_pub", Base64.encodeToString(id.encodedPublic(), Base64.NO_WRAP))
            .apply()
        return id
    }

    fun recents(): List<Recent> =
        (sp.getString("recents", "") ?: "").lines().mapNotNull { line ->
            val i = line.indexOf('|')
            if (i <= 0) null else Recent(line.substring(0, i), line.substring(i + 1))
        }

    fun addRecent(r: Recent) {
        val list = listOf(r) + recents().filter { it.address != r.address }
        sp.edit().putString("recents", list.take(8).joinToString("\n") { "${it.address}|${it.key}" }).apply()
    }

    fun removeRecent(address: String) {
        sp.edit().putString("recents", recents().filter { it.address != address }.joinToString("\n") { "${it.address}|${it.key}" }).apply()
    }
}
