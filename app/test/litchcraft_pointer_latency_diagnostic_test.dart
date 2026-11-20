import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';

void main() {
  for (final flipped in [false, true]) {
    testWidgets('crossing a cell boundary below touch slop paints while held flipped=$flipped', (tester) async {
      final painted = <int>{};
      var began = 0, erased = 0;
      await tester.pumpWidget(MaterialApp(home: Scaffold(body: Center(
        child: SizedBox.square(dimension: 320, child: LitchcraftCanvasView(
          canvas: LitchcraftCanvas.blank(), flipped: flipped,
          onBegin: () => began++, onEnd: () {}, onPaint: painted.addAll,
          onEraseSquare: (_) => erased++, onRotateQuarterTurn: (_) {},
        )),
      ))));
      final rect = tester.getRect(find.byKey(const ValueKey('litchcraft-canvas')));
      final width = rect.width / 8;
      final from = rect.topLeft + Offset(width - 2, width / 2);
      final intoNext = rect.topLeft + Offset(width + 2, width / 2);
      final logicalNext = litchcraftLogicalSquareForDisplay(1, flipped: flipped);
      final pointer = await tester.startGesture(from);
      try {
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveTo(intoNext);
        await tester.pump(const Duration(milliseconds: 16));
        debugPrint('POINTER_LATENCY_DIAG flipped=$flipped travel=4 held=true began=$began painted=$painted entered=$logicalNext erased=$erased');
        expect(painted, contains(logicalNext), reason: 'Entering another square must paint it on the next local display frame while the finger remains held; no BLE, receipt, or 18px movement gate is involved.');
        expect(erased, 0);
      } finally {
        await pointer.up();
        await tester.pump();
        await tester.pumpWidget(const SizedBox.shrink());
      }
    });
  }
}
