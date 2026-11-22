import 'dart:collection';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';

int _hex(String value) => int.parse(value, radix: 16);

class _LengthOnlyList extends ListBase<int> {
  _LengthOnlyList(this._length);

  int _length;
  int reads = 0;

  @override
  int get length => _length;

  @override
  set length(int value) => _length = value;

  @override
  int operator [](int index) {
    reads++;
    throw StateError('Oversized input must be rejected before indexing');
  }

  @override
  void operator []=(int index, int value) {
    throw UnsupportedError('read-only probe');
  }
}

LitchcraftAnimationDocument _canonicalDocument() {
  final emitters = List<LitchcraftAnimatedEmitter>.filled(
    256,
    const LitchcraftAnimatedEmitter(color: 0),
  );
  emitters[0] = const LitchcraftAnimatedEmitter(
    color: 0x112233,
    effect: LitchcraftEffectStyle.breathe,
    effectDurationMs: 500,
  );
  emitters[1] = const LitchcraftAnimatedEmitter(
    color: 0,
    effect: LitchcraftEffectStyle.sparkle,
    effectDurationMs: 10000,
  );
  emitters[255] = const LitchcraftAnimatedEmitter(
    color: 0xabcdef,
    effect: LitchcraftEffectStyle.rainbow,
    effectDurationMs: 2000,
  );
  return LitchcraftAnimationDocument(
    frames: [LitchcraftAnimationFrame(emitters: emitters, durationMs: 500)],
    topology: LitchcraftTopology.v3,
    favoriteColors: litchcraftDefaultFavoriteColors,
    playbackOnEntry: true,
    sparkleSeed: 0x78563412,
  );
}

void main() {
  test('shared golden vectors define every deterministic animation style', () {
    final lines = File(
      '../test/fixtures/litchcraft_animation_vectors.csv',
    ).readAsLinesSync();
    var vectorCount = 0;
    for (final line in lines) {
      if (line.isEmpty || line.startsWith('#')) continue;
      final fields = line.split(',');
      expect(fields, hasLength(16), reason: line);
      final effect = LitchcraftEffectStyle.fromId(int.parse(fields[1]));
      final duration = int.parse(fields[2]);
      final mask = int.parse(fields[7]);
      final emitters = List<LitchcraftAnimatedEmitter>.generate(4, (index) {
        final selected = mask & (1 << index) != 0;
        return LitchcraftAnimatedEmitter(
          color: _hex(fields[8 + index]),
          effect: selected ? effect : LitchcraftEffectStyle.off,
          effectDurationMs: selected ? duration : 0,
        );
      });
      final output = composeLitchcraftAnimatedSquare(
        emitters: emitters,
        elapsedMs: int.parse(fields[3]),
        sparkleSeed: _hex(fields[4]),
        frameIndex: int.parse(fields[5]),
        emitterBaseIndex: int.parse(fields[6]),
      );
      expect(output, [
        for (var index = 0; index < 4; index++) _hex(fields[12 + index]),
      ], reason: fields[0]);
      vectorCount++;
    }
    expect(vectorCount, 23);
  });

  test(
    'schema-3 document enforces frame, emitter, and encoded size bounds',
    () {
      final emitter = const LitchcraftAnimatedEmitter(color: 0x123456);
      final frame = LitchcraftAnimationFrame(
        emitters: List.filled(256, emitter),
      );
      final maximum = LitchcraftAnimationDocument(
        frames: List.filled(8, frame),
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        sparkleSeed: 0xffffffff,
      );
      expect(maximum.encodedByteLength, litchcraftAnimationMaximumEncodedBytes);
      expect(
        encodeLitchcraftAnimationDocument(maximum).bytes,
        hasLength(litchcraftAnimationMaximumEncodedBytes),
      );
      expect(
        () => LitchcraftAnimationDocument(
          frames: const [],
          topology: LitchcraftTopology.v3,
          favoriteColors: litchcraftDefaultFavoriteColors,
        ),
        throwsFormatException,
      );
      expect(
        () => LitchcraftAnimationDocument(
          frames: List.filled(257, frame),
          topology: LitchcraftTopology.v3,
          favoriteColors: litchcraftDefaultFavoriteColors,
        ),
        throwsFormatException,
      );
      expect(
        () => LitchcraftAnimationFrame(emitters: List.filled(255, emitter)),
        throwsFormatException,
      );
      expect(
        () => LitchcraftAnimationFrame(
          emitters: List.filled(256, emitter),
          durationMs: 49,
        ),
        throwsFormatException,
      );
      expect(
        () => LitchcraftAnimationFrame(
          emitters: List.filled(256, emitter),
          durationMs: 5001,
        ),
        throwsFormatException,
      );
      expect(
        const LitchcraftAnimatedEmitter(
          color: 0x123456,
          effect: LitchcraftEffectStyle.flash,
          effectDurationMs: 501,
        ).isValid,
        isFalse,
      );
    },
  );

  test(
    'frame durations retain every exact millisecond from 50 through 5000',
    () {
      const emitter = LitchcraftAnimatedEmitter(color: 0x123456);
      final document = LitchcraftAnimationDocument(
        frames: [
          for (final duration in const [50, 137, 5000])
            LitchcraftAnimationFrame(
              emitters: List.filled(256, emitter),
              durationMs: duration,
            ),
        ],
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
      );

      final encoded = encodeLitchcraftAnimationDocument(document);
      final decoded = decodeLitchcraftAnimationDocument(
        encoded.bytes,
        encoded.crc32,
      );

      expect(decoded.frames.map((frame) => frame.durationMs), [50, 137, 5000]);
      expect(encoded.bytes[32] | encoded.bytes[33] << 8, 50);
      expect(encoded.bytes[1570] | encoded.bytes[1571] << 8, 137);
      expect(encoded.bytes[3108] | encoded.bytes[3109] << 8, 5000);
    },
  );

  test('group membership depends on both style and duration', () {
    const red = 0xff0000;
    final output = composeLitchcraftAnimatedSquare(
      emitters: const [
        LitchcraftAnimatedEmitter(
          color: red,
          effect: LitchcraftEffectStyle.orbitClockwise,
          effectDurationMs: 2000,
        ),
        LitchcraftAnimatedEmitter(
          color: red,
          effect: LitchcraftEffectStyle.orbitClockwise,
          effectDurationMs: 1000,
        ),
        LitchcraftAnimatedEmitter(
          color: red,
          effect: LitchcraftEffectStyle.orbitClockwise,
          effectDurationMs: 2000,
        ),
        LitchcraftAnimatedEmitter(color: 0),
      ],
      elapsedMs: 1000,
      sparkleSeed: 1,
      frameIndex: 0,
      emitterBaseIndex: 0,
    );
    expect(output, [0, red, red, 0]);
  });

  test('all eight effects respect one- through four-emitter masks', () {
    for (final effect in LitchcraftEffectStyle.values.skip(1)) {
      for (var memberCount = 1; memberCount <= 4; memberCount++) {
        final emitters = List<LitchcraftAnimatedEmitter>.generate(
          4,
          (index) => LitchcraftAnimatedEmitter(
            color: index < memberCount ? 0x804020 : 0x07090b,
            effect: index < memberCount ? effect : LitchcraftEffectStyle.off,
            effectDurationMs: index < memberCount ? 2000 : 0,
          ),
        );
        for (final elapsed in [0, 499, 500, 999, 1000, 1500, 1999, 2000]) {
          final output = composeLitchcraftAnimatedSquare(
            emitters: emitters,
            elapsedMs: elapsed,
            sparkleSeed: 0x12345678,
            frameIndex: 3,
            emitterBaseIndex: 40,
          );
          var litMembers = 0;
          for (var index = 0; index < output.length; index++) {
            final color = output[index];
            expect((color >> 16) & 0xff, lessThanOrEqualTo(128));
            expect((color >> 8) & 0xff, lessThanOrEqualTo(128));
            expect(color & 0xff, lessThanOrEqualTo(128));
            if (index >= memberCount) {
              expect(color, emitters[index].color);
            } else if (color != 0) {
              litMembers++;
            }
          }
          if (effect.id >= 5 && effect.id <= 7) {
            expect(litMembers, 1);
          }
        }
      }
    }
  });

  test(
    'full-document timeline hard-cuts, wraps, seeks, pauses, and resumes',
    () {
      LitchcraftAnimationFrame frame(int red, int duration) {
        final emitters = List<LitchcraftAnimatedEmitter>.filled(
          256,
          const LitchcraftAnimatedEmitter(color: 0),
        );
        emitters[0] = LitchcraftAnimatedEmitter(color: red << 16);
        return LitchcraftAnimationFrame(
          emitters: emitters,
          durationMs: duration,
        );
      }

      final document = LitchcraftAnimationDocument(
        frames: [frame(1, 100), frame(2, 200), frame(3, 300)],
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
      );
      expect(
        [
          for (var index = 0; index < 3; index++)
            litchcraftAnimationFrameStartOrNull(document, index),
        ],
        [0, 100, 300],
      );
      const elapsed = [0, 99, 100, 299, 300, 599, 600, 700, 900];
      const expected = [0, 0, 1, 1, 2, 2, 0, 1, 2];
      for (var index = 0; index < elapsed.length; index++) {
        expect(
          litchcraftAnimationFrameIndexAt(document, elapsed[index]),
          expected[index],
        );
        final composed = composeLitchcraftAnimationDocument(
          document,
          elapsed[index],
        );
        expect(composed.frameIndex, expected[index]);
        expect(composed.colors[0] >> 16, expected[index] + 1);
      }

      expect(litchcraftAnimationFrameStartOrNull(document, 3), isNull);
      var cursor = LitchcraftPlaybackCursor();
      expect(cursor.sample(50), 0);
      cursor = cursor.resume(100);
      expect(cursor.sample(175), 75);
      final repeatedResume = cursor.resume(175);
      expect(identical(repeatedResume, cursor), isTrue);
      cursor = repeatedResume;
      expect(cursor.sample(200), 100);
      cursor = cursor.pause(200);
      expect(cursor.sample(900), 100);
      cursor = cursor.seek(
        litchcraftAnimationFrameStartOrNull(document, 2)!,
        900,
      );
      expect(cursor.sample(999), 300);
      cursor = cursor.resume(1000);
      expect(cursor.sample(1050), 350);
      cursor = cursor.selectFrame(document, 1, 1060)!;
      expect(cursor.isPlaying, isFalse);
      expect(cursor.sample(9999), 100);
      expect(cursor.selectFrame(document, 3, 10000), isNull);
      expect(cursor.sample(10001), 100);

      final denseEmitter = const LitchcraftAnimatedEmitter(
        color: 0x804020,
        effect: LitchcraftEffectStyle.bounce,
        effectDurationMs: 2000,
      );
      final denseDocument = LitchcraftAnimationDocument(
        frames: [
          LitchcraftAnimationFrame(emitters: List.filled(256, denseEmitter)),
        ],
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
      );
      final dense = composeLitchcraftAnimationDocument(denseDocument, 777);
      expect(dense.emittersEvaluated, 256);
      expect(dense.groupCandidatesVisited, 1024);
      final reusedOutput = List<int>.filled(256, 0);
      final reusedStats = LitchcraftAnimationRenderStats();
      for (var sample = 0; sample < 1000; sample++) {
        expect(
          composeLitchcraftAnimationDocumentInto(
            denseDocument,
            sample,
            reusedOutput,
            stats: reusedStats,
          ),
          0,
        );
      }
      expect(reusedOutput, contains(isNot(0)));
      expect(
        reusedOutput,
        composeLitchcraftAnimationDocument(denseDocument, 999).colors,
      );
      expect(reusedStats.emittersEvaluated, 256);
      expect(reusedStats.groupCandidatesVisited, 1024);
    },
  );

  test('schema-3 codec matches shared exact bytes and existing CRC32', () {
    final encoded = encodeLitchcraftAnimationDocument(_canonicalDocument());
    final fixture = File(
      '../test/fixtures/litchcraft_animation_codec_bytes.csv',
    ).readAsLinesSync();
    late final List<int> expected;
    late final int expectedCrc;
    for (final line in fixture) {
      if (line.isEmpty || line.startsWith('#')) continue;
      final fields = line.split(',');
      if (fields[0] == 'length') {
        expected = List<int>.filled(int.parse(fields[1]), 0);
      } else if (fields[0] == 'crc32') {
        expectedCrc = _hex(fields[1]);
      } else {
        final offset = int.parse(fields[0]);
        final hex = fields[1];
        for (var index = 0; index < hex.length; index += 2) {
          expected[offset + index ~/ 2] = _hex(hex.substring(index, index + 2));
        }
      }
    }
    expect(encoded.bytes, expected);
    expect(encoded.crc32, expectedCrc);
    expect(litchcraftAnimationCrc32('123456789'.codeUnits), 0xcbf43926);
    final decoded = decodeLitchcraftAnimationDocument(
      encoded.bytes,
      encoded.crc32,
    );
    expect(decoded.sparkleSeed, 0x78563412);
    expect(
      decoded.frames.single.emitters.last.effect,
      LitchcraftEffectStyle.rainbow,
    );

    final damaged = encoded.bytes.toList()..[34] ^= 1;
    expect(
      () => decodeLitchcraftAnimationDocument(damaged, encoded.crc32),
      throwsFormatException,
    );
    final invalidEffect = encoded.bytes.toList()..[37] = 9;
    expect(
      () => decodeLitchcraftAnimationDocument(
        invalidEffect,
        litchcraftAnimationCrc32(invalidEffect),
      ),
      throwsFormatException,
    );
    final reservedFlag = encoded.bytes.toList()..[3] = 2;
    expect(
      () => decodeLitchcraftAnimationDocument(
        reservedFlag,
        litchcraftAnimationCrc32(reservedFlag),
      ),
      throwsFormatException,
    );
    final zeroSeed = encoded.bytes.toList()
      ..[4] = 0
      ..[5] = 0
      ..[6] = 0
      ..[7] = 0;
    final duplicateFavorite = encoded.bytes.toList()
      ..setRange(11, 14, encoded.bytes.sublist(8, 11));
    final retainedFavorites = decodeLitchcraftAnimationDocument(
      duplicateFavorite,
      litchcraftAnimationCrc32(duplicateFavorite),
    );
    expect(
      retainedFavorites.favoriteColors[1],
      retainedFavorites.favoriteColors[0],
    );
    expect(
      encodeLitchcraftAnimationDocument(retainedFavorites).bytes,
      duplicateFavorite,
    );
    final badFrameDuration = encoded.bytes.toList()
      ..[32] = 49
      ..[33] = 0;
    final badEffectDuration = encoded.bytes.toList()
      ..[38] = 0xf5
      ..[39] = 0x01;
    for (final semanticallyInvalid in [
      zeroSeed,
      badFrameDuration,
      badEffectDuration,
    ]) {
      expect(
        () => decodeLitchcraftAnimationDocument(
          semanticallyInvalid,
          litchcraftAnimationCrc32(semanticallyInvalid),
        ),
        throwsFormatException,
      );
    }
    final oversized = _LengthOnlyList(
      litchcraftAnimationMaximumEncodedBytes + 1,
    );
    expect(
      () => decodeLitchcraftAnimationDocument(oversized, 0),
      throwsFormatException,
    );
    expect(oversized.reads, 0);
    for (final impossibleFrameCount in [9, 255]) {
      final header = List<int>.filled(32, 0)
        ..[0] = litchcraftAnimationSchema
        ..[1] = impossibleFrameCount
        ..[2] = 3
        ..[4] = 1;
      expect(
        () => decodeLitchcraftAnimationDocument(header, 0),
        throwsFormatException,
      );
    }
    expect(
      () => decodeLitchcraftAnimationDocument(
        encoded.bytes.sublist(0, encoded.bytes.length - 1),
        encoded.crc32,
      ),
      throwsFormatException,
    );
  });

  test('independent effect edge oracles settle rounding and wrap behavior', () {
    int sample(
      int color,
      LitchcraftEffectStyle effect,
      int duration,
      int elapsed,
    ) {
      return composeLitchcraftAnimatedSquare(
        emitters: [
          LitchcraftAnimatedEmitter(
            color: color,
            effect: effect,
            effectDurationMs: duration,
          ),
          ...List.filled(3, const LitchcraftAnimatedEmitter(color: 0)),
        ],
        elapsedMs: elapsed,
        sparkleSeed: 1,
        frameIndex: 0,
        emitterBaseIndex: 0,
      )[0];
    }

    expect(sample(0xffff00, LitchcraftEffectStyle.rainbow, 2000, 0), 0xffff00);
    expect(sample(0x00ffff, LitchcraftEffectStyle.rainbow, 2000, 0), 0x00ffff);
    expect(sample(0xff00ff, LitchcraftEffectStyle.rainbow, 2000, 0), 0xff00ff);
    expect(sample(0x010203, LitchcraftEffectStyle.breathe, 500, 0), 0x000101);
    expect(
      sample(0x112233, LitchcraftEffectStyle.flash, 500, 0),
      sample(0x112233, LitchcraftEffectStyle.flash, 500, 500),
    );
    expect(
      sample(0x112233, LitchcraftEffectStyle.fadeToBlack, 10000, 0),
      sample(0x112233, LitchcraftEffectStyle.fadeToBlack, 10000, 10000),
    );

    List<int> group(LitchcraftEffectStyle effect, int elapsed) =>
        composeLitchcraftAnimatedSquare(
          emitters: [
            for (var index = 1; index <= 4; index++)
              LitchcraftAnimatedEmitter(
                color: index << 16,
                effect: effect,
                effectDurationMs: effect == LitchcraftEffectStyle.bounce
                    ? 600
                    : 2000,
              ),
          ],
          elapsedMs: elapsed,
          sparkleSeed: 1,
          frameIndex: 0,
          emitterBaseIndex: 0,
        );
    const bounceTimes = [0, 101, 201, 301, 401, 501, 600];
    const bouncePath = [0, 1, 2, 3, 2, 1, 0];
    for (var step = 0; step < bounceTimes.length; step++) {
      final output = group(LitchcraftEffectStyle.bounce, bounceTimes[step]);
      expect(
        [for (final color in output) color != 0],
        [for (var index = 0; index < 4; index++) index == bouncePath[step]],
      );
    }
    expect(group(LitchcraftEffectStyle.orbitClockwise, 0)[0], isNot(0));
    expect(group(LitchcraftEffectStyle.orbitCounterclockwise, 0)[3], isNot(0));
    expect(
      sample(0x804020, LitchcraftEffectStyle.sparkle, 2000, 500),
      sample(
        0x804020,
        LitchcraftEffectStyle.sparkle,
        2000,
        0x100000000 * 2000 + 500,
      ),
    );
    const signed64Max = 0x7fffffffffffffff;
    expect(
      sample(0x123456, LitchcraftEffectStyle.off, 0, signed64Max),
      0x123456,
    );
    expect(
      sample(0x123456, LitchcraftEffectStyle.flash, 500, signed64Max),
      sample(0x123456, LitchcraftEffectStyle.flash, 500, signed64Max % 500),
    );
  });
}
