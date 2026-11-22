import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as image;
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_media.dart';
import 'package:litch_mobile/src/services/litchcraft_media_codec.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';

LitchcraftAnimationDocument _document({
  LitchcraftEffectStyle effect = LitchcraftEffectStyle.off,
  int effectDuration = 0,
}) => LitchcraftAnimationDocument(
  frames: [
    LitchcraftAnimationFrame(
      emitters: List.generate(
        256,
        (_) => LitchcraftAnimatedEmitter(
          color: 0xff0000,
          effect: effect,
          effectDurationMs: effectDuration,
        ),
      ),
    ),
  ],
  topology: LitchcraftTopology.v3,
  favoriteColors: litchcraftDefaultFavoriteColors,
);

Future<Uint8List> _uniformRenderer(
  LitchcraftCanvas canvas,
  int size,
  bool flipped,
) async {
  final rgba = Uint8List(size * size * 4);
  final color = canvas.colors.first;
  for (var index = 0; index < rgba.length; index += 4) {
    rgba[index] = color >> 16;
    rgba[index + 1] = color >> 8;
    rgba[index + 2] = color;
    rgba[index + 3] = 255;
  }
  return rgba;
}

Matcher _error(LitchcraftMediaErrorCode code) =>
    isA<LitchcraftMediaException>().having((error) => error.code, 'code', code);

Uint8List _chunk(String type, List<int> payload) {
  final bytes = Uint8List(payload.length + 12);
  final data = ByteData.sublistView(bytes)..setUint32(0, payload.length);
  bytes.setRange(4, 8, type.codeUnits);
  bytes.setRange(8, bytes.length - 4, payload);
  data.setUint32(
    bytes.length - 4,
    litchcraftAnimationCrc32(Uint8List.sublistView(bytes, 4, bytes.length - 4)),
  );
  return bytes;
}

image.Image _rgba(int width, int height, int rgb, {int alpha = 255}) {
  final raster = image.Image(width: width, height: height, numChannels: 4);
  image.fill(
    raster,
    color: image.ColorRgba8(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, alpha),
  );
  return raster;
}

class _PngFrame {
  const _PngFrame(
    this.raster, {
    this.x = 0,
    this.duration = 100,
    this.denominator = 1000,
    this.dispose = 0,
    this.over = false,
  });
  final image.Image raster;
  final int x, duration, denominator, dispose;
  final int y = 0;
  final bool over;
}

List<Uint8List> _idat(image.Image raster) {
  final png = image.encodePng(raster);
  final blocks = <Uint8List>[];
  for (var offset = 8; offset < png.length;) {
    final length = ByteData.sublistView(png).getUint32(offset);
    if (ascii.decode(png.sublist(offset + 4, offset + 8)) == 'IDAT') {
      blocks.add(Uint8List.sublistView(png, offset + 8, offset + 8 + length));
    }
    offset += length + 12;
  }
  return blocks;
}

Uint8List _apng(
  List<_PngFrame> frames, {
  int plays = 0,
  image.Image? defaultImage,
}) {
  final first = frames.first.raster;
  final header = ByteData(13)
    ..setUint32(0, first.width)
    ..setUint32(4, first.height)
    ..setUint8(8, 8)
    ..setUint8(9, 6);
  final animation = ByteData(8)
    ..setUint32(0, frames.length)
    ..setUint32(4, plays);
  final output = BytesBuilder(copy: false)
    ..add([137, 80, 78, 71, 13, 10, 26, 10])
    ..add(_chunk('IHDR', header.buffer.asUint8List()))
    ..add(_chunk('acTL', animation.buffer.asUint8List()));
  if (defaultImage != null) {
    for (final block in _idat(defaultImage)) {
      output.add(_chunk('IDAT', block));
    }
  }
  var sequence = 0;
  for (var index = 0; index < frames.length; index++) {
    final frame = frames[index];
    final control = ByteData(26)
      ..setUint32(0, sequence++)
      ..setUint32(4, frame.raster.width)
      ..setUint32(8, frame.raster.height)
      ..setUint32(12, frame.x)
      ..setUint32(16, frame.y)
      ..setUint16(20, frame.duration)
      ..setUint16(22, frame.denominator)
      ..setUint8(24, frame.dispose)
      ..setUint8(25, frame.over ? 1 : 0);
    output.add(_chunk('fcTL', control.buffer.asUint8List()));
    for (final block in _idat(frame.raster)) {
      if (index == 0 && defaultImage == null) {
        output.add(_chunk('IDAT', block));
      } else {
        output.add(
          _chunk('fdAT', [
            ...(ByteData(4)..setUint32(0, sequence++)).buffer.asUint8List(),
            ...block,
          ]),
        );
      }
    }
  }
  output.add(_chunk('IEND', []));
  return output.takeBytes();
}

image.Image _indexed(int width, int height, int index) {
  final palette = image.PaletteUint8(256, 3)
    ..setRgb(0, 0, 0, 0)
    ..setRgb(1, 255, 0, 0)
    ..setRgb(2, 0, 0, 255)
    ..setRgb(3, 0, 255, 0);
  final raster = image.Image(
    width: width,
    height: height,
    numChannels: 1,
    palette: palette,
  );
  final data = raster.data!.toUint8List();
  data.fillRange(0, data.length, index);
  return raster;
}

Uint8List _gifImage(image.Image raster) {
  final gif = image.GifEncoder().encode(raster);
  var offset = 13 + (gif[10] & 128 == 0 ? 0 : 3 * (1 << ((gif[10] & 7) + 1)));
  while (gif[offset] == 33) {
    offset += 2;
    while (gif[offset] != 0) {
      offset += gif[offset] + 1;
    }
    offset++;
  }
  expect(gif[offset], 44);
  return Uint8List.fromList(gif.sublist(offset, gif.length - 1));
}

class _GifFrame {
  const _GifFrame(
    this.raster, {
    this.duration = 100,
    this.dispose = 1,
    this.transparent = -1,
  });
  final image.Image raster;
  final int duration, dispose, transparent;
  final int x = 0, y = 0;
}

Uint8List _gif(List<_GifFrame> frames, {int? repeats, int? backgroundIndex}) {
  final width = frames.first.raster.width, height = frames.first.raster.height;
  final output = BytesBuilder(copy: false)
    ..add([
      ...ascii.encode('GIF89a'),
      width & 255,
      width >> 8,
      height & 255,
      height >> 8,
      backgroundIndex == null ? 0 : 135,
      backgroundIndex ?? 0,
      0,
    ]);
  if (backgroundIndex != null) {
    output.add(frames.first.raster.palette!.toUint8List());
  }
  if (repeats != null) {
    output.add([
      33,
      255,
      11,
      ...ascii.encode('NETSCAPE2.0'),
      3,
      1,
      repeats & 255,
      repeats >> 8,
      0,
    ]);
  }
  for (final frame in frames) {
    output.add([
      33,
      249,
      4,
      frame.dispose << 2 | (frame.transparent >= 0 ? 1 : 0),
      frame.duration ~/ 10 & 255,
      frame.duration ~/ 10 >> 8,
      frame.transparent >= 0 ? frame.transparent : 0,
      0,
    ]);
    final payload = _gifImage(frame.raster);
    final data = ByteData.sublistView(payload)
      ..setUint16(1, frame.x, Endian.little)
      ..setUint16(3, frame.y, Endian.little);
    output.add(data.buffer.asUint8List());
  }
  output.add([59]);
  return output.takeBytes();
}

void main() {
  test(
    'async encoded sink reconstructs real GIF and APNG with per-frame retention',
    () async {
      final source = _document(
        effect: LitchcraftEffectStyle.breathe,
        effectDuration: 500,
      );
      for (final format in LitchcraftMediaFormat.values) {
        final output = BytesBuilder(copy: false);
        var writes = 0;
        final result = await exportLitchcraftMedia(
          source,
          settings: LitchcraftMediaExportSettings(format: format, size: 64),
          renderRgba: _uniformRenderer,
          encodedSink: (chunk) async {
            writes++;
            output.add(chunk);
          },
        );
        final bytes = output.takeBytes();
        final metadata = inspectLitchcraftMedia(bytes);
        expect(result.streamed, isTrue);
        expect(result.bytes, isEmpty);
        expect(result.encodedByteLength, bytes.length);
        expect(result.peakBufferedByteLength, lessThan(bytes.length));
        expect(writes, greaterThan(metadata.frameCount));
        expect(metadata.frameCount, 10);
        expect(metadata.durationsMs, everyElement(50));
        expect(metadata.loopCount, 0);
        final decoded = format == LitchcraftMediaFormat.gif
            ? image.decodeGif(bytes)
            : image.decodePng(bytes);
        expect(decoded!.numFrames, 10);
        final buffered = await exportLitchcraftMedia(
          source,
          settings: LitchcraftMediaExportSettings(format: format, size: 64),
          renderRgba: _uniformRenderer,
        );
        expect(bytes, buffered.bytes);
      }
    },
  );

  test(
    'container drains stream history and enforces 32 MiB streamed and 6 MiB buffered limits',
    () {
      const settings = LitchcraftMediaExportSettings(durationMs: 10000);
      final plan = planLitchcraftMediaExport(_document(), settings);
      // This admission test uses opaque compressed blocks; the preceding test
      // independently reopens real formats through the complete service seam.
      final block = Uint8List(1024 * 1024);
      final streamed = LitchcraftMediaContainerEncoder(
        settings,
        plan,
        retainBytes: false,
      );
      for (var index = 0; index < 31; index++) {
        streamed.addFrame(block, index);
        expect(streamed.retainedByteLength, lessThan(2 * 1024 * 1024));
        streamed.takeChunks();
        expect(streamed.retainedByteLength, 0);
      }
      expect(streamed.peakBufferedByteLength, lessThan(2 * 1024 * 1024));
      expect(
        () => streamed.addFrame(block, 31),
        throwsA(_error(LitchcraftMediaErrorCode.outputTooLarge)),
      );
      expect(
        streamed.encodedByteLength,
        lessThanOrEqualTo(litchcraftMediaMaximumEncodedBytes),
      );
      final buffered = LitchcraftMediaContainerEncoder(settings, plan);
      for (var index = 0; index < 5; index++) {
        buffered.addFrame(block, index);
      }
      expect(
        () => buffered.addFrame(block, 5),
        throwsA(_error(LitchcraftMediaErrorCode.outputTooLarge)),
      );
      expect(
        buffered.encodedByteLength,
        lessThanOrEqualTo(litchcraftMediaMaximumBufferedBytes),
      );
    },
  );

  test(
    'cancelled or failed encoded sink stops writing and preserves snapshot',
    () async {
      final source = _document(
        effect: LitchcraftEffectStyle.breathe,
        effectDuration: 500,
      );
      final before = encodeLitchcraftAnimationDocument(source).bytes.toList();
      final token = LitchcraftMediaCancellation();
      var writes = 0, renders = 0;
      await expectLater(
        exportLitchcraftMedia(
          source,
          settings: const LitchcraftMediaExportSettings(size: 64),
          token: token,
          renderRgba: (canvas, size, flipped) {
            renders++;
            return _uniformRenderer(canvas, size, flipped);
          },
          encodedSink: (chunk) async {
            if (++writes == 2) token.cancel();
          },
        ),
        throwsA(_error(LitchcraftMediaErrorCode.cancelled)),
      );
      expect(writes, 2);
      expect(renders, 1);
      await expectLater(
        exportLitchcraftMedia(
          source,
          settings: const LitchcraftMediaExportSettings(size: 64),
          renderRgba: _uniformRenderer,
          encodedSink: (_) async => throw StateError('disk full'),
        ),
        throwsStateError,
      );
      expect(encodeLitchcraftAnimationDocument(source).bytes, before);
    },
  );

  test(
    'GIF odd complete period exports an exact timed once preview without zero delays',
    () async {
      final source = LitchcraftAnimationDocument(
        frames: [
          LitchcraftAnimationFrame(
            durationMs: 700,
            emitters: List.generate(
              256,
              (_) => const LitchcraftAnimatedEmitter(color: 0xff0000),
            ),
          ),
          LitchcraftAnimationFrame(
            durationMs: 701,
            emitters: List.generate(
              256,
              (_) => const LitchcraftAnimatedEmitter(color: 0x0000ff),
            ),
          ),
        ],
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
      );
      const settings = LitchcraftMediaExportSettings(
        format: LitchcraftMediaFormat.gif,
        size: 64,
        fps: 5,
      );
      final plan = planLitchcraftMediaExport(source, settings);
      expect(plan.isPreview, isTrue);
      expect(plan.durationMs, 10000);
      expect(plan.repeats, isFalse);
      expect(
        planLitchcraftMediaExport(
          source,
          const LitchcraftMediaExportSettings(size: 64, fps: 5),
        ).completePeriod,
        isTrue,
      );
      final result = await exportLitchcraftMedia(
        source,
        settings: settings,
        renderRgba: _uniformRenderer,
      );
      final reopened = inspectLitchcraftMedia(result.bytes);
      expect(reopened.durationMs, 10000);
      expect(reopened.durationsMs, everyElement(greaterThan(0)));
      expect(reopened.loopCount, 1);
      expect(image.decodeGif(result.bytes)!.numFrames, 50);
    },
  );

  test(
    'structurally terminated GIF with early LZW EOI is rejected before document adoption',
    () async {
      final bytes = Uint8List.fromList([
        ...ascii.encode('GIF89a'),
        8,
        0,
        8,
        0,
        128,
        0,
        0,
        0,
        0,
        0,
        255,
        255,
        255,
        44,
        0,
        0,
        0,
        0,
        8,
        0,
        8,
        0,
        0,
        2,
        1,
        44,
        0,
        59,
      ]);
      // This is the escaped production seam: the pinned codec returns a
      // plausible 8x8 image despite CLEAR + EOI producing zero of 64 pixels.
      expect(image.decodeGif(bytes)!.width, 8);
      expect(inspectLitchcraftMedia(bytes).frameCount, 1);
      final retainedDraft = _document();
      final before = encodeLitchcraftAnimationDocument(
        retainedDraft,
      ).bytes.toList();
      await expectLater(
        importLitchcraftMedia(bytes),
        throwsA(_error(LitchcraftMediaErrorCode.malformed)),
      );
      expect(encodeLitchcraftAnimationDocument(retainedDraft).bytes, before);
    },
  );

  test(
    'bounded GIF validation accepts dictionary width growth and clear resets',
    () async {
      final raster = _indexed(128, 128, 0);
      final pixels = raster.data!.toUint8List();
      var state = 1;
      for (var index = 0; index < pixels.length; index++) {
        state = (state * 1103515245 + 12345) & 0x7fffffff;
        pixels[index] = state >> 16 & 255;
      }
      final bytes = _gif([_GifFrame(raster)]);
      expect(image.decodeGif(bytes)!.width, 128);
      final result = await importLitchcraftMedia(bytes);
      expect(result.document.frames.length, 1);
      expect(result.preview.colors.length, 256);
    },
  );

  test(
    'automatic duration uses global effect periods and treats sparkle as preview',
    () {
      expect(
        planLitchcraftMediaExport(
          _document(),
          const LitchcraftMediaExportSettings(),
        ).frameCount,
        1,
      );
      final breathe = planLitchcraftMediaExport(
        _document(effect: LitchcraftEffectStyle.breathe, effectDuration: 2000),
        const LitchcraftMediaExportSettings(),
      );
      expect(breathe.durationMs, 2000);
      expect(breathe.completePeriod, isTrue);
      expect(breathe.repeats, isTrue);
      final sparkle = planLitchcraftMediaExport(
        _document(effect: LitchcraftEffectStyle.sparkle, effectDuration: 500),
        const LitchcraftMediaExportSettings(),
      );
      expect(sparkle.durationMs, 10000);
      expect(sparkle.frameCount, 200);
      expect(sparkle.isPreview, isTrue);
      expect(sparkle.repeats, isFalse);
      expect(
        planLitchcraftMediaExport(
          _document(effect: LitchcraftEffectStyle.sparkle, effectDuration: 500),
          const LitchcraftMediaExportSettings(
            durationMs: 500,
            loop: LitchcraftMediaLoop.repeat,
          ),
        ).repeats,
        isTrue,
      );
    },
  );

  test(
    'APNG export reopens actual canonical effect samples without changing document',
    () async {
      final source = _document(
        effect: LitchcraftEffectStyle.breathe,
        effectDuration: 500,
      );
      final before = encodeLitchcraftAnimationDocument(source).bytes.toList();
      final result = await exportLitchcraftMedia(
        source,
        settings: const LitchcraftMediaExportSettings(size: 64),
        renderRgba: _uniformRenderer,
      );
      final reopened = image.decodePng(result.bytes)!;
      expect(reopened.numFrames, 10);
      expect(reopened.width, 64);
      expect(inspectLitchcraftMedia(result.bytes).loopCount, 0);
      for (var index = 0; index < reopened.numFrames; index++) {
        final expected = composeLitchcraftAnimationDocument(
          source,
          index * 50,
        ).colors.first;
        expect(
          reopened.getFrame(index).getPixel(0, 0).r.toInt(),
          expected >> 16,
        );
        expect(reopened.getFrame(index).frameDuration, 50);
      }
      expect(encodeLitchcraftAnimationDocument(source).bytes, before);
    },
  );

  test(
    'GIF export has exact centisecond delay and once omits repeat extension',
    () async {
      final result = await exportLitchcraftMedia(
        _document(),
        settings: const LitchcraftMediaExportSettings(
          format: LitchcraftMediaFormat.gif,
          size: 64,
          fps: 5,
          durationMs: 400,
          loop: LitchcraftMediaLoop.once,
        ),
        renderRgba: _uniformRenderer,
      );
      final reopened = image.decodeGif(result.bytes)!;
      expect(reopened.numFrames, 2);
      expect(reopened.getFrame(0).frameDuration, 200);
      expect(inspectLitchcraftMedia(result.bytes).loopCount, 1);
      expect(latin1.decode(result.bytes), isNot(contains('NETSCAPE2.0')));
      expect(reopened.getFrame(1).getPixel(10, 10).r.toInt(), 255);
      final repeat = await exportLitchcraftMedia(
        _document(),
        settings: const LitchcraftMediaExportSettings(
          format: LitchcraftMediaFormat.gif,
          size: 64,
          durationMs: 100,
          loop: LitchcraftMediaLoop.repeat,
        ),
        renderRgba: _uniformRenderer,
      );
      expect(inspectLitchcraftMedia(repeat.bytes).loopCount, 0);
    },
  );

  testWidgets('production CanvasPainter feeds APNG and preserves orientation', (
    tester,
  ) async {
    final colors = List<int>.filled(256, 0xff0000);
    colors.fillRange(252, 256, 0x0000ff);
    final source = LitchcraftAnimationDocument(
      frames: [
        LitchcraftAnimationFrame(
          emitters: colors.map(
            (color) => LitchcraftAnimatedEmitter(color: color),
          ),
        ),
      ],
      topology: LitchcraftTopology.v3,
      favoriteColors: litchcraftDefaultFavoriteColors,
    );
    final result = await tester.runAsync(
      () => exportLitchcraftMedia(
        source,
        settings: const LitchcraftMediaExportSettings(size: 64, flipped: true),
        renderRgba: renderLitchcraftArtworkRgba,
      ),
    );
    final reopened = image.decodePng(result!.bytes)!;
    final pixel = reopened.getPixel(4, 4);
    expect(pixel.b, greaterThan(pixel.r));
    final imported = await tester.runAsync(
      () => importLitchcraftMedia(
        result.bytes,
        settings: const LitchcraftMediaImportSettings(
          flipped: true,
          sampling: LitchcraftMediaSampling.crisp,
        ),
      ),
    );
    expect(
      imported!.document.frames.single.emitters.last.color & 255,
      greaterThan(imported.document.frames.single.emitters.last.color >> 16),
    );
  });

  test(
    'APNG partial blend and previous disposal restore actual prior canvas',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(16, 16, 0xff0000), duration: 100),
        _PngFrame(
          _rgba(8, 16, 0x0000ff, alpha: 128),
          duration: 200,
          dispose: 2,
          over: true,
        ),
        _PngFrame(_rgba(16, 8, 0x00ff00), duration: 300),
      ], plays: 3);
      final result = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          sampling: LitchcraftMediaSampling.crisp,
        ),
      );
      expect(result.metadata.loopCount, 3);
      expect(result.document.frames.map((frame) => frame.durationMs), [
        100,
        200,
        300,
      ]);
      expect(result.document.frames[1].emitters.first.color, 0x7f0080);
      expect(result.document.frames[2].emitters.first.color, 0x00ff00);
      expect(result.document.frames[2].emitters[56 * 4].color, 0xff0000);
      expect(result.timingAdapted, isFalse);
    },
  );

  test(
    'GIF local palettes and previous disposal preserve background outside patch',
    () async {
      final bytes = _gif([
        _GifFrame(_indexed(16, 16, 1), duration: 50),
        _GifFrame(_indexed(8, 16, 2), duration: 100, dispose: 3),
        _GifFrame(_indexed(16, 8, 3), duration: 150),
      ], repeats: 1);
      final result = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          sampling: LitchcraftMediaSampling.crisp,
        ),
      );
      expect(result.metadata.loopCount, 2);
      expect(result.document.frames[1].emitters.first.color, 0x0000ff);
      expect(result.document.frames[1].emitters.last.color, 0xff0000);
      expect(result.document.frames[2].emitters[56 * 4].color, 0xff0000);
      expect(result.document.frames.map((frame) => frame.durationMs), [
        50,
        100,
        150,
      ]);
    },
  );

  test(
    'background disposal clears just the patch and transparency respects matte',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(16, 16, 0xff0000)),
        _PngFrame(_rgba(8, 16, 0x0000ff), dispose: 1),
        _PngFrame(_rgba(16, 8, 0x00ff00)),
      ]);
      final result = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          matteColor: 0xffffff,
          sampling: LitchcraftMediaSampling.crisp,
        ),
      );
      expect(result.document.frames[2].emitters[56 * 4].color, 0xffffff);
      expect(result.document.frames[2].emitters.last.color, 0xff0000);
      final transparent = image.encodePng(_rgba(8, 8, 0xff0000, alpha: 128));
      final white = await importLitchcraftMedia(
        transparent,
        settings: const LitchcraftMediaImportSettings(matteColor: 0xffffff),
      );
      final black = await importLitchcraftMedia(transparent);
      expect(white.preview.colors.first, 0xff7f7f);
      expect(black.preview.colors.first, 0x800000);
    },
  );

  test(
    'GIF background disposal honors global background and transparent palette indices',
    () async {
      final bytes = _gif([
        _GifFrame(_indexed(16, 16, 1)),
        _GifFrame(_indexed(8, 16, 2), dispose: 2),
        _GifFrame(_indexed(16, 8, 1)),
      ], backgroundIndex: 3);
      final result = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          sampling: LitchcraftMediaSampling.crisp,
        ),
      );
      expect(result.document.frames[2].emitters[56 * 4].color, 0x00ff00);
      expect(result.document.frames[2].emitters.last.color, 0xff0000);
      final transparent = _gif([
        _GifFrame(_indexed(8, 8, 0), transparent: 0),
      ], backgroundIndex: 0);
      final white = await importLitchcraftMedia(
        transparent,
        settings: const LitchcraftMediaImportSettings(matteColor: 0xffffff),
      );
      expect(white.preview.colors, everyElement(0xffffff));
    },
  );

  test(
    'excluded PNG default image is not adopted as an animation frame',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(8, 8, 0xff0000)),
      ], defaultImage: _rgba(8, 8, 0x0000ff));
      final result = await importLitchcraftMedia(bytes);
      expect(result.document.frames.length, 1);
      expect(result.preview.colors.first, 0xff0000);
    },
  );

  test(
    'range preserves prior composition and chosen fps samples source time',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(8, 8, 0xff0000), duration: 100),
        _PngFrame(_rgba(8, 8, 0x0000ff), duration: 200),
      ]);
      final source = await importLitchcraftMedia(bytes);
      expect(source.document.frames.map((frame) => frame.durationMs), [
        100,
        200,
      ]);
      final resampled = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          timing: LitchcraftMediaTiming.chosenFps,
        ),
      );
      expect(resampled.document.frames.length, 6);
      expect(
        resampled.document.frames.map((frame) => frame.emitters.first.color),
        [0xff0000, 0xff0000, 0x0000ff, 0x0000ff, 0x0000ff, 0x0000ff],
      );
      expect(resampled.timingAdapted, isTrue);
      final trimmed = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          firstFrame: 1,
          lastFrame: 1,
        ),
      );
      expect(trimmed.document.frames.length, 1);
      expect(trimmed.preview.colors.first, 0x0000ff);
    },
  );

  test(
    'fit and crop are explicit and keep exactly 64 four-emitter squares',
    () async {
      final png = image.encodePng(_rgba(16, 8, 0xff0000));
      final fit = await importLitchcraftMedia(png);
      final crop = await importLitchcraftMedia(
        png,
        settings: const LitchcraftMediaImportSettings(
          fit: LitchcraftMediaFit.crop,
        ),
      );
      expect(fit.preview.colors.first, 0);
      expect(crop.preview.colors, everyElement(0xff0000));
      expect(crop.preview.colors.length, 256);
    },
  );

  test(
    '256 frame limit rejects rather than truncating and can be explicitly trimmed',
    () async {
      final bytes = _apng(
        List.generate(
          257,
          (_) => _PngFrame(_rgba(1, 1, 0xff0000), duration: 50),
        ),
      );
      expect(inspectLitchcraftMedia(bytes).frameCount, 257);
      await expectLater(
        importLitchcraftMedia(bytes),
        throwsA(_error(LitchcraftMediaErrorCode.frameLimit)),
      );
      final trimmed = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(lastFrame: 255),
      );
      expect(trimmed.document.frames.length, 256);
      final resampled = await importLitchcraftMedia(
        bytes,
        settings: const LitchcraftMediaImportSettings(
          timing: LitchcraftMediaTiming.chosenFps,
          fps: 5,
        ),
      );
      expect(resampled.document.frames.length, 65);
    },
  );

  test(
    'source delays outside document bounds are adapted with explicit metadata',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(1, 1, 0xff0000), duration: 0),
        _PngFrame(_rgba(1, 1, 0x0000ff), duration: 6000),
      ]);
      final result = await importLitchcraftMedia(bytes);
      expect(result.metadata.durationsMs, [0, 6000]);
      expect(result.document.frames.map((frame) => frame.durationMs), [
        50,
        5000,
      ]);
      expect(result.timingAdapted, isTrue);
    },
  );

  test(
    'APNG zero timing denominator uses the specified hundredths fallback',
    () async {
      final bytes = _apng([
        _PngFrame(_rgba(1, 1, 0xff0000), duration: 10, denominator: 0),
      ]);
      final result = await importLitchcraftMedia(bytes);
      expect(result.metadata.durationsMs, [100]);
      expect(result.document.frames.single.durationMs, 100);
      expect(result.timingAdapted, isFalse);
    },
  );

  test('editable old and advanced envelopes reopen through real decoder', () {
    final now = DateTime.utc(2026, 10, 9);
    final old = LitchcraftArtwork(
      id: 'old',
      name: 'Old',
      canvas: LitchcraftCanvas.blank(),
      favoriteColors: litchcraftDefaultFavoriteColors,
      createdAt: now,
      modifiedAt: now,
    );
    expect(
      decodeLitchcraftEditableArtwork(
        encodeLitchcraftEditableArtwork(old),
      ).canvas,
      old.canvas,
    );
    final advanced = LitchcraftArtwork.animated(
      id: 'effect',
      name: 'Effect',
      document: _document(
        effect: LitchcraftEffectStyle.rainbow,
        effectDuration: 1000,
      ),
      createdAt: now,
      modifiedAt: now,
    );
    final reopened = decodeLitchcraftEditableArtwork(
      encodeLitchcraftEditableArtwork(advanced),
    );
    expect(
      reopened.document.frames.first.emitters.first.effect,
      LitchcraftEffectStyle.rainbow,
    );
    expect(reopened.toJson(), advanced.toJson());
    expect(
      () =>
          decodeLitchcraftEditableArtwork(Uint8List.fromList(utf8.encode('{'))),
      throwsA(_error(LitchcraftMediaErrorCode.malformed)),
    );
  });

  test(
    'preflight rejects encoded size, dimensions, CRC, bounds and inflation bombs',
    () async {
      expect(
        () => inspectLitchcraftMedia(
          Uint8List(litchcraftMediaMaximumEncodedBytes + 1),
        ),
        throwsA(_error(LitchcraftMediaErrorCode.inputTooLarge)),
      );
      final hugeHeader = ByteData(13)
        ..setUint32(0, 100000)
        ..setUint32(4, 100000)
        ..setUint8(8, 8)
        ..setUint8(9, 6);
      final huge = Uint8List.fromList([
        137,
        80,
        78,
        71,
        13,
        10,
        26,
        10,
        ..._chunk('IHDR', hugeHeader.buffer.asUint8List()),
        ..._chunk('IEND', []),
      ]);
      expect(
        () => inspectLitchcraftMedia(huge),
        throwsA(_error(LitchcraftMediaErrorCode.dimensionsTooLarge)),
      );
      final corrupt = image.encodePng(_rgba(1, 1, 0xff0000));
      corrupt[corrupt.length - 1] ^= 1;
      expect(
        () => inspectLitchcraftMedia(corrupt),
        throwsA(_error(LitchcraftMediaErrorCode.malformed)),
      );
      final outOfBounds = _apng([
        _PngFrame(_rgba(8, 8, 0xff0000)),
        _PngFrame(_rgba(8, 8, 0x0000ff), x: 1),
      ]);
      expect(
        () => inspectLitchcraftMedia(outOfBounds),
        throwsA(_error(LitchcraftMediaErrorCode.malformed)),
      );
      final header = ByteData(13)
        ..setUint32(0, 1)
        ..setUint32(4, 1)
        ..setUint8(8, 8)
        ..setUint8(9, 6);
      final bomb = Uint8List.fromList([
        137,
        80,
        78,
        71,
        13,
        10,
        26,
        10,
        ..._chunk('IHDR', header.buffer.asUint8List()),
        ..._chunk('IDAT', ZLibEncoder().convert(Uint8List(65536))),
        ..._chunk('IEND', []),
      ]);
      await expectLater(
        importLitchcraftMedia(bomb),
        throwsA(_error(LitchcraftMediaErrorCode.malformed)),
      );
    },
  );

  test(
    'cancelled renderer and worker return no result and preserve source',
    () async {
      final token = LitchcraftMediaCancellation();
      final source = _document(
        effect: LitchcraftEffectStyle.breathe,
        effectDuration: 500,
      );
      final original = encodeLitchcraftAnimationDocument(source).bytes.toList();
      var renders = 0;
      await expectLater(
        exportLitchcraftMedia(
          source,
          settings: const LitchcraftMediaExportSettings(size: 64),
          token: token,
          renderRgba: (canvas, size, flipped) async {
            renders++;
            token.cancel();
            return _uniformRenderer(canvas, size, flipped);
          },
        ),
        throwsA(_error(LitchcraftMediaErrorCode.cancelled)),
      );
      expect(renders, 1);
      expect(encodeLitchcraftAnimationDocument(source).bytes, original);
      final importToken = LitchcraftMediaCancellation();
      final bytes = _apng(
        List.generate(20, (_) => _PngFrame(_rgba(16, 16, 0xff0000))),
      );
      await expectLater(
        importLitchcraftMedia(
          bytes,
          token: importToken,
          onProgress: (progress) {
            if (progress > 0) importToken.cancel();
          },
        ),
        throwsA(_error(LitchcraftMediaErrorCode.cancelled)),
      );
    },
  );
}
