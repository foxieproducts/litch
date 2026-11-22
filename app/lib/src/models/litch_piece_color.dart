import 'dart:math' as math;

import 'package:flutter/material.dart';

class LitchPieceColorChoice {
  const LitchPieceColorChoice({required this.wheel, required this.name});

  final int wheel;
  final String name;
}

const List<LitchPieceColorChoice> litchPieceColorChoices =
    <LitchPieceColorChoice>[
      LitchPieceColorChoice(wheel: 0, name: 'Red'),
      LitchPieceColorChoice(wheel: 43, name: 'Gold'),
      LitchPieceColorChoice(wheel: 85, name: 'Green'),
      LitchPieceColorChoice(wheel: 128, name: 'Teal'),
      LitchPieceColorChoice(wheel: 170, name: 'Blue'),
      LitchPieceColorChoice(wheel: 213, name: 'Violet'),
    ];

String litchPieceColorName(int wheel) {
  return litchClosestPieceColorChoice(wheel).name;
}

LitchPieceColorChoice litchClosestPieceColorChoice(int wheel) {
  final int normalized = _normalizeWheel(wheel);
  LitchPieceColorChoice closest = litchPieceColorChoices.first;
  int closestDistance = _circularDistance(normalized, closest.wheel);
  for (final LitchPieceColorChoice choice in litchPieceColorChoices.skip(1)) {
    final int distance = _circularDistance(normalized, choice.wheel);
    if (distance < closestDistance) {
      closest = choice;
      closestDistance = distance;
    }
  }
  return closest;
}

Color litchWheelToColor(int wheel) {
  final int clamped = wheel < 0 ? 0 : (wheel > 255 ? 255 : wheel);
  final int position = 255 - clamped;
  final int red;
  final int green;
  final int blue;
  if (position < 85) {
    red = 255 - (position * 3);
    green = 0;
    blue = position * 3;
  } else if (position < 170) {
    final int offset = position - 85;
    red = 0;
    green = offset * 3;
    blue = 255 - (offset * 3);
  } else {
    final int offset = position - 170;
    red = offset * 3;
    green = 255 - (offset * 3);
    blue = 0;
  }
  return Color.fromARGB(255, red, green, blue);
}

/// Display appearance of a board hue, separate from the wheel value we send.
Color litchWheelPreviewColor(int wheel) =>
    _ledPreviewColor(litchWheelToColor(wheel).toARGB32());

/// Display appearance of the RGB24 drive values in a Litchcraft document.
Color litchcraftPreviewColor(int rgb) => _ledPreviewColor(rgb);

/// Choose the higher-contrast check or history mark for a swatch.
Color litchSwatchMarkColor(Color swatch) =>
    swatch.computeLuminance() > 0.179 ? Colors.black : Colors.white;

// Canvas and filmstrip painters can sample hundreds of channels per frame.
final _displayChannels = List<double>.generate(256, (channel) {
  if (channel == 255) return 1;
  final linear = channel / 255;
  return linear <= 0.0031308
      ? 12.92 * linear
      : 1.055 * math.pow(linear, 1 / 2.4) - 0.055;
}, growable: false);

Color _ledPreviewColor(int rgb) => Color.from(
  alpha: 1,
  red: _displayChannels[(rgb >> 16) & 255],
  green: _displayChannels[(rgb >> 8) & 255],
  blue: _displayChannels[rgb & 255],
);

int _normalizeWheel(int value) {
  int normalized = value % 256;
  if (normalized < 0) {
    normalized += 256;
  }
  return normalized;
}

int _circularDistance(int left, int right) {
  final int direct = (left - right).abs();
  return direct < 256 - direct ? direct : 256 - direct;
}
