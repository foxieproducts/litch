import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';

void main() {
  test(
    'active capability waiter observes closure while ordinary OTA consumers keep existing semantics',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final channel = LitchOtaControlChannel(
        deviceId: 'board-1',
        connectionToken: 1,
        generation: 1,
        subscribe: () => packets.stream,
      );
      // No error handler: clean native closure must stay invisible to public OTA
      // status consumers rather than becoming an unhandled public stream error.
      final public = channel.events.listen((_) {});
      final errors = <Object>[];
      final setup = channel.waitFor(
        (_) => true,
        timeout: const Duration(seconds: 1),
        onNotificationError: errors.add,
      );
      final ordinary = channel.waitFor(
        (_) => true,
        timeout: const Duration(milliseconds: 20),
      );
      await Future<void>.delayed(Duration.zero);
      await packets.close();
      expect(await setup.timeout(const Duration(milliseconds: 100)), isNull);
      expect(errors, hasLength(1));
      expect(await ordinary, isNull);
      await public.cancel();
      await channel.dispose();
    },
  );

  for (final closes in [false, true]) {
    test(
      'capability observer sees early native ${closes ? 'closure' : 'error'}',
      () async {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        var subscriptions = 0;
        final channel = LitchOtaControlChannel(
          deviceId: 'board-1',
          connectionToken: 1,
          generation: 1,
          subscribe: () {
            subscriptions++;
            return packets.stream;
          },
        );
        final errors = <Object>[];
        final bridge = channel.events.listen((_) {}, onError: (_) {});
        await channel.start();
        if (closes) {
          await packets.close();
        } else {
          packets.addError(
            StateError('native setup failed before first waiter'),
          );
        }
        await Future<void>.delayed(Duration.zero);
        final result = await channel.waitFor(
          (_) => true,
          timeout: const Duration(milliseconds: 20),
          onNotificationError: errors.add,
        );
        expect(result, isNull);
        expect(errors, hasLength(1));
        expect(
          subscriptions,
          1,
          reason: 'Capability setup cannot silently replace its failed owner.',
        );
        await bridge.cancel();
        await channel.dispose();
        await packets.close();
      },
    );
  }

  test('shared-session packets feed the existing status owner', () async {
    final LitchOtaControlChannel channel = LitchOtaControlChannel(
      connectionToken: 9,
      deviceId: 'board-1',
      generation: 4,
      subscribe: () => const Stream<List<int>>.empty(),
    );
    final List<OtaStatusEvent> observed = <OtaStatusEvent>[];
    final StreamSubscription<OtaStatusEvent> events = channel.events.listen(
      observed.add,
    );

    channel.acceptPacket(utf8.encode('{"p":42,"r":4096}'));
    expect(observed, hasLength(1));
    expect(observed.single.kind, OtaStatusKind.progress);
    expect(observed.single.progressPercent, 42);
    expect(observed.single.rxBytes, 4096);
    expect(observed.single.generation, 4);

    await events.cancel();
    await channel.dispose();
  });

  test(
    'uses one notification subscription for bridge and request waiters',
    () async {
      final StreamController<List<int>> packets =
          StreamController<List<int>>.broadcast();
      int subscribeCalls = 0;
      final LitchOtaControlChannel channel = LitchOtaControlChannel(
        connectionToken: 9,
        deviceId: 'board-1',
        generation: 9,
        subscribe: () {
          subscribeCalls += 1;
          return packets.stream;
        },
      );

      await channel.start();
      final StreamSubscription<OtaStatusEvent> bridge = channel.events.listen(
        (_) {},
      );
      final Future<OtaStatusEvent?> version = channel.waitFor(
        (OtaStatusEvent event) => event.kind == OtaStatusKind.version,
        timeout: const Duration(seconds: 1),
      );

      packets.add(utf8.encode('{"v":"2.0.0"}'));
      expect((await version)?.detail, '2.0.0');
      expect(subscribeCalls, 1);

      await bridge.cancel();
      await channel.dispose();
      await packets.close();
    },
  );

  test(
    'shared-session takeover retires direct packets without closing status',
    () async {
      final StreamController<List<int>> packets =
          StreamController<List<int>>.broadcast();
      int subscribeCalls = 0;
      final LitchOtaControlChannel channel = LitchOtaControlChannel(
        connectionToken: 9,
        deviceId: 'board-1',
        generation: 10,
        subscribe: () {
          subscribeCalls += 1;
          return packets.stream;
        },
      );
      final List<OtaStatusEvent> observed = <OtaStatusEvent>[];
      final StreamSubscription<OtaStatusEvent> events = channel.events.listen(
        observed.add,
      );

      await channel.start();
      packets.add(utf8.encode('{"p":1}'));
      await Future<void>.delayed(Duration.zero);
      expect(observed, hasLength(1));

      final setupErrors = <Object>[];
      final waiting = channel.waitFor(
        (event) => event.progressPercent == 3,
        timeout: const Duration(seconds: 1),
        onNotificationError: setupErrors.add,
      );
      await Future<void>.delayed(Duration.zero);

      await channel.retireDirectPackets();
      await channel.start();
      packets.add(utf8.encode('{"p":2}'));
      channel.acceptPacket(utf8.encode('{"p":3}'));
      await Future<void>.delayed(Duration.zero);

      expect(subscribeCalls, 1);
      expect(observed.map((event) => event.progressPercent), <int?>[1, 3]);
      expect((await waiting)?.progressPercent, 3);
      expect(
        setupErrors,
        isEmpty,
        reason: 'Intentional shared takeover is not a native setup closure.',
      );

      await events.cancel();
      await channel.dispose();
      await packets.close();
    },
  );

  test('normalizes received bytes monotonically within one transfer', () async {
    final StreamController<List<int>> packets =
        StreamController<List<int>>.broadcast();
    final LitchOtaControlChannel channel = LitchOtaControlChannel(
      connectionToken: 9,
      deviceId: 'board-1',
      generation: 1,
      subscribe: () => packets.stream,
    );
    await channel.start();
    final List<int?> observedRx = <int?>[];
    final StreamSubscription<OtaStatusEvent> sub = channel.events.listen(
      (OtaStatusEvent event) => observedRx.add(event.rxBytes),
    );

    packets.add(utf8.encode('{"b":1}'));
    packets.add(utf8.encode('{"p":10,"r":4096}'));
    packets.add(utf8.encode('{"p":12,"r":2048}'));
    await Future<void>.delayed(Duration.zero);

    expect(observedRx, <int?>[0, 4096, 4096]);
    await sub.cancel();
    await channel.dispose();
    await packets.close();
  });

  test(
    'can establish a fresh notification after the packet stream ends',
    () async {
      final List<StreamController<List<int>>> packetStreams =
          <StreamController<List<int>>>[];
      final LitchOtaControlChannel channel = LitchOtaControlChannel(
        connectionToken: 9,
        deviceId: 'board-1',
        generation: 1,
        subscribe: () {
          final StreamController<List<int>> packets =
              StreamController<List<int>>.broadcast();
          packetStreams.add(packets);
          return packets.stream;
        },
      );

      await channel.start();
      expect(packetStreams, hasLength(1));
      await packetStreams.single.close();
      await Future<void>.delayed(Duration.zero);
      await channel.start();

      expect(packetStreams, hasLength(2));
      await channel.dispose();
      await packetStreams.last.close();
    },
  );

  test('can establish a fresh notification after a stream error', () async {
    final List<StreamController<List<int>>> packetStreams =
        <StreamController<List<int>>>[];
    final List<Object> observedErrors = <Object>[];
    final LitchOtaControlChannel channel = LitchOtaControlChannel(
      connectionToken: 9,
      deviceId: 'board-1',
      generation: 1,
      subscribe: () {
        final StreamController<List<int>> packets =
            StreamController<List<int>>.broadcast();
        packetStreams.add(packets);
        return packets.stream;
      },
    );
    final StreamSubscription<OtaStatusEvent> events = channel.events.listen(
      (_) {},
      onError: observedErrors.add,
    );

    await channel.start();
    packetStreams.single.addError(StateError('notification failed'));
    await Future<void>.delayed(Duration.zero);
    await channel.start();

    expect(observedErrors, hasLength(1));
    expect(packetStreams, hasLength(2));
    final Future<OtaStatusEvent?> version = channel.waitFor(
      (OtaStatusEvent event) => event.kind == OtaStatusKind.version,
      timeout: const Duration(seconds: 1),
    );
    packetStreams.last.add(utf8.encode('{"v":"2.0.1"}'));
    expect((await version)?.detail, '2.0.1');

    await events.cancel();
    await channel.dispose();
    await packetStreams.first.close();
    await packetStreams.last.close();
  });

  test('waiters finish promptly when the channel closes', () async {
    final StreamController<List<int>> packets =
        StreamController<List<int>>.broadcast();
    final LitchOtaControlChannel channel = LitchOtaControlChannel(
      connectionToken: 9,
      deviceId: 'board-1',
      generation: 1,
      subscribe: () => packets.stream,
    );
    final Future<OtaStatusEvent?> waiting = channel.waitFor(
      (OtaStatusEvent event) => event.kind == OtaStatusKind.runtimeHealth,
      timeout: const Duration(seconds: 5),
      onNotificationError: (error) =>
          fail('Intentional disposal is not a native setup failure: $error'),
    );

    await channel.dispose();
    expect(await waiting, isNull);
    await packets.close();
  });
}
