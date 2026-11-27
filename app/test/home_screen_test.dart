import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' show Tristate;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/litch_workspace_geometry.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';

class _FakeFlutterReactiveBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}

class _HomeAwareConsoleHarness {
  final StreamController<List<String>> _consoleController =
      StreamController<List<String>>.broadcast();
  final StreamController<HomeStateNotification> _homeStateController =
      StreamController<HomeStateNotification>.broadcast(sync: true);
  HomeStateNotification? latestHomeState;

  Stream<List<String>> get stream => _consoleController.stream;

  Stream<HomeStateNotification> get homeStateStream =>
      _homeStateController.stream;

  void add(List<String> lines) {
    for (final String line in lines.reversed) {
      final HomeStateNotification? state = HomeStateNotification.tryParse(line);
      if (state != null) {
        latestHomeState = state;
        _homeStateController.add(state);
        return;
      }
    }
    _consoleController.add(lines);
  }

  Future<void> close() async {
    await _consoleController.close();
    await _homeStateController.close();
  }
}

class _FakeLitchBleService extends LitchBleService {
  _FakeLitchBleService()
    : super(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FakeFlutterReactiveBle(),
      );

  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async => const RuntimeHealthUnavailable(
    RuntimeHealthUnavailableReason.unsupported,
  );

  final _HomeAwareConsoleHarness controller = _HomeAwareConsoleHarness();
  final StreamController<LitchControlEvent> controlController =
      StreamController<LitchControlEvent>.broadcast(sync: true);
  final List<String> commands = <String>[];
  final List<(String, Object)> settingWrites = <(String, Object)>[];
  final Map<String, Object?> latestSettings = <String, Object?>{
    'GAME_FIRST_PLAYER': 1,
    'VS_CPU_LEVEL': 0,
    'VS_CPU_PLAYER': 2,
  };
  Completer<LitchSettingWriteResult>? settingWriteCompleter;
  LitchSettingWriteResult? acknowledgedSettingWriteResult;
  int snapshotRequests = 0;
  bool connected = true;
  bool ready = true;
  final connectionEvents = StreamController<ConnectionStateUpdate>.broadcast();
  final readinessEvents = StreamController<bool>.broadcast();

  @override
  Stream<ConnectionStateUpdate> get connectionStream => connectionEvents.stream;

  @override
  Stream<bool> get productReadinessStream => readinessEvents.stream;

  @override
  bool get productReady => connected && ready;

  @override
  Future<bool> ensureProductReady(
    String deviceId, {
    bool retry = false,
  }) async => productReady && connectedId == deviceId;

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3_3,
        capabilityMask:
            LitchDeviceCapabilities.ownerPairingBit |
            LitchDeviceCapabilities.consoleBit |
            LitchDeviceCapabilities.settingsBit,
        explicitlyReported: true,
      );

  @override
  Stream<LitchControlEvent> get controlActionEvents => controlController.stream;

  @override
  Future<LitchControlEvent> dispatchControlAction({
    required String deviceId,
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) async {
    final LitchControlEvent event = LitchControlEvent(
      requestId: 1,
      action: action,
      owner: owner,
      phase: LitchControlPhase.pending,
    );
    controlController.add(event);
    return event;
  }

  @override
  Stream<List<String>> get consoleStream => controller.stream;

  @override
  Stream<HomeStateNotification> get homeStateStream =>
      controller.homeStateStream;

  @override
  HomeStateNotification? get latestHomeState => controller.latestHomeState;

  @override
  void discardCachedHomeAssistance({
    required String boardId,
    required int sequence,
  }) {
    final HomeStateNotification? state = controller.latestHomeState;
    if (connectedId == boardId && state?.sequence == sequence) {
      controller.latestHomeState = state!.withoutAssistance();
    }
  }

  @override
  String? get connectedId => connected ? 'board-12345' : null;

  @override
  bool get isConnected => connected;

  @override
  Object? latestSettingValue(String key) => latestSettings[key];

  @override
  LitchControlAvailability controlActionAvailability(String deviceId) =>
      LitchControlAvailability.available;

  @override
  Future<bool> sendConsoleLine(String deviceId, String line) async {
    commands.add(line);
    return true;
  }

  @override
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async {
    snapshotRequests += 1;
    return true;
  }

  @override
  Future<LitchSettingWriteResult> writeSettingAcknowledged({
    required String deviceId,
    required String key,
    required Object value,
  }) async {
    settingWrites.add((key, value));
    final LitchSettingWriteResult? configured = acknowledgedSettingWriteResult;
    if (configured != null) {
      return configured;
    }
    final Completer<LitchSettingWriteResult>? completer = settingWriteCompleter;
    if (completer != null) {
      return completer.future;
    }
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

  @override
  Future<void> dispose() async {
    await controller.close();
    await controlController.close();
    await connectionEvents.close();
    await readinessEvents.close();
    await super.dispose();
  }
}

double _contrastRatio(Color first, Color second) {
  final double firstLuminance = first.computeLuminance();
  final double secondLuminance = second.computeLuminance();
  final double brighter = firstLuminance > secondLuminance
      ? firstLuminance
      : secondLuminance;
  final double darker = firstLuminance > secondLuminance
      ? secondLuminance
      : firstLuminance;
  return (brighter + 0.05) / (darker + 0.05);
}

Widget _harness(
  _FakeLitchBleService ble, {
  TextScaler textScaler = TextScaler.noScaling,
  ValueChanged<bool>? onHealthAttentionChanged,
  bool dark = false,
  TargetPlatform? platform,
  GlobalKey<ScaffoldMessengerState>? scaffoldMessengerKey,
  bool reduceMotion = true,
}) {
  return MaterialApp(
    debugShowCheckedModeBanner: false,
    scaffoldMessengerKey: scaffoldMessengerKey,
    theme: (dark ? buildLitchDarkTheme() : buildLitchTheme()).copyWith(
      platform: platform,
    ),
    builder: (BuildContext context, Widget? child) {
      return MediaQuery(
        // These status/layout fixtures settle on the supported steady cues.
        // Real amber motion is exercised with finite pumps in its board journey.
        data: MediaQuery.of(
          context,
        ).copyWith(textScaler: textScaler, disableAnimations: reduceMotion),
        child: child!,
      );
    },
    home: Scaffold(
      body: RepaintBoundary(
        key: const ValueKey<String>('home-capture'),
        child: HomeScreen(
          bleService: ble,
          boardId: 'board-12345',
          onHealthAttentionChanged: onHealthAttentionChanged,
        ),
      ),
    ),
  );
}

Finder _homeDetailsScrollable() => find.byWidgetPredicate(
  (widget) =>
      widget is Scrollable && widget.physics is AlwaysScrollableScrollPhysics,
  description: 'the Play details scroll owner',
);

String _cpuState(int sequence, String action, String extra) =>
    '{"event":"home_state","v":1,"seq":$sequence,"a":"$action",'
    '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
    '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
    '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
    '"cl":[1,299,300,"w",1,0,3,"n"]$extra}';

void main() {
  testWidgets('setup keeps Hint unavailable when assistance is omitted', (
    tester,
  ) async {
    final ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    final hint = find.byKey(const ValueKey<String>('control:hint'));
    var sequence = 0;
    for (final setup in [true, false, true]) {
      ble.controller.add([
        '{"event":"home_state","v":1,"seq":${++sequence},'
            '"a":"${setup ? 'corr' : 'idle'}",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"${setup ? 'FFFF00000000FFEF' : 'FFFF00000000FFFF'}",'
            '"g":"u","duck":"--",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":${setup ? 1 : 0},"fs":1,"co":1}',
      ]);
      await tester.pump();
      await tester.pump();
      await tester.scrollUntilVisible(
        hint,
        200,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      expect(
        tester.widget<OutlinedButton>(hint).onPressed,
        setup ? isNull : isNotNull,
        reason: 'omitted assistance must not override authoritative setup',
      );
    }
  });

  testWidgets(
    'review Hint calculation on whole Play and controls at phone sizes',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      addTearDown(() => tester.binding.setSurfaceSize(null));
      for (final scale in <double>[1, 2]) {
        await tester.binding.setSurfaceSize(const Size(390, 844));
        final ble = _FakeLitchBleService();
        addTearDown(ble.dispose);
        await tester.pumpWidget(
          _harness(ble, textScaler: TextScaler.linear(scale)),
        );
        await tester.pump();
        ble.controller.add(<String>[_cpuState(1, 'idle', ',"co":1,"as":"7p"')]);
        await tester.pumpAndSettle();
        await decodeVisibleReviewImages(tester);
        await captureHealthReview(
          tester,
          'hint-calculating-play-${scale.toInt()}x',
        );
        final hint = find.byKey(const ValueKey<String>('control:hint'));
        await tester.scrollUntilVisible(
          hint,
          200,
          scrollable: _homeDetailsScrollable(),
        );
        await tester.pumpAndSettle();
        await captureHealthReview(
          tester,
          'hint-calculating-controls-${scale.toInt()}x',
        );
        expect(
          find.descendant(
            of: hint,
            matching: find.byType(CircularProgressIndicator),
          ),
          findsOneWidget,
        );
        expect(tester.getSize(hint).height, greaterThanOrEqualTo(48));
        expectReviewTextFits(
          tester,
          find.text('Show Move'),
          'Waiting action remains legible on Play',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
      }
    },
  );

  testWidgets('capture Home replays the production physical journey', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(800, 1600));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final fixture =
        jsonDecode(
              File(
                '../test/fixtures/hint_capture_home_journey.json',
              ).readAsStringSync(),
            )
            as Map<String, dynamic>;
    final rows = (fixture['checkpoints'] as List).cast<Map<String, dynamic>>();
    final semantics = tester.ensureSemantics();
    try {
      for (final run in rows.map((row) => row['run'] as String).toSet()) {
        final opposite = run.endsWith('black-swapped');
        final victim = opposite ? 35 : 36;
        final ble = _FakeLitchBleService()
          ..latestSettings['GAME_FIRST_PLAYER'] = opposite ? 2 : 1;
        addTearDown(ble.dispose);
        await tester.pumpWidget(_harness(ble));
        await tester.pump();
        final marker = find.byKey(
          ValueKey('capture-removal-${algebraicSquare(victim)}'),
        );
        var sequence = 0;
        for (final row in rows.where((row) => row['run'] == run)) {
          final stage = row['stage'] as String;
          // This shared fixture is checked byte-for-byte by the native journey.
          // HomeStateLine observes with seq=1; only sequence is normalized here
          // so the real notification/feed revision filter can advance.
          final wire = (row['home'] as String).replaceFirst(
            '"seq":1,',
            '"seq":${++sequence},',
          );
          final expected = HomeStateNotification.tryParse(wire);
          expect(expected, isNotNull, reason: '$run/$stage must parse');
          ble.controller.add([wire]);
          await tester.pumpAndSettle();
          final snapshot = tester
              .widget<HomeChessboard>(find.byType(HomeChessboard))
              .snapshot;
          expect(
            snapshot.liveSequence,
            sequence,
            reason: '$run/$stage must reach Home',
          );
          expect(snapshot.homeStateAction, expected!.action);
          final removal = <String>{
            'CPU-reference',
            'Hint-visible',
            'Hint-replayed',
            'other-pickup-returned',
            'illegal-piece-relifted',
            'illegal-placement-restored',
            'mover-held',
            'en-passant-pawn-pending',
          }.contains(stage);
          expect(
            marker,
            removal ? findsOneWidget : findsNothing,
            reason: '$run/$stage',
          );
          expect(
            tester
                .getSemantics(
                  find.byKey(ValueKey('square-${algebraicSquare(victim)}')),
                )
                .label,
            removal
                ? contains('Remove this piece')
                : isNot(contains('Remove this piece')),
            reason: '$run/$stage',
          );
          if (stage == 'mover-held' ||
              stage == 'illegal-piece-relifted' ||
              stage == 'other-piece-held') {
            final legal = snapshot.visibleAssistance!;
            expect(legal.legalDestinations, isNotNull);
            expect(legal.isReady, isFalse);
            expect(
              legal.move,
              stage == 'other-piece-held' ? isNull : isNotNull,
            );
            for (var square = 0; square < 64; ++square) {
              if ((legal.legalDestinations! & (BigInt.one << square)) ==
                  BigInt.zero) {
                continue;
              }
              expect(
                tester
                    .getSemantics(
                      find.byKey(ValueKey('square-${algebraicSquare(square)}')),
                    )
                    .label,
                contains('Legal destination'),
                reason: '$run/$stage ${algebraicSquare(square)}',
              );
              expect(
                find.byKey(
                  ValueKey(
                    '${removal && square == victim ? 'capture-removal' : 'assistance-to'}-${algebraicSquare(square)}',
                  ),
                ),
                findsOneWidget,
              );
            }
          }
          if (stage == 'en-passant-pawn-pending') {
            expect(snapshot.homeStateAction, HomeStateAction.settle);
            expect(
              find.text('Remove the piece on ${algebraicSquare(victim)}.'),
              findsOneWidget,
            );
            expect(
              find.byKey(ValueKey('cpu-move-from-${algebraicSquare(36)}')),
              findsNothing,
            );
            expect(
              find.byKey(ValueKey('cpu-move-to-${algebraicSquare(43)}')),
              findsNothing,
            );
            for (final square in [36, 43]) {
              expect(
                tester
                    .getSemantics(
                      find.byKey(ValueKey('square-${algebraicSquare(square)}')),
                    )
                    .label,
                isNot(contains('Computer move')),
              );
            }
            expect(find.textContaining('computer piece'), findsNothing);
            expect(find.textContaining('then move'), findsNothing);
          }
          if (stage == 'victim-removed') {
            expect(snapshot.visibleAssistance?.move, isNull);
            expect(
              find.byKey(
                ValueKey(
                  'assistance-to-${algebraicSquare(opposite ? 43 : 36)}',
                ),
              ),
              findsNothing,
            );
          }
          expect(tester.takeException(), isNull, reason: '$run/$stage');
        }
        await tester.pumpWidget(const SizedBox.shrink());
      }
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('capture Hints share CPU removal cues across board orientations', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(800, 1600));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final semantics = tester.ensureSemantics();
    try {
      for (final (fen, from, to, victim) in <(String, String, String, String)>[
        ('7k/8/3p4/4P3/8/8/8/K7 w - - 0 1', 'e5', 'd6', 'd6'),
        ('7k/8/8/3pP3/8/8/8/K7 w - d6 0 1', 'e5', 'd6', 'd5'),
        ('7k/8/8/8/3p4/4P3/8/K7 b - - 0 1', 'd4', 'e3', 'e3'),
        ('7k/8/8/8/3pP3/8/8/K7 b - e3 0 1', 'd4', 'e3', 'e4'),
      ]) {
        for (final opposite in [false, true]) {
          final ble = _FakeLitchBleService()
            ..latestSettings['GAME_FIRST_PLAYER'] = opposite ? 2 : 1;
          addTearDown(ble.dispose);
          await tester.pumpWidget(_harness(ble));
          await tester.pump();
          final position = HomeChessPosition.tryParse(fen)!;
          final fromSquare = tryParseAlgebraicSquare(from)!;
          final victimSquare = tryParseAlgebraicSquare(victim)!;
          final full = position.occupiedSquares;
          int sequence = 1;
          Future<void> publish(
            String action,
            BigInt occupied,
            String extra,
            bool rotated,
          ) async {
            ble.controller.add([
              '{"event":"home_state","v":1,"seq":${sequence++},"a":"$action",'
                  '"fen":"$fen","occ":"${occupied.toRadixString(16).padLeft(16, '0')}",'
                  '"g":"c","duck":"--","out":"n","win":"n",'
                  '"rot":${rotated ? 1 : 0},"white":1,"pm":0$extra}',
            ]);
            await tester.pumpAndSettle();
          }

          final marker = find.byKey(ValueKey('capture-removal-$victim'));
          final square = find.byKey(ValueKey('square-$victim'));
          final guidance =
              'Remove the piece on $victim, then move the piece from $from to $to.';
          final cleared =
              full &
              ~(BigInt.one << fromSquare) &
              ~(BigInt.one << victimSquare);
          for (final rotated in [false, true]) {
            await publish('idle', full, ',"as":"3h$from$to"', rotated);
            expect(
              tester.getSemantics(square).label,
              contains('Remove this piece'),
            );
            expect(marker, findsOneWidget);
            expect(
              tester.getRect(square).contains(tester.getCenter(marker)),
              isTrue,
            );
            expect(find.text(guidance), findsOneWidget);
            final hintMarker = tester.widget<Icon>(
              find.descendant(of: marker, matching: find.byType(Icon)),
            );
            await publish('idle', full, ',"as":"2o"', rotated);
            expect(marker, findsNothing);
            await publish('corr', full, ',"as":"3h$from$to"', rotated);
            expect(marker, findsNothing);
            await publish(
              'cpu',
              full,
              ',"c":["$from","$to","$victim"]',
              rotated,
            );
            expect(marker, findsOneWidget);
            expect(find.text(guidance), findsOneWidget);
            final cpuMarker = tester.widget<Icon>(
              find.descendant(of: marker, matching: find.byType(Icon)),
            );
            expect(cpuMarker.icon, hintMarker.icon);
            expect(cpuMarker.color, hintMarker.color);
            await publish(
              'cpu',
              cleared,
              ',"c":["$from","$to","$victim"]',
              rotated,
            );
            expect(marker, findsNothing);
            expect(find.text(guidance), findsNothing);
            expect(
              find.text('Move the computer piece from $from to $to.'),
              findsOneWidget,
            );
            expect(tester.takeException(), isNull);
          }
        }
      }
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'idle Play centers visible guidance while Hint space is reserved',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final offsets = <double>[];
      for (final scale in [1.0, 2.0]) {
        final ble = _FakeLitchBleService();
        await tester.pumpWidget(
          _harness(ble, textScaler: TextScaler.linear(scale)),
        );
        ble.controller.add([_cpuState(1, 'idle', ',"as":"2o"')]);
        await tester.pump();
        await tester.pump();
        final title = tester.getRect(find.text('P1 to move'));
        final context = tester.getRect(
          find.byKey(const ValueKey('home-quiet-game-context')),
        );
        final rail = tester.getRect(
          find.byKey(const ValueKey('home-guidance-rail')),
        );
        offsets.add(title.expandToInclude(context).center.dy - rail.center.dy);
        expect(title.left, rail.right + 12);
        expect(find.byKey(const ValueKey('home-hint-status')), findsNothing);
        expect(tester.takeException(), isNull);
        await decodeVisibleReviewImages(tester);
        await captureHealthReview(tester, 'idle-play-centering-${scale}x');
        await tester.pumpWidget(const SizedBox());
        await ble.dispose();
      }
      expect(
        offsets,
        everyElement(closeTo(0, 0.01)),
        reason:
            'Invisible Hint reservations must not pull the visible title '
            'and context above the status rail at either text scale.',
      );
    },
  );

  testWidgets('portrait Play starts with four pixels above the setup surface', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(393, 852);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    final ble = _FakeLitchBleService()..ready = false;
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble, platform: TargetPlatform.iOS));
    await tester.pump();
    final viewport = tester.getRect(
      find.byKey(const ValueKey('home-details-scroll')),
    );
    final surface = tester.getRect(
      find.byKey(const ValueKey('home-game-surface')),
    );
    expect(surface.top - viewport.top, 4);
    final board = tester.getRect(
      find.byKey(const ValueKey('home-chessboard-surface')),
    );
    expect(
      board.left - viewport.left,
      LitchWorkspaceGeometry.canvasHorizontalInset,
    );
    expect(
      viewport.right - board.right,
      LitchWorkspaceGeometry.canvasHorizontalInset,
    );
    expect(
      tester.state<ScrollableState>(_homeDetailsScrollable()).position.pixels,
      0,
    );
    expect(find.text('Reading the board…'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    expect(
      tester.takeException(),
      isNull,
      reason: 'Home can close before the board is ready with reduced motion.',
    );
  });

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'landscape Play keeps the board fixed beside readable details at ${scale}x',
      (tester) async {
        tester.view.physicalSize = const Size(640, 360);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final ble = _FakeLitchBleService();
        addTearDown(ble.dispose);
        await tester.pumpWidget(
          _harness(ble, textScaler: TextScaler.linear(scale)),
        );
        await tester.pump();
        expect(tester.takeException(), isNull);
        final board = tester.getRect(
          find.byKey(const ValueKey('home-chessboard-surface')),
        );
        expect(
          board.height,
          LitchWorkspaceGeometry(
            const Size(
              640,
              360 + LitchWorkspaceGeometry.canvasHorizontalInset * 2,
            ),
          ).landscapeCanvasSide,
        );
        expect(board.center.dy, 180);
        await captureHealthReview(tester, 'play-top-alignment-${scale}x');
        expect(
          tester.getRect(find.byKey(const ValueKey('home-game-surface'))).top,
          closeTo(board.top, 0.01),
          reason: 'Landscape game status begins beside the board top edge',
        );
        final details = _homeDetailsScrollable();
        await tester.drag(details, const Offset(0, -250));
        await tester.pump();
        expect(
          tester.getRect(find.byKey(const ValueKey('home-chessboard-surface'))),
          board,
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
      },
    );
  }

  testWidgets(
    'CPU thinking announces once while ETA changes and yields to physical guidance',
    (tester) async {
      final ble = _FakeLitchBleService()
        ..latestSettings.addAll({'VS_CPU_LEVEL': 6, 'VS_CPU_PLAYER': 1});
      addTearDown(ble.dispose);
      final semantics = tester.ensureSemantics();
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      try {
        await tester.pumpWidget(_harness(ble));
        ble.controller.add([_cpuState(1, 'progress', ',"th":["w",0,null]')]);
        await tester.pumpAndSettle();
        final announcement = find.byKey(
          const ValueKey<String>('cpu-thinking-announcement'),
        );
        final header = find.byKey(
          const ValueKey<String>('home-guidance-live-region'),
        );
        final firstNode = tester.getSemantics(announcement).id;
        expect(
          tester.getSemantics(announcement),
          isSemantics(label: 'CPU is thinking', isLiveRegion: true),
        );
        expect(
          find.byKey(const ValueKey<String>('cpu-thinking-robot')),
          findsOneWidget,
        );
        expect(find.text('Move in progress'), findsNothing);

        for (final (sequence, remaining) in [(2, 8), (3, 7)]) {
          ble.controller.add([
            _cpuState(sequence, 'progress', ',"th":["w",$sequence,$remaining]'),
          ]);
          await tester.pumpAndSettle();
          final eta = 'About $remaining seconds left';
          expect(find.text(eta), findsOneWidget);
          expect(tester.getSemantics(announcement).id, firstNode);
          expect(
            tester.getSemantics(announcement),
            isSemantics(label: 'CPU is thinking', isLiveRegion: true),
          );
          expect(
            tester.getSemantics(find.text(eta)),
            isSemantics(label: eta, isLiveRegion: false),
          );
          expect(tester.getSemantics(header).getSemanticsData().label, eta);
        }
        await tester.pump(const Duration(seconds: 3));
        expect(find.text('About 7 seconds left'), findsNothing);
        expect(find.text('CPU is thinking'), findsOneWidget);
        expect(tester.getSemantics(announcement).id, firstNode);
        ble.controller.add([
          _cpuState(
            4,
            'corr',
            ',"th":["w",12,null]',
          ).replaceFirst('FFFF00000000FFFF', 'FFEF00000000FFFF'),
        ]);
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey<String>('cpu-thinking-robot')),
          findsNothing,
        );
        expect(find.text('Follow the board’s guidance'), findsOneWidget);
        ble.controller.add([_cpuState(5, 'progress', ',"th":["w",14,3]')]);
        await tester.pumpAndSettle();
        expect(find.text('CPU is thinking'), findsOneWidget);
        ble.controller.add([
          _cpuState(
            6,
            'cpu',
            ',"c":["e2","e4","--"]',
          ).replaceFirst('"w",1,0,3,"n"', '"w",1,1,3,"n"'),
        ]);
        await tester.pumpAndSettle();
        expect(find.text('CPU is thinking'), findsNothing);
        expect(
          find.byKey(const ValueKey<String>('cpu-thinking-robot')),
          findsNothing,
        );
        expect(
          find.text('Move the computer piece from e2 to e4.'),
          findsOneWidget,
        );
        expect(
          tester.getSemantics(header),
          isSemantics(
            label:
                'COMPUTER MOVE. Computer move ready. Move the computer piece from e2 to e4.',
            isLiveRegion: true,
          ),
        );
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        semantics.dispose();
      }
    },
  );

  testWidgets('review Home combined restart and session-only recovery', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(360, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    ble.controller.add(<String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"FFFF00000000FFFF","g":"u","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1,"up":0,"cl":[0,0,0,"w",0,0,0,"y"]}',
    ]);
    await tester.pumpAndSettle();
    await tester.runAsync(() async {
      final BuildContext context = tester.element(find.byType(HomeChessboard));
      for (final Image image in tester.widgetList<Image>(find.byType(Image))) {
        await precacheImage(image.image, context);
      }
    });
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'home-healthy');
    ble.debugAppendConsolePacket(
      '{"event":"board_health","v":1,"h":"000000000","ps":"i","rr":6,"boot":2,"wd":1,"sd":1}\n'
          .codeUnits,
    );
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'home-restarted');
    expect(find.textContaining('after it stopped responding'), findsOneWidget);
    await tester.pump(const Duration(seconds: 13));
    await tester.pump();
    expect(
      find.textContaining('Saving is unavailable for this session'),
      findsNothing,
    );
    expect(find.text('P1 to move'), findsOneWidget);
    await captureHealthReview(tester, 'home-session-only');
    ble.debugAppendConsolePacket(
      '{"event":"board_health","v":1,"h":"000100000","ps":"n","rr":6,"boot":2,"wd":1,"sd":0}\n'
          .codeUnits,
    );
    await tester.pumpAndSettle();
    expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
    expect(find.text('P1 to move'), findsOneWidget);
    expect(find.byType(Image), findsNWidgets(32));
    await captureHealthReview(tester, 'home-saving-recovered');
  });

  testWidgets('review Home authoritative Coaching at phone and large text sizes', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(393, 852));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final semantics = tester.ensureSemantics();
    try {
      for (final TargetPlatform platform in <TargetPlatform>[
        TargetPlatform.android,
        TargetPlatform.iOS,
      ]) {
        for (final double scale in <double>[1, 2]) {
          await tester.pumpWidget(
            _harness(
              ble,
              textScaler: TextScaler.linear(scale),
              platform: platform,
            ),
          );
          ble.controller.add(<String>[
            '{"event":"home_state","v":1,"seq":${scale.toInt()},"a":"idle",'
                '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
                '"occ":"FFFF00000000FFFF","g":"c","duck":"--","out":"n","win":"n",'
                '"rot":0,"white":1,"pm":0,"up":0,"co":1,"cl":[0,0,0,"w",0,0,0,"y"]}',
          ]);
          await tester.pumpAndSettle();
          tester
              .state<ScrollableState>(_homeDetailsScrollable())
              .position
              .jumpTo(0);
          await tester.pumpAndSettle();
          await captureHealthReview(
            tester,
            'home-coaching-top-${platform.name}-${scale.toInt()}',
          );
          final Finder p1 = find.byKey(
            const ValueKey<String>('control:coaching:p1'),
          );
          final Finder p2 = find.byKey(
            const ValueKey<String>('control:coaching:p2'),
          );
          await tester.ensureVisible(p1);
          await tester.pumpAndSettle();
          expect(
            tester
                    .getSemantics(p1)
                    .getSemanticsData()
                    .flagsCollection
                    .isToggled ==
                Tristate.isTrue,
            isTrue,
          );
          expect(
            tester
                    .getSemantics(p2)
                    .getSemanticsData()
                    .flagsCollection
                    .isToggled ==
                Tristate.isTrue,
            isFalse,
          );
          expect(tester.getSize(p1).height, greaterThanOrEqualTo(48));
          expect(tester.getSize(p2).height, greaterThanOrEqualTo(48));
          expect(tester.getSemantics(p1).label, contains('Player 1 coaching'));
          expect(tester.getSemantics(p2).label, contains('Player 2 coaching'));
          expect(
            tester
                .getSemantics(p1)
                .getSemanticsData()
                .flagsCollection
                .isToggled,
            Tristate.isTrue,
          );
          expect(
            tester
                .getSemantics(p2)
                .getSemanticsData()
                .flagsCollection
                .isToggled,
            Tristate.isFalse,
          );
          expect(tester.takeException(), isNull);
          await captureHealthReview(
            tester,
            'home-coaching-controls-${platform.name}-${scale.toInt()}',
          );
        }
      }
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('shows honest unknown state before board replies', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    expect(
      find.text('Position not available'),
      findsNothing,
      reason: 'The first painted frame precedes the deferred board refresh',
    );
    expect(find.text('Reading the board…'), findsOneWidget);
    await tester.pump();

    expect(find.text('Board connected'), findsNothing);
    expect(find.byIcon(Icons.refresh), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('home-board-health')),
      findsNothing,
    );
    expect(find.text('Reading the board…'), findsOneWidget);
    expect(find.text('P1 to move'), findsNothing);
    expect(find.text('The board comes first'), findsNothing);
    expect(find.byIcon(Icons.visibility_outlined), findsNothing);
    expect(ble.snapshotRequests, 1);
    expect(ble.commands, isEmpty);
  });

  testWidgets(
    'exhausted board reads remain unavailable without a health fault',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final List<bool> attention = <bool>[];

      await tester.pumpWidget(
        _harness(ble, onHealthAttentionChanged: attention.add),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey<String>('home-board-health')),
        findsNothing,
      );

      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));

      expect(find.text('Position not available'), findsOneWidget);
      expect(
        find.byKey(const ValueKey<String>('home-board-health')),
        findsNothing,
      );
      expect(
        find.text('Board details could not be read. Pull down to try again.'),
        findsOneWidget,
      );
      expect(attention.last, isFalse);
      expect(ble.snapshotRequests, 3);
      expect(ble.commands, isEmpty);
    },
  );

  testWidgets(
    'reconnect retires old read failure while owner proof is pending',
    (WidgetTester tester) async {
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      expect(find.text('Position not available'), findsOneWidget);

      ble.connected = false;
      ble.ready = false;
      ble.connectionEvents.add(
        const ConnectionStateUpdate(
          deviceId: 'board-12345',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
      await tester.pump();
      ble.connected = true;
      ble.connectionEvents.add(
        const ConnectionStateUpdate(
          deviceId: 'board-12345',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      await tester.pump();
      expect(
        find.text('Position not available'),
        findsNothing,
        reason:
            'A prior link failure cannot describe current owner preparation',
      );
      expect(find.text('Reading the board…'), findsOneWidget);
      expect(find.text('P1 to move'), findsNothing);

      ble.ready = true;
      ble.readinessEvents.add(true);
      await tester.pump();
      expect(find.text('Reading the board…'), findsOneWidget);
      ble.controller.add(const [
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      ]);
      await tester.pump();
      expect(find.text('Position not available'), findsNothing);
      expect(find.text('Reading the board…'), findsNothing);
      expect(find.text('P1 to move'), findsOneWidget);
    },
  );

  testWidgets('renders current position and physical mismatch guidance', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00000000fffe","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('Follow the board’s guidance'), findsOneWidget);
    expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
    expect(find.text('Classic · Move 1'), findsNothing);
    expect(find.text('P2 rook expected on a8.'), findsOneWidget);
    expect(find.byKey(const ValueKey('expected-missing-a8')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e8')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e1')), findsOneWidget);

    await tester.scrollUntilVisible(
      find.text('Moves'),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('Moves'), findsOneWidget);
    expect(
      find.text('Move history is not available for this game.'),
      findsOneWidget,
    );
  });

  testWidgets('presents a busy prior-game board as staged New Game setup', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"8386040308882780","g":"u","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":1,"fs":1,'
          '"cl":[0,0,0,"w",0,0,0,"b"]}',
    ]);
    await tester.pump();
    await tester.pump();

    expect(find.text('NEW GAME SETUP'), findsOneWidget);
    expect(find.text('Remove leftover pieces'), findsOneWidget);
    expect(
      find.text('Remove the 6 highlighted pieces from the board.'),
      findsOneWidget,
    );
    expect(find.text('POSITION NEEDS CORRECTION'), findsNothing);
    expect(find.textContaining('guiding 27 squares'), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('unexpected-occupied-c3')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('expected-missing-a8')),
      findsNothing,
    );
  });

  testWidgets('saving failures leave repeated Home play guidance quiet', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,"ps":"q"}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
    expect(find.text('P1 to move'), findsOneWidget);
    expect(
      find.text(
        'A saved game could not be restored. The board started a new game and is repairing its saved copy.',
      ),
      findsNothing,
    );
    final SemanticsHandle semantics = tester.ensureSemantics();
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey<String>('home-guidance-live-region')),
          )
          .label,
      contains('Player 1 to move'),
    );
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey<String>('home-guidance-live-region')),
          )
          .label,
      isNot(contains('saved')),
    );
    semantics.dispose();

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":9,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFE","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,"ps":"i"}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
    expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":10,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,"ps":"i"}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
    expect(
      find.text(
        'The board could not confirm a saved copy. Recent changes may be lost when the board turns off.',
      ),
      findsNothing,
    );

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":11,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
  });

  for (final failure in ['i', 'q']) {
    testWidgets(
      'saving $failure retains CPU thinking and physical move guidance',
      (tester) async {
        final ble = _FakeLitchBleService()
          ..latestSettings.addAll({'VS_CPU_LEVEL': 6, 'VS_CPU_PLAYER': 1});
        addTearDown(ble.dispose);
        await tester.pumpWidget(_harness(ble));
        for (var sequence = 1; sequence <= 3; sequence++) {
          ble.controller.add([
            _cpuState(
              sequence,
              'progress',
              ',"ps":"$failure","th":["w",$sequence,8]',
            ),
          ]);
          await tester.pumpAndSettle();
          expect(find.text('CPU is thinking'), findsOneWidget);
          expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
          expect(find.textContaining('saved copy'), findsNothing);
        }
        ble.controller.add([
          _cpuState(4, 'cpu', ',"ps":"$failure","c":["e2","e4","--"]'),
        ]);
        await tester.pumpAndSettle();
        expect(find.text('CPU is thinking'), findsNothing);
        expect(
          find.text('Move the computer piece from e2 to e4.'),
          findsOneWidget,
        );
        expect(find.text('SAVING NEEDS ATTENTION'), findsNothing);
      },
    );
  }

  testWidgets('keeps staged New Game setup usable at 200 percent text', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(
      _harness(ble, textScaler: const TextScaler.linear(2)),
    );
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"8386040308882780","g":"u","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":1,"fs":1,'
          '"cl":[0,0,0,"w",0,0,0,"b"]}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('NEW GAME SETUP'), findsOneWidget);
    expect(find.text('Remove leftover pieces'), findsOneWidget);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('home-board-actions')),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'returns to ordinary correction after Fresh Setup reaches Play Ready',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);

      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":9,"a":"corr",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"ffff00000000fffe","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1, '
            '"cl":[0,0,0,"w",0,0,0,"n"]}',
      ]);
      await tester.pumpAndSettle();

      expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
      expect(find.text('Follow the board’s guidance'), findsOneWidget);
      expect(find.text('NEW GAME SETUP'), findsNothing);
      expect(find.text('Set up the starting position'), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('expected-missing-a8')),
        findsOneWidget,
      );
    },
  );

  testWidgets('shows clock recovery as a read in progress', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00000000ffff","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pump();
    await tester.pump();

    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    expect(find.text('Syncing clock with the board…'), findsOneWidget);
    expect(find.text('Unavailable'), findsNothing);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('home-clock-strip')))
          .label,
      'Game clock is syncing',
    );
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('home-game-clock-static')),
      ),
      isSemantics(
        label: 'Time limit',
        value: 'Syncing…',
        hint: 'Syncing clock with the board…',
        isLiveRegion: true,
      ),
    );
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey<String>('home-game-clock-static')),
          )
          .flagsCollection
          .isEnabled,
      Tristate.none,
    );

    await tester.pump(const Duration(milliseconds: 350));
    await tester.pump();
  });

  testWidgets('uses the available phone width for the Home chessboard', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    for (final config in [(scale: 1.0, first: 1), (scale: 2.0, first: 2)]) {
      final ble = _FakeLitchBleService()
        ..latestSettings['GAME_FIRST_PLAYER'] = config.first
        ..latestSettings['P1_PIECE_WHEEL'] = 121
        ..latestSettings['P2_PIECE_WHEEL'] = 24;
      ble.controller.add([_cpuState(1, 'idle', '')]);
      addTearDown(ble.dispose);
      await tester.pumpWidget(
        _harness(ble, textScaler: TextScaler.linear(config.scale)),
      );
      await tester.pump();

      final Size boardSize = tester.getSize(
        find.byKey(const ValueKey<String>('home-chessboard-surface')),
      );
      final Finder board = find.byKey(
        const ValueKey<String>('home-chessboard-surface'),
      );
      final Finder gameSurface = find.byKey(
        const ValueKey<String>('home-game-surface'),
      );
      final Finder boardActions = find.byKey(
        const ValueKey<String>('home-board-actions'),
      );
      final Finder topPlayer = find.byKey(
        ValueKey<String>(
          'home-player-name-${config.first == 1 ? 'black' : 'white'}',
        ),
      );
      final Finder bottomPlayer = find.byKey(
        ValueKey<String>(
          'home-player-name-${config.first == 1 ? 'white' : 'black'}',
        ),
      );
      expect(
        boardSize.width,
        closeTo(LitchWorkspaceGeometry.portraitCanvasSide(390), 0.01),
      );
      expect(boardSize.height, boardSize.width);
      expect(
        find.ancestor(of: board, matching: gameSurface),
        findsNothing,
        reason: 'The Play board stays outside the dark status surface.',
      );
      expect(
        tester.getRect(topPlayer).top,
        closeTo(
          tester.getRect(gameSurface).bottom + LitchWorkspaceGeometry.outerGap,
          0.01,
        ),
      );
      expect(tester.getSize(topPlayer).height, greaterThanOrEqualTo(48));
      expect(
        tester.getRect(board).top,
        closeTo(tester.getRect(topPlayer).bottom, 0.01),
      );
      expect(
        tester.getRect(bottomPlayer).top,
        closeTo(tester.getRect(board).bottom, 0.01),
      );
      expect(tester.getSize(bottomPlayer).height, greaterThanOrEqualTo(48));
      expect(
        tester.getTopLeft(board).dx,
        closeTo(LitchWorkspaceGeometry.canvasHorizontalInset, 0.01),
      );
      final boardLeft = tester
          .getTopLeft(
            find.byKey(const ValueKey<String>('home-chessboard-surface')),
          )
          .dx;
      final guidanceLeft = tester
          .getTopLeft(find.byKey(const ValueKey<String>('home-guidance-rail')))
          .dx;
      expect(guidanceLeft, greaterThan(boardLeft));
      final topSide = config.first == 1 ? 'black' : 'white';
      final bottomSide = config.first == 1 ? 'white' : 'black';
      final topSwatch = find.byKey(
        ValueKey('home-player-color-swatch-$topSide'),
      );
      final bottomSwatch = find.byKey(
        ValueKey('home-player-color-swatch-$bottomSide'),
      );
      int swatchColor(Finder swatch) =>
          (tester.widget<Container>(swatch).decoration! as BoxDecoration).color!
              .toARGB32();
      expect(swatchColor(topSwatch), 0xffdc9100);
      expect(swatchColor(bottomSwatch), 0xff00c8ae);
      expect(
        tester.getRect(board).top - tester.getRect(topSwatch).bottom,
        closeTo(
          tester.getRect(bottomSwatch).top - tester.getRect(board).bottom,
          .01,
        ),
        reason: 'Visible swatches have equal gaps above and below the board.',
      );
      final topName = find.descendant(
        of: find.byKey(ValueKey('home-player-name-edit-$topSide')),
        matching: find.text('Add name'),
      );
      final bottomName = find.descendant(
        of: find.byKey(ValueKey('home-player-name-edit-$bottomSide')),
        matching: find.text('Add name'),
      );
      expect(
        tester.getRect(board).top - tester.getRect(topName).bottom,
        closeTo(
          tester.getRect(bottomName).top - tester.getRect(board).bottom,
          .01,
        ),
        reason: 'Visible name labels have equal board-edge gaps.',
      );
      expect(
        tester.getRect(board).top - tester.getRect(gameSurface).bottom,
        closeTo(
          tester.getSize(topPlayer).height + LitchWorkspaceGeometry.outerGap,
          .01,
        ),
        reason: 'The board and lower row retain their existing anchors.',
      );
      final scrollPosition = tester
          .state<ScrollableState>(_homeDetailsScrollable())
          .position;
      final bottomContentEdge =
          tester.getRect(bottomPlayer).bottom + scrollPosition.pixels;
      await tester.scrollUntilVisible(
        boardActions,
        100,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pump();
      expect(
        tester.getRect(boardActions).top +
            scrollPosition.pixels -
            bottomContentEdge,
        closeTo(12, 0.01),
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
  });

  testWidgets('uses the duck cameo and keeps a synchronized Home quiet', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    await tester.runAsync(
      () => precacheImage(
        const AssetImage(HomeChessboard.duckAsset),
        tester.element(find.byType(HomeScreen)),
      ),
    );
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00004000ffff","g":"d","duck":"g5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pump();
    await tester.pump();

    expect(find.text('P1 to move'), findsOneWidget);
    expect(find.text('Duck · Move 1'), findsOneWidget);
    final double contentStart =
        tester
            .getTopRight(
              find.byKey(const ValueKey<String>('home-guidance-rail')),
            )
            .dx +
        12;
    expect(tester.getTopLeft(find.text('P1 to move')).dx, contentStart);
    expect(
      tester
          .getTopLeft(find.byKey(const ValueKey<String>('duck-variant-icon')))
          .dx,
      contentStart,
    );
    expect(
      find.byKey(const ValueKey<String>('duck-variant-icon')),
      findsOneWidget,
    );
    expect(find.byIcon(Icons.pets_outlined), findsNothing);
    expect(
      find.text('The physical pieces and game position agree.'),
      findsNothing,
    );
  });

  testWidgets('shows and clears the authoritative Vex move-first instruction', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    const String base =
        '{"event":"home_state","v":1,"seq":1,"a":"corr",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"ffff00001000ffff","g":"d","duck":"e3",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0';
    ble.controller.add(const <String>['$base,"vx":1}']);
    await tester.pump();
    await tester.pump();

    expect(find.text('YOUR MOVE'), findsOneWidget);
    expect(find.text('Vex is in the way. Move Vex first.'), findsOneWidget);

    final String cleared = "${base.replaceFirst('"seq":1', '"seq":2')}}";
    ble.controller.add(<String>[cleared]);
    await tester.pump();
    await tester.pump();
    expect(find.text('Vex is in the way. Move Vex first.'), findsNothing);
  });

  testWidgets(
    'keeps provisional Duck ownership quiet until the variant latch',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);

      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"ffff00004000ffff","g":"u","duck":"g5",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
      ]);
      await tester.pump();
      await tester.pump();

      expect(find.text('P1 to move'), findsOneWidget);
      expect(find.text('Undecided'), findsNothing);
      expect(find.textContaining('square to check'), findsNothing);
      expect(find.textContaining('Unexpected occupied'), findsNothing);
    },
  );

  testWidgets('never flashes a warning before Duck candidate publication', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    const String preCandidate =
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"ffff00004000ffff","g":"u","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
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

    for (final String snapshot in <String>[
      preCandidate,
      provisional,
      latched,
    ]) {
      ble.controller.add(<String>[snapshot]);
      await tester.pump();
      await tester.pump();
      expect(find.textContaining('Unexpected occupied'), findsNothing);
      expect(find.textContaining('square to check'), findsNothing);
      expect(find.textContaining('Amber marks'), findsNothing);
    }
    expect(find.text('Duck · Move 1'), findsOneWidget);
  });

  testWidgets('surfaces future informational firmware notices', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '10:00:00.000 [FW] '
          '{"event":"notice","tone":"info","message":"Ready for your move."}',
    ]);
    await tester.pump();
    await tester.pump();

    expect(find.text('Ready for your move.'), findsOneWidget);
  });

  testWidgets('Coaching never queues stale confirmation behind a board notice', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    ble.controller.add([_cpuState(1, 'idle', ',"co":0')]);
    await tester.pumpAndSettle();
    final Finder p1 = find.byKey(const ValueKey<String>('control:coaching:p1'));
    final actions = tester
        .widget<HomeBoardActions>(find.byType(HomeBoardActions))
        .controller;
    bool p1Pending() =>
        actions.isPending(LitchControlAction.coaching, LitchControlOwner.p1);
    await tester.scrollUntilVisible(
      p1,
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    ble.controller.add(const <String>[
      '{"event":"notice","tone":"warning","message":"Check the board position."}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('Check the board position.'), findsOneWidget);
    await tester.tap(p1);
    await tester.pump();
    expect(p1Pending(), isTrue);
    ble.controlController.add(
      const LitchControlEvent(
        requestId: 1,
        action: LitchControlAction.coaching,
        owner: LitchControlOwner.p1,
        phase: LitchControlPhase.applied,
      ),
    );
    ble.controller.add([_cpuState(2, 'idle', ',"co":1')]);
    await tester.pumpAndSettle();
    expect(p1Pending(), isFalse);
    expect(tester.getSemantics(p1).flagsCollection.isToggled, Tristate.isTrue);
    expect(find.text('Check the board position.'), findsOneWidget);
    // A newer action makes any queued success obsolete before it is shown.
    await tester.tap(p1);
    await tester.pump();
    expect(p1Pending(), isTrue);
    expect(tester.takeException(), isNull);
    expect(find.text('Check the board position.'), findsOneWidget);
    await tester.pump(const Duration(seconds: 5));
    // The replacement request is pending as the board notice exits.
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.textContaining('P1 coaching changed.'), findsNothing);
    expect(find.byType(SnackBar), findsNothing);
  });

  testWidgets('presents a legal live lift as calm positive progress', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":4,"a":"lift",'
          '"fen":"8/8/8/8/8/8/8/6Nk w - - 0 1",'
          '"occ":"8000000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pump();
    await tester.pump();

    expect(find.text('Move in progress'), findsOneWidget);
    expect(find.text('P1 knight expected on g1.'), findsNothing);
    expect(
      find.text('The physical board and app are following this move together.'),
      findsNothing,
    );
    expect(find.text('1 square to check'), findsNothing);
  });

  testWidgets('Duck settling stops asking for another Duck placement', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"settle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFE00010001FFFF","g":"d","duck":"a5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pump();
    await tester.pump();

    expect(find.text('MOVE IN PROGRESS'), findsOneWidget);
    expect(find.text('Move in progress'), findsOneWidget);
    expect(find.text('DUCK MOVE'), findsNothing);
    expect(find.text('Place the duck'), findsNothing);
    expect(find.textContaining('Duck hint:'), findsNothing);
  });

  testWidgets('mirrors the board-first promotion instructions', (
    WidgetTester tester,
  ) async {
    final SemanticsHandle semantics = tester.ensureSemantics();
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(
      _harness(ble, textScaler: const TextScaler.linear(2)),
    );
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"promote",'
          '"fen":"7k/1P6/8/8/8/8/8/K7 w - - 0 1",'
          '"occ":"0100000000000082","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('PROMOTION'), findsOneWidget);
    expect(find.text('Choose a promotion piece'), findsOneWidget);
    expect(
      find.text(
        'Use Left or Right on either board D-pad to choose Queen, Rook, Bishop, or Knight, then press Circle to confirm.',
      ),
      findsOneWidget,
    );
    expect(find.text('Move in progress'), findsNothing);
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('home-guidance-live-region')),
      ),
      isSemantics(
        label:
            'PROMOTION. Choose a promotion piece. Use Left or Right on either board D-pad to choose Queen, Rook, Bishop, or Knight, then press Circle to confirm.',
        isLiveRegion: true,
      ),
    );
    final Finder promotedPawn = find.byKey(
      const ValueKey<String>('square-b7'),
      skipOffstage: false,
    );
    await tester.scrollUntilVisible(
      promotedPawn,
      200,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    expect(tester.getSemantics(promotedPawn).label, 'P1 pawn lifted from b7');
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('square-b8')))
          .label,
      'Move in progress on b8',
    );
    expect(tester.takeException(), isNull);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('home-board-actions')),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
    semantics.dispose();
  });

  testWidgets(
    'Duck relocation correction preserves factual progress and legacy guidance',
    (WidgetTester tester) async {
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final semantics = tester.ensureSemantics();
      try {
        await tester.pumpWidget(_harness(ble));
        await tester.pump();
        void emit(int sequence, String action, String occupied, String extra) {
          ble.controller.add(<String>[
            '{"event":"home_state","v":1,"seq":$sequence,"a":"$action",'
                '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
                '"occ":"$occupied","g":"d","duck":"d5",'
                '"out":"n","win":"n","rot":0,"white":1,"pm":0'
                '$extra}',
          ]);
        }

        emit(1, 'corr', 'FFEF00100001FFFF', ',"dr":"cd5c6"');
        await tester.pumpAndSettle();
        expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
        expect(find.text('Follow the board’s guidance'), findsOneWidget);
        expect(find.text('Place the duck'), findsNothing);
        expect(
          find.textContaining('The board’s LEDs are guiding'),
          findsNothing,
        );
        expect(find.textContaining('expected on'), findsNothing);
        expect(find.byKey(const ValueKey('cpu-move-to-c6')), findsNothing);
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-e2'))).label,
          'P1 pawn lifted from e2',
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-e4'))).label,
          'Move in progress on e4',
        );

        emit(2, 'duck', 'FFEF00100001FFFF', '');
        await tester.pumpAndSettle();
        expect(find.text('DUCK MOVE'), findsOneWidget);
        expect(find.text('Place the duck'), findsOneWidget);
        expect(find.text('POSITION NEEDS CORRECTION'), findsNothing);

        emit(3, 'corr', 'FFEF00000800FFFF', '');
        await tester.pumpAndSettle();
        expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
        expect(find.textContaining('P1 pawn expected on e2'), findsOneWidget);
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-e2'))).label,
          'P1 pawn expected on e2',
        );

        emit(4, 'corr', 'FFEF00100001FFFF', ',"dr":"cd5c6","tw":"b"');
        await tester.pumpAndSettle();
        expect(find.text('GAME COMPLETE'), findsOneWidget);
        expect(find.text('P2 wins on time'), findsOneWidget);
        expect(find.text('POSITION NEEDS CORRECTION'), findsNothing);
        expect(find.text('Place the duck'), findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  for (final bool firstP2 in [false, true]) {
    for (final bool rotated in [false, true]) {
      testWidgets(
        'canonical Home guidance and history firstP2=$firstP2 rotate=$rotated',
        (tester) async {
          final ble = _FakeLitchBleService()
            ..latestSettings['GAME_FIRST_PLAYER'] = firstP2 ? 2 : 1;
          addTearDown(ble.dispose);
          final semantics = tester.ensureSemantics();
          try {
            await tester.binding.setSurfaceSize(const Size(800, 1600));
            addTearDown(() => tester.binding.setSurfaceSize(null));
            await tester.pumpWidget(_harness(ble));
            await tester.pump();
            var sequence = 1;
            Future<void> publish(
              String action,
              String extra, {
              bool duck = false,
              bool captureAtDestination = false,
            }) async {
              final fen = captureAtDestination
                  ? '7k/8/3p4/4P3/8/8/8/K7 w - - 0 1'
                  : '7k/8/8/3pP3/8/8/8/K7 w - d6 0 1';
              final occupied =
                  HomeChessPosition.tryParse(fen)!.occupiedSquares |
                  (duck ? BigInt.one << 24 : BigInt.zero);
              ble.controller.add([
                '{"event":"home_state","v":1,"seq":${sequence++},"a":"$action",'
                    '"fen":"$fen",'
                    '"occ":"${occupied.toRadixString(16).padLeft(16, '0')}","g":"${duck ? 'd' : 'c'}",'
                    '"duck":"${duck ? 'a5' : '--'}","out":"n","win":"n",'
                    '"rot":${rotated ? 1 : 0},"white":1,"pm":0$extra}',
              ]);
              await tester.pumpAndSettle();
            }

            const from = 'e5';
            const to = 'd6';
            const separateCapture = 'd5';
            for (final captureAtDestination in [false, true]) {
              final capture = captureAtDestination ? to : separateCapture;
              await publish(
                'cpu',
                ',"c":["e5","d6","${captureAtDestination ? 'd6' : 'd5'}"]',
                captureAtDestination: captureAtDestination,
              );
              final instruction =
                  'Remove the piece on $capture, then move the piece from $from to $to.';
              expect(find.text(instruction), findsOneWidget);
              expect(
                tester.getSemantics(
                  find.byKey(const ValueKey('home-guidance-live-region')),
                ),
                isSemantics(
                  label: 'COMPUTER MOVE. Computer move ready. $instruction',
                  isLiveRegion: true,
                ),
              );
              for (final marker in [
                'cpu-move-from-$from',
                'cpu-move-to-$to',
                'capture-removal-$capture',
              ]) {
                expect(find.byKey(ValueKey(marker)), findsOneWidget);
              }
              expect(
                tester.getSemantics(find.byKey(ValueKey('square-$from'))).label,
                contains(from),
              );
              expect(
                tester.getSemantics(find.byKey(ValueKey('square-$to'))).label,
                contains(to),
              );
            }

            const duckFrom = 'a5';
            const duckTo = 'd4';
            for (final owner in ['c', 'u']) {
              await publish('duck', ',"dr":"${owner}a5d4"', duck: true);
              final instruction = 'Move the duck from $duckFrom to $duckTo.';
              expect(find.text(instruction), findsOneWidget);
              expect(
                tester.getSemantics(
                  find.byKey(const ValueKey('home-guidance-live-region')),
                ),
                isSemantics(
                  label:
                      '${owner == 'c' ? 'COMPUTER MOVE' : 'DUCK MOVE'}. Place the duck. $instruction',
                  isLiveRegion: true,
                ),
              );
              expect(
                find.byKey(ValueKey('duck-marker-$duckFrom')),
                findsOneWidget,
              );
              expect(
                find.byKey(ValueKey('cpu-move-to-$duckTo')),
                owner == 'c' ? findsOneWidget : findsNothing,
              );
            }

            await publish('idle', ',"m":"e2e4e7e5b"');
            const p1From = 'e2';
            const p1To = 'e4';
            const p2From = 'e7';
            const p2To = 'e5';
            expect(find.text('$p1From → $p1To'), findsOneWidget);
            expect(find.text('$p2From → $p2To'), findsOneWidget);
            expect(
              find.bySemanticsLabel('Player 1 latest move, $p1From to $p1To'),
              findsOneWidget,
            );
            expect(
              find.bySemanticsLabel(
                'Player 2 latest move, $p2From to $p2To, most recent',
              ),
              findsOneWidget,
            );
            for (final marker in [
              'last-move-white-from-$p1From',
              'last-move-white-to-$p1To',
              'last-move-black-from-$p2From',
              'last-move-black-to-$p2To',
            ]) {
              expect(find.byKey(ValueKey(marker)), findsOneWidget);
            }
          } finally {
            semantics.dispose();
          }
        },
      );
    }
  }

  testWidgets('shows an exact computer move in guidance and on the board', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"cpu",'
          '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
          '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"c":["g8","f6","--"]}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('Computer move ready'), findsOneWidget);
    expect(find.text('Move the computer piece from g8 to f6.'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('cpu-move-from-g8')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('cpu-move-to-f6')),
      findsOneWidget,
    );
  });

  testWidgets('review Home assistance at phone and large text sizes', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    addTearDown(() => tester.binding.setSurfaceSize(null));
    for (final (double width, double scale, bool dark)
        in <(double, double, bool)>[
          (390, 1, false),
          (390, 1, true),
          (320, 2, false),
        ]) {
      await tester.binding.setSurfaceSize(Size(width, 844));
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      await tester.pumpWidget(
        _harness(ble, textScaler: TextScaler.linear(scale), dark: dark),
      );
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
            '"occ":"0110000001000080","g":"d","duck":"a5",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
            '"co":0,"cl":[0,0,0,"w",0,0,0,"n"],"as":"3be2e4a5d4"}',
      ]);
      await tester.pumpAndSettle();
      await tester.runAsync(() async {
        final BuildContext context = tester.element(
          find.byType(HomeChessboard),
        );
        for (final Image image in tester.widgetList<Image>(
          find.byType(Image),
        )) {
          await precacheImage(image.image, context);
        }
      });
      await tester.pumpAndSettle();
      final String suffix =
          '${width.toInt()}-${scale.toInt()}-${dark ? 'dark' : 'light'}';
      expect(find.text('Hint: e2 to e4.'), findsOneWidget);
      expect(find.textContaining('Duck hint:'), findsNothing);
      expect(find.byKey(const ValueKey('duck-assistance-to-d4')), findsNothing);
      expect(
        tester
            .widget<Text>(find.byKey(const ValueKey('home-hint-status')))
            .data,
        'You can play this hint now.',
      );
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'assistance-duck-$suffix');
      final boardBeforeUpdate = tester.getRect(find.byType(HomeChessboard));
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":2,"a":"idle",'
            '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
            '"occ":"0110000001000080","g":"d","duck":"a5",'
            '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
            '"co":0,"cl":[0,0,0,"w",0,0,0,"n"],"as":"3be2e3a5e4"}',
      ]);
      await tester.pumpAndSettle();
      expect(find.text('Hint: e2 to e3.'), findsOneWidget);
      expect(find.textContaining('Duck hint:'), findsNothing);
      expect(find.byKey(const ValueKey('duck-assistance-to-e4')), findsNothing);
      expect(
        tester
            .widget<Text>(find.byKey(const ValueKey('home-hint-status')))
            .data,
        'Hint updated. Previously: e2 to e4. Now: e2 to e3.',
      );
      expect(tester.getRect(find.byType(HomeChessboard)), boardBeforeUpdate);
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'assistance-duck-updated-$suffix');
      final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
      await tester.scrollUntilVisible(
        hint,
        200,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      expect(find.text('Hide hint'), findsOneWidget);
      expect(tester.getSize(hint).height, greaterThanOrEqualTo(48));
      final actions = tester.getRect(
        find.byKey(const ValueKey('board-action-controls')),
      );
      for (final player in ['p1', 'p2']) {
        final coaching = tester.getRect(
          find.byKey(ValueKey('control:coaching:$player')),
        );
        expect(
          coaching.top,
          greaterThan(actions.bottom),
          reason: 'All three game actions precede both coaching controls.',
        );
        expect(
          tester.getRect(find.byKey(const ValueKey('home-clock-strip'))).top,
          greaterThan(coaching.bottom),
          reason: 'Clock controls remain below coaching.',
        );
      }
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'assistance-controls-$suffix');
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
  });

  testWidgets('ready Hint replay Applied then new FEN retires old guidance', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final semantics = tester.ensureSemantics();
    final ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final events = <LitchControlEvent>[];
    final subscription = ble.controlActionEvents.listen(events.add);
    addTearDown(subscription.cancel);
    const readyFen =
        '1rb1k1nr/pp1p1ppp/2npp3/qN6/3P1B2/5N2/PPP2PPP/R2QKB1R w KQk - 0 1';
    const nextFen =
        '1rbqk1nr/pp1p1ppp/2npp3/1N6/3P1B2/5N2/PPP2PPP/R2QKB1R b KQk - 0 1';
    void publish(
      int sequence, {
      bool ready = true,
      String assistance = '7rc2c3',
    }) {
      ble.controller.add([
        jsonEncode({
          'event': 'home_state',
          'v': 1,
          'seq': sequence,
          'a': ready ? 'idle' : 'corr',
          'fen': ready ? readyFen : nextFen,
          'occ': 'B9E72028031CEBD6',
          'g': 'c',
          'duck': '--',
          'out': 'n',
          'win': 'n',
          'rot': 0,
          'white': 1,
          'pm': ready ? 0 : 1,
          'co': ready ? 1 : 0,
          'cl': [0, 0, 0, 'w', 0, 0, 0, 'n'],
          'as': ready ? assistance : '0x',
        }),
      ]);
    }

    await tester.pumpWidget(
      _harness(ble, platform: TargetPlatform.iOS, reduceMotion: false),
    );
    publish(5);
    await tester.pump();
    final status = find.byKey(const ValueKey('home-hint-status-semantics'));
    expect(
      tester.getSemantics(status).label,
      'Still analysing. You can play this hint now.',
    );
    expect(tester.getSemantics(status).flagsCollection.isLiveRegion, isFalse);
    final hint = find.byKey(const ValueKey<String>('control:hint'));
    await tester.scrollUntilVisible(
      hint,
      200,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    final actions = tester
        .widget<HomeBoardActions>(find.byType(HomeBoardActions))
        .controller;
    expect(actions.hintLabel, 'Show Move');
    expect(find.byKey(const ValueKey('assistance-to-c3')), findsOneWidget);
    await tester.tap(hint);
    await tester.pump();
    final pending = events.single;
    expect(pending.action, LitchControlAction.hint);
    expect(pending.phase, LitchControlPhase.pending);
    expect(actions.isPending(LitchControlAction.hint), isTrue);
    ble.controlController.add(
      LitchControlEvent(
        requestId: pending.requestId,
        action: LitchControlAction.hint,
        phase: LitchControlPhase.applied,
      ),
    );
    await tester.pump();
    publish(6);
    await tester.pump();
    expect(actions.isPending(LitchControlAction.hint), isFalse);
    expect(actions.hintLabel, 'Show Move');
    expect(find.byKey(const ValueKey('assistance-to-c3')), findsOneWidget);

    await tester.scrollUntilVisible(
      status,
      -200,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pump();
    expect(find.textContaining('Hint updated.'), findsNothing);
    publish(7, assistance: '7rc2c4');
    await tester.pump();
    expect(find.byKey(const ValueKey('assistance-to-c4')), findsOneWidget);
    expect(find.byKey(const ValueKey('assistance-to-c3')), findsNothing);
    expect(
      tester.getSemantics(status).label,
      'Hint updated. Previously: c2 to c3. Now: c2 to c4. Still analysing.',
    );
    final guidance = find.byKey(const ValueKey('home-guidance-live-region'));
    final updateAnnouncement = tester.getSemantics(guidance).label;
    expect(
      updateAnnouncement,
      contains('Hint updated. Previously: c2 to c3. Now: c2 to c4.'),
    );
    publish(8, assistance: '7rc2c4');
    await tester.pump();
    expect(tester.getSemantics(guidance).label, updateAnnouncement);
    publish(9, assistance: '7hc2c4');
    await tester.pump();
    expect(tester.getSemantics(guidance).label, updateAnnouncement);
    expect(
      tester.getSemantics(status).label,
      'Hint updated. Previously: c2 to c3. Now: c2 to c4.',
    );

    publish(10, ready: false);
    debugPrint('HINT_REPLACEMENT before new-authority frame');
    await tester.pump();
    final snapshot = tester
        .widget<HomeChessboard>(find.byType(HomeChessboard))
        .snapshot;
    expect(snapshot.liveSequence, 10);
    expect(snapshot.position!.fen, nextFen);
    expect(snapshot.homeStateAction, HomeStateAction.correction);
    expect(snapshot.visibleAssistance, isNull);
    expect(find.byKey(const ValueKey('assistance-to-c4')), findsNothing);
    expect(status, findsNothing);
    expect(actions.isPending(LitchControlAction.hint), isFalse);
    expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
    expect(tester.takeException(), isNull);
    debugPrint('HINT_REPLACEMENT finite frame with old guidance retired');
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    semantics.dispose();
  });

  for (final bool expire in <bool>[true, false]) {
    testWidgets(
      'lost pending-Cancel confirmation clears controls and board guidance together (expire=$expire)',
      (WidgetTester tester) async {
        final _FakeLitchBleService ble = _FakeLitchBleService();
        addTearDown(ble.dispose);
        void emitReady(int seq) => ble.controller.add(<String>[
          '{"event":"home_state","v":1,"seq":$seq,"a":"idle",'
              '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
              '"occ":"0110000000000080","g":"c","duck":"--",'
              '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
              '"as":"3he2e4"}',
        ]);
        await tester.pumpWidget(_harness(ble));
        await tester.pump();
        ble.controller.add(const <String>[
          '{"event":"home_state","v":1,"seq":1,"a":"idle",'
              '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
              '"occ":"0110000000000080","g":"c","duck":"--",'
              '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
              '"as":"3t"}',
        ]);
        await tester.pump();
        expect(
          find.byKey(const ValueKey<String>('assistance-to-e4')),
          findsNothing,
        );
        final Finder hint = find.byKey(const ValueKey<String>('control:hint'));
        await tester.scrollUntilVisible(
          hint,
          200,
          scrollable: _homeDetailsScrollable(),
        );
        await tester.pump();
        expect(find.text('Cancel hint'), findsOneWidget);
        await tester.tap(hint);
        await tester.pump();
        ble.controlController.add(
          const LitchControlEvent(
            requestId: 1,
            action: LitchControlAction.hint,
            phase: LitchControlPhase.applied,
          ),
        );
        await tester.pump();
        // A completed result arriving after Cancel is not proof that the
        // requested=false latch was applied. Do not rehydrate its guidance.
        emitReady(2);
        await tester.pump();
        if (expire) {
          await tester.pump(const Duration(seconds: 3));
          expect(
            find.text('Hint state is unavailable. Follow the board lights.'),
            findsOneWidget,
          );
          expect(find.text('Show Move'), findsNothing);
          await tester.drag(_homeDetailsScrollable(), const Offset(0, 1200));
          await tester.pumpAndSettle();
          expect(
            find.byKey(const ValueKey<String>('assistance-to-e4')),
            findsNothing,
          );
          expect(find.text('Hint: e2 to e4.'), findsNothing);
        } else {
          expect(
            find.text('Updating hint state from the board…'),
            findsNothing,
          );
          expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
        }
        // Reopening Home must not rehydrate the old ready hint from the service.
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
        await tester.pumpWidget(_harness(ble));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey<String>('assistance-to-e4')),
          findsNothing,
        );
        expect(find.text('Hint: e2 to e4.'), findsNothing);
        emitReady(3);
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey<String>('assistance-to-e4')),
          findsOneWidget,
        );
        expect(find.text('Hint: e2 to e4.'), findsOneWidget);
      },
    );
  }

  testWidgets('Duck physical plan owns copy and CPU markers through recovery', (
    tester,
  ) async {
    final ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final semantics = tester.ensureSemantics();
    try {
      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      var sequence = 30;
      Future<void> publish(String extra) async {
        ble.controller.add([
          '{"event":"home_state","v":1,"seq":${sequence++},"a":"duck",'
              '"fen":"7k/8/8/8/4P3/8/8/K7 w - - 0 1",'
              '"occ":"0100001001000080","g":"d","duck":"a5",'
              '"out":"n","win":"n","rot":0,"white":1,"pm":0$extra}',
        ]);
        await tester.pumpAndSettle();
      }

      // Stale settings still say two humans. Only the current board plan owns
      // this computer instruction; neither FEN side nor configuration infers it.
      await publish(',"dr":"ca5d4"');
      expect(find.text('COMPUTER MOVE'), findsOneWidget);
      expect(find.text('Place the duck'), findsOneWidget);
      expect(find.text('Move the duck from a5 to d4.'), findsOneWidget);
      expect(find.byKey(const ValueKey('cpu-move-from-a5')), findsOneWidget);
      expect(find.byKey(const ValueKey('cpu-move-to-d4')), findsOneWidget);
      expect(
        find.bySemanticsLabel(RegExp('.*Computer move starts here.*')),
        findsOneWidget,
      );
      for (final omitted in ['', ',"dr":"unsupported"']) {
        await publish(omitted);
        expect(find.text('DUCK MOVE'), findsOneWidget);
        expect(find.text('YOUR MOVE'), findsNothing);
        expect(find.text('Follow the board’s guidance'), findsOneWidget);
        expect(find.byKey(const ValueKey('cpu-move-from-a5')), findsNothing);
        expect(find.byKey(const ValueKey('cpu-move-to-d4')), findsNothing);
      }
      await publish(',"dr":"ca5d4"');
      expect(find.byKey(const ValueKey('cpu-move-to-d4')), findsOneWidget);
      await publish(',"dr":"ua5d4"');
      expect(find.text('DUCK MOVE'), findsOneWidget);
      expect(find.text('Move the duck from a5 to d4.'), findsOneWidget);
      expect(find.byKey(const ValueKey('cpu-move-to-d4')), findsNothing);
      await publish(',"dr":"ha5--"');
      expect(find.text('YOUR MOVE'), findsOneWidget);
      expect(
        find.text('Move the duck to a different empty square.'),
        findsOneWidget,
      );
      await publish(',"dr":"ca5d4","tw":"b"');
      expect(find.text('P2 wins on time'), findsOneWidget);
      expect(find.text('Place the duck'), findsNothing);
      expect(find.byKey(const ValueKey('cpu-move-to-d4')), findsNothing);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('one Duck hint follows the live chess and Duck handoff', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"7k/8/8/8/8/5p2/4P3/K7 w - - 0 1",'
          '"occ":"0110200001000080","g":"d","duck":"a5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"as":"3be2f3a5d4"}',
    ]);
    await tester.pumpAndSettle();
    expect(
      find.text('Remove the piece on f3, then move the piece from e2 to f3.'),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('capture-removal-f3')), findsOneWidget);
    expect(find.byKey(const ValueKey('duck-assistance-to-d4')), findsNothing);
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":2,"a":"idle",'
          '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
          '"occ":"0110000001000080","g":"d","duck":"a5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"as":"3be2e4a5d4"}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('Hint: e2 to e4.'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('assistance-to-e4')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('duck-assistance-to-d4')),
      findsNothing,
    );
    // An older board may publish the remaining Duck hint while the chess
    // move is still settling. Neither text nor markers may advance early.
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":3,"a":"settle",'
          '"fen":"7k/8/8/8/8/8/4P3/K7 w - - 0 1",'
          '"occ":"0100001001000080","g":"d","duck":"a5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"as":"3da5d4"}',
    ]);
    await tester.pumpAndSettle();
    expect(find.textContaining('Duck hint:'), findsNothing);
    expect(find.byKey(const ValueKey('duck-assistance-to-d4')), findsNothing);
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":4,"a":"duck",'
          '"fen":"7k/8/8/8/4P3/8/8/K7 w - - 0 1",'
          '"occ":"0100001001000080","g":"d","duck":"a5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"as":"3da5d4"}',
    ]);
    await tester.pumpAndSettle();
    expect(find.text('Place the duck'), findsOneWidget);
    expect(find.text('Duck hint: a5 to d4.'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('assistance-to-e4')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('duck-assistance-to-d4')),
      findsOneWidget,
    );
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"idle",'
          '"fen":"7k/8/8/8/4P3/8/8/K7 b - - 0 1",'
          '"occ":"0100001800000080","g":"d","duck":"d4",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    expect(find.textContaining('Duck hint:'), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('duck-assistance-to-d4')),
      findsNothing,
    );
    expect(find.text('Place the duck'), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('announces BLE-driven guidance changes as one live region', (
    WidgetTester tester,
  ) async {
    final SemanticsHandle semantics = tester.ensureSemantics();
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    final Finder liveRegion = find.byKey(
      const ValueKey<String>('home-guidance-live-region'),
    );

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"cpu",'
          '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
          '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"c":["g8","f6","--"]}',
    ]);
    await tester.pumpAndSettle();

    final Finder board = find.byKey(
      const ValueKey<String>('home-chessboard-surface'),
    );
    final double stableBoardTop = tester.getTopLeft(board).dy;

    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label:
            'COMPUTER MOVE. Computer move ready. Move the computer piece from g8 to f6.',
        isLiveRegion: true,
      ),
    );
    expect(find.bySemanticsLabel('Computer move ready'), findsNothing);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":6,"a":"duck",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00004000ffff","g":"d","duck":"g5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    expect(tester.getTopLeft(board).dy, stableBoardTop);
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label: 'DUCK MOVE. Place the duck. Follow the board’s guidance',
        isLiveRegion: true,
      ),
    );

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":7,"a":"corr",'
          '"fen":"8/8/8/8/8/3B4/8/6Kk w - - 0 1",'
          '"occ":"c000000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    expect(tester.getTopLeft(board).dy, stableBoardTop);
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label:
            'POSITION NEEDS CORRECTION. Follow the board’s guidance. Player 1 bishop expected on d3.',
        isLiveRegion: true,
      ),
    );

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"ffff00000000ffff","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    expect(tester.getTopLeft(board).dy, stableBoardTop);
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label: 'Player 1 to move. Classic · Move 1',
        isLiveRegion: true,
      ),
    );
    expect(find.text('CURRENT GAME'), findsNothing);
    expect(
      tester.getTopLeft(find.text('Classic · Move 1')).dx,
      tester.getTopLeft(find.text('P1 to move')).dx,
    );
    semantics.dispose();
  });

  testWidgets('keeps Home geometry stable while connection state hydrates', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    final Finder board = find.byKey(
      const ValueKey<String>('home-chessboard-surface'),
    );
    final Finder actions = find.byKey(
      const ValueKey<String>('home-board-actions'),
    );
    final double initialBoardTop = tester.getTopLeft(board).dy;
    final double initialActionsTop = tester.getTopLeft(actions).dy;

    ble.controller.add(const <String>[
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    ]);
    await tester.pumpAndSettle();
    expect(tester.getTopLeft(board).dy, initialBoardTop);
    expect(tester.getTopLeft(actions).dy, initialActionsTop);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
          '"cl":[0,0,0,"w",0,0,0,"y"]}',
    ]);
    await tester.pumpAndSettle();
    expect(tester.getTopLeft(board).dy, initialBoardTop);
    expect(tester.getTopLeft(actions).dy, initialActionsTop);
  });

  testWidgets('selects the pregame clock and keeps the board anchored', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    ble.latestSettings
      ..['P1_PIECE_WHEEL'] = 85
      ..['P2_PIECE_WHEEL'] = 0;
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    const String pregame =
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
        '"cl":[0,0,0,"w",0,0,0,"y"]}';
    ble.controller.add(const <String>[pregame]);
    await tester.pumpAndSettle();
    expect(find.byIcon(Icons.play_arrow_rounded), findsNothing);
    expect(find.byIcon(Icons.pause_rounded), findsNothing);

    final Finder board = find.byKey(
      const ValueKey<String>('home-chessboard-surface'),
    );
    final Finder selector = find.byKey(
      const ValueKey<String>('home-game-clock'),
    );
    final Finder actions = find.byKey(
      const ValueKey<String>('home-board-actions'),
    );
    expect(selector, findsOneWidget);
    expect(tester.getSize(selector).height, greaterThanOrEqualTo(48));
    expect(find.ancestor(of: selector, matching: actions), findsOneWidget);
    expect(
      tester.getTopLeft(selector).dy,
      greaterThan(tester.getBottomLeft(board).dy),
    );
    final Finder p1Clock = find.byKey(const ValueKey<String>('home-clock-p1'));
    final Finder p2Clock = find.byKey(const ValueKey<String>('home-clock-p2'));
    expect(tester.getTopLeft(p1Clock).dy, tester.getTopLeft(selector).dy);
    expect(tester.getTopLeft(p2Clock).dy, tester.getTopLeft(selector).dy);
    expect(
      tester.getTopLeft(selector).dx,
      greaterThan(tester.getTopRight(p2Clock).dx),
    );
    final Finder dropdown = find.descendant(
      of: selector,
      matching: find.byType(DropdownButton<int>),
    );
    // The full-size board intentionally places the remaining Play controls
    // below the fold on this compact phone. Exercise the real scroll path
    // before measuring the visible touch target and opening the clock menu.
    await tester.ensureVisible(dropdown);
    await tester.pumpAndSettle();
    expect(tester.getSemantics(dropdown).label, contains('TIME LIMIT'));
    expect(tester.getSemantics(dropdown).rect.height, greaterThanOrEqualTo(48));
    expect(
      tester.getSemantics(dropdown).rect.height,
      lessThanOrEqualTo(tester.getSize(selector).height),
    );
    final BoxDecoration p1Decoration =
        tester
                .widget<Container>(
                  find.byKey(const ValueKey<String>('home-clock-p1')),
                )
                .decoration
            as BoxDecoration;
    final BoxDecoration p2Decoration =
        tester
                .widget<Container>(
                  find.byKey(const ValueKey<String>('home-clock-p2')),
                )
                .decoration
            as BoxDecoration;
    expect(p1Decoration.color, isNot(p2Decoration.color));
    expect(
      _contrastRatio(
        (p1Decoration.border! as Border).top.color,
        p1Decoration.color!,
      ),
      greaterThanOrEqualTo(3),
    );
    expect(
      _contrastRatio(
        (p2Decoration.border! as Border).top.color,
        p2Decoration.color!,
      ),
      greaterThanOrEqualTo(3),
    );
    double boardContentTop() =>
        tester.getTopLeft(board).dy +
        Scrollable.of(tester.element(board)).position.pixels;
    final double pregameBoardTop = boardContentTop();

    await tester.tap(dropdown);
    await tester.pumpAndSettle();
    await tester.tap(find.text('3 + 2').last);
    await tester.pumpAndSettle();
    expect(ble.settingWrites, <(String, Object)>[("TIMER", 2)]);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":2,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
          '"cl":[1,300,300,"w",1,0,2,"n"]}',
    ]);
    await tester.pumpAndSettle();
    expect(boardContentTop(), pregameBoardTop);
    expect(
      find.text('Start a new game to change or turn off the clock.'),
      findsOneWidget,
    );
    expect(dropdown, findsNothing);
    expect(
      find.byKey(const ValueKey<String>('home-game-clock-static')),
      findsOneWidget,
    );
    expect(find.byIcon(Icons.lock_outline_rounded), findsOneWidget);
    final SemanticsNode lockedClock = tester.getSemantics(
      find.byKey(const ValueKey<String>('home-game-clock-static')),
    );
    expect(lockedClock.label, 'Time limit');
    expect(lockedClock.value, '3 + 2');
    expect(
      lockedClock.hint,
      'Start a new game to change or turn off the clock.',
    );
    expect(lockedClock.flagsCollection.isEnabled, Tristate.none);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":3,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"pm":0,"white":1}',
    ]);
    await tester.pumpAndSettle();
    expect(boardContentTop(), pregameBoardTop);
  });

  testWidgets('does not duplicate a clock write and explains rejection', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    final Completer<LitchSettingWriteResult> write =
        Completer<LitchSettingWriteResult>();
    ble.settingWriteCompleter = write;
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
          '"cl":[0,0,0,"w",0,0,0,"y"]}',
    ]);
    await tester.pumpAndSettle();

    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    final Finder dropdown = find.byType(DropdownButton<int>);
    await tester.tap(dropdown);
    await tester.pumpAndSettle();
    await tester.tap(find.text('5 min').last);
    await tester.pump();
    expect(ble.settingWrites, <(String, Object)>[("TIMER", 3)]);
    expect(dropdown, findsNothing);
    expect(find.byIcon(Icons.hourglass_top_rounded), findsOneWidget);
    final SemanticsNode pendingClock = tester.getSemantics(
      find.byKey(const ValueKey<String>('home-game-clock-static')),
    );
    expect(pendingClock.label, 'Time limit');
    expect(pendingClock.value, '5 min');
    expect(
      pendingClock.hint,
      'Ready. Starts when the first legal piece is lifted.',
    );
    expect(find.textContaining('Setting clock'), findsNothing);

    await tester.tap(
      find.byKey(const ValueKey<String>('home-game-clock')),
      warnIfMissed: false,
    );
    expect(ble.settingWrites, hasLength(1));
    write.complete(
      const LitchSettingWriteAcknowledged(
        LitchSettingOutcome(
          requestId: 1,
          key: 'TIMER',
          ok: false,
          reason: LitchMutationReason.busy,
          hasValue: false,
          isCompact: true,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(
      find.text(
        'The board is finishing another action. Try again when it’s ready.',
      ),
      findsOneWidget,
    );
    expect(dropdown, findsOneWidget);
    expect(tester.widget<DropdownButton<int>>(dropdown).value, 0);
  });

  testWidgets(
    'clock controls stay stable through a quiet write at 140 percent text',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      final Completer<LitchSettingWriteResult> write =
          Completer<LitchSettingWriteResult>();
      ble.settingWriteCompleter = write;
      addTearDown(ble.dispose);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));

      await tester.pumpWidget(
        _harness(ble, textScaler: const TextScaler.linear(1.4)),
      );
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
            '"cl":[0,0,0,"w",0,0,0,"y"]}',
      ]);
      await tester.pumpAndSettle();

      final Finder dropdown = find.byType(DropdownButton<int>);
      await tester.ensureVisible(dropdown);
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      final clock = find.byKey(const ValueKey('home-game-clock'));
      final Rect clockBefore = tester.getRect(clock);

      await tester.tap(dropdown);
      await tester.pumpAndSettle();
      await tester.tap(find.text('5 min').last);
      await tester.pumpAndSettle();

      expect(tester.takeException(), isNull);
      expect(find.textContaining('Setting clock'), findsNothing);
      expect(tester.getRect(clock), clockBefore);
      expect(find.byIcon(Icons.hourglass_top_rounded), findsOneWidget);

      write.complete(
        const LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 1,
            key: 'TIMER',
            ok: false,
            reason: LitchMutationReason.busy,
            hasValue: false,
            isCompact: true,
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(tester.takeException(), isNull);
      expect(tester.getRect(clock), clockBefore);
      expect(dropdown, findsOneWidget);
    },
  );

  testWidgets(
    'a start-only clock rejection resyncs and locks the stale selector',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final Completer<LitchSettingWriteResult> write =
          Completer<LitchSettingWriteResult>();
      ble.settingWriteCompleter = write;
      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
            '"cl":[0,0,0,"w",0,0,0,"y"]}',
      ]);
      await tester.pumpAndSettle();

      await tester.scrollUntilVisible(
        find.byKey(const ValueKey<String>('home-board-actions')),
        300,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byType(DropdownButton<int>));
      await tester.pumpAndSettle();
      await tester.tap(find.text('5 min').last);
      await tester.pump();

      final requestsBeforeRejection = ble.snapshotRequests;
      write.complete(
        const LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 1,
            key: 'TIMER',
            ok: false,
            reason: LitchMutationReason.notAtStart,
            hasValue: false,
            isCompact: true,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();

      expect(find.byType(DropdownButton<int>), findsNothing);
      expect(find.text('Syncing…'), findsOneWidget);
      expect(find.text('Unavailable'), findsNothing);
      expect(ble.snapshotRequests - requestsBeforeRejection, 1);
      expect(
        find.text(
          'The game has started, so the clock wasn’t changed. '
          'Start a new game to choose a clock.',
        ),
        findsOneWidget,
      );
      final SemanticsNode syncing = tester.getSemantics(
        find.byKey(const ValueKey<String>('home-game-clock-static')),
      );
      expect(syncing.flagsCollection.isEnabled, Tristate.none);

      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":2,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
            '"cl":[0,0,0,"b",0,0,0,"n"]}',
      ]);
      await tester.pumpAndSettle();

      expect(find.text('Off'), findsOneWidget);
      expect(find.text('Start a new game to choose a clock.'), findsOneWidget);
      expect(find.byType(DropdownButton<int>), findsNothing);
      final SemanticsNode locked = tester.getSemantics(
        find.byKey(const ValueKey<String>('home-game-clock-static')),
      );
      expect(locked.value, 'Off');
      expect(locked.flagsCollection.isEnabled, Tristate.none);
    },
  );

  testWidgets(
    'a missed start-only rejection recovery never restores the stale selector',
    (WidgetTester tester) async {
      final _FakeLitchBleService ble = _FakeLitchBleService()
        ..latestSettings['TIMER'] = 0;
      addTearDown(ble.dispose);
      final Completer<LitchSettingWriteResult> write =
          Completer<LitchSettingWriteResult>();
      ble.settingWriteCompleter = write;
      await tester.pumpWidget(_harness(ble));
      await tester.pump();
      ble.controller.add(const <String>[
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
            '"out":"n","win":"n","rot":0,"pm":0,"white":1,'
            '"cl":[0,0,0,"w",0,0,0,"y"]}',
      ]);
      await tester.pumpAndSettle();

      await tester.scrollUntilVisible(
        find.byKey(const ValueKey<String>('home-board-actions')),
        300,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byType(DropdownButton<int>));
      await tester.pumpAndSettle();
      await tester.tap(find.text('5 min').last);
      await tester.pump();
      final requestsBeforeRejection = ble.snapshotRequests;
      write.complete(
        const LitchSettingWriteAcknowledged(
          LitchSettingOutcome(
            requestId: 2,
            key: 'TIMER',
            ok: false,
            reason: LitchMutationReason.notAtStart,
            hasValue: false,
            isCompact: true,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      expect(find.text('Syncing…'), findsOneWidget);
      expect(find.byType(DropdownButton<int>), findsNothing);

      await tester.pump(const Duration(milliseconds: 350));
      await tester.pump();

      expect(ble.snapshotRequests - requestsBeforeRejection, 2);
      expect(find.text('Off'), findsNothing);
      expect(find.text('Couldn’t sync'), findsOneWidget);
      expect(
        find.text('The game can continue. Pull down to try again.'),
        findsOneWidget,
      );
      expect(find.byType(DropdownButton<int>), findsNothing);
      final SemanticsNode locked = tester.getSemantics(
        find.byKey(const ValueKey<String>('home-game-clock-static')),
      );
      expect(locked.value, 'Couldn’t sync');
      expect(locked.flagsCollection.isEnabled, Tristate.none);
    },
  );

  testWidgets('running clock Off becomes selectable after New Game', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":12,"a":"idle",'
          '"fen":"r1b5/pppk2p1/2n1q3/3p3P/1b1P3P/P4r2/1PP5/RN2K2R b - - 0 1",'
          '"occ":"9306218A88106F05","g":"d","duck":"f7",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"cl":[0,0,0,"b",0,0,0,"n"]}',
    ]);
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();

    expect(find.text('Off'), findsOneWidget);
    expect(find.text('Couldn’t sync'), findsNothing);
    expect(find.text('Unavailable'), findsNothing);
    expect(find.text('Start a new game to choose a clock.'), findsOneWidget);
    final SemanticsNode clock = tester.getSemantics(
      find.byKey(const ValueKey<String>('home-game-clock-static')),
    );
    expect(clock.value, 'Off');
    expect(clock.hint, 'Start a new game to choose a clock.');
    expect(clock.flagsCollection.isEnabled, Tristate.none);

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":13,"a":"idle",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"cl":[0,0,0,"w",0,0,0,"y"]}',
    ]);
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();

    expect(find.byType(DropdownButton<int>), findsOneWidget);
    expect(find.text('Set game time limit to enable clocks.'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('home-game-clock-static')),
      findsNothing,
    );
  });

  testWidgets('Duck timeout keeps winner when final pieces are handled', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    for (final (int sequence, String occupied) in <(int, String)>[
      (365, 'FFEF00108000FFFF'),
      (366, 'FFEF00008000FFFF'),
    ]) {
      ble.controller.add(<String>[
        '{"event":"home_state","v":1,"seq":$sequence,"a":"idle",'
            '"fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 1",'
            '"occ":"$occupied","g":"d","duck":"h5",'
            '"out":"n","win":"n","tw":"b",'
            '"rot":0,"white":1,"pm":0,'
            '"cl":[1,0,60,"w",0,0,1,"n"]}',
      ]);
      await tester.pumpAndSettle();
      expect(find.text('GAME COMPLETE'), findsOneWidget);
      expect(find.text('P2 wins on time'), findsOneWidget);
      expect(find.text('Place the duck'), findsNothing);
      expect(find.text('1 square to check'), findsNothing);
      expect(find.textContaining('expected on e4'), findsNothing);
      expect(find.textContaining('The board’s LEDs are guiding'), findsNothing);
      expect(
        find.text(
          'The physical board and app are following this move together.',
        ),
        findsNothing,
      );
      // Firmware's terminal FEN is retained; physical occupancy remains the
      // newest actual observation rather than being replaced with that FEN.
      expect(
        tester
            .widget<HomeChessboard>(find.byType(HomeChessboard))
            .snapshot
            .physicalStatus
            ?.occupiedSquares,
        BigInt.parse(occupied, radix: 16),
      );
    }
  });

  testWidgets('shows both clocks and terminal actions on timeout', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":9,"a":"idle",'
          '"fen":"8/8/8/8/8/8/8/K6k b - - 0 1",'
          '"occ":"8100000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","tw":"w",'
          '"rot":0,"white":1,"pm":0,'
          '"cl":[1,23,0,"b",0,0,3,"n"]}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('GAME COMPLETE'), findsOneWidget);
    expect(find.text('P1 wins on time'), findsOneWidget);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(find.text('0:23'), findsOneWidget);
    expect(find.text('0:00'), findsOneWidget);
    expect(find.byIcon(Icons.play_arrow_rounded), findsNothing);
    expect(find.byIcon(Icons.pause_rounded), findsNothing);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      400,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<OutlinedButton>(
            find.byKey(const ValueKey<String>('control:hint')),
          )
          .onPressed,
      isNull,
    );
    expect(
      tester
          .widget<OutlinedButton>(
            find.byKey(const ValueKey<String>('control:undo')),
          )
          .onPressed,
      isNull,
    );
    expect(
      tester
          .widget<OutlinedButton>(
            find.byKey(const ValueKey<String>('control:new-game')),
          )
          .onPressed,
      isNotNull,
    );
  });

  testWidgets('shows the latest board-reported moves without inventing SAN', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":6,"a":"idle",'
          '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
          '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"m":"e2e4e7e5b"}',
    ]);
    await tester.pumpAndSettle();

    expect(
      find.byKey(const ValueKey<String>('last-move-white-from-e2')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('last-move-black-to-e5')),
      findsOneWidget,
    );

    await tester.scrollUntilVisible(
      find.text('Latest board-reported moves'),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(find.text('Latest board-reported moves'), findsOneWidget);
    expect(find.text('e2 → e4'), findsOneWidget);
    expect(find.text('e7 → e5'), findsOneWidget);
    expect(find.text('P2 · LATEST'), findsOneWidget);
  });

  testWidgets('keeps an older coordinate-less computer prompt honest', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":5,"a":"cpu",'
          '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
          '"occ":"8100000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('Computer move ready'), findsOneWidget);
    expect(
      find.text('Follow the board’s lights to make the computer’s move.'),
      findsOneWidget,
    );
  });

  testWidgets('keeps correction guidance usable at 200 percent text', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(
      _harness(ble, textScaler: const TextScaler.linear(2)),
    );
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"corr",'
          '"fen":"8/8/8/8/8/3B4/8/6Kk w - - 0 1",'
          '"occ":"c000000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"cl":[1,300,300,"w",1,0,3,"n"]}',
    ]);
    await tester.pumpAndSettle();

    expect(find.text('POSITION NEEDS CORRECTION'), findsOneWidget);
    expect(find.textContaining('P1 bishop expected on d3'), findsOneWidget);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('home-game-clock')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('home-clock-strip-large-text')),
      findsOneWidget,
    );
    expect(
      tester
          .getTopLeft(find.byKey(const ValueKey<String>('home-game-clock')))
          .dy,
      greaterThan(
        tester
            .getBottomLeft(find.byKey(const ValueKey<String>('home-clock-p2')))
            .dy,
      ),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('Fresh Setup king lift never announces a king and queen swap', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    String state(int sequence, String occupancy) =>
        '{"event":"home_state","v":1,"seq":$sequence,"a":"${occupancy == 'EFFF00000000FFFF' ? 'corr' : 'idle'}",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"$occupancy","g":"u","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1}';
    int sequence = 8;
    for (final occupancy in <String>[
      'FFFF00000000FFFF', // King flashing at the untouched opening.
      'EFFF00000000FFFF', // Physical P1 king lifted from e1.
      'FFFF00000000FFFF', // Replace the king and recover.
    ]) {
      ble.controller.add(<String>[state(sequence++, occupancy)]);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Swap the king and queen'), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('king-queen-swap-shuttle')),
        findsNothing,
      );
      expect(find.byType(SnackBar), findsNothing);
    }
  });

  testWidgets('treats a guided king and queen swap as setup, not an error', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    final SemanticsHandle semantics = tester.ensureSemantics();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble));
    await tester.pump();

    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":8,"a":"corr",'
          '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
          '"occ":"FFFF00000000FFEF","g":"u","duck":"--",'
          '"out":"n","win":"n","rot":1,"white":1,"pm":0,"kq":1,'
          '"cl":[1,600,600,"w",0,0,4,"b"]}',
    ]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('BOARD SETUP'), findsOneWidget);
    expect(find.text('Swap the king and queen'), findsOneWidget);
    expect(
      find.text(
        'Follow the moving board lights to exchange the king and queen on both sides.',
      ),
      findsOneWidget,
    );
    expect(
      find.text(
        'Swap the king and queen on both sides. Follow the moving board lights.',
      ),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('king-queen-swap-shuttle')),
      findsOneWidget,
    );
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('home-guidance-live-region')),
      ),
      isSemantics(isLiveRegion: false),
    );
    expect(
      tester.getSemantics(
        find.text(
          'Swap the king and queen on both sides. Follow the moving board lights.',
        ),
      ),
      isSemantics(isLiveRegion: true),
    );
    expect(find.text('POSITION NEEDS CORRECTION'), findsNothing);
    expect(find.textContaining('expected on'), findsNothing);
    semantics.dispose();
  });

  testWidgets('announces each king and queen swap prompt only once', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    final GlobalKey<ScaffoldMessengerState> messengerKey =
        GlobalKey<ScaffoldMessengerState>();
    addTearDown(ble.dispose);
    await tester.pumpWidget(_harness(ble, scaffoldMessengerKey: messengerKey));
    await tester.pump();

    String state(int sequence, String action, String occupancy) =>
        '{"event":"home_state","v":1,"seq":$sequence,"a":"$action",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"$occupancy",'
        '"g":"u","duck":"--","out":"n","win":"n","rot":1,'
        '"white":1,"pm":0,"kq":${action == 'corr' ? 1 : 0}}';
    const String toast =
        'Swap the king and queen on both sides. Follow the moving board lights.';

    ble.controller.add(<String>[state(8, 'corr', 'FFFF00000000FFFF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byType(SnackBar), findsOneWidget);
    expect(find.text(toast), findsOneWidget);
    expect(
      tester.widget<SnackBar>(find.byType(SnackBar)).duration,
      const Duration(seconds: 6),
    );

    messengerKey.currentState!.removeCurrentSnackBar();
    await tester.pump();
    expect(find.text(toast), findsNothing);
    ble.controller.add(<String>[state(9, 'corr', 'FFFF00000000FFEF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text(toast), findsNothing);

    ble.controller.add(<String>[state(10, 'idle', 'FFFF00000000FFFF')]);
    await tester.pump();
    ble.controller.add(<String>[state(11, 'corr', 'FFFF00000000FFEF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text(toast), findsOneWidget);
  });

  testWidgets('reconnect does not repeat an active king and queen prompt', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    String state(int sequence, String action, String occupancy) =>
        '{"event":"home_state","v":1,"seq":$sequence,"a":"$action",'
        '"fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",'
        '"occ":"$occupancy","g":"u","duck":"--",'
        '"out":"n","win":"n","rot":1,"white":1,"pm":0,"kq":${action == 'corr' ? 1 : 0}}';
    const String toast =
        'Swap the king and queen on both sides. Follow the moving board lights.';

    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(<String>[state(8, 'corr', 'FFFF00000000FFFF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text(toast), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    ble.controller.latestHomeState = null;
    await tester.pumpWidget(_harness(ble));
    await tester.pump();
    ble.controller.add(<String>[state(1, 'corr', 'FFFF00000000FFEF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text(toast), findsNothing);

    ble.controller.add(<String>[state(2, 'idle', 'FFFF00000000FFFF')]);
    await tester.pump();
    ble.controller.add(<String>[state(3, 'corr', 'FFFF00000000FFEF')]);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text(toast), findsOneWidget);
  });

  testWidgets('shows complete failed clock sync at 200 percent text', (
    WidgetTester tester,
  ) async {
    final _FakeLitchBleService ble = _FakeLitchBleService();
    addTearDown(ble.dispose);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(
      _harness(ble, textScaler: const TextScaler.linear(2)),
    );
    await tester.pump();
    ble.controller.add(const <String>[
      '{"event":"home_state","v":1,"seq":9,"a":"corr",'
          '"fen":"8/8/8/8/8/3B4/8/6Kk w - - 0 1",'
          '"occ":"c000000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    ]);
    await tester.pumpAndSettle();
    await tester.pump(const Duration(milliseconds: 350));
    await tester.pumpAndSettle();

    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('home-board-actions')),
      300,
      scrollable: _homeDetailsScrollable(),
    );
    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('home-game-clock')),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('home-clock-strip-large-text')),
      findsOneWidget,
    );
    expect(find.text('—'), findsNWidgets(2));
    expect(find.text('Couldn’t sync'), findsOneWidget);
    expect(find.byIcon(Icons.info_outline_rounded), findsOneWidget);
    expect(
      find.text('The game can continue. Pull down to try again.'),
      findsOneWidget,
    );
    final Text helper = tester.widget<Text>(
      find.text('The game can continue. Pull down to try again.'),
    );
    expect(helper.maxLines, isNull);
    expect(helper.overflow, TextOverflow.visible);
    expect(
      find.byKey(const ValueKey<String>('home-clock-board-health')),
      findsNothing,
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('home-clock-strip')))
          .label,
      'Game clock could not sync',
    );
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('home-game-clock-static')),
      ),
      isSemantics(
        label: 'Time limit',
        value: 'Couldn’t sync',
        hint: 'The game can continue. Pull down to try again.',
        isLiveRegion: true,
      ),
    );
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey<String>('home-game-clock-static')),
          )
          .flagsCollection
          .isEnabled,
      Tristate.none,
    );
    expect(tester.takeException(), isNull);
  });
}

// Registered in localization_screens_test.dart so SDK fonts cannot alter
// the existing Ahem goldens or interaction fixtures in this file.
void registerHomeLocalizationTests() {
  testWidgets(
    'CPU thinking phone review covers unknown, ETA, correction and ready',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      for (final dark in [false, true]) {
        for (final scale in [1.0, 2.0]) {
          final ble = _FakeLitchBleService()
            ..latestSettings.addAll({
              'VS_CPU_LEVEL': 6,
              'VS_CPU_PLAYER': 1,
              'P1_PIECE_WHEEL': 170,
              'P2_PIECE_WHEEL': 0,
            });
          addTearDown(ble.dispose);
          final name =
              'cpu-thinking-${dark ? 'dark' : 'light'}-${scale.toInt()}x';
          await tester.pumpWidget(
            _harness(ble, textScaler: TextScaler.linear(scale), dark: dark),
          );
          ble.controller.add([_cpuState(1, 'progress', ',"th":["w",0,null]')]);
          await tester.pumpAndSettle();
          await decodeVisibleReviewImages(tester);
          expectReviewTextFits(
            tester,
            find.text('CPU is thinking'),
            'Complete thinking title at phone width',
          );
          if (scale == 1) {
            expect(
              tester
                  .getTopRight(
                    find.byKey(const ValueKey<String>('cpu-thinking-robot')),
                  )
                  .dx,
              lessThan(tester.getTopLeft(find.text('CPU is thinking')).dx),
              reason:
                  'Normal phone text retains the compact side-by-side robot.',
            );
          }
          await captureHealthReview(tester, '$name-unknown');

          ble.controller.add([_cpuState(2, 'progress', ',"th":["w",2,8]')]);
          await tester.pumpAndSettle();
          expectReviewTextFits(
            tester,
            find.text('About 8 seconds left'),
            'Complete estimate at phone width',
          );
          await captureHealthReview(tester, '$name-estimate');

          ble.controller.add([
            _cpuState(
              3,
              'corr',
              ',"th":["w",4,7]',
            ).replaceFirst('FFFF00000000FFFF', 'FFEF00000000FFFF'),
          ]);
          await tester.pumpAndSettle();
          expect(find.text('CPU is thinking'), findsNothing);
          expect(find.text('Follow the board’s guidance'), findsOneWidget);
          await captureHealthReview(tester, '$name-correction');

          ble.controller.add([
            _cpuState(
              4,
              'cpu',
              ',"c":["e2","e4","--"]',
            ).replaceFirst('"w",1,0,3,"n"', '"w",1,1,3,"n"'),
          ]);
          await tester.pumpAndSettle();
          expect(find.text('CPU is thinking'), findsNothing);
          expect(
            find.text('Move the computer piece from e2 to e4.'),
            findsOneWidget,
          );
          await captureHealthReview(tester, '$name-ready');
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump();
        }
      }
    },
  );

  for (final locale in localizationReviewLocales) {
    testWidgets('CPU thinking wraps at 200 percent in $locale', (tester) async {
      await prepareLocalizationReview(tester);
      tester.view.physicalSize = const Size(320, 700);
      final ble = _FakeLitchBleService();
      addTearDown(ble.dispose);
      final strings = reviewStrings(locale);
      await tester.pumpWidget(
        localizationHarness(
          locale: locale,
          scale: 2,
          home: Scaffold(
            body: HomeScreen(bleService: ble, boardId: 'board-12345'),
          ),
        ),
      );
      ble.controller.add([_cpuState(1, 'progress', ',"th":["w",2,8]')]);
      await tester.pumpAndSettle();
      expectReviewTextFits(
        tester,
        find.text(strings.cpuIsThinking),
        'Complete thinking title',
      );
      if (locale.languageCode == 'de' || locale.languageCode == 'fr') {
        final firstWord = strings.cpuIsThinking.split(' ').first;
        final title = tester.renderObject<RenderParagraph>(
          find.descendant(
            of: find.text(strings.cpuIsThinking),
            matching: find.byType(RichText),
          ),
        );
        expect(
          title.getBoxesForSelection(
            TextSelection(baseOffset: 0, extentOffset: firstWord.length),
          ),
          hasLength(1),
          reason: 'Keep $firstWord whole at narrow 200 percent text.',
        );
      }
      expectReviewTextFits(
        tester,
        find.text(strings.cpuThinkingRemaining(8)),
        'Complete approximate estimate',
      );
      await decodeVisibleReviewImages(tester);
      await captureHealthReview(
        tester,
        'cpu-thinking-${locale.toLanguageTag()}-320-200',
      );
      await tester.scrollUntilVisible(
        find.byKey(const ValueKey<String>('home-board-actions')),
        300,
        scrollable: _homeDetailsScrollable(),
      );
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  for (final locale in localizationReviewLocales) {
    testWidgets(
      'localization Home correction preserves player and square $locale',
      (tester) async {
        await prepareLocalizationReview(tester);
        final strings = reviewStrings(locale);
        final ble = _FakeLitchBleService();
        addTearDown(ble.dispose);
        await tester.pumpWidget(
          localizationHarness(
            locale: locale,
            scale: 2,
            home: Scaffold(
              body: HomeScreen(bleService: ble, boardId: 'board-12345'),
            ),
          ),
        );
        ble.controller.add(const <String>[
          '{"event":"home_state","v":1,"seq":1,"a":"corr","fen":"8/8/8/8/8/3B4/8/6Kk w - - 0 1","occ":"c000000000000000","g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0}',
        ]);
        // Correction now has a continuous pulse; localization needs only the
        // next coherent screen frame, not animation completion.
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.textContaining(strings.playerBishop('P1')), findsOneWidget);
        expect(find.textContaining('d3'), findsWidgets);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }
  testWidgets('localization owner regression: Home follows German', (
    tester,
  ) async {
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
          body: HomeScreen(bleService: ble, boardId: 'board-12345'),
        ),
      ),
    );
    ble.controller.add(<String>[
      '{"event":"home_state","v":1,"seq":1,"a":"idle","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"FFFF00000000FFFF","g":"u","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1,"up":0,"cl":[0,0,0,"w",0,0,0,"y"]}',
    ]);
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'localization-home-before');
    expect(find.text('P1 to move'), findsNothing);
    expect(find.text('P1 am Zug'), findsOneWidget);
    expect(find.textContaining('Classic or Duck'), findsNothing);
    expect(
      find.text('Klassisch oder Entenschach (Duck) · Zug 1'),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
