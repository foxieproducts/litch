import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/foundation.dart'
    show TargetPlatform, defaultTargetPlatform;
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';

import '../support/memory_ota_target_store.dart';
import 'native_process.dart';

class _Operation {
  _Operation(this.request, this.complete, this.fail);
  final Map<String, Object?> request;
  final void Function(Map<String, dynamic>) complete;
  final void Function(Object, StackTrace)? fail;
}

/// Keeps the SDK callback address so a test can deliberately deliver an old
/// callback after cancellation. Ordinary delivery still uses the real Stream.
class VirtualNotificationPipe extends Stream<List<int>> {
  VirtualNotificationPipe(this.onCancel) {
    _controller = StreamController<List<int>>(sync: true, onCancel: onCancel);
  }
  final void Function() onCancel;
  late final StreamController<List<int>> _controller;
  void Function(List<int>)? _callback;

  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) {
    _callback = onData;
    return _controller.stream.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    );
  }

  void deliver(List<int> bytes, {bool lateCallback = false}) {
    if (lateCallback) {
      _callback?.call(bytes);
    } else {
      _controller.add(bytes);
    }
  }

  void fail() => _controller.addError(StateError('scheduled SDK stream loss'));
  Future<void> close() => _controller.close();
}

class VirtualPacket {
  VirtualPacket(this.channel, this.bytes, this.epoch, this.pipe);
  final String channel;
  final List<int> bytes;
  final int epoch;
  final VirtualNotificationPipe? pipe;

  ConnectionSessionPacket? get sessionPacket => channel == 'connection_session'
      ? ConnectionSessionCodec.decode(Uint8List.fromList(bytes))
      : null;
}

extension VirtualSessionPacket on ConnectionSessionPacket {
  bool get isHomeState =>
      kind == ConnectionSessionKind.state &&
      code >= HomeStateWireCodec.firstStateCode &&
      code <= HomeStateWireCodec.lastStateCode;

  bool get isCheckpoint =>
      kind == ConnectionSessionKind.control &&
      code == ConnectionSessionControl.checkpoint;

  bool confirmsApplied(ConnectionSessionPacket request) {
    if (!isCheckpoint || request.token != token) return false;
    final offset = sequence - request.sequence;
    final exceptions = ByteData.sublistView(body).getUint16(8, Endian.little);
    return offset >= 0 && offset < 16 && (exceptions & (1 << offset)) == 0;
  }
}

/// SDK boundary only. Packet schedules hold/drop production formatted bytes;
/// they never manufacture an ACK, Home state, setting value, or engine result.
class VirtualLitchBle implements FlutterReactiveBle {
  VirtualLitchBle(
    this.native, {
    this.negotiatedMtu = 247,
    this.operationIntervalMs = 0,
    this.enableOtaControl = false,
  });
  final VirtualLitchProcess native;
  // Widget fixtures create this SDK in their fake-async body. Teardown may
  // invoke it from the external test invoker instead of that original zone.
  final Zone _testerCreationZone = Zone.current;
  final int negotiatedMtu;
  final int operationIntervalMs;
  final bool enableOtaControl;
  bool get hasPendingOperations => _operations.isNotEmpty;
  final List<_Operation> _operations = [];
  final Map<String, VirtualNotificationPipe> _current = {};
  final List<VirtualNotificationPipe> pipes = [];
  final List<StreamController<ConnectionStateUpdate>> connections = [];
  final Set<String> heldChannels = {};
  bool Function(ConnectionSessionPacket)? holdSessionPacket;
  bool Function(ConnectionSessionPacket)? dropSessionPacket;
  bool holdConnectionObservations = false;
  bool holdPhysicalStoreObjects = false;
  // A local GATT client can close/reopen while the peripheral link survives.
  bool deliverPhysicalConnectionEvents = true;
  int localCancellations = 0;
  bool deliverRequestedDisconnects = false;
  int nativeDisconnectRequests = 0;
  int nativeDisconnectsDelivered = 0;
  int nativeEpoch = 0;
  bool nativeSessionActivated = false;
  int _scheduledNativeDisconnects = 0;
  bool _peripheralDisconnected = false;
  StreamController<ConnectionStateUpdate>? _currentConnection;
  String? _currentDeviceId;
  int? heldObjectProgressToken;
  final List<VirtualPacket> held = [];
  final List<VirtualPacket> emitted = [];
  final List<Map<String, Object?>> writes = [];
  final List<Map<String, Object?>> subscriptionEvents = [];
  bool dropConsole = false;
  bool failNextConnectionSessionWrite = false;
  int reverseProgressDropsRemaining = 0;
  int droppedReverseProgress = 0;

  Future<Map<String, dynamic>> _enqueue(Map<String, Object?> operation) {
    final result = Completer<Map<String, dynamic>>();
    _operations.add(
      _Operation(operation, result.complete, result.completeError),
    );
    return result.future;
  }

  void _enqueueEvent(Map<String, Object?> operation) {
    // These SDK callbacks return no future. flush owns any transport error.
    _operations.add(_Operation(operation, (_) {}, null));
  }

  String _channel(QualifiedCharacteristic characteristic) {
    if (characteristic.serviceId == BleProtocol.settingsService) {
      if (characteristic.characteristicId ==
          BleProtocol.settingsEnumerateCharacteristic) {
        return 'enumerate';
      }
      if (characteristic.characteristicId ==
          BleProtocol.settingsReadWriteCharacteristic) {
        return 'settings';
      }
    }
    if (characteristic.serviceId == BleProtocol.baseService) {
      if (characteristic.characteristicId ==
          BleProtocol.litchcraftCharacteristic) {
        return 'litchcraft';
      }
      if (characteristic.characteristicId ==
              BleProtocol.connectionSessionIngressCharacteristic ||
          characteristic.characteristicId ==
              BleProtocol.connectionSessionEgressCharacteristic) {
        return 'connection_session';
      }
      if (characteristic.characteristicId ==
          BleProtocol.otaDataCharacteristic) {
        return 'artwork_data';
      }
      if (characteristic.characteristicId ==
          BleProtocol.commandCharacteristic) {
        return 'command';
      }
      if (characteristic.characteristicId ==
          BleProtocol.consoleStreamCharacteristic) {
        return 'console';
      }
      if (characteristic.characteristicId ==
              BleProtocol.otaControlCharacteristic &&
          enableOtaControl) {
        return 'ota_control';
      }
    }
    throw UnsupportedError(
      'OTA/radio capability is outside the Coaching bridge',
    );
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    late StreamController<ConnectionStateUpdate> controller;
    controller = StreamController<ConnectionStateUpdate>(
      sync: true,
      onListen: () {
        _currentConnection = controller;
        _currentDeviceId = id;
        if (!deliverPhysicalConnectionEvents && !_peripheralDisconnected) {
          scheduleMicrotask(
            () => controller.add(
              ConnectionStateUpdate(
                deviceId: id,
                connectionState: DeviceConnectionState.connected,
                failure: null,
              ),
            ),
          );
          return;
        }
        _operations.add(
          _Operation(
            {'op': 'connect', 'connected': true, 'mtu': negotiatedMtu},
            (_) {
              _peripheralDisconnected = false;
              controller.add(
                ConnectionStateUpdate(
                  deviceId: id,
                  connectionState: DeviceConnectionState.connected,
                  failure: null,
                ),
              );
            },
            controller.addError,
          ),
        );
      },
      onCancel: () {
        ++localCancellations;
        if (deliverPhysicalConnectionEvents) {
          _enqueueEvent({'op': 'connect', 'connected': false});
        }
      },
    );
    connections.add(controller);
    return controller.stream;
  }

  // Native Connect uses ATT MTU. Android returns that MTU, while Apple
  // returns the maximum write-without-response value length.
  int mtuCallbackValue(int attMtu) =>
      defaultTargetPlatform == TargetPlatform.android ? attMtu : attMtu - 3;

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async =>
      mtuCallbackValue(negotiatedMtu);

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    final channel = _channel(characteristic);
    late VirtualNotificationPipe pipe;
    pipe = VirtualNotificationPipe(() {
      if (_current[channel] == pipe) {
        _current.remove(channel);
        subscriptionEvents.add({'channel': channel, 'enabled': false});
        _enqueueEvent({
          'op': 'subscribe',
          'channel': channel,
          'enabled': false,
        });
      }
    });
    _current[channel] = pipe;
    pipes.add(pipe);
    subscriptionEvents.add({'channel': channel, 'enabled': true});
    _enqueueEvent({'op': 'subscribe', 'channel': channel, 'enabled': true});
    return pipe;
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    final response = await _enqueue({
      'op': 'read',
      'channel': _channel(characteristic),
    });
    return (response['bytes'] as List<dynamic>).cast<int>();
  }

  Future<void> _write(
    QualifiedCharacteristic characteristic,
    List<int> value,
  ) async {
    final String channel = _channel(characteristic);
    writes.add({'channel': channel, 'bytes': List<int>.of(value)});
    if (channel == 'connection_session' && failNextConnectionSessionWrite) {
      failNextConnectionSessionWrite = false;
      throw StateError('scheduled native connection-session write failure');
    }
    if (channel == 'connection_session' && reverseProgressDropsRemaining > 0) {
      final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value));
      if (packet != null &&
          packet.kind == ConnectionSessionKind.control &&
          packet.code == ConnectionSessionControl.progress &&
          (packet.token & 0x80000000) != 0) {
        reverseProgressDropsRemaining--;
        droppedReverseProgress++;
        return;
      }
    }
    await _enqueue({'op': 'write', 'channel': channel, 'bytes': value});
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => _write(characteristic, value);
  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => _write(characteristic, value);

  void _capture(Map<String, dynamic> response) {
    nativeDisconnectRequests =
        response['disconnect_requests'] as int? ?? nativeDisconnectRequests;
    nativeEpoch = response['native_epoch'] as int? ?? nativeEpoch;
    nativeSessionActivated =
        response['session_activated'] as bool? ?? nativeSessionActivated;
    if (deliverRequestedDisconnects &&
        nativeDisconnectRequests > _scheduledNativeDisconnects) {
      _scheduledNativeDisconnects = nativeDisconnectRequests;
      // Deliver the SDK effect only after production requested it. The native
      // adapter owns OnDisconnected; no Dart protocol/session state is reset.
      _operations.add(
        _Operation({'op': 'connect', 'connected': false}, (_) {
          ++nativeDisconnectsDelivered;
          _peripheralDisconnected = true;
          final connection = _currentConnection;
          if (connection != null && connection.hasListener) {
            connection.add(
              ConnectionStateUpdate(
                deviceId: _currentDeviceId!,
                connectionState: DeviceConnectionState.disconnected,
                failure: null,
              ),
            );
          }
        }, null),
      );
    }
    for (final raw in (response['packets'] as List<dynamic>)) {
      final packet = VirtualPacket(
        raw['channel'] as String,
        (raw['bytes'] as List<dynamic>).cast<int>(),
        raw['epoch'] as int,
        _current[raw['channel']],
      );
      emitted.add(packet);
      if (packet.channel == 'console' && dropConsole) continue;
      final decoded = packet.sessionPacket;
      if (decoded != null && dropSessionPacket?.call(decoded) == true) continue;
      if ((decoded != null && holdSessionPacket?.call(decoded) == true) ||
          (heldObjectProgressToken != null &&
              decoded?.kind == ConnectionSessionKind.control &&
              decoded?.code == ConnectionSessionControl.progress &&
              decoded?.token == heldObjectProgressToken) ||
          heldChannels.contains(packet.channel) ||
          (holdPhysicalStoreObjects &&
              decoded?.kind == ConnectionSessionKind.object &&
              decoded?.code == LitchcraftV2Protocol.physicalStoreBatch) ||
          (holdConnectionObservations &&
              (decoded?.kind == ConnectionSessionKind.state ||
                  decoded?.kind == ConnectionSessionKind.patch))) {
        held.add(packet);
      } else {
        deliver(packet);
      }
    }
  }

  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    // Preserve each actual production notification. Console records already
    // span these paced callbacks; ACK formatting fits one negotiated ATT PDU.
    packet.pipe?.deliver(packet.bytes, lateCallback: lateCallback);
  }

  void release(String channel, {bool lateCallback = false}) => releaseWhere(
    (packet) => packet.channel == channel,
    lateCallback: lateCallback,
  );

  void releaseWhere(
    bool Function(VirtualPacket) matches, {
    bool lateCallback = false,
  }) {
    final packets = held.where(matches).toList();
    held.removeWhere(matches);
    for (final packet in packets) {
      deliver(packet, lateCallback: lateCallback);
    }
  }

  void failConsole() => _current['console']!.fail();

  Future<Map<String, dynamic>> _request(
    WidgetTester? tester,
    Map<String, Object?> operation,
  ) async {
    if (tester == null) return native.request(operation);
    Map<String, dynamic>? response;
    Object? failure;
    StackTrace? failureStack;
    await tester.runAsync(() async {
      try {
        response = await native.request(operation);
      } catch (error, stack) {
        // runAsync otherwise reports the cause and returns null to its caller.
        failure = error;
        failureStack = stack;
      }
    });
    if (failure != null) Error.throwWithStackTrace(failure!, failureStack!);
    return response!;
  }

  Future<void> _pump(WidgetTester? tester, [Duration? duration]) {
    if (tester == null) return Future<void>.value();
    final pumping = _testerCreationZone.run(() => tester.pump(duration));
    return finishTesterWork(tester, pumping);
  }

  /// Joins existing microtask-completing work without advancing time, pumping
  /// another frame, or driving native I/O. Create tester operations in the fake
  /// zone, then call from an unguarded test-body helper outside runAsync; the
  /// barrier checks that all tester scopes close. Post-flush completions and
  /// microtask cancellations may use the same owner. Do not pass work that
  /// starts runAsync or waits for timers or native I/O.
  Future<T> finishTesterWork<T>(WidgetTester tester, Future<T> work) =>
      _finishOwnedWork<T>(tester, () => work);

  // Resource producers start in real async. An existing fake-owned future is
  // still observed before the barrier, including cached disposal completions.
  Future<T> _finishOwnedWork<T>(
    WidgetTester tester,
    Future<T> Function() createWork,
  ) {
    late T result;
    Object? failure;
    StackTrace? failureStack;
    final owner = tester.runAsync<void>(() {
      final completion = Completer<void>();
      void finish([Object? error, StackTrace? stack]) {
        failure ??= error;
        failureStack ??= stack;
        if (!completion.isCompleted) completion.complete();
      }

      try {
        final work = createWork();
        work.then<void>(
          (value) {
            result = value;
            finish();
          },
          onError: (Object error, StackTrace stack) => finish(error, stack),
        );
        tester.binding.asyncBarrier();
      } catch (error, stack) {
        finish(error, stack);
      }
      return completion.future;
    });
    return owner.then<T>((_) {
      if (failure != null) {
        Error.throwWithStackTrace(failure!, failureStack!);
      }
      return result;
    });
  }

  Future<Map<String, dynamic>> command(
    WidgetTester? tester,
    Map<String, Object?> operation, {
    bool flushOperations = true,
  }) async {
    final response = await _request(tester, operation);
    _capture(response);
    await _pump(tester);
    if (flushOperations) await flush(tester);
    return response;
  }

  Future<void> flush(WidgetTester? tester, {int? maxOperations}) async {
    // I/O completes in real async; only tester.pump advances Flutter's clock.
    // Production futures resume between operations in their original zone.
    for (int pass = 0; pass < (maxOperations ?? 200); ++pass) {
      await _pump(tester);
      if (_operations.isEmpty) return;
      if (operationIntervalMs > 0) {
        _capture(
          await _request(tester, {'op': 'advance', 'ms': operationIntervalMs}),
        );
        await _pump(tester, Duration(milliseconds: operationIntervalMs));
      }
      final operation = _operations.removeAt(0);
      final Map<String, dynamic> response;
      try {
        response = await _request(tester, operation.request);
      } catch (error, stack) {
        operation.fail?.call(error, stack);
        await _pump(tester);
        Error.throwWithStackTrace(error, stack);
      }
      _capture(response);
      operation.complete(response);
    }
    if (maxOperations == null && _operations.isNotEmpty) {
      throw StateError(
        'VirtualLitch SDK queue failed to settle: pending=${_operations.length} '
        'next=${_operations.take(5).map((op) => op.request).toList()}',
      );
    }
  }

  Future<void> advance(WidgetTester tester, int milliseconds) async {
    await command(tester, {'op': 'advance', 'ms': milliseconds});
    await _pump(tester, Duration(milliseconds: milliseconds));
    await flush(tester);
    // Requests caused by Flutter timer expiry are drained at this explicit
    // native time. Flush the production console cadence on the next step.
  }

  /// Drive both clocks and real SDK I/O until the production operation settles.
  Future<T> completeTransaction<T>(
    WidgetTester tester,
    Future<T> work, {
    int timeoutMs = 6000,
  }) async {
    var done = false;
    work.then<void>(
      (_) => done = true,
      onError: (Object _, StackTrace _) {
        done = true;
      },
    );
    await advanceUntil(
      tester,
      () => done,
      'the SDK transaction finishes',
      timeoutMs: timeoutMs,
    );
    return work;
  }

  Future<void> advanceUntil(
    WidgetTester tester,
    bool Function() ready,
    String condition, {
    int timeoutMs = 6000,
  }) async {
    await flush(tester);
    for (var elapsed = 0; !ready() && elapsed < timeoutMs; elapsed += 20) {
      await advance(tester, 20);
    }
    expect(
      ready(),
      isTrue,
      reason: 'Expected $condition within ${timeoutMs}ms',
    );
  }

  Iterable<ConnectionSessionPacket> get sessionWrites sync* {
    for (final write in writes) {
      if (write['channel'] != 'connection_session') continue;
      final packet = ConnectionSessionCodec.decode(
        Uint8List.fromList(write['bytes'] as List<int>),
      );
      if (packet != null) yield packet;
    }
  }

  Future<Map<String, dynamic>> frame(WidgetTester tester) async {
    final response = await command(tester, {'op': 'frame'});
    return response['frame'] as Map<String, dynamic>;
  }

  Iterable<ConnectionSessionPacket> get actionWrites => sessionWrites.where(
    (packet) =>
        packet.kind == ConnectionSessionKind.ordered &&
        packet.code == LitchControlProtocol.connectionActionOpcode,
  );

  Future<void> close() async {
    for (final pipe in pipes) {
      await pipe.close();
    }
    for (final connection in connections) {
      await connection.close();
    }
  }

  Future<void> disposeJourney(
    WidgetTester tester,
    LitchBleService service,
  ) async {
    try {
      try {
        await _finishOwnedWork<void>(tester, service.dispose);
        await flush(tester);
      } finally {
        await _finishOwnedWork<void>(tester, close);
      }
    } finally {
      await tester.runAsync(native.close);
    }
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// Exact SDK discovery/characteristic leaves shared by VirtualLitch journeys.
/// Board capability replies, shared packets and domain authority stay native.
/// Batch forwarding records API admission and preserves bytes; it does not model
/// Android/iOS queue priority, radio turns or CoreBluetooth readiness.
class VirtualLitchGattBle extends VirtualLitchBle {
  VirtualLitchGattBle(
    super.native, {
    super.negotiatedMtu,
    super.operationIntervalMs,
  }) : super(enableOtaControl: true);

  LitchBleService createService() => LitchBleService(
    ble: this,
    gattSessionFactory: ReactiveBleGattSessionFactory(this),
    pendingOtaStore: MemoryOtaTargetStore(),
  );

  Future<void> confirmSetting(
    WidgetTester tester,
    LitchBleService service,
    String key,
    Object value,
  ) async {
    final reply = await completeTransaction(
      tester,
      service.writeSettingAcknowledged(
        deviceId: service.connectedId!,
        key: key,
        value: value,
      ),
    );
    expect(reply, isA<LitchSettingWriteAcknowledged>(), reason: key);
    final acknowledged = reply as LitchSettingWriteAcknowledged;
    expect(acknowledged.outcome.ok, isTrue, reason: key);
    expect(
      await completeTransaction(tester, acknowledged.boardStateConfirmation!),
      isTrue,
      reason: key,
    );
    expect(service.latestSettingValue(key), value, reason: key);
  }

  Completer<void>? bootstrapWriteCompletionGate;
  bool bootstrapWriteCompletionHeld = false;
  int discoveryCalls = 0;
  int exactOperations = 0;
  int qualifiedCalls = 0;
  int pendingExactBatches = 0;
  final exactBatches = <({Uuid characteristicId, List<List<int>> values})>[];
  final singleWrites =
      <
        ({
          Uuid characteristicId,
          List<int> value,
          bool withResponse,
          bool batchPending,
        })
      >[];

  @override
  Future<List<Service>> getDiscoveredServices(String deviceId) async {
    ++discoveryCalls;
    return [
      _SdkService(this, deviceId, BleProtocol.baseService, [
        BleProtocol.commandCharacteristic,
        BleProtocol.consoleStreamCharacteristic,
        BleProtocol.litchcraftCharacteristic,
        BleProtocol.connectionSessionIngressCharacteristic,
        BleProtocol.connectionSessionEgressCharacteristic,
        BleProtocol.otaControlCharacteristic,
        BleProtocol.otaDataCharacteristic,
      ]),
      _SdkService(this, deviceId, BleProtocol.settingsService, [
        BleProtocol.settingsEnumerateCharacteristic,
        BleProtocol.settingsReadWriteCharacteristic,
      ]),
    ];
  }

  Future<List<int>> _readExact(QualifiedCharacteristic address) {
    ++exactOperations;
    return super.readCharacteristic(address);
  }

  Future<void> _writeExact(
    QualifiedCharacteristic address,
    List<int> value,
    bool withResponse, {
    bool fromBatch = false,
  }) async {
    ++exactOperations;
    if (!fromBatch) {
      singleWrites.add((
        characteristicId: address.characteristicId,
        value: List<int>.of(value),
        withResponse: withResponse,
        batchPending: pendingExactBatches > 0,
      ));
    }
    await (withResponse
        ? super.writeCharacteristicWithResponse(address, value: value)
        : super.writeCharacteristicWithoutResponse(address, value: value));
    final gate = bootstrapWriteCompletionGate;
    if (gate != null &&
        address.characteristicId == BleProtocol.otaControlCharacteristic &&
        utf8.decode(value, allowMalformed: true) == '{"cmd":"info"}') {
      bootstrapWriteCompletionHeld = true;
      await gate.future;
      bootstrapWriteCompletionHeld = false;
    }
  }

  Stream<List<int>> _subscribeExact(QualifiedCharacteristic address) {
    ++exactOperations;
    return super.subscribeToCharacteristic(address);
  }

  Never _qualifiedOperation() {
    ++qualifiedCalls;
    throw StateError('VirtualLitch bypassed its exact SDK handle');
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) => _qualifiedOperation();

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => _qualifiedOperation();

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => _qualifiedOperation();

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) => _qualifiedOperation();

  @override
  Future<void> requestConnectionPriority({
    required String deviceId,
    required ConnectionPriority priority,
  }) async {}
}

// SDK constructors are private. Reuse the existing app-shell fixture's public
// Service/Characteristic shape, forwarding every packet to VirtualLitchBle.
class _SdkService implements Service {
  _SdkService(this.ble, this.deviceId, this.id, List<Uuid> ids) {
    characteristics = [for (final id in ids) _SdkCharacteristic(this, id)];
  }

  final VirtualLitchGattBle ble;
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

  QualifiedCharacteristic get _address => QualifiedCharacteristic(
    deviceId: service.deviceId,
    serviceId: service.id,
    characteristicId: id,
  );

  @override
  Future<List<int>> read() => service.ble._readExact(_address);

  @override
  Future<void> write(List<int> value, {bool withResponse = true}) =>
      service.ble._writeExact(_address, value, withResponse);

  @override
  Future<void> writeBatchWithoutResponse(List<List<int>> values) async {
    // This forwards bytes only. Native queue/readiness behavior is verified by
    // the existing Kotlin/Swift admission tests, not this SDK substitute.
    final ble = service.ble;
    ble.exactBatches.add((
      characteristicId: id,
      values: values.map(List<int>.of).toList(growable: false),
    ));
    ++ble.pendingExactBatches;
    try {
      for (final value in values) {
        await ble._writeExact(_address, value, false, fromBatch: true);
      }
    } finally {
      --ble.pendingExactBatches;
    }
  }

  @override
  Stream<List<int>> subscribe() => Future<void>.value().asStream().asyncExpand(
    (_) => service.ble._subscribeExact(_address),
  );

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}
