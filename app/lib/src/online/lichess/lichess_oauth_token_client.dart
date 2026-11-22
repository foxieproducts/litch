import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import 'lichess_http.dart';
import 'lichess_oauth_pkce.dart';
import 'lichess_request_coordinator.dart';

export 'lichess_http.dart' show LichessResponseBodyDeadline;

enum LichessTokenCustodyState { active, pendingRevocation }

final class LichessTokenRecord {
  const LichessTokenRecord({required this.state, required this.token});

  final LichessTokenCustodyState state;
  final String token;

  @override
  String toString() => 'LichessTokenRecord(${state.name}, <redacted>)';
}

abstract interface class LichessTokenVault {
  Future<LichessTokenRecord?> read();

  Future<void> write(LichessTokenRecord record);

  Future<void> delete();
}

enum LichessSignOutStatus { alreadySignedOut, revoked, revocationPending }

final class LichessSignOutResult {
  const LichessSignOutResult(this.status);

  final LichessSignOutStatus status;
}

final class LichessOAuthTokenClient {
  LichessOAuthTokenClient({
    required http.Client httpClient,
    required LichessRequestCoordinator requestCoordinator,
    required LichessTokenVault tokenVault,
    required LichessOAuthConfiguration configuration,
    this.requestStartTimeout = const Duration(seconds: 30),
    this.responseBodyTimeout = const Duration(seconds: 10),
    LichessRequestDeadline? requestDeadline,
    LichessResponseBodyDeadline? responseBodyDeadline,
  }) : _httpClient = httpClient,
       _requestCoordinator = requestCoordinator,
       _tokenVault = tokenVault,
       _configuration = configuration,
       _requestDeadline = requestDeadline ?? lichessDefaultRequestDeadline,
       _responseBodyDeadline =
           responseBodyDeadline ?? lichessDefaultResponseBodyDeadline,
       assert(requestStartTimeout > Duration.zero),
       assert(responseBodyTimeout > Duration.zero);

  static const int maximumResponseBytes = 64 * 1024;

  final http.Client _httpClient;
  final LichessRequestCoordinator _requestCoordinator;
  final LichessTokenVault _tokenVault;
  final LichessOAuthConfiguration _configuration;
  final LichessRequestDeadline _requestDeadline;
  final LichessResponseBodyDeadline _responseBodyDeadline;
  final Duration requestStartTimeout;
  final Duration responseBodyTimeout;

  final Set<Completer<void>> _activeAborts = <Completer<void>>{};
  bool _closed = false;

  Future<void> exchange(LichessOAuthTokenExchange exchange) {
    _ensureAvailable();
    if (!_sameConfiguration(exchange.configuration, _configuration)) {
      throw const LichessApiException(
        LichessApiFailureKind.invalidRequest,
        'The Lichess authorization request does not match this client.',
      );
    }
    try {
      exchange.claimForSubmission();
    } on LichessOAuthException catch (error) {
      throw LichessApiException(
        LichessApiFailureKind.invalidRequest,
        error.message,
      );
    }
    return _requestCoordinator.runFinite<void>(() async {
      _ensureAvailable();
      if (await _readVault() != null) {
        throw const LichessApiException(
          LichessApiFailureKind.invalidRequest,
          'Existing Lichess authorization must be signed out before another account can be authorized.',
        );
      }
      final _OwnedTokenRequest owned = _request(
        'POST',
        _configuration.baseUri.replace(path: '/api/token'),
      );
      owned.request.bodyFields = <String, String>{
        'grant_type': 'authorization_code',
        'code': exchange.authorizationCode,
        'code_verifier': exchange.codeVerifier,
        'redirect_uri': _configuration.redirectUri.toString(),
        'client_id': _configuration.clientId,
      };
      try {
        final http.StreamedResponse response = await _send(owned);
        if (response.statusCode != HttpStatus.ok) {
          await _throwForStatus(response);
        }
        final Uint8List body = await _readBoundedBody(response.stream, owned);
        final String token = _decodeToken(body);
        try {
          await _tokenVault.write(
            LichessTokenRecord(
              state: LichessTokenCustodyState.active,
              token: token,
            ),
          );
        } catch (_) {
          final bool revoked = await _revokeUnstoredToken(token);
          throw LichessApiException(
            LichessApiFailureKind.secureStorage,
            revoked
                ? 'Secure Lichess token storage is unavailable; authorization was revoked.'
                : 'Secure Lichess token storage is unavailable; revoke Litch access in Lichess settings.',
          );
        }
        if (_closed) {
          try {
            await _tokenVault.write(
              LichessTokenRecord(
                state: LichessTokenCustodyState.pendingRevocation,
                token: token,
              ),
            );
          } catch (_) {
            throw const LichessApiException(
              LichessApiFailureKind.secureStorage,
              'Secure Lichess token storage is unavailable; authorization may remain active, so revoke Litch access in Lichess settings.',
            );
          }
          throw const LichessApiException(
            LichessApiFailureKind.disposed,
            'The Lichess client is closed.',
          );
        }
      } finally {
        _finish(owned);
      }
    });
  }

  Future<LichessSignOutResult> revokeAndSignOut() {
    _ensureAvailable();
    return _requestCoordinator.runFinite<LichessSignOutResult>(() async {
      _ensureAvailable();
      final LichessTokenRecord? existing = await _readVault();
      if (existing == null) {
        return const LichessSignOutResult(
          LichessSignOutStatus.alreadySignedOut,
        );
      }
      if (!_isValidToken(existing.token)) {
        await _deleteVault();
        throw const LichessApiException(
          LichessApiFailureKind.unavailableCredentials,
          'Lichess authorization is unavailable.',
        );
      }
      if (existing.state == LichessTokenCustodyState.active) {
        await _writeVault(
          LichessTokenRecord(
            state: LichessTokenCustodyState.pendingRevocation,
            token: existing.token,
          ),
        );
      }

      final bool revoked = await _tryRemoteRevoke(existing.token);
      if (!revoked) {
        return const LichessSignOutResult(
          LichessSignOutStatus.revocationPending,
        );
      }
      await _deleteVault();
      return const LichessSignOutResult(LichessSignOutStatus.revoked);
    });
  }

  Future<bool> _tryRemoteRevoke(String token) async {
    final _OwnedTokenRequest owned = _request(
      'DELETE',
      _configuration.baseUri.replace(path: '/api/token'),
    );
    owned.request.headers[HttpHeaders.authorizationHeader] = 'Bearer $token';
    try {
      final http.StreamedResponse response = await _send(owned);
      if (response.statusCode == HttpStatus.noContent ||
          response.statusCode == HttpStatus.unauthorized) {
        await _discardBody(response);
        return true;
      }
      try {
        await _throwForStatus(response);
      } catch (_) {
        return false;
      }
      return false;
    } catch (_) {
      return false;
    } finally {
      _finish(owned);
    }
  }

  Future<bool> _revokeUnstoredToken(String token) async {
    try {
      return await _tryRemoteRevoke(token);
    } catch (_) {
      return false;
    }
  }

  Future<LichessTokenRecord?> _readVault() async {
    final Completer<void> abort = Completer<void>();
    _activeAborts.add(abort);
    try {
      final Future<LichessTokenRecord?> read = _tokenVault.read();
      return await Future.any<LichessTokenRecord?>(
        <Future<LichessTokenRecord?>>[
          read,
          abort.future.then<LichessTokenRecord?>((void _) {
            throw const LichessApiException(
              LichessApiFailureKind.disposed,
              'The Lichess client is closed.',
            );
          }),
        ],
      );
    } catch (_) {
      if (_closed) {
        throw const LichessApiException(
          LichessApiFailureKind.disposed,
          'The Lichess client is closed.',
        );
      }
      throw const LichessApiException(
        LichessApiFailureKind.secureStorage,
        'Secure Lichess token storage is unavailable.',
      );
    } finally {
      _activeAborts.remove(abort);
      if (!abort.isCompleted) {
        abort.complete();
      }
    }
  }

  Future<void> _writeVault(LichessTokenRecord record) async {
    try {
      await _tokenVault.write(record);
    } catch (_) {
      throw const LichessApiException(
        LichessApiFailureKind.secureStorage,
        'Secure Lichess token storage is unavailable.',
      );
    }
  }

  Future<void> _deleteVault() async {
    try {
      await _tokenVault.delete();
    } catch (_) {
      throw const LichessApiException(
        LichessApiFailureKind.secureStorage,
        'Secure Lichess token storage is unavailable.',
      );
    }
  }

  void close() {
    if (_closed) {
      return;
    }
    _closed = true;
    for (final Completer<void> abort in _activeAborts.toList()) {
      if (!abort.isCompleted) {
        abort.complete();
      }
    }
    _activeAborts.clear();
  }

  _OwnedTokenRequest _request(String method, Uri uri) {
    final Completer<void> abort = Completer<void>();
    final http.AbortableRequest request = http.AbortableRequest(
      method,
      uri,
      abortTrigger: abort.future,
    )..followRedirects = false;
    request.headers[HttpHeaders.acceptHeader] = 'application/json';
    _activeAborts.add(abort);
    return _OwnedTokenRequest(request: request, abort: abort);
  }

  Future<http.StreamedResponse> _send(_OwnedTokenRequest owned) async {
    _ensureAvailable();
    try {
      final http.StreamedResponse? response = await _requestDeadline(
        _httpClient.send(owned.request),
        requestStartTimeout,
      );
      if (response == null) {
        if (!owned.abort.isCompleted) {
          owned.abort.complete();
        }
        throw const LichessApiException(
          LichessApiFailureKind.transport,
          'Lichess did not respond before the request deadline.',
        );
      }
      return response;
    } on LichessApiException {
      rethrow;
    } catch (_) {
      if (_closed) {
        throw const LichessApiException(
          LichessApiFailureKind.disposed,
          'The Lichess client is closed.',
        );
      }
      throw const LichessApiException(
        LichessApiFailureKind.transport,
        'The Lichess request could not be completed.',
      );
    }
  }

  void _finish(_OwnedTokenRequest owned) {
    _activeAborts.remove(owned.abort);
    if (!owned.abort.isCompleted) {
      owned.abort.complete();
    }
  }

  Future<void> _throwForStatus(http.StreamedResponse response) async {
    final int statusCode = response.statusCode;
    if (statusCode == HttpStatus.tooManyRequests) {
      await _requestCoordinator.recordRateLimit(
        retryAfter: response.headers[HttpHeaders.retryAfterHeader],
      );
    }
    await _discardBody(response);
    throw LichessApiException(
      lichessFailureKindForStatus(statusCode),
      lichessSafeStatusMessage(statusCode),
      statusCode: statusCode,
    );
  }

  Future<Uint8List> _readBoundedBody(
    Stream<List<int>> source,
    _OwnedTokenRequest owned,
  ) async {
    return lichessReadBoundedResponseBody(
      source,
      maximumBytes: maximumResponseBytes,
      timeout: responseBodyTimeout,
      deadline: _responseBodyDeadline,
      isClosed: () => _closed,
      abort: () {
        if (!owned.abort.isCompleted) {
          owned.abort.complete();
        }
      },
      oversizedMessage: 'Lichess returned an oversized token response.',
      deadlineMessage:
          'Lichess did not complete the token response before the deadline.',
    );
  }

  String _decodeToken(Uint8List body) {
    final Object? decoded;
    try {
      decoded = jsonDecode(utf8.decode(body));
    } catch (_) {
      throw const LichessApiException(
        LichessApiFailureKind.invalidResponse,
        'Lichess returned an invalid token response.',
      );
    }
    final Object? tokenType = decoded is Map<String, dynamic>
        ? decoded['token_type']
        : null;
    if (decoded is! Map<String, dynamic> ||
        tokenType is! String ||
        tokenType.toLowerCase() != 'bearer' ||
        decoded['expires_in'] is! int ||
        (decoded['expires_in'] as int) <= 0) {
      throw const LichessApiException(
        LichessApiFailureKind.invalidResponse,
        'Lichess returned an invalid token response.',
      );
    }
    final Object? token = decoded['access_token'];
    if (token is! String || !_isValidToken(token)) {
      throw const LichessApiException(
        LichessApiFailureKind.invalidResponse,
        'Lichess returned an invalid token response.',
      );
    }
    return token;
  }

  Future<void> _discardBody(http.StreamedResponse response) async {
    final StreamSubscription<List<int>> subscription = response.stream.listen(
      (_) {},
      onError: (_) {},
    );
    await subscription.cancel();
  }

  void _ensureAvailable() {
    if (_closed) {
      throw const LichessApiException(
        LichessApiFailureKind.disposed,
        'The Lichess client is closed.',
      );
    }
  }
}

bool _isValidToken(String token) {
  return token.isNotEmpty &&
      token.length <= 4096 &&
      token.codeUnits.every((int unit) => unit > 0x20 && unit < 0x7f);
}

bool _sameConfiguration(
  LichessOAuthConfiguration first,
  LichessOAuthConfiguration second,
) {
  return first.clientId == second.clientId &&
      first.redirectUri == second.redirectUri &&
      first.baseUri == second.baseUri;
}

final class _OwnedTokenRequest {
  const _OwnedTokenRequest({required this.request, required this.abort});

  final http.AbortableRequest request;
  final Completer<void> abort;
}
