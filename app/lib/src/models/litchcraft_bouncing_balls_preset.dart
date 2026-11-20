import 'dart:convert';
import 'dart:io';

import 'litchcraft_animation.dart';
import 'litchcraft_bouncing_balls_data.dart';

/// An ordinary, exact 256-frame animation used for full-scale benchmarks.
class LitchcraftBouncingBallsPreset {
  LitchcraftBouncingBallsPreset._(this.frames);

  factory LitchcraftBouncingBallsPreset.decode() {
    final rgb = zlib.decode(base64Decode(litchcraftBouncingBallsPackedRgb));
    if (rgb.length != frameCount * 256 * 3) {
      throw const FormatException('Invalid bouncing-balls benchmark preset');
    }
    final frames = List<LitchcraftAnimationFrame>.generate(frameCount, (frame) {
      return LitchcraftAnimationFrame(
        durationMs: frameDurationMs,
        emitters: List<LitchcraftAnimatedEmitter>.generate(256, (emitter) {
          final offset = (frame * 256 + emitter) * 3;
          return LitchcraftAnimatedEmitter(
            color:
                (rgb[offset] << 16) | (rgb[offset + 1] << 8) | rgb[offset + 2],
          );
        }, growable: false),
      );
    }, growable: false);
    return LitchcraftBouncingBallsPreset._(List.unmodifiable(frames));
  }

  static const int frameCount = 256;
  static const int frameDurationMs = 50;

  final List<LitchcraftAnimationFrame> frames;
}
