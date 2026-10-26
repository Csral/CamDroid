package csral.camdroid

/** Payload is owned by this event. Listeners run in wire order on the control worker. */
data class IncomingPacket(val type: Int, val flags: Int, val payloadFlags: Int, val timestampMs: Long, val payload: ByteArray)
