import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/online/lichess/lichess_oauth_pkce.dart';

void main() {
  test('starts a public-client S256 request with only board play scope', () {
    final _DeterministicRandom random = _DeterministicRandom();
    final LichessOAuthConfiguration configuration = LichessOAuthConfiguration(
      clientId: 'com.foxie.litch.test',
      redirectUri: Uri.parse('com.foxie.litch.test:/oauth/callback'),
    );

    final LichessOAuthPendingAuthorization pending =
        LichessOAuthPendingAuthorization.start(
          configuration: configuration,
          secureRandomBytes: random.call,
        );

    final Map<String, String> query = pending.authorizationUri.queryParameters;
    expect(pending.authorizationUri.origin, 'https://lichess.org');
    expect(pending.authorizationUri.path, '/oauth');
    expect(query['response_type'], 'code');
    expect(query['client_id'], 'com.foxie.litch.test');
    expect(query['redirect_uri'], configuration.redirectUri.toString());
    expect(query['code_challenge_method'], 'S256');
    expect(query['scope'], 'board:play');
    expect(query, isNot(contains('client_secret')));
    expect(pending.codeVerifier, hasLength(43));
    expect(query['state'], hasLength(43));
    expect(query['state'], isNot(pending.codeVerifier));
    expect(
      query['code_challenge'],
      base64UrlEncode(
        sha256.convert(ascii.encode(pending.codeVerifier)).bytes,
      ).replaceAll('=', ''),
    );
  });

  test('accepts one exact callback and returns its code', () {
    final LichessOAuthPendingAuthorization pending = _pending();
    final String state = pending.authorizationUri.queryParameters['state']!;
    final Uri callback = pending.configuration.redirectUri.replace(
      queryParameters: <String, String>{
        'code': 'authorization-code',
        'state': state,
      },
    );

    final LichessOAuthAuthorizationCode result =
        pending.complete(callback) as LichessOAuthAuthorizationCode;

    expect(result.code, 'authorization-code');
    expect(
      () => pending.complete(callback),
      throwsA(_oauthFailure(LichessOAuthFailureKind.alreadyCompleted)),
    );
  });

  test('binds each code to one authorization and one token exchange', () {
    final LichessOAuthPendingAuthorization first = _pending();
    final LichessOAuthPendingAuthorization second = _pending(randomOffset: 64);
    final LichessOAuthAuthorizationCode firstResult =
        first.complete(
              first.configuration.redirectUri.replace(
                queryParameters: <String, String>{
                  'code': 'first-code',
                  'state': first.authorizationUri.queryParameters['state']!,
                },
              ),
            )
            as LichessOAuthAuthorizationCode;
    final LichessOAuthAuthorizationCode secondResult =
        second.complete(
              second.configuration.redirectUri.replace(
                queryParameters: <String, String>{
                  'code': 'second-code',
                  'state': second.authorizationUri.queryParameters['state']!,
                },
              ),
            )
            as LichessOAuthAuthorizationCode;

    expect(
      () => first.createTokenExchange(secondResult),
      throwsA(_oauthFailure(LichessOAuthFailureKind.transactionMismatch)),
    );

    final LichessOAuthTokenExchange exchange = first.createTokenExchange(
      firstResult,
    );
    expect(exchange.authorizationCode, 'first-code');
    expect(exchange.codeVerifier, first.codeVerifier);
    expect(exchange.toString(), isNot(contains('first-code')));
    expect(exchange.toString(), isNot(contains(first.codeVerifier)));
    expect(
      () => first.createTokenExchange(firstResult),
      throwsA(_oauthFailure(LichessOAuthFailureKind.alreadyClaimed)),
    );
  });

  test('rejects a different redirect without consuming the request', () {
    final LichessOAuthPendingAuthorization pending = _pending();
    final String state = pending.authorizationUri.queryParameters['state']!;
    final Uri wrongRedirect = Uri.parse('com.attacker.app:/oauth/callback')
        .replace(
          queryParameters: <String, String>{'code': 'stolen', 'state': state},
        );

    expect(
      () => pending.complete(wrongRedirect),
      throwsA(_oauthFailure(LichessOAuthFailureKind.redirectMismatch)),
    );

    final Uri correctRedirect = pending.configuration.redirectUri.replace(
      queryParameters: <String, String>{'code': 'fresh', 'state': state},
    );
    expect(
      (pending.complete(correctRedirect) as LichessOAuthAuthorizationCode).code,
      'fresh',
    );
  });

  test('rejects missing, duplicate, or mismatched state', () {
    for (final Uri Function(LichessOAuthPendingAuthorization) callback
        in <Uri Function(LichessOAuthPendingAuthorization)>[
          (LichessOAuthPendingAuthorization pending) => pending
              .configuration
              .redirectUri
              .replace(queryParameters: const <String, String>{'code': 'code'}),
          (LichessOAuthPendingAuthorization pending) => Uri.parse(
            '${pending.configuration.redirectUri}?code=code&state=one&state=two',
          ),
          (LichessOAuthPendingAuthorization pending) =>
              pending.configuration.redirectUri.replace(
                queryParameters: const <String, String>{
                  'code': 'code',
                  'state': 'wrong-state',
                },
              ),
        ]) {
      final LichessOAuthPendingAuthorization pending = _pending();
      expect(
        () => pending.complete(callback(pending)),
        throwsA(
          isA<LichessOAuthException>().having(
            (LichessOAuthException error) => error.kind,
            'kind',
            anyOf(
              LichessOAuthFailureKind.missingState,
              LichessOAuthFailureKind.stateMismatch,
            ),
          ),
        ),
      );
    }
  });

  test('maps denial without retaining provider description', () {
    final LichessOAuthPendingAuthorization pending = _pending();
    final String state = pending.authorizationUri.queryParameters['state']!;
    final Uri callback = pending.configuration.redirectUri.replace(
      queryParameters: <String, String>{
        'error': 'access_denied',
        'error_description': 'sensitive provider description',
        'state': state,
      },
    );

    final LichessOAuthDenied result =
        pending.complete(callback) as LichessOAuthDenied;

    expect(result.kind, LichessOAuthDenialKind.accessDenied);
    expect(
      result.toString(),
      isNot(contains('sensitive provider description')),
    );
  });

  test('rejects ambiguous response fields and unknown query data', () {
    for (final Map<String, String> fields in <Map<String, String>>[
      <String, String>{'code': 'code', 'error': 'access_denied'},
      <String, String>{'code': 'code', 'error_description': 'failure only'},
      <String, String>{},
      <String, String>{'code': 'code', 'unexpected': 'value'},
    ]) {
      final LichessOAuthPendingAuthorization pending = _pending();
      final String state = pending.authorizationUri.queryParameters['state']!;
      final Uri callback = pending.configuration.redirectUri.replace(
        queryParameters: <String, String>{...fields, 'state': state},
      );

      expect(
        () => pending.complete(callback),
        throwsA(_oauthFailure(LichessOAuthFailureKind.invalidResponse)),
      );
    }
  });

  test('rejects duplicate or conflicting callback result fields', () {
    for (final String query in <String>[
      'code=code&error=access_denied&error=provider_error',
      'code=code&code=other&error=access_denied',
      'error=access_denied&code=code&code=other',
    ]) {
      final LichessOAuthPendingAuthorization pending = _pending();
      final String state = pending.authorizationUri.queryParameters['state']!;

      expect(
        () => pending.complete(
          Uri.parse('${pending.configuration.redirectUri}?$query&state=$state'),
        ),
        throwsA(_oauthFailure(LichessOAuthFailureKind.invalidResponse)),
      );
    }
  });

  test('redacts malformed callback query encoding', () {
    final LichessOAuthPendingAuthorization pending = _pending();
    final String state = pending.authorizationUri.queryParameters['state']!;

    expect(
      () => pending.complete(
        Uri.parse(
          '${pending.configuration.redirectUri}?code=secret%FF&state=$state',
        ),
      ),
      throwsA(
        isA<LichessOAuthException>()
            .having(
              (LichessOAuthException error) => error.kind,
              'kind',
              LichessOAuthFailureKind.invalidResponse,
            )
            .having(
              (LichessOAuthException error) => error.toString(),
              'message',
              isNot(contains('secret')),
            ),
      ),
    );
  });

  test('rejects insecure configuration and broken randomness', () {
    for (final String redirect in <String>[
      'http://example.test/callback',
      'https:/callback',
      'javascript:alert',
      'data:text/plain,callback',
      'file:/callback',
      'ftp://example.test/callback',
      'ws://example.test/callback',
      'litch:/callback',
    ]) {
      expect(
        () => LichessOAuthConfiguration(
          clientId: 'client',
          redirectUri: Uri.parse(redirect),
        ),
        throwsA(_oauthFailure(LichessOAuthFailureKind.invalidConfiguration)),
      );
    }
    expect(
      LichessOAuthConfiguration(
        clientId: 'client',
        redirectUri: Uri.parse('https://example.test/callback'),
      ).redirectUri.host,
      'example.test',
    );
    expect(
      () => LichessOAuthPendingAuthorization.start(
        configuration: LichessOAuthConfiguration(
          clientId: 'client',
          redirectUri: Uri.parse('com.example.client:/callback'),
        ),
        secureRandomBytes: (int length) => Uint8List(length),
      ),
      throwsA(_oauthFailure(LichessOAuthFailureKind.invalidRandomness)),
    );
  });
}

LichessOAuthPendingAuthorization _pending({int randomOffset = 0}) {
  return LichessOAuthPendingAuthorization.start(
    configuration: LichessOAuthConfiguration(
      clientId: 'com.foxie.litch.test',
      redirectUri: Uri.parse('com.foxie.litch.test:/oauth/callback'),
    ),
    secureRandomBytes: _DeterministicRandom(randomOffset).call,
  );
}

Matcher _oauthFailure(LichessOAuthFailureKind kind) {
  return isA<LichessOAuthException>().having(
    (LichessOAuthException error) => error.kind,
    'kind',
    kind,
  );
}

final class _DeterministicRandom {
  _DeterministicRandom([this._offset = 0]);

  final int _offset;
  var _callCount = 0;

  Uint8List call(int length) {
    _callCount += 1;
    return Uint8List.fromList(
      List<int>.generate(length, (int index) => index + _callCount + _offset),
    );
  }
}
