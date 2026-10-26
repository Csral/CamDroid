package csral.camdroid.ui

import android.Manifest
import android.content.pm.PackageManager
import android.util.Size
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import csral.camdroid.Camera2Preview
import csral.camdroid.StreamTuning
import csral.camdroid.TcpStreamer

@Composable
internal fun LiveCameraPreview(
    front: Boolean,
    streaming: Boolean,
    connection: TcpStreamer?,

    resolution: String,
    onReady: (Boolean) -> Unit,
    onStreaming: () -> Unit,
    onStopped: (TcpStreamer?, Boolean) -> Unit,
    onError: (String) -> Unit,

    onResolutions: (List<String>) -> Unit,
    requestedFps: Int,
    requestedBitrate: Int,
    userFormatSelected: Boolean,
    onTuningApplied: (StreamTuning) -> Unit,

    onTuningError: (String) -> Unit,
    onPreparing: () -> Unit
) {

    val context = LocalContext.current
    var granted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        )
    }

    var showPreview by remember { mutableStateOf(false) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {

        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_PAUSE || event == Lifecycle.Event.ON_STOP)
                showPreview = false

            if (event == Lifecycle.Event.ON_RESUME)
                granted =

                    ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                        PackageManager.PERMISSION_GRANTED

        }

        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }

    }

    val launcher =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
            granted = it
            if (!it)
                onReady(false)
        }

    if (granted) {

        val size =
            when (resolution) {

                "4K" -> Size(3840, 2160)
                "720p" -> Size(1280, 720)
                "480p" -> Size(640, 480)
                else -> Size(1920, 1080)

            }

        Box(Modifier.fillMaxSize()) {

            Camera2Preview(
                front,
                Modifier.fillMaxSize(),
                streaming,
                connection,

                size,
                onReady,
                onStreaming,
                onStopped,
                onError,

                onAvailableResolutions = { sizes ->
                    onResolutions(sizes.map { if (it.width == 3840) "4K" else "${it.height}p" })
                },

                requestedFps = requestedFps,
                requestedBitrate = requestedBitrate,
                userFormatSelected = userFormatSelected,
                onTuningApplied = onTuningApplied,
                onTuningError = onTuningError,

                onPreparing = onPreparing,
                previewEnabled = showPreview
            )

            if (showPreview)
                TextButton(onClick = { showPreview = false }, modifier = Modifier.align(Alignment.TopEnd).padding(12.dp)) {
                    Text("Hide preview")
                }
            else
                Button(onClick = { showPreview = true }, modifier = Modifier.align(Alignment.Center)) {
                    Text("Show preview")
                }

        }

    } else {

        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {

            Column(
                Modifier.padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text("Allow camera access to stream or show the preview.")

                Button(onClick = { launcher.launch(Manifest.permission.CAMERA) }) { Text("Allow camera") }
            }

        }

    }

}
