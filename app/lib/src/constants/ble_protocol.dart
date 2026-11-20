import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../models/owner_pairing_protocol.dart';

class BleProtocol {
  static const String litchNamePrefix = 'Foxie Litch';
  static const List<String> compatibleDeviceNames = <String>[
    'Foxie Litch',
    'Foxie_Litch',
    'Litch',
  ];

  // Existing base service UUID in firmware.
  static final Uuid baseService = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0100',
  );

  // Base service characteristics (console + identify commands).
  static final Uuid commandCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0101',
  );
  static final Uuid consoleStreamCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0102',
  );

  // Settings service contract.
  static final Uuid settingsService = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0101',
  );
  static final Uuid settingsEnumerateCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0102',
  );
  static final Uuid settingsReadWriteCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0103',
  );

  // OTA service characteristics (same base service, chunked upload).
  static final Uuid otaControlCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0104',
  );
  static final Uuid otaDataCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0105',
  );

  static final Uuid litchcraftCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0106',
  );
  static final Uuid connectionSessionIngressCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0108',
  );
  static final Uuid connectionSessionEgressCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0109',
  );

  static final Uuid gameSyncService = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0300',
  );
  static final Uuid gameSyncCharacteristic = Uuid.parse(
    '6f2a20d1-89df-4ac6-a9f2-53f6f7db0301',
  );

  // Reserved until Owner Pairing is activated with its capability gate.
  static final Uuid ownerPairingCharacteristic = Uuid.parse(
    ownerPairingCharacteristicUuid,
  );
}
