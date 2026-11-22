import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../localization/litch_strings.dart';
import '../models/home_board_snapshot.dart';
import '../models/litch_piece_color.dart';
import '../models/litchcraft_state.dart';
export '../models/litchcraft_state.dart' show litchcraftLogicalSquareForDisplay;
import '../theme/litch_theme.dart';

/// Exact grid traversal retains cells crossed between sparse pointer events.
List<int> litchcraftSquaresAlongSegment(
  Offset from,
  Offset to,
  Size size, {
  bool flipped = false,
}) {
  Offset grid(Offset p) => Offset(
    (p.dx * 8 / size.width).clamp(0.0, 7.999999),
    (p.dy * 8 / size.height).clamp(0.0, 7.999999),
  );
  final a = grid(from), b = grid(to);
  var x = a.dx.floor(), y = a.dy.floor();
  final endX = b.dx.floor(), endY = b.dy.floor();
  final dx = b.dx - a.dx, dy = b.dy - a.dy;
  final stepX = dx.sign.toInt(), stepY = dy.sign.toInt();
  final deltaX = dx == 0 ? double.infinity : 1 / dx.abs();
  final deltaY = dy == 0 ? double.infinity : 1 / dy.abs();
  var maxX = dx == 0
      ? double.infinity
      : ((stepX > 0 ? x + 1 - a.dx : a.dx - x) / dx.abs());
  var maxY = dy == 0
      ? double.infinity
      : ((stepY > 0 ? y + 1 - a.dy : a.dy - y) / dy.abs());
  final result = <int>[];
  for (var count = 0; count < 64; count++) {
    final displaySquare = y * 8 + x;
    result.add(
      litchcraftLogicalSquareForDisplay(displaySquare, flipped: flipped),
    );
    if (x == endX && y == endY) break;
    // A pointer can remain captured after leaving the canvas. Both endpoints
    // are clamped, but an axis whose endpoint cell was already reached could
    // otherwise advance once more while the other axis catches up, producing
    // a transient -1 or 8 grid coordinate.
    if (x == endX) {
      y += stepY;
      maxY += deltaY;
    } else if (y == endY) {
      x += stepX;
      maxX += deltaX;
    } else if ((maxX - maxY).abs() < 1e-10) {
      x += stepX;
      y += stepY;
      maxX += deltaX;
      maxY += deltaY;
    } else if (maxX < maxY) {
      x += stepX;
      maxX += deltaX;
    } else {
      y += stepY;
      maxY += deltaY;
    }
  }
  return result;
}

/// Returns one artwork-rotation direction after crossing the twist threshold.
int litchcraftQuarterTurnDirectionForAngle(double radians) =>
    radians.abs() < math.pi / 4 ? 0 : (radians.isNegative ? -1 : 1);

/// Same rounded top/bottom average used by the firmware's legacy LED mapper.
/// Projection is presentation only; both source intent colors remain editable.
int litchcraftLegacyCenterColor(int top, int bottom) {
  int channel(int shift) =>
      (((top >> shift) & 255) + ((bottom >> shift) & 255) + 1) ~/ 2;
  return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

class LitchcraftCanvasView extends StatefulWidget {
  const LitchcraftCanvasView({
    super.key,
    required this.canvas,
    required this.onBegin,
    required this.onPaint,
    required this.onEnd,
    this.onInspectSquare,
    this.onEraseSquare,
    this.previewPaint,
    this.inspectLabel,
    this.paintLabel,
    this.cancelGestures,
    this.flipped = false,
    this.onRotateQuarterTurn,
    this.onRotationGestureChanged,
    this.enabled = true,
    this.effectLabels = const <int, String>{},
  });
  final LitchcraftCanvas canvas;
  final VoidCallback onBegin;
  final ValueChanged<Iterable<int>> onPaint;
  final VoidCallback onEnd;

  /// Selects a logical square without painting for the entire pointer gesture.
  final ValueChanged<int>? onInspectSquare;
  final ValueChanged<int>? onEraseSquare;

  /// Presents an undecided touch without editing. Return null when its captured
  /// tool or frame is no longer the commit target. The callback is latched until
  /// this pointer paints, holds to erase, rotates, or cancels.
  final LitchcraftCanvas? Function(LitchcraftCanvas base, int square)?
  previewPaint;
  final String? inspectLabel;
  final String? paintLabel;
  final Listenable? cancelGestures;
  final bool flipped;
  final ValueChanged<int>? onRotateQuarterTurn;
  final ValueChanged<bool>? onRotationGestureChanged;
  final bool enabled;

  /// Localized effect names for the displayed frame, keyed by logical square.
  final Map<int, String> effectLabels;
  @override
  State<LitchcraftCanvasView> createState() => _LitchcraftCanvasViewState();
}

class _LitchcraftCanvasViewState extends State<LitchcraftCanvasView> {
  int? _pointer;
  int? _secondPointer;
  Offset? _previous;
  bool _gestureFlipped = false;
  int? _previewSquare;
  LitchcraftCanvas? Function(LitchcraftCanvas base, int square)? _previewPaint;
  bool _inspectingPointer = false;
  bool _strokeStarted = false;
  bool _rotating = false;
  double? _lastRotationAngle;
  double _rotationTravel = 0;
  int _committedRotationTurns = 0;
  Timer? _holdTimer;
  Timer? _holdRecognitionTimer;
  Timer? _eraseFeedbackTimer;
  int? _eraseFeedbackSquare;
  int _eraseFeedbackGeneration = 0;
  final Map<int, Offset> _pointerLocations = <int, Offset>{};

  @override
  void initState() {
    super.initState();
    widget.cancelGestures?.addListener(_cancelGesture);
  }

  @override
  void didUpdateWidget(covariant LitchcraftCanvasView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.cancelGestures != widget.cancelGestures) {
      oldWidget.cancelGestures?.removeListener(_cancelGesture);
      widget.cancelGestures?.addListener(_cancelGesture);
    }
    if (_holdTimer != null &&
        (!widget.enabled || widget.onEraseSquare == null)) {
      _cancelGesture(notify: false);
    }
  }

  void _cancelHold() {
    _holdTimer?.cancel();
    _holdTimer = null;
    _holdRecognitionTimer?.cancel();
    _holdRecognitionTimer = null;
  }

  bool _canHold(int pointer) =>
      mounted &&
      _pointer == pointer &&
      !_strokeStarted &&
      !_rotating &&
      !_inspectingPointer &&
      widget.onEraseSquare != null &&
      _pendingTouchIsValid();

  void _cancelGesture({bool notify = true}) {
    _cancelHold();
    final wasRotating = _rotating;
    _clearPreview(notify: notify);
    _pointer = null;
    _secondPointer = null;
    _previous = null;
    _inspectingPointer = false;
    _strokeStarted = false;
    _rotating = false;
    _lastRotationAngle = null;
    _rotationTravel = 0;
    _committedRotationTurns = 0;
    _pointerLocations.clear();
    if (notify && wasRotating) widget.onRotationGestureChanged?.call(false);
  }

  void _clearPreview({bool notify = true}) {
    final hadPreview = _previewSquare != null && _previewPaint != null;
    _previewSquare = null;
    _previewPaint = null;
    if (notify && hadPreview && mounted) setState(() {});
  }

  bool _pendingTouchIsValid() {
    final square = _previewSquare;
    final preview = _previewPaint;
    if (!widget.enabled ||
        (square != null &&
            preview != null &&
            preview(widget.canvas, square) == null)) {
      _cancelGesture();
      return false;
    }
    return true;
  }

  @override
  void dispose() {
    widget.cancelGestures?.removeListener(_cancelGesture);
    _cancelGesture(notify: false);
    _eraseFeedbackTimer?.cancel();
    // Page departure explicitly finishes its controller stroke. Avoid notifying
    // another widget while Flutter is unmounting this subtree.
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, bounds) {
      final size = Size.square(bounds.maxWidth);
      var displayedCanvas = widget.canvas;
      final previewSquare = _previewSquare;
      final previewPaint = _previewPaint;
      if (previewSquare != null && previewPaint != null) {
        final preview = previewPaint(widget.canvas, previewSquare);
        if (preview == null || !widget.enabled) {
          _cancelGesture(notify: false);
        } else {
          displayedCanvas = preview;
        }
      }
      void apply(Offset location) {
        widget.onPaint(
          litchcraftSquaresAlongSegment(
            _previous ?? location,
            location,
            size,
            flipped: _gestureFlipped,
          ),
        );
        _previous = location;
      }

      void finish(PointerEvent event) {
        _cancelHold();
        if (_rotating &&
            (event.pointer == _pointer || event.pointer == _secondPointer)) {
          _cancelGesture();
          return;
        }
        if (_pointer != event.pointer) return;
        if (!_inspectingPointer && !_strokeStarted && widget.enabled) {
          if (!_pendingTouchIsValid()) return;
          _clearPreview();
          widget.onBegin();
          _strokeStarted = true;
          apply(event.localPosition);
        }
        _clearPreview();
        _pointerLocations.remove(event.pointer);
        _pointer = null;
        _previous = null;
        final wasInspecting = _inspectingPointer;
        _inspectingPointer = false;
        if (!wasInspecting && _strokeStarted) widget.onEnd();
        _strokeStarted = false;
      }

      void cancel(PointerEvent event) {
        _cancelHold();
        if (_rotating &&
            (event.pointer == _pointer || event.pointer == _secondPointer)) {
          _cancelGesture();
          return;
        }
        if (_pointer != event.pointer) return;
        if (!_inspectingPointer && _strokeStarted) widget.onEnd();
        _cancelGesture();
      }

      return AspectRatio(
        aspectRatio: 1,
        child: RawGestureDetector(
          key: const ValueKey('litchcraft-canvas'),
          behavior: HitTestBehavior.opaque,
          gestures: <Type, GestureRecognizerFactory>{
            EagerGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<EagerGestureRecognizer>(
                  EagerGestureRecognizer.new,
                  (_) {},
                ),
          },
          child: Listener(
            behavior: HitTestBehavior.opaque,
            onPointerDown: (event) {
              if (!widget.enabled) return;
              if (_pointer != null) {
                if (_secondPointer != null ||
                    _inspectingPointer ||
                    widget.onRotateQuarterTurn == null) {
                  return;
                }
                if (!_strokeStarted && !_pendingTouchIsValid()) return;
                if (_strokeStarted) widget.onEnd();
                _strokeStarted = false;
                _cancelHold();
                _clearPreview();
                _secondPointer = event.pointer;
                _pointerLocations[event.pointer] = event.localPosition;
                _rotating = true;
                _rotationTravel = 0;
                _committedRotationTurns = 0;
                _lastRotationAngle = _rotationAngle();
                widget.onRotationGestureChanged?.call(true);
                return;
              }
              _pointer = event.pointer;
              _gestureFlipped = widget.flipped;
              _pointerLocations[event.pointer] = event.localPosition;
              _strokeStarted = false;
              final inspect = widget.onInspectSquare;
              // Selection may immediately rebuild the editor in paint mode.
              // Keep this gesture owned by its original action until release.
              _inspectingPointer = inspect != null;
              if (inspect != null) {
                inspect(
                  litchcraftSquaresAlongSegment(
                    event.localPosition,
                    event.localPosition,
                    size,
                    flipped: widget.flipped,
                  ).single,
                );
                return;
              }
              _previous = event.localPosition;
              _previewSquare = litchcraftSquaresAlongSegment(
                event.localPosition,
                event.localPosition,
                size,
                flipped: _gestureFlipped,
              ).single;
              _previewPaint = widget.previewPaint;
              if (_previewPaint != null) setState(() {});
              if (widget.onEraseSquare case final erase?) {
                final pointer = event.pointer;
                final location = event.localPosition;
                _cancelHold();
                _holdRecognitionTimer = Timer(kPressTimeout, () {
                  _holdRecognitionTimer = null;
                  if (_canHold(pointer)) HapticFeedback.lightImpact();
                });
                _holdTimer = Timer(kLongPressTimeout, () {
                  _holdTimer = null;
                  if (!_canHold(pointer)) return;
                  final square = litchcraftSquaresAlongSegment(
                    location,
                    location,
                    size,
                    flipped: _gestureFlipped,
                  ).single;
                  _clearPreview();
                  _inspectingPointer = true;
                  erase(square);
                  if (!mounted) return;
                  HapticFeedback.mediumImpact();
                  _eraseFeedbackTimer?.cancel();
                  setState(() {
                    _eraseFeedbackSquare = square;
                    _eraseFeedbackGeneration++;
                  });
                  _eraseFeedbackTimer = Timer(
                    const Duration(milliseconds: 280),
                    () {
                      if (mounted) setState(() => _eraseFeedbackSquare = null);
                    },
                  );
                });
              }
            },
            onPointerMove: (event) {
              if (_rotating && _pointerLocations.containsKey(event.pointer)) {
                _pointerLocations[event.pointer] = event.localPosition;
                final double angle = _rotationAngle();
                final double previous = _lastRotationAngle ?? angle;
                double delta = angle - previous;
                if (delta > math.pi) delta -= math.pi * 2;
                if (delta < -math.pi) delta += math.pi * 2;
                _lastRotationAngle = angle;
                _rotationTravel += delta;
                final int direction = litchcraftQuarterTurnDirectionForAngle(
                  _rotationTravel,
                );
                if (_committedRotationTurns == 0 && direction != 0) {
                  widget.onRotateQuarterTurn?.call(direction);
                  _committedRotationTurns = direction;
                }
                return;
              }
              if (_pointer == event.pointer && !_inspectingPointer) {
                _pointerLocations[event.pointer] = event.localPosition;
                if (!_strokeStarted) {
                  final Offset origin = _previous ?? event.localPosition;
                  final squares = litchcraftSquaresAlongSegment(
                    origin,
                    event.localPosition,
                    size,
                    flipped: _gestureFlipped,
                  );
                  if (squares.length == 1 &&
                      (event.localPosition - origin).distance < kTouchSlop) {
                    return;
                  }
                  if (!_pendingTouchIsValid()) return;
                  _clearPreview();
                  _cancelHold();
                  widget.onBegin();
                  _strokeStarted = true;
                }
                apply(event.localPosition);
              }
            },
            onPointerUp: finish,
            onPointerCancel: cancel,
            child: Semantics(
              container: true,
              label: litchStrings(context).craftCanvas,
              hint: widget.enabled && widget.onInspectSquare != null
                  ? widget.inspectLabel
                  : null,
              child: Stack(
                children: <Widget>[
                  Positioned.fill(
                    child: LitchcraftThumbnail(
                      canvas: displayedCanvas,
                      flipped: widget.flipped,
                    ),
                  ),
                  for (var display = 0; display < 64; display++)
                    Positioned(
                      left: display % 8 * size.width / 8,
                      top: display ~/ 8 * size.height / 8,
                      width: size.width / 8,
                      height: size.height / 8,
                      child: _squareSemantics(
                        context,
                        litchcraftLogicalSquareForDisplay(
                          display,
                          flipped: widget.flipped,
                        ),
                      ),
                    ),
                  if (_eraseFeedbackSquare case final square?)
                    Positioned(
                      key: ValueKey(
                        'craft-erase-feedback-$_eraseFeedbackGeneration',
                      ),
                      left:
                          (widget.flipped ? 63 - square : square) %
                          8 *
                          size.width /
                          8,
                      top:
                          (widget.flipped ? 63 - square : square) ~/
                          8 *
                          size.height /
                          8,
                      width: size.width / 8,
                      height: size.height / 8,
                      child: IgnorePointer(
                        child: TweenAnimationBuilder<double>(
                          tween: Tween(begin: 1, end: 0),
                          duration: const Duration(milliseconds: 260),
                          builder: (context, value, _) => CustomPaint(
                            painter: _EraseCornerFeedbackPainter(value),
                          ),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ),
        ),
      );
    },
  );

  double _rotationAngle() {
    final Offset first = _pointerLocations[_pointer] ?? Offset.zero;
    final Offset second = _pointerLocations[_secondPointer] ?? first;
    final Offset delta = second - first;
    return math.atan2(delta.dy, delta.dx);
  }

  Widget _squareSemantics(BuildContext context, int square) {
    final colors = widget.canvas.colors.skip(square * 4).take(4).toList();
    final visibleColors = colors.where((color) => color != 0).toSet();
    final state = visibleColors.isEmpty
        ? litchStrings(context).craftOff
        : visibleColors.length > 1
        ? litchStrings(context).craftMultipleColors
        : '#${visibleColors.single.toRadixString(16).padLeft(6, '0').toUpperCase()}';
    return Semantics(
      key: ValueKey('craft-square-$square'),
      button: true,
      label: litchStrings(
        context,
      ).craftSquareSemantics(algebraicSquare(square), state),
      enabled: widget.enabled,
      value: widget.effectLabels[square],
      onTapHint: !widget.enabled
          ? null
          : widget.onInspectSquare != null
          ? widget.inspectLabel
          : widget.paintLabel ?? litchStrings(context).craftPaint,
      onTap: widget.enabled
          ? () {
              final inspect = widget.onInspectSquare;
              if (inspect != null) {
                inspect(square);
                return;
              }
              widget.onBegin();
              widget.onPaint(<int>[square]);
              widget.onEnd();
            }
          : null,
      child: widget.effectLabels.containsKey(square)
          ? IgnorePointer(
              child: Align(
                alignment: Alignment.topRight,
                child: Padding(
                  padding: const EdgeInsets.all(3),
                  child: Icon(
                    Icons.auto_awesome,
                    key: ValueKey('craft-effect-marker-$square'),
                    size: 10,
                    color: Colors.white,
                    shadows: const [Shadow(color: Colors.black, blurRadius: 2)],
                  ),
                ),
              ),
            )
          : const SizedBox.expand(),
    );
  }
}

class _EraseCornerFeedbackPainter extends CustomPainter {
  const _EraseCornerFeedbackPainter(this.progress);
  final double progress;

  @override
  void paint(Canvas canvas, Size size) {
    final inset = size.shortestSide * (0.12 + (1 - progress) * 0.14);
    final travel = size.shortestSide * 0.25;
    final paint = Paint()
      ..color = const Color(0xffd9f3f2).withValues(alpha: progress * 0.8)
      ..strokeWidth = 2
      ..strokeCap = StrokeCap.round;
    final corners = <(Offset, Offset)>[
      (Offset(inset, inset), Offset(inset + travel, inset + travel)),
      (
        Offset(size.width - inset, inset),
        Offset(size.width - inset - travel, inset + travel),
      ),
      (
        Offset(inset, size.height - inset),
        Offset(inset + travel, size.height - inset - travel),
      ),
      (
        Offset(size.width - inset, size.height - inset),
        Offset(size.width - inset - travel, size.height - inset - travel),
      ),
    ];
    for (final stroke in corners) {
      canvas.drawLine(stroke.$1, stroke.$2, paint);
    }
  }

  @override
  bool shouldRepaint(_EraseCornerFeedbackPainter oldDelegate) =>
      oldDelegate.progress != progress;
}

class LitchcraftThumbnail extends StatelessWidget {
  const LitchcraftThumbnail({
    super.key,
    required this.canvas,
    this.flipped = false,
    this.squarePreview = false,
    this.blackBackground = false,
  });
  final LitchcraftCanvas canvas;
  final bool flipped;
  final bool squarePreview;
  final bool blackBackground;
  @override
  Widget build(BuildContext context) => ClipPath(
    clipper: ShapeBorderClipper(
      shape: BeveledRectangleBorder(
        borderRadius: BorderRadius.circular(LitchShapes.boardChamfer),
      ),
    ),
    child: CustomPaint(
      painter: _CanvasPainter(
        canvas,
        flipped,
        squarePreview: squarePreview,
        blackBackground: blackBackground,
      ),
      child: const AspectRatio(aspectRatio: 1),
    ),
  );
}

class _CanvasPainter extends CustomPainter {
  _CanvasPainter(
    this.canvas,
    this.flipped, {
    this.squarePreview = false,
    this.blackBackground = false,
    this.displayColors = true,
  });
  final LitchcraftCanvas canvas;
  final bool flipped;
  final bool squarePreview;
  final bool blackBackground;
  final bool displayColors;
  Color color(int rgb, bool dark) => rgb == 0
      ? (blackBackground
            ? Colors.black
            : dark
            ? const Color(0xff26383e)
            : const Color(0xff33494e))
      : displayColors
      ? litchcraftPreviewColor(rgb)
      : Color(0xff000000 | (rgb & 0xffffff));

  @override
  void paint(Canvas target, Size size) {
    final paint = Paint()..isAntiAlias = true;
    final side = size.width / 8;
    target.drawRect(
      Offset.zero & size,
      paint..color = blackBackground ? Colors.black : const Color(0xff162629),
    );
    for (var display = 0; display < 64; display++) {
      final square = litchcraftLogicalSquareForDisplay(
        display,
        flipped: flipped,
      );
      final left = display % 8 * side, top = display ~/ 8 * side;
      final rect = Rect.fromLTWH(left + 1, top + 1, side - 2, side - 2);
      final dark = litchcraftDisplaySquareIsDark(display);
      final colors = canvas.colors.sublist(square * 4, square * 4 + 4);
      if (squarePreview) {
        final red = colors.fold<int>(0, (sum, rgb) => sum + (rgb >> 16 & 255));
        final green = colors.fold<int>(0, (sum, rgb) => sum + (rgb >> 8 & 255));
        final blue = colors.fold<int>(0, (sum, rgb) => sum + (rgb & 255));
        final average = (red ~/ 4 << 16) | (green ~/ 4 << 8) | blue ~/ 4;
        final squareRect = Rect.fromLTWH(
          left + side * .06,
          top + side * .06,
          side * .88,
          side * .88,
        );
        target.drawRRect(
          RRect.fromRectAndRadius(squareRect, Radius.circular(side * .08)),
          paint..color = color(average, dark),
        );
        continue;
      }
      target.save();
      target.clipPath(
        BeveledRectangleBorder(
          borderRadius: BorderRadius.circular(LitchShapes.squareChamfer),
        ).getOuterPath(rect),
      );
      if (colors.every((color) => color == colors.first)) {
        target.drawRect(rect, paint..color = color(colors.first, dark));
      } else if (canvas.topology == LitchcraftTopology.v3) {
        final points = <Offset>[
          rect.topLeft,
          rect.topRight,
          rect.bottomRight,
          rect.bottomLeft,
        ];
        for (var region = 0; region < 4; region++) {
          final semantic = litchcraftLogicalRegionForDisplay(
            region,
            flipped: flipped,
          );
          final first = (region + 3) % 4;
          final path = Path()
            ..moveTo(rect.center.dx, rect.center.dy)
            ..lineTo(points[first].dx, points[first].dy)
            ..lineTo(points[region].dx, points[region].dy)
            ..close();
          target.drawPath(path, paint..color = color(colors[semantic], dark));
        }
      } else {
        final mapped = <int>[
          colors[0],
          litchcraftLegacyCenterColor(colors[1], colors[3]),
          colors[2],
        ];
        for (var emitter = 0; emitter < 3; emitter++) {
          final index = flipped ? 2 - emitter : emitter;
          target.drawRect(
            Rect.fromLTWH(
              rect.left + rect.width * emitter / 3,
              rect.top,
              rect.width / 3 + .2,
              rect.height,
            ),
            paint..color = color(mapped[index], dark),
          );
        }
      }
      target.restore();
    }
  }

  @override
  bool shouldRepaint(_CanvasPainter oldDelegate) =>
      !identical(oldDelegate.canvas, canvas) ||
      oldDelegate.flipped != flipped ||
      oldDelegate.squarePreview != squarePreview ||
      oldDelegate.blackBackground != blackBackground ||
      oldDelegate.displayColors != displayColors;
}

/// Renders the semantic artwork at a useful sharing size without capturing
/// surrounding editor chrome or depending on the on-screen canvas dimensions.
Future<Uint8List> renderLitchcraftArtworkPng(
  LitchcraftCanvas canvas, {
  bool flipped = false,
  int size = 1024,
}) async {
  if (size < 8) throw RangeError.range(size, 8, null, 'size');
  final recorder = ui.PictureRecorder();
  _CanvasPainter(
    canvas,
    flipped,
    blackBackground: true,
    displayColors: false,
  ).paint(ui.Canvas(recorder), Size.square(size.toDouble()));
  final picture = recorder.endRecording();
  final image = await picture.toImage(size, size);
  try {
    final data = await image.toByteData(format: ui.ImageByteFormat.png);
    if (data == null) throw StateError('Could not encode Litchcraft artwork.');
    return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
  } finally {
    image.dispose();
    picture.dispose();
  }
}

bool litchcraftDisplaySquareIsDark(int display) {
  if (display < 0 || display >= 64) throw RangeError.range(display, 0, 63);
  return (display % 8 + display ~/ 8).isOdd;
}

/// The editor's artwork painter, without compression. Animation encoders
/// consume one straight RGBA raster at a time.
Future<Uint8List> renderLitchcraftArtworkRgba(
  LitchcraftCanvas canvas,
  int size,
  bool flipped,
) async {
  if (size < 8 || size > 1024) throw RangeError.range(size, 8, 1024, 'size');
  final recorder = ui.PictureRecorder();
  _CanvasPainter(
    canvas,
    flipped,
    blackBackground: true,
    displayColors: false,
  ).paint(ui.Canvas(recorder), Size.square(size.toDouble()));
  final picture = recorder.endRecording();
  final image = await picture.toImage(size, size);
  try {
    final data = await image.toByteData(
      format: ui.ImageByteFormat.rawStraightRgba,
    );
    if (data == null) throw StateError('Could not render artwork.');
    return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
  } finally {
    image.dispose();
    picture.dispose();
  }
}
