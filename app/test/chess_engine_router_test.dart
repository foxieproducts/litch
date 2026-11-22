import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/engine/chess_engine_models.dart';
import 'package:litch_mobile/src/engine/chess_engine_provider.dart';
import 'package:litch_mobile/src/engine/chess_engine_router.dart';

void main() {
  test('buffers a legal best move until successful completion', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final events = <EngineSearchEvent>[];
    final done = router.search(_request()).listen(events.add).asFuture<void>();
    await pumpEventQueue();
    final source = provider.searches.single;

    source.controller
      ..add(_started())
      ..add(_best());
    await pumpEventQueue();
    expect(events.map((event) => event.runtimeType), [EngineSearchStarted]);

    source.controller.add(_completed());
    await done;
    expect(events.map((event) => event.runtimeType), [
      EngineSearchStarted,
      EngineBestMove,
      EngineSearchCompleted,
    ]);
    await router.close();
    expect(provider.closed, isTrue);
  });

  test('accepts no-move completion only for a terminal position', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final terminalDone = router
        .search(_request(fen: '7k/6Q1/6K1/8/8/8/8/8 b - - 0 1'))
        .toList();
    await pumpEventQueue();
    provider.searches.single.controller
      ..add(_started())
      ..add(_terminalInfo(1))
      ..add(_terminalInfo(2))
      ..add(
        const EngineNoLegalMove(
          jobId: 'job',
          sessionId: 7,
          positionRevision: 11,
        ),
      )
      ..add(_completed());

    final terminal = await terminalDone;
    expect(terminal.map((event) => event.runtimeType), <Type>[
      EngineSearchStarted,
      EngineSearchInfo,
      EngineSearchInfo,
      EngineNoLegalMove,
      EngineSearchCompleted,
    ]);
    expect(terminal.whereType<EngineNoLegalMove>(), hasLength(1));
    expect(terminal.last, isA<EngineSearchCompleted>());

    final invalidDone = router.search(_request()).toList();
    await pumpEventQueue();
    provider.searches.last.controller
      ..add(_started())
      ..add(
        const EngineNoLegalMove(
          jobId: 'job',
          sessionId: 7,
          positionRevision: 11,
        ),
      );
    expect(
      (await invalidDone).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.invalidResponse,
    );

    final failedDone = router
        .search(
          _request(jobId: 'failed', fen: '7k/6Q1/6K1/8/8/8/8/8 b - - 0 1'),
        )
        .toList();
    await pumpEventQueue();
    provider.searches.last.controller
      ..add(_started(jobId: 'failed'))
      ..add(
        const EngineNoLegalMove(
          jobId: 'failed',
          sessionId: 7,
          positionRevision: 11,
        ),
      )
      ..add(
        EngineSearchFailed(
          jobId: 'failed',
          sessionId: 7,
          positionRevision: 11,
          reason: EngineFailureReason.transport,
        ),
      );
    final failed = await failedDone;
    expect(failed.whereType<EngineNoLegalMove>(), isEmpty);
    expect(failed.last, isA<EngineSearchFailed>());
    await router.close();
  });

  test('rejects a syntactically valid but illegal best move', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final eventsFuture = router.search(_request()).toList();
    await pumpEventQueue();
    provider.searches.single.controller
      ..add(_started())
      ..add(_best(uci: 'e2e5'));

    final events = await eventsFuture;
    expect(
      (events.last as EngineSearchFailed).reason,
      EngineFailureReason.invalidResponse,
    );
    await router.close();
  });

  test('replacement revokes old authority and waits for cleanup', () async {
    final cleanup = Completer<void>();
    final provider = _FakeProvider(cleanup: cleanup);
    final router = ChessEngineRouter(provider);
    final firstDone = router.search(_request(jobId: 'first')).toList();
    await pumpEventQueue();
    final first = provider.searches.single;
    first.controller.add(_started(jobId: 'first'));

    final secondDone = router.search(_request(jobId: 'second')).toList();
    expect(first.cancellation.isCancelled, isTrue);
    await pumpEventQueue();
    expect(provider.searches, hasLength(1));

    cleanup.complete();
    await pumpEventQueue();
    expect(provider.searches, hasLength(2));
    provider.searches.last.controller
      ..add(_started(jobId: 'second'))
      ..add(_best(jobId: 'second'))
      ..add(_completed(jobId: 'second'));
    expect(
      (await firstDone).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.cancelled,
    );
    expect((await secondDone).last, isA<EngineSearchCompleted>());
    await router.close();
  });

  test(
    'provider failure releases cleanup before a same-position retry',
    () async {
      final provider = _FakeProvider();
      final router = ChessEngineRouter(provider);
      final firstDone = router.search(_request(jobId: 'failed')).toList();
      await pumpEventQueue();
      provider.searches.single.controller
        ..add(_started(jobId: 'failed'))
        ..add(
          EngineSearchFailed(
            jobId: 'failed',
            sessionId: 7,
            positionRevision: 11,
            reason: EngineFailureReason.transport,
          ),
        );
      expect(
        (await firstDone).whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.transport,
      );

      final secondDone = router.search(_request(jobId: 'retry')).toList();
      await pumpEventQueue();
      expect(provider.searches, hasLength(2));
      provider.searches.last.controller
        ..add(_started(jobId: 'retry'))
        ..add(_best(jobId: 'retry'))
        ..add(_completed(jobId: 'retry'));
      expect((await secondDone).last, isA<EngineSearchCompleted>());
      await router.close();
    },
  );

  test('play preempts analysis while analysis cannot preempt play', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final analysisDone = router.search(_request(jobId: 'analysis')).toList();
    await pumpEventQueue();
    final playDone = router
        .search(
          _request(jobId: 'play', purpose: EngineSearchPurpose.computerMove),
        )
        .toList();
    await pumpEventQueue();
    expect(
      (await analysisDone).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.cancelled,
    );

    final denied = await router
        .search(_request(jobId: 'second-analysis'))
        .toList();
    expect(
      (denied.single as EngineSearchFailed).reason,
      EngineFailureReason.busy,
    );
    expect(provider.searches, hasLength(2));

    router.cancel(EngineSearchPurpose.computerMove);
    await playDone;
    await router.close();
  });

  test(
    'drops stale identities and caps visible progress at four hertz',
    () async {
      final provider = _FakeProvider();
      var nowMicros = 0;
      final router = ChessEngineRouter(
        provider,
        monotonicMicros: () => nowMicros,
      );
      final eventsFuture = router.search(_request()).toList();
      await pumpEventQueue();
      final source = provider.searches.single;
      source.controller.add(_started(jobId: 'stale'));
      source.controller.add(_started());
      for (var depth = 1; depth <= 6; depth++) {
        source.controller.add(_info(depth));
        nowMicros += 100000;
      }
      source.controller
        ..add(_best())
        ..add(_completed());

      final events = await eventsFuture;
      expect(events.whereType<EngineSearchInfo>().map((info) => info.depth), [
        1,
        4,
        6,
      ]);
      await router.close();
    },
  );

  test('deadline ends a provider that never completes', () async {
    final provider = _FakeProvider();
    final deadlines = _FakeDeadlineFactory();
    final router = ChessEngineRouter(
      provider,
      deadlineFactory: deadlines.create,
    );
    final eventsFuture = router.search(_request()).toList();
    await pumpEventQueue();
    final source = provider.searches.single;
    source.controller.add(_started());

    deadlines.handles.single.fire();

    final events = await eventsFuture;
    expect(
      (events.last as EngineSearchFailed).reason,
      EngineFailureReason.timedOut,
    );
    expect(source.cancellation.isCancelled, isTrue);
    expect(
      deadlines.delays.first,
      const Duration(milliseconds: 500),
      reason: '250 ms compute time plus bounded adapter completion grace',
    );
    await router.close();
  });

  test(
    'synchronous cancel failure quarantines without stranding work',
    () async {
      final provider = _FakeProvider(throwSynchronouslyOnCancel: true);
      final router = ChessEngineRouter(provider);
      final first = router.search(_request(jobId: 'first')).toList();
      await pumpEventQueue();
      final second = router.search(_request(jobId: 'second')).toList();
      await pumpEventQueue();

      expect(
        (await first).whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.cancelled,
      );
      expect(
        (await second).whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.unavailable,
      );
      expect(provider.searches, hasLength(1));
      await expectLater(router.close(), completes);
    },
  );

  test(
    'a paused result consumer cannot strand the global engine lane',
    () async {
      final provider = _FakeProvider();
      final router = ChessEngineRouter(provider);
      final firstSubscription = router
          .search(_request(jobId: 'first'))
          .listen((_) {});
      await pumpEventQueue();
      firstSubscription.pause();

      final secondEvents = router.search(_request(jobId: 'second')).toList();
      await pumpEventQueue();
      expect(provider.searches, hasLength(2));
      provider.searches.last.controller
        ..add(_started(jobId: 'second'))
        ..add(_best(jobId: 'second'))
        ..add(_completed(jobId: 'second'));
      expect((await secondEvents).last, isA<EngineSearchCompleted>());
      await expectLater(router.close(), completes);

      firstSubscription.resume();
      await firstSubscription.cancel();
    },
  );

  test('asynchronous cancel failure quarantines the provider', () async {
    final cleanup = Completer<void>();
    final provider = _FakeProvider(cleanup: cleanup);
    final router = ChessEngineRouter(provider);
    final first = router.search(_request(jobId: 'first')).toList();
    await pumpEventQueue();
    final second = router.search(_request(jobId: 'second')).toList();
    cleanup.completeError(StateError('asynchronous cancel failure'));
    await pumpEventQueue();

    expect(
      (await first).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.cancelled,
    );
    expect(
      (await second).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.unavailable,
    );
    expect(provider.searches, hasLength(1));
    await expectLater(router.close(), completes);
  });

  test('a terminal-event listener can start the next search safely', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final firstEvents = <EngineSearchEvent>[];
    late Future<List<EngineSearchEvent>> secondEvents;
    final firstDone = Completer<void>();
    router.search(_request(jobId: 'first')).listen((event) {
      firstEvents.add(event);
      if (event is EngineSearchCompleted) {
        secondEvents = router.search(_request(jobId: 'second')).toList();
        firstDone.complete();
      }
    });
    await pumpEventQueue();
    provider.searches.single.controller
      ..add(_started(jobId: 'first'))
      ..add(_best(jobId: 'first'))
      ..add(_completed(jobId: 'first'));
    await firstDone.future;
    await pumpEventQueue();

    expect(provider.searches, hasLength(2));
    provider.searches.last.controller
      ..add(_started(jobId: 'second'))
      ..add(_best(jobId: 'second'))
      ..add(_completed(jobId: 'second'));
    expect((await secondEvents).last, isA<EngineSearchCompleted>());
    expect(firstEvents.whereType<EngineSearchCompleted>(), hasLength(1));
    expect(firstEvents.whereType<EngineSearchFailed>(), isEmpty);
    await router.close();
  });

  test('consumer cancellation revokes provider authority', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final subscription = router.search(_request()).listen((_) {});
    await pumpEventQueue();
    final running = provider.searches.single;

    await subscription.cancel();

    expect(running.cancellation.isCancelled, isTrue);
    await router.close();
  });

  test('stale fair-play lease release cannot reopen engine access', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final oldSession = router.prohibitForHumanOnlineSession('old');
    final currentSession = router.prohibitForHumanOnlineSession('current');

    oldSession.release();
    final denied = await router.search(_request()).toList();
    expect(
      (denied.single as EngineSearchFailed).reason,
      EngineFailureReason.prohibited,
    );
    expect(provider.searches, isEmpty);

    currentSession.release();
    final subscription = router
        .search(_request(jobId: 'allowed'))
        .listen((_) {});
    await pumpEventQueue();
    expect(provider.searches, hasLength(1));
    await subscription.cancel();
    await router.close();
  });

  test('remote provider requires an explicit authorization lease', () async {
    final provider = _FakeProvider(privacyClass: EnginePrivacyClass.remote);
    final router = ChessEngineRouter(provider);
    final denied = await router.search(_request()).toList();
    expect(
      (denied.single as EngineSearchFailed).reason,
      EngineFailureReason.prohibited,
    );

    final authorization = router.authorizeRemoteEngine();
    final subscription = router
        .search(_request(jobId: 'allowed'))
        .listen((_) {});
    await pumpEventQueue();
    expect(provider.searches, hasLength(1));
    await subscription.cancel();
    authorization.release();
    await router.close();
  });

  test(
    'revoking remote authorization retires active and queued work',
    () async {
      final cleanup = Completer<void>();
      final provider = _FakeProvider(
        cleanup: cleanup,
        privacyClass: EnginePrivacyClass.remote,
      );
      final router = ChessEngineRouter(provider);
      final authorization = router.authorizeRemoteEngine();
      final active = router.search(_request(jobId: 'active')).toList();
      await pumpEventQueue();
      final queued = router.search(_request(jobId: 'queued')).toList();

      authorization.release();
      cleanup.complete();
      await pumpEventQueue();

      expect(
        (await active).whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.cancelled,
      );
      expect(
        (await queued).whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.prohibited,
      );
      expect(provider.searches, hasLength(1));
      await router.close();
    },
  );

  test('hung cleanup is bounded and quarantines the provider', () async {
    final cleanup = Completer<void>();
    final provider = _FakeProvider(cleanup: cleanup);
    final deadlines = _FakeDeadlineFactory();
    final router = ChessEngineRouter(
      provider,
      deadlineFactory: deadlines.create,
    );
    final first = router.search(_request(jobId: 'first')).toList();
    await pumpEventQueue();
    final second = router.search(_request(jobId: 'second')).toList();
    await pumpEventQueue();

    deadlines.handles.where((handle) => !handle.cancelled).single.fire();
    await pumpEventQueue();

    expect(
      (await first).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.cancelled,
    );
    expect(
      (await second).whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.unavailable,
    );
    expect(provider.searches, hasLength(1));
    await router.close();
  });

  test('illegal ponder and negative secondary limits are rejected', () async {
    expect(
      () => EngineSearchLimits(
        moveTime: const Duration(milliseconds: 10),
        nodes: -1,
      ),
      throwsArgumentError,
    );
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final eventsFuture = router.search(_request()).toList();
    await pumpEventQueue();
    provider.searches.single.controller
      ..add(_started())
      ..add(_best(ponder: 'e7e4'));

    final events = await eventsFuture;
    expect(
      (events.last as EngineSearchFailed).reason,
      EngineFailureReason.invalidResponse,
    );
    await router.close();
  });

  test('unsupported variant fails without calling the provider', () async {
    final provider = _FakeProvider();
    final router = ChessEngineRouter(provider);
    final events = await router
        .search(_request(variant: ChessEngineVariant.duck))
        .toList();

    expect(provider.searches, isEmpty);
    expect(
      (events.single as EngineSearchFailed).reason,
      EngineFailureReason.unsupported,
    );
    await router.close();
  });
}

EngineSearchRequest _request({
  String jobId = 'job',
  ChessEngineVariant variant = ChessEngineVariant.classic,
  EngineSearchPurpose purpose = EngineSearchPurpose.postGameAnalysis,
  String fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
}) => EngineSearchRequest(
  jobId: jobId,
  sessionId: 7,
  positionRevision: 11,
  fen: fen,
  variant: variant,
  purpose: purpose,
  limits: EngineSearchLimits(moveTime: const Duration(milliseconds: 250)),
);

EngineSearchStarted _started({String jobId = 'job'}) =>
    EngineSearchStarted(jobId: jobId, sessionId: 7, positionRevision: 11);

EngineSearchInfo _info(int depth) => EngineSearchInfo(
  jobId: 'job',
  sessionId: 7,
  positionRevision: 11,
  depth: depth,
  nodes: depth * 10,
  elapsed: Duration(milliseconds: depth * 10),
  score: EngineScore.centipawns(12),
  principalVariation: const <String>['e2e4'],
);

EngineSearchInfo _terminalInfo(int depth) => EngineSearchInfo(
  jobId: 'job',
  sessionId: 7,
  positionRevision: 11,
  depth: depth,
  nodes: depth * 10,
  elapsed: Duration(milliseconds: depth * 10),
  score: EngineScore.mate(0),
  principalVariation: const <String>[],
);

EngineBestMove _best({
  String jobId = 'job',
  String uci = 'e2e4',
  String? ponder,
}) => EngineBestMove(
  jobId: jobId,
  sessionId: 7,
  positionRevision: 11,
  uci: uci,
  ponder: ponder,
);

EngineSearchCompleted _completed({String jobId = 'job'}) =>
    EngineSearchCompleted(jobId: jobId, sessionId: 7, positionRevision: 11);

class _FakeSearch {
  _FakeSearch(
    this.request,
    this.cancellation,
    Future<void> Function()? onCancel,
  ) : controller = StreamController<EngineSearchEvent>(
        sync: true,
        onCancel: onCancel,
      );

  final EngineSearchRequest request;
  final EngineCancellation cancellation;
  final StreamController<EngineSearchEvent> controller;
}

class _FakeProvider implements ChessEngineProvider {
  _FakeProvider({
    this.cleanup,
    this.privacyClass = EnginePrivacyClass.onDevice,
    this.throwSynchronouslyOnCancel = false,
  });

  final Completer<void>? cleanup;
  final EnginePrivacyClass privacyClass;
  final bool throwSynchronouslyOnCancel;
  final searches = <_FakeSearch>[];
  bool closed = false;

  @override
  EngineCapabilities get capabilities => EngineCapabilities(
    providerId: 'fake',
    engineVersion: '1',
    privacyClass: privacyClass,
    variants: const <ChessEngineVariant>{ChessEngineVariant.classic},
    purposes: EngineSearchPurpose.values.toSet(),
  );

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) {
    final search = _FakeSearch(
      request,
      cancellation,
      cleanup == null ? null : () => cleanup!.future,
    );
    searches.add(search);
    final stream = search.controller.stream;
    return throwSynchronouslyOnCancel
        ? _ThrowingCancelStream<EngineSearchEvent>(stream)
        : stream;
  }

  @override
  Future<void> close() async {
    closed = true;
  }
}

class _ThrowingCancelStream<T> extends Stream<T> {
  _ThrowingCancelStream(this.source);

  final Stream<T> source;

  @override
  StreamSubscription<T> listen(
    void Function(T event)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => _ThrowingCancelSubscription<T>(
    source.listen(
      onData,
      onError: onError,
      onDone: onDone,
      cancelOnError: cancelOnError,
    ),
  );
}

class _ThrowingCancelSubscription<T> implements StreamSubscription<T> {
  _ThrowingCancelSubscription(this.source);

  final StreamSubscription<T> source;

  @override
  Future<void> cancel() => throw StateError('synchronous cancel failure');

  @override
  void onData(void Function(T data)? handleData) => source.onData(handleData);

  @override
  void onError(Function? handleError) => source.onError(handleError);

  @override
  void onDone(void Function()? handleDone) => source.onDone(handleDone);

  @override
  void pause([Future<void>? resumeSignal]) => source.pause(resumeSignal);

  @override
  void resume() => source.resume();

  @override
  bool get isPaused => source.isPaused;

  @override
  Future<E> asFuture<E>([E? futureValue]) => source.asFuture(futureValue);
}

class _FakeDeadlineFactory {
  final handles = <_FakeDeadline>[];
  final delays = <Duration>[];

  EngineDeadlineHandle create(Duration delay, void Function() callback) {
    delays.add(delay);
    final handle = _FakeDeadline(callback);
    handles.add(handle);
    return handle;
  }
}

class _FakeDeadline implements EngineDeadlineHandle {
  _FakeDeadline(this.callback);

  final void Function() callback;
  var cancelled = false;

  void fire() {
    if (!cancelled) callback();
  }

  @override
  void cancel() => cancelled = true;
}
