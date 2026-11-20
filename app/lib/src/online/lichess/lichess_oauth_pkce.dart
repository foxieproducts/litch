import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

typedef LichessSecureRandomBytes = Uint8List Function(int length);

enum LichessOAuthFailureKind {
  invalidConfiguration,
  invalidRandomness,
  redirectMismatch,
  missingState,
  stateMismatch,
  invalidResponse,
  alreadyCompleted,
  transactionMismatch,
  alreadyClaimed,
}

final class LichessOAuthException implements Exception {
  const LichessOAuthException(this.kind, this.message);

  final LichessOAuthFailureKind kind;
  final String message;

  @override
  String toString() => message;
}

final class LichessOAuthConfiguration {
  LichessOAuthConfiguration({
    required this.clientId,
    required this.redirectUri,
    Uri? baseUri,
  }) : baseUri = baseUri ?? Uri.parse('https://lichess.org') {
    if (!_isBoundedPrintableAscii(clientId, maximumLength: 128)) {
      _invalidConfiguration();
    }
    final bool secureWebRedirect =
        redirectUri.scheme == 'https' && redirectUri.host.isNotEmpty;
    final bool privateUseMobileRedirect =
        _reverseDomainScheme.hasMatch(redirectUri.scheme) &&
        !redirectUri.hasAuthority;
    if (!redirectUri.isAbsolute ||
        (!secureWebRedirect && !privateUseMobileRedirect) ||
        redirectUri.path.isEmpty ||
        redirectUri.userInfo.isNotEmpty ||
        redirectUri.hasQuery ||
        redirectUri.hasFragment) {
      _invalidConfiguration();
    }
    if (this.baseUri.scheme != 'https' ||
        this.baseUri.host.isEmpty ||
        this.baseUri.userInfo.isNotEmpty ||
        this.baseUri.hasQuery ||
        this.baseUri.hasFragment ||
        (this.baseUri.path.isNotEmpty && this.baseUri.path != '/')) {
      _invalidConfiguration();
    }
  }

  static final RegExp _reverseDomainScheme = RegExp(
    r'^[a-z][a-z0-9-]*(?:\.[a-z0-9][a-z0-9-]*)+$',
  );

  static const String scope = 'board:play';

  final String clientId;
  final Uri redirectUri;
  final Uri baseUri;
}

sealed class LichessOAuthCallbackResult {
  const LichessOAuthCallbackResult();
}

final class LichessOAuthAuthorizationCode extends LichessOAuthCallbackResult {
  const LichessOAuthAuthorizationCode._(this.code, this._transactionBinding);

  final String code;
  final String _transactionBinding;
}

final class LichessOAuthTokenExchange {
  LichessOAuthTokenExchange._({
    required this.configuration,
    required this.authorizationCode,
    required this.codeVerifier,
  });

  final LichessOAuthConfiguration configuration;
  final String authorizationCode;
  final String codeVerifier;

  bool _claimed = false;

  void claimForSubmission() {
    if (_claimed) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.alreadyClaimed,
        'This Lichess token exchange was already submitted.',
      );
    }
    _claimed = true;
  }

  @override
  String toString() => 'LichessOAuthTokenExchange(<redacted>)';
}

enum LichessOAuthDenialKind { accessDenied, providerError }

final class LichessOAuthDenied extends LichessOAuthCallbackResult {
  const LichessOAuthDenied._(this.kind);

  final LichessOAuthDenialKind kind;
}

final class LichessOAuthPendingAuthorization {
  LichessOAuthPendingAuthorization._({
    required this.configuration,
    required this.authorizationUri,
    required this.codeVerifier,
    required String state,
  }) : _state = state;

  factory LichessOAuthPendingAuthorization.start({
    required LichessOAuthConfiguration configuration,
    LichessSecureRandomBytes? secureRandomBytes,
  }) {
    final LichessSecureRandomBytes randomBytes =
        secureRandomBytes ?? _defaultSecureRandomBytes;
    final Uint8List verifierBytes = _exactRandomBytes(randomBytes, 32);
    final Uint8List stateBytes = _exactRandomBytes(randomBytes, 32);
    if (_bytesEqual(verifierBytes, stateBytes)) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.invalidRandomness,
        'Secure OAuth randomness is unavailable.',
      );
    }

    final String codeVerifier = _base64UrlWithoutPadding(verifierBytes);
    final String state = _base64UrlWithoutPadding(stateBytes);
    final String codeChallenge = _base64UrlWithoutPadding(
      Uint8List.fromList(sha256.convert(ascii.encode(codeVerifier)).bytes),
    );
    final Uri authorizationUri = configuration.baseUri.replace(
      path: '/oauth',
      queryParameters: <String, String>{
        'response_type': 'code',
        'client_id': configuration.clientId,
        'redirect_uri': configuration.redirectUri.toString(),
        'code_challenge_method': 'S256',
        'code_challenge': codeChallenge,
        'scope': LichessOAuthConfiguration.scope,
        'state': state,
      },
    );
    return LichessOAuthPendingAuthorization._(
      configuration: configuration,
      authorizationUri: authorizationUri,
      codeVerifier: codeVerifier,
      state: state,
    );
  }

  final LichessOAuthConfiguration configuration;
  final Uri authorizationUri;
  final String codeVerifier;
  final String _state;

  bool _completed = false;
  bool _exchangeCreated = false;

  LichessOAuthTokenExchange createTokenExchange(
    LichessOAuthAuthorizationCode result,
  ) {
    if (result._transactionBinding != _state) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.transactionMismatch,
        'The Lichess authorization code belongs to a different request.',
      );
    }
    if (_exchangeCreated) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.alreadyClaimed,
        'This Lichess authorization code was already claimed.',
      );
    }
    _exchangeCreated = true;
    return LichessOAuthTokenExchange._(
      configuration: configuration,
      authorizationCode: result.code,
      codeVerifier: codeVerifier,
    );
  }

  LichessOAuthCallbackResult complete(Uri callbackUri) {
    if (_completed) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.alreadyCompleted,
        'This Lichess authorization request is already complete.',
      );
    }
    if (callbackUri.toString().length > 8192 ||
        !_matchesRedirect(callbackUri, configuration.redirectUri)) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.redirectMismatch,
        'The Lichess authorization redirect did not match this app.',
      );
    }

    const Set<String> allowedParameters = <String>{
      'code',
      'error',
      'error_description',
      'state',
    };
    final Map<String, List<String>> parameters;
    try {
      parameters = callbackUri.queryParametersAll;
    } on FormatException {
      _invalidResponse();
    }
    if (parameters.keys.any(
      (String parameter) => !allowedParameters.contains(parameter),
    )) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.invalidResponse,
        'Lichess returned an invalid authorization response.',
      );
    }

    final List<String>? states = parameters['state'];
    if (states == null || states.length != 1) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.missingState,
        'Lichess authorization state was missing or ambiguous.',
      );
    }
    if (states.single != _state) {
      throw const LichessOAuthException(
        LichessOAuthFailureKind.stateMismatch,
        'Lichess authorization state did not match this request.',
      );
    }
    _completed = true;

    final List<String>? codes = parameters['code'];
    final List<String>? errors = parameters['error'];
    final List<String>? descriptions = parameters['error_description'];
    final bool validCode = codes == null || codes.length == 1;
    final bool validError = errors == null || errors.length == 1;
    final bool hasOneCode = codes?.length == 1;
    final bool hasOneError = errors?.length == 1;
    final bool validDescription =
        descriptions == null || descriptions.length == 1;
    final bool descriptionMatchesFailure = descriptions == null || hasOneError;
    if (!validCode ||
        !validError ||
        hasOneCode == hasOneError ||
        !validDescription ||
        !descriptionMatchesFailure) {
      _invalidResponse();
    }

    if (hasOneCode) {
      final String code = codes!.single;
      if (!_isBoundedPrintableAscii(code, maximumLength: 4096)) {
        _invalidResponse();
      }
      return LichessOAuthAuthorizationCode._(code, _state);
    }

    final String error = errors!.single;
    if (!_isBoundedPrintableAscii(error, maximumLength: 128) ||
        (descriptions != null && descriptions.single.length > 1024)) {
      _invalidResponse();
    }
    return LichessOAuthDenied._(
      error == 'access_denied'
          ? LichessOAuthDenialKind.accessDenied
          : LichessOAuthDenialKind.providerError,
    );
  }
}

final Random _secureRandom = Random.secure();

Uint8List _defaultSecureRandomBytes(int length) {
  return Uint8List.fromList(
    List<int>.generate(length, (_) => _secureRandom.nextInt(256)),
  );
}

Uint8List _exactRandomBytes(LichessSecureRandomBytes randomBytes, int length) {
  final Uint8List result;
  try {
    result = Uint8List.fromList(randomBytes(length));
  } catch (_) {
    throw const LichessOAuthException(
      LichessOAuthFailureKind.invalidRandomness,
      'Secure OAuth randomness is unavailable.',
    );
  }
  if (result.length != length) {
    throw const LichessOAuthException(
      LichessOAuthFailureKind.invalidRandomness,
      'Secure OAuth randomness is unavailable.',
    );
  }
  return result;
}

String _base64UrlWithoutPadding(Uint8List bytes) {
  return base64UrlEncode(bytes).replaceAll('=', '');
}

bool _bytesEqual(Uint8List first, Uint8List second) {
  if (first.length != second.length) {
    return false;
  }
  var difference = 0;
  for (var index = 0; index < first.length; index += 1) {
    difference |= first[index] ^ second[index];
  }
  return difference == 0;
}

bool _matchesRedirect(Uri actual, Uri expected) {
  return actual.scheme == expected.scheme &&
      actual.authority == expected.authority &&
      actual.path == expected.path &&
      !actual.hasFragment;
}

bool _isBoundedPrintableAscii(String value, {required int maximumLength}) {
  return value.isNotEmpty &&
      value.length <= maximumLength &&
      value.codeUnits.every((int unit) => unit >= 0x21 && unit <= 0x7e);
}

Never _invalidConfiguration() {
  throw const LichessOAuthException(
    LichessOAuthFailureKind.invalidConfiguration,
    'The Lichess OAuth configuration is invalid.',
  );
}

Never _invalidResponse() {
  throw const LichessOAuthException(
    LichessOAuthFailureKind.invalidResponse,
    'Lichess returned an invalid authorization response.',
  );
}
