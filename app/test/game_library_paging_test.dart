import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_library_index.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _initialFen =
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

void main() {
  late Directory directory;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-library-paging-');
  });

  tearDown(() async {
    if (await directory.exists()) await directory.delete(recursive: true);
  });

  test(
    'legacy data migrates once, then windows decode only requested games',
    () async {
      await _writeRealArchiveFiles(directory, 120);

      final _CountingStore migrationStore = _CountingStore(directory);
      final GameLibraryRepository migration = GameLibraryRepository(
        migrationStore,
      );
      final GameLibraryCatalog migrated = await migration.loadCatalog();
      expect(migrated.totalCount, 120);
      expect(migrationStore.fullLoads, 1);
      await migration.close();

      final _CountingStore coldStore = _CountingStore(directory);
      final GameLibraryRepository cold = GameLibraryRepository(coldStore);
      final GameLibraryCatalog catalog = await cold.loadCatalog();
      expect(catalog.totalCount, 120);
      expect(coldStore.fullLoads, 0);
      expect(cold.snapshot.games, isEmpty);

      final GameLibraryWindow window = await cold.loadWindow(
        offset: 40,
        limit: 41,
      );
      expect(window.games, hasLength(41));
      expect(coldStore.exactReads, 41);
      expect(cold.snapshot.games, isEmpty);

      final GameArchive? exact = await cold.findByIdentity(
        window.games.last.libraryId,
        window.games.last.gameId,
        window.games.last.origin,
      );
      expect(exact?.libraryId, window.games.last.libraryId);
      expect(coldStore.exactReads, 42);
      await cold.close();
    },
  );

  test(
    'page read rebuilds when durable identity changes at same timestamp',
    () async {
      final GameLibraryRepository repository = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      final GameArchive original = _appGame(
        libraryId: 'app-page-identity',
        gameId: 'app-page-original',
      );
      await repository.save(original);
      final GameLibraryCatalog catalog = await repository.loadCatalog();

      final File archiveFile = File(
        '${directory.path}/game-${original.libraryId}.json',
      );
      final Map<String, Object?> altered =
          jsonDecode(await archiveFile.readAsString()) as Map<String, Object?>
            ..['gameId'] = 'app-page-replaced';
      await archiveFile.writeAsString('${jsonEncode(altered)}\n', flush: true);

      final GameLibraryWindow window = await repository.loadWindow(
        offset: 0,
        limit: 1,
      );
      expect(window.games.single.gameId, 'app-page-replaced');
      expect(window.generation, greaterThan(catalog.generation));
      await repository.close();
    },
  );

  test('index refuses duplicate origin and game identities', () async {
    final GameLibraryStorageIndex index = GameLibraryStorageIndex(directory);
    await expectLater(
      index.replace(
        games: <GameArchive>[
          _appGame(libraryId: 'app-duplicate-a', gameId: 'app-shared'),
          _appGame(libraryId: 'app-duplicate-b', gameId: 'app-shared'),
        ],
        recoveredFiles: const <String>[],
        rejectedFiles: const <String>[],
      ),
      throwsFormatException,
    );
  });

  test('cold rebuild with duplicate identities withholds both files', () async {
    final GameArchive first = _appGame(
      libraryId: 'cold-duplicate-a',
      gameId: 'cold-shared',
    );
    final GameArchive second = _appGame(
      libraryId: 'cold-duplicate-b',
      gameId: 'cold-shared',
    );
    await directory.create(recursive: true);
    await File(
      '${directory.path}/game-${first.libraryId}.json',
    ).writeAsString('${jsonEncode(first.toJson())}\n', flush: true);
    await File(
      '${directory.path}/game-${second.libraryId}.json',
    ).writeAsString('${jsonEncode(second.toJson())}\n', flush: true);
    final File firstFile = File(
      '${directory.path}/game-${first.libraryId}.json',
    );
    final File secondFile = File(
      '${directory.path}/game-${second.libraryId}.json',
    );
    final List<int> firstBytes = await firstFile.readAsBytes();
    final List<int> secondBytes = await secondFile.readAsBytes();

    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(directory),
    );
    final GameLibraryCatalog catalog = await repository.loadCatalog();
    expect(catalog.totalCount, 0);
    expect(catalog.rejectedFiles, hasLength(2));
    expect((await repository.loadWindow(offset: 0, limit: 20)).games, isEmpty);
    final GameArchive third = _appGame(
      libraryId: 'cold-duplicate-c',
      gameId: 'cold-shared',
    );
    await expectLater(
      GameLibraryStore(directory).save(third),
      throwsFormatException,
    );
    expect(await firstFile.readAsBytes(), firstBytes);
    expect(await secondFile.readAsBytes(), secondBytes);
    expect(
      await File('${directory.path}/game-cold-duplicate-c.json').exists(),
      isFalse,
    );
    await repository.close();
  });

  for (final int count in <int>[1000, 10000]) {
    test(
      '$count real files load and page without full archive materialization',
      () async {
        await _writeRealArchiveFiles(directory, count);
        await GameLibraryStorageIndex(directory).replace(
          games: Iterable<GameArchive>.generate(count, _game),
          recoveredFiles: const <String>[],
          rejectedFiles: const <String>[],
        );

        final _CountingStore store = _CountingStore(directory);
        final GameLibraryRepository repository = GameLibraryRepository(store);
        final int rssBefore = ProcessInfo.currentRss;
        final Stopwatch stopwatch = Stopwatch()..start();
        final GameLibraryCatalog catalog = await repository.loadCatalog();
        final GameLibraryWindow window = await repository.loadWindow(
          offset: count - 49,
          limit: 49,
        );
        stopwatch.stop();
        final int rssDelta = ProcessInfo.currentRss - rssBefore;

        expect(catalog.totalCount, count);
        expect(window.games, hasLength(49));
        expect(store.fullLoads, 0);
        expect(store.exactReads, 49);
        expect(repository.snapshot.games, isEmpty);
        expect(rssDelta, lessThan(48 * 1024 * 1024));
        stderr.writeln(
          'GAME_LIBRARY_PAGE_PERF count=$count elapsed_ms='
          '${stopwatch.elapsedMilliseconds} rss_delta_bytes=$rssDelta '
          'decoded_archives=${store.exactReads}',
        );
        if (count == 10000) {
          await repository.rename(window.games.first.libraryId, 'Paged game');
          expect(store.fullLoads, 0);
          expect(repository.snapshot.games, hasLength(1));
          await repository.delete(window.games.last.libraryId);
          expect(store.fullLoads, 0);
          expect(repository.snapshot.games, isEmpty);
        }
        await repository.close();
      },
    );
  }

  test(
    'a malformed index rebuilds from validated last-good archives',
    () async {
      final GameLibraryStore store = GameLibraryStore(directory);
      final GameArchive original = _game(1);
      await store.save(original);
      final GameLibraryRepository first = GameLibraryRepository(store);
      await first.loadCatalog();
      await first.close();

      final File current = File(
        '${directory.path}/game-${original.libraryId}.json',
      );
      final File previous = File('${current.path}.previous');
      await previous.writeAsBytes(await current.readAsBytes(), flush: true);
      await current.writeAsString('{broken', flush: true);
      await File(
        '${directory.path}/.game-library-index-v1/catalog.json',
      ).writeAsString('{broken-index', flush: true);

      final GameLibraryRepository recovered = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      final GameLibraryCatalog catalog = await recovered.loadCatalog();
      expect(catalog.totalCount, 1);
      expect(catalog.recoveredFiles, <String>[current.path]);
      expect(
        (await recovered.loadWindow(
          offset: 0,
          limit: 1,
        )).games.single.libraryId,
        original.libraryId,
      );
      await recovered.close();
    },
  );

  test(
    'an interrupted index update cannot bless a stale later save or delete',
    () async {
      final GameLibraryStore store = GameLibraryStore(directory);
      await store.save(_game(0));
      final GameLibraryRepository initial = GameLibraryRepository(store);
      await initial.loadCatalog();
      await initial.close();

      final Directory blockedTemporary = Directory(
        '${directory.path}/.game-library-index-v1/catalog.json.tmp',
      );
      await blockedTemporary.create();
      await store.save(_game(1));
      expect(
        await File('${directory.path}/game-scale-00001.json').exists(),
        isTrue,
      );
      await blockedTemporary.delete();
      await store.save(_game(2));

      final GameLibraryRepository afterSaves = GameLibraryRepository(store);
      expect((await afterSaves.loadCatalog()).totalCount, 3);
      await afterSaves.close();

      await blockedTemporary.create();
      await store.delete('scale-00001');
      await blockedTemporary.delete();
      await store.delete('scale-00002');

      final GameLibraryRepository afterDeletes = GameLibraryRepository(store);
      final GameLibraryCatalog catalog = await afterDeletes.loadCatalog();
      expect(catalog.totalCount, 1);
      expect(
        (await afterDeletes.loadWindow(
          offset: 0,
          limit: 1,
        )).games.single.libraryId,
        'scale-00000',
      );
      await afterDeletes.close();
    },
  );

  test(
    'durable invalidation survives an equal-stat index fault and cold reopen',
    () async {
      final GameLibraryStore store = GameLibraryStore(directory);
      await store.save(_game(0));
      final GameLibraryRepository indexed = GameLibraryRepository(store);
      await indexed.loadCatalog();
      await indexed.close();
      final File catalogFile = File(
        '${directory.path}/.game-library-index-v1/catalog.json',
      );
      final Map<String, Object?> staleCatalog =
          jsonDecode(await catalogFile.readAsString()) as Map<String, Object?>;
      final Directory blockedTemporary = Directory(
        '${directory.path}/.game-library-index-v1/catalog.json.tmp',
      );
      await blockedTemporary.create();

      await store.save(_game(1));
      staleCatalog['directoryModifiedMicros'] =
          (await directory.stat()).modified.microsecondsSinceEpoch;
      await catalogFile.writeAsString('${jsonEncode(staleCatalog)}\n');
      expect(
        await GameLibraryStorageIndex(directory).load(),
        isNull,
        reason: 'the durable dirty marker, not timestamp precision, fences it',
      );

      final _CountingStore firstStore = _CountingStore(directory);
      final GameLibraryRepository first = GameLibraryRepository(firstStore);
      expect((await first.loadCatalog()).totalCount, 2);
      expect(firstStore.fullLoads, 1);
      await first.close();

      final _CountingStore reopenedStore = _CountingStore(directory);
      final GameLibraryRepository reopened = GameLibraryRepository(
        reopenedStore,
      );
      expect((await reopened.loadCatalog()).totalCount, 2);
      expect(reopenedStore.fullLoads, 1);
      expect(
        (await reopened.loadWindow(offset: 0, limit: 2)).games,
        hasLength(2),
      );
      await reopened.close();
      await blockedTemporary.delete();
    },
  );

  test(
    'a blocked disposable index serves validated catalog and retries repair',
    () async {
      final GameLibraryStore store = GameLibraryStore(directory);
      await store.save(_game(0));
      await File(
        '${directory.path}/.game-library-index-v1/catalog.json',
      ).writeAsString('{broken', flush: true);
      final Directory blockedTemporary = Directory(
        '${directory.path}/.game-library-index-v1/catalog.json.tmp',
      );
      await blockedTemporary.create();
      final _CountingStore countingStore = _CountingStore(directory);
      final GameLibraryRepository repository = GameLibraryRepository(
        countingStore,
      );

      final GameLibraryCatalog first = await repository.loadCatalog();
      final GameLibraryWindow window = await repository.loadWindow(
        offset: 0,
        limit: 1,
      );
      final GameLibraryCatalog retry = await repository.loadCatalog();

      expect(first.totalCount, 1);
      expect(window.games.single.libraryId, 'scale-00000');
      expect(retry.totalCount, 1);
      expect(countingStore.fullLoads, 1);
      await repository.close();
      await blockedTemporary.delete();
    },
  );

  test('the storage index has no bundle-sized 10000-game ceiling', () async {
    await GameLibraryStorageIndex(directory).replace(
      games: Iterable<GameArchive>.generate(10001, _game),
      recoveredFiles: const <String>[],
      rejectedFiles: const <String>[],
    );
    expect(
      (await GameLibraryStorageIndex(directory).load())?.entries.length,
      10001,
    );
  });

  test('window recovery publishes a new coherent catalog generation', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    await store.save(_game(0));
    await store.save(_game(1));
    final GameLibraryRepository indexed = GameLibraryRepository(store);
    await indexed.loadCatalog();
    await indexed.close();

    await File(
      '${directory.path}/game-scale-00001.json',
    ).writeAsString('{broken', flush: true);
    final GameLibraryRepository repository = GameLibraryRepository(store);
    final GameLibraryCatalog before = await repository.loadCatalog();
    expect(before.totalCount, 2);

    final GameLibraryWindow recovered = await repository.loadWindow(
      offset: 0,
      limit: 2,
    );
    final GameLibraryCatalog after = await repository.loadCatalog();
    expect(recovered.totalCount, 1);
    expect(recovered.generation, isNot(before.generation));
    expect(after.generation, recovered.generation);
    expect(after.totalCount, recovered.totalCount);
    expect(after.rejectedFiles, hasLength(1));
    await repository.close();
  });

  test('late-page recovery publishes one bounded invalidation', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    for (int index = 0; index < 80; index += 1) {
      await store.save(_game(index));
    }
    final GameLibraryRepository indexed = GameLibraryRepository(store);
    await indexed.loadCatalog();
    await indexed.close();
    await File(
      '${directory.path}/game-scale-00005.json',
    ).writeAsString('{broken', flush: true);
    final GameLibraryRepository repository = GameLibraryRepository(store);
    await repository.loadCatalog();
    final List<GameLibrarySnapshot> changes = <GameLibrarySnapshot>[];
    final subscription = repository.changes.listen(changes.add);

    final GameLibraryWindow recovered = await repository.loadWindow(
      offset: 70,
      limit: 10,
    );

    expect(recovered.totalCount, 79);
    expect(changes, hasLength(1));
    expect(changes.single.games, isEmpty);
    expect(repository.snapshot.games, isEmpty);
    expect((await repository.loadCatalog()).rejectedFiles, hasLength(1));
    await subscription.cancel();
    await repository.close();
  });
}

class _CountingStore extends GameLibraryStore {
  _CountingStore(super.directory);

  int fullLoads = 0;
  int exactReads = 0;

  @override
  Future<GameLibrarySnapshot> load() {
    fullLoads += 1;
    return super.load();
  }

  @override
  Future<GameLibraryArchiveRead> read(String libraryId) {
    exactReads += 1;
    return super.read(libraryId);
  }
}

Future<void> _writeRealArchiveFiles(Directory directory, int count) async {
  await directory.create(recursive: true);
  const int batchSize = 200;
  for (int start = 0; start < count; start += batchSize) {
    final int end = (start + batchSize).clamp(start, count);
    await Future.wait<void>(<Future<void>>[
      for (int index = start; index < end; index += 1)
        File(
          '${directory.path}/game-${_game(index).libraryId}.json',
        ).writeAsString('${jsonEncode(_game(index).toJson())}\n'),
    ]);
  }
}

GameArchive _game(int index) {
  final String suffix = index.toString().padLeft(5, '0');
  return GameArchive(
    libraryId: 'scale-$suffix',
    boardGameId: 'scale-board-$suffix',
    createdAt: DateTime.utc(2026, 1, 1),
    updatedAt: DateTime.utc(2026, 1, 1).add(Duration(minutes: index)),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: _initialFen),
    ],
    transitions: const <ArchivedTransition>[],
  );
}

GameArchive _appGame({required String libraryId, required String gameId}) {
  return GameArchive(
    libraryId: libraryId,
    gameId: gameId,
    origin: GameArchiveOrigin.app,
    createdAt: DateTime.utc(2026, 1, 1),
    updatedAt: DateTime.utc(2026, 1, 1),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: _initialFen),
    ],
    transitions: const <ArchivedTransition>[],
  );
}
