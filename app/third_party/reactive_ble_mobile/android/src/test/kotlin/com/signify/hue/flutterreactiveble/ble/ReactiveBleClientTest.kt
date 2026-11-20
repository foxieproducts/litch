package com.signify.hue.flutterreactiveble.ble

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothDevice.BOND_BONDED
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.content.Context
import com.google.common.truth.Truth.assertThat
import com.polidea.rxandroidble2.RxBleClient
import com.polidea.rxandroidble2.RxBleConnection
import com.polidea.rxandroidble2.RxBleCustomOperation
import com.polidea.rxandroidble2.ConnectionParameters
import com.polidea.rxandroidble2.ConnectionSetup
import com.polidea.rxandroidble2.RxBleDevice
import com.polidea.rxandroidble2.RxBleDeviceServices
import com.polidea.rxandroidble2.exceptions.BleGattCallbackTimeoutException
import com.polidea.rxandroidble2.exceptions.BleGattCannotStartException
import com.polidea.rxandroidble2.exceptions.BleDisconnectedException
import com.polidea.rxandroidble2.internal.connection.RxBleGattCallback
import com.polidea.rxandroidble2.internal.connection.RxBleConnectionImpl
import com.polidea.rxandroidble2.internal.connection.DisconnectionRouterOutput
import com.polidea.rxandroidble2.internal.operations.MtuRequestOperation
import com.polidea.rxandroidble2.internal.operations.Operation
import com.polidea.rxandroidble2.internal.operations.TimeoutConfiguration
import com.polidea.rxandroidble2.internal.serialization.ConnectionOperationQueue
import com.polidea.rxandroidble2.internal.serialization.ConnectionOperationQueueImpl
import com.signify.hue.flutterreactiveble.ble.extensions.resolveCharacteristic
import com.signify.hue.flutterreactiveble.ble.extensions.writeCharWithResponse
import com.signify.hue.flutterreactiveble.ble.extensions.writeCharWithoutResponse
import com.signify.hue.flutterreactiveble.ble.extensions.writeCharWithoutResponseBatch
import com.signify.hue.flutterreactiveble.utils.Duration
import io.mockk.MockKAnnotations
import io.mockk.every
import io.mockk.impl.annotations.MockK
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.verify
import io.reactivex.Completable
import io.reactivex.Observable
import io.reactivex.Single
import io.reactivex.Scheduler
import io.reactivex.android.plugins.RxAndroidPlugins
import io.reactivex.disposables.Disposable
import io.reactivex.schedulers.Schedulers
import io.reactivex.schedulers.TestScheduler
import io.reactivex.subjects.BehaviorSubject
import io.reactivex.subjects.CompletableSubject
import io.reactivex.subjects.PublishSubject
import org.junit.After
import org.junit.Before
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.DisplayName
import org.junit.jupiter.api.Nested
import org.junit.jupiter.api.Test
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

private class BleClientForTesting(
    val bleClient: RxBleClient,
    appContext: Context,
    val deviceConnector: DeviceConnector,
) : ReactiveBleClient(
        appContext,
    ) {
    override fun initializeClient() {
        rxBleClient = bleClient
        activeConnections = mutableMapOf()
    }

    override fun createDeviceConnector(
        device: RxBleDevice,
        timeout: Duration,
    ): DeviceConnector = deviceConnector
}

@DisplayName("BleClient unit tests")
class ReactiveBleClientTest {
    @MockK
    private lateinit var context: Context

    @MockK
    private lateinit var rxBleClient: RxBleClient

    @MockK
    private lateinit var deviceConnector: DeviceConnector

    @MockK
    private lateinit var bleDevice: RxBleDevice

    @MockK
    private lateinit var bluetoothDevice: BluetoothDevice

    @MockK
    lateinit var rxConnection: RxBleConnection

    private lateinit var subject: BehaviorSubject<EstablishConnectionResult>

    private lateinit var sut: BleClientForTesting

    private val testTimeout = Duration(100L, TimeUnit.MILLISECONDS)

    private var testScheduler: TestScheduler? = null

    @Before
    fun before() {
        testScheduler = TestScheduler()
        // Set calls to AndroidSchedulers.mainThread() to use the test scheduler
        RxAndroidPlugins.setMainThreadSchedulerHandler { testScheduler }
    }

    @After
    fun after() {
        RxAndroidPlugins.reset()
    }

    @BeforeEach
    fun setup() {
        MockKAnnotations.init(this)
        mockkStatic("com.signify.hue.flutterreactiveble.ble.extensions.RxBleConnectionExtensionKt")
        subject = BehaviorSubject.create<EstablishConnectionResult>()

        sut = BleClientForTesting(rxBleClient, context, deviceConnector)
        sut.initializeClient()

        every { bleDevice.observeConnectionStateChanges() }.returns(Observable.just(RxBleConnection.RxBleConnectionState.CONNECTED))
        every { rxBleClient.getBleDevice(any()) }.returns(bleDevice)
        every { deviceConnector.connection }.returns(subject)

        subject.onNext(EstablishedConnection("test", rxConnection))
    }

    @AfterEach
    fun teardown() {
        subject.onComplete()
    }

    @DisplayName("Establishing a connection")
    @Nested
    inner class EstablishConnectionTest {
        @Test
        fun `should use deviceconnector when connecting to a device`() {
            sut.connectToDevice("test", testTimeout)

            verify(exactly = 1) { deviceConnector.connection }
        }
    }

    @Nested
    @DisplayName("Writing reading and subscribing to characteristics")
    inner class BleOperationsTest {
        @Test
        fun `should call readcharacteristic in case the connection is established`() {
            every {
                rxConnection.resolveCharacteristic(
                    any(),
                    any(),
                )
            }.returns(Single.just(BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)))

            sut.readCharacteristic("test", UUID.randomUUID(), 11).test()

            verify(exactly = 1) { rxConnection.readCharacteristic(any<BluetoothGattCharacteristic>()) }
        }

        @Test
        fun `should not call readcharacteristic in case the connection is not established`() {
            subject.onNext(EstablishConnectionFailure("test", "error"))

            val result = sut.readCharacteristic("test", UUID.randomUUID(), 11).test()

            assertThat(result.values().first()).isInstanceOf(CharOperationFailed::class.java)
            verify(exactly = 0) { rxConnection.readCharacteristic(any<BluetoothGattCharacteristic>()) }
        }

        @Test
        fun `should report failure in case reading characteristic fails`() {
            subject.onNext(EstablishConnectionFailure("test", "error"))

            val observable = sut.readCharacteristic("test", UUID.randomUUID(), 11).test()

            assertThat(observable.values().first()).isInstanceOf(CharOperationFailed::class.java)
        }

        @Test
        fun `should incorporate the value in case readcharacteristic succeeds`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE

            every {
                rxConnection.readCharacteristic(
                    any<BluetoothGattCharacteristic>(),
                )
            }.returns(Single.just(byteArrayOf(byteMin, byteMax)))
            every {
                rxConnection.resolveCharacteristic(
                    any(),
                    any(),
                )
            }.returns(Single.just(BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)))
            val observable =
                sut.readCharacteristic("test", UUID.randomUUID(), 11)
                    .map { result -> result as CharOperationSuccessful }.test()

            assertThat(observable.values().first().value).isEqualTo(listOf(byteMin, byteMax))
        }

        @Test
        fun `should call writecharacteristicResponse in case the connection is established`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)

            every {
                rxConnection.resolveCharacteristic(
                    any(),
                    any(),
                )
            }.returns(Single.just(BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)))
            sut.writeCharacteristicWithResponse("test", UUID.randomUUID(), 11, bytes).test()

            verify(exactly = 1) { rxConnection.writeCharWithResponse(any(), any()) }
        }

        @Test
        fun `should not call writecharacteristicWithoutResponse in case the connection is established`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)

            sut.writeCharacteristicWithResponse("test", UUID.randomUUID(), 11, bytes).test()

            verify(exactly = 0) { rxConnection.writeCharWithoutResponse(any(), any()) }
        }

        @Test
        fun `should not call writecharacteristicResponse in case the connection is not established`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)
            subject.onNext(EstablishConnectionFailure("test", "error"))

            val result = sut.writeCharacteristicWithResponse("test", UUID.randomUUID(), 11, bytes).test()
            assertThat(result.values().first()).isInstanceOf(CharOperationFailed::class.java)

            verify(exactly = 0) { rxConnection.writeCharWithResponse(any(), any()) }
        }

        @Test
        fun `should call writecharacteristicWithoutResponse in case the connection is established`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)

            every {
                rxConnection.resolveCharacteristic(
                    any(),
                    any(),
                )
            }.returns(Single.just(BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)))
            sut.writeCharacteristicWithoutResponse("test", UUID.randomUUID(), 11, bytes).test()

            verify(exactly = 1) { rxConnection.writeCharWithoutResponse(any(), any()) }
        }

        @Test
        fun `batch resolves once and enters the native low priority writer once`() {
            val characteristicId = UUID.randomUUID()
            val characteristic = BluetoothGattCharacteristic(characteristicId, 0, 0)
            val values = listOf(byteArrayOf(1, 2), byteArrayOf(3), byteArrayOf(4, 5))
            every {
                rxConnection.resolveCharacteristic(any(), any())
            }.returns(Single.just(characteristic))
            every {
                rxConnection.writeCharWithoutResponseBatch(characteristic, values)
            }.returns(Completable.complete())

            sut.writeCharacteristicWithoutResponseBatch(
                "test",
                characteristicId,
                11,
                values,
            ).test().assertComplete().assertNoErrors()

            verify(exactly = 1) { rxConnection.resolveCharacteristic(characteristicId, 11) }
            verify(exactly = 1) {
                rxConnection.writeCharWithoutResponseBatch(characteristic, values)
            }
            verify(exactly = 0) { rxConnection.writeCharWithoutResponse(any(), any()) }
        }

        @Test
        fun `should not call writecharacteristicWithoutResponse in case the connection is not established`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)
            subject.onNext(EstablishConnectionFailure("test", "error"))

            val result = sut.writeCharacteristicWithoutResponse("test", UUID.randomUUID(), 11, bytes).test()

            assertThat(result.values().first()).isInstanceOf(CharOperationFailed::class.java)
            verify(exactly = 0) { rxConnection.writeCharWithoutResponse(any(), any()) }
        }

        @Test
        fun `should report failure in case writing characteristic fails`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)

            subject.onNext(EstablishConnectionFailure("test", "error"))

            val observable = sut.writeCharacteristicWithResponse("test", UUID.randomUUID(), 11, bytes).test()

            assertThat(observable.values().first()).isInstanceOf(CharOperationFailed::class.java)
        }

        @Test
        fun `should incorporate the value in case writecharacteristic succeeds`() {
            val byteMin = Byte.MIN_VALUE
            val byteMax = Byte.MAX_VALUE
            val bytes = byteArrayOf(byteMin, byteMax)

            every { rxConnection.writeCharWithResponse(any(), any()) }.returns(Single.just(byteArrayOf(byteMin, byteMax)))
            every {
                rxConnection.resolveCharacteristic(
                    any(),
                    any(),
                )
            }.returns(Single.just(BluetoothGattCharacteristic(UUID.randomUUID(), 0, 0)))
            val observable =
                sut.writeCharacteristicWithResponse("test", UUID.randomUUID(), 11, bytes)
                    .map { result -> result as CharOperationSuccessful }.test()

            assertThat(observable.values().first().value).isEqualTo(bytes.toList())
        }
    }

    @Nested
    @DisplayName("Negotiate mtu")
    inner class NegotiateMtuTest {
        private fun useSdkMtuRequest(
            scheduler: Scheduler,
            callbacks: Observable<Int>,
            nativeQueue: ConnectionOperationQueue? = null,
            onQueueRelease: () -> Unit,
        ): BluetoothGatt {
            val gatt = mockk<BluetoothGatt>(relaxed = true)
            val callback = mockk<RxBleGattCallback>(relaxed = true)
            every { gatt.requestMtu(517) }.returns(true)
            every { callback.onMtuChanged }.returns(callbacks)
            every { rxConnection.mtu }.returns(23)
            val timeout = ConnectionSetup.Builder().build().operationTimeout
            // The pinned SDK constructor is package-private. Exercise its real
            // callback/deadline and queue release behavior instead of faking it.
            val constructor = MtuRequestOperation::class.java.getDeclaredConstructor(
                RxBleGattCallback::class.java,
                BluetoothGatt::class.java,
                TimeoutConfiguration::class.java,
                Int::class.javaPrimitiveType,
            )
            constructor.isAccessible = true
            val operation = constructor.newInstance(
                callback,
                gatt,
                TimeoutConfiguration(timeout.timeout, timeout.timeUnit, scheduler),
                517,
            )
            // Keep the same controlled callback on the old production route
            // so the deadline regression can also run against that candidate.
            every { rxConnection.requestMtu(517) }.answers {
                operation.run { onQueueRelease() }.firstOrError()
            }
            val queue = nativeQueue ?: mockk<ConnectionOperationQueue>()
            if (nativeQueue == null) {
                every { queue.queue(any<Operation<Int>>()) }.answers {
                    firstArg<Operation<Int>>().run { onQueueRelease() }
                }
            }
            // Use the real public queue wrapper. Unused constructor owners are
            // null; reflection stays in the test because their types are private.
            val sdkConnection = RxBleConnectionImpl::class.java.constructors.single()
                .newInstance(
                    queue, callback, gatt, null, null, null,
                    null, null, null, scheduler, null,
                ) as RxBleConnection
            every { rxConnection.queue(any<RxBleCustomOperation<Int>>()) }.answers {
                sdkConnection.queue(firstArg<RxBleCustomOperation<Int>>())
            }
            return gatt
        }

        @Test
        fun `missing MTU callback fails and releases actual SDK queue wrapper after five seconds`() {
            val scheduler = TestScheduler()
            val callbacks = PublishSubject.create<Int>()
            var queueReleases = 0
            useSdkMtuRequest(scheduler, callbacks) { queueReleases++ }

            val result = sut.negotiateMtuSize("test", 517).test()

            scheduler.advanceTimeBy(4, TimeUnit.SECONDS)
            result.assertNoValues().assertNoErrors().assertNotComplete()
            assertThat(queueReleases).isEqualTo(0)

            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            result.assertError(BleGattCallbackTimeoutException::class.java)
            assertThat(queueReleases).isEqualTo(1)
            assertThat(callbacks.hasObservers()).isFalse()
            callbacks.onNext(517)
            scheduler.advanceTimeBy(30, TimeUnit.SECONDS)
            result.assertNoValues()
            assertThat(queueReleases).isEqualTo(1)
            verify(exactly = 1) { rxConnection.queue(any<RxBleCustomOperation<Int>>()) }
        }

        @Test
        fun `canceling pending MTU wait still releases actual SDK queue wrapper at five seconds`() {
            val scheduler = TestScheduler()
            var queueReleases = 0
            useSdkMtuRequest(scheduler, Observable.never()) { queueReleases++ }
            val result = sut.negotiateMtuSize("test", 517).test()

            scheduler.advanceTimeBy(2, TimeUnit.SECONDS)
            result.dispose()
            scheduler.advanceTimeBy(2, TimeUnit.SECONDS)
            assertThat(queueReleases).isEqualTo(0)

            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            assertThat(queueReleases).isEqualTo(1)
            result.assertNoValues().assertNoErrors()
        }

        @Test
        fun `MTU deadline admits following operation through real SDK connection queue`() {
            val clock = TestScheduler()
            val scheduled = CountDownLatch(1)
            val scheduler = object : Scheduler() {
                override fun createWorker(): Worker {
                    val worker = clock.createWorker()
                    return object : Worker() {
                        override fun schedule(run: Runnable, delay: Long, unit: TimeUnit): Disposable {
                            val disposable = worker.schedule(run, delay, unit)
                            if (delay == 0L) scheduled.countDown()
                            return disposable
                        }
                        override fun dispose() = worker.dispose()
                        override fun isDisposed() = worker.isDisposed
                    }
                }
            }
            val executor = Executors.newSingleThreadExecutor()
            val constructor = ConnectionOperationQueueImpl::class.java.declaredConstructors.single()
            constructor.isAccessible = true
            val queue = constructor.newInstance(
                "test", mockk<DisconnectionRouterOutput>(), executor, Schedulers.trampoline(),
            ) as ConnectionOperationQueueImpl
            try {
                useSdkMtuRequest(scheduler, Observable.never(), nativeQueue = queue) {}
                val mtu = sut.negotiateMtuSize("test", 517).test()
                check(scheduled.await(1, TimeUnit.SECONDS))
                clock.triggerActions()
                val admitted = CountDownLatch(1)
                val following = rxConnection.queue(
                    RxBleCustomOperation<Int> { _, _, _ ->
                        Observable.fromCallable {
                            admitted.countDown()
                            1
                        }
                    },
                ).test()

                clock.advanceTimeBy(4, TimeUnit.SECONDS)
                assertThat(admitted.count).isEqualTo(1)
                clock.advanceTimeBy(1, TimeUnit.SECONDS)

                mtu.assertError(BleGattCallbackTimeoutException::class.java)
                check(admitted.await(1, TimeUnit.SECONDS))
                following.awaitDone(1, TimeUnit.SECONDS).assertValue(1).assertComplete()
            } finally {
                queue.terminate(BleDisconnectedException("test", 0))
                executor.shutdownNow()
                check(executor.awaitTermination(1, TimeUnit.SECONDS))
            }
        }

        @Test
        fun `delayed MTU callback completes negotiation and releases SDK queue immediately`() {
            val scheduler = TestScheduler()
            val callbacks = PublishSubject.create<Int>()
            var queueReleases = 0
            useSdkMtuRequest(scheduler, callbacks) { queueReleases++ }
            val result = sut.negotiateMtuSize("test", 517).test()
            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            result.assertNoValues()

            callbacks.onNext(517)

            result.assertValue { it is MtuNegotiateSuccessful && it.size == 517 }
                .assertComplete()
            assertThat(queueReleases).isEqualTo(1)
            scheduler.advanceTimeBy(30, TimeUnit.SECONDS)
            assertThat(queueReleases).isEqualTo(1)
        }

        @Test
        fun `MTU callback is observed before starting the platform request`() {
            val scheduler = TestScheduler()
            val callbacks = PublishSubject.create<Int>()
            var queueReleases = 0
            val gatt = useSdkMtuRequest(scheduler, callbacks) { queueReleases++ }
            every { gatt.requestMtu(517) }.answers {
                assertThat(callbacks.hasObservers()).isTrue()
                callbacks.onNext(247)
                true
            }

            val result = sut.negotiateMtuSize("test", 517).test()
            verify(exactly = 0) { gatt.requestMtu(any()) }
            scheduler.triggerActions()

            result.assertValue { it is MtuNegotiateSuccessful && it.size == 247 }
            assertThat(queueReleases).isEqualTo(1)
        }

        @Test
        fun `rejected platform MTU start fails with typed error and releases SDK queue`() {
            val scheduler = TestScheduler()
            var queueReleases = 0
            val gatt = useSdkMtuRequest(scheduler, Observable.never()) { queueReleases++ }
            every { gatt.requestMtu(517) }.returns(false)

            val result = sut.negotiateMtuSize("test", 517).test()
            scheduler.triggerActions()

            result.assertError(BleGattCannotStartException::class.java)
            assertThat(queueReleases).isEqualTo(1)
        }

        @Test
        fun `disconnect propagates unchanged and releases SDK queue before MTU deadline`() {
            val scheduler = TestScheduler()
            val callbacks = PublishSubject.create<Int>()
            var queueReleases = 0
            useSdkMtuRequest(scheduler, callbacks) { queueReleases++ }
            val result = sut.negotiateMtuSize("test", 517).test()
            scheduler.triggerActions()
            val disconnected = BleDisconnectedException("test", 0)

            callbacks.onError(disconnected)

            result.assertError(disconnected)
            assertThat(queueReleases).isEqualTo(1)
            scheduler.advanceTimeBy(30, TimeUnit.SECONDS)
            assertThat(queueReleases).isEqualTo(1)
        }

        @Test
        fun `already observed sufficient MTU skips another platform negotiation`() {
            val scheduler = TestScheduler()
            var queueReleases = 0
            val gatt = useSdkMtuRequest(scheduler, Observable.never()) { queueReleases++ }
            every { rxConnection.mtu }.returns(517)

            val result = sut.negotiateMtuSize("test", 517).test()

            result.assertValue { it is MtuNegotiateSuccessful && it.size == 517 }
            verify(exactly = 0) { gatt.requestMtu(any()) }
            verify(exactly = 0) { rxConnection.queue(any<RxBleCustomOperation<Int>>()) }
            assertThat(queueReleases).isEqualTo(0)
        }

        @Test
        fun `MTU observed while queued skips redundant request at native admission`() {
            val scheduler = TestScheduler()
            var queueReleases = 0
            val gatt = useSdkMtuRequest(scheduler, Observable.never()) { queueReleases++ }
            var observedMtu = 23
            every { rxConnection.mtu }.answers { observedMtu }

            val result = sut.negotiateMtuSize("test", 517).test()
            result.assertNoValues()
            observedMtu = 517
            scheduler.triggerActions()

            result.assertValue { it is MtuNegotiateSuccessful && it.size == 517 }
                .assertComplete()
            verify(exactly = 0) { gatt.requestMtu(any()) }
            verify(exactly = 1) { rxConnection.queue(any<RxBleCustomOperation<Int>>()) }
            assertThat(queueReleases).isEqualTo(1)
        }

        @Test
        fun `should return mtunegotiatefailed in case it fails`() {
            val mtuSize = 19
            subject.onNext(EstablishConnectionFailure("test", "error"))

            val result = sut.negotiateMtuSize("", mtuSize).test()

            assertThat(result.values().first()).isInstanceOf(MtuNegotiateFailed::class.java)
        }
    }

    @Nested
    @DisplayName("Read RSSI")
    inner class ReadRssiTest {
        @Test
        fun `should return RSSI in case it succeeds`() {
            val rssi = -42
            every { rxConnection.readRssi() }.returns(Single.just(rssi))

            val result = sut.readRssi("").test()

            assertThat(result.values().first()).isEqualTo(rssi)
        }

        @Test
        fun `should return error in case it fails`() {
            every { rxConnection.readRssi() }.returns(Single.error(IllegalStateException("boom")))

            val result = sut.readRssi("").test()

            assertThat(result.errors().first()).isInstanceOf(IllegalStateException::class.java)
        }
    }

    @Nested
    @DisplayName("Observe status")
    inner class ObserveBleStatusTest {
        private val currentStatus = RxBleClient.State.BLUETOOTH_NOT_ENABLED
        private val changedStatus = RxBleClient.State.READY

        @BeforeEach
        fun setup() {
            every { rxBleClient.observeStateChanges() }.returns(Observable.just(changedStatus))
            every { rxBleClient.state }.returns(currentStatus)
        }

        @Test
        fun `observes status changes`() {
            val result = sut.observeBleStatus().test()

            assertThat(result.values().last()).isEqualTo(BleStatus.READY)
            assertThat(result.values().count()).isEqualTo(2)
        }

        @Test
        fun `starts with current state`() {
            val result = sut.observeBleStatus().test()
            assertThat(result.values().count()).isEqualTo(2)
            assertThat(result.values().first()).isEqualTo(BleStatus.POWERED_OFF)
        }
    }

    @Nested
    @DisplayName("Change priority")
    inner class ChangePriorityTest {
        @Test
        fun `15 ms priority update cannot hold an accepted request ahead of full MTU`() = verifyPriorityQueue("slow")

        @Test
        fun `synchronous priority callback does not change SDK request acceptance`() = verifyPriorityQueue("synchronous")

        @Test
        fun `accepted priority without a callback releases real SDK queue immediately`() = verifyPriorityQueue("missing")

        @Test
        fun `priority start failure releases real SDK queue`() = verifyPriorityQueue("start failure")

        @Test
        fun `priority disconnect releases real SDK queue`() = verifyPriorityQueue("disconnect")

        @Test
        fun `cancelled priority observer cannot retain native queue after SDK acceptance`() = verifyPriorityQueue("cancel")

        private fun verifyPriorityQueue(ending: String) {
            val clock = TestScheduler()
            val scheduled = LinkedBlockingQueue<Unit>()
            val scheduler = object : Scheduler() {
                override fun createWorker(): Worker {
                    val worker = clock.createWorker()
                    return object : Worker() {
                        override fun schedule(run: Runnable, delay: Long, unit: TimeUnit): Disposable {
                            val disposable = worker.schedule(run, delay, unit)
                            if (delay == 0L) scheduled.offer(Unit)
                            return disposable
                        }
                        override fun dispose() = worker.dispose()
                        override fun isDisposed() = worker.isDisposed
                    }
                }
            }
            val executor = Executors.newSingleThreadExecutor()
            val constructor = ConnectionOperationQueueImpl::class.java.declaredConstructors.single()
            constructor.isAccessible = true
            val queue = constructor.newInstance(
                "test", mockk<DisconnectionRouterOutput>(), executor, Schedulers.trampoline(),
            ) as ConnectionOperationQueueImpl
            try {
                val updates = PublishSubject.create<ConnectionParameters>()
                val mtuUpdates = PublishSubject.create<Int>()
                val gatt = mockk<BluetoothGatt>(relaxed = true)
                val callback = mockk<RxBleGattCallback>(relaxed = true)
                val priorityStarted = CountDownLatch(1)
                every { gatt.requestConnectionPriority(any()) }.answers {
                    priorityStarted.countDown()
                    if (ending == "disconnect") throw BleDisconnectedException("test", 0)
                    if (ending == "synchronous") {
                        val fast = mockk<ConnectionParameters>()
                        every { fast.connectionInterval }.returns(6)
                        updates.onNext(fast)
                    }
                    ending != "start failure"
                }
                every { gatt.requestMtu(517) }.answers {
                    mtuUpdates.onNext(517)
                    true
                }
                every { callback.connectionParametersUpdates }.returns(updates)
                every { callback.onMtuChanged }.returns(mtuUpdates)
                every { rxConnection.mtu }.returns(23)
                val sdkConnection = RxBleConnectionImpl::class.java.constructors.single().newInstance(
                    queue, callback, gatt, null, null, null, null, null, null, scheduler, null,
                ) as RxBleConnection
                every { rxConnection.queue(any<RxBleCustomOperation<Any>>()) }.answers {
                    sdkConnection.queue(firstArg<RxBleCustomOperation<Any>>())
                }
                val result = sut.requestConnectionPriority("test", ConnectionPriority.HIGH_PERFORMACE).test()
                for (pass in 0 until 10) {
                    if (priorityStarted.count == 0L) break
                    scheduled.poll(100, TimeUnit.MILLISECONDS)
                    clock.triggerActions()
                }
                check(priorityStarted.count == 0L)
                val mtu = sut.negotiateMtuSize("test", 517).test()
                if (ending == "start failure" || ending == "disconnect") {
                    result.assertValue { it is RequestConnectionPriorityFailed }
                } else {
                    result.assertValue { it is RequestConnectionPrioritySuccess }
                    if (ending == "slow") {
                        val slow = mockk<ConnectionParameters>()
                        every { slow.connectionInterval }.returns(12)
                        updates.onNext(slow)
                    }
                    if (ending == "cancel") result.dispose()
                }
                // No virtual time elapses: completing Dart alone is insufficient.
                // The actual SDK owner must admit the following full-size MTU.
                check(scheduled.poll(1, TimeUnit.SECONDS) != null) {
                    "Priority SDK result left the native GATT queue held"
                }
                clock.triggerActions()
                mtu.awaitDone(1, TimeUnit.SECONDS)
                    .assertValue { it is MtuNegotiateSuccessful && it.size == 517 }
                assertThat(clock.now(TimeUnit.MILLISECONDS)).isEqualTo(0)
                assertThat(updates.hasObservers()).isFalse()
                verify(exactly = 1) { gatt.requestConnectionPriority(any()) }
                verify(exactly = 1) { gatt.requestMtu(517) }
            } finally {
                clock.advanceTimeBy(30, TimeUnit.SECONDS)
                queue.terminate(BleDisconnectedException("test", 0))
                executor.shutdownNow()
                clock.triggerActions()
                executor.awaitTermination(1, TimeUnit.SECONDS)
            }
        }

        @Test
        fun `returns prioritysuccess when completed`() {
            val completer = Completable.fromCallable { true }

            every { rxConnection.requestConnectionPriority(any(), any(), any()) }.returns(completer)
            val result = sut.requestConnectionPriority("", ConnectionPriority.BALANCED).test()
            assertThat(result.values().first()).isInstanceOf(RequestConnectionPrioritySuccess::class.java)
        }

        @Test
        fun `returns false when connectionfailed`() {
            subject.onNext(EstablishConnectionFailure("test", "error"))
            val result = sut.requestConnectionPriority("", ConnectionPriority.BALANCED).test()
            assertThat(result.values().first()).isInstanceOf(RequestConnectionPriorityFailed::class.java)
        }
    }

    @Nested
    @DisplayName("Discover services")
    inner class DiscoverServicesTest {
        @BeforeEach
        fun setup() {
            every { bleDevice.bluetoothDevice }.returns(bluetoothDevice)
            every { bluetoothDevice.bondState }.returns(BOND_BONDED)
        }

        @Test
        fun `It returns success in case services can be discovered`() {
            every { rxConnection.discoverServices() }.returns(Single.just(RxBleDeviceServices(listOf())))

            val result = sut.discoverServices("test").test()

            assertThat(result.values().first()).isInstanceOf(RxBleDeviceServices::class.java)
        }

        @Test
        fun `It returns failure when connectionfailed`() {
            subject.onNext(EstablishConnectionFailure("test", "error"))
            val result = sut.discoverServices("test").test()
            result.assertError(Exception::class.java)
        }
    }
}
