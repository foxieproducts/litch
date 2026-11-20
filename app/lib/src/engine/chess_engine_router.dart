import 'dart:async';

import 'chess_engine_models.dart';
import 'chess_engine_provider.dart';
import 'engine_position_validator.dart';

const int _minimumInfoIntervalMicros = 250000;
const int _maximumProviderEvents = 4096;
final RegExp _uciMove = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');

abstract interface class EngineDeadlineHandle {
  void cancel();
}

typedef EngineDeadlineFactory =
    EngineDeadlineHandle Function(Duration delay, void Function() callback);

class EnginePolicyLease {
  EnginePolicyLease._(this._release);

  void Function()? _release;

  void release() {
    _release?.call();
    _release = null;
  }
}

/// Owns the one process-wide provider search and fences stale provider output.
class ChessEngineRouter {
  ChessEngineRouter(
    this.provider, {
    Iterable<EnginePositionValidator> validators =
        const <EnginePositionValidator>[StandardChessEnginePositionValidator()],
    int Function()? monotonicMicros,
    EngineDeadlineFactory? deadlineFactory,
    this.maximumSearchDuration = const Duration(seconds: 20),
    this.moveTimeCompletionGrace = const Duration(milliseconds: 250),
    this.providerCleanupTimeout = const Duration(seconds: 2),
  }) : _validators = <ChessEngineVariant, EnginePositionValidator>{
         for (final validator in validators) validator.variant: validator,
       },
       _deadlineFactory = deadlineFactory ?? _defaultDeadlineFactory {
    if (maximumSearchDuration <= Duration.zero ||
        maximumSearchDuration > const Duration(minutes: 10) ||
        moveTimeCompletionGrace.isNegative ||
        moveTimeCompletionGrace > const Duration(seconds: 2) ||
        providerCleanupTimeout <= Duration.zero ||
        providerCleanupTimeout > const Duration(seconds: 10)) {
      throw ArgumentError('Engine router timeout is out of bounds.');
    }
    if (monotonicMicros != null) {
      _monotonicMicros = monotonicMicros;
    } else {
      final clock = Stopwatch()..start();
      _monotonicMicros = () => clock.elapsedMicroseconds;
    }
  }

  final ChessEngineProvider provider;
  final Map<ChessEngineVariant, EnginePositionValidator> _validators;
  final EngineDeadlineFactory _deadlineFactory;
  final Duration maximumSearchDuration;

  /// Failure-only allowance for an adapter to emit its terminal response after
  /// the requested compute time expires.
  final Duration moveTimeCompletionGrace;
  final Duration providerCleanupTimeout;
  late final int Function() _monotonicMicros;
  final Set<Object> _humanOnlineProhibitions = <Object>{};
  final Set<Object> _remoteAuthorizations = <Object>{};
  Future<void> _cleanupTail = Future<void>.value();
  Future<void>? _closeFuture;
  _EngineJob? _job;
  var _disposed = false;
  var _providerQuarantined = false;

  bool get humanOnlineSessionActive => _humanOnlineProhibitions.isNotEmpty;

  EnginePolicyLease prohibitForHumanOnlineSession(String sessionId) {
    if (sessionId.trim().isEmpty || sessionId.length > 128) {
      throw ArgumentError.value(sessionId, 'sessionId');
    }
    final token = Object();
    _humanOnlineProhibitions.add(token);
    _retireCurrent(EngineFailureReason.prohibited);
    return EnginePolicyLease._(() => _humanOnlineProhibitions.remove(token));
  }

  EnginePolicyLease authorizeRemoteEngine() {
    final token = Object();
    _remoteAuthorizations.add(token);
    return EnginePolicyLease._(() {
      _remoteAuthorizations.remove(token);
      if (_remoteAuthorizations.isEmpty &&
          provider.capabilities.privacyClass == EnginePrivacyClass.remote) {
        _retireCurrent(EngineFailureReason.prohibited);
      }
    });
  }

  Stream<EngineSearchEvent> search(EngineSearchRequest request) {
    final rejection = _admissionFailure(request);
    if (rejection != null) {
      return Stream<EngineSearchEvent>.value(_failure(request, rejection));
    }
    final current = _job;
    if (current != null &&
        current.request.purpose == EngineSearchPurpose.computerMove &&
        request.purpose != EngineSearchPurpose.computerMove) {
      return Stream<EngineSearchEvent>.value(
        _failure(request, EngineFailureReason.busy),
      );
    }
    _retireCurrent(EngineFailureReason.cancelled);
    final waitForCleanup = _cleanupTail;
    final capabilities = provider.capabilities;
    late final _EngineJob job;
    job = _EngineJob(request, capabilities, () => _cancelConsumer(job));
    _job = job;
    unawaited(_start(job, waitForCleanup));
    return job.controller.stream;
  }

  void cancel(EngineSearchPurpose purpose) {
    if (_job?.request.purpose == purpose) {
      _retireCurrent(EngineFailureReason.cancelled);
    }
  }

  Future<void> close() => _closeFuture ??= _close();

  Future<void> _close() async {
    _disposed = true;
    _retireCurrent(EngineFailureReason.cancelled);
    await _cleanupTail;
    await provider.close();
  }

  EngineFailureReason? _admissionFailure(EngineSearchRequest request) {
    if (_disposed || _providerQuarantined) {
      return EngineFailureReason.unavailable;
    }
    if (_humanOnlineProhibitions.isNotEmpty) {
      return EngineFailureReason.prohibited;
    }
    final capabilities = provider.capabilities;
    if (capabilities.privacyClass == EnginePrivacyClass.remote &&
        _remoteAuthorizations.isEmpty) {
      return EngineFailureReason.prohibited;
    }
    final validator = _validators[request.variant];
    if (!_supports(request, capabilities) ||
        validator == null ||
        !validator.isCanonicalPosition(request.fen)) {
      return EngineFailureReason.unsupported;
    }
    return null;
  }

  bool _supports(EngineSearchRequest request, EngineCapabilities capabilities) {
    final limits = request.limits;
    return capabilities.supportsCancellation &&
        capabilities.variants.contains(request.variant) &&
        capabilities.purposes.contains(request.purpose) &&
        limits.multiPv <= capabilities.maximumMultiPv &&
        (limits.moveTime == null || capabilities.supportsMoveTime) &&
        (limits.nodes == null || capabilities.supportsNodes) &&
        (limits.depth == null || capabilities.supportsDepth) &&
        request.fen.length <= capabilities.maximumRequestBytes;
  }

  Future<void> _start(_EngineJob job, Future<void> waitForCleanup) async {
    await waitForCleanup;
    if (!_isCurrent(job)) return;
    final rejection = _admissionFailure(job.request);
    if (rejection != null) {
      _finishFailure(job, rejection);
      return;
    }
    try {
      final subscription = provider
          .search(job.request, job.cancellation)
          .listen(
            (event) => _handleEvent(job, event),
            onError: (Object _, StackTrace stackTrace) =>
                _finishFailure(job, EngineFailureReason.transport),
            onDone: () {
              if (!job.terminal) {
                _finishFailure(job, EngineFailureReason.invalidResponse);
              }
            },
            cancelOnError: false,
          );
      job.subscription = subscription;
      if (job.terminal) return;
      final timeout = _safetyDeadline(job.request);
      job.timeout = _deadlineFactory(
        timeout,
        () => _finishFailure(job, EngineFailureReason.timedOut),
      );
    } on Object {
      _finishFailure(job, EngineFailureReason.transport);
    }
  }

  Duration _safetyDeadline(EngineSearchRequest request) {
    var timeout = maximumSearchDuration;
    final hardBudget = request.timeControl?.hardMoveBudget;
    if (hardBudget != null && hardBudget < timeout) timeout = hardBudget;
    final moveTime = request.limits.moveTime;
    if (moveTime != null) {
      final adapterBudget = moveTime + moveTimeCompletionGrace;
      if (adapterBudget < timeout) timeout = adapterBudget;
    }
    return timeout;
  }

  void _handleEvent(_EngineJob job, EngineSearchEvent event) {
    if (!_isCurrent(job) || job.terminal) return;
    job.eventCount++;
    if (job.eventCount > _maximumProviderEvents) {
      _finishFailure(job, EngineFailureReason.invalidResponse);
      return;
    }
    if (!_matches(job.request, event)) {
      if (event is EngineSearchCompleted || event is EngineSearchFailed) {
        _finishFailure(job, EngineFailureReason.invalidResponse);
      }
      return;
    }
    switch (event) {
      case EngineSearchStarted():
        if (job.started) {
          _finishFailure(job, EngineFailureReason.invalidResponse);
          return;
        }
        job.started = true;
        job.controller.add(event);
      case EngineSearchInfo():
        if (!job.started || !_validInfo(event, job)) {
          _finishFailure(job, EngineFailureReason.invalidResponse);
          return;
        }
        _publishOrCoalesceInfo(job, event);
      case EngineBestMove():
        if (!job.started ||
            job.bestMove != null ||
            !_validUci(event.uci) ||
            (event.ponder != null && !_validUci(event.ponder!)) ||
            !_validators[job.request.variant]!.isLegalLine(
              job.request.fen,
              <String>[event.uci, ?event.ponder],
            )) {
          _finishFailure(job, EngineFailureReason.invalidResponse);
          return;
        }
        job.bestMove = event;
      case EngineNoLegalMove():
        if (!job.started ||
            job.bestMove != null ||
            job.noLegalMove != null ||
            _validators[job.request.variant]!.hasLegalMoves(job.request.fen)) {
          _finishFailure(job, EngineFailureReason.invalidResponse);
          return;
        }
        job.noLegalMove = event;
      case EngineSearchCompleted():
        if (!job.started || (job.bestMove == null && job.noLegalMove == null)) {
          _finishFailure(job, EngineFailureReason.invalidResponse);
          return;
        }
        _flushPendingInfo(job);
        final bestMove = job.bestMove;
        if (bestMove != null) job.controller.add(bestMove);
        final noLegalMove = job.noLegalMove;
        if (noLegalMove != null) job.controller.add(noLegalMove);
        job.controller.add(event);
        _finish(job);
      case EngineSearchFailed():
        job.controller.add(event);
        job.cancellation.cancel();
        _finish(job);
    }
  }

  void _publishOrCoalesceInfo(_EngineJob job, EngineSearchInfo info) {
    final now = _monotonicMicros();
    final elapsed = now - job.lastInfoMicros;
    job.pendingInfo[info.multiPv] = info;
    if (elapsed >= _minimumInfoIntervalMicros) {
      _flushPendingInfo(job);
      return;
    }
    job.infoDeadline ??= _deadlineFactory(
      Duration(microseconds: _minimumInfoIntervalMicros - elapsed),
      () {
        job.infoDeadline = null;
        if (!_isCurrent(job)) return;
        _flushPendingInfo(job);
      },
    );
  }

  void _flushPendingInfo(_EngineJob job) {
    if (job.pendingInfo.isEmpty) return;
    job.infoDeadline?.cancel();
    job.infoDeadline = null;
    final ranks = job.pendingInfo.keys.toList()..sort();
    for (final rank in ranks) {
      job.controller.add(job.pendingInfo[rank]!);
    }
    job.pendingInfo.clear();
    job.lastInfoMicros = _monotonicMicros();
  }

  bool _validInfo(EngineSearchInfo info, _EngineJob job) =>
      info.multiPv <= job.request.limits.multiPv &&
      info.principalVariation.length <= job.capabilities.maximumPvPlies &&
      info.principalVariation.every(_validUci) &&
      _validators[job.request.variant]!.isLegalLine(
        job.request.fen,
        info.principalVariation,
      );

  bool _validUci(String value) => _uciMove.hasMatch(value);

  bool _matches(EngineSearchRequest request, EngineSearchEvent event) =>
      event.jobId == request.jobId &&
      event.sessionId == request.sessionId &&
      event.positionRevision == request.positionRevision;

  bool _isCurrent(_EngineJob job) =>
      identical(_job, job) && !job.cancellation.isCancelled;

  void _finishFailure(_EngineJob job, EngineFailureReason reason) {
    if (!_isCurrent(job) || job.terminal) return;
    job.controller.add(_failure(job.request, reason));
    job.cancellation.cancel();
    _finish(job);
  }

  void _finish(_EngineJob job) {
    if (job.terminal) return;
    job.terminal = true;
    job.timeout?.cancel();
    job.infoDeadline?.cancel();
    job.pendingInfo.clear();
    if (identical(_job, job)) _job = null;
    _queueSubscriptionCleanup(job);
  }

  void _retireCurrent(EngineFailureReason reason) {
    final job = _job;
    if (job == null || job.terminal) return;
    _job = null;
    job.controller.add(_failure(job.request, reason));
    job.cancellation.cancel();
    _finishDetached(job);
  }

  void _cancelConsumer(_EngineJob job) {
    if (!_isCurrent(job) || job.terminal) return;
    _job = null;
    job.cancellation.cancel();
    _finishDetached(job);
  }

  void _finishDetached(_EngineJob job) {
    job.terminal = true;
    job.timeout?.cancel();
    job.infoDeadline?.cancel();
    job.pendingInfo.clear();
    _queueSubscriptionCleanup(job);
  }

  void _queueSubscriptionCleanup(_EngineJob job) {
    final prior = _cleanupTail;
    _cleanupTail = () async {
      try {
        await prior;
      } on Object {
        // Cleanup failure cannot strand all later searches.
      }
      late final Future<void> cancellation;
      try {
        cancellation = job.subscription?.cancel() ?? Future<void>.value();
      } on Object {
        _providerQuarantined = true;
        if (!job.controller.isClosed) unawaited(job.controller.close());
        return;
      }
      final completion = Completer<bool>();
      unawaited(
        cancellation.then(
          (_) {
            if (!completion.isCompleted) completion.complete(true);
          },
          onError: (Object _, StackTrace stackTrace) {
            if (!completion.isCompleted) completion.complete(false);
          },
        ),
      );
      // A normal StreamSubscription cancellation commonly completes in the
      // next microtask. Let that fast path settle before arming the watchdog;
      // otherwise a fake clock (or a heavily delayed event loop) can fire the
      // timeout first and quarantine a provider which already cleaned up.
      await Future<void>.value();
      final bool cleaned;
      if (completion.isCompleted) {
        cleaned = await completion.future;
      } else {
        final timeout = _deadlineFactory(providerCleanupTimeout, () {
          if (!completion.isCompleted) completion.complete(false);
        });
        cleaned = await completion.future;
        timeout.cancel();
      }
      if (!cleaned) _providerQuarantined = true;
      if (!job.controller.isClosed) unawaited(job.controller.close());
    }();
  }

  EngineSearchFailed _failure(
    EngineSearchRequest request,
    EngineFailureReason reason,
  ) => EngineSearchFailed(
    jobId: request.jobId,
    sessionId: request.sessionId,
    positionRevision: request.positionRevision,
    reason: reason,
  );
}

class _EngineJob {
  _EngineJob(
    this.request,
    this.capabilities,
    FutureOr<void> Function() onCancel,
  ) : controller = StreamController<EngineSearchEvent>(onCancel: onCancel);

  final EngineSearchRequest request;
  final EngineCapabilities capabilities;
  final EngineCancellation cancellation = EngineCancellation();
  final StreamController<EngineSearchEvent> controller;
  StreamSubscription<EngineSearchEvent>? subscription;
  EngineDeadlineHandle? timeout;
  EngineDeadlineHandle? infoDeadline;
  final Map<int, EngineSearchInfo> pendingInfo = <int, EngineSearchInfo>{};
  EngineBestMove? bestMove;
  EngineNoLegalMove? noLegalMove;
  int lastInfoMicros = -_minimumInfoIntervalMicros;
  int eventCount = 0;
  bool started = false;
  bool terminal = false;
}

class _TimerDeadline implements EngineDeadlineHandle {
  _TimerDeadline(Duration delay, void Function() callback)
    : _timer = Timer(delay, callback);

  final Timer _timer;

  @override
  void cancel() => _timer.cancel();
}

EngineDeadlineHandle _defaultDeadlineFactory(
  Duration delay,
  void Function() callback,
) => _TimerDeadline(delay, callback);
