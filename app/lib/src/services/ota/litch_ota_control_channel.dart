import 'dart:async';

import 'ota_status_event.dart';

typedef OtaPacketSubscriptionFactory = Stream<List<int>> Function();

class _NotificationClosed implements Exception {
  @override
  String toString() => 'BLE notification stream closed during setup.';
}

class LitchOtaControlChannel {
  LitchOtaControlChannel({
    required this.deviceId,
    required this.connectionToken,
    required this.generation,
    required OtaPacketSubscriptionFactory subscribe,
  }) : _subscribe = subscribe;

  final String deviceId;
  // A notification subscription may renew within this one BLE connection.
  final int connectionToken;
  final int generation;
  final OtaPacketSubscriptionFactory _subscribe;
  final StreamController<OtaStatusEvent> _events =
      StreamController<OtaStatusEvent>.broadcast(sync: true);

  StreamSubscription<List<int>>? _packetSubscription;
  Object? _notificationFailure;
  int _packetGeneration = 0;
  int _maxRxBytes = 0;
  bool _directPacketsRetired = false;
  bool _disposed = false;

  // Closure is a setup-lifetime signal for capability waiters. Public OTA
  // consumers retain the existing no-reply/explicit-renewal semantics.
  Stream<OtaStatusEvent> get events => _events.stream.handleError(
    (Object _) {},
    test: (error) => error is _NotificationClosed,
  );

  void acceptPacket(List<int> packet) {
    if (!_disposed) _handlePacket(packet);
  }

  Future<void> start() async {
    if (_disposed || _directPacketsRetired || _packetSubscription != null) {
      return;
    }
    final generation = ++_packetGeneration;
    _notificationFailure = null;
    bool current() =>
        !_disposed && !_directPacketsRetired && generation == _packetGeneration;
    void ended(Object error) {
      if (!current()) return;
      _packetSubscription = null;
      _notificationFailure = error;
    }

    StreamSubscription<List<int>>? subscription;
    subscription = _subscribe().listen(
      (packet) {
        if (current()) _handlePacket(packet);
      },
      onError: (Object error, StackTrace stackTrace) {
        if (!current()) return;
        ended(error);
        unawaited(subscription?.cancel());
        if (!_events.isClosed) {
          _events.addError(error, stackTrace);
        }
      },
      onDone: () {
        if (!current()) return;
        final error = _NotificationClosed();
        ended(error);
        if (!_events.isClosed) _events.addError(error);
      },
    );
    if (current() && _notificationFailure == null) {
      _packetSubscription = subscription;
    } else {
      unawaited(subscription.cancel());
    }
  }

  /// Stops the compatibility notification without closing the typed status
  /// owner. Shared-session packets may continue through [acceptPacket].
  Future<void> retireDirectPackets() async {
    if (_disposed || _directPacketsRetired) return;
    _directPacketsRetired = true;
    ++_packetGeneration;
    _notificationFailure = null;
    final StreamSubscription<List<int>>? subscription = _packetSubscription;
    _packetSubscription = null;
    await subscription?.cancel();
  }

  Future<OtaStatusEvent?> waitFor(
    bool Function(OtaStatusEvent event) predicate, {
    required Duration timeout,
    Future<void>? cancelled,
    void Function(Object error)? onNotificationError,
  }) async {
    bool observedFailure() {
      final failure = _notificationFailure;
      if (!_disposed &&
          !_directPacketsRetired &&
          failure != null &&
          onNotificationError != null) {
        onNotificationError(failure);
        return true;
      }
      return false;
    }

    // Capability setup observes the failed owner before start() can renew it.
    // Default OTA waiters retain their existing explicit renewal semantics.
    if (observedFailure()) return null;
    await start();
    if (observedFailure()) return null;
    final Completer<OtaStatusEvent?> completer = Completer<OtaStatusEvent?>();
    late final StreamSubscription<OtaStatusEvent> subscription;
    Timer? timer;
    subscription = _events.stream.listen(
      (OtaStatusEvent event) {
        if (!completer.isCompleted && predicate(event)) {
          completer.complete(event);
          timer?.cancel();
          unawaited(subscription.cancel());
        }
      },
      onError: (Object error) {
        if (error is _NotificationClosed && onNotificationError == null) return;
        if (!completer.isCompleted) {
          onNotificationError?.call(error);
          completer.complete(null);
          timer?.cancel();
          unawaited(subscription.cancel());
        }
      },
      onDone: () {
        if (!completer.isCompleted) {
          completer.complete(null);
          timer?.cancel();
        }
      },
    );
    timer = Timer(timeout, () {
      if (!completer.isCompleted) {
        completer.complete(null);
        unawaited(subscription.cancel());
      }
    });
    cancelled?.then((_) {
      if (!completer.isCompleted) completer.complete(null);
    });
    try {
      return await completer.future;
    } finally {
      timer.cancel();
      // Detach this local broadcast listener without delaying a reply that is
      // already complete. The shared native notification owner stays alive.
      unawaited(subscription.cancel());
    }
  }

  void _handlePacket(List<int> packet) {
    OtaStatusEvent? event = OtaStatusEvent.decode(
      packet,
      deviceId: deviceId,
      generation: generation,
    );
    if (event == null || _events.isClosed) {
      return;
    }
    if (event.kind == OtaStatusKind.begin) {
      _maxRxBytes = 0;
    }
    final int? reportedRx = event.rxBytes;
    if (reportedRx != null && reportedRx > _maxRxBytes) {
      _maxRxBytes = reportedRx;
    }
    if (event.kind == OtaStatusKind.begin ||
        event.kind == OtaStatusKind.progress ||
        event.kind == OtaStatusKind.done) {
      event = event.withRxBytes(_maxRxBytes);
    }
    _events.add(event);
  }

  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    ++_packetGeneration;
    _notificationFailure = null;
    await _packetSubscription?.cancel();
    _packetSubscription = null;
    await _events.close();
  }
}
