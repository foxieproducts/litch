import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/game_history_sync_state_store.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';

void main() {
  late Directory directory;
  late GameHistorySyncStateStore store;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-sync-state-');
    store = GameHistorySyncStateStore(directory);
  });

  tearDown(() async {
    if (await directory.exists()) {
      await directory.delete(recursive: true);
    }
  });

  test('saves an exact journal identity and cumulative durable ACK', () async {
    const GameHistorySyncCursor cursor = GameHistorySyncCursor(
      journalId: '0102030405060708',
      durableAck: 42,
    );

    await store.save('paired-board-1', cursor);
    final GameHistorySyncStateRead loaded = await store.load('paired-board-1');

    expect(loaded.cursor?.journalId, cursor.journalId);
    expect(loaded.cursor?.durableAck, 42);
    expect(loaded.recovered, isFalse);
  });

  test(
    'optional commit receipt round-trips and validates against its cursor',
    () async {
      const GameHistorySyncCursor cursor = GameHistorySyncCursor(
        journalId: '0102030405060708',
        durableAck: 42,
        lastSegmentSha256:
            '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
        lastCommit: GameHistoryCommitPayload(
          firstOutboxSequence: 40,
          lastOutboxSequence: 42,
          crc32: 0x12345678,
        ),
      );
      await store.save('paired-board-1', cursor);
      final GameHistorySyncCursor saved = (await store.load(
        'paired-board-1',
      )).cursor!;
      expect(saved.lastCommit?.firstOutboxSequence, 40);
      expect(saved.lastCommit?.lastOutboxSequence, 42);
      expect(saved.lastCommit?.crc32, 0x12345678);
      for (final Object? bad in <Object?>[
        null,
        <String, Object>{},
        <String, Object>{
          'firstOutboxSequence': 40,
          'lastOutboxSequence': 41,
          'crc32': 1,
        },
        <String, Object>{
          'firstOutboxSequence': 0,
          'lastOutboxSequence': 42,
          'crc32': 1,
        },
        <String, Object>{
          'firstOutboxSequence': 40,
          'lastOutboxSequence': 42,
          'crc32': -1,
        },
        <String, Object>{
          'firstOutboxSequence': '40',
          'lastOutboxSequence': 42,
          'crc32': 1,
        },
      ]) {
        expect(
          () => GameHistorySyncCursor.fromJson(<String, Object?>{
            ...cursor.toJson(),
            'lastCommit': bad,
          }),
          throwsFormatException,
        );
      }
      for (final Object? digest in <Object?>[null, 1, '', 'A' * 64, '0' * 63]) {
        expect(
          () => GameHistorySyncCursor.fromJson(<String, Object?>{
            ...cursor.toJson(),
            'lastSegmentSha256': digest,
          }),
          throwsFormatException,
        );
      }
      final Map<String, Object?> legacy = cursor.toJson()
        ..remove('lastCommit')
        ..remove('lastSegmentSha256');
      expect(GameHistorySyncCursor.fromJson(legacy).lastCommit, isNull);
      await store.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '1112131415161718',
          durableAck: 0,
        ),
      );
      expect((await store.load('paired-board-1')).cursor?.lastCommit, isNull);
    },
  );

  test('rejects a regressing ACK within one journal identity', () async {
    await store.save(
      'paired-board-1',
      const GameHistorySyncCursor(
        journalId: '0102030405060708',
        durableAck: 42,
      ),
    );

    await expectLater(
      store.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 41,
        ),
      ),
      throwsFormatException,
    );
    expect((await store.load('paired-board-1')).cursor?.durableAck, 42);
  });

  test('accepts a new journal identity with a reset cursor', () async {
    await store.save(
      'paired-board-1',
      const GameHistorySyncCursor(
        journalId: '0102030405060708',
        durableAck: 42,
      ),
    );

    await store.save(
      'paired-board-1',
      const GameHistorySyncCursor(journalId: '1112131415161718', durableAck: 0),
    );

    final GameHistorySyncCursor cursor = (await store.load(
      'paired-board-1',
    )).cursor!;
    expect(cursor.journalId, '1112131415161718');
    expect(cursor.durableAck, 0);
  });

  test('recovers the last validated cursor after current corruption', () async {
    await store.save(
      'paired-board-1',
      const GameHistorySyncCursor(
        journalId: '0102030405060708',
        durableAck: 41,
      ),
    );
    await store.save(
      'paired-board-1',
      const GameHistorySyncCursor(
        journalId: '0102030405060708',
        durableAck: 42,
      ),
    );
    await File(
      '${directory.path}/history-sync-paired-board-1.json',
    ).writeAsString('{broken');

    final GameHistorySyncStateRead recovered = await store.load(
      'paired-board-1',
    );

    expect(recovered.cursor?.durableAck, 41);
    expect(recovered.recovered, isTrue);
  });

  test('clears only the selected paired-board cursor', () async {
    const GameHistorySyncCursor cursor = GameHistorySyncCursor(
      journalId: '0102030405060708',
      durableAck: 42,
    );
    await store.save('paired-board-1', cursor);
    await store.save('paired-board-2', cursor);

    await store.clear('paired-board-1');

    expect((await store.load('paired-board-1')).cursor, isNull);
    expect((await store.load('paired-board-2')).cursor, isNotNull);
  });

  test('rejects unsafe identities and out-of-range cursor fields', () async {
    await expectLater(store.load('../board'), throwsFormatException);
    await expectLater(
      store.save(
        'paired-board-1',
        const GameHistorySyncCursor(journalId: 'UPPER', durableAck: 0),
      ),
      throwsFormatException,
    );
    await expectLater(
      store.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0000000000000000',
          durableAck: 0,
        ),
      ),
      throwsFormatException,
    );
    await expectLater(
      store.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 0x100000000,
        ),
      ),
      throwsFormatException,
    );
  });
}
