import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:shared_preferences/shared_preferences.dart';

typedef LichessMonotonicNow = Duration Function();
typedef LichessWallNow = DateTime Function();
typedef LichessDelay = Future<void> Function(Duration duration);

final class LichessCooldownMarker {
  const LichessCooldownMarker({
    required this.recordedAtUtc,
    required this.notBeforeUtc,
  });

  final DateTime recordedAtUtc;
  final DateTime notBeforeUtc;
}

abstract interface class LichessCooldownStore {
  Future<LichessCooldownMarker?> read();

  Future<void> write(LichessCooldownMarker marker);

  Future<void> clear();
}

final class LichessCooldownPersistenceException implements Exception {
  const LichessCooldownPersistenceException();

  @override
  String toString() => 'Lichess request cooldown storage is unavailable.';
}

final class LichessSharedPreferencesCooldownStore
    implements LichessCooldownStore {
  const LichessSharedPreferencesCooldownStore(this._preferences);

  static const String _storageKey = 'lichess.request_cooldown.v1';

  final SharedPreferences _preferences;

  @override
  Future<LichessCooldownMarker?> read() async {
    final String? source = _preferences.getString(_storageKey);
    if (source == null) {
      return null;
    }
    final Object? decoded = jsonDecode(source);
    if (decoded is! Map<String, dynamic>) {
      throw const FormatException('Invalid Lichess cooldown marker.');
    }
    final Object? recordedValue = decoded['recordedAtUtc'];
    final Object? notBeforeValue = decoded['notBeforeUtc'];
    if (recordedValue is! String || notBeforeValue is! String) {
      throw const FormatException('Invalid Lichess cooldown marker.');
    }
    return LichessCooldownMarker(
      recordedAtUtc: DateTime.parse(recordedValue).toUtc(),
      notBeforeUtc: DateTime.parse(notBeforeValue).toUtc(),
    );
  }

  @override
  Future<void> write(LichessCooldownMarker marker) async {
    final bool stored = await _preferences.setString(
      _storageKey,
      jsonEncode(<String, String>{
        'recordedAtUtc': marker.recordedAtUtc.toIso8601String(),
        'notBeforeUtc': marker.notBeforeUtc.toIso8601String(),
      }),
    );
    if (!stored) {
      throw const LichessCooldownPersistenceException();
    }
  }

  @override
  Future<void> clear() async {
    final bool removed = await _preferences.remove(_storageKey);
    if (!removed) {
      throw const LichessCooldownPersistenceException();
    }
  }
}

final class LichessRequestCoordinator {
  factory LichessRequestCoordinator({
    required LichessCooldownStore cooldownStore,
    LichessMonotonicNow? monotonicNow,
    LichessWallNow? wallNow,
    LichessDelay? delay,
  }) {
    final Stopwatch stopwatch = Stopwatch()..start();
    return LichessRequestCoordinator._(
      cooldownStore: cooldownStore,
      monotonicNow: monotonicNow ?? () => stopwatch.elapsed,
      wallNow: wallNow ?? DateTime.now,
      delay: delay ?? Future<void>.delayed,
    );
  }

  LichessRequestCoordinator._({
    required LichessCooldownStore cooldownStore,
    required LichessMonotonicNow monotonicNow,
    required LichessWallNow wallNow,
    required LichessDelay delay,
  }) : _cooldownStore = cooldownStore,
       _monotonicNow = monotonicNow,
       _wallNow = wallNow,
       _delay = delay;

  static const Duration minimumCooldown = Duration(minutes: 1);
  static const Duration _maximumValidCooldown = Duration(days: 365);
  static const Duration _wallClockRollbackTolerance = Duration(minutes: 5);

  final LichessCooldownStore _cooldownStore;
  final LichessMonotonicNow _monotonicNow;
  final LichessWallNow _wallNow;
  final LichessDelay _delay;

  Future<void> _startTail = Future<void>.value();
  Future<void> _finiteTail = Future<void>.value();
  Future<void>? _restoreFuture;
  Duration _cooldownUntil = Duration.zero;
  bool _persistenceFailed = false;

  Future<T> startStream<T>(Future<T> Function() start) {
    return _queueStart(start);
  }

  Future<T> runFinite<T>(Future<T> Function() request) {
    final Future<void> previous = _finiteTail;
    final Completer<void> release = Completer<void>();
    _finiteTail = release.future;
    return (() async {
      await previous;
      try {
        return await _queueStart(request);
      } finally {
        release.complete();
      }
    })();
  }

  Future<void> recordRateLimit({String? retryAfter}) async {
    await _ensureRestored();
    final DateTime wallNowUtc = _wallNow().toUtc();
    final Duration requested = _retryAfterDuration(retryAfter, wallNowUtc);
    final Duration currentRemaining = _cooldownUntil - _monotonicNow();
    final Duration cooldown = _maximumDuration(
      minimumCooldown,
      _maximumDuration(requested, currentRemaining),
    );
    _cooldownUntil = _monotonicNow() + cooldown;
    await _writeMarker(
      LichessCooldownMarker(
        recordedAtUtc: wallNowUtc,
        notBeforeUtc: wallNowUtc.add(cooldown),
      ),
    );
  }

  Future<T> _queueStart<T>(Future<T> Function() operation) {
    final Future<void> previous = _startTail;
    final Completer<void> release = Completer<void>();
    _startTail = release.future;
    return (() async {
      await previous;
      try {
        await _waitForCooldown();
        return await operation();
      } finally {
        release.complete();
      }
    })();
  }

  Future<void> _waitForCooldown() async {
    await _ensureRestored();
    if (_persistenceFailed) {
      throw const LichessCooldownPersistenceException();
    }
    while (true) {
      final Duration remaining = _cooldownUntil - _monotonicNow();
      if (remaining <= Duration.zero) {
        return;
      }
      await _delay(remaining);
    }
  }

  Future<void> _ensureRestored() {
    return _restoreFuture ??= _restoreCooldown();
  }

  Future<void> _restoreCooldown() async {
    final LichessCooldownMarker? marker;
    try {
      marker = await _cooldownStore.read();
    } on FormatException {
      await _installFreshRestartCooldown();
      return;
    } catch (_) {
      _persistenceFailed = true;
      throw const LichessCooldownPersistenceException();
    }
    if (marker == null) {
      return;
    }

    final DateTime wallNowUtc = _wallNow().toUtc();
    final Duration markerLength = marker.notBeforeUtc.difference(
      marker.recordedAtUtc,
    );
    final bool invalidMarker =
        markerLength < minimumCooldown || markerLength > _maximumValidCooldown;
    final bool uncertainClock = wallNowUtc.isBefore(
      marker.recordedAtUtc.subtract(_wallClockRollbackTolerance),
    );
    if (invalidMarker || uncertainClock) {
      await _installFreshRestartCooldown();
      return;
    }

    final Duration remaining = marker.notBeforeUtc.difference(wallNowUtc);
    if (remaining > Duration.zero) {
      _cooldownUntil = _monotonicNow() + remaining;
      return;
    }
    await _clearMarker();
  }

  Future<void> _installFreshRestartCooldown() async {
    final DateTime wallNowUtc = _wallNow().toUtc();
    _cooldownUntil = _monotonicNow() + minimumCooldown;
    await _writeMarker(
      LichessCooldownMarker(
        recordedAtUtc: wallNowUtc,
        notBeforeUtc: wallNowUtc.add(minimumCooldown),
      ),
    );
  }

  Future<void> _writeMarker(LichessCooldownMarker marker) async {
    try {
      await _cooldownStore.write(marker);
    } catch (_) {
      _persistenceFailed = true;
      throw const LichessCooldownPersistenceException();
    }
  }

  Future<void> _clearMarker() async {
    try {
      await _cooldownStore.clear();
    } catch (_) {
      _persistenceFailed = true;
      throw const LichessCooldownPersistenceException();
    }
  }

  Duration _retryAfterDuration(String? source, DateTime wallNowUtc) {
    if (source == null || source.length > 128) {
      return Duration.zero;
    }
    final int? seconds = int.tryParse(source.trim());
    if (seconds != null && seconds >= 0) {
      final Duration duration = Duration(seconds: seconds);
      return duration <= _maximumValidCooldown ? duration : Duration.zero;
    }
    try {
      final Duration duration = HttpDate.parse(
        source,
      ).toUtc().difference(wallNowUtc);
      if (duration > Duration.zero && duration <= _maximumValidCooldown) {
        return duration;
      }
    } on FormatException {
      // Retry-After is optional; malformed values fall back to the minimum.
    }
    return Duration.zero;
  }
}

Duration _maximumDuration(Duration first, Duration second) {
  return first >= second ? first : second;
}
