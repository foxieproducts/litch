import 'dart:async';
import 'dart:io';

import 'package:package_info_plus/package_info_plus.dart';

import '../models/app_release_identity.dart';
import '../models/runtime_health_snapshot.dart';
import '../models/support_board_collection.dart';
import '../models/support_diagnostics.dart';
import '../models/support_report.dart';
import '../models/support_phone_diagnostics.dart';
import '../models/support_action.dart';
import 'litch_ble_service.dart';

typedef SupportAppInfoLoader = Future<SupportAppInfo> Function();

class SupportReportService {
  static const Duration _appInfoTimeout = Duration(seconds: 3);

  SupportReportService({
    this.bleService,
    this.boardId,
    SupportAppInfoLoader? loadAppInfo,
  }) : _loadAppInfo = loadAppInfo ?? _loadDefaultAppInfo;

  final LitchBleService? bleService;
  final String? boardId;
  final SupportAppInfoLoader _loadAppInfo;
  final Completer<void> _cancelled = Completer<void>();

  /// The screen owns collection, but cannot cancel an already-issued SDK write.
  /// The BLE owner retires report-local work and fences its late completion.
  void cancel() {
    if (!_cancelled.isCompleted) _cancelled.complete();
  }

  Future<SupportReportDraft> createDraft() async {
    // Start both optional phone reads together, not as stacked timeout windows.
    final LitchBleService? service = bleService;
    final String? targetId = boardId ?? service?.connectedId;
    final bool connected =
        service != null &&
        service.isConnected &&
        !service.identifyConnectionActive &&
        targetId != null &&
        service.connectedId == targetId;

    final int? connectionToken = service?.connectionSessionToken;
    // Snapshot the target before any platform await. The board budget runs in
    // parallel with phone metadata, not after another three-second window.
    final Future<SupportBoardCollection?> boardRead = connected
        ? _safeBoardDiagnostics(service, targetId, connectionToken!)
        : Future<SupportBoardCollection?>.value();
    final Future<SupportPhoneDiagnostics?> phoneRead = _safePhoneDiagnostics();
    final SupportAppInfo app = await _safeAppInfo();
    final SupportPhoneDiagnostics? phone = await phoneRead;
    final SupportBoardCollection? board = await boardRead;
    final bool connectionStillActive =
        connected &&
        service.isConnected &&
        !service.identifyConnectionActive &&
        service.connectedId == targetId &&
        service.connectionSessionToken == connectionToken;

    SupportAppActionSnapshot? appActions;
    String? ownerState, ownerReason;
    final recoveryOnly =
        board?.publicHealth.value?.recoveryMode == 1 ||
        board?.capabilities.value?.recoveryOnly == true;
    bool privateStillReady = false;
    try {
      privateStillReady =
          service?.productReady == true &&
          board?.publicHealth.value?.recoveryMode != 1 &&
          board?.capabilities.value?.recoveryOnly != true;
    } catch (_) {
      /* Unknown owner authority stays private. */
    }
    try {
      appActions = service?.supportActionSnapshot;
      ownerState = service?.ownerPairingStatus?.state.name;
      ownerReason = service?.ownerPairingStatus?.reason.name;
    } catch (_) {
      /* Optional diagnostics must never block the editable draft. */
    }
    return const SupportReportBuilder().build(
      app: app,
      phone: phone,
      appActions: appActions,
      board: SupportBoardInfo(
        connected: connectionStillActive,
        publicHealth: connectionStillActive ? board?.publicHealth.value : null,
        actionHistory: connectionStillActive && privateStillReady
            ? board?.actionHistory.value
            : null,
        ownerState: connectionStillActive ? ownerState : null,
        ownerReason: connectionStillActive ? ownerReason : null,
        firmwareVersion: connectionStillActive
            ? board?.firmwareVersion.value
            : null,
        capabilities: connectionStillActive ? board?.capabilities.value : null,
        runtimeHealth: connectionStillActive
            ? board?.runtimeHealth.value
            : const RuntimeHealthUnavailable(
                RuntimeHealthUnavailableReason.disconnected,
              ),
        incident: connectionStillActive && privateStillReady
            ? board?.incident.value
            : null,
        boardHealth: connectionStillActive && privateStillReady
            ? board?.boardHealth
            : null,
        firmwareDigest: connectionStillActive
            ? board?.firmwareDigest.value
            : null,
        queryOutcomes: <SupportQueryField, SupportQueryOutcome>{
          for (final field in SupportQueryField.values)
            field: !connectionStillActive && connected
                ? SupportQueryOutcome.sessionChanged
                : !privateStillReady &&
                      (field == SupportQueryField.actionHistory ||
                          field == SupportQueryField.incident)
                ? recoveryOnly
                      ? SupportQueryOutcome.unavailable
                      : SupportQueryOutcome.ownerNotReady
                : _queryOutcome(board, field),
        },
        collectionStatus: connectionStillActive
            ? 'current_session'
            : 'disconnected_or_session_changed',
      ),
      session:
          service?.supportSessionDiagnostics ??
          const LitchSupportSessionDiagnostics(
            settingsWriteAttempts: 0,
            settingsWriteSucceeded: 0,
            settingsWriteFailed: 0,
          ),
    );
  }

  Future<SupportBoardCollection?> _safeBoardDiagnostics(
    LitchBleService service,
    String targetId,
    int connectionToken,
  ) async {
    try {
      return await service.collectSupportBoardDiagnostics(
        deviceId: targetId,
        connectionSessionToken: connectionToken,
        cancelled: _cancelled.future,
      );
    } catch (_) {
      return null;
    }
  }

  SupportQueryOutcome _queryOutcome(
    SupportBoardCollection? board,
    SupportQueryField field,
  ) {
    if (board == null) return SupportQueryOutcome.unavailable;
    return switch (field) {
      SupportQueryField.publicHealth => board.publicHealth.outcome,
      SupportQueryField.actionHistory => board.actionHistory.outcome,
      SupportQueryField.incident => board.incident.outcome,
      SupportQueryField.firmwareDigest => board.firmwareDigest.outcome,
      SupportQueryField.capabilities => board.capabilities.outcome,
      SupportQueryField.runtimeHealth => board.runtimeHealth.outcome,
      SupportQueryField.firmwareVersion => board.firmwareVersion.outcome,
    };
  }

  Future<SupportPhoneDiagnostics?> _safePhoneDiagnostics() async {
    try {
      return await bleService?.collectSupportPhoneDiagnostics();
    } catch (_) {
      return null;
    }
  }

  Future<SupportAppInfo> _safeAppInfo() async {
    try {
      // Optional platform metadata must not prevent a user from asking for
      // help. Complete once; a late native reply cannot replace an edited draft.
      return await _loadAppInfo().timeout(_appInfoTimeout);
    } catch (_) {
      return SupportAppInfo(
        version: 'unavailable',
        buildNumber: 'unavailable',
        platform: Platform.operatingSystem,
        osVersion: Platform.operatingSystemVersion,
      );
    }
  }

  static Future<SupportAppInfo> _loadDefaultAppInfo() async {
    final PackageInfo package = await PackageInfo.fromPlatform();
    return SupportAppInfo(
      version: litchAppReleaseIdentity.versionFor(package),
      buildNumber: package.buildNumber,
      platform: Platform.operatingSystem,
      osVersion: Platform.operatingSystemVersion,
    );
  }
}
