import 'package:flutter/material.dart';

/// The board's X, rounded D-pad and O, in their physical left-to-right order.
/// Pair this decorative outline with an accessible control/topic label.
class BoardControlsGlyph extends StatelessWidget {
  const BoardControlsGlyph({
    super.key,
    this.width = 40,
    this.color,
    this.optionalOAvailable = true,
  });

  final double width;
  final Color? color;
  final bool optionalOAvailable;

  @override
  Widget build(BuildContext context) => ExcludeSemantics(
    child: CustomPaint(
      size: Size(width, width * 32 / 60),
      painter: _BoardControlsPainter(
        color ?? Theme.of(context).colorScheme.onSurface,
        optionalOAvailable,
      ),
    ),
  );
}

class _BoardControlsPainter extends CustomPainter {
  const _BoardControlsPainter(this.color, this.optionalOAvailable);

  final Color color;
  final bool optionalOAvailable;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    final scale = size.width / 60;
    canvas.translate(0, (size.height - 32 * scale) / 2);
    canvas.scale(scale);
    final outline = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.6
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    canvas.drawCircle(const Offset(7.5, 16), 6.3, outline);
    canvas.drawLine(const Offset(4.5, 13), const Offset(10.5, 19), outline);
    canvas.drawLine(const Offset(10.5, 13), const Offset(4.5, 19), outline);
    final dpad = Path()
      ..moveTo(28, 4)
      ..lineTo(32, 4)
      ..quadraticBezierTo(34, 4, 34, 6)
      ..lineTo(34, 10)
      ..quadraticBezierTo(34, 12, 36, 12)
      ..lineTo(40, 12)
      ..quadraticBezierTo(42, 12, 42, 14)
      ..lineTo(42, 18)
      ..quadraticBezierTo(42, 20, 40, 20)
      ..lineTo(36, 20)
      ..quadraticBezierTo(34, 20, 34, 22)
      ..lineTo(34, 26)
      ..quadraticBezierTo(34, 28, 32, 28)
      ..lineTo(28, 28)
      ..quadraticBezierTo(26, 28, 26, 26)
      ..lineTo(26, 22)
      ..quadraticBezierTo(26, 20, 24, 20)
      ..lineTo(20, 20)
      ..quadraticBezierTo(18, 20, 18, 18)
      ..lineTo(18, 14)
      ..quadraticBezierTo(18, 12, 20, 12)
      ..lineTo(24, 12)
      ..quadraticBezierTo(26, 12, 26, 10)
      ..lineTo(26, 6)
      ..quadraticBezierTo(26, 4, 28, 4)
      ..close();
    canvas.drawPath(dpad, outline);
    if (!optionalOAvailable) outline.color = color.withValues(alpha: 0.3);
    canvas.drawCircle(const Offset(52.5, 16), 6.3, outline);
    canvas.drawCircle(const Offset(52.5, 16), 3.1, outline);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_BoardControlsPainter oldDelegate) =>
      oldDelegate.color != color ||
      oldDelegate.optionalOAvailable != optionalOAvailable;
}
