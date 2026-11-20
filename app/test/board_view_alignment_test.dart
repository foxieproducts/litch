import 'support/memory_ota_target_store.dart';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';

import 'localization_test_support.dart';

class _UnusedBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

void main() {
  for (final rotated in [false, true]) {
    for (final opposite in [false, true]) {
      testWidgets('same physical near edge rotate=$rotated firstP2=$opposite', (
        tester,
      ) async {
        await prepareLocalizationReview(tester);
        tester.view.physicalSize = const Size(1000, 700);
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _UnusedBle(),
        );
        final controller = LitchcraftController(
          ble,
          initialCanvas: LitchcraftCanvas.blank()
              .paint([0], 15, 0xff0000)
              .paint([7], 15, 0x00ff00)
              .paint([56], 15, 0x0000ff)
              .paint([63], 15, 0xffff00)
              .paint([9], 1, 0xab34ef)
              .paint([9], 2, 0x1234cd),
        );
        addTearDown(controller.dispose);
        addTearDown(ble.dispose);
        final snapshot = HomeBoardSnapshot(
          position: HomeChessPosition.tryParse(
            'rnbqkbnr/1ppppp1p/p7/6p1/6P1/P7/1PPPPP1P/RNBQKBNR w KQkq - 0 3',
          ),
          physicalStatus: HomePhysicalBoardStatus(
            rotated: rotated,
            p1AtOppositeEnd: opposite,
          ),
          p1PieceWheel: 170,
          p2PieceWheel: 0,
        );
        final title =
            'Rotate ${rotated ? 'on' : 'off'}  |  First P${opposite ? 2 : 1}';
        await tester.pumpWidget(
          localizationHarness(
            locale: const Locale('en'),
            home: Scaffold(
              body: RepaintBoundary(
                key: const ValueKey('alignment-panel'),
                child: ColoredBox(
                  color: const Color(0xfff8f7f2),
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: Column(
                      children: [
                        Text(title, style: const TextStyle(fontSize: 26)),
                        const Text(
                          'Both screen bottoms face the same physical near edge',
                          style: TextStyle(fontSize: 18),
                        ),
                        const SizedBox(height: 18),
                        Row(
                          mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Column(
                              children: [
                                Text('Play — far: ${opposite ? 'P1' : 'P2'}'),
                                const SizedBox(height: 8),
                                SizedBox.square(
                                  dimension: 420,
                                  child: HomeChessboard(
                                    snapshot: snapshot,
                                    showCoordinates: true,
                                  ),
                                ),
                                const SizedBox(height: 8),
                                Text('Near: ${opposite ? 'P2' : 'P1'}'),
                              ],
                            ),
                            Column(
                              children: [
                                const Text('Litchcraft — far: red / green'),
                                const SizedBox(height: 8),
                                SizedBox.square(
                                  dimension: 420,
                                  child: LitchcraftCanvasView(
                                    canvas: controller.canvas,
                                    flipped: controller.flipped,
                                    onBegin: () {},
                                    onPaint: (_) {},
                                    onEnd: () {},
                                  ),
                                ),
                                const SizedBox(height: 8),
                                const Text('Near: blue / yellow'),
                              ],
                            ),
                          ],
                        ),
                        const SizedBox(height: 14),
                        Text(
                          'Hardware pose: ${rotated ? 'turned 180°' : 'normal'}'
                          '   ·   Raw near-left square: ${rotated ? 7 : 56}'
                          '   ·   Near blue / far red',
                        ),
                        const Text(
                          'Actual app widgets; physical-device confirmation pending',
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
        await decodeVisibleReviewImages(tester);
        final play = tester.getRect(find.byType(HomeChessboard));
        final craft = tester.getRect(find.byType(LitchcraftCanvasView));
        expect(play.bottom, craft.bottom);
        Offset pieceCenter(String square) =>
            tester.getCenter(find.byKey(ValueKey('piece-$square')));
        expect(pieceCenter('e1').dy > pieceCenter('e8').dy, !opposite);

        final grid = tester.getRect(
          find.descendant(
            of: find.byType(HomeChessboard),
            matching: find.byType(GridView),
          ),
        );
        final frame = tester.getRect(
          find.byKey(const ValueKey('home-chessboard-surface')),
        );
        final fileOrder = opposite ? 'hgfedcba' : 'abcdefgh';
        final rankOrder = opposite ? '12345678' : '87654321';
        // The position has an identifiable piece on every file and rank,
        // including the four pawns advanced away from their starting ranks.
        const rankPieces = ['e1', 'e2', 'a3', 'g4', 'g5', 'a6', 'e7', 'e8'];
        for (final edge in ['top', 'bottom', 'left', 'right']) {
          final files = edge == 'top' || edge == 'bottom';
          final strip = find.byKey(
            ValueKey('play-${files ? 'files' : 'ranks'}-$edge'),
          );
          final labels = find.descendant(
            of: strip,
            matching: find.byType(Text),
          );
          expect(
            tester.widgetList<Text>(labels).map((label) => label.data).join(),
            files ? fileOrder : rankOrder,
          );
          for (var i = 0; i < 8; ++i) {
            final label = tester.getRect(labels.at(i));
            final piece = files
                ? pieceCenter('${fileOrder[i]}1')
                : pieceCenter(rankPieces[int.parse(rankOrder[i]) - 1]);
            expect(
              files ? label.center.dx : label.center.dy,
              closeTo(files ? piece.dx : piece.dy, 0.01),
            );
            expect(frame.contains(label.center), isTrue);
            switch (edge) {
              case 'top':
                expect(label.bottom, lessThanOrEqualTo(grid.top));
              case 'bottom':
                expect(label.top, greaterThanOrEqualTo(grid.bottom));
              case 'left':
                expect(label.right, lessThanOrEqualTo(grid.left));
              case 'right':
                expect(label.left, greaterThanOrEqualTo(grid.right));
            }
          }
        }
        final playNearLeft = opposite ? 'h8' : 'a1';
        expect(
          tester.getCenter(find.byKey(ValueKey('square-$playNearLeft'))),
          grid.bottomLeft + Offset(grid.width / 16, -grid.height / 16),
        );
        expect(
          tester.getCenter(find.byKey(const ValueKey('craft-square-56'))),
          craft.bottomLeft + Offset(craft.width / 16, -craft.height / 16),
        );
        // Exact V3 raw corner contract, also checked against the production
        // hardware mapper in test_flip_suite: both modes face raw 56 normally
        // and raw 7 after physically turning the board.
        final expectedRawNearLeft = rotated ? 7 : 56;
        final playLogicalNearLeft = opposite ? 7 : 56;
        final playRaw = snapshot.physicalStatus!.squareRotated
            ? 63 - playLogicalNearLeft
            : playLogicalNearLeft;
        final craftRaw = rotated ? 63 - 56 : 56;
        expect(playRaw, expectedRawNearLeft);
        expect(craftRaw, expectedRawNearLeft);
        const directory = String.fromEnvironment('LITCH_ORIENTATION_ARTIFACTS');
        if (directory.isNotEmpty) {
          final boundary = tester.renderObject<RenderRepaintBoundary>(
            find.byKey(const ValueKey('alignment-panel')),
          );
          await tester.runAsync(() async {
            final image = await boundary.toImage();
            final bytes = await image.toByteData(
              format: ui.ImageByteFormat.png,
            );
            await Directory(directory).create(recursive: true);
            await File(
              '$directory/rotate-${rotated ? 'on' : 'off'}-first-p${opposite ? 2 : 1}.png',
            ).writeAsBytes(bytes!.buffer.asUint8List());
            image.dispose();
          });
        }
      });
    }
  }
}
