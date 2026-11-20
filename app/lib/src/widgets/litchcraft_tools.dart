import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import 'compact_controls.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';

import '../localization/litch_strings.dart';
import '../models/litch_piece_color.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_state.dart';
import 'color_slider.dart';

const litchcraftPalette = litchcraftDefaultFavoriteColors;

Color _previewHsv(HSVColor color) =>
    litchcraftPreviewColor(color.toColor().toARGB32());

LinearGradient _previewGradient(
  HSVColor Function(double) sample, {
  int steps = 36,
}) => LinearGradient(
  colors: [
    for (var step = 0; step <= steps; step++) _previewHsv(sample(step / steps)),
  ],
);

final _previewHueGradient = _previewGradient(
  (fraction) => HSVColor.fromAHSV(1, fraction * 360, 1, 1),
  steps: 120,
);

class _ColorFieldPainter extends CustomPainter {
  const _ColorFieldPainter(this.hue, {this.minimumValue = 0});

  final double hue;
  final double minimumValue;

  @override
  void paint(Canvas canvas, Size size) {
    const steps = 16;
    final positions = <Offset>[];
    final colors = <Color>[];
    final indices = <int>[];
    double squaredFraction(int step) => (step * step) / (steps * steps);
    // The transfer bends most near zero. Concentrate samples near dark and
    // fully saturated colors, where one or more raw channels approach zero.
    final values = [
      for (var step = steps; step >= 0; step--) squaredFraction(step),
    ];
    if (minimumValue > 0) {
      values.add(minimumValue);
      values.sort((a, b) => b.compareTo(a));
    }
    for (var row = 0; row < values.length; row++) {
      final fraction = 1 - values[row];
      final value = math.max(minimumValue, values[row]);
      for (var column = 0; column <= steps; column++) {
        final saturation = 1 - squaredFraction(steps - column);
        positions.add(Offset(saturation * size.width, fraction * size.height));
        colors.add(_previewHsv(HSVColor.fromAHSV(1, hue, saturation, value)));
        if (row == values.length - 1 || column == steps) continue;
        final topLeft = row * (steps + 1) + column;
        final bottomLeft = topLeft + steps + 1;
        indices.addAll([
          topLeft,
          topLeft + 1,
          bottomLeft,
          topLeft + 1,
          bottomLeft + 1,
          bottomLeft,
        ]);
      }
    }
    final vertices = ui.Vertices(
      ui.VertexMode.triangles,
      positions,
      colors: colors,
      indices: indices,
    );
    canvas.drawVertices(vertices, BlendMode.dst, Paint());
    vertices.dispose();
  }

  @override
  bool shouldRepaint(_ColorFieldPainter oldDelegate) =>
      hue != oldDelegate.hue || minimumValue != oldDelegate.minimumValue;
}

class _ColorFieldSelection extends StatelessWidget {
  const _ColorFieldSelection({
    required this.color,
    required this.width,
    required this.height,
  });

  static const diameter = 40.0;
  final HSVColor color;
  final double width, height;

  @override
  Widget build(BuildContext context) => Positioned(
    left: (color.saturation * width - diameter / 2).clamp(
      0,
      math.max(0, width - diameter),
    ),
    top: ((1 - color.value) * height - diameter / 2).clamp(
      0,
      math.max(0, height - diameter),
    ),
    child: Container(
      width: diameter,
      height: diameter,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        border: Border.all(color: Colors.white, width: 2),
        boxShadow: const <BoxShadow>[
          BoxShadow(color: Colors.black54, blurRadius: 2),
        ],
      ),
    ),
  );
}

/// A tiny chessboard: filled alternating squares, not an app-grid outline.
class LitchPlayGlyph extends StatelessWidget {
  const LitchPlayGlyph({super.key, this.size = 24});
  final double size;
  @override
  Widget build(BuildContext context) => CustomPaint(
    size: Size.square(size),
    painter: _LitchPlayPainter(Theme.of(context).brightness == Brightness.dark),
  );
}

class _LitchPlayPainter extends CustomPainter {
  const _LitchPlayPainter(this.dark);
  final bool dark;
  @override
  void paint(Canvas canvas, Size size) {
    final unit = size.width / 24;
    canvas.save();
    canvas.scale(unit);
    for (var row = 0; row < 2; row++) {
      for (var column = 0; column < 2; column++) {
        final left = 1.0 + column * 12;
        final top = 1.0 + row * 12;
        final path = Path()
          ..moveTo(left + 1.5, top)
          ..lineTo(left + 8.5, top)
          ..lineTo(left + 10, top + 1.5)
          ..lineTo(left + 10, top + 8.5)
          ..lineTo(left + 8.5, top + 10)
          ..lineTo(left + 1.5, top + 10)
          ..lineTo(left, top + 8.5)
          ..lineTo(left, top + 1.5)
          ..close();
        canvas.drawPath(
          path,
          Paint()
            ..color = (row + column).isEven
                ? (dark ? const Color(0xffd4e3df) : const Color(0xffa8c2ba))
                : const Color(0xff3d5b53),
        );
      }
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_LitchPlayPainter oldDelegate) => dark != oldDelegate.dark;
}

/// Board quick-palette indices retain Arduino hue samples 0, 8, …, 248.
/// Normalize their peak channel so intermediate hues are as bright as primaries.
final List<int> litchcraftPaletteRailColors = List<int>.unmodifiable(
  List<int>.generate(32, (index) {
    final rgb = litchWheelToColor(index * 8).toARGB32();
    final channels = [(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255];
    final peak = channels.reduce(math.max);
    return channels.fold(
      0,
      (color, channel) => (color << 8) | ((channel * 255 + peak ~/ 2) ~/ peak),
    );
  }),
);

/// An endless browsing rail. Scrolling never selects or rewrites brush history.
class LitchcraftPaletteRail extends StatefulWidget {
  const LitchcraftPaletteRail({
    super.key,
    required this.color,
    required this.enabled,
    required this.onSelected,
    this.onLongPress,
    this.axis = Axis.vertical,
  });

  final int color;
  final bool enabled;
  final ValueChanged<int> onSelected;
  final ValueChanged<int>? onLongPress;
  final Axis axis;

  @override
  State<LitchcraftPaletteRail> createState() => _LitchcraftPaletteRailState();
}

class _LitchcraftPaletteRailState extends State<LitchcraftPaletteRail> {
  late final FixedExtentScrollController _scroll = FixedExtentScrollController(
    initialItem: _closestColor(widget.color),
  );

  int _closestColor(int color) {
    int distance(int candidate) => [0, 8, 16].fold<int>(0, (sum, shift) {
      final delta = ((candidate >> shift) & 255) - ((color >> shift) & 255);
      return sum + delta * delta;
    });
    var closest = 0;
    for (var index = 1; index < litchcraftPaletteRailColors.length; index++) {
      if (distance(litchcraftPaletteRailColors[index]) <
          distance(litchcraftPaletteRailColors[closest])) {
        closest = index;
      }
    }
    return closest;
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => GestureDetector(
    // This region owns horizontal drags too; they must not turn the app page.
    onHorizontalDragStart: widget.axis == Axis.vertical ? (_) {} : null,
    onHorizontalDragUpdate: widget.axis == Axis.vertical ? (_) {} : null,
    onVerticalDragStart: widget.axis == Axis.horizontal ? (_) {} : null,
    onVerticalDragUpdate: widget.axis == Axis.horizontal ? (_) {} : null,
    child: RotatedBox(
      quarterTurns: widget.axis == Axis.horizontal ? 3 : 0,
      child: ListWheelScrollView.useDelegate(
        controller: _scroll,
        itemExtent: 48,
        diameterRatio: 100000,
        perspective: .00001,
        physics: const FixedExtentScrollPhysics(),
        childDelegate: ListWheelChildLoopingListDelegate(
          children: [
            for (
              var index = 0;
              index < litchcraftPaletteRailColors.length;
              index++
            )
              Builder(
                builder: (context) {
                  final rgb = litchcraftPaletteRailColors[index];
                  final preview = litchcraftPreviewColor(rgb);
                  final selected = widget.color == rgb;
                  final label =
                      '${_litchcraftColorName(context, rgb)} · #${rgb.toRadixString(16).padLeft(6, '0').toUpperCase()}';
                  return RotatedBox(
                    quarterTurns: widget.axis == Axis.horizontal ? 1 : 0,
                    child: Semantics(
                      selected: selected,
                      button: true,
                      enabled: widget.enabled,
                      label: label,
                      excludeSemantics: true,
                      onTap: widget.enabled
                          ? () => widget.onSelected(rgb)
                          : null,
                      onLongPress: LitchcraftHoldAction.withCompletionFeedback(
                        context,
                        widget.enabled && widget.onLongPress != null
                            ? () => widget.onLongPress!(rgb)
                            : null,
                      ),
                      child: LitchcraftHoldAction(
                        key: ValueKey('craft-wheel-color-$index'),
                        duration: kLongPressTimeout,
                        holdLabel: litchStrings(context).craftFillCanvas,
                        completedLabel: '',
                        showCompletionSnackBar: false,
                        reserveProgressSpace: false,
                        onTap: widget.enabled
                            ? () => widget.onSelected(rgb)
                            : null,
                        onHold: widget.enabled && widget.onLongPress != null
                            ? () => widget.onLongPress!(rgb)
                            : null,
                        child: IgnorePointer(
                          child: IconButton(
                            tooltip: label,
                            onPressed: widget.enabled
                                ? () => widget.onSelected(rgb)
                                : null,
                            style: IconButton.styleFrom(
                              fixedSize: const Size.square(48),
                              padding: const EdgeInsets.all(8),
                            ),
                            icon: Container(
                              width: 32,
                              height: 32,
                              decoration: BoxDecoration(
                                shape: BoxShape.circle,
                                color: preview,
                                border: selected
                                    ? Border.all(color: Colors.white, width: 2)
                                    : null,
                              ),
                              child: selected
                                  ? Icon(
                                      Icons.check,
                                      size: 16,
                                      color: litchSwatchMarkColor(preview),
                                    )
                                  : null,
                            ),
                          ),
                        ),
                      ),
                    ),
                  );
                },
              ),
          ],
        ),
      ),
    ),
  );
}

enum LitchcraftSecondaryTray { palette, leds, effects, animation }

/// Owns the single adjacent tray selected from the primary Litchcraft toolbar.
/// Keeping this policy in one small controller makes future launchers mutually
/// exclusive without teaching the canvas about individual tools.
class LitchcraftSecondaryTrayController extends ChangeNotifier {
  LitchcraftSecondaryTray? _active;

  LitchcraftSecondaryTray? get active => _active;

  void toggle(LitchcraftSecondaryTray tray) {
    if (_active == tray) return;
    _active = tray;
    notifyListeners();
  }

  void show(LitchcraftSecondaryTray tray) {
    if (_active == tray) return;
    _active = tray;
    notifyListeners();
  }

  void close() {
    if (_active == null) return;
    _active = null;
    notifyListeners();
  }
}

class LitchcraftSecondaryToolbarHost extends StatelessWidget {
  const LitchcraftSecondaryToolbarHost({
    super.key,
    required this.axis,
    required this.activeTray,
    required this.child,
  });

  final Axis axis;
  final LitchcraftSecondaryTray activeTray;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final reduceMotion = MediaQuery.disableAnimationsOf(context);
    final duration = reduceMotion
        ? Duration.zero
        : const Duration(milliseconds: 140);
    final base = Theme.of(context);
    return Theme(
      data: base.copyWith(
        iconTheme: const IconThemeData(color: Colors.white),
        iconButtonTheme: IconButtonThemeData(
          style: IconButton.styleFrom(foregroundColor: Colors.white),
        ),
      ),
      child: AnimatedSwitcher(
        key: const ValueKey('craft-secondary-toolbar-host'),
        duration: duration,
        reverseDuration: duration,
        switchInCurve: Curves.easeOutCubic,
        switchOutCurve: Curves.easeInCubic,
        transitionBuilder: (child, animation) => FadeTransition(
          opacity: animation,
          child: SlideTransition(
            position: Tween<Offset>(
              begin: axis == Axis.vertical
                  ? const Offset(.10, 0)
                  : const Offset(0, .10),
              end: Offset.zero,
            ).animate(animation),
            child: child,
          ),
        ),
        child: Material(
          key: ValueKey('craft-secondary-${activeTray.name}'),
          color: const Color(0xff33494e),
          clipBehavior: Clip.antiAlias,
          child: child,
        ),
      ),
    );
  }
}

/// Product identity mark: four differently colored chamfered blocks form an L
/// inside a palette silhouette, accompanied by exactly three round swatches.
class LitchcraftIdentityGlyph extends StatelessWidget {
  const LitchcraftIdentityGlyph({super.key, this.size = 24});

  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('litchcraft-identity-glyph'),
    size: Size.square(size),
    painter: _LitchcraftIdentityGlyphPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
    ),
  );
}

class _LitchcraftIdentityGlyphPainter extends CustomPainter {
  const _LitchcraftIdentityGlyphPainter(this.silhouetteColor);

  final Color silhouetteColor;

  @override
  void paint(Canvas canvas, Size size) {
    final sx = size.width / 20;
    final sy = size.height / 20;
    final silhouette = Path()
      ..moveTo(19.5 * sx, 9.5 * sy)
      ..cubicTo(19.5 * sx, 4.1 * sy, 15.5 * sx, .5 * sy, 10 * sx, .5 * sy)
      ..cubicTo(4.4 * sx, .5 * sy, .5 * sx, 4.7 * sy, .5 * sx, 10 * sy)
      ..cubicTo(.5 * sx, 15.5 * sy, 4.6 * sx, 19.5 * sy, 9.8 * sx, 19.5 * sy)
      ..lineTo(11.2 * sx, 19.5 * sy)
      ..cubicTo(13 * sx, 19.5 * sy, 13.8 * sx, 17.7 * sy, 12.5 * sx, 16.2 * sy)
      ..cubicTo(
        11.2 * sx,
        14.8 * sy,
        11.9 * sx,
        13.4 * sy,
        13.7 * sx,
        13.4 * sy,
      )
      ..lineTo(16 * sx, 13.4 * sy)
      ..cubicTo(18.3 * sx, 13.4 * sy, 19.5 * sx, 12 * sy, 19.5 * sx, 9.5 * sy)
      ..close();
    canvas.drawPath(silhouette, Paint()..color = silhouetteColor);

    const colors = <Color>[
      Color(0xffff7836),
      Color(0xffffd343),
      Color(0xff41c895),
      Color(0xff21b9cf),
    ];
    const origins = <Offset>[
      Offset(3.4, 3.2),
      Offset(3.4, 7.2),
      Offset(3.4, 11.2),
      Offset(7.4, 11.2),
    ];
    for (var index = 0; index < origins.length; index++) {
      final origin = origins[index];
      final left = origin.dx * sx;
      final top = origin.dy * sy;
      final right = (origin.dx + 3.55) * sx;
      final bottom = (origin.dy + 3.55) * sy;
      final cut = .35 * math.min(sx, sy);
      final block = Path()
        ..moveTo(left + cut, top)
        ..lineTo(right - cut, top)
        ..lineTo(right, top + cut)
        ..lineTo(right, bottom - cut)
        ..lineTo(right - cut, bottom)
        ..lineTo(left + cut, bottom)
        ..lineTo(left, bottom - cut)
        ..lineTo(left, top + cut)
        ..close();
      canvas.drawPath(block, Paint()..color = colors[index]);
    }
    for (final swatch in <(Offset, Color)>[
      (const Offset(11.5, 4.4), const Color(0xff347df5)),
      (const Offset(15.1, 6.5), const Color(0xffa257e6)),
      (const Offset(16, 10.2), const Color(0xffef4760)),
    ]) {
      canvas.drawCircle(
        Offset(swatch.$1.dx * sx, swatch.$1.dy * sy),
        1.25 * math.min(sx, sy),
        Paint()..color = swatch.$2,
      );
    }
  }

  @override
  bool shouldRepaint(_LitchcraftIdentityGlyphPainter oldDelegate) =>
      oldDelegate.silhouetteColor != silhouetteColor;
}

class LitchcraftArtworkGlyph extends StatelessWidget {
  const LitchcraftArtworkGlyph({super.key, this.size = 24});

  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('craft-artwork-glyph'),
    size: Size.square(size),
    painter: _LitchcraftArtworkGlyphPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
    ),
  );
}

class _LitchcraftArtworkGlyphPainter extends CustomPainter {
  const _LitchcraftArtworkGlyphPainter(this.color);
  final Color color;
  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    canvas.scale(size.shortestSide / 24);
    final fill = Paint()..color = const Color(0xff33494e);
    final stroke = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.3
      ..strokeJoin = StrokeJoin.round;
    for (final offset in [2.0, 4.0, 6.0]) {
      final path = Path()
        ..moveTo(offset + 1.15, offset)
        ..lineTo(offset + 14.85, offset)
        ..lineTo(offset + 16, offset + 1.15)
        ..lineTo(offset + 16, offset + 14.85)
        ..lineTo(offset + 14.85, offset + 16)
        ..lineTo(offset + 1.15, offset + 16)
        ..lineTo(offset, offset + 14.85)
        ..lineTo(offset, offset + 1.15)
        ..close();
      canvas.drawPath(path, fill);
      canvas.drawPath(path, stroke);
    }
    const colors = [
      Color(0xffff7836),
      Color(0xffffd343),
      Color(0xff21b9cf),
      Color(0xffa257e6),
    ];
    for (var i = 0; i < 4; i++) {
      canvas.drawRect(
        Rect.fromLTWH(
          9.625 + (i % 2) * 5.1,
          9.625 + (i ~/ 2) * 5.1,
          3.65,
          3.65,
        ),
        Paint()..color = colors[i],
      );
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_LitchcraftArtworkGlyphPainter oldDelegate) =>
      oldDelegate.color != color;
}

/// Three quiet palette-colored sparkles use the same flat mark treatment as
/// the neighboring section glyphs.
class LitchcraftEffectsGlyph extends StatelessWidget {
  const LitchcraftEffectsGlyph({super.key, this.size = 28});
  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('craft-effects-glyph'),
    size: Size.square(size),
    painter: _LitchcraftEffectsGlyphPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
    ),
  );
}

class _LitchcraftEffectsGlyphPainter extends CustomPainter {
  const _LitchcraftEffectsGlyphPainter(this.outline);
  final Color outline;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    canvas.scale(size.shortestSide / 24);
    for (final spark in <(Offset, double, Color)>[
      (const Offset(8.5, 14), 6.5, const Color(0xff21b9cf)),
      (const Offset(17.5, 6), 4.2, const Color(0xffffd343)),
      (const Offset(19, 18), 3.2, const Color(0xffa257e6)),
    ]) {
      final c = spark.$1;
      final r = spark.$2;
      final star = Path()
        ..moveTo(c.dx, c.dy - r)
        ..quadraticBezierTo(c.dx + r * .22, c.dy - r * .22, c.dx + r, c.dy)
        ..quadraticBezierTo(c.dx + r * .22, c.dy + r * .22, c.dx, c.dy + r)
        ..quadraticBezierTo(c.dx - r * .22, c.dy + r * .22, c.dx - r, c.dy)
        ..quadraticBezierTo(c.dx - r * .22, c.dy - r * .22, c.dx, c.dy - r)
        ..close();
      canvas.drawPath(star, Paint()..color = spark.$3);
      canvas.drawPath(
        star,
        Paint()
          ..color = outline.withValues(alpha: .7)
          ..style = PaintingStyle.stroke
          ..strokeWidth = .8
          ..strokeJoin = StrokeJoin.round,
      );
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_LitchcraftEffectsGlyphPainter oldDelegate) =>
      oldDelegate.outline != outline;
}

/// The LED mark and target diagrams share the flat chamfered outline and
/// palette accents used by the Color and Artwork marks. Physical light previews
/// retain the separate acrylic/glow painter.
class LitchcraftLedsGlyph extends StatelessWidget {
  const LitchcraftLedsGlyph({super.key, this.size = 28});
  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('craft-leds-glyph'),
    size: Size.square(size),
    painter: _LitchcraftLedsGlyphPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
    ),
  );
}

class _LitchcraftLedsGlyphPainter extends CustomPainter {
  const _LitchcraftLedsGlyphPainter(
    this.outline, {
    this.mask,
    this.topology = LitchcraftTopology.v3,
    this.flipped = false,
    this.color = 0xffffff,
    this.multicolor = false,
  });
  final Color outline;
  final int? mask;
  final LitchcraftTopology topology;
  final bool flipped;
  final int color;
  final bool multicolor;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    canvas.scale(size.shortestSide / 24);
    final square = Path()
      ..moveTo(5, 2)
      ..lineTo(19, 2)
      ..lineTo(22, 5)
      ..lineTo(22, 19)
      ..lineTo(19, 22)
      ..lineTo(5, 22)
      ..lineTo(2, 19)
      ..lineTo(2, 5)
      ..close();
    canvas.drawPath(square, Paint()..color = const Color(0xff33494e));
    canvas.drawPath(
      square,
      Paint()
        ..color = outline
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.3
        ..strokeJoin = StrokeJoin.round,
    );
    const emitters = <(Rect, Color)>[
      (Rect.fromLTWH(5, 9.5, 2.5, 5), Color(0xffff7836)),
      (Rect.fromLTWH(9.5, 5, 5, 2.5), Color(0xffffd343)),
      (Rect.fromLTWH(16.5, 9.5, 2.5, 5), Color(0xff21b9cf)),
      (Rect.fromLTWH(9.5, 16.5, 5, 2.5), Color(0xffa257e6)),
    ];
    if (mask == null) {
      for (final emitter in emitters) {
        canvas.drawRRect(
          RRect.fromRectAndRadius(emitter.$1, const Radius.circular(.5)),
          Paint()..color = emitter.$2,
        );
      }
    } else {
      for (final target in topology.targets) {
        final displayTarget = litchcraftDisplayTargetMask(
          target,
          flipped: flipped,
        );
        final selected =
            mask == 15 || (target == 10 ? mask == 10 : (mask! & target) != 0);
        final index = switch (displayTarget) {
          1 => 0,
          2 => 1,
          4 => 2,
          _ => 3,
        };
        final horizontal = displayTarget == 2 || displayTarget == 8;
        final rect = displayTarget == 10
            ? const Rect.fromLTWH(9, 9, 6, 6)
            : Rect.fromCenter(
                center: emitters[index].$1.center,
                width: horizontal ? (selected ? 9 : 5) : (selected ? 3.5 : 2.5),
                height: horizontal
                    ? (selected ? 3.5 : 2.5)
                    : (selected ? 9 : 5),
              );
        final bar = RRect.fromRectAndRadius(rect, const Radius.circular(.6));
        if (selected) {
          canvas.drawRRect(
            bar,
            Paint()
              ..color = multicolor
                  ? emitters[index].$2
                  : litchcraftPreviewColor(color),
          );
        }
        // A large filled bar and stronger outline identify the target even
        // with a black brush. Other emitter positions remain hollow.
        canvas.drawRRect(
          bar,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = selected ? .85 : .5
            ..color = selected
                ? const Color(0xfff8f7f2)
                : const Color(0xff81979c),
        );
      }
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_LitchcraftLedsGlyphPainter oldDelegate) =>
      oldDelegate.outline != outline ||
      oldDelegate.mask != mask ||
      oldDelegate.topology != topology ||
      oldDelegate.flipped != flipped ||
      oldDelegate.color != color ||
      oldDelegate.multicolor != multicolor;
}

class LitchcraftAnimationGlyph extends StatelessWidget {
  const LitchcraftAnimationGlyph({
    super.key,
    this.size = 26,
    this.playing = false,
  });

  final double size;
  final bool playing;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('craft-animation-glyph'),
    size: Size.square(size),
    painter: _LitchcraftAnimationGlyphPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
      playing,
    ),
  );
}

class _LitchcraftAnimationGlyphPainter extends CustomPainter {
  const _LitchcraftAnimationGlyphPainter(this.color, this.playing);

  final Color color;
  final bool playing;

  @override
  void paint(Canvas canvas, Size size) {
    final stroke = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = size.shortestSide / 16
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final frame = RRect.fromRectAndRadius(
      Rect.fromLTWH(
        size.width * .08,
        size.height * .14,
        size.width * .84,
        size.height * .72,
      ),
      Radius.circular(size.width * .08),
    );
    canvas.drawRRect(frame, stroke);
    // Separate perforations from the border, preserving their rhythm at 25 px.
    for (final x in <double>[.24, .5, .76]) {
      for (final y in <double>[.24, .76]) {
        canvas.drawRRect(
          RRect.fromRectAndRadius(
            Rect.fromCenter(
              center: Offset(size.width * x, size.height * y),
              width: size.width * .09,
              height: size.height * .055,
            ),
            Radius.circular(size.width * .015),
          ),
          Paint()..color = color,
        );
      }
    }
    // Match the transport's next action: pause while playing, play while paused.
    // The dark inset keeps either mark readable in a selected tab.
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(
          size.width * .2,
          size.height * .32,
          size.width * .6,
          size.height * .36,
        ),
        Radius.circular(size.width * .025),
      ),
      Paint()..color = Colors.black,
    );
    final mark = Paint()
      ..color = playing ? Colors.white : const Color(0xff41c895);
    if (playing) {
      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(
            size.width * .4,
            size.height * .36,
            size.width * .08,
            size.height * .28,
          ),
          Radius.circular(size.width * .02),
        ),
        mark,
      );
      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(
            size.width * .56,
            size.height * .36,
            size.width * .08,
            size.height * .28,
          ),
          Radius.circular(size.width * .02),
        ),
        mark,
      );
    } else {
      final play = Path()
        ..moveTo(size.width * .43, size.height * .36)
        ..lineTo(size.width * .43, size.height * .64)
        ..lineTo(size.width * .66, size.height * .5)
        ..close();
      canvas.drawPath(play, mark);
    }
  }

  @override
  bool shouldRepaint(covariant _LitchcraftAnimationGlyphPainter oldDelegate) =>
      oldDelegate.color != color || oldDelegate.playing != playing;
}

class LitchcraftPaletteController extends ChangeNotifier {
  bool _isOpen = false;

  bool get isOpen => _isOpen;

  void open() {
    if (_isOpen) return;
    _isOpen = true;
    notifyListeners();
  }

  void close() {
    if (!_isOpen) return;
    _isOpen = false;
    notifyListeners();
  }

  void toggle() => _isOpen ? close() : open();
}

/// The palette replaces the dock's normal tools in place. It never creates a
/// second surface over the canvas, so opening it cannot move or obscure art.
class LitchcraftPaletteButton extends StatefulWidget {
  const LitchcraftPaletteButton({
    super.key,
    required this.color,
    required this.recentColors,
    required this.enabled,
    required this.onSelected,
    required this.onCustomColor,
    required this.onOpen,
    required this.onClose,
    this.previousColor,
    this.onSwap,
    this.controller,
    this.axis = Axis.horizontal,
    this.openTrayKey = const ValueKey('craft-palette-tray'),
  });

  final int color;
  final List<int> recentColors;
  final bool enabled;
  final ValueChanged<int> onSelected;
  final VoidCallback onCustomColor;
  final VoidCallback onOpen;
  final VoidCallback onClose;
  final int? previousColor;
  final VoidCallback? onSwap;
  final LitchcraftPaletteController? controller;
  final Axis axis;
  final Key? openTrayKey;

  @override
  State<LitchcraftPaletteButton> createState() =>
      _LitchcraftPaletteButtonState();
}

class _LitchcraftPaletteButtonState extends State<LitchcraftPaletteButton> {
  late final LitchcraftPaletteController _ownedController;
  bool _handlingKeys = false;
  bool _open = false;

  LitchcraftPaletteController get _controller =>
      widget.controller ?? _ownedController;

  @override
  void initState() {
    super.initState();
    _ownedController = LitchcraftPaletteController();
    _controller.addListener(_syncController);
    _open = _controller.isOpen;
  }

  @override
  void didUpdateWidget(covariant LitchcraftPaletteButton oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.controller != widget.controller) {
      (oldWidget.controller ?? _ownedController).removeListener(
        _syncController,
      );
      _controller.addListener(_syncController);
      _syncController();
    }
  }

  @override
  void dispose() {
    _controller.removeListener(_syncController);
    if (_handlingKeys) HardwareKeyboard.instance.removeHandler(_handleKeyEvent);
    _ownedController.dispose();
    super.dispose();
  }

  void _syncController() {
    if (!mounted || _open == _controller.isOpen) return;
    setState(() => _open = _controller.isOpen);
    if (_open) {
      HardwareKeyboard.instance.addHandler(_handleKeyEvent);
      _handlingKeys = true;
      widget.onOpen();
    } else {
      HardwareKeyboard.instance.removeHandler(_handleKeyEvent);
      _handlingKeys = false;
      widget.onClose();
    }
  }

  bool _handleKeyEvent(KeyEvent event) {
    if (event is! KeyDownEvent ||
        event.logicalKey != LogicalKeyboardKey.escape) {
      return false;
    }
    _controller.close();
    return true;
  }

  Widget _paletteStrip(BuildContext context) {
    final strings = litchStrings(context);
    final extras =
        widget.recentColors
            .where((value) => value != 0 && !litchcraftPalette.contains(value))
            .toSet()
            .toList()
          ..sort();
    final colors = <int>[...litchcraftPalette, ...extras];
    return Flex(
      direction: widget.axis,
      key: const ValueKey('craft-palette-content'),
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        IconButton(
          key: const ValueKey('craft-palette-collapse'),
          tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
          autofocus: true,
          onPressed: _controller.close,
          icon: const Icon(Icons.arrow_back_rounded),
          style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
        ),
        _swapButton(context),
        for (final value in colors)
          Semantics(
            selected: value == widget.color,
            child: IconButton(
              key: ValueKey('craft-picker-color-$value'),
              tooltip: _litchcraftColorName(context, value),
              style: IconButton.styleFrom(
                minimumSize: const Size(48, 48),
                padding: EdgeInsets.zero,
              ),
              onPressed: widget.enabled
                  ? () {
                      _controller.close();
                      widget.onSelected(value);
                    }
                  : null,
              icon: Container(
                width: 32,
                height: 32,
                decoration: BoxDecoration(
                  color: litchcraftPreviewColor(value),
                  shape: BoxShape.circle,
                  border: Border.all(
                    width: value == widget.previousColor ? 3 : 1,
                    color: Theme.of(context).colorScheme.outline,
                  ),
                ),
                child: value == widget.color
                    ? Icon(
                        Icons.check,
                        size: 18,
                        color: litchSwatchMarkColor(
                          litchcraftPreviewColor(value),
                        ),
                        shadows: const <Shadow>[
                          Shadow(color: Colors.black, blurRadius: 3),
                        ],
                      )
                    : value == widget.previousColor
                    ? Icon(
                        Icons.history,
                        size: 16,
                        color: litchSwatchMarkColor(
                          litchcraftPreviewColor(value),
                        ),
                        shadows: const <Shadow>[
                          Shadow(color: Colors.black, blurRadius: 3),
                        ],
                      )
                    : null,
              ),
            ),
          ),
        IconButton(
          key: const ValueKey('craft-custom-color'),
          tooltip: strings.craftCustomColor,
          onPressed: widget.enabled
              ? () {
                  _controller.close();
                  widget.onCustomColor();
                }
              : null,
          icon: const Icon(Icons.tune),
          style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
        ),
      ],
    );
  }

  Widget _swapButton(BuildContext context) => IconButton(
    key: const ValueKey('craft-swap-color'),
    tooltip: litchStrings(context).craftSwapColors,
    onPressed: widget.enabled ? widget.onSwap : null,
    icon: LitchcraftColorSwapIcon(
      current: widget.color,
      previous: widget.previousColor,
    ),
    style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
  );

  Widget _launcher(BuildContext context) {
    return IconButton(
      key: const ValueKey('craft-palette'),
      tooltip: litchStrings(context).craftPalette,
      onPressed: widget.enabled ? _controller.open : null,
      icon: CustomPaint(
        size: const Size.square(26),
        painter: _LitchcraftPalettePainter(widget.color),
      ),
      style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final duration = MediaQuery.disableAnimationsOf(context)
        ? Duration.zero
        : const Duration(milliseconds: 140);
    return TapRegion(
      consumeOutsideTaps: _open,
      onTapOutside: _open ? (_) => _controller.close() : null,
      child: Material(
        key: const ValueKey('craft-color-control'),
        color: Colors.transparent,
        child: ClipRect(
          child: AnimatedSwitcher(
            duration: duration,
            reverseDuration: duration,
            transitionBuilder: (child, animation) => FadeTransition(
              opacity: animation,
              child: SlideTransition(
                position: Tween<Offset>(
                  begin: widget.axis == Axis.vertical
                      ? const Offset(0, .08)
                      : const Offset(.08, 0),
                  end: Offset.zero,
                ).animate(animation),
                child: child,
              ),
            ),
            child: _open
                ? KeyedSubtree(
                    key: widget.openTrayKey,
                    child: _paletteStrip(context),
                  )
                : Flex(
                    direction: widget.axis,
                    key: const ValueKey('craft-palette-collapsed'),
                    mainAxisSize: MainAxisSize.min,
                    children: <Widget>[
                      _launcher(context),
                      _swapButton(context),
                    ],
                  ),
          ),
        ),
      ),
    );
  }
}

class _LitchcraftPalettePainter extends CustomPainter {
  const _LitchcraftPalettePainter(this.current);

  final int current;

  @override
  void paint(Canvas canvas, Size size) {
    final body = Paint()..color = const Color(0xffedb449);
    final outline = Paint()
      ..color = const Color(0xff33494e)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5;
    final path = Path()
      ..moveTo(size.width * .12, size.height * .6)
      ..cubicTo(0, size.height * .18, size.width * .3, 0, size.width * .62, 0)
      ..cubicTo(
        size.width,
        0,
        size.width,
        size.height * .34,
        size.width * .82,
        size.height * .5,
      )
      ..cubicTo(
        size.width * .68,
        size.height * .64,
        size.width * .58,
        size.height * .5,
        size.width * .49,
        size.height * .7,
      )
      ..cubicTo(
        size.width * .36,
        size.height,
        size.width * .2,
        size.height * .9,
        size.width * .12,
        size.height * .6,
      )
      ..close();
    canvas
      ..drawPath(path, body)
      ..drawPath(path, outline);
    final dots = <(Offset, Color)>[
      (Offset(size.width * .3, size.height * .26), const Color(0xffff3b57)),
      (Offset(size.width * .54, size.height * .2), const Color(0xff34d477)),
      (Offset(size.width * .73, size.height * .34), const Color(0xff2bbbd9)),
      (Offset(size.width * .27, size.height * .52), const Color(0xffa84ff0)),
      (
        Offset(size.width * .5, size.height * .45),
        litchcraftPreviewColor(current),
      ),
    ];
    for (final dot in dots) {
      canvas.drawCircle(dot.$1, size.width * .07, Paint()..color = dot.$2);
    }
    canvas.drawCircle(
      Offset(size.width * .72, size.height * .73),
      size.width * .1,
      Paint()..color = const Color(0xfff7f5ee),
    );
  }

  @override
  bool shouldRepaint(covariant _LitchcraftPalettePainter oldDelegate) =>
      oldDelegate.current != current;
}

class LitchcraftPaletteGlyph extends StatelessWidget {
  const LitchcraftPaletteGlyph({
    super.key,
    required this.color,
    this.size = 26,
  });

  final int color;
  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    key: const ValueKey('craft-palette-glyph'),
    size: Size.square(size),
    painter: _LitchcraftPalettePainter(color),
  );
}

class LitchcraftColorInspector extends StatefulWidget {
  const LitchcraftColorInspector({
    super.key,
    required this.color,
    required this.previousColor,
    required this.recentColors,
    required this.enabled,
    required this.onChanged,
    required this.onPreviousColor,
    this.onPreviewStarted,
    this.onPreviewCancelled,
    this.onPreviewChanged,
    this.onHexPadVisibilityChanged,
    this.spacious = false,
  });

  final int color;
  final int? previousColor;
  final List<int> recentColors;
  final bool enabled;
  final ValueChanged<int> onChanged;
  final VoidCallback? onPreviousColor;
  final VoidCallback? onPreviewStarted;
  final VoidCallback? onPreviewCancelled;
  final ValueChanged<int>? onPreviewChanged;
  final ValueChanged<bool>? onHexPadVisibilityChanged;
  final bool spacious;

  @override
  State<LitchcraftColorInspector> createState() =>
      _LitchcraftColorInspectorState();
}

class _LitchcraftColorInspectorState extends State<LitchcraftColorInspector> {
  late HSVColor _color = _hsv(widget.color);
  late final TextEditingController _hex = TextEditingController(
    text: _formatHex(widget.color),
  );
  final FocusNode _hexFocus = FocusNode();
  final MenuController _hexMenuController = MenuController();
  bool _hexInvalid = false;
  bool _replaceHexOnNextKey = false;

  static HSVColor _hsv(int color) =>
      HSVColor.fromColor(Color(0xff000000 | (color & 0xffffff)));

  static String _formatHex(int color) =>
      '#${(color & 0xffffff).toRadixString(16).padLeft(6, '0').toUpperCase()}';

  int get _rgb => _color.toColor().toARGB32() & 0xffffff;

  @override
  void didUpdateWidget(LitchcraftColorInspector oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.enabled && !widget.enabled) {
      widget.onPreviewCancelled?.call();
    }
    if (oldWidget.color == widget.color) return;
    if (_rgb != widget.color) _color = _hsv(widget.color);
    if (!_hexMenuController.isOpen) {
      _hexInvalid = false;
      _hex.text = _formatHex(widget.color);
    }
  }

  @override
  void dispose() {
    widget.onPreviewCancelled?.call();
    _hex.dispose();
    _hexFocus.dispose();
    super.dispose();
  }

  void _commitColor(
    HSVColor next, {
    bool updateHex = true,
    bool continuous = true,
  }) {
    setState(() {
      _color = next;
      _hexInvalid = false;
      if (updateHex) _hex.text = _formatHex(_rgb);
    });
    (continuous
        ? widget.onPreviewChanged ?? widget.onChanged
        : widget.onChanged)(_rgb);
  }

  bool _commitHex() {
    final match = RegExp(r'^#?([0-9a-fA-F]{6})$').firstMatch(_hex.text.trim());
    if (match == null) {
      setState(() => _hexInvalid = true);
      return false;
    }
    final rgb = int.parse(match.group(1)!, radix: 16);
    _hex.text = _formatHex(rgb);
    _commitColor(_hsv(rgb), updateHex: false, continuous: false);
    return true;
  }

  String _sanitizedHex(String value) {
    var source = value.trim();
    if (source.startsWith('#')) source = source.substring(1);
    if (source.toLowerCase().startsWith('0x')) source = source.substring(2);
    final filtered = source
        .toUpperCase()
        .split('')
        .where((character) => RegExp(r'[0-9A-F]').hasMatch(character))
        .take(6)
        .join();
    return filtered;
  }

  void _replaceHex(String value) {
    final filtered = _sanitizedHex(value);
    setState(() {
      _hexInvalid = false;
      _hex.text = '#$filtered';
      _hex.selection = TextSelection.collapsed(offset: _hex.text.length);
    });
  }

  void _appendHex(String value) {
    if (_replaceHexOnNextKey) {
      _replaceHexOnNextKey = false;
      _replaceHex(value);
      return;
    }
    final current = _sanitizedHex(_hex.text);
    if (current.length >= 6) return;
    _replaceHex('$current$value');
  }

  void _deleteHexDigit() {
    _replaceHexOnNextKey = false;
    final current = _sanitizedHex(_hex.text);
    if (current.isEmpty) return;
    _replaceHex(current.substring(0, current.length - 1));
  }

  Future<void> _pasteHex() async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    if (!mounted || data?.text == null) return;
    _replaceHex(data!.text!);
    _replaceHexOnNextKey = false;
  }

  Future<void> _copyHex() => Clipboard.setData(
    ClipboardData(
      text: _formatHex(
        int.tryParse(_sanitizedHex(_hex.text), radix: 16) ?? _rgb,
      ),
    ),
  );

  void _openHexPad() {
    _replaceHexOnNextKey = true;
    _hexMenuController.open();
  }

  void _restoreHex() {
    setState(() {
      _hexInvalid = false;
      _hex.text = _formatHex(widget.color);
      _color = _hsv(widget.color);
    });
    _hexFocus.unfocus();
  }

  Widget _field(double height) => LayoutBuilder(
    builder: (context, bounds) {
      void update(Offset point) {
        final saturation = (point.dx / bounds.maxWidth).clamp(0.0, 1.0);
        final value = (1 - point.dy / height).clamp(0.0, 1.0);
        _commitColor(HSVColor.fromAHSV(1, _color.hue, saturation, value));
      }

      return Semantics(
        label: litchStrings(context).craftSaturationValueField,
        value: _formatHex(_rgb),
        enabled: widget.enabled,
        child: RawGestureDetector(
          // This is a two-axis control. Claim its pointers immediately so a
          // horizontal color drag cannot be mistaken for a page swipe.
          gestures: widget.enabled
              ? <Type, GestureRecognizerFactory>{
                  EagerGestureRecognizer:
                      GestureRecognizerFactoryWithHandlers<
                        EagerGestureRecognizer
                      >(() => EagerGestureRecognizer(), (_) {}),
                }
              : const <Type, GestureRecognizerFactory>{},
          child: Listener(
            key: const ValueKey('craft-inspector-color-field'),
            behavior: HitTestBehavior.opaque,
            onPointerDown: widget.enabled
                ? (event) {
                    widget.onPreviewStarted?.call();
                    update(event.localPosition);
                  }
                : null,
            onPointerMove: widget.enabled
                ? (event) => update(event.localPosition)
                : null,
            onPointerUp: widget.enabled ? (_) => widget.onChanged(_rgb) : null,
            onPointerCancel: widget.enabled
                ? (_) => widget.onChanged(_rgb)
                : null,
            child: SizedBox(
              height: height,
              child: ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: Stack(
                  children: <Widget>[
                    Positioned.fill(
                      child: CustomPaint(
                        painter: _ColorFieldPainter(_color.hue),
                      ),
                    ),
                    _ColorFieldSelection(
                      color: _color,
                      width: bounds.maxWidth,
                      height: height,
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      );
    },
  );

  Widget _hue(BuildContext context) => ColorSlider(
    sliderKey: const ValueKey('craft-inspector-hue'),
    trackKey: const ValueKey('craft-inspector-hue-track'),
    value: _color.hue,
    label: litchStrings(context).craftHue,
    gradient: _previewHueGradient,
    onChanged: widget.enabled
        ? (value) => _commitColor(_color.withHue(value))
        : null,
    onChangeStart: widget.enabled
        ? (_) => widget.onPreviewStarted?.call()
        : null,
    onChangeEnd: widget.enabled ? (_) => widget.onChanged(_rgb) : null,
  );

  Widget _hexPad(BuildContext context) {
    final material = MaterialLocalizations.of(context);
    return SizedBox(
      key: const ValueKey('craft-hex-pad'),
      width: 232,
      child: SingleChildScrollView(
        primary: false,
        child: Padding(
          padding: const EdgeInsets.all(8),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Column(
                    children: [
                      for (final row in ['123', '456', '789'])
                        Row(
                          children: [
                            for (final value in row.split(''))
                              _hexKey(value, height: 72),
                          ],
                        ),
                      SizedBox(
                        width: 156,
                        child: Center(child: _hexKey('0', height: 72)),
                      ),
                    ],
                  ),
                  const SizedBox(width: 8),
                  Column(
                    children: [
                      for (final value in 'ABCDEF'.split('')) _hexKey(value),
                    ],
                  ),
                ],
              ),
              Row(
                children: [
                  Expanded(
                    child: IconButton(
                      key: const ValueKey('craft-hex-copy'),
                      tooltip: material.copyButtonLabel,
                      onPressed: _copyHex,
                      icon: const Icon(Icons.content_copy_rounded),
                    ),
                  ),
                  Expanded(
                    child: IconButton(
                      key: const ValueKey('craft-hex-delete'),
                      tooltip: material.deleteButtonTooltip,
                      onPressed: _deleteHexDigit,
                      icon: const Icon(Icons.backspace_outlined),
                    ),
                  ),
                  Expanded(
                    child: IconButton(
                      key: const ValueKey('craft-hex-paste'),
                      tooltip: material.pasteButtonLabel,
                      onPressed: _pasteHex,
                      icon: const Icon(Icons.content_paste_rounded),
                    ),
                  ),
                  Expanded(
                    child: IconButton(
                      key: const ValueKey('craft-hex-apply'),
                      tooltip: litchStrings(context).craftApplyColor,
                      onPressed: () {
                        if (_commitHex()) _hexMenuController.close();
                      },
                      icon: const Icon(Icons.check_rounded),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _hexKey(String value, {double height = 48}) => SizedBox(
    width: 52,
    height: height,
    child: TextButton(
      key: ValueKey('craft-hex-key-$value'),
      onPressed: () => _appendHex(value),
      child: Text(value),
    ),
  );

  Widget _hexField(BuildContext context) {
    final strings = litchStrings(context);
    return MenuAnchor(
      controller: _hexMenuController,
      onOpen: () => widget.onHexPadVisibilityChanged?.call(true),
      onClose: () {
        if (_hexInvalid || _hex.text != _formatHex(_rgb)) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (!mounted || _hexMenuController.isOpen) return;
            setState(() {
              _hexInvalid = false;
              _hex.text = _formatHex(_rgb);
            });
          });
        }
        widget.onHexPadVisibilityChanged?.call(false);
      },
      builder: (context, menu, _) => Focus(
        onKeyEvent: (_, event) {
          if (event is KeyDownEvent &&
              event.logicalKey == LogicalKeyboardKey.escape) {
            menu.close();
            _restoreHex();
            return KeyEventResult.handled;
          }
          return KeyEventResult.ignored;
        },
        child: Semantics(
          label: _hexInvalid
              ? strings.craftHexColorInvalid
              : strings.craftHexColor,
          button: true,
          child: TextField(
            key: const ValueKey('craft-inspector-hex'),
            controller: _hex,
            focusNode: _hexFocus,
            enabled: widget.enabled,
            readOnly: true,
            showCursor: false,
            enableInteractiveSelection: true,
            onTap: _openHexPad,
            contextMenuBuilder: (context, editable) {
              final material = MaterialLocalizations.of(context);
              return AdaptiveTextSelectionToolbar.buttonItems(
                anchors: editable.contextMenuAnchors,
                buttonItems: <ContextMenuButtonItem>[
                  ContextMenuButtonItem(
                    label: material.copyButtonLabel,
                    onPressed: () {
                      editable.hideToolbar();
                      unawaited(_copyHex());
                    },
                  ),
                  ContextMenuButtonItem(
                    label: material.pasteButtonLabel,
                    onPressed: () {
                      editable.hideToolbar();
                      unawaited(_pasteHex());
                    },
                  ),
                ],
              );
            },
            style: const TextStyle(color: Colors.white, fontSize: 11),
            decoration: InputDecoration(
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(
                horizontal: 6,
                vertical: 10,
              ),
              hintText: '#RRGGBB',
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(6),
              ),
              enabledBorder: OutlineInputBorder(
                borderRadius: BorderRadius.circular(6),
                borderSide: BorderSide(
                  color: _hexInvalid ? Colors.redAccent : Colors.white54,
                ),
              ),
              suffixIconConstraints: const BoxConstraints.tightFor(
                width: 48,
                height: 48,
              ),
              suffixIcon: Tooltip(
                message: _hexInvalid
                    ? strings.craftHexColorInvalid
                    : strings.craftApplyColor,
                child: IconButton(
                  key: const ValueKey('craft-inspector-hex-apply'),
                  onPressed: widget.enabled ? _openHexPad : null,
                  style: IconButton.styleFrom(
                    minimumSize: const Size(48, 48),
                    foregroundColor: Colors.white,
                  ),
                  icon: Icon(_hexInvalid ? Icons.error_outline : Icons.check),
                ),
              ),
            ),
          ),
        ),
      ),
      menuChildren: [LitchCompactControls(child: _hexPad(context))],
    );
  }

  Widget _previous(BuildContext context) => IconButton(
    key: const ValueKey('craft-inspector-previous-color'),
    tooltip: litchStrings(context).craftSwapColors,
    onPressed: widget.enabled ? widget.onPreviousColor : null,
    style: IconButton.styleFrom(
      minimumSize: const Size(48, 48),
      foregroundColor: Colors.white,
      disabledForegroundColor: Colors.white38,
    ),
    icon: LitchcraftColorSwapIcon(
      current: _rgb,
      previous: widget.previousColor,
    ),
  );

  Widget _recent(BuildContext context) => LayoutBuilder(
    builder: (context, bounds) => Wrap(
      children: [
        for (final value in widget.recentColors.take(4))
          SizedBox(
            width: bounds.maxWidth / (bounds.maxWidth < 192 ? 2 : 4),
            height: 48,
            child: Center(
              child: IconButton(
                key: ValueKey('craft-inspector-recent-$value'),
                tooltip: _litchcraftColorName(context, value),
                onPressed: widget.enabled
                    ? () => _commitColor(_hsv(value), continuous: false)
                    : null,
                style: IconButton.styleFrom(
                  minimumSize: const Size(48, 48),
                  backgroundColor: litchcraftPreviewColor(value),
                ),
                icon: value == _rgb
                    ? Icon(
                        Icons.check,
                        color: litchSwatchMarkColor(
                          litchcraftPreviewColor(value),
                        ),
                      )
                    : const SizedBox.shrink(),
              ),
            ),
          ),
      ],
    ),
  );

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, bounds) {
      final compact = bounds.maxHeight < 216;
      if (!widget.spacious) {
        final hexText = TextPainter(
          text: TextSpan(
            text: '#FFFFFF',
            style: Theme.of(
              context,
            ).textTheme.bodyLarge!.copyWith(fontSize: 11),
          ),
          textDirection: Directionality.of(context),
          textScaler: MediaQuery.textScalerOf(context),
        )..layout();
        final hexWidth = hexText.width.ceilToDouble() + 72;
        hexText.dispose();
        return Column(
          children: [
            Row(
              children: [
                SizedBox(
                  width: math.min(hexWidth, bounds.maxWidth - 56),
                  child: SizedBox(
                    height: 48,
                    child: LitchCompactControls(child: _hexField(context)),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(child: _hue(context)),
              ],
            ),
            const SizedBox(height: 6),
            Expanded(
              child: LayoutBuilder(
                builder: (context, field) => _field(field.maxHeight),
              ),
            ),
            if (bounds.maxHeight >= 270) _recent(context),
          ],
        );
      }
      if (bounds.maxHeight >= 144) {
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
              height: 48,
              child: LitchCompactControls(child: _hexField(context)),
            ),
            const SizedBox(height: 6),
            _hue(context),
            const SizedBox(height: 10),
            Expanded(
              child: LayoutBuilder(
                builder: (context, field) => _field(field.maxHeight),
              ),
            ),
            if (bounds.maxHeight >= 270) _recent(context),
          ],
        );
      }
      final horizontallyTight = bounds.maxHeight < 102;
      final controls = horizontallyTight
          ? Row(
              children: [
                Expanded(child: _hue(context)),
                const SizedBox(width: 6),
                Expanded(
                  child: SizedBox(
                    height: 48,
                    child: LitchCompactControls(child: _hexField(context)),
                  ),
                ),
                if (!widget.spacious) _previous(context),
              ],
            )
          : Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Row(
                  children: [
                    Expanded(child: _hue(context)),
                    if (!widget.spacious) _previous(context),
                  ],
                ),
                const SizedBox(height: 6),
                SizedBox(
                  height: 48,
                  child: LitchCompactControls(child: _hexField(context)),
                ),
              ],
            );
      if (compact) {
        return Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Expanded(flex: 2, child: _field(bounds.maxHeight)),
            const SizedBox(width: 6),
            Expanded(flex: 3, child: Center(child: controls)),
          ],
        );
      }
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            child: LayoutBuilder(
              builder: (context, field) => _field(field.maxHeight),
            ),
          ),
          const SizedBox(height: 6),
          controls,
          if (bounds.maxHeight >= 270) _recent(context),
        ],
      );
    },
  );
}

class LitchcraftPaletteTray extends StatelessWidget {
  const LitchcraftPaletteTray({
    super.key,
    required this.color,
    required this.recentColors,
    required this.enabled,
    required this.onSelected,
    required this.onCustomColor,
    required this.onClose,
    this.previousColor,
    this.onSwap,
  });

  final int color;
  final List<int> recentColors;
  final bool enabled;
  final ValueChanged<int> onSelected;
  final VoidCallback onCustomColor;
  final VoidCallback onClose;
  final int? previousColor;
  final VoidCallback? onSwap;

  @override
  Widget build(BuildContext context) {
    final extras =
        recentColors
            .where((value) => value != 0 && !litchcraftPalette.contains(value))
            .toSet()
            .toList()
          ..sort();
    final colors = <int>[...litchcraftPalette, ...extras];
    return Semantics(
      container: true,
      explicitChildNodes: true,
      label: litchStrings(context).craftPalette,
      child: Wrap(
        key: const ValueKey('craft-palette-tray'),
        spacing: 0,
        runSpacing: 0,
        children: <Widget>[
          IconButton(
            key: const ValueKey('craft-palette-collapse'),
            tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
            autofocus: true,
            onPressed: onClose,
            icon: const Icon(Icons.close),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-swap-color'),
            tooltip: litchStrings(context).craftSwapColors,
            onPressed: enabled ? onSwap : null,
            icon: LitchcraftColorSwapIcon(
              current: color,
              previous: previousColor,
            ),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          for (final value in colors)
            Semantics(
              selected: value == color,
              child: IconButton(
                key: ValueKey('craft-picker-color-$value'),
                tooltip: _litchcraftColorName(context, value),
                onPressed: enabled ? () => onSelected(value) : null,
                style: IconButton.styleFrom(
                  minimumSize: const Size(48, 48),
                  padding: EdgeInsets.zero,
                ),
                icon: Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: litchcraftPreviewColor(value),
                    shape: BoxShape.circle,
                    border: Border.all(
                      width: value == previousColor ? 3 : 1,
                      color: Theme.of(context).colorScheme.outline,
                    ),
                  ),
                  child: value == color
                      ? Icon(
                          Icons.check,
                          size: 18,
                          color: litchSwatchMarkColor(
                            litchcraftPreviewColor(value),
                          ),
                          shadows: const <Shadow>[
                            Shadow(color: Colors.black, blurRadius: 3),
                          ],
                        )
                      : null,
                ),
              ),
            ),
          IconButton(
            key: const ValueKey('craft-custom-color'),
            tooltip: litchStrings(context).craftCustomColor,
            onPressed: enabled ? onCustomColor : null,
            icon: const Icon(Icons.tune),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
        ],
      ),
    );
  }
}

class LitchcraftAnimationQuickTray extends StatelessWidget {
  const LitchcraftAnimationQuickTray({
    super.key,
    required this.document,
    required this.selectedFrameIndex,
    required this.playing,
    required this.enabled,
    required this.playbackVisible,
    required this.playLabel,
    required this.pauseLabel,
    required this.previousFrameLabel,
    required this.nextFrameLabel,
    required this.addFrameLabel,
    required this.deleteFrameLabel,
    required this.motionLabel,
    required this.detailsLabel,
    required this.frameLabel,
    required this.onClose,
    required this.onTogglePlayback,
    required this.onSelectFrame,
    required this.onAddFrame,
    required this.onDeleteFrame,
    required this.onOpenEffects,
    required this.onOpenFrameDetails,
    required this.frameEditingEnabled,
    required this.motionEnabled,
  });

  final LitchcraftAnimationDocument document;
  final int selectedFrameIndex;
  final bool playing;
  final bool enabled;
  final bool playbackVisible;
  final String playLabel;
  final String pauseLabel;
  final String previousFrameLabel;
  final String nextFrameLabel;
  final String addFrameLabel;
  final String deleteFrameLabel;
  final String motionLabel;
  final String detailsLabel;
  final String Function(int frame, int count) frameLabel;
  final VoidCallback onClose;
  final VoidCallback onTogglePlayback;
  final ValueChanged<int> onSelectFrame;
  final VoidCallback onAddFrame;
  final VoidCallback onDeleteFrame;
  final VoidCallback onOpenEffects;
  final VoidCallback onOpenFrameDetails;
  final bool frameEditingEnabled;
  final bool motionEnabled;

  @override
  Widget build(BuildContext context) {
    final count = document.frames.length;
    final indexes = <int>{
      if (selectedFrameIndex > 0) selectedFrameIndex - 1,
      selectedFrameIndex,
      if (selectedFrameIndex + 1 < count) selectedFrameIndex + 1,
    }.toList(growable: false);
    final canBack = enabled && selectedFrameIndex > 0;
    final canForward = enabled && selectedFrameIndex + 1 < count;
    final canPlay = enabled && playbackVisible;
    final canAdd =
        enabled &&
        frameEditingEnabled &&
        count < litchcraftAnimationMaximumFrameCount;
    final canDelete = enabled && frameEditingEnabled && count > 1;
    return Semantics(
      container: true,
      explicitChildNodes: true,
      child: Wrap(
        key: const ValueKey('craft-animation-quick-tray'),
        spacing: 0,
        runSpacing: 0,
        children: <Widget>[
          IconButton(
            key: const ValueKey('craft-close-animation'),
            tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
            autofocus: true,
            onPressed: onClose,
            icon: const Icon(Icons.close),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          for (final index in indexes)
            Semantics(
              selected: index == selectedFrameIndex,
              button: true,
              enabled: enabled,
              label: frameLabel(index + 1, count),
              onTap: enabled ? () => onSelectFrame(index) : null,
              excludeSemantics: true,
              child: IconButton(
                key: ValueKey('craft-quick-frame-$index'),
                tooltip: frameLabel(index + 1, count),
                onPressed: enabled ? () => onSelectFrame(index) : null,
                style: IconButton.styleFrom(
                  minimumSize: const Size(48, 48),
                  backgroundColor: index == selectedFrameIndex
                      ? Theme.of(context).colorScheme.secondaryContainer
                      : null,
                ),
                icon: LitchcraftFrameThumbnail(
                  document: document,
                  frameIndex: index,
                ),
              ),
            ),
          IconButton(
            key: const ValueKey('craft-frame-previous'),
            tooltip: previousFrameLabel,
            onPressed: canBack
                ? () => onSelectFrame(selectedFrameIndex - 1)
                : null,
            icon: const Icon(Icons.skip_previous),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-preview'),
            tooltip: playing ? pauseLabel : playLabel,
            onPressed: canPlay ? onTogglePlayback : null,
            icon: Icon(playing ? Icons.pause : Icons.play_arrow),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-frame-next'),
            tooltip: nextFrameLabel,
            onPressed: canForward
                ? () => onSelectFrame(selectedFrameIndex + 1)
                : null,
            icon: const Icon(Icons.skip_next),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-add-frame'),
            tooltip: addFrameLabel,
            onPressed: canAdd ? onAddFrame : null,
            icon: const Icon(Icons.add),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-delete-current-frame'),
            tooltip: deleteFrameLabel,
            onPressed: canDelete ? onDeleteFrame : null,
            icon: const Icon(Icons.remove),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-animation-effect'),
            tooltip: motionLabel,
            onPressed: enabled && motionEnabled ? onOpenEffects : null,
            icon: const Icon(Icons.animation_outlined),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          IconButton(
            key: const ValueKey('craft-frame-details'),
            tooltip: detailsLabel,
            onPressed: enabled ? onOpenFrameDetails : null,
            icon: const Icon(Icons.tune),
            style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
        ],
      ),
    );
  }
}

class LitchcraftToolButton extends StatelessWidget {
  const LitchcraftToolButton({
    super.key,
    required this.eraser,
    required this.pickingColor,
    required this.targetMask,
    required this.topology,
    required this.flipped,
    required this.color,
    required this.enabled,
    required this.holdDuration,
    required this.onPaint,
    required this.onErase,
    required this.onPaintAll,
    required this.onEraseAll,
    required this.onPickColor,
    required this.onTarget,
    required this.onOpen,
    required this.onClose,
    this.rootSafeInsets = EdgeInsets.zero,
    this.openTowardStart = false,
    this.maxMenuWidth = 280,
    this.menuController,
  });

  final bool eraser;
  final bool pickingColor;
  final int targetMask;
  final LitchcraftTopology topology;
  final bool flipped;
  final int color;
  final bool enabled;
  final Duration holdDuration;
  final VoidCallback onPaint;
  final VoidCallback onErase;
  final VoidCallback onPaintAll;
  final VoidCallback onEraseAll;
  final VoidCallback onPickColor;
  final ValueChanged<int> onTarget;
  final VoidCallback onOpen;
  final VoidCallback onClose;
  final EdgeInsets rootSafeInsets;
  final bool openTowardStart;
  final double maxMenuWidth;
  final MenuController? menuController;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final media = MediaQuery.of(context);
    final menuWidth = (media.size.width - rootSafeInsets.horizontal - 16)
        .clamp(64.0, maxMenuWidth)
        .toDouble();
    final menuMaxHeight = (media.size.height - rootSafeInsets.vertical - 16)
        .clamp(48.0, double.infinity)
        .toDouble();
    final compactMenu = menuWidth < 224;
    final compactMenuHeight = math.min(menuMaxHeight, 320.0);
    final targets = <int>[...topology.targets, 15];
    final selectedLabel = eraser ? strings.craftErase : strings.craftPaint;
    Widget modeAction({required bool paint}) => LitchcraftHoldAction(
      duration: holdDuration,
      showCompletionSnackBar: false,
      holdLabel: paint ? strings.craftPaintAll : strings.craftEraseAll,
      completedLabel: paint ? strings.craftAllPainted : strings.craftAllErased,
      onTap: enabled ? (paint ? onPaint : onErase) : null,
      onHold: enabled ? (paint ? onPaintAll : onEraseAll) : null,
      child: Semantics(
        selected: (paint ? !eraser : eraser) && !pickingColor,
        button: true,
        child: FilledButton.tonalIcon(
          key: ValueKey(paint ? 'craft-paint' : 'craft-erase'),
          onPressed: enabled ? (paint ? onPaint : onErase) : null,
          icon: paint
              ? const Icon(Icons.brush_outlined)
              : const LitchcraftEraserIcon(),
          label: Text(paint ? strings.craftPaint : strings.craftErase),
          style: FilledButton.styleFrom(minimumSize: const Size(104, 48)),
        ),
      ),
    );
    Widget compactModeAction({required bool paint}) => LitchcraftHoldAction(
      duration: holdDuration,
      showCompletionSnackBar: false,
      holdLabel: paint ? strings.craftPaintAll : strings.craftEraseAll,
      completedLabel: paint ? strings.craftAllPainted : strings.craftAllErased,
      onTap: enabled ? (paint ? onPaint : onErase) : null,
      onHold: enabled ? (paint ? onPaintAll : onEraseAll) : null,
      child: Semantics(
        label: paint ? strings.craftPaint : strings.craftErase,
        selected: (paint ? !eraser : eraser) && !pickingColor,
        button: true,
        child: Tooltip(
          message: paint ? strings.craftPaint : strings.craftErase,
          excludeFromSemantics: true,
          triggerMode: TooltipTriggerMode.manual,
          child: IconButton(
            key: ValueKey(paint ? 'craft-paint' : 'craft-erase'),
            onPressed: enabled ? (paint ? onPaint : onErase) : null,
            icon: paint
                ? const Icon(Icons.brush_outlined)
                : const LitchcraftEraserIcon(),
            style: IconButton.styleFrom(
              minimumSize: const Size(48, 48),
              foregroundColor: (paint ? !eraser : eraser) && !pickingColor
                  ? Theme.of(context).colorScheme.onSecondaryContainer
                  : Theme.of(context).colorScheme.onSurface,
              disabledForegroundColor: Theme.of(
                context,
              ).colorScheme.onSurface.withValues(alpha: 0.38),
              backgroundColor: (paint ? !eraser : eraser) && !pickingColor
                  ? Theme.of(context).colorScheme.secondaryContainer
                  : null,
            ),
          ),
        ),
      ),
    );
    void openMenu(BuildContext anchorContext, MenuController menu) {
      final anchor = anchorContext.findRenderObject()! as RenderBox;
      final overlay =
          Overlay.of(
                anchorContext,
                rootOverlay: true,
              ).context.findRenderObject()!
              as RenderBox;
      final origin = anchor.localToGlobal(Offset.zero, ancestor: overlay);
      final safeLeft = math.max(8.0, rootSafeInsets.left);
      final safeTop = math.max(8.0, rootSafeInsets.top);
      final safeRight = math.max(8.0, rootSafeInsets.right);
      final safeBottom = math.max(8.0, rootSafeInsets.bottom);
      final preferredLeft = openTowardStart
          ? origin.dx - menuWidth
          : origin.dx + anchor.size.width;
      final left = preferredLeft
          .clamp(safeLeft, overlay.size.width - safeRight - menuWidth)
          .toDouble();
      final top = compactMenu
          ? (origin.dy + anchor.size.height / 2 - compactMenuHeight / 2)
                .clamp(
                  safeTop,
                  overlay.size.height - safeBottom - compactMenuHeight,
                )
                .toDouble()
          : origin.dy;
      menu.open(position: Offset(left - origin.dx, top - origin.dy));
    }

    return MenuAnchor(
      controller: menuController,
      consumeOutsideTap: false,
      useRootOverlay: true,
      crossAxisUnconstrained: false,
      reservedPadding: rootSafeInsets.add(const EdgeInsets.all(8)),
      onOpen: onOpen,
      onClose: onClose,
      style: MenuStyle(
        fixedSize: compactMenu
            ? WidgetStatePropertyAll(Size(menuWidth, compactMenuHeight))
            : null,
        maximumSize: WidgetStatePropertyAll(Size(menuWidth, menuMaxHeight)),
      ),
      menuChildren: <Widget>[
        Padding(
          key: const ValueKey('craft-tool-tray'),
          padding: const EdgeInsets.fromLTRB(8, 8, 8, 4),
          child: compactMenu
              ? Wrap(
                  spacing: 4,
                  runSpacing: 4,
                  children: <Widget>[
                    compactModeAction(paint: true),
                    compactModeAction(paint: false),
                    Semantics(
                      selected: pickingColor,
                      button: true,
                      child: IconButton(
                        key: const ValueKey('craft-pick-color'),
                        tooltip: strings.craftPickColor,
                        onPressed: enabled ? onPickColor : null,
                        icon: Icon(
                          pickingColor
                              ? Icons.colorize
                              : Icons.colorize_outlined,
                        ),
                        style: IconButton.styleFrom(
                          minimumSize: const Size(48, 48),
                          foregroundColor: pickingColor
                              ? Theme.of(
                                  context,
                                ).colorScheme.onSecondaryContainer
                              : Theme.of(context).colorScheme.onSurface,
                          disabledForegroundColor: Theme.of(
                            context,
                          ).colorScheme.onSurface.withValues(alpha: 0.38),
                          backgroundColor: pickingColor
                              ? Theme.of(context).colorScheme.secondaryContainer
                              : null,
                        ),
                      ),
                    ),
                  ],
                )
              : Row(
                  children: <Widget>[
                    Expanded(child: modeAction(paint: true)),
                    const SizedBox(width: 8),
                    Expanded(child: modeAction(paint: false)),
                  ],
                ),
        ),
        if (!compactMenu)
          MenuItemButton(
            key: const ValueKey('craft-pick-color'),
            leadingIcon: const Icon(Icons.colorize),
            trailingIcon: pickingColor ? const Icon(Icons.check) : null,
            onPressed: enabled ? onPickColor : null,
            child: Text(strings.craftPickColor),
          ),
        const Divider(height: 1),
        Padding(
          key: const ValueKey('craft-target-row'),
          padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
          child: Semantics(
            container: true,
            explicitChildNodes: true,
            label: strings.craftTargetHelp,
            child: Wrap(
              spacing: 4,
              runSpacing: 4,
              children: <Widget>[
                for (var index = 0; index < targets.length; index++)
                  MergeSemantics(
                    child: Semantics(
                      label: litchcraftTargetLabel(
                        context,
                        targets[index],
                        flipped: flipped,
                      ),
                      selected: targetMask == targets[index],
                      child: Tooltip(
                        message: litchcraftTargetLabel(
                          context,
                          targets[index],
                          flipped: flipped,
                        ),
                        excludeFromSemantics: true,
                        child: IconButton(
                          key: ValueKey('craft-target-${targets[index]}'),
                          onPressed: enabled
                              ? () => onTarget(targets[index])
                              : null,
                          isSelected: targetMask == targets[index],
                          icon: LitchcraftTargetIcon(
                            mask: targets[index],
                            topology: topology,
                            flipped: flipped,
                            color: color,
                          ),
                          selectedIcon: Stack(
                            clipBehavior: Clip.none,
                            children: <Widget>[
                              LitchcraftTargetIcon(
                                mask: targets[index],
                                topology: topology,
                                flipped: flipped,
                                color: color,
                              ),
                              const Positioned(
                                right: -6,
                                bottom: -6,
                                child: Icon(Icons.check_circle, size: 16),
                              ),
                            ],
                          ),
                          style: IconButton.styleFrom(
                            minimumSize: const Size(48, 48),
                            backgroundColor: targetMask == targets[index]
                                ? Theme.of(
                                    context,
                                  ).colorScheme.secondaryContainer
                                : null,
                          ),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ],
      builder: (context, menu, child) => Semantics(
        key: const ValueKey('craft-tool-state'),
        expanded: menu.isOpen,
        button: true,
        label: strings.craftPaintErase,
        value: selectedLabel,
        child: IconButton(
          key: const ValueKey('craft-tool'),
          tooltip: selectedLabel,
          onPressed: enabled
              ? () => menu.isOpen ? menu.close() : openMenu(context, menu)
              : null,
          icon: Stack(
            clipBehavior: Clip.none,
            children: <Widget>[
              if (eraser)
                const LitchcraftEraserIcon()
              else
                const Icon(Icons.brush_outlined),
              Positioned(
                right: -8,
                bottom: -8,
                child: DecoratedBox(
                  key: const ValueKey('craft-target'),
                  decoration: BoxDecoration(
                    color: Theme.of(context).colorScheme.surfaceContainerHigh,
                    shape: BoxShape.circle,
                  ),
                  child: Padding(
                    padding: const EdgeInsets.all(2),
                    child: LitchcraftTargetIcon(
                      mask: targetMask,
                      topology: topology,
                      flipped: flipped,
                      color: color,
                    ),
                  ),
                ),
              ),
            ],
          ),
          style: IconButton.styleFrom(
            minimumSize: const Size(48, 48),
            foregroundColor: Theme.of(context).colorScheme.onSurface,
            backgroundColor: Theme.of(context).colorScheme.surfaceContainerHigh,
          ),
        ),
      ),
    );
  }
}

class LitchcraftEraserIcon extends StatelessWidget {
  const LitchcraftEraserIcon({super.key, this.size = 24});

  final double size;

  @override
  Widget build(BuildContext context) => CustomPaint(
    size: Size.square(size),
    painter: _LitchcraftEraserPainter(
      IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
    ),
  );
}

class LitchcraftColorSwapIcon extends StatelessWidget {
  const LitchcraftColorSwapIcon({
    super.key,
    required this.current,
    required this.previous,
  });

  final int current;
  final int? previous;

  @override
  Widget build(BuildContext context) {
    final Color outline = Theme.of(context).colorScheme.outline;
    Widget dot(int color) => Container(
      width: 13,
      height: 13,
      decoration: BoxDecoration(
        color: litchcraftPreviewColor(color),
        shape: BoxShape.circle,
        border: Border.all(color: outline),
      ),
    );
    return SizedBox.square(
      dimension: 28,
      child: Stack(
        alignment: Alignment.center,
        children: <Widget>[
          Positioned(left: 1, top: 2, child: dot(current)),
          Positioned(right: 1, bottom: 2, child: dot(previous ?? current)),
          const Icon(Icons.swap_horiz_rounded, size: 22),
        ],
      ),
    );
  }
}

class _LitchcraftEraserPainter extends CustomPainter {
  const _LitchcraftEraserPainter(this.color);

  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final scale = size.shortestSide / 24;
    final paint = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.9 * scale
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    canvas
      ..save()
      ..translate(size.width / 2, size.height / 2)
      ..rotate(-math.pi / 4)
      ..drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromCenter(
            center: Offset.zero,
            width: 16 * scale,
            height: 9 * scale,
          ),
          Radius.circular(2.2 * scale),
        ),
        paint,
      )
      ..drawLine(
        Offset(2 * scale, -4.5 * scale),
        Offset(2 * scale, 4.5 * scale),
        paint,
      )
      ..restore();
  }

  @override
  bool shouldRepaint(_LitchcraftEraserPainter oldDelegate) =>
      oldDelegate.color != color;
}

/// A deliberate, user-tuned hold competes with the child's ordinary tap.
class LitchcraftHoldAction extends StatefulWidget {
  const LitchcraftHoldAction({
    super.key,
    required this.child,
    required this.holdLabel,
    required this.completedLabel,
    this.duration = const Duration(milliseconds: 750),
    this.showCompletionSnackBar = true,
    this.reserveProgressSpace = true,
    this.onTap,
    this.onHold,
  });
  final Widget child;
  final String holdLabel;
  final String completedLabel;
  final Duration duration;
  final bool showCompletionSnackBar;
  final bool reserveProgressSpace;
  final VoidCallback? onTap;
  final VoidCallback? onHold;

  /// Outer semantics may exclude this widget's node while retaining its action.
  static VoidCallback? withCompletionFeedback(
    BuildContext context,
    VoidCallback? action,
  ) => action == null
      ? null
      : () {
          if (!context.mounted) return;
          action();
          if (context.mounted) HapticFeedback.mediumImpact();
        };

  @override
  State<LitchcraftHoldAction> createState() => _LitchcraftHoldActionState();
}

class _LitchcraftHoldActionState extends State<LitchcraftHoldAction>
    with SingleTickerProviderStateMixin {
  late final AnimationController _progress = AnimationController(
    vsync: this,
    duration: widget.duration,
  );
  LongPressGestureRecognizer? _recognizer;
  Timer? _recognitionTimer;
  bool _focused = false;
  void _complete() {
    final complete = LitchcraftHoldAction.withCompletionFeedback(
      context,
      widget.onHold,
    );
    if (complete == null) return;
    _recognitionTimer?.cancel();
    complete();
    if (!mounted || !widget.showCompletionSnackBar) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(widget.completedLabel),
        duration: const Duration(seconds: 2),
      ),
    );
  }

  void _resetProgress() {
    _recognitionTimer?.cancel();
    _recognitionTimer = null;
    _progress.reset();
  }

  @override
  void dispose() {
    _recognitionTimer?.cancel();
    _progress.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(LitchcraftHoldAction oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.duration != widget.duration) {
      _progress.duration = widget.duration;
      _resetProgress();
    }
    if (widget.onHold == null && oldWidget.onHold != null) {
      _resetProgress();
    }
  }

  @override
  Widget build(BuildContext context) => FocusableActionDetector(
    enabled: widget.onTap != null || widget.onHold != null,
    onShowFocusHighlight: (focused) {
      if (_focused != focused) setState(() => _focused = focused);
    },
    actions: <Type, Action<Intent>>{
      ActivateIntent: CallbackAction<ActivateIntent>(
        onInvoke: (_) {
          widget.onTap?.call();
          return null;
        },
      ),
    },
    child: Semantics(
      onTap: widget.onTap,
      onLongPress: widget.onHold == null ? null : _complete,
      hint: widget.holdLabel,
      child: Tooltip(
        message: widget.holdLabel,
        triggerMode: TooltipTriggerMode.manual,
        child: RawGestureDetector(
          key: ValueKey<int>(widget.duration.inMilliseconds),
          behavior: HitTestBehavior.opaque,
          gestures: {
            if (widget.onTap != null)
              TapGestureRecognizer:
                  GestureRecognizerFactoryWithHandlers<TapGestureRecognizer>(
                    TapGestureRecognizer.new,
                    (recognizer) => recognizer.onTap = widget.onTap,
                  ),
            if (widget.onHold != null)
              LongPressGestureRecognizer:
                  GestureRecognizerFactoryWithHandlers<
                    LongPressGestureRecognizer
                  >(
                    () => LongPressGestureRecognizer(duration: widget.duration),
                    (recognizer) {
                      _recognizer = recognizer;
                      recognizer.onLongPressDown = (_) {
                        _progress.forward(from: 0);
                        _recognitionTimer?.cancel();
                        // Leave quick taps and drags quiet without changing the
                        // action's deliberately longer completion deadline.
                        _recognitionTimer = Timer(kPressTimeout, () {
                          _recognitionTimer = null;
                          if (mounted && widget.onHold != null) {
                            HapticFeedback.lightImpact();
                          }
                        });
                      };
                      recognizer.onLongPressCancel = _resetProgress;
                      recognizer.onLongPressStart = (_) {
                        _complete();
                      };
                      recognizer.onLongPressEnd = (_) {
                        _resetProgress();
                      };
                    },
                  ),
          },
          child: Listener(
            // Pointer-up is the one completion signal that every tap/hold path
            // shares. Gesture-arena cancellation normally resets progress too,
            // but a swatch can rebuild or be recycled by the looping wheel
            // between down and recognizer cancellation. Reset here as the
            // lifecycle backstop so a tap can never leave a full hold bar.
            onPointerUp: (_) {
              if (mounted) _resetProgress();
            },
            onPointerMove: (event) {
              // A wheel may recycle this swatch while its drag is still held.
              if (!mounted) return;
              final box = context.findRenderObject() as RenderBox?;
              if (box != null &&
                  !(Offset.zero & box.size).contains(
                    box.globalToLocal(event.position),
                  )) {
                _recognizer?.rejectGesture(event.pointer);
                _resetProgress();
              }
            },
            onPointerCancel: (_) {
              if (mounted) _resetProgress();
            },
            child: AnimatedContainer(
              duration: const Duration(milliseconds: 120),
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(12),
                border: _focused
                    ? Border.all(
                        color: Theme.of(context).colorScheme.primary,
                        width: 2,
                      )
                    : null,
              ),
              child: AnimatedBuilder(
                animation: _progress,
                builder: (context, _) => Stack(
                  alignment: Alignment.center,
                  fit: StackFit.passthrough,
                  children: [
                    Padding(
                      padding: EdgeInsets.only(
                        top: widget.reserveProgressSpace ? 5 : 0,
                      ),
                      child: widget.child,
                    ),
                    if (_progress.value > 0)
                      Positioned(
                        left: 10,
                        right: 10,
                        top: 0,
                        child: LinearProgressIndicator(
                          key: const ValueKey('craft-hold-progress'),
                          value: _progress.value,
                          minHeight: 4,
                          borderRadius: BorderRadius.circular(2),
                        ),
                      ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  );
}

class LitchcraftAnimationBar extends StatelessWidget {
  const LitchcraftAnimationBar({
    super.key,
    required this.document,
    required this.selectedFrameIndex,
    required this.playing,
    this.playbackPending = false,
    required this.enabled,
    required this.frameEditingEnabled,
    required this.motionEnabled,
    required this.playbackVisible,
    required this.motionLabel,
    required this.playLabel,
    required this.pauseLabel,
    required this.previousFrameLabel,
    required this.nextFrameLabel,
    required this.addFrameLabel,
    required this.deleteFrameLabel,
    required this.motionDisabledHint,
    required this.frameLabel,
    required this.onOpenEffects,
    required this.onOpenFrameDetails,
    required this.onTogglePlayback,
    required this.onSelectFrame,
    required this.onAddFrame,
    required this.onDeleteFrame,
    this.onFrameLongPress,
    this.expanded = false,
    this.spacious = false,
    this.dockOnLeft,
    this.motionKey = const ValueKey('craft-motion'),
  });

  final LitchcraftAnimationDocument document;
  final int selectedFrameIndex;
  final bool playing;
  final bool playbackPending;
  final bool enabled;
  final bool frameEditingEnabled;
  final bool motionEnabled;
  final bool playbackVisible;
  final String motionLabel;
  final String playLabel;
  final String pauseLabel;
  final String previousFrameLabel;
  final String nextFrameLabel;
  final String addFrameLabel;
  final String deleteFrameLabel;
  final String motionDisabledHint;
  final String Function(int frame, int count) frameLabel;
  final VoidCallback onOpenEffects;
  final VoidCallback onOpenFrameDetails;
  final VoidCallback onTogglePlayback;
  final ValueChanged<int> onSelectFrame;
  final VoidCallback onAddFrame;
  final VoidCallback onDeleteFrame;
  final void Function(int index, Rect anchor)? onFrameLongPress;
  final bool expanded;
  final bool spacious;
  final bool? dockOnLeft;
  final Key motionKey;

  Widget _controlSemantics({
    required String label,
    required bool controlEnabled,
    required VoidCallback? onTap,
    required Widget child,
  }) => Semantics(
    label: label,
    button: true,
    enabled: controlEnabled,
    onTap: controlEnabled ? onTap : null,
    excludeSemantics: true,
    child: child,
  );

  @override
  Widget build(BuildContext context) {
    final frameCount = document.frames.length;
    final canGoBack = enabled && selectedFrameIndex != 0;
    final canGoForward = enabled && selectedFrameIndex != frameCount - 1;
    final play = _controlSemantics(
      label: playing ? pauseLabel : playLabel,
      controlEnabled: enabled && playbackVisible,
      onTap: onTogglePlayback,
      child: IconButton(
        key: const ValueKey('craft-preview'),
        tooltip: playing ? pauseLabel : playLabel,
        onPressed: enabled && playbackVisible ? onTogglePlayback : null,
        icon: Icon(
          playbackPending
              ? Icons.close
              : playing
              ? Icons.pause
              : Icons.play_arrow,
        ),
        style: IconButton.styleFrom(
          minimumSize: const Size(48, 48),
          foregroundColor: Colors.white,
          disabledForegroundColor: Colors.white38,
        ),
      ),
    );
    final first = _controlSemantics(
      label:
          '$previousFrameLabel. '
          '${frameLabel(1, frameCount)}',
      controlEnabled: canGoBack,
      onTap: canGoBack ? () => onSelectFrame(0) : null,
      child: IconButton(
        key: const ValueKey('craft-frame-previous'),
        tooltip: '$previousFrameLabel. ${frameLabel(1, frameCount)}',
        onPressed: canGoBack ? () => onSelectFrame(0) : null,
        icon: const Icon(Icons.skip_previous),
        style: IconButton.styleFrom(
          minimumSize: const Size(48, 48),
          foregroundColor: Colors.white,
          disabledForegroundColor: Colors.white38,
        ),
      ),
    );
    final last = _controlSemantics(
      label:
          '$nextFrameLabel. '
          '${frameLabel(frameCount, frameCount)}',
      controlEnabled: canGoForward,
      onTap: canGoForward ? () => onSelectFrame(frameCount - 1) : null,
      child: IconButton(
        key: const ValueKey('craft-frame-next'),
        tooltip: '$nextFrameLabel. ${frameLabel(frameCount, frameCount)}',
        onPressed: canGoForward ? () => onSelectFrame(frameCount - 1) : null,
        icon: const Icon(Icons.skip_next),
        style: IconButton.styleFrom(
          minimumSize: const Size(48, 48),
          foregroundColor: Colors.white,
          disabledForegroundColor: Colors.white38,
        ),
      ),
    );
    final strip = LitchcraftSwipeFrameStrip(
      document: document,
      selectedFrameIndex: selectedFrameIndex,
      playing: playing,
      enabled: enabled,
      frameLabel: frameLabel,
      onSelectFrame: onSelectFrame,
      onAddFrame: onAddFrame,
      addEnabled: frameEditingEnabled,
      spacious: spacious,
      dockOnLeft: dockOnLeft,
      onFrameLongPress: frameEditingEnabled ? onFrameLongPress : null,
    );
    return Material(
      key: const ValueKey('craft-animation-bar'),
      color: Colors.black,
      borderRadius: BorderRadius.circular(12),
      child: expanded
          ? Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                SizedBox(
                  key: const ValueKey('craft-motion-controls'),
                  height: 56,
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                    children: <Widget>[first, play, last],
                  ),
                ),
                strip,
              ],
            )
          : SizedBox(
              key: const ValueKey('craft-motion-controls'),
              // The controls retain 48 px targets; the four decorative pixels
              // are reclaimed so the full portrait animation panel fits beside
              // the same-width Play/Litchcraft board on safe-area-heavy phones.
              height: spacious ? 64 : 52,
              child: Row(
                children: <Widget>[
                  SizedBox(width: 56, child: play),
                  Expanded(child: strip),
                ],
              ),
            ),
    );
  }
}

class LitchcraftSwipeFrameStrip extends StatefulWidget {
  const LitchcraftSwipeFrameStrip({
    super.key,
    required this.document,
    required this.selectedFrameIndex,
    required this.playing,
    required this.enabled,
    required this.frameLabel,
    required this.onSelectFrame,
    this.onAddFrame,
    this.addEnabled = true,
    this.onFrameLongPress,
    this.spacious = false,
    this.dockOnLeft,
  });

  final LitchcraftAnimationDocument document;
  final int selectedFrameIndex;
  final bool playing;
  final bool enabled;
  final String Function(int frame, int count) frameLabel;
  final ValueChanged<int> onSelectFrame;
  final VoidCallback? onAddFrame;
  final bool addEnabled;
  final bool spacious;
  final bool? dockOnLeft;
  final void Function(int index, Rect anchor)? onFrameLongPress;

  @override
  State<LitchcraftSwipeFrameStrip> createState() =>
      _LitchcraftSwipeFrameStripState();
}

class _LitchcraftSwipeFrameStripState extends State<LitchcraftSwipeFrameStrip> {
  static const _itemExtent = 53.0;

  late final ScrollController _scroll = ScrollController(
    initialScrollOffset: widget.selectedFrameIndex * _itemExtent,
    onAttach: (position) {
      final viewport = _lastViewportDimension;
      if (!position.hasContentDimensions &&
          viewport != null &&
          widget.document.frames.length * _itemExtent <= viewport) {
        position.correctPixels(0);
      }
      position.isScrollingNotifier.addListener(_scrollActivityChanged);
    },
    onDetach: (position) =>
        position.isScrollingNotifier.removeListener(_scrollActivityChanged),
  );
  double? _lastViewportDimension;
  int? _lastFrameCount;
  bool _selectionNeedsReveal = false;

  double _offsetForIndex(int index) {
    return (index * _itemExtent)
        .clamp(
          _scroll.position.minScrollExtent,
          _scroll.position.maxScrollExtent,
        )
        .toDouble();
  }

  void _centerSelection(int index) {
    // Playback follows immediately; a reveal animation could lag a fast tick.
    _scroll.jumpTo(_offsetForIndex(index));
  }

  double _revealOffset(double viewport, double maximum) {
    final center = widget.selectedFrameIndex * _itemExtent;
    final room = math.max(0.0, (viewport - _itemExtent) / 2);
    return _scroll.position.pixels
        .clamp(center - room, center + room)
        .clamp(0.0, maximum)
        .toDouble();
  }

  void _revealSelection() {
    if (!_scroll.hasClients || _scroll.position.isScrollingNotifier.value) {
      return;
    }
    _selectionNeedsReveal = false;
    _scroll.jumpTo(
      _revealOffset(
        _scroll.position.viewportDimension,
        _scroll.position.maxScrollExtent,
      ),
    );
  }

  void _scrollActivityChanged() {
    if (_selectionNeedsReveal &&
        _scroll.hasClients &&
        !_scroll.position.isScrollingNotifier.value) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _revealSelection();
      });
    }
  }

  @override
  void didUpdateWidget(LitchcraftSwipeFrameStrip oldWidget) {
    super.didUpdateWidget(oldWidget);
    if ((oldWidget.selectedFrameIndex == widget.selectedFrameIndex &&
            oldWidget.document.frames.length == widget.document.frames.length &&
            oldWidget.playing == widget.playing) ||
        !_scroll.hasClients) {
      return;
    }
    if (!widget.playing) {
      _selectionNeedsReveal = true;
      if (oldWidget.document.frames.length == widget.document.frames.length) {
        _revealSelection();
      }
      return;
    }
    // Move before layout so the newly selected thumbnail is centered in the
    // same rendered frame, rather than flashing one position ahead first.
    if (oldWidget.document.frames.length == widget.document.frames.length) {
      _centerSelection(widget.selectedFrameIndex);
    }
    // A changed frame count uses the new extent in LayoutBuilder below.
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final frameCount = widget.document.frames.length;
    return SizedBox(
      key: const ValueKey('craft-frame-filmstrip'),
      height: widget.spacious ? 64 : 55,
      child: Row(
        // Only the fixed Add button follows the outside edge of the dock.
        textDirection: switch (widget.dockOnLeft) {
          true => TextDirection.rtl,
          false => TextDirection.ltr,
          null => null,
        },
        children: <Widget>[
          Expanded(
            child: LayoutBuilder(
              builder: (context, constraints) {
                final allFramesFit =
                    frameCount * _itemExtent <= constraints.maxWidth;
                if (_lastViewportDimension != constraints.maxWidth ||
                    _lastFrameCount != frameCount) {
                  final oldWidth = _lastViewportDimension;
                  // A fully fitted row has no scroll offset to preserve. On
                  // narrowing it, reveal the selection in the new scrollable
                  // row. Otherwise leave a manually browsed viewport alone.
                  if (_lastFrameCount != frameCount ||
                      (!allFramesFit &&
                          oldWidth != null &&
                          frameCount * _itemExtent <= oldWidth)) {
                    _selectionNeedsReveal = true;
                  }
                  _lastViewportDimension = constraints.maxWidth;
                  _lastFrameCount = frameCount;
                  if (_scroll.hasClients &&
                      (widget.playing ||
                          allFramesFit ||
                          (_selectionNeedsReveal &&
                              !_scroll.position.isScrollingNotifier.value))) {
                    // Apply topology changes against the incoming extent.
                    // A finger keeps the viewport until its scroll ends.
                    final offset = allFramesFit
                        ? 0.0
                        : widget.playing
                        ? widget.selectedFrameIndex * _itemExtent
                        : _revealOffset(
                            constraints.maxWidth,
                            (frameCount - 1) * _itemExtent,
                          );
                    _scroll.position.correctBy(
                      offset - _scroll.position.pixels,
                    );
                    _selectionNeedsReveal = false;
                  }
                  WidgetsBinding.instance.addPostFrameCallback((_) {
                    if (mounted && widget.playing && _scroll.hasClients) {
                      _centerSelection(widget.selectedFrameIndex);
                    } else if (mounted && _selectionNeedsReveal) {
                      _revealSelection();
                    }
                  });
                }
                final edgeSpace = allFramesFit
                    ? 0.0
                    : math.max(0.0, (constraints.maxWidth - _itemExtent) / 2);
                return ListView.builder(
                  key: const ValueKey('craft-frame-scroll-viewport'),
                  controller: _scroll,
                  scrollDirection: Axis.horizontal,
                  reverse:
                      widget.dockOnLeft != null &&
                      Directionality.of(context) == TextDirection.rtl,
                  physics: widget.playing
                      ? const NeverScrollableScrollPhysics()
                      : null,
                  padding: EdgeInsets.symmetric(horizontal: edgeSpace),
                  itemCount: frameCount,
                  itemExtent: _itemExtent,
                  // Build real thumbnails in the viewport and ordinary scroll
                  // cache, rather than all 256 controls on every playback tick.
                  itemBuilder: (_, index) => Builder(
                    builder: (frameContext) {
                      void openActions() {
                        final box =
                            frameContext.findRenderObject()! as RenderBox;
                        widget.onFrameLongPress!(
                          index,
                          box.localToGlobal(Offset.zero) & box.size,
                        );
                      }

                      return SizedBox(
                        width: _itemExtent,
                        child: Semantics(
                          selected:
                              !widget.playing &&
                              index == widget.selectedFrameIndex,
                          button: true,
                          enabled: widget.enabled,
                          label:
                              widget.playing &&
                                  index == widget.selectedFrameIndex
                              ? litchStrings(
                                  context,
                                ).craftPlayingFrame(index + 1)
                              : widget.frameLabel(index + 1, frameCount),
                          excludeSemantics: true,
                          onTap: widget.enabled
                              ? () => widget.onSelectFrame(index)
                              : null,
                          onLongPress:
                              LitchcraftHoldAction.withCompletionFeedback(
                                context,
                                widget.enabled &&
                                        widget.onFrameLongPress != null
                                    ? openActions
                                    : null,
                              ),
                          child: LitchcraftHoldAction(
                            duration: kLongPressTimeout,
                            holdLabel: litchStrings(context).craftDeleteFrame,
                            completedLabel: '',
                            showCompletionSnackBar: false,
                            reserveProgressSpace: false,
                            onTap: widget.enabled
                                ? () => widget.onSelectFrame(index)
                                : null,
                            onHold:
                                widget.enabled &&
                                    widget.onFrameLongPress != null
                                ? openActions
                                : null,
                            child: ExcludeFocus(
                              child: IconButton(
                                key: ValueKey('craft-select-frame-$index'),
                                // The outer gesture owns hold-to-edit. An
                                // IconButton tooltip would steal that hold.
                                isSelected:
                                    !widget.playing &&
                                    index == widget.selectedFrameIndex,
                                onPressed: widget.enabled
                                    ? () => widget.onSelectFrame(index)
                                    : null,
                                style: IconButton.styleFrom(
                                  minimumSize: const Size(53, 53),
                                  padding: EdgeInsets.zero,
                                  side: BorderSide(
                                    color:
                                        !widget.playing &&
                                            index == widget.selectedFrameIndex
                                        ? const Color(0xffc6e5e4)
                                        : Colors.transparent,
                                    width: 2,
                                  ),
                                  shape: RoundedRectangleBorder(
                                    borderRadius: BorderRadius.circular(7),
                                  ),
                                  backgroundColor:
                                      !widget.playing &&
                                          index == widget.selectedFrameIndex
                                      ? Theme.of(
                                          context,
                                        ).colorScheme.secondaryContainer
                                      : null,
                                ),
                                icon: Column(
                                  mainAxisSize: MainAxisSize.min,
                                  children: [
                                    Row(
                                      mainAxisSize: MainAxisSize.min,
                                      children: [
                                        if (widget.playing &&
                                            index == widget.selectedFrameIndex)
                                          Icon(
                                            Icons.play_arrow,
                                            size: 12,
                                            color: widget.enabled
                                                ? Colors.white
                                                : Colors.white38,
                                          ),
                                        Text(
                                          '${index + 1}',
                                          key: ValueKey(
                                            'craft-frame-number-$index',
                                          ),
                                          style: TextStyle(
                                            fontSize: widget.spacious ? 11 : 9,
                                            height: 1,
                                            color:
                                                !widget.playing &&
                                                    index ==
                                                        widget
                                                            .selectedFrameIndex
                                                ? const Color(0xff263c40)
                                                : Colors.white,
                                          ),
                                        ),
                                      ],
                                    ),
                                    const SizedBox(height: 2),
                                    LitchcraftFrameThumbnail(
                                      document: widget.document,
                                      frameIndex: index,
                                      size: widget.spacious ? 36 : 28,
                                    ),
                                  ],
                                ),
                              ),
                            ),
                          ),
                        ),
                      );
                    },
                  ),
                );
              },
            ),
          ),
          if (widget.onAddFrame != null)
            SizedBox(
              width: _itemExtent,
              child: IconButton(
                key: const ValueKey('craft-add-frame'),
                tooltip: litchStrings(context).craftNewFrame,
                onPressed:
                    widget.enabled &&
                        widget.addEnabled &&
                        frameCount < litchcraftAnimationMaximumFrameCount
                    ? widget.onAddFrame
                    : null,
                style: IconButton.styleFrom(
                  minimumSize: const Size(53, 53),
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  side: const BorderSide(color: Colors.white38),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(7),
                  ),
                ),
                icon: const Icon(Icons.add, size: 32),
              ),
            ),
        ],
      ),
    );
  }
}

/// A compact source-RGB view of the actual frame, not a representative swatch.
/// It remains legible as an 8-by-8 artwork map at the 48 px filmstrip target.
class LitchcraftFrameThumbnail extends StatelessWidget {
  const LitchcraftFrameThumbnail({
    super.key,
    required this.document,
    required this.frameIndex,
    this.size = 28,
  });

  final LitchcraftAnimationDocument document;
  final int frameIndex;
  final double size;

  @override
  Widget build(BuildContext context) {
    // Filmstrip is an editing affordance, so show intended source RGB rather
    // than a temporal trough (Fade begins fully dark) that hides distinctions.
    final colors = document.frames[frameIndex].emitters
        .map((emitter) => emitter.color)
        .toList(growable: false);
    return Semantics(
      label: litchStrings(context).craftFrameNumber(frameIndex + 1),
      image: true,
      child: CustomPaint(
        size: Size.square(size),
        painter: _LitchcraftFrameThumbnailPainter(colors),
      ),
    );
  }
}

class _LitchcraftFrameThumbnailPainter extends CustomPainter {
  const _LitchcraftFrameThumbnailPainter(this.colors);

  final List<int> colors;

  @override
  void paint(Canvas canvas, Size size) {
    final cell = size.width / 8;
    final paint = Paint();
    for (var square = 0; square < 64; square++) {
      final base = square * 4;
      final red =
          (colors[base] >> 16 & 255) +
          (colors[base + 1] >> 16 & 255) +
          (colors[base + 2] >> 16 & 255) +
          (colors[base + 3] >> 16 & 255);
      final green =
          (colors[base] >> 8 & 255) +
          (colors[base + 1] >> 8 & 255) +
          (colors[base + 2] >> 8 & 255) +
          (colors[base + 3] >> 8 & 255);
      final blue =
          (colors[base] & 255) +
          (colors[base + 1] & 255) +
          (colors[base + 2] & 255) +
          (colors[base + 3] & 255);
      paint.color = litchcraftPreviewColor(
        (red ~/ 4 << 16) | (green ~/ 4 << 8) | blue ~/ 4,
      );
      canvas.drawRect(
        Rect.fromLTWH((square % 8) * cell, (square ~/ 8) * cell, cell, cell),
        paint,
      );
    }
    paint
      ..color = const Color(0xff6b6b6b)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1;
    canvas.drawRRect(
      RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(3)),
      paint,
    );
  }

  @override
  bool shouldRepaint(_LitchcraftFrameThumbnailPainter oldDelegate) =>
      !identical(colors, oldDelegate.colors);
}

@immutable
class LitchcraftColorPickerResult {
  const LitchcraftColorPickerResult(this.color, {this.fillCanvas = false});

  final int color;
  final bool fillCanvas;
}

Future<LitchcraftColorPickerResult?> showLitchcraftColorPicker(
  BuildContext context,
  int color, {
  List<int> recentColors = const <int>[],
}) => showModalBottomSheet<LitchcraftColorPickerResult>(
  context: context,
  isScrollControlled: true,
  useSafeArea: true,
  showDragHandle: true,
  builder: (_) => _CraftColorPicker(initial: color, recentColors: recentColors),
);

class _CraftColorPicker extends StatefulWidget {
  const _CraftColorPicker({required this.initial, required this.recentColors});
  final int initial;
  final List<int> recentColors;
  @override
  State<_CraftColorPicker> createState() => _CraftColorPickerState();
}

class _CraftColorPickerState extends State<_CraftColorPicker> {
  static const double _minimumValue = .04;
  late HSVColor _color = HSVColor.fromColor(
    Color(
      0xff000000 |
          (widget.initial == 0
              ? litchcraftDefaultFavoriteColors.first
              : widget.initial),
    ),
  );
  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 0, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Text(
            strings.craftCustomColor,
            style: Theme.of(context).textTheme.headlineSmall,
          ),
          const SizedBox(height: 12),
          _swatchGrid(context, <int>[
            ...litchcraftPalette,
            ...widget.recentColors,
          ]),
          const SizedBox(height: 16),
          _colorField(),
          const SizedBox(height: 12),
          Row(
            children: <Widget>[
              Semantics(
                label:
                    '#${(_color.toColor().toARGB32() & 0xffffff).toRadixString(16).padLeft(6, '0')}',
                child: Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: litchcraftPreviewColor(
                      _color.toColor().toARGB32() & 0xffffff,
                    ),
                    shape: BoxShape.circle,
                    border: Border.all(
                      color: Theme.of(context).colorScheme.outline,
                    ),
                  ),
                ),
              ),
              const SizedBox(width: 16),
              Expanded(
                child: Text(
                  '#${(_color.toColor().toARGB32() & 0xffffff).toRadixString(16).padLeft(6, '0').toUpperCase()}',
                  style: Theme.of(context).textTheme.titleMedium,
                ),
              ),
            ],
          ),
          _slider(
            strings.craftHue,
            _color.hue,
            360,
            (value) => _color = _color.withHue(value),
            _previewHueGradient,
          ),
          ExpansionTile(
            tilePadding: EdgeInsets.zero,
            title: Text(strings.craftFineColor),
            children: <Widget>[
              _slider(
                strings.craftSaturation,
                _color.saturation,
                1,
                (value) => _color = _color.withSaturation(value),
                _previewGradient(_color.withSaturation),
              ),
              _slider(
                strings.craftValue,
                _color.value,
                1,
                (value) => _color = _color.withValue(value),
                _previewGradient(
                  (fraction) => _color.withValue(
                    _minimumValue + (1 - _minimumValue) * fraction,
                  ),
                ),
                min: _minimumValue,
              ),
            ],
          ),
          Row(
            children: <Widget>[
              Expanded(
                child: OutlinedButton.icon(
                  key: const ValueKey('craft-picker-fill-canvas'),
                  onPressed: () => Navigator.of(context).pop(
                    LitchcraftColorPickerResult(
                      _color.toColor().toARGB32() & 0xffffff,
                      fillCanvas: true,
                    ),
                  ),
                  icon: const Icon(Icons.format_color_fill),
                  label: Text(strings.craftFillCanvas),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: FilledButton(
                  onPressed: () => Navigator.of(context).pop(
                    LitchcraftColorPickerResult(
                      _color.toColor().toARGB32() & 0xffffff,
                    ),
                  ),
                  child: Text(strings.craftUseColor),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _colorField() => LayoutBuilder(
    builder: (context, bounds) {
      const height = 156.0;
      void change(Offset point) => setState(() {
        _color = _color
            .withSaturation((point.dx / bounds.maxWidth).clamp(0, 1))
            .withValue((1 - point.dy / height).clamp(_minimumValue, 1));
      });
      return ExcludeSemantics(
        // The hue slider and expandable precision sliders provide keyboard and
        // screen-reader equivalents for this two-dimensional touch surface.
        child: GestureDetector(
          key: const ValueKey('craft-color-field'),
          behavior: HitTestBehavior.opaque,
          onTapDown: (event) => change(event.localPosition),
          onPanDown: (event) => change(event.localPosition),
          onPanUpdate: (event) => change(event.localPosition),
          child: SizedBox(
            height: height,
            child: ClipRRect(
              borderRadius: BorderRadius.circular(12),
              child: Stack(
                children: <Widget>[
                  Positioned.fill(
                    child: CustomPaint(
                      painter: _ColorFieldPainter(
                        _color.hue,
                        minimumValue: _minimumValue,
                      ),
                    ),
                  ),
                  _ColorFieldSelection(
                    color: _color,
                    width: bounds.maxWidth,
                    height: height,
                  ),
                ],
              ),
            ),
          ),
        ),
      );
    },
  );

  Widget _swatchGrid(BuildContext context, List<int> colors) {
    final unique = <int>[];
    for (final color in colors) {
      if (color == 0) continue;
      if (!unique.contains(color)) unique.add(color);
    }
    return Wrap(
      spacing: 12,
      runSpacing: 12,
      children: <Widget>[
        for (final value in unique)
          Semantics(
            button: true,
            selected: (_color.toColor().toARGB32() & 0xffffff) == value,
            label: _litchcraftColorName(context, value),
            child: InkResponse(
              key: ValueKey('craft-picker-color-$value'),
              radius: 28,
              onTap: () =>
                  Navigator.of(context).pop(LitchcraftColorPickerResult(value)),
              child: Container(
                width: 48,
                height: 48,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: litchcraftPreviewColor(value),
                  border: Border.all(
                    width: (_color.toColor().toARGB32() & 0xffffff) == value
                        ? 3
                        : 1,
                    color: Theme.of(context).colorScheme.outline,
                  ),
                ),
              ),
            ),
          ),
      ],
    );
  }

  Widget _slider(
    String label,
    double value,
    double max,
    ValueChanged<double> change,
    Gradient gradient, {
    double min = 0,
  }) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: <Widget>[
      const SizedBox(height: 12),
      Text(label),
      SizedBox(
        height: 48,
        child: Stack(
          alignment: Alignment.center,
          children: <Widget>[
            Positioned(
              left: 24,
              right: 24,
              child: Container(
                height: 5,
                decoration: BoxDecoration(
                  gradient: gradient,
                  borderRadius: BorderRadius.circular(3),
                ),
              ),
            ),
            SliderTheme(
              data: SliderTheme.of(context).copyWith(
                trackHeight: 5,
                activeTrackColor: Colors.transparent,
                inactiveTrackColor: Colors.transparent,
              ),
              child: Slider(
                value: value,
                min: min,
                max: max,
                label: '${(value / max * 100).round()}%',
                onChanged: (value) => setState(() => change(value)),
              ),
            ),
          ],
        ),
      ),
    ],
  );
}

String _litchcraftColorName(BuildContext context, int value) {
  final strings = litchStrings(context);
  final index = litchcraftPalette.indexOf(value);
  if (index < 0) {
    return '#${value.toRadixString(16).padLeft(6, '0').toUpperCase()}';
  }
  return <String>[
    strings.craftColorOrange,
    strings.craftColorPink,
    strings.colorGold,
    strings.colorGreen,
    strings.colorTeal,
    strings.colorBlue,
    strings.colorViolet,
    strings.craftColorWhite,
  ][index];
}

/// Keeps square-light selection attached to its toolbar control in compact
/// two-pane layouts. Portrait keeps the larger diagram sheet below.
class LitchcraftDetailButton extends StatelessWidget {
  const LitchcraftDetailButton({
    super.key,
    required this.targetMask,
    required this.topology,
    required this.flipped,
    required this.color,
    required this.enabled,
    required this.safePadding,
    required this.onSelected,
    required this.onOpen,
    required this.onClosed,
  });

  final int targetMask;
  final LitchcraftTopology topology;
  final bool flipped;
  final int color;
  final bool enabled;
  final EdgeInsets safePadding;
  final ValueChanged<int> onSelected;
  final VoidCallback onOpen;
  final VoidCallback onClosed;

  Future<void> _open(BuildContext context) async {
    onOpen();
    final targets = <int>[...topology.targets, 15];
    final button = context.findRenderObject()! as RenderBox;
    final overlay =
        Overlay.of(context, rootOverlay: true).context.findRenderObject()!
            as RenderBox;
    final origin = button.localToGlobal(Offset.zero, ancestor: overlay);
    const trayWidth = 216.0;
    const trayHeight = 240.0;
    final safeLeft = math.max(8.0, safePadding.left);
    final safeTop = math.max(8.0, safePadding.top);
    final safeRight = math.max(8.0, safePadding.right);
    final safeBottom = math.max(8.0, safePadding.bottom);
    final left = (origin.dx + button.size.width - trayWidth)
        .clamp(safeLeft, overlay.size.width - safeRight - trayWidth)
        .toDouble();
    final top = (origin.dy - 16)
        .clamp(safeTop, overlay.size.height - safeBottom - trayHeight)
        .toDouble();
    int? selected;
    try {
      selected = await showMenu<int>(
        context: context,
        useRootNavigator: true,
        requestFocus: true,
        constraints: const BoxConstraints.tightFor(width: trayWidth),
        position: RelativeRect.fromLTRB(
          left,
          top,
          overlay.size.width - left - trayWidth,
          overlay.size.height - top - trayHeight,
        ),
        items: <PopupMenuEntry<int>>[
          for (final mask in targets)
            PopupMenuItem<int>(
              key: ValueKey('craft-target-$mask'),
              value: mask,
              height: 48,
              child: Semantics(
                selected: targetMask == mask,
                child: Row(
                  children: <Widget>[
                    LitchcraftTargetIcon(
                      mask: mask,
                      topology: topology,
                      flipped: flipped,
                      color: color,
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        litchcraftTargetLabel(context, mask, flipped: flipped),
                      ),
                    ),
                    if (targetMask == mask) const Icon(Icons.check, size: 20),
                  ],
                ),
              ),
            ),
        ],
      );
    } finally {
      onClosed();
    }
    if (selected != null) onSelected(selected);
  }

  @override
  Widget build(BuildContext context) => IconButton(
    key: const ValueKey('craft-target'),
    tooltip: litchStrings(context).craftDetail,
    onPressed: enabled ? () => _open(context) : null,
    icon: LitchcraftTargetIcon(
      mask: targetMask,
      topology: topology,
      flipped: flipped,
      color: color,
    ),
    style: IconButton.styleFrom(minimumSize: const Size(48, 48)),
  );
}

Future<int?> showLitchcraftDetailPicker(
  BuildContext context,
  LitchcraftTopology topology,
  int targetMask, {
  bool flipped = false,
  int color = 0xff7430,
}) => showModalBottomSheet<int>(
  context: context,
  isScrollControlled: true,
  useSafeArea: true,
  showDragHandle: true,
  builder: (context) {
    final strings = litchStrings(context);
    String label(int mask) =>
        litchcraftTargetLabel(context, mask, flipped: flipped);
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(20, 0, 20, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Text(
            strings.craftDetail,
            style: Theme.of(context).textTheme.headlineSmall,
          ),
          const SizedBox(height: 8),
          Text(strings.craftTargetHelp),
          const SizedBox(height: 16),
          Center(
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 272),
              child: AspectRatio(
                aspectRatio: 1,
                child: LayoutBuilder(
                  builder: (context, bounds) => Stack(
                    children: <Widget>[
                      Positioned.fill(
                        child: CustomPaint(
                          painter: LitchcraftTargetPainter(
                            mask: targetMask,
                            topology: topology,
                            flipped: flipped,
                            color: color,
                          ),
                        ),
                      ),
                      for (final mask in topology.targets)
                        Positioned(
                          left:
                              litchcraftEmitterPosition(
                                    mask,
                                    topology,
                                    flipped,
                                  ).dx *
                                  bounds.maxWidth -
                              28,
                          top:
                              litchcraftEmitterPosition(
                                    mask,
                                    topology,
                                    flipped,
                                  ).dy *
                                  bounds.maxHeight -
                              28,
                          child: Semantics(
                            button: true,
                            selected: targetMask == mask,
                            label: label(mask),
                            child: Tooltip(
                              message: label(mask),
                              child: InkResponse(
                                key: ValueKey('craft-target-$mask'),
                                onTap: () => Navigator.of(context).pop(mask),
                                containedInkWell: true,
                                radius: 28,
                                child: SizedBox(
                                  width: 56,
                                  height: 56,
                                  child: targetMask == mask
                                      ? const Icon(
                                          Icons.check_circle,
                                          color: Colors.white,
                                          shadows: <Shadow>[
                                            Shadow(
                                              color: Colors.black,
                                              blurRadius: 3,
                                            ),
                                          ],
                                        )
                                      : null,
                                ),
                              ),
                            ),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            ),
          ),
          if (targetMask != 15) ...<Widget>[
            const SizedBox(height: 12),
            Text(
              label(targetMask),
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ],
          const SizedBox(height: 16),
          Semantics(
            selected: targetMask == 15,
            child: OutlinedButton.icon(
              key: const ValueKey('craft-target-15'),
              onPressed: () => Navigator.of(context).pop(15),
              icon: Icon(
                targetMask == 15
                    ? Icons.check_circle_outline
                    : Icons.crop_square,
              ),
              label: Text(strings.craftWholeSquare),
            ),
          ),
          const SizedBox(height: 8),
          Text(
            strings.craftTargetOrientation,
            textAlign: TextAlign.center,
            style: Theme.of(context).textTheme.bodySmall,
          ),
        ],
      ),
    );
  },
);

String litchcraftTargetLabel(
  BuildContext context,
  int mask, {
  bool flipped = false,
}) {
  final strings = litchStrings(context);
  final displayMask = litchcraftDisplayTargetMask(mask, flipped: flipped);
  return switch (displayMask) {
    1 => strings.craftLeftLight,
    2 => strings.craftTopLight,
    4 => strings.craftRightLight,
    8 => strings.craftBottomLight,
    10 => strings.craftCenterLight,
    _ => strings.craftWholeSquare,
  };
}

String litchcraftTargetShortLabel(
  BuildContext context,
  int mask, {
  bool flipped = false,
}) {
  final strings = litchStrings(context);
  final displayMask = litchcraftDisplayTargetMask(mask, flipped: flipped);
  return switch (displayMask) {
    1 => strings.craftTargetLeftShort,
    2 => strings.craftTargetTopShort,
    4 => strings.craftTargetRightShort,
    8 => strings.craftTargetBottomShort,
    10 => strings.craftTargetCenterShort,
    _ => strings.craftTargetAllShort,
  };
}

int litchcraftDisplayTargetMask(int mask, {bool flipped = false}) {
  // V1/V2's mask 10 is one averaged center emitter, not independently
  // addressable top and bottom bits. It remains centered in every view.
  if (mask == 10) return 10;
  final turns = flipped ? 2 : 0;
  var result = mask & 15;
  for (var turn = 0; turn < turns; turn++) {
    result = ((result << 1) & 15) | (result >> 3);
  }
  return result;
}

/// Physical emitter centers, shared by LED-detail hit targets and asset sampling.
Offset litchcraftEmitterPosition(
  int mask,
  LitchcraftTopology topology,
  bool flipped,
) {
  final displayMask = litchcraftDisplayTargetMask(mask, flipped: flipped);
  final position = topology == LitchcraftTopology.v3
      ? switch (displayMask) {
          1 => const Offset(.18, .5),
          2 => const Offset(.5, .18),
          4 => const Offset(.82, .5),
          _ => const Offset(.5, .82),
        }
      : switch (displayMask) {
          1 => const Offset(.18, .5),
          2 => const Offset(.5, .18),
          4 => const Offset(.82, .5),
          8 => const Offset(.5, .82),
          _ => const Offset(.5, .5),
        };
  return position;
}

class LitchcraftTargetIcon extends StatelessWidget {
  const LitchcraftTargetIcon({
    super.key,
    required this.mask,
    required this.topology,
    required this.flipped,
    required this.color,
    this.size = 28,
    this.multicolor = false,
  });

  final int mask;
  final LitchcraftTopology topology;
  final bool flipped;
  final int color;
  final double size;
  final bool multicolor;

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: size,
    child: CustomPaint(
      painter: _LitchcraftLedsGlyphPainter(
        IconTheme.of(context).color ?? Theme.of(context).colorScheme.onSurface,
        mask: mask,
        topology: topology,
        flipped: flipped,
        color: color,
        multicolor: multicolor,
      ),
    ),
  );
}

/// The single physical LED/acrylic/glow geometry owner. Semantic canvas wedges
/// remain an editing aid; read-only physical previews reuse this exact painter.
class LitchcraftTargetPainter extends CustomPainter {
  const LitchcraftTargetPainter({
    required this.mask,
    required this.topology,
    required this.flipped,
    required this.color,
    this.multicolor = false,
    this.emitterColors,
    this.emitterOffset = 0,
    this.artworkOnly = false,
  });

  final int mask;
  final LitchcraftTopology topology;
  final bool flipped;
  final int color;
  final bool multicolor;
  final List<int>? emitterColors;
  final int emitterOffset;
  final bool artworkOnly;

  // Coordinates are in the displayed octagon. Side emitters face away from
  // the board; top and bottom emitters face toward its center.
  static Path _emitterFace(int displayTarget) {
    final Rect rect = switch (displayTarget) {
      1 => const Rect.fromLTRB(6.1, 9, 7.45, 15),
      2 => const Rect.fromLTRB(9, 6.1, 15, 7.45),
      4 => const Rect.fromLTRB(16.55, 9, 17.9, 15),
      8 => const Rect.fromLTRB(9, 16.55, 15, 17.9),
      // Legacy topology has one averaged center emitter, not independently
      // addressable edge bars.
      _ => const Rect.fromLTRB(9, 9, 15, 15),
    };
    if (displayTarget == 10) {
      return Path()..addPolygon(const <Offset>[
        Offset(12, 8.5),
        Offset(15.5, 12),
        Offset(12, 15.5),
        Offset(8.5, 12),
      ], true);
    }
    return Path()
      ..addRRect(RRect.fromRectAndRadius(rect, const Radius.circular(.7)));
  }

  static Path _emitterGlow(int displayTarget) {
    final points = switch (displayTarget) {
      1 => const <Offset>[
        Offset(6.7, 9.15),
        Offset(6.7, 14.85),
        Offset(1.6, 14.15),
        Offset(1.6, 9.85),
      ],
      2 => const <Offset>[
        Offset(9.15, 6.7),
        Offset(14.85, 6.7),
        Offset(15.9, 14.1),
        Offset(8.1, 14.1),
      ],
      4 => const <Offset>[
        Offset(17.3, 9.15),
        Offset(22.4, 9.85),
        Offset(22.4, 14.15),
        Offset(17.3, 14.85),
      ],
      8 => const <Offset>[
        Offset(8.1, 9.9),
        Offset(15.9, 9.9),
        Offset(14.85, 17.3),
        Offset(9.15, 17.3),
      ],
      _ => const <Offset>[],
    };
    return Path()..addPolygon(points, true);
  }

  static (Offset, Offset) _glowAxis(int displayTarget) =>
      switch (displayTarget) {
        1 => (const Offset(6.7, 12), const Offset(1.6, 12)),
        2 => (const Offset(12, 6.7), const Offset(12, 14.1)),
        4 => (const Offset(17.3, 12), const Offset(22.4, 12)),
        8 => (const Offset(12, 17.3), const Offset(12, 9.9)),
        _ => (Offset.zero, Offset.zero),
      };

  Color _targetColor(int displayTarget) => litchcraftPreviewColor(
    emitterColors != null
        ? emitterColors![emitterOffset +
              switch (litchcraftDisplayTargetMask(
                displayTarget,
                flipped: flipped,
              )) {
                1 => 0,
                2 => 1,
                4 => 2,
                _ => 3,
              }]
        : multicolor
        ? switch (displayTarget) {
            1 => 0xff7430,
            2 => 0x22c3e2,
            4 => 0xae53ff,
            _ => 0x37d769,
          }
        : color,
  );

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    canvas.scale(size.width / 24, size.height / 24);
    final surround = Path()
      ..moveTo(2.8, .8)
      ..lineTo(21.2, .8)
      ..lineTo(23.2, 2.8)
      ..lineTo(23.2, 21.2)
      ..lineTo(21.2, 23.2)
      ..lineTo(2.8, 23.2)
      ..lineTo(.8, 21.2)
      ..lineTo(.8, 2.8)
      ..close();
    final paint = Paint()..isAntiAlias = true;
    if (!artworkOnly) {
      canvas.drawPath(surround, paint..color = const Color(0xff314146));
      canvas.drawPath(
        surround,
        paint
          ..color = const Color(0xff75888c)
          ..style = PaintingStyle.stroke
          ..strokeWidth = .28,
      );
      // The recessed acrylic cutout is a regular octagon: every edge has the
      // same length. Its scale matches the physical square inside the larger
      // chamfered board tile.
      const near = 5.5;
      const far = 18.5;
      const inset = 9.307611845;
      const farInset = 24 - inset;
      final cutout = Path()
        ..moveTo(inset, near)
        ..lineTo(farInset, near)
        ..lineTo(far, inset)
        ..lineTo(far, farInset)
        ..lineTo(farInset, far)
        ..lineTo(inset, far)
        ..lineTo(near, farInset)
        ..lineTo(near, inset)
        ..close();
      canvas.drawPath(cutout, paint..color = const Color(0xff162428));
      canvas.drawPath(
        cutout,
        paint
          ..color = const Color(0xff9eafb1)
          ..style = PaintingStyle.stroke
          ..strokeWidth = .275,
      );
    }
    for (final target in topology.targets) {
      final displayTarget = litchcraftDisplayTargetMask(
        target,
        flipped: flipped,
      );
      final active =
          mask == 15 || (target == 10 ? mask == 10 : (mask & target) != 0);
      final face = _emitterFace(displayTarget);
      final activeColor = _targetColor(displayTarget);
      if (active && (!artworkOnly || activeColor != Colors.black)) {
        if (displayTarget != 10) {
          final (begin, end) = _glowAxis(displayTarget);
          final side = displayTarget == 1 || displayTarget == 4;
          final glow = ui.Gradient.linear(
            begin,
            end,
            <Color>[
              activeColor.withValues(alpha: side ? .58 : .88),
              activeColor.withValues(alpha: side ? .15 : .25),
              activeColor.withValues(alpha: 0),
            ],
            const <double>[0, .58, 1],
          );
          canvas.drawPath(
            _emitterGlow(displayTarget),
            Paint()
              ..isAntiAlias = true
              ..shader = glow
              ..maskFilter = MaskFilter.blur(
                BlurStyle.normal,
                side ? 1.05 : 1.35,
              ),
          );
        }
        canvas.drawPath(
          face,
          paint
            ..style = PaintingStyle.fill
            ..color = activeColor,
        );
      }
      if (!artworkOnly) {
        canvas.drawPath(
          face,
          paint
            ..style = PaintingStyle.stroke
            ..strokeJoin = StrokeJoin.round
            ..strokeWidth = active ? .25 : .19
            ..color = active
                ? const Color(0xfff8f7f2)
                : const Color(0xff6f888d),
        );
      }
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(LitchcraftTargetPainter oldDelegate) =>
      oldDelegate.artworkOnly != artworkOnly ||
      oldDelegate.mask != mask ||
      oldDelegate.topology != topology ||
      oldDelegate.flipped != flipped ||
      oldDelegate.color != color ||
      oldDelegate.multicolor != multicolor ||
      !identical(oldDelegate.emitterColors, emitterColors) ||
      oldDelegate.emitterOffset != emitterOffset;
}
