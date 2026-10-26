package csral.camdroid.ui

import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import android.os.Build
import android.util.Size
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import csral.camdroid.DesktopDiscovery
import csral.camdroid.DesktopPeer
import csral.camdroid.EncoderLimits
import csral.camdroid.R
import csral.camdroid.StreamTuning
import csral.camdroid.StreamingService
import csral.camdroid.TcpStreamer
import csral.camdroid.VideoEncoder
import csral.camdroid.VideoFormatRequest
import csral.camdroid.camera.CameraController
import csral.camdroid.selectStreamTuning
import csral.camdroid.ui.theme.CamDroidTheme
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private data class CaptureCapabilities(val encoder: EncoderLimits?, val rates: List<Int>)

private enum class Screen {

    Method,
    Wifi,
    Camera

}

enum class ConnectionStatus(val label: String) {

    Ready("Ready"),
    Connecting("Connecting"),
    Connected("Connected"),
    Starting("Starting stream"),
    Stopping("Stopping"),

    Streaming("Streaming"),
    Disconnected("Disconnected"),
    Error("Connection error")

}

@Composable
fun CamDroidApp(onConnect: suspend (String, Int, Long) -> TcpStreamer, onDisconnect: () -> Unit) {

    val context = LocalContext.current
    val serviceState by StreamingService.state.collectAsState()
    val networkPermission = "android.permission.ACCESS_LOCAL_NETWORK"

    var networkAllowed by remember {
        mutableStateOf(
            Build.VERSION.SDK_INT < 37 || context.checkSelfPermission(networkPermission) == PackageManager.PERMISSION_GRANTED
        )
    }

    var discoveryAttempt by remember { mutableIntStateOf(0) }
    var discovering by remember { mutableStateOf(false) }
    var discoveredPeer by remember { mutableStateOf<DesktopPeer?>(null) }
    var discoveryStatus by remember { mutableStateOf("Looking for your desktop") }
    var operationGeneration by remember { mutableIntStateOf(0) }

    val permission =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { allowed ->
            networkAllowed = allowed
            if (allowed)
                discoveryAttempt++
            else discoveryStatus = "Allow local network access to find and connect to your desktop."

        }

    LaunchedEffect(Unit) { if (!networkAllowed) permission.launch(networkPermission) }
    var screen by rememberSaveable { mutableStateOf(Screen.Method) }
    var desktopName by remember { mutableStateOf<String?>(null) }
    var selectedNetwork by remember { mutableLongStateOf(0L) }
    var ip by rememberSaveable { mutableStateOf("") }

    var port by rememberSaveable { mutableStateOf("6969") }
    var streaming by remember { mutableStateOf(StreamingService.isActive) }
    var streamBusy by remember { mutableStateOf(false) }
    var cameraReady by remember { mutableStateOf(false) }
    var front by rememberSaveable { mutableStateOf(false) }

    var settings by rememberSaveable { mutableStateOf(false) }
    var resolution by rememberSaveable { mutableStateOf("1080p") }
    var userFormatSelected by rememberSaveable { mutableStateOf(false) }
    var requestedFps by rememberSaveable { mutableStateOf(30) }
    var requestedBitrate by rememberSaveable { mutableStateOf(0) }

    var tuningInfo by remember { mutableStateOf<StreamTuning?>(null) }
    var connectJob by remember { mutableStateOf<Job?>(null) }
    var availableResolutions by remember { mutableStateOf(listOf("1080p")) }
    val captureCapabilities by
        produceState<CaptureCapabilities?>(null, front, resolution) {

            value = null
            value =
                withContext(Dispatchers.Default) {

                    runCatching {

                            val size =
                                when (resolution) {

                                    "4K" -> Size(3840, 2160)
                                    "1080p" -> Size(1920, 1080)
                                    "720p" -> Size(1280, 720)
                                    else -> Size(640, 480)

                                }

                            val facing =
                                if (front)
                                    CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                            val encoder = VideoEncoder.limits(size)
                            val ranges = CameraController(context).getSupportedFpsRanges(facing, size)

                            CaptureCapabilities(encoder, VideoEncoder.supportedRates(size, ranges, encoder))

                        }

                        .getOrDefault(CaptureCapabilities(null, emptyList()))

                }

        }

    val scope = rememberCoroutineScope()
    var connecting by remember { mutableStateOf(false) }

    var connectionError by remember { mutableStateOf<String?>(null) }
    var connection by remember { mutableStateOf(serviceState.request?.connection?.takeIf { it.isConnected }) }
    var status by remember { mutableStateOf(ConnectionStatus.Ready) }

    LaunchedEffect(connection, front, resolution, requestedFps, requestedBitrate) {

        val peer = connection ?: return@LaunchedEffect
        if (StreamingService.isActive)
            return@LaunchedEffect

        val size =
            when (resolution) {

                "4K" -> Size(3840, 2160)
                "1080p" -> Size(1920, 1080)
                "720p" -> Size(1280, 720)
                else -> Size(640, 480)

            }

        try {

            peer.announceCapture(context, front, size, requestedFps, requestedBitrate)

        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            connectionError = error.message ?: "Could not exchange camera capabilities."
        }

    }

    val notificationPermission =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (!granted)
                connectionError = "Allow notifications to start streaming with a visible camera notification."
        }

    LaunchedEffect(serviceState.revision, serviceState.streaming, serviceState.preparing) {

        val request = serviceState.request ?: return@LaunchedEffect

        if (serviceState.streaming || serviceState.preparing) {

            connection = request.connection
            streaming = true
            streamBusy = serviceState.preparing
            screen = Screen.Camera
            front = request.front

            resolution =
                when (maxOf(request.size.width, request.size.height)) {

                    3840 -> "4K"
                    1920 -> "1080p"
                    1280 -> "720p"
                    else -> "480p"

                }

            requestedFps = request.fps
            requestedBitrate = request.bitrate
            userFormatSelected = request.userSelected
            tuningInfo = serviceState.tuning
            status = if (serviceState.streaming) ConnectionStatus.Streaming else ConnectionStatus.Starting

        } else if (connection === request.connection) {

            streaming = false
            streamBusy = false
            connectionError = serviceState.error
            status =
                if (serviceState.error != null)

                    ConnectionStatus.Error
                else if (request.connection.isConnected) ConnectionStatus.Connected else ConnectionStatus.Disconnected

        }

    }

    var formatRequest by remember { mutableStateOf<VideoFormatRequest?>(null) }
    var previousResolution by remember { mutableStateOf("1080p") }

    var previousFps by remember { mutableIntStateOf(30) }
    var previousBitrate by remember { mutableIntStateOf(4_000_000) }
    var requestNeedsRestart by remember { mutableStateOf(false) }
    var formatTimeout by remember { mutableStateOf<Job?>(null) }

    fun rejectFormat() {

        val request = formatRequest ?: return
        formatRequest = null
        formatTimeout?.cancel()
        connection?.let { peer -> runCatching { request.reply(peer, 3) } }
        resolution = previousResolution

        requestedFps = previousFps
        requestedBitrate = previousBitrate
        connectionError = "Requested capture settings failed; restoring the previous capture settings."

    }

    fun acceptFormat() {

        val request = formatRequest ?: return
        formatRequest = null
        formatTimeout?.cancel()
        streamBusy = false
        status = ConnectionStatus.Streaming
        connection?.let { peer -> runCatching { request.reply(peer, 0) } }

    }

    LaunchedEffect(streaming) {
        if (!streaming && formatRequest != null)
            rejectFormat()
    }

    DisposableEffect(connection) {

        val peer = connection
        peer?.setPacketListener { packet ->
            val request = VideoFormatRequest.parse(packet)
            if (request != null)
                scope.launch {

                    if (connection !== peer)
                        return@launch

                    if (screen != Screen.Camera || !streaming || streamBusy || formatRequest != null) {

                        runCatching { request.reply(peer, 2) }

                        return@launch

                    }

                    val w = maxOf(request.width, request.height)
                    val h = minOf(request.width, request.height)
                    val label =
                        when (w to h) {

                            3840 to 2160 -> "4K"
                            1920 to 1080 -> "1080p"
                            1280 to 720 -> "720p"
                            640 to 480 -> "480p"
                            else -> null

                        }

                    val accepted =
                        label != null &&
                            runCatching {

                                val controller = CameraController(context)
                                val facing =
                                    if (front)
                                        CameraCharacteristics.LENS_FACING_FRONT
                                    else CameraCharacteristics.LENS_FACING_BACK

                                val size = Size(w, h)
                                check(size in controller.getSupportedResolutions(facing))
                                check(request.bitrate >= -1)
                                val target =
                                    selectStreamTuning(
                                        request.fps,

                                        if (request.bitrate == -1)
                                            0
                                        else request.bitrate.takeIf { it > 0 } ?: requestedBitrate,
                                        controller.getSupportedFpsRanges(facing, size),
                                        VideoEncoder.limits(size)

                                    )
                                check(target.encoderFps == request.fps)

                                if (request.bitrate > 0)
                                    check(target.bitrate == request.bitrate)

                            }

                                .isSuccess
                    if (!accepted) {

                        runCatching { request.reply(peer, 1) }

                    } else if (

                        resolution == label &&
                            tuningInfo?.encoderFps == request.fps &&
                            (request.bitrate == 0 || tuningInfo?.bitrate == request.bitrate)
                    ) {
                        runCatching { request.reply(peer, 0) }
                    } else {

                        previousResolution = resolution
                        previousFps = requestedFps
                        previousBitrate = requestedBitrate
                        requestNeedsRestart = resolution != label || tuningInfo?.encoderFps != request.fps
                        if (requestNeedsRestart)
                            userFormatSelected = true

                        formatRequest = request
                        resolution = checkNotNull(label)
                        requestedFps = request.fps
                        if (request.bitrate != 0)
                            requestedBitrate = if (request.bitrate == -1) 0 else request.bitrate

                        streamBusy = true
                        status = ConnectionStatus.Starting
                        formatTimeout = scope.launch {
                            delay(10000)
                            rejectFormat()
                        }

                    }

                }

        }

        onDispose {
            peer?.setPacketListener(null)
            formatTimeout?.cancel()
            formatRequest = null
        }

    }

    LaunchedEffect(Unit) {
        if (screen == Screen.Camera && !StreamingService.isActive)
            screen = Screen.Wifi
    }

    LaunchedEffect(screen, discoveryAttempt, networkAllowed) {

        if (screen != Screen.Method || !networkAllowed)
            return@LaunchedEffect

        val generation = ++operationGeneration
        connectJob = launch {

            discovering = true
            discoveredPeer = null
            connecting = false
            connectionError = null
            try {

                // USB discovery will be tried here when its transport is implemented.
                discoveryStatus = "USB discovery is coming later. Searching Wi-Fi..."
                val peers = DesktopDiscovery.find(context)
                if (peers.isEmpty()) {

                    discoveryStatus = "No available desktop found. Open CamDroid on your computer, then scan again."
                    return@launch

                }

                if (!isActive || generation != operationGeneration)
                    return@launch

                discoveredPeer = peers.first()
                val peer = peers.first()
                discoveryStatus = "Found ${peer.name} · ${peer.host}:${peer.port}"

            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                discoveryStatus = error.message ?: "Could not search the local network. Try again or connect manually."
            } finally {

                if (generation == operationGeneration) {

                    discovering = false
                    connecting = false

                }

            }

        }

    }

    LaunchedEffect(connection) {

        val currentConnection = connection ?: return@LaunchedEffect
        currentConnection.failure.collect { error ->
            if (error != null && connection === currentConnection) {

                streaming = false
                streamBusy = false
                status = ConnectionStatus.Error
                connectionError = "Desktop connection lost. Reconnect to continue."
                onDisconnect()
                connection = null

            }

        }

    }

    val back = {

        operationGeneration++
        connectJob?.cancel()
        connectJob = null
        streaming = false
        onDisconnect()

        connection = null
        connecting = false
        settings = false
        screen = if (screen == Screen.Camera) Screen.Wifi else Screen.Method

        connectionError = null
        cameraReady = false
        streamBusy = false

    }

    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val currentDisconnect by rememberUpdatedState(onDisconnect)
    DisposableEffect(lifecycle) {

        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP && !StreamingService.isActive) {

                operationGeneration++
                connectJob?.cancel()
                connectJob = null
                streaming = false
                streamBusy = false

                connecting = false
                discovering = false
                currentDisconnect()
                connection = null
                cameraReady = false

                settings = false
                status = ConnectionStatus.Disconnected
                if (screen == Screen.Camera)
                    screen = Screen.Wifi

            }

        }

        lifecycle.addObserver(observer)
        onDispose {

            lifecycle.removeObserver(observer)
            connectJob?.cancel()
            if (!StreamingService.isActive)
                currentDisconnect()

        }

    }

    BackHandler(screen != Screen.Method && !settings) { back() }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {

        AnimatedContent(
            targetState = screen,
            transitionSpec = {
                (fadeIn(tween(180)) + slideInHorizontally { it / 12 }) togetherWith fadeOut(tween(120))
            },

            label = "Screen transition"
        ) { current ->
            when (current) {

                Screen.Method ->
                    ConnectionMethodScreen(
                        discoveryStatus = discoveryStatus,
                        discovering = discovering,
                        peer = discoveredPeer,

                        connecting = connecting,
                        onPeerConnect = {

                            val peer = discoveredPeer
                            if (peer != null && !connecting && !discovering) {

                                val generation = ++operationGeneration
                                connectJob = scope.launch {

                                    connecting = true
                                    discoveryStatus = "Connecting to ${peer.name}..."
                                    try {

                                        val candidate = onConnect(peer.host, peer.port, peer.networkHandle)
                                        if (!isActive || generation != operationGeneration) {

                                            candidate.close()
                                            return@launch

                                        }

                                        connection = candidate
                                        desktopName = peer.name
                                        selectedNetwork = peer.networkHandle
                                        ip = peer.host
                                        port = peer.port.toString()

                                        DesktopDiscovery.remember(context, peer)
                                        status = ConnectionStatus.Connected
                                        screen = Screen.Camera

                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (_: Exception) {
                                        discoveryStatus = "Could not connect to ${peer.name}. Retry or scan again."
                                    } finally {

                                        if (generation == operationGeneration)
                                            connecting = false

                                    }

                                }

                            }

                        },

                        onRetry = {
                            if (!networkAllowed)
                                permission.launch(networkPermission) else discoveryAttempt++
                        },

                        onWifi = {

                            operationGeneration++
                            connectJob?.cancel()
                            connectJob = null
                            onDisconnect()
                            discovering = false

                            connecting = false
                            selectedNetwork = 0L
                            desktopName = null
                            screen = Screen.Wifi
                            if (!networkAllowed)
                                permission.launch(networkPermission)

                        }
                    )
                Screen.Wifi ->
                    WifiConfigurationScreen(
                        ip,
                        port,

                        {

                            ip = it
                            connectionError = null

                        },

                        {

                            port = it
                            connectionError = null

                        },

                        back,
                        connecting = connecting,
                        connectionError = connectionError,
                        onConnect = {

                            if (!connecting)
                                connectJob = scope.launch {

                                    connecting = true
                                    connectionError = null
                                    try {

                                        connection = onConnect(ip, port.toInt(), selectedNetwork)
                                        status = ConnectionStatus.Connected
                                        screen = Screen.Camera

                                    } catch (cancelled: CancellationException) {
                                        throw cancelled
                                    } catch (_: Exception) {
                                        connectionError =
                                            "Could not connect. Check the address and port, " +
                                                "and make sure your desktop is listening."
                                    } finally {

                                        connecting = false

                                    }

                                }

                        }
                    )
                Screen.Camera ->
                    CameraScreen(
                        endpoint = desktopName?.let { "$it · $ip:$port" } ?: "$ip:$port",

                        status = status,
                        front = front,
                        resolution = resolution,
                        fps = if (requestedFps == 0) "Auto" else requestedFps.toString(),

                        streamingPreview = streaming,
                        supportedFpsLabel = tuningInfo?.fpsLabel,
                        bitrateLabel = tuningInfo?.let { formatBitrate(it.bitrate) },
                        onDisconnect = back,
                        busy = streamBusy,

                        cameraReady = cameraReady,
                        message = connectionError,
                        onBack = back,
                        onSettings = { settings = true },
                        onFlip = { if (!streamBusy) front = !front },
                        onStream = {

                            if ((!streamBusy || streaming) && (cameraReady || streaming)) {

                                if (streaming) {

                                    streamBusy = true
                                    status = ConnectionStatus.Stopping
                                    streaming = false

                                } else if (

                                    Build.VERSION.SDK_INT >= 33 &&
                                        context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) !=
                                            PackageManager.PERMISSION_GRANTED
                                ) {
                                    notificationPermission.launch(android.Manifest.permission.POST_NOTIFICATIONS)
                                } else

                                    connectJob = scope.launch {

                                        streamBusy = true
                                        connectionError = null
                                        try {

                                            if (connection == null) {

                                                status = ConnectionStatus.Connecting
                                                connection = onConnect(ip, port.toInt(), selectedNetwork)

                                            }

                                            status = ConnectionStatus.Starting
                                            streaming = true

                                        } catch (cancelled: CancellationException) {
                                            throw cancelled
                                        } catch (_: Exception) {
                                            connectionError = "Could not connect to your desktop. Try again."
                                            status = ConnectionStatus.Error
                                            streamBusy = false
                                        }

                                    }

                            }

                        },

                        preview = {

                            LiveCameraPreview(
                                front,
                                streaming,
                                connection,
                                resolution,

                                onReady = { cameraReady = it },
                                onStreaming = {

                                    if (streaming) {

                                        status = ConnectionStatus.Streaming
                                        streamBusy = false
                                        acceptFormat()

                                    }

                                },

                                onStopped = { stoppedConnection, preserved ->
                                    if (screen == Screen.Camera && connection === stoppedConnection) {

                                        streaming = false
                                        streamBusy = false
                                        if (!preserved) {

                                            connection = null
                                            onDisconnect()

                                        }

                                        if (status != ConnectionStatus.Error)
                                            status =

                                                if (preserved)
                                                    ConnectionStatus.Connected else ConnectionStatus.Disconnected

                                    }

                                },

                                onError = {

                                    if (formatRequest != null && connection?.isConnected == true)
                                        rejectFormat()
                                    else {

                                        connectionError = it
                                        status = ConnectionStatus.Error
                                        streamBusy = streaming
                                        streaming = false
                                        if (connection?.isConnected != true) {

                                            onDisconnect()
                                            connection = null

                                        }

                                    }

                                },

                                onResolutions = { values ->
                                    availableResolutions = values
                                    if (values.isNotEmpty() && resolution !in values)
                                        resolution = values.first()
                                },

                                requestedFps = requestedFps,
                                requestedBitrate = requestedBitrate,
                                userFormatSelected = userFormatSelected,

                                onTuningApplied = {
                                    tuningInfo = it

                                    if (formatRequest != null && !requestNeedsRestart)
                                        acceptFormat()
                                },

                                onTuningError = { connectionError = it },
                                onPreparing = {

                                    if (streaming) {

                                        streamBusy = true
                                        status = ConnectionStatus.Starting

                                    }

                                }
                            )

                        }
                    )

            }

        }

        if (settings)
            ConfigurationSheet(
                resolution,
                requestedFps.toString(),
                "H.264",
                front,

                {

                    userFormatSelected = true
                    resolution = it

                },

                {

                    userFormatSelected = true
                    requestedFps = it.toInt()
                    connectionError = null

                },

                {},
                { front = it },
                { settings = false },
                resolutions = availableResolutions,
                editable = !streamBusy,

                supportedRates = captureCapabilities?.rates,
                encoderLimits = captureCapabilities?.encoder,
                bitrate = requestedBitrate,

                onBitrate = {
                    requestedBitrate = it
                    connectionError = null
                },

                tuningInfo = tuningInfo,
                tuningEnabled = !streamBusy
            )

    }

}

@Composable
private fun PageHeader(step: String, onBack: (() -> Unit)? = null) {

    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {

        if (onBack != null) {

            IconButton(onClick = onBack) { LineIcon("back", "Go back") }
            Spacer(Modifier.width(8.dp))

        } else {

            Image(
                painterResource(R.drawable.camdroid_logo),
                contentDescription = "CamDroid logo",
                modifier = Modifier.size(48.dp).clip(RoundedCornerShape(14.dp))
            )

            Spacer(Modifier.width(12.dp))

        }

        Text("CamDroid", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold)
        Spacer(Modifier.weight(1f))
        Text(step, color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.labelMedium)

    }

}

@Composable
fun ConnectionMethodScreen(
    discoveryStatus: String = "Find your desktop automatically over Wi-Fi.",
    discovering: Boolean = false,
    peer: DesktopPeer? = null,

    connecting: Boolean = false,
    onPeerConnect: () -> Unit = {},
    onRetry: () -> Unit = {},
    onWifi: () -> Unit
) {

    Column(
        Modifier.fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 20.dp),

        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {

        PageHeader("01 / 02")
        Spacer(Modifier.height(24.dp))
        Eyebrow("PHONE → DESKTOP")
        OutlinedCard(
            Modifier.fillMaxWidth(),

            shape = RoundedCornerShape(22.dp),
            colors = CardDefaults.outlinedCardColors(containerColor = MaterialTheme.colorScheme.surface),
            border = androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = .55f))
        ) {

            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {

                    if (discovering || connecting)
                        CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.dp)
                    else LineIcon("wifi", "Find desktop")
                    Text("Find your desktop", style = MaterialTheme.typography.titleLarge)

                }

                Text(
                    discoveryStatus,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

                if (peer != null)
                    Button(onClick = onPeerConnect, enabled = !connecting && !discovering) {

                        Text(
                            if (connecting)
                                "Connecting..." else "Connect to ${peer.name}",

                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )

                    }

                if (!discovering)
                    OutlinedButton(onClick = onRetry, enabled = !connecting) {
                        Text(if (peer == null) "Find desktop" else "Scan again")
                    }

                Text(
                    "Finds the best available receiver. Tap Connect when you're ready. USB support will be added later.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

            }

        }

        ConnectionMethodCard("Manual Wi-Fi", "Enter your desktop's IP address and port", "wifi", true, onWifi)
        ConnectionMethodCard("USB", "A direct, wired connection", "usb", false) {}
        Spacer(Modifier.weight(1f))
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

        Text(
            "OPEN SOURCE · PHONE TO DESKTOP",
            fontFamily = FontFamily.Monospace,
            fontSize = 10.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,

            modifier = Modifier.padding(vertical = 8.dp)
        )

    }

}

@Composable
fun ConnectionMethodCard(title: String, detail: String, icon: String, enabled: Boolean, onClick: () -> Unit) {

    val interaction = remember { MutableInteractionSource() }
    var appeared by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { appeared = true }
    val entrance by animateFloatAsState(if (appeared) 1f else 0f, tween(220), label = "Card entrance")
    val pressed by interaction.collectIsPressedAsState()

    val scale by animateFloatAsState(if (pressed) .98f else 1f, label = "Card press")
    OutlinedCard(
        onClick = onClick,
        enabled = enabled,
        interactionSource = interaction,

        modifier =
            Modifier.fillMaxWidth().scale(scale).graphicsLayer {
                alpha = entrance
                translationY = (1f - entrance) * 12.dp.toPx()
            },

        shape = RoundedCornerShape(22.dp),
        colors =
            CardDefaults.outlinedCardColors(
                containerColor = MaterialTheme.colorScheme.surface,
                disabledContainerColor = MaterialTheme.colorScheme.surface.copy(alpha = .5f)

            ),
        border =
            androidx.compose.foundation.BorderStroke(
                1.dp,
                if (enabled)

                    MaterialTheme.colorScheme.primary.copy(alpha = .55f)
                else MaterialTheme.colorScheme.outlineVariant
            )
    ) {

        Row(Modifier.padding(20.dp), verticalAlignment = Alignment.CenterVertically) {

            LineIcon(
                icon,
                null,
                if (enabled)
                    MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant

            )

            Spacer(Modifier.width(18.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {

                Text(title, style = MaterialTheme.typography.titleMedium)
                Text(
                    detail,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

                if (!enabled)
                    Text(
                        "Coming soon",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )

            }

            if (enabled)
                LineIcon("next", null, MaterialTheme.colorScheme.primary)

        }

    }

}

@Composable
fun WifiConfigurationScreen(
    ip: String,
    port: String,
    onIp: (String) -> Unit,

    onPort: (String) -> Unit,
    onBack: () -> Unit,
    onConnect: () -> Unit,
    connecting: Boolean = false,
    connectionError: String? = null
) {

    var submitted by rememberSaveable { mutableStateOf(false) }
    val validIp = isValidDesktopIp(ip)
    val validPort = port.toIntOrNull()?.let { it in 1..65535 } == true
    val focus = LocalFocusManager.current
    val submit = {

        submitted = true
        if (!connecting && validIp && validPort) {

            focus.clearFocus()
            onConnect()

        }

    }

    Column(
        Modifier.fillMaxSize().safeDrawingPadding().imePadding().verticalScroll(rememberScrollState()).padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp)
    ) {

        PageHeader("02 / 02", onBack)
        Spacer(Modifier.height(16.dp))
        Eyebrow("WI-FI SETUP")
        Text("Connect to desktop.", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.SemiBold)

        Text(
            "Enter the address shown on your desktop. Keep both devices on the same network.",
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        OutlinedTextField(
            ip,
            onIp,
            Modifier.fillMaxWidth(),
            label = { Text("Desktop IP address") },

            placeholder = { Text("192.168.1.10", fontFamily = FontFamily.Monospace) },
            singleLine = true,
            shape = RoundedCornerShape(16.dp),
            enabled = !connecting,
            isError = submitted && !validIp,
            supportingText = {

                Text(
                    if (submitted && !validIp)
                        "Enter an IPv4 address, such as 192.168.1.10."
                    else "Find this in the CamDroid desktop app."
                )

            },

            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal, imeAction = ImeAction.Next)
        )
        OutlinedTextField(
            port,
            onPort,

            Modifier.fillMaxWidth(),
            label = { Text("Port") },
            placeholder = { Text("6969") },
            singleLine = true,
            enabled = !connecting,

            shape = RoundedCornerShape(16.dp),
            isError = submitted && !validPort,
            supportingText = {
                Text(if (submitted && !validPort) "Use a port from 1 to 65535." else "Use the same port as your desktop.")
            },

            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { submit() })
        )
        Spacer(Modifier.weight(1f))
        Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceVariant) {

            Text(
                connectionError ?: "Connect to the CamDroid receiver running on your desktop.",
                Modifier.padding(16.dp),
                style = MaterialTheme.typography.bodySmall,

                color =
                    if (connectionError != null)
                        MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant
            )

        }

        Button(
            onClick = { submit() },
            enabled = !connecting,
            modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp),
            shape = RoundedCornerShape(16.dp)
        ) {

            if (connecting)
                CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)

            Spacer(Modifier.width(8.dp))
            Text(if (connecting) "Connecting…" else "Connect", fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(12.dp))
            LineIcon("next", null, MaterialTheme.colorScheme.onPrimary)

        }

    }

}

fun isValidDesktopIp(value: String): Boolean {

    val parts = value.split('.')
    return parts.size == 4 &&
        parts.all { part ->
            part.isNotEmpty() &&
                part.length <= 3 &&
                part.all { it in '0'..'9' } &&
                part.toIntOrNull()?.let { it in 0..255 } == true

        }

}

@Composable
fun CameraScreen(
    endpoint: String,
    status: ConnectionStatus,
    front: Boolean,

    resolution: String,
    fps: String,
    onBack: () -> Unit,
    onSettings: () -> Unit,
    onFlip: () -> Unit,

    onStream: () -> Unit,
    streamingPreview: Boolean = status == ConnectionStatus.Streaming,
    busy: Boolean = false,
    cameraReady: Boolean = true,
    message: String? = null,

    onDisconnect: () -> Unit = onBack,
    bitrateLabel: String? = null,
    supportedFpsLabel: String? = null,
    preview: @Composable BoxScope.() -> Unit = { PreviewPlaceholder() }

) {

    BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding()) {

        val landscape = maxWidth > maxHeight
        @Composable
        fun Viewfinder(modifier: Modifier) {

            Box(modifier.clip(RoundedCornerShape(24.dp)).background(Color(0xFF080B08))) {

                preview()
                Row(Modifier.align(Alignment.TopStart).padding(16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ConnectionStatusIndicator(status)
                }

                Text(
                    if (front)
                        "FRONT CAMERA" else "REAR CAMERA",

                    Modifier.align(Alignment.BottomStart).padding(20.dp),
                    color = Color(0xFFBCC5B3),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 10.sp
                )

            }

        }

        @Composable
        fun Controls(modifier: Modifier) {

            Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {

                Eyebrow("WI-FI · DESKTOP")
                Row(verticalAlignment = Alignment.CenterVertically) {

                    Text(
                        endpoint,
                        Modifier.weight(1f),
                        fontFamily = FontFamily.Monospace,
                        style = MaterialTheme.typography.titleMedium,

                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    TextButton(onClick = onDisconnect) { Text("Disconnect") }

                }

                val fpsReadout = if (fps == "Auto") "Auto FPS" else "$fps FPS"

                val supportedReadout = supportedFpsLabel?.takeIf { it != fps }?.let { " · $it FPS supported" } ?: ""
                Text(
                    "$resolution / $fpsReadout$supportedReadout" + (bitrateLabel?.let { " · $it" } ?: ""),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodySmall
                )

                if (message != null)
                    Text(message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)

                CameraControls(
                    streamingPreview,
                    onStream,
                    onFlip,
                    onSettings,

                    busy,
                    cameraReady,
                    status == ConnectionStatus.Stopping
                )

            }

        }

        Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {

            Row(verticalAlignment = Alignment.CenterVertically) {

                IconButton(onClick = onBack) { LineIcon("back", "Disconnect and go back") }
                Text("Viewfinder", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                IconButton(onClick = onSettings) { LineIcon("settings", "Open configuration") }

            }

            if (landscape)
                Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    Viewfinder(Modifier.weight(1f).fillMaxHeight())
                    Controls(Modifier.width(260.dp).fillMaxHeight().verticalScroll(rememberScrollState()))
                }

            else {

                Viewfinder(Modifier.fillMaxWidth().weight(1f))
                Controls(Modifier.fillMaxWidth())

            }

        }

    }

}

@Composable
private fun PreviewPlaceholder() {

    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {

        Canvas(Modifier.fillMaxSize()) {

            val grid = Color(0xFFB8F568).copy(alpha = .07f)
            for (i in 1..2) {

                drawLine(grid, Offset(size.width * i / 3, 0f), Offset(size.width * i / 3, size.height))
                drawLine(grid, Offset(0f, size.height * i / 3), Offset(size.width, size.height * i / 3))

            }

        }

        Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(12.dp)) {

            Image(
                painterResource(R.drawable.camdroid_logo),
                contentDescription = "CamDroid logo",
                modifier = Modifier.size(64.dp).clip(RoundedCornerShape(12.dp))
            )

            Text("Preview area", color = Color(0xFFF0F3E9), style = MaterialTheme.typography.titleMedium)
            Text("Camera feed will appear here", color = Color(0xFF8B9683), style = MaterialTheme.typography.bodySmall)

        }

    }

}

@Composable
fun ConnectionStatusIndicator(status: ConnectionStatus) {

    val color by
        animateColorAsState(
            when (status) {

                ConnectionStatus.Error,
                ConnectionStatus.Disconnected -> MaterialTheme.colorScheme.error
                ConnectionStatus.Connecting,
                ConnectionStatus.Starting,
                ConnectionStatus.Stopping -> Color(0xFFFFD88A)
                else -> MaterialTheme.colorScheme.primary

            },

            label = "Status color"
        )
    Surface(color = Color.Black.copy(alpha = .7f), shape = CircleShape) {

        Row(
            Modifier.padding(horizontal = 12.dp, vertical = 7.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp)
        ) {

            if (status in listOf(ConnectionStatus.Connecting, ConnectionStatus.Starting, ConnectionStatus.Stopping))
                CircularProgressIndicator(Modifier.size(10.dp), color = color, strokeWidth = 1.dp)
            else Box(Modifier.size(6.dp).background(color, CircleShape))
            AnimatedContent(status.label, transitionSpec = { fadeIn() togetherWith fadeOut() }, label = "Status label") {
                Text(it, color = color, style = MaterialTheme.typography.labelMedium)
            }

        }

    }

}

@Composable
fun CameraControls(
    streaming: Boolean,
    onStream: () -> Unit,
    onFlip: () -> Unit,

    onSettings: () -> Unit,
    busy: Boolean = false,
    cameraReady: Boolean = true,
    stopping: Boolean = false
) {

    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {

        OutlinedIconButton(onClick = onFlip, enabled = !busy, modifier = Modifier.size(52.dp)) {
            LineIcon("flip", "Switch camera selection")
        }

        Button(
            onClick = onStream,
            enabled = if (streaming) !stopping else !busy && cameraReady,

            modifier = Modifier.weight(1f).heightIn(min = 56.dp),
            shape = RoundedCornerShape(16.dp),
            colors =
                ButtonDefaults.buttonColors(
                    containerColor = if (streaming) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary

                )
        ) {

            AnimatedContent(
                when {

                    busy && stopping -> "Stopping…"
                    streaming -> "Stop streaming"
                    busy -> "Starting…"
                    else -> "Start streaming"

                },

                label = "Stream button"
            ) {
                Text(it)
            }

        }

        OutlinedIconButton(onClick = onSettings, modifier = Modifier.size(52.dp)) {
            LineIcon("settings", "Open configuration")
        }

    }

}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConfigurationSheet(
    resolution: String,
    fps: String,

    encoding: String,
    front: Boolean,
    onResolution: (String) -> Unit,
    onFps: (String) -> Unit,
    onEncoding: (String) -> Unit,

    onFront: (Boolean) -> Unit,
    onDismiss: () -> Unit,
    resolutions: List<String> = listOf("1080p"),
    editable: Boolean = true,
    bitrate: Int = 0,

    supportedRates: List<Int>? = null,
    encoderLimits: EncoderLimits? = null,
    onBitrate: (Int) -> Unit = {},
    tuningInfo: StreamTuning? = null,
    tuningEnabled: Boolean = true
) {

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {

        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 24.dp).padding(bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {

            Eyebrow("CAMERA CONFIGURATION")
            Text("Video settings", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
            SettingOptions("Resolution", resolutions, resolution, onResolution, editable)
            val rates =
                (listOf(15, 24, 25, 30, 48, 50, 60, 90, 120, 144, 165, 180, 200, 240) + supportedRates.orEmpty())

                    .distinct()
                    .sorted()
            SettingOptions(
                "Frame rate",
                listOf("Auto") + rates.map { "$it FPS" },

                if (fps == "0")
                    "Auto" else "$fps FPS",

                { onFps(if (it == "Auto") "0" else it.substringBefore(" ")) },
                tuningEnabled,
                available = { option ->
                    supportedRates?.let { values ->
                        if (option == "Auto")
                            values.isNotEmpty() else option.substringBefore(" ").toInt() in values
                    } == true

                }
            )
            Text(
                if (supportedRates == null)
                    "Checking camera and encoder capabilities…"

                else if (supportedRates.isEmpty()) "No compatible frame rates reported for this camera and resolution."
                else "Dimmed frame rates are unavailable for this camera, resolution and AVC encoder.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            val presets =
                linkedMapOf(
                    "Adaptive" to 0,
                    "Low · 5 Mbps" to 5_000_000,
                    "Medium · 12 Mbps" to 12_000_000,

                    "High · 18 Mbps" to 18_000_000,
                    "Extreme · 28 Mbps" to 28_000_000,
                    "Ig bro · 40 Mbps" to 40_000_000
                )

            SettingOptions(
                "Bitrate",
                presets.keys.toList(),
                presets.entries.firstOrNull { it.value == bitrate }?.key.orEmpty(),
                { onBitrate(presets.getValue(it)) },

                tuningEnabled,
                available = { option ->
                    encoderLimits?.let { limits ->
                        val bits = presets.getValue(option)
                        if (bits == 0)

                            maxOf(2_000_000, limits.minBitrate) <= minOf(40_000_000, limits.maxBitrate)
                        else bits in limits.minBitrate..limits.maxBitrate

                    } == true

                }
            )
            Text(
                if (bitrate == 0)
                    "Adaptive adjusts between 2–40 Mbps using desktop delivery feedback, within encoder limits."

                else if (bitrate !in presets.values) "Custom desktop target: ${formatBitrate(bitrate)}"
                else "Fixed target: ${formatBitrate(bitrate)}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            tuningInfo?.let {

                Text(
                    "Supported target: ${it.fpsLabel} FPS · encoder ${it.encoderFps} FPS · ${formatBitrate(it.bitrate)}",
                    style = MaterialTheme.typography.bodySmall
                )

                Text(
                    "Encoder bitrate limits: ${formatBitrate(it.encoder.minBitrate)}–${formatBitrate(it.encoder.maxBitrate)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

            }

            SettingOptions("Video encoding", listOf("H.264"), encoding, onEncoding, false)
            SettingOptions(
                "Camera",
                listOf("Rear", "Front"),
                if (front)
                    "Front" else "Rear",

                { onFront(it == "Front") },
                editable
            )

            Text(
                "Higher FPS, bit rate or resolution or any combination of them increases lag. " +
                    "Bitrate targets are limited by the encoder. Camera, resolution and FPS changes " +
                    "restart capture internally while keeping the desktop connection.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Button(onClick = onDismiss, modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp)) {
                Text("Done")
            }

        }

    }

    // TODO: Enable additional encoding formats when supported by the backend.

}

private fun formatBitrate(bitsPerSecond: Int): String =
    if (bitsPerSecond < 1_000_000)
        "${bitsPerSecond / 1000} kbps"
    else String.format(java.util.Locale.ROOT, "%.1f Mbps", bitsPerSecond / 1_000_000f)

@Composable
private fun SettingOptions(
    label: String,
    options: List<String>,
    selected: String,

    onSelect: (String) -> Unit,
    enabled: Boolean = true,
    available: (String) -> Boolean = { true }

) {

    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {

        Text(label, style = MaterialTheme.typography.labelLarge)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {

            options.forEach { option ->
                FilterChip(
                    selected == option,
                    { onSelect(option) },
                    label = { Text(option) },

                    enabled = enabled && available(option)
                )

            }

        }

    }

}

@Composable
private fun Eyebrow(text: String) {

    Text(
        text,
        color = MaterialTheme.colorScheme.primary,
        fontSize = 10.sp,
        fontFamily = FontFamily.Monospace,

        letterSpacing = 1.5.sp,
        fontWeight = FontWeight.Medium
    )

}

@Composable
private fun LineIcon(
    kind: String,
    description: String?,
    color: Color = MaterialTheme.colorScheme.onSurface,

    modifier: Modifier = Modifier.size(24.dp)
) {

    val semanticsModifier =
        if (description == null)
            modifier else modifier.then(Modifier.semantics { contentDescription = description })

    Canvas(semanticsModifier) {

        val s = size.width / 24f
        fun line(x1: Float, y1: Float, x2: Float, y2: Float) =
            drawLine(color, Offset(x1 * s, y1 * s), Offset(x2 * s, y2 * s), 1.7f * s, StrokeCap.Round)
        when (kind) {

            "back",
            "next" -> {

                val d = if (kind == "back") 1f else -1f

                line(12f - 7 * d, 12f, 12f + 7 * d, 12f)
                line(12f - 7 * d, 12f, 12f - d, 6f)
                line(12f - 7 * d, 12f, 12f - d, 18f)

            }

            "wifi" -> {

                for (r in listOf(6f, 10f, 14f))
                    drawArc(
                    color,
                    225f,
                    90f,

                    false,
                    Offset((12 - r) * s, (19 - r) * s),
                    androidx.compose.ui.geometry.Size(2 * r * s, 2 * r * s),
                    style = Stroke(1.7f * s, cap = StrokeCap.Round)
                )
                drawCircle(color, 1.5f * s, Offset(12 * s, 19 * s))

            }

            "usb" -> {

                line(12f, 21f, 12f, 3f)
                line(9f, 6f, 12f, 3f)
                line(12f, 3f, 15f, 6f)
                line(12f, 16f, 6f, 12f)
                line(6f, 12f, 6f, 9f)

                line(12f, 13f, 18f, 10f)
                line(18f, 10f, 18f, 7f)

            }

            "settings" -> {

                for (i in 0..2) {

                    val y = 6f + i * 6f
                    line(3f, y, 21f, y)
                    drawCircle(color, 2.5f * s, Offset((if (i == 1) 15f else 8f) * s, y * s), style = Stroke(1.7f * s))

                }

            }

            "flip" -> {

                line(4f, 8f, 19f, 8f)
                line(19f, 8f, 15f, 4f)
                line(19f, 8f, 15f, 12f)
                line(20f, 16f, 5f, 16f)
                line(5f, 16f, 9f, 12f)
                line(5f, 16f, 9f, 20f)

            }

            else -> {

                drawRoundRect(
                    color,
                    Offset(3 * s, 6 * s),
                    androidx.compose.ui.geometry.Size(18 * s, 14 * s),
                    androidx.compose.ui.geometry.CornerRadius(3 * s),

                    style = Stroke(1.7f * s)
                )
                drawCircle(color, 4 * s, Offset(12 * s, 13 * s), style = Stroke(1.7f * s))
                line(8f, 3f, 16f, 3f)

            }

        }

    }

}

@Preview(showBackground = true, widthDp = 393, heightDp = 852)
@Composable
private fun ConnectionPreview() {
    CamDroidTheme(darkTheme = true, dynamicColor = false) { ConnectionMethodScreen {} }
}

@Preview(showBackground = true, widthDp = 852, heightDp = 393)
@Composable
private fun CameraLandscapePreview() {

    CamDroidTheme(darkTheme = true, dynamicColor = false) {
        CameraScreen("192.168.1.10:6969", ConnectionStatus.Ready, false, "1080p", "30", {}, {}, {}, {})
    }

}

@Preview(showBackground = true, widthDp = 393, heightDp = 852)
@Composable
private fun WifiPreview() {

    CamDroidTheme(darkTheme = true, dynamicColor = false) {
        WifiConfigurationScreen("", "6969", {}, {}, {}, {})
    }

}

@Preview(showBackground = true, widthDp = 393, heightDp = 852)
@Composable
private fun CameraPortraitPreview() {

    CamDroidTheme(darkTheme = true, dynamicColor = false) {
        CameraScreen("192.168.1.10:6969", ConnectionStatus.Streaming, true, "1080p", "30", {}, {}, {}, {})
    }

}
