package csral.camdroid

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.SurfaceTexture
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.camera2.CameraCharacteristics
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.util.Size
import android.view.Surface
import androidx.annotation.RequiresPermission
import csral.camdroid.camera.CameraController
import java.nio.ByteBuffer
import kotlin.math.atan2
import kotlin.math.roundToInt
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

// The service, rather than a visible preview, owns active capture and its connection.
data class StreamingRequest(
    val connection: TcpStreamer,
    val size: Size,
    val front: Boolean,
    val rotation: Int,

    val fps: Int,
    val bitrate: Int,
    val userSelected: Boolean
)

data class StreamingState(
    val request: StreamingRequest? = null,
    val tuning: StreamTuning? = null,
    val preparing: Boolean = false,
    val streaming: Boolean = false,

    val error: String? = null,
    val revision: Long = 0
)

private class StreamOrientationMonitor(context: Context) : AutoCloseable {

    private data class Target(val peer: TcpStreamer, val front: Boolean, val sensor: Int, val fallback: Int)

    private val manager = checkNotNull(context.getSystemService(SensorManager::class.java))
    private val sensor = manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val thread = HandlerThread("CamDroid-orientation").apply { start() }
    private val handler = Handler(thread.looper)
    @Volatile private var closed = false

    private var target: Target? = null
    private var quadrant: Int? = null
    private var sentRotation: Int? = null

    private val listener =
        object : SensorEventListener {

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

            override fun onSensorChanged(event: SensorEvent) {

                if (closed)
                    return

                val x = -event.values[0].toDouble()
                val y = -event.values[1].toDouble()
                val z = -event.values[2].toDouble()
                if ((x * x + y * y) * 4 < z * z || x * x + y * y < 1)
                    return

                // Match Android's orientation convention; retain the last quadrant when lying flat.
                val angle = ((90 - Math.toDegrees(atan2(-y, x)).roundToInt()) % 360 + 360) % 360
                val candidate = ((angle + 45) / 90 * 90) % 360
                val previous = quadrant

                if (previous != null) {

                    val difference = kotlin.math.abs((angle - previous + 540) % 360 - 180)
                    if (candidate == previous || difference < 55)
                        return

                }

                quadrant = candidate
                publish()

            }

        }

    init {

        handler.post {

            if (
                !closed &&
                    sensor != null &&
                    !manager.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI, handler)
            )
                Log.w("CamDroid", "Physical orientation sensor could not be registered")

        }

    }

    fun configure(peer: TcpStreamer, front: Boolean, sensor: Int, fallback: Int) {

        handler.post {

            if (closed)
                return@post

            if (target?.peer !== peer || target?.front != front)
                sentRotation = null

            target = Target(peer, front, sensor, fallback)
            publish()

        }

    }

    private fun publish() {

        val current = target ?: return
        if (closed || !current.peer.isConnected)
            return

        val display = quadrant?.let { (360 - it) % 360 }
        val rotation =
            if (display == null)
                current.fallback
            else if (current.front) (current.sensor + display) % 360 else (current.sensor - display + 360) % 360

        if (rotation == sentRotation)
            return

        runCatching { current.peer.updateVideoRotation(rotation) }
            .onSuccess { sentRotation = rotation }
            .onFailure { Log.w("CamDroid", "Could not update stream orientation", it) }

    }

    override fun close() {

        closed = true
        handler.removeCallbacksAndMessages(null)

        handler.post {
            manager.unregisterListener(listener)
            target = null
            thread.quitSafely()
        }

    }

    fun join() {
        thread.join()
    }

}

class StreamingService : Service() {

    companion object {

        private const val CHANNEL = "camdroid-streaming"
        private const val NOTIFICATION = 6969
        private const val STOP = "csral.camdroid.STOP_STREAM"
        private val mutableState = MutableStateFlow(StreamingState())
        val state = mutableState.asStateFlow()

        @Volatile private var instance: StreamingService? = null
        private var pending: StreamingRequest? = null
        private var preview: Surface? = null
        private var previewTexture: SurfaceTexture? = null

        val isActive: Boolean
            get() = state.value.preparing || state.value.streaming

        fun start(context: Context, request: StreamingRequest, surface: Surface?, texture: SurfaceTexture?) {

            val service = instance
            check(service?.closing != true) { "The previous camera stream is still stopping. Try again." }
            preview = surface
            previewTexture = texture

            if (service != null) {

                service.updatePreview()
                service.configure(request)
                return

            }

            pending = request
            mutableState.value = StreamingState(request = request, preparing = true)

            try {

                context.startForegroundService(Intent(context, StreamingService::class.java))

            } catch (error: Exception) {
                pending = null
                mutableState.value = StreamingState(error = error.message)
                throw error
            }

        }

        suspend fun awaitStopped() {

            withTimeout(5000) {
                while (instance != null)
                    delay(10)
            }

        }

        fun attachPreview(surface: Surface?, texture: SurfaceTexture? = null) {
            preview = surface
            previewTexture = texture
            instance?.takeUnless { it.closing }?.updatePreview()
        }

        fun detachPreview(surface: Surface?) {
            if (preview === surface)
                attachPreview(null)
        }

        fun stop(context: Context, disconnect: Boolean = false) {

            pending = null
            val service = instance

            if (service != null) {

                service.shutdown(disconnect)

            } else {

                val peer = mutableState.value.request?.connection
                mutableState.value = StreamingState()
                preview = null
                previewTexture = null
                context.stopService(Intent(context, StreamingService::class.java))

                if (disconnect)
                    peer?.close()

            }

        }

    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val requests = MutableStateFlow<StreamingRequest?>(null)
    private val encoderLock = Mutex()
    private lateinit var controller: CameraController
    private val encoder = VideoEncoder()
    private lateinit var orientation: StreamOrientationMonitor

    private var capture: Job? = null
    private var failure: Job? = null
    private var adaptive: Job? = null
    private var delivery: Delivery? = null

    private var wakeLock: PowerManager.WakeLock? = null

    private var activeRequest: StreamingRequest? = null
    private var remote: IncomingPacket? = null
    private var remoteTimeout: Job? = null
    private var generation = 0L
    private var destroyed = false

    private var cleanupFinished = false
    private var closing = false
    private var disconnectOnClose = false

    private data class Delivery(val expected: Long, val usable: Long, val queueMs: Int, val arrived: Long)

    private fun announceCapture(request: StreamingRequest) {

        scope.launch {

            try {

                request.connection.announceCapture(
                    this@StreamingService,
                    request.front,
                    request.size,
                    request.fps,

                    request.bitrate
                )

            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                Log.w("CamDroid", "Could not announce camera capabilities", error)
            }

        }

    }

    private fun receiveDelivery(packet: IncomingPacket) {

        check(packet.flags and 0xF7 == 0 && packet.payloadFlags == 0 && packet.payload.size == 11)
        val bytes = ByteBuffer.wrap(packet.payload)
        check(bytes.get().toInt() == 0x12)
        val expected = bytes.int.toLong() and 0xffffffffL
        val usable = bytes.int.toLong() and 0xffffffffL
        val age = bytes.short.toInt() and 0xffff

        check(usable <= expected)
        delivery = Delivery(expected, usable, age, SystemClock.elapsedRealtime())

    }

    private fun startAdaptive(epoch: Long) {

        adaptive?.cancel()
        delivery = null
        adaptive = scope.launch {

            var healthy = 0
            var lastReport = -1L
            val started = SystemClock.elapsedRealtime()

            while (isActive && epoch == generation && !closing) {

                delay(1000)
                val request = activeRequest ?: continue
                val state = mutableState.value
                val tuning = state.tuning ?: continue

                if (request.bitrate != 0 || !state.streaming) {

                    healthy = 0
                    continue

                }

                val minimum = maxOf(2_000_000, tuning.encoder.minBitrate)
                val maximum = minOf(40_000_000, tuning.encoder.maxBitrate)
                if (minimum > maximum)
                    continue

                val report = delivery
                val stale = SystemClock.elapsedRealtime() - (report?.arrived ?: started) > 3000
                if (!stale && (report == null || report.arrived == lastReport || report.expected == 0L))
                    continue

                lastReport = report?.arrived ?: -1L
                val overloaded =
                    stale ||
                        report == null ||
                        report.queueMs >= 50 ||
                        report.usable.toDouble() / report.expected.coerceAtLeast(1) < 0.97

                healthy = if (overloaded) 0 else healthy + 1

                val target =
                    when {

                        overloaded -> (tuning.bitrate * 0.8).toInt()
                        healthy >= 3 -> tuning.bitrate + maxOf(500_000, tuning.bitrate / 10)
                        else -> tuning.bitrate

                    }.coerceIn(minimum, maximum)

                if (healthy >= 3)
                    healthy = 0

                if (target == tuning.bitrate)
                    continue

                try {

                    withContext(Dispatchers.IO) {
                        encoderLock.withLock { encoder.updateBitrate(target) }
                    }

                    if (epoch == generation && activeRequest === request && !closing)
                        mutableState.value =

                            mutableState.value.copy(
                                tuning = tuning.copy(bitrate = target),
                                revision = mutableState.value.revision + 1
                            )

                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    Log.w("CamDroid", "Adaptive bitrate update rejected", error)
                    return@launch
                }

            }

        }

    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {

        super.onCreate()
        instance = this
        controller = CameraController(this)
        orientation = StreamOrientationMonitor(this)
        controller.onCaptureError = { message -> shutdown(false, message) }
        val manager = getSystemService(NotificationManager::class.java)

        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "Camera streaming", NotificationManager.IMPORTANCE_LOW)
        )

    }

    @RequiresPermission(Manifest.permission.CAMERA)
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {

        if (intent?.action == STOP) {

            shutdown(true)
            return START_NOT_STICKY

        }

        val request = pending ?: activeRequest

        if (request == null || closing) {

            stopSelf(startId)
            return START_NOT_STICKY

        }

        try {

            val open =
                PendingIntent.getActivity(
                    this,
                    0,
                    Intent(this, MainActivity::class.java)

                        .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                )
            val stop =
                PendingIntent.getService(
                    this,

                    1,
                    Intent(this, StreamingService::class.java).setAction(STOP),
                    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                )

            val notification =
                Notification.Builder(this, CHANNEL)
                    .setSmallIcon(R.drawable.ic_streaming)
                    .setContentTitle("You are streaming")
                    .setContentText("CamDroid is streaming your camera to a desktop.")

                    .setContentIntent(open)
                    .setOngoing(true)
                    .setCategory(Notification.CATEGORY_SERVICE)
                    .setOnlyAlertOnce(true)
                    .addAction(Notification.Action.Builder(null, "Stop streaming", stop).build())
                    .build()

            if (Build.VERSION.SDK_INT >= 29)
                startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            else startForeground(NOTIFICATION, notification)

            if (wakeLock == null) {

                wakeLock =
                    getSystemService(PowerManager::class.java)
                        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "CamDroid:CameraStreaming")
                        .apply {
                            setReferenceCounted(false)
                            acquire()
                        }

            }

            if (capture == null)
                launchCapture()

            pending = null
            configure(request)

        } catch (error: Exception) {
            shutdown(false, error.message ?: "Could not start background camera streaming.")
        }

        return START_NOT_STICKY

    }

    private fun updatePreview() {

        val state = mutableState.value
        val request = state.request
        val tuning = state.tuning
        val surface = preview?.takeIf { it.isValid }
        val texture = previewTexture

        try {

            if (surface == null || texture == null || request == null || tuning == null) {

                controller.setPreviewSurface(null)
                return

            }

            val facing =
                if (request.front)
                    CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

            val size = controller.previewSize(facing, request.size, tuning.fpsRange)
            texture.setDefaultBufferSize(size.width, size.height)
            controller.setPreviewSurface(surface)

        } catch (error: Exception) {

            Log.w("CamDroid", "Preview attachment failed; retrying encoder-only capture", error)
            runCatching {
                controller.setPreviewSurface(null)
                controller.reconfigure()
            }

                .onFailure {
                    shutdown(false, it.message ?: "Could not restore encoder-only capture.")
                }

        }

    }

    @RequiresPermission(Manifest.permission.CAMERA)
    private fun launchCapture() {

        capture = scope.launch {

            requests.collectLatest { request ->
                if (request == null)
                    return@collectLatest

                val epoch = ++generation

                try {

                    val facing =
                        if (request.front)
                            CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                    check(request.size in controller.getSupportedResolutions(facing)) {
                        "Camera does not support this resolution."
                    }

                    val tuning =
                        selectStreamTuning(
                            request.fps,
                            request.bitrate,
                            controller.getSupportedFpsRanges(facing, request.size),

                            VideoEncoder.limits(request.size)
                        )
                    mutableState.value =
                        StreamingState(request, tuning, preparing = true, revision = mutableState.value.revision + 1)
                    val surface =
                        withContext(Dispatchers.IO) {

                            encoderLock.withLock {

                                encoder.start(
                                    request.size.width,
                                    request.size.height,
                                    request.connection,
                                    frameRate = tuning.encoderFps,

                                    bitsPerSecond = tuning.bitrate,
                                    codecName = tuning.encoder.codecName,
                                    rotation = request.rotation,
                                    userFormatSelected = request.userSelected,
                                    onStreaming = {

                                        scope.launch {

                                            if (epoch == generation && !closing) {

                                                mutableState.value =
                                                    StreamingState(
                                                        activeRequest ?: request,
                                                        tuning,
                                                        streaming = true,

                                                        revision = mutableState.value.revision + 1
                                                    )
                                                announceCapture(activeRequest ?: request)
                                                startAdaptive(epoch)
                                                reply(0)

                                            }

                                        }

                                    },

                                    onError = { error ->
                                        scope.launch {
                                            if (epoch == generation && !closing)
                                                shutdown(false, error.message ?: "Camera encoder failed.")
                                        }

                                    }
                                )

                            }

                        }

                    controller.setCaptureMode(facing, request.size, tuning.fpsRange)
                    updatePreview()
                    controller.setEncoderSurface(surface)
                    controller.openCamera(facing)
                    awaitCancellation()

                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    shutdown(false, error.message ?: "Could not configure camera streaming.")
                } finally {

                    ++generation
                    adaptive?.cancel()
                    controller.closeCamera()
                    controller.setEncoderSurface(null)
                    withContext(NonCancellable + Dispatchers.IO) {

                        adaptive?.join()
                        adaptive = null
                        runCatching { encoderLock.withLock { encoder.stop(closeConnection = false) } }

                            .onFailure {
                                Log.w("CamDroid", "Encoder cleanup failed", it)
                                request.connection.close()
                            }

                    }

                }

            }

        }

    }

    private fun configure(incoming: StreamingRequest) {

        if (closing)
            return

        val existing = activeRequest
        val request =
            if (existing != null && existing.front == incoming.front)
                incoming.copy(rotation = existing.rotation)
            else incoming
        val facing = if (request.front) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

        orientation.configure(
            request.connection,
            request.front,
            controller.videoRotation(facing, Surface.ROTATION_0),
            incoming.rotation
        )

        if (request == existing)
            return

        check(activeRequest == null || activeRequest?.connection === request.connection) {
            "Disconnect before changing peers."
        }

        val previous = activeRequest
        adaptive?.cancel()
        adaptive = null
        delivery = null
        activeRequest = request

        request.connection.setServicePacketListener { packet ->
            if (packet.type != 1 || packet.payload.firstOrNull()?.toInt() !in listOf(0x10, 0x11, 0x12))
                false
            else {

                scope.launch {
                    if (packet.payload[0].toInt() == 0x12)
                        receiveDelivery(packet) else handleRequest(packet)
                }

                true

            }

        }

        if (failure == null)
            failure = scope.launch {

                request.connection.failure.collect { error ->
                    if (error != null && !closing)
                        shutdown(true, error)
                }

            }

        if (
            previous != null &&
                mutableState.value.streaming &&
                request.copy(bitrate = previous.bitrate, userSelected = previous.userSelected) == previous
        ) {

            scope.launch {

                try {

                    val facing =
                        if (request.front)
                            CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                    val target =
                        selectStreamTuning(
                            request.fps,
                            request.bitrate,
                            controller.getSupportedFpsRanges(facing, request.size),

                            VideoEncoder.limits(request.size)
                        )
                    withContext(Dispatchers.IO) {

                        encoderLock.withLock {
                            encoder.updateBitrate(target.bitrate)
                            encoder.updateUserSelection(request.userSelected)
                        }

                    }

                    mutableState.value =
                        mutableState.value.copy(
                            request = request,
                            tuning = target,
                            revision = mutableState.value.revision + 1

                        )
                    announceCapture(request)
                    startAdaptive(generation)
                    reply(0)

                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    shutdown(false, error.message ?: "Could not update encoder settings.")
                }

            }

        } else {

            requests.value = request

        }

    }

    private fun reply(status: Int, packet: IncomingPacket? = remote) {

        packet ?: return
        activeRequest?.connection?.let { peer ->
            runCatching { peer.sendPacket(1, if (status == 0) 1 else 3, payload = packet.payload + status.toByte()) }
        }

        if (packet === remote) {

            remote = null
            remoteTimeout?.cancel()

        }

    }

    private fun handleRequest(packet: IncomingPacket) {

        val previous = activeRequest ?: return

        if (remote != null || !mutableState.value.streaming || closing) {

            reply(2, packet)
            return

        }

        try {

            val request =
                if (packet.payload[0].toInt() == 0x11) {

                    val front = !previous.front
                    val facing =
                        if (front)
                            CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                    val sizes =
                        controller.getSupportedResolutions(facing).filter {
                            it in listOf(Size(3840, 2160), Size(1920, 1080), Size(1280, 720), Size(640, 480))
                        }

                    val size =
                        sizes
                            .filter { it.width.toLong() * it.height <= previous.size.width.toLong() * previous.size.height }
                            .maxByOrNull { it.width.toLong() * it.height }
                            ?: sizes.minByOrNull { it.width.toLong() * it.height }

                            ?: error("The other camera is unavailable.")
                    val display =
                        if (previous.front)
                            (previous.rotation -
                                controller.videoRotation(CameraCharacteristics.LENS_FACING_FRONT, Surface.ROTATION_0) + 360) %

                                360
                        else
                            (controller.videoRotation(CameraCharacteristics.LENS_FACING_BACK, Surface.ROTATION_0) -
                                previous.rotation + 360) % 360
                    val displayRotation =
                        when (display) {

                            90 -> Surface.ROTATION_90
                            180 -> Surface.ROTATION_180
                            270 -> Surface.ROTATION_270
                            else -> Surface.ROTATION_0

                        }

                    previous.copy(
                        front = front,
                        size = size,
                        rotation = controller.videoRotation(facing, displayRotation),
                        userSelected = true
                    )

                } else {

                    val format = checkNotNull(VideoFormatRequest.parse(packet))
                    val size = Size(maxOf(format.width, format.height), minOf(format.width, format.height))
                    val facing =
                        if (previous.front)
                            CameraCharacteristics.LENS_FACING_FRONT

                        else CameraCharacteristics.LENS_FACING_BACK
                    check(size in controller.getSupportedResolutions(facing))
                    check(format.bitrate >= -1)
                    val requestedBitrate =
                        when (format.bitrate) {

                            -1 -> 0
                            0 -> previous.bitrate
                            else -> format.bitrate

                        }

                    val target =
                        selectStreamTuning(
                            format.fps,
                            requestedBitrate,
                            controller.getSupportedFpsRanges(facing, size),

                            VideoEncoder.limits(size)
                        )
                    check(target.encoderFps == format.fps && (format.bitrate <= 0 || target.bitrate == format.bitrate))
                    previous.copy(size = size, fps = format.fps, bitrate = requestedBitrate, userSelected = true)

                }

            if (request == previous) {

                reply(0, packet)
                return

            }

            remote = packet
            remoteTimeout = scope.launch {
                delay(10000)
                reply(3)
                configure(previous)
            }

            configure(request)

        } catch (_: Exception) {
            reply(1, packet)
        }

    }

    private fun shutdown(disconnect: Boolean, error: String? = null) {

        disconnectOnClose = disconnectOnClose || disconnect

        if (closing)
            return

        closing = true
        orientation.close()
        val ending = activeRequest ?: mutableState.value.request ?: pending
        pending = null
        reply(3)
        val peer = ending?.connection

        peer?.setServicePacketListener(null)
        controller.closeCamera()
        failure?.cancel()

        scope.launch {

            try {

                capture?.cancelAndJoin()

            } finally {

                withContext(NonCancellable + Dispatchers.IO) { orientation.join() }

                if (disconnectOnClose)
                    peer?.close()

                runCatching { wakeLock?.takeIf { it.isHeld }?.release() }
                wakeLock = null
                mutableState.value =
                    StreamingState(request = ending, error = error, revision = mutableState.value.revision + 1)
                preview = null
                previewTexture = null

                activeRequest = null
                cleanupFinished = true
                if (destroyed && instance === this@StreamingService)
                    instance = null

                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                scope.cancel()

            }

        }

    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        shutdown(true)
        super.onTaskRemoved(rootIntent)
    }

    override fun onDestroy() {

        destroyed = true

        if (!closing)
            shutdown(true)

        if (cleanupFinished && instance === this)
            instance = null

        super.onDestroy()

    }

}
