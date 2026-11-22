import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';
import 'package:litch_mobile/src/services/game_library_bundle.dart';
import 'package:litch_mobile/src/services/game_library_index.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _initialFen =
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const String _afterE4 =
    'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';
const String _boardId = '000102030405060708090a0b0c0d0e0f';

void main() {
  late Directory directory;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-game-repository-');
  });

  tearDown(() async {
    if (await directory.exists()) {
      await directory.delete(recursive: true);
    }
  });

  test('serializes overlapping mutations and publishes each result', () async {
    final _ObservedStore store = _ObservedStore(directory);
    final GameLibraryRepository repository = GameLibraryRepository(store);
    final List<int> counts = <int>[];
    final StreamSubscription<GameLibrarySnapshot> subscription = repository
        .changes
        .listen((GameLibrarySnapshot value) => counts.add(value.games.length));

    await Future.wait<void>(<Future<void>>[
      repository.save(_game('local-1', minute: 1)),
      repository.save(_game('local-2', minute: 2)),
      repository.delete('local-1'),
    ]);

    expect(store.maximumConcurrentMutations, 1);
    expect(repository.snapshot.games, isEmpty);
    expect(
      (await repository.findByIdentity(
        'local-2',
        'board-local-2',
        GameArchiveOrigin.board,
      ))?.libraryId,
      'local-2',
    );
    expect(counts, <int>[1, 1, 0]);
    await subscription.cancel();
    await repository.close();
  });

  test('keeps a phone game out of board identity lookup', () async {
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(directory),
    );
    final GameArchive phone = _game('phone-1', minute: 1).branchAt(
      ply: 0,
      libraryId: 'phone-1',
      gameId: 'phone-durable-1',
      createdAt: DateTime.utc(2026, 9, 2),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );

    await repository.save(phone);

    expect(phone.origin, GameArchiveOrigin.app);
    expect(phone.boardGameId, isNull);
    expect(await repository.findByBoardGameId('board-phone-1'), isNull);
    expect(
      await repository.findByIdentity(
        phone.libraryId,
        phone.gameId,
        GameArchiveOrigin.board,
      ),
      isNull,
    );
    await repository.close();
  });

  for (final reuseLocalId in [false, true]) {
    test('Undo preserves a later record with the same '
        '${reuseLocalId ? 'local' : 'durable'} identity', () async {
      final store = GameLibraryStore(directory);
      final repository = GameLibraryRepository(store);
      addTearDown(repository.close);
      final original = _game('local-1', minute: 1);
      await repository.save(original);
      final removed = (await repository.delete(original.libraryId))!;
      final replacement = GameArchive.fromJson({
        ...(reuseLocalId ? _game('local-2', minute: 2) : original).toJson(),
        'libraryId': reuseLocalId ? original.libraryId : 'local-2',
        'displayName': 'Imported after deletion',
      });

      await Future.wait<void>([
        repository.save(replacement),
        repository.restoreDeleted(removed),
      ]);

      expect((await store.load()).games.single.toJson(), replacement.toJson());
    });
  }

  test(
    'Undo preserves an unreadable replacement instead of overwriting it',
    () async {
      final store = GameLibraryStore(directory);
      final repository = GameLibraryRepository(store);
      addTearDown(repository.close);
      final original = _game('local-1', minute: 1);
      await repository.save(original);
      final removed = (await repository.delete(original.libraryId))!;
      final replacement = File(
        '${directory.path}/game-${original.libraryId}.json',
      );
      await replacement.writeAsString('{unreadable-replacement');

      await expectLater(
        repository.restoreDeleted(removed),
        throwsFormatException,
      );

      expect(await replacement.readAsString(), '{unreadable-replacement');
      expect(await File('${replacement.path}.previous').exists(), isFalse);
    },
  );

  test(
    'rejects a second library entry with the same durable identity',
    () async {
      final GameLibraryRepository repository = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      final GameArchive first = _game('phone-a', minute: 1).branchAt(
        ply: 0,
        libraryId: 'phone-a',
        gameId: 'phone-shared-1',
        createdAt: DateTime.utc(2026, 9, 2),
        whitePlayer: const ArchivedPlayerAttribution.guest(),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );
      final GameArchive duplicate = first.branchAt(
        ply: 0,
        libraryId: 'phone-b',
        gameId: 'phone-shared-1',
        createdAt: DateTime.utc(2026, 9, 2, 1),
        whitePlayer: const ArchivedPlayerAttribution.guest(),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );

      await repository.save(first);
      await expectLater(repository.save(duplicate), throwsFormatException);
      expect((await repository.load()).games, hasLength(1));
      await repository.close();
    },
  );

  test('direct store rejects a duplicate before writing any bytes', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    final GameArchive first = _game('direct-a', minute: 1).branchAt(
      ply: 0,
      libraryId: 'direct-a',
      gameId: 'direct-shared',
      createdAt: DateTime.utc(2026, 9, 2),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );
    final GameArchive duplicate = first.branchAt(
      ply: 0,
      libraryId: 'direct-b',
      gameId: 'direct-shared',
      createdAt: DateTime.utc(2026, 9, 2, 1),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );

    await store.save(first);
    final File firstFile = File('${directory.path}/game-direct-a.json');
    final List<int> before = await firstFile.readAsBytes();
    await expectLater(store.save(duplicate), throwsFormatException);

    expect(await firstFile.readAsBytes(), before);
    expect(
      await File('${directory.path}/game-direct-b.json').exists(),
      isFalse,
    );
    final GameArchive persisted = (await store.load()).games.single;
    expect(persisted.libraryId, first.libraryId);
    expect(persisted.gameId, first.gameId);
    expect(persisted.origin, first.origin);
  });

  test('direct store rejects a duplicate through a warm index', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    final GameArchive first = _game('warm-a', minute: 1).branchAt(
      ply: 0,
      libraryId: 'warm-a',
      gameId: 'warm-shared',
      createdAt: DateTime.utc(2026, 9, 2),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );
    final GameArchive duplicate = first.branchAt(
      ply: 0,
      libraryId: 'warm-b',
      gameId: 'warm-shared',
      createdAt: DateTime.utc(2026, 9, 2, 1),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );

    await store.save(first);
    await GameLibraryStorageIndex(directory).replace(
      games: <GameArchive>[first],
      recoveredFiles: const <String>[],
      rejectedFiles: const <String>[],
    );
    final File firstFile = File('${directory.path}/game-warm-a.json');
    final List<int> before = await firstFile.readAsBytes();

    await expectLater(store.save(duplicate), throwsFormatException);
    expect(await firstFile.readAsBytes(), before);
    expect(await File('${directory.path}/game-warm-b.json').exists(), isFalse);
  });

  test(
    'partial and duplicate-only imports invalidate a warm bounded catalog',
    () async {
      final GameLibraryStore setup = GameLibraryStore(directory);
      await setup.save(_game('existing', minute: 1));
      final _FailingImportStore store = _FailingImportStore(
        directory,
        failLibraryId: 'incoming-b',
      );
      final GameLibraryRepository repository = GameLibraryRepository(store);
      expect((await repository.loadCatalog()).totalCount, 1);

      final File partialBundle = File('${directory.path}/partial.litchgames');
      await GameLibraryBundle.exportToFile(
        output: partialBundle,
        games: <GameArchive>[
          _game('incoming-a', minute: 2),
          _game('incoming-b', minute: 3),
        ],
        exportedAt: DateTime.utc(2026, 9, 2),
      );
      await expectLater(
        repository.importFromFile(partialBundle),
        throwsA(isA<FileSystemException>()),
      );

      final GameLibraryCatalog afterFailure = await repository.loadCatalog();
      expect(afterFailure.totalCount, 2);
      expect(
        (await repository.loadWindow(
          offset: 0,
          limit: 5,
        )).games.map((GameArchive game) => game.libraryId).toSet(),
        <String>{'existing', 'incoming-a'},
      );
      expect(repository.snapshot.games, isEmpty);

      await setup.save(_game('external-c', minute: 4));
      final File duplicateBundle = File(
        '${directory.path}/duplicate-only.litchgames',
      );
      await GameLibraryBundle.exportToFile(
        output: duplicateBundle,
        games: <GameArchive>[_game('incoming-a', minute: 2)],
        exportedAt: DateTime.utc(2026, 9, 3),
      );
      final GameLibraryImportResult duplicate = await repository.importFromFile(
        duplicateBundle,
      );
      expect(duplicate.importedIds, isEmpty);
      expect(duplicate.duplicateIds, <String>['incoming-a']);
      expect((await repository.loadCatalog()).totalCount, 3);
      expect(repository.snapshot.games, isEmpty);
      await repository.close();
    },
  );

  test('refuses to rename a recovered copy and preserves both files', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    final GameArchive source = _game('local-recovered', minute: 1);
    await store.save(source);
    await store.save(source.rename('newer current'));
    final File current = File(
      '${directory.path}/game-${source.libraryId}.json',
    );
    final File previous = File('${current.path}.previous');
    await current.writeAsString('{corrupt', flush: true);
    final List<int> currentBefore = await current.readAsBytes();
    final List<int> previousBefore = await previous.readAsBytes();
    final GameLibraryRepository repository = GameLibraryRepository(store);

    final GameLibrarySnapshot recovered = await repository.load();
    expect(recovered.recoveredFiles, <String>[current.path]);
    await expectLater(
      repository.rename(source.libraryId, 'must not promote'),
      throwsA(isA<GameLibraryRecoveredRenameException>()),
    );

    expect(await current.readAsBytes(), currentBefore);
    expect(await previous.readAsBytes(), previousBefore);
    expect((await repository.load()).recoveredFiles, <String>[current.path]);
    await repository.close();
  });

  test(
    'cold-loads and renames maximum-length Classic and Duck games',
    () async {
      final GameLibraryStore store = GameLibraryStore(directory);
      final GameArchive maximumClassic = _maximumClassicArchive();
      final GameArchive maximumDuck = _asDuckArchive(maximumClassic);
      await store.save(maximumClassic);
      await store.save(maximumDuck);

      final GameLibraryRepository coldRepository = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      final GameLibrarySnapshot cold = await coldRepository.load();
      expect(cold.games, hasLength(2));
      expect(
        cold.games.every(
          (GameArchive game) => game.currentPly == gameArchiveMaximumPlies,
        ),
        isTrue,
      );

      await coldRepository.rename(maximumClassic.libraryId, 'Maximum Classic');
      await coldRepository.rename(maximumDuck.libraryId, 'Maximum Duck');
      final GameLibrarySnapshot reloaded = await GameLibraryStore(
        directory,
      ).load();
      expect(
        reloaded.games.map((GameArchive game) => game.displayName).toSet(),
        <String?>{'Maximum Classic', 'Maximum Duck'},
      );
      expect(
        reloaded.games.every(
          (GameArchive game) => game.currentPly == gameArchiveMaximumPlies,
        ),
        isTrue,
      );
      await coldRepository.close();
    },
  );

  test('creates one deterministic archive for a synced board game', () async {
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(directory),
    );
    final AuthoritativeLineSegment segment = _segment(revision: 1);

    final GameArchive created = (await repository.mergeAuthoritativeSegment(
      segment,
      observedAt: DateTime.utc(2026, 9, 3, 10),
    ))!;
    final GameArchive replayed = (await repository.mergeAuthoritativeSegment(
      segment,
      observedAt: DateTime.utc(2026, 9, 3, 11),
    ))!;

    expect(created.libraryId, 'board-$_boardId');
    expect(replayed.libraryId, created.libraryId);
    expect(repository.snapshot.games, hasLength(1));
    expect(repository.snapshot.games.single.lineRevision, 1);
    await repository.close();
  });

  for (final restoreByImport in [false, true]) {
    test(
      '${restoreByImport ? 'Import' : 'Undo'} explicitly restores a deleted board game and resumes capture',
      () async {
        var repository = GameLibraryRepository(GameLibraryStore(directory));
        addTearDown(() => repository.close());
        final created = await repository.mergeAuthoritativeSegment(
          _segment(revision: 1),
          observedAt: DateTime.utc(2026, 9, 3),
        );
        final bundle = File('${directory.path}/restore.litchgames');
        await repository.exportToFile(
          output: bundle,
          exportedAt: DateTime.utc(2026, 9, 3),
        );
        final removed = (await repository.delete(created!.libraryId))!;
        expect(
          await repository.mergeAuthoritativeSegment(
            _segment(revision: 2),
            observedAt: DateTime.utc(2026, 9, 3, 0, 1),
          ),
          isNull,
        );
        expect((await repository.load()).games, isEmpty);

        if (restoreByImport) {
          expect((await repository.importFromFile(bundle)).importedIds, [
            removed.libraryId,
          ]);
        } else {
          await repository.restoreDeleted(removed);
        }
        // A successful explicit restore must retire deletion authority even if
        // its newly restored file later needs ordinary capture recovery.
        await File(
          '${directory.path}/game-${removed.libraryId}.json',
        ).writeAsString('{unreadable-restored-copy');
        await repository.close();
        repository = GameLibraryRepository(GameLibraryStore(directory));
        final updated = await repository.mergeAuthoritativeSegment(
          _segment(revision: 2),
          observedAt: DateTime.utc(2026, 9, 3, 0, 2),
        );
        expect(updated?.libraryId, removed.libraryId);
        expect(updated?.lineRevision, 2);
        expect((await repository.load()).games.single.lineRevision, 2);
      },
    );
  }

  test('failed deletion marker write retains the archive', () async {
    final repository = GameLibraryRepository(GameLibraryStore(directory));
    addTearDown(repository.close);
    final created = await repository.mergeAuthoritativeSegment(
      _segment(revision: 1),
      observedAt: DateTime.utc(2026, 9, 3),
    );
    await File(
      '${directory.path}/.board-game-deletions-v1',
    ).writeAsString('blocked');

    await expectLater(
      repository.delete(created!.libraryId),
      throwsA(isA<FileSystemException>()),
    );

    expect((await repository.load()).games.single.toJson(), created.toJson());
  });

  test('Delete uses a validated recovery copy for deletion and Undo', () async {
    final store = GameLibraryStore(directory);
    final repository = GameLibraryRepository(store);
    addTearDown(repository.close);
    final original = _game('local-1', minute: 1);
    await repository.save(original);
    await repository.save(_game('local-1', minute: 2));
    await File(
      '${directory.path}/game-local-1.json',
    ).writeAsString('{corrupt-current');

    final removed = (await repository.delete(original.libraryId))!;

    expect(removed.toJson(), original.toJson());
    expect((await store.load()).games, isEmpty);
    expect(await store.isBoardGameCaptureSuppressed(original.gameId), isTrue);
    await repository.restoreDeleted(removed);
    expect((await store.load()).games.single.toJson(), original.toJson());
    expect(await store.isBoardGameCaptureSuppressed(original.gameId), isFalse);
  });

  for (final retryByImport in [false, true]) {
    test(
      '${retryByImport ? 'Import' : 'Undo'} retries failed deletion marker cancellation without replacing the archive',
      () async {
        final store = GameLibraryStore(directory);
        final repository = GameLibraryRepository(store);
        addTearDown(repository.close);
        final created = (await repository.mergeAuthoritativeSegment(
          _segment(revision: 1),
          observedAt: DateTime.utc(2026, 9, 3),
        ))!;
        final bundle = File('${directory.path}/retry.litchgames');
        await repository.exportToFile(
          output: bundle,
          exportedAt: DateTime.utc(2026, 9, 3),
        );
        final removed = (await repository.delete(created.libraryId))!;
        final markers = await Directory(
          '${directory.path}/.board-game-deletions-v1',
        ).list().toList();
        final marker = markers.single;
        await marker.delete();
        await Directory(marker.path).create();
        await File('${marker.path}/blocked').writeAsString('retain');

        Future<void> restore() async {
          if (retryByImport) {
            await repository.importFromFile(bundle);
          } else {
            await repository.restoreDeleted(removed);
          }
        }

        await expectLater(restore(), throwsA(isA<FileSystemException>()));

        expect(
          (await store.read(removed.libraryId)).archive!.toJson(),
          removed.toJson(),
        );
        expect(await File('${marker.path}/blocked').readAsString(), 'retain');
        await Directory(marker.path).delete(recursive: true);
        await File(marker.path).writeAsString('${removed.gameId}\n');

        await restore();

        expect(await File(marker.path).exists(), isFalse);
        expect(
          (await store.read(removed.libraryId)).archive!.toJson(),
          removed.toJson(),
        );
        expect((await repository.loadCatalog()).totalCount, 1);
      },
    );
  }

  test(
    'renames one saved game without changing its library ordering time',
    () async {
      final GameLibraryRepository repository = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      final GameArchive source = _game('local-1', minute: 1);
      await repository.save(source);

      await repository.rename(source.libraryId, '  First   tournament  ');

      expect(repository.snapshot.games.single.displayName, 'First tournament');
      expect(repository.snapshot.games.single.updatedAt, source.updatedAt);
      expect(
        (await GameLibraryStore(directory).load()).games.single.displayName,
        'First tournament',
      );
      await repository.rename(source.libraryId, '');
      expect(repository.snapshot.games.single.displayName, isNull);
      await repository.close();
    },
  );

  test(
    'merges a newer suffix and never moves observed time backward',
    () async {
      final GameLibraryRepository repository = GameLibraryRepository(
        GameLibraryStore(directory),
      );
      await repository.mergeAuthoritativeSegment(
        _segment(revision: 1, includeMove: false),
        observedAt: DateTime.utc(2026, 9, 3, 10),
      );

      final GameArchive merged = (await repository.mergeAuthoritativeSegment(
        _segment(revision: 2),
        observedAt: DateTime.utc(2026, 9, 3, 9),
      ))!;

      expect(merged.positions, hasLength(2));
      expect(merged.transitions.single.to, 36);
      expect(merged.updatedAt, DateTime.utc(2026, 9, 3, 10));
      await repository.close();
    },
  );

  test('marks a first observed nonzero base as partial', () async {
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(directory),
    );
    final AuthoritativeLineSegment partial = AuthoritativeLineSegment(
      boardGameId: _boardId,
      lineRevision: 7,
      status: GameArchiveStatus.active,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 1, fen: _afterE4),
      ],
      transitions: const <ArchivedTransition>[],
    );

    final GameArchive created = (await repository.mergeAuthoritativeSegment(
      partial,
      observedAt: DateTime.utc(2026, 9, 3),
    ))!;

    expect(created.captureQuality, GameCaptureQuality.partial);
    expect(created.positions.single.ply, 1);
    await repository.close();
  });

  test('withholds ambiguous imported board identities', () async {
    final GameLibraryStore store = GameLibraryStore(directory);
    final GameArchive first = _game(
      'local-1',
      minute: 1,
      boardGameId: _boardId,
    );
    final GameArchive second = _game(
      'local-2',
      minute: 2,
      boardGameId: _boardId,
    );
    await directory.create(recursive: true);
    await File(
      '${directory.path}/game-${first.libraryId}.json',
    ).writeAsString('${jsonEncode(first.toJson())}\n', flush: true);
    await File(
      '${directory.path}/game-${second.libraryId}.json',
    ).writeAsString('${jsonEncode(second.toJson())}\n', flush: true);
    final GameLibraryRepository repository = GameLibraryRepository(store);

    final GameLibrarySnapshot recovered = await repository.load();
    expect(recovered.games, isEmpty);
    expect(recovered.rejectedFiles, hasLength(2));
    expect(await repository.findByBoardGameId(_boardId), isNull);
    await repository.close();
  });
}

AuthoritativeLineSegment _segment({
  required int revision,
  bool includeMove = true,
}) {
  return AuthoritativeLineSegment(
    boardGameId: _boardId,
    lineRevision: revision,
    status: GameArchiveStatus.active,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    positions: <ArchivedPosition>[
      const ArchivedPosition(ply: 0, fen: _initialFen),
      if (includeMove) const ArchivedPosition(ply: 1, fen: _afterE4),
    ],
    transitions: <ArchivedTransition>[
      if (includeMove)
        const ArchivedTransition(side: GameSide.white, from: 52, to: 36),
    ],
  );
}

GameArchive _game(
  String libraryId, {
  required int minute,
  String? boardGameId,
}) {
  return GameArchive(
    libraryId: libraryId,
    boardGameId: boardGameId ?? 'board-$libraryId',
    createdAt: DateTime.utc(2026, 9, 3),
    updatedAt: DateTime.utc(2026, 9, 3, 0, minute),
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

GameArchive _maximumClassicArchive() {
  const List<String> cycle = <String>['g1f3', 'g8f6', 'f3g1', 'f6g8'];
  return replayStandardChessExtension(
    _game('maximum-classic', minute: 1),
    List<String>.generate(
      gameArchiveMaximumPlies,
      (int index) => cycle[index % cycle.length],
    ),
  );
}

GameArchive _asDuckArchive(GameArchive classic) {
  final List<ArchivedPosition> positions = <ArchivedPosition>[
    for (int index = 0; index < classic.positions.length; index += 1)
      ArchivedPosition(
        ply: classic.positions[index].ply,
        fen: classic.positions[index].fen,
        duckSquare: index.isEven ? 27 : 28,
      ),
  ];
  return GameArchive(
    libraryId: 'maximum-duck',
    boardGameId: 'board-maximum-duck',
    createdAt: classic.createdAt,
    updatedAt: classic.updatedAt,
    status: classic.status,
    captureQuality: classic.captureQuality,
    variant: GameVariant.duck,
    outcome: classic.outcome,
    winner: classic.winner,
    lineRevision: classic.lineRevision,
    positions: positions,
    transitions: <ArchivedTransition>[
      for (int index = 0; index < classic.transitions.length; index += 1)
        ArchivedTransition(
          side: classic.transitions[index].side,
          from: classic.transitions[index].from,
          to: classic.transitions[index].to,
          captureSquare: classic.transitions[index].captureSquare,
          promotion: classic.transitions[index].promotion,
          secondaryFrom: classic.transitions[index].secondaryFrom,
          secondaryTo: classic.transitions[index].secondaryTo,
          duckFrom: positions[index].duckSquare,
          duckTo: positions[index + 1].duckSquare,
        ),
    ],
  );
}

class _ObservedStore extends GameLibraryStore {
  _ObservedStore(super.directory);

  int _concurrentMutations = 0;
  int maximumConcurrentMutations = 0;

  @override
  Future<void> save(GameArchive game) async {
    _concurrentMutations += 1;
    if (_concurrentMutations > maximumConcurrentMutations) {
      maximumConcurrentMutations = _concurrentMutations;
    }
    await Future<void>.delayed(const Duration(milliseconds: 5));
    try {
      await super.save(game);
    } finally {
      _concurrentMutations -= 1;
    }
  }

  @override
  Future<void> delete(String libraryId) async {
    _concurrentMutations += 1;
    if (_concurrentMutations > maximumConcurrentMutations) {
      maximumConcurrentMutations = _concurrentMutations;
    }
    await Future<void>.delayed(const Duration(milliseconds: 5));
    try {
      await super.delete(libraryId);
    } finally {
      _concurrentMutations -= 1;
    }
  }
}

class _FailingImportStore extends GameLibraryStore {
  _FailingImportStore(super.directory, {required this.failLibraryId});

  final String failLibraryId;

  @override
  Future<void> save(GameArchive game) {
    if (game.libraryId == failLibraryId) {
      return Future<void>.error(
        FileSystemException('scripted import write failure', game.libraryId),
      );
    }
    return super.save(game);
  }
}
