import 'dart:convert';
import 'dart:io';

import 'litchcraft_animation.dart';
import 'litchcraft_animation_renderer.dart';
import 'litchcraft_rickroll_data.dart';
import 'litchcraft_state.dart';

/// One explicitly owned decode, never a global cache or per-tick GIF decoder.
/// The normal eight-frame editable document and its persistence are untouched.
class LitchcraftRickrollPreset {
  LitchcraftRickrollPreset._(this.frames, this._canvases);

  factory LitchcraftRickrollPreset.decode() {
    final rgb = zlib.decode(base64Decode(litchcraftRickrollPackedRgb));
    if (rgb.length != 32 * 256 * 3) {
      throw const FormatException('Invalid Rickroll preset');
    }
    final frames = List<LitchcraftAnimationFrame>.generate(32, (frame) {
      return LitchcraftAnimationFrame(
        durationMs: 50,
        emitters: List<LitchcraftAnimatedEmitter>.generate(256, (emitter) {
          final offset = (frame * 256 + emitter) * 3;
          final column = (emitter ~/ 4) % 8;
          final side = emitter % 4;
          // The cropped image's black letterbox gets a dim, fixed blue breathe.
          // It never touches the subject or follows image content, so metadata
          // stays spatially stable and the left window cannot pulse brightly.
          final backdrop =
              (column == 0 && side == 0) || (column == 7 && side == 2);
          return LitchcraftAnimatedEmitter(
            color: backdrop
                ? 0x081018
                : (rgb[offset] << 16) |
                      (rgb[offset + 1] << 8) |
                      rgb[offset + 2],
            effect: backdrop
                ? LitchcraftEffectStyle.breathe
                : LitchcraftEffectStyle.off,
            effectDurationMs: backdrop ? 1600 : 0,
          );
        }, growable: false),
      );
    }, growable: false);
    final output = List<int>.filled(256, 0);
    final canvases = List<LitchcraftCanvas>.generate(32, (index) {
      composeLitchcraftAnimationFrameInto(
        frames[index],
        index * 50,
        output,
        frameIndex: index,
      );
      return LitchcraftCanvas(colors: output);
    }, growable: false);
    return LitchcraftRickrollPreset._(List.unmodifiable(frames), canvases);
  }

  final List<LitchcraftAnimationFrame> frames;
  final List<LitchcraftCanvas> _canvases;

  /// At most 32 immutable 256-color canvases; ticks only select a reference.
  LitchcraftCanvas canvasAt(int elapsedMs) =>
      _canvases[(elapsedMs < 0 ? 0 : elapsedMs) ~/ 50 % 32];
}
