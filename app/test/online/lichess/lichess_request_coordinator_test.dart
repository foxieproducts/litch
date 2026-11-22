import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/online/lichess/lichess_request_coordinator.dart';

void main() {
  test('finite requests remain serialized until each completes', () async {
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = _coordinator(time);
    final Completer<void> firstStarted = Completer<void>();
    final Completer<void> releaseFirst = Completer<void>();
    bool secondStarted = false;

    final Future<int> first = coordinator.runFinite<int>(() async {
      firstStarted.complete();
      await releaseFirst.future;
      return 1;
    });
    await firstStarted.future;
    final Future<int> second = coordinator.runFinite<int>(() async {
      secondStarted = true;
      return 2;
    });
    await Future<void>.delayed(Duration.zero);

    expect(secondStarted, isFalse);
    releaseFirst.complete();
    expect(await first, 1);
    expect(await second, 2);
    expect(secondStarted, isTrue);
  });

  test('stream ownership releases after the request handshake', () async {
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = _coordinator(time);

    expect(
      await coordinator.startStream<String>(() async => 'open-1'),
      'open-1',
    );
    expect(
      await coordinator.startStream<String>(() async => 'open-2'),
      'open-2',
    );
  });

  test('429 uses the longer retry window and persists it', () async {
    final _ManualTime time = _ManualTime();
    final _FakeCooldownStore store = _FakeCooldownStore();
    final LichessRequestCoordinator coordinator = _coordinator(time, store);

    await coordinator.recordRateLimit(retryAfter: '120');
    bool started = false;
    await coordinator.startStream<void>(() async {
      started = true;
    });

    expect(started, isTrue);
    expect(time.delays, <Duration>[const Duration(minutes: 2)]);
    expect(
      store.marker!.notBeforeUtc.difference(store.marker!.recordedAtUtc),
      const Duration(minutes: 2),
    );
  });

  test('persisted cooldown survives a process restart', () async {
    final DateTime recorded = DateTime.utc(2026, 9, 3, 10);
    final _FakeCooldownStore store = _FakeCooldownStore(
      marker: LichessCooldownMarker(
        recordedAtUtc: recorded,
        notBeforeUtc: recorded.add(const Duration(seconds: 90)),
      ),
    );
    final _ManualTime time = _ManualTime(wallNow: recorded);
    final LichessRequestCoordinator restarted = _coordinator(time, store);

    await restarted.startStream<void>(() async {});

    expect(time.delays, <Duration>[const Duration(seconds: 90)]);
  });

  test('uncertain restart wall clock waits a fresh minute', () async {
    final DateTime recorded = DateTime.utc(2026, 9, 3, 10);
    final _FakeCooldownStore store = _FakeCooldownStore(
      marker: LichessCooldownMarker(
        recordedAtUtc: recorded,
        notBeforeUtc: recorded.add(const Duration(minutes: 2)),
      ),
    );
    final _ManualTime time = _ManualTime(
      wallNow: recorded.subtract(const Duration(days: 1)),
    );
    final LichessRequestCoordinator restarted = _coordinator(time, store);

    await restarted.startStream<void>(() async {});

    expect(time.delays, <Duration>[LichessRequestCoordinator.minimumCooldown]);
    expect(
      store.marker!.recordedAtUtc,
      time.wallNow.subtract(const Duration(minutes: 1)),
    );
  });

  test('an HTTP-date Retry-After can extend the minimum', () async {
    final _ManualTime time = _ManualTime(wallNow: DateTime.utc(2026, 9, 3, 10));
    final _FakeCooldownStore store = _FakeCooldownStore();
    final LichessRequestCoordinator coordinator = _coordinator(time, store);

    await coordinator.recordRateLimit(
      retryAfter: 'Thu, 03 Sep 2026 10:03:00 GMT',
    );
    await coordinator.startStream<void>(() async {});

    expect(time.delays, <Duration>[const Duration(minutes: 3)]);
  });

  test('an expired persisted marker is cleared without delay', () async {
    final DateTime recorded = DateTime.utc(2026, 9, 3, 10);
    final _FakeCooldownStore store = _FakeCooldownStore(
      marker: LichessCooldownMarker(
        recordedAtUtc: recorded,
        notBeforeUtc: recorded.add(const Duration(minutes: 1)),
      ),
    );
    final _ManualTime time = _ManualTime(
      wallNow: recorded.add(const Duration(hours: 1)),
    );
    final LichessRequestCoordinator restarted = _coordinator(time, store);

    await restarted.startStream<void>(() async {});

    expect(time.delays, isEmpty);
    expect(store.marker, isNull);
  });

  test('a cooldown write failure fails closed for later starts', () async {
    final _ManualTime time = _ManualTime();
    final _FakeCooldownStore store = _FakeCooldownStore(throwOnWrite: true);
    final LichessRequestCoordinator coordinator = _coordinator(time, store);

    await expectLater(
      coordinator.recordRateLimit(),
      throwsA(isA<LichessCooldownPersistenceException>()),
    );
    bool started = false;
    await expectLater(
      coordinator.startStream<void>(() async {
        started = true;
      }),
      throwsA(isA<LichessCooldownPersistenceException>()),
    );

    expect(started, isFalse);
  });

  test('a cooldown read failure fails closed without starting work', () async {
    final _ManualTime time = _ManualTime();
    final _FakeCooldownStore store = _FakeCooldownStore(throwOnRead: true);
    final LichessRequestCoordinator coordinator = _coordinator(time, store);
    bool started = false;

    await expectLater(
      coordinator.startStream<void>(() async {
        started = true;
      }),
      throwsA(isA<LichessCooldownPersistenceException>()),
    );

    expect(started, isFalse);
  });
}

LichessRequestCoordinator _coordinator(
  _ManualTime time, [
  _FakeCooldownStore? store,
]) {
  return LichessRequestCoordinator(
    cooldownStore: store ?? _FakeCooldownStore(),
    monotonicNow: () => time.monotonicNow,
    wallNow: () => time.wallNow,
    delay: time.delay,
  );
}

final class _ManualTime {
  _ManualTime({DateTime? wallNow})
    : wallNow = wallNow ?? DateTime.utc(2026, 9, 3);

  Duration monotonicNow = Duration.zero;
  DateTime wallNow;
  final List<Duration> delays = <Duration>[];

  Future<void> delay(Duration duration) async {
    delays.add(duration);
    monotonicNow += duration;
    wallNow = wallNow.add(duration);
  }
}

final class _FakeCooldownStore implements LichessCooldownStore {
  _FakeCooldownStore({
    this.marker,
    this.throwOnRead = false,
    this.throwOnWrite = false,
  });

  LichessCooldownMarker? marker;
  final bool throwOnRead;
  final bool throwOnWrite;

  @override
  Future<void> clear() async {
    marker = null;
  }

  @override
  Future<LichessCooldownMarker?> read() async {
    if (throwOnRead) {
      throw StateError('platform read detail');
    }
    return marker;
  }

  @override
  Future<void> write(LichessCooldownMarker value) async {
    if (throwOnWrite) {
      throw StateError('platform write detail');
    }
    marker = value;
  }
}
