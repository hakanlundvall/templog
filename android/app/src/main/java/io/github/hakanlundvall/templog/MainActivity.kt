package io.github.hakanlundvall.templog

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import io.github.hakanlundvall.templog.ui.PermissionRationale
import io.github.hakanlundvall.templog.ui.TemplogScreen
import io.github.hakanlundvall.templog.ui.TemplogTheme

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            val model: TemplogViewModel = viewModel()
            var granted by remember { mutableStateOf(hasBluetoothPermissions()) }

            val launcher = rememberLauncherForActivityResult(
                ActivityResultContracts.RequestMultiplePermissions(),
            ) { results ->
                granted = results.values.all { it }
            }

            LaunchedEffect(granted) {
                if (granted) model.onPermissionsGranted()
            }

            TemplogTheme {
                if (granted) {
                    TemplogScreen(
                        viewModel = model,
                        permissionsGranted = true,
                        onRequestPermissions = { launcher.launch(requiredPermissions()) },
                    )
                } else {
                    PermissionRationale(
                        onRequestPermissions = { launcher.launch(requiredPermissions()) },
                    )
                }
            }
        }
    }

    private fun hasBluetoothPermissions(): Boolean = requiredPermissions().all {
        ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
    }

    private companion object {
        /**
         * Android 12 split the Bluetooth permissions; before that, scanning was
         * gated behind fine location instead.
         */
        fun requiredPermissions(): Array<String> =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                arrayOf(
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT,
                )
            } else {
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
            }
    }
}
