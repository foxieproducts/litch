import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

import '../models/game_archive.dart';
import 'game_library_index.dart';

const int _maximumArchiveFileBytes = 2 * 1024 * 1024;

class GameLibrarySnapshot {
  GameLibrarySnapshot({
    Iterable<GameArchive> games = const <GameArchive>[],
    Iterable<String> recoveredFiles = const <String>[],
    Iterable<String> rejectedFiles = const <String>[],
  }) : games = List<GameArchive>.unmodifiable(games),
       recoveredFiles = List<String>.unmodifiable(recoveredFiles),
       rejectedFiles = List<String>.unmodifiable(rejectedFiles);

  final List<GameArchive> games;
  final List<String> recoveredFiles;
  final List<String> rejectedFiles;
}

class GameLibraryArchiveRead {
  const GameLibraryArchiveRead({
    required this.archive,
    this.recoveredFile,
    this.rejectedFile,
  });

  final GameArchive? archive;
  final String? recoveredFile;
  final String? rejectedFile;
}

class GameLibraryStore {
  const GameLibraryStore(this.directory);

  final Directory directory;

  Future<void> save(GameArchive game) async {
    game.validate();
    await directory.create(recursive: true);
    final GameLibraryStorageIndex index = GameLibraryStorageIndex(directory);
    final GameLibraryIndexSnapshot? indexBeforeMutation = await index.load();
    await _rejectDurableIdentityCollision(game, indexBeforeMutation);

    final File target = _gameFile(game.libraryId);
    final File temporary = _temporaryFile(game.libraryId);
    final File previous = _previousFile(game.libraryId);
    final String encoded = '${jsonEncode(game.toJson())}\n';
    if (utf8.encode(encoded).length > _maximumArchiveFileBytes) {
      throw const FileSystemException('Game archive exceeds its file bound.');
    }

    try {
      await temporary.writeAsString(encoded, flush: true);
      await _readArchive(temporary, expectedLibraryId: game.libraryId);
      if (await target.exists()) {
        try {
          await _readArchive(target, expectedLibraryId: game.libraryId);
          await target.copy(previous.path);
        } on FormatException {
          // Preserve the earlier validated backup rather than replacing it
          // with a corrupt current record.
        } on FileSystemException {
          // Preserve the earlier validated backup rather than replacing it
          // with an unreadable current record.
        }
      }
      await index.invalidate();
      await temporary.rename(target.path);
      try {
        await index.recordSaved(indexBeforeMutation, game);
      } on FileSystemException {
        // The archive is already durable. The disposable stale index fails its
        // directory-generation check and is rebuilt on the next catalog read.
      } on FormatException {
        // Do not report a successful archive commit as a failed save merely
        // because its disposable catalog could not be updated.
      }
      if (game.origin == GameArchiveOrigin.board) {
        // Restoration is complete only after the old deletion intent is gone.
        // A crash before this step still leaves the validated archive dominant.
        await clearBoardGameCaptureSuppression(game.gameId);
      }
    } finally {
      if (await temporary.exists()) {
        await temporary.delete();
      }
    }
  }

  Future<GameLibrarySnapshot> load() async {
    if (!await directory.exists()) {
      return GameLibrarySnapshot();
    }

    final List<File> files = <File>[];
    await for (final FileSystemEntity entity in directory.list()) {
      if (entity is File && _isCurrentGameFile(entity)) {
        files.add(entity);
      }
    }
    files.sort((File left, File right) => left.path.compareTo(right.path));

    final List<GameArchive> games = <GameArchive>[];
    final List<String> recoveredFiles = <String>[];
    final List<String> rejectedFiles = <String>[];
    for (final File file in files) {
      final String expectedId = _libraryIdFromFile(file);
      if (!isValidGameArchiveLibraryIdentifier(expectedId)) {
        // Preserve malformed source bytes without deriving a recovery path.
        rejectedFiles.add(file.path);
        continue;
      }
      final GameLibraryArchiveRead result = await read(expectedId);
      if (result.archive case final GameArchive archive) games.add(archive);
      if (result.recoveredFile case final String path) recoveredFiles.add(path);
      if (result.rejectedFile case final String path) rejectedFiles.add(path);
    }

    games.sort((GameArchive left, GameArchive right) {
      final int byUpdate = right.updatedAt.compareTo(left.updatedAt);
      return byUpdate != 0
          ? byUpdate
          : left.libraryId.compareTo(right.libraryId);
    });
    final Map<String, int> durableIdentityCounts = <String, int>{};
    for (final GameArchive game in games) {
      final String identity = '${game.origin.name}:${game.gameId}';
      durableIdentityCounts[identity] =
          (durableIdentityCounts[identity] ?? 0) + 1;
    }
    final List<GameArchive> uniqueGames = <GameArchive>[];
    for (final GameArchive game in games) {
      final String identity = '${game.origin.name}:${game.gameId}';
      if (durableIdentityCounts[identity] == 1) {
        uniqueGames.add(game);
      } else {
        rejectedFiles.add(_gameFile(game.libraryId).path);
      }
    }
    return GameLibrarySnapshot(
      games: uniqueGames,
      recoveredFiles: recoveredFiles,
      rejectedFiles: rejectedFiles,
    );
  }

  Future<void> _rejectDurableIdentityCollision(
    GameArchive candidate,
    GameLibraryIndexSnapshot? index,
  ) async {
    if (index != null &&
        index.recoveredFiles.isEmpty &&
        index.rejectedFiles.isEmpty) {
      for (final GameLibraryIndexEntry entry in index.entries) {
        if (entry.libraryId != candidate.libraryId &&
            entry.origin == candidate.origin &&
            entry.gameId == candidate.gameId) {
          throw const FormatException(
            'Game library already contains this durable identity.',
          );
        }
      }
      return;
    }
    if (!await directory.exists()) return;
    await for (final FileSystemEntity entity in directory.list()) {
      if (entity is! File || !_isCurrentGameFile(entity)) continue;
      final String expectedId = _libraryIdFromFile(entity);
      if (!isValidGameArchiveLibraryIdentifier(expectedId) ||
          expectedId == candidate.libraryId) {
        continue;
      }
      final GameLibraryArchiveRead existing = await read(expectedId);
      final GameArchive? archive = existing.archive;
      if (archive != null &&
          archive.origin == candidate.origin &&
          archive.gameId == candidate.gameId) {
        throw const FormatException(
          'Game library already contains this durable identity.',
        );
      }
    }
  }

  Future<void> delete(String libraryId) async {
    _validateFileIdentifier(libraryId);
    final GameLibraryStorageIndex index = GameLibraryStorageIndex(directory);
    final GameLibraryIndexSnapshot? indexBeforeMutation = await index.load();
    await index.invalidate();
    for (final File file in <File>[
      _gameFile(libraryId),
      _temporaryFile(libraryId),
      _previousFile(libraryId),
    ]) {
      if (await file.exists()) {
        await file.delete();
      }
    }
    try {
      await index.recordDeleted(indexBeforeMutation, libraryId);
    } on FileSystemException {
      // The deletion is authoritative; a stale catalog is rebuilt on read.
    } on FormatException {
      // The deletion is authoritative; a stale catalog is rebuilt on read.
    }
  }

  /// Records user intent before removing any archive bytes. Explicitly restored
  /// archives take precedence; this small marker is only consulted when absent.
  Future<void> suppressBoardGameCapture(String gameId) async {
    final File target = _boardGameDeletionFile(gameId);
    await target.parent.create(recursive: true);
    if (await isBoardGameCaptureSuppressed(gameId)) return;
    final File temporary = File('${target.path}.tmp');
    final String encoded = '$gameId\n';
    try {
      await temporary.writeAsString(encoded, flush: true);
      if (await temporary.readAsString() != encoded) {
        throw FileSystemException(
          'Game deletion marker failed readback.',
          temporary.path,
        );
      }
      await temporary.rename(target.path);
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Future<bool> isBoardGameCaptureSuppressed(String gameId) async {
    final File file = _boardGameDeletionFile(gameId);
    if (await _isMissingArchive(file)) return false;
    final String expected = '$gameId\n';
    if (await FileSystemEntity.type(file.path, followLinks: false) !=
            FileSystemEntityType.file ||
        await file.length() != utf8.encode(expected).length ||
        await file.readAsString() != expected) {
      throw FileSystemException(
        'Game deletion marker is unreadable.',
        file.path,
      );
    }
    return true;
  }

  /// Call only after a matching archive has been durably saved or validated.
  /// Explicit Undo/import retries can finish cleanup without replacing its bytes.
  Future<void> clearBoardGameCaptureSuppression(String gameId) async {
    final File marker = _boardGameDeletionFile(gameId);
    if (await _isMissingArchive(marker)) return;
    if (await FileSystemEntity.type(marker.path, followLinks: false) !=
        FileSystemEntityType.file) {
      throw FileSystemException(
        'Game deletion marker cannot be removed.',
        marker.path,
      );
    }
    await marker.delete();
  }

  File _boardGameDeletionFile(String gameId) {
    if (!isValidGameArchiveGameIdentifier(gameId)) {
      throw const FormatException('Invalid deleted board-game identity.');
    }
    // Preserve the origin namespace and case-sensitive durable identity even
    // on a case-insensitive filesystem. No positions or player data are kept.
    final String identity = sha256
        .convert(utf8.encode('board:$gameId'))
        .toString();
    return File('${directory.path}/.board-game-deletions-v1/$identity');
  }

  /// Reads one exact archive while preserving current/last-good recovery.
  Future<GameLibraryArchiveRead> read(String libraryId) async {
    _validateFileIdentifier(libraryId);
    final File current = _gameFile(libraryId);
    final File previous = _previousFile(libraryId);
    try {
      return GameLibraryArchiveRead(
        archive: await _readArchive(current, expectedLibraryId: libraryId),
      );
    } on FormatException {
      // Try the last validated version without mutating the corrupt source.
    } on FileSystemException {
      // Try the last validated version without mutating the unreadable source.
    }
    try {
      return GameLibraryArchiveRead(
        archive: await _readArchive(previous, expectedLibraryId: libraryId),
        recoveredFile: current.path,
      );
    } on FormatException {
      // Distinguish a genuinely absent record from unusable replacement bytes.
    } on FileSystemException {
      // A failed read alone does not establish absence.
    }
    if (await _isMissingArchive(current) && await _isMissingArchive(previous)) {
      return const GameLibraryArchiveRead(archive: null);
    }
    return GameLibraryArchiveRead(archive: null, rejectedFile: current.path);
  }

  Future<bool> _isMissingArchive(File file) async {
    try {
      await file.length();
      return false;
    } on FileSystemException catch (error) {
      // ENOENT on supported mobile/host platforms. Lookup helpers alone also
      // report notFound for permission errors, and a dangling link is occupied.
      return error.osError?.errorCode == 2 &&
          await FileSystemEntity.type(file.path, followLinks: false) ==
              FileSystemEntityType.notFound;
    }
  }

  Future<GameArchive> _readArchive(
    File file, {
    required String expectedLibraryId,
  }) async {
    if (!await file.exists()) {
      throw FileSystemException('Game archive is unavailable.', file.path);
    }
    final int length = await file.length();
    if (length <= 0 || length > _maximumArchiveFileBytes) {
      throw const FormatException('Game archive file size is invalid.');
    }
    final Object? decoded;
    try {
      decoded = jsonDecode(await file.readAsString());
    } on FormatException {
      rethrow;
    }
    if (decoded is! Map<String, Object?>) {
      throw const FormatException('Game archive root must be an object.');
    }
    final GameArchive archive = GameArchive.fromJson(decoded);
    if (archive.libraryId != expectedLibraryId) {
      throw const FormatException(
        'Game archive identity does not match its filename.',
      );
    }
    return archive;
  }

  File _gameFile(String libraryId) {
    _validateFileIdentifier(libraryId);
    return File('${directory.path}/game-$libraryId.json');
  }

  File _temporaryFile(String libraryId) {
    return File('${_gameFile(libraryId).path}.tmp');
  }

  File _previousFile(String libraryId) {
    return File('${_gameFile(libraryId).path}.previous');
  }
}

bool _isCurrentGameFile(File file) {
  final String name = file.uri.pathSegments.last;
  return name.startsWith('game-') && name.endsWith('.json');
}

String _libraryIdFromFile(File file) {
  final String name = file.uri.pathSegments.last;
  return name.substring('game-'.length, name.length - '.json'.length);
}

void _validateFileIdentifier(String libraryId) {
  if (!isValidGameArchiveLibraryIdentifier(libraryId)) {
    throw const FormatException('Invalid game archive filename identity.');
  }
}
