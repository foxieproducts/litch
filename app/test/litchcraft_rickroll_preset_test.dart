import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_rickroll_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';

void main() {
  test(
    'V3 sampling geometry is the production LED-detail left top right bottom order',
    () {
      expect(
        [
          for (final mask in [1, 2, 4, 8])
            litchcraftEmitterPosition(mask, LitchcraftTopology.v3, false),
        ],
        const [
          Offset(.18, .5),
          Offset(.5, .18),
          Offset(.82, .5),
          Offset(.5, .82),
        ],
      );
      expect(
        litchcraftEmitterPosition(1, LitchcraftTopology.v3, true),
        const Offset(.82, .5),
      );
    },
  );
  test(
    'preset fits the existing schema-4 transaction and ATT244 packet bound',
    () {
      final preset = LitchcraftRickrollPreset.decode();
      final transfer = LitchcraftFrameStoreProtocol.prepare(
        session: 1,
        transfer: 1,
        expectedGeneration: 0,
        topology: 3,
        active: true,
        playbackOnEntry: true,
        sparkleSeed: 1,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: preset.frames,
      );
      final packets = transfer.chunkPackets(maximumPacketBytes: 244).toList();
      expect(packets.every((packet) => packet.length <= 244), isTrue);
      expect(transfer.bodyBytes, lessThan(30000));
      // Evidence emitted for owner documentation; this is not a latency claim.
      // ignore: avoid_print
      print(
        'Rickroll schema4 body=${transfer.bodyBytes} ATT244 chunks=${packets.length}',
      );
    },
  );
  test(
    'Rickroll preserves every source dwell and four semantic samples per square',
    () {
      final preset = LitchcraftRickrollPreset.decode();
      expect(preset.frames, hasLength(32));
      expect(preset.frames.map((frame) => frame.durationMs), everyElement(50));
      expect(
        preset.frames.every((frame) => frame.emitters.length == 256),
        isTrue,
      );
      final first = preset.frames.first.emitters;
      expect(
        first.map((emitter) => emitter.color).toSet().length,
        greaterThan(64),
      );
      expect(
        first.take(4).map((emitter) => emitter.color).toSet().length,
        greaterThan(1),
      );
    },
  );

  test(
    'cached phone frames use the production effect compositor and loop without allocations',
    () {
      final preset = LitchcraftRickrollPreset.decode();
      final output = List<int>.filled(256, 0);
      for (var frame = 0; frame < 32; frame++) {
        composeLitchcraftAnimationFrameInto(
          preset.frames[frame],
          frame * 50,
          output,
          frameIndex: frame,
        );
        expect(preset.canvasAt(frame * 50).colors, output);
        expect(
          identical(
            preset.canvasAt(frame * 50),
            preset.canvasAt(frame * 50 + 1600),
          ),
          isTrue,
        );
        for (var emitter = 0; emitter < 256; emitter++) {
          expect(
            preset.frames[frame].emitters[emitter].effect,
            preset.frames.first.emitters[emitter].effect,
          );
        }
      }
      expect(
        preset.frames.first.emitters.where(
          (e) => e.effect != LitchcraftEffectStyle.off,
        ),
        hasLength(16),
      );
    },
  );
}
