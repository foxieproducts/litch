import 'dart:async';

import 'package:flutter/widgets.dart';

import 'chess_engine_models.dart';
import 'chess_engine_provider.dart';
import 'chess_engine_router.dart';

const int _maximumEngineSessionId = 0x7fffffff;
const int _maximumAuthorityRevision = 0x7fffffff;

typedef EngineProviderFactory = FutureOr<ChessEngineProvider?> Function();
typedef EngineRouterFactory =
    ChessEngineRouter Function(ChessEngineProvider provider);
typedef EngineShutdownWaiter =
    Future<void> Function(Future<void> shutdown, Duration timeout);

@immutable
class EngineRuntimeIdentity {
  const EngineRuntimeIdentity({
    required this.providerId,
    required this.engineVersion,
  });

  final String providerId;
  final String engineVersion;
}

/// App-root owner for the single provider and router used by phone-side chess.
///
/// A runtime can be unavailable, allowing the rest of the app to start without
/// a packaged engine. Sessions still return a typed unavailable result in that
/// state rather than making screens special-case a missing router.
class EngineRuntime {
  EngineRuntime({
    ChessEngineProvider? provider,
    EngineRouterFactory? routerFactory,
    EngineShutdownWaiter? shutdownWaiter,
    this.shutdownTimeout = const Duration(seconds: 3),
  }) : _router = provider == null
           ? null
           : (routerFactory ?? ChessEngineRouter.new)(provider),
       _shutdownWaiter = shutdownWaiter ?? _waitForShutdown {
    if (shutdownTimeout <= Duration.zero ||
        shutdownTimeout > const Duration(seconds: 10)) {
      throw ArgumentError.value(shutdownTimeout, 'shutdownTimeout');
    }
  }

  /// Opens the provider once and degrades to an unavailable runtime on failure.
  static Future<EngineRuntime> open({
    required EngineProviderFactory providerFactory,
    EngineRouterFactory? routerFactory,
    EngineShutdownWaiter? shutdownWaiter,
    Duration shutdownTimeout = const Duration(seconds: 3),
  }) async {
    ChessEngineProvider? provider;
    try {
      provider = await providerFactory();
      return EngineRuntime(
        provider: provider,
        routerFactory: routerFactory,
        shutdownWaiter: shutdownWaiter,
        shutdownTimeout: shutdownTimeout,
      );
    } on Object {
      if (provider != null) {
        try {
          await (shutdownWaiter ?? _waitForShutdown)(
            provider.close(),
            shutdownTimeout,
          );
        } on Object {
          // A failed initializer is unavailable even if its cleanup also fails.
        }
      }
      return EngineRuntime(
        shutdownWaiter: shutdownWaiter,
        shutdownTimeout: shutdownTimeout,
      );
    }
  }

  final ChessEngineRouter? _router;
  final EngineShutdownWaiter _shutdownWaiter;
  final Duration shutdownTimeout;
  final Set<EngineSearchSession> _sessions = <EngineSearchSession>{};
  int _nextSessionId = 1;
  _ActiveSearch? _activeSearch;
  Future<void>? _closeFuture;
  bool _closing = false;
  bool _closed = false;
  bool? _shutdownWasClean;

  bool get available => _router != null && !_closing && !_closed;
  EngineRuntimeIdentity? get identity {
    final capabilities = _router?.provider.capabilities;
    return capabilities == null
        ? null
        : EngineRuntimeIdentity(
            providerId: capabilities.providerId,
            engineVersion: capabilities.engineVersion,
          );
  }

  bool get supportsClassicComputerMove {
    final EngineCapabilities? capabilities = _router?.provider.capabilities;
    return available &&
        capabilities != null &&
        capabilities.supportsCancellation &&
        capabilities.variants.contains(ChessEngineVariant.classic) &&
        capabilities.purposes.contains(EngineSearchPurpose.computerMove);
  }

  bool get supportsClassicPostGameAnalysis {
    final EngineCapabilities? capabilities = _router?.provider.capabilities;
    return available &&
        capabilities != null &&
        capabilities.supportsCancellation &&
        capabilities.variants.contains(ChessEngineVariant.classic) &&
        capabilities.purposes.contains(EngineSearchPurpose.postGameAnalysis);
  }

  bool get closed => _closed;

  /// Null until shutdown starts, then records whether provider cleanup finished.
  bool? get shutdownWasClean => _shutdownWasClean;

  EngineSearchSession openSession() {
    if (_closing || _closed) {
      throw StateError('Engine runtime is closed.');
    }
    if (_nextSessionId > _maximumEngineSessionId) {
      throw StateError('Engine session identifiers are exhausted.');
    }
    final EngineSearchSession session = EngineSearchSession._(
      this,
      _nextSessionId++,
    );
    _sessions.add(session);
    return session;
  }

  Stream<EngineSearchEvent> _startSearch(
    EngineSearchSession session, {
    required String fen,
    required int positionRevision,
    required EngineSearchPurpose purpose,
    required EngineSearchLimits limits,
    EngineTimeControl? timeControl,
  }) {
    final int authorityRevision = _replaceAuthority(session, purpose);
    final EngineSearchRequest request = EngineSearchRequest(
      jobId:
          'app-${purpose.index}-$authorityRevision-${session.sessionId}-$positionRevision',
      sessionId: session.sessionId,
      positionRevision: positionRevision,
      fen: fen,
      variant: ChessEngineVariant.classic,
      purpose: purpose,
      limits: limits,
      timeControl: timeControl,
    );
    final ChessEngineRouter? router = available ? _router : null;
    final Stream<EngineSearchEvent> source = router == null
        ? Stream<EngineSearchEvent>.value(
            EngineSearchFailed(
              jobId: request.jobId,
              sessionId: request.sessionId,
              positionRevision: request.positionRevision,
              reason: EngineFailureReason.unavailable,
            ),
          )
        : router.search(request);
    return _fenceEvents(session, authorityRevision, request, source);
  }

  Stream<EngineSearchEvent> _fenceEvents(
    EngineSearchSession session,
    int authorityRevision,
    EngineSearchRequest request,
    Stream<EngineSearchEvent> source,
  ) async* {
    try {
      await for (final EngineSearchEvent event in source) {
        if (!session._accepts(authorityRevision, request, event)) continue;
        yield event;
      }
    } finally {
      _completeAuthority(session, authorityRevision, request.purpose);
    }
  }

  int _replaceAuthority(
    EngineSearchSession session,
    EngineSearchPurpose purpose,
  ) {
    final _ActiveSearch? active = _activeSearch;
    if (active != null &&
        active.purpose == EngineSearchPurpose.computerMove &&
        purpose != EngineSearchPurpose.computerMove) {
      return identical(active.session, session)
          ? session.authorityRevision
          : session._advanceAuthority();
    }
    late final int authorityRevision;
    if (active != null) {
      active.session._invalidateFromRuntime(active.authorityRevision);
      authorityRevision = identical(active.session, session)
          ? session.authorityRevision
          : session._advanceAuthority();
      _activeSearch = null;
      _router?.cancel(active.purpose);
    } else {
      authorityRevision = session._advanceAuthority();
    }
    _activeSearch = _ActiveSearch(session, authorityRevision, purpose);
    return authorityRevision;
  }

  void _retireAuthority(EngineSearchSession session) {
    final _ActiveSearch? active = _activeSearch;
    final int previousRevision = session.authorityRevision;
    session._advanceAuthority();
    if (active == null ||
        !identical(active.session, session) ||
        active.authorityRevision != previousRevision) {
      return;
    }
    _activeSearch = null;
    _router?.cancel(active.purpose);
  }

  void _completeAuthority(
    EngineSearchSession session,
    int authorityRevision,
    EngineSearchPurpose purpose,
  ) {
    final _ActiveSearch? active = _activeSearch;
    if (active != null &&
        identical(active.session, session) &&
        active.authorityRevision == authorityRevision &&
        active.purpose == purpose) {
      _activeSearch = null;
    }
  }

  void _closeSession(EngineSearchSession session) {
    if (!_sessions.remove(session)) return;
    _retireAuthority(session);
  }

  Future<void> close() => _closeFuture ??= _close();

  Future<void> _close() async {
    _closing = true;
    for (final EngineSearchSession session in List<EngineSearchSession>.of(
      _sessions,
    )) {
      session._closeFromRuntime();
    }
    _sessions.clear();
    _activeSearch = null;
    final ChessEngineRouter? router = _router;
    if (router == null) {
      _shutdownWasClean = true;
      _closed = true;
      return;
    }
    try {
      await _shutdownWaiter(router.close(), shutdownTimeout);
      _shutdownWasClean = true;
    } on Object {
      _shutdownWasClean = false;
    } finally {
      _closed = true;
    }
  }
}

/// Narrow consumer contract used by a phone-game coordinator.
abstract interface class ComputerMoveSearchSession {
  int get sessionId;
  int get authorityRevision;

  Stream<EngineSearchEvent> searchComputerMove({
    required String fen,
    required int positionRevision,
    EngineTimeControl? timeControl,
    EngineSearchLimits? limits,
  });

  void retireAuthority();
  void cancelComputerMove();
  void close();
}

/// One phone-game consumer of the process-wide engine lane.
class EngineSearchSession implements ComputerMoveSearchSession {
  EngineSearchSession._(this._runtime, this.sessionId);

  final EngineRuntime _runtime;
  @override
  final int sessionId;
  int _authorityRevision = 0;
  bool _closed = false;

  @override
  int get authorityRevision => _authorityRevision;

  @override
  Stream<EngineSearchEvent> searchComputerMove({
    required String fen,
    required int positionRevision,
    EngineTimeControl? timeControl,
    EngineSearchLimits? limits,
  }) {
    if (_closed) throw StateError('Engine search session is closed.');
    return _runtime._startSearch(
      this,
      fen: fen,
      positionRevision: positionRevision,
      purpose: EngineSearchPurpose.computerMove,
      timeControl: timeControl,
      limits:
          limits ??
          EngineSearchLimits(moveTime: const Duration(milliseconds: 750)),
    );
  }

  Stream<EngineSearchEvent> searchPostGameAnalysis({
    required String fen,
    required int positionRevision,
    EngineSearchLimits? limits,
  }) {
    if (_closed) throw StateError('Engine search session is closed.');
    return _runtime._startSearch(
      this,
      fen: fen,
      positionRevision: positionRevision,
      purpose: EngineSearchPurpose.postGameAnalysis,
      limits:
          limits ??
          EngineSearchLimits(
            moveTime: const Duration(milliseconds: 250),
            multiPv: 2,
          ),
    );
  }

  /// Revokes the current identity before requesting asynchronous cancellation.
  @override
  void retireAuthority() {
    if (_closed) return;
    _runtime._retireAuthority(this);
  }

  @override
  void cancelComputerMove() => retireAuthority();

  @override
  void close() {
    if (_closed) return;
    _closed = true;
    _runtime._closeSession(this);
  }

  int _advanceAuthority() {
    if (_authorityRevision >= _maximumAuthorityRevision) {
      throw StateError('Engine authority revisions are exhausted.');
    }
    return ++_authorityRevision;
  }

  void _invalidateFromRuntime(int authorityRevision) {
    if (!_closed && _authorityRevision == authorityRevision) {
      _advanceAuthority();
    }
  }

  bool _accepts(
    int authorityRevision,
    EngineSearchRequest request,
    EngineSearchEvent event,
  ) =>
      !_closed &&
      _authorityRevision == authorityRevision &&
      event.jobId == request.jobId &&
      event.sessionId == sessionId &&
      event.positionRevision == request.positionRevision;

  void _closeFromRuntime() {
    if (_closed) return;
    _closed = true;
    if (_authorityRevision < _maximumAuthorityRevision) {
      _authorityRevision++;
    }
  }
}

class EngineRuntimeScope extends InheritedWidget {
  const EngineRuntimeScope({
    super.key,
    required this.runtime,
    required super.child,
  });

  final EngineRuntime runtime;

  static EngineRuntime of(BuildContext context) {
    final EngineRuntime? runtime = maybeOf(context);
    if (runtime == null) {
      throw FlutterError('No EngineRuntimeScope found in this context.');
    }
    return runtime;
  }

  static EngineRuntime? maybeOf(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<EngineRuntimeScope>()?.runtime;

  @override
  bool updateShouldNotify(EngineRuntimeScope oldWidget) =>
      !identical(runtime, oldWidget.runtime);
}

class _ActiveSearch {
  const _ActiveSearch(this.session, this.authorityRevision, this.purpose);

  final EngineSearchSession session;
  final int authorityRevision;
  final EngineSearchPurpose purpose;
}

Future<void> _waitForShutdown(Future<void> shutdown, Duration timeout) =>
    shutdown.timeout(timeout);
