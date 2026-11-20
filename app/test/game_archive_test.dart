import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';

void main() {
  const String whitePlayerId = 'lp_0123456789abcdef0123456789abcdef';
  const String blackPlayerId = 'lp_fedcba9876543210fedcba9876543210';
  const String initialFen =
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
  const String afterE4 =
      'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';

  test('round trips portable records without private GAME fields', () {
    final GameArchive game = GameArchive(
      libraryId: 'local-1',
      boardGameId: 'board-1',
      createdAt: DateTime.utc(2026, 9, 1, 12),
      updatedAt: DateTime.utc(2026, 9, 1, 12, 1),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 1,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen),
        ArchivedPosition(ply: 1, fen: afterE4),
      ],
      transitions: const <ArchivedTransition>[
        ArchivedTransition(side: GameSide.white, from: 52, to: 36),
      ],
      whitePlayer: ArchivedPlayerAttribution.profile(whitePlayerId),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );

    final Map<String, Object?> encoded = game.toJson();
    expect(encoded, containsPair('whitePlayer', anything));
    expect(encoded, containsPair('blackPlayer', anything));
    expect(encoded, isNot(containsPair('whiteSeat', anything)));
    expect(encoded, isNot(containsPair('blackSeat', anything)));
    final GameArchive decoded = GameArchive.fromJson(
      jsonDecode(jsonEncode(encoded)) as Map<String, Object?>,
    );

    expect(decoded.libraryId, 'local-1');
    expect(decoded.positions.last.fen, afterE4);
    expect(decoded.transitions.single.from, 52);
    expect(decoded.transitions.single.to, 36);
    expect(decoded.whitePlayer.kind, ArchivedPlayerKind.profile);
    expect(decoded.whitePlayer.playerId, whitePlayerId);
    expect(decoded.blackPlayer.kind, ArchivedPlayerKind.guest);
    expect(decoded.blackPlayer.playerId, isNull);
    expect(jsonEncode(decoded.toJson()), isNot(contains('GAME_')));
  });

  test('completed board resignation survives portable Classic archive', () {
    final game = GameArchive(
      libraryId: 'local-resigned',
      boardGameId: 'board-resigned',
      createdAt: DateTime.utc(2026, 10, 12),
      updatedAt: DateTime.utc(2026, 10, 12),
      status: GameArchiveStatus.completed,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.resignation,
      winner: GameWinner.black,
      lineRevision: 1,
      positions: const [ArchivedPosition(ply: 0, fen: initialFen)],
      transitions: const [],
    );
    final decoded = GameArchive.fromJson(game.toJson());
    expect(decoded.outcome, GameOutcome.resignation);
    expect(decoded.winner, GameWinner.black);
    expect(decoded.positions.single.fen, initialFen);
  });

  test('defaults legacy archives without players to unknown attribution', () {
    final Map<String, Object?> legacy = _lineWithPlies(count: 1).toJson();

    final GameArchive decoded = GameArchive.fromJson(legacy);

    expect(decoded.whitePlayer.kind, ArchivedPlayerKind.unknown);
    expect(decoded.blackPlayer.kind, ArchivedPlayerKind.unknown);
    expect(decoded.toJson(), isNot(containsPair('whitePlayer', anything)));
    expect(decoded.toJson(), isNot(containsPair('blackPlayer', anything)));
  });

  test('migrates a legacy board archive to durable board identity', () {
    final Map<String, Object?> legacy = _lineWithPlies(count: 1).toJson()
      ..remove('gameId')
      ..remove('origin')
      ..['schemaVersion'] = legacyGameArchiveSchemaVersion;

    final GameArchive migrated = GameArchive.fromJson(legacy);

    expect(migrated.origin, GameArchiveOrigin.board);
    expect(migrated.gameId, legacy['boardGameId']);
    expect(migrated.boardGameId, legacy['boardGameId']);
    expect(migrated.toJson()['schemaVersion'], gameArchiveSchemaVersion);
  });

  test('round trips an app archive without a physical board identity', () {
    final GameArchive appArchive = GameArchive(
      libraryId: 'app-game-1',
      gameId: 'app-durable-1',
      origin: GameArchiveOrigin.app,
      createdAt: DateTime.utc(2026, 9, 1),
      updatedAt: DateTime.utc(2026, 9, 1),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen),
      ],
      transitions: const <ArchivedTransition>[],
    );

    final GameArchive decoded = GameArchive.fromJson(appArchive.toJson());

    expect(decoded.gameId, 'app-durable-1');
    expect(decoded.origin, GameArchiveOrigin.app);
    expect(decoded.boardGameId, isNull);
    expect(decoded.toJson(), appArchive.toJson());
  });

  test('round trips and preserves a sampled app-game clock', () {
    final GameArchive source = _appClockArchive();

    final Map<String, Object?> encoded = source.toJson();
    final GameArchive decoded = GameArchive.fromJson(
      jsonDecode(jsonEncode(encoded)) as Map<String, Object?>,
    );

    expect(encoded['schemaVersion'], gameArchiveSchemaVersion);
    expect(decoded.clock?.baseMilliseconds, 180000);
    expect(decoded.clock?.incrementMilliseconds, 2000);
    expect(decoded.clock?.whiteRemainingMilliseconds, 182000);
    expect(decoded.clock?.blackRemainingMilliseconds, 179000);
    expect(decoded.clock?.runState, ArchivedGameClockRunState.running);
    expect(decoded.clock?.sampledAtUtc, DateTime.utc(2026, 9, 13, 12, 1));
    expect(decoded.clock?.turnStartCheckpoints, hasLength(2));
    expect(decoded.toJson(), encoded);
    expect(source.rename('Clock game').clock, same(source.clock));
  });

  test('migrates schemas 1 through 4 without adopting a clock field', () {
    final Object clockJson = _appClockArchive().toJson()['clock']!;
    final Map<String, Object?> schema1 = _lineWithPlies(count: 1).toJson()
      ..remove('gameId')
      ..remove('origin')
      ..['schemaVersion'] = legacyGameArchiveSchemaVersion
      ..['clock'] = clockJson;
    final Map<String, Object?> schema2 = _lineWithPlies(count: 1).toJson()
      ..remove('gameId')
      ..remove('origin')
      ..['schemaVersion'] = legacyProviderGameArchiveSchemaVersion
      ..['provider'] = <String, Object?>{
        'provider': 'lichess',
        'gameId': 'providerGame1',
        'accountId': 'account-1',
        'localSide': 'white',
        'rawStatus': 'started',
      }
      ..['clock'] = clockJson;
    final Map<String, Object?> schema3 = _appClockArchive().toJson()
      ..['schemaVersion'] = legacyOriginGameArchiveSchemaVersion;
    final Map<String, Object?> schema4 = _appClockArchive().toJson()
      ..['schemaVersion'] = legacyPlayerGameArchiveSchemaVersion;

    for (final Map<String, Object?> prior in <Map<String, Object?>>[
      schema1,
      schema2,
      schema3,
      schema4,
    ]) {
      final GameArchive migrated = GameArchive.fromJson(prior);
      expect(migrated.clock, isNull);
      expect(migrated.toJson(), isNot(containsPair('clock', anything)));
      expect(migrated.toJson()['schemaVersion'], gameArchiveSchemaVersion);
    }
  });

  test('rejects malformed clock fields and checkpoint misalignment', () {
    final Map<String, Object?> encoded = _appClockArchive().toJson();
    final Map<String, Object?> clock = Map<String, Object?>.of(
      encoded['clock']! as Map<String, Object?>,
    );
    final List<Map<String, Object?>> corruptClocks = <Map<String, Object?>>[
      <String, Object?>{...clock, 'baseMilliseconds': 0},
      <String, Object?>{...clock, 'incrementMilliseconds': -1},
      <String, Object?>{...clock, 'whiteRemainingMilliseconds': -1},
      <String, Object?>{...clock, 'runState': 'unknown'},
      <String, Object?>{...clock}..remove('sampledAtUtc'),
      <String, Object?>{
        ...clock,
        'runState': ArchivedGameClockRunState.suspended.name,
      },
      <String, Object?>{...clock, 'turnStartCheckpoints': <Object?>[]},
      <String, Object?>{
        ...clock,
        'turnStartCheckpoints': <Object?>[
          (clock['turnStartCheckpoints']! as List<Object?>).first,
        ],
      },
      <String, Object?>{
        ...clock,
        'runState': ArchivedGameClockRunState.stopped.name,
      }..remove('sampledAtUtc'),
      <String, Object?>{
        ...clock,
        'sampledAtUtc': DateTime.utc(2026, 9, 13, 12, 2).toIso8601String(),
      },
    ];

    for (final Map<String, Object?> corruptClock in corruptClocks) {
      final Map<String, Object?> corrupt = <String, Object?>{
        ...encoded,
        'clock': corruptClock,
      };
      expect(
        () => GameArchive.fromJson(corrupt),
        throwsFormatException,
        reason: corruptClock.toString(),
      );
    }
  });

  test('rejects corrupted position-aligned intermediate clock checkpoints', () {
    final List<List<ArchivedClockCheckpoint>> corruptHistories =
        <List<ArchivedClockCheckpoint>>[
          const <ArchivedClockCheckpoint>[
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 180000,
              blackRemainingMilliseconds: 180000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 181000,
              blackRemainingMilliseconds: 170000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 181000,
              blackRemainingMilliseconds: 172000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 183000,
              blackRemainingMilliseconds: 172000,
            ),
          ],
          const <ArchivedClockCheckpoint>[
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 180000,
              blackRemainingMilliseconds: 180000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 183000,
              blackRemainingMilliseconds: 180000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 183000,
              blackRemainingMilliseconds: 181000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 184000,
              blackRemainingMilliseconds: 181000,
            ),
          ],
        ];

    for (final List<ArchivedClockCheckpoint> checkpoints in corruptHistories) {
      expect(
        () => _threePlyClockArchive(checkpoints),
        throwsFormatException,
        reason:
            'each move must preserve the inactive balance and add at most '
            'one increment to the mover',
      );
    }
  });

  test(
    'accepts a consistent timeout and rejects contradictory clock winners',
    () {
      final ArchivedGameClock timeoutClock = ArchivedGameClock(
        baseMilliseconds: 180000,
        incrementMilliseconds: 2000,
        whiteRemainingMilliseconds: 182000,
        blackRemainingMilliseconds: 0,
        runState: ArchivedGameClockRunState.stopped,
        turnStartCheckpoints: const <ArchivedClockCheckpoint>[
          ArchivedClockCheckpoint(
            whiteRemainingMilliseconds: 180000,
            blackRemainingMilliseconds: 180000,
          ),
          ArchivedClockCheckpoint(
            whiteRemainingMilliseconds: 182000,
            blackRemainingMilliseconds: 180000,
          ),
        ],
      );
      final GameArchive timedOut = _appClockArchive(
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.timeout,
        winner: GameWinner.white,
        clock: timeoutClock,
      );

      expect(GameArchive.fromJson(timedOut.toJson()).winner, GameWinner.white);
      for (final GameWinner winner in <GameWinner>[
        GameWinner.black,
        GameWinner.draw,
      ]) {
        expect(
          () => _appClockArchive(
            status: GameArchiveStatus.completed,
            outcome: GameOutcome.timeout,
            winner: winner,
            clock: timeoutClock,
          ),
          throwsFormatException,
        );
      }
      expect(
        () => _appClockArchive(
          status: GameArchiveStatus.completed,
          outcome: GameOutcome.timeout,
          winner: GameWinner.white,
          clock: ArchivedGameClock(
            baseMilliseconds: 180000,
            incrementMilliseconds: 2000,
            whiteRemainingMilliseconds: 182000,
            blackRemainingMilliseconds: 1,
            runState: ArchivedGameClockRunState.stopped,
            turnStartCheckpoints: timeoutClock.turnStartCheckpoints,
          ),
        ),
        throwsFormatException,
      );
    },
  );

  test('keeps board, provider, partial, and Duck paths clockless', () {
    final ArchivedGameClock clock = _appClockArchive().clock!;
    expect(
      () => GameArchive(
        libraryId: 'board-clock',
        boardGameId: 'board-clock',
        createdAt: DateTime.utc(2026, 9, 13, 12),
        updatedAt: DateTime.utc(2026, 9, 13, 12, 1),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 1,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen),
          ArchivedPosition(ply: 1, fen: afterE4),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(side: GameSide.white, from: 52, to: 36),
        ],
        clock: clock,
      ),
      throwsFormatException,
    );
    expect(
      () => GameArchive(
        libraryId: 'provider-clock',
        gameId: 'provider_clock',
        origin: GameArchiveOrigin.provider,
        createdAt: DateTime.utc(2026, 9, 13, 12),
        updatedAt: DateTime.utc(2026, 9, 13, 12),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 0,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen),
        ],
        transitions: const <ArchivedTransition>[],
        provider: const ProviderGameProvenance(
          provider: 'lichess',
          gameId: 'providerGame1',
          accountId: 'account-1',
          localSide: 'white',
          rawStatus: 'started',
        ),
        clock: _readyClock(),
      ),
      throwsFormatException,
    );
    expect(
      () => _appClockArchive(
        captureQuality: GameCaptureQuality.partial,
        clock: clock,
      ),
      throwsFormatException,
    );
    expect(
      () => GameArchive(
        libraryId: 'app-duck-clock',
        gameId: 'app_duck_clock',
        origin: GameArchiveOrigin.app,
        createdAt: DateTime.utc(2026, 9, 13, 12),
        updatedAt: DateTime.utc(2026, 9, 13, 12),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.duck,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 0,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen, duckSquare: 28),
        ],
        transitions: const <ArchivedTransition>[],
        clock: _readyClock(),
      ),
      throwsFormatException,
    );
  });

  test('round trips computer attribution without inventing a profile', () {
    final GameArchive appArchive = GameArchive(
      libraryId: 'app-computer-1',
      gameId: 'app_computer_1',
      origin: GameArchiveOrigin.app,
      createdAt: DateTime.utc(2026, 9, 13),
      updatedAt: DateTime.utc(2026, 9, 13),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen),
      ],
      transitions: const <ArchivedTransition>[],
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.computer(),
    );

    final GameArchive decoded = GameArchive.fromJson(appArchive.toJson());

    expect(decoded.blackPlayer.kind, ArchivedPlayerKind.computer);
    expect(decoded.blackPlayer.playerId, isNull);
    expect(decoded.toJson(), appArchive.toJson());
  });

  test('migrates the prior origin-aware schema before computer players', () {
    final Map<String, Object?> prior = GameArchive(
      libraryId: 'app-schema-3',
      gameId: 'app_schema_3',
      origin: GameArchiveOrigin.app,
      createdAt: DateTime.utc(2026, 9, 13),
      updatedAt: DateTime.utc(2026, 9, 13),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen),
      ],
      transitions: const <ArchivedTransition>[],
    ).toJson()..['schemaVersion'] = legacyOriginGameArchiveSchemaVersion;

    final GameArchive migrated = GameArchive.fromJson(prior);

    expect(migrated.gameId, 'app_schema_3');
    expect(migrated.origin, GameArchiveOrigin.app);
    expect(migrated.toJson()['schemaVersion'], gameArchiveSchemaVersion);
  });

  test('rejects a current archive with a missing identity or origin', () {
    final Map<String, Object?> encoded = GameArchive(
      libraryId: 'app-game-2',
      gameId: 'app-durable-2',
      origin: GameArchiveOrigin.app,
      createdAt: DateTime.utc(2026, 9, 1),
      updatedAt: DateTime.utc(2026, 9, 1),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen),
      ],
      transitions: const <ArchivedTransition>[],
    ).toJson();

    for (final String field in <String>['gameId', 'origin']) {
      final Map<String, Object?> corrupt = <String, Object?>{...encoded}
        ..remove(field);
      expect(() => GameArchive.fromJson(corrupt), throwsFormatException);
    }
  });

  test('normalizes, persists, clears, and bounds a user game name', () {
    final GameArchive source = _lineWithPlies(count: 1);
    final GameArchive named = source.rename('  Friday   rematch  ');

    expect(named.displayName, 'Friday rematch');
    expect(named.updatedAt, source.updatedAt);
    expect(GameArchive.fromJson(named.toJson()).displayName, 'Friday rematch');
    expect(named.rename('   ').displayName, isNull);
    expect(
      () => source.rename(
        List<String>.filled(
          gameArchiveDisplayNameMaximumCharacters + 1,
          'x',
        ).join(),
      ),
      throwsFormatException,
    );

    final Map<String, Object?> nonCanonical = source.toJson();
    nonCanonical['displayName'] = ' padded ';
    expect(() => GameArchive.fromJson(nonCanonical), throwsFormatException);
  });

  test('bounds display names by graphemes and encoded storage bytes', () {
    final GameArchive source = _lineWithPlies(count: 1);
    final String decomposedAccent = 'e\u0301';
    final String familyEmoji = '👨‍👩‍👧‍👦';

    expect(
      source
          .rename(List<String>.filled(64, decomposedAccent).join())
          .displayName,
      isNotNull,
    );
    expect(
      source.rename(List<String>.filled(8, familyEmoji).join()).displayName,
      isNotNull,
    );
    expect(
      () => source.rename(List<String>.filled(65, decomposedAccent).join()),
      throwsFormatException,
    );
    expect(
      () => source.rename('a${List<String>.filled(600, '\u0301').join()}'),
      throwsFormatException,
      reason: 'one unbounded grapheme must not bypass the storage byte cap',
    );
  });

  test('rejects malformed or contradictory archived player attribution', () {
    for (final String invalidId in <String>[
      'lp_0123456789ABCDEF0123456789ABCDEF',
      'lp_0123456789abcdef0123456789abcde',
      'profile-0123456789abcdef0123456789abcdef',
    ]) {
      expect(
        () => ArchivedPlayerAttribution.profile(invalidId),
        throwsFormatException,
      );
    }
    expect(
      () => ArchivedPlayerAttribution.fromJson(<String, Object?>{
        'kind': 'profile',
      }),
      throwsFormatException,
    );
    for (final String kind in <String>['guest', 'unknown']) {
      expect(
        () => ArchivedPlayerAttribution.fromJson(<String, Object?>{
          'kind': kind,
          'playerId': whitePlayerId,
        }),
        throwsFormatException,
      );
    }

    final Map<String, Object?> archive = _lineWithPlies(count: 1).toJson();
    archive['whitePlayer'] = null;
    expect(() => GameArchive.fromJson(archive), throwsFormatException);
  });

  test('validates castling, en passant, and promotion board deltas', () {
    final List<AuthoritativeLineSegment> segments = <AuthoritativeLineSegment>[
      AuthoritativeLineSegment(
        boardGameId: 'castle',
        lineRevision: 1,
        status: GameArchiveStatus.active,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: '4k3/8/8/8/8/8/8/4K2R w K - 0 1'),
          ArchivedPosition(ply: 1, fen: '4k3/8/8/8/8/8/8/5RK1 b - - 1 1'),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(
            side: GameSide.white,
            from: 60,
            to: 62,
            secondaryFrom: 63,
            secondaryTo: 61,
          ),
        ],
      ),
      AuthoritativeLineSegment(
        boardGameId: 'en-passant',
        lineRevision: 1,
        status: GameArchiveStatus.active,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: '7k/8/8/3pP3/8/8/8/K7 w - d6 0 2'),
          ArchivedPosition(ply: 1, fen: '7k/8/3P4/8/8/8/8/K7 b - - 0 2'),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(
            side: GameSide.white,
            from: 28,
            to: 19,
            captureSquare: 27,
          ),
        ],
      ),
      AuthoritativeLineSegment(
        boardGameId: 'promotion',
        lineRevision: 1,
        status: GameArchiveStatus.active,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: '7k/P7/8/8/8/8/8/K7 w - - 0 1'),
          ArchivedPosition(ply: 1, fen: 'Q6k/8/8/8/8/8/8/K7 b - - 0 1'),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(
            side: GameSide.white,
            from: 8,
            to: 0,
            promotion: PromotionPiece.queen,
          ),
        ],
      ),
    ];

    expect(segments, hasLength(3));
  });

  test('accepts Duck relocation as one committed transition', () {
    final GameArchive game = GameArchive(
      libraryId: 'duck-1',
      boardGameId: 'board-duck-1',
      createdAt: DateTime.utc(2026, 9, 1),
      updatedAt: DateTime.utc(2026, 9, 1),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.duck,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 1,
      positions: const <ArchivedPosition>[
        ArchivedPosition(ply: 0, fen: initialFen, duckSquare: 28),
        ArchivedPosition(ply: 1, fen: afterE4, duckSquare: 27),
      ],
      transitions: const <ArchivedTransition>[
        ArchivedTransition(
          side: GameSide.white,
          from: 52,
          to: 36,
          duckFrom: 28,
          duckTo: 27,
        ),
      ],
    );

    expect(game.transitions.single.duckFrom, 28);
    expect(game.positions.last.duckSquare, 27);
  });

  test(
    'copies an arbitrary nonterminal prefix into a self-contained branch',
    () {
      final GameArchive source = _lineWithPlies(
        count: 6,
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.checkmate,
        winner: GameWinner.black,
        whitePlayer: ArchivedPlayerAttribution.profile(whitePlayerId),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );

      final GameArchive branch = source.branchAt(
        ply: 4,
        libraryId: 'branch-1',
        boardGameId: 'board-branch-1',
        origin: GameArchiveOrigin.board,
        createdAt: DateTime.utc(2026, 9, 1, 18),
        whitePlayer: const ArchivedPlayerAttribution.guest(),
        blackPlayer: ArchivedPlayerAttribution.profile(blackPlayerId),
      );

      expect(branch.positions, hasLength(5));
      expect(branch.transitions, hasLength(4));
      expect(branch.status, GameArchiveStatus.active);
      expect(branch.outcome, GameOutcome.ongoing);
      expect(branch.winner, GameWinner.none);
      expect(branch.provenance?.sourceLibraryId, source.libraryId);
      expect(branch.provenance?.sourcePly, 4);
      expect(branch.positions.last.fen, source.positions[4].fen);
      expect(branch.whitePlayer.kind, ArchivedPlayerKind.guest);
      expect(branch.blackPlayer.playerId, blackPlayerId);
    },
  );

  test('rejects an app archive carrying a physical board identity', () {
    expect(
      () => GameArchive(
        libraryId: 'app-invalid-board-link',
        boardGameId: 'board-should-not-be-here',
        gameId: 'app-durable-id',
        origin: GameArchiveOrigin.app,
        createdAt: DateTime.utc(2026, 9, 1),
        updatedAt: DateTime.utc(2026, 9, 1),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 0,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen),
        ],
        transitions: const <ArchivedTransition>[],
      ),
      throwsFormatException,
    );
  });

  test(
    'does not offer a terminal final position or a partial line as a branch',
    () {
      final GameArchive completed = _lineWithPlies(
        count: 2,
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.checkmate,
        winner: GameWinner.black,
      );
      final GameArchive partial = _lineWithPlies(
        count: 2,
        captureQuality: GameCaptureQuality.partial,
      );

      expect(completed.canBranchAt(2), isFalse);
      expect(completed.canBranchAt(1), isTrue);
      expect(partial.canBranchAt(1), isFalse);
      expect(
        () => partial.branchAt(
          ply: 1,
          libraryId: 'bad-branch',
          boardGameId: 'bad-board',
          createdAt: DateTime.utc(2026, 9, 1),
          whitePlayer: const ArchivedPlayerAttribution.unknown(),
          blackPlayer: const ArchivedPlayerAttribution.unknown(),
        ),
        throwsStateError,
      );
    },
  );

  test('constructs the existing bounded 128-ply Undo tail automatically', () {
    final GameArchive game = _lineWithPlies(count: 200);

    final UndoSeed seed = game.undoSeedFor(ply: 200);

    expect(seed.basePosition.ply, 72);
    expect(seed.currentPosition.ply, 200);
    expect(seed.transitions, hasLength(128));
    expect(seed.transitions.first.side, GameSide.white);
    expect(seed.transitions.last.side, GameSide.black);
  });

  test(
    'authoritative reconnect replaces an undone suffix by line revision',
    () {
      final GameArchive original = _lineWithPlies(
        count: 6,
        whitePlayer: ArchivedPlayerAttribution.profile(whitePlayerId),
        blackPlayer: const ArchivedPlayerAttribution.guest(),
      );
      final AuthoritativeLineSegment undone = AuthoritativeLineSegment(
        boardGameId: original.boardGameId!,
        lineRevision: 7,
        status: GameArchiveStatus.active,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: original.positions.sublist(2, 5),
        transitions: original.transitions.sublist(2, 4),
      );

      final GameArchive merged = original.mergeAuthoritativeSegment(
        undone,
        observedAt: DateTime.utc(2026, 9, 1, 2),
      );

      expect(merged.currentPly, 4);
      expect(merged.positions, hasLength(5));
      expect(merged.transitions, hasLength(4));
      expect(merged.captureQuality, GameCaptureQuality.complete);
      expect(merged.lineRevision, 7);
      expect(merged.whitePlayer.playerId, whitePlayerId);
      expect(merged.blackPlayer.kind, ArchivedPlayerKind.guest);
      expect(
        merged.mergeAuthoritativeSegment(
          undone,
          observedAt: DateTime.utc(2026, 9, 1, 3),
        ),
        same(merged),
      );

      final AuthoritativeLineSegment conflictingReplay =
          AuthoritativeLineSegment(
            boardGameId: original.boardGameId!,
            lineRevision: 7,
            status: GameArchiveStatus.active,
            variant: GameVariant.classic,
            outcome: GameOutcome.ongoing,
            winner: GameWinner.none,
            positions: <ArchivedPosition>[
              ...original.positions.sublist(2, 4),
              ArchivedPosition(
                ply: 4,
                fen: _fenFromPieces(
                  <int, String>{2: 'k', 8: 'r', 56: 'R', 57: 'K'},
                  whiteToMove: true,
                  fullmoveNumber: 3,
                ),
              ),
            ],
            transitions: <ArchivedTransition>[
              original.transitions[2],
              const ArchivedTransition(side: GameSide.black, from: 1, to: 2),
            ],
          );
      expect(
        () => merged.mergeAuthoritativeSegment(
          conflictingReplay,
          observedAt: DateTime.utc(2026, 9, 1, 4),
        ),
        throwsFormatException,
      );
    },
  );

  test(
    'marks a non-overlapping reconnect tail partial instead of inventing a gap',
    () {
      final GameArchive original = _lineWithPlies(count: 2);
      final GameArchive remoteSource = _lineWithPlies(count: 11);
      final AuthoritativeLineSegment remoteTail = AuthoritativeLineSegment(
        boardGameId: original.boardGameId!,
        lineRevision: 9,
        status: GameArchiveStatus.active,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        positions: remoteSource.positions.sublist(10, 12),
        transitions: remoteSource.transitions.sublist(10, 11),
      );

      final GameArchive merged = original.mergeAuthoritativeSegment(
        remoteTail,
        observedAt: DateTime.utc(2026, 9, 1, 2),
      );

      expect(merged.captureQuality, GameCaptureQuality.partial);
      expect(merged.positions.first.ply, 10);
      expect(merged.canBranchAt(11), isFalse);
    },
  );

  test('requires Duck relocation except for a final king capture', () {
    expect(
      () => GameArchive(
        libraryId: 'duck-missing',
        boardGameId: 'board-duck-missing',
        createdAt: DateTime.utc(2026, 9, 1),
        updatedAt: DateTime.utc(2026, 9, 1),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.duck,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 1,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen, duckSquare: 28),
          ArchivedPosition(ply: 1, fen: afterE4, duckSquare: 28),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(side: GameSide.white, from: 52, to: 36),
        ],
      ),
      throwsFormatException,
    );

    final GameArchive terminal = _duckKingCapture(GameWinner.white);
    expect(terminal.outcome, GameOutcome.kingCaptured);
    expect(() => _duckKingCapture(GameWinner.black), throwsFormatException);
  });

  test('rejects discontinuous plies and malformed paired relocations', () {
    expect(
      () => GameArchive(
        libraryId: 'gap',
        boardGameId: 'board-gap',
        createdAt: DateTime.utc(2026, 9, 1),
        updatedAt: DateTime.utc(2026, 9, 1),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 1,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen),
          ArchivedPosition(ply: 2, fen: afterE4),
        ],
        transitions: const <ArchivedTransition>[
          ArchivedTransition(side: GameSide.white, from: 52, to: 36),
        ],
      ),
      throwsFormatException,
    );
    expect(
      () => const ArchivedTransition(
        side: GameSide.white,
        from: 52,
        to: 36,
        secondaryFrom: 63,
      ).validate(),
      throwsFormatException,
    );
  });

  test('rejects inconsistent terminal outcomes and winners', () {
    expect(
      () => _lineWithPlies(
        count: 1,
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.checkmate,
        winner: GameWinner.none,
      ),
      throwsFormatException,
    );
    expect(
      () => _lineWithPlies(
        count: 1,
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.draw,
        winner: GameWinner.white,
      ),
      throwsFormatException,
    );
    expect(
      _lineWithPlies(
        count: 1,
        status: GameArchiveStatus.completed,
        outcome: GameOutcome.draw,
        winner: GameWinner.draw,
      ).winner,
      GameWinner.draw,
    );
  });

  test('enforces variant-specific terminal outcomes', () {
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.classic,
        outcome: GameOutcome.kingCaptured,
        winner: GameWinner.white,
      ),
      throwsFormatException,
    );
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.classic,
        outcome: GameOutcome.fowled,
        winner: GameWinner.white,
      ),
      throwsFormatException,
    );
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.duck,
        outcome: GameOutcome.checkmate,
        winner: GameWinner.black,
      ),
      throwsFormatException,
    );
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.duck,
        outcome: GameOutcome.draw,
        winner: GameWinner.draw,
      ),
      throwsFormatException,
    );
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.duck,
        outcome: GameOutcome.kingCaptured,
        winner: GameWinner.white,
      ),
      throwsFormatException,
    );
    expect(
      () => _terminalAtInitial(
        variant: GameVariant.duck,
        outcome: GameOutcome.fowled,
        winner: GameWinner.black,
      ),
      throwsFormatException,
    );
    expect(
      _terminalAtInitial(
        variant: GameVariant.duck,
        outcome: GameOutcome.fowled,
        winner: GameWinner.white,
      ).winner,
      GameWinner.white,
    );
  });

  test('accepts timeout only for the side opposing the final turn', () {
    for (final GameVariant variant in GameVariant.values) {
      expect(
        _terminalAtInitial(
          variant: variant,
          outcome: GameOutcome.timeout,
          winner: GameWinner.black,
        ).winner,
        GameWinner.black,
      );
      expect(
        () => _terminalAtInitial(
          variant: variant,
          outcome: GameOutcome.timeout,
          winner: GameWinner.white,
        ),
        throwsFormatException,
      );
    }
  });

  test('requires explicit UTC timestamps when decoding an archive', () {
    for (final String field in <String>['createdAt', 'updatedAt']) {
      final Map<String, Object?> payload =
          jsonDecode(jsonEncode(_lineWithPlies(count: 1).toJson()))
              as Map<String, Object?>;
      payload[field] = '2026-09-01T00:00:00';

      expect(() => GameArchive.fromJson(payload), throwsFormatException);
    }
  });

  test(
    'requires lowercase library identities but accepts opaque board casing',
    () {
      final GameArchive canonical = GameArchive(
        libraryId: 'local-1',
        boardGameId: 'Board-ABC_1',
        createdAt: DateTime.utc(2026, 9, 1),
        updatedAt: DateTime.utc(2026, 9, 1),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 0,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: initialFen),
        ],
        transitions: const <ArchivedTransition>[],
      );

      expect(canonical.boardGameId, 'Board-ABC_1');
      expect(
        () => GameArchive(
          libraryId: 'Local-1',
          boardGameId: 'Board-ABC_1',
          createdAt: DateTime.utc(2026, 9, 1),
          updatedAt: DateTime.utc(2026, 9, 1),
          status: GameArchiveStatus.active,
          captureQuality: GameCaptureQuality.complete,
          variant: GameVariant.classic,
          outcome: GameOutcome.ongoing,
          winner: GameWinner.none,
          lineRevision: 0,
          positions: const <ArchivedPosition>[
            ArchivedPosition(ply: 0, fen: initialFen),
          ],
          transitions: const <ArchivedTransition>[],
        ),
        throwsFormatException,
      );
      expect(
        () => const BranchProvenance(
          sourceLibraryId: 'Source-1',
          sourcePly: 0,
        ).validate(),
        throwsFormatException,
      );
    },
  );

  test('ignores additive JSON fields but rejects invalid FEN', () {
    final Map<String, Object?> payload = _lineWithPlies(count: 1).toJson();
    payload['future'] = <String, Object?>{'value': 1};
    final List<Object?> positions = payload['positions']! as List<Object?>;
    (positions.first! as Map<String, Object?>)['future'] = true;

    expect(GameArchive.fromJson(payload).positions, hasLength(2));

    (positions.first! as Map<String, Object?>)['fen'] = 'not a FEN';
    expect(() => GameArchive.fromJson(payload), throwsFormatException);
  });
}

GameArchive _appClockArchive({
  GameArchiveStatus status = GameArchiveStatus.active,
  GameCaptureQuality captureQuality = GameCaptureQuality.complete,
  GameOutcome outcome = GameOutcome.ongoing,
  GameWinner winner = GameWinner.none,
  ArchivedGameClock? clock,
}) {
  return GameArchive(
    libraryId: 'app-clock-1',
    gameId: 'app_clock_1',
    origin: GameArchiveOrigin.app,
    createdAt: DateTime.utc(2026, 9, 13, 12),
    updatedAt: DateTime.utc(2026, 9, 13, 12, 1),
    status: status,
    captureQuality: captureQuality,
    variant: GameVariant.classic,
    outcome: outcome,
    winner: winner,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
      ArchivedPosition(
        ply: 1,
        fen:
            'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
            'RNBQKBNR b KQkq e3 0 1',
      ),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(side: GameSide.white, from: 52, to: 36),
    ],
    clock:
        clock ??
        ArchivedGameClock(
          baseMilliseconds: 180000,
          incrementMilliseconds: 2000,
          whiteRemainingMilliseconds: 182000,
          blackRemainingMilliseconds: 179000,
          runState: ArchivedGameClockRunState.running,
          sampledAtUtc: DateTime.utc(2026, 9, 13, 12, 1),
          turnStartCheckpoints: const <ArchivedClockCheckpoint>[
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 180000,
              blackRemainingMilliseconds: 180000,
            ),
            ArchivedClockCheckpoint(
              whiteRemainingMilliseconds: 182000,
              blackRemainingMilliseconds: 180000,
            ),
          ],
        ),
  );
}

ArchivedGameClock _readyClock() => ArchivedGameClock(
  baseMilliseconds: 180000,
  incrementMilliseconds: 2000,
  whiteRemainingMilliseconds: 180000,
  blackRemainingMilliseconds: 180000,
  runState: ArchivedGameClockRunState.ready,
  turnStartCheckpoints: const <ArchivedClockCheckpoint>[
    ArchivedClockCheckpoint(
      whiteRemainingMilliseconds: 180000,
      blackRemainingMilliseconds: 180000,
    ),
  ],
);

GameArchive _threePlyClockArchive(List<ArchivedClockCheckpoint> checkpoints) {
  final ArchivedClockCheckpoint currentTurn = checkpoints.last;
  return GameArchive(
    libraryId: 'app-clock-history',
    gameId: 'app_clock_history',
    origin: GameArchiveOrigin.app,
    createdAt: DateTime.utc(2026, 9, 13, 12),
    updatedAt: DateTime.utc(2026, 9, 13, 12, 3),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 3,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
      ArchivedPosition(
        ply: 1,
        fen:
            'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
            'RNBQKBNR b KQkq e3 0 1',
      ),
      ArchivedPosition(
        ply: 2,
        fen:
            'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/'
            'RNBQKBNR w KQkq e6 0 2',
      ),
      ArchivedPosition(
        ply: 3,
        fen:
            'rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/'
            'RNBQKB1R b KQkq - 1 2',
      ),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(side: GameSide.white, from: 52, to: 36),
      ArchivedTransition(side: GameSide.black, from: 12, to: 28),
      ArchivedTransition(side: GameSide.white, from: 62, to: 45),
    ],
    clock: ArchivedGameClock(
      baseMilliseconds: 180000,
      incrementMilliseconds: 2000,
      whiteRemainingMilliseconds: currentTurn.whiteRemainingMilliseconds,
      blackRemainingMilliseconds: currentTurn.blackRemainingMilliseconds - 1000,
      runState: ArchivedGameClockRunState.running,
      sampledAtUtc: DateTime.utc(2026, 9, 13, 12, 3),
      turnStartCheckpoints: checkpoints,
    ),
  );
}

GameArchive _duckKingCapture(GameWinner winner) {
  return GameArchive(
    libraryId: 'duck-terminal',
    boardGameId: 'board-duck-terminal',
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1),
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.duck,
    outcome: GameOutcome.kingCaptured,
    winner: winner,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen: '7k/8/8/8/8/8/8/K6R w - - 0 1',
        duckSquare: 28,
      ),
      ArchivedPosition(
        ply: 1,
        fen: '7R/8/8/8/8/8/8/K7 b - - 0 1',
        duckSquare: 28,
      ),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(
        side: GameSide.white,
        from: 63,
        to: 7,
        captureSquare: 7,
      ),
    ],
  );
}

GameArchive _terminalAtInitial({
  required GameVariant variant,
  required GameOutcome outcome,
  required GameWinner winner,
}) {
  return GameArchive(
    libraryId: 'terminal-${variant.name}-${outcome.name}',
    boardGameId: 'board-${variant.name}-${outcome.name}',
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1),
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: variant,
    outcome: outcome,
    winner: winner,
    lineRevision: 0,
    positions: <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
        duckSquare: variant == GameVariant.duck ? 28 : null,
      ),
    ],
    transitions: const <ArchivedTransition>[],
  );
}

GameArchive _lineWithPlies({
  required int count,
  GameArchiveStatus status = GameArchiveStatus.active,
  GameCaptureQuality captureQuality = GameCaptureQuality.complete,
  GameOutcome outcome = GameOutcome.ongoing,
  GameWinner winner = GameWinner.none,
  ArchivedPlayerAttribution whitePlayer =
      const ArchivedPlayerAttribution.unknown(),
  ArchivedPlayerAttribution blackPlayer =
      const ArchivedPlayerAttribution.unknown(),
}) {
  final List<ArchivedPosition> positions = <ArchivedPosition>[];
  final List<ArchivedTransition> transitions = <ArchivedTransition>[];
  final Map<int, String> pieces = <int, String>{
    0: 'r',
    1: 'k',
    56: 'R',
    57: 'K',
  };
  for (int ply = 0; ply <= count; ply += 1) {
    final bool whiteToMove = ply.isEven;
    positions.add(
      ArchivedPosition(
        ply: ply,
        fen: _fenFromPieces(
          pieces,
          whiteToMove: whiteToMove,
          fullmoveNumber: (ply ~/ 2) + 1,
        ),
      ),
    );
    if (ply != count) {
      final int from;
      final int to;
      if (whiteToMove) {
        from = pieces.containsKey(56) ? 56 : 48;
        to = from == 56 ? 48 : 56;
      } else {
        from = pieces.containsKey(0) ? 0 : 8;
        to = from == 0 ? 8 : 0;
      }
      transitions.add(
        ArchivedTransition(
          side: whiteToMove ? GameSide.white : GameSide.black,
          from: from,
          to: to,
        ),
      );
      pieces[to] = pieces.remove(from)!;
    }
  }
  return GameArchive(
    libraryId: 'source-$count',
    boardGameId: 'board-$count',
    createdAt: DateTime.utc(2026, 9, 1),
    updatedAt: DateTime.utc(2026, 9, 1, 1),
    status: status,
    captureQuality: captureQuality,
    variant: GameVariant.classic,
    outcome: outcome,
    winner: winner,
    lineRevision: count,
    positions: positions,
    transitions: transitions,
    whitePlayer: whitePlayer,
    blackPlayer: blackPlayer,
  );
}

String _fenFromPieces(
  Map<int, String> pieces, {
  required bool whiteToMove,
  required int fullmoveNumber,
}) {
  final List<String> ranks = <String>[];
  for (int rank = 0; rank < 8; rank += 1) {
    final StringBuffer buffer = StringBuffer();
    int empty = 0;
    for (int file = 0; file < 8; file += 1) {
      final String? piece = pieces[(rank * 8) + file];
      if (piece == null) {
        empty += 1;
        continue;
      }
      if (empty != 0) {
        buffer.write(empty);
        empty = 0;
      }
      buffer.write(piece);
    }
    if (empty != 0) {
      buffer.write(empty);
    }
    ranks.add(buffer.toString());
  }
  return '${ranks.join('/')} ${whiteToMove ? 'w' : 'b'} - - 0 '
      '$fullmoveNumber';
}
