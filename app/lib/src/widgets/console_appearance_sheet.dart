import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../localization/litch_strings.dart';
import 'color_slider.dart';
import 'compact_controls.dart';
import 'settings_ui.dart';

@immutable
class ConsoleAppearance {
  const ConsoleAppearance({
    this.foreground = const Color(0xffd9f3f2),
    this.background = Colors.black,
    this.timestamp = const Color(0xff9cafae),
    this.font = 'monospace',
    this.showTimestamps = false,
    this.resetWelcome = false,
  });

  final Color foreground;
  final Color background;
  final Color timestamp;
  final String font;
  final bool showTimestamps;
  final bool resetWelcome;
}

enum _ColorTarget { foreground, background, timestamp }

/// Appearance edits are a draft; Trace changes apply to the live session.
class ConsoleAppearanceSheet extends StatefulWidget {
  const ConsoleAppearanceSheet({
    super.key,
    required this.initial,
    required this.traceOutputEnabled,
    required this.traceOutputStatus,
    this.onTraceOutputChanged,
  });
  final ConsoleAppearance initial;
  final bool traceOutputEnabled;
  final String traceOutputStatus;
  final ValueChanged<bool>? onTraceOutputChanged;

  @override
  State<ConsoleAppearanceSheet> createState() => _ConsoleAppearanceSheetState();
}

class _ConsoleAppearanceSheetState extends State<ConsoleAppearanceSheet> {
  late ConsoleAppearance _draft = widget.initial;
  _ColorTarget _target = _ColorTarget.foreground;
  late HSVColor _color = HSVColor.fromColor(_selectedColor);
  late final _hex = TextEditingController(text: _formatHex(_selectedColor));
  final List<ConsoleAppearance> _history = [];
  bool _invalid = false;

  Color get _selectedColor => switch (_target) {
    _ColorTarget.foreground => _draft.foreground,
    _ColorTarget.background => _draft.background,
    _ColorTarget.timestamp => _draft.timestamp,
  };

  String _formatHex(Color color) =>
      '#${(color.toARGB32() & 0xffffff).toRadixString(16).padLeft(6, '0').toUpperCase()}';

  void _remember() {
    if (_history.length == 32) _history.removeAt(0);
    _history.add(_draft);
  }

  void _syncColor() {
    _color = HSVColor.fromColor(_selectedColor);
    _hex.text = _formatHex(_selectedColor);
    _invalid = false;
  }

  void _changeColor(HSVColor color, {bool updateHex = true}) {
    setState(() {
      // Preserve hue even at zero saturation/brightness during a gesture.
      _color = color;
      _draft = ConsoleAppearance(
        foreground: _target == _ColorTarget.foreground
            ? color.toColor()
            : _draft.foreground,
        background: _target == _ColorTarget.background
            ? color.toColor()
            : _draft.background,
        timestamp: _target == _ColorTarget.timestamp
            ? color.toColor()
            : _draft.timestamp,
        font: _draft.font,
        showTimestamps: _draft.showTimestamps,
        resetWelcome: _draft.resetWelcome,
      );
      if (updateHex) _hex.text = _formatHex(_selectedColor);
      _invalid = false;
    });
  }

  @override
  void dispose() {
    _hex.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final buttonStyle = TextButton.styleFrom(minimumSize: const Size(48, 48));
    final fonts = <String, String>{
      'monospace': strings.consoleFontFixedWidth,
      'sans-serif': strings.consoleFontSystem,
      'serif': strings.consoleFontSerif,
    }.entries.toList()..sort((a, b) => compareLitchLabels(a.value, b.value));
    return SafeArea(
      top: false,
      child: Padding(
        padding: EdgeInsets.only(
          bottom: MediaQuery.viewInsetsOf(context).bottom,
        ),
        child: SingleChildScrollView(
          key: const ValueKey('console-appearance-scroll'),
          padding: const EdgeInsets.fromLTRB(20, 16, 20, 20),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Semantics(
                header: true,
                child: Text(
                  strings.consoleAppearance,
                  style: Theme.of(context).textTheme.titleLarge,
                ),
              ),
              SwitchListTile.adaptive(
                key: const ValueKey('console-timestamps-toggle'),
                contentPadding: EdgeInsets.zero,
                title: Text(strings.consoleShowTimestamps),
                value: _draft.showTimestamps,
                onChanged: (visible) => setState(() {
                  _remember();
                  _draft = ConsoleAppearance(
                    foreground: _draft.foreground,
                    background: _draft.background,
                    timestamp: _draft.timestamp,
                    font: _draft.font,
                    showTimestamps: visible,
                    resetWelcome: _draft.resetWelcome,
                  );
                }),
              ),
              Semantics(
                liveRegion: true,
                child: SwitchListTile.adaptive(
                  key: const ValueKey('console-trace-output-toggle'),
                  contentPadding: EdgeInsets.zero,
                  title: Text(strings.consoleTraceOutput),
                  subtitle: Text(widget.traceOutputStatus),
                  value: widget.traceOutputEnabled,
                  onChanged: widget.onTraceOutputChanged,
                ),
              ),
              Text(
                strings.consoleTraceAppliesImmediately,
                key: const ValueKey('console-trace-immediate-hint'),
                style: Theme.of(context).textTheme.bodySmall,
              ),
              const SizedBox(height: 12),
              Container(
                key: const ValueKey('console-color-preview'),
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: _draft.background,
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text.rich(
                  TextSpan(
                    children: [
                      if (_draft.showTimestamps)
                        TextSpan(
                          text: '12:34:56.000 ',
                          style: TextStyle(color: _draft.timestamp),
                        ),
                      TextSpan(text: strings.consoleWaiting),
                    ],
                  ),
                  style: TextStyle(
                    color: _draft.foreground,
                    fontFamily: _draft.font == 'sans-serif'
                        ? null
                        : _draft.font == 'monospace' &&
                              Theme.of(context).platform == TargetPlatform.iOS
                        ? 'Menlo'
                        : _draft.font,
                  ),
                ),
              ),
              const SizedBox(height: 12),
              SettingsChoicePills<_ColorTarget>(
                key: const ValueKey('console-color-targets'),
                segments: [
                  for (final target in _ColorTarget.values)
                    ButtonSegment(
                      value: target,
                      label: Text(switch (target) {
                        _ColorTarget.foreground => strings.consoleForeground,
                        _ColorTarget.background => strings.consoleBackground,
                        _ColorTarget.timestamp => strings.consoleTimestamp,
                      }),
                    ),
                ],
                selected: {_target},
                optionKey: (target) => ValueKey('console-${target.name}-color'),
                onSelectionChanged: (selected) => setState(() {
                  _target = selected.single;
                  _syncColor();
                }),
              ),
              Text(strings.craftHue),
              ColorSlider(
                sliderKey: const ValueKey('console-color-hue'),
                label: strings.craftHue,
                value: _color.hue,
                onChangeStart: (_) => _remember(),
                onChanged: (value) => _changeColor(_color.withHue(value)),
              ),
              Text(strings.craftSaturation),
              ColorSlider(
                sliderKey: const ValueKey('console-color-saturation'),
                label: strings.craftSaturation,
                value: _color.saturation,
                max: 1,
                gradient: LinearGradient(
                  colors: [
                    _color.withSaturation(0).toColor(),
                    _color.withSaturation(1).toColor(),
                  ],
                ),
                onChangeStart: (_) => _remember(),
                onChanged: (value) =>
                    _changeColor(_color.withSaturation(value)),
              ),
              Text(strings.brightness),
              ColorSlider(
                sliderKey: const ValueKey('console-color-brightness'),
                label: strings.brightness,
                value: _color.value,
                max: 1,
                gradient: LinearGradient(
                  colors: [Colors.black, _color.withValue(1).toColor()],
                ),
                onChangeStart: (_) => _remember(),
                onChanged: (value) => _changeColor(_color.withValue(value)),
              ),
              TextField(
                key: const ValueKey('console-color-hex'),
                controller: _hex,
                autocorrect: false,
                enableSuggestions: false,
                textCapitalization: TextCapitalization.characters,
                inputFormatters: [
                  FilteringTextInputFormatter.allow(RegExp('[#0-9a-fA-F]')),
                  LengthLimitingTextInputFormatter(7),
                ],
                decoration: InputDecoration(
                  labelText: strings.craftHexColor,
                  errorText: _invalid ? strings.consoleColorFormatError : null,
                  border: const OutlineInputBorder(),
                ),
                onChanged: (text) {
                  final match = RegExp(
                    r'^#?([0-9a-fA-F]{6})$',
                  ).firstMatch(text);
                  if (match == null) {
                    setState(() => _invalid = true);
                    return;
                  }
                  _remember();
                  _changeColor(
                    HSVColor.fromColor(
                      Color(0xff000000 | int.parse(match[1]!, radix: 16)),
                    ),
                    updateHex: false,
                  );
                },
              ),
              const SizedBox(height: 12),
              DropdownButtonFormField<String>(
                itemHeight: null,
                key: ValueKey('console-font-${_draft.font}'),
                initialValue: _draft.font,
                isExpanded: true,
                decoration: InputDecoration(
                  labelText: strings.consoleFont,
                  border: const OutlineInputBorder(),
                ),
                items: [
                  for (final font in fonts)
                    DropdownMenuItem(value: font.key, child: Text(font.value)),
                ],
                onChanged: (font) {
                  if (font == null || font == _draft.font) return;
                  setState(() {
                    _remember();
                    _draft = ConsoleAppearance(
                      foreground: _draft.foreground,
                      background: _draft.background,
                      timestamp: _draft.timestamp,
                      font: font,
                      showTimestamps: _draft.showTimestamps,
                      resetWelcome: _draft.resetWelcome,
                    );
                  });
                },
              ),
              const SizedBox(height: 12),
              LitchCompactControls(
                child: Wrap(
                  alignment: WrapAlignment.end,
                  spacing: 8,
                  runSpacing: 4,
                  children: [
                    TextButton(
                      key: const ValueKey('console-appearance-defaults'),
                      style: buttonStyle,
                      onPressed: () => setState(() {
                        _remember();
                        _draft = const ConsoleAppearance(resetWelcome: true);
                        _syncColor();
                      }),
                      child: Text(strings.consoleDefaults),
                    ),
                    TextButton(
                      key: const ValueKey('console-appearance-undo'),
                      style: buttonStyle,
                      onPressed: _history.isEmpty
                          ? null
                          : () => setState(() {
                              _draft = _history.removeLast();
                              _syncColor();
                            }),
                      child: Text(strings.undoAction),
                    ),
                    TextButton(
                      key: const ValueKey('console-appearance-cancel'),
                      style: buttonStyle,
                      onPressed: () => Navigator.of(context).pop(),
                      child: Text(strings.cancelAction),
                    ),
                    FilledButton(
                      key: const ValueKey('console-appearance-apply'),
                      style: FilledButton.styleFrom(
                        minimumSize: const Size(48, 48),
                      ),
                      onPressed: _invalid
                          ? null
                          : () => Navigator.of(context).pop(_draft),
                      child: Text(strings.consoleApply),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
