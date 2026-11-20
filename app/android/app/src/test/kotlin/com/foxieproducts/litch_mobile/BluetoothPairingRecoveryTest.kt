package com.foxieproducts.litch_mobile

import org.junit.Assert.*
import org.junit.Test

class BluetoothPairingRecoveryTest {
    private val board = "AA:BB:CC:DD:EE:01"
    private val other = "AA:BB:CC:DD:EE:02"

    private class Os : PairingRecoveryPlatform {
        var supported = true
        var state = PairingRecoveryBond.BONDED
        var known = emptyList<PairingRecoveryAssociation>()
        var associationCallback: ((PairingRecoveryAssociation?, String) -> Unit)? = null
        var timer: (() -> Unit)? = null
        var removalAccepted = true
        var cleanupAccepted = true
        val removed = mutableListOf<Int>()
        val cleaned = mutableListOf<Int>()
        var cancelled = 0
        var bondAccepted = true
        val createdBonds = mutableListOf<String>()
        override fun available() = supported
        override fun bond(address: String) = state
        override fun createBond(address: String): Boolean { createdBonds.add(address); return bondAccepted }
        override fun associations() = known
        override fun associate(address: String, completion: (PairingRecoveryAssociation?, String) -> Unit) {
            associationCallback = completion
        }
        override fun cancelAssociation() { cancelled++ }
        override fun removeBond(associationId: Int): Boolean { removed.add(associationId); return removalAccepted }
        override fun disassociate(associationId: Int): Boolean { cleaned.add(associationId); return cleanupAccepted }
        override fun timeout(action: () -> Unit): () -> Unit { timer = action; return { timer = null } }
    }

    @Test fun normalBondStartsOnceForTheExactWatchedAttemptWithoutRepairEffects() {
        val os = Os().apply { state = PairingRecoveryBond.NONE }
        val owner = BluetoothPairingRecovery(os) {}
        assertEquals("failed", owner.startBond(board, 7, 1))
        owner.watch(board, 7)
        assertEquals("failed", owner.startBond(other, 7, 1))
        assertEquals("failed", owner.startBond(board, 6, 1))
        assertEquals("failed", owner.startBond(board, 7, 0))
        assertEquals("started", owner.startBond(board, 7, 1))
        os.state = PairingRecoveryBond.OTHER // OS bonding is still pending.
        assertEquals("started", owner.startBond(board, 7, 1))
        assertEquals(listOf(board), os.createdBonds)
        owner.watch(other, 8)
        assertEquals("failed", owner.startBond(board, 7, 2))
        assertEquals(listOf(board), os.createdBonds)
        assertTrue(os.removed.isEmpty())
        assertTrue(os.cleaned.isEmpty())
        assertNull(os.timer)
    }

    @Test fun normalBondFailureAndExistingKeysRequireFreshIntentInsteadOfAutomaticRepair() {
        val os = Os().apply { state = PairingRecoveryBond.NONE; bondAccepted = false }
        val owner = BluetoothPairingRecovery(os) {}
        owner.watch(board, 7)
        assertEquals("failed", owner.startBond(board, 7, 2))
        os.bondAccepted = true
        assertEquals("failed", owner.startBond(board, 7, 2))
        assertEquals("failed", owner.startBond(board, 7, 1))
        assertEquals(1, os.createdBonds.size)
        os.state = PairingRecoveryBond.BONDED
        assertEquals("alreadyBonded", owner.startBond(board, 7, 3))
        assertEquals(1, os.createdBonds.size)
        assertTrue(os.removed.isEmpty())
        assertTrue(os.cleaned.isEmpty())
    }

    @Test fun missingKeysOnlyIdentifyTheWatchedPeerAndGeneration() {
        val os = Os()
        val events = mutableListOf<PairingRecoveryTarget>()
        val owner = BluetoothPairingRecovery(os, events::add)
        owner.keyMissing(board)
        owner.watch(board, 7)
        owner.keyMissing(other)
        owner.keyMissing(board.lowercase())
        owner.watch(board, 8)
        owner.keyMissing(board)
        owner.watch(null, 9)
        owner.keyMissing(board)
        assertEquals(listOf(PairingRecoveryTarget(board, 7), PairingRecoveryTarget(board, 8)), events)
    }

    @Test fun repairWaitsForExactBondRemovalThenCleansItsTemporaryAssociation() {
        val os = Os()
        val owner = BluetoothPairingRecovery(os) {}
        val results = mutableListOf<String>()
        owner.watch(board, 7)
        owner.repair(board, 7, results::add)
        os.associationCallback!!(PairingRecoveryAssociation(42, board), "failed")
        assertEquals(listOf(42), os.removed)
        assertTrue(results.isEmpty()) // removeBond(true) is not completion.
        owner.bondChanged(other, PairingRecoveryBond.NONE)
        owner.bondChanged(board, PairingRecoveryBond.BONDED)
        assertTrue(results.isEmpty())
        owner.bondChanged(board, PairingRecoveryBond.NONE)
        owner.bondChanged(board, PairingRecoveryBond.NONE)
        os.associationCallback!!(PairingRecoveryAssociation(42, board), "failed")
        assertEquals(listOf("removed"), results)
        assertEquals(listOf(42), os.cleaned)
        assertNull(os.timer)
    }

    @Test fun anExistingAssociationIsReusedAndPreserved() {
        val os = Os().apply { known = listOf(PairingRecoveryAssociation(10, board), PairingRecoveryAssociation(11, other)) }
        val owner = BluetoothPairingRecovery(os) {}
        val results = mutableListOf<String>()
        owner.watch(board, 7)
        owner.repair(board, 7, results::add)
        assertNull(os.associationCallback)
        assertEquals(listOf(10), os.removed)
        owner.bondChanged(board, PairingRecoveryBond.NONE)
        assertEquals(listOf("removed"), results)
        assertTrue(os.cleaned.isEmpty())
    }

    @Test fun wrongPeerOrAmbiguousAssociationsCannotRemoveAnyBond() {
        val os = Os().apply { known = listOf(PairingRecoveryAssociation(11, other)) }
        val owner = BluetoothPairingRecovery(os) {}
        val results = mutableListOf<String>()
        owner.watch(board, 7)
        owner.repair(board, 7, results::add)
        os.associationCallback!!(PairingRecoveryAssociation(11, other), "failed")
        assertEquals(listOf("failed"), results)
        assertTrue(os.removed.isEmpty())
        assertTrue(os.cleaned.isEmpty())
        os.known = listOf(PairingRecoveryAssociation(12, board), PairingRecoveryAssociation(13, board))
        owner.repair(board, 7, results::add)
        assertEquals(listOf("failed", "failed"), results)
        assertTrue(os.removed.isEmpty())
    }

    @Test fun watchRetirementRejectsLateConsentAndOldGeneration() {
        val os = Os()
        val owner = BluetoothPairingRecovery(os) {}
        val results = mutableListOf<String>()
        owner.watch(board, 7)
        owner.repair(board, 7, results::add)
        val late = os.associationCallback!!
        owner.watch(other, 8)
        late(PairingRecoveryAssociation(42, board), "failed")
        owner.repair(board, 7, results::add)
        assertEquals(listOf("cancelled", "failed"), results)
        assertTrue(os.removed.isEmpty())
        assertEquals(listOf(42), os.cleaned)
        assertEquals(1, os.cancelled)
    }

    @Test fun cancellationTimeoutRemovalFailureAndCleanupFailureRemainTerminal() {
        for (failure in listOf("cancel", "timeout", "remove", "cleanup")) {
            val os = Os()
            val owner = BluetoothPairingRecovery(os) {}
            val results = mutableListOf<String>()
            owner.watch(board, 7)
            owner.repair(board, 7, results::add)
            when (failure) {
                "cancel" -> os.associationCallback!!(null, "cancelled")
                "timeout" -> os.timer!!()
                "remove" -> { os.removalAccepted = false; os.associationCallback!!(PairingRecoveryAssociation(42, board), "failed") }
                "cleanup" -> {
                    os.cleanupAccepted = false
                    os.associationCallback!!(PairingRecoveryAssociation(42, board), "failed")
                    owner.bondChanged(board, PairingRecoveryBond.NONE)
                }
            }
            assertEquals(listOf(if (failure == "cancel") "cancelled" else "failed"), results)
            owner.bondChanged(board, PairingRecoveryBond.NONE)
            assertEquals(1, results.size)
        }
    }

    @Test fun unsupportedOrAlreadyUnpairedNeedsNoAssociation() {
        val os = Os().apply { supported = false }
        val owner = BluetoothPairingRecovery(os) {}
        val results = mutableListOf<String>()
        owner.watch(board, 7)
        owner.repair(board, 7, results::add)
        os.supported = true
        os.state = PairingRecoveryBond.NONE
        owner.repair(board, 7, results::add)
        assertEquals(listOf("unsupported", "removed"), results)
        assertNull(os.associationCallback)
        assertTrue(os.removed.isEmpty())
    }
}
