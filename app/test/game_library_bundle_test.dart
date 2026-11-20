import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:crypto/crypto.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';
import 'package:litch_mobile/src/services/game_library_bundle.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

void main() {
  const String whitePlayerId = 'lp_0123456789abcdef0123456789abcdef';
  late Directory directory;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-game-bundle-');
  });

  tearDown(() async {
    if (await directory.exists()) {
      await directory.delete(recursive: true);
    }
  });

  test(
    'exports all games as one deterministic versioned local bundle',
    () async {
      final File output = File('${directory.path}/history.litchgames');
      final GameArchive later = _game(
        'later',
        minute: 2,
        whitePlayer: ArchivedPlayerAttribution.profile(whitePlayerId),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );
      final GameArchive earlier = _game('earlier', minute: 1);

      await GameLibraryBundle.exportToFile(
        output: output,
        games: <GameArchive>[later, earlier],
        exportedAt: DateTime.utc(2026, 9, 1, 3),
      );
      final DecodedGameLibraryBundle decoded = await GameLibraryBundle.readFile(
        output,
      );

      expect(decoded.exportedAt, DateTime.utc(2026, 9, 1, 3));
      expect(
        decoded.games.map((GameArchive value) => value.libraryId),
        <String>['earlier', 'later'],
      );
      final GameArchive decodedLater = decoded.games.singleWhere(
        (GameArchive game) => game.libraryId == 'later',
      );
      expect(decodedLater.whitePlayer.playerId, whitePlayerId);
      expect(decodedLater.blackPlayer.kind, ArchivedPlayerKind.guest);
      expect(File('${output.path}.part').existsSync(), isFalse);
    },
  );

  test('rejects a modified entry digest before importing anything', () async {
    final File input = File('${directory.path}/history.litchgames');
    final String valid = GameLibraryBundle.encode(
      games: <GameArchive>[_game('incoming', minute: 1)],
      exportedAt: DateTime.utc(2026, 9, 1),
    );
    final Map<String, Object?> payload =
        jsonDecode(valid) as Map<String, Object?>;
    final List<Object?> entries = payload['games']! as List<Object?>;
    (entries.single! as Map<String, Object?>)['sha256'] = List<String>.filled(
      64,
      '0',
    ).join();
    await input.writeAsString(jsonEncode(payload));
    final GameLibraryStore store = GameLibraryStore(
      Directory('${directory.path}/library'),
    );

    await expectLater(
      GameLibraryBundle.importFromFile(input: input, store: store),
      throwsFormatException,
    );
    expect((await store.load()).games, isEmpty);
  });

  test('validates legacy archive bytes before migrating bundle entries', () {
    final List<Map<String, Object?>> legacyGames = <Map<String, Object?>>[
      _legacyArchiveJson(_game('legacy-board', minute: 1), schemaVersion: 1),
      _legacyArchiveJson(_providerGame(), schemaVersion: 2),
    ];

    for (final Map<String, Object?> legacy in legacyGames) {
      final String digest = sha256
          .convert(utf8.encode(jsonEncode(legacy)))
          .toString();
      final String bundle = jsonEncode(<String, Object?>{
        'format': gameLibraryBundleFormat,
        'schemaVersion': gameLibraryBundleSchemaVersion,
        'exportedAt': DateTime.utc(2026, 9, 1).toIso8601String(),
        'games': <Object?>[
          <String, Object?>{'sha256': digest, 'game': legacy},
        ],
      });

      final GameArchive migrated = GameLibraryBundle.decode(
        bundle,
      ).games.single;
      expect(migrated.toJson()['schemaVersion'], gameArchiveSchemaVersion);
      expect(migrated.gameId, legacy['boardGameId']);

      final Map<String, Object?> tampered =
          jsonDecode(bundle) as Map<String, Object?>;
      final List<Object?> entries = tampered['games']! as List<Object?>;
      final Map<String, Object?> envelope =
          entries.single! as Map<String, Object?>;
      final Map<String, Object?> game =
          envelope['game']! as Map<String, Object?>;
      game['updatedAt'] = DateTime.utc(2026, 9, 2).toIso8601String();
      expect(
        () => GameLibraryBundle.decode(jsonEncode(tampered)),
        throwsFormatException,
      );
    }
  });

  test('rejects a bundle timestamp without an explicit UTC offset', () {
    final Map<String, Object?> payload =
        jsonDecode(
              GameLibraryBundle.encode(
                games: <GameArchive>[_game('timestamp', minute: 1)],
                exportedAt: DateTime.utc(2026, 9, 1),
              ),
            )
            as Map<String, Object?>;
    payload['exportedAt'] = '2026-09-01T00:00:00';

    expect(
      () => GameLibraryBundle.decode(jsonEncode(payload)),
      throwsFormatException,
    );
  });

  test(
    'merges new records without overwriting duplicates or conflicts',
    () async {
      final GameLibraryStore store = GameLibraryStore(
        Directory('${directory.path}/library'),
      );
      final GameArchive existing = _game('existing', minute: 1);
      await store.save(existing);

      final File input = File('${directory.path}/history.litchgames');
      await GameLibraryBundle.exportToFile(
        output: input,
        games: <GameArchive>[existing, _game('new-game', minute: 3)],
        exportedAt: DateTime.utc(2026, 9, 1, 4),
      );
      final GameLibraryImportResult first =
          await GameLibraryBundle.importFromFile(input: input, store: store);
      expect(first.duplicateIds, <String>['existing']);
      expect(first.importedIds, <String>['new-game']);

      await GameLibraryBundle.exportToFile(
        output: input,
        games: <GameArchive>[_game('existing', minute: 2)],
        exportedAt: DateTime.utc(2026, 9, 1, 5),
      );
      final GameLibraryImportResult second =
          await GameLibraryBundle.importFromFile(input: input, store: store);

      expect(second.conflictingIds, <String>['existing']);
      final GameLibrarySnapshot snapshot = await store.load();
      expect(snapshot.games, hasLength(2));
      expect(
        snapshot.games
            .singleWhere((GameArchive game) => game.libraryId == 'existing')
            .updatedAt
            .minute,
        1,
      );
    },
  );
}

GameArchive _game(
  String libraryId, {
  required int minute,
  ArchivedPlayerAttribution whitePlayer =
      const ArchivedPlayerAttribution.unknown(),
  ArchivedPlayerAttribution blackPlayer =
      const ArchivedPlayerAttribution.unknown(),
}) {
  return GameArchive(
    libraryId: libraryId,
    boardGameId: 'board-$libraryId',
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1, 0, minute),
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
    whitePlayer: whitePlayer,
    blackPlayer: blackPlayer,
  );
}

GameArchive _providerGame() {
  return GameArchive(
    libraryId: 'legacy-provider',
    boardGameId: 'provider-board-id',
    gameId: 'provider-board-id',
    origin: GameArchiveOrigin.provider,
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1),
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
    provider: const ProviderGameProvenance(
      provider: 'lichess',
      gameId: 'legacyGame',
      accountId: 'player',
      localSide: 'white',
      rawStatus: 'started',
    ),
  );
}

Map<String, Object?> _legacyArchiveJson(
  GameArchive archive, {
  required int schemaVersion,
}) {
  return <String, Object?>{...archive.toJson()}
    ..['schemaVersion'] = schemaVersion
    ..remove('gameId')
    ..remove('origin');
}
