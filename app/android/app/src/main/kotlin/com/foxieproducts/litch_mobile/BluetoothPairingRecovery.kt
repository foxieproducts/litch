package com.foxieproducts.litch_mobile

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

internal data class PairingRecoveryTarget(val deviceId: String, val generation: Long) {
    val address: String get() = deviceId.uppercase(java.util.Locale.ROOT)
}

internal data class PairingRecoveryAssociation(val id: Int, val address: String?)

internal enum class PairingRecoveryBond { NONE, BONDING, BONDED, OTHER }

/** OS effects only: the transaction below owns identity, completion and retirement. */
internal interface PairingRecoveryPlatform {
    fun available(): Boolean
    fun bond(address: String): PairingRecoveryBond
    fun createBond(address: String): Boolean
    fun associations(): List<PairingRecoveryAssociation>?
    fun associate(address: String, completion: (PairingRecoveryAssociation?, String) -> Unit)
    fun cancelAssociation()
    fun removeBond(associationId: Int): Boolean
    fun disassociate(associationId: Int): Boolean
    fun timeout(action: () -> Unit): () -> Unit
}

internal class BluetoothPairingRecovery(
    private val platform: PairingRecoveryPlatform,
    private val emitKeyMissing: (PairingRecoveryTarget) -> Unit,
    private val emitBondFailure: (PairingRecoveryTarget) -> Unit = {},
    private val emitEncryptionKeyMissing: (PairingRecoveryTarget) -> Unit = {},
) {
    private class Repair(
        val target: PairingRecoveryTarget,
        val previousIds: Set<Int>,
        val completion: (String) -> Unit,
    ) {
        var association: PairingRecoveryAssociation? = null
        var temporary = false
        var removing = false
        var cancelTimeout: (() -> Unit)? = null
    }

    private var watched: PairingRecoveryTarget? = null
    private var pending: Repair? = null
    private var bondingTarget: PairingRecoveryTarget? = null
    private var bondAttempt = 0
    private var bondAdmission = "failed"
    private var watchedWasBonded = false
    private var reportedEncryptionKeyMissing = false

    fun watch(deviceId: String?, generation: Long) {
        val next = deviceId?.takeIf {
            generation >= 0 && it.matches(Regex("(?i)[0-9a-f]{2}(:[0-9a-f]{2}){5}"))
        }?.let { PairingRecoveryTarget(it, generation) }
        if (watched == next) return
        watched = next
        watchedWasBonded = next != null &&
            runCatching { platform.bond(next.address) == PairingRecoveryBond.BONDED }.getOrDefault(false)
        reportedEncryptionKeyMissing = false
        bondingTarget = null
        bondAttempt = 0
        bondAdmission = "failed"
        pending?.let { finish(it, "cancelled") }
    }

    fun startBond(deviceId: String, generation: Long, attempt: Int): String {
        val target = watched
        if (target == null || target.deviceId != deviceId || target.generation != generation ||
            attempt !in 1..65535 || attempt < bondAttempt) return "failed"
        if (attempt == bondAttempt) return bondAdmission
        // Consume the exact attempt before the SDK call, including failure.
        // Replayed State or late permission changes cannot solicit another dialog.
        bondAttempt = attempt
        bondAdmission = "failed"
        if (pending != null) return bondAdmission
        bondAdmission = runCatching {
            when (platform.bond(target.address)) {
                PairingRecoveryBond.BONDED -> "alreadyBonded"
                PairingRecoveryBond.BONDING, PairingRecoveryBond.OTHER -> "failed"
                PairingRecoveryBond.NONE -> if (platform.createBond(target.address)) "started" else "failed"
            }
        }.getOrDefault("failed")
        return bondAdmission
    }

    fun keyMissing(address: String) {
        watched?.takeIf { it.address.equals(address, ignoreCase = true) }?.let(emitKeyMissing)
    }

    fun encryptionChanged(address: String, transport: Int, status: Int, enabled: Boolean?) {
        // HCI error 0x06 is PIN or Key Missing. Only a watched LE peer that
        // already had phone keys can enter saved-pairing recovery; this is not
        // evidence that a new pairing should be removed or started.
        val target = watched ?: return
        if (!target.address.equals(address, ignoreCase = true) ||
            !watchedWasBonded || reportedEncryptionKeyMissing || pending != null ||
            transport != BluetoothDevice.TRANSPORT_LE || status != 0x06 || enabled != false) return
        reportedEncryptionKeyMissing = true
        emitEncryptionKeyMissing(target)
    }

    fun repair(deviceId: String, generation: Long, completion: (String) -> Unit) {
        val target = watched
        if (target == null || target.deviceId != deviceId || target.generation != generation || pending != null) {
            completion("failed")
            return
        }
        if (!platform.available()) {
            completion("unsupported")
            return
        }
        when (platform.bond(target.address)) {
            PairingRecoveryBond.NONE -> { completion("removed"); return }
            PairingRecoveryBond.BONDING, PairingRecoveryBond.OTHER -> { completion("failed"); return }
            PairingRecoveryBond.BONDED -> Unit
        }
        val associations = platform.associations()
        if (associations == null) { completion("failed"); return }
        val matches = associations.filter { it.address.equals(target.address, ignoreCase = true) }
        if (matches.size > 1) { completion("failed"); return }
        val repair = Repair(target, associations.map { it.id }.toSet(), completion)
        pending = repair
        repair.cancelTimeout = platform.timeout { finish(repair, "failed") }
        if (matches.isNotEmpty()) {
            associationReady(repair, matches.single(), "failed")
        } else {
            platform.associate(target.address) { association, status ->
                associationReady(repair, association, status)
            }
        }
    }

    private fun associationReady(repair: Repair, association: PairingRecoveryAssociation?, status: String) {
        if (repair.association != null) return
        if (association != null) repair.association = association
        if (pending !== repair) {
            // An approved OS sheet can race retirement. It may only retire the
            // association this request created, never touch the peer's bond.
            if (association != null && association.id !in repair.previousIds) platform.disassociate(association.id)
            return
        }
        if (association == null) { finish(repair, status); return }
        repair.temporary = association.id !in repair.previousIds
        if (!association.address.equals(repair.target.address, ignoreCase = true)) {
            finish(repair, "failed")
            return
        }
        repair.removing = true
        if (!platform.removeBond(association.id)) { finish(repair, "failed"); return }
        if (platform.bond(repair.target.address) == PairingRecoveryBond.NONE) finish(repair, "removed")
    }

    fun bondChanged(address: String, bond: PairingRecoveryBond,
                    previous: PairingRecoveryBond = PairingRecoveryBond.OTHER) {
        val target = watched?.takeIf { it.address.equals(address, ignoreCase = true) }
        if (target != null) {
            if (bond == PairingRecoveryBond.BONDING) bondingTarget = target
            else {
                val failed = bond == PairingRecoveryBond.NONE && previous == PairingRecoveryBond.BONDING &&
                    bondingTarget == target && pending == null
                bondingTarget = null
                if (failed) emitBondFailure(target)
            }
        }
        pending?.takeIf {
            it.removing && it.target.address.equals(address, ignoreCase = true) && bond == PairingRecoveryBond.NONE
        }?.let { finish(it, "removed") }
    }

    private fun finish(repair: Repair, status: String) {
        if (pending !== repair) return
        pending = null
        repair.cancelTimeout?.invoke()
        val association = repair.association
        if (association == null) platform.cancelAssociation()
        val cleaned = !repair.temporary || association == null || platform.disassociate(association.id)
        repair.completion(if (status == "removed" && !cleaned) "failed" else status)
    }
}

/** All SDK calls and callbacks stay on the Activity's main thread. */
internal class AndroidBluetoothPairingRecovery(
    private val activity: Activity,
    private val channel: MethodChannel,
) : PairingRecoveryPlatform {
    private class AssociationCall(
        val requestCode: Int,
        val completion: (PairingRecoveryAssociation?, String) -> Unit,
    ) {
        var completed = false
    }

    private val handler = Handler(Looper.getMainLooper())
    private var closed = false
    private var receiverRegistered = false
    private var nextRequestCode = 0x4C00
    private var associationCall: AssociationCall? = null
    private val owner = BluetoothPairingRecovery(this, emitKeyMissing = { target ->
        channel.invokeMethod("pairingKeyMissing", mapOf(
            "deviceId" to target.deviceId,
            "generation" to target.generation,
            "source" to "android_key_missing",
        ))
    }, emitBondFailure = { target ->
        channel.invokeMethod("pairingBondFailed", mapOf(
            "deviceId" to target.deviceId,
            "generation" to target.generation,
            "source" to "android_bond_failed",
        ))
    }, emitEncryptionKeyMissing = { target ->
        channel.invokeMethod("pairingKeyMissing", mapOf(
            "deviceId" to target.deviceId,
            "generation" to target.generation,
            "source" to "android_encryption_key_missing",
        ))
    })
    private val manager: CompanionDeviceManager?
        get() = if (Build.VERSION.SDK_INT >= 36) activity.getSystemService(CompanionDeviceManager::class.java) else null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (closed) return
            @Suppress("DEPRECATION")
            val device = (if (Build.VERSION.SDK_INT >= 33)
                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                else intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)) ?: return
            when (intent.action) {
                BluetoothDevice.ACTION_KEY_MISSING -> if (Build.VERSION.SDK_INT >= 36) owner.keyMissing(device.address)
                BluetoothDevice.ACTION_ENCRYPTION_CHANGE -> if (Build.VERSION.SDK_INT >= 36) owner.encryptionChanged(
                    device.address,
                    intent.getIntExtra(BluetoothDevice.EXTRA_TRANSPORT, -1),
                    intent.getIntExtra(BluetoothDevice.EXTRA_ENCRYPTION_STATUS, -1),
                    if (intent.hasExtra(BluetoothDevice.EXTRA_ENCRYPTION_ENABLED))
                        intent.getBooleanExtra(BluetoothDevice.EXTRA_ENCRYPTION_ENABLED, true)
                    else null,
                )
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> owner.bondChanged(
                    device.address, decodeBond(intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)),
                    decodeBond(intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1)),
                )
            }
        }
    }

    init {
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED).apply {
            if (Build.VERSION.SDK_INT >= 36) {
                addAction(BluetoothDevice.ACTION_KEY_MISSING)
                addAction(BluetoothDevice.ACTION_ENCRYPTION_CHANGE)
            }
        }
        receiverRegistered = runCatching {
            if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
            else {
                @Suppress("DEPRECATION")
                activity.registerReceiver(receiver, filter)
            }
            true
        }.getOrDefault(false)
        channel.setMethodCallHandler(::handle)
    }

    private fun handle(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "watchDevice" -> {
                owner.watch(call.argument<String>("deviceId"), call.argument<Number>("generation")?.toLong() ?: -1)
                result.success(null)
            }
            "canRepairPairing" -> result.success(available())
            "startBond" -> result.success(owner.startBond(
                call.argument<String>("deviceId") ?: "",
                call.argument<Number>("generation")?.toLong() ?: -1,
                call.argument<Number>("attempt")?.toInt() ?: 0,
            ))
            "repairPairing" -> owner.repair(
                call.argument<String>("deviceId") ?: "",
                call.argument<Number>("generation")?.toLong() ?: -1,
            ) { status -> result.success(mapOf("status" to status)) }
            "openBluetoothSettings" -> result.success(runCatching {
                activity.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                true
            }.getOrDefault(false))
            else -> result.notImplemented()
        }
    }

    override fun available(): Boolean = !closed && receiverRegistered && Build.VERSION.SDK_INT >= 36 &&
        activity.packageManager.hasSystemFeature(PackageManager.FEATURE_COMPANION_DEVICE_SETUP) &&
        activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED && manager != null

    private fun device(address: String): BluetoothDevice? =
        activity.getSystemService(android.bluetooth.BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)

    override fun bond(address: String): PairingRecoveryBond = runCatching {
        decodeBond(device(address)?.bondState ?: -1)
    }.getOrDefault(PairingRecoveryBond.OTHER)

    override fun createBond(address: String): Boolean = !closed &&
        (Build.VERSION.SDK_INT < 31 ||
         activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED) &&
        runCatching { device(address)?.createBond() == true }.getOrDefault(false)

    override fun associations(): List<PairingRecoveryAssociation>? = runCatching {
        manager?.myAssociations?.map { PairingRecoveryAssociation(it.id, it.deviceMacAddress?.toString()) }
    }.getOrNull()

    override fun associate(address: String, completion: (PairingRecoveryAssociation?, String) -> Unit) {
        if (!available() || nextRequestCode > 0x7FFF || associationCall != null) { completion(null, "failed"); return }
        val call = AssociationCall(nextRequestCode++, completion)
        associationCall = call
        val request = AssociationRequest.Builder()
            .addDeviceFilter(BluetoothDeviceFilter.Builder().setAddress(address).build())
            .setSingleDevice(true)
            .build()
        val callback = object : CompanionDeviceManager.Callback() {
            override fun onAssociationPending(intentSender: android.content.IntentSender) {
                if (closed || associationCall !== call || call.completed) return
                runCatching { activity.startIntentSenderForResult(intentSender, call.requestCode, null, 0, 0, 0) }
                    .onFailure { deliver(call, null, "failed") }
            }
            override fun onAssociationCreated(info: AssociationInfo) {
                deliver(call, PairingRecoveryAssociation(info.id, info.deviceMacAddress?.toString()), "failed")
            }
            override fun onFailure(error: CharSequence?) { deliver(call, null, "failed") }
            override fun onFailure(errorCode: Int, error: CharSequence?) {
                deliver(call, null, if (errorCode == CompanionDeviceManager.RESULT_USER_REJECTED ||
                    errorCode == CompanionDeviceManager.RESULT_CANCELED) "cancelled" else "failed")
            }
        }
        runCatching { manager!!.associate(request, activity.mainExecutor, callback) }
            .onFailure { deliver(call, null, "failed") }
    }

    private fun deliver(call: AssociationCall, association: PairingRecoveryAssociation?, status: String) {
        if (call.completed) return
        call.completed = true
        if (associationCall === call) associationCall = null
        call.completion(association, status)
    }

    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        val call = associationCall?.takeIf { it.requestCode == requestCode } ?: return false
        val info = if (Build.VERSION.SDK_INT >= 36) data?.getParcelableExtra(
            CompanionDeviceManager.EXTRA_ASSOCIATION, AssociationInfo::class.java,
        ) else null
        if (resultCode == Activity.RESULT_OK && info != null) {
            deliver(call, PairingRecoveryAssociation(info.id, info.deviceMacAddress?.toString()), "failed")
        } else {
            deliver(call, null, if (resultCode == Activity.RESULT_CANCELED ||
                resultCode == CompanionDeviceManager.RESULT_USER_REJECTED) "cancelled" else "failed")
        }
        return true
    }

    override fun cancelAssociation() {
        associationCall = null
        if (Build.VERSION.SDK_INT >= 36) runCatching {
            manager?.buildAssociationCancellationIntent()?.let {
                activity.startIntentSenderForResult(it, 0x4BFF, null, 0, 0, 0)
            }
        }
    }

    override fun removeBond(associationId: Int): Boolean =
        Build.VERSION.SDK_INT >= 36 && runCatching { manager?.removeBond(associationId) == true }.getOrDefault(false)

    override fun disassociate(associationId: Int): Boolean = runCatching {
        if (Build.VERSION.SDK_INT < 36 || manager == null) false else {
            manager!!.disassociate(associationId)
            true
        }
    }.getOrDefault(false)

    override fun timeout(action: () -> Unit): () -> Unit {
        val task = Runnable(action)
        handler.postDelayed(task, REPAIR_TIMEOUT_MS)
        return { handler.removeCallbacks(task) }
    }

    fun close() {
        owner.watch(null, -1)
        closed = true
        if (receiverRegistered) runCatching { activity.unregisterReceiver(receiver) }
        receiverRegistered = false
        channel.setMethodCallHandler(null)
    }

    private fun decodeBond(state: Int): PairingRecoveryBond = when (state) {
        BluetoothDevice.BOND_NONE -> PairingRecoveryBond.NONE
        BluetoothDevice.BOND_BONDED -> PairingRecoveryBond.BONDED
        BluetoothDevice.BOND_BONDING -> PairingRecoveryBond.BONDING
        else -> PairingRecoveryBond.OTHER
    }

    private companion object {
        const val REPAIR_TIMEOUT_MS = 60_000L
    }
}
