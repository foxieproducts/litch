import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';

import '../models/game_archive.dart';
import 'game_library_store.dart';

const String gameLibraryBundleFormat = 'foxie-litch-game-library';
const int gameLibraryBundleSchemaVersion = 1;
const int _maximumBundleBytes = 128 * 1024 * 1024;
const int _maximumBundleGames = 10000;

class DecodedGameLibraryBundle {
  DecodedGameLibraryBundle({
    required this.exportedAt,
    required Iterable<GameArchive> games,
  }) : games = List<GameArchive>.unmodifiable(games);

  final DateTime exportedAt;
  final List<GameArchive> games;
}

class GameLibraryImportResult {
  GameLibraryImportResult({
    required Iterable<String> importedIds,
    required Iterable<String> duplicateIds,
    required Iterable<String> conflictingIds,
  }) : importedIds = List<String>.unmodifiable(importedIds),
       duplicateIds = List<String>.unmodifiable(duplicateIds),
       conflictingIds = List<String>.unmodifiable(conflictingIds);

  final List<String> importedIds;
  final List<String> duplicateIds;
  final List<String> conflictingIds;
}

class GameLibraryBundle {
  const GameLibraryBundle._();

  static String encode({
    required Iterable<GameArchive> games,
    required DateTime exportedAt,
  }) {
    final List<GameArchive> ordered = List<GameArchive>.of(games)
      ..sort((GameArchive left, GameArchive right) {
        return left.libraryId.compareTo(right.libraryId);
      });
    if (ordered.length > _maximumBundleGames) {
      throw const FormatException('Game library contains too many records.');
    }

    final Set<String> identities = <String>{};
    final String prefix =
        '{"format":${jsonEncode(gameLibraryBundleFormat)},'
        '"schemaVersion":$gameLibraryBundleSchemaVersion,'
        '"exportedAt":${jsonEncode(exportedAt.toUtc().toIso8601String())},'
        '"games":[';
    const String suffix = ']}\n';
    final StringBuffer encoded = StringBuffer(prefix);
    int aggregateBytes =
        utf8.encode(prefix).length + utf8.encode(suffix).length;
    bool firstEntry = true;
    for (final GameArchive game in ordered) {
      game.validate();
      if (!identities.add(game.libraryId)) {
        throw const FormatException(
          'Game library contains a duplicate identity.',
        );
      }
      final String entry = jsonEncode(<String, Object?>{
        'sha256': digest(game),
        'game': game.toJson(),
      });
      final int entryBytes = utf8.encode(entry).length + (firstEntry ? 0 : 1);
      if (aggregateBytes + entryBytes > _maximumBundleBytes) {
        throw const FormatException('Game library bundle is too large.');
      }
      if (!firstEntry) {
        encoded.write(',');
      }
      encoded.write(entry);
      aggregateBytes += entryBytes;
      firstEntry = false;
    }
    encoded.write(suffix);
    return encoded.toString();
  }

  static DecodedGameLibraryBundle decode(String source) {
    if (utf8.encode(source).length > _maximumBundleBytes) {
      throw const FormatException('Game library bundle is too large.');
    }
    final Object? decoded = jsonDecode(source);
    if (decoded is! Map<String, Object?> ||
        decoded['format'] != gameLibraryBundleFormat ||
        decoded['schemaVersion'] != gameLibraryBundleSchemaVersion) {
      throw const FormatException('Unsupported game library bundle.');
    }

    final Object? rawExportedAt = decoded['exportedAt'];
    final DateTime? exportedAt = rawExportedAt is String
        ? DateTime.tryParse(rawExportedAt)
        : null;
    final Object? rawGames = decoded['games'];
    if (exportedAt == null || !exportedAt.isUtc || rawGames is! List<Object?>) {
      throw const FormatException('Invalid game library bundle envelope.');
    }
    if (rawGames.length > _maximumBundleGames) {
      throw const FormatException('Game library contains too many records.');
    }

    final Set<String> identities = <String>{};
    final List<GameArchive> games = <GameArchive>[];
    for (final Object? rawEntry in rawGames) {
      if (rawEntry is! Map<String, Object?>) {
        throw const FormatException('Invalid game library entry.');
      }
      final Object? rawDigest = rawEntry['sha256'];
      final Object? rawGame = rawEntry['game'];
      if (rawDigest is! String ||
          !RegExp(r'^[0-9a-f]{64}$').hasMatch(rawDigest) ||
          rawGame is! Map<String, Object?>) {
        throw const FormatException('Invalid game library entry envelope.');
      }
      final GameArchive game = GameArchive.fromJson(rawGame);
      if (_digestArchiveJson(rawGame) != rawDigest) {
        throw const FormatException('Game library entry digest mismatch.');
      }
      if (!identities.add(game.libraryId)) {
        throw const FormatException('Game library bundle repeats an identity.');
      }
      games.add(game);
    }

    return DecodedGameLibraryBundle(exportedAt: exportedAt, games: games);
  }

  static Future<void> exportToFile({
    required File output,
    required Iterable<GameArchive> games,
    required DateTime exportedAt,
  }) async {
    final String encoded = encode(games: games, exportedAt: exportedAt);
    final Directory parent = output.parent;
    await parent.create(recursive: true);
    final File temporary = File('${output.path}.part');
    try {
      await temporary.writeAsString(encoded, flush: true);
      decode(await temporary.readAsString());
      await temporary.rename(output.path);
    } finally {
      if (await temporary.exists()) {
        await temporary.delete();
      }
    }
  }

  static Future<DecodedGameLibraryBundle> readFile(File input) async {
    if (!await input.exists()) {
      throw FileSystemException(
        'Game library bundle is unavailable.',
        input.path,
      );
    }
    final int length = await input.length();
    if (length <= 0 || length > _maximumBundleBytes) {
      throw const FormatException('Game library bundle size is invalid.');
    }
    return decode(await input.readAsString());
  }

  static Future<GameLibraryImportResult> importFromFile({
    required File input,
    required GameLibraryStore store,
  }) async {
    // Decode and validate the complete source before mutating the local store.
    final DecodedGameLibraryBundle bundle = await readFile(input);
    final GameLibrarySnapshot current = await store.load();
    final Map<String, GameArchive> currentById = <String, GameArchive>{
      for (final GameArchive game in current.games) game.libraryId: game,
    };
    final Map<String, GameArchive> currentByIdentity = <String, GameArchive>{
      for (final GameArchive game in current.games)
        _durableIdentity(game): game,
    };
    final Set<String> currentDigests = current.games
        .map<String>(digest)
        .toSet();

    final List<String> importedIds = <String>[];
    final List<String> duplicateIds = <String>[];
    final List<String> conflictingIds = <String>[];
    for (final GameArchive game in bundle.games) {
      final String incomingDigest = digest(game);
      final GameArchive? sameIdentity = currentById[game.libraryId];
      if (sameIdentity != null) {
        if (digest(sameIdentity) == incomingDigest) {
          if (sameIdentity.origin == GameArchiveOrigin.board) {
            await store.clearBoardGameCaptureSuppression(sameIdentity.gameId);
          }
          duplicateIds.add(game.libraryId);
        } else {
          conflictingIds.add(game.libraryId);
        }
        continue;
      }
      final GameArchive? sameDurableIdentity =
          currentByIdentity[_durableIdentity(game)];
      if (sameDurableIdentity != null) {
        if (digest(sameDurableIdentity) == incomingDigest) {
          if (sameDurableIdentity.origin == GameArchiveOrigin.board) {
            await store.clearBoardGameCaptureSuppression(
              sameDurableIdentity.gameId,
            );
          }
          duplicateIds.add(game.libraryId);
        } else {
          conflictingIds.add(game.libraryId);
        }
        continue;
      }
      if (currentDigests.contains(incomingDigest)) {
        if (game.origin == GameArchiveOrigin.board) {
          await store.clearBoardGameCaptureSuppression(game.gameId);
        }
        duplicateIds.add(game.libraryId);
        continue;
      }

      await store.save(game);
      currentById[game.libraryId] = game;
      currentByIdentity[_durableIdentity(game)] = game;
      currentDigests.add(incomingDigest);
      importedIds.add(game.libraryId);
    }

    return GameLibraryImportResult(
      importedIds: importedIds,
      duplicateIds: duplicateIds,
      conflictingIds: conflictingIds,
    );
  }

  static String digest(GameArchive game) {
    return _digestArchiveJson(game.toJson());
  }

  /// Digest of provider-owned archive authority. A local display name is
  /// intentionally excluded so naming cannot invalidate provider reconciliation.
  static String providerAuthorityDigest(GameArchive game) {
    final Map<String, Object?> authority = game.toJson()..remove('displayName');
    return sha256.convert(utf8.encode(jsonEncode(authority))).toString();
  }
}

String _digestArchiveJson(Map<String, Object?> json) {
  return sha256.convert(utf8.encode(jsonEncode(json))).toString();
}

String _durableIdentity(GameArchive game) =>
    '${game.origin.name}:${game.gameId}';
