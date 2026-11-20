import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:litch_mobile/src/services/connection_game_history_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'owner_pairing_board.dart';

// Scripted external board packets only. The app collector, shared session,
// fragment assembler, grants, native writer, deadlines and report builder run
// unchanged. Native suites own actual firmware capture and auth enforcement.
class SupportSessionBoard extends OwnerPairingBoard {
  SupportSessionBoard() {
    capabilityMask = 159;
    mtu = 23;
  }
  final requests = <(int, int, int)>[];
  final nativeSupportWrites = <ConnectionSessionPacket>[];
  final Set<(int, int)> silent = {};
  Completer<void>? heldWrite;
  bool replyBeforeWriteReturns = false;
  final _seen = <(int, int)>{};
  int _token = 0, _sequence = 0, _grant = 0;
  final _pending = <(int, Uint8List)>[];
  int publicValidity = 15;
  int epochUptime = 0x100001234;
  final staleReplies = <(int, Uint8List)>[];

  @override
  Future<void> commandWrite(List<int> value) async {
    final command = jsonDecode(utf8.decode(value)) as Map;
    if (recoveryOnly && command['cmd'] == 'info') {
      emitStatus('{"i":[3,3,24,1]}');
      return;
    }
    await super.commandWrite(value);
  }

  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.opened &&
        packet.sequence == 2) {
      _token = ByteData.sublistView(packet.body).getUint32(0, Endian.little);
      _sequence = 0;
      _grant = packet.body[6];
      _pending.clear();
      _seen.clear();
    }
    if (packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.progress &&
        packet.token == _token) {
      _grant = ByteData.sublistView(packet.body).getUint32(4, Endian.little);
      _drain();
    }
    final support =
        packet.kind == ConnectionSessionKind.object &&
        {0x44, 0x45}.contains(packet.code);
    if (support) nativeSupportWrites.add(packet);
    final held = support ? heldWrite : null;
    if (held != null && !replyBeforeWriteReturns) await held.future;
    await super.sharedWrite(value);
    if (support && _seen.add((packet.token, packet.sequence))) {
      final request = ByteData.sublistView(
        packet.body,
      ).getUint32(2, Endian.little);
      final kind = packet.body[1];
      requests.add((packet.code, kind, request));
      final bytes = packet.code == 0x45
          ? publicBytes(request)
          : kind == 0
          ? historyBytes(request)
          : incidentBytes(request);
      staleReplies.add((packet.code, bytes));
      if (!silent.contains((packet.code, kind))) {
        emitSupport(packet.code, bytes);
      }
    }
    if (held != null && replyBeforeWriteReturns) await held.future;
  }

  void emitSupport(int opcode, Uint8List bytes) {
    for (final body in ConnectionGameHistoryCodec.encodeFrame(
      bytes,
      maximumBodyBytes: 10,
    )) {
      _pending.add((opcode, body));
    }
    _drain();
  }

  void _drain() {
    while (_pending.isNotEmpty && _sequence < _grant) {
      final (code, body) = _pending.removeAt(0);
      sessionPackets.add(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.object,
            code: code,
            token: _token,
            sequence: ++_sequence,
            body: body,
          ),
        ),
      );
    }
  }

  Uint8List publicBytes(int request) {
    final bytes = Uint8List(106);
    final d = ByteData.sublistView(bytes);
    bytes[0] = 2;
    bytes[1] = recoveryOnly ? publicValidity & ~9 : publicValidity;
    bytes[4] = 1;
    d.setUint16(2, 7, Endian.little);
    d.setUint16(5, 1, Endian.little);
    bytes[7] = 5;
    bytes[8] = 6;
    bytes[9] = 2;
    d.setUint64(10, epochUptime, Endian.little);
    bytes[18] = 3;
    bytes[19] = 3;
    bytes[20] = 2;
    bytes.fillRange(21, 53, 0xaa);
    final label = ascii.encode('0.9.5-support');
    bytes[53] = label.length;
    bytes.setRange(54, 54 + label.length, label);
    d.setUint32(82, request, Endian.little);
    bytes[86] = recoveryOnly ? 1 : 0;
    bytes[87] = recoveryOnly ? 1 : 0;
    bytes[88] = recoveryOnly ? 2 : 0;
    d.setUint32(90, 4, Endian.little);
    d.setUint32(94, recoveryOnly ? 3 : 0, Endian.little);
    bytes[98] = 2;
    bytes[99] = recoveryOnly ? 1 : 2;
    bytes[100] = 9;
    d.setUint32(101, 979, Endian.little);
    bytes[105] = 1;
    return bytes;
  }

  Uint8List historyBytes(int request) {
    final bytes = Uint8List(480), d = ByteData(480);
    d.setUint8(0, 1);
    d.setUint8(6, 1);
    d.setUint8(7, 20);
    d.setUint32(2, request, Endian.little);
    d.setUint32(8, 4, Endian.little);
    for (final (at, value) in [
      (12, 3),
      (20, 120000),
      (28, epochUptime),
      (36, epochUptime - 10),
      (44, 2),
      (52, 4),
      (60, 1),
    ]) {
      d.setUint64(at, value, Endian.little);
    }
    d.setUint32(68, 25, Endian.little);
    d.setUint32(72, 7, Endian.little);
    d.setUint32(76, 1, Endian.little);
    for (var i = 0; i < 20; i++) {
      final p = 80 + i * 20;
      d.setUint64(p, epochUptime - 1000 + i * 20, Endian.little);
      d.setUint32(p + 8, i + 1, Endian.little);
      d.setUint32(p + 12, 10, Endian.little);
      d.setUint8(p + 16, 2);
      d.setUint8(p + 17, 2);
      d.setUint8(p + 18, 4);
    }
    bytes.setAll(0, d.buffer.asUint8List());
    return bytes;
  }

  Uint8List incidentBytes(int request) {
    final bytes = Uint8List(136), d = ByteData(136);
    d.setUint8(0, 1);
    d.setUint8(1, 1);
    d.setUint32(2, request, Endian.little);
    d.setUint32(6, 5, Endian.little);
    d.setUint32(10, 2, Endian.little);
    d.setUint32(14, 1, Endian.little);
    d.setUint8(18, 1);
    d.setUint32(127, 7, Endian.little);
    d.setUint32(131, 1, Endian.little);
    bytes.setAll(0, d.buffer.asUint8List());
    return bytes;
  }
}
