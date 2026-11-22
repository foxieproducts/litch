import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_bouncing_balls_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_wire_codec.dart';

LitchcraftAnimationFrame baseFrame({int durationMs = 1000}) {
  return LitchcraftAnimationFrame(
    durationMs: durationMs,
    emitters: List<LitchcraftAnimatedEmitter>.generate(
      256,
      (index) => LitchcraftAnimatedEmitter(
        color:
            ((index & 0xff) << 16) |
            (((index * 3) & 0xff) << 8) |
            ((index * 7) & 0xff),
      ),
    ),
  );
}

void main() {
  test(
    'opt-in RGB raw anchor has exact bytes and preserves an all-off frame',
    () {
      final frame = baseFrame();
      final encoded = encodeLitchcraftWireFrame(
        previous: null,
        frame: frame,
        enableRgbOnly: true,
      );
      final expected = Uint8List.fromList(<int>[
        4,
        0xe8,
        0x03,
        for (final emitter in frame.emitters) ...<int>[
          (emitter.color >> 16) & 0xff,
          (emitter.color >> 8) & 0xff,
          emitter.color & 0xff,
        ],
      ]);
      expect(encoded.length, 771);
      expect(encoded, expected);
      expect(litchcraftAnimationCrc32(encoded), 0x9da85fb6);
      expect(encoded.sublist(0, 9), <int>[4, 0xe8, 0x03, 0, 0, 0, 1, 3, 7]);
      expect(
        decodeLitchcraftWireFrame(
          previous: null,
          bytes: encoded,
          enableRgbOnly: true,
        ).emitters.map((emitter) => emitter.color),
        frame.emitters.map((emitter) => emitter.color),
      );
      expect(
        () => decodeLitchcraftWireFrame(previous: null, bytes: encoded),
        throwsFormatException,
      );
      for (final damaged in <Uint8List>[
        Uint8List.fromList(encoded.sublist(0, encoded.length - 1)),
        Uint8List.fromList(<int>[...encoded, 0]),
      ]) {
        expect(
          () => decodeLitchcraftWireFrame(
            previous: null,
            bytes: damaged,
            enableRgbOnly: true,
          ),
          throwsFormatException,
        );
      }
    },
  );

  test('opt-in RGB bitmap has exact bytes and rejects damaged records', () {
    final previousEmitters = baseFrame().emitters.toList();
    previousEmitters[0] = const LitchcraftAnimatedEmitter(
      color: 0,
      effect: LitchcraftEffectStyle.breathe,
      effectDurationMs: 500,
    );
    final previous = LitchcraftAnimationFrame(emitters: previousEmitters);
    final emitters = previous.emitters.toList();
    for (var index = 0; index < 16; index++) {
      emitters[index] = LitchcraftAnimatedEmitter(color: 0x120000 | index);
    }
    final frame = LitchcraftAnimationFrame(emitters: emitters);
    final encoded = encodeLitchcraftWireFrame(
      previous: previous,
      frame: frame,
      enableRgbOnly: true,
    );
    final expected = Uint8List.fromList(<int>[
      5,
      0xe8,
      0x03,
      0xff,
      0xff,
      ...List<int>.filled(30, 0),
      for (var index = 0; index < 16; index++) ...<int>[0x12, 0, index],
    ]);
    expect(encoded, expected);
    final decoded = decodeLitchcraftWireFrame(
      previous: previous,
      bytes: encoded,
      enableRgbOnly: true,
    );
    expect(
      decoded.emitters.map((emitter) => emitter.color),
      frame.emitters.map((emitter) => emitter.color),
    );
    expect(
      decoded.emitters.every(
        (emitter) =>
            emitter.effect == LitchcraftEffectStyle.off &&
            emitter.effectDurationMs == 0,
      ),
      isTrue,
    );
    final omittedActiveEffect = Uint8List.fromList(<int>[
      5,
      0xe8,
      0x03,
      ...List<int>.filled(32, 0),
    ]);
    expect(
      () => decodeLitchcraftWireFrame(
        previous: previous,
        bytes: omittedActiveEffect,
        enableRgbOnly: true,
      ),
      throwsFormatException,
    );
    expect(
      () => decodeLitchcraftWireFrame(previous: previous, bytes: encoded),
      throwsFormatException,
    );
    final unknownTag = Uint8List.fromList(encoded)..[0] = 6;
    expect(
      () => decodeLitchcraftWireFrame(
        previous: previous,
        bytes: unknownTag,
        enableRgbOnly: true,
      ),
      throwsFormatException,
    );
    for (final damaged in <Uint8List>[
      Uint8List.fromList(encoded.sublist(0, encoded.length - 1)),
      Uint8List.fromList(<int>[...encoded, 0]),
    ]) {
      expect(
        () => decodeLitchcraftWireFrame(
          previous: previous,
          bytes: damaged,
          enableRgbOnly: true,
        ),
        throwsFormatException,
      );
    }
    expect(
      () => decodeLitchcraftWireFrame(
        previous: null,
        bytes: encoded,
        enableRgbOnly: true,
      ),
      throwsFormatException,
    );
  });

  test('Bouncing Balls opt-in codec stays near 123 KiB for 256 frames', () {
    final frames = LitchcraftBouncingBallsPreset.decode().frames;
    var total = 0;
    for (var index = 0; index < frames.length; index++) {
      final bytes = encodeLitchcraftWireFrame(
        previous: index % 32 == 0 ? null : frames[index - 1],
        frame: frames[index],
        enableRgbOnly: true,
      );
      expect(bytes.length, lessThanOrEqualTo(litchcraftWireMaximumFrameBytes));
      total += 6 + bytes.length; // Existing Store record length and CRC.
    }
    expect(total, 122791);
  });
  test(
    'repeat and indexed deltas match the firmware wire fixtures exactly',
    () {
      final previous = baseFrame();
      final repeated = LitchcraftAnimationFrame(
        durationMs: 50,
        emitters: previous.emitters,
      );
      expect(
        encodeLitchcraftWireFrame(previous: previous, frame: repeated),
        Uint8List.fromList(<int>[0, 50, 0]),
      );

      final changed = previous.emitters.toList();
      changed[173] = const LitchcraftAnimatedEmitter(
        color: 0xff0180,
        effect: LitchcraftEffectStyle.sparkle,
        effectDurationMs: 10000,
      );
      final frame = LitchcraftAnimationFrame(
        durationMs: 5000,
        emitters: changed,
      );
      final bytes = encodeLitchcraftWireFrame(previous: previous, frame: frame);
      expect(
        bytes,
        Uint8List.fromList(<int>[
          3,
          0x88,
          0x13,
          1,
          173,
          0xff,
          0x01,
          0x80,
          0x48,
          0x06,
        ]),
      );
      expect(
        decodeLitchcraftWireFrame(previous: previous, bytes: bytes),
        isA<LitchcraftAnimationFrame>()
            .having((value) => value.durationMs, 'duration', 5000)
            .having((value) => value.emitters[173].color, 'color', 0xff0180)
            .having(
              (value) => value.emitters[173].effect,
              'effect',
              LitchcraftEffectStyle.sparkle,
            )
            .having(
              (value) => value.emitters[173].effectDurationMs,
              'effect duration',
              10000,
            ),
      );
    },
  );

  test('dense raw fallback matches the firmware fixture and round-trips', () {
    final emitters = List<LitchcraftAnimatedEmitter>.generate(256, (index) {
      final effect = LitchcraftEffectStyle.values[index % 9];
      return LitchcraftAnimatedEmitter(
        color:
            (((index * 11 + 1) & 0xff) << 16) |
            (((index * 17 + 3) & 0xff) << 8) |
            ((index * 23 + 5) & 0xff),
        effect: effect,
        effectDurationMs: effect == LitchcraftEffectStyle.off
            ? 0
            : 500 + (index % 96) * 100,
      );
    });
    final frame = LitchcraftAnimationFrame(
      durationMs: 4950,
      emitters: emitters,
    );
    final bytes = encodeLitchcraftWireFrame(previous: null, frame: frame);

    expect(bytes, hasLength(litchcraftWireMaximumFrameBytes));
    expect(litchcraftAnimationCrc32(bytes), 0x77821a36);
    final decoded = decodeLitchcraftWireFrame(previous: null, bytes: bytes);
    expect(decoded.durationMs, frame.durationMs);
    for (var index = 0; index < emitters.length; index++) {
      expect(decoded.emitters[index].color, emitters[index].color);
      expect(decoded.emitters[index].effect, emitters[index].effect);
      expect(
        decoded.emitters[index].effectDurationMs,
        emitters[index].effectDurationMs,
      );
    }
  });
}
