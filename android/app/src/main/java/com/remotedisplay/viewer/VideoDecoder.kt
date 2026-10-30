package com.remotedisplay.viewer

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import java.nio.ByteBuffer

// Hardware H.264 decoding straight onto the screen (MediaCodec with an output Surface).
// The host sends Annex B access units; the first keyframe carries SPS and PPS, which configure the decoder.
class VideoDecoder(private val surface: Surface, private val onVideoSize: (Int, Int) -> Unit) {
    private var codec: MediaCodec? = null
    private val info = MediaCodec.BufferInfo()
    private var pts = 0L

    // False when the frame could not be decoded; the caller then asks the host for a keyframe.
    fun decode(data: ByteArray, length: Int, keyframe: Boolean): Boolean {
        val c = codec ?: (if (keyframe) create(data, length) else null) ?: return false
        codec = c
        return try {
            val idx = c.dequeueInputBuffer(20_000)
            if (idx >= 0) {
                val buf = c.getInputBuffer(idx) ?: return false
                buf.clear()
                if (buf.capacity() < length) return false
                buf.put(data, 0, length)
                c.queueInputBuffer(idx, 0, length, pts, if (keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
                pts += 16_666
            }
            drain(c)
            true
        } catch (e: Exception) {  // CodecException / IllegalStateException: start again at the next keyframe
            release()
            false
        }
    }

    private fun drain(c: MediaCodec) {
        while (true) {
            val o = c.dequeueOutputBuffer(info, 0)
            when {
                o >= 0 -> c.releaseOutputBuffer(o, true)
                o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> reportSize(c.outputFormat)
                else -> return
            }
        }
    }

    private fun reportSize(f: MediaFormat) {
        var w = f.getInteger(MediaFormat.KEY_WIDTH)
        var h = f.getInteger(MediaFormat.KEY_HEIGHT)
        if (f.containsKey("crop-right") && f.containsKey("crop-left")) w = f.getInteger("crop-right") - f.getInteger("crop-left") + 1
        if (f.containsKey("crop-bottom") && f.containsKey("crop-top")) h = f.getInteger("crop-bottom") - f.getInteger("crop-top") + 1
        if (w > 0 && h > 0) onVideoSize(w, h)
    }

    private fun create(data: ByteArray, length: Int): MediaCodec? {
        val nals = splitAnnexB(data, length)
        val sps = nals.firstOrNull { (data[it.first].toInt() and 0x1F) == 7 } ?: return null
        val pps = nals.firstOrNull { (data[it.first].toInt() and 0x1F) == 8 } ?: return null
        val start = byteArrayOf(0, 0, 0, 1)
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080)
        format.setByteBuffer("csd-0", ByteBuffer.wrap(start + data.copyOfRange(sps.first, sps.second)))
        format.setByteBuffer("csd-1", ByteBuffer.wrap(start + data.copyOfRange(pps.first, pps.second)))
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 shl 20)
        if (Build.VERSION.SDK_INT >= 30) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        format.setInteger(MediaFormat.KEY_PRIORITY, 0)  // real time
        return try {
            MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).apply {
                configure(format, surface, null, 0)
                start()
            }
        } catch (e: Exception) {
            null
        }
    }

    fun release() {
        try {
            codec?.stop()
        } catch (e: Exception) {
        }
        try {
            codec?.release()
        } catch (e: Exception) {
        }
        codec = null
    }

    companion object {
        // NAL units of an Annex B buffer as [start, end) ranges, without their start codes.
        fun splitAnnexB(d: ByteArray, len: Int): List<Pair<Int, Int>> {
            val starts = mutableListOf<Int>()
            var i = 0
            while (i + 3 <= len) {
                if (d[i].toInt() == 0 && d[i + 1].toInt() == 0 && d[i + 2].toInt() == 1) {
                    starts += i + 3
                    i += 3
                } else {
                    i++
                }
            }
            return starts.mapIndexed { k, s ->
                var e = if (k + 1 < starts.size) starts[k + 1] - 3 else len
                while (e > s && d[e - 1].toInt() == 0) e--  // trailing zero of a 4-byte start code
                s to e
            }
        }
    }
}
