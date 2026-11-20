package com.signify.hue.flutterreactiveble.ble

import android.os.SystemClock
import com.google.common.truth.Truth.assertThat
import com.polidea.rxandroidble2.RxBleConnection
import com.polidea.rxandroidble2.RxBleDevice
import com.signify.hue.flutterreactiveble.model.ConnectionState
import com.signify.hue.flutterreactiveble.utils.Duration
import io.mockk.MockKAnnotations
import io.mockk.every
import io.mockk.impl.annotations.MockK
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import io.mockk.verify
import io.reactivex.Observable
import io.reactivex.subjects.BehaviorSubject
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.DisplayName
import org.junit.jupiter.api.Nested
import org.junit.jupiter.api.Test
import java.lang.Exception
import java.util.concurrent.TimeUnit

@DisplayName("DeviceConnector unit tests")
class DeviceConnectorTest {
    @MockK
    private lateinit var connection: RxBleConnection

    @MockK
    private lateinit var device: RxBleDevice

    @MockK
    private lateinit var connectionQueue: ConnectionQueue

    @MockK
    private lateinit var updateListener: (update: ConnectionUpdate) -> Unit

    private lateinit var sut: DeviceConnector
    private lateinit var subject: BehaviorSubject<List<String>>
    private lateinit var nativeStates: BehaviorSubject<RxBleConnection.RxBleConnectionState>
    private val deviceId = "123"
    private var elapsedMillis = 10_000L

    @BeforeEach
    fun setup() {
        MockKAnnotations.init(this)
        mockkStatic(SystemClock::class)
        every { SystemClock.elapsedRealtime() }.answers { elapsedMillis }
        subject = BehaviorSubject.create()
        nativeStates = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.DISCONNECTED)
        every { device.connectionState }.answers { nativeStates.value!! }
        every { device.observeConnectionStateChanges() }.returns(nativeStates.skip(1))
        every { device.macAddress }.returns(deviceId)
        every { updateListener.invoke(allAny()) }.returns(Unit)

        every { connectionQueue.addToQueue(any()) }.returns(Unit)
        every { connectionQueue.observeQueue() }.returns(subject)
        every { connectionQueue.removeFromQueue(any()) }.returns(Unit)

        subject.onNext(listOf(device.macAddress))
        sut = DeviceConnector(device, Duration(0L, TimeUnit.MILLISECONDS), updateListener, connectionQueue)
    }

    private fun connectedNativeSource(): Observable<RxBleConnection> =
        Observable.defer {
            nativeStates.onNext(RxBleConnection.RxBleConnectionState.CONNECTING)
            nativeStates.onNext(RxBleConnection.RxBleConnectionState.CONNECTED)
            Observable.just(connection).concatWith(Observable.never())
        }.doOnDispose { publishNativeCleanup() }

    private fun failedNativeSource(message: String): Observable<RxBleConnection> =
        Observable.error<RxBleConnection>(Exception(message)).doFinally { publishNativeCleanup() }

    private fun publishNativeCleanup() {
        nativeStates.onNext(RxBleConnection.RxBleConnectionState.DISCONNECTING)
        nativeStates.onNext(RxBleConnection.RxBleConnectionState.DISCONNECTED)
    }

    @Test
    @DisplayName("Disconnect completion follows delayed native disposal")
    fun disconnectCompletionFollowsDisposal() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            // A published connection starts the native minimum-age timer.
            every { device.establishConnection(any()) }.returns(connectedNativeSource())
            sut.connection.test()
            var completed = false
            sut.disconnectDevice(deviceId) {
                assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
                completed = true
            }
            assertThat(completed).isFalse()
            scheduler.advanceTimeBy(200, TimeUnit.MILLISECONDS)
            assertThat(completed).isTrue()
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    fun disconnectWaitUsesRemainingElapsedAgeWhenWallTimeDiverges() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            every { device.establishConnection(any()) }.returns(connectedNativeSource())
            sut.connection.test()
            elapsedMillis += 150
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            scheduler.advanceTimeBy(49, TimeUnit.MILLISECONDS)
            assertThat(sut.connectionDisposable!!.isDisposed).isFalse()
            assertThat(completions).isEqualTo(0)
            scheduler.advanceTimeBy(1, TimeUnit.MILLISECONDS)
            assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
            assertThat(completions).isEqualTo(1)
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    fun matureConnectionDisconnectDoesNotRestartMinimumAgeTimer() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            every { device.establishConnection(any()) }.returns(connectedNativeSource())
            sut.connection.test()
            elapsedMillis += 200
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
            assertThat(completions).isEqualTo(1)
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    @DisplayName("Disconnect completion waits for native disconnected state")
    fun disconnectCompletionWaitsForNativeState() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        val nativeStates = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.CONNECTED)
        var nativeState = RxBleConnection.RxBleConnectionState.CONNECTED
        every { device.connectionState }.answers { nativeState }
        every { device.observeConnectionStateChanges() }.returns(nativeStates)
        every { device.establishConnection(any()) }.returns(
            Observable.just(connection).concatWith(Observable.never()),
        )
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            sut.connection.test()
            assertThat(sut.connectionDisposable!!.isDisposed).isFalse()
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
            // RxAndroidBle queues radio teardown after subscription disposal.
            // The old link still owns its negotiated session until this callback.
            assertThat(nativeState).isEqualTo(RxBleConnection.RxBleConnectionState.CONNECTED)
            assertThat(completions).isEqualTo(0)
            verify(exactly = 0) {
                updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.DISCONNECTED.code))
            }
            nativeState = RxBleConnection.RxBleConnectionState.DISCONNECTED
            nativeStates.onNext(nativeState)
            assertThat(completions).isEqualTo(1)
            verify(exactly = 1) {
                updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.DISCONNECTED.code))
            }
        } finally {
            nativeState = RxBleConnection.RxBleConnectionState.DISCONNECTED
            nativeStates.onNext(nativeState)
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    fun disconnectWithoutStartingConnectionCompletesOnce() {
        every { device.observeConnectionStateChanges() }.returns(
            Observable.just(RxBleConnection.RxBleConnectionState.DISCONNECTED),
        )
        var completions = 0
        sut.disconnectDevice(deviceId) { completions++ }
        sut.disconnectDevice(deviceId) { completions++ }
        assertThat(completions).isEqualTo(2)
        verify(exactly = 0) { device.establishConnection(any()) }
        verify(exactly = 1) {
            updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.DISCONNECTED.code))
        }
    }

    @Test
    fun publishedCancellationStillRunsQueuedSdkAdmissionAndCleanup() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        val states = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.DISCONNECTED)
        every { device.connectionState }.answers { states.value!! }
        every { device.observeConnectionStateChanges() }.returns(states.skip(1))
        var admissions = 0
        var cleanupRequests = 0
        every { device.establishConnection(any()) }.returns(
            Observable.defer {
                admissions++
                Observable.never<RxBleConnection>().doOnDispose { cleanupRequests++ }
            }.subscribeOn(scheduler).unsubscribeOn(scheduler),
        )
        sut.connection.test()
        assertThat(admissions).isEqualTo(0)
        var completions = 0
        sut.disconnectDevice(deviceId) { completions++ }
        assertThat(admissions).isEqualTo(0)
        assertThat(completions).isEqualTo(0)
        scheduler.triggerActions()
        // RxAndroidBle uses this exact single-scheduler operator order. The
        // published disposable cannot cancel before SubscribeTask was queued.
        assertThat(admissions).isEqualTo(1)
        assertThat(cleanupRequests).isEqualTo(1)
        assertThat(completions).isEqualTo(0)
        states.onNext(RxBleConnection.RxBleConnectionState.DISCONNECTING)
        states.onNext(RxBleConnection.RxBleConnectionState.DISCONNECTED)
        assertThat(completions).isEqualTo(1)
    }

    @Test
    fun admittedDisconnectedSnapshotCannotCompleteBeforeNativeCleanup() {
        val states = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.DISCONNECTED)
        var state = RxBleConnection.RxBleConnectionState.DISCONNECTED
        every { device.connectionState }.answers { state }
        every { device.observeConnectionStateChanges() }.returns(states)
        every { device.establishConnection(any()) }.returns(
            Observable.never<RxBleConnection>().doOnDispose {
                // The native operation was admitted before CONNECTING was
                // published; unsubscribe and final cleanup remain deferred.
                state = RxBleConnection.RxBleConnectionState.CONNECTING
                states.onNext(state)
            },
        )
        try {
            sut.connection.test()
            verify(exactly = 1) { device.establishConnection(any()) }
            assertThat(state).isEqualTo(RxBleConnection.RxBleConnectionState.DISCONNECTED)
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            assertThat(state).isEqualTo(RxBleConnection.RxBleConnectionState.CONNECTING)
            assertThat(completions).isEqualTo(0)
            state = RxBleConnection.RxBleConnectionState.DISCONNECTED
            states.onNext(state)
            assertThat(completions).isEqualTo(1)
        } finally {
            state = RxBleConnection.RxBleConnectionState.DISCONNECTED
            states.onNext(state)
        }
    }

    @Test
    fun queuedDisconnectRemovesCanceledAdmission() {
        subject.onNext(listOf("other", deviceId))
        every { device.observeConnectionStateChanges() }.returns(
            Observable.just(RxBleConnection.RxBleConnectionState.DISCONNECTED),
        )
        sut.connection.test()
        var completions = 0
        sut.disconnectDevice(deviceId) { completions++ }
        assertThat(completions).isEqualTo(1)
        verify(exactly = 0) { device.establishConnection(any()) }
        verify(exactly = 1) { connectionQueue.removeFromQueue(deviceId) }
    }

    @Test
    fun disconnectWhileConnectingWaitsForNativeCleanup() {
        val states = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.CONNECTING)
        var state = RxBleConnection.RxBleConnectionState.CONNECTING
        every { device.connectionState }.answers { state }
        every { device.observeConnectionStateChanges() }.returns(states)
        every { device.establishConnection(any()) }.returns(Observable.never())
        sut.connection.test()
        var completions = 0
        sut.disconnectDevice(deviceId) { completions++ }
        sut.disconnectDevice(deviceId) { completions++ }
        assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
        assertThat(completions).isEqualTo(0)
        state = RxBleConnection.RxBleConnectionState.DISCONNECTED
        states.onNext(state)
        states.onNext(state)
        assertThat(completions).isEqualTo(2)
        verify(exactly = 1) {
            updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.DISCONNECTED.code))
        }
    }

    @Test
    fun synchronousNativeDisconnectCompletesAfterSubjectDisposal() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        val states = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.CONNECTED)
        var state = RxBleConnection.RxBleConnectionState.CONNECTED
        every { device.connectionState }.answers { state }
        every { device.observeConnectionStateChanges() }.returns(states)
        every { device.establishConnection(any()) }.returns(
            Observable.just(connection).concatWith(Observable.never()).doOnDispose {
                state = RxBleConnection.RxBleConnectionState.DISCONNECTED
                states.onNext(state)
            },
        )
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            val observer = sut.connection.test()
            var completions = 0
            sut.disconnectDevice(deviceId) {
                observer.assertComplete()
                completions++
            }
            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            assertThat(completions).isEqualTo(1)
            states.onNext(state)
            assertThat(completions).isEqualTo(1)
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    fun nativeDisconnectBeforeMinimumAgeStillWaitsForDisposal() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        val states = BehaviorSubject.createDefault(RxBleConnection.RxBleConnectionState.CONNECTED)
        var state = RxBleConnection.RxBleConnectionState.CONNECTED
        every { device.connectionState }.answers { state }
        every { device.observeConnectionStateChanges() }.returns(states)
        every { device.establishConnection(any()) }.returns(
            Observable.just(connection).concatWith(Observable.never()),
        )
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            sut.connection.test()
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            state = RxBleConnection.RxBleConnectionState.DISCONNECTED
            states.onNext(state)
            assertThat(completions).isEqualTo(0)
            assertThat(sut.connectionDisposable!!.isDisposed).isFalse()
            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            assertThat(completions).isEqualTo(1)
            assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @Test
    fun failedEstablishmentDisconnectCompletesAfterCleanup() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        every { device.establishConnection(any()) }.returns(failedNativeSource("failed"))
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            sut.connection.test()
            var completions = 0
            sut.disconnectDevice(deviceId) { completions++ }
            scheduler.advanceTimeBy(1, TimeUnit.SECONDS)
            assertThat(completions).isEqualTo(1)
            assertThat(sut.connectionDisposable!!.isDisposed).isTrue()
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }

    @AfterEach
    fun teardown() {
        sut.disconnectDevice(deviceId)
        unmockkStatic(SystemClock::class)
    }

    @Nested
    @DisplayName("Successfull connection")
    inner class SuccesfullConnectionTest {
        @BeforeEach
        fun setup() {
            every { device.establishConnection(any()) }.returns(connectedNativeSource())
        }

        @Test
        @DisplayName("Add device to queue")
        fun addDeviceToQueue() {
            sut.connection.test()
            verify(exactly = 1) { connectionQueue.addToQueue(deviceId) }
        }

        @Test
        @DisplayName("Connects to device only once")
        fun connectOnlyOnce() {
            sut.connection.test()

            verify(exactly = 1) { device.establishConnection(allAny()) }
        }

        @Test
        @DisplayName("Does not connect when device is not in queue")
        fun doNotConnectWhenInQueue() {
            subject.onNext(listOf("Otherdevice", device.macAddress))
            sut.connection.test()

            verify(exactly = 0) { device.establishConnection(allAny()) }
        }

        @Test
        @DisplayName("Updates listeners in case of succesfull connection")
        fun updatesWhenSuccess() {
            sut.connection.test()

            verify(exactly = 1) { updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.CONNECTED.code)) }
        }

        @Test
        @DisplayName("Connects to device only once in casealready connected")
        fun connectOnlyOnceAlreadyConnected() {
            sut.connection.test()
            every { device.connectionState }.returns(RxBleConnection.RxBleConnectionState.CONNECTED)

            sut.connection.test()

            verify(exactly = 1) { device.establishConnection(allAny()) }
        }

        @Test
        @DisplayName("Remove device from queue")
        fun removeDeviceFromQueue() {
            sut.connection.test()
            verify(exactly = 1) { connectionQueue.removeFromQueue(deviceId) }
        }
    }

    @Nested
    @DisplayName("Failed connection")
    inner class NotSuccesfullConnectionTest {
        private val errorMessage = "aaa"

        @BeforeEach
        fun setup() {
            every { device.establishConnection(any()) }.returns(failedNativeSource(errorMessage))
        }

        @Test
        @DisplayName("Returns connection failed when connecting failed")
        fun failedToConnect() {
            val observer = sut.connection.test()

            assertThat(observer.values().first()).isInstanceOf(EstablishConnectionFailure::class.java)
        }

        @Test
        @DisplayName("Updates listeners in case of failed connection")
        fun updatesWhenFailed() {
            sut.connection.test()

            verify(exactly = 1) { updateListener.invoke(ConnectionUpdateError(deviceId, errorMessage)) }
        }

        @Test
        @DisplayName("Remove device from queue")
        fun removeDeviceFromQueue() {
            sut.connection.test()
            verify(exactly = 1) { connectionQueue.removeFromQueue(deviceId) }
        }
    }

    @Test
    @DisplayName("Dispose observable in case disconnecting")
    fun disposeOnDisconnect() {
        val scheduler = io.reactivex.schedulers.TestScheduler()
        every { device.establishConnection(any()) }.returns(connectedNativeSource())
        io.reactivex.plugins.RxJavaPlugins.setComputationSchedulerHandler { scheduler }
        try {
            sut.connection.test()
            sut.disconnectDevice(deviceId)
            scheduler.advanceTimeBy(200, TimeUnit.MILLISECONDS)
            assertThat(sut.connectionDisposable?.isDisposed).isTrue()
            verify(exactly = 1) {
                updateListener.invoke(ConnectionUpdateSuccess(deviceId, ConnectionState.DISCONNECTED.code))
            }
        } finally {
            io.reactivex.plugins.RxJavaPlugins.reset()
        }
    }
}
