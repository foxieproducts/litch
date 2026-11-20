import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/engine/chess_engine_models.dart';
import 'package:litch_mobile/src/engine/chess_engine_provider.dart';
import 'package:litch_mobile/src/engine/chess_engine_router.dart';
import 'package:litch_mobile/src/engine/engine_runtime.dart';

const String _initialFen =
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

void main() {
  test(
    'unavailable runtime fails softly with the requested identity',
    () async {
      final EngineRuntime runtime = EngineRuntime();
      final EngineSearchSession first = runtime.openSession();
      final EngineSearchSession second = runtime.openSession();

      expect(runtime.available, isFalse);
      expect(first.sessionId, 1);
      expect(second.sessionId, 2);
      final List<EngineSearchEvent> events = await first
          .searchComputerMove(fen: _initialFen, positionRevision: 12)
          .toList();

      final EngineSearchFailed failure = events.single as EngineSearchFailed;
      expect(failure.reason, EngineFailureReason.unavailable);
      expect(failure.sessionId, first.sessionId);
      expect(failure.positionRevision, 12);
      expect(failure.jobId, isNotEmpty);
      await runtime.close();
      expect(runtime.closed, isTrue);
      expect(runtime.shutdownWasClean, isTrue);
    },
  );

  test('open initializes once and cleans up a rejected provider', () async {
    final _FakeProvider provider = _FakeProvider();
    var opens = 0;
    final EngineRuntime runtime = await EngineRuntime.open(
      providerFactory: () {
        opens++;
        return provider;
      },
      routerFactory: (_) => throw StateError('adapter unavailable'),
      shutdownWaiter: (Future<void> shutdown, Duration _) => shutdown,
    );

    expect(opens, 1);
    expect(provider.closeCount, 1);
    expect(runtime.available, isFalse);
    await runtime.close();
  });

  test(
    'routes one computer move with caller limits through the real router',
    () async {
      final _FakeProvider provider = _FakeProvider();
      final EngineRuntime runtime = EngineRuntime(provider: provider);
      final EngineSearchSession session = runtime.openSession();
      final Future<List<EngineSearchEvent>> done = session
          .searchComputerMove(
            fen: _initialFen,
            positionRevision: 4,
            limits: EngineSearchLimits(depth: 8),
          )
          .toList();
      await pumpEventQueue();
      final _ProviderSearch search = provider.searches.single;

      expect(search.request.sessionId, session.sessionId);
      expect(search.request.positionRevision, 4);
      expect(search.request.purpose, EngineSearchPurpose.computerMove);
      expect(search.request.limits.depth, 8);
      expect(session.authorityRevision, 1);
      search.controller
        ..add(_started(search.request))
        ..add(_bestMove(search.request))
        ..add(_completed(search.request));

      expect((await done).map((EngineSearchEvent event) => event.runtimeType), [
        EngineSearchStarted,
        EngineBestMove,
        EngineSearchCompleted,
      ]);
      await runtime.close();
      expect(provider.closeCount, 1);
    },
  );

  test(
    'routes sequential post-game searches through the shared identity',
    () async {
      final _FakeProvider provider = _FakeProvider();
      final EngineRuntime runtime = EngineRuntime(provider: provider);
      final EngineSearchSession session = runtime.openSession();
      final Future<List<EngineSearchEvent>> done = session
          .searchPostGameAnalysis(
            fen: _initialFen,
            positionRevision: 0,
            limits: EngineSearchLimits(depth: 8, multiPv: 2),
          )
          .toList();
      await pumpEventQueue();
      final _ProviderSearch search = provider.searches.single;

      expect(runtime.supportsClassicPostGameAnalysis, isTrue);
      expect(search.request.purpose, EngineSearchPurpose.postGameAnalysis);
      expect(search.request.limits.multiPv, 2);
      search.controller
        ..add(_started(search.request))
        ..add(_bestMove(search.request))
        ..add(_completed(search.request));

      expect((await done).last, isA<EngineSearchCompleted>());
      await runtime.close();
    },
  );

  test('background review cannot revoke an active computer move', () async {
    final _FakeProvider provider = _FakeProvider();
    final EngineRuntime runtime = EngineRuntime(provider: provider);
    final EngineSearchSession play = runtime.openSession();
    final EngineSearchSession review = runtime.openSession();
    final Future<List<EngineSearchEvent>> playDone = play
        .searchComputerMove(fen: _initialFen, positionRevision: 1)
        .toList();
    await pumpEventQueue();

    final List<EngineSearchEvent> reviewEvents = await review
        .searchPostGameAnalysis(fen: _initialFen, positionRevision: 1)
        .toList();
    expect(
      (reviewEvents.single as EngineSearchFailed).reason,
      EngineFailureReason.busy,
    );
    expect(provider.searches, hasLength(1));

    final _ProviderSearch search = provider.searches.single;
    search.controller
      ..add(_started(search.request))
      ..add(_bestMove(search.request))
      ..add(_completed(search.request));
    expect((await playDone).last, isA<EngineSearchCompleted>());
    await runtime.close();
  });

  test(
    'retirement fences late output before requesting cancellation',
    () async {
      final _FakeProvider provider = _FakeProvider();
      late final _ControlledRouter router;
      final EngineRuntime runtime = EngineRuntime(
        provider: provider,
        routerFactory: (ChessEngineProvider provider) =>
            router = _ControlledRouter(provider),
      );
      final EngineSearchSession session = runtime.openSession();
      final Future<List<EngineSearchEvent>> done = session
          .searchComputerMove(fen: _initialFen, positionRevision: 2)
          .toList();
      await pumpEventQueue();
      final EngineSearchRequest request = router.requests.single;
      router.controller.add(_started(request));
      await pumpEventQueue();
      expect(session.authorityRevision, 1);

      session.retireAuthority();
      expect(session.authorityRevision, 2);
      expect(router.cancelCount, 1);
      router.controller
        ..add(_bestMove(request))
        ..add(_completed(request));
      await router.controller.close();

      expect((await done).map((EngineSearchEvent event) => event.runtimeType), [
        EngineSearchStarted,
      ]);
      await runtime.close();
    },
  );

  test(
    'replacement fences the old session and gives every job a unique id',
    () async {
      final _FakeProvider provider = _FakeProvider();
      late final _ControlledRouter router;
      final EngineRuntime runtime = EngineRuntime(
        provider: provider,
        routerFactory: (ChessEngineProvider provider) =>
            router = _ControlledRouter(provider),
      );
      final EngineSearchSession first = runtime.openSession();
      final EngineSearchSession second = runtime.openSession();
      final Future<List<EngineSearchEvent>> firstDone = first
          .searchComputerMove(fen: _initialFen, positionRevision: 1)
          .toList();
      await pumpEventQueue();
      final EngineSearchRequest oldRequest = router.requests.single;

      final Future<List<EngineSearchEvent>> secondDone = second
          .searchComputerMove(fen: _initialFen, positionRevision: 1)
          .toList();
      await pumpEventQueue();
      final EngineSearchRequest newRequest = router.requests.last;
      expect(first.authorityRevision, 2);
      expect(second.authorityRevision, 1);
      expect(newRequest.jobId, isNot(oldRequest.jobId));
      final int replacementCancellations = router.cancelCount;
      first.cancelComputerMove();
      expect(
        router.cancelCount,
        replacementCancellations,
        reason: 'an old session must not cancel the current session search',
      );

      router.controllers[0]
        ..add(_started(oldRequest))
        ..add(_bestMove(oldRequest))
        ..add(_completed(oldRequest));
      await router.controllers[0].close();
      router.controllers[1]
        ..add(_started(newRequest))
        ..add(_bestMove(newRequest))
        ..add(_completed(newRequest));
      await router.controllers[1].close();

      expect(await firstDone, isEmpty);
      expect((await secondDone).last, isA<EngineSearchCompleted>());
      await runtime.close();
    },
  );

  test('same-position retry gets a new job after provider failure', () async {
    final _FakeProvider provider = _FakeProvider();
    final EngineRuntime runtime = EngineRuntime(provider: provider);
    final EngineSearchSession session = runtime.openSession();
    final Future<List<EngineSearchEvent>> firstDone = session
        .searchComputerMove(fen: _initialFen, positionRevision: 3)
        .toList();
    await pumpEventQueue();
    final _ProviderSearch first = provider.searches.single;
    first.controller
      ..add(_started(first.request))
      ..add(
        EngineSearchFailed(
          jobId: first.request.jobId,
          sessionId: first.request.sessionId,
          positionRevision: first.request.positionRevision,
          reason: EngineFailureReason.transport,
        ),
      );
    expect(
      (await firstDone).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.transport,
    );

    final Future<List<EngineSearchEvent>> retryDone = session
        .searchComputerMove(fen: _initialFen, positionRevision: 3)
        .toList();
    await pumpEventQueue();
    expect(provider.searches, hasLength(2));
    final _ProviderSearch retry = provider.searches.last;
    expect(retry.request.jobId, isNot(first.request.jobId));
    expect(retry.request.fen, first.request.fen);
    expect(retry.request.positionRevision, first.request.positionRevision);
    retry.controller
      ..add(_started(retry.request))
      ..add(_bestMove(retry.request))
      ..add(_completed(retry.request));
    expect((await retryDone).last, isA<EngineSearchCompleted>());
    await runtime.close();
  });

  test('close is idempotent and records a bounded shutdown failure', () async {
    final _FakeProvider provider = _FakeProvider();
    late final _ControlledRouter router;
    Duration? observedTimeout;
    var waits = 0;
    final EngineRuntime runtime = EngineRuntime(
      provider: provider,
      routerFactory: (ChessEngineProvider provider) =>
          router = _ControlledRouter(provider, hangOnClose: true),
      shutdownTimeout: const Duration(seconds: 2),
      shutdownWaiter: (Future<void> _, Duration timeout) async {
        waits++;
        observedTimeout = timeout;
        throw TimeoutException('controlled timeout');
      },
    );
    final EngineSearchSession session = runtime.openSession();

    final Future<void> firstClose = runtime.close();
    final Future<void> secondClose = runtime.close();
    expect(identical(firstClose, secondClose), isTrue);
    await firstClose;

    expect(session.authorityRevision, 1);
    expect(runtime.available, isFalse);
    expect(runtime.closed, isTrue);
    expect(runtime.shutdownWasClean, isFalse);
    expect(observedTimeout, const Duration(seconds: 2));
    expect(waits, 1);
    expect(router.closeCount, 1);
  });

  testWidgets('scope exposes only its injected runtime', (
    WidgetTester tester,
  ) async {
    final EngineRuntime runtime = EngineRuntime();
    EngineRuntime? observed;
    await tester.pumpWidget(
      EngineRuntimeScope(
        runtime: runtime,
        child: Builder(
          builder: (BuildContext context) {
            observed = EngineRuntimeScope.of(context);
            return const SizedBox();
          },
        ),
      ),
    );

    expect(observed, same(runtime));
    await runtime.close();
  });
}

EngineSearchStarted _started(EngineSearchRequest request) =>
    EngineSearchStarted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );

EngineBestMove _bestMove(EngineSearchRequest request) => EngineBestMove(
  jobId: request.jobId,
  sessionId: request.sessionId,
  positionRevision: request.positionRevision,
  uci: 'e2e4',
);

EngineSearchCompleted _completed(EngineSearchRequest request) =>
    EngineSearchCompleted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );

class _FakeProvider implements ChessEngineProvider {
  final List<_ProviderSearch> searches = <_ProviderSearch>[];
  int closeCount = 0;

  @override
  EngineCapabilities get capabilities => EngineCapabilities(
    providerId: 'fake-local',
    engineVersion: '1',
    privacyClass: EnginePrivacyClass.onDevice,
    variants: <ChessEngineVariant>{ChessEngineVariant.classic},
    purposes: <EngineSearchPurpose>{
      EngineSearchPurpose.computerMove,
      EngineSearchPurpose.postGameAnalysis,
    },
    supportsDepth: true,
    maximumMultiPv: 2,
  );

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) {
    final _ProviderSearch search = _ProviderSearch(request, cancellation);
    searches.add(search);
    return search.controller.stream;
  }

  @override
  Future<void> close() async {
    closeCount++;
  }
}

class _ProviderSearch {
  _ProviderSearch(this.request, this.cancellation);

  final EngineSearchRequest request;
  final EngineCancellation cancellation;
  final StreamController<EngineSearchEvent> controller =
      StreamController<EngineSearchEvent>();
}

class _ControlledRouter extends ChessEngineRouter {
  _ControlledRouter(super.provider, {this.hangOnClose = false});

  final bool hangOnClose;
  final List<EngineSearchRequest> requests = <EngineSearchRequest>[];
  final List<StreamController<EngineSearchEvent>> controllers =
      <StreamController<EngineSearchEvent>>[];
  int cancelCount = 0;
  final List<EngineSearchPurpose> cancelledPurposes = <EngineSearchPurpose>[];
  int closeCount = 0;

  StreamController<EngineSearchEvent> get controller => controllers.single;

  @override
  Stream<EngineSearchEvent> search(EngineSearchRequest request) {
    requests.add(request);
    final StreamController<EngineSearchEvent> controller =
        StreamController<EngineSearchEvent>();
    controllers.add(controller);
    return controller.stream;
  }

  @override
  void cancel(EngineSearchPurpose purpose) {
    cancelCount++;
    cancelledPurposes.add(purpose);
  }

  @override
  Future<void> close() {
    closeCount++;
    return hangOnClose ? Completer<void>().future : Future<void>.value();
  }
}
