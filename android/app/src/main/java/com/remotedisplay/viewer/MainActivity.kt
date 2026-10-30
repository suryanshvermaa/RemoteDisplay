package com.remotedisplay.viewer

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.remotedisplay.viewer.core.Discovery
import com.remotedisplay.viewer.core.PairingKey

// The connect screen: address and pairing key, PCs found on this network, and recent computers.
class MainActivity : AppCompatActivity() {
    private lateinit var prefs: Prefs
    private lateinit var addressLayout: TextInputLayout
    private lateinit var keyLayout: TextInputLayout
    private lateinit var address: TextInputEditText
    private lateinit var key: TextInputEditText
    private lateinit var foundList: LinearLayout
    private lateinit var foundEmpty: TextView
    private lateinit var searching: LinearProgressIndicator
    private lateinit var recentList: LinearLayout
    private lateinit var recentCard: View
    private var finding = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        prefs = Prefs(this)
        addressLayout = findViewById(R.id.address_layout)
        keyLayout = findViewById(R.id.key_layout)
        address = findViewById(R.id.address)
        key = findViewById(R.id.key)
        foundList = findViewById(R.id.found_list)
        foundEmpty = findViewById(R.id.found_empty)
        searching = findViewById(R.id.searching)
        recentList = findViewById(R.id.recent_list)
        recentCard = findViewById(R.id.recent_card)

        findViewById<MaterialButton>(R.id.connect).setOnClickListener { connect() }
        findViewById<MaterialButton>(R.id.find).setOnClickListener { find() }
        find()
    }

    override fun onResume() {
        super.onResume()
        showRecents()
    }

    private fun connect() {
        addressLayout.error = null
        keyLayout.error = null
        val a = address.text?.toString()?.trim().orEmpty()
        val k = key.text?.toString()?.trim().orEmpty()
        var ok = true
        if (a.isEmpty()) {
            addressLayout.error = "Enter the computer's address"
            ok = false
        }
        if (PairingKey.parse(k) == null) {
            keyLayout.error = "26 letters and digits, as shown on the other PC"
            ok = false
        }
        if (!ok) return
        val formatted = PairingKey.format(PairingKey.parse(k)!!)
        prefs.addRecent(Prefs.Recent(a, formatted))
        startActivity(
            Intent(this, ViewerActivity::class.java)
                .putExtra(ViewerActivity.EXTRA_ADDRESS, a)
                .putExtra(ViewerActivity.EXTRA_KEY, formatted)
        )
    }

    private fun find() {
        if (finding) return
        finding = true
        searching.visibility = View.VISIBLE
        foundEmpty.visibility = View.GONE
        Thread {
            val found = try {
                Discovery.find()
            } catch (e: Exception) {
                emptyList()
            }
            runOnUiThread {
                finding = false
                searching.visibility = View.GONE
                showFound(found)
            }
        }.start()
    }

    private fun showFound(found: List<Discovery.Found>) {
        foundList.removeAllViews()
        foundEmpty.visibility = if (found.isEmpty()) View.VISIBLE else View.GONE
        for (f in found) {
            foundList.addView(row(f.name, f.ip) {
                address.setText(f.ip)
                key.requestFocus()
            })
        }
    }

    private fun showRecents() {
        recentList.removeAllViews()
        val recents = prefs.recents()
        recentCard.visibility = if (recents.isEmpty()) View.GONE else View.VISIBLE
        for (r in recents) {
            recentList.addView(row(r.address, "Pairing key saved") {
                address.setText(r.address)
                key.setText(r.key)
            })
        }
    }

    private fun row(title: String, subtitle: String, onClick: () -> Unit): View {
        val v = LayoutInflater.from(this).inflate(R.layout.item_device, foundList, false)
        v.findViewById<TextView>(R.id.title).text = title
        v.findViewById<TextView>(R.id.subtitle).text = subtitle
        v.setOnClickListener { onClick() }
        return v
    }
}
