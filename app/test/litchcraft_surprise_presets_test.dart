import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as image;
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/litchcraft_surprise_presets.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/widgets/litchcraft_physical_canvas.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart'
    show renderLitchcraftArtworkPng, renderLitchcraftArtworkRgba;

void main() {
  testWidgets(
    'black artwork previews and shared images distinguish Off from dim color',
    (tester) async {
      final blank = LitchcraftCanvas.blank();
      final png = (await tester.runAsync(
        () => renderLitchcraftArtworkPng(blank, size: 64),
      ))!;
      final raster = image.decodePng(png)!;
      expect(
        raster.every(
          (pixel) =>
              pixel.r == 0 && pixel.g == 0 && pixel.b == 0 && pixel.a == 255,
        ),
        isTrue,
      );
      final rgba = (await tester.runAsync(
        () => renderLitchcraftArtworkRgba(blank, 64, false),
      ))!;
      for (var offset = 0; offset < rgba.length; offset += 4) {
        expect(rgba.sublist(offset, offset + 4), [0, 0, 0, 255]);
      }
      final dim = blank.paint([7], 15, 0x0f0101);
      final before = List<int>.of(dim.colors);
      final dimRgba = (await tester.runAsync(
        () => renderLitchcraftArtworkRgba(dim, 64, false),
      ))!;
      expect(
        dimRgba.asMap().entries.any(
          (entry) => entry.key % 4 != 3 && entry.value != 0,
        ),
        isTrue,
      );
      expect(
        dim.colors,
        before,
        reason: 'Presentation never clamps low RGB in stored user artwork',
      );
      final key = GlobalKey();
      await tester.pumpWidget(
        Directionality(
          textDirection: TextDirection.ltr,
          child: Center(
            child: RepaintBoundary(
              key: key,
              child: SizedBox.square(
                dimension: 64,
                child: LitchcraftPhysicalCanvas(canvas: blank),
              ),
            ),
          ),
        ),
      );
      final boundary =
          key.currentContext!.findRenderObject()! as RenderRepaintBoundary;
      final pixels = (await tester.runAsync(() async {
        final frame = await boundary.toImage();
        final bytes = (await frame.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        ))!;
        final result = bytes.buffer.asUint8List(
          bytes.offsetInBytes,
          bytes.lengthInBytes,
        );
        frame.dispose();
        return result;
      }))!;
      for (var offset = 0; offset < pixels.length; offset += 4) {
        expect(pixels.sublist(offset, offset + 4), [0, 0, 0, 255]);
      }
      await tester.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'six surprises retain every emitter and exact boundary through codecs',
    (tester) async {
      final renderDirectory = Platform.environment['LITCH_SURPRISE_RENDER_DIR'];
      final key = GlobalKey();
      for (final preset in LitchcraftSurprisePreset.values) {
        final document = LitchcraftAnimationDocument(
          frames: preset.createFrames(),
          topology: LitchcraftTopology.v3,
          favoriteColors: litchcraftDefaultFavoriteColors,
          sparkleSeed: preset.sparkleSeed,
        );
        final artwork = LitchcraftArtwork.animated(
          id: 'surprise-${preset.taps}',
          name: 'Artwork',
          document: document,
          createdAt: DateTime.utc(2026),
          modifiedAt: DateTime.utc(2026),
        );
        final restored = LitchcraftArtwork.fromJson(artwork.toJson());
        expect(sameLitchcraftDocument(document, restored.document), isTrue);
        expect(
          sameLitchcraftDocument(
            document,
            LitchcraftAnimationDocument(
              frames: preset.createFrames(),
              topology: document.topology,
              favoriteColors: document.favoriteColors,
              sparkleSeed: preset.sparkleSeed,
            ),
          ),
          isTrue,
          reason: 'Artwork generation must be deterministic',
        );
        expect(document.frames, hasLength(preset.frameCount));
        final lit = <int>{};
        var offCount = 0;
        var boundary = 0;
        for (var index = 0; index < document.frames.length; index++) {
          final frame = document.frames[index];
          for (final emitter in frame.emitters) {
            if (emitter.color == 0) {
              offCount++;
              expect(emitter.effect, LitchcraftEffectStyle.off);
              expect(emitter.effectDurationMs, 0);
            } else {
              expect(
                [
                  emitter.color >> 16 & 255,
                  emitter.color >> 8 & 255,
                  emitter.color & 255,
                ].reduce((a, b) => a > b ? a : b),
                greaterThanOrEqualTo(40),
              );
            }
          }
          expect(litchcraftAnimationFrameIndexAt(document, boundary), index);
          expect(
            litchcraftAnimationFrameIndexAt(
              document,
              boundary + frame.durationMs - 1,
            ),
            index,
          );
          for (final delta in [
            0,
            frame.durationMs ~/ 2,
            frame.durationMs - 1,
          ]) {
            final colors = composeLitchcraftAnimationDocument(
              document,
              boundary + delta,
            ).colors;
            for (var emitter = 0; emitter < 256; emitter++) {
              if (colors[emitter] != 0) lit.add(emitter);
            }
          }
          boundary += frame.durationMs;
        }
        expect(lit, hasLength(256));
        expect(
          offCount,
          greaterThan(0),
          reason: 'Each scene needs true black negative space',
        );
        expect(boundary, inInclusiveRange(3000, 15000));
        expect(litchcraftAnimationFrameIndexAt(document, boundary), 0);
        final transfer = LitchcraftFrameStoreProtocol.prepare(
          session: 1,
          transfer: 1,
          expectedGeneration: 0,
          enableRgbWire: true,
          topology: 3,
          active: true,
          playbackOnEntry: false,
          sparkleSeed: document.sparkleSeed,
          favoriteColors: document.favoriteColors,
          frames: document.frames,
        );
        debugPrint(
          'SURPRISE ${preset.taps} ${preset.name}: ${preset.frameCount} frames, ${boundary}ms, wire=${transfer.bodyBytes}B',
        );
        final renderTaps = Platform.environment['LITCH_SURPRISE_RENDER_TAPS'];
        if (renderDirectory != null &&
            (renderTaps == null ||
                renderTaps.split(',').contains('${preset.taps}'))) {
          final gif = image.GifEncoder(repeat: 0);
          // Include every authored cut, with effect samples at most 50 ms apart.
          // A 70 ms frame is held for 50+20 ms rather than rounded to 100 ms.
          var elapsed = 0;
          while (elapsed < boundary) {
            final composed = composeLitchcraftAnimationDocument(
              document,
              elapsed,
            );
            final colors = composed.colors;
            final frameEnd =
                litchcraftAnimationFrameStartOrNull(
                  document,
                  composed.frameIndex,
                )! +
                document.frames[composed.frameIndex].durationMs;
            final sampleDuration = (frameEnd - elapsed).clamp(0, 50);
            await tester.pumpWidget(
              Directionality(
                textDirection: TextDirection.ltr,
                child: Center(
                  child: RepaintBoundary(
                    key: key,
                    child: SizedBox.square(
                      dimension: 320,
                      child: ColoredBox(
                        color: Colors.black,
                        child: LitchcraftPhysicalCanvas(
                          canvas: LitchcraftCanvas(
                            topology: LitchcraftTopology.v3,
                            colors: colors,
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            );
            final boundaryWidget =
                key.currentContext!.findRenderObject()!
                    as RenderRepaintBoundary;
            await tester.runAsync(() async {
              final pixels = await boundaryWidget.toImage();
              final bytes = (await pixels.toByteData(
                format: ui.ImageByteFormat.rawRgba,
              ))!;
              gif.addFrame(
                image.Image.fromBytes(
                  width: 320,
                  height: 320,
                  bytes: bytes.buffer,
                  bytesOffset: bytes.offsetInBytes,
                  numChannels: 4,
                ),
                duration: sampleDuration ~/ 10,
              );
              pixels.dispose();
            });
            elapsed += sampleDuration;
          }
          await tester.runAsync(() async {
            final dir = Directory(renderDirectory);
            await dir.create(recursive: true);
            await File(
              '${dir.path}/${Platform.environment['LITCH_SURPRISE_RENDER_PREFIX'] ?? ''}${preset.taps}-${preset.name.toLowerCase().replaceAll(' ', '-')}.gif',
            ).writeAsBytes(gif.finish()!);
          });
        }
      }
      await tester.pumpWidget(const SizedBox());
    },
  );
}
