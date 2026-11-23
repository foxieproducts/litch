package com.foxieproducts.litch_mobile

import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Build
import android.view.Display
import android.view.Surface
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private var orientationChannel: MethodChannel? = null
    private var bluetoothRecovery: AndroidBluetoothPairingRecovery? = null
    private val orientationListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) = Unit
        override fun onDisplayRemoved(displayId: Int) = Unit
        override fun onDisplayChanged(displayId: Int) {
            if (currentDisplay()?.displayId == displayId) {
                orientationChannel?.invokeMethod("orientationChanged", null)
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun currentDisplay(): Display? {
        // Context.display starts at API 30; the app also supports API 24–29.
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display
        } else {
            windowManager.defaultDisplay
        }
    }

    private fun landscapeSide(): String? {
        if (resources.configuration.orientation != Configuration.ORIENTATION_LANDSCAPE) return null
        val screen = currentDisplay() ?: return null
        val mode = screen.mode
        // Display rotation is relative to the panel's natural orientation.
        // Use the panel shape even when a split window has a different shape.
        return if (mode.physicalWidth > mode.physicalHeight) {
            when (screen.rotation) {
                Surface.ROTATION_0 -> "right"
                Surface.ROTATION_180 -> "left"
                else -> null
            }
        } else {
            when (screen.rotation) {
                Surface.ROTATION_90 -> "left"
                Surface.ROTATION_270 -> "right"
                else -> null
            }
        }
    }

    override fun onStart() {
        super.onStart()
        // Ask Android to jump-cut its window rotation. Flutter animates only
        // opted-in content; never stack a whole-window turn over that motion.
        window.attributes = window.attributes.apply {
            rotationAnimation = WindowManager.LayoutParams.ROTATION_ANIMATION_JUMPCUT
        }
        getSystemService(DisplayManager::class.java)
            .registerDisplayListener(orientationListener, null)
    }

    override fun onStop() {
        getSystemService(DisplayManager::class.java)
            .unregisterDisplayListener(orientationListener)
        super.onStop()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (bluetoothRecovery?.onActivityResult(requestCode, resultCode, data) == true) return
        super.onActivityResult(requestCode, resultCode, data)
    }

    override fun onDestroy() {
        bluetoothRecovery?.close()
        bluetoothRecovery = null
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateSystemBars()
        // Publish after Android has committed the new configuration. Display
        // callbacks still cover landscape-left <-> landscape-right, where the
        // orientation class itself does not change; Dart coalesces duplicates.
        window.decorView.post {
            orientationChannel?.invokeMethod("orientationChanged", null)
        }
    }

    override fun onPostResume() {
        super.onPostResume()
        updateSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) updateSystemBars()
    }

    private fun updateSystemBars() {
        // This Activity is the sole visibility authority. Run after Flutter's
        // own resume/focus restoration; Dart only consumes the resulting insets.
        window.decorView.post {
            WindowCompat.setDecorFitsSystemWindows(window, false)
            val controller = WindowCompat.getInsetsController(window, window.decorView)
            val multiWindow = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && isInMultiWindowMode
            val immersive = resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE && !multiWindow
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            if (immersive) {
                controller.hide(WindowInsetsCompat.Type.systemBars())
            } else {
                controller.show(WindowInsetsCompat.Type.systemBars())
            }
        }
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        bluetoothRecovery?.close()
        bluetoothRecovery = AndroidBluetoothPairingRecovery(
            this,
            MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "com.foxieproducts.litch/bluetooth_recovery"),
        )
        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "com.foxieproducts.litch/support",
        ).setMethodCallHandler { call, result ->
            if (call.method != "openEmail") {
                result.notImplemented()
                return@setMethodCallHandler
            }

            val recipient = call.argument<String>("recipient") ?: ""
            val subject = call.argument<String>("subject") ?: ""
            val body = call.argument<String>("body") ?: ""
            if (recipient != "support@foxieproducts.com" ||
                subject.length > 160 ||
                body.length > 6000
            ) {
                result.success(false)
                return@setMethodCallHandler
            }

            val uri = Uri.parse(
                "mailto:${Uri.encode(recipient)}" +
                    "?subject=${Uri.encode(subject)}&body=${Uri.encode(body)}",
            )
            val intent = Intent(Intent.ACTION_SENDTO, uri)
            // Launch the target so a missing mail handler reaches the copy fallback.
            try {
                startActivity(intent)
                result.success(true)
            } catch (_: ActivityNotFoundException) {
                result.success(false)
            }
        }

        MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "com.foxieproducts.litch/external_links",
        ).setMethodCallHandler { call, result ->
            if (call.method != "openUrl") {
                result.notImplemented()
                return@setMethodCallHandler
            }

            val url = call.argument<String>("url") ?: ""
            val allowedUrls = setOf(
                "https://foxieproducts.com/litch",
                "https://foxieproducts.com/policies/privacy-policy",
                "https://github.com/foxieproducts/litch",
                "https://duckchess.com",
            )
            if (!allowedUrls.contains(url)) {
                result.success(false)
                return@setMethodCallHandler
            }

            try {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                result.success(true)
            } catch (_: ActivityNotFoundException) {
                result.success(false)
            }
        }

        val orientation = MethodChannel(
            flutterEngine.dartExecutor.binaryMessenger,
            "com.foxieproducts.litch/interface_orientation",
        )
        orientationChannel = orientation
        orientation.setMethodCallHandler { call, result ->
            val side = landscapeSide()
            when (call.method) {
                "orientationState" -> result.success(
                    mapOf(
                        "side" to side,
                        "compensateHalfTurn" to false,
                    ),
                )
                "landscapeSide" -> result.success(side)
                else -> result.notImplemented()
            }
        }
    }
}
