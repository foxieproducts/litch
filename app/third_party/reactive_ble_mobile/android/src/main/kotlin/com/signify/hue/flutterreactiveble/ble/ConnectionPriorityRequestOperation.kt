package com.signify.hue.flutterreactiveble.ble

import android.bluetooth.BluetoothGatt
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.exceptions.BleGattCannotStartException
import com.polidea.rxandroidble2.exceptions.BleGattOperationType
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import io.reactivex.Observable
import io.reactivex.Scheduler

/** A priority hint owns this queue slot only until Android accepts or refuses it. */
internal class ConnectionPriorityRequestOperation(
    private val priority: Int,
) : RxBleCustomOperation<Unit> {
    override fun asObservable(
        bluetoothGatt: BluetoothGatt,
        rxBleGattCallback: RxBleGattCallback,
        bluetoothInteractionScheduler: Scheduler,
    ): Observable<Unit> =
        Observable.fromCallable {
            // An unchanged or declined interval need not produce a callback.
            // This advisory request must not hold discovery, MTU or session work.
            if (!bluetoothGatt.requestConnectionPriority(priority)) {
                throw BleGattCannotStartException(
                    bluetoothGatt, BleGattOperationType.CONNECTION_PRIORITY_CHANGE,
                )
            }
            Unit
        }.subscribeOn(bluetoothInteractionScheduler)
}
