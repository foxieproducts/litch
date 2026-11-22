package com.signify.hue.flutterreactiveble.ble

import android.content.Context
import com.google.common.truth.Truth.assertThat
import com.polidea.rxandroidble2.RxBleClient
import com.polidea.rxandroidble2.RxBleDevice
import com.signify.hue.flutterreactiveble.utils.Duration
import io.mockk.every
import io.mockk.mockk
import io.reactivex.subjects.BehaviorSubject
import org.junit.jupiter.api.Test
import java.util.concurrent.TimeUnit

class ReactiveBleClientReinitializationTest {
    @Test
    fun `deinitialize then initialize admits a fresh connection subscription`() {
        val native = mockk<RxBleClient>()
        val connector = mockk<DeviceConnector>()
        val peripheral = mockk<RxBleDevice>()
        every { native.getBleDevice(any()) } returns peripheral
        every { connector.disconnectDevice(any(), any()) } answers { secondArg<() -> Unit>()() }
        // Only native construction and SDK result delivery are supplied. The
        // production client's connection ownership/deinitialization is real.
        val client = object : ReactiveBleClient(mockk<Context>()) {
            override fun initializeClient() {
                rxBleClient = native
                activeConnections = mutableMapOf()
            }
            override fun createDeviceConnector(device: RxBleDevice, timeout: Duration) = connector
        }
        val old = BehaviorSubject.create<EstablishConnectionResult>()
        every { connector.connection } returns old
        client.initializeClient()
        client.connectToDevice("board-1", Duration(1, TimeUnit.SECONDS))
        assertThat(old.hasObservers()).isTrue()
        client.disconnectAllDevices()
        assertThat(old.hasObservers()).isFalse()

        val fresh = BehaviorSubject.create<EstablishConnectionResult>()
        every { connector.connection } returns fresh
        client.initializeClient()
        client.connectToDevice("board-1", Duration(1, TimeUnit.SECONDS))
        try {
            assertThat(fresh.hasObservers()).isTrue()
            assertThat(old.hasObservers()).isFalse()
        } finally {
            client.disconnectAllDevices()
        }
        assertThat(fresh.hasObservers()).isFalse()
    }
}
