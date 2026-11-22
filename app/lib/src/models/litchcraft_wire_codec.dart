import 'dart:typed_data';

import 'litchcraft_animation.dart';

const int litchcraftWireMaximumFrameBytes = 1123;
const int _repeat = 0;
const int _bitmap = 1;
const int _raw = 2;
const int _indexed = 3;
const int _rgbRaw = 4;
const int _rgbBitmap = 5;
const int _headerBytes = 3;
const int _bitmapBytes = 32;
const int _valueBytes = 5;
const int _rgbRawBytes = _headerBytes + litchcraftAnimationEmitterCount * 3;

bool _sameEmitter(
  LitchcraftAnimatedEmitter left,
  LitchcraftAnimatedEmitter right,
) =>
    left.color == right.color &&
    left.effect == right.effect &&
    left.effectDurationMs == right.effectDurationMs;

int _packedEffect(LitchcraftAnimatedEmitter emitter) =>
    emitter.effect.id | ((emitter.effectDurationMs ~/ 100) << 4);

void _writeUint16(Uint8List bytes, int offset, int value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >> 8) & 0xff;
}

int _readUint16(Uint8List bytes, int offset) =>
    bytes[offset] | (bytes[offset + 1] << 8);

void _writeValue(
  Uint8List bytes,
  int offset,
  LitchcraftAnimatedEmitter emitter,
) {
  bytes[offset] = (emitter.color >> 16) & 0xff;
  bytes[offset + 1] = (emitter.color >> 8) & 0xff;
  bytes[offset + 2] = emitter.color & 0xff;
  _writeUint16(bytes, offset + 3, _packedEffect(emitter));
}

LitchcraftAnimatedEmitter _readValue(Uint8List bytes, int offset) {
  final packed = _readUint16(bytes, offset + 3);
  return LitchcraftAnimatedEmitter(
    color: (bytes[offset] << 16) | (bytes[offset + 1] << 8) | bytes[offset + 2],
    effect: LitchcraftEffectStyle.fromId(packed & 0x0f),
    effectDurationMs: (packed >> 4) * 100,
  );
}

Uint8List encodeLitchcraftWireFrame({
  required LitchcraftAnimationFrame? previous,
  required LitchcraftAnimationFrame frame,
  bool enableRgbOnly = false,
}) {
  final changed = <int>[];
  if (previous != null) {
    for (var index = 0; index < litchcraftAnimationEmitterCount; index++) {
      if (!_sameEmitter(previous.emitters[index], frame.emitters[index])) {
        changed.add(index);
      }
    }
    if (changed.isEmpty) {
      final bytes = Uint8List(_headerBytes)..[0] = _repeat;
      _writeUint16(bytes, 1, frame.durationMs);
      return bytes;
    }
  }

  final indexedSize = _headerBytes + 1 + changed.length * 6;
  final bitmapSize = _headerBytes + _bitmapBytes + changed.length * _valueBytes;
  if (enableRgbOnly &&
      frame.emitters.every(
        (emitter) => emitter.effect == LitchcraftEffectStyle.off,
      )) {
    final legacySize = previous == null
        ? litchcraftWireMaximumFrameBytes
        : indexedSize < bitmapSize
        ? indexedSize
        : bitmapSize <= litchcraftWireMaximumFrameBytes
        ? bitmapSize
        : litchcraftWireMaximumFrameBytes;
    final rgbBitmapSize = _headerBytes + _bitmapBytes + changed.length * 3;
    if (previous != null &&
        rgbBitmapSize < legacySize &&
        rgbBitmapSize < _rgbRawBytes) {
      final bytes = Uint8List(rgbBitmapSize)..[0] = _rgbBitmap;
      _writeUint16(bytes, 1, frame.durationMs);
      var offset = _headerBytes + _bitmapBytes;
      for (final index in changed) {
        bytes[_headerBytes + index ~/ 8] |= 1 << (index % 8);
        final color = frame.emitters[index].color;
        bytes[offset++] = (color >> 16) & 0xff;
        bytes[offset++] = (color >> 8) & 0xff;
        bytes[offset++] = color & 0xff;
      }
      return bytes;
    }
    if (_rgbRawBytes < legacySize) {
      final bytes = Uint8List(_rgbRawBytes)..[0] = _rgbRaw;
      _writeUint16(bytes, 1, frame.durationMs);
      var offset = _headerBytes;
      for (final emitter in frame.emitters) {
        bytes[offset++] = (emitter.color >> 16) & 0xff;
        bytes[offset++] = (emitter.color >> 8) & 0xff;
        bytes[offset++] = emitter.color & 0xff;
      }
      return bytes;
    }
  }
  if (previous != null && indexedSize < bitmapSize) {
    final bytes = Uint8List(indexedSize)..[0] = _indexed;
    _writeUint16(bytes, 1, frame.durationMs);
    bytes[_headerBytes] = changed.length;
    var offset = _headerBytes + 1;
    for (final index in changed) {
      bytes[offset++] = index;
      _writeValue(bytes, offset, frame.emitters[index]);
      offset += _valueBytes;
    }
    return bytes;
  }
  if (previous != null && bitmapSize <= litchcraftWireMaximumFrameBytes) {
    final bytes = Uint8List(bitmapSize)..[0] = _bitmap;
    _writeUint16(bytes, 1, frame.durationMs);
    var offset = _headerBytes + _bitmapBytes;
    for (final index in changed) {
      bytes[_headerBytes + index ~/ 8] |= 1 << (index % 8);
      _writeValue(bytes, offset, frame.emitters[index]);
      offset += _valueBytes;
    }
    return bytes;
  }

  final bytes = Uint8List(litchcraftWireMaximumFrameBytes)..[0] = _raw;
  _writeUint16(bytes, 1, frame.durationMs);
  var offset = _headerBytes;
  for (final emitter in frame.emitters) {
    bytes[offset++] = (emitter.color >> 16) & 0xff;
    bytes[offset++] = (emitter.color >> 8) & 0xff;
    bytes[offset++] = emitter.color & 0xff;
  }
  for (var index = 0; index < frame.emitters.length; index++) {
    final packed = _packedEffect(frame.emitters[index]);
    final firstBit = index * 11;
    for (var bit = 0; bit < 11; bit++) {
      if ((packed & (1 << bit)) == 0) continue;
      final destination = firstBit + bit;
      bytes[offset + destination ~/ 8] |= 1 << (destination % 8);
    }
  }
  return bytes;
}

LitchcraftAnimationFrame decodeLitchcraftWireFrame({
  required LitchcraftAnimationFrame? previous,
  required Uint8List bytes,
  bool enableRgbOnly = false,
}) {
  if (bytes.length < _headerBytes) {
    throw const FormatException('Truncated Litchcraft wire frame');
  }
  final durationMs = _readUint16(bytes, 1);
  var emitters = previous?.emitters.toList();
  switch (bytes[0]) {
    case _repeat:
      if (previous == null || bytes.length != _headerBytes) {
        throw const FormatException('Invalid repeated Litchcraft wire frame');
      }
    case _indexed:
      if (emitters == null || bytes.length < _headerBytes + 1) {
        throw const FormatException('Invalid indexed Litchcraft wire frame');
      }
      final changed = bytes[_headerBytes];
      if (changed == 0 || bytes.length != _headerBytes + 1 + changed * 6) {
        throw const FormatException('Invalid indexed Litchcraft wire length');
      }
      var offset = _headerBytes + 1;
      var prior = -1;
      for (var change = 0; change < changed; change++) {
        final index = bytes[offset++];
        if (index <= prior) {
          throw const FormatException('Non-canonical indexed wire frame');
        }
        prior = index;
        emitters[index] = _readValue(bytes, offset);
        offset += _valueBytes;
      }
    case _bitmap:
      if (emitters == null || bytes.length < _headerBytes + _bitmapBytes) {
        throw const FormatException('Invalid bitmap Litchcraft wire frame');
      }
      var offset = _headerBytes + _bitmapBytes;
      for (var index = 0; index < litchcraftAnimationEmitterCount; index++) {
        if ((bytes[_headerBytes + index ~/ 8] & (1 << (index % 8))) == 0) {
          continue;
        }
        if (offset + _valueBytes > bytes.length) {
          throw const FormatException('Truncated bitmap Litchcraft wire frame');
        }
        emitters[index] = _readValue(bytes, offset);
        offset += _valueBytes;
      }
      if (offset != bytes.length) {
        throw const FormatException('Invalid bitmap Litchcraft wire length');
      }
    case _raw:
      if (bytes.length != litchcraftWireMaximumFrameBytes) {
        throw const FormatException('Invalid raw Litchcraft wire frame');
      }
      emitters = List<LitchcraftAnimatedEmitter>.filled(
        litchcraftAnimationEmitterCount,
        const LitchcraftAnimatedEmitter(color: 0),
      );
      var offset = _headerBytes;
      final colors = List<int>.filled(litchcraftAnimationEmitterCount, 0);
      for (var index = 0; index < colors.length; index++) {
        colors[index] =
            (bytes[offset] << 16) |
            (bytes[offset + 1] << 8) |
            bytes[offset + 2];
        offset += 3;
      }
      for (var index = 0; index < emitters.length; index++) {
        var packed = 0;
        final firstBit = index * 11;
        for (var bit = 0; bit < 11; bit++) {
          final source = firstBit + bit;
          if ((bytes[offset + source ~/ 8] & (1 << (source % 8))) != 0) {
            packed |= 1 << bit;
          }
        }
        emitters[index] = LitchcraftAnimatedEmitter(
          color: colors[index],
          effect: LitchcraftEffectStyle.fromId(packed & 0x0f),
          effectDurationMs: (packed >> 4) * 100,
        );
      }
    case _rgbRaw:
      if (!enableRgbOnly || bytes.length != _rgbRawBytes) {
        throw const FormatException('Invalid RGB raw Litchcraft wire frame');
      }
      emitters = <LitchcraftAnimatedEmitter>[];
      for (var offset = _headerBytes; offset < bytes.length; offset += 3) {
        emitters.add(
          LitchcraftAnimatedEmitter(
            color:
                (bytes[offset] << 16) |
                (bytes[offset + 1] << 8) |
                bytes[offset + 2],
          ),
        );
      }
    case _rgbBitmap:
      if (!enableRgbOnly ||
          emitters == null ||
          bytes.length < _headerBytes + _bitmapBytes) {
        throw const FormatException('Invalid RGB bitmap Litchcraft wire frame');
      }
      var offset = _headerBytes + _bitmapBytes;
      for (var index = 0; index < litchcraftAnimationEmitterCount; index++) {
        if ((bytes[_headerBytes + index ~/ 8] & (1 << (index % 8))) == 0) {
          final inherited = emitters[index];
          if (inherited.effect != LitchcraftEffectStyle.off ||
              inherited.effectDurationMs != 0) {
            throw const FormatException(
              'RGB bitmap cannot inherit an active Litchcraft effect',
            );
          }
          continue;
        }
        if (offset + 3 > bytes.length) {
          throw const FormatException(
            'Truncated RGB bitmap Litchcraft wire frame',
          );
        }
        emitters[index] = LitchcraftAnimatedEmitter(
          color:
              (bytes[offset] << 16) |
              (bytes[offset + 1] << 8) |
              bytes[offset + 2],
        );
        offset += 3;
      }
      if (offset != bytes.length) {
        throw const FormatException(
          'Invalid RGB bitmap Litchcraft wire length',
        );
      }
    default:
      throw const FormatException('Unknown Litchcraft wire frame encoding');
  }
  final decoded = emitters;
  if (decoded == null) {
    throw const FormatException('Wire frame has no emitter source');
  }
  return LitchcraftAnimationFrame(emitters: decoded, durationMs: durationMs);
}
