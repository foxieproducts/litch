import 'dart:convert';

import 'home_board_snapshot.dart';
import 'provider_game_provenance.dart';
import 'user_text.dart';

const int legacyGameArchiveSchemaVersion = 1;
const int legacyProviderGameArchiveSchemaVersion = 2;
const int legacyOriginGameArchiveSchemaVersion = 3;
const int legacyPlayerGameArchiveSchemaVersion = 4;
const int gameArchiveSchemaVersion = 5;
const int gameArchiveMaximumPlies = 4096;
const int firmwareUndoTailMaximumPlies = 128;
const int gameArchiveDisplayNameMaximumCharacters = 64;
const int gameArchiveDisplayNameMaximumBytes = 512;
const int archivedGameClockMaximumMilliseconds = 7 * 24 * 60 * 60 * 1000;
const int archivedGameClockMaximumIncrementMilliseconds = 60 * 60 * 1000;
const int _maximumArchiveFenBytes = 159;

enum GameArchiveStatus { active, completed, unfinished }

enum GameCaptureQuality { complete, partial }

enum GameVariant { classic, duck }

/// The authority which created the durable game identity.
///
/// A board identity is an optional transport capability, not a substitute for
/// this identity. In particular, app games must never acquire a synthetic
/// board identity merely to fit the archive schema.
enum GameArchiveOrigin { board, app, provider }

enum GameOutcome {
  ongoing,
  checkmate,
  draw,
  kingCaptured,
  fowled,
  timeout,
  resignation,
  aborted,
  providerDefined,
}

enum GameWinner { none, white, black, draw }

enum GameSide { white, black }

enum PromotionPiece { queen, rook, bishop, knight }

enum ArchivedPlayerKind { profile, guest, computer, unknown }

enum ArchivedGameClockRunState { ready, running, suspended, stopped }

const int archivedPlayerIdLength = 35;
final RegExp _archivedPlayerIdPattern = RegExp(r'^lp_[0-9a-f]{32}$');

bool isValidArchivedPlayerId(String value) {
  return value.length == archivedPlayerIdLength &&
      _archivedPlayerIdPattern.hasMatch(value);
}

class ArchivedPlayerAttribution {
  factory ArchivedPlayerAttribution.profile(String playerId) {
    final ArchivedPlayerAttribution attribution = ArchivedPlayerAttribution._(
      kind: ArchivedPlayerKind.profile,
      playerId: playerId,
    );
    attribution.validate();
    return attribution;
  }

  const ArchivedPlayerAttribution.guest()
    : this._(kind: ArchivedPlayerKind.guest);

  const ArchivedPlayerAttribution.computer()
    : this._(kind: ArchivedPlayerKind.computer);

  const ArchivedPlayerAttribution.unknown()
    : this._(kind: ArchivedPlayerKind.unknown);

  const ArchivedPlayerAttribution._({required this.kind, this.playerId});

  final ArchivedPlayerKind kind;
  final String? playerId;

  void validate() {
    if (kind == ArchivedPlayerKind.profile) {
      if (playerId == null || !isValidArchivedPlayerId(playerId!)) {
        throw const FormatException(
          'Profile attribution requires a valid local player identity.',
        );
      }
      return;
    }
    if (playerId != null) {
      throw const FormatException(
        'Non-profile attribution cannot contain a player identity.',
      );
    }
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'kind': kind.name,
      if (playerId != null) 'playerId': playerId,
    };
  }

  factory ArchivedPlayerAttribution.fromJson(Map<String, Object?> json) {
    final ArchivedPlayerKind kind = _requiredEnum(
      json,
      'kind',
      ArchivedPlayerKind.values,
    );
    final String? playerId = _optionalString(json, 'playerId');
    final ArchivedPlayerAttribution attribution = switch (kind) {
      ArchivedPlayerKind.profile => ArchivedPlayerAttribution.profile(
        playerId ?? '',
      ),
      ArchivedPlayerKind.guest => const ArchivedPlayerAttribution.guest(),
      ArchivedPlayerKind.computer => const ArchivedPlayerAttribution.computer(),
      ArchivedPlayerKind.unknown => const ArchivedPlayerAttribution.unknown(),
    };
    if (kind != ArchivedPlayerKind.profile && playerId != null) {
      throw const FormatException(
        'Non-profile attribution cannot contain a player identity.',
      );
    }
    return attribution;
  }
}

class ArchivedClockCheckpoint {
  const ArchivedClockCheckpoint({
    required this.whiteRemainingMilliseconds,
    required this.blackRemainingMilliseconds,
  });

  final int whiteRemainingMilliseconds;
  final int blackRemainingMilliseconds;

  void validate() {
    if (!_validClockBalance(whiteRemainingMilliseconds) ||
        !_validClockBalance(blackRemainingMilliseconds) ||
        whiteRemainingMilliseconds == 0 ||
        blackRemainingMilliseconds == 0) {
      throw const FormatException(
        'Clock checkpoints require positive bounded balances.',
      );
    }
  }

  Map<String, Object?> toJson() => <String, Object?>{
    'whiteRemainingMilliseconds': whiteRemainingMilliseconds,
    'blackRemainingMilliseconds': blackRemainingMilliseconds,
  };

  factory ArchivedClockCheckpoint.fromJson(Map<String, Object?> json) {
    final ArchivedClockCheckpoint checkpoint = ArchivedClockCheckpoint(
      whiteRemainingMilliseconds: _requiredInt(
        json,
        'whiteRemainingMilliseconds',
      ),
      blackRemainingMilliseconds: _requiredInt(
        json,
        'blackRemainingMilliseconds',
      ),
    );
    checkpoint.validate();
    return checkpoint;
  }
}

/// Durable sampled balances and turn-start history for an app-owned game.
///
/// Live elapsed time is deliberately absent: the session owns its monotonic
/// anchor. [sampledAtUtc] is only a process-relaunch reconciliation anchor.
class ArchivedGameClock {
  factory ArchivedGameClock({
    required int baseMilliseconds,
    required int incrementMilliseconds,
    required int whiteRemainingMilliseconds,
    required int blackRemainingMilliseconds,
    required ArchivedGameClockRunState runState,
    DateTime? sampledAtUtc,
    required Iterable<ArchivedClockCheckpoint> turnStartCheckpoints,
  }) {
    final ArchivedGameClock clock = ArchivedGameClock._(
      baseMilliseconds: baseMilliseconds,
      incrementMilliseconds: incrementMilliseconds,
      whiteRemainingMilliseconds: whiteRemainingMilliseconds,
      blackRemainingMilliseconds: blackRemainingMilliseconds,
      runState: runState,
      sampledAtUtc: sampledAtUtc?.toUtc(),
      turnStartCheckpoints: List<ArchivedClockCheckpoint>.unmodifiable(
        turnStartCheckpoints,
      ),
    );
    clock.validate();
    return clock;
  }

  const ArchivedGameClock._({
    required this.baseMilliseconds,
    required this.incrementMilliseconds,
    required this.whiteRemainingMilliseconds,
    required this.blackRemainingMilliseconds,
    required this.runState,
    required this.sampledAtUtc,
    required this.turnStartCheckpoints,
  });

  final int baseMilliseconds;
  final int incrementMilliseconds;
  final int whiteRemainingMilliseconds;
  final int blackRemainingMilliseconds;
  final ArchivedGameClockRunState runState;
  final DateTime? sampledAtUtc;
  final List<ArchivedClockCheckpoint> turnStartCheckpoints;

  void validate() {
    if (baseMilliseconds <= 0 ||
        baseMilliseconds > archivedGameClockMaximumMilliseconds ||
        incrementMilliseconds < 0 ||
        incrementMilliseconds > archivedGameClockMaximumIncrementMilliseconds ||
        !_validClockBalance(whiteRemainingMilliseconds) ||
        !_validClockBalance(blackRemainingMilliseconds) ||
        turnStartCheckpoints.isEmpty ||
        turnStartCheckpoints.length > gameArchiveMaximumPlies + 1) {
      throw const FormatException('Archived clock values are out of range.');
    }
    for (final ArchivedClockCheckpoint checkpoint in turnStartCheckpoints) {
      checkpoint.validate();
    }
    if ((runState == ArchivedGameClockRunState.running) !=
        (sampledAtUtc != null)) {
      throw const FormatException(
        'Only a running clock may contain a relaunch anchor.',
      );
    }
    if (runState == ArchivedGameClockRunState.ready &&
        (whiteRemainingMilliseconds != baseMilliseconds ||
            blackRemainingMilliseconds != baseMilliseconds)) {
      throw const FormatException('A ready clock must retain its base time.');
    }
  }

  Map<String, Object?> toJson() => <String, Object?>{
    'baseMilliseconds': baseMilliseconds,
    'incrementMilliseconds': incrementMilliseconds,
    'whiteRemainingMilliseconds': whiteRemainingMilliseconds,
    'blackRemainingMilliseconds': blackRemainingMilliseconds,
    'runState': runState.name,
    if (sampledAtUtc != null) 'sampledAtUtc': sampledAtUtc!.toIso8601String(),
    'turnStartCheckpoints': turnStartCheckpoints
        .map((ArchivedClockCheckpoint value) => value.toJson())
        .toList(growable: false),
  };

  factory ArchivedGameClock.fromJson(Map<String, Object?> json) {
    final Object? rawSampledAt = json['sampledAtUtc'];
    return ArchivedGameClock(
      baseMilliseconds: _requiredInt(json, 'baseMilliseconds'),
      incrementMilliseconds: _requiredInt(json, 'incrementMilliseconds'),
      whiteRemainingMilliseconds: _requiredInt(
        json,
        'whiteRemainingMilliseconds',
      ),
      blackRemainingMilliseconds: _requiredInt(
        json,
        'blackRemainingMilliseconds',
      ),
      runState: _requiredEnum(
        json,
        'runState',
        ArchivedGameClockRunState.values,
      ),
      sampledAtUtc: rawSampledAt == null
          ? null
          : _requiredDateTime(json, 'sampledAtUtc'),
      turnStartCheckpoints: _requiredList(json, 'turnStartCheckpoints').map(
        (Object? value) =>
            ArchivedClockCheckpoint.fromJson(_requiredMap(value)),
      ),
    );
  }
}

class ArchivedPosition {
  const ArchivedPosition({
    required this.ply,
    required this.fen,
    this.duckSquare,
  });

  final int ply;
  final String fen;
  final int? duckSquare;

  void validate() {
    if (ply < 0 || ply > gameArchiveMaximumPlies) {
      throw const FormatException('Archive position ply is out of range.');
    }
    if (fen.trim() != fen ||
        fen.isEmpty ||
        utf8.encode(fen).length > _maximumArchiveFenBytes ||
        fen.codeUnits.any((int value) => value < 0x20 || value > 0x7e)) {
      throw const FormatException('Archive position FEN is not bounded ASCII.');
    }
    final HomeChessPosition? parsed = HomeChessPosition.tryParse(fen);
    if (parsed == null || parsed.fen != fen) {
      throw const FormatException('Archive position contains an invalid FEN.');
    }
    final List<String> fenFields = fen.split(' ');
    if (fenFields[2].isEmpty ||
        !RegExp(r'^(?:-|K?Q?k?q?)$').hasMatch(fenFields[2]) ||
        !RegExp(r'^(?:-|[a-h][36])$').hasMatch(fenFields[3])) {
      throw const FormatException(
        'Archive position contains invalid FEN metadata.',
      );
    }
    _validateSquare(duckSquare, field: 'duck square', optional: true);
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'ply': ply,
      'fen': fen,
      if (duckSquare != null) 'duckSquare': duckSquare,
    };
  }

  factory ArchivedPosition.fromJson(Map<String, Object?> json) {
    final ArchivedPosition position = ArchivedPosition(
      ply: _requiredInt(json, 'ply'),
      fen: _requiredString(json, 'fen'),
      duckSquare: _optionalInt(json, 'duckSquare'),
    );
    position.validate();
    return position;
  }
}

class ArchivedTransition {
  const ArchivedTransition({
    required this.side,
    required this.from,
    required this.to,
    this.captureSquare,
    this.promotion,
    this.secondaryFrom,
    this.secondaryTo,
    this.duckFrom,
    this.duckTo,
  });

  final GameSide side;
  final int from;
  final int to;
  final int? captureSquare;
  final PromotionPiece? promotion;
  final int? secondaryFrom;
  final int? secondaryTo;
  final int? duckFrom;
  final int? duckTo;

  void validate() {
    _validateSquare(from, field: 'move origin');
    _validateSquare(to, field: 'move destination');
    if (from == to) {
      throw const FormatException('Archive move must change squares.');
    }
    _validateSquare(captureSquare, field: 'capture square', optional: true);
    _validateSquare(secondaryFrom, field: 'secondary origin', optional: true);
    _validateSquare(
      secondaryTo,
      field: 'secondary destination',
      optional: true,
    );
    _validateSquare(duckFrom, field: 'Duck origin', optional: true);
    _validateSquare(duckTo, field: 'Duck destination', optional: true);

    _validatePair(secondaryFrom, secondaryTo, field: 'secondary relocation');
    _validatePair(duckFrom, duckTo, field: 'Duck relocation');
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'side': side.name,
      'from': from,
      'to': to,
      if (captureSquare != null) 'captureSquare': captureSquare,
      if (promotion != null) 'promotion': promotion!.name,
      if (secondaryFrom != null) 'secondaryFrom': secondaryFrom,
      if (secondaryTo != null) 'secondaryTo': secondaryTo,
      if (duckFrom != null) 'duckFrom': duckFrom,
      if (duckTo != null) 'duckTo': duckTo,
    };
  }

  factory ArchivedTransition.fromJson(Map<String, Object?> json) {
    final ArchivedTransition transition = ArchivedTransition(
      side: _requiredEnum(json, 'side', GameSide.values),
      from: _requiredInt(json, 'from'),
      to: _requiredInt(json, 'to'),
      captureSquare: _optionalInt(json, 'captureSquare'),
      promotion: _optionalEnum(json, 'promotion', PromotionPiece.values),
      secondaryFrom: _optionalInt(json, 'secondaryFrom'),
      secondaryTo: _optionalInt(json, 'secondaryTo'),
      duckFrom: _optionalInt(json, 'duckFrom'),
      duckTo: _optionalInt(json, 'duckTo'),
    );
    transition.validate();
    return transition;
  }
}

class BranchProvenance {
  const BranchProvenance({
    required this.sourceLibraryId,
    required this.sourcePly,
  });

  final String sourceLibraryId;
  final int sourcePly;

  void validate() {
    _validateLibraryIdentifier(
      sourceLibraryId,
      field: 'source library identity',
    );
    if (sourcePly < 0 || sourcePly > gameArchiveMaximumPlies) {
      throw const FormatException('Branch source ply is out of range.');
    }
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'sourceLibraryId': sourceLibraryId,
      'sourcePly': sourcePly,
    };
  }

  factory BranchProvenance.fromJson(Map<String, Object?> json) {
    final BranchProvenance provenance = BranchProvenance(
      sourceLibraryId: _requiredString(json, 'sourceLibraryId'),
      sourcePly: _requiredInt(json, 'sourcePly'),
    );
    provenance.validate();
    return provenance;
  }
}

class UndoSeed {
  UndoSeed({
    required this.basePosition,
    required this.currentPosition,
    required Iterable<ArchivedTransition> transitions,
  }) : transitions = List<ArchivedTransition>.unmodifiable(transitions);

  final ArchivedPosition basePosition;
  final ArchivedPosition currentPosition;
  final List<ArchivedTransition> transitions;
}

class AuthoritativeLineSegment {
  factory AuthoritativeLineSegment({
    required String boardGameId,
    required int lineRevision,
    required GameArchiveStatus status,
    required GameVariant variant,
    required GameOutcome outcome,
    required GameWinner winner,
    required Iterable<ArchivedPosition> positions,
    required Iterable<ArchivedTransition> transitions,
  }) {
    final AuthoritativeLineSegment segment = AuthoritativeLineSegment._(
      boardGameId: boardGameId,
      lineRevision: lineRevision,
      status: status,
      variant: variant,
      outcome: outcome,
      winner: winner,
      positions: List<ArchivedPosition>.unmodifiable(positions),
      transitions: List<ArchivedTransition>.unmodifiable(transitions),
    );
    segment.validate();
    return segment;
  }

  const AuthoritativeLineSegment._({
    required this.boardGameId,
    required this.lineRevision,
    required this.status,
    required this.variant,
    required this.outcome,
    required this.winner,
    required this.positions,
    required this.transitions,
  });

  final String boardGameId;
  final int lineRevision;
  final GameArchiveStatus status;
  final GameVariant variant;
  final GameOutcome outcome;
  final GameWinner winner;
  final List<ArchivedPosition> positions;
  final List<ArchivedTransition> transitions;

  int get basePly => positions.first.ply;

  void validate() {
    _validateBoardIdentifier(boardGameId, field: 'board-game identity');
    if (lineRevision < 0 || lineRevision > 0x7fffffffffffffff) {
      throw const FormatException('Line segment revision is out of range.');
    }
    _validateLine(
      positions: positions,
      transitions: transitions,
      variant: variant,
      status: status,
      outcome: outcome,
      winner: winner,
      requireOpeningPrefix: false,
    );
  }
}

class GameArchive {
  factory GameArchive({
    required String libraryId,
    String? boardGameId,
    String? gameId,
    GameArchiveOrigin? origin,
    required DateTime createdAt,
    required DateTime updatedAt,
    required GameArchiveStatus status,
    required GameCaptureQuality captureQuality,
    required GameVariant variant,
    required GameOutcome outcome,
    required GameWinner winner,
    required int lineRevision,
    required Iterable<ArchivedPosition> positions,
    required Iterable<ArchivedTransition> transitions,
    String? displayName,
    ArchivedPlayerAttribution whitePlayer =
        const ArchivedPlayerAttribution.unknown(),
    ArchivedPlayerAttribution blackPlayer =
        const ArchivedPlayerAttribution.unknown(),
    BranchProvenance? provenance,
    ProviderGameProvenance? provider,
    ArchivedGameClock? clock,
  }) {
    final GameArchiveOrigin resolvedOrigin =
        origin ??
        (provider == null
            ? GameArchiveOrigin.board
            : GameArchiveOrigin.provider);
    final String resolvedGameId = gameId ?? boardGameId ?? libraryId;
    final GameArchive archive = GameArchive._(
      libraryId: libraryId,
      boardGameId: boardGameId,
      gameId: resolvedGameId,
      origin: resolvedOrigin,
      createdAt: createdAt.toUtc(),
      updatedAt: updatedAt.toUtc(),
      status: status,
      captureQuality: captureQuality,
      variant: variant,
      outcome: outcome,
      winner: winner,
      lineRevision: lineRevision,
      positions: List<ArchivedPosition>.unmodifiable(positions),
      transitions: List<ArchivedTransition>.unmodifiable(transitions),
      displayName: displayName,
      whitePlayer: whitePlayer,
      blackPlayer: blackPlayer,
      provenance: provenance,
      provider: provider,
      clock: clock,
    );
    archive.validate();
    return archive;
  }

  const GameArchive._({
    required this.libraryId,
    required this.boardGameId,
    required this.gameId,
    required this.origin,
    required this.createdAt,
    required this.updatedAt,
    required this.status,
    required this.captureQuality,
    required this.variant,
    required this.outcome,
    required this.winner,
    required this.lineRevision,
    required this.positions,
    required this.transitions,
    required this.displayName,
    required this.whitePlayer,
    required this.blackPlayer,
    required this.provenance,
    required this.provider,
    required this.clock,
  });

  final String libraryId;
  final String? boardGameId;
  final String gameId;
  final GameArchiveOrigin origin;
  final DateTime createdAt;
  final DateTime updatedAt;
  final GameArchiveStatus status;
  final GameCaptureQuality captureQuality;
  final GameVariant variant;
  final GameOutcome outcome;
  final GameWinner winner;
  final int lineRevision;
  final List<ArchivedPosition> positions;
  final List<ArchivedTransition> transitions;
  final String? displayName;
  final ArchivedPlayerAttribution whitePlayer;
  final ArchivedPlayerAttribution blackPlayer;
  final BranchProvenance? provenance;
  final ProviderGameProvenance? provider;
  final ArchivedGameClock? clock;

  int get currentPly => positions.last.ply;

  void validate() {
    _validateLibraryIdentifier(libraryId, field: 'library identity');
    _validateBoardIdentifier(gameId, field: 'durable game identity');
    if (boardGameId != null) {
      _validateBoardIdentifier(boardGameId!, field: 'board-game identity');
    }
    if (origin == GameArchiveOrigin.board &&
        (boardGameId == null || gameId != boardGameId)) {
      throw const FormatException(
        'Board archives require their durable and board identities to match.',
      );
    }
    if (origin == GameArchiveOrigin.app && provider != null) {
      throw const FormatException(
        'App archives cannot contain provider evidence.',
      );
    }
    if (origin == GameArchiveOrigin.app && boardGameId != null) {
      throw const FormatException(
        'App archives cannot contain a board-game identity.',
      );
    }
    if (origin == GameArchiveOrigin.provider && provider == null) {
      throw const FormatException(
        'Provider archives require provider evidence.',
      );
    }
    if (provider != null && origin != GameArchiveOrigin.provider) {
      throw const FormatException(
        'Provider evidence requires provider origin.',
      );
    }
    if (updatedAt.isBefore(createdAt)) {
      throw const FormatException('Archive update precedes its creation.');
    }
    if (lineRevision < 0 || lineRevision > 0x7fffffffffffffff) {
      throw const FormatException('Archive line revision is out of range.');
    }
    if (displayName != null &&
        (displayName != normalizeGameArchiveDisplayName(displayName!) ||
            !isGameArchiveDisplayNameWithinBounds(displayName!))) {
      throw const FormatException('Archive display name is invalid.');
    }
    whitePlayer.validate();
    blackPlayer.validate();
    _validateLine(
      positions: positions,
      transitions: transitions,
      variant: variant,
      status: status,
      outcome: outcome,
      winner: winner,
      requireOpeningPrefix: captureQuality == GameCaptureQuality.complete,
      provider: provider,
    );
    _validateClock();
    provenance?.validate();
  }

  void _validateClock() {
    final ArchivedGameClock? value = clock;
    if (value == null) return;
    if (origin != GameArchiveOrigin.app ||
        variant != GameVariant.classic ||
        captureQuality != GameCaptureQuality.complete ||
        boardGameId != null ||
        provider != null) {
      throw const FormatException(
        'Only complete app-owned Classic games may contain a clock.',
      );
    }
    value.validate();
    if (value.turnStartCheckpoints.length != positions.length) {
      throw const FormatException(
        'Clock checkpoints must align with archive positions.',
      );
    }
    final ArchivedClockCheckpoint firstCheckpoint =
        value.turnStartCheckpoints.first;
    if (firstCheckpoint.whiteRemainingMilliseconds != value.baseMilliseconds ||
        firstCheckpoint.blackRemainingMilliseconds != value.baseMilliseconds) {
      throw const FormatException(
        'The first clock checkpoint must contain the base time.',
      );
    }
    for (int index = 1; index < value.turnStartCheckpoints.length; index += 1) {
      final ArchivedClockCheckpoint previous =
          value.turnStartCheckpoints[index - 1];
      final ArchivedClockCheckpoint next = value.turnStartCheckpoints[index];
      final bool validTransition = switch (transitions[index - 1].side) {
        GameSide.white =>
          next.blackRemainingMilliseconds ==
                  previous.blackRemainingMilliseconds &&
              next.whiteRemainingMilliseconds <=
                  previous.whiteRemainingMilliseconds +
                      value.incrementMilliseconds,
        GameSide.black =>
          next.whiteRemainingMilliseconds ==
                  previous.whiteRemainingMilliseconds &&
              next.blackRemainingMilliseconds <=
                  previous.blackRemainingMilliseconds +
                      value.incrementMilliseconds,
      };
      if (!validTransition) {
        throw const FormatException(
          'Clock checkpoint balances contradict the committed move.',
        );
      }
    }
    final bool active = status == GameArchiveStatus.active;
    if (status == GameArchiveStatus.unfinished ||
        active == (value.runState == ArchivedGameClockRunState.stopped)) {
      throw const FormatException(
        'Clock run state contradicts archive lifecycle.',
      );
    }
    if (active &&
        (value.whiteRemainingMilliseconds == 0 ||
            value.blackRemainingMilliseconds == 0)) {
      throw const FormatException(
        'An active clock requires time on both sides.',
      );
    }
    if (value.runState == ArchivedGameClockRunState.ready &&
        (positions.length != 1 || transitions.isNotEmpty)) {
      throw const FormatException('A played game cannot have a ready clock.');
    }
    final DateTime? sampledAtUtc = value.sampledAtUtc;
    if (sampledAtUtc != null &&
        (sampledAtUtc.isBefore(createdAt) || sampledAtUtc.isAfter(updatedAt))) {
      throw const FormatException(
        'Clock relaunch anchor is outside the archive lifetime.',
      );
    }
    final ArchivedClockCheckpoint currentTurn = value.turnStartCheckpoints.last;
    final HomeSide sideToMove = HomeChessPosition.tryParse(
      positions.last.fen,
    )!.sideToMove;
    final bool currentBalancesValid = switch (sideToMove) {
      HomeSide.white =>
        value.whiteRemainingMilliseconds <=
                currentTurn.whiteRemainingMilliseconds &&
            value.blackRemainingMilliseconds ==
                currentTurn.blackRemainingMilliseconds,
      HomeSide.black =>
        value.blackRemainingMilliseconds <=
                currentTurn.blackRemainingMilliseconds &&
            value.whiteRemainingMilliseconds ==
                currentTurn.whiteRemainingMilliseconds,
    };
    if (!currentBalancesValid) {
      throw const FormatException(
        'Current clock balances contradict the turn checkpoint.',
      );
    }
    if (outcome != GameOutcome.timeout) return;
    final bool whiteTimedOut = sideToMove == HomeSide.white;
    final int losingBalance = whiteTimedOut
        ? value.whiteRemainingMilliseconds
        : value.blackRemainingMilliseconds;
    final int winningBalance = whiteTimedOut
        ? value.blackRemainingMilliseconds
        : value.whiteRemainingMilliseconds;
    final GameWinner expectedWinner = whiteTimedOut
        ? GameWinner.black
        : GameWinner.white;
    if (value.runState != ArchivedGameClockRunState.stopped ||
        losingBalance != 0 ||
        winningBalance == 0 ||
        winner != expectedWinner) {
      throw const FormatException(
        'Clock timeout balances contradict the archive winner.',
      );
    }
  }

  GameArchive rename(String? value) {
    final String? normalized = value == null
        ? null
        : normalizeGameArchiveDisplayName(value);
    if (normalized != null &&
        !isGameArchiveDisplayNameWithinBounds(normalized)) {
      throw const FormatException('Archive display name is too long.');
    }
    if (normalized == displayName) {
      return this;
    }
    return GameArchive(
      libraryId: libraryId,
      boardGameId: boardGameId,
      gameId: gameId,
      origin: origin,
      createdAt: createdAt,
      updatedAt: updatedAt,
      status: status,
      captureQuality: captureQuality,
      variant: variant,
      outcome: outcome,
      winner: winner,
      lineRevision: lineRevision,
      positions: positions,
      transitions: transitions,
      displayName: normalized,
      whitePlayer: whitePlayer,
      blackPlayer: blackPlayer,
      provenance: provenance,
      provider: provider,
      clock: clock,
    );
  }

  GameArchive assignPlayers({
    required ArchivedPlayerAttribution whitePlayer,
    required ArchivedPlayerAttribution blackPlayer,
  }) => GameArchive(
    libraryId: libraryId,
    boardGameId: boardGameId,
    gameId: gameId,
    origin: origin,
    createdAt: createdAt,
    updatedAt: updatedAt,
    status: status,
    captureQuality: captureQuality,
    variant: variant,
    outcome: outcome,
    winner: winner,
    lineRevision: lineRevision,
    positions: positions,
    transitions: transitions,
    displayName: displayName,
    whitePlayer: whitePlayer,
    blackPlayer: blackPlayer,
    provenance: provenance,
    provider: provider,
    clock: clock,
  );

  bool canBranchAt(int ply) {
    if (captureQuality != GameCaptureQuality.complete ||
        ply < 0 ||
        ply > currentPly) {
      return false;
    }
    return !(status == GameArchiveStatus.completed && ply == currentPly);
  }

  GameArchive branchAt({
    required int ply,
    required String libraryId,
    String? boardGameId,
    String? gameId,
    GameArchiveOrigin origin = GameArchiveOrigin.app,
    required DateTime createdAt,
    required ArchivedPlayerAttribution whitePlayer,
    required ArchivedPlayerAttribution blackPlayer,
  }) {
    if (!canBranchAt(ply)) {
      throw StateError('The selected archive position cannot start a branch.');
    }
    return GameArchive(
      libraryId: libraryId,
      boardGameId: boardGameId,
      gameId: gameId,
      origin: origin,
      createdAt: createdAt,
      updatedAt: createdAt,
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: variant,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: positions.take(ply + 1),
      transitions: transitions.take(ply),
      whitePlayer: whitePlayer,
      blackPlayer: blackPlayer,
      provenance: BranchProvenance(
        sourceLibraryId: this.libraryId,
        sourcePly: ply,
      ),
    );
  }

  UndoSeed undoSeedFor({
    required int ply,
    int maximumPlies = firmwareUndoTailMaximumPlies,
  }) {
    if (provider != null || boardGameId == null) {
      throw StateError('Only board archives can construct firmware Undo.');
    }
    if (captureQuality != GameCaptureQuality.complete) {
      throw StateError('Partial archives cannot construct firmware Undo.');
    }
    if (ply < 0 || ply > currentPly) {
      throw RangeError.range(ply, 0, currentPly, 'ply');
    }
    if (maximumPlies < 0 || maximumPlies > firmwareUndoTailMaximumPlies) {
      throw RangeError.range(
        maximumPlies,
        0,
        firmwareUndoTailMaximumPlies,
        'maximumPlies',
      );
    }
    final int firstPly = positions.first.ply;
    final int basePly = ply - firstPly > maximumPlies
        ? ply - maximumPlies
        : firstPly;
    final int baseIndex = basePly - firstPly;
    final int currentIndex = ply - firstPly;
    return UndoSeed(
      basePosition: positions[baseIndex],
      currentPosition: positions[currentIndex],
      transitions: transitions.sublist(baseIndex, currentIndex),
    );
  }

  GameArchive mergeAuthoritativeSegment(
    AuthoritativeLineSegment segment, {
    required DateTime observedAt,
  }) {
    if (provider != null) {
      throw const FormatException(
        'Native history cannot mutate a provider-confirmed archive.',
      );
    }
    if (boardGameId == null ||
        segment.boardGameId != boardGameId ||
        segment.variant != variant) {
      throw const FormatException(
        'Authoritative segment belongs to a different game.',
      );
    }
    if (segment.lineRevision < lineRevision) {
      return this;
    }
    if (segment.lineRevision == lineRevision) {
      return _mergeEqualRevisionSegment(segment, observedAt: observedAt);
    }

    if (status == GameArchiveStatus.unfinished) {
      throw const FormatException(
        'A newer line revision cannot change an unfinished archive.',
      );
    }

    final int overlapIndex = segment.basePly - positions.first.ply;
    final bool hasMatchingOverlap =
        overlapIndex >= 0 &&
        overlapIndex < positions.length &&
        _samePosition(positions[overlapIndex], segment.positions.first);

    final List<ArchivedPosition> mergedPositions;
    final List<ArchivedTransition> mergedTransitions;
    final GameCaptureQuality mergedQuality;
    if (segment.basePly == 0) {
      mergedPositions = segment.positions;
      mergedTransitions = segment.transitions;
      mergedQuality = GameCaptureQuality.complete;
    } else if (hasMatchingOverlap) {
      mergedPositions = <ArchivedPosition>[
        ...positions.take(overlapIndex),
        ...segment.positions,
      ];
      mergedTransitions = <ArchivedTransition>[
        ...transitions.take(overlapIndex),
        ...segment.transitions,
      ];
      mergedQuality = captureQuality;
    } else {
      mergedPositions = segment.positions;
      mergedTransitions = segment.transitions;
      mergedQuality = GameCaptureQuality.partial;
    }

    return GameArchive(
      libraryId: libraryId,
      boardGameId: boardGameId,
      gameId: gameId,
      origin: origin,
      createdAt: createdAt,
      updatedAt: observedAt,
      status: segment.status,
      captureQuality: mergedQuality,
      variant: variant,
      outcome: segment.outcome,
      winner: segment.winner,
      lineRevision: segment.lineRevision,
      positions: mergedPositions,
      transitions: mergedTransitions,
      displayName: displayName,
      whitePlayer: whitePlayer,
      blackPlayer: blackPlayer,
      provenance: provenance,
    );
  }

  GameArchive _mergeEqualRevisionSegment(
    AuthoritativeLineSegment segment, {
    required DateTime observedAt,
  }) {
    final int start = segment.basePly - positions.first.ply;
    if (start < 0 || start >= positions.length) {
      throw const FormatException(
        'Equal line revisions do not overlap durable history.',
      );
    }

    final int overlappingPositionCount =
        segment.positions.length < positions.length - start
        ? segment.positions.length
        : positions.length - start;
    for (int index = 0; index < overlappingPositionCount; index += 1) {
      if (!_samePosition(segment.positions[index], positions[start + index])) {
        throw const FormatException(
          'Equal line revisions contain conflicting authoritative history.',
        );
      }
    }

    final int existingTransitionsFromStart = transitions.length - start;
    final int overlappingTransitionCount =
        segment.transitions.length < existingTransitionsFromStart
        ? segment.transitions.length
        : existingTransitionsFromStart;
    for (int index = 0; index < overlappingTransitionCount; index += 1) {
      if (!_sameTransition(
        segment.transitions[index],
        transitions[start + index],
      )) {
        throw const FormatException(
          'Equal line revisions contain conflicting authoritative history.',
        );
      }
    }

    final bool archiveIsSealed = status != GameArchiveStatus.active;
    final bool segmentIsSealed = segment.status != GameArchiveStatus.active;
    final bool extendsLine = segment.positions.last.ply > positions.last.ply;
    if (extendsLine) {
      if (archiveIsSealed) {
        throw const FormatException(
          'Equal line revisions cannot extend a sealed archive.',
        );
      }
      return GameArchive(
        libraryId: libraryId,
        boardGameId: boardGameId,
        gameId: gameId,
        origin: origin,
        createdAt: createdAt,
        updatedAt: observedAt,
        status: segment.status,
        captureQuality: captureQuality,
        variant: variant,
        outcome: segment.outcome,
        winner: segment.winner,
        lineRevision: lineRevision,
        positions: <ArchivedPosition>[
          ...positions,
          ...segment.positions.skip(overlappingPositionCount),
        ],
        transitions: <ArchivedTransition>[
          ...transitions,
          ...segment.transitions.skip(overlappingTransitionCount),
        ],
        displayName: displayName,
        whitePlayer: whitePlayer,
        blackPlayer: blackPlayer,
        provenance: provenance,
      );
    }

    if (!segmentIsSealed) {
      return this;
    }
    if (segment.positions.last.ply != positions.last.ply) {
      throw const FormatException(
        'Equal line revisions cannot seal an earlier archive position.',
      );
    }
    if (archiveIsSealed) {
      if (segment.status == status &&
          segment.outcome == outcome &&
          segment.winner == winner) {
        return this;
      }
      throw const FormatException(
        'Equal line revisions contain conflicting archive seals.',
      );
    }
    return GameArchive(
      libraryId: libraryId,
      boardGameId: boardGameId,
      gameId: gameId,
      origin: origin,
      createdAt: createdAt,
      updatedAt: observedAt,
      status: segment.status,
      captureQuality: captureQuality,
      variant: variant,
      outcome: segment.outcome,
      winner: segment.winner,
      lineRevision: lineRevision,
      positions: positions,
      transitions: transitions,
      displayName: displayName,
      whitePlayer: whitePlayer,
      blackPlayer: blackPlayer,
      provenance: provenance,
    );
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'schemaVersion': gameArchiveSchemaVersion,
      'libraryId': libraryId,
      'gameId': gameId,
      'origin': origin.name,
      if (boardGameId != null) 'boardGameId': boardGameId,
      'createdAt': createdAt.toUtc().toIso8601String(),
      'updatedAt': updatedAt.toUtc().toIso8601String(),
      'status': status.name,
      'captureQuality': captureQuality.name,
      'variant': variant.name,
      'outcome': outcome.name,
      'winner': winner.name,
      'lineRevision': lineRevision,
      if (displayName != null) 'displayName': displayName,
      'positions': positions
          .map((ArchivedPosition value) => value.toJson())
          .toList(growable: false),
      'transitions': transitions
          .map((ArchivedTransition value) => value.toJson())
          .toList(growable: false),
      if (whitePlayer.kind != ArchivedPlayerKind.unknown)
        'whitePlayer': whitePlayer.toJson(),
      if (blackPlayer.kind != ArchivedPlayerKind.unknown)
        'blackPlayer': blackPlayer.toJson(),
      if (provenance != null) 'provenance': provenance!.toJson(),
      if (provider != null) 'provider': provider!.toJson(),
      if (clock != null) 'clock': clock!.toJson(),
    };
  }

  factory GameArchive.fromJson(Map<String, Object?> json) {
    final schema = _requiredInt(json, 'schemaVersion');
    if (schema != legacyGameArchiveSchemaVersion &&
        schema != legacyProviderGameArchiveSchemaVersion &&
        schema != legacyOriginGameArchiveSchemaVersion &&
        schema != legacyPlayerGameArchiveSchemaVersion &&
        schema != gameArchiveSchemaVersion) {
      throw const FormatException('Unsupported game archive schema.');
    }
    if ((schema == legacyProviderGameArchiveSchemaVersion &&
            json['provider'] == null) ||
        (schema == legacyGameArchiveSchemaVersion &&
            json.containsKey('provider'))) {
      throw const FormatException(
        'Provider provenance requires archive schema 2.',
      );
    }
    final List<Object?> rawPositions = _requiredList(json, 'positions');
    final List<Object?> rawTransitions = _requiredList(json, 'transitions');
    final Object? rawProvenance = json['provenance'];

    return GameArchive(
      libraryId: _requiredString(json, 'libraryId'),
      boardGameId: _optionalString(json, 'boardGameId'),
      gameId: schema >= legacyOriginGameArchiveSchemaVersion
          ? _requiredString(json, 'gameId')
          : _requiredString(json, 'boardGameId'),
      origin: schema >= legacyOriginGameArchiveSchemaVersion
          ? _requiredEnum(json, 'origin', GameArchiveOrigin.values)
          : (schema == legacyProviderGameArchiveSchemaVersion
                ? GameArchiveOrigin.provider
                : GameArchiveOrigin.board),
      createdAt: _requiredDateTime(json, 'createdAt'),
      updatedAt: _requiredDateTime(json, 'updatedAt'),
      status: _requiredEnum(json, 'status', GameArchiveStatus.values),
      captureQuality: _requiredEnum(
        json,
        'captureQuality',
        GameCaptureQuality.values,
      ),
      variant: _requiredEnum(json, 'variant', GameVariant.values),
      outcome: _requiredEnum(json, 'outcome', GameOutcome.values),
      winner: _requiredEnum(json, 'winner', GameWinner.values),
      lineRevision: _requiredInt(json, 'lineRevision'),
      displayName: _optionalString(json, 'displayName'),
      positions: rawPositions.map(
        (Object? value) => ArchivedPosition.fromJson(_requiredMap(value)),
      ),
      transitions: rawTransitions.map(
        (Object? value) => ArchivedTransition.fromJson(_requiredMap(value)),
      ),
      whitePlayer: _optionalPlayerAttribution(json, 'whitePlayer'),
      blackPlayer: _optionalPlayerAttribution(json, 'blackPlayer'),
      provider:
          (schema == legacyProviderGameArchiveSchemaVersion ||
              schema >= legacyOriginGameArchiveSchemaVersion &&
                  json.containsKey('provider'))
          ? ProviderGameProvenance.fromJson(_requiredMap(json['provider']))
          : null,
      provenance: rawProvenance == null
          ? null
          : BranchProvenance.fromJson(_requiredMap(rawProvenance)),
      clock: schema >= gameArchiveSchemaVersion && json.containsKey('clock')
          ? ArchivedGameClock.fromJson(_requiredMap(json['clock']))
          : null,
    );
  }
}

String? normalizeGameArchiveDisplayName(String value) {
  final String normalized = value.trim().replaceAll(RegExp(r'\s+'), ' ');
  return normalized.isEmpty ? null : normalized;
}

bool isGameArchiveDisplayNameWithinBounds(String value) {
  return isDisplayTextWithinBounds(
    value,
    maxCharacters: gameArchiveDisplayNameMaximumCharacters,
    maxBytes: gameArchiveDisplayNameMaximumBytes,
  );
}

ArchivedPlayerAttribution _optionalPlayerAttribution(
  Map<String, Object?> json,
  String key,
) {
  if (!json.containsKey(key)) {
    return const ArchivedPlayerAttribution.unknown();
  }
  return ArchivedPlayerAttribution.fromJson(_requiredMap(json[key]));
}

void _validateLine({
  required List<ArchivedPosition> positions,
  required List<ArchivedTransition> transitions,
  required GameVariant variant,
  required GameArchiveStatus status,
  required GameOutcome outcome,
  required GameWinner winner,
  required bool requireOpeningPrefix,
  ProviderGameProvenance? provider,
}) {
  if (positions.isEmpty || positions.length > gameArchiveMaximumPlies + 1) {
    throw const FormatException('Archive position count is out of range.');
  }
  if (transitions.length != positions.length - 1) {
    throw const FormatException(
      'Archive transitions must connect every adjacent position.',
    );
  }
  if (requireOpeningPrefix && positions.first.ply != 0) {
    throw const FormatException('A complete archive must begin at ply zero.');
  }

  final int firstPly = positions.first.ply;
  for (int index = 0; index < positions.length; index += 1) {
    final ArchivedPosition position = positions[index];
    position.validate();
    if (position.ply != firstPly + index) {
      throw const FormatException('Archive plies must be contiguous.');
    }
    if (variant == GameVariant.classic && position.duckSquare != null) {
      throw const FormatException('Classic archives cannot contain a Duck.');
    }
    if (variant == GameVariant.duck && position.duckSquare == null) {
      throw const FormatException('Duck archives require a Duck square.');
    }
    if (position.duckSquare != null &&
        HomeChessPosition.tryParse(
          position.fen,
        )!.pieces.containsKey(position.duckSquare)) {
      throw const FormatException('A Duck cannot share a square with a piece.');
    }
  }

  bool finalTransitionCapturedKing = false;
  for (int index = 0; index < transitions.length; index += 1) {
    final ArchivedTransition transition = transitions[index];
    transition.validate();
    final HomeChessPosition before = HomeChessPosition.tryParse(
      positions[index].fen,
    )!;
    final HomeChessPosition after = HomeChessPosition.tryParse(
      positions[index + 1].fen,
    )!;
    final GameSide expectedSide = before.sideToMove == HomeSide.white
        ? GameSide.white
        : GameSide.black;
    if (transition.side != expectedSide) {
      throw const FormatException(
        'Archive transition side disagrees with its source FEN.',
      );
    }
    final bool capturedKing = _validateTransitionBoard(
      before: before,
      after: after,
      transition: transition,
    );
    if (index == transitions.length - 1) {
      finalTransitionCapturedKing = capturedKing;
    }
    if (variant == GameVariant.classic) {
      if (transition.duckFrom != null || transition.duckTo != null) {
        throw const FormatException(
          'Classic archive transitions cannot relocate a Duck.',
        );
      }
      if (capturedKing) {
        throw const FormatException(
          'Classic transitions cannot capture a king.',
        );
      }
      continue;
    }

    final int? beforeDuck = positions[index].duckSquare;
    final int? afterDuck = positions[index + 1].duckSquare;
    final bool terminalKingCapture =
        index == transitions.length - 1 &&
        status == GameArchiveStatus.completed &&
        outcome == GameOutcome.kingCaptured &&
        capturedKing;
    if (transition.duckFrom == null || transition.duckTo == null) {
      if (!terminalKingCapture || beforeDuck != afterDuck) {
        throw const FormatException(
          'Duck transitions require a relocation except for final king capture.',
        );
      }
      continue;
    }
    if (capturedKing) {
      throw const FormatException(
        'A terminal king capture cannot be followed by Duck relocation.',
      );
    }
    if (transition.duckFrom != beforeDuck || transition.duckTo != afterDuck) {
      throw const FormatException(
        'Duck relocation disagrees with its adjacent positions.',
      );
    }
  }

  if (provider != null) {
    provider.validate();
    final category = provider.category;
    final mappedOutcome = switch (category) {
      ProviderResultCategory.ongoing => GameOutcome.ongoing,
      ProviderResultCategory.checkmate => GameOutcome.checkmate,
      ProviderResultCategory.resignation => GameOutcome.resignation,
      ProviderResultCategory.draw => GameOutcome.draw,
      ProviderResultCategory.timeout => GameOutcome.timeout,
      ProviderResultCategory.aborted => GameOutcome.aborted,
      ProviderResultCategory.providerDefined => GameOutcome.providerDefined,
    };
    final mappedWinner = category == ProviderResultCategory.draw
        ? GameWinner.draw
        : switch (provider.winner) {
            'white' => GameWinner.white,
            'black' => GameWinner.black,
            _ => GameWinner.none,
          };
    final mappedStatus = category == ProviderResultCategory.ongoing
        ? GameArchiveStatus.active
        : GameArchiveStatus.completed;
    if (variant != GameVariant.classic ||
        outcome != mappedOutcome ||
        winner != mappedWinner ||
        status != mappedStatus) {
      throw const FormatException(
        'Archive result contradicts provider evidence.',
      );
    }
    return;
  }

  if (status == GameArchiveStatus.completed && outcome == GameOutcome.ongoing) {
    throw const FormatException('Completed archives require an outcome.');
  }
  if (status != GameArchiveStatus.completed && outcome != GameOutcome.ongoing) {
    throw const FormatException(
      'Only completed archives may contain a terminal outcome.',
    );
  }
  if (outcome == GameOutcome.ongoing && winner != GameWinner.none) {
    throw const FormatException('An ongoing game cannot have a winner.');
  }
  if (outcome == GameOutcome.draw && winner != GameWinner.draw) {
    throw const FormatException('A drawn game requires a draw result.');
  }
  if (outcome != GameOutcome.ongoing &&
      outcome != GameOutcome.draw &&
      winner != GameWinner.white &&
      winner != GameWinner.black) {
    throw const FormatException(
      'A decisive terminal outcome requires a winning side.',
    );
  }
  if (winner == GameWinner.draw && outcome != GameOutcome.draw) {
    throw const FormatException('Only a drawn game may have a draw result.');
  }

  final bool outcomeAllowed = switch (variant) {
    GameVariant.classic =>
      outcome == GameOutcome.ongoing ||
          outcome == GameOutcome.checkmate ||
          outcome == GameOutcome.draw ||
          outcome == GameOutcome.timeout ||
          outcome == GameOutcome.resignation,
    GameVariant.duck =>
      outcome == GameOutcome.ongoing ||
          outcome == GameOutcome.kingCaptured ||
          outcome == GameOutcome.fowled ||
          outcome == GameOutcome.timeout ||
          outcome == GameOutcome.resignation,
  };
  if (!outcomeAllowed) {
    throw const FormatException(
      'Archive outcome is incompatible with its game variant.',
    );
  }

  final HomeChessPosition finalPosition = HomeChessPosition.tryParse(
    positions.last.fen,
  )!;
  final GameWinner finalSide = finalPosition.sideToMove == HomeSide.white
      ? GameWinner.white
      : GameWinner.black;
  final GameWinner oppositeFinalSide = finalSide == GameWinner.white
      ? GameWinner.black
      : GameWinner.white;
  if (outcome == GameOutcome.kingCaptured) {
    if (!finalTransitionCapturedKing ||
        transitions.isEmpty ||
        winner != _winnerForSide(transitions.last.side)) {
      throw const FormatException(
        'Duck king capture requires a matching final capture and winner.',
      );
    }
  }
  if (outcome == GameOutcome.fowled && winner != finalSide) {
    throw const FormatException(
      'Fowled winner must match the final side to move.',
    );
  }
  if ((outcome == GameOutcome.checkmate || outcome == GameOutcome.timeout) &&
      winner != oppositeFinalSide) {
    throw const FormatException(
      'Checkmate and timeout winner must oppose the final side to move.',
    );
  }
}

bool _validateTransitionBoard({
  required HomeChessPosition before,
  required HomeChessPosition after,
  required ArchivedTransition transition,
}) {
  if (before.sideToMove == after.sideToMove) {
    throw const FormatException(
      'Archive transition must change the side to move.',
    );
  }

  final Map<int, String> expected = Map<int, String>.of(before.pieces);
  final String? movedPiece = expected.remove(transition.from);
  if (movedPiece == null || _pieceSide(movedPiece) != transition.side) {
    throw const FormatException(
      'Archive transition origin does not contain the moving side.',
    );
  }

  String? capturedPiece;
  final int? captureSquare = transition.captureSquare;
  if (captureSquare != null) {
    capturedPiece = expected.remove(captureSquare);
    if (capturedPiece == null || _pieceSide(capturedPiece) == transition.side) {
      throw const FormatException(
        'Archive capture square does not contain an opposing piece.',
      );
    }
  }
  if (expected.containsKey(transition.to)) {
    throw const FormatException(
      'Archive move destination remains occupied after its capture.',
    );
  }

  final bool reachesPromotionRank =
      movedPiece.toLowerCase() == 'p' &&
      (transition.to ~/ 8 == 0 || transition.to ~/ 8 == 7);
  if (transition.promotion != null && !reachesPromotionRank) {
    throw const FormatException(
      'Archive promotion does not move a pawn to its final rank.',
    );
  }
  if (transition.promotion == null && reachesPromotionRank) {
    throw const FormatException(
      'Archive pawn reaching its final rank requires promotion.',
    );
  }
  final String resultingPiece = transition.promotion == null
      ? movedPiece
      : _promotionToken(transition.promotion!, transition.side);
  expected[transition.to] = resultingPiece;

  final int? secondaryFrom = transition.secondaryFrom;
  final int? secondaryTo = transition.secondaryTo;
  if (secondaryFrom != null && secondaryTo != null) {
    final String? secondaryPiece = expected.remove(secondaryFrom);
    if (secondaryPiece == null ||
        _pieceSide(secondaryPiece) != transition.side ||
        expected.containsKey(secondaryTo)) {
      throw const FormatException('Archive secondary relocation is invalid.');
    }
    expected[secondaryTo] = secondaryPiece;
  }

  if (expected.length != after.pieces.length ||
      expected.entries.any(
        (MapEntry<int, String> entry) => after.pieces[entry.key] != entry.value,
      )) {
    throw const FormatException(
      'Archive transition does not produce its destination FEN.',
    );
  }
  return capturedPiece?.toLowerCase() == 'k';
}

GameSide _pieceSide(String piece) {
  return piece == piece.toUpperCase() ? GameSide.white : GameSide.black;
}

String _promotionToken(PromotionPiece promotion, GameSide side) {
  final String token = switch (promotion) {
    PromotionPiece.queen => 'q',
    PromotionPiece.rook => 'r',
    PromotionPiece.bishop => 'b',
    PromotionPiece.knight => 'n',
  };
  return side == GameSide.white ? token.toUpperCase() : token;
}

bool _samePosition(ArchivedPosition left, ArchivedPosition right) {
  return left.ply == right.ply &&
      left.fen == right.fen &&
      left.duckSquare == right.duckSquare;
}

bool _sameTransition(ArchivedTransition left, ArchivedTransition right) {
  return left.side == right.side &&
      left.from == right.from &&
      left.to == right.to &&
      left.captureSquare == right.captureSquare &&
      left.promotion == right.promotion &&
      left.secondaryFrom == right.secondaryFrom &&
      left.secondaryTo == right.secondaryTo &&
      left.duckFrom == right.duckFrom &&
      left.duckTo == right.duckTo;
}

GameWinner _winnerForSide(GameSide side) {
  return side == GameSide.white ? GameWinner.white : GameWinner.black;
}

bool isValidGameArchiveLibraryIdentifier(String value) {
  return value.length <= 80 &&
      RegExp(r'^[a-z0-9][a-z0-9._-]*$').hasMatch(value);
}

bool isValidGameArchiveGameIdentifier(String value) {
  return value.length <= 80 &&
      RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]*$').hasMatch(value);
}

void _validateLibraryIdentifier(String value, {required String field}) {
  if (!isValidGameArchiveLibraryIdentifier(value)) {
    throw FormatException('Invalid $field.');
  }
}

void _validateBoardIdentifier(String value, {required String field}) {
  if (!isValidGameArchiveGameIdentifier(value)) {
    throw FormatException('Invalid $field.');
  }
}

void _validateSquare(
  int? value, {
  required String field,
  bool optional = false,
}) {
  if (value == null) {
    if (optional) {
      return;
    }
    throw FormatException('Missing $field.');
  }
  if (value < 0 || value >= 64) {
    throw FormatException('Invalid $field.');
  }
}

void _validatePair(int? from, int? to, {required String field}) {
  if ((from == null) != (to == null)) {
    throw FormatException('$field must contain both endpoints.');
  }
  if (from != null && from == to) {
    throw FormatException('$field must change squares.');
  }
}

String _requiredString(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value is! String || value.isEmpty) {
    throw FormatException('Missing or invalid $key.');
  }
  return value;
}

int _requiredInt(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value is! int) {
    throw FormatException('Missing or invalid $key.');
  }
  return value;
}

int? _optionalInt(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value == null) {
    return null;
  }
  if (value is! int) {
    throw FormatException('Invalid $key.');
  }
  return value;
}

String? _optionalString(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value == null) {
    return null;
  }
  if (value is! String || value.isEmpty) {
    throw FormatException('Invalid $key.');
  }
  return value;
}

List<Object?> _requiredList(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value is! List<Object?>) {
    throw FormatException('Missing or invalid $key.');
  }
  return value;
}

Map<String, Object?> _requiredMap(Object? value) {
  if (value is! Map<String, Object?>) {
    throw const FormatException('Expected an archive object.');
  }
  return value;
}

DateTime _requiredDateTime(Map<String, Object?> json, String key) {
  final String value = _requiredString(json, key);
  final DateTime? parsed = DateTime.tryParse(value);
  if (parsed == null || !parsed.isUtc) {
    throw FormatException('Invalid $key.');
  }
  return parsed;
}

T _requiredEnum<T extends Enum>(
  Map<String, Object?> json,
  String key,
  List<T> values,
) {
  final String value = _requiredString(json, key);
  return values.firstWhere(
    (T candidate) => candidate.name == value,
    orElse: () => throw FormatException('Invalid $key.'),
  );
}

T? _optionalEnum<T extends Enum>(
  Map<String, Object?> json,
  String key,
  List<T> values,
) {
  if (json[key] == null) {
    return null;
  }
  return _requiredEnum(json, key, values);
}

bool _validClockBalance(int milliseconds) =>
    milliseconds >= 0 && milliseconds <= archivedGameClockMaximumMilliseconds;
