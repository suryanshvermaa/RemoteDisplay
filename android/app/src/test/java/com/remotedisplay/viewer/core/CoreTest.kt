package com.remotedisplay.viewer.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.Signature

class CoreTest {
    @Test
    fun pairingKeyRoundTrip() {
        // A key printed by the Windows rd-host (rd::FormatKey).
        val text = "3YY3K-MJAEW-TC4GJ-MMI4K-2TB4E-U"
        val k = PairingKey.parse(text)
        assertNotNull(k)
        assertEquals(16, k!!.size)
        assertEquals(text, PairingKey.format(k))
        assertArrayEquals(k, PairingKey.parse(text.lowercase().replace("-", " ")))
        assertNull(PairingKey.parse("too-short"))
        assertNull(PairingKey.parse("3YY3K-MJAEW-TC4GJ-MMI4K-2TB4E-1"))  // '1' is not base32
    }

    @Test
    fun sessionRoundTripAndReplay() {
        val k1 = Crypto.randomBytes(16)
        val k2 = Crypto.randomBytes(16)
        val th = Crypto.randomBytes(32)
        val a = Session(k1, k2, th)  // a sends with k1
        val b = Session(k2, k1, th)  // b receives with k1
        val msg = "hello".toByteArray()
        val wire = a.seal(msg)
        assertEquals(16 + msg.size + 16, wire.size)
        assertArrayEquals(msg, b.open(wire, wire.size))
        assertNull("a replay is rejected", b.open(wire, wire.size))
        val forged = wire.copyOf().also { it[20] = (it[20].toInt() xor 1).toByte() }
        assertNull("a modified packet is rejected", Session(k2, k1, th).open(forged, forged.size))
        val later = a.seal(msg)
        assertArrayEquals(msg, b.open(later, later.size))
    }

    @Test
    fun handshakeAgreesOnKeys() {
        // Host side computed the way rd::HostRespond does, to check both ends derive the same session.
        val psk = Crypto.randomBytes(16)
        val client = ClientHandshake()
        val host = Ecdh()
        val hNonceAndPub = Crypto.randomBytes(16) + host.pub
        val shared = host.derive(client.hs1.copyOfRange(16, 80))!!
        val hostSession = Session.derive(psk, client.hs1, hNonceAndPub, shared, isHost = true)
        val hs2 = hNonceAndPub + Session.hostMac(psk, hostSession.transcriptHash)
        val clientSession = client.finish(psk, hs2)
        assertNotNull(clientSession)
        val wire = clientSession!!.seal("ping".toByteArray())
        assertArrayEquals("ping".toByteArray(), hostSession.open(wire, wire.size))
        assertNull("a wrong pairing key fails", ClientHandshake().finish(Crypto.randomBytes(16), hs2))
    }

    @Test
    fun identifySignatureIsRawAndVerifies() {
        val id = DeviceIdentity.generate()
        val th = Crypto.randomBytes(32)
        val body = id.buildIdentify(th, "Pixel 9")!!
        assertEquals(Proto.IDENTIFY_LEN, body.size)
        assertArrayEquals(id.pub, body.copyOfRange(0, 64))
        // Re-encode r|s as DER and verify it the way the host does (signature over the digest itself).
        val raw = body.copyOfRange(64, 128)
        val der = rawToDer(raw)
        val pub = Crypto.publicFromRaw(id.pub, (Crypto.newEcKeyPair().public as java.security.interfaces.ECPublicKey).params)!!
        val v = Signature.getInstance("NONEwithECDSA")
        v.initVerify(pub)
        v.update(Crypto.sha256("rd-device".toByteArray(), th))
        assertTrue(v.verify(der))
        assertEquals("Pixel 9", String(body, 128, 7))
    }

    @Test
    fun keyMap() {
        val a = Keys.forChar('a')!!
        assertEquals(0x1E, a.scan)
        assertFalse(a.shift)
        val at = Keys.forChar('@')!!
        assertEquals(0x03, at.scan)
        assertTrue(at.shift)
        assertEquals(4, Keys.events(at).size)  // shift down, key down, key up, shift up
        assertTrue(Keys.isExtended(Keys.LEFT))
    }

    @Test
    fun packetHeaderLayout() {
        val p = buildPacket(Proto.T_INPUT, 7, byteArrayOf(1, 2, 3))
        assertEquals(24 + 3, p.size)
        val h = PacketHeader.parse(p, p.size)!!
        assertEquals(Proto.T_INPUT, h.type)
        assertEquals(3, h.payloadLen)
        assertEquals(7L, h.frameId)
    }

    private fun rawToDer(raw: ByteArray): ByteArray {
        fun int(b: ByteArray): ByteArray {
            var v = b.dropWhile { it.toInt() == 0 }.toByteArray()
            if (v.isEmpty() || v[0] < 0) v = byteArrayOf(0) + v
            return byteArrayOf(2, v.size.toByte()) + v
        }
        val body = int(raw.copyOfRange(0, 32)) + int(raw.copyOfRange(32, 64))
        return byteArrayOf(0x30, body.size.toByte()) + body
    }
}
