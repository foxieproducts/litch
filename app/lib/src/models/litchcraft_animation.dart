import 'package:flutter/foundation.dart';

import 'litchcraft_state.dart';

const int litchcraftAnimationSchema = 3;
const int litchcraftAnimationEmitterCount = 256;
const int litchcraftAnimationMinimumFrameDurationMs = 50;
const int litchcraftAnimationMaximumFrameDurationMs = 5000;
const int litchcraftAnimationDefaultFrameDurationMs = 1000;
const int litchcraftAnimationMinimumEffectDurationMs = 500;
const int litchcraftAnimationMaximumEffectDurationMs = 10000;
const int litchcraftAnimationDefaultEffectDurationMs = 2000;
const int litchcraftAnimationLegacyMaximumFrameCount = 8;
const int litchcraftAnimationMaximumFrameCount = 256;
const int litchcraftAnimationMaximumEncodedBytes = 12336;

enum LitchcraftEffectStyle {
  off(0),
  breathe(1),
  flash(2),
  fadeToBlack(3),
  rainbow(4),
  orbitClockwise(5),
  orbitCounterclockwise(6),
  bounce(7),
  sparkle(8);

  const LitchcraftEffectStyle(this.id);
  final int id;

  static LitchcraftEffectStyle fromId(int id) => values.firstWhere(
    (value) => value.id == id,
    orElse: () => throw const FormatException('Unknown Litchcraft effect'),
  );
}

@immutable
class LitchcraftAnimatedEmitter {
  const LitchcraftAnimatedEmitter({
    required this.color,
    this.effect = LitchcraftEffectStyle.off,
    this.effectDurationMs = 0,
  });

  final int color;
  final LitchcraftEffectStyle effect;
  final int effectDurationMs;

  bool get isValid =>
      color >= 0 &&
      color <= 0xffffff &&
      (effect == LitchcraftEffectStyle.off
          ? effectDurationMs == 0
          : effectDurationMs >= litchcraftAnimationMinimumEffectDurationMs &&
                effectDurationMs <=
                    litchcraftAnimationMaximumEffectDurationMs &&
                effectDurationMs % 100 == 0);
}

@immutable
class LitchcraftAnimationFrame {
  LitchcraftAnimationFrame({
    required Iterable<LitchcraftAnimatedEmitter> emitters,
    this.durationMs = litchcraftAnimationDefaultFrameDurationMs,
  }) : emitters = List<LitchcraftAnimatedEmitter>.unmodifiable(emitters) {
    if (this.emitters.length != litchcraftAnimationEmitterCount ||
        this.emitters.any((emitter) => !emitter.isValid) ||
        durationMs < litchcraftAnimationMinimumFrameDurationMs ||
        durationMs > litchcraftAnimationMaximumFrameDurationMs) {
      throw const FormatException('Invalid Litchcraft animation frame');
    }
  }

  final List<LitchcraftAnimatedEmitter> emitters;
  final int durationMs;
}

/// Schema-3 semantic artwork, before topology, optical, and power mapping.
@immutable
class LitchcraftAnimationDocument {
  LitchcraftAnimationDocument({
    required Iterable<LitchcraftAnimationFrame> frames,
    required this.topology,
    required Iterable<int> favoriteColors,
    this.playbackOnEntry = false,
    this.sparkleSeed = 1,
  }) : frames = List<LitchcraftAnimationFrame>.unmodifiable(frames),
       favoriteColors = List<int>.unmodifiable(favoriteColors) {
    if (this.frames.isEmpty ||
        this.frames.length > litchcraftAnimationMaximumFrameCount ||
        this.favoriteColors.length != 8 ||
        this.favoriteColors.any((color) => color < 0 || color > 0xffffff) ||
        sparkleSeed < 1 ||
        sparkleSeed > 0xffffffff) {
      throw const FormatException('Invalid Litchcraft animation document');
    }
  }

  final List<LitchcraftAnimationFrame> frames;
  final LitchcraftTopology topology;
  final List<int> favoriteColors;
  final bool playbackOnEntry;
  final int sparkleSeed;

  int get encodedByteLength => 32 + frames.length * 1538;
}
