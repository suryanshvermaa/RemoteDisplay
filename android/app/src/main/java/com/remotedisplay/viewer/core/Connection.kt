package com.remotedisplay.viewer.core

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.util.ArrayDeque

// One viewer connection to a RemoteDisplay host: the same loop as Worker() in client/src/viewer.h.
// Runs on its own thread; the listener is called from that thread.
class Connection(
    private val host: String,
    private val port: Int,
    private val psk: ByteArray,
    private val identity: DeviceIdentity?,
    private val deviceName: String,
    private val listener: Listener,
) {
    enum class State { CONNECTING, NOT_FOUND, WRONG_KEY, WAITING_APPROVAL, REFUSED, CONNECTED, RECONNECTING }

    interface Listener {
        fun onState(state: State, controlAllowed: Boolean)
        // One complete H.264 access unit (Annex B). Return false if it could not be decoded (a keyframe is then requested).
        fun onFrame(data: ByteArray, length: Int, keyframe: Boolean): Boolean
        fun onStats(fps: Double, mbps: Double) {}
        // The host's mouse pointer: BGRA pixels, top-down.
        fun onCursor(hotX: Int, hotY: Int, w: Int, h: Int, bgra: ByteArray) {}
    }

    @Volatile private var running = false
    private var thread: Thread? = null

    // Input from the UI thread.
    private val inputLock = Any()
    private val inQueue = ArrayDeque<InputEvent>()
    @Volatile private var movePos = -1  // x | y << 16, -1 = nothing new
    @Volatile var controlAllowed = false
        private set

    fun start() {
        if (running) return
        running = true
        thread = Thread({ run() }, "rd-connection").also { it.start() }
    }

    fun stop() {
        running = false
        thread?.join(1500)
        thread = null
    }

    // ---- input (any thread) -------------------------------------------------------------------
    fun move(x: Int, y: Int) {
        movePos = (x and 0xFFFF) or ((y and 0xFFFF) shl 16)
    }

    fun send(e: InputEvent) {
        if (!controlAllowed) return
        synchronized(inputLock) { inQueue.addLast(e) }
    }

    @Volatile private var keyRequested = false
    fun requestKeyframe() {
        keyRequested = true
    }

    // ---- the loop ----------------------------------------------------------------------------
    private fun now() = System.nanoTime() / 1000  // microseconds

    private fun run() {
        val addr = try {
            InetAddress.getByName(host)
        } catch (e: Exception) {
            listener.onState(State.NOT_FOUND, false)
            running = false
            return
        }
        val sock = DatagramSocket()
        sock.receiveBufferSize = 8 shl 20
        sock.soTimeout = 2
        fun tx(b: ByteArray) {
            try {
                sock.send(DatagramPacket(b, b.size, addr, port))
            } catch (e: Exception) {
            }
        }

        var state = State.CONNECTING
        var lastReported: State? = null
        var lastReportedControl = false
        fun setState(s: State) {
            state = s
            if (s != lastReported || controlAllowed != lastReportedControl) {
                lastReported = s
                lastReportedControl = controlAllowed
                listener.onState(s, controlAllowed)
            }
        }
        setState(State.CONNECTING)

        var session: Session? = null
        var hs: ClientHandshake? = null
        var hs1Wire = ByteArray(0)
        var connected = false
        var hostSeen = false
        var gotHostData = false
        val start = now()
        var lastHs = 0L
        var lastRx = now()
        var lastHello = 0L
        var lastIdent = 0L
        var lastKeyReq = 0L

        // Frame reassembly.
        var haveCur = false
        var curId = 0L
        var curKey = false
        var curCount = 0
        var curGot = 0
        var curBytes = 0
        var curBuf = ByteArray(0)
        var curSeen = BooleanArray(0)
        var needKey = true
        var haveDecoded = false
        var lastDecoded = 0L

        // Cursor reassembly.
        var cId = -1L
        var cCount = 0
        var cGot = 0
        var cBuf = ByteArray(0)
        var cSeen = BooleanArray(0)
        var cLen = 0

        // Reliable input: events are resent until the host acks them.
        var inAcked = 0L
        val unacked = ArrayDeque<InputEvent>()
        var inputFresh = false
        var lastInputSend = 0L
        var lastMoveSend = 0L

        var statStart = now()
        var statFrames = 0
        var statBytes = 0L

        fun sendInner(type: Int, frameId: Long = 0, body: ByteArray = ByteArray(0)) {
            val s = session ?: return
            tx(s.seal(buildPacket(type, frameId, body)))
        }

        fun sendIdentify() {
            val s = session ?: return
            val body = identity?.buildIdentify(s.transcriptHash, deviceName) ?: return
            sendInner(Proto.T_IDENTIFY, 0, body)
        }

        fun requestKey() {
            needKey = true
            if (now() - lastKeyReq > 100_000) {  // at most 10 per second
                sendInner(Proto.T_KEYREQ)
                lastKeyReq = now()
            }
        }

        fun resetInput() {
            inAcked = 0
            unacked.clear()
            inputFresh = false
            controlAllowed = false
            movePos = -1
            synchronized(inputLock) { inQueue.clear() }
        }

        fun pumpInput() {
            if (!connected || !controlAllowed) return
            synchronized(inputLock) {
                while (inQueue.isNotEmpty()) {
                    unacked.addLast(inQueue.removeFirst())
                    inputFresh = true
                }
            }
            val mp = movePos
            if (mp != -1 && now() - lastMoveSend > 3_000) {
                movePos = -1
                sendInner(Proto.T_INPUT_MOVE, 0, le(4).apply { putShort((mp and 0xFFFF).toShort()); putShort((mp ushr 16).toShort()) }.array())
                lastMoveSend = now()
            }
            if (unacked.isNotEmpty() && (inputFresh || now() - lastInputSend > 30_000)) {
                val cnt = minOf(unacked.size, Proto.MAX_INPUT_EVENTS)
                val bb = le(5 + cnt * 8)
                bb.putInt(inAcked.toInt())
                bb.put(cnt.toByte())
                unacked.asSequence().take(cnt).forEach { it.writeTo(bb) }
                sendInner(Proto.T_INPUT, 0, bb.array())
                lastInputSend = now()
                inputFresh = unacked.size > cnt
            }
        }

        val buf = ByteArray(Proto.WIRE_HEADER_LEN + Proto.HEADER_LEN + Proto.MAX_PAYLOAD + Proto.TAG_LEN + 64)
        val dp = DatagramPacket(buf, buf.size)

        try {
            while (running) {
                if (connected && now() - lastRx > 3_000_000) {  // host gone or restarted: pair again
                    connected = false
                    session = null
                    hs = null
                    haveCur = false
                    needKey = true
                    hostSeen = false
                    resetInput()
                    setState(State.RECONNECTING)
                }
                if (!connected && !hostSeen && now() - start > 8_000_000 && state == State.CONNECTING) {
                    setState(State.NOT_FOUND)
                }
                if (!connected && now() - lastHs > 300_000) {
                    if (hs == null) {  // new ephemeral keys; resend the SAME Hs1 until Hs2 arrives
                        hs = ClientHandshake()
                        hs1Wire = hs.hs1Wire()
                    }
                    tx(hs1Wire)
                    lastHs = now()
                }
                if (connected && now() - lastHello > 1_000_000) {
                    sendInner(Proto.T_HELLO)
                    lastHello = now()
                }
                if (connected && !gotHostData && identity != null && now() - lastIdent > 300_000) {
                    sendIdentify()
                    lastIdent = now()
                }
                if (keyRequested) {
                    keyRequested = false
                    requestKey()
                }
                pumpInput()

                dp.length = buf.size
                try {
                    sock.receive(dp)
                } catch (e: SocketTimeoutException) {
                    continue
                }
                val n = dp.length
                if (n < Proto.WIRE_HEADER_LEN) continue
                val wh = le(buf, 0, n)
                if (wh.int != Proto.WIRE_MAGIC) continue
                val wtype = wh.get().toInt() and 0xFF

                if (wtype == Proto.W_HS2) {
                    val h = hs
                    if (!connected && h != null && n == Proto.WIRE_HEADER_LEN + Proto.HS2_LEN) {
                        hostSeen = true
                        val s = h.finish(psk, buf.copyOfRange(Proto.WIRE_HEADER_LEN, n))
                        if (s == null) {
                            setState(State.WRONG_KEY)  // the device answered but does not know this key
                            continue
                        }
                        session = s
                        connected = true
                        lastRx = now()
                        gotHostData = false
                        if (identity != null) {
                            sendIdentify()
                            lastIdent = now()
                        } else {
                            sendInner(Proto.T_HELLO)  // the first authenticated packet activates the host session
                        }
                        lastHello = now()
                        needKey = true
                        resetInput()
                        setState(State.CONNECTED)
                    }
                    continue
                }
                if (wtype != Proto.W_DATA || !connected) continue
                val plain = session?.open(buf, n) ?: continue
                lastRx = now()
                gotHostData = true
                val ph = PacketHeader.parse(plain, plain.size) ?: continue
                val body = Proto.HEADER_LEN

                when (ph.type) {
                    Proto.T_HELLO -> {  // keepalive: carries what the host user allows
                        controlAllowed = (ph.flags and Proto.F_CONTROL_ALLOWED) != 0
                        setState(
                            when {
                                ph.flags and Proto.F_DENIED != 0 -> State.REFUSED
                                ph.flags and Proto.F_AWAITING_APPROVAL != 0 -> State.WAITING_APPROVAL
                                else -> State.CONNECTED
                            }
                        )
                    }
                    Proto.T_INPUT_ACK -> {
                        while ((ph.frameId - inAcked).toInt() > 0 && unacked.isNotEmpty()) {
                            unacked.removeFirst()
                            inAcked = (inAcked + 1) and 0xFFFFFFFFL
                        }
                    }
                    Proto.T_CURSOR -> {
                        if (ph.fragCount == 0 || ph.fragIdx >= ph.fragCount || ph.payloadLen > Proto.MAX_PAYLOAD ||
                            plain.size < body + ph.payloadLen
                        ) continue
                        if (ph.frameId != cId || ph.fragCount != cCount) {
                            cId = ph.frameId
                            cCount = ph.fragCount
                            cGot = 0
                            cBuf = ByteArray(cCount * Proto.MAX_PAYLOAD)
                            cSeen = BooleanArray(cCount)
                            cLen = cBuf.size
                        }
                        if (cSeen[ph.fragIdx]) continue
                        System.arraycopy(plain, body, cBuf, ph.fragIdx * Proto.MAX_PAYLOAD, ph.payloadLen)
                        cSeen[ph.fragIdx] = true
                        if (ph.fragIdx + 1 == cCount) cLen = ph.fragIdx * Proto.MAX_PAYLOAD + ph.payloadLen
                        if (++cGot == cCount) {
                            cGot = 0
                            cSeen.fill(false)
                            if (cLen >= 8) {
                                val hb = le(cBuf, 0, 8)
                                val hx = hb.short.toInt()
                                val hy = hb.short.toInt()
                                val w = hb.short.toInt() and 0xFFFF
                                val hh = hb.short.toInt() and 0xFFFF
                                if (w in 1..256 && hh in 1..256 && cLen >= 8 + w * hh * 4)
                                    listener.onCursor(hx, hy, w, hh, cBuf.copyOfRange(8, 8 + w * hh * 4))
                            }
                        }
                    }
                    Proto.T_VIDEO -> {
                        if (ph.fragCount == 0 || ph.fragIdx >= ph.fragCount || ph.payloadLen > Proto.MAX_PAYLOAD ||
                            plain.size < body + ph.payloadLen
                        ) continue
                        if (!haveCur || ph.frameId != curId) {
                            if (haveCur && (ph.frameId - curId).toInt() < 0) continue  // stale
                            if (haveCur && curGot < curCount) requestKey()           // previous frame incomplete
                            haveCur = true
                            curId = ph.frameId
                            curCount = ph.fragCount
                            curGot = 0
                            curBytes = 0
                            curKey = (ph.flags and Proto.F_KEYFRAME) != 0
                            if (curBuf.size < curCount * Proto.MAX_PAYLOAD) curBuf = ByteArray(curCount * Proto.MAX_PAYLOAD)
                            curSeen = BooleanArray(curCount)
                        }
                        if (ph.fragCount != curCount || curSeen[ph.fragIdx]) continue
                        System.arraycopy(plain, body, curBuf, ph.fragIdx * Proto.MAX_PAYLOAD, ph.payloadLen)
                        curSeen[ph.fragIdx] = true
                        curBytes += ph.payloadLen
                        if (++curGot < curCount) continue

                        // Frame complete. Fragments are full-size except the last, so the frame is contiguous.
                        haveCur = false
                        if (haveDecoded && curId != ((lastDecoded + 1) and 0xFFFFFFFFL) && !curKey) requestKey()
                        if (needKey && !curKey) continue
                        if (curKey) needKey = false
                        if (!listener.onFrame(curBuf, curBytes, curKey)) requestKey()
                        lastDecoded = curId
                        haveDecoded = true
                        statFrames++
                        statBytes += curBytes
                        if (now() - statStart >= 1_000_000) {
                            val sec = (now() - statStart) / 1e6
                            listener.onStats(statFrames / sec, statBytes * 8 / sec / 1e6)
                            statStart = now()
                            statFrames = 0
                            statBytes = 0
                        }
                    }
                }
            }
        } finally {
            sock.close()
        }
    }
}
