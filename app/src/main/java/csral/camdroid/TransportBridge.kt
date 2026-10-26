package csral.camdroid

import java.nio.ByteBuffer

/** Native workers never call Java. Managed workers submit commands through JNI. */
internal object TransportBridge {

    init {

        System.loadLibrary("camdroid_native")

    }

    external fun create(): Long

    external fun connect(handle: Long, host: String, port: Int, optimized: Boolean, networkHandle: Long)

    external fun interrupt(handle: Long)

    external fun close(handle: Long)

    external fun isConnected(handle: Long): Boolean

    external fun videoConfig(
        handle: Long,
        csd0: ByteArray,
        csd1: ByteArray,
        rotation: Int,

        mirrored: Boolean,
        fps: Int,
        bitrate: Int,
        userConfigured: Boolean
    )

    external fun videoFragment(
        handle: Long,
        buffer: ByteBuffer,
        offset: Int,
        size: Int,

        ptsUs: Long,
        keyframe: Boolean,
        partial: Boolean
    ): Boolean

    external fun videoSettings(handle: Long, fps: Int, bitrate: Int, userConfigured: Boolean)

    external fun videoRotation(handle: Long, rotation: Int)

    external fun videoEos(handle: Long)

    external fun needsVideoKeyframe(handle: Long): Boolean

    external fun sendPacket(handle: Long, type: Int, flags: Int, payloadFlags: Int, payload: ByteArray)

    external fun waitPacket(handle: Long): IncomingPacket?

}
