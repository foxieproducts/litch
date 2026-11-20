package com.polidea.rxandroidble2.internal.serialization

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import com.google.common.truth.Truth.assertThat
import com.polidea.rxandroidble2.exceptions.BleDisconnectedException
import com.polidea.rxandroidble2.internal.connection.DisconnectionRouterOutput
import com.polidea.rxandroidble2.internal.connection.IllegalOperationChecker
import com.polidea.rxandroidble2.internal.connection.RxBleConnectionImpl
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import com.polidea.rxandroidble2.internal.operations.CharacteristicWriteOperation
import com.polidea.rxandroidble2.internal.operations.OperationsProvider
import com.polidea.rxandroidble2.internal.operations.TimeoutConfiguration
import com.polidea.rxandroidble2.internal.util.ByteAssociation
import com.polidea.rxandroidble2.internal.util.CharacteristicChangedEvent
import com.signify.hue.flutterreactiveble.ble.extensions.writeCharWithoutResponseBatch
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkClass
import io.mockk.slot
import io.reactivex.Completable
import io.reactivex.observers.TestObserver
import io.reactivex.schedulers.Schedulers
import io.reactivex.subjects.PublishSubject
import org.junit.jupiter.api.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

class NativeObjectWriteQueueTest {
    @Test
    fun `early native callback releases Object queue and priority Query before remaining thirty two packets`() {
        val executor = Executors.newSingleThreadExecutor()
        val callback = mockk<RxBleGattCallback>(relaxed = true)
        val bluetoothGatt = mockk<BluetoothGatt>()
        val characteristic = mockk<BluetoothGattCharacteristic>()
        val uuid = UUID.randomUUID()
        val writes = CopyOnWriteArrayList<Int>()
        val callbacks = PublishSubject.create<ByteAssociation<UUID>>()
        val notifications = PublishSubject.create<CharacteristicChangedEvent>()
        val value = slot<ByteArray>()
        val control = ByteArray(10).also {
            it[0] = 0x40
            it[1] = 12 // Shared Object Query, on the forward stream token.
            ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN).putInt(2, 3)
        }
        val queryResult = AtomicReference<TestObserver<ByteArray>>()
        val firstObjectEntered = CountDownLatch(1)
        val completionObservers = AtomicReference<Pair<Boolean, Boolean>>()
        every { characteristic.uuid } returns uuid
        every { bluetoothGatt.device } returns mockk {
            every { address } returns "00:00:00:00:00:01"
        }
        every { characteristic.setWriteType(any()) } returns Unit
        every { characteristic.setValue(capture(value)) } returns true
        every { callback.onCharacteristicWrite } returns callbacks
        every { callback.onCharacteristicChanged } returns notifications
        val router = mockk<DisconnectionRouterOutput>(relaxed = true)
        val queue = ConnectionOperationQueueImpl(
            "00:00:00:00:00:01", router, executor, Schedulers.trampoline(),
        )
        val operations = mockk<OperationsProvider>()
        val checker = mockk<IllegalOperationChecker>()
        every { checker.checkAnyPropertyMatches(characteristic, any()) } returns Completable.complete()
        // Package-private SDK construction only; the normal-priority native
        // write, connection and FIFO/priority queue all remain production code.
        val writeConstructor = CharacteristicWriteOperation::class.java.getDeclaredConstructor(
            RxBleGattCallback::class.java, BluetoothGatt::class.java,
            TimeoutConfiguration::class.java, BluetoothGattCharacteristic::class.java,
            ByteArray::class.java,
        ).apply { isAccessible = true }
        every { operations.provideWriteCharacteristic(characteristic, any()) } answers {
            writeConstructor.newInstance(
                callback, bluetoothGatt,
                TimeoutConfiguration(5, TimeUnit.SECONDS, Schedulers.computation()),
                characteristic, secondArg<ByteArray>(),
            )
        }
        // The SDK hides its shaded Provider from consumers' compile classpath.
        // Construct its public connection reflectively; unused collaborators
        // are mocks and its unused long-write builder provider remains null.
        val connectionConstructor = RxBleConnectionImpl::class.java.constructors.single()
        val dependencies = arrayOfNulls<Any>(11)
        dependencies[0] = queue
        dependencies[1] = callback
        dependencies[2] = bluetoothGatt
        for (index in 3..6) {
            dependencies[index] = mockkClass(connectionConstructor.parameterTypes[index].kotlin)
        }
        dependencies[7] = operations
        dependencies[9] = Schedulers.trampoline()
        dependencies[10] = checker
        val connection = connectionConstructor.newInstance(*dependencies) as RxBleConnectionImpl
        every { bluetoothGatt.writeCharacteristic(characteristic) } answers {
            val bytes = value.captured.copyOf()
            val sequence = if (bytes[0] == 0x44.toByte()) {
                ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getInt(6)
            } else -1
            writes.add(sequence)
            if (sequence == 83) {
                completionObservers.set(callbacks.hasObservers() to notifications.hasObservers())
                firstObjectEntered.countDown()
                queryResult.set(connection.writeCharacteristic(characteristic, control).test())
                // Other-direction traffic cannot acknowledge this Object.
                repeat(32) { index ->
                    val reverse = ByteArray(11).also {
                        it[0] = 0x44
                        ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN)
                            .putInt(2, 0x80000002.toInt()).putInt(6, index + 1)
                    }
                    notifications.onNext(CharacteristicChangedEvent(uuid, 1, reverse))
                }
            }
            // Controlled earliest legal delivery, during native entry rather
            // than after asObservable().test() has returned. No terminal fault.
            callbacks.onNext(ByteAssociation.create(uuid, bytes))
            true
        }
        val packets = (83..115).map { sequence ->
            ByteArray(20).also {
                it[0] = 0x44
                it[1] = 0x40
                ByteBuffer.wrap(it).order(ByteOrder.LITTLE_ENDIAN)
                    .putInt(2, 3).putInt(6, sequence)
            }
        }
        val result = connection.writeCharWithoutResponseBatch(characteristic, packets).test()
        try {
            assertThat(firstObjectEntered.await(1, TimeUnit.SECONDS)).isTrue()
            assertThat(completionObservers.get()).isEqualTo(true to true)
            result.awaitDone(1, TimeUnit.SECONDS)
                .withTag("writes=$writes; query_errors=${queryResult.get()?.errors()}")
                .assertComplete().assertNoErrors()
            queryResult.get().assertComplete().assertNoErrors()
            assertThat(writes).containsExactlyElementsIn(listOf(83, -1) + (84..115)).inOrder()
        } finally {
            result.dispose()
            queryResult.get()?.dispose()
            queue.terminate(BleDisconnectedException("00:00:00:00:00:01"))
            executor.shutdownNow()
            executor.awaitTermination(1, TimeUnit.SECONDS)
        }
    }
}
