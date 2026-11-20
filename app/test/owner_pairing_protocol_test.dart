import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/owner_pairing_protocol.dart';

void main() {
  const end0 = <int>[1, 0x82, 0x34, 0x12, 0, 1, 4, 0, 0x20, 0, 0, 2, 0x80];
  const end1 = <int>[1, 0x82, 0x34, 0x12, 1, 0x80, 2, 0, 0, 0x20, 0, 4, 1];
  test('reserved endpoint is distinct from shipped Litchcraft', () {
    expect(
      ownerPairingCharacteristicUuid,
      '6f2a20d1-89df-4ac6-a9f2-53f6f7db0107',
    );
    expect(
      BleProtocol.ownerPairingCharacteristic,
      isNot(BleProtocol.litchcraftCharacteristic),
    );
  });
  test('goldens cover every frame and physical end', () {
    expect(
      File('../test/fixtures/owner_pairing_v1.txt').readAsLinesSync(),
      <String>[
        'pattern_end0=01823412000104002000000280',
        'pattern_end1=01823412018002000020000401',
      ],
    );
    for (final v in [
      <int>[1, 1],
      <int>[1, 2],
      <int>[1, 3, 0x34, 0x12],
      <int>[1, 4, 0x34, 0x12],
      end0,
      end1,
      <int>[1, 0x81, 1, 8, ...List.filled(15, 0), 1],
      <int>[1, 0x83, 0x34, 0x12, 1, 0],
      <int>[1, 0xff, 2, 1],
    ]) {
      final f = OwnerPairingFrame.decode(v);
      expect(f, isNotNull, reason: '$v');
      expect(f!.encode(), v);
    }
    expect(
      OwnerPairingFrame.decode(end0)!.mask,
      BigInt.parse('8002000020000401', radix: 16),
    );
    expect(
      OwnerPairingFrame.decode(end1)!.mask,
      BigInt.parse('0104002000000280', radix: 16),
    );
  });
  test('all state and reason ordinals are frozen', () {
    for (final s in OwnerPairingState.values) {
      final r = s == OwnerPairingState.failed
          ? OwnerPairingReason.malformed
          : OwnerPairingReason.none;
      expect(
        OwnerPairingFrame(0x83, session: 1, state: s, reason: r).encode()![4],
        s.index,
      );
    }
    for (final r in OwnerPairingReason.values.skip(1)) {
      expect(
        OwnerPairingFrame(0xff, requestOpcode: 2, reason: r).encode()![3],
        r.index,
      );
    }
  });
  test('bounds and dependencies fail closed without exceptions', () {
    for (final v in [
      <int>[-1, 1],
      <int>[256, 1],
      <int>[2, 1],
      <int>[1, 9],
      <int>[1, 3, 0, 0],
      <int>[1, 0x81, 1, 16, ...List.filled(16, 0)],
      <int>[1, 0x81, 1, 8, ...List.filled(16, 0)],
      <int>[1, 0x83, 1, 0, 11, 0],
      <int>[1, 0xff, 2, 0],
      end0.sublist(0, 12),
    ]) {
      expect(() => OwnerPairingFrame.decode(v), returnsNormally);
      expect(OwnerPairingFrame.decode(v), isNull);
    }
    for (final f in [
      OwnerPairingFrame(3, session: -1),
      OwnerPairingFrame(3, session: 0x10000),
      OwnerPairingFrame(0x82, session: 1, viewEnd: 2, mask: BigInt.from(31)),
      OwnerPairingFrame(0x82, session: 1, mask: BigInt.from(-1)),
      OwnerPairingFrame(
        0xff,
        requestOpcode: 256,
        reason: OwnerPairingReason.busy,
      ),
      OwnerPairingFrame(
        0x81,
        state: OwnerPairingState.unpaired,
        identity: [...List.filled(15, 0), 256],
      ),
    ]) {
      expect(() => f.encode(), returnsNormally);
      expect(f.encode(), isNull);
    }
  });
}
