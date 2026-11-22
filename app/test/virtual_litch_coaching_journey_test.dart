import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/localization/litch_strings.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/controllers/board_actions_controller.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';
import 'virtual_litch/board_led_expectations.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const String boardId = 'virtual-litch-production';
final BigInt opening = BigInt.parse('FFFF00000000FFFF', radix: 16);

Finder coaching(int owner) => find.byKey(ValueKey('control:coaching:p$owner'));

OutlinedButton coachingButton(WidgetTester tester, Finder control) =>
    tester.widget<OutlinedButton>(control);

void expectCoaching(
  WidgetTester tester,
  int owner,
  bool? value, {
  bool enabled = true,
}) {
  final control = coaching(owner);
  final toggle = coachingButton(tester, control);
  final changeable = enabled && value != null;
  expect(toggle.onPressed, changeable ? isNotNull : isNull);
  final node = tester.getSemantics(control);
  expect(
    node.flagsCollection.isToggled,
    value == null
        ? ui.Tristate.none
        : value
        ? ui.Tristate.isTrue
        : ui.Tristate.isFalse,
  );
  expect(
    node.flagsCollection.isEnabled,
    changeable ? ui.Tristate.isTrue : ui.Tristate.isFalse,
  );
  expect(node.getSemanticsData().hasAction(ui.SemanticsAction.tap), changeable);
  final strings = litchStrings(tester.element(control));
  expect(
    find.descendant(
      of: control,
      matching: find.text(
        value == null
            ? strings.unknownLabel
            : value
            ? strings.onLabel
            : strings.offLabel,
      ),
    ),
    findsOneWidget,
  );
  if (value == null) {
    expect(
      find.descendant(of: control, matching: find.text(strings.unknownLabel)),
      findsOneWidget,
    );
    expect(node.label, strings.coachingUnknown('$owner'));
  } else if (enabled) {
    expect(node.label, strings.coachingSemantics('$owner', value.toString()));
  }
  final size = tester.getSize(control);
  expect(size.width, greaterThanOrEqualTo(48));
  expect(size.height, greaterThanOrEqualTo(48));
}

Future<void> checkpoint(
  WidgetTester tester,
  VirtualLitchBle ble,
  LitchBleService service,
  String name,
  int mask,
  bool squareRotate, {
  int? retainedAppMask,
  bool cpuDirected = false,
}) async {
  final frame = await publishedBoardFrame(tester, ble);
  expect(service.latestHomeState?.coachingMask, retainedAppMask ?? mask);
  final expectedBadges = <bool>[
    (mask & (squareRotate ? 2 : 1)) != 0,
    (mask & (squareRotate ? 1 : 2)) != 0,
  ];
  expect(frame['physical_coaching_badges'], expectedBadges);
  expectOrdinaryEmptyCenters(ble, frame, name, cpuDirected: cpuDirected);
  final String? output = Platform.environment['LITCH_VIRTUAL_EVIDENCE'];
  if (output != null) {
    await tester.runAsync(() async {
      final file = File('$output/$name-leds.json');
      await file.parent.create(recursive: true);
      await file.writeAsString(
        const JsonEncoder.withIndent('  ').convert(frame),
      );
    });
  }
  await captureHealthReview(tester, name);
  debugPrint(
    'VIRTUAL_LITCH $name native=${frame['now_ms']}ms '
    'flutter=${tester.binding.clock.now().toIso8601String()} '
    'Home seq=${service.latestHomeState?.sequence} co=${service.latestHomeState?.coachingMask} '
    'occupancy=${frame['occupancy']} action=${frame['required_action']} '
    'display="${frame['display']}" physical P1/P2 badges=$expectedBadges; '
    'next action: use the enabled Home control or follow the board square pairs.',
  );
}

Future<void> tapCoaching(WidgetTester tester, int owner) async {
  await tester.ensureVisible(coaching(owner));
  await tester.pump();
  await tester.tap(coaching(owner));
  await tester.pump();
}

Future<void> followCoachedDuckHint(
  WidgetTester tester,
  VirtualLitchBle ble,
  LitchBleService service,
  BigInt occupancy,
) async {
  expect(ble.nativeSessionActivated, isTrue);
  expect(service.deviceCapabilities(boardId).supportsConnectionSession, isTrue);
  final chessboard = find.byType(HomeChessboard);
  final strings = litchStrings(tester.element(chessboard));
  HomeBoardSnapshot shown() =>
      tester.widget<HomeChessboard>(chessboard).snapshot;
  Future<void> until(bool Function() ready, String reason) async {
    for (var elapsed = 0; elapsed < 4000 && !ready(); elapsed += 100) {
      await ble.advance(tester, 100);
    }
    expect(ready(), isTrue, reason: reason);
  }

  Future<void> hall() async {
    // In this existing pose, first player at P2 and board rotation cancel.
    await ble.command(tester, {
      'op': 'hall',
      'mask': occupancy.toRadixString(16).padLeft(16, '0'),
    });
  }

  Future<void> circle() async {
    for (final down in [true, false]) {
      await ble.command(tester, {
        'op': 'button',
        'owner': 1,
        'button': 6,
        'down': down,
      });
      await ble.advance(tester, 20);
    }
  }

  await until(
    () => shown().gameStatus?.variant == 'duck',
    'Binary Home sees Duck.',
  );
  LitchSettingWriteResult? ack;
  unawaited(
    service
        .writeSettingAcknowledged(
          deviceId: boardId,
          key: 'MOVE_CONFIRM',
          value: 'press_circle_to_confirm',
        )
        .then((result) => ack = result),
  );
  await until(() => ack != null, 'The board acknowledges Circle confirmation.');
  expect(ack, isA<LitchSettingWriteAcknowledged>());
  expect((ack as LitchSettingWriteAcknowledged).outcome.ok, isTrue);

  await tapCoaching(tester, 1);
  await tester.ensureVisible(
    find.byKey(const ValueKey('home-game-surface'), skipOffstage: false),
  );
  await tester.pump();
  await until(
    () =>
        shown().visibleAssistance?.presentation ==
        HomeAssistancePresentation.compoundDuck,
    'Coaching reveals one compound Hint from the real Duck search.',
  );
  final compound = shown().visibleAssistance!;
  expect(compound.coachingOwned, isTrue);
  final move = compound.move!;
  final duckMove = compound.duckMove!;
  final duckDestination = find.bySemanticsLabel(
    RegExp(RegExp.escape(strings.duckHintDestination)),
  );
  final chessSource = find.bySemanticsLabel(
    RegExp(RegExp.escape(strings.hintStartsHere)),
  );
  final duckMarker = find.byKey(
    ValueKey('duck-assistance-to-${algebraicSquare(duckMove.to)}'),
  );
  expect(duckDestination, findsOneWidget);

  occupancy &= ~(BigInt.one << move.from);
  await hall();
  await until(
    () =>
        shown().visibleAssistance?.presentation ==
        HomeAssistancePresentation.legal,
    'Pickup keeps the normal legal guide.',
  );
  final held = shown().visibleAssistance!;
  expect(held.move, move);
  expect(held.duckMove, duckMove);
  final native = await ble.frame(tester);
  expect(
    held.legalDestinations,
    BigInt.parse(native['destinations'] as String, radix: 16),
  );
  expect(duckDestination, findsOneWidget);

  final alternate = Iterable<int>.generate(64).firstWhere(
    (square) =>
        square != move.to &&
        (held.legalDestinations! & (BigInt.one << square)) != BigInt.zero,
  );
  expect(duckMarker, findsOneWidget);
  expect(
    find.byKey(ValueKey('assistance-to-${algebraicSquare(alternate)}')),
    findsOneWidget,
  );
  final alternateLanding = BigInt.one << alternate;
  final chessLanding = BigInt.one << move.to;
  final duckSource = BigInt.one << duckMove.from;
  occupancy &= ~duckSource;
  await hall();
  await until(
    () => shown().physicalStatus?.occupiedSquares == occupancy,
    'The Duck is lifted after the chess piece.',
  );
  occupancy |= alternateLanding | chessLanding;
  await hall();
  await until(
    () => shown().isDuckLandingCorrection,
    'Shared Home reports both ambiguous landings as removal guidance.',
  );
  expect(shown().guidedSquares, {move.to, alternate});
  expect(find.text(strings.removePieces(2)), findsOneWidget);
  for (final square in [move.to, alternate]) {
    final name = algebraicSquare(square);
    expect(
      find.bySemanticsLabel(strings.removeCapturePiece(name)),
      findsOneWidget,
    );
  }
  expect(duckDestination, findsNothing);
  expect(duckMarker, findsNothing);

  occupancy &= ~alternateLanding;
  await hall();
  await until(
    () => shown().duckLandingRemovalMask == chessLanding,
    'Removing one ambiguous landing leaves the other marked.',
  );
  expect(find.text(strings.removePieces(1)), findsOneWidget);
  occupancy &= ~chessLanding;
  await hall();
  await until(
    () =>
        shown().visibleAssistance?.presentation ==
        HomeAssistancePresentation.legal,
    'Lifting both ambiguous placements restores the held guide.',
  );
  expect(shown().duckLandingRemovalMask, isNull);
  expect(shown().visibleAssistance?.duckMove, duckMove);
  occupancy |= duckSource;
  await hall();

  occupancy |= BigInt.one << move.to;
  await hall();
  await until(
    () => shown().homeStateAction == HomeStateAction.settle,
    'Chess placement waits for physical Circle.',
  );
  expect(shown().visibleAssistance?.duckMove, duckMove);
  expect(shown().visibleAssistance?.move, isNull);
  expect(duckDestination, findsOneWidget);
  expect(duckMarker, findsOneWidget);
  expect(chessSource, findsNothing);

  await circle();
  await until(
    () => shown().visibleDuckRelocation?.owner == HomeDuckRelocationOwner.human,
    'The binary handoff keeps the human relocation selectable.',
  );
  expect(shown().visibleAssistance?.duckMove, duckMove);
  expect(shown().visibleAssistance?.move, isNull);
  expect(duckDestination, findsOneWidget);
  debugPrint(
    'VIRTUAL_LITCH modern Duck Hint $move then $duckMove stayed visible '
    'through legal pickup, ambiguity recovery, Circle settle and relocation; '
    'Console dropped.',
  );
}

Map<String, Object?> boardActionAppearance(WidgetTester tester) {
  final result = <String, Object?>{
    'cardBounds': tester.getRect(
      find.byKey(const ValueKey('home-board-actions')),
    ),
  };
  for (final key in [
    'control:hint',
    'control:undo',
    'control:new-game',
    'control:coaching:p1',
    'control:coaching:p2',
  ]) {
    final control = find.byKey(ValueKey(key));
    final isCoaching = key.startsWith('control:coaching:');
    result[key] = <String, Object?>{
      'enabled': isCoaching
          ? coachingButton(tester, control).onPressed != null
          : tester.widget<OutlinedButton>(control).onPressed != null,
      'bounds': tester.getRect(control),
      'labels': tester
          .widgetList<Text>(
            find.descendant(of: control, matching: find.byType(Text)),
          )
          .map((text) => [text.data, text.style])
          .toList(),
      if (isCoaching)
        'toggled': tester.getSemantics(control).flagsCollection.isToggled
      else
        'selected': tester.getSemantics(control).flagsCollection.isSelected,
    };
  }
  return result;
}

List<int> _coachingBody(int owner, bool enabled) =>
    LitchControlProtocol.encodeConnectionAction(
      action: LitchControlAction.coaching,
      owner: owner == 1 ? LitchControlOwner.p1 : LitchControlOwner.p2,
      enabled: enabled,
    );

List<VirtualPacket> _heldHomeWithMask(VirtualLitchBle ble, int mask) {
  final assembler = HomeStateWireAssembler();
  final fragments = <int, VirtualPacket>{};
  int? revision;
  for (final wire in ble.held) {
    final packet = wire.sessionPacket;
    if (packet == null || !packet.isHomeState) continue;
    if (revision != packet.sequence) {
      revision = packet.sequence;
      fragments.clear();
    }
    fragments[packet.code] = wire;
    final home = assembler.accept(
      code: packet.code,
      revision: packet.sequence,
      body: packet.body,
      sequence: 1,
    );
    if (home?.coachingMask == mask) return fragments.values.toList();
  }
  return [];
}

Future<void> tapCoachingWithStableActions(
  WidgetTester tester,
  VirtualLitchBle ble,
  int owner,
) async {
  await tester.ensureVisible(coaching(owner));
  await tester.pumpAndSettle();
  final before = boardActionAppearance(tester);
  // Delay real firmware ACK/Home delivery, preserving the production request
  // and rendering path while checking the transition a person actually sees.
  ble.holdSessionPacket = (packet) => packet.isHomeState || packet.isCheckpoint;
  try {
    await tester.tap(coaching(owner));
    await tester.pump();
    await ble.flush(tester);
    final request = ble.actionWrites.last;
    expect(request.body, _coachingBody(owner, true));
    await ble.advanceUntil(
      tester,
      () => ble.held.any(
        (wire) => wire.sessionPacket?.confirmsApplied(request) == true,
      ),
      'the board’s Applied checkpoint is held before pending-frame checks',
    );
    for (final elapsed in [0, 16, 80, 180]) {
      await tester.pump(Duration(milliseconds: elapsed));
      expect(
        boardActionAppearance(tester),
        before,
        reason:
            'P$owner pending communication must not dim or move any Board Actions control.',
      );
    }
  } finally {
    ble.holdSessionPacket = null;
    ble.release('connection_session');
  }
}

Future<void> fragmentedHomeAcrossAck(
  WidgetTester tester,
  VirtualLitchBle ble,
  LitchBleService service,
  int owner,
  int expectedMask,
) async {
  await tester.ensureVisible(coaching(owner));
  await tester.pump();
  final card = find.byKey(const ValueKey('home-board-actions'));
  final bounds = [
    tester.getRect(card),
    tester.getRect(coaching(1)),
    tester.getRect(coaching(2)),
  ];
  void expectStable(String phase) {
    expect(
      [
        tester.getRect(card),
        tester.getRect(coaching(1)),
        tester.getRect(coaching(2)),
      ],
      bounds,
      reason: 'P$owner normal Coaching $phase must not move the controls.',
    );
    expect(
      find.byType(SnackBar),
      findsNothing,
      reason: 'Coaching has no toast.',
    );
  }

  ble.holdSessionPacket = (packet) => packet.isHomeState || packet.isCheckpoint;
  final pointer = await tester.startGesture(tester.getCenter(coaching(owner)));
  await tester.pump(const Duration(milliseconds: 16));
  expectStable('pointer held');
  await pointer.up();
  await tester.pump();
  await captureHealthReview(tester, 'coaching-ios-normal-p$owner-pending');
  expectStable('pending');
  await ble.flush(tester);
  final command = ble.actionWrites.last;
  expect(command.body, _coachingBody(owner, true));
  List<VirtualPacket> homePackets = [];
  await ble.advanceUntil(tester, () {
    homePackets = _heldHomeWithMask(ble, expectedMask);
    return homePackets.isNotEmpty &&
        ble.held.any(
          (wire) => wire.sessionPacket?.confirmsApplied(command) == true,
        );
  }, 'the original Home publication and its Applied checkpoint are held');
  final first = homePackets.first;
  expect(first.sessionPacket!.body.length, HomeStateWireCodec.fragmentBytes);
  expect(homePackets.length, HomeStateWireCodec.fragmentCount);
  ble.held.remove(first);
  ble.deliver(first);
  await tester.pump();
  expectCoaching(tester, owner, false);
  expectStable('partial Home');

  // The real terminal ACK falls between actual Home notification fragments.
  // The subsequent explicit readback is lost. The in-flight coherent Home
  // report must still complete on its healthy SDK subscription.
  ble.dropSessionPacket = (packet) => packet.isHomeState;
  ble.releaseWhere(
    (wire) => wire.sessionPacket?.confirmsApplied(command) == true,
  );
  await tester.pump();
  await ble.advance(tester, 50);
  expectStable('ACK before complete Home');
  for (final wire in homePackets.skip(1)) {
    ble.held.remove(wire);
    ble.deliver(wire);
  }
  ble.holdSessionPacket = null;
  ble.held.removeWhere((wire) => wire.sessionPacket?.isHomeState == true);
  await tester.pump();
  await tester.pump();
  expectCoaching(tester, owner, true);
  expect(service.latestHomeState?.coachingMask, expectedMask);
  expectStable('confirmed Home');
  await ble.advance(tester, 3700);
  expectStable('past the former toast and readback expiry');
  expectCoaching(tester, owner, true);
  expect(find.textContaining('Coaching state is unavailable'), findsNothing);
  expect(
    find.text('P$owner coaching on. Watch the board lights.'),
    findsNothing,
  );
  ble.dropSessionPacket = null;
  debugPrint(
    'VIRTUAL_LITCH race P$owner action=${command.body} '
    'Home fragments=${homePackets.length}; prefix, outcome, original remainder; '
    'readback lost; saved On remains.',
  );
}

void main() {
  testWidgets(
    'setup Coaching and Hint use real board authority through hydration',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false),
      ))!;
      final ble = VirtualLitchGattBle(native);
      final service = ble.createService();
      addTearDown(() => ble.disposeJourney(tester, service));
      Future<void> until(bool Function() ready, String reason) async {
        for (var elapsed = 0; !ready() && elapsed < 3000; elapsed += 10) {
          await ble.advance(tester, 10);
        }
        expect(ready(), isTrue, reason: reason);
      }

      await ble.command(tester, {
        'op': 'hall',
        'mask': opening.toRadixString(16),
      });
      await ble.advance(tester, 200);
      final connection = service.connect(boardId);
      await ble.flush(tester);
      expect(await connection, isTrue);
      await until(
        () => service.latestHomeState?.coachingMask == 0,
        'the connected board must publish its initial saved preferences',
      );
      await ble.command(tester, {
        'op': 'button',
        'owner': 1,
        'button': 6,
        'down': true,
      });
      await ble.advance(tester, 1100);
      await ble.command(tester, {
        'op': 'button',
        'owner': 1,
        'button': 6,
        'down': false,
      });
      await until(
        () => service.latestHomeState?.coachingMask == 1,
        'a raw human O hold must establish the saved preference before setup',
      );
      await ble.command(tester, {
        'op': 'hall',
        'mask': (opening & ~(BigInt.one << 52)).toRadixString(16),
      });
      var newGameCompleted = false;
      final newGame = service
          .dispatchControlAction(
            deviceId: boardId,
            action: LitchControlAction.newGame,
          )
          .then((event) {
            newGameCompleted = true;
            return event;
          });
      await until(
        () => newGameCompleted,
        'the existing action owner must finish New Game while native time advances',
      );
      await newGame;
      await until(
        () => service.latestHomeState?.physicalStatus.placementMode == true,
        'the real New Game owner must publish incomplete setup',
      );

      // Use the actual cache-invalidation seam used by ambiguous Hint recovery.
      // No Home payload, app flag, engine position or physical state is injected.
      service.discardCachedHomeAssistance(
        boardId: boardId,
        sequence: service.latestHomeState!.sequence,
      );
      expect(service.latestHomeState!.assistance, isNull);
      final controller = BoardActionsController(
        bleService: service,
        boardId: boardId,
      );
      try {
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: Scaffold(body: HomeBoardActions(controller: controller)),
          ),
        );
        await ble.flush(tester);
        final hint = find.byKey(const ValueKey('control:hint'));
        expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
        final writes = ble.actionWrites.length;
        await controller.dispatch(LitchControlAction.hint);
        await ble.flush(tester);
        expect(
          ble.actionWrites.length,
          writes,
          reason: 'the disabled setup action must not submit a Hint request',
        );
        await until(
          () =>
              controller.coachingEnabled(LitchControlOwner.p1) == true &&
              controller.canDispatchAction(
                LitchControlAction.coaching,
                owner: LitchControlOwner.p1,
              ),
          'saved P1 preference and human role must hydrate from the actual service',
        );
        expectCoaching(tester, 1, true);
        await ble.command(tester, {
          'op': 'button',
          'owner': 1,
          'button': 6,
          'down': true,
        });
        await ble.advance(tester, 1100);
        await ble.command(tester, {
          'op': 'button',
          'owner': 1,
          'button': 6,
          'down': false,
        });
        await until(
          () => controller.coachingEnabled(LitchControlOwner.p1) == false,
          'raw setup O hold must update the same saved preference shown by Play',
        );
        expectCoaching(tester, 1, false);
        expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
        expect(service.latestHomeState!.physicalStatus.placementMode, isTrue);
        expect(service.latestHomeState!.assistance?.requested, isNot(true));
        await ble.command(tester, {
          'op': 'hall',
          'mask': opening.toRadixString(16),
        });
        await until(
          () => service.latestHomeState?.assistance?.manualAvailable == true,
          'completed stable physical setup must enable Hint through board authority',
        );
        expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
        debugPrint(
          'SETUP_COACHING_APP input=raw-Hall/O+real-NewGame '
          'hydration=actual-cache-invalidation saved=${service.latestHomeState!.coachingMask} '
          'placement=${service.latestHomeState!.physicalStatus.placementMode} '
          'Hint=${service.latestHomeState!.assistance!.manualAvailable} '
          'next=play-or-request-Hint; no premature Hint write',
        );
      } finally {
        controller.dispose();
        final disconnect = service.disconnect();
        await ble.flush(tester);
        await disconnect;
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  test('empty-center guard admits only visible knight Piece legs', () {
    HomeStateNotification home(
      String move, {
      String presentation = '3h',
      bool undo = false,
      bool cpu = false,
      bool held = false,
    }) => HomeStateNotification.tryParse(
      jsonEncode({
        'event': 'home_state',
        'v': 1,
        'seq': 1,
        'a': cpu ? 'cpu' : 'idle',
        'fen': 'k7/8/8/8/3N4/8/8/7K w - - 0 1',
        'occ': held ? '8000000000000001' : '8000000800000001',
        'g': 'c',
        'duck': '--',
        'out': 'n',
        'win': 'n',
        'rot': 0,
        'white': 1,
        'pm': 0,
        'up': undo ? 1 : 0,
        'as': presentation == '5x' ? presentation : '$presentation$move',
        if (cpu) 'c': ['d4', move.substring(2), '--'],
      }),
    )!;
    Map<String, dynamic> frame(List<int> lit, {bool held = false}) => {
      'occupancy': held ? '8000000000000001' : '8000000800000001',
      'board': List.generate(
        64,
        (square) => [
          [0, 0, 0, 0],
          lit.contains(square) ? [64, 0, 0, 0] : [0, 0, 0, 0],
          [0, 0, 0, 0],
        ],
      ),
    };
    // Supporting ownership checks, not proof of motion: the real native
    // knight journey owns OLDRED, timing, colors, occupancy and all eight Ls.
    expectOrdinaryEmptyCentersForHome(
      home('d4e6'),
      frame([20, 27, 19]),
      'vertical-long-leg',
    );
    expectOrdinaryEmptyCentersForHome(
      home('d4f5'),
      frame([29, 37]),
      'horizontal-long-leg-elbow',
    );
    for (final square in [36, 28]) {
      expect(
        () => expectOrdinaryEmptyCentersForHome(
          home('d4f5'),
          frame([29, square]),
          'horizontal-or-off-L-fault',
        ),
        throwsA(isA<TestFailure>()),
      );
    }
    for (final authority in [
      home('d4f5', presentation: '5x'),
      home('d4f5', undo: true),
      home('d4f5', cpu: true),
    ]) {
      expect(
        () => expectOrdinaryEmptyCentersForHome(
          authority,
          frame([29, 37]),
          'suppressed-Undo-or-unowned-CPU-fault',
        ),
        throwsA(isA<TestFailure>()),
      );
    }
    for (final trail in [
      [20, 35, 27, 19],
      [20, 35, 19],
    ]) {
      expect(
        () => expectOrdinaryEmptyCentersForHome(
          home('d4e6', cpu: true, held: true),
          frame(trail, held: true),
          'all-trail-or-nonadjacent-handoff-fault',
          cpuDirected: true,
        ),
        throwsA(isA<TestFailure>()),
      );
    }
    expectOrdinaryEmptyCentersForHome(
      home('d4f5', cpu: true),
      frame([29, 37]),
      'explicit-CPU-plan',
      cpuDirected: true,
    );
  });
  for (final platform in [TargetPlatform.iOS, TargetPlatform.android]) {
    for (final scale in [1, 2]) {
      testWidgets(
        'coaching-${platform.name}-cpu-ownership-$scale: actual settings gate Home and submission',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          final native = (await tester.runAsync(
            () => VirtualLitchProcess.start(1, false),
          ))!;
          final ble = VirtualLitchGattBle(native);
          final service = ble.createService();
          addTearDown(() => ble.disposeJourney(tester, service));
          final semantics = tester.ensureSemantics();
          try {
            await ble.command(tester, {
              'op': 'hall',
              'mask': opening.toRadixString(16),
            });
            await ble.advance(tester, 200);
            final connection = service.connect(boardId);
            await ble.flush(tester);
            expect(await connection, isTrue);
            await ble.advance(tester, 100);
            await tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme(),
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(scale.toDouble())),
                  child: child!,
                ),
                home: RepaintBoundary(
                  child: LitchMainScaffold(
                    bleService: service,
                    boardId: boardId,
                    boards: const [
                      LitchBoard(id: boardId, name: 'Foxie_Litch', rssi: -45),
                    ],
                    onDisconnect: service.disconnect,
                    loadAppConsolePreference: () async => false,
                  ),
                ),
              ),
            );
            await ble.flush(tester);
            await ble.advance(tester, 100);
            // The existing setup-complete sweep temporarily replaces Coaching badges.
            // Finish that board-owned animation before checking retained badges.
            await ble.advance(tester, 2000);
            for (final owner in [1, 2]) {
              await tapCoachingWithStableActions(tester, ble, owner);
              await ble.flush(tester);
              await ble.advance(tester, 100);
              expectCoaching(tester, owner, true);
            }
            Future<void> blocked(int owner) async {
              expectCoaching(tester, owner, false, enabled: false);
              final node = tester.getSemantics(coaching(owner));
              expect(node.label, contains('Computer player.'));
              expect(node.flagsCollection.isEnabled, ui.Tristate.isFalse);
              expect(node.flagsCollection.isToggled, ui.Tristate.isFalse);
              expectCoaching(tester, owner == 1 ? 2 : 1, true);
              final before = ble.actionWrites.length;
              await tapCoaching(tester, owner);
              final controller = tester
                  .widget<HomeBoardActions>(find.byType(HomeBoardActions))
                  .controller;
              await controller.dispatch(
                LitchControlAction.coaching,
                owner: owner == 1 ? LitchControlOwner.p1 : LitchControlOwner.p2,
                enabled: false,
              );
              await ble.flush(tester);
              await ble.advance(tester, 100);
              expect(
                ble.actionWrites.length,
                before,
                reason:
                    'A CPU owner cannot submit through either UI or controller.',
              );
              expect(service.latestHomeState?.coachingMask, 3);
              expect(find.byType(SnackBar), findsNothing);
            }

            await ble.confirmSetting(tester, service, 'VS_CPU_LEVEL', 5);
            await captureHealthReview(
              tester,
              'coaching-${platform.name}-cpu-p2-assigned-$scale',
            );
            await blocked(2);
            await checkpoint(
              tester,
              ble,
              service,
              'coaching-${platform.name}-cpu-p2-disabled-$scale',
              1,
              false,
              retainedAppMask: 3,
              cpuDirected: true,
            );
            await ble.confirmSetting(tester, service, 'VS_CPU_PLAYER', 1);
            await blocked(1);
            await checkpoint(
              tester,
              ble,
              service,
              'coaching-${platform.name}-cpu-p1-disabled-$scale',
              2,
              false,
              retainedAppMask: 3,
              cpuDirected: true,
            );
            // Physical end/rotation invariance is covered through live settings
            // notifications in the controller test and the four mapped journeys.
            // Here the real CPU has queued White's move, so start-only settings
            // are correctly unavailable until that physical journey is resolved.
            final beforeHumanRestore = ble.actionWrites.length;
            await ble.confirmSetting(tester, service, 'VS_CPU_LEVEL', 0);
            expectCoaching(tester, 1, true);
            expectCoaching(tester, 2, true);
            expect(service.latestHomeState?.coachingMask, 3);
            expect(
              ble.actionWrites.length,
              beforeHumanRestore,
              reason:
                  'Human control restores saved preferences without another Coaching action.',
            );
            expect(tester.takeException(), isNull);
          } finally {
            semantics.dispose();
            final disconnect = service.disconnect();
            await ble.flush(tester);
            await disconnect;
            await tester.pumpWidget(const SizedBox.shrink());
          }
        },
        variant: TargetPlatformVariant.only(platform),
        skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      );
    }
  }
  for (final firstPlayer in <int>[1, 2]) {
    for (final rotate in <bool>[false, true]) {
      final name = 'coaching-ios-first-p$firstPlayer-rotate-${rotate ? 1 : 0}';
      final modernDuck = firstPlayer == 2 && rotate;
      testWidgets(
        modernDuck
            ? '$name: shared-session Duck Hint continuation'
            : '$name: production app, BLE and physical board journey',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          final native = (await tester.runAsync(
            () => VirtualLitchProcess.start(
              firstPlayer,
              rotate,
              hardwareV3: modernDuck,
            ),
          ))!;
          final ble = VirtualLitchGattBle(native)..dropConsole = true;
          final service = ble.createService();
          addTearDown(() => ble.disposeJourney(tester, service));
          final semantics = tester.ensureSemantics();
          var modeResolved = false;
          try {
            final squareRotate = rotate != (firstPlayer == 2);
            final initialOccupancy = modernDuck
                ? opening | (BigInt.one << 27)
                : opening;
            await ble.command(tester, {
              'op': 'hall',
              'mask': initialOccupancy.toRadixString(16),
            });
            await ble.advance(tester, 200);
            final connection = service.connect(boardId);
            await ble.flush(tester);
            expect(await connection, isTrue);
            await ble.advance(tester, 100);
            await tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme(),
                home: RepaintBoundary(
                  child: LitchMainScaffold(
                    bleService: service,
                    boardId: boardId,
                    boards: const [
                      LitchBoard(id: boardId, name: 'Foxie_Litch', rssi: -45),
                    ],
                    onDisconnect: service.disconnect,
                    loadAppConsolePreference: () async => false,
                    onModeResolved: () => modeResolved = true,
                  ),
                ),
              ),
            );
            // Home is intentionally unmounted until the real startup owner
            // resolves mode. One SDK drain or elapsed 100 ms cannot prove it.
            final modeStarted = tester.binding.clock.now();
            while (!modeResolved &&
                tester.binding.clock.now().difference(modeStarted) <
                    const Duration(seconds: 10)) {
              expect(
                find.byType(HomeScreen, skipOffstage: false),
                findsNothing,
              );
              await ble.advance(tester, 10);
            }
            expect(
              modeResolved,
              isTrue,
              reason:
                  'Startup mode must resolve before accessing Play controls',
            );
            await tester.pump();
            expect(find.byType(HomeScreen), findsOneWidget);
            if (modernDuck) {
              await followCoachedDuckHint(
                tester,
                ble,
                service,
                initialOccupancy,
              );
              expect(tester.takeException(), isNull);
              return;
            }
            await tester.ensureVisible(coaching(1));
            await tester.pump();
            expect(service.latestHomeState, isNotNull);
            expectCoaching(tester, 1, false);
            expectCoaching(tester, 2, false);
            await checkpoint(
              tester,
              ble,
              service,
              '$name-ready',
              0,
              squareRotate,
            );

            await fragmentedHomeAcrossAck(tester, ble, service, 1, 1);
            await checkpoint(
              tester,
              ble,
              service,
              '$name-p1-on',
              1,
              squareRotate,
            );
            await fragmentedHomeAcrossAck(tester, ble, service, 2, 3);
            await checkpoint(
              tester,
              ble,
              service,
              '$name-both-on',
              3,
              squareRotate,
            );

            // Real Hall pickup retains legal side-pair guidance; no engine/FEN or
            // Home snapshot is injected into the app or native session.
            final physicalE2 = squareRotate ? 63 - 52 : 52;
            await ble.command(tester, {
              'op': 'hall',
              'mask': (opening & ~(BigInt.one << physicalE2))
                  .toRadixString(16)
                  .padLeft(16, '0'),
            });
            await ble.advance(tester, 100);
            final lifted = await publishedBoardFrame(tester, ble);
            final destinations = BigInt.parse(
              lifted['destinations'] as String,
              radix: 16,
            );
            expect(destinations & (BigInt.one << 44), isNot(BigInt.zero));
            expect(destinations & (BigInt.one << 36), isNot(BigInt.zero));
            final board = lifted['board'] as List<dynamic>;
            expectOrdinaryEmptyCenters(ble, lifted, '$name-e2-lifted');
            for (final square in <int>[44, 36]) {
              final colors = <int>[
                ...(board[square][0] as List).cast<int>(),
                ...(board[square][2] as List).cast<int>(),
              ];
              expect(colors.any((channel) => channel != 0), isTrue);
            }
            await ble.command(tester, {
              'op': 'hall',
              'mask': opening.toRadixString(16),
            });
            await ble.advance(tester, 1000);
            await checkpoint(
              tester,
              ble,
              service,
              '$name-pickup-recovered',
              3,
              squareRotate,
            );

            // Home-before-ACK changes toggled state from board authority while
            // the command remains pending; the controls stay visually available.
            for (final owner in <int>[1, 2]) {
              ble.holdSessionPacket = (packet) => packet.isCheckpoint;
              await tapCoaching(tester, owner);
              await ble.flush(tester);
              await ble.advance(tester, 100);
              expectCoaching(tester, owner, false);
              final request = ble.actionWrites.last;
              expect(request.body, _coachingBody(owner, false));
              expect(
                ble.held.any(
                  (wire) =>
                      wire.sessionPacket?.confirmsApplied(request) == true,
                ),
                isTrue,
              );
              ble.holdSessionPacket = null;
              ble.release('connection_session');
              await tester.pump();
              await ble.advance(tester, 300);
              await ble.advance(tester, 3100);
              await ble.advance(tester, 300);
              expectCoaching(tester, owner, false);
            }
            await checkpoint(
              tester,
              ble,
              service,
              '$name-both-off',
              0,
              squareRotate,
            );

            if (firstPlayer == 1 && !rotate) {
              // Genuine no-report failure: the board accepts P1 On, but neither
              // the original report nor readback reaches the app. Unknown is
              // truthful; the physical badge remains On throughout recovery.
              ble.dropSessionPacket = (packet) => packet.isHomeState;
              await tapCoaching(tester, 1);
              await ble.flush(tester);
              await ble.advance(tester, 1000);
              expectCoaching(tester, 1, false);
              await checkpoint(
                tester,
                ble,
                service,
                '$name-pending-report',
                1,
                squareRotate,
                retainedAppMask: 0,
              );
              await ble.advance(tester, 2200);
              expectCoaching(tester, 1, null);
              expect(
                find.text(
                  'Coaching state is unavailable. Follow the board lights.',
                ),
                findsOneWidget,
              );
              await checkpoint(
                tester,
                ble,
                service,
                '$name-report-unavailable',
                1,
                squareRotate,
                retainedAppMask: 0,
              );
              ble.dropSessionPacket = null;
              unawaited(service.requestCurrentHomeStateSnapshot(boardId));
              await ble.advance(tester, 50);
              await ble.advance(tester, 100);
              expectCoaching(tester, 1, true);
              await checkpoint(
                tester,
                ble,
                service,
                '$name-report-recovered',
                1,
                squareRotate,
              );
              await tapCoaching(tester, 1);
              await ble.flush(tester);
              await ble.advance(tester, 3100);
              expectCoaching(tester, 1, false);

              // Removing Duck and e2 together is ambiguous physical handling,
              // so Duck remains selected in correction. Coaching must still
              // confirm through the real coordinator within its ordinary window.
              final duckSetup = opening | (BigInt.one << 27); // d5
              await ble.command(tester, {
                'op': 'hall',
                'mask': duckSetup.toRadixString(16),
              });
              await ble.advance(tester, 1000);
              expect((await ble.frame(tester))['variant'], 2);
              final beforeRemoval = service.latestHomeState!.sequence;
              await ble.command(tester, {
                'op': 'hall',
                'mask': (opening & ~(BigInt.one << 52)).toRadixString(16),
              });
              await tapCoaching(tester, 1);
              await ble.flush(tester);
              await ble.advance(tester, 100);
              expectCoaching(tester, 1, true);
              expect(
                service.latestHomeState!.sequence,
                greaterThan(beforeRemoval),
              );
              expect((await ble.frame(tester))['variant'], 2);
              expect(
                service.latestHomeState!.action,
                HomeStateAction.correction,
              );
              expect(service.latestHomeState!.gameStatus.duckSquare, 27);
              debugPrint(
                'VIRTUAL_LITCH $name-duck-coalesced-removal '
                'variant=Duck action=correction coaching=On '
                'next=restore-e2-and-Duck-d5',
              );
              await ble.advance(tester, 3100);
              expectCoaching(tester, 1, true);
              expect(
                find.textContaining('Coaching state is unavailable'),
                findsNothing,
              );
              await ble.command(tester, {
                'op': 'hall',
                'mask': duckSetup.toRadixString(16),
              });
              await ble.advance(tester, 1000);
              expect((await ble.frame(tester))['variant'], 2);
              expect(service.latestHomeState!.action, HomeStateAction.idle);
              expect(
                service.latestHomeState!.physicalStatus.occupiedSquares,
                duckSetup,
              );
              expectCoaching(tester, 1, true);
              // Only the Duck now leaves the otherwise untouched opening.
              // This is the reversible variant-selection observation.
              await ble.command(tester, {
                'op': 'hall',
                'mask': opening.toRadixString(16),
              });
              await ble.advance(tester, 1000);
              expect((await ble.frame(tester))['variant'], 0);
              expect(service.latestHomeState!.action, HomeStateAction.idle);
              expect(service.latestHomeState!.gameStatus.duckSquare, isNull);
              debugPrint(
                'VIRTUAL_LITCH $name-duck-only-removal '
                'variant=Undecided action=idle coaching=On '
                'next=continue-opening-play',
              );
              await checkpoint(
                tester,
                ble,
                service,
                '$name-duck-setup-recovered',
                1,
                squareRotate,
              );
              await tapCoaching(tester, 1);
              await ble.flush(tester);
              await ble.advance(tester, 3100);
              expectCoaching(tester, 1, false);
            }

            final beforeReplay = (await ble.frame(tester))['home_sequence'];
            unawaited(service.requestCurrentHomeStateSnapshot(boardId));
            await ble.advance(tester, 50);
            await ble.advance(tester, 50);
            expect(
              (await ble.frame(tester))['home_sequence'],
              beforeReplay,
              reason:
                  'Unchanged forced replay must preserve production sequence.',
            );

            // Capture one real report, then retire the callback across stream loss
            // and reconnect. Deliberately invoking it cannot replace current Home.
            ble.holdSessionPacket = (packet) => packet.isHomeState;
            unawaited(service.requestCurrentHomeStateSnapshot(boardId));
            await ble.advanceUntil(
              tester,
              () => _heldHomeWithMask(ble, 0).isNotEmpty,
              'a complete current Home is captured for retirement',
            );
            final retired = _heldHomeWithMask(ble, 0);
            ble.held.clear();
            ble.holdSessionPacket = null;
            // Recovering the diagnostic Console does not retire the typed Home.
            ble.failConsole();
            await ble.advance(tester, 550);
            await ble.advance(tester, 100);
            expectCoaching(tester, 1, false);
            retired.first.pipe!.fail();
            await tester.pump();
            final disconnect = service.disconnect();
            await ble.flush(tester);
            await disconnect;
            expect(service.latestHomeState, isNull);
            final reconnect = service.connect(boardId);
            await ble.flush(tester);
            expect(await reconnect, isTrue);
            await ble.advance(tester, 100);
            await ble.advanceUntil(
              tester,
              () => service.latestHomeState?.coachingMask == 0,
              'the new connection hydrates both Coaching settings',
            );
            final current = service.latestHomeState;
            for (final packet in retired) {
              ble.deliver(packet, lateCallback: true);
            }
            await tester.pump();
            expect(identical(service.latestHomeState, current), isTrue);
            expectCoaching(tester, 1, false);
            expectCoaching(tester, 2, false);
            await checkpoint(
              tester,
              ble,
              service,
              '$name-reconnected',
              0,
              squareRotate,
            );
            expect(tester.takeException(), isNull);
          } finally {
            semantics.dispose();
            // Stop the real keepalive before Flutter verifies timer invariants.
            // Full stream/process disposal remains in addTearDown's real zone,
            // whose outer finally reaps the child even if this step fails.
            final disconnect = service.disconnect();
            await ble.flush(tester);
            await disconnect;
            await tester.pumpWidget(const SizedBox.shrink());
          }
        },
        variant: TargetPlatformVariant.only(TargetPlatform.iOS),
        skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      );
    }
  }
}
