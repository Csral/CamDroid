package csral.camdroid

import androidx.test.ext.junit.runners.AndroidJUnit4
import java.nio.ByteBuffer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith

/** No desktop required. Run on a device/emulator to check the actual JNI boundary. */
@RunWith(AndroidJUnit4::class)
class NativeTransportTest {

    @Test
    fun fpsTargetsAreIndependentOfCameraRangeUpperBounds() {

        val limits = EncoderLimits("test", 1, 120, 1000, 20000000)
        val ranges = listOf(android.util.Range(15, 30), android.util.Range(30, 30), android.util.Range(60, 60))
        for (fps in listOf(5, 15, 24, 30, 60)) {

            val tuning = selectStreamTuning(fps, 4000000, ranges, limits)
            assertEquals(fps, tuning.encoderFps)

        }

        assertEquals(30, selectStreamTuning(60, 4000000, ranges.take(2), limits).encoderFps)
        assertEquals(30, selectStreamTuning(0, 0, ranges, limits).encoderFps)

    }

    @Test
    fun discoveryRepliesValidateNoncePortVersionAndAvailability() {

        val name = "Test desktop".toByteArray()
        val bytes =
            ByteBuffer.allocate(21 + name.size)
                .put("CDPEER01".toByteArray())
                .putLong(123)

                .putShort(7000.toShort())
                .put(1)
                .put(0)
                .put(name.size.toByte())
                .put(name)

                .array()
        val peer = DesktopDiscovery.parseReply(bytes, bytes.size, 123, "192.168.1.4", 5, 10)
        assertNotNull(peer)
        assertEquals("Test desktop", peer?.name)
        assertEquals(7000, peer?.port)

        assertNull(DesktopDiscovery.parseReply(bytes, bytes.size, 124, "192.168.1.4", 5, 10))
        for (size in 0 until bytes.size)
            assertNull(DesktopDiscovery.parseReply(bytes, size, 123, "192.168.1.4", 5, 10))
        for (index in listOf(0, 18, 19, 20)) {

            val malformed = bytes.copyOf()
            malformed[index] = 127
            assertNull(DesktopDiscovery.parseReply(malformed, malformed.size, 123, "192.168.1.4", 5, 10))

        }

        val busy = bytes.copyOf().also { it[19] = 1 }
        assertNull(DesktopDiscovery.parseReply(busy, busy.size, 123, "192.168.1.4", 5, 10))

    }

    @Test
    fun closedTransportHandlesAreRejectedAndCloseIsIdempotent() {

        val handle = TransportBridge.create()
        assertFalse(TransportBridge.isConnected(handle))
        assertFalse(TransportBridge.needsVideoKeyframe(handle))
        TransportBridge.close(handle)
        TransportBridge.close(handle)

        assertThrows(IllegalStateException::class.java) { TransportBridge.isConnected(handle) }
        assertThrows(IllegalStateException::class.java) { TransportBridge.needsVideoKeyframe(handle) }

    }

    @Test
    fun arbitraryTransportHandlesAreRejected() {
        assertThrows(IllegalStateException::class.java) { TransportBridge.isConnected(Long.MAX_VALUE) }
    }

    @Test
    fun heapBuffersAndInvalidDirectBufferRangesAreRejected() {

        val handle = TransportBridge.create()
        try {

            assertThrows(IllegalArgumentException::class.java) {
                TransportBridge.videoFragment(handle, ByteBuffer.allocate(8), 0, 8, 0, true, false)
            }

            assertThrows(IllegalArgumentException::class.java) {
                TransportBridge.videoFragment(handle, ByteBuffer.allocateDirect(8), 7, 2, 0, true, false)
            }

            assertThrows(IllegalArgumentException::class.java) {
                TransportBridge.videoFragment(handle, ByteBuffer.allocateDirect(8), 0, 8, -1, true, false)
            }

        } finally {

            TransportBridge.close(handle)

        }

    }

    @Test
    fun legacySessionHandlesAreAlsoValidated() {

        val handle = NativeBridge.createSession()
        NativeBridge.closeSession(handle)
        NativeBridge.closeSession(handle)
        assertThrows(IllegalStateException::class.java) { NativeBridge.isEstablished(handle) }
        assertThrows(IllegalStateException::class.java) { NativeBridge.processNextPacket(handle) }

    }

}
