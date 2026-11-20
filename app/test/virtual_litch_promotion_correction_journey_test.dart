import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'health_render_capture.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const boardId = 'virtual-promotion-correction';
Uint8List latestHomeBytes(VirtualLitchBle ble) {
  final values = <int, Uint8List>{};
  final fragments = <int, Set<int>>{};
  Uint8List? latest;
  for (final packet in ble.emitted.where((p) => p.epoch == ble.nativeEpoch)) {
    final decoded = packet.sessionPacket;
    if (decoded == null || !decoded.isHomeState) continue;
    final value = values.putIfAbsent(
      decoded.sequence,
      () => Uint8List(HomeStateWireCodec.payloadBytes),
    );
    final seen = fragments.putIfAbsent(decoded.sequence, () => <int>{});
    final index = decoded.code - HomeStateWireCodec.firstStateCode;
    const fragmentBytes = HomeStateWireCodec.fragmentBytes;
    expect(decoded.body.length, fragmentBytes);
    value.setRange(
      index * fragmentBytes,
      (index + 1) * fragmentBytes,
      decoded.body,
    );
    seen.add(index);
    if (seen.length == HomeStateWireCodec.fragmentCount) {
      latest = Uint8List.fromList(value);
    }
  }
  expect(
    latest,
    isNotNull,
    reason: 'actual epoch4 retained Home must complete ten fragments',
  );
  return latest!;
}

void main() {
  for (final (platform, choose, minimal) in [
    (TargetPlatform.iOS, true, false),
    (TargetPlatform.android, true, false),
    (TargetPlatform.iOS, false, false),
    (TargetPlatform.android, false, true),
    (TargetPlatform.iOS, true, true),
  ]) {
    final mode = choose ? 'Choose Knight' : 'automatic Queen';
    testWidgets(
      'raw promotion ${platform.name}: $mode, Minimal $minimal, Home 200%',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native);
        final service = ble.createService();
        addTearDown(() => ble.disposeJourney(tester, service));
        final semantics = tester.ensureSemantics();
        try {
          var physical = BigInt.parse('FFFF00000000FFFF', radix: 16);
          Future<void> hall() => ble.command(tester, {
            'op': 'hall',
            'mask': physical.toRadixString(16).padLeft(16, '0'),
          });
          Future<void> move(int from, int to, {bool capture = false}) async {
            physical &= ~(BigInt.one << from);
            await hall();
            if (capture) {
              physical &= ~(BigInt.one << to);
              await hall();
            }
            physical |= BigInt.one << to;
            await hall();
            await ble.advance(tester, 2000);
          }

          Future<void> tap(int button) async {
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': true,
            });
            await ble.advance(tester, 20);
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': false,
            });
            await ble.advance(tester, 20);
          }

          final guidance = find.byKey(
            const ValueKey('home-guidance-live-region'),
            skipOffstage: false,
          );
          final highlighted = find.byKey(
            const ValueKey('promotion-confirmed-b8'),
          );
          final artwork = find.byKey(const ValueKey('piece-b8'));
          Future<void> verifyConfirmation() async {
            final piece = choose ? 'N' : 'Q';
            final confirmation = choose
                ? 'Promoted to Knight'
                : 'Promoted to Queen';
            final confirmationText = find.text(
              confirmation,
              skipOffstage: false,
            );
            await ble.advance(tester, 100);
            final live = service.latestHomeState!;
            expect(live.action, HomeStateAction.idle);
            expect(live.position.pieces[1], piece);
            expect(live.position.sideToMove, HomeSide.black);
            expect(live.promotionConfirmation, (
              from: 9,
              to: 1,
              piece: piece,
              reveal: true,
              automatic: !choose,
            ));
            expect(live.physicalMovePrompt, isNull);
            expect(live.lastMoves?.white?.fromSquare, 9);
            expect(live.lastMoves?.white?.toSquare, 1);
            expect(live.lastMoves?.mostRecent, HomeSide.white);
            await tester.ensureVisible(guidance);
            await tester.pump();
            expect(confirmationText, findsOneWidget);
            expect(tester.getSemantics(guidance).label, contains(confirmation));
            expect(
              find.text('Replace the pawn on b8.'),
              choose ? findsNothing : findsOneWidget,
            );

            await tester.ensureVisible(artwork);
            await tester.pump();
            final fill = find
                .descendant(
                  of: highlighted,
                  matching: find.byType(DecoratedBox),
                )
                .first;
            double highlightAlpha() =>
                (tester.widget<DecoratedBox>(fill).decoration as BoxDecoration)
                    .color!
                    .a;
            final initialAlpha = highlightAlpha();
            final initialWidth = tester.getRect(artwork).width;
            await ble.advance(tester, 320);
            expect(confirmationText, findsOneWidget);
            if (minimal) {
              expect(initialAlpha, greaterThan(0.6));
              expect(highlightAlpha(), initialAlpha);
              expect(
                tester.getRect(artwork).width,
                closeTo(initialWidth, 0.01),
              );
            } else {
              expect(highlightAlpha(), greaterThan(initialAlpha));
              expect(tester.getRect(artwork).width, greaterThan(initialWidth));
            }
            await ble.advance(tester, 2500);
            expect(service.latestHomeState!.promotionConfirmation, isNull);
            expect(service.latestHomeState!.position.pieces[1], piece);
            expect(confirmationText, findsNothing);
            expect(highlighted, findsNothing);
            expect(tester.takeException(), isNull);
          }

          await hall();
          await ble.advance(tester, 2001);
          final connecting = service.connect(boardId);
          await ble.flush(tester);
          expect(await connecting, isTrue);
          await ble.advance(tester, 100);
          if (choose) {
            await ble.confirmSetting(
              tester,
              service,
              'PROMOTION_MODE',
              'choose',
            );
          }
          if (minimal) {
            await ble.confirmSetting(
              tester,
              service,
              'MINIMAL_ANIMATIONS',
              true,
            );
          }

          // Existing diagnostic subscription exercises the actual JSON formatter too;
          // the product Home remains owned by the modern binary State assembler.
          await ble.command(tester, {
            'op': 'subscribe',
            'channel': 'console',
            'enabled': true,
          });
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme().copyWith(platform: platform),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: const TextScaler.linear(2)),
                child: child!,
              ),
              home: RepaintBoundary(
                child: Scaffold(
                  body: HomeScreen(bleService: service, boardId: boardId),
                ),
              ),
            ),
          );
          await ble.flush(tester);
          await move(48, 32);
          await move(1, 18);
          await move(32, 24);
          await move(15, 23);
          await move(24, 16);
          await move(23, 31);
          await move(16, 9, capture: true);
          await move(31, 39);
          await move(9, 1);
          if (!choose) {
            await verifyConfirmation();
            return;
          }
          Future<void> expectSelection(String piece, String suffix) async {
            await ble.advance(tester, 100);
            final live = service.latestHomeState!;
            expect(live.action, HomeStateAction.promote);
            expect(live.physicalMovePrompt?.promotionSelection, suffix);
            expect(live.promotionConfirmation, isNull);
            expect(find.text('Selected $piece'), findsOneWidget);
            expect(
              find.byKey(ValueKey('promotion-selection-$piece')),
              findsOneWidget,
            );
            expect(highlighted, findsNothing);
          }

          await expectSelection('queen', 'q');
          await tap(2);
          await expectSelection('knight', 'n');
          await tap(3);
          await expectSelection('queen', 'q');
          await tap(3);
          await expectSelection('rook', 'r');
          await tap(3);
          await expectSelection('bishop', 'b');
          await tap(3);
          await expectSelection('knight', 'n');
          if (minimal) {
            expect(service.latestHomeState!.action, HomeStateAction.promote);
            expect(service.latestHomeState!.promotionConfirmation, isNull);
            await tap(6);
            await verifyConfirmation();
            return;
          }
          final preFen = service.latestHomeState!.position.fen;
          final token = (await ble.frame(tester))['pending_token'];
          final retired = ble.emitted
              .where((packet) => packet.sessionPacket?.isHomeState == true)
              .toList();
          expect(retired, isNotEmpty);
          Future<void> checkpoint(
            String name,
            HomeStateAction action,
            Set<int> missing,
          ) async {
            await ble.advance(tester, 100);
            final current = service.latestHomeState!;
            expect(current.action, action);
            expect(current.promotionConfirmation, isNull);
            expect(highlighted, findsNothing);
            expect(current.position.fen, preFen);
            expect(current.lastMoves?.white?.fromSquare, 16);
            expect(current.lastMoves?.white?.toSquare, 9);
            expect(current.lastMoves?.mostRecent, HomeSide.black);
            expect(current.physicalMovePrompt?.fromSquare, 9);
            expect(current.physicalMovePrompt?.toSquare, 1);
            expect(current.physicalMovePrompt?.captureSquare, isNull);
            final binaryBytes = latestHomeBytes(ble);
            final binary = HomeStateWireCodec.tryDecode(
              binaryBytes,
              sequence: current.sequence,
            )!;
            expect(binary.position.fen, current.position.fen);
            expect(binary.action, current.action);
            expect(binary.physicalStatus.occupiedSquares, physical);
            final board = HomeBoardSnapshot(
              position: binary.position,
              gameStatus: binary.gameStatus,
              physicalStatus: binary.physicalStatus,
              physicalMovePrompt: binary.physicalMovePrompt,
              homeStateAction: binary.action,
              liveSequence: binary.sequence,
            );
            expect(board.missingExpectedSquares, missing);
            expect(board.unexpectedOccupiedSquares, isEmpty);
            expect(board.physicalPieceAt(9), isNull);
            expect(board.physicalPieceAt(1), isNull);
            final console = utf8.decode(
              ble.emitted
                  .where(
                    (p) => p.channel == 'console' && p.epoch == ble.nativeEpoch,
                  )
                  .expand((p) => p.bytes)
                  .toList(),
            );
            final homeJson = console.split('\n').lastWhere((line) {
              final value = HomeStateNotification.tryParse(line);
              return value != null &&
                  value.action == current.action &&
                  value.position.fen == preFen &&
                  value.physicalStatus.occupiedSquares == physical;
            });
            final json = HomeStateNotification.tryParse(homeJson)!;
            expect(json.physicalMovePrompt?.fromSquare, 9);
            expect(json.physicalMovePrompt?.toSquare, 1);
            expect(json.physicalMovePrompt?.captureSquare, isNull);
            await tester.ensureVisible(
              find.byKey(
                const ValueKey('home-guidance-live-region'),
                skipOffstage: false,
              ),
            );
            await tester.pump();
            if (action == HomeStateAction.promote) {
              await expectSelection('knight', 'n');
            } else {
              expect(current.physicalMovePrompt?.promotionSelection, isNull);
              expect(
                find.byKey(const ValueKey('promotion-selection-knight')),
                findsNothing,
              );
            }
            expect(
              tester
                  .getSemantics(
                    find.byKey(const ValueKey('home-guidance-live-region')),
                  )
                  .label,
              missing.isEmpty
                  ? contains('Circle to confirm')
                  : isNot(contains('Circle to confirm')),
            );
            await tester.pump(const Duration(milliseconds: 40));
            await captureHealthReview(
              tester,
              'promotion-${platform.name}-$name-top-200',
            );
            final cell = find.byKey(
              ValueKey(missing.contains(50) ? 'square-c2' : 'square-b8'),
            );
            await tester.ensureVisible(cell);
            await tester.pump();
            expect(
              tester
                  .getSemantics(find.byKey(const ValueKey('square-b7')))
                  .label,
              startsWith('b7, empty'),
            );
            expect(
              tester
                  .getSemantics(find.byKey(const ValueKey('square-b8')))
                  .label,
              missing.contains(1)
                  ? 'Place the promotion piece on b8.'
                  : 'b8, occupied by a piece',
            );
            if (missing.contains(50)) {
              expect(tester.getSemantics(cell).label, 'P1 pawn expected on c2');
            }
            await captureHealthReview(
              tester,
              'promotion-${platform.name}-$name-board-200',
            );
            final frame = await ble.frame(tester);
            expect(frame['pending_token'], token);
            final directory = Platform.environment['LITCH_VIRTUAL_EVIDENCE'];
            if (directory != null) {
              await tester.runAsync(() async {
                final file = File(
                  '$directory/promotion-${platform.name}-$name.json',
                );
                await file.parent.create(recursive: true);
                await file.writeAsString(
                  const JsonEncoder.withIndent('  ').convert({
                    'native_frame': frame,
                    'home_json': homeJson,
                    'actual_json': console,
                    'actual_binary': binaryBytes.toList(),
                    'missing': missing.toList(),
                    'pre_fen': preFen,
                    'token': token,
                  }),
                );
              });
            }
            expect(tester.takeException(), isNull);
          }

          await checkpoint('selected-knight', HomeStateAction.promote, {});
          physical &= ~(BigInt.one << 50);
          await hall();
          await ble.advance(tester, 2000);
          await checkpoint('missing-c2', HomeStateAction.correction, {50});
          final validBytes = latestHomeBytes(ble);
          for (final change in <void Function(Uint8List)>[
            (bytes) => bytes[62] = 0,
            (bytes) {
              bytes[7] |= 0x40;
              bytes[64] = 1;
            },
            (bytes) => bytes[6] |= 0x20,
          ]) {
            final invalid = Uint8List.fromList(validBytes);
            change(invalid);
            expect(HomeStateWireCodec.tryDecode(invalid, sequence: 1), isNull);
          }
          final rotated = Uint8List.fromList(validBytes)..[6] |= 1;
          expect(HomeStateWireCodec.tryDecode(rotated, sequence: 1), isNotNull);
          await tap(6);
          await checkpoint('c2-blocked-o', HomeStateAction.correction, {50});
          final disconnecting = service.disconnect();
          await ble.flush(tester);
          await disconnecting;
          expect(service.latestHomeState, isNull);
          final reconnecting = service.connect(boardId);
          await ble.flush(tester);
          expect(await reconnecting, isTrue);
          await ble.advance(tester, 100);
          // This test mounts Play directly. Its caller normally completes the
          // current board readiness check before exposing live guidance.
          expect(
            await ble.completeTransaction(
              tester,
              service.ensureProductReady(boardId),
            ),
            isTrue,
          );
          await ble.advanceUntil(tester, () {
            final board = tester
                .widgetList<HomeChessboard>(
                  find.byType(HomeChessboard, skipOffstage: false),
                )
                .singleOrNull;
            return board != null &&
                board.liveGuidanceAvailable &&
                board.snapshot.homeStateAction == HomeStateAction.correction;
          }, 'Play hydrates the current correction after reconnect');
          final currentAfterReconnect = service.latestHomeState;
          for (final packet in retired) {
            ble.deliver(packet, lateCallback: true);
          }
          await tester.pump();
          expect(
            identical(service.latestHomeState, currentAfterReconnect),
            isTrue,
          );
          await checkpoint('c2-reconnected', HomeStateAction.correction, {50});
          physical |= BigInt.one << 50;
          await hall();
          await ble.advance(tester, 2000);
          await checkpoint('c2-restored', HomeStateAction.promote, {});
          physical &= ~(BigInt.one << 1);
          await hall();
          await ble.advance(tester, 2000);
          await checkpoint('replace-b8', HomeStateAction.promote, {1});
          await tap(6);
          await checkpoint('b8-blocked-o', HomeStateAction.promote, {1});
          physical |= BigInt.one << 1;
          await hall();
          await ble.advance(tester, 2000);
          await checkpoint('b8-restored', HomeStateAction.promote, {});
          await tap(6);
          await verifyConfirmation();
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump();
        } finally {
          semantics.dispose();
          final disconnecting = service.disconnect();
          await ble.flush(tester);
          await disconnecting;
          await tester.pumpWidget(const SizedBox.shrink());
        }
      },
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
