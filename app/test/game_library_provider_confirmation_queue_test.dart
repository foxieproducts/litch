import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';
import 'package:litch_mobile/src/services/game_library_bundle.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const _boardId = '000102030405060708090a0b0c0d0e0f';
GameArchive _provider({String status = 'started'}) => GameArchive(
  libraryId: 'board-$_boardId',
  boardGameId: _boardId,
  createdAt: DateTime.utc(2026, 9, 8),
  updatedAt: DateTime.utc(2026, 9, 8),
  status: GameArchiveStatus.active,
  captureQuality: GameCaptureQuality.complete,
  variant: GameVariant.classic,
  outcome: GameOutcome.ongoing,
  winner: GameWinner.none,
  lineRevision: 0,
  positions: const [
    ArchivedPosition(
      ply: 0,
      fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    ),
  ],
  transitions: const [],
  provider: ProviderGameProvenance(
    provider: 'lichess',
    gameId: 'game1234',
    accountId: 'player',
    localSide: 'white',
    rawStatus: status,
  ),
);

void main() {
  late Directory root;
  late GameLibraryStore store;
  late GameLibraryRepository repository;
  setUp(() async {
    root = await Directory.systemTemp.createTemp('provider-cas-');
    store = GameLibraryStore(Directory('${root.path}/archives'));
    repository = GameLibraryRepository(store);
  });
  tearDown(() async {
    await repository.close();
    await root.delete(recursive: true);
  });

  test(
    'queued delete makes expected stale and confirmation cannot recreate archive',
    () async {
      final original = _provider();
      await repository.save(original);
      final expected = (await repository.load()).games.single;
      final candidate = replayStandardChessExtension(expected, ['e2e4']);
      // Deliberately enqueue without awaiting: confirmation must compare after delete.
      final deletion = repository.delete(original.libraryId);
      final confirmation = repository.saveProviderConfirmation(
        candidate,
        expected: expected,
      );
      final rejection = expectLater(confirmation, throwsFormatException);
      await deletion;
      await rejection;
      expect((await store.load()).games, isEmpty);
      expect((await repository.load()).games, isEmpty);
    },
  );

  test(
    'queued replacement import survives stale confirmation byte for byte',
    () async {
      final original = _provider();
      await repository.save(original);
      final expected = (await repository.load()).games.single;
      final candidate = replayStandardChessExtension(expected, ['e2e4']);
      final replacement = _provider(status: 'created');
      final bundle = File('${root.path}/replacement.json');
      await bundle.writeAsString(
        GameLibraryBundle.encode(
          games: [replacement],
          exportedAt: DateTime.utc(2026, 9, 8),
        ),
      );
      // Import respects existing collisions, so replacement explicitly queues delete then import.
      final deletion = repository.delete(original.libraryId);
      final importing = repository.importFromFile(bundle);
      final confirmation = repository.saveProviderConfirmation(
        candidate,
        expected: expected,
      );
      final rejection = expectLater(confirmation, throwsFormatException);
      await deletion;
      expect((await importing).importedIds, [replacement.libraryId]);
      await rejection;
      final disk = (await store.load()).games.single;
      expect(disk.toJson(), replacement.toJson());
      expect(disk.currentPly, 0);
      // The failed queued operation must not poison later legitimate confirmation.
      final fresh = (await repository.load()).games.single;
      final extension = replayStandardChessExtension(fresh, ['e2e4']);
      await repository.saveProviderConfirmation(extension, expected: fresh);
      expect((await store.load()).games.single.toJson(), extension.toJson());
    },
  );

  test(
    'queued first import defeats stale absent expectation without overwrite',
    () async {
      expect((await repository.load()).games, isEmpty);
      final imported = _provider();
      final bundle = File('${root.path}/first.json');
      await bundle.writeAsString(
        GameLibraryBundle.encode(
          games: [imported],
          exportedAt: DateTime.utc(2026, 9, 8),
        ),
      );
      final importing = repository.importFromFile(bundle);
      final candidate = replayStandardChessExtension(imported, ['e2e4']);
      final confirmation = repository.saveProviderConfirmation(candidate);
      final rejection = expectLater(confirmation, throwsFormatException);
      expect((await importing).importedIds, [imported.libraryId]);
      await rejection;
      expect((await store.load()).games.single.toJson(), imported.toJson());
    },
  );

  test(
    'queued local rename does not reject confirmation and its name survives',
    () async {
      final original = _provider();
      await repository.save(original);
      final expected = (await repository.load()).games.single;
      final candidate = replayStandardChessExtension(expected, ['e2e4']);

      final rename = repository.rename(original.libraryId, 'Club night');
      final confirmation = repository.saveProviderConfirmation(
        candidate,
        expected: expected,
      );
      await rename;
      await confirmation;

      final GameArchive saved = (await store.load()).games.single;
      expect(saved.displayName, 'Club night');
      expect(saved.currentPly, 1);
      expect(saved.provider, isNotNull);
    },
  );
}
