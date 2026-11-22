// Container parsing and raster codecs are separate from the asynchronous API
// so the worker isolate never depends on the editor's Flutter canvas painter.
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:image/image.dart' as image;

import '../models/litchcraft_animation.dart';
import '../models/litchcraft_state.dart';
import 'litchcraft_media.dart';

const _pngSignature = <int>[137, 80, 78, 71, 13, 10, 26, 10];
const _maximumSourceFrames = 4096;

Never _fail(LitchcraftMediaErrorCode code, String message) =>
    throw LitchcraftMediaException(code, message);
Never _malformed() => _fail(
  LitchcraftMediaErrorCode.malformed,
  'The image is damaged or incomplete.',
);

int _u16(Uint8List bytes, int offset, [Endian endian = Endian.big]) =>
    ByteData.sublistView(bytes, offset, offset + 2).getUint16(0, endian);
int _u32(Uint8List bytes, int offset) =>
    ByteData.sublistView(bytes, offset, offset + 4).getUint32(0);
Uint8List _words(List<int> values) {
  final data = ByteData(values.length * 4);
  for (var index = 0; index < values.length; index++) {
    data.setUint32(index * 4, values[index]);
  }
  return data.buffer.asUint8List();
}

// PNG's CRC is also the canonical document CRC polynomial. The local table
// belongs to this parser invocation, avoiding mutable process-wide state.
class _Crc {
  final List<int> table = List<int>.generate(256, (value) {
    for (var bit = 0; bit < 8; bit++) {
      value = value & 1 != 0 ? 0xedb88320 ^ (value >> 1) : value >> 1;
    }
    return value;
  });
  int of(List<int> bytes) {
    var value = 0xffffffff;
    for (final byte in bytes) {
      value = table[(value ^ byte) & 255] ^ (value >> 8);
    }
    return value ^ 0xffffffff;
  }
}

Uint8List _chunk(String type, Uint8List data, _Crc crc) {
  final result = Uint8List(data.length + 12);
  final words = ByteData.sublistView(result);
  words.setUint32(0, data.length);
  result.setRange(4, 8, type.codeUnits);
  result.setRange(8, result.length - 4, data);
  words.setUint32(
    result.length - 4,
    crc.of(Uint8List.sublistView(result, 4, result.length - 4)),
  );
  return result;
}

class _BoundedBytes {
  _BoundedBytes({
    this.retainBytes = true,
    this.maximumBytes = litchcraftMediaMaximumEncodedBytes,
  });
  final bool retainBytes;
  final int maximumBytes;
  final _builder = BytesBuilder(copy: false);
  List<Uint8List> _pending = [];
  int length = 0;
  int peakRetainedLength = 0;
  int get retainedLength => retainBytes
      ? _builder.length
      : _pending.fold<int>(0, (sum, bytes) => sum + bytes.length);
  void add(List<int> bytes) {
    if (length + bytes.length > maximumBytes) {
      _fail(
        LitchcraftMediaErrorCode.outputTooLarge,
        maximumBytes == litchcraftMediaMaximumBufferedBytes
            ? 'Buffered export exceeds 6 MiB. Use a streamed file export, smaller size or shorter duration.'
            : 'The encoded image exceeds 32 MiB. Choose a smaller size or shorter duration.',
      );
    }
    if (retainBytes) {
      _builder.add(bytes);
    } else {
      _pending.add(bytes is Uint8List ? bytes : Uint8List.fromList(bytes));
    }
    length += bytes.length;
    peakRetainedLength = math.max(peakRetainedLength, retainedLength);
  }

  List<Uint8List> takeChunks() {
    final chunks = _pending;
    _pending = [];
    return chunks;
  }

  Uint8List finish() => retainBytes ? _builder.takeBytes() : Uint8List(0);
}

/// Holds encoded chunks only. It never retains an uncompressed raster.
class LitchcraftMediaContainerEncoder {
  LitchcraftMediaContainerEncoder(
    this.settings,
    this.plan, {
    bool retainBytes = true,
  }) : _output = _BoundedBytes(
         retainBytes: retainBytes,
         maximumBytes: retainBytes
             ? litchcraftMediaMaximumBufferedBytes
             : litchcraftMediaMaximumEncodedBytes,
       );
  final LitchcraftMediaExportSettings settings;
  final LitchcraftMediaExportPlan plan;
  final _BoundedBytes _output;
  final _crc = _Crc();
  var _sequence = 0;
  var _count = 0;
  int get encodedByteLength => _output.length;
  int get retainedByteLength => _output.retainedLength;
  int get peakBufferedByteLength => _output.peakRetainedLength;
  List<Uint8List> takeChunks() => _output.takeChunks();

  void addFrame(Uint8List encoded, int index) {
    if (!_output.retainBytes && retainedByteLength != 0) {
      throw StateError('Drain encoded chunks before adding the next frame.');
    }
    if (index != _count || index >= plan.frameCount) _malformed();
    if (settings.format == LitchcraftMediaFormat.gif) {
      // Each standalone GIF uses a local palette. Keep the first logical
      // screen header and append just the GCE + image block for every frame.
      final source = _inspectMediaSource(encoded);
      if (index == 0) {
        _output.add(source.gifHeader!);
        if (plan.repeats) {
          _output.add(<int>[
            33,
            255,
            11,
            ...'NETSCAPE2.0'.codeUnits,
            3,
            1,
            0,
            0,
            0,
          ]);
        }
      }
      final frame = source.frames.single;
      _output.add(<int>[
        33,
        249,
        4,
        4,
        (plan.frameDurationMs(index) ~/ 10) & 255,
        (plan.frameDurationMs(index) ~/ 10) >> 8,
        0,
        0,
      ]);
      _output.add(frame.gifPayload!);
    } else {
      if (index == 0) {
        _output.add(_pngSignature);
        _output.add(
          _chunk('IHDR', _pngHeader(settings.size, settings.size), _crc),
        );
        _output.add(
          _chunk('acTL', _words([plan.frameCount, plan.repeats ? 0 : 1]), _crc),
        );
      }
      final control = ByteData(26)
        ..setUint32(0, _sequence++)
        ..setUint32(4, settings.size)
        ..setUint32(8, settings.size)
        ..setUint16(20, plan.frameDurationMs(index))
        ..setUint16(22, 1000);
      _output.add(_chunk('fcTL', control.buffer.asUint8List(), _crc));
      // The worker returns one deflated scanline stream, not a whole PNG.
      if (index == 0) {
        _output.add(_chunk('IDAT', encoded, _crc));
      } else {
        final data = Uint8List(encoded.length + 4)
          ..setRange(0, 4, _words([_sequence++]))
          ..setRange(4, encoded.length + 4, encoded);
        _output.add(_chunk('fdAT', data, _crc));
      }
    }
    _count++;
  }

  Uint8List finish() {
    if (_count != plan.frameCount) _malformed();
    if (settings.format == LitchcraftMediaFormat.gif) {
      _output.add(const [59]);
    } else {
      _output.add(_chunk('IEND', Uint8List(0), _crc));
    }
    return _output.finish();
  }
}

Uint8List _pngHeader(int width, int height, {Uint8List? source}) {
  final bytes = source == null ? Uint8List(13) : Uint8List.fromList(source);
  final data = ByteData.sublistView(bytes);
  data.setUint32(0, width);
  data.setUint32(4, height);
  if (source == null) {
    bytes[8] = 8;
    bytes[9] = 6;
  }
  return bytes;
}

class _EncodingSink implements Sink<List<int>> {
  final bytes = _BoundedBytes();
  @override
  void add(List<int> data) => bytes.add(data);
  @override
  void close() {}
}

Uint8List encodeLitchcraftMediaRaster(
  Uint8List rgba,
  int size,
  LitchcraftMediaFormat format,
  int durationMs,
) {
  if (rgba.length != size * size * 4 || size > 512) _malformed();
  if (format == LitchcraftMediaFormat.apng) {
    final sink = _EncodingSink();
    final encoder = ZLibEncoder(level: 6).startChunkedConversion(sink);
    // One scanline scratch buffer instead of an entire filtered raster.
    final row = Uint8List(size * 4 + 1);
    for (var y = 0; y < size; y++) {
      row.setRange(1, row.length, rgba, y * size * 4);
      encoder.add(row);
    }
    encoder.close();
    return sink.bytes.finish();
  }
  final palette = image.PaletteUint8(256, 3);
  for (var index = 0; index < 256; index++) {
    palette.setRgb(
      index,
      ((index >> 5) * 255 / 7).round(),
      (((index >> 2) & 7) * 255 / 7).round(),
      ((index & 3) * 255 / 3).round(),
    );
  }
  final frame = image.Image(
    width: size,
    height: size,
    numChannels: 1,
    palette: palette,
  );
  final indices = frame.data!.toUint8List();
  for (var pixel = 0; pixel < indices.length; pixel++) {
    final offset = pixel * 4;
    final alpha = rgba[offset + 3];
    final red = (rgba[offset] * alpha + 127) ~/ 255;
    final green = (rgba[offset + 1] * alpha + 127) ~/ 255;
    final blue = (rgba[offset + 2] * alpha + 127) ~/ 255;
    indices[pixel] =
        ((red * 7 + 127) ~/ 255 << 5) |
        ((green * 7 + 127) ~/ 255 << 2) |
        (blue * 3 + 127) ~/ 255;
  }
  return image.GifEncoder(dispose: 1).encode(frame);
}

class _RasterFrame {
  _RasterFrame({
    required this.width,
    required this.height,
    this.x = 0,
    this.y = 0,
    this.durationMs = 1000,
    this.dispose = 0,
    this.over = false,
    this.transparent = -1,
    this.gifPayload,
  });
  final int width;
  final int height;
  final int x;
  final int y;
  final int durationMs;
  final int dispose;
  final bool over;
  final int transparent;
  final Uint8List? gifPayload;
  final List<Uint8List> compressed = [];
  int get compressedLength =>
      compressed.fold(0, (sum, bytes) => sum + bytes.length);
}

class _MediaSource {
  _MediaSource(
    this.metadata,
    this.frames, {
    this.pngHeader,
    this.gifHeader,
    this.gifBackground = 0,
    this.pngPalette,
    this.pngTransparency,
  });
  final LitchcraftMediaMetadata metadata;
  final List<_RasterFrame> frames;
  final Uint8List? pngHeader;
  final Uint8List? pngPalette;
  final Uint8List? pngTransparency;
  final Uint8List? gifHeader;
  final int gifBackground;
}

LitchcraftMediaMetadata inspectLitchcraftMediaSource(Uint8List bytes) =>
    _inspectMediaSource(bytes).metadata;

_MediaSource _inspectMediaSource(Uint8List bytes) {
  if (bytes.length > litchcraftMediaMaximumEncodedBytes) {
    _fail(
      LitchcraftMediaErrorCode.inputTooLarge,
      'Images must be 32 MiB or smaller.',
    );
  }
  try {
    if (bytes.length >= 8 &&
        Iterable<int>.generate(
          8,
        ).every((index) => bytes[index] == _pngSignature[index])) {
      return _inspectPng(bytes);
    }
    if (bytes.length >= 13 &&
        const [
          'GIF87a',
          'GIF89a',
        ].contains(ascii.decode(Uint8List.sublistView(bytes, 0, 6)))) {
      return _inspectGif(bytes);
    }
    _fail(
      LitchcraftMediaErrorCode.unsupported,
      'Choose a GIF, PNG or animated PNG image.',
    );
  } on LitchcraftMediaException {
    rethrow;
  } catch (_) {
    _malformed();
  }
}

void _dimensions(int width, int height) {
  if (width < 1 || height < 1) _malformed();
  if (width > 4096 ||
      height > 4096 ||
      width * height * 4 > litchcraftMediaMaximumWorkingBytes - 65536) {
    _fail(
      LitchcraftMediaErrorCode.dimensionsTooLarge,
      'This image is too large to convert safely. Resize it before importing.',
    );
  }
}

void _frameBounds(_RasterFrame frame, int width, int height) {
  if (frame.width < 1 ||
      frame.height < 1 ||
      frame.x + frame.width > width ||
      frame.y + frame.height > height) {
    _malformed();
  }
}

_MediaSource _inspectGif(Uint8List bytes) {
  final width = _u16(bytes, 6, Endian.little);
  final height = _u16(bytes, 8, Endian.little);
  _dimensions(width, height);
  var offset = 13;
  final globalSize = bytes[10] & 128 == 0
      ? 0
      : 3 * (1 << ((bytes[10] & 7) + 1));
  offset += globalSize;
  if (offset > bytes.length) _malformed();
  final header = Uint8List.sublistView(bytes, 0, offset);
  var background = 0;
  if (globalSize > bytes[11] * 3) {
    final color = 13 + bytes[11] * 3;
    background =
        bytes[color] << 24 |
        bytes[color + 1] << 16 |
        bytes[color + 2] << 8 |
        255;
  }
  var loopCount = 1;
  var duration = 0;
  var dispose = 0;
  var transparent = -1;
  final frames = <_RasterFrame>[];
  int skipBlocks(int start) {
    while (true) {
      if (start >= bytes.length) _malformed();
      final length = bytes[start++];
      if (length == 0) return start;
      start += length;
      if (start > bytes.length) _malformed();
    }
  }

  var ended = false;
  while (offset < bytes.length) {
    final marker = bytes[offset++];
    if (marker == 59) {
      ended = true;
      break;
    }
    if (marker == 33) {
      final extension = bytes[offset++];
      if (extension == 249) {
        if (bytes[offset++] != 4 || offset + 5 > bytes.length) _malformed();
        final flags = bytes[offset++];
        dispose = flags >> 2 & 7;
        if (dispose > 3 || flags & 224 != 0) _malformed();
        duration = _u16(bytes, offset, Endian.little) * 10;
        offset += 2;
        transparent = flags & 1 == 0 ? -1 : bytes[offset];
        offset++;
        if (bytes[offset++] != 0) _malformed();
      } else {
        if (extension == 1) {
          _fail(
            LitchcraftMediaErrorCode.unsupported,
            'GIF text overlays are not supported. Export the animation as pixels first.',
          );
        }
        if (extension == 255 &&
            offset + 17 <= bytes.length &&
            bytes[offset] == 11) {
          final name = ascii.decode(
            Uint8List.sublistView(bytes, offset + 1, offset + 12),
          );
          if ((name == 'NETSCAPE2.0' || name == 'ANIMEXTS1.0') &&
              bytes[offset + 12] == 3 &&
              bytes[offset + 13] == 1) {
            final repeats = _u16(bytes, offset + 14, Endian.little);
            loopCount = repeats == 0 ? 0 : repeats + 1;
          }
        }
        offset = skipBlocks(offset);
      }
      continue;
    }
    if (marker != 44 || offset + 9 > bytes.length) _malformed();
    final start = offset - 1;
    final x = _u16(bytes, offset, Endian.little);
    final y = _u16(bytes, offset + 2, Endian.little);
    final fw = _u16(bytes, offset + 4, Endian.little);
    final fh = _u16(bytes, offset + 6, Endian.little);
    final flags = bytes[offset + 8];
    if (flags & 24 != 0) _malformed();
    offset += 9;
    final colorCount = flags & 128 != 0
        ? 1 << ((flags & 7) + 1)
        : globalSize ~/ 3;
    if (colorCount == 0 || transparent >= colorCount) _malformed();
    if (flags & 128 != 0) offset += 3 * colorCount;
    if (offset >= bytes.length || bytes[offset] < 2 || bytes[offset] > 8) {
      _malformed();
    }
    offset = skipBlocks(offset + 1);
    final frame = _RasterFrame(
      width: fw,
      height: fh,
      x: x,
      y: y,
      durationMs: duration,
      dispose: dispose,
      over: true,
      transparent: transparent,
      gifPayload: Uint8List.sublistView(bytes, start, offset),
    );
    _frameBounds(frame, width, height);
    if (width * height * 4 +
            fw * fh +
            frame.gifPayload!.length * 4 +
            1024 * 1024 >
        litchcraftMediaMaximumWorkingBytes) {
      _fail(
        LitchcraftMediaErrorCode.dimensionsTooLarge,
        'Resize this image before importing.',
      );
    }
    frames.add(frame);
    if (frames.length > _maximumSourceFrames) {
      _fail(
        LitchcraftMediaErrorCode.frameLimit,
        'Trim this source to 4,096 frames or fewer before importing.',
      );
    }
    duration = 0;
    dispose = 0;
    transparent = -1;
  }
  if (!ended || frames.isEmpty || offset != bytes.length) _malformed();
  return _MediaSource(
    LitchcraftMediaMetadata(
      format: LitchcraftMediaFormat.gif,
      width: width,
      height: height,
      durationsMs: frames.map((frame) => frame.durationMs),
      loopCount: loopCount,
    ),
    frames,
    gifHeader: header,
    gifBackground: background,
  );
}

_MediaSource _inspectPng(Uint8List bytes) {
  final crc = _Crc();
  Uint8List? header;
  Uint8List? palette;
  Uint8List? transparency;
  var width = 0,
      height = 0,
      offset = 8,
      declaredFrames = 0,
      loopCount = 1,
      sequence = 0;
  var sawData = false, ended = false;
  var idatEnded = false, firstUsesIdat = false;
  var chunkCount = 0;
  _RasterFrame? current;
  final frames = <_RasterFrame>[];
  while (offset + 12 <= bytes.length) {
    if (++chunkCount > 16384) {
      _fail(
        LitchcraftMediaErrorCode.dimensionsTooLarge,
        'This PNG has too many data chunks to import safely. Re-export it before importing.',
      );
    }
    final length = _u32(bytes, offset);
    if (length > bytes.length - offset - 12) _malformed();
    final type = ascii.decode(
      Uint8List.sublistView(bytes, offset + 4, offset + 8),
    );
    if (!type.codeUnits.every(
          (value) => value >= 65 && value <= 90 || value >= 97 && value <= 122,
        ) ||
        type.codeUnitAt(2) & 32 != 0) {
      _malformed();
    }
    final payload = Uint8List.sublistView(
      bytes,
      offset + 8,
      offset + 8 + length,
    );
    if (crc.of(Uint8List.sublistView(bytes, offset + 4, offset + 8 + length)) !=
        _u32(bytes, offset + 8 + length)) {
      _malformed();
    }
    if (header == null && type != 'IHDR') _malformed();
    if (sawData && type != 'IDAT') idatEnded = true;
    switch (type) {
      case 'IHDR':
        if (header != null || length != 13) _malformed();
        header = payload;
        width = _u32(payload, 0);
        height = _u32(payload, 4);
        _dimensions(width, height);
        if (!const [1, 2, 4, 8, 16].contains(payload[8]) ||
            !const [0, 2, 3, 4, 6].contains(payload[9]) ||
            payload[10] != 0 ||
            payload[11] != 0 ||
            payload[12] > 1) {
          _malformed();
        }
        if ((payload[9] != 0 && payload[9] != 3 && payload[8] < 8) ||
            (payload[9] == 3 && payload[8] == 16)) {
          _malformed();
        }
      case 'PLTE':
        if (sawData ||
            palette != null ||
            length < 3 ||
            length > 768 ||
            length % 3 != 0 ||
            header![9] == 0 ||
            header[9] == 4 ||
            (header[9] == 3 && length ~/ 3 > 1 << header[8])) {
          _malformed();
        }
        palette = payload;
      case 'tRNS':
        if (sawData || transparency != null || length > 256) _malformed();
        if (header![9] == 0 && length != 2 ||
            header[9] == 2 && length != 6 ||
            header[9] == 3 &&
                (palette == null ||
                    length < 1 ||
                    length > palette.length ~/ 3) ||
            header[9] == 4 ||
            header[9] == 6) {
          _malformed();
        }
        transparency = payload;
      case 'acTL':
        if (sawData || declaredFrames != 0 || length != 8) _malformed();
        declaredFrames = _u32(payload, 0);
        loopCount = _u32(payload, 4);
        if (declaredFrames < 1) _malformed();
        if (declaredFrames > _maximumSourceFrames) {
          _fail(
            LitchcraftMediaErrorCode.frameLimit,
            'Trim this source to 4,096 frames or fewer before importing.',
          );
        }
      case 'fcTL':
        if (declaredFrames == 0 ||
            length != 26 ||
            _u32(payload, 0) != sequence++ ||
            payload[24] > 2 ||
            payload[25] > 1) {
          _malformed();
        }
        if (current != null && current.compressed.isEmpty) _malformed();
        current = _RasterFrame(
          width: _u32(payload, 4),
          height: _u32(payload, 8),
          x: _u32(payload, 12),
          y: _u32(payload, 16),
          durationMs:
              (1000 *
                      _u16(payload, 20) /
                      (_u16(payload, 22) == 0 ? 100 : _u16(payload, 22)))
                  .round(),
          dispose: payload[24],
          over: payload[25] == 1,
        );
        _frameBounds(current, width, height);
        if (!sawData &&
            (current.x != 0 ||
                current.y != 0 ||
                current.width != width ||
                current.height != height)) {
          _malformed();
        }
        if (frames.isEmpty) firstUsesIdat = !sawData;
        frames.add(current);
        if (frames.length > declaredFrames) _malformed();
      case 'IDAT':
        if (idatEnded) _malformed();
        sawData = true;
        if (declaredFrames == 0) {
          current ??= _RasterFrame(width: width, height: height);
          if (frames.isEmpty) frames.add(current);
        }
        // A default PNG outside the animation is deliberately not imported.
        if (current != null) current.compressed.add(payload);
      case 'fdAT':
        if (!sawData ||
            declaredFrames == 0 ||
            current == null ||
            length < 4 ||
            (frames.length == 1 && firstUsesIdat) ||
            _u32(payload, 0) != sequence++) {
          _malformed();
        }
        current.compressed.add(Uint8List.sublistView(payload, 4));
      case 'IEND':
        if (length != 0) _malformed();
        ended = true;
      default:
        if (type.codeUnitAt(0) & 32 == 0) {
          _fail(
            LitchcraftMediaErrorCode.unsupported,
            'This PNG uses an unsupported image extension.',
          );
        }
      // Omit ancillary metadata from decoder input: compressed profiles and
      // text can otherwise expand before any raster memory admission check.
    }
    offset += length + 12;
    if (ended) break;
  }
  if (!ended ||
      offset != bytes.length ||
      header == null ||
      frames.isEmpty ||
      (declaredFrames != 0 && frames.length != declaredFrames) ||
      frames.any((frame) => frame.compressed.isEmpty)) {
    _malformed();
  }
  if (header[9] == 3 && palette == null) _malformed();
  final channels = switch (header[9]) {
    0 || 3 => 1,
    2 => 3,
    4 => 2,
    _ => 4,
  };
  for (final frame in frames) {
    final scan = _scanlineBytes(
      frame.width,
      frame.height,
      channels,
      header[8],
      header[12] == 1,
    );
    final working =
        width * height * 4 +
        frame.width * frame.height * channels * (header[8] == 16 ? 2 : 1) +
        scan * 3 +
        frame.compressedLength * 4 +
        2 * 1024 * 1024;
    if (working > litchcraftMediaMaximumWorkingBytes) {
      _fail(
        LitchcraftMediaErrorCode.dimensionsTooLarge,
        'This image needs too much decoding memory. Resize it before importing.',
      );
    }
  }
  return _MediaSource(
    LitchcraftMediaMetadata(
      format: LitchcraftMediaFormat.apng,
      width: width,
      height: height,
      durationsMs: frames.map((frame) => frame.durationMs),
      loopCount: loopCount,
    ),
    frames,
    pngHeader: header,
    pngPalette: palette,
    pngTransparency: transparency,
  );
}

int _scanlineBytes(
  int width,
  int height,
  int channels,
  int bits,
  bool interlaced,
) {
  int pass(int x, int y, int sx, int sy) {
    final w = math.max(0, (width - x + sx - 1) ~/ sx);
    final h = math.max(0, (height - y + sy - 1) ~/ sy);
    return w == 0 ? 0 : h * (1 + (w * channels * bits + 7) ~/ 8);
  }

  if (!interlaced) return pass(0, 0, 1, 1);
  return pass(0, 0, 8, 8) +
      pass(4, 0, 8, 8) +
      pass(0, 4, 4, 8) +
      pass(2, 0, 4, 4) +
      pass(0, 2, 2, 4) +
      pass(1, 0, 2, 2) +
      pass(0, 1, 1, 2);
}

class _InflationCheck implements Sink<List<int>> {
  _InflationCheck(this.maximum);
  final int maximum;
  int length = 0;
  @override
  void add(List<int> data) {
    length += data.length;
    if (length > maximum) _malformed();
  }

  @override
  void close() {}
}

class _GifCodes {
  _GifCodes(this.bytes, this.offset);
  final Uint8List bytes;
  int offset;
  int remaining = 0, bits = 0, value = 0;
  bool ended = false;

  int? code(int width) {
    while (bits < width) {
      if (ended) return null;
      if (remaining == 0) {
        if (offset >= bytes.length) _malformed();
        remaining = bytes[offset++];
        if (remaining == 0) {
          ended = true;
          return null;
        }
        if (offset + remaining > bytes.length) _malformed();
      }
      value |= bytes[offset++] << bits;
      remaining--;
      bits += 8;
    }
    final result = value & ((1 << width) - 1);
    value >>= width;
    bits -= width;
    return result;
  }
}

/// The pinned decoder returns a partial Image on early EOI or invalid codes.
/// Validate expansion lengths and palette indices without a third pixel raster
/// before trusting its pixels. The LZW dictionary has exactly 4,096 entries.
void _validateGifPixels(_MediaSource source, _RasterFrame frame) {
  final payload = frame.gifPayload!;
  final local = payload[9] & 128 != 0;
  final colorCount = local
      ? 1 << ((payload[9] & 7) + 1)
      : (source.gifHeader!.length - 13) ~/ 3;
  final minimumOffset = 10 + (local ? colorCount * 3 : 0);
  final minimum = payload[minimumOffset];
  final clear = 1 << minimum, end = clear + 1;
  final lengths = Uint16List(4096);
  final first = Uint8List(4096), maximum = Uint8List(4096);
  for (var index = 0; index < clear; index++) {
    lengths[index] = 1;
    first[index] = index;
    maximum[index] = index;
  }
  final codes = _GifCodes(payload, minimumOffset + 1);
  var next = end + 1, width = minimum + 1, previous = -1, pixels = 0;
  final expected = frame.width * frame.height;
  while (true) {
    final code = codes.code(width);
    if (code == null) _malformed();
    if (code == clear) {
      next = end + 1;
      width = minimum + 1;
      previous = -1;
      continue;
    }
    if (code == end) {
      if (pixels != expected) _malformed();
      return;
    }
    int length, leading, highest;
    if (code < next) {
      length = lengths[code];
      leading = first[code];
      highest = maximum[code];
      if (length == 0 || previous < 0 && code >= clear) _malformed();
    } else if (code == next && previous >= 0 && next < 4096) {
      length = lengths[previous] + 1;
      leading = first[previous];
      highest = maximum[previous];
    } else {
      _malformed();
    }
    pixels += length;
    if (pixels > expected || highest >= colorCount) _malformed();
    if (previous >= 0 && next < 4096) {
      lengths[next] = lengths[previous] + 1;
      first[next] = first[previous];
      maximum[next] = math.max(maximum[previous], leading);
      next++;
      if (next == 1 << width && width < 12) width++;
    }
    previous = code;
  }
}

image.Image _decodePatch(_MediaSource source, _RasterFrame frame) {
  if (source.metadata.format == LitchcraftMediaFormat.gif) {
    _validateGifPixels(source, frame);
    final header = Uint8List.fromList(source.gifHeader!);
    ByteData.sublistView(header).setUint16(6, frame.width, Endian.little);
    ByteData.sublistView(header).setUint16(8, frame.height, Endian.little);
    final payload = Uint8List.fromList(frame.gifPayload!);
    payload.fillRange(1, 5, 0);
    final bytes = BytesBuilder(copy: false)
      ..add(header)
      ..add([
        33,
        249,
        4,
        frame.transparent >= 0 ? 1 : 0,
        0,
        0,
        math.max(0, frame.transparent),
        0,
      ])
      ..add(payload)
      ..add([59]);
    final result = image.GifDecoder().decode(bytes.takeBytes(), frame: 0);
    if (result == null ||
        result.width != frame.width ||
        result.height != frame.height) {
      _malformed();
    }
    return result;
  }
  final header = source.pngHeader!;
  final channels = switch (header[9]) {
    0 || 3 => 1,
    2 => 3,
    4 => 2,
    _ => 4,
  };
  final expected = _scanlineBytes(
    frame.width,
    frame.height,
    channels,
    header[8],
    header[12] == 1,
  );
  final check = _InflationCheck(expected);
  final inflater = ZLibDecoder().startChunkedConversion(check);
  for (final block in frame.compressed) {
    // Small compressed chunks bound native inflater output before our sink
    // rejects an expansion bomb. The validated stream is then decoded once.
    for (var offset = 0; offset < block.length; offset += 512) {
      inflater.add(
        Uint8List.sublistView(
          block,
          offset,
          math.min(offset + 512, block.length),
        ),
      );
    }
  }
  inflater.close();
  if (check.length != expected) _malformed();
  final crc = _Crc();
  final bytes = BytesBuilder(copy: false)
    ..add(_pngSignature)
    ..add(
      _chunk(
        'IHDR',
        _pngHeader(frame.width, frame.height, source: header),
        crc,
      ),
    );
  if (source.pngPalette != null) {
    bytes.add(_chunk('PLTE', source.pngPalette!, crc));
  }
  if (source.pngTransparency != null) {
    bytes.add(_chunk('tRNS', source.pngTransparency!, crc));
  }
  for (final compressed in frame.compressed) {
    bytes.add(_chunk('IDAT', compressed, crc));
  }
  bytes.add(_chunk('IEND', Uint8List(0), crc));
  final result = image.PngDecoder().decode(bytes.takeBytes(), frame: 0);
  if (result == null ||
      result.width != frame.width ||
      result.height != frame.height) {
    _malformed();
  }
  return result;
}

class _OutputSample {
  const _OutputSample(this.sourceFrame, this.durationMs);
  final int sourceFrame;
  final int durationMs;
}

List<_OutputSample> _importSamples(
  _MediaSource source,
  LitchcraftMediaImportSettings settings,
) {
  final last = settings.lastFrame ?? source.frames.length - 1;
  if (settings.firstFrame < 0 ||
      last < settings.firstFrame ||
      last >= source.frames.length ||
      !const [5, 10, 20].contains(settings.fps) ||
      !const [0, 0xffffff].contains(settings.matteColor)) {
    _fail(
      LitchcraftMediaErrorCode.invalidSettings,
      'Choose a valid frame range, speed and matte color.',
    );
  }
  final samples = <_OutputSample>[];
  if (settings.timing == LitchcraftMediaTiming.source) {
    if (last - settings.firstFrame + 1 > litchcraftAnimationMaximumFrameCount) {
      _fail(
        LitchcraftMediaErrorCode.frameLimit,
        'Artwork supports 256 frames. Trim the range or choose a slower frame rate.',
      );
    }
    for (var index = settings.firstFrame; index <= last; index++) {
      samples.add(
        _OutputSample(index, source.frames[index].durationMs.clamp(50, 5000)),
      );
    }
    return samples;
  }
  final step = 1000 ~/ settings.fps;
  final durations = <int>[
    for (var index = settings.firstFrame; index <= last; index++)
      math.max(50, source.frames[index].durationMs),
  ];
  final total = durations.fold<int>(0, (sum, duration) => sum + duration);
  final count = (total + step - 1) ~/ step;
  if (count > litchcraftAnimationMaximumFrameCount) {
    _fail(
      LitchcraftMediaErrorCode.frameLimit,
      'This conversion would exceed 256 frames. Trim the range or choose a slower frame rate.',
    );
  }
  var sourceIndex = 0;
  var sourceEnd = durations.first;
  for (var time = 0; time < total; time += step) {
    while (time >= sourceEnd) {
      sourceEnd += durations[++sourceIndex];
    }
    samples.add(
      _OutputSample(
        settings.firstFrame + sourceIndex,
        math.max(50, math.min(step, total - time)),
      ),
    );
  }
  return samples;
}

int _patchPixel(image.Image patch, int x, int y) {
  final pixel = patch.getPixel(x, y);
  return (pixel.rNormalized * 255).round() << 24 |
      (pixel.gNormalized * 255).round() << 16 |
      (pixel.bNormalized * 255).round() << 8 |
      (pixel.aNormalized * 255).round();
}

int _over(int source, int destination) {
  final sa = source & 255;
  if (sa == 0) return destination;
  if (sa == 255) return source;
  final da = destination & 255;
  final alphaNumerator = sa * 255 + da * (255 - sa);
  int channel(int shift) =>
      (((source >> shift & 255) * sa * 255 +
          (destination >> shift & 255) * da * (255 - sa) +
          alphaNumerator ~/ 2) ~/
      alphaNumerator);
  return channel(24) << 24 |
      channel(16) << 16 |
      channel(8) << 8 |
      (alphaNumerator + 127) ~/ 255;
}

int _matte(int rgba, int matte) {
  final alpha = rgba & 255;
  int channel(int shift) =>
      ((rgba >> (shift + 8) & 255) * alpha +
          (matte >> shift & 255) * (255 - alpha) +
          127) ~/
      255;
  return channel(16) << 16 | channel(8) << 8 | channel(0);
}

List<int> _sampleBoard(
  int Function(int x, int y) pixel,
  LitchcraftMediaMetadata metadata,
  LitchcraftMediaImportSettings settings,
) {
  final scale = settings.fit == LitchcraftMediaFit.fit
      ? math.min(1 / metadata.width, 1 / metadata.height)
      : math.max(1 / metadata.width, 1 / metadata.height);
  int point(double boardX, double boardY) {
    final x = (boardX - .5) / scale + metadata.width / 2 - .5;
    final y = (boardY - .5) / scale + metadata.height / 2 - .5;
    if (x < -.5 ||
        x >= metadata.width - .5 ||
        y < -.5 ||
        y >= metadata.height - .5) {
      return settings.matteColor;
    }
    int at(int px, int py) => _matte(
      pixel(px.clamp(0, metadata.width - 1), py.clamp(0, metadata.height - 1)),
      settings.matteColor,
    );
    if (settings.sampling == LitchcraftMediaSampling.crisp) {
      return at(x.round(), y.round());
    }
    final x0 = x.floor(), y0 = y.floor();
    final dx = x - x0, dy = y - y0;
    final colors = <int>[
      at(x0, y0),
      at(x0 + 1, y0),
      at(x0, y0 + 1),
      at(x0 + 1, y0 + 1),
    ];
    int channel(int shift) =>
        ((colors[0] >> shift & 255) * (1 - dx) * (1 - dy) +
                (colors[1] >> shift & 255) * dx * (1 - dy) +
                (colors[2] >> shift & 255) * (1 - dx) * dy +
                (colors[3] >> shift & 255) * dx * dy)
            .round();
    return channel(16) << 16 | channel(8) << 8 | channel(0);
  }

  final result = List<int>.filled(litchcraftAnimationEmitterCount, 0);
  for (var display = 0; display < 64; display++) {
    final square = litchcraftLogicalSquareForDisplay(
      display,
      flipped: settings.flipped,
    );
    for (var region = 0; region < 4; region++) {
      final semantic = litchcraftLogicalRegionForDisplay(
        region,
        flipped: settings.flipped,
      );
      final (x, y) = litchcraftEmitterSamplePoints[region];
      result[square * 4 + semantic] = point(
        (display % 8 + x) / 8,
        (display ~/ 8 + y) / 8,
      );
    }
  }
  return result;
}

LitchcraftMediaImportResult convertLitchcraftMediaSource(
  Uint8List bytes,
  LitchcraftMediaImportSettings settings,
  LitchcraftTopology topology,
  List<int> favoriteColors, {
  void Function(double)? onProgress,
}) {
  final source = _inspectMediaSource(bytes);
  final samples = _importSamples(source, settings);
  final width = source.metadata.width;
  final canvas = Uint32List(width * source.metadata.height);
  int gifBackground(_RasterFrame frame) =>
      source.gifHeader![10] & 128 == 0 ||
          frame.transparent == source.gifHeader![11]
      ? 0
      : source.gifBackground;
  // GIF's logical-screen background can itself be the transparent index.
  // APNG always begins with transparent black.
  if (source.gifHeader != null) {
    canvas.fillRange(0, canvas.length, gifBackground(source.frames.first));
  }
  final output = <LitchcraftAnimationFrame>[];
  var nextSample = 0;
  var adapted = settings.timing == LitchcraftMediaTiming.chosenFps;
  onProgress?.call(0);
  for (var index = 0; index <= samples.last.sourceFrame; index++) {
    final frame = source.frames[index];
    final patch = _decodePatch(source, frame);
    int composed(int x, int y) {
      final base = canvas[y * width + x];
      if (x < frame.x ||
          y < frame.y ||
          x >= frame.x + frame.width ||
          y >= frame.y + frame.height) {
        return base;
      }
      final rgba = _patchPixel(patch, x - frame.x, y - frame.y);
      return frame.over ? _over(rgba, base) : rgba;
    }

    // A previous-disposal patch is sampled over the untouched base. Keeping
    // that composition virtual avoids a third restore-to-previous raster.
    final previous = source.gifHeader != null
        ? frame.dispose == 3
        : frame.dispose == 2;
    if (!previous) {
      for (var y = frame.y; y < frame.y + frame.height; y++) {
        for (var x = frame.x; x < frame.x + frame.width; x++) {
          canvas[y * width + x] = composed(x, y);
        }
      }
    }
    while (nextSample < samples.length &&
        samples[nextSample].sourceFrame == index) {
      final sample = samples[nextSample++];
      final colors = _sampleBoard(
        previous ? composed : (x, y) => canvas[y * width + x],
        source.metadata,
        settings,
      );
      output.add(
        LitchcraftAnimationFrame(
          durationMs: sample.durationMs,
          emitters: colors.map(
            (color) => LitchcraftAnimatedEmitter(color: color),
          ),
        ),
      );
      adapted |= sample.durationMs != frame.durationMs;
    }
    final background = source.gifHeader != null
        ? frame.dispose == 2
        : frame.dispose == 1;
    if (background) {
      final color = source.gifHeader != null ? gifBackground(frame) : 0;
      for (var y = frame.y; y < frame.y + frame.height; y++) {
        canvas.fillRange(
          y * width + frame.x,
          y * width + frame.x + frame.width,
          color,
        );
      }
    }
    onProgress?.call((index + 1) / (samples.last.sourceFrame + 1));
  }
  return LitchcraftMediaImportResult(
    document: LitchcraftAnimationDocument(
      frames: output,
      topology: topology,
      favoriteColors: favoriteColors,
    ),
    metadata: source.metadata,
    timingAdapted: adapted,
  );
}
