import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/widgets/board_controls_glyph.dart';

class _OutlineCanvas implements Canvas {
  Path? outline;
  double? scaleX;
  double? scaleY;
  @override
  void drawPath(Path path, Paint paint) {
    outline = path;
  }

  @override
  void scale(double sx, [double? sy]) {
    scaleX = sx;
    scaleY = sy ?? sx;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => null;
}

void main() {
  testWidgets('Shared board D-pad is symmetric in every direction and size', (
    tester,
  ) async {
    for (final width in [40.0, 60.0, 120.0]) {
      await tester.pumpWidget(
        MaterialApp(
          home: Center(child: BoardControlsGlyph(width: width)),
        ),
      );
      final paint = tester.widget<CustomPaint>(
        find.descendant(
          of: find.byType(BoardControlsGlyph),
          matching: find.byType(CustomPaint),
        ),
      );
      final canvas = _OutlineCanvas();
      paint.painter!.paint(canvas, Size(width, width * 32 / 60));
      final bounds = canvas.outline!.getBounds();
      expect(
        bounds.width,
        bounds.height,
        reason: 'D-pad must have equal physical horizontal and vertical reach',
      );
      expect(
        canvas.scaleX,
        canvas.scaleY,
        reason: 'The painter must not distort those physical proportions',
      );
      final center = bounds.center;
      // Compare the painted outline after a quarter turn at many interior points.
      for (double x = -14; x <= 14; x += .5) {
        for (double y = -14; y <= 14; y += .5) {
          expect(
            canvas.outline!.contains(center + Offset(x, y)),
            canvas.outline!.contains(center + Offset(-y, x)),
            reason: 'Every D-pad arm must have identical geometry',
          );
        }
      }
      expect(tester.takeException(), isNull);
    }
  });
}
