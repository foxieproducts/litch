package com.signify.hue.flutterreactiveble.ble

import android.bluetooth.BluetoothGatt
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.exceptions.BleGattCallbackTimeoutException
import com.polidea.rxandroidble2.exceptions.BleGattCannotStartException
import com.polidea.rxandroidble2.exceptions.BleGattOperationType
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import io.reactivex.Observable
import io.reactivex.Scheduler
import java.util.concurrent.TimeUnit

private const val MTU_CALLBACK_TIMEOUT_SECONDS = 5L

/** Bounds MTU negotiation inside the SDK queue so later GATT work can proceed. */
internal class BoundedMtuRequestOperation(
    private val requestedMtu: Int,
    private val observedMtu: () -> Int,
) : RxBleCustomOperation<Int> {
    override fun asObservable(
        bluetoothGatt: BluetoothGatt,
        rxBleGattCallback: RxBleGattCallback,
        bluetoothInteractionScheduler: Scheduler,
    ): Observable<Int> =
        Observable.create<Int> { emitter ->
            // Install the callback before issuing the platform request. The
            // deadline belongs here: an outer timeout cannot release SDK work.
            val callback = rxBleGattCallback.onMtuChanged
                .take(1)
                .timeout(
                    MTU_CALLBACK_TIMEOUT_SECONDS,
                    TimeUnit.SECONDS,
                    bluetoothInteractionScheduler,
                    Observable.error(
                        BleGattCallbackTimeoutException(
                            bluetoothGatt,
                            BleGattOperationType.ON_MTU_CHANGED,
                        ),
                    ),
                )
                .subscribe(
                    { emitter.onNext(it) },
                    { emitter.tryOnError(it) },
                    { emitter.onComplete() },
                )
            emitter.setDisposable(callback)
            if (emitter.isDisposed) return@create
            // An update may have arrived while another GATT operation owned
            // the queue. Reuse that observed value before asking Android again.
            val currentMtu = observedMtu()
            if (currentMtu >= requestedMtu) {
                emitter.onNext(currentMtu)
                emitter.onComplete()
            } else if (!emitter.isDisposed && !bluetoothGatt.requestMtu(requestedMtu)) {
                emitter.tryOnError(
                    BleGattCannotStartException(
                        bluetoothGatt,
                        BleGattOperationType.ON_MTU_CHANGED,
                    ),
                )
            }
        }.subscribeOn(bluetoothInteractionScheduler)
}
