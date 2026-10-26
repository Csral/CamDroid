package csral.camdroid

import android.Manifest
import android.content.Context
import android.hardware.camera2.*
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.util.Size
import android.view.Surface
import androidx.test.core.app.ActivityScenario
import androidx.test.platform.app.InstrumentationRegistry
import csral.camdroid.camera.CameraController
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

class HighSpeedCameraTest {

    @Test
    fun highSpeedEncoderProducesMoreThan30Fps() {

        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val context = instrumentation.targetContext
        val thread = HandlerThread("CamDroid-high-speed-test")
        var cleanupHandler: Handler? = null
        val device = AtomicReference<CameraDevice?>(null)

        val session = AtomicReference<CameraCaptureSession?>(null)
        var codec: MediaCodec? = null
        var surface: Surface? = null
        var activity: ActivityScenario<MainActivity>? = null
        val closing = AtomicBoolean(false)
        try {

            thread.start()
            val handler = Handler(thread.looper)
            cleanupHandler = handler
            instrumentation.uiAutomation.grantRuntimePermission(context.packageName, Manifest.permission.CAMERA)
            activity = ActivityScenario.launch(MainActivity::class.java)

            instrumentation.waitForIdleSync()
            val size = Size(1280, 720)
            val controller = CameraController(context)
            val target =
                selectStreamTuning(
                    60,

                    2000000,
                    controller.getSupportedFpsRanges(CameraCharacteristics.LENS_FACING_BACK, size),
                    VideoEncoder.limits(size)
                )

            assumeTrue(
                target.encoderFps == 60 &&
                    target.fpsRange !in controller.getNormalFpsRanges(CameraCharacteristics.LENS_FACING_BACK, size)
            )
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

            val id =
                manager.cameraIdList.first {
                    manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) ==
                        CameraCharacteristics.LENS_FACING_BACK
                }

            val encoder = MediaCodec.createByCodecName(target.encoder.codecName)
            codec = encoder
            val format =
                MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {

                    setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                    setInteger(MediaFormat.KEY_FRAME_RATE, 60)
                    setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, 60f)
                    setInteger(MediaFormat.KEY_BIT_RATE, target.bitrate)
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)

                }

            encoder.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            surface = encoder.createInputSurface()
            encoder.start()
            val input = checkNotNull(surface)
            val ready = CountDownLatch(1)

            val failure = AtomicReference<String?>(null)
            manager.openCamera(
                id,
                object : CameraDevice.StateCallback() {

                    override fun onOpened(camera: CameraDevice) {

                        if (closing.get()) {

                            camera.close()
                            return

                        }

                        device.set(camera)
                        try {

                            camera.createConstrainedHighSpeedCaptureSession(
                                listOf(input),
                                object : CameraCaptureSession.StateCallback() {

                                    override fun onConfigured(capture: CameraCaptureSession) {

                                        if (closing.get()) {

                                            capture.close()
                                            camera.close()
                                            return

                                        }

                                        session.set(capture)
                                        try {

                                            val request =
                                                camera
                                                    .createCaptureRequest(CameraDevice.TEMPLATE_RECORD)
                                                    .apply {
                                                        addTarget(input)
                                                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, target.fpsRange)
                                                    }

                                                    .build()
                                            val highSpeed = capture as CameraConstrainedHighSpeedCaptureSession
                                            highSpeed.setRepeatingBurst(
                                                highSpeed.createHighSpeedRequestList(request), null, handler
                                            )

                                        } catch (error: Exception) {
                                            failure.set(error.toString())
                                        }

                                        ready.countDown()

                                    }

                                    override fun onConfigureFailed(capture: CameraCaptureSession) {
                                        failure.set("High-speed configuration failed")
                                        ready.countDown()
                                    }

                                },

                                handler
                            )

                        } catch (error: Exception) {
                            failure.set(error.toString())
                            runCatching { camera.close() }
                            ready.countDown()
                        }

                    }

                    override fun onDisconnected(camera: CameraDevice) {
                        failure.set("Camera disconnected")
                        camera.close()
                        ready.countDown()
                    }

                    override fun onError(camera: CameraDevice, error: Int) {
                        failure.set("Camera error $error")
                        camera.close()
                        ready.countDown()
                    }

                },

                handler
            )
            assertTrue("Camera configuration timed out", ready.await(8, TimeUnit.SECONDS))
            check(failure.get() == null) { checkNotNull(failure.get()) }
            val info = MediaCodec.BufferInfo()

            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(4)
            var first = 0L
            var last = 0L
            var frames = 0
            while (System.nanoTime() < deadline) {

                val index = encoder.dequeueOutputBuffer(info, 10000)
                if (index >= 0) {

                    if (info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {

                        if (frames == 0)
                            first = info.presentationTimeUs

                        last = info.presentationTimeUs
                        frames++

                    }

                    encoder.releaseOutputBuffer(index, false)

                }

            }

            val fps = (frames - 1) * 1000000.0 / (last - first).coerceAtLeast(1)
            println("CamDroid hardware test: $frames frames, measured $fps FPS, capture range ${target.fpsRange}")
            assertTrue("High-speed capture remains capped at 30 FPS: $fps", frames > 120 && fps > 45)

        } finally {

            closing.set(true)
            // Serialize teardown behind callbacks already queued by openCamera().
            val cleanup = {

                val capture = session.getAndSet(null)
                val camera = device.getAndSet(null)
                runCatching { capture?.stopRepeating() }
                runCatching { capture?.close() }
                runCatching { camera?.close() }

                thread.quitSafely()
                Unit

            }

            if (cleanupHandler?.post { cleanup() } != true)
                cleanup()

            var interrupted = false
            while (thread.isAlive) {

                try {

                    thread.join()

                } catch (_: InterruptedException) {
                    interrupted = true
                }

            }

            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            runCatching { surface?.release() }
            runCatching { activity?.close() }

            if (interrupted)
                Thread.currentThread().interrupt()

        }

    }

}
