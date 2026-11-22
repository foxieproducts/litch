import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../l10n/generated/app_localizations.dart';
import '../localization/litch_strings.dart';
import '../models/litch_piece_color.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_renderer.dart';

String litchcraftDurationSeconds(int durationMs) {
  final wholeSeconds = durationMs ~/ 1000;
  final remainder = durationMs % 1000;
  if (remainder == 0) return '$wholeSeconds.0';
  final fraction = remainder
      .toString()
      .padLeft(3, '0')
      .replaceFirst(RegExp(r'0+$'), '');
  return '$wholeSeconds.$fraction';
}

Future<int?> _editExactDuration(
  BuildContext context, {
  required int durationMs,
  required int minimumMs,
  required int maximumMs,
  required int stepMs,
  required String title,
  required String invalidMessage,
}) async {
  return showDialog<int>(
    context: context,
    builder: (_) => _ExactDurationDialog(
      durationMs: durationMs,
      minimumMs: minimumMs,
      maximumMs: maximumMs,
      stepMs: stepMs,
      title: title,
      invalidMessage: invalidMessage,
    ),
  );
}

class _ExactDurationDialog extends StatefulWidget {
  const _ExactDurationDialog({
    required this.durationMs,
    required this.minimumMs,
    required this.maximumMs,
    required this.stepMs,
    required this.title,
    required this.invalidMessage,
  });

  final int durationMs;
  final int minimumMs;
  final int maximumMs;
  final int stepMs;
  final String title;
  final String invalidMessage;

  @override
  State<_ExactDurationDialog> createState() => _ExactDurationDialogState();
}

class _ExactDurationDialogState extends State<_ExactDurationDialog> {
  late final TextEditingController _controller = TextEditingController(
    text: '${widget.durationMs}',
  );
  final GlobalKey<FormState> _formKey = GlobalKey<FormState>();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _submit() {
    if (!_formKey.currentState!.validate()) return;
    Navigator.of(context).pop(int.parse(_controller.text));
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return AlertDialog(
      title: Text(widget.title),
      scrollable: true,
      content: Form(
        key: _formKey,
        child: TextFormField(
          key: const ValueKey('craft-exact-duration-field'),
          controller: _controller,
          autofocus: true,
          keyboardType: TextInputType.number,
          inputFormatters: <TextInputFormatter>[
            FilteringTextInputFormatter.digitsOnly,
            LengthLimitingTextInputFormatter(5),
          ],
          decoration: InputDecoration(
            labelText: strings.craftMilliseconds,
            helperText: '${widget.minimumMs}–${widget.maximumMs} ms',
            errorMaxLines: 4,
          ),
          validator: (raw) {
            final parsed = int.tryParse(raw ?? '');
            if (parsed == null ||
                parsed < widget.minimumMs ||
                parsed > widget.maximumMs ||
                (parsed - widget.minimumMs) % widget.stepMs != 0) {
              return widget.invalidMessage;
            }
            return null;
          },
          onFieldSubmitted: (_) => _submit(),
        ),
      ),
      actions: <Widget>[
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(strings.cancelAction),
        ),
        FilledButton(
          key: const ValueKey('craft-set-exact-duration'),
          onPressed: _submit,
          child: Text(strings.craftSetDuration),
        ),
      ],
    );
  }
}

class _LitchcraftTimingControl extends StatefulWidget {
  const _LitchcraftTimingControl({
    required this.sliderKey,
    required this.valueKey,
    required this.durationMs,
    required this.minimumMs,
    required this.maximumMs,
    required this.divisions,
    required this.enabled,
    required this.extendTrack,
    required this.semanticLabel,
    required this.onDurationChanged,
    required this.onEditExact,
    this.onDurationChangeStarted,
    this.onDurationPreview,
  });

  final Key sliderKey;
  final Key valueKey;
  final int durationMs;
  final int minimumMs;
  final int maximumMs;
  final int divisions;
  final bool enabled;
  final bool extendTrack;
  final String Function(int durationMs) semanticLabel;
  final ValueChanged<int> onDurationChanged;
  final VoidCallback onEditExact;
  final VoidCallback? onDurationChangeStarted;
  final ValueChanged<int>? onDurationPreview;

  @override
  State<_LitchcraftTimingControl> createState() =>
      _LitchcraftTimingControlState();
}

class _LitchcraftTimingControlState extends State<_LitchcraftTimingControl> {
  int? _dragDurationMs;
  ValueChanged<int>? _dragCommit;

  @override
  void didUpdateWidget(_LitchcraftTimingControl oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!widget.enabled) {
      _dragDurationMs = null;
      _dragCommit = null;
    }
  }

  int _durationForSliderValue(double value) =>
      widget.minimumMs + widget.maximumMs - value.round();

  void _startDrag(double _) {
    widget.onDurationChangeStarted?.call();
    _dragCommit = widget.onDurationChanged;
    setState(() => _dragDurationMs = widget.durationMs);
  }

  void _previewDuration(double value) {
    if (_dragCommit == null) return;
    final next = _durationForSliderValue(value);
    if (next == _dragDurationMs) return;
    setState(() => _dragDurationMs = next);
    widget.onDurationPreview?.call(next);
  }

  void _endDrag(double value) {
    final commit = _dragCommit;
    if (commit == null) return;
    final durationMs = _dragDurationMs ?? _durationForSliderValue(value);
    setState(() {
      _dragDurationMs = null;
      _dragCommit = null;
    });
    commit(durationMs);
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final durationMs = _dragDurationMs ?? widget.durationMs;
    final value = '${litchcraftDurationSeconds(durationMs)} s';
    final semanticLabel = widget.semanticLabel(durationMs);
    final sliderValue = (widget.minimumMs + widget.maximumMs - durationMs)
        .toDouble();
    final spanMs = widget.maximumMs - widget.minimumMs;
    final discreteSliderValue =
        widget.minimumMs +
        ((sliderValue - widget.minimumMs) / spanMs * widget.divisions).round() *
            spanMs /
            widget.divisions;
    // Exact millisecond labels have different lengths. Reserve their width so
    // the track never resizes underneath a held pointer as the value changes.
    final valuePainter = TextPainter(
      text: TextSpan(
        text: '0.000 s',
        style: Theme.of(context).textTheme.labelLarge?.copyWith(fontSize: 12),
      ),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
    )..layout();
    final valueWidth = (valuePainter.width.ceilToDouble() + 8).clamp(
      56.0,
      double.infinity,
    );
    valuePainter.dispose();
    return Stack(
      key: const ValueKey('craft-timing-control'),
      children: <Widget>[
        Positioned(
          top: 0,
          left: 0,
          right: 0,
          child: SizedBox(
            height: 48,
            child: Row(
              children: <Widget>[
                Expanded(
                  child: Slider(
                    key: widget.sliderKey,
                    padding: widget.extendTrack
                        ? const EdgeInsets.symmetric(horizontal: 8)
                        : null,
                    min: widget.minimumMs.toDouble(),
                    max: widget.maximumMs.toDouble(),
                    divisions: widget.divisions,
                    value: sliderValue,
                    label: value,
                    // Flutter rounds discrete slider semantics, but retained
                    // exact timing must still be announced at its current stop.
                    semanticFormatterCallback: (next) =>
                        (next - discreteSliderValue).abs() < .001
                        ? semanticLabel
                        : widget.semanticLabel(_durationForSliderValue(next)),
                    onChangeStart: widget.enabled ? _startDrag : null,
                    onChanged: widget.enabled ? _previewDuration : null,
                    onChangeEnd: widget.enabled ? _endDrag : null,
                  ),
                ),
                SizedBox(
                  width: valueWidth,
                  child: Tooltip(
                    message: semanticLabel,
                    waitDuration: const Duration(milliseconds: 550),
                    excludeFromSemantics: true,
                    child: TextButton(
                      key: widget.valueKey,
                      onPressed: widget.enabled && _dragCommit == null
                          ? widget.onEditExact
                          : null,
                      style: TextButton.styleFrom(
                        minimumSize: const Size(56, 48),
                        foregroundColor: Colors.white,
                        disabledForegroundColor: Colors.white38,
                        padding: const EdgeInsets.symmetric(horizontal: 4),
                      ),
                      child: Semantics(
                        label: semanticLabel,
                        child: ExcludeSemantics(
                          child: Text(
                            value,
                            style: const TextStyle(fontSize: 12),
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
        // Captions use eight empty pixels below the track. The complete 48 px
        // control targets remain available underneath their noninteractive text.
        IgnorePointer(
          child: Padding(
            padding: const EdgeInsets.only(top: 40),
            child: LayoutBuilder(
              builder: (context, bounds) {
                var leftInset = widget.extendTrack ? 8.0 : 24.0;
                var rightInset = valueWidth + (widget.extendTrack ? 8.0 : 24.0);
                final endpoints = <Widget>[
                  Text(strings.craftSlower, style: _endpointStyle),
                  Text(
                    strings.craftFaster,
                    textAlign: TextAlign.end,
                    style: _endpointStyle,
                  ),
                ];
                double measureLabel(String label) {
                  final painter = TextPainter(
                    text: TextSpan(
                      text: label,
                      style: DefaultTextStyle.of(
                        context,
                      ).style.merge(_endpointStyle),
                    ),
                    textDirection: Directionality.of(context),
                    textScaler: MediaQuery.textScalerOf(context),
                    textWidthBasis: TextWidthBasis.longestLine,
                    locale: Localizations.maybeLocaleOf(context),
                  )..layout();
                  final width = painter.width;
                  painter.dispose();
                  return width;
                }

                final endpointWidth =
                    measureLabel(strings.craftSlower) +
                    measureLabel(strings.craftFaster);
                if (endpointWidth > bounds.maxWidth - leftInset - rightInset) {
                  // The value button occupies the track row, not these captions.
                  // Keep the existing edge gap for both the full row and stack.
                  leftInset = 8;
                  rightInset = 8;
                }
                final stackEndpoints =
                    endpointWidth > bounds.maxWidth - leftInset - rightInset;
                return Padding(
                  padding: EdgeInsets.only(left: leftInset, right: rightInset),
                  child: stackEndpoints
                      ? Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: endpoints,
                        )
                      : Row(
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: endpoints,
                        ),
                );
              },
            ),
          ),
        ),
      ],
    );
  }

  static const TextStyle _endpointStyle = TextStyle(
    color: Colors.white70,
    fontSize: 9,
    height: 1,
  );
}

class LitchcraftFrameEditorPanel extends StatelessWidget {
  const LitchcraftFrameEditorPanel({
    super.key,
    required this.document,
    required this.selectedFrameIndex,
    required this.enabled,
    required this.onDurationChanged,
    this.onDurationChangeStarted,
    this.onDurationPreview,
    required this.onDuplicate,
    required this.onDelete,
    required this.onOpenEffects,
    this.extendTrack = false,
    this.actions = const [],
  });

  final LitchcraftAnimationDocument document;
  final int selectedFrameIndex;
  final bool enabled;
  final ValueChanged<int> onDurationChanged;
  final VoidCallback? onDurationChangeStarted;
  final ValueChanged<int>? onDurationPreview;
  final VoidCallback onDuplicate;
  final VoidCallback onDelete;
  final VoidCallback onOpenEffects;
  final bool extendTrack;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, bounds) {
      final strings = litchStrings(context);
      final frame = document.frames[selectedFrameIndex];
      Future<void> editDuration() async {
        onDurationChangeStarted?.call();
        final value = await _editExactDuration(
          context,
          durationMs: frame.durationMs,
          minimumMs: litchcraftAnimationMinimumFrameDurationMs,
          maximumMs: litchcraftAnimationMaximumFrameDurationMs,
          stepMs: 1,
          title: strings.craftExactFrameTiming,
          invalidMessage: strings.craftFrameTimingRange,
        );
        if (value != null) onDurationChanged(value);
      }

      final timing = _LitchcraftTimingControl(
        sliderKey: const ValueKey('craft-frame-duration'),
        valueKey: const ValueKey('craft-frame-duration-context'),
        durationMs: frame.durationMs,
        minimumMs: litchcraftAnimationMinimumFrameDurationMs,
        maximumMs: litchcraftAnimationMaximumFrameDurationMs,
        divisions: 99,
        enabled: enabled,
        extendTrack: extendTrack,
        semanticLabel: (durationMs) =>
            strings.craftFrameDuration(litchcraftDurationSeconds(durationMs)),
        onDurationChangeStarted: onDurationChangeStarted,
        onDurationPreview: onDurationPreview,
        onDurationChanged: onDurationChanged,
        onEditExact: editDuration,
      );
      final compact = bounds.maxWidth < 180 && !extendTrack;
      if (actions.isNotEmpty) {
        return Column(
          key: const ValueKey('craft-frame-panel'),
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: actions,
            ),
            timing,
          ],
        );
      }
      return Flex(
        key: const ValueKey('craft-frame-panel'),
        direction: compact ? Axis.vertical : Axis.horizontal,
        mainAxisSize: MainAxisSize.min,
        children: [if (compact) timing else Expanded(child: timing)],
      );
    },
  );
}

class LitchcraftEffectEditorPanel extends StatelessWidget {
  const LitchcraftEffectEditorPanel({
    super.key,
    required this.selectedEffect,
    required this.effectDurationMs,
    required this.enabled,
    required this.onEffectChanged,
    required this.onDurationChanged,
    this.onDurationChangeStarted,
    this.onDurationPreview,
  });

  final LitchcraftEffectStyle? selectedEffect;
  final int effectDurationMs;
  final bool enabled;
  final ValueChanged<LitchcraftEffectStyle> onEffectChanged;
  final ValueChanged<int> onDurationChanged;
  final VoidCallback? onDurationChangeStarted;
  final ValueChanged<int>? onDurationPreview;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return LayoutBuilder(
      builder: (context, bounds) {
        final expandedLabels =
            MediaQuery.textScalerOf(context).scale(10) >= 18 ||
            bounds.maxHeight < 154;
        final columns = expandedLabels
            ? 1
            : (bounds.maxWidth / 48).floor().clamp(1, 5);
        Future<void> editDuration() async {
          onDurationChangeStarted?.call();
          final value = await _editExactDuration(
            context,
            durationMs: effectDurationMs,
            minimumMs: litchcraftAnimationMinimumEffectDurationMs,
            maximumMs: litchcraftAnimationMaximumEffectDurationMs,
            stepMs: 100,
            title: strings.craftExactEffectTiming,
            invalidMessage: strings.craftEffectTimingRange,
          );
          if (value != null) onDurationChanged(value);
        }

        Widget effectChoice(LitchcraftEffectStyle effect) {
          final selected = effect == selectedEffect;
          final label = _effectLabel(strings, effect);
          final hint = _effectHint(strings, effect);
          final preview = Stack(
            alignment: Alignment.center,
            children: <Widget>[
              _LitchcraftEffectPreview(effect: effect),
              if (selected)
                const Positioned(
                  key: ValueKey('craft-active-effect'),
                  right: 0,
                  bottom: 0,
                  child: Icon(Icons.check, size: 14, color: Color(0xff263c40)),
                ),
            ],
          );
          final style = IconButton.styleFrom(
            minimumSize: const Size(48, 48),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(7),
            ),
            backgroundColor: selected ? const Color(0xffc6e5e4) : null,
          );
          return SizedBox(
            width: bounds.maxWidth / columns,
            height: 48,
            child: Semantics(
              selected: selected,
              button: true,
              enabled: enabled,
              label: label,
              hint: hint,
              child: Tooltip(
                message: '$label\n$hint',
                excludeFromSemantics: true,
                child: IconButton(
                  key: ValueKey('craft-effect-${effect.name}'),
                  onPressed: enabled ? () => onEffectChanged(effect) : null,
                  isSelected: selected,
                  style: style,
                  icon: preview,
                ),
              ),
            ),
          );
        }

        Widget expandedEffectSelector() {
          final menuController = MenuController();
          final effects = [...LitchcraftEffectStyle.values]
            ..sort(
              (a, b) => compareLitchLabels(
                _effectLabel(strings, a),
                _effectLabel(strings, b),
              ),
            );
          return MenuAnchor(
            key: const ValueKey('craft-effect-menu'),
            controller: menuController,
            style: MenuStyle(
              minimumSize: WidgetStatePropertyAll(Size(bounds.maxWidth, 48)),
            ),
            menuChildren: <Widget>[
              for (final effect in effects)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 4),
                  child: SizedBox(
                    width: bounds.maxWidth,
                    child: MenuItemButton(
                      key: ValueKey('craft-effect-${effect.name}'),
                      style: MenuItemButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 4),
                      ),
                      onPressed: enabled
                          ? () {
                              onEffectChanged(effect);
                              menuController.close();
                            }
                          : null,
                      leadingIcon: _LitchcraftEffectPreview(effect: effect),
                      trailingIcon: effect == selectedEffect
                          ? const Icon(Icons.check, size: 20)
                          : null,
                      child: SizedBox(
                        width: math.max(48, bounds.maxWidth - 144),
                        child: Text(_effectLabel(strings, effect)),
                      ),
                    ),
                  ),
                ),
            ],
            builder: (context, menu, _) {
              final effect = selectedEffect;
              final label = effect == null
                  ? strings.craftEffectsTab
                  : _effectLabel(strings, effect);
              final hint = effect == null ? null : _effectHint(strings, effect);
              return Semantics(
                button: true,
                enabled: enabled,
                label: label,
                hint: hint,
                child: Tooltip(
                  message: hint == null ? label : '$label\n$hint',
                  excludeFromSemantics: true,
                  child: TextButton(
                    key: const ValueKey('craft-effect-selector'),
                    onPressed: enabled
                        ? () => menu.isOpen ? menu.close() : menu.open()
                        : null,
                    style: TextButton.styleFrom(
                      minimumSize: const Size.fromHeight(48),
                      foregroundColor: Colors.white,
                      backgroundColor: Colors.white.withValues(alpha: .08),
                      alignment: Alignment.centerLeft,
                      padding: const EdgeInsets.symmetric(horizontal: 12),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(7),
                      ),
                    ),
                    child: Row(
                      children: <Widget>[
                        if (effect == null)
                          const Icon(Icons.auto_awesome_outlined)
                        else
                          _LitchcraftEffectPreview(effect: effect),
                        const SizedBox(width: 12),
                        Expanded(child: Text(label)),
                        const Icon(Icons.arrow_drop_down),
                      ],
                    ),
                  ),
                ),
              );
            },
          );
        }

        return Column(
          key: const ValueKey('craft-effect-panel'),
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            if (expandedLabels)
              expandedEffectSelector()
            else
              Wrap(
                alignment: WrapAlignment.center,
                children: [
                  for (final effect in LitchcraftEffectStyle.values)
                    effectChoice(effect),
                ],
              ),
            _LitchcraftTimingControl(
              sliderKey: const ValueKey('craft-effect-duration'),
              valueKey: const ValueKey('craft-effect-duration-context'),
              durationMs: effectDurationMs,
              minimumMs: litchcraftAnimationMinimumEffectDurationMs,
              maximumMs: litchcraftAnimationMaximumEffectDurationMs,
              divisions: 95,
              enabled:
                  enabled &&
                  selectedEffect != null &&
                  selectedEffect != LitchcraftEffectStyle.off,
              extendTrack: false,
              semanticLabel: (durationMs) => strings.craftEffectDuration(
                litchcraftDurationSeconds(durationMs),
              ),
              onDurationChangeStarted: onDurationChangeStarted,
              onDurationPreview: onDurationPreview,
              onDurationChanged: onDurationChanged,
              onEditExact: editDuration,
            ),
          ],
        );
      },
    );
  }
}

class _LitchcraftEffectPreview extends StatefulWidget {
  const _LitchcraftEffectPreview({required this.effect});

  final LitchcraftEffectStyle effect;

  @override
  State<_LitchcraftEffectPreview> createState() =>
      _LitchcraftEffectPreviewState();
}

class _LitchcraftEffectPreviewState extends State<_LitchcraftEffectPreview>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 2000),
  );

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (MediaQuery.disableAnimationsOf(context)) {
      _controller.stop();
      _controller.value = .25;
    } else if (!_controller.isAnimating) {
      _controller.repeat();
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (widget.effect == LitchcraftEffectStyle.off) {
      return const SizedBox.square(
        dimension: 32,
        child: Icon(Icons.motion_photos_off_outlined),
      );
    }
    final emitters = List<LitchcraftAnimatedEmitter>.generate(
      4,
      (_) => LitchcraftAnimatedEmitter(
        color: 0x22c3e2,
        effect: widget.effect,
        effectDurationMs: 2000,
      ),
      growable: false,
    );
    return AnimatedBuilder(
      animation: _controller,
      builder: (context, _) {
        final colors = composeLitchcraftAnimatedSquare(
          emitters: emitters,
          elapsedMs: (_controller.value * 2000).round(),
          sparkleSeed: 1,
          frameIndex: 0,
          emitterBaseIndex: 0,
        );
        return CustomPaint(
          size: const Size.square(32),
          painter: _EffectPreviewPainter(colors),
        );
      },
    );
  }
}

class _EffectPreviewPainter extends CustomPainter {
  const _EffectPreviewPainter(this.colors);

  final List<int> colors;

  @override
  void paint(Canvas canvas, Size size) {
    final positions = <Offset>[
      Offset(6, size.height / 2),
      Offset(size.width / 2, 6),
      Offset(size.width - 6, size.height / 2),
      Offset(size.width / 2, size.height - 6),
    ];
    final paint = Paint();
    for (var index = 0; index < 4; index++) {
      final color = colors[index];
      paint
        ..style = color == 0 ? PaintingStyle.stroke : PaintingStyle.fill
        ..strokeWidth = 1.5
        ..color = color == 0
            ? const Color(0xff7a8588)
            : litchcraftPreviewColor(color);
      canvas.drawCircle(positions[index], 4.5, paint);
    }
  }

  @override
  bool shouldRepaint(_EffectPreviewPainter oldDelegate) =>
      oldDelegate.colors != colors;
}

String _effectLabel(AppLocalizations strings, LitchcraftEffectStyle effect) =>
    switch (effect) {
      LitchcraftEffectStyle.off => strings.craftEffectOff,
      LitchcraftEffectStyle.breathe => strings.craftEffectBreathe,
      LitchcraftEffectStyle.flash => strings.craftEffectFlash,
      LitchcraftEffectStyle.fadeToBlack => strings.craftEffectFade,
      LitchcraftEffectStyle.rainbow => strings.craftEffectRainbow,
      LitchcraftEffectStyle.orbitClockwise => strings.craftEffectOrbitClockwise,
      LitchcraftEffectStyle.orbitCounterclockwise =>
        strings.craftEffectOrbitCounterclockwise,
      LitchcraftEffectStyle.bounce => strings.craftEffectBounce,
      LitchcraftEffectStyle.sparkle => strings.craftEffectSparkle,
    };

String _effectHint(AppLocalizations strings, LitchcraftEffectStyle effect) =>
    switch (effect) {
      LitchcraftEffectStyle.off => strings.craftEffectOffHint,
      LitchcraftEffectStyle.breathe => strings.craftEffectBreatheHint,
      LitchcraftEffectStyle.flash => strings.craftEffectFlashHint,
      LitchcraftEffectStyle.fadeToBlack => strings.craftEffectFadeHint,
      LitchcraftEffectStyle.rainbow => strings.craftEffectRainbowHint,
      LitchcraftEffectStyle.orbitClockwise =>
        strings.craftEffectOrbitClockwiseHint,
      LitchcraftEffectStyle.orbitCounterclockwise =>
        strings.craftEffectOrbitCounterclockwiseHint,
      LitchcraftEffectStyle.bounce => strings.craftEffectBounceHint,
      LitchcraftEffectStyle.sparkle => strings.craftEffectSparkleHint,
    };
