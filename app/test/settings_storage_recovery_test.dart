import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/board_storage_recovery.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

class _Ble extends Fake implements FlutterReactiveBle {}

class _RecoveryService extends LitchBleService {
  _RecoveryService()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _Ble());
  final connectionUpdates = StreamController<ConnectionStateUpdate>.broadcast(
    sync: true,
  );
  final recoveryUpdates =
      StreamController<BoardStorageRecoverySnapshot>.broadcast(sync: true);
  bool connected = true;
  bool savingIncident = true;
  bool unconfirmedSessionOnly = false;
  bool includeBoardHealth = true;
  HomeStateNotification? retainedHomeState;
  int token = 1;
  List<LitchSetting> settings = <LitchSetting>[];
  final writtenValues = <String, Object>{};
  BoardStorageRecoverySnapshot? retainedRecovery;
  int formatCalls = 0;
  int statusCalls = 0;
  final statusDeviceIds = <String>[];
  final formatDeviceIds = <String>[];
  Completer<BoardStorageRecoverySnapshot>? statusCompletion;
  Completer<BoardStorageRecoverySnapshot>? completion;
  BoardStorageRecoverySnapshot reported = const BoardStorageRecoverySnapshot(
    status: BoardStorageRecoveryStatus.idle,
  );

  @override
  bool get isConnected => connected;
  @override
  String? get connectedId => connected ? 'board-1' : null;
  @override
  int get connectionSessionToken => token;
  @override
  Stream<ConnectionStateUpdate> get connectionStream =>
      connectionUpdates.stream;
  @override
  Stream<BoardStorageRecoverySnapshot> get boardStorageRecoveryStream =>
      recoveryUpdates.stream;
  @override
  BoardStorageRecoverySnapshot? get latestBoardStorageRecovery =>
      retainedRecovery;
  @override
  HomeStateNotification? get latestHomeState => retainedHomeState;
  @override
  BoardHealthStatus? get latestBoardHealth => includeBoardHealth
      ? BoardHealthStatus(
          runtimeHealth: RuntimeHealthSnapshot.fromToken('000000000')!,
          persistence: savingIncident
              ? HomePersistenceDegradation.storageIo
              : null,
          sessionOnly: savingIncident || unconfirmedSessionOnly,
          resetReason: 0,
          bootCount: 1,
          watchdogRestarts: 0,
        )
      : null;
  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async =>
      RuntimeHealthAvailable(RuntimeHealthSnapshot.fromToken('000000000')!);
  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask: settings.isEmpty ? 0 : LitchDeviceCapabilities.settingsBit,
        explicitlyReported: true,
      );
  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async => settings;
  @override
  Future<LitchEndPalette?> readPhysicalEndPalette({
    required String deviceId,
  }) async => null;
  @override
  Future<bool> writeSetting({
    required String deviceId,
    required String key,
    required Object value,
    bool awaitAck = false,
    bool preferWithoutResponse = false,
  }) async {
    writtenValues[key] = value;
    return true;
  }

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    if (writtenValues.containsKey(key)) return writtenValues[key];
    for (final setting in settings) {
      if (setting.key == key) return setting.value;
    }
    return null;
  }

  @override
  Future<LitchSettingWriteResult> writeSettingAcknowledged({
    required String deviceId,
    required String key,
    required Object value,
  }) async => LitchSettingWriteAcknowledged(
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
  @override
  Future<BoardStorageRecoverySnapshot> fetchBoardStorageRecovery({
    required String deviceId,
  }) async {
    statusCalls++;
    statusDeviceIds.add(deviceId);
    return statusCompletion?.future ?? reported;
  }

  @override
  Future<BoardStorageRecoverySnapshot> reformatBoardStorage({
    required String deviceId,
  }) {
    formatCalls++;
    formatDeviceIds.add(deviceId);
    return completion!.future;
  }

  void simulateDisconnect({bool anonymous = false}) {
    connected = false;
    token++;
    connectionUpdates.add(
      ConnectionStateUpdate(
        deviceId: anonymous ? '' : 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
  }

  @override
  Future<void> dispose() async {
    await connectionUpdates.close();
    await recoveryUpdates.close();
    await super.dispose();
  }
}

Widget _harness(
  _RecoveryService service, {
  Locale locale = const Locale('en'),
  double scale = 1,
  String boardId = 'board-1',
}) => MaterialApp(
  locale: locale,
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(scale)),
    child: child!,
  ),
  home: Scaffold(
    body: SettingsScreen(
      bleService: service,
      boardId: boardId,
      enableAppConsole: false,
      onEnableAppConsoleChanged: (_) async {},
    ),
  ),
);
final _open = find.byKey(const ValueKey<String>('storage-reformat-open'));
final _confirm = find.byKey(const ValueKey<String>('storage-reformat-confirm'));
final _cancel = find.byKey(const ValueKey<String>('storage-reformat-cancel'));
final _dialog = find.byKey(
  const ValueKey<String>('storage-reformat-confirmation'),
);

Future<void> _openDialog(
  WidgetTester tester,
  _RecoveryService service, {
  Locale locale = const Locale('en'),
  double scale = 1,
  String boardId = 'board-1',
}) async {
  await tester.pumpWidget(
    _harness(service, locale: locale, scale: scale, boardId: boardId),
  );
  await tester.pumpAndSettle();
  final scrollable = find
      .ancestor(of: _open, matching: find.byType(Scrollable))
      .first;
  await tester.scrollUntilVisible(_open, 200, scrollable: scrollable);
  await tester.pumpAndSettle();
  await tester.tap(_open);
  await tester.pumpAndSettle();
  expect(_dialog, findsOneWidget);
}

void main() {
  Future<void> openSetupPage(WidgetTester tester, String page) async {
    final destination = find.byKey(ValueKey<String>('setup-open:$page'));
    await tester.ensureVisible(destination);
    await tester.pumpAndSettle();
    await tester.tap(destination);
    await tester.pumpAndSettle();
  }

  testWidgets(
    'session-only refusal offers information without erase recovery',
    (tester) async {
      final service = _RecoveryService()
        ..savingIncident = false
        ..unconfirmedSessionOnly = true;
      addTearDown(service.dispose);
      await tester.pumpWidget(_harness(service));
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
      expect(find.byKey(const ValueKey('setup-back')), findsNothing);
      final health = find.byKey(const ValueKey('runtime-health-card'));
      await tester.ensureVisible(health);
      await tester.pumpAndSettle();
      expect(health.hitTestable(), findsOneWidget);
      expect(find.text('Changes are not saved'), findsOneWidget);
      expect(
        find.textContaining('will be lost when the board turns off'),
        findsOneWidget,
      );
      expect(find.text('Saving needs attention'), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('board-health-priority')),
        findsNothing,
      );
      expect(_open, findsNothing);
      expect(service.statusCalls, 0);
      expect(service.formatCalls, 0);
    },
  );

  testWidgets(
    'empty Settings identity uses active board for storage confirmation',
    (tester) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service, boardId: '');
      expect(service.statusDeviceIds, ['board-1']);
      expect(service.formatCalls, 0);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      expect(service.formatDeviceIds, ['board-1']);
      service.completion!.complete(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.failed,
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.textContaining('Storage could not be reformatted'),
        findsOneWidget,
      );
    },
  );

  testWidgets('explicit Settings board excludes another board saving incident', (
    tester,
  ) async {
    final service = _RecoveryService()
      ..includeBoardHealth = false
      ..retainedHomeState = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":42,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1","occ":"8100000000000000",'
        '"g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"ps":"i"}',
      )!;
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service, boardId: 'board-2'));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
    expect(find.byKey(const ValueKey('setup-back')), findsNothing);
    final health = find.byKey(const ValueKey('runtime-health-card'));
    await tester.ensureVisible(health);
    await tester.pumpAndSettle();
    expect(health.hitTestable(), findsOneWidget);
    expect(find.text('Saving needs attention'), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('board-health-priority')),
      findsNothing,
    );
    expect(
      find.text('Connect to the board to read its health.'),
      findsOneWidget,
    );
    expect(_open, findsNothing);
    expect(service.statusCalls, 0);
    expect(service.formatCalls, 0);
  });

  testWidgets(
    'disconnect retires pending storage status without late support',
    (tester) async {
      final pending = Completer<BoardStorageRecoverySnapshot>();
      final service = _RecoveryService()..statusCompletion = pending;
      addTearDown(service.dispose);
      await tester.pumpWidget(_harness(service));
      await tester.pump();
      expect(service.statusDeviceIds, ['board-1']);
      service.simulateDisconnect();
      await tester.pump();
      expect(find.byType(CircularProgressIndicator), findsNothing);
      expect(_open, findsNothing);
      pending.complete(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.idle,
        ),
      );
      await tester.pumpAndSettle();
      expect(_open, findsNothing);
      expect(
        find.text('Connect to the board to read its health.'),
        findsOneWidget,
      );
      expect(service.formatCalls, 0);
    },
  );

  testWidgets(
    'storage erase is offered only in the existing saving incident after support proof',
    (tester) async {
      final service = _RecoveryService()..savingIncident = false;
      addTearDown(service.dispose);
      await tester.pumpWidget(_harness(service));
      await tester.pumpAndSettle();
      service.recoveryUpdates.add(service.reported);
      await tester.pumpAndSettle();
      expect(_open, findsNothing);
      expect(service.formatCalls, 0);
      expect(service.statusCalls, 0);
    },
  );

  testWidgets(
    'busy recovery names the operation exit and refreshes without erasing',
    (tester) async {
      final service = _RecoveryService()
        ..reported = const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.busy,
        );
      addTearDown(service.dispose);
      await tester.pumpWidget(_harness(service));
      await tester.pumpAndSettle();
      expect(
        find.textContaining('Finish the current board operation'),
        findsOneWidget,
      );
      expect(find.textContaining('leave Litchcraft'), findsOneWidget);
      expect(tester.widget<OutlinedButton>(_open).onPressed, isNull);
      final status = find.byKey(
        const ValueKey<String>('storage-reformat-status'),
      );
      await tester.ensureVisible(status);
      service.reported = const BoardStorageRecoverySnapshot(
        status: BoardStorageRecoveryStatus.idle,
      );
      await tester.tap(status);
      await tester.pumpAndSettle();
      expect(tester.widget<OutlinedButton>(_open).onPressed, isNotNull);
      expect(service.statusCalls, 2);
      expect(service.formatCalls, 0);
    },
  );

  testWidgets(
    'opening and cancelling discloses complete board erase without executing',
    (tester) async {
      final service = _RecoveryService();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      expect(find.textContaining('all files on the board'), findsOneWidget);
      expect(find.textContaining('saved games and artwork'), findsOneWidget);
      expect(
        find.textContaining('saved on this device are unaffected'),
        findsOneWidget,
      );
      expect(
        find.textContaining(
          'Readable board identity, calibration and protection',
        ),
        findsOneWidget,
      );
      expect(service.formatCalls, 0);
      await tester.tap(_cancel);
      await tester.pumpAndSettle();
      expect(_dialog, findsNothing);
      expect(service.formatCalls, 0);
      expect(_open, findsOneWidget);
    },
  );

  for (final anonymous in <bool>[false, true]) {
    testWidgets(
      'disconnect before confirmation cancels erase (anonymous=$anonymous)',
      (tester) async {
        final service = _RecoveryService();
        addTearDown(service.dispose);
        await _openDialog(tester, service);
        service.simulateDisconnect(anonymous: anonymous);
        await tester.pumpAndSettle();
        expect(_dialog, findsNothing);
        expect(service.formatCalls, 0);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'confirmation starts once, progress stays clear, and failed format retains play',
    (tester) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      expect(service.formatCalls, 1);
      expect(_open, findsNothing);
      expect(find.textContaining('Reformatting board storage'), findsOneWidget);
      service.recoveryUpdates.add(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.running,
        ),
      );
      await tester.pump();
      expect(service.formatCalls, 1);
      service.completion!.complete(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.failed,
        ),
      );
      await tester.pumpAndSettle();
      expect(find.textContaining('You can keep playing'), findsOneWidget);
      expect(
        find.textContaining('changes remain for this session only'),
        findsOneWidget,
      );
      expect(_open, findsOneWidget);
      expect(tester.widget<OutlinedButton>(_open).onPressed, isNotNull);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'verified success explains explicit restart and loss of live session',
    (tester) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      service.completion!.complete(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.succeeded,
          rebootRequired: true,
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.textContaining('Restart the board to use saving again'),
        findsOneWidget,
      );
      expect(find.textContaining('will be lost on restart'), findsOneWidget);
      expect(_open, findsNothing);
      expect(service.formatCalls, 1);
    },
  );

  testWidgets(
    'unknown receipt offers status query and never automatically repeats erase',
    (tester) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      service.completion!.complete(BoardStorageRecoverySnapshot.unknown);
      await tester.pumpAndSettle();
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(_open, findsNothing);
      final status = find.byKey(
        const ValueKey<String>('storage-reformat-status'),
      );
      await tester.ensureVisible(status);
      service.reported = const BoardStorageRecoverySnapshot(
        status: BoardStorageRecoveryStatus.succeeded,
        rebootRequired: true,
      );
      await tester.tap(status);
      await tester.pumpAndSettle();
      expect(find.textContaining('Restart the board'), findsOneWidget);
      expect(service.formatCalls, 1);
    },
  );

  testWidgets(
    'failed retry requires a fresh full erase dialog and explicit confirmation',
    (tester) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      service.completion!.complete(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.failed,
        ),
      );
      await tester.pumpAndSettle();
      expect(service.formatCalls, 1);
      expect(_open, findsOneWidget);
      expect(tester.widget<OutlinedButton>(_open).onPressed, isNotNull);
      await tester.ensureVisible(_open);
      await tester.tap(_open);
      await tester.pumpAndSettle();
      expect(_dialog, findsOneWidget);
      expect(find.textContaining('all files on the board'), findsOneWidget);
      expect(find.textContaining('saved games and artwork'), findsOneWidget);
      expect(
        find.textContaining('saved on this device are unaffected'),
        findsOneWidget,
      );
      expect(
        service.formatCalls,
        1,
        reason: 'A prior confirmation cannot authorize the retry.',
      );
      await tester.tap(_cancel);
      await tester.pumpAndSettle();
      expect(service.formatCalls, 1);
      await tester.tap(_open);
      await tester.pumpAndSettle();
      service.completion = Completer<BoardStorageRecoverySnapshot>();
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      expect(service.formatCalls, 2);
      expect(_open, findsNothing);
      expect(find.textContaining('Reformatting board storage'), findsOneWidget);
      service.completion!.complete(BoardStorageRecoverySnapshot.unknown);
      await tester.pumpAndSettle();
      expect(_open, findsNothing);
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(
        service.formatCalls,
        2,
        reason: 'An uncertain retry result cannot submit another erase.',
      );
    },
  );

  for (final terminal in <BoardStorageRecoveryStatus>[
    BoardStorageRecoveryStatus.failed,
    BoardStorageRecoveryStatus.succeeded,
  ]) {
    testWidgets('typed Unknown is not replaced by cached ${terminal.name}', (
      tester,
    ) async {
      final service = _RecoveryService()
        ..completion = Completer<BoardStorageRecoverySnapshot>();
      addTearDown(service.dispose);
      await _openDialog(tester, service);
      await tester.tap(_confirm);
      await tester.pump(const Duration(milliseconds: 350));
      service.retainedRecovery = BoardStorageRecoverySnapshot(
        status: terminal,
        rebootRequired: terminal == BoardStorageRecoveryStatus.succeeded,
      );
      service.recoveryUpdates.add(service.retainedRecovery!);
      service.completion!.complete(BoardStorageRecoverySnapshot.unknown);
      await tester.pumpAndSettle();
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(
        find.textContaining('Storage could not be reformatted'),
        findsNothing,
      );
      expect(find.textContaining('Restart the board'), findsNothing);
      expect(_open, findsNothing);
      expect(
        find.byKey(const ValueKey<String>('storage-reformat-status')),
        findsOneWidget,
      );
    });
  }

  testWidgets(
    'service replacement cancels confirmation before any submission',
    (tester) async {
      final first = _RecoveryService();
      final second = _RecoveryService();
      addTearDown(first.dispose);
      addTearDown(second.dispose);
      await _openDialog(tester, first);
      await tester.pumpWidget(_harness(second));
      await tester.pumpAndSettle();
      expect(_dialog, findsNothing);
      expect(first.formatCalls, 0);
      expect(second.formatCalls, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('disconnect after submission ignores an old terminal success', (
    tester,
  ) async {
    final service = _RecoveryService()
      ..completion = Completer<BoardStorageRecoverySnapshot>();
    addTearDown(service.dispose);
    await _openDialog(tester, service);
    await tester.tap(_confirm);
    await tester.pump(const Duration(milliseconds: 350));
    service.simulateDisconnect();
    service.completion!.complete(
      const BoardStorageRecoverySnapshot(
        status: BoardStorageRecoveryStatus.succeeded,
        rebootRequired: true,
      ),
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('was reformatted'), findsNothing);
    expect(
      find.textContaining('result could not be confirmed'),
      findsOneWidget,
    );
    expect(service.formatCalls, 1);
  });

  testWidgets(
    'late recovery status preserves held multi-point slider geometry and preview',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(360, 1500));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final service = _RecoveryService()
        ..reported = BoardStorageRecoverySnapshot.unknown
        ..settings = const <LitchSetting>[
          LitchSetting(
            key: 'WHITE_SQUARES_ON',
            type: 'bool',
            category: 'display',
            value: true,
            defaultValue: true,
          ),
          LitchSetting(
            key: 'LIGHT_SQUARE_HUE',
            type: 'u32',
            category: 'display',
            value: 85,
            defaultValue: 85,
            min: 0,
            max: 255,
          ),
          LitchSetting(
            key: 'LIGHT_SQUARE_MODE',
            type: 'enum',
            category: 'display',
            value: 'white',
            defaultValue: 'white',
            options: <String>['off', 'white', 'color'],
          ),
          LitchSetting(
            key: 'LIGHT_SQUARE_BRIGHTNESS',
            type: 'u32',
            category: 'display',
            value: 4,
            defaultValue: 4,
            min: 1,
            max: 8,
          ),
        ];
      final board = SettingsSessionBoard()
        ..configureSettings = (data) {
          data
            ..setUint16(74, 85, Endian.little) // LIGHT_SQUARE_HUE
            ..setUint16(76, 4, Endian.little); // LIGHT_SQUARE_BRIGHTNESS
        };
      addTearDown(() => disposeBoard(service, board));
      for (final packet in board.settingsPackets()) {
        service.debugCaptureConnectionSessionState(packet);
      }
      await tester.pumpWidget(_harness(service));
      await tester.pumpAndSettle();
      await openSetupPage(tester, 'lights');
      final slider = find.byKey(
        const ValueKey<String>('setting-editor:LIGHT_SQUARE_BRIGHTNESS'),
      );
      expect(slider, findsOneWidget);
      final scrollable = find
          .ancestor(of: slider, matching: find.byType(Scrollable))
          .first;
      await tester.scrollUntilVisible(slider, 200, scrollable: scrollable);
      await tester.pumpAndSettle();
      final originalRect = tester.getRect(slider);
      final initialSlider = tester.widget<Slider>(slider);
      expect(initialSlider.onChanged, isNotNull);
      final gesture = await tester.startGesture(originalRect.center);
      await tester.pump(const Duration(milliseconds: 40));
      await gesture.moveTo(
        Offset(
          originalRect.left + originalRect.width * 0.7,
          originalRect.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 100));
      final heldValue = tester.widget<Slider>(slider).value;
      expect(heldValue, greaterThan(initialSlider.value));
      service.recoveryUpdates.add(
        const BoardStorageRecoverySnapshot(
          status: BoardStorageRecoveryStatus.idle,
        ),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.getRect(slider), originalRect);
      expect(tester.widget<Slider>(slider).value, heldValue);
      expect(
        _open,
        findsNothing,
        reason:
            'A newly available recovery action must wait for gesture commit.',
      );
      await gesture.moveTo(
        Offset(
          originalRect.left + originalRect.width * 0.85,
          originalRect.center.dy,
        ),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.getRect(slider), originalRect);
      expect(tester.widget<Slider>(slider).value, greaterThan(heldValue));
      await gesture.up();
      await tester.pumpAndSettle();
      expect(_open, findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('unsupported storage recovery has no dead erase action', (
    tester,
  ) async {
    final service = _RecoveryService()
      ..reported = BoardStorageRecoverySnapshot.unavailable;
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service));
    await tester.pumpAndSettle();
    expect(_open, findsNothing);
    expect(service.formatCalls, 0);
  });

  for (final locale in <String>['en', 'en_GB', 'fr', 'de', 'es', 'it']) {
    testWidgets(
      'erase confirmation fits 200% text with complete targets in $locale',
      (tester) async {
        await tester.binding.setSurfaceSize(const Size(320, 700));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final service = _RecoveryService();
        addTearDown(service.dispose);
        final parts = locale.split('_');
        await _openDialog(
          tester,
          service,
          locale: Locale(parts[0], parts.length > 1 ? parts[1] : null),
          scale: 2,
        );
        for (final target in <Finder>[_cancel, _confirm]) {
          final rect = tester.getRect(target);
          expect(rect.height, greaterThanOrEqualTo(48));
          expect(rect.width, greaterThanOrEqualTo(48));
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(320));
          expect(rect.bottom, lessThanOrEqualTo(700));
          expect(tester.getSemantics(target).flagsCollection.isButton, isTrue);
        }
        expect(tester.takeException(), isNull);
        await tester.tap(_cancel);
        await tester.pumpAndSettle();
        expect(service.formatCalls, 0);
      },
    );
  }
}
