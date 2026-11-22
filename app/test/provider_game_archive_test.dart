import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_library_bundle.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const _fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const _boardId = '000102030405060708090a0b0c0d0e0f';
GameArchive _local() => GameArchive(
  libraryId: 'online-1',
  boardGameId: _boardId,
  createdAt: DateTime.utc(2026, 9, 8),
  updatedAt: DateTime.utc(2026, 9, 8),
  status: GameArchiveStatus.active,
  captureQuality: GameCaptureQuality.complete,
  variant: GameVariant.classic,
  outcome: GameOutcome.ongoing,
  winner: GameWinner.none,
  lineRevision: 0,
  positions: const [ArchivedPosition(ply: 0, fen: _fen)],
  transitions: const [],
);
Map<String, Object?> _json(
  String status,
  GameOutcome outcome, {
  String? winner,
}) => {
  ..._local().toJson(),
  'schemaVersion': 2,
  'status': outcome == GameOutcome.ongoing ? 'active' : 'completed',
  'outcome': outcome.name,
  'winner': outcome == GameOutcome.draw ? 'draw' : winner ?? 'none',
  'provider': {
    'provider': 'lichess',
    'gameId': 'game1234',
    'accountId': 'player',
    'localSide': 'white',
    'rawStatus': status,
    'winner': ?winner,
  },
};

void main() {
  final mapping = <String, GameOutcome>{
    'created': GameOutcome.ongoing,
    'started': GameOutcome.ongoing,
    'mate': GameOutcome.checkmate,
    'resign': GameOutcome.resignation,
    'stalemate': GameOutcome.draw,
    'draw': GameOutcome.draw,
    'insufficientMaterialClaim': GameOutcome.draw,
    'timeout': GameOutcome.timeout,
    'outoftime': GameOutcome.timeout,
    'aborted': GameOutcome.aborted,
    'noStart': GameOutcome.aborted,
    'cheat': GameOutcome.providerDefined,
    'unknownFinish': GameOutcome.providerDefined,
    'variantEnd': GameOutcome.providerDefined,
  };
  for (final entry in mapping.entries) {
    test('${entry.key} retains exact cause and absent provider winner', () {
      final raw = _json(entry.key, entry.value);
      final archive = GameArchive.fromJson(raw);
      expect(archive.provider!.rawStatus, entry.key);
      expect(archive.provider!.winner, isNull);
      expect(archive.outcome, entry.value);
      expect(
        archive.winner,
        entry.value == GameOutcome.draw ? GameWinner.draw : GameWinner.none,
      );
      expect(archive.origin, GameArchiveOrigin.provider);
      expect(archive.gameId, _boardId);
      expect(archive.toJson()['schemaVersion'], gameArchiveSchemaVersion);
      expect(GameArchive.fromJson(archive.toJson()).toJson(), archive.toJson());
    });
  }
  test('provider winner is authoritative, not inferred from final turn', () {
    for (final winner in ['white', 'black']) {
      for (final status in [
        'mate',
        'resign',
        'timeout',
        'outoftime',
        'cheat',
      ]) {
        final archive = GameArchive.fromJson(
          _json(status, mapping[status]!, winner: winner),
        );
        expect(archive.provider!.winner, winner);
        expect(archive.winner.name, winner);
      }
    }
    for (final status in [
      'created',
      'started',
      'draw',
      'stalemate',
      'insufficientMaterialClaim',
    ]) {
      expect(
        () => GameArchive.fromJson(
          _json(status, mapping[status]!, winner: 'white'),
        ),
        throwsFormatException,
      );
    }
  });
  test(
    'ordinary schema1 roundtrip is unchanged and provider downgrade fails closed',
    () {
      final old = <String, Object?>{..._local().toJson()}
        ..remove('gameId')
        ..remove('origin')
        ..['schemaVersion'] = legacyGameArchiveSchemaVersion;
      expect(old['schemaVersion'], legacyGameArchiveSchemaVersion);
      expect(old.containsKey('provider'), isFalse);
      expect(GameArchive.fromJson(old).gameId, _boardId);
      final online = _json('started', GameOutcome.ongoing);
      for (final invalid in [
        {...online, 'schemaVersion': 1},
        {...old, 'schemaVersion': 2},
        {...old, 'provider': null},
        {...online, 'schemaVersion': 3},
      ]) {
        expect(() => GameArchive.fromJson(invalid), throwsFormatException);
      }
      // The previous reader's exact admission rule rejects version2, before fields.
      expect(online['schemaVersion'] != gameArchiveSchemaVersion, isTrue);
    },
  );
  test(
    'unknown status and contradictory archive metadata never become results',
    () {
      final online = _json('resign', GameOutcome.resignation);
      for (final status in ['futureFinish', '', 'x' * 65, 'bad\nstatus']) {
        final bad = {
          ...online,
          'provider': {
            ...online['provider'] as Map<String, Object?>,
            'rawStatus': status,
          },
        };
        expect(() => GameArchive.fromJson(bad), throwsFormatException);
      }
      expect(
        () => GameArchive.fromJson({...online, 'outcome': 'checkmate'}),
        throwsFormatException,
      );
      expect(
        () => GameArchive.fromJson({...online, 'winner': 'white'}),
        throwsFormatException,
      );
      expect(
        () => GameArchive.fromJson({...online, 'variant': 'duck'}),
        throwsFormatException,
      );
      final provider = online['provider'] as Map<String, Object?>;
      for (final field in ['provider', 'gameId', 'accountId', 'localSide']) {
        expect(
          () => GameArchive.fromJson({
            ...online,
            'provider': {...provider, field: ''},
          }),
          throwsFormatException,
        );
      }
      for (final id in ['a', 'A1' * 32]) {
        expect(
          ProviderGameProvenance.fromJson({...provider, 'gameId': id}).gameId,
          id,
        );
      }
      expect(
        () =>
            ProviderGameProvenance.fromJson({...provider, 'gameId': 'a' * 65}),
        throwsFormatException,
      );
      expect(
        () =>
            ProviderGameProvenance.fromJson({...provider, 'accountId': 'a\nb'}),
        throwsFormatException,
      );
    },
  );
  test(
    'legal replay retains provider identity; new local branch strips it',
    () {
      final prior = GameArchive.fromJson(_json('started', GameOutcome.ongoing));
      final replayed = replayStandardChessExtension(prior, ['e2e4', 'e7e5']);
      expect(replayed.provider!.toJson(), prior.provider!.toJson());
      expect(replayed.transitions.length, 2);
      expect(() => replayed.undoSeedFor(ply: 1), throwsStateError);
      final branch = replayed.branchAt(
        ply: 1,
        libraryId: 'branch-1',
        boardGameId: 'branch-board',
        origin: GameArchiveOrigin.board,
        createdAt: DateTime.utc(2026, 9, 9),
        whitePlayer: const ArchivedPlayerAttribution.guest(),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );
      expect(branch.provider, isNull);
      expect(branch.undoSeedFor(ply: 1).transitions.length, 1);
      expect(branch.toJson()['schemaVersion'], gameArchiveSchemaVersion);
      expect(branch.outcome, GameOutcome.ongoing);
      expect(branch.provenance!.sourceLibraryId, prior.libraryId);
    },
  );
  test(
    'native wire numbers remain fixed and provider-only outcomes cannot encode',
    () {
      for (final outcome in [
        GameOutcome.aborted,
        GameOutcome.providerDefined,
      ]) {
        expect(
          () => GameHistoryCodec.encodeRecord(
            GameHistorySealRecord(
              outboxSequence: 1,
              boardGameId: _boardId,
              lineRevision: 1,
              status: GameArchiveStatus.completed,
              outcome: outcome,
              winner: GameWinner.white,
            ),
          ),
          throwsFormatException,
        );
      }
      final encoded = GameHistoryCodec.encodeRecord(
        const GameHistorySealRecord(
          outboxSequence: 1,
          boardGameId: _boardId,
          lineRevision: 1,
          status: GameArchiveStatus.completed,
          outcome: GameOutcome.checkmate,
          winner: GameWinner.black,
        ),
      );
      expect(
        (GameHistoryCodec.decodeRecord(encoded) as GameHistorySealRecord)
            .outcome,
        GameOutcome.checkmate,
      );
    },
  );
  test(
    'real repository export/import preserves provider data and rejects native merge',
    () async {
      final dir = await Directory.systemTemp.createTemp('provider-library-');
      final repo = GameLibraryRepository(
        GameLibraryStore(Directory('${dir.path}/source')),
      );
      final imported = GameLibraryRepository(
        GameLibraryStore(Directory('${dir.path}/destination')),
      );
      try {
        final archive = GameArchive.fromJson(
          _json('mate', GameOutcome.checkmate),
        );
        await repo.save(archive);
        final bundle = File('${dir.path}/bundle.json');
        await repo.exportToFile(
          output: bundle,
          exportedAt: DateTime.utc(2026, 9, 8),
        );
        await imported.importFromFile(bundle);
        final importedArchive = await imported.findByIdentity(
          archive.libraryId,
          archive.gameId,
          archive.origin,
        );
        expect(importedArchive?.toJson(), archive.toJson());
        final duplicate = await imported.importFromFile(bundle);
        expect(duplicate.duplicateIds, [archive.libraryId]);
        final before = await File(
          '${dir.path}/source/game-online-1.json',
        ).readAsString();
        final segment = AuthoritativeLineSegment(
          boardGameId: _boardId,
          lineRevision: 1,
          status: GameArchiveStatus.active,
          variant: GameVariant.classic,
          outcome: GameOutcome.ongoing,
          winner: GameWinner.none,
          positions: const [ArchivedPosition(ply: 0, fen: _fen)],
          transitions: const [],
        );
        await expectLater(
          repo.mergeAuthoritativeSegment(
            segment,
            observedAt: DateTime.utc(2026, 9, 9),
          ),
          throwsFormatException,
        );
        expect(
          await File('${dir.path}/source/game-online-1.json').readAsString(),
          before,
        );
      } finally {
        await repo.close();
        await imported.close();
        await dir.delete(recursive: true);
      }
    },
  );
  test(
    'unknown persisted status recovers validated backup without erasing evidence',
    () async {
      final dir = await Directory.systemTemp.createTemp('provider-recovery-');
      final store = GameLibraryStore(dir);
      try {
        final archive = GameArchive.fromJson(
          _json('started', GameOutcome.ongoing),
        );
        await store.save(archive);
        await store.save(archive);
        final current = File('${dir.path}/game-online-1.json');
        final unknown = _json('futureFinish', GameOutcome.providerDefined);
        final bytes = jsonEncode(unknown);
        await current.writeAsString(bytes, flush: true);
        final result = await store.load();
        expect(result.recoveredFiles, [current.path]);
        expect(result.games.single.toJson(), archive.toJson());
        expect(await current.readAsString(), bytes);
        expect(
          () => GameLibraryBundle.decode(
            GameLibraryBundle.encode(
              games: [archive],
              exportedAt: DateTime.utc(2026),
            ),
          ),
          returnsNormally,
        );
      } finally {
        await dir.delete(recursive: true);
      }
    },
  );
}
