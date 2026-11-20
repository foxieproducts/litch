import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/online/lichess/lichess_http.dart';
import 'package:litch_mobile/src/online/lichess/lichess_oauth_pkce.dart';
import 'package:litch_mobile/src/online/lichess/lichess_oauth_token_client.dart';
import 'package:litch_mobile/src/online/lichess/lichess_request_coordinator.dart';

void main() {
  test('exchanges a code without a secret and stores a long token', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final String token = 't' * 600;
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(<String, Object?>{
          'token_type': 'bearer',
          'access_token': token,
          'expires_in': 31536000,
        }),
      ),
    ]);
    final _MemoryTokenVault vault = _MemoryTokenVault();
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
    );

    await client.exchange(authorization.exchange);

    final http.AbortableRequest request = httpClient.requests.single;
    expect(request.method, 'POST');
    expect(request.url, Uri.parse('https://lichess.test/api/token'));
    expect(request.headers, isNot(contains(HttpHeaders.authorizationHeader)));
    expect(request.bodyFields, <String, String>{
      'grant_type': 'authorization_code',
      'code': 'test-authorization-code',
      'code_verifier': authorization.pending.codeVerifier,
      'redirect_uri': configuration.redirectUri.toString(),
      'client_id': configuration.clientId,
    });
    expect(request.bodyFields, isNot(contains('client_secret')));
    expect(vault.record?.state, LichessTokenCustodyState.active);
    expect(vault.token, token);
  });

  test('does not overwrite active or pending-revocation custody', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    for (final LichessTokenCustodyState state
        in LichessTokenCustodyState.values) {
      final _AuthorizationFixture authorization = _authorization(configuration);
      final _MemoryTokenVault vault = _MemoryTokenVault()
        ..record = LichessTokenRecord(state: state, token: 'existing-token');
      final _RecordingClient httpClient = _RecordingClient(
        <_ResponseFactory>[],
      );
      final LichessOAuthTokenClient client = _client(
        configuration,
        httpClient,
        vault,
      );

      await expectLater(
        client.exchange(authorization.exchange),
        throwsA(
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.invalidRequest,
          ),
        ),
      );

      expect(httpClient.requests, isEmpty);
      expect(vault.record?.state, state);
      expect(vault.token, 'existing-token');
    }
  });

  test('redacts typed vault read failures', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      _TypedFailingReadVault(),
    );

    await expectLater(
      client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>()
            .having(
              (LichessApiException error) => error.kind,
              'kind',
              LichessApiFailureKind.secureStorage,
            )
            .having(
              (LichessApiException error) => error.statusCode,
              'status',
              isNull,
            )
            .having(
              (LichessApiException error) => error.toString(),
              'message',
              isNot(contains('synthetic-secret')),
            ),
      ),
    );
    expect(httpClient.requests, isEmpty);
  });

  test(
    'rejects malformed and oversized token responses without leaking them',
    () async {
      final LichessOAuthConfiguration configuration = _configuration();
      for (final http.StreamedResponse response in <http.StreamedResponse>[
        _jsonResponse(
          HttpStatus.ok,
          body: '{"access_token":"secret-response-token"}',
        ),
        http.StreamedResponse(
          Stream<List<int>>.value(
            Uint8List(LichessOAuthTokenClient.maximumResponseBytes + 1),
          ),
          HttpStatus.ok,
        ),
      ]) {
        final _AuthorizationFixture authorization = _authorization(
          configuration,
        );
        final _MemoryTokenVault vault = _MemoryTokenVault();
        final LichessOAuthTokenClient client = _client(
          configuration,
          _RecordingClient(<_ResponseFactory>[(_) => response]),
          vault,
        );

        await expectLater(
          client.exchange(authorization.exchange),
          throwsA(
            isA<LichessApiException>()
                .having(
                  (LichessApiException error) => error.kind,
                  'kind',
                  LichessApiFailureKind.invalidResponse,
                )
                .having(
                  (LichessApiException error) => error.toString(),
                  'message',
                  isNot(contains('secret-response-token')),
                ),
          ),
        );
        expect(vault.token, isNull);
      }
    },
  );

  test('secure-vault write failures are typed and redacted', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(<String, Object?>{
          'token_type': 'Bearer',
          'access_token': 'provider-token',
          'expires_in': 31536000,
        }),
      ),
      (_) => _jsonResponse(HttpStatus.noContent, body: ''),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      _MemoryTokenVault(throwOnWrite: true),
    );

    await expectLater(
      client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>()
            .having(
              (LichessApiException error) => error.kind,
              'kind',
              LichessApiFailureKind.secureStorage,
            )
            .having(
              (LichessApiException error) => error.toString(),
              'message',
              isNot(contains('provider-token')),
            ),
      ),
    );
    expect(httpClient.requests, hasLength(2));
    expect(httpClient.requests.last.method, 'DELETE');
    expect(
      httpClient.requests.last.headers[HttpHeaders.authorizationHeader],
      'Bearer provider-token',
    );
  });

  test('closing during a vault write preserves revocation custody', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(<String, Object?>{
          'token_type': 'Bearer',
          'access_token': 'late-provider-token',
          'expires_in': 31536000,
        }),
      ),
    ]);
    final _BlockingWriteTokenVault vault = _BlockingWriteTokenVault();
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
    );

    final Future<void> exchange = client.exchange(authorization.exchange);
    await vault.writeStarted.future;
    client.close();
    vault.releaseWrite.complete();

    await expectLater(
      exchange,
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.disposed,
        ),
      ),
    );
    expect(vault.token, 'late-provider-token');
    expect(vault.record?.state, LichessTokenCustodyState.pendingRevocation);
    expect(vault.deleteCount, 0);

    final LichessOAuthTokenClient replacement = _client(
      configuration,
      _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.noContent, body: ''),
      ]),
      vault,
    );
    expect(
      (await replacement.revokeAndSignOut()).status,
      LichessSignOutStatus.revoked,
    );
    expect(vault.token, isNull);
  });

  test(
    'failed post-close revocation custody reports the active-token recovery',
    () async {
      final LichessOAuthConfiguration configuration = _configuration();
      final _AuthorizationFixture authorization = _authorization(configuration);
      final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(
          HttpStatus.ok,
          body: jsonEncode(<String, Object?>{
            'token_type': 'Bearer',
            'access_token': 'late-provider-token',
            'expires_in': 31536000,
          }),
        ),
      ]);
      final _FailingPostCloseWriteVault vault = _FailingPostCloseWriteVault();
      final LichessOAuthTokenClient client = _client(
        configuration,
        httpClient,
        vault,
      );

      final Future<void> exchange = client.exchange(authorization.exchange);
      await vault.firstWriteStarted.future;
      client.close();
      vault.releaseFirstWrite.complete();

      await expectLater(
        exchange,
        throwsA(
          isA<LichessApiException>()
              .having(
                (LichessApiException error) => error.kind,
                'kind',
                LichessApiFailureKind.secureStorage,
              )
              .having(
                (LichessApiException error) => error.toString(),
                'recovery',
                contains('may remain active'),
              )
              .having(
                (LichessApiException error) => error.toString(),
                'revoke',
                contains('revoke Litch access in Lichess settings'),
              ),
        ),
      );
      expect(vault.token, 'late-provider-token');
      expect(vault.record?.state, LichessTokenCustodyState.active);
      expect(vault.deleteCount, 0);
    },
  );

  test('a later sign-out wins over an in-flight exchange', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(<String, Object?>{
          'token_type': 'Bearer',
          'access_token': 'new-provider-token',
          'expires_in': 31536000,
        }),
      ),
      (_) => _jsonResponse(HttpStatus.noContent, body: ''),
    ]);
    final _BlockingWriteTokenVault vault = _BlockingWriteTokenVault();
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
    );

    final Future<void> exchange = client.exchange(authorization.exchange);
    await vault.writeStarted.future;
    final Future<LichessSignOutResult> signOut = client.revokeAndSignOut();
    vault.releaseWrite.complete();

    await exchange;
    expect((await signOut).status, LichessSignOutStatus.revoked);
    expect(vault.token, isNull);
    expect(httpClient.requests, hasLength(2));
  });

  test('sign-out persists pending custody until remote revocation', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final String token = 'r' * 600;
    final _MemoryTokenVault vault = _MemoryTokenVault(token: token);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) {
        expect(vault.record?.state, LichessTokenCustodyState.pendingRevocation);
        expect(vault.token, token);
        return _jsonResponse(HttpStatus.noContent, body: '');
      },
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
    );

    final LichessSignOutResult result = await client.revokeAndSignOut();

    final http.AbortableRequest request = httpClient.requests.single;
    expect(request.method, 'DELETE');
    expect(request.url, Uri.parse('https://lichess.test/api/token'));
    expect(request.headers[HttpHeaders.authorizationHeader], 'Bearer $token');
    expect(result.status, LichessSignOutStatus.revoked);
    expect(vault.token, isNull);
    expect(vault.deleteCount, 1);
  });

  test('an already-invalid token still completes local sign-out', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _MemoryTokenVault vault = _MemoryTokenVault(token: 'stored-token');
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.unauthorized),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
    );

    final LichessSignOutResult result = await client.revokeAndSignOut();

    expect(result.status, LichessSignOutStatus.revoked);
    expect(vault.token, isNull);
    expect(httpClient.requests, hasLength(1));
  });

  test('OAuth 429 shares the injected provider cooldown', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
      cooldownStore: _FakeCooldownStore(),
      monotonicNow: () => time.monotonicNow,
      wallNow: () => time.wallNow,
      delay: time.delay,
    );
    final _MemoryTokenVault vault = _MemoryTokenVault();
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.tooManyRequests,
        headers: <String, String>{HttpHeaders.retryAfterHeader: '75'},
      ),
      (_) => _jsonResponse(HttpStatus.noContent, body: ''),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
      coordinator: coordinator,
    );

    await expectLater(
      client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.rateLimited,
        ),
      ),
    );
    vault.record = const LichessTokenRecord(
      state: LichessTokenCustodyState.active,
      token: 'stored-token',
    );
    final LichessSignOutResult result = await client.revokeAndSignOut();

    expect(time.delays, <Duration>[const Duration(seconds: 75)]);
    expect(httpClient.requests, hasLength(2));
    expect(result.status, LichessSignOutStatus.revoked);
  });

  test('revoke 429 preserves retryable custody across a retry', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _ManualTime time = _ManualTime();
    final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
      cooldownStore: _FakeCooldownStore(),
      monotonicNow: () => time.monotonicNow,
      wallNow: () => time.wallNow,
      delay: time.delay,
    );
    final _MemoryTokenVault vault = _MemoryTokenVault(token: 'stored-token');
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.tooManyRequests,
        headers: <String, String>{HttpHeaders.retryAfterHeader: '75'},
      ),
      (_) => _jsonResponse(HttpStatus.noContent, body: ''),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      vault,
      coordinator: coordinator,
    );

    final LichessSignOutResult first = await client.revokeAndSignOut();
    expect(first.status, LichessSignOutStatus.revocationPending);
    expect(vault.record?.state, LichessTokenCustodyState.pendingRevocation);
    expect(vault.token, 'stored-token');

    final LichessSignOutResult retry = await client.revokeAndSignOut();
    expect(retry.status, LichessSignOutStatus.revoked);
    expect(vault.token, isNull);
    expect(time.delays, <Duration>[const Duration(seconds: 75)]);
  });

  test('pending revocation resumes after client replacement', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _MemoryTokenVault vault = _MemoryTokenVault(token: 'stored-token');
    final LichessOAuthTokenClient first = _client(
      configuration,
      _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.serviceUnavailable),
      ]),
      vault,
    );

    final LichessSignOutResult pending = await first.revokeAndSignOut();
    expect(pending.status, LichessSignOutStatus.revocationPending);
    expect(vault.record?.state, LichessTokenCustodyState.pendingRevocation);

    final LichessOAuthTokenClient replacement = _client(
      configuration,
      _RecordingClient(<_ResponseFactory>[
        (_) => _jsonResponse(HttpStatus.noContent, body: ''),
      ]),
      vault,
    );
    final LichessSignOutResult resumed = await replacement.revokeAndSignOut();

    expect(resumed.status, LichessSignOutStatus.revoked);
    expect(vault.token, isNull);
  });

  test('a token request deadline completes the abort trigger', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.ok),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      _MemoryTokenVault(),
      requestDeadline: (_, _) async => null,
    );

    await expectLater(
      client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.transport,
        ),
      ),
    );

    await expectLater(httpClient.requests.single.abortTrigger, completes);
  });

  test('a token response-body deadline completes the abort trigger', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(HttpStatus.ok),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      _MemoryTokenVault(),
      responseBodyDeadline: (_, _) async => null,
    );

    await expectLater(
      client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.transport,
        ),
      ),
    );

    await expectLater(httpClient.requests.single.abortTrigger, completes);
  });

  for (final String operation in <String>['exchange', 'revoke']) {
    test(
      'closing during $operation vault read releases the shared coordinator',
      () async {
        final LichessOAuthConfiguration configuration = _configuration();
        final _FirstBlockingReadTokenVault vault =
            _FirstBlockingReadTokenVault();
        final LichessRequestCoordinator coordinator = LichessRequestCoordinator(
          cooldownStore: _FakeCooldownStore(),
        );
        final _RecordingClient firstHttp = _RecordingClient(
          <_ResponseFactory>[],
        );
        final LichessOAuthTokenClient first = _client(
          configuration,
          firstHttp,
          vault,
          coordinator: coordinator,
        );
        final Future<Object?> pending = operation == 'exchange'
            ? first.exchange(_authorization(configuration).exchange)
            : first.revokeAndSignOut();
        bool completed = false;
        Object? completionError;
        unawaited(
          pending.then<void>(
            (_) => completed = true,
            onError: (Object error, StackTrace _) {
              completed = true;
              completionError = error;
            },
          ),
        );

        await vault.readStarted.future;
        first.close();
        await Future<void>.microtask(() {});

        expect(completed, isTrue);
        expect(
          completionError,
          isA<LichessApiException>().having(
            (LichessApiException error) => error.kind,
            'kind',
            LichessApiFailureKind.disposed,
          ),
        );
        expect(firstHttp.requests, isEmpty);
        expect(vault.record, isNull);
        expect(vault.deleteCount, 0);

        final _RecordingClient replacementHttp = _RecordingClient(
          operation == 'exchange'
              ? <_ResponseFactory>[
                  (_) => _jsonResponse(
                    HttpStatus.ok,
                    body: jsonEncode(<String, Object?>{
                      'token_type': 'Bearer',
                      'access_token': 'replacement-token',
                      'expires_in': 31536000,
                    }),
                  ),
                ]
              : <_ResponseFactory>[],
        );
        final LichessOAuthTokenClient replacement = _client(
          configuration,
          replacementHttp,
          vault,
          coordinator: coordinator,
        );
        if (operation == 'exchange') {
          await replacement.exchange(_authorization(configuration).exchange);
          expect(replacementHttp.requests, hasLength(1));
          expect(vault.token, 'replacement-token');
        } else {
          expect(
            (await replacement.revokeAndSignOut()).status,
            LichessSignOutStatus.alreadySignedOut,
          );
          expect(replacementHttp.requests, isEmpty);
        }

        if (operation == 'exchange') {
          vault.releaseRead.completeError(StateError('late vault failure'));
        } else {
          vault.releaseRead.complete();
        }
        await Future<void>.microtask(() {});
      },
    );
  }

  test('the same token exchange cannot be submitted twice', () async {
    final LichessOAuthConfiguration configuration = _configuration();
    final _AuthorizationFixture authorization = _authorization(configuration);
    final _RecordingClient httpClient = _RecordingClient(<_ResponseFactory>[
      (_) => _jsonResponse(
        HttpStatus.ok,
        body: jsonEncode(<String, Object?>{
          'token_type': 'Bearer',
          'access_token': 'provider-token',
          'expires_in': 31536000,
        }),
      ),
    ]);
    final LichessOAuthTokenClient client = _client(
      configuration,
      httpClient,
      _MemoryTokenVault(),
    );

    await client.exchange(authorization.exchange);
    await expectLater(
      () => client.exchange(authorization.exchange),
      throwsA(
        isA<LichessApiException>().having(
          (LichessApiException error) => error.kind,
          'kind',
          LichessApiFailureKind.invalidRequest,
        ),
      ),
    );
    expect(httpClient.requests, hasLength(1));
  });
}

LichessOAuthTokenClient _client(
  LichessOAuthConfiguration configuration,
  http.Client httpClient,
  LichessTokenVault tokenVault, {
  LichessRequestCoordinator? coordinator,
  LichessRequestDeadline? requestDeadline,
  LichessResponseBodyDeadline? responseBodyDeadline,
}) {
  return LichessOAuthTokenClient(
    httpClient: httpClient,
    requestCoordinator:
        coordinator ??
        LichessRequestCoordinator(cooldownStore: _FakeCooldownStore()),
    tokenVault: tokenVault,
    configuration: configuration,
    requestDeadline: requestDeadline,
    responseBodyDeadline: responseBodyDeadline,
  );
}

LichessOAuthConfiguration _configuration() {
  return LichessOAuthConfiguration(
    clientId: 'com.foxie.litch.test',
    redirectUri: Uri.parse('com.foxie.litch.test:/oauth/callback'),
    baseUri: Uri.parse('https://lichess.test'),
  );
}

_AuthorizationFixture _authorization(LichessOAuthConfiguration configuration) {
  var callCount = 0;
  final LichessOAuthPendingAuthorization pending =
      LichessOAuthPendingAuthorization.start(
        configuration: configuration,
        secureRandomBytes: (int length) {
          callCount += 1;
          return Uint8List.fromList(
            List<int>.generate(length, (int index) => index + callCount),
          );
        },
      );
  final Uri callback = configuration.redirectUri.replace(
    queryParameters: <String, String>{
      'code': 'test-authorization-code',
      'state': pending.authorizationUri.queryParameters['state']!,
    },
  );
  final LichessOAuthAuthorizationCode result =
      pending.complete(callback) as LichessOAuthAuthorizationCode;
  return _AuthorizationFixture(
    pending: pending,
    result: result,
    exchange: pending.createTokenExchange(result),
  );
}

final class _AuthorizationFixture {
  const _AuthorizationFixture({
    required this.pending,
    required this.result,
    required this.exchange,
  });

  final LichessOAuthPendingAuthorization pending;
  final LichessOAuthAuthorizationCode result;
  final LichessOAuthTokenExchange exchange;
}

typedef _ResponseFactory =
    http.StreamedResponse Function(http.AbortableRequest request);

final class _RecordingClient extends http.BaseClient {
  _RecordingClient(this._responses);

  final List<_ResponseFactory> _responses;
  final List<http.AbortableRequest> requests = <http.AbortableRequest>[];

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    final http.AbortableRequest typed = request as http.AbortableRequest;
    requests.add(typed);
    return _responses.removeAt(0)(typed);
  }
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

final class _MemoryTokenVault implements LichessTokenVault {
  _MemoryTokenVault({String? token, this.throwOnWrite = false})
    : record = token == null
          ? null
          : LichessTokenRecord(
              state: LichessTokenCustodyState.active,
              token: token,
            );

  LichessTokenRecord? record;
  final bool throwOnWrite;
  int deleteCount = 0;

  String? get token => record?.token;

  @override
  Future<void> delete() async {
    deleteCount += 1;
    record = null;
  }

  @override
  Future<LichessTokenRecord?> read() async => record;

  @override
  Future<void> write(LichessTokenRecord record) async {
    if (throwOnWrite) {
      throw StateError('secure storage detail');
    }
    this.record = record;
  }
}

final class _BlockingWriteTokenVault implements LichessTokenVault {
  final Completer<void> writeStarted = Completer<void>();
  final Completer<void> releaseWrite = Completer<void>();
  LichessTokenRecord? record;
  int deleteCount = 0;

  String? get token => record?.token;

  @override
  Future<void> delete() async {
    deleteCount += 1;
    record = null;
  }

  @override
  Future<LichessTokenRecord?> read() async => record;

  @override
  Future<void> write(LichessTokenRecord record) async {
    if (!writeStarted.isCompleted) {
      writeStarted.complete();
    }
    await releaseWrite.future;
    this.record = record;
  }
}

final class _FirstBlockingReadTokenVault extends _MemoryTokenVault {
  final Completer<void> readStarted = Completer<void>();
  final Completer<void> releaseRead = Completer<void>();
  var _readCount = 0;

  @override
  Future<LichessTokenRecord?> read() async {
    _readCount += 1;
    if (_readCount == 1) {
      readStarted.complete();
      await releaseRead.future;
    }
    return record;
  }
}

final class _FailingPostCloseWriteVault extends _MemoryTokenVault {
  final Completer<void> firstWriteStarted = Completer<void>();
  final Completer<void> releaseFirstWrite = Completer<void>();
  var _writeCount = 0;

  @override
  Future<void> write(LichessTokenRecord record) async {
    _writeCount += 1;
    if (_writeCount == 1) {
      firstWriteStarted.complete();
      await releaseFirstWrite.future;
      this.record = record;
      return;
    }
    throw StateError('secure storage detail');
  }
}

final class _TypedFailingReadVault implements LichessTokenVault {
  @override
  Future<void> delete() async {}

  @override
  Future<LichessTokenRecord?> read() async {
    throw const LichessApiException(
      LichessApiFailureKind.transport,
      'synthetic-secret',
      statusCode: 418,
    );
  }

  @override
  Future<void> write(LichessTokenRecord record) async {}
}

final class _ManualTime {
  Duration monotonicNow = Duration.zero;
  DateTime wallNow = DateTime.utc(2026, 9, 5);
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
