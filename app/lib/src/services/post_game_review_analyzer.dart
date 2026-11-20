import 'dart:async';
import 'dart:collection';
import 'dart:convert';

import 'package:chess/chess.dart' as rules;
import 'package:crypto/crypto.dart';

import '../engine/chess_engine_models.dart';
import '../engine/engine_runtime.dart';
import '../models/game_archive.dart';
import '../models/home_board_snapshot.dart';
import '../models/post_game_review.dart';
import '../models/standard_chess_replay.dart';

export '../models/post_game_review.dart';

class PostGameReviewCancellation {
  bool _cancelled = false;
  final Set<void Function()> _listeners = <void Function()>{};
  bool get isCancelled => _cancelled;
  void cancel() {
    if (_cancelled) return;
    _cancelled = true;
    for (final listener in List<void Function()>.of(_listeners)) {
      try {
        listener();
      } on Object {
        // Cancellation is best-effort cleanup. One faulty adapter callback
        // must not prevent the remaining owners from retiring authority.
      }
    }
    _listeners.clear();
  }

  void Function() addListener(void Function() listener) {
    if (_cancelled) {
      listener();
      return () {};
    }
    _listeners.add(listener);
    return () => _listeners.remove(listener);
  }

  void throwIfCancelled() {
    if (_cancelled) throw const PostGameReviewCancelled();
  }
}

class PostGameReviewCancelled implements Exception {
  const PostGameReviewCancelled();
}

/// Port for bounded, on-device Classic analysis after a game has ended.
///
/// Implementations must not use a network service. No production engine is
/// wired through the saved-games review route.
abstract interface class LocalPostGameReviewAnalyzer {
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  });
}

/// The single app-lifetime authority for post-game review work.
///
/// Starting a job synchronously retires its predecessor. Timeouts and caller
/// cancellation complete the public result even when an analyzer is slow to
/// cooperate, while the cancellation token retires any shared-engine session.
class PostGameReviewJobOwner {
  PostGameReviewJobOwner({
    required this.analyzer,
    this.timeout = const Duration(seconds: 20),
  }) {
    if (timeout <= Duration.zero || timeout > const Duration(minutes: 2)) {
      throw ArgumentError.value(timeout, 'timeout');
    }
  }

  final LocalPostGameReviewAnalyzer analyzer;
  final Duration timeout;
  PostGameReviewJob? _activeJob;
  bool _closed = false;
  bool _starting = false;

  bool get active => _activeJob != null;

  PostGameReviewJob start(GameArchive archive) {
    if (_closed) throw StateError('Post-game review owner is closed.');
    if (_starting) {
      throw StateError('Post-game review cannot start reentrantly.');
    }
    _starting = true;
    try {
      final previous = _activeJob;
      _activeJob = null;
      previous?._cancel();
      if (_closed) throw StateError('Post-game review owner is closed.');
      final job = PostGameReviewJob._(this, archive);
      _activeJob = job;
      job._timer = Timer(timeout, () => _timeout(job));
      unawaited(_run(job));
      return job;
    } finally {
      _starting = false;
    }
  }

  Future<void> _run(PostGameReviewJob job) async {
    try {
      final result = await analyzer.analyze(
        job.archive,
        cancellation: job._cancellation,
      );
      if (job._completed || !identical(_activeJob, job)) return;
      job._cancellation.throwIfCancelled();
      if (!result.matches(job.archive)) {
        throw const FormatException(
          'Post-game review result no longer matches its archive.',
        );
      }
      job._complete(result);
    } catch (error, stackTrace) {
      if (!job._completed) job._completeError(error, stackTrace);
    } finally {
      job._timer?.cancel();
      if (identical(_activeJob, job) && job._completed) _activeJob = null;
    }
  }

  void _timeout(PostGameReviewJob job) {
    if (job._completed) return;
    if (identical(_activeJob, job)) _activeJob = null;
    job._completeError(TimeoutException('Quick review timed out.', timeout));
    job._cancellation.cancel();
  }

  void _cancel(PostGameReviewJob job) {
    if (job._completed) return;
    if (identical(_activeJob, job)) _activeJob = null;
    job._completeError(const PostGameReviewCancelled());
    job._cancellation.cancel();
  }

  void close() {
    if (_closed) return;
    _closed = true;
    _activeJob?._cancel();
    _activeJob = null;
  }
}

class PostGameReviewJob {
  PostGameReviewJob._(this._owner, this.archive);

  final PostGameReviewJobOwner _owner;
  final GameArchive archive;
  final PostGameReviewCancellation _cancellation = PostGameReviewCancellation();
  final Completer<PostGameReviewAnalysis> _result =
      Completer<PostGameReviewAnalysis>();
  Timer? _timer;

  Future<PostGameReviewAnalysis> get result => _result.future;
  bool get _completed => _result.isCompleted;

  void cancel() => _owner._cancel(this);

  void _cancel() => _owner._cancel(this);

  void _complete(PostGameReviewAnalysis analysis) {
    _timer?.cancel();
    _result.complete(analysis);
  }

  void _completeError(Object error, [StackTrace? stackTrace]) {
    _timer?.cancel();
    _result.completeError(error, stackTrace ?? StackTrace.current);
  }
}

/// Uses the shared engine lane for a bounded set of positions, then falls back
/// atomically to the existing on-device heuristic if the engine is unavailable.
class EngineBackedPostGameReviewAnalyzer
    implements LocalPostGameReviewAnalyzer {
  EngineBackedPostGameReviewAnalyzer({
    required this.runtime,
    this.fallback = const BoundedLocalPostGameReviewAnalyzer(),
    this.maximumPositions = 6,
    this.limits,
    PostGameReviewAnalysisCache? cache,
  }) : cache = cache ?? PostGameReviewAnalysisCache() {
    if (maximumPositions <= 0 || maximumPositions > 12) {
      throw ArgumentError.value(maximumPositions, 'maximumPositions');
    }
  }

  final EngineRuntime runtime;
  final LocalPostGameReviewAnalyzer fallback;
  final int maximumPositions;
  final EngineSearchLimits? limits;
  final PostGameReviewAnalysisCache cache;

  @override
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  }) async {
    final token = cancellation ?? PostGameReviewCancellation();
    token.throwIfCancelled();
    if (archive.variant != GameVariant.classic ||
        archive.status == GameArchiveStatus.active ||
        archive.captureQuality != GameCaptureQuality.complete ||
        archive.positions.first.ply != 0) {
      return fallback.analyze(archive, cancellation: token);
    }

    final effectiveLimits =
        limits ??
        EngineSearchLimits(
          moveTime: const Duration(milliseconds: 250),
          multiPv: 2,
        );
    final candidates = _candidatePositionIndices(archive, maximumPositions);
    final identity = runtime.identity;
    final cacheKey = identity == null
        ? null
        : _reviewCacheKey(
            identity: identity,
            archive: archive,
            candidates: candidates,
            limits: effectiveLimits,
          );
    final cached = cacheKey == null ? null : cache.read(cacheKey);
    if (cached != null && cached.matches(archive)) return cached;
    if (!runtime.supportsClassicPostGameAnalysis) {
      return fallback.analyze(archive, cancellation: token);
    }

    final session = runtime.openSession();
    final removeCancellation = token.addListener(session.retireAuthority);
    try {
      final engineInsights = <_EngineInsight>[];
      for (final index in candidates) {
        token.throwIfCancelled();
        final position = archive.positions[index];
        final playedMove = _transitionUci(archive.transitions[index]);
        final infos = <int, EngineSearchInfo>{};
        EngineBestMove? bestMove;
        EngineFailureReason? failure;
        var completed = false;
        await for (final event in session.searchPostGameAnalysis(
          fen: position.fen,
          positionRevision: position.ply,
          limits: effectiveLimits,
        )) {
          token.throwIfCancelled();
          switch (event) {
            case EngineSearchInfo():
              infos[event.multiPv] = event;
            case EngineBestMove():
              bestMove = event;
            case EngineSearchFailed():
              failure = event.reason;
            case EngineSearchCompleted():
              completed = true;
            default:
              break;
          }
        }
        if (failure != null || !completed) {
          return fallback.analyze(archive, cancellation: token);
        }
        final best = infos[1];
        final bestUci = best?.principalVariation.firstOrNull ?? bestMove?.uci;
        if (bestUci == null || bestUci == playedMove) continue;
        final played = infos.values.where(
          (info) => info.principalVariation.firstOrNull == playedMove,
        );
        if (best == null || played.isEmpty) continue;
        engineInsights.add(
          _EngineInsight(
            ply: position.ply,
            continuation: best.principalVariation
                .take(postGameReviewMaximumContinuationPlies)
                .toList(growable: false),
            scoreLoss: _scoreLoss(best.score, played.first.score, position.fen),
          ),
        );
      }
      token.throwIfCancelled();
      engineInsights.sort(_compareEngineInsights);
      final result = PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: engineInsights
            .take(postGameReviewMaximumInsights)
            .map(
              (value) => PostGameReviewInsight(
                ply: value.ply,
                kind: PostGameReviewInsightKind.candidateLine,
                continuation: value.continuation,
              ),
            ),
      );
      if (cacheKey != null) cache.write(cacheKey, result);
      return result;
    } on PostGameReviewCancelled {
      rethrow;
    } on Object {
      token.throwIfCancelled();
      return fallback.analyze(archive, cancellation: token);
    } finally {
      removeCancellation();
      session.close();
    }
  }
}

class PostGameReviewAnalysisCache {
  PostGameReviewAnalysisCache({
    this.maximumBytes = 256 * 1024,
    this.maximumEntries = 128,
  }) {
    if (maximumBytes <= 0 || maximumBytes > 16 * 1024 * 1024) {
      throw ArgumentError.value(maximumBytes, 'maximumBytes');
    }
    if (maximumEntries <= 0 || maximumEntries > 1024) {
      throw ArgumentError.value(maximumEntries, 'maximumEntries');
    }
  }

  final int maximumBytes;
  final int maximumEntries;
  final LinkedHashMap<String, _CachedPostGameReview> _entries =
      LinkedHashMap<String, _CachedPostGameReview>();
  int _usedBytes = 0;

  int get usedBytes => _usedBytes;
  int get entryCount => _entries.length;

  PostGameReviewAnalysis? read(String key) {
    final entry = _entries.remove(key);
    if (entry == null) return null;
    _entries[key] = entry;
    return entry.analysis;
  }

  void write(String key, PostGameReviewAnalysis analysis) {
    final bytes = _estimatedCacheBytes(key, analysis);
    final replaced = _entries.remove(key);
    if (replaced != null) _usedBytes -= replaced.bytes;
    if (bytes > maximumBytes) return;
    while (_entries.isNotEmpty &&
        (_usedBytes + bytes > maximumBytes ||
            _entries.length >= maximumEntries)) {
      final oldestKey = _entries.keys.first;
      _usedBytes -= _entries.remove(oldestKey)!.bytes;
    }
    _entries[key] = _CachedPostGameReview(analysis, bytes);
    _usedBytes += bytes;
  }
}

String _reviewCacheKey({
  required EngineRuntimeIdentity identity,
  required GameArchive archive,
  required List<int> candidates,
  required EngineSearchLimits limits,
}) {
  final fenPayload = candidates
      .map(
        (index) =>
            '${archive.positions[index].ply}:${archive.positions[index].fen}:'
            '${_transitionUci(archive.transitions[index])}:'
            '${archive.positions[index + 1].fen}',
      )
      .join('\n');
  final fenDigest = sha256.convert(utf8.encode(fenPayload)).toString();
  return jsonEncode(<Object?>[
    identity.providerId,
    identity.engineVersion,
    archive.libraryId,
    archive.gameId,
    archive.origin.name,
    archive.lineRevision,
    archive.currentPly,
    candidates.map((index) => archive.positions[index].ply).toList(),
    limits.moveTime?.inMicroseconds,
    limits.nodes,
    limits.depth,
    limits.multiPv,
    fenDigest,
  ]);
}

int _estimatedCacheBytes(String key, PostGameReviewAnalysis analysis) {
  var bytes = utf8.encode(key).length + 256;
  for (final insight in analysis.insights) {
    bytes += 64;
    for (final move in insight.continuation) {
      bytes += utf8.encode(move).length;
    }
  }
  return bytes;
}

class _CachedPostGameReview {
  const _CachedPostGameReview(this.analysis, this.bytes);

  final PostGameReviewAnalysis analysis;
  final int bytes;
}

class _EngineInsight {
  const _EngineInsight({
    required this.ply,
    required this.continuation,
    required this.scoreLoss,
  });

  final int ply;
  final List<String> continuation;
  final int? scoreLoss;
}

List<int> _candidatePositionIndices(GameArchive archive, int maximum) {
  final count = archive.transitions.length;
  if (count <= maximum) return List<int>.generate(count, (index) => index);
  final indices = <int>{};
  for (var index = 0; index < count; index++) {
    final transition = archive.transitions[index];
    if (transition.captureSquare != null || transition.promotion != null) {
      indices.add(index);
    }
  }
  for (var slot = 0; indices.length < maximum && slot < maximum * 2; slot++) {
    indices.add(((slot + 1) * count ~/ (maximum + 1)).clamp(0, count - 1));
  }
  final ordered = indices.toList()..sort();
  return ordered.take(maximum).toList(growable: false);
}

int? _scoreLoss(EngineScore best, EngineScore played, String fen) {
  if (best.value == null || played.value == null) return null;
  final whiteToMove = fen.split(' ')[1] == 'w';
  final delta = whiteToMove
      ? best.value! - played.value!
      : played.value! - best.value!;
  return delta < 0 ? 0 : delta;
}

int _compareEngineInsights(_EngineInsight left, _EngineInsight right) {
  final byConfidence = (right.scoreLoss != null ? 1 : 0).compareTo(
    left.scoreLoss != null ? 1 : 0,
  );
  if (byConfidence != 0) return byConfidence;
  final byLoss = (right.scoreLoss ?? 0).compareTo(left.scoreLoss ?? 0);
  return byLoss != 0 ? byLoss : left.ply.compareTo(right.ply);
}

class BoundedLocalPostGameReviewAnalyzer
    implements LocalPostGameReviewAnalyzer {
  const BoundedLocalPostGameReviewAnalyzer({
    this.nodeBudget = 8192,
    this.yieldEvery = 64,
    this.yieldControl,
  }) : assert(nodeBudget >= 0),
       assert(yieldEvery > 0);

  final int nodeBudget;
  final int yieldEvery;
  final Future<void> Function()? yieldControl;

  @override
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  }) async {
    final token = cancellation ?? PostGameReviewCancellation();
    if (archive.variant != GameVariant.classic ||
        archive.status == GameArchiveStatus.active ||
        archive.captureQuality != GameCaptureQuality.complete ||
        archive.positions.first.ply != 0) {
      throw const FormatException('Archive is not eligible for quick review.');
    }
    var nodes = 0;
    Future<void> yieldNow() async {
      await (yieldControl?.call() ?? Future<void>.delayed(Duration.zero));
      token.throwIfCancelled();
    }

    Future<void> tick() async {
      token.throwIfCancelled();
      nodes++;
      if (nodes % yieldEvery == 0) {
        await yieldNow();
      }
    }

    Future<_RankedMove?> chooseRankedMove(rules.Chess chess) async {
      final mover = chess.turn;
      final legal = List<rules.Move>.of(chess.generate_moves())
        ..sort((left, right) => _moveUci(left).compareTo(_moveUci(right)));
      _RankedMove? best;
      for (final candidate in legal) {
        if (nodes >= nodeBudget) return null;
        chess.make_move(candidate);
        await tick();
        final immediate = _materialBalance(chess, mover);
        var replyFloor = immediate;
        var allowsMateReply = false;
        final replies = List<rules.Move>.of(chess.generate_moves());
        for (final reply in replies) {
          if (nodes >= nodeBudget) {
            chess.undo();
            return null;
          }
          chess.make_move(reply);
          await tick();
          final afterReply = _materialBalance(chess, mover);
          if (afterReply < replyFloor) replyFloor = afterReply;
          allowsMateReply = allowsMateReply || chess.in_checkmate;
          chess.undo();
        }
        final ranked = _RankedMove(
          move: candidate,
          mate: chess.in_checkmate,
          allowsMateReply: allowsMateReply,
          replyMaterialFloor: replyFloor,
          immediateMaterial: immediate,
          check: chess.in_check,
          replyMobility: replies.length,
          uci: _moveUci(candidate),
        );
        chess.undo();
        if (best == null || ranked.compareTo(best) < 0) best = ranked;
      }
      return best;
    }

    final initialFen = archive.positions.first.fen;
    final replay = validatedStandardChessInitialPosition(initialFen);
    for (var index = 0; index < archive.transitions.length; index++) {
      token.throwIfCancelled();
      final uci = _transitionUci(archive.transitions[index]);
      if (!replay.move(<String, String>{
            'from': uci.substring(0, 2),
            'to': uci.substring(2, 4),
            if (uci.length == 5) 'promotion': uci[4],
          }) ||
          replay.fen != archive.positions[index + 1].fen) {
        throw const FormatException('Archive contains an illegal replay.');
      }
      if ((index + 1) % 32 == 0) await yieldNow();
    }

    final insights = <PostGameReviewInsight>[];
    for (
      var index = 0;
      index < archive.positions.length &&
          insights.length < postGameReviewMaximumInsights;
      index++
    ) {
      token.throwIfCancelled();
      final position = archive.positions[index];
      if (nodes < nodeBudget) {
        final chess = rules.Chess.fromFEN(position.fen);
        final legal = List<rules.Move>.of(chess.generate_moves())
          ..sort((a, b) => _moveUci(a).compareTo(_moveUci(b)));
        if (legal.isNotEmpty && nodes + legal.length <= nodeBudget) {
          String? matingMove;
          for (final move in legal) {
            chess.make_move(move);
            await tick();
            if (chess.in_checkmate) matingMove ??= _moveUci(move);
            chess.undo();
          }
          if (matingMove != null) {
            insights.add(
              PostGameReviewInsight(
                ply: position.ply,
                kind: PostGameReviewInsightKind.mateInOne,
                side: chess.turn == rules.Chess.WHITE
                    ? GameSide.white
                    : GameSide.black,
                continuation: <String>[matingMove],
              ),
            );
            continue;
          }
        }
      }
      if (index > 0) {
        final transition = archive.transitions[index - 1];
        if (transition.captureSquare != null || transition.promotion != null) {
          final captured = transition.captureSquare == null
              ? null
              : validatedStandardChessInitialPosition(
                  archive.positions[index - 1].fen,
                ).get(algebraicSquare(transition.captureSquare!).toLowerCase());
          insights.add(
            PostGameReviewInsight(
              ply: position.ply,
              kind: PostGameReviewInsightKind.materialChange,
              side: transition.side,
              capturedPiece: captured == null
                  ? null
                  : _reviewPiece(captured.type),
              captureSquare: transition.captureSquare,
              promotion: transition.promotion,
            ),
          );
        }
      }
      if ((index + 1) % 32 == 0) await yieldNow();
    }

    if (insights.length < postGameReviewMaximumInsights) {
      for (final position in archive.positions.reversed) {
        if (insights.any((value) => value.ply == position.ply)) continue;
        final chess = rules.Chess.fromFEN(position.fen);
        final line = <String>[];
        while (!chess.game_over &&
            line.length < postGameReviewMaximumContinuationPlies &&
            nodes < nodeBudget) {
          final best = await chooseRankedMove(chess);
          final move = best?.move;
          if (move == null) break;
          line.add(_moveUci(move));
          chess.make_move(move);
          if (chess.in_checkmate) break;
        }
        if (line.isNotEmpty) {
          insights.add(
            PostGameReviewInsight(
              ply: position.ply,
              kind: PostGameReviewInsightKind.candidateLine,
              continuation: line,
            ),
          );
        }
        break;
      }
    }
    token.throwIfCancelled();
    return PostGameReviewAnalysis.forArchive(
      archive: archive,
      insights: insights,
    );
  }
}

class _RankedMove implements Comparable<_RankedMove> {
  const _RankedMove({
    required this.move,
    required this.mate,
    required this.allowsMateReply,
    required this.replyMaterialFloor,
    required this.immediateMaterial,
    required this.check,
    required this.replyMobility,
    required this.uci,
  });

  final rules.Move move;
  final bool mate;
  final bool allowsMateReply;
  final int replyMaterialFloor;
  final int immediateMaterial;
  final bool check;
  final int replyMobility;
  final String uci;

  @override
  int compareTo(_RankedMove other) {
    int compare(bool left, bool right) =>
        (right ? 1 : 0).compareTo(left ? 1 : 0);
    var result = compare(mate, other.mate);
    if (result != 0) return result;
    result = (allowsMateReply ? 1 : 0).compareTo(other.allowsMateReply ? 1 : 0);
    if (result != 0) return result;
    result = other.replyMaterialFloor.compareTo(replyMaterialFloor);
    if (result != 0) return result;
    result = other.immediateMaterial.compareTo(immediateMaterial);
    if (result != 0) return result;
    result = compare(check, other.check);
    if (result != 0) return result;
    result = replyMobility.compareTo(other.replyMobility);
    return result != 0 ? result : uci.compareTo(other.uci);
  }
}

int _materialBalance(rules.Chess chess, rules.Color perspective) {
  var balance = 0;
  for (var rank = 1; rank <= 8; rank++) {
    for (var file = 0; file < 8; file++) {
      final piece = chess.get('${String.fromCharCode(97 + file)}$rank');
      if (piece == null) continue;
      final value = switch (piece.type.name) {
        'p' => 100,
        'n' => 320,
        'b' => 330,
        'r' => 500,
        'q' => 900,
        _ => 0,
      };
      balance += piece.color == perspective ? value : -value;
    }
  }
  return balance;
}

String _transitionUci(ArchivedTransition move) {
  final promotion = switch (move.promotion) {
    PromotionPiece.queen => 'q',
    PromotionPiece.rook => 'r',
    PromotionPiece.bishop => 'b',
    PromotionPiece.knight => 'n',
    null => '',
  };
  return '${algebraicSquare(move.from).toLowerCase()}'
      '${algebraicSquare(move.to).toLowerCase()}$promotion';
}

String _moveUci(rules.Move move) {
  final promotion = move.promotion?.name ?? '';
  return '${move.fromAlgebraic}${move.toAlgebraic}$promotion';
}

PostGameReviewPiece _reviewPiece(rules.PieceType piece) => switch (piece.name) {
  'p' => PostGameReviewPiece.pawn,
  'n' => PostGameReviewPiece.knight,
  'b' => PostGameReviewPiece.bishop,
  'r' => PostGameReviewPiece.rook,
  'q' => PostGameReviewPiece.queen,
  _ => throw const FormatException('King cannot be captured.'),
};
