import 'support/console_session_board.dart';
import 'support/reboot_reply_ble.dart' as ota_board;
import 'support/firmware_test_fixture.dart';
import 'support/memory_ota_target_store.dart';
import 'support/support_session_board.dart';
import 'fixtures/board_state_fixture.dart';
import 'health_render_capture.dart';
import 'package:litch_mobile/src/screens/app_console_screen.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:litch_mobile/src/controllers/board_actions_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_identify_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/board_physical_input_state.dart';
import 'package:litch_mobile/src/models/current_game_line_state.dart';
import 'package:litch_mobile/src/models/board_switch_state.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/home_board_feed.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/ota/litch_firmware_uploader.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';

OutlinedButton _coachingButton(WidgetTester tester, Finder control) =>
    tester.widget<OutlinedButton>(control);

ColorFilter _expectedPieceToneFilter(
  Color accent, {
  required bool invertNeutral,
}) {
  final List<double> rows = <double>[];
  for (final double target in <double>[accent.r, accent.g, accent.b]) {
    if (invertNeutral) {
      rows.addAll(<double>[-target / 2, target - 1, -target / 2, 0, 255]);
    } else {
      final double neutral = (1 - target) / 2;
      rows.addAll(<double>[neutral, target, neutral, 0, 0]);
    }
  }
  rows.addAll(const <double>[0, 0, 0, 1, 0]);
  return ColorFilter.matrix(rows);
}

Future<void> _flushSdkCancellation(
  WidgetTester tester,
  Future<void> cancellation,
) async {
  bool completed = false;
  final settlement = cancellation.whenComplete(() => completed = true);
  // Only yield SDK cancellation to its outer scheduler. Service operations
  // and all timed behavior assertions stay in the widget's normal fake zone.
  for (int pass = 0; pass < 8 && !completed; ++pass) {
    await tester.pump();
    await tester.runAsync(() async {});
    await tester.pump();
  }
  expect(completed, isTrue, reason: 'SDK cancellation must settle boundedly.');
  await settlement;
}

class _FailingBle implements FlutterReactiveBle {
  int consoleSubscribeAttempts = 0;
  int keepAliveWriteAttempts = 0;

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    ++consoleSubscribeAttempts;
    return Stream<List<int>>.error(StateError('transient notify failure'));
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    ++keepAliveWriteAttempts;
    throw StateError('transient write failure');
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    ++keepAliveWriteAttempts;
    throw StateError('transient write failure');
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ConsoleCommandBle implements FlutterReactiveBle {
  final List<String> writes = <String>[];
  bool failWrites = false;
  Completer<void>? deferredWrite;

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    if (failWrites) {
      throw StateError('write failed');
    }
    writes.add(utf8.decode(value));
    await deferredWrite?.future;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ReconnectBle implements FlutterReactiveBle {
  final List<StreamController<ConnectionStateUpdate>> connections =
      <StreamController<ConnectionStateUpdate>>[];
  final List<StreamController<ConnectionStateUpdate>> directConnections =
      <StreamController<ConnectionStateUpdate>>[];
  final List<StreamController<ConnectionStateUpdate>> advertisingConnections =
      <StreamController<ConnectionStateUpdate>>[];
  final List<StreamController<List<int>>> notificationStreams =
      <StreamController<List<int>>>[];
  final List<StreamController<List<int>>> settingsNotificationStreams =
      <StreamController<List<int>>>[];
  final List<StreamController<List<int>>> consoleNotificationStreams =
      <StreamController<List<int>>>[];
  final List<StreamController<List<int>>> otaNotificationStreams =
      <StreamController<List<int>>>[];
  final List<StreamController<List<int>>> litchcraftNotificationStreams =
      <StreamController<List<int>>>[];
  final List<String> settingsSubscriptionDeviceIds = <String>[];
  final List<String> consoleSubscriptionDeviceIds = <String>[];
  final List<String> otaSubscriptionDeviceIds = <String>[];
  final List<Completer<List<int>>> deferredSettingsReads =
      <Completer<List<int>>>[];
  final List<Map<String, Object?>> settingsWrites = <Map<String, Object?>>[];
  final List<Map<String, Object?>> commandWrites = <Map<String, Object?>>[];
  final List<String> consoleLineWrites = <String>[];
  void Function(StreamController<List<int>> stream)? onConsoleSubscribed;
  // Default explicitly refuses capability discovery. Current-session tests
  // provide their own contract-3 response and shared-session peer.
  String? capabilityReply = '{"e":"unknown_cmd"}';
  bool deferSettingsReads = false;
  Completer<int>? mtuReply;
  int lastRequestedMtu = 0;
  final List<ConnectionPriority> connectionPriorities = <ConnectionPriority>[];
  int otaSubscribeAttempts = 0;
  int settingsSubscribeAttempts = 0;
  int connectionCancellationCount = 0;
  int identifyStopWriteCount = 0;
  int qualifiedReadAttempts = 0;
  int qualifiedSubscribeAttempts = 0;
  int qualifiedWriteAttempts = 0;
  final Completer<void> advertisingConnectionOpened = Completer<void>();

  Stream<ConnectionStateUpdate> _recordConnection(
    List<StreamController<ConnectionStateUpdate>> modeConnections,
  ) {
    final StreamController<ConnectionStateUpdate> controller =
        StreamController<ConnectionStateUpdate>.broadcast(
          sync: true,
          onCancel: () => connectionCancellationCount += 1,
        );
    connections.add(controller);
    modeConnections.add(controller);
    return controller.stream;
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    return _recordConnection(directConnections);
  }

  @override
  Stream<ConnectionStateUpdate> connectToAdvertisingDevice({
    required String id,
    required List<Uuid> withServices,
    required Duration prescanDuration,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    if (!advertisingConnectionOpened.isCompleted) {
      advertisingConnectionOpened.complete();
    }
    return _recordConnection(advertisingConnections);
  }

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    qualifiedSubscribeAttempts += 1;
    return _subscribePackets(characteristic);
  }

  Stream<List<int>> _subscribePackets(QualifiedCharacteristic characteristic) {
    final StreamController<List<int>> controller =
        StreamController<List<int>>.broadcast(sync: true);
    notificationStreams.add(controller);
    if (characteristic.characteristicId ==
        BleProtocol.settingsReadWriteCharacteristic) {
      settingsSubscribeAttempts += 1;
      settingsNotificationStreams.add(controller);
      settingsSubscriptionDeviceIds.add(characteristic.deviceId);
    }
    if (characteristic.characteristicId ==
        BleProtocol.consoleStreamCharacteristic) {
      consoleNotificationStreams.add(controller);
      consoleSubscriptionDeviceIds.add(characteristic.deviceId);
      final callback = onConsoleSubscribed;
      if (callback != null) {
        scheduleMicrotask(() => callback(controller));
      }
    }
    if (characteristic.characteristicId ==
        BleProtocol.otaControlCharacteristic) {
      otaSubscribeAttempts += 1;
      otaNotificationStreams.add(controller);
      otaSubscriptionDeviceIds.add(characteristic.deviceId);
    }
    if (characteristic.characteristicId ==
        BleProtocol.litchcraftCharacteristic) {
      litchcraftNotificationStreams.add(controller);
    }
    return controller.stream;
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    qualifiedReadAttempts += 1;
    if (deferSettingsReads &&
        characteristic.characteristicId ==
            BleProtocol.settingsEnumerateCharacteristic) {
      final Completer<List<int>> completer = Completer<List<int>>();
      deferredSettingsReads.add(completer);
      return completer.future;
    }
    return utf8.encode('[]');
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    qualifiedWriteAttempts += 1;
    _recordPacketWrite(characteristic, value);
  }

  void _recordPacketWrite(
    QualifiedCharacteristic characteristic,
    List<int> value,
  ) {
    if (characteristic.characteristicId ==
            BleProtocol.otaControlCharacteristic &&
        capabilityReply != null) {
      final request = jsonDecode(utf8.decode(value));
      if (request is Map && request['cmd'] == 'info') {
        scheduleMicrotask(() {
          for (final stream in otaNotificationStreams) {
            if (!stream.isClosed) stream.add(utf8.encode(capabilityReply!));
          }
        });
      }
    }
    if (characteristic.characteristicId == BleProtocol.commandCharacteristic) {
      final String text = utf8.decode(value);
      if (!text.trimLeft().startsWith('{')) {
        consoleLineWrites.add(text);
        return;
      }
      final Object? decoded = jsonDecode(text);
      if (decoded is Map<String, Object?>) {
        commandWrites.add(Map<String, Object?>.from(decoded));
        if (decoded['cmd'] == 'identify_stop') {
          identifyStopWriteCount += 1;
        }
      }
      return;
    }
    if (characteristic.characteristicId !=
        BleProtocol.settingsReadWriteCharacteristic) {
      return;
    }
    final Object? decoded = jsonDecode(utf8.decode(value));
    if (decoded is Map<String, Object?>) {
      settingsWrites.add(Map<String, Object?>.from(decoded));
    }
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    qualifiedWriteAttempts += 1;
  }

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async {
    lastRequestedMtu = mtu;
    if (mtuReply case final reply?) return reply.future;
    return 247;
  }

  @override
  Future<void> requestConnectionPriority({
    required String deviceId,
    required ConnectionPriority priority,
  }) async {
    connectionPriorities.add(priority);
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

// Hold the real service's connection-subscription cancellation at the SDK seam.
class _HeldDisconnectBle extends _ReconnectBle {
  final cancellationStarted = Completer<void>();
  final nativeDisconnected = Completer<void>();

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    if (connections.isNotEmpty) {
      return super.connectToDevice(
        id: id,
        servicesWithCharacteristicsToDiscover:
            servicesWithCharacteristicsToDiscover,
        connectionTimeout: connectionTimeout,
      );
    }
    final controller = StreamController<ConnectionStateUpdate>(
      sync: true,
      onCancel: () {
        connectionCancellationCount++;
        cancellationStarted.complete();
        return nativeDisconnected.future;
      },
    );
    connections.add(controller);
    directConnections.add(controller);
    return controller.stream;
  }
}

// Only the device/SDK boundary is simulated; retained settings and palette
// projection remain owned by the real service and its board_state parser.
class _SeatingJourneyBle extends _AppShellReconnectBle {
  _SeatingJourneyBle() : super(exactGatt: true) {
    useSharedPeer = true;
    capabilityReply = '{"i":[3,3,31]}';
    sharedPeer = _SeatingSessionBoard(this, consoleLineWrites);
  }

  int first = 1;
  bool rotated = false;
  int revision = 100;
  _SeatingSessionBoard get seatingPeer => sharedPeer as _SeatingSessionBoard;

  Map<String, Object> get values => {
    'GAME_FIRST_PLAYER': first,
    'P1_PIECE_WHEEL': 40,
    'P2_PIECE_WHEEL': 200,
    'VS_CPU_LEVEL': 0,
    'VS_CPU_PLAYER': 2,
    'BOARD_ROTATE': rotated,
    'TIMER': 0,
    'BRIGHTNESS': 5,
    'WHITE_SQUARES_ON': true,
  };

  @override
  Future<List<int>> _readPacket(QualifiedCharacteristic characteristic) async =>
      utf8.encode(
        jsonEncode([
          for (final e in values.entries)
            {
              'i': values.keys.toList().indexOf(e.key),
              'k': e.key,
              't': e.value is bool ? 'bool' : 'u32',
              'v': e.value,
              'mn': e.key == 'GAME_FIRST_PLAYER' ? 1 : 0,
              'mx': e.key == 'GAME_FIRST_PLAYER' ? 2 : 255,
            },
        ]),
      );

  void publish() => seatingPeer.publish();
}

class _RecordingGattSessionFactory implements LitchGattSessionFactory {
  final List<_RecordingGattSession> sessions = <_RecordingGattSession>[];
  bool failOpen = false;
  void Function(_RecordingGattSession, Uuid, List<int>)? onWrite;
  Future<List<int>> Function(QualifiedCharacteristic)? onRead;
  int openAttempts = 0;
  Completer<void>? holdOpen;

  @override
  Future<LitchGattSession> open(String deviceId) async {
    openAttempts += 1;
    if (holdOpen case final hold?) await hold.future;
    if (failOpen) {
      throw StateError('service discovery failed');
    }
    final _RecordingGattSession session = _RecordingGattSession(
      deviceId,
      onWrite: onWrite,
      onRead: onRead,
    );
    sessions.add(session);
    return session;
  }

  Future<void> close() async {
    for (final _RecordingGattSession session in sessions) {
      await session.close();
    }
  }
}

class _RecordingGattSession implements LitchGattSession, LitchGattBatchSession {
  _RecordingGattSession(this.deviceId, {this.onWrite, this.onRead});

  final void Function(_RecordingGattSession, Uuid, List<int>)? onWrite;
  Future<List<int>> Function(QualifiedCharacteristic)? onRead;

  final String deviceId;
  final Map<Uuid, StreamController<List<int>>> _notifications =
      <Uuid, StreamController<List<int>>>{};
  final List<Uuid> reads = <Uuid>[];
  final Map<Uuid, List<int>> readResponses = {};
  final List<Uuid> subscriptions = <Uuid>[];
  final List<Uuid> writes = <Uuid>[];
  final List<({Uuid characteristicId, List<int> value, bool withResponse})>
  writeRecords =
      <({Uuid characteristicId, List<int> value, bool withResponse})>[];
  final List<({Uuid characteristicId, List<List<int>> values})> batchWrites =
      <({Uuid characteristicId, List<List<int>> values})>[];
  Completer<void>? holdOtaDataWrites;
  Completer<void>? holdSharedBatchWrites;
  Completer<void>? holdNextOrderedWrite;

  @override
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) async {
    reads.add(characteristicId);
    if (onRead case final read?) {
      return read(
        QualifiedCharacteristic(
          serviceId: serviceId,
          characteristicId: characteristicId,
          deviceId: deviceId,
        ),
      );
    }
    return readResponses[characteristicId] ?? utf8.encode('[]');
  }

  @override
  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    subscriptions.add(characteristicId);
    return _notifications
        .putIfAbsent(
          characteristicId,
          () => StreamController<List<int>>.broadcast(sync: true),
        )
        .stream;
  }

  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    writes.add(characteristicId);
    writeRecords.add((
      characteristicId: characteristicId,
      value: List<int>.of(value),
      withResponse: withResponse,
    ));
    onWrite?.call(this, characteristicId, value);
    final held = holdNextOrderedWrite;
    if (held != null &&
        characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic &&
        ConnectionSessionCodec.decode(Uint8List.fromList(value))?.kind ==
            ConnectionSessionKind.ordered) {
      holdNextOrderedWrite = null;
      await held.future;
    }
    if (characteristicId == BleProtocol.otaDataCharacteristic &&
        holdOtaDataWrites != null) {
      await holdOtaDataWrites!.future;
    }
  }

  @override
  Future<void> writeWithoutResponseBatch({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<List<int>> values,
  }) async {
    batchWrites.add((
      characteristicId: characteristicId,
      values: values.map(List<int>.of).toList(growable: false),
    ));
    for (final value in values) {
      onWrite?.call(this, characteristicId, value);
    }
    if (characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic &&
        holdSharedBatchWrites != null) {
      await holdSharedBatchWrites!.future;
    }
    if (characteristicId == BleProtocol.otaDataCharacteristic &&
        holdOtaDataWrites != null) {
      await holdOtaDataWrites!.future;
    }
  }

  void emit(Uuid characteristicId, List<int> bytes) {
    _notifications[characteristicId]!.add(bytes);
  }

  Future<void> close() async {
    for (final StreamController<List<int>> controller
        in _notifications.values) {
      await controller.close();
    }
  }
}

class _ChunkedEnumerationBle implements FlutterReactiveBle {
  _ChunkedEnumerationBle({
    this.failBeforeReadNumber,
    this.alwaysFail = false,
    List<Map<String, Object?>>? chunks,
  }) : chunks = chunks ?? _chunks;
  final List<Map<String, Object?>> chunks;

  final int? failBeforeReadNumber;
  final bool alwaysFail;
  int readCount = 0;
  int _cursor = 0;
  bool _injectedFailure = false;

  static const List<Map<String, Object?>> _chunks = <Map<String, Object?>>[
    <String, Object?>{
      'it': <Object?>[
        <String, Object?>{
          'i': 0,
          'k': 'GAME_FIRST_PLAYER',
          't': 'u32',
          'c': 'behavior',
          'v': 1,
          'df': 1,
          'mn': 1,
          'mx': 2,
        },
      ],
      'nx': 1,
      'dn': false,
    },
    <String, Object?>{
      'it': <Object?>[
        <String, Object?>{
          'i': 1,
          'k': 'TIMER',
          't': 'u32',
          'c': 'clock',
          'v': 3,
          'df': 0,
          'mn': 0,
          'mx': 6,
        },
      ],
      'nx': 0,
      'dn': true,
    },
  ];

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    readCount += 1;
    if (alwaysFail) {
      throw StateError('injected persistent chunk read failure');
    }
    if (!_injectedFailure && readCount == failBeforeReadNumber) {
      _injectedFailure = true;
      throw StateError('injected chunk read failure');
    }
    final Map<String, Object?> payload = chunks[_cursor];
    _cursor = (_cursor + 1) % chunks.length;
    await Future<void>.delayed(const Duration(milliseconds: 1));
    return utf8.encode(jsonEncode(payload));
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

// Metadata enumeration still uses the exact GATT adapter; its values are not
// live Settings authority until the typed session supplies a complete State.
LitchBleService _metadataService(_ChunkedEnumerationBle ble) {
  final factory = _RecordingGattSessionFactory()
    ..onRead = ble.readCharacteristic;
  final service = LitchBleService(
    pendingOtaStore: MemoryOtaTargetStore(),
    ble: ble,
    gattSessionFactory: factory,
  );
  service.debugSetConnectedDevice('board-1');
  service.debugSetDeviceCapabilities(
    'board-1',
    const LitchDeviceCapabilities(
      contractVersion: 3,
      hardwareRevision: LitchHardwareRevision.v3,
      capabilityMask: LitchDeviceCapabilities.settingsBit,
      explicitlyReported: true,
    ),
  );
  return service;
}

class _ScanBle implements FlutterReactiveBle {
  @override
  Future<void> initialize() async {
    Timer.run(() => statusController.add(BleStatus.ready));
  }

  @override
  Future<void> deinitialize() async {}

  final StreamController<BleStatus> statusController =
      StreamController<BleStatus>.broadcast(sync: true);
  late final StreamController<DiscoveredDevice> _sharedScanController =
      StreamController<DiscoveredDevice>.broadcast(
        sync: true,
        onListen: () => scanSubscriptionCount += 1,
      );
  bool separateScanSessions = false;
  DateTime Function()? androidScanClock;
  final List<DateTime> nativeScanStarts = <DateTime>[];
  int nativeScanThrottles = 0;
  bool failDirectConnect = false;
  int advertisingConnectionAttempts = 0;
  final List<StreamController<DiscoveredDevice>> scanSessions = [];
  Completer<void>? heldScanCancellation;
  int scanCancellationCount = 0;
  StreamController<DiscoveredDevice> get scanController =>
      scanSessions.isEmpty ? _sharedScanController : scanSessions.last;

  int scanForDevicesCalls = 0;
  int scanSubscriptionCount = 0;
  final List<List<Uuid>> scanFilters = <List<Uuid>>[];
  final List<StreamController<ConnectionStateUpdate>> connections =
      <StreamController<ConnectionStateUpdate>>[];
  final List<Map<String, Object?>> commandWrites = <Map<String, Object?>>[];

  @override
  Stream<BleStatus> get statusStream => statusController.stream;

  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) {
    scanForDevicesCalls += 1;
    scanFilters.add(List<Uuid>.from(withServices));
    final clock = androidScanClock;
    if (clock != null) {
      final now = clock();
      nativeScanStarts.removeWhere(
        (start) => now.difference(start) >= const Duration(seconds: 30),
      );
      if (nativeScanStarts.length == 5) {
        nativeScanThrottles += 1;
        return Stream<DiscoveredDevice>.error(
          Exception('Undocumented scan throttle (code 2147483646)'),
        );
      }
      nativeScanStarts.add(now);
    }
    if (separateScanSessions) {
      final controller = StreamController<DiscoveredDevice>(
        sync: true,
        onListen: () => scanSubscriptionCount += 1,
        onCancel: () {
          scanCancellationCount += 1;
          return heldScanCancellation?.future;
        },
      );
      scanSessions.add(controller);
      return controller.stream;
    }
    return scanController.stream;
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    if (failDirectConnect) {
      return Stream.error(StateError('controlled direct failure'));
    }
    final controller = StreamController<ConnectionStateUpdate>.broadcast(
      sync: true,
    );
    connections.add(controller);
    return controller.stream;
  }

  @override
  Stream<ConnectionStateUpdate> connectToAdvertisingDevice({
    required String id,
    required List<Uuid> withServices,
    required Duration prescanDuration,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    advertisingConnectionAttempts++;
    // Pinned SDK performs its own scan when the target advertisement is stale.
    final scan = scanForDevices(
      withServices: withServices,
    ).listen((_) {}, onError: (_) {});
    unawaited(scan.cancel());
    return Stream.error(
      StateError('controlled prescan completed without target'),
    );
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    final decoded = jsonDecode(utf8.decode(value));
    if (decoded is Map<String, Object?>) {
      commandWrites.add(decoded);
    }
  }

  Future<void> close() async {
    for (final connection in connections) {
      await connection.close();
    }
    await statusController.close();
    await _sharedScanController.close();
    for (final scan in scanSessions) {
      await scan.close();
    }
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _PermissionGateway implements BlePermissionGateway {
  _PermissionGateway(this.statuses);

  final Map<Permission, PermissionStatus> statuses;
  int requestCalls = 0;

  @override
  Future<bool> openSettings() async => true;

  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async {
    requestCalls += 1;
    return statuses;
  }

  @override
  Future<PermissionStatus> status(Permission permission) async =>
      statuses[permission] ?? PermissionStatus.denied;
}

class _CpuPromptSessionBoard extends ConsoleSessionBoard {
  _CpuPromptSessionBoard(super.commands, this.from, this.to);

  final int from;
  final int to;
  void publishCpuMove({int sequence = 5}) {
    if (sessionPackets.isClosed) return;
    final Uint8List home = Uint8List(100);
    home[0] = 1;
    home[1] = HomeStateAction.cpu.index;
    home[2] = 1;
    // Initial chess position, with Black to move and the ordinary castling
    // rights. Square numbering in this payload starts at a8.
    const String pieces = ' PNBRQKpnbrqk';
    const String squares =
        'rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR';
    for (int square = 0; square < 64; ++square) {
      final int code = squares[square] == '.'
          ? 0
          : pieces.indexOf(squares[square]);
      home[11 + square ~/ 2] |= code << (square.isEven ? 0 : 4);
    }
    home[43] = 0x1f; // Black to move, with both sides' castling rights.
    home[44] = 0xff;
    ByteData.sublistView(home).setUint32(49, 1, Endian.little);
    home[53] = 0xff;
    ByteData.sublistView(home).setUint64(54, 0xffff00000000ffff, Endian.little);
    home[7] = 0x20; // A physical move prompt is present.
    home[8] = 0x40; // CPU timing is present.
    home[9] = 0x01; // Black is the thinking side.
    home.fillRange(62, 71, 0xff);
    home[62] = from;
    home[63] = to;
    home[64] = 0xff;
    for (int fragment = 0; fragment < 10; ++fragment) {
      sessionPackets.add(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 4 + fragment,
            token: 0x80000001,
            sequence: sequence,
            body: Uint8List.sublistView(
              home,
              fragment * 10,
              fragment * 10 + 10,
            ),
          ),
        ),
      );
    }
  }
}

class _PerBoardCpuReconnectBle extends _AppShellReconnectBle {
  _PerBoardCpuReconnectBle({required super.exactGatt});

  final Map<String, _CpuPromptSessionBoard> _peers =
      <String, _CpuPromptSessionBoard>{};
  Iterable<_CpuPromptSessionBoard> get peers => _peers.values;

  _CpuPromptSessionBoard peerFor(String deviceId) => _peers.putIfAbsent(
    deviceId,
    () => _CpuPromptSessionBoard(
      consoleLineWrites,
      deviceId == 'board-1' ? 6 : 1,
      deviceId == 'board-1' ? 21 : 18,
    ),
  );

  _CpuPromptSessionBoard get activePeer =>
      peerFor(connections.length == 1 ? 'board-1' : 'board-2');

  @override
  Stream<List<int>> _subscribePackets(QualifiedCharacteristic characteristic) {
    if (useSharedPeer &&
        characteristic.characteristicId ==
            BleProtocol.connectionSessionEgressCharacteristic) {
      return peerFor(characteristic.deviceId).sessionPackets.stream;
    }
    return super._subscribePackets(characteristic);
  }

  @override
  Future<void> _writeWithResponsePacket(
    QualifiedCharacteristic characteristic,
    List<int> value,
  ) async {
    if (useSharedPeer &&
        characteristic.characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic) {
      await peerFor(
        characteristic.deviceId,
      ).sharedWrite(Uint8List.fromList(value));
      return;
    }
    await super._writeWithResponsePacket(characteristic, value);
  }
}

class _SeatingSessionBoard extends ConsoleSessionBoard {
  _SeatingSessionBoard(this.board, super.commands);
  final _SeatingJourneyBle board;
  final settingMutations = <List<int>>[];

  @override
  int orderedRejection(ConnectionSessionPacket packet) {
    if (packet.code == SettingsStateWireCodec.mutationOpcode) {
      final body = packet.body;
      if (body.length != 4 || body[0] != 2) return 4;
      settingMutations.add(List<int>.of(body));
      final value = body[2] | (body[3] << 8);
      if (body[1] == 11) board.first = value;
      if (body[1] == 10) board.rotated = value != 0;
      scheduleMicrotask(publish);
      return 0;
    }
    return super.orderedRejection(packet);
  }

  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    await super.sharedWrite(value);
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.query &&
        packet.token == 0x80000001) {
      scheduleMicrotask(publish);
    }
  }

  void publish() {
    if (!sessionActivated || sessionPackets.isClosed) return;
    final revision = ++board.revision;
    final home = Uint8List(100);
    home[0] = 1;
    home[2] = 1;
    home[6] = (board.rotated ? 1 : 0) | (board.first == 2 ? 2 : 0);
    const pieces = ' PNBRQKpnbrqk';
    const squares =
        'rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR';
    for (var square = 0; square < 64; ++square) {
      final code = squares[square] == '.' ? 0 : pieces.indexOf(squares[square]);
      home[11 + square ~/ 2] |= code << (square.isEven ? 0 : 4);
    }
    home[43] = 0x1e;
    home[44] = 0xff;
    ByteData.sublistView(home).setUint32(49, 1, Endian.little);
    home[53] = 0xff;
    ByteData.sublistView(home).setUint64(54, 0xffff00000000ffff, Endian.little);
    home.fillRange(62, 71, 0xff);
    for (var i = 0; i < 10; ++i) {
      _state(4 + i, revision, home.sublist(i * 10, i * 10 + 10));
    }

    final line = File(
      '../test/fixtures/settings_state_wire_v3.csv',
    ).readAsLinesSync().firstWhere((line) => line.startsWith('defaults,'));
    final hex = line.substring(line.indexOf(',') + 1);
    final settings = Uint8List.fromList([
      for (var i = 0; i < hex.length; i += 2)
        int.parse(hex.substring(i, i + 2), radix: 16),
    ]);
    final data = ByteData.sublistView(settings);
    data.setUint32(0, revision, Endian.little);
    data.setUint16(8, 40, Endian.little);
    data.setUint16(10, 200, Endian.little);
    data.setUint16(24, board.rotated ? 1 : 0, Endian.little);
    data.setUint16(26, board.first, Endian.little);
    settings[78] = 40;
    settings[79] = 200;
    var identity = 0xffffffff;
    for (final byte in settings) {
      identity ^= byte;
      for (var bit = 0; bit < 8; ++bit) {
        identity = (identity & 1) != 0
            ? (identity >> 1) ^ 0xedb88320
            : identity >> 1;
      }
    }
    identity = (identity ^ 0xffffffff) & 0xffffffff;
    for (var i = 0; i < 9; ++i) {
      final start = i * 10;
      final length = SettingsStateWireCodec.fragmentLength(i);
      _state(
        SettingsStateWireCodec.fragmentCode(i),
        identity == 0 ? 1 : identity,
        settings.sublist(start, start + length),
      );
    }
  }

  void _state(int code, int revision, List<int> body) {
    sessionPackets.add(
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: code,
          token: 0x80000001,
          sequence: revision,
          body: Uint8List.fromList(body),
        ),
      ),
    );
  }
}

class _AppShellReconnectBle extends _ReconnectBle {
  _AppShellReconnectBle({required this.exactGatt}) {
    capabilityReply = null;
  }

  final bool exactGatt;
  bool useSharedPeer = false;
  late ConsoleSessionBoard sharedPeer = ConsoleSessionBoard(
    consoleLineWrites,
  );
  bool permitIosInventory = false;
  // Model the real SDK's initialize().asStream().asyncExpand before native
  // notification registration, while holding discovery for Home to mount.
  bool asynchronousSdkSubscriptions = false;
  Completer<void>? discoveryReady;
  final List<String> settingsReadsBeforeSubscription = <String>[];
  final _ScanBle discovery = _ScanBle();
  final List<List<_AppShellSdkService>> discoveredSessions =
      <List<_AppShellSdkService>>[];
  final List<String> discoveredDeviceIds = <String>[];
  int unexpectedQualifiedCalls = 0;
  int unexpectedRetiredHandleCalls = 0;
  int exactWriteAttempts = 0;
  int deviceInfoWriteCount = 0;
  Completer<void>? heldInfoWrite;
  final List<List<int>> otaControlWritePayloads = <List<int>>[];
  final List<int> litchcraftOperations = <int>[];
  final Map<String, int> settingValues = <String, int>{
    'GAME_FIRST_PLAYER': 1,
    'P1_END_PIECE_WHEEL': 170,
    'P2_END_PIECE_WHEEL': 0,
    'TIMER': 0,
  };
  bool incompleteDiscovery = false;
  final Set<Uuid> missingOptionalEndpoints = {};
  bool holdNextEnumeration = false;
  int enumerationReplies = 0;

  void _checkQualifiedPath() {
    if (exactGatt) {
      unexpectedQualifiedCalls += 1;
      throw StateError('Android journey used a UUID-only GATT operation');
    }
  }

  @override
  Future<List<Service>> getDiscoveredServices(String deviceId) async {
    discoveredDeviceIds.add(deviceId);
    if (discoveryReady case final pending?) await pending.future;
    // Discovery can resume during a guarded tester.pump. Keep the same fixture
    // invariants without invoking a second guarded test API from its callback.
    expectSync(exactGatt || permitIosInventory, isTrue);
    final int connectionIndex = connections.length - 1;
    expectSync(discoveredSessions, hasLength(connectionIndex));
    final List<_AppShellSdkService> services = <_AppShellSdkService>[
      _AppShellSdkService(this, deviceId, BleProtocol.baseService, <Uuid>[
        BleProtocol.commandCharacteristic,
        BleProtocol.consoleStreamCharacteristic,
        if (!missingOptionalEndpoints.contains(
          BleProtocol.otaControlCharacteristic,
        ))
          BleProtocol.otaControlCharacteristic,
        if (!missingOptionalEndpoints.contains(
          BleProtocol.otaDataCharacteristic,
        ))
          BleProtocol.otaDataCharacteristic,
        if (!missingOptionalEndpoints.contains(
          BleProtocol.litchcraftCharacteristic,
        ))
          BleProtocol.litchcraftCharacteristic,
        if (!missingOptionalEndpoints.contains(
          BleProtocol.connectionSessionIngressCharacteristic,
        ))
          BleProtocol.connectionSessionIngressCharacteristic,
        if (!missingOptionalEndpoints.contains(
          BleProtocol.connectionSessionEgressCharacteristic,
        ))
          BleProtocol.connectionSessionEgressCharacteristic,
      ]),
      _AppShellSdkService(this, deviceId, BleProtocol.settingsService, <Uuid>[
        BleProtocol.settingsEnumerateCharacteristic,
        BleProtocol.settingsReadWriteCharacteristic,
      ]),
    ];
    discoveredSessions.add(services);
    // Model only the SDK handle lifetime at its external boundary. A pending
    // read may still complete; no newly issued operation may use retired handles.
    connections[connectionIndex].stream.listen((ConnectionStateUpdate update) {
      if (update.connectionState == DeviceConnectionState.disconnected ||
          update.connectionState == DeviceConnectionState.disconnecting) {
        for (final _AppShellSdkService service in services) {
          service.retired = true;
        }
      }
    });
    return incompleteDiscovery ? services.take(1).toList() : services;
  }

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    _checkQualifiedPath();
    return super.subscribeToCharacteristic(characteristic);
  }

  @override
  Stream<BleStatus> get statusStream => discovery.statusStream;

  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) => discovery.scanForDevices(
    withServices: withServices,
    scanMode: scanMode,
    requireLocationServicesEnabled: requireLocationServicesEnabled,
  );

  @override
  Stream<List<int>> _subscribePackets(QualifiedCharacteristic characteristic) {
    if (useSharedPeer &&
        characteristic.characteristicId ==
            BleProtocol.connectionSessionEgressCharacteristic) {
      return sharedPeer.sessionPackets.stream;
    }
    return super._subscribePackets(characteristic);
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    _checkQualifiedPath();
    qualifiedReadAttempts += 1;
    return _readPacket(characteristic);
  }

  Future<List<int>> _readPacket(QualifiedCharacteristic characteristic) async {
    if (characteristic.characteristicId !=
        BleProtocol.settingsEnumerateCharacteristic) {
      return utf8.encode('[]');
    }
    if (holdNextEnumeration) {
      holdNextEnumeration = false;
      final Completer<List<int>> read = Completer<List<int>>();
      deferredSettingsReads.add(read);
      return read.future;
    }
    enumerationReplies += 1;
    return utf8.encode(
      jsonEncode(<Object?>[
        for (final MapEntry<String, int> entry in settingValues.entries)
          <String, Object?>{
            'i': settingValues.keys.toList().indexOf(entry.key),
            'k': entry.key,
            't': 'u32',
            'v': entry.value,
          },
      ]),
    );
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    _checkQualifiedPath();
    qualifiedWriteAttempts += 1;
    await _writeWithResponsePacket(characteristic, value);
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    _checkQualifiedPath();
    if (useSharedPeer &&
        characteristic.characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic) {
      await sharedPeer.sharedWrite(Uint8List.fromList(value));
      return;
    }
    await super.writeCharacteristicWithoutResponse(
      characteristic,
      value: value,
    );
  }

  Future<void> _writeWithResponsePacket(
    QualifiedCharacteristic characteristic,
    List<int> value,
  ) async {
    if (useSharedPeer &&
        characteristic.characteristicId ==
            BleProtocol.connectionSessionIngressCharacteristic) {
      await sharedPeer.sharedWrite(Uint8List.fromList(value));
      return;
    }
    _writePacket(characteristic, value);
    if (characteristic.characteristicId ==
        BleProtocol.otaControlCharacteristic) {
      otaControlWritePayloads.add(List<int>.of(value));
      final Object? request = jsonDecode(utf8.decode(value));
      if (request is Map<String, Object?> && request['cmd'] == 'info') {
        // Hold only the external SDK future, without modeling native admission.
        if (heldInfoWrite case final pending?) await pending.future;
      }
    }
  }

  void _writePacket(QualifiedCharacteristic characteristic, List<int> value) {
    _recordPacketWrite(characteristic, value);
    if (characteristic.characteristicId ==
        BleProtocol.litchcraftCharacteristic) {
      litchcraftOperations.add(value[1]);
      final int requestId = value[6] | (value[7] << 8);
      litchcraftNotificationStreams.last.add(<int>[
        LitchcraftProtocol.magic,
        LitchcraftProtocol.state,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        requestId & 0xff,
        requestId >> 8,
        0,
        0,
        LitchcraftTopology.v3.id,
        0,
        0,
        0,
        15,
        0,
      ]);
      return;
    }
    if (characteristic.characteristicId ==
        BleProtocol.otaControlCharacteristic) {
      final Object? request = jsonDecode(utf8.decode(value));
      if (request is Map<String, Object?> && request['cmd'] == 'info') {
        deviceInfoWriteCount += 1;
      }
      return;
    }
    if (characteristic.characteristicId !=
        BleProtocol.settingsReadWriteCharacteristic) {
      return;
    }
    final Map<String, Object?> request =
        jsonDecode(utf8.decode(value)) as Map<String, Object?>;
    if (request.containsKey('v')) return;
    if (asynchronousSdkSubscriptions &&
        (settingsNotificationStreams.isEmpty ||
            !settingsNotificationStreams.last.hasListener)) {
      // A successful GATT write cannot deliver the read reply before CCCD.
      settingsReadsBeforeSubscription.add(request['k']! as String);
      return;
    }
    final Object? reply = settingValues[request['k']];
    // Match the actual request id AND key, including GAME_FIRST_PLAYER. Only
    // the explicitly selected enumeration is held; ordinary reads keep working.
    settingsNotificationStreams.last.add(
      utf8.encode(
        jsonEncode(<String, Object?>{
          'i': request['i'],
          'k': request['k'],
          'ok': reply != null,
          if (reply != null) 'v': reply else 'e': 'unknown_key',
        }),
      ),
    );
  }
}

// SDK objects have private constructors. These implement only their external
// surface; the actual ReactiveBleGattSessionFactory selects and owns the handles.
class _AppShellSdkService implements Service {
  _AppShellSdkService(this.ble, this.deviceId, this.id, List<Uuid> ids) {
    characteristics = <_AppShellSdkCharacteristic>[
      for (final Uuid id in ids) _AppShellSdkCharacteristic(this, id),
    ];
  }

  final _AppShellReconnectBle ble;
  @override
  final String deviceId;
  @override
  final Uuid id;
  @override
  late final List<_AppShellSdkCharacteristic> characteristics;
  bool retired = false;

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _AppShellSdkCharacteristic implements Characteristic {
  _AppShellSdkCharacteristic(this.service, this.id);

  @override
  final _AppShellSdkService service;
  @override
  final Uuid id;
  int readCount = 0;
  int writeCount = 0;
  int subscribeCount = 0;
  int get operationCount => readCount + writeCount + subscribeCount;

  QualifiedCharacteristic _checkHandle() {
    if (service.retired ||
        !service.ble.discoveredSessions.last.contains(service)) {
      service.ble.unexpectedRetiredHandleCalls += 1;
      throw StateError('Android journey used a retired SDK characteristic');
    }
    return QualifiedCharacteristic(
      deviceId: service.deviceId,
      serviceId: service.id,
      characteristicId: id,
    );
  }

  @override
  Future<List<int>> read() {
    final QualifiedCharacteristic address = _checkHandle();
    readCount += 1;
    return service.ble._readPacket(address);
  }

  @override
  Future<void> write(List<int> value, {bool withResponse = true}) async {
    final QualifiedCharacteristic address = _checkHandle();
    writeCount += 1;
    service.ble.exactWriteAttempts += 1;
    if (withResponse ||
        service.ble.useSharedPeer &&
            address.characteristicId ==
                BleProtocol.connectionSessionIngressCharacteristic) {
      await service.ble._writeWithResponsePacket(address, value);
    }
  }

  @override
  Stream<List<int>> subscribe() {
    final QualifiedCharacteristic address = _checkHandle();
    subscribeCount += 1;
    if (service.ble.asynchronousSdkSubscriptions) {
      // flutter_reactive_ble 5.4.0 Characteristic.subscribe() defers this
      // boundary even when initialize() has already completed.
      return Future<void>.value().asStream().asyncExpand(
        (_) => service.ble._subscribePackets(address),
      );
    }
    return service.ble._subscribePackets(address);
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

Future<void> _waitFor(bool Function() condition) async {
  for (int iteration = 0; iteration < 50; ++iteration) {
    if (condition()) {
      return;
    }
    await Future<void>.delayed(Duration.zero);
  }
  expect(condition(), isTrue);
}

Future<void> _disposeReconnectHarness(
  LitchBleService service,
  _ReconnectBle fakeBle,
) async {
  await service.dispose();
  for (final StreamController<ConnectionStateUpdate> stream
      in fakeBle.connections) {
    if (!stream.isClosed) {
      await stream.close();
    }
  }
  for (final StreamController<List<int>> stream
      in fakeBle.notificationStreams) {
    if (!stream.isClosed) {
      await stream.close();
    }
  }
}

class _MemoryTrace implements AppTraceSink {
  final List<(String, Map<String, Object?>)> records =
      <(String, Map<String, Object?>)>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    records.add((event, Map<String, Object?>.from(fields)));
  }
}

// Counts actual SDK snapshot requests without reading or resetting a log.
class _JournalCountingBle extends _ReconnectBle {
  final diagnosticRequests = <String>[];

  @override
  Future<List<Map<String, Object?>>> getConnectionDiagnostics(
    String deviceId,
  ) async {
    diagnosticRequests.add(deviceId);
    return <Map<String, Object?>>[];
  }
}

class _RecoverySharedPeer {
  _RecoverySharedPeer({
    bool connected = true,
    _ReconnectBle? nativeBle,
    Duration receivedHeadCompletionTimeout =
        LitchBleService.orderedHeadCompletionTimeout,
    AppTraceSink? appTrace,
  }) : ble = nativeBle ?? _ReconnectBle() {
    sessions.onWrite = write;
    service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: ble,
      gattSessionFactory: sessions,
      receivedHeadCompletionTimeout: receivedHeadCompletionTimeout,
      appTrace: appTrace,
    );
    if (connected) {
      service.debugSetConnectedDevice('board-1');
      service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.consoleBit |
              LitchDeviceCapabilities.settingsBit |
              LitchDeviceCapabilities.identifyBit |
              LitchDeviceCapabilities.firmwareDigestBit |
              LitchDeviceCapabilities.firmwareUpdateBit |
              LitchDeviceCapabilities.runtimeHealthBit |
              LitchDeviceCapabilities.gameHistoryBit,
          explicitlyReported: true,
        ),
      );
    }
  }
  final _ReconnectBle ble;
  final sessions = _RecordingGattSessionFactory();
  late final LitchBleService service;
  _RecordingGattSession? native;
  String? infoPayload;
  int queries = 0, actions = 0, terminal = 0;
  final stateQueryCodes = <int>[];
  bool retained = false;
  void Function()? onStateQuery;
  void Function()? onOrdered;
  Future<void> open({String deviceId = 'board-1'}) async {
    await service.openConnectionSession(deviceId);
  }

  void control(int code, int token, int sequence, Uint8List body) {
    native!.emit(
      BleProtocol.connectionSessionEgressCharacteristic,
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.control,
          code: code,
          token: token,
          sequence: sequence,
          body: body,
        ),
      ),
    );
  }

  void progress() {
    final body = Uint8List(10);
    ByteData.sublistView(body)
      ..setUint32(0, terminal, Endian.little)
      ..setUint32(4, terminal + 8, Endian.little)
      ..setUint16(8, retained && terminal == 0 ? 1 : 0, Endian.little);
    control(ConnectionSessionControl.progress, 2, terminal, body);
  }

  void finish({int revision = 1}) {
    terminal = actions;
    final body = Uint8List(10);
    ByteData.sublistView(body).setUint32(0, revision, Endian.little);
    control(ConnectionSessionControl.checkpoint, 2, terminal, body);
    progress();
  }

  Uint8List fixture(String file, String row) {
    final line = File(
      '../test/fixtures/$file',
    ).readAsLinesSync().firstWhere((line) => line.startsWith('$row,'));
    final hex = line.substring(line.indexOf(',') + 1);
    return Uint8List.fromList([
      for (var i = 0; i < hex.length; i += 2)
        int.parse(hex.substring(i, i + 2), radix: 16),
    ]);
  }

  void home({
    int count = 10,
    int firstFragment = 0,
    int revision = 1,
    HomeStateAction? action,
  }) {
    final bytes = fixture('home_state_wire_v1.csv', 'representative');
    if (action != null) bytes[1] = action.index;
    stateFragments(bytes, 4, count, revision, firstFragment: firstFragment);
  }

  Uint8List settingsPayload({
    int revision = 0,
    int brightness = 5,
    int setupGuidance = 1,
    int hintStyle = 3,
    int placementStyle = 1,
    int sleepTimeout = 2,
    int wakeMode = 0,
    int lightSquares = 1,
  }) {
    final bytes = fixture('settings_state_wire_v3.csv', 'defaults');
    ByteData.sublistView(bytes)
      ..setUint32(0, revision, Endian.little)
      ..setUint16(4, brightness, Endian.little)
      ..setUint16(6, lightSquares, Endian.little)
      ..setUint16(30, sleepTimeout, Endian.little)
      ..setUint16(46, wakeMode, Endian.little)
      ..setUint16(4 + 28 * 2, setupGuidance, Endian.little)
      ..setUint16(80, hintStyle, Endian.little)
      ..setUint16(64, placementStyle, Endian.little);
    return bytes;
  }

  int settingsIdentity(Uint8List bytes) {
    // firmware_home_state.cpp identifies fragments by CRC32 of the entire
    // payload; the first payload uint32 is the separate settings revision.
    var identity = 0xffffffff;
    for (final byte in bytes) {
      identity ^= byte;
      for (var bit = 0; bit < 8; ++bit) {
        identity = (identity & 1) != 0
            ? (identity >> 1) ^ 0xedb88320
            : identity >> 1;
      }
    }
    identity = (identity ^ 0xffffffff) & 0xffffffff;
    return identity == 0 ? 1 : identity;
  }

  void settings({
    int count = 9,
    int firstFragment = 0,
    int revision = 0,
    int brightness = 5,
    int setupGuidance = 1,
    int hintStyle = 3,
    int placementStyle = 1,
    int sleepTimeout = 2,
    int wakeMode = 0,
    int lightSquares = 1,
    int? identity,
  }) {
    final bytes = settingsPayload(
      revision: revision,
      brightness: brightness,
      setupGuidance: setupGuidance,
      hintStyle: hintStyle,
      placementStyle: placementStyle,
      sleepTimeout: sleepTimeout,
      wakeMode: wakeMode,
      lightSquares: lightSquares,
    );
    stateFragments(
      bytes,
      14,
      count,
      identity ?? settingsIdentity(bytes),
      firstFragment: firstFragment,
    );
  }

  Future<void> startConsole(WidgetTester tester) async {
    service.debugStartConsoleMirror('board-1');
    for (var turn = 0; turn < 8; ++turn) {
      await tester.pump();
      await tester.runAsync(() async {});
      if (native!.subscriptions.contains(
        BleProtocol.consoleStreamCharacteristic,
      )) {
        return;
      }
    }
    expect(
      native!.subscriptions,
      contains(BleProtocol.consoleStreamCharacteristic),
    );
  }

  void consoleSettings({int revision = 7}) {
    final values = SettingsStateWireCodec.tryDecode(
      settingsPayload(revision: revision),
    )!.values;
    final record = boardStateRecord()
      ..['sr'] = revision
      ..['settings'] = [
        for (final entry in values.entries)
          if (!entry.key.endsWith('_END_PIECE_WHEEL')) [entry.key, entry.value],
      ]
      ..['ends'] = [values['P1_PIECE_WHEEL'], values['P2_PIECE_WHEEL']];
    final bytes = utf8.encode('${jsonEncode(record)}\n');
    for (var offset = 0; offset < bytes.length; offset += 120) {
      native!.emit(
        BleProtocol.consoleStreamCharacteristic,
        bytes.sublist(offset, (offset + 120).clamp(0, bytes.length)),
      );
    }
  }

  Future<List<LitchSetting>> readMetadata(WidgetTester tester) async {
    enumerateMetadata();
    bool complete = false;
    final reading = service
        .readSettings('board-1')
        .whenComplete(() => complete = true);
    for (var turn = 0; turn < 8 && !complete; ++turn) {
      await tester.pump();
      await tester.runAsync(() async {});
    }
    expect(complete, isTrue, reason: 'immutable enumeration must complete');
    return reading;
  }

  void enumerateMetadata() {
    native!.readResponses[BleProtocol.settingsEnumerateCharacteristic] = utf8
        .encode(
          jsonEncode([
            {
              'i': 4,
              'k': 'TURN_ANIM_STYLE',
              't': 'enum',
              'c': 'display',
              'v': 'active_end_only',
              'df': 'active_end_only',
              'opts': [
                'active_end_only',
                'active_end_pingpong_sides',
                'both_ends',
                'side_middle_band',
              ],
              'd': 'Turn indicator',
            },
          ]),
        );
  }

  void stateFragments(
    Uint8List bytes,
    int firstCode,
    int count,
    int revision, {
    int firstFragment = 0,
  }) {
    for (var i = firstFragment; i < firstFragment + count; ++i) {
      native!.emit(
        BleProtocol.connectionSessionEgressCharacteristic,
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: firstCode == 14
                ? SettingsStateWireCodec.fragmentCode(i)
                : firstCode + i,
            token: 0x80000001,
            sequence: revision,
            body: Uint8List.sublistView(
              bytes,
              i * 10,
              i * 10 +
                  (firstCode == 14
                      ? SettingsStateWireCodec.fragmentLength(i)
                      : 10),
            ),
          ),
        ),
      );
    }
  }

  void write(
    _RecordingGattSession session,
    Uuid characteristic,
    List<int> bytes,
  ) {
    if (characteristic == BleProtocol.otaControlCharacteristic &&
        infoPayload != null) {
      final command = jsonDecode(utf8.decode(bytes));
      if (command is Map && command['cmd'] == 'info') {
        scheduleMicrotask(
          () => session.emit(characteristic, utf8.encode(infoPayload!)),
        );
      }
      return;
    }
    if (characteristic != BleProtocol.connectionSessionIngressCharacteristic) {
      return;
    }
    native = session;
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(bytes))!;
    if (packet.kind == ConnectionSessionKind.ordered) {
      actions++;
      retained = true;
      progress();
      onOrdered?.call();
      return;
    }
    if (packet.kind != ConnectionSessionKind.control) return;
    switch (packet.code) {
      case ConnectionSessionControl.hello:
        control(ConnectionSessionControl.welcome, 1, 0, packet.body);
        final identity = Uint8List(10);
        ByteData.sublistView(identity)
          ..setUint64(0, 1, Endian.little)
          ..setUint16(
            8,
            ConnectionSession.orderedFeature |
                ConnectionSession.stateFeature |
                ConnectionSession.objectFeature |
                ConnectionSession.reverseObjectFeature,
            Endian.little,
          );
        control(ConnectionSessionControl.identity, 1, 0, identity);
      case ConnectionSessionControl.ready:
        control(ConnectionSessionControl.activated, 1, 0, packet.body);
        for (final sequence in [1, 2]) {
          final body = Uint8List(8);
          ByteData.sublistView(body)
            ..setUint16(
              0,
              sequence == 1
                  ? ConnectionSession.litchcraftStateTopic
                  : ConnectionSession.litchcraftReverseObjectTopic,
              Endian.little,
            )
            ..setUint16(4, 10, Endian.little);
          body[2] = 1;
          body[3] = sequence == 1
              ? ConnectionSession.stateCapacity
              : ConnectionSession.reverseObjectCapacity;
          control(ConnectionSessionControl.open, 1, sequence, body);
        }
      case ConnectionSessionControl.open:
        final body = Uint8List(10);
        ByteData.sublistView(body)
          ..setUint32(0, packet.sequence == 1 ? 2 : 3, Endian.little)
          ..setUint16(
            4,
            ByteData.sublistView(packet.body).getUint16(4, Endian.little),
            Endian.little,
          );
        body[6] = packet.body[3];
        body[7] = packet.body[2];
        control(ConnectionSessionControl.opened, 1, packet.sequence, body);
        if (packet.sequence == 1) progress();
      case ConnectionSessionControl.query:
        if (packet.token == 0x80000001) {
          queries++;
          stateQueryCodes.add(packet.sequence);
          onStateQuery?.call();
        }
        if (packet.token == 2) progress();
      case ConnectionSessionControl.confirm:
        if (packet.token == 2) progress();
    }
  }

  Future<void> close() async {
    await service.dispose();
    await sessions.close();
  }
}

Iterable<ConnectionSessionPacket> _sharedPackets(
  _RecordingGattSession native,
) => native.writeRecords
    .where(
      (entry) =>
          entry.characteristicId ==
          BleProtocol.connectionSessionIngressCharacteristic,
    )
    .map(
      (entry) =>
          ConnectionSessionCodec.decode(Uint8List.fromList(entry.value))!,
    );

Iterable<ConnectionSessionPacket> _orderedPackets(
  _RecordingGattSession native,
) => _sharedPackets(
  native,
).where((p) => p.kind == ConnectionSessionKind.ordered);

int _heartbeats(_RecordingGattSession native) => _sharedPackets(native)
    .where(
      (packet) =>
          packet.kind == ConnectionSessionKind.control &&
          packet.code == ConnectionSessionControl.heartbeat,
    )
    .length;

void _setHeartbeatProfile(_RecoverySharedPeer peer) {
  peer.service.debugSetConnectedDevice('board-1');
  peer.service.debugSetDeviceCapabilities(
    'board-1',
    const LitchDeviceCapabilities(
      contractVersion: 3,
      hardwareRevision: LitchHardwareRevision.v3,
      capabilityMask: 0,
      explicitlyReported: true,
    ),
  );
}

Future<void> _openRecoveryPeer(
  WidgetTester tester,
  _RecoverySharedPeer peer, {
  String deviceId = 'board-1',
}) async {
  bool complete = false;
  final opening = peer
      .open(deviceId: deviceId)
      .whenComplete(() => complete = true);
  for (var turn = 0; turn < 8 && !complete; ++turn) {
    await tester.pump(const Duration(milliseconds: 100));
  }
  expect(complete, isTrue, reason: 'actual shared handshake must activate');
  await opening;
  // A coherent startup replay releases its subscription on the outer stream
  // scheduler. Settle that boundary before injecting the next user operation.
  for (var turn = 0; turn < 8; ++turn) {
    await tester.pump();
    await tester.runAsync(() async {});
  }
}

void main() {
  testWidgets(
    'queued domain work cannot adopt a same-ID replacement after capability discovery',
    (tester) async {
      final peer = _RecoverySharedPeer(connected: false)
        ..infoPayload = '{"i":[99,4,0]}';
      final completed = <String, Object?>{};
      void track(String name, Future<Object?> work) {
        unawaited(
          work.then(
            (value) => completed[name] = value,
            onError: (Object error) => completed[name] = error,
          ),
        );
      }

      Future<void> connectAt(int index) async {
        final connecting = peer.service.connect('board-1');
        for (
          var turn = 0;
          turn < 32 && peer.ble.connections.length <= index;
          ++turn
        ) {
          await tester.pump(Duration.zero);
          await tester.runAsync(() async {});
        }
        expect(peer.ble.connections, hasLength(index + 1));
        peer.ble.connections[index].add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump(Duration.zero);
        expect(await connecting, isTrue);
        for (var turn = 0; turn < 16; ++turn) {
          await tester.pump(Duration.zero);
          await tester.runAsync(() async {});
        }
      }

      try {
        await connectAt(0);
        final oldDiscovery = peer.service.fetchDeviceCapabilities(
          deviceId: 'board-1',
        );
        bool oldFinished = false;
        unawaited(oldDiscovery.then((_) => oldFinished = true));
        final oldToken = peer.service.connectionSessionToken;
        track(
          'action',
          peer.service.dispatchControlAction(
            deviceId: 'board-1',
            action: LitchControlAction.newGame,
          ),
        );
        track(
          'setting',
          peer.service.writeSettingAcknowledged(
            deviceId: 'board-1',
            key: 'BRIGHTNESS',
            value: 2,
          ),
        );
        track('sleep', peer.service.requestSleep(deviceId: 'board-1'));
        track(
          'bulk',
          peer.service.writeLitchcraftBulkArtwork('board-1', 1, 1, [1]),
        );
        track('console', peer.service.sendConsoleLine('board-1', 'newgame'));
        track(
          'consoleBytes',
          peer.service.sendConsoleBytes('board-1', utf8.encode('newgame\n')),
        );
        track('home', peer.service.requestCurrentHomeStateSnapshot('board-1'));
        track('settingsRead', peer.service.readSettings('board-1'));
        track(
          'settingRead',
          peer.service.readSettingValue(deviceId: 'board-1', key: 'BRIGHTNESS'),
        );
        track(
          'craftAvailable',
          peer.service.litchcraftControlAvailable('board-1'),
        );
        track(
          'craftStream',
          peer.service.litchcraftPackets('board-1').toList(),
        );
        track(
          'historyStream',
          peer.service.gameHistoryPackets('board-1').toList(),
        );
        track(
          'craftPacket',
          peer.service.writeLitchcraftPacket('board-1', [
            0x4c,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
          ]),
        );
        track(
          'craftV2',
          peer.service.writeLitchcraftV2Packet('board-1', [
            0x4d,
            2,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
          ], connectionSessionToken: oldToken),
        );
        await tester.pump(Duration.zero);
        expect(
          oldFinished,
          isFalse,
          reason: 'malformed info holds the real discovery retry',
        );
        expect(completed, isEmpty);
        peer.ble.connections.first.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await tester.pump(Duration.zero);
        peer.infoPayload = '{"i":[3,4,65535]}';
        await connectAt(1);
        bool activated = false;
        final opening = peer.service.openConnectionSession('board-1').then((
          session,
        ) {
          activated = session?.isActivated == true;
        });
        for (var turn = 0; turn < 32 && !activated; ++turn) {
          await tester.pump(Duration.zero);
          await tester.runAsync(() async {});
        }
        expect(
          activated,
          isTrue,
          reason:
              'replacement is an actual activated shared session; '
              '${peer.sessions.sessions.map((s) => (s.writes, s.subscriptions)).toList()}',
        );
        await opening;
        expect(peer.service.connectionSessionToken, isNot(oldToken));
        expect(oldFinished, isFalse);
        final replacement = peer.sessions.sessions.last;
        final reads = replacement.reads.length;
        final actions = peer.actions;
        await tester.pump(const Duration(milliseconds: 200));
        for (var turn = 0; turn < 16; ++turn) {
          await tester.pump(Duration.zero);
          await tester.runAsync(() async {});
        }
        expect(oldFinished, isTrue);
        expect(
          peer.actions,
          actions,
          reason: 'retired work cannot mutate the replacement session',
        );
        expect(
          replacement.reads.length,
          reads,
          reason: 'retired domain reads cannot adopt replacement GATT',
        );
        expect(
          completed.keys,
          containsAll([
            'action',
            'setting',
            'sleep',
            'bulk',
            'console',
            'consoleBytes',
            'home',
            'settingsRead',
            'settingRead',
            'craftAvailable',
            'craftStream',
            'historyStream',
            'craftPacket',
            'craftV2',
          ]),
        );
        expect(
          (completed['action']! as LitchControlEvent).phase,
          LitchControlPhase.interrupted,
        );
        expect(completed['setting'], isA<LitchSettingWriteNotAcknowledged>());
        expect(completed['sleep'], isNull);
        expect(completed['console'], isFalse);
        expect(completed['home'], isFalse);
        expect(completed['settingsRead'], isEmpty);
        expect(completed['settingRead'], isNull);
        expect(completed['craftAvailable'], isFalse);
        expect(completed['craftStream'], isEmpty);
        expect(completed['historyStream'], isEmpty);
        expect(completed['bulk'], isA<StateError>());
        expect(completed['craftPacket'], isA<StateError>());
        expect(completed['craftV2'], isA<StateError>());
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );
  test('recovery discovery precedes all automatic domain ingress', () async {
    final peer = _RecoverySharedPeer(connected: false)
      ..infoPayload = '{"i":[3,4,24,1]}';
    try {
      final connecting = peer.service.connect('board-1');
      await _waitFor(() => peer.ble.connections.isNotEmpty);
      expect(peer.ble.connections, hasLength(1));
      peer.ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
      final capabilities = await peer.service.fetchDeviceCapabilities(
        deviceId: 'board-1',
      );
      expect(capabilities.recoveryOnly, isTrue);
      final session = await peer.service.openConnectionSession('board-1');
      expect(session!.isActivated, isTrue);
      expect(
        await peer.service.requestCurrentHomeStateSnapshot('board-1'),
        isFalse,
      );
      expect(await peer.service.readSettings('board-1'), isEmpty);
      expect(await peer.service.litchcraftPackets('board-1').toList(), isEmpty);
      expect(await peer.service.litchcraftControlAvailable('board-1'), isFalse);
      await expectLater(
        peer.service.writeLitchcraftV2Packet('board-1', [
          0x4d,
          2,
          1,
          0,
          0,
          0,
          0,
          0,
          0,
        ], connectionSessionToken: peer.service.connectionSessionToken),
        throwsStateError,
      );
      expect(await peer.service.sendConsoleLine('board-1', 'newgame'), isFalse);
      await peer.service.sendConsoleBytes('board-1', utf8.encode('newgame\n'));
      expect(
        await peer.service.writeSetting(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
          value: 2,
        ),
        isFalse,
      );
      expect(
        await peer.service.gameHistoryPackets('board-1').toList(),
        isEmpty,
      );
      expect(
        peer.queries,
        0,
        reason: 'no Home/Setup recovery state reader is armed',
      );
      expect(peer.actions, 0);
      final native = peer.sessions.sessions.single;
      expect(native.reads, isEmpty);
      expect(
        native.subscriptions,
        isNot(contains(BleProtocol.consoleStreamCharacteristic)),
      );
      expect(
        native.subscriptions,
        isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
      );
      expect(
        native.subscriptions,
        contains(BleProtocol.connectionSessionEgressCharacteristic),
      );
      expect(
        native.subscriptions,
        contains(BleProtocol.otaControlCharacteristic),
      );
      expect(
        native.writes.toSet(),
        everyElement(
          anyOf(
            BleProtocol.otaControlCharacteristic,
            BleProtocol.connectionSessionIngressCharacteristic,
          ),
        ),
      );
    } finally {
      await peer.close();
    }
  });

  testWidgets(
    'Sleep uses acknowledged Ordered control and never legacy writes',
    (tester) async {
      final peer = _RecoverySharedPeer();
      try {
        await _openRecoveryPeer(tester, peer);
        peer.onOrdered = () => peer.finish();
        final sleeping = peer.service.requestSleep(deviceId: 'board-1');
        await tester.pump();
        expect(
          (await sleeping)?.disposition,
          ConnectionSessionDisposition.applied,
        );
        final records = peer.native!.writeRecords
            .where(
              (record) =>
                  record.characteristicId ==
                  BleProtocol.connectionSessionIngressCharacteristic,
            )
            .map(
              (record) => ConnectionSessionCodec.decode(
                Uint8List.fromList(record.value),
              )!,
            )
            .where((packet) => packet.kind == ConnectionSessionKind.ordered)
            .toList();
        expect(records, hasLength(1));
        expect(records.single.code, 25);
        expect(records.single.body, <int>[1, 6, 0, 0, 1]);
        expect(
          peer.native!.writes,
          isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
        );
        expect(
          await peer.service.requestSleep(deviceId: 'other-board'),
          isNull,
        );
        expect(peer.actions, 1);
      } finally {
        await peer.close();
      }
    },
  );

  for (final change
      in <
        ({
          String key,
          Object value,
          int index,
          int normalized,
          int timeout,
          int mode,
          int light,
        })
      >[
        (
          key: 'SLEEP_TIMEOUT',
          value: '5m',
          index: 13,
          normalized: 0,
          timeout: 0,
          mode: 0,
          light: 0,
        ),
        (
          key: 'MOTION_WAKE_MODE',
          value: 'tap',
          index: 21,
          normalized: 1,
          timeout: 3,
          mode: 1,
          light: 0,
        ),
        (
          key: 'MOTION_WAKE_MODE',
          value: 'motion_and_tap',
          index: 21,
          normalized: 2,
          timeout: 3,
          mode: 2,
          light: 0,
        ),
        (
          key: 'WHITE_SQUARES_ON',
          value: true,
          index: 1,
          normalized: 1,
          timeout: 3,
          mode: 0,
          light: 1,
        ),
      ]) {
    testWidgets(
      'Sleep/Wake ${change.key} ${change.value} confirms from split typed State',
      (tester) async {
        final peer = _RecoverySharedPeer();
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 7, sleepTimeout: 3, lightSquares: 0);
        };
        try {
          await _openRecoveryPeer(tester, peer);
          await tester.pump();
          peer.onStateQuery = () {};
          peer.onOrdered = () => peer.finish(revision: 8);
          final writing = peer.service.writeSettingAcknowledged(
            deviceId: 'board-1',
            key: change.key,
            value: change.value,
          );
          await tester.pump();
          final result = await writing as LitchSettingWriteAcknowledged;
          expect(result.outcome.ok, isTrue);
          final command = peer.native!.writeRecords
              .where(
                (write) =>
                    write.characteristicId ==
                    BleProtocol.connectionSessionIngressCharacteristic,
              )
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write.value),
                )!,
              )
              .singleWhere(
                (packet) =>
                    packet.kind == ConnectionSessionKind.ordered &&
                    packet.code == SettingsStateWireCodec.mutationOpcode,
              );
          expect(command.body, <int>[2, change.index, change.normalized, 0]);
          bool? confirmed;
          result.boardStateConfirmation!.then((value) => confirmed = value);
          // A complete but unchanged value cannot confirm an applied enum change.
          peer.settings(revision: 8, sleepTimeout: 3, lightSquares: 0);
          await tester.pump();
          expect(confirmed, isNull);
          peer.settings(
            count: 8,
            revision: 8,
            sleepTimeout: change.timeout,
            wakeMode: change.mode,
            lightSquares: change.light,
          );
          await tester.pump();
          expect(confirmed, isNull);
          peer.settings(
            count: 1,
            firstFragment: 8,
            revision: 8,
            sleepTimeout: change.timeout,
            wakeMode: change.mode,
            lightSquares: change.light,
          );
          for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
            await tester.pump();
            await tester.runAsync(() async {});
          }
          expect(confirmed, isTrue);
          expect(peer.service.latestSettingValue(change.key), change.value);
          expect(
            peer.native!.writes,
            isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
          );
        } finally {
          await _flushSdkCancellation(tester, peer.close());
        }
      },
    );
  }

  testWidgets('None placement uses typed settings and coherent live readback', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings(revision: 7);
    };
    try {
      await _openRecoveryPeer(tester, peer);
      await tester.pump();
      expect(
        peer.service.latestSettingValue('SETTLE_ANIM_STYLE'),
        'mini_orbit',
      );
      peer.onStateQuery = () {};
      peer.onOrdered = () => peer.finish(revision: 8);
      final writing = peer.service.writeSettingAcknowledged(
        deviceId: 'board-1',
        key: 'SETTLE_ANIM_STYLE',
        value: 'none',
      );
      await tester.pump();
      final result = await writing as LitchSettingWriteAcknowledged;
      expect(result.outcome.ok, isTrue);
      final command = peer.native!.writeRecords
          .where(
            (write) =>
                write.characteristicId ==
                BleProtocol.connectionSessionIngressCharacteristic,
          )
          .map(
            (write) =>
                ConnectionSessionCodec.decode(Uint8List.fromList(write.value))!,
          )
          .singleWhere(
            (packet) =>
                packet.kind == ConnectionSessionKind.ordered &&
                packet.code == SettingsStateWireCodec.mutationOpcode,
          );
      expect(command.body, <int>[2, 30, 3, 0]);
      bool? confirmed;
      result.boardStateConfirmation!.then((value) => confirmed = value);
      peer.settings(count: 8, revision: 8, placementStyle: 3);
      await tester.pump();
      expect(confirmed, isNull);
      expect(
        peer.service.latestSettingValue('SETTLE_ANIM_STYLE'),
        'mini_orbit',
      );
      peer.settings(count: 1, firstFragment: 8, revision: 8, placementStyle: 3);
      for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
        await tester.pump();
        await tester.runAsync(() async {});
      }
      expect(confirmed, isTrue);
      expect(peer.service.latestSettingValue('SETTLE_ANIM_STYLE'), 'none');
      expect(
        peer.native!.writes,
        isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
      );
    } finally {
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  testWidgets(
    'Beachball Settings uses typed mutation and coherent readback after reconnect',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7, hintStyle: 1);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        expect(
          peer.service.latestSettingValue('HINT_THINKING_STYLE'),
          'player_clockwise',
        );
        peer.onStateQuery = () {};
        peer.onOrdered = () => peer.finish(revision: 8);
        final writing = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'HINT_THINKING_STYLE',
          value: 'beachball',
        );
        await tester.pump();
        final result = await writing as LitchSettingWriteAcknowledged;
        expect(result.outcome.ok, isTrue);
        final command = peer.native!.writeRecords
            .where(
              (write) =>
                  write.characteristicId ==
                  BleProtocol.connectionSessionIngressCharacteristic,
            )
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write.value),
              )!,
            )
            .singleWhere(
              (packet) =>
                  packet.kind == ConnectionSessionKind.ordered &&
                  packet.code == SettingsStateWireCodec.mutationOpcode,
            );
        expect(command.body, <int>[2, 39, 3, 0]);
        expect(
          peer.native!.writes,
          isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
        );
        expect(peer.actions, 1);
        bool? confirmed;
        result.boardStateConfirmation!.then((value) => confirmed = value);
        peer.settings(count: 8, revision: 8, hintStyle: 3);
        peer.settings(count: 1, firstFragment: 8, revision: 8, hintStyle: 2);
        await tester.pump();
        expect(
          confirmed,
          isNull,
          reason: 'A different-identity tail cannot confirm Beachball.',
        );
        expect(
          peer.service.latestSettingValue('HINT_THINKING_STYLE'),
          'player_clockwise',
        );
        peer.settings(revision: 8, hintStyle: 3);
        for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(confirmed, isTrue);
        expect(
          peer.service.latestSettingValue('HINT_THINKING_STYLE'),
          'beachball',
        );
        await _flushSdkCancellation(tester, peer.service.disconnect());
        peer.service.debugSetConnectedDevice('board-1');
        peer.service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.gameHistoryBit,
            explicitlyReported: true,
          ),
        );
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 8, hintStyle: 3);
        };
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.confirmed,
        );
        expect(
          peer.service.latestSettingValue('HINT_THINKING_STYLE'),
          'beachball',
        );
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'typed Settings authority keeps accepted same-value enum pending until matching tail',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 6);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        expect(peer.service.boardStateSettingsRevision, 6);
        final value = peer.service.latestSettingValue('TURN_ANIM_STYLE')!;
        peer.onOrdered = () => peer.finish(revision: 7);
        final writing = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'TURN_ANIM_STYLE',
          value: value,
        );
        await tester.pump();
        final result = await writing as LitchSettingWriteAcknowledged;
        expect(
          result.outcome.ok,
          isTrue,
          reason: 'Ordered accepts the unchanged canonical enum',
        );
        expect(peer.actions, 1);
        bool? confirmed;
        result.boardStateConfirmation!.then((value) => confirmed = value);
        for (var turn = 0; turn < 8; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(
          confirmed,
          isNull,
          reason: 'The prior binary revision cannot confirm the new mutation',
        );
        final identity = peer.settingsIdentity(
          peer.settingsPayload(revision: 7),
        );
        peer.settings(
          count: 1,
          firstFragment: 8,
          revision: 7,
          identity: identity ^ 1,
        );
        await tester.pump();
        expect(
          confirmed,
          isNull,
          reason:
              'a different snapshot identity cannot supply the missing tail',
        );
        peer.settings(count: 8, revision: 7);
        await tester.pump();
        expect(confirmed, isNull);
        peer.settings(count: 1, firstFragment: 8, revision: 7);
        for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(confirmed, isTrue);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'typed Settings authority publishes first complete unchanged snapshot',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(count: 8, revision: 7);
      };
      StreamSubscription<String>? subscription;
      try {
        await _openRecoveryPeer(tester, peer);
        await peer.startConsole(tester);
        peer.consoleSettings();
        await tester.pump();
        final descriptors = await peer.readMetadata(tester);
        expect(descriptors.single.description, 'Turn indicator');
        final changes = <String>[];
        final observedAuthorities = <LitchSettingsReadAuthority>[];
        subscription = peer.service.settingsChangedStream.listen((key) {
          changes.add(key);
          observedAuthorities.add(
            peer.service.settingsReadAuthority('board-1'),
          );
        });
        peer.settings(count: 1, firstFragment: 8, revision: 7);
        await tester.pump();
        expect(
          changes,
          contains('TURN_ANIM_STYLE'),
          reason: 'unchanged values must publish the first binary authority',
        );
        expect(observedAuthorities.toSet(), {
          LitchSettingsReadAuthority.confirmed,
        });
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.confirmed,
        );
        final count = changes.length;
        peer.settings(revision: 7);
        await tester.pump();
        expect(changes, hasLength(count), reason: 'exact replay stays quiet');
      } finally {
        await _flushSdkCancellation(
          tester,
          subscription?.cancel() ?? Future<void>.value(),
        );
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'typed Settings authority rejects metadata and retired tail after reconnect',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(count: 8, revision: 7);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await peer.startConsole(tester);
        peer.consoleSettings();
        await tester.pump();
        expect(await peer.readMetadata(tester), hasLength(1));
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.unavailable,
          reason: 'complete immutable descriptors cannot grant live authority',
        );
        peer.onOrdered = () => peer.finish(revision: 7);
        final writing = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'TURN_ANIM_STYLE',
          value: peer.service.latestSettingValue('TURN_ANIM_STYLE')!,
        );
        await tester.pump();
        final result = await writing as LitchSettingWriteAcknowledged;
        expect(result.outcome.ok, isTrue);
        bool? confirmed;
        result.boardStateConfirmation!.then((value) => confirmed = value);
        final retired = peer.native!;
        await _flushSdkCancellation(tester, peer.service.disconnect());
        for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(
          confirmed,
          isFalse,
          reason: 'retirement cancels the pending editor',
        );
        peer.service.debugSetConnectedDevice('board-1');
        peer.service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.gameHistoryBit,
            explicitlyReported: true,
          ),
        );
        await _openRecoveryPeer(tester, peer);
        await peer.startConsole(tester);
        peer.consoleSettings();
        await tester.pump();
        await peer.readMetadata(tester);
        final active = peer.native;
        peer.native = retired;
        peer.settings(count: 1, firstFragment: 8, revision: 7);
        peer.native = active;
        await tester.pump();
        expect(confirmed, isFalse);
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.unavailable,
          reason: 'retired packets cannot finish a new connection snapshot',
        );
        peer.settings(count: 1, firstFragment: 8, revision: 7);
        await tester.pump();
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.confirmed,
        );
        expect(
          confirmed,
          isFalse,
          reason: 'new authority cannot revive a retired edit',
        );
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets('typed Settings restores unchanged board after reconnect', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings(revision: 7, brightness: 8, setupGuidance: 0);
    };
    try {
      await _openRecoveryPeer(tester, peer);
      await tester.pump();
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 8);
      await _flushSdkCancellation(tester, peer.service.disconnect());
      expect(peer.service.hasBoardStateSnapshot, isFalse);
      peer.service.debugSetConnectedDevice('board-1');
      peer.service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.gameHistoryBit |
              LitchDeviceCapabilities.settingsBit,
          explicitlyReported: true,
        ),
      );
      await _openRecoveryPeer(tester, peer);
      await tester.pump();
      expect(
        peer.service.hasBoardStateSnapshot,
        isTrue,
        reason: 'same firmware CRC belongs to a fresh connection lifetime',
      );
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 8);
      expect(peer.actions, 0);
      peer.native!.readResponses[BleProtocol.settingsEnumerateCharacteristic] =
          utf8.encode(
            jsonEncode([
              {
                'i': 28,
                'k': 'BOARD_SETUP_ASSISTANCE',
                't': 'bool',
                'c': 'behavior',
                'v': true,
                'df': true,
                'd': 'Board setup assistance',
              },
            ]),
          );
      SharedPreferences.setMockInitialValues({});
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            body: SettingsScreen(
              bleService: peer.service,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 100));
      final search = find.byKey(const ValueKey<String>('advanced-search'));
      expect(find.byKey(const ValueKey<String>('setup-search')), findsNothing);
      await tester.ensureVisible(search);
      await tester.pumpAndSettle();
      expect(search.hitTestable(), findsOneWidget);
      await tester.enterText(search, 'board setup');
      await tester.pump(const Duration(milliseconds: 200));
      final guidance = find.byKey(
        const ValueKey<String>('setting-editor:BOARD_SETUP_ASSISTANCE'),
      );
      expect(
        tester.widget<Switch>(guidance).value,
        isFalse,
        reason: 'Setup shows restored board authority rather than a default',
      );
      expect(
        tester.widget<Switch>(guidance).onChanged,
        isNotNull,
        reason: 'reconnected board controls are ready',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    } finally {
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  testWidgets('typed Settings confirms same value at newer document revision', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings(revision: 7, brightness: 8);
    };
    try {
      await _openRecoveryPeer(tester, peer);
      await tester.pump(const Duration(milliseconds: 10));
      peer.onOrdered = () => peer.finish(revision: 8);
      peer.onStateQuery = () => peer.settings(revision: 8, brightness: 8);
      final writing = peer.service.writeSettingAcknowledged(
        deviceId: 'board-1',
        key: 'BRIGHTNESS',
        value: 8,
      );
      await tester.pump(const Duration(milliseconds: 10));
      final result = await writing as LitchSettingWriteAcknowledged;
      bool? confirmed;
      result.boardStateConfirmation!.then((value) => confirmed = value);
      // The successful reader cancels its stream subscription before returning.
      // Drain that SDK/stream cleanup without advancing the confirmation timer.
      for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
        await tester.pump();
        await tester.runAsync(() async {});
      }
      expect(
        confirmed,
        isTrue,
        reason:
            'a coherent newer same-value reply is confirmation, without a change event',
      );
      expect(peer.actions, 1);
    } finally {
      await _flushSdkCancellation(tester, peer.close());
      await tester.pump(const Duration(seconds: 2));
    }
  });

  testWidgets(
    'typed Settings confirmation rejects retired reply after reconnect',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        peer.onOrdered = () => peer.finish(revision: 8);
        peer.onStateQuery = () =>
            peer.settings(count: 2, revision: 8, brightness: 8);
        final writing = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
          value: 8,
        );
        await tester.pump();
        final result = await writing as LitchSettingWriteAcknowledged;
        bool? confirmed;
        result.boardStateConfirmation!.then((value) => confirmed = value);
        final retired = peer.native!;
        await _flushSdkCancellation(tester, peer.service.disconnect());
        for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(
          confirmed,
          isFalse,
          reason:
              'retirement cancels confirmation without waiting for its deadline',
        );
        peer.service.debugSetConnectedDevice('board-1');
        peer.service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.gameHistoryBit,
            explicitlyReported: true,
          ),
        );
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 9, brightness: 9);
        };
        await _openRecoveryPeer(tester, peer);
        final queries = peer.queries;
        final active = peer.native;
        peer.native = retired;
        peer.settings(revision: 8, brightness: 8);
        peer.native = active;
        await tester.pump(const Duration(milliseconds: 500));
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
        expect(confirmed, isFalse);
        expect(
          peer.queries,
          queries,
          reason: 'retired partial assembly cannot restart repair',
        );
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets('typed Settings rejects intent addressed to a different board', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    try {
      await _openRecoveryPeer(tester, peer);
      await tester.pump();
      peer.onOrdered = () => peer.finish(revision: 1);
      final writing = peer.service.writeSettingAcknowledged(
        deviceId: 'retired-board',
        key: 'BRIGHTNESS',
        value: 8,
      );
      await tester.pump();
      final result = await writing;
      expect(
        peer.actions,
        0,
        reason: 'validate requested board before native admission',
      );
      expect(result, isA<LitchSettingWriteNotAcknowledged>());
    } finally {
      await _flushSdkCancellation(tester, peer.close());
      await tester.pump(const Duration(seconds: 2));
    }
  });
  testWidgets('typed State passive repair leaves healthy publications quiet', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings(revision: 7, brightness: 9);
    };
    final changed = <String>[];
    final subscription = peer.service.settingsChangedStream.listen(changed.add);
    try {
      await _openRecoveryPeer(tester, peer);
      await tester.pump();
      changed.clear();
      final queries = peer.queries;
      peer.home(revision: 2, action: HomeStateAction.idle);
      peer.native!.emit(
        BleProtocol.connectionSessionEgressCharacteristic,
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 46,
            token: 0x80000001,
            sequence: 0x12345678,
            body: Uint8List.fromList([1, 8, 0, 0, 0, 43, 200, 2]),
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
      expect(peer.service.latestSettingValue('P1_END_PIECE_WHEEL'), 43);
      expect(peer.service.latestSettingValue('P1_PIECE_WHEEL'), 43);
      expect(peer.service.latestSettingValue('P2_END_PIECE_WHEEL'), 200);
      expect(peer.service.latestSettingValue('P2_PIECE_WHEEL'), 200);
      expect(peer.service.latestSettingValue('GAME_FIRST_PLAYER'), 2);
      expect(changed, contains('P1_END_PIECE_WHEEL'));
      expect(peer.service.boardStateSettingsRevision, 7);
      expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
      expect(
        peer.queries,
        queries,
        reason: 'healthy complete publications and live palette need no Query',
      );
    } finally {
      await _flushSdkCancellation(tester, subscription.cancel());
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  testWidgets(
    'typed State passive repair coalesces partial Home and Settings',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        final queries = peer.queries;
        var repairs = 0;
        peer.onStateQuery = () {
          repairs++;
          if (repairs == 1) return; // Entire first response is lost.
          peer.home(revision: 2, action: HomeStateAction.idle);
          peer.settings(revision: 8, brightness: 9);
        };
        for (var duplicate = 0; duplicate < 3; ++duplicate) {
          peer.home(count: 3, revision: 2, action: HomeStateAction.idle);
          peer.settings(count: 2, revision: 8, brightness: 9);
        }
        await tester.pump();
        expect(
          peer.queries,
          queries,
          reason: 'observe the normal remainder before repair',
        );
        for (
          var turn = 0;
          turn < 6 && peer.service.latestSettingValue('BRIGHTNESS') != 9;
          ++turn
        ) {
          await tester.pump(const Duration(milliseconds: 125));
        }
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
        expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
        expect(
          repairs,
          2,
          reason:
              'one shared reader repairs both snapshots and a lost response',
        );
        await tester.pump(const Duration(milliseconds: 500));
        expect(repairs, 2, reason: 'coherent completion retires recovery');
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  test(
    'durable current-line State uses the shared parser and rejects invalid or stale publications',
    () async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      final observed = <CurrentGameLineState>[];
      final subscription = peer.service.currentGameLineStream.listen(
        observed.add,
      );
      try {
        await peer.open();
        void publish(
          int revision,
          List<int> body, {
          int code = CurrentGameLineState.stateCode,
        }) {
          peer.native!.emit(
            BleProtocol.connectionSessionEgressCharacteristic,
            ConnectionSessionCodec.encode(
              ConnectionSessionPacket(
                kind: ConnectionSessionKind.state,
                code: code,
                token: 0x80000001,
                sequence: revision,
                body: Uint8List.fromList(body),
              ),
            ),
          );
        }

        // Fox owns State 44. Even a body shaped like a line publication cannot
        // become history authority when it arrives on the other domain's key.
        publish(7, [1, 1, 0, 0, 0, 0], code: 44);
        publish(8, [1, 1], code: 44);
        await Future<void>.delayed(Duration.zero);
        expect(observed, isEmpty);
        publish(10, [1, 1]);
        await Future<void>.delayed(Duration.zero);
        publish(10, [1, 0]);
        publish(9, [1, 0]);
        publish(11, [2, 0]);
        publish(12, [1, 2]);
        publish(13, [1, 0, 0]);
        await Future<void>.delayed(Duration.zero);
        expect(observed.map((state) => state.revision), [10]);
        publish(11, [1, 0]);
        await Future<void>.delayed(Duration.zero);
        expect(observed.map((state) => state.revision), [10, 11]);
        expect(observed.map((state) => state.present), [true, false]);
      } finally {
        await subscription.cancel();
        await peer.close();
      }
    },
    timeout: const Timeout(Duration(seconds: 10)),
  );

  testWidgets(
    'typed State rejects malformed complete repair before valid replay',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        var repairs = 0;
        peer.onStateQuery = () {
          repairs++;
          if (repairs == 1) {
            final home = peer.fixture(
              'home_state_wire_v1.csv',
              'representative',
            )..[0] = 255;
            final settings = peer.fixture(
              'settings_state_wire_v3.csv',
              'defaults',
            )..[70] = 1;
            peer.stateFragments(home, 4, 10, 55);
            peer.stateFragments(settings, 14, 9, 66);
          } else {
            peer.home(revision: 3, action: HomeStateAction.idle);
            peer.settings(revision: 9, brightness: 9);
          }
        };
        peer.home(count: 3, revision: 2);
        peer.settings(count: 2, revision: 8, brightness: 9);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 125));
        expect(repairs, 1);
        expect(peer.service.latestHomeState!.action, HomeStateAction.cpu);
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 5);
        await tester.pump(const Duration(milliseconds: 125));
        expect(
          repairs,
          2,
          reason: 'invalid full payloads cannot stop the common reader',
        );
        expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  for (final passiveFirst in [false, true]) {
    testWidgets(
      'typed Settings slow partial confirmation owns concurrent passive repair passiveFirst=$passiveFirst',
      (tester) async {
        final peer = _RecoverySharedPeer();
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 7);
        };
        try {
          await _openRecoveryPeer(tester, peer);
          if (passiveFirst) {
            peer.home(count: 3, revision: 2, action: HomeStateAction.idle);
            await tester.pump();
          }
          peer.onOrdered = () => peer.finish(revision: 8);
          var repairs = 0;
          peer.onStateQuery = () {
            repairs++;
            if (repairs == 1) return; // Entire first confirmation reply lost.
            if (repairs == 2) {
              peer.settings(count: 2, revision: 8, brightness: 9);
              return;
            }
            peer.settings(revision: 8, brightness: 9);
            peer.home(revision: 2, action: HomeStateAction.idle);
          };
          final writing = peer.service.writeSettingAcknowledged(
            deviceId: 'board-1',
            key: 'BRIGHTNESS',
            value: 9,
          );
          await tester.pump();
          final result = await writing as LitchSettingWriteAcknowledged;
          bool? confirmed;
          result.boardStateConfirmation!.then((value) => confirmed = value);
          expect(repairs, 1);
          // An unrelated ordinary Home publication loses its tail while the active
          // settings reader is waiting. Repeated partial packets must not add a reader.
          for (var duplicate = 0; duplicate < 3; ++duplicate) {
            peer.home(count: 3, revision: 2, action: HomeStateAction.idle);
          }
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 125));
          expect(repairs, 2);
          expect(confirmed, isNull);
          expect(peer.service.latestSettingValue('BRIGHTNESS'), 5);
          await tester.pump(const Duration(milliseconds: 125));
          for (var turn = 0; turn < 8 && confirmed == null; ++turn) {
            await tester.pump();
            await tester.runAsync(() async {});
          }
          expect(confirmed, isTrue);
          expect(
            repairs,
            3,
            reason: 'one active reader, with one lost and one partial response',
          );
          expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
          await tester.pump(const Duration(milliseconds: 500));
          expect(repairs, 3, reason: 'passive repair adds no background query');
        } finally {
          await _flushSdkCancellation(tester, peer.close());
        }
      },
    );
  }

  testWidgets(
    'generic explicit State completion leaves unrelated partial Home repair live',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      try {
        debugPrint('RECOVERY: opening');
        await _openRecoveryPeer(tester, peer);
        debugPrint('RECOVERY: open');
        final session = (await peer.service.openConnectionSession('board-1'))!;
        final queries = peer.queries;
        peer.onStateQuery = () {};
        final reading = session.readState(code: 38);
        await tester.pump();
        peer.home(count: 3, revision: 2, action: HomeStateAction.idle);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 125));
        expect(
          peer.queries - queries,
          2,
          reason: 'only explicit reader probes while pending',
        );
        expect(peer.stateQueryCodes.sublist(queries), [38, 38]);
        peer.stateFragments(Uint8List(10)..[0] = 1, 38, 1, 2);
        for (var turn = 0; turn < 8; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        await reading;
        peer.onStateQuery = () =>
            peer.home(revision: 2, action: HomeStateAction.idle);
        await tester.pump(const Duration(milliseconds: 125));
        expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
        expect(peer.stateQueryCodes.last, 0);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  for (final succeeds in [true, false]) {
    test(
      'staggered passive deadline then explicit State succeeds=$succeeds stays bounded and recoverable',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 7);
        };
        final session = (await peer.service.openConnectionSession('board-1'))!;
        await Future<void>.delayed(const Duration(milliseconds: 30));
        peer.onStateQuery = () {};
        peer.home(count: 3, revision: 2, action: HomeStateAction.idle);
        peer.settings(count: 2, revision: 8, brightness: 9);
        // Stagger actual common monotonic deadlines; fake time does not advance
        // the production Stopwatch that enforces their absolute bound.
        await Future<void>.delayed(const Duration(milliseconds: 1500));
        final passiveBeforeActive = peer.stateQueryCodes
            .where((code) => code == 0)
            .length;
        final reading = session.readState(code: 38);
        final failed = succeeds
            ? null
            : expectLater(reading, throwsA(isA<TimeoutException>()));
        await Future<void>.delayed(const Duration(milliseconds: 650));
        expect(peer.service.latestHomeState!.action, HomeStateAction.cpu);
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 5);
        expect(
          peer.stateQueryCodes.where((code) => code == 0).length,
          passiveBeforeActive,
          reason:
              'passive deadline expires while later active reader owns probes',
        );
        if (succeeds) {
          peer.onStateQuery = () {
            if (peer.stateQueryCodes.last == 0) {
              peer.home(revision: 2, action: HomeStateAction.idle);
              peer.settings(revision: 8, brightness: 9);
            }
          };
          peer.stateFragments(Uint8List(10)..[0] = 1, 38, 1, 2);
          await reading;
          await Future<void>.delayed(const Duration(milliseconds: 250));
          expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
          expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
          expect(
            peer.stateQueryCodes.where((code) => code == 0).length,
            passiveBeforeActive + 1,
          );
        } else {
          await failed;
          final exhaustedQueries = peer.queries;
          await Future<void>.delayed(const Duration(milliseconds: 300));
          expect(
            peer.queries,
            exhaustedQueries,
            reason: 'neither failure restarts a timeout loop',
          );
          peer.onStateQuery = () {
            peer.home(revision: 2, action: HomeStateAction.idle);
            peer.settings(revision: 8, brightness: 9);
          };
          expect(
            await peer.service.requestCurrentHomeStateSnapshot('board-1'),
            isTrue,
          );
          expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
          expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
        }
        final completedQueries = peer.queries;
        await Future<void>.delayed(const Duration(milliseconds: 250));
        expect(peer.queries, completedQueries);
      },
    );
  }

  testWidgets('typed Settings unacknowledged API adds no confirmation query', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings(revision: 7);
    };
    try {
      await _openRecoveryPeer(tester, peer);
      final queries = peer.queries;
      peer.onOrdered = () => peer.finish(revision: 8);
      final writing = peer.service.writeSetting(
        deviceId: 'board-1',
        key: 'BRIGHTNESS',
        value: 9,
      );
      await tester.pump();
      expect(await writing, isTrue);
      await tester.pump(const Duration(milliseconds: 500));
      expect(peer.actions, 1);
      expect(peer.queries, queries);
    } finally {
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  testWidgets(
    'typed State passive repair retires before disconnect can query',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        final queries = peer.queries;
        peer.settings(count: 2, revision: 8, brightness: 9);
        await tester.pump();
        await _flushSdkCancellation(tester, peer.service.disconnect());
        await tester.pump(const Duration(milliseconds: 500));
        expect(peer.queries, queries);
        expect(peer.service.hasBoardStateSnapshot, isFalse);
        expect(peer.service.latestSettingValue('BRIGHTNESS'), isNull);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  test(
    'typed Home zero and partial replies recover, quiet repeated replay stays healthy',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        if (peer.queries == 1) return;
        if (peer.queries == 2) {
          peer.home(count: 3);
          peer.settings(count: 2);
          return;
        }
        peer.home();
        peer.settings();
      };
      await peer.open();
      await peer.service.homeStateStream.first.timeout(
        const Duration(seconds: 1),
      );
      expect(peer.queries, greaterThanOrEqualTo(3));
      final home = peer.service.latestHomeState!;
      expect(
        home.position.fen,
        'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
      );
      expect(
        await peer.service.requestCurrentHomeStateSnapshot('board-1'),
        isTrue,
      );
      expect(
        await peer.service.requestCurrentHomeStateSnapshot('board-1'),
        isTrue,
      );
      expect(peer.service.latestHomeState, same(home));
      expect(peer.ble.commandWrites, isEmpty);
      expect(peer.actions, 0);
      expect(peer.sessions.openAttempts, 1);
    },
  );

  for (final partial in [false, true]) {
    test(
      'typed Home bounded absent completion is restartable partial=$partial',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        await peer.open();
        await _waitFor(() => peer.service.latestHomeState != null);
        final home = peer.service.latestHomeState;
        peer.onStateQuery = () {
          if (partial) peer.home(count: 4, revision: 2);
        };
        final clock = Stopwatch()..start();
        expect(
          await peer.service.requestCurrentHomeStateSnapshot('board-1'),
          isFalse,
        );
        expect(clock.elapsedMilliseconds, inInclusiveRange(1900, 3000));
        expect(peer.service.latestHomeState, same(home));
        peer.onStateQuery = () => peer.home();
        expect(
          await peer.service.requestCurrentHomeStateSnapshot('board-1'),
          isTrue,
        );
        expect(peer.ble.commandWrites, isEmpty);
        expect(peer.sessions.openAttempts, 1);
      },
    );
  }

  test(
    'typed Home initial combined replay requires coherent Settings too',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings(count: 2);
      };
      await peer.open();
      await Future<void>.delayed(const Duration(milliseconds: 350));
      expect(peer.service.latestHomeState, isNotNull);
      expect(
        peer.queries,
        greaterThanOrEqualTo(3),
        reason: 'Home alone cannot complete combined replay',
      );
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      await Future<void>.delayed(const Duration(milliseconds: 250));
      final queries = peer.queries;
      await Future<void>.delayed(const Duration(milliseconds: 250));
      expect(
        peer.queries,
        queries,
        reason: 'coherent pair terminates shared replay',
      );
    },
  );

  test('typed Home disconnect prevents stale snapshot completion', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    await peer.open();
    await _waitFor(() => peer.service.latestHomeState != null);
    peer.onStateQuery = () {};
    final replay = peer.service.requestCurrentHomeStateSnapshot('board-1');
    await Future<void>.delayed(Duration.zero);
    final oldNative = peer.native!;
    await peer.service.disconnect();
    expect(await replay.timeout(const Duration(milliseconds: 300)), isFalse);
    expect(peer.service.isConnected, isFalse);
    peer.home(revision: 9);
    await Future<void>.delayed(Duration.zero);
    expect(peer.service.latestHomeState, isNull);
    expect(peer.actions, 0);
    peer.service.debugSetConnectedDevice('board-2');
    peer.service.debugSetDeviceCapabilities(
      'board-2',
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask: LitchDeviceCapabilities.gameHistoryBit,
        explicitlyReported: true,
      ),
    );
    peer.onStateQuery = () {
      peer.home(action: HomeStateAction.idle);
      peer.settings();
    };
    await peer.open(deviceId: 'board-2');
    await _waitFor(() => peer.service.latestHomeState != null);
    expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
    final stale = peer.fixture('home_state_wire_v1.csv', 'representative');
    for (var i = 0; i < 10; ++i) {
      oldNative.emit(
        BleProtocol.connectionSessionEgressCharacteristic,
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 4 + i,
            token: 0x80000001,
            sequence: 99,
            body: Uint8List.sublistView(stale, i * 10, (i + 1) * 10),
          ),
        ),
      );
    }
    await Future<void>.delayed(Duration.zero);
    expect(peer.service.latestHomeState!.action, HomeStateAction.idle);
    expect(peer.service.connectedId, 'board-2');
  });

  test(
    'shared control remains pending beyond six seconds and applies once',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      await peer.open();
      final owner = BoardActionsController(
        bleService: peer.service,
        boardId: 'board-1',
      );
      addTearDown(owner.dispose);
      final events = <LitchControlEvent>[];
      final sub = peer.service.controlActionEvents.listen(events.add);
      addTearDown(sub.cancel);
      final work = peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.newGame,
      );
      var completed = false;
      work.then((_) => completed = true);
      await Future<void>.delayed(const Duration(milliseconds: 6500));
      expect(completed, isFalse);
      expect(events.last.phase, LitchControlPhase.pending);
      expect(owner.hasPendingAction, isTrue);
      expect(peer.actions, 1);
      peer.finish();
      expect((await work).phase, LitchControlPhase.applied);
      expect(peer.actions, 1);
      expect(events.where((event) => event.isTerminal), hasLength(1));
      expect(owner.hasPendingAction, isFalse);
    },
  );

  test('Identify stop uses current Ordered packets at minimum MTU', () async {
    final ble = _ReconnectBle()..mtuReply = (Completer<int>()..complete(23));
    final peer = _RecoverySharedPeer(nativeBle: ble);
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    peer.onOrdered = peer.finish;
    await peer.open();
    expect(await peer.service.sendIdentifyStop('board-1'), isTrue);
    final records = peer.native!.writeRecords
        .where(
          (record) =>
              record.characteristicId ==
              BleProtocol.connectionSessionIngressCharacteristic,
        )
        .map(
          (record) =>
              ConnectionSessionCodec.decode(Uint8List.fromList(record.value))!,
        )
        .where((packet) => packet.kind == ConnectionSessionKind.ordered)
        .toList();
    expect(records.map((packet) => packet.code), [28, 28]);
    expect(records.map((packet) => packet.body), [
      [1, 0, 0, 0, 0, 0, 0, 0],
      [1, 2, 0, 0, 0, 0, 0, 0],
    ]);
    expect(
      peer.native!.writeRecords.map((record) => record.value.length),
      everyElement(lessThanOrEqualTo(20)),
    );
    expect(
      peer.native!.writes,
      isNot(contains(BleProtocol.commandCharacteristic)),
    );
  });

  test(
    'responsive shared control obeys the fixed received-head deadline',
    () async {
      expect(
        LitchBleService.orderedHeadCompletionTimeout,
        const Duration(seconds: 20),
      );
      const completionTimeout = Duration(seconds: 3);
      final peer = _RecoverySharedPeer(
        receivedHeadCompletionTimeout: completionTimeout,
      );
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      await peer.open();
      final session = (await peer.service.openConnectionSession('board-1'))!;
      expect(session.receivedHeadCompletionTimeout, completionTimeout);
      expect(completionTimeout, greaterThan(session.recoveryFailureTimeout));
      final clock = Stopwatch()..start();
      final result = await peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.undo,
      );
      // Preserve the original 200 ms early tolerance and 2 s scheduling margin.
      expect(
        clock.elapsedMilliseconds,
        inInclusiveRange(
          completionTimeout.inMilliseconds - 200,
          completionTimeout.inMilliseconds + 2000,
        ),
      );
      expect(result.phase, LitchControlPhase.timeout);
      expect(peer.actions, 1);
    },
    timeout: const Timeout(Duration(seconds: 30)),
  );

  test(
    'shared control generation disconnect interrupts pending authority',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      await peer.open();
      final work = peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.undo,
      );
      await _waitFor(() => peer.actions == 1);
      await peer.service.disconnect();
      expect(
        (await work.timeout(const Duration(milliseconds: 300))).phase,
        LitchControlPhase.interrupted,
      );
      expect(peer.actions, 1);
    },
  );

  test('shared control preempts a held native Object batch', () async {
    final sessions = _RecordingGattSessionFactory();
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _ReconnectBle(),
      gattSessionFactory: sessions,
    );
    addTearDown(() async {
      await service.dispose();
      await sessions.close();
    });
    service.debugSetConnectedDevice('board-1');
    await _waitFor(() => sessions.sessions.isNotEmpty);
    final native = sessions.sessions.single;
    final held = Completer<void>();
    native.holdSharedBatchWrites = held;
    final batch = service.writeConnectionSessionPackets('board-1', [
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.object,
          code: 40,
          token: 3,
          sequence: 1,
          body: Uint8List.fromList([1]),
        ),
      ),
    ]);
    await _waitFor(() => native.batchWrites.isNotEmpty);
    var controlAdmitted = false;
    final query = service
        .writeConnectionSessionPacket(
          'board-1',
          ConnectionSessionCodec.encode(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.control,
              code: ConnectionSessionControl.query,
              token: 3,
              sequence: 0,
              body: Uint8List(0),
            ),
          ),
        )
        .then((_) => controlAdmitted = true);
    await Future<void>.delayed(const Duration(milliseconds: 30));
    final preempted = controlAdmitted;
    held.complete();
    await batch;
    await query;
    expect(
      preempted,
      isTrue,
      reason: 'Query must reach native before stalled bulk completes',
    );
  });

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      'Console keeps its draft and history through platform unplug and reconnect',
      (tester) async {
        await loadHealthReviewFonts(tester);
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetViewInsets);
        addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
        SharedPreferences.setMockInitialValues({'enable_app_console': true});
        final exactGatt = platform == TargetPlatform.android;
        final fakeBle = _AppShellReconnectBle(exactGatt: exactGatt)
          ..useSharedPeer = true
          ..capabilityReply = '{"i":[3,3,31]}';
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: fakeBle,
          gattSessionFactory: exactGatt
              ? ReactiveBleGattSessionFactory(fakeBle)
              : null,
          permissionGateway: _PermissionGateway({
            Permission.bluetooth: PermissionStatus.granted,
            Permission.bluetoothScan: PermissionStatus.granted,
            Permission.bluetoothConnect: PermissionStatus.granted,
            Permission.locationWhenInUse: PermissionStatus.granted,
          }),
        );
        Future<void> flushPlatform(bool Function() ready) async {
          for (int pass = 0; pass < 12 && !ready(); ++pass) {
            await tester.pump(Duration.zero);
            await tester.runAsync(() async {});
            await tester.pump(Duration.zero);
          }
          expect(ready(), isTrue);
        }

        bool cleaned = false;
        Future<void> cleanup() async {
          if (cleaned) return;
          cleaned = true;
          await tester.pumpWidget(const SizedBox.shrink());
          final disposing = _disposeReconnectHarness(service, fakeBle);
          bool done = false;
          unawaited(disposing.then((_) => done = true));
          await flushPlatform(() => done);
          await disposing;
          await fakeBle.discovery.close();
          await fakeBle.sharedPeer.packets.close();
          await fakeBle.sharedPeer.sessionPackets.close();
          // Retired optional mode/clock probes must settle without new writes.
          await tester.pump(const Duration(seconds: 6));
        }

        addTearDown(cleanup);

        Future<void> connect(String boardId, int count) async {
          final connecting = service.connect(boardId);
          await flushPlatform(() => fakeBle.connections.length == count);
          fakeBle.connections.last.add(
            ConnectionStateUpdate(
              deviceId: boardId,
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          await tester.pump();
          expect(await connecting, isTrue);
          for (int pass = 0; pass < 4; ++pass) {
            await tester.pump(const Duration(milliseconds: 10));
          }
          await flushPlatform(() => service.productReady);
          await tester.pump();
        }

        await connect('board-1', 1);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: ConnectionGateScreen(
              bleService: service,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        for (int pass = 0; pass < 8; ++pass) {
          await tester.pump(const Duration(milliseconds: 10));
        }
        await tester.tap(
          find.byKey(const ValueKey('main-destination-console')),
        );
        for (int pass = 0; pass < 4; ++pass) {
          await tester.pump(const Duration(milliseconds: 20));
        }
        final input = find.byKey(const ValueKey('console-command-input'));
        final trace = find.byKey(const ValueKey('console-trace-output-toggle'));
        Future<void> openTrace() async {
          await tester.tap(
            find.byKey(const ValueKey('console-appearance-button')),
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await tester.ensureVisible(trace);
          await tester.pump();
        }

        Future<void> closeTrace() async {
          final cancel = find.byKey(
            const ValueKey('console-appearance-cancel'),
          );
          await tester.ensureVisible(cancel);
          await tester.tap(cancel);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
        }

        final retained = tester.state(find.byType(AppConsoleScreen));
        final oldNotifications = fakeBle.consoleNotificationStreams.last;
        oldNotifications.add(utf8.encode('before unplug diagnostic\npartial'));
        await tester.pump();
        await openTrace();
        await tester.tap(trace);
        await tester.pump();
        expect(service.consoleTraceOutputEnabled, isTrue);
        await closeTrace();
        await tester.enterText(input, 'draft help');
        tester.view.viewInsets = const FakeViewPadding(bottom: 200);
        await tester.pump();
        expect(
          service.consoleTraceOutputEnabled,
          isTrue,
          reason: 'Closing Appearance must leave Trace on until disconnect.',
        );
        final writesAtLoss = fakeBle.consoleLineWrites.length;

        // Physical unplug starts at the real SDK event boundary. The service,
        // gate, retained scaffold and Console are all production objects.
        fakeBle.connections.last.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await tester.pump(const Duration(milliseconds: 16));
        await tester.pump();
        fakeBle.discovery.statusController.add(BleStatus.ready);
        await tester.pump();
        expect(service.isConnected, isFalse);
        expect(tester.state(find.byType(AppConsoleScreen)), same(retained));
        expect(tester.widget<TextField>(input).controller!.text, 'draft help');
        expect(find.textContaining('before unplug diagnostic'), findsOneWidget);
        expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
        expect(tester.testTextInput.isVisible, isTrue);
        expect(service.consoleTraceOutputEnabled, isFalse);
        expect(
          find.byKey(const ValueKey('console-connection-status')),
          findsOneWidget,
        );
        expect(tester.widget<TextField>(input).onSubmitted, isNull);
        expect(
          tester.widget<TextField>(input).textInputAction,
          TextInputAction.unspecified,
        );
        await openTrace();
        expect(tester.widget<SwitchListTile>(trace).onChanged, isNull);
        await closeTrace();
        await tester.tap(input);
        await tester.enterText(input, 'draft while offline');
        await tester.testTextInput.receiveAction(TextInputAction.unspecified);
        await tester.pump();
        expect(fakeBle.consoleLineWrites, hasLength(writesAtLoss));
        expect(
          tester.widget<TextField>(input).controller!.text,
          'draft while offline',
        );
        expect(tester.testTextInput.isVisible, isTrue);
        oldNotifications.add(utf8.encode('late old session\n'));
        await tester.pump();
        expect(
          service.consoleLinesSnapshot.any(
            (line) => line.contains('late old session'),
          ),
          isFalse,
        );
        await captureHealthReview(tester, 'console-unplug-${platform.name}');
        tester.view.physicalSize = const Size(844, 390);
        tester.platformDispatcher.textScaleFactorTestValue = 2;
        await tester.pump();
        await tester.pump();
        final status = find.byKey(const ValueKey('console-connection-status'));
        expect(
          tester.getRect(status).bottom,
          lessThanOrEqualTo(tester.getRect(input).top),
        );
        expect(tester.getRect(input).bottom, lessThanOrEqualTo(190));
        expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
        expect(tester.testTextInput.isVisible, isTrue);
        expect(tester.takeException(), isNull);
        await captureHealthReview(
          tester,
          'console-unplug-${platform.name}-landscape-200',
        );
        tester.view.physicalSize = const Size(390, 844);
        tester.platformDispatcher.clearTextScaleFactorTestValue();
        await tester.pump();

        await connect('board-1', 2);
        fakeBle.consoleNotificationStreams.last.add(
          utf8.encode('after reconnect diagnostic\n'),
        );
        await tester.pump();
        await tester.pump();
        expect(tester.state(find.byType(AppConsoleScreen)), same(retained));
        expect(
          tester.widget<TextField>(input).controller!.text,
          'draft while offline',
        );
        expect(find.textContaining('before unplug diagnostic'), findsOneWidget);
        expect(
          find.textContaining('after reconnect diagnostic'),
          findsOneWidget,
        );
        expect(
          service.consoleLinesSnapshot.any(
            (line) => line.contains('partialafter'),
          ),
          isFalse,
        );
        expect(
          tester.widget<TextField>(input).focusNode!.hasFocus,
          isTrue,
          reason: 'Reconnect must preserve focus before another modal opens.',
        );
        expect(tester.testTextInput.isVisible, isTrue);
        await openTrace();
        expect(tester.widget<SwitchListTile>(trace).onChanged, isNotNull);
        expect(tester.widget<SwitchListTile>(trace).value, isFalse);
        await closeTrace();
        await tester.tap(input);
        expect(
          find.byKey(const ValueKey('console-connection-status')),
          findsNothing,
        );
        expect(
          fakeBle.consoleLineWrites,
          isNot(contains('draft while offline\n')),
        );
        await tester.testTextInput.receiveAction(TextInputAction.send);
        await flushPlatform(
          () =>
              fakeBle.consoleLineWrites
                  .where((line) => line == 'draft while offline\n')
                  .length ==
              1,
        );
        expect(
          fakeBle.consoleLineWrites.where(
            (line) => line == 'draft while offline\n',
          ),
          hasLength(1),
        );
        expect(tester.widget<TextField>(input).controller!.text, isEmpty);
        expect(tester.testTextInput.isVisible, isTrue);

        // A different board cannot inherit diagnostic history or an unsent
        // command addressed to the previous board.
        await tester.enterText(input, 'old board draft');
        await connect('board-2', 3);
        await tester.pump();
        expect(
          tester
              .widget<AppConsoleScreen>(find.byType(AppConsoleScreen))
              .boardId,
          'board-2',
        );
        expect(tester.widget<TextField>(input).controller!.text, isEmpty);
        expect(
          service.consoleLinesSnapshot.any(
            (line) => line.contains('before unplug diagnostic'),
          ),
          isFalse,
        );
        expect(
          service.consoleLinesSnapshot.any(
            (line) => line.contains('after reconnect diagnostic'),
          ),
          isFalse,
        );
        expect(fakeBle.consoleLineWrites, isNot(contains('old board draft\n')));
        expect(fakeBle.unexpectedQualifiedCalls, 0);
        expect(fakeBle.unexpectedRetiredHandleCalls, 0);
        expect(tester.takeException(), isNull);
        await cleanup();
      },
      variant: TargetPlatformVariant.only(platform),
    );
  }

  test(
    'connection lifecycle emits structured trace without board identity',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      final _ReconnectBle fakeBle = _ReconnectBle();
      final _MemoryTrace trace = _MemoryTrace();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        appTrace: trace,
      );
      addTearDown(() async {
        await _disposeReconnectHarness(service, fakeBle);
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<bool> connecting = service.connect('private-board-id');
      await _waitFor(() => fakeBle.directConnections.isNotEmpty);
      fakeBle.directConnections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'private-board-id',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);

      expect(trace.records.first.$1, 'ble.connect_requested');
      expect(
        trace.records.any(
          (record) =>
              record.$1 == 'ble.connection_state' &&
              record.$2['state'] == 'connected' &&
              record.$2['generation'] == 1,
        ),
        isTrue,
      );
      expect(trace.records.toString(), isNot(contains('private-board-id')));
    },
  );

  {
    testWidgets(
      'first-player changes preserve Home and Setup colors through reconnect with current typed State',
      (tester) async {
        final ble = _SeatingJourneyBle();
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        );
        addTearDown(
          () => tester.runAsync(() async {
            await _disposeReconnectHarness(service, ble);
            await ble.sharedPeer.packets.close();
            await ble.sharedPeer.sessionPackets.close();
            await ble.discovery.close();
          }),
        );
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final colors = <int, Color>{};
        final filters = <int, ColorFilter>{};
        Future<void> home() async {
          await tester.pumpWidget(
            MaterialApp(
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey<String>('seating-screen-capture'),
                child: child!,
              ),
              theme: buildLitchTheme(),
              home: Scaffold(
                body: HomeScreen(bleService: service, boardId: 'board-1'),
              ),
            ),
          );
          await tester.pump(const Duration(milliseconds: 350));
          final snapshot = tester
              .widget<HomeChessboard>(find.byType(HomeChessboard))
              .snapshot;
          expect(snapshot.playerColorsConfirmed, isTrue);
          expect(snapshot.p1PieceWheel, 40);
          expect(snapshot.p2PieceWheel, 200);
          // With coordinates shown, piece keys name logical algebraic squares.
          final p1 = find.byKey(const ValueKey<String>('piece-e1'));
          final p2 = find.byKey(const ValueKey<String>('piece-e8'));
          expect(
            tester.getCenter(p1).dy > tester.getCenter(p2).dy,
            ble.first == 1,
            reason:
                'Home must mirror physical square orientation after reconnect; '
                'rotate=${ble.rotated} first=${ble.first}',
          );
          expect(snapshot.physicalStatus?.rotated, ble.rotated);
          expect(snapshot.physicalStatus?.p1AtOppositeEnd, ble.first == 2);
          for (final player in [1, 2]) {
            final wheel = player == ble.first ? 40 : 200;
            final piece = player == 1 ? p1 : p2;
            final filter = tester
                .widget<ColorFiltered>(
                  find.descendant(
                    of: piece,
                    matching: find.byType(ColorFiltered),
                  ),
                )
                .colorFilter;
            final card = tester.widget<Container>(
              find.byKey(ValueKey<String>('home-clock-p$player')),
            );
            final color = (card.decoration! as BoxDecoration).color!;
            final accent = litchWheelPreviewColor(wheel);
            expect(
              filter,
              _expectedPieceToneFilter(
                accent,
                invertNeutral: player != ble.first,
              ),
              reason:
                  'the rendered ${player == 1 ? 'White/P1' : 'Black/P2'} '
                  'piece must use physical end wheel $wheel exactly once; '
                  'rotate=${ble.rotated} first=${ble.first} '
                  'current typed state',
            );
            expect(
              color,
              Color.alphaBlend(
                accent.withValues(alpha: 0.10),
                Theme.of(
                  tester.element(
                    find.byKey(ValueKey<String>('home-clock-p$player')),
                  ),
                ).colorScheme.surfaceContainerLow,
              ),
            );
            if (colors.containsKey(wheel)) {
              expect(
                color,
                colors[wheel],
                reason: 'actual card palette follows owner, not rotation',
              );
              if (filters.containsKey(player * 256 + wheel)) {
                expect(
                  filter,
                  filters[player * 256 + wheel],
                  reason: 'actual piece tint survives rotation and reconnect',
                );
              }
            } else {
              colors[wheel] = color;
            }
            filters[player * 256 + wheel] = filter;
          }
          expect(colors[40], isNot(colors[200]));
          // Widget existence precedes asynchronous asset decode. Capture only after
          // the exact resized image providers used by the board have completed.
          final imageContext = tester.element(find.byType(HomeChessboard));
          final providers = tester
              .widgetList<Image>(
                find.descendant(
                  of: find.byType(HomeChessboard),
                  matching: find.byType(Image),
                ),
              )
              .map((image) => image.image)
              .toSet();
          await tester.runAsync(
            () => Future.wait([
              for (final provider in providers)
                precacheImage(provider, imageContext),
            ]),
          );
          await tester.pump();
          await captureHealthReview(
            tester,
            'seating-home-rot${ble.rotated}-first${ble.first}',
            boundaryFinder: find.byKey(
              const ValueKey<String>('seating-screen-capture'),
            ),
          );
        }

        for (final rotated in [false, true]) {
          ble.rotated = rotated;
          final connecting = service.connect('board-1');
          final priorConnections = rotated ? 1 : 0;
          for (
            var pass = 0;
            pass < 8 && ble.connections.length == priorConnections;
            pass++
          ) {
            await tester.pump();
          }
          ble.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          await tester.pump();
          expect(await connecting, isTrue);
          for (var turn = 0; turn < 40 && !service.productReady; ++turn) {
            await tester.pump(const Duration(milliseconds: 10));
            await tester.runAsync(() async {});
          }
          expect(
            service.productReady,
            isTrue,
            reason: 'current session must activate',
          );
          ble.publish();
          await tester.pump();
          for (final first in [1, 2]) {
            await tester.pumpWidget(
              MaterialApp(
                builder: (context, child) => RepaintBoundary(
                  key: const ValueKey<String>('seating-screen-capture'),
                  child: child!,
                ),
                theme: buildLitchTheme(),
                home: Scaffold(
                  body: SettingsScreen(
                    bleService: service,
                    boardId: 'board-1',
                    enableAppConsole: false,
                    onEnableAppConsoleChanged: (_) async {},
                  ),
                ),
              ),
            );
            await tester.pumpAndSettle();
            final editor = find.byKey(
              const ValueKey<String>('setting-editor:GAME_FIRST_PLAYER'),
            );
            await tester.ensureVisible(editor);
            if (service.latestSettingValue('GAME_FIRST_PLAYER') != first) {
              final swapPlayers = find.byKey(const ValueKey('swap-players'));
              await tester.ensureVisible(swapPlayers);
              await tester.pumpAndSettle();
              expect(swapPlayers.hitTestable(), findsOneWidget);
              await tester.tap(swapPlayers);
              await tester.pumpAndSettle();
              // Bulk confirmation has a timer even when no frame is scheduled.
              await tester.pump(const Duration(milliseconds: 100));
              await tester.pumpAndSettle();
              expect(
                tester
                    .widget<OutlinedButton>(
                      find.byKey(const ValueKey('swap-players')),
                    )
                    .onPressed,
                isNotNull,
              );
              expect(
                ble.seatingPeer.settingMutations.where(
                  (body) => body[1] == 11 && body[2] == first,
                ),
                isNotEmpty,
              );
            }
            expect(service.latestSettingValue('GAME_FIRST_PLAYER'), first);
            await tester.ensureVisible(editor);
            await tester.pump();
            final rightPlayer = (first == 1) != rotated ? 1 : 2;
            final leftPlayer = 3 - rightPlayer;
            for (final assignment in [
              'USB right\nPlayer $rightPlayer',
              'USB left\nPlayer $leftPlayer',
            ]) {
              expect(
                find.descendant(of: editor, matching: find.text(assignment)),
                findsOneWidget,
              );
            }
            final semantics = tester.ensureSemantics();
            try {
              await tester.pump();
              expect(
                find.bySemanticsLabel('USB-right side: Player $rightPlayer'),
                findsOneWidget,
              );
              expect(
                find.bySemanticsLabel('USB-left side: Player $leftPlayer'),
                findsOneWidget,
              );
            } finally {
              semantics.dispose();
            }
            for (final end in [1, 2]) {
              final row = find.byKey(
                ValueKey<String>('quick:P${end}_PIECE_WHEEL'),
              );
              await tester.ensureVisible(row);
              expect(
                find.descendant(
                  of: row,
                  matching: find.text('Player $end color'),
                ),
                findsOneWidget,
              );
              final color = litchWheelPreviewColor(end == 1 ? 40 : 200);
              expect(
                find.descendant(
                  of: row,
                  matching: find.byKey(
                    ValueKey<String>('piece-color-swatch:${color.toARGB32()}'),
                  ),
                ),
                findsOneWidget,
                reason: 'Setup palette end=$end first=$first rotated=$rotated',
              );
            }
            expect(ble.first, first);
            await captureHealthReview(
              tester,
              'seating-setup-rot${ble.rotated}-first${ble.first}',
              boundaryFinder: find.byKey(
                const ValueKey<String>('seating-screen-capture'),
              ),
            );
            await home();
          }
          await tester.pumpWidget(const SizedBox.shrink());
          await _flushSdkCancellation(tester, service.disconnect());
          await tester.pump();
        }
        expect(filters.keys.toSet(), {
          256 + 40,
          256 + 200,
          512 + 40,
          512 + 200,
        });
        expect(filters[256 + 40], isNot(filters[512 + 200]));
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  // Former raw JSON confirmation and notification tests exercised a retired
  // peer route. Current confirmation retry and late replies are covered by
  // the typed Settings slow-partial and retired-reply journeys above.
  testWidgets(
    'typed Settings keeps Home current when older metadata finishes',
    (tester) async {
      final peer = _RecoverySharedPeer();
      final tail = Completer<List<int>>();
      var reads = 0;
      Future<List<int>> readMetadata(
        QualifiedCharacteristic characteristic,
      ) async {
        if (characteristic.characteristicId !=
            BleProtocol.settingsEnumerateCharacteristic) {
          return utf8.encode('[]');
        }
        reads++;
        if (reads == 1) {
          return utf8.encode(
            jsonEncode({
              'it': [
                {'i': 0, 'k': 'P1_PIECE_WHEEL', 't': 'u32', 'v': 42},
                {'i': 1, 'k': 'P2_PIECE_WHEEL', 't': 'u32', 'v': 203},
              ],
              'nx': 2,
              'dn': false,
            }),
          );
        }
        return tail.future;
      }

      void palette(int revision, int wheel) {
        final bytes = peer.settingsPayload(revision: revision);
        final data = ByteData.sublistView(bytes);
        data.setUint16(8, wheel, Endian.little);
        data.setUint16(10, 203, Endian.little);
        bytes[78] = wheel;
        bytes[79] = 203;
        peer.stateFragments(bytes, 14, 9, peer.settingsIdentity(bytes));
      }

      peer.onStateQuery = () {
        peer.home();
        palette(7, 42);
      };
      HomeBoardFeed? feed;
      try {
        await _openRecoveryPeer(tester, peer);
        peer.native!.onRead = readMetadata;
        feed = HomeBoardFeed(bleService: peer.service, boardId: 'board-1');
        await tester.pump();
        expect(feed.current.p1PieceWheel, 42);
        final older = peer.service.readSettings('board-1');
        for (var i = 0; i < 20 && reads < 2; i++) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(reads, 2, reason: 'the old metadata tail is in flight');
        palette(8, 96);
        await tester.pump();
        expect(peer.service.latestSettingValue('P1_PIECE_WHEEL'), 96);
        expect(feed.current.p1PieceWheel, 96);
        tail.complete(
          utf8.encode(
            jsonEncode({
              'it': [
                {'i': 2, 'k': 'WHITE_SQUARES_ON', 't': 'bool', 'v': false},
              ],
              'nx': 0,
              'dn': true,
            }),
          ),
        );
        await tester.pump();
        expect(
          (await older).singleWhere((s) => s.key == 'P1_PIECE_WHEEL').value,
          96,
        );
        expect(peer.service.latestSettingValue('P1_PIECE_WHEEL'), 96);
        expect(feed.current.p1PieceWheel, 96);
        expect(feed.current.p2PieceWheel, 203);
      } finally {
        if (!tail.isCompleted) tail.complete(utf8.encode('[]'));
        await _flushSdkCancellation(tester, () async {
          await feed?.dispose();
          await peer.close();
        }());
      }
    },
  );

  for (final missing in [BleProtocol.otaControlCharacteristic]) {
    test(
      'known current profile rejects partial GATT missing $missing',
      () async {
        final ble = _AppShellReconnectBle(exactGatt: true)
          ..missingOptionalEndpoints.add(missing);
        ble.connections.add(
          StreamController<ConnectionStateUpdate>.broadcast(),
        );
        await expectLater(
          ReactiveBleGattSessionFactory(ble).openWithCapabilities(
            'board-1',
            const LitchDeviceCapabilities(
              contractVersion: 3,
              hardwareRevision: LitchHardwareRevision.v3,
              capabilityMask:
                  LitchDeviceCapabilities.litchcraftBit |
                  LitchDeviceCapabilities.litchcraftBulkBit,
              explicitlyReported: true,
            ),
          ),
          throwsA(isA<LitchGattDiscoveryIncomplete>()),
        );
        expect(ble.exactWriteAttempts, 0);
        await ble.connections.single.close();
      },
    );
  }

  for (final missing in [
    BleProtocol.connectionSessionIngressCharacteristic,
    BleProtocol.connectionSessionEgressCharacteristic,
  ]) {
    test('session contract epoch rejects GATT missing $missing', () async {
      final ble = _AppShellReconnectBle(exactGatt: true)
        ..missingOptionalEndpoints.add(missing);
      ble.connections.add(StreamController<ConnectionStateUpdate>.broadcast());
      await expectLater(
        ReactiveBleGattSessionFactory(ble).openWithCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.litchcraftBit,
            explicitlyReported: true,
          ),
        ),
        throwsA(isA<LitchGattDiscoveryIncomplete>()),
      );
      expect(ble.exactWriteAttempts, 0);
      await ble.connections.single.close();
    });
  }

  test(
    'session-v3 migrated domains do not require legacy data endpoints',
    () async {
      final ble = _AppShellReconnectBle(exactGatt: true)
        ..missingOptionalEndpoints.addAll(<Uuid>{
          BleProtocol.litchcraftCharacteristic,
          BleProtocol.otaDataCharacteristic,
        });
      ble.connections.add(StreamController<ConnectionStateUpdate>.broadcast());
      final session = await ReactiveBleGattSessionFactory(ble)
          .openWithCapabilities(
            'board-1',
            const LitchDeviceCapabilities(
              contractVersion: 3,
              hardwareRevision: LitchHardwareRevision.v3,
              capabilityMask:
                  LitchDeviceCapabilities.litchcraftBit |
                  LitchDeviceCapabilities.litchcraftBulkBit |
                  LitchDeviceCapabilities.firmwareUpdateBit,
              explicitlyReported: true,
            ),
          );
      expect(session, isA<LitchGattSession>());
      await ble.connections.single.close();
    },
  );

  test(
    'session-v3 history and owner pairing use shared session without legacy GATT endpoints',
    () async {
      final ble = _AppShellReconnectBle(exactGatt: true);
      ble.connections.add(StreamController<ConnectionStateUpdate>.broadcast());
      final session = await ReactiveBleGattSessionFactory(ble)
          .openWithCapabilities(
            'board-1',
            const LitchDeviceCapabilities(
              contractVersion: 3,
              hardwareRevision: LitchHardwareRevision.v3,
              capabilityMask:
                  LitchDeviceCapabilities.gameHistoryBit |
                  LitchDeviceCapabilities.ownerPairingBit,
              explicitlyReported: true,
            ),
          );
      expect(session, isA<LitchGattSession>());
      await ble.connections.single.close();
    },
  );

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      'current Litchcraft uses the shared session, without the retired direct route on ${platform.name}',
      (tester) async {
        debugDefaultTargetPlatformOverride = platform;
        addTearDown(() => debugDefaultTargetPlatformOverride = null);
        final exactGatt = platform == TargetPlatform.android;
        final ble = _AppShellReconnectBle(exactGatt: exactGatt)
          ..useSharedPeer = true;
        ble.permitIosInventory = !exactGatt;
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: ble,
          gattSessionFactory: exactGatt
              ? ReactiveBleGattSessionFactory(ble)
              : null,
        );
        final connecting = service.connect('board-1');
        for (var i = 0; i < 1000 && ble.connections.isEmpty; i++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        expect(ble.connections, hasLength(1));
        ble.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(await connecting, isTrue);

        bool resolved = false;
        final availability = service.litchcraftControlAvailable('board-1').then(
          (value) {
            resolved = true;
            return value;
          },
        );
        for (
          var turn = 0;
          turn < 100 && ble.deviceInfoWriteCount == 0;
          ++turn
        ) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        expect(ble.deviceInfoWriteCount, 1);
        expect(resolved, isFalse);
        expect(ble.litchcraftOperations, isEmpty);
        ble.otaNotificationStreams.last.add(utf8.encode('{"i":[3,3,1055]}'));
        for (var turn = 0; turn < 12 && !resolved; ++turn) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(resolved, isTrue);
        expect(await availability, isTrue);
        expect(ble.litchcraftOperations, isEmpty);
        await _flushSdkCancellation(tester, service.dispose());
        await tester.pump(const Duration(seconds: 3));
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      'silent capability probe on modern GATT retires the connection on ${platform.name}',
      (tester) async {
        final exactGatt = platform == TargetPlatform.android;
        debugDefaultTargetPlatformOverride = platform;
        final ble = _AppShellReconnectBle(exactGatt: exactGatt)
          ..permitIosInventory = !exactGatt;
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: ble,
          gattSessionFactory: exactGatt
              ? ReactiveBleGattSessionFactory(ble)
              : null,
        );
        final connecting = service.connect('board-1');
        for (var i = 0; i < 1000 && ble.connections.isEmpty; i++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        expect(ble.connections, hasLength(1));
        ble.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(await connecting, isTrue);

        final capabilities = service.fetchDeviceCapabilities(
          deviceId: 'board-1',
        );
        for (var i = 0; i < 100 && service.isConnected; i++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(ble.deviceInfoWriteCount, 3);
        expect((await capabilities).explicitlyReported, isFalse);
        expect(service.isConnected, isFalse);
        expect(await service.litchcraftControlAvailable('board-1'), isFalse);
        expect(ble.litchcraftOperations, isEmpty);
        await _flushSdkCancellation(tester, service.dispose());
        await tester.pump(const Duration(seconds: 3));
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'modern GATT without capability endpoint retires instead of caching legacy',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final ble = _AppShellReconnectBle(exactGatt: true)
        ..missingOptionalEndpoints.add(BleProtocol.otaControlCharacteristic);
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      );
      final connecting = service.connect('board-1');
      for (var i = 0; i < 1000 && ble.connections.isEmpty; i++) {
        await tester.pump(const Duration(milliseconds: 10));
        await tester.runAsync(() async {});
      }
      expect(ble.connections, hasLength(1));
      ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(await connecting, isTrue);

      final capabilities = service.fetchDeviceCapabilities(deviceId: 'board-1');
      for (var i = 0; i < 100 && service.isConnected; i++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect((await capabilities).explicitlyReported, isFalse);
      expect(service.isConnected, isFalse);
      expect(ble.deviceInfoWriteCount, 0);
      await _flushSdkCancellation(tester, service.dispose());
      await tester.pump(const Duration(seconds: 3));
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'Android incomplete discovery retires cached handles before bootstrap writes',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final ble = _AppShellReconnectBle(exactGatt: true)
        ..incompleteDiscovery = true;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      );
      for (var attempt = 0; attempt < 2; attempt++) {
        final connecting = service.connect('board-1');
        for (var i = 0; i < 1000 && ble.connections.length <= attempt; i++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        expect(ble.connections, hasLength(attempt + 1));
        ble.connections.last.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        await connecting;
        await tester.pump();
        if (attempt == 0) {
          for (
            var turn = 0;
            turn < 40 && ble.otaNotificationStreams.isEmpty;
            ++turn
          ) {
            await tester.pump(const Duration(milliseconds: 10));
          }
          ble.otaNotificationStreams.last.add(utf8.encode('{"i":[3,3,31]}'));
          for (var turn = 0; turn < 40 && service.isConnected; ++turn) {
            await tester.pump(const Duration(milliseconds: 10));
          }
          expect(service.isConnected, isFalse);
          expect(ble.settingsWrites, isEmpty);
          expect(ble.unexpectedQualifiedCalls, 0);
          ble.incompleteDiscovery = false;
        } else {
          expect(service.isConnected, isTrue);
          expect(ble.discoveredSessions, hasLength(2));
        }
      }
      await _flushSdkCancellation(tester, service.dispose());
      await tester.pump(const Duration(seconds: 3));
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets('Android rejects a missing shared endpoint after cache refresh', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    final ble = _AppShellReconnectBle(exactGatt: true);
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: ble,
      gattSessionFactory: ReactiveBleGattSessionFactory(ble),
    );

    Future<void> connectAt(int index) async {
      final connecting = service.connect('board-1');
      for (var i = 0; i < 1000 && ble.connections.length <= index; i++) {
        await tester.pump(const Duration(milliseconds: 10));
        await tester.runAsync(() async {});
      }
      expect(ble.connections, hasLength(index + 1));
      ble.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(await connecting, isTrue);
      await tester.pump();
    }

    await connectAt(0);
    final capabilities = service.fetchDeviceCapabilities(deviceId: 'board-1');
    for (var i = 0; i < 100 && ble.deviceInfoWriteCount == 0; i++) {
      await tester.pump(const Duration(milliseconds: 10));
    }
    expect(ble.deviceInfoWriteCount, 1);
    ble.otaNotificationStreams.last.add(utf8.encode('{"i":[3,3,1055]}'));
    expect((await capabilities).supportsLitchcraft, isTrue);

    ble.connections.first.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    await tester.pump();
    expect(service.isConnected, isFalse);
    ble.missingOptionalEndpoints.add(
      BleProtocol.connectionSessionIngressCharacteristic,
    );

    await connectAt(1);
    for (var i = 0; i < 100 && service.isConnected; i++) {
      await tester.pump(const Duration(milliseconds: 10));
    }
    expect(service.isConnected, isFalse);

    await connectAt(2);
    for (var i = 0; i < 100 && service.isConnected; i++) {
      await tester.pump(const Duration(milliseconds: 10));
    }
    expect(service.isConnected, isFalse);

    await connectAt(3);
    for (var i = 0; i < 100 && service.isConnected; i++) {
      await tester.pump(const Duration(milliseconds: 10));
    }
    expect(service.isConnected, isFalse);
    expect(service.deviceCapabilities('board-1').supportsLitchcraft, isFalse);
    expect(ble.discoveredSessions, hasLength(4));
    expect(ble.unexpectedQualifiedCalls, 0);
    expect(ble.unexpectedRetiredHandleCalls, 0);

    await _flushSdkCancellation(tester, service.dispose());
    await tester.pump(const Duration(seconds: 3));
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets(
    'Android bootstrap waits for discovery before subscribing to shared Home',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final discovery = Completer<void>();
      final ble = _SeatingJourneyBle()
        ..asynchronousSdkSubscriptions = true
        ..discoveryReady = discovery;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      );
      try {
        final connecting = service.connect('board-1');
        for (var i = 0; i < 8 && ble.connections.isEmpty; i++) {
          await tester.pump();
        }
        expect(ble.connections, hasLength(1));
        ble.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(await connecting, isTrue);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: HomeScreen(bleService: service, boardId: 'board-1'),
          ),
        );
        await tester.pump();
        expect(ble.consoleNotificationStreams, isEmpty);
        expect(ble.sharedPeer.sessionActivated, isFalse);
        expect(
          tester
              .widget<HomeChessboard>(find.byType(HomeChessboard))
              .snapshot
              .position,
          isNull,
        );
        discovery.complete();
        for (var i = 0; i < 30 && ble.otaNotificationStreams.isEmpty; i++) {
          await tester.pump(const Duration(milliseconds: 10));
        }
        expect(ble.otaNotificationStreams, isNotEmpty);
        ble.otaNotificationStreams.last.add(utf8.encode('{"i":[3,3,31]}'));
        for (
          var i = 0;
          i < 60 &&
              tester
                      .widget<HomeChessboard>(find.byType(HomeChessboard))
                      .snapshot
                      .position ==
                  null;
          i++
        ) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        final snapshot = tester
            .widget<HomeChessboard>(find.byType(HomeChessboard))
            .snapshot;
        expect(ble.sharedPeer.sessionActivated, isTrue);
        expect(snapshot.position, isNotNull);
        expect(snapshot.playerColorsConfirmed, isTrue);
        expect(ble.unexpectedQualifiedCalls, 0);
        expect(
          ble.settingsWrites,
          isEmpty,
          reason: 'Home uses shared typed State, not retired direct reads.',
        );
      } finally {
        if (!discovery.isCompleted) discovery.complete();
        await tester.pumpWidget(const SizedBox.shrink());
        await _flushSdkCancellation(
          tester,
          _disposeReconnectHarness(service, ble),
        );
        debugDefaultTargetPlatformOverride = null;
      }
    },
  );

  testWidgets(
    'CPU thinking follows fragmented shared Home into the production feed',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      HomeBoardFeed? feed;
      Uint8List thinking(int elapsed, int remaining) {
        final bytes = peer.fixture('home_state_wire_v1.csv', 'representative');
        bytes[1] = HomeStateAction.progress.index;
        bytes[8] |= 0x40;
        bytes[9] &= ~0x01;
        ByteData.sublistView(bytes)
          ..setUint32(71, elapsed, Endian.little)
          ..setUint16(75, remaining, Endian.little);
        return bytes;
      }

      try {
        await _openRecoveryPeer(tester, peer);
        feed = HomeBoardFeed(bleService: peer.service, boardId: 'board-1');
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
        final initial = thinking(0, 0);
        peer.stateFragments(initial, 4, 9, 2);
        await tester.pump();
        expect(
          feed.current.cpuThinking,
          isNull,
          reason:
              'An incomplete Home publication cannot install thinking state.',
        );
        peer.stateFragments(initial, 4, 1, 2, firstFragment: 9);
        await tester.pump();
        expect(peer.service.latestHomeState?.cpuThinking?.side, HomeSide.white);
        expect(feed.current.showsCpuThinking, isTrue);
        expect(feed.current.cpuThinking?.remainingSeconds, isNull);
        peer.stateFragments(thinking(2, 8), 4, 10, 3);
        await tester.pump();
        expect(feed.current.cpuThinking?.remainingSeconds, 8);
        final sequence = peer.service.latestHomeState!.sequence;
        await tester.pump(const Duration(seconds: 3));
        expect(peer.service.latestHomeState?.sequence, sequence);
        expect(feed.current.cpuThinking?.remainingSeconds, isNull);
        expect(feed.current.showsCpuThinking, isTrue);
        peer.home(revision: 4);
        await tester.pump();
        expect(feed.current.liveSequence, greaterThan(sequence));
        expect(feed.current.cpuThinking, isNull);
        peer.stateFragments(thinking(10, 3), 4, 10, 5);
        await tester.pump();
        expect(feed.current.showsCpuThinking, isTrue);
        peer.home(revision: 6);
        await tester.pump();
        expect(feed.current.cpuThinking, isNull);
      } finally {
        await _flushSdkCancellation(tester, () async {
          await feed?.dispose();
          await peer.close();
        }());
      }
    },
  );

  testWidgets('Home clock recovers when the first shared State reply is lost', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    HomeBoardFeed? feed;
    try {
      await _openRecoveryPeer(tester, peer);
      feed = HomeBoardFeed(bleService: peer.service, boardId: 'board-1');
      await tester.pump();
      var queries = 0;
      peer.onStateQuery = () {
        queries++;
        if (queries == 1) return;
        final bytes = peer.fixture('home_state_wire_v1.csv', 'representative');
        bytes[1] = HomeStateAction.idle.index;
        bytes[6] |= 0x40;
        bytes[9] |= 0x0e;
        ByteData.sublistView(bytes)
          ..setUint32(77, 143, Endian.little)
          ..setUint32(81, 127, Endian.little);
        peer.stateFragments(bytes, 4, 10, 2);
      };
      unawaited(feed.refresh());
      await tester.pump();
      expect(queries, 1);
      await tester.pump(const Duration(milliseconds: 125));
      for (var i = 0; i < 8 && queries < 2; i++) {
        await tester.pump();
        await tester.runAsync(() async {});
      }
      expect(queries, 2, reason: 'the lost State reply is retried');
      expect(feed.current.clockStatus?.whiteRemainingSeconds, 143);
      expect(feed.current.clockStatus?.blackRemainingSeconds, 127);
      expect(feed.current.clockStatus?.paused, isTrue);
      expect(feed.current.clockStatus?.running, isTrue);
      expect(
        feed.current.clockStatus?.setupState,
        HomeClockSetupState.gameStarted,
      );
      expect(feed.current.isClockRefreshing, isFalse);
    } finally {
      await _flushSdkCancellation(tester, () async {
        await feed?.dispose();
        await peer.close();
      }());
    }
  });

  testWidgets(
    'shared Home accepts coherent replacements and fences retired fragments',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      final observed = <HomeStateNotification>[];
      final subscription = peer.service.homeStateStream.listen(observed.add);
      try {
        await _openRecoveryPeer(tester, peer);
        await tester.pump();
        final baseline = observed.length;
        peer.onStateQuery = null;
        peer.home(count: 5, revision: 2, action: HomeStateAction.idle);
        peer.home(count: 5, revision: 1, firstFragment: 5);
        await tester.pump();
        expect(
          observed,
          hasLength(baseline),
          reason: 'fragments from different identities cannot form State',
        );
        peer.home(revision: 2, action: HomeStateAction.idle);
        await tester.pump();
        expect(observed, hasLength(baseline + 1));
        expect(observed.last.action, HomeStateAction.idle);
        peer.home(revision: 2, action: HomeStateAction.idle);
        await tester.pump();
        expect(
          observed,
          hasLength(baseline + 1),
          reason: 'identical replay stays quiet',
        );
        final retired = peer.native!;
        await _flushSdkCancellation(tester, peer.service.disconnect());
        peer.service.debugSetConnectedDevice('board-1');
        peer.service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.settingsBit,
            explicitlyReported: true,
          ),
        );
        await _openRecoveryPeer(tester, peer);
        final active = peer.native;
        final before = observed.length;
        peer.home(count: 5, revision: 3);
        peer.native = retired;
        peer.home(count: 5, revision: 3, firstFragment: 5);
        peer.native = active;
        await tester.pump();
        expect(
          observed,
          hasLength(before),
          reason: 'a retired connection cannot finish the new publication',
        );
        peer.home(count: 5, revision: 3, firstFragment: 5);
        await tester.pump();
        expect(observed, hasLength(before + 1));
      } finally {
        await _flushSdkCancellation(tester, subscription.cancel());
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  Uint8List coachingHomeBytes(_RecoverySharedPeer peer, int mask) {
    final bytes = peer.fixture('home_state_wire_v1.csv', 'representative');
    bytes[1] = HomeStateAction.idle.index;
    bytes[6] = 0;
    bytes[7] = 4 | mask;
    bytes[8] = 0;
    bytes[9] = 0;
    bytes[10] = 0;
    bytes.fillRange(62, 71, 255);
    bytes[71] = 0;
    return bytes;
  }

  void coachingSettings(
    _RecoverySharedPeer peer, {
    required int revision,
    required int cpuLevel,
    required int cpuPlayer,
  }) {
    final bytes = peer.settingsPayload(revision: revision);
    ByteData.sublistView(bytes)
      ..setUint16(18, cpuLevel, Endian.little)
      ..setUint16(22, cpuPlayer, Endian.little)
      ..setUint16(26, 1, Endian.little);
    peer.stateFragments(bytes, 14, 9, peer.settingsIdentity(bytes));
  }

  for (final boundary in ['Settings refresh', 'Home refresh']) {
    testWidgets(
      'Coaching role hydration recovers through $boundary without write notifications',
      (tester) async {
        final peer = _RecoverySharedPeer();
        final service = peer.service;
        addTearDown(() => tester.runAsync(peer.close));
        service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.settingsBit,
            explicitlyReported: true,
          ),
        );
        var sequence = 1;
        var rolesAvailable = false;
        var currentLevel = 5, currentPlayer = 2, settingsRevision = 7;
        var metadataReads = 0;
        var publishOnMetadataRead = false;
        void publish() {
          peer.stateFragments(coachingHomeBytes(peer, 3), 4, 10, sequence);
          if (rolesAvailable) {
            coachingSettings(
              peer,
              revision: settingsRevision,
              cpuLevel: currentLevel,
              cpuPlayer: currentPlayer,
            );
          }
        }

        peer.onStateQuery = publish;
        final changed = <String>[];
        StreamSubscription<String>? subscription;
        try {
          await _openRecoveryPeer(tester, peer);
          peer.native!.onRead = (characteristic) async {
            if (characteristic.characteristicId !=
                BleProtocol.settingsEnumerateCharacteristic) {
              return utf8.encode('[]');
            }
            metadataReads++;
            if (publishOnMetadataRead) publish();
            // Metadata alone supplies no role inference. The complete current
            // Settings State above is the later role authority.
            return utf8.encode(
              '[{"i":0,"k":"GAME_FIRST_PLAYER","t":"u32","v":1}]',
            );
          };
          subscription = service.settingsChangedStream.listen(changed.add);
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              home: Scaffold(
                body: HomeScreen(bleService: service, boardId: 'board-1'),
              ),
            ),
          );
          await tester.pump(const Duration(milliseconds: 200));
          await tester.pump();
          await tester.scrollUntilVisible(
            find.byType(HomeBoardActions),
            300,
            scrollable: find
                .descendant(
                  of: find.byType(HomeScreen),
                  matching: find.byType(Scrollable),
                )
                .first,
          );
          // Complete the mounted feed's initial refresh with a later board
          // snapshot while roles are still absent. A new pull must not be
          // silently coalesced into that earlier in-flight refresh.
          ++sequence;
          publish();
          await tester.pump();
          final p1 = find.byKey(const ValueKey('control:coaching:p1'));
          final p2 = find.byKey(const ValueKey('control:coaching:p2'));
          final original = tester
              .widget<HomeBoardActions>(find.byType(HomeBoardActions))
              .controller;
          expect(_coachingButton(tester, p1).onPressed, isNull);
          expect(_coachingButton(tester, p2).onPressed, isNull);
          expect(service.latestSettingValue('VS_CPU_LEVEL'), isNull);
          expect(service.latestSettingValue('VS_CPU_PLAYER'), isNull);
          final beforeEnumerations = metadataReads;
          final beforeQueries = peer.queries;
          rolesAvailable = true;
          ++sequence;
          Future<void> refreshHome() async {
            final refresh = tester
                .widget<RefreshIndicator>(find.byType(RefreshIndicator))
                .onRefresh;
            var complete = false;
            final refreshing = Future.wait([
              refresh(),
              refresh(),
            ]).whenComplete(() => complete = true);
            for (var turn = 0; turn < 20 && !complete; ++turn) {
              await tester.pump();
              await tester.runAsync(() async {});
            }
            expect(complete, isTrue);
            await refreshing;
            await tester.pump();
          }

          if (boundary == 'Settings refresh') {
            publishOnMetadataRead = true;
            final refreshing = service.readSettings('board-1');
            await tester.pump();
            await refreshing;
            await tester.pump();
            expect(metadataReads, beforeEnumerations + 1);
          } else {
            await refreshHome();
            expect(peer.queries, greaterThan(beforeQueries));
            expect(
              metadataReads,
              beforeEnumerations + 1,
              reason:
                  'Missing role readers share one bounded metadata fallback while current typed State arrives.',
            );
            expect(
              peer.native!.writeRecords.where(
                (write) =>
                    write.characteristicId ==
                    BleProtocol.settingsReadWriteCharacteristic,
              ),
              isEmpty,
              reason:
                  'An activated current session never opens the old direct role-read path.',
            );
          }
          expect(
            identical(
              tester
                  .widget<HomeBoardActions>(find.byType(HomeBoardActions))
                  .controller,
              original,
            ),
            isTrue,
          );
          expect(_coachingButton(tester, p1).onPressed, isNotNull);
          expect(_coachingButton(tester, p2).onPressed, isNull);
          expect(service.latestHomeState?.coachingMask, 3);
          expect(
            find.descendant(of: p1, matching: find.text('On')),
            findsOneWidget,
          );
          expect(
            find.descendant(of: p2, matching: find.text('Off')),
            findsOneWidget,
            reason:
                'CPU Coaching is effectively off without changing the saved preference.',
          );
          expect(original.coachingEnabled(LitchControlOwner.p2), isTrue);
          expect(find.byType(SnackBar), findsNothing);
          final notifications = List<String>.of(changed);
          final beforeDuplicates = metadataReads;
          final duplicated = Future.wait([
            service.readSettings('board-1'),
            service.readSettings('board-1'),
          ]);
          await tester.pump();
          await duplicated;
          await tester.pump();
          expect(
            metadataReads - beforeDuplicates,
            lessThanOrEqualTo(1),
            reason: 'Readers share one immutable descriptor adoption.',
          );
          expect(
            changed,
            notifications,
            reason:
                'Unchanged typed values never make a notification/read loop.',
          );
          ++sequence;
          await refreshHome();
          expect(changed, notifications);
          final writesBeforeCachedRead = peer.native!.writes.length;
          expect(
            await service.readSettingValue(
              deviceId: 'board-1',
              key: 'VS_CPU_LEVEL',
            ),
            5,
          );
          expect(
            peer.native!.writes.length,
            writesBeforeCachedRead,
            reason:
                'Known roles come from current Settings State without another transport read.',
          );
          settingsRevision = 8;
          currentPlayer = 1;
          coachingSettings(
            peer,
            revision: settingsRevision,
            cpuLevel: currentLevel,
            cpuPlayer: currentPlayer,
          );
          await tester.pump();
          expect(_coachingButton(tester, p1).onPressed, isNull);
          expect(_coachingButton(tester, p2).onPressed, isNotNull);
          expect(changed, [...notifications, 'VS_CPU_PLAYER']);
          expect(
            find.descendant(of: p1, matching: find.text('Off')),
            findsOneWidget,
          );
          expect(
            find.descendant(of: p2, matching: find.text('On')),
            findsOneWidget,
          );
          expect(original.coachingEnabled(LitchControlOwner.p2), isTrue);
          settingsRevision = 9;
          currentLevel = 0;
          coachingSettings(
            peer,
            revision: settingsRevision,
            cpuLevel: currentLevel,
            cpuPlayer: currentPlayer,
          );
          await tester.pump();
          expect(changed, [...notifications, 'VS_CPU_PLAYER', 'VS_CPU_LEVEL']);
          expect(service.latestHomeState?.coachingMask, 3);
          for (final control in [p1, p2]) {
            expect(_coachingButton(tester, control).onPressed, isNotNull);
            expect(
              find.descendant(of: control, matching: find.text('On')),
              findsOneWidget,
            );
          }
          expect(
            peer.actions,
            0,
            reason:
                'Role/readback hydration never mutates Coaching preferences.',
          );
          expect(tester.takeException(), isNull);
        } finally {
          await _flushSdkCancellation(tester, service.disconnect());
          await tester.pumpWidget(const SizedBox.shrink());
          await _flushSdkCancellation(
            tester,
            subscription?.cancel() ?? Future<void>.value(),
          );
          await tester.pump(const Duration(seconds: 4));
        }
      },
      variant: TargetPlatformVariant({
        TargetPlatform.iOS,
        TargetPlatform.android,
      }),
    );
  }

  for (final LitchControlOwner owner in LitchControlOwner.values) {
    testWidgets(
      'Home ${owner.wireToken} Coaching readback preserves a fragmented report across ACK',
      (tester) async {
        final peer = _RecoverySharedPeer();
        final service = peer.service;
        addTearDown(() => tester.runAsync(peer.close));
        service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.settingsBit,
            explicitlyReported: true,
          ),
        );
        var respond = true;
        var mask = 0;
        var sequence = 1;
        void publish() {
          peer.stateFragments(coachingHomeBytes(peer, mask), 4, 10, sequence);
          coachingSettings(peer, revision: 7, cpuLevel: 0, cpuPlayer: 2);
        }

        peer.onStateQuery = () {
          if (respond) {
            publish();
          }
        };
        peer.onOrdered = peer.finish;
        try {
          await _openRecoveryPeer(tester, peer);
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              home: Scaffold(
                body: HomeScreen(bleService: service, boardId: 'board-1'),
              ),
            ),
          );
          await tester.pump();
          final button = find.byKey(
            ValueKey<String>('control:coaching:${owner.wireToken}'),
          );
          await tester.scrollUntilVisible(
            button,
            300,
            scrollable: find
                .descendant(
                  of: find.byType(HomeScreen),
                  matching: find.byType(Scrollable),
                )
                .first,
          );
          await tester.pump();
          final controller = tester
              .widget<HomeBoardActions>(find.byType(HomeBoardActions))
              .controller;
          expect(controller.coachingEnabled(owner), isFalse);
          List<ConnectionSessionPacket> actions() => [
            for (final write in peer.native!.writeRecords)
              if (write.characteristicId ==
                  BleProtocol.connectionSessionIngressCharacteristic)
                if (ConnectionSessionCodec.decode(
                      Uint8List.fromList(write.value),
                    )
                    case final packet?)
                  if (packet.kind == ConnectionSessionKind.ordered &&
                      packet.code ==
                          LitchControlProtocol.connectionActionOpcode)
                    packet,
          ];
          peer.onOrdered = null;
          respond = false;
          final beforeQueries = peer.queries;
          await tester.tap(button);
          await tester.pump();
          final command = actions().single;
          expect(
            command.body,
            LitchControlProtocol.encodeConnectionAction(
              action: LitchControlAction.coaching,
              owner: owner,
              enabled: true,
            ),
          );
          mask = owner == LitchControlOwner.p1 ? 1 : 2;
          sequence = 2;
          final changed = coachingHomeBytes(peer, mask);
          // A current typed Home snapshot can straddle the separate retained
          // Ordered terminal receipt. Neither the prefix nor ACK is authority.
          peer.stateFragments(changed, 4, 5, sequence);
          peer.finish();
          await tester.pump();
          expect(
            controller.coachingEnabled(owner),
            isFalse,
            reason: 'ACK alone is not state authority.',
          );
          expect(_coachingButton(tester, button).onPressed, isNotNull);
          expect(
            find.descendant(of: button, matching: find.text('Off')),
            findsOneWidget,
          );
          peer.stateFragments(changed, 4, 5, sequence, firstFragment: 5);
          await tester.pump();
          expect(
            controller.coachingEnabled(owner),
            isTrue,
            reason: 'The prefix survives ACK and completes coherently.',
          );
          publish();
          await tester.pump(const Duration(milliseconds: 40));
          expect(peer.queries, greaterThan(beforeQueries));
          expect(
            find.text(
              'Coaching state is unavailable. Follow the board lights.',
            ),
            findsNothing,
          );
          expect(
            find.descendant(of: button, matching: find.text('On')),
            findsOneWidget,
          );
          expect(_coachingButton(tester, button).onPressed, isNotNull);
          expect(
            peer.native!.subscriptions.where(
              (uuid) =>
                  uuid == BleProtocol.connectionSessionEgressCharacteristic,
            ),
            hasLength(1),
          );
          await tester.tap(button);
          await tester.pump();
          expect(
            actions().last.body,
            LitchControlProtocol.encodeConnectionAction(
              action: LitchControlAction.coaching,
              owner: owner,
              enabled: false,
            ),
          );
          peer.finish();
          await tester.pump();
          expect(controller.coachingEnabled(owner), isTrue);
          expect(_coachingButton(tester, button).onPressed, isNotNull);
          await tester.pump(const Duration(milliseconds: 3100));
          await tester.pump();
          expect(
            find.text(
              'Coaching state is unavailable. Follow the board lights.',
            ),
            findsOneWidget,
          );
          expect(controller.coachingEnabled(owner), isNull);
          expect(_coachingButton(tester, button).onPressed, isNull);
          expect(
            tester.getSemantics(button).flagsCollection.isToggled,
            ui.Tristate.none,
          );
          expect(
            find.descendant(of: button, matching: find.text('Unknown')),
            findsOneWidget,
          );
          mask = 0;
          sequence = 3;
          respond = true;
          publish();
          await tester.pump();
          expect(controller.coachingEnabled(owner), isFalse);
          expect(
            find.descendant(of: button, matching: find.text('Off')),
            findsOneWidget,
          );
          expect(
            find.text(
              'Coaching state is unavailable. Follow the board lights.',
            ),
            findsNothing,
          );
          expect(
            actions(),
            hasLength(2),
            reason:
                'Later State recovers without another tap or command replay.',
          );
          expect(tester.takeException(), isNull);
        } finally {
          peer.onStateQuery = null;
          await tester.pumpWidget(const SizedBox.shrink());
        }
      },
      variant: TargetPlatformVariant({
        TargetPlatform.iOS,
        TargetPlatform.android,
      }),
    );
  }

  for (final unfinished in <String>[
    'incomplete old line',
    List<String>.filled(1100, 'x').join(),
    'old carriage return\r',
  ]) {
    testWidgets(
      'Console recovery clears a failed partial line of length ${unfinished.length}',
      (tester) async {
        final peer = _RecoverySharedPeer();
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        try {
          await _openRecoveryPeer(tester, peer);
          await peer.startConsole(tester);
          final console = peer
              .native!
              ._notifications[BleProtocol.consoleStreamCharacteristic]!;
          console.add(utf8.encode('retained console history\n$unfinished'));
          console.addError(StateError('notification interrupted'));
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 500));
          for (
            var i = 0;
            i < 8 &&
                peer.native!.subscriptions
                        .where(
                          (id) => id == BleProtocol.consoleStreamCharacteristic,
                        )
                        .length <
                    2;
            i++
          ) {
            await tester.pump();
            await tester.runAsync(() async {});
          }
          expect(
            peer.native!.subscriptions
                .where((id) => id == BleProtocol.consoleStreamCharacteristic)
                .length,
            greaterThanOrEqualTo(2),
          );
          if (unfinished.endsWith('\r')) console.add([10]);
          console.add(utf8.encode('fresh console line\n'));
          await tester.pump();
          final lines = peer.service.consoleLinesSnapshot;
          expect(
            lines.any((line) => line.contains('retained console history')),
            isTrue,
          );
          expect(
            lines.any((line) => line.endsWith('fresh console line')),
            isTrue,
          );
          expect(
            lines.any((line) => line.contains('incomplete old linefresh')),
            isFalse,
          );
          expect(
            lines.any((line) => line.contains('old carriage return')),
            isFalse,
          );
        } finally {
          await _flushSdkCancellation(tester, peer.close());
        }
      },
    );
  }

  testWidgets(
    'overlapping shared Home snapshot requests accept one coherent report',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        peer.onStateQuery = null;
        final before = peer.queries;
        bool firstDone = false, secondDone = false;
        final first = peer.service
            .requestCurrentHomeStateSnapshot('board-1')
            .whenComplete(() => firstDone = true);
        final second = peer.service
            .requestCurrentHomeStateSnapshot('board-1')
            .whenComplete(() => secondDone = true);
        for (var i = 0; i < 8 && peer.queries < before + 2; i++) {
          await tester.pump();
        }
        expect(peer.queries, before + 2);
        peer.home(revision: 2, action: HomeStateAction.idle);
        for (var i = 0; i < 8 && (!firstDone || !secondDone); i++) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(firstDone && secondDone, isTrue);
        expect(await first, isTrue);
        expect(await second, isTrue);
        expect(peer.service.latestHomeState?.action, HomeStateAction.idle);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  for (final nextDevice in <String>['board-1', 'board-2']) {
    testWidgets(
      'pending shared Home request retires before replacement with $nextDevice',
      (tester) async {
        final peer = _RecoverySharedPeer();
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        try {
          await _openRecoveryPeer(tester, peer);
          peer.onStateQuery = null;
          final old = peer.native!;
          final before = peer.queries;
          final pending = peer.service.requestCurrentHomeStateSnapshot(
            'board-1',
          );
          for (var i = 0; i < 8 && peer.queries == before; i++) {
            await tester.pump();
          }
          expect(peer.queries, before + 1);
          await _flushSdkCancellation(tester, peer.service.disconnect());
          expect(await pending, isFalse);
          peer.service.debugSetConnectedDevice(nextDevice);
          peer.service.debugSetDeviceCapabilities(
            nextDevice,
            const LitchDeviceCapabilities(
              contractVersion: 3,
              hardwareRevision: LitchHardwareRevision.v3,
              capabilityMask: LitchDeviceCapabilities.settingsBit,
              explicitlyReported: true,
            ),
          );
          await _openRecoveryPeer(tester, peer, deviceId: nextDevice);
          final active = peer.native;
          peer.native = old;
          peer.home(revision: 2, action: HomeStateAction.idle);
          peer.native = active;
          await tester.pump();
          expect(peer.service.latestHomeState, isNull);
          peer.home(revision: 3, action: HomeStateAction.idle);
          await tester.pump();
          expect(peer.service.latestHomeState?.action, HomeStateAction.idle);
        } finally {
          await _flushSdkCancellation(tester, peer.close());
        }
      },
    );
  }

  testWidgets(
    'startup timing: iOS discovers compatible names immediately and rejects unrelated devices',
    (WidgetTester tester) async {
      final _ScanBle ble = _ScanBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetooth: PermissionStatus.granted,
        }),
      );
      final List<List<String>> discoveries = <List<String>>[];
      final subscription = service.boardsStream.listen(
        (boards) => discoveries.add(boards.map((board) => board.id).toList()),
      );
      addTearDown(() async {
        final Future<void> cleanup = () async {
          await subscription.cancel();
          await service.dispose();
          await ble.close();
        }();
        bool disposed = false;
        unawaited(cleanup.then((_) => disposed = true));
        for (int pass = 0; pass < 8 && !disposed; ++pass) {
          await tester.runAsync(() async {});
          await tester.pump();
        }
        expect(disposed, isTrue);
        await cleanup;
      });
      unawaited(service.startScan());
      await tester.pump();
      ble.statusController.add(BleStatus.ready);
      await tester.pump();
      expect(ble.scanFilters, <List<Uuid>>[<Uuid>[]]);
      ble.scanController.add(
        DiscoveredDevice(
          id: 'headphones',
          name: 'Nearby headphones',
          serviceUuids: const <Uuid>[],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -30,
          connectable: Connectable.available,
        ),
      );
      await tester.pump();
      expect(discoveries.last, isEmpty);
      service.rememberCompatibleDevice('renamed-v3');
      ble.scanController.add(
        DiscoveredDevice(
          id: 'renamed-v3',
          name: 'Studio Board',
          serviceUuids: const <Uuid>[],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -45,
          connectable: Connectable.available,
        ),
      );
      await tester.pump();
      expect(
        discoveries.last,
        <String>['renamed-v3'],
        reason:
            'A mutable board name must not make the established device identity undiscoverable.',
      );
      ble.scanController.add(
        DiscoveredDevice(
          id: 'legacy-v2',
          name: 'Litch',
          serviceUuids: const <Uuid>[],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -50,
          connectable: Connectable.available,
        ),
      );
      await tester.pump();
      expect(discoveries.last, <String>['legacy-v2', 'renamed-v3']);
      expect(ble.scanForDevicesCalls, 1);
      await tester.pump(const Duration(seconds: 2));
      expect(ble.scanForDevicesCalls, 1);
      expect(discoveries.last, <String>['legacy-v2', 'renamed-v3']);
      debugPrint('STARTUP virtual iOS legacy-name discovery delay=0 ms');
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  testWidgets(
    'Identify scan restart retains the discovered board roster',
    (WidgetTester tester) async {
      final ble = _ScanBle()..separateScanSessions = true;
      final peer = _RecoverySharedPeer(connected: false)
        ..infoPayload = jsonEncode({
          'i': [3, 4, LitchDeviceCapabilities.identifyBit],
        });
      peer.onOrdered = peer.finish;
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        gattSessionFactory: peer.sessions,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetooth: PermissionStatus.granted,
        }),
      );
      final discoveries = <List<String>>[];
      final subscription = service.boardsStream.listen(
        (boards) => discoveries.add(boards.map((board) => board.id).toList()),
      );
      addTearDown(() async {
        await subscription.cancel();
        await service.dispose();
        await peer.close();
        await ble.close();
      });

      unawaited(service.startScan());
      await tester.pump();
      ble.statusController.add(BleStatus.ready);
      await tester.pump();
      void discover(String id) => ble.scanController.add(
        DiscoveredDevice(
          id: id,
          name: 'Foxie_Litch',
          serviceUuids: <Uuid>[BleProtocol.baseService],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -50,
          connectable: Connectable.available,
        ),
      );
      discover('board-1');
      await tester.pump();
      expect(discoveries.last, <String>['board-1']);
      final int emptyRostersBefore = discoveries.where((e) => e.isEmpty).length;

      final identify = service.sendIdentify(
        'board-1',
        1,
        durationMs: 3000,
        untilConnected: false,
      );
      for (int pass = 0; pass < 8 && ble.connections.isEmpty; ++pass) {
        await tester.pump(const Duration(milliseconds: 20));
        await tester.runAsync(() async {});
      }
      expect(ble.connections, hasLength(1));
      ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      bool identifyCompleted = false;
      bool? identifyResult;
      unawaited(
        identify.then((result) {
          identifyResult = result;
          identifyCompleted = true;
        }),
      );
      for (int pass = 0; pass < 8 && !identifyCompleted; ++pass) {
        await tester.pump(const Duration(milliseconds: 100));
        await tester.runAsync(() async {});
      }
      expect(identifyCompleted, isTrue);
      expect(identifyResult, isTrue);
      expect(ble.scanForDevicesCalls, 2);
      expect(
        discoveries.where((e) => e.isEmpty).length,
        emptyRostersBefore,
        reason: 'internal Identify restart must not clear visible discovery',
      );
      expect(discoveries.last, <String>['board-1']);
      final ordered = peer.sessions.sessions.single.writeRecords
          .where(
            (record) =>
                record.characteristicId ==
                BleProtocol.connectionSessionIngressCharacteristic,
          )
          .map(
            (record) => ConnectionSessionCodec.decode(
              Uint8List.fromList(record.value),
            )!,
          )
          .where((packet) => packet.kind == ConnectionSessionKind.ordered);
      expect(ordered.map((packet) => [packet.code, ...packet.body]), [
        [28, 1, 0, 0, 0, 0, 0, 0, 0], // Probe
        [28, 1, 1, 1, 0, 0, 255, 0xb8, 0x0b], // Start number 1 for 3000 ms
      ]);
      discover('board-2');
      await tester.pump();
      expect(discoveries.last, unorderedEquals(['board-1', 'board-2']));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  test(
    'current and legacy board names qualify during unfiltered discovery',
    () {
      final DiscoveredDevice currentName = DiscoveredDevice(
        id: 'current-1',
        name: 'Foxie Litch',
        serviceUuids: const <Uuid>[],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -48,
        connectable: Connectable.available,
      );
      final DiscoveredDevice legacyByName = DiscoveredDevice(
        id: 'legacy-1',
        name: ' Foxie_Litch Beta ',
        serviceUuids: const <Uuid>[],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -48,
        connectable: Connectable.available,
      );
      final DiscoveredDevice matchedByService = DiscoveredDevice(
        id: 'service-1',
        name: 'Board',
        serviceUuids: <Uuid>[BleProtocol.baseService],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -52,
        connectable: Connectable.available,
      );
      final DiscoveredDevice originalName = DiscoveredDevice(
        id: 'legacy-2',
        name: 'Litch',
        serviceUuids: const <Uuid>[],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -50,
        connectable: Connectable.available,
      );
      final DiscoveredDevice unrelated = DiscoveredDevice(
        id: 'other-1',
        name: 'AirPods',
        serviceUuids: const <Uuid>[],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -40,
        connectable: Connectable.available,
      );

      expect(LitchBleService.isLitchCandidateDevice(currentName), isTrue);
      expect(LitchBleService.isLitchCandidateDevice(legacyByName), isTrue);
      expect(LitchBleService.isLitchCandidateDevice(originalName), isTrue);
      expect(LitchBleService.isLitchCandidateDevice(matchedByService), isTrue);
      expect(LitchBleService.isLitchCandidateDevice(unrelated), isFalse);
    },
  );

  test('Android retries advertising-aware connection after direct mode', () {
    expect(
      LitchBleService.connectionModesForPlatform(TargetPlatform.android),
      <bool>[false, true, true],
    );
    expect(
      LitchBleService.connectionRetryDelayForAttempt(TargetPlatform.android, 1),
      const Duration(milliseconds: 250),
    );
    expect(
      LitchBleService.connectionRetryDelayForAttempt(TargetPlatform.android, 2),
      const Duration(milliseconds: 1500),
    );
    expect(
      LitchBleService.connectionModesForPlatform(TargetPlatform.iOS),
      <bool>[false],
    );
  });

  testWidgets(
    'Android gate keeps the fifth native scan alive for a late waking board',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues(<String, Object>{
        'last_connected_board_id': 'board-1',
      });
      final ble = _ScanBle()
        ..separateScanSessions = true
        ..androidScanClock = tester.binding.clock.now;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: tester.binding.clock.now,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      Future<void> flushUntil(bool Function() complete) async {
        for (int pass = 0; pass < 64 && !complete(); ++pass) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(complete(), isTrue);
      }

      bool cleaned = false;
      Future<void> cleanup() async {
        if (cleaned) return;
        cleaned = true;
        await tester.pumpWidget(const SizedBox.shrink());
        bool disposed = false;
        final disposal = service.dispose().then((_) => disposed = true);
        await flushUntil(() => disposed);
        await disposal;
        await tester.runAsync(ble.close);
        debugDefaultTargetPlatformOverride = null;
      }

      addTearDown(cleanup);
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await flushUntil(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await flushUntil(() => ble.scanSessions.length == 1);
      for (int tick = 0; tick < 32 && ble.scanSessions.length < 5; ++tick) {
        await tester.pump(const Duration(milliseconds: 500));
        await tester.runAsync(() async {});
      }
      await flushUntil(() => ble.scanSessions.length == 5);
      expect(ble.scanSessions, hasLength(5));
      final fifthScan = ble.scanSessions.last;
      bool refreshed = false;
      unawaited(service.rescan().then((_) => refreshed = true));
      await flushUntil(() => refreshed);
      for (int tick = 0; tick < 24; ++tick) {
        await tester.pump(const Duration(milliseconds: 500));
        await tester.runAsync(() async {});
      }
      expect(
        fifthScan.hasListener,
        isTrue,
        reason:
            'Native admission must be checked before cancelling a live scan.',
      );
      expect(ble.scanForDevicesCalls, 5);
      expect(ble.scanCancellationCount, 4);
      expect(ble.nativeScanThrottles, 0);
      fifthScan.add(
        DiscoveredDevice(
          id: 'board-1',
          name: 'Foxie Litch',
          serviceUuids: <Uuid>[BleProtocol.baseService],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -40,
          connectable: Connectable.available,
        ),
      );
      await tester.pump(const Duration(seconds: 2));
      await flushUntil(() => ble.connections.length == 1);
      expect(service.discoveredBoard('board-1'), isNotNull);
      expect(ble.scanForDevicesCalls, 5);
      expect(tester.takeException(), isNull);
      await cleanup();
    },
  );

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      '$platform unsuccessful discovery renews admitted scans and finds a late board',
      (tester) async {
        debugDefaultTargetPlatformOverride = platform;
        SharedPreferences.setMockInitialValues(<String, Object>{
          'last_connected_board_id': 'board-1',
        });
        final board = platform == TargetPlatform.android
            ? _SeatingJourneyBle()
            : null;
        final ble = (board?.discovery ?? _ScanBle())
          ..separateScanSessions = true;
        final connections = board?.connections ?? ble.connections;
        if (platform == TargetPlatform.android) {
          ble.androidScanClock = tester.binding.clock.now;
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
        }
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: board ?? ble,
          gattSessionFactory: board == null
              ? null
              : ReactiveBleGattSessionFactory(board),
          scanClock: tester.binding.clock.now,
          permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
            Permission.bluetooth: PermissionStatus.granted,
            Permission.bluetoothScan: PermissionStatus.granted,
            Permission.bluetoothConnect: PermissionStatus.granted,
            Permission.locationWhenInUse: PermissionStatus.granted,
          }),
        );
        final cancellation = Completer<void>();
        final choiceCancellation = Completer<void>();
        Future<void> closeBoard() async {
          if (board != null) {
            await _disposeReconnectHarness(service, board);
            await board.sharedPeer.packets.close();
            await board.sharedPeer.sessionPackets.close();
          } else {
            await service.dispose();
          }
          await ble.close();
        }

        bool cleaned = false;
        Future<void> flushUntil(bool Function() complete) async {
          for (int pass = 0; pass < 64 && !complete(); ++pass) {
            await tester.pump();
            await tester.runAsync(() async {});
          }
          expect(complete(), isTrue);
        }

        Future<void> cleanup() async {
          if (cleaned) return;
          cleaned = true;
          if (!cancellation.isCompleted) cancellation.complete();
          if (!choiceCancellation.isCompleted) choiceCancellation.complete();
          await tester.pumpWidget(const SizedBox.shrink());
          final close = closeBoard();
          bool closed = false;
          unawaited(close.then((_) => closed = true));
          await flushUntil(() => closed);
          await close;
          debugDefaultTargetPlatformOverride = null;
        }

        addTearDown(cleanup);
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        await flushUntil(() => ble.statusController.hasListener);
        ble.statusController.add(BleStatus.ready);
        await flushUntil(() => ble.scanSessions.length == 1);
        final oldScan = ble.scanSessions.single;
        // Native incoming traffic does not prove delivery of a Litch candidate.
        // This models result-starved board discovery, not an RF-silent scanner.
        oldScan.add(
          DiscoveredDevice(
            id: 'unrelated',
            name: 'Other Bluetooth device',
            serviceUuids: const <Uuid>[],
            serviceData: const <Uuid, Uint8List>{},
            manufacturerData: Uint8List(0),
            rssi: -45,
            connectable: Connectable.available,
          ),
        );
        await tester.pump(const Duration(milliseconds: 1500));
        expect(ble.scanSessions, hasLength(1));
        expect(service.discoveredBoard('unrelated'), isNull);
        ble.heldScanCancellation = cancellation;
        await tester.pump(const Duration(milliseconds: 500));
        await flushUntil(() => ble.scanCancellationCount == 1);
        expect(
          ble.scanSessions,
          hasLength(1),
          reason: 'Renewal waits for native cancellation.',
        );
        oldScan.add(
          DiscoveredDevice(
            id: 'retired-board',
            name: 'Litch retired',
            serviceUuids: <Uuid>[BleProtocol.baseService],
            serviceData: const <Uuid, Uint8List>{},
            manufacturerData: Uint8List(0),
            rssi: -30,
            connectable: Connectable.available,
          ),
        );
        await tester.pump(const Duration(seconds: 2));
        expect(service.discoveredBoard('retired-board'), isNull);
        expect(ble.scanCancellationCount, 1);
        cancellation.complete();
        await flushUntil(() => ble.scanSessions.length == 2);
        ble.heldScanCancellation = null;
        for (int window = 0; window < 31; ++window) {
          if (platform == TargetPlatform.android) {
            for (int tick = 0; tick < 4; ++tick) {
              await tester.pump(const Duration(milliseconds: 500));
              await tester.runAsync(() async {});
            }
            expect(ble.nativeScanThrottles, 0);
            await flushUntil(() => ble.scanController.hasListener);
            expect(ble.scanController.hasListener, isTrue);
          } else {
            await tester.pump(const Duration(milliseconds: 1500));
            expect(ble.scanSessions, hasLength(2 + window));
            await tester.pump(const Duration(milliseconds: 500));
            await flushUntil(() => ble.scanSessions.length == 3 + window);
          }
        }
        // The board becomes discoverable after more than a minute asleep.
        final target = DiscoveredDevice(
          id: 'board-1',
          name: 'Foxie Litch',
          serviceUuids: <Uuid>[BleProtocol.baseService],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -40,
          connectable: Connectable.available,
        );
        ble.scanController.add(target);
        await tester.pump(const Duration(seconds: 2));
        await flushUntil(() => connections.length == 1);
        expect(service.discoveredBoard('board-1'), isNotNull);
        if (platform == TargetPlatform.android) {
          expect(ble.scanForDevicesCalls, inInclusiveRange(10, 15));
          expect(ble.nativeScanThrottles, 0);
        } else {
          expect(ble.scanSessions, hasLength(33));
        }
        if (board != null) {
          const connected = ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          );
          Future<void> awaitPlay() async {
            for (var turn = 0; turn < 100 && !service.productReady; ++turn) {
              await tester.pump(const Duration(milliseconds: 20));
              await tester.runAsync(() async {});
            }
            expect(service.productReady, isTrue);
            board.publish();
            await flushUntil(
              () => find.byType(HomeScreen).evaluate().isNotEmpty,
            );
            await flushUntil(
              () => find.text('P1 to move').evaluate().isNotEmpty,
            );
          }

          connections.single.add(connected);
          await awaitPlay();
          final scanCount = ble.scanSessions.length;
          connections.single.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
          await flushUntil(() => ble.scanSessions.length > scanCount);
          expect(service.isConnected, isFalse);
          expect(service.latestHomeState, isNull);
          final stale = ble.scanController;
          DiscoveredDevice candidate(String id) => DiscoveredDevice(
            id: id,
            name: 'Litch $id',
            serviceUuids: [BleProtocol.baseService],
            serviceData: {},
            manufacturerData: Uint8List(0),
            rssi: -45,
            connectable: Connectable.available,
          );
          stale.add(candidate('stale-board'));
          await tester.pump(const Duration(seconds: 2));
          expect(service.discoveredBoard('stale-board'), isNotNull);
          expect(ble.scanSessions.length, scanCount + 1);
          final setup = find.byKey(
            const ValueKey('disconnected-destination-setup'),
          );
          await tester.tap(setup);
          await tester.pump(const Duration(milliseconds: 600));
          ble.heldScanCancellation = choiceCancellation;
          final priorCancellations = ble.scanCancellationCount;
          final priorScans = ble.scanSessions.length;
          await tester.tap(
            find.byKey(const ValueKey('disconnected-board-status-action')),
          );
          await flushUntil(
            () => ble.scanCancellationCount > priorCancellations,
          );
          stale.add(candidate('retired-board'));
          await tester.pump();
          expect(service.discoveredBoard('retired-board'), isNull);
          expect(ble.scanSessions.length, priorScans);
          expect(service.discoveredBoard('stale-board'), isNotNull);
          expect(connections, hasLength(1));
          await tester.tap(setup);
          await tester.pump(const Duration(milliseconds: 600));
          choiceCancellation.complete();
          await flushUntil(() => ble.scanSessions.length > priorScans);
          ble.heldScanCancellation = null;
          expect(ble.scanController.hasListener, isTrue);
          expect(service.discoveredBoard('stale-board'), isNull);
          expect(
            find.byKey(const ValueKey('discovered-board-retired-board')),
            findsNothing,
          );
          expect(
            tester
                .widget<NavigationBar>(find.byType(NavigationBar))
                .selectedIndex,
            2,
          );
          ble.scanController.add(target);
          await tester.pump(const Duration(seconds: 2));
          expect(
            connections,
            hasLength(1),
            reason: 'Choose board requires an explicit choice.',
          );
          await tester.tap(
            find.byKey(const ValueKey('disconnected-destination-play')),
          );
          await tester.pump(const Duration(milliseconds: 600));
          final connect = find.descendant(
            of: find.byKey(const ValueKey('discovered-board-board-1')),
            matching: find.widgetWithText(FilledButton, 'Connect'),
          );
          expect(connect.hitTestable(), findsOneWidget);
          expect(ble.scanSessions.length, priorScans + 1);
          await tester.tap(connect);
          await flushUntil(() => connections.length == 2);
          connections.last.add(connected);
          await awaitPlay();
          expect(board.unexpectedRetiredHandleCalls, 0);
        }
        expect(tester.takeException(), isNull);
        await cleanup();
      },
    );
  }

  test(
    'Android stale-target fallback prescan shares native scan admission',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final origin = DateTime(2026);
      var now = origin;
      final ble = _ScanBle()
        ..separateScanSessions = true
        ..androidScanClock = () => now;
      final service = LitchBleService(
        ble: ble,
        pendingOtaStore: MemoryOtaTargetStore(),
        scanClock: () => now,
        permissionGateway: _PermissionGateway({
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      addTearDown(() async {
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;
      for (var window = 1; window < 5; window++) {
        now = origin.add(Duration(seconds: window * 2));
        await service.rescan();
      }
      final oldScan = ble.scanController;
      ble.failDirectConnect = true;
      now = origin.add(
        const Duration(seconds: 29),
      ); // Target's SDK advertisement cache has expired after25s.
      expect(await service.connect('stale-board'), isFalse);
      expect(
        ble.advertisingConnectionAttempts,
        0,
        reason:
            'Do not enter SDK fallback when its possible prescan cannot start.',
      );
      expect(ble.nativeScanThrottles, 0);
      now = origin.add(const Duration(seconds: 30));
      expect(await service.connect('stale-board'), isFalse);
      expect(
        ble.advertisingConnectionAttempts,
        1,
        reason:
            'The first expired slot admits one existing fallback; the next stays fenced.',
      );
      expect(ble.scanForDevicesCalls, 6);
      now = origin.add(const Duration(seconds: 32));
      await service.startScan();
      expect(ble.scanForDevicesCalls, 7);
      expect(ble.scanController.hasListener, isTrue);
      expect(oldScan.hasListener, isFalse);
      expect(ble.nativeScanThrottles, 0);
    },
  );

  test(
    'Android scan budget survives pull stop background and native completion',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final origin = DateTime(2026);
      var now = origin;
      final ble = _ScanBle()
        ..separateScanSessions = true
        ..androidScanClock = () => now;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: () => now,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      addTearDown(() async {
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;
      for (int window = 1; window < 5; ++window) {
        now = origin.add(Duration(seconds: 2 * window));
        await service.startScan();
      }
      expect(ble.scanForDevicesCalls, 5);
      final retiredScan = ble.scanSessions.last;
      await service.stopScan();
      await service.startScan();
      await service.rescan();
      await service.setAppForegroundActive(false);
      await service.setAppForegroundActive(true);
      await service.rescan();
      now = origin.add(const Duration(milliseconds: 29999));
      await service.startScan();
      expect(ble.scanForDevicesCalls, 5);
      expect(ble.nativeScanThrottles, 0);
      now = origin.add(const Duration(seconds: 30));
      await service.startScan();
      expect(ble.scanForDevicesCalls, 6);
      final board = DiscoveredDevice(
        id: 'board-1',
        name: 'Foxie Litch',
        serviceUuids: <Uuid>[BleProtocol.baseService],
        serviceData: const <Uuid, Uint8List>{},
        manufacturerData: Uint8List(0),
        rssi: -40,
        connectable: Connectable.available,
      );
      retiredScan.add(board);
      expect(service.discoveredBoard('board-1'), isNull);
      ble.scanController.add(board);
      await service.rescan();
      expect(ble.scanForDevicesCalls, 6);
      expect(
        service.discoveredBoard('board-1'),
        isNotNull,
        reason: 'A blocked pull retains both the live listener and its roster.',
      );
      await ble.scanController.close();
      await service.startScan();
      expect(ble.scanForDevicesCalls, 6);
      now = origin.add(const Duration(seconds: 32));
      await service.startScan();
      expect(ble.scanForDevicesCalls, 7);
      expect(ble.nativeScanThrottles, 0);
      expect(ble.nativeScanStarts, hasLength(5));
    },
  );

  test(
    'discovery rescan resets the two-second window and honors native cooldown',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      var now = DateTime(2026);
      final ble = _ScanBle()..separateScanSessions = true;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: () => now,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      addTearDown(() async {
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;
      now = now.add(const Duration(seconds: 1));
      await service.rescan();
      expect(ble.scanSessions, hasLength(2));
      now = now.add(const Duration(milliseconds: 1500));
      await service.startScan();
      expect(ble.scanSessions, hasLength(2));
      now = now.add(const Duration(milliseconds: 500));
      await service.startScan();
      expect(ble.scanSessions, hasLength(3));
      ble.scanController.add(
        DiscoveredDevice(
          id: 'board',
          name: 'Foxie Litch',
          serviceUuids: <Uuid>[BleProtocol.baseService],
          serviceData: const <Uuid, Uint8List>{},
          manufacturerData: Uint8List(0),
          rssi: -40,
          connectable: Connectable.available,
        ),
      );
      now = now.add(const Duration(seconds: 60));
      await service.startScan();
      expect(
        ble.scanSessions,
        hasLength(3),
        reason: 'A discovered board ends automatic renewal.',
      );
      await service.rescan();
      expect(ble.scanSessions, hasLength(4));
      expect(service.discoveredBoard('board'), isNull);
      ble.scanController.addError(Exception('scan throttle'));
      for (int tick = 0; tick < 15; ++tick) {
        now = now.add(const Duration(milliseconds: 500));
        await service.startScan();
        await service.rescan();
        expect(
          ble.scanSessions,
          hasLength(4),
          reason: 'Pull and passive ticks honor the 8s cooldown.',
        );
      }
      now = now.add(const Duration(milliseconds: 500));
      await service.rescan();
      expect(ble.scanSessions, hasLength(5));
      ble.scanController.addError(Exception('native scan failed'));
      for (int tick = 0; tick < 3; ++tick) {
        now = now.add(const Duration(milliseconds: 500));
        await service.rescan();
        expect(ble.scanSessions, hasLength(5));
      }
      now = now.add(const Duration(milliseconds: 500));
      await service.startScan();
      expect(ble.scanSessions, hasLength(6));
    },
  );

  test(
    'pinned scanner throttle wrapper delays automatic and pulled rescan eight seconds',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      var now = DateTime(2026);
      final ble = _ScanBle()..separateScanSessions = true;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: () => now,
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      addTearDown(() async {
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;

      const message =
          'Undocumented scan throttle (code 2147483646), suggested retry date is Sun Oct 25 00:00:08 UTC 2026';
      const failure = GenericFailure<ScanFailure>(
        code: ScanFailure.unknown,
        message: message,
      );
      // Pinned DeviceScannerImpl dematerializes this platform result and rethrows
      // its Exception unchanged. Exercise that production wrapping, not a shorter
      // synthetic throttle string; Android preserves the RxAndroidBle message.
      const result =
          Result<DiscoveredDevice, GenericFailure<ScanFailure>>.failure(
            failure,
          );
      Object? scannerError;
      try {
        result.dematerialize();
      } catch (error) {
        scannerError = error;
      }
      expect(scannerError, isException);
      expect(scannerError.toString(), 'Exception: $failure');
      ble.scanController.addError(scannerError!);
      for (int tick = 0; tick < 15; ++tick) {
        now = now.add(const Duration(milliseconds: 500));
        await service.startScan();
        await service.rescan();
        expect(
          ble.scanSessions,
          hasLength(1),
          reason: 'Neither path bypasses the pinned native throttle before8s.',
        );
      }
      now = now.add(const Duration(milliseconds: 500));
      await service.startScan();
      expect(ble.scanSessions, hasLength(2));
      await service.rescan();
      expect(
        ble.scanSessions,
        hasLength(3),
        reason: 'Pull is allowed when the same cooldown expires.',
      );
      now = now.add(const Duration(milliseconds: 1500));
      await service.startScan();
      expect(ble.scanSessions, hasLength(3));
      now = now.add(const Duration(milliseconds: 500));
      await service.startScan();
      expect(
        ble.scanSessions,
        hasLength(4),
        reason: 'Automatic renewal resumes its2s window after cooldown.',
      );
    },
  );

  test(
    'connection retires a held discovery renewal before native connect',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      var now = DateTime(2026);
      final ble = _ScanBle()..separateScanSessions = true;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: () => now,
      );
      final cancellation = Completer<void>();
      addTearDown(() async {
        if (!cancellation.isCompleted) cancellation.complete();
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;
      ble.heldScanCancellation = cancellation;
      now = now.add(const Duration(seconds: 2));
      final renewal = service.startScan();
      await _waitFor(() => ble.scanCancellationCount == 1);
      final connection = service.connect('board');
      await service.rescan();
      expect(ble.connections, isEmpty);
      cancellation.complete();
      await renewal;
      await _waitFor(() => ble.connections.length == 1);
      expect(ble.scanSessions, hasLength(1));
      await service.startScan();
      await service.rescan();
      expect(ble.scanSessions, hasLength(1));
      await service.disconnect();
      expect(await connection, isFalse);
    },
  );

  for (final interruption in <String>['stop', 'background', 'dispose']) {
    test('held discovery renewal is retired by $interruption', () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      var now = DateTime(2026);
      final ble = _ScanBle()..separateScanSessions = true;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        scanClock: () => now,
      );
      final cancellation = Completer<void>();
      addTearDown(() async {
        if (!cancellation.isCompleted) cancellation.complete();
        await service.dispose();
        await ble.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final first = service.startScan();
      await _waitFor(() => ble.statusController.hasListener);
      ble.statusController.add(BleStatus.ready);
      await first;
      ble.heldScanCancellation = cancellation;
      now = now.add(const Duration(seconds: 2));
      final renewal = service.startScan();
      await _waitFor(() => ble.scanCancellationCount == 1);
      final retirement = switch (interruption) {
        'background' => service.setAppForegroundActive(false),
        'dispose' => service.dispose(),
        _ => service.stopScan(),
      };
      await service.startScan();
      await service.rescan();
      cancellation.complete();
      await renewal;
      await retirement;
      expect(
        ble.scanSessions,
        hasLength(1),
        reason: 'Retired cancellation cannot reopen a scan.',
      );
      if (interruption == 'background') {
        await service.startScan();
        expect(ble.scanSessions, hasLength(1));
        await service.setAppForegroundActive(true);
        await service.startScan();
        expect(ble.scanSessions, hasLength(2));
      }
    });
  }

  test('concurrent scan starts create one BLE scan subscription', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    final _ScanBle fakeBle = _ScanBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(() async {
      await service.dispose();
      await fakeBle.close();
      debugDefaultTargetPlatformOverride = null;
    });

    final Future<void> firstScan = service.startScan();
    final Future<void> duplicateScan = service.startScan();
    await _waitFor(() => fakeBle.statusController.hasListener);

    fakeBle.statusController.add(BleStatus.ready);
    await Future.wait(<Future<void>>[firstScan, duplicateScan]);

    expect(fakeBle.scanForDevicesCalls, 1);
    expect(fakeBle.scanSubscriptionCount, 1);
  });

  test(
    'adapter recovery resumes one requested scan after powered off',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      final _ScanBle fakeBle = _ScanBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      final List<BleAvailability> availability = <BleAvailability>[];
      final StreamSubscription<BleAvailability> availabilitySub = service
          .availabilityStream
          .listen(availability.add);
      addTearDown(() async {
        await availabilitySub.cancel();
        await service.dispose();
        await fakeBle.close();
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<void> requestedScan = service.startScan();
      await _waitFor(() => fakeBle.statusController.hasListener);
      fakeBle.statusController.add(BleStatus.poweredOff);
      await requestedScan;

      expect(fakeBle.scanForDevicesCalls, 0);
      expect(service.bleAvailability, BleAvailability.poweredOff);
      expect(availability, contains(BleAvailability.poweredOff));

      fakeBle.statusController.add(BleStatus.ready);
      await _waitFor(() => fakeBle.scanForDevicesCalls == 1);
      fakeBle.statusController.add(BleStatus.ready);
      await Future<void>.delayed(Duration.zero);

      expect(service.bleAvailability, BleAvailability.ready);
      expect(fakeBle.scanForDevicesCalls, 1);
      expect(fakeBle.scanSubscriptionCount, 1);
    },
  );

  test(
    'unknown adapter status remains checking until a terminal status',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      final _ScanBle fakeBle = _ScanBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() async {
        await service.dispose();
        await fakeBle.close();
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<void> requestedScan = service.startScan();
      await _waitFor(() => fakeBle.statusController.hasListener);
      fakeBle.statusController.add(BleStatus.unknown);
      await Future<void>.delayed(Duration.zero);

      expect(service.bleAvailability, BleAvailability.checking);
      fakeBle.statusController.add(BleStatus.ready);
      await requestedScan;
    },
  );

  test(
    'passive Android scan retries never request denied permissions',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final _ScanBle fakeBle = _ScanBle();
      final _PermissionGateway permissions =
          _PermissionGateway(<Permission, PermissionStatus>{
            Permission.bluetoothScan: PermissionStatus.denied,
            Permission.bluetoothConnect: PermissionStatus.denied,
            Permission.locationWhenInUse: PermissionStatus.denied,
          });
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        permissionGateway: permissions,
      );
      addTearDown(() async {
        await service.dispose();
        await fakeBle.close();
        debugDefaultTargetPlatformOverride = null;
      });

      for (int retry = 0; retry < 30; retry += 1) {
        await service.startScan();
      }

      expect(
        service.bleAvailability,
        BleAvailability.bluetoothPermissionDenied,
      );
      expect(permissions.requestCalls, 0);
      await service.requestBluetoothAccess();
      expect(permissions.requestCalls, 1);
    },
  );

  test('backgrounding invalidates a pending scan reservation', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    final _ScanBle fakeBle = _ScanBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(() async {
      await service.dispose();
      await fakeBle.close();
      debugDefaultTargetPlatformOverride = null;
    });

    final Future<void> pendingScan = service.startScan();
    await _waitFor(() => fakeBle.statusController.hasListener);
    await service.setAppForegroundActive(false);

    fakeBle.statusController.add(BleStatus.ready);
    await pendingScan;
    expect(fakeBle.scanForDevicesCalls, 0);

    await service.setAppForegroundActive(true);
    final Future<void> resumedScan = service.startScan();
    await _waitFor(() => fakeBle.statusController.hasListener);
    fakeBle.statusController.add(BleStatus.ready);
    await resumedScan;

    expect(fakeBle.scanForDevicesCalls, 1);
    expect(fakeBle.scanSubscriptionCount, 1);
  });

  testWidgets(
    'background and resume retain the same Console and shared session subscriptions',
    (tester) async {
      final peer = _RecoverySharedPeer();
      final statuses = <String>[];
      final status = peer.service.otaStatusStream.listen(statuses.add);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        await peer.service.ensureProductReady('board-1');
        await peer.startConsole(tester);
        peer.service.debugStartOtaStatusBridge('board-1');
        final session = await peer.service.openConnectionSession('board-1');
        final native = peer.native!;
        final subscriptions = List.of(native.subscriptions);
        expect(
          subscriptions,
          containsAll([
            BleProtocol.consoleStreamCharacteristic,
            BleProtocol.connectionSessionEgressCharacteristic,
          ]),
        );
        await Future.wait([
          peer.service.setAppForegroundActive(false),
          peer.service.setAppForegroundActive(true),
        ]);
        await tester.pump();
        expect(peer.service.appForegroundActive, isTrue);
        expect(
          await peer.service.openConnectionSession('board-1'),
          same(session),
        );
        for (final characteristic in [
          BleProtocol.consoleStreamCharacteristic,
          BleProtocol.connectionSessionEgressCharacteristic,
        ]) {
          expect(
            native.subscriptions.where((id) => id == characteristic),
            hasLength(1),
          );
          expect(native._notifications[characteristic]!.hasListener, isTrue);
        }
        native.emit(
          BleProtocol.consoleStreamCharacteristic,
          utf8.encode('after resume\n'),
        );
        await tester.pump();
        expect(
          peer.service.consoleLinesSnapshot.last,
          endsWith('after resume'),
        );
        var sequence = 0;
        for (final fragment in ConnectionOtaCodec.encodeControl(
          Uint8List.fromList(utf8.encode('{"v":"after-resume"}')),
          maximumBodyBytes: 10,
        )) {
          native.emit(
            BleProtocol.connectionSessionEgressCharacteristic,
            ConnectionSessionCodec.encode(
              ConnectionSessionPacket(
                kind: ConnectionSessionKind.object,
                code: ConnectionSession.otaStatusReceiptCode,
                token: session!.reverseObjectStreamToken,
                sequence: ++sequence,
                body: fragment,
              ),
            ),
          );
        }
        await tester.pump();
        expect(
          statuses.where((line) => line.contains('after-resume')),
          hasLength(1),
        );
      } finally {
        await _flushSdkCancellation(tester, status.cancel());
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  test('reconnect waits for pending explicit disconnect cleanup', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    final ble = _HeldDisconnectBle();
    final peer = _RecoverySharedPeer(connected: false, nativeBle: ble)
      ..infoPayload = '{"i":[3,4,1]}';
    final service = peer.service;
    StreamSubscription<ConnectionStateUpdate>? observer;
    Future<bool>? reconnecting;
    addTearDown(() async {
      if (!ble.nativeDisconnected.isCompleted) {
        ble.nativeDisconnected.complete();
      }
      await observer?.cancel();
      await _disposeReconnectHarness(service, ble);
      await peer.sessions.close();
      debugDefaultTargetPlatformOverride = null;
    });
    final connecting = service.connect('board-1');
    await _waitFor(() => ble.connections.length == 1);
    ble.connections.single.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    expect(await connecting, isTrue);
    expect(await service.ensureProductReady('board-1'), isTrue);
    final previous = await service.openConnectionSession('board-1');
    observer = service.connectionStream.listen((update) {
      if (update.connectionState == DeviceConnectionState.disconnected) {
        reconnecting ??= service.connect('board-1');
      }
    });
    bool disconnectComplete = false;
    final disconnecting = service.disconnect().whenComplete(
      () => disconnectComplete = true,
    );
    await ble.cancellationStarted.future;
    await _waitFor(() => reconnecting != null);
    for (int i = 0; i < 12; i++) {
      await Future<void>.delayed(Duration.zero);
    }
    expect(disconnectComplete, isFalse);
    expect(
      ble.connections,
      hasLength(1),
      reason: 'The old native link still owns teardown.',
    );
    ble.nativeDisconnected.complete();
    await disconnecting;
    await _waitFor(() => ble.connections.length == 2);
    ble.connections.last.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    expect(await reconnecting, isTrue);
    expect(await service.ensureProductReady('board-1'), isTrue);
    final current = await service.openConnectionSession('board-1');
    expect(current!.isActivated, isTrue);
    expect(current, isNot(same(previous)));
    expect(peer.sessions.sessions, hasLength(2));
    expect(service.connectedId, 'board-1');
  });

  for (final stop in ['disconnect', 'dispose']) {
    test('$stop cancels reconnect waiting on old native teardown', () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
      final ble = _HeldDisconnectBle();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
      );
      addTearDown(() async {
        if (!ble.nativeDisconnected.isCompleted) {
          ble.nativeDisconnected.complete();
        }
        await _disposeReconnectHarness(service, ble);
        debugDefaultTargetPlatformOverride = null;
      });
      final connecting = service.connect('board-1');
      await _waitFor(() => ble.connections.length == 1);
      ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
      final firstDisconnect = service.disconnect();
      await ble.cancellationStarted.future;
      final reconnecting = service.connect('board-1');
      final stopped = stop == 'dispose'
          ? service.dispose()
          : service.disconnect();
      expect(ble.connectionCancellationCount, 1);
      ble.nativeDisconnected.complete();
      await firstDisconnect;
      await stopped;
      expect(await reconnecting, isFalse);
      expect(ble.connections, hasLength(1));
      expect(service.isConnected, isFalse);
    });
  }

  test(
    'disconnect while fallback cancellation waits opens no next mode',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final ble = _HeldDisconnectBle();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
      );
      addTearDown(() async {
        if (!ble.nativeDisconnected.isCompleted) {
          ble.nativeDisconnected.complete();
        }
        await _disposeReconnectHarness(service, ble);
        debugDefaultTargetPlatformOverride = null;
      });
      final connecting = service.connect('board-1');
      await _waitFor(() => ble.connections.length == 1);
      ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await ble.cancellationStarted.future;
      final disconnected = service.disconnect();
      ble.nativeDisconnected.complete();
      expect(await connecting, isFalse);
      await disconnected;
      expect(ble.connections, hasLength(1));
      expect(ble.advertisingConnections, isEmpty);
      expect(service.isConnected, isFalse);
    },
  );

  test('duplicate connect cannot replace or cancel the owner', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.macOS;
    final _ReconnectBle fakeBle = _ReconnectBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(() async {
      await _disposeReconnectHarness(service, fakeBle);
      debugDefaultTargetPlatformOverride = null;
    });

    final Future<bool> owner = service.connect('board-1');
    await _waitFor(() => fakeBle.directConnections.length == 1);

    bool duplicateCompleted = false;
    bool? duplicateResult;
    final Future<bool> duplicate = service.connect('board-1');
    unawaited(
      duplicate.then((bool result) {
        duplicateResult = result;
        duplicateCompleted = true;
      }),
    );
    await _waitFor(
      () => duplicateCompleted || fakeBle.directConnections.length > 1,
    );

    expect(fakeBle.directConnections, hasLength(1));
    expect(fakeBle.advertisingConnections, isEmpty);
    expect(fakeBle.connectionCancellationCount, 0);
    expect(duplicateCompleted, isTrue);
    expect(duplicateResult, isFalse);

    fakeBle.directConnections.single.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );

    expect(await owner, isTrue);
    expect(fakeBle.directConnections, hasLength(1));
  });

  test(
    'connected observers wait for capability discovery before the console bridge',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final peer = _RecoverySharedPeer(connected: false);
      final service = peer.service;
      final discovery = Completer<void>();
      peer.sessions.holdOpen = discovery;
      peer.infoPayload = '{"i":[3,4,1]}';
      peer.onOrdered = () => peer.finish();
      final command = Completer<bool>();
      final observer = service.connectionStream.listen((update) {
        if (update.connectionState == DeviceConnectionState.connected &&
            !command.isCompleted) {
          command.complete(service.sendConsoleLine('board-1', 'help'));
        }
      });
      try {
        final connecting = service.connect('board-1');
        await _waitFor(() => peer.ble.directConnections.isNotEmpty);
        peer.ble.directConnections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        expect(command.isCompleted, isTrue);
        expect(peer.sessions.sessions, isEmpty);
        expect(peer.actions, 0);
        discovery.complete();
        expect(await command.future, isTrue);
        final ordered = _sharedPackets(peer.native!).where(
          (packet) =>
              packet.kind == ConnectionSessionKind.ordered &&
              packet.code == LitchControlProtocol.connectionConsoleOpcode,
        );
        expect(ordered, hasLength(1));
        expect(utf8.decode(ordered.single.body.sublist(3)), 'help\n');
        await _waitFor(
          () => peer.native!.subscriptions.contains(
            BleProtocol.consoleStreamCharacteristic,
          ),
        );
      } finally {
        if (!discovery.isCompleted) discovery.complete();
        await observer.cancel();
        await _disposeReconnectHarness(peer.service, peer.ble);
        await peer.sessions.close();
        debugDefaultTargetPlatformOverride = null;
      }
    },
  );

  test('Android direct success opens no advertising fallback', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    final peer = _RecoverySharedPeer(connected: false)
      ..infoPayload = '{"i":[3,4,4]}';
    peer.onOrdered = () => peer.finish();
    try {
      final connecting = peer.service.connect('board-1');
      await _waitFor(() => peer.ble.directConnections.length == 1);
      peer.ble.directConnections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
      expect(await peer.service.ensureProductReady('board-1'), isTrue);
      await _waitFor(() => peer.actions == 2);
      final identify = _sharedPackets(peer.native!).where(
        (packet) =>
            packet.kind == ConnectionSessionKind.ordered &&
            packet.code == ConnectionIdentifyCodec.opcode,
      );
      expect(identify.map((packet) => packet.body.toList()), [
        ConnectionIdentifyCodec.probe(),
        ConnectionIdentifyCodec.stop(),
      ]);
      expect(peer.ble.directConnections, hasLength(1));
      expect(peer.ble.advertisingConnections, isEmpty);
    } finally {
      await _disposeReconnectHarness(peer.service, peer.ble);
      await peer.sessions.close();
      debugDefaultTargetPlatformOverride = null;
    }
  });

  test(
    'Android direct failure opens exactly one advertising fallback',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final _ReconnectBle fakeBle = _ReconnectBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() async {
        await _disposeReconnectHarness(service, fakeBle);
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<bool> connecting = service.connect('board-1');
      await _waitFor(() => fakeBle.directConnections.length == 1);
      expect(fakeBle.directConnections, hasLength(1));

      fakeBle.directConnections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await fakeBle.advertisingConnectionOpened.future.timeout(
        const Duration(seconds: 1),
      );

      expect(fakeBle.directConnections, hasLength(1));
      expect(fakeBle.advertisingConnections, hasLength(1));

      fakeBle.advertisingConnections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );

      expect(await connecting, isTrue);
      expect(fakeBle.advertisingConnections, hasLength(1));
    },
  );

  test(
    'Android repeated pre-connect failures get one delayed GATT recovery attempt',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final _ReconnectBle fakeBle = _ReconnectBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() async {
        await _disposeReconnectHarness(service, fakeBle);
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<bool> connecting = service.connect('board-1');
      await _waitFor(() => fakeBle.directConnections.length == 1);
      fakeBle.directConnections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: GenericFailure<ConnectionError>(
            code: ConnectionError.failedToConnect,
            message: 'GATT_ERROR(133)',
          ),
        ),
      );
      await fakeBle.advertisingConnectionOpened.future.timeout(
        const Duration(seconds: 1),
      );
      fakeBle.advertisingConnections.first.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: GenericFailure<ConnectionError>(
            code: ConnectionError.failedToConnect,
            message: 'GATT_ERROR(133)',
          ),
        ),
      );

      await Future<void>.delayed(const Duration(milliseconds: 1700));
      expect(fakeBle.advertisingConnections, hasLength(2));
      fakeBle.advertisingConnections.last.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );

      expect(await connecting, isTrue);
      expect(fakeBle.directConnections, hasLength(1));
      expect(fakeBle.advertisingConnections, hasLength(2));
    },
  );

  testWidgets(
    'keepalive waits for exact GATT readiness without a false failure or late startup ping',
    (tester) async {
      final peer = _RecoverySharedPeer(connected: false);
      final discovery = Completer<void>();
      peer.sessions.holdOpen = discovery;
      try {
        _setHeartbeatProfile(peer);
        peer.service.debugStartKeepAlive('board-1');
        await tester.pump();
        await tester.pump(const Duration(seconds: 3));
        expect(peer.sessions.sessions, isEmpty);
        expect(
          peer.service.consoleLinesSnapshot.where(
            (line) => line.contains('keepalive write failed'),
          ),
          isEmpty,
        );
        discovery.complete();
        await tester.pump();
        final native = peer.sessions.sessions.single;
        expect(
          _heartbeats(native),
          0,
          reason:
              'Resolved handles must not release a stale startup heartbeat.',
        );
        await tester.pump(const Duration(seconds: 1));
        await tester.pump();
        expect(_heartbeats(native), 1);
      } finally {
        if (!discovery.isCompleted) discovery.complete();
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'keepalive readiness is renewed on same-ID reconnect and ignores retired discovery',
    (tester) async {
      final peer = _RecoverySharedPeer(connected: false);
      final retiredDiscovery = Completer<void>();
      final currentDiscovery = Completer<void>();
      try {
        _setHeartbeatProfile(peer);
        await _openRecoveryPeer(tester, peer);
        peer.service.debugStartKeepAlive('board-1');
        await tester.pump();
        final first = peer.native!;
        expect(_heartbeats(first), greaterThanOrEqualTo(1));
        await _flushSdkCancellation(tester, peer.service.disconnect());
        peer.sessions.holdOpen = retiredDiscovery;
        _setHeartbeatProfile(peer);
        peer.service.debugStartKeepAlive('board-1');
        await tester.pump(const Duration(seconds: 7));
        await _flushSdkCancellation(tester, peer.service.disconnect());
        peer.sessions.holdOpen = currentDiscovery;
        _setHeartbeatProfile(peer);
        peer.service.debugStartKeepAlive('board-1');
        await tester.pump();
        await tester.pump(const Duration(seconds: 3));
        retiredDiscovery.complete();
        await tester.pump(const Duration(seconds: 1));
        expect(peer.sessions.sessions, hasLength(2));
        final retired = peer.sessions.sessions.last;
        expect(_heartbeats(retired), 0);
        currentDiscovery.complete();
        await tester.pump();
        final current = peer.sessions.sessions.last;
        expect(peer.sessions.sessions, hasLength(3));
        expect(_heartbeats(current), 0);
        await tester.pump(const Duration(seconds: 4));
        await tester.pump();
        expect(_heartbeats(current), 1);
        expect(_heartbeats(retired), 0);
        expect(
          peer.service.consoleLinesSnapshot.where(
            (line) => line.contains('keepalive write failed'),
          ),
          isEmpty,
        );
      } finally {
        if (!retiredDiscovery.isCompleted) retiredDiscovery.complete();
        if (!currentDiscovery.isCompleted) currentDiscovery.complete();
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'transient keepalive write failures do not stop later keepalives',
    (tester) async {
      final peer = _RecoverySharedPeer(connected: false);
      var failNext = false;
      peer.sessions.onWrite = (session, id, bytes) {
        if (failNext &&
            id == BleProtocol.connectionSessionIngressCharacteristic) {
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(bytes),
          );
          if (packet?.kind == ConnectionSessionKind.control &&
              packet?.code == ConnectionSessionControl.heartbeat) {
            failNext = false;
            throw StateError('controlled native heartbeat failure');
          }
        }
        peer.write(session, id, bytes);
      };
      try {
        _setHeartbeatProfile(peer);
        await _openRecoveryPeer(tester, peer);
        peer.service.debugStartKeepAlive('board-1');
        await tester.pump();
        final native = peer.native!;
        final before = _heartbeats(native);
        failNext = true;
        await tester.pump(const Duration(seconds: 4));
        expect(_heartbeats(native), before + 1);
        expect(
          peer.service.consoleLinesSnapshot.where(
            (line) => line.contains('keepalive write failed'),
          ),
          isNotEmpty,
        );
        await tester.pump(const Duration(seconds: 4));
        expect(_heartbeats(native), before + 2);
        expect(
          (await peer.service.openConnectionSession('board-1'))?.isActivated,
          isTrue,
        );
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'current Home snapshot request reuses its healthy stream and explicitly requests state',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        final native = peer.native!;
        final before = peer.queries;
        peer.onStateQuery = null;
        var completed = false;
        final request = peer.service
            .requestCurrentHomeStateSnapshot('board-1')
            .whenComplete(() => completed = true);
        for (var i = 0; i < 8 && before == peer.queries; ++i) {
          await tester.pump();
        }
        expect(completed, isFalse);
        peer.home(revision: 2, action: HomeStateAction.idle);
        for (var i = 0; i < 8 && !completed; ++i) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(completed, isTrue);
        expect(await request, isTrue);
        expect(peer.queries, before + 1);
        expect(peer.service.latestHomeState?.sequence, 2);
        expect(
          native.subscriptions.where(
            (id) => id == BleProtocol.connectionSessionEgressCharacteristic,
          ),
          hasLength(1),
        );
        expect(peer.sessions.sessions.single, same(native));
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  test(
    'production health/support fixture survives BLE chunks and late delivery',
    () async {
      final List<String> lines = File(
        'test/fixtures/board_health_console.jsonl',
      ).readAsLinesSync();
      for (final int chunkSize in <int>[1, 20, 244]) {
        final LitchBleService service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _ConsoleCommandBle(),
        );
        service.debugSetConnectedDevice('board-1');
        final List<int> bytes = utf8.encode(lines.join('\r\n'));
        for (int offset = 0; offset < bytes.length; offset += chunkSize) {
          final int end = (offset + chunkSize).clamp(0, bytes.length);
          service.debugAppendConsolePacket(bytes.sublist(offset, end));
        }
        Iterable<String> incidentLines() => service.consoleLinesSnapshot.where(
          (line) => line.contains('"event":"support_incident"'),
        );
        expect(incidentLines(), isEmpty);
        // A delayed terminator must complete the original bounded line; a
        // Console packet is not a reply to the typed support collector.
        await Future<void>.delayed(Duration.zero);
        service.debugAppendConsolePacket(utf8.encode('\r\n'));
        expect(incidentLines(), hasLength(1));
        final line = incidentLines().single;
        final incident = SupportIncident.tryParse(
          line.substring(line.indexOf('{')),
        );
        expect(incident, isNotNull);
        expect(incident!.fields['prior_up'], 23456);
        expect(incident.fields['stages'], <int>[9, 8, 14, 14]);
        expect(service.latestBoardHealth?.sessionOnly, isTrue);
        expect(service.latestBoardHealth?.watchdogRestart, isTrue);
        await service.disconnect();
        expect(service.latestBoardHealth, isNull);
        await service.dispose();
      }
    },
  );

  test(
    'support incident collection uses the current shared-session owner',
    () async {
      SharedPreferences.setMockInitialValues({});
      final board = SupportSessionBoard();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: board,
      );
      addTearDown(() => ota_board.disposeBoard(service, board));
      await ota_board.connectBoard(service, board);
      expect(await service.ensureProductReady('board-1'), isTrue);
      final incident = await service.fetchSupportIncident(deviceId: 'board-1');
      expect(incident, isNotNull);
      expect(incident!.fields['saved'], 7);
      expect(incident.fields['failed'], 1);
      expect(board.requests.map((request) => (request.$1, request.$2)), [
        (0x45, 0),
        (0x44, 0),
        (0x44, 1),
      ]);
      expect(board.firstOversizeNativeWrite, isNull);
    },
  );

  test(
    'console publishes once per completed packet and remains bounded',
    () async {
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FailingBle(),
      );
      addTearDown(service.dispose);
      service.debugSetConsoleTraceOutputEnabled(true);
      final List<List<String>> snapshots = <List<String>>[];
      final StreamSubscription<List<String>> subscription = service
          .consoleStream
          .listen(snapshots.add);
      addTearDown(subscription.cancel);

      service.debugAppendConsolePacket(utf8.encode('0.000 [TRACE] par'));
      service.debugAppendConsolePacket(utf8.encode('tial'));
      await Future<void>.delayed(Duration.zero);
      expect(snapshots, isEmpty);

      service.debugAppendConsolePacket(utf8.encode('\n0.001 [TRACE] second\n'));
      await Future<void>.delayed(Duration.zero);
      expect(snapshots, hasLength(1));
      expect(snapshots.single, hasLength(2));

      // The firmware mirror sends completed response records, not a command
      // echo classification. BLE packet boundaries may split either record.
      service.debugAppendConsolePacket(utf8.encode('Console\nTransfer com'));
      service.debugAppendConsolePacket(
        utf8.encode('plete\nx > y\n\r> v\r> version\r\n  5 > 3\nhaha\n\n'),
      );
      // Split a multibyte character between notifications as a real link may.
      for (final byte in utf8.encode('réponse ✓\n')) {
        service.debugAppendConsolePacket([byte]);
      }
      await Future<void>.delayed(Duration.zero);
      expect(
        service.consoleLinesSnapshot
            .skip(2)
            .map((line) => line.split('[FW] ').last),
        [
          'Console',
          'Transfer complete',
          'x > y',
          '> version',
          '  5 > 3',
          'haha',
          'réponse ✓',
        ],
      );

      for (int index = 0; index < 130; index += 1) {
        service.debugAppendConsolePacket(utf8.encode('$index [INFO] line\n'));
      }
      await Future<void>.delayed(Duration.zero);
      expect(service.consoleLinesSnapshot, hasLength(120));
    },
  );

  test(
    'Settings State supersedes late descriptor and value reads before an Ordered edit',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () => peer.home(revision: 42);
      await peer.open();
      await _waitFor(() => peer.service.latestHomeState != null);
      final descriptor = Completer<List<int>>();
      final native = peer.native!;
      native.onRead = (characteristic) {
        expect(
          characteristic.characteristicId,
          BleProtocol.settingsEnumerateCharacteristic,
        );
        return descriptor.future;
      };
      final metadata = peer.service.readSettings('board-1');
      final reading = peer.service.readSettingValue(
        deviceId: 'board-1',
        key: 'BRIGHTNESS',
      );
      await _waitFor(
        () =>
            native.reads.contains(BleProtocol.settingsEnumerateCharacteristic),
      );

      final observed = <(int?, Object?)>[];
      final changes = peer.service.settingsChangedStream.listen((key) {
        if (key == 'BRIGHTNESS') {
          observed.add((
            peer.service.boardStateSettingsRevision,
            peer.service.latestSettingValue(key),
          ));
        }
      });
      addTearDown(changes.cancel);
      peer.settings(count: 8, revision: 14, brightness: 6);
      await Future<void>.delayed(Duration.zero);
      expect(peer.service.hasBoardStateSnapshot, isFalse);
      expect(observed, isEmpty);
      peer.settings(firstFragment: 8, count: 1, revision: 14, brightness: 6);
      await _waitFor(() => observed.isNotEmpty);
      expect(observed, [(14, 6)]);
      descriptor.complete(
        utf8.encode('[{"i":0,"k":"BRIGHTNESS","t":"u32","v":3}]'),
      );
      expect((await metadata).single.value, 6);
      expect(await reading, 6);

      peer.onStateQuery = () {};
      peer.onOrdered = () => peer.finish(revision: 15);
      final edit =
          await peer.service.writeSettingAcknowledged(
                deviceId: 'board-1',
                key: 'BRIGHTNESS',
                value: 8,
              )
              as LitchSettingWriteAcknowledged;
      expect(edit.outcome.ok, isTrue);
      expect(
        await peer.service.readSettingValue(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
        ),
        6,
      );
      expect(observed, [(14, 6)]);
      peer.settings(revision: 15, brightness: 8);
      expect(await edit.boardStateConfirmation, isTrue);
      expect(observed, [(14, 6), (15, 8)]);
      expect(
        native.writes,
        isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
      );
    },
  );

  test(
    'Settings State cannot use a superseded revision to confirm the newer edit',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 14, brightness: 9);
      };
      await peer.open();
      await _waitFor(() => peer.service.hasBoardStateSnapshot);
      peer.onStateQuery = () {};
      peer.onOrdered = () => peer.finish(revision: 14 + peer.actions);

      Future<LitchSettingWriteAcknowledged> edit(int value) async =>
          await peer.service.writeSettingAcknowledged(
                deviceId: 'board-1',
                key: 'BRIGHTNESS',
                value: value,
              )
              as LitchSettingWriteAcknowledged;
      final first = await edit(7);
      final second = await edit(9);
      expect([first.outcome.ok, second.outcome.ok], [true, true]);
      bool? firstConfirmed, secondConfirmed;
      first.boardStateConfirmation!.then((value) => firstConfirmed = value);
      second.boardStateConfirmation!.then((value) => secondConfirmed = value);

      // The board starts at 9, changes to 7, then returns to 9.
      // Replaying the original 9 cannot confirm the second edit.
      peer.settings(revision: 14, brightness: 9);
      await Future<void>.delayed(Duration.zero);
      expect(firstConfirmed, isNull);
      expect(secondConfirmed, isNull);
      peer.settings(revision: 16, brightness: 9);
      await _waitFor(() => secondConfirmed != null);
      expect(secondConfirmed, isTrue);
      peer.settings(revision: 15, brightness: 7);
      await Future<void>.delayed(Duration.zero);
      expect(firstConfirmed, isNull);
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 9);
      await peer.service.disconnect();
      expect(await first.boardStateConfirmation, isFalse);
      expect(peer.actions, 2);
    },
  );

  for (final failure in ['deadline', 'replacement', 'transport error']) {
    test(
      'Settings State retains an accepted edit after a readback $failure',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 14, brightness: 5);
        };
        await peer.open();
        await _waitFor(() => peer.service.hasBoardStateSnapshot);
        final queried = Completer<void>();
        peer.onOrdered = () => peer.finish(revision: 15);
        peer.onStateQuery = () {
          if (!queried.isCompleted) queried.complete();
          if (failure == 'transport error') {
            throw StateError('State Query write failed');
          }
        };
        final writing = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
          value: 8,
        );
        await queried.future;
        if (failure == 'replacement') {
          final capabilities = peer.service.deviceCapabilities('board-1');
          await peer.service.disconnect();
          peer.service.debugSetConnectedDevice('board-1');
          peer.service.debugSetDeviceCapabilities('board-1', capabilities);
          peer.onStateQuery = () {
            peer.home();
            peer.settings(revision: 15, brightness: 8);
          };
          await peer.open();
        }
        final edit = await writing as LitchSettingWriteAcknowledged;
        expect(edit.outcome.ok, isTrue);
        expect(await edit.boardStateConfirmation, isFalse);
        peer.settings(revision: 15, brightness: 8);
        await _waitFor(
          () => peer.service.latestSettingValue('BRIGHTNESS') == 8,
        );
        expect(await edit.boardStateConfirmation, isFalse);
        expect(
          peer.actions,
          1,
          reason: 'State recovery cannot repeat the applied mutation.',
        );
      },
    );
  }

  test(
    'Settings State completes a foreground edit while the app is in the background',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 14, brightness: 5);
      };
      await peer.open();
      await _waitFor(() => peer.service.hasBoardStateSnapshot);
      final token = peer.service.connectionSessionToken;
      peer.onStateQuery = () {};
      final writing = peer.service.writeSettingAcknowledged(
        deviceId: 'board-1',
        key: 'BRIGHTNESS',
        value: 8,
      );
      await _waitFor(() => peer.retained);
      await peer.service.setAppForegroundActive(false);
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 5);
      peer.finish(revision: 15);
      final edit = await writing as LitchSettingWriteAcknowledged;
      expect(edit.outcome.ok, isTrue);
      peer.settings(revision: 15, brightness: 8);
      expect(await edit.boardStateConfirmation, isTrue);
      expect(peer.service.appForegroundActive, isFalse);
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 8);
      await peer.service.setAppForegroundActive(true);
      expect(peer.service.latestSettingValue('BRIGHTNESS'), 8);
      expect(peer.service.connectionSessionToken, token);
      expect(peer.actions, 1);
    },
  );

  testWidgets(
    'typed Home stays retained through Console history and trace changes',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        final home = peer.service.latestHomeState!;
        expect(home.coachingMask, isNotNull);
        peer.service.debugSetConsoleTraceOutputEnabled(true);
        for (var i = 0; i < 120; i++) {
          peer.service.debugAppendConsolePacket(
            utf8.encode('$i [INFO] line\n'),
          );
        }
        peer.service.debugAppendConsolePacket(
          utf8.encode('0 [TRACE] chatter\n'),
        );
        peer.service.debugSetConsoleTraceOutputEnabled(false);
        await tester.pump();
        expect(peer.service.consoleLinesSnapshot, hasLength(119));
        expect(
          peer.service.consoleLinesSnapshot.first,
          contains('1 [INFO] line'),
        );
        expect(peer.service.consoleTraceOutputEnabled, isFalse);
        expect(peer.service.latestHomeState, same(home));
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  testWidgets(
    'Hint cache invalidation preserves Home and ignores duplicate State',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        final before = peer.service.latestHomeState!;
        final sequence = before.sequence;
        expect(before.assistance?.isReady, isTrue);
        peer.service.discardCachedHomeAssistance(
          boardId: 'different-board',
          sequence: sequence,
        );
        peer.service.discardCachedHomeAssistance(
          boardId: 'board-1',
          sequence: sequence + 1,
        );
        expect(peer.service.latestHomeState?.assistance, isNotNull);
        peer.service.discardCachedHomeAssistance(
          boardId: 'board-1',
          sequence: sequence,
        );
        final after = peer.service.latestHomeState!;
        expect(after.assistance, isNull);
        expect(after.sequence, sequence);
        expect(after.position, same(before.position));
        expect(after.coachingMask, before.coachingMask);
        expect(after.undoPending, before.undoPending);
        expect(after.clockStatus, same(before.clockStatus));
        peer.home();
        await tester.pump();
        expect(peer.service.latestHomeState?.assistance, isNull);
        peer.home(revision: 2, action: HomeStateAction.idle);
        await tester.pump();
        final refreshed = peer.service.latestHomeState!;
        expect(refreshed.assistance?.isReady, isTrue);
        peer.service.discardCachedHomeAssistance(
          boardId: 'board-1',
          sequence: sequence,
        );
        expect(peer.service.latestHomeState?.assistance?.isReady, isTrue);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  for (final target in <String>['board-1', 'board-2']) {
    testWidgets('Home state is renewed for connection to $target', (
      tester,
    ) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      try {
        await _openRecoveryPeer(tester, peer);
        expect(peer.service.latestHomeState, isNotNull);
        await _flushSdkCancellation(tester, peer.service.disconnect());
        expect(peer.service.latestHomeState, isNull);
        peer.service.debugSetConnectedDevice(target);
        peer.service.debugSetDeviceCapabilities(
          target,
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.settingsBit,
            explicitlyReported: true,
          ),
        );
        await _openRecoveryPeer(tester, peer, deviceId: target);
        expect(peer.service.latestHomeState, isNotNull);
        expect(peer.service.latestHomeState!.sequence, 1);
      } finally {
        await _flushSdkCancellation(tester, peer.close());
      }
    });
  }

  test(
    'normal console drops trace traffic without hiding protocol or info lines',
    () async {
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FailingBle(),
      );
      addTearDown(service.dispose);
      final List<List<String>> snapshots = <List<String>>[];
      final StreamSubscription<List<String>> subscription = service
          .consoleStream
          .listen(snapshots.add);
      addTearDown(subscription.cancel);

      service.debugAppendConsolePacket(
        utf8.encode(
          '0.000 [TRACE] sensor chatter\n'
          '{"type":"tick","t_ms":1}\n'
          '{"event":"home_state","v":1}\n'
          '0.001 [INFO] useful status\n',
        ),
      );
      await Future<void>.delayed(Duration.zero);

      expect(snapshots, hasLength(1));
      expect(service.consoleLinesSnapshot, hasLength(2));
      expect(
        service.consoleLinesSnapshot,
        everyElement(isNot(contains('[TRACE]'))),
      );
      expect(
        service.consoleLinesSnapshot,
        everyElement(isNot(contains('{"type":"tick"'))),
      );
      expect(
        service.consoleLinesSnapshot,
        contains(contains('{"event":"home_state"')),
      );
      expect(
        service.consoleLinesSnapshot,
        contains(contains('[INFO] useful status')),
      );
    },
  );

  test(
    'default trace flood causes no history snapshots or retained lines',
    () async {
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FailingBle(),
      );
      addTearDown(service.dispose);
      final List<List<String>> snapshots = <List<String>>[];
      final StreamSubscription<List<String>> subscription = service
          .consoleStream
          .listen(snapshots.add);
      addTearDown(subscription.cancel);

      for (int index = 0; index < 500; index += 1) {
        service.debugAppendConsolePacket(
          utf8.encode('$index.000 [TRACE] sensor=$index\n'),
        );
      }
      await Future<void>.delayed(Duration.zero);

      expect(snapshots, isEmpty);
      expect(service.consoleLinesSnapshot, isEmpty);
    },
  );

  testWidgets('trace output command controls retention and purges trace', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    peer.onOrdered = peer.finish;
    try {
      await _openRecoveryPeer(tester, peer);
      final states = <bool>[];
      final subscription = peer.service.consoleTraceOutputStream.listen(
        states.add,
      );
      Future<bool> change(bool enabled) async {
        bool? result;
        peer.service
            .setConsoleTraceOutputEnabled('board-1', enabled)
            .then((v) => result = v);
        for (var i = 0; i < 12 && result == null; i++) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(result, isNotNull);
        return result!;
      }

      expect(await change(true), isTrue);
      expect(peer.service.consoleTraceOutputEnabled, isTrue);
      peer.service.debugAppendConsolePacket(
        utf8.encode(
          '0.000 [TRACE] sensor chatter\n{"type":"tick","t_ms":1}\n0.001 [INFO] useful status\n',
        ),
      );
      expect(
        peer.service.consoleLinesSnapshot.where(
          (line) => line.contains('[FW]'),
        ),
        hasLength(3),
      );
      expect(await change(false), isTrue);
      expect(peer.service.consoleTraceOutputEnabled, isFalse);
      expect(states, [true, false]);
      final firmwareLines = peer.service.consoleLinesSnapshot.where(
        (line) => line.contains('[FW]'),
      );
      expect(firmwareLines, hasLength(1));
      expect(firmwareLines.single, contains('[INFO]'));
      final commands = <String>[];
      var record = <int>[];
      for (final write in peer.native!.writeRecords) {
        if (write.characteristicId !=
            BleProtocol.connectionSessionIngressCharacteristic) {
          continue;
        }
        final packet = ConnectionSessionCodec.decode(
          Uint8List.fromList(write.value),
        );
        if (packet?.kind != ConnectionSessionKind.ordered ||
            packet?.code != LitchControlProtocol.connectionConsoleOpcode) {
          continue;
        }
        final body = packet!.body;
        final begin =
            body[1] & LitchControlProtocol.connectionConsoleBegin != 0;
        final end = body[1] & LitchControlProtocol.connectionConsoleEnd != 0;
        if (begin) record = <int>[];
        record.addAll(body.sublist(begin ? 3 : 2));
        if (end) commands.add(utf8.decode(record));
      }
      expect(commands, ['debug trace\n', 'debug info\n']);
      expect(
        peer.actions,
        4,
        reason: 'two multi-fragment Ordered console commands',
      );
      await _flushSdkCancellation(tester, subscription.cancel());
    } finally {
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  for (final initiallyEnabled in [false, true]) {
    test(
      'failed current trace command preserves enabled=$initiallyEnabled',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        await peer.open();
        peer.service.debugSetConsoleTraceOutputEnabled(initiallyEnabled);
        peer.onOrdered = () => throw StateError('controlled native failure');
        expect(
          await peer.service.setConsoleTraceOutputEnabled(
            'board-1',
            !initiallyEnabled,
          ),
          isFalse,
        );
        expect(
          peer.actions,
          1,
          reason: 'the admitted current command attempted its native send',
        );
        expect(peer.service.consoleTraceOutputEnabled, initiallyEnabled);
      },
    );
  }

  test('disconnect resets trace state and purges retained trace', () async {
    final _ConsoleCommandBle fakeBle = _ConsoleCommandBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(service.dispose);
    service.debugSetConnectedDevice('board-1');
    service.debugSetConsoleTraceOutputEnabled(true);
    service.debugAppendConsolePacket(
      utf8.encode('0.000 [TRACE] sensor chatter\n0.001 [INFO] status\n'),
    );

    await service.disconnect();

    expect(service.consoleTraceOutputEnabled, isFalse);
    expect(service.consoleLinesSnapshot, hasLength(1));
    expect(service.consoleLinesSnapshot.single, contains('[INFO] status'));
  });

  testWidgets('pending trace command cannot cross a disconnected session', (
    tester,
  ) async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    try {
      await _openRecoveryPeer(tester, peer);
      bool? enabled;
      final pending = peer.service
          .setConsoleTraceOutputEnabled('board-1', true)
          .then((value) => enabled = value);
      for (var i = 0; i < 8 && peer.actions == 0; i++) {
        await tester.pump();
      }
      expect(peer.actions, 1);
      await _flushSdkCancellation(tester, peer.service.disconnect());
      for (var i = 0; i < 8 && enabled == null; i++) {
        await tester.pump();
        await tester.runAsync(() async {});
      }
      await pending;
      expect(enabled, isFalse);
      expect(peer.service.consoleTraceOutputEnabled, isFalse);
    } finally {
      await _flushSdkCancellation(tester, peer.close());
    }
  });

  test(
    'newline-free console input cannot retain an oversized partial line',
    () async {
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _FailingBle(),
      );
      addTearDown(service.dispose);

      service.debugAppendConsolePacket(List<int>.filled(4096, 0x61));
      service.debugAppendConsolePacket(utf8.encode('\n0.001 [INFO] ready\n'));
      await Future<void>.delayed(Duration.zero);

      expect(service.consoleLinesSnapshot, hasLength(1));
      expect(service.consoleLinesSnapshot.single, contains('[INFO] ready'));
    },
  );

  test(
    'bridge and firmware request share one OTA-control subscription',
    () async {
      SharedPreferences.setMockInitialValues({});
      final board = ota_board.RebootReplyBle()
        ..runningVersion = '2.3.4'
        ..dropFirstVersionReply = false;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: board,
      );
      addTearDown(() => ota_board.disposeBoard(service, board));
      await ota_board.connectBoard(service, board);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(board.sessionActivated, isTrue);
      expect(
        board.otaSubscriptions,
        1,
        reason: 'Bootstrap subscribes to the OTA-control characteristic once.',
      );
      service.debugStartOtaStatusBridge('board-1');
      final observed = service.otaStatusStream.firstWhere(
        (line) => line == 'OTA: version (2.3.4)',
      );
      expect(
        await service.fetchBoardFirmwareVersion(deviceId: 'board-1'),
        '2.3.4',
      );
      await observed.timeout(const Duration(seconds: 1));
      expect(board.versionRequests, 1);
      expect(board.otaSubscriptions, 1);
    },
  );

  test(
    'actual background link loss creates a fresh OTA notification session',
    () async {
      final _ReconnectBle fakeBle = _ReconnectBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );

      final Future<bool> firstConnect = service.connect('board-1');
      while (fakeBle.connections.isEmpty) {
        await Future<void>.delayed(Duration.zero);
      }
      fakeBle.connections[0].add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await firstConnect, isTrue);
      await Future<void>.delayed(Duration.zero);
      expect(fakeBle.otaSubscribeAttempts, 1);

      await service.setAppForegroundActive(false);
      fakeBle.connections[0].add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(service.isConnected, isFalse);
      await service.setAppForegroundActive(true);

      final Future<bool> secondConnect = service.connect('board-1');
      while (fakeBle.connections.length < 2) {
        await Future<void>.delayed(Duration.zero);
      }
      fakeBle.connections[1].add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await secondConnect, isTrue);
      await Future<void>.delayed(Duration.zero);

      expect(fakeBle.otaSubscribeAttempts, 2);
      await service.dispose();
      for (final StreamController<ConnectionStateUpdate> stream
          in fakeBle.connections) {
        await stream.close();
      }
      for (final StreamController<List<int>> stream
          in fakeBle.notificationStreams) {
        await stream.close();
      }
    },
  );

  test(
    'raw frame-store packets cannot bypass the connection Object stream',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final fakeBle = _ReconnectBle();
      final sessions = _RecordingGattSessionFactory();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        gattSessionFactory: sessions,
      );
      addTearDown(() async {
        await service.dispose();
        await sessions.close();
        debugDefaultTargetPlatformOverride = null;
      });
      service.debugSetConnectedDevice('board-1');
      await _waitFor(() => sessions.sessions.isNotEmpty);
      await service.litchcraftPacketPayloadBytes(
        'board-1',
        connectionSessionToken: service.connectionSessionToken,
      );
      expect(fakeBle.lastRequestedMtu, 517);

      for (final operation in <int>[
        LitchcraftV2Protocol.frameStoreBegin,
        LitchcraftV2Protocol.frameStoreCheckpoint,
        LitchcraftV2Protocol.frameStoreCommit,
      ]) {
        final bytes = Uint8List(
          operation == LitchcraftV2Protocol.frameStoreBegin ? 53 : 13,
        );
        bytes[0] = LitchcraftV2Protocol.magic;
        bytes[1] = LitchcraftV2Protocol.version;
        bytes[2] = operation;
        expect(
          () => service.writeLitchcraftBulkDocumentPacket(
            'board-1',
            bytes,
            connectionSessionToken: service.connectionSessionToken,
          ),
          throwsStateError,
        );
      }
      expect(sessions.sessions.single.writeRecords, isEmpty);
    },
  );

  Future<void> expectRefusedCraftSession(
    Future<void> Function(LitchBleService service) send,
  ) async {
    final peer = _RecoverySharedPeer(connected: false);
    final hellos = <ConnectionSessionPacket>[];
    peer.sessions.onWrite = (native, characteristic, bytes) {
      expect(
        characteristic,
        BleProtocol.connectionSessionIngressCharacteristic,
      );
      final hello = ConnectionSessionCodec.decode(Uint8List.fromList(bytes))!;
      expect(hello.kind, ConnectionSessionKind.control);
      expect(hello.code, ConnectionSessionControl.hello);
      hellos.add(hello);
      peer.native = native;
      peer.control(ConnectionSessionControl.welcome, 1, 0, hello.body);
      final identity = Uint8List(10);
      ByteData.sublistView(identity)
        ..setUint64(0, 1, Endian.little)
        ..setUint16(
          8,
          ConnectionSession.orderedFeature | ConnectionSession.stateFeature,
          Endian.little,
        );
      peer.control(ConnectionSessionControl.identity, 1, 0, identity);
    };
    try {
      peer.service.debugSetConnectedDevice('board-1');
      peer.service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.litchcraftBit |
              LitchDeviceCapabilities.litchcraftBulkBit |
              LitchDeviceCapabilities.litchcraftInspectBit,
          explicitlyReported: true,
        ),
      );
      await expectLater(
        send(peer.service),
        throwsA(
          isA<FormatException>().having(
            (error) => error.message,
            'unsupported transcript',
            'Invalid connection-session transcript',
          ),
        ),
      );
      expect(hellos, hasLength(1));
      expect(peer.native!.writes, [
        BleProtocol.connectionSessionIngressCharacteristic,
      ]);
      expect(peer.native!.batchWrites, isEmpty);
    } finally {
      await peer.close();
    }
  }

  test(
    'V3 session refusal blocks Litchcraft Enter on the direct characteristic',
    () async {
      await expectRefusedCraftSession(
        (service) => service.writeLitchcraftPacket(
          'board-1',
          LitchcraftProtocol.request(LitchcraftProtocol.enter, 0, 1),
        ),
      );
    },
  );

  test(
    'V3 session refusal blocks document requests on the direct characteristic',
    () async {
      await expectRefusedCraftSession(
        (service) => service.writeLitchcraftV2Packet(
          'board-1',
          LitchcraftV2Protocol.request(LitchcraftV2Protocol.inspect, 0, 1),
          connectionSessionToken: service.connectionSessionToken,
        ),
      );
      await expectRefusedCraftSession(
        (service) => service.writeLitchcraftBulkDocumentPacket(
          'board-1',
          LitchcraftV2Protocol.request(
            LitchcraftV2Protocol.streamCheckpoint,
            1,
            2,
            [1, 0, 0, 0],
          ),
          connectionSessionToken: service.connectionSessionToken,
        ),
      );
    },
  );

  for (final modeFeature in [true, false]) {
    test(
      modeFeature
          ? 'Litchcraft refusal preserves the BLE session for another domain owner'
          : 'missing Ordered mode feature stays unsupported without Object fallback',
      () async {
        debugDefaultTargetPlatformOverride = TargetPlatform.android;
        final ble = _ReconnectBle();
        final sessions = _RecordingGattSessionFactory();
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: ble,
          gattSessionFactory: sessions,
        );
        final owner = LitchcraftController(service);
        final draftReady = Completer<void>();
        final assembler = ConnectionLitchcraftAssembler();
        final operations = <int>[];
        var reverseToken = 0;
        var reverseSequence = 0;
        var controlActions = 0;
        var objectGrant = ConnectionSessionCodec.maximumWindow;
        var objectAccepted = 0;
        var freezeObjectGrant = false;
        final objectWrites = <ConnectionSessionPacket>[];
        addTearDown(() async {
          owner.dispose();
          if (!draftReady.isCompleted) draftReady.complete();
          await _disposeReconnectHarness(service, ble);
          await sessions.close();
          debugDefaultTargetPlatformOverride = null;
        });

        // Only firmware wire replies and the external SDK are scripted. Enter
        // pauses at the real draft boundary so a compact refusal can exercise
        // the same failure owner as Store readback without another artwork fake.
        sessions.onWrite = (session, characteristic, bytes) {
          if (characteristic !=
              BleProtocol.connectionSessionIngressCharacteristic) {
            return;
          }
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(bytes),
          )!;
          void reply(
            ConnectionSessionKind kind,
            int code,
            int token,
            int sequence,
            Uint8List body,
          ) {
            session.emit(
              BleProtocol.connectionSessionEgressCharacteristic,
              ConnectionSessionCodec.encode(
                ConnectionSessionPacket(
                  kind: kind,
                  code: code,
                  token: token,
                  sequence: sequence,
                  body: body,
                ),
              ),
            );
          }

          void control(int code, int token, int sequence, Uint8List body) =>
              reply(ConnectionSessionKind.control, code, token, sequence, body);
          void progress(int token, int sequence) {
            final body = Uint8List(10);
            final data = ByteData.sublistView(body);
            data.setUint32(0, sequence, Endian.little);
            if (token == 3) {
              objectAccepted = sequence;
              if (!freezeObjectGrant) {
                objectGrant = sequence + ConnectionSessionCodec.maximumWindow;
              }
            }
            data.setUint32(
              4,
              token == 3 ? objectGrant : sequence + 8,
              Endian.little,
            );
            control(ConnectionSessionControl.progress, token, sequence, body);
          }

          if (packet.kind == ConnectionSessionKind.object) {
            objectWrites.add(packet);
            progress(packet.token, packet.sequence);
            if (packet.code == ConnectionLitchcraftCodec.objectOpcode) {
              final request = assembler.accept(packet.body);
              if (request != null &&
                  request.length >= 2 &&
                  request[0] == LitchcraftProtocol.magic) {
                expect(
                  request[1],
                  isNot(
                    anyOf(LitchcraftProtocol.enter, LitchcraftProtocol.exit),
                  ),
                  reason: 'Compact mode must never fall back to shared Objects',
                );
              }
            }
            return;
          }
          if (packet.kind == ConnectionSessionKind.ordered &&
              packet.code == ConnectionSession.litchcraftModeOpcode) {
            expect(modeFeature, isTrue);
            final request = packet.body;
            expect(request, hasLength(8));
            final operation = request[1];
            operations.add(operation);
            expect(
              operation,
              anyOf(LitchcraftProtocol.enter, LitchcraftProtocol.exit),
            );
            final state = Uint8List(20);
            final data = ByteData.sublistView(state);
            state[0] = LitchcraftProtocol.magic;
            state[1] = LitchcraftProtocol.state;
            data.setUint32(2, 7, Endian.little);
            data.setUint32(6, 1, Endian.little);
            data.setUint16(
              10,
              ByteData.sublistView(request).getUint16(6, Endian.little),
              Endian.little,
            );
            state.setRange(12, 20, <int>[
              operation == LitchcraftProtocol.exit ? 2 : 0,
              LitchcraftProtocol.stateActive,
              1,
              0,
              0,
              0,
              15,
              0,
            ]);
            void terminal() {
              if (operation == LitchcraftProtocol.exit) {
                control(
                  ConnectionSessionControl.outcome,
                  packet.token,
                  packet.sequence,
                  Uint8List.fromList([1, 2, 0]),
                );
              }
              final checkpoint = Uint8List(10);
              ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
              control(
                ConnectionSessionControl.checkpoint,
                packet.token,
                packet.sequence,
                checkpoint,
              );
            }

            // Exercise both real arbitration orders: terminal before Enter's
            // compact ACK, and a compact refusal before Exit's terminal.
            if (operation == LitchcraftProtocol.enter) terminal();
            for (final fragment in ConnectionLitchcraftCodec.encodePacket(
              state,
              maximumBodyBytes: 64,
            )) {
              reply(
                ConnectionSessionKind.object,
                ConnectionLitchcraftCodec.receiptCode,
                reverseToken,
                ++reverseSequence,
                fragment,
              );
            }
            if (operation == LitchcraftProtocol.exit) terminal();
            return;
          }
          if (packet.kind == ConnectionSessionKind.ordered) {
            ++controlActions;
            expect(packet.code, LitchControlProtocol.connectionActionOpcode);
            final body = Uint8List(10);
            ByteData.sublistView(body).setUint32(0, 1, Endian.little);
            control(
              ConnectionSessionControl.checkpoint,
              packet.token,
              packet.sequence,
              body,
            );
            return;
          }
          if (packet.kind != ConnectionSessionKind.control) return;
          switch (packet.code) {
            case ConnectionSessionControl.hello:
              control(ConnectionSessionControl.welcome, 1, 0, packet.body);
              final body = Uint8List(10);
              final data = ByteData.sublistView(body);
              data.setUint64(0, 1, Endian.little);
              data.setUint16(
                8,
                ConnectionSession.orderedFeature |
                    ConnectionSession.stateFeature |
                    ConnectionSession.objectFeature |
                    ConnectionSession.reverseObjectFeature |
                    (modeFeature ? ConnectionSession.orderedModeFeature : 0),
                Endian.little,
              );
              control(ConnectionSessionControl.identity, 1, 0, body);
            case ConnectionSessionControl.ready:
              control(ConnectionSessionControl.activated, 1, 0, packet.body);
              for (final sequence in <int>[1, 2]) {
                final body = Uint8List(8);
                final data = ByteData.sublistView(body);
                data.setUint16(
                  0,
                  sequence == 1
                      ? ConnectionSession.litchcraftStateTopic
                      : ConnectionSession.litchcraftReverseObjectTopic,
                  Endian.little,
                );
                body[2] = 1;
                body[3] = 8;
                data.setUint16(4, sequence == 1 ? 10 : 64, Endian.little);
                control(ConnectionSessionControl.open, 1, sequence, body);
              }
            case ConnectionSessionControl.open:
              final body = Uint8List(10);
              final data = ByteData.sublistView(body);
              data.setUint32(0, packet.sequence == 1 ? 2 : 3, Endian.little);
              data.setUint16(
                4,
                ByteData.sublistView(packet.body).getUint16(4, Endian.little),
                Endian.little,
              );
              body[6] = packet.body[3];
              body[7] = packet.body[2];
              control(
                ConnectionSessionControl.opened,
                1,
                packet.sequence,
                body,
              );
              progress(packet.sequence == 1 ? 2 : 3, 0);
            case ConnectionSessionControl.opened:
              if (packet.sequence == 2) {
                reverseToken = ByteData.sublistView(
                  packet.body,
                ).getUint32(0, Endian.little);
              }
            case ConnectionSessionControl.confirm:
              progress(packet.token, packet.sequence);
          }
        };

        final connecting = service.connect('board-1');
        await _waitFor(() => ble.connections.isNotEmpty);
        ble.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        service.debugSetDeviceCapabilities(
          'board-1',
          const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask: LitchDeviceCapabilities.litchcraftBit,
            explicitlyReported: true,
          ),
        );
        final session = (await service.openConnectionSession('board-1'))!;
        final connectionToken = service.connectionSessionToken;
        final cancellations = ble.connectionCancellationCount;
        if (modeFeature) {
          // Close genuine Object credit using this existing activated SDK peer.
          // The actual V2 adapter must retire its queued Inspect without
          // retiring the shared Session or affecting another domain owner.
          await Future<void>.delayed(Duration.zero);
          freezeObjectGrant = true;
          final remaining = objectGrant - objectAccepted;
          if (remaining > 0) {
            await session.sendObjectBatch([
              for (var index = 0; index < remaining; ++index)
                (opcode: 0x7e, body: Uint8List.fromList([index])),
            ]);
          }
          expect(objectAccepted, objectGrant);
          final issuedBeforeInspect = objectWrites.length;
          late final LitchcraftLink inspectionLink;
          inspectionLink = LitchcraftLink(
            packets: service.litchcraftPackets('board-1'),
            onPacket: (_) {},
            onFailure: (_) {},
            write: (bytes) {
              final current = inspectionLink.v2WriteValidity(bytes);
              return service.writeLitchcraftV2Packet(
                'board-1',
                bytes,
                connectionSessionToken: connectionToken,
                isCurrent: current,
                cancellationFuture: inspectionLink.v2WriteRetirement(bytes),
              );
            },
          );
          final inspecting = inspectionLink.inspectV2();
          final cancelled = expectLater(
            inspecting,
            throwsA(isA<LitchcraftTransferCancelled>()),
          );
          await Future<void>.delayed(Duration.zero);
          expect(objectWrites, hasLength(issuedBeforeInspect));
          await inspectionLink.dispose();
          await cancelled;
          expect(session.isActivated, isTrue);
          expect(objectWrites, hasLength(issuedBeforeInspect));
          freezeObjectGrant = false;
          final reopened = Uint8List(10);
          ByteData.sublistView(reopened)
            ..setUint32(0, objectAccepted, Endian.little)
            ..setUint32(
              4,
              objectAccepted + ConnectionSessionCodec.maximumWindow,
              Endian.little,
            );
          sessions.sessions.single.emit(
            BleProtocol.connectionSessionEgressCharacteristic,
            ConnectionSessionCodec.encode(
              ConnectionSessionPacket(
                kind: ConnectionSessionKind.control,
                code: ConnectionSessionControl.progress,
                token: 3,
                sequence: objectAccepted,
                body: reopened,
              ),
            ),
          );
          objectGrant = objectAccepted + ConnectionSessionCodec.maximumWindow;
          await Future<void>.delayed(Duration.zero);
          expect(
            objectWrites,
            hasLength(issuedBeforeInspect),
            reason: 'Reopened credit cannot resurrect the retired read',
          );
        }
        final firstPacket = Completer<void>();
        final borrowedPackets = service.litchcraftPackets('board-1').listen((
          _,
        ) {
          if (!firstPacket.isCompleted) firstPacket.complete();
        });
        final entering = owner.enter(draftReady: draftReady.future);
        if (!modeFeature) {
          await entering;
          expect(owner.status, LitchcraftStatus.unsupported);
          expect(operations, isEmpty);
          expect(session.isActivated, isTrue);
          await borrowedPackets.cancel();
          final result = await service.dispatchControlAction(
            deviceId: 'board-1',
            action: LitchControlAction.newGame,
          );
          expect(result.phase, LitchControlPhase.applied);
          expect(service.connectionSessionToken, connectionToken);
          return;
        }
        await _waitFor(() => owner.boardActive);
        await firstPacket.future;
        // A domain must be able to leave while the board is quiet. Waiting for
        // another shared receipt here stalls cleanup and eventually disconnects
        // the healthy BLE owner during the controller's next recovery attempt.
        await borrowedPackets.cancel().timeout(const Duration(seconds: 1));
        expect(session.isActivated, isTrue);
        final reboundPackets = <List<int>>[];
        final rebound = service
            .litchcraftPackets('board-1')
            .listen(reboundPackets.add);
        await Future<void>.delayed(Duration.zero);
        await expectLater(owner.leave(), throwsA(isA<StateError>()));
        await _waitFor(() => reboundPackets.isNotEmpty);
        expect(reboundPackets, hasLength(1));
        await rebound.cancel().timeout(const Duration(seconds: 1));
        await entering;
        expect(operations, <int>[
          LitchcraftProtocol.enter,
          LitchcraftProtocol.exit,
        ]);
        expect(session.isActivated, isTrue);

        // This call used to reopen BLE after the domain disposed its borrowed
        // session. Another domain must instead retain the same native generation.
        expect(await service.openConnectionSession('board-1'), same(session));
        final result = await service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.newGame,
        );
        expect(result.phase, LitchControlPhase.applied);
        expect(controlActions, 1);
        expect(service.isConnected, isTrue);
        expect(service.connectionSessionToken, connectionToken);
        expect(ble.connections, hasLength(1));
        expect(ble.connectionCancellationCount, cancellations);
        expect(sessions.openAttempts, 1);
      },
    );
  }

  test(
    'Ordered archive refusal keeps the game and waits for another user New game',
    () async {
      final peer = _RecoverySharedPeer();
      final service = peer.service;
      addTearDown(peer.close);
      service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.gameHistoryBit |
              LitchDeviceCapabilities.settingsBit,
          explicitlyReported: true,
        ),
      );
      const fen = 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';
      const start = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
      void publish({bool newGame = false}) {
        final bytes = peer.fixture('home_state_wire_v1.csv', 'representative');
        bytes[1] = HomeStateAction.idle.index;
        bytes[6] = 0x40; // Clock present and disabled.
        bytes[7] = 0;
        bytes[8] = newGame ? 0 : 0x14; // The latest move was White's e2–e4.
        bytes[9] = newGame ? 0 : 0x10;
        bytes[10] = 0;
        bytes.setAll(
          11,
          GameHistoryCodec.encodePosition(
            ArchivedPosition(ply: newGame ? 0 : 1, fen: newGame ? start : fen),
            GameVariant.classic,
          ),
        );
        final occupancy = ByteData.sublistView(bytes);
        occupancy.setUint32(54, 0x0000ffff, Endian.little);
        occupancy.setUint32(
          58,
          newGame ? 0xffff0000 : 0xffef0010,
          Endian.little,
        );
        bytes.fillRange(62, 71, 0xff);
        if (!newGame) {
          bytes[67] = 52;
          bytes[68] = 36;
        }
        bytes.fillRange(71, bytes.length, 0);
        peer.stateFragments(bytes, 4, 10, peer.settingsIdentity(bytes));
      }

      peer.onStateQuery = () {
        publish(newGame: peer.actions >= 2);
        peer.settings(revision: 1);
      };
      peer.onOrdered = () {
        final request = ConnectionSessionCodec.decode(
          Uint8List.fromList(peer.native!.writeRecords.last.value),
        )!;
        expect(request.kind, ConnectionSessionKind.ordered);
        expect(request.code, LitchControlProtocol.connectionActionOpcode);
        expect(request.body, [1, 3, 0, 0, 1]);
        if (peer.actions == 1) {
          peer.control(
            ConnectionSessionControl.outcome,
            request.token,
            request.sequence,
            Uint8List.fromList([
              1,
              LitchMutationReason.archiveRequired.wireCode,
              0,
            ]),
          );
          final checkpoint = Uint8List(10)
            ..[0] = 1
            ..[8] = 1;
          peer.terminal = request.sequence;
          peer.control(
            ConnectionSessionControl.checkpoint,
            request.token,
            request.sequence,
            checkpoint,
          );
          peer.progress();
        } else {
          peer.finish(revision: 2);
          publish(newGame: true);
        }
      };
      await peer.open();
      await _waitFor(() => service.latestHomeState?.position.fen == fen);
      final controller = BoardActionsController(
        bleService: service,
        boardId: 'board-1',
      );
      final feed = HomeBoardFeed(bleService: service, boardId: 'board-1');
      addTearDown(controller.dispose);
      addTearDown(feed.dispose);
      await _waitFor(() => feed.current.position?.fen == fen);
      final events = <LitchControlEvent>[];
      final subscription = service.controlActionEvents.listen(events.add);
      addTearDown(subscription.cancel);

      await controller.dispatch(LitchControlAction.newGame);
      await _waitFor(() => events.lastOrNull?.isTerminal == true);
      expect(events.last.phase, LitchControlPhase.rejected);
      expect(events.last.reason, LitchMutationReason.archiveRequired);
      expect(
        controller.statusMessage,
        'Keep Litch connected while this game is saved, then try New game again.',
      );
      expect(controller.hasPendingAction, isFalse);
      publish();
      await Future<void>.delayed(const Duration(seconds: 2));
      expect(peer.actions, 1);
      expect(events.map((event) => event.phase), [
        LitchControlPhase.pending,
        LitchControlPhase.rejected,
      ]);
      expect(feed.current.position?.fen, fen);
      expect(
        feed.current.physicalStatus?.occupiedSquares,
        BigInt.parse('ffef00100000ffff', radix: 16),
      );
      expect(feed.current.gameStatus?.outcome, 'none');
      expect(
        service.controlActionAvailability('board-1'),
        LitchControlAvailability.available,
      );

      await controller.dispatch(LitchControlAction.newGame);
      await _waitFor(
        () =>
            events.last.phase == LitchControlPhase.applied &&
            feed.current.position?.fen == start,
      );
      expect(peer.actions, 2);
      expect(
        controller.statusMessage,
        'New game started. Follow any setup guidance on the board.',
      );
      expect(
        peer.native!.writes,
        isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
      );
    },
  );

  test(
    'ready iOS native batches do not capture extra connection journals',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final ble = _JournalCountingBle();
      final peer =
          _RecoverySharedPeer(
              connected: false,
              nativeBle: ble,
              appTrace: _MemoryTrace(),
            )
            ..infoPayload = jsonEncode({
              'i': [3, 4, LitchDeviceCapabilities.gameHistoryBit],
            });
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      addTearDown(() async {
        await _disposeReconnectHarness(peer.service, ble);
        await peer.sessions.close();
        debugDefaultTargetPlatformOverride = null;
      });
      final connecting = peer.service.connect('board-1');
      await _waitFor(() => ble.connections.isNotEmpty);
      ble.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
      expect(await peer.service.ensureProductReady('board-1'), isTrue);
      final shared = await peer.service.openConnectionSession('board-1');
      expect(shared, isNotNull);
      expect(shared!.isActivated, isTrue);
      expect(peer.service.productReady, isTrue);
      await _waitFor(() => ble.diagnosticRequests.isNotEmpty);
      final initialJournals = ble.diagnosticRequests.length;
      expect(
        initialJournals,
        greaterThanOrEqualTo(1),
        reason:
            'The real connect/ready path must exercise the journal provider first.',
      );
      final native = peer.sessions.sessions.single;
      final initialBatches = native.batchWrites.length;
      final journalsAfterBatches = <int>[];
      for (var batch = 0; batch < 2; batch++) {
        final packets = List<Uint8List>.generate(
          2,
          (index) => ConnectionSessionCodec.encode(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.object,
              code: 40,
              token: 3,
              sequence: batch * 2 + index + 1,
              body: Uint8List.fromList([batch, index]),
            ),
          ),
        );
        await peer.service.writeConnectionSessionPackets('board-1', packets);
        expect(native.batchWrites, hasLength(initialBatches + batch + 1));
        expect(
          native.batchWrites.last.characteristicId,
          BleProtocol.connectionSessionIngressCharacteristic,
        );
        expect(native.batchWrites.last.values, packets);
        await Future<void>.delayed(Duration.zero);
        journalsAfterBatches.add(ble.diagnosticRequests.length);
      }
      expect(
        native.batchWrites.length - initialBatches,
        greaterThanOrEqualTo(2),
      );
      expect(
        journalsAfterBatches,
        everyElement(initialJournals),
        reason:
            'Ordinary byte batches must not take another native journal snapshot.',
      );
      expect(peer.service.productReady, isTrue);
    },
  );

  test(
    'iOS uses the native Object batch instead of serial packet writes',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final _ReconnectBle fakeBle = _ReconnectBle();
      final _RecordingGattSessionFactory sessions =
          _RecordingGattSessionFactory();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        gattSessionFactory: sessions,
      );
      addTearDown(() async {
        await service.dispose();
        await sessions.close();
        debugDefaultTargetPlatformOverride = null;
      });

      service.debugSetConnectedDevice('board-1');
      await _waitFor(() => sessions.sessions.isNotEmpty);
      await service.writeConnectionSessionPackets(
        'board-1',
        List<Uint8List>.generate(
          ConnectionSessionCodec.maximumWindow,
          (_) => Uint8List(ConnectionSessionCodec.headerBytes + 1),
          growable: false,
        ),
      );

      expect(
        await service.litchcraftPacketPayloadBytes(
          'board-1',
          connectionSessionToken: service.connectionSessionToken,
        ),
        247,
        reason: 'Apple reports a native value limit, not ATT MTU',
      );
      final _RecordingGattSession session = sessions.sessions.single;
      expect(session.batchWrites, hasLength(1));
      expect(
        session.batchWrites.single.values,
        hasLength(ConnectionSessionCodec.maximumWindow),
      );
      expect(session.writeRecords, isEmpty);
    },
  );

  test(
    'same-board reconnect renews one exact GATT session without UUID-only operations',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final peer = _RecoverySharedPeer(connected: false)
        ..infoPayload = '{"i":[3,3,31]}';
      addTearDown(() async {
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      });
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      peer.onOrdered = peer.finish;
      Future<void> connectAt(int index) async {
        final connecting = peer.service.connect('board-1');
        await _waitFor(() => peer.ble.connections.length > index);
        peer.ble.connections[index].add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        await peer.open();
        expect(
          (await peer.service.dispatchControlAction(
            deviceId: 'board-1',
            action: LitchControlAction.hint,
          )).phase,
          LitchControlPhase.applied,
        );
      }

      await connectAt(0);
      final old = peer.sessions.sessions.single;
      expect(
        old.subscriptions,
        contains(BleProtocol.connectionSessionEgressCharacteristic),
      );
      expect(
        old.writes,
        contains(BleProtocol.connectionSessionIngressCharacteristic),
      );
      peer.ble.connections[0].add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await _waitFor(() => !peer.service.isConnected);
      peer.actions = 0;
      peer.terminal = 0;
      peer.retained = false;
      await connectAt(1);
      expect(peer.sessions.openAttempts, 2);
      expect(peer.sessions.sessions, hasLength(2));
      expect(peer.sessions.sessions.last, isNot(same(old)));
      expect(peer.ble.qualifiedReadAttempts, 0);
      expect(peer.ble.qualifiedSubscribeAttempts, 0);
      expect(peer.ble.qualifiedWriteAttempts, 0);
    },
  );

  test(
    'GATT discovery failure is contained by connection-owned operations',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final _ReconnectBle fakeBle = _ReconnectBle();
      final _RecordingGattSessionFactory sessions =
          _RecordingGattSessionFactory()..failOpen = true;
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        gattSessionFactory: sessions,
      );
      addTearDown(() async {
        await _disposeReconnectHarness(service, fakeBle);
        await sessions.close();
        debugDefaultTargetPlatformOverride = null;
      });

      final Future<bool> connecting = service.connect('board-1');
      await _waitFor(() => fakeBle.connections.isNotEmpty);
      fakeBle.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );

      expect(await connecting, isTrue);
      await Future<void>.delayed(Duration.zero);
      await Future<void>.delayed(Duration.zero);
      expect(sessions.openAttempts, 1);
      expect(fakeBle.qualifiedReadAttempts, 0);
      expect(fakeBle.qualifiedSubscribeAttempts, 0);
      expect(fakeBle.qualifiedWriteAttempts, 0);
    },
  );

  test(
    'same-board reconnect isolates settings replies and enumeration state',
    () async {
      final peer = _RecoverySharedPeer(connected: false)
        ..infoPayload = '{"i":[3,3,31]}';
      addTearDown(peer.close);
      final reads = <Completer<List<int>>>[];
      peer.sessions.onRead = (characteristic) {
        if (characteristic.characteristicId !=
            BleProtocol.settingsEnumerateCharacteristic) {
          return Future.value(utf8.encode('[]'));
        }
        final pending = Completer<List<int>>();
        reads.add(pending);
        return pending.future;
      };
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 1, brightness: 5);
      };
      Future<void> connectAt(int index) async {
        final connecting = peer.service.connect('board-1');
        await _waitFor(() => peer.ble.connections.length > index);
        peer.ble.connections[index].add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        await peer.open();
      }

      await connectAt(0);
      final old = peer.native!;
      final oldRead = peer.service.readSettings('board-1');
      await _waitFor(() => reads.isNotEmpty);
      final oldAction = peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.hint,
      );
      await _waitFor(() => peer.actions == 1);
      peer.ble.connections[0].add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      expect((await oldAction).phase, LitchControlPhase.interrupted);
      await _waitFor(() => !peer.service.isConnected);
      peer.actions = 0;
      peer.terminal = 0;
      peer.retained = false;
      await connectAt(1);
      final newRead = peer.service.readSettings('board-1');
      await _waitFor(() => reads.length == 2);
      reads[1].complete(
        utf8.encode(
          jsonEncode([
            {'i': 7, 'k': 'NEW_SESSION_SETTING', 't': 'u32', 'v': 5},
          ]),
        ),
      );
      expect(
        (await newRead).map((v) => v.key),
        contains('NEW_SESSION_SETTING'),
      );
      final newAction = peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.hint,
      );
      await _waitFor(() => peer.actions == 1);
      bool done = false;
      unawaited(newAction.then((_) => done = true));
      final checkpoint = Uint8List(10);
      ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
      old.emit(
        BleProtocol.connectionSessionEgressCharacteristic,
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.control,
            code: ConnectionSessionControl.checkpoint,
            token: 2,
            sequence: 1,
            body: checkpoint,
          ),
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(done, isFalse);
      peer.finish();
      expect((await newAction).phase, LitchControlPhase.applied);
      reads[0].complete(
        utf8.encode(
          jsonEncode([
            {'i': 7, 'k': 'OLD_SESSION_SETTING', 't': 'u32', 'v': 4},
          ]),
        ),
      );
      expect(
        await oldRead,
        isEmpty,
        reason: 'retired metadata cannot become current',
      );
      expect(
        peer.service.latestSettingValue('BRIGHTNESS'),
        5,
        reason: 'retired enumeration cannot replace current typed authority',
      );
      expect(peer.sessions.openAttempts, 2);
    },
  );

  test(
    'direct board replacement renews every connection-owned bridge',
    () async {
      final peer = _RecoverySharedPeer(connected: false)
        ..infoPayload = '{"i":[3,3,3]}';
      final service = peer.service;
      final statuses = <String>[];
      final statusSub = service.otaStatusStream.listen(statuses.add);
      final metadata = <String, Completer<List<int>>>{};
      peer.sessions.onRead = (characteristic) {
        if (characteristic.characteristicId !=
            BleProtocol.settingsEnumerateCharacteristic) {
          return Future.value(utf8.encode('[]'));
        }
        final read = Completer<List<int>>();
        metadata[characteristic.deviceId] = read;
        return read.future;
      };
      peer.onStateQuery = () {
        final first = peer.native!.deviceId == 'board-A';
        peer.home(
          revision: first ? 20 : 1,
          action: first ? HomeStateAction.lift : HomeStateAction.idle,
        );
        peer.settings(revision: first ? 20 : 1, brightness: first ? 3 : 5);
      };

      Future<ConnectionSession> connectBoard(String id) async {
        final index = peer.ble.connections.length;
        final connecting = service.connect(id);
        await _waitFor(() => peer.ble.connections.length == index + 1);
        peer.ble.connections[index].add(
          ConnectionStateUpdate(
            deviceId: id,
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        expect(await service.ensureProductReady(id), isTrue);
        final current = (await service.openConnectionSession(id))!;
        await _waitFor(
          () => peer.native!.subscriptions.contains(
            BleProtocol.consoleStreamCharacteristic,
          ),
        );
        return current;
      }

      void emitVersion(
        _RecordingGattSession native,
        int token,
        String version,
      ) {
        var sequence = 0;
        for (final body in ConnectionOtaCodec.encodeControl(
          Uint8List.fromList(utf8.encode(jsonEncode({'v': version}))),
          maximumBodyBytes: 10,
        )) {
          native.emit(
            BleProtocol.connectionSessionEgressCharacteristic,
            ConnectionSessionCodec.encode(
              ConnectionSessionPacket(
                kind: ConnectionSessionKind.object,
                code: ConnectionSession.otaStatusReceiptCode,
                token: token,
                sequence: ++sequence,
                body: body,
              ),
            ),
          );
        }
      }

      try {
        final a = await connectBoard('board-A');
        final old = peer.native!;
        final oldReverse = a.reverseObjectStreamToken;
        final oldMetadata = service.readSettings('board-A');
        await _waitFor(() => metadata.containsKey('board-A'));
        await _waitFor(() => service.latestSettingValue('BRIGHTNESS') == 3);
        expect(service.latestHomeState?.action, HomeStateAction.lift);
        // Request B while A remains connected and its SDK read is outstanding.
        final b = await connectBoard('board-B');
        final current = peer.native!;
        expect(service.connectedId, 'board-B');
        expect(b, isNot(same(a)));
        expect(peer.sessions.sessions.map((session) => session.deviceId), [
          'board-A',
          'board-B',
        ]);
        for (final handle in [old, current]) {
          for (final endpoint in [
            BleProtocol.consoleStreamCharacteristic,
            BleProtocol.connectionSessionEgressCharacteristic,
            BleProtocol.otaControlCharacteristic,
          ]) {
            expect(
              handle.subscriptions.where((id) => id == endpoint),
              hasLength(1),
            );
          }
        }
        final newMetadata = service.readSettings('board-B');
        await _waitFor(() => metadata.containsKey('board-B'));
        List<int> descriptor(String key) => utf8.encode(
          jsonEncode([
            {'i': 7, 'k': key, 't': 'u32', 'v': 8},
          ]),
        );
        metadata['board-B']!.complete(descriptor('B_ONLY'));
        expect((await newMetadata).map((item) => item.key), contains('B_ONLY'));
        metadata['board-A']!.complete(descriptor('A_ONLY'));
        expect(await oldMetadata, isEmpty);

        old.emit(
          BleProtocol.consoleStreamCharacteristic,
          utf8.encode('OLD-A\n'),
        );
        emitVersion(old, oldReverse, 'old-A');
        peer.native = old;
        peer.home(revision: 21, action: HomeStateAction.lift);
        peer.settings(revision: 21, brightness: 9);
        peer.native = current;
        await Future<void>.delayed(Duration.zero);
        expect(
          service.consoleLinesSnapshot.where((line) => line.contains('OLD-A')),
          isEmpty,
        );
        expect(statuses.where((line) => line.contains('old-A')), isEmpty);
        expect(service.latestHomeState?.action, HomeStateAction.idle);
        expect(service.latestSettingValue('BRIGHTNESS'), 5);

        current.emit(
          BleProtocol.consoleStreamCharacteristic,
          utf8.encode('BOARD-B\n'),
        );
        emitVersion(current, b.reverseObjectStreamToken, 'new-B');
        peer.home(revision: 2, action: HomeStateAction.lift);
        peer.settings(revision: 2, brightness: 6);
        await _waitFor(() => statuses.any((line) => line.contains('new-B')));
        expect(
          service.consoleLinesSnapshot.where(
            (line) => line.endsWith('BOARD-B'),
          ),
          hasLength(1),
        );
        expect(service.latestHomeState?.action, HomeStateAction.lift);
        expect(service.latestSettingValue('BRIGHTNESS'), 6);

        final action = service.dispatchControlAction(
          deviceId: 'board-B',
          action: LitchControlAction.hint,
        );
        await _waitFor(() => peer.actions == 1);
        expect(
          _sharedPackets(
            old,
          ).where((packet) => packet.kind == ConnectionSessionKind.ordered),
          isEmpty,
        );
        peer.finish();
        expect((await action).phase, LitchControlPhase.applied);
      } finally {
        for (final read in metadata.values) {
          if (!read.isCompleted) read.complete(utf8.encode('[]'));
        }
        await statusSub.cancel();
        await _disposeReconnectHarness(service, peer.ble);
        await peer.sessions.close();
      }
    },
  );

  test(
    'OTA peer rejects unsupported Ordered without a false Applied receipt',
    () async {
      final peer = ota_board.RebootReplyBle()..mtu = 23;
      final replies = <ConnectionSessionPacket>[];
      final subscription = peer.sessionPackets.stream.listen((bytes) {
        replies.add(ConnectionSessionCodec.decode(Uint8List.fromList(bytes))!);
      });
      final confirmEntered = Completer<void>();
      final confirmRelease = Completer<void>();
      Uint8List? probe;
      final session = ConnectionSession(
        packets: peer.sessionPackets.stream,
        clientNonce: 42,
        send: (bytes) async {
          final packet = ConnectionSessionCodec.decode(bytes)!;
          if (packet.kind == ConnectionSessionKind.ordered) probe = bytes;
          if (packet.kind == ConnectionSessionKind.control &&
              packet.code == ConnectionSessionControl.confirm) {
            if (!confirmEntered.isCompleted) confirmEntered.complete();
            await confirmRelease.future;
          }
          await peer.sharedWrite(bytes);
        },
      );
      addTearDown(() async {
        if (!confirmRelease.isCompleted) confirmRelease.complete();
        await session.dispose();
        await subscription.cancel();
        await peer.sessionPackets.close();
        await peer.packets.close();
      });
      await session.activate();
      final outcome = await session.sendOrdered(
        28,
        Uint8List.fromList([1, 0, 0, 0, 0, 0, 0, 0]),
      );
      expect(outcome.disposition, ConnectionSessionDisposition.rejected);
      expect(outcome.reason, 4, reason: 'native Unsupported domain outcome');
      await confirmEntered.future;
      replies.clear();
      await peer.sharedWrite(probe!);
      expect(replies, hasLength(1));
      expect(replies.single.code, ConnectionSessionControl.progress);
      replies.clear();
      await peer.sharedWrite(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.control,
            code: ConnectionSessionControl.query,
            token: 2,
            sequence: 0,
            body: Uint8List(0),
          ),
        ),
      );
      await peer.sharedWrite(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.control,
            code: ConnectionSessionControl.query,
            token: 2,
            sequence: 1,
            body: Uint8List(0),
          ),
        ),
      );
      final replay = replies.singleWhere(
        (packet) => packet.code == ConnectionSessionControl.outcome,
      );
      expect(replay.body, [1, 4, 0]);
      final checkpoint = replies.singleWhere(
        (packet) => packet.code == ConnectionSessionControl.checkpoint,
      );
      expect(
        ByteData.sublistView(checkpoint.body).getUint16(8, Endian.little),
        1,
      );
      final progress = replies.firstWhere(
        (packet) => packet.code == ConnectionSessionControl.progress,
      );
      expect(
        ByteData.sublistView(progress.body).getUint32(4, Endian.little),
        8,
      );
      confirmRelease.complete();
      for (var turn = 0; turn < 20 && session.grantThrough != 9; ++turn) {
        await Future<void>.delayed(Duration.zero);
      }
      expect(session.grantThrough, 9);
      replies.clear();
      await peer.sharedWrite(probe!);
      expect(
        replies.where(
          (packet) => packet.code == ConnectionSessionControl.outcome,
        ),
        isEmpty,
        reason: 'Confirm frees the retained exceptional outcome',
      );
      final next = await session.sendOrdered(29, Uint8List(0));
      expect(next.disposition, ConnectionSessionDisposition.rejected);
      expect(next.reason, 4);
      expect(session.isActivated, isTrue);
    },
  );

  test(
    'OTA peer retains only its granted eight Unsupported receipts',
    () async {
      final peer = ota_board.RebootReplyBle();
      final replies = <ConnectionSessionPacket>[];
      final subscription = peer.sessionPackets.stream.listen((bytes) {
        replies.add(ConnectionSessionCodec.decode(Uint8List.fromList(bytes))!);
      });
      addTearDown(() async {
        await subscription.cancel();
        await peer.sessionPackets.close();
        await peer.packets.close();
      });
      Future<void> write(
        ConnectionSessionKind kind,
        int code,
        int sequence, [
        Uint8List? body,
      ]) => peer.sharedWrite(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: kind,
            code: code,
            token:
                kind == ConnectionSessionKind.control &&
                    code == ConnectionSessionControl.open
                ? 1
                : 2,
            sequence: sequence,
            body: body ?? Uint8List(0),
          ),
        ),
      );
      final open = Uint8List(8);
      ByteData.sublistView(open)
        ..setUint16(0, ConnectionSession.litchcraftOrderedTopic, Endian.little)
        ..setUint16(4, 10, Endian.little);
      open[3] = 8;
      await write(
        ConnectionSessionKind.control,
        ConnectionSessionControl.open,
        1,
        open,
      );
      replies.clear();
      for (var sequence = 1; sequence <= 8; ++sequence) {
        await write(ConnectionSessionKind.ordered, 28, sequence);
      }
      expect(
        replies.where(
          (packet) => packet.code == ConnectionSessionControl.outcome,
        ),
        hasLength(8),
      );
      final checkpoint = replies.lastWhere(
        (packet) => packet.code == ConnectionSessionControl.checkpoint,
      );
      expect(checkpoint.sequence, 8);
      expect(
        ByteData.sublistView(checkpoint.body).getUint16(8, Endian.little),
        255,
      );
      replies.clear();
      await write(ConnectionSessionKind.ordered, 28, 9);
      await write(
        ConnectionSessionKind.control,
        ConnectionSessionControl.confirm,
        9,
      );
      expect(
        replies,
        hasLength(1),
        reason: 'overflow and invalid Confirm cannot advance the grant',
      );
      expect(
        ByteData.sublistView(replies.single.body).getUint32(4, Endian.little),
        8,
      );
      replies.clear();
      await write(
        ConnectionSessionKind.control,
        ConnectionSessionControl.query,
        0,
      );
      expect(
        replies.where(
          (packet) => packet.code == ConnectionSessionControl.outcome,
        ),
        isEmpty,
      );
      expect(replies.last.code, ConnectionSessionControl.checkpoint);
      for (var sequence = 1; sequence <= 8; ++sequence) {
        await write(
          ConnectionSessionKind.control,
          ConnectionSessionControl.query,
          sequence,
        );
      }
      expect(
        replies.where(
          (packet) => packet.code == ConnectionSessionControl.outcome,
        ),
        hasLength(8),
      );
      expect(
        replies
            .where((packet) => packet.code == ConnectionSessionControl.outcome)
            .map((packet) => packet.body),
        everyElement([1, 4, 0]),
      );
      await write(
        ConnectionSessionKind.control,
        ConnectionSessionControl.confirm,
        8,
      );
      replies.clear();
      await write(
        ConnectionSessionKind.control,
        ConnectionSessionControl.query,
        1,
      );
      expect(replies, hasLength(1));
      expect(
        ByteData.sublistView(replies.single.body).getUint32(4, Endian.little),
        16,
      );
      replies.clear();
      await write(ConnectionSessionKind.ordered, 29, 9);
      expect(
        replies
            .singleWhere(
              (packet) => packet.code == ConnectionSessionControl.outcome,
            )
            .body,
        [1, 4, 0],
      );
    },
  );

  test(
    'minimum MTU keeps every chunk ATT-safe for an image over one megabyte',
    () async {
      final bytes = makeEsp32s3Firmware(
        payloadBytes: List<int>.filled(1000001, 0x5a),
      );
      final imageBytes = bytes.length;
      final manifest = makeFirmwareManifest(bytes, version: '0.8');
      expect(imageBytes, greaterThan(1000000));
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-min-mtu-',
      );
      addTearDown(() => temp.delete(recursive: true));
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(bytes);
      final fakeBle = ota_board.RebootReplyBle()
        ..mtu = 23
        ..receiveAckInterval = 400
        ..beginReply = jsonEncode({
          'st': 'begin',
          'proto': 2,
          'rx': 0,
          'total': imageBytes,
          'chunk': 10,
          'window': 480,
          'burst': 12,
        });
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() => ota_board.disposeBoard(service, fakeBle));
      await ota_board.connectBoard(service, fakeBle);

      expect(
        await service.uploadFirmwareFile(
          deviceId: 'board-1',
          firmwareFile: firmware,
          manifest: manifest,
        ),
        isA<OtaUploadFinalized>(),
      );
      expect(fakeBle.received, imageBytes);
      expect(fakeBle.imageChunkLengths, isNotEmpty);
      expect(
        fakeBle.imageChunkLengths.last,
        imageBytes % 10 == 0 ? 10 : imageBytes % 10,
      );
      expect(
        fakeBle.nativeWriteLengths,
        everyElement(lessThanOrEqualTo(20)),
        reason: fakeBle.firstOversizeNativeWrite,
      );
      expect(fakeBle.maximumUnacknowledgedBytes, lessThanOrEqualTo(480));
      expect(fakeBle.lastRxAcknowledgement, imageBytes);
      expect(service.pendingOtaVerification!.attempt!.sourceSequence, 23);
      expect(service.pendingOtaVerification!.attempt!.expectedSequence, 24);
      expect(fakeBle.uploadCommands, ['ota_begin', 'ota_end']);
    },
    // Minimum MTU requires over 100,000 real async reads and shared-session handoffs.
    timeout: const Timeout(Duration(seconds: 60)),
  );

  for (final reason in [
    LitchMutationReason.unknownAction,
    LitchMutationReason.unsupported,
  ]) {
    test(
      '${reason.wireToken} marks controls unavailable for the session',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        peer.onOrdered = () {
          peer.control(
            ConnectionSessionControl.outcome,
            2,
            peer.actions,
            Uint8List.fromList([1, reason.wireCode, 0]),
          );
          final checkpoint = Uint8List(10);
          ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
          checkpoint[8] = 1;
          peer.control(
            ConnectionSessionControl.checkpoint,
            2,
            peer.actions,
            checkpoint,
          );
        };
        await peer.open();
        final terminal = await peer.service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.hint,
        );
        expect(terminal.phase, LitchControlPhase.rejected);
        expect(terminal.reason, reason);
        expect(
          peer.service.controlActionAvailability('board-1'),
          LitchControlAvailability.unavailable,
        );
        final unavailable = await peer.service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.hint,
        );
        expect(unavailable.phase, LitchControlPhase.unavailable);
        expect(peer.actions, 1);
      },
    );
  }

  test('transient Ordered rejection does not disable later action', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    peer.onOrdered = () {
      if (peer.actions > 1) {
        peer.finish();
        return;
      }
      peer.control(
        ConnectionSessionControl.outcome,
        2,
        peer.actions,
        Uint8List.fromList([1, LitchMutationReason.queueFull.wireCode, 0]),
      );
      final checkpoint = Uint8List(10);
      ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
      checkpoint[8] = 1;
      peer.control(
        ConnectionSessionControl.checkpoint,
        2,
        peer.actions,
        checkpoint,
      );
    };
    await peer.open();
    final first = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(first.phase, LitchControlPhase.rejected);
    expect(first.reason, LitchMutationReason.queueFull);
    expect(
      peer.service.controlActionAvailability('board-1'),
      LitchControlAvailability.available,
    );
    final second = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(second.phase, LitchControlPhase.applied);
    expect(peer.actions, 2);
  });

  test('late applied action cannot clear unavailable latch', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    await peer.open();
    final first = peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    final second = peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.undo,
    );
    await _waitFor(() => peer.actions == 2);
    peer.control(
      ConnectionSessionControl.outcome,
      2,
      1,
      Uint8List.fromList([1, LitchMutationReason.unsupported.wireCode, 0]),
    );
    final checkpoint = Uint8List(10);
    ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
    checkpoint[8] = 1;
    peer.control(ConnectionSessionControl.checkpoint, 2, 1, checkpoint);
    expect((await first).phase, LitchControlPhase.rejected);
    peer.finish();
    expect((await second).phase, LitchControlPhase.applied);
    expect(
      peer.service.controlActionAvailability('board-1'),
      LitchControlAvailability.unavailable,
    );
    expect(
      (await peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.hint,
      )).phase,
      LitchControlPhase.unavailable,
    );
    expect(peer.actions, 2);
  });

  test(
    'a fresh attachment permits control after an unavailable result',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      peer.onOrdered = () {
        peer.control(
          ConnectionSessionControl.outcome,
          2,
          peer.actions,
          Uint8List.fromList([1, LitchMutationReason.unsupported.wireCode, 0]),
        );
        final checkpoint = Uint8List(10);
        ByteData.sublistView(checkpoint).setUint32(0, 1, Endian.little);
        checkpoint[8] = 1;
        peer.control(
          ConnectionSessionControl.checkpoint,
          2,
          peer.actions,
          checkpoint,
        );
      };
      await peer.open();
      expect(
        (await peer.service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.hint,
        )).phase,
        LitchControlPhase.rejected,
      );
      expect(
        peer.service.controlActionAvailability('board-1'),
        LitchControlAvailability.unavailable,
      );
      await peer.service.disconnect();
      peer.service.debugSetConnectedDevice('board-1');
      peer.service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask: LitchDeviceCapabilities.settingsBit,
          explicitlyReported: true,
        ),
      );
      peer.actions = 0;
      peer.terminal = 0;
      peer.retained = false;
      peer.onOrdered = peer.finish;
      await peer.open();
      expect(
        (await peer.service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.hint,
        )).phase,
        LitchControlPhase.applied,
      );
      expect(peer.actions, 1);
    },
  );

  // Raw request IDs and same-session retry retired with the direct route.
  // The fixed received-head deadline, generation interruption and fresh
  // attachment cases above cover timeout and recovery on the shared session.

  test(
    'control intent stays pending while discovery is held and sends once after release',
    () async {
      final peer = _RecoverySharedPeer(connected: false);
      addTearDown(peer.close);
      peer.sessions.holdOpen = Completer<void>();
      peer.service.debugSetConnectedDevice('board-1');
      peer.service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask: LitchDeviceCapabilities.settingsBit,
          explicitlyReported: true,
        ),
      );
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      peer.onOrdered = peer.finish;
      final owner = BoardActionsController(
        bleService: peer.service,
        boardId: 'board-1',
      );
      addTearDown(owner.dispose);
      await owner.dispatch(LitchControlAction.newGame);
      await Future<void>.delayed(const Duration(seconds: 2));
      expect(owner.hasPendingAction, isTrue);
      expect(peer.actions, 0);
      peer.sessions.holdOpen!.complete();
      await _waitFor(() => peer.actions == 1);
      await _waitFor(() => !owner.hasPendingAction);
      expect(peer.actions, 1);
    },
  );

  // The former raw one-second queue expiry is replaced by the shared
  // received-head deadline and held-Ordered retirement cases.

  test(
    'Ordered rejection reaches its caller before the native write returns',
    () async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      final nativeReturn = Completer<void>();
      final events = <LitchControlEvent>[];
      final subscription = peer.service.controlActionEvents.listen(events.add);
      final results = <LitchControlEvent>[];
      try {
        await peer.open();
        final native = peer.sessions.sessions.single;
        native.holdNextOrderedWrite = nativeReturn;
        unawaited(
          peer.service
              .dispatchControlAction(
                deviceId: 'board-1',
                action: LitchControlAction.coaching,
                owner: LitchControlOwner.p2,
                enabled: true,
              )
              .then(results.add),
        );
        await _waitFor(() => native.holdNextOrderedWrite == null);
        expect(native.holdNextOrderedWrite, isNull);
        final head = _orderedPackets(native).single;
        expect(head.code, LitchControlProtocol.connectionActionOpcode);
        expect(head.sequence, 1);
        expect(
          head.body,
          LitchControlProtocol.encodeConnectionAction(
            action: LitchControlAction.coaching,
            owner: LitchControlOwner.p2,
            enabled: true,
          ),
        );
        final pending = events.single;
        expect(pending.phase, LitchControlPhase.pending);
        expect(pending.requestId, isNotNull);

        peer.control(
          ConnectionSessionControl.outcome,
          head.token,
          head.sequence,
          Uint8List.fromList([1, LitchMutationReason.busy.wireCode, 0]),
        );
        await pumpEventQueue(times: 1);
        expect(
          results,
          isEmpty,
          reason: 'The terminal Checkpoint is still due.',
        );
        peer.terminal = head.sequence;
        final checkpoint = Uint8List(10)..[8] = 1;
        peer.control(
          ConnectionSessionControl.checkpoint,
          head.token,
          head.sequence,
          checkpoint,
        );
        await pumpEventQueue(times: 1);
        final rejected = results.single;
        expect(rejected.phase, LitchControlPhase.rejected);
        expect(rejected.reason, LitchMutationReason.busy);
        expect(rejected.action, LitchControlAction.coaching);
        expect(rejected.owner, LitchControlOwner.p2);
        expect(rejected.requestId, pending.requestId);
        expect(events, [pending, rejected]);
        final confirms = _sharedPackets(native).where(
          (p) =>
              p.kind == ConnectionSessionKind.control &&
              p.code == ConnectionSessionControl.confirm &&
              p.token == head.token,
        );
        expect(confirms, isEmpty, reason: 'The Ordered write still owns GATT.');

        nativeReturn.complete();
        await pumpEventQueue(times: 1);
        expect(confirms.map((p) => p.sequence), [head.sequence]);
        expect(_orderedPackets(native), hasLength(1));
        expect(events, [pending, rejected]);
        expect(results, [rejected]);
      } finally {
        if (!nativeReturn.isCompleted) nativeReturn.complete();
        await subscription.cancel();
        await peer.close();
      }
    },
  );

  for (final failOldWrite in [false, true]) {
    final settlement = failOldWrite ? 'failure' : 'success';
    test(
      'late Ordered native $settlement leaves same-board replacement pending',
      () async {
        // With Settings alone advertised, each control action owns sequence one.
        // Identify-capable peers also issue a startup Probe and Stop.
        final peer = _RecoverySharedPeer(connected: false)
          ..infoPayload = '{"i":[3,3,${LitchDeviceCapabilities.settingsBit}]}';
        peer.onStateQuery = () {
          peer.home();
          peer.settings();
        };
        final nativeReturn = Completer<void>();
        final events = <LitchControlEvent>[];
        final subscription = peer.service.controlActionEvents.listen(
          events.add,
        );
        final results = <LitchControlEvent>[];
        Future<ConnectionSession> connect(int index) async {
          final connecting = peer.service.connect('board-1');
          await _waitFor(() => peer.ble.connections.length > index);
          peer.ble.connections[index].add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          expect(await connecting, isTrue);
          return (await peer.service.openConnectionSession('board-1'))!;
        }

        void dispatch(LitchControlAction action) {
          unawaited(
            peer.service
                .dispatchControlAction(deviceId: 'board-1', action: action)
                .then(results.add),
          );
        }

        try {
          final oldSession = await connect(0);
          final oldGeneration = peer.service.connectionSessionToken;
          final oldNative = peer.sessions.sessions.single;
          oldNative.holdNextOrderedWrite = nativeReturn;
          dispatch(LitchControlAction.hint);
          await _waitFor(() => oldNative.holdNextOrderedWrite == null);
          expect(oldNative.holdNextOrderedWrite, isNull);
          final oldHead = _orderedPackets(oldNative).single;
          expect(oldHead.code, LitchControlProtocol.connectionActionOpcode);
          expect(oldHead.sequence, 1);
          expect(
            oldHead.body,
            LitchControlProtocol.encodeConnectionAction(
              action: LitchControlAction.hint,
            ),
          );
          final oldPending = events.single;
          expect(oldPending.phase, LitchControlPhase.pending);
          expect(oldPending.action, LitchControlAction.hint);
          expect(results, isEmpty);

          peer.ble.connections.single.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
          await pumpEventQueue(times: 1);
          expect(oldSession.isActivated, isFalse);
          final interrupted = results.single;
          expect(interrupted.phase, LitchControlPhase.interrupted);
          expect(interrupted.action, LitchControlAction.hint);
          expect(interrupted.requestId, oldPending.requestId);
          peer.actions = 0;
          peer.terminal = 0;
          peer.retained = false;
          final replacement = await connect(1);
          expect(replacement, isNot(same(oldSession)));
          expect(peer.service.connectionSessionToken, isNot(oldGeneration));
          expect(peer.sessions.sessions, hasLength(2));
          final newNative = peer.sessions.sessions.last;
          dispatch(LitchControlAction.undo);
          await _waitFor(() => _orderedPackets(newNative).isNotEmpty);
          final newHead = _orderedPackets(newNative).single;
          expect(newHead.code, LitchControlProtocol.connectionActionOpcode);
          expect(
            (newHead.token, newHead.sequence),
            (oldHead.token, oldHead.sequence),
          );
          expect(
            newHead.body,
            LitchControlProtocol.encodeConnectionAction(
              action: LitchControlAction.undo,
            ),
          );
          final pending = events.last;
          expect(pending.action, LitchControlAction.undo);
          expect(pending.phase, LitchControlPhase.pending);

          if (failOldWrite) {
            nativeReturn.completeError(
              StateError('retired native write failed'),
            );
          } else {
            nativeReturn.complete();
          }
          await pumpEventQueue(times: 1);
          expect(oldSession.isActivated, isFalse);
          expect(replacement.isActivated, isTrue);
          expect(results, [interrupted]);
          expect(events, [oldPending, interrupted, pending]);

          peer.terminal = newHead.sequence;
          peer.control(
            ConnectionSessionControl.checkpoint,
            newHead.token,
            newHead.sequence,
            Uint8List(10),
          );
          await pumpEventQueue(times: 1);
          final applied = results.last;
          expect(applied.phase, LitchControlPhase.applied);
          expect(applied.action, LitchControlAction.undo);
          expect(applied.requestId, pending.requestId);
          expect(results, [interrupted, applied]);
          expect(events, [oldPending, interrupted, pending, applied]);
          expect(_orderedPackets(oldNative), hasLength(1));
          expect(_orderedPackets(newNative), hasLength(1));
        } finally {
          if (!nativeReturn.isCompleted) nativeReturn.complete();
          await subscription.cancel();
          await peer.close();
        }
      },
    );
  }

  test('Ordered action native send failure is contained', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    await peer.open();
    peer.onOrdered = () => throw StateError('controlled native write failure');
    final result = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(result.phase, LitchControlPhase.sendFailed);
    expect(
      peer.service.controlActionAvailability('board-1'),
      isNot(LitchControlAvailability.unavailable),
    );
    expect(peer.actions, 1);
  });

  // The raw enumeration/JSON-action case is replaced by the current
  // enumeration recovery case below and the shared Ordered action cases above.

  test(
    'enumeration fault cannot promote a terminal suffix as a complete map',
    () async {
      final _ChunkedEnumerationBle fakeBle = _ChunkedEnumerationBle(
        failBeforeReadNumber: 2,
      );
      final LitchBleService service = _metadataService(fakeBle);

      final List<LitchSetting> recovered = await service.readSettings(
        'board-1',
      );

      expect(
        recovered.map((LitchSetting setting) => setting.key).toSet(),
        <String>{'GAME_FIRST_PLAYER', 'TIMER'},
      );
      expect(fakeBle.readCount, 5);
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.unavailable,
        reason: 'Metadata cannot establish live typed Settings authority',
      );

      await service.dispose();
    },
  );

  for (final splitAt in <int>[31, 32, 33, 34]) {
    test(
      'settings enumeration preserves retired slots32/33 across chunk$splitAt',
      () async {
        Map<String, Object?> item(int index) => <String, Object?>{
          'i': index,
          'k': SettingsStateWireCodec.visibleKeys[index],
          't': 'u32',
          'c': 'display',
          'v': index == 36 ? 6 : index,
          'df': 0,
          'mn': 0,
          'mx': 360,
        };
        final ble = _ChunkedEnumerationBle(
          chunks: <Map<String, Object?>>[
            <String, Object?>{
              'it': <Object?>[
                for (var index = 0; index < splitAt; ++index)
                  if (!SettingsStateWireCodec.retiredVisibleIndices.contains(
                    index,
                  ))
                    item(index),
              ],
              'nx': splitAt,
              'dn': false,
            },
            <String, Object?>{
              'it': <Object?>[
                for (var index = splitAt; index < 37; ++index)
                  if (!SettingsStateWireCodec.retiredVisibleIndices.contains(
                    index,
                  ))
                    item(index),
              ],
              'nx': 0,
              'dn': true,
            },
          ],
        );
        final service = _metadataService(ble);
        final settings = await service.readSettings('board-1');
        expect(
          service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.unavailable,
          reason: 'Metadata alone does not provide live typed State',
        );
        expect(
          settings.map((setting) => setting.key),
          isNot(contains('BLE_CONNECTION_INDICATOR')),
        );
        expect(
          settings.map((setting) => setting.key),
          isNot(contains('V3_BUTTON_LED_EMULATION')),
        );
        expect(settings.map((setting) => setting.key), isNot(contains('')));
        expect(
          settings
              .singleWhere(
                (setting) => setting.key == 'LIGHT_SQUARE_BRIGHTNESS',
              )
              .value,
          6,
        );
        expect(ble.readCount, 2);
        await service.dispose();
      },
    );
  }
  test(
    'settings enumeration rejects unreserved34 gap beside retired slots32/33',
    () async {
      final ble = _ChunkedEnumerationBle(
        chunks: <Map<String, Object?>>[
          <String, Object?>{
            'it': <Object?>[
              for (var index = 0; index < 37; ++index)
                if (!SettingsStateWireCodec.retiredVisibleIndices.contains(
                      index,
                    ) &&
                    index != 34)
                  <String, Object?>{
                    'i': index,
                    'k': SettingsStateWireCodec.visibleKeys[index],
                    't': 'u32',
                    'v': index,
                  },
            ],
            'nx': 0,
            'dn': true,
          },
        ],
      );
      final service = _metadataService(ble);
      await service.readSettings('board-1');
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.unavailable,
      );
      await service.dispose();
    },
  );

  test('exhausted settings enumeration is not authoritative', () async {
    final _ChunkedEnumerationBle fakeBle = _ChunkedEnumerationBle(
      alwaysFail: true,
    );
    final LitchBleService service = _metadataService(fakeBle);

    expect(
      await service.readSettings('board-1'),
      isEmpty,
      reason: 'An exhausted read must not invent metadata.',
    );
    expect(
      service.settingsReadAuthority('board-1'),
      LitchSettingsReadAuthority.unavailable,
    );

    await service.dispose();
  });

  test('concurrent settings readers share one complete enumeration', () async {
    final _ChunkedEnumerationBle fakeBle = _ChunkedEnumerationBle();
    final LitchBleService service = _metadataService(fakeBle);

    final List<List<LitchSetting>> results = await Future.wait(
      <Future<List<LitchSetting>>>[
        service.readSettings('board-1'),
        service.readSettings('board-1'),
      ],
    );

    for (final List<LitchSetting> settings in results) {
      expect(
        settings.map((LitchSetting setting) => setting.key).toSet(),
        <String>{'GAME_FIRST_PLAYER', 'TIMER'},
      );
    }
    expect(fakeBle.readCount, 2);
    expect(
      service.settingsReadAuthority('board-1'),
      LitchSettingsReadAuthority.unavailable,
      reason: 'Metadata alone does not provide live typed State',
    );

    await service.dispose();
  });

  test('allocator exhaustion is a local send failure', () async {
    final peer = _RecoverySharedPeer();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    await peer.open();
    final pending = <Future<LitchControlEvent>>[];
    for (var i = 0; i < 32; ++i) {
      pending.add(
        peer.service.dispatchControlAction(
          deviceId: 'board-1',
          action: LitchControlAction.hint,
        ),
      );
    }
    await _waitFor(() => peer.actions == 8);
    final exhausted = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(exhausted.phase, LitchControlPhase.sendFailed);
    expect(exhausted.requestId, isNull);
    expect(peer.actions, 8, reason: 'Ordered credit bounds native admissions');
    expect(
      peer.service.controlActionAvailability('board-1'),
      LitchControlAvailability.unknown,
    );
    await peer.close();
    expect(
      (await Future.wait(
        pending,
      )).every((event) => event.phase == LitchControlPhase.interrupted),
      isTrue,
    );
  });

  test('dispatch while disconnected is interrupted before transport', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    await peer.open();
    await peer.service.disconnect();
    final terminal = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(terminal.phase, LitchControlPhase.interrupted);
    expect(peer.actions, 0);
  });

  test('backgrounded dispatch is interrupted before transport', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    peer.onOrdered = peer.finish;
    await peer.open();
    await peer.service.setAppForegroundActive(false);
    final blocked = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.newGame,
    );
    expect(blocked.phase, LitchControlPhase.interrupted);
    expect(peer.actions, 0);
    await peer.service.setAppForegroundActive(true);
    final active = await peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.hint,
    );
    expect(active.phase, LitchControlPhase.applied);
    expect(peer.actions, 1);
  });

  test('foreground loss during discovery blocks the pending action', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.sessions.holdOpen = Completer<void>();
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    peer.onOrdered = peer.finish;
    final pending = peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.newGame,
    );
    await Future<void>.delayed(Duration.zero);
    await peer.service.setAppForegroundActive(false);
    peer.sessions.holdOpen!.complete();
    expect((await pending).phase, LitchControlPhase.interrupted);
    expect(peer.actions, 0);
  });

  test('an admitted action settles after foreground loss', () async {
    final peer = _RecoverySharedPeer();
    addTearDown(peer.close);
    peer.onStateQuery = () {
      peer.home();
      peer.settings();
    };
    await peer.open();
    final pending = peer.service.dispatchControlAction(
      deviceId: 'board-1',
      action: LitchControlAction.newGame,
    );
    await _waitFor(() => peer.actions == 1);
    await peer.service.setAppForegroundActive(false);
    peer.finish();
    expect((await pending).phase, LitchControlPhase.applied);
    expect(peer.actions, 1);
  });

  void publishPlayerPalette(
    _RecoverySharedPeer peer,
    int revision,
    int near,
    int far,
    int first,
  ) {
    final body = Uint8List.fromList([1, 0, 0, 0, 0, near, far, first]);
    ByteData.sublistView(body).setUint32(1, revision, Endian.little);
    peer.native!.emit(
      BleProtocol.connectionSessionEgressCharacteristic,
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: 46,
          token: 0x80000001,
          sequence: peer.settingsIdentity(body),
          body: body,
        ),
      ),
    );
  }

  testWidgets(
    'live palette State keeps both physical ends current while Settings metadata is pending',
    (tester) async {
      final peer = _RecoverySharedPeer();
      peer.onStateQuery = () => peer.home();
      final metadata = Completer<List<int>>();
      HomeBoardFeed? feed;
      StreamSubscription<String>? subscription;

      try {
        var reads = 0;
        peer.sessions.sessions.single.onRead = (characteristic) {
          if (characteristic.characteristicId !=
              BleProtocol.settingsEnumerateCharacteristic) {
            return Future.value(utf8.encode('[]'));
          }
          reads++;
          return metadata.future;
        };
        await _openRecoveryPeer(tester, peer);
        publishPlayerPalette(peer, 6, 42, 203, 1);
        await tester.pump();
        var done = false;
        final reading = peer.service
            .readSettings('board-1')
            .whenComplete(() => done = true);
        for (var pass = 0; pass < 20 && reads == 0; pass++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
        }
        expect(reads, 1);
        feed = HomeBoardFeed(bleService: peer.service, boardId: 'board-1');
        await tester.pump();
        expect(
          [feed.current.p1PieceWheel, feed.current.p2PieceWheel],
          [42, 203],
        );
        // A complete Settings revision is still in flight when the player moves the wheel.
        peer.settings(count: 8, revision: 7);
        final observations = <List<Object?>>[];
        final keys = [
          'P1_END_PIECE_WHEEL',
          'P2_END_PIECE_WHEEL',
          'P1_PIECE_WHEEL',
          'P2_PIECE_WHEEL',
          'GAME_FIRST_PLAYER',
        ];
        subscription = peer.service.settingsChangedStream
            .where(keys.contains)
            .listen((_) {
              observations.add(
                keys.map(peer.service.latestSettingValue).toList(),
              );
            });
        publishPlayerPalette(peer, 8, 96, 211, 2);
        await tester.pump(const Duration(milliseconds: 16));
        expect(observations, hasLength(keys.length));
        expect(observations, everyElement([96, 211, 96, 211, 2]));
        expect(
          [feed.current.p1PieceWheel, feed.current.p2PieceWheel],
          [96, 211],
        );
        expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
        expect(
          peer.service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.unavailable,
        );
        LitchEndPalette? immediatePalette;
        final catchingUp = peer.service
            .readPhysicalEndPalette(deviceId: 'board-1')
            .then((value) => immediatePalette = value);
        publishPlayerPalette(peer, 8, 96, 211, 2);
        await tester.pump();
        final availableBeforeMetadata = [
          immediatePalette?.p1ControlsWheel,
          immediatePalette?.p2ControlsWheel,
        ];
        expect(
          observations,
          hasLength(keys.length),
          reason: 'replaying the same palette is quiet',
        );
        metadata.complete(
          utf8.encode(
            jsonEncode([
              {'i': 2, 'k': 'P1_PIECE_WHEEL', 't': 'u32', 'v': 42},
              {'i': 3, 'k': 'P2_PIECE_WHEEL', 't': 'u32', 'v': 203},
              {'i': 11, 'k': 'GAME_FIRST_PLAYER', 't': 'u32', 'v': 1},
            ]),
          ),
        );
        await tester.pump();
        for (var pass = 0; pass < 12 && !done; pass++) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(done, isTrue, reason: 'released descriptor read must settle');
        final descriptors = await reading;
        await catchingUp;
        await _flushSdkCancellation(tester, feed.dispose());
        feed = HomeBoardFeed(bleService: peer.service, boardId: 'board-1');
        await tester.pump(const Duration(milliseconds: 16));
        expect(
          {
            'palette available before metadata': availableBeforeMetadata,
            'descriptor P1': descriptors
                .singleWhere((s) => s.key == 'P1_PIECE_WHEEL')
                .value,
            'physical wheels, aliases and first player': keys
                .map(peer.service.latestSettingValue)
                .toList(),
            'Home P1 at opposite end':
                feed.current.physicalStatus?.p1AtOppositeEnd,
          },
          {
            'palette available before metadata': [96, 211],
            'descriptor P1': 96,
            'physical wheels, aliases and first player': [96, 211, 96, 211, 2],
            'Home P1 at opposite end': true,
          },
        );
        expect(keys.map(peer.service.latestSettingValue), [
          96,
          211,
          96,
          211,
          2,
        ]);
        expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
        peer.settings(count: 1, firstFragment: 8, revision: 7);
        await tester.pump();
        expect(peer.service.boardStateSettingsRevision, 7);
        expect(keys.map(peer.service.latestSettingValue), [
          96,
          211,
          96,
          211,
          2,
        ]);
        final palette = await peer.service.readPhysicalEndPalette(
          deviceId: 'board-1',
        );
        expect(palette?.usesDirectPhysicalKeys, isTrue);
        expect([palette?.p1ControlsWheel, palette?.p2ControlsWheel], [96, 211]);
        peer.settings(revision: 9);
        await tester.pump();
        expect(keys.map(peer.service.latestSettingValue), [170, 0, 170, 0, 1]);
        expect(
          [feed.current.p1PieceWheel, feed.current.p2PieceWheel],
          [170, 0],
        );
        expect(feed.current.physicalStatus?.p1AtOppositeEnd, isFalse);
        expect(
          peer.native!.writes,
          isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
        );
      } finally {
        if (!metadata.isCompleted) metadata.complete(utf8.encode('[]'));
        await _flushSdkCancellation(
          tester,
          subscription?.cancel() ?? Future<void>.value(),
        );
        await _flushSdkCancellation(
          tester,
          feed?.dispose() ?? Future<void>.value(),
        );
        await _flushSdkCancellation(tester, peer.close());
      }
    },
  );

  test(
    'Setup edits use one Ordered packet and confirm the exact typed value',
    () async {
      final peer = _RecoverySharedPeer();
      var revision = 7;
      final bytes = peer.settingsPayload(revision: revision);
      final data = ByteData.sublistView(bytes);
      void publish([Uint8List? payload]) {
        final snapshot = payload ?? bytes;
        peer.stateFragments(snapshot, 14, 9, peer.settingsIdentity(snapshot));
      }

      void setWheel(Uint8List payload, int index, int value) {
        ByteData.sublistView(
          payload,
        ).setUint16(4 + 2 * index, value, Endian.little);
        payload[78 + index - 2] = value;
      }

      peer.onStateQuery = () {
        peer.home();
        publish();
      };
      try {
        await peer.open();
        await _waitFor(() => peer.service.boardStateSettingsRevision == 7);
        peer.native!.readResponses[BleProtocol
            .settingsEnumerateCharacteristic] = utf8.encode(
          jsonEncode([
            {
              'i': 28,
              'k': 'BOARD_SETUP_ASSISTANCE',
              't': 'bool',
              'c': 'behavior',
              'v': false,
              'df': true,
              'd': 'Board setup assistance',
            },
          ]),
        );
        final descriptor = (await peer.service.readSettings(
          'board-1',
        )).singleWhere((setting) => setting.key == 'BOARD_SETUP_ASSISTANCE');
        expect(
          [
            descriptor.type,
            descriptor.category,
            descriptor.description,
            descriptor.defaultValue,
            descriptor.value,
          ],
          ['bool', 'behavior', 'Board setup assistance', true, true],
        );
        for (final (key, value, index, normalized)
            in <(String, Object, int, int)>[
              ('BRIGHTNESS', 8, 0, 8),
              ('VS_CPU_LEVEL', 11, 7, 11),
              ('VS_CPU_PLAYER', 2, 9, 2),
              ('GAME_FIRST_PLAYER', 2, 11, 2),
              ('P2_PIECE_WHEEL', 128, 3, 128),
              ('P1_PIECE_WHEEL', 255, 2, 255),
              ('P1_PIECE_WHEEL', 85, 2, 85),
              ('P2_PIECE_WHEEL', 255, 3, 255),
              ('P2_PIECE_WHEEL', 40, 3, 40),
              ('WHITE_SQUARES_ON', true, 1, 1),
              ('SETTLE_ANIM_STYLE', 'ripple', 30, 2),
              ('TIMER', 6, 12, 6),
              ('BOARD_SETUP_ASSISTANCE', false, 28, 0),
              ('BOARD_ROTATE', false, 10, 0),
              ('BOARD_ROTATE', true, 10, 1),
              ('BOARD_ROTATE', false, 10, 0),
            ]) {
          final previous = peer.service.latestSettingValue(key);
          final before = peer.native!.writeRecords.length;
          final actions = peer.actions;
          peer.onOrdered = () => peer.finish(revision: revision + 1);
          final writing = peer.service.writeSettingAcknowledged(
            deviceId: 'board-1',
            key: key,
            value: value,
          );
          await Future<void>.delayed(Duration.zero);
          final result = await writing as LitchSettingWriteAcknowledged;
          expect([result.outcome.key, result.outcome.ok], [key, true]);
          expect(peer.actions, actions + 1);
          final ordered = peer.native!.writeRecords
              .skip(before)
              .where(
                (record) =>
                    record.characteristicId ==
                    BleProtocol.connectionSessionIngressCharacteristic,
              )
              .map(
                (record) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(record.value),
                )!,
              )
              .where((packet) => packet.kind == ConnectionSessionKind.ordered)
              .single;
          expect(
            [ordered.code, ...ordered.body],
            [24, 2, index, normalized, 0],
            reason: key,
          );
          expect(
            ConnectionSessionCodec.encode(ordered).length,
            lessThanOrEqualTo(20),
          );
          expect(
            peer.service.latestSettingValue(key),
            previous,
            reason: 'the terminal receipt alone cannot publish a setting',
          );
          revision++;
          data.setUint32(0, revision, Endian.little);
          data.setUint16(4 + 2 * index, normalized, Endian.little);
          if (index == 2 || index == 3) {
            setWheel(bytes, index, normalized);
            bool? confirmed;
            result.boardStateConfirmation!.then((value) => confirmed = value);
            final impossible = Uint8List.fromList(bytes);
            setWheel(impossible, index == 2 ? 3 : 2, normalized);
            publish(impossible);
            await Future<void>.delayed(Duration.zero);
            expect(
              confirmed,
              isNull,
              reason: 'The two ends cannot share one hue.',
            );
          }
          publish();
          await Future<void>.delayed(Duration.zero);
          expect(await result.boardStateConfirmation!, isTrue, reason: key);
          expect(
            await peer.service.readSettingValue(deviceId: 'board-1', key: key),
            value,
          );
        }
        expect(
          peer.native!.writes,
          isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
        );

        // Starting at 85/40, P1=0 is accepted literally, then P1=235 and
        // P2=11 supersede it. Their cumulative Applied Checkpoint has one revision.
        peer.onOrdered = null;
        peer.onStateQuery = null;
        final actions = peer.actions;
        final issued = _orderedPackets(peer.native!).length;
        final edits = [
          for (final (key, value) in [
            ('P1_END_PIECE_WHEEL', 0),
            ('P1_END_PIECE_WHEEL', 235),
            ('P2_END_PIECE_WHEEL', 11),
          ])
            peer.service.writeSettingAcknowledged(
              deviceId: 'board-1',
              key: key,
              value: value,
            ),
        ];
        await _waitFor(() => peer.actions == actions + 3);
        expect(_orderedPackets(peer.native!).skip(issued).map((p) => p.body), [
          [2, 37, 0, 0],
          [2, 37, 235, 0],
          [2, 38, 11, 0],
        ]);
        revision += 3;
        peer.finish(revision: revision);
        final results = (await Future.wait(
          edits,
        )).cast<LitchSettingWriteAcknowledged>();
        expect(results.every((edit) => edit.outcome.ok), isTrue);
        bool? superseded;
        results.first.boardStateConfirmation!.then(
          (value) => superseded = value,
        );
        setWheel(bytes, 2, 235);
        setWheel(bytes, 3, 11);
        data.setUint32(0, revision, Endian.little);
        publish();
        expect(await results[1].boardStateConfirmation, isTrue);
        expect(await results[2].boardStateConfirmation, isTrue);
        expect(
          superseded,
          isNull,
          reason: 'The later opposite hue cannot reinterpret the first write.',
        );
        final retired = peer.native!;
        final capabilities = peer.service.deviceCapabilities('board-1');
        await peer.service.disconnect();
        expect(await results.first.boardStateConfirmation, isFalse);
        peer.actions = 0;
        peer.terminal = 0;
        peer.retained = false;
        peer.service.debugSetConnectedDevice('board-1');
        peer.service.debugSetDeviceCapabilities('board-1', capabilities);
        peer.onStateQuery = () {
          peer.home();
          publish();
        };
        await peer.open();
        await _waitFor(
          () => peer.service.latestSettingValue('P1_END_PIECE_WHEEL') == 235,
        );
        final active = peer.native;
        peer.native = retired;
        final late = Uint8List.fromList(bytes);
        setWheel(late, 2, 0);
        setWheel(late, 3, 40);
        publish(late);
        peer.native = active;
        await Future<void>.delayed(Duration.zero);
        expect(superseded, isFalse);
        expect(peer.service.latestSettingValue('P1_END_PIECE_WHEEL'), 235);
      } finally {
        await peer.close();
      }
    },
  );

  for (final reason in [
    LitchMutationReason.notAtStart,
    LitchMutationReason.busy,
    LitchMutationReason.outOfRange,
  ]) {
    test(
      'Ordered Settings rejection preserves ${reason.wireToken} and leaves the value intact',
      () async {
        final peer = _RecoverySharedPeer();
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 7, brightness: 5);
        };
        await peer.open();
        final write = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
          value: 99,
        );
        await _waitFor(() => peer.actions == 1);
        var settled = false;
        write.then((_) => settled = true);
        final checkpoint = Uint8List(10)..[8] = 1;
        ByteData.sublistView(checkpoint).setUint32(0, 7, Endian.little);
        peer.control(ConnectionSessionControl.checkpoint, 2, 1, checkpoint);
        // Neither another stream's outcome nor an invalid disposition settles this request.
        peer.control(
          ConnectionSessionControl.outcome,
          3,
          1,
          Uint8List.fromList([1, 8, 0]),
        );
        final malformed = ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.control,
            code: ConnectionSessionControl.outcome,
            token: 2,
            sequence: 1,
            body: Uint8List.fromList([1, 8, 0]),
          ),
        )..[ConnectionSessionCodec.headerBytes] = 3;
        peer.native!.emit(
          BleProtocol.connectionSessionEgressCharacteristic,
          malformed,
        );
        await Future<void>.delayed(Duration.zero);
        expect(settled, isFalse);
        peer.control(
          ConnectionSessionControl.outcome,
          2,
          1,
          Uint8List.fromList([1, reason.wireCode, 0]),
        );
        final result = await write as LitchSettingWriteAcknowledged;
        expect(result.outcome.ok, isFalse);
        expect(result.outcome.reason, reason);
        expect(result.boardStateConfirmation, isNull);
        expect(peer.service.latestSettingValue('BRIGHTNESS'), 5);
        final diagnostics = peer.service.supportSessionDiagnostics;
        expect(
          [
            diagnostics.settingsWriteAttempts,
            diagnostics.settingsWriteFailed,
            diagnostics.settingsWriteCompleted,
          ],
          [1, 1, 1],
        );
        expect(
          diagnostics.lastSettingsFailureToken,
          'board:${reason.wireToken}',
        );
      },
    );
  }

  test(
    'native Settings packet write failure reports an interrupted operation',
    () async {
      final peer = _RecoverySharedPeer(connected: false);
      addTearDown(peer.close);
      var attempts = 0;
      peer.sessions.onWrite = (native, characteristic, bytes) {
        if (characteristic ==
            BleProtocol.connectionSessionIngressCharacteristic) {
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(bytes),
          )!;
          if (packet.kind == ConnectionSessionKind.ordered &&
              packet.code == 24) {
            attempts++;
            throw StateError('controlled native write failure');
          }
        }
        peer.write(native, characteristic, bytes);
      };
      peer.service.debugSetConnectedDevice('board-1');
      peer.service.debugSetDeviceCapabilities(
        'board-1',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          explicitlyReported: true,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask: LitchDeviceCapabilities.settingsBit,
        ),
      );
      peer.onStateQuery = () {
        peer.home();
        peer.settings(revision: 7);
      };
      await peer.open();
      final result =
          await peer.service.writeSettingAcknowledged(
                deviceId: 'board-1',
                key: 'BRIGHTNESS',
                value: 8,
              )
              as LitchSettingWriteNotAcknowledged;
      expect(result.failure, LitchSettingWriteFailure.interrupted);
      expect(attempts, 1);
      expect(peer.actions, 0);
      expect(
        peer.service.supportSessionDiagnostics.lastSettingsFailureToken,
        'app:interrupted',
      );
    },
  );

  for (final deadline in [false, true]) {
    test(
      deadline
          ? 'retained Settings write reports interruption after its Ordered deadline'
          : 'disconnect interrupts a retained Settings write without updating the cache',
      () async {
        final peer = _RecoverySharedPeer(
          receivedHeadCompletionTimeout: const Duration(seconds: 2),
        );
        addTearDown(peer.close);
        peer.onStateQuery = () {
          peer.home();
          peer.settings(revision: 7);
        };
        await peer.open();
        final write = peer.service.writeSettingAcknowledged(
          deviceId: 'board-1',
          key: 'BRIGHTNESS',
          value: 8,
        );
        await _waitFor(() => peer.actions == 1);
        if (!deadline) await peer.service.disconnect();
        final result = await write as LitchSettingWriteNotAcknowledged;
        expect(result.failure, LitchSettingWriteFailure.interrupted);
        expect(peer.actions, 1);
        final diagnostics = peer.service.supportSessionDiagnostics;
        expect(
          [
            diagnostics.settingsWriteAttempts,
            diagnostics.settingsWriteFailed,
            diagnostics.settingsWriteCompleted,
          ],
          [1, 1, 1],
        );
        expect(diagnostics.lastSettingsOperation, 'setting_write:BRIGHTNESS');
        expect(diagnostics.lastSettingsFailureToken, 'app:interrupted');
        expect(peer.service.latestSettingValue('BRIGHTNESS'), isNot(8));
      },
    );
  }

  for (final retire in [false, true]) {
    test(
      retire
          ? 'pending palette read stops at connection retirement'
          : 'pending palette read returns both ends from the completed Settings State',
      () async {
        final peer = _RecoverySharedPeer();
        final held = Completer<List<int>>();
        var reads = 0;
        peer.sessions.sessions.single.onRead = (characteristic) {
          if (characteristic.characteristicId !=
              BleProtocol.settingsEnumerateCharacteristic) {
            return Future.value(utf8.encode('[]'));
          }
          reads++;
          return held.future;
        };
        peer.onStateQuery = () => peer.home();
        try {
          await peer.open();
          final reading = peer.service.readPhysicalEndPalette(
            deviceId: 'board-1',
          );
          await _waitFor(() => reads == 1);
          if (retire) {
            publishPlayerPalette(peer, 8, 96, 211, 2);
            await Future<void>.delayed(Duration.zero);
            expect(peer.service.latestSettingValue('P1_END_PIECE_WHEEL'), 96);
            await peer.service.disconnect();
            peer.service.debugSetConnectedDevice('board-1');
          } else {
            final bytes = peer.settingsPayload(revision: 7);
            final data = ByteData.sublistView(bytes);
            data.setUint16(8, 42, Endian.little);
            data.setUint16(10, 203, Endian.little);
            data.setUint16(26, 2, Endian.little);
            bytes[78] = 42;
            bytes[79] = 203;
            peer.stateFragments(bytes, 14, 9, peer.settingsIdentity(bytes));
          }
          held.complete(utf8.encode('[]'));
          final palette = await reading;
          if (retire) {
            expect(palette, isNull);
            expect(
              peer.service.latestSettingValue('P1_END_PIECE_WHEEL'),
              isNull,
            );
            expect(
              peer.service.latestSettingValue('GAME_FIRST_PLAYER'),
              isNull,
            );
            expect(peer.sessions.sessions.last.reads, isEmpty);
          } else {
            expect(
              [palette?.p1ControlsWheel, palette?.p2ControlsWheel],
              [42, 203],
            );
            expect(palette?.usesDirectPhysicalKeys, isTrue);
            expect(peer.service.latestSettingValue('GAME_FIRST_PLAYER'), 2);
          }
          expect(reads, 1);
        } finally {
          if (!held.isCompleted) held.complete(utf8.encode('[]'));
          await peer.close();
        }
      },
    );
  }

  test(
    'disconnect interrupts a received Ordered action exactly once',
    () async {
      final peer = _RecoverySharedPeer();
      addTearDown(peer.close);
      peer.onStateQuery = () {
        peer.home();
        peer.settings();
      };
      await peer.open();
      final events = <LitchControlEvent>[];
      final subscription = peer.service.controlActionEvents.listen(events.add);
      addTearDown(subscription.cancel);
      final pending = peer.service.dispatchControlAction(
        deviceId: 'board-1',
        action: LitchControlAction.undo,
      );
      await _waitFor(() => peer.actions == 1 && peer.retained);
      expect(events.where((event) => event.isTerminal), isEmpty);
      await peer.service.disconnect();
      final terminal = await pending;
      expect(terminal.phase, LitchControlPhase.interrupted);
      expect(
        events.where((event) => event.isTerminal).map((event) => event.phase),
        [LitchControlPhase.interrupted],
      );
    },
  );

  test('dispose is idempotent across repeated app lifecycle cleanup', () async {
    final _ReconnectBle fakeBle = _ReconnectBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );

    final Future<bool> connecting = service.connect('board-1');
    await _waitFor(() => fakeBle.connections.isNotEmpty);
    fakeBle.connections.single.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    expect(await connecting, isTrue);

    await service.dispose();
    await service.dispose();

    for (final StreamController<ConnectionStateUpdate> stream
        in fakeBle.connections) {
      if (!stream.isClosed) {
        await stream.close();
      }
    }
    for (final StreamController<List<int>> stream
        in fakeBle.notificationStreams) {
      if (!stream.isClosed) {
        await stream.close();
      }
    }
  });

  testWidgets(
    'cold app shell waits for board capabilities before requesting a firmware digest',
    (WidgetTester tester) async {
      PackageInfo.setMockInitialValues(
        appName: 'Litch',
        packageName: 'com.foxieproducts.litchMobile',
        version: '0.5.0',
        buildNumber: '1',
        buildSignature: '',
      );
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final heldInfoWrite = Completer<void>();
      final fakeBle = _AppShellReconnectBle(exactGatt: true)
        ..heldInfoWrite = heldInfoWrite;
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        gattSessionFactory: ReactiveBleGattSessionFactory(fakeBle),
        permissionGateway: _PermissionGateway(<Permission, PermissionStatus>{
          Permission.bluetooth: PermissionStatus.granted,
          Permission.bluetoothScan: PermissionStatus.granted,
          Permission.bluetoothConnect: PermissionStatus.granted,
          Permission.locationWhenInUse: PermissionStatus.granted,
        }),
      );
      bool cleaned = false;
      Future<void> cleanup() async {
        if (cleaned) return;
        cleaned = true;
        await tester.pumpWidget(const SizedBox.shrink());
        final disposing = _disposeReconnectHarness(service, fakeBle);
        if (!heldInfoWrite.isCompleted) heldInfoWrite.complete();
        await _flushSdkCancellation(tester, disposing);
        await fakeBle.discovery.close();
        await tester.pump(const Duration(seconds: 6));
      }

      addTearDown(cleanup);
      final connecting = service.connect('board-1');
      for (int pass = 0; pass < 8 && fakeBle.connections.isEmpty; ++pass) {
        await tester.pump();
      }
      expect(fakeBle.connections, hasLength(1));
      fakeBle.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      await tester.pump();
      expect(await connecting, isTrue);
      for (
        int pass = 0;
        pass < 8 && fakeBle.deviceInfoWriteCount == 0;
        ++pass
      ) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(fakeBle.deviceInfoWriteCount, 1);
      expect(heldInfoWrite.isCompleted, isFalse);
      bool capabilitiesCompleted = false;
      unawaited(
        service
            .fetchDeviceCapabilities(deviceId: 'board-1')
            .then((_) => capabilitiesCompleted = true),
      );

      // The retained Firmware page can ask for provenance while Connect
      // is visible. The current board must first report its capabilities.
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: ConnectionGateScreen(bleService: service),
        ),
      );
      for (var pass = 0; pass < 8; ++pass) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(
        find.byType(LitchMainScaffold, skipOffstage: false),
        findsOneWidget,
      );
      expect(find.byType(LitchMainScaffold), findsNothing);
      expect(find.byType(FirmwareScreen, skipOffstage: false), findsOneWidget);
      expect(
        find.byKey(const ValueKey('connection-progress-overlay')),
        findsOneWidget,
      );
      expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
      expect(
        find.byKey(const ValueKey('discovered-board-board-1')),
        findsOneWidget,
      );
      expect(find.text('Connected'), findsNothing);
      final semantics = tester.ensureSemantics();
      expect(
        tester
            .getSemantics(find.byKey(const ValueKey('chosen-board-phase')))
            .label,
        'Connecting…',
      );
      final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
      expect(cancel.hitTestable(), findsOneWidget);
      expect(tester.getSize(cancel).shortestSide, greaterThanOrEqualTo(48));
      expect(
        tester
            .getSemantics(cancel)
            .getSemanticsData()
            .hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      semantics.dispose();
      expect(
        fakeBle.otaControlWritePayloads.map(
          (bytes) => jsonDecode(utf8.decode(bytes)),
        ),
        [
          <String, Object?>{'cmd': 'info'},
        ],
        reason:
            'The current attachment has not advertised firmware digest support.',
      );
      expect(heldInfoWrite.isCompleted, isFalse);
      expect(capabilitiesCompleted, isFalse);
      expect(service.deviceCapabilities('board-1').explicitlyReported, isFalse);
      final handles = fakeBle.discoveredSessions.single.expand(
        (sdkService) => sdkService.characteristics,
      );
      expect(
        handles
            .singleWhere(
              (handle) => handle.id == BleProtocol.otaControlCharacteristic,
            )
            .writeCount,
        1,
      );
      expect(
        handles
            .singleWhere(
              (handle) =>
                  handle.id ==
                  BleProtocol.connectionSessionIngressCharacteristic,
            )
            .writeCount,
        0,
        reason: 'Shared work waits for the capability response.',
      );
      expect(fakeBle.unexpectedQualifiedCalls, 0);
      expect(fakeBle.unexpectedRetiredHandleCalls, 0);
      expect(tester.takeException(), isNull);
      await cleanup();

      // Reuse the production service/session seam for the current S3 path.
      // Metadata must not start a second image scan before normal acceptance.
      await tester.runAsync(() async {
        final peer = ota_board.RebootReplyBle()..acceptanceReply = '{"ov":"p"}';
        final gated = LitchBleService(
          ble: peer,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        try {
          await ota_board.connectBoard(gated, peer);
          final firstSession = await gated.openConnectionSession('board-1');
          expect(firstSession?.isActivated, isTrue);
          final pending = await gated.fetchFirmwareProvenance(
            deviceId: 'board-1',
          );
          expect(
            pending.sha256,
            isNull,
            reason:
                'Pending health must not launch ordinary provenance hashing.',
          );
          expect(peer.acceptanceRequests, 1);
          expect(peer.digestRequests, 0);

          peer.acceptanceReply = '{"e":"unavailable"}';
          expect(
            (await gated.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
            isNull,
          );
          expect(peer.acceptanceRequests, 2);
          expect(
            peer.digestRequests,
            0,
            reason:
                'Unavailable current-peer acceptance never falls through to a scan.',
          );

          peer.acceptanceReply = '{"ov":"v"}';
          expect(
            (await gated.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
            peer.runningDigest,
          );
          expect(peer.acceptanceRequests, 3);
          expect(peer.digestRequests, 1);

          await gated.disconnect();
          peer.acceptanceReply = '{"ov":"p"}';
          await ota_board.connectBoard(gated, peer);
          final nextSession = await gated.openConnectionSession('board-1');
          expect(nextSession?.isActivated, isTrue);
          expect(nextSession, isNot(same(firstSession)));
          expect(
            (await gated.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
            isNull,
          );
          expect(peer.acceptanceRequests, 4);
          expect(
            peer.digestRequests,
            1,
            reason:
                'An earlier accepted connection cannot lend this generation readiness.',
          );
          peer.acceptanceReply = '{"ov":"v"}';
          expect(
            (await gated.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
            peer.runningDigest,
          );
          expect(peer.acceptanceRequests, 5);
          expect(peer.digestRequests, 2);

          await gated.disconnect();
          peer.recoveryOnly = true;
          peer.acceptanceReply = '{"ov":"p"}';
          await ota_board.connectBoard(gated, peer);
          await gated.openConnectionSession('board-1');
          expect(gated.deviceCapabilities('board-1').recoveryOnly, isTrue);
          expect(
            (await gated.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
            peer.runningDigest,
          );
          expect(
            peer.acceptanceRequests,
            5,
            reason: 'Recovery deliberately never publishes normal acceptance.',
          );
          expect(peer.digestRequests, 3);
        } finally {
          await ota_board.disposeBoard(gated, peer);
        }
      });
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  // The former raw-peer reconnect matrix used removed capability fallback.
  // Current Android/iOS Console, seating, capability-gate and retired-reply
  // journeys above retain its supported reconnect obligations.

  testWidgets(
    'platform link loss clears typed Home CPU prompts across board replacement',
    (WidgetTester tester) async {
      final _PerBoardCpuReconnectBle fakeBle =
          _PerBoardCpuReconnectBle(exactGatt: true)
            ..useSharedPeer = true
            ..capabilityReply = '{"i":[3,3,31]}';
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
        gattSessionFactory: ReactiveBleGattSessionFactory(fakeBle),
      );
      Future<void> flushPlatform(bool Function() ready) async {
        for (int pass = 0; pass < 12 && !ready(); ++pass) {
          await tester.pump(Duration.zero);
          await tester.runAsync(() async {});
          await tester.pump(Duration.zero);
        }
        expect(ready(), isTrue);
      }

      Future<void> connect(String boardId, int count) async {
        final Future<bool> connecting = service.connect(boardId);
        await flushPlatform(() => fakeBle.connections.length == count);
        fakeBle.connections.last.add(
          ConnectionStateUpdate(
            deviceId: boardId,
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await tester.pump();
        expect(await connecting, isTrue);
        for (int pass = 0; pass < 4; ++pass) {
          await tester.pump(const Duration(milliseconds: 10));
        }
        final Future<bool> preparing = service.ensureProductReady(boardId);
        for (int pass = 0; pass < 12; ++pass) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() async {});
          await tester.pump();
          if (service.productReady) break;
        }
        expect(service.productReady, isTrue);
        expect(await preparing, isTrue);
      }

      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        for (final _CpuPromptSessionBoard peer in fakeBle.peers) {
          if (!peer.packets.isClosed) await peer.packets.close();
          if (!peer.sessionPackets.isClosed) await peer.sessionPackets.close();
        }
        final Future<void> cleanup = _disposeReconnectHarness(service, fakeBle);
        bool cleaned = false;
        unawaited(cleanup.then((_) => cleaned = true));
        await flushPlatform(() => cleaned);
        await cleanup;
        await fakeBle.discovery.close();
        await tester.pump(const Duration(seconds: 6));
      });

      await connect('board-1', 1);
      final _CpuPromptSessionBoard firstBoard = fakeBle.activePeer;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            body: HomeScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      firstBoard.publishCpuMove();
      for (
        int pass = 0;
        pass < 12 &&
            tester
                    .widget<HomeChessboard>(find.byType(HomeChessboard))
                    .snapshot
                    .liveSequence ==
                null;
        ++pass
      ) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(fakeBle.activePeer.sessionActivated, isTrue);
      expect(service.latestHomeState?.position.sideToMove, HomeSide.black);
      expect(service.latestHomeState?.cpuThinking?.side, HomeSide.black);
      expect(
        find.text('Move the computer piece from g8 to f6.'),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('cpu-move-from-g8')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('cpu-move-to-f6')),
        findsOneWidget,
      );
      expect(
        tester
            .widget<HomeChessboard>(find.byType(HomeChessboard))
            .snapshot
            .liveSequence,
        isNotNull,
      );

      final StreamController<ConnectionStateUpdate> firstConnection =
          fakeBle.connections.first;
      final DateTime lossAt = tester.binding.clock.now();
      firstConnection.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump(const Duration(milliseconds: 16));
      await tester.pump();
      expect(
        tester.binding.clock.now().difference(lossAt),
        const Duration(milliseconds: 16),
      );
      expect(service.isConnected, isFalse);
      expect(service.latestHomeState, isNull);
      expect(find.text('Move the computer piece from g8 to f6.'), findsNothing);
      expect(
        tester
            .widget<HomeChessboard>(find.byType(HomeChessboard))
            .snapshot
            .liveSequence,
        isNull,
      );
      expect(tester.takeException(), isNull);

      await connect('board-2', 2);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            body: HomeScreen(bleService: service, boardId: 'board-2'),
          ),
        ),
      );
      fakeBle.activePeer.publishCpuMove();
      for (
        int pass = 0;
        pass < 12 &&
            tester
                    .widget<HomeChessboard>(find.byType(HomeChessboard))
                    .snapshot
                    .liveSequence ==
                null;
        ++pass
      ) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(fakeBle.activePeer.sessionActivated, isTrue);
      expect(
        find.text('Move the computer piece from b8 to c6.'),
        findsOneWidget,
      );
      expect(
        tester
            .widget<HomeChessboard>(find.byType(HomeChessboard))
            .snapshot
            .liveSequence,
        isNotNull,
      );

      // A late publication from board-1's retired egress stream must not be
      // attributed to board-2 after its own typed Home snapshot is active.
      final int board2Sequence = service.latestHomeState!.sequence;
      firstBoard.publishCpuMove(sequence: 6);
      await tester.pump();
      expect(service.connectedId, 'board-2');
      expect(service.latestHomeState?.sequence, board2Sequence);
      expect(
        find.text('Move the computer piece from b8 to c6.'),
        findsOneWidget,
      );
      expect(find.text('Move the computer piece from g8 to f6.'), findsNothing);
      expect(tester.takeException(), isNull);
      fakeBle.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: 'board-2',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump();
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
  );

  test(
    'Android disconnect retires capability work while native MTU is pending',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final fakeBle = _ReconnectBle()..mtuReply = Completer<int>();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      try {
        final connecting = service.connect('board-1');
        await _waitFor(() => fakeBle.connections.isNotEmpty);
        fakeBle.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await connecting, isTrue);
        final generation = service.connectionSessionToken;
        var finished = false;
        final capabilities = service.fetchDeviceCapabilities(
          deviceId: 'board-1',
        );
        unawaited(capabilities.then((_) => finished = true));
        await Future<void>.delayed(Duration.zero);
        expect(fakeBle.connectionPriorities, <ConnectionPriority>[
          ConnectionPriority.highPerformance,
        ]);
        expect(finished, isFalse);
        fakeBle.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.disconnected,
            failure: null,
          ),
        );
        await Future<void>.delayed(Duration.zero);
        expect(service.isConnected, isFalse);
        expect(service.connectionSessionToken, greaterThan(generation));
        expect(
          finished,
          isTrue,
          reason:
              'Disconnected capability callers must not await Android MTU completion.',
        );
        expect((await capabilities).explicitlyReported, isFalse);
        final writesAtDisconnect = fakeBle.qualifiedWriteAttempts;
        fakeBle.mtuReply!.complete(247);
        await Future<void>.delayed(Duration.zero);
        expect(service.isConnected, isFalse);
        expect(
          fakeBle.qualifiedWriteAttempts,
          writesAtDisconnect,
          reason: 'A late MTU completion cannot restart old capability writes.',
        );
      } finally {
        if (!fakeBle.mtuReply!.isCompleted) fakeBle.mtuReply!.complete(247);
        await service.dispose();
        for (final stream in [
          ...fakeBle.connections,
          ...fakeBle.notificationStreams,
        ]) {
          if (!stream.isClosed) await stream.close();
        }
        debugDefaultTargetPlatformOverride = null;
      }
    },
  );

  test('platform link loss reaches app observers within 500 ms', () async {
    final _ReconnectBle fakeBle = _ReconnectBle();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );

    final Future<bool> connecting = service.connect('board-1');
    await _waitFor(() => fakeBle.connections.isNotEmpty);
    fakeBle.connections.single.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    expect(await connecting, isTrue);

    final Future<ConnectionStateUpdate> observed = service.connectionStream
        .firstWhere(
          (ConnectionStateUpdate update) =>
              update.connectionState == DeviceConnectionState.disconnected,
        );
    final Stopwatch stopwatch = Stopwatch()..start();
    fakeBle.connections.single.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    final ConnectionStateUpdate update = await observed.timeout(
      const Duration(milliseconds: 500),
    );
    stopwatch.stop();

    expect(update.deviceId, 'board-1');
    expect(stopwatch.elapsed, lessThan(const Duration(milliseconds: 500)));
    expect(service.isConnected, isFalse);

    await service.dispose();
    for (final StreamController<ConnectionStateUpdate> stream
        in fakeBle.connections) {
      if (!stream.isClosed) {
        await stream.close();
      }
    }
    for (final StreamController<List<int>> stream
        in fakeBle.notificationStreams) {
      if (!stream.isClosed) {
        await stream.close();
      }
    }
  });

  test(
    'assistance State joins the deployed Settings cache without invented old-firmware values',
    () async {
      final peer = _RecoverySharedPeer();
      final service = peer.service;
      addTearDown(service.dispose);
      void deliverBase(int revision, int brightness) {
        final bytes = peer.settingsPayload(
          revision: revision,
          brightness: brightness,
        );
        final identity = peer.settingsIdentity(bytes);
        for (
          var index = 0;
          index < SettingsStateWireCodec.fragmentCount;
          index++
        ) {
          service.debugCaptureConnectionSessionState(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.state,
              code: SettingsStateWireCodec.fragmentCode(index),
              token: 1,
              sequence: identity,
              body: Uint8List.sublistView(
                bytes,
                index * 10,
                index * 10 + SettingsStateWireCodec.fragmentLength(index),
              ),
            ),
          );
        }
      }

      void deliverAssistance(int revision, int bits) {
        final body = Uint8List.fromList(<int>[1, 0, 0, 0, 0, bits]);
        ByteData.sublistView(body).setUint32(1, revision, Endian.little);
        service.debugCaptureConnectionSessionState(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 43,
            token: 1,
            sequence: peer.settingsIdentity(body),
            body: body,
          ),
        );
      }

      deliverBase(7, 5);
      expect(service.latestSettingValue('BRIGHTNESS'), 5);
      expect(service.latestSettingValue('P1_AUTO_HINTS'), isNull);
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.confirmed,
      );
      expect(
        service
            .immediateSettingsSnapshot('board-1')
            .any(
              (setting) =>
                  setting.key == 'BRIGHTNESS' || setting.key == 'VS_CPU_LEVEL',
            ),
        isTrue,
      );
      expect(
        service
            .immediateSettingsSnapshot('board-1')
            .any((setting) => setting.key == 'P1_AUTO_HINTS'),
        isFalse,
      );
      deliverAssistance(7, 0x1f);
      expect(service.latestSettingValue('P1_AUTO_HINTS'), isTrue);
      expect(service.latestSettingValue('P2_AUTO_HINTS'), isFalse);
      deliverBase(8, 6);
      expect(service.latestSettingValue('BRIGHTNESS'), 6);
      expect(service.latestSettingValue('P1_AUTO_HINTS'), isNull);
      deliverAssistance(8, 0x2f);
      expect(service.latestSettingValue('P1_AUTO_HINTS'), isFalse);
      expect(service.latestSettingValue('P2_AUTO_HINTS'), isTrue);
      deliverBase(7, 5);
      deliverAssistance(7, 0x1f);
      expect(service.latestSettingValue('BRIGHTNESS'), 6);
      expect(service.latestSettingValue('P2_AUTO_HINTS'), isTrue);
      void deliverStartup(int revision, int choice) {
        final body = Uint8List.fromList([1, 0, 0, 0, 0, choice]);
        ByteData.sublistView(body).setUint32(1, revision, Endian.little);
        service.debugCaptureConnectionSessionState(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 44,
            token: 1,
            sequence: peer.settingsIdentity(body),
            body: body,
          ),
        );
      }

      expect(
        service
            .immediateSettingsSnapshot('board-1')
            .any((s) => s.key == 'BOOT_DISPLAY'),
        false,
      );
      deliverStartup(8, 1);
      final startup = service
          .immediateSettingsSnapshot('board-1')
          .singleWhere((s) => s.key == 'BOOT_DISPLAY');
      expect(startup.value, 'artwork');
      expect(startup.options, ['fox', 'artwork', 'off']);
      deliverBase(9, 4);
      deliverAssistance(9, 0x3f);
      expect(service.latestSettingValue('BOOT_DISPLAY'), isNull);
      deliverStartup(9, 2);
      expect(service.latestSettingValue('BOOT_DISPLAY'), 'off');
      deliverStartup(8, 0);
      expect(service.latestSettingValue('BOOT_DISPLAY'), 'off');
    },
  );

  test(
    'packed physical input publishes occupancy and all twelve sampled switches',
    () async {
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _ReconnectBle(),
      );
      final observed = service.boardPhysicalInputStateStream.first;
      final body = Uint8List(BoardPhysicalInputState.bodyBytes);
      ByteData.sublistView(body)
        ..setUint64(0, 0x8000000000000001, Endian.little)
        ..setUint16(8, (1 << 3) | (1 << 11), Endian.little);

      service.debugCaptureConnectionSessionState(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: BoardPhysicalInputState.stateCode,
          token: 1,
          sequence: 19,
          body: body,
        ),
      );

      final state = await observed;
      expect(state.occupancyMask, 0x8000000000000001);
      expect(service.latestBoardPhysicalInputState, same(state));
      expect(service.boardSwitchStates, hasLength(12));
      expect(service.boardSwitchStates[BoardSwitch.p1Right]?.isDown, isTrue);
      expect(service.boardSwitchStates[BoardSwitch.p2O]?.isDown, isTrue);
      expect(service.boardSwitchStates[BoardSwitch.p1Center], isNull);
      expect(service.boardSwitchStates[BoardSwitch.p2Center], isNull);
      await service.dispose();
    },
  );

  test(
    'shared-session switch state publishes synchronously to app observers',
    () async {
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _ReconnectBle(),
      );
      final observed = service.boardSwitchStateStream.first;
      final body = Uint8List.fromList(<int>[1, 8, 2, 1, 42, 0, 0, 0]);

      service.debugCaptureConnectionSessionState(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: BoardSwitchState.lastStateCode,
          token: 1,
          sequence: 7,
          body: body,
        ),
      );

      final state = await observed;
      expect(state.switchId, BoardSwitch.p2O);
      expect(state.isDown, isTrue);
      expect(state.revision, 7);
      expect(service.boardSwitchStates[BoardSwitch.p2O], same(state));
      await service.dispose();
    },
  );
}
