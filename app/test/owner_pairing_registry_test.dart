import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

const _identity = '0102030405060708090a0b0c0d0e0f10';
const _historyKey = 'abcdefabcdefabcdefabcdefabcdefab';
const _registryKey = 'flutter.owned_litch_boards_v1';
const _preferencesChannel = MethodChannel(
  'plugins.flutter.io/shared_preferences',
);

// Exercise SharedPreferences' real cache-before-save behavior, controlling only
// its platform persistence boundary. The pairing/session service stays real.
class _RegistryPreferences {
  final values = <String, Object>{};
  final writes = <String>[];
  Future<bool> Function()? saveRegistry;
  Future<void> Function()? afterRead;
  bool failRead = false;

  Future<Object?> handle(MethodCall call) async {
    if (call.method == 'getAll') {
      if (failRead) throw PlatformException(code: 'read_unavailable');
      final snapshot = Map<String, Object>.of(values);
      await afterRead?.call();
      return snapshot;
    }
    if (call.method.startsWith('set')) {
      final arguments = call.arguments as Map;
      final key = arguments['key'] as String;
      final value = arguments['value'] as Object;
      if (key == _registryKey) {
        writes.add(value as String);
        if (!await (saveRegistry?.call() ?? Future.value(true))) return false;
      }
      values[key] = value;
      return true;
    }
    throw StateError('Unexpected preference method ${call.method}');
  }

  String seed(List<String> aliases) {
    final raw = const JsonEncoder.withIndent('  ').convert({
      _identity: {'aliases': aliases, 'historyStorageKey': _historyKey},
    });
    values[_registryKey] = raw;
    return raw;
  }
}

Future<void> _until(bool Function() done) async {
  for (var turn = 0; turn < 100 && !done(); ++turn) {
    await Future<void>.delayed(Duration.zero);
  }
  expect(
    done(),
    isTrue,
    reason: 'The controlled service operation must settle',
  );
}

Future<void> _connectOwner(
  LitchBleService service,
  OwnerPairingBoard board, {
  String deviceId = 'board-1',
}) async {
  board
    ..state = 6
    ..flags = 5
    ..attempt = 0;
  await connectBoard(service, board, deviceId: deviceId);
  await _until(
    () =>
        service.ownerPairingStatus?.state ==
        OwnerPairingSessionState.ownedOther,
  );
  expect(service.productReady, isFalse);
  expect(board.pairingCommands, isEmpty);
}

void main() {
  final binding = TestWidgetsFlutterBinding.ensureInitialized();
  late _RegistryPreferences preferences;
  late OwnerPairingBoard board;
  late LitchBleService service;

  setUp(() {
    SharedPreferences.resetStatic();
    preferences = _RegistryPreferences();
    binding.defaultBinaryMessenger.setMockMethodCallHandler(
      _preferencesChannel,
      preferences.handle,
    );
    board = OwnerPairingBoard();
    service = LitchBleService(
      ble: board,
      pendingOtaStore: MemoryOtaTargetStore(),
    );
  });

  tearDown(() async {
    await disposeBoard(service, board);
    binding.defaultBinaryMessenger.setMockMethodCallHandler(
      _preferencesChannel,
      null,
    );
    SharedPreferences.resetStatic();
  });

  test('quiet owner reconnect skips unchanged registry persistence', () async {
    final raw = preferences.seed(['board-1']);
    await _connectOwner(service, board);
    expect(preferences.writes, isEmpty);
    expect(board.productQueries, isEmpty);
    board.setState(5, nextFlags: 7);
    expect(await service.ensureProductReady('board-1'), isTrue);
    expect(preferences.writes, isEmpty);
    expect(preferences.values[_registryKey], raw);
    expect(await service.historyStorageIdentity('board-1'), _historyKey);

    final previousToken = service.connectionSessionToken;
    board.productQueries.clear();
    await _connectOwner(service, board);
    expect(service.connectionSessionToken, isNot(previousToken));
    expect(board.productQueries, isEmpty);
    board.attempt = 7;
    board.setState(3, nextFlags: 5);
    await _until(
      () =>
          service.ownerPairingStatus?.state ==
          OwnerPairingSessionState.securing,
    );
    expect(service.productReady, isFalse);
    board.setState(5, nextFlags: 7);
    expect(await service.ensureProductReady('board-1'), isTrue);
    expect(preferences.writes, isEmpty);
    expect(preferences.values[_registryKey], raw);
    expect((await service.rememberedOwnedBoards()).values.single.aliases, [
      'board-1',
    ]);
  });

  test(
    'new owner alias awaits its one registry save and retains history',
    () async {
      final raw = preferences.seed(['previous-platform-alias']);
      final save = Completer<bool>();
      preferences.saveRegistry = () => save.future;
      await _connectOwner(service, board);
      board.setState(5, nextFlags: 7);
      final ready = service.ensureProductReady('board-1');
      await _until(() => preferences.writes.isNotEmpty);
      expect(service.productReady, isFalse);
      expect(preferences.values[_registryKey], raw);
      expect(preferences.writes, hasLength(1));
      save.complete(true);
      expect(await ready, isTrue);
      expect(await service.historyStorageIdentity('board-1'), _historyKey);
      expect((await service.rememberedOwnedBoards()).values.single.aliases, [
        'previous-platform-alias',
        'board-1',
      ]);
      final stored =
          jsonDecode(preferences.values[_registryKey]! as String) as Map;
      expect(stored[_identity]['historyStorageKey'], _historyKey);
      expect(stored[_identity]['aliases'], [
        'previous-platform-alias',
        'board-1',
      ]);
    },
  );

  for (final throws in [false, true]) {
    test(
      'failed owner alias save remains retryable (throws=$throws)',
      () async {
        final raw = preferences.seed(['previous-platform-alias']);
        preferences.saveRegistry = () async {
          if (throws) throw PlatformException(code: 'unavailable');
          return false;
        };
        await _connectOwner(service, board);
        board.setState(5, nextFlags: 7);
        expect(await service.ensureProductReady('board-1'), isTrue);
        expect(preferences.writes, hasLength(1));
        expect(preferences.values[_registryKey], raw);

        if (throws) {
          preferences.failRead = true;
          await _connectOwner(service, board);
          board.setState(5, nextFlags: 7);
          expect(await service.ensureProductReady('board-1'), isTrue);
          expect(preferences.writes, hasLength(1));
          expect(preferences.values[_registryKey], raw);
          preferences.failRead = false;
        }
        preferences.saveRegistry = null;
        await _connectOwner(service, board);
        board.setState(5, nextFlags: 7);
        expect(await service.ensureProductReady('board-1'), isTrue);
        expect(preferences.writes, hasLength(2));
        final stored =
            jsonDecode(preferences.values[_registryKey]! as String) as Map;
        expect(stored[_identity]['aliases'], [
          'previous-platform-alias',
          'board-1',
        ]);
        expect(stored[_identity]['historyStorageKey'], _historyKey);
      },
    );
  }

  test(
    'malformed registry retains exact bytes after proven owner readiness',
    () async {
      const raw = '{"damaged-board-entry": "retain this history evidence"}';
      preferences.values[_registryKey] = raw;
      await _connectOwner(service, board);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(preferences.writes, isEmpty);
      expect(preferences.values[_registryKey], raw);
      await expectLater(
        service.historyStorageIdentity('board-1'),
        throwsFormatException,
      );
    },
  );

  test('late registry save cannot authorize a retired connection', () async {
    preferences.seed(['previous-platform-alias']);
    final save = Completer<bool>();
    preferences.saveRegistry = () => save.future;
    await _connectOwner(service, board);
    board.setState(5, nextFlags: 7);
    final ready = service.ensureProductReady('board-1');
    await _until(() => preferences.writes.isNotEmpty);
    await service.disconnect();
    save.complete(true);
    expect(await ready, isFalse);
    expect(service.productReady, isFalse);
    expect(preferences.writes, hasLength(1));

    preferences.saveRegistry = null;
    await _connectOwner(service, board);
    board.setState(5, nextFlags: 7);
    expect(await service.ensureProductReady('board-1'), isTrue);
    expect(preferences.writes, hasLength(1));
    expect(await service.historyStorageIdentity('board-1'), _historyKey);
  });

  for (final newerSaveSucceeds in [false, true]) {
    test(
      'owner registry orders predecessor and successor saves (newer saved=$newerSaveSucceeds)',
      () async {
        preferences.seed(['previous-platform-alias']);
        final predecessorSave = Completer<bool>();
        preferences.saveRegistry = () => preferences.writes.length == 1
            ? predecessorSave.future
            : Future.value(newerSaveSucceeds);
        await _connectOwner(service, board, deviceId: 'old-platform-alias');
        board.setState(5, nextFlags: 7);
        final predecessorReady = service.ensureProductReady(
          'old-platform-alias',
        );
        await _until(() => preferences.writes.length == 1);
        await service.disconnect();

        await _connectOwner(service, board, deviceId: 'new-platform-alias');
        board.setState(5, nextFlags: 7);
        final ready = service.ensureProductReady('new-platform-alias');
        await _until(
          () =>
              service.ownerPairingStatus?.state ==
              OwnerPairingSessionState.ownerReady,
        );
        await Future<void>.delayed(Duration.zero);
        expect(
          preferences.writes,
          hasLength(1),
          reason: 'A new generation must not overtake the pending native save',
        );
        expect(service.productReady, isFalse);
        predecessorSave.complete(true);
        expect(await predecessorReady, isFalse);
        expect(await ready, isTrue);
        expect(preferences.writes, hasLength(2));

        preferences.saveRegistry = null;
        await _connectOwner(service, board, deviceId: 'new-platform-alias');
        board.setState(5, nextFlags: 7);
        expect(await service.ensureProductReady('new-platform-alias'), isTrue);
        expect(preferences.writes, hasLength(newerSaveSucceeds ? 2 : 3));
        final stored =
            jsonDecode(preferences.values[_registryKey]! as String) as Map;
        expect(stored[_identity]['aliases'], [
          'previous-platform-alias',
          'old-platform-alias',
          'new-platform-alias',
        ]);
        expect(
          await service.historyStorageIdentity('new-platform-alias'),
          _historyKey,
        );
      },
    );
  }

  test('owner registry skips retired queued generations', () async {
    preferences.seed(['previous-platform-alias']);
    final predecessorSave = Completer<bool>();
    preferences.saveRegistry = () => preferences.writes.length == 1
        ? predecessorSave.future
        : Future.value(true);
    await _connectOwner(service, board, deviceId: 'old-platform-alias');
    board.setState(5, nextFlags: 7);
    final predecessorReady = service.ensureProductReady('old-platform-alias');
    await _until(() => preferences.writes.length == 1);
    await service.disconnect();
    await _connectOwner(service, board, deviceId: 'retired-platform-alias');
    board.setState(5, nextFlags: 7);
    final retiredReady = service.ensureProductReady('retired-platform-alias');
    await _until(
      () =>
          service.ownerPairingStatus?.state ==
          OwnerPairingSessionState.ownerReady,
    );
    await service.disconnect();
    await _connectOwner(service, board, deviceId: 'new-platform-alias');
    board.setState(5, nextFlags: 7);
    final ready = service.ensureProductReady('new-platform-alias');
    await _until(
      () =>
          service.ownerPairingStatus?.state ==
          OwnerPairingSessionState.ownerReady,
    );
    await Future<void>.delayed(Duration.zero);
    expect(preferences.writes, hasLength(1));
    predecessorSave.complete(true);
    expect(await predecessorReady, isFalse);
    expect(await retiredReady, isFalse);
    expect(await ready, isTrue);
    expect(preferences.writes, hasLength(2));
    final stored =
        jsonDecode(preferences.values[_registryKey]! as String) as Map;
    expect(stored[_identity]['aliases'], [
      'previous-platform-alias',
      'old-platform-alias',
      'new-platform-alias',
    ]);
    expect(
      await service.historyStorageIdentity('new-platform-alias'),
      _historyKey,
    );
  });

  test('owner registry orders successor save after pending readback', () async {
    preferences.seed(['previous-platform-alias']);
    preferences.saveRegistry = () async => false;
    await _connectOwner(service, board);
    board.setState(5, nextFlags: 7);
    expect(await service.ensureProductReady('board-1'), isTrue);
    expect(preferences.writes, hasLength(1));
    preferences.saveRegistry = null;

    final readCaptured = Completer<void>();
    final releaseRead = Completer<void>();
    preferences.afterRead = () async {
      preferences.afterRead = null;
      readCaptured.complete();
      await releaseRead.future;
    };
    await _connectOwner(service, board, deviceId: 'old-platform-alias');
    board.setState(5, nextFlags: 7);
    final predecessorReady = service.ensureProductReady('old-platform-alias');
    await readCaptured.future;
    await service.disconnect();
    await _connectOwner(service, board, deviceId: 'new-platform-alias');
    board.setState(5, nextFlags: 7);
    final ready = service.ensureProductReady('new-platform-alias');
    await _until(
      () =>
          service.ownerPairingStatus?.state ==
          OwnerPairingSessionState.ownerReady,
    );
    await Future<void>.delayed(Duration.zero);
    expect(
      preferences.writes,
      hasLength(1),
      reason: 'Pending readback cannot overwrite a successor save',
    );
    releaseRead.complete();
    expect(await predecessorReady, isFalse);
    expect(await ready, isTrue);
    expect(preferences.writes, hasLength(2));
    final stored =
        jsonDecode(preferences.values[_registryKey]! as String) as Map;
    expect(stored[_identity]['aliases'], [
      'previous-platform-alias',
      'new-platform-alias',
    ]);
    expect(
      await service.historyStorageIdentity('new-platform-alias'),
      _historyKey,
    );
  });
}
