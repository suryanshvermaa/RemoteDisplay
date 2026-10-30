package com.remotedisplay.viewer.core

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

// LAN discovery, same as app/discovery.h: a "RDQ1" broadcast to the host port; sharing hosts answer "RDA1" + name.
object Discovery {
    data class Found(val ip: String, val name: String)

    // Broadcast address of every IPv4 network this device is on, plus the limited broadcast.
    fun broadcastAddresses(): List<InetAddress> {
        val out = mutableListOf<InetAddress>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (ia in ni.interfaceAddresses) {
                    if (ia.address is Inet4Address) ia.broadcast?.let { out += it }
                }
            }
        } catch (e: Exception) {
        }
        out += InetAddress.getByName("255.255.255.255")
        return out.distinct()
    }

    // Asks the network and collects answers for about waitMs. Blocking: call it off the UI thread.
    fun find(port: Int = Proto.DEFAULT_PORT, waitMs: Long = 1500): List<Found> {
        val found = LinkedHashMap<String, Found>()
        DatagramSocket().use { s ->
            s.broadcast = true
            s.soTimeout = 200
            val targets = broadcastAddresses()
            repeat(2) {  // twice: UDP can lose a packet
                for (t in targets) {
                    try {
                        s.send(DatagramPacket(Proto.DISCOVER_QUERY, Proto.DISCOVER_QUERY.size, t, port))
                    } catch (e: Exception) {
                    }
                }
            }
            val buf = ByteArray(256)
            val deadline = System.currentTimeMillis() + waitMs
            while (System.currentTimeMillis() < deadline) {
                val p = DatagramPacket(buf, buf.size)
                try {
                    s.receive(p)
                } catch (e: SocketTimeoutException) {
                    continue
                }
                if (p.length < 4 || !buf.copyOfRange(0, 4).contentEquals(Proto.DISCOVER_REPLY)) continue
                val ip = p.address.hostAddress ?: continue
                if (found.containsKey(ip)) continue
                val name = String(buf, 4, p.length - 4, Charsets.US_ASCII).filter { it.code in 0x20..0x7E }.take(64)
                found[ip] = Found(ip, name.ifEmpty { ip })
            }
        }
        return found.values.toList()
    }
}
