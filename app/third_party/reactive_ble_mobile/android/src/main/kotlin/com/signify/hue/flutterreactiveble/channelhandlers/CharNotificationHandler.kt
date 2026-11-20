package com.signify.hue.flutterreactiveble.channelhandlers

import android.util.Log
import com.polidea.rxandroidble2.exceptions.BleCannotSetCharacteristicNotificationException
import com.polidea.rxandroidble2.exceptions.BleDisconnectedException
import com.polidea.rxandroidble2.exceptions.BleGattException
import com.polidea.rxandroidble2.exceptions.BleGattOperationType
import com.signify.hue.flutterreactiveble.converters.ProtobufMessageConverter
import com.signify.hue.flutterreactiveble.converters.UuidConverter
import io.flutter.plugin.common.EventChannel
import io.reactivex.android.schedulers.AndroidSchedulers
import io.reactivex.disposables.Disposable
import com.signify.hue.flutterreactiveble.ProtobufModel as pb

class CharNotificationHandler(private val bleClient: com.signify.hue.flutterreactiveble.ble.BleClient) :
    EventChannel.StreamHandler {
    private val uuidConverter = UuidConverter()
    private val protobufConverter = ProtobufMessageConverter()

    companion object {
        private const val MAX_DIAGNOSTIC_CAUSES = 4
        private const val MAX_DIAGNOSTIC_CLASS_CHARS = 64
        private const val MAX_DIAGNOSTIC_CHARS = 512
        private var charNotificationSink: EventChannel.EventSink? = null

        private val subscriptionMap = mutableMapOf<pb.CharacteristicAddress, Disposable>()
    }

    override fun onListen(
        objectSink: Any?,
        eventSink: EventChannel.EventSink?,
    ) {
        eventSink?.let {
            charNotificationSink = eventSink
        }
    }

    override fun onCancel(objectSink: Any?) {
        unsubscribeFromAllNotifications()
    }

    fun subscribeToNotifications(request: pb.NotifyCharacteristicRequest) {
        val charUuid =
            uuidConverter
                .uuidFromByteArray(request.characteristic.characteristicUuid.data.toByteArray())
        val subscription =
            bleClient.setupNotification(
                request.characteristic.deviceId,
                charUuid,
                request.characteristic.characteristicInstanceId.toInt(),
            )
                .observeOn(AndroidSchedulers.mainThread())
                .subscribe({ value ->
                    handleNotificationValue(request.characteristic, value)
                }, {
                    when (it) {
                        is BleDisconnectedException -> {
                            subscriptionMap.remove(request.characteristic)?.dispose()
                        }
                        else -> {
                            handleNotificationError(request.characteristic, it)
                        }
                    }
                })
        subscriptionMap[request.characteristic] = subscription
    }

    fun unsubscribeFromNotifications(request: pb.NotifyNoMoreCharacteristicRequest) {
        subscriptionMap.remove(request.characteristic)?.dispose()
    }

    fun addSingleReadToStream(charInfo: pb.CharacteristicValueInfo) {
        handleNotificationValue(charInfo.characteristic, charInfo.value.toByteArray())
    }

    fun addSingleErrorToStream(
        subscriptionRequest: pb.CharacteristicAddress,
        error: String,
    ) {
        val convertedMsg = protobufConverter.convertCharacteristicError(subscriptionRequest, error)
        charNotificationSink?.success(convertedMsg.toByteArray())
    }

    private fun unsubscribeFromAllNotifications() {
        charNotificationSink = null
        subscriptionMap.forEach { it.value.dispose() }
    }

    private fun handleNotificationValue(
        subscriptionRequest: pb.CharacteristicAddress,
        value: ByteArray,
    ) {
        val convertedMsg = protobufConverter.convertCharacteristicInfo(subscriptionRequest, value)
        charNotificationSink?.success(convertedMsg.toByteArray())
    }

    private fun handleNotificationError(
        subscriptionRequest: pb.CharacteristicAddress,
        error: Throwable,
    ) {
        // The existing wire error keeps only the outer message. Preserve safe
        // native evidence locally without letting diagnostics affect delivery.
        runCatching { Log.w("FlutterReactiveBle", notificationDiagnostic(subscriptionRequest, error)) }
        val convertedMsg =
            protobufConverter.convertCharacteristicError(subscriptionRequest, error.message ?: "")
        charNotificationSink?.success(convertedMsg.toByteArray())
    }

    private fun notificationDiagnostic(request: pb.CharacteristicAddress, error: Throwable): String {
        val uuid = uuidConverter.uuidFromByteArray(request.characteristicUuid.data.toByteArray())
        val instance = request.characteristicInstanceId.toInt()
        val classes = mutableListOf<String>()
        var current: Throwable? = error
        var reason: Int? = null
        var status = "unknown"
        var operation = "unknown"
        var foundGatt = false
        for (depth in 0 until MAX_DIAGNOSTIC_CAUSES) {
            val cause = current ?: break
            classes.add(cause.javaClass.simpleName
                .filter { it.isLetterOrDigit() || it == '_' || it == '$' }
                .take(MAX_DIAGNOSTIC_CLASS_CHARS).ifEmpty { "anonymous" })
            if (reason == null && cause is BleCannotSetCharacteristicNotificationException) {
                reason = cause.reason
            }
            if (!foundGatt && cause is BleGattException) {
                foundGatt = true
                status = if (cause.status == BleGattException.UNKNOWN_STATUS) "unknown" else "${cause.status}"
                operation = diagnosticOperation(cause.bleGattOperationType)
            }
            current = cause.cause
        }
        return ("ble_notification_setup uuid=$uuid instance=$instance rx_reason=${reason ?: "unknown"} " +
            "causes=${classes.joinToString(">")} gatt_status=$status operation=$operation truncated=${current != null}")
            .take(MAX_DIAGNOSTIC_CHARS)
    }

    private fun diagnosticOperation(operation: BleGattOperationType?): String = when (operation) {
        BleGattOperationType.CONNECTION_STATE -> "connection_state"
        BleGattOperationType.SERVICE_DISCOVERY -> "service_discovery"
        BleGattOperationType.CHARACTERISTIC_READ -> "characteristic_read"
        BleGattOperationType.CHARACTERISTIC_WRITE -> "characteristic_write"
        BleGattOperationType.CHARACTERISTIC_LONG_WRITE -> "characteristic_long_write"
        BleGattOperationType.CHARACTERISTIC_CHANGED -> "characteristic_changed"
        BleGattOperationType.DESCRIPTOR_READ -> "descriptor_read"
        BleGattOperationType.DESCRIPTOR_WRITE -> "descriptor_write"
        BleGattOperationType.RELIABLE_WRITE_COMPLETED -> "reliable_write_completed"
        BleGattOperationType.READ_RSSI -> "read_rssi"
        BleGattOperationType.ON_MTU_CHANGED -> "mtu_changed"
        BleGattOperationType.CONNECTION_PRIORITY_CHANGE -> "connection_priority_change"
        null -> "unknown"
        else -> "other"
    }
}
