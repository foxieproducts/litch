package com.signify.hue.flutterreactiveble.ble.extensions

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import com.google.common.truth.Truth.assertThat
import com.polidea.rxandroidble2.RxBleConnection
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.internal.Priority
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import com.polidea.rxandroidble2.internal.util.ByteAssociation
import com.polidea.rxandroidble2.internal.util.CharacteristicChangedEvent
import io.mockk.every
import io.mockk.mockk
import io.mockk.slot
import io.mockk.verify
import io.reactivex.schedulers.Schedulers
import io.reactivex.subjects.PublishSubject
import org.junit.jupiter.api.Test
import java.util.UUID

class LowPriorityCharacteristicWriteBatchTest {
    @Test
    fun `operation ignores stale same-characteristic callbacks until its packet completes`() {
        val uuid = UUID.randomUUID()
        val characteristic = mockk<BluetoothGattCharacteristic>()
        val bluetoothGatt = mockk<BluetoothGatt>()
        val callback = mockk<RxBleGattCallback>()
        val writes = mutableListOf<ByteArray>()
        val pendingValue = slot<ByteArray>()
        val completions = PublishSubject.create<ByteAssociation<UUID>>()
        every { characteristic.uuid } returns uuid
        every { characteristic.setWriteType(any()) } returns Unit
        every { characteristic.setValue(capture(pendingValue)) } returns true
        every { callback.onCharacteristicWrite } returns completions
        every { bluetoothGatt.writeCharacteristic(characteristic) } answers {
            writes.add(pendingValue.captured.copyOf())
            true
        }
        val first = byteArrayOf(1, 2, 3)
        val second = byteArrayOf(4, 5)
        val operation = LowPriorityCharacteristicWriteOperation(characteristic, first)

        val result = operation
            .asObservable(bluetoothGatt, callback, Schedulers.trampoline())
            .test()

        assertThat(writes.map(ByteArray::toList)).containsExactly(first.toList()).inOrder()
        result.assertNotComplete()
        completions.onNext(ByteAssociation.create(uuid, byteArrayOf(99)))
        result.assertNotComplete()
        completions.onNext(ByteAssociation.create(uuid, first))
        result.assertComplete().assertNoErrors()
        verify {
            characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
        }

        val secondResult = LowPriorityCharacteristicWriteOperation(characteristic, second)
            .asObservable(bluetoothGatt, callback, Schedulers.trampoline())
            .test()
        assertThat(writes.map(ByteArray::toList))
            .containsExactly(first.toList(), second.toList())
            .inOrder()
        secondResult.assertNotComplete()
        completions.onNext(ByteAssociation.create(uuid, second))
        secondResult.assertComplete().assertNoErrors()
    }

    @Test
    fun `Object progress completes a write command when Android omits its callback`() {
        val characteristic = mockk<BluetoothGattCharacteristic>()
        val bluetoothGatt = mockk<BluetoothGatt>()
        val callback = mockk<RxBleGattCallback>()
        val callbacks = PublishSubject.create<ByteAssociation<UUID>>()
        val notifications = PublishSubject.create<CharacteristicChangedEvent>()
        val value = ByteArray(20).also {
            it[0] = 0x44
            it[2] = 18
            it[3] = 0x56
            it[4] = 0x34
            it[5] = 0x12
            it[6] = 7
        }
        val progress = ByteArray(20).also {
            it[0] = 0x40
            it[1] = 8
            value.copyInto(it, destinationOffset = 2, startIndex = 2, endIndex = 6)
            it[6] = 7
            it[10] = 7
            it[14] = 16
        }
        every { characteristic.uuid } returns UUID.randomUUID()
        every { characteristic.setWriteType(any()) } returns Unit
        every { characteristic.setValue(any<ByteArray>()) } returns true
        every { callback.onCharacteristicWrite } returns callbacks
        every { callback.onCharacteristicChanged } returns notifications
        every { bluetoothGatt.writeCharacteristic(characteristic) } returns true

        val result = LowPriorityCharacteristicWriteOperation(characteristic, value)
            .asObservable(bluetoothGatt, callback, Schedulers.trampoline())
            .test()

        result.assertNotComplete()
        notifications.onNext(
            CharacteristicChangedEvent(
                UUID.randomUUID(),
                1,
                progress.copyOf().also { it[10] = 6 },
            ),
        )
        result.assertNotComplete()
        notifications.onNext(
            CharacteristicChangedEvent(
                UUID.randomUUID(),
                1,
                progress,
            ),
        )
        result.assertComplete().assertNoErrors()
    }

    @Test
    fun `Object progress rejects previous session epoch`() {
        val value = ByteArray(20).also {
            it[0] = 0x44
            it[2] = 1
            it[6] = 1
        }
        val progress = ByteArray(20).also {
            it[0] = 0x40
            it[1] = 8
            it[2] = 1
            it[6] = 1
            it[10] = 1
        }
        assertThat(connectionSessionObjectProgressAcceptsPacket(value, progress)).isTrue()
        val oldValue = value.copyOf().also { it[0] = 0x34 }
        val oldProgress = progress.copyOf().also { it[0] = 0x30 }
        assertThat(connectionSessionObjectProgressAcceptsPacket(oldValue, oldProgress)).isFalse()
        assertThat(connectionSessionObjectProgressAcceptsPacket(value, oldProgress)).isFalse()
        assertThat(connectionSessionObjectProgressAcceptsPacket(oldValue, progress)).isFalse()
    }

    @Test
    fun `batch queues every packet at low priority without an artificial delay`() {
        val connection = mockk<RxBleConnection>()
        val characteristic = BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)
        val queued = mutableListOf<RxBleCustomOperation<Unit>>()
        val completions = mutableListOf<PublishSubject<Unit>>()
        every {
            connection.queue(capture(queued), Priority.LOW)
        } answers {
            PublishSubject.create<Unit>().also(completions::add)
        }

        val result = connection.writeCharWithoutResponseBatch(
            characteristic,
            listOf(
                byteArrayOf(0x44, 0, 1, 0, 0, 0, 1, 0, 0, 0),
                byteArrayOf(0x44, 0, 1, 0, 0, 0, 2, 0, 0, 0),
                byteArrayOf(0x44, 0, 1, 0, 0, 0, 3, 0, 0, 0),
            ),
        ).test()

        assertThat(queued).hasSize(1)
        result.assertNotComplete()
        completions[0].onNext(Unit)
        assertThat(queued).hasSize(2)
        completions[1].onNext(Unit)
        assertThat(queued).hasSize(3)
        completions[2].onNext(Unit)
        result.assertComplete().assertNoErrors()
        verify(exactly = 3) { connection.queue(any<RxBleCustomOperation<Unit>>(), Priority.LOW) }
    }

    @Test
    fun `operation fails immediately when Android rejects the write`() {
        val characteristic = mockk<BluetoothGattCharacteristic>()
        val bluetoothGatt = mockk<BluetoothGatt>()
        val callback = mockk<RxBleGattCallback>()
        every { characteristic.uuid } returns UUID.randomUUID()
        every { characteristic.setWriteType(any()) } returns Unit
        every { characteristic.setValue(any<ByteArray>()) } returns true
        every { callback.onCharacteristicWrite } returns
            PublishSubject.create<ByteAssociation<UUID>>()
        every { bluetoothGatt.writeCharacteristic(characteristic) } returns false

        LowPriorityCharacteristicWriteOperation(characteristic, byteArrayOf(1))
            .asObservable(bluetoothGatt, callback, Schedulers.trampoline())
            .test()
            .assertError(com.polidea.rxandroidble2.exceptions.BleGattCannotStartException::class.java)
    }
}
