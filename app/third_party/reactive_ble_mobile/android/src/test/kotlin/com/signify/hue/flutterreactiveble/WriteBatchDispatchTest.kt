package com.signify.hue.flutterreactiveble

import com.signify.hue.flutterreactiveble.ble.BleClient
import com.google.common.truth.Truth.assertThat
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.mockk.every
import io.mockk.mockk
import io.mockk.verify
import io.reactivex.Completable
import io.reactivex.android.plugins.RxAndroidPlugins
import io.reactivex.schedulers.Schedulers
import org.junit.jupiter.api.AfterEach
import org.junit.jupiter.api.BeforeEach
import org.junit.jupiter.api.Test
import java.util.UUID

class WriteBatchDispatchTest {
    private lateinit var ble: BleClient
    private lateinit var controller: PluginController

    @BeforeEach
    fun setup() {
        RxAndroidPlugins.setInitMainThreadSchedulerHandler { Schedulers.trampoline() }
        RxAndroidPlugins.setMainThreadSchedulerHandler { Schedulers.trampoline() }
        ble = mockk()
        controller = PluginController()
        val field = PluginController::class.java.getDeclaredField("bleClient")
        field.isAccessible = true
        field.set(controller, ble)
    }

    @AfterEach
    fun teardown() {
        RxAndroidPlugins.reset()
    }

    @Test
    fun `valid map dispatches one native batch and completes once`() {
        val uuid = UUID.randomUUID()
        val values = List(54) { index -> byteArrayOf(index.toByte()) }
        every {
            ble.writeCharacteristicWithoutResponseBatch(
                "board-1",
                uuid,
                11,
                any(),
            )
        } returns Completable.complete()
        val result = mockk<MethodChannel.Result>(relaxed = true)

        controller.execute(
            MethodCall(
                "writeCharacteristicWithoutResponseBatch",
                mapOf(
                    "deviceId" to "board-1",
                    "characteristicUuid" to uuid.toString(),
                    "characteristicInstanceId" to "11",
                    "values" to values,
                ),
            ),
            result,
        )

        verify(exactly = 1) {
            ble.writeCharacteristicWithoutResponseBatch(
                "board-1",
                uuid,
                11,
                match { actual ->
                    actual.map(ByteArray::toList) == values.map(ByteArray::toList)
                },
            )
        }
        verify(exactly = 1) { result.success(null) }
        verify(exactly = 0) { result.error(any(), any(), any()) }
    }

    @Test
    fun `oversized batch is rejected before BLE`() {
        val result = mockk<MethodChannel.Result>(relaxed = true)

        controller.execute(
            MethodCall(
                "writeCharacteristicWithoutResponseBatch",
                mapOf(
                    "deviceId" to "board-1",
                    "characteristicUuid" to UUID.randomUUID().toString(),
                    "characteristicInstanceId" to "11",
                    "values" to List(55) { byteArrayOf(1) },
                ),
            ),
            result,
        )

        verify(exactly = 0) {
            ble.writeCharacteristicWithoutResponseBatch(any(), any(), any(), any())
        }
        verify(exactly = 1) {
            result.error("invalid_write_batch", any(), null)
        }
    }

}
