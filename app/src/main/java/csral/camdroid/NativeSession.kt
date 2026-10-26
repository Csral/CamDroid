package csral.camdroid

class NativeSession : AutoCloseable {

    private var handle: Long = NativeBridge.createSession()

    init {

        check(handle != 0L) {
            "Could not create a native session."
        }

    }

    @Synchronized
    override fun close() {

        if (handle == 0L) {

            return

        }

        NativeBridge.closeSession(handle)
        handle = 0L

    }

    @Synchronized
    fun feed(data: ByteArray, size: Int = data.size) {
        check(handle != 0L) { "Native session is closed." }
        require(size in 0..data.size) { "Invalid received byte count." }

        NativeBridge.feedSession(handle, data, size)
    }

    @Synchronized
    fun processNextPacket(): Boolean {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.processNextPacket(handle)
    }

    @Synchronized
    fun nextResponse(): NativeResponse? {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.nextResponse(handle)
    }

    @Synchronized
    fun initialHandshake(optimized: Boolean = true): ByteArray {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.initialHandshake(handle, optimized)
    }

    @Synchronized
    fun isTerminalFinPending(): Boolean {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.isTerminalFinPending(handle)
    }

    @Synchronized
    fun terminalFinSent() {
        check(handle != 0L) { "Native session is closed." }

        NativeBridge.terminalFinSent(handle)
    }

    @Synchronized
    fun isEstablished(): Boolean {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.isEstablished(handle)
    }

    @Synchronized
    fun videoConfig(csd0: ByteArray, csd1: ByteArray): ByteArray {

        check(handle != 0L) { "Native session is closed." }

        require(csd0.isNotEmpty() || csd1.isNotEmpty()) {
            "At least one codec configuration buffer is required."
        }

        return NativeBridge.videoConfig(handle, csd0, csd1)

    }

    @Synchronized
    fun videoFrame(data: ByteArray, ptsUs: Long, keyframe: Boolean): ByteArray {

        check(handle != 0L) { "Native session is closed." }
        require(data.isNotEmpty()) { "Encoded frame must not be empty." }
        require(ptsUs >= 0) { "Presentation timestamp must not be negative." }

        return NativeBridge.videoFrame(handle, data, ptsUs, keyframe)

    }

    @Synchronized
    fun videoEos(): ByteArray {
        check(handle != 0L) { "Native session is closed." }

        return NativeBridge.videoEos(handle)
    }

}
