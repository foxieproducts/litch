package com.signify.hue.flutterreactiveble

import com.polidea.rxandroidble2.exceptions.BleDisconnectedException
import com.polidea.rxandroidble2.exceptions.BleGattException
import io.mockk.mockk
import io.reactivex.exceptions.UndeliverableException
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assertions.assertTrue
import org.junit.jupiter.api.Test

class BleRxErrorPolicyTest {
    @Test
    fun `expected disconnect and GATT teardown errors are contained`() {
        val disconnected = mockk<BleDisconnectedException>()
        val gatt = mockk<BleGattException>()
        assertTrue(BleRxErrorPolicy.isExpectedTeardown(disconnected))
        assertTrue(BleRxErrorPolicy.isExpectedTeardown(UndeliverableException(disconnected)))
        assertTrue(BleRxErrorPolicy.isExpectedTeardown(UndeliverableException(gatt)))
    }

    @Test
    fun `programming failures remain fatal`() {
        assertFalse(BleRxErrorPolicy.isExpectedTeardown(IllegalStateException("bug")))
        assertFalse(
            BleRxErrorPolicy.isExpectedTeardown(
                UndeliverableException(IllegalArgumentException("bug")),
            ),
        )
    }
}
