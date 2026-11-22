import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/online/lichess/lichess_board_api_client.dart';
import 'package:litch_mobile/src/online/lichess/lichess_board_api_events.dart';
import 'package:litch_mobile/src/online/lichess/lichess_request_coordinator.dart';

void main() {
  test('late cancelled seek handshake cannot retire its replacement', () async {
    final late = Completer<http.StreamedResponse>();
    final replacementBody = StreamController<List<int>>();
    final httpClient = _RecordingClient([
      (_) => late.future,
      (request) {
        unawaited(
          request.abortTrigger!.then((_) {
            unawaited(replacementBody.close());
          }),
        );
        return http.StreamedResponse(replacementBody.stream, 200);
      },
    ]);
    final client = _client(httpClient);
    final first = client
        .seek(LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0))
        .drain<void>();
    final failure = expectLater(first, throwsA(isA<LichessApiException>()));
    await httpClient.requestSeen.future;
    client.cancelSeek();
    await failure;
    final second = client
        .seek(LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0))
        .listen((_) {});
    await Future<void>.delayed(Duration.zero);
    var staleBodyCancelled = false;
    final staleBody = StreamController<List<int>>(
      onCancel: () {
        staleBodyCancelled = true;
        return Future<void>.error(
          StateError('synthetic late disposal failure'),
        );
      },
    );
    late.complete(http.StreamedResponse(staleBody.stream, 200));
    await Future<void>.delayed(Duration.zero);
    expect(staleBodyCancelled, isTrue);
    expect(replacementBody.isClosed, isFalse);
    client.close();
    await second.cancel();
    await staleBody.close();
  });

  test(
    'cancelSeek aborts a pending handshake without closing the client',
    () async {
      final httpClient = _AbortAwarePendingClient();
      final client = _client(httpClient);
      final seek = client
          .seek(
            LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0),
          )
          .drain<void>();
      final failure = expectLater(seek, throwsA(isA<LichessApiException>()));
      await httpClient.requestSeen.future;
      client.cancelSeek();
      await failure;
      await expectLater(httpClient.request!.abortTrigger, completes);
      client.close();
    },
  );

  test(
    'cancelSeek ends quiet matching and leaves account and replacement alive',
    () async {
      final bodies = <StreamController<List<int>>>[];
      http.StreamedResponse quiet(http.AbortableRequest request) {
        final body = StreamController<List<int>>();
        bodies.add(body);
        unawaited(
          request.abortTrigger!.then((_) {
            if (!body.isClosed) unawaited(body.close());
          }),
        );
        return http.StreamedResponse(body.stream, 200);
      }

      final httpClient = _RecordingClient([quiet, quiet, quiet]);
      final client = _client(httpClient);
      final account = client.streamAccountEvents().listen((_) {});
      await Future<void>.delayed(Duration.zero);
      final seek = client
          .seek(
            LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0),
          )
          .listen((_) {});
      await Future<void>.delayed(Duration.zero);
      client.cancelSeek();
      await seek.cancel();
      expect(bodies[0].isClosed, isFalse);
      expect(bodies[1].isClosed, isTrue);
      final replacement = client
          .seek(
            LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0),
          )
          .listen((_) {});
      await Future<void>.delayed(Duration.zero);
      expect(bodies[2].isClosed, isFalse);
      client.close();
      await replacement.cancel();
      await account.cancel();
    },
  );

  test('opens and decodes the account event stream', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _ndjsonResponse(
        '{"type":"gameStart","game":'
        '{"gameId":"BEOucQJo","fullId":"BEOucQJoabcd"}}\n',
      ),
    ]);
    final LichessBoardApiClient client = _client(httpClient);

    final LichessAccountGameEvent event =
        await client.streamAccountEvents().single as LichessAccountGameEvent;

    expect(event.type, 'gameStart');
    expect(event.gameId, 'BEOucQJo');
    expect(httpClient.requests.single.method, 'GET');
    expect(httpClient.requests.single.url.path, '/api/stream/event');
    expect(
      httpClient.requests.single.headers[HttpHeaders.authorizationHeader],
      'Bearer test-token',
    );
  });

  test(
    'reports account stream transport readiness before a silent body',
    () async {
      final StreamController<List<int>> body = StreamController<List<int>>();
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => http.StreamedResponse(body.stream, HttpStatus.ok),
      ]);
      final LichessBoardApiClient client = _client(httpClient);
      int opened = 0;
      final StreamSubscription<LichessAccountEvent> subscription = client
          .streamAccountEvents(onOpened: () => opened += 1)
          .listen((_) {});

      await httpClient.requestSeen.future;
      await Future<void>.microtask(() {});
      expect(opened, 1);
      expect(
        httpClient.requests.single.headers[HttpHeaders.authorizationHeader],
        'Bearer test-token',
      );
      await body.addStream(
        Stream<List<int>>.fromIterable(<List<int>>[
          utf8.encode('\n\r\n'),
          utf8.encode(
            '{"type":"gameStart","game":{"gameId":"BEOucQJo","fullId":"BEOucQJoabcd"}}\n',
          ),
        ]),
      );
      await body.close();
      await subscription.cancel();
      expect(opened, 1);
    },
  );

  test(
    'account readiness callback failure and closure release stream ownership',
    () async {
      final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
        cooldownStore: _FakeCooldownStore(),
      );
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _ndjsonResponse('\n'),
        (_) => _ndjsonResponse('\n'),
        (_) => _ndjsonResponse('\n'),
      ]);
      final LichessBoardApiClient throwing = _client(
        httpClient,
        coordinator: coordinator,
      );
      await expectLater(
        throwing
            .streamAccountEvents(
              onOpened: () => throw StateError('test callback'),
            )
            .drain<void>(),
        throwsStateError,
      );
      final LichessBoardApiClient closing = _client(
        httpClient,
        coordinator: coordinator,
      );
      await expectLater(
        closing.streamAccountEvents(onOpened: closing.close).drain<void>(),
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.disposed,
          ),
        ),
      );
      final LichessBoardApiClient replacement = _client(
        httpClient,
        coordinator: coordinator,
      );
      await replacement.streamAccountEvents().drain<void>();
      expect(httpClient.requests, hasLength(3));
    },
  );

  test(
    'account readiness is not reported for failed or prior-closed streams',
    () async {
      final List<LichessBoardApiClient> clients = <LichessBoardApiClient>[
        _client(
          _RecordingClient(<_ResponseFactory>[
            (_) => _jsonResponse(HttpStatus.unauthorized),
          ]),
        ),
        _client(
          _RecordingClient(<_ResponseFactory>[(_) => _ndjsonResponse('\n')]),
          requestDeadline: (_, _) async => null,
        ),
        _client(_RecordingClient(<_ResponseFactory>[])),
      ];
      clients.last.close();
      for (final LichessBoardApiClient client in clients) {
        bool opened = false;
        await expectLater(
          client
              .streamAccountEvents(onOpened: () => opened = true)
              .drain<void>(),
          throwsA(isA<LichessApiException>()),
        );
        expect(opened, isFalse);
      }
    },
  );

  test('closing before delayed 200 headers never reports readiness', () async {
    final _LateResponseClient httpClient = _LateResponseClient();
    final LichessBoardApiClient client = _client(httpClient);
    bool opened = false;
    final Future<void> stream = client
        .streamAccountEvents(onOpened: () => opened = true)
        .drain<void>();
    await httpClient.requestSeen.future;
    final Future<void> expectedFailure = expectLater(
      stream,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    client.close();
    httpClient.respond(_ndjsonResponse('\n'));

    await expectedFailure;
    expect(opened, isFalse);
    await expectLater(httpClient.request!.abortTrigger, completes);
  });

  test(
    'credential failure never reports readiness or starts a handshake',
    () async {
      final _RecordingClient httpClient = _RecordingClient(
        <_ResponseFactory>[],
      );
      final LichessBoardApiClient client = _client(
        httpClient,
        accessTokenProvider: () async => throw StateError('missing token'),
      );
      bool opened = false;

      await expectLater(
        client.streamAccountEvents(onOpened: () => opened = true).drain<void>(),
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.unavailableCredentials,
          ),
        ),
      );
      expect(opened, isFalse);
      expect(httpClient.requests, isEmpty);
    },
  );

  test(
    'a throwing readiness callback releases the same client stream lease',
    () async {
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _ndjsonResponse('\n'),
        (_) => _ndjsonResponse('\n'),
      ]);
      final LichessBoardApiClient client = _client(httpClient);

      await expectLater(
        client
            .streamAccountEvents(onOpened: () => throw StateError('callback'))
            .drain<void>(),
        throwsStateError,
      );
      await client.streamAccountEvents().drain<void>();
      expect(httpClient.requests, hasLength(2));
    },
  );

  test('reads the bounded unfiltered playing inventory', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(
          _playing(<Map<String, Object?>>[
            _playingGame(),
            _playingGame(
              gameId: 'other123',
              color: 'black',
              variant: 'threeCheck',
              speed: 'blitz',
              rated: true,
              isMyTurn: false,
              ai: 5,
            ),
          ], nbMyTurn: 1),
        ),
      ),
    ]);
    final LichessPlayingInventory inventory = await _client(
      httpClient,
    ).fetchPlayingInventory();

    expect(httpClient.requests.single.url.path, '/api/account/playing');
    expect(httpClient.requests.single.url.queryParameters, <String, String>{
      'nb': '50',
    });
    expect(inventory.games, hasLength(2));
    expect(inventory.games.last.variantKey, 'threeCheck');
    expect(inventory.games.last.opponent.aiLevel, 5);
    expect(inventory.isUncertain, isFalse);
  });

  test(
    'marks cap and turn-count disagreement uncertain without dropping games',
    () async {
      final List<Map<String, Object?>> games =
          List<Map<String, Object?>>.generate(
            50,
            (int index) => _playingGame(gameId: 'game$index'),
          );
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(_playing(games, nbMyTurn: 1)),
        ),
      ]);
      final LichessPlayingInventory inventory = await _client(
        httpClient,
      ).fetchPlayingInventory();

      expect(inventory.games, hasLength(50));
      expect(inventory.potentiallyCapped, isTrue);
      expect(inventory.turnCountUncertain, isTrue);
      expect(inventory.isUncertain, isTrue);
    },
  );

  test(
    'marks a full consistent inventory capped but not turn-uncertain',
    () async {
      final List<Map<String, Object?>> games =
          List<Map<String, Object?>>.generate(
            50,
            (int index) =>
                _playingGame(gameId: 'consistent$index', isMyTurn: false),
          );
      final LichessPlayingInventory inventory = await _client(
        _RecordingClient(<_ResponseFactory>[
          (_) => _jsonResponse(
            HttpStatus.ok,
            body: jsonEncode(_playing(games, nbMyTurn: 0)),
          ),
        ]),
      ).fetchPlayingInventory();

      expect(inventory.potentiallyCapped, isTrue);
      expect(inventory.turnCountUncertain, isFalse);
    },
  );

  test('retains the pinned count-disagreement example as uncertain', () async {
    final LichessPlayingInventory inventory = await _client(
      _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(
            _playing(<Map<String, Object?>>[
              _playingGame(gameId: 'rCRw1AuO', color: 'black', isMyTurn: false),
            ], nbMyTurn: 1),
          ),
        ),
      ]),
    ).fetchPlayingInventory();

    expect(inventory.potentiallyCapped, isFalse);
    expect(inventory.turnCountUncertain, isTrue);
    expect(inventory.games.single.gameId, 'rCRw1AuO');
  });

  test('rejects malformed consumed game fields and a 51st row', () async {
    final List<Map<String, Object?>> malformed = <Map<String, Object?>>[
      <String, Object?>{..._playingGame(), 'color': 'green'},
      <String, Object?>{..._playingGame(), 'variant': <String, Object?>{}},
      <String, Object?>{..._playingGame(), 'speed': 3},
      <String, Object?>{..._playingGame(), 'rated': 'false'},
      <String, Object?>{..._playingGame(), 'isMyTurn': 1},
      <String, Object?>{
        ..._playingGame(),
        'opponent': <String, Object?>{'id': 'x'},
      },
      <String, Object?>{
        ..._playingGame(),
        'opponent': <String, Object?>{'id': 'x', 'username': 'X', 'ai': 10},
      },
    ];
    for (final Map<String, Object?> game in malformed) {
      await expectLater(
        _client(
          _RecordingClient(<_ResponseFactory>[
            (_) => _jsonResponse(
              HttpStatus.ok,
              body: jsonEncode(
                _playing(<Map<String, Object?>>[game], nbMyTurn: 0),
              ),
            ),
          ]),
        ).fetchPlayingInventory(),
        throwsA(isA<LichessApiException>()),
      );
    }
    final List<Map<String, Object?>> tooMany =
        List<Map<String, Object?>>.generate(
          51,
          (int index) => _playingGame(gameId: 'overflow$index'),
        );
    await expectLater(
      _client(
        _RecordingClient(<_ResponseFactory>[
          (_) => _jsonResponse(
            HttpStatus.ok,
            body: jsonEncode(_playing(tooMany, nbMyTurn: 51)),
          ),
        ]),
      ).fetchPlayingInventory(),
      throwsA(isA<LichessApiException>()),
    );
  });

  test('returned current-game inventory cannot be mutated', () async {
    final LichessPlayingInventory inventory = await _client(
      _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(
            _playing(<Map<String, Object?>>[_playingGame()], nbMyTurn: 1),
          ),
        ),
      ]),
    ).fetchPlayingInventory();

    expect(
      () => inventory.games.add(inventory.games.single),
      throwsUnsupportedError,
    );
  });

  test(
    'rejects malformed, duplicate, oversized, and expired inventories',
    () async {
      final List<_ResponseFactory> responses = <_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.ok, body: '{"nowPlaying":[]}'),
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(
            _playing(<Map<String, Object?>>[
              _playingGame(),
              _playingGame(),
            ], nbMyTurn: 0),
          ),
        ),
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: 'x' * (LichessBoardApiClient.maximumPlayingInventoryBytes + 1),
        ),
        (_) => http.StreamedResponse(Stream<List<int>>.empty(), HttpStatus.ok),
      ];
      for (int index = 0; index < responses.length; index += 1) {
        final LichessBoardApiClient client = _client(
          _RecordingClient(<_ResponseFactory>[responses[index]]),
          responseBodyDeadline: index == 3 ? (_, _) async => null : null,
        );
        await expectLater(
          client.fetchPlayingInventory(),
          throwsA(
            isA<LichessApiException>().having(
              (LichessApiException error) => error.kind,
              'kind',
              index == 3
                  ? LichessApiFailureKind.transport
                  : LichessApiFailureKind.invalidResponse,
            ),
          ),
        );
      }
    },
  );

  test(
    'accepts a fragmented empty point-in-time inventory without extra certainty',
    () async {
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => http.StreamedResponse(
          Stream<List<int>>.fromIterable(<List<int>>[
            utf8.encode('{"nowPlaying"'),
            utf8.encode(':[],"nbMyTurn":0}'),
          ]),
          HttpStatus.ok,
        ),
      ]);
      final LichessPlayingInventory inventory = await _client(
        httpClient,
      ).fetchPlayingInventory();

      expect(inventory.games, isEmpty);
      expect(inventory.isUncertain, isFalse);
    },
  );

  test(
    'playing inventory completes its body before the next finite request',
    () async {
      final StreamController<List<int>> body = StreamController<List<int>>();
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => http.StreamedResponse(body.stream, HttpStatus.ok),
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(_playing(<Map<String, Object?>>[], nbMyTurn: 0)),
        ),
      ]);
      final LichessBoardApiClient client = _client(httpClient);
      final Future<LichessPlayingInventory> first = client
          .fetchPlayingInventory();
      await httpClient.requestSeen.future;
      final Future<LichessPlayingInventory> second = client
          .fetchPlayingInventory();
      await Future<void>.microtask(() {});
      expect(httpClient.requests, hasLength(1));
      body.add(
        utf8.encode(
          jsonEncode(_playing(<Map<String, Object?>>[], nbMyTurn: 0)),
        ),
      );
      await body.close();
      await first;
      await second;
      expect(httpClient.requests, hasLength(2));
    },
  );

  test('a 429 inventory response blocks the next stream start', () async {
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
      cooldownStore: _FakeCooldownStore(),
      monotonicNow: () => time.monotonicNow,
      wallNow: () => time.wallNow,
      delay: time.delay,
    );
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.tooManyRequests),
      (_) => _ndjsonResponse('\n'),
    ]);
    final LichessBoardApiClient client = _client(
      httpClient,
      coordinator: coordinator,
    );

    await expectLater(
      client.fetchPlayingInventory(),
      throwsA(isA<LichessApiException>()),
    );
    await client.streamAccountEvents().drain<void>();

    expect(time.delays, <Duration>[LichessRequestCoordinator.minimumCooldown]);
    expect(httpClient.requests, hasLength(2));
  });

  test('closing during an inventory body produces no late inventory', () async {
    final StreamController<List<int>> body = StreamController<List<int>>();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => http.StreamedResponse(body.stream, HttpStatus.ok),
    ]);
    final LichessBoardApiClient client = _client(httpClient);
    final Future<LichessPlayingInventory> request = client
        .fetchPlayingInventory();
    await httpClient.requestSeen.future;
    final Future<void> expectedFailure = expectLater(
      request,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    client.close();
    body.add(
      utf8.encode(jsonEncode(_playing(<Map<String, Object?>>[], nbMyTurn: 0))),
    );
    await body.close();

    await expectedFailure;
  });

  test('posts a standard casual Rapid real-time seek form', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _ndjsonResponse('\n\r\n'),
    ]);
    final LichessBoardApiClient client = _client(httpClient);

    await client
        .seek(LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0))
        .drain<void>();

    final http.AbortableRequest request = httpClient.requests.single;
    expect(request.method, 'POST');
    expect(request.url.path, '/api/board/seek');
    expect(request.bodyFields, <String, String>{
      'rated': 'false',
      'variant': 'standard',
      'time': '10',
      'increment': '0',
    });
  });

  test('opens a game stream and validates gameFull first', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _ndjsonResponse('${jsonEncode(_gameFull())}\n'),
    ]);
    final LichessBoardApiClient client = _client(httpClient);

    final LichessGameFullEvent event =
        await client.streamGame('BEOucQJo').single as LichessGameFullEvent;

    expect(event.gameId, 'BEOucQJo');
    expect(
      httpClient.requests.single.url.path,
      '/api/board/game/stream/BEOucQJo',
    );
  });

  test('a game stream closing before gameFull is a protocol failure', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _ndjsonResponse('\n'),
    ]);
    final LichessBoardApiClient client = _client(httpClient);

    await expectLater(
      client.streamGame('BEOucQJo').drain<void>(),
      throwsA(isA<Exception>()),
    );
  });

  test('move and action commands use the official paths', () async {
    final _RecordingClient httpClient = _RecordingClient(
      List<_ResponseFactory>.generate(
        7,
        (_) =>
            (_) => _jsonResponse(HttpStatus.ok),
      ),
    );
    final LichessBoardApiClient client = _client(httpClient);

    await client.makeMove('BEOucQJo', 'e7e8q', offeringDraw: true);
    await client.abort('BEOucQJo');
    await client.resign('BEOucQJo');
    await client.handleDraw('BEOucQJo', accept: false);
    await client.handleTakeback('BEOucQJo', accept: true);
    await client.claimVictory('BEOucQJo');
    await client.claimDraw('BEOucQJo');

    expect(
      httpClient.requests.map((http.BaseRequest request) => request.url.path),
      <String>[
        '/api/board/game/BEOucQJo/move/e7e8q',
        '/api/board/game/BEOucQJo/abort',
        '/api/board/game/BEOucQJo/resign',
        '/api/board/game/BEOucQJo/draw/no',
        '/api/board/game/BEOucQJo/takeback/yes',
        '/api/board/game/BEOucQJo/claim-victory',
        '/api/board/game/BEOucQJo/claim-draw',
      ],
    );
    expect(httpClient.requests.first.url.queryParameters, <String, String>{
      'offeringDraw': 'true',
    });
  });

  test('move dispatch guard is checked after awaited credentials', () async {
    final credentials = Completer<String>();
    final httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.ok),
    ]);
    final client = _client(
      httpClient,
      accessTokenProvider: () => credentials.future,
    );
    var admitted = true;
    final move = client.makeMove(
      'BEOucQJo',
      'e2e4',
      dispatchGuard: () => admitted,
    );
    admitted = false;
    credentials.complete('test-token');

    await expectLater(
      move,
      throwsA(
        isA<LichessApiException>().having(
          (error) => error.kind,
          'kind',
          LichessApiFailureKind.streamOwnership,
        ),
      ),
    );
    expect(httpClient.requests, isEmpty);
  });

  test('401 and 403 remain distinct and redact response bodies', () async {
    for (final ({int status, LichessApiFailureKind kind}) fixture
        in <({int status, LichessApiFailureKind kind})>[
          (
            status: HttpStatus.unauthorized,
            kind: LichessApiFailureKind.unauthorized,
          ),
          (status: HttpStatus.forbidden, kind: LichessApiFailureKind.forbidden),
        ]) {
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(fixture.status, body: 'secret-response-body'),
      ]);
      final LichessBoardApiClient client = _client(httpClient);

      await expectLater(
        client.abort('BEOucQJo'),
        throwsA(
          isA<LichessApiException>()
              .having(
                (LichessApiException error) => error.kind,
                'kind',
                fixture.kind,
              )
              .having(
                (LichessApiException error) => error.toString(),
                'message',
                isNot(contains('secret-response-body')),
              )
              .having(
                (LichessApiException error) => error.toString(),
                'token',
                isNot(contains('test-token')),
              ),
        ),
      );
    }
  });

  test('closing during credential lookup prevents a finite request', () async {
    final Completer<void> tokenRequested = Completer<void>();
    final Completer<String> token = Completer<String>();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.ok),
    ]);
    final LichessBoardApiClient client = _client(
      httpClient,
      accessTokenProvider: () {
        tokenRequested.complete();
        return token.future;
      },
    );

    final Future<void> command = client.abort('BEOucQJo');
    await tokenRequested.future;
    client.close();
    token.complete('test-token');

    await expectLater(
      command,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    expect(httpClient.requests, isEmpty);
  });

  test(
    'closing during unresolved credential lookup releases the shared coordinator',
    () async {
      final Completer<void> tokenRequested = Completer<void>();
      final Completer<String> token = Completer<String>();
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.ok),
      ]);
      final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
        cooldownStore: _FakeCooldownStore(),
      );
      final LichessBoardApiClient first = _client(
        httpClient,
        coordinator: coordinator,
        accessTokenProvider: () {
          tokenRequested.complete();
          return token.future;
        },
      );
      final LichessBoardApiClient replacement = _client(
        httpClient,
        coordinator: coordinator,
      );
      bool firstCompleted = false;
      Object? firstError;
      final Future<void> firstCommand = first.abort('BEOucQJo');
      unawaited(
        firstCommand.then<void>(
          (_) => firstCompleted = true,
          onError: (Object error, StackTrace _) {
            firstCompleted = true;
            firstError = error;
          },
        ),
      );

      await tokenRequested.future;
      first.close();
      await Future<void>.microtask(() {});

      expect(firstCompleted, isTrue);
      expect(
        firstError,
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      );
      expect(httpClient.requests, isEmpty);

      await replacement.abort('BEOucQJo');
      expect(httpClient.requests, hasLength(1));
      token.completeError(StateError('late credential failure'));
      await Future<void>.microtask(() {});
    },
  );

  test(
    'closing during credential lookup releases the shared coordinator for a stream',
    () async {
      final Completer<void> tokenRequested = Completer<void>();
      final Completer<String> token = Completer<String>();
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _ndjsonResponse('\n'),
      ]);
      final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
        cooldownStore: _FakeCooldownStore(),
      );
      final LichessBoardApiClient first = _client(
        httpClient,
        coordinator: coordinator,
        accessTokenProvider: () {
          tokenRequested.complete();
          return token.future;
        },
      );
      final LichessBoardApiClient replacement = _client(
        httpClient,
        coordinator: coordinator,
      );

      final Future<void> firstCommand = first.abort('BEOucQJo');
      await tokenRequested.future;
      first.close();

      await expectLater(
        firstCommand,
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.disposed,
          ),
        ),
      );
      expect(httpClient.requests, isEmpty);

      await replacement.streamAccountEvents().drain<void>();

      expect(httpClient.requests, hasLength(1));
      token.complete('test-token');
      await Future<void>.microtask(() {});
    },
  );

  test('closing during credential lookup prevents a stream request', () async {
    final Completer<void> tokenRequested = Completer<void>();
    final Completer<String> token = Completer<String>();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _ndjsonResponse('\n'),
    ]);
    final LichessBoardApiClient client = _client(
      httpClient,
      accessTokenProvider: () {
        tokenRequested.complete();
        return token.future;
      },
    );

    final Future<void> stream = client.streamAccountEvents().drain<void>();
    await tokenRequested.future;
    client.close();
    token.complete('test-token');

    await expectLater(
      stream,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    expect(httpClient.requests, isEmpty);
  });

  test('a response-body transport error is typed and redacted', () async {
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => http.StreamedResponse(
        Stream<List<int>>.error(StateError('secret transport detail')),
        HttpStatus.ok,
      ),
    ]);
    final LichessBoardApiClient client = _client(httpClient);

    await expectLater(
      client.streamAccountEvents().drain<void>(),
      throwsA(
        isA<LichessApiException>()
            .having(
              (LichessApiException error) => error.kind,
              'kind',
              LichessApiFailureKind.transport,
            )
            .having(
              (LichessApiException error) => error.toString(),
              'message',
              isNot(contains('secret transport detail')),
            ),
      ),
    );
  });

  for (final String requestKind in <String>['finite', 'stream']) {
    test('$requestKind request deadlines abort stalled handshakes', () async {
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.ok),
      ]);
      final LichessBoardApiClient client = _client(
        httpClient,
        requestDeadline: (_, _) async => null,
      );

      final Future<void> request = requestKind == 'finite'
          ? client.abort('BEOucQJo')
          : client.streamAccountEvents().drain<void>();

      await expectLater(
        request,
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.transport,
          ),
        ),
      );
      expect(httpClient.requests, hasLength(1));
      await expectLater(httpClient.requests.single.abortTrigger, completes);
    });
  }

  test('closing during a pending handshake aborts before headers', () async {
    final _AbortAwarePendingClient httpClient = _AbortAwarePendingClient();
    final LichessBoardApiClient client = _client(httpClient);

    final Future<void> stream = client.streamAccountEvents().drain<void>();
    await httpClient.requestSeen.future;
    client.close();

    await expectLater(
      stream,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    await expectLater(httpClient.request!.abortTrigger, completes);
  });

  for (final String streamKind in <String>['account', 'seek', 'game']) {
    test('429 during $streamKind handshake delays the next request', () async {
      final _ManualTime time = _ManualTime();
      final _FakeCooldownStore store = _FakeCooldownStore();
      final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
        cooldownStore: store,
        monotonicNow: () => time.monotonicNow,
        wallNow: () => time.wallNow,
        delay: time.delay,
      );
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(
          HttpStatus.tooManyRequests,
          headers: <String, String>{HttpHeaders.retryAfterHeader: '75'},
        ),
        (_) => _jsonResponse(HttpStatus.ok),
      ]);
      final LichessBoardApiClient client = _client(
        httpClient,
        coordinator: coordinator,
      );

      final Future<void> failedStream = switch (streamKind) {
        'account' => client.streamAccountEvents().drain<void>(),
        'seek' =>
          client
              .seek(
                LichessRealtimeSeek.casualRapid(
                  minutes: 10,
                  incrementSeconds: 0,
                ),
              )
              .drain<void>(),
        _ => client.streamGame('BEOucQJo').drain<void>(),
      };
      await expectLater(
        failedStream,
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.rateLimited,
          ),
        ),
      );

      await client.abort('BEOucQJo');

      expect(time.delays, <Duration>[const Duration(seconds: 75)]);
      expect(httpClient.requests, hasLength(2));
      expect(store.marker, isNotNull);
    });
  }

  test('429 during finite work delays the next request', () async {
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
      cooldownStore: _FakeCooldownStore(),
      monotonicNow: () => time.monotonicNow,
      wallNow: () => time.wallNow,
      delay: time.delay,
    );
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.tooManyRequests),
      (_) => _jsonResponse(HttpStatus.ok),
    ]);
    final LichessBoardApiClient client = _client(
      httpClient,
      coordinator: coordinator,
    );

    await expectLater(
      client.abort('BEOucQJo'),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.rateLimited,
        ),
      ),
    );
    await client.resign('BEOucQJo');

    expect(time.delays, <Duration>[LichessRequestCoordinator.minimumCooldown]);
    expect(httpClient.requests, hasLength(2));
  });

  test('only one account stream can own the client at a time', () async {
    final StreamController<List<int>> body = StreamController<List<int>>();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => http.StreamedResponse(body.stream, HttpStatus.ok),
    ]);
    final LichessBoardApiClient client = _client(httpClient);
    final StreamSubscription<LichessAccountEvent> first = client
        .streamAccountEvents()
        .listen((_) {});
    await httpClient.requestSeen.future;

    await expectLater(
      client.streamAccountEvents().drain<void>(),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.streamOwnership,
        ),
      ),
    );

    await body.close();
    await first.cancel();
  });

  test('closing the client triggers cancellation of an owned stream', () async {
    final StreamController<List<int>> body = StreamController<List<int>>();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => http.StreamedResponse(body.stream, HttpStatus.ok),
    ]);
    final LichessBoardApiClient client = _client(httpClient);
    final StreamSubscription<LichessAccountEvent> subscription = client
        .streamAccountEvents()
        .listen((_) {});
    await httpClient.requestSeen.future;
    final http.AbortableRequest request = httpClient.requests.single;
    bool aborted = false;
    request.abortTrigger!.then((_) => aborted = true);

    client.close();
    await Future<void>.delayed(Duration.zero);

    expect(aborted, isTrue);
    await body.close();
    await subscription.cancel();
  });
}

typedef _ResponseFactory =
    FutureOr<http.StreamedResponse> Function(http.AbortableRequest request);

final class _RecordingClient extends http.BaseClient {
  _RecordingClient(this._responses);

  final List<_ResponseFactory> _responses;
  final List<http.AbortableRequest> requests = <http.AbortableRequest>[];
  final Completer<void> requestSeen = Completer<void>();

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    final http.AbortableRequest typed = request as http.AbortableRequest;
    requests.add(typed);
    if (!requestSeen.isCompleted) {
      requestSeen.complete();
    }
    if (_responses.isEmpty) {
      throw StateError('No synthetic response remains.');
    }
    return _responses.removeAt(0)(typed);
  }
}

final class _AbortAwarePendingClient extends http.BaseClient {
  final Completer<void> requestSeen = Completer<void>();
  http.AbortableRequest? request;

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) {
    final http.AbortableRequest typed = request as http.AbortableRequest;
    this.request = typed;
    requestSeen.complete();
    return typed.abortTrigger!.then<http.StreamedResponse>((_) {
      throw StateError('synthetic abort detail');
    });
  }
}

final class _LateResponseClient extends http.BaseClient {
  final Completer<void> requestSeen = Completer<void>();
  final Completer<http.StreamedResponse> _response =
      Completer<http.StreamedResponse>();
  http.AbortableRequest? request;

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) {
    this.request = request as http.AbortableRequest;
    requestSeen.complete();
    return _response.future;
  }

  void respond(http.StreamedResponse response) {
    _response.complete(response);
  }
}

LichessBoardApiClient _client(
  http.Client httpClient, {
  LichessRequestCoordinator? coordinator,
  LichessAccessTokenProvider? accessTokenProvider,
  LichessRequestDeadline? requestDeadline,
  LichessResponseBodyDeadline? responseBodyDeadline,
}) {
  return LichessBoardApiClient(
    httpClient: httpClient,
    accessTokenProvider: accessTokenProvider ?? () async => 'test-token',
    requestCoordinator:
        coordinator ??
        LichessRequestCoordinator(cooldownStore: _FakeCooldownStore()),
    baseUri: Uri.parse('https://lichess.test'),
    requestDeadline: requestDeadline,
    responseBodyDeadline: responseBodyDeadline,
  );
}

Map<String, Object?> _playing(
  List<Map<String, Object?>> games, {
  required int nbMyTurn,
}) {
  return <String, Object?>{'nowPlaying': games, 'nbMyTurn': nbMyTurn};
}

Map<String, Object?> _playingGame({
  String gameId = 'BEOucQJo',
  String color = 'white',
  String variant = 'standard',
  String speed = 'rapid',
  bool rated = false,
  bool isMyTurn = true,
  int? ai,
}) {
  return <String, Object?>{
    'gameId': gameId,
    'color': color,
    'variant': <String, Object?>{'key': variant},
    'speed': speed,
    'rated': rated,
    'isMyTurn': isMyTurn,
    'opponent': <String, Object?>{
      'id': 'opponent',
      'username': 'Opponent',
      if (ai case final int level) 'ai': level,
    },
  };
}

http.StreamedResponse _ndjsonResponse(String body) {
  return http.StreamedResponse(
    Stream<List<int>>.value(utf8.encode(body)),
    HttpStatus.ok,
    headers: const <String, String>{
      HttpHeaders.contentTypeHeader: 'application/x-ndjson',
    },
  );
}

http.StreamedResponse _jsonResponse(
  int status, {
  String body = '{}',
  Map<String, String> headers = const <String, String>{},
}) {
  return http.StreamedResponse(
    Stream<List<int>>.value(utf8.encode(body)),
    status,
    headers: headers,
  );
}

Map<String, Object?> _gameFull() {
  return <String, Object?>{
    'type': 'gameFull',
    'id': 'BEOucQJo',
    'variant': <String, Object?>{'key': 'standard', 'name': 'Standard'},
    'speed': 'rapid',
    'perf': <String, Object?>{'name': 'Rapid'},
    'rated': false,
    'createdAt': 1788426000000,
    'white': <String, Object?>{'id': 'white-id', 'name': 'White'},
    'black': <String, Object?>{'id': 'black-id', 'name': 'Black'},
    'initialFen': 'startpos',
    'state': <String, Object?>{
      'type': 'gameState',
      'moves': '',
      'wtime': 600000,
      'btime': 600000,
      'winc': 0,
      'binc': 0,
      'status': 'started',
    },
  };
}

final class _ManualTime {
  Duration monotonicNow = Duration.zero;
  DateTime wallNow = DateTime.utc(2026, 9, 3);
  final List<Duration> delays = <Duration>[];

  Future<void> delay(Duration duration) async {
    delays.add(duration);
    monotonicNow += duration;
    wallNow = wallNow.add(duration);
  }
}

final class _FakeCooldownStore implements LichessCooldownStore {
  LichessCooldownMarker? marker;

  @override
  Future<void> clear() async {
    marker = null;
  }

  @override
  Future<LichessCooldownMarker?> read() async => marker;

  @override
  Future<void> write(LichessCooldownMarker value) async {
    marker = value;
  }
}
