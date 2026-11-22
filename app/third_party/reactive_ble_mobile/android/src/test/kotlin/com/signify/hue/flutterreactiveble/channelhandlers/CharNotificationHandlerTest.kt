package com.signify.hue.flutterreactiveble.channelhandlers

import android.bluetooth.BluetoothGattCharacteristic
import android.util.Log
import com.google.common.truth.Truth.assertThat
import com.google.protobuf.ByteString
import com.polidea.rxandroidble2.exceptions.BleCannotSetCharacteristicNotificationException
import com.polidea.rxandroidble2.exceptions.BleGattException
import com.polidea.rxandroidble2.exceptions.BleGattOperationType
import com.signify.hue.flutterreactiveble.ble.BleClient
import com.signify.hue.flutterreactiveble.converters.ProtobufMessageConverter
import com.signify.hue.flutterreactiveble.converters.UuidConverter
import io.flutter.plugin.common.EventChannel
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import io.reactivex.Observable
import io.reactivex.android.plugins.RxAndroidPlugins
import io.reactivex.schedulers.Schedulers
import io.reactivex.subjects.PublishSubject
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import java.util.UUID
import com.signify.hue.flutterreactiveble.ProtobufModel as pb

class CharNotificationHandlerTest {
    private val uuid = UUID.fromString("6f2a20d1-89df-4ac6-a9f2-53f6f7db0109")
    private val address = pb.CharacteristicAddress.newBuilder()
        .setDeviceId("AA:BB:CC:DD:EE:FF")
        .setCharacteristicUuid(pb.Uuid.newBuilder().setData(ByteString.copyFrom(UuidConverter().byteArrayFromUuid(uuid))))
        .setServiceInstanceId("7")
        .setCharacteristicInstanceId("25")
        .build()
    private val request = pb.NotifyCharacteristicRequest.newBuilder().setCharacteristic(address).build()
    private val stop = pb.NotifyNoMoreCharacteristicRequest.newBuilder().setCharacteristic(address).build()
    private val ble = mockk<BleClient>()
    private val handler = CharNotificationHandler(ble)
    private val sink = mockk<EventChannel.EventSink>()
    private val lines = mutableListOf<String>()
    private val forwarded = mutableListOf<ByteArray>()

    @BeforeEach
    fun setup() {
        RxAndroidPlugins.setInitMainThreadSchedulerHandler { Schedulers.trampoline() }
        RxAndroidPlugins.setMainThreadSchedulerHandler { Schedulers.trampoline() }
        mockkStatic(Log::class)
        every { Log.w(any<String>(), any<String>()) } answers {
            lines.add(secondArg())
            0
        }
        every { sink.success(any()) } answers { forwarded.add(firstArg()) }
        handler.onListen(null, sink)
    }

    @AfterEach
    fun teardown() {
        handler.unsubscribeFromNotifications(stop)
        handler.onCancel(null)
        RxAndroidPlugins.reset()
        unmockkStatic(Log::class)
    }

    private fun deliver(error: Throwable) {
        every { ble.setupNotification(address.deviceId, uuid, 25) } returns Observable.error(error)
        handler.subscribeToNotifications(request)
        val original = ProtobufMessageConverter().convertCharacteristicError(address, error.message ?: "")
        assertThat(forwarded).hasSize(1)
        assertThat(forwarded.single().contentEquals(original.toByteArray())).isTrue()
    }

    @Test
    fun `wrapped descriptor failure preserves status and original protobuf bytes`() {
        val characteristic = mockk<BluetoothGattCharacteristic>()
        every { characteristic.uuid } returns uuid
        val cause = BleGattException(133, BleGattOperationType.DESCRIPTOR_WRITE)
        deliver(BleCannotSetCharacteristicNotificationException(characteristic, 3, cause))
        assertThat(lines).hasSize(1)
        val line = lines.single()
        assertThat(line).contains("uuid=$uuid instance=25")
        assertThat(line).contains("rx_reason=3")
        assertThat(line).contains("BleCannotSetCharacteristicNotificationException>BleGattException")
        assertThat(line).contains("gatt_status=133 operation=descriptor_write")
        assertThat(line).doesNotContain(address.deviceId)
    }

    @Test
    fun `native admission without callback status remains unknown`() {
        deliver(BleGattException(BleGattException.UNKNOWN_STATUS, BleGattOperationType.DESCRIPTOR_WRITE))
        assertThat(lines.single()).contains("rx_reason=unknown")
        assertThat(lines.single()).contains("gatt_status=unknown operation=descriptor_write")
    }

    @Test
    fun `cyclic hostile causes stay bounded and never log messages`() {
        val first = RuntimeException("AA:BB:CC:DD:EE:FF secret-stack raw-packet bond-secret")
        val second = RuntimeException("other-private-message")
        first.initCause(second)
        second.initCause(first)
        deliver(first)
        val line = lines.single()
        assertThat(line.length).isAtMost(512)
        assertThat(line).contains("causes=RuntimeException>RuntimeException>RuntimeException>RuntimeException")
        for (secret in listOf(address.deviceId, "secret-stack", "raw-packet", "bond-secret", "private-message")) {
            assertThat(line).doesNotContain(secret)
        }
        assertThat(line).contains("gatt_status=unknown operation=unknown")
    }

    private class NotificationCauseWithAnIntentionallyLongNameToExerciseTheSixtyFourCharacterDiagnosticTokenLimit : RuntimeException()

    @Test
    fun `class tokens are limited independently of total line length`() {
        deliver(NotificationCauseWithAnIntentionallyLongNameToExerciseTheSixtyFourCharacterDiagnosticTokenLimit())
        val line = lines.single()
        val token = line.substringAfter("causes=").substringBefore(' ')
        assertThat(token.length).isEqualTo(64)
        assertThat(line.length).isAtMost(512)
    }

    @Test
    fun `diagnostic failure cannot change forwarding`() {
        every { Log.w(any<String>(), any<String>()) } throws IllegalStateException("logger unavailable")
        deliver(RuntimeException("original failure"))
    }

    @Test
    fun `successful subscription and explicit unsubscribe keep existing behavior`() {
        val packets = PublishSubject.create<ByteArray>()
        var cancelled = 0
        every { ble.setupNotification(address.deviceId, uuid, 25) } returns packets.doOnDispose { cancelled++ }
        handler.subscribeToNotifications(request)
        packets.onNext(byteArrayOf(1, 2, 3))
        val original = ProtobufMessageConverter().convertCharacteristicInfo(address, byteArrayOf(1, 2, 3))
        assertThat(forwarded.single().contentEquals(original.toByteArray())).isTrue()
        handler.unsubscribeFromNotifications(stop)
        assertThat(cancelled).isEqualTo(1)
        assertThat(lines).isEmpty()
    }
}
