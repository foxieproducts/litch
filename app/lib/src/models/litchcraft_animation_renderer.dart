import 'litchcraft_animation.dart';

class LitchcraftComposedAnimationFrame {
  const LitchcraftComposedAnimationFrame({
    required this.frameIndex,
    required this.colors,
    required this.emittersEvaluated,
    required this.groupCandidatesVisited,
  });

  final int frameIndex;
  final List<int> colors;
  final int emittersEvaluated;
  final int groupCandidatesVisited;
}

class LitchcraftAnimationRenderStats {
  int emittersEvaluated = 0;
  int groupCandidatesVisited = 0;
}

/// Immutable runtime clock state. Callers inject one monotonic sample for each
/// rendered frame; pause and seek do not mutate the artwork document.
class LitchcraftPlaybackCursor {
  LitchcraftPlaybackCursor({
    this.elapsedMs = 0,
    this.anchorMs = 0,
    this.isPlaying = false,
  }) {
    if (elapsedMs < 0 || anchorMs < 0) {
      throw ArgumentError('Animation time must be monotonic and nonnegative');
    }
  }

  final int elapsedMs;
  final int anchorMs;
  final bool isPlaying;

  int sample(int monotonicMs) {
    if (monotonicMs < 0) {
      throw ArgumentError.value(monotonicMs, 'monotonicMs');
    }
    return isPlaying && monotonicMs >= anchorMs
        ? elapsedMs + monotonicMs - anchorMs
        : elapsedMs;
  }

  LitchcraftPlaybackCursor pause(int monotonicMs) {
    if (monotonicMs < 0) {
      throw ArgumentError.value(monotonicMs, 'monotonicMs');
    }
    return LitchcraftPlaybackCursor(
      elapsedMs: sample(monotonicMs),
      anchorMs: monotonicMs,
    );
  }

  LitchcraftPlaybackCursor resume(int monotonicMs) {
    if (monotonicMs < 0) {
      throw ArgumentError.value(monotonicMs, 'monotonicMs');
    }
    if (isPlaying) return this;
    return LitchcraftPlaybackCursor(
      elapsedMs: elapsedMs,
      anchorMs: monotonicMs,
      isPlaying: true,
    );
  }

  LitchcraftPlaybackCursor seek(int value, int monotonicMs) {
    if (value < 0 || monotonicMs < 0) {
      throw ArgumentError('Animation time must be monotonic and nonnegative');
    }
    return LitchcraftPlaybackCursor(
      elapsedMs: value,
      anchorMs: monotonicMs,
      isPlaying: isPlaying,
    );
  }

  LitchcraftPlaybackCursor? selectFrame(
    LitchcraftAnimationDocument document,
    int frameIndex,
    int monotonicMs,
  ) {
    final frameStart = litchcraftAnimationFrameStartOrNull(
      document,
      frameIndex,
    );
    if (frameStart == null || monotonicMs < 0) return null;
    return pause(monotonicMs).seek(frameStart, monotonicMs);
  }
}

const int _phaseScale = 65536;
const int _channelScale = 65535;

int _phase(int elapsedMs, int durationMs) =>
    (elapsedMs % durationMs) * _phaseScale ~/ durationMs;

int _scaleChannel(int channel, int scale) =>
    (channel * scale + _channelScale ~/ 2) ~/ _channelScale;

int _scaleColor(int color, int scale) =>
    _scaleChannel((color >> 16) & 0xff, scale) << 16 |
    _scaleChannel((color >> 8) & 0xff, scale) << 8 |
    _scaleChannel(color & 0xff, scale);

int _roundedDivide(int numerator, int denominator) =>
    (numerator + denominator ~/ 2) ~/ denominator;

int _rotateHue(int color, int phase) {
  final red = (color >> 16) & 0xff;
  final green = (color >> 8) & 0xff;
  final blue = color & 0xff;
  final maximum = [red, green, blue].reduce((a, b) => a > b ? a : b);
  final minimum = [red, green, blue].reduce((a, b) => a < b ? a : b);
  final chroma = maximum - minimum;
  if (chroma == 0) return color;

  late final int hue;
  if (maximum == red) {
    hue = green >= blue
        ? _roundedDivide(256 * (green - blue), chroma)
        : 1536 - _roundedDivide(256 * (blue - green), chroma);
  } else if (maximum == green) {
    hue = blue >= red
        ? 512 + _roundedDivide(256 * (blue - red), chroma)
        : 512 - _roundedDivide(256 * (red - blue), chroma);
  } else {
    hue = red >= green
        ? 1024 + _roundedDivide(256 * (red - green), chroma)
        : 1024 - _roundedDivide(256 * (green - red), chroma);
  }

  final rotation = phase * 1536 ~/ _phaseScale;
  final rotated = ((hue % 1536) + rotation) % 1536;
  final sector = rotated ~/ 256;
  final fraction = rotated % 256;
  final rising = minimum + _roundedDivide(chroma * fraction, 256);
  final falling = minimum + chroma - _roundedDivide(chroma * fraction, 256);
  return switch (sector) {
    0 => maximum << 16 | rising << 8 | minimum,
    1 => falling << 16 | maximum << 8 | minimum,
    2 => minimum << 16 | maximum << 8 | rising,
    3 => minimum << 16 | falling << 8 | maximum,
    4 => rising << 16 | minimum << 8 | maximum,
    _ => maximum << 16 | minimum << 8 | falling,
  };
}

int _sparkleSlot(int seed, int frameIndex, int emitterIndex, int cycle) {
  var hash =
      (seed ^ (frameIndex * 0x9e3779b9) ^ (emitterIndex * 0x85ebca6b) ^ cycle) &
      0xffffffff;
  hash = (hash ^ (hash << 13)) & 0xffffffff;
  hash = (hash ^ (hash >> 17)) & 0xffffffff;
  hash = (hash ^ (hash << 5)) & 0xffffffff;
  return hash & 3;
}

int _composeIndependent(
  LitchcraftAnimatedEmitter emitter,
  int elapsedMs,
  int sparkleSeed,
  int frameIndex,
  int emitterIndex,
) {
  if (emitter.color == 0 || emitter.effect == LitchcraftEffectStyle.off) {
    return emitter.color;
  }
  final phase = _phase(elapsedMs, emitter.effectDurationMs);
  final triangle = 65535 - (2 * phase - 65535).abs();
  return switch (emitter.effect) {
    LitchcraftEffectStyle.breathe => _scaleColor(
      emitter.color,
      16384 + 49151 * triangle ~/ 65535,
    ),
    LitchcraftEffectStyle.flash => phase < 32768 ? emitter.color : 0,
    LitchcraftEffectStyle.fadeToBlack => _scaleColor(emitter.color, triangle),
    LitchcraftEffectStyle.rainbow => _rotateHue(emitter.color, phase),
    LitchcraftEffectStyle.sparkle =>
      phase * 4 ~/ _phaseScale ==
              _sparkleSlot(
                sparkleSeed,
                frameIndex,
                emitterIndex,
                elapsedMs ~/ emitter.effectDurationMs,
              )
          ? emitter.color
          : _scaleColor(emitter.color, 16384),
    _ => emitter.color,
  };
}

int _composeSquareUnchecked({
  required List<LitchcraftAnimatedEmitter> emitters,
  required int emitterOffset,
  required int semanticBaseIndex,
  required List<int> output,
  required int outputOffset,
  required int elapsedMs,
  required int sparkleSeed,
  required int frameIndex,
}) {
  var groupCandidatesVisited = 0;
  for (var region = 0; region < 4; region++) {
    final emitterIndex = emitterOffset + region;
    final semanticIndex = semanticBaseIndex + region;
    final outputIndex = outputOffset + region;
    final emitter = emitters[emitterIndex];
    output[outputIndex] = _composeIndependent(
      emitter,
      elapsedMs,
      sparkleSeed,
      frameIndex,
      semanticIndex,
    );
    if (emitter.effect != LitchcraftEffectStyle.orbitClockwise &&
        emitter.effect != LitchcraftEffectStyle.orbitCounterclockwise &&
        emitter.effect != LitchcraftEffectStyle.bounce) {
      continue;
    }
    var memberCount = 0;
    var memberPosition = 0;
    for (var candidateRegion = 0; candidateRegion < 4; candidateRegion++) {
      groupCandidatesVisited++;
      final candidate = emitters[emitterOffset + candidateRegion];
      if (candidate.effect != emitter.effect ||
          candidate.effectDurationMs != emitter.effectDurationMs) {
        continue;
      }
      if (candidateRegion == region) memberPosition = memberCount;
      memberCount++;
    }
    final phase = _phase(elapsedMs, emitter.effectDurationMs);
    late int activePosition;
    if (emitter.effect == LitchcraftEffectStyle.bounce && memberCount > 1) {
      final pathLength = 2 * memberCount - 2;
      final step = phase * pathLength ~/ _phaseScale;
      activePosition = step < memberCount ? step : pathLength - step;
    } else {
      activePosition = phase * memberCount ~/ _phaseScale;
      if (emitter.effect == LitchcraftEffectStyle.orbitCounterclockwise) {
        activePosition = memberCount - 1 - activePosition;
      }
    }
    output[outputIndex] = memberPosition == activePosition ? emitter.color : 0;
  }
  return groupCandidatesVisited;
}

/// Composes four semantic emitters in left/top/right/bottom order.
List<int> composeLitchcraftAnimatedSquare({
  required List<LitchcraftAnimatedEmitter> emitters,
  required int elapsedMs,
  required int sparkleSeed,
  required int frameIndex,
  required int emitterBaseIndex,
}) {
  if (emitters.length != 4 ||
      emitters.any((emitter) => !emitter.isValid) ||
      elapsedMs < 0 ||
      sparkleSeed < 1 ||
      sparkleSeed > 0xffffffff ||
      frameIndex < 0 ||
      frameIndex >= litchcraftAnimationMaximumFrameCount ||
      emitterBaseIndex < 0 ||
      emitterBaseIndex > 252 ||
      emitterBaseIndex % 4 != 0) {
    throw ArgumentError('Invalid Litchcraft animation sample');
  }

  final output = List<int>.filled(4, 0);
  _composeSquareUnchecked(
    emitters: emitters,
    emitterOffset: 0,
    semanticBaseIndex: emitterBaseIndex,
    output: output,
    outputOffset: 0,
    elapsedMs: elapsedMs,
    sparkleSeed: sparkleSeed,
    frameIndex: frameIndex,
  );
  return List<int>.unmodifiable(output);
}

int? litchcraftAnimationFrameStartOrNull(
  LitchcraftAnimationDocument document,
  int frameIndex,
) {
  if (frameIndex < 0 || frameIndex >= document.frames.length) {
    return null;
  }
  var start = 0;
  for (var index = 0; index < frameIndex; index++) {
    start += document.frames[index].durationMs;
  }
  return start;
}

int litchcraftAnimationFrameIndexAt(
  LitchcraftAnimationDocument document,
  int elapsedMs,
) {
  if (elapsedMs < 0) throw ArgumentError.value(elapsedMs, 'elapsedMs');
  var totalDuration = 0;
  for (final frame in document.frames) {
    totalDuration += frame.durationMs;
  }
  final loopElapsed = elapsedMs % totalDuration;
  var boundary = 0;
  for (var index = 0; index < document.frames.length; index++) {
    boundary += document.frames[index].durationMs;
    if (loopElapsed < boundary) return index;
  }
  return 0;
}

/// Composes into a caller-owned buffer. The 50 Hz hot path creates no
/// per-square lists and callers can reuse both [output] and [stats].
/// [elapsedMs] selects the frame; [effectElapsedMs] advances effects independently
/// when frame playback is paused. Omit it for a deterministic document sample.
int composeLitchcraftAnimationDocumentInto(
  LitchcraftAnimationDocument document,
  int elapsedMs,
  List<int> output, {
  LitchcraftAnimationRenderStats? stats,
  int? effectElapsedMs,
}) {
  if (output.length != litchcraftAnimationEmitterCount) {
    throw ArgumentError('Animation output must contain 256 emitters');
  }
  final frameIndex = litchcraftAnimationFrameIndexAt(document, elapsedMs);
  final frame = document.frames[frameIndex];
  composeLitchcraftAnimationFrameInto(
    frame,
    effectElapsedMs ?? elapsedMs,
    output,
    sparkleSeed: document.sparkleSeed,
    frameIndex: frameIndex,
    stats: stats,
  );
  return frameIndex;
}

/// Shared single-frame compositor for editable documents and streamed presets.
/// Frame-store indices are not constrained by the eight-frame editor model.
void composeLitchcraftAnimationFrameInto(
  LitchcraftAnimationFrame frame,
  int elapsedMs,
  List<int> output, {
  int sparkleSeed = 1,
  int frameIndex = 0,
  LitchcraftAnimationRenderStats? stats,
}) {
  if (output.length != litchcraftAnimationEmitterCount ||
      elapsedMs < 0 ||
      frameIndex < 0 ||
      frameIndex >= 256 ||
      sparkleSeed < 1 ||
      sparkleSeed > 0xffffffff) {
    throw ArgumentError('Invalid animation frame composition');
  }
  var groupCandidatesVisited = 0;
  for (var square = 0; square < 64; square++) {
    final base = square * 4;
    groupCandidatesVisited += _composeSquareUnchecked(
      emitters: frame.emitters,
      emitterOffset: base,
      semanticBaseIndex: base,
      output: output,
      outputOffset: base,
      elapsedMs: elapsedMs,
      sparkleSeed: sparkleSeed,
      frameIndex: frameIndex,
    );
  }
  if (stats != null) {
    stats.emittersEvaluated = litchcraftAnimationEmitterCount;
    stats.groupCandidatesVisited = groupCandidatesVisited;
  }
}

LitchcraftComposedAnimationFrame composeLitchcraftAnimationDocument(
  LitchcraftAnimationDocument document,
  int elapsedMs, {
  int? effectElapsedMs,
}) {
  final output = List<int>.filled(litchcraftAnimationEmitterCount, 0);
  final stats = LitchcraftAnimationRenderStats();
  final frameIndex = composeLitchcraftAnimationDocumentInto(
    document,
    elapsedMs,
    output,
    stats: stats,
    effectElapsedMs: effectElapsedMs,
  );
  return LitchcraftComposedAnimationFrame(
    frameIndex: frameIndex,
    colors: List<int>.unmodifiable(output),
    emittersEvaluated: stats.emittersEvaluated,
    groupCandidatesVisited: stats.groupCandidatesVisited,
  );
}
