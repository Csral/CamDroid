package csral.camdroid

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Size
import android.view.Surface
import java.nio.ByteBuffer
import kotlin.math.ceil
import kotlin.math.floor

/** Limits belong to the exact AVC encoder used for this size, not to the device in general. */
data class EncoderLimits(val codecName: String, val minFps: Int, val maxFps: Int, val minBitrate: Int, val maxBitrate: Int)

class VideoEncoder(private val fps: Int = 30, private val bitrate: Int = 4_000_000) {

    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private val codecAccess = Any()
    @Volatile private var running = false
    private var consumer: Thread? = null

    private var tcpStreamer: TcpStreamer? = null
    @Volatile private var userSelectedFormat = false
    @Volatile private var configuredFps = fps
    @Volatile private var configuredBitrate = bitrate

    companion object {

        fun supportedRates(size: Size, ranges: List<android.util.Range<Int>>, encoder: EncoderLimits): List<Int> {

            val info = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.first { it.name == encoder.codecName }
            val video = info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities ?: return emptyList()

            return ranges
                .map { it.upper }
                .distinct()
                .filter {
                    it in 1..240 &&
                        it in encoder.minFps..encoder.maxFps &&
                        video.areSizeAndRateSupported(size.width, size.height, it.toDouble())
                }

                .sorted()

        }

        fun limits(size: Size): EncoderLimits {

            val candidates =
                MediaCodecList(MediaCodecList.REGULAR_CODECS)
                    .codecInfos
                    .filter {
                        it.isEncoder && it.supportedTypes.any { type -> type.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) }
                    }

                    .sortedBy { if (Build.VERSION.SDK_INT >= 29 && it.isHardwareAccelerated) 0 else 1 }

            for (info in candidates) {

                val caps = runCatching { info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC) }.getOrNull() ?: continue
                if (MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface !in caps.colorFormats)
                    continue

                val video = caps.videoCapabilities ?: continue
                if (!video.isSizeSupported(size.width, size.height))
                    continue

                val rates = video.getSupportedFrameRatesFor(size.width, size.height)
                val minRate = ceil(rates.lower - 0.000001).toInt().coerceAtLeast(1)
                val maxRate = floor(rates.upper + 0.000001).toInt().coerceAtMost(255)
                if (maxRate < minRate)
                    continue

                return EncoderLimits(info.name, minRate, maxRate, video.bitrateRange.lower, video.bitrateRange.upper)

            }

            error("No surface AVC encoder supports ${size.width}×${size.height}.")

        }

    }

    @Synchronized
    fun start(
        width: Int,
        height: Int,
        connectedStreamer: TcpStreamer? = null,

        onStreaming: () -> Unit = {},
        onError: (Throwable) -> Unit = {},
        frameRate: Int = fps,
        bitsPerSecond: Int = bitrate,
        codecName: String? = null,

        rotation: Int = 0,
        userFormatSelected: Boolean = false
    ): Surface {

        check(codec == null && consumer == null) { "Video encoder is still stopping." }
        require(frameRate > 0 && bitsPerSecond > 0)
        val streamer = requireNotNull(connectedStreamer) { "Connect to the desktop before starting video." }
        check(streamer.isConnected) { "Desktop session is not established." }
        val format =
            MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {

                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
                setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, frameRate.toFloat())
                setInteger(MediaFormat.KEY_BIT_RATE, bitsPerSecond)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)

            }

        val encoder =
            if (codecName == null)
                MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            else MediaCodec.createByCodecName(codecName)
        var surface: Surface? = null
        try {

            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            surface = encoder.createInputSurface()
            encoder.start()

        } catch (error: Throwable) {
            runCatching { encoder.release() }
            runCatching { surface?.release() }
            throw error
        }

        codec = encoder
        inputSurface = surface
        tcpStreamer = streamer
        userSelectedFormat = userFormatSelected

        configuredFps = frameRate
        configuredBitrate = bitsPerSecond
        running = true
        val ownedSurface = checkNotNull(surface)
        try {

            val worker =
                Thread(

                    {

                        try {

                            consumeEncoder(encoder, streamer, onStreaming, rotation)

                        } catch (error: Throwable) {

                            if (running) {

                                runCatching { streamer.close() }
                                onError(error)

                            }

                        } finally {

                            running = false
                            // The output worker owns these until its final buffer is returned.
                            synchronized(codecAccess) {

                                runCatching { encoder.stop() }
                                    .onFailure { android.util.Log.w("CamDroid", "Encoder stop failed", it) }
                                runCatching { encoder.release() }
                                    .onFailure { android.util.Log.w("CamDroid", "Encoder release failed", it) }

                            }

                            runCatching { ownedSurface.release() }

                        }

                    },

                    "CamDroid-encoder-output"
                )
            consumer = worker
            worker.start()

        } catch (error: Throwable) {

            running = false
            consumer = null
            codec = null
            inputSurface = null
            tcpStreamer = null

            runCatching { encoder.stop() }
            runCatching { encoder.release() }
            runCatching { ownedSurface.release() }
            runCatching { streamer.close() }
            throw error

        }

        return checkNotNull(surface)

    }

    /** Returns false only if a stalled write forced the shared connection to close. */
    @Synchronized
    fun stop(closeConnection: Boolean = true): Boolean {

        running = false
        val streamer = tcpStreamer
        if (closeConnection)
            runCatching { streamer?.close() }

        val worker = consumer
        var preserved = !closeConnection
        try {

            worker?.join(1_000)
            if (worker?.isAlive == true) {

                preserved = false
                runCatching { streamer?.close() }
                worker.join(1_000)

            }

            check(worker?.isAlive != true) { "Encoder output did not stop after closing the connection." }

        } catch (error: Throwable) {

            runCatching { streamer?.close() }

            if (error is InterruptedException)
                Thread.currentThread().interrupt()

            throw error

        } finally {

            // A stalled worker retains ownership and releases resources when it exits.
            if (worker?.isAlive != true) {

                consumer = null
                tcpStreamer = null
                codec = null
                inputSurface = null

            }

        }

        return preserved && (streamer == null || streamer.isConnected)

    }

    @Synchronized
    fun updateBitrate(bitsPerSecond: Int) {

        require(bitsPerSecond > 0)
        synchronized(codecAccess) {

            check(running) { "Video encoder is stopped." }

            checkNotNull(codec)
                .setParameters(
                    Bundle().apply {
                        putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitsPerSecond)
                    }
                )

        }

        configuredBitrate = bitsPerSecond
        tcpStreamer?.sendVideoSettings(configuredFps, configuredBitrate, userSelectedFormat)

    }

    @Synchronized
    fun updateUserSelection(selected: Boolean) {

        check(running) { "Video encoder is stopped." }

        if (userSelectedFormat == selected)
            return

        userSelectedFormat = selected
        tcpStreamer?.sendVideoSettings(configuredFps, configuredBitrate, selected)

    }

    private fun consumeEncoder(encoder: MediaCodec, streamer: TcpStreamer, onStreaming: () -> Unit, rotation: Int) {

        val bufferInfo = MediaCodec.BufferInfo()
        val emptyBuffer = ByteBuffer.allocateDirect(0)
        var configSent = false
        var eosSent = false
        var reported = false
        var lastKeyframeRequestMs = -1_000L

        try {

            while (running) {

                val nowMs = SystemClock.elapsedRealtime()
                if (configSent && nowMs - lastKeyframeRequestMs >= 500 && streamer.needsVideoKeyframe()) {

                    runCatching {

                        encoder.setParameters(
                            Bundle().apply {
                                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                            }
                        )

                    }

                        .onFailure {
                            // Periodic encoder keyframes still permit recovery if this
                            // device rejects an explicit sync-frame request.
                            android.util.Log.w("CamDroid-encoder", "Sync-frame request rejected", it)
                        }

                    lastKeyframeRequestMs = nowMs

                }

                val index = encoder.dequeueOutputBuffer(bufferInfo, 10_000)

                when {

                    index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {

                        val format = encoder.outputFormat
                        val csd0 = format.getByteBuffer("csd-0")?.let { copyBuffer(it) } ?: byteArrayOf()
                        val csd1 = format.getByteBuffer("csd-1")?.let { copyBuffer(it) } ?: byteArrayOf()
                        if (!running)
                            return

                        streamer.sendVideoConfig(
                            csd0,
                            csd1,
                            rotation,
                            fps = configuredFps,

                            bitrate = configuredBitrate,
                            userConfigured = userSelectedFormat
                        )
                        configSent = true

                    }

                    index >= 0 ->
                        try {

                            if (!running)
                                return

                            val flags = bufferInfo.flags
                            val codecConfig = flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                            val keyframe = flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            val partial = flags and MediaCodec.BUFFER_FLAG_PARTIAL_FRAME != 0
                            val eos = flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0

                            if (!codecConfig && (bufferInfo.size > 0 || !partial)) {

                                // Zero-size terminal buffers may finish a previous partial unit.
                                if (bufferInfo.size > 0)
                                    check(configSent) {
                                        "Encoder produced a frame before codec configuration."
                                    }

                                if (configSent) {

                                    val buffer =
                                        if (bufferInfo.size == 0)
                                            emptyBuffer
                                        else checkNotNull(encoder.getOutputBuffer(index))

                                    val submitted =
                                        streamer.sendVideoFragment(
                                            buffer,
                                            if (bufferInfo.size == 0)
                                                0 else bufferInfo.offset,

                                            bufferInfo.size,
                                            bufferInfo.presentationTimeUs,
                                            keyframe,
                                            partial
                                        )
                                    if (!reported && submitted) {

                                        reported = true
                                        onStreaming()

                                    }

                                }

                            }

                            if (eos) {

                                streamer.sendVideoEos()
                                eosSent = true
                                return

                            }

                        } finally {

                            // JNI copied the encoded bytes before returning. Native workers
                            // own that copy; they never retain the codec's buffer pointer.
                            encoder.releaseOutputBuffer(index, false)

                        }

                }

            }

        } finally {

            // Normal UI stop ends this stream while preserving the connection.
            // Queued complete frames precede EOS; an unfinished partial unit is discarded.
            // A failure while running is handled by the outer worker and closes the socket.
            if (configSent && !eosSent && !running && streamer.isConnected)
                streamer.sendVideoEos()

        }

    }

    private fun copyBuffer(buffer: ByteBuffer): ByteArray {
        val copy = buffer.duplicate()
        return ByteArray(copy.remaining()).also { copy.get(it) }
    }

}
