import 'dart:async';
import 'dart:io';

import '../models/game_archive.dart';
import 'game_library_bundle.dart';
import 'game_library_index.dart';
import 'game_library_store.dart';

class GameLibraryRecoveredRenameException implements Exception {
  const GameLibraryRecoveredRenameException();
}

class GameLibraryCatalog {
  const GameLibraryCatalog({
    required this.totalCount,
    required this.recoveredFiles,
    required this.rejectedFiles,
    this.namedGames = const <bool>[],
    this.generation = 0,
  });

  final int totalCount;
  final List<String> recoveredFiles;
  final List<String> rejectedFiles;
  final List<bool> namedGames;
  final int generation;

  bool hasDisplayNameAt(int index) {
    return index >= 0 && index < namedGames.length && namedGames[index];
  }
}

class GameLibraryWindow {
  GameLibraryWindow({
    required this.offset,
    required this.totalCount,
    required Iterable<GameArchive> games,
    this.generation = 0,
  }) : games = List<GameArchive>.unmodifiable(games);

  final int offset;
  final int totalCount;
  final List<GameArchive> games;
  final int generation;
}

/// Serializes every library mutation so imports, sync, and user actions cannot
/// race through the store's shared temporary-file names.
class GameLibraryRepository {
  GameLibraryRepository(this._store)
    : _index = GameLibraryStorageIndex(_store.directory),
      _snapshot = GameLibrarySnapshot();

  final GameLibraryStore _store;
  final GameLibraryStorageIndex _index;
  final StreamController<GameLibrarySnapshot> _changes =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);
  Future<void> _mutationTail = Future<void>.value();
  GameLibrarySnapshot _snapshot;
  bool _closed = false;
  GameLibraryIndexSnapshot? _catalogIndex;
  GameLibraryIndexSnapshot? _lastCatalogProjection;
  int _catalogGeneration = 0;
  bool _catalogRepairPending = false;
  bool _catalogInvalidated = false;

  GameLibrarySnapshot get snapshot => _snapshot;

  Stream<GameLibrarySnapshot> get changes => _changes.stream;

  Future<GameLibrarySnapshot> load() {
    return _serialize<GameLibrarySnapshot>(() async {
      final GameLibrarySnapshot current = await _store.load();
      _notifyChange();
      return current;
    });
  }

  /// Loads only the small durable catalog, rebuilding it once for legacy data.
  Future<GameLibraryCatalog> loadCatalog() {
    return _serialize<GameLibraryCatalog>(() async {
      final GameLibraryIndexSnapshot index = await _ensureCatalog();
      return _catalogFromIndex(index, generation: _catalogGeneration);
    });
  }

  /// Decodes only the requested ordered archive window from storage.
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) {
    return _serialize<GameLibraryWindow>(() async {
      if (offset < 0 || limit < 1 || limit > 100) {
        throw RangeError('Invalid game-library window.');
      }
      GameLibraryIndexSnapshot index = await _ensureCatalog();
      final int start = offset.clamp(0, index.entries.length);
      final int end = (start + limit).clamp(start, index.entries.length);
      final List<GameArchive> games = <GameArchive>[];
      bool needsRecoveryRebuild = false;
      for (final GameLibraryIndexEntry entry in index.entries.sublist(
        start,
        end,
      )) {
        final GameLibraryArchiveRead result = await _store.read(
          entry.libraryId,
        );
        final GameArchive? archive = result.archive;
        final String? recoveredFile = result.recoveredFile;
        if (archive == null ||
            archive.gameId != entry.gameId ||
            archive.origin != entry.origin ||
            archive.updatedAt != entry.updatedAt ||
            (recoveredFile != null &&
                !index.recoveredFiles.contains(recoveredFile)) ||
            result.rejectedFile != null) {
          needsRecoveryRebuild = true;
          break;
        }
        games.add(archive);
      }
      if (needsRecoveryRebuild) {
        index = await _rebuildCatalog();
        final int rebuiltStart = offset.clamp(0, index.entries.length);
        final int rebuiltEnd = (rebuiltStart + limit).clamp(
          rebuiltStart,
          index.entries.length,
        );
        games.clear();
        for (final GameLibraryIndexEntry entry in index.entries.sublist(
          rebuiltStart,
          rebuiltEnd,
        )) {
          final GameLibraryArchiveRead result = await _store.read(
            entry.libraryId,
          );
          if (result.archive case final GameArchive archive) games.add(archive);
        }
        return GameLibraryWindow(
          offset: rebuiltStart,
          totalCount: index.entries.length,
          games: games,
          generation: _catalogGeneration,
        );
      }
      return GameLibraryWindow(
        offset: start,
        totalCount: index.entries.length,
        games: games,
        generation: _catalogGeneration,
      );
    });
  }

  Future<GameArchive?> findByIdentity(
    String libraryId,
    String gameId,
    GameArchiveOrigin origin,
  ) {
    return _serialize<GameArchive?>(() async {
      final GameLibraryArchiveRead result = await _store.read(libraryId);
      final GameArchive? archive = result.archive;
      return archive?.gameId == gameId && archive?.origin == origin
          ? archive
          : null;
    });
  }

  Future<GameArchive?> findByBoardGameId(String boardGameId) {
    return _serialize<GameArchive?>(() async {
      final GameLibrarySnapshot current = await _store.load();
      return _singleBoardMatch(current.games, boardGameId);
    });
  }

  Future<void> save(GameArchive game) {
    return _serialize<void>(() async {
      final GameLibraryIndexSnapshot catalog = await _ensureCatalog();
      _rejectIndexedDurableIdentityCollision(catalog.entries, game);
      await _store.save(game);
      _notifyChange(game: game);
    });
  }

  Future<void> rename(String libraryId, String? displayName) {
    return _serialize<void>(() async {
      final GameLibraryArchiveRead result = await _store.read(libraryId);
      final GameArchive? current = result.archive;
      if (current == null) {
        throw const FormatException('The saved game is unavailable.');
      }
      if (result.recoveredFile != null) {
        throw const GameLibraryRecoveredRenameException();
      }
      final GameArchive renamed = current.rename(displayName);
      if (!identical(renamed, current)) {
        await _store.save(renamed);
        _notifyChange(game: renamed);
      }
    });
  }

  /// Compare and save inside the existing library mutation queue. Provider
  /// reconciliation cannot race an import/delete or overwrite a native archive.
  Future<void> saveProviderConfirmation(
    GameArchive candidate, {
    GameArchive? expected,
  }) {
    return _serialize<void>(() async {
      candidate.validate();
      if (candidate.provider == null) {
        throw const FormatException('Missing provider confirmation.');
      }
      final snapshot = await _store.load();
      if (snapshot.recoveredFiles.isNotEmpty ||
          snapshot.rejectedFiles.isNotEmpty) {
        throw const FormatException('Library recovery must finish first.');
      }
      final matches = snapshot.games
          .where(
            (g) =>
                g.libraryId == candidate.libraryId ||
                _sameDurableIdentity(g, candidate),
          )
          .toList();
      if (matches.length > 1 ||
          (expected == null && matches.isNotEmpty) ||
          (expected != null &&
              (matches.length != 1 ||
                  matches.single.provider == null ||
                  GameLibraryBundle.providerAuthorityDigest(matches.single) !=
                      GameLibraryBundle.providerAuthorityDigest(expected)))) {
        throw const FormatException(
          'Provider archive changed during reconciliation.',
        );
      }
      final GameArchive confirmed = matches.isEmpty
          ? candidate
          : candidate.rename(matches.single.displayName);
      await _store.save(confirmed);
      _notifyChange(game: confirmed);
    });
  }

  /// Captures the removed version inside the same queue as incoming history.
  Future<GameArchive?> delete(String libraryId) {
    return _serialize<GameArchive?>(() async {
      final GameLibraryArchiveRead removed = await _store.read(libraryId);
      if (removed.rejectedFile != null) {
        throw const FormatException('The saved game is unreadable.');
      }
      final GameArchive? archive = removed.archive;
      if (archive != null && archive.origin == GameArchiveOrigin.board) {
        await _store.suppressBoardGameCapture(archive.gameId);
      }
      await _store.delete(libraryId);
      _notifyChange();
      return removed.archive;
    });
  }

  /// Undo cannot replace a game captured or imported after the deletion.
  Future<void> restoreDeleted(GameArchive removed) {
    return _serialize<void>(() async {
      final GameLibraryArchiveRead current = await _store.read(
        removed.libraryId,
      );
      final GameArchive? currentArchive = current.archive;
      if (currentArchive != null) {
        if (_sameDurableIdentity(currentArchive, removed)) {
          if (removed.origin == GameArchiveOrigin.board) {
            await _store.clearBoardGameCaptureSuppression(removed.gameId);
          }
          // A prior restore may have committed before marker cleanup failed.
          _notifyChange(game: currentArchive);
        }
        return;
      }
      if (current.rejectedFile != null) {
        throw const FormatException('The saved game is unreadable.');
      }
      final GameLibraryIndexSnapshot catalog = await _ensureCatalog();
      final existing = catalog.entries
          .where(
            (GameLibraryIndexEntry entry) =>
                entry.origin == removed.origin &&
                entry.gameId == removed.gameId,
          )
          .firstOrNull;
      if (existing != null) {
        final restored = (await _store.read(existing.libraryId)).archive;
        if (restored == null || !_sameDurableIdentity(restored, removed)) {
          throw const FormatException('The restored game is unavailable.');
        }
        if (removed.origin == GameArchiveOrigin.board) {
          await _store.clearBoardGameCaptureSuppression(removed.gameId);
        }
        _notifyChange(game: restored);
        return;
      }
      await _store.save(removed);
      _notifyChange(game: removed);
    });
  }

  /// A null result is a durably deleted game, intentionally omitted from capture.
  Future<GameArchive?> mergeAuthoritativeSegment(
    AuthoritativeLineSegment segment, {
    required DateTime observedAt,
    ArchivedPlayerAttribution whitePlayer =
        const ArchivedPlayerAttribution.unknown(),
    ArchivedPlayerAttribution blackPlayer =
        const ArchivedPlayerAttribution.unknown(),
  }) {
    return _serialize<GameArchive?>(() async {
      segment.validate();
      final DateTime observation = observedAt.toUtc();
      final GameLibrarySnapshot current = await _store.load();
      final GameArchive? existing = _singleBoardMatch(
        current.games,
        segment.boardGameId,
      );
      if (existing == null &&
          await _store.isBoardGameCaptureSuppressed(segment.boardGameId)) {
        return null;
      }
      final GameArchive merged;
      if (existing == null) {
        final String libraryId = _libraryIdForBoardGame(segment.boardGameId);
        final GameArchive? identityCollision = current.games
            .where((GameArchive game) => game.libraryId == libraryId)
            .firstOrNull;
        if (identityCollision != null) {
          throw const FormatException(
            'Board-game identity collides with a local archive.',
          );
        }
        merged = GameArchive(
          libraryId: libraryId,
          boardGameId: segment.boardGameId,
          createdAt: observation,
          updatedAt: observation,
          status: segment.status,
          captureQuality: segment.basePly == 0
              ? GameCaptureQuality.complete
              : GameCaptureQuality.partial,
          variant: segment.variant,
          outcome: segment.outcome,
          winner: segment.winner,
          lineRevision: segment.lineRevision,
          positions: segment.positions,
          transitions: segment.transitions,
          whitePlayer: whitePlayer,
          blackPlayer: blackPlayer,
        );
      } else {
        final DateTime monotonicObservation =
            observation.isBefore(existing.updatedAt)
            ? existing.updatedAt
            : observation;
        merged = existing.mergeAuthoritativeSegment(
          segment,
          observedAt: monotonicObservation,
        );
      }

      if (!identical(merged, existing)) {
        await _store.save(merged);
        _notifyChange(game: merged);
      }
      return merged;
    });
  }

  /// Changes only local attribution after a fresh current-line observation.
  /// No stale archive object is saved over a later move/Undo/import.
  Future<void> assignCurrentBoardPlayer({
    required GameArchive expected,
    required bool white,
    required ArchivedPlayerAttribution player,
    required bool Function() isCurrent,
  }) => _serialize<void>(() async {
    final read = await _store.read(expected.libraryId);
    final current = read.archive;
    if (!isCurrent() ||
        current == null ||
        read.recoveredFile != null ||
        current.origin != GameArchiveOrigin.board ||
        current.gameId != expected.gameId ||
        current.lineRevision != expected.lineRevision) {
      throw StateError(
        'The current game changed. Try assigning the player again.',
      );
    }
    final assigned = current.assignPlayers(
      whitePlayer: white ? player : current.whitePlayer,
      blackPlayer: white ? current.blackPlayer : player,
    );
    if (!isCurrent()) throw StateError('The board connection changed.');
    await _store.save(assigned);
    // The repository mutation owner still excludes newer archive writes. If
    // authority retired during this write, restore only this exact prior file.
    if (!isCurrent()) {
      await _store.save(current);
      throw StateError('The board connection changed.');
    }
    _notifyChange(game: assigned);
  });

  /// Applies a proven same-board identity replacement without changing its line.
  Future<void> markBoardGameUnfinished(
    String boardGameId, {
    required DateTime observedAt,
  }) => _serialize<void>(() async {
    final current = await _store.load();
    final archive = _singleBoardMatch(current.games, boardGameId);
    if (archive == null ||
        archive.origin != GameArchiveOrigin.board ||
        archive.status != GameArchiveStatus.active) {
      return;
    }
    final observation = observedAt.toUtc().isBefore(archive.updatedAt)
        ? archive.updatedAt
        : observedAt.toUtc();
    final unfinished = archive.mergeAuthoritativeSegment(
      AuthoritativeLineSegment(
        boardGameId: boardGameId,
        lineRevision: archive.lineRevision,
        status: GameArchiveStatus.unfinished,
        variant: archive.variant,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: archive.positions,
        transitions: archive.transitions,
      ),
      observedAt: observation,
    );
    await _store.save(unfinished);
    _notifyChange(game: unfinished);
  });

  Future<GameLibraryImportResult> importFromFile(File input) {
    return _serialize<GameLibraryImportResult>(() async {
      try {
        return await GameLibraryBundle.importFromFile(
          input: input,
          store: _store,
        );
      } finally {
        // Bundle imports commit one archive at a time. Invalidate even when the
        // operation throws or reports duplicates so a warm catalog cannot hide
        // durable progress observed by the bundle's authoritative store scan.
        _notifyChange();
      }
    });
  }

  Future<void> exportToFile({
    required File output,
    required DateTime exportedAt,
  }) {
    return _serialize<void>(() async {
      final GameLibrarySnapshot current = await _store.load();
      await GameLibraryBundle.exportToFile(
        output: output,
        games: current.games,
        exportedAt: exportedAt,
      );
    });
  }

  Future<void> close() async {
    if (_closed) {
      return;
    }
    _closed = true;
    await _mutationTail;
    await _changes.close();
  }

  Future<T> _serialize<T>(Future<T> Function() action) {
    if (_closed) {
      return Future<T>.error(
        StateError('The game-library repository is closed.'),
      );
    }
    final Completer<T> result = Completer<T>();
    _mutationTail = _mutationTail.then<void>((_) async {
      try {
        result.complete(await action());
      } catch (error, stackTrace) {
        result.completeError(error, stackTrace);
      }
    });
    return result.future;
  }

  void _notifyChange({GameArchive? game, bool invalidateCatalog = true}) {
    if (invalidateCatalog) {
      _catalogIndex = null;
      _catalogInvalidated = true;
    }
    _snapshot = GameLibrarySnapshot(
      games: game == null ? const <GameArchive>[] : <GameArchive>[game],
    );
    if (!_closed) {
      _changes.add(_snapshot);
    }
  }

  Future<GameLibraryIndexSnapshot> _ensureCatalog() async {
    final GameLibraryIndexSnapshot? cached = _catalogIndex;
    if (cached != null) {
      if (_catalogRepairPending) {
        try {
          await _index.replaceSnapshot(cached);
          _catalogRepairPending = false;
        } on FileSystemException {
          // The validated in-memory projection remains usable. Retry later.
        } on FormatException {
          // The validated in-memory projection remains usable. Retry later.
        }
      }
      return cached;
    }
    final bool invalidatedByRepositoryMutation = _catalogInvalidated;
    // Store mutations invalidate the repository's in-memory projection, not a
    // newly committed storage index. The index validates its dirty marker,
    // schema and directory generation before returning; use it when valid and
    // retain the full archive scan as the recovery path.
    final GameLibraryIndexSnapshot? stored = await _index.load();
    if (stored != null) {
      _setCatalog(stored);
      _catalogRepairPending = false;
      _catalogInvalidated = false;
      return stored;
    }
    return _rebuildCatalog(
      notifyProjectionChange: !invalidatedByRepositoryMutation,
    );
  }

  Future<GameLibraryIndexSnapshot> _rebuildCatalog({
    bool notifyProjectionChange = true,
  }) async {
    final GameLibrarySnapshot current = await _store.load();
    final GameLibraryIndexSnapshot rebuilt = GameLibraryIndexSnapshot(
      entries: current.games.map(
        (GameArchive game) => GameLibraryIndexEntry(
          libraryId: game.libraryId,
          gameId: game.gameId,
          origin: game.origin,
          updatedAt: game.updatedAt,
          hasDisplayName: game.displayName != null,
        ),
      ),
      recoveredFiles: current.recoveredFiles,
      rejectedFiles: current.rejectedFiles,
    );
    final bool hadProjection = _lastCatalogProjection != null;
    final bool changed = _setCatalog(rebuilt);
    try {
      await _index.replaceSnapshot(rebuilt);
      _catalogRepairPending = false;
    } on FileSystemException {
      _catalogRepairPending = true;
    } on FormatException {
      _catalogRepairPending = true;
    }
    _catalogInvalidated = false;
    _snapshot = GameLibrarySnapshot();
    if (notifyProjectionChange && hadProjection && changed) {
      _notifyChange(invalidateCatalog: false);
    }
    return rebuilt;
  }

  bool _setCatalog(GameLibraryIndexSnapshot index) {
    final bool changed = !_sameCatalogProjection(_lastCatalogProjection, index);
    if (changed) {
      _catalogGeneration += 1;
    }
    _catalogIndex = index;
    _lastCatalogProjection = index;
    return changed;
  }
}

bool _sameCatalogProjection(
  GameLibraryIndexSnapshot? left,
  GameLibraryIndexSnapshot right,
) {
  if (left == null || left.entries.length != right.entries.length) return false;
  for (int index = 0; index < left.entries.length; index += 1) {
    final GameLibraryIndexEntry leftEntry = left.entries[index];
    final GameLibraryIndexEntry rightEntry = right.entries[index];
    if (leftEntry.libraryId != rightEntry.libraryId ||
        leftEntry.gameId != rightEntry.gameId ||
        leftEntry.origin != rightEntry.origin ||
        leftEntry.updatedAt != rightEntry.updatedAt ||
        leftEntry.hasDisplayName != rightEntry.hasDisplayName) {
      return false;
    }
  }
  return _sameStrings(left.recoveredFiles, right.recoveredFiles) &&
      _sameStrings(left.rejectedFiles, right.rejectedFiles);
}

bool _sameStrings(List<String> left, List<String> right) {
  if (left.length != right.length) return false;
  for (int index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

bool _sameDurableIdentity(GameArchive left, GameArchive right) =>
    left.origin == right.origin && left.gameId == right.gameId;

void _rejectIndexedDurableIdentityCollision(
  Iterable<GameLibraryIndexEntry> entries,
  GameArchive candidate,
) {
  for (final GameLibraryIndexEntry entry in entries) {
    if (entry.libraryId != candidate.libraryId &&
        entry.origin == candidate.origin &&
        entry.gameId == candidate.gameId) {
      throw const FormatException(
        'The library already contains this durable game identity.',
      );
    }
  }
}

GameLibraryCatalog _catalogFromIndex(
  GameLibraryIndexSnapshot index, {
  required int generation,
}) {
  return GameLibraryCatalog(
    totalCount: index.entries.length,
    recoveredFiles: index.recoveredFiles,
    rejectedFiles: index.rejectedFiles,
    namedGames: index.entries
        .map((GameLibraryIndexEntry entry) => entry.hasDisplayName)
        .toList(growable: false),
    generation: generation,
  );
}

GameArchive? _singleBoardMatch(
  Iterable<GameArchive> games,
  String boardGameId,
) {
  GameArchive? match;
  for (final GameArchive game in games) {
    if (game.boardGameId != boardGameId) {
      continue;
    }
    if (match != null) {
      throw const FormatException(
        'The library contains duplicate board-game identities.',
      );
    }
    match = game;
  }
  return match;
}

String _libraryIdForBoardGame(String boardGameId) {
  if (!RegExp(r'^[0-9a-f]{32}$').hasMatch(boardGameId)) {
    throw const FormatException(
      'Synced board-game identities must be 16-byte lowercase hex.',
    );
  }
  return 'board-$boardGameId';
}
