import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/home_state_action.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';

import 'owner_pairing_board.dart';

// Immutable metadata is still read from the registered Settings endpoint.
// Live values cross the actual mobile shared State adapter and its CRC identity.
class SettingsSessionBoard extends OwnerPairingBoard {
  SettingsSessionBoard({bool includeHomeSnapshot = false}) {
    capabilityMask = LitchDeviceCapabilities.settingsBit;
    onProductQuery = (_) {
      if (includeHomeSnapshot) publishHome();
      publishSettings();
    };
  }
  int enumerationReads = 0;
  int settingsRevision = 14;
  int firstPlayer = 2;
  int wheel = 40;
  void Function(ByteData data)? configureSettings;

  static Uint8List _fixture(String filename, String name) {
    final row = File('../test/fixtures/$filename')
        .readAsLinesSync()
        .firstWhere((line) => line.startsWith('$name,'));
    final hex = row.substring(row.indexOf(',') + 1);
    return Uint8List.fromList([
      for (var i = 0; i < hex.length; i += 2)
        int.parse(hex.substring(i, i + 2), radix: 16),
    ]);
  }

  void publishHome() {
    // Keep the firmware corpus's starting FEN and occupancy, with idle play
    // and no pending move, CPU calculation or assistance overlays.
    final bytes = _fixture('home_state_wire_v1.csv', 'representative')
      ..[1] = HomeStateAction.idle.index
      ..fillRange(6, 11, 0)
      ..fillRange(62, 71, 0xff)
      ..fillRange(71, HomeStateWireCodec.payloadBytes, 0);
    for (var index = 0; index < HomeStateWireCodec.fragmentCount; ++index) {
      final offset = index * HomeStateWireCodec.fragmentBytes;
      emitState(
        HomeStateWireCodec.firstStateCode + index,
        bytes.sublist(offset, offset + HomeStateWireCodec.fragmentBytes),
      );
    }
  }

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    if (characteristic.characteristicId ==
        BleProtocol.settingsEnumerateCharacteristic) {
      enumerationReads++;
      return utf8.encode(
        jsonEncode([
          {'i': 0, 'k': 'GAME_FIRST_PLAYER', 't': 'u32', 'v': 1},
          {'i': 1, 'k': 'P1_PIECE_WHEEL', 't': 'u32', 'v': 12},
        ]),
      );
    }
    return super.readCharacteristic(characteristic);
  }

  void publishSettings() {
    for (final packet in settingsPackets()) {
      emitState(
        packet.code,
        packet.body,
        token: packet.token,
        sequence: packet.sequence,
      );
    }
  }

  Iterable<ConnectionSessionPacket> settingsPackets() sync* {
    final bytes = _fixture('settings_state_wire_v3.csv', 'defaults');
    final data = ByteData.sublistView(bytes)
      ..setUint32(0, settingsRevision, Endian.little)
      ..setUint16(8, wheel, Endian.little)
      ..setUint16(26, firstPlayer, Endian.little);
    configureSettings?.call(data);
    final identity = litchcraftAnimationCrc32(bytes);
    for (var index = 0; index < SettingsStateWireCodec.fragmentCount; ++index) {
      yield ConnectionSessionPacket(
        kind: ConnectionSessionKind.state,
        code: SettingsStateWireCodec.fragmentCode(index),
        token: stateToken,
        sequence: identity == 0 ? 1 : identity,
        body: bytes.sublist(
          index * 10,
          index * 10 + SettingsStateWireCodec.fragmentLength(index),
        ),
      );
    }
  }
}
