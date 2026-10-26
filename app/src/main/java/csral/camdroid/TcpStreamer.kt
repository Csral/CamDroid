package csral.camdroid

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.SystemClock
import android.util.Log
import android.util.Size
import csral.camdroid.camera.CameraController
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.security.SecureRandom
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** One native session/socket survives encoder stops, restarts and control exchanges. */
class TcpStreamer : AutoCloseable {

    private val handle = TransportBridge.create()
    private val closed = AtomicBoolean(false)
    private val capabilityLock = Mutex()
    private var announcedCamera: Pair<Boolean, Size>? = null
    private var announcedSelection: Pair<Int, Int>? = null

    private val connectionFailure = MutableStateFlow<String?>(null)
    val failure = connectionFailure.asStateFlow()
    @Volatile private var packetListener: ((IncomingPacket) -> Unit)? = null

    @Volatile private var servicePacketListener: ((IncomingPacket) -> Boolean)? = null
    @Volatile private var failureListener: ((Throwable) -> Unit)? = null
    @Volatile private var controlWorker: Thread? = null

    companion object {

        // Shutdown is immediate; joining native workers never blocks the UI thread.
        private val cleanup = Executors.newSingleThreadExecutor { task ->
            Thread(task, "CamDroid-connection-cleanup").apply { isDaemon = true }
        }

    }

    val isConnected: Boolean
        get() = !closed.get() && runCatching { TransportBridge.isConnected(handle) }.getOrDefault(false)

    /** Blocking connect/handshake. MainActivity already calls this on Dispatchers.IO. */
    fun connect(host: String, port: Int, networkHandle: Long = 0) {

        check(!closed.get()) { "Use a new TcpStreamer after disconnecting." }

        try {

            TransportBridge.connect(handle, host, port, true, networkHandle)
            check(!closed.get()) { "Connection was cancelled." }
            val worker = Thread({ dispatchPackets() }, "CamDroid-control").apply { isDaemon = true }
            controlWorker = worker
            worker.start()
            Log.i("CamDroid-JNI", "Desktop session established")

        } catch (error: Throwable) {
            close()
            throw error
        }

    }

    suspend fun announceCapture(context: Context, front: Boolean, size: Size, fps: Int, bitrate: Int) {

        capabilityLock.withLock {

            if (!isConnected)
                return@withLock

            val key = front to size
            if (announcedCamera != key) {

                val formats =
                    withContext(Dispatchers.Default) {

                        val controller = CameraController(context.applicationContext)
                        val facing =
                            if (front)
                                CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                        val sizes = controller.getSupportedResolutions(facing)

                        listOf(Size(640, 480), Size(1280, 720), Size(1920, 1080), Size(3840, 2160))
                            .filter { it in sizes }

                            .mapNotNull { candidate ->
                                runCatching {

                                    val encoder = VideoEncoder.limits(candidate)
                                    val rates =
                                        VideoEncoder.supportedRates(
                                            candidate,
                                            controller.getSupportedFpsRanges(facing, candidate),

                                            encoder
                                        )
                                    if (rates.isEmpty())
                                        null else Triple(candidate, encoder, rates)

                                }

                                    .getOrNull()

                            }

                    }

                val bytes = ByteBuffer.allocate(7 + formats.sumOf { 13 + it.third.size * 2 })
                bytes.put(0x13.toByte()).put(if (front) 1.toByte() else 0.toByte())
                bytes.putShort(size.width.toShort()).putShort(size.height.toShort()).put(formats.size.toByte())

                for ((resolution, encoder, rates) in formats) {

                    bytes.putShort(resolution.width.toShort()).putShort(resolution.height.toShort())
                    bytes.putInt(encoder.minBitrate).putInt(encoder.maxBitrate).put(rates.size.toByte())
                    rates.forEach { bytes.putShort(it.toShort()) }

                }

                sendPacket(1, 0, payload = bytes.array())
                announcedCamera = key

            }

            val selection = fps to bitrate
            if (announcedSelection != selection) {

                val bytes = ByteBuffer.allocate(7).put(0x14.toByte()).putShort(fps.toShort()).putInt(bitrate)
                sendPacket(1, 0, payload = bytes.array())
                announcedSelection = selection

            }

        }

    }

    /** Fast enqueue; socket writes happen on the native writer, not the encoder. */
    fun sendVideoConfig(
        csd0: ByteArray,
        csd1: ByteArray,
        rotation: Int = 0,
        mirrored: Boolean = false,

        fps: Int = 30,
        bitrate: Int = 4_000_000,
        userConfigured: Boolean = false
    ) = submit {
        TransportBridge.videoConfig(handle, csd0, csd1, rotation, mirrored, fps, bitrate, userConfigured)
    }

    /** The codec buffer must remain valid until this method returns, then may be released. */
    fun sendVideoFragment(buffer: ByteBuffer, offset: Int, size: Int, ptsUs: Long, keyframe: Boolean, partial: Boolean) =
        submit {
            require(buffer.isDirect) { "Encoder output must be a direct buffer." }
            TransportBridge.videoFragment(handle, buffer, offset, size, ptsUs, keyframe, partial)
        }

    fun sendVideoSettings(fps: Int, bitrate: Int, userConfigured: Boolean = false) = submit {
        TransportBridge.videoSettings(handle, fps, bitrate, userConfigured)
    }

    fun updateVideoRotation(rotation: Int) = submit { TransportBridge.videoRotation(handle, rotation) }

    fun sendVideoEos() = submit { TransportBridge.videoEos(handle) }

    fun needsVideoKeyframe(): Boolean = submit { TransportBridge.needsVideoKeyframe(handle) }

    /** Transport plumbing only: schemas and peer support still belong to the shared protocol. */
    fun sendPacket(type: Int, flags: Int, payloadFlags: Int = 0, payload: ByteArray) = submit {
        TransportBridge.sendPacket(handle, type, flags, payloadFlags, payload)
    }

    /** Listener must be quick. Apply UI changes via the main dispatcher; never touch Compose here. */
    fun setPacketListener(listener: ((IncomingPacket) -> Unit)?) {
        packetListener = listener
    }

    fun setServicePacketListener(listener: ((IncomingPacket) -> Boolean)?) {
        servicePacketListener = listener
    }

    fun setFailureListener(listener: ((Throwable) -> Unit)?) {
        failureListener = listener
    }

    private inline fun <T> submit(action: () -> T): T {

        check(!closed.get()) { "Desktop connection is closed." }

        try {

            return action()

        } catch (error: Exception) {

            // Video backlog recovers natively at a keyframe. Remaining exceptions
            // indicate invalid data, control queue overload or a connection failure.
            connectionFailure.value = error.message ?: "Desktop connection failed."
            close()
            throw error

        }

    }

    private fun dispatchPackets() {

        try {

            while (!closed.get()) {

                val packet = TransportBridge.waitPacket(handle) ?: continue
                if (servicePacketListener?.invoke(packet) == true)
                    continue

                val listener = packetListener
                if (listener != null && !closed.get())
                    listener(packet)
                else Log.d("CamDroid-control", "Received packet type ${packet.type}; no handler installed")

            }

        } catch (error: Throwable) {

            if (!closed.get()) {

                Log.e("CamDroid-control", "Desktop connection failed", error)
                connectionFailure.value = error.message ?: "Desktop connection failed."
                val listener = failureListener
                close()
                runCatching { listener?.invoke(error) }

            }

        }

    }

    override fun close() {

        if (!closed.compareAndSet(false, true))
            return

        packetListener = null
        servicePacketListener = null
        failureListener = null
        // shutdown() wakes both socket workers before cleanup joins them.
        runCatching { TransportBridge.interrupt(handle) }

        cleanup.execute {

            runCatching { TransportBridge.close(handle) }
            val worker = controlWorker
            if (worker !== Thread.currentThread())
                runCatching { worker?.join() }

            controlWorker = null

        }

    }

}

data class DesktopPeer(
    val name: String,
    val host: String,
    val port: Int,
    val networkHandle: Long,

    val responseMs: Long,
    val networkRank: Int = 0
)

object DesktopDiscovery {

    private const val PORT = 39696
    private val requestMagic = "CDDISC01".toByteArray(Charsets.US_ASCII)
    private val responseMagic = "CDPEER01".toByteArray(Charsets.US_ASCII)

    internal fun parseReply(
        bytes: ByteArray,
        size: Int,
        nonce: Long,
        host: String,

        networkHandle: Long,
        responseMs: Long,
        version: Int = 1
    ): DesktopPeer? {

        if (size !in 22..84 || size > bytes.size)
            return null

        if (!bytes.copyOfRange(0, 8).contentEquals(responseMagic))
            return null

        val data = ByteBuffer.wrap(bytes, 0, size)
        if (data.getLong(8) != nonce)
            return null

        val port = data.getShort(16).toInt() and 0xffff
        val nameSize = bytes[20].toInt() and 0xff

        if (
            port < 4097 ||
                bytes[18].toInt() != version ||
                bytes[19].toInt() != 0 ||
                nameSize !in 1..63 ||
                size != 21 + nameSize
        )
            return null

        val name = String(bytes, 21, nameSize, Charsets.UTF_8).filter { it >= ' ' && it != '\u007f' }.trim()
        return DesktopPeer(name.ifEmpty { host }, host, port, networkHandle, responseMs)

    }

    suspend fun find(context: Context): List<DesktopPeer> = coroutineScope {

        val manager = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val networks =
            manager.allNetworks
                .filter { network ->
                    val capabilities = manager.getNetworkCapabilities(network) ?: return@filter false

                    !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN) &&
                        (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
                            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))

                }

                .sortedBy { if (it == manager.activeNetwork) 0 else 1 }
                .take(3)
        check(networks.isNotEmpty()) { "Connect to Wi-Fi to find your desktop, or enter its address manually." }
        val preferences = context.getSharedPreferences("desktop_discovery", Context.MODE_PRIVATE)
        val previousHost = preferences.getString("host", "") ?: ""

        val previousName = preferences.getString("name", "") ?: ""
        val version = NativeBridge.protocolVersion()
        networks
            .mapIndexed { rank, network ->
                async(Dispatchers.IO) {

                    try {

                        scan(manager, network, previousHost, version).map { it.copy(networkRank = rank) }

                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (denied: SecurityException) {
                        throw denied
                    } catch (_: Exception) {
                        emptyList()
                    }

                }

            }

            .awaitAll()
            .flatten()
            .distinctBy { "${it.host}:${it.port}:${it.networkHandle}" }
            .sortedWith(
                compareBy<DesktopPeer> {

                        when {

                            it.name == previousName && it.host == previousHost -> 0
                            it.name == previousName -> 1
                            it.host == previousHost -> 2
                            else -> 3

                        }

                    }

                    .thenBy { it.networkRank }
                    .thenBy { it.responseMs }
                    .thenBy { it.name }
            )

    }

    private suspend fun scan(
        manager: ConnectivityManager,
        network: Network,
        previousHost: String,
        version: Int
    ): List<DesktopPeer> {

        val targets = linkedSetOf(InetAddress.getByName("255.255.255.255"))
        manager.getLinkProperties(network)?.linkAddresses?.forEach { link ->
            val address = link.address as? Inet4Address ?: return@forEach
            if (link.prefixLength !in 1..30)
                return@forEach

            val value = ByteBuffer.wrap(address.address).int
            val mask = -1 shl (32 - link.prefixLength)
            targets += InetAddress.getByAddress(ByteBuffer.allocate(4).putInt(value or mask.inv()).array())

        }

        if (previousHost.isNotEmpty())
            runCatching { targets += InetAddress.getByName(previousHost) }

        val nonce = SecureRandom().nextLong()
        val request = ByteBuffer.allocate(16).put(requestMagic).putLong(nonce).array()
        val peers = mutableMapOf<String, DesktopPeer>()
        DatagramSocket().use { socket ->
            network.bindSocket(socket)

            socket.broadcast = true
            socket.soTimeout = 120
            val buffer = ByteArray(128)
            repeat(3) {

                currentCoroutineContext().ensureActive()
                val sent = SystemClock.elapsedRealtime()
                targets.forEach { target ->
                    runCatching { socket.send(DatagramPacket(request, request.size, target, PORT)) }
                }

                while (SystemClock.elapsedRealtime() - sent < 600) {

                    currentCoroutineContext().ensureActive()
                    val reply = DatagramPacket(buffer, buffer.size)
                    try {

                        socket.receive(reply)

                    } catch (_: SocketTimeoutException) {
                        continue
                    }

                    if (reply.port != PORT || reply.address !is Inet4Address)
                        continue

                    val host = reply.address.hostAddress ?: continue
                    val peer =
                        parseReply(
                            buffer,
                            reply.length,

                            nonce,
                            host,
                            network.networkHandle,
                            SystemClock.elapsedRealtime() - sent,
                            version

                        ) ?: continue
                    val key = "$host:${peer.port}"
                    if (peers[key]?.responseMs?.let { it <= peer.responseMs } != true)
                        peers[key] = peer

                }

            }

        }

        return peers.values.toList()

    }

    fun remember(context: Context, peer: DesktopPeer) {

        context
            .getSharedPreferences("desktop_discovery", Context.MODE_PRIVATE)
            .edit()
            .putString("name", peer.name)
            .putString("host", peer.host)
            .apply()

    }

}
