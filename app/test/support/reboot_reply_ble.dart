import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_status_codec.dart';

// Only the SDK/board replies are simulated. Connection lifetime, subscription
// suspension, upload completion and acceptance remain production service paths.
class RebootReplyBle implements FlutterReactiveBle {
  final packets = StreamController<List<int>>.broadcast(sync: true);
  final connections = <StreamController<ConnectionStateUpdate>>[];
  final sessionPackets = StreamController<List<int>>.broadcast(sync: true);
  final controls = ConnectionOtaStatusAssembler();
  int bootNonce = 2;
  bool supportsSession = true;
  bool recoveryOnly = false;
  String runningDigest = 'a' * 64;
  String runningVersion = '0.8';
  int runningSlot = 0;
  int selectedSlot = 0;
  int runningState = 2;
  int selectedSector = 0;
  int selectedSequence = 23;
  final selectorSequences = <int>[23, 22];
  final selectorStates = <int>[2, 2];
  final selectorCrcValid = <int>[1, 1];
  int admittedTargetSequence = 24;
  int admissionAttempt = 0;
  int admissionPhase = 0;
  int admissionSourceSlot = 255;
  int admissionTargetSlot = 255;
  int admissionSourceSequence = 0;
  int admissionExpectedSequence = 0;
  int runtimeAccepted = 1;
  int proofRequests = 0;
  final proofRequestIds = <int>[];
  final imageBytesAtProof = <int>[];
  final admissionPhasesAtProof = <int>[];
  bool replyToProof = true;
  Map<String, Object?> Function(Map<String, Object?>)? transformProof;
  void Function(int request)? onProofRequest;
  bool digestAvailable = true;
  String? beginReply = '{"b":1}';
  bool holdBeginAdmission = false;
  Completer<void>? beginWorkGate;
  final beginWorkEntered = Completer<void>();
  int _acceptedObjectSequence = 0;
  int _orderedReceivedThrough = 0;
  int _orderedConfirmedThrough = 0;
  int _orderedCapacity = 8;
  final _orderedRejections = <int, int>{};
  int _beginAdmissionSequence = 0;
  bool disconnectBeforeBeginReply = false;
  bool disconnectBeforeFinalReply = false;
  final uploadCommands = <String>[];
  int imageWrites = 0;
  int mtu = 247;
  bool failMtuRequest = false;
  int mtuRequests = 0;
  final nativeWriteLengths = <int>[];
  String? firstOversizeNativeWrite;
  final imageChunkLengths = <int>[];
  int receiveAckInterval = 1;
  int lastRxAcknowledgement = 0;
  int maximumUnacknowledgedBytes = 0;
  int statusSequence = 0;
  bool sessionActivated = false;
  int otaSubscriptions = 0;
  int received = 0;
  int expectedImageBytes = 0;
  bool dropFirstVersionReply = true;
  String? acceptanceReply = '{"ov":"v"}';
  int versionRequests = 0;
  int digestRequests = 0;
  int acceptanceRequests = 0;
  final firstAcceptanceRequest = Completer<void>();

  // Native connect alone never simulates activation or a board reboot.
  void activateAcceptedTarget({String? digest, String? version}) {
    assert(admissionPhase != 0);
    runningSlot = admissionTargetSlot;
    selectedSlot = runningSlot;
    runningState = 2;
    selectedSector = 1 - selectedSector;
    selectedSequence = admissionExpectedSequence;
    selectorSequences[selectedSector] = selectedSequence;
    selectorStates[selectedSector] = 2;
    ++bootNonce;
    runtimeAccepted = 1;
    if (digest != null) runningDigest = digest;
    if (version != null) runningVersion = version;
    admittedTargetSequence = selectedSequence + 1;
    _clearAdmission();
  }

  void failTarget({bool reboot = true, int state = 4}) {
    assert(admissionPhase != 0);
    selectorSequences[1 - selectedSector] = admissionExpectedSequence;
    selectorStates[1 - selectedSector] = state;
    if (reboot) {
      ++bootNonce;
      _clearAdmission();
    } else {
      admissionPhase = 4;
    }
  }

  void _clearAdmission() {
    admissionAttempt = 0;
    admissionPhase = 0;
    admissionSourceSlot = 255;
    admissionTargetSlot = 255;
    admissionSourceSequence = 0;
    admissionExpectedSequence = 0;
  }

  Map<String, Object?> attemptProof(int request) => {
    'op': 1,
    'q': request,
    'n': bootNonce.toRadixString(16).padLeft(16, '0'),
    'v': runtimeAccepted,
    's': [
      11,
      0,
      runningSlot,
      selectedSlot,
      selectedSector,
      selectedSequence,
      runningState,
      selectorSequences[0],
      selectorStates[0],
      selectorCrcValid[0],
      selectorSequences[1],
      selectorStates[1],
      selectorCrcValid[1],
    ],
    'a': [
      admissionAttempt,
      admissionPhase,
      admissionSourceSlot,
      admissionTargetSlot,
      admissionSourceSequence,
      admissionExpectedSequence,
    ],
  };

  @override
  Stream<List<int>> subscribeToCharacteristic(QualifiedCharacteristic c) {
    if (c.characteristicId == BleProtocol.otaControlCharacteristic) {
      ++otaSubscriptions;
    }
    if (c.characteristicId ==
        BleProtocol.connectionSessionEgressCharacteristic) {
      return sessionPackets.stream;
    }
    return packets.stream;
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    final connection = StreamController<ConnectionStateUpdate>.broadcast(
      sync: true,
    );
    sessionActivated = false;
    statusSequence = 0;
    controls.clear();
    connections.add(connection);
    return connection.stream;
  }

  @override
  Future<List<int>> readCharacteristic(QualifiedCharacteristic c) async =>
      utf8.encode('[]');

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    nativeWriteLengths.add(value.length);
    if (value.length > mtu - 3) {
      firstOversizeNativeWrite ??=
          '${c.characteristicId}: ${value.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join()}';
    }
    if (c.characteristicId ==
        BleProtocol.connectionSessionIngressCharacteristic) {
      await sharedWrite(Uint8List.fromList(value));
      return;
    }
    if (c.characteristicId == BleProtocol.otaDataCharacteristic) {
      _acceptImageChunk(value);
      return;
    }
    if (c.characteristicId != BleProtocol.otaControlCharacteristic) return;
    await commandWrite(value);
  }

  void emitStatus(String json) {
    scheduleMicrotask(() {
      if (!sessionActivated) {
        packets.add(utf8.encode(json));
        return;
      }
      for (final body in ConnectionOtaCodec.encodeControl(
        Uint8List.fromList(utf8.encode(json)),
        maximumBodyBytes: 10,
      )) {
        sessionPackets.add(
          ConnectionSessionCodec.encode(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.state,
              code: 0x32,
              token: 3,
              sequence: ++statusSequence,
              body: body,
            ),
          ),
        );
      }
    });
  }

  void control(int code, int token, int sequence, Uint8List body) {
    sessionPackets.add(
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

  void progress(int token, int sequence) {
    final body = Uint8List(10);
    ByteData.sublistView(body)
      ..setUint32(0, sequence, Endian.little)
      ..setUint32(
        4,
        sequence + (token == 3 ? ConnectionSessionCodec.maximumWindow : 8),
        Endian.little,
      );
    control(ConnectionSessionControl.progress, token, sequence, body);
  }

  // Scripted domains can reuse this existing receiver lifecycle. Default
  // behavior matches production's unknown-op dispatcher; terminal evidence
  // remains retained within the negotiated window until cumulative Confirm.
  Future<void> onAdmittedObject(ConnectionSessionPacket packet) async {}

  int orderedRejection(ConnectionSessionPacket packet) => 4;

  int get sessionFeatures =>
      ConnectionSession.orderedFeature |
      ConnectionSession.stateFeature |
      ConnectionSession.objectFeature |
      ConnectionSession.reverseObjectFeature;

  void _orderedProgress() {
    final body = Uint8List(10);
    ByteData.sublistView(body)
      ..setUint32(0, _orderedReceivedThrough, Endian.little)
      ..setUint32(
        4,
        _orderedConfirmedThrough + _orderedCapacity,
        Endian.little,
      );
    control(
      ConnectionSessionControl.progress,
      2,
      _orderedReceivedThrough,
      body,
    );
  }

  void _orderedOutcome(int sequence) {
    if (sequence <= _orderedConfirmedThrough ||
        sequence > _orderedReceivedThrough) {
      return;
    }
    final rejection = _orderedRejections[sequence] ?? 4;
    if (rejection == 0) return;
    control(
      ConnectionSessionControl.outcome,
      2,
      sequence,
      Uint8List.fromList([1, rejection & 255, rejection >> 8]),
    );
  }

  void _orderedCheckpoint() {
    final retained = _orderedReceivedThrough - _orderedConfirmedThrough;
    if (retained == 0) return;
    final checkpoint = Uint8List(10);
    var rejected = 0;
    for (var offset = 0; offset < retained; ++offset) {
      if ((_orderedRejections[_orderedConfirmedThrough + offset + 1] ?? 4) !=
          0) {
        rejected |= 1 << offset;
      }
    }
    ByteData.sublistView(checkpoint).setUint16(8, rejected, Endian.little);
    control(
      ConnectionSessionControl.checkpoint,
      2,
      _orderedReceivedThrough,
      checkpoint,
    );
  }

  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.object) {
      if (packet.sequence <= _acceptedObjectSequence) {
        if (!holdBeginAdmission ||
            _acceptedObjectSequence != _beginAdmissionSequence) {
          progress(packet.token, _acceptedObjectSequence);
        }
        return;
      }
      if (packet.sequence != _acceptedObjectSequence + 1) {
        progress(packet.token, _acceptedObjectSequence);
        return;
      }
      final command = packet.code == ConnectionOtaCodec.controlObjectOpcode
          ? controls.accept(packet.body)
          : null;
      final isBegin =
          command != null &&
          (jsonDecode(utf8.decode(command)) as Map<String, dynamic>)['cmd'] ==
              'ota_begin';
      _acceptedObjectSequence = packet.sequence;
      if (isBegin) _beginAdmissionSequence = packet.sequence;
      // Model retained receiver admission independently from domain execution.
      // Native BLE contract tests cover the real queue/notification handoff.
      if (!isBegin || !holdBeginAdmission) {
        progress(packet.token, _acceptedObjectSequence);
      }
      if (packet.code == ConnectionOtaCodec.dataObjectOpcode) {
        _acceptImageChunk(packet.body);
      } else if (command != null) {
        if (isBegin && beginWorkGate != null) {
          final gate = beginWorkGate!;
          if (!beginWorkEntered.isCompleted) beginWorkEntered.complete();
          unawaited(gate.future.then((_) => commandWrite(command)));
        } else {
          await commandWrite(command);
        }
      }
      await onAdmittedObject(packet);
      return;
    }
    if (packet.kind == ConnectionSessionKind.ordered) {
      if (packet.token != 2) return;
      if (packet.sequence == _orderedReceivedThrough + 1 &&
          packet.sequence <= _orderedConfirmedThrough + _orderedCapacity) {
        _orderedReceivedThrough = packet.sequence;
        _orderedRejections[packet.sequence] = orderedRejection(packet);
        _orderedProgress();
        _orderedOutcome(packet.sequence);
        _orderedCheckpoint();
      } else {
        // Duplicate ingress only replays admission. Query recovers terminal
        // evidence without executing the unsupported operation again.
        _orderedProgress();
      }
      return;
    }
    if (packet.kind != ConnectionSessionKind.control) return;
    switch (packet.code) {
      case ConnectionSessionControl.confirm:
        if (packet.token == 2 &&
            packet.sequence > _orderedConfirmedThrough &&
            packet.sequence <= _orderedReceivedThrough) {
          _orderedConfirmedThrough = packet.sequence;
          _orderedRejections.removeWhere(
            (sequence, _) => sequence <= packet.sequence,
          );
          _orderedProgress();
        }
      case ConnectionSessionControl.query:
        if (packet.token == 2) {
          _orderedProgress();
          if (packet.sequence == 0) {
            _orderedCheckpoint();
          } else {
            _orderedOutcome(packet.sequence);
          }
        }
        if (packet.token == 3 &&
            packet.sequence == 0 &&
            (!holdBeginAdmission ||
                _acceptedObjectSequence != _beginAdmissionSequence)) {
          progress(3, _acceptedObjectSequence);
        }
      case ConnectionSessionControl.hello:
        control(ConnectionSessionControl.welcome, 1, 0, packet.body);
        final identity = Uint8List(10);
        ByteData.sublistView(identity)
          ..setUint64(0, bootNonce, Endian.little)
          ..setUint16(8, sessionFeatures, Endian.little);
        control(ConnectionSessionControl.identity, 1, 0, identity);
      case ConnectionSessionControl.ready:
        _acceptedObjectSequence = 0;
        _beginAdmissionSequence = 0;
        sessionActivated = true;
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
        if (packet.sequence == 1) {
          _orderedReceivedThrough = 0;
          _orderedConfirmedThrough = 0;
          _orderedCapacity = packet.body[3].clamp(1, 8);
          _orderedRejections.clear();
        }
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
        if (packet.sequence == 1) {
          _orderedProgress();
        } else {
          progress(3, 0);
        }
    }
  }

  Future<void> commandWrite(List<int> value) async {
    final command = jsonDecode(utf8.decode(value)) as Map<String, dynamic>;
    if (['ota_begin', 'ota_end', 'ota_abort'].contains(command['cmd'])) {
      uploadCommands.add(command['cmd'] as String);
    }
    if (command['cmd'] == 'info') {
      emitStatus(
        recoveryOnly
            ? '{"i":[3,3,24,1]}'
            : '{"i":[${supportsSession ? 3 : 1},3,31]}',
      );
    } else if (command['cmd'] == 'ota_begin') {
      received = 0;
      lastRxAcknowledgement = 0;
      maximumUnacknowledgedBytes = 0;
      expectedImageBytes = command['size'] as int;
      if (disconnectBeforeBeginReply) {
        // The transport accepts the command before the link ends. No OTA
        // acceptance or image bytes have crossed the board transaction seam.
        Timer.run(() {
          connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
        });
      } else if (beginReply case final String reply) {
        final begin = jsonDecode(reply) as Map<String, dynamic>;
        if (begin['b'] == 1 || begin['st'] == 'begin') {
          ++admissionAttempt;
          admissionPhase = 1;
          admissionSourceSlot = runningSlot;
          admissionTargetSlot = 1 - runningSlot;
          admissionSourceSequence = selectedSequence;
          admissionExpectedSequence = admittedTargetSequence;
        }
        emitStatus(reply);
      }
    } else if (command['cmd'] == 'ota_end') {
      if (disconnectBeforeFinalReply) {
        // Let the SDK acknowledge the command write, then lose the link before
        // any finalization reply. No reboot or activation is simulated.
        Timer.run(() {
          connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
        });
        return;
      }
      emitStatus('{"d":1}');
    } else if (command['cmd'] == 'ota_abort') {
      if (admissionPhase != 0) admissionPhase = 3;
    } else if (command['cmd'] == 'fw_version') {
      if (++versionRequests > 1 || !dropFirstVersionReply) {
        emitStatus(jsonEncode({'v': runningVersion}));
      }
    } else if (command['cmd'] == 'fw_digest') {
      ++digestRequests;
      if (!digestAvailable) {
        emitStatus('{"h":"e"}');
        return;
      }
      emitStatus(
        jsonEncode({
          for (int i = 0; i < 8; ++i)
            'h$i': runningDigest.substring(i * 8, (i + 1) * 8),
          'h': 'r',
        }),
      );
    } else if (command['cmd'] == 'ota_state') {
      if (command['proof'] == 1) {
        ++proofRequests;
        final request = command['q'] as int;
        proofRequestIds.add(request);
        imageBytesAtProof.add(received);
        admissionPhasesAtProof.add(admissionPhase);
        onProofRequest?.call(request);
        if (replyToProof) {
          final proof = attemptProof(request);
          emitStatus(jsonEncode(transformProof?.call(proof) ?? proof));
        }
        return;
      }
      ++acceptanceRequests;
      if (!firstAcceptanceRequest.isCompleted) {
        firstAcceptanceRequest.complete();
      }
      final reply = acceptanceReply;
      if (reply != null) {
        emitStatus(reply);
      }
    }
  }

  void _acceptImageChunk(List<int> bytes) {
    ++imageWrites;
    imageChunkLengths.add(bytes.length);
    received += bytes.length;
    final outstanding = received - lastRxAcknowledgement;
    if (outstanding > maximumUnacknowledgedBytes) {
      maximumUnacknowledgedBytes = outstanding;
    }
    if (outstanding >= receiveAckInterval || received == expectedImageBytes) {
      lastRxAcknowledgement = received;
      emitStatus('{"r":$received}');
    }
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) => writeCharacteristicWithResponse(c, value: value);

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async {
    ++mtuRequests;
    if (failMtuRequest) throw StateError('Controlled unavailable MTU');
    return this.mtu;
  }

  @override
  Future<void> requestConnectionPriority({
    required String deviceId,
    required ConnectionPriority priority,
  }) async {}

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

Future<void> connectBoard(
  LitchBleService service,
  RebootReplyBle ble, {
  String deviceId = 'board-1',
}) async {
  final index = ble.connections.length;
  final connecting = service.connect(deviceId);
  for (int turn = 0; turn < 50 && ble.connections.length == index; ++turn) {
    await Future<void>.delayed(Duration.zero);
  }
  expect(ble.connections.length, index + 1);
  ble.connections[index].add(
    ConnectionStateUpdate(
      deviceId: deviceId,
      connectionState: DeviceConnectionState.connected,
      failure: null,
    ),
  );
  expect(await connecting, isTrue);
}

Future<void> disposeBoard(LitchBleService service, RebootReplyBle ble) async {
  await service.dispose();
  for (final connection in ble.connections) {
    await connection.close();
  }
  await ble.packets.close();
  await ble.sessionPackets.close();
}
