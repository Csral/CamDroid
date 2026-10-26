package csral.camdroid

object NativeBridge {

    init {

        System.loadLibrary("camdroid_native")

    }

    external fun protocolVersion(): Int

    external fun createSession(): Long

    external fun closeSession(handle: Long)

    external fun feedSession(handle: Long, data: ByteArray, size: Int)

    external fun processNextPacket(handle: Long): Boolean

    external fun nextResponse(handle: Long): NativeResponse?

    external fun initialHandshake(handle: Long, optimized: Boolean): ByteArray

    external fun isTerminalFinPending(handle: Long): Boolean

    external fun terminalFinSent(handle: Long)

    external fun isEstablished(handle: Long): Boolean

    external fun videoConfig(handle: Long, csd0: ByteArray, csd1: ByteArray): ByteArray

    external fun videoFrame(handle: Long, data: ByteArray, ptsUs: Long, keyframe: Boolean): ByteArray

    external fun videoEos(handle: Long): ByteArray

}
