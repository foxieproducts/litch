import 'package:flutter/material.dart';

/// Exact CPU_PLAYER_ICON from include/modules/menu_icons_6x6.hpp.
/// The dark backing keeps its white outline readable in either app theme.
class CpuThinkingRobot extends StatelessWidget {
  const CpuThinkingRobot({super.key});

  static const List<String> pixels = <String>[
    '..C...',
    '..C...',
    '.WWWW.',
    'W.BB.W',
    'W....W',
    '.WWWW.',
  ];

  @override
  Widget build(BuildContext context) => ExcludeSemantics(
    child: SizedBox(
      width: 36,
      height: 36,
      child: CustomPaint(painter: _RobotPainter()),
    ),
  );
}

class _RobotPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(
      Offset.zero & size,
      Paint()..color = const Color(0xFF202A2C),
    );
    for (int y = 0; y < 6; ++y) {
      for (int x = 0; x < 6; ++x) {
        final Color? color = switch (CpuThinkingRobot.pixels[y][x]) {
          'C' => const Color(0xFF00FFFF),
          'W' => const Color(0xFFFFFFFF),
          'B' => const Color(0xFF0000FF),
          _ => null,
        };
        if (color != null) {
          canvas.drawRect(
            Rect.fromLTWH(
              x * size.width / 6,
              y * size.height / 6,
              size.width / 6,
              size.height / 6,
            ),
            Paint()
              ..color = color
              ..isAntiAlias = false,
          );
        }
      }
    }
  }

  @override
  bool shouldRepaint(_RobotPainter oldDelegate) => false;
}
