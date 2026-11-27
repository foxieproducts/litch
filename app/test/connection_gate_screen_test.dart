import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/models/board_storage_recovery.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/firmware_provenance.dart';
import 'package:litch_mobile/src/models/firmware_release_snapshot.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/screens/app_chess_play_screen.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:litch_mobile/src/widgets/litch_workspace_geometry.dart';
import 'package:litch_mobile/src/widgets/orientation_workspace.dart';
import 'package:litch_mobile/src/screens/app_console_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/screens/fen_preview_screen.dart';
import 'package:litch_mobile/src/screens/game_library_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/widgets/recent_games_section.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/app_upgrade_notice_service.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_index.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/bluetooth_pairing_recovery.dart';
import 'package:litch_mobile/src/services/device_landscape_side.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';
// Hold the existing plugin read boundary; this adds no product dependency.
// ignore: depend_on_referenced_packages
import 'package:shared_preferences_platform_interface/shared_preferences_platform_interface.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';
import 'post_game_review_test_fixture.dart';
import 'support/litchcraft_frame_strip_gestures.dart';
import 'support/memory_ota_target_store.dart';
import 'support/firmware_test_fixture.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

Widget _withTestPreferences(Widget child, {bool showTips = true}) =>
    AppPreferencesScope(
      appearanceMode: LitchAppearanceMode.system,
      onAppearanceModeChanged: (_) async {},
      localePreference: const LitchLocalePreference.system(),
      onLocalePreferenceChanged: (_) async {},
      showTips: showTips,
      onShowTipsChanged: (_) async {},
      child: child,
    );

Finder _homeDetailsScrollable() => find.descendant(
  of: find.byKey(const ValueKey<String>('home-details-scroll')),
  matching: find.byWidgetPredicate(
    (widget) =>
        widget is Scrollable && widget.physics is AlwaysScrollableScrollPhysics,
  ),
);

void _usePortraitPhone(WidgetTester tester) {
  tester.view.physicalSize = const Size(390, 844);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

Finder _navigationAction(WidgetTester tester, String destination) {
  final suffix = destination == 'litchcraft' ? 'craft' : destination;
  for (final key in <String>[
    'main-destination-$suffix',
    'landscape-$suffix-action',
    'disconnected-destination-$suffix',
  ]) {
    final candidate = find.byKey(ValueKey<String>(key)).hitTestable();
    if (candidate.evaluate().isNotEmpty) return candidate;
  }
  final Finder glyph = switch (destination) {
    'play' => find.byType(LitchPlayGlyph),
    'litchcraft' => find.byType(LitchcraftIdentityGlyph),
    'setup' => find.byIcon(Icons.tune_outlined),
    'console' => find.byIcon(Icons.terminal),
    _ => find.byKey(ValueKey<String>('missing-$destination')),
  };
  final hitTestableGlyph = find
      .descendant(of: _activeNavigation(tester), matching: glyph)
      .hitTestable();
  if (hitTestableGlyph.evaluate().isNotEmpty) return hitTestableGlyph.first;
  final label = destination == 'litchcraft'
      ? 'Litchcraft'
      : '${destination[0].toUpperCase()}${destination.substring(1)}';
  final labelled = find
      .descendant(of: _activeNavigation(tester), matching: find.text(label))
      .hitTestable();
  if (labelled.evaluate().isNotEmpty) return labelled;
  throw StateError('No hit-testable $destination navigation action');
}

Finder _activeNavigation(WidgetTester tester) {
  final bar = find.byType(NavigationBar);
  if (bar.evaluate().isNotEmpty) return bar;
  final rail = find.byType(NavigationRail);
  if (rail.evaluate().isNotEmpty) return rail;
  final disconnected = find.byKey(const ValueKey('disconnected-navigation'));
  if (disconnected.evaluate().isNotEmpty) return disconnected;
  for (final key in <String>[
    'main-navigation-surface',
    'disconnected-navigation-surface',
  ]) {
    final surface = find.byKey(ValueKey(key));
    if (surface.evaluate().isNotEmpty) return surface;
  }
  return find.byKey(const ValueKey('landscape-navigation-switcher'));
}

Finder _landscapeNavigationSelections(WidgetTester tester) => find.descendant(
  of: _activeNavigation(tester),
  matching: find.byWidgetPredicate(
    (widget) => widget is Semantics && widget.properties.selected != null,
  ),
);

int _activeNavigationIndex(WidgetTester tester) {
  final navigation = _activeNavigation(tester);
  final widget = tester.widget(navigation);
  return switch (widget) {
    NavigationBar(:final selectedIndex) => selectedIndex,
    NavigationRail(:final selectedIndex) => selectedIndex ?? 0,
    _ =>
      tester
          .widgetList<Semantics>(_landscapeNavigationSelections(tester))
          .toList()
          .indexWhere((widget) => widget.properties.selected == true),
  };
}

int _activeNavigationDestinationCount(WidgetTester tester) {
  final widget = tester.widget(_activeNavigation(tester));
  return switch (widget) {
    NavigationBar(:final destinations) => destinations.length,
    NavigationRail(:final destinations) => destinations.length,
    _ => _landscapeNavigationSelections(tester).evaluate().length,
  };
}

class _RibbonCraftController extends _NavigationCraftController {
  _RibbonCraftController(super.bleService);
  int strokeEnds = 0;
  @override
  void endStroke() {
    strokeEnds++;
    super.endStroke();
  }
}

class _NavigationCraftController extends LitchcraftController {
  _NavigationCraftController(super.bleService, {super.initialCanvas});
  int enterCalls = 0;
  int adoptCalls = 0;
  bool? hasMarksAtAdopt;
  int leaveCalls = 0;
  int revalidateCalls = 0;
  int paintCalls = 0;
  Completer<void>? enterGate;
  Completer<void>? exitGate;
  Completer<void>? revalidateGate;
  bool rejectExit = false;
  bool retained = false;
  bool connectedBoardActive = false;
  bool failModeInspection = false;
  bool? _inspectedMode;
  int? _inspectedModeConnectionToken;
  String? _inspectedModeDeviceId;
  @override
  bool? get inspectedBoardActive =>
      bleService.isConnected &&
          _inspectedModeConnectionToken == bleService.connectionSessionToken &&
          _inspectedModeDeviceId == bleService.connectedId
      ? _inspectedMode
      : null;

  void showInspectedMode(bool active) {
    _inspectedMode = active;
    _inspectedModeConnectionToken = bleService.connectionSessionToken;
    _inspectedModeDeviceId = bleService.connectedId;
    notifyListeners();
  }

  bool transientSelectionDuringAdopt = false;
  bool preserveSelectionOnInactiveAdopt = false;
  bool forceLiveStatus = false;
  bool storageUploadAvailable = false;
  bool storageUploadAccepted = true;
  int storageUploads = 0;
  @override
  bool get canSaveArtworkToBoard => storageUploadAvailable;
  @override
  Future<bool> saveArtworkToBoard() async {
    storageUploads++;
    return storageUploadAccepted;
  }

  LitchcraftStatus? forcedStatus;
  Completer<bool>? adoptGate;
  Completer<void>? modeResolutionGate;
  @override
  bool get selected => retained;
  @override
  bool get boardActive => connectedBoardActive;
  @override
  bool get authorityResolving =>
      revalidateGate != null && !revalidateGate!.isCompleted;
  @override
  LitchcraftStatus get status =>
      forcedStatus ?? (forceLiveStatus ? LitchcraftStatus.live : super.status);

  void showStatus(LitchcraftStatus value) {
    forcedStatus = value;
    notifyListeners();
  }

  void simulatePhysicalMode(bool active) {
    connectedBoardActive = active;
    retained = active;
    if (bleService.isConnected) {
      _inspectedMode = active;
      _inspectedModeConnectionToken = bleService.connectionSessionToken;
      _inspectedModeDeviceId = bleService.connectedId;
    }
    notifyListeners();
  }

  @override
  Future<void> enter({Future<void>? draftReady}) async {
    ++enterCalls;
    await enterGate?.future;
    retained = true;
  }

  @override
  Future<void> revalidateSelectedBoard() async {
    ++revalidateCalls;
    notifyListeners();
    await revalidateGate?.future;
    revalidateGate = null;
    notifyListeners();
  }

  @override
  void paintSquares(Iterable<int> squares) {
    ++paintCalls;
    super.paintSquares(squares);
  }

  @override
  Future<bool> adoptActiveBoard() async {
    ++adoptCalls;
    if (failModeInspection) throw StateError('Mode inspection failed');
    hasMarksAtAdopt = hasMarks;
    final inspectedDeviceId = bleService.connectedId;
    final inspectedConnectionToken = bleService.connectionSessionToken;
    if (transientSelectionDuringAdopt) {
      retained = true;
      notifyListeners();
    }
    final bool active =
        await (adoptGate?.future ?? Future<bool>.value(connectedBoardActive));
    connectedBoardActive = active;
    retained = active || preserveSelectionOnInactiveAdopt;
    _inspectedMode = active;
    _inspectedModeDeviceId = inspectedDeviceId;
    _inspectedModeConnectionToken = inspectedConnectionToken;
    notifyListeners();
    return active;
  }

  @override
  Future<void> settleModeResolution() async {
    await modeResolutionGate?.future;
  }

  @override
  Future<void> leave() async {
    ++leaveCalls;
    if (rejectExit) throw StateError('Scripted exit rejection');
    await exitGate?.future;
    retained = false;
    await super.leave();
  }
}

// Scripted wire responses exercise the production controller and link; this
// peer does not implement Hall input, firmware admission or persistence policy.
class _CraftNavigationBle extends _ConnectionGateBleService {
  _CraftNavigationBle() : super(scriptCapabilities: true) {
    reviewBoardRotationKnown = true;
  }
  final packets = StreamController<List<int>>.broadcast(sync: true);
  final commands = <int>[];
  Completer<LitchDeviceCapabilities>? capabilityGate;
  Completer<void>? paintGate, exitGate, inspectionGate;
  bool active = false, rejectExit = false;
  int revision = 10;
  LitchcraftCanvas artwork = LitchcraftCanvas.blank();

  // This fixture scripts the domain adapter after discovery. Native session
  // activation is exercised by the shared-session and VirtualLitch journeys.
  @override
  Future<bool> litchcraftControlAvailable(String deviceId) async => false;

  @override
  bool get productReady => isConnected;

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async =>
      await capabilityGate?.future ??
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask: LitchDeviceCapabilities.litchcraftBit,
        explicitlyReported: true,
      );
  @override
  Stream<List<int>> litchcraftPackets(String id) => packets.stream;

  void emit(int event, int request, [List<int> payload = const []]) {
    final bytes = Uint8List(12 + payload.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = LitchcraftProtocol.magic;
    bytes[1] = event;
    data.setUint32(2, 41, Endian.little);
    data.setUint32(6, revision, Endian.little);
    data.setUint16(10, request, Endian.little);
    bytes.setRange(12, bytes.length, payload);
    packets.add(bytes);
  }

  List<int> get state => [0, active ? 1 : 0, 3, 0, 0, 0, 15, 0];

  @override
  Future<void> writeLitchcraftPacket(
    String id,
    List<int> bytes, {
    bool withResponse = true,
  }) async {
    final opcode = bytes[1], request = bytes[6] | bytes[7] << 8;
    commands.add(opcode);
    if (opcode == LitchcraftProtocol.inspect ||
        opcode == LitchcraftProtocol.snapshot) {
      await inspectionGate?.future;
    }
    if (opcode == LitchcraftProtocol.enter) active = true;
    if (opcode == LitchcraftProtocol.paint) {
      await paintGate?.future;
      for (final square in bytes.skip(11)) {
        artwork = artwork.paint([square], 15, 0xff7430);
        ++revision;
        emit(LitchcraftProtocol.cell, request, [square, 15, 0xff, 0x74, 0x30]);
      }
    }
    if (opcode == LitchcraftProtocol.exit) {
      await exitGate?.future;
      if (rejectExit) throw StateError('Scripted Exit transport failure');
      active = false;
      ++revision;
    }
    emit(LitchcraftProtocol.state, request, state);
    if (opcode == LitchcraftProtocol.snapshot) {
      emit(LitchcraftProtocol.snapshotBegin, request, state);
      for (var offset = 0; offset < 256; offset += 2) {
        emit(LitchcraftProtocol.snapshotChunk, request, [
          ...LitchcraftProtocol.u16(offset),
          ...artwork.colors.skip(offset).take(2).expand(LitchcraftProtocol.rgb),
        ]);
      }
      emit(LitchcraftProtocol.snapshotEnd, request);
    }
  }

  @override
  Future<void> dispose() async {
    await packets.close();
    await super.dispose();
  }
}

class _DelayedCraftStore extends LitchcraftArtworkStore {
  _DelayedCraftStore(super.preferences);
  Completer<void>? gate;
  bool fail = false;
  int writes = 0, bytes = 0;
  @override
  Future<void> save(LitchcraftLibrarySnapshot snapshot) async {
    ++writes;
    bytes = jsonEncode(snapshot.toJson()).length;
    await gate?.future;
    if (fail) throw StateError('Scripted draft storage failure');
    await super.save(snapshot);
  }
}

class _DelayedArtworkStore extends LitchcraftArtworkStore {
  _DelayedArtworkStore(super.preferences);

  final Completer<LitchcraftLibrarySnapshot> loadGate =
      Completer<LitchcraftLibrarySnapshot>();

  @override
  Future<LitchcraftLibrarySnapshot> load() => loadGate.future;
}

class _NoopBle extends Fake implements FlutterReactiveBle {}

class _OwnerVerificationUiService extends LitchBleService {
  _OwnerVerificationUiService(OwnerPairingBoard board)
    : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());

  final _discoveredBoards = StreamController<List<LitchBoard>>.broadcast(
    sync: true,
  );
  @override
  Stream<List<LitchBoard>> get boardsStream => _discoveredBoards.stream;

  void advertise(LitchBoard board) =>
      _discoveredBoards.add(<LitchBoard>[board]);

  // Native scan permission is outside the owner-proof presentation journey.
  @override
  Future<void> startScan() async {}

  @override
  Future<void> dispose() async {
    await super.dispose();
    await _discoveredBoards.close();
  }
}

class _OfflineFirmwareDownloadService extends FirmwareDownloadService {
  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    throw const FirmwareFetchException('offline test');
  }
}

const OtaVerificationTarget _otaTarget = OtaVerificationTarget(
  deviceId: 'board-1',
  uploadConnectionToken: 7,

  version: '1.2.3',
  imageSha256:
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
);

class _ConnectionGateBleService extends LitchBleService {
  _ConnectionGateBleService({
    bool useProductionConsoleStream = false,
    bool scriptCapabilities = false,
    SettingsSessionBoard? board,
  }) : this._(
         board ?? SettingsSessionBoard(includeHomeSnapshot: true),
         useProductionConsoleStream,
         scriptCapabilities,
       );

  _ConnectionGateBleService._(
    this._peer,
    this.useProductionConsoleStream,
    this._scriptCapabilities,
  ) : super(
         ble: _scriptCapabilities ? _NoopBle() : _peer,
         pendingOtaStore: MemoryOtaTargetStore(),
       );

  final SettingsSessionBoard _peer;
  final bool useProductionConsoleStream;
  final bool _scriptCapabilities;
  Future<void>? _linkCleanup;

  final StreamController<List<LitchBoard>> _boardsController =
      StreamController<List<LitchBoard>>.broadcast(sync: true);
  final StreamController<BleAvailability> _availabilityController =
      StreamController<BleAvailability>.broadcast(sync: true);
  final StreamController<ConnectionStateUpdate> _connectionController =
      StreamController<ConnectionStateUpdate>.broadcast(sync: true);
  final StreamController<List<String>> _consoleController =
      StreamController<List<String>>.broadcast(sync: true);
  final List<({String deviceId, Completer<bool> completer})> _pendingConnects =
      <({String deviceId, Completer<bool> completer})>[];

  final List<String> connectCalls = <String>[];
  final List<({String deviceId, int number, Completer<bool> completer})>
  identifyCalls =
      <({String deviceId, int number, Completer<bool> completer})>[];
  bool deferIdentify = false;
  final List<bool> foregroundCalls = <bool>[];
  int stopScanCalls = 0;
  Completer<void>? heldStopScan;
  int identifyStopCalls = 0;
  int disconnectCalls = 0;
  bool settlePendingOnDisconnect = false;
  int homeReadRequests = 0;
  final List<String> sentConsoleLines = <String>[];
  String? boardFirmwareVersion;
  OtaPostRebootVerification otaVerification = const OtaPostRebootPending(
    'The update remains unverified.',
  );
  bool reviewBoardRotationKnown = false;

  @override
  Object? latestSettingValue(String key) => reviewBoardRotationKnown
      ? switch (key) {
          'BOARD_ROTATE' => false,
          'GAME_FIRST_PLAYER' => 1,
          _ => super.latestSettingValue(key),
        }
      : super.latestSettingValue(key);

  RuntimeHealthResult healthResult = const RuntimeHealthUnavailable(
    RuntimeHealthUnavailableReason.unsupported,
  );

  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async => healthResult;
  Completer<void>? _backgroundTransition;
  bool _connected = false;
  String? _connectedId;
  int _connectionSessionToken = 0;
  bool _disposed = false;
  BleAvailability _availability = BleAvailability.checking;
  bool _availabilityRecoveryNeedsSettings = false;
  int startScanCalls = 0;
  int rescanCalls = 0;
  BluetoothPairingRecoveryState? recoveryState;

  @override
  BluetoothPairingRecoveryState? get pairingRecovery =>
      recoveryState ?? super.pairingRecovery;
  int requestBluetoothAccessCalls = 0;
  int openBluetoothSettingsCalls = 0;

  bool get disposed => _disposed;

  void delayNextBackgroundTransition() {
    _backgroundTransition = Completer<void>();
  }

  void completeBackgroundTransition() {
    _backgroundTransition?.complete();
    _backgroundTransition = null;
  }

  @override
  Stream<List<LitchBoard>> get boardsStream => _boardsController.stream;

  @override
  Stream<BleAvailability> get availabilityStream =>
      _availabilityController.stream;

  @override
  BleAvailability get bleAvailability => _availability;

  @override
  bool get availabilityRecoveryNeedsSettings =>
      _availabilityRecoveryNeedsSettings;

  @override
  Stream<ConnectionStateUpdate> get connectionStream =>
      _connectionController.stream;

  @override
  Stream<List<String>> get consoleStream => useProductionConsoleStream
      ? super.consoleStream
      : _consoleController.stream;

  @override
  bool get isConnected => _connected;

  @override
  String? get connectedId => _connectedId;

  @override
  int get connectionSessionToken => _connectionSessionToken;

  void emitBoards(List<LitchBoard> boards) {
    _boardsController.add(List<LitchBoard>.from(boards));
  }

  void emitAvailability(
    BleAvailability availability, {
    bool recoveryNeedsSettings = false,
  }) {
    _availability = availability;
    _availabilityRecoveryNeedsSettings = recoveryNeedsSettings;
    _availabilityController.add(availability);
  }

  @override
  Future<void> requestBluetoothAccess() async {
    requestBluetoothAccessCalls += 1;
  }

  @override
  Future<bool> openBluetoothSettings() async {
    openBluetoothSettingsCalls += 1;
    return true;
  }

  void emitConnection(
    String deviceId,
    DeviceConnectionState state, {
    bool sessionReserved = false,
  }) {
    final bool startsSession =
        state == DeviceConnectionState.connected &&
        (!_connected || _connectedId != deviceId);
    if (!_scriptCapabilities &&
        _connected &&
        (state != DeviceConnectionState.connected || _connectedId != deviceId)) {
      _linkCleanup = super.disconnect();
    }
    if (startsSession && !sessionReserved) {
      ++_connectionSessionToken;
    }
    _connected = state == DeviceConnectionState.connected;
    _connectedId = _connected ? deviceId : null;
    if (startsSession && !_scriptCapabilities) {
      unawaited(_activateBoard(deviceId, _connectionSessionToken));
    } else {
      _publishConnection(deviceId, state);
    }
  }

  void _publishConnection(String deviceId, DeviceConnectionState state) {
    _connectionController.add(
      ConnectionStateUpdate(
        deviceId: deviceId,
        connectionState: state,
        failure: null,
      ),
    );
  }

  Future<void> _activateBoard(String deviceId, int token) async {
    final cleanup = _linkCleanup;
    if (cleanup != null) await cleanup;
    if (_disposed ||
        _connectedId != deviceId ||
        _connectionSessionToken != token) {
      return;
    }
    _peer.sessionActivated = false;
    debugSetConnectedDevice(deviceId);
    // A replacement must not start channel discovery through the gate while
    // the old native link is retiring its status subscription.
    _publishConnection(deviceId, DeviceConnectionState.connected);
    await ensureProductReady(deviceId);
  }

  void reportHome() => _peer.publishHome();

  void reportHealth({
    int stateCode = 0,
    int causeMask = 0,
    bool storageFailed = false,
    int resetReason = 3,
    int watchdogRestarts = 0,
  }) {
    if (storageFailed) {
      _peer.onStateQuery = (code) {
        if (code == BoardStorageRecoverySnapshot.stateCode) {
          _peer.emitState(code, [1, BoardStorageRecoveryStatus.idle.index, 0]);
        }
      };
    }
    final revision = ++_peer.revision;
    final runtime = Uint8List(6)
      ..[0] = 1
      ..[3] = stateCode;
    ByteData.sublistView(runtime)
      ..setUint16(1, revision, Endian.little)
      ..setUint16(4, causeMask, Endian.little);
    final context = Uint8List(10)
      ..[0] = storageFailed ? 0x46 : 0x40
      ..[1] = resetReason;
    ByteData.sublistView(context)
      ..setUint32(2, 2, Endian.little)
      ..setUint32(6, watchdogRestarts, Endian.little);
    _peer.emitState(ConnectionSession.boardHealthContextStateCode, context);
    _peer.emitState(ConnectionSession.runtimeHealthStateCode, runtime);
  }

  void completeNextConnect(bool connected) {
    final ({String deviceId, Completer<bool> completer}) pending =
        _pendingConnects.removeAt(0);
    if (connected) {
      emitConnection(
        pending.deviceId,
        DeviceConnectionState.connected,
        sessionReserved: true,
      );
    }
    pending.completer.complete(connected);
  }

  void completeNextIdentify([bool identified = true]) {
    identifyCalls.removeAt(0).completer.complete(identified);
  }

  @override
  Future<void> startScan() async {
    startScanCalls += 1;
  }

  @override
  Future<void> rescan() async {
    rescanCalls += 1;
  }

  @override
  Future<void> stopScan() async {
    stopScanCalls += 1;
    await heldStopScan?.future;
  }

  @override
  Future<bool> connect(String deviceId) {
    // Production reserves the generation synchronously before connect returns;
    // the later native connected event belongs to that same reservation.
    ++_connectionSessionToken;
    connectCalls.add(deviceId);
    final Completer<bool> completer = Completer<bool>();
    _pendingConnects.add((deviceId: deviceId, completer: completer));
    return completer.future;
  }

  @override
  Future<void> disconnect() async {
    disconnectCalls += 1;
    if (settlePendingOnDisconnect) {
      final pending = List.of(_pendingConnects);
      _pendingConnects.clear();
      for (final attempt in pending) {
        attempt.completer.complete(false);
      }
    }
    final String deviceId = _connectedId ?? '';
    emitConnection(deviceId, DeviceConnectionState.disconnected);
  }

  @override
  Future<void> setAppForegroundActive(bool isForeground) async {
    foregroundCalls.add(isForeground);
    if (!isForeground) {
      await _backgroundTransition?.future;
    }
  }

  @override
  Future<bool> sendIdentify(
    String deviceId,
    int number, {
    int durationMs = 10000,
    bool untilConnected = true,
  }) {
    final completer = Completer<bool>();
    identifyCalls.add((
      deviceId: deviceId,
      number: number,
      completer: completer,
    ));
    if (!deferIdentify) {
      completer.complete(true);
    }
    return completer.future;
  }

  @override
  Future<bool> sendIdentifyStop(String deviceId) async {
    identifyStopCalls += 1;
    return true;
  }

  @override
  Future<bool> sendConsoleLine(String deviceId, String line) async {
    sentConsoleLines.add(line);
    if (const <String>{
      'engine fen',
      'game status',
      'board status',
    }.contains(line)) {
      ++homeReadRequests;
    }
    return true;
  }

  @override
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async {
    ++homeReadRequests;
    return true;
  }

  @override
  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) async =>
      boardFirmwareVersion;

  // The real service/channel acceptance boundary is covered by
  // ota_reconnect_service_test; these gate tests restore its owned result.
  @override
  Future<OtaPostRebootVerification> verifyFirmwareAfterOta({
    required String deviceId,
    required int uploadConnectionToken,

    required String expectedVersion,
    required String expectedImageSha256,
  }) async => otaVerification;

  @override
  Future<FirmwareProvenance> fetchFirmwareProvenance({
    required String deviceId,
  }) async => FirmwareProvenance.offline;

  @override
  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    await super.dispose();
    await _peer.packets.close();
    await _peer.sessionPackets.close();
    await _boardsController.close();
    await _availabilityController.close();
    await _connectionController.close();
    await _consoleController.close();
  }
}

class _RecoveryNavigationBle extends _ConnectionGateBleService {
  _RecoveryNavigationBle() : super(scriptCapabilities: true);

  bool reportConnectedAsReady = false;

  @override
  bool get productReady =>
      reportConnectedAsReady ? isConnected : super.productReady;

  static const recovery = LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask:
        LitchDeviceCapabilities.firmwareDigestBit |
        LitchDeviceCapabilities.firmwareUpdateBit,
    explicitlyReported: true,
    recoveryOnly: true,
  );
  static const normal = LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask:
        LitchDeviceCapabilities.settingsBit |
        LitchDeviceCapabilities.litchcraftBit |
        LitchDeviceCapabilities.firmwareDigestBit |
        LitchDeviceCapabilities.firmwareUpdateBit,
    explicitlyReported: true,
  );
  static const digestOnly = LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask: LitchDeviceCapabilities.firmwareDigestBit,
    explicitlyReported: true,
    recoveryOnly: true,
  );
  LitchDeviceCapabilities capabilities = recovery;
  Completer<LitchDeviceCapabilities>? capabilityGate;
  int capabilityRequests = 0;
  bool failCapabilities = false;

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    ++capabilityRequests;
    if (failCapabilities) throw StateError('Capability discovery failed');
    return await capabilityGate?.future ?? capabilities;
  }
}

class _DelayedStartupPreferences extends InMemorySharedPreferencesStore {
  _DelayedStartupPreferences(super.data) : super.withData();

  final ready = Completer<void>();

  @override
  Future<Map<String, Object>> getAll() async {
    await ready.future;
    return super.getAll();
  }
}

class _FailingStartupPreferences extends InMemorySharedPreferencesStore {
  _FailingStartupPreferences() : super.withData(<String, Object>{});

  @override
  Future<Map<String, Object>> getAll() async {
    throw StateError('Optional phone preferences are unavailable');
  }
}

class _EmptyGameLibraryRepository extends GameLibraryRepository {
  _EmptyGameLibraryRepository({GameLibrarySnapshot? snapshot})
    : _snapshot = snapshot ?? GameLibrarySnapshot(),
      super(GameLibraryStore(Directory.systemTemp));

  GameLibrarySnapshot _snapshot;
  final StreamController<GameLibrarySnapshot> _changes =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);

  @override
  GameLibrarySnapshot get snapshot => _snapshot;

  @override
  Stream<GameLibrarySnapshot> get changes => _changes.stream;

  @override
  Future<GameLibrarySnapshot> load() {
    _changes.add(_snapshot);
    return Future<GameLibrarySnapshot>.value(_snapshot);
  }

  int catalogCalls = 0;
  int windowCalls = 0;

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    ++catalogCalls;
    return _catalogFor(_snapshot);
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async {
    ++windowCalls;
    return _windowFor(_snapshot, offset: offset, limit: limit);
  }

  @override
  Future<void> save(GameArchive game) async {
    _snapshot = GameLibrarySnapshot(
      games: <GameArchive>[
        game,
        ..._snapshot.games.where(
          (GameArchive current) => current.libraryId != game.libraryId,
        ),
      ],
      recoveredFiles: _snapshot.recoveredFiles,
      rejectedFiles: _snapshot.rejectedFiles,
    );
    _changes.add(_snapshot);
  }

  @override
  Future<GameArchive?> findByIdentity(
    String libraryId,
    String gameId,
    GameArchiveOrigin origin,
  ) async => _snapshot.games
      .where(
        (GameArchive game) =>
            game.libraryId == libraryId &&
            game.gameId == gameId &&
            game.origin == origin,
      )
      .firstOrNull;

  @override
  Future<void> rename(String libraryId, String? displayName) async {
    _snapshot = GameLibrarySnapshot(
      games: _snapshot.games.map((GameArchive game) {
        return game.libraryId == libraryId ? game.rename(displayName) : game;
      }),
      recoveredFiles: _snapshot.recoveredFiles,
      rejectedFiles: _snapshot.rejectedFiles,
    );
    _changes.add(_snapshot);
  }

  @override
  Future<void> close() => _changes.close();
}

class _DeferredGameLibraryRepository extends GameLibraryRepository {
  _DeferredGameLibraryRepository()
    : super(GameLibraryStore(Directory.systemTemp));

  final Completer<GameLibrarySnapshot> pendingLoad =
      Completer<GameLibrarySnapshot>();
  final StreamController<GameLibrarySnapshot> _changes =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);
  GameLibrarySnapshot _snapshot = GameLibrarySnapshot();

  @override
  GameLibrarySnapshot get snapshot => _snapshot;

  @override
  Stream<GameLibrarySnapshot> get changes => _changes.stream;

  @override
  Future<GameLibrarySnapshot> load() async {
    _snapshot = await pendingLoad.future;
    _changes.add(_snapshot);
    return _snapshot;
  }

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    _snapshot = await pendingLoad.future;
    return _catalogFor(_snapshot);
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async => _windowFor(_snapshot, offset: offset, limit: limit);

  @override
  Future<void> close() => _changes.close();
}

class _RetryGameLibraryRepository extends GameLibraryRepository {
  _RetryGameLibraryRepository(this.successSnapshot)
    : super(GameLibraryStore(Directory.systemTemp));

  final GameLibrarySnapshot successSnapshot;
  final StreamController<GameLibrarySnapshot> _changes =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);
  GameLibrarySnapshot _snapshot = GameLibrarySnapshot();
  int loadCalls = 0;

  @override
  GameLibrarySnapshot get snapshot => _snapshot;

  @override
  Stream<GameLibrarySnapshot> get changes => _changes.stream;

  @override
  Future<GameLibrarySnapshot> load() async {
    loadCalls += 1;
    if (loadCalls == 1) throw StateError('scripted library load failure');
    _snapshot = successSnapshot;
    _changes.add(_snapshot);
    return _snapshot;
  }

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    loadCalls += 1;
    if (loadCalls == 1) throw StateError('scripted library load failure');
    _snapshot = successSnapshot;
    return _catalogFor(_snapshot);
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async => _windowFor(_snapshot, offset: offset, limit: limit);

  @override
  Future<void> close() => _changes.close();
}

class _PagingCountingGameLibraryStore extends GameLibraryStore {
  _PagingCountingGameLibraryStore(super.directory);

  int fullLoads = 0;
  int exactReads = 0;

  @override
  Future<GameLibrarySnapshot> load() {
    fullLoads += 1;
    return super.load();
  }

  @override
  Future<GameLibraryArchiveRead> read(String libraryId) {
    exactReads += 1;
    return super.read(libraryId);
  }
}

class _PreloadedPagingGameLibraryRepository extends GameLibraryRepository {
  _PreloadedPagingGameLibraryRepository({
    required this.catalog,
    required this.window,
  }) : super(GameLibraryStore(Directory.systemTemp));

  GameLibraryCatalog catalog;
  GameLibraryWindow window;
  GameLibrarySnapshot _snapshot = GameLibrarySnapshot();
  final StreamController<GameLibrarySnapshot> _changes =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);
  int fullLoadCalls = 0;
  int windowCalls = 0;

  @override
  GameLibrarySnapshot get snapshot => _snapshot;

  @override
  Stream<GameLibrarySnapshot> get changes => _changes.stream;

  void publish({
    required GameLibraryCatalog catalog,
    required GameLibraryWindow window,
    required GameLibrarySnapshot change,
  }) {
    this.catalog = catalog;
    this.window = window;
    _snapshot = change;
    _changes.add(change);
  }

  @override
  Future<GameLibrarySnapshot> load() async {
    fullLoadCalls += 1;
    throw StateError('Play must not materialize the complete game library.');
  }

  @override
  Future<GameLibraryCatalog> loadCatalog() async => catalog;

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async {
    windowCalls += 1;
    expect(offset, 0);
    expect(limit, 5);
    return window;
  }

  @override
  Future<void> close() => _changes.close();
}

GameLibraryCatalog _catalogFor(GameLibrarySnapshot snapshot) {
  return GameLibraryCatalog(
    totalCount: snapshot.games.length,
    recoveredFiles: snapshot.recoveredFiles,
    rejectedFiles: snapshot.rejectedFiles,
  );
}

GameLibraryWindow _windowFor(
  GameLibrarySnapshot snapshot, {
  required int offset,
  required int limit,
}) {
  final List<GameArchive> games = List<GameArchive>.of(snapshot.games)
    ..sort((GameArchive left, GameArchive right) {
      final int byUpdate = right.updatedAt.compareTo(left.updatedAt);
      return byUpdate != 0
          ? byUpdate
          : left.libraryId.compareTo(right.libraryId);
    });
  final int start = offset.clamp(0, games.length);
  final int end = (start + limit).clamp(start, games.length);
  return GameLibraryWindow(
    offset: start,
    totalCount: games.length,
    games: games.sublist(start, end),
  );
}

class _PagedSettingsBleService extends _ConnectionGateBleService {
  _PagedSettingsBleService({super.board})
    : super(useProductionConsoleStream: true);

  int sleepRequests = 0;
  Completer<ConnectionSessionOutcome?>? sleepResponse;
  @override
  Future<ConnectionSessionOutcome?> requestSleep({
    required String deviceId,
  }) async {
    ++sleepRequests;
    return sleepResponse == null ? null : await sleepResponse!.future;
  }

  int settingsReadCalls = 0;
  final StreamController<String> settingsChanges =
      StreamController<String>.broadcast();
  final Map<String, Object> boardValues = <String, Object>{
    'VS_CPU_LEVEL': 0,
    'VS_CPU_PLAYER': 2,
    'BRIGHTNESS': 5,
    'AUTO_BRIGHTNESS': true,
    'BOARD_ROTATE': false,
    'BLE_DEVICE_NAME': 'Foxie Litch',
  };
  final List<({String key, Object value, bool awaitAck})> settingWrites = [];
  bool rejectNextAcknowledgedWrite = false;
  bool loseNextAcknowledgedWriteAfterApply = false;
  bool applyRejectedAcknowledgedWrite = false;
  bool failSettingsRead = false;
  bool applyPreviewWrites = true;
  Completer<void>? previewWriteGate;
  Completer<bool>? nextBoardStateConfirmation;
  bool usesExplicitBoardStateReports = false;
  int boardSettingsRevision = 10;
  int? advertisedBrightnessMin;
  int? advertisedBrightnessMax;
  LitchSettingsReadAuthority reportedSettingsAuthority =
      LitchSettingsReadAuthority.confirmed;

  @override
  LitchSettingsReadAuthority settingsReadAuthority(String deviceId) =>
      reportedSettingsAuthority;

  @override
  bool get hasBoardStateSnapshot => usesExplicitBoardStateReports;

  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async {
    ++settingsReadCalls;
    if (failSettingsRead) throw StateError('scripted settings read failure');
    return <LitchSetting>[
      for (final String key in boardValues.keys)
        LitchSetting(
          key: key,
          type: boardValues[key] is bool
              ? 'bool'
              : boardValues[key] is String
              ? 'string'
              : 'u32',
          category: key == 'BRIGHTNESS' || key == 'AUTO_BRIGHTNESS'
              ? 'display'
              : 'behavior',
          value: boardValues[key]!,
          defaultValue: boardValues[key]!,
          min: key == 'BRIGHTNESS'
              ? 1
              : key == 'VS_CPU_LEVEL'
              ? 0
              : key == 'VS_CPU_PLAYER'
              ? 1
              : null,
          max: key == 'BRIGHTNESS'
              ? 8
              : key == 'VS_CPU_LEVEL'
              ? 11
              : key == 'VS_CPU_PLAYER'
              ? 2
              : null,
        ),
    ];
  }

  @override
  Object? latestSettingValue(String key) =>
      boardValues[key] ?? super.latestSettingValue(key);

  @override
  List<LitchSetting> immediateSettingsSnapshot(String deviceId) {
    if (advertisedBrightnessMin case final min?) {
      return <LitchSetting>[
        LitchSetting(
          key: 'BRIGHTNESS',
          type: 'u32',
          category: 'display',
          value: boardValues['BRIGHTNESS']!,
          defaultValue: 5,
          min: min,
          max: advertisedBrightnessMax,
        ),
      ];
    }
    return const <LitchSetting>[];
  }

  @override
  Stream<String> get settingsChangedStream => settingsChanges.stream;

  void reportBoardSetting(String key, Object value, {int? revision}) {
    // The production service rejects older board_state revisions before
    // publishing a settings change. Keep this fake's late-state path aligned.
    if (revision != null && revision < boardSettingsRevision) return;
    boardSettingsRevision = revision ?? boardSettingsRevision + 1;
    boardValues[key] = value;
    settingsChanges.add(key);
  }

  @override
  Future<void> dispose() async {
    await settingsChanges.close();
    await super.dispose();
  }

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async => boardValues[key];

  @override
  Future<bool> writeSetting({
    required String deviceId,
    required String key,
    required Object value,
    bool awaitAck = false,
    bool preferWithoutResponse = false,
  }) async {
    settingWrites.add((key: key, value: value, awaitAck: awaitAck));
    await previewWriteGate?.future;
    if (applyPreviewWrites) boardValues[key] = value;
    return true;
  }

  @override
  Future<LitchSettingWriteResult> writeSettingAcknowledged({
    required String deviceId,
    required String key,
    required Object value,
  }) async {
    settingWrites.add((key: key, value: value, awaitAck: true));
    if (loseNextAcknowledgedWriteAfterApply) {
      loseNextAcknowledgedWriteAfterApply = false;
      boardValues[key] = value;
      return const LitchSettingWriteNotAcknowledged(
        LitchSettingWriteFailure.disconnected,
      );
    }
    if (rejectNextAcknowledgedWrite) {
      rejectNextAcknowledgedWrite = false;
      if (applyRejectedAcknowledgedWrite) boardValues[key] = value;
      return LitchSettingWriteAcknowledged(
        LitchSettingOutcome(
          requestId: 1,
          key: key,
          ok: false,
          reason: LitchMutationReason.busy,
          hasValue: true,
          value: boardValues[key],
          isCompact: false,
        ),
      );
    }
    final Completer<bool>? confirmation = nextBoardStateConfirmation;
    nextBoardStateConfirmation = null;
    if (confirmation == null) {
      boardValues[key] = value;
      ++boardSettingsRevision;
    }
    final Future<bool>? boardStateConfirmation = confirmation?.future.then((
      bool confirmed,
    ) {
      if (confirmed && !usesExplicitBoardStateReports) {
        boardValues[key] = value;
        ++boardSettingsRevision;
      }
      return confirmed;
    });
    return LitchSettingWriteAcknowledged(
      LitchSettingOutcome(
        requestId: 1,
        key: key,
        ok: true,
        reason: LitchMutationReason.none,
        hasValue: true,
        value: value,
        isCompact: false,
      ),
      boardStateConfirmation: boardStateConfirmation,
    );
  }
}

Future<void> _selectAdjacentMainPage(
  WidgetTester tester, {
  required bool forward,
  VoidCallback? afterSelection,
}) async {
  const destinations = ['play', 'litchcraft', 'setup', 'console'];
  final index = _activeNavigationIndex(tester) + (forward ? 1 : -1);
  await tester.tap(_navigationAction(tester, destinations[index]));
  afterSelection?.call();
  await tester.pumpAndSettle();
}

Future<void> _expectCraftFramesReachable(
  WidgetTester tester,
  LitchcraftLibrary library,
  int frameCount,
) async {
  final frames = library.animationDocument.frames;
  final originalSelection = library.selectedFrameIndex;
  expect(frames, hasLength(frameCount));
  Future<void> select(int index) async {
    final target = await revealCraftFilmstripFrame(
      tester,
      index: index,
      frameCount: frameCount,
      selectedFrame: () => library.selectedFrameIndex,
    );
    final thumbnail = tester.widget<LitchcraftFrameThumbnail>(
      find.descendant(
        of: target,
        matching: find.byType(LitchcraftFrameThumbnail),
      ),
    );
    expect(thumbnail.frameIndex, index);
    expect(
      thumbnail.document.frames[index].emitters.map((e) => e.color),
      frames[index].emitters.map((e) => e.color),
    );
    await tester.tap(target);
    await tester.pump();
    await settleCraftFilmstripScroll(tester);
    expect(
      library.selectedFrameIndex,
      index,
      reason: 'Every logical frame remains selectable through its real tile.',
    );
    expect(tester.widget<IconButton>(target).isSelected, isTrue);
    expect(
      library.session.canvas.colors,
      frames[index].emitters.map((e) => e.color).toList(),
    );
  }

  for (var index = 0; index < frameCount; index++) {
    await select(index);
  }
  await select(originalSelection);
  expect(
    library.animationDocument.frames,
    frames,
    reason: 'Browsing and selecting preserve all distinct frame contents.',
  );
}

Future<void> _pumpConnectedGate(
  WidgetTester tester,
  _ConnectionGateBleService service, {
  FirmwarePageBuilder? firmwarePageBuilder,
  GameLibraryRepository? gameLibrary,
  double bottomSystemInset = 0,
  EdgeInsets? systemInsets,
  ThemeData? theme,
  ThemeData? darkTheme,
  ValueNotifier<TextScaler>? textScalerOverride,
  TextScaler textScaler = TextScaler.noScaling,
  Size? layoutSize,
  bool liveKeyboardPadding = false,
  TextDirection? textDirection,
  Future<DeviceLandscapeSide?> Function()? orientationSideReader,
}) async {
  const LitchBoard firstBoard = LitchBoard(
    id: 'board-1',
    name: 'Foxie One',
    rssi: -40,
  );
  const LitchBoard secondBoard = LitchBoard(
    id: 'board-2',
    name: 'Foxie Two',
    rssi: -45,
  );

  await tester.pumpWidget(
    MaterialApp(
      theme: theme,
      darkTheme: darkTheme,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      builder: (BuildContext context, Widget? child) {
        final EdgeInsets systemPadding =
            systemInsets ??
            (orientationSideReader != null
                ? MediaQuery.viewPaddingOf(context)
                : EdgeInsets.only(bottom: bottomSystemInset));
        Widget content(TextScaler scale) => MediaQuery(
          data: MediaQuery.of(context).copyWith(
            size: layoutSize,
            padding: liveKeyboardPadding
                ? systemPadding.copyWith(
                    bottom:
                        (systemPadding.bottom -
                                MediaQuery.viewInsetsOf(context).bottom)
                            .clamp(0.0, systemPadding.bottom),
                  )
                : systemPadding,
            viewPadding: systemPadding,
            textScaler: scale,
          ),
          child: RepaintBoundary(
            key: const ValueKey<String>('connected-gate-render'),
            child: Directionality(
              textDirection: textDirection ?? Directionality.of(context),
              child: orientationSideReader == null
                  ? child!
                  : OrientationWorkspace(
                      sideReader: orientationSideReader,
                      child: child!,
                    ),
            ),
          ),
        );
        return textScalerOverride == null
            ? content(textScaler)
            : ValueListenableBuilder<TextScaler>(
                valueListenable: textScalerOverride,
                builder: (_, scale, _) => content(scale),
              );
      },
      home: ConnectionGateScreen(
        bleService: service,
        firmwarePageBuilder: firmwarePageBuilder,
        gameLibrary: gameLibrary,
      ),
    ),
  );
  await tester.pump();
  service.emitBoards(const <LitchBoard>[firstBoard, secondBoard]);
  await tester.pump();
  final strings = AppLocalizations.of(
    tester.element(find.byType(ConnectionGateScreen)),
  );
  final connect = find
      .widgetWithText(FilledButton, strings.connectAction)
      .first;
  await tester.ensureVisible(connect);
  await tester.pumpAndSettle();
  await tester.tap(connect);
  service.completeNextConnect(true);
  await tester.pumpAndSettle();
  expect(service.isConnected, isTrue);
}

Rect _connectionMenuContentBounds(WidgetTester tester, Finder action) {
  final icon = find.descendant(
    of: action,
    matching: find.byWidgetPredicate(
      (widget) => widget is Icon || widget is CircularProgressIndicator,
    ),
  );
  final label = find.descendant(of: action, matching: find.byType(Text));
  return tester.getRect(icon).expandToInclude(tester.getRect(label));
}

void _expectConnectionMenuContentPosition(
  WidgetTester tester,
  Finder action, {
  bool alignEnd = false,
}) {
  final target = tester.getRect(action);
  final content = _connectionMenuContentBounds(tester, action);
  expect(target.shortestSide, greaterThanOrEqualTo(48));
  if (alignEnd) {
    final rtl = Directionality.of(tester.element(action)) == TextDirection.rtl;
    expect(
      rtl ? content.left : content.right,
      closeTo(rtl ? target.left : target.right, .1),
    );
  } else {
    final rtl = Directionality.of(tester.element(action)) == TextDirection.rtl;
    expect(
      rtl ? content.right : content.left,
      closeTo(rtl ? target.right : target.left, .1),
    );
  }
  expect(content.center.dy, closeTo(target.center.dy, .1));
  expect(
    content.left >= target.left && content.top >= target.top,
    isTrue,
    reason: 'target=$target content=$content',
  );
  expect(
    content.right <= target.right && content.bottom <= target.bottom,
    isTrue,
    reason: 'target=$target content=$content',
  );
}

void _expectSleepTextThenIcon(WidgetTester tester, Finder action) {
  final label = tester.getRect(
    find.descendant(of: action, matching: find.byType(Text)),
  );
  final icon = tester.getRect(
    find.descendant(
      of: action,
      matching: find.byWidgetPredicate(
        (widget) => widget is Icon || widget is CircularProgressIndicator,
      ),
    ),
  );
  final rtl = Directionality.of(tester.element(action)) == TextDirection.rtl;
  expect(label.center.dy, closeTo(icon.center.dy, .1));
  expect(
    rtl ? label.left - icon.right : icon.left - label.right,
    closeTo(12, .1),
    reason: 'Sleep keeps its text before the trailing icon in the same row.',
  );
}

GameArchive _recentArchive(int index) {
  final GameArchive source = completedClassicArchive();
  final String suffix = index.toString();
  return GameArchive(
    libraryId: 'recent-$suffix',
    boardGameId: 'recent-board-$suffix',
    createdAt: source.createdAt,
    updatedAt: source.updatedAt.add(Duration(days: index)),
    status: source.status,
    captureQuality: source.captureQuality,
    variant: source.variant,
    outcome: source.outcome,
    winner: source.winner,
    lineRevision: source.lineRevision,
    positions: source.positions,
    transitions: source.transitions,
    whitePlayer: source.whitePlayer,
    blackPlayer: source.blackPlayer,
  );
}

GameArchive _pagingArchive(int index) {
  final String suffix = index.toString();
  return GameArchive(
    libraryId: 'recent-$suffix',
    boardGameId: 'recent-board-$suffix',
    createdAt: DateTime.utc(2026, 1, 1),
    updatedAt: DateTime.utc(2026, 1, 1).add(Duration(minutes: index)),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
      ),
    ],
    transitions: const <ArchivedTransition>[],
  );
}

Future<void> _openGamesFromConnectedPlay(WidgetTester tester) async {
  if (_activeNavigationIndex(tester) != 0) {
    await tester.tap(_navigationAction(tester, 'play'));
    await tester.pumpAndSettle();
  }
  expect(_activeNavigationIndex(tester), 0);
  expect(find.byType(HomeScreen), findsOneWidget);
  expect(
    tester.widget<HomeScreen>(find.byType(HomeScreen)).gameLibrary,
    isNotNull,
  );
  final Finder scrollable = _homeDetailsScrollable();
  expect(scrollable, findsOneWidget);
  final ScrollPosition position = tester
      .state<ScrollableState>(scrollable)
      .position;
  position.jumpTo(position.maxScrollExtent);
  await tester.pumpAndSettle();
  final Finder action = find.byKey(
    const ValueKey<String>('game-library-action'),
  );
  await tester.tap(action);
  await tester.pumpAndSettle();
}

void _expectLabelContained(WidgetTester tester, String label, Rect bounds) {
  final richText = find.descendant(
    of: find.text(label),
    matching: find.byType(RichText),
  );
  final paragraph = tester.renderObject<RenderParagraph>(richText);
  expect(paragraph.didExceedMaxLines, isFalse, reason: label);
  final rect = tester.getRect(richText);
  expect(rect.left, greaterThanOrEqualTo(bounds.left - 0.1), reason: label);
  expect(rect.right, lessThanOrEqualTo(bounds.right + 0.1), reason: label);
  expect(rect.top, greaterThanOrEqualTo(bounds.top - 0.1), reason: label);
  expect(rect.bottom, lessThanOrEqualTo(bounds.bottom + 0.1), reason: label);
}

Future<ui.Image> _captureConnectedGate(
  WidgetTester tester, {
  double pixelRatio = 1,
}) async {
  final RenderRepaintBoundary boundary = tester.renderObject(
    find.byKey(const ValueKey<String>('connected-gate-render')),
  );
  return (await tester.runAsync(
    () => boundary.toImage(pixelRatio: pixelRatio),
  ))!;
}

Future<Map<String, double>> _labelContrasts(
  WidgetTester tester,
  Rect target,
  Map<String, Rect> labels,
  String state,
) async {
  // A phone-density render provides solid glyph interiors even for the small
  // update line. Repeated flat 2x2 patches exclude antialiased edge samples.
  const double scale = 3;
  final ui.Image image = await _captureConnectedGate(tester, pixelRatio: scale);
  final pixels = (await tester.runAsync(
    () => image.toByteData(format: ui.ImageByteFormat.rawRgba),
  ))!;
  int pixel(int x, int y) => pixels.getUint32((y * image.width + x) * 4);
  // A fitted one-line label can reach the target's leading center. Sample
  // above the labels so glyph ink cannot be mistaken for the background.
  final backgroundPoint = Offset(target.center.dx, target.top + 1);
  expect(
    labels.values.every((rect) => !rect.contains(backgroundPoint)),
    isTrue,
    reason: 'The contrast background sample must stay outside every label.',
  );
  final int background = pixel(
    (backgroundPoint.dx * scale).round(),
    (backgroundPoint.dy * scale).round(),
  );
  final Color backgroundColor = Color(0xff000000 | (background >> 8));
  final double backgroundLuminance = backgroundColor.computeLuminance();
  double contrast(Color color) {
    final double foregroundLuminance = color.computeLuminance();
    return foregroundLuminance > backgroundLuminance
        ? (foregroundLuminance + .05) / (backgroundLuminance + .05)
        : (backgroundLuminance + .05) / (foregroundLuminance + .05);
  }

  final RenderAnimatedOpacity feedback = tester.renderObject(
    find.byKey(const ValueKey<String>('connected-board-status-feedback')),
  );
  final Map<String, double> ratios = <String, double>{};
  for (final MapEntry<String, Rect> label in labels.entries) {
    final Map<int, int> cores = <int, int>{};
    final Rect rect = label.value;
    for (
      int y = (rect.top * scale).ceil();
      y < (rect.bottom * scale).floor() - 1;
      ++y
    ) {
      for (
        int x = (rect.left * scale).ceil();
        x < (rect.right * scale).floor() - 1;
        ++x
      ) {
        final int color = pixel(x, y);
        if (color != background &&
            color == pixel(x + 1, y) &&
            color == pixel(x, y + 1) &&
            color == pixel(x + 1, y + 1)) {
          cores.update(color, (int count) => count + 1, ifAbsent: () => 1);
        }
      }
    }
    expect(cores, isNotEmpty, reason: '${label.key} must render solid cores.');
    final MapEntry<int, int> core = cores.entries.reduce(
      (a, b) => a.value > b.value ? a : b,
    );
    expect(core.value, greaterThanOrEqualTo(8));
    expect(background & 0xff, 255);
    expect(core.key & 0xff, 255);
    final Color coreColor = Color(0xff000000 | (core.key >> 8));
    final RenderParagraph paragraph = tester.renderObject(find.text(label.key));
    final Color authored = paragraph.text.style!.color!;
    final Color composited = Color.alphaBlend(
      authored.withValues(alpha: authored.a * feedback.opacity.value),
      backgroundColor,
    );
    // Corroborate the effective authored foreground, including its ancestor's
    // settled fade. A popular antialias edge must not masquerade as a core.
    for (final (double actual, double expected) in <(double, double)>[
      (coreColor.r, composited.r),
      (coreColor.g, composited.g),
      (coreColor.b, composited.b),
    ]) {
      expect(actual, closeTo(expected, 1.5 / 255));
    }
    final double ratio = contrast(coreColor);
    ratios['$state ${label.key} glyph cores'] = ratio;
    ratios['$state ${label.key} composited foreground'] = contrast(composited);
    debugPrint(
      '$state ${label.key}: contrast=${ratio.toStringAsFixed(3)} '
      'core=#${(core.key >> 8).toRadixString(16).padLeft(6, '0')} '
      'background=#${(background >> 8).toRadixString(16).padLeft(6, '0')} '
      'solidPatches=${core.value} '
      'compositedContrast=${contrast(composited).toStringAsFixed(3)}',
    );
  }
  image.dispose();
  return ratios;
}

Future<void> _expectOnlyLabelsChanged(
  WidgetTester tester,
  ui.Image resting,
  ui.Image pressed,
  Rect target,
  List<Rect> foreground,
) async {
  final before = (await tester.runAsync(
    () => resting.toByteData(format: ui.ImageByteFormat.rawRgba),
  ))!;
  final after = (await tester.runAsync(
    () => pressed.toByteData(format: ui.ImageByteFormat.rawRgba),
  ))!;
  int outsideForegroundChanges = 0;
  int foregroundChanges = 0;
  for (int y = target.top.ceil(); y < target.bottom.floor(); ++y) {
    for (int x = target.left.ceil(); x < target.right.floor(); ++x) {
      final int offset = (y * resting.width + x) * 4;
      if (before.getUint32(offset) == after.getUint32(offset)) continue;
      if (foreground.any(
        (Rect rect) => rect.contains(Offset(x + .5, y + .5)),
      )) {
        ++foregroundChanges;
      } else {
        ++outsideForegroundChanges;
      }
    }
  }
  expect(
    foregroundChanges,
    greaterThan(0),
    reason: 'The held label and chevron must visibly fade.',
  );
  expect(
    outsideForegroundChanges,
    0,
    reason: 'A Connected press must not paint the surrounding app-bar target.',
  );
}

Future<void> _pumpConnectedOtaGate(
  WidgetTester tester,
  _ConnectionGateBleService service,
  _OfflineFirmwareDownloadService downloadService,
  LitchBoard board, {
  LitchcraftController? craftController,
  LitchcraftLibrary? craftLibrary,
  bool settle = true,
}) async {
  PackageInfo.setMockInitialValues(
    appName: 'Litch',
    packageName: 'com.foxieproducts.litchMobile',
    version: '0.5.0',
    buildNumber: '1',
    buildSignature: '',
  );
  await tester.pumpWidget(
    MaterialApp(
      home: ConnectionGateScreen(
        bleService: service,
        craftController: craftController,
        craftLibrary: craftLibrary,
        firmwarePageBuilder:
            (
              LitchBleService bleService,
              String boardId,
              VoidCallback onRequestShowFirmwareTab,
              ValueChanged<bool> onUpdateActivityChanged,
              ValueChanged<FirmwareUpdateAvailability> onAvailabilityChanged,
            ) => FirmwareScreen(
              bleService: bleService,
              boardId: boardId,
              onRequestShowFirmwareTab: onRequestShowFirmwareTab,
              onUpdateActivityChanged: onUpdateActivityChanged,
              onAvailabilityChanged: onAvailabilityChanged,
              downloadService: downloadService,
            ),
      ),
    ),
  );
  await tester.pump();
  service.emitBoards(<LitchBoard>[board]);
  service.emitConnection(board.id, DeviceConnectionState.connected);
  if (settle) {
    await tester.pumpAndSettle();
  } else {
    await tester.pump();
  }
  expect(find.byType(LitchMainScaffold), findsOneWidget);
}

Future<void> _recreateGateAfterReboot(
  WidgetTester tester,
  _ConnectionGateBleService service,
  LitchBoard board,
) async {
  service.emitConnection(board.id, DeviceConnectionState.disconnected);
  await tester.pump();
  expect(find.byType(LitchMainScaffold), findsOneWidget);
  expect(service.pendingOtaVerification, same(_otaTarget));

  service.emitBoards(<LitchBoard>[board]);
  await tester.pump(const Duration(milliseconds: 900));
  service.completeNextConnect(true);
  await tester.pump();
  expect(find.byType(LitchMainScaffold), findsOneWidget);
}

void _sendAppToBackground(WidgetTester tester) {
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
}

void _sendAppToForeground(WidgetTester tester) {
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
  tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
}

void main() {
  for (final enabled in [false, true]) {
    testWidgets(
      'disconnected Play retains Console preference $enabled',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues({'enable_app_console': enabled});
        final service = _ConnectionGateBleService(
          useProductionConsoleStream: true,
        );
        service.emitAvailability(BleAvailability.poweredOff);
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('connection-gate-content')),
          findsOneWidget,
        );
        expect(_activeNavigationDestinationCount(tester), enabled ? 4 : 3);
        if (enabled) {
          service.debugAppendConsolePacket(
            'history before offline Console\n'.codeUnits,
          );
          await tester.tap(_navigationAction(tester, 'console'));
          await tester.pumpAndSettle();
          final console = tester.state(find.byType(AppConsoleScreen));
          expect(
            find.textContaining('history before offline Console'),
            findsOneWidget,
          );
          final command = find.byKey(const ValueKey('console-command-input'));
          await tester.enterText(command, 'draft while offline');
          expect(tester.widget<TextField>(command).onSubmitted, isNull);
          FocusManager.instance.primaryFocus?.unfocus();
          for (final landscape in [false, true, false]) {
            tester.view.physicalSize = landscape
                ? const Size(896, 414)
                : const Size(414, 896);
            await tester.pumpAndSettle();
            for (final destination in ['play', 'setup', 'litchcraft', 'play']) {
              await tester.tap(_navigationAction(tester, destination));
              await tester.pumpAndSettle();
              expect(_activeNavigationDestinationCount(tester), 4);
              await tester.tap(_navigationAction(tester, 'console'));
              await tester.pumpAndSettle();
              expect(
                tester.state(find.byType(AppConsoleScreen)),
                same(console),
              );
              expect(
                tester.widget<TextField>(command).controller!.text,
                'draft while offline',
              );
              FocusManager.instance.primaryFocus?.unfocus();
            }
          }
          expect(service.connectCalls, isEmpty);
        } else {
          expect(find.byIcon(Icons.terminal), findsNothing);
        }
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets(
    'warm Play link loss retains the same offline Console draft and history',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues({'enable_app_console': true});
      final service = _ConnectionGateBleService(
        useProductionConsoleStream: true,
      );
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      service.debugAppendConsolePacket('history before link loss\n'.codeUnits);
      await tester.tap(_navigationAction(tester, 'console'));
      await tester.pumpAndSettle();
      final console = tester.state(find.byType(AppConsoleScreen));
      final command = find.byKey(const ValueKey('console-command-input'));
      await tester.enterText(command, 'retained offline draft');
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pumpAndSettle();

      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitBoards(const []);
      service.emitAvailability(BleAvailability.poweredOff);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('connection-gate-content')),
        findsOneWidget,
      );
      expect(_activeNavigationDestinationCount(tester), 4);
      await captureHealthReview(tester, 'console-enabled-warm-offline-play');
      await tester.tap(_navigationAction(tester, 'console'));
      await tester.pumpAndSettle();
      expect(tester.state(find.byType(AppConsoleScreen)), same(console));
      expect(find.textContaining('history before link loss'), findsOneWidget);
      expect(
        tester.widget<TextField>(command).controller!.text,
        'retained offline draft',
      );
      expect(
        tester.widget<TextField>(command).textInputAction,
        TextInputAction.unspecified,
      );
      await tester.showKeyboard(command);
      await tester.testTextInput.receiveAction(TextInputAction.send);
      await tester.pump();
      expect(
        service.sentConsoleLines,
        isNot(contains('retained offline draft')),
      );
      expect(
        tester.widget<TextField>(command).controller!.text,
        'retained offline draft',
      );
      await captureHealthReview(tester, 'console-enabled-warm-offline-console');

      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pumpAndSettle();
      service.emitAvailability(BleAvailability.ready);
      service.emitBoards(const [
        LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
      ]);
      await tester.pump();
      await tester.tap(find.text('Connect'));
      await tester.pump();
      service.completeNextConnect(true);
      await tester.pumpAndSettle();
      await tester.tap(_navigationAction(tester, 'console'));
      await tester.pumpAndSettle();
      expect(tester.state(find.byType(AppConsoleScreen)), same(console));
      expect(
        tester.widget<TextField>(command).controller!.text,
        'retained offline draft',
      );
      expect(
        tester.widget<TextField>(command).textInputAction,
        TextInputAction.send,
      );
      expect(
        service.sentConsoleLines,
        isNot(contains('retained offline draft')),
      );
      await tester.showKeyboard(command);
      await tester.testTextInput.receiveAction(TextInputAction.send);
      await tester.pump();
      expect(
        service.sentConsoleLines.where((line) => line == 'retained offline draft'),
        <String>['retained offline draft'],
      );
      expect(tester.widget<TextField>(command).controller!.text, isEmpty);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'pairing confirms accepted consent while connection stays pending',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      _usePortraitPhone(tester);
      final semantics = tester.ensureSemantics();
      const confirmed = 'Confirmed. Connecting…';
      final strings = lookupAppLocalizations(const Locale('en'));
      final cases = <(OwnerPairingSessionState, int, String)>[
        (OwnerPairingSessionState.securing, 4, confirmed),
        (OwnerPairingSessionState.persistingOwner, 4, confirmed),
        (OwnerPairingSessionState.securing, 5, confirmed),
        (OwnerPairingSessionState.persistingOwner, 5, confirmed),
        (
          OwnerPairingSessionState.awaitPhysical,
          4,
          strings.ownerPairingPhysical,
        ),
        (OwnerPairingSessionState.ownedOther, 5, strings.boardConnecting),
        (OwnerPairingSessionState.failed, 5, strings.ownerPairingRestartBoard),
        (OwnerPairingSessionState.clearing, 5, strings.ownerPairingClearing),
      ];
      try {
        for (final (state, flags, expected) in cases) {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final board = OwnerPairingBoard()
            ..state = state.index
            ..flags = flags
            ..attempt = 7;
          final service = _OwnerVerificationUiService(board);
          try {
            await tester.runAsync(() async {
              await connectBoard(service, board);
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            await tester.pumpWidget(
              localizationHarness(
                locale: const Locale('en'),
                scale: 2,
                home: ConnectionGateScreen(bleService: service),
              ),
            );
            await tester.pump();
            expect(service.ownerPairingStatus?.state, state);
            expect(service.productReady, isFalse);
            expect(board.productQueries, isEmpty);
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            expect(find.text(expected), findsWidgets, reason: state.name);
            if (expected == confirmed) {
              final message = find.text(confirmed);
              expect(message, findsOneWidget);
              expectReviewTextFits(tester, message, state.name);
              final spoken = tester.getSemantics(message).getSemanticsData();
              expect(spoken.label, confirmed);
              expect(spoken.flagsCollection.isLiveRegion, isTrue);
              expect(
                find
                    .byKey(const ValueKey('owner-pairing-cancel'))
                    .hitTestable(),
                findsOneWidget,
              );
            } else {
              expect(find.text(confirmed), findsNothing, reason: state.name);
            }
            if (flags == 5 && expected == confirmed) {
              await tester.pump(const Duration(seconds: 11));
              expect(service.isConnected, isTrue);
              expect(find.text(confirmed), findsOneWidget);
            }
            if (state == OwnerPairingSessionState.ownedOther) {
              expect(service.ownerPairingBusy, isTrue);
              expect(
                find.byKey(const ValueKey('connection-progress-overlay')),
                findsOneWidget,
              );
            }
            expect(tester.takeException(), isNull, reason: state.name);
          } finally {
            await tester.pumpWidget(const SizedBox());
            await tester.runAsync(() => disposeBoard(service, board));
          }
        }
      } finally {
        semantics.dispose();
      }
    },
  );

  final quietProofTextScale = ValueVariant<double>({1, 2});
  for (final nextGate in <String>[
    'physical pairing',
    'other-owner refusal',
    'stale Bluetooth pairing',
  ]) {
    testWidgets(
      'quiet owner proof preserves compact branded shell before $nextGate',
      (tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'board_friendly_name:board-1': 'Studio Board',
        });
        const advertisedBoard = LitchBoard(
          id: 'board-1',
          name: 'Comet 1-3.3',
          rssi: -40,
        );
        final semantics = tester.ensureSemantics();
        final board = OwnerPairingBoard()
          ..state = 6
          ..flags = 5;
        if (nextGate == 'physical pairing') {
          board.capabilityMask |= LitchDeviceCapabilities.litchcraftBit;
          board.capabilityReplyGate = Completer<void>();
        }
        final service = _OwnerVerificationUiService(board);
        Rect? transportOverlayRect;
        Rect? transportBrandRect;
        try {
          if (nextGate != 'physical pairing') {
            await tester.runAsync(() async {
              await connectBoard(service, board);
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
          }
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: TextScaler.linear(
                    nextGate == 'physical pairing'
                        ? quietProofTextScale.currentValue!
                        : 1,
                  ),
                ),
                child: child!,
              ),
              home: ConnectionGateScreen(bleService: service),
            ),
          );
          await tester.pump();
          final support = find.byKey(const ValueKey('owner-pairing-support'));
          final supportLabel = lookupAppLocalizations(
            const Locale('en'),
          ).contactSupport;
          void expectSupportUnavailable() {
            expect(support, findsNothing);
            expect(find.bySemanticsLabel(supportLabel), findsNothing);
          }

          if (nextGate == 'physical pairing') {
            service.advertise(advertisedBoard);
            await tester.pump();
            await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
            await tester.pump();
            transportOverlayRect = tester.getRect(
              find.byKey(const ValueKey('connection-progress-overlay')),
            );
            transportBrandRect = tester.getRect(
              find.byType(LitchAboutBrandButton),
            );
            expectSupportUnavailable();
            await tester.runAsync(() async {
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            expect(board.connections, hasLength(1));
            board.connections.single.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.connected,
                failure: null,
              ),
            );
            await tester.runAsync(() async {
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            await tester.pump();
          }
          if (board.capabilityReplyGate case final delayedCapabilities?) {
            expect(board.productQueries, isEmpty);
            delayedCapabilities.complete();
            await tester.runAsync(() async {
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            await tester.pump();
          }

          final brand = find.byType(LitchAboutBrandButton);
          expect(brand, findsOneWidget);
          expect(tester.getSize(brand).height, greaterThan(0));
          final overlay = find.byKey(
            const ValueKey('connection-progress-overlay'),
          );
          expect(overlay, findsOneWidget);
          expect(tester.getSize(overlay).height, lessThan(844 / 2));
          expect(tester.widget<Material>(overlay).color!.a, lessThan(1));
          final compactOverlayRect = tester.getRect(overlay);
          expectSupportUnavailable();
          await tester.pump(const Duration(seconds: 2));
          expectSupportUnavailable();
          await tester.pump(const Duration(seconds: 1));
          expect(support.hitTestable(), findsOneWidget);
          expect(tester.widget<TextButton>(support).onPressed, isNotNull);
          expect(tester.getSize(support).height, greaterThanOrEqualTo(48));
          expect(find.bySemanticsLabel(supportLabel), findsOneWidget);
          final supportOverlayRect = tester.getRect(overlay);
          expect(supportOverlayRect.width, compactOverlayRect.width);
          expect(
            supportOverlayRect.height - compactOverlayRect.height,
            greaterThanOrEqualTo(48),
          );
          if (transportOverlayRect != null) {
            expect(compactOverlayRect, transportOverlayRect);
            expect(tester.getRect(brand), transportBrandRect);
            expect(
              find.descendant(
                of: overlay,
                matching: find.textContaining(advertisedBoard.name),
              ),
              findsOneWidget,
            );
            expect(
              find.descendant(of: overlay, matching: find.text('Foxie Litch')),
              findsNothing,
            );
          }
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(board.productQueries, isEmpty);
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          expect(
            tester
                .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
                .label,
            contains('Connecting'),
          );
          final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
          expect(cancel.hitTestable(), findsOneWidget);
          expect(find.text('Cancel'), findsOneWidget);
          expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
          expect(
            tester
                .getSemantics(cancel)
                .getSemanticsData()
                .hasAction(ui.SemanticsAction.tap),
            isTrue,
          );
          final main = find.byType(LitchMainScaffold, skipOffstage: false);
          final choice = find.byKey(const ValueKey('discovered-board-board-1'));
          expect(
            choice,
            findsOneWidget,
            reason:
                'Quiet proof retains useful board-choice content, not an empty page.',
          );
          void expectProductControlsLocked() {
            for (final guard in <bool Function(Widget)>[
              (widget) => widget is AbsorbPointer && widget.absorbing,
              (widget) => widget is ExcludeFocus && widget.excluding,
              (widget) => widget is ExcludeSemantics && widget.excluding,
            ]) {
              expect(
                find.ancestor(
                  of: choice,
                  matching: find.byWidgetPredicate(guard),
                ),
                findsOneWidget,
              );
            }
          }

          expect(
            main,
            findsOneWidget,
            reason:
                'The same mounted Main owns mode resolution behind discovery.',
          );
          expectProductControlsLocked();
          await tester.pump(const Duration(milliseconds: 200));
          expect(brand, findsOneWidget);
          expect(board.productQueries, isEmpty);
          expect(board.pairingCommands, isEmpty);
          if (nextGate == 'physical pairing') {
            tester.view.physicalSize = const Size(844, 390);
            await tester.pump();
            expect(find.byType(LinearProgressIndicator), findsOneWidget);
            expect(find.byType(CircularProgressIndicator), findsNothing);
            expect(cancel.hitTestable(), findsOneWidget);
            expect(
              tester.getSize(cancel).shortestSide,
              greaterThanOrEqualTo(48),
            );
            expect(board.productQueries, isEmpty);
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            expectProductControlsLocked();
            tester.view.physicalSize = const Size(390, 844);
            await tester.pump();
            expect(tester.getRect(overlay), supportOverlayRect);
          }

          if (nextGate == 'stale Bluetooth pairing') {
            board.connections.last.add(
              ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.disconnected,
                failure: const GenericFailure(
                  code: ConnectionError.failedToConnect,
                  message: iosPairingKeyMissingMarker,
                ),
              ),
            );
          } else if (nextGate == 'other-owner refusal') {
            board.setState(6, nextFlags: 5, nextReason: 8);
          } else {
            board.attempt = 7;
            board.setState(2, nextFlags: 4);
          }
          await tester.runAsync(() async {
            for (var turn = 0; turn < 30; ++turn) {
              await Future<void>.delayed(Duration.zero);
            }
          });
          await tester.pump();
          expect(overlay, findsNothing);
          if (nextGate == 'stale Bluetooth pairing') {
            expect(
              find.byKey(const ValueKey('bluetooth-pairing-recovery')),
              findsOneWidget,
            );
            expect(
              find
                  .byKey(const ValueKey('pairing-recovery-primary'))
                  .hitTestable(),
              findsOneWidget,
            );
          } else {
            expect(
              find.byKey(const ValueKey('owner-pairing-gate')),
              findsOneWidget,
            );
            if (nextGate == 'physical pairing') {
              expect(find.text('X + O'), findsOneWidget);
            } else {
              expect(service.ownerPairingRefusedDeviceId, 'board-1');
              final strings = lookupAppLocalizations(const Locale('en'));
              expect(find.text(strings.ownerPairingOtherOwner), findsOneWidget);
              expect(
                find.byKey(const ValueKey('owner-pairing-retry')),
                findsNothing,
              );
            }
            await tester.dragUntilVisible(
              find.byKey(const ValueKey('owner-pairing-cancel')),
              find.descendant(
                of: find.byKey(const ValueKey('owner-pairing-gate')),
                matching: find.byType(SingleChildScrollView),
              ),
              const Offset(0, -120),
            );
            expect(
              find.byKey(const ValueKey('owner-pairing-cancel')).hitTestable(),
              findsOneWidget,
            );
          }
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(board.productQueries, isEmpty);
          if (nextGate == 'physical pairing') {
            board.setState(5, nextFlags: 7);
            await tester.runAsync(() async {
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            await tester.pump();
            await tester.runAsync(() async {
              for (var turn = 0; turn < 30; ++turn) {
                await Future<void>.delayed(Duration.zero);
              }
            });
            expect(service.productReady, isTrue);
            expectSupportUnavailable();
            expect(tester.getRect(overlay), transportOverlayRect);
            expect(tester.getRect(brand), transportBrandRect);
            expect(
              find.descendant(
                of: overlay,
                matching: find.textContaining(advertisedBoard.name),
              ),
              findsOneWidget,
            );
            expect(
              board.productQueries,
              contains(ConnectionSession.litchcraftStateCode),
              reason: 'Mode inspection resumes only after durable owner proof.',
            );
          }
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox());
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          for (var turn = 0; turn < 80 && !disposed; turn++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          expect(disposed, isTrue);
          await disposal;
          await tester.runAsync(() async {
            for (final connection in board.connections) {
              await connection.close();
            }
            await board.packets.close();
            await board.sessionPackets.close();
          });
          semantics.dispose();
        }
        if (nextGate == 'other-owner refusal') {
          for (final cancelProof in [false, true]) {
            final retainedBoard = OwnerPairingBoard()
              ..state = 6
              ..flags = 5;
            final retainedService = _OwnerVerificationUiService(retainedBoard);
            final craft = _NavigationCraftController(retainedService)
              ..connectedBoardActive = true;
            final library = LitchcraftLibrary(craft);
            await library.ready;
            final retainedSemantics = tester.ensureSemantics();
            try {
              await tester.runAsync(() async {
                await connectBoard(retainedService, retainedBoard);
                for (
                  var turn = 0;
                  turn < 600 && retainedService.ownerPairingStatus == null;
                  ++turn
                ) {
                  await Future<void>.delayed(Duration.zero);
                }
                expect(
                  retainedService.ownerPairingStatus?.state,
                  OwnerPairingSessionState.ownedOther,
                );
              });
              retainedBoard.setState(5, nextFlags: 7);
              await tester.runAsync(() async {
                for (var turn = 0; turn < 30; ++turn) {
                  await Future<void>.delayed(Duration.zero);
                }
              });
              await tester.pump();
              await tester.runAsync(() async {
                for (var turn = 0; turn < 30; ++turn) {
                  await Future<void>.delayed(Duration.zero);
                }
              });
              await tester.pump();
              expect(
                retainedService.productReady,
                isTrue,
                reason:
                    'state=${retainedService.ownerPairingStatus?.state} flags=${retainedService.ownerPairingStatus?.flags} busy=${retainedService.ownerPairingBusy}',
              );
              await tester.pumpWidget(
                MaterialApp(
                  home: ConnectionGateScreen(
                    bleService: retainedService,
                    craftController: craft,
                    craftLibrary: library,
                    firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
                  ),
                ),
              );
              await tester.pumpAndSettle();
              final screen = find.byType(LitchcraftScreen);
              final editor = tester.state(screen);
              await tester.runAsync(() async {
                await retainedService.disconnect();
                retainedBoard.state = 6;
                retainedBoard.flags = 5;
                retainedBoard.revision++;
                await connectBoard(retainedService, retainedBoard);
                for (var turn = 0; turn < 30; ++turn) {
                  await Future<void>.delayed(Duration.zero);
                }
              });
              await tester.pump();
              expect(retainedService.isConnected, isTrue);
              expect(retainedService.productReady, isFalse);
              expect(retainedService.ownerPairingBusy, isTrue);
              expect(
                find.byKey(const ValueKey('owner-pairing-gate')),
                findsNothing,
              );
              expect(tester.state(screen), same(editor));
              expect(
                find.byKey(const ValueKey('connection-progress-overlay')),
                findsOneWidget,
                reason:
                    'Unready owner proof keeps accessible progress above the locked retained canvas.',
              );
              expect(find.byType(LinearProgressIndicator), findsOneWidget);
              final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
              expect(cancel.hitTestable(), findsOneWidget);
              expect(
                tester
                    .getSemantics(cancel)
                    .getSemanticsData()
                    .hasAction(ui.SemanticsAction.tap),
                isTrue,
              );
              for (final guard in <bool Function(Widget)>[
                (widget) => widget is AbsorbPointer && widget.absorbing,
                (widget) => widget is ExcludeFocus && widget.excluding,
                (widget) => widget is ExcludeSemantics && widget.excluding,
              ]) {
                expect(
                  find.ancestor(
                    of: screen,
                    matching: find.byWidgetPredicate(guard),
                  ),
                  findsOneWidget,
                );
              }
              if (cancelProof) {
                await tester.tap(cancel);
                await tester.pump();
                await tester.runAsync(() async {
                  for (var turn = 0; turn < 30; ++turn) {
                    await Future<void>.delayed(Duration.zero);
                  }
                });
                await tester.pump();
                expect(retainedService.isConnected, isFalse);
                retainedService.advertise(
                  const LitchBoard(
                    id: 'board-1',
                    name: 'Studio Board',
                    rssi: -40,
                  ),
                );
                await tester.pump();
                expect(
                  find.byKey(const ValueKey('discovered-board-board-1')),
                  findsOneWidget,
                );
              }
              retainedBoard.setState(5, nextFlags: 7);
              await tester.runAsync(() async {
                for (var turn = 0; turn < 30; ++turn) {
                  await Future<void>.delayed(Duration.zero);
                }
              });
              await tester.pump();
              expect(
                find.byKey(const ValueKey('connection-progress-overlay')),
                findsNothing,
              );
              if (cancelProof) {
                expect(retainedService.isConnected, isFalse);
                expect(
                  find.byKey(const ValueKey('discovered-board-board-1')),
                  findsOneWidget,
                );
              } else {
                expect(retainedService.productReady, isTrue);
                expect(tester.state(screen), same(editor));
                final paintsBefore = craft.paintCalls;
                await tester.tapAt(
                  tester.getCenter(
                    find.byKey(const ValueKey('litchcraft-canvas')),
                  ),
                );
                await tester.pump();
                expect(craft.paintCalls, greaterThan(paintsBefore));
              }
            } finally {
              await tester.pumpWidget(const SizedBox());
              await library.flush();
              library.dispose();
              craft.dispose();
              var retainedDisposed = false;
              final retainedDisposal = retainedService.dispose().whenComplete(
                () => retainedDisposed = true,
              );
              for (var turn = 0; turn < 80 && !retainedDisposed; turn++) {
                await tester.pump(const Duration(milliseconds: 20));
                await tester.runAsync(
                  () => Future<void>.delayed(Duration.zero),
                );
              }
              expect(retainedDisposed, isTrue);
              await retainedDisposal;
              await tester.runAsync(() async {
                for (final connection in retainedBoard.connections) {
                  await connection.close();
                }
                await retainedBoard.packets.close();
                await retainedBoard.sessionPackets.close();
              });
              retainedSemantics.dispose();
            }
          }
        }
      },
      variant: nextGate == 'physical pairing'
          ? quietProofTextScale
          : const DefaultTestVariant(),
    );
  }

  for (final (size, manualReconnect) in <(Size, bool)>[
    (const Size(390, 844), false),
    (const Size(390, 844), true),
    (const Size(844, 390), false),
    (const Size(844, 390), true),
  ]) {
    testWidgets(
      '${manualReconnect ? 'manual reconnect' : 'cold connection'} keeps useful discovery under compact progress and routes directly to Craft $size',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        final portrait = size.width < size.height;
        final repository = _EmptyGameLibraryRepository(
          snapshot: GameLibrarySnapshot(games: [_recentArchive(1)]),
        );
        addTearDown(repository.close);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
          'enable_app_console': manualReconnect && portrait,
        });
        final service = _CraftNavigationBle()..active = true;
        final craft = LitchcraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        final semantics = tester.ensureSemantics();
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        try {
          if (manualReconnect) {
            service.emitConnection('board-1', DeviceConnectionState.connected);
          }
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey('connection-underlay-review-root'),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: service,
                gameLibrary: repository,
                craftController: craft,
                craftLibrary: library,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          await tester.pump();
          if (manualReconnect) {
            await tester.pumpAndSettle();
            expect(find.byType(LitchcraftScreen), findsOneWidget);
            await tester.tap(
              find.byKey(const ValueKey('connected-board-status-action')),
            );
            await tester.pump(const Duration(milliseconds: 200));
            await tester.tap(
              find.byKey(const ValueKey('board-menu-disconnect')),
            );
            await tester.pump();
            await tester.pump();
            expect(service.disconnectCalls, 1);
            expect(service.active, isTrue);
            expect(find.byType(LitchcraftScreen), findsOneWidget);
            await tester.tap(
              find.byKey(
                ValueKey(
                  portrait
                      ? 'disconnected-board-status-action'
                      : 'connected-board-status-action',
                ),
              ),
            );
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 250));
          }
          service.emitAvailability(BleAvailability.ready);
          service.emitBoards(const <LitchBoard>[
            LitchBoard(id: 'board-1', name: 'Comet 1-3.3', rssi: -40),
          ]);
          await tester.pump();
          final brand = find.byType(LitchAboutBrandButton);
          expect(brand, findsOneWidget);
          final boardCard = find.byKey(
            const ValueKey('discovered-board-board-1'),
          );
          final boardCardRect = tester.getRect(boardCard);
          final libraryAction = find.byKey(
            ValueKey(
              size.width < size.height
                  ? 'game-library-action'
                  : 'discovery-review-games',
            ),
          );
          expect(libraryAction, findsOneWidget);
          final libraryRect = tester.getRect(libraryAction);
          service.inspectionGate = Completer<void>();
          await tester.tap(find.text('Connect'));
          await tester.pump();

          final captureName =
              'underlay-${Theme.of(tester.element(find.byType(ConnectionGateScreen))).platform.name}-${manualReconnect ? 'manual' : 'cold'}-${size.width.toInt()}';
          Future<void> capturePending(String phase) => captureHealthReview(
            tester,
            '$captureName-$phase',
            boundaryFinder: find.byKey(
              const ValueKey('connection-underlay-review-root'),
            ),
          );
          Rect? pendingOverlayRect;
          Rect? pendingBrandRect;
          void expectPendingShell() {
            expect(brand, findsOneWidget);
            if (portrait) {
              expect(tester.getRect(brand).top, lessThan(56));
              final connectingLabel = find.descendant(
                of: find.byType(AppBar),
                matching: find.text('Connecting…'),
              );
              expect(connectingLabel, findsOneWidget);
              expectReviewTextFits(
                tester,
                connectingLabel,
                'Connecting header',
              );
            }
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            expect(service.homeReadRequests, 0);
            expect(
              boardCard,
              findsOneWidget,
              reason:
                  'The chosen discovery content must not disappear behind progress.',
            );
            expect(tester.getRect(boardCard), boardCardRect);
            for (final button in tester.widgetList<ButtonStyleButton>(
              find.descendant(
                of: boardCard,
                matching: find.byWidgetPredicate(
                  (widget) => widget is ButtonStyleButton,
                ),
              ),
            )) {
              expect(button.onPressed, isNull);
            }
            expect(
              find.byKey(const ValueKey('offline-board-status-action')),
              findsNothing,
            );
            expect(libraryAction, findsOneWidget);
            expect(tester.getRect(libraryAction), libraryRect);
            final progress = find.byKey(
              const ValueKey('chosen-board-connection'),
            );
            expect(progress, findsOneWidget);
            final overlay = find.byKey(
              const ValueKey('connection-progress-overlay'),
            );
            expect(overlay, findsOneWidget);
            pendingOverlayRect ??= tester.getRect(overlay);
            expect(tester.getRect(overlay), pendingOverlayRect);
            if (portrait) {
              pendingBrandRect ??= tester.getRect(brand);
              expect(tester.getRect(brand), pendingBrandRect);
            }
            expect(
              find.descendant(
                of: overlay,
                matching: find.textContaining('Comet 1-3.3'),
              ),
              findsOneWidget,
            );
            expect(
              tester.getSize(overlay).height,
              lessThan(size.height / 2),
              reason:
                  'Progress must occupy a compact panel over the app shell.',
            );
            final surface = tester.widget<Material>(overlay);
            expect(
              surface.color?.a ?? 1,
              lessThan(1),
              reason:
                  'The progress panel must leave its underlying shell visible.',
            );
            final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
            expect(cancel.hitTestable(), findsOneWidget);
            expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
            for (final destination in <String>['Play', 'Litchcraft']) {
              final announcedDestination = find.bySemanticsLabel(destination);
              for (final node in announcedDestination.evaluate()) {
                expect(
                  tester
                      .getSemantics(find.byElementPredicate((e) => e == node))
                      .flagsCollection
                      .isSelected,
                  ui.Tristate.isFalse,
                  reason: 'Unknown board mode must not select $destination.',
                );
              }
            }
            expect(tester.takeException(), isNull);
          }

          await capturePending('transport');
          expectPendingShell();
          await tester.pump(const Duration(milliseconds: 16));
          expectPendingShell();
          service.completeNextConnect(true);
          await tester.pump();
          await capturePending('proof');
          for (final frame in <Duration>[
            Duration.zero,
            const Duration(milliseconds: 16),
            const Duration(milliseconds: 200),
          ]) {
            await tester.pump(frame);
            expectPendingShell();
          }
          expect(service.commands, contains(LitchcraftProtocol.snapshot));
          service.emitBoards(const []);
          await tester.pump();
          expectPendingShell();
          service.inspectionGate!.complete();
          // Check each frame through the route change: waiting only for settle
          // would miss the brief Play flash reported after manual reconnect.
          for (var frame = 0; frame < 5; ++frame) {
            await tester.pump(const Duration(milliseconds: 16));
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            expect(service.homeReadRequests, 0);
            expect(brand, findsOneWidget);
          }
          await tester.runAsync(() async {
            await Future<void>.delayed(Duration.zero);
          });
          await tester.pumpAndSettle();
          expect(
            service.commands,
            isNot(contains(LitchcraftProtocol.exit)),
            reason: 'Choosing a board must not exit its active Craft session.',
          );
          expect(
            find.byType(LitchcraftScreen),
            findsOneWidget,
            reason:
                'boardActive=${service.active}; commands=${service.commands}; '
                'mode=${craft.inspectedBoardActive}; selected=${craft.selected}; '
                'controllerActive=${craft.boardActive}',
          );
          expect(_activeNavigationIndex(tester), 1);
          expect(service.commands, isNot(contains(LitchcraftProtocol.enter)));
          expect(
            find.byKey(const ValueKey('chosen-board-connection')),
            findsNothing,
          );
          expect(service.homeReadRequests, 0);
          await tester.pumpWidget(const SizedBox());
          await library.flush();
        } finally {
          await tester.pumpWidget(const SizedBox());
          semantics.dispose();
        }
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets(
    'connected discovery navigation preserves explicit Setup during mode proof',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      try {
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pump();
        service.emitAvailability(BleAvailability.ready);
        service.emitBoards(const [
          LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        ]);
        await tester.pump();
        await tester.tap(find.text('Connect'));
        service.completeNextConnect(true);
        await tester.pump();
        await tester.pump();
        expect(craft.adoptCalls, 1);
        final main = tester.state(
          find.byType(LitchMainScaffold, skipOffstage: false),
        );
        expect(
          find.byKey(const ValueKey('discovered-board-board-1')),
          findsOneWidget,
        );
        await tester.tap(_navigationAction(tester, 'setup'));
        await tester.pump();
        expect(
          find.byKey(const ValueKey('connection-progress-overlay')),
          findsNothing,
        );
        expect(find.byType(SettingsScreen), findsOneWidget);
        expect(_activeNavigationIndex(tester), 2);
        craft.showInspectedMode(true);
        craft.adoptGate!.complete(true);
        await tester.pumpAndSettle();
        expect(tester.state(find.byType(LitchMainScaffold)), same(main));
        expect(_activeNavigationIndex(tester), 2);
        expect(craft.adoptCalls, 1);
        expect(craft.enterCalls, 0);
      } finally {
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      }
    },
  );

  testWidgets(
    'failed reconnect mode proof cannot route from retained Craft authority',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..connectedBoardActive = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      try {
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(_activeNavigationIndex(tester), 1);
        await tester.tap(
          find.byKey(const ValueKey('connected-board-status-action')),
        );
        await tester.pump(const Duration(milliseconds: 200));
        await tester.tap(find.byKey(const ValueKey('board-menu-disconnect')));
        await tester.pump();
        await tester.pump();
        expect(craft.selected, isTrue);
        expect(craft.boardActive, isTrue);
        await tester.tap(
          find.byKey(const ValueKey('disconnected-board-status-action')),
        );
        await tester.pump();
        service.emitAvailability(BleAvailability.ready);
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        ]);
        await tester.pump();
        craft.failModeInspection = true;
        await tester.tap(find.text('Connect'));
        await tester.pump();
        service.completeNextConnect(true);
        await tester.pumpAndSettle();
        expect(craft.inspectedBoardActive, isNull);
        expect(find.byType(HomeScreen), findsOneWidget);
        expect(_activeNavigationIndex(tester), 0);

        // Ordinary error/recovery notifications do not prove the mode of this
        // new link, even while the controller retains its old drawing cache.
        craft.showStatus(LitchcraftStatus.error);
        await tester.pump();
        expect(_activeNavigationIndex(tester), 0);
        craft.showStatus(LitchcraftStatus.connecting);
        await tester.pump();
        expect(_activeNavigationIndex(tester), 0);
        craft.showInspectedMode(true);
        await tester.pumpAndSettle();
        expect(_activeNavigationIndex(tester), 1);
        expect(craft.enterCalls, 0);
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      }
    },
  );

  for (final config in <({Size size, double scale, Locale locale})>[
    (size: Size(390, 844), scale: 1, locale: Locale('en')),
    (size: Size(320, 844), scale: 2, locale: Locale('de')),
    (size: Size(390, 844), scale: 2, locale: Locale('fr')),
  ]) {
    testWidgets(
      'chosen board connection keeps localized progress accessible ${config.size} ${config.locale} ${config.scale}x',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = config.size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        final repository = _EmptyGameLibraryRepository();
        addTearDown(repository.close);
        final semantics = tester.ensureSemantics();
        try {
          await tester.pumpWidget(
            MaterialApp(
              locale: config.locale,
              theme: buildLitchTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey('badge-review-root'),
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(config.scale)),
                  child: child!,
                ),
              ),
              home: ConnectionGateScreen(
                bleService: service,
                gameLibrary: repository,
              ),
            ),
          );
          await tester.pump();
          service.emitBoards(const [
            LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
            LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -45),
          ]);
          await tester.pump();
          final connect = find
              .descendant(
                of: find.byKey(const ValueKey('discovered-board-board-1')),
                matching: find.byType(FilledButton),
              )
              .first;
          await tester.ensureVisible(connect);
          await tester.pumpAndSettle();
          await tester.tap(connect);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 200));
          final strings = AppLocalizations.of(
            tester.element(find.byType(ConnectionGateScreen)),
          );
          final namedProgress = strings.connectingToBoard('Foxie One');
          final label = find.byKey(const ValueKey('chosen-board-phase'));
          expect(tester.widget<Text>(label).data, namedProgress);
          final progress = find.byType(LinearProgressIndicator);
          final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
          expect(
            find.descendant(
              of: find.byKey(const ValueKey('discovered-board-board-1')),
              matching: find.text('Foxie One'),
            ),
            findsOneWidget,
          );
          expect(label, findsOneWidget);
          expect(progress, findsOneWidget);
          expect(tester.getSemantics(label).label, contains(namedProgress));
          expect(
            tester
                .getSemantics(cancel)
                .getSemanticsData()
                .hasAction(ui.SemanticsAction.tap),
            isTrue,
          );
          expect(cancel.hitTestable(), findsOneWidget);
          expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
          final paragraph = tester.renderObject<RenderParagraph>(label);
          expect(paragraph.didExceedMaxLines, isFalse);
          expect(tester.takeException(), isNull);
          final name =
              'connecting-${Theme.of(tester.element(label)).platform.name}-${config.size.width.toInt()}-${config.scale.toInt()}x-${config.locale}';
          await captureHealthReview(
            tester,
            '$name-pending',
            boundaryFinder: find.byKey(const ValueKey('badge-review-root')),
          );
          service.completeNextConnect(false);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 200));
          expect(progress, findsOneWidget);
          expect(label, findsOneWidget);
          await tester.tap(cancel);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 200));
          expect(progress, findsNothing);
          expect(label, findsNothing);
          await captureHealthReview(
            tester,
            '$name-ended',
            boundaryFinder: find.byKey(const ValueKey('badge-review-root')),
          );
          expect(service.connectCalls, ['board-1']);
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
        } finally {
          semantics.dispose();
        }
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  for (final size in [const Size(414, 896), const Size(896, 414)]) {
    testWidgets(
      'Play disconnect after acknowledged Craft departure opens Connect $size',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = size;
        addTearDown(tester.view.reset);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _CraftNavigationBle()..active = true;
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = LitchcraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await library.dismissWelcome();
        var cleanedUp = false;
        Future<void> cleanup() async {
          if (cleanedUp) return;
          cleanedUp = true;
          library.dispose();
          craft.dispose();
          await service.dispose();
        }

        addTearDown(cleanup);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchDarkTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byType(LitchcraftScreen), findsOneWidget);
        final document = library.animationDocument;
        await tester.tap(_navigationAction(tester, 'play'));
        await tester.pumpAndSettle();
        expect(
          service.commands
              .where((opcode) => opcode == LitchcraftProtocol.exit)
              .length,
          1,
        );
        expect(craft.boardActive, isFalse);
        expect(service.isConnected, isTrue);
        expect(find.byType(HomeScreen), findsOneWidget);
        service.emitConnection('board-1', DeviceConnectionState.disconnected);
        service.emitBoards(const <LitchBoard>[]);
        await tester.pump();
        await tester.pump();
        expect(
          find.byKey(const ValueKey('connection-gate-content')),
          findsOneWidget,
          reason: 'Unexpected link loss from Play immediately exposes Connect.',
        );
        expect(find.byType(HomeScreen), findsNothing);
        expect(library.animationDocument, same(document));
        final attempts = service.connectCalls.length;
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        ]);
        await tester.pump(const Duration(milliseconds: 900));
        expect(service.connectCalls.length, attempts + 1);
        service.completeNextConnect(true);
        // Notification close completion runs on the real event queue.
        await tester.runAsync(() async {
          await Future<void>.delayed(Duration.zero);
          await craft.settleModeResolution();
        });
        await tester.pumpAndSettle();
        expect(find.byType(HomeScreen), findsOneWidget);
        expect(
          find.byKey(const ValueKey('connection-gate-content')),
          findsNothing,
        );
        expect(library.animationDocument, same(document));
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await cleanup();
        await tester.pump();
      },
    );
  }

  for (final platform in [TargetPlatform.iOS, TargetPlatform.android]) {
    for (final controlsLeft in [false, true]) {
      testWidgets(
        '$platform landscape uses protected edges without added canvas wells $controlsLeft',
        (tester) async {
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = const Size(896, 414);
          addTearDown(tester.view.reset);
          SharedPreferences.setMockInitialValues(<String, Object>{
            'litchcraft_discovery_dismissed': true,
          });
          final service = _ConnectionGateBleService()
            ..reviewBoardRotationKnown = true;
          service.emitConnection('board-1', DeviceConnectionState.connected);
          final craft = _NavigationCraftController(service)
            ..simulatePhysicalMode(true);
          final library = LitchcraftLibrary(craft);
          await library.ready;
          await library.dismissWelcome();
          addTearDown(() async {
            library.dispose();
            craft.dispose();
            await service.dispose();
          });
          final vertical = platform == TargetPlatform.iOS ? 21.0 : 24.0;
          final camera = platform == TargetPlatform.iOS ? 44.0 : 30.0;
          final insets = controlsLeft
              ? EdgeInsets.fromLTRB(0, vertical, camera, vertical)
              : EdgeInsets.fromLTRB(camera, vertical, 0, vertical);
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchDarkTheme().copyWith(platform: platform),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(padding: insets, viewPadding: insets),
                child: OrientationWorkspace(
                  sideReader: () async => controlsLeft
                      ? DeviceLandscapeSide.left
                      : DeviceLandscapeSide.right,
                  child: child!,
                ),
              ),
              home: LitchMainScaffold(
                bleService: service,
                boardId: 'board-1',
                boards: const [],
                onDisconnect: () async {},
                craftController: craft,
                craftLibrary: library,
                initialPageIndex: 0,
                initialCraftModeResolved: true,
                landscapeRibbonSide: controlsLeft
                    ? Alignment.centerLeft
                    : Alignment.centerRight,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          await tester.pumpAndSettle();
          Rect rect(String key) => tester.getRect(find.byKey(ValueKey(key)));
          final board = rect('craft-ribbon-board');
          final quick = rect('craft-quick-actions');
          final palette = rect('craft-color-wheel-rail');
          final inspector = rect('craft-persistent-inspector');
          expect(board.top, closeTo(vertical, 0.01));
          expect(board.bottom, closeTo(414 - vertical, 0.01));
          expect(board.width, closeTo(board.height, 0.01));
          if (controlsLeft) {
            expect(inspector.left, closeTo(10, 0.01));
            expect(quick.right, closeTo(896 - camera, 0.01));
            expect(board.left - palette.right, closeTo(12, 0.01));
            expect(quick.left - board.right, closeTo(12, 0.01));
          } else {
            expect(quick.left, closeTo(camera, 0.01));
            expect(inspector.right, closeTo(886, 0.01));
            expect(board.left - quick.right, closeTo(12, 0.01));
            expect(palette.left - board.right, closeTo(12, 0.01));
          }
          expect(quick.width, 76);
          expect(palette.width, 48);
          await tester.tap(_navigationAction(tester, 'play'));
          await tester.pumpAndSettle();
          final play = rect('home-chessboard-surface');
          expect(play.size, board.size);
          expect(play.top, closeTo(board.top, 0.01));
          final nav = rect('main-landscape-navigation');
          expect(nav.left, closeTo(inspector.left, 0.01));
          expect(nav.right, closeTo(inspector.right, 0.01));
          expect(nav.bottom, closeTo(inspector.bottom, 0.01));
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
        },
      );
    }
  }

  for (final scenario in [
    (
      'android-portrait',
      const Size(384, 832),
      const EdgeInsets.fromLTRB(0, 28, 0, 24),
      1.0,
    ),
    (
      'android-portrait-200',
      const Size(384, 832),
      const EdgeInsets.fromLTRB(0, 28, 0, 24),
      2.0,
    ),
    (
      'android-landscape-left',
      const Size(832, 384),
      const EdgeInsets.fromLTRB(34, 0, 48, 0),
      1.0,
    ),
    (
      'android-landscape-left-200',
      const Size(832, 384),
      const EdgeInsets.fromLTRB(34, 0, 48, 0),
      2.0,
    ),
    (
      'android-landscape-right',
      const Size(832, 384),
      const EdgeInsets.fromLTRB(48, 0, 34, 0),
      1.0,
    ),
    (
      'android-landscape-right-200',
      const Size(832, 384),
      const EdgeInsets.fromLTRB(48, 0, 34, 0),
      2.0,
    ),
    (
      'iphone-portrait-200',
      const Size(390, 844),
      const EdgeInsets.fromLTRB(0, 47, 0, 34),
      2.0,
    ),
    (
      'iphone-landscape-200',
      const Size(844, 390),
      const EdgeInsets.fromLTRB(54, 0, 22, 21),
      2.0,
    ),
  ]) {
    testWidgets('Console keyboard retains warm pager geometry ${scenario.$1}', (
      tester,
    ) async {
      var keyboardPreparations = 0;
      if (scenario.$1.startsWith('iphone')) {
        const channel = MethodChannel(
          'com.foxieproducts.litch/interface_orientation',
        );
        // AppDelegate acknowledges editing preparation before Flutter focuses
        // the input. Widget tests have no native orientation handler.
        tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          channel,
          (call) async {
            if (call.method == 'editing' && call.arguments == true) {
              ++keyboardPreparations;
            }
            return null;
          },
        );
        addTearDown(
          () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
            channel,
            null,
          ),
        );
      }
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      final size = scenario.$2;
      final consoleFont = Platform.environment['LITCH_CONSOLE_FONT'];
      if (consoleFont != null) {
        await tester.runAsync(() async {
          await (FontLoader('monospace')..addFont(
                File(
                  consoleFont,
                ).readAsBytes().then((bytes) => bytes.buffer.asByteData()),
              ))
              .load();
        });
      }
      final safe = scenario.$3;
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      tester.view.viewPadding = FakeViewPadding(
        left: safe.left,
        top: safe.top,
        right: safe.right,
        bottom: safe.bottom,
      );
      tester.view.padding = tester.view.viewPadding;
      tester.platformDispatcher.textScaleFactorTestValue = scenario.$4;
      addTearDown(tester.view.reset);
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      SharedPreferences.setMockInitialValues({'enable_app_console': true});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(
        tester,
        service,
        layoutSize: size,
        systemInsets: safe,
        liveKeyboardPadding: true,
        textScaler: TextScaler.linear(scenario.$4),
        theme: buildLitchTheme().copyWith(
          platform: scenario.$1.startsWith('iphone')
              ? TargetPlatform.iOS
              : TargetPlatform.android,
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          ValueKey(
            size.width > size.height
                ? 'landscape-console-action'
                : 'main-destination-console',
          ),
        ),
      );
      expect(keyboardPreparations, 0);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('console-terminal-surface')).hitTestable(),
        findsOneWidget,
      );
      if (scenario.$1.startsWith('iphone')) expect(keyboardPreparations, 1);
      await tester.pumpAndSettle();
      service.debugAppendConsolePacket(
        'Connected to Foxie Litch\nType help for commands\n'.codeUnits,
      );
      await tester.pumpAndSettle();
      final pager = find.byKey(const ValueKey('main-pages'));
      final before = tester.getRect(pager);
      final keyboardButton = find.byKey(
        const ValueKey('console-hide-keyboard'),
      );
      expect(keyboardButton.hitTestable(), findsOneWidget);
      expect(
        tester.getSize(keyboardButton).shortestSide,
        greaterThanOrEqualTo(48),
      );
      await tester.tap(keyboardButton);
      await tester.pump();
      expect(find.byTooltip('Show keyboard'), findsOneWidget);
      if (size.width > size.height) {
        final terminal = tester.getRect(
          find.byKey(const ValueKey('console-terminal-surface')),
        );
        final commandRow = tester.getRect(
          find.byKey(const ValueKey('console-command-row')),
        );
        final compactNavigation = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        expect(terminal.bottom - commandRow.bottom, closeTo(4, .01));
        expect(compactNavigation.center.dx, greaterThan(terminal.center.dx));
        expect(
          tester.getRect(keyboardButton).right,
          lessThanOrEqualTo(compactNavigation.left),
        );
      }
      await captureHealthReview(tester, 'console-${scenario.$1}-closed');
      await tester.tap(find.byTooltip('Show keyboard'));
      await tester.pump();
      for (final inset in [
        40.0,
        80.0,
        120.0,
        160.0,
        if (size.height > size.width) 320.0,
        160.0,
        120.0,
        80.0,
        40.0,
        0.0,
      ]) {
        tester.view.viewInsets = FakeViewPadding(bottom: inset);
        tester.view.padding = FakeViewPadding(
          left: safe.left,
          top: safe.top,
          right: safe.right,
          bottom: inset > 0 ? 0 : safe.bottom,
        );
        await tester.pump(const Duration(milliseconds: 16));
        expect(
          tester.getRect(pager),
          before,
          reason: 'IME animation must not resize every warm page',
        );
        final input = tester.getRect(
          find.byKey(const ValueKey('console-command-input')),
        );
        expect(input.bottom, lessThanOrEqualTo(size.height - inset + .01));
        expect(tester.takeException(), isNull);
        if (inset == 160) {
          await captureHealthReview(tester, 'console-${scenario.$1}-keyboard');
        }
      }
      await tester.pumpAndSettle();
      expect(find.byTooltip('Show keyboard').hitTestable(), findsOneWidget);
      await tester.tap(find.byTooltip('Show keyboard'));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(
              find.byKey(const ValueKey('console-command-input')),
            )
            .focusNode!
            .hasFocus,
        isTrue,
      );
      tester.view.viewInsets = const FakeViewPadding(bottom: 160);
      tester.view.padding = FakeViewPadding(
        left: safe.left,
        top: safe.top,
        right: safe.right,
      );
      await tester.pump();
      expect(find.byTooltip('Hide keyboard'), findsOneWidget);
    });
  }
  testWidgets(
    'returning to Craft waits for board ownership but never draft storage',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final store = _DelayedCraftStore(await SharedPreferences.getInstance());
      final library = LitchcraftLibrary(craft, store: store);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      Finder tab(String label) => find
          .descendant(
            of: _activeNavigation(tester),
            matching: find.byType(NavigationDestination),
          )
          .at(label == 'Play' ? 0 : 1);
      await tester.tap(tab('Litchcraft'));
      await tester.pumpAndSettle();
      store.gate = Completer<void>();
      await tester.tap(tab('Play'));
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 1);
      expect(store.gate!.isCompleted, isFalse);
      await tester.tap(tab('Litchcraft'));
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 2);
      expect(craft.selected, isTrue);
      expect(store.writes, 1);
      store.gate!.complete();
      await tester.pumpAndSettle();
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final newerPage in ['Play', 'Litchcraft', 'same Litchcraft']) {
    testWidgets(
      'pending Firmware navigation cannot replace newer $newerPage intent',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        VoidCallback? requestFirmware;
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: service.disconnect,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, request, _, _) {
                requestFirmware = request;
                return const Text('Firmware content');
              },
            ),
          ),
        );
        await tester.pumpAndSettle();
        Finder tab(String label) => find
            .descendant(
              of: _activeNavigation(tester),
              matching: find.byType(NavigationDestination),
            )
            .at(label == 'Play' ? 0 : 1);
        await tester.tap(tab('Litchcraft'));
        await tester.pumpAndSettle();
        craft.exitGate = Completer<void>();
        requestFirmware!();
        await tester.pump();
        expect(craft.leaveCalls, 1);
        if (newerPage != 'same Litchcraft') {
          await tester.tap(tab('Play'));
          await tester.pump();
        }
        if (newerPage != 'Play') {
          await tester.tap(tab('Litchcraft'));
          await tester.pump();
        }
        craft.exitGate!.complete();
        await tester.pumpAndSettle();
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          newerPage == 'Play' ? 0 : 1,
        );
        expect(find.text('Board update'), findsNothing);
        expect(craft.leaveCalls, 1);
        expect(craft.selected, newerPage != 'Play');
        expect(craft.enterCalls, newerPage == 'Play' ? 1 : 2);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets(
    'passive departure failure is dismissed when artwork is selected again',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      Finder tab(String label) => find
          .descendant(
            of: _activeNavigation(tester),
            matching: find.byType(NavigationDestination),
          )
          .at(label == 'Play' ? 0 : 1);
      await tester.tap(tab('Litchcraft'));
      await tester.pumpAndSettle();
      craft.rejectExit = true;
      await tester.tap(tab('Play'));
      await tester.pumpAndSettle();
      expect(find.byType(SnackBarAction), findsNothing);
      craft.rejectExit = false;
      await tester.tap(tab('Litchcraft'));
      await tester.pumpAndSettle();
      expect(find.byType(SnackBarAction), findsNothing);
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 1);
      expect(craft.selected, isTrue);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final failure in ['none', 'exit', 'storage']) {
    testWidgets(
      'Play renders next frame while paint, Exit and draft save wait ($failure)',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _CraftNavigationBle();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = LitchcraftController(service);
        final store = _DelayedCraftStore(await SharedPreferences.getInstance());
        final library = LitchcraftLibrary(craft, store: store);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: service.disconnect,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) =>
                  const Text('Firmware content'),
            ),
          ),
        );
        await tester.pumpAndSettle();
        Finder tab(String label) => find
            .descendant(
              of: _activeNavigation(tester),
              matching: find.byType(NavigationDestination),
            )
            .at(label == 'Play' ? 0 : 1);
        await tester.tap(tab('Litchcraft'));
        await tester.pumpAndSettle();
        expect(craft.status, LitchcraftStatus.live);
        service.commands.clear();
        service.paintGate = Completer<void>();
        service.exitGate = Completer<void>();
        store.gate = Completer<void>();
        store.fail = failure == 'storage';
        service.rejectExit = failure == 'exit';
        craft.paintSquares([7]);
        craft.endStroke();
        await tester.pump(const Duration(milliseconds: 1));
        final tabTappedAt = tester.binding.clock.now();
        await tester.tap(tab('Play'));
        await tester.pump(const Duration(milliseconds: 16));
        final renderedAfter = tester.binding.clock.now().difference(
          tabTappedAt,
        );
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          0,
        );
        expect(
          tester
              .widget<PageView>(
                find.byWidgetPredicate((widget) => widget is PageView),
              )
              .controller!
              .page,
          0,
        );
        expect(find.byType(HomeScreen).hitTestable(), findsOneWidget);
        expect(service.commands, [
          LitchcraftProtocol.paint,
          LitchcraftProtocol.exit,
        ]);
        expect(store.writes, 1);
        for (var repeat = 0; repeat < 5; repeat++) {
          await tester.tap(tab('Play'));
          await tester.pump();
        }
        await tester.pump(const Duration(seconds: 2));
        expect(
          store.writes,
          1,
          reason: 'pending storage is coalesced, including draft debounce',
        );
        service.paintGate!.complete();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 1));
        expect(service.commands, [
          LitchcraftProtocol.paint,
          LitchcraftProtocol.exit,
        ]);
        expect(
          service.active,
          isTrue,
          reason: 'showing Play does not claim board Exit is acknowledged',
        );
        service.exitGate!.complete();
        store.gate!.complete();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 16));
        if (failure != 'none') {
          expect(find.text('Try again'), findsNothing);
          expect(find.byType(SnackBarAction), findsNothing);
          service.rejectExit = false;
          store.fail = false;
          await tester.pump(const Duration(milliseconds: 500));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        await tester.pumpAndSettle();
        expect(service.active, isFalse);
        expect(library.saveFailed, isFalse);
        expect(
          service.commands.where((op) => op == LitchcraftProtocol.snapshot),
          isEmpty,
        );
        final saved = await store.load();
        expect(saved.draft!.canvas.colors[28], 0xff7430);
        expect(store.writes, failure == 'storage' ? 2 : 1);
        debugPrint(
          'LITCHCRAFT_NAV_METRIC failure=$failure '
          'render_ms=${renderedAfter.inMilliseconds} '
          'settled_ms=${tester.binding.clock.now().difference(tabTappedAt).inMilliseconds} '
          'commands=${service.commands} writes=${store.writes} bytes=${store.bytes}',
        );
        expect(
          store.bytes,
          lessThan(4096),
          reason: 'one compact draft transaction',
        );
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets(
    'explicit Litchcraft selection admits immediately and keeps the first board canvas',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _CraftNavigationBle()
        ..active = true
        ..artwork = LitchcraftCanvas.blank().paint(<int>[37], 15, 0x26a7df)
        ..capabilityGate = Completer<LitchDeviceCapabilities>();
      final craft = LitchcraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();

      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pump();
      await tester.tap(
        find.byKey(const ValueKey<String>('main-destination-craft')),
      );
      await tester.pump();
      expect(find.byType(LitchcraftScreen), findsOneWidget);

      service.capabilityGate!.complete(
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask: LitchDeviceCapabilities.litchcraftBit,
          explicitlyReported: true,
        ),
      );
      await tester.pumpAndSettle();

      expect(craft.status, LitchcraftStatus.live);
      expect(craft.canvas.colors, contains(0x26a7df));
      expect(
        service.commands.where((opcode) => opcode == LitchcraftProtocol.enter),
        hasLength(1),
        reason:
            'explicit selection preempts the passive probe with idempotent Enter',
      );
      expect(_activeNavigationIndex(tester), 1);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final configuration in [
    ('android-hidden', const Size(844, 390), EdgeInsets.zero),
    (
      'android-transient',
      const Size(844, 390),
      const EdgeInsets.fromLTRB(44, 24, 0, 16),
    ),
    (
      'iphone-left',
      const Size(932, 430),
      const EdgeInsets.fromLTRB(59, 0, 0, 21),
    ),
    (
      'iphone-right',
      const Size(932, 430),
      const EdgeInsets.fromLTRB(0, 0, 59, 21),
    ),
    (
      'duo-outer-left',
      const Size(678, 466),
      const EdgeInsets.fromLTRB(59, 0, 0, 21),
    ),
    (
      'duo-outer-right',
      const Size(678, 466),
      const EdgeInsets.fromLTRB(0, 0, 59, 21),
    ),
    (
      'duo-inner-left',
      const Size(890, 626),
      const EdgeInsets.fromLTRB(20, 24, 0, 20),
    ),
    (
      'duo-inner-right',
      const Size(890, 626),
      const EdgeInsets.fromLTRB(0, 24, 20, 20),
    ),
  ]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets('connection landscape hierarchy ${configuration.$1} $scale', (
        tester,
      ) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = configuration.$2;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues({});
        final service = _ConnectionGateBleService();
        final repository = _EmptyGameLibraryRepository(
          snapshot: GameLibrarySnapshot(
            games: List.generate(3, _recentArchive),
          ),
        );
        addTearDown(service.dispose);
        addTearDown(repository.close);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchDarkTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: TextScaler.linear(scale),
                padding: configuration.$3,
                viewPadding: configuration.$3,
              ),
              child: RepaintBoundary(
                key: const ValueKey('connection-landscape-review-root'),
                child: child!,
              ),
            ),
            home: ConnectionGateScreen(
              bleService: service,
              gameLibrary: repository,
            ),
          ),
        );
        await tester.pump();
        Future<void> capture(String state) async {
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 100));
          await tester.pump(const Duration(milliseconds: 100));
          await tester.pump(const Duration(milliseconds: 100));
          expect(tester.takeException(), isNull, reason: state);
          await captureHealthReview(
            tester,
            'connection-${configuration.$1}-$scale-$state',
            boundaryFinder: find.byKey(
              const ValueKey('connection-landscape-review-root'),
            ),
          );
        }

        await capture('empty');
        final boards = List.generate(
          4,
          (index) => LitchBoard(
            id: 'landscape-$index',
            name: 'Foxie Litch Living room tournament board ${index + 1}',
            rssi: -40,
          ),
        );
        service.emitBoards(boards.take(1).toList());
        await tester.pump();
        final card = find.byKey(const ValueKey('discovered-board-landscape-0'));
        final identify = find.descendant(
          of: card,
          matching: find.byType(OutlinedButton),
        );
        final connect = find.descendant(
          of: card,
          matching: find.byType(FilledButton),
        );
        final tablet = LitchWorkspaceGeometry.isTabletWindow(configuration.$2);
        final identifyRect = tester.getRect(identify);
        final connectRect = tester.getRect(connect);
        if (!tablet) {
          expect(identifyRect.left, connectRect.left);
          expect(connectRect.top, greaterThanOrEqualTo(identifyRect.bottom));
        } else if (scale == 1) {
          expect(identifyRect.top, connectRect.top);
          expect(connectRect.left, greaterThanOrEqualTo(identifyRect.right));
        } else {
          expect(identifyRect.left, connectRect.left);
          expect(identifyRect.top, greaterThanOrEqualTo(connectRect.bottom));
        }
        for (final button in [identify, connect]) {
          expect(button.hitTestable(), findsOneWidget);
          expect(tester.getSize(button).height, greaterThanOrEqualTo(48));
        }
        final name = find.text(boards.first.name);
        final nameRect = tester.getRect(name);
        final cardRect = tester.getRect(card);
        expect(nameRect.width, greaterThan(0));
        expect(nameRect.left, greaterThanOrEqualTo(cardRect.left));
        expect(nameRect.right, lessThanOrEqualTo(cardRect.right));
        expect(
          tester.renderObject<RenderParagraph>(name).didExceedMaxLines,
          isFalse,
        );
        final rail = find.byKey(
          const ValueKey('disconnected-play-landscape-action-rail'),
        );
        if (tablet) {
          final controls = tester.getRect(
            find.byKey(const ValueKey('bottom-navigation-controls')),
          );
          expect(controls.top, greaterThanOrEqualTo(cardRect.bottom));
          expect(controls.width, LitchWorkspaceGeometry.controlGroupWidth);
          expect(find.byKey(const ValueKey('discovery-recent-games')), findsOneWidget);
        } else {
          expect(tester.getRect(rail).left, greaterThanOrEqualTo(cardRect.right));
          expect(
            find.byKey(const ValueKey('disconnected-recent-games-scroll')),
            findsNothing,
          );
        }
        await capture('one');
        service.emitBoards(boards);
        await capture('many');
        await tester.tap(connect);
        await capture('connecting');
        service.completeNextConnect(false);
        await capture('error');
        expect(find.byKey(const ValueKey('chosen-board-phase')), findsOneWidget);
        await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
        await tester.pump();
        expect(tester.widget<FilledButton>(connect).onPressed, isNotNull);
        await tester.ensureVisible(connect);
        await tester.pumpAndSettle();
        expect(connect.hitTestable(), findsOneWidget);
        await tester.tap(connect);
        await capture('retry');
        expect(service.connectCalls.length, 2);
        service.completeNextConnect(false);
        await tester.pump();
        await tester.pumpWidget(const SizedBox());
      });
    }
  }
  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      'page and navigation drags stay put while explicit accessible controls work $platform',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues({});
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..simulatePhysicalMode(true);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await library.dismissWelcome();
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchDarkTheme().copyWith(platform: platform),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              craftController: craft,
              craftLibrary: library,
              initialPageIndex: 0,
              initialCraftModeResolved: true,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        final semantics = tester.ensureSemantics();
        Future<void> settle() async {
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 600));
        }

        await settle();
        double page() => tester
            .widget<PageView>(find.byKey(const ValueKey('main-pages')))
            .controller!
            .page!;
        for (final size in [const Size(390, 844), const Size(1000, 472.5)]) {
          tester.view.physicalSize = size;
          await settle();
          for (final destination in ['litchcraft', 'setup', 'play']) {
            await tester.tap(_navigationAction(tester, destination));
            await settle();
            final expected = page();
            final body = tester.getRect(
              find.byKey(const ValueKey('main-pages')),
            );
            final nav = tester.getRect(_navigationAction(tester, destination));
            for (final start in [
              Offset(body.center.dx, body.top + 4),
              nav.center,
            ]) {
              for (final direction in [-1, 1]) {
                final pointer = await tester.startGesture(start);
                for (var step = 0; step < 5; step++) {
                  await pointer.moveBy(Offset(direction * size.width * .12, 0));
                  await tester.pump(const Duration(milliseconds: 40));
                  expect(
                    page(),
                    expected,
                    reason: '$destination drag stays on page',
                  );
                }
                await pointer.up();
                await settle();
                expect(page(), expected);
              }
            }
          }
          for (final destination in ['litchcraft', 'setup', 'play']) {
            final label = destination == 'litchcraft'
                ? 'Litchcraft'
                : '${destination[0].toUpperCase()}${destination.substring(1)}';
            // NavigationDestination's public key wraps the Material semantics;
            // address the actual merged, labelled accessibility node.
            final accessibleControl = find
                .descendant(
                  of: find.ancestor(
                    of: _navigationAction(tester, destination),
                    matching: find.byWidgetPredicate(
                      (widget) =>
                          widget is NavigationBar ||
                          widget is NavigationRail ||
                          widget.key ==
                              const ValueKey('landscape-navigation-switcher'),
                    ),
                  ),
                  matching: find.bySemanticsLabel(RegExp('^$label(?:\\n|\\b)')),
                )
                .hitTestable()
                .first;
            final node = tester.getSemantics(accessibleControl);
            expect(
              node.getSemanticsData().hasAction(ui.SemanticsAction.tap),
              isTrue,
            );
            node.owner!.performAction(node.id, ui.SemanticsAction.tap);
            await settle();
            expect(
              page(),
              ['play', 'litchcraft', 'setup'].indexOf(destination),
            );
            expect(
              tester
                  .getSemantics(accessibleControl)
                  .getSemanticsData()
                  .flagsCollection
                  .isSelected,
              ui.Tristate.isTrue,
            );
          }
          expect(tester.takeException(), isNull);
        }
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        semantics.dispose();
      },
    );
  }
  for (final configuration in [
    (
      Alignment.centerLeft,
      const Size(1000, 472.5),
      2.0,
      const EdgeInsets.fromLTRB(20, 24, 34, 16),
    ),
    (
      Alignment.centerRight,
      const Size(1000, 472.5),
      2.0,
      const EdgeInsets.fromLTRB(20, 24, 34, 16),
    ),
    (
      Alignment.centerLeft,
      const Size(844, 390),
      2.0,
      const EdgeInsets.fromLTRB(44, 0, 0, 21),
    ),
    (
      Alignment.centerRight,
      const Size(844, 390),
      2.0,
      const EdgeInsets.fromLTRB(0, 0, 44, 21),
    ),
    (
      Alignment.centerLeft,
      const Size(932, 430),
      2.0,
      const EdgeInsets.fromLTRB(59, 0, 0, 21),
    ),
    (
      Alignment.centerRight,
      const Size(932, 430),
      2.0,
      const EdgeInsets.fromLTRB(0, 0, 59, 21),
    ),
    (
      Alignment.center,
      const Size(390, 844),
      1.0,
      const EdgeInsets.fromLTRB(0, 24, 0, 24),
    ),
    (
      Alignment.center,
      const Size(384, 832),
      1.0,
      const EdgeInsets.fromLTRB(0, 24, 0, 24),
    ),
    (
      Alignment.center,
      const Size(390, 844),
      2.0,
      const EdgeInsets.fromLTRB(20, 24, 34, 16),
    ),
    (Alignment.center, const Size(320, 640), 2.0, EdgeInsets.zero),
    (
      Alignment.center,
      const Size(393, 852),
      1.0,
      const EdgeInsets.fromLTRB(0, 59, 0, 34),
    ),
    (
      Alignment.center,
      const Size(393, 852),
      2.0,
      const EdgeInsets.fromLTRB(0, 59, 0, 34),
    ),
    (
      Alignment.center,
      const Size(320, 640),
      2.0,
      const EdgeInsets.fromLTRB(0, 24, 0, 16),
    ),
  ]) {
    final (side, size, scale, insets) = configuration;
    testWidgets(
      'production looping palette rail preserves last color $configuration',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final portrait = side == Alignment.center;
        final consoleEnabled = !portrait && size.width == 1000;
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues({
          'enable_app_console': consoleEnabled,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..simulatePhysicalMode(true);
        await craft.setBrush(color: 0xff0000);
        final colorBeforePaletteSelection = craft.color;
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await library.dismissWelcome();
        craft.paintSquares([9, 18, 27]);
        craft.endStroke();
        library.applyEffect(
          square: 9,
          mask: 15,
          effect: LitchcraftEffectStyle.breathe,
        );
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchDarkTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: TextScaler.linear(scale),
                padding: insets,
                viewPadding: insets,
              ),
              child: child!,
            ),
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              craftController: craft,
              craftLibrary: library,
              initialPageIndex: 0,
              initialCraftModeResolved: true,
              landscapeRibbonSide: side,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final rail = find.byKey(const ValueKey('craft-color-wheel-rail'));
        expect(rail, findsOneWidget);
        if (portrait) {
          final bar = tester.widget<NavigationBar>(find.byType(NavigationBar));
          final destination = bar.destinations[1] as NavigationDestination;
          expect(destination.label, 'Litchcraft');
          expect(destination.icon, isA<LitchcraftIdentityGlyph>());
          expect(
            (bar.destinations[0] as NavigationDestination).icon,
            isA<LitchPlayGlyph>(),
          );
          expect(
            bar.labelBehavior,
            NavigationDestinationLabelBehavior.alwaysHide,
          );
          expect(tester.getSize(find.byType(NavigationBar)).height, 48);
          expect(
            tester
                .getRect(find.byKey(const ValueKey('main-navigation-surface')))
                .bottom,
            size.height,
          );
          expect(
            tester.getRect(find.byType(NavigationBar)).bottom,
            size.height - insets.bottom,
          );
          final headerImages = tester.widgetList<Image>(
            find.descendant(
              of: find.byType(AppBar),
              matching: find.byType(Image),
            ),
          );
          expect(
            headerImages.where(
              (image) =>
                  (image.image as AssetImage).assetName.contains('litchcraft'),
            ),
            hasLength(1),
          );
          expect(headerImages, isNotEmpty);
          await tester.runAsync(() async {
            for (final image in headerImages) {
              await precacheImage(
                image.image,
                tester.element(find.byType(AppBar)),
              );
            }
          });
          await tester.pump();
        } else {
          final fill = find.byKey(const ValueKey('craft-quick-fill'));
          final surface = find.descendant(
            of: fill,
            matching: find.byType(AnimatedContainer),
          );
          expect(surface, findsOneWidget);
          final resting =
              tester.widget<AnimatedContainer>(surface).decoration!
                  as BoxDecoration;
          expect(resting.shape, BoxShape.circle);
          expect(resting.border, isNull);
          final pointer = await tester.startGesture(tester.getCenter(fill));
          await tester.pump(const Duration(milliseconds: 100));
          final pressed =
              tester.widget<AnimatedContainer>(surface).decoration!
                  as BoxDecoration;
          expect(pressed.color, isNot(resting.color));
          await pointer.cancel();
          await tester.pumpAndSettle();
        }
        final railRect = tester.getRect(rail);
        if (!portrait) {
          expect(
            find.byKey(const ValueKey('landscape-more-action')),
            findsNothing,
          );
          for (final destination in [
            'play',
            'craft',
            'setup',
            if (consoleEnabled) 'console',
          ]) {
            final action = find.byKey(
              ValueKey('landscape-$destination-action'),
            );
            expect(action.hitTestable(), findsOneWidget);
            expect(
              tester.getSize(action).shortestSide,
              greaterThanOrEqualTo(48),
            );
          }
          expect(
            find.byKey(const ValueKey('landscape-console-action')),
            consoleEnabled ? findsOneWidget : findsNothing,
          );
        }
        final board = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        expect(railRect.overlaps(board), isFalse);
        expect(railRect.overlaps(inspector), isFalse);
        if (portrait) {
          final shell = tester.getSize(
            find.byKey(const ValueKey('craft-portrait-shell')),
          );
          final compactAccessibility =
              size.width <= 360 && size.height <= 640 && scale >= 1.8;
          final inspectorReserve = compactAccessibility
              ? 252.0
              : scale >= 1.6
              ? 218.0
              : 205.0;
          final expectedBoard = math.min(
            shell.width - LitchWorkspaceGeometry.contentEdgeInset * 2,
            shell.height -
                inspectorReserve -
                64 -
                3 * LitchWorkspaceGeometry.outerGap,
          );
          expect(board.width, expectedBoard);
          expect(
            inspector.width,
            shell.width - LitchWorkspaceGeometry.contentEdgeInset * 2,
          );
          expect(railRect.right, inspector.right);
          final field = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-color-field')),
          );
          final hex = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-hex')),
          );
          final hue = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-hue')),
          );
          expect(hex.left, field.left);
          expect(hue.right, field.right);
          expect(hex.right + 8, hue.left);
          expect(hex.height, greaterThanOrEqualTo(48));
          expect(hue.height, greaterThanOrEqualTo(48));
          expect(field.height, greaterThanOrEqualTo(48));
          expect(railRect.top, greaterThanOrEqualTo(board.bottom));
          expect(railRect.bottom, lessThanOrEqualTo(inspector.top));
        } else {
          expect(
            board.center.dy,
            moreOrLessEquals(
              (insets.top + size.height - insets.bottom) / 2,
              epsilon: .01,
            ),
          );
          expect(board.top, greaterThanOrEqualTo(insets.top));
          expect(board.bottom, lessThanOrEqualTo(size.height - insets.bottom));
          final field = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-color-field')),
          );
          final color = tester.getRect(
            find.byKey(const ValueKey('craft-color-panel')),
          );
          expect(field.left, color.left);
          expect(field.right, color.right);
          expect(
            find.byKey(const ValueKey('craft-inspector-divider')),
            findsNothing,
          );
          expect(
            railRect.center.dx,
            inInclusiveRange(
              side == Alignment.centerLeft ? inspector.right : board.right,
              side == Alignment.centerLeft ? board.left : inspector.left,
            ),
          );
        }
        final swatch = find.byKey(const ValueKey('craft-wheel-color-1'));
        for (
          var attempt = 0;
          attempt < 3 && swatch.hitTestable().evaluate().isEmpty;
          attempt++
        ) {
          await tester.drag(rail, const Offset(-48, 0));
          await tester.pumpAndSettle();
        }
        expect(swatch.hitTestable(), findsOneWidget);
        expect(tester.getSize(swatch).shortestSide, greaterThanOrEqualTo(48));
        await tester.tap(swatch);
        await tester.pump();
        expect(craft.color, litchcraftPaletteRailColors[1]);
        expect(craft.previousColor, colorBeforePaletteSelection);
        final footer = find.byKey(const ValueKey('craft-history-footer'));
        if (!portrait) {
          expect(tester.getSize(footer).height, 48);
          final field = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-color-field')),
          );
          expect(field.height, greaterThanOrEqualTo(48));
          for (final key in ['craft-inspector-hue', 'craft-inspector-hex']) {
            final rect = tester.getRect(find.byKey(ValueKey(key)));
            expect(rect.left, field.left);
            expect(rect.right, field.right);
          }
        }
        final orderedActions = [
          'craft-undo',
          'craft-redo',
          'craft-rotate-artwork',
        ];
        for (final key in orderedActions) {
          expect(
            find.descendant(of: footer, matching: find.byKey(ValueKey(key))),
            findsOneWidget,
          );
          expect(
            tester.getSize(find.byKey(ValueKey(key))).shortestSide,
            greaterThanOrEqualTo(48),
          );
        }
        final artworkMenu = find.byKey(const ValueKey('craft-artwork-menu'));
        expect(artworkMenu.hitTestable(), findsOneWidget);
        expect(
          tester.getSize(artworkMenu).shortestSide,
          greaterThanOrEqualTo(48),
        );
        expect(
          find.descendant(of: footer, matching: artworkMenu),
          findsNothing,
        );
        final originalBoard = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        await tester.drag(rail, const Offset(-140, 0));
        await tester.pumpAndSettle();
        expect(rail, findsOneWidget);
        expect(
          tester.getRect(find.byKey(const ValueKey('craft-ribbon-board'))),
          originalBoard,
        );
        expect(craft.color, litchcraftPaletteRailColors[1]);
        expect(craft.previousColor, colorBeforePaletteSelection);
        expect(tester.takeException(), isNull);
        if (portrait) {
          for (final key in [
            'craft-inspector-hue',
            'craft-inspector-color-field',
          ]) {
            final control = find.byKey(ValueKey(key));
            final rect = tester.getRect(control);
            final before = craft.color;
            final previousBeforePreview = craft.previousColor;
            final pointer = await tester.startGesture(
              Offset(rect.left + rect.width * .25, rect.center.dy),
            );
            for (final fraction in [.35, .5, .7]) {
              await pointer.moveTo(
                Offset(rect.left + rect.width * fraction, rect.center.dy),
              );
              await tester.pump(const Duration(milliseconds: 40));
              expect(
                craft.previousColor,
                previousBeforePreview,
                reason: '$key held preview preserves last color',
              );
            }
            await pointer.up();
            await tester.pump();
            expect(craft.color, isNot(before));
            expect(craft.previousColor, before);
          }
          final previousBeforeSwap = craft.previousColor;
          await tester.tap(
            find.byKey(const ValueKey('craft-quick-previous-color')),
          );
          await tester.pump();
          expect(craft.color, previousBeforeSwap);
          final previousButton = find.byKey(
            const ValueKey('craft-quick-previous-color'),
          );
          expect(previousButton.hitTestable(), findsOneWidget);
          expect(tester.getSize(previousButton), const Size(48, 48));
        }
        await captureHealthReview(
          tester,
          portrait
              ? 'palette-rail-portrait-${size.width.toInt()}-$scale'
              : side == Alignment.centerLeft
              ? 'palette-rail-left-${size.width.toInt()}'
              : 'palette-rail-right-${size.width.toInt()}',
        );
        expect(
          find.byKey(const ValueKey('craft-effect-marker-9')),
          findsOneWidget,
        );
        final canvasRect = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        for (final tab in [
          'craft-leds',
          'craft-motion',
          'craft-animate',
          'craft-artwork-menu',
        ]) {
          await tester.tap(find.byKey(ValueKey(tab)));
          // Effect previews intentionally animate continuously; wait only for
          // the finite tab transition, never for the illustrative ticker.
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 250));
          expect(tester.takeException(), isNull, reason: '$configuration $tab');
          expect(
            tester.getRect(find.byKey(const ValueKey('craft-ribbon-board'))),
            canvasRect,
          );
          if (tab == 'craft-motion') {
            if (find
                .byKey(const ValueKey('craft-effect-selector'))
                .evaluate()
                .isNotEmpty) {
              await tester.tap(
                find.byKey(const ValueKey('craft-effect-selector')),
              );
              await tester.pump();
              await tester.pump(const Duration(milliseconds: 300));
            }
            for (final effect in LitchcraftEffectStyle.values) {
              final button = find.byKey(
                ValueKey('craft-effect-${effect.name}'),
              );
              await tester.ensureVisible(button);
              await tester.pump();
              expect(button.hitTestable(), findsOneWidget);
              expect(
                tester.getSize(button).shortestSide,
                greaterThanOrEqualTo(48),
              );
            }
            final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
            await tester.ensureVisible(breathe);
            await tester.pump();
            await tester.tap(breathe);
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 300));
            final durationAction = find.byKey(
              const ValueKey('craft-effect-duration-context'),
            );
            if (durationAction.evaluate().isNotEmpty) {
              await tester.tap(durationAction);
              await tester.pump();
              await tester.pump(const Duration(milliseconds: 250));
              expect(find.byType(AlertDialog), findsOneWidget);
              final exact = find.byKey(
                const ValueKey('craft-exact-duration-field'),
              );
              await tester.enterText(exact, '2500');
              await tester.pump();
              await tester.tap(
                find.byKey(const ValueKey('craft-set-exact-duration')),
              );
              await tester.pump();
              await tester.pump(const Duration(milliseconds: 250));
              expect(find.byType(AlertDialog), findsNothing);
            }
          }
          if (tab == 'craft-animate') {
            while (library.animationDocument.frames.length < 6) {
              library.addFrame();
            }
            for (var index = 1; index < 6; index++) {
              library.selectFrame(index);
              library.session.replaceCanvas(
                LitchcraftCanvas.blank().paint(
                  [index, index + 8, index + 16],
                  15,
                  const [
                    0xff782d,
                    0x39c78c,
                    0x337cff,
                    0x9252e8,
                    0xf54275,
                    0xffd343,
                  ][index],
                ),
              );
            }
            library.selectFrame(0);
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 250));
            if (!portrait) {
              await _expectCraftFramesReachable(tester, library, 6);
              final strip = tester.getRect(
                find.byKey(const ValueKey('craft-frame-filmstrip')),
              );
              final play = tester.getRect(
                find.byKey(const ValueKey('craft-preview')),
              );
              final add = tester.getRect(
                find.byKey(const ValueKey('craft-add-frame')),
              );
              final duration = tester.getRect(
                find.byKey(const ValueKey('craft-frame-duration')),
              );
              expect(strip.top, greaterThanOrEqualTo(play.bottom));
              expect(add.center.dy, strip.center.dy);
              expect(duration.top, greaterThanOrEqualTo(strip.bottom));
              expect(
                find.descendant(
                  of: find.byKey(const ValueKey('craft-frame-filmstrip')),
                  matching: find.byKey(const ValueKey('craft-add-frame')),
                ),
                findsOneWidget,
              );
            } else {
              final strip = tester.getRect(
                find.byKey(const ValueKey('craft-frame-filmstrip')),
              );
              final play = tester.getRect(
                find.byKey(const ValueKey('craft-preview')),
              );
              final add = tester.getRect(
                find.byKey(const ValueKey('craft-add-frame')),
              );
              expect(strip.center.dy, play.center.dy);
              expect(add.center.dy, strip.center.dy);
              expect(
                find.byKey(const ValueKey('craft-frame-previous')),
                findsNothing,
              );
              expect(
                find.byKey(const ValueKey('craft-frame-next')),
                findsNothing,
              );
            }
            expect(
              find.byKey(const ValueKey('craft-selected-context')),
              findsNothing,
            );
            expect(
              find.byKey(const ValueKey('craft-frame-details')),
              findsNothing,
            );
            expect(
              find.byKey(const ValueKey('craft-frame-number-0')),
              findsOneWidget,
            );
            expect(
              find.descendant(
                of: find.byKey(const ValueKey('craft-add-frame')),
                matching: find.byType(Text),
              ),
              findsNothing,
            );
            expect(
              tester
                  .getSize(find.byKey(const ValueKey('craft-preview')))
                  .shortestSide,
              greaterThanOrEqualTo(48),
            );
          }
          await captureHealthReview(
            tester,
            'palette-${portrait
                ? "portrait"
                : side.x < 0
                ? "left"
                : "right"}-${size.width.toInt()}-$scale-$tab',
          );
        }
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 250));
        if (!portrait) {
          final filmstrip = find.byKey(const ValueKey('craft-frame-filmstrip'));
          final selectedBeforeBrowse = library.selectedFrameIndex;
          final framesBeforeBrowse = library.animationDocument.frames;
          final filmDrag = await tester.startGesture(
            tester.getCenter(filmstrip),
          );
          for (var step = 0; step < 3; step++) {
            await filmDrag.moveBy(const Offset(-35, 0));
            await tester.pump(const Duration(milliseconds: 70));
            expect(library.animationDocument.frames, framesBeforeBrowse);
            expect(library.selectedFrameIndex, selectedBeforeBrowse);
          }
          await filmDrag.up();
          for (var frame = 0; frame < 20; frame++) {
            await tester.pump(const Duration(milliseconds: 50));
          }
          final visibleFrame = List.generate(6, (index) => index).firstWhere(
            (index) =>
                index != library.selectedFrameIndex &&
                find
                    .byKey(ValueKey('craft-select-frame-$index'))
                    .hitTestable()
                    .evaluate()
                    .isNotEmpty,
          );
          final visibleFrameTarget = find.byKey(
            ValueKey('craft-select-frame-$visibleFrame'),
          );
          await tester.longPress(visibleFrameTarget);
          await tester.pumpAndSettle();
          expect(
            find.text('Delete frame ${visibleFrame + 1}?'),
            findsOneWidget,
          );
          await tester.tap(find.text('Cancel'));
          await tester.pumpAndSettle();
          await tester.tap(visibleFrameTarget);
          await tester.pump(const Duration(milliseconds: 250));
          expect(library.selectedFrameIndex, visibleFrame);
          expect(
            find.byKey(const ValueKey('craft-frame-details')),
            findsNothing,
          );
          final editorState = tester.state(find.byType(LitchcraftScreen));
          for (final destination in [
            'play',
            'setup',
            if (consoleEnabled) 'console',
          ]) {
            await tester.tap(
              find.byKey(ValueKey('landscape-$destination-action')),
            );
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 350));
            await tester.tap(
              find.byKey(const ValueKey('landscape-craft-action')),
            );
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 350));
            expect(
              identical(
                tester.state(find.byType(LitchcraftScreen)),
                editorState,
              ),
              isTrue,
            );
          }
        } else {
          final nextFrame = find.byKey(const ValueKey('craft-select-frame-1'));
          expect(nextFrame.hitTestable(), findsOneWidget);
          await tester.tap(nextFrame);
          await tester.pump(const Duration(milliseconds: 250));
          expect(library.selectedFrameIndex, 1);
        }
        await tester.pumpWidget(const SizedBox());
        await library.flush();
      },
    );
  }
  for (final locale in ['en', 'de', 'fr']) {
    for (final size in [const Size(640, 360), const Size(667, 375)]) {
      testWidgets(
        'compact localized toolbar $locale ${size.width} keeps icon actions usable at 200 percent',
        (tester) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          SharedPreferences.setMockInitialValues({});
          final service = _ConnectionGateBleService();
          service.emitConnection('board-1', DeviceConnectionState.connected);
          final craft = _NavigationCraftController(service)
            ..simulatePhysicalMode(true);
          final library = LitchcraftLibrary(craft);
          await library.ready;
          await library.dismissWelcome();
          addTearDown(() async {
            library.dispose();
            craft.dispose();
            await service.dispose();
          });
          await tester.pumpWidget(
            MaterialApp(
              locale: Locale(locale),
              theme: buildLitchDarkTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: const TextScaler.linear(2),
                  padding: const EdgeInsets.fromLTRB(20, 0, 12, 16),
                  viewPadding: const EdgeInsets.fromLTRB(20, 0, 12, 16),
                ),
                child: child!,
              ),
              home: LitchMainScaffold(
                bleService: service,
                boardId: 'board-1',
                boards: const [],
                onDisconnect: () async {},
                craftController: craft,
                craftLibrary: library,
                initialPageIndex: 0,
                initialCraftModeResolved: true,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          final toolbar = find.byKey(const ValueKey('craft-toolbar'));
          for (final key in [
            'craft-palette',
            'craft-leds',
            'craft-motion',
            'craft-animate',
            'craft-rotate-artwork',
          ]) {
            final action = find.byKey(ValueKey(key));
            expect(action.hitTestable(), findsOneWidget);
            expect(
              tester.getSize(action).shortestSide,
              greaterThanOrEqualTo(48),
            );
          }
          for (final paragraph
              in find
                  .descendant(of: toolbar, matching: find.byType(RichText))
                  .evaluate()) {
            expect(
              (paragraph.renderObject! as RenderParagraph).didExceedMaxLines,
              isFalse,
            );
          }
          final navigation = find.byKey(
            const ValueKey('craft-landscape-navigation'),
          );
          expect(navigation, findsOneWidget);
          expect(tester.getSize(navigation).height, greaterThanOrEqualTo(48));
          expect(
            find.byKey(const ValueKey('landscape-more-action')),
            findsNothing,
          );
          for (final key in [
            'landscape-play-action',
            'landscape-craft-action',
            'landscape-setup-action',
          ]) {
            final action = find.byKey(ValueKey(key));
            expect(action.hitTestable(), findsOneWidget);
            expect(
              tester.getSize(action).shortestSide,
              greaterThanOrEqualTo(48),
            );
          }
          expect(tester.takeException(), isNull);
          await captureHealthReview(
            tester,
            'compact-toolbar-$locale-${size.width.toInt()}',
          );
          await tester.pumpWidget(const SizedBox());
          await library.flush();
        },
      );
    }
  }
  testWidgets('188px production Color inspector preserves every 48px control', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    // A compact landscape phone gives the color panel exactly 188 px.
    tester.view.physicalSize = const Size(744, 460);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues({});
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await library.dismissWelcome();
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchDarkTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const [],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    final panel = find.byKey(const ValueKey('craft-color-panel'));
    expect(tester.getSize(panel).width, 188);
    for (final key in [
      'craft-inspector-hex',
      'craft-inspector-hue',
      'craft-inspector-color-field',
    ]) {
      final target = find.byKey(ValueKey(key));
      expect(target.hitTestable(), findsOneWidget);
      expect(tester.getSize(target).height, greaterThanOrEqualTo(48));
      expect(
        tester.getRect(panel).contains(tester.getRect(target).topLeft),
        isTrue,
      );
      expect(
        tester
            .getRect(panel)
            .contains(
              tester.getRect(target).bottomRight - const Offset(.1, .1),
            ),
        isTrue,
      );
    }
    for (final key in [
      'craft-quick-brush',
      'craft-quick-previous-color',
      'craft-quick-fill',
    ]) {
      final target = find.byKey(ValueKey(key));
      expect(target.hitTestable(), findsOneWidget);
      expect(tester.getSize(target).shortestSide, greaterThanOrEqualTo(48));
      expect(
        tester.getRect(panel).overlaps(tester.getRect(target)),
        isFalse,
        reason: '$key remains in the fixed quick-action rail.',
      );
    }
    final recent = find.byWidgetPredicate(
      (widget) =>
          widget.key is ValueKey<String> &&
          (widget.key! as ValueKey<String>).value.startsWith(
            'craft-inspector-recent-',
          ),
    );
    expect(recent, findsNWidgets(4));
    for (final element in recent.evaluate()) {
      final target = find.byWidget(element.widget);
      expect(target.hitTestable(), findsOneWidget);
      expect(tester.getSize(target).shortestSide, greaterThanOrEqualTo(48));
    }
    await captureHealthReview(tester, 'production-color-inspector-188px');
    await tester.pumpWidget(const SizedBox());
    await library.flush();
  });
  testWidgets(
    'production acrylic preview shows side light outward and top-bottom light inward',
    (tester) async {
      const glowSide = {
        1: Offset(4, 12),
        2: Offset(12, 10),
        4: Offset(20, 12),
        8: Offset(12, 14),
      };
      const darkSide = {
        1: Offset(10, 12),
        2: Offset(12, 3),
        4: Offset(14, 12),
        8: Offset(12, 21),
      };
      for (final flipped in [false, true]) {
        for (final mask in glowSide.keys) {
          await tester.pumpWidget(
            MaterialApp(
              home: Center(
                child: RepaintBoundary(
                  key: const ValueKey('physical-emitter-face'),
                  child: SizedBox.square(
                    dimension: 96,
                    child: CustomPaint(
                      painter: LitchcraftTargetPainter(
                        mask: mask,
                        topology: LitchcraftTopology.v3,
                        flipped: flipped,
                        color: 0xff7836,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          final boundary = tester.renderObject<RenderRepaintBoundary>(
            find.byKey(const ValueKey('physical-emitter-face')),
          );
          await tester.runAsync(() async {
            final image = await boundary.toImage();
            final bytes = (await image.toByteData(
              format: ui.ImageByteFormat.rawRgba,
            ))!.buffer.asUint8List();
            int orangeEnergy(Offset point) {
              final offset =
                  ((point.dy * 4).round() * image.width +
                      (point.dx * 4).round()) *
                  4;
              return bytes[offset] - bytes[offset + 2];
            }

            final displayed = litchcraftDisplayTargetMask(
              mask,
              flipped: flipped,
            );
            expect(
              orangeEnergy(glowSide[displayed]!),
              greaterThan(orangeEnergy(darkSide[displayed]!) + 12),
              reason:
                  'Side LEDs must glow outward; top and bottom LEDs must glow inward.',
            );
            image.dispose();
          });
        }
      }
    },
  );
  testWidgets('Frames glyph separates film perforations from its border', (
    tester,
  ) async {
    await tester.pumpWidget(
      const MaterialApp(
        home: Center(child: LitchcraftAnimationGlyph(size: 100)),
      ),
    );
    final painter = tester
        .widget<CustomPaint>(
          find.descendant(
            of: find.byType(LitchcraftAnimationGlyph),
            matching: find.byType(CustomPaint),
          ),
        )
        .painter!;
    final recorder = ui.PictureRecorder();
    painter.paint(Canvas(recorder), const Size(100, 100));
    final picture = recorder.endRecording();
    final image = (await tester.runAsync(() => picture.toImage(100, 100)))!;
    final bytes = await tester.runAsync(
      () => image.toByteData(format: ui.ImageByteFormat.rawRgba),
    );
    expect(
      bytes!.getUint8((19 * 100 + 24) * 4 + 3),
      0,
      reason:
          'Separate small film perforations must not merge into a heavy comb-shaped border.',
    );
    image.dispose();
    picture.dispose();
  });
  testWidgets('board-not-empty guidance is one temporary canvas toast', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(2000, 945);
    tester.view.devicePixelRatio = 2;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await library.dismissWelcome();
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const [],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          landscapeRibbonSide: Alignment.centerRight,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final inspector = tester.getRect(
      find.byKey(const ValueKey('craft-persistent-inspector')),
    );

    craft.showStatus(LitchcraftStatus.boardNotEmpty);
    await tester.pump();
    await tester.pump();
    final strings = AppLocalizations.of(
      tester.element(find.byType(LitchcraftScreen)),
    );
    final toast = find.byKey(const ValueKey('craft-canvas-toast-position'));
    expect(find.text(strings.craftBoardNotEmpty), findsOneWidget);
    expect(toast, findsOneWidget);
    final toastRect = tester.getRect(toast);
    expect(canvas.overlaps(toastRect), isTrue);
    expect(inspector.overlaps(toastRect), isFalse);
    expect(toastRect.top, closeTo(canvas.top + 12, 1));
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('craft-persistent-inspector')),
        matching: find.text(strings.craftBoardNotEmpty),
      ),
      findsNothing,
    );

    await tester.pump(const Duration(seconds: 4));
    expect(find.text(strings.craftBoardNotEmpty), findsNothing);
    expect(toast, findsNothing);
  });

  for (final scenario in <(String, Size, EdgeInsets, Alignment)>[
    (
      'android-left',
      const Size(1000, 472.5),
      const EdgeInsets.fromLTRB(8, 24, 34, 16),
      Alignment.centerLeft,
    ),
    (
      'android-right',
      const Size(1000, 472.5),
      const EdgeInsets.fromLTRB(34, 24, 8, 16),
      Alignment.centerRight,
    ),
    (
      'portrait',
      const Size(390, 844),
      const EdgeInsets.fromLTRB(0, 24, 0, 16),
      Alignment.centerRight,
    ),
    (
      'compact-portrait',
      const Size(320, 640),
      const EdgeInsets.fromLTRB(0, 24, 0, 16),
      Alignment.centerRight,
    ),
  ]) {
    testWidgets('physical Litchcraft fixed inspector ${scenario.$1}', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = scenario.$2 * 2;
      tester.view.devicePixelRatio = 2;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues({'enable_app_console': true});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(
        service,
        initialCanvas: LitchcraftCanvas.blank().paint(
          [0, 1, 2, 8, 9, 16, 24, 32, 40, 48, 56],
          15,
          0xff782d,
        ),
      );
      craft.simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await library.dismissWelcome();
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchDarkTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              padding: scenario.$3,
              viewPadding: scenario.$3,
              textScaler: const TextScaler.linear(2),
              disableAnimations: true,
            ),
            child: RepaintBoundary(
              key: const ValueKey('physical-craft-root'),
              child: child!,
            ),
          ),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            landscapeRibbonSide: scenario.$4,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final canvasBeforeStatus = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final inspectorBeforeStatus = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      craft.showStatus(LitchcraftStatus.boardNotEmpty);
      await tester.pump();
      await tester.pump();
      final initialStrings = AppLocalizations.of(
        tester.element(find.byType(LitchcraftScreen)),
      );
      final teachingHint = find.text(initialStrings.craftSeparateCanvasHint);
      expect(teachingHint, findsNothing);
      final boardNotEmpty = find.text(initialStrings.craftBoardNotEmpty);
      final transientToast = find.byKey(
        const ValueKey('craft-canvas-toast-position'),
      );
      expect(boardNotEmpty, findsOneWidget);
      expect(transientToast, findsOneWidget);
      final toastRect = tester.getRect(transientToast);
      expect(canvasBeforeStatus.overlaps(toastRect), isTrue);
      expect(inspectorBeforeStatus.overlaps(toastRect), isFalse);
      expect(toastRect.top, closeTo(canvasBeforeStatus.top + 12, 1));
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-persistent-inspector')),
          matching: boardNotEmpty,
        ),
        findsNothing,
        reason: 'Board-clear guidance is temporary canvas feedback.',
      );
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        canvasBeforeStatus,
        reason: 'Connection/status changes cannot resize the paint surface.',
      );
      expect(
        tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        ),
        inspectorBeforeStatus,
      );
      await tester.pump(const Duration(seconds: 8));
      expect(boardNotEmpty, findsNothing);
      expect(transientToast, findsNothing);
      craft.showStatus(LitchcraftStatus.boardNotEmpty);
      await tester.pump();
      expect(
        teachingHint,
        findsNothing,
        reason:
            'The removed teaching toast cannot be rearmed by reconnect/status notifications.',
      );
      final editor = find.byType(LitchcraftScreen);
      final board = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final inspector = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      final toolbar = find.byKey(const ValueKey('craft-toolbar'));
      if (scenario.$2.width < scenario.$2.height) {
        expect(
          find.byType(AppBar),
          findsOneWidget,
          reason:
              'Litchcraft uses the same portrait app header/status pattern as Play and Setup.',
        );
        final brand = find.descendant(
          of: find.byType(AppBar),
          matching: find.byType(LitchAboutBrandButton),
        );
        expect(brand, findsOneWidget);
        expect(tester.widget<LitchAboutBrandButton>(brand).litchcraft, isTrue);
        expect(
          find
              .descendant(
                of: brand,
                matching: find.byKey(const ValueKey('open-about')),
              )
              .hitTestable(),
          findsOneWidget,
        );
        for (final image in tester.widgetList<Image>(
          find.descendant(of: brand, matching: find.byType(Image)),
        )) {
          await tester.runAsync(
            () => precacheImage(image.image, tester.element(brand)),
          );
        }
        await tester.pump();
        expect(
          find.descendant(
            of: brand,
            matching: find.byWidgetPredicate(
              (widget) => widget is RawImage && widget.image != null,
            ),
          ),
          findsNWidgets(2),
          reason:
              'The actual wordmark and fox must be decoded, not an empty AppBar.',
        );
      }
      final inspectorStatus = find.descendant(
        of: find.byKey(const ValueKey('craft-persistent-inspector')),
        matching: find.byKey(const ValueKey('connected-board-status-action')),
      );
      if (scenario.$2.width > scenario.$2.height) {
        expect(inspectorStatus, findsOneWidget);
        expect(
          tester.getSize(inspectorStatus).shortestSide,
          greaterThanOrEqualTo(48),
        );
      } else {
        expect(inspectorStatus, findsNothing);
        expect(
          find.byKey(const ValueKey('connected-board-status-action')),
          findsOneWidget,
        );
      }
      expect(find.byKey(const ValueKey('craft-painting-status')), findsNothing);
      final painted = List<int>.of(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors,
      );
      final rotate = find.byKey(const ValueKey('craft-rotate-artwork'));
      expect(
        find.byKey(const ValueKey('craft-rotate-two-finger-badge')),
        findsNothing,
      );
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-history-footer')),
          matching: rotate,
        ),
        findsOneWidget,
      );
      expect(tester.getSize(rotate).shortestSide, greaterThanOrEqualTo(48));
      expect(
        find.descendant(of: rotate, matching: find.byType(Text)),
        findsNothing,
        reason: 'Litchcraft quick actions are intentionally icon-only.',
      );
      expect(board.overlaps(inspector), isFalse);
      if (scenario.$2.width > scenario.$2.height) {
        expect(
          board.height,
          closeTo(
            LitchWorkspaceGeometry(
              Size(
                scenario.$2.width - scenario.$3.horizontal,
                scenario.$2.height - scenario.$3.vertical,
              ),
              safeInsets: scenario.$3,
            ).landscapeCanvasSide,
            .1,
          ),
        );
        expect(inspector.height, closeTo(board.height, .1));
      }
      for (final tab in [
        'craft-palette',
        'craft-leds',
        'craft-motion',
        'craft-animate',
      ]) {
        await tester.tap(find.byKey(ValueKey(tab)));
        await tester.pumpAndSettle();
        expect(
          find.byWidgetPredicate(
            (widget) =>
                widget is ModalBarrier &&
                (widget.dismissible || (widget.color?.a ?? 0) > 0),
          ),
          findsNothing,
          reason:
              'Changing an inspector tab must not open a modal or dim the canvas.',
        );
        expect(
          tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas
              .colors,
          painted,
          reason:
              'All 64 squares and their four emitter colors remain identical when changing tabs.',
        );
        for (final paragraph
            in find
                .descendant(of: toolbar, matching: find.byType(RichText))
                .evaluate()) {
          expect(
            (paragraph.renderObject! as RenderParagraph).didExceedMaxLines,
            isFalse,
            reason:
                'Every toolbar caption including Rotate must be fully visible.',
          );
        }
        expect(
          find.byKey(const ValueKey('craft-short-portrait-scroll')),
          findsNothing,
          reason:
              'Landscape tools fit without the short-portrait page fallback; the palette wheel remains intentionally scrollable.',
        );
        final strings = AppLocalizations.of(tester.element(editor));
        expect(find.text('FX'), findsNothing);
        if (tab == 'craft-motion') {
          final selector = find.byKey(const ValueKey('craft-effect-selector'));
          if (selector.evaluate().isNotEmpty) {
            expect(selector.hitTestable(), findsOneWidget);
            expect(tester.getSize(selector).height, greaterThanOrEqualTo(48));
            await tester.tap(selector);
            await tester.pumpAndSettle();
            for (final effect in LitchcraftEffectStyle.values) {
              final choice = find.byKey(
                ValueKey('craft-effect-${effect.name}'),
              );
              await tester.ensureVisible(choice);
              await tester.pumpAndSettle();
              expect(choice.hitTestable(), findsOneWidget);
              expect(tester.getSize(choice).height, greaterThanOrEqualTo(48));
              expect(
                tester.widget<MenuItemButton>(choice).onPressed,
                isNotNull,
              );
            }
            final off = find.byKey(const ValueKey('craft-effect-off'));
            await tester.ensureVisible(off);
            await tester.pumpAndSettle();
            await tester.tap(off);
            await tester.pumpAndSettle();
            expect(
              find.byKey(const ValueKey('craft-effect-off')),
              findsNothing,
            );
            expect(
              library.animationDocument.frames.single.emitters.first.effect,
              LitchcraftEffectStyle.off,
            );
          } else {
            for (final effect in LitchcraftEffectStyle.values) {
              final button = find.byKey(
                ValueKey('craft-effect-${effect.name}'),
              );
              expect(button.hitTestable(), findsOneWidget);
              expect(
                tester.getSize(button).shortestSide,
                greaterThanOrEqualTo(48),
              );
              expect(
                find.descendant(of: button, matching: find.byType(Text)),
                findsNothing,
                reason:
                    'Effects use a fixed icon grid, not visible names or descriptive copy.',
              );
              expect(tester.widget<IconButton>(button).tooltip, contains('\n'));
            }
            final off = find.byKey(const ValueKey('craft-effect-off'));
            final meaning = tester.widget<IconButton>(off).tooltip!;
            await tester.longPress(off);
            await tester.pump(const Duration(milliseconds: 200));
            expect(find.text(meaning), findsOneWidget);
            await tester.pump(const Duration(seconds: 3));
            await tester.pumpAndSettle();
            expect(
              find.text(meaning),
              findsNothing,
              reason:
                  'Effect meaning appears only for the hold, never as a stuck help region.',
            );
          }
        }
        for (final guidance in [
          strings.craftTargetHelp,
          strings.craftEffectInstruction,
          strings.craftFrameSwipeHint,
        ]) {
          expect(find.text(guidance), findsNothing);
        }
        expect(
          find.byKey(const ValueKey('craft-status-card')),
          findsNothing,
          reason:
              'Unchanged admission guidance must not become a permanent obscuring toast.',
        );
        expect(
          tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
          board,
        );
        expect(
          find.byKey(const ValueKey('craft-quick-fill')).hitTestable(),
          findsOneWidget,
          reason:
              'Fill remains available in the fixed quick-action rail across inspector tabs.',
        );
        expect(tester.takeException(), isNull);
        final directory = Platform.environment['LITCH_HEALTH_CAPTURE_DIR'];
        if (directory != null) {
          final boundary = tester.renderObject<RenderRepaintBoundary>(
            find.byKey(const ValueKey('physical-craft-root')),
          );
          await tester.runAsync(() async {
            final image = await boundary.toImage(pixelRatio: 2);
            final bytes = await image.toByteData(
              format: ui.ImageByteFormat.png,
            );
            final file = File('$directory/${scenario.$1}-$tab.png');
            await file.parent.create(recursive: true);
            await file.writeAsBytes(bytes!.buffer.asUint8List());
            image.dispose();
          });
        }
      }
      final strings = AppLocalizations.of(tester.element(editor));
      // Exercise the real filmstrip and toolbar gestures, not callback invocation.
      final strip = find.byKey(const ValueKey('craft-frame-filmstrip'));
      final addFrame = find.byKey(const ValueKey('craft-add-frame'));
      expect(find.descendant(of: strip, matching: addFrame), findsOneWidget);
      expect(
        tester.widget<IconButton>(addFrame).tooltip,
        strings.craftNewFrame,
      );
      await tester.tap(addFrame);
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames.length, 2);
      expect(library.selectedFrameIndex, 1);
      expect(find.byKey(const ValueKey('craft-duplicate-frame')), findsNothing);
      expect(find.byKey(const ValueKey('craft-delete-frame')), findsNothing);
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump();
      expect(
        library.selectedFrameIndex,
        1,
        reason: 'Play resumes the selected frame without an implicit seek.',
      );
      await tester.pump(const Duration(milliseconds: 1100));
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump();
      final frozen = List<int>.of(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors,
      );
      await tester.pump(const Duration(seconds: 2));
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors,
        frozen,
      );
      await tester.tap(find.byKey(const ValueKey('craft-palette')));
      await tester.pumpAndSettle();
      await tester.longPress(find.byKey(const ValueKey('craft-animate')));
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
        reason: 'Holding Frames toggles playback without also switching tabs.',
      );
      await tester.tap(find.byKey(const ValueKey('craft-animate')));
      await tester.pump();
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-preview')),
          matching: find.byIcon(Icons.pause),
        ),
        findsOneWidget,
      );
      await tester.longPress(find.byKey(const ValueKey('craft-animate')));
      await tester.pumpAndSettle();
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-preview')),
          matching: find.byIcon(Icons.play_arrow),
        ),
        findsOneWidget,
      );
      final frame = find.byKey(const ValueKey('craft-select-frame-0'));
      final beforeCancelledDelete = List.of(library.animationDocument.frames);
      await tester.longPress(frame);
      await tester.pumpAndSettle();
      expect(find.text(strings.craftDeleteFrameQuestion(1)), findsOneWidget);
      await tester.tap(find.text(strings.cancelAction));
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames, beforeCancelledDelete);
      await tester.tap(addFrame);
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames.length, 3);
      expect(library.selectedFrameIndex, 1);
      // Six deliberately distinct frames expose holes hidden by one-frame renders.
      while (library.animationDocument.frames.length < 6) {
        library.addFrame();
      }
      const frameColors = [
        0xff782d,
        0x39c78c,
        0x337cff,
        0x9252e8,
        0xf54275,
        0xffd343,
      ];
      for (var index = 0; index < 6; index++) {
        library.selectFrame(index);
        library.session.replaceCanvas(
          LitchcraftCanvas.blank().paint(
            [index, index + 8, index + 16],
            15,
            frameColors[index],
          ),
        );
      }
      library.selectFrame(0);
      await tester.pumpAndSettle();
      await _expectCraftFramesReachable(tester, library, 6);
      final filmGesture = await tester.startGesture(tester.getCenter(strip));
      await filmGesture.moveBy(const Offset(-45, 0));
      await tester.pump(const Duration(milliseconds: 100));
      expect(library.animationDocument.frames, hasLength(6));
      expect(library.selectedFrameIndex, 0);
      await filmGesture.moveBy(const Offset(-45, 0));
      await tester.pump(const Duration(milliseconds: 100));
      await filmGesture.up();
      await tester.pumpAndSettle();
      final offCenterFrame = find.byKey(const ValueKey('craft-select-frame-4'));
      // The fitted inspector exposes fewer thumbnails at once. Advance the
      // actual filmstrip until frame five is reachable, never the outer pager.
      for (
        var swipe = 0;
        swipe < 6 && offCenterFrame.hitTestable().evaluate().isEmpty;
        swipe++
      ) {
        await tester.drag(
          find.byKey(const ValueKey('craft-frame-scroll-viewport')),
          const Offset(-60, 0),
        );
        await tester.pumpAndSettle();
        expect(library.animationDocument.frames, hasLength(6));
        expect(library.selectedFrameIndex, 0);
      }
      expect(offCenterFrame.hitTestable(), findsOneWidget);
      await tester.tap(offCenterFrame);
      await tester.pumpAndSettle();
      expect(library.selectedFrameIndex, 4);
      await tester.longPress(offCenterFrame);
      await tester.pumpAndSettle();
      expect(find.text(strings.craftDeleteFrameQuestion(5)), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
        findsOneWidget,
      );
      await tester.tap(find.text(strings.cancelAction));
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames.length, 6);
      final stripCaptureDirectory =
          Platform.environment['LITCH_HEALTH_CAPTURE_DIR'];
      if (stripCaptureDirectory != null) {
        final boundary = tester.renderObject<RenderRepaintBoundary>(
          find.byKey(const ValueKey('physical-craft-root')),
        );
        await tester.runAsync(() async {
          final image = await boundary.toImage(pixelRatio: 2);
          final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
          await File(
            '$stripCaptureDirectory/${scenario.$1}-six-frames.png',
          ).writeAsBytes(bytes!.buffer.asUint8List());
          image.dispose();
        });
      }
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-3')));
      await tester.pumpAndSettle();
      final removedFrame = library.animationDocument.frames[4];
      await tester.longPress(offCenterFrame);
      await tester.pumpAndSettle();
      expect(find.text(strings.craftDeleteFrameQuestion(5)), findsOneWidget);
      await tester.tap(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
      );
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames.length, 5);
      expect(library.animationDocument.frames.contains(removedFrame), isFalse);
      craft.showStatus(LitchcraftStatus.gameActive);
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-artwork-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('craft-send-to-litch')), findsNothing);
      for (final key in ['craft-library', 'craft-save', 'craft-share']) {
        final control = find.byKey(ValueKey(key));
        expect(control.hitTestable(), findsOneWidget);
        expect(
          find.descendant(
            of: find.byKey(const ValueKey('craft-artwork-panel')),
            matching: control,
          ),
          findsOneWidget,
        );
        expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
      }
      if (stripCaptureDirectory != null) {
        final boundary = tester.renderObject<RenderRepaintBoundary>(
          find.byKey(const ValueKey('physical-craft-root')),
        );
        await tester.runAsync(() async {
          final image = await boundary.toImage(pixelRatio: 2);
          final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
          await File(
            '$stripCaptureDirectory/${scenario.$1}-artwork.png',
          ).writeAsBytes(bytes!.buffer.asUint8List());
          image.dispose();
        });
      }
      final retainedEditor = tester.state(editor);
      final retainedDocument = library.animationDocument;
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pumpAndSettle();
      expect(find.text(strings.craftLeaveUnsavedTitle), findsNothing);
      expect(craft.leaveCalls, 1);
      expect(library.animationDocument, same(retainedDocument));
      expect(
        tester.state(find.byType(LitchcraftScreen, skipOffstage: false)),
        same(retainedEditor),
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
    });
  }
  for (final rotation in <int>[1, 3]) {
    testWidgets(
      'connected Play honors Android status bar safe pane rotation $rotation',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.devicePixelRatio = 2;
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetPhysicalSize);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService(
          useProductionConsoleStream: true,
        )..reviewBoardRotationKnown = true;
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        State? retainedHome;
        for (final size in <Size>[
          const Size(1000, 472.5),
          const Size(844, 390),
          const Size(667, 375),
        ]) {
          double? insetBoardSize;
          for (final top in <double>[24, 0]) {
            final insets = rotation == 1
                ? EdgeInsets.fromLTRB(8, top, 34, 16)
                : EdgeInsets.fromLTRB(34, top, 8, 16);
            tester.view.physicalSize = size * 2;
            await tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme().copyWith(
                  platform: TargetPlatform.android,
                ),
                localizationsDelegates: AppLocalizations.localizationsDelegates,
                supportedLocales: AppLocalizations.supportedLocales,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(padding: insets, viewPadding: insets),
                  child: RepaintBoundary(
                    key: const ValueKey('connected-play-safe-root'),
                    child: child!,
                  ),
                ),
                home: LitchMainScaffold(
                  bleService: service,
                  boardId: 'board-1',
                  boards: const [],
                  onDisconnect: () async {},
                  craftController: craft,
                  craftLibrary: library,
                  initialPageIndex: 1,
                  initialCraftModeResolved: true,
                  firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
                ),
              ),
            );
            await tester.pumpAndSettle();
            service.reportHome();
            await tester.pumpAndSettle();
            await decodeVisibleReviewImages(tester);
            final home = tester.state(find.byType(HomeScreen));
            if (retainedHome == null) {
              retainedHome = home;
            } else {
              expect(home, same(retainedHome));
            }
            expect(
              find.byKey(const ValueKey('home-landscape-workspace')),
              findsOneWidget,
            );
            expect(find.byType(AppBar), findsNothing);
            final board = tester.getRect(
              find.byKey(const ValueKey('home-chessboard-surface')),
            );
            final safe = Rect.fromLTRB(
              insets.left,
              insets.top,
              size.width - insets.right,
              size.height - insets.bottom,
            );
            final nav = tester.getRect(
              find.byKey(const ValueKey('main-landscape-navigation')),
            );
            final navSurface = tester.getRect(
              find.byKey(const ValueKey('main-landscape-navigation-surface')),
            );
            // Capture the actual root at the HIL screenshot's 2000×945 pixels.
            final directory = Platform.environment['LITCH_HEALTH_CAPTURE_DIR'];
            if (directory != null && size.width == 1000 && top == 24) {
              final boundary = tester.renderObject<RenderRepaintBoundary>(
                find.byKey(const ValueKey('connected-play-safe-root')),
              );
              await tester.runAsync(() async {
                final image = await boundary.toImage(pixelRatio: 2);
                final bytes = await image.toByteData(
                  format: ui.ImageByteFormat.png,
                );
                final file = File(
                  '$directory/connected-play-rotation-$rotation-2000x945.png',
                );
                await file.parent.create(recursive: true);
                await file.writeAsBytes(bytes!.buffer.asUint8List());
                image.dispose();
              });
            }
            expect(
              board.top,
              greaterThanOrEqualTo(safe.top),
              reason:
                  'Android clock/status occupies the top ${insets.top}px; board=$board safe=$safe',
            );
            expect(board.bottom, lessThanOrEqualTo(safe.bottom));
            expect(board.left, greaterThanOrEqualTo(safe.left));
            expect(board.right, lessThanOrEqualTo(safe.right));
            expect(board.center.dy, closeTo(safe.center.dy, .01));
            expect(
              board.width,
              closeTo(
                LitchWorkspaceGeometry(
                  safe.size,
                  safeInsets: insets,
                ).landscapeCanvasSide,
                .01,
              ),
            );
            expect(board.height, closeTo(board.width, .01));
            expect(board.overlaps(navSurface), isFalse);
            expect(navSurface, nav);
            expect(
              nav.bottom,
              math.min(
                safe.bottom,
                size.height - LitchWorkspaceGeometry.landscapeGap,
              ),
            );
            expect(
              nav.right,
              math.min(
                safe.right,
                size.width - LitchWorkspaceGeometry.landscapeGap,
              ),
            );
            expect(nav.top, greaterThanOrEqualTo(safe.top));
            for (final key in <String>[
              'connected-board-status-action',
              'landscape-craft-action',
              'landscape-setup-action',
            ]) {
              final action = find.byKey(ValueKey(key));
              final bounds = tester.getRect(action);
              expect(action.hitTestable(), findsOneWidget);
              expect(
                tester.getSize(action).shortestSide,
                greaterThanOrEqualTo(48),
              );
              expect(
                safe.contains(bounds.topLeft) && safe.contains(bounds.center),
                isTrue,
              );
              expect(bounds.right, lessThanOrEqualTo(safe.right));
              expect(bounds.bottom, lessThanOrEqualTo(safe.bottom));
            }
            if (top == 24) {
              insetBoardSize = board.width;
            } else {
              expect(
                board.width - insetBoardSize!,
                inInclusiveRange(0, 24),
                reason:
                    'Adding a status inset cannot trigger a board-size cliff.',
              );
            }
            expect(tester.takeException(), isNull);
          }
        }
        await tester.pumpWidget(const SizedBox());
        await library.flush();
      },
    );
  }

  for (final scenario in <(String, TargetPlatform, EdgeInsets, double)>[
    ('zero', TargetPlatform.android, EdgeInsets.zero, 180),
    (
      'android',
      TargetPlatform.android,
      const EdgeInsets.fromLTRB(0, 24, 0, 16),
      140,
    ),
    ('ios', TargetPlatform.iOS, const EdgeInsets.fromLTRB(0, 20, 0, 0), 160),
    (
      'ios-notch',
      TargetPlatform.iOS,
      const EdgeInsets.fromLTRB(0, 44, 0, 34),
      102,
    ),
  ]) {
    testWidgets(
      'compact portrait production shell 320x640 200 ${scenario.$1}',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(320, 640);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'enable_app_console': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        var drawing = LitchcraftCanvas.blank();
        for (var row = 0; row < 8; row++) {
          drawing = drawing.paint(
            <int>[
              for (var col = 0; col < 8; col++)
                if ((row + col) % 3 != 0) row * 8 + col,
            ],
            15,
            <int>[0xf0a64b, 0x52d6c0, 0x54a6ed, 0xa18be8][row % 4],
          );
        }
        final craft = _NavigationCraftController(
          service,
          initialCanvas: drawing,
        )..forceLiveStatus = true;
        craft.simulatePhysicalMode(true);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await library.dismissWelcome();
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme().copyWith(platform: scenario.$2),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                padding: scenario.$3,
                viewPadding: scenario.$3,
                textScaler: const TextScaler.linear(2),
              ),
              child: RepaintBoundary(
                key: const ValueKey('portrait-production-render'),
                child: child!,
              ),
            ),
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              craftController: craft,
              craftLibrary: library,
              initialPageIndex: 0,
              initialCraftModeResolved: true,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final editor = tester.state(find.byType(LitchcraftScreen));
        expect(find.byType(AppBar), findsOneWidget);
        final board = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final content = tester.getRect(
          find.byKey(const ValueKey('craft-inspector-content')),
        );
        final footer = tester.getRect(
          find.byKey(const ValueKey('craft-history-footer')),
        );
        expect(board.width, greaterThanOrEqualTo(scenario.$4));
        expect(board.height, board.width);
        expect(board.top, greaterThanOrEqualTo(scenario.$3.top));
        expect(board.bottom + 8, lessThanOrEqualTo(inspector.top + .1));
        expect(
          content.height - 30,
          greaterThanOrEqualTo(100),
          reason: 'Content excludes top padding and continuation fade.',
        );
        expect(find.byKey(const ValueKey('craft-document-bar')), findsNothing);
        expect(
          find.byKey(const ValueKey('craft-selected-context')),
          findsNothing,
        );
        for (final key in <String>[
          'craft-undo',
          'craft-redo',
          'craft-rotate-artwork',
        ]) {
          final control = find.byKey(ValueKey(key));
          expect(
            tester.getSize(control).shortestSide,
            greaterThanOrEqualTo(48),
          );
          expect(footer.contains(tester.getCenter(control)), isTrue);
        }
        for (final key in <String>[
          'craft-artwork-menu',
          'craft-quick-brush',
          'craft-quick-previous-color',
          'connected-board-status-action',
        ]) {
          final control = find.byKey(ValueKey(key));
          expect(
            tester.getSize(control).shortestSide,
            greaterThanOrEqualTo(48),
          );
          expect(
            (Offset.zero & tester.view.physicalSize).contains(
              tester.getCenter(control),
            ),
            isTrue,
            reason: key,
          );
        }
        final nav = tester.widget<NavigationBar>(find.byType(NavigationBar));
        expect(
          nav.destinations
              .cast<NavigationDestination>()
              .map((d) => d.label)
              .toList(),
          <String>['Play', 'Litchcraft', 'Setup', 'Console'],
        );
        expect(nav.selectedIndex, 1);
        expect(
          tester
              .getRect(find.byKey(const ValueKey('main-navigation-surface')))
              .bottom,
          640,
        );
        expect(
          tester.getRect(find.byType(NavigationBar)).bottom,
          640 - scenario.$3.bottom,
        );
        await captureHealthReview(
          tester,
          'portrait-production-${scenario.$1}-320x640-200-color',
          boundaryFinder: find.byKey(
            const ValueKey('portrait-production-render'),
          ),
        );
        for (final key in <String>[
          'craft-inspector-hue',
          'craft-inspector-hex',
        ]) {
          final target = find.byKey(ValueKey(key));
          final scroll = await tester.startGesture(
            Offset(content.right - 3, content.center.dy),
          );
          for (var step = 0; step < 60; step++) {
            final rect = tester.getRect(target);
            if (rect.top >= content.top &&
                rect.bottom <= content.bottom - 18 &&
                target.hitTestable().evaluate().isNotEmpty) {
              break;
            }
            final shift = (content.center.dy - 9 - rect.center.dy).clamp(
              -24.0,
              24.0,
            );
            await scroll.moveBy(Offset(0, shift));
            await tester.pump(const Duration(milliseconds: 80));
          }
          await tester.pump(const Duration(milliseconds: 250));
          await scroll.up();
          await tester.pump(const Duration(milliseconds: 120));
          expect(
            target.hitTestable(),
            findsOneWidget,
            reason: '$key is reachable by an actual inspector scroll',
          );
          expect(
            tester.getRect(target).top,
            greaterThanOrEqualTo(content.top),
            reason: '$key ${tester.getRect(target)} in $content',
          );
          expect(
            tester.getRect(target).bottom,
            lessThanOrEqualTo(content.bottom - 18),
          );
          expect(
            tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
            board,
          );
          expect(
            tester.getRect(find.byKey(const ValueKey('craft-history-footer'))),
            footer,
          );
        }
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pumpAndSettle();
        expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
        expect(
          tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
          board,
        );
        expect(
          tester.getRect(find.byKey(const ValueKey('craft-history-footer'))),
          footer,
        );
        await captureHealthReview(
          tester,
          'portrait-production-${scenario.$1}-320x640-200-animation',
          boundaryFinder: find.byKey(
            const ValueKey('portrait-production-render'),
          ),
        );
        await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
        await tester.pumpAndSettle();
        for (final key in <String>[
          'craft-library',
          'craft-save',
          'craft-share',
          'craft-rotate-artwork',
        ]) {
          final action = find.byKey(ValueKey(key));
          expect(action.hitTestable(), findsOneWidget);
          expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
        }
        await tester.tap(find.byKey(const ValueKey('craft-palette')));
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey('connected-board-status-action')),
        );
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('connected-board-popover')),
          findsOneWidget,
        );
        Navigator.of(
          tester.element(find.byKey(const ValueKey('connected-board-popover'))),
        ).pop();
        await tester.pumpAndSettle();
        expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
      },
    );
  }
  for (final consoleEnabled in <bool>[false, true]) {
    for (final platform in <TargetPlatform>[
      TargetPlatform.iOS,
      TargetPlatform.android,
    ]) {
      testWidgets(
        'ordered navigation icons and edge surfaces ${platform.name} console $consoleEnabled',
        (tester) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.view.resetPhysicalSize);
          SharedPreferences.setMockInitialValues(<String, Object>{
            'enable_app_console': consoleEnabled,
          });
          final service = _ConnectionGateBleService();
          service.emitConnection('board-1', DeviceConnectionState.connected);
          final craft = _RibbonCraftController(service)..forceLiveStatus = true;
          craft.simulatePhysicalMode(true);
          final library = LitchcraftLibrary(craft);
          await library.ready;
          await library.dismissWelcome();
          addTearDown(() async {
            library.dispose();
            craft.dispose();
            await service.dispose();
          });
          Future<void> show(
            Size size,
            EdgeInsets insets,
            Alignment side,
          ) async {
            tester.view.physicalSize = size;
            await tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme().copyWith(platform: platform),
                localizationsDelegates: AppLocalizations.localizationsDelegates,
                supportedLocales: AppLocalizations.supportedLocales,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context).copyWith(
                    padding: insets,
                    viewPadding: insets,
                    textScaler: const TextScaler.linear(2),
                    disableAnimations: true,
                  ),
                  child: child!,
                ),
                home: LitchMainScaffold(
                  bleService: service,
                  boardId: 'board-1',
                  boards: const [],
                  onDisconnect: () async {},
                  craftController: craft,
                  craftLibrary: library,
                  initialPageIndex: 0,
                  initialCraftModeResolved: true,
                  landscapeRibbonSide: side,
                  firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
                ),
              ),
            );
            await tester.pumpAndSettle();
          }

          final expected = <String>[
            'Play',
            'Litchcraft',
            'Setup',
            if (consoleEnabled) 'Console',
          ];
          await show(
            const Size(390, 844),
            const EdgeInsets.only(top: 24, bottom: 34),
            Alignment.centerRight,
          );
          final editor = tester.state(find.byType(LitchcraftScreen));
          final bar = tester.widget<NavigationBar>(find.byType(NavigationBar));
          expect(
            bar.destinations.cast<NavigationDestination>().map(
              (item) => item.label,
            ),
            expected,
          );
          expect(bar.selectedIndex, 1);
          final surface = tester.getRect(
            find.byKey(const ValueKey<String>('main-navigation-surface')),
          );
          final controls = tester.getRect(find.byType(NavigationBar));
          expect(surface.bottom, 844);
          expect(surface.left, 0);
          expect(surface.right, 390);
          expect(controls.bottom, 844 - 34);
          expect(
            surface.height - controls.height,
            34,
            reason: 'Bottom inset is painted and consumed exactly once.',
          );
          for (final insets in <EdgeInsets>[
            EdgeInsets.zero,
            const EdgeInsets.fromLTRB(7, 24, 13, 34),
          ]) {
            await show(const Size(390, 844), insets, Alignment.centerRight);
            final edge = tester.getRect(
              find.byKey(const ValueKey<String>('main-navigation-surface')),
            );
            final inner = tester.getRect(find.byType(NavigationBar));
            expect(edge.bottom, 844);
            expect(edge.left, 0);
            expect(edge.right, 390);
            expect(inner.bottom, 844 - insets.bottom);
            expect(inner.left, insets.left);
            expect(inner.right, 390 - insets.right);
            expect(edge.height - inner.height, insets.bottom);
            expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
          }
          for (final side in [Alignment.centerLeft, Alignment.centerRight]) {
            await show(
              const Size(800, 1100),
              const EdgeInsets.fromLTRB(17, 24, 9, 21),
              side,
            );
            final bar = tester.getRect(find.byType(NavigationBar));
            expect(bar, const Rect.fromLTRB(17, 1031, 791, 1079));
            for (final label in expected) {
              final destination = find.byWidgetPredicate(
                (widget) =>
                    widget is NavigationDestination && widget.label == label,
              );
              final target = tester.getRect(destination);
              final labelFinder = find.descendant(
                of: destination,
                matching: find.text(label),
              );
              final text = tester.getRect(labelFinder);
              expect(target.height, 48);
              expect(target.width, greaterThanOrEqualTo(48));
              expect(destination.hitTestable(), findsOneWidget);
              expect(text.top, greaterThanOrEqualTo(target.top));
              expect(text.bottom, lessThanOrEqualTo(target.bottom));
              final focus = Focus.of(tester.element(labelFinder));
              expect(
                focus.context!
                    .findAncestorWidgetOfExactType<NavigationDestination>()
                    ?.label,
                label,
              );
              focus.requestFocus();
              await tester.pump();
              expect(focus.hasPrimaryFocus, isTrue);
            }
          }
          await show(
            const Size(844, 600),
            const EdgeInsets.fromLTRB(30, 0, 8, 21),
            Alignment.centerRight,
          );
          final tabletSurface = tester.getRect(
            find.byKey(const ValueKey<String>('main-navigation-surface')),
          );
          final tabletControls = tester.getRect(
            find.byKey(const ValueKey<String>('bottom-navigation-controls')),
          );
          expect(tabletSurface.size, const Size(844, 64 + 21));
          expect(tabletSurface.bottom, 600);
          expect(tabletControls.size, const Size(320, 64));
          expect(tabletControls.right, 844 - 8 - 12);
          expect(tabletControls.bottom, 600 - 21);
          expect(
            tester
                .getSize(
                  find.byKey(const ValueKey('craft-persistent-inspector')),
                )
                .width,
            tabletControls.width,
          );
          expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
          await show(
            const Size(844, 600),
            const EdgeInsets.fromLTRB(30, 0, 8, 21),
            Alignment.centerLeft,
          );
          final leftControls = tester.getRect(
            find.byKey(const ValueKey<String>('bottom-navigation-controls')),
          );
          expect(leftControls.left, 30 + 12);
          expect(leftControls.size, tabletControls.size);
          const finalLandscapeInsets = EdgeInsets.fromLTRB(8, 0, 30, 21);
          for (final insets in <EdgeInsets>[
            EdgeInsets.zero,
            const EdgeInsets.fromLTRB(30, 0, 8, 21),
            finalLandscapeInsets,
          ]) {
            final side = insets.left > insets.right
                ? Alignment.centerLeft
                : Alignment.centerRight;
            await show(const Size(844, 390), insets, side);
            expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
            final canvas = tester.getRect(
              find.byKey(const ValueKey('litchcraft-canvas')),
            );
            final navigation = find.byKey(
              const ValueKey<String>('craft-landscape-navigation'),
            );
            final launcherRect = tester.getRect(navigation);
            expect(launcherRect.height, greaterThanOrEqualTo(48));
            for (final destination in <String>['play', 'craft', 'setup']) {
              final action = find.byKey(
                ValueKey<String>('landscape-$destination-action'),
              );
              expect(action.hitTestable(), findsOneWidget);
              expect(
                tester.getSize(action).shortestSide,
                greaterThanOrEqualTo(48),
              );
            }
            expect(
              launcherRect.overlaps(canvas),
              isFalse,
              reason:
                  'Navigation $launcherRect must not cover canvas $canvas with $insets.',
            );
            for (final name in <String>[
              'play',
              'craft',
              'setup',
              if (consoleEnabled) 'console',
            ]) {
              final item = find.byKey(
                ValueKey<String>('landscape-$name-action'),
              );
              final rect = tester.getRect(item);
              expect(rect.height, greaterThanOrEqualTo(48));
              expect(rect.left, greaterThanOrEqualTo(insets.left));
              expect(rect.right, lessThanOrEqualTo(844 - insets.right));
              expect(rect.bottom, lessThanOrEqualTo(390 - insets.bottom));
              expect(
                rect.overlaps(canvas),
                isFalse,
                reason:
                    'The scaled menu must stay in its document-side gutter.',
              );
              expect(
                find.descendant(
                  of: item,
                  matching: switch (name) {
                    'play' => find.byType(LitchPlayGlyph),
                    'craft' => find.byType(LitchcraftIdentityGlyph),
                    _ => find.byType(Icon),
                  },
                ),
                findsOneWidget,
              );
            }
            await tester.tap(
              find.byKey(const ValueKey<String>('landscape-craft-action')),
            );
            await tester.pumpAndSettle();
            expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
            expect(tester.takeException(), isNull);
          }
          if (consoleEnabled) {
            await tester.tap(
              find.byKey(const ValueKey<String>('landscape-console-action')),
            );
            await tester.pumpAndSettle();
            expect(find.byType(AppConsoleScreen), findsOneWidget);
            await tester.tap(
              find.byKey(const ValueKey<String>('landscape-craft-action')),
            );
            await tester.pumpAndSettle();
            expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
          }
          await tester.tap(
            find.byKey(const ValueKey<String>('landscape-setup-action')),
          );
          await tester.pumpAndSettle();
          expect(find.byType(SettingsScreen), findsOneWidget);
          final landscapeControls = tester.getRect(
            find.byKey(const ValueKey('main-landscape-navigation')),
          );
          final landscapeSurface = tester.getRect(
            find.byKey(
              const ValueKey<String>('main-landscape-navigation-surface'),
            ),
          );
          expect(landscapeSurface, landscapeControls);
          expect(
            landscapeControls.bottom,
            390 - math.max(21, LitchWorkspaceGeometry.landscapeGap),
          );
          expect(
            landscapeControls.left,
            greaterThanOrEqualTo(finalLandscapeInsets.left),
          );
          expect(
            landscapeControls.right,
            lessThanOrEqualTo(844 - finalLandscapeInsets.right),
          );
          await tester.tap(find.byKey(const ValueKey('landscape-play-action')));
          await tester.pumpAndSettle();
          final leave = find.byKey(const ValueKey('craft-leave-unsaved'));
          if (leave.evaluate().isNotEmpty) {
            await tester.tap(leave);
            await tester.pumpAndSettle();
          }
          expect(find.byType(HomeScreen), findsOneWidget);
          for (final insets in <EdgeInsets>[
            EdgeInsets.zero,
            const EdgeInsets.fromLTRB(30, 0, 8, 21),
            const EdgeInsets.fromLTRB(8, 0, 30, 21),
          ]) {
            await show(const Size(844, 390), insets, Alignment.centerRight);
            final playSurface = tester.getRect(
              find.byKey(const ValueKey('main-landscape-navigation-surface')),
            );
            final playControls = tester.getRect(
              find.byKey(const ValueKey('main-landscape-navigation')),
            );
            expect(playSurface, playControls);
            expect(
              playControls.bottom,
              390 -
                  math.max(insets.bottom, LitchWorkspaceGeometry.landscapeGap),
            );
            expect(
              playControls.right,
              844 - math.max(insets.right, LitchWorkspaceGeometry.landscapeGap),
            );
            expect(playControls.left, greaterThanOrEqualTo(insets.left));
          }
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox());
        },
      );
    }
  }
  testWidgets('4A connected shell retains identity across ribbon sides', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _RibbonCraftController(service)..forceLiveStatus = true;
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await library.dismissWelcome();
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });
    Future<void> show(
      Alignment? side,
      String name, {
      Future<DeviceLandscapeSide?> Function()? readDeviceSide,
    }) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              padding: const EdgeInsets.fromLTRB(30, 0, 30, 21),
              viewPadding: const EdgeInsets.fromLTRB(30, 0, 30, 21),
              disableAnimations: true,
            ),
            child: RepaintBoundary(
              key: const ValueKey('4a-connected-render'),
              child: child!,
            ),
          ),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            landscapeRibbonSide: side,
            deviceLandscapeSideReader: readDeviceSide,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-landscape-navigation')),
        findsOneWidget,
      );
      final status = find.byKey(
        const ValueKey('connected-board-status-action'),
      );
      expect(status.hitTestable(), findsOneWidget);
      expect(tester.getSize(status).shortestSide, greaterThanOrEqualTo(48));
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-landscape-navigation')),
          matching: status,
        ),
        findsOneWidget,
        reason: 'Board status stays in mini-nav chrome outside the canvas.',
      );
      final canvasRect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final toolbarRect = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      expect(canvasRect.width, greaterThan(300));
      if (side?.x.isNegative ?? false) {
        expect(toolbarRect.right, lessThan(canvasRect.left));
      } else if (side != null) {
        expect(toolbarRect.left, greaterThan(canvasRect.right));
      }
      await captureHealthReview(
        tester,
        '4a-connected-$name',
        boundaryFinder: find.byKey(const ValueKey('4a-connected-render')),
      );
    }

    await show(Alignment.centerLeft, 'landscape-left');
    final editor = tester.state(find.byType(LitchcraftScreen));
    final document = library.animationDocument;
    final calls = (craft.enterCalls, craft.leaveCalls, craft.adoptCalls);
    await show(Alignment.centerRight, 'landscape-right');
    expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
    expect(library.animationDocument, same(document));
    expect((craft.enterCalls, craft.leaveCalls, craft.adoptCalls), calls);
    final gesture = await tester.startGesture(
      tester.getCenter(find.byKey(const ValueKey('litchcraft-canvas'))),
    );
    await gesture.moveBy(const Offset(20, 0));
    await tester.pump();
    final ends = craft.strokeEnds;
    await show(Alignment.centerLeft, 'landscape-left-after-held-flip');
    expect(craft.strokeEnds, ends + 1);
    final afterFlip = craft.canvas;
    await gesture.moveBy(const Offset(30, 0));
    await gesture.up();
    await tester.pump();
    expect(craft.strokeEnds, ends + 1);
    expect(craft.canvas, same(afterFlip));
    expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
    await tester.pump(const Duration(milliseconds: 601));
    await library.flush();
    await show(
      null,
      'landscape-native-left',
      readDeviceSide: () async => DeviceLandscapeSide.left,
    );
    expect(
      tester.getRect(find.byKey(const ValueKey('craft-toolbar'))).right,
      lessThan(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))).left,
      ),
    );
    final navigation = find.byKey(const ValueKey('craft-landscape-navigation'));
    final navigationRect = tester.getRect(navigation);
    final canvasRect = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    expect(navigationRect.overlaps(canvasRect), isFalse);
    expect(find.byKey(const ValueKey('landscape-more-action')), findsNothing);
    for (final key in <String>[
      'landscape-play-action',
      'landscape-craft-action',
      'landscape-setup-action',
    ]) {
      final action = find.byKey(ValueKey<String>(key));
      expect(tester.getSize(action).shortestSide, greaterThanOrEqualTo(48));
      expect(action.hitTestable(), findsOneWidget);
    }
    await tester.tap(find.byKey(const ValueKey('landscape-craft-action')));
    await tester.pumpAndSettle();
    final retainedStatus = find.byKey(
      const ValueKey<String>('connected-board-status-action'),
    );
    expect(retainedStatus.hitTestable(), findsOneWidget);
    expect(
      tester.getSize(retainedStatus).shortestSide,
      greaterThanOrEqualTo(48),
    );
    expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets(
    'narrow 200 percent Litchcraft bounds busy status and optional navigation',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(540, 360);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'enable_app_console': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _RibbonCraftController(service)..forceLiveStatus = true;
      craft.simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await library.dismissWelcome();
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });

      Future<void> pump({required bool resolving}) => tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: RepaintBoundary(
              key: const ValueKey('compact-footer-root'),
              child: child!,
            ),
          ),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            modeResolving: resolving,
            landscapeRibbonSide: Alignment.centerLeft,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );

      await pump(resolving: true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      final editor = tester.state(find.byType(LitchcraftScreen));
      final footer = tester.getRect(
        find.byKey(const ValueKey('craft-landscape-navigation')),
      );
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      expect(
        find.byKey(const ValueKey<String>('craft-context-progress')),
        findsNothing,
        reason: 'connected navigation never blocks on ordinary mode sync',
      );
      expect(footer.overlaps(canvas), isFalse);
      expect(tester.takeException(), isNull);

      await pump(resolving: false);
      await tester.pumpAndSettle();
      expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
      expect(find.byTooltip('Effects'), findsOneWidget);
      expect(find.byTooltip('Animation'), findsOneWidget);
      final historyFooter = tester.getRect(
        find.byKey(const ValueKey('craft-history-footer')),
      );
      expect(historyFooter.overlaps(canvas), isFalse);
      for (final key in <String>[
        'craft-undo',
        'craft-redo',
        'craft-rotate-artwork',
      ]) {
        final control = find.byKey(ValueKey(key));
        final rect = tester.getRect(control);
        expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
        expect(historyFooter.contains(rect.center), isTrue);
      }
      final artwork = find.byKey(const ValueKey('craft-artwork-menu'));
      final artworkTab = tester.getRect(
        find.byKey(const ValueKey('craft-inspector-tab-artwork')),
      );
      final toolGrid = tester.getRect(
        find.byKey(const ValueKey('craft-tool-grid')),
      );
      expect(artwork, findsOneWidget);
      expect(tester.getSize(artwork).height, greaterThanOrEqualTo(48));
      expect(toolGrid.contains(artworkTab.center), isTrue);
      expect(historyFooter.overlaps(artworkTab), isFalse);
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('craft-history-footer')),
          matching: find.byType(VerticalDivider),
        ),
        findsNothing,
      );
      await captureHealthReview(
        tester,
        'compact-footer-540x360-200',
        boundaryFinder: find.byKey(const ValueKey('compact-footer-root')),
      );
      await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
      await tester.pumpAndSettle();
      for (final key in <String>[
        'craft-library',
        'craft-save',
        'craft-share',
        'craft-rotate-artwork',
      ]) {
        final action = find.byKey(ValueKey(key));
        expect(action.hitTestable(), findsOneWidget);
        expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
      }
      await captureHealthReview(
        tester,
        'compact-footer-artwork-menu-540x360-200',
        boundaryFinder: find.byKey(const ValueKey('compact-footer-root')),
      );
      await tester.tap(find.byKey(const ValueKey('craft-palette')));
      await tester.pumpAndSettle();
      final fill = find.byKey(const ValueKey('craft-quick-fill'));
      expect(fill.hitTestable(), findsOneWidget);
      expect(tester.getSize(fill).shortestSide, greaterThanOrEqualTo(48));
      expect(tester.getRect(fill).overlaps(canvas), isFalse);
      final fillSemantics = tester.widget<Semantics>(
        find.byKey(const ValueKey('craft-quick-fill-semantics')),
      );
      expect(fillSemantics.properties.onTap, isNotNull);
      expect(fillSemantics.properties.onLongPress, isNotNull);
      await captureHealthReview(
        tester,
        'compact-footer-fill-menu-540x360-200',
        boundaryFinder: find.byKey(const ValueKey('compact-footer-root')),
      );
      final more = find.byKey(const ValueKey<String>('landscape-more-action'));
      expect(more, findsNothing);
      for (final key in <String>[
        'landscape-play-action',
        'landscape-setup-action',
        'landscape-console-action',
      ]) {
        final action = find.byKey(ValueKey<String>(key));
        expect(action, findsOneWidget);
        expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
        expect(
          action.hitTestable(),
          findsOneWidget,
          reason: '$key: ${tester.getRect(action)}',
        );
      }
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets('Connected affordance opens the canonical board controls', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    _usePortraitPhone(tester);
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await _pumpConnectedGate(tester, service);

    expect(
      find.byKey(const ValueKey<String>('connected-board-status-chevron')),
      findsOneWidget,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);

    final Rect anchor = tester.getRect(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    final Rect popover = tester.getRect(
      find.byKey(const ValueKey<String>('connected-board-popover')),
    );
    expect(popover.top, greaterThanOrEqualTo(anchor.bottom));
    expect(popover.width, lessThan(tester.view.physicalSize.width));
    expect(
      popover.height,
      lessThan(520),
      reason: 'the Connected popover shrink-wraps its actual controls',
    );
    expect(find.byType(BottomSheet), findsNothing);
    expect(find.byKey(const ValueKey('board-menu-brightness')), findsOneWidget);
    expect(
      find.byKey(const ValueKey('board-menu-auto-brightness')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('board-menu-identify')), findsNothing);
    expect(find.byKey(const ValueKey('board-menu-rotate')), findsOneWidget);
    expect(find.byType(CircularProgressIndicator), findsNothing);
    expect(service.settingsReadCalls, 0);
    await captureHealthReview(
      tester,
      'connected-sleep-default',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );

    await tester.tap(find.byKey(const ValueKey('board-menu-rotate')));
    await tester.pumpAndSettle();
    expect(
      service.settingWrites,
      contains((key: 'BOARD_ROTATE', value: true, awaitAck: true)),
    );
  });

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final scenario
        in <({String name, Size size, double scale, Locale locale})>[
          (
            name: 'portrait',
            size: Size(390, 844),
            scale: 1,
            locale: Locale('en'),
          ),
          (
            name: 'portrait-200',
            size: Size(390, 844),
            scale: 2,
            locale: Locale('en'),
          ),
          (
            name: 'landscape',
            size: Size(844, 390),
            scale: 1,
            locale: Locale('en'),
          ),
          (
            name: 'german-200',
            size: Size(390, 844),
            scale: 2,
            locale: Locale('de'),
          ),
          (
            name: 'rtl-200',
            size: Size(390, 844),
            scale: 2,
            locale: Locale('en'),
          ),
        ]) {
      testWidgets(
        'Connected menu aligns actions with its content ${platform.name} ${scenario.name}',
        (tester) async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final semantics = tester.ensureSemantics();
          tester.view.physicalSize = scenario.size;
          tester.view.devicePixelRatio = 1;
          tester.platformDispatcher.localesTestValue = [scenario.locale];
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.platformDispatcher.clearLocalesTestValue);
          final service = _PagedSettingsBleService();
          addTearDown(service.dispose);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          late ValueChanged<FirmwareUpdateAvailability> publishAvailability;
          await _pumpConnectedGate(
            tester,
            service,
            theme: buildLitchTheme().copyWith(platform: platform),
            textScaler: TextScaler.linear(scenario.scale),
            textDirection: scenario.name == 'rtl-200'
                ? TextDirection.rtl
                : null,
            firmwarePageBuilder: (_, _, _, _, changed) {
              publishAvailability = changed;
              return const SizedBox.shrink();
            },
          );
          publishAvailability(
            const FirmwareUpdateAvailability(
              installedVersion: '0.9.0-7',
              latestVersion: '0.9.0-7',
              supportsUpdate: true,
              loading: false,
            ),
          );
          await tester.pumpAndSettle();
          await tester.tap(
            find.byKey(const ValueKey('connected-board-status-action')),
          );
          await tester.pumpAndSettle();
          await decodeVisibleReviewImages(tester);
          final sleep = find.byKey(const ValueKey('board-menu-sleep'));
          final disconnect = find.byKey(
            const ValueKey('board-menu-disconnect'),
          );
          await tester.ensureVisible(sleep);
          await tester.pumpAndSettle();
          final strings = AppLocalizations.of(tester.element(sleep));
          void expectActionSemantics(
            Finder action,
            String label, {
            bool enabled = true,
            bool pending = false,
          }) {
            final node = tester.getSemantics(action);
            expect(
              node,
              isSemantics(
                label: label,
                isButton: true,
                hasEnabledState: true,
                isEnabled: enabled,
                isFocusable: enabled,
                hasTapAction: enabled,
                isLiveRegion: pending,
              ),
            );
            expect(node.childrenCountInTraversalOrder, 0);
          }

          expectActionSemantics(sleep, strings.sleepAction);
          expectActionSemantics(disconnect, strings.disconnectAction);
          _expectSleepTextThenIcon(tester, sleep);
          if (scenario.name == 'german-200') {
            final label = tester.renderObject<RenderParagraph>(
              find.descendant(of: sleep, matching: find.byType(Text)),
            );
            expect(
              label.getBoxesForSelection(
                TextSelection(
                  baseOffset: 0,
                  extentOffset: strings.sleepAction.length,
                ),
              ),
              hasLength(1),
              reason: 'The German action name must not break inside its word.',
            );
          }
          final captureName =
              'connected-menu-${platform.name}-${scenario.name}';
          await captureHealthReview(
            tester,
            captureName,
            boundaryFinder: find.byKey(const ValueKey('connected-gate-render')),
          );
          // Include the scroller's bottom padding in the footer the user sees.
          final menu = find.byKey(const ValueKey('board-menu-scroll'));
          final contentBounds = tester.getRect(
            find.descendant(of: menu, matching: find.byType(Column)).first,
          );
          final divider = tester.getRect(
            find.descendant(of: menu, matching: find.byType(Divider)).last,
          );
          final padding = tester
              .widget<ListView>(menu)
              .padding!
              .resolve(Directionality.of(tester.element(menu)));
          final softwareTile = find
              .ancestor(
                of: find.byIcon(Icons.system_update_outlined),
                matching: find.byType(ListTile),
              )
              .first;
          final tileContent = tester
              .getRect(
                find
                    .descendant(of: softwareTile, matching: find.byType(Icon))
                    .first,
              )
              .expandToInclude(
                tester.getRect(
                  find
                      .descendant(of: softwareTile, matching: find.byType(Text))
                      .first,
                ),
              );
          final footer = Rect.fromLTRB(
            tileContent.left,
            divider.bottom,
            tileContent.right,
            contentBounds.bottom + padding.bottom,
          );
          final disconnectRect = tester.getRect(disconnect);
          final sleepRect = tester.getRect(sleep);
          expect(disconnectRect.top, closeTo(sleepRect.top, .1));
          expect(disconnectRect.bottom, closeTo(sleepRect.bottom, .1));
          final rtl =
              Directionality.of(tester.element(sleep)) == TextDirection.rtl;
          expect(
            rtl ? sleepRect.right : disconnectRect.right,
            lessThanOrEqualTo(rtl ? disconnectRect.left : sleepRect.left),
          );
          for (final (index, action) in [disconnect, sleep].indexed) {
            final target = tester.getRect(action);
            expect(target.shortestSide, greaterThanOrEqualTo(48));
            _expectConnectionMenuContentPosition(
              tester,
              action,
              alignEnd: index == 1,
            );
          }
          expect(disconnectRect.left, greaterThanOrEqualTo(footer.left));
          expect(sleepRect.right, lessThanOrEqualTo(footer.right));
          final target = tester.getRect(sleep);
          final icon = tester.getRect(
            find.descendant(of: sleep, matching: find.byType(Icon)),
          );
          service.sleepResponse = Completer<ConnectionSessionOutcome?>();
          await tester.tap(sleep);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 250));
          expect(tester.getRect(sleep), target);
          expect(
            tester.getRect(
              find.descendant(
                of: sleep,
                matching: find.byType(CircularProgressIndicator),
              ),
            ),
            icon,
          );
          _expectConnectionMenuContentPosition(tester, sleep, alignEnd: true);
          _expectSleepTextThenIcon(tester, sleep);
          expect(tester.widget<TextButton>(disconnect).onPressed, isNull);
          expectActionSemantics(
            sleep,
            strings.sleeping,
            enabled: false,
            pending: true,
          );
          expectActionSemantics(
            disconnect,
            strings.disconnectAction,
            enabled: false,
          );
          await captureHealthReview(
            tester,
            '$captureName-pending',
            boundaryFinder: find.byKey(const ValueKey('connected-gate-render')),
          );
          service.sleepResponse!.complete(null);
          await tester.pumpAndSettle();
          expectActionSemantics(sleep, strings.sleepAction);
          expectActionSemantics(disconnect, strings.disconnectAction);
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          semantics.dispose();
        },
      );
    }
  }

  testWidgets('Connected menu explicitly labels the actual installed firmware', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    PackageInfo.setMockInitialValues(
      appName: 'Foxie Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.5.0',
      buildNumber: '1',
      buildSignature: '',
    );
    _usePortraitPhone(tester);
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    final noticeScale = ValueNotifier<TextScaler>(const TextScaler.linear(2));
    addTearDown(noticeScale.dispose);
    tester.platformDispatcher.platformBrightnessTestValue = Brightness.light;
    addTearDown(tester.platformDispatcher.clearPlatformBrightnessTestValue);
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    addTearDown(tester.platformDispatcher.clearAccessibilityFeaturesTestValue);
    late ValueChanged<FirmwareUpdateAvailability> publishAvailability;
    await _pumpConnectedGate(
      tester,
      service,
      textScalerOverride: noticeScale,
      theme: buildLitchTheme(),
      darkTheme: buildLitchDarkTheme(),
      firmwarePageBuilder: (_, _, _, _, changed) {
        publishAvailability = changed;
        return const SizedBox.shrink();
      },
    );
    final action = find.byKey(const ValueKey('connected-board-status-action'));
    Future<void> openMenu(String? installed, String? latest) async {
      publishAvailability(
        FirmwareUpdateAvailability(
          installedVersion: installed,
          latestVersion: latest,
          supportsUpdate: true,
          loading: false,
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(action);
      await tester.pumpAndSettle();
    }

    await openMenu('0.9.0-7', '0.9.0-7');
    expect(find.text('Current firmware version: 0.9.0-7'), findsOneWidget);
    tester.state<NavigatorState>(find.byType(Navigator)).pop();
    await tester.pumpAndSettle();
    await openMenu('0.9.0-8', '0.9.0-8');
    expect(find.text('Current firmware version: 0.9.0-8'), findsOneWidget);
    expect(find.text('Current firmware version: 0.9.0-7'), findsNothing);
    tester.state<NavigatorState>(find.byType(Navigator)).pop();
    await tester.pumpAndSettle();
    await openMenu('0.9.0-8', '0.9.0-9');
    expect(find.text('Update available'), findsOneWidget);
    expect(
      find.text('Current firmware version: 0.9.0-8 → 0.9.0-9'),
      findsOneWidget,
    );
    tester.state<NavigatorState>(find.byType(Navigator)).pop();
    await tester.pumpAndSettle();
    await openMenu(null, '0.9.0-9');
    final tile = tester.widget<ListTile>(
      find.ancestor(
        of: find.text('Check for updates'),
        matching: find.byType(ListTile),
      ),
    );
    expect(tile.subtitle, isNull);
    expect(find.text('Update available'), findsNothing);
    tester.state<NavigatorState>(find.byType(Navigator).first).pop();
    await tester.pumpAndSettle();

    FirmwareUpdateAvailability offer(
      String version, {
      String installed = '0.63-beta',
      FirmwareReleaseInfo? release,
      String? boardId,
      int? token,
      String? releaseNotes,
    }) {
      final selected = release ?? FirmwareReleaseInfo.stable;
      final notes =
          releaseNotes ?? 'Version: $version\n\nVerified notes for $version.';
      return FirmwareUpdateAvailability(
        installedVersion: installed,
        latestVersion: version,
        supportsUpdate: true,
        loading: false,
        boardId: boardId ?? service.connectedId,
        connectionToken: token ?? service.connectionSessionToken,
        snapshot: FirmwareReleaseSnapshot(
          release: selected,
          manifest: makeFirmwareManifest(
            makeEsp32s3Firmware(),
            channel: selected.channel.name,
            manifestUrl: selected.manifestUrl,
            version: version,
            notesMarkdown: notes,
          ),
          notesMarkdown: notes,
        ),
      );
    }

    final prompt = find.byKey(const ValueKey<String>('firmware-update-offer'));
    publishAvailability(offer('1.0.3', installed: 'custom-build'));
    await tester.pumpAndSettle();
    expect(prompt, findsNothing);
    publishAvailability(offer('1.0.1-0', installed: '1.0.2'));
    await tester.pumpAndSettle();
    expect(
      prompt,
      findsNothing,
      reason: 'Manual downgrade is not a newer offer.',
    );
    final actualNotes = File(
      '../docs/release/initial_1.0.2.txt',
    ).readAsStringSync();
    final actualOffer = offer(
      '1.0.2',
      installed: '1.0.1-0',
      releaseNotes: 'Version: 1.0.2\n\n$actualNotes',
    );
    // Metadata may arrive while the user is using an existing root popup.
    await tester.tap(action);
    await tester.pumpAndSettle();
    publishAvailability(actualOffer);
    await tester.pumpAndSettle();
    expect(prompt, findsNothing);
    tester.state<NavigatorState>(find.byType(Navigator).first).pop();
    await tester.pumpAndSettle();
    expect(prompt, findsOneWidget);
    expect(find.text('Board firmware update: 1.0.2'), findsOneWidget);
    expect(find.textContaining('Initial release'), findsOneWidget);
    expect(find.text(actualNotes.trim()), findsOneWidget);
    expect(
      find.text('Ready to install. Your board is running 1.0.1-0.'),
      findsOneWidget,
    );
    expect(
      find.text("Don't show new firmware update notes again."),
      findsOneWidget,
    );
    expect(find.textContaining('Version: 1.0.2'), findsNothing);
    expect(
      tester
          .widget<CheckboxListTile>(
            find.byKey(const ValueKey<String>('release-notice-opt-out')),
          )
          .subtitle,
      isNull,
    );
    final primaryRect = tester.getRect(
      find.widgetWithText(FilledButton, 'Review update'),
    );
    final choiceRect = tester.getRect(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    expect(choiceRect.top, greaterThanOrEqualTo(primaryRect.bottom));
    expect(choiceRect.shortestSide, greaterThanOrEqualTo(48));
    expect(primaryRect.height, greaterThanOrEqualTo(48));
    expect(
      tester.widget<AlertDialog>(find.byType(AlertDialog)).backgroundColor?.a,
      closeTo(.96, .001),
    );
    noticeScale.value = TextScaler.noScaling;
    await tester.pumpAndSettle();
    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-portrait-100',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    tester.platformDispatcher.platformBrightnessTestValue = Brightness.dark;
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'firmware-offer-refined-1.0.2-portrait-dark-100',
      boundaryFinder: find.byKey(const ValueKey<String>('connected-gate-render')));
    tester.platformDispatcher.platformBrightnessTestValue = Brightness.light;
    noticeScale.value = const TextScaler.linear(2);
    await tester.pumpAndSettle();

    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-portrait-light-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    tester.platformDispatcher.platformBrightnessTestValue = Brightness.dark;
    await tester.pumpAndSettle();
    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-portrait-dark-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    tester.platformDispatcher.localesTestValue = const [Locale('de')];
    await tester.pumpAndSettle();
    expect(
      find.text('Versionshinweise werden in ihrer Originalsprache angezeigt.'),
      findsOneWidget,
    );
    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-german-dark-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    tester.platformDispatcher.localesTestValue = const [Locale('en')];
    tester.platformDispatcher.platformBrightnessTestValue = Brightness.light;
    await tester.pumpAndSettle();
    tester.view.physicalSize = const Size(844, 390);
    await tester.pumpAndSettle();
    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-landscape-light-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    final footerChoiceBefore = tester.getRect(find.byKey(const ValueKey<String>('release-notice-opt-out')));
    final footerActionBefore = tester.getRect(find.widgetWithText(FilledButton, 'Review update'));
    final notesScroll = find.ancestor(of: find.text('Board firmware update: 1.0.2'),
      matching: find.byType(Scrollable)).first;
    await tester.drag(notesScroll, const Offset(0, -600));
    await tester.pumpAndSettle();
    final notePosition = tester.state<ScrollableState>(notesScroll).position;
    expect(notePosition.pixels, greaterThan(0));
    expect(notePosition.pixels, closeTo(notePosition.maxScrollExtent, .01));
    expect(tester.getRect(find.byKey(const ValueKey<String>('release-notice-opt-out'))), footerChoiceBefore);
    expect(tester.getRect(find.widgetWithText(FilledButton, 'Review update')), footerActionBefore);
    await captureHealthReview(tester, 'firmware-offer-refined-1.0.2-landscape-final-paragraph-200',
      boundaryFinder: find.byKey(const ValueKey<String>('connected-gate-render')));
    tester.platformDispatcher.accessibilityFeaturesTestValue =
        const FakeAccessibilityFeatures(highContrast: true);
    await tester.pumpAndSettle();
    expect(
      tester.widget<AlertDialog>(find.byType(AlertDialog)).backgroundColor?.a,
      1,
    );
    await captureHealthReview(
      tester,
      'firmware-offer-refined-1.0.2-high-contrast-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );
    tester.platformDispatcher.clearAccessibilityFeaturesTestValue();
    tester.view.physicalSize = const Size(390, 844);
    await tester.pumpAndSettle();
    final preferences = await SharedPreferences.getInstance();
    await preferences.setBool('app_update_notices_disabled', true);
    expect(preferences.getStringList('firmware_update_seen_versions'), [
      '1.0.2',
    ]);

    // Simulate process death before either action is tapped. The next shell
    // reloads durable history, including the retained series-0 spelling.
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
    await preferences.setBool('app_update_notices_disabled', false);
    final installedAt = DateTime.utc(2026, 1, 1);
    String? appIdentity = jsonEncode(<String, Object>{
      'schema': 1,
      'platform': 'android',
      'package': 'com.foxieproducts.litchMobile',
      'version': '1.0.2',
      'build': '50',
      'installed': installedAt.millisecondsSinceEpoch,
    });
    final bundledCatalog = await tester.runAsync(
      () => rootBundle.loadString(AppUpgradeNoticeService.notesAsset),
    );
    expect(bundledCatalog, contains('Follow your game on the Play screen'));
    final appRelease = AppUpgradeNoticeService(
      platform: TargetPlatform.android,
      loadReleaseNotes: () async => bundledCatalog!,
      loadPackageInfo: () async => PackageInfo(
        appName: 'Foxie Litch',
        packageName: 'com.foxieproducts.litchMobile',
        version: '1.0.2',
        buildNumber: '51',
        installTime: installedAt,
        updateTime: DateTime.utc(2026, 1, 2),
      ),
      readIdentity: () async => appIdentity,
      writeIdentity: (value) async {
        appIdentity = value;
        return true;
      },
    );
    service.emitAvailability(BleAvailability.ready);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          appUpgradeNoticeService: appRelease,
          firmwarePageBuilder: (_, _, _, _, changed) {
            publishAvailability = changed;
            return const Text('Firmware content');
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
    final appNotice = find.byKey(const ValueKey<String>('app-upgrade-notice'));
    expect(service.productReady, isTrue);
    expect(
      appNotice,
      findsOneWidget,
      reason:
          'App notes use the actual ready paired shell, not a separate route.',
    );
    expect(prompt, findsNothing);
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Continue'));
    await tester.pumpAndSettle();
    expect(appNotice, findsNothing);
    expect(preferences.getBool('app_update_notices_disabled'), isTrue);
    publishAvailability(offer('1.0.2-0', installed: '1.0.1-0'));
    await tester.pumpAndSettle();
    expect(prompt, findsNothing);

    // An incoming offer waits for the active pointer, then Later records it.
    final pointer = await tester.startGesture(
      tester.getCenter(
        find.byKey(const ValueKey('connected-board-status-action')),
      ),
    );
    publishAvailability(offer('1.0.3-1', installed: '0.7'));
    await tester.pump();
    expect(prompt, findsNothing);
    await pointer.cancel();
    await tester.pumpAndSettle();
    expect(prompt, findsOneWidget);
    await tester.tap(find.text('Later'));
    await tester.pumpAndSettle();
    publishAvailability(offer('1.0.3-1', release: FirmwareReleaseInfo.beta));
    await tester.pumpAndSettle();
    expect(
      prompt,
      findsNothing,
      reason: 'Channel promotion does not create a new version.',
    );

    publishAvailability(offer('1.0.3-2'));
    await tester.pumpAndSettle();
    publishAvailability(
      FirmwareUpdateAvailability(
        loading: true,
        boardId: 'board-1',
        connectionToken: service.connectionSessionToken,
      ),
    );
    publishAvailability(offer('1.0.3-2'));
    await tester.pumpAndSettle();
    expect(prompt, findsOneWidget);
    await tester.tap(find.text('Review update'));
    await tester.pumpAndSettle();
    expect(find.text('Board update'), findsOneWidget);
    expect(find.text('Firmware content'), findsOneWidget);
    expect(prompt, findsNothing);
    publishAvailability(offer('1.0.3-3'));
    await tester.pumpAndSettle();
    expect(
      prompt,
      findsNothing,
      reason: 'The visible Update offer counts as seen.',
    );
    expect(
      preferences.getStringList('firmware_update_seen_versions'),
      contains('1.0.3-3'),
    );
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    publishAvailability(offer('1.0.3-3', release: FirmwareReleaseInfo.beta));
    await tester.pumpAndSettle();
    expect(prompt, findsNothing);

    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    publishAvailability(offer('1.0.3-4'));
    await tester.pumpAndSettle();
    expect(prompt, findsNothing);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pumpAndSettle();
    expect(prompt, findsOneWidget);
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Later'));
    await tester.pumpAndSettle();
    expect(preferences.getBool('firmware_update_notices_disabled'), isTrue);
    expect(preferences.getBool('app_update_notices_disabled'), isTrue);
    publishAvailability(offer('1.0.3-5'));
    await tester.pumpAndSettle();
    expect(
      prompt,
      findsNothing,
      reason: 'The board opt-out applies to future versions.',
    );
    final oldToken = service.connectionSessionToken;
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    expect(appNotice, findsNothing);
    await tester.pump(const Duration(milliseconds: 300));
    expect(prompt, findsNothing);
    publishAvailability(offer('1.0.3-5', boardId: 'board-1', token: oldToken));
    await tester.pump();
    expect(
      prompt,
      findsNothing,
      reason: 'A late retired-link result cannot prompt.',
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'Connected Sleep and Disconnect stay ordered and recoverable at 200% text',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      _usePortraitPhone(tester);
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await _pumpConnectedGate(
        tester,
        service,
        textScaler: TextScaler.linear(2),
      );
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      final sleep = find.byKey(const ValueKey('board-menu-sleep'));
      final disconnect = find.byKey(const ValueKey('board-menu-disconnect'));
      await tester.ensureVisible(sleep);
      await tester.pumpAndSettle();
      final sleepRect = tester.getRect(sleep);
      final disconnectRect = tester.getRect(disconnect);
      expect(disconnectRect.top, closeTo(sleepRect.top, .1));
      expect(disconnectRect.bottom, closeTo(sleepRect.bottom, .1));
      expect(disconnectRect.right, lessThanOrEqualTo(sleepRect.left));
      expect(sleepRect.height, greaterThanOrEqualTo(48));
      expect(disconnectRect.height, greaterThanOrEqualTo(48));
      expect(
        _connectionMenuContentBounds(tester, sleep).right,
        closeTo(sleepRect.right, .1),
        reason: 'Sleep aligns with the trailing edge of the menu controls.',
      );
      _expectSleepTextThenIcon(tester, sleep);
      await captureHealthReview(
        tester,
        'connected-sleep-200',
        boundaryFinder: find.byKey(
          const ValueKey<String>('connected-gate-render'),
        ),
      );
      for (final action in [sleep, disconnect]) {
        expect(tester.getSize(action).shortestSide, greaterThanOrEqualTo(48));
        expect(action.hitTestable(), findsOneWidget);
      }
      expect(find.byIcon(Icons.bedtime_outlined), findsOneWidget);
      expect(find.byIcon(Icons.link_off_rounded), findsOneWidget);
      service.sleepResponse = Completer<ConnectionSessionOutcome?>();
      await tester.tap(sleep);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(service.sleepRequests, 1);
      expect(tester.getRect(sleep), sleepRect);
      expect(
        find.descendant(
          of: sleep,
          matching: find.byType(CircularProgressIndicator),
        ),
        findsOneWidget,
      );
      expect(tester.widget<TextButton>(sleep).onPressed, isNull);
      expect(tester.widget<TextButton>(disconnect).onPressed, isNull);
      expect(find.text('Sleeping…'), findsOneWidget);
      expect(
        tester
            .widget<ListTile>(find.byKey(const ValueKey('board-menu-name')))
            .onTap,
        isNull,
      );
      await captureHealthReview(
        tester,
        'connected-sleep-pending-200',
        boundaryFinder: find.byKey(
          const ValueKey<String>('connected-gate-render'),
        ),
      );
      service.sleepResponse!.complete(
        const ConnectionSessionOutcome(
          sequence: 1,
          disposition: ConnectionSessionDisposition.rejected,
          reason: 9,
          domainRevision: 0,
          domainDigest: 0,
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('board-menu-sleep-error')),
        findsOneWidget,
      );
      expect(tester.widget<TextButton>(sleep).onPressed, isNotNull);
      expect(tester.widget<TextButton>(disconnect).onPressed, isNotNull);
      expect(sleep.hitTestable(), findsOneWidget);
      expect(disconnect.hitTestable(), findsOneWidget);
      await captureHealthReview(
        tester,
        'connected-sleep-rejected-200',
        boundaryFinder: find.byKey(
          const ValueKey<String>('connected-gate-render'),
        ),
      );
      service.sleepResponse = Completer<ConnectionSessionOutcome?>();
      // Recovery stays visible after rejection, with no user scroll.
      await tester.tap(sleep);
      await tester.pump();
      expect(service.sleepRequests, 2);
      expect(tester.widget<TextButton>(sleep).onPressed, isNull);
      unawaited(
        showDialog<void>(
          context: tester.element(sleep),
          builder: (_) => const AlertDialog(title: Text('Covering dialog')),
        ),
      );
      await tester.pump();
      service.sleepResponse!.complete(
        const ConnectionSessionOutcome(
          sequence: 2,
          disposition: ConnectionSessionDisposition.applied,
          reason: 0,
          domainRevision: 0,
          domainDigest: 0,
        ),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('connected-board-popover')),
        findsOneWidget,
      );
      expect(find.text('Sleeping…'), findsOneWidget);
      expect(tester.widget<TextButton>(sleep).onPressed, isNull);
      await tester.pump(const Duration(seconds: 6));
      await tester.pump();
      expect(tester.widget<TextButton>(sleep).onPressed, isNotNull);
      expect(tester.widget<TextButton>(disconnect).onPressed, isNotNull);
      expect(
        find.text(
          lookupAppLocalizations(const Locale('en')).actionUnconfirmed('Sleep'),
        ),
        findsOneWidget,
      );
      Navigator.of(tester.element(find.byType(AlertDialog))).pop();
      await tester.pumpAndSettle();
      await tester.dragUntilVisible(
        sleep,
        find.byKey(const ValueKey<String>('board-menu-scroll')),
        const Offset(0, -100),
      );
      await tester.pumpAndSettle();
      expect(sleep.hitTestable(), findsOneWidget);
      expect(disconnect.hitTestable(), findsOneWidget);
      service.sleepResponse = Completer<ConnectionSessionOutcome?>();
      await tester.tap(sleep);
      await tester.pump();
      service.sleepResponse!.complete(
        const ConnectionSessionOutcome(
          sequence: 3,
          disposition: ConnectionSessionDisposition.applied,
          reason: 0,
          domainRevision: 0,
          domainDigest: 0,
        ),
      );
      await tester.pump();
      expect(find.text('Sleeping…'), findsOneWidget);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump(const Duration(milliseconds: 16));
      final staleBoard = find.byKey(const ValueKey('discovered-board-board-1'));
      final menu = find.byKey(const ValueKey('connected-board-popover'));
      final discoveryStatus = find.byKey(
        const ValueKey<String>('connection-status-live-region'),
      );
      final waiting = tester
          .widget<Semantics>(discoveryStatus)
          .properties
          .label;
      expect(waiting, contains('Waiting for previous board'));
      expect(staleBoard, findsNothing);
      expect(menu, findsNothing);
      await captureHealthReview(tester, 'sleep-first-offline-frame');
      service.emitBoards(const []);
      await tester.pump(const Duration(milliseconds: 16));
      expect(staleBoard, findsNothing);
      expect(
        tester.widget<Semantics>(discoveryStatus).properties.label,
        waiting,
      );
      await captureHealthReview(tester, 'sleep-first-scan-frame');
      expect(service.disconnectCalls, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('Late Sleep acknowledgement cannot close a replacement route', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    _usePortraitPhone(tester);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final sleep = find.byKey(const ValueKey('board-menu-sleep'));
    await tester.ensureVisible(sleep);
    await tester.pumpAndSettle();
    service.sleepResponse = Completer<ConnectionSessionOutcome?>();
    await tester.tap(sleep);
    await tester.pump();
    expect(service.sleepRequests, 1);
    final popupElement = tester.element(sleep);
    final navigator = Navigator.of(popupElement);
    navigator.pop();
    final replacement = MaterialPageRoute<void>(
      builder: (_) => const Scaffold(body: Text('Replacement route')),
    );
    unawaited(navigator.push(replacement));
    await tester.pump(const Duration(milliseconds: 16));
    expect(
      popupElement.mounted,
      isTrue,
      reason: 'the old popup stays mounted during its reverse animation',
    );
    expect(replacement.isCurrent, isTrue);
    service.sleepResponse!.complete(
      const ConnectionSessionOutcome(
        sequence: 1,
        disposition: ConnectionSessionDisposition.applied,
        reason: 0,
        domainRevision: 0,
        domainDigest: 0,
      ),
    );
    await tester.pump();
    expect(
      replacement.isCurrent,
      isTrue,
      reason: 'a retired popup must not pop the new current route',
    );
    await tester.pumpAndSettle();
    expect(find.text('Replacement route'), findsOneWidget);
    expect(service.disconnectCalls, 0);
    expect(tester.takeException(), isNull);
  });

  testWidgets('Connected menu follows board settings while it stays open', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();

    service.reportBoardSetting('BOARD_ROTATE', true);
    service.reportBoardSetting('AUTO_BRIGHTNESS', false);
    service.reportBoardSetting('BRIGHTNESS', 7);
    await tester.pumpAndSettle();

    expect(
      tester
          .widget<SwitchListTile>(
            find.byKey(const ValueKey('board-menu-rotate')),
          )
          .value,
      isTrue,
    );
    expect(
      tester
          .widget<SwitchListTile>(
            find.byKey(const ValueKey('board-menu-auto-brightness')),
          )
          .value,
      isFalse,
    );
    expect(
      tester
          .widget<Slider>(
            find.descendant(
              of: find.byKey(const ValueKey('board-menu-brightness')),
              matching: find.byType(Slider),
            ),
          )
          .value,
      7,
    );
    expect(service.settingsReadCalls, 0);
  });

  testWidgets('Connected menu shows the paired board Bluetooth suffix', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final board = SettingsSessionBoard(includeHomeSnapshot: true)
      ..capabilityMask =
          LitchDeviceCapabilities.settingsBit |
          LitchDeviceCapabilities.ownerPairingBit
      ..state = 6
      ..flags = 5
      ..supportsBluetoothSuffix = true;
    final service = _PagedSettingsBleService(board: board);
    addTearDown(() => tester.runAsync(service.dispose));
    await tester.pumpWidget(
      MaterialApp(
        home: ConnectionGateScreen(
          bleService: service,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pump();
    service.emitBoards(const [
      LitchBoard(id: 'board-1', name: 'Studio', rssi: -40),
      LitchBoard(id: 'board-2', name: 'Other', rssi: -50),
    ]);
    await tester.pump();
    await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
    service.completeNextConnect(true);
    for (
      var frame = 0;
      frame < 30 && service.ownerPairingStatus == null;
      ++frame
    ) {
      await tester.pump();
    }
    expect(
      service.ownerPairingStatus?.state,
      OwnerPairingSessionState.ownedOther,
    );
    board.setState(5, nextFlags: 7);
    await tester.runAsync(() async {
      for (var turn = 0; turn < 20; ++turn) {
        await Future<void>.delayed(Duration.zero);
      }
    });
    await tester.pumpAndSettle();
    expect(service.productReady, isTrue);
    await tester.tap(
      find.byKey(const ValueKey('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    expect(find.text('Litch ID: 3F81'), findsOneWidget);
    expect(find.textContaining('0F10'), findsNothing);
    expect(find.textContaining('BAD1'), findsNothing);
    expect(board.bluetoothSuffixQueries, 1);
    await tester.pumpWidget(const SizedBox());
    await tester.runAsync(service.dispose);
    await tester.pump();
  });

  testWidgets(
    'Connected menu omits unavailable product identity and saves a friendly name',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);

      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Connected'), findsWidgets);
      expect(find.textContaining('Litch ID:'), findsNothing);
      expect(find.text('Foxie_Litch'), findsNothing);

      await tester.tap(find.byKey(const ValueKey('board-menu-name')));
      await tester.pumpAndSettle();
      final field = find.byKey(const ValueKey('board-friendly-name-field'));
      expect(tester.widget<TextFormField>(field).initialValue, 'My Litch');
      expect(
        find.byKey(const ValueKey('board-friendly-name-reset')),
        findsOneWidget,
        reason:
            'Restoring the default board name must always remain reachable.',
      );
      await tester.enterText(field, 'Studio Litch');
      await tester.tap(find.byKey(const ValueKey('board-friendly-name-save')));
      await tester.pumpAndSettle();
      expect(find.text('Studio Litch'), findsOneWidget);
      expect(find.text('Foxie One'), findsNothing);
      expect(find.textContaining('Litch ID:'), findsNothing);

      final preferences = await SharedPreferences.getInstance();
      expect(preferences.getString('board_friendly_name:board-1'), isNull);
      expect(service.boardValues['BLE_DEVICE_NAME'], 'Studio Litch');
      expect(
        service.settingWrites,
        contains((
          key: 'BLE_DEVICE_NAME',
          value: 'Studio Litch',
          awaitAck: true,
        )),
      );

      await tester.tap(find.byKey(const ValueKey('board-menu-name')));
      await tester.pumpAndSettle();
      await tester.enterText(field, 'Hidden\u202eName');
      await tester.tap(find.byKey(const ValueKey('board-friendly-name-save')));
      await tester.pump();
      expect(find.text('Use 1–20 visible characters.'), findsOneWidget);
      expect(
        preferences.getString('board_friendly_name:board-1'),
        isNull,
        reason: 'invalid text never creates a local-only board name',
      );
      expect(service.boardValues['BLE_DEVICE_NAME'], 'Studio Litch');
    },
  );

  testWidgets(
    'reset name immediately replaces a stale custom advertisement with identity',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      service.boardValues['BLE_DEVICE_NAME'] = 'Studio Litch';
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);

      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Studio Litch'), findsOneWidget);
      expect(find.text('Foxie One'), findsNothing);
      await tester.tap(find.byKey(const ValueKey('board-menu-name')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('board-friendly-name-reset')));
      await tester.pumpAndSettle();

      expect(service.boardValues['BLE_DEVICE_NAME'], 'Foxie Litch');
      expect(find.text('Studio Litch'), findsNothing);
      expect(find.text('Foxie One'), findsNothing);
      expect(find.text('Connected'), findsWidgets);
      expect(find.textContaining('Litch ID:'), findsNothing);
    },
  );

  testWidgets(
    'renamed board reconnects by immutable id and refreshes its visible name',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);

      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('board-menu-name')));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const ValueKey('board-friendly-name-field')),
        'Studio Board',
      );
      await tester.tap(find.byKey(const ValueKey('board-friendly-name-save')));
      await tester.pumpAndSettle();
      expect(service.boardValues['BLE_DEVICE_NAME'], 'Studio Board');

      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('connection-gate-content')),
        findsOneWidget,
      );
      expect(find.byType(HomeScreen), findsNothing);
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-2', name: 'Studio Board', rssi: -20),
        LitchBoard(id: 'board-1', name: 'Studio Board', rssi: -45),
      ]);
      await tester.pump(Duration.zero);
      expect(
        service.connectCalls,
        <String>['board-1', 'board-1'],
        reason:
            'The remembered transport id, not the mutable advertised name, owns reconnect.',
      );
      service.completeNextConnect(true);
      await tester.pumpAndSettle();

      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Studio Board'), findsOneWidget);
      expect(find.text('Foxie One'), findsNothing);
      expect(find.textContaining('Litch ID:'), findsNothing);
    },
  );

  testWidgets('Connected rename survives keyboard-only metrics changes', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    addTearDown(tester.view.resetViewInsets);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('board-menu-name')));
    await tester.pumpAndSettle();

    tester.view.viewInsets = const FakeViewPadding(bottom: 300);
    await tester.pumpAndSettle();

    expect(
      find.byKey(const ValueKey('board-friendly-name-field')),
      findsOneWidget,
      reason: 'keyboard appearance must not dismiss the rename journey',
    );
    expect(
      find.byKey(const ValueKey('connected-board-popover')),
      findsOneWidget,
    );
  });

  testWidgets('Connected rename reconciles an applied write across restart', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()
      ..loseNextAcknowledgedWriteAfterApply = true;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('board-menu-name')));
    await tester.pumpAndSettle();
    final field = find.byKey(const ValueKey('board-friendly-name-field'));
    await tester.enterText(field, "Club's Litch");
    await tester.tap(find.byKey(const ValueKey('board-friendly-name-save')));
    await tester.pumpAndSettle();

    expect(service.boardValues['BLE_DEVICE_NAME'], "Club's Litch");
    expect(find.text("Club's Litch"), findsOneWidget);
    expect(
      find.byKey(const ValueKey('board-menu-inline-error')),
      findsNothing,
      reason: 'restart-before-ACK reconciles against the board-owned readback',
    );
    final preferences = await SharedPreferences.getInstance();
    expect(preferences.getString('board_friendly_name:board-1'), isNull);
  });

  for (final textScale in <double>[1, 2]) {
    testWidgets('Connected brightness retains a held drag at $textScale text', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      _usePortraitPhone(tester);
      await _pumpConnectedGate(
        tester,
        service,
        textScaler: TextScaler.linear(textScale),
      );
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();

      final Finder slider = find.descendant(
        of: find.byKey(const ValueKey('board-menu-brightness')),
        matching: find.byType(Slider),
      );
      expect(
        tester.widget<Slider>(slider).onChanged,
        isNotNull,
        reason: 'Automatic brightness keeps the user-selected maximum editable',
      );
      final SemanticsNode brightnessSemantics = tester.getSemantics(slider);
      expect(brightnessSemantics.value, contains('Maximum brightness'));
      expect(brightnessSemantics.value, contains('5'));
      final Rect track = tester.getRect(slider);
      final brightnessIcon = tester.getRect(
        find.byIcon(Icons.brightness_6_outlined),
      );
      final boardIcon = tester.getRect(
        find.byIcon(Icons.developer_board_outlined),
      );
      final editIcon = tester.getRect(
        find.descendant(
          of: find.byKey(const ValueKey('board-menu-name')),
          matching: find.byIcon(Icons.edit_outlined),
        ),
      );
      final brightnessValue = tester.getRect(
        find.descendant(
          of: find.byKey(const ValueKey('board-menu-brightness')),
          matching: find.text('5'),
        ),
      );
      await captureHealthReview(
        tester,
        'connected-brightness-portrait-$textScale',
      );
      expect(brightnessIcon.left, boardIcon.left);
      expect(track.left, brightnessIcon.left);
      expect(track.right, editIcon.right);
      expect(brightnessValue.right, editIcon.right);
      expect(tester.widget<Slider>(slider).padding, EdgeInsets.zero);
      final TestGesture gesture = await tester.startGesture(
        Offset(track.left + 30, track.center.dy),
      );
      for (final double fraction in <double>[0.35, 0.7, 0.9]) {
        await gesture.moveTo(
          Offset(track.left + track.width * fraction, track.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 40));
        final heldValue = tester.widget<Slider>(slider).value;
        final heldRect = tester.getRect(slider);
        service.reportBoardSetting('BRIGHTNESS', 2);
        service.reportBoardSetting('AUTO_BRIGHTNESS', fraction != 0.7);
        service.reportHealth(stateCode: 1, causeMask: 0x10);
        await tester.pump(const Duration(milliseconds: 16));
        expect(tester.widget<Slider>(slider).value, heldValue);
        expect(tester.getRect(slider), heldRect);
        expect(find.text('Maximum brightness'), findsOneWidget);
        expect(find.byKey(const ValueKey('board-menu-health')), findsNothing);
        expect(service.settingWrites.where((write) => write.awaitAck), isEmpty);
      }
      final int preview = tester.widget<Slider>(slider).value.round();
      expect(preview, greaterThan(5));
      expect(
        service.settingWrites.where((write) => write.key == 'BRIGHTNESS'),
        isNotEmpty,
      );
      if (textScale == 2) {
        await gesture.cancel();
      } else {
        await gesture.up();
      }
      await tester.pumpAndSettle();
      final brightnessWrites = service.settingWrites
          .where((write) => write.key == 'BRIGHTNESS')
          .toList();
      expect(brightnessWrites.last.value, preview);
      expect(brightnessWrites.last.awaitAck, isTrue);
      expect(brightnessWrites.where((write) => write.awaitAck), hasLength(1));
      expect(tester.getSize(slider).height, greaterThanOrEqualTo(48));
      expect(tester.takeException(), isNull);
      expect(find.byKey(const ValueKey('board-menu-health')), findsOneWidget);
    });
  }

  testWidgets('Connected Auto keeps the same maximum across mode changes', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final slider = find.descendant(
      of: find.byKey(const ValueKey('board-menu-brightness')),
      matching: find.byType(Slider),
    );
    final automatic = find.byKey(const ValueKey('board-menu-auto-brightness'));
    expect(find.text('Maximum brightness'), findsOneWidget);
    expect(tester.widget<SwitchListTile>(automatic).subtitle, isNull);
    await tester.ensureVisible(automatic);
    await tester.tap(automatic);
    await tester.pumpAndSettle();
    expect(find.text('Maximum brightness'), findsNothing);
    expect(find.text('Brightness'), findsOneWidget);
    expect(tester.widget<Slider>(slider).value, 5);
    expect(tester.getSemantics(slider).value, contains('Brightness'));
    await tester.tap(automatic);
    await tester.pumpAndSettle();
    expect(find.text('Maximum brightness'), findsOneWidget);
    expect(tester.widget<Slider>(slider).value, 5);
    expect(service.boardValues['BRIGHTNESS'], 5);
    expect(service.settingWrites.map((write) => write.key), <String>[
      'AUTO_BRIGHTNESS',
      'AUTO_BRIGHTNESS',
    ]);
    expect(tester.takeException(), isNull);
  });

  testWidgets('Connected brightness reaches and names level zero', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();

    final Finder brightness = find.byKey(
      const ValueKey('board-menu-brightness'),
    );
    final Finder slider = find.descendant(
      of: brightness,
      matching: find.byType(Slider),
    );
    expect(tester.widget<Slider>(slider).min, 0);
    expect(tester.widget<Slider>(slider).max, 8);
    expect(service.settingsReadCalls, 0);
    final Rect track = tester.getRect(slider);
    final TestGesture gesture = await tester.startGesture(
      Offset(track.left + track.width * 0.6, track.center.dy),
    );
    for (final double fraction in <double>[0.45, 0.2, 0.01]) {
      await gesture.moveTo(
        Offset(track.left + track.width * fraction, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
    }
    expect(tester.widget<Slider>(slider).value, 0);
    expect(
      find.descendant(of: brightness, matching: find.text('0 · Near dark')),
      findsOneWidget,
    );
    expect(tester.getSemantics(slider).value, contains('0 · Near dark'));
    expect(tester.widget<Slider>(slider).label, '0 · Near dark');
    final automatic = find.byKey(const ValueKey('board-menu-auto-brightness'));
    expect(tester.widget<SwitchListTile>(automatic).onChanged, isNull);
    expect(tester.widget<SwitchListTile>(automatic).value, isTrue);
    expect(find.text('Disabled at level 0'), findsOneWidget);
    expect(
      tester
          .getSemantics(automatic)
          .getSemanticsData()
          .flagsCollection
          .isEnabled,
      ui.Tristate.isFalse,
    );
    await gesture.up();
    await tester.pumpAndSettle();
    expect(service.boardValues['BRIGHTNESS'], 0);
    expect(service.settingWrites.last.value, 0);
    expect(service.settingWrites.last.awaitAck, isTrue);
    service.reportBoardSetting('BRIGHTNESS', 1);
    await tester.pumpAndSettle();
    expect(tester.widget<SwitchListTile>(automatic).onChanged, isNotNull);
    expect(tester.widget<SwitchListTile>(automatic).value, isTrue);
    expect(tester.widget<SwitchListTile>(automatic).subtitle, isNull);
    expect(service.boardValues['AUTO_BRIGHTNESS'], isTrue);
    expect(
      service.settingWrites.where((write) => write.key == 'AUTO_BRIGHTNESS'),
      isEmpty,
    );
  });

  testWidgets('Connected brightness honors available board limits', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()
      ..advertisedBrightnessMin = 2
      ..advertisedBrightnessMax = 6;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final Slider slider = tester.widget<Slider>(
      find.descendant(
        of: find.byKey(const ValueKey('board-menu-brightness')),
        matching: find.byType(Slider),
      ),
    );
    expect(slider.min, 2);
    expect(slider.max, 6);
    expect(slider.divisions, 4);
    expect(service.settingsReadCalls, 0);
  });

  testWidgets(
    'Connected brightness waits for the requested level after an intermediate board report',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService()
        ..applyPreviewWrites = false
        ..usesExplicitBoardStateReports = true;
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();

      final Finder slider = find.descendant(
        of: find.byKey(const ValueKey('board-menu-brightness')),
        matching: find.byType(Slider),
      );
      final Rect track = tester.getRect(slider);
      final TestGesture gesture = await tester.startGesture(
        Offset(track.left + track.width * 4 / 7, track.center.dy),
      );
      await tester.pump();
      for (final double fraction in <double>[0.7, 0.85, 0.95]) {
        await gesture.moveTo(
          Offset(track.left + track.width * fraction, track.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 40));
        final double preview = tester.widget<Slider>(slider).value;
        expect(preview, greaterThan(5));

        service.reportBoardSetting('BRIGHTNESS', 5);
        await tester.pump();
        expect(
          tester.widget<Slider>(slider).value,
          preview,
          reason: 'a delayed settings update must not move the held thumb',
        );
      }

      final double committed = tester.widget<Slider>(slider).value;
      final confirmation = Completer<bool>();
      service.nextBoardStateConfirmation = confirmation;
      await gesture.up();
      await tester.pump();
      expect(tester.widget<Slider>(slider).value, committed);
      expect(tester.widget<Slider>(slider).onChanged, isNull);

      service.reportBoardSetting('BRIGHTNESS', 5);
      await tester.pump();
      expect(
        tester.widget<Slider>(slider).value,
        committed,
        reason: 'the pending final ACK still owns the displayed choice',
      );
      confirmation.complete(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      expect(service.boardValues['BRIGHTNESS'], 5);
      expect(
        tester.widget<Slider>(slider).value,
        committed,
        reason: 'a receipt with the old board value must not undo the preview',
      );
      service.reportBoardSetting('BRIGHTNESS', 5);
      await tester.pump();
      expect(tester.widget<Slider>(slider).value, committed);
      service.reportBoardSetting('BRIGHTNESS', 7);
      await tester.pump(const Duration(milliseconds: 500));
      expect(tester.widget<Slider>(slider).value, committed);
      service.reportBoardSetting('BRIGHTNESS', 7);
      await tester.pump();
      expect(tester.widget<Slider>(slider).value, committed);
      service.reportBoardSetting('BRIGHTNESS', committed.round());
      await tester.pumpAndSettle();
      expect(tester.widget<Slider>(slider).value, committed);
      expect(tester.widget<Slider>(slider).onChanged, isNotNull);
      final int acceptedRevision = service.boardSettingsRevision;
      service.reportBoardSetting(
        'BRIGHTNESS',
        5,
        revision: acceptedRevision - 1,
      );
      await tester.pump();
      expect(tester.widget<Slider>(slider).value, committed);
      expect(service.boardValues['BRIGHTNESS'], committed.round());
      expect(service.boardSettingsRevision, acceptedRevision);
      service.reportBoardSetting(
        'BRIGHTNESS',
        6,
        revision: acceptedRevision + 1,
      );
      await tester.pumpAndSettle();
      expect(tester.widget<Slider>(slider).value, 6);
      expect(
        find.byKey(const ValueKey('board-menu-inline-error')),
        findsNothing,
      );
    },
  );

  testWidgets('Connected brightness times out at an intermediate board level', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()
      ..applyPreviewWrites = false
      ..usesExplicitBoardStateReports = true;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();

    final Finder slider = find.descendant(
      of: find.byKey(const ValueKey('board-menu-brightness')),
      matching: find.byType(Slider),
    );
    final confirmation = Completer<bool>();
    service.nextBoardStateConfirmation = confirmation;
    final Rect track = tester.getRect(slider);
    final TestGesture gesture = await tester.startGesture(
      Offset(track.left + track.width * 4 / 7, track.center.dy),
    );
    for (final double fraction in <double>[0.7, 0.85, 0.95]) {
      await gesture.moveTo(
        Offset(track.left + track.width * fraction, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
    }
    await gesture.up();
    await tester.pump();
    expect(tester.widget<Slider>(slider).value, 8);

    service.reportBoardSetting('BRIGHTNESS', 7);
    await tester.pump();
    expect(tester.widget<Slider>(slider).value, 8);
    confirmation.complete(true);
    await tester.pump();
    for (int i = 0; i < 3; i++) {
      service.reportBoardSetting('BRIGHTNESS', 7);
      await tester.pump(const Duration(milliseconds: 500));
      expect(tester.widget<Slider>(slider).value, 8);
    }
    service.reportBoardSetting('BRIGHTNESS', 7);
    await tester.pump(const Duration(milliseconds: 600));
    expect(service.boardValues['BRIGHTNESS'], 7);
    expect(tester.widget<Slider>(slider).value, 7);
    expect(
      find.byKey(const ValueKey('board-menu-inline-error')),
      findsOneWidget,
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey('board-menu-inline-error')))
          .label,
      contains('Brightness'),
    );

    service.reportBoardSetting('BRIGHTNESS', 6);
    await tester.pump();
    expect(tester.widget<Slider>(slider).value, 6);
    expect(
      find.byKey(const ValueKey('board-menu-inline-error')),
      findsOneWidget,
    );
    service.reportBoardSetting('BRIGHTNESS', 8);
    await tester.pump();
    expect(tester.widget<Slider>(slider).value, 8);
    expect(find.byKey(const ValueKey('board-menu-inline-error')), findsNothing);
  });

  testWidgets(
    'Connected brightness reconciles failed commits to board authority',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      final Finder slider = find.descendant(
        of: find.byKey(const ValueKey('board-menu-brightness')),
        matching: find.byType(Slider),
      );

      Rect track = tester.getRect(slider);
      TestGesture gesture = await tester.startGesture(
        Offset(track.left + track.width * 4 / 7, track.center.dy),
      );
      await gesture.moveTo(
        Offset(track.left + track.width * 0.7, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
      await gesture.up();
      await tester.pumpAndSettle();
      final double accepted = tester.widget<Slider>(slider).value;
      expect(accepted, greaterThan(5));

      service.applyPreviewWrites = false;
      service.rejectNextAcknowledgedWrite = true;
      track = tester.getRect(slider);
      gesture = await tester.startGesture(
        Offset(track.left + track.width * 6 / 7, track.center.dy),
      );
      await gesture.moveTo(Offset(track.right - 4, track.center.dy));
      await tester.pump(const Duration(milliseconds: 40));
      await gesture.up();
      await tester.pumpAndSettle();
      expect(tester.widget<Slider>(slider).value, accepted);
      expect(
        find.byKey(const ValueKey('board-menu-inline-error')),
        findsOneWidget,
      );
      final SemanticsNode failure = tester.getSemantics(
        find.byKey(const ValueKey('board-menu-inline-error')),
      );
      expect(failure, isSemantics(isLiveRegion: true));
      expect(failure.label, contains('Brightness'));

      service.applyPreviewWrites = true;
      service.rejectNextAcknowledgedWrite = true;
      track = tester.getRect(slider);
      gesture = await tester.startGesture(
        Offset(track.left + track.width * 6 / 7, track.center.dy),
      );
      await gesture.moveTo(Offset(track.left + 20, track.center.dy));
      await tester.pump(const Duration(milliseconds: 40));
      await gesture.up();
      await tester.pumpAndSettle();
      expect(
        tester.widget<Slider>(slider).value,
        lessThan(accepted),
        reason:
            'a lost final ACK is recovered from the accepted preview readback',
      );
      expect(
        find.byKey(const ValueKey('board-menu-inline-error')),
        findsNothing,
      );
    },
  );

  testWidgets('Connected opening never fabricates an action failure', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()
      ..reportedSettingsAuthority = LitchSettingsReadAuthority.unavailable;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();

    expect(find.byKey(const ValueKey('board-menu-brightness')), findsOneWidget);
    expect(find.byKey(const ValueKey('board-menu-inline-error')), findsNothing);
    expect(find.text('Try again'), findsNothing);
    expect(service.settingsReadCalls, 0);
  });

  testWidgets('Connected awaits board-state confirmation for brightness', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()..applyPreviewWrites = false;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final Finder slider = find.descendant(
      of: find.byKey(const ValueKey('board-menu-brightness')),
      matching: find.byType(Slider),
    );
    final confirmation = Completer<bool>();
    service.nextBoardStateConfirmation = confirmation;
    final Rect track = tester.getRect(slider);
    final TestGesture gesture = await tester.startGesture(
      Offset(track.left + track.width * 4 / 7, track.center.dy),
    );
    await gesture.moveTo(Offset(track.right - 4, track.center.dy));
    await tester.pump(const Duration(milliseconds: 40));
    await gesture.up();
    await tester.pump();
    expect(tester.widget<Slider>(slider).onChanged, isNull);
    expect(service.boardValues['BRIGHTNESS'], 5);

    confirmation.complete(true);
    await tester.pumpAndSettle();
    expect(tester.widget<Slider>(slider).value, 8);
    expect(service.boardValues['BRIGHTNESS'], 8);
  });

  testWidgets('Connected keeps failed board-state confirmation unconfirmed', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService()..applyPreviewWrites = false;
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final Finder slider = find.descendant(
      of: find.byKey(const ValueKey('board-menu-brightness')),
      matching: find.byType(Slider),
    );
    service.nextBoardStateConfirmation = Completer<bool>()..complete(false);
    final Rect track = tester.getRect(slider);
    final TestGesture gesture = await tester.startGesture(
      Offset(track.left + track.width * 4 / 7, track.center.dy),
    );
    await gesture.moveTo(Offset(track.right - 4, track.center.dy));
    await tester.pump(const Duration(milliseconds: 40));
    await gesture.up();
    await tester.pumpAndSettle();

    expect(tester.widget<Slider>(slider).value, 8);
    expect(service.boardValues['BRIGHTNESS'], 5);
    expect(
      find.byKey(const ValueKey('board-menu-inline-error')),
      findsOneWidget,
    );
  });

  testWidgets(
    'Connected switches reconcile opposite board reports after failure',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService()
        ..usesExplicitBoardStateReports = true;
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();

      for (final (key, initial) in <(String, bool)>[
        ('AUTO_BRIGHTNESS', true),
        ('BOARD_ROTATE', false),
      ]) {
        final Finder switchTile = find.byKey(
          ValueKey(
            key == 'AUTO_BRIGHTNESS'
                ? 'board-menu-auto-brightness'
                : 'board-menu-rotate',
          ),
        );
        final confirmation = Completer<bool>();
        service.nextBoardStateConfirmation = confirmation;
        await tester.tap(switchTile);
        await tester.pump();
        expect(tester.widget<SwitchListTile>(switchTile).value, !initial);
        service.reportBoardSetting(key, initial);
        await tester.pump();
        confirmation.complete(false);
        await tester.pumpAndSettle();
        expect(tester.widget<SwitchListTile>(switchTile).value, initial);
        expect(
          find.byKey(const ValueKey('board-menu-inline-error')),
          findsOneWidget,
        );
      }
    },
  );

  testWidgets(
    'Connected reconciles applied lost ACKs for switches and rotate',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService()
        ..applyRejectedAcknowledgedWrite = true;
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();

      service.rejectNextAcknowledgedWrite = true;
      await tester.tap(
        find.byKey(const ValueKey('board-menu-auto-brightness')),
      );
      await tester.pumpAndSettle();
      expect(service.boardValues['AUTO_BRIGHTNESS'], isFalse);
      expect(
        tester
            .widget<SwitchListTile>(
              find.byKey(const ValueKey('board-menu-auto-brightness')),
            )
            .value,
        isFalse,
      );

      service.rejectNextAcknowledgedWrite = true;
      await tester.tap(find.byKey(const ValueKey('board-menu-rotate')));
      await tester.pumpAndSettle();
      expect(service.boardValues['BOARD_ROTATE'], isTrue);
      expect(
        find.byKey(const ValueKey('board-menu-inline-error')),
        findsNothing,
      );
    },
  );

  testWidgets('dismissing a sheet cancels a held brightness preview', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    final Finder slider = find.descendant(
      of: find.byKey(const ValueKey('board-menu-brightness')),
      matching: find.byType(Slider),
    );
    final previewGate = Completer<void>();
    service.previewWriteGate = previewGate;
    final Rect track = tester.getRect(slider);
    final TestGesture gesture = await tester.startGesture(
      Offset(track.left + track.width * 4 / 7, track.center.dy),
    );
    await gesture.moveTo(Offset(track.right - 4, track.center.dy));
    await tester.pump(const Duration(milliseconds: 40));
    await gesture.up();
    await tester.pump();
    Navigator.of(tester.element(find.byType(Slider))).pop();
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('board-menu-brightness')), findsNothing);
    previewGate.complete();
    await tester.pumpAndSettle();

    expect(service.settingWrites.where((write) => write.awaitAck), isEmpty);
  });

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      'landscape Setup and navigation stay inside system insets on ${platform.name}',
      (tester) async {
        await tester.binding.setSurfaceSize(const Size(844, 390));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _PagedSettingsBleService();
        addTearDown(service.dispose);
        const insets = EdgeInsets.fromLTRB(22, 0, 54, 24);
        await _pumpConnectedGate(
          tester,
          service,
          systemInsets: insets,
          theme: ThemeData(platform: platform),
          layoutSize: const Size(844, 390),
        );

        await tester.tap(
          find.byKey(const ValueKey<String>('landscape-setup-action')),
        );
        await tester.pumpAndSettle();

        final setup = tester.getRect(find.byType(SettingsScreen));
        final navigation = tester.getRect(
          find.byKey(const ValueKey('main-landscape-navigation')),
        );
        expect(setup.left, greaterThanOrEqualTo(insets.left));
        expect(setup.right, lessThanOrEqualTo(844 - insets.right));
        expect(navigation.left, greaterThanOrEqualTo(insets.left));
        expect(navigation.right, lessThanOrEqualTo(844 - insets.right));
        expect(navigation.bottom, lessThanOrEqualTo(390 - insets.bottom));
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets('landscape Litchcraft is a board-first fullscreen workspace', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final craft = _NavigationCraftController(service)
      ..forceLiveStatus = true
      ..exitGate = Completer<void>();
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    addTearDown(() async {
      if (!(craft.exitGate?.isCompleted ?? true)) craft.exitGate?.complete();
      library.dispose();
      craft.dispose();
      await service.dispose();
    });

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await decodeVisibleReviewImages(tester);
    await captureHealthReview(tester, 'litchcraft-landscape-workspace-844');
    expect(find.byKey(const ValueKey('craft-landscape-shell')), findsOneWidget);
    expect(
      find.byKey(const ValueKey('craft-landscape-navigation')),
      findsOneWidget,
    );
    expect(find.byType(AppBar), findsNothing);
    final busyBoardStatus = find.byKey(
      const ValueKey<String>('connected-board-status-action'),
    );
    expect(busyBoardStatus.hitTestable(), findsOneWidget);
    expect(
      tester.getSize(busyBoardStatus).shortestSide,
      greaterThanOrEqualTo(48),
    );
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('craft-landscape-navigation')),
        matching: busyBoardStatus,
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('craft-landscape-navigation')),
        matching: find.byType(NavigationBar),
      ),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('landscape-craft-action')),
      findsOneWidget,
    );
    for (final key in <String>[
      'landscape-play-action',
      'landscape-setup-action',
    ]) {
      final action = find.byKey(ValueKey<String>(key));
      expect(action, findsOneWidget);
      expect(tester.getSize(action).width, greaterThanOrEqualTo(48));
      expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
      expect(action.hitTestable(), findsOneWidget);
    }
    await tester.tap(find.byKey(const ValueKey('landscape-craft-action')));
    await tester.pumpAndSettle();
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final tools = tester.getRect(find.byKey(const ValueKey('craft-toolbar')));
    expect(canvas.overlaps(tools), isFalse);
    expect(canvas.height, greaterThanOrEqualTo(350));
    expect(canvas.width, moreOrLessEquals(canvas.height, epsilon: 0.5));
    expect(find.byKey(const ValueKey('craft-palette')), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-leds')), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-palette-wrap')), findsNothing);
    expect(find.byKey(const ValueKey('craft-target-15')), findsNothing);
    final exitNavigation = tester.getRect(
      find.byKey(const ValueKey('craft-landscape-navigation')),
    );
    expect(exitNavigation.overlaps(canvas), isFalse);
    expect(
      exitNavigation.center.dx < canvas.center.dx,
      tools.center.dx < canvas.center.dx,
      reason: 'The mini-nav remains on the same side as the tool inspector.',
    );
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('craft-inspector')),
        matching: find.byKey(const ValueKey('craft-landscape-navigation')),
      ),
      findsOneWidget,
    );

    final Finder playAction = find
        .byKey(const ValueKey<String>('landscape-play-action'))
        .hitTestable();
    await tester.tap(playAction);
    await tester.pump();
    expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
    expect(craft.leaveCalls, 1);
    expect(find.byType(HomeScreen), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('craft-context-progress')),
      findsNothing,
    );
    final boardStatus = find.byKey(
      const ValueKey<String>('connected-board-status-action'),
    );
    expect(boardStatus.hitTestable(), findsOneWidget);
    expect(tester.getSize(boardStatus).shortestSide, greaterThanOrEqualTo(48));

    craft.exitGate!.complete();
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('craft-context-progress')),
      findsNothing,
    );
  });

  testWidgets('landscape Litchcraft remains usable at 200 percent text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);
    await captureHealthReview(tester, 'litchcraft-landscape-844-200-top');

    expect(find.byKey(const ValueKey('craft-landscape-shell')), findsOneWidget);
    expect(tester.takeException(), isNull);
    final Rect inspector = tester.getRect(
      find.byKey(const ValueKey('craft-toolbar')),
    );
    final Rect targetButton = tester.getRect(
      find.byKey(const ValueKey('craft-leds')),
    );
    expect(targetButton.top, greaterThanOrEqualTo(inspector.top));
    expect(targetButton.bottom, lessThanOrEqualTo(inspector.bottom));
    expect(targetButton.height, greaterThanOrEqualTo(48));

    Future<void> expectInspectorStable(String tray) async {
      await tester.pump();
      await tester.pump();
      expect(find.byKey(const ValueKey('craft-ribbon-scroll')), findsNothing);
      expect(find.byType(Scrollbar), findsNothing);
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      for (final key in <String>['craft-toolbar']) {
        final rect = tester.getRect(find.byKey(ValueKey(key)));
        expect(rect.overlaps(canvas), isFalse, reason: '$tray $key');
      }
    }

    await tester.tap(find.byKey(const ValueKey('craft-leds')));
    await tester.pumpAndSettle();
    final Rect target = tester.getRect(
      find.byKey(const ValueKey('craft-target-4')),
    );
    expect(target.height, greaterThanOrEqualTo(48));
    expect(
      find.byKey(const ValueKey('craft-target-4')).hitTestable(),
      findsOneWidget,
      reason: 'LED targets remain visible without a scrolling inspector',
    );
    await tester.tap(find.byKey(const ValueKey('craft-target-4')));
    await tester.pumpAndSettle();
    await expectInspectorStable('Target');

    await tester.tap(find.byKey(const ValueKey('craft-palette')));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey('craft-inspector-hex')),
      '#22C3E2',
    );
    await tester.testTextInput.receiveAction(TextInputAction.done);
    FocusManager.instance.primaryFocus?.unfocus();
    await tester.pumpAndSettle();
    await expectInspectorStable('Palette');

    await tester.tap(find.byKey(const ValueKey('craft-motion')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    final effectSelector = find.byKey(const ValueKey('craft-effect-selector'));
    expect(effectSelector.hitTestable(), findsOneWidget);
    expect(tester.getSize(effectSelector).height, greaterThanOrEqualTo(48));
    await tester.tap(effectSelector);
    final armEffect = find.byKey(const ValueKey('craft-effect-breathe'));
    for (
      var frame = 0;
      frame < 20 && armEffect.hitTestable().evaluate().isEmpty;
      frame++
    ) {
      await tester.pump(const Duration(milliseconds: 16));
    }
    expect(
      tester
          .widget<PageView>(find.byKey(const ValueKey('main-pages')))
          .controller!
          .page,
      1,
    );
    expect(armEffect.hitTestable(), findsOneWidget);
    await tester.tap(armEffect);
    // Effect previews animate continuously; inspect a rendered intermediate frame.
    await tester.pump(const Duration(milliseconds: 300));
    await expectInspectorStable('Effect');

    await tester.ensureVisible(find.byKey(const ValueKey('craft-animate')));
    expect(
      tester.getSize(find.byKey(const ValueKey('craft-animate'))).shortestSide,
      greaterThanOrEqualTo(48),
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'litchcraft-landscape-844-200-tools');
  });

  testWidgets('landscape Litchcraft navigation uses the real wordmark', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)..forceLiveStatus = true;
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);
    final navigation = find.byKey(const ValueKey('craft-landscape-navigation'));
    expect(navigation, findsOneWidget);
    expect(
      find.descendant(
        of: navigation,
        matching: find.byKey(const ValueKey('craft-landscape-wordmark')),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(of: navigation, matching: find.text('Litchcraft')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey('craft-inspector-previous-color')),
      findsNothing,
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'litchcraft-landscape-r16-wordmark');

    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          landscapeRibbonSide: Alignment.centerLeft,
          initialPageIndex: 0,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);
    final inspector = tester.getRect(
      find.byKey(const ValueKey('craft-persistent-inspector')),
    );
    final quick = tester.getRect(
      find.byKey(const ValueKey('craft-quick-actions')),
    );
    final board = tester.getRect(
      find.byKey(const ValueKey('craft-ribbon-board')),
    );
    expect(inspector.right, lessThan(board.left));
    expect(quick.left, greaterThan(board.right));
    expect(
      find.byKey(const ValueKey('craft-inspector-previous-color')),
      findsNothing,
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'litchcraft-landscape-r16-left-200');
    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets(
    'landscape Litchcraft centers in the safe pane without tray or authority drift',
    (tester) async {
      tester.view.physicalSize = const Size(667, 375);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      const insets = EdgeInsets.fromLTRB(44, 0, 44, 18);
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..forceLiveStatus = true
        ..simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });

      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(padding: insets, viewPadding: insets),
            child: child!,
          ),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            landscapeRibbonSide: Alignment.centerLeft,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(
        tester
            .widget<LitchcraftScreen>(find.byType(LitchcraftScreen))
            .rootSafeInsets,
        insets,
        reason: 'the connected shell must preserve root overlay safe insets',
      );

      final objectFinder = find.byKey(const ValueKey('craft-ribbon-board'));
      final canvasFinder = find.byKey(const ValueKey('litchcraft-canvas'));
      final pageState = tester.state(find.byKey(const ValueKey('main-pages')));
      final craftState = tester.state(find.byType(LitchcraftScreen));
      final object = tester.getRect(objectFinder);
      final canvas = tester.getRect(canvasFinder);
      final toolbar = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      final document = library.animationDocument;
      final dirty = library.dirty;
      final activeName = library.activeName;
      final commandCounts = (
        enter: craft.enterCalls,
        leave: craft.leaveCalls,
        adopt: craft.adoptCalls,
      );
      expect(
        object.center.dy,
        moreOrLessEquals((insets.top + 375 - insets.bottom) / 2, epsilon: .5),
        reason:
            'the canvas centers in the OS-safe viewport, not behind a gesture inset',
      );
      expect(object.top, greaterThanOrEqualTo(insets.top));
      expect(object.bottom, lessThanOrEqualTo(375 - insets.bottom));
      expect(canvas.center.dy, moreOrLessEquals(object.center.dy, epsilon: .5));
      expect(canvas.width, moreOrLessEquals(canvas.height, epsilon: .5));
      expect(toolbar.height, greaterThan(toolbar.width));
      expect(canvas.overlaps(toolbar), isFalse);

      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pumpAndSettle();
      final targetRow = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      expect(
        targetRow.left,
        closeTo(
          math.max(insets.left, LitchWorkspaceGeometry.landscapeGap),
          .01,
        ),
        reason:
            'the tool-side edge counts protected space toward its ordinary gap',
      );
      expect(targetRow.top, greaterThanOrEqualTo(insets.top));
      expect(targetRow.right, lessThanOrEqualTo(667 - insets.right));
      expect(targetRow.bottom, lessThanOrEqualTo(375 - insets.bottom));
      expect(targetRow.overlaps(canvas), isFalse);
      expect(
        find.byKey(const ValueKey('craft-inspector-scroll')),
        findsNothing,
        reason: 'the fitted landscape tool area must not require scrolling',
      );
      expect(
        find.byKey(const ValueKey('craft-target-15')).hitTestable(),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-target-15')));
      await tester.pumpAndSettle();

      expect(
        find.byKey(const ValueKey('craft-palette')).hitTestable(),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-palette')));
      await tester.pumpAndSettle();
      expect(tester.getRect(objectFinder), object);
      expect(tester.getRect(canvasFinder), canvas);

      tester.view.physicalSize = const Size(599, 700);
      await tester.pumpAndSettle();
      final portraitToolbar = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      final portraitCanvas = tester.getRect(canvasFinder);
      expect(portraitToolbar.width, greaterThan(portraitToolbar.height));
      expect(portraitCanvas.overlaps(portraitToolbar), isFalse);
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );
      tester.view.physicalSize = const Size(667, 375);
      await tester.pumpAndSettle();
      expect(tester.getRect(objectFinder), object);
      expect(tester.getRect(canvasFinder), canvas);
      final restoredToolbar = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      expect(restoredToolbar.height, greaterThan(restoredToolbar.width));
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );

      expect(
        identical(
          tester.state(find.byKey(const ValueKey('main-pages'))),
          pageState,
        ),
        isTrue,
      );
      expect(
        identical(tester.state(find.byType(LitchcraftScreen)), craftState),
        isTrue,
      );
      expect(library.animationDocument, same(document));
      expect(library.dirty, dirty);
      expect(library.activeName, activeName);
      expect(craft.selected, isTrue);
      expect((
        enter: craft.enterCalls,
        leave: craft.leaveCalls,
        adopt: craft.adoptCalls,
      ), commandCounts);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'adaptive navigation retains the live Litchcraft page across phone and tablet sizes',
    (tester) async {
      tester.view.physicalSize = const Size(599, 700);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..forceLiveStatus = true
        ..simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      final pageState = tester.state(find.byKey(const ValueKey('main-pages')));
      final craftState = tester.state(find.byType(LitchcraftScreen));
      final canvasState = tester.state(find.byType(LitchcraftCanvasView));
      final document = library.animationDocument;
      final selectedFrame = library.selectedFrameIndex;
      final dirty = library.dirty;
      final activeName = library.activeName;
      final calls = (
        enter: craft.enterCalls,
        leave: craft.leaveCalls,
        adopt: craft.adoptCalls,
      );

      void expectRetained({bool compactLandscape = false}) {
        expect(
          find.byType(NavigationBar),
          compactLandscape ? findsNothing : findsOneWidget,
        );
        if (!compactLandscape) {
          final size = tester.view.physicalSize / tester.view.devicePixelRatio;
          expect(
            tester.getSize(find.byType(NavigationBar)).width,
            size.height >= size.width ? size.width : 320,
          );
        }
        expect(
          find.byKey(const ValueKey('craft-landscape-navigation')),
          compactLandscape ? findsOneWidget : findsNothing,
        );
        expect(
          identical(
            tester.state(find.byKey(const ValueKey('main-pages'))),
            pageState,
          ),
          isTrue,
        );
        expect(
          identical(tester.state(find.byType(LitchcraftScreen)), craftState),
          isTrue,
        );
        expect(
          tester.state(find.byType(LitchcraftCanvasView)),
          same(canvasState),
        );
        expect(library.animationDocument, same(document));
        expect(library.selectedFrameIndex, selectedFrame);
        expect(library.dirty, dirty);
        expect(library.activeName, activeName);
        expect(craft.selected, isTrue);
        expect((
          enter: craft.enterCalls,
          leave: craft.leaveCalls,
          adopt: craft.adoptCalls,
        ), calls);
      }

      expectRetained();
      for (final size in <Size>[
        const Size(600, 700),
        const Size(839, 700),
        const Size(840, 700),
        const Size(600, 480),
        const Size(600, 481),
        const Size(599, 700),
      ]) {
        tester.view.physicalSize = size;
        await tester.pump();
        expectRetained(
          compactLandscape:
              size.width >= 480 &&
              size.height <= 480 &&
              size.width > size.height,
        );
      }
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('compact landscape keeps one coherent wide workspace', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1000, 472);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final semantics = tester.ensureSemantics();
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: RepaintBoundary(
          key: const ValueKey('craft-compact-review-boundary'),
          child: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(tester.takeException(), isNull);
    expect(find.byKey(const ValueKey('craft-landscape-shell')), findsOneWidget);
    expect(
      find.byKey(const ValueKey('craft-landscape-navigation')),
      findsOneWidget,
    );
    expect(find.byType(AppBar), findsNothing);
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final inspector = tester.getRect(
      find.byKey(const ValueKey('craft-toolbar')),
    );
    expect(canvas.width, greaterThanOrEqualTo(243));
    expect(canvas.width, moreOrLessEquals(canvas.height, epsilon: 0.5));
    expect(canvas.right, lessThanOrEqualTo(inspector.left - 8));
    expect(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('craft-ribbon-scroll')), findsNothing);
    expect(find.byType(Scrollbar), findsNothing);
    final motion = find.byKey(const ValueKey('craft-motion'));
    expect(motion, findsOneWidget);
    expect(
      find.descendant(of: motion, matching: find.text('Effects')),
      findsNothing,
      reason: 'Inspector tabs are icon-only; their names remain in semantics.',
    );
    expect(tester.getSize(motion).width, greaterThanOrEqualTo(48));
    expect(tester.getSize(motion).width, lessThanOrEqualTo(112));
    expect(tester.getSize(motion).height, greaterThanOrEqualTo(48));
    final motionSemantics = tester.getSemantics(motion);
    expect(motionSemantics.label, 'Effects');
    expect(
      motionSemantics.getSemanticsData().hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    final ribbonControlCenters = <int>{};
    for (final key in <String>[
      'craft-palette',
      'craft-leds',
      'craft-motion',
      'craft-animate',
    ]) {
      final control = find.byKey(ValueKey(key));
      expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
      ribbonControlCenters.add(tester.getCenter(control).dx.round());
    }
    expect(ribbonControlCenters, hasLength(1));
    final history = tester.getRect(
      find.byKey(const ValueKey('craft-history-footer')),
    );
    for (final key in <String>['craft-undo', 'craft-redo']) {
      final control = find.byKey(ValueKey(key));
      expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
      expect(history.contains(tester.getCenter(control)), isTrue);
    }
    await captureHealthReview(
      tester,
      'litchcraft-landscape-shell-1000x472',
      boundaryFinder: find.byKey(
        const ValueKey('craft-compact-review-boundary'),
      ),
    );
    motionSemantics.owner!.performAction(
      motionSemantics.id,
      ui.SemanticsAction.tap,
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
    final armEffect = find.byKey(const ValueKey('craft-effect-breathe'));
    // The fitted inspector needs no scroll; dragging its owned tool surface
    // must still leave the outer pager on Litchcraft.
    expect(find.byKey(const ValueKey('craft-inspector-scroll')), findsNothing);
    await tester.drag(
      find.byKey(const ValueKey('craft-toolbar')),
      const Offset(0, -160),
    );
    await tester.pump(const Duration(milliseconds: 300));
    expect(
      tester
          .widget<PageView>(find.byKey(const ValueKey('main-pages')))
          .controller!
          .page,
      1,
    );
    expect(armEffect.hitTestable(), findsOneWidget);
    expect(tester.getSize(armEffect).shortestSide, greaterThanOrEqualTo(48));
    expect(
      tester
          .getSemantics(armEffect)
          .getSemanticsData()
          .hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    // Main toolbar tabs replace content in the persistent inspector.
    expect(
      find.byKey(const ValueKey('craft-animate')).hitTestable(),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const ValueKey('craft-animate')));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('craft-frame-filmstrip')), findsOneWidget);
    final addFrame = find.byKey(const ValueKey('craft-add-frame'));
    expect(tester.widget<IconButton>(addFrame).onPressed, isNotNull);
    expect(tester.getSize(addFrame).width, greaterThanOrEqualTo(48));
    expect(tester.getSize(addFrame).height, greaterThanOrEqualTo(48));
    final addFrameSemantics = tester.getSemantics(find.byTooltip('New frame'));
    expect(
      addFrameSemantics.getSemanticsData().hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    addFrameSemantics.owner!.performAction(
      addFrameSemantics.id,
      ui.SemanticsAction.tap,
    );
    await tester.pump();
    expect(library.animationDocument.frames, hasLength(2));
    expect(library.selectedFrameIndex, 1);
    await tester.pump(const Duration(milliseconds: 601));
    await tester.pumpWidget(const SizedBox());
    await library.flush();
    semantics.dispose();
  });

  testWidgets(
    'compact Litchcraft keeps board options outside the creative workspace',
    (tester) async {
      tester.view.physicalSize = const Size(540, 360);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..simulatePhysicalMode(true)
        ..showStatus(LitchcraftStatus.live);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final inspector = find.byKey(const ValueKey('craft-toolbar'));
      final initialCanvas = tester.getRect(canvas);
      void expectStableWorkspace() {
        final status = find.byKey(
          const ValueKey<String>('connected-board-status-action'),
        );
        expect(status.hitTestable(), findsOneWidget);
        expect(tester.getSize(status).shortestSide, greaterThanOrEqualTo(48));
        expect(tester.getRect(canvas), initialCanvas);
      }

      expect(
        find.byKey(const ValueKey('craft-landscape-navigation')),
        findsOneWidget,
      );
      expect(tester.getRect(inspector).overlaps(initialCanvas), isFalse);
      expectStableWorkspace();

      craft.showStatus(LitchcraftStatus.connecting);
      await tester.pump();
      expectStableWorkspace();

      craft.showStatus(LitchcraftStatus.offline);
      await tester.pump();
      expectStableWorkspace();
      expect(tester.takeException(), isNull);
    },
  );

  for (final width in <double>[540, 640, 844]) {
    testWidgets(
      'connected Play uses a fixed board and compact chrome at ${width.toInt()}x360',
      (tester) async {
        tester.view.physicalSize = Size(width, 360);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        addTearDown(() async {
          library.dispose();
          craft.dispose();
          await service.dispose();
        });

        await tester.pumpWidget(
          MaterialApp(
            home: RepaintBoundary(
              key: const ValueKey('play-landscape-review-boundary'),
              child: LitchMainScaffold(
                bleService: service,
                boardId: 'board-1',
                boards: const <LitchBoard>[],
                onDisconnect: () async {},
                craftController: craft,
                craftLibrary: library,
                initialPageIndex: 1,
                initialCraftModeResolved: true,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();

        expect(find.byType(AppBar), findsNothing);
        expect(
          find.byKey(const ValueKey('main-landscape-navigation')),
          findsOneWidget,
        );
        for (final key in <String>[
          'landscape-play-action',
          'landscape-craft-action',
          'landscape-setup-action',
        ]) {
          final action = find.byKey(ValueKey<String>(key));
          expect(action, findsOneWidget);
          expect(tester.getSize(action).width, greaterThanOrEqualTo(48));
          expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
        }
        final boardFinder = find.byKey(
          const ValueKey('home-chessboard-surface'),
        );
        final before = tester.getRect(boardFinder);
        final expectedBoard = LitchWorkspaceGeometry(
          Size(width, 360),
        ).landscapeCanvasSide;
        expect(before.width, moreOrLessEquals(expectedBoard, epsilon: 1));
        expect(before.height, moreOrLessEquals(before.width, epsilon: .5));
        final compactChrome = tester.getRect(
          find.byKey(const ValueKey('main-landscape-navigation')),
        );
        expect(
          compactChrome.left,
          greaterThanOrEqualTo(before.right),
          reason:
              'compact Play chrome must never tax or cover the board column',
        );
        final details = _homeDetailsScrollable();
        final detailsState = tester.state<ScrollableState>(details);
        expect(detailsState.position.maxScrollExtent, greaterThan(0));
        await tester.drag(details, const Offset(0, -180));
        await tester.pump();
        expect(detailsState.position.pixels, greaterThan(0));
        expect(tester.getRect(boardFinder), before);
        expect(tester.takeException(), isNull);
        await captureHealthReview(
          tester,
          'play-landscape-shell-${width.toInt()}',
          boundaryFinder: find.byKey(
            const ValueKey('play-landscape-review-boundary'),
          ),
        );
      },
    );
  }

  testWidgets(
    'Litchcraft Play control leaves immediately and autosaves its draft',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)..retained = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final Rect pages = tester.getRect(
        find.byKey(const ValueKey<String>('main-pages')),
      );

      var swipe = await tester.startGesture(
        Offset(pages.left + 20, pages.center.dy),
      );
      await swipe.moveBy(Offset(pages.width * 0.52, 0));
      await tester.pump(const Duration(milliseconds: 500));
      await swipe.up();
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(_activeNavigationIndex(tester), 1); // Litchcraft follows Play.

      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final canvasRect = tester.getRect(canvas);
      await tester.tapAt(
        canvasRect.topLeft +
            Offset(canvasRect.width / 16, canvasRect.height / 16),
      );
      await tester.pump();
      await _selectAdjacentMainPage(tester, forward: false);
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.leaveCalls, 1);
      await library.flush();
      final prefs = await SharedPreferences.getInstance();
      expect(prefs.getString(LitchcraftArtworkStore.currentKey), isNotNull);
    },
  );

  testWidgets(
    'Litchcraft exit renders Play immediately while board exit finishes',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..retained = true
        ..exitGate = Completer<void>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await library.save('Saved artwork');
      addTearDown(() async {
        if (!(craft.exitGate?.isCompleted ?? true)) craft.exitGate?.complete();
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.leaveCalls, 1);
      expect(find.byType(HomeScreen), findsOneWidget);
      expect(find.text('Saving artwork…'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );

      craft.exitGate!.complete();
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.selected, isFalse);
      expect(find.text('Saving artwork…'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
    },
  );

  testWidgets('Play does not show a Litchcraft discovery card', (tester) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    final library = LitchcraftLibrary(craft);
    await library.ready;

    Widget screen() => MaterialApp(
      builder: (_, child) => _withTestPreferences(child!, showTips: true),
      home: LitchMainScaffold(
        bleService: service,
        boardId: 'board-1',
        boards: const <LitchBoard>[],
        onDisconnect: () async {},
        craftController: craft,
        craftLibrary: library,
        initialPageIndex: 1,
        firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
      ),
    );

    await tester.pumpWidget(screen());
    await tester.pumpAndSettle();
    expect(find.text('New: Litchcraft'), findsNothing);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets(
    'backgrounding preserves a just-painted draft before its debounce expires',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.pumpAndSettle();
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pumpAndSettle();
      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      await tester.ensureVisible(canvas);
      await tester.pumpAndSettle();
      final rect = tester.getRect(canvas);
      await tester.tapAt(
        rect.topLeft + Offset(rect.width / 16, rect.height / 16),
      );
      await tester.pump();
      final prefs = await SharedPreferences.getInstance();
      expect(prefs.getString(LitchcraftArtworkStore.currentKey), isNull);
      _sendAppToBackground(tester);
      await tester.pump();
      final source = prefs.getString(LitchcraftArtworkStore.currentKey);
      expect(source, isNotNull);
      final json = jsonDecode(source!) as Map<String, dynamic>;
      expect((json['draft'] as Map<String, dynamic>)['colors'][0], 0xff7430);
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    },
  );

  testWidgets(
    'Litchcraft follows initial Play and admission only follows selection',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.enterCalls, 0);
      await _selectAdjacentMainPage(tester, forward: true);
      expect(_activeNavigationIndex(tester), 1);
      expect(craft.enterCalls, 1);
      craft.exitGate = Completer<void>();
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump();
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(craft.leaveCalls, 1);
      expect(_activeNavigationIndex(tester), 0);
      expect(
        find.byWidgetPredicate(
          (widget) => widget is AbsorbPointer && widget.absorbing,
        ),
        findsNothing,
      );
      craft.exitGate!.complete();
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.selected, isFalse);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'an already-active connected board selects Litchcraft without entering it',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..connectedBoardActive = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(craft.adoptCalls, 1);
      expect(craft.enterCalls, 0);
      expect(
        _activeNavigationIndex(tester),
        1,
      ); // Active board selects Litchcraft.

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'retained Setup and Console returns revalidate live Litchcraft authority',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'enable_app_console': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)..forceLiveStatus = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            initialCraftModeResolved: true,
            loadAppConsolePreference: () async => true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 1);
      for (final page in <String>['setup', 'console']) {
        await tester.tap(_navigationAction(tester, page));
        await tester.pumpAndSettle();
        craft.revalidateGate = Completer<void>();
        final int paintCalls = craft.paintCalls;
        await tester.tap(_navigationAction(tester, 'litchcraft'));
        await tester.pump();
        final Finder canvas = find
            .byKey(const ValueKey<String>('litchcraft-canvas'))
            .hitTestable();
        expect(canvas, findsOneWidget);
        await tester.tapAt(tester.getCenter(canvas));
        await tester.pump();
        expect(
          craft.paintCalls,
          paintCalls + 1,
          reason: 'local artwork remains editable during board revalidation',
        );
        craft.revalidateGate!.complete();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 250));
        expect(craft.authorityResolving, isFalse);
        await tester.tapAt(tester.getCenter(canvas));
        await tester.pump();
        expect(craft.paintCalls, paintCalls + 2);
      }

      expect(craft.leaveCalls, 0);
      expect(craft.enterCalls, 1);
      expect(
        craft.revalidateCalls,
        2,
        reason:
            'same-link returns must inspect cached session/revision authority',
      );
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'physical board entry and exit follow Play while Setup keeps its place',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      int navigationIndex() => _activeNavigationIndex(tester);
      expect(
        navigationIndex(),
        0,
      ); // Play is now the first visible destination.

      craft.simulatePhysicalMode(true);
      await tester.pumpAndSettle();
      expect(navigationIndex(), 1); // The active board selects Litchcraft.
      expect(craft.enterCalls, 0);

      craft.simulatePhysicalMode(false);
      await tester.pumpAndSettle();
      expect(navigationIndex(), 0);
      expect(craft.leaveCalls, 0);

      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      expect(navigationIndex(), 2);

      craft.simulatePhysicalMode(true);
      await tester.pumpAndSettle();
      expect(navigationIndex(), 2);
      expect(craft.enterCalls, 0);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets('warm mode resync keeps Setup visible when the board is active', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    final library = LitchcraftLibrary(craft);
    int resolvedModes = 0;
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: service.disconnect,
          craftController: craft,
          craftLibrary: library,
          initialCraftModeResolved: true,
          onModeResolved: () => ++resolvedModes,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));

    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(_activeNavigationIndex(tester), 2);

    craft
      ..connectedBoardActive = true
      ..adoptGate = Completer<bool>();
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    service.emitConnection('board-1', DeviceConnectionState.connected);
    await tester.pump();

    await tester.tap(_navigationAction(tester, 'play'));
    await tester.pump();
    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pump();
    craft.adoptGate!.complete(true);
    await tester.pumpAndSettle();

    expect(craft.adoptCalls, 1);
    expect(resolvedModes, 1);
    expect(_activeNavigationIndex(tester), 2);

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets('finishing a cold probe does not replace Setup with Litchcraft', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    Widget shell({required bool resolving, required int initialPage}) =>
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: initialPage,
            initialCraftModeResolved: true,
            modeResolving: resolving,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        );

    await tester.pumpWidget(shell(resolving: true, initialPage: 1));
    await tester.pump(const Duration(milliseconds: 100));
    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pump(const Duration(milliseconds: 500));

    await tester.pumpWidget(shell(resolving: false, initialPage: 0));
    await tester.pumpAndSettle();

    expect(_activeNavigationIndex(tester), 2);

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets(
    'Setup keeps connected status while selected controller recovery settles',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..retained = true
        ..preserveSelectionOnInactiveAdopt = true
        ..modeResolutionGate = Completer<void>();
      final library = LitchcraftLibrary(craft);
      int resolvedModes = 0;
      await library.ready;

      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 2,
            modeResolving: true,
            onModeResolved: () => ++resolvedModes,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();

      expect(craft.adoptCalls, 1);
      expect(resolvedModes, 0);
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );

      craft.modeResolutionGate!.complete();
      await tester.pump();

      expect(resolvedModes, 1);
      expect(_activeNavigationIndex(tester), 2);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'artwork startup stays usable while connection status awaits mode resolution',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>()
        ..modeResolutionGate = Completer<void>();
      final preferences = await SharedPreferences.getInstance();
      final store = _DelayedArtworkStore(preferences);
      final library = LitchcraftLibrary(craft, store: store);

      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();

      expect(craft.adoptCalls, 1);
      expect(
        craft.hasMarksAtAdopt,
        isFalse,
        reason:
            'Read-only mode inspection starts before saved-draft restoration.',
      );
      expect(craft.inspectedBoardActive, isNull);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsOneWidget,
      );
      expect(find.text('Preparing board…'), findsNothing);
      expect(service.isConnected, isTrue);
      expect(
        find.byKey(const ValueKey('discovered-board-board-1')),
        findsOneWidget,
      );
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);

      craft.adoptGate!.complete(true);
      await tester.pump();
      await tester.pump();
      expect(craft.inspectedBoardActive, isTrue);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(store.loadGate.isCompleted, isFalse);
      expect(
        craft.modeResolutionGate!.isCompleted,
        isFalse,
        reason:
            'Mode proof does not imply rich artwork reconciliation is ready.',
      );

      final now = DateTime.utc(2026, 9, 11);
      store.loadGate.complete(
        LitchcraftLibrarySnapshot(
          draft: LitchcraftArtwork(
            id: 'draft',
            name: 'Recovered canvas',
            canvas: LitchcraftCanvas.blank().paint(<int>[0], 1, 0x123456),
            createdAt: now,
            modifiedAt: now,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      expect(craft.hasMarks, isTrue);
      expect(library.session.canvas.colors[0], 0x123456);
      craft.modeResolutionGate!.complete();
      await tester.pumpAndSettle();

      expect(craft.adoptCalls, 1);
      expect(craft.hasMarksAtAdopt, isFalse);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(_activeNavigationIndex(tester), 1); // Litchcraft follows Play.
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'app launch keeps useful discovery while its compact mode overlay resolves',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      tester.view.physicalSize = const Size(320, 1200);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;

      await tester.pumpWidget(
        MaterialApp(
          builder: (BuildContext context, Widget? child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();

      expect(find.byKey(const ValueKey('connected-mode-gate')), findsNothing);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );
      final RenderParagraph connectingLabel = tester.renderObject(
        find.byKey(const ValueKey('chosen-board-phase')),
      );
      expect(connectingLabel.didExceedMaxLines, isFalse);
      expect(tester.takeException(), isNull);
      await tester.pump(const Duration(seconds: 9));
      expect(find.text('Preparing board…'), findsNothing);
      expect(service.isConnected, isTrue);
      expect(
        find.byKey(const ValueKey('discovered-board-board-1')),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Text>(find.byKey(const ValueKey('chosen-board-phase')))
            .data,
        'Connecting…',
      );
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(craft.enterCalls, 0);
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pump();
      expect(craft.adoptCalls, 1);

      craft.adoptGate!.complete(true);
      await tester.pumpAndSettle();

      expect(craft.adoptCalls, 1);
      expect(craft.enterCalls, 0);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(_activeNavigationIndex(tester), 1); // Resolved active board.
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pump();
      expect(find.byKey(const ValueKey('connected-mode-gate')), findsNothing);
      expect(craft.adoptCalls, 1);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets('version artwork asks before loading during slow board entry', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'test.litch',
      version: '1.0.0',
      buildNumber: '1',
      buildSignature: '',
    );
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..enterGate = Completer<void>();
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const [],
          onDisconnect: service.disconnect,
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 1,
          initialCraftModeResolved: true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('open-about')));
    await tester.pumpAndSettle();
    for (var i = 0; i < 5; i++) {
      await tester.tap(find.byKey(const ValueKey('about-version')));
      await tester.pump();
    }
    await tester.pump(const Duration(seconds: 2));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsNothing);
    expect(find.byType(AlertDialog), findsOneWidget);
    expect(library.animationDocument.frames, hasLength(1));
    await tester.tap(find.widgetWithText(FilledButton, 'Open'));
    await tester.pumpAndSettle();
    expect(find.byType(LitchcraftScreen), findsOneWidget);
    // The owning About contract maps five taps to Firefly Garden, opened paused.
    expect(library.animationDocument.frames, hasLength(128));
    expect(library.animationDocument.playbackOnEntry, isFalse);
    expect(library.selectedFrameIndex, 0);
    expect(craft.enterGate!.isCompleted, isFalse);
    expect(find.text('Preparing board…'), findsNothing);
    expect(find.byType(CircularProgressIndicator), findsNothing);
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .enabled,
      isTrue,
    );
    craft.enterGate!.complete();
    await tester.pump();
    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets('eight paced About taps choose RR without naming it', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'test.litch',
      version: '1.0.0',
      buildNumber: '1',
      buildSignature: '',
    );
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service);
    final library = LitchcraftLibrary(craft);
    try {
      await library.ready;
      final original = library.animationDocument;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            versionTapClock: tester.binding.clock.now,
            initialPageIndex: 1,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      Future<void> requestRickroll() async {
        await tester.tap(find.byKey(const ValueKey('open-about')));
        await tester.pumpAndSettle();
        final version = find.byKey(const ValueKey('about-version'));
        for (var tap = 0; tap < 8; tap++) {
          if (tap > 0) {
            await tester.pump(const Duration(milliseconds: 850));
          }
          await tester.tap(version);
          await tester.pump();
        }
        expect(find.byType(AboutScreen), findsOneWidget);
        await tester.pump(const Duration(milliseconds: 1301));
        await tester.pumpAndSettle();
        expect(find.byType(AboutScreen), findsNothing);
        expect(find.byType(AlertDialog), findsOneWidget);
        expect(find.textContaining('Rickroll'), findsNothing);
        expect(find.textContaining('Bouncing Balls'), findsNothing);
      }

      await requestRickroll();
      expect(library.animationDocument, same(original));
      await tester.tap(find.widgetWithText(TextButton, 'Cancel'));
      await tester.pumpAndSettle();
      expect(library.animationDocument, same(original));

      await requestRickroll();
      expect(library.animationDocument, same(original));
      await tester.tap(find.widgetWithText(FilledButton, 'Open'));
      await tester.pumpAndSettle();
      expect(library.animationDocument.frames, hasLength(32));
      expect(library.activeName, isNot('Rickroll'));
      expect(library.animationDocument.playbackOnEntry, isTrue);
      expect(craft.animationPlaying, isFalse);
    } finally {
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    }
  });

  testWidgets('disconnected mode probe cannot finish board preparation', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..adoptGate = Completer<bool>();
    final library = LitchcraftLibrary(craft);
    await library.ready;
    int resolvedModes = 0;
    final selectedPages = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: service.disconnect,
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 1,
          onModeResolved: () => ++resolvedModes,
          onPageSelected: selectedPages.add,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pump();
    expect(find.text('Preparing board…'), findsNothing);
    expect(find.text('Connected'), findsOneWidget);
    await tester.pump(const Duration(seconds: 9));
    await tester.tap(
      find.byKey(const ValueKey('connected-board-status-action')),
    );
    await tester.pump(const Duration(milliseconds: 200));
    expect(
      find.byKey(const ValueKey('connected-board-popover')),
      findsOneWidget,
    );
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    expect(find.byKey(const ValueKey('connected-board-popover')), findsNothing);
    expect(find.text('Preparing board…'), findsNothing);
    expect(find.text('Not connected'), findsOneWidget);
    expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
    expect(
      find.byKey(const ValueKey('connected-board-mode-progress')),
      findsNothing,
    );
    final retiredProbe = craft.adoptGate!;
    craft.adoptGate = Completer<bool>();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    await tester.pump();
    expect(find.text('Preparing board…'), findsNothing);
    retiredProbe.complete(true);
    await tester.pump();
    expect(
      find.text('Preparing board…'),
      findsNothing,
      reason: 'Mode probes never replace the connected status with progress.',
    );
    expect(resolvedModes, 0);
    expect(selectedPages, isNot(contains(0)));
    expect(craft.enterCalls, 0);
    craft.adoptGate!.complete(false);
    await tester.pumpAndSettle();
    expect(resolvedModes, 1);
    expect(find.text('Preparing board…'), findsNothing);
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  for (final (Locale locale, String label) in <(Locale, String)>[
    (const Locale('en'), 'Connected'),
    (const Locale('it'), 'Connesso'),
    (const Locale('de'), 'Verbunden'),
    (const Locale('fr'), 'Connecté'),
    (const Locale('es'), 'Conectado'),
  ]) {
    testWidgets(
      'top-right connected label fits ${locale.languageCode} at 320px and 130 percent text',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        tester.view.physicalSize = const Size(320, 1200);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..adoptGate = Completer<bool>();
        final library = LitchcraftLibrary(craft);
        await library.ready;

        await tester.pumpWidget(
          MaterialApp(
            locale: locale,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (BuildContext context, Widget? child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(1.3)),
              child: child!,
            ),
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pump();

        expect(
          find.byKey(const ValueKey('connection-progress-overlay')),
          findsOneWidget,
        );
        expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
        craft.adoptGate!.complete(false);
        await tester.pumpAndSettle();
        final RenderParagraph connectedLabel = tester.renderObject(
          find.text(label),
        );
        expect(connectedLabel.didExceedMaxLines, isFalse);
        expect(tester.takeException(), isNull);

        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets(
    'warm Litchcraft reconnect keeps the canvas visible while mode resynchronizes',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..connectedBoardActive = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;

      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      final Finder canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final editor = tester.state(find.byType(LitchcraftScreen));

      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      final Rect disconnectedCanvas = tester.getRect(canvas);
      expect(
        disconnectedCanvas.width,
        moreOrLessEquals(disconnectedCanvas.height, epsilon: .5),
      );
      expect(disconnectedCanvas.top, greaterThanOrEqualTo(0));
      expect(disconnectedCanvas.bottom, lessThanOrEqualTo(600));

      craft.adoptGate = Completer<bool>();
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
      ]);
      await tester.pump(Duration.zero);
      expect(service.connectCalls, ['board-1']);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsNothing,
        reason:
            'Warm reconnect must not flash a connection card over the canvas.',
      );
      expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
      expect(tester.getRect(canvas), disconnectedCanvas);
      await tester.pump(const Duration(milliseconds: 700));
      expect(find.byKey(const ValueKey('chosen-board-phase')), findsNothing);
      final paintsBeforeReconnect = craft.paintCalls;
      await tester.tapAt(
        disconnectedCanvas.center +
            Offset(
              disconnectedCanvas.width / 16,
              disconnectedCanvas.height / 16,
            ),
      );
      await tester.pump();
      expect(
        craft.paintCalls,
        greaterThan(paintsBeforeReconnect),
        reason: 'The retained offline editor remains usable during reconnect.',
      );
      service.completeNextConnect(true);
      await tester.pump();

      expect(find.byKey(const ValueKey('connected-mode-gate')), findsNothing);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
      expect(craft.leaveCalls, 0);
      expect(find.text('Connecting your canvas'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('connected-board-status-action')),
        findsOneWidget,
      );
      final Rect resyncCanvas = tester.getRect(canvas);
      expect(
        resyncCanvas.width,
        moreOrLessEquals(resyncCanvas.height, epsilon: .5),
      );
      expect(resyncCanvas.top, greaterThanOrEqualTo(0));
      expect(resyncCanvas.bottom, lessThanOrEqualTo(600));

      craft.adoptGate!.complete(true);
      await tester.pumpAndSettle();
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
      expect(tester.getRect(canvas), resyncCanvas);

      await tester.tap(
        find.byKey(const ValueKey('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('Disconnect'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(service.isConnected, isFalse);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(tester.state(find.byType(LitchcraftScreen)), same(editor));
      expect(tester.getRect(canvas), resyncCanvas);
      expect(find.byKey(const ValueKey('chosen-board-phase')), findsNothing);
      expect(
        find.byKey(const ValueKey('disconnected-board-status-action')),
        findsOneWidget,
      );
      expect(craft.leaveCalls, 0);

      await tester.tap(
        find.byKey(const ValueKey('disconnected-board-status-action')),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
      ]);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('discovered-board-board-1')),
        findsOneWidget,
        reason: 'The offline status action opens the existing board chooser.',
      );
      expect(find.byType(LitchcraftScreen), findsNothing);

      final chosenBoard = find.byKey(
        const ValueKey('discovered-board-board-1'),
      );
      await tester.tap(
        find.descendant(
          of: chosenBoard,
          matching: find.widgetWithText(FilledButton, 'Connect'),
        ),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsOneWidget,
      );
      service.completeNextConnect(true);
      await tester.pumpAndSettle();
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      final reconnectedEditor = tester.state(find.byType(LitchcraftScreen));
      final reconnectedCanvas = tester.getRect(canvas);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
      ]);
      await tester.pump(Duration.zero);
      expect(service.connectCalls, hasLength(3));
      expect(find.text('Connecting…'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsNothing,
      );
      expect(tester.state(find.byType(LitchcraftScreen)), same(reconnectedEditor));
      expect(tester.getRect(canvas), reconnectedCanvas);
      final paintsBeforeRetry = craft.paintCalls;
      await tester.tapAt(
        reconnectedCanvas.center +
            Offset(reconnectedCanvas.width / 16, reconnectedCanvas.height / 16),
      );
      await tester.pump();
      expect(
        craft.paintCalls,
        greaterThan(paintsBeforeRetry),
        reason: 'Pending warm transport keeps the retained canvas editable.',
      );
      service.completeNextConnect(false);
      await tester.pumpAndSettle();
      expect(find.text('Connecting…'), findsNothing);
      final offlineStatus = find.byKey(
        const ValueKey('disconnected-board-status-action'),
      );
      expect(offlineStatus.hitTestable(), findsOneWidget);
      expect(tester.state(find.byType(LitchcraftScreen)), same(reconnectedEditor));
      final disconnectsBeforeChoosing = service.disconnectCalls;
      await tester.tap(offlineStatus);
      await tester.pump();
      expect(service.disconnectCalls, disconnectsBeforeChoosing + 1);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsNothing,
      );
      expect(
        chosenBoard,
        findsOneWidget,
        reason: 'After the failed attempt, offline status opens board choices.',
      );
      expect(craft.leaveCalls, 0);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'warm Litchcraft reconnect retains canvas without connecting copy while admission settles',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..connectedBoardActive = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final Finder canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final Rect restingCanvas = tester.getRect(canvas);
      expect(find.text('Connecting your canvas'), findsNothing);

      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      craft
        ..connectedBoardActive = false
        ..adoptGate = Completer<bool>()
        ..enterGate = Completer<void>();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
      expect(tester.getRect(canvas), restingCanvas);

      craft.adoptGate!.complete(false);
      await tester.pump();
      expect(craft.enterCalls, 1);
      expect(find.text('Connecting your canvas'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
      expect(tester.getRect(canvas), restingCanvas);

      craft.enterGate!.complete();
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-context-progress')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('connected-board-status-action')),
        findsOneWidget,
      );
      expect(tester.getRect(canvas), restingCanvas);
      expect(_activeNavigationIndex(tester), 1); // Reconnected Litchcraft.

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final explicitCraft in [false, true]) {
    testWidgets(
      'retained Craft reconnect ${explicitCraft ? 'honors current explicit Craft' : 'follows inactive board to Play'}',
      (tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..connectedBoardActive = true;
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(_activeNavigationIndex(tester), 1);
        service.emitConnection('board-1', DeviceConnectionState.disconnected);
        await tester.pump();
        craft.adoptGate = Completer<bool>();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        await tester.pump();
        expect(craft.inspectedBoardActive, isNull);
        expect(_activeNavigationIndex(tester), 1);
        if (explicitCraft) {
          await tester.tap(_navigationAction(tester, 'litchcraft'));
          await tester.pump();
        }
        craft.adoptGate!.complete(false);
        await tester.pumpAndSettle();
        expect(craft.enterCalls, explicitCraft ? 1 : 0);
        expect(_activeNavigationIndex(tester), explicitCraft ? 1 : 0);
        expect(craft.selected, explicitCraft);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets('page drag interrupted by disconnect retains Litchcraft', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..connectedBoardActive = true;
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: ConnectionGateScreen(
          bleService: service,
          craftController: craft,
          craftLibrary: library,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final pages = tester.getRect(find.byKey(const ValueKey('main-pages')));
    final pointer = await tester.startGesture(
      Offset(pages.center.dx, pages.top + 4),
    );
    await pointer.moveBy(const Offset(80, 0));
    await tester.pump();
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    await pointer.moveBy(const Offset(80, 0));
    await pointer.up();
    await tester.pump();

    expect(craft.leaveCalls, 0);
    expect(craft.selected, isTrue);
    expect(find.byType(LitchcraftScreen), findsOneWidget);

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets(
    'replacement board gets a cold mode probe before any destination',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byType(HomeScreen), findsOneWidget);

      craft.adoptGate = Completer<bool>();
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitConnection('board-2', DeviceConnectionState.connected);
      await tester.pump();

      expect(find.byKey(const ValueKey('connected-mode-gate')), findsNothing);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );

      craft.adoptGate!.complete(true);
      await tester.pumpAndSettle();
      expect(find.byType(LitchcraftScreen), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets('app launch routes an inactive board to Play after mode probe', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..adoptGate = Completer<bool>();
    final library = LitchcraftLibrary(craft);
    await library.ready;

    await tester.pumpWidget(
      MaterialApp(
        home: ConnectionGateScreen(
          bleService: service,
          craftController: craft,
          craftLibrary: library,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pump();
    expect(find.byKey(const ValueKey('connected-mode-gate')), findsNothing);
    expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
    expect(
      find.byKey(const ValueKey('connected-board-mode-progress')),
      findsNothing,
    );

    craft.adoptGate!.complete(false);
    await tester.pumpAndSettle();

    expect(craft.adoptCalls, 1);
    expect(craft.enterCalls, 0);
    expect(
      _activeNavigationIndex(tester),
      0,
    ); // Inactive board returns to Play.

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets(
    'initial Litchcraft destination admits an inactive connected board',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            initialPageIndex: 0,
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(craft.adoptCalls, 1);
      expect(craft.enterCalls, 1);
      expect(_activeNavigationIndex(tester), 1); // Admitted Litchcraft.

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final (size, scale) in <(Size, double)>[
    (const Size(390, 844), 1),
    (const Size(390, 844), 2),
    (const Size(844, 390), 2),
  ]) {
    for (final cancel in <bool>[false, true]) {
      testWidgets(
        'chosen board stays visible through cold proof and retry $size $scale cancel=$cancel',
        (tester) async {
          final semantics = tester.ensureSemantics();
          SharedPreferences.setMockInitialValues(<String, Object>{
            'litchcraft_discovery_dismissed': true,
          });
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final service = _ConnectionGateBleService();
          final craft = _NavigationCraftController(service)
            ..adoptGate = Completer<bool>();
          final library = LitchcraftLibrary(craft);
          await library.ready;
          const board = LitchBoard(
            id: 'chosen-board',
            name: 'Foxie Litch in the living room',
            rssi: -40,
          );
          await tester.pumpWidget(
            MaterialApp(
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(scale)),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: service,
                craftController: craft,
                craftLibrary: library,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          await tester.pump();
          service.emitAvailability(BleAvailability.ready);
          service.emitBoards(const <LitchBoard>[board]);
          await tester.pump();
          await tester.tap(find.text('Connect'));
          await tester.pump();
          void expectChosenBoard() {
            expect(
              find.byKey(const ValueKey('chosen-board-connection')),
              findsOneWidget,
            );
            expect(
              tester
                  .widget<Text>(
                    find.byKey(const ValueKey('chosen-board-phase')),
                  )
                  .data,
              contains(board.name),
            );
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            final escape = find.byKey(const ValueKey('chosen-board-cancel'));
            expect(escape.hitTestable(), findsOneWidget);
            expect(tester.getSize(escape).height, greaterThanOrEqualTo(48));
            expect(
              tester
                  .getSemantics(escape)
                  .getSemanticsData()
                  .hasAction(ui.SemanticsAction.tap),
              isTrue,
            );
            expect(tester.takeException(), isNull);
          }

          expectChosenBoard();
          await tester.pump(const Duration(seconds: 2));
          expectChosenBoard();
          service.completeNextConnect(true);
          await tester.pump();
          await tester.pump();
          expectChosenBoard();
          await tester.pump(const Duration(seconds: 2));
          expectChosenBoard();
          final retiredProof = craft.adoptGate!;
          service.emitConnection(board.id, DeviceConnectionState.disconnected);
          await tester.pump();
          expectChosenBoard();
          craft.adoptGate = Completer<bool>();
          service.emitBoards(const <LitchBoard>[board]);
          await tester.pump();
          await tester.pump(Duration.zero);
          expect(service.connectCalls, hasLength(2));
          service.completeNextConnect(true);
          await tester.pump();
          retiredProof.complete(true);
          await tester.pump();
          expectChosenBoard();
          if (cancel) {
            await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            await tester.pump();
            craft.adoptGate!.complete(true);
            await tester.pump();
            await tester.pump();
            expect(
              find.byKey(const ValueKey('chosen-board-connection')),
              findsNothing,
            );
            expect(find.byType(LitchMainScaffold), findsNothing);
            expect(service.disconnectCalls, 1);
          } else {
            craft.showInspectedMode(true);
            await tester.pump();
            expect(_activeNavigationIndex(tester), 1);
            expect(
              find.byKey(const ValueKey('chosen-board-connection')),
              findsNothing,
            );
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
            craft.adoptGate!.complete(true);
          }
          expect(craft.enterCalls, 0);
          await tester.pumpWidget(const SizedBox());
          await library.flush();
          library.dispose();
          craft.dispose();
          await service.dispose();
          semantics.dispose();
        },
      );
    }
  }

  testWidgets(
    'accepted mode inspection routes Craft before rich adoption or settle',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>()
        ..modeResolutionGate = Completer<void>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      var resolved = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            onModeResolved: () => ++resolved,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(find.text('Connected'), findsOneWidget);

      craft.showInspectedMode(true);
      await tester.pump();
      expect(_activeNavigationIndex(tester), 1);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        craft.boardActive,
        isFalse,
        reason: 'A mode-only fact must not grant rich editing authority',
      );
      expect(craft.enterCalls, 0);
      expect(resolved, 1);
      craft.adoptGate!.complete(true);
      await tester.pump();
      expect(_activeNavigationIndex(tester), 1);
      expect(resolved, 1);
      craft.modeResolutionGate!.complete();
      await tester.pumpAndSettle();
      expect(resolved, 1);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'newer inactive mode supersedes passive Craft during rich adoption',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      craft.showInspectedMode(true);
      await tester.pump();
      expect(_activeNavigationIndex(tester), 1);
      craft.showInspectedMode(false);
      await tester.pump();
      expect(_activeNavigationIndex(tester), 0);
      expect(find.byType(HomeScreen), findsOneWidget);
      expect(craft.enterCalls, 0);
      expect(craft.leaveCalls, 0);
      craft.adoptGate!.complete(false);
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.enterCalls, 0);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'accepted active mode keeps its repair path after artwork failure',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      craft.showInspectedMode(true);
      await tester.pump();
      craft.adoptGate!.completeError(StateError('Artwork unavailable'));
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 1);
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        craft.enterCalls,
        0,
        reason: 'A mode-only route must never turn artwork repair into Enter',
      );
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'cold navigation accepts an edge tap across its full 48px target',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      final ink = find.descendant(
        of: find.byKey(const ValueKey('main-destination-setup')),
        matching: find.byType(InkWell),
      );
      expect(tester.getSize(ink), const Size(48, 48));
      await tester.tapAt(tester.getCenter(ink) + const Offset(23, 23));
      await tester.pump();
      expect(_activeNavigationIndex(tester), 2);
      craft.adoptGate!.complete(true);
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 2);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'explicit Play fences a late active inspection from the default slot',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..adoptGate = Completer<bool>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump();
      expect(find.byType(HomeScreen), findsOneWidget);
      craft.showInspectedMode(true);
      await tester.pump();
      expect(_activeNavigationIndex(tester), 0);
      craft.adoptGate!.complete(true);
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.enterCalls, 0);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final capabilityFailure in <bool>[false, true]) {
    testWidgets(
      'cold destination recovers after ${capabilityFailure ? 'capability' : 'inspection'} failure',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _RecoveryNavigationBle()
          ..capabilities = LitchDeviceCapabilities.unavailable
          ..failCapabilities = capabilityFailure;
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..failModeInspection = !capabilityFailure;
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const <LitchBoard>[],
              onDisconnect: service.disconnect,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byType(HomeScreen), findsOneWidget);
        expect(_activeNavigationIndex(tester), 0);
        expect(craft.enterCalls, 0);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  for (final page in <int>[2, 4]) {
    testWidgets(
      'explicit ${page == 2 ? 'Setup' : 'Console'} retains its context before rich adoption',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..adoptGate = Completer<bool>();
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const <LitchBoard>[],
              onDisconnect: service.disconnect,
              craftController: craft,
              craftLibrary: library,
              initialPageIndex: page,
              loadAppConsolePreference: () async => true,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pump();
        craft.showInspectedMode(true);
        await tester.pump();
        expect(_activeNavigationIndex(tester), page == 2 ? 2 : 3);
        expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
        craft.adoptGate!.complete(true);
        await tester.pumpAndSettle();
        expect(_activeNavigationIndex(tester), page == 2 ? 2 : 3);
        expect(craft.enterCalls, 0);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets('explicit navigation wins an in-flight board-mode probe', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(service)
      ..adoptGate = Completer<bool>();
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: service.disconnect,
          craftController: craft,
          craftLibrary: library,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pump();
    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pump();

    craft.adoptGate!.complete(true);
    await tester.pumpAndSettle();

    expect(_activeNavigationIndex(tester), 2);

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  for (final disconnectDuringProbe in [false, true]) {
    testWidgets(
      disconnectDuringProbe
          ? 'explicit Litchcraft entry survives a retired probe and reconnects once'
          : 'repeated Litchcraft selection during inactive probe enters once',
      (tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service)
          ..transientSelectionDuringAdopt = true
          ..adoptGate = Completer<bool>();
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pump();

        await tester.tap(_navigationAction(tester, 'litchcraft'));
        await tester.pump();
        await tester.tap(_navigationAction(tester, 'litchcraft'));
        await tester.pump();
        expect(craft.enterCalls, 1);

        final pendingProbe = craft.adoptGate!;
        if (disconnectDuringProbe) {
          service.emitConnection('board-1', DeviceConnectionState.disconnected);
          await tester.pump();
          pendingProbe.complete(false);
          await tester.pump();
          expect(
            craft.enterCalls,
            1,
            reason: 'A retired probe cannot add Enter on a disconnected link.',
          );
          craft.adoptGate = Completer<bool>();
          service.emitConnection('board-1', DeviceConnectionState.connected);
          await tester.pump();
          await tester.tap(_navigationAction(tester, 'litchcraft'));
          await tester.pump();
        }
        if (!disconnectDuringProbe) {
          pendingProbe.complete(false);
        } else {
          craft.adoptGate!.complete(false);
        }
        await tester.pumpAndSettle();

        expect(craft.enterCalls, disconnectDuringProbe ? 2 : 1);
        expect(
          _activeNavigationIndex(tester),
          1,
        ); // Repeated selection still Litchcraft.

        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      },
    );
  }

  testWidgets(
    'newer Play selection exits immediate Litchcraft entry while probe is pending',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..transientSelectionDuringAdopt = true
        ..adoptGate = Completer<bool>()
        ..exitGate = Completer<void>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pump();
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump();
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.leaveCalls, 1);

      craft.adoptGate!.complete(false);
      await tester.pump();
      expect(craft.enterCalls, 1);

      craft.exitGate!.complete();
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 1);
      expect(_activeNavigationIndex(tester), 0); // Newer Play intent wins.

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'Setup reselection supersedes Firmware navigation awaiting Craft Exit',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service)
        ..retained = true
        ..connectedBoardActive = true
        ..exitGate = Completer<void>();
      final library = LitchcraftLibrary(craft);
      await library.ready;
      addTearDown(() async {
        if (!craft.exitGate!.isCompleted) craft.exitGate!.complete();
        library.dispose();
        craft.dispose();
        await service.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 2,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware destination'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final leaf = find.byKey(const ValueKey('setup-open:behavior'));
      await tester.ensureVisible(leaf);
      await tester.tap(leaf);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(const ValueKey('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      final software = find.byIcon(Icons.system_update_outlined);
      await tester.ensureVisible(software);
      await tester.tap(software);
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 1);
      expect(_activeNavigationIndex(tester), 2);
      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('setup-common-controls')),
        findsOneWidget,
      );
      craft.exitGate!.complete();
      await tester.pumpAndSettle();
      expect(
        find.text('Firmware destination'),
        findsNothing,
        reason:
            'The latest Setup tap must supersede the older Firmware request.',
      );
      expect(_activeNavigationIndex(tester), 2);
      expect(
        find.byKey(const ValueKey('setup-common-controls')),
        findsOneWidget,
      );
      expect(craft.leaveCalls, 1);
      expect(craft.enterCalls, 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
    },
  );

  testWidgets(
    'Setup and Console retain Litchcraft mode until the user returns to Play',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
        'enable_app_console': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            loadAppConsolePreference: () async => true,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pumpAndSettle();

      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 1);
      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 0);
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 1);
      await tester.tap(_navigationAction(tester, 'console'));
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 0);
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 1);

      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final width in <double>[320, 390]) {
    for (final scale in <double>[1, 2]) {
      testWidgets('header keeps its visual height on $width px at $scale text', (
        tester,
      ) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = Size(width, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'litchcraft_discovery_dismissed': true,
        });
        final service = _ConnectionGateBleService();
        service.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(service);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: TextScaler.linear(scale)),
              child: child!,
            ),
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const <LitchBoard>[],
              onDisconnect: service.disconnect,
              craftController: craft,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        Finder wordmark(bool isCraft) => find.image(
          AssetImage(
            'assets/branding/foxie-${isCraft ? 'litchcraft' : 'litch'}-wordmark.png',
          ),
        );
        double visualHeight(Finder finder) =>
            tester.getBottomRight(finder).dy - tester.getTopLeft(finder).dy;
        final normalHeight = visualHeight(wordmark(false));
        final double normalToolbarHeight = tester
            .getSize(find.byType(AppBar))
            .height;
        expect(
          normalHeight,
          moreOrLessEquals(23),
          reason:
              'Portrait artwork keeps its designed height; status text must fit around it.',
        );
        final status = find.byKey(
          const ValueKey('connected-board-status-action'),
        );
        expect(
          tester.getRect(wordmark(false)).center.dy,
          closeTo(tester.getRect(status).center.dy, .1),
          reason: 'Branding and Connected remain on one portrait row.',
        );
        final connectedText = find.descendant(
          of: status,
          matching: find.text('Connected'),
        );
        final paragraph = tester.renderObject<RenderParagraph>(connectedText);
        expect(paragraph.maxLines, 1);
        expect(paragraph.softWrap, isFalse);
        expect(paragraph.didExceedMaxLines, isFalse);
        expect(
          paragraph
              .getBoxesForSelection(
                const TextSelection(baseOffset: 0, extentOffset: 9),
              )
              .map((box) => box.top)
              .toSet(),
          hasLength(1),
        );
        expect(
          tester.getRect(connectedText).left,
          greaterThanOrEqualTo(tester.getRect(wordmark(false)).right),
        );

        await _selectAdjacentMainPage(tester, forward: true);
        await decodeVisibleReviewImages(tester);
        expect(wordmark(true), findsOneWidget);
        expect(wordmark(false), findsNothing);
        expect(visualHeight(wordmark(true)), moreOrLessEquals(normalHeight));
        expect(
          tester.getSize(find.byType(AppBar)).height,
          moreOrLessEquals(normalToolbarHeight),
        );
        expect(find.text('Connected'), findsOneWidget);
        expect(
          find.byKey(const ValueKey('connected-board-status-action')),
          findsOneWidget,
        );
        await captureHealthReview(tester, 'craft-header-$width-$scale');

        await tester.tap(_navigationAction(tester, 'play'));
        await tester.pump();
        expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        expect(visualHeight(wordmark(false)), moreOrLessEquals(normalHeight));
        await captureHealthReview(tester, 'play-header-$width-$scale');
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        craft.dispose();
        await service.dispose();
      });

      testWidgets(
        'offline header keeps its visual height on $width px at $scale text',
        (tester) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          tester.view.physicalSize = Size(width, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final service = _ConnectionGateBleService();
          addTearDown(service.dispose);
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(scale)),
                child: _withTestPreferences(child!),
              ),
              home: ConnectionGateScreen(bleService: service),
            ),
          );
          await tester.runAsync(() async {
            final context = tester.element(find.byType(ConnectionGateScreen));
            for (final asset in <String>[
              'foxie-litch-wordmark.png',
              'foxie-litchcraft-wordmark.png',
              'foxie-fox.png',
            ]) {
              await precacheImage(
                AssetImage('assets/branding/$asset'),
                context,
              );
            }
          });
          await tester.pump(const Duration(milliseconds: 300));
          Finder wordmark(bool isCraft) => find.image(
            AssetImage(
              'assets/branding/foxie-${isCraft ? 'litchcraft' : 'litch'}-wordmark.png',
            ),
          );
          double visualHeight(Finder finder) =>
              tester.getBottomRight(finder).dy - tester.getTopLeft(finder).dy;
          final normalHeight = visualHeight(wordmark(false));
          expect(normalHeight, moreOrLessEquals(23));
          Future<void> select(String label) async {
            await tester.tap(_navigationAction(tester, label.toLowerCase()));
            await tester.pump(const Duration(milliseconds: 300));
            await tester.pump();
          }

          await select('Litchcraft');
          expect(wordmark(true), findsOneWidget);
          expect(wordmark(false), findsNothing);
          expect(visualHeight(wordmark(true)), moreOrLessEquals(normalHeight));
          await captureHealthReview(
            tester,
            'offline-craft-header-$width-$scale',
          );
          await select('Play');
          expect(visualHeight(wordmark(false)), moreOrLessEquals(normalHeight));
          await select('Setup');
          expect(visualHeight(wordmark(false)), moreOrLessEquals(normalHeight));
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox());
        },
      );
    }
  }

  testWidgets('Litchcraft wordmark keeps its size across connection state', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});

    final offline = _ConnectionGateBleService();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: ConnectionGateScreen(bleService: offline),
      ),
    );
    await tester.tap(_navigationAction(tester, 'litchcraft'));
    await tester.pump(const Duration(milliseconds: 400));
    await decodeVisibleReviewImages(tester);
    final wordmark = find.image(
      const AssetImage('assets/branding/foxie-litchcraft-wordmark.png'),
    );
    double visibleHeight() =>
        tester.getBottomRight(wordmark).dy - tester.getTopLeft(wordmark).dy;
    final offlineHeight = visibleHeight();
    expect(find.text('Not connected'), findsOneWidget);
    await captureHealthReview(tester, 'litchcraft-header-offline-390');

    await tester.pumpWidget(const SizedBox.shrink());
    await offline.dispose();
    final connected = _ConnectionGateBleService();
    connected.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _NavigationCraftController(connected);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: LitchMainScaffold(
          bleService: connected,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: connected.disconnect,
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 0,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);

    expect(visibleHeight(), moreOrLessEquals(offlineHeight));
    expect(find.text('Connected'), findsOneWidget);
    expect(
      find.byKey(const ValueKey('connected-board-status-action')),
      findsOneWidget,
    );
    await captureHealthReview(tester, 'litchcraft-header-connected-390');
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await library.flush();
    library.dispose();
    craft.dispose();
    await connected.dispose();
  });

  for (final insets in <EdgeInsets>[
    const EdgeInsets.fromLTRB(59, 0, 59, 21),
    const EdgeInsets.fromLTRB(59, 0, 12, 21),
    const EdgeInsets.fromLTRB(12, 0, 59, 21),
    const EdgeInsets.fromLTRB(44, 0, 0, 24),
    const EdgeInsets.fromLTRB(0, 0, 44, 24),
  ]) {
    testWidgets(
      'landscape mini navigation keeps one rectangle across connection and views ($insets)',
      (tester) async {
        tester.view.physicalSize = const Size(852, 393);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{});

        var keyboardHeight = 0.0;
        Widget withPhoneInsets(Widget child) => MediaQuery(
          data: MediaQueryData(
            size: const Size(852, 393),
            padding: keyboardHeight > 0 ? insets.copyWith(bottom: 0) : insets,
            viewPadding: insets,
            viewInsets: EdgeInsets.only(bottom: keyboardHeight),
          ),
          child: child,
        );
        final platform = insets.left == 59 || insets.right == 59
            ? TargetPlatform.iOS
            : TargetPlatform.android;
        const ordinaryGap = LitchWorkspaceGeometry.landscapeGap;

        final offline = _ConnectionGateBleService();
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchDarkTheme().copyWith(platform: platform),
            builder: (context, child) => withPhoneInsets(
              OrientationWorkspace(
                sideReader: () async => DeviceLandscapeSide.right,
                child: child!,
              ),
            ),
            home: ConnectionGateScreen(bleService: offline),
          ),
        );
        await tester.pump();
        final Rect offlinePlay = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        final verticalClearance = math.max(insets.top, insets.bottom);
        var safeRect = Rect.fromLTRB(
          insets.left,
          verticalClearance,
          852 - (platform == TargetPlatform.iOS ? 0 : insets.right),
          393 - verticalClearance,
        );
        void expectSafe(Rect rect) {
          expect(rect.left, greaterThanOrEqualTo(safeRect.left));
          expect(rect.right, lessThanOrEqualTo(safeRect.right));
          expect(rect.top, greaterThanOrEqualTo(safeRect.top));
          expect(rect.bottom, lessThanOrEqualTo(safeRect.bottom));
        }

        void expectSelectedDestination(String selected) {
          for (final name in ['play', 'craft', 'setup', 'console']) {
            final action = find.byKey(ValueKey('landscape-$name-action'));
            if (action.evaluate().isEmpty) continue;
            final semantics = tester.widget<Semantics>(
              find
                  .descendant(of: action, matching: find.byType(Semantics))
                  .first,
            );
            expect(semantics.properties.selected, name == selected);
            final ink = find.descendant(of: action, matching: find.byType(Ink));
            final selection = tester.widget<Ink>(ink);
            expect(tester.getSize(ink), const Size(44, 36));
            expect(
              (selection.decoration! as BoxDecoration).color,
              name == selected
                  ? Theme.of(
                      tester.element(action),
                    ).colorScheme.secondaryContainer
                  : Colors.transparent,
            );
            final capsule = tester.widget<Material>(
              find.ancestor(of: action, matching: find.byType(Material)).first,
            );
            expect(
              capsule.color,
              Theme.of(tester.element(action)).colorScheme.surfaceContainer,
            );
            expect(
              capsule.borderRadius,
              BorderRadius.circular(LitchShapes.panelRadius),
            );
          }
        }

        expectSafe(offlinePlay);
        expectSelectedDestination('play');
        await tester.tap(find.byKey(const ValueKey('landscape-craft-action')));
        await tester.pumpAndSettle();
        expectSelectedDestination('craft');
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          offlinePlay,
        );
        await tester.tap(find.byKey(const ValueKey('landscape-setup-action')));
        await tester.pumpAndSettle();
        expectSelectedDestination('setup');
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          offlinePlay,
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await offline.dispose();

        final connected = _ConnectionGateBleService();
        connected.emitConnection('board-1', DeviceConnectionState.connected);
        final craft = _NavigationCraftController(connected);
        final library = LitchcraftLibrary(craft);
        await library.ready;
        var ribbonSide = Alignment.centerRight;
        Widget connectedApp() => MaterialApp(
          theme: buildLitchDarkTheme().copyWith(platform: platform),
          builder: (context, child) => withPhoneInsets(
            OrientationWorkspace(
              sideReader: () async => ribbonSide.x < 0
                  ? DeviceLandscapeSide.left
                  : DeviceLandscapeSide.right,
              child: child!,
            ),
          ),
          home: LitchMainScaffold(
            bleService: connected,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: connected.disconnect,
            craftController: craft,
            craftLibrary: library,
            landscapeRibbonSide: ribbonSide,
            loadAppConsolePreference: () async => true,
            initialPageIndex: 1,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        );
        await tester.pumpWidget(connectedApp());
        await tester.pumpAndSettle();
        final Rect connectedPlay = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        expectSelectedDestination('play');
        expect(connectedPlay, offlinePlay);
        expect(
          connectedPlay.right,
          closeTo(math.min(safeRect.right, 852 - ordinaryGap), .01),
          reason: 'navigation uses the shared inset from the clear usable edge',
        );
        final Rect playBoard = tester.getRect(
          find.byKey(const ValueKey('home-chessboard-surface')),
        );
        expectSafe(playBoard);
        expect(
          playBoard.center.dy,
          closeTo(safeRect.center.dy, .01),
          reason: 'the visible Play board centers on the physical display',
        );

        await tester.tap(find.byKey(const ValueKey('landscape-craft-action')));
        await tester.pumpAndSettle();
        final Rect connectedCraft = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        expectSelectedDestination('craft');
        final scaffoldMedia = MediaQuery.of(
          tester.element(find.byType(LitchMainScaffold)),
        );
        expect(
          connectedCraft,
          connectedPlay,
          reason:
              'all views share media size=${scaffoldMedia.size}, padding=${scaffoldMedia.padding}, viewPadding=${scaffoldMedia.viewPadding}',
        );
        final Rect craftBoard = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        expectSafe(craftBoard);
        expect(
          tester
              .getRect(find.byKey(const ValueKey('craft-persistent-inspector')))
              .right,
          closeTo(math.min(safeRect.right, 852 - ordinaryGap), .01),
        );
        expect(
          tester
              .getRect(find.byKey(const ValueKey('craft-quick-actions')))
              .left,
          closeTo(math.max(safeRect.left, ordinaryGap), .01),
        );
        expect(
          connectedCraft.bottom,
          closeTo(math.min(safeRect.bottom, 393 - ordinaryGap), .01),
        );
        expect(
          tester
              .getRect(find.byKey(const ValueKey('craft-persistent-inspector')))
              .top,
          closeTo(math.max(safeRect.top, ordinaryGap), .01),
        );
        expect(
          craftBoard.center.dy,
          closeTo(safeRect.center.dy, .01),
          reason: 'the visible canvas centers on the physical display',
        );
        expectSafe(
          tester.getRect(find.byKey(const ValueKey('craft-quick-fill'))),
        );
        expectSafe(
          tester.getRect(
            find.byKey(const ValueKey('craft-persistent-inspector')),
          ),
        );
        expect(craftBoard.size, playBoard.size);
        expect(craftBoard.center.dy, playBoard.center.dy);
        expect(
          playBoard.left,
          closeTo(math.max(safeRect.left, ordinaryGap), .01),
          reason:
              'Play has no tool rail, so its full-size board stays nearest the outside edge and leaves the rest to status.',
        );
        await tester.tap(find.byKey(const ValueKey('landscape-setup-action')));
        await tester.pumpAndSettle();
        expectSelectedDestination('setup');
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          connectedPlay,
        );
        await tester.tap(
          find.byKey(const ValueKey('landscape-console-action')),
        );
        await tester.pumpAndSettle();
        expectSelectedDestination('console');
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          connectedPlay,
        );
        expect(tester.takeException(), isNull);
        expectSafe(
          tester.getRect(find.byKey(const ValueKey('console-command-row'))),
        );
        expectSafe(
          tester.getRect(find.byKey(const ValueKey('console-command-input'))),
        );

        final consoleInput = find.byKey(
          const ValueKey('console-command-input'),
        );
        final beforeKeyboardInput = tester.getRect(consoleInput);
        final beforeKeyboardMedia = MediaQuery.of(
          tester.element(find.byType(LitchMainScaffold)),
        );
        keyboardHeight = 170;
        await tester.pumpWidget(connectedApp());
        await tester.pumpAndSettle();
        final keyboardMedia = MediaQuery.of(
          tester.element(find.byType(LitchMainScaffold)),
        );
        expect(keyboardMedia.padding, beforeKeyboardMedia.padding);
        expect(keyboardMedia.viewPadding, beforeKeyboardMedia.viewPadding);
        expect(keyboardMedia.viewInsets.bottom, 170);
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          connectedPlay,
          reason:
              'keyboard intrusion cannot change permanent workspace geometry',
        );
        final keyboardInput = tester.getRect(consoleInput);
        expect(keyboardInput.left, beforeKeyboardInput.left);
        expect(keyboardInput.right, beforeKeyboardInput.right);
        expect(keyboardInput.bottom, lessThanOrEqualTo(393 - keyboardHeight));
        keyboardHeight = 0;
        await tester.pumpWidget(connectedApp());
        await tester.pumpAndSettle();
        expect(tester.getRect(consoleInput), beforeKeyboardInput);
        expect(tester.takeException(), isNull);

        // A physical half-turn moves the iOS camera inset with native
        // orientation; the clear edge matches portrait. Android retains both
        // actual cutout insets.
        ribbonSide = Alignment.centerLeft;
        safeRect = Rect.fromLTRB(
          platform == TargetPlatform.iOS ? 0 : insets.left,
          verticalClearance,
          852 - insets.right,
          393 - verticalClearance,
        );
        await tester.pumpWidget(connectedApp());
        tester.binding.handleMetricsChanged();
        await tester.tap(find.byKey(const ValueKey('landscape-play-action')));
        await tester.pumpAndSettle();
        final leftNavigation = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        expectSafe(leftNavigation);
        expect(leftNavigation.left, math.max(safeRect.left, ordinaryGap));
        final oppositePlay = tester.getRect(
          find.byKey(const ValueKey('home-chessboard-surface')),
        );
        expectSafe(oppositePlay);
        expect(oppositePlay.center.dy, closeTo(393 / 2, .01));
        expect(oppositePlay.right, math.min(safeRect.right, 852 - ordinaryGap));
        await tester.tap(find.byKey(const ValueKey('landscape-craft-action')));
        await tester.pumpAndSettle();
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          leftNavigation,
        );
        final oppositeCraft = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        expectSafe(oppositeCraft);
        expect(oppositeCraft.center.dy, closeTo(393 / 2, .01));
        expect(
          tester
              .getRect(find.byKey(const ValueKey('craft-persistent-inspector')))
              .left,
          closeTo(math.max(safeRect.left, ordinaryGap), .01),
        );
        expect(
          tester
              .getRect(find.byKey(const ValueKey('craft-quick-actions')))
              .right,
          closeTo(math.min(safeRect.right, 852 - ordinaryGap), .01),
        );
        expectSafe(
          tester.getRect(find.byKey(const ValueKey('craft-quick-fill'))),
        );
        expect(oppositeCraft.width, closeTo(oppositePlay.width, .01));
        expect(oppositeCraft.height, closeTo(oppositePlay.height, .01));
        expect(oppositeCraft.center.dy, oppositePlay.center.dy);
        expectSafe(
          tester.getRect(
            find.byKey(const ValueKey('craft-persistent-inspector')),
          ),
        );
        expect(tester.takeException(), isNull);

        await tester.pumpWidget(const SizedBox.shrink());
        await library.flush();
        library.dispose();
        craft.dispose();
        await connected.dispose();
      },
    );
  }

  testWidgets('offline destinations keep Litchcraft between Play and Setup', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        builder: (_, child) =>
            _withTestPreferences(RepaintBoundary(child: child!)),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();

    expect(_activeNavigation(tester), findsOneWidget);
    expect(_activeNavigationIndex(tester), 0);
    expect(find.text('Paint in Litchcraft'), findsNothing);
    expect(find.text('Looking for the undead...'), findsOneWidget);
    expect(find.text('Play'), findsOneWidget);
    expect(find.text('Setup'), findsOneWidget);

    Future<void> assertNavigationDragStays(int expected) async {
      final bounds = tester.getRect(_activeNavigation(tester));
      for (final direction in [-1, 1]) {
        final pointer = await tester.startGesture(bounds.center);
        for (var step = 0; step < 4; step++) {
          await pointer.moveBy(Offset(direction * 30, 0));
          await tester.pump(const Duration(milliseconds: 40));
        }
        await pointer.up();
        await tester.pump(const Duration(milliseconds: 300));
        expect(_activeNavigationIndex(tester), expected);
      }
    }

    await assertNavigationDragStays(0);
    await tester.tap(_navigationAction(tester, 'litchcraft'));
    await tester.pump();
    expect(find.byType(LitchcraftScreen), findsOneWidget);
    expect(_activeNavigationIndex(tester), 1);

    await assertNavigationDragStays(1);

    await tester.tap(_navigationAction(tester, 'play'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byType(LitchcraftScreen), findsNothing);
    expect(find.text('Looking for the undead...'), findsOneWidget);
    await tester.runAsync(() async {
      await precacheImage(
        const AssetImage('assets/branding/foxie-litch-wordmark.png'),
        tester.element(find.byType(ConnectionGateScreen)),
      );
    });
    await tester.pump(const Duration(milliseconds: 100));
    await captureHealthReview(tester, 'offline-play-android-390');

    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pumpAndSettle();
    expect(find.byType(AppPreferences), findsOneWidget);
    await captureHealthReview(tester, 'offline-setup-android-390');
  });

  testWidgets('iOS phone screenshot reference matrix', (tester) async {
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    tester.view.devicePixelRatio = 1;
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await loadHealthReviewFonts(tester, useSdkFonts: true);

    for (final layout in <(String, Size, EdgeInsets, Alignment)>[
      (
        'portrait',
        const Size(393, 852),
        const EdgeInsets.fromLTRB(0, 59, 0, 34),
        Alignment.centerRight,
      ),
      (
        'landscape-right',
        const Size(852, 393),
        const EdgeInsets.fromLTRB(59, 0, 0, 21),
        Alignment.centerRight,
      ),
      (
        'landscape-left',
        const Size(852, 393),
        const EdgeInsets.fromLTRB(0, 0, 59, 21),
        Alignment.centerLeft,
      ),
    ]) {
      tester.view.physicalSize = layout.$2;
      final boundaryKey = ValueKey<String>('ios-matrix-${layout.$1}');
      Widget shell(Widget child) => MaterialApp(
        theme: buildLitchDarkTheme().copyWith(platform: TargetPlatform.iOS),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(padding: layout.$3, viewPadding: layout.$3),
          child: RepaintBoundary(key: boundaryKey, child: child!),
        ),
        home: child,
      );

      final connected = _ConnectionGateBleService();
      connected.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(connected)
        ..forceLiveStatus = true
        ..simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        shell(
          LitchMainScaffold(
            bleService: connected,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: connected.disconnect,
            craftController: craft,
            craftLibrary: library,
            landscapeRibbonSide: layout.$4,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await decodeVisibleReviewImages(tester);
      final safeRect = Rect.fromLTRB(
        layout.$3.left,
        layout.$3.top,
        layout.$2.width - layout.$3.right,
        layout.$2.height - layout.$3.bottom,
      );
      void expectInsideSafeViewport(Finder finder) {
        final rect = tester.getRect(finder);
        expect(rect.left, greaterThanOrEqualTo(safeRect.left));
        expect(rect.right, lessThanOrEqualTo(safeRect.right));
        expect(rect.top, greaterThanOrEqualTo(safeRect.top));
        expect(rect.bottom, lessThanOrEqualTo(safeRect.bottom));
      }

      expectInsideSafeViewport(find.byKey(const ValueKey('litchcraft-canvas')));
      expectInsideSafeViewport(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      await captureHealthReview(
        tester,
        'ios-litchcraft-${layout.$1}',
        boundaryFinder: find.byKey(boundaryKey),
      );

      await tester.pumpWidget(
        shell(
          LitchMainScaffold(
            key: ValueKey<String>('ios-play-${layout.$1}'),
            bleService: connected,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: connected.disconnect,
            craftController: craft,
            craftLibrary: library,
            landscapeRibbonSide: layout.$4,
            initialPageIndex: 1,
            initialCraftModeResolved: true,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await decodeVisibleReviewImages(tester);
      expectInsideSafeViewport(
        find.byKey(const ValueKey('home-chessboard-surface')),
      );
      await captureHealthReview(
        tester,
        'ios-chess-${layout.$1}',
        boundaryFinder: find.byKey(boundaryKey),
      );
      await tester.pumpWidget(const SizedBox.shrink());

      final offline = _ConnectionGateBleService();
      await tester.pumpWidget(shell(ConnectionGateScreen(bleService: offline)));
      await tester.pump();
      final setup = layout.$2.width > layout.$2.height
          ? find.byKey(const ValueKey<String>('landscape-setup-action'))
          : find.bySemanticsLabel(RegExp(r'^Setup'));
      expect(setup.hitTestable(), findsOneWidget);
      await tester.tap(setup.hitTestable());
      await tester.pumpAndSettle();
      await captureHealthReview(
        tester,
        'ios-setup-${layout.$1}',
        boundaryFinder: find.byKey(boundaryKey),
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await offline.dispose();
      await library.flush();
      library.dispose();
      craft.dispose();
      await connected.dispose();
    }
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'Android landscape offline Play keeps recovery boards and games clear of system navigation',
    (WidgetTester tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(1000, 472);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      final repository = _EmptyGameLibraryRepository(
        snapshot: GameLibrarySnapshot(
          games: <GameArchive>[
            _recentArchive(1),
            _recentArchive(2),
            _recentArchive(3),
          ],
        ),
      );
      addTearDown(repository.close);
      const EdgeInsets systemInsets = EdgeInsets.only(top: 24, right: 32);

      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (BuildContext context, Widget? child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(padding: systemInsets, viewPadding: systemInsets),
            child: RepaintBoundary(
              key: const ValueKey<String>(
                'offline-play-landscape-review-boundary',
              ),
              child: child!,
            ),
          ),
          home: ConnectionGateScreen(
            bleService: service,
            gameLibrary: repository,
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));

      final Finder scaffoldFinder = find.byType(Scaffold).first;
      final Finder contentFinder = find.byKey(
        const ValueKey<String>('connection-gate-content'),
      );
      final Finder discoveryScrollFinder = find.byKey(
        const PageStorageKey<String>('disconnected-play-scroll'),
      );
      final Finder gamesAction = find.byKey(
        const ValueKey<String>('discovery-review-games'),
      );
      final Finder railFinder = find.byKey(
        const ValueKey<String>('disconnected-play-landscape-action-rail'),
      );

      void expectCompactShell({required bool ordinaryContentFits}) {
        expect(railFinder, findsOneWidget);
        expect(find.byType(AppBar), findsNothing);
        expect(find.byType(NavigationBar), findsNothing);
        expect(
          find.byTooltip('Disconnected. Select a board to connect.'),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey('home-chessboard-surface')),
          findsNothing,
        );
        final Rect scaffold = tester.getRect(scaffoldFinder);
        final Rect content = tester.getRect(contentFinder);
        final Rect rail = tester.getRect(railFinder);
        expect(content.right, lessThanOrEqualTo(rail.left));
        expect(gamesAction, findsOneWidget);
        expect(tester.getSize(gamesAction).height, greaterThanOrEqualTo(48));
        expect(
          rail.right,
          lessThanOrEqualTo(scaffold.right - systemInsets.right),
        );
        expect(rail.top, greaterThanOrEqualTo(systemInsets.top));
        for (final (key, tooltip) in <(String, String)>[
          ('landscape-craft-action', 'Litchcraft'),
          ('landscape-setup-action', 'Setup'),
        ]) {
          final Finder action = find.byKey(ValueKey<String>(key));
          expect(action, findsOneWidget);
          expect(find.byTooltip(tooltip), findsOneWidget);
          expect(tester.getSize(action).width, greaterThanOrEqualTo(48));
          expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
        }
        expect(
          find.byKey(const ValueKey<String>('landscape-play-action')),
          findsOneWidget,
          reason: 'The fixed mini navigator retains the current destination.',
        );
        if (ordinaryContentFits) {
          expect(
            tester
                .state<ScrollableState>(
                  find
                      .descendant(
                        of: discoveryScrollFinder,
                        matching: find.byType(Scrollable),
                      )
                      .first,
                )
                .position
                .maxScrollExtent,
            0,
          );
        }
        expect(tester.takeException(), isNull);
      }

      expect(find.text('Looking for the undead...'), findsOneWidget);
      expectCompactShell(ordinaryContentFits: true);
      await tester.tap(gamesAction);
      await tester.pumpAndSettle();
      expect(find.byType(GameLibraryScreen), findsOneWidget);
      await tester.pageBack();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.byType(GameLibraryScreen), findsNothing);
      expectCompactShell(ordinaryContentFits: true);

      service.emitAvailability(BleAvailability.bluetoothPermissionDenied);
      await tester.pump();
      expect(
        find.text('Allow Nearby devices and Bluetooth access to find boards.'),
        findsOneWidget,
      );
      final Finder recovery = find.byKey(
        const ValueKey<String>('bluetooth-recovery'),
      );
      expect(recovery, findsOneWidget);
      expect(tester.getSize(recovery).height, greaterThanOrEqualTo(48));
      expectCompactShell(ordinaryContentFits: true);
      await captureHealthReview(
        tester,
        'offline-play-permission-landscape-1000x472-right-inset',
        boundaryFinder: find.byKey(
          const ValueKey<String>('offline-play-landscape-review-boundary'),
        ),
      );

      service.emitAvailability(BleAvailability.ready);
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -45),
      ]);
      await tester.pump();
      for (final String id in <String>['board-1', 'board-2']) {
        final Finder board = find.byKey(
          ValueKey<String>('discovered-board-$id'),
        );
        expect(board, findsOneWidget);
        expect(
          tester.getRect(board).right,
          lessThanOrEqualTo(tester.getRect(railFinder).left),
        );
      }
      expectCompactShell(ordinaryContentFits: true);

      await tester.tap(
        find.byKey(const ValueKey<String>('landscape-setup-action')),
      );
      await tester.pumpAndSettle();
      expect(find.byType(AppBar), findsNothing);
      expect(find.byType(NavigationBar), findsNothing);
      expect(find.byType(AppPreferences), findsOneWidget);
      expect(
        find.byTooltip('Disconnected. Select a board to connect.'),
        findsOneWidget,
      );
      final Finder setupRailFinder = find.byKey(
        const ValueKey<String>('disconnected-setup-landscape-action-rail'),
      );
      final Finder setupScrollFinder = find.byKey(
        const ValueKey<String>('offline-setup-scroll'),
      );
      expect(setupRailFinder, findsOneWidget);
      expect(setupScrollFinder, findsOneWidget);
      final Rect setupRail = tester.getRect(setupRailFinder);
      final Rect setupScroll = tester.getRect(setupScrollFinder);
      final Rect scaffold = tester.getRect(find.byType(Scaffold).first);
      expect(setupScroll.right, lessThanOrEqualTo(setupRail.left));
      expect(
        setupRail.right,
        lessThanOrEqualTo(scaffold.right - systemInsets.right),
      );
      expect(setupRail.top, greaterThanOrEqualTo(systemInsets.top));
      expect(
        find.descendant(
          of: setupScrollFinder,
          matching: find.byType(Scrollable),
        ),
        findsOneWidget,
      );
      for (final (key, tooltip) in <(String, String)>[
        ('landscape-craft-action', 'Litchcraft'),
        ('landscape-play-action', 'Play'),
      ]) {
        final Finder action = find.byKey(ValueKey<String>(key));
        expect(action, findsOneWidget);
        expect(find.byTooltip(tooltip), findsOneWidget);
        expect(tester.getSize(action).width, greaterThanOrEqualTo(48));
        expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
      }
      expect(
        find.byKey(const ValueKey<String>('landscape-setup-action')),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.pump();
      await captureHealthReview(
        tester,
        'offline-setup-compact-landscape-1000x472-right-inset',
        boundaryFinder: find.byKey(
          const ValueKey<String>('offline-play-landscape-review-boundary'),
        ),
      );
    },
  );

  testWidgets('offline Setup fits 320px at 200 percent text', (tester) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: TextScaler.linear(2)),
          child: _withTestPreferences(RepaintBoundary(child: child!)),
        ),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();
    tester
        .widget<NavigationBar>(
          find.byKey(const ValueKey<String>('disconnected-navigation')),
        )
        .onDestinationSelected!(2);
    await tester.pumpAndSettle();
    expect(find.byType(AppPreferences), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.runAsync(() async {
      await precacheImage(
        const AssetImage('assets/branding/foxie-litch-wordmark.png'),
        tester.element(find.byType(ConnectionGateScreen)),
      );
    });
    await tester.pump(const Duration(milliseconds: 100));
    await captureHealthReview(tester, 'offline-setup-android-320-200');
    final support = find.byKey(const ValueKey('open-contact-support'));
    expect(support, findsOneWidget);
    await tester.ensureVisible(support);
    await tester.pumpAndSettle();
    expect(tester.getSize(support).height, greaterThanOrEqualTo(48));
    await tester.tap(support);
    await tester.pump();
    await tester.pump(const Duration(seconds: 3));
    await tester.pumpAndSettle();
    final list = find.byKey(const ValueKey('support-report-list'));
    expect(list, findsOneWidget);
    final body = find.byKey(const ValueKey('support-report-body'));
    await tester.scrollUntilVisible(
      body,
      300,
      scrollable: find
          .descendant(of: list, matching: find.byType(Scrollable))
          .first,
    );
    final draft = tester.widget<TextField>(body).controller!.text;
    expect(draft, contains('board_connection: disconnected'));
    await tester.enterText(body, '$draft\nOffline support question');
    expect(
      tester.widget<TextField>(body).controller!.text,
      contains('Offline support question'),
    );
    expect(service.connectCalls, isEmpty);
    await tester.pageBack();
    await tester.pumpAndSettle();
    expect(_activeNavigationIndex(tester), 2);
    expect(tester.takeException(), isNull);
  });

  testWidgets('auto-connect grace leaves manual board actions available', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();
    service.emitBoards(const <LitchBoard>[
      LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
    ]);
    await tester.pump();

    final connect = tester.widget<FilledButton>(
      find.widgetWithText(FilledButton, 'Connect'),
    );
    expect(connect.onPressed, isNotNull);
    final identifyButtons = tester.widgetList<OutlinedButton>(
      find.widgetWithText(OutlinedButton, 'Identify'),
    );
    expect(identifyButtons, hasLength(1));
    expect(identifyButtons.every((button) => button.onPressed != null), isTrue);
    expect(find.textContaining('RSSI'), findsNothing);

    await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
    await tester.pump();
    expect(service.connectCalls, <String>['board-1']);
    service.completeNextConnect(false);
    await tester.pump();
  });

  for (final destination in <(String, int)>[('Litchcraft', 1), ('Setup', 2)]) {
    testWidgets(
      'auto-connect remains owned by the shell while ${destination.$1} is visible',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.pump();
        tester
            .widget<NavigationBar>(
              find.byKey(const ValueKey<String>('disconnected-navigation')),
            )
            .onDestinationSelected!(destination.$2);
        await tester.pump();
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        ]);
        await tester.pump(const Duration(seconds: 2));

        expect(service.connectCalls, <String>['board-1']);
      },
    );
  }

  testWidgets(
    'multi-board roster stays stable through advertisements and Identify restart',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      final repository = _EmptyGameLibraryRepository();
      addTearDown(repository.close);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          builder: (_, child) => RepaintBoundary(child: child!),
          home: ConnectionGateScreen(
            bleService: service,
            gameLibrary: repository,
          ),
        ),
      );
      await tester.pump();
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-b', name: 'Same Litch', rssi: -40),
        LitchBoard(id: 'board-a', name: 'Same Litch', rssi: -45),
      ]);
      await tester.pump();
      expect(service.identifyCalls, isEmpty, reason: 'Identify is user-owned');

      Finder card(String id) =>
          find.byKey(ValueKey<String>('discovered-board-$id'));
      expect(
        find.descendant(of: card('board-b'), matching: find.text('#1')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: card('board-a'), matching: find.text('#2')),
        findsOneWidget,
      );

      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-a', name: 'Same Litch', rssi: -20),
        LitchBoard(id: 'board-b', name: 'Same Litch', rssi: -80),
        LitchBoard(id: 'board-c', name: 'Earlier name', rssi: -30),
      ]);
      await tester.pump();
      expect(
        find.descendant(of: card('board-b'), matching: find.text('#1')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: card('board-a'), matching: find.text('#2')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: card('board-c'), matching: find.text('#3')),
        findsOneWidget,
      );
      expect(
        find.widgetWithText(OutlinedButton, 'Identify all'),
        findsOneWidget,
      );
      expect(find.text('Same Litch'), findsNWidgets(2));
      expect(find.textContaining('ard-b'), findsNothing);
      await decodeVisibleReviewImages(tester);
      await captureHealthReview(tester, 'multi-board-stable-android-390');

      service.deferIdentify = true;
      await tester.tap(find.widgetWithText(OutlinedButton, 'Identify all'));
      await tester.pump();
      expect(service.identifyCalls.single.deviceId, 'board-b');
      service.emitBoards(const <LitchBoard>[]);
      await tester.pump();
      expect(card('board-b'), findsOneWidget);
      expect(card('board-a'), findsOneWidget);
      expect(card('board-c'), findsOneWidget);

      final connect = find.descendant(
        of: card('board-a'),
        matching: find.widgetWithText(FilledButton, 'Connect'),
      );
      expect(connect.hitTestable(), findsOneWidget);
      await tester.tap(connect);
      await tester.pump();
      expect(service.connectCalls, isEmpty, reason: 'Connect waits safely');
      expect(find.byType(HomeScreen), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('connection-gate-content')),
        findsOneWidget,
      );
      service.completeNextIdentify();
      await tester.pump();
      expect(
        service.identifyCalls,
        isEmpty,
        reason: 'Connect retires unstarted Identify-all work after cleanup.',
      );
      expect(find.textContaining('Identify sent to'), findsNothing);
      expect(service.connectCalls, <String>['board-a']);
      service.completeNextConnect(false);
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
      await tester.pump();

      await tester.tap(find.widgetWithText(OutlinedButton, 'Identify all'));
      await tester.pump();
      for (final id in ['board-b', 'board-a', 'board-c']) {
        expect(service.identifyCalls.single.deviceId, id);
        service.completeNextIdentify();
        await tester.pump();
      }
      expect(service.identifyCalls, isEmpty);
      expect(find.text('Identify sent to all boards.'), findsOneWidget);
      ScaffoldMessenger.of(
        tester.element(find.byType(ConnectionGateScreen)),
      ).removeCurrentSnackBar();
      await tester.pump();
      expect(find.textContaining('Identify sent to'), findsNothing);

      await tester.tap(find.widgetWithText(OutlinedButton, 'Identify all'));
      await tester.pump();
      expect(service.identifyCalls.single.deviceId, 'board-b');
      _sendAppToBackground(tester);
      await tester.pump();
      _sendAppToForeground(tester);
      await tester.pump();
      service.completeNextIdentify();
      await tester.pump();
      expect(
        service.identifyCalls,
        isEmpty,
        reason: 'A retired Identify-all loop cannot resume with the app.',
      );
      expect(find.textContaining('Identify sent to'), findsNothing);
    },
  );

  testWidgets(
    'Identify All reports partial failure and continues through the snapshot',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService()..deferIdentify = true;
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-a', name: 'A', rssi: -40),
        LitchBoard(id: 'board-b', name: 'B', rssi: -50),
        LitchBoard(id: 'board-c', name: 'C', rssi: -60),
      ]);
      await tester.pump();
      await tester.tap(find.widgetWithText(OutlinedButton, 'Identify all'));
      await tester.pump();
      for (var index = 0; index < 3; ++index) {
        expect(
          service.identifyCalls.single.deviceId,
          ['board-a', 'board-b', 'board-c'][index],
        );
        expect(service.identifyCalls.single.number, index + 1);
        service.completeNextIdentify(index != 1);
        await tester.pump();
      }
      expect(find.text('Identify sent to 2/3 boards.'), findsOneWidget);
    },
  );

  testWidgets('individual Identify still reports a current failure', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService()..deferIdentify = true;
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    service.emitBoards(const <LitchBoard>[
      LitchBoard(id: 'board-a', name: 'First Litch', rssi: -40),
      LitchBoard(id: 'board-b', name: 'Second Litch', rssi: -50),
    ]);
    await tester.pump();
    await tester.tap(
      find.descendant(
        of: find.byKey(const ValueKey('discovered-board-board-a')),
        matching: find.widgetWithText(OutlinedButton, 'Identify'),
      ),
    );
    await tester.pump();
    service.completeNextIdentify(false);
    await tester.pump();
    expect(
      find.text('Couldn’t identify this Litch. Try again later.'),
      findsOneWidget,
    );
  });

  for (final interruption in [
    'background',
    'connection replaced',
    'choice cancelled',
    'Bluetooth cycle',
    'dispose',
  ]) {
    for (final identified in [true, false]) {
      testWidgets(
        'individual Identify ignores $interruption after result=$identified',
        (tester) async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final service = _ConnectionGateBleService(scriptCapabilities: true)
            ..deferIdentify = true;
          addTearDown(service.dispose);
          await tester.pumpWidget(
            MaterialApp(home: ConnectionGateScreen(bleService: service)),
          );
          await tester.pump();
          service.emitBoards(const <LitchBoard>[
            LitchBoard(id: 'board-a', name: 'First Litch', rssi: -40),
            LitchBoard(id: 'board-b', name: 'Second Litch', rssi: -50),
          ]);
          await tester.pump();
          final card = find.byKey(const ValueKey('discovered-board-board-a'));
          await tester.tap(
            find.descendant(
              of: card,
              matching: find.widgetWithText(OutlinedButton, 'Identify'),
            ),
          );
          await tester.pump();
          expect(service.identifyCalls.single.deviceId, 'board-a');
          switch (interruption) {
            case 'background':
              _sendAppToBackground(tester);
              await tester.pump();
              _sendAppToForeground(tester);
            case 'connection replaced':
              service.emitConnection(
                'board-b',
                DeviceConnectionState.connected,
              );
              await tester.pump();
              service.emitConnection(
                'board-b',
                DeviceConnectionState.disconnected,
              );
            case 'choice cancelled':
              await tester.tap(find.descendant(
                of: find.byKey(const ValueKey('discovered-board-board-b')),
                matching: find.widgetWithText(FilledButton, 'Connect'),
              ));
              await tester.pump();
              await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            case 'Bluetooth cycle':
              service.emitAvailability(BleAvailability.poweredOff);
              await tester.pump();
              service.emitAvailability(BleAvailability.ready);
            case 'dispose':
              await tester.pumpWidget(const SizedBox.shrink());
          }
          await tester.pump();
          service.completeNextIdentify(identified);
          await tester.pump();
          expect(find.textContaining('Sent identify'), findsNothing);
          expect(find.textContaining('Couldn’t identify'), findsNothing);
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  for (final interruption in <String>[
    'new board choice',
    'new connection',
    'repeated Choose',
    'background/resume',
    'back',
    'dispose',
  ]) {
    testWidgets('held chooser recovery respects $interruption', (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      final navigator = GlobalKey<NavigatorState>();
      final cancellation = Completer<void>();
      addTearDown(() async {
        if (!cancellation.isCompleted) cancellation.complete();
        final disposal = service.dispose();
        bool disposed = false;
        unawaited(disposal.then((_) => disposed = true));
        for (int pass = 0; pass < 8 && !disposed; ++pass) {
          await tester.pump();
          await tester.runAsync(() async {});
          await tester.pump();
        }
        expect(disposed, isTrue, reason: 'Gate fixture disposal must drain.');
        await disposal;
      });
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          navigatorKey: navigator,
          home: const SizedBox.shrink(),
        ),
      );
      unawaited(
        navigator.currentState!.push<void>(
          MaterialPageRoute(
            builder: (_) => ConnectionGateScreen(
              bleService: service,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      const boards = <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'First Litch', rssi: -40),
        LitchBoard(id: 'board-2', name: 'Second Litch', rssi: -45),
      ];
      service.emitBoards(boards);
      await tester.pump();
      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      service.heldStopScan = cancellation;
      final choose = find.byKey(
        const ValueKey('disconnected-board-status-action'),
      );
      await tester.tap(choose);
      if (interruption == 'repeated Choose') await tester.tap(choose);
      await tester.pump();
      expect(service.stopScanCalls, interruption == 'repeated Choose' ? 2 : 1);
      if (interruption == 'new board choice') {
        await tester.tap(_navigationAction(tester, 'play'));
        await tester.pump();
        final connect = find.descendant(
          of: find.byKey(const ValueKey('discovered-board-board-1')),
          matching: find.widgetWithText(FilledButton, 'Connect'),
        );
        await tester.tap(connect);
        await tester.pump();
        expect(service.connectCalls, ['board-1']);
      } else if (interruption == 'new connection') {
        service.emitConnection('board-2', DeviceConnectionState.connected);
        await tester.pump();
      } else if (interruption == 'background/resume') {
        _sendAppToBackground(tester);
        await tester.pump();
        _sendAppToForeground(tester);
        await tester.pump();
      } else if (interruption == 'back') {
        navigator.currentState!.pop();
        await tester.pumpAndSettle();
      } else if (interruption == 'dispose') {
        await tester.pumpWidget(const SizedBox.shrink());
      }
      final startsBeforeRelease = service.startScanCalls;
      cancellation.complete();
      await tester.pump();
      await tester.pump();
      expect(
        service.startScanCalls,
        startsBeforeRelease +
            (interruption == 'repeated Choose' ||
                    interruption == 'background/resume'
                ? 1
                : 0),
        reason: 'Only the latest chooser action may renew discovery.',
      );
      if (interruption == 'new board choice') {
        expect(
          find.byKey(const ValueKey('chosen-board-cancel')),
          findsOneWidget,
        );
        service.completeNextConnect(false);
        await tester.pump();
      } else if (interruption == 'new connection') {
        expect(service.isConnected, isTrue);
        expect(service.connectedId, 'board-2');
      }
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  testWidgets(
    'nearby boards show learned Bluetooth suffixes across platforms',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      SharedPreferences.setMockInitialValues(<String, Object>{
        'owned_litch_boards_v1': jsonEncode({
          '1234567890abcdef1234567890abcafe': {
            'aliases': [
              'CBAC74C1-949A-469A-897C-BCF04733BEEF',
              'AA:BB:CC:DD:EE:FF',
            ],
            'historyStorageKey': '1234567890abcdef1234567890abcafe',
            'bluetoothSuffix': '3F81',
          },
          '22222222222222222222222222222222': {
            'aliases': ['11:22:33:44:55:66'],
            'historyStorageKey': '22222222222222222222222222222222',
          },
          '33333333333333333333333333333333': {
            'aliases': ['77:88:99:AA:BB:CC'],
            'historyStorageKey': '33333333333333333333333333333333',
            'bluetoothSuffix': 'AA02',
          },
        }),
        'board_friendly_name:CBAC74C1-949A-469A-897C-BCF04733BEEF': 'Old name',
        'board_friendly_name:AA:BB:CC:DD:EE:FF': 'Travel Litch',
      });
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();

      service.emitBoards(const <LitchBoard>[
        LitchBoard(
          id: 'CBAC74C1-949A-469A-897C-BCF04733BEEF',
          name: 'Studio Litch',
          rssi: -35,
        ),
        LitchBoard(id: 'AA:BB:CC:DD:EE:FF', name: '', rssi: -45),
        LitchBoard(id: '77:88:99:AA:BB:CC', name: 'Another board', rssi: -55),
      ]);
      await tester.pump();

      expect(find.text('Litch ID: 3F81 · #1'), findsOneWidget);
      expect(find.text('Litch ID: 3F81 · #2'), findsOneWidget);
      expect(find.text('Litch ID: AA02 · #3'), findsOneWidget);
      expect(
        find.textContaining('CBAC74C1-949A-469A-897C-BCF04733BEEF'),
        findsNothing,
      );
      expect(find.textContaining('AA:BB:CC:DD:EE:FF'), findsNothing);
      expect(find.text('Studio Litch'), findsOneWidget);
      expect(find.text('Travel Litch'), findsOneWidget);
      expect(find.text('Old name'), findsNothing);

      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: '11:22:33:44:55:66', name: '', rssi: -50),
        LitchBoard(id: 'unknown-ios-UUID', name: 'Guest Litch', rssi: -60),
      ]);
      await tester.pump();
      expect(find.textContaining('Litch ID:'), findsNothing);
      expect(find.text('#1'), findsOneWidget);
      expect(find.text('#2'), findsOneWidget);
      expect(find.text('Foxie Litch'), findsWidgets);
      final first = find.byKey(
        const ValueKey('discovered-board-11:22:33:44:55:66'),
      );
      await tester.tap(
        find.descendant(
          of: first,
          matching: find.widgetWithText(OutlinedButton, 'Identify'),
        ),
      );
      await tester.pump();
      expect(service.identifyCalls.single.deviceId, '11:22:33:44:55:66');
      expect(service.identifyCalls.single.number, 1);
    },
  );

  for (final sameBoard in [false, true]) {
    testWidgets(
      'HeldIdentifyChoiceJourney stale cancelled Connect cannot cancel ${sameBoard ? 'same-board retry' : 'another board selection'}',
      (tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues({
          'board_friendly_name:board-1': 'Studio Board',
          'board_friendly_name:board-2': 'Travel Board',
        });
        final service = _ConnectionGateBleService()..deferIdentify = true;
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.pump();
        const first = LitchBoard(id: 'board-1', name: 'First Litch', rssi: -40);
        const second = LitchBoard(
          id: 'board-2',
          name: 'Second Litch',
          rssi: -45,
        );
        service.emitBoards(const [first, second]);
        await tester.pump();
        Finder card(String id) => find.byKey(ValueKey('discovered-board-$id'));
        Finder action(String id, Type type, String label) => find.descendant(
          of: card(id),
          matching: find.widgetWithText(type, label),
        );
        await tester.tap(action(first.id, OutlinedButton, 'Identify'));
        await tester.pump();
        expect(service.identifyCalls, hasLength(1));
        expect(
          find.byType(LinearProgressIndicator),
          findsOneWidget,
          reason: 'Identify alone retains its existing activity feedback.',
        );
        await tester.tap(action(first.id, FilledButton, 'Connect'));
        await tester.pump();
        expect(service.connectCalls, isEmpty);
        expect(
          find.byKey(const ValueKey('connection-progress-overlay')),
          findsOneWidget,
        );
        expect(
          find.byType(LinearProgressIndicator),
          findsOneWidget,
          reason: 'Connect owns the only bar while Identify cleanup is held.',
        );
        await tester.pump(const Duration(seconds: 8));
        expect(find.byType(LinearProgressIndicator), findsOneWidget);
        await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
        await tester.pump();
        await tester.pump();
        expect(
          find.byKey(const ValueKey('connection-progress-overlay')),
          findsNothing,
        );
        expect(
          find.byType(LinearProgressIndicator),
          findsOneWidget,
          reason:
              'Cancel restores Identify-only feedback until cleanup completes.',
        );
        final target = sameBoard ? first : second;
        final connect = action(target.id, FilledButton, 'Connect');
        await tester.ensureVisible(connect);
        await tester.pump();
        await tester.tap(connect);
        await tester.pump();
        await tester.pump(const Duration(seconds: 1));
        expect(find.byType(LinearProgressIndicator), findsOneWidget);
        expect(
          find.byKey(const ValueKey('owner-pairing-support')).hitTestable(),
          findsNothing,
        );
        expect(
          service.connectCalls,
          isEmpty,
          reason: 'New choice still waits for Identify cleanup',
        );
        service.completeNextIdentify();
        await tester.pump();
        await tester.pump();
        expect(find.byType(LinearProgressIndicator), findsOneWidget);
        expect(
          service.connectCalls,
          [target.id],
          reason:
              'Only the newest choice can own or clear its connecting state',
        );
        expect(find.text('Connecting to ${target.name}...'), findsOneWidget);
        service.completeNextConnect(false);
        await tester.pump();
        expect(
          find.byKey(const ValueKey('chosen-board-phase')),
          findsOneWidget,
        );
        await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
        await tester.pump();
        expect(find.byKey(const ValueKey('chosen-board-phase')), findsNothing);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  for (final disruption in <String>['background', 'Bluetooth off']) {
    testWidgets('queued Connect after Identify is cancelled by $disruption', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService()..deferIdentify = true;
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: ConnectionGateScreen(bleService: service),
        ),
      );
      await tester.pump();
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'First Litch', rssi: -40),
        LitchBoard(id: 'board-2', name: 'Second Litch', rssi: -45),
      ]);
      await tester.pump();

      final board = find.byKey(
        const ValueKey<String>('discovered-board-board-1'),
      );
      await tester.tap(
        find.descendant(
          of: board,
          matching: find.widgetWithText(OutlinedButton, 'Identify'),
        ),
      );
      await tester.pump();
      await tester.tap(
        find.descendant(
          of: board,
          matching: find.widgetWithText(FilledButton, 'Connect'),
        ),
      );
      await tester.pump();
      expect(service.connectCalls, isEmpty);

      if (disruption == 'background') {
        _sendAppToBackground(tester);
      } else {
        service.emitAvailability(BleAvailability.poweredOff);
      }
      await tester.pump();
      service.completeNextIdentify();
      await tester.pump();
      expect(service.connectCalls, isEmpty);
    });
  }

  for (final console in [false, true]) {
    for (final size in [const Size(390, 844), const Size(844, 390)]) {
      testWidgets(
        'Setup manual disconnect retains its tab console=$console $size',
        (tester) async {
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.reset);
          SharedPreferences.setMockInitialValues(<String, Object>{
            'enable_app_console': console,
          });
          final service = _ConnectionGateBleService();
          addTearDown(service.dispose);
          await _pumpConnectedGate(tester, service);
          await tester.tap(_navigationAction(tester, 'setup'));
          await tester.pumpAndSettle();
          expect(_activeNavigationIndex(tester), 2);
          await tester.tap(
            find.byKey(const ValueKey('connected-board-status-action')),
          );
          await tester.pumpAndSettle();
          final disconnect = find.byKey(const ValueKey('board-menu-disconnect'));
          await tester.ensureVisible(disconnect);
          await tester.pumpAndSettle();
          await tester.tap(disconnect.hitTestable());
          await tester.pumpAndSettle();
          expect(service.disconnectCalls, 1);
          expect(service.isConnected, isFalse);
          expect(
            _activeNavigationIndex(tester),
            2,
            reason: 'The actual Disconnect action must preserve Setup.',
          );
          final support = find.byKey(const ValueKey('open-contact-support'));
          await tester.ensureVisible(support);
          await tester.pumpAndSettle();
          expect(support.hitTestable(), findsOneWidget);

          // The offline status action deliberately opens board choices.
          await tester.tap(
            find.byKey(
              ValueKey(
                size.width > size.height
                    ? 'offline-board-status-action'
                    : 'disconnected-board-status-action',
              ),
            ),
          );
          await tester.pumpAndSettle();
          expect(_activeNavigationIndex(tester), 0);
          expect(
            find.byKey(const ValueKey('connection-gate-content')),
            findsOneWidget,
          );
          expect(service.connectCalls, hasLength(1));
          await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
          service.completeNextConnect(true);
          await tester.pumpAndSettle();
          expect(service.isConnected, isTrue);
          expect(
            _activeNavigationIndex(tester),
            0,
            reason: 'An explicit board chooser must not revive cached Setup.',
          );
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  testWidgets('Setup remains selected across connection and loss', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();
    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pumpAndSettle();
    expect(find.byType(AppPreferences), findsOneWidget);
    expect(_activeNavigationIndex(tester), 2);

    service.emitConnection('board-1', DeviceConnectionState.connected);
    await tester.pumpAndSettle();
    expect(find.byType(SettingsScreen), findsOneWidget);
    expect(_activeNavigationIndex(tester), 2);

    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    expect(
      find.byType(SettingsScreen),
      findsOneWidget,
      reason: 'Temporary link loss retains the selected Setup surface.',
    );
    expect(find.text('Not connected'), findsOneWidget);
    expect(_activeNavigationIndex(tester), 2);
    final phone = find.byKey(const ValueKey<String>('setup-open:phone'));
    await tester.ensureVisible(phone);
    await tester.pumpAndSettle();
    await tester.tap(phone.hitTestable());
    await tester.pumpAndSettle();
    expect(find.byType(AppPreferences), findsOneWidget);
    expect(find.text('This device'), findsOneWidget);
    expect(_activeNavigationIndex(tester), 2);
  });

  testWidgets(
    'rejected Litchcraft board exit stays on Play and retries on reconnect',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pump(const Duration(milliseconds: 300));
      craft.rejectExit = true;
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(craft.leaveCalls, 1);
      expect(_activeNavigationIndex(tester), 0);
      expect(
        tester
            .widget<PageView>(
              find.byWidgetPredicate((widget) => widget is PageView),
            )
            .controller!
            .page,
        0,
      );
      expect(
        find.byKey(const ValueKey<String>('craft-context-progress')),
        findsNothing,
      );
      expect(
        find.text(
          'The board has not confirmed exit. Press X and Circle together to try again.',
        ),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byType(SnackBarAction), findsNothing);
      final snackRect = tester.getRect(find.byType(SnackBar));
      final navigationRect = tester.getRect(
        find.byKey(const ValueKey<String>('main-navigation-surface')),
      );
      expect(snackRect.bottom, lessThanOrEqualTo(navigationRect.top));

      craft.rejectExit = false;
      craft.connectedBoardActive = true;
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pumpAndSettle();
      expect(craft.leaveCalls, 2);
      expect(
        _activeNavigationIndex(tester),
        0,
      ); // Successful retry reaches Play.
      expect(
        find.byKey(const ValueKey<String>('craft-context-progress')),
        findsNothing,
      );
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets(
    'reselecting Litchcraft during a confirmed exit re-enters after the ACK',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'litchcraft_discovery_dismissed': true,
      });
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            craftController: craft,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const Text('Firmware content'),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pumpAndSettle();
      expect(craft.enterCalls, 1);

      craft.exitGate = Completer<void>();
      await _selectAdjacentMainPage(tester, forward: false);
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      expect(_activeNavigationIndex(tester), 0);
      expect(craft.leaveCalls, 1);
      // Reselecting Litchcraft while the board exit is in flight queues a
      // re-entry behind the same ordered departure transaction.
      await tester.tap(_navigationAction(tester, 'litchcraft'));
      await tester.pump();
      craft.exitGate!.complete();
      await tester.pumpAndSettle();

      expect(craft.leaveCalls, 1);
      expect(craft.enterCalls, 2);
      expect(_activeNavigationIndex(tester), 1);
      expect(
        tester
            .widget<PageView>(
              find.byWidgetPredicate((widget) => widget is PageView),
            )
            .controller!
            .page,
        1,
      );
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  for (final platform in [TargetPlatform.iOS, TargetPlatform.android]) {
    testWidgets('disconnected header anchors brand and status $platform', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme().copyWith(platform: platform),
          builder: (_, child) => RepaintBoundary(child: child!),
          home: ConnectionGateScreen(bleService: service),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      await tester.runAsync(() async {
        final context = tester.element(find.byType(ConnectionGateScreen));
        for (final asset in ['foxie-litch-wordmark.png', 'foxie-fox.png']) {
          await precacheImage(AssetImage('assets/branding/$asset'), context);
        }
      });
      await tester.pump(const Duration(milliseconds: 300));
      await captureHealthReview(
        tester,
        'disconnected-header-${platform.name}-390',
      );
      final brand = tester.getRect(
        find.byKey(const ValueKey<String>('open-about')),
      );
      final status = tester.getRect(find.text('Not connected'));
      expect(brand.left, 16);
      expect(brand.right, lessThanOrEqualTo(status.left));
      expect(status.right, 390 - 12);
      final label = tester.renderObject<RenderParagraph>(
        find.text('Not connected'),
      );
      expect(
        label.textScaler.scale(label.text.style!.fontSize!),
        greaterThan(14),
      );
      expect(
        find.byKey(const ValueKey<String>('open-app-settings')),
        findsNothing,
      );
      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      expect(find.byType(AppPreferences), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      final connected = _ConnectionGateBleService();
      addTearDown(connected.dispose);
      await _pumpConnectedGate(
        tester,
        connected,
        theme: buildLitchTheme().copyWith(platform: platform),
      );
      expect(
        tester.getRect(find.byKey(const ValueKey<String>('open-about'))).left,
        brand.left,
      );
      await captureHealthReview(
        tester,
        'connected-header-${platform.name}-390',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }
  for (final (language, connectLabel, pendingLabel)
      in <(String, String, String)>[
        ('de', 'Verbinden', 'Verbinden…'),
        ('fr', 'Connecter', 'Connexion…'),
        ('it', 'Connetti', 'Connessione…'),
      ]) {
    testWidgets('pending $language connection fits 320px at 200 percent text', (
      WidgetTester tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(320, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      final GameLibraryRepository repository = _EmptyGameLibraryRepository();
      addTearDown(repository.close);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme().copyWith(platform: TargetPlatform.iOS),
          locale: Locale(language),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: TextScaler.linear(2)),
            child: child!,
          ),
          home: ConnectionGateScreen(
            bleService: service,
            gameLibrary: repository,
          ),
        ),
      );
      await tester.pump();
      expect(
        MediaQuery.sizeOf(tester.element(find.byType(ConnectionGateScreen))),
        const Size(320, 844),
      );
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
        LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -45),
      ]);
      await tester.pump();
      final connect = find.widgetWithText(FilledButton, connectLabel).first;
      await tester.ensureVisible(connect);
      await tester.pumpAndSettle();
      await decodeVisibleReviewImages(tester);
      await tester.tap(connect);
      await tester.pump();
      final badge = find.byKey(const ValueKey('chosen-board-phase'));
      final namedPendingLabel = AppLocalizations.of(
        tester.element(find.byType(ConnectionGateScreen)),
      ).connectingToBoard('Foxie One');
      expect(tester.widget<Text>(badge).data, namedPendingLabel);
      expect(badge, findsOneWidget);
      final semantics = tester.ensureSemantics();
      expect(tester.getSemantics(badge).label, contains(namedPendingLabel));
      final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
      expect(
        tester
            .getSemantics(cancel)
            .getSemanticsData()
            .hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      semantics.dispose();
      expect(
        tester.renderObject<RenderParagraph>(badge).didExceedMaxLines,
        isFalse,
      );
      expect(cancel.hitTestable(), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const ValueKey('discovered-board-board-1')),
          matching: find.text('Foxie One'),
        ),
        findsOneWidget,
      );
      expect(find.byType(LinearProgressIndicator), findsOneWidget);
      expect(tester.takeException(), isNull);
      // An explicit Play choice still reveals the offline library while the
      // chosen connection attempt keeps its single owner.
      await tester.tap(_navigationAction(tester, 'play'));
      await tester.pump();
      final toolbar = tester.getRect(find.byType(AppBar));
      final bounds = tester.getRect(find.text(pendingLabel));
      await tester.ensureVisible(
        find.byKey(const ValueKey<String>('game-library-action')),
      );
      await tester.pump();
      final Rect gameAction = tester.getRect(
        find.byKey(const ValueKey<String>('game-library-action')),
      );
      final Rect brand = tester.getRect(
        find.byKey(const ValueKey<String>('open-about')),
      );
      final double connectedBrandImageHeight = tester
          .getRect(
            find
                .descendant(
                  of: find.byKey(const ValueKey<String>('open-about')),
                  matching: find.byType(Image),
                )
                .first,
          )
          .height;
      expect(gameAction.width, greaterThanOrEqualTo(48));
      expect(gameAction.height, greaterThanOrEqualTo(48));
      expect(gameAction.top, greaterThanOrEqualTo(toolbar.bottom));
      expect(gameAction.right, lessThanOrEqualTo(320));
      expect(brand.right, lessThanOrEqualTo(bounds.left));
      expect(
        tester
            .widget<TextButton>(
              find.byKey(const ValueKey<String>('game-library-action')),
            )
            .onPressed,
        isNotNull,
      );
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'connecting-$language-320-200');
      service.completeNextConnect(false);
      await tester.pump();
      expect(badge, findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
      await tester.pump();
      expect(badge, findsNothing);
      final double disconnectedBrandImageHeight = tester
          .getRect(
            find
                .descendant(
                  of: find.byKey(const ValueKey<String>('open-about')),
                  matching: find.byType(Image),
                )
                .first,
          )
          .height;
      expect(
        disconnectedBrandImageHeight,
        closeTo(connectedBrandImageHeight, 0.01),
      );
      await tester.pump(const Duration(milliseconds: 300));
      await captureHealthReview(tester, 'disconnected-$language-320-200');
    });
  }

  testWidgets('review Connected health journey at phone sizes', (tester) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(393, 852));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    for (final platform in <TargetPlatform>[
      TargetPlatform.iOS,
      TargetPlatform.android,
    ]) {
      for (final scale in <double>[1, 2]) {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _ConnectionGateBleService(
          useProductionConsoleStream: true,
        )..reviewBoardRotationKnown = true;
        addTearDown(service.dispose);
        await _pumpConnectedGate(
          tester,
          service,
          theme: buildLitchTheme().copyWith(platform: platform),
          textScaler: TextScaler.linear(scale),
        );
        service.reportHome();
        await tester.pumpAndSettle();
        await tester.runAsync(() async {
          final context = tester.element(find.byType(LitchMainScaffold));
          for (final image in tester.widgetList<Image>(find.byType(Image))) {
            await precacheImage(image.image, context);
          }
        });
        await tester.pumpAndSettle();
        expect(
          find.descendant(
            of: find.byType(HomeChessboard),
            matching: find.byType(Image),
          ),
          findsNWidgets(32),
        );
        final suffix = '${platform.name}-${scale.toInt()}';
        await captureHealthReview(tester, 'connected-healthy-$suffix');
        service.reportHealth(stateCode: 1, causeMask: 0x10);
        await tester.pumpAndSettle();
        expect(find.text('Needs attention'), findsOneWidget);
        await captureHealthReview(tester, 'connected-attention-$suffix');
        await tester.tap(
          find.byKey(const ValueKey<String>('connected-board-status-action')),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(tester, 'connected-menu-$suffix');
        await tester.tap(
          find.byKey(const ValueKey<String>('board-menu-health')),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(tester, 'settings-health-$suffix');
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    }
  });

  for (final scenario
      in <
        ({
          String name,
          int stateCode,
          int causeMask,
          bool storageFailed,
          int reset,
          bool attention,
        })
      >[
        (
          name: 'healthy',
          stateCode: 0,
          causeMask: 0,
          storageFailed: false,
          reset: 3,
          attention: false,
        ),
        (
          name: 'retained brownout',
          stateCode: 1,
          causeMask: 16,
          storageFailed: false,
          reset: 3,
          attention: true,
        ),
        (
          name: 'input paused',
          stateCode: 2,
          causeMask: 4,
          storageFailed: false,
          reset: 3,
          attention: true,
        ),
        (
          name: 'saving unavailable',
          stateCode: 0,
          causeMask: 0,
          storageFailed: true,
          reset: 3,
          attention: true,
        ),
        (
          name: 'past watchdog only',
          stateCode: 0,
          causeMask: 0,
          storageFailed: false,
          reset: 6,
          attention: false,
        ),
        (
          name: 'unrecognized health',
          stateCode: 9,
          causeMask: 0,
          storageFailed: false,
          reset: 3,
          attention: false,
        ),
      ]) {
    testWidgets('health attention uses Connected menu: ${scenario.name}', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      service.reportHealth(
        stateCode: scenario.stateCode,
        causeMask: scenario.causeMask,
        storageFailed: scenario.storageFailed,
        resetReason: scenario.reset,
        watchdogRestarts: 1,
      );
      await tester.pump();
      await tester.pump();
      expect(find.text('Check board health'), findsNothing);
      expect(
        find.text('Needs attention'),
        scenario.attention ? findsOneWidget : findsNothing,
      );
      final status = tester.widget<Semantics>(
        find.byKey(
          const ValueKey<String>('connected-board-status-live-region'),
        ),
      );
      expect(
        status.properties.label,
        scenario.attention
            ? contains('Board health needs attention')
            : isNot(contains('health needs attention')),
      );
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('board-menu-health')),
        scenario.attention ? findsOneWidget : findsNothing,
      );
      expect(find.text('Check for updates'), findsOneWidget);
      if (scenario.attention) {
        await tester.tap(
          find.byKey(const ValueKey<String>('board-menu-health')),
        );
        await tester.pumpAndSettle();
        expect(_activeNavigationIndex(tester), 2);
        expect(find.text('Support & software'), findsNothing);
        expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
        expect(find.byKey(const ValueKey('setup-back')), findsNothing);
        expect(find.byKey(const ValueKey('advanced-search')), findsOneWidget);
        expect(
          find.byKey(const ValueKey('board-health-priority')),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey('open-contact-support')),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey('open-board-software')),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey<String>('runtime-health-card')),
          findsOneWidget,
        );
        expect(
          find
              .byKey(const ValueKey<String>('runtime-health-toggle'))
              .hitTestable(),
          findsOneWidget,
          reason: 'The explicit health destination exposes its next action.',
        );
        expect(
          WidgetsBinding.instance.focusManager.primaryFocus?.debugLabel,
          'Board health section',
        );
      }
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  testWidgets(
    'health read failure is unavailable and never orange fault evidence',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService()
        ..healthResult = const RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.timedOut,
        );
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.pump(const Duration(seconds: 4));
      await tester.pump();
      expect(find.text('Needs attention'), findsNothing);
      expect(find.text('Check board health'), findsNothing);
      expect(find.text('Board health needs attention.'), findsNothing);
      expect(service.disconnectCalls, 0);
      expect(service.connectCalls, <String>['board-1']);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('health warning clears in an already open Connected menu', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);
    service.reportHealth(stateCode: 1, causeMask: 0x10);
    await tester.pump();
    await tester.pump();
    await tester.tap(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('board-menu-health')),
      findsOneWidget,
    );
    service.reportHealth();
    await tester.pump();
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('board-menu-health')),
      findsNothing,
    );
    expect(find.text('Needs attention'), findsNothing);
    expect(service.disconnectCalls, 0);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final bool unavailable in <bool>[false, true]) {
    testWidgets(
      'startup continues when optional preferences are ${unavailable ? 'unavailable' : 'wrongly typed'}',
      (WidgetTester tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final service = _ConnectionGateBleService();
        final craft = _NavigationCraftController(service);
        final library = LitchcraftLibrary(
          craft,
          store: LitchcraftArtworkStore(await SharedPreferences.getInstance()),
        );
        await library.ready;
        final originalPreferences = SharedPreferencesStorePlatform.instance;
        addTearDown(() async {
          SharedPreferencesStorePlatform.instance = originalPreferences;
          SharedPreferences.resetStatic();
          library.dispose();
          craft.dispose();
          await service.dispose();
        });
        if (unavailable) {
          SharedPreferences.resetStatic();
          SharedPreferencesStorePlatform.instance = _FailingStartupPreferences();
        } else {
          SharedPreferences.setMockInitialValues(<String, Object>{
            'board_friendly_name:board-1': 42,
            'enable_app_console': 'invalid',
          });
        }
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: craft,
              craftLibrary: library,
            ),
          ),
        );
        await tester.pump();
        expect(tester.takeException(), isNull);
        expect(service.startScanCalls, 1);
        expect(find.text('Console'), findsNothing);
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
        ]);
        await tester.pump(const Duration(seconds: 2));
        expect(service.connectCalls, <String>['board-1']);
        service.completeNextConnect(false);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  for (final bool remembered in <bool>[false, true]) {
    testWidgets(
      'startup timing: ${remembered ? 'remembered' : 'unknown'} single board',
      (WidgetTester tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{
          if (remembered) 'last_connected_board_id': 'board-1',
        });
        final _ConnectionGateBleService service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        final DateTime started = tester.binding.clock.now();
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await tester.pump();
        expect(find.byType(ConnectionGateScreen), findsOneWidget);
        expect(service.startScanCalls, 1);
        expect(service.connectCalls, isEmpty);

        await tester.pump(const Duration(milliseconds: 120));
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
        ]);
        expect(
          service.connectCalls,
          isEmpty,
          reason: 'Connection is queued, not reentrant.',
        );
        if (!remembered) {
          await tester.pump(const Duration(milliseconds: 1999));
          expect(service.connectCalls, isEmpty);
          await tester.pump(const Duration(milliseconds: 1));
        } else {
          await tester.pump(Duration.zero);
        }
        expect(service.connectCalls, <String>['board-1']);
        debugPrint(
          'STARTUP virtual ${remembered ? 'remembered' : 'unknown'}: '
          'first frame/scan=0 ms; discovery=120 ms; '
          'connect request=${tester.binding.clock.now().difference(started).inMilliseconds} ms',
        );
        service.completeNextConnect(false);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );

    testWidgets(
      'startup timing: ${remembered ? 'remembered identity wins' : 'unknown candidate loses'} multi-board arbitration',
      (WidgetTester tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{
          if (remembered) 'last_connected_board_id': 'board-1',
        });
        final _ConnectionGateBleService service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await tester.pump();
        const LitchBoard first = LitchBoard(
          id: 'board-1',
          name: 'Litch',
          rssi: -40,
        );
        const LitchBoard second = LitchBoard(
          id: 'board-2',
          name: 'Foxie_Litch',
          rssi: -45,
        );
        service.emitBoards(const <LitchBoard>[first]);
        if (!remembered) await tester.pump(const Duration(milliseconds: 300));
        service.emitBoards(const <LitchBoard>[first, second]);
        await tester.pump(const Duration(seconds: 2));
        expect(service.connectCalls, remembered ? ['board-1'] : isEmpty);
        if (remembered) {
          expect(find.byType(HomeScreen), findsNothing);
          expect(
            find.byKey(const ValueKey<String>('connection-gate-content')),
            findsOneWidget,
          );
          expect(
            find.byKey(const ValueKey('chosen-board-connection')),
            findsOneWidget,
          );
        } else {
          expect(
            find.widgetWithText(FilledButton, 'Connect'),
            findsNWidgets(2),
          );
        }
        if (remembered) service.completeNextConnect(false);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('first connection keeps the branded shell until mode is known', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    const LitchBoard board = LitchBoard(
      id: 'board-1',
      name: 'Litch',
      rssi: -40,
    );

    await tester.pumpWidget(
      MaterialApp(
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: RepaintBoundary(
            key: const ValueKey('first-connect-review'),
            child: child!,
          ),
        ),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();
    service.emitBoards(const <LitchBoard>[board]);
    await tester.pump(const Duration(seconds: 2));

    expect(service.connectCalls, <String>[board.id]);
    expect(find.byType(LitchMainScaffold), findsNothing);
    expect(find.byType(HomeScreen), findsNothing);
    expect(_activeNavigationIndex(tester), -1); // No destination is proven yet.
    expect(find.byKey(const ValueKey<String>('control:hint')), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('connection-gate-content')),
      findsOneWidget,
    );
    expect(find.text('Position not available'), findsNothing);
    expect(
      find.text(
        'The app could not read the position after retrying. Play can continue normally on the physical board.',
      ),
      findsNothing,
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(
      tester,
      'first-connect-play-390x844-text-200',
      boundaryFinder: find.byKey(const ValueKey('first-connect-review')),
    );

    service.completeNextConnect(false);
    await tester.pump();
    expect(find.byType(LitchMainScaffold), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('connection-gate-content')),
      findsOneWidget,
      reason: 'A failed first connection must restore actionable recovery.',
    );
    expect(
      find.text('Failed to connect to ${board.name}. Tap Connect to retry.'),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'first connection gives authoritative mode resolution one owner',
    (WidgetTester tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      final _NavigationCraftController craft = _NavigationCraftController(
        service,
      )..connectedBoardActive = true;
      final LitchcraftLibrary library = LitchcraftLibrary(craft);
      await library.ready;
      const LitchBoard board = LitchBoard(
        id: 'board-1',
        name: 'Litch',
        rssi: -40,
      );

      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: craft,
            craftLibrary: library,
          ),
        ),
      );
      await tester.pump();
      service.emitBoards(const <LitchBoard>[board]);
      await tester.pump(const Duration(seconds: 2));
      service.completeNextConnect(true);
      await tester.pumpAndSettle();

      expect(craft.adoptCalls, 1);
      expect(craft.enterCalls, 0);
      expect(find.byType(LitchcraftScreen), findsOneWidget);
      expect(_activeNavigationIndex(tester), 1); // Authoritative active board.
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );

      await tester.pumpWidget(const SizedBox.shrink());
      await library.flush();
      library.dispose();
      craft.dispose();
      await service.dispose();
    },
  );

  testWidgets('first connection keeps one mode owner after selecting Setup', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    final _NavigationCraftController craft = _NavigationCraftController(
      service,
    );
    final LitchcraftLibrary library = LitchcraftLibrary(craft);
    await library.ready;
    const LitchBoard board = LitchBoard(
      id: 'board-1',
      name: 'Litch',
      rssi: -40,
    );

    await tester.pumpWidget(
      MaterialApp(
        home: ConnectionGateScreen(
          bleService: service,
          craftController: craft,
          craftLibrary: library,
        ),
      ),
    );
    await tester.pump();
    service.emitBoards(const <LitchBoard>[
      board,
      LitchBoard(id: 'board-2', name: 'Second Litch', rssi: -45),
    ]);
    await tester.pump();
    await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
    await tester.pump();
    await tester.tap(_navigationAction(tester, 'setup'));
    await tester.pump(const Duration(milliseconds: 500));
    expect(_activeNavigationIndex(tester), 2);
    expect(find.byType(SettingsScreen), findsNothing);
    expect(find.byType(AppPreferences), findsOneWidget);
    service.emitBoards(const <LitchBoard>[
      board,
      LitchBoard(id: 'board-2', name: 'Second Litch', rssi: -45),
      LitchBoard(id: 'board-3', name: 'Third Litch', rssi: -50),
    ]);
    await tester.pump();
    expect(find.byType(LitchMainScaffold), findsNothing);
    expect(_activeNavigationIndex(tester), 2);
    expect(service.connectCalls, <String>[board.id]);
    expect(find.text('Connecting…'), findsOneWidget);

    craft.connectedBoardActive = true;
    service.completeNextConnect(true);
    await tester.pumpAndSettle();

    expect(craft.adoptCalls, 1);
    expect(craft.enterCalls, 0);
    expect(_activeNavigationIndex(tester), 2);
    expect(find.byType(SettingsScreen), findsOneWidget);
    expect(
      find.byKey(const ValueKey('connected-board-mode-progress')),
      findsNothing,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    await library.flush();
    library.dispose();
    craft.dispose();
    await service.dispose();
  });

  testWidgets('startup timing: absent board never becomes ready', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{
      'last_connected_board_id': 'board-1',
    });
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    await tester.pump(const Duration(seconds: 10));
    expect(service.connectCalls, isEmpty);
    expect(find.byType(LitchMainScaffold), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('discovery-support')),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'startup timing: later board does not replace a queued remembered choice',
    (WidgetTester tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'last_connected_board_id': 'board-1',
      });
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      const LitchBoard first = LitchBoard(
        id: 'board-1',
        name: 'Litch',
        rssi: -40,
      );
      const LitchBoard second = LitchBoard(
        id: 'board-2',
        name: 'Foxie_Litch',
        rssi: -45,
      );
      service.emitBoards(const <LitchBoard>[first]);
      await tester.pump(const Duration(milliseconds: 300));
      expect(service.connectCalls, <String>['board-1']);
      service.emitBoards(const <LitchBoard>[first, second]);
      await tester.pump(const Duration(seconds: 2));
      expect(service.connectCalls, <String>['board-1']);
      expect(
        find.byKey(const ValueKey('chosen-board-connection')),
        findsOneWidget,
      );
      service.completeNextConnect(false);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  for (final String interruption in <String>[
    'permission',
    'off',
    'background',
    'replacement',
  ]) {
    testWidgets(
      'startup timing: queued known target is canceled by $interruption',
      (WidgetTester tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{
          'last_connected_board_id': 'board-1',
        });
        final _ConnectionGateBleService service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await tester.pump();
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
        ]);
        switch (interruption) {
          case 'permission':
            service.emitAvailability(BleAvailability.bluetoothPermissionDenied);
          case 'off':
            service.emitAvailability(BleAvailability.poweredOff);
          case 'background':
            _sendAppToBackground(tester);
          case 'replacement':
            service.emitBoards(const <LitchBoard>[
              LitchBoard(id: 'board-2', name: 'Litch', rssi: -40),
            ]);
        }
        await tester.pump(Duration.zero);
        expect(service.connectCalls, isEmpty);
        if (interruption == 'background') {
          _sendAppToForeground(tester);
          await tester.pump(Duration.zero);
          expect(service.connectCalls, isEmpty);
        }
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets(
    'startup timing: real loss while backgrounded waits for fresh discovery on restore',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      const List<LitchBoard> boards = <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
      ];
      service.emitBoards(boards);
      _sendAppToBackground(tester);
      await tester.pump();
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      expect(service.isConnected, isFalse);
      _sendAppToForeground(tester);
      await tester.pump(const Duration(seconds: 1));
      expect(find.text('Connected'), findsNothing);
      expect(service.connectCalls, <String>['board-1']);
      service.emitBoards(boards);
      await tester.pump(Duration.zero);
      expect(service.connectCalls, <String>['board-1', 'board-1']);
      service.completeNextConnect(true);
      await tester.pump();
      expect(service.isConnected, isTrue);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'startup timing: manual disconnect keeps the remembered board disconnected',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.tap(find.text('Connected'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Disconnect'));
      await tester.pumpAndSettle();
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board-1', name: 'Litch', rssi: -40),
      ]);
      await tester.pump(const Duration(seconds: 3));
      expect(service.connectCalls, <String>['board-1']);
      expect(service.isConnected, isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('keeps disconnected content above platform navigation', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    const double bottomSystemInset = 34;
    const systemPadding = EdgeInsets.fromLTRB(13, 0, 7, bottomSystemInset);

    await tester.pumpWidget(
      MaterialApp(
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(padding: systemPadding, viewPadding: systemPadding),
          child: child!,
        ),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();

    final Rect scaffold = tester.getRect(find.byType(Scaffold));
    final Rect content = tester.getRect(
      find.byKey(const ValueKey<String>('connection-gate-content')),
    );
    final Finder navigationFinder = find.byKey(
      const ValueKey<String>('disconnected-navigation'),
    );
    final Rect navigation = tester.getRect(navigationFinder);
    final Finder surfaceFinder = find.byKey(
      const ValueKey<String>('disconnected-navigation-surface'),
    );
    final Rect surface = tester.getRect(surfaceFinder);
    expect(navigation.left, scaffold.left + systemPadding.left);
    expect(navigation.right, scaffold.right - systemPadding.right);
    expect(content.bottom, lessThanOrEqualTo(navigation.top));
    expect(
      navigation.bottom,
      moreOrLessEquals(scaffold.bottom - bottomSystemInset),
    );
    expect(
      tester.getRect(_navigationAction(tester, 'play')).bottom,
      lessThanOrEqualTo(scaffold.bottom - bottomSystemInset),
    );
    expect(surface.bottom, scaffold.bottom);
    expect(surface.top, navigation.top);
    expect(surface.height, navigation.height + bottomSystemInset);
    expect(
      tester.widget<ColoredBox>(surfaceFinder).color,
      Theme.of(tester.element(surfaceFinder)).colorScheme.surfaceContainer,
    );
  });

  for (final destination in ['play', 'litchcraft', 'setup']) {
    for (final landscape in [false, true]) {
      testWidgets(
        'warm offline header recovers discovery after failed reconnect '
        '$destination ${landscape ? 'landscape' : 'portrait'}',
        (tester) async {
          tester.view.physicalSize = landscape
              ? const Size(844, 390)
              : const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final service = _ConnectionGateBleService();
          addTearDown(service.dispose);
          final semantics = tester.ensureSemantics();
          await _pumpConnectedGate(tester, service);
          await tester.tap(_navigationAction(tester, destination));
          await tester.pump();
          final shell = tester.state(find.byType(LitchMainScaffold));
          service.emitConnection('board-1', DeviceConnectionState.disconnected);
          await tester.pump();
          if (destination == 'play') {
            expect(find.byType(LitchMainScaffold), findsNothing);
            expect(
              find.byKey(const ValueKey('connection-gate-content')),
              findsOneWidget,
            );
          }
          const first = LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40);
          const second = LitchBoard(
            id: 'board-2',
            name: 'Foxie Two',
            rssi: -45,
          );
          service.emitBoards(const [first]);
          await tester.pump(const Duration(milliseconds: 900));
          service.completeNextConnect(false);
          await tester.pump();
          if (destination == 'play') {
            expect(find.byKey(const ValueKey('chosen-board-phase')), findsOneWidget);
            await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            await tester.pump();
          }
          service.emitBoards(const [second]);
          await tester.pump();
          if (destination != 'play') {
            expect(
              identical(tester.state(find.byType(LitchMainScaffold)), shell),
              isTrue,
            );
            final action = find.byKey(
              ValueKey<String>(
                landscape
                    ? 'connected-board-status-action'
                    : 'disconnected-board-status-action',
              ),
            );
            expect(
              action,
              findsOneWidget,
              reason:
                  'Warm offline connection recovery must be reachable without restart',
            );
            expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
            if (!landscape) {
              expect(
                tester.getSemantics(
                  find.byKey(
                    const ValueKey('disconnected-board-status-live-region'),
                  ),
                ),
                isSemantics(
                  label: 'Disconnected. Select a board to connect.',
                  isButton: true,
                  isLiveRegion: true,
                  hasTapAction: true,
                ),
              );
            } else {
              expect(tester.widget<TextButton>(action).onPressed, isNotNull);
              expect(
                tester.getSemantics(
                  find.byKey(
                    const ValueKey('compact-board-status-live-region'),
                  ),
                ),
                isSemantics(
                  label: 'Disconnected. Select a board to connect.',
                  isButton: true,
                  isLiveRegion: true,
                  hasTapAction: true,
                ),
              );
            }
            await tester.tap(action);
            await tester.pump();
            final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
            if (cancel.evaluate().isNotEmpty) {
              await tester.tap(cancel);
              await tester.pump();
            }
          }
          expect(
            find.byKey(const ValueKey('connection-gate-content')),
            findsOneWidget,
          );
          expect(find.text('Foxie Two'), findsWidgets);
          if (!landscape) {
            final status = find.text('Not connected');
            expect(status, findsOneWidget);
            final text = tester.widget<Text>(status);
            final color =
                text.style?.color ??
                DefaultTextStyle.of(tester.element(status)).style.color;
            expect(
              color,
              Theme.of(tester.element(status)).colorScheme.onSurfaceVariant,
              reason:
                  'Connect cannot turn the same offline status into an error',
            );
            expect(
              find.byKey(const ValueKey('connected-board-status-chevron')),
              findsNothing,
              reason: 'Connect exposes discovery; its status is passive',
            );
          }
          if (destination != 'play') {
            await tester.tap(_navigationAction(tester, destination));
            await tester.pump();
            final offlineAction = find.byKey(
              ValueKey(
                destination == 'setup'
                    ? 'offline-board-status-action'
                    : landscape
                    ? 'connected-board-status-action'
                    : 'disconnected-board-status-action',
              ),
            );
            expect(offlineAction, findsOneWidget);
            if (!landscape) {
              expect(
                find.descendant(
                  of: offlineAction,
                  matching: find.byIcon(Icons.keyboard_arrow_down_rounded),
                ),
                findsNothing,
                reason: 'The offline shortcut opens Play without a menu cue',
              );
            }
            expect(
              tester.getSize(offlineAction).height,
              greaterThanOrEqualTo(48),
            );
            await tester.tap(offlineAction);
            await tester.pump();
            expect(
              find.byKey(const ValueKey('connection-gate-content')),
              findsOneWidget,
            );
            expect(find.text('Foxie Two'), findsWidgets);
          }
          await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
          await tester.pump();
          expect(service.connectCalls.last, second.id);
          service.completeNextConnect(true);
          await tester.pump();
          await tester.pump();
          expect(service.connectedId, second.id);
          expect(find.byType(LitchMainScaffold), findsOneWidget);
          expect(tester.takeException(), isNull);
          semantics.dispose();
        },
      );
    }
  }

  testWidgets('opens About from the disconnected app identity', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);

    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('open-about')));
    await tester.pumpAndSettle();

    expect(find.byType(AboutScreen), findsOneWidget);
  });

  testWidgets('connection transitions use one live region without scan chatter', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final SemanticsHandle semantics = tester.ensureSemantics();
    final _ConnectionGateBleService service = _ConnectionGateBleService()
      ..settlePendingOnDisconnect = true;
    addTearDown(service.dispose);
    final craft = _NavigationCraftController(service)
      ..adoptGate = Completer<bool>();
    addTearDown(craft.dispose);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    addTearDown(library.dispose);
    const LitchBoard firstBoard = LitchBoard(
      id: 'board-1',
      name: 'Foxie One',
      rssi: -40,
    );
    const LitchBoard secondBoard = LitchBoard(
      id: 'board-2',
      name: 'Foxie Two',
      rssi: -45,
    );

    await tester.pumpWidget(
      MaterialApp(
        home: ConnectionGateScreen(
          bleService: service,
          craftController: craft,
          craftLibrary: library,
        ),
      ),
    );
    await tester.pump();
    final Finder liveRegion = find.byKey(
      const ValueKey<String>('connection-status-live-region'),
    );
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(label: 'Looking for the undead...', isLiveRegion: true),
    );

    service.emitBoards(const <LitchBoard>[firstBoard, secondBoard]);
    await tester.pump();
    expect(
      tester.getSemantics(liveRegion).label,
      'Select a board to connect. Use Identify if needed.',
    );
    expect(
      find.byKey(const ValueKey<String>('discovery-support')),
      findsNothing,
    );
    service.emitBoards(const <LitchBoard>[firstBoard, secondBoard]);
    await tester.pump();
    expect(
      tester.getSemantics(liveRegion).label,
      'Select a board to connect. Use Identify if needed.',
      reason: 'routine scan refreshes must not alter the announcement',
    );

    await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
    await tester.pump();
    final Finder connectedLiveRegion = find.byKey(
      const ValueKey<String>('connected-board-status-live-region'),
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
          .label,
      contains(firstBoard.name),
    );
    expect(connectedLiveRegion, findsNothing);
    expect(find.byType(HomeScreen), findsNothing);
    final quietStatus = tester.widget<Semantics>(liveRegion).properties;
    expect(quietStatus.liveRegion, isFalse);
    expect(quietStatus.label, isEmpty);
    final hiddenChooserStatus = find.descendant(
      of: liveRegion,
      matching: find.byType(Opacity),
    );
    expect(hiddenChooserStatus, findsOneWidget);
    expect(tester.widget<Opacity>(hiddenChooserStatus).opacity, 0);
    expect(
      find.descendant(
        of: find.byKey(const ValueKey('chosen-board-connection')),
        matching: find.textContaining(firstBoard.name),
      ),
      findsOneWidget,
    );
    expect(find.text('Not connected'), findsNothing);

    final support = find.byKey(const ValueKey('owner-pairing-support'));
    await tester.pump(const Duration(seconds: 2));
    expect(support, findsNothing);
    await tester.pump(const Duration(seconds: 1));
    expect(support.hitTestable(), findsOneWidget);
    expect(
      find.byKey(const ValueKey('chosen-board-cancel')).hitTestable(),
      findsOneWidget,
    );
    await tester.tap(support);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
    final report = find.byType(SupportReportScreen);
    expect(tester.widget<SupportReportScreen>(report).boardId, firstBoard.id);
    expect(service.isConnected, isFalse);
    Navigator.of(tester.element(report)).pop();
    await tester.pump();
    await tester.pump(const Duration(seconds: 1));
    await tester.pump();
    expect(report, findsNothing);
    expect(support, findsNothing);
    expect(tester.getSemantics(liveRegion).label, isNotEmpty);
    expect(find.text('Connecting…'), findsNothing);
    expect(find.text('Not connected'), findsOneWidget);
    for (final label in <String>['Litchcraft', 'Setup', 'Play']) {
      await tester.tap(_navigationAction(tester, label.toLowerCase()));
      await tester.pump();
      expect(find.text('Not connected'), findsOneWidget);
      expect(find.text('Connecting…'), findsNothing);
    }
    expect(
      find.byKey(const ValueKey<String>('connection-status-live-region')),
      findsOneWidget,
    );
    final attemptsAtBack = service.connectCalls.length;
    await tester.pump(const Duration(seconds: 3));
    expect(service.connectCalls.length, attemptsAtBack);
    final selectedConnect = find.descendant(
      of: find.byKey(const ValueKey('discovered-board-board-1')),
      matching: find.widgetWithText(FilledButton, 'Connect'),
    );
    expect(selectedConnect.hitTestable(), findsOneWidget);

    await tester.tap(selectedConnect);
    await tester.pump();
    service.completeNextConnect(true);
    await tester.pump();
    await tester.pump();
    expect(connectedLiveRegion, findsNothing);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
          .label,
      contains(firstBoard.name),
    );
    expect(tester.widget<Semantics>(liveRegion).properties.liveRegion, isFalse);
    craft.adoptGate!.complete(false);
    await tester.pumpAndSettle();
    expect(
      tester.getSemantics(connectedLiveRegion),
      isSemantics(
        label: 'Connected. Open board options.',
        isButton: true,
        isLiveRegion: true,
        hasTapAction: true,
      ),
    );
    expect(
      find.byKey(const ValueKey<String>('discovery-support')),
      findsNothing,
    );

    service.emitConnection(firstBoard.id, DeviceConnectionState.disconnected);
    await tester.pump();
    expect(connectedLiveRegion, findsNothing);
    expect(liveRegion, findsOneWidget);
    expect(
      find.byKey(const ValueKey('connection-gate-content')),
      findsOneWidget,
    );
    expect(find.byType(HomeScreen), findsNothing);
    expect(
      tester
          .getSemantics(liveRegion)
          .getSemanticsData()
          .flagsCollection
          .isLiveRegion,
      isTrue,
    );
    final attemptsBeforeReconnect = service.connectCalls.length;
    service.emitBoards(const <LitchBoard>[firstBoard]);
    await tester.pump(const Duration(milliseconds: 900));
    expect(service.connectCalls.length, attemptsBeforeReconnect + 1);
    expect(tester.getSemantics(liveRegion).label, isEmpty);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
          .label,
      contains(firstBoard.name),
    );
    expect(find.text('Connecting…'), findsOneWidget);
    service.emitBoards(const <LitchBoard>[firstBoard]);
    await tester.pump(const Duration(milliseconds: 900));
    expect(
      service.connectCalls.length,
      attemptsBeforeReconnect + 1,
      reason:
          'Repeated advertisements cannot duplicate an in-flight reconnect.',
    );
    expect(support, findsNothing);
    service.completeNextConnect(false);
    await tester.pump();
    expect(tester.getSemantics(liveRegion).label, isEmpty);
    expect(find.text('Not connected'), findsNothing);
    expect(find.text('Connecting…'), findsOneWidget);
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, 'Connect'))
          .onPressed,
      isNull,
    );
    expect(
      find.byKey(const ValueKey<String>('discovery-support')),
      findsNothing,
    );
    await tester.pump(const Duration(seconds: 3));
    service.emitBoards(const <LitchBoard>[firstBoard]);
    await tester.pump(const Duration(milliseconds: 900));
    expect(
      service.connectCalls.length,
      attemptsBeforeReconnect + 2,
      reason:
          'A later advertisement may retry after the prior attempt has failed.',
    );
    expect(support.hitTestable(), findsOneWidget);
    expect(tester.getSemantics(liveRegion).label, isEmpty);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
          .label,
      contains(firstBoard.name),
    );
    service.completeNextConnect(true);
    await tester.pump();
    await tester.pumpAndSettle();
    expect(
      tester.getSemantics(connectedLiveRegion),
      isSemantics(
        label: 'Connected. Open board options.',
        isButton: true,
        isLiveRegion: true,
        hasTapAction: true,
      ),
    );
    expect(
      find.byKey(const ValueKey<String>('discovery-support')),
      findsNothing,
    );
    semantics.dispose();
  });

  testWidgets('stalled Connecting retries at 3s and keeps help visible', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{
      'last_connected_board_id': 'board-1',
    });
    final service = _ConnectionGateBleService()
      ..settlePendingOnDisconnect = true;
    addTearDown(service.dispose);
    const board = LitchBoard(id: 'board-1', name: 'Studio Board', rssi: -40);
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    service.emitBoards(const [board]);
    await tester.pump();
    final connect = find.widgetWithText(FilledButton, 'Connect');
    await tester.tap(connect);
    await tester.pump();
    final help = find.byKey(const ValueKey('owner-pairing-support'));
    final progress = find.byKey(const ValueKey('connection-progress-overlay'));
    await tester.pump(const Duration(milliseconds: 2999));
    expect(help, findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    expect(help.hitTestable(), findsOneWidget);
    expect(
      service.connectCalls.length,
      2,
      reason: 'The stalled attempt restarts at 3s without a new advertisement.',
    );
    final original = tester.element(progress);
    final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
    expect(cancel.hitTestable(), findsOneWidget);
    await tester.pump(const Duration(seconds: 8));
    expect(tester.element(progress), same(original));
    expect(help.hitTestable(), findsOneWidget);
    expect(cancel.hitTestable(), findsOneWidget);
    final connectionHelp = find.byKey(const ValueKey('connection-help'));
    expect(connectionHelp.hitTestable(), findsOneWidget);
    await tester.tap(connectionHelp);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
    final helpPage = find.byKey(const ValueKey('bluetooth-connection-help'));
    expect(helpPage, findsOneWidget);
    expect(
      find.descendant(of: helpPage, matching: find.text('Studio Board')),
      findsOneWidget,
    );
    expect(find.textContaining('three success flashes'), findsOneWidget);
    final helpRoute = ModalRoute.of(tester.element(helpPage))!;
    Navigator.of(tester.element(helpPage)).pop();
    await tester.pump();
    await tester.pump(helpRoute.reverseTransitionDuration);
    await tester.pump();
    expect(helpPage, findsNothing);
    expect(tester.element(progress), same(original));
    expect(help, findsOneWidget);
    await tester.tap(help);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
    final report = find.byType(SupportReportScreen);
    expect(tester.widget<SupportReportScreen>(report).boardId, board.id);
    final attemptsAtReport = service.connectCalls.length;
    await tester.pump(const Duration(seconds: 12));
    expect(service.connectCalls.length, attemptsAtReport);
    expect(report, findsOneWidget);
    Navigator.of(tester.element(report)).pop();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
    expect(connect.hitTestable(), findsOneWidget);
    await tester.pump(const Duration(seconds: 6));
    expect(service.connectCalls.length, attemptsAtReport);
    await tester.tap(connect);
    await tester.pump();
    expect(progress, findsOneWidget);
    await tester.tap(cancel);
    await tester.pump();
    final attemptsAtCancel = service.connectCalls.length;
    await tester.pump(const Duration(seconds: 12));
    expect(service.connectCalls.length, attemptsAtCancel);
    expect(connect.hitTestable(), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final lateMode in [false, true]) {
    testWidgets(
      'cold Litchcraft keeps Connecting visible through an incomplete board read '
      '${lateMode ? "until current mode arrives" : "through retries beyond ten seconds"}',
      (tester) async {
        _usePortraitPhone(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{
          'last_connected_board_id': 'board-1',
          'litchcraft_discovery_dismissed': true,
        });
        // Use the same negotiated session as the other gate journeys, with a
        // retained owner and no Home snapshot before the delayed mode read.
        final peer = SettingsSessionBoard()
          ..state = 5
          ..flags = 7;
        peer.capabilityMask |=
            LitchDeviceCapabilities.litchcraftBit |
            LitchDeviceCapabilities.ownerPairingBit;
        final service = _ConnectionGateBleService(board: peer)
          ..settlePendingOnDisconnect = true;
        final craft = _NavigationCraftController(service)
          ..adoptGate = Completer<bool>();
        Future<void> waitFor(bool Function() ready) async {
          for (var turn = 0; turn < 80 && !ready(); turn++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          expect(ready(), isTrue);
        }

        final library = LitchcraftLibrary(craft);
        await library.ready;
        try {
          const board = LitchBoard(
            id: 'board-1',
            name: 'Studio Board',
            rssi: -40,
          );
          await tester.pumpWidget(
            MaterialApp(
              home: ConnectionGateScreen(
                bleService: service,
                craftController: craft,
                craftLibrary: library,
              ),
            ),
          );
          await tester.pump();
          await tester.tap(_navigationAction(tester, 'litchcraft'));
          await tester.pump();
          service.emitBoards(const [board]);
          await tester.pump(Duration.zero);
          await tester.pump();
          final dialog = find.byKey(
            const ValueKey('connection-progress-overlay'),
          );
          final help = find.byKey(const ValueKey('owner-pairing-support'));
          expect(service.connectCalls, [board.id]);
          expect(dialog, findsOneWidget);
          final original = tester.element(dialog);
          // This attempt ends without a connection event or a fresh advertisement.
          await tester.pump(const Duration(seconds: 1));
          service.completeNextConnect(false);
          service.emitBoards(const []);
          await tester.pump();
          expect(tester.element(dialog), same(original));
          service.emitBoards(const [board]);
          await tester.pump(Duration.zero);
          expect(service.connectCalls, [board.id]);
          await tester.pump(const Duration(seconds: 2));
          expect(service.connectCalls, [board.id, board.id]);
          await tester.pump(const Duration(seconds: 1));
          service.completeNextConnect(true);
          await waitFor(() => peer.stateQueries.contains(40));
          expect(peer.sessionActivated, isTrue);
          peer.setState(5, nextFlags: 7);
          await waitFor(() => service.productReady && craft.adoptCalls > 0);
          await tester.pump();
          expect(tester.element(dialog), same(original));
          await tester.pump(const Duration(seconds: 1));
          expect(help.hitTestable(), findsOneWidget);
          // Mode inspection can fail while the native link still reports connected.
          craft.adoptGate!.completeError(StateError('Board mode unavailable'));
          await tester.pump();
          await tester.pump();
          expect(service.isConnected, isTrue);
          expect(tester.element(dialog), same(original));
          expect(help.hitTestable(), findsOneWidget);
          if (lateMode) {
            craft.simulatePhysicalMode(true);
            await tester.pump();
            expect(dialog, findsNothing);
            expect(find.byType(LitchcraftScreen), findsOneWidget);
            await tester.pump(const Duration(seconds: 10));
            expect(service.isConnected, isTrue);
            expect(service.disconnectCalls, 0);
          } else {
            service.emitConnection(
              board.id,
              DeviceConnectionState.disconnected,
            );
            await tester.pump();
            expect(tester.element(dialog), same(original));
            expect(help.hitTestable(), findsOneWidget);
            await tester.pump(const Duration(seconds: 3));
            await tester.pump();
            expect(service.connectCalls, [board.id, board.id, board.id]);
            expect(tester.element(dialog), same(original));
            await tester.pump(const Duration(seconds: 4));
            await tester.pump();
            expect(tester.element(dialog), same(original));
            expect(help.hitTestable(), findsOneWidget);
            expect(
              find.byKey(const ValueKey('chosen-board-cancel')).hitTestable(),
              findsOneWidget,
            );
            service.completeNextConnect(false);
          }
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          craft.dispose();
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          await waitFor(() => disposed);
          await disposal;
        }
      },
    );
  }

  testWidgets('does not duplicate Contact Support while looking for boards', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    tester.view.physicalSize = const Size(320, 844);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);

    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(2)),
          child: child!,
        ),
        home: ConnectionGateScreen(bleService: service),
      ),
    );
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('contact-support-button')),
      findsNothing,
    );
    expect(find.byIcon(Icons.mail_outline), findsNothing);
    final Finder help = find.byKey(const ValueKey<String>('discovery-support'));
    expect(help, findsOneWidget);
    expect(find.text('Connection help'), findsOneWidget);
    expect(tester.getSize(help).height, greaterThanOrEqualTo(48));

    await tester.tap(help);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
    final helpPage = find.byKey(const ValueKey('bluetooth-connection-help'));
    expect(helpPage, findsOneWidget);
    expect(find.textContaining('one device at a time'), findsOneWidget);
    expect(find.textContaining('If it is not listed'), findsOneWidget);
    expect(
      find.textContaining('all phones, tablets and other devices'),
      findsOneWidget,
    );
    expect(find.textContaining('three success flashes'), findsOneWidget);
    expect(find.text('Contact Support'), findsOneWidget);
    expect(service.connectCalls, isEmpty);
    expect(service.disconnectCalls, 0);
    final support = find.descendant(
      of: helpPage,
      matching: find.byKey(const ValueKey('owner-pairing-support')),
    );
    await tester.ensureVisible(support);
    await tester.pump();
    expect(support.hitTestable(), findsOneWidget);
    expect(tester.getSize(support).height, greaterThanOrEqualTo(48));
    final helpRoute = ModalRoute.of(tester.element(helpPage))!;
    Navigator.of(tester.element(helpPage)).pop();
    await tester.pump();
    await tester.pump(helpRoute.reverseTransitionDuration);
    await tester.pump();
    expect(helpPage, findsNothing);

    await _pumpConnectedGate(tester, service);
    expect(
      find.byKey(const ValueKey<String>('contact-support-button')),
      findsNothing,
    );
  });

  for (final page in <String>['Console', 'Board Update']) {
    testWidgets('radio off from $page retains Console and exposes Connect', (
      WidgetTester tester,
    ) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{
        'enable_app_console': true,
      });
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(
        tester,
        service,
        firmwarePageBuilder: (_, _, _, _, _) =>
            const Text('Board Update test content'),
      );
      if (page == 'Console') {
        await tester.tap(_navigationAction(tester, 'console'));
        await tester.pumpAndSettle();
        expect(find.byType(AppConsoleScreen), findsOneWidget);
      } else {
        await tester.tap(
          find.byKey(const ValueKey('connected-board-status-action')),
        );
        await tester.pumpAndSettle();
        final strings = AppLocalizations.of(
          tester.element(find.byType(ConnectionGateScreen)),
        );
        await tester.tap(find.widgetWithText(ListTile, strings.boardSoftware));
        await tester.pumpAndSettle();
        expect(find.text('Board Update test content'), findsOneWidget);
      }

      service.emitAvailability(BleAvailability.poweredOff);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();

      expect(tester.takeException(), isNull);
      if (page == 'Console') {
        expect(find.byType(AppConsoleScreen), findsOneWidget);
        expect(
          tester
              .widget<TextField>(
                find.byKey(const ValueKey('console-command-input')),
              )
              .onSubmitted,
          isNull,
        );
        await tester.tap(_navigationAction(tester, 'play'));
        await tester.pumpAndSettle();
      }
      expect(find.byType(LitchMainScaffold), findsNothing);
      expect(
        find.byKey(const ValueKey('connection-gate-content')),
        findsOneWidget,
      );
      expect(find.text('Turn on Bluetooth to find boards.'), findsOneWidget);
      expect(_activeNavigationDestinationCount(tester), 4);
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        0,
        reason: 'The fallback navigator selects Connect, not a hidden page.',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  for (final boardCount in <int>[0, 1, 3]) {
    testWidgets('pull discovery refresh works with $boardCount boards', (
      tester,
    ) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      service.emitAvailability(BleAvailability.ready);
      service.emitBoards(<LitchBoard>[
        for (int index = 0; index < boardCount; ++index)
          LitchBoard(id: 'board-$index', name: 'Foxie Litch $index', rssi: -40),
      ]);
      await tester.pump();
      expect(find.byKey(const ValueKey('discovery-refresh')), findsOneWidget);
      await tester.drag(
        find.byKey(const PageStorageKey<String>('disconnected-play-scroll')),
        const Offset(0, 350),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(service.rescanCalls, 1);
      expect(service.connectCalls, isEmpty);
      expect(service.disconnectCalls, 0);
      await tester.pump(const Duration(milliseconds: 500));
      expect(
        service.rescanCalls,
        1,
        reason: 'One gesture requests one common rescan.',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  testWidgets(
    'discovery refresh is inactive while backgrounded, blocked, or on other pages',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      final refresh = find.byKey(const ValueKey('discovery-refresh'));
      final scroll = find.byKey(
        const PageStorageKey<String>('disconnected-play-scroll'),
      );
      expect(refresh, findsOneWidget);
      _sendAppToBackground(tester);
      await tester.pump();
      expect(refresh, findsOneWidget);
      await tester.drag(scroll, const Offset(0, 350));
      await tester.pump(const Duration(milliseconds: 300));
      expect(service.rescanCalls, 0);
      _sendAppToForeground(tester);
      await tester.pump();
      expect(refresh, findsOneWidget);
      service.emitAvailability(BleAvailability.bluetoothPermissionDenied);
      await tester.pump();
      expect(refresh, findsOneWidget);
      await tester.drag(scroll, const Offset(0, 350));
      await tester.pump(const Duration(milliseconds: 300));
      expect(service.rescanCalls, 0);
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(refresh, findsOneWidget);
      service.recoveryState = const BluetoothPairingRecoveryState(
        deviceId: 'board',
      );
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('pairing-recovery-board')),
        findsOneWidget,
      );
      expect(refresh, findsNothing);
      service.recoveryState = null;
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(refresh, findsOneWidget);
      for (final destination in <String>['litchcraft', 'setup']) {
        await tester.tap(_navigationAction(tester, destination));
        await tester.pumpAndSettle();
        expect(refresh, findsNothing);
      }
      expect(service.rescanCalls, 0);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'held discovery pull drains when refresh eligibility changes',
    (tester) async {
      _usePortraitPhone(tester);
      final stuck = <bool>[];
      for (final interruption in [
        'denied',
        'background',
        'unpainted background',
        'connecting',
      ]) {
        SharedPreferences.setMockInitialValues(<String, Object>{
          'last_connected_board_id': 'board-1',
        });
        final service = _ConnectionGateBleService();
        final repository = _EmptyGameLibraryRepository(
          snapshot: GameLibrarySnapshot(games: [_recentArchive(1)]),
        );
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              gameLibrary: repository,
            ),
          ),
        );
        service.emitAvailability(BleAvailability.ready);
        await tester.pump();
        await tester.pump();
        final recent = find.byType(RecentGamesSection);
        final recentState = tester.state(recent);
        final scroll = find.byKey(
          const PageStorageKey<String>('disconnected-play-scroll'),
        );
        final pointer = await tester.startGesture(
          tester.getTopLeft(scroll) + const Offset(40, 80),
        );
        await pointer.moveBy(const Offset(0, 80));
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveBy(const Offset(0, 140));
        await tester.pump(const Duration(milliseconds: 16));
        expect(find.byType(RefreshProgressIndicator), findsOneWidget);
        switch (interruption) {
          case 'denied':
            service.emitAvailability(BleAvailability.bluetoothPermissionDenied);
          case 'background':
            _sendAppToBackground(tester);
          case 'unpainted background':
            // No frame/scroll notification occurs between these callbacks.
            _sendAppToBackground(tester);
            _sendAppToForeground(tester);
          case 'connecting':
            service.emitBoards(const [
              LitchBoard(id: 'board-1', name: 'Studio Board', rssi: -40),
            ]);
        }
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 16));
        expect(tester.state(recent), same(recentState));
        await pointer.up();
        await tester.pump();
        // The app does not paint or advance indicator animations while paused.
        // Observe its recovery on return, after release occurred in background.
        if (interruption == 'background') _sendAppToForeground(tester);
        for (var frame = 0; frame < 40; ++frame) {
          await tester.pump(const Duration(milliseconds: 16));
        }
        stuck.add(find.byType(RefreshProgressIndicator).evaluate().isNotEmpty);
        expect(service.rescanCalls, 0);
        if (interruption == 'denied') {
          service.emitAvailability(BleAvailability.ready);
          await tester.pump();
          await tester.drag(scroll, const Offset(0, 350));
          await tester.pump(const Duration(milliseconds: 300));
          await tester.pump(const Duration(milliseconds: 300));
          expect(service.rescanCalls, 1);
          expect(tester.state(recent), same(recentState));
        }
        if (interruption == 'connecting') {
          expect(service.connectCalls, ['board-1']);
          service.completeNextConnect(false);
        }
        await tester.pumpWidget(const SizedBox());
        await service.dispose();
        await repository.close();
      }
      expect(
        stuck,
        everyElement(isFalse),
        reason:
            'A held pull must receive its end notification after denial, '
            'backgrounding or connection admission, without rescanning.',
      );
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets('discovery scroll survives refresh availability changes', (
    tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    service.emitAvailability(BleAvailability.ready);
    service.emitBoards(<LitchBoard>[
      for (int index = 0; index < 8; ++index)
        LitchBoard(id: 'board-$index', name: 'Foxie Litch $index', rssi: -40),
    ]);
    await tester.pump();
    final scroll = find.byKey(
      const PageStorageKey<String>('disconnected-play-scroll'),
    );
    ScrollableState scrollState() => tester.state<ScrollableState>(
      find.descendant(of: scroll, matching: find.byType(Scrollable)).first,
    );
    await tester.drag(scroll, const Offset(0, -250));
    await tester.pumpAndSettle();
    final retainedScroll = scrollState();
    final before = retainedScroll.position.pixels;
    expect(before, greaterThan(0));
    void expectRetainedDiscovery() {
      expect(scrollState(), same(retainedScroll));
      expect(
        retainedScroll.position.pixels,
        before.clamp(0, retainedScroll.position.maxScrollExtent),
        reason: 'Expired board cards may shorten the retained discovery list.',
      );
    }

    _sendAppToBackground(tester);
    await tester.pump();
    await tester.pump();
    expectRetainedDiscovery();
    _sendAppToForeground(tester);
    await tester.pump();
    await tester.pump();
    expectRetainedDiscovery();
    expect(find.byKey(const ValueKey('discovered-board-board-0')), findsNothing);
    expect(service.rescanCalls, 0);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'discovery refresh is inactive during connection and absent on connected pages',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            firmwarePageBuilder: (_, _, _, _, _) =>
                const SizedBox(key: ValueKey('refresh-test-firmware')),
          ),
        ),
      );
      await tester.pump();
      service.emitAvailability(BleAvailability.ready);
      service.emitBoards(const <LitchBoard>[
        LitchBoard(id: 'board', name: 'Foxie Litch', rssi: -40),
      ]);
      await tester.pump();
      expect(find.byKey(const ValueKey('discovery-refresh')), findsOneWidget);
      await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
      await tester.pump();
      expect(find.byKey(const ValueKey('discovery-refresh')), findsOneWidget);
      await tester.drag(
        find.byKey(const PageStorageKey<String>('disconnected-play-scroll')),
        const Offset(0, 350),
      );
      await tester.pump(const Duration(milliseconds: 300));
      expect(service.rescanCalls, 0);
      service.completeNextConnect(true);
      await tester.pumpAndSettle();
      for (final destination in <String>['play', 'litchcraft', 'setup']) {
        await tester.tap(_navigationAction(tester, destination));
        await tester.pumpAndSettle();
        expect(find.byKey(const ValueKey('discovery-refresh')), findsNothing);
      }
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('Check for updates').last);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('refresh-test-firmware')),
        findsOneWidget,
      );
      expect(find.byKey(const ValueKey('discovery-refresh')), findsNothing);
      expect(service.rescanCalls, 0);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Bluetooth availability replaces empty discovery and resumes it once ready',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);

      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      expect(service.startScanCalls, 1);

      service.emitAvailability(BleAvailability.bluetoothPermissionDenied);
      await tester.pump();
      expect(
        find.text('Allow Nearby devices and Bluetooth access to find boards.'),
        findsOneWidget,
      );
      expect(find.text('Looking for the undead...'), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('discovery-support')),
        findsOneWidget,
      );
      final Finder recovery = find.byKey(
        const ValueKey<String>('bluetooth-recovery'),
      );
      expect(recovery, findsOneWidget);
      expect(tester.getSize(recovery).height, greaterThanOrEqualTo(48));

      await tester.pump(const Duration(seconds: 15));
      expect(
        service.requestBluetoothAccessCalls,
        0,
        reason:
            'periodic discovery retries must not reopen a permission prompt',
      );
      await tester.tap(recovery);
      await tester.pump();
      expect(service.requestBluetoothAccessCalls, 1);

      final int passiveScanCalls = service.startScanCalls;
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(find.text('Looking for the undead...'), findsOneWidget);

      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(service.startScanCalls, passiveScanCalls);
    },
  );

  for (final (availability, message, action, opensSettings)
      in <(BleAvailability, String, String, bool)>[
        (
          BleAvailability.bluetoothPermissionBlocked,
          'Nearby devices and Bluetooth access are blocked. Open Settings to allow them.',
          'Open Settings',
          true,
        ),
        (
          BleAvailability.poweredOff,
          'Turn on Bluetooth to find boards.',
          'Try again',
          false,
        ),
      ]) {
    testWidgets('blocked BLE after connection exposes $availability recovery', (
      WidgetTester tester,
    ) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);

      service.emitAvailability(
        availability,
        recoveryNeedsSettings: opensSettings,
      );
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();

      expect(find.byType(LitchMainScaffold), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('connection-gate-content')),
        findsOneWidget,
      );
      expect(find.text(message), findsOneWidget);
      expect(find.widgetWithText(FilledButton, action), findsOneWidget);
      expect(find.text('Connecting…'), findsNothing);
      expect(
        find.byKey(const ValueKey('connected-board-mode-progress')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('chosen-board-connection')),
        findsNothing,
      );
    });
  }

  testWidgets('blocked discovery discards stale boards until rediscovered', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    const LitchBoard board = LitchBoard(
      id: 'board-1',
      name: 'Foxie One',
      rssi: -40,
    );

    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    service.emitBoards(const <LitchBoard>[board]);
    await tester.pump();
    expect(find.textContaining('Foxie One'), findsOneWidget);

    service.emitAvailability(BleAvailability.poweredOff);
    await tester.pump();
    expect(find.textContaining('Foxie One'), findsNothing);
    expect(find.widgetWithText(OutlinedButton, 'Identify'), findsNothing);

    service.emitAvailability(BleAvailability.ready);
    await tester.pump();
    expect(find.text('Looking for the undead...'), findsOneWidget);
    expect(find.textContaining('Foxie One'), findsNothing);

    service.emitBoards(const <LitchBoard>[board]);
    await tester.pump();
    expect(find.textContaining('Foxie One'), findsOneWidget);
  });

  testWidgets('blocked Location access offers Open Settings', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);

    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump();
    service.emitAvailability(
      BleAvailability.locationPermissionBlocked,
      recoveryNeedsSettings: true,
    );
    await tester.pump();

    expect(
      find.text('Location access is blocked. Open Settings to allow it.'),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const ValueKey<String>('bluetooth-recovery')));
    await tester.pump();
    expect(service.openBluetoothSettingsCalls, 1);
    expect(service.requestBluetoothAccessCalls, 0);
  });

  for (final double width in <double>[320, 390, 412]) {
    testWidgets(
      'disconnected board list stays usable at ${width.toInt()}px and 200% text',
      (WidgetTester tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final _ConnectionGateBleService service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        await tester.binding.setSurfaceSize(Size(width, 700));
        addTearDown(() => tester.binding.setSurfaceSize(null));

        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            builder: (BuildContext context, Widget? child) {
              return MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: const TextScaler.linear(2)),
                child: child!,
              );
            },
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.pump();
        service.emitBoards(const <LitchBoard>[
          LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
          LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -45),
        ]);
        await tester.pumpAndSettle();

        expect(find.text('Not connected'), findsOneWidget);
        expect(find.textContaining('Foxie One'), findsOneWidget);
        for (final name in <String>['Foxie One', 'Foxie Two']) {
          final card = find.ancestor(
            of: find.textContaining(name),
            matching: find.byType(Card),
          );
          final connect = find.descendant(
            of: card,
            matching: find.widgetWithText(FilledButton, 'Connect'),
          );
          expect(connect, findsOneWidget);
          await tester.ensureVisible(connect);
          await tester.pumpAndSettle();
          expect(connect.hitTestable(), findsOneWidget);
          final size = tester.getSize(connect);
          expect(size.width, greaterThanOrEqualTo(48));
          expect(size.height, greaterThanOrEqualTo(48));
        }
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'rapid inactive then resumed cannot let stale background work disconnect',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);

      service.delayNextBackgroundTransition();
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await tester.pump();
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pump();
      service.completeBackgroundTransition();
      await tester.pump();

      expect(service.disconnectCalls, 0);
      expect(service.isConnected, isTrue);
      expect(service.foregroundCalls, isEmpty);
      expect(find.text('Connected'), findsOneWidget);
    },
  );

  for (final bool dark in <bool>[false, true]) {
    for (final bool updateAvailable in <bool>[false, true]) {
      for (final double textScale in <double>[1, 2]) {
        testWidgets('connection press keeps label paint and contrast '
            '${dark ? 'dark' : 'light'} ${updateAvailable ? 'update' : 'healthy'} '
            '${(textScale * 100).round()}%', (WidgetTester tester) async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          tester.view.physicalSize = const Size(384, 832);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          final _ConnectionGateBleService service = _ConnectionGateBleService(
            useProductionConsoleStream: true,
          )..reviewBoardRotationKnown = true;
          addTearDown(service.dispose);
          late ValueChanged<FirmwareUpdateAvailability> publishAvailability;
          await _pumpConnectedGate(
            tester,
            service,
            theme: dark ? buildLitchDarkTheme() : buildLitchTheme(),
            textScaler: TextScaler.linear(textScale),
            firmwarePageBuilder: (_, _, _, _, onAvailabilityChanged) {
              publishAvailability = onAvailabilityChanged;
              return const SizedBox.shrink();
            },
          );
          publishAvailability(
            FirmwareUpdateAvailability(
              installedVersion: '1.0.0',
              latestVersion: updateAvailable ? '1.0.1' : '1.0.0',
              supportsUpdate: true,
              loading: false,
            ),
          );
          service.reportHome();
          await tester.pumpAndSettle();
          await decodeVisibleReviewImages(tester);
          final Finder status = find.byKey(
            const ValueKey<String>('connected-board-status-live-region'),
          );
          final Finder feedback = find.byKey(
            const ValueKey<String>('connected-board-status-feedback'),
          );
          final Rect target = tester.getRect(status);
          final Map<String, Rect> labels = <String, Rect>{
            'Connected': tester.getRect(find.text('Connected')),
            if (updateAvailable)
              'Update available': tester.getRect(find.text('Update available')),
          };
          expect(target.height, greaterThanOrEqualTo(48));
          expect(target.width, greaterThanOrEqualTo(48));
          final String renderName =
              'connected-${dark ? 'dark' : 'light'}-'
              '${updateAvailable ? 'update' : 'healthy'}-${(textScale * 100).round()}';
          final Map<String, double> contrast = <String, double>{};
          Future<void> sample(String state) async {
            await captureHealthReview(
              tester,
              '$renderName-${state.replaceAll(' ', '-')}',
            );
            contrast.addAll(
              await _labelContrasts(
                tester,
                target,
                labels,
                '$renderName $state',
              ),
            );
          }

          final ui.Image resting = await _captureConnectedGate(tester);
          addTearDown(resting.dispose);
          await sample('resting');
          final TestGesture gesture = await tester.startGesture(target.center);
          await tester.pump(const Duration(milliseconds: 100));
          await tester.pump(const Duration(milliseconds: 100));
          expect(tester.widget<AnimatedOpacity>(feedback).opacity, lessThan(1));
          final ui.Image pressed = await _captureConnectedGate(tester);
          addTearDown(pressed.dispose);
          await sample('held');
          await gesture.cancel();
          await tester.pumpAndSettle();
          expect(tester.widget<AnimatedOpacity>(feedback).opacity, 1);
          expect(find.text('Disconnect'), findsNothing);
          expect(service.disconnectCalls, 0);
          await _expectOnlyLabelsChanged(
            tester,
            resting,
            pressed,
            target,
            <Rect>[
              ...labels.values.map((Rect rect) => rect.inflate(1)),
              tester
                  .getRect(
                    find.byKey(
                      const ValueKey<String>('connected-board-status-chevron'),
                    ),
                  )
                  .inflate(1),
            ],
          );
          await tester.drag(find.byType(ListView).first, const Offset(0, -240));
          await tester.pumpAndSettle();
          expect(
            tester
                .widget<Material>(
                  find
                      .descendant(
                        of: find.byType(AppBar),
                        matching: find.byType(Material),
                      )
                      .first,
                )
                .elevation,
            1,
          );
          await sample('scrolled');
          final TestGesture scrolledGesture = await tester.startGesture(
            target.center,
          );
          await tester.pump(const Duration(milliseconds: 100));
          await tester.pump(const Duration(milliseconds: 100));
          await sample('scrolled held');
          await scrolledGesture.cancel();
          await tester.pumpAndSettle();
          if (textScale == 1) {
            // Keep keyboard focus visible while a pointer is held, as on a
            // keyboard-driven surface. The overlay is part of the background.
            final FocusHighlightStrategy previousStrategy =
                FocusManager.instance.highlightStrategy;
            FocusManager.instance.highlightStrategy =
                FocusHighlightStrategy.alwaysTraditional;
            addTearDown(() {
              FocusManager.instance.highlightStrategy = previousStrategy;
            });
            final InkWell action = tester.widget<InkWell>(
              find.byKey(
                const ValueKey<String>('connected-board-status-action'),
              ),
            );
            action.focusNode!.requestFocus();
            await tester.pumpAndSettle();
            expect(action.focusNode!.hasFocus, isTrue);
            await sample('focused');
            final TestGesture focusedGesture = await tester.startGesture(
              target.center,
            );
            await tester.pump(const Duration(milliseconds: 100));
            await tester.pump(const Duration(milliseconds: 100));
            await sample('focused held');
            await focusedGesture.cancel();
            await tester.pumpAndSettle();
            final TestGesture mouse = await tester.createGesture(
              kind: ui.PointerDeviceKind.mouse,
            );
            await mouse.addPointer(location: const Offset(1, 1));
            await mouse.moveTo(target.center);
            await tester.pumpAndSettle();
            await mouse.down(target.center);
            await tester.pump(const Duration(milliseconds: 100));
            await tester.pump(const Duration(milliseconds: 100));
            await sample('focused hovered held');
            // A focus change while still held must keep the pointer-owned fade
            // and the independent hover treatment until each state clears.
            action.focusNode!.unfocus();
            await tester.pumpAndSettle();
            expect(
              tester.widget<AnimatedOpacity>(feedback).opacity,
              lessThan(1),
            );
            await sample('hovered held');
            await mouse.cancel();
            await mouse.moveTo(const Offset(1, 1));
            await mouse.removePointer();
            await tester.pumpAndSettle();
            expect(tester.widget<AnimatedOpacity>(feedback).opacity, 1);
            expect(find.text('Disconnect'), findsNothing);
            await sample('recovered');
          }
          for (final MapEntry<String, double> sample in contrast.entries) {
            expect(sample.value, greaterThanOrEqualTo(4.5), reason: sample.key);
          }
          expect(tester.takeException(), isNull);
        });
      }
    }
  }

  testWidgets(
    'status labels remain readable through scrolled focus and hover exits',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      tester.view.physicalSize = const Size(384, 832);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await loadHealthReviewFonts(tester);
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      late ValueChanged<FirmwareUpdateAvailability> publishAvailability;
      await _pumpConnectedGate(
        tester,
        service,
        theme: buildLitchTheme(),
        firmwarePageBuilder: (_, _, _, _, onAvailabilityChanged) {
          publishAvailability = onAvailabilityChanged;
          return const SizedBox.shrink();
        },
      );
      publishAvailability(
        const FirmwareUpdateAvailability(
          installedVersion: '1.0.0',
          latestVersion: '1.0.1',
          supportsUpdate: true,
          loading: false,
        ),
      );
      await tester.pumpAndSettle();
      await tester.drag(find.byType(ListView).first, const Offset(0, -240));
      await tester.pumpAndSettle();

      final Finder status = find.byKey(
        const ValueKey<String>('connected-board-status-live-region'),
      );
      final Rect target = tester.getRect(status);
      final Map<String, Rect> labels = <String, Rect>{
        'Connected': tester.getRect(find.text('Connected')),
        'Update available': tester.getRect(find.text('Update available')),
      };
      Future<void> expectReadable(String state) async {
        await captureHealthReview(tester, 'focus-exit-$state');
        final Map<String, double> contrast = await _labelContrasts(
          tester,
          target,
          labels,
          'focus-exit $state',
        );
        for (final MapEntry<String, double> sample in contrast.entries) {
          expect(sample.value, greaterThanOrEqualTo(4.5), reason: sample.key);
        }
      }

      final FocusHighlightStrategy previousStrategy =
          FocusManager.instance.highlightStrategy;
      FocusManager.instance.highlightStrategy =
          FocusHighlightStrategy.alwaysTraditional;
      addTearDown(() {
        FocusManager.instance.highlightStrategy = previousStrategy;
      });
      final InkWell action = tester.widget<InkWell>(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      action.focusNode!.requestFocus();
      await tester.pumpAndSettle();
      action.focusNode!.unfocus();
      await tester.pump();
      await tester.pump();
      await expectReadable('focus-exit-0ms');
      await tester.pump(const Duration(milliseconds: 16));
      await expectReadable('focus-exit-16ms');
      await tester.pump(const Duration(milliseconds: 34));
      await expectReadable('focus-exit-50ms');
      await tester.pump(const Duration(milliseconds: 30));
      await expectReadable('focus-exit-80ms');
      final Color ordinaryColor = Theme.of(
        tester.element(status),
      ).colorScheme.secondary;
      for (final String label in labels.keys) {
        final RenderParagraph paragraph = tester.renderObject(find.text(label));
        expect(paragraph.text.style!.color, ordinaryColor);
      }

      action.focusNode!.requestFocus();
      await tester.pumpAndSettle();
      action.focusNode!.unfocus();
      await tester.binding.delayed(const Duration(milliseconds: 100));
      await tester.pump();
      await expectReadable('focus-exit-delayed-first-frame');

      action.focusNode!.requestFocus();
      await tester.pump();
      await expectReadable('focus-reentry-after-old-deadline');
      action.focusNode!.unfocus();
      await tester.pumpAndSettle();

      final TestGesture mouse = await tester.createGesture(
        kind: ui.PointerDeviceKind.mouse,
      );
      await mouse.addPointer(location: const Offset(1, 1));
      await mouse.moveTo(target.center);
      await tester.pumpAndSettle();
      await mouse.moveTo(const Offset(1, 1));
      await tester.pump();
      await tester.pump();
      await expectReadable('hover-exit-0ms');

      await mouse.moveTo(target.center);
      await tester.pumpAndSettle();
      await mouse.moveTo(const Offset(1, 1));
      await tester.pump();
      action.focusNode!.requestFocus();
      await tester.pump();
      await expectReadable('hover-exit-cancelled-by-focus');
      await tester.pump(const Duration(milliseconds: 80));
      await expectReadable('hover-exit-cancelled-after-old-deadline');
      await mouse.removePointer();
      action.focusNode!.unfocus();
      await tester.pump();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.takeException(), isNull);
      expect(service.disconnectCalls, 0);
      expect(find.text('Disconnect'), findsNothing);
    },
  );

  for (final scenario in <(Locale, String, String, String)>[
    (const Locale('en'), 'Play', 'Setup', 'Connected'),
    (const Locale('en', 'GB'), 'Play', 'Setup', 'Connected'),
    (const Locale('es'), 'Jugar', 'Configurar', 'Conectado'),
    (const Locale('de'), 'Spielen', 'Einrichten', 'Verbunden'),
    (const Locale('it'), 'Gioca', 'Configura', 'Connesso'),
    (const Locale('fr'), 'Jouer', 'Configurer', 'Connecté'),
  ]) {
    for (final scale in <double>[1, 2]) {
      for (final width in <double>[320, 384]) {
        testWidgets(
          'localized board navigation ${scenario.$1} at $scale text $width px',
          (tester) async {
            SharedPreferences.setMockInitialValues(<String, Object>{});
            tester.view.physicalSize = Size(width, 832);
            tester.view.devicePixelRatio = 1;
            tester.platformDispatcher.localesTestValue = <Locale>[scenario.$1];
            addTearDown(tester.view.resetPhysicalSize);
            addTearDown(tester.view.resetDevicePixelRatio);
            addTearDown(tester.platformDispatcher.clearLocalesTestValue);
            final service = _ConnectionGateBleService();
            addTearDown(service.dispose);
            await loadHealthReviewFonts(tester, useSdkFonts: true);
            late ValueChanged<FirmwareUpdateAvailability> publishAvailability;
            await _pumpConnectedGate(
              tester,
              service,
              theme: buildLitchTheme(),
              textScaler: TextScaler.linear(scale),
              firmwarePageBuilder: (_, _, _, _, onAvailabilityChanged) {
                publishAvailability = onAvailabilityChanged;
                return const SizedBox.shrink();
              },
            );
            await tester.pumpAndSettle();
            expect(find.text(scenario.$2), findsOneWidget);
            expect(find.text(scenario.$3), findsOneWidget);
            expect(find.text(scenario.$4), findsOneWidget);
            final strings = lookupAppLocalizations(scenario.$1);
            final captureName =
                'shell-${scenario.$1}-${(scale * 100).toInt()}-${width.toInt()}';
            expect(find.byTooltip(strings.aboutApp), findsOneWidget);
            final Finder brand = find.byKey(
              const ValueKey<String>('open-about'),
            );
            final Rect toolbar = tester.getRect(find.byType(AppBar));
            expect(tester.getSize(brand).height, greaterThanOrEqualTo(48));
            final Finder artwork = find.descendant(
              of: brand,
              matching: find.byType(Image),
            );
            expect(artwork, findsNWidgets(2));
            for (final Element element in artwork.evaluate()) {
              final Rect rect = tester.getRect(find.byWidget(element.widget));
              expect(rect.left, greaterThanOrEqualTo(toolbar.left));
              expect(rect.right, lessThanOrEqualTo(toolbar.right));
              expect(rect.top, greaterThanOrEqualTo(toolbar.top));
              expect(rect.bottom, lessThanOrEqualTo(toolbar.bottom));
            }
            final navigationBounds = tester.getRect(find.byType(NavigationBar));
            for (final key in <String>[
              'main-destination-play',
              'main-destination-craft',
              'main-destination-setup',
            ]) {
              final destination = find.byKey(ValueKey<String>(key));
              expect(destination.hitTestable(), findsOneWidget);
              final destinationBounds = tester.getRect(destination);
              expect(
                navigationBounds.contains(destinationBounds.center),
                isTrue,
                reason: key,
              );
            }
            final action = find.byKey(
              const ValueKey<String>('connected-board-status-action'),
            );
            final status = find.byKey(
              const ValueKey<String>('connected-board-status-live-region'),
            );
            expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
            expect(
              tester.widget<Semantics>(status).properties.label,
              strings.connectedBoardOptions,
            );
            await captureHealthReview(tester, '$captureName-healthy');
            await tester.tap(action);
            await tester.pumpAndSettle();
            expect(find.text(strings.boardSoftware), findsOneWidget);
            expect(find.text(strings.boardSoftwareDescription), findsNothing);
            expect(find.text(strings.disconnectAction), findsOneWidget);
            for (final label in <String>[
              strings.boardSoftware,
              strings.sleepAction,
              strings.disconnectAction,
            ]) {
              await tester.ensureVisible(find.text(label));
              await tester.pumpAndSettle();
              _expectLabelContained(
                tester,
                label,
                Offset.zero & tester.view.physicalSize,
              );
            }
            tester.state<NavigatorState>(find.byType(Navigator)).pop();
            await tester.pumpAndSettle();
            publishAvailability(
              const FirmwareUpdateAvailability(
                installedVersion: '1.0.0',
                latestVersion: '1.0.1',
                supportsUpdate: true,
                loading: false,
              ),
            );
            await tester.pumpAndSettle();
            expect(find.text(strings.updateAvailable), findsOneWidget);
            expect(
              tester.widget<Semantics>(status).properties.label,
              strings.connectedUpdateBoardOptions,
            );
            await captureHealthReview(tester, '$captureName-update');
            for (final label in <String>[
              scenario.$4,
              strings.updateAvailable,
            ]) {
              final paragraph = tester.renderObject<RenderParagraph>(
                find.descendant(
                  of: find.text(label),
                  matching: find.byType(RichText),
                ),
              );
              expect(paragraph.didExceedMaxLines, isFalse, reason: label);
            }
            await tester.tap(action);
            await tester.pumpAndSettle();
            await captureHealthReview(tester, '$captureName-menu');
            expect(
              find.text('${strings.currentFirmwareVersion('1.0.0')} → 1.0.1'),
              findsOneWidget,
            );
            expect(find.text(strings.boardSoftwareDescription), findsNothing);
            final updateAction = find.text(strings.boardSoftware);
            await tester.ensureVisible(updateAction);
            await tester.pumpAndSettle();
            await tester.tap(updateAction);
            await tester.pumpAndSettle();
            expect(find.text(strings.boardUpdateTitle), findsOneWidget);
            await captureHealthReview(tester, '$captureName-update-page');
            _expectLabelContained(
              tester,
              strings.boardUpdateTitle,
              tester.getRect(find.byType(AppBar)),
            );
            expect(
              tester
                  .getSize(find.byKey(const ValueKey<String>('firmware-back')))
                  .shortestSide,
              greaterThanOrEqualTo(48),
            );
            await tester.tap(
              find.byKey(const ValueKey<String>('firmware-back')),
            );
            await tester.pumpAndSettle();
            expect(find.text(scenario.$2), findsOneWidget);
            expect(service.connectCalls, <String>['board-1']);
            expect(service.disconnectCalls, 0);
            expect(tester.takeException(), isNull);
            await tester.pumpWidget(const SizedBox.shrink());
          },
        );
      }
    }
  }

  testWidgets(
    'navigation controls retain Setup and show Console history received before first entry',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      SharedPreferences.setMockInitialValues(<String, Object>{
        'enable_app_console': true,
      });
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.pumpAndSettle();

      await _selectAdjacentMainPage(
        tester,
        forward: true,
      ); // Play → Litchcraft.
      expect(_activeNavigationIndex(tester), 1);
      final PageView pager = tester.widget<PageView>(
        find.byKey(const ValueKey<String>('main-pages')),
      );
      expect(pager.allowImplicitScrolling, isFalse);
      expect(
        service.settingsReadCalls,
        0,
        reason: 'Setup shell is warm without hidden BLE enumeration',
      );
      final setupState = tester.state(
        find.byType(SettingsScreen, skipOffstage: false),
      );
      service.debugAppendConsolePacket(
        'before first Console visit\n'.codeUnits,
      );
      await tester.pump();

      await _selectAdjacentMainPage(
        tester,
        forward: true,
        afterSelection: () => service.reportHealth(
          storageFailed: true,
          resetReason: 6,
          watchdogRestarts: 1,
        ),
      );
      expect(_activeNavigationIndex(tester), 2);
      expect(tester.state(find.byType(SettingsScreen)), same(setupState));
      expect(service.settingsReadCalls, 1);

      await _selectAdjacentMainPage(tester, forward: true);
      expect(_activeNavigationIndex(tester), 3);
      expect(find.textContaining('before first Console visit'), findsOneWidget);
      final consoleState = tester.state(find.byType(AppConsoleScreen));
      final Finder command = find.descendant(
        of: find.byType(AppConsoleScreen),
        matching: find.byType(TextField),
      );
      await tester.enterText(command, 'help');
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.pumpAndSettle();

      await _selectAdjacentMainPage(tester, forward: false);
      expect(tester.state(find.byType(SettingsScreen)), same(setupState));
      await _selectAdjacentMainPage(tester, forward: true);
      expect(tester.state(find.byType(AppConsoleScreen)), same(consoleState));
      expect(tester.widget<TextField>(command).controller!.text, 'help');
      expect(service.connectCalls, <String>['board-1']);
      expect(service.disconnectCalls, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'CPU slider retains the finger preview inside the pager during live rebuilds',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.pumpAndSettle();
      await _selectAdjacentMainPage(
        tester,
        forward: true,
      ); // Play → Litchcraft.
      await _selectAdjacentMainPage(tester, forward: true);
      final Finder slider = find.byKey(
        const ValueKey<String>('setting-editor:VS_CPU_LEVEL'),
      );
      await tester.ensureVisible(slider);
      await tester.pumpAndSettle();
      final Rect bounds = tester.getRect(slider);
      final Finder playerChoice = find.byKey(
        const ValueKey<String>('quick:VS_CPU_PLAYER:visibility'),
      );
      final PageView pager = tester.widget<PageView>(
        find.byKey(const ValueKey<String>('main-pages')),
      );
      final TestGesture gesture = await tester.startGesture(
        Offset(bounds.left + 24, bounds.center.dy),
      );
      await gesture.moveTo(
        Offset(bounds.left + bounds.width * 0.55, bounds.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final double firstPreview = tester.widget<Slider>(slider).value;
      expect(firstPreview, greaterThan(0));
      final double previewTop = tester.getTopLeft(slider).dy;

      service.boardValues['VS_CPU_LEVEL'] = 0;
      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'VS_CPU_LEVEL',
        'v': 0,
      });
      // This reaches Home's health callback and rebuilds the parent scaffold.
      service.reportHealth(
        storageFailed: true,
        resetReason: 6,
        watchdogRestarts: 1,
      );
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, firstPreview);
      expect(tester.getTopLeft(slider).dy, previewTop);
      expect(pager.controller!.page, moreOrLessEquals(2));

      await gesture.moveTo(Offset(bounds.left + 24, bounds.center.dy));
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, 0);
      expect(tester.widget<Visibility>(playerChoice).visible, isFalse);
      expect(tester.widget<Visibility>(playerChoice).maintainSize, isTrue);
      service.boardValues['VS_CPU_LEVEL'] = 10;
      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'VS_CPU_LEVEL',
        'v': 10,
      });
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, 0);
      expect(tester.widget<Visibility>(playerChoice).visible, isFalse);
      await gesture.moveTo(Offset(bounds.right - 24, bounds.center.dy));
      await tester.pump(const Duration(milliseconds: 16));
      final int finalPreview = tester.widget<Slider>(slider).value.round();
      expect(finalPreview, greaterThan(firstPreview));
      expect(tester.widget<Visibility>(playerChoice).visible, isTrue);
      expect(service.settingWrites.where((write) => write.awaitAck), isEmpty);
      expect(pager.controller!.page, moreOrLessEquals(2));

      await gesture.up();
      await tester.pumpAndSettle();
      expect(service.settingWrites.last.value, finalPreview);
      expect(service.settingWrites.last.awaitAck, isTrue);
      expect(tester.widget<Slider>(slider).value.round(), finalPreview);
      expect(tester.widget<Visibility>(playerChoice).maintainSize, isFalse);
      expect(_activeNavigationIndex(tester), 2);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'disabling Console after selecting it returns to the retained Setup',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final List<Completer<void>> savedPreferences = <Completer<void>>[];
      final List<bool> saves = <bool>[];
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: service.disconnect,
            loadAppConsolePreference: () async => true,
            saveAppConsolePreference: (bool enabled) {
              saves.add(enabled);
              final saved = Completer<void>();
              savedPreferences.add(saved);
              return saved.future;
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _selectAdjacentMainPage(
        tester,
        forward: true,
      ); // Play → Litchcraft.
      await _selectAdjacentMainPage(tester, forward: true);
      final setupState = tester.state(find.byType(SettingsScreen));
      final developer = find.byKey(
        const ValueKey<String>('setup-open:developer'),
      );
      await tester.scrollUntilVisible(
        developer,
        300,
        scrollable: find
            .descendant(
              of: find.byKey(const ValueKey<String>('settings-scroll')),
              matching: find.byType(Scrollable),
            )
            .first,
      );
      await tester.pumpAndSettle();
      await tester.tap(developer);
      await tester.pumpAndSettle();
      expect(find.text('Developer tools'), findsOneWidget);
      final Finder toggle = find.byKey(
        const ValueKey<String>('enable-app-console'),
      );
      await tester.ensureVisible(toggle);
      await tester.pumpAndSettle();
      await tester.tap(toggle);
      await tester.pump();
      expect(saves, <bool>[false]);
      expect(tester.widget<Switch>(toggle).value, isFalse);
      expect(tester.widget<Switch>(toggle).onChanged, isNotNull);
      for (final preview in <bool>[true, false, true]) {
        await tester.tap(toggle);
        await tester.pump();
        expect(tester.widget<Switch>(toggle).value, preview);
        expect(saves, <bool>[false]);
      }
      savedPreferences[0].complete();
      await tester.pump();
      expect(saves, <bool>[false, true]);
      expect(tester.widget<Switch>(toggle).value, isTrue);
      savedPreferences[1].complete();
      await tester.pumpAndSettle();
      expect(tester.widget<Switch>(toggle).value, isTrue);
      expect(_activeNavigationDestinationCount(tester), 4);

      await tester.tap(toggle);
      await tester.pump();
      expect(tester.widget<Switch>(toggle).value, isFalse);
      expect(saves, <bool>[false, true, false]);

      await _selectAdjacentMainPage(tester, forward: true);
      expect(_activeNavigationIndex(tester), 3);
      savedPreferences[2].complete();
      await tester.pumpAndSettle();
      expect(_activeNavigationDestinationCount(tester), 3);
      expect(_activeNavigationIndex(tester), 2);
      expect(
        tester
            .widget<PageView>(
              find.byWidgetPredicate((widget) => widget is PageView),
            )
            .controller!
            .page,
        moreOrLessEquals(2),
      );
      expect(tester.state(find.byType(SettingsScreen)), same(setupState));
      expect(tester.widget<Switch>(toggle).value, isFalse);
      await _selectAdjacentMainPage(tester, forward: false);
      expect(_activeNavigationIndex(tester), 1);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'language changes preserve connected navigation and open options',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('en')];
      addTearDown(tester.platformDispatcher.clearLocalesTestValue);
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      await tester.pumpAndSettle();
      final scaffold = tester.state(find.byType(LitchMainScaffold));
      final navigator = Navigator.of(
        tester.element(find.byType(LitchMainScaffold)),
        rootNavigator: true,
      );
      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      expect(_activeNavigationIndex(tester), 2);
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('de')];
      await tester.pumpAndSettle();
      expect(find.text('Spielen'), findsOneWidget);
      expect(find.text('Verbunden'), findsOneWidget);
      expect(tester.state(find.byType(LitchMainScaffold)), same(scaffold));
      expect(_activeNavigationIndex(tester), 2);
      await tester.tap(
        find.byKey(const ValueKey<String>('connected-board-status-action')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Trennen'), findsOneWidget);
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('fr')];
      await tester.pumpAndSettle();
      expect(find.text('Déconnecter'), findsOneWidget);
      expect(
        Navigator.of(
          tester.element(find.byType(LitchMainScaffold)),
          rootNavigator: true,
        ),
        same(navigator),
      );
      navigator.pop();
      await tester.pumpAndSettle();
      expect(find.text('Jouer'), findsOneWidget);
      expect(service.connectCalls, <String>['board-1']);
      expect(service.disconnectCalls, 0);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('connected board options support keyboard activation', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);

    final InkWell action = tester.widget<InkWell>(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    action.focusNode!.requestFocus();
    await tester.pump();
    expect(action.focusNode!.hasFocus, isTrue);

    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pumpAndSettle();

    expect(find.text('Check for updates'), findsOneWidget);
    expect(find.text('Disconnect'), findsOneWidget);
  });

  testWidgets('main navigation keeps destinations above the system inset', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    const double bottomSystemInset = 34;
    await _pumpConnectedGate(
      tester,
      service,
      bottomSystemInset: bottomSystemInset,
      layoutSize: const Size(599, 600),
    );

    final Rect scaffold = tester.getRect(find.byType(Scaffold).first);
    final Rect navigation = tester.getRect(find.byType(NavigationBar));
    final Finder surfaceFinder = find.byKey(
      const ValueKey<String>('main-navigation-surface'),
    );
    final Rect surface = tester.getRect(surfaceFinder);
    final playTarget = find.byKey(const ValueKey('main-destination-play'));
    final Rect playBounds = tester.getRect(playTarget);
    expect(playTarget.hitTestable(), findsOneWidget);
    expect(playBounds.height, greaterThanOrEqualTo(48));
    expect(
      navigation.bottom,
      moreOrLessEquals(scaffold.bottom - bottomSystemInset),
    );
    expect(
      playBounds.bottom,
      lessThanOrEqualTo(scaffold.bottom - bottomSystemInset),
    );
    expect(surface.bottom, scaffold.bottom);
    expect(surface.top, navigation.top);
    expect(surface.height, navigation.height + bottomSystemInset);
    expect(
      tester.widget<ColoredBox>(surfaceFinder).color,
      Theme.of(tester.element(surfaceFinder)).colorScheme.surfaceContainer,
    );
  });

  testWidgets('board options popover anchors beside status within safe bounds', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    const double bottomSystemInset = 34;
    await _pumpConnectedGate(
      tester,
      service,
      bottomSystemInset: bottomSystemInset,
    );

    final status = find.byKey(
      const ValueKey<String>('connected-board-status-action'),
    );
    final anchor = tester.getRect(status);
    await tester.tap(status);
    await tester.pumpAndSettle();

    final Rect scaffold = tester.getRect(find.byType(Scaffold).first);
    final Rect sheetContent = tester.getRect(
      find.byKey(const ValueKey<String>('board-menu-scroll')),
    );
    expect(
      sheetContent.bottom,
      lessThanOrEqualTo(scaffold.bottom - bottomSystemInset - 8),
      reason:
          'The natural-height popover must remain above the protected platform inset.',
    );
    expect(sheetContent.top, anchor.bottom + 4);
    expect(sheetContent.right, lessThanOrEqualTo(scaffold.right - 8));
    expect(
      find.byKey(const ValueKey('board-menu-rotate')).hitTestable(),
      findsOneWidget,
    );
  });

  testWidgets(
    'system back returns from Board update to the originating Setup',
    (WidgetTester tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      const double bottomSystemInset = 34;
      await _pumpConnectedGate(
        tester,
        service,
        bottomSystemInset: bottomSystemInset,
      );

      await tester.tap(_navigationAction(tester, 'setup'));
      await tester.pumpAndSettle();
      final origin = tester.state(find.byType(SettingsScreen));

      Future<void> openUpdate() async {
        await tester.tap(
          find.byKey(const ValueKey<String>('connected-board-status-action')),
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find.descendant(
            of: find.byKey(const ValueKey('connected-board-popover')),
            matching: find.text('Check for updates'),
          ),
        );
        await tester.pumpAndSettle();
      }

      await openUpdate();
      expect(find.text('Board update'), findsOneWidget);
      final Rect scaffold = tester.getRect(find.byType(Scaffold).first);
      final Rect firmwareContent = tester.getRect(
        find.byKey(const ValueKey<String>('firmware-safe-content')),
      );
      expect(
        firmwareContent.bottom,
        moreOrLessEquals(scaffold.bottom - bottomSystemInset),
      );

      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();

      expect(find.text('Board update'), findsNothing);
      expect(_activeNavigationIndex(tester), 2);
      expect(tester.state(find.byType(SettingsScreen)), same(origin));
      expect(find.text('Connected'), findsOneWidget);

      await openUpdate();
      tester.view.physicalSize = const Size(844, 390);
      await tester.pumpAndSettle();
      final back = find.byKey(const ValueKey<String>('firmware-back'));
      expect(back.hitTestable(), findsOneWidget);
      expect(
        find.byKey(const ValueKey('main-landscape-navigation')),
        findsNothing,
      );
      await captureHealthReview(tester, 'board-update-landscape-back');
      await tester.tap(back);
      await tester.pumpAndSettle();
      expect(find.text('Board update'), findsNothing);
      expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
      expect(tester.state(find.byType(SettingsScreen)), same(origin));
    },
  );

  testWidgets('system back cannot leave an active board update', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    tester.platformDispatcher.localesTestValue = const <Locale>[Locale('en')];
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    late ValueChanged<bool> updateActivityChanged;
    await _pumpConnectedGate(
      tester,
      service,
      firmwarePageBuilder:
          (
            LitchBleService bleService,
            String boardId,
            VoidCallback onRequestShowFirmwareTab,
            ValueChanged<bool> onUpdateActivity,
            ValueChanged<FirmwareUpdateAvailability> onAvailabilityChanged,
          ) {
            updateActivityChanged = onUpdateActivity;
            return const Center(child: Text('Firmware test content'));
          },
    );

    // Firmware can take ownership while a page-body pointer is held.
    final pages = tester.getRect(find.byKey(const ValueKey('main-pages')));
    final pointer = await tester.startGesture(pages.center);
    await pointer.moveBy(const Offset(-80, 0));
    await tester.pump();
    updateActivityChanged(true);
    await tester.pump();
    await pointer.up();
    await tester.pumpAndSettle();
    expect(find.byType(NavigationBar), findsNothing);
    expect(
      tester
          .widget<PageView>(
            find.byWidgetPredicate(
              (widget) => widget is PageView,
              skipOffstage: false,
            ),
          )
          .physics,
      isA<NeverScrollableScrollPhysics>(),
    );

    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();

    expect(find.text('Board update'), findsOneWidget);
    expect(find.text('Firmware test content'), findsOneWidget);
    tester.platformDispatcher.localesTestValue = const <Locale>[Locale('de')];
    await tester.pumpAndSettle();
    expect(find.text('Brett aktualisieren'), findsOneWidget);
    expect(find.byTooltip('Zurück'), findsOneWidget);
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(find.text('Firmware test content'), findsOneWidget);
    expect(
      tester
          .widget<IconButton>(
            find.byKey(const ValueKey<String>('firmware-back')),
          )
          .onPressed,
      isNull,
    );

    updateActivityChanged(false);
    await tester.pump();
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(find.text('Spielen'), findsOneWidget);
    expect(
      tester
          .widget<PageView>(
            find.byWidgetPredicate((widget) => widget is PageView),
          )
          .controller!
          .page,
      moreOrLessEquals(0), // Play is now the first page.
    );
    await _selectAdjacentMainPage(tester, forward: true); // Play → Litchcraft.
    await _selectAdjacentMainPage(tester, forward: true);
    expect(_activeNavigationIndex(tester), 2);
    expect(tester.takeException(), isNull);
  });

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      '${platform.name} minimize and restore retain the connected page and link',
      (WidgetTester tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final _ConnectionGateBleService service = _ConnectionGateBleService();
        addTearDown(service.dispose);
        await _pumpConnectedGate(tester, service);
        await tester.tap(_navigationAction(tester, 'setup'));
        await tester.pumpAndSettle();
        final shell = tester.element(find.byType(LitchMainScaffold));
        final token = service.connectionSessionToken;
        final scans = service.startScanCalls;
        const LitchBoard firstBoard = LitchBoard(
          id: 'board-1',
          name: 'Foxie One',
          rssi: -40,
        );

        _sendAppToBackground(tester);
        await tester.pump();

        expect(service.foregroundCalls, <bool>[false]);
        expect(service.disconnectCalls, 0);
        expect(service.isConnected, isTrue);

        _sendAppToForeground(tester);
        await tester.pump();

        expect(service.foregroundCalls, <bool>[false, true]);

        for (int index = 0; index < 6; index += 1) {
          service.emitBoards(const <LitchBoard>[firstBoard]);
          await tester.pump(const Duration(milliseconds: 250));
        }

        expect(service.connectCalls, <String>['board-1']);
        expect(service.startScanCalls, scans);
        expect(service.connectionSessionToken, token);
        expect(tester.element(find.byType(LitchMainScaffold)), same(shell));
        expect(_activeNavigationIndex(tester), 2);
        expect(find.text('Connected'), findsOneWidget);
        expect(service.isConnected, isTrue);
      },
      variant: TargetPlatformVariant({platform}),
    );
  }

  testWidgets('restore keeps a link during delayed background housekeeping', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service);

    service.delayNextBackgroundTransition();
    _sendAppToBackground(tester);
    await tester.pump();
    _sendAppToForeground(tester);
    await tester.pump();
    service.completeBackgroundTransition();
    await tester.pump();

    expect(service.foregroundCalls, <bool>[false, true]);
    expect(service.disconnectCalls, 0);
    expect(service.isConnected, isTrue);
  });

  testWidgets('landscape connected controls open above the footer anchor', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _PagedSettingsBleService();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    final craft = _RibbonCraftController(service)..forceLiveStatus = true;
    craft.simulatePhysicalMode(true);
    final library = LitchcraftLibrary(craft);
    await library.ready;
    await library.dismissWelcome();
    addTearDown(() async {
      library.dispose();
      craft.dispose();
      await service.dispose();
    });
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context).copyWith(
            padding: const EdgeInsets.fromLTRB(30, 0, 30, 21),
            viewPadding: const EdgeInsets.fromLTRB(30, 0, 30, 21),
            textScaler: const TextScaler.linear(2),
          ),
          child: RepaintBoundary(child: child!),
        ),
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const [],
          onDisconnect: () async {},
          craftController: craft,
          craftLibrary: library,
          initialPageIndex: 1,
          initialCraftModeResolved: true,
          landscapeRibbonSide: Alignment.centerLeft,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final Finder action = find.byKey(
      const ValueKey<String>('connected-board-status-action'),
    );
    await tester.tap(action);
    await tester.pumpAndSettle();

    final Rect anchor = tester.getRect(action);
    final Rect popover = tester.getRect(
      find.byKey(const ValueKey<String>('connected-board-popover')),
    );
    expect(popover.bottom, lessThanOrEqualTo(anchor.top));
    expect(popover.top, greaterThanOrEqualTo(8));
    final Finder scroll = find.byKey(
      const ValueKey<String>('board-menu-scroll'),
    );
    expect(scroll.hitTestable(), findsOneWidget);
    await captureHealthReview(tester, 'connected-menu-landscape-left-200');
    await tester.drag(scroll, const Offset(0, -240));
    await tester.pumpAndSettle();
    final brightness = find.byKey(const ValueKey('board-menu-brightness'));
    expect(brightness, findsOneWidget);
    final brightnessIcon = tester.getRect(
      find.byIcon(Icons.brightness_6_outlined),
    );
    final rotateIcon = tester.getRect(find.byIcon(Icons.rotate_right_outlined));
    final slider = find.descendant(
      of: brightness,
      matching: find.byType(Slider),
    );
    final sleep = find.byKey(const ValueKey('board-menu-sleep'));
    final disconnect = find.byKey(const ValueKey('board-menu-disconnect'));
    expect(tester.getRect(slider).left, brightnessIcon.left);
    expect(brightnessIcon.left, rotateIcon.left);
    expect(tester.getRect(slider).right, tester.getRect(sleep).right);
    expect(tester.getRect(sleep).top, tester.getRect(disconnect).top);
    await captureHealthReview(
      tester,
      'connected-menu-landscape-left-200-scrolled',
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'rotation dismisses board controls and retains reachable shell screens',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      _usePortraitPhone(tester);
      addTearDown(tester.view.reset);
      SharedPreferences.setMockInitialValues({'enable_app_console': true});
      final service = _PagedSettingsBleService();
      addTearDown(service.dispose);
      var side = DeviceLandscapeSide.right;

      void portrait() {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.viewPadding = const FakeViewPadding(top: 28, bottom: 24);
        tester.view.padding = tester.view.viewPadding;
      }

      void landscape(DeviceLandscapeSide next) {
        side = next;
        tester.view.physicalSize = const Size(844, 390);
        tester.view.viewPadding = next == DeviceLandscapeSide.left
            ? const FakeViewPadding(left: 48, right: 18, bottom: 21)
            : const FakeViewPadding(left: 18, right: 48, bottom: 21);
        tester.view.padding = tester.view.viewPadding;
      }

      portrait();
      await _pumpConnectedGate(
        tester,
        service,
        orientationSideReader: () async => side,
      );
      final platform = Theme.of(
        tester.element(find.byType(ConnectionGateScreen)),
      ).platform.name;
      final menu = find.byKey(const ValueKey('connected-board-popover'));
      final status = find.byKey(
        const ValueKey<String>('connected-board-status-action'),
      );

      Future<void> rotate(String page, Finder surface) async {
        for (final next in DeviceLandscapeSide.values) {
          landscape(next);
          await tester.pump();
          if (page == 'play-connected') {
            expect(menu, findsNothing);
            // A side change at the same size also retires its modal barrier.
            await tester.pump();
          }
          expect(
            surface.hitTestable(),
            findsOneWidget,
            reason: '$page first frame ${next.name}',
          );
          expect(tester.takeException(), isNull);
          await tester.pump(const Duration(milliseconds: 130));
          await tester.pump(const Duration(milliseconds: 230));
          expect(
            surface.hitTestable(),
            findsOneWidget,
            reason: '$page settled ${next.name}',
          );
          if (page.startsWith('setup')) {
            final rail = page == 'setup-offline'
                ? find.byKey(
                    const ValueKey('disconnected-setup-landscape-action-rail'),
                  )
                : find.byKey(const ValueKey('main-landscape-navigation'));
            final pane = tester.getRect(surface);
            final nav = tester.getRect(rail);
            if (page == 'setup-offline') {
              expect(pane.overlaps(nav), isFalse, reason: '$page ${next.name}');
            } else {
              expect(
                pane.bottom,
                greaterThan(nav.top),
                reason: 'Connected Setup scrolls behind its floating navigation.',
              );
              expect(nav.bottom, lessThanOrEqualTo(pane.bottom));
            }
          }
          if (page == 'play-connected') {
            await tester.tap(status);
            await tester.pumpAndSettle();
            expect(menu, findsOneWidget);
            await captureHealthReview(
              tester,
              'rotation-$platform-connected-menu-${next.name}',
            );
          }
          if (page.startsWith('setup') || page.startsWith('craft')) {
            await captureHealthReview(
              tester,
              'rotation-$platform-$page-${next.name}',
            );
          }
        }
        portrait();
        await tester.pump();
        expect(
          surface.hitTestable(),
          findsOneWidget,
          reason: '$page portrait return',
        );
        expect(tester.takeException(), isNull);
        await tester.pump(const Duration(milliseconds: 230));
      }

      Future<void> go(String page) async {
        await tester.tap(_navigationAction(tester, page));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
      }

      await tester.tap(status);
      await tester.pumpAndSettle();
      expect(menu, findsOneWidget);
      await rotate('play-connected', find.byType(HomeScreen));
      expect(menu, findsNothing);

      await tester.tap(status);
      await tester.pumpAndSettle();
      expect(menu, findsOneWidget);
      landscape(DeviceLandscapeSide.right);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitBoards(const []);
      await tester.pump();
      expect(menu, findsNothing);
      expect(
        find.byKey(const PageStorageKey<String>('disconnected-play-scroll')),
        findsOneWidget,
      );
      portrait();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pumpAndSettle();

      await go('litchcraft');
      final canvas = find.byType(LitchcraftCanvasView);
      final canvasState = tester.state(canvas);
      await rotate('craft-connected', canvas);
      expect(tester.state(canvas), same(canvasState));

      await go('setup');
      final setup = find.byKey(const ValueKey('settings-scroll'));
      final settingsState = tester.state(find.byType(SettingsScreen));
      await rotate('setup-connected', setup);
      expect(tester.state(find.byType(SettingsScreen)), same(settingsState));

      await go('console');
      final console = find.byKey(const ValueKey('console-terminal-surface'));
      final consoleState = tester.state(find.byType(AppConsoleScreen));
      final command = find.byKey(const ValueKey('console-command-input'));
      await tester.enterText(command, 'rotation draft');
      final commandFocus = tester.widget<TextField>(command).focusNode!;
      expect(commandFocus.hasFocus, isTrue);
      await rotate('console-connected', console);
      expect(tester.widget<TextField>(command).focusNode, same(commandFocus));
      expect(commandFocus.hasFocus, isTrue);
      expect(tester.state(find.byType(AppConsoleScreen)), same(consoleState));
      expect(
        tester.widget<TextField>(command).controller!.text,
        'rotation draft',
      );

      await tester.tap(status);
      await tester.pumpAndSettle();
      await tester.tap(
        find.descendant(of: menu, matching: find.text('Check for updates')),
      );
      await tester.pumpAndSettle();
      final update = find.byType(FirmwareScreen);
      await rotate('update-connected', update);

      // A terminal loss during rotation retires Connected while the explicit
      // update destination and its Back path stay available.
      landscape(DeviceLandscapeSide.left);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitBoards(const []);
      await tester.pump();
      expect(update.hitTestable(), findsOneWidget);
      expect(menu, findsNothing);
      portrait();
      await tester.pump(const Duration(milliseconds: 230));
      await rotate('update-offline', update);
      await tester.tap(find.byKey(const ValueKey('firmware-back')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 230));
      await rotate('console-offline', console);
      expect(tester.state(find.byType(AppConsoleScreen)), same(consoleState));
      expect(
        tester.widget<TextField>(command).controller!.text,
        'rotation draft',
      );

      await go('setup');
      await rotate('setup-reconnecting', setup);
      expect(tester.state(find.byType(SettingsScreen)), same(settingsState));

      await go('litchcraft');
      await rotate('craft-offline', canvas);
      await go('play');
      await rotate(
        'play-offline',
        find.byKey(const PageStorageKey<String>('disconnected-play-scroll')),
      );

      // Cold/offline Setup has phone controls and no board settings.
      service.emitAvailability(BleAvailability.poweredOff);
      await tester.pump();
      await go('setup');
      await rotate(
        'setup-offline',
        find.byKey(const ValueKey('offline-setup-scroll')),
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets('reconnect pins the chosen board among nearby boards', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    await _pumpConnectedGate(tester, service, layoutSize: const Size(844, 390));
    await tester.tap(
      find.byKey(const ValueKey<String>('landscape-craft-action')),
    );
    await tester.pumpAndSettle();
    final State retainedCraft = tester.state(find.byType(LitchcraftScreen));
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    expect(
      find.byType(LitchMainScaffold),
      findsOneWidget,
      reason: 'A reconnect must not replace and resize the established shell.',
    );
    expect(
      tester.state(find.byType(LitchcraftScreen)),
      same(retainedCraft),
      reason: 'The live Litchcraft workspace must survive reconnect in place.',
    );
    const other = LitchBoard(id: 'board-2', name: 'Litch', rssi: -20);
    const chosen = LitchBoard(id: 'board-1', name: 'Litch', rssi: -60);
    service.emitBoards(const [other]);
    await tester.pump(const Duration(seconds: 3));
    expect(service.connectCalls, ['board-1']);
    service.emitBoards(const [other, chosen]);
    await tester.pump(Duration.zero);
    expect(service.connectCalls, ['board-1', 'board-1']);
    service.completeNextConnect(true);
    await tester.pump();
    expect(
      tester.widget<LitchMainScaffold>(find.byType(LitchMainScaffold)).boardId,
      chosen.id,
    );
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'connected scaffold uses transport identity over saved selection',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{
        'last_connected_board_id': 'board-1',
      });
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      service.emitConnection('board-2', DeviceConnectionState.connected);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      expect(
        tester
            .widget<LitchMainScaffold>(find.byType(LitchMainScaffold))
            .boardId,
        'board-2',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'board replacement rebuilds retained pages for the actual board',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      await _pumpConnectedGate(tester, service);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.emitConnection('board-2', DeviceConnectionState.connected);
      await tester.pump();
      expect(
        tester.widget<HomeScreen>(find.byType(HomeScreen)).boardId,
        'board-2',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'manual connect owns the gate and normal reconnect stays single-attempt',
    (WidgetTester tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final _ConnectionGateBleService service = _ConnectionGateBleService();
      addTearDown(service.dispose);
      const LitchBoard firstBoard = LitchBoard(
        id: 'board-1',
        name: 'Foxie One',
        rssi: -40,
      );
      const LitchBoard secondBoard = LitchBoard(
        id: 'board-2',
        name: 'Foxie Two',
        rssi: -45,
      );

      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();

      service.emitBoards(const <LitchBoard>[firstBoard, secondBoard]);
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
      await tester.tap(find.widgetWithText(FilledButton, 'Connect').first);
      await tester.pump();

      expect(service.connectCalls, <String>['board-1']);
      expect(service.stopScanCalls, 0);
      expect(service.identifyStopCalls, 0);
      expect(
        find.byKey(const ValueKey('chosen-board-connection')),
        findsOneWidget,
      );
      final connectButtons = tester.widgetList<FilledButton>(
        find.widgetWithText(FilledButton, 'Connect'),
      );
      expect(connectButtons, isNotEmpty);
      expect(
        connectButtons.every((button) => button.onPressed == null),
        isTrue,
      );
      expect(
        find.byKey(const ValueKey('chosen-board-cancel')).hitTestable(),
        findsOneWidget,
      );

      service.emitBoards(const <LitchBoard>[firstBoard, secondBoard]);
      await tester.pump(const Duration(milliseconds: 900));
      expect(service.connectCalls, <String>['board-1']);

      service.completeNextConnect(true);
      await tester.pumpAndSettle();
      expect(find.text('Connected'), findsOneWidget);
      service.emitConnection(firstBoard.id, DeviceConnectionState.disconnected);
      await tester.pump();

      service.emitBoards(const <LitchBoard>[firstBoard]);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 900));

      expect(service.connectCalls, <String>['board-1', 'board-1']);
      expect(service.stopScanCalls, 0);
      expect(service.identifyStopCalls, 0);

      service.completeNextConnect(true);
      await tester.pump();

      await tester.pumpWidget(const SizedBox.shrink());
      expect(service.disposed, isFalse);
      await service.dispose();
    },
  );

  testWidgets(
    'recovery entry discovers capabilities before waiting for the Craft library',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final preferences = await SharedPreferences.getInstance();
      final service = _RecoveryNavigationBle()
        ..capabilityGate = Completer<LitchDeviceCapabilities>();
      final craft = _NavigationCraftController(service);
      final localStore = _DelayedArtworkStore(preferences);
      final library = LitchcraftLibrary(craft, store: localStore);
      final download = _OfflineFirmwareDownloadService();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        if (!localStore.loadGate.isCompleted) {
          localStore.loadGate.complete(LitchcraftLibrarySnapshot());
        }
        await tester.pumpAndSettle();
        library.dispose();
        craft.dispose();
        await service.dispose();
        download.dispose();
      });
      await _pumpConnectedOtaGate(
        tester,
        service,
        download,
        const LitchBoard(id: 'board-1', name: 'Litch Recovery', rssi: -40),
        craftController: craft,
        craftLibrary: library,
        settle: false,
      );
      expect(service.capabilityRequests, greaterThan(0));
      expect(craft.adoptCalls, 0);
      debugPrint(
        '[RECOVERY_ENTRY] connected; capability discovery owns admission',
      );

      service.capabilityGate!.complete(_RecoveryNavigationBle.recovery);
      await tester.pumpAndSettle();
      final disconnect = find.byKey(const ValueKey('recovery-disconnect'));
      try {
        expect(localStore.loadGate.isCompleted, isFalse);
        expect(find.text('Setup'), findsOneWidget);
        expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
        expect(find.text('Recovery'), findsOneWidget);
        tester.view.physicalSize = const Size(844, 390);
        await tester.pumpAndSettle();
        expect(find.text('Setup'), findsOneWidget);
        expect(find.byType(AppBar), findsOneWidget);
        expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
        tester.view.physicalSize = const Size(390, 844);
        await tester.pumpAndSettle();
        for (final type in [HomeScreen, LitchcraftScreen, AppConsoleScreen]) {
          expect(find.byType(type, skipOffstage: false), findsNothing);
        }
        expect(find.byType(NavigationBar), findsNothing);
        expect(find.byType(NavigationRail), findsNothing);
        expect(craft.adoptCalls, 0);
        expect(craft.enterCalls, 0);
        debugPrint(
          '[RECOVERY_ENTRY] Setup health visible; local library still pending; no domain pages',
        );
        expect(disconnect.hitTestable(), findsOneWidget);
        expect(tester.widget<IconButton>(disconnect).tooltip, 'Disconnect');
        expect(
          tester.getSize(disconnect).shortestSide,
          greaterThanOrEqualTo(48),
        );
      } finally {
        localStore.loadGate.complete(LitchcraftLibrarySnapshot());
        await tester.pumpAndSettle();
        await library.ready;
      }
      expect(craft.adoptCalls, 0);
      debugPrint(
        '[RECOVERY_ENTRY] library ready; no Craft probe; disconnect available',
      );
      await tester.tap(disconnect);
      debugPrint('[RECOVERY_ENTRY] disconnect tapped');
      await tester.pump();
      debugPrint('[RECOVERY_ENTRY] disconnected frame pumped');
      expect(service.disconnectCalls, 1);
      expect(find.byType(LitchMainScaffold), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      debugPrint('[RECOVERY_ENTRY] shell unmounted');
    },
  );

  testWidgets(
    'recovery Setup opens software and retains the updater and unresolved target across reconnect and exit',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _RecoveryNavigationBle();
      service.debugSetDeviceCapabilities(
        'board-1',
        _RecoveryNavigationBle.recovery,
      );
      final craft = _NavigationCraftController(service)
        ..retained = true
        ..connectedBoardActive = true;
      final library = LitchcraftLibrary(craft);
      await library.ready;
      final download = _OfflineFirmwareDownloadService();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        craft.dispose();
        await service.dispose();
        download.dispose();
      });
      await _pumpConnectedOtaGate(
        tester,
        service,
        download,
        const LitchBoard(id: 'board-1', name: 'Litch Recovery', rssi: -40),
        craftController: craft,
        craftLibrary: library,
      );
      expect(find.text('Setup'), findsOneWidget);
      await tester.ensureVisible(
        find.byKey(const ValueKey('open-board-software')),
      );
      await tester.tap(find.byKey(const ValueKey('open-board-software')));
      await tester.pumpAndSettle();
      expect(find.text('Board update'), findsOneWidget);
      final updater = tester.state(find.byType(FirmwareScreen));
      await service.rememberPendingOtaVerification(_otaTarget);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      await tester.pump();
      expect(tester.state(find.byType(FirmwareScreen)), same(updater));
      expect(service.pendingOtaVerification, same(_otaTarget));
      service.debugSetDeviceCapabilities(
        'board-1',
        LitchDeviceCapabilities.unavailable,
      );
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pumpAndSettle();
      expect(tester.state(find.byType(FirmwareScreen)), same(updater));
      expect(find.text('Board update'), findsOneWidget);
      expect(
        find.widgetWithText(OutlinedButton, 'Verify installed update'),
        findsOneWidget,
      );
      expect(service.pendingOtaVerification, same(_otaTarget));
      expect(craft.adoptCalls, 0);
      expect(craft.enterCalls, 0);
      expect(craft.leaveCalls, 0);
      await tester.tap(find.byKey(const ValueKey('firmware-back')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('recovery-disconnect')));
      await tester.pump();
      expect(service.pendingOtaVerification, same(_otaTarget));
      expect(service.disconnectCalls, 1);
      expect(find.byType(LitchMainScaffold), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'recovery entry ignores retired capability replies and restores normal navigation only for the current connection',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final oldCapabilities = Completer<LitchDeviceCapabilities>();
      final service = _RecoveryNavigationBle()
        ..reportConnectedAsReady = true
        ..capabilityGate = oldCapabilities;
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      final download = _OfflineFirmwareDownloadService();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        if (!oldCapabilities.isCompleted) {
          oldCapabilities.complete(_RecoveryNavigationBle.normal);
        }
        library.dispose();
        craft.dispose();
        await service.dispose();
        download.dispose();
      });
      await _pumpConnectedOtaGate(
        tester,
        service,
        download,
        const LitchBoard(id: 'board-1', name: 'Litch Recovery', rssi: -40),
        craftController: craft,
        craftLibrary: library,
        settle: false,
      );
      expect(
        craft.adoptCalls,
        0,
        reason: 'Craft must await capability discovery.',
      );
      final oldConnectionToken = service.connectionSessionToken;
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.capabilityGate = null;
      service.capabilities = _RecoveryNavigationBle.digestOnly;
      service.emitConnection('board-1', DeviceConnectionState.connected);
      expect(service.connectionSessionToken, greaterThan(oldConnectionToken));
      await tester.pumpAndSettle();
      expect(find.text('Setup'), findsOneWidget);
      oldCapabilities.complete(_RecoveryNavigationBle.normal);
      await tester.pumpAndSettle();
      expect(find.text('Setup'), findsOneWidget);
      await tester.ensureVisible(
        find.byKey(const ValueKey('open-board-software')),
      );
      await tester.tap(find.byKey(const ValueKey('open-board-software')));
      await tester.pumpAndSettle();
      expect(find.text('Board update'), findsOneWidget);
      expect(
        find.text('V3.3 board is updated manually over USB.'),
        findsWidgets,
      );
      expect(find.textContaining('supports app settings'), findsNothing);
      expect(find.widgetWithText(FilledButton, 'Update board'), findsNothing);
      expect(craft.adoptCalls, 0);
      service.emitConnection('board-1', DeviceConnectionState.disconnected);
      service.capabilities = _RecoveryNavigationBle.normal;
      service.emitConnection('board-1', DeviceConnectionState.connected);
      await tester.pumpAndSettle();
      expect(craft.adoptCalls, 1);
      expect(find.byKey(const ValueKey('recovery-disconnect')), findsNothing);
      await tester.tap(find.byKey(const ValueKey('firmware-back')));
      await tester.pumpAndSettle();
      expect(find.byType(NavigationBar), findsOneWidget);
      expect(_activeNavigationIndex(tester), 2);
      expect(craft.enterCalls, 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'digest-only recovery entry offers USB without normal feature claims',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _RecoveryNavigationBle()
        ..capabilities = LitchDeviceCapabilities.fromPayload({
          'i': [3, 4, 8, 1],
        })!;
      final craft = _NavigationCraftController(service);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      final download = _OfflineFirmwareDownloadService();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        craft.dispose();
        await service.dispose();
        download.dispose();
      });
      await _pumpConnectedOtaGate(
        tester,
        service,
        download,
        const LitchBoard(id: 'board-1', name: 'Litch Recovery', rssi: -40),
        craftController: craft,
        craftLibrary: library,
      );
      expect(find.text('Setup'), findsOneWidget);
      expect(find.text('Recovery'), findsOneWidget);
      await tester.ensureVisible(
        find.byKey(const ValueKey('open-board-software')),
      );
      await tester.tap(find.byKey(const ValueKey('open-board-software')));
      await tester.pumpAndSettle();
      expect(find.text('Board update'), findsOneWidget);
      expect(
        find.text('V3.3 board is updated manually over USB.'),
        findsWidgets,
      );
      expect(find.textContaining('runs normal app features'), findsNothing);
      expect(find.textContaining('supports app settings'), findsNothing);
      expect(find.widgetWithText(FilledButton, 'Update board'), findsNothing);
      expect(craft.adoptCalls, 0);
      await tester.tap(find.byKey(const ValueKey('firmware-back')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('recovery-disconnect')));
      await tester.pump();
      expect(service.disconnectCalls, 1);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'pending OTA stays visible while startup capabilities wait and fail',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final capabilityGate = Completer<LitchDeviceCapabilities>();
      final service = _RecoveryNavigationBle()
        ..capabilities = _RecoveryNavigationBle.normal
        ..capabilityGate = capabilityGate;
      final download = _OfflineFirmwareDownloadService();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        await service.dispose();
        download.dispose();
      });
      const board = LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40);
      await service.rememberPendingOtaVerification(_otaTarget);
      service.emitConnection(board.id, DeviceConnectionState.connected);

      await _pumpConnectedOtaGate(
        tester,
        service,
        download,
        board,
        settle: false,
      );
      expect(service.capabilityRequests, greaterThan(0));
      expect(find.byType(FirmwareScreen), findsOneWidget);
      expect(find.text('Board update'), findsOneWidget);

      capabilityGate.completeError(StateError('Capability discovery failed'));
      await tester.pumpAndSettle();
      expect(find.byType(FirmwareScreen), findsOneWidget);
      expect(find.text('Board update'), findsOneWidget);
      expect(service.pendingOtaVerification, same(_otaTarget));
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('OTA verification resumes visibly and clears an exact match', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService()
      ..otaVerification = const OtaPostRebootVerified();
    addTearDown(service.dispose);
    final _OfflineFirmwareDownloadService downloadService =
        _OfflineFirmwareDownloadService();
    addTearDown(downloadService.dispose);
    const LitchBoard board = LitchBoard(
      id: 'board-1',
      name: 'Foxie One',
      rssi: -40,
    );

    await _pumpConnectedOtaGate(tester, service, downloadService, board);
    await service.rememberPendingOtaVerification(_otaTarget);
    await _recreateGateAfterReboot(tester, service, board);
    await tester.pumpAndSettle();

    expect(service.pendingOtaVerification, isNull);
    expect(find.text('Board update'), findsOneWidget);
    expect(
      find.text('Selected firmware verified on this board.'),
      findsOneWidget,
    );
    expect(service.connectCalls, const <String>['board-1']);
  });

  testWidgets('OTA mismatch remains visible and retryable after recreation', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService()
      ..otaVerification = const OtaPostRebootMismatch(
        'The reconnected board reports 1.2.2, not 1.2.3.',
      );
    addTearDown(service.dispose);
    final _OfflineFirmwareDownloadService downloadService =
        _OfflineFirmwareDownloadService();
    addTearDown(downloadService.dispose);
    const LitchBoard board = LitchBoard(
      id: 'board-1',
      name: 'Foxie One',
      rssi: -40,
    );

    await _pumpConnectedOtaGate(tester, service, downloadService, board);
    await service.rememberPendingOtaVerification(_otaTarget);
    await _recreateGateAfterReboot(tester, service, board);
    await tester.pumpAndSettle();

    expect(find.text('Board update'), findsOneWidget);
    expect(find.textContaining('reports 1.2.2, not 1.2.3'), findsOneWidget);
    expect(
      find.widgetWithText(OutlinedButton, 'Verify installed update'),
      findsOneWidget,
    );
    expect(service.pendingOtaVerification, same(_otaTarget));
    expect(service.connectCalls, const <String>['board-1']);
  });

  testWidgets('pending OTA verification never replaces another live board', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final _OfflineFirmwareDownloadService downloadService =
        _OfflineFirmwareDownloadService();
    addTearDown(downloadService.dispose);
    const LitchBoard board = LitchBoard(
      id: 'board-2',
      name: 'Foxie Two',
      rssi: -45,
    );

    await _pumpConnectedOtaGate(tester, service, downloadService, board);
    await service.rememberPendingOtaVerification(_otaTarget);
    await _recreateGateAfterReboot(tester, service, board);
    await tester.pumpAndSettle();

    expect(find.text('Board update'), findsOneWidget);
    expect(
      find.textContaining('A different board is connected'),
      findsOneWidget,
    );
    expect(service.connectedId, 'board-2');
    expect(service.connectCalls, const <String>['board-2']);
    expect(service.pendingOtaVerification, same(_otaTarget));
  });

  testWidgets(
    'cold discovery keeps Games geometry from first paint through the first advertisement',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      final originalPreferences = SharedPreferencesStorePlatform.instance;
      addTearDown(() {
        SharedPreferencesStorePlatform.instance = originalPreferences;
        SharedPreferences.resetStatic();
        tester.view.reset();
      });
      for (final config in [
        (size: const Size(384, 832), scale: 1.15, saved: true),
        (size: const Size(390, 844), scale: 1.0, saved: true),
        (size: const Size(390, 844), scale: 2.0, saved: true),
        (size: const Size(320, 832), scale: 1.0, saved: true),
        (size: const Size(320, 832), scale: 2.0, saved: true),
        (size: const Size(384, 832), scale: 1.15, saved: false),
      ]) {
        tester.view.physicalSize = config.size;
        tester.view.devicePixelRatio = 1;
        SharedPreferences.resetStatic();
        final preferences = _DelayedStartupPreferences({
          if (config.saved) 'flutter.last_connected_board_id': 'board-1',
          'flutter.board_friendly_name:board-1': 'Comet 5',
        });
        SharedPreferencesStorePlatform.instance = preferences;
        final service = _CraftNavigationBle()
          ..inspectionGate = Completer<void>();
        final repository = _EmptyGameLibraryRepository(
          snapshot: GameLibrarySnapshot(
            games: [_recentArchive(1), _recentArchive(2), _recentArchive(3)],
          ),
        );
        try {
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchDarkTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey('connection-stage-review-root'),
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(config.scale)),
                  child: child!,
                ),
              ),
              home: ConnectionGateScreen(
                bleService: service,
                gameLibrary: repository,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          final section = find.byKey(const ValueKey('recent-games-section'));
          final firstTop = tester.getRect(section).top;
          final state = tester.state(find.byType(RecentGamesSection));
          final offsets = <String, double>{};
          double? firstRowTop;
          void record(String phase) {
            offsets[phase] = tester.getRect(section).top - firstTop;
            final row = find.byKey(const ValueKey('recent-game-recent-1'));
            if (row.evaluate().isNotEmpty) {
              firstRowTop ??= tester.getRect(row).top;
              offsets['$phase/row'] = tester.getRect(row).top - firstRowTop!;
            }
            expect(tester.state(find.byType(RecentGamesSection)), same(state));
          }

          Future<void> capture(String phase) async {
            final samsung = config.size.width == 384 && config.saved;
            final narrowLarge = config.size.width == 320 && config.scale == 2;
            if (!samsung && !(narrowLarge && phase == 'proof')) return;
            if (phase == 'scan' &&
                Platform.environment['LITCH_HEALTH_CAPTURE_DIR'] != null) {
              await tester.runAsync(() async {
                final context = tester.element(section);
                await Future.wait([
                  for (final image in tester.widgetList<Image>(
                    find.byType(Image),
                  ))
                    precacheImage(image.image, context),
                ]);
              });
              await tester.pump(const Duration(milliseconds: 16));
              record('assets ready');
            }
            await captureHealthReview(
              tester,
              'stage-${Theme.of(tester.element(section)).platform.name}-'
              '${config.size.width.toInt()}-${config.scale}-$phase',
              boundaryFinder: find.byKey(
                const ValueKey('connection-stage-review-root'),
              ),
            );
          }

          record('first paint');
          for (var frame = 0; frame < 3; ++frame) {
            await tester.pump(const Duration(milliseconds: 16));
            record('preferences held/$frame');
          }
          preferences.ready.complete();
          await tester.pump(const Duration(milliseconds: 16));
          record('preferences resolved');
          service.emitAvailability(BleAvailability.ready);
          await tester.pump(const Duration(milliseconds: 16));
          record('radio ready');
          await capture('scan');
          tester.binding.scheduleFrameCallback(
            (_) => service.emitBoards(const [
              LitchBoard(id: 'board-1', name: '', rssi: -40),
            ]),
          );
          await tester.pump(const Duration(milliseconds: 16));
          record('first ad before queued Connect');
          expect(service.connectCalls, isEmpty);
          await capture('first-ad');
          final card = find.byKey(const ValueKey('discovered-board-board-1'));
          final rowName = tester
              .widgetList<Text>(
                find.descendant(of: card, matching: find.byType(Text)),
              )
              .first
              .data;
          if (!config.saved) await tester.tap(find.text('Connect'));
          await tester.pump(const Duration(milliseconds: 16));
          record('connecting');
          expect(service.connectCalls, ['board-1']);
          service.completeNextConnect(true);
          for (var frame = 0; frame < 4; ++frame) {
            await tester.pump(const Duration(milliseconds: 16));
            record('mode proof/$frame');
            expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          }
          await capture('proof');
          final changed = offsets.entries.where((e) => e.value.abs() > .01);
          debugPrint(
            'STARTUP_GEOMETRY ${config.size.width}/${config.scale}/'
            '${config.saved ? 'saved' : 'new'} top=$firstTop '
            'max_delta=${offsets.values.map((v) => v.abs()).reduce(math.max)} '
            'ad=${offsets['first ad before queued Connect']} proof=${offsets['mode proof/3']}',
          );
          expect(
            changed,
            isEmpty,
            reason:
                'First ad must not push the already visible Games heading or rows.',
          );
          expect(rowName, 'Comet 5');
          expect(
            tester
                .widget<Text>(find.byKey(const ValueKey('chosen-board-phase')))
                .data,
            'Connecting to Comet 5...',
          );
          expect((repository.catalogCalls, repository.windowCalls), (1, 1));
          service.inspectionGate!.complete();
          for (var frame = 0; frame < 5; ++frame) {
            await tester.pump(const Duration(milliseconds: 16));
          }
          expect(find.byType(HomeScreen), findsOneWidget);
        } finally {
          await tester.pumpWidget(const SizedBox());
          if (!preferences.ready.isCompleted) preferences.ready.complete();
          await tester.runAsync(() async {
            await service.dispose();
            await repository.close();
          });
        }
      }
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'absent remembered board preserves manual choices and selected geometry',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      _usePortraitPhone(tester);
      final semantics = tester.ensureSemantics();
      try {
        for (final scale in [1.15, 2.0]) {
          SharedPreferences.setMockInitialValues({
            'last_connected_board_id': 'missing-board',
            'board_friendly_name:board-2': 'Travel Board',
          });
          final service = _ConnectionGateBleService();
          final repository = _EmptyGameLibraryRepository(
            snapshot: GameLibrarySnapshot(
              games: [_recentArchive(1), _recentArchive(2), _recentArchive(3)],
            ),
          );
          try {
            await tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme(),
                localizationsDelegates: AppLocalizations.localizationsDelegates,
                supportedLocales: AppLocalizations.supportedLocales,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(scale)),
                  child: child!,
                ),
                home: ConnectionGateScreen(
                  bleService: service,
                  gameLibrary: repository,
                ),
              ),
            );
            await tester.pump();
            service.emitAvailability(BleAvailability.ready);
            const roster = [
              LitchBoard(id: 'board-3', name: 'Foxie Three', rssi: -30),
              LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -40),
            ];
            service.emitBoards(roster);
            await tester.pump();
            expect(service.connectCalls, isEmpty);
            Finder card(String id) =>
                find.byKey(ValueKey('discovered-board-$id'));
            Finder connect(String id) => find.descendant(
              of: card(id),
              matching: find.widgetWithText(FilledButton, 'Connect'),
            );
            expect(card('board-3'), findsOneWidget);
            expect(card('board-2'), findsOneWidget);
            expect(find.bySemanticsLabel(RegExp(r'^W{20}$')), findsNothing);
            await Scrollable.ensureVisible(
              tester.element(connect('board-2')),
              alignment: .3,
            );
            await tester.tap(connect('board-2'));
            await tester.pump(const Duration(milliseconds: 16));
            await tester.pump(const Duration(milliseconds: 16));
            final section = find.byKey(const ValueKey('recent-games-section'));
            final chosenTop = tester.getRect(section).top;
            final stage = find.byKey(
              const ValueKey('discovery-connection-stage'),
            );
            final chosenStage = tester.getSize(stage);
            final chosenCard = tester.getRect(card('board-2'));
            expect(service.connectCalls, ['board-2']);
            expect(find.text('Connecting to Foxie Two...'), findsOneWidget);
            service.emitBoards(const []);
            await tester.pump(const Duration(milliseconds: 16));
            expect(
              tester.getRect(section).top,
              chosenTop,
              reason: 'Scan cleanup must not resize the selected presentation.',
            );
            expect(tester.getSize(stage), chosenStage);
            expect(tester.getRect(card('board-2')), chosenCard);
            expect(find.text('#2'), findsOneWidget);
            await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            await tester.pump(const Duration(milliseconds: 16));
            service.completeNextConnect(false);
            await tester.pump(const Duration(milliseconds: 16));
            expect(tester.getRect(section).top, chosenTop);
            expect(
              find.byKey(const ValueKey('chosen-board-connection')),
              findsNothing,
            );
            expect(
              card('board-2'),
              findsNothing,
              reason:
                  'A sizing guide must not claim an absent board is discovered.',
            );
            service.emitBoards(roster);
            await tester.pump();
            expect(service.connectCalls, ['board-2']);
            final scroll = find
                .descendant(
                  of: find.byKey(
                    const PageStorageKey('disconnected-play-scroll'),
                  ),
                  matching: find.byType(Scrollable),
                )
                .first;
            await tester.drag(scroll, const Offset(0, 500));
            await tester.pump();
            for (final id in ['board-3', 'board-2']) {
              await tester.scrollUntilVisible(
                connect(id),
                200,
                scrollable: scroll,
              );
              await Scrollable.ensureVisible(
                tester.element(connect(id)),
                alignment: .3,
              );
              await tester.pump(const Duration(milliseconds: 16));
              expect(connect(id).hitTestable(), findsOneWidget);
              expect(
                tester.widget<FilledButton>(connect(id)).onPressed,
                isNotNull,
              );
            }
            final gamesAction = find.byKey(
              const ValueKey('game-library-action'),
            );
            await Scrollable.ensureVisible(
              tester.element(gamesAction),
              alignment: .3,
            );
            await tester.pump();
            expect(gamesAction.hitTestable(), findsOneWidget);
            await tester.tap(gamesAction);
            await tester.pumpAndSettle();
            expect(find.byType(GameLibraryScreen), findsOneWidget);
          } finally {
            await tester.pumpWidget(const SizedBox());
            await tester.runAsync(() async {
              await service.dispose();
              await repository.close();
            });
          }
        }
      } finally {
        semantics.dispose();
      }
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'Recent games retains discovery through Connect and Cancel then hands off to Play',
    (tester) async {
      _usePortraitPhone(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _CraftNavigationBle();
      final repository = _EmptyGameLibraryRepository(
        snapshot: GameLibrarySnapshot(games: [_recentArchive(1)]),
      );
      addTearDown(service.dispose);
      addTearDown(repository.close);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: ConnectionGateScreen(
            bleService: service,
            gameLibrary: repository,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      final recent = find.byType(RecentGamesSection);
      final state = tester.state(recent);
      final game = find.byKey(const ValueKey('recent-game-recent-1'));
      expect(game, findsOneWidget);
      expect((repository.catalogCalls, repository.windowCalls), (1, 1));
      service.emitAvailability(BleAvailability.ready);
      await tester.pump();
      expect(
        tester.state(recent),
        same(state),
        reason: 'Bluetooth readiness must not remount the discovery subtree.',
      );
      service.emitBoards(const [
        LitchBoard(id: 'board-1', name: 'Studio Board', rssi: -40),
      ]);
      await tester.pump();
      expect(
        tester.state(recent),
        same(state),
        reason: 'First discovery must retain the loaded Recent games owner.',
      );
      expect(game, findsOneWidget);
      expect(find.text('Loading saved games'), findsNothing);
      await tester.tap(find.text('Connect'));
      await tester.pump();
      expect(tester.state(recent), same(state));
      expect(game, findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
      await tester.pump();
      service.completeNextConnect(false);
      await tester.pump();
      expect(tester.state(recent), same(state));
      expect(game, findsOneWidget);
      expect((repository.catalogCalls, repository.windowCalls), (1, 1));
      expect(
        find.byKey(const ValueKey('chosen-board-connection')),
        findsNothing,
      );
      await tester.tap(find.byKey(const ValueKey('game-library-action')));
      await tester.pumpAndSettle();
      expect(find.byType(GameLibraryScreen), findsOneWidget);
      await tester.pageBack();
      await tester.pumpAndSettle();

      // A new successful attempt keeps this underlay until actual Play proof.
      final readsBeforeConnect = (
        repository.catalogCalls,
        repository.windowCalls,
      );
      service.inspectionGate = Completer<void>();
      await tester.tap(find.text('Connect'));
      await tester.pump();
      service.completeNextConnect(true);
      for (var frame = 0; frame < 3; ++frame) {
        await tester.pump(const Duration(milliseconds: 16));
        expect(tester.state(recent), same(state));
        expect(game, findsOneWidget);
        expect(find.text('Loading saved games'), findsNothing);
        expect((
          repository.catalogCalls,
          repository.windowCalls,
        ), readsBeforeConnect);
        expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      }
      service.inspectionGate!.complete();
      for (var frame = 0; frame < 5; ++frame) {
        await tester.pump(const Duration(milliseconds: 16));
        expect(find.text('Loading saved games').hitTestable(), findsNothing);
      }
      expect(find.byType(HomeScreen), findsOneWidget);
      await tester.scrollUntilVisible(
        game,
        300,
        scrollable: find.byWidgetPredicate(
          (widget) =>
              widget is Scrollable &&
              widget.physics is AlwaysScrollableScrollPhysics,
        ),
      );
      expect(game, findsOneWidget);
      debugPrint(
        'RECENT_HANDOFF catalog=${repository.catalogCalls} '
        'window=${repository.windowCalls}; no visible loading frame',
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('saved games are available while disconnected', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final GameLibraryRepository repository = _EmptyGameLibraryRepository();
    addTearDown(repository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();

    await tester.tap(find.byKey(const ValueKey<String>('game-library-action')));
    await tester.pumpAndSettle();
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    expect(find.text('No saved games yet'), findsOneWidget);
  });

  testWidgets('offline Play does not expose phone-only chess', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final _EmptyGameLibraryRepository repository =
        _EmptyGameLibraryRepository();
    addTearDown(repository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();

    expect(
      find.byKey(const ValueKey<String>('play-on-phone-card')),
      findsNothing,
    );
    expect(
      find.textContaining('The physical board stays separate.'),
      findsNothing,
    );
    expect(find.byType(AppChessPlayScreen), findsNothing);
    expect(repository.snapshot.games, isEmpty);
  });

  testWidgets('Recent games opens the exact selected saved game', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final archive = _recentArchive(3);
    final repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(games: <GameArchive>[archive]),
    );
    addTearDown(repository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));

    final Finder recentGame = find.byKey(
      const ValueKey<String>('recent-game-recent-3'),
    );
    final Finder recentGameSemantics = find
        .ancestor(of: recentGame, matching: find.byType(Semantics))
        .first;
    expect(
      tester
          .getSemantics(recentGameSemantics)
          .getSemanticsData()
          .hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    await tester.tap(recentGame);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(
      find.byKey(
        const ValueKey<String>('game-library-detail:recent-3:recent-board-3'),
      ),
      findsOneWidget,
    );
  });

  testWidgets('Play reads only five archives from a 10000-file catalog', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    final Stopwatch performance = Stopwatch()..start();
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final Directory directory = (await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch-play-paged-games-'),
    ))!;
    addTearDown(() async {
      if (await directory.exists()) await directory.delete(recursive: true);
    });
    const int count = 10000;
    late GameLibraryCatalog catalog;
    late GameLibraryWindow window;
    late _PagingCountingGameLibraryStore store;
    late GameLibraryRepository storageRepository;
    await tester.runAsync(() async {
      for (int start = 0; start < count; start += 200) {
        final int end = (start + 200).clamp(start, count);
        await Future.wait<void>(<Future<void>>[
          for (int index = start; index < end; index += 1)
            File(
              '${directory.path}/game-${_pagingArchive(index).libraryId}.json',
            ).writeAsString('${jsonEncode(_pagingArchive(index).toJson())}\n'),
        ]);
      }
      await GameLibraryStorageIndex(directory).replace(
        games: Iterable<GameArchive>.generate(count, _pagingArchive),
        recoveredFiles: const <String>[],
        rejectedFiles: const <String>[],
      );
      store = _PagingCountingGameLibraryStore(directory);
      storageRepository = GameLibraryRepository(store);
      catalog = await storageRepository.loadCatalog();
      window = await storageRepository.loadWindow(offset: 0, limit: 5);
    });
    stderr.writeln(
      'GAME_LIBRARY_PLAY_10K setup_ms=${performance.elapsedMilliseconds}',
    );
    final _PreloadedPagingGameLibraryRepository repository =
        _PreloadedPagingGameLibraryRepository(catalog: catalog, window: window);
    final service = _ConnectionGateBleService();

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 200));
    stderr.writeln(
      'GAME_LIBRARY_PLAY_10K mounted_ms=${performance.elapsedMilliseconds}',
    );

    expect(store.fullLoads, 0);
    expect(store.exactReads, 5);
    expect(repository.fullLoadCalls, 0);
    expect(repository.windowCalls, 1);
    expect(repository.snapshot.games, isEmpty);
    for (final int index in <int>[9999, 9998, 9997]) {
      expect(
        find.byKey(ValueKey<String>('recent-game-recent-$index')),
        findsOneWidget,
      );
    }
    expect(
      find.byKey(const ValueKey<String>('recent-game-recent-9996')),
      findsNothing,
    );

    late GameLibraryCatalog afterSaveCatalog;
    late GameLibraryWindow afterSaveWindow;
    await tester.runAsync(() async {
      await storageRepository.save(_pagingArchive(count));
      afterSaveCatalog = await storageRepository.loadCatalog();
      afterSaveWindow = await storageRepository.loadWindow(offset: 0, limit: 5);
    });
    stderr.writeln(
      'GAME_LIBRARY_PLAY_10K mutated_ms=${performance.elapsedMilliseconds}',
    );
    expect(store.fullLoads, 0);
    expect(store.exactReads, 10);
    expect(storageRepository.snapshot.games, hasLength(1));
    repository.publish(
      catalog: afterSaveCatalog,
      window: afterSaveWindow,
      change: storageRepository.snapshot,
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));

    expect(repository.fullLoadCalls, 0);
    expect(
      repository.windowCalls,
      1,
      reason: 'the one-game change payload updates Recent Games in memory',
    );
    expect(repository.snapshot.games, hasLength(1));
    expect(
      find.byKey(const ValueKey<String>('recent-game-recent-10000')),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await service.dispose();
    await repository.close();
    await tester.runAsync(() async {
      await storageRepository.close();
      await directory.delete(recursive: true);
    });
    performance.stop();
    stderr.writeln(
      'GAME_LIBRARY_PLAY_10K total_ms=${performance.elapsedMilliseconds}',
    );
  });

  testWidgets('a name chosen in Games returns to Play and opens exact replay', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final archive = _recentArchive(3);
    final repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(games: <GameArchive>[archive]),
    );
    addTearDown(repository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    await tester.tap(find.byKey(const ValueKey<String>('game-library-action')));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      'Friday rematch',
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pumpAndSettle();
    await tester.pageBack();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));

    expect(find.text('Friday rematch'), findsOneWidget);
    expect(find.textContaining('P1 (W) won'), findsOneWidget);
    await tester.tap(
      find.byKey(const ValueKey<String>('recent-game-recent-3')),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(
      find.byKey(
        const ValueKey<String>('game-library-detail:recent-3:recent-board-3'),
      ),
      findsOneWidget,
    );
  });

  testWidgets('Recent games resolves loading and retries a failed load', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final deferredRepository = _DeferredGameLibraryRepository();
    addTearDown(deferredRepository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: deferredRepository,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('Loading saved games'), findsOneWidget);
    deferredRepository.pendingLoad.complete(
      GameLibrarySnapshot(games: <GameArchive>[_recentArchive(1)]),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(
      find.byKey(const ValueKey<String>('recent-game-recent-1')),
      findsOneWidget,
    );

    final retryRepository = _RetryGameLibraryRepository(
      GameLibrarySnapshot(games: <GameArchive>[_recentArchive(2)]),
    );
    addTearDown(retryRepository.close);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: retryRepository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(
      find.byKey(const ValueKey<String>('recent-games-retry')),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const ValueKey<String>('recent-games-retry')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(retryRepository.loadCalls, 2);
    expect(
      find.byKey(const ValueKey<String>('recent-game-recent-2')),
      findsOneWidget,
    );
  });

  testWidgets('Recent games reports recovered and rejected library files', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final recoveredRepository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[_recentArchive(1)],
        recoveredFiles: const <String>['restored.json'],
      ),
    );
    addTearDown(recoveredRepository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: recoveredRepository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    final Finder warning = find.byKey(
      const ValueKey<String>('recent-games-recovery-warning'),
    );
    expect(warning, findsOneWidget);
    expect(tester.getSize(warning).height, greaterThanOrEqualTo(48));
    expect(find.text('Some saved games need attention'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('recent-game-recent-1')),
      findsOneWidget,
    );
    expect(
      tester
          .getSemantics(
            find.ancestor(of: warning, matching: find.byType(Semantics)).first,
          )
          .getSemanticsData()
          .label,
      contains('1 game restored'),
    );
    await tester.tap(warning);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    await tester.pageBack();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));

    final rejectedRepository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(
        rejectedFiles: const <String>['unreadable.json'],
      ),
    );
    addTearDown(rejectedRepository.close);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: rejectedRepository,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(
      find.byKey(const ValueKey<String>('recent-games-recovery-warning')),
      findsOneWidget,
    );
    expect(find.text('Saved games aren’t available'), findsOneWidget);
    expect(
      find.text(
        'Games saved on this device will appear here, even when the board is disconnected.',
      ),
      findsNothing,
    );
  });

  testWidgets('connection recovery and discovered boards precede saved games', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(5, _recentArchive),
      ),
    );
    addTearDown(repository.close);
    const boards = <LitchBoard>[
      LitchBoard(id: 'board-1', name: 'Foxie One', rssi: -40),
      LitchBoard(id: 'board-2', name: 'Foxie Two', rssi: -45),
    ];

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: TextScaler.linear(2)),
          child: child!,
        ),
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    service.emitBoards(boards);
    await tester.pumpAndSettle();
    expect(
      tester
          .getTopLeft(
            find.byKey(const ValueKey<String>('discovered-board-board-2')),
          )
          .dy,
      lessThan(
        tester
            .getTopLeft(
              find.byKey(const ValueKey<String>('recent-games-section')),
            )
            .dy,
      ),
    );

    service.emitAvailability(
      BleAvailability.bluetoothPermissionBlocked,
      recoveryNeedsSettings: true,
    );
    await tester.pumpAndSettle();
    expect(
      tester
          .getTopLeft(find.byKey(const ValueKey<String>('bluetooth-recovery')))
          .dy,
      lessThan(
        tester
            .getTopLeft(
              find.byKey(const ValueKey<String>('recent-games-section')),
            )
            .dy,
      ),
    );
  });

  testWidgets('offline recent games remain usable at 320px and 200 percent', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(5, _recentArchive),
      ),
    );
    addTearDown(repository.close);

    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: TextScaler.linear(2)),
          child: child!,
        ),
        home: ConnectionGateScreen(
          bleService: service,
          gameLibrary: repository,
        ),
      ),
    );
    await tester.pump();

    expect(
      find.byKey(const ValueKey<String>('recent-games-section')),
      findsOneWidget,
    );
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('game-library-action')))
          .height,
      greaterThanOrEqualTo(48),
    );
    for (final int index in <int>[4, 3, 2]) {
      expect(
        find.byKey(ValueKey<String>('recent-game-recent-$index')),
        findsOneWidget,
      );
    }
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'play-offline-recent-games-320-200');
  });

  testWidgets('Play opens Connect on warm link loss and keeps Games available', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
    });
    final service = _ConnectionGateBleService(useProductionConsoleStream: true)
      ..reviewBoardRotationKnown = true;
    addTearDown(service.dispose);
    final repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(5, _recentArchive),
      ),
    );
    addTearDown(repository.close);

    await _pumpConnectedGate(tester, service, gameLibrary: repository);
    service.reportHome();
    await tester.pumpAndSettle();
    expect(find.byType(HomeScreen), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('control:hint')), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('control:new-game')),
      findsOneWidget,
    );
    Future<double> sectionPosition(String key) async {
      final section = find.byKey(ValueKey<String>(key));
      await tester.scrollUntilVisible(
        section,
        200,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      final scroll = tester.state<ScrollableState>(_homeDetailsScrollable());
      return tester.getTopLeft(section).dy + scroll.position.pixels;
    }

    final actions = await sectionPosition('home-board-actions-section');
    final moves = await sectionPosition('home-moves-section');
    final recent = await sectionPosition('recent-games-home-section');
    expect(actions, lessThan(moves));
    expect(moves, lessThan(recent));
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('game-library-action')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('game-library-action')),
      findsOneWidget,
    );
    for (final int index in <int>[4, 3, 2]) {
      expect(
        find.byKey(ValueKey<String>('recent-game-recent-$index')),
        findsOneWidget,
      );
    }
    for (final int index in <int>[1, 0]) {
      expect(
        find.byKey(ValueKey<String>('recent-game-recent-$index')),
        findsNothing,
      );
    }
    await decodeVisibleReviewImages(tester);
    await captureHealthReview(
      tester,
      'play-connected-recent-games-390',
      boundaryFinder: find.byKey(
        const ValueKey<String>('connected-gate-render'),
      ),
    );

    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    service.emitBoards(const <LitchBoard>[]);
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.byType(HomeScreen), findsNothing);
    expect(
      find.byKey(const ValueKey('connection-gate-content')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('control:hint')), findsNothing);
    expect(find.byKey(const ValueKey('control:new-game')), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('game-library-action')),
      findsOneWidget,
    );
    expect(find.text('Console'), findsNothing);
    final BuildContext gateContext = tester.element(
      find.byType(ConnectionGateScreen),
    );
    await tester.runAsync(() async {
      for (final Image image in tester.widgetList<Image>(find.byType(Image))) {
        await precacheImage(image.image, gateContext);
      }
    });
    await tester.pump(const Duration(milliseconds: 100));
    await captureHealthReview(tester, 'play-offline-recent-games-390');
  });

  testWidgets('saved games are available from the connected shell', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final GameLibraryRepository repository = _EmptyGameLibraryRepository();
    addTearDown(repository.close);

    await _pumpConnectedGate(tester, service, gameLibrary: repository);
    expect(find.text('Connected'), findsOneWidget);
    await _openGamesFromConnectedPlay(tester);
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    expect(find.text('No saved games yet'), findsOneWidget);
  });

  testWidgets('Games action belongs to Play and not other board screens', (
    WidgetTester tester,
  ) async {
    _usePortraitPhone(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{
      'litchcraft_discovery_dismissed': true,
      'enable_app_console': true,
    });
    final service = _ConnectionGateBleService();
    final craft = _NavigationCraftController(service);
    final library = LitchcraftLibrary(craft);
    final repository = _EmptyGameLibraryRepository();
    service.emitConnection('board-1', DeviceConnectionState.connected);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const <LitchBoard>[],
          onDisconnect: service.disconnect,
          craftController: craft,
          craftLibrary: library,
          initialCraftModeResolved: true,
          gameLibrary: repository,
          loadAppConsolePreference: () async => true,
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(_activeNavigationIndex(tester), 0);

    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('game-library-action')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('game-library-action')),
      findsOneWidget,
    );
    expect(find.text('Previous games'), findsOneWidget);

    for (final String label in <String>['Litchcraft', 'Setup', 'Console']) {
      await tester.tap(_navigationAction(tester, label.toLowerCase()));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('game-library-action')),
        findsNothing,
        reason: label,
      );
    }

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    craft.dispose();
    await repository.close();
    await service.dispose();
  });

  testWidgets('disconnect preserves the local replay position chooser', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final archive = completedClassicArchive();
    final GameLibraryRepository repository = _EmptyGameLibraryRepository(
      snapshot: GameLibrarySnapshot(games: <GameArchive>[archive]),
    );
    addTearDown(repository.close);

    await _pumpConnectedGate(tester, service, gameLibrary: repository);
    await _openGamesFromConnectedPlay(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    final Finder choosePosition = find.byKey(
      const ValueKey<String>('review-choose-position'),
    );
    await tester.ensureVisible(choosePosition);
    await tester.pumpAndSettle();
    await tester.tap(choosePosition);
    await tester.pumpAndSettle();
    expect(find.text('Choose a move'), findsOneWidget);

    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();

    expect(find.text('Choose a move'), findsOneWidget);
    await tester.tap(
      find
          .descendant(
            of: find.byType(BottomSheet),
            matching: find.byType(ListTile),
          )
          .first,
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('Starting position'), findsWidgets);
  });

  testWidgets('disconnect preserves an editable local FEN preview journey', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final _ConnectionGateBleService service = _ConnectionGateBleService();
    addTearDown(service.dispose);
    final GameLibraryRepository repository = _EmptyGameLibraryRepository();
    addTearDown(repository.close);

    await _pumpConnectedGate(tester, service, gameLibrary: repository);
    await _openGamesFromConnectedPlay(tester);
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-fen-preview')),
    );
    await tester.pumpAndSettle();

    const draft = '4k3/8/8/8/8/8/8/4K3 w - - 0 1';
    await tester.enterText(
      find.byKey(const ValueKey<String>('fen-preview-field')),
      draft,
    );
    service.emitConnection('board-1', DeviceConnectionState.disconnected);
    await tester.pump();
    expect(find.byType(FenPreviewScreen), findsOneWidget);
    expect(
      tester
          .widget<TextField>(
            find.byKey(const ValueKey<String>('fen-preview-field')),
          )
          .controller!
          .text,
      draft,
    );

    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('fen-preview-validate')),
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('fen-preview-validate')),
    );
    await tester.pumpAndSettle();
    expect(find.text(draft), findsOneWidget);
    expect(
      find.text('Preview on this device. Your board is unchanged.'),
      findsOneWidget,
    );
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('fen-preview-edit-again')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('fen-preview-edit-again')),
    );
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('fen-preview-field')),
      findsOneWidget,
    );
    await tester.pageBack();
    await tester.pumpAndSettle();
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    await tester.pageBack();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(find.byType(ConnectionGateScreen), findsOneWidget);
  });

  const adaptiveCraftCaptures = <(String, Size, EdgeInsets, Alignment)>[
    (
      'android-portrait',
      Size(384, 832),
      EdgeInsets.fromLTRB(0, 28, 0, 24),
      Alignment.centerRight,
    ),
    (
      'android-landscape-left',
      Size(832, 384),
      EdgeInsets.fromLTRB(34, 0, 48, 0),
      Alignment.centerRight,
    ),
    (
      'android-landscape-right',
      Size(832, 384),
      EdgeInsets.fromLTRB(48, 0, 34, 0),
      Alignment.centerLeft,
    ),
    (
      'android-landscape-left-200',
      Size(832, 384),
      EdgeInsets.fromLTRB(34, 0, 48, 0),
      Alignment.centerRight,
    ),
    (
      'android-landscape-right-200',
      Size(832, 384),
      EdgeInsets.fromLTRB(48, 0, 34, 0),
      Alignment.centerLeft,
    ),
    (
      'android-portrait-200',
      Size(384, 832),
      EdgeInsets.fromLTRB(0, 28, 0, 24),
      Alignment.centerRight,
    ),
    (
      'iphone-standard-portrait',
      Size(390, 844),
      EdgeInsets.fromLTRB(0, 47, 0, 34),
      Alignment.centerRight,
    ),
    (
      'iphone-standard-portrait-200',
      Size(390, 844),
      EdgeInsets.fromLTRB(0, 47, 0, 34),
      Alignment.centerRight,
    ),
    (
      'iphone-standard-landscape-left',
      Size(844, 390),
      EdgeInsets.fromLTRB(54, 0, 22, 0),
      Alignment.centerLeft,
    ),
    (
      'iphone-standard-landscape-right',
      Size(844, 390),
      EdgeInsets.fromLTRB(22, 0, 54, 0),
      Alignment.centerRight,
    ),
    (
      'iphone-duo-outer-portrait',
      Size(466, 678),
      EdgeInsets.fromLTRB(0, 18, 0, 12),
      Alignment.centerRight,
    ),
    (
      'iphone-duo-outer-landscape',
      Size(678, 466),
      EdgeInsets.fromLTRB(28, 0, 12, 0),
      Alignment.centerLeft,
    ),
    (
      'iphone-duo-inner-portrait',
      Size(626, 890),
      EdgeInsets.fromLTRB(18, 16, 8, 12),
      Alignment.centerRight,
    ),
    (
      'iphone-duo-inner-landscape',
      Size(890, 626),
      EdgeInsets.fromLTRB(16, 18, 8, 12),
      Alignment.centerLeft,
    ),
    (
      'ipad-large-portrait',
      Size(1024, 1366),
      EdgeInsets.fromLTRB(0, 24, 0, 20),
      Alignment.centerRight,
    ),
  ];
  for (final scenario in adaptiveCraftCaptures) {
    testWidgets('adaptive Litchcraft review ${scenario.$1}', (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = scenario.$2;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final service = _ConnectionGateBleService();
      service.emitConnection('board-1', DeviceConnectionState.connected);
      final previewCanvas =
          LitchcraftCanvas.blank(topology: LitchcraftTopology.v3)
              .paint([0, 9, 18, 27, 36, 45, 54, 63], 15, 0xff7430)
              .paint([7, 14, 21, 28, 35, 42, 49, 56], 15, 0x28d66f)
              .paint([3, 4, 11, 12, 19, 20], 15, 0x914cff);
      final craft = _NavigationCraftController(
        service,
        initialCanvas: previewCanvas,
      )..forceLiveStatus = true;
      craft.simulatePhysicalMode(true);
      final library = LitchcraftLibrary(craft);
      await library.ready;
      await library.dismissWelcome();
      addTearDown(() async {
        library.dispose();
        craft.dispose();
        await service.dispose();
      });

      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme().copyWith(platform: TargetPlatform.iOS),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              padding: scenario.$3,
              viewPadding: scenario.$3,
              textScaler: TextScaler.linear(
                scenario.$1.endsWith('-200') ? 2 : 1,
              ),
              disableAnimations: true,
            ),
            child: Theme(
              data: Theme.of(context).copyWith(
                platform: scenario.$1.startsWith('android')
                    ? TargetPlatform.android
                    : TargetPlatform.iOS,
              ),
              child: child!,
            ),
          ),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            craftController: craft,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
            landscapeRibbonSide: scenario.$4,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await decodeVisibleReviewImages(tester);
      expect(find.byKey(const ValueKey('litchcraft-canvas')), findsOneWidget);
      final craftToolbar = find.byKey(const ValueKey('craft-toolbar'));
      expect(
        find.descendant(of: craftToolbar, matching: find.byType(Text)),
        findsNothing,
        reason:
            'Litchcraft section buttons are icon-only; their names remain in semantics and tooltips.',
      );
      void expectPlayBoardBreathingSpace() {
        final outer = tester.getRect(
          find.byKey(const ValueKey('home-board-breathing-space')),
        );
        final board = tester.getRect(find.byType(HomeChessboard));
        if (scenario.$2.width > scenario.$2.height) {
          final minimum = LitchWorkspaceGeometry.landscapeGap;
          expect(
            board.left,
            greaterThanOrEqualTo(math.max(scenario.$3.left, minimum)),
          );
          expect(
            board.right,
            lessThanOrEqualTo(
              scenario.$2.width - math.max(scenario.$3.right, minimum),
            ),
          );
          expect(
            board.top,
            greaterThanOrEqualTo(math.max(scenario.$3.top, minimum)),
          );
          expect(
            board.bottom,
            lessThanOrEqualTo(
              scenario.$2.height - math.max(scenario.$3.bottom, minimum),
            ),
          );
          return;
        }
        final gaps = <double>[
          board.left - outer.left,
          outer.right - board.right,
          board.top - scenario.$3.top,
          tester.getRect(find.byType(NavigationBar)).top - board.bottom,
        ];
        for (final gap in gaps) {
          expect(
            gap,
            greaterThanOrEqualTo(LitchWorkspaceGeometry.outerGap),
            reason:
                'Every portrait board edge must retain chamfer breathing space.',
          );
        }
      }

      expect(tester.takeException(), isNull);
      final initialCraftBoard = tester.getRect(
        find.byKey(const ValueKey('craft-ribbon-board')),
      );
      final hex = tester
          .state<EditableTextState>(
            find.descendant(
              of: find.byKey(const ValueKey('craft-inspector-hex')),
              matching: find.byType(EditableText),
            ),
          )
          .renderEditable;
      expect(
        hex.maxScrollExtent,
        0,
        reason: 'The complete seven-character hex value must remain visible.',
      );
      await captureHealthReview(tester, 'adaptive-${scenario.$1}');
      if (scenario.$1.endsWith('portrait-200')) {
        await tester.tap(find.byKey(const ValueKey('craft-motion')));
        await tester.pump();
        expect(tester.takeException(), isNull);
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-effects');
      }
      if (scenario.$2.height > scenario.$2.width) {
        final tablet = LitchWorkspaceGeometry.isTabletWindow(scenario.$2);
        final utilities = tester.getRect(
          find.byKey(const ValueKey('craft-history-footer')),
        );
        if (!tablet) {
          final workspace = tester.getRect(
            find.byKey(const ValueKey('craft-portrait-shell')),
          );
          final inspector = tester.getRect(
            find.byKey(const ValueKey('craft-persistent-inspector')),
          );
          final palette = tester.getRect(
            find.byKey(const ValueKey('craft-color-wheel-rail')),
          );
          final quick = tester.getRect(
            find.byKey(const ValueKey('craft-quick-actions-capsule-portrait')),
          );
          final inset = LitchWorkspaceGeometry.contentEdgeInset;
          expect(inspector.left, closeTo(workspace.left + inset, .01));
          expect(inspector.right, closeTo(workspace.right - inset, .01));
          expect(quick.left, closeTo(inspector.left, .01));
          expect(palette.right, closeTo(inspector.right, .01));
          final tools = tester.getRect(find.byKey(const ValueKey('craft-toolbar')));
          final content = tester.getRect(find.byKey(const ValueKey('craft-inspector-content')));
          expect(content.bottom, lessThanOrEqualTo(utilities.top));
          expect(utilities.bottom, lessThanOrEqualTo(tools.top));
        } else {
          final tray = tester.getRect(find.byKey(const ValueKey('craft-portrait-tools-tray')));
          final navigation = tester.getRect(find.byType(NavigationBar));
          expect(tray.top, greaterThanOrEqualTo(initialCraftBoard.bottom));
          expect(tray.bottom, lessThanOrEqualTo(navigation.top));
          expect(tray.contains(utilities.center), isTrue);
          for (final group in ['color', 'leds', 'effects', 'frames']) {
            if (scenario.$1 == 'iphone-duo-inner-portrait') {
              final tab = find.byKey(ValueKey('craft-portrait-select-$group'));
              expect(tester.getSize(tab).height, greaterThanOrEqualTo(48));
              await tester.tap(tab);
              await tester.pumpAndSettle();
              expect(tester.getRect(find.byKey(const ValueKey('craft-ribbon-board'))), initialCraftBoard);
            }
            final section = tester.getRect(find.byKey(ValueKey('craft-tablet-section-$group')));
            expect(section.top, greaterThanOrEqualTo(utilities.bottom));
            expect(section.bottom, lessThanOrEqualTo(tray.bottom));
          }
        }
        for (final key in [
          'craft-undo',
          'craft-redo',
          'craft-rotate-artwork',
          if (!tablet) ...['craft-palette', 'craft-leds', 'craft-motion', 'craft-animate', 'craft-artwork-menu'],
        ]) {
          final rect = tester.getRect(find.byKey(ValueKey(key)));
          expect(rect.width, greaterThanOrEqualTo(48));
          expect(rect.height, greaterThanOrEqualTo(48));
        }
      }
      if (scenario.$1 == 'android-portrait' ||
          scenario.$1.startsWith('iphone-standard-portrait')) {
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final quick = tester.getRect(
          find.byKey(const ValueKey('craft-quick-actions-capsule-portrait')),
        );
        final navigation = tester.getRect(find.byType(NavigationBar));
        expect(inspector.top - quick.bottom, greaterThanOrEqualTo(8));
        final palette = tester.getRect(
          find.byKey(const ValueKey('craft-color-wheel-rail')),
        );
        expect(palette.left - quick.right, greaterThanOrEqualTo(8));
        expect(inspector.top - palette.bottom, greaterThanOrEqualTo(8));
        expect(navigation.top - inspector.bottom, greaterThanOrEqualTo(8));
        final toolbar = tester.getRect(find.byType(AppBar));
        final navigationIcons = [
          for (final key in [
            'main-destination-play',
            'main-destination-craft',
            'main-destination-setup',
          ])
            tester.getRect(find.byKey(ValueKey(key))).center,
        ];
        expect(
          tester
              .widget<LitchPlayGlyph>(
                find.descendant(
                  of: find.byType(NavigationBar),
                  matching: find.byType(LitchPlayGlyph),
                ),
              )
              .size,
          31,
        );
        final brand = tester.getRect(find.byKey(const ValueKey('open-about')));
        await tester.tap(find.byKey(const ValueKey('main-destination-play')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-play');
        expectPlayBoardBreathingSpace();
        if (scenario.$1 == 'android-portrait' ||
            scenario.$1 == 'iphone-standard-portrait') {
          final playBoard = tester.getRect(find.byType(HomeChessboard));
          expect(
            playBoard.width,
            closeTo(initialCraftBoard.width, .01),
            reason: 'Play and Litchcraft share one portrait board width.',
          );
          expect(
            playBoard.left,
            closeTo(LitchWorkspaceGeometry.contentEdgeInset, .01),
          );
          expect(
            scenario.$2.width - playBoard.right,
            closeTo(LitchWorkspaceGeometry.contentEdgeInset, .01),
          );
        }
        expect(tester.getRect(find.byType(AppBar)), toolbar);
        expect(tester.getRect(find.byType(NavigationBar)), navigation);
        expect([
          for (final key in [
            'main-destination-play',
            'main-destination-craft',
            'main-destination-setup',
          ])
            tester.getRect(find.byKey(ValueKey(key))).center,
        ], navigationIcons);
        expect(
          tester.getRect(find.byKey(const ValueKey('open-about'))).center.dy,
          brand.center.dy,
        );
        await tester.tap(find.byKey(const ValueKey('main-destination-setup')));
        await tester.pump();
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          2,
        );
        await tester.pump(const Duration(milliseconds: 400));
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-setup');
        expect(tester.getRect(find.byType(AppBar)), toolbar);
        expect(tester.getRect(find.byType(NavigationBar)), navigation);
        expect(
          tester.getRect(find.byKey(const ValueKey('open-about'))).center.dy,
          brand.center.dy,
        );
      }
      if (scenario.$2.width > scenario.$2.height && scenario.$2.height <= 480) {
        final renderBoard = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        final eyedropper = tester.getRect(
          find.byKey(const ValueKey('craft-quick-eyedropper')),
        );
        final currentColor = tester.getRect(
          find.byKey(const ValueKey('craft-current-color-dot')),
        );
        final previousColor = tester.getRect(
          find.byKey(const ValueKey('craft-previous-color-dot')),
        );
        expect(
          eyedropper.width,
          closeTo(25.6, .01),
          reason: 'The open landscape eyedropper is optically reduced by 20%.',
        );
        expect(currentColor.width, greaterThan(previousColor.width));
        expect(eyedropper.bottom - currentColor.top, lessThanOrEqualTo(2));
        debugPrint(
          'Shell measured ${scenario.$1}: board=$renderBoard inspector=${tester.getRect(find.byKey(const ValueKey('craft-persistent-inspector')))}',
        );
        await tester.tap(find.byKey(const ValueKey('craft-motion')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        expect(tester.takeException(), isNull);
        expect(
          find.descendant(
            of: find.byKey(const ValueKey('craft-effect-panel')),
            matching: find.byType(Scrollable),
          ),
          findsNothing,
        );
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-effects');
        final navigation = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        expect(
          tester
              .getSize(find.byKey(const ValueKey('craft-landscape-navigation')))
              .height,
          closeTo(LitchWorkspaceGeometry.navigationHeight, .01),
        );
        expect(LitchWorkspaceGeometry.navigationHeight, closeTo(76.8, .01));
        expect(
          find.descendant(
            of: find.byKey(const ValueKey('craft-landscape-navigation')),
            matching: find.byIcon(Icons.sensors),
          ),
          findsOneWidget,
        );
        expect(
          tester
              .widget<Icon>(
                find.descendant(
                  of: find.byKey(const ValueKey('craft-landscape-navigation')),
                  matching: find.byIcon(Icons.sensors),
                ),
              )
              .size,
          36,
        );
        expect(
          find.descendant(
            of: find.byKey(const ValueKey('craft-landscape-navigation')),
            matching: find.byIcon(Icons.keyboard_arrow_up_rounded),
          ),
          findsNothing,
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final inspectorPanel = tester.getRect(
          find.byKey(const ValueKey('craft-inspector')),
        );
        expect(
          tester
              .widget<Material>(
                find.byKey(
                  const ValueKey('craft-landscape-navigation-surface'),
                ),
              )
              .borderRadius,
          BorderRadius.circular(LitchShapes.panelRadius),
          reason: 'Mini-nav and its containing tool panel share one radius.',
        );
        expect(navigation.left, closeTo(inspectorPanel.left, .01));
        expect(navigation.right, closeTo(inspectorPanel.right, .01));
        expect(navigation.bottom, closeTo(inspectorPanel.bottom, .01));
        final quick = tester.getRect(
          find.byKey(const ValueKey('craft-quick-actions')),
        );
        final safeLeft = scenario.$3.left;
        final safeRight = scenario.$2.width - scenario.$3.right;
        final left = scenario.$4.x < 0 ? inspector.left : quick.left;
        final right = scenario.$4.x < 0 ? quick.right : inspector.right;
        expect(
          left,
          closeTo(math.max(safeLeft, LitchWorkspaceGeometry.landscapeGap), .01),
        );
        expect(
          right,
          closeTo(
            math.min(
              safeRight,
              scenario.$2.width - LitchWorkspaceGeometry.landscapeGap,
            ),
            .01,
          ),
        );
        expect(find.text('Connected'), findsNothing);
        for (final action in ['play', 'craft', 'setup']) {
          final rect = tester.getRect(
            find.byKey(ValueKey('landscape-$action-action')),
          );
          expect(rect.width, greaterThanOrEqualTo(48));
          expect(rect.height, greaterThanOrEqualTo(48));
        }
        final pager = find.byKey(const ValueKey('main-pages'));
        final pagerRect = tester.getRect(pager);
        final warmPlay = tester.element(
          find.byKey(const ValueKey('play-page'), skipOffstage: false),
        );
        final warmSetup = tester.element(
          find.byKey(const ValueKey('setup-page'), skipOffstage: false),
        );
        final swipe = await tester.startGesture(
          Offset(pagerRect.left + pagerRect.width * .12, pagerRect.top + 4),
        );
        for (var step = 0; step < 8; step++) {
          await swipe.moveBy(Offset(pagerRect.width * .10, 0));
          await tester.pump(const Duration(milliseconds: 40));
          expect(
            tester.getRect(pager),
            pagerRect,
            reason: 'Dragging must not change workspace geometry.',
          );
        }
        await swipe.up();
        await tester.pumpAndSettle();
        expect(
          tester.element(
            find.byKey(const ValueKey('play-page'), skipOffstage: false),
          ),
          same(warmPlay),
        );
        expect(
          tester.element(
            find.byKey(const ValueKey('setup-page'), skipOffstage: false),
          ),
          same(warmSetup),
        );
        expect(
          tester.widget<PageView>(pager).controller!.page,
          moreOrLessEquals(1),
        );
        await tester.tap(_navigationAction(tester, 'play'));
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        final playBoard = tester.getRect(
          find.byKey(const ValueKey('home-landscape-workspace')),
        );
        final details = tester.getRect(
          find.byKey(const ValueKey('home-details-scroll')),
        );
        final playCanvas = tester.getRect(find.byType(HomeChessboard));
        expectPlayBoardBreathingSpace();
        final surface = tester.getRect(
          find.byKey(const ValueKey('home-game-surface')),
        );
        expect(
          surface.top,
          closeTo(
            tester
                .getRect(find.byKey(const ValueKey('home-chessboard-surface')))
                .top,
            .01,
          ),
          reason: 'game status and actual board have aligned top edges',
        );
        expect(
          surface.left - details.left,
          closeTo(LitchWorkspaceGeometry.landscapeGap, .01),
          reason: 'status left gap',
        );
        expect(
          details.right - surface.right,
          closeTo(LitchWorkspaceGeometry.landscapeGap, .01),
          reason: 'status right gap',
        );
        expect(
          playCanvas.top - playBoard.top,
          closeTo(0, .01),
          reason: 'board top within already padded workspace',
        );
        if (scenario.$4.x < 0) {
          expect(details.right, lessThanOrEqualTo(playCanvas.left));
        } else {
          expect(playCanvas.right, lessThanOrEqualTo(details.left));
        }
        expect(
          details.bottom,
          greaterThan(navigation.top),
          reason:
              'The compact navigation floats over the Play details pane; it '
              'must not carve a footer-height hole out of the workspace.',
        );
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-play-top');
        final detailsScroll = _homeDetailsScrollable();
        final position = tester.state<ScrollableState>(detailsScroll).position;
        var previousOffset = -1.0;
        while (position.extentAfter > .01 && position.pixels > previousOffset) {
          previousOffset = position.pixels;
          await tester.drag(detailsScroll, Offset(0, -details.height * .45));
          await tester.pumpAndSettle();
        }
        expect(
          position.extentAfter,
          lessThanOrEqualTo(.01),
          reason: 'The final Moves row must clear navigation at the end of the scroll.',
        );
        expect(
          tester.getRect(
            find.byKey(const ValueKey('home-landscape-workspace')),
          ),
          playBoard,
        );
        expect(
          tester
              .getRect(find.byKey(const ValueKey('home-moves-section')))
              .bottom,
          lessThanOrEqualTo(navigation.top),
        );
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-play');
        final playNavigation = tester.getRect(
          find.byKey(const ValueKey('landscape-navigation-switcher')),
        );
        await tester.tap(find.byKey(const ValueKey('landscape-setup-action')));
        await tester.pump();
        expect(
          tester
              .getSemantics(
                find.byKey(const ValueKey('landscape-setup-action')),
              )
              .flagsCollection
              .isSelected,
          ui.Tristate.isTrue,
        );
        await tester.pump(const Duration(milliseconds: 400));
        await captureHealthReview(tester, 'adaptive-${scenario.$1}-setup');
        if (scenario.$2.width >= 832 && !scenario.$1.endsWith('-200')) {
          final playerColor = tester.getRect(
            find.byKey(const ValueKey('quick:P2_PIECE_WHEEL')),
          );
          final lights = tester.getRect(
            find.byKey(const ValueKey('setup-open:lights')),
          );
          expect(lights.left, greaterThan(playerColor.right));
          expect(lights.top, lessThanOrEqualTo(playerColor.top));
        }
        final setupSearch = find.byKey(const ValueKey('advanced-search'));
        final setupScrollable = find.byWidget(
          Scrollable.of(
            tester.element(setupSearch),
            axis: Axis.vertical,
          ).widget,
        );
        final setupViewport = find.ancestor(
          of: setupSearch,
          matching: find.byKey(const ValueKey('settings-scroll')),
        );
        await tester.scrollUntilVisible(
          setupSearch,
          160,
          scrollable: setupScrollable,
        );
        await tester.pumpAndSettle();
        expect(setupSearch.hitTestable(), findsOneWidget);
        expect(
          find.ancestor(of: setupSearch, matching: setupScrollable),
          findsOneWidget,
        );
        final searchBeforeDrag = tester.getRect(setupSearch);
        await tester.drag(setupScrollable, const Offset(0, 48));
        await tester.pumpAndSettle();
        expect(
          tester.getRect(setupSearch).top,
          greaterThan(searchBeforeDrag.top),
          reason: 'Search is the last scrolling item, not a pinned footer.',
        );
        await tester.ensureVisible(setupSearch);
        await tester.pumpAndSettle();
        final searchRect = tester.getRect(setupSearch);
        expect(searchRect.width, greaterThanOrEqualTo(48));
        expect(searchRect.height, greaterThanOrEqualTo(48));
        expect(searchRect.left, greaterThanOrEqualTo(pagerRect.left));
        expect(searchRect.right, lessThanOrEqualTo(pagerRect.right));
        expect(
          tester.getRect(setupViewport).bottom,
          closeTo(pagerRect.bottom, .01),
          reason: 'Setup uses the full safe height beneath the floating nav.',
        );
        expect(
          searchRect.bottom,
          lessThanOrEqualTo(playNavigation.top),
          reason: 'Trailing scroll space lets the last control clear the nav.',
        );
        await tester.showKeyboard(setupSearch);
        tester.view.viewInsets = const FakeViewPadding(bottom: 260);
        addTearDown(tester.view.resetViewInsets);
        await tester.pumpAndSettle();
        await tester.ensureVisible(setupSearch);
        await tester.pumpAndSettle();
        expect(setupSearch.hitTestable(), findsOneWidget);
        final keyboardSearch = tester.getRect(setupSearch);
        final keyboardViewport = tester.getRect(setupViewport);
        expect(keyboardSearch.top, greaterThanOrEqualTo(keyboardViewport.top));
        expect(
          keyboardSearch.bottom,
          lessThanOrEqualTo(keyboardViewport.bottom),
        );
        expect(
          tester.widget<TextField>(setupSearch).focusNode!.hasFocus,
          isTrue,
        );
        tester.testTextInput.hide();
        tester.widget<TextField>(setupSearch).focusNode!.unfocus();
        tester.view.resetViewInsets();
        await tester.pumpAndSettle();
        expect(
          tester.getRect(
            find.byKey(const ValueKey('landscape-navigation-switcher')),
          ),
          playNavigation,
        );
        expect(playNavigation.left, closeTo(inspectorPanel.left, .01));
        expect(playNavigation.right, closeTo(inspectorPanel.right, .01));
        expect(
          inspectorPanel.bottom - playNavigation.bottom,
          closeTo(0, .01),
          reason:
              'Play, Setup, and Litchcraft must share one navigation baseline.',
        );
      }
    });
  }
}
