import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/services/home_board_feed.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

class _FakeFlutterReactiveBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}

class _FakeLitchBleService extends LitchBleService {
  _FakeLitchBleService()
    : super(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FakeFlutterReactiveBle(),
      );

  int healthReads = 0;
  LitchDeviceCapabilities capabilities = const LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask:
        LitchDeviceCapabilities.consoleBit |
        LitchDeviceCapabilities.settingsBit,
    explicitlyReported: true,
  );

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) => capabilities;

  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async {
    ++healthReads;
    return const RuntimeHealthUnavailable(
      RuntimeHealthUnavailableReason.unsupported,
    );
  }

  final StreamController<List<String>> controller =
      StreamController<List<String>>.broadcast();
  final StreamController<HomeStateNotification> homeStateController =
      StreamController<HomeStateNotification>.broadcast(sync: true);
  final StreamController<String> settingsController =
      StreamController<String>.broadcast();
  final StreamController<ConnectionStateUpdate> connectionController =
      StreamController<ConnectionStateUpdate>.broadcast();
  final StreamController<LitchControlEvent> controlController =
      StreamController<LitchControlEvent>.broadcast(sync: true);
  List<String> initialConsoleLines = const <String>[];
  HomeStateNotification? retainedHomeState;
  final List<String> commands = <String>[];
  final List<String> settingReads = <String>[];
  final Map<String, Object> latestSettings = <String, Object>{
    'GAME_FIRST_PLAYER': 1,
  };
  final Map<String, Object> readSettingsValues = <String, Object>{};
  Completer<bool>? nextSnapshotRequest;
  Completer<Object?>? pendingPlayerEndRead;
  int snapshotRequests = 0;
  int paletteReads = 0;
  bool connected = true;
  void Function(int request)? onSnapshotRequest;

  @override
  Stream<List<String>> get consoleStream => controller.stream;

  @override
  Stream<HomeStateNotification> get homeStateStream =>
      homeStateController.stream;

  @override
  HomeStateNotification? get latestHomeState => retainedHomeState;

  @override
  List<String> get consoleLinesSnapshot => initialConsoleLines;

  @override
  Stream<String> get settingsChangedStream => settingsController.stream;

  @override
  Stream<ConnectionStateUpdate> get connectionStream =>
      connectionController.stream;

  @override
  Stream<LitchControlEvent> get controlActionEvents => controlController.stream;

  @override
  Object? latestSettingValue(String key) => latestSettings[key];

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    settingReads.add('$deviceId:$key');
    if (key == 'GAME_FIRST_PLAYER' && pendingPlayerEndRead != null) {
      return pendingPlayerEndRead!.future;
    }
    return readSettingsValues[key];
  }

  @override
  Future<LitchEndPalette?> readPhysicalEndPalette({
    required String deviceId,
  }) async {
    paletteReads += 1;
    final Object? p1Raw =
        latestSettings['P1_PIECE_WHEEL'] ??
        readSettingsValues['P1_PIECE_WHEEL'];
    final Object? p2Raw =
        latestSettings['P2_PIECE_WHEEL'] ??
        readSettingsValues['P2_PIECE_WHEEL'];
    final int? p1 = int.tryParse(p1Raw?.toString() ?? '');
    final int? p2 = int.tryParse(p2Raw?.toString() ?? '');
    if (p1 == null || p2 == null) {
      return null;
    }
    return LitchEndPalette(
      p1ControlsWheel: p1,
      p2ControlsWheel: p2,
      usesDirectPhysicalKeys: false,
    );
  }

  @override
  bool get productReady =>
      connected &&
      capabilities.supportsConnectionSession &&
      !capabilities.recoveryOnly;

  @override
  Future<bool> ensureProductReady(
    String deviceId, {
    bool retry = false,
  }) async => productReady;

  @override
  bool get isConnected => connected;

  @override
  String? get connectedId => connected ? 'connected-board' : null;

  void emitConnection(DeviceConnectionState state) {
    connected = state == DeviceConnectionState.connected;
    connectionController.add(
      ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: state,
        failure: null,
      ),
    );
  }

  @override
  Future<bool> sendConsoleLine(String deviceId, String line) async {
    commands.add('$deviceId:$line');
    return true;
  }

  void emitHomeState(String line) {
    homeStateController.add(HomeStateNotification.tryParse(line)!);
  }

  @override
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async {
    snapshotRequests += 1;
    final Completer<bool>? deferred = nextSnapshotRequest;
    nextSnapshotRequest = null;
    onSnapshotRequest?.call(snapshotRequests);
    if (deferred != null) return deferred.future;
    return true;
  }

  @override
  Future<void> dispose() async {
    await controller.close();
    await homeStateController.close();
    await settingsController.close();
    await connectionController.close();
    await controlController.close();
    await super.dispose();
  }
}

void main() {
  HomeStateNotification cpuState(
    int sequence, {
    String action = 'progress',
    String side = 'w',
    String? thinking = '["w",6,8]',
    String extra = '',
  }) => HomeStateNotification.tryParse(
    '{"event":"home_state","v":1,"seq":$sequence,"a":"$action",'
    '"fen":"8/8/8/8/8/8/8/K6k $side - - 0 1",'
    '"occ":"8100000000000000","g":"c","duck":"--",'
    '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
    '"cl":[1,299,300,"$side",1,0,3,"n"]'
    '${thinking == null ? '' : ',"th":$thinking'}$extra}',
  )!;

  testWidgets(
    'feed created before transport waits without spending its retry budget',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..connected = false;
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board-1',
        refreshTimeout: const Duration(milliseconds: 50),
        automaticRetryDelay: const Duration(milliseconds: 10),
      );
      addTearDown(feed.dispose);
      addTearDown(ble.dispose);

      expect(feed.current.isRefreshing, isTrue);
      await feed.refresh();
      await tester.pump(const Duration(seconds: 1));
      expect(ble.snapshotRequests, 0);
      expect(ble.healthReads, 0);
      expect(ble.settingReads, isEmpty);
      expect(ble.paletteReads, 0);
      expect(feed.current.refreshMessage, isNull);

      ble.emitConnection(DeviceConnectionState.connected);
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await tester.pump();
      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 1);
      expect(ble.healthReads, 1);
      expect(ble.paletteReads, 1);
      expect(feed.current.isRefreshing, isTrue);
      await tester.runAsync(feed.dispose);
    },
  );

  testWidgets(
    'CPU estimate expiry cannot be extended by stale or retained Home',
    (tester) async {
      final ble = _FakeLitchBleService()..retainedHomeState = cpuState(7);
      final feed = HomeBoardFeed(bleService: ble, boardId: 'board-1');
      addTearDown(ble.dispose);
      try {
        expect(feed.current.showsCpuThinking, isTrue);
        expect(
          feed.current.cpuThinking?.remainingSeconds,
          isNull,
          reason: 'A retained record has no fresh arrival-time evidence.',
        );

        ble.homeStateController.add(cpuState(8));
        expect(feed.current.cpuThinking?.remainingSeconds, 8);
        await tester.pump(const Duration(milliseconds: 2999));
        ble.homeStateController.add(cpuState(7, thinking: '["w",3,20]'));
        ble.homeStateController.add(cpuState(8));
        expect(feed.current.liveSequence, 8);
        expect(feed.current.cpuThinking?.remainingSeconds, 8);
        await tester.pump(const Duration(milliseconds: 1));
        expect(feed.current.showsCpuThinking, isTrue);
        expect(feed.current.cpuThinking?.remainingSeconds, isNull);
        expect(feed.current.clockStatus?.whiteRemainingSeconds, 299);

        ble.homeStateController.add(cpuState(9, thinking: '["w",12,1]'));
        await tester.pump(const Duration(seconds: 1));
        expect(
          feed.current.cpuThinking?.remainingSeconds,
          isNull,
          reason: 'An overdue estimate becomes unknown, never zero or ready.',
        );
        ble.homeStateController.add(cpuState(10, thinking: '["w",14,5]'));
        await tester.pump(const Duration(seconds: 2));
        ble.homeStateController.add(cpuState(11, thinking: '["w",18,4]'));
        await tester.pump(const Duration(seconds: 2));
        expect(
          feed.current.cpuThinking?.remainingSeconds,
          4,
          reason: 'The canceled older timer must not erase a fresh estimate.',
        );
      } finally {
        await tester.runAsync(feed.dispose);
      }
    },
  );

  testWidgets('CPU estimate stays unknown after a delayed player-end read', (
    tester,
  ) async {
    final read = Completer<Object?>();
    final ble = _FakeLitchBleService()
      ..latestSettings.remove('GAME_FIRST_PLAYER')
      ..pendingPlayerEndRead = read;
    final feed = HomeBoardFeed(bleService: ble, boardId: 'board-1');
    addTearDown(ble.dispose);
    try {
      ble.homeStateController.add(cpuState(1));
      expect(feed.current.hasPosition, isFalse);
      await tester.pump(const Duration(seconds: 4));
      read.complete(2);
      await tester.pump();
      expect(feed.current.showsCpuThinking, isTrue);
      expect(feed.current.cpuThinking?.remainingSeconds, isNull);
      expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
      expect(feed.current.clockStatus?.whiteRemainingSeconds, 299);
      ble.homeStateController.add(cpuState(2));
      expect(feed.current.cpuThinking?.remainingSeconds, 8);
    } finally {
      await tester.runAsync(feed.dispose);
    }
  });

  testWidgets(
    'CPU feed retires authority for ready, omission, role change and reconnect',
    (tester) async {
      final ble = _FakeLitchBleService()
        ..latestSettings.addAll({'VS_CPU_LEVEL': 6, 'VS_CPU_PLAYER': 1});
      final feed = HomeBoardFeed(bleService: ble, boardId: 'board-1');
      addTearDown(ble.dispose);
      try {
        ble.homeStateController.add(cpuState(7));
        ble.homeStateController.add(cpuState(8, action: 'corr'));
        expect(feed.current.cpuThinking, isNotNull);
        expect(feed.current.showsCpuThinking, isFalse);
        ble.homeStateController.add(cpuState(9));
        expect(feed.current.showsCpuThinking, isTrue);
        ble.homeStateController.add(
          cpuState(10, action: 'cpu', extra: ',"c":["a1","b1","--"]'),
        );
        expect(feed.current.cpuThinking, isNull);
        expect(feed.current.physicalMovePrompt?.toSquare, 57);
        ble.homeStateController.add(cpuState(9));
        expect(
          feed.current.cpuThinking,
          isNull,
          reason: 'Old search cannot replace a ready move.',
        );
        ble.homeStateController.add(cpuState(11));
        ble.homeStateController.add(cpuState(12, thinking: null));
        expect(feed.current.cpuThinking, isNull);
        ble.homeStateController.add(cpuState(13));
        ble.homeStateController.add(cpuState(14, thinking: '["w",6,0]'));
        expect(feed.current.cpuThinking, isNull);
        expect(feed.current.clockStatus?.running, isTrue);
        ble.homeStateController.add(cpuState(15));
        ble.settingsController.add('VS_CPU_LEVEL');
        await tester.pump();
        expect(
          feed.current.showsCpuThinking,
          isTrue,
          reason: 'An unchanged settings replay is neutral.',
        );
        ble.latestSettings['VS_CPU_LEVEL'] = 0;
        ble.settingsController.add('VS_CPU_LEVEL');
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
        ble.latestSettings['VS_CPU_LEVEL'] = 6;
        ble.settingsController.add('VS_CPU_LEVEL');
        await tester.pump();
        ble.homeStateController.add(cpuState(16));
        ble.connectionController.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
        ble.homeStateController.add(cpuState(17));
        expect(feed.current.cpuThinking, isNull);
        ble.latestSettings['VS_CPU_PLAYER'] = 2;
        ble.connectionController.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
        ble.homeStateController.add(
          cpuState(1, side: 'b', thinking: '["b",0,9]'),
        );
        expect(feed.current.cpuThinking?.side, HomeSide.black);
        expect(feed.current.showsCpuThinking, isTrue);
        ble.controlController.add(
          const LitchControlEvent(
            action: LitchControlAction.newGame,
            phase: LitchControlPhase.applied,
          ),
        );
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
      } finally {
        await tester.runAsync(feed.dispose);
      }
    },
  );

  test(
    'Duck plan clears on omission and disconnect before new-session replay',
    () async {
      HomeStateNotification state(
        int seq,
        String? plan, {
        String action = 'duck',
      }) => HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":$seq,"a":"$action",'
        '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
        '"occ":"8100000008000000","g":"d","duck":"d5",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
        '"cl":[0,0,0,"w",0,0,0,"n"]'
        '${plan == null ? '' : ',"dr":"$plan"'}}',
      )!;
      final ble = _FakeLitchBleService()..retainedHomeState = state(7, 'cd5e4');
      addTearDown(ble.dispose);
      final feed = HomeBoardFeed(bleService: ble, boardId: 'board-1');
      addTearDown(feed.dispose);
      expect(feed.current.visibleDuckRelocation?.toSquare, 36);
      ble.homeStateController.add(state(8, 'cd5e4', action: 'corr'));
      expect(feed.current.isDuckRelocationCorrection, isTrue);
      expect(feed.current.gameStatus?.duckMovePending, isTrue);
      expect(feed.current.visibleDuckRelocation, isNull);
      ble.homeStateController.add(state(9, null, action: 'corr'));
      expect(feed.current.isDuckRelocationCorrection, isFalse);
      expect(feed.current.gameStatus?.duckMovePending, isFalse);
      expect(feed.current.visibleDuckRelocation, isNull);
      ble.homeStateController.add(state(10, 'cd5e4'));
      expect(feed.current.visibleDuckRelocation?.toSquare, 36);
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.visibleDuckRelocation, isNull);
      expect(feed.current.isDuckRelocationCorrection, isFalse);
      ble.retainedHomeState = null;
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      ble.homeStateController.add(state(1, 'he4--'));
      expect(
        feed.current.visibleDuckRelocation?.owner,
        HomeDuckRelocationOwner.human,
      );
      expect(feed.current.visibleDuckRelocation?.fromSquare, 36);
      expect(feed.current.visibleDuckRelocation?.toSquare, isNull);
    },
  );

  for (final timer in [0, 4]) {
    test(
      'Duck relocation correction cannot recover an unused clock (TIMER=$timer)',
      () async {
        final ble = _FakeLitchBleService()..readSettingsValues['TIMER'] = timer;
        addTearDown(ble.dispose);
        final feed = HomeBoardFeed(
          bleService: ble,
          boardId: 'fallback-board',
          automaticRetryDelay: const Duration(milliseconds: 5),
        );
        addTearDown(feed.dispose);
        ble.emitHomeState(
          '{"event":"home_state","v":1,"seq":2,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFEF00100001FFFF","g":"d","duck":"d5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,"dr":"cd5c6"}',
        );
        await Future<void>.delayed(const Duration(milliseconds: 25));
        expect(ble.snapshotRequests, 2);
        expect(ble.settingReads, isEmpty);
        expect(feed.current.gameStatus?.duckMovePending, isTrue);
        expect(feed.current.isDuckRelocationCorrection, isTrue);
        expect(feed.current.visibleDuckRelocation, isNull);
        expect(feed.current.clockStatus, isNull);
        expect(feed.current.isClockRefreshing, isFalse);
      },
    );
  }

  test(
    'live assistance hydrates, clears on omission, and clears on disconnect',
    () async {
      HomeStateNotification state(int sequence, String? assistance) =>
          HomeStateNotification.tryParse(
            '{"event":"home_state","v":1,"seq":$sequence,"a":"idle",'
            '"fen":"8/8/8/8/8/8/4K3/7k w - - 0 1",'
            '"occ":"0000000000000000","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
            '"cl":[0,0,0,"w",0,0,0,"n"]'
            '${assistance == null ? '' : ',"as":"$assistance"'}}',
          )!;
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..retainedHomeState = state(7, '3he2e4');
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board-1',
      );
      addTearDown(feed.dispose);
      expect(feed.current.assistance?.move, (from: 52, to: 36));
      ble.homeStateController.add(state(8, null));
      expect(feed.current.assistance, isNull);
      ble.homeStateController.add(state(9, '3he2e4'));
      expect(feed.current.assistance?.isReady, isTrue);
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.assistance, isNull);
      ble.retainedHomeState = null;
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      ble.homeStateController.add(state(1, '3he2e4'));
      expect(feed.current.assistance?.move, (from: 52, to: 36));
    },
  );

  test('hydrates retained typed Home state without console history', () {
    final _FakeLitchBleService ble = _FakeLitchBleService()
      ..retainedHomeState = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":7,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
        '"cl":[0,0,0,"w",0,0,0,"y"]}',
      );
    addTearDown(ble.dispose);

    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'fallback-board',
    );
    addTearDown(feed.dispose);

    expect(ble.initialConsoleLines, isEmpty);
    expect(feed.current.liveSequence, 7);
    expect(feed.current.clockStatus?.timerSetting, 0);
  });

  for (final bool connectedAtConstruction in <bool>[true, false]) {
    test('diagnostic Home history cannot restore authority '
        '(connected at construction: $connectedAtConstruction)', () async {
      const String historical =
          '{"event":"home_state","v":1,"seq":9,"a":"cpu",'
          '"fen":"8/8/8/8/8/8/4K3/7k b - - 1 1",'
          '"occ":"0000000000001080","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"c":["g8","f6","--"]}';
      const List<String> history = <String>[
        historical,
        '10:00:00.001 [FW] old diagnostic A',
        '10:00:00.002 [FW] old diagnostic B',
      ];
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..connected = connectedAtConstruction
        ..initialConsoleLines = history;
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board-1',
      );
      addTearDown(feed.dispose);

      expect(ble.latestHomeState, isNull);
      expect(feed.current.liveSequence, isNull);
      expect(feed.current.hasPosition, isFalse);
      expect(feed.current.physicalMovePrompt, isNull);

      if (connectedAtConstruction) {
        ble.emitConnection(DeviceConnectionState.disconnected);
        await Future<void>.delayed(Duration.zero);
      }
      ble.emitConnection(DeviceConnectionState.connected);
      await Future<void>.delayed(Duration.zero);

      // These older snapshots were queued before the service exposed its
      // longer retained history. A shorter prefix must remain diagnostic.
      ble.controller.add(history.sublist(0, 2));
      ble.controller.add(history.sublist(0, 1));
      await Future<void>.delayed(Duration.zero);
      expect(ble.latestHomeState, isNull);
      expect(feed.current.liveSequence, isNull);
      expect(feed.current.hasPosition, isFalse);
      expect(feed.current.physicalMovePrompt, isNull);

      ble.emitHomeState(
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/4K2k w - - 0 1",'
        '"occ":"0000000000000090","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      );
      expect(feed.current.liveSequence, 1);
      expect(feed.current.hasPosition, isTrue);
      expect(feed.current.physicalMovePrompt, isNull);
    });
  }

  test(
    'hydrates and clears retained persistence degradation with live state',
    () {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..retainedHomeState = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":7,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,"ps":"i"}',
        );
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
      );
      addTearDown(feed.dispose);

      expect(
        feed.current.persistenceDegradation,
        HomePersistenceDegradation.storageIo,
      );
      final HomeStateNotification recovered = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":8,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      )!;
      ble.homeStateController.add(recovered);

      expect(feed.current.persistenceDegradation, isNull);
    },
  );

  test('applies live typed Home state without a console update', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService()
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask:
            LitchDeviceCapabilities.consoleBit |
            LitchDeviceCapabilities.settingsBit,
        explicitlyReported: true,
      );
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'fallback-board',
    );
    addTearDown(feed.dispose);
    expect(ble.productReady, isTrue);
    expect(feed.current.liveSequence, isNull);

    await feed.refresh();
    expect(ble.snapshotRequests, 1);
    expect(
      ble.commands,
      isEmpty,
      reason: 'Shared support is known before the first typed Home snapshot.',
    );

    // A complete fresh Console reply cannot establish Play authority, even
    // while a typed readback is outstanding and no snapshot has arrived yet.
    ble.controller.add(const <String>[
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
      '10:00:00.001 [FW] game: variant=classic duck=-- phase=idle '
          'pending=0 outcome=none winner=none variant_hash=0x1 app_hash=0x2',
      '10:00:00.002 [FW] rotate=0 firstplayer=1 '
          '(white_owner=P1 square_rotate=0) placement_mode=0 i2c_errors=0',
      '10:00:00.003 [FW] mapped_occ64=0xffff00000000ffff',
    ]);
    await Future<void>.delayed(Duration.zero);
    expect(
      ble.commands,
      isEmpty,
      reason: 'Console replies cannot request FEN.',
    );
    expect(feed.current.liveSequence, isNull);
    expect(feed.current.hasPosition, isFalse);

    final HomeStateNotification state = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":8,"a":"lift",'
      '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
      '"occ":"FFFF00000000FFFE","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    )!;

    ble.retainedHomeState = state;
    ble.homeStateController.add(state);

    expect(ble.initialConsoleLines, isEmpty);
    expect(ble.commands, isEmpty);
    expect(feed.current.liveSequence, 8);
    expect(feed.current.hasPosition, isTrue);
    expect(feed.current.homeStateAction, HomeStateAction.lift);
  });

  test(
    'hydrates an authoritative clock snapshot received before Home opens',
    () {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..retainedHomeState = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":7,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"cl":[0,0,0,"w",0,0,0,"y"]}',
        );
      addTearDown(ble.dispose);

      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
      );
      addTearDown(feed.dispose);

      expect(feed.current.liveSequence, 7);
      expect(feed.current.clockStatus?.timerSetting, 0);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.available,
      );
    },
  );

  testWidgets(
    'BOARD_ROTATE preserves fixed view corners and end hues while player assignment changes engine hit-testing',
    (tester) async {
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final feed = HomeBoardFeed(bleService: ble, boardId: 'board');
      addTearDown(feed.dispose);
      var sequence = 0;
      for (final first in [1, 2]) {
        ble.latestSettings
          ..['GAME_FIRST_PLAYER'] = first
          ..['P1_PIECE_WHEEL'] = 42
          ..['P2_PIECE_WHEEL'] = 203;
        ble.settingsController.add('GAME_FIRST_PLAYER');
        ble.settingsController.add('P1_PIECE_WHEEL');
        await tester.pump();
        for (final rot in [0, 1, 0]) {
          ble.emitHomeState(
            '{"event":"home_state","v":1,"seq":${++sequence},"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":$rot,"white":1,"pm":0,"cl":[0,0,0,"w",0,0,0,"y"]}',
          );
          await tester.pump();
          int? pressedSquare;
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: HomeChessboard(
                  snapshot: feed.current,
                  onSquarePressed: (square) => pressedSquare = square,
                ),
              ),
            ),
          );
          await tester.pump();
          final p1Y = tester
              .getCenter(
                find.byKey(
                  ValueKey<String>(first == 1 ? 'piece-e1' : 'piece-d8'),
                ),
              )
              .dy;
          final p2Y = tester
              .getCenter(
                find.byKey(
                  ValueKey<String>(first == 1 ? 'piece-e8' : 'piece-d1'),
                ),
              )
              .dy;
          final squareRotated = rot == 1;
          expect(
            p1Y > p2Y,
            first == 1,
            reason: 'the phone mirrors the physical square orientation',
          );
          // Rotating the hardware and enabling BOARD_ROTATE cancel visually.
          // Player-end placement alone determines which player's end is near.
          const corners = ['a8', 'h8', 'a1', 'h1'];
          final topLeft = tester.getCenter(
            find.byKey(ValueKey('square-${corners[0]}')),
          );
          final topRight = tester.getCenter(
            find.byKey(ValueKey('square-${corners[1]}')),
          );
          final bottomLeft = tester.getCenter(
            find.byKey(ValueKey('square-${corners[2]}')),
          );
          final bottomRight = tester.getCenter(
            find.byKey(ValueKey('square-${corners[3]}')),
          );
          expect(topLeft.dx, lessThan(topRight.dx));
          expect(topLeft.dy, topRight.dy);
          expect(bottomLeft.dx, lessThan(bottomRight.dx));
          expect(bottomLeft.dy, bottomRight.dy);
          expect(topLeft.dy, lessThan(bottomLeft.dy));
          await tester.tapAt(topLeft);
          expect(pressedSquare, first == 1 ? 0 : 63);
          expect(
            feed.current.physicalStatus?.squareRotated,
            squareRotated != (first == 2),
          );
          expect(feed.current.p1PieceWheel, 42);
          expect(feed.current.p2PieceWheel, 203);
        }
      }
    },
  );

  test('projects the typed player-end setting onto a Home snapshot', () {
    final _FakeLitchBleService ble = _FakeLitchBleService()
      ..latestSettings['GAME_FIRST_PLAYER'] = 2
      ..retainedHomeState = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":7,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      );
    addTearDown(ble.dispose);

    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'fallback-board',
    );
    addTearDown(feed.dispose);

    expect(feed.current.liveSequence, 7);
    expect(feed.current.physicalStatus?.whiteOwner, 'P1');
    expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
    expect(feed.current.physicalStatus?.squareRotated, isTrue);
  });

  test('waits for the typed player-end setting before showing Home', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService()
      ..latestSettings.remove('GAME_FIRST_PLAYER')
      ..readSettingsValues['GAME_FIRST_PLAYER'] = 2
      ..retainedHomeState = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":7,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      );
    addTearDown(ble.dispose);

    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'fallback-board',
    );
    addTearDown(feed.dispose);

    expect(feed.current.liveSequence, isNull);
    expect(feed.current.hasPosition, isFalse);
    await Future<void>.delayed(Duration.zero);

    expect(ble.settingReads, <String>['connected-board:GAME_FIRST_PLAYER']);
    expect(feed.current.liveSequence, 7);
    expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
    expect(feed.current.physicalStatus?.squareRotated, isTrue);
  });

  test(
    'keeps player-end orientation unresolved after a transient read failure',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..latestSettings.remove('GAME_FIRST_PLAYER')
        ..retainedHomeState = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":7,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
        );
      addTearDown(ble.dispose);

      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
      );
      addTearDown(feed.dispose);

      expect(feed.current.liveSequence, isNull);
      await Future<void>.delayed(Duration.zero);

      expect(feed.current.liveSequence, isNull);
      expect(feed.current.hasPosition, isFalse);
    },
  );

  testWidgets(
    'keeps a hydrated typed position quiet when a readback times out',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..retainedHomeState = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":7,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
        );
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
      );
      addTearDown(feed.dispose);

      await feed.refresh();
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));

      expect(feed.current.liveSequence, 7);
      expect(feed.current.hasPosition, isTrue);
      expect(feed.current.isRefreshing, isFalse);
      expect(feed.current.refreshMessage, isNull);
    },
  );

  test(
    'missing typed clock stays unknown without synthesizing a TIMER preset',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..readSettingsValues['TIMER'] = 4;
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
        automaticRetryDelay: const Duration(milliseconds: 5),
      );
      addTearDown(feed.dispose);

      ble.emitHomeState(
        '{"event":"home_state","v":1,"seq":2,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      );
      await Future<void>.delayed(const Duration(milliseconds: 25));

      expect(ble.snapshotRequests, 2);
      expect(ble.settingReads, isEmpty);
      expect(feed.current.clockStatus, isNull);
      expect(feed.current.isClockRefreshing, isFalse);
      expect(feed.current.refreshMessage, isNull);

      ble.emitHomeState(
        '{"event":"home_state","v":1,"seq":3,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",'
        '"occ":"FFEF00100000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
        '"cl":[1,175,180,"b",1,0,2,"n"]}',
      );
      expect(feed.current.clockStatus?.whiteRemainingSeconds, 175);
      expect(feed.current.clockStatus?.timerSetting, 2);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.gameStarted,
      );
      expect(feed.current.clockStatus?.running, isTrue);
    },
  );

  testWidgets(
    'a typed snapshot clears unavailable details after bounded readback fails',
    (tester) async {
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'fallback-board',
        refreshTimeout: const Duration(milliseconds: 5),
        automaticRetryDelay: const Duration(milliseconds: 5),
      );
      addTearDown(feed.dispose);
      await feed.refresh();
      for (int attempt = 0; attempt < 3; ++attempt) {
        await tester.pump(const Duration(milliseconds: 5));
        await tester.pump();
      }
      expect(feed.current.refreshMessage, isNotNull);
      expect(feed.current.hasPosition, isFalse);
      expect(ble.snapshotRequests, 3);
      expect(ble.commands, isEmpty);

      await feed.refresh();
      ble.emitHomeState(
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00004000FFFF","g":"d","duck":"g5",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
        '"cl":[0,0,0,"w",0,0,0,"y"]}',
      );
      expect(feed.current.position?.pieces, hasLength(32));
      expect(feed.current.physicalPositionMatches, isTrue);
      expect(feed.current.refreshMessage, isNull);
      expect(feed.current.isRefreshing, isFalse);
    },
  );
  test(
    'physical palette keeps player colors across first-player changes',
    () async {
      final ble = _FakeLitchBleService()
        ..readSettingsValues['P1_PIECE_WHEEL'] = 42
        ..readSettingsValues['P2_PIECE_WHEEL'] = 203;
      addTearDown(ble.dispose);
      final feed = HomeBoardFeed(bleService: ble, boardId: 'board');
      addTearDown(feed.dispose);
      for (final first in [1, 2, 1]) {
        ble.latestSettings['GAME_FIRST_PLAYER'] = first;
        ble.settingsController.add('GAME_FIRST_PLAYER');
        await Future<void>.delayed(Duration.zero);
        expect(feed.current.playerColorsConfirmed, isTrue);
        expect(
          feed.current.p1PieceWheel,
          42,
          reason: 'P1 color at first=$first',
        );
        expect(
          feed.current.p2PieceWheel,
          203,
          reason: 'P2 color at first=$first',
        );
      }
    },
  );

  test(
    'loads player color wheels and follows live board setting changes',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      ble.latestSettings
        ..['P1_PIECE_WHEEL'] = 42
        ..['P2_PIECE_WHEEL'] = '203';
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
      );
      addTearDown(feed.dispose);

      await Future<void>.delayed(Duration.zero);
      expect(feed.current.p1PieceWheel, 42);
      expect(feed.current.p2PieceWheel, 203);

      ble.latestSettings['P1_PIECE_WHEEL'] = 96;
      ble.settingsController.add('P1_PIECE_WHEEL');
      await Future<void>.delayed(Duration.zero);

      expect(feed.current.p1PieceWheel, 96);
      expect(feed.current.p2PieceWheel, 203);

      // Holding the board D-pad changes canonical end values while the older
      // aliases can remain present in the same authoritative Settings state.
      ble.latestSettings['P2_END_PIECE_WHEEL'] = 203;
      for (final wheel in [110, 120, 130]) {
        ble.latestSettings['P1_END_PIECE_WHEEL'] = wheel;
        ble.settingsController.add('P1_END_PIECE_WHEEL');
        await Future<void>.delayed(Duration.zero);
        expect(feed.current.p1PieceWheel, wheel);
        expect(feed.current.p2PieceWheel, 203);
      }
      ble.latestSettings.remove('P1_END_PIECE_WHEEL');
      ble.latestSettings.remove('P2_END_PIECE_WHEEL');
      ble.latestSettings.remove('P2_PIECE_WHEEL');
      ble.readSettingsValues['P2_PIECE_WHEEL'] = 111;
      ble.settingsController.add('P2_PIECE_WHEEL');
      await Future<void>.delayed(Duration.zero);

      expect(feed.current.p2PieceWheel, 111);
      expect(ble.paletteReads, greaterThanOrEqualTo(3));
    },
  );

  test(
    'emits a structured notice only once as console history grows',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
      );
      addTearDown(feed.dispose);
      final List<BoardNotice> notices = <BoardNotice>[];
      final StreamSubscription<BoardNotice> subscription = feed.notices.listen(
        notices.add,
      );
      addTearDown(subscription.cancel);

      const String noticeLine =
          '10:00:00.000 [FW] '
          '{"event":"notice","tone":"info","message":"Ready."}';
      ble.controller.add(const <String>[noticeLine]);
      ble.controller.add(const <String>[noticeLine, '10:00:00.001 [FW] OK']);
      await Future<void>.delayed(Duration.zero);

      expect(notices, hasLength(1));
      expect(notices.single.message, 'Ready.');
    },
  );

  test(
    'applies ordered live snapshots atomically and rejects stale ones',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
      );
      addTearDown(feed.dispose);

      const String lifted =
          '{"event":"home_state","v":1,"seq":2,"a":"lift",'
          '"fen":"8/8/8/8/8/8/8/4K2k w - - 0 1",'
          '"occ":"0000000000000080","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
      const String placed =
          '{"event":"home_state","v":1,"seq":3,"a":"idle",'
          '"fen":"8/8/8/8/8/8/4K3/7k b - - 1 1",'
          '"occ":"0000000000001080","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';

      ble.emitHomeState(lifted);
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.liveSequence, 2);
      expect(feed.current.homeStateAction, HomeStateAction.lift);

      ble.emitHomeState(placed);
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.liveSequence, 3);
      expect(feed.current.position?.sideToMove, HomeSide.black);

      ble.emitHomeState(lifted);
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.liveSequence, 3);
      expect(feed.current.position?.sideToMove, HomeSide.black);
    },
  );

  test('projects a queued CPU move atomically into Home state', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(bleService: ble, boardId: 'board');
    addTearDown(feed.dispose);

    ble.emitHomeState(
      '{"event":"home_state","v":1,"seq":12,"a":"cpu",'
      '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
      '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
      '"c":["g8","f6","--"]}',
    );
    await Future<void>.delayed(Duration.zero);

    expect(feed.current.liveSequence, 12);
    expect(feed.current.homeStateAction, HomeStateAction.cpu);
    expect(feed.current.physicalMovePrompt?.fromSquare, 6);
    expect(feed.current.physicalMovePrompt?.toSquare, 21);
    expect(feed.current.physicalPositionMatches, isTrue);
  });

  test('keeps both authoritative last moves across a live update', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(bleService: ble, boardId: 'board');
    addTearDown(feed.dispose);

    ble.emitHomeState(
      '{"event":"home_state","v":1,"seq":13,"a":"idle",'
      '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
      '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
      '"m":"e2e4e7e5b"}',
    );
    await Future<void>.delayed(Duration.zero);

    expect(feed.current.lastMoves?.white?.fromSquare, 52);
    expect(feed.current.lastMoves?.black?.toSquare, 28);
    expect(feed.current.lastMoves?.mostRecent, HomeSide.black);
  });

  test(
    'never exposes a valid provisional Duck as an unexpected piece',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
      );
      addTearDown(feed.dispose);
      await Future<void>.delayed(Duration.zero);
      final List<HomeBoardSnapshot> liveStates = <HomeBoardSnapshot>[];
      final StreamSubscription<HomeBoardSnapshot> subscription = feed.states
          .where((HomeBoardSnapshot state) => state.liveSequence != null)
          .listen(liveStates.add);
      addTearDown(subscription.cancel);

      const String provisional =
          '{"event":"home_state","v":1,"seq":2,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00004000ffff","g":"u","duck":"g5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
      const String latched =
          '{"event":"home_state","v":1,"seq":3,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00004000ffff","g":"d","duck":"g5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';

      const String preCandidate =
          '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00004000ffff","g":"u","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';

      ble.emitHomeState(preCandidate);
      await Future<void>.delayed(Duration.zero);
      ble.emitHomeState(provisional);
      await Future<void>.delayed(Duration.zero);
      ble.emitHomeState(latched);
      await Future<void>.delayed(Duration.zero);

      expect(
        liveStates.map((HomeBoardSnapshot state) => state.liveSequence),
        <int?>[1, 2, 3],
      );
      expect(
        liveStates.every(
          (HomeBoardSnapshot state) =>
              state.mismatchedSquares.isEmpty &&
              state.unexpectedOccupiedSquares.isEmpty,
        ),
        isTrue,
      );
      expect(liveStates.first.gameStatus?.variant, 'undecided');
      expect(liveStates.last.gameStatus?.variant, 'duck');
    },
  );

  test('treats a row-4 opening extra as a provisional Vex', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(bleService: ble, boardId: 'board');
    addTearDown(feed.dispose);

    ble.emitHomeState(
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
      '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
      '"occ":"ffff00080000ffff","g":"u","duck":"d4",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    );
    await Future<void>.delayed(Duration.zero);

    expect(feed.current.gameStatus?.variant, 'undecided');
    expect(feed.current.gameStatus?.duckSquare, 35);
    expect(feed.current.unexpectedOccupiedSquares, isEmpty);
  });

  test('renews live ordering and typed refresh after reconnect', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'connected-board',
    );
    addTearDown(feed.dispose);

    const String beforeReconnect =
        '{"event":"home_state","v":1,"seq":9,"a":"idle",'
        '"fen":"8/8/8/8/8/8/4K3/7k b - - 1 1",'
        '"occ":"0000000000001080","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
    const String afterReconnect =
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/4K2k w - - 0 1",'
        '"occ":"0000000000000090","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
    ble.emitHomeState(beforeReconnect);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, 9);
    final int requestsBeforeReconnect = ble.snapshotRequests;

    ble.connectionController.add(
      const ConnectionStateUpdate(
        deviceId: 'connected-board',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, isNull);
    ble.controller.add(const <String>[beforeReconnect]);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, isNull);
    ble.connectionController.add(
      const ConnectionStateUpdate(
        deviceId: 'connected-board',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await Future<void>.delayed(Duration.zero);

    expect(ble.commands, isEmpty);
    expect(ble.snapshotRequests, requestsBeforeReconnect + 1);

    ble.controller.add(const <String>[]);
    ble.emitHomeState(afterReconnect);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, 1);
    expect(feed.current.position?.sideToMove, HomeSide.white);
  });

  test(
    'duplicate Connected in one session preserves typed Home authority',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'connected-board',
      );
      addTearDown(feed.dispose);
      final HomeStateNotification live = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":9,"a":"idle",'
        '"fen":"8/8/8/8/8/8/4K3/7k b - - 1 1",'
        '"occ":"0000000000001080","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      )!;
      ble.homeStateController.add(live);
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.liveSequence, 9);

      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'connected-board',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      ble.homeStateController.add(live);
      await Future<void>.delayed(Duration.zero);

      expect(feed.current.liveSequence, 9);
      expect(feed.current.position?.sideToMove, HomeSide.black);
    },
  );

  test(
    'connection recovery is not dropped behind an in-flight startup read',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'connected-board',
      );
      addTearDown(feed.dispose);
      final Completer<bool> firstSend = Completer<bool>();
      ble.nextSnapshotRequest = firstSend;

      final Future<void> startupRead = feed.refresh();
      await Future<void>.delayed(Duration.zero);
      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 1);

      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'connected-board',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'connected-board',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      firstSend.complete(true);
      await startupRead;
      await Future<void>.delayed(Duration.zero);

      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 2);
    },
  );

  testWidgets(
    'exhausts two quick automatic reads before reporting unavailable details',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'connected-board',
      );
      addTearDown(feed.dispose);

      await feed.refresh();
      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 1);

      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 2);
      expect(feed.current.isRefreshing, isTrue);
      expect(feed.current.refreshMessage, isNull);

      await tester.pump(const Duration(milliseconds: 350));
      await tester.pump();
      expect(ble.commands, isEmpty);
      expect(ble.snapshotRequests, 3);
      expect(feed.current.isRefreshing, isTrue);
      expect(feed.current.refreshMessage, isNull);

      await tester.pump(const Duration(milliseconds: 350));
      expect(feed.current.isRefreshing, isFalse);
      expect(
        feed.current.refreshMessage,
        'Board details could not be read. Pull down to try again.',
      );
    },
  );

  test('accepts the current revision replayed after reconnect', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(
      bleService: ble,
      boardId: 'connected-board',
    );
    addTearDown(feed.dispose);

    const String current =
        '{"event":"home_state","v":1,"seq":9,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
        '"occ":"8100000000000000","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
    ble.emitHomeState(current);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, 9);

    ble.connectionController.add(
      const ConnectionStateUpdate(
        deviceId: 'connected-board',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    ble.connectionController.add(
      const ConnectionStateUpdate(
        deviceId: 'connected-board',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await Future<void>.delayed(Duration.zero);
    ble.controller.add(const <String>[]);
    ble.emitHomeState(current);
    await Future<void>.delayed(Duration.zero);

    expect(feed.current.liveSequence, 9);
    expect(feed.current.position?.pieces, hasLength(2));
  });

  test('atomically replaces a played position after New Game reset', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(bleService: ble, boardId: 'board');
    addTearDown(feed.dispose);

    const String played =
        '{"event":"home_state","v":1,"seq":10,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/K6k b - - 8 12",'
        '"occ":"8100000000000000","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
    const String reset =
        '{"event":"home_state","v":1,"seq":11,"a":"corr",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"8100000000000000","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';

    ble.emitHomeState(played);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.position?.pieces, hasLength(2));

    ble.emitHomeState(reset);
    await Future<void>.delayed(Duration.zero);
    expect(feed.current.liveSequence, 11);
    expect(feed.current.position?.fen, contains('RNBQKBNR w KQkq'));
    expect(feed.current.position?.pieces, hasLength(32));
    expect(feed.current.homeStateAction, HomeStateAction.correction);
    expect(feed.current.physicalPositionMatches, isFalse);
  });

  test(
    'New Game recovers a missed reset snapshot and unlocks the setup clock',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
        refreshTimeout: const Duration(milliseconds: 5),
        automaticRetryDelay: const Duration(milliseconds: 5),
      );
      addTearDown(feed.dispose);
      const String played =
          '{"event":"home_state","v":1,"seq":10,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",'
          '"occ":"FFEF00100000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"cl":[1,175,180,"b",1,0,2,"n"]}';
      const String reset =
          '{"event":"home_state","v":1,"seq":11,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFEF00100000FFFF","g":"u","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":1,"fs":1,'
          '"cl":[1,180,180,"w",0,0,2,"y"]}';
      ble.homeStateController.add(HomeStateNotification.tryParse(played)!);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.gameStarted,
      );

      // Lose the unsolicited New Game notification and the first readback.
      // The next bounded request delivers the real, newer setup snapshot.
      ble.onSnapshotRequest = (int request) {
        if (request == 2) {
          ble.homeStateController.add(HomeStateNotification.tryParse(reset)!);
        }
      };
      ble.controlController.add(
        const LitchControlEvent(
          requestId: 17,
          action: LitchControlAction.newGame,
          phase: LitchControlPhase.pending,
        ),
      );
      expect(ble.snapshotRequests, 0);
      ble.controlController.add(
        const LitchControlEvent(
          requestId: 17,
          action: LitchControlAction.newGame,
          phase: LitchControlPhase.applied,
        ),
      );
      expect(ble.snapshotRequests, 1);
      expect(feed.current.liveSequence, 10);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.gameStarted,
      );
      // A delayed packet from the old game cannot reopen/reset the clock.
      ble.homeStateController.add(HomeStateNotification.tryParse(played)!);
      await Future<void>.delayed(const Duration(milliseconds: 25));

      expect(ble.snapshotRequests, 2);
      expect(ble.commands, isEmpty);
      expect(feed.current.liveSequence, 11);
      expect(feed.current.physicalStatus?.placementMode, isTrue);
      expect(feed.current.physicalPositionMatches, isFalse);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.available,
      );
      expect(feed.current.clockStatus?.running, isFalse);
      expect(feed.current.clockStatus?.whiteRemainingSeconds, 180);
      expect(feed.current.clockStatus?.blackRemainingSeconds, 180);
      expect(feed.current.isRefreshing, isFalse);
      expect(feed.current.refreshMessage, isNull);
      ble.homeStateController.add(HomeStateNotification.tryParse(played)!);
      expect(feed.current.liveSequence, 11);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.available,
      );
    },
  );

  test('requests typed Home after an acknowledged app New Game', () async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final HomeBoardFeed feed = HomeBoardFeed(bleService: ble, boardId: 'board');
    addTearDown(feed.dispose);

    ble.controlController.add(
      const LitchControlEvent(
        requestId: 17,
        action: LitchControlAction.newGame,
        phase: LitchControlPhase.applied,
      ),
    );
    await Future<void>.delayed(Duration.zero);

    expect(ble.commands, isEmpty);
    expect(ble.snapshotRequests, 1);
    expect(feed.current.isRefreshing, isTrue);
  });

  test(
    'Vex move-first authority replaces and clears across connection lifecycle',
    () async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final HomeBoardFeed feed = HomeBoardFeed(
        bleService: ble,
        boardId: 'board',
      );
      addTearDown(feed.dispose);
      await Future<void>.delayed(Duration.zero);

      String home(int sequence, {required bool vex}) =>
          '{"event":"home_state","v":1,"seq":$sequence,"a":"${vex ? 'corr' : 'idle'}",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF10000000FFFF","g":"d","duck":"e3",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0'
          '${vex ? ',"vx":1' : ''}}';

      ble.homeStateController.add(
        HomeStateNotification.tryParse(home(1, vex: true))!,
      );
      expect(feed.current.vexMoveFirst, isTrue);
      ble.homeStateController.add(
        HomeStateNotification.tryParse(home(2, vex: false))!,
      );
      expect(feed.current.vexMoveFirst, isFalse);
      ble.homeStateController.add(
        HomeStateNotification.tryParse(home(3, vex: true))!,
      );
      expect(feed.current.vexMoveFirst, isTrue);

      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.vexMoveFirst, isFalse);
      ble.connectionController.add(
        const ConnectionStateUpdate(
          deviceId: 'board',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(feed.current.vexMoveFirst, isFalse);
      ble.homeStateController.add(
        HomeStateNotification.tryParse(home(1, vex: true))!,
      );
      expect(feed.current.vexMoveFirst, isTrue);
    },
  );
}
