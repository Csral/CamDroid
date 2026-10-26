package csral.camdroid.camera

import android.Manifest
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import androidx.annotation.RequiresPermission
import androidx.compose.runtime.remember
import kotlin.math.roundToInt

class CameraController(context: Context) {

    private var cameraDevice: CameraDevice? = null

    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

    private var previewSurface: Surface? = null

    private var captureSession: CameraCaptureSession? = null

    private var encoderSurface: Surface? = null
    private var cameraGeneration = 0
    private var sessionGeneration = 0
    private var targetFpsRange: Range<Int>? = null
    private var repeatingRequest: CaptureRequest.Builder? = null

    private var highSpeed = false
    var onCaptureError: ((String) -> Unit)? = null

    fun videoRotation(lensFacing: Int, displayRotation: Int): Int {

        val id = findCamera(lensFacing) ?: error("Camera is unavailable.")
        val sensor = cameraManager.getCameraCharacteristics(id).get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val degrees =
            when (displayRotation) {

                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0

            }

        return if (lensFacing == CameraCharacteristics.LENS_FACING_FRONT) (sensor + degrees) % 360
        else (sensor - degrees + 360) % 360

    }

    fun getNormalFpsRanges(lensFacing: Int, size: Size): List<Range<Int>> {

        val id = findCamera(lensFacing) ?: return emptyList()
        val characteristics = cameraManager.getCameraCharacteristics(id)
        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        val durations =
            listOf(
                runCatching { map.getOutputMinFrameDuration(SurfaceTexture::class.java, size) }.getOrDefault(0L),

                runCatching { map.getOutputMinFrameDuration(MediaCodec::class.java, size) }.getOrDefault(0L)
            )
        val duration = durations.maxOrNull() ?: 0L
        val ceiling = if (duration > 0L) (1_000_000_000.0 / duration).roundToInt() else 255

        return characteristics
            .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.filter { it.lower > 0 && it.upper <= ceiling.coerceAtMost(255) }
            .orEmpty()

    }

    fun getSupportedFpsRanges(lensFacing: Int, size: Size): List<Range<Int>> {

        val normal = getNormalFpsRanges(lensFacing, size)
        val id = findCamera(lensFacing) ?: return normal
        val characteristics = cameraManager.getCameraCharacteristics(id)
        val capabilities = (characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf())
        if (CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO !in capabilities)
            return normal

        val map = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return normal
        val fast = runCatching {
            if (size in map.highSpeedVideoSizes)
                map.getHighSpeedVideoFpsRangesFor(size).toList() else emptyList()
        }

            .getOrDefault(emptyList())
        // Preview plus recording requires a fixed range in a high-speed session.
        return (normal + fast.filter { it.lower == it.upper && it.lower > 0 && it.upper <= 240 }).distinct()

    }

    fun previewSize(lensFacing: Int, capture: Size, range: Range<Int>): Size {

        // Constrained high-speed outputs must retain the same recording dimensions.
        if (range !in getNormalFpsRanges(lensFacing, capture))
            return capture

        val id = findCamera(lensFacing) ?: return capture
        val map =
            cameraManager.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?: return capture

        return map.getOutputSizes(SurfaceTexture::class.java)
            ?.filter {

                it.width <= capture.width &&
                    it.height <= capture.height &&
                    it.width <= 1920 &&
                    it.height <= 1080 &&
                    it.width.toLong() * capture.height == it.height.toLong() * capture.width &&
                    map.getOutputMinFrameDuration(SurfaceTexture::class.java, it).let { duration ->
                        duration == 0L || duration <= 1_000_000_000L / range.upper

                    }

            }

            ?.maxByOrNull { it.width.toLong() * it.height } ?: capture

    }

    fun setCaptureMode(lensFacing: Int, size: Size, range: Range<Int>) {
        highSpeed = range !in getNormalFpsRanges(lensFacing, size)
        targetFpsRange = range
    }

    private fun repeat(session: CameraCaptureSession, builder: CaptureRequest.Builder) {
        if (session is CameraConstrainedHighSpeedCaptureSession)
            session.setRepeatingBurst(session.createHighSpeedRequestList(builder.build()), null, null)
        else session.setRepeatingRequest(builder.build(), null, null)
    }

    fun setFrameRateRange(range: Range<Int>) {

        val builder = repeatingRequest
        val previous = targetFpsRange
        targetFpsRange = range
        if (builder != null && captureSession != null) {

            try {

                builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, range)
                captureSession?.let { repeat(it, builder) }

            } catch (error: Exception) {
                targetFpsRange = previous
                builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, previous)
                throw error
            }

        }

    }

    fun listCameras() {

        for (cameraId in cameraManager.cameraIdList) {

            val characteristics = cameraManager.getCameraCharacteristics(cameraId)

            val facing = characteristics.get(CameraCharacteristics.LENS_FACING)

            Log.d("CamDroid", "CameraID = $cameraId -> facing $facing")

        }

    }

    fun getSupportedResolutions(lensFacing: Int): List<Size> {

        val cameraID = findCamera(lensFacing) ?: return emptyList()
        val characteristics = cameraManager.getCameraCharacteristics(cameraID)

        val configMap = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()

        return configMap.getOutputSizes(SurfaceTexture::class.java)?.toList() ?: emptyList()

    }

    private fun findCamera(lensFacing: Int): String? {

        for (cameraId in cameraManager.cameraIdList) {

            val characteristics = cameraManager.getCameraCharacteristics(cameraId)

            val facing = characteristics.get(CameraCharacteristics.LENS_FACING)

            // found required camera
            if (facing == lensFacing)
                return cameraId

        }

        // no camera found
        return null

    }

    @RequiresPermission(Manifest.permission.CAMERA)
    fun openCamera(lensFacing: Int) {

        closeCamera()
        val cameraId = findCamera(lensFacing) ?: return
        val generation = cameraGeneration

        cameraManager.openCamera(
            cameraId,
            object : CameraDevice.StateCallback() {

                override fun onOpened(camera: CameraDevice) {

                    if (generation != cameraGeneration) {

                        runCatching { camera.close() }

                        return

                    }

                    cameraDevice = camera
                    try {

                        tryConfigure()

                    } catch (error: Exception) {
                        closeCamera()
                        onCaptureError?.invoke(error.message ?: "Could not configure the camera.")
                    }

                }

                override fun onDisconnected(camera: CameraDevice) {
                    failCamera(camera, generation, "Camera disconnected.")
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    failCamera(camera, generation, "Camera failed ($error).")
                }

            },

            null
        )

    }

    @RequiresPermission(Manifest.permission.CAMERA)
    fun switchCamera(lensFacing: Int) {
        openCamera(lensFacing)
    }

    fun closeCamera() {

        cameraGeneration++
        sessionGeneration++
        repeatingRequest = null
        val session = captureSession
        val camera = cameraDevice

        captureSession = null
        cameraDevice = null
        runCatching { session?.stopRepeating() }
        runCatching { session?.close() }.onFailure { Log.w("CamDroid", "Could not close capture session", it) }
        runCatching { camera?.close() }.onFailure { Log.w("CamDroid", "Could not close camera", it) }

    }

    private fun failCamera(camera: CameraDevice, generation: Int, message: String) {

        if (generation != cameraGeneration) {

            runCatching { camera.close() }

            return

        }

        closeCamera()
        runCatching { camera.close() }
        onCaptureError?.invoke(message)

    }

    fun clearPreviewSurface() {
        closeCamera()
        previewSurface = null
    }

    fun setPreviewSurface(surface: Surface?) {

        if (previewSurface === surface)
            return

        previewSurface = surface
        reconfigure()

    }

    fun setEncoderSurface(surface: Surface?) {
        encoderSurface = surface
        reconfigure()
    }

    private fun retryWithoutPreview(): Boolean {

        if (previewSurface == null || encoderSurface == null)
            return false

        Log.w("CamDroid", "Preview configuration rejected; retrying encoder-only capture")
        previewSurface = null
        reconfigure()

        return true

    }

    fun tryConfigure() {

        val camera = cameraDevice ?: return
        val surface = previewSurface
        val encoder = encoderSurface

        if (surface == null && encoder == null)
            return

        val generation = ++sessionGeneration

        Log.d("CamDroid", "Ready to go.")

        val requestBuilder =
            camera.createCaptureRequest(
                if (encoderSurface == null)
                    CameraDevice.TEMPLATE_PREVIEW else CameraDevice.TEMPLATE_RECORD

            )

        val characteristics = cameraManager.getCameraCharacteristics(camera.id)
        val antibanding = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_ANTIBANDING_MODES)

        if (antibanding?.contains(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_AUTO) == true)
            requestBuilder.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE, CaptureRequest.CONTROL_AE_ANTIBANDING_MODE_AUTO)

        val outputs = mutableListOf<Surface>()

        if (surface != null) {

            outputs.add(surface)
            requestBuilder.addTarget(surface)

        }

        if (encoder != null) {

            requestBuilder.addTarget(encoder)
            outputs.add(encoder)

        }

        val callback =
            object : CameraCaptureSession.StateCallback() {

                override fun onConfigured(session: CameraCaptureSession) {

                    if (
                        generation != sessionGeneration ||
                            cameraDevice !== camera ||
                            encoderSurface !== encoder ||
                            previewSurface !== surface
                    ) {

                        runCatching { session.close() }

                        return
                    }

                    captureSession = session
                    repeatingRequest = requestBuilder

                    Log.d("CamDroid", "Capture session configured")

                    try {

                        targetFpsRange?.let { requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                        repeat(session, requestBuilder)

                    } catch (error: Exception) {

                        if (runCatching { retryWithoutPreview() }.getOrDefault(false))
                            return

                        closeCamera()
                        onCaptureError?.invoke(error.message ?: "Camera rejected the capture mode.")

                    }

                }

                override fun onConfigureFailed(session: CameraCaptureSession) {

                    Log.e("CamDroid", "Capture session configuration failed")
                    if (
                        generation == sessionGeneration &&
                            cameraDevice === camera &&
                            encoderSurface === encoder &&
                            previewSurface === surface

                    ) {

                        runCatching { session.close() }
                        if (runCatching { retryWithoutPreview() }.getOrDefault(false))
                            return

                        closeCamera()
                        onCaptureError?.invoke("Camera rejected this resolution/FPS combination.")

                    }

                }

            }

        try {

            if (highSpeed)
                camera.createConstrainedHighSpeedCaptureSession(outputs, callback, null)
            else camera.createCaptureSession(outputs, callback, null)

        } catch (error: Exception) {
            if (!retryWithoutPreview())
                throw error
        }

    }

    fun reconfigure() {

        sessionGeneration++
        repeatingRequest = null
        val session = captureSession
        captureSession = null
        runCatching { session?.stopRepeating() }
        runCatching { session?.close() }

        tryConfigure()

    }

}
