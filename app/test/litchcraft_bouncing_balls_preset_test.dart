import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_bouncing_balls_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';

void main() {
  test('bouncing balls is the full-scale dense animation benchmark', () {
    final decode = Stopwatch()..start();
    final preset = LitchcraftBouncingBallsPreset.decode();
    decode.stop();
    expect(preset.frames, hasLength(256));
    expect(preset.frames.map((frame) => frame.durationMs), everyElement(50));
    expect(
      preset.frames.every((frame) => frame.emitters.length == 256),
      isTrue,
    );
    expect(
      preset.frames
          .map(
            (frame) =>
                Object.hashAll(frame.emitters.map((emitter) => emitter.color)),
          )
          .toSet()
          .length,
      255,
    );
    expect(
      preset.frames.last.emitters.map((emitter) => emitter.color),
      preset.frames.first.emitters.map((emitter) => emitter.color),
      reason: 'the fixed 256-frame document must loop without a visual seam',
    );

    final encode = Stopwatch()..start();
    final transfer = LitchcraftFrameStoreProtocol.prepare(
      session: 1,
      transfer: 1,
      expectedGeneration: 0,
      topology: LitchcraftTopology.v3.index,
      active: true,
      playbackOnEntry: true,
      sparkleSeed: 1,
      favoriteColors: litchcraftDefaultFavoriteColors,
      frames: preset.frames,
    );
    encode.stop();
    final packets = transfer.chunkPackets(maximumPacketBytes: 244).toList();
    expect(transfer.bodyBytes, inInclusiveRange(160000, 220000));
    expect(packets.length, inInclusiveRange(690, 955));
    expect(packets.every((packet) => packet.length <= 244), isTrue);
    // Stable benchmark evidence, not a cross-host pass/fail latency threshold.
    // ignore: avoid_print
    print(
      'Balls256 decode=${decode.elapsedMilliseconds}ms '
      'encode=${encode.elapsedMilliseconds}ms body=${transfer.bodyBytes} '
      'ATT244=${packets.length}',
    );
  });
}
