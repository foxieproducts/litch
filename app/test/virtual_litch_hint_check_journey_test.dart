import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';

import 'health_render_capture.dart';
import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const _boardId = 'virtual-hint-check';
const _reportedFen =
    '1rb1k1nr/pp1p1ppp/2npp3/qN6/3P1B2/5N2/PPP2PPP/R2QKB1R w KQk - 0 1';
const _beforeCheckFen =
    '1rbqk1nr/pp1p1ppp/2npp3/1N6/3P1B2/5N2/PPP2PPP/R2QKB1R b KQk - 0 1';
const _legalEvasions = {'b5c3', 'f4d2', 'f3d2', 'b2b4', 'c2c3', 'd1d2', 'e1e2'};

BigInt _occupancy(String fen) {
  var square = 0;
  var mask = BigInt.zero;
  for (final character in fen.split(' ').first.split('')) {
    if (character == '/') continue;
    final empty = int.tryParse(character);
    if (empty != null) {
      square += empty;
    } else {
      mask |= BigInt.one << square++;
    }
  }
  return mask;
}

String _square(int index) =>
    '${String.fromCharCode(97 + index % 8)}${8 - index ~/ 8}';

void main() {
  for (final platform in [TargetPlatform.iOS, TargetPlatform.android]) {
    testWidgets(
      'reported check FEN ${platform.name}: actual app Hint ready replay and terminal guards',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native, enableOtaControl: true);
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: ble,
        );
        var cleanupStarted = false;
        Future<void> Function()? cancelControlEvents;
        Future<void> cleanupJourney() async {
          cleanupStarted = true;
          try {
            final cancel = cancelControlEvents;
            if (cancel != null) {
              await ble.finishTesterWork(tester, cancel());
            }
          } finally {
            await ble.disposeJourney(tester, service);
          }
        }

        addTearDown(() {
          if (!cleanupStarted) return cleanupJourney();
        });
        final events = <LitchControlEvent>[];
        final subscription = service.controlActionEvents.listen(events.add);
        cancelControlEvents = subscription.cancel;
        final hint = find.byKey(const ValueKey('control:hint'));
        Future<void> waitFor(
          bool Function() predicate,
          String checkpoint,
        ) {
          if (predicate()) {
            // No pump follows an already-satisfied check. Return a real-owned
            // completion so awaiting it cannot strand a fake-zone microtask.
            return tester.runAsync<void>(() async {});
          }

          Future<void> poll() async {
            for (var attempt = 0; attempt < 40; ++attempt) {
              if (predicate()) return;
              await ble.advance(tester, 100);
            }
            fail(
              '$checkpoint: ${service.latestHomeState?.position.fen} '
              '${service.latestHomeState?.assistance?.presentation}',
            );
          }

          return poll();
        }

        Future<void> hall(BigInt mask) async {
          await ble.command(tester, {
            'op': 'hall',
            'mask': mask.toRadixString(16).padLeft(16, '0'),
          });
        }

        Future<void> load(String fen) async {
          // Setup alone uses the production FEN loader. The Hint action,
          // acknowledgements, engine result and Home delivery are not injected.
          await ble.command(tester, {'op': 'load_fen', 'fen': fen});
          await hall(_occupancy(fen));
          await ble.advance(tester, 200);
          await waitFor(
            () => service.latestHomeState?.position.fen == fen,
            'loaded authoritative FEN',
          );
          await waitFor(
            () =>
                service.latestHomeState?.physicalStatus.placementMode == false,
            'physical placement stabilized',
          );
        }

        Future<void> tapHint() async {
          await waitFor(
            () =>
                service.latestHomeState?.action == HomeStateAction.idle &&
                service.latestHomeState?.assistance?.manualAvailable == true,
            'eligible human Hint admission',
          );
          await ble.finishTesterWork(tester, tester.ensureVisible(hint));
          await ble.finishTesterWork(tester, tester.pump());
          expect(tester.widget<OutlinedButton>(hint).onPressed, isNotNull);
          await ble.finishTesterWork(tester, tester.tap(hint));
          await ble.flush(tester);
        }

        Future<void> expectReady(String checkpoint) async {
          await waitFor(
            () => service.latestHomeState?.assistance?.isReady == true,
            checkpoint,
          );
          final assistance = service.latestHomeState!.assistance!;
          expect(service.latestHomeState!.action, HomeStateAction.idle);
          expect(service.latestHomeState!.position.sideToMove, HomeSide.white);
          expect(service.latestHomeState!.gameStatus.isFinished, isFalse);
          expect(assistance.requested, isTrue);
          expect(assistance.coachingOwned, isTrue);
          expect(service.latestHomeState!.coachingMask! & 1, 1);
          expect(assistance.move, isNotNull);
          final move =
              '${_square(assistance.move!.from)}'
              '${_square(assistance.move!.to)}';
          expect(_legalEvasions, contains(move));
          expect(find.text('Show Move'), findsOneWidget);
          expect(
            find.byKey(
              ValueKey('assistance-to-${_square(assistance.move!.to)}'),
            ),
            findsOneWidget,
          );
          final frame = await ble.frame(tester);
          expect(frame['phase'], 0);
          expect(frame['occupancy'], frame['expected']);
          final board = frame['board'] as List<dynamic>;
          for (final square in [assistance.move!.from, assistance.move!.to]) {
            for (final slot in [0, 2]) {
              expect(
                (board[square][slot] as List<dynamic>).any((v) => v > 0),
                isTrue,
                reason: '$checkpoint visible Hint side LED $square/$slot',
              );
            }
          }
          debugPrint(
            'HINT_CHECK $checkpoint fen=${service.latestHomeState!.position.fen} '
            'occupancy=${frame['occupancy']} phase=${frame['phase']} '
            'display=${frame['display']} ready=$move next=follow hinted move',
          );
        }

        Future<void> replayReadyHint() async {
          final before = service.latestHomeState!;
          final tuple = before.assistance!.move;
          final duck = before.assistance!.duckMove;
          final appliedBefore = events
              .where(
                (event) =>
                    event.action == LitchControlAction.hint &&
                    event.phase == LitchControlPhase.applied,
              )
              .map((event) => event.requestId)
              .toSet();
          final observed = <HomeAssistancePresentation>[];
          final observation = service.homeStateStream.listen((state) {
            if (state.position.fen == before.position.fen &&
                state.assistance != null) {
              observed.add(state.assistance!.presentation);
            }
          });
          Iterable<LitchControlEvent> replayReceipts() => events.where(
            (event) =>
                event.action == LitchControlAction.hint &&
                event.phase == LitchControlPhase.applied &&
                !appliedBefore.contains(event.requestId),
          );
          try {
            ble.heldChannels.add('connection_session');
            await tapHint();
            final actions = tester
                .widget<HomeBoardActions>(find.byType(HomeBoardActions))
                .controller;
            await ble.advance(tester, 40);
            expect(
              actions.isPending(LitchControlAction.hint),
              isTrue,
              reason: 'ready Home alone cannot acknowledge a replay',
            );
            expect(replayReceipts(), isEmpty);
            expect(service.latestHomeState!.assistance!.move, tuple);
            expect(actions.hintLabel, 'Show Move');
            ble.heldChannels.remove('connection_session');
            ble.release('connection_session');
            await ble.flush(tester);
            await waitFor(
              () => replayReceipts().isNotEmpty,
              'real native replay Applied receipt',
            );
            expect(actions.isPending(LitchControlAction.hint), isFalse);
            // The bridge bounds engine work but exposes no search counter.
            // Observe finite same-tuple readiness; ReplayHintMove's existing
            // native ownership tests/source establish no calculation restart.
            await ble.advance(tester, 3100);
            final after = service.latestHomeState!;
            expect(after.position.fen, before.position.fen);
            expect(
              after.physicalStatus.occupiedSquares,
              before.physicalStatus.occupiedSquares,
            );
            expect(after.assistance!.requested, isTrue);
            expect(after.assistance!.isReady, isTrue);
            expect(after.assistance!.move, tuple);
            expect(after.assistance!.duckMove, duck);
            expect(
              observed,
              isNot(contains(HomeAssistancePresentation.pending)),
            );
            expect(
              observed,
              isNot(contains(HomeAssistancePresentation.thinking)),
            );
            expect(
              replayReceipts().map((event) => event.requestId).toSet(),
              hasLength(1),
            );
            await expectReady('same-position-replay');
          } finally {
            ble.heldChannels.remove('connection_session');
            ble.release('connection_session');
            await ble.flush(tester);
            await ble.finishTesterWork(tester, observation.cancel());
          }
        }

        try {
          await hall(
            _occupancy(
              'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
            ),
          );
          await ble.advance(tester, 200);
          final connection = service.connect(_boardId);
          await ble.flush(tester);
          expect(await ble.finishTesterWork(tester, connection), isTrue);
          await ble.advance(tester, 100);
          await ble.finishTesterWork(
            tester,
            tester.pumpWidget(
              MaterialApp(
                theme: buildLitchTheme().copyWith(platform: platform),
                home: Scaffold(
                  body: HomeScreen(bleService: service, boardId: _boardId),
                ),
              ),
            ),
          );
          await ble.flush(tester);

          await load(_reportedFen);
          // Show Move is the Coaching action. Establish that saved preference
          // through the real Home control before exercising ready replay.
          final coaching = find.byKey(const ValueKey('control:coaching:p1'));
          await ble.finishTesterWork(tester, tester.ensureVisible(coaching));
          await ble.finishTesterWork(tester, tester.pump());
          expect(tester.widget<OutlinedButton>(coaching).onPressed, isNotNull);
          await ble.finishTesterWork(tester, tester.tap(coaching));
          await ble.flush(tester);
          await waitFor(
            () =>
                service.latestHomeState?.coachingMask == 1 &&
                service.latestHomeState?.assistance?.coachingOwned == true,
            'P1 Coaching established by native authority',
          );
          await tapHint();
          await expectReady('imported-check');
          expect(
            events.where(
              (event) =>
                  event.action == LitchControlAction.hint &&
                  event.phase == LitchControlPhase.applied,
            ),
            isNotEmpty,
          );
          await replayReadyHint();

          await load(_beforeCheckFen);
          var occupied = _occupancy(_beforeCheckFen) & ~(BigInt.one << 3);
          await hall(occupied);
          occupied |= BigInt.one << 24;
          await hall(occupied);
          await ble.advance(tester, 2300);
          await waitFor(
            () =>
                service.latestHomeState?.position.fen.split(' ').first ==
                _reportedFen.split(' ').first,
            'physical Qa5 gives check',
          );
          await tapHint();
          await expectReady('physical-check');

          for (final terminal in [
            '7k/6Q1/6K1/8/8/8/8/8 b - - 0 1',
            '7k/5Q2/6K1/8/8/8/8/8 b - - 0 1',
          ]) {
            await load(terminal);
            expect(service.latestHomeState?.action, HomeStateAction.idle);
            expect(
              service.latestHomeState?.assistance?.manualAvailable,
              isFalse,
            );
            await ble.finishTesterWork(tester, tester.ensureVisible(hint));
            await ble.finishTesterWork(tester, tester.pump());
            expect(tester.widget<OutlinedButton>(hint).onPressed, isNull);
            expect(service.latestHomeState?.assistance?.isReady, isNot(true));
            // Bypass only the disabled app button to exercise the real board
            // guard; no rejection or engine status is supplied by this test.
            final request = service.dispatchControlAction(
              deviceId: _boardId,
              action: LitchControlAction.hint,
            );
            await ble.flush(tester);
            final sent = await ble.finishTesterWork(tester, request);
            await waitFor(
              () => events.any(
                (event) =>
                    event.requestId == sent.requestId &&
                    event.phase == LitchControlPhase.rejected &&
                    event.reason == LitchMutationReason.gameOver,
              ),
              'terminal Hint rejected by native authority',
            );
            debugPrint(
              'HINT_CHECK terminal=$terminal next=no legal move; Hint disabled',
            );
          }
        } catch (error, stack) {
          // Preserve the body failure if asynchronous cleanup also fails.
          debugPrint('HINT_CHECK body failed before cleanup: $error\n$stack');
          rethrow;
        } finally {
          try {
            await ble.finishTesterWork(
              tester,
              tester.pumpWidget(const SizedBox.shrink()),
            );
            await ble.finishTesterWork(tester, tester.pump());
          } finally {
            // Retire the service before Flutter checks pending timers. The
            // synchronous teardown fallback only owns earlier setup failures.
            await cleanupJourney();
          }
        }
      },
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
