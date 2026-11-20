import 'dart:async';
import 'dart:convert';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

const _boardId = 'private-board-id';

class _SettingReadBoard extends SettingsSessionBoard {
  _SettingReadBoard() {
    onProductQuery = null;
  }

  Completer<List<int>>? heldMetadata;
  final metadataRequested = Completer<void>();
  bool failMetadata = false;
  bool emptyMetadata = false;
  int directWrites = 0;

  @override
  Future<List<int>> readCharacteristic(
    QualifiedCharacteristic characteristic,
  ) async {
    if (characteristic.characteristicId ==
        BleProtocol.settingsEnumerateCharacteristic) {
      if (!metadataRequested.isCompleted) metadataRequested.complete();
      if (heldMetadata != null || failMetadata || emptyMetadata) {
        enumerationReads++;
        if (failMetadata) throw StateError('controlled enumeration fault');
        if (heldMetadata case final held?) return held.future;
        return utf8.encode('[]');
      }
    }
    return super.readCharacteristic(characteristic);
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) {
    if (characteristic.characteristicId ==
        BleProtocol.settingsReadWriteCharacteristic) {
      directWrites++;
    }
    return super.writeCharacteristicWithResponse(characteristic, value: value);
  }
}

Future<LitchBleService> _connect(_SettingReadBoard board) async {
  final service = LitchBleService(
    pendingOtaStore: MemoryOtaTargetStore(),
    ble: board,
  );
  addTearDown(() async {
    await disposeBoard(service, board);
    expect(board.directWrites, 0);
    expect(
      board.subscriptions.map((endpoint) => endpoint.characteristicId),
      isNot(contains(BleProtocol.settingsReadWriteCharacteristic)),
    );
  });
  await connectBoard(service, board, deviceId: _boardId);
  expect(await service.ensureProductReady(_boardId), isTrue);
  return service;
}

void _outcomes(
  SettingReadDiagnosticsSnapshot value, {
  required int operations,
  int succeeded = 0,
  int exhausted = 0,
  int cancelled = 0,
  int unknown = 0,
  int pending = 0,
  int enumerations = 0,
}) {
  expect(value.operations, operations);
  expect(value.succeeded, succeeded);
  expect(value.exhausted, exhausted);
  expect(value.cancelled, cancelled);
  expect(value.unknown, unknown);
  expect(value.pending, pending);
  expect(value.completed + value.pending, value.operations);
  expect(value.fallbackEntries, enumerations);
  expect(value.directAttempts, 0);
  expect(value.directSuperseded, 0);
  expect(value.retryAttempts, 0);
  expect(value.recovered, 0);
  expect(value.saturated, isFalse);
  expect(value.totalDurationMs, greaterThanOrEqualTo(value.maxDurationMs));
}

void main() {
  test(
    'retained Settings State and empty reads never count as retries',
    () async {
      final board = _SettingReadBoard();
      final service = await _connect(board);
      board.publishSettings();
      expect(service.hasBoardStateSnapshot, isTrue);
      Future<Object?> read(String key) =>
          service.readSettingValue(deviceId: _boardId, key: key);
      expect(await read(' '), isNull);
      _outcomes(service.supportSessionDiagnostics.settingReads, operations: 0);
      for (var poll = 0; poll < 3; poll++) {
        expect(await read('P1_PIECE_WHEEL'), 40);
      }
      expect(await read('P1_PIECE_ANIMATION_STYLE'), isNull);
      _outcomes(
        service.supportSessionDiagnostics.settingReads,
        operations: 4,
        succeeded: 3,
        unknown: 1,
      );
      expect(board.enumerationReads, 0);
    },
  );

  test(
    'confirmed metadata distinguishes a missing key from a failed read',
    () async {
      final board = _SettingReadBoard();
      final service = await _connect(board);
      expect(
        await service.readSettingValue(
          deviceId: _boardId,
          key: 'GAME_FIRST_PLAYER',
        ),
        1,
      );
      expect(
        await service.readSettingValue(
          deviceId: _boardId,
          key: 'P1_PIECE_ANIMATION_STYLE',
        ),
        isNull,
      );
      _outcomes(
        service.supportSessionDiagnostics.settingReads,
        operations: 2,
        succeeded: 1,
        unknown: 1,
        enumerations: 2,
      );
      expect(board.enumerationReads, 2);
    },
  );

  for (final empty in [false, true]) {
    test(
      '${empty ? 'empty' : 'failed'} metadata cannot fabricate a setting or recovery',
      () async {
        final board = _SettingReadBoard()
          ..emptyMetadata = empty
          ..failMetadata = !empty;
        final service = await _connect(board);
        expect(
          await service.readSettingValue(deviceId: _boardId, key: 'BRIGHTNESS'),
          isNull,
        );
        _outcomes(
          service.supportSessionDiagnostics.settingReads,
          operations: 1,
          exhausted: 1,
          enumerations: 1,
        );
        // An unreadable first chunk also attempts to drain to the next boundary.
        expect(board.enumerationReads, empty ? 1 : 2);
      },
    );
  }

  test(
    'overlapping reads share metadata and observe newer Settings State',
    () async {
      final board = _SettingReadBoard()..heldMetadata = Completer<List<int>>();
      final service = await _connect(board);
      final reads = [
        for (final key in [
          'GAME_FIRST_PLAYER',
          'P1_PIECE_WHEEL',
          'GAME_FIRST_PLAYER',
        ])
          service.readSettingValue(deviceId: _boardId, key: key),
      ];
      await board.metadataRequested.future;
      _outcomes(
        service.supportSessionDiagnostics.settingReads,
        operations: 3,
        pending: 3,
        enumerations: 3,
      );
      expect(board.enumerationReads, 1);
      board.publishSettings();
      board.heldMetadata!.complete(
        utf8.encode('[{"k":"GAME_FIRST_PLAYER","t":"u32","v":1}]'),
      );
      expect(await Future.wait(reads), [2, 40, 2]);
      _outcomes(
        service.supportSessionDiagnostics.settingReads,
        operations: 3,
        succeeded: 3,
        enumerations: 3,
      );
      expect(board.enumerationReads, 1);
    },
  );

  for (final dispose in [false, true]) {
    test(
      '${dispose ? 'disposal' : 'reconnect'} retires a held read exactly once',
      () async {
        final board = _SettingReadBoard()
          ..heldMetadata = Completer<List<int>>();
        final service = await _connect(board);
        final old = service.readSettingValue(
          deviceId: _boardId,
          key: 'GAME_FIRST_PLAYER',
        );
        await board.metadataRequested.future;
        _outcomes(
          service.supportSessionDiagnostics.settingReads,
          operations: 1,
          pending: 1,
          enumerations: 1,
        );
        if (dispose) {
          await service.dispose();
        } else {
          await service.disconnect();
        }
        expect(await old, isNull);
        _outcomes(
          service.supportSessionDiagnostics.settingReads,
          operations: 1,
          cancelled: 1,
          enumerations: 1,
        );
        if (!dispose) {
          await connectBoard(service, board, deviceId: _boardId);
          expect(await service.ensureProductReady(_boardId), isTrue);
          board.publishSettings();
        }
        board.heldMetadata!.complete(
          utf8.encode('[{"k":"GAME_FIRST_PLAYER","t":"u32","v":1}]'),
        );
        await Future<void>.delayed(Duration.zero);
        if (!dispose) {
          expect(
            await service.readSettingValue(
              deviceId: _boardId,
              key: 'GAME_FIRST_PLAYER',
            ),
            2,
          );
        }
        _outcomes(
          service.supportSessionDiagnostics.settingReads,
          operations: dispose ? 1 : 2,
          cancelled: 1,
          succeeded: dispose ? 0 : 1,
          enumerations: 1,
        );
      },
    );
  }
}
