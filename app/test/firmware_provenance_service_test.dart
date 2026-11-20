import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';
import 'package:litch_mobile/src/models/firmware_provenance.dart';
import 'package:litch_mobile/src/services/firmware_digest_reader.dart';
import 'package:litch_mobile/src/services/firmware_provenance_service.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';

class _Verifier implements FirmwareProvenanceVerifier {
  _Verifier(this.result);

  final bool? result;

  @override
  Future<bool?> isOfficialDigest(String sha256) async => result;
}

void main() {
  test('unavailable verifier and old firmware stay unverified', () async {
    final FirmwareProvenanceService service = FirmwareProvenanceService(
      verifier: _Verifier(null),
    );
    expect(
      (await service.inspect(
        connected: false,
        readDigest: () async => null,
      )).state,
      FirmwareProvenanceState.offline,
    );
    expect(
      (await service.inspect(
        connected: true,
        readDigest: () async => null,
      )).state,
      FirmwareProvenanceState.unverified,
    );
    expect(
      (await service.inspect(
        connected: true,
        readDigest: () async => 'a' * 64,
      )).state,
      FirmwareProvenanceState.unverified,
    );
  });

  test(
    'verifier distinguishes official and custom with fair-play copy',
    () async {
      final FirmwareProvenance official = await FirmwareProvenanceService(
        verifier: _Verifier(true),
      ).inspect(connected: true, readDigest: () async => 'a' * 64);
      expect(official.state, FirmwareProvenanceState.official);

      final FirmwareProvenance custom = await FirmwareProvenanceService(
        verifier: _Verifier(false),
      ).inspect(connected: true, readDigest: () async => 'b' * 64);
      expect(custom.state, FirmwareProvenanceState.custom);
      expect(custom.message, contains('rated or online play'));
      expect(custom.message, contains('Makers rule.'));
    },
  );

  test('digest reader assembles eight fragments before ready', () async {
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast(sync: true);
    final FirmwareDigestReader reader = FirmwareDigestReader(
      timeout: const Duration(milliseconds: 200),
    );
    final Future<String?> digest = reader.read(
      events: events.stream,
      sendRequest: () async => true,
    );
    await Future<void>.delayed(Duration.zero);
    for (int index = 0; index < 8; ++index) {
      events.add(
        OtaStatusEvent(
          deviceId: 'board-1',
          generation: 1,
          kind: OtaStatusKind.digest,
          state: 'digest_fragment',
          detail: '$index:${index.toString().padLeft(8, '0')}',
          rawText: '{}',
          payload: <String, Object?>{
            'h$index': index.toString().padLeft(8, '0'),
          },
        ),
      );
    }
    events.add(
      const OtaStatusEvent(
        deviceId: 'board-1',
        generation: 1,
        kind: OtaStatusKind.digest,
        state: 'digest_ready',
        rawText: '{}',
        payload: <String, Object?>{'h': 'r'},
      ),
    );

    expect(
      await digest,
      List<String>.generate(
        8,
        (int index) => index.toString().padLeft(8, '0'),
      ).join(),
    );
    await events.close();
  });
  test(
    'overlapping digest readers ignore the prior ready marker before their fragments',
    () async {
      final channel = LitchOtaControlChannel(
        deviceId: 'board-1',
        connectionToken: 7,
        generation: 11,
        subscribe: () => const Stream<List<int>>.empty(),
      );
      const reader = FirmwareDigestReader();
      var requests = 0, firstTerminals = 0, secondTerminals = 0;
      final first = reader.read(
        events: channel.events,
        sendRequest: () async {
          requests++;
          return true;
        },
        onTerminal: () => firstTerminals++,
      );
      await Future<void>.delayed(Duration.zero);
      for (var index = 0; index < 8; ++index) {
        channel.acceptPacket(
          utf8.encode(
            jsonEncode({'h$index': index.toString().padLeft(8, '0')}),
          ),
        );
      }
      // Real device order: availability metadata opens read2 after read1's eight
      // fragments, but before read1's delayed ready. Both use the SAME channel.
      var secondDone = false;
      final second = reader
          .read(
            events: channel.events,
            sendRequest: () async {
              requests++;
              return true;
            },
            onTerminal: () => secondTerminals++,
          )
          .then((value) {
            secondDone = true;
            return value;
          });
      await Future<void>.delayed(Duration.zero);
      channel.acceptPacket(utf8.encode('{"h":"r"}'));
      final expected = List.generate(
        8,
        (i) => i.toString().padLeft(8, '0'),
      ).join();
      expect(await first, expected);
      await Future<void>.delayed(Duration.zero);
      expect(
        secondDone,
        isFalse,
        reason:
            'The previous response ready has zero fragments in read2; it must keep waiting',
      );
      expect(firstTerminals, 1);
      expect(secondTerminals, 0);
      expect(requests, 2);
      for (var index = 0; index < 8; ++index) {
        channel.acceptPacket(
          utf8.encode(
            jsonEncode({'h$index': index.toString().padLeft(8, '0')}),
          ),
        );
      }
      await Future<void>.delayed(Duration.zero);
      expect(
        secondDone,
        isFalse,
        reason: 'Eight fragments alone are not terminal proof',
      );
      channel.acceptPacket(utf8.encode('{"h":"r"}'));
      expect(await second, expected);
      expect(secondTerminals, 1);
      expect(requests, 2);
      await channel.dispose();
    },
  );
  for (final throwOnTimeout in [false, true]) {
    testWidgets(
      'early ready and partial digest retain the exact eight-second deadline throw=$throwOnTimeout',
      (tester) async {
        final channel = LitchOtaControlChannel(
          deviceId: 'board-1',
          connectionToken: 1,
          generation: 1,
          subscribe: () => const Stream<List<int>>.empty(),
        );
        var done = false, requests = 0, terminals = 0;
        Object? result;
        const reader = FirmwareDigestReader();
        expect(reader.timeout, const Duration(seconds: 8));
        reader
            .read(
              events: channel.events,
              sendRequest: () async {
                requests++;
                return true;
              },
              throwOnTimeout: throwOnTimeout,
              onTerminal: () => terminals++,
            )
            .then<void>(
              (value) {
                done = true;
                result = value;
              },
              onError: (Object error) {
                done = true;
                result = error;
              },
            );
        channel.acceptPacket(utf8.encode('{"h":"r"}'));
        for (var i = 0; i < 7; i++) {
          channel.acceptPacket(utf8.encode(jsonEncode({'h$i': 'aaaaaaaa'})));
        }
        channel.acceptPacket(utf8.encode('{"h7":"not-sha8","h":"r"}'));
        await tester.pump();
        expect(done, isFalse);
        await tester.pump(const Duration(seconds: 7));
        channel.acceptPacket(utf8.encode('{"h":"r"}'));
        await tester.pump(const Duration(milliseconds: 999));
        expect(done, isFalse);
        await tester.pump(const Duration(milliseconds: 1));
        await tester.pump();
        expect(terminals, 1);
        // The sender is fenced at exactly 8s. Drain the standard broadcast
        // cancellation Future in its real zone before observing read's return.
        await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        await tester.pump();
        expect(done, isTrue);
        expect(requests, 1);
        expect(result, throwOnTimeout ? isA<TimeoutException>() : isNull);
        channel.acceptPacket(utf8.encode('{"h7":"aaaaaaaa","h":"r"}'));
        await tester.pump();
        expect(terminals, 1);
        await channel.dispose();
      },
    );
  }

  test(
    'cancelled digest cannot adopt late complete fragments after early ready',
    () async {
      final channel = LitchOtaControlChannel(
        deviceId: 'board-1',
        connectionToken: 1,
        generation: 1,
        subscribe: () => const Stream<List<int>>.empty(),
      );
      final cancel = Completer<void>();
      var terminals = 0;
      final digest = const FirmwareDigestReader().read(
        events: channel.events,
        sendRequest: () async => true,
        cancelled: cancel.future,
        onTerminal: () => terminals++,
      );
      channel.acceptPacket(utf8.encode('{"h":"r"}'));
      channel.acceptPacket(utf8.encode('{"h0":"aaaaaaaa"}'));
      cancel.complete();
      expect(await digest, isNull);
      for (var i = 0; i < 8; i++) {
        channel.acceptPacket(utf8.encode(jsonEncode({'h$i': 'aaaaaaaa'})));
      }
      channel.acceptPacket(utf8.encode('{"h":"r"}'));
      expect(terminals, 1);
      await channel.dispose();
    },
  );

  test(
    'retired digest channel cannot contribute fragments across connection generations',
    () async {
      LitchOtaControlChannel channel(int generation) => LitchOtaControlChannel(
        deviceId: 'board-1',
        connectionToken: generation,
        generation: generation,
        subscribe: () => const Stream<List<int>>.empty(),
      );
      final old = channel(1), current = channel(2);
      const reader = FirmwareDigestReader();
      final prior = reader.read(
        events: old.events,
        sendRequest: () async => true,
      );
      for (var i = 0; i < 4; i++) {
        old.acceptPacket(utf8.encode(jsonEncode({'h$i': 'aaaaaaaa'})));
      }
      await old.dispose();
      expect(await prior, isNull);
      var done = false;
      final fresh = reader
          .read(events: current.events, sendRequest: () async => true)
          .then((value) {
            done = true;
            return value;
          });
      for (var i = 4; i < 8; i++) {
        current.acceptPacket(utf8.encode(jsonEncode({'h$i': 'bbbbbbbb'})));
      }
      current.acceptPacket(utf8.encode('{"h":"r"}'));
      for (var i = 0; i < 8; i++) {
        old.acceptPacket(utf8.encode(jsonEncode({'h$i': 'aaaaaaaa'})));
      }
      old.acceptPacket(utf8.encode('{"h":"r"}'));
      await Future<void>.delayed(Duration.zero);
      expect(
        done,
        isFalse,
        reason: 'Old-channel fragments cannot fill the new channel digest',
      );
      for (var i = 0; i < 4; i++) {
        current.acceptPacket(utf8.encode(jsonEncode({'h$i': 'bbbbbbbb'})));
      }
      current.acceptPacket(utf8.encode('{"h":"r"}'));
      expect(await fresh, 'b' * 64);
      await current.dispose();
    },
  );

  for (final response in [
    'error',
    'unavailable',
    'unknown_cmd',
    'send_failed',
    'stream_error',
    'stream_done',
  ]) {
    test('early ready keeps existing terminal refusal $response', () async {
      final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
      final digest = const FirmwareDigestReader().read(
        events: events.stream,
        sendRequest: () async => response != 'send_failed',
      );
      events.add(
        const OtaStatusEvent(
          deviceId: 'board-1',
          generation: 1,
          kind: OtaStatusKind.digest,
          state: 'digest_ready',
          rawText: '{}',
          payload: {'h': 'r'},
        ),
      );
      if (response == 'stream_error') {
        events.addError(StateError('controlled notification failure'));
      } else if (response == 'stream_done') {
        await events.close();
      } else if (response != 'send_failed') {
        events.add(
          OtaStatusEvent(
            deviceId: 'board-1',
            generation: 1,
            kind: response == 'unknown_cmd'
                ? OtaStatusKind.error
                : OtaStatusKind.digest,
            state: 'unavailable',
            detail: response == 'unknown_cmd' ? 'unknown_cmd' : '',
            rawText: '{}',
            payload: {'h': response == 'error' ? 'e' : 'u'},
          ),
        );
      }
      expect(await digest, isNull);
      if (!events.isClosed) await events.close();
    });
  }
}
