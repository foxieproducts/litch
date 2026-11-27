import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart'
    show debugPrintSynchronously, debugDefaultTargetPlatformOverride;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/board_actions_controller.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/localization/litch_strings.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/firmware_provenance.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/device_landscape_side.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:litch_mobile/src/widgets/settings_ui.dart';
import 'package:litch_mobile/src/widgets/orientation_workspace.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';
import 'package:litch_mobile/src/widgets/board_controls_glyph.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:package_info_plus/package_info_plus.dart';

import 'firmware_screen_test.dart' show FlakyFirmwareDownloadService;
import 'health_render_capture.dart';
import 'localization_test_support.dart';
import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

import 'dart:ui' as ui;

OutlinedButton _coachingButton(WidgetTester tester, Finder control) =>
    tester.widget<OutlinedButton>(control);

Future<void> _revealSettingsSearch(WidgetTester tester) async {
  final search = find.byKey(const ValueKey<String>('advanced-search'));
  await tester.ensureVisible(search);
  await tester.pumpAndSettle();
  final scroll = find
      .ancestor(of: search, matching: find.byType(Scrollable))
      .first;
  final position = tester.state<ScrollableState>(scroll).position;
  position.jumpTo(position.maxScrollExtent);
  await tester.pumpAndSettle();
}

Future<void> _openSettingsPage(WidgetTester tester, String page) async {
  final search = find.byKey(const ValueKey<String>('advanced-search'));
  if (page == 'search') {
    // Search belongs to the overview; reach it through the real page Back.
    final back = find.byKey(const ValueKey<String>('setup-back'));
    while (back.evaluate().isNotEmpty) {
      await tester.tap(back);
      await tester.pumpAndSettle();
    }
    await _revealSettingsSearch(tester);
    await tester.tap(search);
    await tester.pumpAndSettle();
    return;
  }
  if (search.evaluate().isNotEmpty) {
    final field = tester.widget<TextField>(search);
    if (field.controller!.text.isNotEmpty) {
      await _revealSettingsSearch(tester);
      await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
      await tester.pumpAndSettle();
    }
    field.focusNode!.unfocus();
    await tester.pumpAndSettle();
  }
  final back = find.byKey(const ValueKey<String>('setup-back'));
  while (back.evaluate().isNotEmpty) {
    await tester.ensureVisible(back);
    await tester.tap(back);
    await tester.pumpAndSettle();
  }
  if (page == 'colors' || page == 'overview') return;
  final destination = find.byKey(ValueKey<String>('setup-open:$page'));
  await tester.ensureVisible(destination);
  await tester.pumpAndSettle();
  await tester.tap(destination);
  await tester.pumpAndSettle();
}

class _FakeFlutterReactiveBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}

class _PaletteBoard extends SettingsSessionBoard {
  _PaletteBoard({super.includeHomeSnapshot}) {
    configureSettings = (data) {
      data
        ..setUint16(4, brightness, Endian.little)
        ..setUint16(10, p2Wheel, Endian.little)
        ..setUint16(24, rotated ? 1 : 0, Endian.little)
        ..setUint8(78, wheel)
        ..setUint8(79, p2Wheel);
    };
  }

  int brightness = 5;
  int p2Wheel = 200;
  bool rotated = false;

  void deliverSettings(LitchBleService service) {
    for (final packet in settingsPackets()) {
      service.debugCaptureConnectionSessionState(packet);
    }
  }
}

class _SettingWrite {
  const _SettingWrite({
    required this.key,
    required this.value,
    required this.awaitAck,
  });

  final String key;
  final Object value;
  final bool awaitAck;
}

class _ControlCall {
  const _ControlCall({
    required this.requestId,
    required this.action,
    required this.owner,
    required this.enabled,
    required this.undoCount,
  });

  final int requestId;
  final LitchControlAction action;
  final LitchControlOwner? owner;
  final bool? enabled;
  final int undoCount;
}

class _SupportActionSink implements AppTraceSink, AppUserActionSink {
  @override
  final userActions = SupportActionHistory(() => 0);
  @override
  int get supportMonotonicMs => 0;
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {}
}

class _FakeLitchBleService extends LitchBleService {
  _FakeLitchBleService({
    List<LitchSetting>? settings,
    FlutterReactiveBle? ble,
    super.appTrace,
  }) : settingsToReturn = settings ?? _defaultSettings,
      super(
        ble: ble ?? _FakeFlutterReactiveBle(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );

  final StreamController<LitchControlEvent> controlController =
      StreamController<LitchControlEvent>.broadcast(sync: true);
  final StreamController<ConnectionStateUpdate> connectionController =
      StreamController<ConnectionStateUpdate>.broadcast(sync: true);
  final StreamController<List<String>> consoleController =
      StreamController<List<String>>.broadcast(sync: true);
  final StreamController<HomeStateNotification> homeStateController =
      StreamController<HomeStateNotification>.broadcast(sync: true);
  List<String> bufferedConsoleLines = const <String>[];
  HomeStateNotification? retainedHomeState;
  List<LitchSetting> settingsToReturn;
  final List<_SettingWrite> settingWrites = <_SettingWrite>[];
  final List<_ControlCall> controlCalls = <_ControlCall>[];
  final Map<String, Object> boardValues = <String, Object>{
    'VS_CPU_LEVEL': 0,
    'VS_CPU_PLAYER': 2,
  };
  final Map<String, List<Completer<Object?>>> deferredValueReads = {};
  final List<String> settingsReadDeviceIds = <String>[];
  Completer<List<LitchSetting>>? settingsReadCompleter;
  LitchSettingsReadAuthority settingsAuthority = LitchSettingsReadAuthority.confirmed;
  LitchEndPalette? endPaletteToReturn;
  Completer<LitchEndPalette?>? deferredEndPalette;
  bool settingWriteResult = true;
  Completer<bool>? settingWriteCompleter;
  Completer<LitchSettingWriteResult>? acknowledgedSettingWriteCompleter;
  LitchSettingWriteResult? acknowledgedSettingWriteResult;
  LitchControlAvailability availability = LitchControlAvailability.available;
  int _nextRequestId = 1;
  int snapshotRequests = 0;
  VoidCallback? onSnapshotRequest;

  @override
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async {
    snapshotRequests++;
    onSnapshotRequest?.call();
    return true;
  }

  @override
  bool get isConnected => true;

  String? activeBoardId = 'board-1';

  @override
  String? get connectedId => activeBoardId;

  RuntimeHealthResult runtimeHealth = const RuntimeHealthUnavailable(
    RuntimeHealthUnavailableReason.unsupported,
  );
  LitchDeviceCapabilities capabilities = const LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask: LitchDeviceCapabilities.settingsBit |
        LitchDeviceCapabilities.firmwareUpdateBit |
        LitchDeviceCapabilities.firmwareDigestBit,
    explicitlyReported: true,
  );
  RuntimeHealthRecheckResult? runtimeHealthRecheckResult;
  int runtimeHealthFetchCalls = 0;
  int runtimeHealthRecheckCalls = 0;

  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async {
    ++runtimeHealthFetchCalls;
    return runtimeHealth;
  }

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) => capabilities;

  @override
  Future<RuntimeHealthRecheckResult> recheckRuntimeHealth({
    required String deviceId,
  }) async {
    ++runtimeHealthRecheckCalls;
    return runtimeHealthRecheckResult ??
        RuntimeHealthRecheckResult(
          outcome: RuntimeHealthRecheckOutcome.unsupported,
          health: runtimeHealth,
        );
  }

  @override
  Stream<LitchControlEvent> get controlActionEvents => controlController.stream;

  @override
  Stream<ConnectionStateUpdate> get connectionStream =>
      connectionController.stream;

  @override
  Stream<List<String>> get consoleStream => consoleController.stream;

  @override
  List<String> get consoleLinesSnapshot => bufferedConsoleLines;

  @override
  Stream<HomeStateNotification> get homeStateStream =>
      homeStateController.stream;

  @override
  HomeStateNotification? get latestHomeState => retainedHomeState;

  void emitSettingValue(String key, Object value) {
    boardValues[key] = value;
    debugHandleSettingsRwPayload('board-1', <String, Object?>{
      'k': key,
      'v': value,
    });
  }

  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async {
    settingsReadDeviceIds.add(deviceId);
    final Completer<List<LitchSetting>>? deferred = settingsReadCompleter;
    if (deferred != null) {
      return deferred.future;
    }
    for (final LitchSetting setting in settingsToReturn) {
      boardValues.putIfAbsent(setting.key, () => setting.value);
    }
    return settingsToReturn;
  }

  @override
  LitchSettingsReadAuthority settingsReadAuthority(String deviceId) => settingsAuthority;

  @override
  LitchControlAvailability controlActionAvailability(String deviceId) {
    return availability;
  }

  @override
  Future<LitchControlEvent> dispatchControlAction({
    required String deviceId,
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) async {
    final int requestId = _nextRequestId++;
    controlCalls.add(
      _ControlCall(
        requestId: requestId,
        action: action,
        owner: owner,
        enabled: enabled,
        undoCount: undoCount,
      ),
    );
    final LitchControlEvent pending = LitchControlEvent(
      requestId: requestId,
      action: action,
      owner: owner,
      phase: LitchControlPhase.pending,
    );
    controlController.add(pending);
    return pending;
  }

  void emitControl({
    required _ControlCall call,
    required LitchControlPhase phase,
    LitchMutationReason? reason,
    LitchControlActionState actionState = LitchControlActionState.unknown,
  }) {
    controlController.add(
      LitchControlEvent(
        requestId: call.requestId,
        action: call.action,
        owner: call.owner,
        phase: phase,
        reason: reason,
        actionState: actionState,
      ),
    );
  }

  void emitHomeState({
    required int sequence,
    required String action,
    bool? undoPending,
    bool kingQueenSwapPromptActive = false,
    String occupancy = 'FFFF00000000FFFF',
    int? coachingMask,
    String? assistance,
    bool clockProjection = false,
    String fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
  }) {
    final String kingQueenField = kingQueenSwapPromptActive ? ',"kq":1' : '';
    final String undoField = undoPending == null
        ? ''
        : ',"up":${undoPending ? 1 : 0}';
    final String clockField = clockProjection
        ? ',"cl":[0,0,0,"w",0,0,0,"y"]'
        : '';
    final String coachingField = coachingMask == null
        ? ''
        : ',"co":$coachingMask';
    final String assistanceField = assistance == null
        ? ''
        : ',"as":"$assistance"';
    bufferedConsoleLines = <String>[
      '{"event":"home_state","v":1,"seq":$sequence,'
          '"a":"$action",'
          '"fen":"$fen",'
          '"occ":"$occupancy","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"pm":0,"white":1$kingQueenField$undoField$clockField$coachingField$assistanceField}',
    ];
    retainedHomeState = HomeStateNotification.tryParse(
      bufferedConsoleLines.single,
    );
    if (retainedHomeState case final HomeStateNotification state) {
      homeStateController.add(state);
    }
    consoleController.add(bufferedConsoleLines);
  }

  @override
  Future<bool> writeSetting({
    required String deviceId,
    required String key,
    required Object value,
    bool awaitAck = false,
  }) async {
    settingWrites.add(
      _SettingWrite(key: key, value: value, awaitAck: awaitAck),
    );
    final bool result = settingWriteCompleter == null
        ? settingWriteResult
        : await settingWriteCompleter!.future;
    if (result) {
      boardValues[key] = value;
    }
    return result;
  }

  @override
  Future<LitchSettingWriteResult> writeSettingAcknowledged({
    required String deviceId,
    required String key,
    required Object value,
  }) async {
    settingWrites.add(
      _SettingWrite(key: key, value: value, awaitAck: true),
    );
    if (acknowledgedSettingWriteCompleter case final completion?) {
      return completion.future;
    }
    final LitchSettingWriteResult? configured = acknowledgedSettingWriteResult;
    if (configured != null) {
      if (configured is LitchSettingWriteAcknowledged &&
          configured.outcome.ok &&
          configured.outcome.hasValue &&
          configured.outcome.value != null) {
        boardValues[key] = configured.outcome.value!;
      }
      return configured;
    }
    final bool result = settingWriteCompleter == null
        ? settingWriteResult
        : await settingWriteCompleter!.future;
    if (result) {
      boardValues[key] = value;
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
      );
    }
    return const LitchSettingWriteNotAcknowledged(
      LitchSettingWriteFailure.timedOut,
    );
  }

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    final waiting = deferredValueReads[key];
    if (waiting != null && waiting.isNotEmpty) {
      return waiting.removeAt(0).future;
    }
    return boardValues[key];
  }

  @override
  Object? latestSettingValue(String key) =>
      key == 'VS_CPU_LEVEL' || key == 'VS_CPU_PLAYER'
      ? boardValues[key]
      : super.latestSettingValue(key);

  @override
  Future<LitchEndPalette?> readPhysicalEndPalette({required String deviceId}) {
    if (deferredEndPalette != null) return deferredEndPalette!.future;
    return endPaletteToReturn != null
        ? Future<LitchEndPalette?>.value(endPaletteToReturn)
        : super.readPhysicalEndPalette(deviceId: deviceId);
  }

  @override
  Future<void> dispose() async {
    await controlController.close();
    await connectionController.close();
    await consoleController.close();
    await homeStateController.close();
    await super.dispose();
  }
}

const List<LitchSetting> _defaultSettings = <LitchSetting>[
  LitchSetting(
    key: 'BRIGHTNESS',
    type: 'u32',
    category: 'display',
    value: 5,
    defaultValue: 5,
    min: 1,
    max: 8,
    description: 'Board brightness step (1..8)',
  ),
  LitchSetting(
    key: 'WHITE_SQUARES_ON',
    type: 'bool',
    category: 'display',
    value: true,
    defaultValue: true,
    description: 'Enable white-square LEDs',
  ),
  LitchSetting(
    key: 'BLE_CONNECTION_INDICATOR',
    type: 'bool',
    category: 'display',
    value: true,
    defaultValue: true,
    description: 'Show the USB-side Bluetooth connection light',
  ),
  LitchSetting(
    key: 'P1_PIECE_WHEEL',
    type: 'u32',
    category: 'display',
    value: 170,
    defaultValue: 170,
    min: 0,
    max: 255,
    description: 'Player 1 piece color wheel',
  ),
  LitchSetting(
    key: 'P2_PIECE_WHEEL',
    type: 'u32',
    category: 'display',
    value: 0,
    defaultValue: 0,
    min: 0,
    max: 255,
    description: 'Player 2 piece color wheel',
  ),
  LitchSetting(
    key: 'MOVE_CONFIRM',
    type: 'enum',
    category: 'behavior',
    value: 'settle_2s',
    defaultValue: 'settle_2s',
    options: <String>['settle_2s', 'settle_5s', 'press_circle_to_confirm'],
    description: 'Move confirmation mode',
  ),
  LitchSetting(
    key: 'VS_CPU_LEVEL',
    type: 'u32',
    category: 'behavior',
    value: 0,
    defaultValue: 0,
    min: 0,
    max: 11,
    description: 'CPU difficulty (0=off, 1..11)',
  ),
  LitchSetting(
    key: 'VS_CPU_PLAYER',
    type: 'u32',
    category: 'behavior',
    value: 2,
    defaultValue: 2,
    min: 1,
    max: 2,
    description: 'Logical player controlled by CPU (1=P1, 2=P2)',
  ),
  LitchSetting(
    key: 'BOARD_ROTATE',
    type: 'bool',
    category: 'behavior',
    value: false,
    defaultValue: false,
    description: 'Rotate all board input and output 180 degrees',
  ),
  LitchSetting(
    key: 'GAME_FIRST_PLAYER',
    type: 'u32',
    category: 'behavior',
    value: 1,
    defaultValue: 1,
    min: 1,
    max: 2,
    description: 'Physical end occupied by P1 (1=normal, 2=opposite)',
  ),
  LitchSetting(
    key: 'TAKEBACK_GRACE_SECONDS',
    type: 'u32',
    category: 'behavior',
    value: 3,
    defaultValue: 0,
    min: 0,
    max: 10,
    description: 'Takeback grace period in seconds (0 disables)',
  ),
  LitchSetting(
    key: 'FUTURE_SWITCH_MODE',
    type: 'future_mode',
    category: 'experimental tools',
    value: 'calm',
    defaultValue: 'calm',
    description: 'A future experimental control',
  ),
  LitchSetting(
    key: 'RGBW_COOL_FUTURE',
    type: 'u32',
    category: 'hardware',
    value: 20,
    defaultValue: 20,
    min: 0,
    max: 255,
    description: 'Hidden color calibration',
  ),
  LitchSetting(
    key: 'USB_CC_TEST_VALUE',
    type: 'u32',
    category: 'hardware',
    value: 100,
    defaultValue: 100,
    min: 0,
    max: 1000,
    description: 'Hidden USB calibration',
  ),
  LitchSetting(
    key: 'BOARD_SETUP_ASSISTANCE',
    type: 'bool',
    category: 'behavior',
    value: true,
    defaultValue: true,
    description: 'Board setup assistance',
  ),
];

const List<LitchSetting> _lightSquareAppearanceSettings = <LitchSetting>[
  LitchSetting(
    key: 'LIGHT_SQUARE_MODE',
    type: 'enum',
    category: 'display',
    value: 'white',
    defaultValue: 'white',
    options: <String>['white', 'color'],
    description: 'Light-square color source',
  ),
  LitchSetting(
    key: 'LIGHT_SQUARE_HUE',
    type: 'u32',
    category: 'display',
    value: 0,
    defaultValue: 0,
    min: 0,
    max: 255,
    description: 'Light-square custom hue',
  ),
  LitchSetting(
    key: 'LIGHT_SQUARE_BRIGHTNESS',
    type: 'u32',
    category: 'display',
    value: 8,
    defaultValue: 8,
    min: 1,
    max: 8,
    description: 'Light-square brightness',
  ),
];

LitchSetting _withValue(LitchSetting setting, Object value) {
  return LitchSetting(
    key: setting.key,
    type: setting.type,
    category: setting.category,
    value: value,
    defaultValue: setting.defaultValue,
    min: setting.min,
    max: setting.max,
    options: setting.options,
    description: setting.description,
  );
}

const LitchSetting _placementEffectSetting = LitchSetting(
  key: 'SETTLE_ANIM_STYLE',
  type: 'enum',
  category: 'animation',
  value: 'mini_orbit',
  defaultValue: 'mini_orbit',
  options: <String>[
    'landing_flash',
    'mini_orbit',
    'ripple',
    'none',
    'rainbow_orbit',
  ],
  description: 'Placement effect',
);

const _animationStyleSettings = <LitchSetting>[
  LitchSetting(
    key: 'TURN_ANIM_STYLE',
    type: 'enum',
    category: 'animation',
    value: 'active_end_only',
    defaultValue: 'active_end_pingpong_sides',
    options: [
      'active_end_only',
      'active_end_pingpong_sides',
      'both_ends',
      'side_middle_band',
      'away_from_active_end',
      'none',
    ],
  ),
  LitchSetting(
    key: 'CHECKMATE_ANIM_STYLE',
    type: 'enum',
    category: 'animation',
    value: 'fireworks',
    defaultValue: 'fire-lob',
    options: [
      'fireworks',
      'fire-lob',
      'ripple-bloom',
      'winner-sweep',
      'winner-constellation',
    ],
  ),
  LitchSetting(
    key: 'HINT_THINKING_STYLE',
    type: 'enum',
    category: 'display',
    value: 'foxie_sweep',
    defaultValue: 'player_clockwise',
    options: ['foxie_sweep', 'player_clockwise', 'none'],
  ),
  LitchSetting(
    key: 'MINIMAL_ANIMATIONS',
    type: 'bool',
    category: 'animation',
    value: false,
    defaultValue: false,
  ),
];

Finder _choiceSegment(Finder label) =>
    find.ancestor(of: label, matching: find.byType(TextButton)).first;

Rect _visibleChoiceContent(WidgetTester tester, Finder button) {
  final paragraph = tester.renderObject<RenderParagraph>(
    find.descendant(
      of: find.descendant(of: button, matching: find.byType(Text)),
      matching: find.byType(RichText),
    ),
  );
  final text = paragraph.text.toPlainText();
  // Soft-wrap selection boxes include invisible trailing spaces beyond the
  // line's painted bounds. Measure the visible words and icon instead.
  final boxes = RegExp(r'\S+')
      .allMatches(text)
      .expand(
        (word) => paragraph.getBoxesForSelection(
          TextSelection(baseOffset: word.start, extentOffset: word.end),
        ),
      )
      .toList();
  expect(boxes, isNotEmpty);
  final origin = paragraph.localToGlobal(Offset.zero);
  final paragraphBounds = Offset.zero & paragraph.size;
  var bounds = boxes.first.toRect().intersect(paragraphBounds).shift(origin);
  for (final box in boxes.skip(1)) {
    bounds = bounds.expandToInclude(
      box.toRect().intersect(paragraphBounds).shift(origin),
    );
  }
  for (final icon
      in find.descendant(of: button, matching: find.byType(Icon)).evaluate()) {
    bounds = bounds.expandToInclude(
      tester.getRect(find.byElementPredicate((element) => element == icon)),
    );
  }
  return bounds;
}

void main() {
  Widget harness(
    _FakeLitchBleService service, {
    TextScaler textScaler = TextScaler.noScaling,
    double bottomSystemInset = 0,
    bool reviewLitchTheme = false,
    TargetPlatform? targetPlatform,
    bool enableAppConsole = false,
    int healthRefreshEpoch = 0,
    int healthFocusEpoch = 0,
    Future<void> Function(bool enabled)? onConsoleChanged,
    LitchAppearanceMode appearanceMode = LitchAppearanceMode.system,
    Future<void> Function(LitchAppearanceMode mode)? onAppearanceChanged,
    VoidCallback? onOpenFirmware,
  }) {
    final EdgeInsets systemPadding = EdgeInsets.only(bottom: bottomSystemInset);
    return MaterialApp(
      theme: targetPlatform != null
          ? buildLitchTheme().copyWith(platform: targetPlatform)
          : reviewLitchTheme
          ? buildLitchTheme()
          : null,
      builder: (BuildContext context, Widget? child) => MediaQuery(
        data: MediaQuery.of(
          context,
        ).copyWith(padding: systemPadding, viewPadding: systemPadding),
        child: child!,
      ),
      home: Scaffold(
        body: MediaQuery(
          data: MediaQueryData(textScaler: textScaler),
          child: SettingsScreen(
            bleService: service,
            boardId: 'board-1',
            enableAppConsole: enableAppConsole,
            healthRefreshEpoch: healthRefreshEpoch,
            healthFocusEpoch: healthFocusEpoch,
            onEnableAppConsoleChanged:
                onConsoleChanged ?? (bool enabled) async {},
            appearanceMode: appearanceMode,
            onAppearanceModeChanged: onAppearanceChanged,
            onOpenFirmware: onOpenFirmware,
          ),
        ),
      ),
    );
  }

  Future<void> pumpSettings(
    WidgetTester tester,
    _FakeLitchBleService service, {
    TextScaler textScaler = TextScaler.noScaling,
  }) async {
    await tester.pumpWidget(harness(service, textScaler: textScaler));
    await tester.pumpAndSettle();
  }

  bool? coachingToggled(WidgetTester tester, Finder control) {
    final ui.Tristate value = tester
        .getSemantics(control)
        .getSemanticsData()
        .flagsCollection
        .isToggled;
    final toggled = value == ui.Tristate.isTrue
        ? true
        : value == ui.Tristate.isFalse
        ? false
        : null;
    final strings = litchStrings(tester.element(control));
    expect(
      find.descendant(
        of: control,
        matching: find.text(
          toggled == null
              ? strings.unknownLabel
              : toggled
              ? strings.onLabel
              : strings.offLabel,
        ),
      ),
      findsOneWidget,
    );
    return toggled;
  }

  Future<void> pumpBoardActions(
    WidgetTester tester,
    _FakeLitchBleService service, {
    TextScaler textScaler = TextScaler.noScaling,
  }) async {
    final BoardActionsController controller = BoardActionsController(
      bleService: service,
      boardId: 'board-1',
    );
    addTearDown(controller.dispose);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: MediaQuery(
            data: MediaQueryData(textScaler: textScaler),
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: <Widget>[HomeBoardActions(controller: controller)],
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
  }

  Future<void> openSetupPage(WidgetTester tester, String page) =>
      _openSettingsPage(tester, page);

  Future<void> scrollIntoView(WidgetTester tester, Finder target) async {
    // Controls now live on focused pages. Reach them through the real UI;
    // retain every original value, acknowledgement and gesture assertion.
    if (target.evaluate().isEmpty) {
      while (find
          .byKey(const ValueKey<String>('setup-back'))
          .evaluate()
          .isNotEmpty) {
        await tester.tap(find.byKey(const ValueKey<String>('setup-back')));
        await tester.pumpAndSettle();
      }
      for (final page in <String>[
        'lights',
        'behavior',
        'phone',
        'advanced',
        'developer',
        'search',
      ]) {
        final destination = page == 'search'
            ? find.byKey(const ValueKey<String>('advanced-search'))
            : find.byKey(ValueKey<String>('setup-open:$page'));
        await tester.ensureVisible(destination);
        await tester.pumpAndSettle();
        await tester.tap(destination);
        await tester.pumpAndSettle();
        if (target.evaluate().isNotEmpty) break;
        await tester.tap(find.byKey(const ValueKey<String>('setup-back')));
        await tester.pumpAndSettle();
      }
    }
    expect(target, findsOneWidget);
    final scrollables = find.ancestor(
      of: target,
      matching: find.byType(Scrollable),
    );
    if (scrollables.evaluate().isNotEmpty) {
      await tester.scrollUntilVisible(
        target,
        200,
        scrollable: scrollables.first,
      );
    } else {
      await tester.ensureVisible(target);
    }
    await tester.pumpAndSettle();
  }

  for (final orientation in <String>['portrait', 'left', 'right']) {
    testWidgets('Setup assistance and CPU end ownership in $orientation', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = orientation == 'portrait'
          ? const Size(430, 932)
          : const Size(932, 430);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      final side = orientation == 'left'
          ? DeviceLandscapeSide.left
          : DeviceLandscapeSide.right;
      for (final first in <int>[1, 2]) {
        for (final rotated in <bool>[false, true]) {
          final service = _FakeLitchBleService(
            settings: <LitchSetting>[
              for (final setting in _defaultSettings)
                _withValue(setting, switch (setting.key) {
                  'VS_CPU_LEVEL' => 1,
                  'VS_CPU_PLAYER' => 2,
                  'GAME_FIRST_PLAYER' => first,
                  'BOARD_ROTATE' => rotated,
                  _ => setting.value,
                }),
              for (final player in <int>[1, 2])
                for (final feature in <String>[
                  'LEGAL_MOVES',
                  'CHECK_CUE',
                  'AUTO_HINTS',
                ])
                  LitchSetting(
                    key: 'P${player}_$feature',
                    type: 'bool',
                    category: 'behavior',
                    value: true,
                    defaultValue: feature != 'AUTO_HINTS',
                  ),
            ],
          )..boardValues['VS_CPU_LEVEL'] = 1;
          addTearDown(service.dispose);
          await tester.pumpWidget(
            localizationHarness(
              locale: const Locale('en'),
              scale: 2,
              home: OrientationWorkspace(
                sideReader: () async => side,
                child: LitchMainScaffold(
                  bleService: service,
                  boardId: 'board-1',
                  boards: const [],
                  onDisconnect: () async {},
                  initialPageIndex: 2,
                  initialCraftModeResolved: true,
                  loadAppConsolePreference: () async => false,
                  deviceLandscapeSideReader: () async => side,
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final topIsPlayer1 = rotated != (first == 2);
          for (final opposite in <bool>[true, false]) {
            final control = find.byKey(
              ValueKey<String>(
                'setup-board-controls:${opposite ? 'opposite' : 'near'}',
              ),
            );
            final glyph = tester.widget<BoardControlsGlyph>(
              find.descendant(
                of: control,
                matching: find.byType(BoardControlsGlyph),
              ),
            );
            final player1 = opposite ? topIsPlayer1 : !topIsPlayer1;
            expect(
              glyph.optionalOAvailable,
              player1,
              reason: 'Only the logical CPU player loses optional O gestures.',
            );
            final row = find
                .ancestor(of: control, matching: find.byType(Row))
                .first;
            expect(
              find.descendant(
                of: row,
                matching: find.text(player1 ? 'Player 1' : 'CPU Player 2'),
              ),
              findsOneWidget,
            );
          }
          await openSetupPage(tester, 'behavior');
          expect(
            find.byKey(const ValueKey<String>('setup-assistance')),
            findsOneWidget,
          );
          for (final player in [1, 2]) {
            expect(
              find.byKey(ValueKey<String>('assistance:P${player}_AUTO_HINTS')),
              findsNothing,
            );
            for (final feature in ['LEGAL_MOVES', 'CHECK_CUE']) {
              final control = find.byKey(
                ValueKey<String>('assistance:P${player}_$feature'),
              );
              expect(
                tester.widget<SwitchListTile>(control).onChanged,
                isNotNull,
              );
              expect(
                find.ancestor(of: control, matching: find.byType(Card)),
                findsOneWidget,
              );
            }
          }
          service.emitSettingValue('VS_CPU_LEVEL', 0);
          await tester.pumpAndSettle();
          expect(service.boardValues['P1_AUTO_HINTS'], isTrue);
          expect(service.boardValues['P2_AUTO_HINTS'], isTrue);
          expect(service.settingWrites, isEmpty);
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
        }
      }
    });
  }

  for (final orientation in <String>['portrait', 'left', 'right']) {
    testWidgets(
      'Assistance UX evidence 200% $orientation',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final semantics = tester.ensureSemantics();
        try {
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = orientation == 'portrait'
              ? const Size(430, 932)
              : const Size(932, 430);
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.view.resetPhysicalSize);
          final platform = debugDefaultTargetPlatformOverride!;
          final side = orientation == 'left'
              ? DeviceLandscapeSide.left
              : DeviceLandscapeSide.right;
          final cpuOwner = orientation == 'portrait' ? 1 : 2;
          final service =
              _FakeLitchBleService(
                  settings: <LitchSetting>[
                    for (final setting in _defaultSettings)
                      _withValue(setting, switch (setting.key) {
                        'VS_CPU_LEVEL' => 1,
                        'VS_CPU_PLAYER' => cpuOwner,
                        _ => setting.value,
                      }),
                    for (final player in <int>[1, 2])
                      for (final feature in <String>[
                        'LEGAL_MOVES',
                        'CHECK_CUE',
                        'AUTO_HINTS',
                      ])
                        LitchSetting(
                          key: 'P${player}_$feature',
                          type: 'bool',
                          category: 'behavior',
                          value: true,
                          defaultValue: feature != 'AUTO_HINTS',
                        ),
                  ],
                )
                ..boardValues['VS_CPU_LEVEL'] = 1
                ..boardValues['VS_CPU_PLAYER'] = cpuOwner;
          addTearDown(service.dispose);
          await tester.pumpWidget(
            localizationHarness(
              locale: const Locale('en'),
              scale: 2,
              home: Theme(
                data: buildLitchDarkTheme().copyWith(platform: platform),
                child: OrientationWorkspace(
                  sideReader: () async => side,
                  child: LitchMainScaffold(
                    bleService: service,
                    boardId: 'board-1',
                    boards: const [],
                    onDisconnect: () async {},
                    initialPageIndex: 2,
                    initialCraftModeResolved: true,
                    loadAppConsolePreference: () async => false,
                    deviceLandscapeSideReader: () async => side,
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final cpuLabel = find.text('CPU Player $cpuOwner');
          expect(cpuLabel, findsOneWidget);
          await Scrollable.ensureVisible(
            tester.element(cpuLabel),
            alignment: 1,
          );
          await tester.pumpAndSettle();
          expect(
            tester.getRect(cpuLabel).center.dy,
            inInclusiveRange(0, tester.view.physicalSize.height),
          );
          await captureHealthReview(
            tester,
            'assistance-review-${platform.name}-$orientation-overview-2x',
          );
          await openSetupPage(tester, 'behavior');
          for (final player in <int>[1, 2]) {
            final heading = find.text(
              player == cpuOwner ? 'CPU Player $player' : 'Player $player',
            );
            await Scrollable.ensureVisible(
              tester.element(heading),
              alignment: .15,
            );
            await tester.pumpAndSettle();
            await captureHealthReview(
              tester,
              'assistance-review-${platform.name}-$orientation-player$player-start-2x',
            );
            for (final feature in <String>['LEGAL_MOVES', 'CHECK_CUE']) {
              final key = 'P${player}_$feature';
              final target = find.byKey(ValueKey<String>('assistance:$key'));
              await tester.ensureVisible(target);
              await tester.pumpAndSettle();
              final merged = find
                  .ancestor(of: target, matching: find.byType(MergeSemantics))
                  .first;
              final node = tester.getSemantics(merged);
              final data = node.getSemanticsData();
              expect(data.flagsCollection.isEnabled, ui.Tristate.isTrue);
              expect(data.flagsCollection.isToggled, ui.Tristate.isTrue);
              expect(data.hasAction(ui.SemanticsAction.tap), isTrue);
              expect(
                data.label,
                contains(
                  player == cpuOwner ? 'CPU Player $player' : 'Player $player',
                ),
              );
              final featureLabel = feature == 'CHECK_CUE'
                  ? 'Check cue'
                  : 'Legal moves';
              expect(data.label, contains(featureLabel));
              expect(data.label, isNot(contains(key)));
              expect(tester.getSize(target).height, greaterThanOrEqualTo(48));
              expect(tester.getSize(target).width, greaterThanOrEqualTo(48));
              expect(node.rect.height, greaterThanOrEqualTo(48));
              expect(node.rect.width, greaterThanOrEqualTo(48));
              debugPrint(
                '[ASSISTANCE_SEMANTICS] ${platform.name}/$orientation/$key enabled=true toggled=true label=${data.label.replaceAll('\n', ' | ')} target=${node.rect.size}',
              );
            }
            await captureHealthReview(
              tester,
              'assistance-review-${platform.name}-$orientation-player$player-2x',
            );
          }
          service.emitSettingValue('VS_CPU_LEVEL', 0);
          await tester.pumpAndSettle();
          for (final player in [1, 2]) {
            expect(
              find.byKey(ValueKey<String>('assistance:P${player}_AUTO_HINTS')),
              findsNothing,
            );
            expect(service.boardValues['P${player}_AUTO_HINTS'], isTrue);
          }
          expect(service.settingWrites, isEmpty);
          expect(tester.takeException(), isNull);
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

  testWidgets(
    'Setup CPU labels follow confirmed roles through a held drag and rejected edit',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final pending = Completer<LitchSettingWriteResult>();
      final previewWrite = Completer<bool>();
      final service = _FakeLitchBleService()
        ..acknowledgedSettingWriteCompleter = pending
        ..settingWriteCompleter = previewWrite;
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final slider = find.byKey(
        const ValueKey<String>('setting-editor:VS_CPU_LEVEL'),
      );
      await scrollIntoView(tester, slider);
      final track = tester.getRect(slider);
      final pointer = await tester.startGesture(
        Offset(track.left + 24, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      for (final fraction in <double>[.35, .65, .8]) {
        await pointer.moveTo(
          Offset(track.left + track.width * fraction, track.center.dy),
        );
        service.emitSettingValue('WHITE_SQUARES_ON', false);
        await tester.pump(const Duration(milliseconds: 40));
        expect(find.text('CPU Player 1'), findsNothing);
        expect(find.text('CPU Player 2'), findsNothing);
      }
      await pointer.up();
      previewWrite.complete(false);
      await tester.pump(const Duration(milliseconds: 500));
      expect(
        service.settingWrites.where((write) => write.key == 'VS_CPU_LEVEL'),
        isNotEmpty,
      );
      expect(
        find.text('CPU Player 2'),
        findsNothing,
        reason: 'An unconfirmed slider intent is not CPU ownership.',
      );
      pending.complete(
        const LitchSettingWriteNotAcknowledged(
          LitchSettingWriteFailure.interrupted,
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('CPU Player 2'), findsNothing);
      service.emitSettingValue('VS_CPU_LEVEL', 1);
      await tester.pumpAndSettle();
      expect(find.text('CPU Player 2'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Setup search omits Automatic Hints already controlled by Coaching',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _FakeLitchBleService(
        settings: <LitchSetting>[
          for (final setting in _defaultSettings)
            setting.key == 'VS_CPU_LEVEL' ? _withValue(setting, 1) : setting,
          for (final player in <int>[1, 2])
            LitchSetting(
              key: 'P${player}_AUTO_HINTS',
              type: 'bool',
              category: 'behavior',
              value: true,
              defaultValue: false,
            ),
        ],
      )..boardValues['VS_CPU_LEVEL'] = 1;
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await openSetupPage(tester, 'search');
      await tester.enterText(
        find.byKey(const ValueKey<String>('advanced-search')),
        'Automatic hints',
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('setting-editor:P2_AUTO_HINTS')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey<String>('setting-editor:P1_AUTO_HINTS')),
        findsNothing,
      );
      expect(find.text('CPU Player 2: Automatic hints'), findsNothing);
      expect(find.text('P2_AUTO_HINTS'), findsNothing);
      expect(service.boardValues['P1_AUTO_HINTS'], isTrue);
      expect(service.boardValues['P2_AUTO_HINTS'], isTrue);
      expect(service.settingWrites, isEmpty);
    },
  );

  testWidgets(
    'older firmware keeps Game behavior usable without assistance defaults',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await openSetupPage(tester, 'behavior');
      expect(
        find.byKey(const ValueKey<String>('setting-editor:MOVE_CONFIRM')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('assistance:P1_AUTO_HINTS')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey<String>('assistance:P2_AUTO_HINTS')),
        findsNothing,
      );
      expect(service.settingWrites, isEmpty);
    },
  );

  for (final scale in <double>[1, 2]) {
    testWidgets(
      'Hint thinking pills put None first and restores rejected edits at $scale',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        await tester.binding.setSurfaceSize(const Size(320, 780));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final service = _FakeLitchBleService(
          settings: [..._defaultSettings, ..._animationStyleSettings],
        );
        addTearDown(service.dispose);
        await pumpSettings(
          tester,
          service,
          textScaler: TextScaler.linear(scale),
        );
        await openSetupPage(tester, 'lights');
        final editor = find.byKey(
          const ValueKey('setting-editor:HINT_THINKING_STYLE'),
        );
        await scrollIntoView(tester, editor);
        Finder field() => find.descendant(
          of: editor,
          matching: find.byType(SettingsChoicePills<String>),
          matchRoot: true,
        );
        expect(field(), findsOneWidget);
        expect(tester.getSize(field()).height, greaterThanOrEqualTo(48));
        expect(
          find.descendant(
            of: editor,
            matching: find.byType(RadioGroup<String>),
          ),
          findsNothing,
        );
        expect(
          tester.widget<SettingsChoicePills<String>>(field()).selected.single,
          'foxie_sweep',
        );
        Future<void> openMenu() async {
          await tester.ensureVisible(editor);
          await tester.pumpAndSettle();
        }

        await openMenu();
        final menu = tester.widget<SettingsChoicePills<String>>(field());
        expect(menu.segments.map((item) => item.value), [
          'none',
          'player_clockwise',
          'foxie_sweep',
        ]);
        for (final label in [
          'None',
          'Clockwise in player color',
          'Litch Sweep',
        ]) {
          final text = find.text(label).last;
          await tester.ensureVisible(text);
          await tester.pumpAndSettle();
          expectReviewTextFits(
            tester,
            text,
            'Hint choices must stay readable at 200% on a narrow phone.',
          );
          final item = find
              .ancestor(of: text, matching: find.byType(TextButton))
              .last;
          expect(tester.getSize(item).height, greaterThanOrEqualTo(48));
        }
        service.settingWriteCompleter = Completer<bool>();
        await tester.tap(_choiceSegment(find.text('None').last));
        await tester.pump();
        expect(service.settingWrites.single.key, 'HINT_THINKING_STYLE');
        expect(service.settingWrites.single.value, 'none');
        expect(
          tester
              .widget<SettingsChoicePills<String>>(field())
              .onSelectionChanged,
          isNull,
        );
        final semantics = tester.ensureSemantics();
        try {
          await tester.pump();
          final pending = tester
              .getSemantics(
                find
                    .ancestor(
                      of: _choiceSegment(find.text('None').last),
                      matching: find.byType(MergeSemantics),
                    )
                    .first,
              )
              .getSemanticsData();
          expect(pending.flagsCollection.isEnabled, ui.Tristate.isFalse);
          expect(pending.hasAction(ui.SemanticsAction.tap), isFalse);
        } finally {
          semantics.dispose();
        }
        await tester.tap(_choiceSegment(find.text('Litch Sweep').last));
        expect(service.settingWrites, hasLength(1));
        service.settingWriteCompleter!.complete(false);
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field()).selected.single,
          'foxie_sweep',
        );
        await tester.pump(const Duration(seconds: 5));
        service.settingWriteCompleter = null;
        await openMenu();
        Focus.of(tester.element(find.text('None').last)).requestFocus();
        await tester.pump();
        await tester.sendKeyEvent(LogicalKeyboardKey.space);
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field()).selected.single,
          'none',
        );
        expect(service.settingWrites.last.awaitAck, isTrue);
        await openMenu();
        await tester.tap(
          _choiceSegment(find.text('Clockwise in player color').last),
        );
        await tester.pumpAndSettle();
        expect(service.settingWrites.last.value, 'player_clockwise');
        expectReviewTextFits(
          tester,
          find
              .descendant(
                of: editor,
                matching: find.text('Clockwise in player color'),
              )
              .first,
          'The complete selected Hint style must wrap.',
        );
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'Short Setup choices share one joined pill with four divided segments',
    (tester) async {
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          ..._animationStyleSettings.where(
            (setting) => setting.key != 'HINT_THINKING_STYLE',
          ),
          LitchSetting(
            key: 'HINT_THINKING_STYLE',
            type: 'enum',
            category: 'display',
            value: 'beachball',
            defaultValue: 'beachball',
            options: ['foxie_sweep', 'player_clockwise', 'none', 'beachball'],
          ),
          LitchSetting(
            key: 'SETTLE_ANIM_STYLE',
            type: 'enum',
            category: 'animation',
            value: 'mini_orbit',
            defaultValue: 'mini_orbit',
            options: ['landing_flash', 'mini_orbit', 'ripple', 'none'],
          ),
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await openSetupPage(tester, 'lights');
      for (final key in ['SETTLE_ANIM_STYLE', 'HINT_THINKING_STYLE']) {
        final group = find.byKey(ValueKey('setting-editor:$key'));
        await tester.ensureVisible(group);
        await tester.pumpAndSettle();
        expect(
          find.descendant(of: group, matching: find.byType(TextButton)),
          findsNWidgets(4),
          reason:
              'Four named alternatives should be visible choices, without opening a menu.',
        );
        final joined = find.descendant(
          of: group,
          matching: find.byType(SegmentedButton<String>),
        );
        expect(
          joined,
          findsOneWidget,
          reason: 'Choices share one outline with internal dividers.',
        );
        expect(
          find.descendant(of: group, matching: find.byType(ChoiceChip)),
          findsNothing,
        );
        final control = tester.widget<SegmentedButton<String>>(joined);
        expect(
          find.descendant(of: joined, matching: find.byIcon(Icons.check)),
          findsOneWidget,
        );
        expect(
          control.direction,
          key == 'SETTLE_ANIM_STYLE' ? Axis.horizontal : Axis.vertical,
        );
        final buttons = find.descendant(
          of: joined,
          matching: find.byType(TextButton),
        );
        final rectangles = buttons
            .evaluate()
            .map((element) => tester.getRect(find.byWidget(element.widget)))
            .toList();
        for (var index = 1; index < rectangles.length; index++) {
          if (control.direction == Axis.horizontal) {
            expect(
              rectangles[index].left,
              closeTo(rectangles[index - 1].right, 0.01),
              reason: 'Adjacent options must meet at their shared divider.',
            );
            expect(rectangles[index].top, rectangles.first.top);
          } else {
            expect(
              rectangles[index].top,
              closeTo(rectangles[index - 1].bottom, 0.01),
              reason: 'Stacked options keep the same shared dividers.',
            );
            expect(rectangles[index].left, rectangles.first.left);
          }
        }
      }
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Board lights centers every visible icon and label through rotation',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      var side = DeviceLandscapeSide.left;
      tester.view.physicalSize = const Size(393, 852);
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          ..._lightSquareAppearanceSettings,
          _placementEffectSetting,
          ..._animationStyleSettings.where(
            (setting) => setting.key != 'HINT_THINKING_STYLE',
          ),
          const LitchSetting(
            key: 'HINT_THINKING_STYLE',
            type: 'enum',
            category: 'display',
            value: 'beachball',
            defaultValue: 'beachball',
            options: ['foxie_sweep', 'player_clockwise', 'none', 'beachball'],
          ),
        ],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('en'),
          home: OrientationWorkspace(
            sideReader: () async => side,
            child: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialPageIndex: 2,
              initialCraftModeResolved: true,
              deviceLandscapeSideReader: () async => side,
              loadAppConsolePreference: () async => false,
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await openSetupPage(tester, 'lights');
      const groups = [
        'light-square-mode',
        'setting-editor:HINT_THINKING_STYLE',
      ];
      final observations = <(String, double, double)>[];
      Future<void> inspect(String phase) async {
        for (final pose in ['portrait', 'left', 'right']) {
          // A portrait frame between landscape sides exercises native pose
          // re-reading while retaining the production page and controls.
          tester.view.physicalSize = const Size(393, 852);
          await tester.pumpAndSettle();
          if (pose != 'portrait') {
            side = pose == 'left'
                ? DeviceLandscapeSide.left
                : DeviceLandscapeSide.right;
            tester.view.physicalSize = const Size(852, 393);
            await tester.pumpAndSettle();
          }
          for (final key in groups) {
            final group = find.byKey(ValueKey(key));
            await tester.ensureVisible(group);
            await tester.pumpAndSettle();
            final buttons = find.descendant(
              of: group,
              matching: find.byType(TextButton),
            );
            for (final element in buttons.evaluate()) {
              final button = find.byWidget(element.widget);
              final target = tester.getRect(button);
              final joined = find.descendant(
                of: group,
                matching: find.byWidgetPredicate(
                  (widget) => widget is SegmentedButton,
                ),
              );
              if (tester.widget<SegmentedButton>(joined).direction ==
                  Axis.vertical) {
                expect(target.left, closeTo(tester.getRect(group).left, .01));
                expect(target.right, closeTo(tester.getRect(group).right, .01));
              }
              expect(target.height, greaterThanOrEqualTo(48));
              expect(target.width, greaterThanOrEqualTo(48));
              observations.add((
                '$phase $pose $key',
                _visibleChoiceContent(tester, button).center.dx,
                target.center.dx,
              ));
            }
            await captureHealthReview(
              tester,
              'board-lights-centering-${debugDefaultTargetPlatformOverride!.name}-$phase-$pose-${key.replaceAll(':', '-')}',
            );
          }
        }
      }

      await inspect('normal');
      for (final key in groups) {
        final group = find.byKey(ValueKey(key));
        final buttons = tester.widgetList<TextButton>(
          find.descendant(of: group, matching: find.byType(TextButton)),
        );
        final alternative = buttons.firstWhere(
          (button) =>
              !button.statesController!.value.contains(WidgetState.selected),
        );
        final target = find.byWidget(alternative);
        await tester.ensureVisible(target);
        await tester.pumpAndSettle();
        await tester.tap(target);
        await tester.pumpAndSettle();
      }
      await inspect('selected');
      for (final (context, actual, expected) in observations) {
        expect(
          actual,
          closeTo(expected, .51),
          reason: '$context must center the visible icon and label together.',
        );
      }
      expect(tester.takeException(), isNull);
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  for (final config in <({double scale, Locale locale})>[
    (scale: 1, locale: Locale('en')),
    (scale: 2, locale: Locale('de')),
  ]) {
    testWidgets(
      'Cauldron Hint/CPU feedback selects with readable controls at ${config.scale}x ${config.locale}',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(393, 852);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final service = _FakeLitchBleService(
          settings: [
            ..._defaultSettings,
            ..._animationStyleSettings.where(
              (setting) => setting.key != 'HINT_THINKING_STYLE',
            ),
            LitchSetting(
              key: 'HINT_THINKING_STYLE',
              type: 'enum',
              category: 'display',
              value: 'cauldron_brewing',
              defaultValue: 'cauldron_brewing',
              options: [
                'foxie_sweep',
                'player_clockwise',
                'none',
                'beachball',
                'cauldron_brewing',
              ],
            ),
          ],
        );
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: config.locale,
            scale: config.scale,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await openSetupPage(tester, 'lights');
        final editor = find.byKey(
          const ValueKey('setting-editor:HINT_THINKING_STYLE'),
        );
        await tester.ensureVisible(editor);
        await tester.pumpAndSettle();
        final pills = find.descendant(
          of: editor,
          matching: find.byType(SettingsChoicePills<String>),
          matchRoot: true,
        );
        expect(
          tester.widget<SettingsChoicePills<String>>(pills).selected.single,
          'cauldron_brewing',
        );
        expect(
          tester
              .widget<SettingsChoicePills<String>>(pills)
              .segments
              .map((segment) => segment.value),
          [
            'none',
            'player_clockwise',
            'foxie_sweep',
            'beachball',
            'cauldron_brewing',
          ],
        );
        final strings = reviewStrings(config.locale);
        expect(strings.hintThinkingStyle, contains('CPU'));
        final title = find.text(strings.hintThinkingStyle);
        await tester.ensureVisible(title);
        expectReviewTextFits(
          tester,
          title,
          'The shared Hint/CPU thinking-style label remains complete.',
        );
        final cauldron = find.text(strings.hintThinkingCauldronBrewing).last;
        await tester.ensureVisible(cauldron);
        expectReviewTextFits(
          tester,
          cauldron,
          'Cauldron Brewing remains readable at the selected text scale.',
        );
        expect(
          tester
              .getSize(
                find
                    .ancestor(of: cauldron, matching: find.byType(TextButton))
                    .last,
              )
              .height,
          greaterThanOrEqualTo(48),
        );
        await tester.ensureVisible(
          find.text(strings.hintThinkingBeachball).last,
        );
        await tester.tap(find.text(strings.hintThinkingBeachball).last);
        await tester.pumpAndSettle();
        expect(service.settingWrites.last.value, 'beachball');
        await tester.ensureVisible(cauldron);
        await tester.tap(cauldron);
        await tester.pumpAndSettle();
        expect(service.settingWrites.last.value, 'cauldron_brewing');
        expect(service.settingWrites.last.awaitAck, isTrue);
        expect(
          tester.widget<SettingsChoicePills<String>>(pills).selected.single,
          'cauldron_brewing',
        );
        expect(tester.takeException(), isNull);
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );

    testWidgets(
      'Beachball Hint style selects with accessible localized controls at ${config.scale}x ${config.locale}',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(393, 852);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final service = _FakeLitchBleService(
          settings: [
            ..._defaultSettings,
            ..._animationStyleSettings.where(
              (setting) => setting.key != 'HINT_THINKING_STYLE',
            ),
            LitchSetting(
              key: 'HINT_THINKING_STYLE',
              type: 'enum',
              category: 'display',
              value: 'beachball',
              defaultValue: 'beachball',
              options: ['foxie_sweep', 'player_clockwise', 'none', 'beachball'],
            ),
          ],
        );
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: config.locale,
            scale: config.scale,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await openSetupPage(tester, 'lights');
        final editor = find.byKey(
          const ValueKey('setting-editor:HINT_THINKING_STYLE'),
        );
        await tester.ensureVisible(editor);
        await tester.pumpAndSettle();
        final field = find.descendant(
          of: editor,
          matching: find.byType(SettingsChoicePills<String>),
          matchRoot: true,
        );
        final selectedChip = find.descendant(
          of: editor,
          matching: find.byKey(const ValueKey('beachball')),
        );
        final strings = reviewStrings(config.locale);
        final name =
            'beachball-${debugDefaultTargetPlatformOverride!.name}-${config.scale.toInt()}x-${config.locale}';
        expect(
          tester.widget<SettingsChoicePills<String>>(field).selected.single,
          'beachball',
        );
        expect(tester.getSize(field).height, greaterThanOrEqualTo(48));
        await captureHealthReview(tester, '$name-before-selection');
        await tester.pumpAndSettle();
        expect(
          tester
              .widget<SettingsChoicePills<String>>(field)
              .segments
              .map((item) => item.value),
          ['none', 'player_clockwise', 'foxie_sweep', 'beachball'],
        );
        await tester.tap(find.text(strings.playerThinkingClockwise).last);
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field).selected.single,
          'player_clockwise',
        );
        expect(service.settingWrites.single.value, 'player_clockwise');
        await tester.pumpAndSettle();
        final choice = find.text(strings.hintThinkingBeachball).last;
        await tester.ensureVisible(choice);
        await tester.pumpAndSettle();
        expectReviewTextFits(
          tester,
          choice,
          'Beachball must remain readable in the visible choices.',
        );
        final menuItem = find
            .ancestor(of: choice, matching: find.byType(TextButton))
            .last;
        expect(tester.getSize(menuItem).height, greaterThanOrEqualTo(48));
        await captureHealthReview(tester, '$name-choices');
        service.settingWriteCompleter = Completer<bool>();
        await tester.tap(choice);
        await tester.pump();
        expect(service.settingWrites, hasLength(2));
        expect(service.settingWrites.last.key, 'HINT_THINKING_STYLE');
        expect(service.settingWrites.last.value, 'beachball');
        expect(service.settingWrites.last.awaitAck, isTrue);
        expect(
          tester.widget<SettingsChoicePills<String>>(field).onSelectionChanged,
          isNull,
        );
        service.settingWriteCompleter!.complete(true);
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field).selected.single,
          'beachball',
        );
        final semantics = tester.ensureSemantics();
        try {
          await tester.pump();
          final node = tester.getSemantics(selectedChip).getSemanticsData();
          expect(node.flagsCollection.isButton, isTrue);
          expect(node.label, contains(strings.hintThinkingBeachball));
        } finally {
          semantics.dispose();
        }
        expectReviewTextFits(
          tester,
          find.descendant(
            of: editor,
            matching: find.text(strings.hintThinkingBeachball),
          ),
          'The selected Beachball label must remain complete.',
        );
        service.emitSettingValue('HINT_THINKING_STYLE', 'none');
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field).selected.single,
          'none',
        );
        service.emitSettingValue('HINT_THINKING_STYLE', 'beachball');
        await tester.pumpAndSettle();
        expect(
          tester.widget<SettingsChoicePills<String>>(field).selected.single,
          'beachball',
        );
        await captureHealthReview(tester, '$name-selected');
        expect(tester.takeException(), isNull);
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  for (final config
      in <
        ({
          double scale,
          Locale locale,
          TextDirection direction,
          Brightness brightness,
        })
      >[
        (
          scale: 1,
          locale: Locale('en'),
          direction: TextDirection.ltr,
          brightness: Brightness.light,
        ),
        (
          scale: 2,
          locale: Locale('de'),
          direction: TextDirection.ltr,
          brightness: Brightness.light,
        ),
        (
          scale: 2,
          locale: litchPseudoLocale,
          direction: TextDirection.rtl,
          brightness: Brightness.light,
        ),
        (
          scale: 1,
          locale: Locale('en'),
          direction: TextDirection.ltr,
          brightness: Brightness.dark,
        ),
      ]) {
    testWidgets(
      'Setup joined choices fit and traverse ${config.locale} ${config.scale}x ${config.direction.name} ${config.brightness.name}',
      (tester) async {
        await prepareLocalizationReview(tester);
        final semantics = tester.ensureSemantics();
        try {
          final service = _FakeLitchBleService(
            settings: [
              ..._defaultSettings.map(
                (setting) => setting.key == 'VS_CPU_LEVEL'
                    ? _withValue(setting, 5)
                    : setting,
              ),
              ..._lightSquareAppearanceSettings,
              _placementEffectSetting,
              ..._animationStyleSettings.where(
                (setting) => setting.key != 'HINT_THINKING_STYLE',
              ),
              const LitchSetting(
                key: 'HINT_THINKING_STYLE',
                type: 'enum',
                category: 'display',
                value: 'beachball',
                defaultValue: 'beachball',
                options: [
                  'foxie_sweep',
                  'player_clockwise',
                  'none',
                  'beachball',
                ],
              ),
              const LitchSetting(
                key: 'PROMOTION_MODE',
                type: 'enum',
                category: 'behavior',
                value: 'queen',
                defaultValue: 'queen',
                options: ['queen', 'choose'],
              ),
              const LitchSetting(
                key: 'MOTION_WAKE_MODE',
                type: 'enum',
                category: 'hardware',
                value: 'motion',
                defaultValue: 'motion',
                options: ['motion', 'tap', 'motion_and_tap'],
              ),
              const LitchSetting(
                key: 'SLEEP_TIMEOUT',
                type: 'enum',
                category: 'hardware',
                value: '60m',
                defaultValue: '60m',
                options: ['5m', '30m', '60m', 'disabled'],
              ),
            ],
          );
          addTearDown(service.dispose);
          await tester.pumpWidget(
            localizationHarness(
              locale: config.locale,
              scale: config.scale,
              home: Theme(
                data: config.brightness == Brightness.dark
                    ? buildLitchDarkTheme()
                    : buildLitchTheme(),
                child: Directionality(
                  textDirection: config.direction,
                  child: LitchMainScaffold(
                    bleService: service,
                    boardId: 'board-1',
                    boards: const [],
                    onDisconnect: () async {},
                    initialCraftModeResolved: true,
                    initialPageIndex: 2,
                    onAppearanceModeChanged: (_) async {},
                    loadAppConsolePreference: () async => false,
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final name =
              'pills-${debugDefaultTargetPlatformOverride!.name}-${config.scale.toInt()}x-${config.locale}-${config.direction.name}-${config.brightness.name}';
          for (final page in [
            'overview',
            'lights',
            'behavior',
            'phone',
            'advanced',
          ]) {
            await openSetupPage(tester, page);
            final keys = switch (page) {
              'overview' => {'setting-editor:VS_CPU_PLAYER': 2},
              'lights' => {
                'light-square-mode': 2,
                'setting-editor:HINT_THINKING_STYLE': 4,
              },
              'behavior' => {
                'setting-editor:MOVE_CONFIRM': 3,
                'setting-editor:PROMOTION_MODE': 2,
              },
              'phone' => {'app-appearance-mode': 3},
              _ => {
                'setting-editor:MOTION_WAKE_MODE': 3,
                'setting-editor:SLEEP_TIMEOUT': 4,
              },
            };
            for (final entry in keys.entries) {
              final group = find.byKey(ValueKey(entry.key));
              await scrollIntoView(tester, group);
              final pills = find.descendant(
                of: group,
                matching: find.byType(TextButton),
              );
              expect(pills, findsNWidgets(entry.value));
              final rectangles = pills
                  .evaluate()
                  .map(
                    (element) => tester.getRect(find.byWidget(element.widget)),
                  )
                  .toList();
              final joined = find.descendant(
                of: group,
                matching: find.byWidgetPredicate(
                  (widget) => widget is SegmentedButton,
                ),
              );
              expect(joined, findsOneWidget);
              expect(
                find.descendant(of: joined, matching: find.byIcon(Icons.check)),
                findsOneWidget,
              );
              expect(
                MediaQuery.textScalerOf(tester.element(group)).scale(10),
                config.scale * 10,
                reason: '${entry.key} must exercise the requested text scale.',
              );
              // Short P1/P2 labels still fit at 200%; the long Hint labels
              // exercise stacking. Other groups must fit on their chosen axis.
              if (entry.key == 'setting-editor:VS_CPU_PLAYER') {
                expect(
                  tester.widget<SegmentedButton>(joined).direction,
                  Axis.horizontal,
                  reason: 'Short player choices retain the horizontal pill.',
                );
              } else if (config.scale > 1.3 &&
                  entry.key == 'setting-editor:HINT_THINKING_STYLE') {
                expect(
                  tester.widget<SegmentedButton>(joined).direction,
                  Axis.vertical,
                  reason: 'Long Hint choices stack at enlarged text sizes.',
                );
              }
              for (var index = 1; index < rectangles.length; index++) {
                final before = rectangles[index - 1];
                final current = rectangles[index];
                if (tester.widget<SegmentedButton>(joined).direction ==
                    Axis.vertical) {
                  expect(current.top, closeTo(before.bottom, .01));
                  expect(current.left, before.left);
                } else if (config.direction == TextDirection.rtl) {
                  expect(current.right, closeTo(before.left, .01));
                  expect(current.top, before.top);
                } else {
                  expect(current.left, closeTo(before.right, .01));
                  expect(current.top, before.top);
                }
              }
              for (final element in pills.evaluate().toList()) {
                final pill = find.byWidget(element.widget);
                await tester.ensureVisible(pill);
                await tester.pumpAndSettle();
                final rect = tester.getRect(pill);
                expect(
                  _visibleChoiceContent(tester, pill).center.dx,
                  closeTo(rect.center.dx, .51),
                  reason:
                      '${entry.key} ${tester.widget<Text>(find.descendant(of: pill, matching: find.byType(Text))).data} centers its visible icon and label.',
                );
                expect(rect.height, greaterThanOrEqualTo(48));
                expect(rect.width, greaterThanOrEqualTo(48));
                expect(rect.left, greaterThanOrEqualTo(0));
                expect(rect.right, lessThanOrEqualTo(393));
                final hitTarget = tester.renderObject<RenderBox>(pill);
                for (final point in [
                  Offset(rect.center.dx, rect.top + 1),
                  Offset(rect.center.dx, rect.bottom - 1),
                  Offset(rect.left + 1, rect.center.dy),
                  Offset(rect.right - 1, rect.center.dy),
                ]) {
                  expect(
                    tester
                        .hitTestOnBinding(point)
                        .path
                        .any((entry) => entry.target == hitTarget),
                    isTrue,
                    reason:
                        '${entry.key} must receive input across its actual target.',
                  );
                }
                final label = find.descendant(
                  of: pill,
                  matching: find.byType(Text),
                );
                expectReviewTextFits(
                  tester,
                  label,
                  '${entry.key} must retain the full visible label.',
                );
                {
                  final paragraph = tester.renderObject<RenderParagraph>(
                    find.descendant(of: label, matching: find.byType(RichText)),
                  );
                  final text = paragraph.text.toPlainText();
                  // Pseudo localization appends punctuation, which is not a
                  // word. Check every real word, including German at 200%.
                  final words = RegExp(
                    r'[\p{L}\p{N}]+',
                    unicode: true,
                  ).allMatches(text).toList();
                  expect(words, isNotEmpty);
                  for (final word in words) {
                    final boxes = paragraph.getBoxesForSelection(
                      TextSelection(
                        baseOffset: word.start,
                        extentOffset: word.end,
                      ),
                    );
                    expect(
                      boxes.map((box) => box.top).toSet(),
                      hasLength(1),
                      reason: '${word.group(0)} must not split within a word.',
                    );
                  }
                }
                final merged = find
                    .ancestor(of: pill, matching: find.byType(MergeSemantics))
                    .first;
                final data = tester.getSemantics(merged).getSemanticsData();
                expect(data.flagsCollection.isButton, isTrue);
                expect(data.flagsCollection.isEnabled, ui.Tristate.isTrue);
                expect(data.flagsCollection.isInMutuallyExclusiveGroup, isTrue);
                expect(data.label, tester.widget<Text>(label).data);
                expect(data.textDirection, config.direction);
                expect(
                  Directionality.of(tester.element(label)),
                  config.direction,
                );
                expect(
                  data.flagsCollection.isSelected,
                  (element.widget as TextButton).statesController!.value
                          .contains(WidgetState.selected)
                      ? ui.Tristate.isTrue
                      : ui.Tristate.isFalse,
                );
              }
              if (entry.key == 'setting-editor:HINT_THINKING_STYLE') {
                final labels = tester
                    .widgetList<TextButton>(pills)
                    .map(
                      (button) => find
                          .text(
                            tester
                                .widget<Text>(
                                  find.descendant(
                                    of: find.byWidget(button),
                                    matching: find.byType(Text),
                                  ),
                                )
                                .data!,
                          )
                          .last,
                    )
                    .toList();
                await tester.ensureVisible(labels.first);
                Focus.of(tester.element(labels.first)).requestFocus();
                await tester.pumpAndSettle();
                expect(
                  Focus.of(tester.element(labels.first)).hasPrimaryFocus,
                  isTrue,
                );
                for (final label in labels.skip(1)) {
                  await tester.sendKeyEvent(LogicalKeyboardKey.tab);
                  await tester.pumpAndSettle();
                  expect(
                    Focus.of(tester.element(label)).hasPrimaryFocus,
                    isTrue,
                    reason:
                        'Tab must follow the visible choice order in ${config.direction.name}.',
                  );
                }
                FocusManager.instance.primaryFocus?.unfocus();
                await tester.pumpAndSettle();
              }
              final cards = find.ancestor(
                of: group,
                matching: find.byType(Card),
              );
              await tester.ensureVisible(
                cards.evaluate().isEmpty ? group : cards.first,
              );
              await tester.pumpAndSettle();
              await captureHealthReview(
                tester,
                '$name-${entry.key.replaceAll(':', '-')}',
              );
            }
            expect(tester.takeException(), isNull);
          }
          expect(service.settingWrites, isEmpty);
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

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'startup choice retains accessibility identity during delayed extension at $scale',
      (tester) async {
        await tester.binding.setSurfaceSize(const Size(393, 852));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final semantics = tester.ensureSemantics();
        try {
          final service = _FakeLitchBleService(
            settings: [
              ..._defaultSettings,
              const LitchSetting(
                key: 'BOOT_DISPLAY',
                type: 'enum',
                category: 'animation',
                value: 'fox',
                defaultValue: 'fox',
                options: ['fox', 'artwork', 'off'],
              ),
            ],
          );
          addTearDown(service.dispose);
          final row = File('../test/fixtures/settings_state_wire_v3.csv')
              .readAsLinesSync()
              .firstWhere((line) => line.startsWith('defaults,'));
          final hex = row.substring(row.indexOf(',') + 1);
          final payload = Uint8List.fromList([
            for (var i = 0; i < hex.length; i += 2)
              int.parse(hex.substring(i, i + 2), radix: 16),
          ]);
          void deliver(int code, int revision, Uint8List body) =>
              service.debugCaptureConnectionSessionState(
                ConnectionSessionPacket(
                  kind: ConnectionSessionKind.state,
                  code: code,
                  token: 1,
                  sequence: revision,
                  body: body,
                ),
              );
          void base(int revision) {
            ByteData.sublistView(payload).setUint32(0, revision, Endian.little);
            for (var i = 0; i < 9; i++) {
              deliver(
                SettingsStateWireCodec.fragmentCode(i),
                1000 + revision,
                Uint8List.sublistView(
                  payload,
                  i * 10,
                  i * 10 + SettingsStateWireCodec.fragmentLength(i),
                ),
              );
            }
          }

          void extension(int code, int revision, int value) {
            final body = Uint8List.fromList([1, 0, 0, 0, 0, value]);
            ByteData.sublistView(body).setUint32(1, revision, Endian.little);
            deliver(code, code * 1000 + revision, body);
          }

          final delayedPalette = Completer<LitchEndPalette?>();
          service.deferredEndPalette = delayedPalette;
          base(7);
          extension(43, 7, 0x0f);
          extension(44, 7, 0);
          await pumpSettings(
            tester,
            service,
            textScaler: TextScaler.linear(scale),
          );
          final card = find.byKey(const ValueKey('setting:BOOT_DISPLAY'));
          await scrollIntoView(tester, card);
          final editor = find.descendant(
            of: card,
            matching: find.byType(SettingsChoicePills<String>),
          );
          final originalElement = tester.element(editor);
          final originalRect = tester.getRect(card);
          final fox = find.descendant(
            of: card,
            matching: find.text('Foxie Fox'),
          );
          Finder button() =>
              find.ancestor(of: fox, matching: find.byType(TextButton)).first;
          final accessibilityId = tester.getSemantics(button()).id;
          base(8);
          await tester.pumpAndSettle();
          expect(
            editor,
            findsOneWidget,
            reason: 'A repair must not remove the focused startup control.',
          );
          expect(tester.element(editor), same(originalElement));
          expect(tester.getRect(card), originalRect);
          expect(tester.getSemantics(button()).id, accessibilityId);
          expect(tester.widget<SettingsChoicePills<String>>(editor).selected, {
            'fox',
          });
          expect(
            tester
                .widget<SettingsChoicePills<String>>(editor)
                .onSelectionChanged,
            isNull,
            reason:
                'Last confirmed selection is visible but unavailable until State44 matches.',
          );
          delayedPalette.complete(null);
          await tester.pumpAndSettle();
          expect(tester.element(editor), same(originalElement));
          expect(tester.getSemantics(button()).id, accessibilityId);
          expect(
            tester
                .widget<SettingsChoicePills<String>>(editor)
                .onSelectionChanged,
            isNull,
            reason:
                'Delayed metadata completion cannot restore unavailable State44 authority.',
          );
          extension(43, 8, 0x0f);
          extension(44, 8, 1);
          await tester.pumpAndSettle();
          expect(tester.element(editor), same(originalElement));
          expect(tester.getRect(card), originalRect);
          expect(tester.getSemantics(button()).id, accessibilityId);
          expect(tester.widget<SettingsChoicePills<String>>(editor).selected, {
            'artwork',
          });
          expect(
            tester
                .widget<SettingsChoicePills<String>>(editor)
                .onSelectionChanged,
            isNotNull,
          );
          expect(service.settingWrites, isEmpty);
          service.connectionController.add(
            ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
          await tester.pumpAndSettle();
          expect(
            card,
            findsNothing,
            reason:
                'Discovered extension controls cannot cross a retired connection.',
          );
          expect(tester.takeException(), isNull);
        } finally {
          semantics.dispose();
        }
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'startup choices use existing pills and recover rejected writes at $scale',
      (tester) async {
        await tester.binding.setSurfaceSize(const Size(393, 852));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final service = _FakeLitchBleService(
          settings: [
            ..._defaultSettings,
            const LitchSetting(
              key: 'BOOT_DISPLAY',
              type: 'enum',
              category: 'animation',
              value: 'fox',
              defaultValue: 'fox',
              options: ['fox', 'artwork', 'off'],
            ),
          ],
        );
        addTearDown(service.dispose);
        await pumpSettings(
          tester,
          service,
          textScaler: TextScaler.linear(scale),
        );
        final card = find.byKey(const ValueKey('setting:BOOT_DISPLAY'));
        await scrollIntoView(tester, card);
        final editor = find.descendant(
          of: card,
          matching: find.byType(SettingsChoicePills<String>),
        );
        expect(editor, findsOneWidget);
        expect(tester.widget<SettingsChoicePills<String>>(editor).selected, {
          'fox',
        });
        void expectTrailingFox() {
          final icon = find.byKey(const ValueKey('boot-display-foxie-fox'));
          final text = find.descendant(
            of: card,
            matching: find.text('Foxie Fox'),
          );
          expect(icon, findsOneWidget);
          final image = tester.widget<Image>(icon);
          expect(
            (image.image as AssetImage).assetName,
            'assets/branding/foxie-fox.png',
          );
          expect(image.excludeFromSemantics, true);
          expect(
            tester.getRect(icon).left,
            greaterThanOrEqualTo(tester.getRect(text).right),
          );
          expect(tester.getSize(icon), const Size(18, 18));
        }

        expectTrailingFox();
        for (final label in ['Foxie Fox', 'Stored artwork', 'Off']) {
          final option = find.descendant(of: card, matching: find.text(label));
          expect(option, findsOneWidget);
          final button = find
              .ancestor(of: option, matching: find.byType(TextButton))
              .first;
          expect(tester.getSize(button).height, greaterThanOrEqualTo(48));
        }
        final artwork = find.descendant(
          of: card,
          matching: find.text('Stored artwork'),
        );
        await tester.ensureVisible(artwork);
        await tester.pumpAndSettle();
        await tester.tap(artwork);
        await tester.pumpAndSettle();
        expect(service.settingWrites.single.key, 'BOOT_DISPLAY');
        expect(service.settingWrites.single.value, 'artwork');
        expect(service.settingWrites.single.awaitAck, true);
        expect(tester.widget<SettingsChoicePills<String>>(editor).selected, {
          'artwork',
        });
        expectTrailingFox();
        service.settingWriteResult = false;
        final off = find.descendant(of: card, matching: find.text('Off'));
        await tester.ensureVisible(off);
        await tester.pumpAndSettle();
        await tester.tap(off);
        await tester.pumpAndSettle();
        expect(service.settingWrites.last.value, 'off');
        expect(tester.widget<SettingsChoicePills<String>>(editor).selected, {
          'artwork',
        });
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'Focused Game behavior survives continuous rotation at ${scale}x',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final semantics = tester.ensureSemantics();
        try {
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetViewPadding);
          addTearDown(tester.view.resetPadding);
          var side = DeviceLandscapeSide.left;
          void geometry(bool portrait) {
            tester.view.physicalSize = portrait
                ? const Size(414, 896)
                : const Size(896, 414);
            final insets = portrait
                ? const FakeViewPadding(top: 44, bottom: 34)
                : const FakeViewPadding(left: 44, right: 44, bottom: 21);
            tester.view.viewPadding = insets;
            tester.view.padding = insets;
          }

          geometry(false);
          final service = _FakeLitchBleService(
            settings: [
              ..._defaultSettings,
              const LitchSetting(
                key: 'PROMOTION_MODE',
                type: 'enum',
                category: 'behavior',
                value: 'queen',
                defaultValue: 'queen',
                options: ['queen', 'choose'],
              ),
            ],
          );
          addTearDown(service.dispose);
          await tester.pumpWidget(
            localizationHarness(
              locale: const Locale('en'),
              scale: scale,
              home: Theme(
                data: buildLitchDarkTheme(),
                child: OrientationWorkspace(
                  sideReader: () async => side,
                  child: LitchMainScaffold(
                    bleService: service,
                    boardId: 'board-1',
                    boards: const [],
                    onDisconnect: () async {},
                    initialPageIndex: 2,
                    initialCraftModeResolved: true,
                    loadAppConsolePreference: () async => false,
                    deviceLandscapeSideReader: () async => side,
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          await openSetupPage(tester, 'behavior');
          final retained = tester.state(find.byType(SettingsScreen));
          final focusedLabel = find.descendant(
            of: find
                .descendant(
                  of: find.byKey(const ValueKey('setting-editor:MOVE_CONFIRM')),
                  matching: find.byType(TextButton),
                )
                .first,
            matching: find.byType(Text),
          );
          final retainedFocus = Focus.of(tester.element(focusedLabel));
          retainedFocus.requestFocus();
          await tester.pump();
          for (final portrait in [true, false, true, false, true]) {
            if (!portrait) {
              side = side == DeviceLandscapeSide.left
                  ? DeviceLandscapeSide.right
                  : DeviceLandscapeSide.left;
            }
            geometry(portrait);
            // Inspect the transition's frames, not only a settled replacement
            // widget: the reported phone failed on this same mounted submenu.
            for (final milliseconds in [0, 16, 32, 80, 160, 300]) {
              await tester.pump(Duration(milliseconds: milliseconds));
              expect(
                tester.takeException(),
                isNull,
                reason:
                    'Game behavior rotation portrait=$portrait scale=$scale',
              );
            }
            expect(tester.state(find.byType(SettingsScreen)), same(retained));
            expect(Focus.of(tester.element(focusedLabel)), same(retainedFocus));
            expect(retainedFocus.hasPrimaryFocus, isTrue);
            expect(find.text('Game behavior'), findsOneWidget);
            final cards = find
                .byType(Card)
                .evaluate()
                .map((element) => tester.getRect(find.byWidget(element.widget)))
                .toList();
            for (var index = 0; index < cards.length; index++) {
              expect(cards[index].height.isFinite, isTrue);
              if (index > 0) {
                expect(
                  cards[index].top,
                  greaterThanOrEqualTo(cards[index - 1].bottom),
                );
              }
            }
            for (final key in ['MOVE_CONFIRM', 'PROMOTION_MODE']) {
              final editor = find.byKey(ValueKey('setting-editor:$key'));
              await tester.ensureVisible(editor);
              await tester.pump();
              final buttons = find.descendant(
                of: editor,
                matching: find.byType(TextButton),
              );
              expect(buttons, findsNWidgets(key == 'MOVE_CONFIRM' ? 3 : 2));
              for (final button in buttons.evaluate()) {
                final target = find.byWidget(button.widget);
                expect(tester.getSize(target).height, greaterThanOrEqualTo(48));
                expect(tester.getSize(target).width, greaterThanOrEqualTo(48));
                final label = find.descendant(
                  of: target,
                  matching: find.byType(Text),
                );
                expectReviewTextFits(
                  tester,
                  label,
                  'A rotated choice retains its full label',
                );
                final merged = find
                    .ancestor(of: target, matching: find.byType(MergeSemantics))
                    .first;
                final data = tester.getSemantics(merged).getSemanticsData();
                expect(data.flagsCollection.isButton, isTrue);
                expect(data.flagsCollection.isEnabled, ui.Tristate.isTrue);
                expect(data.flagsCollection.isInMutuallyExclusiveGroup, isTrue);
                expect(data.label, tester.widget<Text>(label).data);
                expect(
                  data.flagsCollection.isSelected,
                  (button.widget as TextButton).statesController!.value
                          .contains(WidgetState.selected)
                      ? ui.Tristate.isTrue
                      : ui.Tristate.isFalse,
                );
              }
            }
            expect(find.text('Setup guidance'), findsOneWidget);
            expect(find.text('Takeback grace period'), findsOneWidget);
            await captureHealthReview(
              tester,
              'game-behavior-rotation-${debugDefaultTargetPlatformOverride!.name}-${scale.toInt()}x-${portrait ? 'portrait' : side.name}',
            );
          }
          expect(service.settingWrites, isEmpty);
          FocusManager.instance.primaryFocus?.unfocus();
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

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'Setup search exists only on the main menu at ${scale}x text',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(393, 852);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final locale = scale == 2 ? const Locale('de') : const Locale('en');
        final strings = reviewStrings(locale);
        final service = _FakeLitchBleService();
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: locale,
            scale: scale,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        final search = find.byKey(const ValueKey('advanced-search'));
        for (final page in [
          'lights',
          'behavior',
          'phone',
          'advanced',
          'developer',
        ]) {
          await _openSettingsPage(tester, page);
          final scroll = find
              .descendant(
                of: find.byKey(const ValueKey('settings-scroll')),
                matching: find.byType(Scrollable),
              )
              .first;
          final position = tester.state<ScrollableState>(scroll).position;
          position.jumpTo(position.maxScrollExtent);
          await tester.pumpAndSettle();
          expect(
            search,
            findsNothing,
            reason: '$page must not contain search.',
          );
          expect(find.text(strings.searchSettings), findsNothing);
          expect(
            find.byWidgetPredicate(
              (widget) =>
                  widget is InputDecorator &&
                  widget.decoration.prefixIcon is Icon &&
                  (widget.decoration.prefixIcon! as Icon).icon == Icons.search,
            ),
            findsNothing,
            reason: '$page has no search field or sizing placeholder.',
          );
          await tester.binding.handlePopRoute();
          await tester.pumpAndSettle();
          expect(find.byKey(const ValueKey('setup-back')), findsNothing);
          await _revealSettingsSearch(tester);
          expect(search.hitTestable(), findsOneWidget);
          expect(find.text(strings.searchSettings), findsOneWidget);
          expect(
            tester.getTopLeft(search).dy,
            greaterThan(
              tester
                  .getBottomLeft(
                    find.byKey(const ValueKey('open-board-software')),
                  )
                  .dy,
            ),
          );
        }
        expect(service.settingsReadDeviceIds, ['board-1']);
        expect(service.settingWrites, isEmpty);
        expect(tester.takeException(), isNull);
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets(
    'Settings overview places colors around a larger truthful preview without duplicate orientation',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(393, 852));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      expect(find.text('Setup'), findsNothing);
      final preview = find.byKey(const ValueKey('setup-board-preview'));
      final board = find.descendant(
        of: preview,
        matching: find.byType(HomeChessboard),
      );
      expect(tester.getSize(board).width, closeTo(280, 1));
      for (final rotated in [false, true]) {
        service.emitSettingValue('BOARD_ROTATE', rotated);
        await tester.pumpAndSettle();
        final top = find.byKey(
          ValueKey('quick:${rotated ? 'P1' : 'P2'}_PIECE_WHEEL'),
        );
        final bottom = find.byKey(
          ValueKey('quick:${rotated ? 'P2' : 'P1'}_PIECE_WHEEL'),
        );
        expect(
          tester.getBottomLeft(top).dy,
          lessThanOrEqualTo(tester.getTopLeft(preview).dy),
        );
        expect(
          tester.getTopLeft(bottom).dy,
          greaterThanOrEqualTo(tester.getBottomLeft(preview).dy),
        );
        expect(
          tester
              .getTopLeft(find.byKey(const ValueKey('quick:VS_CPU_LEVEL')))
              .dy,
          greaterThan(tester.getBottomLeft(bottom).dy),
        );
        await tester.ensureVisible(top);
        await tester.pumpAndSettle();
        await tester.tap(top);
        await tester.pumpAndSettle();
        final key = rotated ? 'P1_PIECE_WHEEL' : 'P2_PIECE_WHEEL';
        final choice = find.byKey(ValueKey('piece-color-choice:$key:85'));
        await tester.ensureVisible(choice);
        await tester.tap(choice);
        await tester.pumpAndSettle();
        expect(service.settingWrites.last.key, key);
      }
      expect(
        find.byKey(const ValueKey('setup-open:orientation')),
        findsNothing,
      );
      await _openSettingsPage(tester, 'search');
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const ValueKey('advanced-search')),
        'BOARD_ROTATE',
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('setting-editor:BOARD_ROTATE')),
        findsNothing,
      );
      expect(find.text('No settings match your search.'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Board lights groups appearance and separates placement from the next setting',
    (tester) async {
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          ..._lightSquareAppearanceSettings,
          _placementEffectSetting,
          ..._animationStyleSettings,
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await openSetupPage(tester, 'lights');
      final appearance = find.byKey(const ValueKey('light-square-appearance'));
      final toggle = find.byKey(const ValueKey('quick:WHITE_SQUARES_ON'));
      final card = find.ancestor(of: appearance, matching: find.byType(Card));
      expect(card, findsOneWidget);
      expect(find.descendant(of: card, matching: toggle), findsOneWidget);
      final placement = find.byKey(const ValueKey('quick:SETTLE_ANIM_STYLE'));
      final placementCard = find.ancestor(
        of: placement,
        matching: find.byType(Card),
      );
      expect(placementCard, findsOneWidget);
      final turn = find.byKey(const ValueKey('setting:TURN_ANIM_STYLE'));
      Finder surface(Finder card) =>
          find.descendant(of: card, matching: find.byType(Material)).first;
      expect(
        tester.getTopLeft(surface(placementCard)).dy -
            tester.getBottomLeft(surface(card)).dy,
        greaterThanOrEqualTo(8),
      );
      expect(
        tester.getTopLeft(surface(turn)).dy -
            tester.getBottomLeft(surface(placementCard)).dy,
        greaterThanOrEqualTo(8),
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Setup search is the final scrolling item and scrolls away with the list',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(393, 852);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final service = _FakeLitchBleService(
        settings: [..._defaultSettings, ..._animationStyleSettings],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('en'),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: () async {},
            initialCraftModeResolved: true,
            initialPageIndex: 2,
            loadAppConsolePreference: () async => false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      final search = find.byKey(const ValueKey('advanced-search'));
      final scroll = find
          .ancestor(of: search, matching: find.byType(Scrollable))
          .first;
      expect(
        find.ancestor(
          of: search,
          matching: find.byKey(const ValueKey('settings-scroll')),
        ),
        findsOneWidget,
        reason:
            'The reported field must belong to the Setup list, not its shell PageView.',
      );
      expect(search.hitTestable(), findsNothing);
      expect(find.byKey(const ValueKey('setup-search')), findsNothing);
      expect(find.byKey(const ValueKey('setup-search-footer')), findsNothing);
      final lastItem = find.byKey(const ValueKey('open-board-software'));
      await tester.ensureVisible(search);
      await tester.pumpAndSettle();
      expect(search.hitTestable(), findsOneWidget);
      expect(
        tester.getRect(search).top,
        greaterThan(tester.getRect(lastItem).bottom),
      );
      expect(tester.getSize(search).height, greaterThanOrEqualTo(48));
      await captureHealthReview(
        tester,
        'scrolling-search-overview-bottom-${debugDefaultTargetPlatformOverride!.name}',
      );
      final position = tester.state<ScrollableState>(scroll).position;
      final searchBottom = tester.getTopLeft(search).dy;
      await tester.drag(
        find.byKey(const ValueKey('settings-scroll')),
        const Offset(0, 180),
      );
      await tester.pumpAndSettle();
      expect(tester.getTopLeft(search).dy, greaterThan(searchBottom + 100));
      position.jumpTo(0);
      await tester.pumpAndSettle();
      expect(search.hitTestable(), findsNothing);
      await captureHealthReview(
        tester,
        'scrolling-search-overview-top-${debugDefaultTargetPlatformOverride!.name}',
      );
      expect(service.settingWrites, isEmpty);
      expect(service.settingsReadDeviceIds, ['board-1']);
      expect(tester.takeException(), isNull);
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'Scrolling Setup search retains focus, original reading position and ordinary Back',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(393, 852);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final service = _FakeLitchBleService(
        settings: [..._defaultSettings, ..._animationStyleSettings],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('en'),
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const [],
            onDisconnect: () async {},
            initialCraftModeResolved: true,
            initialPageIndex: 2,
            loadAppConsolePreference: () async => false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _openSettingsPage(tester, 'search');
      final search = find.byKey(const ValueKey('advanced-search'));
      final field = tester.widget<TextField>(search);
      final focus = field.focusNode!;
      final route = ModalRoute.of(tester.element(search));
      final scroll = find
          .ancestor(of: search, matching: find.byType(Scrollable))
          .first;
      final offset = tester.state<ScrollableState>(scroll).position.pixels;
      final semantics = tester.ensureSemantics();
      try {
        await tester.pump();
        final input = find.descendant(
          of: search,
          matching: find.byType(EditableText),
        );
        expect(tester.getSemantics(input).flagsCollection.isTextField, isTrue);
        expect(tester.getSemantics(input).label, contains('Search settings'));
        await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
        await tester.pumpAndSettle();
        expect(focus.hasFocus, isFalse);
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.pumpAndSettle();
        expect(focus.hasFocus, isTrue);
        await tester.enterText(search, 'FUTURE_SWITCH_MODE');
        await tester.pumpAndSettle();
        expect(
          focus.hasFocus,
          isTrue,
          reason: 'Filtering must not replace the finger-owned input.',
        );
        expect(find.text('A future experimental control'), findsOneWidget);
        expect(ModalRoute.of(tester.element(search)), same(route));
        await _revealSettingsSearch(tester);
        await _revealSettingsSearch(tester);
        await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
        await tester.pumpAndSettle();
        expect(field.controller!.text, isEmpty);
        expect(focus.hasFocus, isTrue);
        expect(
          tester.state<ScrollableState>(scroll).position.pixels,
          closeTo(offset, .1),
        );
        final mainPosition = tester.state<ScrollableState>(scroll).position;
        await _openSettingsPage(tester, 'behavior');
        final retainedOffset = mainPosition.pixels;
        expect(search, findsNothing);
        expect(focus.hasFocus, isFalse);
        await tester.binding.handlePopRoute();
        await tester.pumpAndSettle();
        expect(find.byKey(const ValueKey('setup-back')), findsNothing);
        expect(
          find.byKey(const ValueKey('setup-common-controls')),
          findsOneWidget,
        );
        expect(field.controller!.text, isEmpty);
        expect(mainPosition.pixels, closeTo(retainedOffset, .1));
        await _openSettingsPage(tester, 'search');
        await tester.enterText(search, 'MOVE_CONFIRM');
        await tester.pumpAndSettle();
        await tester.testTextInput.receiveAction(TextInputAction.search);
        await tester.pumpAndSettle();
        expect(field.controller!.text, 'MOVE_CONFIRM');
        expect(focus.hasFocus, isFalse);
        expect(
          find.byKey(const ValueKey('setup-back')),
          findsNothing,
          reason: 'Main-menu search adds no route or Back stage.',
        );
        expect(service.settingsReadDeviceIds, ['board-1']);
        expect(service.settingWrites, isEmpty);
        expect(tester.takeException(), isNull);
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
    'Scrolling Setup search clears navigation and German 200% keyboard',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(852, 393);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetViewInsets);
      addTearDown(tester.view.resetViewPadding);
      addTearDown(tester.view.resetPadding);
      tester.view.viewPadding = const FakeViewPadding(
        left: 59,
        right: 21,
        bottom: 21,
      );
      tester.view.padding = tester.view.viewPadding;
      var protectedInsets = EdgeInsets.fromLTRB(
        59,
        21,
        debugDefaultTargetPlatformOverride == TargetPlatform.iOS ? 0 : 21,
        21,
      );
      final service = _FakeLitchBleService(
        settings: [..._defaultSettings, ..._animationStyleSettings],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('de'),
          scale: 2,
          home: OrientationWorkspace(
            sideReader: () async => DeviceLandscapeSide.right,
            child: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pumpAndSettle();
      final search = find.byKey(const ValueKey('advanced-search'));
      await _openSettingsPage(tester, 'search');
      final field = tester.widget<TextField>(search);
      void expectSearchFits() {
        final rect = tester.getRect(search);
        expect(rect.height, greaterThanOrEqualTo(48));
        expect(rect.left, greaterThanOrEqualTo(protectedInsets.left + 16));
        expect(
          rect.right,
          lessThanOrEqualTo(
            tester.view.physicalSize.width - protectedInsets.right - 16,
          ),
        );
        expect(
          rect.bottom,
          lessThanOrEqualTo(
            tester.view.physicalSize.height - tester.view.viewInsets.bottom,
          ),
        );
        expect(
          rect.top,
          greaterThanOrEqualTo(
            tester.getRect(find.byKey(const ValueKey('settings-scroll'))).top,
          ),
        );
        final label = find.descendant(
          of: search,
          matching: find.text(reviewStrings(const Locale('de')).searchSettings),
        );
        expect(
          tester.getRect(label).top,
          greaterThanOrEqualTo(
            tester.getRect(find.byKey(const ValueKey('settings-scroll'))).top,
          ),
          reason:
              'The floating label must remain inside the scrolling viewport.',
        );
        final landscapeNavigation = find.byKey(
          const ValueKey('main-landscape-navigation'),
        );
        if (landscapeNavigation.evaluate().isNotEmpty) {
          expect(
            tester
                .getRect(search)
                .overlaps(tester.getRect(landscapeNavigation)),
            isFalse,
          );
        }
      }

      expectSearchFits();
      expectReviewTextFits(
        tester,
        find.descendant(
          of: search,
          matching: find.text(reviewStrings(const Locale('de')).searchSettings),
        ),
        'The scrolling field label remains complete at 200% German text.',
      );
      await tester.enterText(search, 'HINT_THINKING_STYLE');
      tester.view.viewInsets = const FakeViewPadding(bottom: 160);
      tester.view.padding = const FakeViewPadding(left: 59, right: 21);
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pumpAndSettle();
      await _revealSettingsSearch(tester);
      expectSearchFits();
      expect(field.focusNode!.hasFocus, isTrue);
      expect(field.controller!.text, 'HINT_THINKING_STYLE');
      expect(tester.takeException(), isNull);
      await captureHealthReview(
        tester,
        'scrolling-search-landscape-keyboard-${debugDefaultTargetPlatformOverride!.name}',
      );
      await tester.testTextInput.receiveAction(TextInputAction.search);
      tester.view.viewInsets = FakeViewPadding.zero;
      tester.view.padding = tester.view.viewPadding;
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pumpAndSettle();
      await _revealSettingsSearch(tester);
      expectSearchFits();
      expect(field.focusNode!.hasFocus, isFalse);
      expect(field.controller!.text, 'HINT_THINKING_STYLE');
      final editor = find.byKey(
        const ValueKey('setting-editor:HINT_THINKING_STYLE'),
      );
      await tester.ensureVisible(editor);
      await tester.pumpAndSettle();
      final selectedOption = find.descendant(
        of: editor,
        matching: find.text(
          reviewStrings(const Locale('de')).foxieThinkingSweep,
        ),
      );
      await tester.ensureVisible(_choiceSegment(selectedOption));
      await tester.pumpAndSettle();
      expect(_choiceSegment(selectedOption).hitTestable(), findsOneWidget);
      expect(
        tester.getSize(_choiceSegment(selectedOption)).height,
        greaterThanOrEqualTo(48),
      );
      await captureHealthReview(
        tester,
        'scrolling-search-landscape-submitted-${debugDefaultTargetPlatformOverride!.name}',
      );
      await tester.tap(_choiceSegment(selectedOption));
      await tester.pumpAndSettle();
      expect(selectedOption, findsOneWidget);
      expect(service.settingWrites, isEmpty);
      await _openSettingsPage(tester, 'search');
      await tester.pumpAndSettle();
      tester.view.physicalSize = const Size(320, 780);
      tester.view.viewPadding = const FakeViewPadding(top: 44, bottom: 34);
      tester.view.padding = const FakeViewPadding(top: 44);
      protectedInsets = const EdgeInsets.fromLTRB(0, 44, 0, 34);
      tester.view.viewInsets = const FakeViewPadding(bottom: 260);
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pumpAndSettle();
      await _revealSettingsSearch(tester);
      expectSearchFits();
      expect(field.controller!.text, 'HINT_THINKING_STYLE');
      final semantics = tester.ensureSemantics();
      try {
        final input = find.descendant(
          of: search,
          matching: find.byType(EditableText),
        );
        expect(
          tester.getSemantics(input).label,
          contains(reviewStrings(const Locale('de')).searchSettings),
          reason: 'The narrow field retains its complete accessible label.',
        );
      } finally {
        semantics.dispose();
      }
      await captureHealthReview(
        tester,
        'scrolling-search-portrait-keyboard-${debugDefaultTargetPlatformOverride!.name}',
      );
      await _revealSettingsSearch(tester);
      await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
      await tester.pumpAndSettle();
      expect(field.controller!.text, isEmpty);
      await _revealSettingsSearch(tester);
      expect(search.hitTestable(), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'Scrolling search retains query and focus through keyboard, rotation and reconnect at 200% text',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(320, 780);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetViewInsets);
      final service = _FakeLitchBleService(
        settings: [..._defaultSettings, ..._animationStyleSettings],
      );
      final replacement = _FakeLitchBleService(
        settings: [..._defaultSettings, ..._animationStyleSettings],
      );
      addTearDown(service.dispose);
      addTearDown(replacement.dispose);
      Widget screen(_FakeLitchBleService ble) => localizationHarness(
        locale: const Locale('de'),
        scale: 2,
        home: Scaffold(
          body: SettingsScreen(
            bleService: ble,
            boardId: 'board-1',
            enableAppConsole: false,
            onEnableAppConsoleChanged: (_) async {},
          ),
        ),
      );
      await tester.pumpWidget(screen(service));
      await tester.pumpAndSettle();
      final preview = find.byKey(const ValueKey('setup-board-preview'));
      final board = find.descendant(
        of: preview,
        matching: find.byType(HomeChessboard),
      );
      expect(tester.getRect(board).left, greaterThanOrEqualTo(16));
      expect(tester.getRect(board).right, lessThanOrEqualTo(304));
      expect(tester.getSize(board).width, lessThanOrEqualTo(280));
      await _openSettingsPage(tester, 'search');
      await tester.pumpAndSettle();
      tester.view.viewInsets = const FakeViewPadding(bottom: 260);
      await tester.pumpAndSettle();
      final search = find.byKey(const ValueKey('advanced-search'));
      final field = tester.widget<TextField>(search);
      expect(tester.getBottomLeft(search).dy, lessThanOrEqualTo(520));
      await tester.enterText(search, 'HINT_THINKING_STYLE');
      await tester.pumpAndSettle();
      final query = field.controller!.text;
      tester.view.physicalSize = const Size(780, 320);
      tester.view.viewInsets = const FakeViewPadding(bottom: 100);
      await tester.pumpAndSettle();
      expect(field.focusNode!.hasFocus, isTrue);
      expect(field.controller!.text, query);
      await _revealSettingsSearch(tester);
      expect(tester.getBottomLeft(search).dy, lessThanOrEqualTo(220));
      expect(tester.takeException(), isNull);
      await tester.testTextInput.receiveAction(TextInputAction.search);
      tester.view.viewInsets = FakeViewPadding.zero;
      await tester.pumpAndSettle();
      expect(field.focusNode!.hasFocus, isFalse);
      final editor = find.byKey(
        const ValueKey('setting-editor:HINT_THINKING_STYLE'),
      );
      await tester.ensureVisible(editor);
      await tester.pumpAndSettle();
      expect(tester.getRect(editor).left, greaterThanOrEqualTo(16));
      expect(tester.getRect(editor).right, lessThanOrEqualTo(764));
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();
      expect(field.controller!.text, query);
      await _revealSettingsSearch(tester);
      expect(search.hitTestable(), findsOneWidget);
      await tester.pumpWidget(screen(replacement));
      await tester.pumpAndSettle();
      expect(search, findsOneWidget);
      expect(tester.widget<TextField>(search).controller!.text, isEmpty);
      expect(tester.widget<TextField>(search).focusNode!.hasFocus, isFalse);
      expect(service.settingWrites, isEmpty);
      expect(replacement.settingWrites, isEmpty);
      expect(tester.takeException(), isNull);
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets(
    'Setup polish mirrors the far controls and draws a vertical USB label beside its port',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final preview = find.byKey(const ValueKey('setup-board-preview'));
      final physicalUsb = find.descendant(
        of: preview,
        matching: find.text('USB'),
      );
      expect(
        tester.getRect(physicalUsb).height,
        greaterThan(tester.getRect(physicalUsb).width),
        reason: 'The visible USB text must actually turn sideways.',
      );
      final far = find.descendant(
        of: preview,
        matching: find.byKey(const ValueKey('setup-board-controls:opposite')),
      );
      final near = find.descendant(
        of: preview,
        matching: find.byKey(const ValueKey('setup-board-controls:near')),
      );
      expect(tester.widget<RotatedBox>(far).quarterTurns, 2);
      expect(tester.widget<RotatedBox>(near).quarterTurns, 0);
      final board = find.descendant(
        of: preview,
        matching: find.byType(HomeChessboard),
      );
      final usb = find.byKey(const ValueKey('setup-usb-label'));
      final port = find.byKey(const ValueKey('setup-usb-port'));
      void check({required bool left}) {
        expect(tester.widget<RotatedBox>(usb).quarterTurns, left ? 3 : 1);
        expect(
          tester.getSize(usb).height,
          greaterThan(tester.getSize(usb).width),
        );
        final boardRect = tester.getRect(board);
        final portRect = tester.getRect(port);
        expect(portRect.size, const Size(4, 10));
        expect(portRect.center.dy, closeTo(boardRect.center.dy, .1));
        final gap = left
            ? boardRect.left - portRect.right
            : portRect.left - boardRect.right;
        expect(gap, inInclusiveRange(0, 12));
        expect(
          left
              ? tester.getRect(usb).right < portRect.left
              : tester.getRect(usb).left > portRect.right,
          isTrue,
        );
      }

      check(left: true);
      service.emitSettingValue('BOARD_ROTATE', true);
      await tester.pumpAndSettle();
      check(left: false);
      expect(tester.widget<RotatedBox>(far).quarterTurns, 2);
      expect(tester.widget<RotatedBox>(near).quarterTurns, 0);
      expect(service.settingWrites, isEmpty);
      expect(tester.takeException(), isNull);
    },
  );

  for (final rtl in [false, true]) {
    for (final largeText in [false, true]) {
      testWidgets(
        'Setup polish iOS edge Back can cancel and complete with retained scroll rtl=$rtl largeText=$largeText',
        (tester) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          tester.view.physicalSize = const Size(414, 896);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final service = _FakeLitchBleService();
          addTearDown(service.dispose);
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey('settings-gesture-review'),
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(largeText ? 2 : 1)),
                  child: Directionality(
                    textDirection: rtl ? TextDirection.rtl : TextDirection.ltr,
                    child: child!,
                  ),
                ),
              ),
              home: LitchMainScaffold(
                bleService: service,
                boardId: 'board-1',
                boards: const [],
                onDisconnect: () async {},
                initialCraftModeResolved: true,
                initialPageIndex: 2,
                loadAppConsolePreference: () async => false,
              ),
            ),
          );
          await tester.pumpAndSettle();
          final overviewScroll = find
              .descendant(
                of: find.byKey(const ValueKey('settings-scroll')),
                matching: find.byType(Scrollable),
              )
              .first;
          final overviewPosition = tester
              .state<ScrollableState>(overviewScroll)
              .position;
          final destination = find.byKey(const ValueKey('setup-open:behavior'));
          await tester.ensureVisible(destination);
          await tester.pumpAndSettle();
          final overviewOffset = overviewPosition.pixels;
          expect(overviewOffset, greaterThan(0));
          await tester.tap(destination);
          await tester.pumpAndSettle();
          final takeback = find.byKey(
            const ValueKey('setting-editor:TAKEBACK_GRACE_SECONDS'),
          );
          await tester.ensureVisible(takeback);
          await tester.pumpAndSettle();
          final sourceScroll = find
              .ancestor(of: takeback, matching: find.byType(Scrollable))
              .first;
          final sourceOffset = tester
              .state<ScrollableState>(sourceScroll)
              .position
              .pixels;
          final sourceRoute = ModalRoute.of(tester.element(takeback));
          final search = find.byKey(const ValueKey('advanced-search'));
          expect(search, findsNothing);
          Future<void> captureGesture(String state) async {
            if (!largeText) return;
            await captureHealthReview(
              tester,
              'ios-scrolling-edge-${rtl ? 'rtl' : 'ltr'}-$state',
              boundaryFinder: find.byKey(
                const ValueKey('settings-gesture-review'),
              ),
            );
          }

          await captureGesture('open');
          final content =
              tester
                      .element(find.byKey(const ValueKey('settings-scroll')))
                      .findRenderObject()!
                  as RenderBox;
          final startX = content.localToGlobal(Offset.zero).dx;
          final leafScroll = find
              .descendant(
                of: find.byKey(const ValueKey('settings-scroll')),
                matching: find.byType(Scrollable),
              )
              .first;
          final leafPosition = tester
              .state<ScrollableState>(leafScroll)
              .position;
          final gestureOffset = leafPosition.pixels;
          final viewport = tester.getRect(
            find.byKey(const ValueKey('settings-scroll')),
          );
          final sign = rtl ? -1.0 : 1.0;
          final edge = Offset(
            rtl ? viewport.right - 1 : viewport.left + 1,
            viewport.center.dy,
          );
          final gesture = await tester.startGesture(edge);
          for (final fraction in [.12, .22, .32]) {
            await gesture.moveTo(
              edge + Offset(sign * viewport.width * fraction, 0),
            );
            await tester.pump(const Duration(milliseconds: 100));
          }
          expect(
            sign * (content.localToGlobal(Offset.zero).dx - startX),
            greaterThan(30),
            reason:
                'A real edge drag must move the active page, not be blocked by Setup.',
          );
          await captureGesture('held');
          service.emitSettingValue('BOARD_ROTATE', true);
          await tester.pump(const Duration(milliseconds: 100));
          await gesture.moveTo(edge + Offset(sign * viewport.width * .05, 0));
          await tester.pump(const Duration(milliseconds: 300));
          await gesture.up();
          await tester.pumpAndSettle();
          expect(
            find.byKey(const ValueKey('setting-editor:MOVE_CONFIRM')),
            findsOneWidget,
          );
          expect(content.localToGlobal(Offset.zero).dx, closeTo(startX, .1));
          expect(leafPosition.pixels, closeTo(gestureOffset, .1));
          expect(search, findsNothing);
          expect(ModalRoute.of(tester.element(takeback)), same(sourceRoute));
          await captureGesture('cancelled');
          final retainedOffset = sourceOffset;
          final completing = await tester.startGesture(edge);
          for (final fraction in [.2, .45, .75]) {
            await completing.moveTo(
              edge + Offset(sign * viewport.width * fraction, 0),
            );
            await tester.pump(const Duration(milliseconds: 100));
          }
          await completing.up();
          await tester.pumpAndSettle();
          expect(search, findsOneWidget);
          expect(tester.widget<TextField>(search).controller!.text, isEmpty);
          expect(tester.widget<TextField>(search).focusNode!.hasFocus, isFalse);
          await captureGesture('committed');
          expect(
            find.byKey(const ValueKey('setup-common-controls')),
            findsOneWidget,
          );
          expect(overviewPosition.pixels, closeTo(overviewOffset, .1));
          await tester.tap(destination);
          await tester.pumpAndSettle();
          final restored = tester
              .state<ScrollableState>(
                find
                    .descendant(
                      of: find.byKey(const ValueKey('settings-scroll')),
                      matching: find.byType(Scrollable),
                    )
                    .first,
              )
              .position;
          expect(restored.pixels, closeTo(retainedOffset, .1));
          expect(search, findsNothing);
          expect(service.settingsReadDeviceIds, ['board-1']);
          expect(service.settingWrites, isEmpty);
          expect(tester.takeException(), isNull);
        },
        variant: const TargetPlatformVariant({TargetPlatform.iOS}),
      );
    }
  }

  for (final layout in <({String name, Size size})>[
    (name: 'portrait', size: Size(390, 780)),
    (name: 'compact-landscape', size: Size(844, 390)),
    (name: 'rail', size: Size(800, 510)),
  ]) {
    testWidgets(
      'Setup navigation returns overview and retains page reading positions ${layout.name}',
      (tester) async {
        tester.view.physicalSize = layout.size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final service = _FakeLitchBleService();
        addTearDown(service.dispose);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        Finder navigation(String page) {
          final key = find.byKey(ValueKey('main-destination-$page'));
          if (key.evaluate().isNotEmpty) return key;
          final rail = find.byType(NavigationRail);
          final surface = rail.evaluate().isNotEmpty
              ? rail
              : find.byKey(const ValueKey('main-landscape-navigation'));
          return find
              .descendant(
                of: surface,
                matching: page == 'play'
                    ? find.byType(LitchPlayGlyph)
                    : find.byWidgetPredicate(
                        (widget) =>
                            widget is Icon &&
                            (widget.icon == Icons.tune_outlined ||
                                widget.icon == Icons.tune_rounded),
                      ),
              )
              .hitTestable()
              .first;
        }

        await tester.pumpWidget(
          localizationHarness(
            locale: const Locale('en'),
            scale: 2,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await openSetupPage(tester, 'behavior');
        final target = find.byKey(
          const ValueKey('setting-editor:TAKEBACK_GRACE_SECONDS'),
        );
        await tester.ensureVisible(target);
        await tester.pumpAndSettle();
        ScrollPosition activePosition() => tester
            .state<ScrollableState>(
              find
                  .descendant(
                    of: find.byKey(const ValueKey('settings-scroll')),
                    matching: find.byType(Scrollable),
                  )
                  .first,
            )
            .position;
        final offset = activePosition().pixels;
        expect(offset, greaterThan(0));
        service.emitSettingValue('TAKEBACK_GRACE_SECONDS', 5);
        await tester.pumpAndSettle();
        expect(activePosition().pixels, closeTo(offset, .1));
        await tester.tap(navigation('play'));
        await tester.pumpAndSettle();
        service.emitSettingValue('TAKEBACK_GRACE_SECONDS', 6);
        await tester.pumpAndSettle();
        await tester.tap(navigation('setup'));
        await tester.pumpAndSettle();
        final setupNavigator = tester.widget<Navigator>(
          find.descendant(
            of: find.byType(SettingsScreen),
            matching: find.byType(Navigator),
          ),
        );
        expect(
          setupNavigator.pages.map((page) => page.name),
          ['overview'],
          reason:
              'Setup navigation from Play must select the actual top-level page.',
        );
        expect(
          find.byKey(const ValueKey('setup-common-controls')),
          findsOneWidget,
        );
        expect(target, findsNothing);
        await openSetupPage(tester, 'behavior');
        expect(target, findsOneWidget);
        expect(tester.widget<Slider>(target).value, 6);
        expect(activePosition().pixels, closeTo(offset, .1));
        await _openSettingsPage(tester, 'search');
        await tester.pumpAndSettle();
        final search = find.byKey(const ValueKey('advanced-search'));
        final searchReadingOffset = activePosition().pixels;
        await tester.enterText(search, 'TAKEBACK_GRACE_SECONDS');
        await tester.pumpAndSettle();
        final searchFocus = tester.widget<TextField>(search).focusNode!;
        expect(searchFocus.hasFocus, isTrue);
        await tester.tap(navigation('play'));
        await tester.pumpAndSettle();
        expect(searchFocus.hasFocus, isFalse);
        await tester.tap(navigation('setup'));
        await tester.pumpAndSettle();
        expect(
          tester.widget<TextField>(search).controller!.text,
          'TAKEBACK_GRACE_SECONDS',
        );
        expect(searchFocus.hasFocus, isFalse);
        await _revealSettingsSearch(tester);
        await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('setup-common-controls')),
          findsOneWidget,
        );
        expect(tester.widget<TextField>(search).controller!.text, isEmpty);
        expect(activePosition().pixels, closeTo(searchReadingOffset, .1));
        await openSetupPage(tester, 'behavior');
        expect(target, findsOneWidget);
        expect(activePosition().pixels, closeTo(offset, .1));
        final capture =
            'setup-reselect-${layout.name}-${debugDefaultTargetPlatformOverride?.name}';
        await captureHealthReview(tester, '$capture-leaf');
        await tester.tap(navigation('setup'));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('setup-common-controls')),
          findsOneWidget,
          reason: 'Tapping the selected Setup tab returns from its submenu.',
        );
        expect(target, findsNothing);
        await captureHealthReview(tester, '$capture-overview');
        await _revealSettingsSearch(tester);
        await tester.enterText(search, 'TAKEBACK_GRACE_SECONDS');
        await tester.pumpAndSettle();
        final overviewOffset = activePosition().pixels;
        await tester.tap(navigation('setup'));
        await tester.pumpAndSettle();
        expect(
          tester.widget<TextField>(search).controller!.text,
          'TAKEBACK_GRACE_SECONDS',
          reason:
              'Reselecting an overview already being read preserves search.',
        );
        expect(activePosition().pixels, closeTo(overviewOffset, .1));
        expect(service.settingsReadDeviceIds, ['board-1']);
        expect(service.settingWrites, isEmpty);
        expect(tester.takeException(), isNull);
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.iOS,
        TargetPlatform.android,
      }),
    );
  }

  testWidgets(
    'At the board preview follows physical ends and rotation independently of player assignment',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final semantics = tester.ensureSemantics();

      await pumpSettings(tester, service);
      final preview = find.byKey(const ValueKey('setup-board-preview'));
      final ends = find.descendant(
        of: preview,
        matching: find.byType(BoardControlsGlyph),
      );
      List<Color?> colors() => tester
          .widgetList<BoardControlsGlyph>(ends)
          .map((x) => x.color)
          .toList();
      final originalColors = colors();
      expect(originalColors, hasLength(2));
      expect(originalColors.first, isNot(originalColors.last));
      final board = tester.widget<HomeChessboard>(
        find.descendant(of: preview, matching: find.byType(HomeChessboard)),
      );
      expect(board.snapshot.position, isNull);
      expect(board.liveGuidanceAvailable, isFalse);
      expect(board.onSquarePressed, isNull);
      expect(tester.getSemantics(preview).label, contains('Board preview'));
      service.emitSettingValue('GAME_FIRST_PLAYER', 2);
      await tester.pumpAndSettle();
      expect(colors(), originalColors);
      service.emitSettingValue('BOARD_ROTATE', true);
      await tester.pumpAndSettle();
      expect(colors(), originalColors.reversed.toList());
      expect(
        find.descendant(of: preview, matching: find.text('Rotate board: On')),
        findsOneWidget,
      );
      final other = _FakeLitchBleService(
        settings: _defaultSettings
            .where((s) => s.key != 'P2_PIECE_WHEEL')
            .toList(),
      );
      addTearDown(other.dispose);
      await tester.pumpWidget(harness(other));
      await tester.pumpAndSettle();
      expect(
        preview,
        findsNothing,
        reason: 'Unknown physical end colors must not be fabricated.',
      );
      expect(
        find.byKey(const ValueKey('setup-common-controls')),
        findsOneWidget,
      );
      expect(service.settingWrites, isEmpty);
      expect(other.settingWrites, isEmpty);
      semantics.dispose();
    },
  );

  testWidgets(
    'At the board holds opponent track through late health and shows attention after commit',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _FakeLitchBleService()
        ..runtimeHealth = RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00B000000')!,
        );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final slider = find.byKey(const ValueKey('setting-editor:VS_CPU_LEVEL'));
      await scrollIntoView(tester, slider);
      final track = tester.getRect(slider);
      final row = find.byKey(const ValueKey('quick:VS_CPU_LEVEL'));
      expect(
        track.width,
        closeTo(tester.getSize(row).width, .1),
        reason: 'Opponent slider spans all available Setup width',
      );
      expect(find.text('Play against another player'), findsOneWidget);
      expect(find.text('Another player'), findsNothing);
      final scrollable = find
          .ancestor(of: slider, matching: find.byType(Scrollable))
          .first;
      final position = tester.state<ScrollableState>(scrollable).position;
      final offset = position.pixels;
      final gesture = await tester.startGesture(
        Offset(track.left + 24, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      await gesture.moveTo(
        Offset(track.left + track.width * .45, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
      final heldValue = tester.widget<Slider>(slider).value;
      expect(find.textContaining('Play against CPU'), findsOneWidget);
      expect(find.textContaining('CPU'), findsWidgets);
      service.debugAppendConsolePacket(
        '{"event":"board_health","v":1,"h":"00AF20004","ps":"n","rr":0,"boot":2,"wd":0,"sd":0}\n'
            .codeUnits,
      );
      service.emitSettingValue('VS_CPU_LEVEL', 2);
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.widget<Slider>(slider).value, heldValue);
      expect(tester.getRect(slider), track);
      expect(position.pixels, offset);
      expect(find.byKey(const ValueKey('board-health-priority')), findsNothing);
      await gesture.moveTo(
        Offset(track.left + track.width * .8, track.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.widget<Slider>(slider).value, greaterThan(heldValue));
      expect(tester.getRect(slider), track);
      await gesture.up();
      await tester.pumpAndSettle();
      expect(find.text('Input paused'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('board-health-priority')),
        findsOneWidget,
      );
      expect(service.settingWrites.last.key, 'VS_CPU_LEVEL');
      expect(service.settingWrites.last.awaitAck, isTrue);
      await scrollIntoView(tester, slider);
      final promotedTrack = tester.getRect(slider);
      final recoveryGesture = await tester.startGesture(
        Offset(promotedTrack.left + 24, promotedTrack.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      await recoveryGesture.moveTo(
        Offset(
          promotedTrack.left + promotedTrack.width * .45,
          promotedTrack.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 40));
      final recoveryHeldValue = tester.widget<Slider>(slider).value;
      service.debugAppendConsolePacket(
        '{"event":"board_health","v":1,"h":"00B000000","ps":"n","rr":0,"boot":2,"wd":0,"sd":0}\n'
            .codeUnits,
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.widget<Slider>(slider).value, recoveryHeldValue);
      expect(tester.getRect(slider), promotedTrack);
      expect(
        find.byKey(const ValueKey('board-health-priority')),
        findsOneWidget,
      );
      await recoveryGesture.moveTo(
        Offset(
          promotedTrack.left + promotedTrack.width * .8,
          promotedTrack.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(
        tester.widget<Slider>(slider).value,
        greaterThan(recoveryHeldValue),
      );
      expect(tester.getRect(slider), promotedTrack);
      await recoveryGesture.up();
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('board-health-priority')), findsNothing);
      expect(find.text('Healthy'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'a hidden prewarmed Setup leaf does not intercept Play route Back',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final navigator = GlobalKey<NavigatorState>();
      await tester.pumpWidget(
        MaterialApp(navigatorKey: navigator, home: const Text('Root page')),
      );
      bool hidden = false;
      late StateSetter update;
      unawaited(
        navigator.currentState!.push(
          MaterialPageRoute<void>(
            builder: (_) => StatefulBuilder(
              builder: (context, setState) {
                update = setState;
                return Scaffold(
                  body: Stack(
                    children: [
                      TickerMode(
                        enabled: !hidden,
                        child: Offstage(
                          offstage: hidden,
                          child: SettingsScreen(
                            bleService: service,
                            boardId: 'board-1',
                            enableAppConsole: false,
                            onEnableAppConsoleChanged: (_) async {},
                          ),
                        ),
                      ),
                      if (hidden) const Text('Play page'),
                    ],
                  ),
                );
              },
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await openSetupPage(tester, 'behavior');
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('setup-common-controls')),
        findsOneWidget,
      );
      expect(find.text('Root page'), findsNothing);
      await openSetupPage(tester, 'behavior');
      await _openSettingsPage(tester, 'search');
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const ValueKey('advanced-search')),
        'MOVE_CONFIRM',
      );
      await tester.pumpAndSettle();
      update(() => hidden = true);
      await tester.pumpAndSettle();
      expect(find.text('Play page'), findsOneWidget);
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(find.text('Root page'), findsOneWidget);
      expect(find.text('Play page'), findsNothing);
      expect(service.settingWrites, isEmpty);
    },
  );

  for (final config in <({Size size, double scale, Locale locale})>[
    (size: Size(393, 852), scale: 1, locale: Locale('en')),
    (size: Size(393, 852), scale: 2, locale: Locale('de')),
    (size: Size(852, 393), scale: 2, locale: Locale('en')),
  ]) {
    testWidgets(
      'At the board whole shell ${config.size} ${config.locale} ${config.scale}x',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = config.size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final service = _FakeLitchBleService(
          settings: [
            ..._defaultSettings,
            ..._lightSquareAppearanceSettings,
            _placementEffectSetting,
            ..._animationStyleSettings,
          ],
        );
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: config.locale,
            scale: config.scale,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              initialPageIndex: 2,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        final name =
            'setup-A-${debugDefaultTargetPlatformOverride!.name}-${config.size.width.toInt()}-${config.scale.toInt()}x-${config.locale}';
        final usb = find.descendant(
          of: find.byKey(const ValueKey('setup-board-preview')),
          matching: find.text('USB'),
        );
        expectReviewTextFits(
          tester,
          usb,
          'The physical port label must remain complete at ${config.scale}x.',
        );
        final usbText = tester.renderObject<RenderParagraph>(
          find.descendant(of: usb, matching: find.byType(RichText)),
        );
        final usbPainter = TextPainter(
          text: usbText.text,
          textDirection: usbText.textDirection,
          textScaler: usbText.textScaler,
        )..layout();
        expect(usbText.size.height, closeTo(usbPainter.height, .1));
        usbPainter.dispose();
        await captureHealthReview(tester, '$name-overview');
        final previewBoard = find.descendant(
          of: find.byKey(const ValueKey('setup-board-preview')),
          matching: find.byType(HomeChessboard),
        );
        final navigation = find.byKey(
          const ValueKey('main-landscape-navigation'),
        );
        await tester.ensureVisible(previewBoard);
        await tester.pumpAndSettle();
        if (navigation.evaluate().isNotEmpty) {
          expect(
            tester.getRect(previewBoard).overlaps(tester.getRect(navigation)),
            isFalse,
            reason:
                'The enlarged board must fit above the floating navigation.',
          );
        }
        await captureHealthReview(tester, '$name-overview-board');
        final overviewScroll = find
            .ancestor(of: previewBoard, matching: find.byType(Scrollable))
            .first;
        tester.state<ScrollableState>(overviewScroll).position.jumpTo(0);
        await tester.pumpAndSettle();
        await _openSettingsPage(tester, 'search');
        await tester.pumpAndSettle();
        final search = find.byKey(const ValueKey('advanced-search'));
        await captureHealthReview(tester, '$name-search-open');
        await tester.enterText(search, 'HINT_THINKING_STYLE');
        await tester.pumpAndSettle();
        final resultEditor = find.byKey(
          const ValueKey('setting-editor:HINT_THINKING_STYLE'),
        );
        await tester.ensureVisible(resultEditor);
        await tester.pumpAndSettle();
        if (navigation.evaluate().isNotEmpty) {
          expect(
            tester.getRect(resultEditor).overlaps(tester.getRect(navigation)),
            isFalse,
          );
        }
        await captureHealthReview(tester, '$name-search-result');
        await _revealSettingsSearch(tester);
        await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
        await tester.testTextInput.receiveAction(TextInputAction.search);
        await tester.pumpAndSettle();
        service.emitSettingValue('BOARD_ROTATE', true);
        await tester.pumpAndSettle();
        await captureHealthReview(tester, '$name-overview-rotated');
        await tester.ensureVisible(
          find.byKey(const ValueKey('quick:P2_PIECE_WHEEL')),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(tester, '$name-overview-rotated-player');
        service.emitSettingValue('BOARD_ROTATE', false);
        await tester.pumpAndSettle();
        await tester.ensureVisible(
          find.byKey(const ValueKey('quick:P1_PIECE_WHEEL')),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(tester, '$name-overview-player');
        await tester.ensureVisible(
          find.byKey(const ValueKey('quick:VS_CPU_LEVEL')),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(tester, '$name-overview-cpu');
        final strings = litchStrings(tester.element(previewBoard));
        final assignments = [
          '${strings.usbRightShort}\n${strings.playerSpoken('1')}',
          '${strings.usbLeftShort}\n${strings.playerSpoken('2')}',
        ];
        for (final label in assignments) {
          final badge = find.text(label);
          await tester.ensureVisible(badge);
          await tester.pumpAndSettle();
          expectReviewTextFits(
            tester,
            badge,
            'Player assignments stay complete.',
          );
          final paragraph = tester.renderObject<RenderParagraph>(
            find.descendant(of: badge, matching: find.byType(RichText)),
          );
          final painter = TextPainter(
            text: paragraph.text,
            textDirection: paragraph.textDirection,
            textScaler: paragraph.textScaler,
          )..layout(maxWidth: paragraph.size.width);
          expect(painter.computeLineMetrics(), hasLength(2));
          painter.dispose();
        }
        await captureHealthReview(tester, '$name-player-assignments');
        final reads = service.settingsReadDeviceIds.length;
        for (final page in [
          'lights',
          'behavior',
          'phone',
          'advanced',
          'developer',
        ]) {
          await openSetupPage(tester, page);
          expect(find.byKey(const ValueKey('setup-back')), findsOneWidget);
          expect(
            tester.getSize(find.byKey(const ValueKey('setup-back'))).height,
            greaterThanOrEqualTo(48),
          );
          expect(find.byType(ExpansionTile), findsNothing);
          expect(
            tester.takeException(),
            isNull,
            reason: '$name/$page must fit the authoritative shell.',
          );
          await captureHealthReview(tester, '$name-$page');
          if (page == 'lights') {
            service.emitSettingValue('LIGHT_SQUARE_MODE', 'color');
            await tester.pumpAndSettle();
            await tester.ensureVisible(
              find.byKey(const ValueKey('light-square-appearance')),
            );
            await tester.pumpAndSettle();
            await captureHealthReview(tester, '$name-lights-color');
            await tester.ensureVisible(
              find.byKey(const ValueKey('setting:HINT_THINKING_STYLE')),
            );
            await tester.pumpAndSettle();
            await captureHealthReview(tester, '$name-lights-hint');
            final hintEditor = find.byKey(
              const ValueKey('setting-editor:HINT_THINKING_STYLE'),
            );
            await tester.ensureVisible(hintEditor);
            await tester.pumpAndSettle();
            if (navigation.evaluate().isNotEmpty) {
              expect(
                tester.getRect(hintEditor).overlaps(tester.getRect(navigation)),
                isFalse,
              );
            }
            expect(
              find.descendant(
                of: hintEditor,
                matching: find.byType(TextButton),
              ),
              findsNWidgets(3),
            );
            await captureHealthReview(tester, '$name-lights-hint-choices');
            await tester.ensureVisible(
              find.byKey(const ValueKey('board-lights-manual')),
            );
            await tester.pumpAndSettle();
            await captureHealthReview(tester, '$name-lights-bottom');
          }
        }
        expect(service.settingsReadDeviceIds.length, reads);
        expect(service.settingWrites, isEmpty);
        await tester.pumpWidget(const SizedBox.shrink());
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets(
    'Board lights defers brightness to Connected and opens contextual help',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(360, 780));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _FakeLitchBleService(settings: _defaultSettings);
      addTearDown(service.dispose);
      await pumpSettings(
        tester,
        service,
        textScaler: const TextScaler.linear(2),
      );
      expect(
        find.byKey(const ValueKey<String>('setting-editor:AUTO_BRIGHTNESS')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey<String>('setting-editor:BRIGHTNESS')),
        findsNothing,
      );
      final help = find.byKey(const ValueKey<String>('board-lights-manual'));
      await scrollIntoView(tester, help);
      expect(tester.getSize(help).height, greaterThanOrEqualTo(48));
      await tester.tap(help);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('manual-lights-body')),
        findsOneWidget,
      );
      expect(
        find.textContaining('Open Connected at the top of the app'),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.pageBack();
      await tester.pumpAndSettle();
      expect(service.settingWrites, isEmpty);
    },
  );

  testWidgets(
    'Board lights omits duplicate brightness controls at 200 percent',
    (WidgetTester tester) async {
      await prepareLocalizationReview(tester);
      final service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          const LitchSetting(
            key: 'AUTO_BRIGHTNESS',
            type: 'bool',
            category: 'display',
            value: true,
            defaultValue: true,
          ),
        ],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('en'),
          scale: 2,
          home: LitchMainScaffold(
            bleService: service,
            boardId: 'board-1',
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            initialCraftModeResolved: true,
            loadAppConsolePreference: () async => false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(const ValueKey<String>('main-destination-setup')),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('setting-editor:AUTO_BRIGHTNESS')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey<String>('setting-editor:BRIGHTNESS')),
        findsNothing,
      );
      final help = find.byKey(const ValueKey<String>('board-lights-manual'));
      await scrollIntoView(tester, help);
      await tester.tap(help);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('manual-lights-body')),
        findsOneWidget,
      );
      await tester.pageBack();
      await tester.pumpAndSettle();
      expect(service.settingWrites, isEmpty);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'older firmware omits automatic brightness without inventing a value',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await scrollIntoView(
        tester,
        find.byKey(const ValueKey<String>('board-lights-manual')),
      );
      expect(
        find.byKey(const ValueKey<String>('setting-editor:AUTO_BRIGHTNESS')),
        findsNothing,
      );
      expect(service.settingWrites, isEmpty);
      expect(
        find.byKey(const ValueKey<String>('board-lights-manual')),
        findsOneWidget,
      );
    },
  );

  testWidgets('review combined board health at large text', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(360, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService service = _FakeLitchBleService(
      settings: <LitchSetting>[
        for (final LitchSetting setting in _defaultSettings)
          setting.key == 'BRIGHTNESS'
              ? const LitchSetting(
                  key: 'BRIGHTNESS',
                  type: 'u32',
                  category: 'display',
                  value: 1,
                  defaultValue: 5,
                  min: 0,
                  max: 8,
                )
              : setting,
      ],
    );
    addTearDown(service.dispose);
    await tester.pumpWidget(
      harness(
        service,
        textScaler: const TextScaler.linear(2),
        reviewLitchTheme: true,
      ),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('setting-editor:BRIGHTNESS')),
      findsNothing,
    );
    service.debugAppendConsolePacket(
      '{"event":"board_health","v":1,"h":"000000000","ps":"i","rr":6,"boot":2,"wd":1,"sd":1}\n'
          .codeUnits,
    );
    await tester.pump();
    await scrollIntoView(
      tester,
      find.byKey(const ValueKey<String>('runtime-health-toggle')),
    );
    await captureHealthReview(tester, 'settings-combined-health');
    await scrollIntoView(tester, find.text('Contact Support'));
    await captureHealthReview(tester, 'settings-health-support-continuation');
    expect(
      find.textContaining('Saving is unavailable for this session'),
      findsOneWidget,
    );
    expect(find.textContaining('after it stopped responding'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('offers System, Dark, and Light appearance modes', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    LitchAppearanceMode? selected;
    await tester.pumpWidget(
      harness(
        service,
        appearanceMode: LitchAppearanceMode.system,
        onAppearanceChanged: (LitchAppearanceMode mode) async {
          selected = mode;
        },
      ),
    );
    await tester.pumpAndSettle();

    final Finder control = find.byKey(
      const ValueKey<String>('app-appearance-mode'),
    );
    await scrollIntoView(tester, control);
    expect(find.text('System'), findsOneWidget);
    expect(find.text('Dark'), findsOneWidget);
    expect(find.text('Light'), findsOneWidget);
    await tester.tap(_choiceSegment(find.text('Dark')));
    await tester.pump();
    expect(selected, LitchAppearanceMode.dark);
    expect(tester.getSize(control).height, greaterThanOrEqualTo(48));
  });

  testWidgets('uses an accessible joined appearance control at 200% text', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(
      harness(
        service,
        textScaler: const TextScaler.linear(2),
        appearanceMode: LitchAppearanceMode.dark,
        onAppearanceChanged: (LitchAppearanceMode mode) async {},
      ),
    );
    await tester.pumpAndSettle();

    final Finder dark = find.byKey(
      const ValueKey<String>('app-appearance-dark'),
    );
    await scrollIntoView(tester, dark);
    expect(
      find.byKey(const ValueKey<String>('app-appearance-mode')),
      findsOneWidget,
    );
    expect(
      tester.getSize(_choiceSegment(dark)).height,
      greaterThanOrEqualTo(48),
    );
    expect(
      tester
          .getSemantics(_choiceSegment(dark))
          .getSemanticsData()
          .flagsCollection
          .isSelected,
      ui.Tristate.isTrue,
    );
  });

  testWidgets('health deep link scrolls to and focuses Board health', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await tester.pumpWidget(harness(service));
    await tester.pumpAndSettle();

    var focusEpoch = 0;
    for (final page in ['overview', 'lights']) {
      await _openSettingsPage(tester, page);
      if (page == 'overview') {
        await _revealSettingsSearch(tester);
        await tester.enterText(
          find.byKey(const ValueKey('advanced-search')),
          'MOVE_CONFIRM',
        );
        await tester.pumpAndSettle();
      } else {
        expect(find.byKey(const ValueKey('advanced-search')), findsNothing);
      }
      await tester.pumpWidget(harness(service, healthFocusEpoch: ++focusEpoch));
      await tester.pumpAndSettle();

      expect(find.text('Support & software'), findsNothing);
      expect(find.byKey(const ValueKey('setup-back')), findsNothing);
      expect(
        tester.getTopLeft(find.byKey(const ValueKey('runtime-health-card'))).dy,
        lessThan(300),
      );
      expect(
        WidgetsBinding.instance.focusManager.primaryFocus?.debugLabel,
        'Board health section',
      );
    }
  });

  Future<void> revealAdvanced(WidgetTester tester) async {
    final Finder advanced = find.byKey(
      const ValueKey<String>('setup-open:advanced'),
    );
    await scrollIntoView(tester, advanced);
    await tester.tap(advanced);
    await tester.pumpAndSettle();
  }

  Future<void> enterSettingsSearch(WidgetTester tester, String query) async {
    final Finder search = find.byKey(const ValueKey<String>('advanced-search'));
    await _openSettingsPage(tester, 'search');
    await tester.enterText(search, query);
    await tester.pumpAndSettle();
  }

  for (final rotated in [false, true]) {
    for (final firstPlayer in [1, 2]) {
      testWidgets(
        'Player end labels follow physical ownership rotate=$rotated first=$firstPlayer',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester);
          final semantics = tester.ensureSemantics();
          try {
            final service = _FakeLitchBleService(
              settings: [
                for (final setting in _defaultSettings)
                  _withValue(setting, switch (setting.key) {
                    'BOARD_ROTATE' => rotated,
                    'GAME_FIRST_PLAYER' => firstPlayer,
                    _ => setting.value,
                  }),
              ],
            );
            addTearDown(service.dispose);
            await pumpSettings(tester, service);
            final editor = find.byKey(
              const ValueKey<String>('setting-editor:GAME_FIRST_PLAYER'),
            );
            await scrollIntoView(tester, editor);
            bool playerOneOnRight(bool boardRotated, int first) =>
                (first != 2) != boardRotated;
            void expectAssignments(bool boardRotated, int first) {
              final p1Right = playerOneOnRight(boardRotated, first);
              expect(
                find.bySemanticsLabel(
                  p1Right
                      ? 'USB-right side: Player 1'
                      : 'USB-right side: Player 2',
                ),
                findsOneWidget,
              );
              expect(
                find.bySemanticsLabel(
                  p1Right
                      ? 'USB-left side: Player 2'
                      : 'USB-left side: Player 1',
                ),
                findsOneWidget,
              );
              expect(
                find.descendant(
                  of: editor,
                  matching: find.text(
                    p1Right ? 'USB right\nPlayer 1' : 'USB right\nPlayer 2',
                  ),
                ),
                findsOneWidget,
              );
              expect(
                find.descendant(
                  of: editor,
                  matching: find.text(
                    p1Right ? 'USB left\nPlayer 2' : 'USB left\nPlayer 1',
                  ),
                ),
                findsOneWidget,
              );
            }

            expectAssignments(rotated, firstPlayer);
            service.emitSettingValue('BOARD_ROTATE', !rotated);
            await tester.pumpAndSettle();
            expectAssignments(!rotated, firstPlayer);
            expect(service.settingWrites, isEmpty);
            final otherPlayer = 3 - firstPlayer;
            await scrollIntoView(tester, editor);
            await tester.tap(
              find.byKey(const ValueKey<String>('swap-players')),
            );
            await tester.pumpAndSettle();
            expect(service.settingWrites.last.key, 'GAME_FIRST_PLAYER');
            expect(service.settingWrites.last.value, otherPlayer);
            expectAssignments(!rotated, otherPlayer);
            service.settingsToReturn = [
              for (final setting in service.settingsToReturn)
                _withValue(setting, switch (setting.key) {
                  'BOARD_ROTATE' => rotated,
                  'GAME_FIRST_PLAYER' => firstPlayer,
                  _ => setting.value,
                }),
            ];
            service.connectionController.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.disconnected,
                failure: null,
              ),
            );
            await tester.pumpAndSettle();
            service.connectionController.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.connected,
                failure: null,
              ),
            );
            await tester.pumpAndSettle();
            expectAssignments(rotated, firstPlayer);
            await tester.pump(const Duration(seconds: 5));
            await scrollIntoView(
              tester,
              find.byKey(const ValueKey<String>('quick:GAME_FIRST_PLAYER')),
            );
            await captureHealthReview(
              tester,
              'player-end-rotate-$rotated-first-$firstPlayer',
            );
          } finally {
            semantics.dispose();
          }
        },
      );
    }
  }

  testWidgets(
    'main Setup directly exposes health support software and one search presentation',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      var firmwareOpened = 0;
      await tester.pumpWidget(
        harness(service, onOpenFirmware: () => firmwareOpened++),
      );
      await tester.pumpAndSettle();

      expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
      expect(find.text('Support & software'), findsNothing);
      for (final key in [
        'runtime-health-card',
        'open-contact-support',
        'open-board-software',
      ]) {
        final entry = find.byKey(ValueKey(key));
        await tester.ensureVisible(entry);
        await tester.pumpAndSettle();
        expect(entry.hitTestable(), findsOneWidget);
        expect(find.byKey(const ValueKey('setup-back')), findsNothing);
      }
      expect(
        tester.getRect(find.byKey(const ValueKey('open-board-software'))).top -
            tester
                .getRect(find.byKey(const ValueKey('open-contact-support')))
                .bottom,
        closeTo(20, .01),
        reason:
            'Update checking has a clear section gap after Contact Support.',
      );
      await tester.tap(find.byKey(const ValueKey('open-board-software')));
      expect(firmwareOpened, 1);
      await _revealSettingsSearch(tester);
      final search = find.byKey(const ValueKey('advanced-search'));
      expect(search, findsOneWidget);
      expect(
        tester.getTopLeft(search).dy -
            tester
                .getBottomLeft(
                  find.byKey(const ValueKey('open-board-software')),
                )
                .dy,
        closeTo(20, .01),
        reason: 'Search has the same clear section gap at the end of Setup.',
      );
      await tester.enterText(search, 'MOVE_CONFIRM');
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('setting:MOVE_CONFIRM')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('advanced-search-clear')));
      await tester.pumpAndSettle();
      await _openSettingsPage(tester, 'lights');
      expect(search, findsNothing);
      expect(find.text('Search settings'), findsNothing);
      expect(
        find.byKey(const ValueKey('advanced-search'), skipOffstage: false),
        findsOneWidget,
        reason: 'Only the retained overview owns a search field.',
      );
      expect(service.settingWrites, isEmpty);
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'setup-one-search-leaf');
      await _openSettingsPage(tester, 'overview');
      await _revealSettingsSearch(tester);
      await captureHealthReview(tester, 'setup-main-support-search');
    },
  );

  testWidgets(
    'At the board exposes common controls and focused destinations without duplicate Home actions',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      expect(find.byKey(const ValueKey('setup-board-preview')), findsOneWidget);
      expect(find.byType(ExpansionTile), findsNothing);
      expect(find.text('Play against another player'), findsOneWidget);
      expect(find.text('Computer plays'), findsNothing);
      expect(find.text('USB right\nPlayer 1'), findsOneWidget);
      expect(find.text('USB left\nPlayer 2'), findsOneWidget);
      expect(find.text('Swap players'), findsOneWidget);
      for (final key in ['VS_CPU_LEVEL', 'GAME_FIRST_PLAYER']) {
        expect(find.byKey(ValueKey('quick:$key')), findsOneWidget);
      }
      for (final key in [
        'BOARD_ROTATE',
        'BRIGHTNESS',
        'VS_CPU_PLAYER',
        'TIMER',
      ]) {
        expect(find.byKey(ValueKey('quick:$key')), findsNothing);
      }
      for (final page in [
        'lights',
        'behavior',
        'phone',
        'advanced',
        'developer',
      ]) {
        expect(find.byKey(ValueKey('setup-open:$page')), findsOneWidget);
      }
      expect(find.text('Board actions'), findsNothing);
      expect(find.text('Quick Setup'), findsNothing);
      expect(service.settingWrites, isEmpty);
    },
  );

  testWidgets('Home board actions are aligned and accessible', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle');
    await tester.pump();

    expect(find.text('Board actions'), findsOneWidget);
    expect(find.text('Coaching'), findsOneWidget);
    expect(find.text('P1'), findsOneWidget);
    expect(find.text('P2'), findsOneWidget);
    for (final String key in <String>[
      'control:hint',
      'control:undo',
      'control:new-game',
      'control:coaching:p1',
      'control:coaching:p2',
    ]) {
      final Finder action = find.byKey(ValueKey<String>(key));
      expect(action, findsOneWidget);
      final Size size = tester.getSize(action);
      expect(size.height, greaterThanOrEqualTo(48));
      expect(size.width, greaterThanOrEqualTo(48));
      if (key.startsWith('control:coaching:')) {
        expect(_coachingButton(tester, action).onPressed, isNull);
        expect(
          tester.getSemantics(action).flagsCollection.isToggled,
          ui.Tristate.none,
        );
        expect(
          tester.getSemantics(action).flagsCollection.isEnabled,
          ui.Tristate.isFalse,
        );
        expect(
          tester
              .getSemantics(action)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isFalse,
        );
      } else {
        expect(
          tester.getSemantics(action),
          isSemantics(isButton: true, hasTapAction: true, isEnabled: true),
        );
      }
    }
    final coachingP1 = find.byKey(
      const ValueKey<String>('control:coaching:p1'),
    );
    expect(
      tester.getSemantics(coachingP1).label,
      litchStrings(tester.element(coachingP1)).coachingUnknown('1'),
    );
  });

  testWidgets('retires Hint level while preserving CPU difficulty', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(360, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final service = _FakeLitchBleService(
      settings: <LitchSetting>[
        ..._defaultSettings,
        const LitchSetting(
          key: 'HINT_LEVEL',
          type: 'u32',
          category: 'behavior',
          value: 8,
          defaultValue: 8,
          min: 1,
          max: 11,
          description: 'Hint strength level',
        ),
      ],
    );
    addTearDown(service.dispose);
    await tester.pumpWidget(harness(service, reviewLitchTheme: true));
    await tester.pumpAndSettle();
    await scrollIntoView(
      tester,
      find.byKey(const ValueKey<String>('quick:VS_CPU_LEVEL')),
    );
    await captureHealthReview(tester, 'settings-hint-level-retired');
    expect(
      find.byKey(const ValueKey<String>('quick:VS_CPU_LEVEL')),
      findsOneWidget,
    );
    expect(find.text('Hint strength'), findsNothing);
    await revealAdvanced(tester);
    await captureHealthReview(tester, 'settings-hint-level-retired-advanced');
    await enterSettingsSearch(tester, 'HINT_LEVEL');
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('setting:HINT_LEVEL')),
      findsNothing,
    );
    expect(find.text('Hint strength'), findsNothing);
  });

  testWidgets('uses named CPU choices and USB-relative player assignments', (
    WidgetTester tester,
  ) async {
    final List<LitchSetting> settings = _defaultSettings.map((setting) {
      return switch (setting.key) {
        'VS_CPU_LEVEL' => _withValue(setting, 5),
        'VS_CPU_PLAYER' => _withValue(setting, 1),
        'GAME_FIRST_PLAYER' => _withValue(setting, 2),
        _ => setting,
      };
    }).toList();
    final _FakeLitchBleService service = _FakeLitchBleService(
      settings: settings,
    );
    addTearDown(service.dispose);

    await pumpSettings(tester, service);

    final Finder computerPlays = find.byKey(
      const ValueKey<String>('quick:VS_CPU_PLAYER'),
    );
    final Finder playerEnds = find.byKey(
      const ValueKey<String>('quick:GAME_FIRST_PLAYER'),
    );
    expect(computerPlays, findsOneWidget);
    expect(playerEnds, findsOneWidget);
    expect(
      find.descendant(of: computerPlays, matching: find.byType(Slider)),
      findsNothing,
    );
    expect(
      find.descendant(of: playerEnds, matching: find.byType(Slider)),
      findsNothing,
    );
    expect(
      tester
          .widget<SettingsChoicePills<int>>(
            find.descendant(
              of: computerPlays,
              matching: find.byType(SettingsChoicePills<int>),
              matchRoot: true,
            ),
          )
          .selected,
      <int>{1},
    );
    expect(
      find.descendant(
        of: playerEnds,
        matching: find.text('USB right\nPlayer 2'),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: playerEnds,
        matching: find.text('USB left\nPlayer 1'),
      ),
      findsOneWidget,
    );

    await tester.ensureVisible(computerPlays);
    await tester.pumpAndSettle();
    await tester.tap(
      _choiceSegment(
        find.descendant(of: computerPlays, matching: find.text('P2')),
      ),
    );
    await tester.pumpAndSettle();
    await tester.ensureVisible(playerEnds);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('swap-players')));
    await tester.pumpAndSettle();
    service.emitHomeState(
      sequence: 1,
      action: 'corr',
      kingQueenSwapPromptActive: true,
    );
    await tester.pumpAndSettle();
    expect(
      find.textContaining('Follow the board lights to swap the king and queen'),
      findsOneWidget,
    );

    expect(
      service.settingWrites
          .map((_SettingWrite write) => (write.key, write.value))
          .toList(),
      <(String, Object)>[('VS_CPU_PLAYER', 2), ('GAME_FIRST_PLAYER', 1)],
    );
  });

  testWidgets('ordinary Home corrections never invent a K/Q prompt in Setup', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    final guidance = find.byKey(
      const ValueKey<String>('orientation-correction-guidance'),
    );
    // Neither an empty mismatch set nor a single king lift identifies a swap.
    service.emitHomeState(sequence: 1, action: 'corr');
    await tester.pumpAndSettle();
    expect(guidance, findsNothing);
    service.emitHomeState(
      sequence: 2,
      action: 'corr',
      occupancy: 'EFFF00000000FFFF',
    );
    await tester.pumpAndSettle();
    expect(guidance, findsNothing);
    service.emitHomeState(
      sequence: 3,
      action: 'corr',
      kingQueenSwapPromptActive: true,
    );
    await tester.pumpAndSettle();
    expect(guidance, findsOneWidget);
    service.emitHomeState(sequence: 4, action: 'idle');
    await tester.pumpAndSettle();
    expect(guidance, findsNothing);
  });

  testWidgets(
    'hydrates buffered board setup across connected-board replacement',
    (WidgetTester tester) async {
      final _FakeLitchBleService firstService = _FakeLitchBleService()
        ..emitHomeState(
          sequence: 1,
          action: 'corr',
          kingQueenSwapPromptActive: true,
        );
      final _FakeLitchBleService idleReplacement = _FakeLitchBleService()
        ..emitHomeState(sequence: 2, action: 'idle');
      final _FakeLitchBleService correctingReplacement = _FakeLitchBleService()
        ..emitHomeState(
          sequence: 3,
          action: 'corr',
          kingQueenSwapPromptActive: true,
        );
      addTearDown(firstService.dispose);
      addTearDown(idleReplacement.dispose);
      addTearDown(correctingReplacement.dispose);

      await pumpSettings(tester, firstService);
      expect(
        find.byKey(const ValueKey<String>('orientation-correction-guidance')),
        findsOneWidget,
      );

      await tester.pumpWidget(harness(idleReplacement));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('orientation-correction-guidance')),
        findsNothing,
      );

      await tester.pumpWidget(harness(correctingReplacement));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('orientation-correction-guidance')),
        findsOneWidget,
      );
    },
  );

  testWidgets('player-end reversal names the active K/Q swap recovery', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 1,
          key: 'GAME_FIRST_PLAYER',
          ok: false,
          reason: LitchMutationReason.busy,
          hasValue: false,
          isCompact: true,
        ),
      );
    addTearDown(service.dispose);
    await pumpSettings(tester, service);

    service.emitHomeState(
      sequence: 1,
      action: 'corr',
      kingQueenSwapPromptActive: true,
    );
    await tester.pumpAndSettle();
    final Finder playerEnds = find.byKey(
      const ValueKey<String>('quick:GAME_FIRST_PLAYER'),
    );
    await scrollIntoView(tester, playerEnds);
    await tester.tap(find.byKey(const ValueKey<String>('swap-players')));
    await tester.pumpAndSettle();

    expect(service.settingWrites.single.key, 'GAME_FIRST_PLAYER');
    expect(service.settingWrites.single.value, 2);
    expect(
      find.text(
        'Finish the king and queen swap by following the board lights. The previous value is still shown.',
      ),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('orientation-correction-guidance')),
      findsOneWidget,
    );
  });

  testWidgets('authoritative K/Q guidance stays visible while settings load', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..settingsReadCompleter = Completer<List<LitchSetting>>()
      ..emitHomeState(
        sequence: 1,
        action: 'corr',
        kingQueenSwapPromptActive: true,
      );
    addTearDown(service.dispose);

    await tester.pumpWidget(harness(service));
    await tester.pump();

    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('orientation-correction-guidance')),
      findsOneWidget,
    );
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('orientation-correction-guidance')),
      ),
      isSemantics(isLiveRegion: true),
    );

    service.settingsReadCompleter!.complete(service.settingsToReturn);
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('orientation-correction-guidance')),
      findsOneWidget,
    );
  });

  testWidgets(
    'first Setup frame is usable while descriptor and palette reads are held',
    (WidgetTester tester) async {
      final board = _PaletteBoard();
      final _FakeLitchBleService service = _FakeLitchBleService()
        ..settingsReadCompleter = Completer<List<LitchSetting>>()
        ..deferredEndPalette = Completer<LitchEndPalette?>()
        ..acknowledgedSettingWriteCompleter =
            Completer<LitchSettingWriteResult>();
      addTearDown(() => disposeBoard(service, board));
      board.deliverSettings(service);

      await tester.pumpWidget(harness(service));
      await tester.pump();

      expect(service.settingsReadDeviceIds, <String>['board-1']);
      expect(find.byType(CircularProgressIndicator), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('quick:GAME_FIRST_PLAYER')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
        findsNothing,
      );
      final Finder playerEnds = find.byKey(
        const ValueKey<String>('quick:GAME_FIRST_PLAYER'),
      );
      await scrollIntoView(tester, playerEnds);
      await tester.tap(find.byKey(const ValueKey<String>('swap-players')));
      await tester.pump();
      expect(service.settingWrites, hasLength(1));
      expect(service.settingWrites.single.key, 'GAME_FIRST_PLAYER');
      expect(find.text('USB right\nPlayer 1'), findsOneWidget);

      service.settingsReadCompleter!.complete(service.settingsToReturn);
      service.deferredEndPalette!.complete(
        const LitchEndPalette(
          p1ControlsWheel: 40,
          p2ControlsWheel: 200,
          usesDirectPhysicalKeys: true,
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byType(CircularProgressIndicator), findsNothing);
      expect(
        find.text('USB right\nPlayer 1'),
        findsOneWidget,
        reason: 'background metadata cannot rewind the locally owned value',
      );

      board
        ..settingsRevision = 15
        ..firstPlayer = 1
        ..deliverSettings(service);
      service.acknowledgedSettingWriteCompleter!.complete(
        const LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 1,
            key: 'GAME_FIRST_PLAYER',
            ok: true,
            reason: LitchMutationReason.none,
            hasValue: true,
            value: 1,
            isCompact: false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('USB right\nPlayer 1'), findsOneWidget);
    },
  );

  testWidgets(
    'delayed initial palette cannot rewind a locally owned end color',
    (WidgetTester tester) async {
      final board = _PaletteBoard();
      final _FakeLitchBleService service = _FakeLitchBleService()
        ..settingsReadCompleter = Completer<List<LitchSetting>>()
        ..deferredEndPalette = Completer<LitchEndPalette?>()
        ..acknowledgedSettingWriteCompleter =
            Completer<LitchSettingWriteResult>();
      addTearDown(() => disposeBoard(service, board));
      board.deliverSettings(service);

      await tester.pumpWidget(harness(service));
      await tester.pump();
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:85'),
        ),
      );
      await tester.pump();
      expect(service.settingWrites.single.value, 85);
      expect(
        find.descendant(of: p1, matching: find.text('Green')),
        findsOneWidget,
      );

      service.settingsReadCompleter!.complete(service.settingsToReturn);
      service.deferredEndPalette!.complete(
        const LitchEndPalette(
          p1ControlsWheel: 170,
          p2ControlsWheel: 0,
          usesDirectPhysicalKeys: true,
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(
        find.descendant(of: p1, matching: find.text('Green')),
        findsOneWidget,
        reason: 'late board-state palette Gold cannot replace in-flight Green',
      );

      board
        ..settingsRevision = 15
        ..wheel = 85
        ..deliverSettings(service);
      service.acknowledgedSettingWriteCompleter!.complete(
        const LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 1,
            key: 'P1_END_PIECE_WHEEL',
            ok: true,
            reason: LitchMutationReason.none,
            hasValue: true,
            value: 85,
            isCompact: false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.descendant(of: p1, matching: find.text('Green')),
        findsOneWidget,
      );
    },
  );

  testWidgets(
    'warm reconnect retains populated Setup and its reading position',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await tester.binding.setSurfaceSize(const Size(360, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await pumpSettings(tester, service);
      await openSetupPage(tester, 'lights');

      final Finder settingsScroll = find
          .descendant(
            of: find.byType(SettingsScreen),
            matching: find.byType(Scrollable),
          )
          .first;
      final ScrollPosition position = tester
          .state<ScrollableState>(settingsScroll)
          .position;
      position.jumpTo(
        (position.maxScrollExtent - 80).clamp(0, position.maxScrollExtent),
      );
      await tester.pump();
      final double retainedOffset = position.pixels;
      final Finder lightSquares = find.descendant(
        of: find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
        matching: find.byType(Switch),
      );
      expect(lightSquares, findsOneWidget);

      service.settingsReadCompleter = Completer<List<LitchSetting>>();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();

      expect(
        find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
        findsOneWidget,
      );
      expect(tester.widget<Switch>(lightSquares).onChanged, isNull);
      expect(position.pixels, retainedOffset);

      service.settingsReadCompleter!.complete(service.settingsToReturn);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
        findsOneWidget,
      );
      expect(tester.widget<Switch>(lightSquares).onChanged, isNotNull);
      expect(position.pixels, retainedOffset);
    },
  );

  testWidgets(
    'omits Game clock when the board does not report TIMER',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);

      await pumpSettings(tester, service);

      expect(
        find.byKey(const ValueKey<String>('quick-card:game-clock')),
        findsNothing,
      );
      expect(find.byKey(const ValueKey<String>('quick:TIMER')), findsNothing);
    },
  );

  testWidgets(
    'shows a reported TIMER setting in Advanced Setup',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          const LitchSetting(
            key: 'TIMER',
            type: 'u32',
            category: 'behavior',
            value: 0,
            defaultValue: 0,
            min: 0,
            max: 6,
          ),
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      expect(
        find.byKey(const ValueKey<String>('quick-card:game-clock')),
        findsNothing,
      );
      expect(find.byKey(const ValueKey<String>('quick:TIMER')), findsNothing);

      await revealAdvanced(tester);
      await enterSettingsSearch(tester, 'TIMER');
      expect(
        find.byKey(const ValueKey<String>('setting:TIMER')),
        findsOneWidget,
      );
    },
  );

  testWidgets(
    'removes duplicate Advanced clock after Home projection arrives',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          const LitchSetting(
            key: 'TIMER',
            type: 'u32',
            category: 'behavior',
            value: 0,
            defaultValue: 0,
            min: 0,
            max: 6,
          ),
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      service.emitHomeState(sequence: 1, action: 'idle', clockProjection: true);
      await tester.pumpAndSettle();
      await revealAdvanced(tester);
      await enterSettingsSearch(tester, 'TIMER');

      expect(find.byKey(const ValueKey<String>('setting:TIMER')), findsNothing);
    },
  );

  testWidgets(
    'retained Home projection removes duplicate Advanced clock on recreation',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          const LitchSetting(
            key: 'TIMER',
            type: 'u32',
            category: 'behavior',
            value: 0,
            defaultValue: 0,
            min: 0,
            max: 6,
          ),
        ],
      );
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 7, action: 'idle', clockProjection: true);
      service.bufferedConsoleLines = const <String>[];

      await pumpSettings(tester, service);
      await revealAdvanced(tester);
      await enterSettingsSearch(tester, 'TIMER');

      expect(find.byKey(const ValueKey<String>('setting:TIMER')), findsNothing);
    },
  );

  testWidgets('prioritizes paused Board health above the common controls', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..runtimeHealth = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00AF20004')!,
      );
    addTearDown(service.dispose);

    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await pumpSettings(tester, service, textScaler: const TextScaler.linear(2));

    expect(
      find.byKey(const ValueKey<String>('runtime-health-card')),
      findsOneWidget,
    );
    expect(find.text('Input paused'), findsOneWidget);
    expect(
      find.textContaining('Board input and clocks are paused.'),
      findsOneWidget,
    );
    expect(find.text('Support code: 00AF-2-0004'), findsOneWidget);
    expect(find.text('Support & software'), findsNothing);
    expect(
      tester
          .getTopLeft(find.byKey(const ValueKey<String>('runtime-health-card')))
          .dy,
      lessThan(
        tester
            .getTopLeft(find.byKey(const ValueKey('setup-common-controls')))
            .dy,
      ),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'refreshes health on settings entry without a default reload UI',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService()
        ..runtimeHealth = RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00B000000')!,
        );
      addTearDown(service.dispose);

      await tester.pumpWidget(harness(service));
      await tester.pumpAndSettle();
      expect(service.runtimeHealthFetchCalls, 1);
      expect(find.byIcon(Icons.refresh), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('runtime-health-retry')),
        findsNothing,
      );

      await tester.pumpWidget(harness(service, healthRefreshEpoch: 1));
      await tester.pumpAndSettle();
      expect(service.runtimeHealthFetchCalls, 2);
    },
  );

  testWidgets('keeps healthy board health compact until the user expands it', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..runtimeHealth = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00B000000')!,
      );
    addTearDown(service.dispose);

    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await pumpSettings(tester, service, textScaler: const TextScaler.linear(2));
    await openSetupPage(tester, 'overview');

    expect(find.text('Healthy'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('runtime-health-details')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('open-contact-support')),
      findsOne,
    );
    expect(find.byIcon(Icons.mail_outline), findsOneWidget);
    expect(find.text('Support code: 00B0-0-0000'), findsNothing);

    final Finder healthToggle = find.byKey(
      const ValueKey<String>('runtime-health-toggle'),
    );
    await scrollIntoView(tester, healthToggle);
    await tester.tap(healthToggle);
    await tester.pumpAndSettle();

    expect(
      find.byKey(const ValueKey<String>('runtime-health-details')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('open-contact-support')),
      findsOne,
    );
    expect(find.text('Support code: 00B0-0-0000'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('opens unhealthy board health and allows it to collapse', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..runtimeHealth = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00AF10010')!,
      );
    addTearDown(service.dispose);

    await pumpSettings(tester, service);
    await openSetupPage(tester, 'overview');

    expect(
      find.byKey(const ValueKey<String>('runtime-health-details')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('open-contact-support')),
      findsOne,
    );

    final Finder healthToggle = find.byKey(
      const ValueKey<String>('runtime-health-toggle'),
    );
    await scrollIntoView(tester, healthToggle);
    await tester.tap(healthToggle);
    await tester.pumpAndSettle();

    expect(
      find.byKey(const ValueKey<String>('runtime-health-details')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('open-contact-support')),
      findsOne,
    );
  });

  testWidgets('offers health retry only after automatic retrieval fails', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..runtimeHealth = const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.timedOut,
      );
    addTearDown(service.dispose);

    await pumpSettings(tester, service);
    await openSetupPage(tester, 'overview');
    expect(service.runtimeHealthFetchCalls, 1);
    expect(
      find.byKey(const ValueKey<String>('runtime-health-retry')),
      findsOneWidget,
    );

    final Finder retry = find.byKey(
      const ValueKey<String>('runtime-health-retry'),
    );
    await scrollIntoView(tester, retry);
    await tester.tap(retry);
    await tester.pumpAndSettle();
    expect(service.runtimeHealthFetchCalls, 2);
  });

  testWidgets('power latch offers one validated recheck and refreshes truth', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v2,
        capabilityMask: LitchDeviceCapabilities.settingsBit |
            LitchDeviceCapabilities.runtimeHealthBit |
            LitchDeviceCapabilities.runtimeHealthRecheckBit,
        explicitlyReported: true,
      )
      ..runtimeHealth = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00AF10010')!,
      )
      ..runtimeHealthRecheckResult = RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.succeeded,
        health: RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00B000000')!,
        ),
      );
    addTearDown(service.dispose);

    await pumpSettings(tester, service);

    expect(
      find.text('USB power is limited, play can continue'),
      findsOneWidget,
    );
    expect(
      find.textContaining('Yellow corners on the active player side'),
      findsOneWidget,
    );
    expect(
      find.textContaining('Check the USB-C source and cable'),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('runtime-health-recheck')),
      findsOneWidget,
    );

    final Finder recheck = find.byKey(
      const ValueKey<String>('runtime-health-recheck'),
    );
    await scrollIntoView(tester, recheck);
    await tester.tap(recheck);
    await tester.pumpAndSettle();

    expect(service.runtimeHealthRecheckCalls, 1);
    expect(find.text('Healthy'), findsOneWidget);
    expect(
      find.textContaining('board accepted the power check'),
      findsOneWidget,
    );
    final Rect viewport = tester.getRect(find.byType(ListView).first);
    expect(viewport.overlaps(tester.getRect(find.text('Healthy'))), isTrue);
    expect(
      viewport.overlaps(
        tester.getRect(find.textContaining('board accepted the power check')),
      ),
      isTrue,
    );
  });

  testWidgets('boards without power recheck omit that control', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v2,
        capabilityMask: LitchDeviceCapabilities.settingsBit |
            LitchDeviceCapabilities.runtimeHealthBit,
        explicitlyReported: true,
      )
      ..runtimeHealth = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00AF10010')!,
      );
    addTearDown(service.dispose);

    await pumpSettings(tester, service);
    await openSetupPage(tester, 'overview');

    expect(
      find.byKey(const ValueKey<String>('runtime-health-recheck')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('open-contact-support')),
      findsOne,
    );
  });

  testWidgets(
    'service replacement ignores the older settings load completion',
    (WidgetTester tester) async {
      final _FakeLitchBleService oldService = _FakeLitchBleService(
        settings: const <LitchSetting>[
          LitchSetting(
            key: 'VS_CPU_LEVEL',
            type: 'u32',
            category: 'behavior',
            value: 5,
            defaultValue: 0,
            min: 0,
            max: 11,
          ),
        ],
      )..settingsReadCompleter = Completer<List<LitchSetting>>();
      final _FakeLitchBleService newService = _FakeLitchBleService(
        settings: const <LitchSetting>[
          LitchSetting(
            key: 'VS_CPU_LEVEL',
            type: 'u32',
            category: 'behavior',
            value: 8,
            defaultValue: 0,
            min: 0,
            max: 11,
          ),
        ],
      )..settingsReadCompleter = Completer<List<LitchSetting>>();
      addTearDown(oldService.dispose);
      addTearDown(newService.dispose);

      await tester.pumpWidget(harness(oldService));
      await tester.pump();
      await tester.pumpWidget(harness(newService));
      await tester.pump();

      newService.settingsReadCompleter!.complete(newService.settingsToReturn);
      await tester.pumpAndSettle();
      final Finder opponent = find.byKey(
        const ValueKey<String>('quick:VS_CPU_LEVEL'),
      );
      expect(
        find.descendant(of: opponent, matching: find.textContaining('level 8')),
        findsOne,
      );

      oldService.settingsReadCompleter!.complete(oldService.settingsToReturn);
      await tester.pumpAndSettle();
      expect(
        find.descendant(of: opponent, matching: find.textContaining('level 8')),
        findsOne,
      );
      expect(
        find.descendant(of: opponent, matching: find.textContaining('level 5')),
        findsNothing,
      );
    },
  );

  testWidgets('action controls preserve accepted until a terminal outcome', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();

    expect(service.controlCalls, hasLength(1));
    expect(service.controlCalls.single.action, LitchControlAction.undo);
    expect(service.controlCalls.single.undoCount, 1);
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.accepted,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(
      find.text('Undo accepted. Waiting for the board to finish…'),
      findsNothing,
    );

    await tester.tap(undo);
    await tester.pump();
    expect(service.controlCalls, hasLength(1));

    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(find.text('Undo ready. Follow the board lights.'), findsNothing);
    expect(find.text('Cancel Undo'), findsOneWidget);
    expect(tester.widget<OutlinedButton>(undo).onPressed, isNotNull);
  });

  testWidgets('late control outcome cannot replace a newer request', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    final _ControlCall first = service.controlCalls.single;
    service.emitControl(call: first, phase: LitchControlPhase.applied);
    await tester.pump();

    await tester.tap(undo);
    await tester.pump();
    final _ControlCall second = service.controlCalls.last;
    expect(second.requestId, isNot(first.requestId));
    expect(
      find.text('Cancel Undo requested. Waiting for the board…'),
      findsNothing,
    );

    service.emitControl(
      call: first,
      phase: LitchControlPhase.rejected,
      reason: LitchMutationReason.busy,
    );
    await tester.pump();
    expect(
      find.text('Cancel Undo requested. Waiting for the board…'),
      findsNothing,
    );
    expect(
      find.text('The board is busy. Try again in a moment.'),
      findsNothing,
    );
    expect(find.text('Cancel Undo'), findsOneWidget);
    await tester.tap(undo);
    await tester.pump();
    expect(service.controlCalls, hasLength(2));

    service.emitControl(call: second, phase: LitchControlPhase.applied);
    await tester.pump();
    expect(find.text('Undo cancelled.'), findsNothing);
    expect(find.text('Undo'), findsOneWidget);
  });

  testWidgets('retained Home state restores Cancel Undo on recreation', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(sequence: 4, action: 'progress', undoPending: true);
    service.bufferedConsoleLines = const <String>[];

    await pumpBoardActions(tester, service);

    expect(find.text('Cancel Undo'), findsOneWidget);
    expect(find.text('Undo ready. Follow the board lights.'), findsNothing);
  });

  testWidgets('Undo becomes Cancel Undo until canonical cancel is acked', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    final _ControlCall first = service.controlCalls.single;
    service.emitControl(
      call: first,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.undoPending,
    );
    await tester.pump();

    expect(find.text('Cancel Undo'), findsOneWidget);
    expect(find.text('Undo ready. Follow the board lights.'), findsNothing);

    await tester.tap(undo);
    await tester.pump();
    expect(service.controlCalls, hasLength(2));
    expect(service.controlCalls.last.action, LitchControlAction.undo);
    expect(find.text('Cancel Undo'), findsOneWidget);

    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.idle,
    );
    await tester.pump();
    expect(find.text('Undo'), findsOneWidget);
    expect(find.text('Undo cancelled.'), findsNothing);
  });

  testWidgets('physical Undo completion clears Cancel Undo from live state', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    service.emitHomeState(
      sequence: 40,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
    );
    await tester.pump();
    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.undoPending,
    );
    service.emitHomeState(
      sequence: 41,
      action: 'progress',
      undoPending: true,
      fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
    );
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);

    service.emitHomeState(
      sequence: 42,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
    );
    await tester.pump();
    expect(find.text('Undo'), findsOneWidget);
    expect(find.text('Cancel Undo'), findsNothing);
    expect(find.text('Undo complete.'), findsNothing);
  });

  testWidgets(
    'Undo completion handles live progress before its acknowledgement',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);

      final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
      await scrollIntoView(tester, undo);
      service.emitHomeState(
        sequence: 50,
        action: 'idle',
        undoPending: false,
        fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
      );
      await tester.pump();
      await tester.tap(undo);
      await tester.pump();
      service.emitHomeState(
        sequence: 51,
        action: 'progress',
        undoPending: true,
        fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.accepted,
        reason: LitchMutationReason.none,
        actionState: LitchControlActionState.idle,
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
        actionState: LitchControlActionState.undoPending,
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);

      service.emitHomeState(
        sequence: 52,
        action: 'idle',
        undoPending: false,
        fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
      );
      await tester.pump();
      expect(find.text('Undo'), findsOneWidget);
      expect(find.text('Undo complete.'), findsNothing);
    },
  );

  testWidgets(
    'authoritative completion before acknowledgement cannot restick Cancel Undo',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);

      final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
      await scrollIntoView(tester, undo);
      service.emitHomeState(
        sequence: 60,
        action: 'idle',
        undoPending: false,
        fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
      );
      await tester.pump();
      await tester.tap(undo);
      await tester.pump();
      service.emitHomeState(
        sequence: 61,
        action: 'progress',
        undoPending: true,
        fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
      );
      service.emitHomeState(
        sequence: 62,
        action: 'idle',
        undoPending: false,
        fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
      );
      await tester.pump();
      expect(find.text('Undo'), findsOneWidget);

      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
        actionState: LitchControlActionState.undoPending,
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsNothing);
      expect(find.text('Undo'), findsOneWidget);
      expect(find.text('Undo complete.'), findsNothing);
    },
  );

  testWidgets('physical X Undo and cancellation stay authoritative on Home', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    service.emitHomeState(
      sequence: 70,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
    );
    service.emitHomeState(
      sequence: 71,
      action: 'progress',
      undoPending: true,
      fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
    );
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);

    service.emitHomeState(
      sequence: 72,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
    );
    await tester.pump();
    expect(find.text('Undo'), findsOneWidget);
    expect(find.text('Undo cancelled.'), findsNothing);
  });

  testWidgets(
    'reconnect rehydrates physical Undo and rejects stale Home state',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);

      final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
      await scrollIntoView(tester, undo);
      service.emitHomeState(
        sequence: 0xffffffff,
        action: 'progress',
        undoPending: true,
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);

      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(find.text('Undo'), findsOneWidget);

      service.emitHomeState(sequence: 1, action: 'progress', undoPending: true);
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);

      service.emitHomeState(
        sequence: 0xffffffff,
        action: 'idle',
        undoPending: false,
      );
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);

      service.emitHomeState(sequence: 2, action: 'idle', undoPending: false);
      await tester.pump();
      expect(find.text('Undo'), findsOneWidget);
      expect(find.text('Undo finished. Board is ready.'), findsNothing);
    },
  );

  testWidgets('Hint acknowledgement alone does not claim a ready hint', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle');
    await tester.pump();

    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.idle,
    );
    await tester.pump();

    // Applied acknowledges the action, not engine completion or a displayed
    // hint. Legacy firmware may never publish that additional authority.
    expect(find.textContaining('board lights'), findsNothing);
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
    expect(find.text('Hint ready. Follow the board lights.'), findsNothing);
  });

  testWidgets(
    'Hint controls follow board search, ready toggles and cancellation, never the ACK',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);
      service.emitHomeState(sequence: 1, action: 'idle', assistance: '2o');
      await tester.pump();
      final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
      await scrollIntoView(tester, hint);
      await tester.tap(hint);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
      );
      await tester.pump();
      expect(service.snapshotRequests, 1);
      expect(find.text('Updating hint state from the board…'), findsNothing);
      expect(find.text('Hint ready. Follow the board lights.'), findsNothing);
      expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);

      service.emitHomeState(sequence: 2, action: 'idle', assistance: '3t');
      await tester.pump();
      expect(find.text('Cancel hint'), findsOneWidget);
      expect(find.text('Finding a hint…'), findsNothing);
      expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
      service.emitHomeState(sequence: 3, action: 'idle', assistance: '3he2e4');
      await tester.pump();
      expect(find.text('Hide hint'), findsOneWidget);
      expect(find.text('Hint ready. Follow the board lights.'), findsNothing);
      expect(find.byTooltip('Hide hint on the board'), findsOneWidget);
      service.emitHomeState(sequence: 4, action: 'idle', assistance: '3re2e4');
      await tester.pump();
      expect(find.text('Hide hint'), findsOneWidget);
      expect(find.text('Finding a hint…'), findsNothing);

      // A ready manual Hint toggles off, then a fresh tap requests it again.
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls, hasLength(2));
      expect(service.controlCalls.last.action, LitchControlAction.hint);
      service.emitHomeState(sequence: 5, action: 'idle', assistance: '2o');
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(find.text('Hint'), findsOneWidget);
      expect(service.snapshotRequests, 1);
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls, hasLength(3));
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      service.emitHomeState(sequence: 6, action: 'idle', assistance: '3p');
      await tester.pump();
      expect(find.text('Cancel hint'), findsOneWidget);
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls, hasLength(4));
      // Board truth can arrive before the acknowledgement too.
      service.emitHomeState(sequence: 7, action: 'idle', assistance: '2o');
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
      );
      await tester.pump();
      expect(find.text('Hint'), findsOneWidget);
      expect(find.text('Hint hidden.'), findsNothing);
      expect(service.snapshotRequests, 2);
    },
  );

  testWidgets(
    'hint calculation indicator follows board waiting and cancellation',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);
      final semantics = tester.ensureSemantics();
      final hint = find.byKey(const ValueKey<String>('control:hint'));
      final progress = find.descendant(
        of: hint,
        matching: find.byType(CircularProgressIndicator),
      );
      await scrollIntoView(tester, hint);
      service.emitHomeState(sequence: 1, action: 'idle', assistance: '2o');
      await tester.pump();
      await tester.tap(hint);
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Sending Hint is not board thinking.',
      );
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'An ACK does not establish thinking.',
      );

      service.emitHomeState(sequence: 2, action: 'idle', assistance: '3p');
      await tester.pump();
      expect(progress, findsOneWidget);
      expect(tester.getSemantics(hint).value, 'Finding a hint…');
      expect(tester.getSemantics(hint).label, 'Cancel hint on the board');
      expect(
        tester.getSemantics(hint).flagsCollection.isSelected,
        ui.Tristate.isTrue,
      );
      final label = find.descendant(
        of: hint,
        matching: find.text('Cancel hint'),
      );
      expect(
        tester.getRect(progress).top,
        greaterThan(tester.getRect(label).bottom),
      );
      expect(
        tester.getRect(progress).center.dx,
        closeTo(tester.getRect(label).center.dx, .1),
      );
      final waitingRect = tester.getRect(hint);

      // The actual button still dispatches cancellation while its ring is moving.
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls, hasLength(2));
      expect(service.controlCalls.last.action, LitchControlAction.hint);
      expect(service.controlCalls.last.enabled, isNull);
      expect(progress, findsOneWidget);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Unconfirmed old waiting is suppressed.',
      );
      service.emitHomeState(sequence: 3, action: 'idle', assistance: '2o');
      await tester.pump();
      expect(progress, findsNothing);
      expect(tester.getRect(hint), waitingRect);

      service.emitHomeState(sequence: 4, action: 'idle', assistance: '3t');
      await tester.pump();
      expect(progress, findsOneWidget);
      service.emitHomeState(sequence: 5, action: 'idle', assistance: '3he2e4');
      await tester.pump();
      expect(progress, findsNothing);
      expect(tester.getSemantics(hint).value, isEmpty);
      expect(find.text('Hide hint'), findsOneWidget);
      expect(tester.getSemantics(hint).label, 'Hide hint on the board');
      service.emitHomeState(sequence: 6, action: 'idle', assistance: '3re2e4');
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Refresh already has a visible move.',
      );
      expect(tester.getRect(hint), waitingRect);

      service.emitHomeState(sequence: 7, action: 'idle', assistance: '3t');
      await tester.pump();
      expect(progress, findsOneWidget);
      service.emitHomeState(sequence: 8, action: 'corr', assistance: '1x');
      await tester.pump();
      expect(progress, findsNothing);
      service.emitHomeState(sequence: 9, action: 'idle', assistance: '3p');
      await tester.pump();
      expect(progress, findsOneWidget);
      service.emitHomeState(sequence: 10, action: 'idle');
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Unknown assistance clears progress.',
      );

      service.emitHomeState(sequence: 11, action: 'idle', assistance: '3t');
      await tester.pump();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(progress, findsNothing);
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Reconnect needs fresh board truth.',
      );
      service.emitHomeState(sequence: 1, action: 'idle', assistance: '3p');
      await tester.pump();
      expect(progress, findsOneWidget);
      service.activeBoardId = 'board-2';
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-2',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(
        progress,
        findsNothing,
        reason: 'Another board cannot inherit waiting.',
      );
      expect(tester.takeException(), isNull);
      semantics.dispose();
    },
  );

  testWidgets(
    'Show Move calculation indicator keeps geometry and accessible motion',
    (WidgetTester tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final controller = BoardActionsController(
        bleService: service,
        boardId: 'board-1',
      );
      addTearDown(controller.dispose);
      final motion = ValueNotifier<(bool, bool)>((false, true));
      addTearDown(motion.dispose);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            body: ValueListenableBuilder<(bool, bool)>(
              valueListenable: motion,
              builder: (context, value, child) => MediaQuery(
                data: MediaQueryData(
                  textScaler: TextScaler.linear(2),
                  disableAnimations: value.$1,
                ),
                child: TickerMode(enabled: value.$2, child: child!),
              ),
              child: ListView(
                padding: const EdgeInsets.all(16),
                children: [HomeBoardActions(controller: controller)],
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final semantics = tester.ensureSemantics();
      final hint = find.byKey(const ValueKey<String>('control:hint'));
      await scrollIntoView(tester, hint);
      final progress = find.descendant(
        of: hint,
        matching: find.byType(CircularProgressIndicator),
      );
      final controls = [
        'hint',
        'undo',
        'new-game',
      ].map((name) => find.byKey(ValueKey('control:$name'))).toList();
      final before = controls.map(tester.getRect).toList();
      service.emitHomeState(
        sequence: 1,
        action: 'idle',
        coachingMask: 1,
        assistance: '7p',
      );
      await tester.pump();
      expect(progress, findsOneWidget);
      expect(find.text('Show Move'), findsOneWidget);
      expect(
        tester.getSemantics(hint).label,
        'Show the suggested move on the board',
      );
      expect(tester.getSemantics(hint).value, 'Finding a hint…');
      expect(
        tester.getSemantics(hint).flagsCollection.isSelected,
        ui.Tristate.none,
      );
      for (var index = 0; index < controls.length; index++) {
        expect(tester.getRect(controls[index]), before[index]);
        expect(before[index].height, greaterThanOrEqualTo(48));
        expect(before[index].width, greaterThanOrEqualTo(48));
      }
      expectReviewTextFits(
        tester,
        find.text('Show Move'),
        '200% Show Move label remains complete',
      );
      expect(tester.getSize(progress), const Size(8, 8));
      expect(tester.widget<CircularProgressIndicator>(progress).value, isNull);
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        tester.binding.hasScheduledFrame,
        isTrue,
        reason: 'Normal waiting animates.',
      );
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls.single.action, LitchControlAction.hint);
      expect(service.controlCalls.single.enabled, isNull);
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(
        progress,
        findsOneWidget,
        reason: 'Show Move ACK leaves board waiting intact.',
      );

      motion.value = (true, true);
      await tester.pump();
      // Finish the preceding tap's Material splash before checking idle frames.
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      expect(progress, findsOneWidget);
      expect(
        tester.widget<CircularProgressIndicator>(progress).value,
        isNotNull,
      );
      expect(
        tester.binding.hasScheduledFrame,
        isFalse,
        reason: 'Reduced motion keeps a steady waiting cue.',
      );
      expect(tester.getSemantics(hint).value, 'Finding a hint…');
      motion.value = (false, false);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(
        tester.binding.hasScheduledFrame,
        isFalse,
        reason: 'Hidden pages do not tick.',
      );
      motion.value = (false, true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(tester.binding.hasScheduledFrame, isTrue);
      service.emitHomeState(
        sequence: 2,
        action: 'idle',
        coachingMask: 1,
        assistance: '7he2e4',
      );
      await tester.pump();
      expect(progress, findsNothing);
      expect(tester.getSemantics(hint).value, isEmpty);
      expect(tester.getRect(hint), before.first);
      expect(tester.takeException(), isNull);
      semantics.dispose();
    },
  );

  testWidgets('hint calculation preserves compact button and neighbor rectangles', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    addTearDown(() => tester.binding.setSurfaceSize(null));
    // Retain the original 1x geometry. Enlarged compact actions use the
    // approved 130% cap and must remain fixed through every Hint state.
    const originalActionRects = <String, List<List<double>>>{
      '390x844-1x-en': [
        [32.0, 68.0, 103.333, 86.0],
        [143.333, 68.0, 103.333, 86.0],
        [254.667, 68.0, 103.333, 86.0],
      ],
      '320x844-1x-de': [
        [32.0, 68.0, 124.0, 86.0],
        [164.0, 68.0, 124.0, 86.0],
        [32.0, 162.0, 124.0, 86.0],
      ],
      '844x390-1x-de': [
        [32.0, 68.0, 254.667, 86.0],
        [294.667, 68.0, 254.667, 86.0],
        [557.333, 68.0, 254.667, 86.0],
      ],
      '390x844-1x-en-XA': [
        [32.0, 68.0, 103.333, 86.0],
        [143.333, 68.0, 103.333, 86.0],
        [254.667, 68.0, 103.333, 86.0],
      ],
    };
    for (final (size, scale, locale) in <(Size, double, Locale)>[
      (const Size(390, 844), 1, const Locale('en')),
      (const Size(320, 844), 1, const Locale('de')),
      (const Size(844, 390), 1, const Locale('de')),
      (const Size(390, 844), 2, const Locale('en')),
      (const Size(320, 844), 2, const Locale('de')),
      (const Size(844, 390), 2, const Locale('de')),
      (const Size(390, 844), 1, litchPseudoLocale),
      (const Size(320, 844), 2, litchPseudoLocale),
    ]) {
      await tester.binding.setSurfaceSize(size);
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final controller = BoardActionsController(
        bleService: service,
        boardId: 'board-1',
      );
      addTearDown(controller.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: locale,
          scale: scale,
          home: Scaffold(
            body: Builder(
              builder: (context) => MediaQuery(
                data: MediaQuery.of(context).copyWith(disableAnimations: true),
                child: ListView(
                  padding: const EdgeInsets.all(16),
                  children: [HomeBoardActions(controller: controller)],
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final layout =
          '${size.width.toInt()}x${size.height.toInt()}-${scale.toInt()}x-${locale.toLanguageTag()}';
      const keys = ['hint', 'undo', 'new-game'];
      final controls = keys
          .map((key) => find.byKey(ValueKey('control:$key')))
          .toList();
      final coachingControls = [
        'coaching:p1',
        'coaching:p2',
      ].map((key) => find.byKey(ValueKey('control:$key'))).toList();
      final actionBounds = controls.map(tester.getRect).toList();
      final coachingBounds = coachingControls.map(tester.getRect).toList();
      for (final control in coachingControls) {
        expect(_coachingButton(tester, control).onPressed, isNull);
        final controlSize = tester.getSize(control);
        expect(controlSize.height, greaterThanOrEqualTo(48));
        expect(controlSize.width, greaterThanOrEqualTo(48));
      }
      List<double> coordinates(Rect rect) => [
        rect.left,
        rect.top,
        rect.width,
        rect.height,
      ].map((value) => (value * 1000).round() / 1000).toList();
      var sequence = 0;
      for (final (name, assistance) in <(String, String)>[
        ('off', '2o'),
        ('manual-wait', '3p'),
        ('manual-ready', '3he2e4'),
        ('coach-wait', '7p'),
        ('coach-ready', '7he2e4'),
      ]) {
        service.emitHomeState(
          sequence: ++sequence,
          action: 'idle',
          coachingMask: 1,
          assistance: assistance,
        );
        await tester.pumpAndSettle();
        final rectangles = controls
            .map((control) => coordinates(tester.getRect(control)))
            .toList();
        if (scale == 1) {
          expect(
            rectangles,
            originalActionRects[layout]!,
            reason: '$layout/$name retains the original 1x action rectangles.',
          );
        }
        expect(
          controls.map(tester.getRect).toList(),
          actionBounds,
          reason: '$layout/$name keeps all action rectangles fixed.',
        );
        expect(
          coachingControls.map(tester.getRect).toList(),
          coachingBounds,
          reason:
              '$layout/$name keeps button geometry stable through hydration.',
        );
        expect(coachingToggled(tester, coachingControls[0]), isTrue);
        expect(coachingToggled(tester, coachingControls[1]), isFalse);
        final hint = controls.first;
        final label = find
            .descendant(of: hint, matching: find.byType(Text))
            .first;
        final progress = find.descendant(
          of: hint,
          matching: find.byType(CircularProgressIndicator),
        );
        final labelRect = tester.getRect(label);
        final Rect? progressRect = progress.evaluate().isEmpty
            ? null
            : tester.getRect(progress);
        debugPrintSynchronously(
          'HINT_GEOMETRY ${jsonEncode({'layout': layout, 'state': name, 'controls': rectangles, 'icons': controls.map((control) => coordinates(tester.getRect(find.descendant(of: control, matching: find.byType(Icon)).first))).toList(), 'labels': controls.map((control) => coordinates(tester.getRect(find.descendant(of: control, matching: find.byType(Text)).first))).toList(), 'hintLabel': tester.widget<Text>(label).data, 'progress': progressRect == null ? null : coordinates(progressRect), 'labelRingGap': progressRect == null ? null : progressRect.top - labelRect.bottom, 'bottomClearance': progressRect == null ? null : tester.getRect(hint).bottom - progressRect.bottom})}',
        );
        if (progressRect != null) {
          expect(progressRect.top, greaterThan(labelRect.bottom));
          expect(progressRect.bottom, lessThan(tester.getRect(hint).bottom));
          expect(progressRect.center.dx, closeTo(labelRect.center.dx, .1));
        }
        for (final control in controls) {
          final bounds = tester.getRect(control);
          expect(bounds.width, greaterThanOrEqualTo(48));
          expect(bounds.height, greaterThanOrEqualTo(48));
          expect(bounds.left, greaterThanOrEqualTo(32));
          expect(bounds.right, lessThanOrEqualTo(size.width - 32));
          final text = find.descendant(
            of: control,
            matching: find.byType(Text),
          );
          expectReviewTextFits(
            tester,
            text,
            '$layout/$name keeps the full localized label.',
          );
          final paragraph = tester.renderObject<RenderParagraph>(
            find.descendant(of: text, matching: find.byType(RichText)),
          );
          expect(paragraph.textScaler.scale(10), scale == 1 ? 10 : 13);
        }
        for (var index = 0; index < actionBounds.length; index++) {
          for (final other in actionBounds.skip(index + 1)) {
            expect(actionBounds[index].overlaps(other), isFalse);
          }
        }
        expect(tester.takeException(), isNull);
        if (name == 'coach-wait') {
          await captureHealthReview(tester, 'hint-original-size-$layout');
        }
      }
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
  });

  testWidgets('unrelated Home revision does not confirm a Hint toggle', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 10, action: 'idle', assistance: '2o');
    await tester.pump();
    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitHomeState(sequence: 11, action: 'idle', assistance: '2o');
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(service.snapshotRequests, 1);
    expect(find.text('Updating hint state from the board…'), findsNothing);
    service.emitHomeState(sequence: 12, action: 'idle', assistance: '3t');
    await tester.pump();
    expect(find.text('Cancel hint'), findsOneWidget);
    expect(find.text('Finding a hint…'), findsNothing);
  });

  testWidgets('physical Hint and Coaching ownership update without app taps', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    service.emitHomeState(sequence: 1, action: 'idle', assistance: '3he2e4');
    await tester.pump();
    expect(find.text('Hide hint'), findsOneWidget);
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
    service.emitHomeState(sequence: 2, action: 'idle', assistance: '5he2e4');
    await tester.pump();
    expect(find.text('Hide hint'), findsNothing);
    expect(find.text('Show Move'), findsOneWidget);
    expect(
      find.byTooltip('Show the suggested move on the board'),
      findsOneWidget,
    );
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);

    service.emitHomeState(
      sequence: 3,
      action: 'lift',
      assistance: '1le20000001010000000',
    );
    await tester.pump();
    expect(find.text('Cancel hint'), findsOneWidget);
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
    service.emitHomeState(sequence: 4, action: 'idle', assistance: '3he2e4');
    await tester.pump();
    expect(find.text('Hide hint'), findsOneWidget);
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
    expect(service.controlCalls, isEmpty);

    service.emitHomeState(sequence: 5, action: 'idle');
    await tester.pump();
    expect(find.text('Hint'), findsOneWidget);
    expect(find.text('Hint ready. Follow the board lights.'), findsNothing);
  });

  testWidgets('Show Move remains repeatable during Coaching at 200% text', (
    WidgetTester tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(
      sequence: 1,
      action: 'idle',
      coachingMask: 1,
      assistance: '7he2e4',
    );
    await pumpBoardActions(tester, service, textScaler: TextScaler.linear(2));
    final semantics = tester.ensureSemantics();
    final hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    expect(
      find.descendant(of: hint, matching: find.text('Show Move')),
      findsOneWidget,
    );
    expect(
      tester.getSemantics(hint).label,
      'Show the suggested move on the board',
    );
    expect(
      tester.getSemantics(hint).flagsCollection.isSelected,
      ui.Tristate.none,
    );
    expect(tester.getSize(hint).height, greaterThanOrEqualTo(48));
    expect(tester.getSize(hint).width, greaterThanOrEqualTo(48));
    for (var tap = 1; tap <= 2; tap++) {
      await tester.tap(hint);
      await tester.pump();
      expect(service.controlCalls, hasLength(tap));
      expect(service.controlCalls.last.action, LitchControlAction.hint);
      expect(service.controlCalls.last.enabled, isNull);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(service.snapshotRequests, 0);
    }
    service.emitHomeState(
      sequence: 2,
      action: 'idle',
      coachingMask: 1,
      assistance: '7p',
    );
    await tester.pump();
    expect(
      find.descendant(of: hint, matching: find.text('Show Move')),
      findsOneWidget,
    );
    await tester.tap(hint);
    await tester.pump();
    expect(service.controlCalls, hasLength(3));
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    service.emitHomeState(
      sequence: 3,
      action: 'cpu',
      coachingMask: 1,
      assistance: '5x',
    );
    await tester.pump();
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
    service.emitHomeState(
      sequence: 4,
      action: 'corr',
      coachingMask: 1,
      assistance: '5x',
    );
    await tester.pump();
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
    expect(service.controlCalls, hasLength(3));
    expect(tester.takeException(), isNull);
    semantics.dispose();
  });

  testWidgets('queued Show Move survives automatic reveal but not Coaching exit', (
    WidgetTester tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(
      sequence: 1,
      action: 'idle',
      coachingMask: 1,
      assistance: '6o',
    );
    await pumpBoardActions(tester, service);
    final controller = tester
        .widget<HomeBoardActions>(find.byType(HomeBoardActions))
        .controller;
    final hint = find.byKey(const ValueKey<String>('control:hint'));
    final p2 = find.byKey(const ValueKey<String>('control:coaching:p2'));
    await scrollIntoView(tester, p2);
    await tester.tap(p2);
    await tester.pump();
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    expect(service.controlCalls, hasLength(1));
    service.emitHomeState(
      sequence: 2,
      action: 'idle',
      coachingMask: 3,
      assistance: '7he2e4',
    );
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(
      service.controlCalls,
      hasLength(2),
      reason:
          'A reveal remains an action even after Coaching has shown the move.',
    );
    expect(service.controlCalls.last.action, LitchControlAction.hint);
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    await controller.dispatch(LitchControlAction.hint);
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.rejected,
      reason: LitchMutationReason.busy,
    );
    service.emitHomeState(
      sequence: 3,
      action: 'idle',
      coachingMask: 3,
      assistance: '7he2e4',
    );
    await tester.pump(const Duration(milliseconds: 200));
    expect(
      service.controlCalls,
      hasLength(4),
      reason:
          'An already visible move does not acknowledge a rejected Show Move replay.',
    );
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    await controller.dispatch(
      LitchControlAction.coaching,
      owner: LitchControlOwner.p2,
      enabled: false,
    );
    await controller.dispatch(LitchControlAction.hint);
    service.emitHomeState(
      sequence: 4,
      action: 'idle',
      coachingMask: 0,
      assistance: '3he2e4',
    );
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(
      service.controlCalls,
      hasLength(5),
      reason:
          'An old Show Move must not become a manual Hide after Coaching exits.',
    );
  });

  testWidgets('Show Move timeout cannot be confirmed by already-visible Home', (
    WidgetTester tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(
      sequence: 1,
      action: 'idle',
      coachingMask: 1,
      assistance: '7he2e4',
    );
    await pumpBoardActions(tester, service);
    final controller = tester
        .widget<HomeBoardActions>(find.byType(HomeBoardActions))
        .controller;
    final hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.timeout,
    );
    service.emitHomeState(
      sequence: 2,
      action: 'idle',
      coachingMask: 1,
      assistance: '7he2e4',
    );
    await tester.pump();
    expect(
      service.supportSessionDiagnostics.controlRecoveryConfirmed,
      0,
      reason:
          'An existing suggestion cannot prove the white acknowledgement replay occurred.',
    );
    await tester.pump(BoardActionsController.coachingSyncTimeout);
    expect(
      service.controlCalls,
      hasLength(1),
      reason: 'An ambiguous timeout must never automatically replay Show Move.',
    );
    expect(service.supportSessionDiagnostics.controlRecoveryUnconfirmed, 1);
    expect(
      controller.statusMessage,
      'The board did not confirm Show Move. Check the board before trying again.',
    );
    expect(
      find.descendant(of: hint, matching: find.text('Show Move')),
      findsOneWidget,
    );
  });

  testWidgets('Coaching takeover retires pending manual toggle ownership', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle', assistance: '3he2e4');
    await tester.pump();
    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitHomeState(sequence: 2, action: 'idle', assistance: '5he2e4');
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(service.snapshotRequests, 0);
    expect(
      find.text('Coaching hint ready. Follow the board lights.'),
      findsNothing,
    );
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
    await tester.pump(const Duration(seconds: 3));
    expect(find.textContaining('Hint state is unavailable'), findsNothing);
  });

  testWidgets(
    'missing Hint confirmation becomes unknown and reconnect recovers',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpBoardActions(tester, service);
      final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
      await scrollIntoView(tester, hint);
      service.emitHomeState(sequence: 20, action: 'idle', assistance: '2o');
      await tester.pump();
      await tester.tap(hint);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
      );
      await tester.pump();
      await tester.pump(const Duration(seconds: 3));
      expect(
        find.text('Hint state is unavailable. Follow the board lights.'),
        findsOneWidget,
      );
      expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      service.emitHomeState(sequence: 1, action: 'idle', assistance: '3he2e4');
      await tester.pump();
      expect(find.text('Hide hint'), findsOneWidget);
      expect(find.textContaining('Hint state is unavailable'), findsNothing);
    },
  );

  testWidgets('physical Undo status supersedes an older app action', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle');
    await tester.pump();

    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.idle,
    );
    await tester.pump();
    expect(
      find.text('Follow the board lights for hint guidance.'),
      findsNothing,
    );

    service.emitHomeState(
      sequence: 80,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
    );
    service.emitHomeState(
      sequence: 81,
      action: 'progress',
      undoPending: true,
      fen: '8/8/8/8/8/8/8/4K2k w - - 0 1',
    );
    await tester.pump();
    expect(find.text('Undo ready. Follow the board lights.'), findsNothing);
    expect(
      find.text('Follow the board lights for hint guidance.'),
      findsNothing,
    );

    service.emitHomeState(
      sequence: 82,
      action: 'idle',
      undoPending: false,
      fen: '8/8/8/8/8/8/4K3/7k b - - 1 1',
    );
    await tester.pump();
    expect(find.text('Undo cancelled.'), findsNothing);
    expect(
      find.text('Follow the board lights for hint guidance.'),
      findsNothing,
    );
  });

  testWidgets('late Coaching acknowledgement cannot clear newer board Undo', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(
      sequence: 90,
      action: 'idle',
      undoPending: false,
      coachingMask: 0,
    );
    await tester.pump();

    final Finder coaching = find.byKey(
      const ValueKey<String>('control:coaching:p1'),
    );
    await scrollIntoView(tester, coaching);
    await tester.tap(coaching);
    await tester.pump();
    final _ControlCall call = service.controlCalls.single;

    service.emitHomeState(sequence: 91, action: 'progress', undoPending: true);
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);
    service.emitControl(
      call: call,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.idle,
    );
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);
    expect(service.snapshotRequests, 1);
    // The newer Undo snapshot omitted Coaching state. Its late ACK therefore
    // starts bounded Coaching readback; expiry must also preserve board Undo.
    await tester.pump(BoardActionsController.coachingSyncTimeout);
    expect(service.snapshotRequests, 2);
    expect(find.text('Cancel Undo'), findsOneWidget);
    expect(coachingToggled(tester, coaching), isNull);
    expect(_coachingButton(tester, coaching).onPressed, isNull);
    expect(service.controlCalls, hasLength(1));
  });

  testWidgets('minimum-MTU Undo outcomes retain cancel parity', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);

    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    );
    await tester.pump();
    expect(find.text('Undo'), findsOneWidget);
    expect(find.text('Undo cancelled.'), findsNothing);
  });

  testWidgets('disconnect clears inferred Cancel Undo state', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
      actionState: LitchControlActionState.undoPending,
    );
    await tester.pump();
    expect(find.text('Cancel Undo'), findsOneWidget);

    service.connectionController.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    await tester.pump();
    expect(find.text('Undo'), findsOneWidget);
    expect(find.text('Cancel Undo'), findsNothing);
  });

  testWidgets('local interruption can replace a completed request status', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(find.text('Undo ready. Follow the board lights.'), findsNothing);

    service.controlController.add(
      const LitchControlEvent(
        action: LitchControlAction.undo,
        phase: LitchControlPhase.interrupted,
      ),
    );
    await tester.pump();
    expect(
      find.text('Undo was interrupted when the connection changed.'),
      findsOneWidget,
    );
  });

  testWidgets('surfaces rejected noop and timeout states without sticking', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle');
    await tester.pump();

    final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
    await scrollIntoView(tester, undo);
    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.rejected,
      reason: LitchMutationReason.busy,
    );
    await tester.pump();
    expect(
      find.text('The board is busy. Try again in a moment.'),
      findsNothing,
    );
    await tester.pump(const Duration(seconds: 3));
    expect(
      find.text('The board is busy. Try again in a moment.'),
      findsOneWidget,
    );

    await tester.tap(undo);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.noop,
      reason: LitchMutationReason.noHistory,
    );
    await tester.pump();
    expect(find.text('There is no move to undo.'), findsNothing);

    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.timeout,
    );
    await tester.pump();
    expect(find.textContaining('The board did not confirm Hint'), findsNothing);
    await tester.pump(const Duration(seconds: 3));
    expect(
      find.textContaining('The board did not confirm Hint'),
      findsOneWidget,
    );
  });

  testWidgets('send failures use transient connection guidance', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    service.emitHomeState(sequence: 1, action: 'idle');
    await tester.pump();

    final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
    await scrollIntoView(tester, hint);
    await tester.tap(hint);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.sendFailed,
    );
    await tester.pump();

    expect(
      find.text('Hint could not be sent. Check the connection and try again.'),
      findsNothing,
    );
    await tester.pump(const Duration(seconds: 3));
    expect(
      find.text('Hint could not be sent. Check the connection and try again.'),
      findsOneWidget,
    );
    expect(find.textContaining('unavailable on this firmware'), findsNothing);
    final OutlinedButton hintButton = tester.widget<OutlinedButton>(hint);
    expect(hintButton.onPressed, isNotNull);
  });

  testWidgets('Coaching buttons share a row and follow physical snapshots', (
    WidgetTester tester,
  ) async {
    final semantics = tester.ensureSemantics();
    try {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 1);
      await pumpBoardActions(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('control:coaching:p1'),
      );
      final Finder p2 = find.byKey(
        const ValueKey<String>('control:coaching:p2'),
      );
      expect(coachingToggled(tester, p1), isTrue);
      expect(coachingToggled(tester, p2), isFalse);
      expect(find.byType(SwitchListTile), findsNothing);
      final Rect p1Bounds = tester.getRect(p1);
      final Rect p2Bounds = tester.getRect(p2);
      expect(p1Bounds.top, p2Bounds.top);
      expect(p1Bounds.bottom, p2Bounds.bottom);
      expect(p1Bounds.width, p2Bounds.width);
      expect(p2Bounds.left - p1Bounds.right, 8);
      expect(find.byIcon(Icons.school), findsOneWidget);
      expect(find.byIcon(Icons.school_outlined), findsOneWidget);
      final Size buttonSize = tester.getSize(p2);
      expect(buttonSize.height, greaterThanOrEqualTo(48));
      await scrollIntoView(tester, p2);
      await tester.tap(p2);
      await tester.pump();
      expect(service.controlCalls.single.owner, LitchControlOwner.p2);
      expect(service.controlCalls.single.enabled, isTrue);
      expect(coachingToggled(tester, p2), isFalse);
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      expect(
        tester.getSemantics(p2).getSemanticsData().flagsCollection.isEnabled,
        ui.Tristate.isTrue,
      );
      expect(
        tester.getSemantics(p2).getSemanticsData().flagsCollection.isToggled,
        ui.Tristate.isFalse,
      );
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(coachingToggled(tester, p2), isFalse);
      expect(service.snapshotRequests, 1);
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      // A physical O-hold or command completion publishes the same wire state.
      service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 3);
      await tester.pump();
      expect(coachingToggled(tester, p2), isTrue);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await tester.pump();
      expect(coachingToggled(tester, p2), isTrue);
      await tester.tap(p2);
      await tester.pump();
      expect(service.controlCalls.last.enabled, isFalse);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.rejected,
        reason: LitchMutationReason.busy,
      );
      await tester.pump();
      expect(coachingToggled(tester, p2), isTrue);
      expect(tester.getSize(p2), buttonSize);
      // Omitted is unknown, never guessed off or retained across capability loss.
      service.emitHomeState(sequence: 3, action: 'idle');
      await tester.pump();
      expect(find.byType(SwitchListTile), findsNothing);
      expect(coachingToggled(tester, p1), isNull);
      expect(coachingToggled(tester, p2), isNull);
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNull);
      expect(tester.getSize(p2), buttonSize);
      expect(find.text('Unknown'), findsNWidgets(2));
      expect(
        find.byKey(const ValueKey<String>('control:coaching:p2')),
        findsOneWidget,
      );
      service.emitHomeState(sequence: 4, action: 'idle', coachingMask: 2);
      await tester.pump();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(find.byType(SwitchListTile), findsNothing);
      expect(coachingToggled(tester, p1), isNull);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await tester.pump();
      expect(coachingToggled(tester, p1), isFalse);
      expect(coachingToggled(tester, p2), isFalse);
      // Recreating the controls hydrates the retained snapshot without a tap.
      await pumpBoardActions(tester, service);
      expect(coachingToggled(tester, p1), isFalse);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'Coaching readback recovers dropped state and bounds missing replies',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await pumpBoardActions(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('control:coaching:p1'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pump();
      service.onSnapshotRequest = () =>
          service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(service.snapshotRequests, 1);
      expect(coachingToggled(tester, p1), isTrue);
      expect(_coachingButton(tester, p1).onPressed, isNotNull);
      // A snapshot that beats the ACK already supplies authority: no readback.
      await tester.tap(p1);
      await tester.pump();
      service.emitHomeState(sequence: 3, action: 'idle', coachingMask: 0);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expect(service.snapshotRequests, 1);
      service.onSnapshotRequest = null;
      await tester.tap(p1);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.noop,
      );
      await tester.pump();
      expect(service.snapshotRequests, 2);
      expect(_coachingButton(tester, p1).onPressed, isNotNull);
      await tester.pump(BoardActionsController.coachingSyncTimeout);
      expect(find.byType(SwitchListTile), findsNothing);
      expect(coachingToggled(tester, p1), isNull);
      expect(
        find.text('Coaching state is unavailable. Follow the board lights.'),
        findsOneWidget,
      );
      service.emitHomeState(sequence: 4, action: 'idle', coachingMask: 1);
      await tester.pump();
      expect(coachingToggled(tester, p1), isTrue);
      expect(
        find.textContaining('Coaching state is unavailable'),
        findsNothing,
      );
      await tester.tap(p1);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 2);
      await tester.pump(BoardActionsController.coachingSyncTimeout);
      expect(coachingToggled(tester, p1), isFalse);
      expect(
        find.textContaining('Coaching state is unavailable'),
        findsNothing,
      );
    },
  );

  testWidgets(
    'Coaching disables the logical CPU owner and guards submission',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.boardValues['VS_CPU_LEVEL'] = 5;
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 3);
      await pumpBoardActions(tester, service);
      final p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
      final p2 = find.byKey(const ValueKey<String>('control:coaching:p2'));
      final controller = tester
          .widget<HomeBoardActions>(find.byType(HomeBoardActions))
          .controller;
      void expectRoles({required bool cpuIsP1}) {
        expect(
          _coachingButton(tester, p1).onPressed,
          cpuIsP1 ? isNull : isNotNull,
        );
        expect(
          _coachingButton(tester, p2).onPressed,
          cpuIsP1 ? isNotNull : isNull,
        );
        expect(coachingToggled(tester, p1), !cpuIsP1);
        expect(coachingToggled(tester, p2), cpuIsP1);
        expect(controller.coachingEnabled(LitchControlOwner.p1), isTrue);
        expect(controller.coachingEnabled(LitchControlOwner.p2), isTrue);
        final cpu = cpuIsP1 ? p1 : p2;
        final human = cpuIsP1 ? p2 : p1;
        expect(
          tester.getSemantics(cpu).flagsCollection.isEnabled,
          ui.Tristate.isFalse,
        );
        expect(
          tester.getSemantics(human).flagsCollection.isEnabled,
          ui.Tristate.isTrue,
        );
      }

      expectRoles(cpuIsP1: false);
      await controller.dispatch(
        LitchControlAction.coaching,
        owner: LitchControlOwner.p2,
        enabled: false,
      );
      expect(service.controlCalls, isEmpty);
      service.emitSettingValue('VS_CPU_PLAYER', 1);
      await tester.pumpAndSettle();
      expectRoles(cpuIsP1: true);
      await controller.dispatch(
        LitchControlAction.coaching,
        owner: LitchControlOwner.p1,
        enabled: false,
      );
      expect(service.controlCalls, isEmpty);
      for (final key in ['GAME_FIRST_PLAYER', 'BOARD_ROTATE']) {
        service.emitSettingValue(key, key == 'GAME_FIRST_PLAYER' ? 2 : true);
        await tester.pumpAndSettle();
        expectRoles(cpuIsP1: true);
      }
      service.emitSettingValue('VS_CPU_LEVEL', 0);
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNotNull);
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      expect(coachingToggled(tester, p1), isTrue);
      expect(coachingToggled(tester, p2), isTrue);
      expect(service.controlCalls, isEmpty);
    },
    variant: TargetPlatformVariant({
      TargetPlatform.iOS,
      TargetPlatform.android,
    }),
  );

  testWidgets(
    'Coaching role reads cannot cross reconnect or missing ownership',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.boardValues['VS_CPU_LEVEL'] = 5;
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 3);
      await pumpBoardActions(tester, service);
      final p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
      final p2 = find.byKey(const ValueKey<String>('control:coaching:p2'));
      final stale = Completer<Object?>();
      service.deferredValueReads['VS_CPU_PLAYER'] = [stale];
      // Reconnection requires fresh authority even when cache values exist.
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNull);
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();
      expect(coachingToggled(tester, p1), isNull);
      service.boardValues['VS_CPU_PLAYER'] = 1;
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 3);
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      stale.complete(2);
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      service.emitSettingValue('VS_CPU_PLAYER', 0);
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNull);
      expect(coachingToggled(tester, p1), isTrue);
      expect(coachingToggled(tester, p2), isTrue);
      final controller = tester
          .widget<HomeBoardActions>(find.byType(HomeBoardActions))
          .controller;
      await controller.dispatch(
        LitchControlAction.coaching,
        owner: LitchControlOwner.p2,
      );
      expect(service.controlCalls, isEmpty);
      service.emitSettingValue('VS_CPU_PLAYER', 1);
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p2).onPressed, isNotNull);
      // A replacement device must not inherit this board's human permission,
      // even if its connection event has not reached the old controller yet.
      service.activeBoardId = 'board-2';
      await controller.dispatch(
        LitchControlAction.coaching,
        owner: LitchControlOwner.p2,
      );
      expect(service.controlCalls, isEmpty);
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-2',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();
      expect(_coachingButton(tester, p1).onPressed, isNull);
      expect(_coachingButton(tester, p2).onPressed, isNull);
      expect(coachingToggled(tester, p2), isNull);
    },
  );

  testWidgets(
    'Coaching normal feedback stays inside stable buttons without a toast',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await pumpBoardActions(tester, service, textScaler: TextScaler.linear(2));
      final Finder p1 = find.byKey(
        const ValueKey<String>('control:coaching:p1'),
      );
      final Finder p2 = find.byKey(
        const ValueKey<String>('control:coaching:p2'),
      );
      final Finder card = find.byKey(
        const ValueKey<String>('home-board-actions'),
      );
      await scrollIntoView(tester, p1);
      final Size size = tester.getSize(p1);
      final Rect cardBounds = tester.getRect(card);
      final Rect p1Bounds = tester.getRect(p1);
      final Rect p2Bounds = tester.getRect(p2);
      void expectStable() {
        expect(tester.getRect(card), cardBounds);
        expect(tester.getRect(p1), p1Bounds);
        expect(tester.getRect(p2), p2Bounds);
        expect(find.byType(SnackBar), findsNothing);
      }

      expect(coachingToggled(tester, p1), isFalse);
      final pointer = await tester.startGesture(tester.getCenter(p1));
      await tester.pump(const Duration(milliseconds: 16));
      expectStable();
      await pointer.up();
      await tester.pump();
      expectStable();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.accepted,
      );
      await tester.pump(const Duration(milliseconds: 100));
      expectStable();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
      expectStable();
      expect(find.byType(SnackBar), findsNothing);
      expect(coachingToggled(tester, p1), isFalse);
      service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
      await tester.pump();
      await tester.pumpAndSettle();
      expectStable();
      expect(coachingToggled(tester, p1), isTrue);
      expect(tester.getSize(p1), size);
      expect(coachingToggled(tester, p1), isTrue);
      final Finder toast = find.byType(SnackBar);
      expect(
        find.descendant(
          of: toast,
          matching: find.text('P1 coaching on. Watch the board lights.'),
        ),
        findsNothing,
      );
      expect(find.textContaining('Player 1 coaching changed'), findsNothing);
      await tester.pump(const Duration(seconds: 4));
      await tester.pumpAndSettle();
      expect(toast, findsNothing);
      expectStable();
      // Unrelated snapshots and a duplicate ACK cannot replay a confirmation.
      service.emitHomeState(sequence: 3, action: 'idle', coachingMask: 1);
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pumpAndSettle();
      expect(toast, findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('Coaching errors retain recovery guidance without a toast', (
    tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 1);
    await pumpBoardActions(tester, service);
    final p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
    await scrollIntoView(tester, p1);
    for (final phase in [
      LitchControlPhase.rejected,
      LitchControlPhase.sendFailed,
      LitchControlPhase.timeout,
    ]) {
      await tester.tap(p1);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.last,
        phase: phase,
        reason: phase == LitchControlPhase.rejected
            ? LitchMutationReason.busy
            : null,
      );
      await tester.pump();
      final message = switch (phase) {
        LitchControlPhase.rejected =>
          'The board is busy. Try again in a moment.',
        LitchControlPhase.sendFailed =>
          'Player 1 coaching could not be sent. Check the connection and try again.',
        _ =>
          'The board did not confirm Player 1 coaching. Check the board before trying again.',
      };
      expect(find.text(message), findsNothing);
      await tester.pump(const Duration(seconds: 3));
      expect(find.text(message), findsOneWidget);
      expect(coachingToggled(tester, p1), isTrue);
      expect(_coachingButton(tester, p1).onPressed, isNotNull);
      expect(find.byType(SnackBar), findsNothing);
    }
  });

  for (final languageFailure in <bool>[false, true]) {
    testWidgets(
      languageFailure
          ? 'Settings language error survives Coaching across the actual board tabs'
          : 'Settings error survives Coaching across the actual board tabs',
      (WidgetTester tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final service = _FakeLitchBleService()..settingWriteResult = false;
        service.boardValues['GAME_FIRST_PLAYER'] = 1;
        service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
        addTearDown(service.dispose);
        await tester.pumpWidget(
          AppPreferencesScope(
            appearanceMode: LitchAppearanceMode.system,
            onAppearanceModeChanged: (_) async {},
            localePreference: const LitchLocalePreference.fixed(Locale('en')),
            onLocalePreferenceChanged: (_) async =>
                throw StateError('save failed'),
            child: MaterialApp(
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              home: LitchMainScaffold(
                bleService: service,
                boardId: 'board-1',
                boards: const [],
                onDisconnect: () async {},
                initialCraftModeResolved: true,
                loadAppConsolePreference: () async => false,
                firmwarePageBuilder:
                    (service, boardId, show, activity, availability) =>
                        const SizedBox.shrink(),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
        await tester.scrollUntilVisible(
          p1,
          200,
          scrollable: find
              .descendant(
                of: find.byType(HomeScreen),
                matching: find.byType(Scrollable),
              )
              .first,
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey<String>('main-destination-setup')),
        );
        await tester.pumpAndSettle();
        if (languageFailure) {
          final language = find.byKey(const ValueKey<String>('app-language'));
          await scrollIntoView(tester, language);
          await tester.tap(language);
          await tester.pumpAndSettle();
          await tester.tap(find.text('Deutsch').last);
          await tester.pumpAndSettle();
        } else {
          final lightSwitch = find.descendant(
            of: find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
            matching: find.byType(Switch),
          );
          await scrollIntoView(tester, lightSwitch);
          await tester.tap(lightSwitch);
          await tester.pumpAndSettle();
        }
        final error = find.textContaining(
          languageFailure
              ? 'Language changed for now, but could not be saved for next time.'
              : 'The previous value is still shown.',
        );
        expect(error, findsOneWidget);
        await tester.tap(
          find.byKey(const ValueKey<String>('main-destination-play')),
        );
        await tester.pumpAndSettle();
        await tester.tap(p1);
        await tester.pump();
        service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
        service.emitControl(
          call: service.controlCalls.single,
          phase: LitchControlPhase.applied,
        );
        await tester.pumpAndSettle();
        expect(error, findsOneWidget);
        await tester.tap(p1);
        await tester.pump();
        await tester.pump();
        expect(error, findsOneWidget);
        expect(tester.takeException(), isNull);
        await tester.pump(const Duration(seconds: 5));
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byType(SnackBar), findsNothing);
        expect(
          find.text('P1 coaching on. Watch the board lights.'),
          findsNothing,
        );
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('Coaching can act again without waiting for a toast', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
    await pumpBoardActions(tester, service);
    final Finder p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
    await scrollIntoView(tester, p1);
    await tester.tap(p1);
    await tester.pump();
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(coachingToggled(tester, p1), isFalse);
    service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 4));
    await tester.pumpAndSettle();
    expect(find.byType(SnackBar), findsNothing);
    await tester.tap(p1);
    await tester.pump();
    expect(service.controlCalls, hasLength(2));
    expect(service.controlCalls.last.enabled, isFalse);
    expect(tester.takeException(), isNull);
  });

  testWidgets('Legacy coaching stays Unknown without a change toast', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpBoardActions(tester, service);
    final Finder p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
    await scrollIntoView(tester, p1);
    final Size size = tester.getSize(p1);
    await tester.tap(p1);
    await tester.pump();
    expect(service.controlCalls, isEmpty);
    expect(_coachingButton(tester, p1).onPressed, isNull);
    expect(tester.getSemantics(p1).flagsCollection.isToggled, ui.Tristate.none);
    expect(
      tester.getSemantics(p1).flagsCollection.isEnabled,
      ui.Tristate.isFalse,
    );
    expect(
      tester
          .getSemantics(p1)
          .getSemanticsData()
          .hasAction(ui.SemanticsAction.tap),
      isFalse,
    );
    await tester.pump();
    await tester.pumpAndSettle();
    expect(coachingToggled(tester, p1), isNull);
    expect(tester.getSize(p1), size);
    expect(find.text('Unknown'), findsNWidgets(2));
    expect(
      find.descendant(
        of: find.byType(SnackBar),
        matching: find.text('P1 coaching changed. Check the board lights.'),
      ),
      findsNothing,
    );
    expect(
      find.textContaining('The board lights show whether it is on'),
      findsNothing,
    );
    await tester.pump(const Duration(seconds: 4));
    await tester.pumpAndSettle();
    expect(find.byType(SnackBar), findsNothing);
    await pumpBoardActions(tester, service);
    expect(find.byType(SnackBar), findsNothing);
  });

  testWidgets(
    'coaching sends strict owner and New Game requires confirmation',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await pumpBoardActions(tester, service);

      final Finder p1 = find.byKey(
        const ValueKey<String>('control:coaching:p1'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pump();
      expect(service.controlCalls.last.action, LitchControlAction.coaching);
      expect(service.controlCalls.last.owner, LitchControlOwner.p1);
      expect(service.controlCalls.last.enabled, isTrue);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
      );
      service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
      await tester.pump();

      final Finder p2 = find.byKey(
        const ValueKey<String>('control:coaching:p2'),
      );
      await scrollIntoView(tester, p2);
      await tester.tap(p2);
      await tester.pump();
      expect(service.controlCalls.last.owner, LitchControlOwner.p2);
      expect(service.controlCalls.last.enabled, isTrue);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
        reason: LitchMutationReason.none,
      );
      service.emitHomeState(sequence: 3, action: 'idle', coachingMask: 3);
      await tester.pump();

      final int beforeNewGame = service.controlCalls.length;
      final Finder newGame = find.byKey(
        const ValueKey<String>('control:new-game'),
      );
      await scrollIntoView(tester, newGame);
      await tester.tap(newGame);
      await tester.pumpAndSettle();
      expect(find.text('Start a new game?'), findsOneWidget);
      expect(service.controlCalls, hasLength(beforeNewGame));

      await tester.tap(find.text('Cancel'));
      await tester.pumpAndSettle();
      expect(service.controlCalls, hasLength(beforeNewGame));

      await scrollIntoView(tester, newGame);
      await tester.tap(newGame);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Start new game'));
      await tester.pump();
      expect(service.controlCalls, hasLength(beforeNewGame + 1));
      expect(service.controlCalls.last.action, LitchControlAction.newGame);
    },
  );

  testWidgets(
    'Board Actions guard New game submission and retire stale confirmation',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle');
      await pumpBoardActions(tester, service);
      final newGame = find.byKey(const ValueKey('control:new-game'));
      await scrollIntoView(tester, newGame);
      await tester.tap(newGame);
      await tester.pumpAndSettle();
      expect(find.byType(AlertDialog), findsOneWidget);
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      service.emitHomeState(sequence: 1, action: 'idle');
      await tester.pump();
      await tester.tap(find.text('Start new game'));
      await tester.pumpAndSettle();
      expect(
        service.controlCalls,
        isEmpty,
        reason: 'Confirmation belongs to the session in which it was opened.',
      );
      await tester.tap(newGame);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Start new game'));
      await tester.pumpAndSettle();
      expect(service.controlCalls, hasLength(1));
      expect(service.controlCalls.single.action, LitchControlAction.newGame);
      await tester.tap(newGame);
      await tester.tap(newGame);
      await tester.pump();
      expect(find.byType(AlertDialog), findsNothing);
      expect(service.controlCalls, hasLength(1));
      expect(tester.widget<OutlinedButton>(newGame).onPressed, isNotNull);
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
      );
      await tester.pump();
    },
  );

  testWidgets('Board Actions serialize distinct taps and coalesce duplicates', (
    tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
    await pumpBoardActions(tester, service);
    final p1 = find.byKey(const ValueKey('control:coaching:p1'));
    final p2 = find.byKey(const ValueKey('control:coaching:p2'));
    await scrollIntoView(tester, p1);
    await tester.tap(p1);
    await tester.pump();
    await tester.tap(p1);
    await tester.tap(p2);
    await tester.tap(p2);
    await tester.pump();
    expect(service.controlCalls, hasLength(1));
    for (final key in [
      'hint',
      'undo',
      'new-game',
      'coaching:p1',
      'coaching:p2',
    ]) {
      final control = find.byKey(ValueKey('control:$key'));
      expect(
        key.startsWith('coaching:')
            ? _coachingButton(tester, control).onPressed
            : tester.widget<OutlinedButton>(control).onPressed,
        isNotNull,
        reason: 'Pending transport does not change semantic availability.',
      );
    }
    expect(
      find.descendant(
        of: find.byType(HomeBoardActions),
        matching: find.byType(CircularProgressIndicator),
      ),
      findsNothing,
    );
    service.emitControl(
      call: service.controlCalls.single,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(
      service.controlCalls,
      hasLength(1),
      reason: 'P2 waits for P1 readback ownership.',
    );
    service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
    await tester.pump();
    expect(service.controlCalls, hasLength(2));
    expect(service.controlCalls.last.owner, LitchControlOwner.p2);
    expect(service.controlCalls.last.enabled, isTrue);
    service.emitHomeState(sequence: 3, action: 'idle', coachingMask: 3);
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(service.controlCalls, hasLength(2));
    expect(service.supportSessionDiagnostics.controlIntents, 2);
    expect(service.supportSessionDiagnostics.controlCoalescedTaps, 2);

    await tester.tap(p1);
    await tester.tap(p2);
    await tester.pump();
    service.emitSettingValue('VS_CPU_LEVEL', 4);
    service.emitSettingValue('VS_CPU_PLAYER', 2);
    await tester.pump();
    service.emitHomeState(sequence: 4, action: 'idle', coachingMask: 2);
    service.emitControl(
      call: service.controlCalls.last,
      phase: LitchControlPhase.applied,
    );
    await tester.pump();
    expect(
      service.controlCalls,
      hasLength(3),
      reason: 'Queued P2 intent is revalidated against CPU ownership.',
    );
    expect(_coachingButton(tester, p2).onPressed, isNull);
  });

  for (final hintAlreadyActive in [false, true]) {
    testWidgets(
      'Board Actions preserve a newer-position Hint while '
      '${hintAlreadyActive ? 'the old Hint is active' : 'the old Hint is queued'}',
      (tester) async {
        final service = _FakeLitchBleService();
        addTearDown(service.dispose);
        service.emitHomeState(
          sequence: 1,
          action: 'idle',
          coachingMask: 0,
          assistance: '2o',
        );
        await pumpBoardActions(tester, service);
        final hint = find.byKey(const ValueKey('control:hint'));
        final p1 = find.byKey(const ValueKey('control:coaching:p1'));
        if (!hintAlreadyActive) {
          await scrollIntoView(tester, p1);
          await tester.tap(p1);
          await tester.pump();
        }
        await scrollIntoView(tester, hint);
        await tester.tap(hint);
        await tester.pump();
        expect(service.controlCalls, hasLength(1));

        const nextFen =
            'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1';
        service.emitHomeState(
          sequence: 2,
          action: 'idle',
          coachingMask: 0,
          assistance: '2o',
          fen: nextFen,
        );
        await tester.pump();
        await tester.tap(hint);
        await tester.pump();
        expect(service.controlCalls, hasLength(1));
        // The older action ends without applying Hint to the new position.
        // The later pointer tap must remain a distinct, current-position intent.
        if (hintAlreadyActive) {
          service.emitControl(
            call: service.controlCalls.single,
            phase: LitchControlPhase.noop,
          );
        } else {
          service.emitHomeState(
            sequence: 3,
            action: 'idle',
            coachingMask: 1,
            assistance: '2o',
            fen: nextFen,
          );
          service.emitControl(
            call: service.controlCalls.single,
            phase: LitchControlPhase.applied,
          );
        }
        await tester.pump();
        expect(service.controlCalls, hasLength(2));
        expect(service.controlCalls.last.action, LitchControlAction.hint);
        expect(service.supportSessionDiagnostics.controlCoalescedTaps, 0);
        service.emitControl(
          call: service.controlCalls.last,
          phase: LitchControlPhase.noop,
        );
        await tester.pump();
      },
    );
  }

  testWidgets(
    'Board Actions distinguish duplicate Undo from intentional Cancel Undo',
    (tester) async {
      final trace = _SupportActionSink();
      final service = _FakeLitchBleService(appTrace: trace);
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', undoPending: false);
      await pumpBoardActions(tester, service);
      final undo = find.byKey(const ValueKey('control:undo'));
      await scrollIntoView(tester, undo);
      await tester.tap(undo);
      await tester.tap(undo);
      await tester.pump();
      expect(service.controlCalls, hasLength(1));
      expect(
        trace.userActions.actions,
        hasLength(1),
        reason: 'duplicate taps are one human intent',
      );
      service.emitHomeState(sequence: 2, action: 'progress', undoPending: true);
      await tester.pump();
      expect(find.text('Cancel Undo'), findsOneWidget);
      await tester.tap(undo);
      await tester.pump();
      expect(service.controlCalls, hasLength(1));
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.applied,
        actionState: LitchControlActionState.undoPending,
      );
      await tester.pump();
      expect(service.controlCalls, hasLength(2));
      service.emitHomeState(sequence: 3, action: 'idle', undoPending: false);
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.applied,
        actionState: LitchControlActionState.idle,
      );
      await tester.pump();
      expect(trace.userActions.actions.map((a) => a.kind), [
        SupportActionKind.undo,
        SupportActionKind.undo,
      ]);
      expect(
        trace.userActions.actions.every(
          (a) => a.outcome == SupportActionOutcome.applied,
        ),
        isTrue,
      );
      expect(find.text('Undo'), findsOneWidget);
      expect(service.controlCalls, hasLength(2));
    },
  );

  testWidgets(
    'Board Actions quietly reconcile a lost ACK with two reads and no mutation replay',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', coachingMask: 0);
      await pumpBoardActions(tester, service);
      final p1 = find.byKey(const ValueKey('control:coaching:p1'));
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.timeout,
      );
      await tester.pump();
      expect(service.snapshotRequests, 1);
      expect(find.textContaining('unconfirmed'), findsNothing);
      expect(_coachingButton(tester, p1).onPressed, isNotNull);
      await tester.pump(const Duration(milliseconds: 1500));
      expect(service.snapshotRequests, 2);
      service.emitHomeState(sequence: 2, action: 'idle', coachingMask: 1);
      await tester.pump();
      await tester.pump(const Duration(seconds: 4));
      expect(coachingToggled(tester, p1), isTrue);
      expect(service.controlCalls, hasLength(1));
      expect(service.snapshotRequests, 2);
      expect(service.supportSessionDiagnostics.controlRecoveryConfirmed, 1);
      expect(service.supportSessionDiagnostics.controlRecoveryUnconfirmed, 0);
      expect(find.textContaining('unconfirmed'), findsNothing);
      expect(find.byType(SnackBar), findsNothing);
    },
  );

  testWidgets(
    'Board Actions retry a definite Busy once after fresh authority',
    (tester) async {
      final trace = _SupportActionSink();
      final service = _FakeLitchBleService(appTrace: trace);
      addTearDown(service.dispose);
      service.emitHomeState(sequence: 1, action: 'idle', assistance: '2o');
      await pumpBoardActions(tester, service);
      final hint = find.byKey(const ValueKey('control:hint'));
      await scrollIntoView(tester, hint);
      await tester.tap(hint);
      await tester.pump();
      service.emitControl(
        call: service.controlCalls.single,
        phase: LitchControlPhase.rejected,
        reason: LitchMutationReason.busy,
      );
      await tester.pump();
      expect(
        find.text('The board is busy. Try again in a moment.'),
        findsNothing,
      );
      service.emitHomeState(sequence: 2, action: 'idle', assistance: '2o');
      await tester.pump(const Duration(milliseconds: 200));
      expect(service.controlCalls, hasLength(2));
      service.emitControl(
        call: service.controlCalls.last,
        phase: LitchControlPhase.rejected,
        reason: LitchMutationReason.busy,
      );
      await tester.pump();
      expect(
        find.text('The board is busy. Try again in a moment.'),
        findsOneWidget,
      );
      await tester.pump(const Duration(seconds: 4));
      expect(service.controlCalls, hasLength(2));
      expect(service.supportSessionDiagnostics.controlBusyRetries, 1);
      expect(
        trace.userActions.actions,
        hasLength(1),
        reason: 'readback and Busy retry update the original intent',
      );
      expect(trace.userActions.actions.single.kind, SupportActionKind.hint);
      expect(
        trace.userActions.actions.single.outcome,
        SupportActionOutcome.rejected,
      );
    },
  );

  testWidgets('Main Setup search covers advanced metadata and future types', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpSettings(tester, service);

    expect(find.text('A future experimental control'), findsNothing);
    await revealAdvanced(tester);
    expect(find.text('Experimental Tools'), findsOneWidget);
    expect(find.text('A future experimental control'), findsOneWidget);
    expect(find.byType(ExpansionTile), findsNothing);

    await enterSettingsSearch(tester, 'future_switch_mode');
    expect(find.text('A future experimental control'), findsOneWidget);
    expect(find.text('FUTURE_SWITCH_MODE'), findsNothing);
    expect(find.byType(TextFormField), findsOneWidget);

    final Finder clear = find.byKey(
      const ValueKey<String>('advanced-search-clear'),
    );
    expect(clear, findsOneWidget);
    expect(tester.getSize(clear).height, greaterThanOrEqualTo(48));
    await tester.tap(clear);
    await tester.pumpAndSettle();
    expect(find.text('A future experimental control'), findsNothing);
    expect(find.byKey(const ValueKey('setup-common-controls')), findsOneWidget);
    expect(
      tester
          .widget<TextField>(find.byKey(const ValueKey('advanced-search')))
          .controller!
          .text,
      isEmpty,
    );

    await enterSettingsSearch(tester, 'experimental tools');
    expect(find.text('A future experimental control'), findsOneWidget);
    await enterSettingsSearch(tester, 'future experimental control');
    expect(find.text('A future experimental control'), findsOneWidget);
    await enterSettingsSearch(tester, 'nothing matches this');
    expect(find.text('No settings match your search.'), findsOneWidget);
    expect(find.byTooltip('Clear settings search'), findsOneWidget);

    await enterSettingsSearch(tester, 'rgbw_cool');
    expect(find.text('RGBW_COOL_FUTURE'), findsNothing);
    expect(find.text('No settings match your search.'), findsOneWidget);
    await enterSettingsSearch(tester, 'usb_cc');
    expect(find.text('USB_CC_TEST_VALUE'), findsNothing);
  });

  testWidgets(
    'retired hardware controls are absent for every hardware identity at 200% text',
    (WidgetTester tester) async {
      const LitchSetting emulation = LitchSetting(
        key: 'V3_BUTTON_LED_EMULATION',
        type: 'bool',
        category: 'display',
        value: true,
        defaultValue: true,
        description: 'Show X/O button cues on V3 edge LEDs',
      );
      for (final revision in LitchHardwareRevision.values) {
        final service =
            _FakeLitchBleService(
                settings: const <LitchSetting>[
                  emulation,
                  LitchSetting(
                    key: 'P1_X_DEEP_WAKE_ENABLED',
                    type: 'bool',
                    category: 'hardware',
                    value: false,
                    defaultValue: true,
                    description: 'Experimental S3 GPIO0 deep wake',
                  ),
                  LitchSetting(
                    key: 'P1_X_WAKE_HOLD_MS',
                    type: 'u32',
                    category: 'hardware',
                    value: 1000,
                    defaultValue: 1000,
                    min: 250,
                    max: 5000,
                    description: 'GPIO0 release grace',
                  ),
                  _placementEffectSetting,
                ],
              )
              ..capabilities = LitchDeviceCapabilities(
                contractVersion: 3,
                hardwareRevision: revision,
                capabilityMask: LitchDeviceCapabilities.settingsBit,
                explicitlyReported: true,
              );
        addTearDown(service.dispose);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        await pumpSettings(
          tester,
          service,
          textScaler: const TextScaler.linear(2),
        );
        await revealAdvanced(tester);
        for (final key in <String>[
          'V3_BUTTON_LED_EMULATION',
          'P1_X_DEEP_WAKE_ENABLED',
          'P1_X_WAKE_HOLD_MS',
        ]) {
          await enterSettingsSearch(tester, key);
          expect(
            find.byKey(ValueKey<String>('setting-editor:$key')),
            findsNothing,
          );
          expect(find.text('No settings match your search.'), findsOneWidget);
        }
        expect(service.settingWrites, isEmpty);
        expect(tester.takeException(), isNull);
      }
    },
  );

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    for (final scale in <double>[1, 2]) {
      testWidgets(
        'Promotion choices ${platform.name} at ${scale}x acknowledge and recover',
        (tester) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          await tester.binding.setSurfaceSize(const Size(390, 844));
          addTearDown(() => tester.binding.setSurfaceSize(null));
          final service = _FakeLitchBleService(
            settings: <LitchSetting>[
              ..._defaultSettings,
              const LitchSetting(
                key: 'PROMOTION_MODE',
                type: 'enum',
                category: 'behavior',
                value: 'queen',
                defaultValue: 'queen',
                options: <String>['queen', 'choose'],
              ),
            ],
          )..settingWriteCompleter = Completer<bool>();
          addTearDown(service.dispose);
          final semantics = tester.ensureSemantics();
          try {
            await tester.pumpWidget(
              harness(
                service,
                textScaler: TextScaler.linear(scale),
                targetPlatform: platform,
              ),
            );
            await tester.pumpAndSettle();
            await scrollIntoView(
              tester,
              find.byKey(const ValueKey('setup-open:behavior')),
            );
            await tester.tap(find.byKey(const ValueKey('setup-open:behavior')));
            await tester.pumpAndSettle();
            final editor = find.byKey(
              const ValueKey('setting-editor:PROMOTION_MODE'),
            );
            await scrollIntoView(tester, editor);
            expect(find.text('Promotion'), findsOneWidget);
            final choose = find.descendant(
              of: editor,
              matching: find.text('Choose'),
            );
            final queen = find.descendant(
              of: editor,
              matching: find.text('Queen'),
            );
            expect(choose, findsOneWidget);
            expect(queen, findsOneWidget);
            if (scale == 1) {
              final segmented = find.descendant(
                of: editor,
                matching: find.byType(SettingsChoicePills<String>),
                matchRoot: true,
              );
              expect(
                tester.widget<SettingsChoicePills<String>>(segmented).selected,
                <String>{'queen'},
              );
              expect(
                tester.getSize(segmented).height,
                greaterThanOrEqualTo(48),
              );
            } else {
              final option = _choiceSegment(
                find.byKey(const ValueKey('promotion-mode:queen')),
              );
              expect(tester.getSize(option).height, greaterThanOrEqualTo(48));
              expect(
                tester.getSemantics(
                  find
                      .ancestor(
                        of: option,
                        matching: find.byType(MergeSemantics),
                      )
                      .first,
                ),
                isSemantics(
                  isButton: true,
                  isSelected: true,
                  isInMutuallyExclusiveGroup: true,
                  hasEnabledState: true,
                  isEnabled: true,
                  hasTapAction: true,
                  hasFocusAction: true,
                  isFocusable: true,
                  label: 'Queen',
                ),
              );
            }
            await captureHealthReview(
              tester,
              'promotion-${platform.name}-${scale.toInt()}x',
            );
            await tester.tap(choose);
            await tester.pump();
            expect(service.settingWrites.single.key, 'PROMOTION_MODE');
            expect(service.settingWrites.single.value, 'choose');
            expect(service.settingWrites.single.awaitAck, isTrue);
            service.settingWriteCompleter!.complete(false);
            await tester.pumpAndSettle();
            if (scale == 1) {
              expect(
                tester
                    .widget<SettingsChoicePills<String>>(
                      find.descendant(
                        of: editor,
                        matching: find.byType(SettingsChoicePills<String>),
                        matchRoot: true,
                      ),
                    )
                    .selected,
                <String>{'queen'},
              );
            } else {
              expect(
                tester
                    .widget<SettingsChoicePills<String>>(
                      find.descendant(
                        of: editor,
                        matching: find.byType(SettingsChoicePills<String>),
                        matchRoot: true,
                      ),
                    )
                    .selected
                    .single,
                'queen',
              );
            }
            service.settingWriteCompleter = null;
            await tester.pump(const Duration(seconds: 5));
            await tester.pumpAndSettle();
            await scrollIntoView(tester, choose);
            await tester.tap(choose);
            await tester.pumpAndSettle();
            expect(service.boardValues['PROMOTION_MODE'], 'choose');
            expect(service.settingWrites, hasLength(2));
            expect(tester.takeException(), isNull);
          } finally {
            semantics.dispose();
          }
        },
      );
    }
  }

  testWidgets(
    'Advanced Settings preserves move/takeback order and empty state',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      await revealAdvanced(tester);
      await enterSettingsSearch(tester, 'behavior');

      final Finder moveConfirm = find.text('Confirm moves');
      final Finder takeback = find.text('Takeback grace period');
      expect(moveConfirm, findsOneWidget);
      expect(takeback, findsOneWidget);
      expect(
        tester.getTopLeft(moveConfirm).dy,
        lessThan(tester.getTopLeft(takeback).dy),
      );

      final _FakeLitchBleService emptyService = _FakeLitchBleService(
        settings: const <LitchSetting>[],
      );
      addTearDown(emptyService.dispose);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      await pumpSettings(tester, emptyService);
      await revealAdvanced(tester);
      expect(
        find.text('No settings were reported by this board.'),
        findsOneWidget,
      );
    },
  );

  testWidgets('Setup guidance metadata is searchable and accessible', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    final semantics = tester.ensureSemantics();
    try {
      await pumpSettings(
        tester,
        service,
        textScaler: const TextScaler.linear(2),
      );
      await revealAdvanced(tester);
      await enterSettingsSearch(tester, 'board setup');

      expect(find.text('Behavior'), findsOneWidget);
      expect(find.text('Setup guidance'), findsOneWidget);
      expect(find.text('BOARD_SETUP_ASSISTANCE'), findsNothing);
      expect(
        find.text(
          'Shows optional king-location guidance during setup. Required correction and safety feedback stay on.',
        ),
        findsOneWidget,
      );

      final Finder toggle = find.byKey(
        const ValueKey<String>('setting-editor:BOARD_SETUP_ASSISTANCE'),
      );
      await scrollIntoView(tester, toggle);
      expect(tester.getSize(toggle).height, greaterThanOrEqualTo(48));
      expect(tester.getSize(toggle).width, greaterThanOrEqualTo(48));
      expect(tester.widget<Switch>(toggle).value, isTrue);
      expect(
        tester.getSemantics(toggle).label,
        allOf(
          contains('Setup guidance'),
          contains('Required correction and safety feedback stay on.'),
        ),
      );
      expect(
        tester.getSemantics(toggle),
        isSemantics(
          hasToggledState: true,
          isToggled: true,
          hasEnabledState: true,
          isEnabled: true,
          hasTapAction: true,
        ),
      );
      expect(tester.takeException(), isNull);

      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'BOARD_SETUP_ASSISTANCE',
        'v': false,
      });
      await tester.pumpAndSettle();
      expect(tester.widget<Switch>(toggle).value, isFalse);

      service.boardValues['BOARD_SETUP_ASSISTANCE'] = true;
      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'BOARD_SETUP_ASSISTANCE',
        'r': 1,
      });
      await tester.pumpAndSettle();
      expect(tester.widget<Switch>(toggle).value, isTrue);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'Setup guidance awaits acknowledgement and rolls back rejection',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService()
        ..settingWriteCompleter = Completer<bool>();
      addTearDown(service.dispose);
      final semantics = tester.ensureSemantics();
      try {
        await pumpSettings(tester, service);
        await revealAdvanced(tester);
        await enterSettingsSearch(tester, 'BOARD_SETUP_ASSISTANCE');

        final Finder toggle = find.byKey(
          const ValueKey<String>('setting-editor:BOARD_SETUP_ASSISTANCE'),
        );
        await scrollIntoView(tester, toggle);
        await tester.tap(toggle);
        await tester.pump();

        expect(service.settingWrites, hasLength(1));
        expect(service.settingWrites.single.key, 'BOARD_SETUP_ASSISTANCE');
        expect(service.settingWrites.single.value, isFalse);
        expect(service.settingWrites.single.awaitAck, isTrue);
        expect(tester.widget<Switch>(toggle).value, isFalse);
        expect(tester.widget<Switch>(toggle).onChanged, isNotNull);
        expect(
          tester.getSemantics(toggle),
          isSemantics(
            hasToggledState: true,
            isToggled: false,
            hasEnabledState: true,
            isEnabled: true,
          ),
        );

        service.settingWriteCompleter!.complete(true);
        await tester.pumpAndSettle();
        expect(tester.widget<Switch>(toggle).value, isFalse);
        expect(tester.widget<Switch>(toggle).onChanged, isNotNull);

        service.acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
          const LitchSettingOutcome(
            requestId: 2,
            key: 'BOARD_SETUP_ASSISTANCE',
            ok: false,
            reason: LitchMutationReason.busy,
            hasValue: false,
            isCompact: true,
          ),
        );
        await tester.tap(toggle);
        await tester.pumpAndSettle();

        expect(service.settingWrites, hasLength(2));
        expect(service.settingWrites.last.key, 'BOARD_SETUP_ASSISTANCE');
        expect(service.settingWrites.last.value, isTrue);
        expect(service.settingWrites.last.awaitAck, isTrue);
        expect(tester.widget<Switch>(toggle).value, isFalse);
        expect(
          find.text(
            'The board is busy; try again. The previous value is still shown.',
          ),
          findsOneWidget,
        );
      } finally {
        semantics.dispose();
      }
    },
  );

  test(
    'player hue help never names the removed commit button in app locales',
    () async {
      for (final locale in [
        const Locale('en'),
        const Locale('en', 'GB'),
        const Locale('es'),
        const Locale('de'),
        const Locale('fr'),
        const Locale('it'),
      ]) {
        final strings = await AppLocalizations.delegate.load(locale);
        expect(
          strings.colorPreviewHelp,
          isNot(contains(strings.craftUseColor)),
          reason:
              '$locale must describe live hue changes, not a removed button',
        );
      }
    },
  );

  for (final player in ['P1', 'P2']) {
    testWidgets(
      '$player hue drag changes the board live and confirms its final value',
      (tester) async {
        final service = _FakeLitchBleService();
        addTearDown(service.dispose);
        await pumpSettings(tester, service);
        final control = find.byKey(ValueKey('quick:${player}_PIECE_WHEEL'));
        await scrollIntoView(tester, control);
        await tester.tap(control);
        await tester.pumpAndSettle();
        final hue = find.byKey(
          ValueKey('piece-color-hue:${player}_PIECE_WHEEL'),
        );
        expect(hue, findsOneWidget);
        bool selected(int wheel) => tester
            .widget<Semantics>(
              find
                  .descendant(
                    of: find.byKey(
                      ValueKey(
                        'piece-color-choice:${player}_PIECE_WHEEL:$wheel',
                      ),
                    ),
                    matching: find.byType(Semantics),
                  )
                  .first,
            )
            .properties
            .selected!;
        expect(selected(player == 'P1' ? 170 : 0), isTrue);
        tester.widget<Slider>(hue).onChanged!(85 / 255 * 360);
        await tester.pump();
        expect(
          selected(85),
          isTrue,
          reason: 'An exact preset hue selects its draft semantics',
        );
        expect(
          selected(player == 'P1' ? 170 : 0),
          isFalse,
          reason: 'The original board value does not retain a stale check',
        );
        expect(
          find.descendant(
            of: find.byType(BottomSheet),
            matching: find.byType(Slider),
          ),
          findsOneWidget,
          reason:
              'Player-end colors expose hue only, never saturation or brightness',
        );
        final rect = tester.getRect(hue);
        expect(rect.height, greaterThanOrEqualTo(48));
        final gesture = await tester.startGesture(
          Offset(rect.left + 24, rect.center.dy),
        );
        await gesture.moveTo(
          Offset(rect.left + rect.width * .25, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final first = tester.widget<Slider>(hue).value;
        service.consoleController.add([
          'unrelated asynchronous console update',
        ]);
        await tester.pump(const Duration(milliseconds: 100));
        expect(tester.widget<Slider>(hue).value, first);
        expect(tester.getRect(hue), rect);
        await gesture.moveTo(
          Offset(rect.left + rect.width * .4, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final value = tester.widget<Slider>(hue).value;
        expect(value, greaterThan(first));
        final wheel = (value / 360 * 255).round();
        expect(
          litchPieceColorChoices.map((choice) => choice.wheel),
          isNot(contains(wheel)),
        );
        final nearest = litchClosestPieceColorChoice(wheel);
        for (final choice in litchPieceColorChoices) {
          expect(
            selected(choice.wheel),
            choice == nearest,
            reason: 'A custom hue identifies exactly one nearest named color',
          );
        }
        final preview = tester.widget<Container>(
          find.byKey(const ValueKey('piece-color-hue-preview')),
        );
        expect(
          (preview.decoration! as BoxDecoration).color,
          litchWheelPreviewColor(wheel),
        );
        expect(service.settingWrites, isNotEmpty);
        expect(
          service.settingWrites.every((write) => !write.awaitAck),
          isTrue,
          reason: 'intermediate hue previews use the low-latency write path',
        );
        await gesture.up();
        await tester.pumpAndSettle();
        final finalWrite = service.settingWrites.last;
        expect(finalWrite.key, '${player}_PIECE_WHEEL');
        expect(finalWrite.value, wheel);
        expect(finalWrite.awaitAck, isTrue);
      },
    );
  }

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets('player hue sheets remain usable at 200% text in $size', (
      tester,
    ) async {
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final padding = size.width > size.height
          ? const EdgeInsets.fromLTRB(59, 0, 59, 21)
          : const EdgeInsets.fromLTRB(0, 44, 0, 34);
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              textScaler: const TextScaler.linear(2),
              padding: padding,
              viewPadding: padding,
            ),
            child: child!,
          ),
          home: Scaffold(
            body: SettingsScreen(
              bleService: service,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      for (final player in ['P1', 'P2']) {
        final control = find.byKey(ValueKey('quick:${player}_PIECE_WHEEL'));
        await scrollIntoView(tester, control);
        await tester.tap(control);
        await tester.pumpAndSettle();
        for (final key in [
          'piece-color-hue:${player}_PIECE_WHEEL',
          'piece-color-choice:${player}_PIECE_WHEEL:170',
        ]) {
          final finder = find.byKey(ValueKey(key));
          await tester.ensureVisible(finder);
          await tester.pumpAndSettle();
          expect(finder.hitTestable(), findsOneWidget);
          final rect = tester.getRect(finder);
          expect(rect.height, greaterThanOrEqualTo(48));
          expect(rect.left, greaterThanOrEqualTo(padding.left));
          expect(rect.right, lessThanOrEqualTo(size.width - padding.right));
          expect(rect.bottom, lessThanOrEqualTo(size.height - padding.bottom));
          expect(tester.takeException(), isNull);
        }
        Navigator.of(
          tester.element(
            find.byKey(ValueKey('piece-color-hue:${player}_PIECE_WHEEL')),
          ),
        ).pop();
        await tester.pumpAndSettle();
      }
      expect(service.settingWrites, isEmpty);
    });
  }

  testWidgets(
    'blocked hue band snaps to the far edge and changes the board live',
    (tester) async {
      final service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final p1 = find.byKey(const ValueKey('quick:P1_PIECE_WHEEL'));
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      final hue = find.byKey(const ValueKey('piece-color-hue:P1_PIECE_WHEEL'));
      final rect = tester.getRect(hue);
      await tester.tapAt(Offset(rect.left + 16, rect.center.dy));
      await tester.pumpAndSettle();
      expect(tester.widget<Slider>(hue).value, greaterThan(300));
      expect(find.textContaining('Too close'), findsNothing);
      expect(service.settingWrites, isNotEmpty);
      expect(service.settingWrites.last.awaitAck, isTrue);
      Navigator.of(tester.element(hue)).pop();
      await tester.pumpAndSettle();
      expect(service.settingWrites, isNotEmpty);
    },
  );

  testWidgets(
    'player colors are directly accessible on Setup with the same searchable color control',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      final SemanticsHandle semantics = tester.ensureSemantics();
      try {
        await pumpSettings(tester, service);

        await openSetupPage(tester, 'behavior');
        final Finder p1 = find.byKey(
          const ValueKey<String>('quick:P1_PIECE_WHEEL'),
        );
        await scrollIntoView(tester, p1);
        expect(find.text('Board-end colors'), findsNothing);
        expect(
          find.byKey(const ValueKey('setup-common-controls')),
          findsOneWidget,
        );
        expect(find.text('170'), findsNothing);
        expect(find.text('0'), findsNothing);
        expect(tester.getSize(p1).height, greaterThanOrEqualTo(56));
        expect(tester.getSemantics(p1).label, contains('Player 1 color'));
        expect(tester.getSemantics(p1).label, contains('Blue'));
        expect(
          tester.getSemantics(p1).label,
          contains('The on-screen swatch is approximate'),
        );

        await tester.tap(p1);
        await tester.pumpAndSettle();
        expect(find.text('Choose Player 1 color'), findsOneWidget);
        expect(find.textContaining('Move the hue slider'), findsOneWidget);
        expect(
          find.textContaining('Presets apply immediately'),
          findsOneWidget,
        );

        final Finder selectedBlue = find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:170'),
        );
        final Finder unavailableRed = find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:0'),
        );
        final blueSemantics = tester
            .getSemantics(selectedBlue)
            .getSemanticsData();
        final redSemantics = tester
            .getSemantics(unavailableRed)
            .getSemanticsData();
        expect(blueSemantics.flagsCollection.isSelected, ui.Tristate.isTrue);
        expect(redSemantics.hasAction(ui.SemanticsAction.tap), isTrue);
        expect(redSemantics.label, isNot(contains('unavailable')));
        expect(tester.getSize(selectedBlue).height, greaterThanOrEqualTo(56));

        final Finder gold = find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
        );
        await tester.tap(gold);
        await tester.pumpAndSettle();

        final _SettingWrite write = service.settingWrites.singleWhere(
          (_SettingWrite item) => item.key == 'P1_PIECE_WHEEL' && item.awaitAck,
        );
        expect(write.value, 43);
        expect(write.awaitAck, isTrue);
        expect(tester.getSemantics(p1).label, contains('Gold'));
        final swatch = litchWheelPreviewColor(43).toARGB32();
        expect(
          find.descendant(
            of: p1,
            matching: find.byKey(ValueKey('piece-color-swatch:$swatch')),
          ),
          findsOneWidget,
        );

        await revealAdvanced(tester);
        await enterSettingsSearch(tester, 'P1_PIECE_WHEEL');
        expect(
          find.byKey(const ValueKey('quick:P1_PIECE_WHEEL')),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey<String>('setting-editor:P1_PIECE_WHEEL')),
          findsNothing,
        );
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets('bulk arriving during palette load owns final Settings values', (
    tester,
  ) async {
    final board = _PaletteBoard()..brightness = 8;
    final service = _FakeLitchBleService()
      ..deferredEndPalette = Completer<LitchEndPalette?>();
    addTearDown(() => disposeBoard(service, board));
    await tester.pumpWidget(harness(service));
    await tester.pump();
    expect(service.settingsReadDeviceIds, ['board-1']);
    board.deliverSettings(service);
    await tester.pump();
    expect(service.hasBoardStateSnapshot, isTrue);
    service.deferredEndPalette!.complete(
      const LitchEndPalette(
        p1ControlsWheel: 170,
        p2ControlsWheel: 0,
        usesDirectPhysicalKeys: true,
      ),
    );
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('quick:BRIGHTNESS')), findsNothing);
    final p1 = find.byKey(const ValueKey('quick:P1_PIECE_WHEEL'));
    final p2 = find.byKey(const ValueKey('quick:P2_PIECE_WHEEL'));
    await scrollIntoView(tester, p1);
    expect(
      find.descendant(of: p1, matching: find.text('Gold')),
      findsOneWidget,
    );
    expect(
      find.descendant(of: p2, matching: find.text('Violet')),
      findsOneWidget,
    );
    expect(find.textContaining('Couldn’t confirm'), findsNothing);
  });

  testWidgets('Play name swatch reuses Setup picker and canonical end color', (
    WidgetTester tester,
  ) async {
    final board = _PaletteBoard(includeHomeSnapshot: true)
      ..wheel = 170
      ..p2Wheel = 0;
    final _FakeLitchBleService service = _FakeLitchBleService(ble: board)
      ..endPaletteToReturn = const LitchEndPalette(
        p1ControlsWheel: 170,
        p2ControlsWheel: 0,
        usesDirectPhysicalKeys: true,
      );
    addTearDown(() => disposeBoard(service, board));
    service.debugSetConnectedDevice('board-1');
    final opening = service.openConnectionSession('board-1');
    await tester.pump();
    expect((await opening)?.isActivated, isTrue);
    expect(service.productReady, isTrue);
    service.emitHomeState(sequence: 1, action: 'idle', assistance: '3he2e4');
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: LitchMainScaffold(
          bleService: service,
          boardId: 'board-1',
          boards: const [],
          onDisconnect: () async {},
          initialCraftModeResolved: true,
          loadAppConsolePreference: () async => false,
        ),
      ),
    );
    await tester.pumpAndSettle();
    final swatch = find.byKey(const ValueKey('home-player-color-white'));
    final paint = find.byKey(const ValueKey('home-player-color-swatch-white'));
    final name = find.descendant(
      of: find.byKey(const ValueKey('home-player-name-edit-white')),
      matching: find.text('Add name'),
    );
    await tester.ensureVisible(swatch);
    expect(tester.getSize(swatch), const Size(48, 48));
    expect(tester.getSize(paint), const Size(24, 24));
    final before = tester.widget<Text>(name).style!.color;
    Color? swatchColor() =>
        (tester.widget<Container>(paint).decoration as BoxDecoration).color;
    expect(swatchColor(), litchWheelPreviewColor(0));
    board
      ..settingsRevision = 15
      ..p2Wheel = 43
      ..rotated = true
      ..publishSettings();
    await tester.pumpAndSettle();
    expect(tester.widget<Text>(name).style!.color, isNot(before));
    expect(swatchColor(), litchWheelPreviewColor(43));
    final pencil = find.descendant(
      of: find.byKey(const ValueKey('home-player-name-edit-white')),
      matching: find.byIcon(Icons.edit_outlined),
    );
    expect(
      tester.getRect(pencil).left - tester.getRect(name).right,
      closeTo(8, .1),
    );
    expect(
      service.settingsReadDeviceIds,
      isEmpty,
      reason: 'The Setup color owner has not been visited before this tap.',
    );
    await tester.tap(swatch);
    await tester.pumpAndSettle();
    expect(service.settingsReadDeviceIds, ['board-1']);
    expect(find.text('Choose Player 1 color'), findsOneWidget);
    await tester.tap(
      find.byKey(
        const ValueKey<String>('piece-color-choice:P2_PIECE_WHEEL:85'),
      ),
    );
    await tester.pumpAndSettle();

    expect(service.settingWrites, hasLength(1));
    expect(
      service.settingWrites.map((write) => write.key),
      everyElement('P2_END_PIECE_WHEEL'),
    );
    expect(service.settingWrites.map((write) => write.value), everyElement(85));
    expect(service.settingWrites.map((write) => write.awaitAck), [true]);
    board
      ..settingsRevision = 16
      ..p2Wheel = 85
      ..publishSettings();
    await tester.pumpAndSettle();
    expect(service.latestSettingValue('P2_END_PIECE_WHEEL'), 85);
    expect(swatchColor(), litchWheelPreviewColor(85));
    expect(
      swatch.hitTestable(),
      findsOneWidget,
      reason: 'Closing the shared picker returns directly to Play.',
    );
    await tester.pumpWidget(const SizedBox.shrink());
    await service.disconnect();
  });

  for (final first in [1, 2]) {
    for (final player in ['P1', 'P2']) {
      testWidgets(
        '$player color writes use the reported setting with first player $first',
        (WidgetTester tester) async {
          final settings = _defaultSettings.map((setting) {
            return switch (setting.key) {
              'GAME_FIRST_PLAYER' => _withValue(setting, first),
              'P1_PIECE_WHEEL' => _withValue(setting, 42),
              'P2_PIECE_WHEEL' => _withValue(setting, 203),
              _ => setting,
            };
          }).toList();
          final service = _FakeLitchBleService(settings: settings);
          addTearDown(service.dispose);
          await pumpSettings(tester, service);

          final row = find.byKey(ValueKey('quick:${player}_PIECE_WHEEL'));
          await scrollIntoView(tester, row);
          await tester.tap(row);
          await tester.pumpAndSettle();
          await tester.tap(
            find.byKey(
              ValueKey('piece-color-choice:${player}_PIECE_WHEEL:170'),
            ),
          );
          await tester.pumpAndSettle();

          expect(service.settingWrites, hasLength(1));
          expect(
            service.settingWrites.map((write) => write.key),
            everyElement('${player}_PIECE_WHEEL'),
          );
          expect(
            service.settingWrites.map((write) => write.value),
            everyElement(170),
          );
          expect(service.settingWrites.map((write) => write.awaitAck), [true]);
        },
      );
    }
  }

  testWidgets('keeps player color choices above platform navigation', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    const double bottomSystemInset = 34;

    await tester.pumpWidget(
      harness(service, bottomSystemInset: bottomSystemInset),
    );
    await tester.pumpAndSettle();
    final Finder p1 = find.byKey(
      const ValueKey<String>('quick:P1_PIECE_WHEEL'),
    );
    await scrollIntoView(tester, p1);
    await tester.tap(p1);
    await tester.pumpAndSettle();

    final Rect scaffold = tester.getRect(find.byType(Scaffold).first);
    final Rect picker = tester.getRect(
      find.byKey(const ValueKey<String>('piece-color-picker-scroll')),
    );
    expect(
      picker.bottom,
      moreOrLessEquals(scaffold.bottom - bottomSystemInset),
    );
  });

  testWidgets('board-adjusted player color replaces the requested swatch', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 4,
          key: 'P1_PIECE_WHEEL',
          ok: true,
          reason: LitchMutationReason.none,
          hasValue: true,
          value: 85,
          isCompact: false,
        ),
      );
    addTearDown(service.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    try {
      await pumpSettings(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
        ),
      );
      await tester.pumpAndSettle();

      expect(tester.getSemantics(p1).label, contains('Green'));
      expect(find.textContaining('Setting Player 1 color'), findsNothing);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('custom board shade selects its nearest named approximation', (
    WidgetTester tester,
  ) async {
    final List<LitchSetting> settings = _defaultSettings.map((setting) {
      return setting.key == 'P1_PIECE_WHEEL'
          ? _withValue(setting, 42)
          : setting;
    }).toList();
    final _FakeLitchBleService service = _FakeLitchBleService(
      settings: settings,
    );
    addTearDown(service.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    try {
      await pumpSettings(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      expect(tester.getSemantics(p1).label, contains('Gold'));

      await tester.tap(p1);
      await tester.pumpAndSettle();
      final Finder gold = find.byKey(
        const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
      );
      final data = tester.getSemantics(gold).getSemanticsData();
      expect(data.flagsCollection.isSelected, ui.Tristate.isTrue);
      expect(data.label, contains('Gold'));
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('compact player color success reads back board authority', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..endPaletteToReturn = const LitchEndPalette(
        p1ControlsWheel: 85,
        p2ControlsWheel: 0,
        usesDirectPhysicalKeys: false,
      )
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 5,
          key: 'P1_PIECE_WHEEL',
          ok: true,
          reason: LitchMutationReason.none,
          hasValue: false,
          isCompact: true,
        ),
      );
    service.boardValues['P1_PIECE_WHEEL'] = 85;
    addTearDown(service.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    try {
      await pumpSettings(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
        ),
      );
      await tester.pumpAndSettle();

      expect(tester.getSemantics(p1).label, contains('Green'));
      expect(find.textContaining('Setting Player 1 color'), findsNothing);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('rejected color preset restores the confirmed board color', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 6,
          key: 'P1_PIECE_WHEEL',
          ok: false,
          reason: LitchMutationReason.busy,
          hasValue: false,
          isCompact: false,
        ),
      );
    addTearDown(service.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    try {
      await pumpSettings(tester, service);
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
        ),
      );
      await tester.pumpAndSettle();

      expect(service.settingWrites, hasLength(1));
      expect(service.settingWrites.single.awaitAck, isTrue);
      expect(tester.getSemantics(p1).label, contains('Blue'));
      expect(find.byType(SnackBar), findsNothing);
      expect(find.textContaining('Setting Player 1 color'), findsNothing);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'player color preset keeps both ends editable and recovers timeout',
    (WidgetTester tester) async {
      final completion = Completer<LitchSettingWriteResult>();
      final _FakeLitchBleService service = _FakeLitchBleService()
        ..acknowledgedSettingWriteCompleter = completion;
      addTearDown(service.dispose);
      final SemanticsHandle semantics = tester.ensureSemantics();
      try {
        await pumpSettings(tester, service);
        final Finder p1 = find.byKey(
          const ValueKey<String>('quick:P1_PIECE_WHEEL'),
        );
        final Finder p2 = find.byKey(
          const ValueKey<String>('quick:P2_PIECE_WHEEL'),
        );
        await scrollIntoView(tester, p1);
        await tester.tap(p1);
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(
            const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
          ),
        );
        await tester.pumpAndSettle();

        expect(service.settingWrites, hasLength(1));
        expect(service.settingWrites.single.awaitAck, isTrue);
        expect(find.textContaining('Setting Player 1 color'), findsNothing);
        expect(
          tester
              .getSemantics(p1)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isTrue,
        );
        expect(
          tester
              .getSemantics(p2)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isTrue,
        );
        expect(tester.getSemantics(p1).label, contains('Gold'));
        expect(tester.getSemantics(p2).label, isNot(contains('locked')));

        completion.complete(
          const LitchSettingWriteNotAcknowledged(
            LitchSettingWriteFailure.timedOut,
          ),
        );
        await tester.pumpAndSettle();

        expect(tester.getSemantics(p1).label, contains('Blue'));
        expect(find.byType(SnackBar), findsNothing);
        expect(
          tester
              .getSemantics(p1)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isTrue,
        );

        service.emitSettingValue('P1_PIECE_WHEEL', 85);
        await tester.pumpAndSettle();
        expect(tester.getSemantics(p1).label, contains('Green'));
        expect(find.textContaining('Setting Player 1 color'), findsNothing);
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets('board replacement rehydrates player color without stale error', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService oldService = _FakeLitchBleService()
      ..settingWriteResult = false;
    final List<LitchSetting> replacementSettings = _defaultSettings.map((
      setting,
    ) {
      return setting.key == 'P1_PIECE_WHEEL'
          ? _withValue(setting, 85)
          : setting;
    }).toList();
    final _FakeLitchBleService newService = _FakeLitchBleService(
      settings: replacementSettings,
    );
    addTearDown(oldService.dispose);
    addTearDown(newService.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    try {
      await pumpSettings(tester, oldService);
      final Finder p1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, p1);
      await tester.tap(p1);
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:43'),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.textContaining('Setting Player 1 color'), findsNothing);

      await tester.pumpWidget(harness(newService));
      await tester.pumpAndSettle();
      final Finder replacementP1 = find.byKey(
        const ValueKey<String>('quick:P1_PIECE_WHEEL'),
      );
      await scrollIntoView(tester, replacementP1);
      expect(tester.getSemantics(replacementP1).label, contains('Green'));
      expect(find.textContaining('Couldn’t confirm'), findsNothing);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'computer level drag owns its preview while live setting echoes arrive',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      final Finder opponent = find.byKey(
        const ValueKey<String>('quick:VS_CPU_LEVEL'),
      );
      final Finder slider = find.descendant(
        of: opponent,
        matching: find.byType(Slider),
      );
      final Finder computerPlayerVisibility = find.byKey(
        const ValueKey<String>('quick:VS_CPU_PLAYER:visibility'),
      );
      await scrollIntoView(tester, slider);
      final Rect bounds = tester.getRect(slider);
      final TestGesture gesture = await tester.startGesture(
        Offset(bounds.left + 24, bounds.center.dy),
      );

      await gesture.moveTo(
        Offset(bounds.left + (bounds.width * 0.55), bounds.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final double firstPreview = tester.widget<Slider>(slider).value;
      expect(firstPreview, greaterThan(0));

      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'VS_CPU_LEVEL',
        'v': 0,
      });
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, firstPreview);
      expect(find.text('Computer plays'), findsOneWidget);

      await gesture.moveBy(Offset(-(bounds.width * 0.5), 0));
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, 0);
      expect(
        tester.widget<Visibility>(computerPlayerVisibility).visible,
        isFalse,
      );

      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'VS_CPU_LEVEL',
        'v': 10,
      });
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value, 0);
      expect(
        tester.widget<Visibility>(computerPlayerVisibility).visible,
        isFalse,
      );

      await gesture.moveBy(Offset(bounds.width * 2, 0));
      await tester.pump(const Duration(milliseconds: 16));
      final int finalPreview = tester.widget<Slider>(slider).value.round();
      expect(finalPreview, greaterThan(firstPreview));
      expect(find.text('Computer plays'), findsOneWidget);

      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'VS_CPU_LEVEL',
        'v': 0,
      });
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(slider).value.round(), finalPreview);
      expect(find.text('Computer plays'), findsOneWidget);

      await gesture.up();
      await tester.pumpAndSettle();

      expect(find.text('Computer plays'), findsOneWidget);
      expect(service.settingWrites.last.value, finalPreview);
      expect(service.settingWrites.last.awaitAck, isTrue);
    },
  );

  testWidgets('advanced slider keeps its track and scroll position stable', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    await revealAdvanced(tester);
    await enterSettingsSearch(tester, 'TAKEBACK_GRACE_SECONDS');

    final Finder card = find.byKey(
      const ValueKey<String>('setting:TAKEBACK_GRACE_SECONDS'),
    );
    final Finder slider = find.descendant(
      of: card,
      matching: find.byType(Slider),
    );
    await scrollIntoView(tester, slider);
    final Rect initialTrack = tester.getRect(slider);
    final ScrollableState scrollable = tester.state<ScrollableState>(
      find.byType(Scrollable).first,
    );
    final double initialScrollOffset = scrollable.position.pixels;
    final double trackWidth = initialTrack.width - 48;
    final TestGesture gesture = await tester.startGesture(
      Offset(
        initialTrack.left + 24 + (trackWidth * 0.3),
        initialTrack.center.dy,
      ),
    );

    for (final double fraction in <double>[0.55, 0.8, 1]) {
      await gesture.moveTo(
        Offset(
          initialTrack.left + 24 + (trackWidth * fraction),
          initialTrack.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 16));
      service.debugHandleSettingsRwPayload('board-1', <String, Object?>{
        'k': 'TAKEBACK_GRACE_SECONDS',
        'v': 3,
      });
      await tester.pump(const Duration(milliseconds: 16));
      final Rect currentTrack = tester.getRect(slider);
      expect(currentTrack.left, closeTo(initialTrack.left, 0.01));
      expect(currentTrack.width, closeTo(initialTrack.width, 0.01));
      expect(scrollable.position.pixels, closeTo(initialScrollOffset, 0.01));
    }

    await gesture.up();
    await tester.pumpAndSettle();
    expect(service.settingWrites.last.value, 10);
    expect(service.settingWrites.last.awaitAck, isTrue);
  });

  testWidgets('slider completion cannot cross a service replacement', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService oldService = _FakeLitchBleService()
      ..settingWriteCompleter = Completer<bool>();
    final _FakeLitchBleService newService = _FakeLitchBleService(
      settings: const <LitchSetting>[
        LitchSetting(
          key: 'VS_CPU_LEVEL',
          type: 'u32',
          category: 'behavior',
          value: 8,
          defaultValue: 0,
          min: 0,
          max: 11,
        ),
      ],
    );
    addTearDown(oldService.dispose);
    addTearDown(newService.dispose);
    await pumpSettings(tester, oldService);

    final Finder oldOpponent = find.byKey(
      const ValueKey<String>('quick:VS_CPU_LEVEL'),
    );
    final Finder slider = find.descendant(
      of: oldOpponent,
      matching: find.byType(Slider),
    );
    await scrollIntoView(tester, slider);
    await tester.drag(slider, const Offset(80, 0));
    await tester.pump();
    expect(oldService.settingWrites, isNotEmpty);

    await tester.pumpWidget(harness(newService));
    await tester.pumpAndSettle();
    oldService.settingWriteCompleter!.complete(true);
    await tester.pumpAndSettle();

    final Finder newOpponent = find.byKey(
      const ValueKey<String>('quick:VS_CPU_LEVEL'),
    );
    expect(
      find.descendant(
        of: newOpponent,
        matching: find.textContaining('level 8'),
      ),
      findsOneWidget,
    );
    expect(newService.settingWrites, isEmpty);
  });

  testWidgets(
    'light-square slider opens at the 1.0.5 midpoint and keeps eight positions',
    (WidgetTester tester) async {
      final service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          ..._lightSquareAppearanceSettings.where(
            (s) => s.key != 'LIGHT_SQUARE_BRIGHTNESS',
          ),
          const LitchSetting(
            key: 'LIGHT_SQUARE_BRIGHTNESS',
            type: 'u32',
            category: 'display',
            value: 4,
            defaultValue: 4,
            min: 1,
            max: 8,
          ),
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final finder = find.byKey(
        const ValueKey<String>('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
      );
      await scrollIntoView(tester, finder);
      final slider = tester.widget<Slider>(finder);
      expect(slider.divisions, 7);
      expect(slider.value, 4);
      slider.onChangeStart!(4);
      slider.onChanged!(8);
      slider.onChangeEnd!(8);
      await tester.pumpAndSettle();
      expect(service.settingWrites.last.key, 'LIGHT_SQUARE_BRIGHTNESS');
      expect(service.settingWrites.last.value, 8);
    },
  );

  testWidgets(
    'square-light controls keep Off on the existing switch and disclose hue only for Color',
    (WidgetTester tester) async {
      final _FakeLitchBleService oldService = _FakeLitchBleService();
      addTearDown(oldService.dispose);
      await pumpSettings(tester, oldService);
      expect(
        find.byKey(const ValueKey<String>('light-square-appearance')),
        findsNothing,
        reason: 'older firmware keeps the existing light-square switch only',
      );

      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: const <LitchSetting>[
          ..._defaultSettings,
          ..._lightSquareAppearanceSettings,
        ],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(harness(service));
      await tester.pumpAndSettle();

      final Finder appearance = find.byKey(
        const ValueKey<String>('light-square-appearance'),
      );
      await scrollIntoView(tester, appearance);
      final Finder mode = find.byKey(
        const ValueKey<String>('light-square-mode'),
      );
      expect(
        tester.widget<SettingsChoicePills<String>>(mode).selected,
        <String>{'white'},
      );
      expect(
        find.byKey(const ValueKey<String>('light-square-hue')),
        findsNothing,
      );
      expect(
        find.byKey(
          const ValueKey<String>('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
        ),
        findsOneWidget,
      );

      await tester.tap(
        _choiceSegment(find.descendant(of: mode, matching: find.text('Color'))),
      );
      await tester.pumpAndSettle();
      expect(service.settingWrites.single.key, 'LIGHT_SQUARE_MODE');
      expect(service.settingWrites.single.value, 'color');
      expect(service.settingWrites.single.awaitAck, isTrue);
      expect(
        find.byKey(const ValueKey<String>('light-square-hue')),
        findsOneWidget,
      );
      expect(tester.getRect(mode).height, greaterThanOrEqualTo(48));
    },
  );

  testWidgets(
    'square-light hue and brightness previews stay finger-owned and confirm at release',
    (WidgetTester tester) async {
      final List<LitchSetting> settings = <LitchSetting>[
        ..._defaultSettings,
        for (final LitchSetting setting in _lightSquareAppearanceSettings)
          setting.key == 'LIGHT_SQUARE_MODE'
              ? _withValue(setting, 'color')
              : setting,
      ];
      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: settings,
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      final Finder hue = find.byKey(const ValueKey<String>('light-square-hue'));
      await scrollIntoView(tester, hue);
      final Rect hueBounds = tester.getRect(hue);
      final TestGesture hueGesture = await tester.startGesture(
        Offset(hueBounds.left + 24, hueBounds.center.dy),
      );
      await hueGesture.moveTo(
        Offset(hueBounds.left + hueBounds.width * .3, hueBounds.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
      final double firstHue = tester.widget<Slider>(hue).value;
      final firstHueWrites = service.settingWrites
          .where((write) => write.key == 'LIGHT_SQUARE_HUE')
          .toList();
      expect(firstHueWrites, isNotEmpty);
      expect(firstHueWrites.last.value, (firstHue / 360 * 255).round());
      expect(firstHueWrites.every((write) => !write.awaitAck), isTrue);
      service.emitSettingValue('LIGHT_SQUARE_HUE', 3);
      await tester.pump(const Duration(milliseconds: 40));
      expect(tester.widget<Slider>(hue).value, firstHue);
      await hueGesture.moveTo(
        Offset(hueBounds.left + hueBounds.width * .7, hueBounds.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 40));
      final double finalHueDegrees = tester.widget<Slider>(hue).value;
      final int finalHue = (finalHueDegrees / 360 * 255).round();
      expect(finalHueDegrees, greaterThan(firstHue));
      expect(
        service.settingWrites
            .where((write) => write.key == 'LIGHT_SQUARE_HUE')
            .toList(),
        isNotEmpty,
        reason: 'The board follows the held hue through the existing writer.',
      );
      expect(service.settingWrites.last.value, finalHue);
      expect(service.settingWrites.where((write) => write.awaitAck), isEmpty);
      await hueGesture.up();
      await tester.pumpAndSettle();
      final List<_SettingWrite> hueWrites = service.settingWrites
          .where((write) => write.key == 'LIGHT_SQUARE_HUE')
          .toList();
      expect(hueWrites.length, greaterThanOrEqualTo(3));
      expect(hueWrites.last.value, finalHue);
      expect(hueWrites.last.awaitAck, isTrue);

      final Finder brightness = find.byKey(
        const ValueKey<String>('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
      );
      await scrollIntoView(tester, brightness);
      final Rect brightnessBounds = tester.getRect(brightness);
      final TestGesture brightnessGesture = await tester.startGesture(
        Offset(brightnessBounds.right - 24, brightnessBounds.center.dy),
      );
      await brightnessGesture.moveTo(
        Offset(
          brightnessBounds.left + brightnessBounds.width * .55,
          brightnessBounds.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final double firstBrightness = tester.widget<Slider>(brightness).value;
      service.emitSettingValue('LIGHT_SQUARE_BRIGHTNESS', 8);
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(brightness).value, firstBrightness);
      await brightnessGesture.moveTo(
        Offset(
          brightnessBounds.left + brightnessBounds.width * .3,
          brightnessBounds.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 40));
      final int finalBrightness = tester
          .widget<Slider>(brightness)
          .value
          .round();
      expect(finalBrightness, lessThan(firstBrightness));
      final previewWrites = service.settingWrites
          .where((write) => write.key == 'LIGHT_SQUARE_BRIGHTNESS')
          .toList();
      expect(
        previewWrites,
        isNotEmpty,
        reason:
            'the board receives brightness while the real drag is still held',
      );
      expect(previewWrites.last.value, finalBrightness);
      expect(previewWrites.every((write) => !write.awaitAck), isTrue);
      await brightnessGesture.up();
      await tester.pumpAndSettle();
      final List<_SettingWrite> brightnessWrites = service.settingWrites
          .where((write) => write.key == 'LIGHT_SQUARE_BRIGHTNESS')
          .toList();
      expect(brightnessWrites.length, greaterThan(previewWrites.length));
      expect(brightnessWrites.last.value, finalBrightness);
      expect(brightnessWrites.last.awaitAck, isTrue);
    },
  );

  testWidgets(
    'square-light controls remain adjustable while mode acknowledgement is pending',
    (WidgetTester tester) async {
      final SemanticsHandle semantics = tester.ensureSemantics();
      final _FakeLitchBleService service =
          _FakeLitchBleService(
              settings: const <LitchSetting>[
                ..._defaultSettings,
                ..._lightSquareAppearanceSettings,
              ],
            )
            ..acknowledgedSettingWriteCompleter =
                Completer<LitchSettingWriteResult>();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final Finder mode = find.byKey(
        const ValueKey<String>('light-square-mode'),
      );
      await scrollIntoView(tester, mode);
      final Finder brightness = find.byKey(
        const ValueKey<String>('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
      );
      void expectBrightnessSemantics({required bool enabled}) {
        final data = tester.getSemantics(brightness).getSemanticsData();
        expect(data.label, contains('Light-square LEDs, Brightness'));
        expect(data.value, '8');
        expect(
          data.flagsCollection.isEnabled,
          enabled ? ui.Tristate.isTrue : ui.Tristate.isFalse,
        );
        expect(data.hasAction(ui.SemanticsAction.increase), enabled);
        expect(data.hasAction(ui.SemanticsAction.decrease), enabled);
      }

      expectBrightnessSemantics(enabled: true);
      await tester.tap(
        _choiceSegment(find.descendant(of: mode, matching: find.text('Color'))),
      );
      await tester.pump();

      expectBrightnessSemantics(enabled: true);

      expect(
        find.descendant(
          of: find.byKey(const ValueKey<String>('light-square-appearance')),
          matching: find.byType(LinearProgressIndicator),
        ),
        findsNothing,
        reason:
            'ordinary Setup edits retain local preview without a visible synchronization wait',
      );
      expect(
        tester.widget<SettingsChoicePills<String>>(mode).onSelectionChanged,
        isNotNull,
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(
                const ValueKey<String>(
                  'setting-editor:LIGHT_SQUARE_BRIGHTNESS',
                ),
              ),
            )
            .onChanged,
        isNotNull,
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(const ValueKey<String>('light-square-hue')),
            )
            .onChanged,
        isNotNull,
      );

      service.acknowledgedSettingWriteCompleter!.complete(
        LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 201,
            key: 'LIGHT_SQUARE_MODE',
            ok: true,
            reason: LitchMutationReason.none,
            hasValue: true,
            value: 'color',
            isCompact: false,
          ),
        ),
      );
      await tester.pumpAndSettle();
      expectBrightnessSemantics(enabled: true);
      expect(
        find.descendant(
          of: find.byKey(const ValueKey<String>('light-square-appearance')),
          matching: find.byType(LinearProgressIndicator),
        ),
        findsNothing,
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(const ValueKey<String>('light-square-hue')),
            )
            .onChanged,
        isNotNull,
      );
      semantics.dispose();
    },
  );

  testWidgets(
    'same-service reconnect retires an old-session square-light slider completion',
    (WidgetTester tester) async {
      final List<LitchSetting> colorSettings = <LitchSetting>[
        ..._defaultSettings,
        for (final LitchSetting setting in _lightSquareAppearanceSettings)
          setting.key == 'LIGHT_SQUARE_MODE'
              ? _withValue(setting, 'color')
              : setting,
      ];
      final _FakeLitchBleService service =
          _FakeLitchBleService(settings: colorSettings)
            ..acknowledgedSettingWriteCompleter =
                Completer<LitchSettingWriteResult>();
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      final Finder hue = find.byKey(const ValueKey<String>('light-square-hue'));
      await scrollIntoView(tester, hue);
      final Rect bounds = tester.getRect(hue);
      final TestGesture gesture = await tester.startGesture(
        Offset(bounds.left + 24, bounds.center.dy),
      );
      await gesture.moveTo(
        Offset(bounds.left + bounds.width * .7, bounds.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      await gesture.up();
      await tester.pump();
      expect(service.settingWrites.last.awaitAck, isTrue);

      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      service.settingsToReturn = <LitchSetting>[
        ..._defaultSettings,
        for (final LitchSetting setting in _lightSquareAppearanceSettings)
          switch (setting.key) {
            'LIGHT_SQUARE_MODE' => _withValue(setting, 'color'),
            'LIGHT_SQUARE_HUE' => _withValue(setting, 91),
            'LIGHT_SQUARE_BRIGHTNESS' => _withValue(setting, 3),
            _ => setting,
          },
      ];
      service.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pumpAndSettle();

      service.acknowledgedSettingWriteCompleter!.complete(
        LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 202,
            key: 'LIGHT_SQUARE_HUE',
            ok: true,
            reason: LitchMutationReason.none,
            hasValue: true,
            value: 180,
            isCompact: false,
          ),
        ),
      );
      await tester.pumpAndSettle();

      final Finder refreshedHue = find.byKey(
        const ValueKey<String>('light-square-hue'),
      );
      await scrollIntoView(tester, refreshedHue);
      expect(
        tester.widget<Slider>(refreshedHue).value,
        closeTo(91 / 255 * 360, .01),
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(
                const ValueKey<String>(
                  'setting-editor:LIGHT_SQUARE_BRIGHTNESS',
                ),
              ),
            )
            .value,
        3,
      );
    },
  );

  testWidgets(
    'square-light mode rejection restores White and reconnect rebases confirmed values',
    (WidgetTester tester) async {
      final _FakeLitchBleService rejected = _FakeLitchBleService(
        settings: const <LitchSetting>[
          ..._defaultSettings,
          ..._lightSquareAppearanceSettings,
        ],
      )..settingWriteResult = false;
      addTearDown(rejected.dispose);
      await pumpSettings(tester, rejected);
      final Finder mode = find.byKey(
        const ValueKey<String>('light-square-mode'),
      );
      await scrollIntoView(tester, mode);
      await tester.tap(
        _choiceSegment(find.descendant(of: mode, matching: find.text('Color'))),
      );
      await tester.pumpAndSettle();
      expect(
        tester.widget<SettingsChoicePills<String>>(mode).selected,
        <String>{'white'},
      );
      expect(
        find.byKey(const ValueKey<String>('light-square-hue')),
        findsNothing,
      );

      final _FakeLitchBleService reconnected = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          for (final LitchSetting setting in _lightSquareAppearanceSettings)
            switch (setting.key) {
              'LIGHT_SQUARE_MODE' => _withValue(setting, 'color'),
              'LIGHT_SQUARE_HUE' => _withValue(setting, 91),
              'LIGHT_SQUARE_BRIGHTNESS' => _withValue(setting, 3),
              _ => setting,
            },
        ],
      );
      addTearDown(reconnected.dispose);
      await tester.pumpWidget(harness(reconnected));
      await tester.pumpAndSettle();
      final Finder reconnectedMode = find.byKey(
        const ValueKey<String>('light-square-mode'),
      );
      await scrollIntoView(tester, reconnectedMode);
      expect(
        tester.widget<SettingsChoicePills<String>>(reconnectedMode).selected,
        <String>{'color'},
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(const ValueKey<String>('light-square-hue')),
            )
            .value,
        closeTo(91 / 255 * 360, .01),
      );
      expect(
        tester
            .widget<Slider>(
              find.byKey(
                const ValueKey<String>(
                  'setting-editor:LIGHT_SQUARE_BRIGHTNESS',
                ),
              ),
            )
            .value,
        3,
      );
      expect(reconnected.settingWrites, isEmpty);
    },
  );

  testWidgets('square-light choices fit with 48px targets at 200% text', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService service = _FakeLitchBleService(
      settings: <LitchSetting>[
        ..._defaultSettings,
        for (final setting in _lightSquareAppearanceSettings)
          setting.key == 'LIGHT_SQUARE_MODE'
              ? _withValue(setting, 'color')
              : setting,
      ],
    );
    addTearDown(service.dispose);
    await pumpSettings(tester, service, textScaler: const TextScaler.linear(2));
    final Finder mode = find.byKey(const ValueKey<String>('light-square-mode'));
    await scrollIntoView(tester, mode);
    final SettingsChoicePills<String> control = tester.widget(mode);
    expect(control.selected, <String>{'color'});
    final joined = find.descendant(
      of: mode,
      matching: find.byType(SegmentedButton<String>),
    );
    expect(joined, findsOneWidget);
    expect(
      tester.widget<SegmentedButton<String>>(joined).direction,
      Axis.horizontal,
      reason: 'The short labels fit together at the requested 200% scale.',
    );
    final options = find.descendant(
      of: mode,
      matching: find.byType(TextButton),
    );
    expect(options, findsNWidgets(2));
    expect(MediaQuery.textScalerOf(tester.element(mode)).scale(10), 20);
    for (final element in options.evaluate()) {
      final option = find.byWidget(element.widget);
      final bounds = tester.getRect(option);
      expect(bounds.size.shortestSide, greaterThanOrEqualTo(48));
      expect(bounds.left, greaterThanOrEqualTo(16));
      expect(bounds.right, lessThanOrEqualTo(304));
      expectReviewTextFits(
        tester,
        find.descendant(of: option, matching: find.byType(Text)),
        'The full square-light choice remains readable at 200%.',
      );
    }
    expect(find.descendant(of: mode, matching: find.text('White')), findsOne);
    expect(find.descendant(of: mode, matching: find.text('Color')), findsOne);
    final hue = find.byKey(const ValueKey('light-square-hue'));
    await scrollIntoView(tester, hue);
    expect(tester.getSize(hue).height, greaterThanOrEqualTo(48));
    expect(tester.getRect(hue).left, greaterThanOrEqualTo(16));
    expect(tester.getRect(hue).right, lessThanOrEqualTo(304));
    expect(tester.takeException(), isNull);
  });

  for (final double width in <double>[360, 393]) {
    testWidgets('light-square sliders align label and value at width $width', (
      tester,
    ) async {
      await tester.binding.setSurfaceSize(Size(width, 800));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      final service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          for (final setting in _lightSquareAppearanceSettings)
            setting.key == 'LIGHT_SQUARE_MODE'
                ? _withValue(setting, 'color')
                : setting,
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final appearance = find.byKey(const ValueKey('light-square-appearance'));
      await scrollIntoView(tester, appearance);
      final label = find.descendant(
        of: appearance,
        matching: find.text('Brightness'),
      );
      final value = find.descendant(of: appearance, matching: find.text('8'));
      final brightness = find.byKey(
        const ValueKey('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
      );
      final hue = find.byKey(const ValueKey('light-square-hue'));
      expect(
        tester.getCenter(label).dy,
        closeTo(tester.getCenter(brightness).dy, 1),
      );
      expect(
        tester.getCenter(value).dy,
        closeTo(tester.getCenter(brightness).dy, 1),
      );
      expect(tester.getRect(hue).left, tester.getRect(brightness).left);
      expect(tester.getRect(hue).right, tester.getRect(brightness).right);
      expect(tester.getSize(brightness).height, greaterThanOrEqualTo(48));
      expect(tester.getSize(hue).height, greaterThanOrEqualTo(48));
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'light-square-inline-${width.toInt()}');
    });
  }

  testWidgets('repeated switches stay usable and coalesce behind one ACK', (
    tester,
  ) async {
    final completion = Completer<LitchSettingWriteResult>();
    final service = _FakeLitchBleService()
      ..acknowledgedSettingWriteCompleter = completion;
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    final toggle = find.byKey(
      const ValueKey('setting-editor:WHITE_SQUARES_ON'),
    );
    await scrollIntoView(tester, toggle);
    for (final value in <bool>[false, true, false, true]) {
      await tester.tap(toggle);
      await tester.pump(const Duration(milliseconds: 30));
      expect(tester.widget<Switch>(toggle).value, value);
      expect(tester.widget<Switch>(toggle).onChanged, isNotNull);
    }
    expect(service.settingWrites.map((write) => write.value), <Object>[false]);
    service.acknowledgedSettingWriteCompleter = null;
    completion.complete(
      const LitchSettingWriteAcknowledged(
        LitchSettingOutcome(
          requestId: 301,
          key: 'WHITE_SQUARES_ON',
          ok: true,
          reason: LitchMutationReason.none,
          hasValue: true,
          value: false,
          isCompact: false,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(service.settingWrites.map((write) => write.value), <Object>[
      false,
      true,
    ]);
    expect(tester.widget<Switch>(toggle).value, isTrue);
    expect(find.byType(SnackBar), findsNothing);
  });

  for (final key in ['LIGHT_SQUARE_BRIGHTNESS', 'LIGHT_SQUARE_HUE']) {
    testWidgets('old $key ACK cannot rewind a second held drag', (
      tester,
    ) async {
      final completion = Completer<LitchSettingWriteResult>();
      final service = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          for (final setting in _lightSquareAppearanceSettings)
            setting.key == 'LIGHT_SQUARE_MODE'
                ? _withValue(setting, 'color')
                : setting,
        ],
      )..acknowledgedSettingWriteCompleter = completion;
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final brightness = find.byKey(
        ValueKey(
          key == 'LIGHT_SQUARE_HUE'
              ? 'light-square-hue'
              : 'setting-editor:$key',
        ),
      );
      await scrollIntoView(tester, brightness);
      final bounds = tester.getRect(brightness);
      Offset at(double fraction) =>
          Offset(bounds.left + bounds.width * fraction, bounds.center.dy);
      final first = await tester.startGesture(at(.9));
      await first.moveTo(at(.7));
      await tester.pump(const Duration(milliseconds: 16));
      await first.moveTo(at(.5));
      await tester.pump(const Duration(milliseconds: 16));
      int encoded(double value) => key == 'LIGHT_SQUARE_HUE'
          ? (value / 360 * 255).round()
          : value.round();
      final firstPreview = tester.widget<Slider>(brightness).value;
      final firstValue = encoded(firstPreview);
      await first.up();
      await tester.pump();
      expect(
        service.settingWrites.where((write) => write.awaitAck),
        hasLength(1),
      );

      final second = await tester.startGesture(at(.5));
      await second.moveTo(at(.7));
      await tester.pump(const Duration(milliseconds: 16));
      final heldValue = tester.widget<Slider>(brightness).value;
      expect(heldValue, greaterThan(firstPreview));
      service.acknowledgedSettingWriteCompleter = null;
      service.boardValues[key] = firstValue;
      completion.complete(
        LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 302,
            key: key,
            ok: true,
            reason: LitchMutationReason.none,
            hasValue: true,
            value: firstValue,
            isCompact: false,
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 16));
      service.emitSettingValue(key, firstValue);
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(brightness).value, heldValue);
      await second.moveTo(at(.9));
      await tester.pump(const Duration(milliseconds: 16));
      final finalValue = encoded(tester.widget<Slider>(brightness).value);
      expect(
        service.settingWrites.where((write) => write.awaitAck),
        hasLength(1),
        reason: 'the newer held drag has not sent its final confirmation',
      );
      await second.up();
      await tester.pumpAndSettle();
      expect(
        service.settingWrites.where((write) => write.awaitAck),
        hasLength(2),
      );
      expect(service.settingWrites.last.value, finalValue);
      expect(
        tester.widget<Slider>(brightness).value,
        closeTo(
          key == 'LIGHT_SQUARE_HUE'
              ? finalValue / 255 * 360
              : finalValue.toDouble(),
          .01,
        ),
      );
    });
  }

  for (final end in ['cancel', 'disconnect', 'dispose']) {
    testWidgets('live Hue retires held writes on $end', (tester) async {
      final completion = Completer<bool>();
      List<LitchSetting> colorSettings(int hue) => [
        ..._defaultSettings,
        for (final setting in _lightSquareAppearanceSettings)
          switch (setting.key) {
            'LIGHT_SQUARE_MODE' => _withValue(setting, 'color'),
            'LIGHT_SQUARE_HUE' => _withValue(setting, hue),
            _ => setting,
          },
      ];
      final service = _FakeLitchBleService(settings: colorSettings(0))
        ..settingWriteCompleter = completion;
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final hue = find.byKey(const ValueKey('light-square-hue'));
      await scrollIntoView(tester, hue);
      final bounds = tester.getRect(hue);
      Offset at(double fraction) =>
          Offset(bounds.left + bounds.width * fraction, bounds.center.dy);
      final gesture = await tester.startGesture(at(.2));
      await gesture.moveTo(at(.4));
      await tester.pump(const Duration(milliseconds: 40));
      await gesture.moveTo(at(.7));
      await tester.pump(const Duration(milliseconds: 40));
      final latest = (tester.widget<Slider>(hue).value / 360 * 255).round();
      expect(service.settingWrites, hasLength(1));
      if (end == 'disconnect') {
        service.activeBoardId = null;
        service.connectionController.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(tester.widget<Slider>(hue).onChanged, isNull);
      } else if (end == 'dispose') {
        await tester.pumpWidget(const SizedBox());
      }
      await gesture.cancel();
      if (end == 'disconnect') {
        service.settingsToReturn = colorSettings(91);
        service.activeBoardId = 'board-1';
        service.connectionController.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pumpAndSettle();
      }
      service.settingWriteCompleter = null;
      completion.complete(true);
      await tester.pumpAndSettle();
      if (end == 'cancel') {
        expect(service.settingWrites.last.awaitAck, isTrue);
        expect(service.settingWrites.last.value, latest);
        expect(
          service.settingWrites.where((write) => write.awaitAck),
          hasLength(1),
        );
      } else {
        expect(
          service.settingWrites,
          hasLength(1),
          reason:
              'Retired queued writes cannot enter a new view or connection.',
        );
        if (end == 'disconnect') {
          expect(
            tester.widget<Slider>(hue).value,
            closeTo(91 / 255 * 360, .01),
          );
        }
      }
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('phone Console preference accepts a new intent while saving', (
    tester,
  ) async {
    final completion = Completer<void>();
    final service = _FakeLitchBleService();
    final requested = <bool>[];
    addTearDown(service.dispose);
    await tester.pumpWidget(
      harness(
        service,
        onConsoleChanged: (value) async {
          requested.add(value);
          if (requested.length == 1) await completion.future;
        },
      ),
    );
    await tester.pumpAndSettle();
    final toggle = find.byKey(const ValueKey('enable-app-console'));
    await scrollIntoView(tester, toggle);
    await tester.tap(toggle);
    await tester.pump();
    expect(tester.widget<Switch>(toggle).value, isTrue);
    expect(tester.widget<Switch>(toggle).onChanged, isNotNull);
    await tester.tap(toggle);
    await tester.pump();
    expect(tester.widget<Switch>(toggle).value, isFalse);
    expect(requested, <bool>[true]);
    completion.complete();
    await tester.pumpAndSettle();
    expect(requested, <bool>[true, false]);
  });

  testWidgets(
    'discrete writes await protocol acknowledgement and revert calmly',
    (WidgetTester tester) async {
      final _FakeLitchBleService service = _FakeLitchBleService();
      service.settingWriteResult = false;
      addTearDown(service.dispose);
      await pumpSettings(tester, service);

      final Finder lightSquares = find.byKey(
        const ValueKey<String>('quick:WHITE_SQUARES_ON'),
      );
      final Finder toggle = find.descendant(
        of: lightSquares,
        matching: find.byType(Switch),
      );
      await scrollIntoView(tester, toggle);
      await tester.tap(toggle);
      await tester.pumpAndSettle();

      expect(service.settingWrites, hasLength(1));
      expect(service.settingWrites.single.key, 'WHITE_SQUARES_ON');
      expect(service.settingWrites.single.awaitAck, isTrue);
      expect(tester.widget<Switch>(toggle).value, isTrue);
      expect(
        find.text(
          'The board did not confirm Light-square LEDs. The previous value is still shown.',
        ),
        findsOneWidget,
      );
    },
  );

  testWidgets('Bluetooth connection light is permanently absent from Setup', (
    WidgetTester tester,
  ) async {
    final service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    expect(
      find.byKey(const ValueKey('quick:BLE_CONNECTION_INDICATOR')),
      findsNothing,
    );
    expect(find.text('Bluetooth connection light'), findsNothing);
    expect(service.settingWrites, isEmpty);
  });

  for (final scale in <double>[1, 2]) {
    testWidgets(
      'placement effect full phone screen at ${scale * 100} percent',
      (tester) async {
        await prepareLocalizationReview(tester);
        final service = _FakeLitchBleService(
          settings: [..._defaultSettings, _placementEffectSetting],
        )..settingWriteCompleter = Completer<bool>();
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: const Locale('en'),
            scale: scale,
            home: LitchMainScaffold(
              bleService: service,
              boardId: 'board-1',
              boards: const <LitchBoard>[],
              onDisconnect: () async {},
              initialCraftModeResolved: true,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey<String>('main-destination-setup')),
        );
        await tester.pumpAndSettle();
        final control = find.byKey(const ValueKey('quick:SETTLE_ANIM_STYLE'));
        await scrollIntoView(tester, control);
        await captureHealthReview(
          tester,
          'placement-${debugDefaultTargetPlatformOverride!.name}-${scale.toInt()}x-ready',
        );
        final editor = find.descendant(
          of: control,
          matching: find.byType(DropdownButtonFormField<String>),
        );
        await tester.tap(editor);
        await tester.pumpAndSettle();
        await tester.tap(find.text('Landing flash').last);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 200));
        expect(service.settingWrites.single.value, 'landing_flash');
        expect(
          tester.widget<DropdownButtonFormField<String>>(editor).initialValue,
          'landing_flash',
        );
        await captureHealthReview(
          tester,
          'placement-${debugDefaultTargetPlatformOverride!.name}-${scale.toInt()}x-pending',
        );
        service.settingWriteCompleter!.complete(false);
        await tester.pumpAndSettle();
        expect(
          find.textContaining('The previous value is still shown.'),
          findsOneWidget,
        );
        await captureHealthReview(
          tester,
          'placement-${debugDefaultTargetPlatformOverride!.name}-${scale.toInt()}x-rejected',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'five placement choices use full-label dropdown, preview retap and latest ACK at $scale',
      (tester) async {
        await tester.binding.setSurfaceSize(const Size(393, 852));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        const five = LitchSetting(
          key: 'SETTLE_ANIM_STYLE',
          type: 'enum',
          category: 'animation',
          value: 'mini_orbit',
          defaultValue: 'mini_orbit',
          options: [
            'landing_flash',
            'mini_orbit',
            'ripple',
            'none',
            'rainbow_orbit',
          ],
        );
        final service = _FakeLitchBleService(
          settings: [
            ..._defaultSettings,
            five,
            const LitchSetting(
              key: 'MINIMAL_ANIMATIONS',
              type: 'bool',
              category: 'animation',
              value: true,
              defaultValue: false,
            ),
          ],
        )..settingWriteCompleter = Completer<bool>();
        addTearDown(service.dispose);
        await pumpSettings(
          tester,
          service,
          textScaler: TextScaler.linear(scale),
        );
        final card = find.byKey(const ValueKey('quick:SETTLE_ANIM_STYLE'));
        await scrollIntoView(tester, card);
        Finder dropdown() => find.descendant(
          of: card,
          matching: find.byType(DropdownButtonFormField<String>),
        );
        expect(dropdown(), findsOneWidget);
        expect(
          tester
              .widget<DropdownButton<String>>(
                find.descendant(
                  of: dropdown(),
                  matching: find.byType(DropdownButton<String>),
                ),
              )
              .items!
              .map((item) => item.value),
          ['none', 'landing_flash', 'mini_orbit', 'rainbow_orbit', 'ripple'],
        );
        expect(
          find.descendant(
            of: card,
            matching: find.byType(SettingsChoicePills<String>),
          ),
          findsNothing,
        );
        expect(tester.getSize(dropdown()).height, greaterThanOrEqualTo(48));
        Future<void> choose(String label) async {
          await tester.ensureVisible(dropdown());
          await tester.tap(dropdown());
          await tester.pumpAndSettle();
          final item = find.text(label).last;
          await tester.ensureVisible(item);
          await tester.tap(item);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
        }

        await choose('Mini orbit');
        expect(
          service.settingWrites.single.value,
          'mini_orbit',
          reason:
              'Current-style retap sends a fresh acknowledged Noop intent for preview.',
        );
        expect(service.settingWrites.single.awaitAck, isTrue);
        await choose('Rainbow orbit');
        await choose('Landing flash');
        expect(
          service.settingWrites,
          hasLength(1),
          reason:
              'Outstanding selection is serialized and later intents coalesce.',
        );
        expect(
          tester
              .widget<DropdownButtonFormField<String>>(dropdown())
              .initialValue,
          'landing_flash',
        );
        final first = service.settingWriteCompleter!;
        service.settingWriteCompleter = Completer<bool>();
        first.complete(false);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        expect(service.settingWrites, hasLength(2));
        expect(service.settingWrites.last.value, 'landing_flash');
        expect(
          find.byType(SnackBar),
          findsNothing,
          reason: 'Superseded failure does not roll back the latest style.',
        );
        service.settingWriteCompleter!.complete(true);
        service.settingWriteCompleter = null;
        await tester.pumpAndSettle();
        expect(
          find.text('With Minimal animations, placement feedback stays still.'),
          findsOneWidget,
        );
        service.settingWriteCompleter = Completer<bool>();
        await choose('Rainbow orbit');
        service.settingWriteCompleter!.complete(false);
        await tester.pumpAndSettle();
        service.settingWriteCompleter = null;
        expect(
          tester
              .widget<DropdownButtonFormField<String>>(dropdown())
              .initialValue,
          'landing_flash',
        );
        expect(
          find.textContaining('The previous value is still shown.'),
          findsOneWidget,
        );
        for (final size in [const Size(393, 852), const Size(852, 393)]) {
          await tester.binding.setSurfaceSize(size);
          await tester.pumpAndSettle();
          for (final (label, value) in [
            ('None', 'none'),
            ('Landing flash', 'landing_flash'),
            ('Mini orbit', 'mini_orbit'),
            ('Rainbow orbit', 'rainbow_orbit'),
            ('Ripple', 'ripple'),
          ]) {
            await tester.ensureVisible(dropdown());
            // Lay out the page's new scroll position before targeting the
            // field; a prior error notice can cover its old position.
            await tester.pump();
            expect(dropdown().hitTestable(), findsOneWidget);
            await tester.tap(dropdown());
            await tester.pumpAndSettle();
            final menu = find.byType(Scrollable).last;
            // Enlarged landscape menus mount only their viewport. Reach
            // every logical choice through the menu's own real scrolling.
            await tester.drag(menu, const Offset(0, 600));
            await tester.pumpAndSettle();
            final item = find.descendant(of: menu, matching: find.text(label));
            await tester.scrollUntilVisible(item, 80, scrollable: menu);
            expect(item, findsOneWidget);
            await tester.tap(item);
            await tester.pumpAndSettle();
            expect(
              tester
                  .widget<DropdownButtonFormField<String>>(dropdown())
                  .initialValue,
              value,
            );
            expect(service.settingWrites.last.value, value);
            expect(tester.takeException(), isNull);
          }
        }
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets('Circle confirmation label writes the existing O policy token', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    await revealAdvanced(tester);
    await enterSettingsSearch(tester, 'MOVE_CONFIRM');
    final Finder editor = find.byKey(
      const ValueKey<String>('setting-editor:MOVE_CONFIRM'),
    );
    await scrollIntoView(tester, editor);
    expect(
      find.descendant(of: editor, matching: find.byType(TextButton)),
      findsNWidgets(3),
    );
    expect(find.text('Press the center button'), findsNothing);
    await tester.tap(find.text('Confirm with Circle button').last);
    await tester.pumpAndSettle();
    expect(service.settingWrites.single.key, 'MOVE_CONFIRM');
    expect(service.settingWrites.single.value, 'press_circle_to_confirm');
    expect(service.settingWrites.single.awaitAck, isTrue);
  });

  testWidgets('rejected enum writes restore the board value', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    service.settingWriteResult = false;
    addTearDown(service.dispose);
    await pumpSettings(tester, service);
    await revealAdvanced(tester);
    await enterSettingsSearch(tester, 'MOVE_CONFIRM');

    final Finder stableEditor = find.byKey(
      const ValueKey<String>('setting-editor:MOVE_CONFIRM'),
    );
    await scrollIntoView(tester, stableEditor);
    Finder choices = find.descendant(
      of: stableEditor,
      matching: find.byType(SettingsChoicePills<String>),
      matchRoot: true,
    );
    expect(
      tester.widget<SettingsChoicePills<String>>(choices).selected.single,
      'settle_2s',
    );

    await tester.tap(find.text('After 5 seconds').last);
    await tester.pumpAndSettle();

    expect(service.settingWrites, hasLength(1));
    expect(service.settingWrites.single.key, 'MOVE_CONFIRM');
    expect(service.settingWrites.single.value, 'settle_5s');
    expect(service.settingWrites.single.awaitAck, isTrue);
    choices = find.descendant(
      of: stableEditor,
      matching: find.byType(SettingsChoicePills<String>),
      matchRoot: true,
    );
    expect(
      tester.widget<SettingsChoicePills<String>>(choices).selected.single,
      'settle_2s',
    );
    expect(find.textContaining('The board did not confirm'), findsOneWidget);
  });

  testWidgets('setting rejection explains not-at-start without a timeout claim', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 1,
          key: 'WHITE_SQUARES_ON',
          ok: false,
          reason: LitchMutationReason.notAtStart,
          hasValue: false,
          isCompact: false,
        ),
      );
    addTearDown(service.dispose);
    await pumpSettings(tester, service);

    final Finder toggle = find.descendant(
      of: find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
      matching: find.byType(Switch),
    );
    await scrollIntoView(tester, toggle);
    await tester.tap(toggle);
    await tester.pumpAndSettle();

    expect(
      find.text(
        'This setting is only available before play begins. The previous value is still shown.',
      ),
      findsOneWidget,
    );
    expect(find.textContaining('did not confirm'), findsNothing);
    expect(tester.widget<Switch>(toggle).value, isTrue);
  });

  testWidgets('setting rejection explains busy without a timeout claim', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService()
      ..acknowledgedSettingWriteResult = LitchSettingWriteAcknowledged(
        const LitchSettingOutcome(
          requestId: 1,
          key: 'WHITE_SQUARES_ON',
          ok: false,
          reason: LitchMutationReason.busy,
          hasValue: false,
          isCompact: true,
        ),
      );
    addTearDown(service.dispose);
    await pumpSettings(tester, service);

    final Finder toggle = find.descendant(
      of: find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
      matching: find.byType(Switch),
    );
    await scrollIntoView(tester, toggle);
    await tester.tap(toggle);
    await tester.pumpAndSettle();

    expect(
      find.text(
        'The board is busy; try again. The previous value is still shown.',
      ),
      findsOneWidget,
    );
    expect(find.textContaining('did not confirm'), findsNothing);
    expect(tester.widget<Switch>(toggle).value, isTrue);
  });

  testWidgets(
    'narrow large-text layout keeps controls reachable and semantic',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(320, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final List<LitchSetting> settings = _defaultSettings.map((setting) {
        return setting.key == 'VS_CPU_LEVEL' ? _withValue(setting, 5) : setting;
      }).toList();
      final _FakeLitchBleService service = _FakeLitchBleService(
        settings: settings,
      );
      addTearDown(service.dispose);
      final semantics = tester.ensureSemantics();
      try {
        await pumpSettings(
          tester,
          service,
          textScaler: const TextScaler.linear(2),
        );
        expect(tester.takeException(), isNull);

        for (final String key in <String>['VS_CPU_PLAYER']) {
          final Finder quick = find.byKey(ValueKey<String>('quick:$key'));
          final Finder choices = find.descendant(
            of: quick,
            matching: find.byType(SettingsChoicePills<int>),
            matchRoot: true,
          );
          await scrollIntoView(tester, choices);
          expect(
            find.descendant(
              of: choices,
              matching: find.byType(SegmentedButton<int>),
            ),
            findsOneWidget,
          );
          final pills = find.descendant(
            of: choices,
            matching: find.byType(TextButton),
          );
          expect(pills, findsNWidgets(2));
          for (final pill in pills.evaluate()) {
            expect(
              tester.getSize(find.byWidget(pill.widget)).height,
              greaterThanOrEqualTo(48),
            );
          }
          expect(tester.widget<SettingsChoicePills<int>>(choices).selected, {
            2,
          });
          expect(tester.takeException(), isNull);
        }

        final players = find.byKey(
          const ValueKey<String>('quick:GAME_FIRST_PLAYER'),
        );
        await scrollIntoView(tester, players);
        final swapPlayers = find.byKey(const ValueKey<String>('swap-players'));
        expect(swapPlayers, findsOneWidget);
        expect(tester.getSize(swapPlayers).height, greaterThanOrEqualTo(48));
        expect(tester.takeException(), isNull);

        for (final String key in <String>['P1_PIECE_WHEEL', 'P2_PIECE_WHEEL']) {
          final Finder color = find.byKey(ValueKey<String>('quick:$key'));
          await scrollIntoView(tester, color);
          expect(tester.getSize(color).height, greaterThanOrEqualTo(96));
          expect(tester.getSemantics(color).label, contains('color'));
          expect(tester.takeException(), isNull);
        }

        final Finder p1Color = find.byKey(
          const ValueKey<String>('quick:P1_PIECE_WHEEL'),
        );
        await scrollIntoView(tester, p1Color);
        await tester.tap(p1Color);
        await tester.pumpAndSettle();
        final Finder pickerHeading = find.text('Choose Player 1 color');
        expect(
          tester
              .getSemantics(pickerHeading)
              .getSemanticsData()
              .flagsCollection
              .isHeader,
          isTrue,
        );
        final Finder violet = find.byKey(
          const ValueKey<String>('piece-color-choice:P1_PIECE_WHEEL:213'),
        );
        await tester.scrollUntilVisible(
          violet,
          160,
          scrollable: find
              .descendant(
                of: find.byType(SingleChildScrollView).last,
                matching: find.byType(Scrollable),
              )
              .first,
        );
        expect(violet, findsOneWidget);
        expect(tester.getSize(violet).height, greaterThanOrEqualTo(56));
        expect(tester.takeException(), isNull);
        Navigator.of(tester.element(pickerHeading)).pop();
        await tester.pumpAndSettle();

        await pumpBoardActions(
          tester,
          service,
          textScaler: const TextScaler.linear(2),
        );
        final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
        await scrollIntoView(tester, hint);
        final List<Size> actionSizes =
            <String>['control:hint', 'control:undo', 'control:new-game'].map((
              String key,
            ) {
              return tester.getSize(find.byKey(ValueKey<String>(key)));
            }).toList();
        expect(
          actionSizes.map((Size size) => size.height).toSet(),
          hasLength(1),
        );
        expect(actionSizes.first.height, greaterThanOrEqualTo(48));
        expect(MediaQuery.textScalerOf(tester.element(hint)).scale(1), 1.3);
        expect(
          tester.getSemantics(hint).label,
          contains('Ask the board for a hint'),
        );
        expect(tester.takeException(), isNull);

        final Finder undo = find.byKey(const ValueKey<String>('control:undo'));
        await tester.tap(undo);
        await tester.pump();
        service.emitControl(
          call: service.controlCalls.single,
          phase: LitchControlPhase.applied,
          reason: LitchMutationReason.none,
          actionState: LitchControlActionState.undoPending,
        );
        await tester.pump();
        expect(find.text('Cancel Undo'), findsOneWidget);
        expect(tester.getSize(undo).height, actionSizes.first.height);
        expect(tester.takeException(), isNull);

        await tester.binding.setSurfaceSize(const Size(900, 500));
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets('unconfirmed settings show reconnect guidance and disable board controls', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService service = _FakeLitchBleService();
    service.settingsAuthority = LitchSettingsReadAuthority.unavailable;
    service.availability = LitchControlAvailability.unavailable;
    addTearDown(service.dispose);
    final SemanticsHandle semantics = tester.ensureSemantics();
    await pumpSettings(tester, service);

    expect(
      find.textContaining('Board settings are unavailable. Reconnect your Litch to try again.'),
      findsOneWidget,
    );
    expect(find.text('Board actions'), findsNothing);
    expect(find.textContaining('Brightness is unavailable'), findsNothing);
    final Finder p1Color = find.byKey(
      const ValueKey<String>('quick:P1_PIECE_WHEEL'),
    );
    await scrollIntoView(tester, p1Color);
    expect(
      tester.getSemantics(p1Color).label,
      contains('Unavailable.'),
    );
    expect(
      tester
          .getSemantics(p1Color)
          .getSemanticsData()
          .hasAction(ui.SemanticsAction.tap),
      isFalse,
    );

    await pumpBoardActions(tester, service);
    expect(
      find.textContaining('Board actions are unavailable. Reconnect your Litch'),
      findsOneWidget,
    );
    final OutlinedButton hint = tester.widget<OutlinedButton>(
      find.byKey(const ValueKey<String>('control:hint')),
    );
    expect(hint.onPressed, isNull);
    semantics.dispose();
  });
  for (final locale in [
    const Locale('en'),
    const Locale('de'),
    const Locale('fr'),
  ]) {
    testWidgets(
      'Edge turn styles show None first, sort localized labels and preserve wire values $locale',
      (tester) async {
        final service = _FakeLitchBleService(
          settings: [..._defaultSettings, ..._animationStyleSettings],
        );
        addTearDown(service.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: locale,
            home: Scaffold(
              body: SettingsScreen(
                bleService: service,
                boardId: 'board-1',
                enableAppConsole: false,
                onEnableAppConsoleChanged: (_) async {},
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await openSetupPage(tester, 'lights');
        final editor = find.byKey(
          const ValueKey('setting-editor:TURN_ANIM_STYLE'),
        );
        await scrollIntoView(tester, editor);
        final dropdown = tester.widget<DropdownButton<String>>(
          find.descendant(
            of: editor,
            matching: find.byType(DropdownButton<String>),
          ),
        );
        final entries = dropdown.items!;
        expect(entries.map((item) => item.value), [
          'none',
          ...switch (locale.languageCode) {
            'de' => [
              'active_end_pingpong_sides',
              'side_middle_band',
              'both_ends',
              'away_from_active_end',
              'active_end_only',
            ],
            'fr' => [
              'away_from_active_end',
              'both_ends',
              'active_end_pingpong_sides',
              'side_middle_band',
              'active_end_only',
            ],
            _ => [
              'active_end_pingpong_sides',
              'side_middle_band',
              'away_from_active_end',
              'active_end_only',
              'both_ends',
            ],
          },
        ]);
        expect(
          entries.map((item) => item.value).toSet(),
          _animationStyleSettings.first.options.toSet(),
        );
        final strings = reviewStrings(locale);
        final away = entries.singleWhere(
          (item) => item.value == 'away_from_active_end',
        );
        expect((away.child as Text).data, strings.awayFromActiveEnd);
        await tester.tap(editor);
        await tester.pumpAndSettle();
        await tester.tap(find.text((away.child as Text).data!).last);
        await tester.pumpAndSettle();
        expect(service.settingWrites.single.key, 'TURN_ANIM_STYLE');
        expect(service.settingWrites.single.value, 'away_from_active_end');
        expect(service.settingWrites.single.awaitAck, isTrue);
      },
    );
  }

  testWidgets(
    'Animation settings have one named owner and meaningful overview',
    (tester) async {
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          _placementEffectSetting,
          ..._animationStyleSettings,
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      expect(find.byKey(const ValueKey('setup-open:colors')), findsNothing);
      expect(
        find.byKey(const ValueKey('quick:P1_PIECE_WHEEL')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('quick:P2_PIECE_WHEEL')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('quick:WHITE_SQUARES_ON')),
        findsNothing,
      );
      await openSetupPage(tester, 'lights');
      expect(find.text('Board lights and animation'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('quick:WHITE_SQUARES_ON')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('setting:TURN_ANIM_STYLE')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('setting:HINT_THINKING_STYLE')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('setting:CHECKMATE_ANIM_STYLE')),
        findsOneWidget,
      );
      await scrollIntoView(
        tester,
        find.byKey(const ValueKey('setting-editor:TURN_ANIM_STYLE')),
      );
      await tester.tap(
        find.byKey(const ValueKey('setting-editor:TURN_ANIM_STYLE')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Moving away from current player'), findsOneWidget);
      await tester.tap(find.text('Moving away from current player'));
      await tester.pumpAndSettle();
      expect(service.settingWrites.single.key, 'TURN_ANIM_STYLE');
      expect(service.settingWrites.single.value, 'away_from_active_end');
      final mateEditor = find.byKey(
        const ValueKey('setting-editor:CHECKMATE_ANIM_STYLE'),
      );
      await scrollIntoView(tester, mateEditor);
      expect(
        find.descendant(of: mateEditor, matching: find.text('Fireworks')),
        findsOneWidget,
      );
      await tester.tap(mateEditor);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Fire lob').last);
      await tester.pumpAndSettle();
      expect(service.settingWrites.last.key, 'CHECKMATE_ANIM_STYLE');
      expect(service.settingWrites.last.value, 'fire-lob');
      await openSetupPage(tester, 'advanced');
      expect(
        find.byKey(const ValueKey('setting:CHECKMATE_ANIM_STYLE')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('setting:TURN_ANIM_STYLE')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('setting:HINT_THINKING_STYLE')),
        findsNothing,
      );
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets(
    'Checkmate choices fit Italian at 200 percent and recover rejected writes',
    (tester) async {
      await prepareLocalizationReview(tester);
      final previousTapPolicy = WidgetController.hitTestWarningShouldBeFatal;
      WidgetController.hitTestWarningShouldBeFatal = true;
      addTearDown(
        () => WidgetController.hitTestWarningShouldBeFatal = previousTapPolicy,
      );
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          const LitchSetting(
            key: 'CHECKMATE_ANIM_STYLE',
            type: 'enum',
            category: 'animation',
            value: 'fireworks',
            defaultValue: 'fire-lob',
            options: [
              'fireworks',
              'fire-lob',
              'ripple-bloom',
              'winner-sweep',
              'winner-constellation',
            ],
          ),
        ],
      );
      addTearDown(service.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('it'),
          scale: 2,
          home: Scaffold(
            body: SettingsScreen(
              bleService: service,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await openSetupPage(tester, 'lights');
      final editor = find.byKey(
        const ValueKey('setting-editor:CHECKMATE_ANIM_STYLE'),
      );
      await scrollIntoView(tester, editor);
      expect(tester.getSize(editor).height, greaterThanOrEqualTo(48));
      final strings = reviewStrings(const Locale('it'));
      expectReviewTextFits(
        tester,
        find.descendant(of: editor, matching: find.text(strings.fireworks)),
        'Saved Fireworks must remain fully readable at 200 percent.',
      );
      await tester.tap(editor);
      await tester.pumpAndSettle();
      final longest = find.text(strings.winnerConstellation).last;
      await tester.ensureVisible(longest);
      await tester.pumpAndSettle();
      expectReviewTextFits(
        tester,
        longest,
        'The longest localized checkmate choice must not clip.',
      );
      final menuItem = find
          .ancestor(
            of: longest,
            matching: find.byType(DropdownMenuItem<String>),
          )
          .last;
      final labelRect = tester.getRect(longest);
      final itemRect = tester.getRect(menuItem);
      expect(itemRect.top, lessThanOrEqualTo(labelRect.top + .1));
      expect(itemRect.bottom, greaterThanOrEqualTo(labelRect.bottom - .1));
      expect(itemRect.height, greaterThanOrEqualTo(48));
      service.settingWriteCompleter = Completer<bool>();
      await tester.tap(longest);
      await tester.pump();
      expect(service.settingWrites.single.value, 'winner-constellation');
      service.settingWriteCompleter!.complete(false);
      await tester.pumpAndSettle();
      final field = find.descendant(
        of: editor,
        matching: find.byType(DropdownButtonFormField<String>),
      );
      expect(tester.state<FormFieldState<String>>(field).value, 'fireworks');
      expectReviewTextFits(
        tester,
        find.descendant(of: editor, matching: find.text(strings.fireworks)),
        'A rejected write restores the complete confirmed label.',
      );
      await tester.pump(const Duration(seconds: 5));
      await tester.pumpAndSettle();
      service.settingWriteCompleter = null;
      await tester.ensureVisible(editor);
      await tester.tap(editor);
      await tester.pumpAndSettle();
      await tester.tap(find.text(strings.fireLob).last);
      await tester.pumpAndSettle();
      expect(service.settingWrites.last.value, 'fire-lob');
      expect(tester.state<FormFieldState<String>>(field).value, 'fire-lob');
      await openSetupPage(tester, 'advanced');
      expect(
        find.byKey(const ValueKey('setting:CHECKMATE_ANIM_STYLE')),
        findsNothing,
      );
      expect(tester.takeException(), isNull);
    },
  );

  for (final direction in [TextDirection.ltr, TextDirection.rtl]) {
    for (final rotated in [false, true]) {
      testWidgets('Physical Setup preview $direction rotate=$rotated', (
        tester,
      ) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final service = _FakeLitchBleService(
          settings: _defaultSettings
              .map(
                (s) => s.key == 'BOARD_ROTATE'
                    ? LitchSetting(
                        key: s.key,
                        type: s.type,
                        category: s.category,
                        value: rotated,
                        defaultValue: s.defaultValue,
                      )
                    : s,
              )
              .toList(),
        );
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) =>
                Directionality(textDirection: direction, child: child!),
            home: Scaffold(
              body: SettingsScreen(
                bleService: service,
                boardId: 'board-1',
                enableAppConsole: false,
                onEnableAppConsoleChanged: (_) async {},
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final board = find.descendant(
          of: find.byKey(const ValueKey('setup-board-preview')),
          matching: find.byType(HomeChessboard),
        );
        final boardRect = tester.getRect(board);
        final usb = find.descendant(
          of: find.byKey(const ValueKey('setup-board-preview')),
          matching: find.text('USB'),
        );
        final usbRect = tester.getRect(usb);
        expect(
          usbRect.height,
          greaterThan(usbRect.width),
          reason:
              'visible USB text must stand sideways before structural checks',
        );
        expect(
          boardRect.width,
          greaterThanOrEqualTo(210),
          reason: 'meaningful board size on Setup',
        );
        final portRect = tester.getRect(
          find.byKey(const ValueKey('setup-usb-port')),
        );
        if (!rotated) {
          expect(portRect.right, lessThanOrEqualTo(boardRect.left));
          expect(usbRect.right, lessThan(portRect.left));
        } else {
          expect(portRect.left, greaterThanOrEqualTo(boardRect.right));
          expect(usbRect.left, greaterThan(portRect.right));
        }
        expect(
          tester
              .widget<RotatedBox>(find.byKey(const ValueKey('setup-usb-label')))
              .quarterTurns,
          rotated ? 1 : 3,
        );
        final far = tester.widget<RotatedBox>(
          find.byKey(const ValueKey('setup-board-controls:opposite')),
        );
        final near = tester.widget<RotatedBox>(
          find.byKey(const ValueKey('setup-board-controls:near')),
        );
        expect(far.quarterTurns, 2);
        expect(near.quarterTurns, 0);
        expect(tester.takeException(), isNull);
      });
    }
  }
  testWidgets(
    'square hue has an immediate finger-owned swatch without a color name',
    (tester) async {
      final service = _FakeLitchBleService(
        settings: [
          ..._defaultSettings,
          for (final s in _lightSquareAppearanceSettings)
            s.key == 'LIGHT_SQUARE_MODE' ? _withValue(s, 'color') : s,
        ],
      );
      addTearDown(service.dispose);
      await pumpSettings(tester, service);
      final hue = find.byKey(const ValueKey<String>('light-square-hue'));
      await scrollIntoView(tester, hue);
      final appearance = find.byKey(
        const ValueKey<String>('light-square-appearance'),
      );
      final strings = litchStrings(tester.element(hue));
      expect(
        find.descendant(of: appearance, matching: find.text(strings.colorRed)),
        findsNothing,
        reason:
            'The hue row uses the live swatch without a redundant color name',
      );
      final bounds = tester.getRect(hue);
      final gesture = await tester.startGesture(
        Offset(bounds.left + 24, bounds.center.dy),
      );
      for (final fraction in [.25, .5, .75]) {
        await gesture.moveTo(
          Offset(bounds.left + bounds.width * fraction, bounds.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final value = tester.widget<Slider>(hue).value;
        final swatch = tester.widget<Container>(
          find.byKey(const ValueKey<String>('light-square-hue-preview')),
        );
        expect(
          (swatch.decoration! as BoxDecoration).color,
          litchWheelPreviewColor((value / 360 * 255).round()),
        );
        service.emitSettingValue('LIGHT_SQUARE_HUE', 0);
        await tester.pump(const Duration(milliseconds: 16));
        expect(tester.widget<Slider>(hue).value, value);
        expect(tester.getRect(hue), bounds);
      }
      await gesture.up();
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );
}

// Registered in localization_screens_test.dart so SDK fonts cannot alter
// the existing Ahem goldens or interaction fixtures in this file.
void registerSettingsLocalizationTests() {
  testWidgets(
    'localization translated setting option writes the registry token',
    (tester) async {
      const locale = Locale('de');
      final strings = reviewStrings(locale);
      final ble = _FakeLitchBleService(
        settings: <LitchSetting>[
          ..._defaultSettings,
          const LitchSetting(
            key: 'MOTION_WAKE_MODE',
            type: 'enum',
            category: 'hardware',
            value: 'motion',
            defaultValue: 'motion',
            options: ['motion', 'tap', 'motion_and_tap'],
            description: 'Wake mode; tap modes experimental on V3',
          ),
        ],
      );
      addTearDown(ble.dispose);
      await tester.pumpWidget(
        localizationHarness(
          locale: locale,
          home: Scaffold(
            body: SettingsScreen(
              bleService: ble,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final scrollable = find
          .descendant(
            of: find.byType(SettingsScreen),
            matching: find.byType(Scrollable),
          )
          .first;
      await tester.scrollUntilVisible(
        find.text(strings.advancedSettings),
        400,
        scrollable: scrollable,
      );
      await tester.pumpAndSettle();
      await _openSettingsPage(tester, 'search');
      await tester.pumpAndSettle();
      final search = find.byKey(const ValueKey<String>('advanced-search'));
      await tester.ensureVisible(search);
      await tester.enterText(search, strings.motionWakeMode);
      await tester.pumpAndSettle();
      final choices = find.descendant(
        of: find.byKey(
          const ValueKey<String>('setting-editor:MOTION_WAKE_MODE'),
        ),
        matching: find.byType(SettingsChoicePills<String>),
        matchRoot: true,
      );
      await tester.ensureVisible(choices);
      await tester.pumpAndSettle();
      expect(find.text(strings.motionAndTapMode), findsOneWidget);
      await tester.tap(find.text(strings.tapMode).last);
      await tester.pumpAndSettle();
      expect(ble.settingWrites.single.key, 'MOTION_WAKE_MODE');
      expect(ble.settingWrites.single.value, 'tap');
      expect(ble.settingWrites.single.awaitAck, isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  for (final locale in localizationReviewLocales) {
    for (final scale in <double>[1, 2]) {
      testWidgets(
        'localization full shell $locale at ${scale * 100}% text',
        (tester) async {
          await prepareLocalizationReview(tester);
          final semantics = tester.ensureSemantics();
          try {
            PackageInfo.setMockInitialValues(
              appName: 'Foxie Litch',
              packageName: 'com.foxie.litch',
              version: '0.5.0',
              buildNumber: '7',
              buildSignature: '',
            );
            final board = SettingsSessionBoard(includeHomeSnapshot: true)
              ..firstPlayer = 1;
            final ble = _FakeLitchBleService(ble: board);
            board.configureSettings = (data) {
              for (final key in ['VS_CPU_LEVEL', 'VS_CPU_PLAYER']) {
                final index = SettingsStateWireCodec.visibleKeys.indexOf(key);
                data.setUint16(
                  4 + 2 * index,
                  ble.boardValues[key] as int,
                  Endian.little,
                );
              }
            };
            void publishPlayerSetting(String key, int value) {
              ble.boardValues[key] = value;
              board.settingsRevision++;
              board.publishSettings();
            }
            addTearDown(() => disposeBoard(ble, board));
            ble.debugSetConnectedDevice('board-1');
            final opening = ble.openConnectionSession('board-1');
            await tester.pump();
            expect((await opening)?.isActivated, isTrue);
            expect(ble.productReady, isTrue);
            board.publishSettings();
            expect(ble.latestSettingValue('GAME_FIRST_PLAYER'), 1);
            final strings = reviewStrings(locale);
            ble.retainedHomeState = HomeStateNotification.tryParse(
              '{"event":"home_state","v":1,"seq":1,"a":"idle","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"FFFF00000000FFFF","g":"u","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1,"up":0,"cl":[0,0,0,"w",0,0,0,"y"]}',
            );
            expect(ble.retainedHomeState, isNotNull);
            await tester.pumpWidget(
              AppPreferencesScope(
                appearanceMode: LitchAppearanceMode.system,
                onAppearanceModeChanged: (_) async {},
                localePreference: locale == litchPseudoLocale
                    ? const LitchLocalePreference.system()
                    : LitchLocalePreference.fixed(locale),
                onLocalePreferenceChanged: (_) async {},
                onLitchcraftStorageLimitChanged: (_) async {},
                child: localizationHarness(
                  locale: locale,
                  scale: scale,
                  home: LitchMainScaffold(
                    bleService: ble,
                    boardId: 'board-1',
                    boards: const <LitchBoard>[],
                    onDisconnect: () async {},
                    loadAppConsolePreference: () async => false,
                    firmwarePageBuilder: (_, _, _, _, _) =>
                        const SizedBox.shrink(),
                  ),
                ),
              ),
            );
            await tester.pumpAndSettle();
            await decodeVisibleReviewImages(tester);
            expect(find.text(strings.playerToMove('P1')), findsOneWidget);
            final contextText = find.descendant(
              of: find.byKey(const ValueKey<String>('home-quiet-game-context')),
              matching: find.byType(RichText),
            );
            final paragraph = tester.renderObject<RenderParagraph>(contextText);
            final expectedContext = strings.gameContext(
              strings.unknownVariant,
              1,
            );
            expect(paragraph.text.toPlainText(), expectedContext);
            expect(
              paragraph.didExceedMaxLines,
              isFalse,
              reason:
                  'The visible variant and move count must not be truncated at $locale / $scale.',
            );
            final fullText = TextPainter(
              text: paragraph.text,
              textDirection: paragraph.textDirection,
              textScaler: paragraph.textScaler,
            )..layout(maxWidth: paragraph.size.width);
            expect(
              paragraph.size.height,
              greaterThanOrEqualTo(fullText.height - .1),
              reason: 'Home must reserve the complete wrapped context extent.',
            );
            fullText.dispose();
            expect(
              tester
                  .getSemantics(
                    find.byKey(
                      const ValueKey<String>('home-guidance-live-region'),
                    ),
                  )
                  .label,
              contains(expectedContext),
            );
            expect(tester.takeException(), isNull);
            await captureHealthReview(
              tester,
              'locale-$locale-${scale.toInt()}x-ios-home',
            );
            final homeScroll = find
                .descendant(
                  of: find.byType(HomeScreen),
                  matching: find.byType(Scrollable),
                )
                .first;
            await tester.scrollUntilVisible(
              find.text(strings.boardActions),
              300,
              scrollable: homeScroll,
            );
            await tester.pumpAndSettle();
            expect(find.text(strings.boardActions), findsOneWidget);
            final noClockLabels = find.text(strings.noClock);
            expect(noClockLabels, findsNWidgets(2));
            for (var clock = 0; clock < 2; clock++) {
              expectReviewTextFits(
                tester,
                noClockLabels.at(clock),
                'P${clock + 1} no-clock label must be fully visible at $locale / $scale.',
              );
            }
            await captureHealthReview(
              tester,
              'locale-$locale-${scale.toInt()}x-ios-home-actions',
            );
            final hintControl = find.byKey(
              const ValueKey<String>('control:hint'),
            );
            await Scrollable.ensureVisible(tester.element(hintControl));
            await tester.pumpAndSettle();
            for (final action in <String, String>{
              'hint': strings.hintAction,
              'undo': strings.undoAction,
              'new-game': strings.newGame,
            }.entries) {
              final control = find.byKey(
                ValueKey<String>('control:${action.key}'),
              );
              expectReviewTextFits(
                tester,
                find.descendant(of: control, matching: find.text(action.value)),
                'The ${action.key} action must be fully visible at $locale / $scale.',
              );
              expect(tester.getSize(control).height, greaterThanOrEqualTo(48));
            }
            expect(tester.takeException(), isNull);
            await captureHealthReview(
              tester,
              'locale-$locale-${scale.toInt()}x-ios-home-action-controls',
            );
            final coachingP2 = find.byKey(
              const ValueKey<String>('control:coaching:p2'),
            );
            await Scrollable.ensureVisible(tester.element(coachingP2));
            publishPlayerSetting('VS_CPU_LEVEL', 5);
            await tester.pumpAndSettle();
            expect(_coachingButton(tester, coachingP2).onPressed, isNull);
            expect(
              tester.getSemantics(coachingP2).label,
              strings.coachingUnavailableSemantics(
                '2',
                'unknown',
                strings.computerPlayer,
              ),
            );
            for (final state in <int, String>{0: 'off', 3: 'on'}.entries) {
              ble.emitHomeState(
                sequence: state.key + 2,
                action: 'idle',
                coachingMask: state.key,
                clockProjection: true,
              );
              await tester.pumpAndSettle();
              expect(
                tester.getSemantics(coachingP2).label,
                strings.coachingUnavailableSemantics(
                  '2',
                  'off',
                  strings.computerPlayer,
                ),
              );
            }
            publishPlayerSetting('VS_CPU_PLAYER', 0);
            await tester.pumpAndSettle();
            expect(_coachingButton(tester, coachingP2).onPressed, isNull);
            expect(
              tester.getSemantics(coachingP2).label,
              strings.coachingUnavailableSemantics(
                '2',
                'on',
                strings.playerRoleUnavailable,
              ),
            );
            expect(ble.controlCalls, isEmpty);
            publishPlayerSetting('VS_CPU_LEVEL', 0);
            await tester.pumpAndSettle();
            expect(_coachingButton(tester, coachingP2).onPressed, isNotNull);
            expect(
              tester.getSemantics(coachingP2).label,
              strings.coachingSemantics('2', 'true'),
            );
            await tester.scrollUntilVisible(
              find.text(strings.movesTitle),
              300,
              scrollable: homeScroll,
            );
            await tester.pumpAndSettle();
            expect(find.text(strings.movesTitle), findsOneWidget);
            expect(tester.takeException(), isNull);
            final home = tester.state(find.byType(HomeScreen));
            await tester.tap(
              find.byKey(const ValueKey<String>('main-destination-setup')),
            );
            await tester.pumpAndSettle();
            expect(
              find.byKey(const ValueKey('setup-common-controls')),
              findsOneWidget,
            );
            expect(find.text(strings.playAgainstAnotherPlayer), findsOneWidget);
            expect(tester.takeException(), isNull);
            await captureHealthReview(
              tester,
              'locale-$locale-${scale.toInt()}x-ios-settings-top',
            );
            await _openSettingsPage(tester, 'phone');
            final language = find.byKey(const ValueKey<String>('app-language'));
            final tips = find.byKey(const ValueKey<String>('app-show-tips'));
            final tipsGroup = find.ancestor(
              of: tips,
              matching: find.byType(SettingsToggleRow),
            );
            expect(tipsGroup, findsOneWidget);
            final tipsHelp = find.descendant(
              of: tipsGroup,
              matching: find.text(strings.showTipsDescription),
            );
            expect(tipsHelp, findsOneWidget);
            expect(tester.widget<Text>(tipsHelp).maxLines, isNull);
            expectReviewTextFits(
              tester,
              find.descendant(
                of: tipsGroup,
                matching: find.text(strings.showTips),
              ),
              'The tips label must be fully visible at $locale / $scale.',
            );
            expectReviewTextFits(
              tester,
              tipsHelp,
              'The tips help must be fully visible at $locale / $scale.',
            );
            final storage = find.byKey(
              const ValueKey<String>('app-litchcraft-storage-limit'),
            );
            expect(storage, findsOneWidget);
            expect(find.text(strings.setupThisPhone), findsOneWidget);
            expect(
              tester.getBottomLeft(language).dy,
              lessThan(tester.getTopLeft(tipsGroup).dy),
            );
            expect(
              tester.getBottomLeft(tipsGroup).dy,
              lessThan(tester.getTopLeft(storage).dy),
            );
            expect(
              tester.getTopLeft(tipsGroup).dy -
                  tester.getBottomLeft(language).dy,
              lessThan(100),
            );
            expect(
              tester.getTopLeft(storage).dy -
                  tester.getBottomLeft(tipsGroup).dy,
              lessThan(100),
            );
            expect(
              tester.getBottomLeft(tipsHelp).dy,
              lessThanOrEqualTo(tester.getBottomLeft(tipsGroup).dy),
            );
            expect(tester.getSize(language).height, greaterThanOrEqualTo(48));
            expect(tester.getSize(tips).height, greaterThanOrEqualTo(48));
            expect(tester.getSize(storage).height, greaterThanOrEqualTo(48));
            expect(tester.getSize(storage).width, greaterThanOrEqualTo(48));
            await tester.ensureVisible(storage);
            await tester.pumpAndSettle();
            final settingsScroll = find
                .descendant(
                  of: find.byType(SettingsScreen),
                  matching: find.byType(Scrollable),
                )
                .first;
            final viewport = tester.getRect(settingsScroll);
            final storageRect = tester.getRect(storage);
            expect(storageRect.top, greaterThanOrEqualTo(viewport.top));
            expect(storageRect.bottom, lessThanOrEqualTo(viewport.bottom));
            expect(storageRect.left, greaterThanOrEqualTo(viewport.left));
            expect(storageRect.right, lessThanOrEqualTo(viewport.right));
            for (final text in <String>[
              strings.litchcraftStorageLimit,
              strings.litchcraftStorageLimitValue('1000'),
            ]) {
              expectReviewTextFits(
                tester,
                find.descendant(of: storage, matching: find.text(text)),
                'The storage preference must be fully visible at $locale / $scale.',
              );
            }
            final storageSemantics = tester.getSemantics(storage);
            expect(storageSemantics.flagsCollection.isButton, isTrue);
            expect(
              storageSemantics.flagsCollection.isEnabled,
              ui.Tristate.isTrue,
            );
            expect(
              storageSemantics.getSemanticsData().hasAction(
                ui.SemanticsAction.tap,
              ),
              isTrue,
            );
            expect(
              storageSemantics.label,
              contains(strings.litchcraftStorageLimit),
            );
            expect(
              storageSemantics.label,
              contains(strings.litchcraftStorageLimitValue('1000')),
            );
            expect(
              tester.state(find.byType(HomeScreen, skipOffstage: false)),
              same(home),
            );
            expect(tester.takeException(), isNull);
            await captureHealthReview(
              tester,
              'locale-$locale-${scale.toInt()}x-ios-settings-language',
            );
            await _openSettingsPage(tester, 'developer');
            expect(find.text(strings.developerTools), findsOneWidget);
            expect(
              tester
                  .getSize(find.byKey(const ValueKey('enable-app-console')))
                  .height,
              greaterThanOrEqualTo(48),
            );
            expect(tester.takeException(), isNull);
            await tester.pumpWidget(const SizedBox.shrink());
          } finally {
            semantics.dispose();
          }
        },
        variant: TargetPlatformVariant.only(TargetPlatform.iOS),
      );
    }
  }
  testWidgets(
    'localization owner regression: connected Settings follows German',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(393, 852));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          theme: buildLitchTheme(),
          home: Scaffold(
            body: SettingsScreen(
              bleService: ble,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await captureHealthReview(tester, 'localization-settings-before');
      expect(find.text('Quick Setup'), findsNothing);
      expect(
        find.byKey(const ValueKey('setup-common-controls')),
        findsOneWidget,
      );
      expect(
        find.text(reviewStrings(const Locale('de')).setupGameBehavior),
        findsOneWidget,
      );
      expect(
        find.text(reviewStrings(const Locale('de')).playAgainstAnotherPlayer),
        findsOneWidget,
      );
      expect(find.text('Opponent'), findsNothing);
      await _openSettingsPage(tester, 'developer');
      expect(find.text('Entwicklerwerkzeuge'), findsOneWidget);
      expect(find.text('Konsole'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}

// Reuse the full-shell BLE fixture and the firmware suite's download fixture.
// Only firmware identity reads differ; no radio or upload seam is exercised.
class _FirmwareReviewBleService extends _FakeLitchBleService {
  @override
  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) async =>
      '1.2.2';

  @override
  Future<FirmwareProvenance> fetchFirmwareProvenance({
    required String deviceId,
  }) async => FirmwareProvenance.custom('a' * 64);

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async => capabilities;
}

void registerFirmwareLocalizationTests() {
  for (final locale in localizationReviewLocales) {
    testWidgets(
      'localization firmware recovery and original notes $locale',
      (tester) async {
        await prepareLocalizationReview(tester);
        final ble = _FirmwareReviewBleService();
        ble.emitSettingValue('GAME_FIRST_PLAYER', 1);
        ble.retainedHomeState = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":1,"a":"idle","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"FFFF00000000FFFF","g":"u","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1,"up":0,"cl":[0,0,0,"w",0,0,0,"y"]}',
        );
        addTearDown(ble.dispose);
        final download = FlakyFirmwareDownloadService();
        final strings = reviewStrings(locale);
        await tester.pumpWidget(
          localizationHarness(
            locale: locale,
            scale: 2,
            home: LitchMainScaffold(
              bleService: ble,
              boardId: 'board-1',
              boards: const <LitchBoard>[],
              onDisconnect: () async {},
              loadAppConsolePreference: () async => false,
              firmwarePageBuilder:
                  (
                    service,
                    boardId,
                    showFirmware,
                    activityChanged,
                    availabilityChanged,
                  ) => FirmwareScreen(
                    bleService: service,
                    boardId: boardId,
                    onRequestShowFirmwareTab: showFirmware,
                    onUpdateActivityChanged: activityChanged,
                    onAvailabilityChanged: availabilityChanged,
                    downloadService: download,
                  ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey<String>('connected-board-status-action')),
        );
        await tester.pumpAndSettle();
        final boardSoftware = find.text(strings.boardSoftware);
        await tester.ensureVisible(boardSoftware);
        await tester.pumpAndSettle();
        await tester.tap(boardSoftware);
        await tester.pumpAndSettle();
        final back = find.byKey(const ValueKey<String>('firmware-back'));
        expect(back, findsOneWidget);
        expect(
          tester.widget<IconButton>(back).tooltip,
          MaterialLocalizations.of(tester.element(back)).backButtonTooltip,
        );
        expectReviewTextFits(
          tester,
          find.descendant(
            of: find.byType(AppBar),
            matching: find.text(strings.boardUpdateTitle),
          ),
          'The real firmware host title must be complete at $locale / 2x.',
        );
        expect(find.text(strings.updateLoadFailed), findsOneWidget);
        expect(find.textContaining('updates.example.invalid'), findsNothing);
        final retry = find.byKey(
          const ValueKey<String>('retry-update-details'),
        );
        await tester.ensureVisible(retry);
        await tester.pumpAndSettle();
        await tester.tap(retry);
        await tester.pumpAndSettle();
        expect(find.text(strings.versionReady('1.2.3')), findsOneWidget);
        expect(
          find.text('Version: 1.2.3\n\nBoard software release notes.'),
          findsOneWidget,
        );
        if (locale.languageCode != 'en' || locale.countryCode == 'XA') {
          expect(find.text(strings.originalNotes), findsOneWidget);
        }
        expect(tester.takeException(), isNull);
        final firmwareScroll = find
            .descendant(
              of: find.byType(FirmwareScreen),
              matching: find.byType(Scrollable),
            )
            .first;
        tester.state<ScrollableState>(firmwareScroll).position.jumpTo(0);
        await tester.pumpAndSettle();
        await captureHealthReview(tester, 'locale-$locale-2x-ios-firmware');
        await tester.tap(back);
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        expect(find.byType(HomeScreen), findsOneWidget);
        expect(find.text(strings.playTab), findsOneWidget);
        expect(
          find.byKey(const ValueKey<String>('firmware-back')),
          findsNothing,
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
    );
  }
}
