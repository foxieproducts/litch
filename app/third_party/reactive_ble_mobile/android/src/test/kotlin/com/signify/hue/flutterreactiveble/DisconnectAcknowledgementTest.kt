package com.signify.hue.flutterreactiveble

import com.google.common.truth.Truth.assertThat
import com.signify.hue.flutterreactiveble.ble.BleClient
import com.signify.hue.flutterreactiveble.channelhandlers.DeviceConnectionHandler
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.mockk.*
import org.junit.jupiter.api.Test

class DisconnectAcknowledgementTest {
    @Test
    fun acknowledgementFollowsCleanupAndQueuedEvents() {
        val queue = mutableListOf<() -> Unit>()
        val observed = mutableListOf<String>()
        val ble = mockk<BleClient>()
        val completed = slot<() -> Unit>()
        every { ble.disconnectDeviceAndNotify("board-1", capture(completed)) } returns Unit
        val controller = PluginController { queue.add(it) }
        val field = PluginController::class.java.getDeclaredField("deviceConnectionHandler")
        field.isAccessible = true
        field.set(controller, DeviceConnectionHandler(ble))
        val result = mockk<MethodChannel.Result>()
        every { result.success(null) } answers { observed.add("ack") }
        val request = ProtobufModel.DisconnectFromDeviceRequest.newBuilder()
            .setDeviceId("board-1").build().toByteArray()
        controller.execute(MethodCall("disconnectFromDevice", request), result)
        verify(exactly = 0) { result.success(any()) }
        queue.add { observed.add("old-disconnect") }
        completed.captured.invoke()
        verify(exactly = 0) { result.success(any()) }
        queue.toList().forEach { it() }
        assertThat(observed).containsExactly("old-disconnect", "ack").inOrder()
        verify(exactly = 1) { result.success(null) }
    }
}
