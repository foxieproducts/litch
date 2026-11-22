import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

void main() {
  testWidgets(
    'clock setup: real New Game recovery and first-pickup ownership',
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
      final controlEvents = <LitchControlEvent>[];
      final controlSubscription = service.controlActionEvents.listen(
        controlEvents.add,
      );
      addTearDown(controlSubscription.cancel);
      int homeRequests() => ble.sessionWrites
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.control &&
                packet.code == ConnectionSessionControl.query &&
                packet.sequence == 0,
          )
          .length;
      const boardId = 'virtual-litch-production';
      final opening = BigInt.parse('FFFF00000000FFFF', radix: 16);
      final lifted = opening & ~(BigInt.one << 52);
      final moved = lifted | (BigInt.one << 36);
      Future<void> hall(BigInt occupancy) => ble.command(tester, {
        'op': 'hall',
        'mask': occupancy.toRadixString(16).padLeft(16, '0'),
      });
      Finder selector(int preset) =>
          find.byKey(ValueKey('home-game-clock-dropdown:$preset'));
      Future<void> tapControl(String key) async {
        final control = find.byKey(ValueKey(key));
        await tester.ensureVisible(control);
        // Scrolling is immediate; the board's correction pulse keeps animating.
        await tester.pump();
        expect(control.hitTestable(), findsOneWidget);
        await tester.tap(control);
        await tester.pump();
      }

      Future<void> checkpoint(String name, String next) async {
        final frame = await ble.frame(tester);
        final clock = service.latestHomeState!.clockStatus!;
        debugPrint(
          'CLOCK_SETUP_JOURNEY $name native=${frame['now_ms']} '
          'occupancy=${frame['occupancy']} expected=${frame['expected']} '
          'action=${frame['required_action']} display=${frame['display']} '
          'setup=${clock.setupState.name} running=${clock.running} '
          'times=${clock.whiteRemainingSeconds}/${clock.blackRemainingSeconds} '
          'next=$next',
        );
        await captureHealthReview(tester, 'clock-setup-$name');
      }

      try {
        // Raw incomplete opening, not an injected FEN or Home notification.
        await hall(lifted);
        await ble.advance(tester, 200);
        final connection = service.connect(boardId);
        await ble.flush(tester);
        expect(await connection, isTrue);
        await ble.confirmSetting(tester, service, 'VS_CPU_LEVEL', 0);
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
              ),
            ),
          ),
        );
        await ble.flush(tester);
        await ble.advance(tester, 200);
        expect(
          service.latestHomeState!.clockStatus!.setupState,
          HomeClockSetupState.available,
        );
        expect(selector(0), findsOneWidget);
        await tapControl('home-game-clock-dropdown:0');
        await tester.pump(const Duration(milliseconds: 300));
        final fiveMinutes = find.text('5 min').last;
        expect(fiveMinutes.hitTestable(), findsOneWidget);
        await tester.tap(fiveMinutes);
        await tester.pump();
        await ble.flush(tester);
        await ble.advance(tester, 200);
        expect(selector(3), findsOneWidget);
        await ble.advance(tester, 1500);
        final setupClock = service.latestHomeState!.clockStatus!;
        expect(setupClock.running, isFalse);
        expect(setupClock.whiteRemainingSeconds, 300);
        expect(setupClock.blackRemainingSeconds, 300);
        await checkpoint('pieces-incomplete', 'place-the-missing-e2-pawn');

        await hall(opening);
        await ble.advance(tester, 2500);
        expect(service.latestHomeState!.clockStatus!.running, isFalse);
        expect(selector(3), findsOneWidget);
        await checkpoint('ready-untouched', 'lift-a-legal-first-piece');
        await hall(lifted);
        await ble.advance(tester, 1100);
        expect(service.latestHomeState!.clockStatus!.running, isTrue);
        await hall(opening);
        await ble.advance(tester, 100);
        expect(
          service.latestHomeState!.clockStatus!.setupState,
          HomeClockSetupState.gameStarted,
        );
        expect(selector(3), findsNothing);
        final rejected = service.writeSettingAcknowledged(
          deviceId: boardId,
          key: 'TIMER',
          value: 6,
        );
        final rejectedAck =
            await ble.completeTransaction(tester, rejected)
                as LitchSettingWriteAcknowledged;
        expect(rejectedAck.outcome.ok, isFalse);
        expect(
          service.latestHomeState!.clockStatus!.whiteRemainingSeconds,
          lessThan(300),
        );
        await checkpoint(
          'pickup-return',
          'continue-playing-or-explicitly-start-a-new-game',
        );

        await hall(lifted);
        await hall(moved);
        await ble.advance(tester, 2600);
        expect(
          service.latestHomeState!.clockStatus!.setupState,
          HomeClockSetupState.gameStarted,
        );
        await tapControl('control:undo');
        await ble.flush(tester);
        await ble.advance(tester, 100);
        await hall(lifted);
        await hall(opening);
        await ble.advance(tester, 1200);
        expect(
          service.latestHomeState!.clockStatus!.setupState,
          HomeClockSetupState.gameStarted,
        );
        expect(selector(3), findsNothing);
        expect(
          service.latestHomeState!.clockStatus!.whiteRemainingSeconds,
          lessThan(300),
        );
        final undoFrame = await ble.frame(tester);
        expect(undoFrame['expected'], opening.toRadixString(16).toUpperCase());
        expect(
          service.latestHomeState!.position.fen,
          startsWith('rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w'),
        );
        expect(
          controlEvents.any(
            (event) =>
                event.action == LitchControlAction.undo &&
                event.phase == LitchControlPhase.applied,
          ),
          isTrue,
        );
        await checkpoint(
          'undo-to-opening',
          'explicit-new-game-is-required-to-reset-the-clock',
        );
        // Move one pawn away again so New Game is observed before piece setup.
        await hall(lifted);
        await hall(moved);
        await ble.advance(tester, 2600);
        final oldSequence = service.latestHomeState!.sequence;
        final requestsBeforeReset = homeRequests();
        await tapControl('control:new-game');
        await tester.pump(const Duration(milliseconds: 150));
        final startNewGame = find.widgetWithText(
          FilledButton,
          'Start new game',
        );
        expect(startNewGame.hitTestable(), findsOneWidget);
        // Drop native Home, including the first replay; the real Ordered
        // outcome, Settings and the rest of the shared session still arrive.
        final droppedReplay = <int>{};
        ble.dropSessionPacket = (packet) {
          if (!packet.isHomeState) return false;
          if (homeRequests() > requestsBeforeReset) {
            droppedReplay.add(packet.code);
          }
          return true;
        };
        final eventStart = controlEvents.length;
        final actionCount = ble.actionWrites.length;
        await tester.tap(startNewGame);
        await ble.advanceUntil(
          tester,
          () => droppedReplay.length == HomeStateWireCodec.fragmentCount,
          'all Home fragments from the first reset readback were dropped',
        );
        final resetEvents = controlEvents
            .skip(eventStart)
            .where((event) => event.action == LitchControlAction.newGame);
        final pending = resetEvents.singleWhere(
          (event) => event.phase == LitchControlPhase.pending,
        );
        final applied = resetEvents.singleWhere(
          (event) => event.phase == LitchControlPhase.applied,
        );
        expect(applied.requestId, pending.requestId);
        expect(pending.requestId, isNotNull);
        final actions = ble.actionWrites.skip(actionCount);
        expect(
          actions.single.body,
          LitchControlProtocol.encodeConnectionAction(
            action: LitchControlAction.newGame,
          ),
        );
        expect(service.latestHomeState!.sequence, oldSequence);
        final requestsAtLoss = homeRequests();
        ble.dropSessionPacket = null;
        await ble.advanceUntil(
          tester,
          () =>
              service.latestHomeState!.clockStatus!.setupState ==
              HomeClockSetupState.available,
          'the reset clock recovers in Play after a lost board snapshot',
        );
        expect(homeRequests(), greaterThan(requestsAtLoss));
        expect(service.latestHomeState!.sequence, greaterThan(oldSequence));
        expect(
          service.latestHomeState!.clockStatus!.setupState,
          HomeClockSetupState.available,
        );
        expect(service.latestHomeState!.clockStatus!.running, isFalse);
        expect(
          service.latestHomeState!.clockStatus!.whiteRemainingSeconds,
          300,
        );
        expect(
          service.latestHomeState!.clockStatus!.blackRemainingSeconds,
          300,
        );
        expect(selector(3), findsOneWidget);
        final frame = await ble.frame(tester);
        expect(frame['occupancy'], moved.toRadixString(16).toUpperCase());
        expect(frame['expected'], opening.toRadixString(16).toUpperCase());
        await checkpoint(
          'new-game-recovered',
          'choose-the-clock-before-restoring-pieces',
        );
        expect(tester.takeException(), isNull);
      } finally {
        ble.dropSessionPacket = null;
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
