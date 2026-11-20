package com.signify.hue.flutterreactiveble.ble.extensions

import android.bluetooth.BluetoothGattCharacteristic
import com.polidea.rxandroidble2.RxBleConnection
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.exceptions.BleGattCannotStartException
import com.polidea.rxandroidble2.exceptions.BleGattOperationType
import com.polidea.rxandroidble2.internal.Priority
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import io.reactivex.Completable
import io.reactivex.Observable
import io.reactivex.Scheduler
import io.reactivex.Single
import java.util.UUID
import java.util.concurrent.TimeUnit

private const val WRITE_CALLBACK_TIMEOUT_SECONDS = 5L
// Keep this byte-exact with ConnectionSessionCodec.version and the firmware
// session epoch. An old kind byte silently falls back to per-packet GATT writes
// and can stall a full artwork upload before the shared session sees progress.
private const val CONNECTION_SESSION_VERSION = 4
private const val CONNECTION_SESSION_OBJECT = (CONNECTION_SESSION_VERSION shl 4) or 4
private const val CONNECTION_SESSION_CONTROL = CONNECTION_SESSION_VERSION shl 4
private const val CONNECTION_SESSION_PROGRESS = 8
private const val CONNECTION_SESSION_HEADER_BYTES = 10
private const val CONNECTION_SESSION_PROGRESS_BYTES = 20

private fun readUInt32Le(bytes: ByteArray, offset: Int): Long =
    (bytes[offset].toLong() and 0xffL) or
        ((bytes[offset + 1].toLong() and 0xffL) shl 8) or
        ((bytes[offset + 2].toLong() and 0xffL) shl 16) or
        ((bytes[offset + 3].toLong() and 0xffL) shl 24)

internal fun connectionSessionObjectProgressAcceptsPacket(
    value: ByteArray,
    notification: ByteArray,
): Boolean {
    if (value.size < CONNECTION_SESSION_HEADER_BYTES ||
        (value[0].toInt() and 0xff) != CONNECTION_SESSION_OBJECT ||
        notification.size != CONNECTION_SESSION_PROGRESS_BYTES ||
        (notification[0].toInt() and 0xff) != CONNECTION_SESSION_CONTROL ||
        (notification[1].toInt() and 0xff) != CONNECTION_SESSION_PROGRESS
    ) {
        return false
    }
    val token = readUInt32Le(value, 2)
    val sequence = readUInt32Le(value, 6)
    val terminalThrough = readUInt32Le(notification, 6)
    val confirmedThrough = readUInt32Le(notification, 10)
    return token != 0L &&
        sequence != 0L &&
        readUInt32Le(notification, 2) == token &&
        confirmedThrough == terminalThrough &&
        confirmedThrough >= sequence
}

internal class LowPriorityCharacteristicWriteOperation(
    private val characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
) : RxBleCustomOperation<Unit> {
    private val value = value.copyOf()

    override fun asObservable(
        bluetoothGatt: android.bluetooth.BluetoothGatt,
        rxBleGattCallback: RxBleGattCallback,
        bluetoothInteractionScheduler: Scheduler,
    ): Observable<Unit> {
        val callbackCompletion = rxBleGattCallback.onCharacteristicWrite
            .filter { association ->
                association.first == characteristic.uuid &&
                    association.second.contentEquals(value)
            }
            .map { Unit }
        val objectProgressCompletion = if (
            value.size < CONNECTION_SESSION_HEADER_BYTES ||
            (value[0].toInt() and 0xff) != CONNECTION_SESSION_OBJECT
        ) {
            Observable.never<Unit>()
        } else {
            rxBleGattCallback.onCharacteristicChanged
                .filter { event ->
                    connectionSessionObjectProgressAcceptsPacket(value, event.data)
                }
                .map { Unit }
        }
        return Observable.create<Unit> { emitter ->
            // merge sends onSubscribe before attaching its input observers.
            // Register both completion sources before native entry so an early
            // callback/Progress cannot be lost while this queue slot is held.
            val completion = Observable.merge(callbackCompletion, objectProgressCompletion)
                .timeout(WRITE_CALLBACK_TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .subscribe(
                    { emitter.onNext(it) },
                    { emitter.tryOnError(it) },
                    { emitter.onComplete() },
                )
            emitter.setDisposable(completion)
            if (emitter.isDisposed) return@create
            characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
            characteristic.setValue(value)
            if (!bluetoothGatt.writeCharacteristic(characteristic)) {
                emitter.tryOnError(
                    BleGattCannotStartException(BleGattOperationType.CHARACTERISTIC_WRITE),
                )
            }
        }
            .take(1)
            .subscribeOn(bluetoothInteractionScheduler)
    }
}

fun RxBleConnection.resolveCharacteristic(
    uuid: UUID,
    instanceId: Int,
): Single<BluetoothGattCharacteristic> =
    discoverServices().flatMap { services ->
        Single.just(
            services.bluetoothGattServices.flatMap { service ->
                service.characteristics.filter {
                    it.uuid == uuid && it.instanceId == instanceId
                }
            }.single(),
        )
    }

fun RxBleConnection.writeCharWithResponse(
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
): Single<ByteArray> {
    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
    return writeCharacteristic(characteristic, value)
}

fun RxBleConnection.writeCharWithoutResponse(
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
): Single<ByteArray> {
    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
    return writeCharacteristic(characteristic, value)
}

/**
 * Keeps one Dart call and one characteristic resolution while advancing only
 * after native write completion or cumulative Object progress. Android can
 * reject the second back-to-back write command as GATT busy even when the
 * first call returned true. The per-packet low-priority queue also lets
 * normal-priority control overtake the remaining bulk data.
 */
fun RxBleConnection.writeCharWithoutResponseBatch(
    characteristic: BluetoothGattCharacteristic,
    values: List<ByteArray>,
): Completable =
    Observable.fromIterable(values)
        .concatMapSingle { value ->
            queue(
                LowPriorityCharacteristicWriteOperation(characteristic, value),
                Priority.LOW,
            ).firstOrError()
        }
        .ignoreElements()
