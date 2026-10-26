package csral.camdroid

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.display.DisplayManager
import android.util.Size
import android.view.Surface
import android.view.TextureView
import androidx.annotation.RequiresPermission
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import csral.camdroid.camera.CameraController
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

private data class StreamRequest(
    val streaming: Boolean,
    val connection: TcpStreamer?,
    val ready: Boolean,
    val foreground: Boolean,

    val fps: Int?,
    val size: Size,
    val rotation: Int,
    val front: Boolean,
    val range: android.util.Range<Int>?,

    val preview: Boolean
)

@Composable
@RequiresPermission(android.Manifest.permission.CAMERA)
fun Camera2Preview(
    useFrontCamera: Boolean,
    modifier: Modifier = Modifier,

    streaming: Boolean = false,
    connection: TcpStreamer? = null,
    selectedSize: Size = Size(1920, 1080),
    onReady: (Boolean) -> Unit = {},
    onStreaming: () -> Unit = {},

    onStopped: (TcpStreamer?, Boolean) -> Unit = { _, _ -> },
    onError: (String) -> Unit = {},
    onAvailableResolutions: (List<Size>) -> Unit = {},
    requestedFps: Int = 30,
    requestedBitrate: Int = 4_000_000,

    onTuningApplied: (StreamTuning) -> Unit = {},
    onTuningError: (String) -> Unit = {},
    userFormatSelected: Boolean = false,
    onPreparing: () -> Unit = {},
    previewEnabled: Boolean = false
) {

    val context = LocalContext.current
    val hostView = LocalView.current
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val cameraController = remember { CameraController(context) }
    val serviceState by StreamingService.state.collectAsState()

    val scope = rememberCoroutineScope()
    val currentOnReady by rememberUpdatedState(onReady)
    val currentOnStreaming by rememberUpdatedState(onStreaming)
    val currentOnStopped by rememberUpdatedState(onStopped)
    val currentOnError by rememberUpdatedState(onError)

    val currentOnResolutions by rememberUpdatedState(onAvailableResolutions)
    val currentOnTuning by rememberUpdatedState(onTuningApplied)
    val currentOnTuningError by rememberUpdatedState(onTuningError)
    val currentOnPreparing by rememberUpdatedState(onPreparing)

    DisposableEffect(cameraController) {
        cameraController.onCaptureError = { message -> scope.launch { currentOnError(message) } }
        onDispose { cameraController.onCaptureError = null }
    }

    val currentUserFormat by rememberUpdatedState(userFormatSelected)
    val currentStreaming by rememberUpdatedState(streaming)
    val currentPreviewEnabled by rememberUpdatedState(previewEnabled)
    val currentConnection by rememberUpdatedState(connection)
    val currentSize by rememberUpdatedState(selectedSize)
    val currentFront by rememberUpdatedState(useFrontCamera)

    var previewReady by remember { mutableStateOf(false) }
    var foreground by remember { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) }

    var appliedCameraRange by remember { mutableStateOf<android.util.Range<Int>?>(null) }
    var previewSurface by remember { mutableStateOf<Surface?>(null) }
    val lensFacing = if (useFrontCamera) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

    val supportedSizes =
        remember(useFrontCamera) {
            runCatching { cameraController.getSupportedResolutions(lensFacing) }.getOrDefault(emptyList())
        }

    val availableSizes =
        listOf(Size(3840, 2160), Size(1920, 1080), Size(1280, 720), Size(640, 480)).filter { it in supportedSizes }
    val sizeSupported = selectedSize in availableSizes
    val capabilities =
        remember(useFrontCamera, selectedSize) {

            runCatching {
                VideoEncoder.limits(selectedSize) to cameraController.getSupportedFpsRanges(lensFacing, selectedSize)
            }

        }

    val tuningResult =
        remember(capabilities, requestedFps, requestedBitrate) {

            runCatching {
                val (limits, ranges) = capabilities.getOrThrow()
                selectStreamTuning(requestedFps, requestedBitrate, ranges, limits)
            }

        }

    val tuning = tuningResult.getOrNull()
    val currentTuning by rememberUpdatedState(tuning)
    val currentRequestedFps by rememberUpdatedState(requestedFps)
    val currentRequestedBitrate by rememberUpdatedState(requestedBitrate)
    val textureView = remember {

        TextureView(context).apply {

            surfaceTextureListener =
                object : TextureView.SurfaceTextureListener {

                    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {

                        currentTuning
                            ?.takeUnless { StreamingService.isActive }

                            ?.let { tuning ->
                                val facing =
                                    if (currentFront)
                                        CameraCharacteristics.LENS_FACING_FRONT
                                    else CameraCharacteristics.LENS_FACING_BACK

                                val size = cameraController.previewSize(facing, currentSize, tuning.fpsRange)
                                texture.setDefaultBufferSize(size.width, size.height)

                            }

                        previewSurface = Surface(texture)
                        previewReady = true

                        if (StreamingService.isActive && foreground && currentPreviewEnabled)
                            StreamingService.attachPreview(previewSurface, texture)

                    }

                    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {

                        previewReady = false
                        StreamingService.detachPreview(previewSurface)
                        cameraController.clearPreviewSurface()
                        runCatching { previewSurface?.release() }
                        previewSurface = null
                        return true

                    }

                    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
                        if (StreamingService.isActive && foreground && currentPreviewEnabled)
                            StreamingService.attachPreview(previewSurface, texture)
                    }

                    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit

                }

        }

    }

    var displayRotation by remember { mutableIntStateOf(Surface.ROTATION_0) }

    DisposableEffect(hostView) {

        val manager = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val listener =
            object : DisplayManager.DisplayListener {

                override fun onDisplayAdded(id: Int) = Unit

                override fun onDisplayRemoved(id: Int) = Unit

                override fun onDisplayChanged(id: Int) {
                    hostView.display?.let { displayRotation = it.rotation }
                }

            }

        hostView.post { hostView.display?.let { displayRotation = it.rotation } }
        manager.registerDisplayListener(listener, android.os.Handler(android.os.Looper.getMainLooper()))
        onDispose { manager.unregisterDisplayListener(listener) }

    }

    val rotation =
        remember(lensFacing, displayRotation) {
            cameraController.videoRotation(lensFacing, displayRotation)
        }

    val currentRotation by rememberUpdatedState(rotation)
    DisposableEffect(lifecycle) {

        val observer = LifecycleEventObserver { _, _ ->
            foreground = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)
            if (!foreground) {

                StreamingService.detachPreview(previewSurface)
                cameraController.closeCamera()

            }

        }

        lifecycle.addObserver(observer)
        onDispose {

            lifecycle.removeObserver(observer)
            StreamingService.detachPreview(previewSurface)
            cameraController.clearPreviewSurface()
            runCatching { previewSurface?.release() }

            previewSurface = null
            previewReady = false
            currentOnReady(false)

        }

    }

    LaunchedEffect(availableSizes) { currentOnResolutions(availableSizes) }

    LaunchedEffect(tuningResult) {
        tuningResult.exceptionOrNull()?.let { currentOnTuningError(it.message ?: "Unsupported video settings.") }
    }

    LaunchedEffect(previewEnabled, previewReady, sizeSupported, foreground, tuning, appliedCameraRange) {

        currentOnReady(
            sizeSupported &&
                foreground &&
                tuning != null &&
                appliedCameraRange == tuning.fpsRange &&
                (!previewEnabled || previewReady)

        )

    }

    LaunchedEffect(previewEnabled, previewReady, foreground, serviceState.streaming, serviceState.preparing) {
        if (StreamingService.isActive && previewEnabled && previewReady && foreground)
            StreamingService.attachPreview(previewSurface, textureView.surfaceTexture)
        else StreamingService.detachPreview(previewSurface)
    }

    // One owner closes the previous capture session and codec before changing their surfaces.
    LaunchedEffect(Unit) {

        snapshotFlow {

            StreamRequest(
                currentStreaming,
                currentConnection,
                if (currentStreaming || !currentPreviewEnabled)
                    true else previewReady,

                if (currentStreaming)
                    true else foreground,

                currentTuning?.encoderFps,
                currentSize,
                StreamingService.state.value.request
                    ?.takeIf {
                        currentStreaming && StreamingService.isActive && it.front == currentFront
                    }

                    ?.rotation ?: currentRotation,
                currentFront,
                currentTuning?.fpsRange,
                if (currentStreaming)
                    false else currentPreviewEnabled

            )

        }

            .collectLatest { request ->
                val connection = request.connection
                val target = currentTuning
                if (!request.ready || !request.foreground) {

                    currentOnStopped(connection, connection?.isConnected == true)
                    return@collectLatest

                }

                if (
                    target == null ||
                        request.size !in
                            cameraController.getSupportedResolutions(
                                if (request.front)

                                    CameraCharacteristics.LENS_FACING_FRONT
                                else CameraCharacteristics.LENS_FACING_BACK
                            )
                )
                    return@collectLatest

                try {

                    val facing =
                        if (request.front)
                            CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK

                    val previewSize = cameraController.previewSize(facing, request.size, target.fpsRange)
                    if (!request.streaming && request.preview)
                        textureView.surfaceTexture?.setDefaultBufferSize(previewSize.width, previewSize.height)

                    if (!request.streaming) {

                        StreamingService.stop(context)
                        StreamingService.awaitStopped()
                        if (request.preview) {

                            cameraController.setPreviewSurface(checkNotNull(previewSurface))
                            cameraController.setCaptureMode(facing, request.size, target.fpsRange)
                            cameraController.switchCamera(facing)

                        } else cameraController.closeCamera()

                        appliedCameraRange = target.fpsRange
                        currentOnTuning(target)
                        awaitCancellation()

                    }

                    if (connection == null || !connection.isConnected) {

                        currentOnStopped(connection, false)
                        return@collectLatest

                    }

                    currentOnPreparing()
                    cameraController.closeCamera()
                    StreamingService.start(
                        context,

                        StreamingRequest(
                            connection,
                            request.size,
                            request.front,
                            request.rotation,

                            currentRequestedFps,
                            currentRequestedBitrate,
                            currentUserFormat
                        ),
                        previewSurface.takeIf { foreground && currentPreviewEnabled },

                        textureView.surfaceTexture.takeIf { foreground && currentPreviewEnabled }
                    )
                    awaitCancellation()

                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    currentOnError("Could not configure the camera or encoder. Change video settings and try again.")
                } finally {

                    cameraController.closeCamera()
                    appliedCameraRange = null

                    if (!currentStreaming || currentConnection !== connection) {

                        StreamingService.stop(context, disconnect = currentConnection !== connection)
                        currentOnStopped(connection, connection?.isConnected == true)

                    } else if (!scope.isActive) {
                        StreamingService.detachPreview(previewSurface)
                    }

                }

            }

    }

    LaunchedEffect(serviceState.revision, serviceState.streaming, serviceState.preparing, serviceState.error) {

        if (serviceState.request?.connection !== currentConnection)
            return@LaunchedEffect

        serviceState.tuning?.let {
            appliedCameraRange = it.fpsRange
            currentOnTuning(it)
        }

        if (serviceState.streaming)
            currentOnStreaming()
        else if (serviceState.error != null) currentOnError(checkNotNull(serviceState.error))
        else if (!serviceState.preparing && currentStreaming)
            currentOnStopped(currentConnection, currentConnection?.isConnected == true)

    }

    LaunchedEffect(tuning?.bitrate, requestedFps, requestedBitrate, userFormatSelected) {

        val target = tuning ?: return@LaunchedEffect
        val peer = currentConnection ?: return@LaunchedEffect

        if (!currentStreaming || !StreamingService.isActive)
            return@LaunchedEffect

        try {

            StreamingService.start(
                context,
                StreamingRequest(
                    peer,
                    currentSize,

                    currentFront,
                    currentRotation,
                    requestedFps,
                    requestedBitrate,
                    userFormatSelected

                ),
                previewSurface.takeIf { foreground && currentPreviewEnabled },
                textureView.surfaceTexture.takeIf { foreground && currentPreviewEnabled }
            )

        } catch (error: Exception) {
            currentOnTuningError(error.message ?: "Could not update stream settings.")
        }

    }

    if (previewEnabled)
        AndroidView(modifier = modifier, factory = { textureView })

}
