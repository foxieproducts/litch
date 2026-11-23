import 'dart:async';
import 'dart:ui' as ui;
import 'package:flutter/services.dart';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

class _Permissions implements BlePermissionGateway {
  Completer<PermissionStatus>? statusGate;
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      await statusGate?.future ?? PermissionStatus.granted;
  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async => {
    for (final permission in permissions) permission: PermissionStatus.granted,
  };
  @override
  Future<bool> openSettings() async => true;
}

class _Trace implements AppTraceSink {
  final events = <(String, Map<String, Object?>)>[];
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) =>
      events.add((event, Map.of(fields)));
}

// Only native connection/notification setup and board proof are supplied.
// Gate, service, session negotiation and owner admission are production owners.
class _SetupBoard extends OwnerPairingBoard {
  _SetupBoard(this.failedCharacteristic) {
    state = 6;
    flags = 5; // Saved owner awaiting quiet proof, with no new-claim admission.
  }
  final Uuid failedCharacteristic;
  bool failNotificationSetup = true;
  final scan = StreamController<DiscoveredDevice>.broadcast(sync: true);
  final cleanupStarted = Completer<void>();
  final cleanupFinished = Completer<void>();
  final failedStreams = <StreamController<List<int>>>[];
  final heldCapabilityWrite = Completer<void>();
  bool capabilityWriteEntered = false;
  String capabilityFailure = 'write';
  int cacheRefreshes = 0;
  bool cachedRecoveryInventory = false;
  final cacheStarted = Completer<void>();
  final cacheFinished = Completer<void>();
  final nativeEvents = <String>[];
  List<Service> discoveredServices = [];
  Completer<void>? discoveryGate;
  int discoveryRequests = 0;
  final heldIngressWrite = Completer<void>();
  int heldIngressWrites = 0;

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    if (connections.length == 1 &&
        capabilityFailure == 'activation' &&
        c.characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic &&
        ConnectionSessionCodec.decode(Uint8List.fromList(value))?.code ==
            ConnectionSessionControl.hello) {
      throw TimeoutException('controlled exhausted session opening');
    }
    if (c.characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic &&
        ConnectionSessionCodec.decode(Uint8List.fromList(value))?.token == 77) {
      heldIngressWrites++;
      await heldIngressWrite.future;
      return;
    }
    await super.writeCharacteristicWithoutResponse(c, value: value);
  }

  Object get notificationError => Exception(
    GenericFailure<CharacteristicValueUpdateError>(
      code: CharacteristicValueUpdateError.unknown,
      message:
          'Cannot write client characteristic config descriptor (code 3) '
          'with characteristic UUID $failedCharacteristic',
    ),
  );

  @override
  Stream<BleStatus> get statusStream => Stream.value(BleStatus.ready);
  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) => scan.stream;

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    final first = connections.isEmpty;
    final connection = StreamController<ConnectionStateUpdate>(
      sync: true,
      onCancel: () {
        if (!first) return Future<void>.value();
        nativeEvents.add('native_cancel');
        if (!cleanupStarted.isCompleted) cleanupStarted.complete();
        return cleanupFinished.future;
      },
    );
    connections.add(connection);
    sessionActivated = false;
    statusSequence = 0;
    controls.clear();
    return connection.stream;
  }

  @override
  Future<void> clearGattCache(String deviceId) async {
    cacheRefreshes++;
    nativeEvents.add('cache_start');
    if (!cacheStarted.isCompleted) cacheStarted.complete();
    if (cachedRecoveryInventory) {
      await cacheFinished.future;
      cachedRecoveryInventory = false;
    }
    nativeEvents.add('cache_finish');
  }

  @override
  Future<List<Service>> getDiscoveredServices(String deviceId) async {
    discoveryRequests++;
    await discoveryGate?.future;
    return discoveredServices = [
      _SdkService(this, deviceId, BleProtocol.baseService, [
        if (!cachedRecoveryInventory) ...[
          BleProtocol.commandCharacteristic,
          BleProtocol.consoleStreamCharacteristic,
        ],
        BleProtocol.connectionSessionIngressCharacteristic,
        BleProtocol.connectionSessionEgressCharacteristic,
        BleProtocol.otaControlCharacteristic,
        BleProtocol.otaDataCharacteristic,
      ]),
      if (!cachedRecoveryInventory)
        _SdkService(this, deviceId, BleProtocol.settingsService, [
          BleProtocol.settingsEnumerateCharacteristic,
          BleProtocol.settingsReadWriteCharacteristic,
        ]),
    ];
  }

  @override
  Stream<List<int>> subscribeToCharacteristic(QualifiedCharacteristic c) {
    if (failNotificationSetup &&
        capabilityFailure != 'activation' &&
        connections.length == 1 &&
        c.characteristicId == failedCharacteristic) {
      late final StreamController<List<int>> stream;
      stream = StreamController<List<int>>.broadcast(
        sync: true,
        onListen: () {
          if (capabilityFailure == 'early close') {
            scheduleMicrotask(stream.close);
          } else if (failedCharacteristic !=
                  BleProtocol.otaControlCharacteristic ||
              capabilityFailure == 'early error') {
            scheduleMicrotask(() => stream.addError(notificationError));
          }
        },
      );
      failedStreams.add(stream);
      return stream.stream;
    }
    return super.subscribeToCharacteristic(c);
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    if (connections.length == 1 &&
        failedCharacteristic == BleProtocol.otaControlCharacteristic &&
        capabilityFailure == 'write' &&
        c.characteristicId == failedCharacteristic) {
      capabilityWriteEntered = true;
      scheduleMicrotask(() => failedStreams.last.addError(notificationError));
      await heldCapabilityWrite.future;
      return;
    }
    await super.writeCharacteristicWithResponse(c, value: value);
  }

  void advertise() => scan.add(
    DiscoveredDevice(
      id: 'board-1',
      name: 'Foxie Litch',
      serviceUuids: [BleProtocol.baseService],
      serviceData: const {},
      manufacturerData: Uint8List(0),
      rssi: -40,
      connectable: Connectable.available,
    ),
  );
  void connected(int index) => connections[index].add(
    const ConnectionStateUpdate(
      deviceId: 'board-1',
      connectionState: DeviceConnectionState.connected,
      failure: null,
    ),
  );
}

class _GattFactory implements LitchGattSessionFactory {
  _GattFactory(this.board, {this.batch = true});
  final _SetupBoard board;
  final bool batch;
  int opens = 0;
  @override
  Future<LitchGattSession> open(String deviceId) async {
    opens++;
    final session = await ReactiveBleGattSessionFactory(board).open(deviceId);
    return batch ? session : _NoBatchGatt(session);
  }
}

// Exercise the supported non-batch SDK surface with the same real inventory
// validation and exact handles; no transport or board behavior is replaced.
class _NoBatchGatt implements LitchGattSession, LitchGattInventory {
  _NoBatchGatt(this.session);
  final LitchGattSession session;
  @override
  bool hasCharacteristic(Uuid serviceId, Uuid characteristicId) =>
      (session as LitchGattInventory).hasCharacteristic(
        serviceId,
        characteristicId,
      );
  @override
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) => session.read(serviceId: serviceId, characteristicId: characteristicId);
  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) => session.write(
    serviceId: serviceId,
    characteristicId: characteristicId,
    value: value,
    withResponse: withResponse,
  );
  @override
  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) => session.subscribe(
    serviceId: serviceId,
    characteristicId: characteristicId,
  );
}

// External SDK handles only. The production GATT factory validates the actual
// cached Recovery/normal inventories and resolves these exact instances.
class _SdkService implements Service {
  _SdkService(this.board, this.deviceId, this.id, List<Uuid> ids) {
    characteristics = [for (final id in ids) _SdkCharacteristic(this, id)];
  }
  final _SetupBoard board;
  @override
  final String deviceId;
  @override
  final Uuid id;
  @override
  late final List<Characteristic> characteristics;
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _SdkCharacteristic implements Characteristic {
  _SdkCharacteristic(this.service, this.id);
  @override
  final _SdkService service;
  @override
  final Uuid id;
  QualifiedCharacteristic get address => QualifiedCharacteristic(
    serviceId: service.id,
    characteristicId: id,
    deviceId: service.deviceId,
  );
  @override
  Stream<List<int>> subscribe() =>
      service.board.subscribeToCharacteristic(address);
  @override
  Future<List<int>> read() => service.board.readCharacteristic(address);
  @override
  Future<void> write(List<int> value, {bool withResponse = false}) =>
      withResponse
      ? service.board.writeCharacteristicWithResponse(address, value: value)
      : service.board.writeCharacteristicWithoutResponse(address, value: value);
  @override
  Future<void> writeBatchWithoutResponse(List<List<int>> values) async {
    for (final value in values) {
      await write(value);
    }
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

Future<void> _turns(
  WidgetTester tester,
  bool Function() done, {
  int maxTurns = 80,
}) async {
  for (var turn = 0; turn < maxTurns && !done(); turn++) {
    await tester.pump(const Duration(milliseconds: 20));
    await tester.runAsync(() async {});
  }
  expect(done(), isTrue);
}

void main() {
  for (final (beforeReady, manualCancel, landscape) in [
    (true, false, false),
    (false, false, false),
    (true, true, false),
    (true, false, true),
  ]) {
    testWidgets(
      landscape
          ? 'LandscapePreReadyHelpJourney SDK early drop exposes one Help button to semantics and keyboard at200percent'
          : manualCancel
          ? 'PreReadyManualDisconnectJourney explicit Cancel does not report an unexpected connection end'
          : beforeReady
          ? 'PreReadyConnectionEndedJourney SDK forty millisecond drop keeps conditional one-phone help through retry and current owner proof'
          : 'ReadyConnectionLostJourney SDK ready link loss retains ordinary reconnect guidance',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'last_connected_board_id': 'board-1',
        });
        final board = _SetupBoard(BleProtocol.settingsEnumerateCharacteristic);
        board.cleanupFinished.complete();
        board.failNotificationSetup = false;
        final permissions = _Permissions();
        if (beforeReady) board.discoveryGate = Completer<void>();
        final gatt = _GattFactory(board);
        final service = LitchBleService(
          ble: board,
          gattSessionFactory: gatt,
          permissionGateway: permissions,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        final semantics = landscape ? tester.ensureSemantics() : null;
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize =
            landscape ? const Size(844, 390) : const Size(390, 844);
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        try {
          await tester.pumpWidget(
            MaterialApp(
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(beforeReady ? 2 : 1)),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: service,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox(),
              ),
            ),
          );
          await _turns(tester, () => board.scan.hasListener);
          board.advertise();
          await _turns(tester, () => board.connections.isNotEmpty);
          board.connections[0].add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connecting,
              failure: null,
            ),
          );
          board.connected(0);
          await tester.pump(const Duration(milliseconds: 40));
          if (beforeReady) {
            expect(board.discoveryRequests, 1);
            expect(service.productReady, isFalse);
            expect(board.productQueries, isEmpty);
          } else {
            await _turns(tester, () => board.sessionActivated);
            board.setState(5, nextFlags: 7);
            await _turns(tester, () => service.productReady);
          }
          if (manualCancel) {
            await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            await _turns(tester, () => !service.isConnected);
            expect(
              find.textContaining('Connection ended before setup finished.'),
              findsNothing,
            );
            expect(board.connections, hasLength(1));
            expect(board.pairingCommands, isEmpty);
            return;
          }
          if (beforeReady) {
            permissions.statusGate = Completer<PermissionStatus>();
          }
          board.connections[0].add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: GenericFailure<ConnectionError>(
                code: ConnectionError.failedToConnect,
                message: 'GATT connection ended, status 19',
              ),
            ),
          );
          await tester.pump();
          expect(service.isConnected, isFalse);
          final panel = find.byKey(
            const ValueKey('connection-progress-overlay'),
          );
          final hint = find.descendant(
            of: panel,
            matching: find.textContaining(
              'Connection ended before setup finished.',
            ),
          );
          if (!beforeReady) {
            expect(hint, findsNothing);
            expect(
              find.text(
                'Connection lost. Waiting for previous board to reappear...',
              ),
              findsOneWidget,
            );
            return;
          }
          final original = tester.element(panel);
          expect(hint, findsOneWidget);
          expect(
            find.descendant(
              of: panel,
              matching: find.textContaining(
                'If another device is connected, open Litch on that device and tap Disconnect.',
              ),
            ),
            findsOneWidget,
          );
          expect(
            find.textContaining('belongs to another device'),
            findsNothing,
          );
          expect(board.pairingCommands, isEmpty);
          await tester.pump(const Duration(seconds: 1));
          expect(tester.element(panel), same(original));
          expect(hint, findsOneWidget);
          permissions.statusGate!.complete(PermissionStatus.granted);
          permissions.statusGate = null;
          await tester.pump(const Duration(seconds: 2));
          await _turns(tester, () => board.connections.length == 2);
          expect(tester.element(panel), same(original));
          expect(hint, findsOneWidget);
          expect(
            board.connections,
            hasLength(2),
            reason: 'Retry does not require another advertisement.',
          );
          final help = find.byKey(const ValueKey('owner-pairing-support'));
          await tester.ensureVisible(help);
          await tester.pump();
          expect(tester.getSize(help).height, greaterThanOrEqualTo(48));
          if (landscape) {
            final node = tester.getSemantics(help);
            expect(node.getSemanticsData().flagsCollection.isButton, isTrue);
            expect(
              node.getSemanticsData().hasAction(ui.SemanticsAction.tap),
              isTrue,
            );
            bool helpFocused() {
              var found = false;
              FocusManager.instance.primaryFocus?.context
                  ?.visitAncestorElements((element) {
                    if (element == tester.element(help)) found = true;
                    return !found;
                  });
              return found;
            }

            FocusManager.instance.primaryFocus?.unfocus();
            for (var turn = 0; turn < 12 && !helpFocused(); turn++) {
              await tester.sendKeyEvent(LogicalKeyboardKey.tab);
              await tester.pump();
            }
            expect(
              helpFocused(),
              isTrue,
              reason: 'Help remains in the keyboard focus order',
            );
          }
          await tester.tap(help);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          expect(find.byType(SupportReportScreen), findsOneWidget);
          await tester.pump(const Duration(seconds: 3));
          expect(board.connections, hasLength(2));
          await tester.pageBack();
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          expect(panel, findsNothing);
          board.discoveryGate!.complete();
          board.discoveryGate = null;
          await _turns(tester, () => board.scan.hasListener);
          board.advertise();
          await tester.pump();
          final connect = find.widgetWithText(FilledButton, 'Connect');
          await _turns(
            tester,
            () => connect.hitTestable().evaluate().isNotEmpty,
          );
          await tester.tap(connect);
          await _turns(tester, () => board.connections.length == 3);
          board.connected(2);
          await _turns(tester, () => board.sessionActivated);
          expect(service.productReady, isFalse);
          board.setState(5, nextFlags: 7);
          await _turns(tester, () => service.productReady);
          expect(hint, findsNothing);
          expect(board.pairingCommands, isEmpty);
        } finally {
          if (permissions.statusGate case final gate? when !gate.isCompleted) {
            gate.complete(PermissionStatus.granted);
          }
          if (board.discoveryGate case final gate? when !gate.isCompleted) {
            gate.complete();
          }
          await tester.pumpWidget(const SizedBox.shrink());
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          await _turns(tester, () => disposed);
          await disposal;
          await tester.runAsync(() async {
            await disposeBoard(service, board);
            await board.scan.close();
          });
          semantics?.dispose();
        }
        expect(tester.takeException(), isNull);
      },
      timeout: const Timeout(Duration(seconds: 20)),
    );
  }

  testWidgets(
    'native setup retries without a final SDK callback or new advertisement',
    (tester) async {
      SharedPreferences.setMockInitialValues({
        'last_connected_board_id': 'board-1',
      });
      final board = _SetupBoard(
        BleProtocol.connectionSessionEgressCharacteristic,
      );
      final service = LitchBleService(
        ble: board,
        gattSessionFactory: _GattFactory(board),
        permissionGateway: _Permissions(),
        pendingOtaStore: MemoryOtaTargetStore(),
        scanClock: tester.binding.clock.now,
      );
      final updates = <ConnectionStateUpdate>[];
      final observation = service.connectionStream.listen(updates.add);
      try {
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await _turns(tester, () => board.scan.hasListener);
        board.advertise();
        await _turns(tester, () => board.connections.isNotEmpty);
        board.connected(0);
        final failedToken = service.connectionSessionToken;
        await _turns(tester, () => board.cleanupStarted.isCompleted);
        expect(service.isConnected, isFalse);
        expect(service.productReady, isFalse);
        expect(service.connectionSessionToken, isNot(failedToken));
        expect(
          updates.where(
            (update) =>
                update.connectionState == DeviceConnectionState.disconnected,
          ),
          hasLength(1),
          reason:
              'The service publishes retirement before cancelling; the cancelled SDK stream emits no disconnected callback.',
        );
        final panel = find.byKey(const ValueKey('connection-progress-overlay'));
        final original = tester.element(panel);
        board.cleanupFinished.complete();
        await tester.pump(const Duration(seconds: 3));
        await _turns(tester, () => board.connections.length == 2);
        expect(board.nativeEvents, ['native_cancel']);
        expect(
          board.connections,
          hasLength(2),
          reason: 'Retry needs no new advertisement.',
        );
        expect(tester.element(panel), same(original));
        expect(
          find.byKey(const ValueKey('owner-pairing-support')).hitTestable(),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey('chosen-board-cancel')).hitTestable(),
          findsOneWidget,
        );
        expect(board.productQueries, isEmpty);
        expect(board.pairingCommands, isEmpty);
        // A late native event from the retired stream cannot end the new attempt.
        final retryToken = service.connectionSessionToken;
        board.connections[0].add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(service.connectionSessionToken, retryToken);
        expect(board.connections, hasLength(2));
        board.connected(1);
        await _turns(tester, () => board.sessionActivated);
        board.setState(5, nextFlags: 7);
        await _turns(tester, () => service.productReady);
        // The async owner proof can complete after the preceding frame.
        await tester.pump();
        expect(service.connectedId, 'board-1');
        expect(service.connectionSessionToken, isNot(failedToken));
        expect(find.text('Connecting to Foxie Litch...'), findsNothing);
      } finally {
        unawaited(observation.cancel());
        if (!board.cleanupFinished.isCompleted) {
          board.cleanupFinished.complete();
        }
        if (!board.heldCapabilityWrite.isCompleted) {
          board.heldCapabilityWrite.complete();
        }
        await tester.pumpWidget(const SizedBox.shrink());
        var disposed = false;
        final disposal = service.dispose().whenComplete(() => disposed = true);
        await _turns(tester, () => disposed);
        await disposal;
        await tester.runAsync(() async {
          await disposeBoard(service, board);
          await board.scan.close();
          for (final stream in board.failedStreams) {
            if (!stream.isClosed) await stream.close();
          }
        });
      }
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  for (final (label, endpoint, failure) in [
    (
      'shared-session activation',
      BleProtocol.connectionSessionEgressCharacteristic,
      'activation',
    ),
    ('capability', BleProtocol.otaControlCharacteristic, 'write'),
    (
      'capability early error',
      BleProtocol.otaControlCharacteristic,
      'early error',
    ),
    (
      'capability early close',
      BleProtocol.otaControlCharacteristic,
      'early close',
    ),
  ]) {
    testWidgets(
      '$label setup failure retires before idle timeout and reconnects safely',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'last_connected_board_id': 'board-1',
        });
        final board = _SetupBoard(endpoint)..capabilityFailure = failure;
        final isCapability = endpoint == BleProtocol.otaControlCharacteristic;
        final gatt = _GattFactory(board);
        final trace = _Trace();
        final service = LitchBleService(
          ble: board,
          gattSessionFactory: gatt,
          permissionGateway: _Permissions(),
          pendingOtaStore: MemoryOtaTargetStore(),
          appTrace: trace,
        );
        try {
          await tester.pumpWidget(
            MaterialApp(home: ConnectionGateScreen(bleService: service)),
          );
          await _turns(tester, () => board.scan.hasListener);
          board.advertise();
          await _turns(tester, () => board.connections.isNotEmpty);
          board.connected(0);
          final oldToken = service.connectionSessionToken;
          await _turns(tester, () => board.cleanupStarted.isCompleted);
          expect(
            service.isConnected,
            isFalse,
            reason:
                'A failed initial notification must not wait for the board 12s idle grace.',
          );
          expect(board.pairingCommands, isEmpty);
          expect(board.productQueries, isEmpty);
          expect(
            board.cacheRefreshes,
            0,
            reason: 'Notification failure is not missing GATT inventory.',
          );
          expect(
            trace.events.any(
              (event) =>
                  event.$1 == 'ble.connection_preparation_failed' &&
                  event.$2['stage'] ==
                      (failure == 'activation'
                          ? 'session_activation'
                          : '${isCapability ? 'capability' : 'session'}_notification'),
            ),
            isTrue,
          );
          if (isCapability) {
            expect(board.capabilityWriteEntered, failure == 'write');
            expect(board.heldCapabilityWrite.isCompleted, isFalse);
            expect(
              trace.events.any(
                (event) =>
                    event.$1 == 'ble.capability_result' &&
                    event.$2['outcome'] == 'notification_failed',
              ),
              isTrue,
            );
          }
          await _turns(tester, () => board.scan.hasListener);
          board.advertise();
          await tester.pump(const Duration(seconds: 1));
          expect(
            board.connections,
            hasLength(1),
            reason: 'New native admission waits for old cleanup.',
          );
          board.cleanupFinished.complete();
          // The selected-board retry runs at 3s, after the native cleanup barrier.
          await _turns(
            tester,
            () => board.connections.length == 2,
            maxTurns: 200,
          );
          board.connected(1);
          await _turns(tester, () => board.sessionActivated);
          expect(service.connectionSessionToken, isNot(oldToken));
          final newToken = service.connectionSessionToken;
          if (!board.heldCapabilityWrite.isCompleted) {
            board.heldCapabilityWrite.complete();
          }
          expect(service.productReady, isFalse);
          expect(board.pairingCommands, isEmpty);
          for (final stream in board.failedStreams) {
            if (stream.isClosed) continue;
            stream.add(
              ConnectionSessionCodec.encode(
                ConnectionSessionPacket(
                  kind: ConnectionSessionKind.state,
                  code: 40,
                  token: 0x80000001,
                  sequence: 99,
                  body: Uint8List.fromList([1, 5, 0, 7, 0, 0, 0, 0]),
                ),
              ),
            );
          }
          await tester.pump();
          expect(
            service.productReady,
            isFalse,
            reason: 'Old native callbacks cannot grant new owner authority.',
          );
          expect(service.connectionSessionToken, newToken);
          board.setState(5, nextFlags: 7);
          await _turns(tester, () => service.productReady);
          expect(service.connectedId, 'board-1');
          expect(gatt.opens, 2);
          expect(board.pairingCommands, isEmpty);
        } finally {
          if (!board.cleanupFinished.isCompleted) {
            board.cleanupFinished.complete();
          }
          if (!board.heldCapabilityWrite.isCompleted) {
            board.heldCapabilityWrite.complete();
          }
          await tester.pumpWidget(const SizedBox.shrink());
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          await _turns(tester, () => disposed);
          await disposal;
          await tester.runAsync(() async {
            await disposeBoard(service, board);
            await board.scan.close();
            for (final stream in board.failedStreams) {
              if (!stream.isClosed) await stream.close();
            }
          });
        }
      },
      timeout: const Timeout(Duration(seconds: 15)),
    );
  }

  for (final batch in [false, true]) {
    testWidgets(
      'quarantine blocks queued ${batch ? 'compatibility batch' : 'single'} ingress at native admission',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'last_connected_board_id': 'board-1',
        });
        final board = _SetupBoard(BleProtocol.otaDataCharacteristic);
        final service = LitchBleService(
          ble: board,
          gattSessionFactory: _GattFactory(board, batch: !batch),
          permissionGateway: _Permissions(),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        Future<Object?> outcome(Future<void> operation) async {
          try {
            await operation;
            return null;
          } catch (error) {
            return error;
          }
        }

        Uint8List packet(int sequence) => ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.control,
            code: ConnectionSessionControl.query,
            token: 77,
            sequence: sequence,
            body: Uint8List(0),
          ),
        );
        try {
          await tester.pumpWidget(
            MaterialApp(home: ConnectionGateScreen(bleService: service)),
          );
          await _turns(tester, () => board.scan.hasListener);
          board.advertise();
          await _turns(tester, () => board.connections.isNotEmpty);
          board.connected(0);
          await _turns(tester, () => board.sessionActivated);
          board.setState(5, nextFlags: 7);
          await _turns(tester, () => service.productReady);
          final first = outcome(
            service.writeConnectionSessionPacket('board-1', packet(1)),
          );
          await _turns(tester, () => board.heldIngressWrites == 1);
          bool secondDone = false;
          final second = outcome(
            batch
                ? service.writeConnectionSessionPackets('board-1', [
                    packet(2),
                    packet(3),
                  ])
                : service.writeConnectionSessionPacket('board-1', packet(2)),
          ).whenComplete(() => secondDone = true);
          await tester.pump(const Duration(milliseconds: 100));
          expect(board.heldIngressWrites, 1);
          board.cachedRecoveryInventory = true;
          board.discoveredServices.first.characteristics.removeWhere(
            (c) => c.id == BleProtocol.connectionSessionEgressCharacteristic,
          );
          final trigger = outcome(
            service.connectionSessionPackets('board-1').drain<void>(),
          );
          await _turns(tester, () => board.cacheStarted.isCompleted);
          expect(await trigger, isA<LitchGattDiscoveryIncomplete>());
          board.heldIngressWrite.complete();
          await _turns(tester, () => secondDone);
          expect(
            board.heldIngressWrites,
            1,
            reason:
                'Queued work cannot enter the SDK while live-handle cache repair owns this generation.',
          );
          expect(await second, isA<LitchGattDiscoveryIncomplete>());
          await first;
          expect(board.cleanupStarted.isCompleted, isFalse);
          expect(service.productReady, isFalse);
        } finally {
          if (!board.heldIngressWrite.isCompleted) {
            board.heldIngressWrite.complete();
          }
          if (!board.cacheFinished.isCompleted) board.cacheFinished.complete();
          if (!board.cleanupFinished.isCompleted) {
            board.cleanupFinished.complete();
          }
          await tester.pumpWidget(const SizedBox.shrink());
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          await _turns(tester, () => disposed);
          await disposal;
          await tester.runAsync(() async {
            await disposeBoard(service, board);
            await board.scan.close();
          });
        }
      },
      timeout: const Timeout(Duration(seconds: 15)),
    );
  }

  testWidgets(
    'cached Recovery inventory on a normal board repairs before native cleanup',
    (tester) async {
      SharedPreferences.setMockInitialValues({
        'last_connected_board_id': 'board-1',
      });
      final board = _SetupBoard(BleProtocol.otaDataCharacteristic)
        ..cachedRecoveryInventory = true;
      final gatt = _GattFactory(board);
      final service = LitchBleService(
        ble: board,
        gattSessionFactory: gatt,
        permissionGateway: _Permissions(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      try {
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await _turns(tester, () => board.scan.hasListener);
        board.advertise();
        await _turns(tester, () => board.connections.isNotEmpty);
        board.connected(0);
        final oldToken = service.connectionSessionToken;
        await _turns(tester, () => board.cacheStarted.isCompleted);
        await tester.pump(const Duration(milliseconds: 500));
        expect(
          board.cleanupStarted.isCompleted,
          isFalse,
          reason: 'Cache refresh needs the still-valid native connection.',
        );
        expect(service.productReady, isFalse);
        expect(board.cacheRefreshes, 1);
        expect(board.pairingCommands, isEmpty);
        expect(board.productQueries, isEmpty);
        board.cacheFinished.complete();
        await _turns(tester, () => board.cleanupStarted.isCompleted);
        expect(board.nativeEvents, [
          'cache_start',
          'cache_finish',
          'native_cancel',
        ]);
        await _turns(tester, () => board.scan.hasListener);
        board.advertise();
        await tester.pump(const Duration(milliseconds: 500));
        expect(board.connections, hasLength(1));
        board.cleanupFinished.complete();
        // Keep cleanup ordered while allowing the existing 3s selected-board retry.
        await _turns(
          tester,
          () => board.connections.length == 2,
          maxTurns: 200,
        );
        board.connected(1);
        await _turns(tester, () => board.sessionActivated);
        expect(service.connectionSessionToken, isNot(oldToken));
        expect(service.productReady, isFalse);
        board.setState(5, nextFlags: 7);
        await _turns(tester, () => service.productReady);
        await tester.pump(const Duration(seconds: 2));
        expect(board.connections, hasLength(2));
        expect(board.cacheRefreshes, 1);
        expect(gatt.opens, 2);
        expect(board.pairingCommands, isEmpty);
      } finally {
        if (!board.cacheFinished.isCompleted) {
          board.cacheFinished.complete();
        }
        if (!board.cleanupFinished.isCompleted) {
          board.cleanupFinished.complete();
        }
        if (!board.heldCapabilityWrite.isCompleted) {
          board.heldCapabilityWrite.complete();
        }
        await tester.pumpWidget(const SizedBox.shrink());
        var disposed = false;
        final disposal = service.dispose().whenComplete(() => disposed = true);
        await _turns(tester, () => disposed);
        await disposal;
        await tester.runAsync(() async {
          await disposeBoard(service, board);
          await board.scan.close();
        });
      }
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  testWidgets(
    'stalled genuine GATT refresh is bounded and cannot expose product controls',
    (tester) async {
      SharedPreferences.setMockInitialValues({
        'last_connected_board_id': 'board-1',
      });
      final board = _SetupBoard(BleProtocol.otaDataCharacteristic)
        ..cachedRecoveryInventory = true;
      final service = LitchBleService(
        ble: board,
        gattSessionFactory: _GattFactory(board),
        permissionGateway: _Permissions(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      try {
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await _turns(tester, () => board.scan.hasListener);
        board.advertise();
        await _turns(tester, () => board.connections.isNotEmpty);
        board.connected(0);
        await _turns(tester, () => board.cacheStarted.isCompleted);
        expect(service.productReady, isFalse);
        expect(board.cleanupStarted.isCompleted, isFalse);
        await tester.pump(const Duration(seconds: 3));
        await _turns(tester, () => board.cleanupStarted.isCompleted);
        expect(board.cacheFinished.isCompleted, isFalse);
        expect(board.cacheRefreshes, 1);
        expect(service.productReady, isFalse);
        expect(board.pairingCommands, isEmpty);
        expect(board.productQueries, isEmpty);
      } finally {
        if (!board.cacheFinished.isCompleted) {
          board.cacheFinished.complete();
        }
        if (!board.cleanupFinished.isCompleted) {
          board.cleanupFinished.complete();
        }
        if (!board.heldCapabilityWrite.isCompleted) {
          board.heldCapabilityWrite.complete();
        }
        await tester.pumpWidget(const SizedBox.shrink());
        var disposed = false;
        final disposal = service.dispose().whenComplete(() => disposed = true);
        await _turns(tester, () => disposed);
        await disposal;
        await tester.runAsync(() async {
          await disposeBoard(service, board);
          await board.scan.close();
        });
      }
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  test(
    'capability waiter observes native notification errors separately from timeout',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final channel = LitchOtaControlChannel(
        deviceId: 'board-1',
        connectionToken: 1,
        generation: 1,
        subscribe: () => packets.stream,
      );
      final errors = <Object>[];
      try {
        final failed = channel.waitFor(
          (_) => true,
          timeout: const Duration(milliseconds: 20),
          onNotificationError: errors.add,
        );
        await Future<void>.delayed(Duration.zero);
        final nativeError = StateError('controlled CCCD setup failure');
        packets.addError(nativeError);
        expect(await failed, isNull);
        expect(errors, [nativeError]);
        await channel
            .start(); // Explicitly renew before testing ordinary silence.
        final silent = channel.waitFor(
          (_) => true,
          timeout: const Duration(milliseconds: 20),
          onNotificationError: errors.add,
        );
        expect(await silent, isNull);
        expect(errors, [
          nativeError,
        ], reason: 'Reply timeout is not a notification setup error.');
      } finally {
        await channel.dispose();
        await packets.close();
      }
    },
  );
}
