package com.remotedisplay.viewer.core

import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// Talks to a real Windows host, so the Android code is checked against the C++ side (byte order, signatures):
//   gradlew testDebugUnitTest -Drd.host=127.0.0.1:47820 -Drd.key=XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-X
// Skipped when no host is given.
class HostInteropTest {
    @Test
    fun connectsAndReceivesVideo() {
        val target = System.getProperty("rd.host").orEmpty()
        val keyText = System.getProperty("rd.key").orEmpty()
        assumeTrue("no -Drd.host given", target.isNotEmpty() && keyText.isNotEmpty())
        val i = target.lastIndexOf(':')
        val host = if (i > 0) target.substring(0, i) else target
        val port = if (i > 0) target.substring(i + 1).toInt() else Proto.DEFAULT_PORT
        val psk = PairingKey.parse(keyText)!!

        val connected = CountDownLatch(1)
        val gotKey = CountDownLatch(1)
        var frames = 0
        val states = mutableListOf<Connection.State>()
        val conn = Connection(host, port, psk, DeviceIdentity.generate(), "JVM interop test", object : Connection.Listener {
            override fun onState(state: Connection.State, controlAllowed: Boolean) {
                states.add(state)
                println("state: $state control=$controlAllowed")
                if (state == Connection.State.CONNECTED) connected.countDown()
            }

            override fun onFrame(data: ByteArray, length: Int, keyframe: Boolean): Boolean {
                frames++
                if (keyframe) {
                    val nals = annexBTypes(data, length)
                    println("keyframe: $length bytes, NAL types $nals")
                    if (7 in nals && 8 in nals) gotKey.countDown()
                }
                return true
            }

        })
        conn.start()
        try {
            assertTrue("handshake with the Windows host (states: $states)", connected.await(10, TimeUnit.SECONDS))
            assertTrue("a keyframe with SPS and PPS arrives", gotKey.await(10, TimeUnit.SECONDS))
            Thread.sleep(2000)
            println("frames received: $frames")
            assertTrue(frames >= 1)
        } finally {
            conn.stop()
        }
    }

    private fun annexBTypes(d: ByteArray, len: Int): Set<Int> {
        val out = mutableSetOf<Int>()
        var i = 0
        while (i + 3 < len) {
            if (d[i].toInt() == 0 && d[i + 1].toInt() == 0 && d[i + 2].toInt() == 1) {
                out += d[i + 3].toInt() and 0x1F
                i += 3
            } else i++
        }
        return out
    }
}
