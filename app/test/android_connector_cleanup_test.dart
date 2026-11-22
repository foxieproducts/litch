import 'dart:async';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_reactive_ble/src/device_connector.dart';
import 'package:flutter_reactive_ble/src/device_scanner.dart';
import 'package:flutter_test/flutter_test.dart';

class _Scanner implements DeviceScanner {
  @override
  ScanSession? get currentScan => null;
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _Platform implements ReactiveBlePlatform {
  final updates = StreamController<ConnectionStateUpdate>.broadcast(sync: true);
  final disconnectFinished = Completer<void>();
  int disconnectCalls = 0;
  int connectCalls = 0;

  @override
  Stream<ConnectionStateUpdate> get connectionUpdateStream => updates.stream;

  @override
  Stream<void> connectToDevice(String id,
      Map<Uuid, List<Uuid>>? services, Duration? timeout) {
    connectCalls++;
    return Stream<void>.value(null);
  }

  @override
  Future<void> disconnectDevice(String id) async {
    disconnectCalls++;
    if (disconnectCalls == 1) {
      await disconnectFinished.future;
      updates.add(ConnectionStateUpdate(deviceId: id,
        connectionState: DeviceConnectionState.disconnected, failure: null));
    }
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

void main() {
  test('real connector cancellation must drain native cleanup before fallback', () async {
    final platform = _Platform();
    final connector = DeviceConnectorImpl(
      blePlatform: platform,
      deviceIsDiscoveredRecently: ({required deviceId, required cacheValidity}) => true,
      deviceScanner: _Scanner(),
      delayAfterScanFailure: Duration.zero,
    );
    final first = connector.connect(id: 'board-1').listen((_) {});
    await Future<void>.delayed(Duration.zero);
    bool cancellationReturned = false;
    final cancelling = first.cancel().then((_) => cancellationReturned = true);
    await Future<void>.delayed(Duration.zero);
    final returnedBeforeNativeCleanup = cancellationReturned;
    final events = <DeviceConnectionState>[];
    final fallbackReady = cancelling.then((_) {
    return connector.connectToAdvertisingDevice(
      id: 'board-1', withServices: const [], prescanDuration: Duration.zero,
    ).listen((event) => events.add(event.connectionState));
    });
    await Future<void>.delayed(Duration.zero);
    platform.disconnectFinished.complete();
    final fallback = await fallbackReady;
    await Future<void>.delayed(Duration.zero);
    platform.updates.add(const ConnectionStateUpdate(deviceId: 'board-1',
      connectionState: DeviceConnectionState.connected, failure: null));
    await Future<void>.delayed(Duration.zero);
    final observed = List<DeviceConnectionState>.of(events);
    unawaited(fallback.cancel());
    await Future<void>.delayed(Duration.zero);
    await cancelling;
    unawaited(platform.updates.close());
    await Future<void>.delayed(Duration.zero);
    // Uses deployed plugin connector/Repeater; only native callback timing is controlled.
    expect(returnedBeforeNativeCleanup, isFalse,
      reason: 'A completed cancel must be a safe boundary for starting fallback');
    expect(observed, contains(DeviceConnectionState.connected),
      reason: 'Previous native cleanup must not terminate the new connection');
  });
}
