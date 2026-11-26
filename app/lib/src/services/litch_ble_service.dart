import '../models/board_health_status.dart';
import '../models/board_storage_recovery.dart';
import '../models/current_game_line_state.dart';
import '../models/board_physical_input_state.dart';
import '../models/board_switch_state.dart';
import '../models/settings_state_wire_codec.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'bluetooth_pairing_recovery.dart';

import '../constants/ble_protocol.dart';
import '../models/firmware_provenance.dart';
import '../models/firmware_manifest.dart';
import '../models/home_state_notification.dart';
import '../models/home_state_wire_codec.dart';
import '../models/litch_control_action.dart';
import '../models/litch_device_capabilities.dart';
import '../models/litch_end_palette.dart';
import '../models/litch_board.dart';
import '../models/litch_setting.dart';
import '../models/owner_pairing_session.dart';
import '../models/runtime_health_snapshot.dart';
import '../models/support_diagnostics.dart';
import '../models/support_board_collection.dart';
import '../models/support_snapshot.dart';
import '../models/support_phone_diagnostics.dart';
import 'app_trace_recorder.dart';
import 'connection_session.dart';
import 'connection_identify_codec.dart';
import 'owner_pairing_session.dart';
import 'connection_session_codec.dart';
import 'connection_game_history_codec.dart';
import 'connection_litchcraft_codec.dart';
import 'connection_ota_codec.dart';
import 'connection_ota_status_codec.dart';
import 'firmware_digest_reader.dart';
import 'firmware_artifact_validator.dart';
import 'support_collection_scope.dart';
import 'setting_read_diagnostics.dart';
import 'firmware_provenance_service.dart';
import 'http_firmware_provenance_verifier.dart';
import 'litch_gatt_session.dart';
import 'litch_control_protocol.dart';
import 'litchcraft_protocol.dart';
import 'ota/litch_firmware_uploader.dart';
import 'ota/litch_ota_control_channel.dart';
import 'ota/ota_post_reboot_verifier.dart';
import 'ota/ota_status_event.dart';
import 'ota/ota_pending_verification_owner.dart';
import 'ota/pending_ota_target_store.dart';

// Keep the Dart admission bound aligned with the negotiated Object window and
// the native iOS/Android batch implementations. Artwork uses a smaller 48
// packet submission burst to leave firmware-ingress headroom; the transport
// must still accept the full negotiated window for other Object traffic.
const int _maximumLitchcraftBulkBatchPackets =
    ConnectionSessionCodec.maximumWindow;

class _ConsolePacketObserver {
  const _ConsolePacketObserver(
    this.deviceId,
    this.connectionToken,
    this.onPacket,
    this.onError,
  );

  final String deviceId;
  final int connectionToken;
  final ValueChanged<List<int>> onPacket;
  final ValueChanged<Object> onError;
}

/// The phone's ability to discover a board, independent of whether a board is
/// currently advertising.
enum BleAvailability {
  checking,
  ready,
  bluetoothPermissionDenied,
  bluetoothPermissionBlocked,
  locationPermissionDenied,
  locationPermissionBlocked,
  poweredOff,
  locationServicesDisabled,
  unsupported,
  unavailable,
}

abstract class BlePermissionGateway {
  Future<PermissionStatus> status(Permission permission);

  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  );

  Future<bool> openSettings();
}

class PermissionHandlerBlePermissionGateway implements BlePermissionGateway {
  const PermissionHandlerBlePermissionGateway();

  @override
  Future<PermissionStatus> status(Permission permission) => permission.status;

  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) => permissions.request();

  @override
  Future<bool> openSettings() => openAppSettings();
}

enum _PendingSettingKind { read, write }

/// The local reason an acknowledged setting write ended without a valid reply.
enum LitchSettingWriteFailure {
  bridgeUnavailable,
  requestIdExhausted,
  sendFailed,
  timedOut,
  disconnected,
  connectionReplaced,
  requestReplaced,
  malformedAcknowledgement,
  interrupted,
}

/// The result of a setting write that explicitly waits for a correlated reply.
sealed class LitchSettingWriteResult {
  const LitchSettingWriteResult();
}

/// A valid full or compact setting outcome correlated to the request.
final class LitchSettingWriteAcknowledged extends LitchSettingWriteResult {
  const LitchSettingWriteAcknowledged(
    this.outcome, {
    this.boardStateConfirmation,
  });

  final LitchSettingOutcome outcome;
  final Future<bool>? boardStateConfirmation;
}

/// No valid acknowledgement was available for the setting write.
final class LitchSettingWriteNotAcknowledged extends LitchSettingWriteResult {
  const LitchSettingWriteNotAcknowledged(this.failure);

  final LitchSettingWriteFailure failure;
}

class _BulkStateQuery {
  _BulkStateQuery(this.nonce, this.generation, this.receipts);
  final int nonce;
  final int generation;
  final Map<String, Completer<bool>> receipts;
  int attempts = 0;
}

enum _SettingsEnumerationAuthority { confirmed, exhausted, sessionChanged }

enum LitchSettingsReadAuthority { unknown, confirmed, unavailable }

class _SettingsEnumerationResult {
  const _SettingsEnumerationResult(this.settings, this.authority);
  final List<LitchSetting> settings;
  final _SettingsEnumerationAuthority authority;
}

class _SettingReadObservation {
  final Stopwatch elapsed = Stopwatch()..start();
  SettingReadOutcome outcome = SettingReadOutcome.unknown;
  bool directAttempted = false;
  bool optionalMissing = false;
}

class _PendingSettingTransaction {
  _PendingSettingTransaction({
    required this.requestId,
    required this.deviceId,
    required this.sessionGeneration,
    required this.key,
    required this.kind,
    required this.requiresExactCorrelation,
    required this.completer,
    this.readObservation,
  });

  final int requestId;
  final String deviceId;
  final int sessionGeneration;
  final String key;
  final _PendingSettingKind kind;
  final bool requiresExactCorrelation;
  final Completer<Object?> completer;
  final _SettingReadObservation? readObservation;
}

typedef RememberedOwnedBoard = ({
  List<String> aliases,
  String historyStorageKey,
  String? bluetoothSuffix,
});

class LitchBleService {
  static DateTime Function() _monotonicScanClock() {
    final elapsed = Stopwatch()..start();
    final origin = DateTime.now();
    return () => origin.add(elapsed.elapsed);
  }

  LitchBleService({
    FlutterReactiveBle? ble,
    LitchGattSessionFactory? gattSessionFactory,
    BlePermissionGateway permissionGateway =
        const PermissionHandlerBlePermissionGateway(),
    Duration receivedHeadCompletionTimeout = orderedHeadCompletionTimeout,
    Duration ownerPairingReadinessTimeout =
        OwnerPairingSession.defaultReadinessTimeout,
    AppTraceSink? appTrace,
    AppConsoleSnapshotSink? consoleSnapshotSink,
    OtaPendingTargetStore? pendingOtaStore,
    BluetoothPairingRecoveryGateway? pairingRecoveryGateway,
    FirmwareProvenanceVerifier firmwareProvenanceVerifier =
        const HttpFirmwareProvenanceVerifier(),
    DateTime Function()? scanClock,
  }) : _scanNow = scanClock ?? _monotonicScanClock(),
       _ble = ble ?? FlutterReactiveBle(),
       _firmwareProvenanceVerifier = firmwareProvenanceVerifier,
       _permissionGateway = permissionGateway,
       _receivedHeadCompletionTimeout = receivedHeadCompletionTimeout,
       _ownerPairingReadinessTimeout = ownerPairingReadinessTimeout,
       _appTrace = appTrace,
       _consoleSnapshotSink =
           consoleSnapshotSink ??
           (appTrace is AppConsoleSnapshotSink
               ? appTrace as AppConsoleSnapshotSink
               : null),
       _pairingRecoveryGateway =
           pairingRecoveryGateway ??
           (ble == null
               ? MethodChannelBluetoothPairingRecovery()
               : const UnavailableBluetoothPairingRecovery()),
       _otaVerificationOwner = OtaPendingVerificationOwner(
         store: pendingOtaStore,
       ) {
    _gattSessionFactory =
        gattSessionFactory ??
        (ble == null ? ReactiveBleGattSessionFactory(_ble) : null);
    _pairingKeyMissingSub =
        _pairingRecoveryGateway is UnavailableBluetoothPairingRecovery
        ? null
        : _pairingRecoveryGateway.keyMissing.listen((event) {
            if (!_disposed &&
                event.generation == _pairingWatchGeneration &&
                event.generation == _connectionGeneration &&
                event.deviceId.toUpperCase() ==
                    _pairingWatchDeviceId?.toUpperCase()) {
              _suspendForStalePairing(_pairingWatchDeviceId!);
            }
          });
    _pairingBondFailureSub = _pairingRecoveryGateway.bondFailures.listen((event) {
      if (event.generation != _connectionGeneration ||
          event.generation != _pairingWatchGeneration ||
          event.deviceId.toUpperCase() != _pairingWatchDeviceId?.toUpperCase()) {
        return;
      }
      _nativeBondFailureGeneration = event.generation;
      _recoverFailedOwnerVerification();
    });
  }

  // Common Ordered completion policy; storage may finish after transport receipt.
  static const Duration orderedHeadCompletionTimeout = Duration(seconds: 20);
  static const Duration orderedOperationTimeout = Duration(seconds: 30);

  final FirmwareProvenanceVerifier _firmwareProvenanceVerifier;

  static const Duration _permissionLogCooldown = Duration(seconds: 30);
  static const Duration _supportPermissionTimeout = Duration(seconds: 3);
  SupportCollectionScope? _supportCollection;
  static const Duration _settingsDiagLogCooldown = Duration(seconds: 10);
  static const Duration _keepAliveInterval = Duration(seconds: 4);
  static const Duration _keepAliveWriteTimeout = Duration(seconds: 2);
  static const Duration _gattCacheRefreshTimeout = Duration(seconds: 3);
  static const int _androidScanStartLimit = 5;
  static const Duration _androidScanWindow = Duration(seconds: 30);
  static const Duration _consoleBridgeRetryDelay = Duration(milliseconds: 500);
  static const Duration _optionalCommandLogCooldown = Duration(seconds: 30);
  static const int _capabilityProbeAttempts = 3;
  static const Duration _capabilityProbeTimeout = Duration(milliseconds: 1200);
  static const Duration _capabilityProbeRetryDelay = Duration(
    milliseconds: 180,
  );
  static const int _consoleHistoryCapacity = 120;
  // Current firmware preserves console records up to 2047 payload bytes before
  // a newline. Drop an oversized partial record instead of retaining an
  // unbounded StringBuffer while waiting for a terminator.
  static const int _consoleLineCapacity = 2047;
  static const Set<String> _playerColorSettingKeys = <String>{
    'P1_PIECE_WHEEL',
    'P2_PIECE_WHEEL',
    'P1_END_PIECE_WHEEL',
    'P2_END_PIECE_WHEEL',
  };
  static const Set<String> _cpuRoleSettingKeys = <String>{
    'VS_CPU_LEVEL',
    'VS_CPU_PLAYER',
  };
  static const String _p1PieceWheelKey = 'P1_PIECE_WHEEL';
  static const String _p2PieceWheelKey = 'P2_PIECE_WHEEL';
  static const String _p1EndPieceWheelKey = 'P1_END_PIECE_WHEEL';
  static const String _p2EndPieceWheelKey = 'P2_END_PIECE_WHEEL';

  final FlutterReactiveBle _ble;
  final BlePermissionGateway _permissionGateway;
  late final LitchGattSessionFactory? _gattSessionFactory;
  final Duration _receivedHeadCompletionTimeout;
  final Duration _ownerPairingReadinessTimeout;
  final AppTraceSink? _appTrace;
  final AppConsoleSnapshotSink? _consoleSnapshotSink;
  final BluetoothPairingRecoveryGateway _pairingRecoveryGateway;
  late final StreamSubscription<BluetoothPairingKeyMissing>?
  _pairingKeyMissingSub;
  late final StreamSubscription<BluetoothPairingBondFailed> _pairingBondFailureSub;
  int? _nativeBondFailureGeneration;
  final _pairingRecoveryController =
      StreamController<BluetoothPairingRecoveryState?>.broadcast(sync: true);
  BluetoothPairingRecoveryState? _pairingRecovery;
  String? _pairingWatchDeviceId;
  int _pairingWatchGeneration = 0;
  bool _pairingRecoveryConnecting = false;

  BluetoothPairingRecoveryState? get pairingRecovery => _pairingRecovery;
  Stream<BluetoothPairingRecoveryState?> get pairingRecoveryStream =>
      _pairingRecoveryController.stream;

  void _publishPairingRecovery(BluetoothPairingRecoveryState? state) {
    _pairingRecovery = state;
    _trace(
      'ble.pairing_recovery',
      fields: <String, Object?>{
        'generation': _connectionGeneration,
        'stage': state?.stage.name ?? 'none',
        'can_repair': state?.canRepair ?? false,
        'failed': state?.failed ?? false,
      },
    );
    if (!_disposed) _pairingRecoveryController.add(state);
  }

  bool get _awaitingSavedOwnerVerification {
    final status = _ownerPairingSession?.status;
    return !productReady &&
        status?.state == OwnerPairingSessionState.ownedOther &&
        status?.reason == OwnerPairingSessionReason.none &&
        status?.ownerRetained == true &&
        _ownerPairingSession?.phoneBondRequested != true;
  }

  void _recoverFailedOwnerVerification() {
    final id = connectedId;
    if (id == null || _nativeBondFailureGeneration != _connectionGeneration) {
      return;
    }
    if (!_awaitingSavedOwnerVerification) {
      // An early native event can precede the first public owner snapshot.
      // Once its phase is known, a fresh claim or a healthy owner consumes it.
      if (_ownerPairingSession?.status != null) {
        _nativeBondFailureGeneration = null;
      }
      return;
    }
    final generation = _connectionGeneration;
    // State delivery is synchronous. Retire only after its current publication
    // unwinds, just as the existing foreign-owner refusal does.
    scheduleMicrotask(() {
      if (_connectionGeneration == generation &&
          connectedId == id &&
          _nativeBondFailureGeneration == generation &&
          _awaitingSavedOwnerVerification) {
        _suspendForStalePairing(
          id,
          reason: BluetoothPairingRecoveryReason.ownerVerificationFailed,
        );
      }
    });
  }

  void _suspendForStalePairing(
    String deviceId, {
    BluetoothPairingRecoveryReason reason =
        BluetoothPairingRecoveryReason.keyMissing,
  }) {
    if (_pairingRecovery?.deviceId == deviceId && !_pairingRecoveryConnecting) {
      return;
    }
    final pending = BluetoothPairingRecoveryState(
      deviceId: deviceId,
      reason: reason,
    );
    _publishPairingRecovery(pending);
    _publishProductReadiness(false);
    _pushAppLog(
      reason == BluetoothPairingRecoveryReason.keyMissing
          ? 'Saved Bluetooth pairing cannot be used; automatic reconnect paused.'
          : 'Native pairing failed while verifying a saved connection; automatic reconnect paused.',
    );
    unawaited(stopScan());
    unawaited(disconnect());
    unawaited(() async {
      var canRepair = false;
      try {
        canRepair = reason == BluetoothPairingRecoveryReason.keyMissing &&
            await _pairingRecoveryGateway.canRepairPairing();
      } catch (_) {
        // The manual recovery journey remains available.
      }
      if (!_disposed && identical(_pairingRecovery, pending)) {
        _publishPairingRecovery(
          pending.copyWith(
            canRepair: canRepair,
            stage: BluetoothPairingRecoveryStage.ready,
          ),
        );
      }
    }());
  }

  Future<bool> openSystemBluetoothSettings() =>
      defaultTargetPlatform == TargetPlatform.iOS
      ? _permissionGateway.openSettings()
      : _pairingRecoveryGateway.openBluetoothSettings();

  Future<bool> repairPhonePairing() async {
    final pending = _pairingRecovery;
    if (pending == null || pending.busy || !pending.canRepair) return false;
    final repairing = pending.copyWith(
      stage: BluetoothPairingRecoveryStage.repairing,
      failed: false,
    );
    _publishPairingRecovery(repairing);
    final generation = _connectionGeneration;
    await stopScan();
    if (_disposed ||
        !identical(_pairingRecovery, repairing) ||
        generation != _connectionGeneration) {
      return false;
    }
    await disconnect();
    if (_disposed || !identical(_pairingRecovery, repairing)) return false;
    var result = BluetoothPairingRepairResult.failed;
    try {
      await _pairingRecoveryGateway.watchDevice(
        pending.deviceId,
        _connectionGeneration,
      );
      result = await _pairingRecoveryGateway.repairPairing(
        pending.deviceId,
        _connectionGeneration,
      );
    } catch (_) {
      // A native failure never implies that a bond was removed.
    }
    if (_disposed || !identical(_pairingRecovery, repairing)) return false;
    if (result == BluetoothPairingRepairResult.removed) {
      _publishPairingRecovery(
        repairing.copyWith(stage: BluetoothPairingRecoveryStage.ready),
      );
      return retryPairingConnection();
    }
    _publishPairingRecovery(
      repairing.copyWith(
        canRepair: result == BluetoothPairingRepairResult.unsupported
            ? false
            : repairing.canRepair,
        stage: BluetoothPairingRecoveryStage.ready,
        failed: result != BluetoothPairingRepairResult.cancelled,
      ),
    );
    return false;
  }

  /// A user retry or confirmed OS removal admits one ordinary connection.
  Future<bool> retryPairingConnection() async {
    final pending = _pairingRecovery;
    if (pending == null || pending.busy || _pairingRecoveryConnecting) {
      return false;
    }
    final reconnecting = pending.copyWith(
      stage: BluetoothPairingRecoveryStage.reconnecting,
      failed: false,
    );
    _publishPairingRecovery(reconnecting);
    _pairingRecoveryConnecting = true;
    try {
      final connected = await connect(pending.deviceId);
      if (!connected && identical(_pairingRecovery, reconnecting)) {
        _publishPairingRecovery(
          reconnecting.copyWith(
            stage: BluetoothPairingRecoveryStage.ready,
            failed: true,
          ),
        );
      }
      return connected;
    } finally {
      _pairingRecoveryConnecting = false;
    }
  }

  Future<void> dismissPairingRecovery() async {
    if (_pairingRecovery == null) return;
    final generation = _connectionGeneration;
    _publishPairingRecovery(null);
    _pairingWatchDeviceId = null;
    await _pairingRecoveryGateway.watchDevice(null, generation);
    if (_disposed ||
        generation != _connectionGeneration ||
        _pairingRecovery != null) {
      return;
    }
    await disconnect();
  }

  void _trace(String event, {Map<String, Object?> fields = const {}}) {
    _appTrace?.record(event, fields: fields);
  }

  // Snapshots can finish out of order and contain different peripheral subsets.
  // A high-water mark would suppress unseen records from an older snapshot.
  final Set<(String, int)> _nativeTraceIdentities = <(String, int)>{};
  static const int _nativeTraceIdentityCapacity = 128;
  static const int _nativeTraceFieldsByteBudget = 312;

  void _recordConnectionDiagnostic(
    Map<String, Object?> record,
    int connectionGeneration,
  ) {
    final lifetime = record['native_lifetime'];
    final sequence = record['seq'];
    final phase = record['phase'];
    if (lifetime is! String || sequence is! int || phase is! String) return;
    if (!_nativeTraceIdentities.add((lifetime, sequence))) return;
    if (_nativeTraceIdentities.length > _nativeTraceIdentityCapacity) {
      _nativeTraceIdentities.remove(_nativeTraceIdentities.first);
    }

    int part = 0;
    Map<String, Object?> identity() => <String, Object?>{
      'generation': connectionGeneration,
      'native_seq': sequence,
      'native_lifetime': lifetime,
      'part': part,
      'last_part': false,
    };
    Map<String, Object?> fields = identity();
    void emit({bool last = false}) {
      fields['last_part'] = last;
      _trace(
        part == 0 ? 'ble.native_bootstrap' : 'ble.native_bootstrap_detail',
        fields: fields,
      );
      part++;
      fields = identity();
    }

    // Reserve 200 bytes for AppTrace's envelope, including maximum-width
    // process counters. Every part retains its join identity; no native scalar
    // disappears into AppTrace's oversized-record fallback.
    for (final entry in <String, Object?>{
      'phase': phase,
      for (final entry in record.entries)
        if (entry.key != 'seq' &&
            entry.key != 'native_lifetime' &&
            entry.key != 'phase' &&
            entry.key != 'generation' &&
            entry.key != 'native_seq' &&
            entry.key != 'part' &&
            entry.key != 'last_part')
          entry.key: entry.value,
    }.entries) {
      final next = <String, Object?>{...fields, entry.key: entry.value};
      if (next.length > 16 ||
          utf8.encode(jsonEncode(next)).length > _nativeTraceFieldsByteBudget) {
        emit();
      }
      fields[entry.key] = entry.value;
    }
    emit(last: true);
  }

  Future<void> _captureConnectionDiagnostics(
    String deviceId,
    int connectionGeneration,
  ) async {
    if (_appTrace == null) return;
    try {
      final records = await _ble
          .getConnectionDiagnostics(deviceId)
          .timeout(const Duration(seconds: 1));
      for (final record in records) {
        _recordConnectionDiagnostic(record, connectionGeneration);
      }
    } catch (error) {
      _trace(
        'ble.native_diagnostics_unavailable',
        fields: <String, Object?>{
          'generation': connectionGeneration,
          'kind': error.runtimeType.toString(),
        },
      );
    }
  }

  StreamSubscription<DiscoveredDevice>? _scanSub;
  StreamSubscription<BleStatus>? _bleStatusSub;
  StreamSubscription<ConnectionStateUpdate>? _connSub;
  StreamSubscription<List<int>>? _consoleSub;
  _ConsolePacketObserver? _consolePacketObserver;
  StreamSubscription<List<int>>? _settingsRwSub;
  StreamSubscription<OtaStatusEvent>? _otaStatusSub;
  StreamSubscription<ConnectionSessionPacket>? _otaSharedStatusSub;
  Timer? _consoleRetryTimer;

  final StreamController<List<LitchBoard>> _boardsController =
      StreamController<List<LitchBoard>>.broadcast();
  final StreamController<BleAvailability> _availabilityController =
      StreamController<BleAvailability>.broadcast();
  final StreamController<List<String>> _consoleController =
      StreamController<List<String>>.broadcast();
  final StreamController<BoardHealthStatus> _boardHealthController =
      StreamController<BoardHealthStatus>.broadcast();
  final StreamController<BoardSwitchState> _boardSwitchStateController =
      StreamController<BoardSwitchState>.broadcast(sync: true);
  final StreamController<BoardPhysicalInputState>
  _boardPhysicalInputStateController =
      StreamController<BoardPhysicalInputState>.broadcast(sync: true);
  final Map<BoardSwitch, BoardSwitchState> _boardSwitchStates =
      <BoardSwitch, BoardSwitchState>{};
  BoardPhysicalInputState? _latestBoardPhysicalInputState;
  BoardHealthStatus? _latestBoardHealth;
  final StreamController<CurrentGameLineState> _currentGameLineController =
      StreamController<CurrentGameLineState>.broadcast(sync: true);
  CurrentGameLineState? _latestCurrentGameLine;
  Stream<CurrentGameLineState> get currentGameLineStream =>
      _currentGameLineController.stream;
  int get connectionSessionToken => _connectionGeneration;

  final StreamController<BoardStorageRecoverySnapshot>
  _boardStorageRecoveryController =
      StreamController<BoardStorageRecoverySnapshot>.broadcast(sync: true);
  BoardStorageRecoverySnapshot? _boardStorageRecovery;
  int? _boardStorageRecoveryRevision;
  Future<BoardStorageRecoverySnapshot>? _boardStorageReformatFuture;
  int? _boardStorageReformatGeneration;
  int? _boardStorageReformatDispatchedGeneration;
  BoardStorageRecoveryStatus? _boardStorageReformatCompletionStatus;
  int? _boardStorageReformatSubmissionRevision;
  bool _boardStorageReformatTerminalRefusal = false;

  BoardStorageRecoverySnapshot? get latestBoardStorageRecovery =>
      _boardStorageRecovery;
  Stream<BoardStorageRecoverySnapshot> get boardStorageRecoveryStream =>
      _boardStorageRecoveryController.stream;

  /// A valid reliable State response is the capability proof. Missing and
  /// pre-session peers never acquire permission to execute recovery.
  Future<BoardStorageRecoverySnapshot> fetchBoardStorageRecovery({
    required String deviceId,
  }) async {
    final generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    if (!current()) return BoardStorageRecoverySnapshot.unknown;
    final capabilities = deviceCapabilities(deviceId);
    if (!capabilities.supportsConnectionSession) {
      return BoardStorageRecoverySnapshot.unavailable;
    }
    try {
      if (capabilities.supportsOwnerPairing &&
          !await ensureProductReady(deviceId)) {
        return BoardStorageRecoverySnapshot.unknown;
      }
      final session = await openConnectionSession(deviceId);
      if (!current() || session?.isActivated != true) {
        return BoardStorageRecoverySnapshot.unknown;
      }
      final packet = await session!.readState(
        code: BoardStorageRecoverySnapshot.stateCode,
      );
      if (!current() || !identical(session, _activeConnectionSession)) {
        return BoardStorageRecoverySnapshot.unknown;
      }
      final snapshot =
          BoardStorageRecoverySnapshot.tryParse(packet.body) ??
          BoardStorageRecoverySnapshot.unknown;
      final submitted = _boardStorageReformatSubmissionRevision;
      if (_boardStorageReformatDispatchedGeneration == generation &&
          submitted != null &&
          _isStorageReformatTerminal(snapshot) &&
          !isNewerHomeStateSequence(packet.sequence, submitted)) {
        // An unchanged terminal preflight describes the previous attempt,
        // including when the user explicitly checks an uncertain new result.
        return BoardStorageRecoverySnapshot.unknown;
      }
      return snapshot;
    } catch (_) {
      return BoardStorageRecoverySnapshot.unknown;
    }
  }

  /// Shares each confirmed submission while its connection owns it.
  /// Lost receipts are queried through the shared owner; this never replays an
  /// uncertain destructive request or sends through a direct characteristic.
  Future<BoardStorageRecoverySnapshot> reformatBoardStorage({
    required String deviceId,
  }) {
    if (_boardStorageReformatGeneration == _connectionGeneration &&
        _boardStorageReformatFuture != null &&
        !_completedStorageReformatFailed) {
      return _boardStorageReformatFuture!;
    }
    final generation = _connectionGeneration;
    _boardStorageReformatDispatchedGeneration = null;
    _boardStorageReformatCompletionStatus = null;
    _boardStorageReformatSubmissionRevision = null;
    _boardStorageReformatTerminalRefusal = false;
    final operation = _reformatBoardStorage(deviceId, generation);
    _boardStorageReformatGeneration = generation;
    _boardStorageReformatFuture = operation;
    operation.then((result) {
      if (generation != _connectionGeneration ||
          !identical(operation, _boardStorageReformatFuture)) {
        return;
      }
      _boardStorageReformatCompletionStatus = result.status;
      if (_boardStorageReformatDispatchedGeneration != generation ||
          _boardStorageReformatTerminalRefusal ||
          _completedStorageReformatFailed) {
        // Verified failure or a reliable retry-safe refusal permits a fresh
        // confirmation. Uncertain and successful submissions remain retained.
        _boardStorageReformatFuture = null;
      }
    });
    return operation;
  }

  bool get _completedStorageReformatFailed {
    final completion = _boardStorageReformatCompletionStatus;
    if (completion == null ||
        completion == BoardStorageRecoveryStatus.succeeded) {
      return false;
    }
    if (completion == BoardStorageRecoveryStatus.failed) return true;
    final observed = _boardStorageRecoveryRevision;
    final submitted = _boardStorageReformatSubmissionRevision;
    // A late reliable failure can resolve an unknown waiter, but a previous
    // attempt's Failed preflight is not proof about this submitted action.
    return _boardStorageRecovery?.status == BoardStorageRecoveryStatus.failed &&
        observed != null &&
        submitted != null &&
        isNewerHomeStateSequence(observed, submitted);
  }

  bool _isStorageReformatTerminal(BoardStorageRecoverySnapshot snapshot) =>
      snapshot.status == BoardStorageRecoveryStatus.failed ||
      snapshot.status == BoardStorageRecoveryStatus.succeeded;

  BoardStorageRecoverySnapshot? _storageReformatTerminalForSubmission() {
    final latest = _boardStorageRecovery;
    final observed = _boardStorageRecoveryRevision;
    final submitted = _boardStorageReformatSubmissionRevision;
    if (latest == null ||
        !_isStorageReformatTerminal(latest) ||
        observed == null ||
        submitted == null ||
        !isNewerHomeStateSequence(observed, submitted)) {
      return null;
    }
    return latest;
  }

  Future<BoardStorageRecoverySnapshot> _reformatBoardStorage(
    String deviceId,
    int generation,
  ) async {
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    final before = await fetchBoardStorageRecovery(deviceId: deviceId);
    if (!current()) return BoardStorageRecoverySnapshot.unknown;
    if (!before.canReformat) return before;
    final session = _activeConnectionSession;
    if (session?.isActivated != true) {
      return BoardStorageRecoverySnapshot.unknown;
    }
    try {
      _boardStorageReformatDispatchedGeneration = generation;
      _boardStorageReformatSubmissionRevision = _boardStorageRecoveryRevision;
      final outcome = await session!.sendOrdered(
        LitchControlProtocol.connectionActionOpcode,
        LitchControlProtocol.encodeConnectionStorageReformat(),
      );
      if (!current() || !identical(session, _activeConnectionSession)) {
        return BoardStorageRecoverySnapshot.unknown;
      }
      if (outcome.disposition == ConnectionSessionDisposition.cancelled) {
        return _storageReformatTerminalForSubmission() ??
            BoardStorageRecoverySnapshot.unknown;
      }
      final reason = LitchMutationReason.fromWireCode(outcome.reason);
      if (outcome.disposition == ConnectionSessionDisposition.rejected) {
        if (reason == LitchMutationReason.busy ||
            reason == LitchMutationReason.queueFull) {
          _boardStorageReformatTerminalRefusal = true;
          return const BoardStorageRecoverySnapshot(
            status: BoardStorageRecoveryStatus.busy,
          );
        }
        if (reason == LitchMutationReason.unsupported ||
            reason == LitchMutationReason.unknownAction) {
          _boardStorageReformatTerminalRefusal = true;
          return BoardStorageRecoverySnapshot.unavailable;
        }
      }
      // Admission or a radio write is never completion. The reliable worker
      // terminal must be newer than this attempt's preflight, even if the
      // receipt or the local status waiter is uncertain.
      final after = await fetchBoardStorageRecovery(deviceId: deviceId);
      if (!current()) return BoardStorageRecoverySnapshot.unknown;
      final terminal = _storageReformatTerminalForSubmission();
      if (terminal != null) return terminal;
      if (outcome.disposition == ConnectionSessionDisposition.applied) {
        return BoardStorageRecoverySnapshot.unknown;
      }
      if (after.status != BoardStorageRecoveryStatus.idle &&
          after.status != BoardStorageRecoveryStatus.unknown &&
          !_isStorageReformatTerminal(after)) {
        return after;
      }
      return BoardStorageRecoverySnapshot.unknown;
    } catch (_) {
      return current() && identical(session, _activeConnectionSession)
          ? _storageReformatTerminalForSubmission() ??
                BoardStorageRecoverySnapshot.unknown
          : BoardStorageRecoverySnapshot.unknown;
    }
  }

  Future<LitchDeviceCapabilities> _connectionCapabilitiesForDomain(
    String deviceId,
  ) async {
    final cached = deviceCapabilities(deviceId);
    final discovery = _capabilityDiscoveryByDevice[deviceId];
    final capabilities = cached.explicitlyReported
        ? cached
        : await (discovery ?? fetchDeviceCapabilities(deviceId: deviceId));
    if (capabilities.supportsOwnerPairing &&
        !await ensureProductReady(deviceId)) {
      throw const OwnerPairingNotReady();
    }
    return capabilities;
  }

  OwnerPairingSession? _ownerPairingSession;
  StreamSubscription<OwnerPairingSessionStatus>? _ownerPairingSubscription;
  Future<bool>? _productPreparation;
  bool _productPreparationFailed = false;
  static const int _maximumOwnerClaimIntents = 64;
  final Set<String> _ownerClaimRetryRequiredBoards = {};
  @visibleForTesting
  int get debugOwnerClaimIntentCount => _ownerClaimRetryRequiredBoards.length;
  bool _productReadinessPending = false;
  bool _publishedProductReady = false;
  void _publishProductReadiness(bool ready, {bool force = false}) {
    _persistConsoleSnapshot();
    if (_productReadinessController.isClosed ||
        (!force && _publishedProductReady == ready)) {
      return;
    }
    _publishedProductReady = ready;
    _productReadinessController.add(ready);
  }

  final _productReadinessController = StreamController<bool>.broadcast(
    sync: true,
  );
  final _ownerPairingStatusController =
      StreamController<OwnerPairingSessionStatus?>.broadcast(sync: true);
  OwnerPairingSessionStatus? _ownerPairingRefusal;
  String? _ownerPairingRefusedDeviceId;
  String? get ownerPairingRefusedDeviceId => _ownerPairingRefusedDeviceId;
  OwnerPairingSessionStatus? get ownerPairingStatus =>
      _ownerPairingSession?.status ?? _ownerPairingRefusal;
  Stream<OwnerPairingSessionStatus?> get ownerPairingStatusStream =>
      _ownerPairingStatusController.stream;
  Stream<bool> get productReadinessStream => _productReadinessController.stream;
  bool get productReady {
    final id = connectedId;
    if (!isConnected ||
        _connectAttemptInProgress ||
        identifyConnectionActive ||
        id == null ||
        _repairingGattInventory ||
        _productReadinessPending ||
        _pairingRecovery != null) {
      return false;
    }
    final capabilities = deviceCapabilities(id);
    return capabilities.supportsConnectionSession &&
        _activeConnectionSession?.isActivated == true &&
        (!capabilities.supportsOwnerPairing ||
            _ownerPairingSession?.ready == true);
  }

  bool get ownerPairingBusy =>
      _connectAttemptInProgress ||
      _productPreparation != null ||
      (_productReadinessPending &&
          _capabilityDiscoveryByDevice.containsKey(connectedId));
  bool get ownerPairingNeedsFreshConnection {
    final status = _ownerPairingSession?.status;
    return _productPreparationFailed &&
        _productPreparation == null &&
        !_connectAttemptInProgress &&
        isConnected &&
        !productReady &&
        status?.state == OwnerPairingSessionState.ownedOther &&
        status?.reason == OwnerPairingSessionReason.none &&
        status?.ownerRetained == true &&
        status?.identityDurable == true &&
        status?.canBegin == false;
  }

  String? get durableBoardIdentity => _ownerPairingSession?.durableIdentity;
  bool get ownerPairingClaimLimitReached {
    final pairing = _ownerPairingSession;
    final identity = pairing?.durableIdentity ?? connectedId;
    return pairing?.status?.canBegin == true &&
        identity != null &&
        !_ownerClaimRetryRequiredBoards.contains(identity) &&
        _ownerClaimRetryRequiredBoards.length >= _maximumOwnerClaimIntents;
  }

  /// One generation owns consent, native progress and final durable proof.
  /// A connected BLE link remains connected while this boundary is pending.
  Future<bool> ensureProductReady(String deviceId, {bool retry = false}) {
    if (identifyConnectionActive) return Future.value(false);
    if (productReady && connectedId == deviceId) return Future.value(true);
    final current = _productPreparation;
    if (current != null) return current;
    if (retry && connectedId == deviceId && ownerPairingNeedsFreshConnection) {
      return _retryQuietOwnerConnection(deviceId);
    }
    if (_productPreparationFailed &&
        !retry &&
        _ownerPairingSession?.ready != true) {
      return Future.value(false);
    }
    _productPreparationFailed = false;
    final generation = _connectionGeneration;
    bool owns() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    final future = () async {
      try {
        final capabilities = await fetchDeviceCapabilities(deviceId: deviceId);
        if (!owns()) return false;
        if (!capabilities.supportsConnectionSession) {
          _pushAppLog(
            'Board capabilities are unavailable. Reconnect the board; if this continues, install the current firmware over USB.',
          );
          return false;
        }
        final session = await openConnectionSession(deviceId);
        if (!owns() || session == null) return false;
        if (!capabilities.supportsOwnerPairing) {
          _productReadinessPending = false;
          _publishProductReadiness(true);
          return true;
        }
        _productReadinessPending = true;
        // Native connection alone grants no product authority.
        _publishProductReadiness(false, force: true);
        _ownerPairingStatusController.add(ownerPairingStatus);
        // Pairing uses an activated transport before product access is granted.
        // Its ordinary heartbeat must survive a consent timeout or a slow
        // native proof, without opening Settings/Console/Home to the peer.
        if (_shouldRunConnectedBridges()) _startKeepAlive(deviceId);
        var pairing = _ownerPairingSession;
        if (pairing == null || !identical(pairing.session, session)) {
          await _ownerPairingSubscription?.cancel();
          if (!owns() || !identical(session, _activeConnectionSession)) {
            return false;
          }
          pairing = OwnerPairingSession(
            session,
            readinessTimeout: _ownerPairingReadinessTimeout,
          );
          _ownerPairingSession = pairing;
          (OwnerPairingSessionState, OwnerPairingSessionReason, int, int)?
          lastTracedStatus;
          _ownerPairingSubscription = pairing.statuses.listen((status) {
            if (!owns() || !identical(pairing, _ownerPairingSession)) return;
            final traceStatus = (
              status.state,
              status.reason,
              status.flags,
              status.attempt,
            );
            if (traceStatus != lastTracedStatus) {
              lastTracedStatus = traceStatus;
              _trace(
                'ble.owner_pairing_state',
                fields: <String, Object?>{
                  'generation': generation,
                  'state': status.state.name,
                  'reason': status.reason.name,
                  'flags': status.flags,
                  'attempt': status.attempt,
                },
              );
            }
            _recoverFailedOwnerVerification();
            if (!owns()) return;
            if (defaultTargetPlatform == TargetPlatform.android &&
                status.nativeBondAllowed) {
              unawaited(_startOwnerPhoneBond(pairing!, deviceId, generation));
            }
            if (status.foreignOwnerRefused) {
              _ownerPairingRefusal = status;
              _ownerPairingRefusedDeviceId = deviceId;
              _ownerPairingStatusController.add(status);
              _publishProductReadiness(false);
              // Finish the synchronous State delivery before disposing its
              // streams. A newer connection owns its own teardown lifetime.
              scheduleMicrotask(() {
                if (owns() && identical(pairing, _ownerPairingSession)) {
                  unawaited(disconnect());
                }
              });
              return;
            }
            if (_ownerPairingSession?.ready != true) {
              if (_publishedProductReady) {
                // Revocation retires retained domain authority. A later proof
                // must publish even an unchanged Home/Settings replay again.
                _clearRetainedHomeState();
                _failPendingSettings(LitchSettingWriteFailure.interrupted);
                unawaited(
                  _safeCancelSubscription(
                    _invalidateSettingsSession(clearCachedState: true),
                    'settings ownership suspension failed',
                  ),
                );
              }
              _productReadinessPending = true;
            }
            _ownerPairingStatusController.add(status);
            _publishProductReadiness(productReady);
            if (_productPreparation == null &&
                !productReady &&
                _ownerPairingSession?.ready == true) {
              unawaited(ensureProductReady(deviceId));
            }
          });
        }
        _productReadinessPending = true;
        if (pairing.ready) {
          return _finishOwnerReadiness(pairing, deviceId, generation);
        }
        var status = await pairing.readStatus();
        if (pairing.awaitingInitialIdentity) {
          status = await pairing.waitForInitialIdentity();
          if (status.state == OwnerPairingSessionState.unpaired &&
              status.reason != OwnerPairingSessionReason.none) {
            return false;
          }
        }
        if (!owns()) return false;
        if (!pairing.ready) {
          if (status.canBegin) {
            // Automatic reconnect is not permission for another OS prompt.
            // Consume this intent before sending so late delivery cannot rearm it.
            final claimIdentity = pairing.durableIdentity ?? deviceId;
            if (retry) _ownerClaimRetryRequiredBoards.remove(claimIdentity);
            if (!_ownerClaimRetryRequiredBoards.contains(claimIdentity) &&
                _ownerClaimRetryRequiredBoards.length >=
                    _maximumOwnerClaimIntents) {
              return false;
            }
            if (!_ownerClaimRetryRequiredBoards.add(claimIdentity)) {
              return false;
            }
            final admitted = await pairing.begin(
              phoneInitiated:
                  defaultTargetPlatform == TargetPlatform.android &&
                  _pairingRecoveryGateway
                      is! UnavailableBluetoothPairingRecovery,
            );
            if (admitted.disposition != ConnectionSessionDisposition.applied) {
              await pairing.readStatus();
              return false;
            }
          } else if (!status.claimPending &&
              status.state != OwnerPairingSessionState.ownerReady &&
              !(status.state == OwnerPairingSessionState.ownedOther &&
                  status.reason == OwnerPairingSessionReason.none)) {
            return false;
          }
          await pairing.waitUntilReady();
        }
        return _finishOwnerReadiness(pairing, deviceId, generation);
      } catch (error) {
        if (owns()) {
          final phoneBondPending =
              _ownerPairingSession?.phoneBondRequested == true;
          _ownerPairingSession?.retireNativeBondAttempt();
          _productReadinessPending = deviceCapabilities(
            deviceId,
          ).supportsOwnerPairing;
          _publishProductReadiness(false);
          _pushAppLog('Board confirmation unavailable: $error');
          if (phoneBondPending) await disconnect();
        }
        return false;
      }
    }();
    _productPreparation = future;
    _persistConsoleSnapshot();
    return future.whenComplete(() {
      if (identical(_productPreparation, future)) {
        _productPreparation = null;
        if (owns()) {
          _productPreparationFailed = !productReady;
          _publishProductReadiness(productReady);
          _ownerPairingStatusController.add(ownerPairingStatus);
          if (!productReady && _ownerPairingSession?.ready == true) {
            unawaited(ensureProductReady(deviceId));
          }
        }
      }
    });
  }

  Future<bool> _retryQuietOwnerConnection(String deviceId) async {
    // The ordinary connect owner retires logical authority synchronously and
    // waits for native/GATT cleanup before admitting the same chosen board.
    final connecting = connect(deviceId);
    final generation = _connectionGeneration;
    if (!await connecting ||
        generation != _connectionGeneration ||
        !isConnected ||
        connectedId != deviceId) {
      return false;
    }
    return ensureProductReady(deviceId);
  }

  Future<void> _startOwnerPhoneBond(
    OwnerPairingSession pairing,
    String deviceId,
    int generation,
  ) async {
    bool owns() =>
        !_disposed &&
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId &&
        identical(pairing, _ownerPairingSession);
    if (!owns()) return;
    final attempt = pairing.takeNativeBondAttempt();
    if (attempt == null || !owns()) return;
    BluetoothPairingBondStartResult result;
    try {
      result = await _pairingRecoveryGateway.startBond(
        deviceId,
        generation,
        attempt,
      );
    } catch (_) {
      result = BluetoothPairingBondStartResult.failed;
    }
    if (!owns()) return;
    if (result == BluetoothPairingBondStartResult.alreadyBonded) {
      _suspendForStalePairing(deviceId);
    } else if (result != BluetoothPairingBondStartResult.started) {
      _pushAppLog('Bluetooth pairing could not start on this device.');
      await disconnect();
    }
  }

  Future<bool> _finishOwnerReadiness(
    OwnerPairingSession pairing,
    String deviceId,
    int generation,
  ) async {
    bool owns() =>
        generation == _connectionGeneration &&
        connectedId == deviceId &&
        isConnected &&
        identical(pairing, _ownerPairingSession) &&
        pairing.ready;
    if (!owns()) return false;
    try {
      await _rememberOwnedBoard(pairing.durableIdentity!, deviceId);
    } catch (error) {
      // Phone metadata cannot veto the board's proven durable owner. Preserve
      // unreadable registry bytes; history pauses at its own identity lookup.
      if (owns()) {
        _pushAppLog('Board confirmed; saved board list unavailable: $error');
      }
    }
    if (!owns()) return false;
    _ownerClaimRetryRequiredBoards.remove(pairing.durableIdentity ?? deviceId);
    _productReadinessPending = false;
    _publishProductReadiness(true);
    // The original preparation consumer may have already returned (for
    // example while native proof was unavailable). Resume its same policy.
    unawaited(_applyConnectedBridgePolicy(deviceId: deviceId));
    _bindSharedOtaStatus(pairing.session);
    unawaited(
      _requestConnectionStateSnapshot(pairing.session).catchError((
        Object error,
      ) {
        _pushAppLog('Board state replay could not be confirmed: $error');
        return false;
      }),
    );
    return true;
  }

  Future<void> cancelOwnerPairing() async {
    final pairing = _ownerPairingSession;
    if (pairing == null) return;
    await pairing.cancel();
    // Cancel ACK is delivery only; the board's current State decides the result.
    await pairing.readStatus();
  }

  static const _ownedBoardsKey = 'owned_litch_boards_v1';
  bool _ownedBoardsNeedReload = false;
  Future<void> _ownedBoardsWork = Future<void>.value();
  Future<Map<String, RememberedOwnedBoard>> rememberedOwnedBoards() async {
    final preferences = await SharedPreferences.getInstance();
    final raw = preferences.getString(_ownedBoardsKey);
    if (raw == null) return {};
    final decoded = jsonDecode(raw);
    if (decoded is! Map<String, dynamic>) {
      throw const FormatException('Invalid owned board registry.');
    }
    final result = <String, RememberedOwnedBoard>{};
    for (final entry in decoded.entries) {
      final value = entry.value;
      if (!RegExp(r'^[0-9a-f]{32}$').hasMatch(entry.key) ||
          entry.key == '00000000000000000000000000000000' ||
          value is! Map ||
          value['aliases'] is! List ||
          !(value['aliases'] as List).every((alias) => alias is String) ||
          value['historyStorageKey'] is! String ||
          !RegExp(
            r'^[0-9a-f]{32}$',
          ).hasMatch(value['historyStorageKey'] as String)) {
        throw const FormatException('Invalid owned board registry.');
      }
      result[entry.key] = (
        aliases: List<String>.from(value['aliases'] as List),
        historyStorageKey: value['historyStorageKey'] as String,
        bluetoothSuffix:
            value['bluetoothSuffix'] is String &&
                RegExp(
                  r'^[0-9A-F]{4}$',
                ).hasMatch(value['bluetoothSuffix'] as String)
            ? value['bluetoothSuffix'] as String
            : null,
      );
    }
    return result;
  }

  static String _platformAliasHistoryStorageKey(String deviceId) =>
      sha256.convert(utf8.encode(deviceId)).toString().substring(0, 32);

  /// Keep the first known platform alias's existing history key. Durable board
  /// identity joins later BLE aliases without renaming or discarding old data.
  Future<String> historyStorageIdentity(String deviceId) async {
    final identity = connectedId == deviceId ? durableBoardIdentity : null;
    if (identity == null) return _platformAliasHistoryStorageKey(deviceId);
    final records = await rememberedOwnedBoards();
    return records[identity]?.historyStorageKey ??
        _platformAliasHistoryStorageKey(deviceId);
  }

  Future<void> _rememberOwnedBoard(
    String identity,
    String deviceId, {
    String? bluetoothSuffix,
  }) {
    final generation = _connectionGeneration;
    final work = _ownedBoardsWork.then((_) async {
      if (generation != _connectionGeneration) return;
      await _rememberOwnedBoardNow(
        identity,
        deviceId,
        generation,
        bluetoothSuffix,
      );
    });
    // Registry storage outlives a connection. Keep its mutations ordered even
    // when a retired connection's platform save has not returned yet.
    _ownedBoardsWork = work.then<void>((_) {}, onError: (Object _) {});
    return work;
  }

  Future<void> _rememberOwnedBoardNow(
    String identity,
    String deviceId,
    int generation,
    String? bluetoothSuffix,
  ) async {
    final preferences = await SharedPreferences.getInstance();
    if (_ownedBoardsNeedReload) {
      // A failed save still changes the plugin cache. Only platform readback
      // can establish whether that alias was actually persisted.
      await preferences.reload();
      _ownedBoardsNeedReload = false;
    }
    final records = await rememberedOwnedBoards();
    if (generation != _connectionGeneration) return;
    final record = records.putIfAbsent(
      identity,
      () => (
        aliases: <String>[],
        historyStorageKey: _platformAliasHistoryStorageKey(deviceId),
        bluetoothSuffix: null,
      ),
    );
    final suffix = bluetoothSuffix ?? record.bluetoothSuffix;
    if (record.aliases.contains(deviceId) && suffix == record.bluetoothSuffix) {
      return;
    }
    if (!record.aliases.contains(deviceId)) record.aliases.add(deviceId);
    records[identity] = (
      aliases: record.aliases,
      historyStorageKey: record.historyStorageKey,
      bluetoothSuffix: suffix,
    );
    final encoded = records.map(
      (identity, value) => MapEntry(identity, {
        'aliases': value.aliases,
        'historyStorageKey': value.historyStorageKey,
        if (value.bluetoothSuffix != null)
          'bluetoothSuffix': value.bluetoothSuffix,
      }),
    );
    _ownedBoardsNeedReload = true;
    if (!await preferences.setString(_ownedBoardsKey, jsonEncode(encoded))) {
      throw StateError('The owned board could not be remembered.');
    }
    _ownedBoardsNeedReload = false;
  }

  int _bluetoothSuffixGeneration = -1;
  Future<String?>? _bluetoothSuffixRead;
  String? _bluetoothSuffix;

  String? get connectedBluetoothSuffix =>
      productReady && _bluetoothSuffixGeneration == _connectionGeneration
      ? _bluetoothSuffix
      : null;

  /// Optional paired metadata. Transport aliases never supply these digits.
  Future<String?> fetchBoardBluetoothSuffix({required String deviceId}) {
    if (!isConnected ||
        connectedId != deviceId ||
        !productReady ||
        identifyConnectionActive ||
        durableBoardIdentity == null ||
        deviceCapabilities(deviceId).recoveryOnly) {
      return Future.value(null);
    }
    final generation = _connectionGeneration;
    if (_bluetoothSuffixGeneration == generation &&
        _bluetoothSuffixRead != null) {
      return _bluetoothSuffixRead!;
    }
    _bluetoothSuffixGeneration = generation;
    _bluetoothSuffix = null;
    return _bluetoothSuffixRead = _readBoardBluetoothSuffix(
      deviceId,
      durableBoardIdentity!,
      generation,
    );
  }

  Future<String?> _readBoardBluetoothSuffix(
    String deviceId,
    String identity,
    int generation,
  ) async {
    bool current() =>
        !_disposed &&
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId &&
        productReady &&
        durableBoardIdentity == identity;
    try {
      final remembered =
          (await rememberedOwnedBoards())[identity]?.bluetoothSuffix;
      if (!current()) return null;
      if (remembered != null) return _bluetoothSuffix = remembered;
      final channel = await _ensureOtaControlChannel(deviceId);
      if (!current() || !_isActiveOtaChannel(deviceId, channel)) return null;
      final cancelled = Completer<void>();
      try {
        final response = channel.waitFor(
          (event) =>
              event.kind == OtaStatusKind.bluetoothSuffix ||
              event.kind == OtaStatusKind.deviceInfo,
          timeout: const Duration(seconds: 2),
          cancelled: cancelled.future,
        );
        // Old firmware ignores the selector and returns ordinary device info.
        // Do not send an unknown command or consume another request's errors.
        if (!await _sendOtaCommandForChannel(
          deviceId,
          channel,
          <String, Object>{'cmd': 'info', 'bs': 1},
          attempts: 1,
          delay: Duration.zero,
          isCurrent: current,
        )) {
          return null;
        }
        final event = await response;
        if (!current() ||
            !_isActiveOtaChannel(deviceId, channel) ||
            event?.deviceId != deviceId ||
            event?.generation != channel.generation) {
          return null;
        }
        final suffix = event?.bluetoothSuffix;
        if (suffix == null) return null;
        _bluetoothSuffix = suffix;
        await _rememberOwnedBoard(identity, deviceId, bluetoothSuffix: suffix);
        return current() ? suffix : null;
      } finally {
        cancelled.complete();
      }
    } catch (_) {
      // Display metadata cannot delay readiness or make a connection fail.
      return current() ? _bluetoothSuffix : null;
    }
  }

  /// Litchcraft shares this connection's resolved GATT instances and generation
  /// checks. Its typed frames never pass through diagnostic console parsing.
  Stream<List<int>> litchcraftPackets(String deviceId) async* {
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return;
    }
    final session = await openConnectionSession(deviceId);
    {
      if (generation != _connectionGeneration ||
          !isConnected ||
          connectedId != deviceId) {
        return;
      }
      if (session?.isActivated != true) {
        throw StateError('Shared Litchcraft session is unavailable');
      }
    }
    if (session?.isActivated == true) {
      final assembler = ConnectionLitchcraftAssembler();
      // Forward cancellation to the borrowed subscription even when no board
      // receipt is pending. An await-for loop can wait for the next receipt,
      // blocking domain cleanup while the common session is correctly kept open.
      yield* session!.objectReceipts
          .takeWhile(
            (_) =>
                generation == _connectionGeneration &&
                isConnected &&
                connectedId == deviceId,
          )
          .expand<List<int>>((packet) {
            if (packet.code == LitchcraftV2Protocol.physicalStoreBatch) {
              if (LitchcraftV2Packet.decode(packet.body) != null) {
                return [packet.body];
              }
              _pushAppLog('Ignoring malformed shared physical Store batch');
              return const [];
            }
            if (packet.code != ConnectionLitchcraftCodec.receiptCode) {
              return const [];
            }
            try {
              final completed = assembler.accept(packet.body);
              return completed == null ? const [] : [completed];
            } on FormatException catch (error) {
              _pushAppLog(
                'Ignoring malformed shared Litchcraft packet: $error',
              );
              return const [];
            }
          });
      return;
    }
  }

  Future<void> _writeSharedLitchcraftPacket(
    ConnectionSession session,
    List<int> bytes, {
    Future<void>? cancellationFuture,
  }) async {
    final modeControl =
        bytes.length >= 2 &&
        bytes[0] == LitchcraftProtocol.magic &&
        (bytes[1] == LitchcraftProtocol.enter ||
            bytes[1] == LitchcraftProtocol.exit);
    if (modeControl) {
      if (!session.orderedModeSupported) {
        throw const LitchcraftModeUnavailable();
      }
      if (bytes.length != 8) {
        throw const FormatException('Invalid compact Litchcraft mode command');
      }
      final outcome = await session.sendOrdered(
        ConnectionSession.litchcraftModeOpcode,
        Uint8List.fromList(bytes),
      );
      if (outcome.disposition != ConnectionSessionDisposition.applied) {
        throw LitchcraftRejected(outcome.reason);
      }
      return;
    }
    final fragments = ConnectionLitchcraftCodec.encodePacket(
      Uint8List.fromList(bytes),
      maximumBodyBytes: session.objectMaximumBody,
    );
    return session
        .sendObjectBatch(
          fragments
              .map(
                (body) => (
                  opcode: ConnectionLitchcraftCodec.objectOpcode,
                  body: body,
                ),
              )
              .toList(growable: false),
          priority: ConnectionSessionObjectPriority.interactive,
          atomicMessage: true,
          cancellationFuture: cancellationFuture,
          messageEnds: [fragments.length],
        )
        .onError<ConnectionSessionObjectCancelled>(
          (error, stack) => throw const LitchcraftTransferCancelled(),
        );
  }

  Stream<List<int>> connectionSessionPackets(String deviceId) {
    final generation = _connectionGeneration;
    return _subscribeToCharacteristic(
      QualifiedCharacteristic(
        serviceId: BleProtocol.baseService,
        characteristicId: BleProtocol.connectionSessionEgressCharacteristic,
        deviceId: deviceId,
      ),
    ).where(
      (_) =>
          generation == _connectionGeneration &&
          isConnected &&
          connectedId == deviceId,
    );
  }

  Future<void> writeConnectionSessionPacket(String deviceId, List<int> bytes) {
    final generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    if (!current() ||
        bytes.length < ConnectionSessionCodec.headerBytes ||
        bytes.length > _litchcraftPacketPayloadBytes) {
      throw StateError('Connection-session transport is unavailable');
    }
    return _writeCharacteristicWithoutResponse(
      QualifiedCharacteristic(
        serviceId: BleProtocol.baseService,
        characteristicId: BleProtocol.connectionSessionIngressCharacteristic,
        deviceId: deviceId,
      ),
      value: bytes,
      isCurrent: current,
    );
  }

  Future<void> writeConnectionSessionPackets(
    String deviceId,
    List<Uint8List> packets,
  ) {
    final generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    if (!current() ||
        packets.isEmpty ||
        packets.length > _maximumLitchcraftBulkBatchPackets ||
        packets.any(
          (bytes) =>
              bytes.length < ConnectionSessionCodec.headerBytes ||
              bytes.length > _litchcraftPacketPayloadBytes,
        )) {
      throw StateError('Connection-session batch transport is unavailable');
    }
    return _writeCharacteristicBatchWithoutResponse(
      QualifiedCharacteristic(
        serviceId: BleProtocol.baseService,
        characteristicId: BleProtocol.connectionSessionIngressCharacteristic,
        deviceId: deviceId,
      ),
      values: packets,
      isCurrent: current,
    );
  }

  /// Opens the one negotiated transport owner for this native connection.
  /// Missing current capabilities never select another domain transport.
  Future<ConnectionSession?> openConnectionSession(String deviceId) {
    if (_repairingGattInventory) return Future<ConnectionSession?>.value();
    final active = _activeConnectionSession;
    if (active != null && active.isActivated) {
      return Future<ConnectionSession?>.value(active);
    }
    final opening = _openingConnectionSession;
    if (opening != null) return opening;
    final future = () async {
      if (active != null && !active.isActivated) {
        // The peripheral still owns the old session until the BLE connection
        // ends. A fresh Hello on that link cannot activate a second session.
        // Reuse the connection owner's existing physical reconnect path so
        // every domain observes one new generation and old callbacks are fenced.
        if (!isConnected ||
            connectedId != deviceId ||
            !await connect(deviceId)) {
          return null;
        }
      }
      final generation = _connectionGeneration;
      // The Object lane inherits this connection's negotiated packet size.
      // Do not open it while BLE calibration is still in flight, otherwise a
      // healthy high-MTU link is pinned to the safe minimum for its lifetime.
      await _ensureLinkCalibrated(deviceId);
      if (generation != _connectionGeneration ||
          !isConnected ||
          connectedId != deviceId) {
        return null;
      }
      final known = deviceCapabilities(deviceId);
      final capabilities = known.explicitlyReported
          ? known
          : await fetchDeviceCapabilities(deviceId: deviceId);
      if (!capabilities.supportsConnectionSession ||
          generation != _connectionGeneration ||
          !isConnected ||
          connectedId != deviceId) {
        return null;
      }
      final random = Random.secure();
      final nonce =
          (random.nextInt(0x7fffffff) << 32) | random.nextInt(0xffffffff);
      var notificationFailed = false;
      late final ConnectionSession session;
      session = ConnectionSession(
        packets: connectionSessionPackets(deviceId).transform(
          StreamTransformer<List<int>, List<int>>.fromHandlers(
            handleError: (error, stack, sink) {
              notificationFailed = true;
              sink.addError(error, stack);
            },
            handleDone: (sink) {
              notificationFailed = true;
              sink.close();
            },
          ),
        ),
        send: (packet) => writeConnectionSessionPacket(deviceId, packet),
        sendBatch: (packets) =>
            writeConnectionSessionPackets(deviceId, packets),
        clientNonce: nonce == 0 ? 1 : nonce,
        packetMaximum: _litchcraftPacketPayloadBytes,
        receivedHeadCompletionTimeout: _receivedHeadCompletionTimeout,
        canSubmitObject: (opcode) {
          // Activation grants transport, not access to private product Objects.
          // Public support is the sole pre-owner Object allowed by firmware.
          if (capabilities.supportsOwnerPairing &&
              !capabilities.recoveryOnly &&
              !productReady &&
              opcode != 0x45) {
            return false;
          }
          final pending = pendingOtaVerification;
          final updateOwnsObjects =
              _otaUploadActive ||
              (pending?.deviceId == deviceId &&
                  pending?.uploadConnectionToken == generation);
          return !updateOwnsObjects ||
              (!_otaUploadActive && opcode == 0x45) ||
              opcode == ConnectionOtaCodec.dataObjectOpcode ||
              opcode == ConnectionOtaCodec.controlObjectOpcode;
        },
        onStateReadCompleted: () {
          if (generation != _connectionGeneration ||
              !identical(session, _activeConnectionSession)) {
            return;
          }
          final passive = _connectionStateRecovery;
          if (passive == null) {
            _recoverIncompleteConnectionState();
          } else {
            // A later successful explicit read is a new authority boundary.
            // An earlier passive deadline may have expired while it ran.
            unawaited(
              passive.then((_) {
                if (generation == _connectionGeneration &&
                    identical(session, _activeConnectionSession)) {
                  _recoverIncompleteConnectionState();
                }
              }),
            );
          }
        },
        onRetired: (fields) {
          _trace(
            'ble.connection_session_retired',
            fields: <String, Object?>{'generation': generation, ...fields},
          );
          final line =
              'BLE shared session retired: generation=$generation '
              'cause=${fields['cause']} kind=${fields['error_kind']} '
              'ordered_pending=${fields['ordered_pending']} '
              'ord_head_seq=${fields['ord_head_seq'] ?? '-'} '
              'ord_head_op=${fields['ord_head_op'] ?? '-'} '
              'ord_terminal=${fields['ord_terminal']} '
              'ord_ahead=${fields['ord_ahead']} '
              'object_pending=${fields['object_pending']} '
              'head_op=${fields['head_op'] ?? '-'} '
              'head_seq=${fields['head_seq'] ?? '-'} '
              'obj_accepted=${fields['obj_accepted']} '
              'obj_grant=${fields['obj_grant']} '
              'reverse_confirmed=${fields['reverse_confirmed']} '
              'reverse_grant=${fields['reverse_grant']} '
              'obj_query=${fields['obj_query'] == true ? 1 : 0} '
              'query_write=${fields['query_write'] == true ? 1 : 0}';
          _pushAppLog(line.length <= 512 ? line : line.substring(0, 512));
        },
      );
      final stateSubscription = session.states.listen(
        _captureConnectionSessionState,
      );
      // Even partial activation may leave a frozen transcript on the board.
      // Retain its owner so a later open must cross the native reconnect barrier.
      _activeConnectionSession = session;
      try {
        await session.activate();
      } catch (error) {
        _retireConnectionPreparationFailure(
          deviceId,
          generation,
          stage: notificationFailed
              ? 'session_notification'
              : 'session_activation',
          error: error,
        );
        await stateSubscription.cancel();
        await session.dispose();
        rethrow;
      }
      if (generation != _connectionGeneration ||
          !isConnected ||
          connectedId != deviceId) {
        await stateSubscription.cancel();
        await session.dispose();
        return null;
      }
      await _connectionSessionStateSubscription?.cancel();
      _connectionSessionStateSubscription = stateSubscription;
      _activeConnectionSession = session;
      _pushAppLog(
        'BLE shared session active: packet_max=${session.selectedPacketMaximum} '
        'object_body=${session.objectMaximumBody} '
        'object_capacity=${session.objectSelectedCapacity} '
        'ordered_body=${session.orderedMaximumBody} '
        'ordered_capacity=${session.orderedSelectedCapacity}',
      );
      if (!identifyConnectionActive &&
          (!capabilities.supportsOwnerPairing || productReady)) {
        _bindSharedOtaStatus(session);
      }
      _retireDirectSettingsBridgeIfIdle();
      if (!identifyConnectionActive &&
          !capabilities.recoveryOnly &&
          !capabilities.supportsOwnerPairing) {
        unawaited(
          _requestConnectionStateSnapshot(session).catchError((Object error) {
            _pushAppLog('Board state replay could not be confirmed: $error');
            return false;
          }),
        );
      }
      return session;
    }();
    _openingConnectionSession = future;
    return future.whenComplete(() {
      if (identical(_openingConnectionSession, future)) {
        _openingConnectionSession = null;
      }
    });
  }

  void _retireConnectionPreparationFailure(
    String deviceId,
    int generation, {
    required String stage,
    required Object error,
  }) {
    if (generation != _connectionGeneration ||
        !isConnected ||
        connectedId != deviceId ||
        _repairingGattInventory ||
        (identifyConnectionActive && stage != 'session_activation')) {
      return;
    }
    _trace(
      'ble.connection_preparation_failed',
      fields: {'generation': generation, 'stage': stage},
    );
    _pushAppLog('BLE connection preparation failed during $stage: $error');
    // Fresh GATT inventory cannot repair notification ownership or a frozen
    // partial session transcript. Cross the existing native cleanup barrier
    // before admitting a successor; never wait for the board's idle timeout.
    unawaited(disconnect());
  }

  Future<void> _openSharedSessionIfSupported(
    String deviceId,
    int generation,
  ) async {
    try {
      final capabilities = await fetchDeviceCapabilities(deviceId: deviceId);
      if (generation != _connectionGeneration) return;
      if (!identifyConnectionActive &&
          capabilities.supportsIdentify &&
          !capabilities.supportsOwnerPairing) {
        unawaited(_stopIdentify(deviceId));
      }
      if (!capabilities.supportsConnectionSession) {
        return;
      }
      await openConnectionSession(deviceId);
      if (!identifyConnectionActive && capabilities.supportsOwnerPairing) {
        await ensureProductReady(deviceId);
      }
    } catch (error) {
      if (generation == _connectionGeneration) {
        _pushAppLog('Shared connection session unavailable: $error');
      }
    }
  }

  Future<bool> _requestConnectionStateSnapshot(
    ConnectionSession session, {
    bool homeOnly = false,
  }) async {
    final home = HomeStateWireAssembler();
    final settings = SettingsStateWireAssembler();
    var hasHome = false;
    var hasSettings = homeOnly;
    return session.readStateSnapshot<bool>(
      accept: (packet) {
        hasHome =
            hasHome ||
            home.accept(
                  code: packet.code,
                  revision: packet.sequence,
                  body: packet.body,
                  sequence: 1,
                ) !=
                null;
        hasSettings =
            hasSettings ||
            settings.accept(
                  code: packet.code,
                  revision: packet.sequence,
                  body: packet.body,
                ) !=
                null;
        return hasHome && hasSettings ? true : null;
      },
    );
  }

  /// Mode controls use the same current session as artwork and State.
  Future<bool> litchcraftControlAvailable(String deviceId) async {
    if (!isConnected || connectedId != deviceId) return false;
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (!capabilities.supportsLitchcraft || capabilities.recoveryOnly) {
      return false;
    }
    final session = await openConnectionSession(deviceId);
    return generation == _connectionGeneration && session?.isActivated == true;
  }

  Future<void> writeLitchcraftPacket(
    String deviceId,
    List<int> bytes, {
    bool withResponse = true,
  }) async {
    if (!isConnected ||
        connectedId != deviceId ||
        bytes.length < 8 ||
        bytes.length > 20) {
      throw StateError('Litchcraft transport is unavailable');
    }
    final generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (!current()) throw StateError('Litchcraft connection changed');
    if (!capabilities.supportsLitchcraft || capabilities.recoveryOnly) {
      throw StateError('Litchcraft is unavailable on this connection');
    }
    if (!isConnected ||
        connectedId != deviceId ||
        !capabilities.supportsLitchcraft) {
      throw StateError('Litchcraft transport is unavailable');
    }
    if (!current()) throw StateError('Litchcraft connection changed');
    final session = await openConnectionSession(deviceId);
    if (!current()) throw StateError('Litchcraft connection changed');
    if (session?.isActivated != true) {
      throw StateError('Shared Litchcraft session is unavailable');
    }
    if (session?.isActivated == true) {
      return _writeSharedLitchcraftPacket(session!, bytes);
    }
  }

  /// Current-game sync shares the connection's retained Object stream.
  Stream<List<int>> gameHistoryPackets(String deviceId) async* {
    final int generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return;
    }
    ConnectionSession? connectionSession = _activeConnectionSession;
    if ((connectionSession == null || !connectionSession.isActivated) &&
        capabilities.supportsConnectionSession) {
      connectionSession = await openConnectionSession(deviceId);
    }
    if (generation != _connectionGeneration ||
        !isConnected ||
        connectedId != deviceId) {
      return;
    }
    if (connectionSession != null &&
        connectionSession.isActivated &&
        capabilities.supportsGameHistory) {
      yield* _connectionGameHistoryPackets(connectionSession);
      return;
    }
  }

  Stream<List<int>> _connectionGameHistoryPackets(
    ConnectionSession connectionSession,
  ) {
    final ConnectionGameHistoryAssembler assembler =
        ConnectionGameHistoryAssembler();
    // Cancellation must reach the session subscription while no next packet
    // exists, including after a completed pull or an ownership revocation.
    return connectionSession.objectReceipts
        .where(
          (packet) => packet.code == ConnectionGameHistoryCodec.receiptCode,
        )
        .map((packet) => assembler.accept(packet.body))
        .where((frame) => frame != null)
        .cast<Uint8List>();
  }

  Future<void> writeGameHistoryPacket(String deviceId, List<int> bytes) async {
    if (!isConnected ||
        connectedId != deviceId ||
        !deviceCapabilities(deviceId).supportsGameHistory ||
        bytes.length < 8 ||
        bytes.length > 20) {
      throw StateError('Game-history transport is unavailable');
    }
    final int generation = _connectionGeneration;
    ConnectionSession? connectionSession = _activeConnectionSession;
    if ((connectionSession == null || !connectionSession.isActivated) &&
        (_openingConnectionSession != null ||
            deviceCapabilities(deviceId).supportsConnectionSession)) {
      connectionSession = await openConnectionSession(deviceId);
    }
    if (connectionSession != null && connectionSession.isActivated) {
      final List<Uint8List> fragments = ConnectionGameHistoryCodec.encodeFrame(
        Uint8List.fromList(bytes),
        maximumBodyBytes: connectionSession.objectMaximumBody,
      );
      await connectionSession.sendObjectBatch(
        fragments
            .map(
              (Uint8List body) =>
                  (opcode: ConnectionGameHistoryCodec.objectOpcode, body: body),
            )
            .toList(growable: false),
        priority: ConnectionSessionObjectPriority.bulk,
        atomicMessage: true,
      );
      if (generation != _connectionGeneration ||
          !identical(connectionSession, _activeConnectionSession) ||
          !isConnected ||
          connectedId != deviceId) {
        throw StateError('Game-history connection changed');
      }
      return;
    }
    throw StateError('Shared game-history session is unavailable');
  }

  /// The effective ATT payload belongs to this physical connection. The pinned
  /// Android plugin reports MTU; its Apple implementation reports payload bytes.
  Future<int> litchcraftPacketPayloadBytes(
    String deviceId, {
    required int connectionSessionToken,
  }) async {
    final gattGeneration = _gattSessionGeneration;
    bool current() =>
        !_disposed &&
        isConnected &&
        connectedId == deviceId &&
        connectionSessionToken == _connectionGeneration &&
        gattGeneration == _gattSessionGeneration;
    if (!current()) throw StateError('Litchcraft connection changed');
    await _ensureLinkCalibrated(deviceId);
    if (!current()) throw StateError('Litchcraft connection changed');
    return _litchcraftPacketPayloadBytes;
  }

  Future<void> writeLitchcraftV2Packet(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
    bool Function()? isCurrent,
    Future<void>? cancellationFuture,
  }) async {
    final gattGeneration = _gattSessionGeneration;
    bool current() {
      if (isCurrent != null && !isCurrent()) {
        throw const LitchcraftTransferCancelled();
      }
      return !_disposed &&
          connectionSessionToken == _connectionGeneration &&
          gattGeneration == _gattSessionGeneration &&
          isConnected &&
          connectedId == deviceId;
    }

    if (!current() ||
        bytes.length < 9 ||
        bytes.length > _litchcraftPacketPayloadBytes ||
        bytes[0] != LitchcraftV2Protocol.magic ||
        bytes[1] != LitchcraftV2Protocol.version ||
        (bytes.length > 20 &&
            bytes[2] != LitchcraftV2Protocol.replaceChunkOp)) {
      throw StateError('Litchcraft v2 transport is unavailable');
    }
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (!capabilities.supportsLitchcraft || capabilities.recoveryOnly) {
      throw StateError('Litchcraft is unavailable on this connection');
    }
    if (!current()) throw StateError('Litchcraft connection changed');
    final session = await openConnectionSession(deviceId);
    if (!current()) throw StateError('Litchcraft connection changed');
    if (session?.isActivated != true) {
      throw StateError('Shared Litchcraft session is unavailable');
    }
    if (session?.isActivated == true) {
      await _writeSharedLitchcraftPacket(
        session!,
        bytes,
        cancellationFuture: cancellationFuture,
      );
      if (!current() || !identical(session, _activeConnectionSession)) {
        throw StateError('Litchcraft connection changed');
      }
      return;
    }
  }

  Future<void> writeLitchcraftBulkDocumentPacket(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
  }) async {
    final gattGeneration = _gattSessionGeneration;
    bool current() =>
        !_disposed &&
        isConnected &&
        connectedId == deviceId &&
        connectionSessionToken == _connectionGeneration &&
        gattGeneration == _gattSessionGeneration;
    if (!current() ||
        bytes.length < 9 ||
        bytes.length > _litchcraftPacketPayloadBytes ||
        bytes[0] != LitchcraftV2Protocol.magic ||
        bytes[1] != 2 ||
        (bytes[2] != LitchcraftV2Protocol.streamChunk &&
            bytes[2] != LitchcraftV2Protocol.streamCommit &&
            bytes[2] != LitchcraftV2Protocol.streamCheckpoint)) {
      throw StateError('Litchcraft bulk document transport is unavailable');
    }
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (!capabilities.supportsLitchcraft || capabilities.recoveryOnly) {
      throw StateError('Litchcraft is unavailable on this connection');
    }
    if (!current()) throw StateError('Litchcraft connection changed');
    final session = await openConnectionSession(deviceId);
    if (!current()) throw StateError('Litchcraft connection changed');
    if (session?.isActivated != true) {
      throw StateError('Shared Litchcraft session is unavailable');
    }
    if (session?.isActivated == true) {
      await _writeSharedLitchcraftPacket(session!, bytes);
      if (!current() || !identical(session, _activeConnectionSession)) {
        throw StateError('Litchcraft connection changed');
      }
      return;
    }
  }

  Future<void> writeLitchcraftBulkArtwork(
    String deviceId,
    int session,
    int request,
    List<int> colors,
  ) async {
    final generation = _connectionGeneration;
    final gattGeneration = _gattSessionGeneration;
    bool isCurrent() =>
        !_disposed &&
        generation == _connectionGeneration &&
        gattGeneration == _gattSessionGeneration &&
        isConnected &&
        connectedId == deviceId;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (!isCurrent()) throw StateError('Litchcraft connection changed');
    if (!capabilities.supportsLitchcraft || capabilities.recoveryOnly) {
      throw StateError('Litchcraft is unavailable on this connection');
    }
    if (!isConnected ||
        connectedId != deviceId ||
        !deviceCapabilities(deviceId).supportsLitchcraftBulk) {
      throw StateError('Litchcraft bulk transport is unavailable');
    }
    await _ensureLinkCalibrated(deviceId);
    if (!isCurrent()) throw StateError('Litchcraft connection changed');
    final packetBytes = _litchcraftPacketPayloadBytes;
    // Small payloads use the bounded per-square controls instead.
    if (packetBytes < 64) {
      throw const LitchcraftBulkUnavailable();
    }
    final frames = LitchcraftProtocol.bulkReplaceFrames(
      session: session,
      request: request,
      colors: colors,
      maximumPacketBytes: packetBytes,
    );
    final sharedSession = await openConnectionSession(deviceId);
    if (!isCurrent() || sharedSession?.isActivated != true) {
      throw StateError('Shared Litchcraft session is unavailable');
    }
    for (final frame in frames) {
      await _writeSharedLitchcraftPacket(sharedSession!, frame);
      if (!isCurrent() || !identical(sharedSession, _activeConnectionSession)) {
        throw StateError('Litchcraft connection changed');
      }
    }
  }

  BoardHealthStatus? get latestBoardHealth => _latestBoardHealth;

  /// Read-only support requests share one deadline and never outlive their
  /// connection. Reopening while a timed-out native call settles returns busy.
  SupportAppActionSnapshot? get supportActionSnapshot {
    final sink = _appTrace.supportActions;
    return sink == null
        ? null
        : SupportAppActionSnapshot(
            actions: sink.userActions.actions,
            capturedMs: sink.supportMonotonicMs,
            omitted: sink.userActions.omitted,
          );
  }

  List<SupportAction> get supportUserActions =>
      _appTrace.supportActions?.userActions.actions ?? const [];
  int? get supportMonotonicMs => _appTrace.supportActions?.supportMonotonicMs;
  int get supportUserActionsOmitted =>
      _appTrace.supportActions?.userActions.omitted ?? 0;
  int _supportRequest = 0;

  Future<Uint8List> _readSupportSnapshot(
    String deviceId,
    bool publicOnly,
    int kind,
    SupportCollectionScope scope,
  ) async {
    scope.check();
    final session = await scope.native(() {
      // Support may await this connection's activation, but never renew a
      // retired shared owner through openConnectionSession's reconnect path.
      final active = _activeConnectionSession;
      final opening = _openingConnectionSession;
      if (!isConnected ||
          connectedId != deviceId ||
          (active != null && !active.isActivated && opening == null)) {
        throw StateError('Support shared session unavailable');
      }
      return opening ?? openConnectionSession(deviceId);
    });
    scope.check();
    if (session == null ||
        !session.isActivated ||
        (!publicOnly && !productReady)) {
      throw StateError('Support session unavailable');
    }
    _supportRequest = (_supportRequest + 1) & 0xffffffff;
    if (_supportRequest == 0) _supportRequest = 1;
    final request = _supportRequest;
    final opcode = publicOnly ? 0x45 : 0x44;
    // Reuse the existing shared Object fragment format/assembler. This does
    // not create a BLE subscription, transport timer or parallel repair lane.
    final assembler = ConnectionGameHistoryAssembler();
    final response = Completer<Uint8List>();
    final subscription = session.objectReceipts.listen(
      (packet) {
        if (response.isCompleted ||
            scope.reason != null ||
            packet.code != opcode ||
            !identical(session, _activeConnectionSession)) {
          return;
        }
        try {
          final bytes = assembler.accept(packet.body);
          if (bytes == null) return;
          if (bytes.length > 500 || bytes[0] != (publicOnly ? 2 : 1)) {
            throw const FormatException('Invalid support object');
          }
          final at = publicOnly ? 82 : 2;
          if (bytes.length < at + 4) {
            throw const FormatException('Invalid support object');
          }
          if (ByteData.sublistView(bytes).getUint32(at, Endian.little) !=
              request) {
            return;
          }
          if (!publicOnly && bytes[1] != kind) {
            throw const FormatException('Invalid support kind');
          }
          response.complete(bytes);
        } catch (_) {
          if (!response.isCompleted) {
            response.completeError(
              const FormatException('Malformed support response'),
            );
          }
        }
      },
      onError: (Object error) {
        if (!response.isCompleted) response.completeError(error);
      },
      onDone: () {
        if (!response.isCompleted) {
          response.completeError(StateError('Support session ended'));
        }
      },
    );
    // Consume a response error even while an uncancelable native send is held.
    final result = scope.wait(response.future);
    result.ignore();
    try {
      final body = Uint8List(6);
      body[0] = 1;
      body[1] = kind;
      ByteData.sublistView(body).setUint32(2, request, Endian.little);
      await scope.native(
        () => session.sendObjectBatch([
          (opcode: opcode, body: body),
        ], priority: ConnectionSessionObjectPriority.bulk),
      );
      final bytes = await result;
      scope.check();
      if (!identical(session, _activeConnectionSession) ||
          (!publicOnly && !productReady)) {
        throw StateError('Support session changed');
      }
      return bytes;
    } finally {
      await subscription.cancel();
    }
  }

  Future<SupportBoardCollection> collectSupportBoardDiagnostics({
    required String deviceId,
    int? connectionSessionToken,
    Duration timeout = const Duration(seconds: 10),
    Future<void>? cancelled,
  }) async {
    final generation = connectionSessionToken ?? _connectionGeneration;
    bool current() =>
        !_disposed &&
        isConnected &&
        connectedId == deviceId &&
        generation == _connectionGeneration;
    if (!current()) {
      return SupportBoardCollection.unavailable(
        outcome: SupportQueryOutcome.sessionChanged,
      );
    }
    if (_supportCollection != null) {
      return SupportBoardCollection.unavailable(
        outcome: SupportQueryOutcome.busy,
      );
    }
    final scope = SupportCollectionScope(
      isCurrent: current,
      timeout: timeout,
      cancelled: cancelled,
    );
    _supportCollection = scope;
    final connection = connectionStream.listen((_) {
      scope.reason;
    });
    Future<SupportQueryResult<T>> read<T>(Future<T> Function() query) async {
      final reason = scope.reason;
      if (reason != null) {
        return SupportQueryResult(
          outcome: reason == SupportQueryOutcome.timedOut
              ? SupportQueryOutcome.notAttemptedDeadline
              : reason,
        );
      }
      try {
        await scope.wait(scope.nativeDrained);
        final value = await scope.wait(query());
        scope.check();
        return SupportQueryResult(
          outcome: SupportQueryOutcome.available,
          value: value,
        );
      } on SupportCollectionStopped catch (stopped) {
        return SupportQueryResult(outcome: stopped.outcome);
      } catch (_) {
        return const SupportQueryResult(
          outcome: SupportQueryOutcome.unavailable,
        );
      }
    }

    try {
      final publicHealth = await read(() async {
        final bytes = await _readSupportSnapshot(deviceId, true, 0, scope);
        return SupportPublicHealth.decode(
          bytes,
          ByteData.sublistView(bytes).getUint32(82, Endian.little),
        );
      });
      final recoveryOnly =
          deviceCapabilities(deviceId).recoveryOnly ||
          publicHealth.value?.recoveryMode == 1;
      final privateUnavailable = recoveryOnly
          ? SupportQueryOutcome.unavailable
          : SupportQueryOutcome.ownerNotReady;
      final privateAllowed = productReady && !recoveryOnly;
      final actionHistory = privateAllowed
          ? await read(() async {
              final start = supportMonotonicMs;
              final bytes = await _readSupportSnapshot(
                deviceId,
                false,
                0,
                scope,
              );
              return SupportBoardHistory.decode(
                bytes,
                ByteData.sublistView(bytes).getUint32(2, Endian.little),
                appRequestedMs: start,
                appReceivedMs: supportMonotonicMs,
                bootNonce: _activeConnectionSession?.boardBootNonce,
              );
            })
          : SupportQueryResult<SupportBoardHistory>(
              outcome: privateUnavailable,
            );
      final incident = privateAllowed
          ? await read(() async {
              final bytes = await _readSupportSnapshot(
                deviceId,
                false,
                1,
                scope,
              );
              return decodeSupportIncident(
                bytes,
                ByteData.sublistView(bytes).getUint32(2, Endian.little),
              );
            })
          : SupportQueryResult<SupportIncident>(outcome: privateUnavailable);
      if (!current()) {
        return SupportBoardCollection.unavailable(
          outcome: SupportQueryOutcome.sessionChanged,
        );
      }
      final stillPrivate = privateAllowed && productReady;
      final h = publicHealth.value;
      final capabilities = deviceCapabilities(deviceId);
      final safeHealth =
          h?.runtimeHealth ??
          (_connectionRuntimeHealth == null
              ? null
              : RuntimeHealthAvailable(_connectionRuntimeHealth!.snapshot));
      return SupportBoardCollection(
        publicHealth: publicHealth,
        actionHistory: stillPrivate
            ? actionHistory
            : SupportQueryResult(outcome: privateUnavailable),
        incident: stillPrivate
            ? incident
            : SupportQueryResult(outcome: privateUnavailable),
        firmwareDigest: SupportQueryResult(
          outcome: h?.firmwareDigest == null
              ? SupportQueryOutcome.unavailable
              : SupportQueryOutcome.available,
          value: h?.firmwareDigest,
        ),
        capabilities: SupportQueryResult(
          outcome: capabilities.explicitlyReported
              ? SupportQueryOutcome.available
              : SupportQueryOutcome.unavailable,
          value: capabilities.explicitlyReported ? capabilities : null,
        ),
        runtimeHealth: SupportQueryResult(
          outcome: safeHealth == null
              ? SupportQueryOutcome.unavailable
              : SupportQueryOutcome.available,
          value: safeHealth,
        ),
        firmwareVersion: SupportQueryResult(
          outcome: h == null || h.releaseLabel.isEmpty
              ? SupportQueryOutcome.unavailable
              : SupportQueryOutcome.available,
          value: h?.releaseLabel,
        ),
        boardHealth: stillPrivate ? _latestBoardHealth : null,
        sessionCurrent: true,
        deadlineReached: scope.reason == SupportQueryOutcome.timedOut,
      );
    } finally {
      scope.finish();
      await connection.cancel();
      unawaited(
        scope.nativeDrained.then((_) {
          if (identical(_supportCollection, scope)) _supportCollection = null;
        }),
      );
    }
  }

  Stream<BoardHealthStatus> get boardHealthStream =>
      _boardHealthController.stream;
  final StreamController<SupportIncident> _incidentController =
      StreamController<SupportIncident>.broadcast();

  Future<SupportIncident?> fetchSupportIncident({
    required String deviceId,
  }) async {
    if (!productReady) return null;
    return (await collectSupportBoardDiagnostics(
      deviceId: deviceId,
    )).incident.value;
  }

  final StreamController<HomeStateNotification> _homeStateController =
      StreamController<HomeStateNotification>.broadcast();
  final StreamController<bool> _consoleTraceOutputController =
      StreamController<bool>.broadcast();
  final StreamController<ConnectionStateUpdate> _connectionController =
      StreamController<ConnectionStateUpdate>.broadcast();
  final StreamController<String> _otaStatusController =
      StreamController<String>.broadcast();
  final StreamController<String> _settingsChangedController =
      StreamController<String>.broadcast();
  final StreamController<LitchControlEvent> _controlActionController =
      StreamController<LitchControlEvent>.broadcast(sync: true);

  final Map<String, LitchBoard> _boardsById = <String, LitchBoard>{};
  final Set<String> _compatibleDeviceIds = <String>{};
  String? _consoleHistoryDeviceId;
  final List<String> _consoleLines = <String>[];
  HomeStateNotification? _latestHomeState;
  String? _kingQueenSwapPromptBoardId;
  bool _kingQueenSwapPromptActive = false;
  final StringBuffer _consoleCurrentLine = StringBuffer();
  final LitchRequestIdAllocator _requestIds = LitchRequestIdAllocator();
  final Map<int, _PendingSettingTransaction> _pendingSettingsById =
      <int, _PendingSettingTransaction>{};
  final Map<String, int> _pendingSettingIdsByKey = <String, int>{};
  int _controlNativeWrites = 0;
  int _controlTimeouts = 0;
  int _controlIntents = 0;
  int _controlCoalescedTaps = 0;
  int _controlRecoveryReadRequests = 0;
  int _controlBusyRetries = 0;
  int _controlRecoveryConfirmed = 0;
  int _controlRecoveryUnconfirmed = 0;
  final Set<String> _knownControlProtocolDevices = <String>{};
  final Set<String> _unavailableControlProtocolDevices = <String>{};
  int? _bulkSettingsRevision;
  Timer? _bulkRefreshTimer;
  Completer<void>? _pendingBoardStateReceipt;
  bool _bulkRefreshPending = false;
  final Map<String, Completer<bool>> _bulkWriteReceiptsPending = {};
  _BulkStateQuery? _bulkStateQuery;
  Timer? _bulkReceiptTimer;
  int _nextBulkReceipt = 1;
  Timer? _bulkConfirmationDeadline;
  bool get hasBoardStateSnapshot => _bulkSettingsRevision != null;
  int? get boardStateSettingsRevision => _bulkSettingsRevision;

  final Map<String, Object> _latestSettingValues = <String, Object>{};
  final Map<String, int> _settingNotificationVersions = <String, int>{};
  int _settingNotificationVersion = 0;
  final Map<int, String> _settingsKeysByIndex = <int, String>{};
  List<LitchSetting>? _settingsDescriptors;
  LitchDeviceCapabilities? _settingsDescriptorCapabilities;
  final Map<int, Map<String, Object?>> _pendingSettingsNotifyPayloadsByIndex =
      <int, Map<String, Object?>>{};
  Timer? _settingsRwRetryTimer;
  Timer? _keepAliveTimer;
  bool _pendingCarriageReturn = false;
  bool _consoleCurrentLineOverflowed = false;
  bool _consoleTraceOutputEnabled = false;
  int _otaDataChunkSize = 96;
  int _litchcraftPacketPayloadBytes = 20;
  int _otaControlSafeBytes = 20;
  Future<void>? _linkCalibrationFuture;
  bool _connectAttemptInProgress = false;
  bool _disposed = false;
  Future<void>? _disposeFuture;
  Future<void>? _disconnectFuture;
  int _connectionGeneration = 0;
  int? _identifyConnectionGeneration;
  bool get identifyConnectionActive =>
      _identifyConnectionGeneration == _connectionGeneration;
  final Set<VoidCallback> _connectionWaiters = {};
  ConnectionSession? _activeConnectionSession;
  Future<ConnectionSession?>? _openingConnectionSession;
  Future<void> _connectionConsoleTail = Future<void>.value();
  Future<void> _withoutResponseWriteTail = Future<void>.value();
  StreamSubscription<ConnectionSessionPacket>?
  _connectionSessionStateSubscription;
  int? _latestRuntimeHealthStateRevision;
  ConnectionRuntimeHealthState? _connectionRuntimeHealth;
  ConnectionBoardHealthContext? _connectionBoardHealthContext;
  final HomeStateWireAssembler _connectionHomeState = HomeStateWireAssembler();
  final SettingsStateWireAssembler _connectionSettingsState =
      SettingsStateWireAssembler();
  Future<bool>? _connectionStateRecovery;
  bool _binarySettingsStateObserved = false;
  SettingsStateSnapshot? _completeConnectionSettings;
  SettingsStateSnapshot? _connectionPlayerPalette;

  int _retireConnection() {
    _nativeBondFailureGeneration = null;
    _invalidateConnectionSession();
    ++_connectionGeneration;
    // A disconnected peripheral owns no native admission slot. New-link work
    // must not wait behind a retired CoreBluetooth/Android operation; stale
    // queued closures still fail their captured generation check.
    _withoutResponseWriteTail = Future<void>.value();
    final waiting = _connectionWaiters.toList();
    _connectionWaiters.clear();
    for (final cancel in waiting) {
      cancel();
    }
    _persistConsoleSnapshot();
    return _connectionGeneration;
  }

  void _invalidateConnectionSession() {
    final pairing = _ownerPairingSession;
    _ownerPairingSession = null;
    _productPreparation = null;
    _productPreparationFailed = false;
    _productReadinessPending = false;
    unawaited(_ownerPairingSubscription?.cancel());
    _ownerPairingSubscription = null;
    unawaited(pairing?.dispose());
    if (!_productReadinessController.isClosed) _publishProductReadiness(false);
    if (!_ownerPairingStatusController.isClosed) {
      _ownerPairingStatusController.add(ownerPairingStatus);
    }
    final session = _activeConnectionSession;
    _activeConnectionSession = null;
    _openingConnectionSession = null;
    _latestRuntimeHealthStateRevision = null;
    _connectionRuntimeHealth = null;
    _connectionBoardHealthContext = null;
    _latestCurrentGameLine = null;
    _boardStorageRecovery = null;
    _boardStorageRecoveryRevision = null;
    _boardStorageReformatFuture = null;
    _boardStorageReformatGeneration = null;
    _boardStorageReformatDispatchedGeneration = null;
    _boardStorageReformatCompletionStatus = null;
    _boardStorageReformatSubmissionRevision = null;
    _boardStorageReformatTerminalRefusal = false;
    _connectionStateRecovery = null;
    _connectionHomeState.clear();
    _connectionSettingsState.clear();
    _completeConnectionSettings = null;
    _connectionPlayerPalette = null;
    _boardSwitchStates.clear();
    _latestBoardPhysicalInputState = null;
    _binarySettingsStateObserved = false;
    unawaited(_connectionSessionStateSubscription?.cancel());
    _connectionSessionStateSubscription = null;
    unawaited(_otaSharedStatusSub?.cancel());
    _otaSharedStatusSub = null;
    unawaited(session?.dispose());
  }

  void _captureConnectionSessionState(ConnectionSessionPacket packet) {
    final id = connectedId;
    if (id != null &&
        deviceCapabilities(id).supportsOwnerPairing &&
        !productReady) {
      return;
    }
    if (packet.code == SettingsStateWireCodec.playerPaletteStateCode) {
      _captureConnectionPlayerPalette(packet);
      return;
    }
    if (packet.code == CurrentGameLineState.stateCode) {
      final state = CurrentGameLineState.tryParse(
        revision: packet.sequence,
        body: packet.body,
      );
      final previous = _latestCurrentGameLine;
      if (state == null ||
          (previous != null &&
              !isNewerHomeStateSequence(state.revision, previous.revision))) {
        return;
      }
      _latestCurrentGameLine = state;
      if (!_currentGameLineController.isClosed) {
        _currentGameLineController.add(state);
      }
      return;
    }
    if (packet.code == BoardStorageRecoverySnapshot.stateCode) {
      final snapshot = BoardStorageRecoverySnapshot.tryParse(packet.body);
      final previous = _boardStorageRecoveryRevision;
      if (snapshot == null ||
          (previous != null &&
              !isNewerHomeStateSequence(packet.sequence, previous))) {
        return;
      }
      _boardStorageRecoveryRevision = packet.sequence;
      _boardStorageRecovery = snapshot;
      if (!_boardStorageRecoveryController.isClosed) {
        _boardStorageRecoveryController.add(snapshot);
      }
      return;
    }
    if (packet.code == BoardPhysicalInputState.stateCode &&
        packet.body.length == BoardPhysicalInputState.bodyBytes) {
      final state = BoardPhysicalInputState.tryParse(
        code: packet.code,
        revision: packet.sequence,
        body: packet.body,
      );
      if (state == null) return;
      final previous = _latestBoardPhysicalInputState;
      if (previous != null &&
          !isNewerHomeStateSequence(state.revision, previous.revision)) {
        return;
      }
      _latestBoardPhysicalInputState = state;
      if (!_boardPhysicalInputStateController.isClosed) {
        _boardPhysicalInputStateController.add(state);
      }
      for (final switchId in BoardPhysicalInputState.sampledSwitches) {
        final down = state.isDown(switchId);
        final priorSwitch = _boardSwitchStates[switchId];
        if (priorSwitch?.isDown == down) continue;
        final switchState = BoardSwitchState(
          switchId: switchId,
          isDown: down,
          revision: state.revision,
          observedAtMs: 0,
        );
        _boardSwitchStates[switchId] = switchState;
        if (!_boardSwitchStateController.isClosed) {
          _boardSwitchStateController.add(switchState);
        }
      }
      return;
    }
    if (packet.code >= BoardSwitchState.firstStateCode &&
        packet.code <= BoardSwitchState.lastStateCode) {
      final state = BoardSwitchState.tryParse(
        code: packet.code,
        revision: packet.sequence,
        body: packet.body,
      );
      if (state == null) return;
      final previous = _boardSwitchStates[state.switchId];
      if (previous != null &&
          !isNewerHomeStateSequence(state.revision, previous.revision)) {
        return;
      }
      _boardSwitchStates[state.switchId] = state;
      if (!_boardSwitchStateController.isClosed) {
        _boardSwitchStateController.add(state);
      }
      return;
    }
    if (SettingsStateWireCodec.acceptsCode(packet.code) ||
        packet.code == SettingsStateWireCodec.assistanceStateCode ||
        packet.code == SettingsStateWireCodec.bootDisplayStateCode) {
      _captureConnectionSettingsState(packet);
      return;
    }
    if (packet.code >= HomeStateWireCodec.firstStateCode &&
        packet.code <= HomeStateWireCodec.lastStateCode) {
      _captureConnectionHomeState(packet);
      return;
    }
    if (packet.code == ConnectionSession.runtimeHealthStateCode) {
      final previous = _latestRuntimeHealthStateRevision;
      if (previous != null &&
          !isNewerHomeStateSequence(packet.sequence, previous)) {
        return;
      }
      final health = ConnectionRuntimeHealthState.tryParse(packet.body);
      if (health == null) return;
      _latestRuntimeHealthStateRevision = packet.sequence;
      _connectionRuntimeHealth = health;
    } else if (packet.code == ConnectionSession.boardHealthContextStateCode) {
      final context = ConnectionBoardHealthContext.tryParse(packet.body);
      if (context == null) return;
      _connectionBoardHealthContext = context;
    } else {
      return;
    }
    final runtime = _connectionRuntimeHealth;
    final context = _connectionBoardHealthContext;
    if (runtime == null || context == null) return;
    final health = BoardHealthStatus.fromConnectionState(runtime, context);
    _latestBoardHealth = health;
    if (!_boardHealthController.isClosed) {
      _boardHealthController.add(health);
    }
  }

  @visibleForTesting
  void debugCaptureConnectionSessionState(ConnectionSessionPacket packet) {
    _captureConnectionSessionState(packet);
  }

  void _captureConnectionSettingsState(ConnectionSessionPacket packet) {
    final state = _connectionSettingsState.accept(
      code: packet.code,
      revision: packet.sequence,
      body: packet.body,
    );
    if (state == null) {
      if (_connectionSettingsState.isIncomplete) {
        _recoverIncompleteConnectionState();
      }
      return;
    }
    final previousRevision = _bulkSettingsRevision;
    if (previousRevision != null &&
        state.settingsRevision != previousRevision &&
        !isNewerHomeStateSequence(state.settingsRevision, previousRevision)) {
      return;
    }
    // First binary completion changes authority even when transitional values
    // already match. Existing Settings listeners must observe that boundary.
    _completeConnectionSettings = state;
    final values = _overlayPlayerPalette(state.values, state.settingsRevision);
    final changed = <String>{
      for (final key in _latestSettingValues.keys)
        if (!values.containsKey(key)) key,
      for (final entry in values.entries)
        if (!_binarySettingsStateObserved ||
            _latestSettingValues[entry.key] != entry.value)
          entry.key,
    };
    _bulkSettingsRevision = state.settingsRevision;
    _binarySettingsStateObserved = true;
    _pendingBoardStateReceipt?.complete();
    _pendingBoardStateReceipt = null;
    _latestSettingValues
      ..clear()
      ..addAll(values);
    _settingsKeysByIndex
      ..clear()
      ..addEntries(
        SettingsStateWireCodec.visibleKeys.indexed.map(
          (entry) => MapEntry(entry.$1, entry.$2),
        ),
      );
    _pendingSettingsNotifyPayloadsByIndex.clear();
    _bulkRefreshTimer?.cancel();
    _bulkRefreshTimer = null;
    _bulkRefreshPending = false;
    for (final key in changed) {
      _emitSettingsChanged(key);
    }
  }

  Map<String, Object> _overlayPlayerPalette(
    Map<String, Object> values,
    int settingsRevision,
  ) {
    final palette = _connectionPlayerPalette;
    return palette != null &&
            isNewerHomeStateSequence(palette.settingsRevision, settingsRevision)
        ? <String, Object>{...values, ...palette.values}
        : values;
  }

  void _captureConnectionPlayerPalette(ConnectionSessionPacket packet) {
    final palette = SettingsStateWireCodec.tryDecodePlayerPalette(packet.body);
    if (palette == null) return;
    final previous = _connectionPlayerPalette;
    if (previous != null &&
        !isNewerHomeStateSequence(
          palette.settingsRevision,
          previous.settingsRevision,
        )) {
      return;
    }
    final fullRevision = _bulkSettingsRevision;
    if (fullRevision != null &&
        palette.settingsRevision != fullRevision &&
        !isNewerHomeStateSequence(palette.settingsRevision, fullRevision)) {
      return;
    }
    _connectionPlayerPalette = palette;
    final changed = <String>[
      for (final entry in palette.values.entries)
        if (_latestSettingValues[entry.key] != entry.value) entry.key,
    ];
    // Live palette projection never publishes a complete Settings receipt.
    _latestSettingValues.addAll(palette.values);
    for (final key in changed) {
      _emitSettingsChanged(key);
    }
  }

  void _captureConnectionHomeState(ConnectionSessionPacket packet) {
    final previous = _latestHomeState?.sequence;
    final nextSequence =
        previous == null || previous == HomeStateNotification.maxSequence
        ? 1
        : previous + 1;
    final home = _connectionHomeState.accept(
      code: packet.code,
      revision: packet.sequence,
      body: packet.body,
      sequence: nextSequence,
    );
    if (home != null) {
      _latestHomeState = home;
      if (!_homeStateController.isClosed) {
        _homeStateController.add(home);
      }
      return;
    }
    if (_connectionHomeState.isIncomplete) {
      _recoverIncompleteConnectionState();
    }
  }

  void _recoverIncompleteConnectionState() {
    if (deviceCapabilities(connectedId ?? '').recoveryOnly) return;
    final session = _activeConnectionSession;
    if (session == null ||
        !session.isActivated ||
        _connectionStateRecovery != null) {
      return;
    }
    bool coherent() =>
        !_connectionHomeState.isIncomplete &&
        !_connectionSettingsState.isIncomplete;
    late final Future<bool> recovery;
    recovery = Future<bool>.microtask(() async {
      try {
        if (!identical(session, _activeConnectionSession)) return false;
        if (coherent()) return true;
        return await session.readStateSnapshot<bool>(
          mode: ConnectionSessionStateReadMode.observeFirst,
          accept: (_) => coherent() ? true : null,
        );
      } catch (error) {
        if (identical(session, _activeConnectionSession)) {
          _pushAppLog('Board state repair could not be confirmed: $error');
        }
        return false;
      } finally {
        if (identical(_connectionStateRecovery, recovery)) {
          _connectionStateRecovery = null;
        }
      }
    });
    _connectionStateRecovery = recovery;
    unawaited(recovery);
  }

  // Native Android work may never complete after link loss. Retire the
  // application's wait immediately; late SDK results cannot revive the epoch.
  Future<T> _untilConnectionEnds<T>(Future<T> operation) {
    final result = Completer<T>();
    void cancel() {
      if (!result.isCompleted) {
        result.completeError(StateError('BLE connection generation changed.'));
      }
    }

    _connectionWaiters.add(cancel);
    operation.then(
      (value) {
        _connectionWaiters.remove(cancel);
        if (!result.isCompleted) result.complete(value);
      },
      onError: (Object error, StackTrace stack) {
        _connectionWaiters.remove(cancel);
        if (!result.isCompleted) result.completeError(error, stack);
      },
    );
    return result.future;
  }

  final DateTime Function() _scanNow;
  bool _isScanning = false;
  bool _scanRequested = false;
  DateTime? _scanStartedAt;
  bool _bleSetupNeedsReset = false;
  int _bleSetupGeneration = 0;
  Future<bool>? _bleSetupResetFuture;
  final List<DateTime> _androidScanStarts = <DateTime>[];
  BleStatus? _lastBleStatus;
  BleAvailability _bleAvailability = BleAvailability.checking;
  int _scanGeneration = 0;
  DateTime? _scanRetryNotBefore;
  DateTime? _lastScanCooldownLogAt;
  int _nonCandidateScanHitsLogged = 0;
  DateTime? _lastPermissionLogAt;
  String _lastPermissionLogToken = '';
  final Map<String, DateTime> _settingsDiagLastLogAt = <String, DateTime>{};
  final Map<String, DateTime> _optionalCommandLastLogAt = <String, DateTime>{};
  final Map<String, LitchSettingsReadAuthority> _settingsAuthorityByDevice =
      <String, LitchSettingsReadAuthority>{};
  final Map<String, LitchDeviceCapabilities> _capabilitiesByDevice =
      <String, LitchDeviceCapabilities>{};
  final Map<String, Future<LitchDeviceCapabilities>>
  _capabilityDiscoveryByDevice = <String, Future<LitchDeviceCapabilities>>{};
  final OtaPendingVerificationOwner _otaVerificationOwner;
  bool _otaUploadActive = false;
  int _otaProofRequestId = 0;
  final SettingReadDiagnostics _settingReadDiagnostics =
      SettingReadDiagnostics();
  int _settingsWriteAttempts = 0;
  static const int _maximumSupportCounter = 0xffffffff;
  int _notificationRepairOperations = 0;
  int _notificationRepairAttempts = 0;
  int _notificationRepairRecovered = 0;
  int _notificationRepairExhausted = 0;
  int _notificationRepairCancelled = 0;
  bool _notificationRepairSaturated = false;

  OtaVerificationTarget? get pendingOtaVerification =>
      _otaVerificationOwner.pending;
  bool get pendingOtaRebootExpected =>
      _otaVerificationOwner.finalizationAcknowledged;
  ({OtaVerificationTarget target, OtaPostRebootDifferentImage result})?
  get failedOtaVerification => _otaVerificationOwner.failed;

  bool get otaVerificationStorageUnavailable =>
      _otaVerificationOwner.storageError != null;

  Future<void> restorePendingOtaVerification() =>
      _otaVerificationOwner.restore();

  Future<void> rememberPendingOtaVerification(OtaVerificationTarget target) =>
      _otaVerificationOwner.remember(target);

  Future<void> resolveFailedOtaVerification(
    OtaVerificationTarget target,
    OtaPostRebootDifferentImage result,
  ) async {
    await _otaVerificationOwner.resolveFailure(target, result);
    _pushAppLog(
      'OTA verification failed on ${target.deviceId}: expected '
      '${target.version}/${target.imageSha256}, running accepted '
      '${result.version}/${result.imageSha256}.',
    );
  }

  Future<bool> clearPendingOtaVerification(OtaVerificationTarget target) =>
      _otaVerificationOwner.clear(target);

  int _settingsWriteSucceeded = 0;
  int _settingsWriteFailed = 0;
  String? _lastSettingsOperation;
  String? _lastSettingsFailureToken;
  bool _settingsIndexRefreshInFlight = false;
  Future<_SettingsEnumerationResult>? _settingsEnumerationInFlight;
  String? _settingsEnumerationDeviceId;
  bool _isAppForeground = true;
  bool _keepAliveWriteInFlight = false;
  int _consoleBridgeGeneration = 0;
  int _settingsSessionGeneration = 0;
  int _settingsBridgeGeneration = 0;
  LitchOtaControlChannel? _otaControlChannel;
  int _otaControlGeneration = 0;
  Future<LitchGattSession?>? _gattSessionFuture;
  LitchGattSession? _resolvedGattSession;
  final Map<String, LitchDeviceCapabilities> _authoritativeCapabilities = {};
  final Set<String> _gattRefreshAttempted = {};
  int? _incompleteGattGeneration;
  Future<void>? _firstGattWriteTurn;
  int _gattSessionGeneration = 0;

  ConnectionStateUpdate? _connectionState;

  bool get _usesExactGattSession => _gattSessionFactory != null;

  void _openGattSession(String deviceId) {
    _resolvedGattSession = null;
    _firstGattWriteTurn = null;
    final LitchGattSessionFactory? factory = _gattSessionFactory;
    if (!_usesExactGattSession || factory == null) {
      _gattSessionFuture = null;
      return;
    }

    final int generation = ++_gattSessionGeneration;
    _gattSessionFuture = () async {
      try {
        final LitchGattSession session =
            factory is ReactiveBleGattSessionFactory
            ? await factory.openWithCapabilities(
                deviceId,
                LitchDeviceCapabilities.unavailable,
              )
            : await factory.open(deviceId);
        if (generation != _gattSessionGeneration ||
            !isConnected ||
            connectedId != deviceId) {
          return null;
        }
        _resolvedGattSession = session;
        _pushAppLog('BLE services ready for this connection.');
        return session;
      } catch (error) {
        if (generation == _gattSessionGeneration &&
            isConnected &&
            connectedId == deviceId) {
          _pushAppLog('BLE service discovery failed: $error');
          if (error is LitchGattDiscoveryIncomplete) {
            // Android caches successful-but-incomplete discovery for the link.
            // Retire its handles and let the existing remembered-board reconnect
            // policy acquire a fresh native connection, without fallback writes.
            await _retireIncompleteGatt(deviceId);
          }
        }
        return null;
      }
    }();
  }

  void _invalidateGattSession() {
    _resolvedGattSession = null;
    ++_gattSessionGeneration;
    _gattSessionFuture = null;
    _firstGattWriteTurn = null;
  }

  Future<LitchGattSession?> _gattSessionFor(String deviceId) async {
    if (!_usesExactGattSession || !isConnected || connectedId != deviceId) {
      return null;
    }
    if (_repairingGattInventory) {
      throw const LitchGattDiscoveryIncomplete(
        'native GATT inventory repair is pending',
      );
    }

    final int generation = _gattSessionGeneration;
    final Future<LitchGattSession?>? opening = _gattSessionFuture;
    if (opening == null) {
      throw StateError('BLE services are not opening for this connection.');
    }
    final LitchGattSession? session = await _untilConnectionEnds(opening);
    if (session == null ||
        !isConnected ||
        connectedId != deviceId ||
        generation != _gattSessionGeneration) {
      throw StateError('BLE services are unavailable for this connection.');
    }
    return session;
  }

  void _requireProductCharacteristic(QualifiedCharacteristic characteristic) {
    if (deviceCapabilities(characteristic.deviceId).supportsOwnerPairing &&
        !productReady &&
        characteristic.characteristicId !=
            BleProtocol.connectionSessionIngressCharacteristic &&
        characteristic.characteristicId !=
            BleProtocol.connectionSessionEgressCharacteristic &&
        characteristic.characteristicId !=
            BleProtocol.otaControlCharacteristic) {
      throw const OwnerPairingNotReady();
    }
  }

  Future<List<int>> _readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    _requireProductCharacteristic(characteristic);
    final LitchGattSession? session = await _gattSessionFor(
      characteristic.deviceId,
    );
    if (session == null) {
      return _untilConnectionEnds(_ble.readCharacteristic(characteristic));
    }
    _requireGattEndpoint(session, characteristic);
    return _untilConnectionEnds(
      session.read(
        serviceId: characteristic.serviceId,
        characteristicId: characteristic.characteristicId,
      ),
    );
  }

  bool get _repairingGattInventory =>
      _incompleteGattGeneration == _connectionGeneration;

  Future<void> _retireIncompleteGatt(String deviceId) async {
    final generation = _connectionGeneration;
    if (_repairingGattInventory || !isConnected || connectedId != deviceId) {
      return;
    }
    _incompleteGattGeneration = generation;
    _publishProductReadiness(false, force: true);
    if (_gattRefreshAttempted.add(deviceId)) {
      if (_gattRefreshAttempted.length > 32) {
        _gattRefreshAttempted.remove(_gattRefreshAttempted.first);
      }
      try {
        // Android refresh operates on this live native handle. Closing it in
        // parallel can make every reconnect inherit the same cached inventory.
        await Future<void>.sync(
          () => _ble.clearGattCache(deviceId),
        ).timeout(_gattCacheRefreshTimeout);
      } catch (error) {
        if (generation == _connectionGeneration) {
          _pushAppLog('BLE cache refresh unavailable: $error');
        }
      }
    } else if (_authoritativeCapabilities.remove(deviceId) != null) {
      // A verified newer profile can legitimately disappear after firmware
      // rollback. One native cache refresh has already ruled out the common
      // stale-handle case, so retire that prior connection's authority and
      // let the next connection discover the board's current profile.
      // Keeping it forever turns a safe board rollback into an unbounded
      // connect/discover/disconnect loop that eventually trips Android's scan
      // throttle.
      _capabilitiesByDevice.remove(deviceId);
      _capabilityDiscoveryByDevice.remove(deviceId);
      _pushAppLog(
        'Board GATT profile changed after cache refresh; retrying with fresh capability discovery.',
      );
    }
    if (generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId) {
      // Start the existing native cleanup barrier, then let discovery unwind:
      // cancelling its subscriptions may itself wait for this GATT future.
      unawaited(disconnect());
    }
  }

  void _requireGattEndpoint(
    LitchGattSession? session,
    QualifiedCharacteristic characteristic,
  ) {
    if (session is LitchGattInventory &&
        !(session as LitchGattInventory).hasCharacteristic(
          characteristic.serviceId,
          characteristic.characteristicId,
        )) {
      final error = LitchGattDiscoveryIncomplete(
        '${characteristic.serviceId}/${characteristic.characteristicId}',
      );
      unawaited(_retireIncompleteGatt(characteristic.deviceId));
      throw error;
    }
  }

  Future<void> _writeCharacteristic(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
    required bool withResponse,
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
    VoidCallback? onWrite,
  }) async {
    final int connectionGeneration = _connectionGeneration;
    final int gattGeneration = _gattSessionGeneration;
    void check() {
      if (_repairingGattInventory) {
        throw const LitchGattDiscoveryIncomplete(
          'native GATT inventory repair is pending',
        );
      }
      _requireProductCharacteristic(characteristic);
      supportScope?.check();
      if (connectionGeneration != _connectionGeneration ||
          gattGeneration != _gattSessionGeneration ||
          (isCurrent != null && !isCurrent())) {
        throw StateError('BLE connection generation changed.');
      }
    }

    check();
    final LitchGattSession? session = await (supportScope == null
        ? _gattSessionFor(characteristic.deviceId)
        : supportScope.native(() => _gattSessionFor(characteristic.deviceId)));
    check();
    _requireGattEndpoint(session, characteristic);
    if (session != null) {
      // Exact SDK subscriptions defer native registration through asyncExpand.
      // One shared event turn lets those requests enqueue before bootstrap
      // writes; it is not a CCCD acknowledgement or a wait for board state.
      await (_firstGattWriteTurn ??= Future<void>.delayed(Duration.zero));
      check();
    }
    Future<void> write() async {
      onWrite?.call();
      if (session == null) {
        if (withResponse) {
          await _ble.writeCharacteristicWithResponse(
            characteristic,
            value: value,
          );
        } else {
          await _ble.writeCharacteristicWithoutResponse(
            characteristic,
            value: value,
          );
        }
      } else {
        await session.write(
          serviceId: characteristic.serviceId,
          characteristicId: characteristic.characteristicId,
          value: value,
          withResponse: withResponse,
        );
      }
    }

    final nativeWrite = withResponse
        ? write()
        : _serializeWriteWithoutResponse(() async {
            check();
            await write();
          });
    if (supportScope == null) {
      await _untilConnectionEnds(nativeWrite);
    } else {
      await supportScope.native(() => _untilConnectionEnds(nativeWrite));
    }
    check();
  }

  Future<void> _serializeWriteWithoutResponse(Future<void> Function() write) {
    final result = _withoutResponseWriteTail.then((_) => write());
    // Native admission is one slot per peripheral. Keep the lane usable after
    // an individual write fails; the caller still observes the original error.
    _withoutResponseWriteTail = result.catchError((Object _, StackTrace _) {});
    return result;
  }

  Future<void> _writeCharacteristicBatchWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<List<int>> values,
    required bool Function() isCurrent,
  }) async {
    final connectionGeneration = _connectionGeneration;
    final gattGeneration = _gattSessionGeneration;
    void check() {
      if (_repairingGattInventory) {
        throw const LitchGattDiscoveryIncomplete(
          'native GATT inventory repair is pending',
        );
      }
      if (connectionGeneration != _connectionGeneration ||
          gattGeneration != _gattSessionGeneration ||
          !isCurrent()) {
        throw StateError('BLE connection generation changed.');
      }
    }

    check();
    final session = await _gattSessionFor(characteristic.deviceId);
    check();
    _requireGattEndpoint(session, characteristic);
    if (session != null) {
      await (_firstGattWriteTurn ??= Future<void>.delayed(Duration.zero));
      check();
    }

    if (session case final LitchGattBatchSession batchSession) {
      await _untilConnectionEnds(
        (() async {
          check();
          await batchSession.writeWithoutResponseBatch(
            serviceId: characteristic.serviceId,
            characteristicId: characteristic.characteristicId,
            values: values,
          );
        })(),
      );
      check();
      return;
    }

    // The compatibility GATT surface has no native batch primitive. Preserve
    // packet order and the single peripheral admission slot explicitly.
    for (final value in values) {
      check();
      await _serializeWriteWithoutResponse(() {
        check();
        return session == null
            ? _ble.writeCharacteristicWithoutResponse(
                characteristic,
                value: value,
              )
            : session.write(
                serviceId: characteristic.serviceId,
                characteristicId: characteristic.characteristicId,
                value: value,
                withResponse: false,
              );
      });
    }
    check();
  }

  Future<void> _writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
    VoidCallback? onWrite,
  }) {
    return _writeCharacteristic(
      characteristic,
      value: value,
      withResponse: true,
      supportScope: supportScope,
      isCurrent: isCurrent,
      onWrite: onWrite,
    );
  }

  Future<void> _writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
  }) {
    return _writeCharacteristic(
      characteristic,
      value: value,
      withResponse: false,
      supportScope: supportScope,
      isCurrent: isCurrent,
    );
  }

  Stream<List<int>> _subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    _requireProductCharacteristic(characteristic);
    if (!_usesExactGattSession ||
        !isConnected ||
        connectedId != characteristic.deviceId) {
      return _ble.subscribeToCharacteristic(characteristic);
    }
    return _subscribeToResolvedCharacteristic(characteristic);
  }

  Stream<List<int>> _subscribeToResolvedCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async* {
    final LitchGattSession? session = await _gattSessionFor(
      characteristic.deviceId,
    );
    if (session == null) {
      throw StateError('BLE services are unavailable for this connection.');
    }
    _requireGattEndpoint(session, characteristic);
    yield* session.subscribe(
      serviceId: characteristic.serviceId,
      characteristicId: characteristic.characteristicId,
    );
  }

  Stream<List<LitchBoard>> get boardsStream => _boardsController.stream;

  LitchBoard? discoveredBoard(String deviceId) => _boardsById[deviceId];

  Stream<BleAvailability> get availabilityStream =>
      _availabilityController.stream;

  BleAvailability get bleAvailability => _bleAvailability;

  /// Non-prompting observations for a user-reviewed support draft. Do not use
  /// readiness/recovery helpers here: they may request access or start scanning.
  Future<SupportPhoneDiagnostics> collectSupportPhoneDiagnostics() async {
    final TargetPlatform platform = defaultTargetPlatform;
    final String? cachedBleStatus = _lastBleStatus?.name;
    final String cachedAvailability = _bleAvailability.name;
    final Map<SupportPhonePermission, Permission>
    permissions = switch (platform) {
      TargetPlatform.iOS => <SupportPhonePermission, Permission>{
        SupportPhonePermission.bluetooth: Permission.bluetooth,
      },
      TargetPlatform.android => <SupportPhonePermission, Permission>{
        SupportPhonePermission.bluetoothScan: Permission.bluetoothScan,
        SupportPhonePermission.bluetoothConnect: Permission.bluetoothConnect,
        SupportPhonePermission.locationWhenInUse: Permission.locationWhenInUse,
      },
      _ => <SupportPhonePermission, Permission>{},
    };
    final List<MapEntry<SupportPhonePermission, SupportPhonePermissionResult>>
    results = await Future.wait(
      permissions.entries.map((entry) async {
        SupportPhonePermissionResult result;
        try {
          final PermissionStatus status = await _permissionGateway
              .status(entry.value)
              .timeout(_supportPermissionTimeout);
          result = switch (status) {
            PermissionStatus.granted => SupportPhonePermissionResult.granted,
            PermissionStatus.denied => SupportPhonePermissionResult.denied,
            PermissionStatus.permanentlyDenied =>
              SupportPhonePermissionResult.permanentlyDenied,
            PermissionStatus.restricted =>
              SupportPhonePermissionResult.restricted,
            PermissionStatus.limited => SupportPhonePermissionResult.limited,
            PermissionStatus.provisional =>
              SupportPhonePermissionResult.provisional,
          };
        } on TimeoutException {
          result = SupportPhonePermissionResult.timedOut;
        } catch (_) {
          result = SupportPhonePermissionResult.unavailable;
        }
        return MapEntry(entry.key, result);
      }),
    );
    return SupportPhoneDiagnostics(
      platform: platform.name,
      permissions: Map.fromEntries(results),
      cachedBleStatus: cachedBleStatus,
      cachedDiscoveryAvailability: cachedAvailability,
    );
  }

  bool get availabilityRecoveryNeedsSettings =>
      _bleAvailability == BleAvailability.bluetoothPermissionBlocked ||
      _bleAvailability == BleAvailability.locationPermissionBlocked;

  Future<void> requestBluetoothAccess() async {
    if (_disposed || !await _ensurePermissions(requestIfNeeded: true)) {
      return;
    }
    await startScan();
  }

  Future<bool> openBluetoothSettings() => _permissionGateway.openSettings();

  Stream<List<String>> get consoleStream => _consoleController.stream;

  /// Observes the existing Console subscription before text decoding. The
  /// returned cancel function retires only this observation, never the bridge.
  VoidCallback observeConsolePackets({
    required String deviceId,
    required int connectionToken,
    required ValueChanged<List<int>> onPacket,
    required ValueChanged<Object> onError,
  }) {
    if (_consolePacketObserver != null ||
        _consoleSub == null ||
        !productReady ||
        !isConnected ||
        connectedId != deviceId ||
        connectionToken != _connectionGeneration) {
      throw StateError('Console byte observation is unavailable.');
    }
    final observer = _ConsolePacketObserver(
      deviceId,
      connectionToken,
      onPacket,
      onError,
    );
    _consolePacketObserver = observer;
    return () {
      if (identical(_consolePacketObserver, observer)) {
        _consolePacketObserver = null;
      }
    };
  }

  void _retireConsolePacketObserver(Object error) {
    final observer = _consolePacketObserver;
    _consolePacketObserver = null;
    if (observer == null) return;
    try {
      observer.onError(error);
    } catch (_) {
      // Optional measurement must not change production Console delivery.
    }
  }

  List<String> get consoleLinesSnapshot =>
      List<String>.unmodifiable(_consoleLines);

  Stream<HomeStateNotification> get homeStateStream =>
      _homeStateController.stream;

  Stream<BoardSwitchState> get boardSwitchStateStream =>
      _boardSwitchStateController.stream;

  Stream<BoardPhysicalInputState> get boardPhysicalInputStateStream =>
      _boardPhysicalInputStateController.stream;

  BoardPhysicalInputState? get latestBoardPhysicalInputState =>
      _latestBoardPhysicalInputState;

  Map<BoardSwitch, BoardSwitchState> get boardSwitchStates =>
      Map<BoardSwitch, BoardSwitchState>.unmodifiable(_boardSwitchStates);

  HomeStateNotification? get latestHomeState => _latestHomeState;

  // A handled Hint toggle invalidates old guidance until its new state arrives.
  // Keep the revision baseline: replaying an old line must not restore it.
  void discardCachedHomeAssistance({
    required String boardId,
    required int sequence,
  }) {
    final HomeStateNotification? state = _latestHomeState;
    if (connectedId == boardId && state?.sequence == sequence) {
      _latestHomeState = state!.withoutAssistance();
    }
  }

  /// Returns true only for the first active king/queen swap snapshot in the
  /// current app session. The service outlives Home across a same-board BLE
  /// reconnect, so a still-active prompt cannot replay merely because the
  /// screen and firmware sequence were recreated.
  bool registerKingQueenSwapPromptState({
    required String boardId,
    required bool active,
  }) {
    if (!active) {
      if (_kingQueenSwapPromptBoardId == boardId) {
        _kingQueenSwapPromptActive = false;
      }
      return false;
    }
    if (_kingQueenSwapPromptBoardId == boardId && _kingQueenSwapPromptActive) {
      return false;
    }
    _kingQueenSwapPromptBoardId = boardId;
    _kingQueenSwapPromptActive = true;
    return true;
  }

  bool get consoleTraceOutputEnabled => _consoleTraceOutputEnabled;

  Stream<bool> get consoleTraceOutputStream =>
      _consoleTraceOutputController.stream;

  /// Requests one coherent Home snapshot through the current State owner.
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async {
    final generation = _connectionGeneration;
    LitchDeviceCapabilities capabilities;
    try {
      capabilities = await _connectionCapabilitiesForDomain(deviceId);
    } on OwnerPairingNotReady {
      return false;
    }
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly ||
        !isConnected ||
        connectedId != deviceId ||
        !_shouldRunConnectedBridges()) {
      return false;
    }
    try {
      final session = await openConnectionSession(deviceId);
      return generation == _connectionGeneration &&
          session != null &&
          await _requestConnectionStateSnapshot(session, homeOnly: true);
    } catch (error) {
      _pushAppLog('Home state replay could not be confirmed: $error');
      return false;
    }
  }

  Stream<ConnectionStateUpdate> get connectionStream =>
      _connectionController.stream;

  Stream<String> get otaStatusStream => _otaStatusController.stream;

  Stream<String> get settingsChangedStream => _settingsChangedController.stream;

  Stream<LitchControlEvent> get controlActionEvents =>
      _controlActionController.stream;

  LitchSupportSessionDiagnostics get supportSessionDiagnostics =>
      LitchSupportSessionDiagnostics(
        settingReads: _settingReadDiagnostics.snapshot,
        settingsWriteAttempts: _settingsWriteAttempts,
        settingsWriteSucceeded: _settingsWriteSucceeded,
        settingsWriteFailed: _settingsWriteFailed,
        lastSettingsOperation: _lastSettingsOperation,
        lastSettingsFailureToken: _lastSettingsFailureToken,
        controlNativeWrites: _controlNativeWrites,
        controlTimeouts: _controlTimeouts,
        controlIntents: _controlIntents,
        controlCoalescedTaps: _controlCoalescedTaps,
        controlRecoveryReadRequests: _controlRecoveryReadRequests,
        controlBusyRetries: _controlBusyRetries,
        controlRecoveryConfirmed: _controlRecoveryConfirmed,
        controlRecoveryUnconfirmed: _controlRecoveryUnconfirmed,
        notificationRepairOperations: _notificationRepairOperations,
        notificationRepairAttempts: _notificationRepairAttempts,
        notificationRepairRecovered: _notificationRepairRecovered,
        notificationRepairExhausted: _notificationRepairExhausted,
        notificationRepairCancelled: _notificationRepairCancelled,
        notificationRepairSaturated: _notificationRepairSaturated,
      );

  int _incrementSupportCounter(int value) {
    if (value == _maximumSupportCounter) {
      _notificationRepairSaturated = true;
      return value;
    }
    return value + 1;
  }

  void recordNotificationRepairOperation() {
    _notificationRepairOperations = _incrementSupportCounter(
      _notificationRepairOperations,
    );
  }

  void recordNotificationRepairAttempt() {
    _notificationRepairAttempts = _incrementSupportCounter(
      _notificationRepairAttempts,
    );
  }

  void recordNotificationRepairRecovered() {
    _notificationRepairRecovered = _incrementSupportCounter(
      _notificationRepairRecovered,
    );
  }

  void recordNotificationRepairExhausted() {
    _notificationRepairExhausted = _incrementSupportCounter(
      _notificationRepairExhausted,
    );
  }

  void recordNotificationRepairCancelled() {
    _notificationRepairCancelled = _incrementSupportCounter(
      _notificationRepairCancelled,
    );
  }

  int recordControlIntent({
    required bool coalesced,
    LitchControlAction? action,
  }) {
    if (coalesced) {
      ++_controlCoalescedTaps;
      return 0;
    }
    ++_controlIntents;
    if (action == null) return 0;
    return _appTrace.beginUserAction(switch (action) {
      LitchControlAction.hint => SupportActionKind.hint,
      LitchControlAction.undo => SupportActionKind.undo,
      LitchControlAction.newGame => SupportActionKind.newGame,
      LitchControlAction.coaching => SupportActionKind.coaching,
    });
  }

  void recordSupportActionOutcome(int action, SupportActionOutcome outcome) =>
      _appTrace.finishUserAction(action, outcome);

  void recordSupportControlOutcome(
    int action,
    LitchControlEvent event, {
    bool recovering = false,
  }) {
    final outcome = switch (event.phase) {
      LitchControlPhase.pending => SupportActionOutcome.requested,
      LitchControlPhase.accepted => SupportActionOutcome.accepted,
      LitchControlPhase.applied => SupportActionOutcome.applied,
      LitchControlPhase.noop => SupportActionOutcome.noChange,
      LitchControlPhase.rejected =>
        recovering
            ? SupportActionOutcome.accepted
            : SupportActionOutcome.rejected,
      LitchControlPhase.timeout ||
      LitchControlPhase.interrupted => SupportActionOutcome.unconfirmed,
      LitchControlPhase.sendFailed || LitchControlPhase.unavailable =>
        recovering
            ? SupportActionOutcome.unconfirmed
            : SupportActionOutcome.failed,
    };
    _appTrace.finishUserAction(action, outcome);
  }

  void recordControlRecoveryRead() => ++_controlRecoveryReadRequests;

  void recordControlBusyRetry() => ++_controlBusyRetries;

  void recordControlRecovery({required bool confirmed}) {
    if (confirmed) {
      ++_controlRecoveryConfirmed;
    } else {
      ++_controlRecoveryUnconfirmed;
    }
  }

  Object? latestSettingValue(String key) => _latestSettingValues[key];

  List<LitchSetting> _withTypedSettingDescriptors(
    Iterable<LitchSetting> settings,
  ) {
    final existing = settings
        .where(
          (setting) =>
              !SettingsStateWireCodec.extensionKeys.contains(setting.key),
        )
        .toList();
    return <LitchSetting>[
      ...existing.map(_rebaseSettingOnLatestValue),
      for (final descriptor in _typedSettingDescriptors)
        if (_latestSettingValues.containsKey(descriptor.key))
          _rebaseSettingOnLatestValue(descriptor),
    ];
  }

  /// Returns controls that can be rendered immediately from the authoritative
  /// connection snapshot, without waiting for immutable descriptor metadata.
  ///
  /// A confirmed enumeration wins when one is already cached. On the first
  /// Setup visit, the stable quick-setup schema is intersected with keys the
  /// current `board_state` actually advertised.
  List<LitchSetting> immediateSettingsSnapshot(String deviceId) {
    if (!isConnected || connectedId != deviceId || !hasBoardStateSnapshot) {
      return const <LitchSetting>[];
    }
    final List<LitchSetting>? descriptors = _settingsDescriptors;
    if (descriptors != null &&
        identical(
          _settingsDescriptorCapabilities,
          deviceCapabilities(deviceId),
        )) {
      return _withTypedSettingDescriptors(descriptors);
    }
    return _withTypedSettingDescriptors(
      _quickSetupDescriptors
          .where(
            (LitchSetting setting) =>
                _latestSettingValues.containsKey(setting.key),
          )
          .map(_rebaseSettingOnLatestValue)
          .toList(),
    );
  }

  LitchSettingsReadAuthority settingsReadAuthority(String deviceId) {
    if (isConnected &&
        connectedId == deviceId &&
        deviceCapabilities(deviceId).supportsConnectionSession) {
      // Descriptor completeness is metadata; shared-session live values need
      // a complete validated binary snapshot from this connection.
      return _binarySettingsStateObserved
          ? LitchSettingsReadAuthority.confirmed
          : LitchSettingsReadAuthority.unavailable;
    }
    return _settingsAuthorityByDevice[deviceId] ??
        LitchSettingsReadAuthority.unknown;
  }

  LitchControlAvailability controlActionAvailability(String deviceId) {
    if (!isConnected ||
        connectedId != deviceId ||
        _unavailableControlProtocolDevices.contains(deviceId)) {
      return LitchControlAvailability.unavailable;
    }
    return _knownControlProtocolDevices.contains(deviceId)
        ? LitchControlAvailability.available
        : LitchControlAvailability.unknown;
  }

  LitchDeviceCapabilities deviceCapabilities(String deviceId) {
    return _capabilitiesByDevice[deviceId] ??
        LitchDeviceCapabilities.unavailable;
  }

  @visibleForTesting
  static List<bool> connectionModesForPlatform(TargetPlatform platform) {
    return platform == TargetPlatform.android
        ? const <bool>[false, true, true]
        : const <bool>[false];
  }

  @visibleForTesting
  static Duration connectionRetryDelayForAttempt(
    TargetPlatform platform,
    int failedAttempt,
  ) {
    if (platform == TargetPlatform.android && failedAttempt >= 2) {
      // Samsung may close/unregister a failed GATT client correctly yet reject
      // the next direct connection with status 133 while its controller is
      // still retiring the ACL. One bounded cool-down avoids requiring a
      // Bluetooth toggle after rapid install/disconnect test cycles.
      return const Duration(milliseconds: 1500);
    }
    return const Duration(milliseconds: 250);
  }

  @visibleForTesting
  static bool isLitchCandidateDevice(DiscoveredDevice device) {
    if (device.serviceUuids.contains(BleProtocol.baseService) ||
        device.serviceUuids.contains(BleProtocol.settingsService)) {
      return true;
    }

    final String name = device.name.trim().toLowerCase();
    return BleProtocol.compatibleDeviceNames.any(
      (String candidate) => name.contains(candidate.toLowerCase()),
    );
  }

  /// Preserve the platform identity of a board whose Litch services were
  /// previously established. Its user-editable advertising name is not an
  /// authentication or discovery boundary.
  void rememberCompatibleDevice(String deviceId) {
    final String candidate = deviceId.trim();
    if (candidate.isNotEmpty) _compatibleDeviceIds.add(candidate);
  }

  bool get isConnected =>
      _connectionState?.connectionState == DeviceConnectionState.connected;

  String? get connectedId => _connectionState?.deviceId;

  Future<void> setAppForegroundActive(bool isForeground) async {
    if (_isAppForeground == isForeground) {
      return;
    }

    _isAppForeground = isForeground;
    _trace(
      'app.lifecycle',
      fields: <String, Object?>{
        'state': isForeground ? 'active' : 'background',
      },
    );

    if (!isForeground) {
      _supportCollection?.stop(SupportQueryOutcome.canceled);
      await stopScan();
      return;
    }

    final id = connectedId;
    if (isConnected && id != null) unawaited(_sendKeepAlive(id));
    await _applyConnectedBridgePolicy();
  }

  bool get appForegroundActive => _isAppForeground;

  bool _shouldRunConnectedBridges() {
    // Subscriptions, accepted receipts and liveness belong to the native link,
    // including while the OS allows this process to run in the background.
    return !_disposed && isConnected && !_repairingGattInventory;
  }

  StreamSubscription<List<int>>? _invalidateConsoleBridge() {
    _retireConsolePacketObserver(StateError('Console subscription retired.'));
    ++_consoleBridgeGeneration;
    _consoleRetryTimer?.cancel();
    _consoleRetryTimer = null;
    final StreamSubscription<List<int>>? subscription = _consoleSub;
    _consoleSub = null;
    // A replacement stream cannot complete bytes from the previous stream.
    // Retain complete diagnostic history, but discard all unfinished framing.
    _clearCurrentLine();
    _pendingCarriageReturn = false;
    return subscription;
  }

  bool _isCurrentConsoleBridge(String deviceId, int generation) {
    return generation == _consoleBridgeGeneration &&
        isConnected &&
        connectedId == deviceId &&
        _shouldRunConnectedBridges();
  }

  bool _isCurrentSettingsSession(String deviceId, int generation) {
    return !_disposed &&
        generation == _settingsSessionGeneration &&
        isConnected &&
        connectedId == deviceId;
  }

  bool _isCurrentSettingsBridge(
    String deviceId,
    int sessionGeneration,
    int bridgeGeneration,
  ) {
    return _isCurrentSettingsSession(deviceId, sessionGeneration) &&
        bridgeGeneration == _settingsBridgeGeneration &&
        _shouldRunConnectedBridges();
  }

  StreamSubscription<List<int>>? _invalidateSettingsSession({
    bool clearCachedState = false,
  }) {
    _cancelBulkConfirmations();
    ++_settingsSessionGeneration;
    ++_settingsBridgeGeneration;
    _settingsRwRetryTimer?.cancel();
    _settingsRwRetryTimer = null;
    _settingsIndexRefreshInFlight = false;
    _settingsEnumerationInFlight = null;
    _settingsEnumerationDeviceId = null;
    _settingsDescriptors = null;
    _settingsDescriptorCapabilities = null;
    _settingsAuthorityByDevice.clear();
    _pendingSettingsNotifyPayloadsByIndex.clear();
    final StreamSubscription<List<int>>? subscription = _settingsRwSub;
    _settingsRwSub = null;
    if (clearCachedState) {
      _completeConnectionSettings = null;
      _connectionPlayerPalette = null;
      _settingsKeysByIndex.clear();
      _latestSettingValues.clear();
      _settingNotificationVersions.clear();
      _settingNotificationVersion = 0;
      _pendingBoardStateReceipt?.complete();
      _pendingBoardStateReceipt = null;

      _bulkSettingsRevision = null;
      _bulkRefreshTimer?.cancel();
      _bulkRefreshTimer = null;
      _bulkRefreshPending = false;
    }
    return subscription;
  }

  bool _ensureSettingsResponseBridge(String deviceId) {
    if (!_isCurrentSettingsSession(deviceId, _settingsSessionGeneration) ||
        !_isAppForeground ||
        !_shouldRunConnectedBridges() ||
        _activeConnectionSession?.isActivated == true) {
      return false;
    }
    if (_settingsRwSub == null) {
      _startSettingsRwBridge(deviceId);
    }
    return _settingsRwSub != null;
  }

  Future<void> _applyConnectedBridgePolicy({String? deviceId}) async {
    if (identifyConnectionActive) return;
    if (!isConnected) {
      return;
    }

    final String id = deviceId ?? connectedId ?? '';
    if (id.isEmpty) {
      return;
    }

    final generation = _connectionGeneration;
    LitchDeviceCapabilities capabilities;
    try {
      capabilities = await fetchDeviceCapabilities(deviceId: id);
    } catch (error) {
      _pushAppLog('Connected service discovery unavailable: $error');
      return;
    }
    if (generation != _connectionGeneration ||
        !isConnected ||
        connectedId != id ||
        !_shouldRunConnectedBridges()) {
      return;
    }
    if (!capabilities.supportsConnectionSession) return;
    if (_activeConnectionSession?.isActivated == true) _startKeepAlive(id);
    if (!await ensureProductReady(id) || generation != _connectionGeneration) {
      return;
    }
    if (capabilities.recoveryOnly) {
      if (_otaStatusSub == null) _startOtaStatusBridge(id);
      _startKeepAlive(id);
      return;
    }
    if (_consoleSub == null) {
      _startConsoleMirror(id);
    }
    if (_settingsRwSub == null &&
        _activeConnectionSession?.isActivated != true) {
      _startSettingsRwBridge(id);
    }
    if (_otaStatusSub == null) {
      _startOtaStatusBridge(id);
    }
    _startKeepAlive(id);
  }

  Stream<ConnectionStateUpdate> _openConnectionStream(
    String deviceId, {
    required Duration connectionTimeout,
    bool useAdvertisingOnAndroid = false,
  }) {
    if (defaultTargetPlatform == TargetPlatform.android &&
        useAdvertisingOnAndroid) {
      // The SDK may prescan when its advertising cache is older than25s.
      // Reserve conservatively without reading or mutating that private cache.
      final now = _scanNow();
      if (!_canStartNativeScan(now)) {
        return Stream<ConnectionStateUpdate>.error(
          StateError(
            'Android advertising fallback scan admission is unavailable.',
          ),
        );
      }
      _androidScanStarts.add(now);
      _pushAppLog(
        'Connecting via connectToAdvertisingDevice (Android) id=$deviceId timeout=${connectionTimeout.inSeconds}s',
      );
      return _ble.connectToAdvertisingDevice(
        id: deviceId,
        withServices: <Uuid>[BleProtocol.baseService],
        prescanDuration: const Duration(seconds: 2),
        connectionTimeout: connectionTimeout,
      );
    }

    if (defaultTargetPlatform == TargetPlatform.android) {
      _pushAppLog(
        'Connecting via connectToDevice (Android) id=$deviceId timeout=${connectionTimeout.inSeconds}s',
      );
    } else {
      _pushAppLog(
        'Connecting via connectToDevice id=$deviceId timeout=${connectionTimeout.inSeconds}s',
      );
    }
    return _ble.connectToDevice(
      id: deviceId,
      connectionTimeout: connectionTimeout,
    );
  }

  bool _ownsScanReservation(int generation) {
    return !_disposed && _isScanning && generation == _scanGeneration;
  }

  Future<bool> _resetFailedBleSetup() {
    final pending = _bleSetupResetFuture;
    if (pending != null) return pending;
    final future = () async {
      ++_bleSetupGeneration;
      _lastBleStatus = null;
      final scan = _scanSub;
      final status = _bleStatusSub;
      _scanSub = null;
      _bleStatusSub = null;
      _setBleAvailability(BleAvailability.checking);
      try {
        await _safeCancelSubscription(scan, 'failed scan cleanup unavailable');
        await _safeCancelSubscription(
          status,
          'failed BLE status cleanup unavailable',
        );
        // A failed status owner can still have a live native connection. Enter
        // its existing cleanup owner, rather than only awaiting prior cleanup.
        await _disconnect(emitUpdate: false);
        if (_disposed) return false;
        // Replace only a failed SDK owner, through its public lifecycle. Keep
        // Android's native scan budget: rebuilding the SDK does not reset it.
        await _ble.deinitialize();
        if (_disposed) return false;
        await _ble.initialize();
        if (_disposed) return false;
        _bleSetupNeedsReset = false;
        _ensureBleStatusObservation();
        return true;
      } catch (error) {
        _bleSetupNeedsReset = true;
        _setBleAvailability(BleAvailability.unavailable);
        _pushAppLog('BLE setup recovery unavailable: $error');
        return false;
      }
    }();
    _bleSetupResetFuture = future;
    return future.whenComplete(() {
      if (identical(_bleSetupResetFuture, future)) _bleSetupResetFuture = null;
    });
  }

  bool _canStartNativeScan(DateTime now) {
    if (defaultTargetPlatform != TargetPlatform.android) return true;
    _androidScanStarts.removeWhere(
      (start) => now.difference(start) >= _androidScanWindow,
    );
    return _androidScanStarts.length < _androidScanStartLimit;
  }

  Future<void> startScan() async {
    await _startScan();
  }

  Future<void> rescan() async {
    await _startScan(renewScan: true);
  }

  Future<void> _startScan({
    bool retainDiscoveredBoards = false,
    bool renewScan = false,
  }) async {
    if (_disposed ||
        _pairingRecovery != null ||
        _connectAttemptInProgress ||
        isConnected ||
        !_isAppForeground) {
      return;
    }
    _scanRequested = true;
    // Keep a healthy native listener when Android cannot admit its replacement.
    // Pull, foreground resume and the existing gate tick share this same budget.
    if (!_canStartNativeScan(_scanNow())) return;
    if (_isScanning) {
      final DateTime? startedAt = _scanStartedAt;
      if (startedAt == null ||
          (!renewScan &&
              (_boardsById.isNotEmpty ||
                  _scanNow().difference(startedAt) <
                      const Duration(seconds: 2)))) {
        return;
      }

      // Fence callbacks before awaiting native cancellation. A null start time
      // holds this reservation while the existing gate tick keeps running.
      final int renewalGeneration = ++_scanGeneration;
      _scanStartedAt = null;
      await _scanSub?.cancel();
      if (!_ownsScanReservation(renewalGeneration)) {
        return;
      }
      _scanSub = null;
      _isScanning = false;
    }

    final int scanGeneration = ++_scanGeneration;
    _isScanning = true;
    bool canOpenScan = false;

    try {
      if (_bleSetupNeedsReset) {
        if (!await _resetFailedBleSetup()) return;
      } else if (_scanSub != null) {
        // A failed/throttled SDK stream may remain open after delivering error.
        // Await its native cancellation before replacing that discovery owner.
        final failedScan = _scanSub;
        await _safeCancelSubscription(
          failedScan,
          'failed scan cleanup unavailable',
        );
        if (identical(_scanSub, failedScan)) _scanSub = null;
      }
      if (!_ownsScanReservation(scanGeneration)) return;
      if (!await _ensurePermissions(requestIfNeeded: false)) {
        return;
      }
      if (!_ownsScanReservation(scanGeneration)) {
        return;
      }

      final bool ready = await _ensureBleReadyForScan();
      canOpenScan =
          ready &&
          _ownsScanReservation(scanGeneration) &&
          !_connectAttemptInProgress &&
          !isConnected &&
          _isAppForeground;
    } finally {
      if (!canOpenScan && _ownsScanReservation(scanGeneration)) {
        _isScanning = false;
      }
    }

    if (!canOpenScan) {
      return;
    }

    final DateTime now = _scanNow();
    if (!_canStartNativeScan(now)) {
      _isScanning = false;
      return;
    }
    final DateTime? retryNotBefore = _scanRetryNotBefore;
    if (retryNotBefore != null && now.isBefore(retryNotBefore)) {
      final DateTime? lastLog = _lastScanCooldownLogAt;
      if (lastLog == null ||
          now.difference(lastLog) >= const Duration(seconds: 3)) {
        _lastScanCooldownLogAt = now;
        final int remainingMs = retryNotBefore.difference(now).inMilliseconds;
        final int remainingSec = (remainingMs / 1000).ceil();
        _pushAppLog('Scan cooldown active; retry in ${remainingSec}s.');
      }
      _isScanning = false;
      return;
    }

    _scanRetryNotBefore = null;
    _lastScanCooldownLogAt = null;

    _nonCandidateScanHitsLogged = 0;
    if (!retainDiscoveredBoards) {
      _boardsById.clear();
      _boardsController.add(const <LitchBoard>[]);
    }

    // Some compatible boards omit service UUIDs from advertisements. Discover
    // their names immediately instead of waiting for a filtered scan to fail.
    _pushAppLog('Starting BLE scan (unfiltered).');
    _scanStartedAt = now;
    if (defaultTargetPlatform == TargetPlatform.android) {
      _androidScanStarts.add(now);
    }

    _scanSub = _ble
        .scanForDevices(
          withServices: const <Uuid>[],
          scanMode: _isAppForeground ? ScanMode.lowLatency : ScanMode.lowPower,
        )
        .listen(
          (DiscoveredDevice device) {
            if (!_ownsScanReservation(scanGeneration)) {
              return;
            }

            if (!isLitchCandidateDevice(device) &&
                !_compatibleDeviceIds.contains(device.id)) {
              if (defaultTargetPlatform == TargetPlatform.iOS &&
                  _nonCandidateScanHitsLogged < 5) {
                _nonCandidateScanHitsLogged += 1;
                _pushAppLog(
                  'Scan non-candidate: name="${device.name}" id=${device.id} services=${device.serviceUuids.length}',
                );
              }
              return;
            }

            // A primary advertisement may omit the local name carried by its
            // scan response. Do not replace a learned name with that absence.
            final advertisedName = device.name.trim();
            final cleanedName = advertisedName.isNotEmpty
                ? advertisedName
                : _boardsById[device.id]?.name ?? '';
            final bool isNewBoard = !_boardsById.containsKey(device.id);
            _boardsById[device.id] = LitchBoard(
              id: device.id,
              name: cleanedName,
              rssi: device.rssi,
            );

            if (isNewBoard) {
              _pushAppLog(
                'Scan hit: $cleanedName (${device.id}) RSSI ${device.rssi}',
              );
            }

            final List<LitchBoard> boards = _boardsById.values.toList()
              ..sort((LitchBoard a, LitchBoard b) {
                final int byName = a.name.toLowerCase().compareTo(
                  b.name.toLowerCase(),
                );
                if (byName != 0) {
                  return byName;
                }
                return a.id.compareTo(b.id);
              });

            _boardsController.add(boards);
          },
          onError: (Object error, StackTrace stackTrace) {
            if (!_ownsScanReservation(scanGeneration)) {
              return;
            }

            _isScanning = false;
            _scanStartedAt = null;

            final String message = error.toString();
            final String lower = message.toLowerCase();
            if (lower.contains('scan throttle')) {
              _scanRetryNotBefore = _scanNow().add(const Duration(seconds: 8));
              _pushAppLog(
                'Scan throttled by Android; backing off for 8s. $message',
              );
              return;
            }

            _scanRetryNotBefore = _scanNow().add(const Duration(seconds: 2));
            _bleSetupNeedsReset = true;
            _pushAppLog('Scan error: $message');
          },
          onDone: () {
            if (!_ownsScanReservation(scanGeneration)) {
              return;
            }

            _isScanning = false;
            _scanStartedAt = null;
            _bleSetupNeedsReset = true;
          },
        );
  }

  Future<void> stopScan() async {
    _scanRequested = false;
    _scanStartedAt = null;
    final int stoppedGeneration = ++_scanGeneration;
    final StreamSubscription<DiscoveredDevice>? subscription = _scanSub;
    await subscription?.cancel();
    if (stoppedGeneration == _scanGeneration) {
      _scanSub = null;
      _isScanning = false;
    }
  }

  Future<bool> connect(String deviceId) => _connect(deviceId);

  Future<bool> _connect(String deviceId, {bool identifyOnly = false}) async {
    if (_disposed || _connectAttemptInProgress) {
      return false;
    }
    if (_pairingRecovery?.deviceId == deviceId && !_pairingRecoveryConnecting) {
      return false;
    }
    if (_pairingRecovery != null && _pairingRecovery?.deviceId != deviceId) {
      await dismissPairingRecovery();
    }

    _ownerPairingRefusal = null;
    _ownerPairingRefusedDeviceId = null;
    final int connectionGeneration = _retireConnection();
    _productReadinessPending = true;
    _identifyConnectionGeneration = identifyOnly ? connectionGeneration : null;
    _trace(
      'ble.connect_requested',
      fields: <String, Object?>{'generation': connectionGeneration},
    );
    _clearRetainedHomeState();
    _connectAttemptInProgress = true;
    _persistConsoleSnapshot();
    try {
      // Restore before a connected event can build the ordinary app shell.
      // A storage error blocks new OTA, while leaving BLE/support available.
      try {
        await restorePendingOtaVerification();
      } catch (error) {
        _pushAppLog('Saved update verification is unavailable: $error');
      }
      if (!_ownsConnectionReservation(connectionGeneration)) return false;
      _pushAppLog('Connect requested for $deviceId');
      // Retire logical authority immediately, but keep native admission fenced
      // until every resource belonging to the previous disconnect is cleaned up.
      await _disconnectFuture;
      final resetting = _bleSetupResetFuture;
      if (resetting != null && !await resetting) return false;
      if (_bleSetupNeedsReset && !await _resetFailedBleSetup()) return false;
      if (!_ownsConnectionReservation(connectionGeneration)) return false;
      await stopScan();
      _failPendingSettings(LitchSettingWriteFailure.connectionReplaced);
      await _safeCancelSubscription(
        _invalidateConsoleBridge(),
        'console subscription replacement failed',
      );
      await _safeCancelSubscription(
        _invalidateSettingsSession(clearCachedState: true),
        'settings subscription replacement failed',
      );
      final StreamSubscription<OtaStatusEvent>? otaStatusSubscription =
          _otaStatusSub;
      _otaStatusSub = null;
      await _safeCancelSubscription(
        otaStatusSubscription,
        'OTA status subscription replacement failed',
      );
      await _disposeOtaControlChannel();
      _clearConnectionScopedCapabilities();
      _invalidateGattSession();
      if (!_ownsConnectionReservation(connectionGeneration)) {
        return false;
      }

      _pairingWatchDeviceId = deviceId;
      _pairingWatchGeneration = connectionGeneration;
      await _pairingRecoveryGateway.watchDevice(deviceId, connectionGeneration);
      if (!_ownsConnectionReservation(connectionGeneration)) return false;

      final List<bool> modes = connectionModesForPlatform(
        defaultTargetPlatform,
      );

      for (int index = 0; index < modes.length; ++index) {
        final bool useAdvertising = modes[index];
        if (index == 1) {
          _pushAppLog('Retrying connect using Android advertising fallback.');
        } else if (index > 1) {
          _pushAppLog(
            'Retrying connect after bounded Android GATT recovery backoff.',
          );
        }

        await _connSub?.cancel();
        if (!_ownsConnectionReservation(connectionGeneration)) return false;
        _connSub = null;
        _connectionState = null;
        _invalidateGattSession();
        bool sawConnected = false;

        final Completer<bool> connectedCompleter = Completer<bool>();

        _connSub =
            _openConnectionStream(
              deviceId,
              connectionTimeout: useAdvertising
                  ? const Duration(seconds: 7)
                  : const Duration(seconds: 5),
              useAdvertisingOnAndroid: useAdvertising,
            ).listen(
              (ConnectionStateUpdate update) {
                _trace(
                  'ble.connection_observed',
                  fields: <String, Object?>{
                    'generation': connectionGeneration,
                    'state': update.connectionState.name,
                    'owns_reservation': _ownsConnectionReservation(
                      connectionGeneration,
                    ),
                  },
                );
                if (!_ownsConnectionReservation(connectionGeneration)) {
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(false);
                  }
                  return;
                }
                final bool isDisconnected =
                    update.connectionState ==
                    DeviceConnectionState.disconnected;

                if (update.connectionState == DeviceConnectionState.connected) {
                  sawConnected = true;
                }

                final bool hasFallbackMode = (index + 1) < modes.length;
                if (isNativePairingKeyMissing(update.failure?.message)) {
                  _suspendForStalePairing(deviceId);
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(false);
                  }
                  return;
                }
                if (isDisconnected && !sawConnected && hasFallbackMode) {
                  _pushAppLog(
                    'Connect mode failed before connected (mode=${useAdvertising ? 'advertising' : 'direct'}); trying fallback.',
                  );
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(false);
                  }
                  return;
                }

                _connectionState = update;
                _trace(
                  'ble.connection_state',
                  fields: <String, Object?>{
                    'generation': connectionGeneration,
                    'state': update.connectionState.name,
                    'has_failure': update.failure != null,
                  },
                );
                if (update.connectionState == DeviceConnectionState.connected) {
                  rememberCompatibleDevice(deviceId);
                  _productReadinessPending = true;
                  if (_pairingRecoveryConnecting &&
                      _pairingRecovery?.deviceId == deviceId) {
                    _publishPairingRecovery(null);
                  }
                  _openGattSession(deviceId);
                } else if (isDisconnected) {
                  _invalidateGattSession();
                }
                if (!identifyOnly) _emitConnectionUpdate(update);
                _pushAppLog(
                  'Connection update: state=${update.connectionState.name} device=${update.deviceId}',
                );

                if (isDisconnected) {
                  unawaited(
                    _captureConnectionDiagnostics(
                      deviceId,
                      connectionGeneration,
                    ),
                  );
                  _clearRetainedHomeState();
                  _failPendingSettings(LitchSettingWriteFailure.disconnected);
                  _setConsoleTraceOutputEnabled(false);
                  unawaited(
                    _safeCancelSubscription(
                      _invalidateConsoleBridge(),
                      'console subscription disconnect cleanup failed',
                    ),
                  );
                  unawaited(
                    _safeCancelSubscription(
                      _invalidateSettingsSession(clearCachedState: true),
                      'settings subscription disconnect cleanup failed',
                    ),
                  );
                  _knownControlProtocolDevices.remove(deviceId);
                  _unavailableControlProtocolDevices.remove(deviceId);
                  _stopKeepAlive();
                  _invalidateOtaControlSession();
                  if (sawConnected &&
                      connectionGeneration == _connectionGeneration) {
                    _retireConnection();
                  }
                }

                if (update.connectionState == DeviceConnectionState.connected) {
                  _pushAppLog('Connected at BLE layer; starting bridges...');
                  unawaited(
                    _safeCancelSubscription(
                      _invalidateSettingsSession(clearCachedState: true),
                      'settings subscription session replacement failed',
                    ),
                  );
                  _knownControlProtocolDevices.remove(deviceId);
                  _unavailableControlProtocolDevices.remove(deviceId);
                  _resetLinkProfileForPlatform();
                  _resetConsoleBuffer(deviceId);
                  unawaited(_applyConnectedBridgePolicy(deviceId: deviceId));
                  _linkCalibrationFuture = _calibrateLinkProfile(deviceId);
                  unawaited(
                    _openSharedSessionIfSupported(
                      deviceId,
                      connectionGeneration,
                    ),
                  );
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(true);
                  }
                }

                if (isDisconnected && !connectedCompleter.isCompleted) {
                  _pushAppLog('Disconnected from board.');
                  connectedCompleter.complete(false);
                }
              },
              onError: (Object error, StackTrace stackTrace) {
                if (!_ownsConnectionReservation(connectionGeneration)) {
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(false);
                  }
                  return;
                }
                _pushAppLog('Connection error: $error');
                _trace(
                  'ble.connection_error',
                  fields: <String, Object?>{
                    'generation': connectionGeneration,
                    'kind': error.runtimeType.toString(),
                  },
                );
                _recordUnexpectedDisconnect(deviceId, connectionGeneration);
                if (!connectedCompleter.isCompleted) {
                  connectedCompleter.complete(false);
                }
              },
              onDone: () {
                if (!_ownsConnectionReservation(connectionGeneration)) {
                  if (!connectedCompleter.isCompleted) {
                    connectedCompleter.complete(false);
                  }
                  return;
                }
                _recordUnexpectedDisconnect(deviceId, connectionGeneration);
              },
            );

        void retirePendingConnection() {
          if (!connectedCompleter.isCompleted) {
            connectedCompleter.complete(false);
          }
        }

        _connectionWaiters.add(retirePendingConnection);
        if (!_ownsConnectionReservation(connectionGeneration)) {
          retirePendingConnection();
        }
        final bool connected = await connectedCompleter.future
            .timeout(
              useAdvertising
                  ? const Duration(seconds: 8)
                  : const Duration(seconds: 6),
              onTimeout: () {
                if (!connectedCompleter.isCompleted) {
                  connectedCompleter.complete(false);
                }
                final DeviceConnectionState? lastState =
                    _connectionState?.connectionState;
                _pushAppLog(
                  'Connect timeout for $deviceId (last_state=${lastState?.name ?? 'none'} mode=${useAdvertising ? 'advertising' : 'direct'})',
                );
                _trace(
                  'ble.connection_timeout',
                  fields: <String, Object?>{
                    'generation': connectionGeneration,
                    'last_state': lastState?.name ?? 'none',
                    'mode': useAdvertising ? 'advertising' : 'direct',
                  },
                );
                return false;
              },
            )
            .whenComplete(() {
              _connectionWaiters.remove(retirePendingConnection);
            });

        unawaited(
          _captureConnectionDiagnostics(deviceId, connectionGeneration),
        );

        if (connected) {
          return true;
        }

        if (!_ownsConnectionReservation(connectionGeneration)) {
          return false;
        }

        await _connSub?.cancel();
        if (!_ownsConnectionReservation(connectionGeneration)) return false;
        _connSub = null;
        _connectionState = null;
        _invalidateGattSession();
        if ((index + 1) < modes.length) {
          await Future<void>.delayed(
            connectionRetryDelayForAttempt(defaultTargetPlatform, index + 1),
          );
          if (!_ownsConnectionReservation(connectionGeneration)) {
            return false;
          }
        }
      }

      return false;
    } finally {
      _connectAttemptInProgress = false;
      _persistConsoleSnapshot();
    }
  }

  Future<void> disconnect() async {
    if (_disposed) {
      return;
    }
    _supportCollection?.stop(SupportQueryOutcome.sessionChanged);
    _retireConnection();
    await _disconnect(emitUpdate: true);
  }

  Future<void> _disconnect({required bool emitUpdate}) {
    // Publish one completion before asynchronous disconnected observers can
    // reconnect. Duplicate disconnect/dispose callers share all old cleanup.
    return _disconnectFuture ??= _performDisconnect(
      emitUpdate: emitUpdate,
    ).whenComplete(() => _disconnectFuture = null);
  }

  Future<void> _performDisconnect({required bool emitUpdate}) async {
    final String? priorDeviceId = connectedId;
    _clearRetainedHomeState();
    _failPendingSettings(LitchSettingWriteFailure.disconnected);
    final StreamSubscription<List<int>>? consoleSubscription =
        _invalidateConsoleBridge();
    final StreamSubscription<List<int>>? settingsSubscription =
        _invalidateSettingsSession(clearCachedState: true);
    _stopKeepAlive();
    final connectionSubscription = _connSub;
    _connSub = null;
    _connectionState = null;
    _setConsoleTraceOutputEnabled(false);
    _invalidateGattSession();
    if (priorDeviceId != null) {
      _knownControlProtocolDevices.remove(priorDeviceId);
      _unavailableControlProtocolDevices.remove(priorDeviceId);
    }
    if (emitUpdate) {
      _emitConnectionUpdate(
        ConnectionStateUpdate(
          deviceId: '',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
    }

    await _safeCancelSubscription(
      connectionSubscription,
      'connection subscription cancellation failed',
    );

    await _safeCancelSubscription(
      consoleSubscription,
      'console subscription cancellation failed',
    );
    await _safeCancelSubscription(
      settingsSubscription,
      'settings subscription cancellation failed',
    );
    await _safeCancelSubscription(
      _otaStatusSub,
      'status subscription cancellation failed',
    );
    _otaStatusSub = null;
    await _disposeOtaControlChannel();

    _consoleCurrentLine.clear();
    _consoleCurrentLineOverflowed = false;
    _pendingCarriageReturn = false;
  }

  void _startKeepAlive(String deviceId) {
    if (_disposed) {
      return;
    }
    final Timer? existing = _keepAliveTimer;
    if (existing != null && existing.isActive) {
      return;
    }

    Future<void>.microtask(() => _sendKeepAlive(deviceId));
    _keepAliveTimer = Timer.periodic(_keepAliveInterval, (_) {
      unawaited(_sendKeepAlive(deviceId));
    });
  }

  void _stopKeepAlive() {
    _keepAliveTimer?.cancel();
    _keepAliveTimer = null;
  }

  Future<void> _sendKeepAlive(String deviceId) async {
    if (!_shouldRunConnectedBridges() ||
        !isConnected ||
        connectedId != deviceId ||
        _keepAliveWriteInFlight) {
      return;
    }

    // Discovery is not a failed liveness write. Do not leave timeout-expired
    // keepalive work waiting to send when exact handles eventually resolve.
    if (_usesExactGattSession && _resolvedGattSession == null) {
      return;
    }

    _keepAliveWriteInFlight = true;
    try {
      final ConnectionSession? session = _activeConnectionSession;
      if (!deviceCapabilities(deviceId).supportsConnectionSession) return;
      bool ok = false;
      try {
        if (session?.isActivated != true && _openingConnectionSession != null) {
          // The existing opener owns recovery while its native work settles.
          ok = true;
        } else {
          ok = await (() async {
            final live = session?.isActivated == true
                ? session
                : await openConnectionSession(deviceId);
            if (live?.isActivated != true) return false;
            await live!.heartbeat();
            return true;
          })().timeout(_keepAliveWriteTimeout, onTimeout: () => false);
        }
      } catch (_) {
        ok = false;
      }
      if (!ok && isConnected && connectedId == deviceId) {
        // Keep the connection owner's next liveness attempt alive after a
        // transient transport failure.
        _logOptionalCommandUnsupported(
          token: 'keepalive_write_failed',
          line: 'BLE keepalive write failed; retrying on the next interval.',
        );
      }
    } finally {
      _keepAliveWriteInFlight = false;
    }
  }

  @visibleForTesting
  void debugSetConnectedDevice(String deviceId) {
    _clearRetainedHomeState();
    final StreamSubscription<List<int>>? oldSettings =
        _invalidateSettingsSession(clearCachedState: true);
    unawaited(oldSettings?.cancel());
    _connectionState = ConnectionStateUpdate(
      deviceId: deviceId,
      connectionState: DeviceConnectionState.connected,
      failure: null,
    );
    _openGattSession(deviceId);
    _isAppForeground = true;
    _clearConnectionScopedCapabilities();
    _invalidateOtaControlSession();
    _persistConsoleSnapshot();
  }

  @visibleForTesting
  void debugSetDeviceCapabilities(
    String deviceId,
    LitchDeviceCapabilities capabilities,
  ) {
    _capabilitiesByDevice[deviceId] = capabilities;
  }

  @visibleForTesting
  void debugStartKeepAlive(String deviceId) => _startKeepAlive(deviceId);

  @visibleForTesting
  void debugStopKeepAlive() => _stopKeepAlive();

  @visibleForTesting
  void debugStartConsoleMirror(String deviceId) =>
      _startConsoleMirror(deviceId);

  @visibleForTesting
  void debugStopConsoleMirror() {
    _connectionState = null;
    _setConsoleTraceOutputEnabled(false);
    final StreamSubscription<List<int>>? subscription =
        _invalidateConsoleBridge();
    unawaited(subscription?.cancel());
  }

  @visibleForTesting
  void debugAppendConsolePacket(List<int> packet) =>
      _appendConsolePacket(packet);

  @visibleForTesting
  void debugSetConsoleTraceOutputEnabled(bool enabled) =>
      _setConsoleTraceOutputEnabled(enabled);

  @visibleForTesting
  void debugStartOtaStatusBridge(String deviceId) {
    _startOtaStatusBridge(deviceId);
  }

  void _resetConsoleBuffer(String deviceId) {
    // Completed diagnostics belong to this board across a reconnect. Framing
    // and Trace remain connection-scoped below.
    if (_consoleHistoryDeviceId != deviceId) {
      _consoleLines.clear();
      _consoleHistoryDeviceId = deviceId;
    }
    _consoleCurrentLine.clear();
    _consoleCurrentLineOverflowed = false;
    _pendingCarriageReturn = false;
    _setConsoleTraceOutputEnabled(false);
    _emitConsoleSnapshot();
  }

  Future<bool> sendIdentify(
    String deviceId,
    int number, {
    int durationMs = 10000,
    bool untilConnected = true,
  }) async {
    final body = ConnectionIdentifyCodec.start(number, durationMs);
    if (isConnected) {
      if (connectedId != deviceId) return false;
      try {
        return await _sendIdentifyOnCurrentConnection(deviceId, body);
      } catch (_) {
        return false;
      }
    }
    if (_connectAttemptInProgress) return false;
    final resumeScan = _isScanning;
    int? generation;
    try {
      final connecting = _connect(deviceId, identifyOnly: true);
      generation = _connectionGeneration;
      if (!await connecting) return false;
      return await _sendIdentifyOnCurrentConnection(deviceId, body);
    } catch (_) {
      return false;
    } finally {
      if (!_disposed &&
          generation != null &&
          _identifyConnectionGeneration == generation &&
          (generation == _connectionGeneration ||
              generation + 1 == _connectionGeneration) &&
          !_connectAttemptInProgress) {
        // Native loss can retire this reservation before its Ordered result.
        // A replacement connect changes the Identify marker synchronously.
        final retired = generation == _connectionGeneration
            ? _retireConnection()
            : _connectionGeneration;
        await _disconnect(emitUpdate: false);
        if (resumeScan &&
            retired == _connectionGeneration &&
            _identifyConnectionGeneration == generation &&
            !isConnected &&
            !_connectAttemptInProgress) {
          await _startScan(retainDiscoveredBoards: true);
        }
      }
    }
  }

  Future<bool> _sendIdentifyOnCurrentConnection(
    String deviceId,
    Uint8List body,
  ) async {
    final generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        isConnected &&
        connectedId == deviceId;
    final capabilities = await fetchDeviceCapabilities(deviceId: deviceId);
    if (!current() ||
        capabilities.recoveryOnly ||
        !capabilities.supportsIdentify) {
      return false;
    }
    final session = await openConnectionSession(deviceId);
    if (!current() || session == null) return false;
    final supported = await session.sendOrdered(
      ConnectionIdentifyCodec.opcode,
      ConnectionIdentifyCodec.probe(),
    );
    if (!current() ||
        supported.disposition != ConnectionSessionDisposition.applied) {
      return false;
    }
    final result = await session.sendOrdered(
      ConnectionIdentifyCodec.opcode,
      body,
    );
    return current() &&
        result.disposition == ConnectionSessionDisposition.applied;
  }

  Future<bool> _sendCommandJson(
    String deviceId,
    Map<String, Object> payload, {
    Uuid? characteristicId,
    bool allowWriteWithoutResponse = false,
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
  }) async {
    final int generation = _connectionGeneration;
    bool current() =>
        !_disposed &&
        generation == _connectionGeneration &&
        (isCurrent?.call() ?? true);
    supportScope?.check();
    if (!current()) return false;
    // Updater discovery establishes the mode; it cannot wait on itself.
    if (characteristicId != BleProtocol.otaControlCharacteristic &&
        (await _connectionCapabilitiesForDomain(deviceId)).recoveryOnly) {
      return false;
    }
    if (!current()) return false;
    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.baseService,
      characteristicId: characteristicId ?? BleProtocol.commandCharacteristic,
      deviceId: deviceId,
    );

    final List<int> encoded = utf8.encode(jsonEncode(payload));

    try {
      await _writeCharacteristicWithResponse(
        characteristic,
        value: encoded,
        supportScope: supportScope,
        isCurrent: current,
      );
      return true;
    } on SupportCollectionStopped {
      rethrow;
    } catch (_) {
      supportScope?.check();
      if (!allowWriteWithoutResponse || !current()) {
        return false;
      }

      try {
        await _writeCharacteristicWithoutResponse(
          characteristic,
          value: encoded,
          supportScope: supportScope,
          isCurrent: current,
        );
        return true;
      } on SupportCollectionStopped {
        rethrow;
      } catch (_) {
        return false;
      }
    }
  }

  Future<bool> sendIdentifyStop(String deviceId) async {
    return _stopIdentify(deviceId);
  }

  Future<bool> _stopIdentify(String deviceId) async {
    if (!isConnected || connectedId != deviceId) return false;
    try {
      return await _sendIdentifyOnCurrentConnection(
        deviceId,
        ConnectionIdentifyCodec.stop(),
      );
    } catch (_) {
      return false;
    }
  }

  /// The Applied receipt confirms that the board accepted its cancellable
  /// sleep transition. Local activity or a failed save may still cancel it.
  /// An uncertain receipt is never replayed outside the shared session owner.
  Future<ConnectionSessionOutcome?> requestSleep({
    required String deviceId,
  }) async {
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return null;
    }
    final session = _activeConnectionSession;
    if (!isConnected ||
        connectedId != deviceId ||
        session?.isActivated != true) {
      return null;
    }
    try {
      final outcome = await session!.sendOrdered(
        LitchControlProtocol.connectionActionOpcode,
        LitchControlProtocol.encodeConnectionSleep(),
      );
      return generation == _connectionGeneration &&
              isConnected &&
              connectedId == deviceId &&
              identical(session, _activeConnectionSession)
          ? outcome
          : null;
    } catch (_) {
      return null;
    }
  }

  Future<LitchControlEvent> dispatchControlAction({
    required String deviceId,
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) async {
    if (!_isAppForeground) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.interrupted,
      );
    }
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.interrupted,
      );
    }
    if (!capabilities.supportsConnectionSession || capabilities.recoveryOnly) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.sendFailed,
      );
    }
    if (!isConnected || connectedId != deviceId) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.interrupted,
      );
    }
    if (!_isValidControlRequest(
      action: action,
      owner: owner,
      enabled: enabled,
      undoCount: undoCount,
    )) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.sendFailed,
      );
    }
    final connectionSession = await openConnectionSession(deviceId);
    if (!_isAppForeground ||
        generation != _connectionGeneration ||
        connectionSession?.isActivated != true) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.interrupted,
      );
    }
    return _dispatchConnectionControlAction(
      deviceId: deviceId,
      connectionSession: connectionSession!,
      action: action,
      owner: owner,
      enabled: enabled,
      undoCount: undoCount,
    );
  }

  Future<LitchControlEvent> _dispatchConnectionControlAction({
    required String deviceId,
    required ConnectionSession connectionSession,
    required LitchControlAction action,
    required LitchControlOwner? owner,
    required bool? enabled,
    required int undoCount,
  }) async {
    if (controlActionAvailability(deviceId) ==
        LitchControlAvailability.unavailable) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.unavailable,
      );
    }
    final int? requestId = _requestIds.reserve();
    if (requestId == null) {
      return _unsentControlAction(
        action: action,
        owner: owner,
        phase: LitchControlPhase.sendFailed,
      );
    }
    final int generation = _connectionGeneration;
    _emitControlEvent(
      LitchControlEvent(
        requestId: requestId,
        action: action,
        owner: owner,
        phase: LitchControlPhase.pending,
      ),
    );
    try {
      ++_controlNativeWrites;
      final ConnectionSessionOutcome outcome = await connectionSession
          .sendOrdered(
            LitchControlProtocol.connectionActionOpcode,
            LitchControlProtocol.encodeConnectionAction(
              action: action,
              owner: owner,
              enabled: enabled,
              undoCount: undoCount,
            ),
          );
      if (generation != _connectionGeneration ||
          !identical(connectionSession, _activeConnectionSession) ||
          !isConnected ||
          connectedId != deviceId) {
        final interrupted = LitchControlEvent(
          requestId: requestId,
          action: action,
          owner: owner,
          phase: LitchControlPhase.interrupted,
        );
        _emitControlEvent(interrupted);
        return interrupted;
      }
      if (outcome.disposition == ConnectionSessionDisposition.cancelled) {
        final interrupted = LitchControlEvent(
          requestId: requestId,
          action: action,
          owner: owner,
          phase: LitchControlPhase.interrupted,
        );
        _emitControlEvent(interrupted);
        return interrupted;
      }
      final bool applied =
          outcome.disposition == ConnectionSessionDisposition.applied;
      final LitchMutationReason reason = applied
          ? LitchMutationReason.none
          : LitchMutationReason.fromWireCode(outcome.reason) ??
                LitchMutationReason.badRequest;
      if (!applied &&
          (reason == LitchMutationReason.unknownAction ||
              reason == LitchMutationReason.unsupported)) {
        _knownControlProtocolDevices.remove(deviceId);
        _unavailableControlProtocolDevices.add(deviceId);
      } else if (!_unavailableControlProtocolDevices.contains(deviceId)) {
        // An earlier request can finish after controls were refused on this attachment.
        _knownControlProtocolDevices.add(deviceId);
      }
      final event = LitchControlEvent(
        requestId: requestId,
        action: action,
        owner: owner,
        phase: applied ? LitchControlPhase.applied : LitchControlPhase.rejected,
        reason: reason,
      );
      _emitControlEvent(event);
      return event;
    } on TimeoutException {
      ++_controlTimeouts;
      final timeout = LitchControlEvent(
        requestId: requestId,
        action: action,
        owner: owner,
        phase: LitchControlPhase.timeout,
      );
      _emitControlEvent(timeout);
      return timeout;
    } catch (_) {
      final failed = LitchControlEvent(
        requestId: requestId,
        action: action,
        owner: owner,
        phase:
            generation == _connectionGeneration &&
                isConnected &&
                connectedId == deviceId
            ? LitchControlPhase.sendFailed
            : LitchControlPhase.interrupted,
      );
      _emitControlEvent(failed);
      return failed;
    } finally {
      _requestIds.release(requestId);
    }
  }

  Future<LitchControlEvent> _unsentControlAction({
    required LitchControlAction action,
    required LitchControlOwner? owner,
    required LitchControlPhase phase,
  }) {
    final LitchControlEvent event = LitchControlEvent(
      action: action,
      owner: owner,
      phase: phase,
    );
    _emitControlEvent(event);
    return Future<LitchControlEvent>.value(event);
  }

  bool _isValidControlRequest({
    required LitchControlAction action,
    required LitchControlOwner? owner,
    required bool? enabled,
    required int undoCount,
  }) {
    return switch (action) {
      LitchControlAction.hint || LitchControlAction.newGame =>
        owner == null && enabled == null && undoCount == 1,
      LitchControlAction.undo =>
        owner == null && enabled == null && undoCount >= 1 && undoCount <= 0xff,
      LitchControlAction.coaching => owner != null && undoCount == 1,
    };
  }

  Future<List<LitchSetting>> readSettings(String deviceId) async {
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration) return const <LitchSetting>[];
    if (!capabilities.supportsSettings || capabilities.recoveryOnly) {
      _settingsAuthorityByDevice[deviceId] =
          LitchSettingsReadAuthority.unavailable;
      return const <LitchSetting>[];
    }
    final int sessionGeneration = _settingsSessionGeneration;
    final _SettingsEnumerationResult result = await _readSettingsWithAuthority(
      deviceId,
    );
    if (_isCurrentSettingsSession(deviceId, sessionGeneration)) {
      _settingsAuthorityByDevice[deviceId] = switch (result.authority) {
        _SettingsEnumerationAuthority.confirmed =>
          LitchSettingsReadAuthority.confirmed,
        _SettingsEnumerationAuthority.exhausted ||
        _SettingsEnumerationAuthority.sessionChanged =>
          LitchSettingsReadAuthority.unavailable,
      };
    }
    return _withTypedSettingDescriptors(result.settings);
  }

  Future<_SettingsEnumerationResult> _readSettingsWithAuthority(
    String deviceId,
  ) async {
    final generation = _settingsSessionGeneration;
    final capabilities = deviceCapabilities(deviceId);
    final descriptors = _settingsDescriptors;
    if (_isCurrentSettingsSession(deviceId, generation) &&
        hasBoardStateSnapshot &&
        descriptors != null &&
        identical(_settingsDescriptorCapabilities, capabilities)) {
      // Enumeration describes controls; board_state already supplies their
      // current values. Reopening Setup must not download the same immutable
      // JSON metadata through dozens of sequential characteristic reads.
      return _SettingsEnumerationResult(
        descriptors.map(_rebaseSettingOnLatestValue).toList(),
        _SettingsEnumerationAuthority.confirmed,
      );
    }
    final Future<_SettingsEnumerationResult>? current =
        _settingsEnumerationInFlight;
    if (current != null && _settingsEnumerationDeviceId == deviceId) {
      return current;
    }

    final Future<_SettingsEnumerationResult> read = _readSettings(deviceId);
    _settingsEnumerationInFlight = read;
    _settingsEnumerationDeviceId = deviceId;
    try {
      final result = await read;
      if (_isCurrentSettingsSession(deviceId, generation) &&
          identical(capabilities, deviceCapabilities(deviceId)) &&
          result.authority == _SettingsEnumerationAuthority.confirmed &&
          result.settings.isNotEmpty) {
        _settingsDescriptors = List.unmodifiable(result.settings);
        _settingsDescriptorCapabilities = capabilities;
      }
      return result;
    } finally {
      if (identical(_settingsEnumerationInFlight, read)) {
        _settingsEnumerationInFlight = null;
        _settingsEnumerationDeviceId = null;
      }
    }
  }

  Future<_SettingsEnumerationResult> _readSettings(String deviceId) async {
    final int sessionGeneration = _settingsSessionGeneration;
    final int notificationVersion = _settingNotificationVersion;
    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.settingsService,
      characteristicId: BleProtocol.settingsEnumerateCharacteristic,
      deviceId: deviceId,
    );

    try {
      final Map<String, LitchSetting> settingsByKey = <String, LitchSetting>{};
      final Map<int, String> keysByIndex = <int, String>{};
      bool complete = false;

      for (int attempt = 0; attempt < 2 && !complete; ++attempt) {
        settingsByKey.clear();
        keysByIndex.clear();
        int expectedIndex = 0;
        bool boundaryReached = false;

        try {
          for (int readIndex = 0; readIndex < 64; ++readIndex) {
            if (!_isCurrentSettingsSession(deviceId, sessionGeneration)) {
              throw StateError('settings session changed during enumeration');
            }

            final List<int> raw = await _readCharacteristic(characteristic);
            final Object? decoded = jsonDecode(utf8.decode(raw));

            if (decoded is List<Object?>) {
              _appendSettingsFromList(decoded, settingsByKey, keysByIndex);
              complete = true;
              boundaryReached = true;
              break;
            }

            if (decoded is! Map<String, Object?>) {
              throw const FormatException('invalid settings enumeration');
            }

            final Object? chunkItems = decoded['it'] ?? decoded['items'];
            final Object? doneToken = decoded['dn'] ?? decoded['done'];
            final int? nextIndex = _toInt(decoded['nx'] ?? decoded['next']);
            if (chunkItems is! List<Object?> ||
                doneToken is! bool ||
                nextIndex == null) {
              throw const FormatException('invalid settings chunk');
            }

            boundaryReached = doneToken;
            final int? indexSpan = _contiguousSettingsChunkSpan(
              chunkItems,
              expectedIndex,
            );
            if (indexSpan == null ||
                (doneToken && nextIndex != 0) ||
                (!doneToken && indexSpan == 0)) {
              throw const FormatException('non-contiguous settings chunk');
            }

            _appendSettingsFromList(chunkItems, settingsByKey, keysByIndex);
            expectedIndex += indexSpan;
            if (doneToken) {
              complete = true;
              break;
            }
          }
        } catch (_) {}

        if (complete ||
            attempt > 0 ||
            !_isCurrentSettingsSession(deviceId, sessionGeneration)) {
          break;
        }

        if (!boundaryReached &&
            !await _drainSettingsEnumerationToBoundary(
              characteristic,
              deviceId,
              sessionGeneration,
            )) {
          break;
        }
      }

      if (!complete) {
        throw const FormatException(
          'settings enumeration did not reach a complete boundary',
        );
      }

      // Live palette State owns its five values even before the first complete
      // Settings snapshot. Preserve those and any notifications received
      // during the enumeration in both the descriptors and the cache.
      final List<LitchSetting> settings = settingsByKey.values.map((setting) {
        final livePaletteKey =
            _connectionPlayerPalette?.values.containsKey(setting.key) ?? false;
        final notified =
            (_settingNotificationVersions[setting.key] ?? 0) >
            notificationVersion;
        return livePaletteKey || notified
            ? _rebaseSettingOnLatestValue(setting)
            : setting;
      }).toList();
      if (!_isCurrentSettingsSession(deviceId, sessionGeneration)) {
        return _SettingsEnumerationResult(
          hasBoardStateSnapshot && connectedId == deviceId
              ? settings.map(_rebaseSettingOnLatestValue).toList()
              : const <LitchSetting>[],
          _SettingsEnumerationAuthority.sessionChanged,
        );
      }
      if (hasBoardStateSnapshot) {
        return _SettingsEnumerationResult(
          settings.map(_rebaseSettingOnLatestValue).toList(),
          _SettingsEnumerationAuthority.confirmed,
        );
      }
      if (settings.isEmpty) {
        return const _SettingsEnumerationResult(
          <LitchSetting>[],
          _SettingsEnumerationAuthority.exhausted,
        );
      }
      _settingsKeysByIndex
        ..clear()
        ..addAll(keysByIndex);
      final Set<String> changedPlayerSettings = <String>{};
      for (final LitchSetting setting in settings) {
        final Object? previous = _latestSettingValues[setting.key];
        _latestSettingValues[setting.key] = setting.value;
        if ((_playerColorSettingKeys.contains(setting.key) ||
                _cpuRoleSettingKeys.contains(setting.key)) &&
            previous != setting.value) {
          changedPlayerSettings.add(setting.key);
        }
      }
      _replayPendingSettingsNotifyPayloads(deviceId);
      for (final String key in changedPlayerSettings) {
        _emitSettingsChanged(key);
      }
      return _SettingsEnumerationResult(
        settings,
        _SettingsEnumerationAuthority.confirmed,
      );
    } catch (_) {
      if (_isCurrentSettingsSession(deviceId, sessionGeneration)) {
        _logSettingsDiagThrottled(
          token: 'enumerate_failed:$deviceId',
          line:
              'settings enumeration unavailable; preserving the last confirmed map',
        );
      }
      return _SettingsEnumerationResult(
        const <LitchSetting>[],
        _isCurrentSettingsSession(deviceId, sessionGeneration)
            ? _SettingsEnumerationAuthority.exhausted
            : _SettingsEnumerationAuthority.sessionChanged,
      );
    }
  }

  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    final String trimmedKey = key.trim();
    if (trimmedKey.isEmpty) return null;
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return null;
    }
    final int sessionGeneration = _settingsSessionGeneration;
    final observation = _SettingReadObservation();
    _settingReadDiagnostics.started();
    try {
      return await _readSettingValueObserved(deviceId, trimmedKey, observation);
    } finally {
      observation.elapsed.stop();
      _settingReadDiagnostics.completed(
        _disposed || !_isCurrentSettingsSession(deviceId, sessionGeneration)
            ? SettingReadOutcome.cancelled
            : observation.outcome,
        observation.elapsed.elapsed,
      );
    }
  }

  Future<Object?> _readSettingValueObserved(
    String deviceId,
    String trimmedKey,
    _SettingReadObservation observation,
  ) async {
    final int sessionGeneration = _settingsSessionGeneration;
    if (hasBoardStateSnapshot && connectedId == deviceId) {
      if (_latestSettingValues[trimmedKey] != null) {
        observation.outcome = SettingReadOutcome.succeeded;
      }
      return _latestSettingValues[trimmedKey];
    }
    if (!_ensureSettingsResponseBridge(deviceId)) {
      return _readSettingValueViaEnumerate(deviceId, trimmedKey, observation);
    }

    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.settingsService,
      characteristicId: BleProtocol.settingsReadWriteCharacteristic,
      deviceId: deviceId,
    );

    final int? requestId = _requestIds.reserve();
    if (requestId == null) {
      return _readSettingValueViaEnumerate(deviceId, trimmedKey, observation);
    }
    _replacePendingSettingForKey(trimmedKey);
    final Completer<Object?> completer = Completer<Object?>();
    final _PendingSettingTransaction pending = _PendingSettingTransaction(
      requestId: requestId,
      deviceId: deviceId,
      sessionGeneration: _settingsSessionGeneration,
      key: trimmedKey,
      kind: _PendingSettingKind.read,
      requiresExactCorrelation: true,
      completer: completer,
      readObservation: observation,
    );
    _pendingSettingsById[requestId] = pending;
    _pendingSettingIdsByKey[trimmedKey] = requestId;

    final Map<String, Object> payload = <String, Object>{
      'k': trimmedKey,
      'i': requestId,
    };
    final List<int> encoded = utf8.encode(jsonEncode(payload));

    observation.directAttempted = true;
    _settingReadDiagnostics.directAttempted();
    try {
      await _writeCharacteristicWithResponse(characteristic, value: encoded);
    } catch (_) {
      if (identical(_pendingSettingsById[requestId], pending)) {
        _completePendingSetting(requestId, null);
      }
      _logSettingsDiagThrottled(
        token: 'read_fallback_write:$trimmedKey',
        line:
            'settings read fallback: direct read write failed for key=$trimmedKey; using enumerate',
      );
      if (!_isCurrentSettingsSession(deviceId, sessionGeneration)) return null;
      return _readSettingValueViaEnumerate(deviceId, trimmedKey, observation);
    }

    final Object? directValue = await completer.future.timeout(
      const Duration(milliseconds: 180),
      onTimeout: () {
        if (identical(_pendingSettingsById[requestId], pending)) {
          _completePendingSetting(requestId, null);
        }
        _logSettingsDiagThrottled(
          token: 'read_fallback_timeout:$trimmedKey',
          line:
              'settings read fallback: direct read timeout for key=$trimmedKey; using enumerate',
        );
        return null;
      },
    );

    if (!_isCurrentSettingsSession(deviceId, sessionGeneration)) return null;
    if (hasBoardStateSnapshot && connectedId == deviceId) {
      if (_latestSettingValues[trimmedKey] != null) {
        observation.outcome = SettingReadOutcome.succeeded;
      }
      return _latestSettingValues[trimmedKey];
    }
    if (directValue != null) {
      observation.outcome = SettingReadOutcome.succeeded;
      return directValue;
    }

    return _readSettingValueViaEnumerate(deviceId, trimmedKey, observation);
  }

  Future<LitchEndPalette?> readPhysicalEndPalette({
    required String deviceId,
  }) async {
    final generation = _settingsSessionGeneration;
    bool hasCurrentPalette() =>
        hasBoardStateSnapshot || _connectionPlayerPalette != null;
    LitchEndPalette? currentPalette() {
      final p1 = _pieceWheelValue(latestSettingValue(_p1EndPieceWheelKey));
      final p2 = _pieceWheelValue(latestSettingValue(_p2EndPieceWheelKey));
      return p1 == null || p2 == null
          ? null
          : LitchEndPalette(
              p1ControlsWheel: p1,
              p2ControlsWheel: p2,
              usesDirectPhysicalKeys: true,
            );
    }

    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    if (hasCurrentPalette()) return currentPalette();
    final palette = await _readPhysicalEndPaletteValues(deviceId, generation);
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    return hasCurrentPalette() ? currentPalette() : palette;
  }

  Future<LitchEndPalette?> _readPhysicalEndPaletteValues(
    String deviceId,
    int generation,
  ) async {
    final int? directP1 = _pieceWheelValue(
      await readSettingValue(deviceId: deviceId, key: _p1EndPieceWheelKey),
    );
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    if (directP1 != null) {
      final int? directP2 = _pieceWheelValue(
        await readSettingValue(deviceId: deviceId, key: _p2EndPieceWheelKey),
      );
      if (!_isCurrentSettingsSession(deviceId, generation)) return null;
      if (directP2 != null) {
        return LitchEndPalette(
          p1ControlsWheel: directP1,
          p2ControlsWheel: directP2,
          usesDirectPhysicalKeys: true,
        );
      }
    }

    final int? logicalP1 = _pieceWheelValue(
      latestSettingValue(_p1PieceWheelKey) ??
          await readSettingValue(deviceId: deviceId, key: _p1PieceWheelKey),
    );
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    final int? logicalP2 = _pieceWheelValue(
      latestSettingValue(_p2PieceWheelKey) ??
          await readSettingValue(deviceId: deviceId, key: _p2PieceWheelKey),
    );
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    if (logicalP1 == null || logicalP2 == null) {
      return null;
    }
    return LitchEndPalette(
      p1ControlsWheel: logicalP1,
      p2ControlsWheel: logicalP2,
      usesDirectPhysicalKeys: false,
    );
  }

  Future<Object?> _readSettingValueViaEnumerate(
    String deviceId,
    String key,
    _SettingReadObservation observation,
  ) async {
    final generation = _settingsSessionGeneration;
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    final bool retry =
        observation.directAttempted && !observation.optionalMissing;
    _settingReadDiagnostics.fallbackEntered(retry: retry);
    void recordConfirmed(Object? value) {
      if (value != null) {
        observation.outcome = retry
            ? SettingReadOutcome.recovered
            : SettingReadOutcome.succeeded;
      }
    }

    if (!hasBoardStateSnapshot &&
        _consoleCurrentLine.toString().startsWith('{"event":"board_state"')) {
      final receipt = _pendingBoardStateReceipt ??= Completer<void>();
      await receipt.future.timeout(
        const Duration(seconds: 1),
        onTimeout: () {},
      );
      if (!_isCurrentSettingsSession(deviceId, generation)) return null;
      if (hasBoardStateSnapshot && connectedId == deviceId) {
        recordConfirmed(_latestSettingValues[key]);
        return _latestSettingValues[key];
      }
    }
    final enumeration = await _readSettingsWithAuthority(deviceId);
    final List<LitchSetting> settings = enumeration.settings;
    if (!_isCurrentSettingsSession(deviceId, generation)) return null;
    if (hasBoardStateSnapshot && connectedId == deviceId) {
      recordConfirmed(_latestSettingValues[key]);
      return _latestSettingValues[key];
    }
    if (enumeration.authority == _SettingsEnumerationAuthority.exhausted &&
        !observation.optionalMissing) {
      observation.outcome = SettingReadOutcome.exhausted;
    }
    for (final LitchSetting setting in settings) {
      if (setting.key == key) {
        if (enumeration.authority == _SettingsEnumerationAuthority.confirmed) {
          recordConfirmed(setting.value);
        }
        return setting.value;
      }
    }
    return null;
  }

  int? _contiguousSettingsChunkSpan(List<Object?> source, int expectedIndex) {
    var cursor = expectedIndex;
    for (final item in source) {
      while (SettingsStateWireCodec.retiredVisibleIndices.contains(cursor)) {
        cursor += 1;
      }
      if (item is! Map<String, Object?> ||
          _toInt(item['i'] ?? item['index']) != cursor) {
        return null;
      }
      cursor += 1;
    }
    return cursor - expectedIndex;
  }

  Future<bool> _drainSettingsEnumerationToBoundary(
    QualifiedCharacteristic characteristic,
    String deviceId,
    int sessionGeneration,
  ) async {
    try {
      for (int readIndex = 0; readIndex < 64; ++readIndex) {
        if (!_isCurrentSettingsSession(deviceId, sessionGeneration)) {
          return false;
        }
        final List<int> raw = await _readCharacteristic(characteristic);
        final Object? decoded = jsonDecode(utf8.decode(raw));
        if (decoded is List<Object?>) {
          return true;
        }
        if (decoded is Map<String, Object?> &&
            (decoded['dn'] ?? decoded['done']) == true) {
          return true;
        }
      }
    } catch (_) {
      return false;
    }
    return false;
  }

  void _appendSettingsFromList(
    List<Object?> source,
    Map<String, LitchSetting> out,
    Map<int, String> outKeysByIndex,
  ) {
    for (final Object? item in source) {
      if (item is! Map<String, Object?>) {
        continue;
      }

      final String key = (item['k'] ?? item['key'] ?? '').toString();
      if (key.isEmpty) {
        continue;
      }
      final int? settingIndex = _toInt(item['i'] ?? item['index']);
      if (settingIndex != null && settingIndex >= 0) {
        outKeysByIndex[settingIndex] = key;
      }

      if (key == 'BLE_ENABLED') {
        continue;
      }

      final String type = (item['t'] ?? item['type'] ?? '').toString();
      final String category = (item['c'] ?? item['category'] ?? 'internal')
          .toString();
      final Object value = _parseValueByType(type, item['v'] ?? item['value']);
      final Object defaultValue = _parseValueByType(
        type,
        item['df'] ?? item['default'],
      );
      final int? min = _toInt(item['mn'] ?? item['min']);
      final int? max = _toInt(item['mx'] ?? item['max']);
      final List<String> options = _toStringList(
        item['opts'] ?? item['options'],
      );
      final String description = (item['d'] ?? item['description'] ?? '')
          .toString();

      out[key] = LitchSetting(
        key: key,
        type: type,
        category: category,
        value: value,
        defaultValue: defaultValue,
        min: min,
        max: max,
        options: options,
        description: description,
      );
    }
  }

  Future<bool> sendConsoleLine(String deviceId, String line) =>
      _sendConsoleLine(deviceId, line);

  Future<bool> _sendConsoleLine(
    String deviceId,
    String line, {
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
  }) async {
    supportScope?.check();
    final String trimmed = line.trimRight();
    // A blank submitted line is Enter for interactive console commands.
    if (!isConnected || connectedId != deviceId) {
      return false;
    }
    final generation = _connectionGeneration;
    LitchDeviceCapabilities capabilities;
    try {
      capabilities = await _connectionCapabilitiesForDomain(deviceId);
    } on OwnerPairingNotReady {
      return false;
    }
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return false;
    }
    ConnectionSession? connectionSession = _activeConnectionSession;
    final requiresSharedSession =
        connectionSession != null ||
        _openingConnectionSession != null ||
        capabilities.supportsConnectionSession;
    if (connectionSession?.isActivated != true && requiresSharedSession) {
      try {
        connectionSession = await openConnectionSession(deviceId);
      } catch (_) {
        connectionSession = null;
      }
    }
    if (generation != _connectionGeneration) return false;
    if (connectionSession != null && connectionSession.isActivated) {
      try {
        final bool sent = await _sendConnectionConsoleRecord(
          deviceId,
          connectionSession,
          utf8.encode('$trimmed\n'),
          isCurrent: isCurrent,
        );
        if (!sent) return false;
        _applyConsoleCommandState(trimmed);
        return true;
      } catch (_) {
        if (!(isCurrent?.call() ?? true)) return false;
        _pushAppLog('Failed to send command.');
        return false;
      }
    }

    _pushAppLog('Failed to send command.');
    return false;
  }

  /// Enables or disables high-volume TRACE output for this connection.
  /// Showing the developer Console alone never changes the firmware log level.
  Future<bool> setConsoleTraceOutputEnabled(String deviceId, bool enabled) {
    if (_consoleTraceOutputEnabled == enabled) {
      return Future<bool>.value(true);
    }
    return sendConsoleLine(deviceId, enabled ? 'debug trace' : 'debug info');
  }

  void _applyConsoleCommandState(String command) {
    final List<String> parts = command.trim().split(RegExp(r'\s+'));
    if (parts.length != 2 || parts.first != 'debug') {
      return;
    }

    const Set<String> levels = <String>{
      'trace',
      'debug',
      'info',
      'warn',
      'error',
    };
    if (!levels.contains(parts[1])) {
      return;
    }
    _setConsoleTraceOutputEnabled(parts[1] == 'trace');
  }

  void _setConsoleTraceOutputEnabled(bool enabled) {
    if (_consoleTraceOutputEnabled == enabled) {
      return;
    }

    _consoleTraceOutputEnabled = enabled;
    if (!_consoleTraceOutputController.isClosed) {
      _consoleTraceOutputController.add(enabled);
    }
    if (!enabled) {
      final int previousLength = _consoleLines.length;
      _consoleLines.removeWhere(_isRetainedTraceConsoleLine);
      if (_consoleLines.length != previousLength) {
        _emitConsoleSnapshot();
      }
    }
  }

  Future<void> sendConsoleBytes(String deviceId, List<int> bytes) async {
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration ||
        !capabilities.supportsConnectionSession ||
        capabilities.recoveryOnly) {
      return;
    }
    if (bytes.isEmpty) {
      return;
    }

    ConnectionSession? connectionSession = _activeConnectionSession;
    final requiresSharedSession =
        connectionSession != null ||
        _openingConnectionSession != null ||
        capabilities.supportsConnectionSession;
    if (connectionSession?.isActivated != true && requiresSharedSession) {
      try {
        connectionSession = await openConnectionSession(deviceId);
      } catch (_) {
        connectionSession = null;
      }
    }
    if (generation != _connectionGeneration) return;
    if (connectionSession != null && connectionSession.isActivated) {
      final bool sent = await _sendConnectionConsoleRecord(
        deviceId,
        connectionSession,
        bytes,
      );
      if (!sent) {
        throw StateError('Connection-session Console record was rejected');
      }
      return;
    }

    throw StateError('Connection-session Console is unavailable');
  }

  Future<bool> _sendConnectionConsoleRecord(
    String deviceId,
    ConnectionSession session,
    List<int> bytes, {
    bool Function()? isCurrent,
  }) {
    final operation = _connectionConsoleTail.then(
      (_) => _sendConnectionConsoleRecordNow(
        deviceId,
        session,
        bytes,
        isCurrent: isCurrent,
      ),
    );
    _connectionConsoleTail = operation.then<void>(
      (_) {},
      onError: (Object _, StackTrace _) {},
    );
    return operation;
  }

  Future<bool> _sendConnectionConsoleRecordNow(
    String deviceId,
    ConnectionSession session,
    List<int> bytes, {
    bool Function()? isCurrent,
  }) async {
    final int generation = _connectionGeneration;
    bool current() =>
        generation == _connectionGeneration &&
        identical(session, _activeConnectionSession) &&
        isConnected &&
        connectedId == deviceId &&
        (isCurrent?.call() ?? true);
    if (!current()) return false;
    final records = LitchControlProtocol.encodeConnectionConsoleRecord(bytes);
    for (final body in records) {
      final outcome = await session.sendOrdered(
        LitchControlProtocol.connectionConsoleOpcode,
        body,
      );
      if (!current() ||
          outcome.disposition != ConnectionSessionDisposition.applied) {
        return false;
      }
    }
    return true;
  }

  /// Writes through the current Ordered owner. Callers needing a rejection
  /// reason use [writeSettingAcknowledged].
  Future<bool> writeSetting({
    required String deviceId,
    required String key,
    required Object value,
    bool awaitAck = false,
  }) async {
    final supportAction = _recordSettingWriteStarted(key);
    final LitchSettingWriteResult? result = await _writeSettingTransaction(
      deviceId: deviceId,
      key: key,
      value: value,
      awaitAck: awaitAck,
    );
    _recordSettingWriteCompleted(result, supportAction);
    return switch (result) {
      null => true,
      LitchSettingWriteAcknowledged(:final outcome) => outcome.ok,
      LitchSettingWriteNotAcknowledged() => false,
    };
  }

  /// Writes a setting and returns its exact correlated acknowledgement or the
  /// local reason that no valid acknowledgement was available.
  Future<LitchSettingWriteResult> writeSettingAcknowledged({
    required String deviceId,
    required String key,
    required Object value,
  }) async {
    final supportAction = _recordSettingWriteStarted(key, userAction: true);
    final LitchSettingWriteResult result =
        (await _writeSettingTransaction(
          deviceId: deviceId,
          key: key,
          value: value,
          awaitAck: true,
        )) ??
        const LitchSettingWriteNotAcknowledged(
          LitchSettingWriteFailure.timedOut,
        );
    _recordSettingWriteCompleted(result, supportAction);
    return result;
  }

  int _recordSettingWriteStarted(String key, {bool userAction = false}) {
    ++_settingsWriteAttempts;
    final String boundedKey = key.length <= 64 ? key : key.substring(0, 64);
    _lastSettingsOperation = 'setting_write:$boundedKey';
    // Native preview writes (for example a held brightness slider) retain
    // their existing counters but are not separate human actions.
    return userAction
        ? _appTrace.beginUserAction(SupportActionKind.settingChange)
        : 0;
  }

  void _recordSettingWriteCompleted(
    LitchSettingWriteResult? result,
    int supportAction,
  ) {
    _appTrace.finishUserAction(supportAction, switch (result) {
      null => SupportActionOutcome.accepted,
      LitchSettingWriteAcknowledged(:final outcome) =>
        outcome.ok
            ? SupportActionOutcome.applied
            : SupportActionOutcome.rejected,
      LitchSettingWriteNotAcknowledged() => SupportActionOutcome.unconfirmed,
    });
    final String? failureToken = switch (result) {
      null => null,
      LitchSettingWriteAcknowledged(:final outcome) =>
        outcome.ok ? null : 'board:${outcome.reason.wireToken}',
      LitchSettingWriteNotAcknowledged(:final failure) =>
        'app:${_settingWriteFailureToken(failure)}',
    };
    if (failureToken == null) {
      ++_settingsWriteSucceeded;
      return;
    }
    ++_settingsWriteFailed;
    _lastSettingsFailureToken = failureToken;
  }

  String _settingWriteFailureToken(LitchSettingWriteFailure failure) {
    return switch (failure) {
      LitchSettingWriteFailure.bridgeUnavailable => 'bridge_unavailable',
      LitchSettingWriteFailure.requestIdExhausted => 'request_id_exhausted',
      LitchSettingWriteFailure.sendFailed => 'send_failed',
      LitchSettingWriteFailure.timedOut => 'timed_out',
      LitchSettingWriteFailure.disconnected => 'disconnected',
      LitchSettingWriteFailure.connectionReplaced => 'connection_replaced',
      LitchSettingWriteFailure.requestReplaced => 'request_replaced',
      LitchSettingWriteFailure.malformedAcknowledgement =>
        'malformed_acknowledgement',
      LitchSettingWriteFailure.interrupted => 'interrupted',
    };
  }

  /// Owns the single setting-write transport and acknowledgement transaction.
  /// A null result is a successfully sent write that did not request an ack.
  Future<LitchSettingWriteResult?> _writeSettingTransaction({
    required String deviceId,
    required String key,
    required Object value,
    required bool awaitAck,
  }) async {
    if (!isConnected || connectedId != deviceId) {
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.connectionReplaced,
      );
    }
    final generation = _connectionGeneration;
    final capabilities = await _connectionCapabilitiesForDomain(deviceId);
    if (generation != _connectionGeneration) {
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.connectionReplaced,
      );
    }
    if (!capabilities.supportsSettings || capabilities.recoveryOnly) {
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.bridgeUnavailable,
      );
    }
    final connectionSession = await openConnectionSession(deviceId);
    if (generation != _connectionGeneration ||
        !isConnected ||
        connectedId != deviceId ||
        !identical(connectionSession, _activeConnectionSession)) {
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.connectionReplaced,
      );
    }
    if (SettingsStateWireCodec.extensionKeys.contains(key) &&
        !_latestSettingValues.containsKey(key)) {
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.bridgeUnavailable,
      );
    }
    final connectionBody = SettingsStateWireCodec.tryEncodeMutation(key, value);
    if (connectionSession != null &&
        connectionSession.isActivated &&
        connectionBody != null) {
      const near = 'P1_END_PIECE_WHEEL';
      const far = 'P2_END_PIECE_WHEEL';
      final ends = switch (key) {
        'P1_PIECE_WHEEL' || near => (edited: near, other: far),
        'P2_PIECE_WHEEL' || far => (edited: far, other: near),
        _ => null,
      };
      final palette = ends == null
          ? null
          : (
              edited: ends.edited,
              other: ends.other,
              otherAtSubmission:
                  _binarySettingsStateObserved ||
                      _connectionPlayerPalette != null
                  ? _latestSettingValues[ends.other]
                  : null,
            );
      try {
        final outcome = await connectionSession.sendOrdered(
          SettingsStateWireCodec.mutationOpcode,
          connectionBody,
        );
        if (generation != _connectionGeneration ||
            !isConnected ||
            connectedId != deviceId ||
            !identical(connectionSession, _activeConnectionSession)) {
          return const LitchSettingWriteNotAcknowledged(
            LitchSettingWriteFailure.connectionReplaced,
          );
        }
        final applied =
            outcome.disposition == ConnectionSessionDisposition.applied;
        // Preview callers omit state confirmation, not the known send result.
        if (!awaitAck && applied) return null;
        final reason = applied
            ? LitchMutationReason.none
            : LitchMutationReason.fromWireCode(outcome.reason) ??
                  LitchMutationReason.badRequest;
        // The compact Checkpoint carries no effective hue when firmware
        // separates the two ends. The coherent Settings readback owns it.
        final hasValue = applied && palette == null;
        return LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            key: key,
            ok: applied,
            reason: reason,
            hasValue: hasValue,
            value: hasValue ? value : null,
            isCompact: true,
          ),
          boardStateConfirmation: applied
              ? _confirmConnectionSetting(
                  connectionSession,
                  key,
                  value,
                  outcome.domainRevision,
                  palette,
                )
              : null,
        );
      } catch (_) {
        return const LitchSettingWriteNotAcknowledged(
          LitchSettingWriteFailure.interrupted,
        );
      }
    }
    return const LitchSettingWriteNotAcknowledged(
      LitchSettingWriteFailure.bridgeUnavailable,
    );
  }

  Future<bool> _confirmConnectionSetting(
    ConnectionSession session,
    String key,
    Object value,
    int revision,
    ({String edited, String other, Object? otherAtSubmission})? palette,
  ) async {
    bool confirmed(SettingsStateSnapshot snapshot, {bool fresh = false}) {
      final observedRevision = snapshot.settingsRevision;
      if (!identical(_activeConnectionSession, session) ||
          !session.isActivated ||
          (observedRevision != revision &&
              !isNewerHomeStateSequence(observedRevision, revision))) {
        return false;
      }
      final observed = snapshot.values[key];
      if (palette == null) return observed == value;
      // A live palette can lead its full snapshot. A late full report must
      // not confirm an end color already superseded in this session.
      final live = _connectionPlayerPalette?.settingsRevision;
      final base = _completeConnectionSettings?.settingsRevision;
      if ((live != null && isNewerHomeStateSequence(live, observedRevision)) ||
          (base != null && isNewerHomeStateSequence(base, observedRevision))) {
        return false;
      }
      final other = snapshot.values[palette.other];
      if (value is! int ||
          value < 0 ||
          value > 255 ||
          other is! int ||
          other < 0 ||
          other > 255 ||
          observed != snapshot.values[palette.edited] ||
          observed != LitchEndPalette.resolveWheel(value, other)) {
        return false;
      }
      if (observed == value) return true;
      // A cumulative Checkpoint can cover a later edit to the other end.
      // Even at its exact revision, do not reinterpret this request using
      // a different opposite hue. A Noop still needs a fresh full readback.
      return fresh && other == palette.otherAtSubmission;
    }

    final complete = _completeConnectionSettings;
    if (_binarySettingsStateObserved &&
        complete != null &&
        confirmed(complete)) {
      return true;
    }
    final assembler = SettingsStateWireAssembler();
    try {
      return await session.readStateSnapshot<bool>(
        accept: (packet) {
          final snapshot = assembler.accept(
            code: packet.code,
            revision: packet.sequence,
            body: packet.body,
          );
          return snapshot != null && confirmed(snapshot, fresh: true)
              ? true
              : null;
        },
      );
    } catch (_) {
      return false;
    }
  }

  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) =>
      _fetchBoardFirmwareVersion(deviceId: deviceId);

  Future<String?> _fetchBoardFirmwareVersion({
    required String deviceId,
    SupportCollectionScope? supportScope,
  }) async {
    final int generation = _connectionGeneration;
    bool current() =>
        !_disposed &&
        isConnected &&
        connectedId == deviceId &&
        generation == _connectionGeneration;
    supportScope?.check();
    if (!isConnected || connectedId != deviceId) {
      _pushAppLog('fw_version skipped: board is not connected.');
      return null;
    }

    // The retained firmware page can load while first claim or quiet owner
    // reconnect is still pending. Join the same readiness owner as Settings;
    // an intentionally blocked identity read says nothing about compatibility.
    final LitchDeviceCapabilities capabilities;
    try {
      final ready = _connectionCapabilitiesForDomain(deviceId);
      capabilities = await (supportScope?.wait(ready) ?? ready);
    } on OwnerPairingNotReady {
      return null;
    }
    supportScope?.check();
    if (!current() || !capabilities.supportsConnectionSession) return null;

    _pushAppLog('Requesting board firmware version...');
    await (supportScope == null
        ? _ensureLinkCalibrated(deviceId)
        : supportScope.native(() => _ensureLinkCalibrated(deviceId)));
    supportScope?.check();
    if (!current()) return null;
    final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
      deviceId,
    );

    for (int attempt = 1; attempt <= 3; ++attempt) {
      supportScope?.check();
      if (!current()) return null;
      final Future<OtaStatusEvent?> response = channel.waitFor(
        (OtaStatusEvent event) =>
            event.kind == OtaStatusKind.version ||
            event.kind == OtaStatusKind.error,
        timeout: const Duration(seconds: 5),
        cancelled: supportScope?.stopped,
      );
      final bool sent = await _sendOtaCommandForChannel(
        deviceId,
        channel,
        <String, Object>{'cmd': 'fw_version'},
        attempts: 10,
        delay: const Duration(milliseconds: 250),
        supportScope: supportScope,
        isCurrent: current,
      );
      if (!sent) {
        _pushAppLog('fw_version command send failed.');
        continue;
      }

      final OtaStatusEvent? event = await response;
      supportScope?.check();
      if (!current()) return null;
      if (event?.kind == OtaStatusKind.version && event!.detail.isNotEmpty) {
        _pushAppLog('Board firmware version: ${event.detail}');
        return event.detail;
      }
      if (event?.kind == OtaStatusKind.error &&
          event?.detail == 'unknown_cmd') {
        _pushAppLog('fw_version not supported by board.');
        break;
      }
      if (attempt < 3) {
        _pushAppLog('fw_version retrying (attempt $attempt/3).');
        final pause = Future<void>.delayed(const Duration(milliseconds: 350));
        await (supportScope?.wait(pause) ?? pause);
      }
    }

    supportScope?.check();
    if (!current()) return null;
    _pushAppLog('Board firmware version unavailable on the current session.');
    return null;
  }

  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) => _fetchDeviceCapabilities(deviceId: deviceId, refresh: refresh);

  Future<LitchDeviceCapabilities> _fetchSupportCapabilities(
    String deviceId,
    SupportCollectionScope scope,
  ) {
    scope.check();
    // Join existing connection-owned discovery; never create a second query.
    final existing = _capabilityDiscoveryByDevice[deviceId];
    if (existing != null) return scope.native(() => existing);
    return _fetchDeviceCapabilities(deviceId: deviceId, supportScope: scope);
  }

  Future<LitchDeviceCapabilities> _fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
    SupportCollectionScope? supportScope,
  }) {
    // Join the connection-owned query. Capability authority is immutable until
    // that connection retires; reprobing an activated, owner-blocked session
    // would turn a harmless page refresh into a private Object request.
    final Future<LitchDeviceCapabilities>? inFlight =
        _capabilityDiscoveryByDevice[deviceId];
    if (inFlight != null) {
      return inFlight;
    }
    final LitchDeviceCapabilities? cached = _capabilitiesByDevice[deviceId];
    if (cached != null &&
        (!refresh ||
            (isConnected &&
                connectedId == deviceId &&
                cached.supportsConnectionSession))) {
      return Future<LitchDeviceCapabilities>.value(cached);
    }

    final Future<LitchDeviceCapabilities> discovery =
        _discoverDeviceCapabilities(deviceId, supportScope: supportScope);
    _capabilityDiscoveryByDevice[deviceId] = discovery;
    return discovery.whenComplete(() {
      if (identical(_capabilityDiscoveryByDevice[deviceId], discovery)) {
        _capabilityDiscoveryByDevice.remove(deviceId);
      }
    });
  }

  Future<LitchDeviceCapabilities> _discoverDeviceCapabilities(
    String deviceId, {
    SupportCollectionScope? supportScope,
  }) async {
    final int generation = _connectionGeneration;
    bool current() =>
        !_disposed &&
        isConnected &&
        connectedId == deviceId &&
        generation == _connectionGeneration;
    supportScope?.check();
    if (!isConnected || connectedId != deviceId) {
      return LitchDeviceCapabilities.unavailable;
    }

    try {
      final inventory = await _gattSessionFor(deviceId);
      if (inventory is LitchGattInventory &&
          !(inventory as LitchGattInventory).hasCharacteristic(
            BleProtocol.baseService,
            BleProtocol.otaControlCharacteristic,
          )) {
        throw const LitchGattDiscoveryIncomplete(
          'board is missing capability discovery',
        );
      }
      await (supportScope == null
          ? _ensureLinkCalibrated(deviceId)
          : supportScope.native(() => _ensureLinkCalibrated(deviceId)));
      supportScope?.check();
      if (!current()) return LitchDeviceCapabilities.unavailable;
      final LitchOtaControlChannel activeChannel =
          await _ensureOtaControlChannel(deviceId);
      // Dart subscription installation does not acknowledge native iOS CCCD
      // readiness. Retry the idempotent exchange, not only failed writes: the
      // first write may succeed while its response still has no listener.
      // Keep one discovery/adoption future and one native notification owner.
      for (int attempt = 1; attempt <= _capabilityProbeAttempts; ++attempt) {
        supportScope?.check();
        if (!current() || !_isActiveOtaChannel(deviceId, activeChannel)) {
          return LitchDeviceCapabilities.unavailable;
        }
        _trace(
          'ble.capability_probe',
          fields: <String, Object?>{
            'generation': generation,
            'attempt': attempt,
          },
        );
        final Future<OtaStatusEvent?> response = activeChannel.waitFor(
          (OtaStatusEvent event) =>
              event.kind == OtaStatusKind.deviceInfo ||
              (event.kind == OtaStatusKind.error &&
                  event.detail == 'unknown_cmd'),
          timeout: _capabilityProbeTimeout,
          cancelled: supportScope?.stopped,
          onNotificationError: (error) {
            if (!current() || _repairingGattInventory) return;
            _trace(
              'ble.capability_result',
              fields: {
                'generation': generation,
                'attempt': attempt,
                'outcome': 'notification_failed',
              },
            );
            _retireConnectionPreparationFailure(
              deviceId,
              generation,
              stage: 'capability_notification',
              error: error,
            );
          },
        );
        final bool sent = await _sendOtaCommandForChannel(
          deviceId,
          activeChannel,
          <String, Object>{'cmd': 'info'},
          attempts: 1,
          delay: Duration.zero,
          allowPreSessionDirect: true,
          supportScope: supportScope,
          isCurrent: current,
        );
        // Drain this attempt even if its write failed, so its listener cannot
        // outlive the exchange and consume a later attempt's response.
        // A retired connection cannot own another attempt; native subscription
        // cancellation must not keep its callers waiting for a probe timeout.
        if (!current()) return LitchDeviceCapabilities.unavailable;
        final OtaStatusEvent? event = await _untilConnectionEnds(response);
        supportScope?.check();
        if (!current() || !_isActiveOtaChannel(deviceId, activeChannel)) {
          return LitchDeviceCapabilities.unavailable;
        }
        final LitchDeviceCapabilities? reported = event == null
            ? null
            : LitchDeviceCapabilities.fromPayload(event.payload);
        final bool refused = event?.detail == 'unknown_cmd';
        _trace(
          'ble.capability_result',
          fields: <String, Object?>{
            'generation': generation,
            'attempt': attempt,
            'outcome': reported != null
                ? 'reported'
                : refused
                ? 'unsupported_contract'
                : !sent
                ? 'send_failed'
                : event == null
                ? 'timeout'
                : 'malformed',
          },
        );
        if (refused) {
          throw const LitchGattDiscoveryIncomplete(
            'board refused current capability discovery',
          );
        }
        if (reported != null) {
          if (inventory is LitchGattInventory) {
            validateLitchGattCapabilities(
              inventory as LitchGattInventory,
              reported,
            );
            _gattRefreshAttempted.remove(deviceId);
          }
          _authoritativeCapabilities[deviceId] = reported;
          if (_authoritativeCapabilities.length > 32) {
            _authoritativeCapabilities.remove(
              _authoritativeCapabilities.keys.first,
            );
          }
          _capabilitiesByDevice[deviceId] = reported;
          _pushAppLog(
            'Board capabilities: ${reported.hardwareLabel}, '
            'app update=${reported.supportsFirmwareUpdate ? 'yes' : 'no'}.',
          );
          return reported;
        }
        if (attempt < _capabilityProbeAttempts) {
          final pause = Future<void>.delayed(_capabilityProbeRetryDelay);
          await (supportScope?.wait(pause) ?? pause);
        }
      }
      _trace(
        'ble.capability_exhausted',
        fields: <String, Object?>{
          'generation': generation,
          'attempts': _capabilityProbeAttempts,
        },
      );
      throw const LitchGattDiscoveryIncomplete(
        'board did not provide the current capability contract',
      );
    } on LitchGattDiscoveryIncomplete catch (error) {
      _pushAppLog('BLE service discovery failed: $error');
      if (current()) await _retireIncompleteGatt(deviceId);
      return LitchDeviceCapabilities.unavailable;
    } on SupportCollectionStopped {
      rethrow;
    } catch (_) {
      _trace(
        'ble.capability_unavailable',
        fields: <String, Object?>{'generation': generation},
      );
      if (current()) {
        _capabilitiesByDevice.putIfAbsent(
          deviceId,
          () => LitchDeviceCapabilities.unavailable,
        );
      }
      return LitchDeviceCapabilities.unavailable;
    }
  }

  /// Requests the board-owned snapshot only when the explicit capability bit
  /// advertises support. Missing discovery never implies health.
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async {
    final int generation = _connectionGeneration;
    RuntimeHealthResult result;
    try {
      result = await _fetchRuntimeHealth(deviceId: deviceId);
    } catch (_) {
      result = const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.sendFailed,
      );
    }
    if (!isConnected ||
        connectedId != deviceId ||
        generation != _connectionGeneration) {
      return const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.disconnected,
      );
    }
    return result;
  }

  Future<RuntimeHealthResult> _fetchRuntimeHealth({
    required String deviceId,
    SupportCollectionScope? supportScope,
  }) async {
    final int generation = _connectionGeneration;
    supportScope?.check();
    if (!isConnected || connectedId != deviceId) {
      return const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.disconnected,
      );
    }
    final LitchDeviceCapabilities capabilities = await (supportScope == null
        ? fetchDeviceCapabilities(deviceId: deviceId)
        : _fetchSupportCapabilities(deviceId, supportScope));
    supportScope?.check();
    if (!isConnected ||
        connectedId != deviceId ||
        generation != _connectionGeneration) {
      return const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.disconnected,
      );
    }
    if (!capabilities.supportsRuntimeHealth) {
      return const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.unsupported,
      );
    }

    final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
      deviceId,
    );
    if (!_isActiveOtaChannel(deviceId, channel)) {
      return const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.disconnected,
      );
    }
    RuntimeHealthUnavailableReason lastFailure =
        RuntimeHealthUnavailableReason.timedOut;
    for (int attempt = 0; attempt < 3; ++attempt) {
      supportScope?.check();
      final Future<OtaStatusEvent?> response = channel.waitFor(
        (OtaStatusEvent event) =>
            event.kind == OtaStatusKind.runtimeHealth ||
            event.kind == OtaStatusKind.runtimeHealthMalformed ||
            (event.kind == OtaStatusKind.error &&
                event.detail == 'unknown_cmd'),
        timeout: const Duration(milliseconds: 1200),
        cancelled: supportScope?.stopped,
      );
      final bool sent = await _sendOtaCommandForChannel(
        deviceId,
        channel,
        <String, Object>{'cmd': 'health'},
        attempts: 1,
        delay: Duration.zero,
        supportScope: supportScope,
      );
      final OtaStatusEvent? event = await response;
      supportScope?.check();
      if (!_isActiveOtaChannel(deviceId, channel)) {
        return const RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        );
      }
      if (!sent) {
        lastFailure = RuntimeHealthUnavailableReason.sendFailed;
      } else if (event == null) {
        lastFailure = RuntimeHealthUnavailableReason.timedOut;
      } else if (event.generation != channel.generation ||
          event.deviceId != deviceId) {
        return const RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        );
      } else if (event.kind == OtaStatusKind.error &&
          event.detail == 'unknown_cmd') {
        return const RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.unknownCommand,
        );
      } else if (event.kind != OtaStatusKind.runtimeHealth) {
        lastFailure = event.kind == OtaStatusKind.runtimeHealthMalformed
            ? RuntimeHealthUnavailableReason.malformed
            : RuntimeHealthUnavailableReason.timedOut;
      } else {
        final RuntimeHealthSnapshot? snapshot = RuntimeHealthSnapshot.fromToken(
          event.detail,
        );
        if (snapshot != null) {
          return RuntimeHealthAvailable(snapshot);
        }
        lastFailure = RuntimeHealthUnavailableReason.malformed;
      }

      if (attempt < 2) {
        final pause = Future<void>.delayed(const Duration(milliseconds: 180));
        await (supportScope?.wait(pause) ?? pause);
      }
    }
    return RuntimeHealthUnavailable(lastFailure);
  }

  /// Invokes the firmware-owned validated power recheck when explicitly
  /// advertised, then fetches ordinary board-owned health truth again.
  /// The action is attempted once; the app never retries or clears locally.
  Future<RuntimeHealthRecheckResult> recheckRuntimeHealth({
    required String deviceId,
  }) async {
    try {
      return await _recheckRuntimeHealth(deviceId: deviceId);
    } catch (_) {
      final bool connected = isConnected && connectedId == deviceId;
      return RuntimeHealthRecheckResult(
        outcome: connected
            ? RuntimeHealthRecheckOutcome.sendFailed
            : RuntimeHealthRecheckOutcome.disconnected,
        health: await _fetchRuntimeHealthAfterRecheck(deviceId),
      );
    }
  }

  Future<RuntimeHealthRecheckResult> _recheckRuntimeHealth({
    required String deviceId,
  }) async {
    if (!isConnected || connectedId != deviceId) {
      return const RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.disconnected,
        health: RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        ),
      );
    }
    final LitchDeviceCapabilities capabilities = await fetchDeviceCapabilities(
      deviceId: deviceId,
    );
    if (!isConnected || connectedId != deviceId) {
      return const RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.disconnected,
        health: RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        ),
      );
    }
    if (!capabilities.supportsRuntimeHealthRecheck) {
      return RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.unsupported,
        health: await _fetchRuntimeHealthAfterRecheck(deviceId),
      );
    }

    final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
      deviceId,
    );
    if (!_isActiveOtaChannel(deviceId, channel)) {
      return const RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.disconnected,
        health: RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        ),
      );
    }
    final Future<OtaStatusEvent?> response = channel.waitFor(
      (OtaStatusEvent event) =>
          event.kind == OtaStatusKind.runtimeHealthRecheckSucceeded ||
          event.kind == OtaStatusKind.runtimeHealthRecheckFailed ||
          event.kind == OtaStatusKind.runtimeHealthRecheckMalformed ||
          (event.kind == OtaStatusKind.error && event.detail == 'unknown_cmd'),
      timeout: const Duration(milliseconds: 1500),
    );
    final bool sent = await _sendOtaCommandForChannel(
      deviceId,
      channel,
      <String, Object>{'cmd': 'health_chk'},
      attempts: 1,
      delay: Duration.zero,
    );
    if (!sent) {
      return RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.sendFailed,
        health: await _fetchRuntimeHealthAfterRecheck(deviceId),
      );
    }
    final OtaStatusEvent? event = await response;
    if (!_isActiveOtaChannel(deviceId, channel)) {
      return const RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.disconnected,
        health: RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        ),
      );
    }
    if (event == null) {
      return RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.timedOut,
        health: await _fetchRuntimeHealthAfterRecheck(deviceId),
      );
    }
    if (event.generation != channel.generation || event.deviceId != deviceId) {
      return const RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.disconnected,
        health: RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        ),
      );
    }

    final RuntimeHealthRecheckOutcome outcome = switch (event.kind) {
      OtaStatusKind.runtimeHealthRecheckSucceeded =>
        RuntimeHealthRecheckOutcome.succeeded,
      OtaStatusKind.runtimeHealthRecheckFailed =>
        RuntimeHealthRecheckOutcome.validationFailed,
      OtaStatusKind.runtimeHealthRecheckMalformed =>
        RuntimeHealthRecheckOutcome.malformed,
      OtaStatusKind.error => RuntimeHealthRecheckOutcome.unsupported,
      _ => RuntimeHealthRecheckOutcome.timedOut,
    };
    return RuntimeHealthRecheckResult(
      outcome: outcome,
      health: await _fetchRuntimeHealthAfterRecheck(deviceId),
    );
  }

  Future<RuntimeHealthResult> _fetchRuntimeHealthAfterRecheck(
    String deviceId,
  ) async {
    try {
      return await fetchRuntimeHealth(deviceId: deviceId);
    } catch (_) {
      return RuntimeHealthUnavailable(
        isConnected && connectedId == deviceId
            ? RuntimeHealthUnavailableReason.sendFailed
            : RuntimeHealthUnavailableReason.disconnected,
      );
    }
  }

  Future<FirmwareProvenance> fetchFirmwareProvenance({
    required String deviceId,
  }) async {
    final generation = _connectionGeneration;
    LitchOtaControlChannel? readChannel;
    var awaitingBoardReadiness = false;
    final provenanceService = FirmwareProvenanceService(
      verifier: _firmwareProvenanceVerifier,
    );
    final provenance = await provenanceService.inspect(
      connected: isConnected && connectedId == deviceId,
      readDigest: () async {
        // The retained firmware page starts before capability discovery. Join
        // the same owner preparation as version/settings reads before issuing
        // any private OTA Object on the newly activated transport.
        final readGeneration = _connectionGeneration;
        final capabilities = await _connectionCapabilitiesForDomain(deviceId);
        if (readGeneration != _connectionGeneration ||
            !isConnected ||
            connectedId != deviceId ||
            !capabilities.supportsConnectionSession) {
          return null;
        }
        final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
          deviceId,
        );
        readChannel = channel;
        if (!_isActiveOtaChannel(deviceId, channel)) return null;
        final session = _activeConnectionSession;
        final s3 = switch (capabilities.hardwareRevision) {
          LitchHardwareRevision.v2 ||
          LitchHardwareRevision.v3 ||
          LitchHardwareRevision.v3_3 => true,
          _ => false,
        };
        if (capabilities.explicitlyReported &&
            capabilities.supportsFirmwareUpdate &&
            !capabilities.recoveryOnly &&
            s3 &&
            session != null &&
            session.isActivated) {
          // Ordinary provenance can reuse the normal health proof. Explicit
          // digest reads, recovery and pre-session peers retain their owner.
          final accepted = await _readOtaRuntimeAcceptanceFromChannel(
            deviceId,
            channel,
          );
          if (!_isActiveOtaChannel(deviceId, channel) ||
              !identical(session, _activeConnectionSession) ||
              !session.isActivated) {
            return null;
          }
          if (accepted != true) {
            // Only explicit pending acceptance requests a later visible-page
            // read. Missing/error replies and unknown origin records do not.
            awaitingBoardReadiness = accepted == false;
            return null;
          }
        }
        return _readFirmwareDigestFromChannel(deviceId, channel);
      },
    );
    if (_disposed || !isConnected || connectedId != deviceId) {
      return FirmwareProvenance.offline;
    }
    if (generation != _connectionGeneration ||
        (readChannel != null && !_isActiveOtaChannel(deviceId, readChannel!))) {
      return FirmwareProvenance.unverified;
    }
    return awaitingBoardReadiness
        ? FirmwareProvenance.pendingBoardReadiness
        : provenance;
  }

  Future<OtaUploadResult> uploadFirmwareFile({
    required String deviceId,
    required File firmwareFile,
    required FirmwareManifest manifest,
    void Function(double progress)? onProgress,
    void Function(String status)? onStatus,
  }) async {
    if (_otaUploadActive) {
      return const OtaUploadFailed('A firmware update is already in progress.');
    }
    _otaUploadActive = true;
    try {
      await restorePendingOtaVerification();
      if (pendingOtaVerification != null) {
        return const OtaUploadFailed(
          'Verify the pending update before starting another update.',
        );
      }
      return await _uploadFirmwareFile(
        deviceId: deviceId,
        firmwareFile: firmwareFile,
        manifest: manifest,
        onProgress: onProgress,
        onStatus: onStatus,
      );
    } catch (error) {
      _pushAppLog('OTA preparation failed: $error');
      final pending = pendingOtaVerification;
      if (pending != null && pending.deviceId == deviceId) {
        return OtaUploadPendingVerification(
          'The update result is still awaiting verification.',
          connectionToken: pending.uploadConnectionToken,
        );
      }
      if (error is FirmwareArtifactValidationException) {
        return const OtaUploadFailed(
          FirmwareArtifactValidationException.userMessage,
        );
      }
      return const OtaUploadFailed(
        'The update could not be prepared safely. Restart the app and try again.',
      );
    } finally {
      _otaUploadActive = false;
    }
  }

  Future<OtaUploadResult> _uploadFirmwareFile({
    required String deviceId,
    required File firmwareFile,
    required FirmwareManifest manifest,
    void Function(double progress)? onProgress,
    void Function(String status)? onStatus,
  }) async {
    if (!isConnected || connectedId != deviceId) {
      _pushAppLog('OTA upload blocked: board is not connected.');
      onStatus?.call('Board is not connected for BLE upload.');
      return const OtaUploadFailed('Board is not connected for BLE upload.');
    }
    // A failed Object lifetime can leave the native link connected. Renew it
    // through the common owner before capturing this attempt's generation.
    if (_activeConnectionSession?.isActivated == false) {
      await openConnectionSession(deviceId);
    }
    final int uploadConnectionToken = _connectionGeneration;
    final LitchDeviceCapabilities capabilities = await fetchDeviceCapabilities(
      deviceId: deviceId,
    );
    if (!capabilities.supportsConnectionSession) {
      const message =
          'Board capabilities are unavailable. Reconnect the board; if this continues, install the current firmware over USB.';
      onStatus?.call(message);
      return const OtaUploadFailed(message);
    }
    if (!capabilities.supportsFirmwareUpdate) {
      _pushAppLog(
        'Firmware update blocked: ${capabilities.hardwareLabel} uses manual USB flashing.',
      );
      onStatus?.call(
        '${capabilities.hardwareLabel} is updated manually over USB.',
      );
      return OtaUploadFailed(
        '${capabilities.hardwareLabel} is updated manually over USB.',
      );
    }
    if (!await firmwareFile.exists()) {
      _pushAppLog('OTA upload blocked: firmware file missing.');
      onStatus?.call('Firmware file missing.');
      return const OtaUploadFailed('Firmware file missing.');
    }

    final int totalBytes = await firmwareFile.length();
    if (totalBytes <= 0) {
      _pushAppLog('OTA upload blocked: firmware file is empty.');
      onStatus?.call('Firmware file is empty.');
      return const OtaUploadFailed('Firmware file is empty.');
    }

    final inspection = await FirmwareArtifactValidator.inspectFile(
      firmwareFile,
      manifest,
    );
    FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
    if (uploadConnectionToken != _connectionGeneration ||
        !isConnected ||
        connectedId != deviceId) {
      return const OtaUploadFailed(
        'The board connection changed while preparing the update.',
      );
    }

    await _ensureLinkCalibrated(deviceId);
    int chunkBytes = _otaDataChunkSize;

    ConnectionSession? otaSession = _activeConnectionSession;
    if (otaSession == null &&
        (_openingConnectionSession != null ||
            deviceCapabilities(deviceId).supportsConnectionSession)) {
      otaSession = await openConnectionSession(deviceId);
    }
    if (otaSession?.isActivated == true) {
      chunkBytes = otaSession!.objectMaximumBody;
    } else {
      otaSession = null;
    }
    if (otaSession == null || otaSession.boardBootNonce == 0) {
      return const OtaUploadFailed(
        'This board cannot provide update verification. Contact support for help.',
      );
    }
    final List<({int opcode, Uint8List body})> pendingSharedChunks =
        <({int opcode, Uint8List body})>[];
    int objectSubmissions = 0;
    int objectNativeUs = 0;
    int objectGrantWaitUs = 0;
    int objectAdmissionTailUs = 0;

    final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
      deviceId,
    );
    if (channel.connectionToken != uploadConnectionToken ||
        !_isActiveOtaChannel(deviceId, channel) ||
        !identical(otaSession, _activeConnectionSession)) {
      return const OtaUploadFailed(
        'The board connection changed before upload. Reconnect it and try again.',
      );
    }
    final preBootNonce = _sessionBootNonce(otaSession);
    bool current() =>
        _isActiveOtaChannel(deviceId, channel) &&
        identical(otaSession, _activeConnectionSession) &&
        _sessionBootNonce(otaSession!) == preBootNonce;
    final preflight = await _readOtaAttemptProofFromChannel(deviceId, channel);
    if (!current()) {
      return const OtaUploadFailed(
        'The board connection changed before starting the update. Reconnect it and try again.',
      );
    }
    if (preflight == null ||
        !preflight.hasCoherentSelection ||
        preflight.runningState != 2) {
      return const OtaUploadFailed(
        'This firmware cannot confirm a safe update source. Open Help for support.',
      );
    }
    // Recovery exposes the same integrity-preserving adapter while deliberately
    // withholding normal-runtime acceptance. Only current explicit mode grants it.
    final recoverySource =
        capabilities.explicitlyReported && capabilities.recoveryOnly;
    if (!preflight.runtimeAccepted && !recoverySource) {
      return const OtaUploadFailed(
        'The board has not confirmed it is ready for an update. Keep it powered on, then try again. If this continues, open Help for support.',
      );
    }
    // New non-OTA producers are fenced before sequence assignment. Finish all
    // earlier Object/native obligations before firmware changes queue authority
    // at Begin; transport admission is still separate from domain persistence.
    await otaSession.drainObjects();
    if (!current()) {
      return const OtaUploadFailed(
        'The board connection changed before starting the update. Reconnect it and try again.',
      );
    }
    Future<void> flushSharedChunks() async {
      if (!current()) throw StateError('OTA connection generation changed.');
      if (pendingSharedChunks.isEmpty) return;
      final chunks = List<({int opcode, Uint8List body})>.of(
        pendingSharedChunks,
      );
      pendingSharedChunks.clear();
      await otaSession!.sendObjectBatch(
        chunks,
        priority: ConnectionSessionObjectPriority.bulk,
        completion: ConnectionSessionObjectCompletion.submitted,
        onObservation: (fields) {
          objectSubmissions += 1;
          objectNativeUs += fields['native_us'] as int;
          objectGrantWaitUs += fields['grant_wait_us'] as int;
          final accepted = fields['accepted_us'] as int?;
          final returned = fields['last_return_us'] as int?;
          if (accepted != null && returned != null && accepted > returned) {
            objectAdmissionTailUs += accepted - returned;
          }
        },
      );
    }

    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: channel.connectionToken,
      events: channel.events,
      disconnects: connectionStream
          .where(
            (ConnectionStateUpdate update) =>
                update.deviceId == deviceId &&
                update.connectionState == DeviceConnectionState.disconnected,
          )
          .map<void>((_) {}),
      sendControl: (Map<String, Object> payload) {
        return _sendOtaCommandForChannel(
          deviceId,
          channel,
          payload,
          attempts: 1,
          delay: Duration.zero,
        );
      },
      beforeEnd: () async {
        final proof = await _readOtaAttemptProofFromChannel(deviceId, channel);
        if (proof == null ||
            !proof.hasCoherentSelection ||
            proof.admission.phase != 1 ||
            proof.admission.attempt == 0 ||
            proof.admission.sourceSlot != proof.runningSlot ||
            proof.admission.sourceSequence != proof.selectedSequence ||
            proof.admission.targetSlot == proof.runningSlot ||
            proof.runningState != 2) {
          throw StateError(
            'The board did not provide a coherent update admission.',
          );
        }
        final source = proof.selectors[proof.selectedSector];
        if (!source.crcValid || source.state != 2) {
          throw StateError('The preserved source is not confirmed.');
        }
        final identity = OtaAttemptIdentity(
          preBootNonce: proof.bootNonce,
          attempt: proof.admission.attempt,
          sourceSlot: proof.admission.sourceSlot,
          sourceSector: proof.selectedSector,
          sourceSequence: proof.admission.sourceSequence,
          targetSlot: proof.admission.targetSlot,
          targetSector: 1 - proof.selectedSector,
          expectedSequence: proof.admission.expectedSequence,
        );
        if (!identity.isValid) {
          throw StateError('Invalid update attempt identity.');
        }
        await rememberPendingOtaVerification(
          OtaVerificationTarget(
            deviceId: deviceId,
            uploadConnectionToken: uploadConnectionToken,
            version: manifest.version,
            imageSha256: inspection.imageSha256,
            artifactSha256: inspection.artifactSha256,
            attempt: identity,
          ),
        );
        if (!_isActiveOtaChannel(deviceId, channel) ||
            !identical(otaSession, _activeConnectionSession)) {
          throw StateError('The connection changed before finalization.');
        }
      },
      flushPendingChunks: flushSharedChunks,
      writeChunk: (List<int> bytes, {required bool reliable}) async {
        if (!_isActiveOtaChannel(deviceId, channel)) {
          throw StateError('OTA connection generation changed.');
        }
        pendingSharedChunks.add((
          opcode: ConnectionOtaCodec.dataObjectOpcode,
          body: Uint8List.fromList(bytes),
        ));
        if (reliable) await flushSharedChunks();
      },
      options: LitchFirmwareUploadOptions(
        chunkBytes: chunkBytes,
        compactStatusWindowBytes: 4096,
        maximumWindowBytes: chunkBytes * 48,
        reliableBurst: 48,
      ),
    );

    _pushAppLog(
      'OTA upload requested: ${firmwareFile.path} ($totalBytes bytes) '
      'packet_max=${otaSession.selectedPacketMaximum} requested_chunk=$chunkBytes.',
    );
    final result = await uploader.upload(
      firmwareFile: firmwareFile,
      totalBytes: totalBytes,
      onProgress: onProgress,
      onDiagnostics: _pushAppLog,
      onStatus: (String status) {
        _pushAppLog(status);
        onStatus?.call(status);
      },
    );
    _pushAppLog(
      'OTA shared Object: submissions=$objectSubmissions '
      'native_ms=${(objectNativeUs / 1000).toStringAsFixed(3)} '
      'grant_wait_ms=${(objectGrantWaitUs / 1000).toStringAsFixed(3)} '
      'admission_tail_ms=${(objectAdmissionTailUs / 1000).toStringAsFixed(3)}',
    );
    if (result is OtaUploadFinalized) {
      _otaVerificationOwner.acknowledgeFinalization(uploadConnectionToken);
    } else if (result is OtaUploadFailed) {
      final target = pendingOtaVerification;
      if (target != null &&
          target.deviceId == deviceId &&
          target.uploadConnectionToken == uploadConnectionToken) {
        // Failed is possible only before End may be submitted. All post-End
        // outcomes retain this target until correlated proof resolves them.
        try {
          await clearPendingOtaVerification(target);
        } catch (error) {
          _pushAppLog('OTA failure retained its unresolved target: $error');
          return OtaUploadPendingVerification(
            'The update result is still awaiting verification.',
            connectionToken: uploadConnectionToken,
          );
        }
      }
    }
    return result;
  }

  Future<OtaPostRebootVerification> verifyFirmwareAfterOta({
    required String deviceId,
    required int uploadConnectionToken,
    required String expectedVersion,
    required String expectedImageSha256,
  }) async {
    try {
      await restorePendingOtaVerification();
    } catch (_) {
      return const OtaPostRebootPending(
        'The pending update could not be restored. Restart the app and retry verification.',
      );
    }
    final target = pendingOtaVerification;
    if (target == null ||
        target.deviceId != deviceId ||
        target.version != expectedVersion ||
        target.imageSha256 != expectedImageSha256) {
      return const OtaPostRebootPending(
        'The original update attempt is unavailable.',
      );
    }
    final waitForReboot = pendingOtaRebootExpected;
    bool hasVerificationLink() =>
        isConnected &&
        connectedId == deviceId &&
        (!waitForReboot ||
            _connectionGeneration > target.uploadConnectionToken);

    const OtaPostRebootVerifier verifier = OtaPostRebootVerifier();
    return verifier.verify(
      target: target,
      openFreshChannel: () async {
        // An acknowledged End can leave the upload link alive through the
        // reboot grace. An uncertain End may still prove a same-boot abort.
        if (!hasVerificationLink()) {
          try {
            await connectionStream
                .where(
                  (ConnectionStateUpdate update) =>
                      update.deviceId == deviceId &&
                      update.connectionState ==
                          DeviceConnectionState.connected &&
                      hasVerificationLink(),
                )
                .timeout(verifier.timeout)
                .first;
          } on TimeoutException {
            return null;
          }
        }
        if (!hasVerificationLink()) return null;
        final generation = _connectionGeneration;
        if (generation < target.uploadConnectionToken) return null;
        await fetchDeviceCapabilities(deviceId: deviceId);
        // A native reconnect precedes quiet owner proof. Join its existing
        // preparation before private identity reads consume their read budget.
        if (!await ensureProductReady(deviceId)) return null;
        if (generation != _connectionGeneration ||
            !isConnected ||
            connectedId != deviceId) {
          return null;
        }
        final LitchOtaControlChannel channel = await _ensureOtaControlChannel(
          deviceId,
        );
        return generation == _connectionGeneration &&
                _isActiveOtaChannel(deviceId, channel)
            ? channel
            : null;
      },
      readVersion: (LitchOtaControlChannel channel) =>
          _readFirmwareVersionFromChannel(deviceId, channel),
      readDigest: (LitchOtaControlChannel channel) =>
          _readFirmwareDigestFromChannel(deviceId, channel),
      readAcceptance: (LitchOtaControlChannel channel) =>
          _readOtaRuntimeAcceptanceFromChannel(deviceId, channel),
      readAttemptProof: (LitchOtaControlChannel channel) =>
          _readOtaAttemptProofFromChannel(deviceId, channel),
      isChannelFresh: (LitchOtaControlChannel channel) =>
          _isActiveOtaChannel(deviceId, channel),
    );
  }

  bool _isActiveOtaChannel(String deviceId, LitchOtaControlChannel channel) {
    return isConnected &&
        connectedId == deviceId &&
        channel.connectionToken == _connectionGeneration &&
        identical(_otaControlChannel, channel);
  }

  static String _sessionBootNonce(ConnectionSession session) =>
      '${((session.boardBootNonce >> 32) & 0xffffffff).toRadixString(16).padLeft(8, '0')}'
      '${(session.boardBootNonce & 0xffffffff).toRadixString(16).padLeft(8, '0')}';

  Future<OtaAttemptProof?> _readOtaAttemptProofFromChannel(
    String deviceId,
    LitchOtaControlChannel channel,
  ) async {
    final session = _activeConnectionSession;
    if (!_isActiveOtaChannel(deviceId, channel) ||
        session?.isActivated != true ||
        session!.boardBootNonce == 0 ||
        _otaProofRequestId == 0xffffffff) {
      return null;
    }
    final nonce = _sessionBootNonce(session);
    final request = ++_otaProofRequestId;
    final cancelled = Completer<void>();
    final response = channel.waitFor(
      (event) =>
          (event.kind == OtaStatusKind.attemptProof ||
              event.kind == OtaStatusKind.attemptProofMalformed) &&
          event.payload['q'] == request,
      timeout: const Duration(seconds: 5),
      cancelled: cancelled.future,
    );
    try {
      final sent = await _sendOtaCommandForChannel(
        deviceId,
        channel,
        <String, Object>{'cmd': 'ota_state', 'proof': 1, 'q': request},
        attempts: 1,
        delay: Duration.zero,
      );
      if (!sent) return null;
      final event = await response;
      if (!_isActiveOtaChannel(deviceId, channel) ||
          !identical(session, _activeConnectionSession) ||
          event?.generation != channel.generation ||
          event?.deviceId != deviceId ||
          event?.kind != OtaStatusKind.attemptProof) {
        return null;
      }
      final proof = OtaAttemptProof.tryParse(event!.payload);
      return proof?.requestId == request && proof?.bootNonce == nonce
          ? proof
          : null;
    } finally {
      cancelled.complete();
    }
  }

  Future<bool> _sendOtaCommandForChannel(
    String deviceId,
    LitchOtaControlChannel channel,
    Map<String, Object> payload, {
    required int attempts,
    required Duration delay,
    bool allowPreSessionDirect = false,
    SupportCollectionScope? supportScope,
    bool Function()? isCurrent,
  }) async {
    if (deviceCapabilities(deviceId).supportsOwnerPairing &&
        !productReady &&
        payload['cmd'] != 'info') {
      return false;
    }
    bool current() =>
        _isActiveOtaChannel(deviceId, channel) && (isCurrent?.call() ?? true);
    for (int attempt = 0; attempt < attempts; ++attempt) {
      supportScope?.check();
      if (!current()) {
        return false;
      }
      final capabilities = deviceCapabilities(deviceId);
      // Direct OTA control is only a bootstrap route while the board profile
      // is unknown. Once a shared-session profile is known, a concurrent Hello
      // may already have retired that ingress on the board before our session
      // object becomes active locally.
      final bool bootstrapInfo =
          allowPreSessionDirect &&
          payload['cmd'] == 'info' &&
          !capabilities.explicitlyReported;
      final bool requiresShared = !bootstrapInfo;
      ConnectionSession? session = _activeConnectionSession;
      if (session?.isActivated != true && requiresShared) {
        session = await openConnectionSession(deviceId);
      }
      bool sent = false;
      if (session?.isActivated == true && current()) {
        // Opening the session can discover ownership after the entry check.
        // Even info is private when carried by the shared Object stream.
        final currentCapabilities = deviceCapabilities(deviceId);
        if (currentCapabilities.supportsOwnerPairing &&
            !currentCapabilities.recoveryOnly &&
            !productReady) {
          return false;
        }
        final Uint8List encoded = Uint8List.fromList(
          utf8.encode(jsonEncode(payload)),
        );
        final fragments = ConnectionOtaCodec.encodeControl(
          encoded,
          maximumBodyBytes: session!.objectMaximumBody,
        );
        try {
          await session.sendObjectBatch(
            fragments
                .map(
                  (body) => (
                    opcode: ConnectionOtaCodec.controlObjectOpcode,
                    body: body,
                  ),
                )
                .toList(growable: false),
            priority: ConnectionSessionObjectPriority.interactive,
            atomicMessage: true,
          );
          sent = identical(session, _activeConnectionSession) && current();
        } catch (_) {
          sent = false;
        }
      } else if (!requiresShared) {
        sent = await _sendCommandJson(
          deviceId,
          payload,
          characteristicId: BleProtocol.otaControlCharacteristic,
          allowWriteWithoutResponse: true,
          supportScope: supportScope,
          isCurrent: current,
        );
      }
      if (sent) {
        return current();
      }
      if (!current()) return false;
      if (attempt + 1 < attempts) {
        final pause = Future<void>.delayed(delay);
        await (supportScope?.wait(pause) ?? pause);
      }
    }
    return false;
  }

  Future<String?> _readFirmwareVersionFromChannel(
    String deviceId,
    LitchOtaControlChannel channel,
  ) async {
    final Future<OtaStatusEvent?> response = channel.waitFor(
      (OtaStatusEvent event) =>
          event.kind == OtaStatusKind.version ||
          event.kind == OtaStatusKind.error,
      timeout: const Duration(seconds: 5),
    );
    if (!await _sendOtaCommandForChannel(
      deviceId,
      channel,
      <String, Object>{'cmd': 'fw_version'},
      attempts: 3,
      delay: const Duration(milliseconds: 250),
    )) {
      return null;
    }
    final OtaStatusEvent? event = await response;
    if (!_isActiveOtaChannel(deviceId, channel) ||
        event?.generation != channel.generation ||
        event?.kind != OtaStatusKind.version) {
      return null;
    }
    return event!.detail;
  }

  Future<String?> _readFirmwareDigestFromChannel(
    String deviceId,
    LitchOtaControlChannel channel, {
    SupportCollectionScope? supportScope,
  }) async {
    supportScope?.check();
    const FirmwareDigestReader reader = FirmwareDigestReader();
    bool queryActive = true;
    try {
      final String? digest = await reader.read(
        events: channel.events,
        cancelled: supportScope?.stopped,
        onTerminal: () => queryActive = false,
        throwOnTimeout: supportScope != null,
        sendRequest: () => _sendOtaCommandForChannel(
          deviceId,
          channel,
          <String, Object>{'cmd': 'fw_digest'},
          attempts: 3,
          delay: const Duration(milliseconds: 250),
          supportScope: supportScope,
          isCurrent: () => queryActive,
        ),
      );
      supportScope?.check();
      return _isActiveOtaChannel(deviceId, channel) ? digest : null;
    } finally {
      queryActive = false;
    }
  }

  Future<bool?> _readOtaRuntimeAcceptanceFromChannel(
    String deviceId,
    LitchOtaControlChannel channel,
  ) async {
    final response = channel.waitFor(
      (event) =>
          event.kind == OtaStatusKind.runtimeAcceptance ||
          event.kind == OtaStatusKind.runtimeAcceptanceMalformed ||
          event.kind == OtaStatusKind.error,
      timeout: const Duration(seconds: 5),
    );
    final sent = await _sendOtaCommandForChannel(
      deviceId,
      channel,
      <String, Object>{'cmd': 'ota_state'},
      attempts: 1,
      delay: Duration.zero,
    );
    final event = await response;
    if (!sent ||
        !_isActiveOtaChannel(deviceId, channel) ||
        event?.generation != channel.generation ||
        event?.deviceId != deviceId ||
        event?.kind != OtaStatusKind.runtimeAcceptance) {
      return null;
    }
    return event!.detail == 'v';
  }

  Future<void> _safeCancelSubscription<T>(
    StreamSubscription<T>? subscription,
    String context,
  ) async {
    if (subscription == null) {
      return;
    }

    try {
      await subscription.cancel();
    } catch (error) {
      _pushAppLog('$context: $error');
    }
  }

  Future<void> dispose() {
    return _disposeFuture ??= _dispose();
  }

  Future<void> _dispose() async {
    _supportCollection?.stop(SupportQueryOutcome.canceled);
    _disposed = true;
    _retireConnection();
    _ownerClaimRetryRequiredBoards.clear();
    await _pairingKeyMissingSub?.cancel();
    await _pairingBondFailureSub.cancel();
    await _pairingRecoveryGateway.watchDevice(null, _connectionGeneration);
    await _pairingRecoveryGateway.dispose();
    await _bleSetupResetFuture;
    await stopScan();
    await _disconnect(emitUpdate: false);
    _persistConsoleSnapshot();
    await _bleStatusSub?.cancel();
    await _boardsController.close();
    await _availabilityController.close();
    await _consoleController.close();
    await _homeStateController.close();
    await _boardSwitchStateController.close();
    await _boardPhysicalInputStateController.close();
    await _currentGameLineController.close();
    await _boardHealthController.close();
    await _boardStorageRecoveryController.close();
    await _incidentController.close();
    await _consoleTraceOutputController.close();
    await _connectionController.close();
    await _productReadinessController.close();
    await _ownerPairingStatusController.close();
    await _pairingRecoveryController.close();
    await _otaStatusController.close();
    await _settingsChangedController.close();
    await _controlActionController.close();
  }

  Future<bool> _ensurePermissions({required bool requestIfNeeded}) async {
    if (defaultTargetPlatform == TargetPlatform.iOS) {
      PermissionStatus bluetoothStatus = await _permissionGateway.status(
        Permission.bluetooth,
      );
      if (!bluetoothStatus.isGranted && requestIfNeeded) {
        bluetoothStatus =
            (await _permissionGateway.request(<Permission>[
              Permission.bluetooth,
            ]))[Permission.bluetooth] ??
            bluetoothStatus;
      }

      if (!bluetoothStatus.isGranted) {
        _logPermissionIssueThrottled(platform: 'iOS', status: bluetoothStatus);
        _setBleAvailability(_bluetoothPermissionAvailability(bluetoothStatus));
        return false;
      }
      return true;
    }

    if (defaultTargetPlatform != TargetPlatform.android) {
      return true;
    }

    PermissionStatus scanStatus = await _permissionGateway.status(
      Permission.bluetoothScan,
    );
    PermissionStatus connectStatus = await _permissionGateway.status(
      Permission.bluetoothConnect,
    );
    PermissionStatus locationStatus = await _permissionGateway.status(
      Permission.locationWhenInUse,
    );

    final bool allGranted =
        scanStatus.isGranted &&
        connectStatus.isGranted &&
        locationStatus.isGranted;
    if (allGranted) {
      return true;
    }

    if (requestIfNeeded) {
      final Map<Permission, PermissionStatus> updated = await _permissionGateway
          .request(<Permission>[
            Permission.bluetoothScan,
            Permission.bluetoothConnect,
            Permission.locationWhenInUse,
          ]);
      scanStatus = updated[Permission.bluetoothScan] ?? scanStatus;
      connectStatus = updated[Permission.bluetoothConnect] ?? connectStatus;
      locationStatus = updated[Permission.locationWhenInUse] ?? locationStatus;
    }

    if (!(scanStatus.isGranted && connectStatus.isGranted)) {
      _logPermissionIssueThrottled(
        platform: 'Android',
        status:
            'scan=$scanStatus connect=$connectStatus location=$locationStatus',
      );
      _setBleAvailability(
        _bluetoothPermissionAvailability(
          scanStatus.isGranted ? connectStatus : scanStatus,
        ),
      );
      return false;
    }
    if (!locationStatus.isGranted) {
      _logPermissionIssueThrottled(
        platform: 'Android',
        status: 'location=$locationStatus',
      );
      _setBleAvailability(_locationPermissionAvailability(locationStatus));
      return false;
    }
    return true;
  }

  static BleAvailability _bluetoothPermissionAvailability(
    PermissionStatus status,
  ) {
    return status.isPermanentlyDenied || status.isRestricted
        ? BleAvailability.bluetoothPermissionBlocked
        : BleAvailability.bluetoothPermissionDenied;
  }

  static BleAvailability _locationPermissionAvailability(
    PermissionStatus status,
  ) {
    return status.isPermanentlyDenied || status.isRestricted
        ? BleAvailability.locationPermissionBlocked
        : BleAvailability.locationPermissionDenied;
  }

  void _logPermissionIssueThrottled({
    required String platform,
    required Object status,
  }) {
    final String token = '$platform:$status';
    final DateTime now = DateTime.now();
    final DateTime? lastLogAt = _lastPermissionLogAt;
    final bool changed = token != _lastPermissionLogToken;
    final bool cooldownElapsed =
        lastLogAt == null ||
        now.difference(lastLogAt) >= _permissionLogCooldown;
    if (!changed && !cooldownElapsed) {
      return;
    }

    _lastPermissionLogToken = token;
    _lastPermissionLogAt = now;
    _pushAppLog('Bluetooth permission not granted on $platform: $status');
  }

  void _logSettingsDiagThrottled({
    required String token,
    required String line,
  }) {
    final DateTime now = DateTime.now();
    final DateTime? last = _settingsDiagLastLogAt[token];
    if (last != null && now.difference(last) < _settingsDiagLogCooldown) {
      return;
    }

    _settingsDiagLastLogAt[token] = now;
    _pushAppLog(line);
  }

  void _logOptionalCommandUnsupported({
    required String token,
    required String line,
  }) {
    final DateTime now = DateTime.now();
    final DateTime? last = _optionalCommandLastLogAt[token];
    if (last != null && now.difference(last) < _optionalCommandLogCooldown) {
      return;
    }

    _optionalCommandLastLogAt[token] = now;
    _pushAppLog(line);
  }

  Future<bool> _ensureBleReadyForScan() async {
    _ensureBleStatusObservation();
    try {
      final BleStatus status =
          _lastBleStatus ??
          await _ble.statusStream
              .where(_isTerminalBleStatus)
              .first
              .timeout(const Duration(seconds: 3));

      _onBleStatus(status);

      if (status == BleStatus.ready) {
        return true;
      }

      return false;
    } catch (error) {
      _setBleAvailability(BleAvailability.unavailable);
      _pushAppLog('BLE readiness check failed: $error');
      return false;
    }
  }

  void _ensureBleStatusObservation() {
    if (_bleStatusSub != null) return;
    final generation = _bleSetupGeneration;
    void failed([Object? error]) {
      if (_disposed || generation != _bleSetupGeneration) return;
      _bleSetupNeedsReset = true;
      _lastBleStatus = null;
      _isScanning = false;
      _scanStartedAt = null;
      ++_scanGeneration;
      _setBleAvailability(BleAvailability.unavailable);
      _pushAppLog('BLE status stream ended: ${error ?? 'closed'}');
    }

    _bleStatusSub = _ble.statusStream.listen(
      (status) {
        if (!_disposed && generation == _bleSetupGeneration) {
          _onBleStatus(status);
        }
      },
      onError: (Object error, StackTrace stackTrace) {
        failed(error);
      },
      onDone: () {
        // A completed status observation can be reacquired without resetting
        // an otherwise healthy scanner/connection (including snapshot shims).
        if (!_disposed && generation == _bleSetupGeneration) {
          _bleStatusSub = null;
        }
      },
    );
  }

  static bool _isTerminalBleStatus(BleStatus status) {
    return status == BleStatus.ready ||
        status == BleStatus.unauthorized ||
        status == BleStatus.poweredOff ||
        status == BleStatus.locationServicesDisabled ||
        status == BleStatus.unsupported;
  }

  void _onBleStatus(BleStatus status) {
    _lastBleStatus = status;
    final BleAvailability availability = switch (status) {
      BleStatus.ready => BleAvailability.ready,
      BleStatus.unauthorized =>
        _isPermissionAvailability(_bleAvailability)
            ? _bleAvailability
            : BleAvailability.bluetoothPermissionDenied,
      BleStatus.poweredOff => BleAvailability.poweredOff,
      BleStatus.locationServicesDisabled =>
        BleAvailability.locationServicesDisabled,
      BleStatus.unsupported => BleAvailability.unsupported,
      BleStatus.unknown => BleAvailability.checking,
    };
    _setBleAvailability(availability);

    if (availability != BleAvailability.ready) {
      if (_isScanning) {
        unawaited(
          _stopScanForUnavailableBle().then<void>(
            (_) => _resumeRequestedScanIfReady(),
          ),
        );
      }
      return;
    }

    _resumeRequestedScanIfReady();
  }

  static bool _isPermissionAvailability(BleAvailability availability) {
    return availability == BleAvailability.bluetoothPermissionDenied ||
        availability == BleAvailability.bluetoothPermissionBlocked ||
        availability == BleAvailability.locationPermissionDenied ||
        availability == BleAvailability.locationPermissionBlocked;
  }

  void _resumeRequestedScanIfReady() {
    if (_lastBleStatus != BleStatus.ready) {
      return;
    }
    if (_scanRequested &&
        !_isScanning &&
        !_connectAttemptInProgress &&
        !isConnected &&
        _isAppForeground) {
      unawaited(startScan());
    }
  }

  Future<void> _stopScanForUnavailableBle() async {
    final int stoppedGeneration = ++_scanGeneration;
    final StreamSubscription<DiscoveredDevice>? subscription = _scanSub;
    await subscription?.cancel();
    if (stoppedGeneration == _scanGeneration) {
      _scanSub = null;
      _isScanning = false;
    }
  }

  void _setBleAvailability(BleAvailability availability) {
    if (_bleAvailability == availability || _disposed) {
      return;
    }
    _bleAvailability = availability;
    if (availability != BleAvailability.ready &&
        availability != BleAvailability.checking) {
      _clearDiscoveredBoards();
    }
    _availabilityController.add(availability);
  }

  void _clearDiscoveredBoards() {
    if (_boardsById.isEmpty) {
      return;
    }
    _boardsById.clear();
    _boardsController.add(const <LitchBoard>[]);
  }

  void _resetLinkProfileForPlatform() {
    _litchcraftPacketPayloadBytes = 20;
    if (defaultTargetPlatform == TargetPlatform.android) {
      _otaDataChunkSize = 20;
      _otaControlSafeBytes = 20;
    } else {
      _otaDataChunkSize = 180;
      _otaControlSafeBytes = 120;
    }
    _linkCalibrationFuture = null;
  }

  Future<void> _ensureLinkCalibrated(String deviceId) async {
    final Future<void>? inFlight = _linkCalibrationFuture;
    if (inFlight != null) {
      await inFlight;
      return;
    }

    _linkCalibrationFuture = _calibrateLinkProfile(deviceId);
    await _linkCalibrationFuture;
  }

  Future<void> _calibrateLinkProfile(String deviceId) async {
    final int generation = _connectionGeneration;
    if (!isConnected || connectedId != deviceId) {
      return;
    }

    if (defaultTargetPlatform == TargetPlatform.android) {
      try {
        await _untilConnectionEnds(
          _ble.requestConnectionPriority(
            deviceId: deviceId,
            priority: ConnectionPriority.highPerformance,
          ),
        );
        if (_disposed ||
            !isConnected ||
            connectedId != deviceId ||
            generation != _connectionGeneration) {
          return;
        }
        _pushAppLog('Android BLE high-performance priority request accepted.');
      } catch (error) {
        if (_disposed ||
            !isConnected ||
            connectedId != deviceId ||
            generation != _connectionGeneration) {
          return;
        }
        // Priority is a performance request, not a compatibility requirement.
        _pushAppLog(
          'Android BLE high-performance priority unavailable: $error',
        );
      }
    }

    try {
      final int mtu = await _untilConnectionEnds(
        _ble.requestMtu(
          deviceId: deviceId,
          mtu: defaultTargetPlatform == TargetPlatform.android ? 517 : 247,
        ),
      );
      if (_disposed ||
          !isConnected ||
          connectedId != deviceId ||
          generation != _connectionGeneration) {
        return;
      }
      // Android reports ATT MTU; Apple reports its native without-response
      // value limit. A 495-byte value plus ATT/L2CAP headers fits two DLE251
      // fragments, while 512 needs three. All shared domains use this profile.
      final nativePayloadBytes = defaultTargetPlatform == TargetPlatform.android
          ? mtu - 3
          : mtu;
      final int payloadBytes = nativePayloadBytes.clamp(20, 495).toInt();
      _otaDataChunkSize = payloadBytes;
      _litchcraftPacketPayloadBytes = payloadBytes;
      _otaControlSafeBytes = payloadBytes.clamp(20, 120).toInt();
      _pushAppLog(
        'BLE link calibrated: mtu=$mtu control<=$_otaControlSafeBytes ota_chunk=$_otaDataChunkSize frame_store=$_litchcraftPacketPayloadBytes',
      );
    } catch (error) {
      if (_disposed ||
          !isConnected ||
          connectedId != deviceId ||
          generation != _connectionGeneration) {
        return;
      }
      // Keep this completed attempt for the physical connection. Retrying MTU
      // during session activation can block the native queue again, and a late
      // callback cannot be correlated with a second request. Safe defaults are
      // valid for every delivery lane; a new connection gets a fresh attempt.
      _pushAppLog('BLE calibration failed; using safe defaults: $error');
    }
  }

  void _startConsoleMirror(String deviceId) {
    if (!productReady) return;
    if ((_capabilityDiscoveryByDevice.containsKey(deviceId) &&
            !deviceCapabilities(deviceId).explicitlyReported) ||
        deviceCapabilities(deviceId).recoveryOnly) {
      return;
    }
    if (!isConnected ||
        connectedId != deviceId ||
        !_shouldRunConnectedBridges()) {
      return;
    }
    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.baseService,
      characteristicId: BleProtocol.consoleStreamCharacteristic,
      deviceId: deviceId,
    );

    final StreamSubscription<List<int>>? oldSubscription =
        _invalidateConsoleBridge();
    final int generation = _consoleBridgeGeneration;
    unawaited(oldSubscription?.cancel());
    _consoleSub = _subscribeToCharacteristic(characteristic).listen(
      (List<int> packet) {
        if (!_isCurrentConsoleBridge(deviceId, generation)) {
          return;
        }
        final observer = _consolePacketObserver;
        if (observer != null) {
          if (observer.deviceId != deviceId ||
              observer.connectionToken != _connectionGeneration ||
              !productReady) {
            _retireConsolePacketObserver(
              StateError('Console observation connection changed.'),
            );
          } else {
            try {
              observer.onPacket(List<int>.unmodifiable(packet));
            } catch (error) {
              _retireConsolePacketObserver(error);
            }
          }
        }
        _appendConsolePacket(packet);
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!_isCurrentConsoleBridge(deviceId, generation)) {
          return;
        }
        _retireConsolePacketObserver(error);
        unawaited(_invalidateConsoleBridge()?.cancel());
        _pushAppLog('Console stream unavailable; retrying: $error');
        _scheduleConsoleBridgeRetry(deviceId, _consoleBridgeGeneration);
      },
      onDone: () {
        if (!_isCurrentConsoleBridge(deviceId, generation)) {
          return;
        }
        unawaited(_invalidateConsoleBridge()?.cancel());
        _scheduleConsoleBridgeRetry(deviceId, _consoleBridgeGeneration);
      },
    );
  }

  void _scheduleConsoleBridgeRetry(String deviceId, int generation) {
    if (!_isCurrentConsoleBridge(deviceId, generation)) {
      return;
    }

    _consoleRetryTimer?.cancel();
    _consoleRetryTimer = Timer(_consoleBridgeRetryDelay, () {
      if (!_isCurrentConsoleBridge(deviceId, generation)) {
        return;
      }
      _startConsoleMirror(deviceId);
    });
  }

  void _startSettingsRwBridge(String deviceId) {
    if (!productReady) return;
    if ((_capabilityDiscoveryByDevice.containsKey(deviceId) &&
            !deviceCapabilities(deviceId).explicitlyReported) ||
        deviceCapabilities(deviceId).recoveryOnly) {
      return;
    }
    if (!_isCurrentSettingsSession(deviceId, _settingsSessionGeneration) ||
        !_shouldRunConnectedBridges()) {
      return;
    }
    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.settingsService,
      characteristicId: BleProtocol.settingsReadWriteCharacteristic,
      deviceId: deviceId,
    );

    final int sessionGeneration = _settingsSessionGeneration;
    final int bridgeGeneration = ++_settingsBridgeGeneration;
    final StreamSubscription<List<int>>? oldSubscription = _settingsRwSub;
    _settingsRwSub = null;
    unawaited(oldSubscription?.cancel());
    _settingsRwRetryTimer?.cancel();
    _settingsRwRetryTimer = null;
    // Capability discovery may already have consumed the connection's initial
    // write turn. The newly installed response bridge still needs that same
    // native registration boundary before its first request.
    _firstGattWriteTurn = null;
    _settingsRwSub = _subscribeToCharacteristic(characteristic).listen(
      (List<int> packet) {
        if (!_isCurrentSettingsBridge(
          deviceId,
          sessionGeneration,
          bridgeGeneration,
        )) {
          return;
        }
        final String text = utf8.decode(packet, allowMalformed: true);
        final Object? decoded = _tryDecodeJson(text);
        if (decoded is Map<String, Object?> &&
            _handleCorrelatedMutationPayload(deviceId, decoded)) {
          return;
        }
        final Map<String, Object?>? payload =
            decoded is List<Object?> && decoded.isNotEmpty
            ? <String, Object?>{
                'i': decoded[0],
                if (decoded.length >= 2) 'v': decoded[1] else 'r': 1,
              }
            : decoded is Map<String, Object?>
            ? decoded
            : null;
        if (payload == null) {
          _logSettingsDiagThrottled(
            token: 'notify_decode_drop',
            line: 'settings notify dropped: unrecognized payload shape',
          );
          return;
        }
        _handleSettingsRwPayload(deviceId, payload);
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!_isCurrentSettingsBridge(
          deviceId,
          sessionGeneration,
          bridgeGeneration,
        )) {
          return;
        }
        _settingsRwSub = null;
        _failPendingSettings(LitchSettingWriteFailure.bridgeUnavailable);
        _pushAppLog('Settings bridge unavailable: $error');
        _scheduleSettingsRwBridgeRetry(
          deviceId,
          sessionGeneration,
          bridgeGeneration,
        );
      },
      onDone: () {
        if (!_isCurrentSettingsBridge(
          deviceId,
          sessionGeneration,
          bridgeGeneration,
        )) {
          return;
        }
        _settingsRwSub = null;
        _failPendingSettings(LitchSettingWriteFailure.bridgeUnavailable);
        _scheduleSettingsRwBridgeRetry(
          deviceId,
          sessionGeneration,
          bridgeGeneration,
        );
      },
    );
  }

  bool _handleCorrelatedMutationPayload(
    String deviceId,
    Map<String, Object?> payload,
  ) {
    if (payload.containsKey('x') ||
        payload.containsKey('a') ||
        payload.containsKey('s')) {
      // Action replies can share an ID with a direct Settings read. Only the
      // Ordered session owns actions, so keep these out of Settings matching.
      return true;
    }

    if (payload.containsKey('y') ||
        (payload.containsKey('k') && payload.containsKey('ok'))) {
      final LitchSettingOutcome? outcome =
          LitchControlProtocol.decodeSettingOutcome(payload);
      if (outcome != null) {
        _handleSettingOutcome(deviceId, outcome);
      } else {
        _failMalformedSettingAcknowledgement(deviceId, payload);
      }
      return true;
    }
    return false;
  }

  void _handleSettingOutcome(String deviceId, LitchSettingOutcome outcome) {
    _PendingSettingTransaction? pending;
    final int? requestId = outcome.requestId;
    if (requestId != null) {
      pending = _pendingSettingsById[requestId];
      if (pending == null ||
          pending.deviceId != deviceId ||
          pending.sessionGeneration != _settingsSessionGeneration) {
        return;
      }
    } else {
      final String? key = outcome.key;
      final int? pendingRequestId = key == null
          ? null
          : _pendingSettingIdsByKey[key];
      if (pendingRequestId != null) {
        pending = _pendingSettingsById[pendingRequestId];
      }
      if (pending != null &&
          (pending.requiresExactCorrelation ||
              pending.deviceId != deviceId ||
              pending.sessionGeneration != _settingsSessionGeneration)) {
        return;
      }
    }

    final String key = outcome.key ?? pending?.key ?? '';
    if (pending != null && key.isNotEmpty && pending.key != key) {
      return;
    }
    if (!hasBoardStateSnapshot &&
        outcome.hasValue &&
        outcome.value != null &&
        key.isNotEmpty) {
      _latestSettingValues[key] = outcome.value!;
    }

    final bool wasRead = pending?.kind == _PendingSettingKind.read;
    final bool successfulValuelessWrite =
        outcome.ok &&
        !outcome.hasValue &&
        pending?.kind == _PendingSettingKind.write;
    if (!hasBoardStateSnapshot && successfulValuelessWrite && key.isNotEmpty) {
      _latestSettingValues.remove(key);
    }
    if (pending != null) {
      if (pending.readObservation case final observation?) {
        observation.optionalMissing =
            outcome.reason == LitchMutationReason.unknownKey;
      }
      final Future<bool>? confirmation =
          hasBoardStateSnapshot &&
              outcome.ok &&
              pending.kind == _PendingSettingKind.write &&
              _latestSettingValues.containsKey(key)
          ? _queueBulkConfirmation(deviceId, key)
          : null;
      final Object? completion = pending.kind == _PendingSettingKind.write
          ? LitchSettingWriteAcknowledged(
              outcome,
              boardStateConfirmation: confirmation,
            )
          : hasBoardStateSnapshot
          ? _latestSettingValues[key]
          : outcome.ok && outcome.hasValue
          ? outcome.value
          : null;
      _completePendingSetting(pending.requestId, completion);
    }
    if (!outcome.ok || wasRead || key.isEmpty) {
      return;
    }
    if (!hasBoardStateSnapshot) _emitSettingsChanged(key);
  }

  void _emitControlEvent(LitchControlEvent event) {
    if (!_controlActionController.isClosed) {
      _controlActionController.add(event);
    }
  }

  void _replacePendingSettingForKey(String key) {
    final int? previousRequestId = _pendingSettingIdsByKey[key];
    if (previousRequestId == null) {
      return;
    }
    final _PendingSettingTransaction? previous =
        _pendingSettingsById[previousRequestId];
    if (previous?.readObservation != null) {
      _settingReadDiagnostics.directSuperseded();
    }
    _completePendingSetting(
      previousRequestId,
      previous?.kind == _PendingSettingKind.write
          ? const LitchSettingWriteNotAcknowledged(
              LitchSettingWriteFailure.requestReplaced,
            )
          : null,
    );
  }

  void _completePendingSetting(int requestId, Object? value) {
    final _PendingSettingTransaction? pending = _pendingSettingsById.remove(
      requestId,
    );
    if (pending == null) {
      return;
    }
    if (_pendingSettingIdsByKey[pending.key] == requestId) {
      _pendingSettingIdsByKey.remove(pending.key);
    }
    _requestIds.release(requestId);
    if (!pending.completer.isCompleted) {
      pending.completer.complete(value);
    }
    _retireDirectSettingsBridgeIfIdle();
  }

  void _retireDirectSettingsBridgeIfIdle() {
    if (_activeConnectionSession?.isActivated != true ||
        _pendingSettingsById.isNotEmpty) {
      return;
    }
    _settingsRwRetryTimer?.cancel();
    _settingsRwRetryTimer = null;
    final StreamSubscription<List<int>>? subscription = _settingsRwSub;
    if (subscription == null) return;
    ++_settingsBridgeGeneration;
    _settingsRwSub = null;
    unawaited(
      _safeCancelSubscription(
        subscription,
        'direct settings subscription retirement failed',
      ),
    );
  }

  void _failPendingSettings([
    LitchSettingWriteFailure failure = LitchSettingWriteFailure.interrupted,
  ]) {
    final List<int> requestIds = _pendingSettingsById.keys.toList();
    for (final int requestId in requestIds) {
      final _PendingSettingTransaction? pending =
          _pendingSettingsById[requestId];
      _completePendingSetting(
        requestId,
        pending?.kind == _PendingSettingKind.write
            ? LitchSettingWriteNotAcknowledged(failure)
            : null,
      );
    }
  }

  void _failMalformedSettingAcknowledgement(
    String deviceId,
    Map<String, Object?> payload,
  ) {
    final Object? compact = payload['y'];
    final Object? rawRequestId = compact is List<Object?> && compact.isNotEmpty
        ? compact.first
        : payload['i'];
    if (rawRequestId is! int) {
      return;
    }
    final _PendingSettingTransaction? pending =
        _pendingSettingsById[rawRequestId];
    if (pending == null ||
        pending.deviceId != deviceId ||
        pending.sessionGeneration != _settingsSessionGeneration) {
      return;
    }
    _completePendingSetting(
      pending.requestId,
      pending.kind == _PendingSettingKind.write
          ? const LitchSettingWriteNotAcknowledged(
              LitchSettingWriteFailure.malformedAcknowledgement,
            )
          : null,
    );
  }

  String _resolveSettingKey(Map<String, Object?> payload) {
    final String key = (payload['k'] ?? '').toString();
    if (key.isNotEmpty) {
      return key;
    }

    final int? settingIndex = _toInt(payload['i'] ?? payload['index']);
    if (settingIndex == null) {
      return '';
    }

    return _settingsKeysByIndex[settingIndex] ?? '';
  }

  void _handleSettingsRwPayload(String deviceId, Map<String, Object?> payload) {
    final String key = _resolveSettingKey(payload);
    if (hasBoardStateSnapshot) {
      _scheduleBulkRefresh(deviceId);
      return;
    }
    final bool hasValue = payload.containsKey('v');
    final Object? value = payload['v'];
    final bool requiresRefresh = (payload['r'] == 1 || payload['r'] == true);

    if (key.isEmpty) {
      final int? settingIndex = _toInt(payload['i'] ?? payload['index']);
      if (settingIndex != null && (hasValue || requiresRefresh)) {
        _pendingSettingsNotifyPayloadsByIndex[settingIndex] =
            Map<String, Object?>.from(payload);
        _logSettingsDiagThrottled(
          token: 'notify_unresolved_index:$settingIndex',
          line:
              'settings notify unresolved index=$settingIndex; key map not ready or stale',
        );
        _scheduleSettingsIndexMapRefresh(deviceId);
      }
      return;
    }

    if (hasValue && value != null) {
      final notificationVersion = ++_settingNotificationVersion;
      _settingNotificationVersions[key] = notificationVersion;
      _latestSettingValues[key] = value;
      final canonicalPaletteKey = switch (key) {
        _p1PieceWheelKey => _p1EndPieceWheelKey,
        _p2PieceWheelKey => _p2EndPieceWheelKey,
        _ => null,
      };
      final wheel = _pieceWheelValue(value);
      if (canonicalPaletteKey != null &&
          wheel != null &&
          _latestSettingValues.containsKey(canonicalPaletteKey)) {
        // These notifications are same-numbered end aliases. Keep an already
        // known canonical value current without inferring direct-key support.
        _latestSettingValues[canonicalPaletteKey] = wheel;
        _settingNotificationVersions[canonicalPaletteKey] = notificationVersion;
      }
    }

    if (requiresRefresh &&
        (!hasValue || value == null) &&
        _cpuRoleSettingKeys.contains(key)) {
      // A refresh-only report cannot certify the previous role. Complete
      // enumeration and value-bearing reports can then restore its authority.
      _latestSettingValues.remove(key);
    }

    if (requiresRefresh) {
      _logSettingsDiagThrottled(
        token: 'notify_refresh:$key',
        line:
            'settings notify refresh path for key=$key (value omitted by board)',
      );
    }
    if (hasValue || requiresRefresh) {
      _emitSettingsChanged(key);
    }
  }

  void _replayPendingSettingsNotifyPayloads(String deviceId) {
    if (_pendingSettingsNotifyPayloadsByIndex.isEmpty) {
      return;
    }

    final List<int> resolvedIndices = <int>[];
    for (final MapEntry<int, Map<String, Object?>> entry
        in _pendingSettingsNotifyPayloadsByIndex.entries) {
      final String? key = _settingsKeysByIndex[entry.key];
      if (key == null || key.isEmpty) {
        continue;
      }

      final Map<String, Object?> replayPayload = Map<String, Object?>.from(
        entry.value,
      );
      replayPayload['k'] = key;
      _handleSettingsRwPayload(deviceId, replayPayload);
      resolvedIndices.add(entry.key);
    }

    for (final int resolvedIndex in resolvedIndices) {
      _pendingSettingsNotifyPayloadsByIndex.remove(resolvedIndex);
    }
  }

  @visibleForTesting
  void debugHandleSettingsRwPayload(
    String deviceId,
    Map<String, Object?> payload,
  ) {
    if (!_handleCorrelatedMutationPayload(deviceId, payload)) {
      _handleSettingsRwPayload(deviceId, payload);
    }
  }

  @visibleForTesting
  void debugSetSettingsIndexMap(
    Map<int, String> keysByIndex, {
    String deviceId = '',
  }) {
    _settingsKeysByIndex
      ..clear()
      ..addAll(keysByIndex);
    _replayPendingSettingsNotifyPayloads(deviceId);
  }

  void _scheduleSettingsRwBridgeRetry(
    String deviceId,
    int sessionGeneration,
    int bridgeGeneration,
  ) {
    if (!_isCurrentSettingsBridge(
      deviceId,
      sessionGeneration,
      bridgeGeneration,
    )) {
      return;
    }

    _settingsRwRetryTimer?.cancel();
    _settingsRwRetryTimer = Timer(const Duration(milliseconds: 500), () {
      if (!_isCurrentSettingsBridge(
        deviceId,
        sessionGeneration,
        bridgeGeneration,
      )) {
        return;
      }
      _startSettingsRwBridge(deviceId);
    });
  }

  void _scheduleSettingsIndexMapRefresh(String deviceId) {
    if (_settingsIndexRefreshInFlight) {
      return;
    }
    if (!isConnected || connectedId != deviceId) {
      return;
    }

    _settingsIndexRefreshInFlight = true;
    final int sessionGeneration = _settingsSessionGeneration;
    unawaited(
      readSettings(deviceId)
          .then((List<LitchSetting> settings) {
            if (settings.isEmpty) {
              _logSettingsDiagThrottled(
                token: 'index_refresh_empty',
                line:
                    'settings index map refresh returned no settings; index-based notify may remain unresolved',
              );
            }
          })
          .catchError((Object error) {
            _logSettingsDiagThrottled(
              token: 'index_refresh_error',
              line: 'settings index map refresh failed: $error',
            );
          })
          .whenComplete(() {
            if (_isCurrentSettingsSession(deviceId, sessionGeneration)) {
              _settingsIndexRefreshInFlight = false;
            }
          }),
    );
  }

  LitchOtaControlChannel _otaControlChannelFor(String deviceId) {
    final LitchOtaControlChannel? existing = _otaControlChannel;
    if (existing != null &&
        existing.deviceId == deviceId &&
        existing.connectionToken == _connectionGeneration) {
      return existing;
    }

    final StreamSubscription<OtaStatusEvent>? oldBridge = _otaStatusSub;
    _otaStatusSub = null;
    unawaited(oldBridge?.cancel());
    unawaited(existing?.dispose());

    final QualifiedCharacteristic characteristic = QualifiedCharacteristic(
      serviceId: BleProtocol.baseService,
      characteristicId: BleProtocol.otaControlCharacteristic,
      deviceId: deviceId,
    );
    final LitchOtaControlChannel channel = LitchOtaControlChannel(
      deviceId: deviceId,
      connectionToken: _connectionGeneration,
      generation: ++_otaControlGeneration,
      subscribe: () => _subscribeToCharacteristic(characteristic),
    );
    _otaControlChannel = channel;
    final session = _activeConnectionSession;
    if (session?.isActivated == true) _bindSharedOtaStatus(session!);
    return channel;
  }

  void _bindSharedOtaStatus(ConnectionSession session) {
    unawaited(_otaSharedStatusSub?.cancel());
    _otaSharedStatusSub = null;
    final LitchOtaControlChannel? channel = _otaControlChannel;
    if (channel == null) return;
    unawaited(
      channel.retireDirectPackets().catchError((Object error) {
        _pushAppLog('Bootstrap OTA status retirement failed: $error');
      }),
    );
    final ConnectionOtaStatusAssembler assembler =
        ConnectionOtaStatusAssembler();
    _otaSharedStatusSub = session.objectReceipts
        .where(
          (packet) => packet.code == ConnectionSession.otaStatusReceiptCode,
        )
        .listen(
          (packet) {
            if (!identical(session, _activeConnectionSession) ||
                !identical(channel, _otaControlChannel)) {
              return;
            }
            try {
              final Uint8List? status = assembler.accept(packet.body);
              if (status != null) channel.acceptPacket(status);
            } on FormatException catch (error) {
              _pushAppLog('Ignoring malformed shared OTA status: $error');
            }
          },
          onDone: () {
            if (identical(session, _activeConnectionSession) &&
                identical(channel, _otaControlChannel)) {
              _invalidateOtaControlSession();
            }
          },
        );
  }

  Future<LitchOtaControlChannel> _ensureOtaControlChannel(
    String deviceId,
  ) async {
    final LitchOtaControlChannel channel = _otaControlChannelFor(deviceId);
    await _untilConnectionEnds(channel.start());
    return channel;
  }

  Future<void> _disposeOtaControlChannel() async {
    final LitchOtaControlChannel? channel = _otaControlChannel;
    _otaControlChannel = null;
    final StreamSubscription<ConnectionSessionPacket>? sharedStatusSub =
        _otaSharedStatusSub;
    _otaSharedStatusSub = null;
    await _safeCancelSubscription(
      sharedStatusSub,
      'shared OTA status cancellation failed',
    );
    await channel?.dispose();
  }

  void _invalidateOtaControlSession() {
    final StreamSubscription<OtaStatusEvent>? bridge = _otaStatusSub;
    final LitchOtaControlChannel? channel = _otaControlChannel;
    _otaStatusSub = null;
    _otaControlChannel = null;
    unawaited(_otaSharedStatusSub?.cancel());
    _otaSharedStatusSub = null;
    _clearConnectionScopedCapabilities();
    unawaited(
      Future<void>.microtask(() async {
        await _safeCancelSubscription(
          bridge,
          'OTA status subscription cancellation failed',
        );
        await channel?.dispose();
      }),
    );
  }

  void _clearConnectionScopedCapabilities() {
    _capabilitiesByDevice.clear();
    _capabilityDiscoveryByDevice.clear();
  }

  bool _ownsConnectionReservation(int generation) {
    return !_disposed && generation == _connectionGeneration;
  }

  void _emitConnectionUpdate(ConnectionStateUpdate update) {
    _persistConsoleSnapshot();
    if (!_connectionController.isClosed) {
      _connectionController.add(update);
    }
  }

  void _recordUnexpectedDisconnect(String deviceId, int generation) {
    if (!_ownsConnectionReservation(generation) ||
        !isConnected ||
        connectedId != deviceId) {
      return;
    }
    final ConnectionStateUpdate update = ConnectionStateUpdate(
      deviceId: deviceId,
      connectionState: DeviceConnectionState.disconnected,
      failure: null,
    );
    _clearRetainedHomeState();
    _connectionState = update;
    _setConsoleTraceOutputEnabled(false);
    _invalidateGattSession();
    if (!identifyConnectionActive) _emitConnectionUpdate(update);
    _failPendingSettings(LitchSettingWriteFailure.disconnected);
    unawaited(
      _safeCancelSubscription(
        _invalidateConsoleBridge(),
        'console subscription unexpected disconnect cleanup failed',
      ),
    );
    unawaited(
      _safeCancelSubscription(
        _invalidateSettingsSession(clearCachedState: true),
        'settings subscription unexpected disconnect cleanup failed',
      ),
    );
    _knownControlProtocolDevices.remove(deviceId);
    _unavailableControlProtocolDevices.remove(deviceId);
    _stopKeepAlive();
    _invalidateOtaControlSession();
    if (generation == _connectionGeneration) {
      _retireConnection();
    }
  }

  void _startOtaStatusBridge(String deviceId) {
    if (!productReady) return;
    final LitchOtaControlChannel channel = _otaControlChannelFor(deviceId);
    unawaited(
      channel.start().catchError((Object error) {
        _pushAppLog('OTA status bridge unavailable: $error');
      }),
    );

    _otaStatusSub?.cancel();
    _otaStatusSub = channel.events.listen(
      (OtaStatusEvent event) {
        if (event.generation != channel.generation ||
            event.deviceId != deviceId) {
          return;
        }
        if (event.kind == OtaStatusKind.bluetoothSuffix ||
            event.kind == OtaStatusKind.runtimeHealth ||
            event.kind == OtaStatusKind.runtimeHealthMalformed ||
            event.kind == OtaStatusKind.runtimeHealthRecheckSucceeded ||
            event.kind == OtaStatusKind.runtimeHealthRecheckFailed ||
            event.kind == OtaStatusKind.runtimeHealthRecheckMalformed) {
          return;
        }
        if (event.kind == OtaStatusKind.error &&
            event.detail == 'unknown_cmd') {
          return;
        }
        final String summary = event.summary;
        if (!_otaStatusController.isClosed) {
          _otaStatusController.add(summary);
        }
        _pushAppLog('BLE status $summary');
      },
      onError: (Object error, StackTrace stackTrace) {
        _pushAppLog('OTA status bridge unavailable: $error');
      },
    );
  }

  void _appendConsolePacket(List<int> packet) {
    if (packet.isEmpty) {
      return;
    }

    var linesChanged = false;
    for (final int byte in packet) {
      final int code = byte & 0xFF;

      if (_pendingCarriageReturn) {
        if (code == 0x0A) {
          linesChanged = _flushCurrentLine() || linesChanged;
          _pendingCarriageReturn = false;
          continue;
        }

        _clearCurrentLine();
        _pendingCarriageReturn = false;
      }

      if (code == 0x0D) {
        _pendingCarriageReturn = true;
        continue;
      }

      if (code == 0x08 || code == 0x7F) {
        _backspaceCurrentLine();
        continue;
      }

      if (code == 0x0A) {
        linesChanged = _flushCurrentLine() || linesChanged;
        continue;
      }

      if (code >= 0x20 || code == 0x09) {
        if (_consoleCurrentLineOverflowed) {
          continue;
        }
        if (_consoleCurrentLine.length >= _consoleLineCapacity) {
          _consoleCurrentLine.clear();
          _consoleCurrentLineOverflowed = true;
          continue;
        }
        _consoleCurrentLine.writeCharCode(code);
      }
    }

    if (linesChanged) {
      _emitConsoleSnapshot();
    }
  }

  void _backspaceCurrentLine() {
    if (_consoleCurrentLineOverflowed) {
      return;
    }
    final String current = _consoleCurrentLine.toString();
    if (current.isEmpty) {
      return;
    }
    _consoleCurrentLine
      ..clear()
      ..write(current.substring(0, current.length - 1));
  }

  void _clearCurrentLine() {
    _consoleCurrentLineOverflowed = false;
    if (_consoleCurrentLine.isNotEmpty) {
      _consoleCurrentLine.clear();
    }
  }

  bool _flushCurrentLine() {
    if (_consoleCurrentLineOverflowed) {
      _clearCurrentLine();
      return false;
    }
    final String line = _consoleCurrentLine.toString();
    _consoleCurrentLine.clear();
    if (line.isNotEmpty) {
      // The assembler stores bytes until a complete record arrives. Decode
      // once so UTF-8 may span notifications, and preserve response text rather
      // than guessing command echoes from its words or recent submissions.
      final String normalized = utf8.decode(
        line.codeUnits,
        allowMalformed: true,
      );
      if (normalized.trim().isEmpty) {
        return false;
      }
      final BoardHealthStatus? health = BoardHealthStatus.tryParse(normalized);
      if (health != null) {
        _latestBoardHealth = health;
        _boardHealthController.add(health);
      }
      final SupportIncident? incident = SupportIncident.tryParse(normalized);
      if (incident != null) _incidentController.add(incident);
      // Home authority arrives only through the typed State stream.
      if (!_consoleTraceOutputEnabled && _isTraceConsoleLine(normalized)) {
        return false;
      }

      final String formatted = _formatFirmwareLine(normalized);
      _consoleLines.add(formatted);
      _emitHostLog(formatted);
      if (_consoleLines.length > _consoleHistoryCapacity) {
        _consoleLines.removeAt(0);
      }
      return true;
    }
    return false;
  }

  void _persistConsoleSnapshot() {
    try {
      _consoleSnapshotSink?.captureConsole(
        lines: _consoleLines,
        connectionGeneration: _connectionGeneration,
        connected: !_disposed && isConnected,
        connectedBoardId: !_disposed && isConnected ? connectedId : null,
        historyBoardId: _consoleHistoryDeviceId,
        readiness: AppConsoleReadiness(
          phase: _disposed
              ? AppConsolePreparationPhase.disconnected
              : _connectAttemptInProgress
              ? AppConsolePreparationPhase.connecting
              : !isConnected
              ? AppConsolePreparationPhase.disconnected
              : productReady
              ? AppConsolePreparationPhase.ready
              : _productPreparationFailed
              ? AppConsolePreparationPhase.failed
              : _ownerPairingSession != null
              ? AppConsolePreparationPhase.confirmingOwner
              : AppConsolePreparationPhase.discovering,
          productReady: !_disposed && productReady,
          sessionActivated:
              !_disposed &&
              isConnected &&
              _activeConnectionSession?.isActivated == true,
          owner: !_disposed && isConnected
              ? _ownerPairingSession?.status
              : null,
        ),
      );
    } catch (_) {
      /* Local observations never affect BLE or Console delivery. */
    }
  }

  void _emitConsoleSnapshot() {
    _persistConsoleSnapshot();
    if (_consoleController.isClosed) {
      return;
    }
    final List<String> snapshot = List<String>.from(_consoleLines);
    _consoleController.add(snapshot);
  }

  LitchSetting _rebaseSettingOnLatestValue(LitchSetting setting) =>
      LitchSetting(
        key: setting.key,
        type: setting.type,
        category: setting.category,
        value: _latestSettingValues[setting.key] ?? setting.value,
        defaultValue: setting.defaultValue,
        min: setting.min,
        max: setting.max,
        options: setting.options,
        description: setting.description,
      );

  void _cancelBulkConfirmations() {
    _bulkReceiptTimer?.cancel();
    _bulkReceiptTimer = null;
    _bulkConfirmationDeadline?.cancel();
    _bulkConfirmationDeadline = null;
    _bulkStateQuery = null;
    for (final receipt in _bulkWriteReceiptsPending.values) {
      if (!receipt.isCompleted) receipt.complete(false);
    }
    _bulkWriteReceiptsPending.clear();
  }

  Future<bool> _queueBulkConfirmation(String deviceId, String key) {
    final previous = _bulkWriteReceiptsPending.remove(key);
    if (previous != null && !previous.isCompleted) previous.complete(false);
    if (_bulkWriteReceiptsPending.isEmpty) {
      _bulkConfirmationDeadline?.cancel();
      _bulkConfirmationDeadline = Timer(const Duration(seconds: 4), () {
        _pushAppLog(
          'Setting state confirmation exhausted; retaining last reported state.',
        );
        _cancelBulkConfirmations();
      });
    }
    final receipt = Completer<bool>();
    _bulkWriteReceiptsPending[key] = receipt;
    _scheduleBulkConfirmation(deviceId);
    return receipt.future;
  }

  void _scheduleBulkConfirmation(String deviceId) {
    _bulkReceiptTimer?.cancel();
    _bulkReceiptTimer = Timer(Duration.zero, () {
      if (_bulkWriteReceiptsPending.isEmpty ||
          !isConnected ||
          connectedId != deviceId) {
        return;
      }
      final query = _BulkStateQuery(
        _nextBulkReceipt,
        _settingsSessionGeneration,
        Map.of(_bulkWriteReceiptsPending),
      );
      _nextBulkReceipt = (_nextBulkReceipt + 1) & 0xffffffff;
      if (_nextBulkReceipt == 0) _nextBulkReceipt = 1;
      _bulkStateQuery = query;
      unawaited(_sendBulkConfirmation(deviceId, query));
    });
  }

  Future<void> _sendBulkConfirmation(
    String deviceId,
    _BulkStateQuery query,
  ) async {
    if (_bulkStateQuery != query ||
        !_isCurrentSettingsSession(deviceId, query.generation)) {
      return;
    }
    ++query.attempts;
    _bulkReceiptTimer?.cancel();
    _bulkReceiptTimer = Timer(const Duration(seconds: 2), () {
      if (_bulkStateQuery != query) return;
      if (query.attempts < 2) {
        unawaited(_sendBulkConfirmation(deviceId, query));
      } else {
        _pushAppLog(
          'Setting state confirmation exhausted; retaining last reported state.',
        );
        _finishBulkConfirmation(query, false);
      }
    });
    try {
      await _writeCharacteristic(
        QualifiedCharacteristic(
          serviceId: BleProtocol.baseService,
          characteristicId: BleProtocol.commandCharacteristic,
          deviceId: deviceId,
        ),
        value: utf8.encode(jsonEncode({'h': query.nonce})),
        withResponse: true,
        isCurrent: () =>
            _bulkStateQuery == query &&
            _isCurrentSettingsSession(deviceId, query.generation),
      );
    } catch (error) {
      // A matching receipt may retire this query before native write completion.
      // Its post-await ownership check then throws despite successful delivery.
      if (_bulkStateQuery != query ||
          !_isCurrentSettingsSession(deviceId, query.generation)) {
        return;
      }
      _pushAppLog(
        'Setting state read could not be sent; bounded retry remains: $error',
      );
    }
  }

  void _finishBulkConfirmation(_BulkStateQuery query, bool confirmed) {
    if (_bulkStateQuery != query) return;
    _bulkStateQuery = null;
    _bulkReceiptTimer?.cancel();
    _bulkReceiptTimer = null;
    for (final entry in query.receipts.entries) {
      if (identical(_bulkWriteReceiptsPending[entry.key], entry.value)) {
        _bulkWriteReceiptsPending.remove(entry.key);
        if (!entry.value.isCompleted) entry.value.complete(confirmed);
      }
    }
    if (_bulkWriteReceiptsPending.isEmpty) {
      _bulkConfirmationDeadline?.cancel();
      _bulkConfirmationDeadline = null;
    } else if (connectedId case final String deviceId) {
      _scheduleBulkConfirmation(deviceId);
    }
  }

  void _scheduleBulkRefresh(String deviceId) {
    _bulkRefreshPending = true;
    if (_bulkRefreshTimer != null) return;
    final int generation = _settingsSessionGeneration;
    // Firmware normally pushes the coalesced envelope itself. Give a minimum
    // MTU record time to finish before one idempotent fallback request.
    _bulkRefreshTimer = Timer(const Duration(seconds: 2), () {
      _bulkRefreshTimer = null;
      if (_bulkRefreshPending &&
          _isCurrentSettingsSession(deviceId, generation)) {
        unawaited(requestCurrentHomeStateSnapshot(deviceId));
      }
    });
  }

  void _clearRetainedHomeState() {
    _latestBoardHealth = null;
    _latestHomeState = null;
    _binarySettingsStateObserved = false;
    _connectionStateRecovery = null;
    _connectionHomeState.clear();
    _connectionSettingsState.clear();
  }

  void _pushConsoleLine(String line) {
    if (line.isEmpty || _consoleController.isClosed) {
      return;
    }

    _consoleLines.add(line);
    if (_consoleLines.length > _consoleHistoryCapacity) {
      _consoleLines.removeAt(0);
    }
    _emitConsoleSnapshot();
  }

  void _pushAppLog(String line) {
    if (line.isEmpty) {
      return;
    }

    final String formatted = _formatAppLine(line);
    _pushConsoleLine(formatted);
    _emitHostLog(formatted);
  }

  void _emitSettingsChanged(String key) {
    if (!_settingsChangedController.isClosed) {
      _settingsChangedController.add(key);
    }
  }

  void _emitHostLog(String line) {
    if (line.isEmpty) {
      return;
    }

    debugPrint(line);
  }

  bool _isTraceConsoleLine(String line) {
    final String trimmed = line.trimLeft();
    return trimmed.startsWith('{"type":"') || line.contains(' [TRACE] ');
  }

  bool _isRetainedTraceConsoleLine(String formattedLine) {
    const String firmwareMarker = '[FW] ';
    final int markerIndex = formattedLine.indexOf(firmwareMarker);
    final String rawLine = markerIndex < 0
        ? formattedLine
        : formattedLine.substring(markerIndex + firmwareMarker.length);
    return _isTraceConsoleLine(rawLine);
  }

  String _formatAppLine(String line) {
    return '${_timestampNow()} $line';
  }

  String _formatFirmwareLine(String line) {
    return '${_timestampNow()} [FW] $line';
  }

  String _timestampNow() {
    final DateTime now = DateTime.now();
    final String hour = now.hour.toString().padLeft(2, '0');
    final String minute = now.minute.toString().padLeft(2, '0');
    final String second = now.second.toString().padLeft(2, '0');
    final String millis = now.millisecond.toString().padLeft(3, '0');
    return '$hour:$minute:$second.$millis';
  }

  // Typed metadata for settings carried by the current shared session.
  static const List<LitchSetting> _typedSettingDescriptors = <LitchSetting>[
    LitchSetting(
      key: 'BOOT_DISPLAY',
      type: 'enum',
      category: 'animation',
      value: 'fox',
      defaultValue: 'fox',
      options: ['fox', 'artwork', 'off'],
    ),
    LitchSetting(
      key: 'P1_LEGAL_MOVES',
      type: 'bool',
      category: 'behavior',
      value: true,
      defaultValue: true,
    ),
    LitchSetting(
      key: 'P2_LEGAL_MOVES',
      type: 'bool',
      category: 'behavior',
      value: true,
      defaultValue: true,
    ),
    LitchSetting(
      key: 'P1_CHECK_CUE',
      type: 'bool',
      category: 'behavior',
      value: true,
      defaultValue: true,
    ),
    LitchSetting(
      key: 'P2_CHECK_CUE',
      type: 'bool',
      category: 'behavior',
      value: true,
      defaultValue: true,
    ),
    LitchSetting(
      key: 'P1_AUTO_HINTS',
      type: 'bool',
      category: 'behavior',
      value: false,
      defaultValue: false,
    ),
    LitchSetting(
      key: 'P2_AUTO_HINTS',
      type: 'bool',
      category: 'behavior',
      value: false,
      defaultValue: false,
    ),
  ];

  static const List<LitchSetting> _quickSetupDescriptors = <LitchSetting>[
    LitchSetting(
      key: 'VS_CPU_LEVEL',
      type: 'u32',
      category: 'behavior',
      value: 0,
      defaultValue: 0,
      min: 0,
      max: 11,
    ),
    LitchSetting(
      key: 'VS_CPU_PLAYER',
      type: 'u32',
      category: 'behavior',
      value: 2,
      defaultValue: 2,
      min: 1,
      max: 2,
    ),
    LitchSetting(
      key: 'GAME_FIRST_PLAYER',
      type: 'u32',
      category: 'behavior',
      value: 1,
      defaultValue: 1,
      min: 1,
      max: 2,
    ),
    LitchSetting(
      key: 'P1_PIECE_WHEEL',
      type: 'u32',
      category: 'display',
      value: 170,
      defaultValue: 170,
      min: 0,
      max: 255,
    ),
    LitchSetting(
      key: 'P2_PIECE_WHEEL',
      type: 'u32',
      category: 'display',
      value: 0,
      defaultValue: 0,
      min: 0,
      max: 255,
    ),
    LitchSetting(
      key: 'WHITE_SQUARES_ON',
      type: 'bool',
      category: 'display',
      value: true,
      defaultValue: true,
    ),
    LitchSetting(
      key: 'SETTLE_ANIM_STYLE',
      type: 'enum',
      category: 'animation',
      value: 'ripple',
      defaultValue: 'ripple',
      options: <String>[
        'landing_flash',
        'mini_orbit',
        'ripple',
        'none',
        'rainbow_orbit',
      ],
    ),
    LitchSetting(
      key: 'BOARD_ROTATE',
      type: 'bool',
      category: 'behavior',
      value: false,
      defaultValue: false,
    ),
  ];

  Object _parseValueByType(String type, Object? value) {
    switch (type) {
      case 'bool':
        if (value is bool) {
          return value;
        }
        return value.toString().toLowerCase() == 'true' ||
            value.toString() == '1';
      case 'u8':
      case 'u32':
        return _toInt(value) ?? 0;
      case 'enum':
      case 'str':
      case 'string':
      default:
        return (value ?? '').toString();
    }
  }

  int? _toInt(Object? value) {
    if (value is int) {
      return value;
    }
    if (value is num) {
      return value.toInt();
    }
    return int.tryParse(value?.toString() ?? '');
  }

  int? _pieceWheelValue(Object? value) {
    final int? parsed = _toInt(value);
    return parsed != null && parsed >= 0 && parsed <= 255 ? parsed : null;
  }

  List<String> _toStringList(Object? value) {
    if (value is! List<Object?>) {
      return const <String>[];
    }
    return value
        .map((Object? item) => (item ?? '').toString())
        .where((String item) => item.isNotEmpty)
        .toList();
  }

  Object? _tryDecodeJson(String input) {
    try {
      return jsonDecode(input);
    } catch (_) {
      return null;
    }
  }
}
