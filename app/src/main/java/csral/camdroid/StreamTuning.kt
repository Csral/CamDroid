package csral.camdroid

import android.util.Range
import kotlin.math.abs

data class StreamTuning(val fpsRange: Range<Int>, val bitrate: Int, val encoder: EncoderLimits, val encoderFps: Int) {
    val fpsLabel: String
        get() = if (fpsRange.lower == fpsRange.upper) "${fpsRange.upper}" else "${fpsRange.lower}–${fpsRange.upper}"
}

fun selectStreamTuning(
    requestedFps: Int,
    requestedBitrate: Int,
    ranges: List<Range<Int>>,
    encoder: EncoderLimits
): StreamTuning {

    val supported = ranges.filter { it.lower > 0 && it.upper in encoder.minFps..encoder.maxFps }
    check(supported.isNotEmpty()) { "No common camera/encoder FPS range for this resolution." }
    val target = if (requestedFps == 0) 30 else requestedFps.coerceIn(1, 240)

    val range =
        supported.minWith(
            compareBy<Range<Int>> {
                    abs(target - it.upper)
                }

                .thenBy { if (requestedFps == 0) -(it.upper - it.lower) else it.upper - it.lower }
        )
    if (requestedBitrate == 0)
        check(maxOf(2_000_000, encoder.minBitrate) <= minOf(40_000_000, encoder.maxBitrate)) {
            "This encoder cannot provide adaptive bitrate in the 2–40 Mbps range. Choose a fixed bitrate."
        }

    val bitrate =
        (if (requestedBitrate == 0) 12_000_000 else requestedBitrate).coerceIn(
            if (requestedBitrate == 0)
                maxOf(2_000_000, encoder.minBitrate) else encoder.minBitrate.coerceAtLeast(1),

            if (requestedBitrate == 0)
                minOf(40_000_000, encoder.maxBitrate) else encoder.maxBitrate

        )

    return StreamTuning(range, bitrate, encoder, range.upper)

}
