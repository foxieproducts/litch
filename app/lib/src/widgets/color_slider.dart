import 'package:flutter/material.dart';

/// Shared Litchcraft color track, with native slider focus and semantics.
class ColorSlider extends StatelessWidget {
  const ColorSlider({
    super.key,
    required this.value,
    required this.label,
    required this.onChanged,
    this.max = 360,
    this.divisions,
    this.gradient = hueGradient,
    this.sliderKey,
    this.trackKey,
    this.onChangeStart,
    this.onChangeEnd,
  });

  static const hueGradient = LinearGradient(
    colors: [
      Color(0xffff0000),
      Color(0xffffff00),
      Color(0xff00ff00),
      Color(0xff00ffff),
      Color(0xff0000ff),
      Color(0xffff00ff),
      Color(0xffff0000),
    ],
  );

  final double value;
  final double max;
  final int? divisions;
  final String label;
  final Gradient gradient;
  final Key? sliderKey;
  final Key? trackKey;
  final ValueChanged<double>? onChanged;
  final ValueChanged<double>? onChangeStart;
  final ValueChanged<double>? onChangeEnd;

  @override
  Widget build(BuildContext context) => SizedBox(
    height: 48,
    child: Stack(
      alignment: Alignment.center,
      children: [
        Positioned(
          left: 12,
          right: 12,
          child: Container(
            key: trackKey,
            height: 10,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(5),
              gradient: gradient,
            ),
          ),
        ),
        Positioned.fill(
          child: SliderTheme(
            data: SliderTheme.of(context).copyWith(
              trackHeight: 10,
              activeTrackColor: Colors.transparent,
              inactiveTrackColor: Colors.transparent,
              disabledActiveTrackColor: Colors.transparent,
              disabledInactiveTrackColor: Colors.transparent,
              thumbColor: Colors.white,
              disabledThumbColor: Colors.white54,
              overlayColor: Colors.white12,
              thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 7),
              overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
              tickMarkShape: SliderTickMarkShape.noTickMark,
            ),
            child: Slider(
              key: sliderKey,
              max: max,
              value: value,
              divisions: divisions,
              semanticFormatterCallback: (value) => max == 360
                  ? '$label ${value.round()}°'
                  : '$label ${(value / max * 100).round()}%',
              onChanged: onChanged,
              onChangeStart: onChangeStart,
              onChangeEnd: onChangeEnd,
            ),
          ),
        ),
      ],
    ),
  );
}
