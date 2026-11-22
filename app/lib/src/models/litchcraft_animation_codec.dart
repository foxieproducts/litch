import 'dart:typed_data';

import 'litchcraft_animation.dart';
import 'litchcraft_state.dart';

class LitchcraftAnimationEncoding {
  const LitchcraftAnimationEncoding({required this.bytes, required this.crc32});

  final Uint8List bytes;
  final int crc32;
}

int litchcraftAnimationCrc32(List<int> bytes) {
  var state = 0xffffffff;
  for (final byte in bytes) {
    if (byte < 0 || byte > 0xff) {
      throw const FormatException('CRC input byte is out of range');
    }
    state ^= byte;
    for (var bit = 0; bit < 8; bit++) {
      state = (state >>> 1) ^ ((state & 1) == 0 ? 0 : 0xedb88320);
    }
  }
  return (~state) & 0xffffffff;
}

LitchcraftAnimationEncoding encodeLitchcraftAnimationDocument(
  LitchcraftAnimationDocument document, {
  bool localStorage = false,
}) {
  final extended =
      document.frames.length > litchcraftAnimationLegacyMaximumFrameCount;
  if (extended && !localStorage) {
    throw const FormatException('Large artwork requires frame-store transport');
  }
  final length = 32 + document.frames.length * 1538;
  final bytes = Uint8List(length);
  final data = ByteData.sublistView(bytes);
  // App storage v4 retains the raw semantic representation, with zero meaning
  // 256 frames. BLE schema-4 uses the separate compressed wire codec.
  bytes[0] = extended ? 4 : litchcraftAnimationSchema;
  bytes[1] = document.frames.length & 255;
  bytes[2] = document.topology.id;
  bytes[3] = document.playbackOnEntry ? 1 : 0;
  data.setUint32(4, document.sparkleSeed, Endian.little);
  var offset = 8;
  for (final favorite in document.favoriteColors) {
    bytes[offset++] = (favorite >> 16) & 0xff;
    bytes[offset++] = (favorite >> 8) & 0xff;
    bytes[offset++] = favorite & 0xff;
  }
  for (final frame in document.frames) {
    data.setUint16(offset, frame.durationMs, Endian.little);
    offset += 2;
    for (final emitter in frame.emitters) {
      bytes[offset++] = (emitter.color >> 16) & 0xff;
      bytes[offset++] = (emitter.color >> 8) & 0xff;
      bytes[offset++] = emitter.color & 0xff;
      bytes[offset++] = emitter.effect.id;
      data.setUint16(offset, emitter.effectDurationMs, Endian.little);
      offset += 2;
    }
  }
  return LitchcraftAnimationEncoding(
    bytes: bytes,
    crc32: litchcraftAnimationCrc32(bytes),
  );
}

LitchcraftAnimationDocument decodeLitchcraftAnimationDocument(
  List<int> source,
  int expectedCrc32, {
  bool localStorage = false,
}) {
  final maximumBytes = localStorage
      ? 32 + 256 * 1538
      : litchcraftAnimationMaximumEncodedBytes;
  if (source.length < 32 || source.length > maximumBytes) {
    throw const FormatException('Invalid Litchcraft animation bytes');
  }
  final extended = localStorage && source[0] == 4;
  final frameCount = extended && source[1] == 0 ? 256 : source[1];
  final expectedLength = 32 + frameCount * 1538;
  if ((!extended && source[0] != litchcraftAnimationSchema) ||
      frameCount < 1 ||
      frameCount >
          (extended
              ? litchcraftAnimationMaximumFrameCount
              : litchcraftAnimationLegacyMaximumFrameCount) ||
      source[2] < 1 ||
      source[2] > 3 ||
      source[3] & 0xfe != 0 ||
      source.length != expectedLength ||
      expectedCrc32 < 0 ||
      expectedCrc32 > 0xffffffff) {
    throw const FormatException('Invalid Litchcraft animation envelope');
  }
  // Admission bounds are established before the only input copy and CRC pass.
  if (source.any((byte) => byte < 0 || byte > 0xff) ||
      litchcraftAnimationCrc32(source) != expectedCrc32) {
    throw const FormatException('Invalid Litchcraft animation envelope');
  }
  final bytes = Uint8List.fromList(source);
  final data = ByteData.sublistView(bytes);
  final seed = data.getUint32(4, Endian.little);
  if (seed == 0) {
    throw const FormatException('Invalid Litchcraft animation seed');
  }
  var offset = 8;
  final favorites = <int>[];
  for (var index = 0; index < 8; index++) {
    favorites.add(
      bytes[offset++] << 16 | bytes[offset++] << 8 | bytes[offset++],
    );
  }
  final frames = <LitchcraftAnimationFrame>[];
  for (var frameIndex = 0; frameIndex < frameCount; frameIndex++) {
    final duration = data.getUint16(offset, Endian.little);
    offset += 2;
    final emitters = <LitchcraftAnimatedEmitter>[];
    for (
      var emitterIndex = 0;
      emitterIndex < litchcraftAnimationEmitterCount;
      emitterIndex++
    ) {
      final color =
          bytes[offset++] << 16 | bytes[offset++] << 8 | bytes[offset++];
      final effect = LitchcraftEffectStyle.fromId(bytes[offset++]);
      final effectDuration = data.getUint16(offset, Endian.little);
      offset += 2;
      emitters.add(
        LitchcraftAnimatedEmitter(
          color: color,
          effect: effect,
          effectDurationMs: effectDuration,
        ),
      );
    }
    frames.add(
      LitchcraftAnimationFrame(emitters: emitters, durationMs: duration),
    );
  }
  return LitchcraftAnimationDocument(
    frames: frames,
    topology: LitchcraftTopology.fromId(bytes[2]),
    favoriteColors: favorites,
    playbackOnEntry: bytes[3] == 1,
    sparkleSeed: seed,
  );
}
