package com.signify.hue.flutterreactiveble

import com.polidea.rxandroidble2.exceptions.BleDisconnectedException
import com.polidea.rxandroidble2.exceptions.BleGattException
import io.reactivex.exceptions.UndeliverableException
import io.reactivex.plugins.RxJavaPlugins
import java.util.concurrent.atomic.AtomicBoolean

/** Keeps expected BLE teardown races from escaping RxJava after Dart cancels. */
internal object BleRxErrorPolicy {
    private val installed = AtomicBoolean(false)

    fun isExpectedTeardown(error: Throwable): Boolean {
        val cause = if (error is UndeliverableException) error.cause ?: error else error
        return cause is BleDisconnectedException || cause is BleGattException
    }

    fun install() {
        if (!installed.compareAndSet(false, true)) return
        RxJavaPlugins.setErrorHandler { error ->
            if (isExpectedTeardown(error)) return@setErrorHandler
            val thread = Thread.currentThread()
            thread.uncaughtExceptionHandler?.uncaughtException(thread, error)
        }
    }
}
