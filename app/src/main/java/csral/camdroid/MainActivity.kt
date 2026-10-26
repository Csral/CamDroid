package csral.camdroid

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import csral.camdroid.ui.CamDroidApp
import csral.camdroid.ui.theme.CamDroidTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {

    private var connection: TcpStreamer? = null
    private var pendingConnection: TcpStreamer? = null
    private var connectionGeneration = 0L

    override fun onCreate(savedInstanceState: Bundle?) {

        super.onCreate(savedInstanceState)

        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT)
        )
        setContent {

            CamDroidTheme(darkTheme = true, dynamicColor = false) {
                CamDroidApp(onConnect = ::connectDesktop, onDisconnect = ::disconnectDesktop)
            }

        }

    }

    private suspend fun connectDesktop(host: String, port: Int, networkHandle: Long): TcpStreamer {

        disconnectDesktop()
        val generation = connectionGeneration
        val candidate = TcpStreamer()
        pendingConnection = candidate
        try {

            val connected =
                withContext(Dispatchers.IO) {
                    candidate.connect(host, port, networkHandle)
                    candidate
                }

            check(generation == connectionGeneration && pendingConnection === candidate) { "Connection was cancelled." }
            connection = connected
            return connected

        } catch (error: Throwable) {
            withContext(NonCancellable + Dispatchers.IO) { runCatching { candidate.close() } }
            throw error
        } finally {

            if (pendingConnection === candidate)
                pendingConnection = null

        }

    }

    private fun disconnectDesktop() {

        StreamingService.stop(this, disconnect = true)

        connectionGeneration++
        runCatching { pendingConnection?.close() }
        pendingConnection = null
        runCatching { connection?.close() }
        connection = null

    }

    override fun onDestroy() {
        if (!StreamingService.isActive)
            disconnectDesktop()

        super.onDestroy()
    }

    override fun onStop() {
        if (!StreamingService.isActive)
            disconnectDesktop()

        super.onStop()
    }

}
