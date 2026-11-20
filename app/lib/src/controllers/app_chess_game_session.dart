import 'dart:async';
import 'dart:math';

import 'package:chess/chess.dart' as rules;
import 'package:flutter/foundation.dart';

import '../models/game_archive.dart';
import '../models/home_board_snapshot.dart';
import '../models/standard_chess_replay.dart';

const String standardChessInitialFen =
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

enum AppChessMoveResult { accepted, illegal, clockPaused, gameOver }

class AppChessClockConfiguration {
  const AppChessClockConfiguration({
    required this.base,
    this.increment = Duration.zero,
  });

  final Duration base;
  final Duration increment;

  void validate() {
    if (base <= Duration.zero ||
        base > const Duration(days: 7) ||
        increment.isNegative ||
        increment > const Duration(hours: 1)) {
      throw const FormatException('App clock configuration is out of range.');
    }
  }
}

@immutable
class AppChessClockView {
  const AppChessClockView({
    required this.whiteRemaining,
    required this.blackRemaining,
    required this.runState,
    required this.sideToMove,
    required this.recoveryRequired,
  });

  final Duration whiteRemaining;
  final Duration blackRemaining;
  final ArchivedGameClockRunState runState;
  final GameSide sideToMove;
  final bool recoveryRequired;
}

typedef AppChessArchiveWriter = Future<void> Function(GameArchive archive);

bool isContinuableAppChessArchive(GameArchive archive) {
  return archive.origin == GameArchiveOrigin.app &&
      archive.status == GameArchiveStatus.active &&
      archive.variant == GameVariant.classic &&
      archive.captureQuality == GameCaptureQuality.complete &&
      archive.boardGameId == null &&
      archive.positions.first.ply == 0;
}

class AppChessGameIdentity {
  const AppChessGameIdentity({required this.gameId, required this.libraryId});

  factory AppChessGameIdentity.generate([Random? random]) {
    final Random source = random ?? Random.secure();
    final StringBuffer suffix = StringBuffer();
    for (int index = 0; index < 16; index += 1) {
      suffix.write(source.nextInt(256).toRadixString(16).padLeft(2, '0'));
    }
    return AppChessGameIdentity(
      gameId: 'app_${suffix.toString()}',
      libraryId: 'app-${suffix.toString()}',
    );
  }

  final String gameId;
  final String libraryId;

  void validate() {
    if (!isValidGameArchiveGameIdentifier(gameId) ||
        !isValidGameArchiveLibraryIdentifier(libraryId)) {
      throw const FormatException('Invalid app-game identity.');
    }
  }
}

/// Owns one phone-side Classic game independently of board transport state.
///
/// Moves commit to the local model synchronously. Persistence keeps at most one
/// write in flight and coalesces any faster moves to the latest complete
/// archive, so storage latency never delays the next legal tap.
class AppChessGameSession extends ChangeNotifier {
  factory AppChessGameSession.newGame({
    required AppChessGameIdentity identity,
    required DateTime Function() now,
    required AppChessArchiveWriter persist,
    String initialFen = standardChessInitialFen,
    AppChessClockConfiguration? clockConfiguration,
    int Function()? monotonicMicros,
  }) {
    identity.validate();
    final DateTime createdAt = now().toUtc();
    final rules.Chess chess = validatedStandardChessInitialPosition(initialFen);
    final ({GameArchiveStatus status, GameOutcome outcome, GameWinner winner})
    result = _resultFor(chess);
    clockConfiguration?.validate();
    final int? baseMilliseconds = clockConfiguration?.base.inMilliseconds;
    final GameArchive archive = GameArchive(
      libraryId: identity.libraryId,
      gameId: identity.gameId,
      origin: GameArchiveOrigin.app,
      createdAt: createdAt,
      updatedAt: createdAt,
      status: result.status,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: result.outcome,
      winner: result.winner,
      lineRevision: 0,
      positions: <ArchivedPosition>[ArchivedPosition(ply: 0, fen: initialFen)],
      transitions: const <ArchivedTransition>[],
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
      clock: baseMilliseconds == null
          ? null
          : ArchivedGameClock(
              baseMilliseconds: baseMilliseconds,
              incrementMilliseconds:
                  clockConfiguration!.increment.inMilliseconds,
              whiteRemainingMilliseconds: baseMilliseconds,
              blackRemainingMilliseconds: baseMilliseconds,
              runState: ArchivedGameClockRunState.ready,
              turnStartCheckpoints: <ArchivedClockCheckpoint>[
                ArchivedClockCheckpoint(
                  whiteRemainingMilliseconds: baseMilliseconds,
                  blackRemainingMilliseconds: baseMilliseconds,
                ),
              ],
            ),
    );
    final int Function() monotonic = monotonicMicros ?? _newMonotonicMicros();
    return AppChessGameSession._(
      archive: archive,
      uciMoves: const <String>[],
      now: now,
      monotonicMicros: monotonic,
      persist: persist,
      persistInitial: true,
    );
  }

  factory AppChessGameSession.restore({
    required GameArchive archive,
    required DateTime Function() now,
    required AppChessArchiveWriter persist,
    int Function()? monotonicMicros,
  }) {
    archive.validate();
    if (archive.origin != GameArchiveOrigin.app ||
        archive.variant != GameVariant.classic ||
        archive.captureQuality != GameCaptureQuality.complete ||
        archive.positions.first.ply != 0 ||
        archive.boardGameId != null ||
        archive.status == GameArchiveStatus.unfinished) {
      throw const FormatException('Archive is not a restorable app game.');
    }
    final List<String> moves = archive.transitions
        .map(_uciForTransition)
        .toList(growable: false);
    replayStandardChessExtension(archive, moves);
    final result = _resultFor(
      validatedStandardChessInitialPosition(archive.positions.last.fen),
    );
    if (archive.status != result.status ||
        archive.outcome != result.outcome ||
        archive.winner != result.winner) {
      throw const FormatException(
        'Archive lifecycle does not match its chess position.',
      );
    }
    final DateTime observedAt = now().toUtc();
    final GameArchive normalized = _normalizeAppPlayers(archive, observedAt);
    final bool playersChanged = !identical(normalized, archive);
    final _RecoveredClock recovered = _recoverClock(normalized, observedAt);
    final int Function() monotonic = monotonicMicros ?? _newMonotonicMicros();
    return AppChessGameSession._(
      archive: recovered.archive,
      uciMoves: moves,
      now: now,
      monotonicMicros: monotonic,
      persist: persist,
      persistInitial: playersChanged || recovered.changed,
      clockRecoveryRequired: recovered.recoveryRequired,
    );
  }

  AppChessGameSession._({
    required GameArchive archive,
    required List<String> uciMoves,
    required DateTime Function() now,
    required int Function() monotonicMicros,
    required AppChessArchiveWriter persist,
    required bool persistInitial,
    bool clockRecoveryRequired = false,
  }) : _archive = archive,
       _uciMoves = List<String>.of(uciMoves),
       _now = now,
       _monotonicMicros = monotonicMicros,
       _clockRecoveryRequired = clockRecoveryRequired,
       _persist = persist {
    if (_archive.clock?.runState == ArchivedGameClockRunState.running) {
      _clockAnchorMicros = _monotonicMicros();
    }
    if (persistInitial) _schedulePersistence();
  }

  final DateTime Function() _now;
  final int Function() _monotonicMicros;
  final AppChessArchiveWriter _persist;
  final List<String> _uciMoves;
  GameArchive _archive;
  GameArchive? _pendingPersistence;
  Future<void>? _persistenceTail;
  bool _disposed = false;
  bool _saveFailed = false;
  int? _clockAnchorMicros;
  bool _clockRecoveryRequired;

  GameArchive get archive => _archive;
  List<String> get uciMoves => List<String>.unmodifiable(_uciMoves);
  bool get savePending =>
      _pendingPersistence != null || _persistenceTail != null;
  bool get saveFailed => _saveFailed;
  bool get canUndo => _uciMoves.isNotEmpty;
  bool get isGameOver => _archive.status == GameArchiveStatus.completed;
  GameSide get sideToMove => _archive.positions.last.fen.split(' ')[1] == 'w'
      ? GameSide.white
      : GameSide.black;

  bool get canChangePlayers => _uciMoves.isEmpty && !isGameOver;

  AppChessClockView? get clock {
    final ArchivedGameClock? sampled = _sampledClock();
    if (sampled == null) return null;
    return AppChessClockView(
      whiteRemaining: Duration(
        milliseconds: sampled.whiteRemainingMilliseconds,
      ),
      blackRemaining: Duration(
        milliseconds: sampled.blackRemainingMilliseconds,
      ),
      runState: sampled.runState,
      sideToMove: sideToMove,
      recoveryRequired: _clockRecoveryRequired,
    );
  }

  Duration? get timeUntilTimeout {
    final ArchivedGameClock? sampled = _sampledClock();
    if (sampled == null ||
        sampled.runState != ArchivedGameClockRunState.running) {
      return null;
    }
    return Duration(
      milliseconds: sideToMove == GameSide.white
          ? sampled.whiteRemainingMilliseconds
          : sampled.blackRemainingMilliseconds,
    );
  }

  bool startClock() {
    final ArchivedGameClock? current = _archive.clock;
    if (_disposed ||
        current == null ||
        isGameOver ||
        current.runState == ArchivedGameClockRunState.running ||
        current.runState == ArchivedGameClockRunState.stopped) {
      return false;
    }
    _clockRecoveryRequired = false;
    _setClock(
      _copyClock(
        current,
        runState: ArchivedGameClockRunState.running,
        sampledAtUtc: _monotonicNow(),
      ),
      persist: true,
    );
    _clockAnchorMicros = _monotonicMicros();
    notifyListeners();
    return true;
  }

  bool settleClock() {
    final ArchivedGameClock? sampled = _sampledClock();
    if (sampled == null ||
        sampled.runState != ArchivedGameClockRunState.running) {
      return false;
    }
    if (_activeBalance(sampled) > 0) return false;
    _sealTimeout(sampled);
    return true;
  }

  void suspendClock() {
    final ArchivedGameClock? sampled = _sampledClock();
    if (sampled == null ||
        sampled.runState == ArchivedGameClockRunState.suspended ||
        sampled.runState == ArchivedGameClockRunState.stopped) {
      return;
    }
    if (sampled.runState == ArchivedGameClockRunState.running &&
        _activeBalance(sampled) == 0) {
      _sealTimeout(sampled);
      return;
    }
    _clockAnchorMicros = null;
    _setClock(
      _copyClock(
        sampled,
        runState: ArchivedGameClockRunState.suspended,
        clearSampledAtUtc: true,
      ),
      persist: true,
    );
    notifyListeners();
  }

  void resumeClock() {
    final ArchivedGameClock? current = _archive.clock;
    if (current == null ||
        current.runState != ArchivedGameClockRunState.suspended ||
        isGameOver) {
      return;
    }
    startClock();
  }

  List<String> legalUciMovesFrom(int square) {
    if (_disposed || isGameOver || square < 0 || square >= 64) {
      return const <String>[];
    }
    final rules.Chess chess = validatedStandardChessInitialPosition(
      _archive.positions.last.fen,
    );
    final String from = _squareName(square);
    return chess
        .generate_moves(<String, Object?>{'square': from})
        .map(
          (rules.Move move) =>
              '$from${move.toAlgebraic}${move.promotion?.name ?? ''}',
        )
        .toList(growable: false);
  }

  AppChessMoveResult playUci(String uci) {
    if (_disposed) throw StateError('App chess session is disposed.');
    if (isGameOver) return AppChessMoveResult.gameOver;
    if (_archive.clock?.runState == ArchivedGameClockRunState.suspended) {
      return AppChessMoveResult.clockPaused;
    }
    final List<String> nextMoves = <String>[..._uciMoves, uci];
    final GameArchive replayed;
    try {
      replayed = replayStandardChessExtension(_archive, nextMoves);
    } on FormatException {
      return AppChessMoveResult.illegal;
    }
    final ArchivedGameClock? sampledClock = _sampledClock();
    if (sampledClock != null &&
        sampledClock.runState == ArchivedGameClockRunState.running &&
        _activeBalance(sampledClock) == 0) {
      _sealTimeout(sampledClock);
      return AppChessMoveResult.gameOver;
    }
    final rules.Chess chess = validatedStandardChessInitialPosition(
      replayed.positions.last.fen,
    );
    final result = _resultFor(chess);
    final ArchivedGameClock? nextClock = sampledClock == null
        ? null
        : _clockAfterMove(sampledClock, result.status);
    _uciMoves.add(uci);
    _archive = _copyArchive(
      replayed,
      updatedAt: _monotonicNow(),
      lineRevision: _archive.lineRevision + 1,
      status: result.status,
      outcome: result.outcome,
      winner: result.winner,
      clock: nextClock,
    );
    _clockRecoveryRequired = false;
    _clockAnchorMicros =
        nextClock?.runState == ArchivedGameClockRunState.running
        ? _monotonicMicros()
        : null;
    _saveFailed = false;
    _schedulePersistence();
    notifyListeners();
    return AppChessMoveResult.accepted;
  }

  bool undo() {
    if (_disposed) throw StateError('App chess session is disposed.');
    if (_uciMoves.isEmpty || _archive.outcome == GameOutcome.timeout) {
      return false;
    }
    final ArchivedGameClock? priorClock = _archive.clock;
    _uciMoves.removeLast();
    final GameArchive replayed = _archiveForMoves(_uciMoves);
    final ArchivedGameClock? restoredClock = priorClock == null
        ? null
        : _clockAfterUndo(priorClock, replayed.positions.length);
    _archive = _copyArchive(
      replayed,
      updatedAt: _monotonicNow(),
      lineRevision: _archive.lineRevision + 1,
      status: GameArchiveStatus.active,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      clock: restoredClock,
    );
    _clockAnchorMicros =
        restoredClock?.runState == ArchivedGameClockRunState.running
        ? _monotonicMicros()
        : null;
    _saveFailed = false;
    _schedulePersistence();
    notifyListeners();
    return true;
  }

  bool setClockConfiguration(AppChessClockConfiguration? configuration) {
    if (_disposed) throw StateError('App chess session is disposed.');
    final ArchivedGameClockRunState? currentState = _archive.clock?.runState;
    if (!canChangePlayers ||
        (currentState != null &&
            currentState != ArchivedGameClockRunState.ready)) {
      return false;
    }
    configuration?.validate();
    final ArchivedGameClock? current = _archive.clock;
    final int? baseMilliseconds = configuration?.base.inMilliseconds;
    final int? incrementMilliseconds = configuration?.increment.inMilliseconds;
    if ((current == null && configuration == null) ||
        (current != null &&
            current.runState == ArchivedGameClockRunState.ready &&
            current.baseMilliseconds == baseMilliseconds &&
            current.incrementMilliseconds == incrementMilliseconds)) {
      return false;
    }
    final ArchivedGameClock? next = baseMilliseconds == null
        ? null
        : ArchivedGameClock(
            baseMilliseconds: baseMilliseconds,
            incrementMilliseconds: incrementMilliseconds!,
            whiteRemainingMilliseconds: baseMilliseconds,
            blackRemainingMilliseconds: baseMilliseconds,
            runState: ArchivedGameClockRunState.ready,
            turnStartCheckpoints: <ArchivedClockCheckpoint>[
              ArchivedClockCheckpoint(
                whiteRemainingMilliseconds: baseMilliseconds,
                blackRemainingMilliseconds: baseMilliseconds,
              ),
            ],
          );
    _archive = _copyArchive(
      _archive,
      updatedAt: _monotonicNow(),
      lineRevision: _archive.lineRevision,
      status: _archive.status,
      outcome: _archive.outcome,
      winner: _archive.winner,
      clock: next,
      clearClock: next == null,
    );
    _clockAnchorMicros = null;
    _clockRecoveryRequired = false;
    _saveFailed = false;
    _schedulePersistence();
    notifyListeners();
    return true;
  }

  Future<void> retryPersistence() async {
    if (_disposed) throw StateError('App chess session is disposed.');
    _saveFailed = false;
    _schedulePersistence();
    notifyListeners();
    await settlePersistence();
  }

  Future<void> settlePersistence() async {
    while (_persistenceTail != null) {
      final Future<void> tail = _persistenceTail!;
      await tail;
    }
  }

  DateTime _monotonicNow() {
    final DateTime candidate = _now().toUtc();
    return candidate.isBefore(_archive.updatedAt)
        ? _archive.updatedAt
        : candidate;
  }

  GameArchive _archiveForMoves(List<String> moves) {
    final GameArchive seed = GameArchive(
      libraryId: _archive.libraryId,
      gameId: _archive.gameId,
      origin: GameArchiveOrigin.app,
      createdAt: _archive.createdAt,
      updatedAt: _archive.updatedAt,
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: _archive.lineRevision,
      positions: <ArchivedPosition>[_archive.positions.first],
      transitions: const <ArchivedTransition>[],
      displayName: _archive.displayName,
      whitePlayer: _archive.whitePlayer,
      blackPlayer: _archive.blackPlayer,
      provenance: _archive.provenance,
    );
    return replayStandardChessExtension(seed, moves);
  }

  ArchivedGameClock? _sampledClock() {
    final ArchivedGameClock? current = _archive.clock;
    if (current == null ||
        current.runState != ArchivedGameClockRunState.running) {
      return current;
    }
    final int anchor = _clockAnchorMicros ?? _monotonicMicros();
    final int elapsedMicros = _monotonicMicros() - anchor;
    final int elapsedMilliseconds = elapsedMicros <= 0
        ? 0
        : elapsedMicros ~/ Duration.microsecondsPerMillisecond;
    if (elapsedMilliseconds == 0) return current;
    final int white = sideToMove == GameSide.white
        ? max(0, current.whiteRemainingMilliseconds - elapsedMilliseconds)
        : current.whiteRemainingMilliseconds;
    final int black = sideToMove == GameSide.black
        ? max(0, current.blackRemainingMilliseconds - elapsedMilliseconds)
        : current.blackRemainingMilliseconds;
    return _copyClock(
      current,
      whiteRemainingMilliseconds: white,
      blackRemainingMilliseconds: black,
    );
  }

  int _activeBalance(ArchivedGameClock clock) => sideToMove == GameSide.white
      ? clock.whiteRemainingMilliseconds
      : clock.blackRemainingMilliseconds;

  ArchivedGameClock _clockAfterMove(
    ArchivedGameClock sampled,
    GameArchiveStatus nextStatus,
  ) {
    var white = sampled.whiteRemainingMilliseconds;
    var black = sampled.blackRemainingMilliseconds;
    if (sideToMove == GameSide.white) {
      white = min(
        archivedGameClockMaximumMilliseconds,
        white + sampled.incrementMilliseconds,
      );
    } else {
      black = min(
        archivedGameClockMaximumMilliseconds,
        black + sampled.incrementMilliseconds,
      );
    }
    final bool stopped = nextStatus == GameArchiveStatus.completed;
    final bool remainsSuspended =
        sampled.runState == ArchivedGameClockRunState.suspended;
    final ArchivedGameClockRunState runState = stopped
        ? ArchivedGameClockRunState.stopped
        : remainsSuspended
        ? ArchivedGameClockRunState.suspended
        : ArchivedGameClockRunState.running;
    return ArchivedGameClock(
      baseMilliseconds: sampled.baseMilliseconds,
      incrementMilliseconds: sampled.incrementMilliseconds,
      whiteRemainingMilliseconds: white,
      blackRemainingMilliseconds: black,
      runState: runState,
      sampledAtUtc: runState == ArchivedGameClockRunState.running
          ? _monotonicNow()
          : null,
      turnStartCheckpoints: <ArchivedClockCheckpoint>[
        ...sampled.turnStartCheckpoints,
        ArchivedClockCheckpoint(
          whiteRemainingMilliseconds: white,
          blackRemainingMilliseconds: black,
        ),
      ],
    );
  }

  ArchivedGameClock _clockAfterUndo(
    ArchivedGameClock current,
    int positionCount,
  ) {
    final List<ArchivedClockCheckpoint> checkpoints = current
        .turnStartCheckpoints
        .take(positionCount)
        .toList(growable: false);
    final ArchivedClockCheckpoint restored = checkpoints.last;
    final ArchivedGameClockRunState runState = switch (current.runState) {
      ArchivedGameClockRunState.running => ArchivedGameClockRunState.running,
      ArchivedGameClockRunState.ready => ArchivedGameClockRunState.ready,
      ArchivedGameClockRunState.suspended ||
      ArchivedGameClockRunState.stopped => ArchivedGameClockRunState.suspended,
    };
    return ArchivedGameClock(
      baseMilliseconds: current.baseMilliseconds,
      incrementMilliseconds: current.incrementMilliseconds,
      whiteRemainingMilliseconds: restored.whiteRemainingMilliseconds,
      blackRemainingMilliseconds: restored.blackRemainingMilliseconds,
      runState: runState,
      sampledAtUtc: runState == ArchivedGameClockRunState.running
          ? _monotonicNow()
          : null,
      turnStartCheckpoints: checkpoints,
    );
  }

  void _setClock(ArchivedGameClock clock, {required bool persist}) {
    _archive = _copyArchive(
      _archive,
      updatedAt: _monotonicNow(),
      lineRevision: _archive.lineRevision,
      status: _archive.status,
      outcome: _archive.outcome,
      winner: _archive.winner,
      clock: clock,
    );
    if (persist) {
      _saveFailed = false;
      _schedulePersistence();
    }
  }

  void _sealTimeout(ArchivedGameClock sampled) {
    final bool whiteLoses = sideToMove == GameSide.white;
    _clockAnchorMicros = null;
    _archive = _copyArchive(
      _archive,
      updatedAt: _monotonicNow(),
      lineRevision: _archive.lineRevision + 1,
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.timeout,
      winner: whiteLoses ? GameWinner.black : GameWinner.white,
      clock: _copyClock(
        sampled,
        whiteRemainingMilliseconds: whiteLoses
            ? 0
            : sampled.whiteRemainingMilliseconds,
        blackRemainingMilliseconds: whiteLoses
            ? sampled.blackRemainingMilliseconds
            : 0,
        runState: ArchivedGameClockRunState.stopped,
        clearSampledAtUtc: true,
      ),
    );
    _saveFailed = false;
    _schedulePersistence();
    notifyListeners();
  }

  void _schedulePersistence() {
    _pendingPersistence = _archive;
    if (_persistenceTail != null) return;
    late final Future<void> tail;
    tail = _flushPersistence().whenComplete(() {
      if (identical(_persistenceTail, tail)) _persistenceTail = null;
      if (_pendingPersistence != null) _schedulePersistence();
      if (!_disposed) notifyListeners();
    });
    _persistenceTail = tail;
  }

  Future<void> _flushPersistence() async {
    while (_pendingPersistence != null) {
      final GameArchive next = _pendingPersistence!;
      _pendingPersistence = null;
      try {
        await _persist(next);
      } on Object {
        _saveFailed = true;
        _pendingPersistence = null;
        if (!_disposed) notifyListeners();
        return;
      }
    }
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }
}

({GameArchiveStatus status, GameOutcome outcome, GameWinner winner}) _resultFor(
  rules.Chess chess,
) {
  if (chess.in_checkmate) {
    return (
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.checkmate,
      winner: chess.turn == rules.Chess.WHITE
          ? GameWinner.black
          : GameWinner.white,
    );
  }
  if (chess.in_draw) {
    return (
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.draw,
      winner: GameWinner.draw,
    );
  }
  return (
    status: GameArchiveStatus.active,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
  );
}

String _uciForTransition(ArchivedTransition transition) {
  return '${_squareName(transition.from)}${_squareName(transition.to)}'
      '${switch (transition.promotion) {
        PromotionPiece.queen => 'q',
        PromotionPiece.rook => 'r',
        PromotionPiece.bishop => 'b',
        PromotionPiece.knight => 'n',
        null => '',
      }}';
}

String _squareName(int square) =>
    '${String.fromCharCode(97 + square % 8)}${8 - square ~/ 8}';

int Function() _newMonotonicMicros() {
  final Stopwatch stopwatch = Stopwatch()..start();
  return () => stopwatch.elapsedMicroseconds;
}

class _RecoveredClock {
  const _RecoveredClock(
    this.archive, {
    required this.changed,
    required this.recoveryRequired,
  });

  final GameArchive archive;
  final bool changed;
  final bool recoveryRequired;
}

GameArchive _normalizeAppPlayers(GameArchive archive, DateTime observedAt) {
  final bool whiteWasComputer =
      archive.whitePlayer.kind == ArchivedPlayerKind.computer;
  final bool blackWasComputer =
      archive.blackPlayer.kind == ArchivedPlayerKind.computer;
  if (!whiteWasComputer && !blackWasComputer) return archive;
  return _copyArchive(
    archive,
    updatedAt: observedAt.isBefore(archive.updatedAt)
        ? archive.updatedAt
        : observedAt,
    lineRevision: archive.lineRevision,
    status: archive.status,
    outcome: archive.outcome,
    winner: archive.winner,
    whitePlayer: whiteWasComputer
        ? const ArchivedPlayerAttribution.guest()
        : archive.whitePlayer,
    blackPlayer: blackWasComputer
        ? const ArchivedPlayerAttribution.guest()
        : archive.blackPlayer,
  );
}

_RecoveredClock _recoverClock(GameArchive archive, DateTime observedAt) {
  final ArchivedGameClock? clock = archive.clock;
  if (clock == null || clock.runState != ArchivedGameClockRunState.running) {
    return _RecoveredClock(archive, changed: false, recoveryRequired: false);
  }
  final DateTime sampledAt = clock.sampledAtUtc!;
  final Duration elapsed = observedAt.difference(sampledAt);
  final DateTime updatedAt = observedAt.isBefore(archive.updatedAt)
      ? archive.updatedAt
      : observedAt;
  if (elapsed.isNegative || elapsed > const Duration(hours: 24)) {
    return _RecoveredClock(
      _copyArchive(
        archive,
        updatedAt: updatedAt,
        lineRevision: archive.lineRevision,
        status: archive.status,
        outcome: archive.outcome,
        winner: archive.winner,
        clock: _copyClock(
          clock,
          runState: ArchivedGameClockRunState.suspended,
          clearSampledAtUtc: true,
        ),
      ),
      changed: true,
      recoveryRequired: true,
    );
  }
  final HomeSide active = HomeChessPosition.tryParse(
    archive.positions.last.fen,
  )!.sideToMove;
  final int elapsedMilliseconds = elapsed.inMilliseconds;
  final int white = active == HomeSide.white
      ? max(0, clock.whiteRemainingMilliseconds - elapsedMilliseconds)
      : clock.whiteRemainingMilliseconds;
  final int black = active == HomeSide.black
      ? max(0, clock.blackRemainingMilliseconds - elapsedMilliseconds)
      : clock.blackRemainingMilliseconds;
  final bool timedOut = active == HomeSide.white ? white == 0 : black == 0;
  final ArchivedGameClock recoveredClock = _copyClock(
    clock,
    whiteRemainingMilliseconds: white,
    blackRemainingMilliseconds: black,
    runState: timedOut
        ? ArchivedGameClockRunState.stopped
        : ArchivedGameClockRunState.running,
    sampledAtUtc: timedOut ? null : observedAt,
    clearSampledAtUtc: timedOut,
  );
  return _RecoveredClock(
    _copyArchive(
      archive,
      updatedAt: updatedAt,
      lineRevision: archive.lineRevision + (timedOut ? 1 : 0),
      status: timedOut ? GameArchiveStatus.completed : archive.status,
      outcome: timedOut ? GameOutcome.timeout : archive.outcome,
      winner: timedOut
          ? (active == HomeSide.white ? GameWinner.black : GameWinner.white)
          : archive.winner,
      clock: recoveredClock,
    ),
    changed: true,
    recoveryRequired: false,
  );
}

ArchivedGameClock _copyClock(
  ArchivedGameClock source, {
  int? whiteRemainingMilliseconds,
  int? blackRemainingMilliseconds,
  ArchivedGameClockRunState? runState,
  DateTime? sampledAtUtc,
  bool clearSampledAtUtc = false,
}) {
  return ArchivedGameClock(
    baseMilliseconds: source.baseMilliseconds,
    incrementMilliseconds: source.incrementMilliseconds,
    whiteRemainingMilliseconds:
        whiteRemainingMilliseconds ?? source.whiteRemainingMilliseconds,
    blackRemainingMilliseconds:
        blackRemainingMilliseconds ?? source.blackRemainingMilliseconds,
    runState: runState ?? source.runState,
    sampledAtUtc: clearSampledAtUtc
        ? null
        : (sampledAtUtc ?? source.sampledAtUtc),
    turnStartCheckpoints: source.turnStartCheckpoints,
  );
}

GameArchive _copyArchive(
  GameArchive source, {
  required DateTime updatedAt,
  required int lineRevision,
  required GameArchiveStatus status,
  required GameOutcome outcome,
  required GameWinner winner,
  ArchivedPlayerAttribution? whitePlayer,
  ArchivedPlayerAttribution? blackPlayer,
  ArchivedGameClock? clock,
  bool clearClock = false,
}) {
  return GameArchive(
    libraryId: source.libraryId,
    gameId: source.gameId,
    origin: source.origin,
    createdAt: source.createdAt,
    updatedAt: updatedAt,
    status: status,
    captureQuality: source.captureQuality,
    variant: source.variant,
    outcome: outcome,
    winner: winner,
    lineRevision: lineRevision,
    positions: source.positions,
    transitions: source.transitions,
    displayName: source.displayName,
    whitePlayer: whitePlayer ?? source.whitePlayer,
    blackPlayer: blackPlayer ?? source.blackPlayer,
    provenance: source.provenance,
    clock: clearClock ? null : (clock ?? source.clock),
  );
}
