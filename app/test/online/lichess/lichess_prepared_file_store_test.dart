import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/online/lichess/lichess_board_api_client.dart';
import 'package:litch_mobile/src/online/lichess/lichess_board_api_events.dart';
import 'package:litch_mobile/src/online/lichess/lichess_confirmed_checkpoint.dart';
import 'package:litch_mobile/src/online/lichess/lichess_prepared_session.dart';
import 'package:litch_mobile/src/online/lichess/lichess_prepared_file_store.dart';

LichessPreparedRecord record(int revision) => LichessPreparedRecord(
  identity: const LichessPreparedIdentity(
    sessionId: 'opaque session!',
    boardIdentity: 'board-1',
    boardGameId: '000102030405060708090a0b0c0d0e0f',
    lineRevision: 7,
  ),
  accountId: 'account',
  seek: LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0),
  revision: revision,
  seekMayHaveStarted: false,
);

const String _digest =
    '0000000000000000000000000000000000000000000000000000000000000000';

LichessPreparedRecord pendingRecord(int revision) {
  const identity = LichessPreparedIdentity(
    sessionId: 'opaque session!',
    boardIdentity: 'board-1',
    boardGameId: '000102030405060708090a0b0c0d0e0f',
    lineRevision: 7,
  );
  const pending = LichessPendingLocalMove(
    identity: identity,
    providerGameId: 'game123',
    targetPly: 1,
    uci: 'e2e4',
    localCommandId: 'local-1',
    preStatePly: 0,
    preStatePrefixSha256: _digest,
    retryUsed: true,
    httpAcknowledged: false,
    rejected: false,
  );
  return LichessPreparedRecord(
    identity: identity,
    accountId: 'account',
    seek: LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0),
    revision: revision,
    seekMayHaveStarted: true,
    providerGameId: 'game123',
    providerSide: LichessPlayerColor.white,
    confirmed: LichessConfirmedCheckpoint(
      libraryId: 'board-${identity.boardGameId}',
      prefixSha256: _digest,
      receipt: LichessProviderReceipt(
        rawStatus: 'started',
        ply: 0,
        movesSha256: _digest,
        whiteTimeMs: 600000,
        blackTimeMs: 600000,
        whiteIncrementMs: 0,
        blackIncrementMs: 0,
        receivedAt: DateTime.utc(2026),
      ),
    ),
    pendingLocalMove: pending,
  );
}

void main() {
  late Directory directory;
  late FileLichessPreparedSessionStore store;
  setUp(() async {
    directory = await Directory.systemTemp.createTemp('prepared-file-');
    store = FileLichessPreparedSessionStore(directory);
  });
  tearDown(() async {
    await directory.delete(recursive: true);
  });
  test(
    'actual file roundtrip, revision fence, duplicate receipt and restart',
    () async {
      expect((await store.load()).kind, LichessPreparedLoadKind.absent);
      await store.write(record(0));
      await store.write(record(1));
      await store.write(record(1));
      final restarted = FileLichessPreparedSessionStore(directory);
      expect(
        encodePreparedRecord((await restarted.load()).record!),
        encodePreparedRecord(record(1)),
      );
      await expectLater(store.write(record(0)), throwsFormatException);
      expect(
        directory.listSync().where((f) => f.path.endsWith('.tmp')),
        isEmpty,
      );
    },
  );
  test(
    'corrupt primary retains bytes and validated backup cannot masquerade current',
    () async {
      await store.write(record(0));
      await store.write(record(1));
      final current = File('${directory.path}/prepared-session.json');
      await current.writeAsString('{rejected-source');
      final loaded = await store.load();
      expect(loaded.kind, LichessPreparedLoadKind.recovered);
      expect(loaded.record!.revision, 0);
      await expectLater(
        store.write(record(2)),
        throwsA(isA<FileSystemException>()),
      );
      expect(await current.readAsString(), '{rejected-source');
    },
  );
  test(
    'oversized primary and missing primary with backup fail closed',
    () async {
      await store.write(record(0));
      await store.write(record(1));
      final current = File('${directory.path}/prepared-session.json');
      await current.writeAsString(
        'x' * (FileLichessPreparedSessionStore.maximumBytes + 1),
      );
      expect((await store.load()).kind, LichessPreparedLoadKind.recovered);
      await current.delete();
      expect((await store.load()).kind, LichessPreparedLoadKind.recovered);
    },
  );
  test(
    'invalid schema, identity, counter and seek are typed corruption without erasure',
    () async {
      final good = encodePreparedRecord(record(0));
      final bads = <Map<String, Object?>>[
        {...good, 'schemaVersion': 3},
        {...good, 'revision': -1},
        {...good, 'minutes': 181},
        {...good, 'providerGameId': 'game1'},
        {
          ...good,
          'identity': {
            ...good['identity'] as Map<String, Object?>,
            'boardGameId': 'line',
          },
        },
        {
          ...good,
          'identity': {
            ...good['identity'] as Map<String, Object?>,
            'sessionId': 'x' * 81,
          },
        },
        {...good, 'accountId': 'bad\naccount'},
      ];
      for (final bad in bads) {
        final current = File('${directory.path}/prepared-session.json');
        final raw = jsonEncode(bad);
        await current.writeAsString(raw);
        expect((await store.load()).kind, LichessPreparedLoadKind.corrupt);
        expect(await current.readAsString(), raw);
      }
    },
  );
  test('non-directory parent is unavailable rather than absent', () async {
    final file = File('${directory.path}/not-directory');
    await file.writeAsString('preserve');
    final invalid = FileLichessPreparedSessionStore(
      Directory('${file.path}/child'),
    );
    expect((await invalid.load()).kind, LichessPreparedLoadKind.unavailable);
    expect(await file.readAsString(), 'preserve');
  });

  test('schema 2 roundtrips pending intent and schema 1 migrates safely', () {
    final pending = pendingRecord(4);
    final encoded = encodePreparedRecord(pending);
    expect(encoded['schemaVersion'], 2);
    final decoded = decodePreparedRecord(encoded);
    expect(decoded.pendingLocalMove!.localCommandId, 'local-1');
    expect(decoded.pendingLocalMove!.retryUsed, isTrue);

    final legacy = Map<String, Object?>.of(encodePreparedRecord(record(2)))
      ..['schemaVersion'] = 1
      ..remove('pendingLocalMove')
      ..remove('confirmedLocalMove');
    final migrated = decodePreparedRecord(legacy);
    expect(migrated.pendingLocalMove, isNull);
    expect(migrated.confirmedLocalMove, isNull);
    expect(encodePreparedRecord(migrated)['schemaVersion'], 2);
  });

  test('pending intent rejects downgrade, mismatch and unbounded identity', () {
    final good = encodePreparedRecord(pendingRecord(4));
    final pending = preparedMap(good['pendingLocalMove']);
    final identity = preparedMap(pending['identity']);
    final bads = <Map<String, Object?>>[
      {...good, 'schemaVersion': 0},
      {...good, 'schemaVersion': 1},
      {
        ...good,
        'pendingLocalMove': {...pending, 'targetPly': 2},
      },
      {
        ...good,
        'pendingLocalMove': {...pending, 'localCommandId': 'x' * 65},
      },
      {
        ...good,
        'pendingLocalMove': {
          ...pending,
          'identity': {...identity, 'lineRevision': 8},
        },
      },
    ];
    for (final bad in bads) {
      expect(() => decodePreparedRecord(bad), throwsFormatException);
    }
  });
}
