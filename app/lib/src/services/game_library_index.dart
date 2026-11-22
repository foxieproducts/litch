import 'dart:convert';
import 'dart:io';

import '../models/game_archive.dart';

const String _indexFormat = 'foxie-litch-game-library-index';
const int _indexSchema = 3;

class GameLibraryIndexEntry {
  const GameLibraryIndexEntry({
    required this.libraryId,
    required this.gameId,
    required this.origin,
    required this.updatedAt,
    required this.hasDisplayName,
  });

  final String libraryId;
  final String gameId;
  final GameArchiveOrigin origin;
  final DateTime updatedAt;
  final bool hasDisplayName;

  Map<String, Object?> toJson() => <String, Object?>{
    'libraryId': libraryId,
    'gameId': gameId,
    'origin': origin.name,
    'updatedAt': updatedAt.toUtc().toIso8601String(),
    'hasDisplayName': hasDisplayName,
  };
}

class GameLibraryIndexSnapshot {
  GameLibraryIndexSnapshot({
    required Iterable<GameLibraryIndexEntry> entries,
    this.directoryModifiedMicros,
    Iterable<String> recoveredFiles = const <String>[],
    Iterable<String> rejectedFiles = const <String>[],
  }) : entries = List<GameLibraryIndexEntry>.unmodifiable(entries),
       recoveredFiles = List<String>.unmodifiable(recoveredFiles),
       rejectedFiles = List<String>.unmodifiable(rejectedFiles);

  final List<GameLibraryIndexEntry> entries;
  final int? directoryModifiedMicros;
  final List<String> recoveredFiles;
  final List<String> rejectedFiles;
}

/// A small, atomic catalog of archive identities and ordering timestamps.
///
/// Full archive JSON remains authoritative. The index is disposable: missing,
/// stale, or malformed indexes are rebuilt from the validated archive store.
class GameLibraryStorageIndex {
  const GameLibraryStorageIndex(this.directory);

  final Directory directory;

  Future<GameLibraryIndexSnapshot?> load() async {
    if (await _isInvalidated()) return null;
    final File file = _file;
    if (!await file.exists()) return null;
    final int length = await file.length();
    if (length <= 0) return null;
    try {
      final Object? decoded = jsonDecode(await file.readAsString());
      if (decoded is! Map<String, Object?> ||
          decoded['format'] != _indexFormat ||
          decoded['schemaVersion'] != _indexSchema) {
        return null;
      }
      final Object? rawEntries = decoded['entries'];
      final Object? rawDirectoryModified = decoded['directoryModifiedMicros'];
      final Object? rawRecovered = decoded['recoveredFiles'];
      final Object? rawRejected = decoded['rejectedFiles'];
      if (rawEntries is! List<Object?> ||
          rawDirectoryModified is! int ||
          rawRecovered is! List<Object?> ||
          rawRejected is! List<Object?>) {
        return null;
      }
      final Set<String> ids = <String>{};
      final Set<String> durableIdentities = <String>{};
      final List<GameLibraryIndexEntry> entries = <GameLibraryIndexEntry>[];
      for (final Object? value in rawEntries) {
        if (value is! Map<String, Object?>) return null;
        final Object? rawId = value['libraryId'];
        final Object? rawUpdatedAt = value['updatedAt'];
        final Object? rawGameId = value['gameId'];
        final Object? rawOrigin = value['origin'];
        final Object? rawHasDisplayName = value['hasDisplayName'];
        final DateTime? updatedAt = rawUpdatedAt is String
            ? DateTime.tryParse(rawUpdatedAt)
            : null;
        if (rawId is! String ||
            !isValidGameArchiveLibraryIdentifier(rawId) ||
            !ids.add(rawId) ||
            rawGameId is! String ||
            !isValidGameArchiveGameIdentifier(rawGameId) ||
            rawOrigin is! String ||
            !_originNames.contains(rawOrigin) ||
            !durableIdentities.add('$rawOrigin:$rawGameId') ||
            updatedAt == null ||
            !updatedAt.isUtc ||
            rawHasDisplayName is! bool) {
          return null;
        }
        entries.add(
          GameLibraryIndexEntry(
            libraryId: rawId,
            gameId: rawGameId,
            origin: GameArchiveOrigin.values.byName(rawOrigin),
            updatedAt: updatedAt,
            hasDisplayName: rawHasDisplayName,
          ),
        );
      }
      if (!_isOrdered(entries)) return null;
      final int currentDirectoryModified =
          (await directory.stat()).modified.microsecondsSinceEpoch;
      if (currentDirectoryModified != rawDirectoryModified) return null;
      final List<String>? recovered = _stringList(rawRecovered);
      final List<String>? rejected = _stringList(rawRejected);
      if (recovered == null || rejected == null) return null;
      return GameLibraryIndexSnapshot(
        entries: entries,
        directoryModifiedMicros: rawDirectoryModified,
        recoveredFiles: recovered,
        rejectedFiles: rejected,
      );
    } on FormatException {
      return null;
    } on FileSystemException {
      return null;
    }
  }

  Future<void> replace({
    required Iterable<GameArchive> games,
    required Iterable<String> recoveredFiles,
    required Iterable<String> rejectedFiles,
  }) async {
    final List<GameLibraryIndexEntry> entries = games
        .map(
          (GameArchive game) => GameLibraryIndexEntry(
            libraryId: game.libraryId,
            gameId: game.gameId,
            origin: game.origin,
            updatedAt: game.updatedAt,
            hasDisplayName: game.displayName != null,
          ),
        )
        .toList(growable: false);
    _sort(entries);
    await replaceSnapshot(
      GameLibraryIndexSnapshot(
        entries: entries,
        recoveredFiles: recoveredFiles,
        rejectedFiles: rejectedFiles,
      ),
    );
  }

  /// Makes a stale catalog observably unusable before an archive transaction.
  ///
  /// Directory timestamps are only a fast secondary check: common file systems
  /// expose them at millisecond precision, so they cannot be the transaction
  /// generation for crash recovery.
  Future<void> invalidate() async {
    await _indexDirectory.create(recursive: true);
    await _invalidatedFile.writeAsString('dirty\n', flush: true);
  }

  Future<void> replaceSnapshot(GameLibraryIndexSnapshot snapshot) async {
    await invalidate();
    await _write(snapshot);
  }

  Future<void> recordSaved(
    GameLibraryIndexSnapshot? current,
    GameArchive game,
  ) async {
    if (current == null) return;
    final List<GameLibraryIndexEntry> entries =
        current.entries
            .where(
              (GameLibraryIndexEntry entry) =>
                  entry.libraryId != game.libraryId,
            )
            .toList()
          ..add(
            GameLibraryIndexEntry(
              libraryId: game.libraryId,
              gameId: game.gameId,
              origin: game.origin,
              updatedAt: game.updatedAt,
              hasDisplayName: game.displayName != null,
            ),
          );
    _sort(entries);
    await _write(
      GameLibraryIndexSnapshot(
        entries: entries,
        recoveredFiles: current.recoveredFiles.where(
          (String path) => !_isPathFor(path, game.libraryId),
        ),
        rejectedFiles: current.rejectedFiles.where(
          (String path) => !_isPathFor(path, game.libraryId),
        ),
      ),
    );
  }

  Future<void> recordDeleted(
    GameLibraryIndexSnapshot? current,
    String libraryId,
  ) async {
    if (current == null) return;
    await _write(
      GameLibraryIndexSnapshot(
        entries: current.entries.where(
          (GameLibraryIndexEntry entry) => entry.libraryId != libraryId,
        ),
        recoveredFiles: current.recoveredFiles.where(
          (String path) => !_isPathFor(path, libraryId),
        ),
        rejectedFiles: current.rejectedFiles.where(
          (String path) => !_isPathFor(path, libraryId),
        ),
      ),
    );
  }

  Future<void> _write(GameLibraryIndexSnapshot snapshot) async {
    if (!_hasValidUniqueEntries(snapshot.entries)) {
      throw const FormatException(
        'Game library index contains invalid or duplicate identities.',
      );
    }
    await _indexDirectory.create(recursive: true);
    final int directoryModifiedMicros =
        (await directory.stat()).modified.microsecondsSinceEpoch;
    final String encoded =
        '${jsonEncode(<String, Object?>{'format': _indexFormat, 'schemaVersion': _indexSchema, 'directoryModifiedMicros': directoryModifiedMicros, 'entries': snapshot.entries.map((GameLibraryIndexEntry entry) => entry.toJson()).toList(growable: false), 'recoveredFiles': snapshot.recoveredFiles, 'rejectedFiles': snapshot.rejectedFiles})}\n';
    final File temporary = _temporaryFile;
    try {
      await temporary.writeAsString(encoded, flush: true);
      await temporary.rename(_file.path);
      if (await _invalidatedFile.exists()) await _invalidatedFile.delete();
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Directory get _indexDirectory =>
      Directory('${directory.path}/.game-library-index-v1');
  File get _file => File('${_indexDirectory.path}/catalog.json');
  File get _temporaryFile => File('${_file.path}.tmp');
  File get _invalidatedFile => File('${_indexDirectory.path}/dirty');

  Future<bool> _isInvalidated() async {
    return await FileSystemEntity.type(_invalidatedFile.path) !=
        FileSystemEntityType.notFound;
  }
}

List<String>? _stringList(List<Object?> values) {
  if (values.any((Object? value) => value is! String)) return null;
  return values.cast<String>();
}

bool _isOrdered(List<GameLibraryIndexEntry> entries) {
  for (int index = 1; index < entries.length; index += 1) {
    if (_compare(entries[index - 1], entries[index]) > 0) return false;
  }
  return true;
}

bool _hasValidUniqueEntries(List<GameLibraryIndexEntry> entries) {
  if (!_isOrdered(entries)) return false;
  final Set<String> libraryIds = <String>{};
  final Set<String> durableIdentities = <String>{};
  for (final GameLibraryIndexEntry entry in entries) {
    if (!isValidGameArchiveLibraryIdentifier(entry.libraryId) ||
        !isValidGameArchiveGameIdentifier(entry.gameId) ||
        !entry.updatedAt.isUtc ||
        !libraryIds.add(entry.libraryId) ||
        !durableIdentities.add('${entry.origin.name}:${entry.gameId}')) {
      return false;
    }
  }
  return true;
}

void _sort(List<GameLibraryIndexEntry> entries) => entries.sort(_compare);

int _compare(GameLibraryIndexEntry left, GameLibraryIndexEntry right) {
  final int byUpdate = right.updatedAt.compareTo(left.updatedAt);
  return byUpdate != 0 ? byUpdate : left.libraryId.compareTo(right.libraryId);
}

bool _isPathFor(String path, String libraryId) =>
    File(path).uri.pathSegments.last == 'game-$libraryId.json';

final Set<String> _originNames = GameArchiveOrigin.values
    .map((GameArchiveOrigin origin) => origin.name)
    .toSet();
