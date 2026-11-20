package com.signify.hue.flutterreactiveble.ble

import android.os.SystemClock
import androidx.annotation.VisibleForTesting
import com.polidea.rxandroidble2.RxBleConnection
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.RxBleDevice
import com.signify.hue.flutterreactiveble.model.ConnectionState
import com.signify.hue.flutterreactiveble.model.toConnectionState
import com.signify.hue.flutterreactiveble.utils.Duration
import io.reactivex.Completable
import io.reactivex.Observable
import io.reactivex.Single
import io.reactivex.disposables.Disposable
import io.reactivex.disposables.SerialDisposable
import io.reactivex.functions.Function
import io.reactivex.subjects.BehaviorSubject
import io.reactivex.subjects.CompletableSubject
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

internal class DeviceConnector(
    private val device: RxBleDevice,
    private val connectionTimeout: Duration,
    private val updateListeners: (update: ConnectionUpdate) -> Unit,
    private val connectionQueue: ConnectionQueue,
) {
    companion object {
        private const val minTimeMsBeforeDisconnectingIsAllowed = 200L
        private const val delayMsAfterClearingCache = 300L
    }

    private val connectDeviceSubject = BehaviorSubject.create<EstablishConnectionResult>()

    private var timestampEstablishConnection: Long = 0

    @VisibleForTesting
    internal var connectionDisposable: Disposable? = null

    private val lazyConnection =
        lazy {
            connectionDisposable = establishConnection(device)
            connectDeviceSubject
        }

    private val currentConnection: EstablishConnectionResult?
        get() = if (lazyConnection.isInitialized()) connection.value else null

    internal val connection by lazyConnection

    private val disconnectRequested = AtomicBoolean()
    private val nativeConnectionAdmitted = AtomicBoolean()
    private val nativeDisconnected = CompletableSubject.create()
    private val disconnectCompleted = CompletableSubject.create()
    // This owner exists before subscribe: a synchronous native event cannot
    // recurse into lazy initialization or escape disposal.
    private val connectionStatusUpdates = SerialDisposable()
    private val listenToConnectionStatus by lazy {
        connectionStatusUpdates.set(device.observeConnectionStateChanges()
            .startWith(device.connectionState)
            // A queued SDK connection may still report DISCONNECTED. Observe
            // before admission; only a later owned publication proves cleanup.
            .filter { nativeConnectionAdmitted.get() || it != RxBleConnection.RxBleConnectionState.DISCONNECTED }
            .distinctUntilChanged()
            .map<ConnectionUpdate> { ConnectionUpdateSuccess(device.macAddress, it.toConnectionState().code) }
            .onErrorReturn {
                ConnectionUpdateError(
                    device.macAddress,
                    it.message
                        ?: "Unknown error",
                )
            }
            .subscribe {
                updateListeners.invoke(it)
                if (nativeConnectionAdmitted.get() && it is ConnectionUpdateSuccess &&
                    it.connectionState == ConnectionState.DISCONNECTED.code
                ) {
                    nativeDisconnected.onComplete()
                }
            })
    }

    internal fun disconnectDevice(deviceId: String, onComplete: () -> Unit = {}) {
        disconnectCompleted.subscribe(onComplete)
        if (!disconnectRequested.compareAndSet(false, true)) return
        val diff = SystemClock.elapsedRealtime() - timestampEstablishConnection

        /*
        in order to prevent Android from ignoring disconnects we add a delay when we try to
        disconnect too quickly after establishing connection. https://issuetracker.google.com/issues/37121223
         */
        if (diff < minTimeMsBeforeDisconnectingIsAllowed) {
            Single.timer(minTimeMsBeforeDisconnectingIsAllowed - diff, TimeUnit.MILLISECONDS)
                .subscribe { disposeSubscriptions() }
        } else {
            disposeSubscriptions()
        }
    }

    private fun disposeSubscriptions() {
        listenToConnectionStatus
        val connectionWasPending = currentConnection == null
        connectionDisposable?.dispose()
        if (connectionWasPending) connectionQueue.removeFromQueue(device.macAddress)
        connectDeviceSubject.onComplete()
        // The published disposable queues unsubscription after subscription on
        // the SDK's same single scheduler. Every admitted attempt then publishes
        // fresh native cleanup, even when it never obtained a GATT. A sampled
        // current DISCONNECTED value cannot certify that queued work is finished.
        if (!nativeConnectionAdmitted.get()) {
            updateListeners(ConnectionUpdateSuccess(device.macAddress, ConnectionState.DISCONNECTED.code))
            nativeDisconnected.onComplete()
        }
        nativeDisconnected.subscribe {
            connectionStatusUpdates.dispose()
            disconnectCompleted.onComplete()
        }
    }

    private fun establishConnection(rxBleDevice: RxBleDevice): Disposable {
        val deviceId = rxBleDevice.macAddress

        val shouldNotTimeout = connectionTimeout.value <= 0L
        connectionQueue.addToQueue(deviceId)
        updateListeners(ConnectionUpdateSuccess(deviceId, ConnectionState.CONNECTING.code))

        return waitUntilFirstOfQueue(deviceId)
            .switchMap { queue ->
                if (!queue.contains(deviceId)) {
                    Observable.just(
                        EstablishConnectionFailure(
                            deviceId,
                            "Device is not in queue",
                        ),
                    )
                } else {
                    connectDevice(rxBleDevice, shouldNotTimeout)
                        .map<EstablishConnectionResult> { EstablishedConnection(rxBleDevice.macAddress, it) }
                }
            }
            .onErrorReturn { error ->
                EstablishConnectionFailure(
                    rxBleDevice.macAddress,
                    error.message ?: "Unknown error",
                )
            }
            .doOnNext {
                // Trigger side effect by calling the lazy initialization of this property so
                // listening to changes starts.
                listenToConnectionStatus
                timestampEstablishConnection = SystemClock.elapsedRealtime()
                connectionQueue.removeFromQueue(deviceId)
                if (it is EstablishConnectionFailure) {
                    updateListeners.invoke(ConnectionUpdateError(deviceId, it.errorMessage))
                }
            }
            .doOnError {
                connectionQueue.removeFromQueue(deviceId)
                updateListeners.invoke(
                    ConnectionUpdateError(
                        deviceId,
                        it.message
                            ?: "Unknown error",
                    ),
                )
            }
            .subscribe(
                { connectDeviceSubject.onNext(it) },
                { throwable -> connectDeviceSubject.onError(throwable) },
            )
    }

    private fun connectDevice(
        rxBleDevice: RxBleDevice,
        shouldNotTimeout: Boolean,
    ): Observable<RxBleConnection> {
        listenToConnectionStatus
        return rxBleDevice.establishConnection(shouldNotTimeout)
            .doOnSubscribe { nativeConnectionAdmitted.set(true) }
            .compose {
                if (shouldNotTimeout) {
                    it
                } else {
                    it.timeout(
                        Observable.timer(connectionTimeout.value, connectionTimeout.unit),
                        Function<RxBleConnection, Observable<Unit>> {
                            Observable.never<Unit>()
                        },
                    )
                }
            }
    }

    internal fun clearGattCache(): Completable =
        currentConnection?.let { connection ->
            when (connection) {
                is EstablishedConnection -> clearGattCache(connection.rxConnection)
                is EstablishConnectionFailure -> Completable.error(Throwable(connection.errorMessage))
            }
        } ?: Completable.error(IllegalStateException("Connection is not established"))

    /**
     * Clear GATT attribute cache using an undocumented method `BluetoothGatt.refresh()`.
     *
     * May trigger the following warning in the system message log:
     *
     * https://android.googlesource.com/platform/frameworks/base/+/pie-release/config/hiddenapi-light-greylist.txt
     *
     *     Accessing hidden method Landroid/bluetooth/BluetoothGatt;->refresh()Z (light greylist, reflection)
     *
     * Known to work up to Android Q beta 2.
     */
    private fun clearGattCache(connection: RxBleConnection): Completable {
        val operation =
            RxBleCustomOperation<Unit> { bluetoothGatt, _, _ ->
                try {
                    val refreshMethod = bluetoothGatt.javaClass.getMethod("refresh")
                    val success = refreshMethod.invoke(bluetoothGatt) as Boolean
                    if (success) {
                        Observable.empty<Unit>()
                            .delay(DeviceConnector.Companion.delayMsAfterClearingCache, TimeUnit.MILLISECONDS)
                    } else {
                        val reason = "BluetoothGatt.refresh() returned false"
                        Observable.error(RuntimeException(reason))
                    }
                } catch (e: ReflectiveOperationException) {
                    Observable.error<Unit>(e)
                }
            }
        return connection.queue(operation).ignoreElements()
    }

    private fun waitUntilFirstOfQueue(deviceId: String) =
        connectionQueue.observeQueue()
            .filter { queue ->
                queue.firstOrNull() == deviceId || !queue.contains(deviceId)
            }
            .takeUntil { it.isEmpty() || it.first() == deviceId }

    /**
     * Reads the current RSSI value of the device
     */
    internal fun readRssi(): Single<Int> =
        currentConnection?.let { connection ->
            when (connection) {
                is EstablishedConnection -> connection.rxConnection.readRssi()
                is EstablishConnectionFailure -> Single.error(Throwable(connection.errorMessage))
            }
        } ?: Single.error(IllegalStateException("Connection is not established"))
}
