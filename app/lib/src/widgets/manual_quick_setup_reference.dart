import 'package:flutter/material.dart';

import '../../l10n/generated/app_localizations.dart';
import 'litch_workspace_geometry.dart';

typedef QuickSetupItem = ({String label, List<String> pixels, String? detail});

// In physical Left/Right order (firmware quick_settings_items.hpp).
// Glyphs mirror menu_icons_6x6.hpp as seen from the end using Quick Setup.
List<QuickSetupItem> quickSetupReference(AppLocalizations strings) => [
  (
    label: strings.manualQuickCpuLevel,
    pixels: const ['.BBBB.', 'B.RR.B', 'B.RC.B', 'B.RR.B', 'B..W.B', '.BBBB.'],
    detail: null,
  ),
  (
    label: strings.manualQuickCpuPlayer,
    pixels: const ['..C...', '..C...', '.WWWW.', 'W.BB.W', 'W....W', '.WWWW.'],
    detail: null,
  ),
  (
    label: strings.manualQuickPlayerEnd,
    pixels: const ['BBBBBB', '..W...', '.WWW..', '..W...', '.WWW..', 'RRRRRR'],
    detail: null,
  ),
  (
    label: strings.brightness,
    pixels: const ['..Y...', '.YWWY.', 'YWWWWY', 'YWWWWY', '.YWWY.', '...Y..'],
    detail: null,
  ),
  (
    label: strings.lightSquares,
    // The firmware lights the alternating squares in place, with no glyph.
    pixels: const [
      'W.W.W.W.',
      '.W.W.W.W',
      'W.W.W.W.',
      '.W.W.W.W',
      'W.W.W.W.',
      '.W.W.W.W',
      'W.W.W.W.',
      '.W.W.W.W',
    ],
    detail: strings.manualQuickLightPreview,
  ),
  (
    label: strings.rotateBoard,
    pixels: const ['..G...', '.GGG..', 'G..M.M', '..M..M', '..MMM.', '...M..'],
    detail: null,
  ),
  (
    label: strings.manualClockTitle,
    pixels: const ['..WW..', '.W..W.', 'W.YY.W', 'W..Y.W', '.W..W.', '..WW..'],
    detail: null,
  ),
  (
    label: 'Bluetooth',
    pixels: const ['..BB..', 'B.B.B.', '.BBB..', '.BBB..', 'B.B.B.', '..BB..'],
    detail: null,
  ),
];

class ManualQuickSetupReference extends StatelessWidget {
  const ManualQuickSetupReference({super.key, required this.strings});

  final AppLocalizations strings;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Semantics(
          header: true,
          child: Text(
            strings.manualQuickSetupHeading,
            style: theme.textTheme.titleMedium,
          ),
        ),
        const SizedBox(height: 8),
        ManualColumns(
          children: [
            for (final item in quickSetupReference(strings))
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 4),
                child: Row(
                  children: [
                    ExcludeSemantics(
                      child: CustomPaint(
                        size: const Size.square(40),
                        painter: _BoardGlyph(item.pixels),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(item.label, style: theme.textTheme.bodyLarge),
                          if (item.detail case final detail?)
                            Text(detail, style: theme.textTheme.bodyMedium),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
          ],
        ),
      ],
    );
  }
}

/// Keeps topic and Quick Setup entries in reading order as the window grows.
class ManualColumns extends StatelessWidget {
  const ManualColumns({super.key, required this.children});
  final List<Widget> children;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) {
      final twoColumns =
          LitchWorkspaceGeometry.isTabletWindow(MediaQuery.sizeOf(context)) &&
          constraints.maxWidth >=
              104 + 30 * MediaQuery.textScalerOf(context).scale(16);
      if (!twoColumns) {
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: children,
        );
      }
      final width = (constraints.maxWidth - 20) / 2;
      return Wrap(
        spacing: 20,
        children: [
          for (final child in children) SizedBox(width: width, child: child),
        ],
      );
    },
  );
}

class _BoardGlyph extends CustomPainter {
  const _BoardGlyph(this.pixels);

  final List<String> pixels;

  static const _colors = {
    'W': Color(0xffffffff),
    'R': Color(0xffff0000),
    'G': Color(0xff00ff00),
    // Boost pure blue against the small black tile for legibility.
    'B': Color(0xff496dff),
    'C': Color(0xff00ffff),
    'M': Color(0xffff00ff),
    'Y': Color(0xffffff00),
  };

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()..color = Colors.black;
    canvas.drawRRect(
      RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(4)),
      paint,
    );
    final pixelSize = size.shortestSide / 8;
    final inset = (8 - pixels.length) ~/ 2;
    for (var row = 0; row < pixels.length; row++) {
      for (var column = 0; column < pixels[row].length; column++) {
        final color = _colors[pixels[row][column]];
        if (color == null) continue;
        paint.color = color;
        canvas.drawRect(
          Rect.fromLTWH(
            (column + inset) * pixelSize + 0.5,
            (row + inset) * pixelSize + 0.5,
            pixelSize - 1,
            pixelSize - 1,
          ),
          paint,
        );
      }
    }
  }

  @override
  bool shouldRepaint(covariant _BoardGlyph oldDelegate) =>
      pixels != oldDelegate.pixels;
}
