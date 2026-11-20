import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';

class _Permissions implements BlePermissionGateway {
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      PermissionStatus.granted;
  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async => {
    for (final permission in permissions) permission: PermissionStatus.granted,
  };
  @override
  Future<bool> openSettings() async => true;
}

// Controlled external SDK terminal fault: its retired native discovery owner
// cannot deliver again until public deinitialize/initialize replaces it. This
// proves app recovery/fencing, not the cause of a physical Android/iOS failure.
class _Sdk extends RebootReplyBle {
  final statuses = [StreamController<BleStatus>.broadcast(sync: true)];
  final scans = <StreamController<DiscoveredDevice>>[];
  final events = <String>[];
  Completer<void>? heldCancellation;
  final cancellationStarted = Completer<void>();
  bool poisoned = false;
  int deinitializations = 0;
  int initializations = 0;
  Completer<void>? heldConnectionCancellation;
  final connectionCancellationStarted = Completer<void>();
  @override
  Stream<BleStatus> get statusStream {
    final source = statuses.last;
    return Stream<BleStatus>.multi((sink) {
      sink.add(BleStatus.ready);
      final sub = source.stream.listen(
        sink.add,
        onError: sink.addError,
        onDone: sink.close,
      );
      sink.onCancel = sub.cancel;
    }, isBroadcast: true);
  }

  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) {
    events.add('scan');
    final scan = StreamController<DiscoveredDevice>(
      sync: true,
      onCancel: () {
        events.add('scan_cancel');
        if (!cancellationStarted.isCompleted) cancellationStarted.complete();
        return heldCancellation?.future;
      },
    );
    scans.add(scan);
    return scan.stream;
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    events.add('connect');
    final connection = StreamController<ConnectionStateUpdate>(
      sync: true,
      onCancel: () async {
        events.add('connection_cancel');
        if (!connectionCancellationStarted.isCompleted) {
          connectionCancellationStarted.complete();
        }
        await heldConnectionCancellation?.future;
        events.add('connection_cancel_complete');
      },
    );
    sessionActivated = false;
    statusSequence = 0;
    controls.clear();
    connections.add(connection);
    return connection.stream;
  }

  @override
  Future<void> deinitialize() async {
    events.add('deinitialize');
    deinitializations++;
  }

  @override
  Future<void> initialize() async {
    events.add('initialize');
    initializations++;
    poisoned = false;
    statuses.add(StreamController<BleStatus>.broadcast(sync: true));
  }

  DiscoveredDevice board(String id) => DiscoveredDevice(
    id: id,
    name: 'Foxie Litch',
    serviceUuids: [BleProtocol.baseService],
    serviceData: const {},
    manufacturerData: Uint8List(0),
    rssi: -40,
    connectable: Connectable.available,
  );
  void advertise(String id) {
    if (!poisoned) scans.last.add(board(id));
  }

  Future<void> close() async {
    for (final connection in connections) {
      if (!connection.isClosed) await connection.close();
    }
    await packets.close();
    await sessionPackets.close();
    for (final source in statuses) {
      if (!source.isClosed) await source.close();
    }
    for (final source in scans) {
      if (!source.isClosed) await source.close();
    }
  }
}

Future<void> _until(bool Function() done) async {
  for (var turn = 0; turn < 128 && !done(); turn++) {
    await Future<void>.delayed(Duration.zero);
  }
  expect(done(), isTrue);
}

void main() {
  test(
    'status failure on a live shared link waits for native connection cancellation before SDK replacement',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final sdk = _Sdk()..dropFirstVersionReply = false;
      final service = LitchBleService(
        ble: sdk,
        pendingOtaStore: MemoryOtaTargetStore(),
        permissionGateway: _Permissions(),
      );
      final release = Completer<void>();
      try {
        await service.startScan();
        await connectBoard(service, sdk);
        final session = await service.openConnectionSession('board-1');
        expect(session?.isActivated, isTrue);
        final retiredConnection = sdk.connections.single;
        final retiredStatus = sdk.statuses.single;
        final retiredToken = service.connectionSessionToken;
        sdk.heldConnectionCancellation = release;
        retiredStatus.addError(StateError('controlled native status failure'));
        await Future<void>.delayed(Duration.zero);
        final replacement = service.connect('board-1');
        expect(await service.connect('board-1'), isFalse);
        await _until(
          () =>
              sdk.deinitializations != 0 ||
              sdk.connectionCancellationStarted.isCompleted,
        );
        expect(
          sdk.deinitializations,
          0,
          reason: 'An active native connection must finish cancellation first.',
        );
        expect(sdk.initializations, 0);
        expect(sdk.connections, hasLength(1));
        expect(service.isConnected, isFalse);
        expect(service.productReady, isFalse);
        retiredConnection.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        retiredStatus.add(BleStatus.poweredOff);
        await Future<void>.delayed(Duration.zero);
        expect(service.isConnected, isFalse);
        release.complete();
        await _until(() => sdk.connections.length == 2);
        expect(sdk.deinitializations, 1);
        expect(sdk.initializations, 1);
        expect(
          sdk.events.indexOf('connection_cancel_complete'),
          lessThan(sdk.events.indexOf('deinitialize')),
        );
        sdk.connections.last.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await replacement, isTrue);
        expect(service.connectionSessionToken, greaterThan(retiredToken));
        final currentToken = service.connectionSessionToken;
        final currentSession = await service.openConnectionSession('board-1');
        expect(currentSession?.isActivated, isTrue);
        retiredConnection.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        retiredStatus.add(BleStatus.poweredOff);
        await Future<void>.delayed(Duration.zero);
        expect(service.isConnected, isTrue);
        expect(service.connectedId, 'board-1');
        expect(service.connectionSessionToken, currentToken);
        expect(service.bleAvailability, BleAvailability.ready);
      } finally {
        if (!release.isCompleted) release.complete();
        await service.dispose();
        await sdk.close();
        debugDefaultTargetPlatformOverride = null;
      }
    },
  );
  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final failedScan in [true, false]) {
      test(
        '$platform replaces failed ${failedScan ? 'scan' : 'status'} owner after cleanup and rejects retired callbacks',
        () async {
          debugDefaultTargetPlatformOverride = platform;
          var now = DateTime(2026);
          final sdk = _Sdk();
          final service = LitchBleService(
            ble: sdk,
            pendingOtaStore: MemoryOtaTargetStore(),
            permissionGateway: _Permissions(),
            scanClock: () => now,
          );
          final release = Completer<void>();
          sdk.heldCancellation = release;
          try {
            await service.startScan();
            expect(sdk.scans, hasLength(1));
            final retiredScan = sdk.scans.single;
            final retiredStatus = sdk.statuses.single;
            sdk.poisoned = true;
            if (failedScan) {
              retiredScan.addError(
                StateError('controlled native discovery owner failure'),
              );
            } else {
              retiredStatus.addError(
                StateError('controlled native status owner failure'),
              );
            }
            await Future<void>.delayed(Duration.zero);
            now = now.add(const Duration(seconds: 2));
            final recovery = service.startScan();
            await _until(() => sdk.cancellationStarted.isCompleted);
            expect(
              sdk.deinitializations,
              0,
              reason: 'Fresh setup waits for native scan cancellation.',
            );
            expect(sdk.scans, hasLength(1));
            retiredScan.add(sdk.board('stale-before-reset'));
            release.complete();
            await recovery;
            expect(sdk.events, [
              'scan',
              'scan_cancel',
              'deinitialize',
              'initialize',
              'scan',
            ]);
            expect(sdk.deinitializations, 1);
            expect(sdk.initializations, 1);
            expect(service.discoveredBoard('stale-before-reset'), isNull);
            retiredScan.add(sdk.board('stale-after-reset'));
            if (!retiredStatus.isClosed) {
              retiredStatus.add(BleStatus.poweredOff);
            }
            await Future<void>.delayed(Duration.zero);
            expect(service.bleAvailability, BleAvailability.ready);
            expect(service.discoveredBoard('stale-after-reset'), isNull);
            sdk.advertise('fresh-board');
            expect(service.discoveredBoard('fresh-board'), isNotNull);
            now = now.add(const Duration(seconds: 1));
            await service.startScan();
            expect(
              sdk.deinitializations,
              1,
              reason: 'Healthy discovery does not reset the SDK.',
            );
          } finally {
            if (!release.isCompleted) release.complete();
            await service.dispose();
            await sdk.close();
            debugDefaultTargetPlatformOverride = null;
          }
        },
      );
    }
  }
}
