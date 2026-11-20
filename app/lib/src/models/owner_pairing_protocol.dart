import 'dart:typed_data';

const String ownerPairingCharacteristicUuid =
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0107';

enum OwnerPairingState {
  unavailable,
  unpaired,
  physicalReady,
  pattern,
  awaitHold,
  securing,
  persistingOwner,
  ownerReady,
  ownedOther,
  clearing,
  failed,
}

enum OwnerPairingReason {
  none,
  malformed,
  unsupportedVersion,
  staleSession,
  physicalActionRequired,
  busy,
  mismatch,
  timeout,
  wrongConnection,
  securityFailed,
  storageFailed,
  bondDeleteFailed,
}

class OwnerPairingFrame {
  OwnerPairingFrame(
    this.opcode, {
    this.session = 0,
    this.state,
    this.reason,
    this.flags = 0,
    this.identity = const <int>[],
    this.viewEnd = 0,
    BigInt? mask,
    this.requestOpcode = 0,
  }) : mask = mask ?? BigInt.zero;
  final int opcode, session, flags, viewEnd, requestOpcode;
  final BigInt mask;
  final OwnerPairingState? state;
  final OwnerPairingReason? reason;
  final List<int> identity;
  static bool _byte(int v) => v >= 0 && v <= 255;
  static int _bits(BigInt v) {
    int n = 0;
    while (v > BigInt.zero) {
      n += (v & BigInt.one).toInt();
      v >>= 1;
    }
    return n;
  }

  static BigInt _rot(BigInt v) {
    var r = BigInt.zero;
    for (int i = 0; i < 64; i++) {
      r |= ((v >> i) & BigInt.one) << (63 - i);
    }
    return r;
  }

  static bool _status(OwnerPairingFrame f) {
    final nz = f.identity.any((b) => b != 0);
    return f.state != null &&
        f.flags >= 0 &&
        f.flags <= 15 &&
        f.identity.length == 16 &&
        f.identity.every(_byte) &&
        nz == ((f.flags & 8) != 0) &&
        ((f.flags & 2) == 0 ||
            ((f.flags & 1) != 0 && f.state == OwnerPairingState.ownerReady));
  }

  Uint8List? encode() {
    if (!_byte(opcode) ||
        session < 0 ||
        session > 0xffff ||
        viewEnd < 0 ||
        viewEnd > 1 ||
        flags < 0 ||
        flags > 255 ||
        requestOpcode < 0 ||
        requestOpcode > 255 ||
        mask < BigInt.zero ||
        mask > ((BigInt.one << 64) - BigInt.one)) {
      return null;
    }
    final n = switch (opcode) {
      1 || 2 => 2,
      3 || 4 || 0xff => 4,
      0x81 => 20,
      0x82 => 13,
      0x83 => 6,
      _ => 0,
    };
    if (n == 0) return null;
    final out = Uint8List(n), d = ByteData.sublistView(out);
    out[0] = 1;
    out[1] = opcode;
    if (opcode == 3 || opcode == 4) {
      if (session == 0) return null;
      d.setUint16(2, session, Endian.little);
    }
    if (opcode == 0x81) {
      if (!_status(this)) return null;
      out[2] = state!.index;
      out[3] = flags;
      out.setRange(4, 20, identity);
    }
    if (opcode == 0x82) {
      if (session == 0 || _bits(mask) != 5 || _rot(mask) == mask) return null;
      d.setUint16(2, session, Endian.little);
      out[4] = viewEnd;
      for (int i = 0; i < 8; i++) {
        out[5 + i] = ((mask >> (8 * i)) & BigInt.from(255)).toInt();
      }
    }
    if (opcode == 0x83) {
      if (session == 0 || state == null || reason == null) return null;
      d.setUint16(2, session, Endian.little);
      out[4] = state!.index;
      out[5] = reason!.index;
    }
    if (opcode == 0xff) {
      if (requestOpcode < 1 ||
          requestOpcode > 4 ||
          reason == null ||
          reason == OwnerPairingReason.none) {
        return null;
      }
      out[2] = requestOpcode;
      out[3] = reason!.index;
    }
    return out;
  }

  static OwnerPairingFrame? decode(List<int> s) {
    if (s.any((v) => !_byte(v)) || s.length < 2 || s[0] != 1) return null;
    final b = Uint8List.fromList(s), d = ByteData.sublistView(b), op = b[1];
    if ((op == 1 || op == 2) && b.length == 2) return OwnerPairingFrame(op);
    if ((op == 3 || op == 4) && b.length == 4) {
      final x = d.getUint16(2, Endian.little);
      return x == 0 ? null : OwnerPairingFrame(op, session: x);
    }
    if (op == 0x81 &&
        b.length == 20 &&
        b[2] < OwnerPairingState.values.length) {
      final f = OwnerPairingFrame(
        op,
        state: OwnerPairingState.values[b[2]],
        flags: b[3],
        identity: b.sublist(4),
      );
      return _status(f) ? f : null;
    }
    if (op == 0x82 && b.length == 13) {
      final x = d.getUint16(2, Endian.little);
      var m = BigInt.zero;
      for (int i = 0; i < 8; i++) {
        m |= BigInt.from(b[5 + i]) << (8 * i);
      }
      final f = OwnerPairingFrame(op, session: x, viewEnd: b[4], mask: m);
      return f.encode() == null ? null : f;
    }
    if (op == 0x83 &&
        b.length == 6 &&
        b[4] < OwnerPairingState.values.length &&
        b[5] < OwnerPairingReason.values.length) {
      final f = OwnerPairingFrame(
        op,
        session: d.getUint16(2, Endian.little),
        state: OwnerPairingState.values[b[4]],
        reason: OwnerPairingReason.values[b[5]],
      );
      return f.encode() == null ? null : f;
    }
    if (op == 0xff &&
        b.length == 4 &&
        b[3] < OwnerPairingReason.values.length) {
      final f = OwnerPairingFrame(
        op,
        requestOpcode: b[2],
        reason: OwnerPairingReason.values[b[3]],
      );
      return f.encode() == null ? null : f;
    }
    return null;
  }
}
