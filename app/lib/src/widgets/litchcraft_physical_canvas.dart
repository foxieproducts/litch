import 'package:flutter/material.dart';

import '../models/litchcraft_state.dart';
import 'litchcraft_tools.dart';

/// Read-only physical V3 view. The ordinary editable canvas remains semantic.
class LitchcraftPhysicalCanvas extends StatelessWidget {
  const LitchcraftPhysicalCanvas({super.key, required this.canvas});

  final LitchcraftCanvas canvas;

  @override
  Widget build(BuildContext context) => CustomPaint(
    painter: _PhysicalCanvasPainter(canvas),
    child: const AspectRatio(aspectRatio: 1),
  );
}

class _PhysicalCanvasPainter extends CustomPainter {
  const _PhysicalCanvasPainter(this.frame);
  final LitchcraftCanvas frame;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.black);
    final side = size.width / 8;
    for (var square = 0; square < 64; square++) {
      canvas.save();
      canvas.translate(square % 8 * side, square ~/ 8 * side);
      LitchcraftTargetPainter(
        mask: 15,
        artworkOnly: true,
        topology: LitchcraftTopology.v3,
        flipped: false,
        color: 0,
        emitterColors: frame.colors,
        emitterOffset: square * 4,
      ).paint(canvas, Size.square(side));
      canvas.restore();
    }
  }

  @override
  bool shouldRepaint(_PhysicalCanvasPainter oldDelegate) =>
      !identical(frame, oldDelegate.frame);
}
