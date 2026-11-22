import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/connection_game_history_codec.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';

import 'reboot_reply_ble.dart';

/// Controlled board responses around the actual shared session and BLE service.
/// Does not simulate native security or durable flash; tests publish that proof.
class OwnerPairingBoard extends RebootReplyBle {
  final protectedReads = <QualifiedCharacteristic>[];
  final subscriptions = <QualifiedCharacteristic>[];
  int historyRequests = 0;
  bool returnAbsentHistory = false;
  final _historyRequest = ConnectionGameHistoryAssembler();
  int _reverseObjectToken = 0;
  int _reverseObjectSequence = 0;
  void Function(int code)? onProductQuery;
  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    subscriptions.add(characteristic);
    return super.subscribeToCharacteristic(characteristic);
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    if (characteristic.characteristicId == BleProtocol.commandCharacteristic &&
        value.isNotEmpty &&
        value.first == 123) {
      final command = jsonDecode(utf8.decode(value)) as Map;
      if (command['cmd'] == 'identify' || command['cmd'] == 'identify_stop') {
        directIdentifyCommands.add(command);
      }
    }
    await super.writeCharacteristicWithResponse(characteristic, value: value);
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    protectedReads.add(characteristic);
    return super.readCharacteristic(characteristic);
  }

  @override
  void emitStatus(String json) {
    scheduleMicrotask(() {
      if (packets.isClosed || sessionPackets.isClosed) return;
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

  Completer<void>? capabilityReplyGate;
  int state = 1;
  int reason = 0;
  int flags = 4;
  int attempt = 0;
  int physicalEnd = 0;
  int revision = 1;
  final identity = List<int>.generate(16, (index) => index + 1);
  final pairingCommands = <ConnectionSessionPacket>[];
  final identifyCommands = <ConnectionSessionPacket>[];
  int identifyRejection = 0;
  bool typedIdentifySupported = true;
  int capabilityMask = 671;
  final directIdentifyCommands = <Map>[];
  final productQueries = <int>[];
  final stateQueries = <int>[];
  int heartbeats = 0;
  bool includeIdentity = true;
  void Function(int code)? onStateQuery;
  bool publishAfterBegin = true;
  int stateToken = 0x80000001;

  @override
  Future<void> commandWrite(List<int> value) async {
    final command = jsonDecode(utf8.decode(value)) as Map;
    if (command['cmd'] == 'info') {
      await capabilityReplyGate?.future;
      emitStatus('{"i":[3,3,$capabilityMask]}');
      return;
    }
    await super.commandWrite(value);
  }

  void publish({int? token}) {
    emitState(40, [
      1,
      state,
      reason,
      flags,
      attempt & 255,
      attempt >> 8,
      physicalEnd,
      0,
    ], token: token);
    if (includeIdentity) {
      // Match production publication, including unavailable identity halves.
      final publishedIdentity = (flags & 4) != 0
          ? identity
          : List<int>.filled(16, 0);
      emitState(41, publishedIdentity.sublist(0, 8), token: token);
      emitState(42, publishedIdentity.sublist(8), token: token);
    }
  }

  void emitState(int code, List<int> body, {int? token, int? sequence}) {
    sessionPackets.add(
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: code,
          token: token ?? stateToken,
          sequence: sequence ?? revision,
          body: Uint8List.fromList(body),
        ),
      ),
    );
  }

  void setState(int next, {int? nextFlags, int nextReason = 0}) {
    state = next;
    if (state != 2 && physicalEnd == 2) physicalEnd = 0;
    flags = nextFlags ?? flags;
    reason = nextReason;
    revision++;
    publish();
  }

  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.heartbeat) {
      ++heartbeats;
    }
    if (packet.kind == ConnectionSessionKind.object && packet.code == 0x30) {
      historyRequests++;
      final frame = _historyRequest.accept(packet.body);
      await super.sharedWrite(value);
      if (returnAbsentHistory && frame != null) {
        final request = GameHistoryFrame.decode(frame);
        if (request.opcode == CurrentGameSyncOpcode.begin.wireValue) {
          _emitHistory(
            request,
            CurrentGameSyncOpcode.state,
            1,
            const CurrentGameDigest.absent().encode(allowAbsent: true),
          );
          _emitHistory(
            request,
            CurrentGameSyncOpcode.bounds,
            2,
            const CurrentGameBounds.absent().encode(),
          );
        }
      }
      return;
    }
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.opened &&
        packet.sequence == 2) {
      _reverseObjectToken = ByteData.sublistView(
        packet.body,
      ).getUint32(0, Endian.little);
      _reverseObjectSequence = 0;
    }
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.opened &&
        ByteData.sublistView(packet.body).getUint32(0, Endian.little) ==
            stateToken) {
      stateToken = ByteData.sublistView(
        packet.body,
      ).getUint32(0, Endian.little);
    }
    if (packet.kind == ConnectionSessionKind.ordered && packet.code == 28) {
      identifyCommands.add(packet);
      progress(packet.token, packet.sequence);
      final rejection = !typedIdentifySupported
          ? 4
          : packet.body[1] == 1
          ? identifyRejection
          : 0;
      final rejected = rejection != 0;
      if (rejected) {
        control(
          ConnectionSessionControl.outcome,
          packet.token,
          packet.sequence,
          Uint8List.fromList([1, rejection & 255, rejection >> 8]),
        );
      }
      final checkpoint = Uint8List(10);
      ByteData.sublistView(
        checkpoint,
      ).setUint16(8, rejected ? 1 : 0, Endian.little);
      control(
        ConnectionSessionControl.checkpoint,
        packet.token,
        packet.sequence,
        checkpoint,
      );
      return;
    }
    if (packet.kind == ConnectionSessionKind.ordered && packet.code == 27) {
      pairingCommands.add(packet);
      attempt = packet.body[2] | packet.body[3] << 8;
      if (packet.body[1] == 1 || packet.body[1] == 3) {
        state = 2;
        flags &= ~8;
        physicalEnd = packet.body[4];
      } else {
        state = 1;
        flags &= ~16;
        physicalEnd = 0;
        reason = 0;
        attempt = 0;
      }
      revision++;
      progress(packet.token, packet.sequence);
      control(
        ConnectionSessionControl.checkpoint,
        packet.token,
        packet.sequence,
        Uint8List(10),
      );
      if (publishAfterBegin) publish();
      return;
    }
    if (packet.kind == ConnectionSessionKind.ordered && packet.code == 26) {
      // Existing Console bridge bootstrap is admitted by this board fixture;
      // command output is outside these controlled ownership tests.
      progress(packet.token, packet.sequence);
      control(
        ConnectionSessionControl.checkpoint,
        packet.token,
        packet.sequence,
        Uint8List(10),
      );
      return;
    }
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.query &&
        packet.token == stateToken) {
      stateQueries.add(packet.sequence);
      onStateQuery?.call(packet.sequence);
      if ([40, 41, 42].contains(packet.sequence)) {
        publish();
      } else {
        productQueries.add(packet.sequence);
        onProductQuery?.call(packet.sequence);
      }
      return;
    }
    await super.sharedWrite(value);
  }

  void _emitHistory(
    GameHistoryFrame request,
    CurrentGameSyncOpcode opcode,
    int sequence,
    Uint8List payload,
  ) {
    final frame = GameHistoryFrame(
      opcode: opcode.wireValue,
      transactionId: request.transactionId,
      frameSequence: sequence,
      payload: payload,
    ).encode();
    for (final body in ConnectionGameHistoryCodec.encodeFrame(
      frame,
      maximumBodyBytes: 10,
    )) {
      sessionPackets.add(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.object,
            code: ConnectionGameHistoryCodec.receiptCode,
            token: _reverseObjectToken,
            sequence: ++_reverseObjectSequence,
            body: body,
          ),
        ),
      );
    }
  }
}
