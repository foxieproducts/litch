import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../constants/ble_protocol.dart';
import '../models/litch_device_capabilities.dart';

/// Android may return a cached, nonempty list containing only system services.
/// Retrying that cache cannot establish the Litch characteristic contract.
class LitchGattDiscoveryIncomplete implements Exception {
  const LitchGattDiscoveryIncomplete(this.inventory);
  final String inventory;
  @override
  String toString() => 'Incomplete Litch services: $inventory';
}

/// Opens one exact set of GATT characteristic instances for a BLE connection.
abstract interface class LitchGattSessionFactory {
  Future<LitchGattSession> open(String deviceId);
}

abstract interface class LitchGattInventory {
  bool hasCharacteristic(Uuid serviceId, Uuid characteristicId);
}

/// Validate current bootstrap and explicitly promised product endpoints.
void validateLitchGattCapabilities(
  LitchGattInventory inventory,
  LitchDeviceCapabilities expected,
) {
  final endpoints = <(Uuid, Uuid)>[
    (BleProtocol.baseService, BleProtocol.otaControlCharacteristic),
    (
      BleProtocol.baseService,
      BleProtocol.connectionSessionIngressCharacteristic,
    ),
    (
      BleProtocol.baseService,
      BleProtocol.connectionSessionEgressCharacteristic,
    ),
    if (expected.supportsConsole)
      (BleProtocol.baseService, BleProtocol.commandCharacteristic),
    if (expected.supportsConsole)
      (BleProtocol.baseService, BleProtocol.consoleStreamCharacteristic),
    if (expected.supportsSettings)
      (
        BleProtocol.settingsService,
        BleProtocol.settingsEnumerateCharacteristic,
      ),
    if (expected.supportsSettings)
      (
        BleProtocol.settingsService,
        BleProtocol.settingsReadWriteCharacteristic,
      ),
  ];
  for (final (service, characteristic) in endpoints) {
    if (!inventory.hasCharacteristic(service, characteristic)) {
      throw LitchGattDiscoveryIncomplete(
        'missing advertised endpoint $service/$characteristic',
      );
    }
  }
}

/// Connection-scoped operations that do not repeat UUID-only resolution.
abstract interface class LitchGattSession {
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  });

  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  });

  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  });
}

/// Optional exact-instance fast path for one ordered write-without-response
/// batch. Native mobile transports use one platform crossing and keep
/// packet-sized native queue operations so normal-priority controls can
/// preempt bulk traffic at the surrounding session boundary.
abstract interface class LitchGattBatchSession {
  Future<void> writeWithoutResponseBatch({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<List<int>> values,
  });
}

final class ReactiveBleGattSessionFactory implements LitchGattSessionFactory {
  ReactiveBleGattSessionFactory(this._ble);

  final FlutterReactiveBle _ble;

  @override
  Future<LitchGattSession> open(String deviceId) =>
      openWithCapabilities(deviceId, LitchDeviceCapabilities.unavailable);

  Future<LitchGattSession> openWithCapabilities(
    String deviceId,
    LitchDeviceCapabilities expected,
  ) async {
    final List<Service> services = await _ble.getDiscoveredServices(deviceId);
    bool has(Uuid serviceId, Uuid characteristicId) => services.any(
      (service) =>
          service.deviceId == deviceId &&
          service.id == serviceId &&
          service.characteristics.any(
            (characteristic) => characteristic.id == characteristicId,
          ),
    );
    // Recovery exposes the same bootstrap/session skeleton but deliberately
    // omits Settings. Domain endpoints are checked after capability discovery.
    if (!has(BleProtocol.baseService, BleProtocol.otaControlCharacteristic) ||
        !has(
          BleProtocol.baseService,
          BleProtocol.connectionSessionIngressCharacteristic,
        ) ||
        !has(
          BleProtocol.baseService,
          BleProtocol.connectionSessionEgressCharacteristic,
        )) {
      throw LitchGattDiscoveryIncomplete(
        services
            .map(
              (service) =>
                  '${service.id}:[${service.characteristics.map((c) => c.id).join(',')}]',
            )
            .join(';'),
      );
    }
    final session = _ReactiveBleGattSession(
      deviceId: deviceId,
      services: services,
    );
    validateLitchGattCapabilities(session, expected);
    return session;
  }
}

final class _ReactiveBleGattSession
    implements LitchGattSession, LitchGattInventory, LitchGattBatchSession {
  _ReactiveBleGattSession({
    required this.deviceId,
    required List<Service> services,
  }) : _services = List<Service>.unmodifiable(services);

  final String deviceId;
  final List<Service> _services;

  @override
  bool hasCharacteristic(Uuid serviceId, Uuid characteristicId) =>
      _services.any(
        (service) =>
            service.deviceId == deviceId &&
            service.id == serviceId &&
            service.characteristics.any(
              (characteristic) => characteristic.id == characteristicId,
            ),
      );

  Characteristic _characteristic({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    for (final Service service in _services) {
      if (service.id != serviceId || service.deviceId != deviceId) {
        continue;
      }
      for (final Characteristic characteristic in service.characteristics) {
        if (characteristic.id == characteristicId) {
          // The exact instance retains Android's service and characteristic
          // instance IDs. UUID-only helpers deliberately reject this case
          // when Android exposes duplicate GATT entries after reconnect.
          return characteristic;
        }
      }
    }
    throw StateError(
      'Characteristic not discovered: service=$serviceId '
      'characteristic=$characteristicId device=$deviceId',
    );
  }

  @override
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    return _characteristic(
      serviceId: serviceId,
      characteristicId: characteristicId,
    ).read();
  }

  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) {
    return _characteristic(
      serviceId: serviceId,
      characteristicId: characteristicId,
    ).write(value, withResponse: withResponse);
  }

  @override
  Future<void> writeWithoutResponseBatch({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<List<int>> values,
  }) {
    return _characteristic(
      serviceId: serviceId,
      characteristicId: characteristicId,
    ).writeBatchWithoutResponse(values);
  }

  @override
  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    return _characteristic(
      serviceId: serviceId,
      characteristicId: characteristicId,
    ).subscribe();
  }
}
