package csral.camdroid

import java.nio.ByteBuffer

data class VideoFormatRequest(
    val id: Int,
    val width: Int,
    val height: Int,
    val fps: Int,

    val payload: ByteArray,
    val bitrate: Int = 0
) {

    fun reply(connection: TcpStreamer, status: Int) {
        require(status in 0..3)
        connection.sendPacket(1, if (status == 0) 1 else 3, payload = payload + status.toByte())
    }

    companion object {

        fun parse(packet: IncomingPacket): VideoFormatRequest? {

            if (packet.type != 1 || packet.payload.firstOrNull()?.toInt() != 0x10)
                return null

            require(
                (packet.payload.size == 11 || packet.payload.size == 15) &&
                    packet.flags and 4 != 0 &&
                    packet.flags and 1 == 0
            )

            val data = ByteBuffer.wrap(packet.payload)
            return VideoFormatRequest(
                data.getInt(1),
                data.getShort(5).toInt() and 0xffff,
                data.getShort(7).toInt() and 0xffff,

                data.getShort(9).toInt() and 0xffff,
                packet.payload.copyOf(),
                if (packet.payload.size == 15)
                    data.getInt(11) else 0

            )

        }

    }

}
