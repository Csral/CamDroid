package csral.camdroid.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val DarkColorScheme =
    darkColorScheme(
        primary = Color(0xFFB8F568),
        onPrimary = Color(0xFF172507),
        secondary = Color(0xFFB6C6AF),

        background = Color(0xFF10130F),
        surface = Color(0xFF191D17),
        surfaceVariant = Color(0xFF252B21),
        onBackground = Color(0xFFF0F3E9),

        onSurface = Color(0xFFF0F3E9),
        onSurfaceVariant = Color(0xFFA7B09F),
        outline = Color(0xFF46503E),
        error = Color(0xFFFFB4AB)
    )

private val LightColorScheme =
    lightColorScheme(
        primary = Color(0xFF416900), onPrimary = Color.White, background = Color(0xFFF7F9EF), surface = Color(0xFFF7F9EF)
    )

@Composable
fun CamDroidTheme(darkTheme: Boolean = isSystemInDarkTheme(), dynamicColor: Boolean = true, content: @Composable () -> Unit) {

    val colors =
        if (dynamicColor && android.os.Build.VERSION.SDK_INT >= 31) {

            val context = androidx.compose.ui.platform.LocalContext.current
            if (darkTheme)
                androidx.compose.material3.dynamicDarkColorScheme(context)
            else androidx.compose.material3.dynamicLightColorScheme(context)

        } else if (darkTheme) DarkColorScheme else LightColorScheme

    MaterialTheme(colorScheme = colors, typography = Typography, content = content)

}
