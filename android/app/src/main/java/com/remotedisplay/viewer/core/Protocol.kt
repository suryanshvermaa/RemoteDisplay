package com.remotedisplay.viewer.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

// The UDP wire format, identical to service/include/protocol.h and session.h on Windows. Little-endian.
object Proto {
    const val MAGIC = 0x32445352          // 'RSD2': inner packet header
    const val WIRE_MAGIC = 0x33445352     // 'RSD3': outer (encrypted) header
    const val DEFAULT_PORT = 47800
    const val MAX_PAYLOAD = 1200

    const val HEADER_LEN = 24             // PacketHeader
    const val WIRE_HEADER_LEN = 16        // WireHeader
    const val TAG_LEN = 16
    const val KEY_LEN = 16
    const val PUB_LEN = 64
    const val NONCE_LEN = 16
    const val HS1_LEN = NONCE_LEN + PUB_LEN        // 80
    const val HS2_LEN = NONCE_LEN + PUB_LEN + 32   // 112

    // Inner packet types.
    const val T_HELLO = 1
    const val T_VIDEO = 2
    const val T_KEYREQ = 3
    const val T_INPUT = 4
    const val T_INPUT_MOVE = 5
    const val T_INPUT_ACK = 6
    const val T_CURSOR = 7
    const val T_IDENTIFY = 8

    // Outer packet types.
    const val W_HS1 = 1
    const val W_HS2 = 2
    const val W_DATA = 3

    // Flags.
    const val F_KEYFRAME = 1
    const val F_CONTROL_ALLOWED = 2
    const val F_AWAITING_APPROVAL = 4
    const val F_DENIED = 8

    // Input events (InputEvent, 8 bytes each).
    const val IN_BUTTON = 1
    const val IN_WHEEL = 2
    const val IN_KEY = 3
    const val MAX_INPUT_EVENTS = 16

    const val DEVICE_NAME_LEN = 48
    const val IDENTIFY_LEN = 64 + 64 + DEVICE_NAME_LEN

    val DISCOVER_QUERY = byteArrayOf('R'.code.toByte(), 'D'.code.toByte(), 'Q'.code.toByte(), '1'.code.toByte())
    val DISCOVER_REPLY = byteArrayOf('R'.code.toByte(), 'D'.code.toByte(), 'A'.code.toByte(), '1'.code.toByte())
}

fun le(size: Int): ByteBuffer = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
fun le(b: ByteArray, off: Int = 0, len: Int = b.size - off): ByteBuffer =
    ByteBuffer.wrap(b, off, len).order(ByteOrder.LITTLE_ENDIAN)

class PacketHeader(
    val type: Int,
    val flags: Int,
    val fragIdx: Int,
    val fragCount: Int,
    val payloadLen: Int,
    val frameId: Long,  // uint32
) {
    companion object {
        fun parse(b: ByteArray, len: Int): PacketHeader? {
            if (len < Proto.HEADER_LEN) return null
            val bb = le(b, 0, len)
            if (bb.int != Proto.MAGIC) return null
            val type = bb.get().toInt() and 0xFF
            val flags = bb.get().toInt() and 0xFF
            val fragIdx = bb.short.toInt() and 0xFFFF
            val fragCount = bb.short.toInt() and 0xFFFF
            val payloadLen = bb.short.toInt() and 0xFFFF
            val frameId = bb.int.toLong() and 0xFFFFFFFFL
            return PacketHeader(type, flags, fragIdx, fragCount, payloadLen, frameId)
        }
    }
}

// Header + body, ready to seal.
fun buildPacket(type: Int, frameId: Long = 0, body: ByteArray = ByteArray(0)): ByteArray {
    val bb = le(Proto.HEADER_LEN + body.size)
    bb.putInt(Proto.MAGIC)
    bb.put(type.toByte())
    bb.put(0)                  // flags
    bb.putShort(0)             // fragIdx
    bb.putShort(0)             // fragCount
    bb.putShort(body.size.toShort())
    bb.putInt(frameId.toInt())
    bb.putLong(0)              // captureUs
    bb.put(body)
    return bb.array()
}

// One input event, 8 bytes: type, flags, x, y, v.
class InputEvent(val type: Int, val flags: Int, val x: Int, val y: Int, val v: Int = 0) {
    fun writeTo(bb: ByteBuffer) {
        bb.put(type.toByte())
        bb.put(flags.toByte())
        bb.putShort(x.toShort())
        bb.putShort(y.toShort())
        bb.putShort(v.toShort())
    }

    companion object {
        // Positions are normalised to 0..65535 across the shared screen.
        fun button(index: Int, down: Boolean, x: Int, y: Int) =
            InputEvent(Proto.IN_BUTTON, index or (if (down) 0x80 else 0), x, y)

        fun wheel(delta: Int, x: Int, y: Int, horizontal: Boolean = false) =
            InputEvent(Proto.IN_WHEEL, if (horizontal) 1 else 0, x, y, delta)

        fun key(scan: Int, extended: Boolean, up: Boolean) =
            InputEvent(Proto.IN_KEY, (if (up) 1 else 0) or (if (extended) 2 else 0), scan, 0)
    }
}

// Pairing key <-> text: RFC 4648 base32 grouped as XXXXX-XXXXX-... (same as rd::FormatKey / ParseKey).
object PairingKey {
    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

    fun format(k: ByteArray): String {
        val s = StringBuilder()
        var buf = 0
        var bits = 0
        for (b in k) {
            buf = (buf shl 8) or (b.toInt() and 0xFF)
            bits += 8
            while (bits >= 5) {
                s.append(ALPHABET[(buf shr (bits - 5)) and 31])
                bits -= 5
            }
            buf = buf and ((1 shl bits) - 1)
        }
        if (bits > 0) s.append(ALPHABET[(buf shl (5 - bits)) and 31])
        return s.chunked(5).joinToString("-")
    }

    fun parse(text: String): ByteArray? {
        val out = ByteArray(Proto.KEY_LEN)
        var n = 0
        var buf = 0
        var bits = 0
        for (raw in text) {
            if (raw == '-' || raw == ' ') continue
            val c = raw.uppercaseChar()
            val v = when (c) {
                in 'A'..'Z' -> c - 'A'
                in '2'..'7' -> c - '2' + 26
                else -> return null
            }
            buf = (buf shl 5) or v
            bits += 5
            if (bits >= 8) {
                if (n < Proto.KEY_LEN) out[n++] = (buf shr (bits - 8)).toByte()
                bits -= 8
                buf = buf and ((1 shl bits) - 1)
            }
        }
        return if (n == Proto.KEY_LEN) out else null
    }
}
