import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_library_index.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

void main() {
  late Directory directory;
  late GameLibraryStore store;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-game-library-');
    store = GameLibraryStore(directory);
  });

  tearDown(() async {
    if (await directory.exists()) {
      await directory.delete(recursive: true);
    }
  });

  test('saves and loads a local game without leaving a partial file', () async {
    final GameArchive game = _game(updatedMinute: 1);

    await store.save(game);
    final GameLibrarySnapshot snapshot = await store.load();

    expect(snapshot.games.single.libraryId, game.libraryId);
    expect(snapshot.rejectedFiles, isEmpty);
    expect(
      directory.listSync().where((FileSystemEntity item) {
        return item.path.endsWith('.tmp');
      }),
      isEmpty,
    );
  });

  test('retains and recovers the last known-good version', () async {
    await store.save(_game(updatedMinute: 1));
    await store.save(_game(updatedMinute: 2));
    final File current = File('${directory.path}/game-local-1.json');
    await current.writeAsString('{broken');

    final GameLibrarySnapshot snapshot = await store.load();

    expect(snapshot.games.single.updatedAt.minute, 1);
    expect(snapshot.recoveredFiles, <String>[current.path]);
    expect(snapshot.rejectedFiles, isEmpty);
  });

  test(
    'does not overwrite a good backup with a corrupt current file',
    () async {
      await store.save(_game(updatedMinute: 1));
      await store.save(_game(updatedMinute: 2));
      final File current = File('${directory.path}/game-local-1.json');
      await current.writeAsString('{broken');

      await store.save(_game(updatedMinute: 3));
      await current.writeAsString('{broken-again');
      final GameLibrarySnapshot snapshot = await store.load();

      expect(snapshot.games.single.updatedAt.minute, 1);
      expect(snapshot.recoveredFiles, <String>[current.path]);
    },
  );

  test('quarantines a corrupt record without dropping healthy games', () async {
    await store.save(_game(updatedMinute: 1));
    final File corrupt = File('${directory.path}/game-corrupt.json');
    await corrupt.writeAsString('{broken');

    final GameLibrarySnapshot snapshot = await store.load();

    expect(snapshot.games, hasLength(1));
    expect(snapshot.rejectedFiles, <String>[corrupt.path]);
  });

  for (final String invalidId in <String>[
    '',
    'INVALID',
    '..',
    'with space',
    'x' * 81,
  ]) {
    test(
      'isolates malformed filename identity "$invalidId" during recovery',
      () async {
        await store.save(_game(updatedMinute: 1));
        await store.save(_game(updatedMinute: 2));
        final current = File('${directory.path}/game-local-1.json');
        await current.writeAsString('{broken-current');
        final malformed = File('${directory.path}/game-$invalidId.json');
        final malformedBackup = File('${malformed.path}.previous');
        await malformed.writeAsString('{broken-identity');
        await malformedBackup.writeAsString('preserve-backup-evidence');

        final snapshot = await store.load();

        expect(snapshot.games.single.libraryId, 'local-1');
        expect(snapshot.games.single.updatedAt.minute, 1);
        expect(snapshot.recoveredFiles, <String>[current.path]);
        expect(snapshot.rejectedFiles, <String>[malformed.path]);
        expect(await current.readAsString(), '{broken-current');
        expect(await malformed.readAsString(), '{broken-identity');
        expect(
          await malformedBackup.readAsString(),
          'preserve-backup-evidence',
        );
      },
    );
  }

  test('snapshot collections are defensive and unmodifiable', () {
    final List<GameArchive> games = <GameArchive>[_game(updatedMinute: 1)];
    final List<String> recovered = <String>['recovered'];
    final List<String> rejected = <String>['rejected'];
    final GameLibrarySnapshot snapshot = GameLibrarySnapshot(
      games: games,
      recoveredFiles: recovered,
      rejectedFiles: rejected,
    );

    games.clear();
    recovered.clear();
    rejected.clear();
    expect(snapshot.games, hasLength(1));
    expect(snapshot.recoveredFiles, <String>['recovered']);
    expect(snapshot.rejectedFiles, <String>['rejected']);
    expect(() => snapshot.games.clear(), throwsUnsupportedError);
    expect(() => snapshot.recoveredFiles.clear(), throwsUnsupportedError);
    expect(() => snapshot.rejectedFiles.clear(), throwsUnsupportedError);
  });

  test('rejects case-distinct filename identities', () async {
    await store.save(_game(updatedMinute: 1));

    await expectLater(store.delete('Local-1'), throwsFormatException);
    expect((await store.load()).games.single.libraryId, 'local-1');
  });

  test(
    'distinguishes absent archives from unreadable replacement paths',
    () async {
      final missing = await store.read('local-1');
      expect(missing.archive, isNull);
      expect(missing.rejectedFile, isNull);

      final target = File('${directory.path}/game-local-1.json');
      await target.writeAsString('{broken');
      expect((await store.read('local-1')).rejectedFile, target.path);
      await target.delete();

      final invalidDirectory = Directory(target.path);
      await invalidDirectory.create();
      expect((await store.read('local-1')).rejectedFile, target.path);
      await invalidDirectory.delete();

      final danglingLink = Link(target.path);
      await danglingLink.create('${directory.path}/unavailable.json');
      expect((await store.read('local-1')).rejectedFile, target.path);
    },
  );

  test(
    'deletes only the requested local record and its recovery files',
    () async {
      await store.save(_game(updatedMinute: 1));
      await store.save(_game(updatedMinute: 2));

      await store.delete('local-1');

      final GameLibrarySnapshot snapshot = await store.load();
      expect(snapshot.games, isEmpty);
      expect(snapshot.recoveredFiles, isEmpty);
      expect(snapshot.rejectedFiles, isEmpty);
      expect(directory.listSync().whereType<File>(), isEmpty);
      expect(
        (await GameLibraryStorageIndex(directory).load())?.entries ??
            const <GameLibraryIndexEntry>[],
        isEmpty,
      );
    },
  );
}

GameArchive _game({required int updatedMinute}) {
  return GameArchive(
    libraryId: 'local-1',
    boardGameId: 'board-1',
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1, 0, updatedMinute),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
    ],
    transitions: const <ArchivedTransition>[],
  );
}
