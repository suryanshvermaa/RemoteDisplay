package com.remotedisplay.viewer

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.google.android.material.button.MaterialButton
import com.remotedisplay.viewer.core.Connection
import com.remotedisplay.viewer.core.InputEvent
import com.remotedisplay.viewer.core.Keys
import com.remotedisplay.viewer.core.PairingKey
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

// Full-screen view of the remote PC. Touch works as the mouse:
//   tap = click, long press = right click, drag = hold the left button and move, two fingers = scroll.
class ViewerActivity : AppCompatActivity(), Connection.Listener {
    private lateinit var root: FrameLayout
    private lateinit var video: SurfaceView
    private lateinit var touch: TouchLayer
    private lateinit var status: TextView
    private val ui = Handler(Looper.getMainLooper())

    private var connection: Connection? = null
    @Volatile private var decoder: VideoDecoder? = null
    private var videoW = 0
    private var videoH = 0
    private var hostLabel = ""
    private var lastState: Connection.State? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_viewer)
        root = findViewById(R.id.root)
        video = findViewById(R.id.video)
        status = findViewById(R.id.status)
        touch = TouchLayer(this)
        root.addView(touch, 1, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        enterFullScreen()

        val address = intent.getStringExtra(EXTRA_ADDRESS) ?: return finish()
        val key = PairingKey.parse(intent.getStringExtra(EXTRA_KEY) ?: "") ?: return finish()
        val (host, port) = splitAddress(address)
        hostLabel = intent.getStringExtra(EXTRA_LABEL) ?: host

        video.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                decoder = VideoDecoder(h.surface) { w, hh -> ui.post { setVideoSize(w, hh) } }
                connection?.requestKeyframe()  // a fresh surface needs a fresh picture
            }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) {
                decoder?.release()
                decoder = null
            }
        })
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> layoutVideo() }

        findViewById<MaterialButton>(R.id.btn_keyboard).setOnClickListener { touch.toggleKeyboard() }
        findViewById<MaterialButton>(R.id.btn_esc).setOnClickListener { tap(Keys.Stroke(Keys.ESC)) }
        findViewById<MaterialButton>(R.id.btn_tab).setOnClickListener { tap(Keys.Stroke(Keys.TAB)) }
        findViewById<MaterialButton>(R.id.btn_win).setOnClickListener { tap(Keys.Stroke(Keys.WIN, extended = true)) }
        findViewById<MaterialButton>(R.id.btn_close).setOnClickListener { finish() }

        setStatus("Connecting to $hostLabel…", true)
        val prefs = Prefs(this)
        connection = Connection(host, port, key, prefs.identity(), deviceName(), this).also { it.start() }
    }

    override fun onDestroy() {
        connection?.stop()
        connection = null
        decoder?.release()
        super.onDestroy()
    }

    private fun deviceName(): String {
        val model = Build.MODEL ?: "Android"
        val maker = Build.MANUFACTURER ?: ""
        return if (model.startsWith(maker, ignoreCase = true)) model else "$maker $model".trim()
    }

    private fun enterFullScreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    // ---- connection callbacks (network thread) ---------------------------------------------------
    override fun onState(state: Connection.State, controlAllowed: Boolean) {
        ui.post {
            lastState = state
            when (state) {
                Connection.State.CONNECTING -> setStatus("Connecting to $hostLabel…", true)
                Connection.State.RECONNECTING -> setStatus("Connection lost. Reconnecting…", true)
                Connection.State.NOT_FOUND -> setStatus("No answer from $hostLabel. Check the address, that RemoteDisplay is sharing there, and the firewall.", true)
                Connection.State.WRONG_KEY -> setStatus("The pairing key is incorrect for this PC.", true)
                Connection.State.WAITING_APPROVAL -> setStatus("Waiting for approval on the other PC…", true)
                Connection.State.REFUSED -> setStatus("The other PC refused the connection.", true)
                Connection.State.CONNECTED -> setStatus(if (controlAllowed) "Connected" else "Connected · view only", false)
            }
        }
    }

    override fun onFrame(data: ByteArray, length: Int, keyframe: Boolean): Boolean {
        val d = decoder ?: return false
        return d.decode(data, length, keyframe)
    }

    override fun onStats(fps: Double, mbps: Double) {}

    // ---- layout -----------------------------------------------------------------------------------
    private fun setStatus(text: String, sticky: Boolean) {
        status.text = text
        status.visibility = View.VISIBLE
        status.animate().cancel()
        status.alpha = 1f
        if (!sticky) status.animate().alpha(0f).setStartDelay(2500).setDuration(400).withEndAction { status.visibility = View.GONE }
    }

    private fun setVideoSize(w: Int, h: Int) {
        videoW = w
        videoH = h
        layoutVideo()
    }

    // Letterbox: the picture keeps its shape inside the screen.
    private fun layoutVideo() {
        val rw = root.width
        val rh = root.height
        if (rw == 0 || rh == 0) return
        val (vw, vh) = if (videoW > 0 && videoH > 0) videoW to videoH else 16 to 9
        val k = minOf(rw.toFloat() / vw, rh.toFloat() / vh)
        val w = (vw * k).roundToInt()
        val h = (vh * k).roundToInt()
        val lp = video.layoutParams as FrameLayout.LayoutParams
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.CENTER
            video.layoutParams = lp
        }
    }

    // Screen position -> 0..65535 across the remote screen.
    private fun norm(x: Float, y: Float): Pair<Int, Int> {
        val l = video.left.toFloat()
        val t = video.top.toFloat()
        val w = video.width.coerceAtLeast(1)
        val h = video.height.coerceAtLeast(1)
        val nx = ((x - l) / w).coerceIn(0f, 1f)
        val ny = ((y - t) / h).coerceIn(0f, 1f)
        return (nx * 65535).roundToInt() to (ny * 65535).roundToInt()
    }

    // ---- input ------------------------------------------------------------------------------------
    private fun send(e: InputEvent) {
        connection?.send(e)
    }

    private fun click(button: Int, x: Float, y: Float) {
        val (nx, ny) = norm(x, y)
        connection?.move(nx, ny)
        send(InputEvent.button(button, true, nx, ny))
        send(InputEvent.button(button, false, nx, ny))
    }

    private fun tap(s: Keys.Stroke) = Keys.events(s).forEach { send(it) }

    private fun typeChar(c: Char) {
        Keys.forChar(c)?.let { tap(it) }
    }

    @SuppressLint("ViewConstructor")
    private inner class TouchLayer(context: Context) : View(context) {
        private val slop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
        private val scrollStep = 28 * resources.displayMetrics.density
        private var x0 = 0f
        private var y0 = 0f
        private var lastX = 0f
        private var lastY = 0f
        private var dragging = false
        private var longPressed = false
        private var scrolling = false
        private var scrollAcc = 0f
        private var scrollY = 0f
        private var keyboardShown = false
        private var composing = ""

        private val longPress = Runnable {
            if (!dragging && !scrolling) {
                longPressed = true
                performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                click(1, x0, y0)  // right click
            }
        }

        init {
            isFocusable = true
            isFocusableInTouchMode = true
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    x0 = e.x; y0 = e.y; lastX = e.x; lastY = e.y
                    dragging = false; longPressed = false; scrolling = false
                    handler?.postDelayed(longPress, 450)
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    handler?.removeCallbacks(longPress)
                    if (!dragging) {
                        scrolling = true
                        scrollAcc = 0f
                        scrollY = avgY(e)
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (scrolling && e.pointerCount >= 2) {
                        val y = avgY(e)
                        scrollAcc += y - scrollY
                        scrollY = y
                        val (nx, ny) = norm(e.getX(0), e.getY(0))
                        while (abs(scrollAcc) >= scrollStep) {  // fingers down = scroll up, like a touch screen
                            val dir = if (scrollAcc > 0) 1 else -1
                            send(InputEvent.wheel(120 * dir, nx, ny))
                            scrollAcc -= dir * scrollStep
                        }
                    } else if (!scrolling && !longPressed) {
                        if (!dragging && hypot(e.x - x0, e.y - y0) > slop) {
                            dragging = true
                            handler?.removeCallbacks(longPress)
                            val (nx, ny) = norm(x0, y0)
                            connection?.move(nx, ny)
                            send(InputEvent.button(0, true, nx, ny))
                        }
                        if (dragging) {
                            val (nx, ny) = norm(e.x, e.y)
                            connection?.move(nx, ny)
                        }
                    }
                    lastX = e.x; lastY = e.y
                }
                MotionEvent.ACTION_UP -> {
                    handler?.removeCallbacks(longPress)
                    when {
                        dragging -> {
                            val (nx, ny) = norm(lastX, lastY)
                            send(InputEvent.button(0, false, nx, ny))
                        }
                        !longPressed && !scrolling -> click(0, e.x, e.y)
                    }
                    dragging = false
                }
                MotionEvent.ACTION_CANCEL -> {
                    handler?.removeCallbacks(longPress)
                    if (dragging) {
                        val (nx, ny) = norm(lastX, lastY)
                        send(InputEvent.button(0, false, nx, ny))
                    }
                    dragging = false
                }
            }
            return true
        }

        private fun avgY(e: MotionEvent): Float {
            var s = 0f
            for (i in 0 until e.pointerCount) s += e.getY(i)
            return s / e.pointerCount
        }

        fun toggleKeyboard() {
            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            if (keyboardShown) {
                imm.hideSoftInputFromWindow(windowToken, 0)
                keyboardShown = false
            } else {
                requestFocus()
                imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
                keyboardShown = true
            }
        }

        override fun onCheckIsTextEditor() = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            // Visible-password text: keyboards send plain characters, without suggestions or autocorrect.
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_NONE
            return object : BaseInputConnection(this, false) {
                override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                    replaceComposing("")
                    text.forEach { typeChar(it) }
                    return true
                }

                // Some keyboards compose words first: send the difference as it changes.
                override fun setComposingText(text: CharSequence, newCursorPosition: Int): Boolean {
                    replaceComposing(text.toString())
                    return true
                }

                override fun finishComposingText(): Boolean {
                    composing = ""
                    return true
                }

                override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                    repeat(maxOf(beforeLength, 1)) { tap(Keys.Stroke(Keys.BACKSPACE)) }
                    return true
                }

                override fun sendKeyEvent(event: KeyEvent): Boolean {
                    if (event.action == KeyEvent.ACTION_DOWN) handleKey(event)
                    return true
                }
            }
        }

        private fun replaceComposing(now: String) {
            var common = 0
            while (common < composing.length && common < now.length && composing[common] == now[common]) common++
            repeat(composing.length - common) { tap(Keys.Stroke(Keys.BACKSPACE)) }
            now.substring(common).forEach { typeChar(it) }
            composing = now
        }

        override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
            if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, event)
            handleKey(event)
            return true
        }

        private fun handleKey(event: KeyEvent) {
            val special = when (event.keyCode) {
                KeyEvent.KEYCODE_DEL -> Keys.BACKSPACE
                KeyEvent.KEYCODE_FORWARD_DEL -> Keys.DELETE
                KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> Keys.ENTER
                KeyEvent.KEYCODE_TAB -> Keys.TAB
                KeyEvent.KEYCODE_ESCAPE -> Keys.ESC
                KeyEvent.KEYCODE_DPAD_UP -> Keys.UP
                KeyEvent.KEYCODE_DPAD_DOWN -> Keys.DOWN
                KeyEvent.KEYCODE_DPAD_LEFT -> Keys.LEFT
                KeyEvent.KEYCODE_DPAD_RIGHT -> Keys.RIGHT
                KeyEvent.KEYCODE_MOVE_HOME -> Keys.HOME
                KeyEvent.KEYCODE_MOVE_END -> Keys.END
                KeyEvent.KEYCODE_PAGE_UP -> Keys.PAGE_UP
                KeyEvent.KEYCODE_PAGE_DOWN -> Keys.PAGE_DOWN
                else -> null
            }
            if (special != null) {
                tap(Keys.Stroke(special))
                return
            }
            val c = event.unicodeChar
            if (c != 0) typeChar(c.toChar())
        }
    }

    companion object {
        const val EXTRA_ADDRESS = "address"
        const val EXTRA_KEY = "key"
        const val EXTRA_LABEL = "label"

        // "host" or "host:port".
        fun splitAddress(a: String): Pair<String, Int> {
            val t = a.trim()
            val i = t.lastIndexOf(':')
            if (i > 0 && t.indexOf(':') == i) {
                val p = t.substring(i + 1).toIntOrNull()
                if (p != null && p in 1..65535) return t.substring(0, i) to p
            }
            return t to com.remotedisplay.viewer.core.Proto.DEFAULT_PORT
        }
    }
}
