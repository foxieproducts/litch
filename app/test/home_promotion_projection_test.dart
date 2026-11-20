import 'dart:convert';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

const preFen = '7k/1P6/8/8/8/8/2P5/K7 w - - 0 1';
final completed =
    (BigInt.one << 7) |
    (BigInt.one << 1) |
    (BigInt.one << 50) |
    (BigInt.one << 56);
String packet({
  String fen = preFen,
  String action = 'promote',
  BigInt? occupancy,
  Map<String, Object?> extra = const {},
}) => jsonEncode({
  'event': 'home_state',
  'v': 1,
  'seq': 1,
  'a': action,
  'fen': fen,
  'occ': (occupancy ?? completed).toRadixString(16).padLeft(16, '0'),
  'g': 'c',
  'duck': '--',
  'out': 'n',
  'win': 'n',
  'rot': 0,
  'white': 1,
  'pm': 0,
  'c': ['b7', 'b8', '--'],
  ...extra,
});
HomeBoardSnapshot snapshot(HomeStateNotification value) => HomeBoardSnapshot(
  position: value.position,
  gameStatus: value.gameStatus,
  physicalStatus: value.physicalStatus,
  homeStateAction: value.action,
  physicalMovePrompt: value.physicalMovePrompt,
  liveSequence: value.sequence,
);

void main() {
  testWidgets(
    'rendered neutral promotion marker remains visible on both square colors through replacement',
    (tester) async {
      tester.view.physicalSize = const Size(430, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      const boundaryKey = ValueKey('promotion-render-boundary');
      final failures = <String>[];
      for (final target in ['b8', 'a8']) {
        final targetIndex = target == 'b8' ? 1 : 0;
        final occupied =
            (completed & ~(BigInt.one << 1)) | (BigInt.one << targetIndex);
        for (final state in ['occupied', 'missing', 'restored']) {
          final value = packet(
            fen: target == 'b8' ? preFen : '7k/P7/8/8/8/8/2P5/K7 w - - 0 1',
            occupancy: state == 'missing'
                ? occupied & ~(BigInt.one << targetIndex)
                : occupied,
            extra: {
              'c': [target == 'b8' ? 'b7' : 'a7', target, '--'],
            },
          );
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              home: RepaintBoundary(
                key: boundaryKey,
                child: Scaffold(
                  body: HomeChessboard(
                    snapshot: snapshot(HomeStateNotification.tryParse(value)!),
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final marker = tester.getRect(
            find.byKey(ValueKey('physical-piece-$target')),
          );
          final boundary = tester.renderObject<RenderRepaintBoundary>(
            find.byKey(boundaryKey),
          );
          final origin = boundary.localToGlobal(Offset.zero);
          final raster = await tester.runAsync(() async {
            final image = await boundary.toImage(pixelRatio: 1);
            final bytes = await image.toByteData(
              format: ui.ImageByteFormat.rawRgba,
            );
            final result = (image.width, bytes!);
            image.dispose();
            return result;
          });
          Color pixel(Offset point) {
            final offset =
                (point.dy.round() * raster!.$1 + point.dx.round()) * 4;
            final bytes = raster.$2;
            return Color.fromARGB(
              bytes.getUint8(offset + 3),
              bytes.getUint8(offset),
              bytes.getUint8(offset + 1),
              bytes.getUint8(offset + 2),
            );
          }

          final center = marker.center - origin;
          final background = pixel(center).computeLuminance();
          final contrasts = <double>[];
          for (final direction in const [
            Offset(1, 0),
            Offset(-1, 0),
            Offset(0, 1),
            Offset(0, -1),
          ]) {
            // Sample the actual two-pixel ring, allowing fractional raster edges.
            var strongest = 1.0;
            for (final radius in [10.0, 10.5, 11.0, 11.5]) {
              final stroke = pixel(
                center + direction * radius,
              ).computeLuminance();
              final contrast = stroke > background
                  ? (stroke + 0.05) / (background + 0.05)
                  : (background + 0.05) / (stroke + 0.05);
              if (contrast > strongest) strongest = contrast;
            }
            contrasts.add(strongest);
          }
          final threshold = state == 'missing' ? 1.4 : 3.0;
          // The missing-piece ghost stays visible; restored occupancy is full contrast.
          // ignore: avoid_print
          print(
            'promotion marker $target $state rendered contrasts=$contrasts',
          );
          if (contrasts.any((contrast) => contrast < threshold)) {
            failures.add('$target $state: $contrasts (minimum $threshold)');
          }
        }
      }
      expect(failures, isEmpty, reason: failures.join('\n'));
    },
  );

  test(
    'pending promotion projects physical presence without committing piece identity',
    () {
      final healthy = HomeStateNotification.tryParse(packet())!;
      final board = snapshot(healthy);
      expect(board.position!.pieces[9], 'P');
      expect(board.position!.pieces[1], isNull);
      expect(board.expectedOccupiedSquares, completed);
      expect(board.mismatchedSquares, isEmpty);
      expect(board.physicalPieceAt(9), isNull);
      expect(board.physicalPieceAt(1), isNull);
      expect(board.pendingPromotionPrompt?.toSquare, 1);
      expect(board.promotionPlacementPending, isFalse);
      final correction = snapshot(
        HomeStateNotification.tryParse(
          packet(action: 'corr', occupancy: completed & ~(BigInt.one << 50)),
        )!,
      );
      expect(correction.mismatchedSquares, {50});
      expect(correction.missingExpectedSquares, {50});
      expect(correction.unexpectedOccupiedSquares, isEmpty);
      expect(correction.primaryAssistanceText, 'P1 pawn expected on c2');
      final replacement = snapshot(
        HomeStateNotification.tryParse(
          packet(occupancy: completed & ~(BigInt.one << 1)),
        )!,
      );
      expect(replacement.mismatchedSquares, {1});
      expect(replacement.promotionPlacementPending, isTrue);
      expect(replacement.missingExpectedSquares, {1});
      expect(replacement.unexpectedOccupiedSquares, isEmpty);
      expect(
        board.copyWith(clearLiveState: true).pendingPromotionPrompt,
        isNull,
      );
    },
  );

  test(
    'capture promotion carries no victim-removal instruction or guessed replacement',
    () {
      final board = snapshot(
        HomeStateNotification.tryParse(
          packet(
            fen: 'r6k/1P6/8/8/8/8/2P5/K7 w - - 0 1',
            occupancy: (completed & ~(BigInt.one << 1)) | BigInt.one,
            extra: {
              'c': ['b7', 'a8', '--'],
            },
          ),
        )!,
      );
      expect(board.position!.pieces[0], 'r');
      expect(board.physicalPieceAt(0), isNull);
      expect(board.physicalPieceAt(9), isNull);
      expect(board.captureRemovalSquare, isNull);
      expect(board.mismatchedSquares, isEmpty);
    },
  );

  test(
    'Black and Duck promotion projections retain turn and unrelated occupancy',
    () {
      final black = snapshot(
        HomeStateNotification.tryParse(
          packet(
            fen: '7k/8/8/8/8/8/1p6/K7 b - - 0 1',
            occupancy:
                (BigInt.one << 7) | (BigInt.one << 56) | (BigInt.one << 57),
            extra: {
              'c': ['b2', 'b1', '--'],
              'rot': 1,
            },
          ),
        )!,
      );
      expect(black.pendingPromotionPrompt?.toSquare, 57);
      expect(black.mismatchedSquares, isEmpty);
      expect(black.position!.sideToMove, HomeSide.black);
      final duck = snapshot(
        HomeStateNotification.tryParse(
          packet(
            occupancy: completed | (BigInt.one << 39),
            extra: {'g': 'd', 'duck': 'h4'},
          ),
        )!,
      );
      expect(duck.expectedOccupiedSquares, completed | (BigInt.one << 39));
      expect(duck.mismatchedSquares, isEmpty);
    },
  );

  test(
    'new compact context rejects foreign, terminal, Undo and legacy aliases',
    () {
      for (final extra in <Map<String, Object?>>[
        {
          'c': ['h8', 'b8', '--'],
        },
        {
          'c': ['b7', 'b6', '--'],
        },
        {
          'c': ['b7', 'b8', 'b8'],
        },
        {'pm': 1},
        {'up': 1},
        {'fs': 1},
        {'vx': 1},
        {'kq': 1},
        {'out': 'r', 'win': 'b'},
        {'cf': 'b7', 'ct': 'b8', 'cr': '--'},
        {
          'th': ['w', 2, null],
        },
        {'dr': 'hh4--'},
      ]) {
        expect(
          HomeStateNotification.tryParse(packet(extra: extra)),
          isNull,
          reason: '$extra',
        );
      }
      expect(
        HomeStateNotification.tryParse(
          packet(fen: preFen.replaceFirst(' w ', ' b ')),
        ),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(
          packet(
            action: 'corr',
            extra: {
              'c': ['a1', 'b1', '--'],
            },
          ),
        ),
        isNull,
      );
    },
  );

  testWidgets(
    'physical plan drives neutral marker and accurate correction semantics at 200%',
    (tester) async {
      tester.view.physicalSize = const Size(430, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      try {
        Future<void> show(String value) => tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: HomeChessboard(
                snapshot: snapshot(HomeStateNotification.tryParse(value)!),
              ),
            ),
          ),
        );
        await show(
          packet(action: 'corr', occupancy: completed & ~(BigInt.one << 50)),
        );
        await tester.pump();
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-c2'))).label,
          'P1 pawn expected on c2',
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-b7'))).label,
          'b7, empty',
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-b8'))).label,
          'b8, occupied by a piece',
        );
        expect(find.byKey(const ValueKey('physical-piece-b8')), findsOneWidget);
        expect(
          find.byKey(const ValueKey('unexpected-occupied-b8')),
          findsNothing,
        );
        await show(packet(occupancy: completed & ~(BigInt.one << 1)));
        await tester.pump();
        expect(
          tester.getSemantics(find.byKey(const ValueKey('square-b8'))).label,
          'Place the promotion piece on b8.',
        );
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );
}
