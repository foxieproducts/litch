import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import 'lichess_board_api_events.dart';
import 'lichess_http.dart';
import 'lichess_ndjson_decoder.dart';
import 'lichess_playing_inventory.dart';
import 'lichess_protocol.dart';
import 'lichess_request_coordinator.dart';

export 'lichess_http.dart';
export 'lichess_playing_inventory.dart';

typedef LichessAccessTokenProvider = Future<String> Function();

final class LichessRealtimeSeek {
  factory LichessRealtimeSeek.casualRapid({
    required int minutes,
    required int incrementSeconds,
  }) {
    if (minutes < 0 || minutes > 180) {
      throw ArgumentError.value(minutes, 'minutes');
    }
    if (incrementSeconds < 0 || incrementSeconds > 180) {
      throw ArgumentError.value(incrementSeconds, 'incrementSeconds');
    }
    final int estimatedSeconds = (minutes * 60) + (incrementSeconds * 40);
    if (estimatedSeconds < 8 * 60 || estimatedSeconds >= 25 * 60) {
      throw ArgumentError(
        'The first Litch release supports casual Rapid seeks only.',
      );
    }
    return LichessRealtimeSeek._(
      minutes: minutes,
      incrementSeconds: incrementSeconds,
    );
  }

  const LichessRealtimeSeek._({
    required this.minutes,
    required this.incrementSeconds,
  });

  final int minutes;
  final int incrementSeconds;
}

final class LichessBoardApiClient {
  LichessBoardApiClient({
    required http.Client httpClient,
    required LichessAccessTokenProvider accessTokenProvider,
    required LichessRequestCoordinator requestCoordinator,
    Uri? baseUri,
    this.ndjsonDecoder = const LichessNdjsonDecoder(),
    this.requestStartTimeout = const Duration(seconds: 30),
    this.responseBodyTimeout = const Duration(seconds: 10),
    LichessRequestDeadline? requestDeadline,
    LichessResponseBodyDeadline? responseBodyDeadline,
  }) : _httpClient = httpClient,
       _accessTokenProvider = accessTokenProvider,
       _requestCoordinator = requestCoordinator,
       _baseUri = baseUri ?? Uri.parse('https://lichess.org'),
       _requestDeadline = requestDeadline ?? lichessDefaultRequestDeadline,
       _responseBodyDeadline =
           responseBodyDeadline ?? lichessDefaultResponseBodyDeadline,
       assert(requestStartTimeout > Duration.zero),
       assert(responseBodyTimeout > Duration.zero);

  /// Sized from a representative 50-game API fixture with room for additions.
  static const int maximumPlayingInventoryBytes = 64 * 1024;

  final http.Client _httpClient;
  final LichessAccessTokenProvider _accessTokenProvider;
  final LichessRequestCoordinator _requestCoordinator;
  final Uri _baseUri;
  final LichessRequestDeadline _requestDeadline;
  final LichessResponseBodyDeadline _responseBodyDeadline;
  final LichessNdjsonDecoder ndjsonDecoder;
  final Duration requestStartTimeout;
  final Duration responseBodyTimeout;

  final Set<Completer<void>> _activeAborts = <Completer<void>>{};
  bool _eventStreamActive = false;
  bool _seekStreamActive = false;
  Completer<void>? _seekAbort;
  Completer<void>? _gameAbort;
  bool _gameStreamActive = false;
  bool _closed = false;

  Stream<LichessAccountEvent> streamAccountEvents({
    void Function()? onOpened,
  }) async* {
    _ensureAvailable();
    if (_eventStreamActive) {
      throw const LichessApiException(
        LichessApiFailureKind.streamOwnership,
        'A Lichess account event stream is already active.',
      );
    }
    _eventStreamActive = true;
    _OpenLichessStream? openStream;
    try {
      openStream = await _openStream(
        method: 'GET',
        uri: _apiUri(<String>['stream', 'event']),
      );
      if (onOpened != null) {
        _ensureAvailable();
        onOpened();
        _ensureAvailable();
      }
      await for (final Map<String, Object?> record in ndjsonDecoder.decode(
        _redactedResponseBody(openStream.response.stream),
      )) {
        yield decodeLichessAccountEvent(record);
      }
    } finally {
      _finishStream(openStream);
      _eventStreamActive = false;
    }
  }

  Future<LichessPlayingInventory> fetchPlayingInventory() {
    _ensureAvailable();
    return _requestCoordinator.runFinite<LichessPlayingInventory>(() async {
      _ensureAvailable();
      final _PreparedLichessRequest prepared = await _authorizedRequest(
        'GET',
        _apiUri(<String>[
          'account',
          'playing',
        ]).replace(queryParameters: const <String, String>{'nb': '50'}),
      );
      try {
        final http.StreamedResponse response = await _send(prepared);
        if (response.statusCode != HttpStatus.ok) {
          await _checkResponse(response, expectedStatus: HttpStatus.ok);
        }
        final Uint8List body = await _readBoundedBody(
          response.stream,
          prepared,
        );
        final Object? decoded;
        try {
          decoded = jsonDecode(utf8.decode(body));
        } catch (_) {
          throw const LichessApiException(
            LichessApiFailureKind.invalidResponse,
            'Lichess returned an unusable current-game inventory.',
          );
        }
        final LichessPlayingInventory inventory = decodeLichessPlayingInventory(
          decoded,
        );
        _ensureAvailable();
        return inventory;
      } finally {
        _finishAbort(prepared.abort);
      }
    });
  }

  /// Ends matching without retiring the account or bound-game streams.
  void cancelSeek() {
    final abort = _seekAbort;
    if (abort != null && !abort.isCompleted) abort.complete();
  }

  Stream<void> seek(LichessRealtimeSeek seek) async* {
    _ensureAvailable();
    if (_seekStreamActive) {
      throw const LichessApiException(
        LichessApiFailureKind.streamOwnership,
        'A Lichess seek stream is already active.',
      );
    }
    _seekStreamActive = true;
    final seekAbort = Completer<void>();
    _seekAbort = seekAbort;
    _OpenLichessStream? openStream;
    try {
      openStream = await _openStream(
        method: 'POST',
        uri: _apiUri(<String>['board', 'seek']),
        requestAbort: seekAbort,
        formFields: <String, String>{
          'rated': 'false',
          'variant': 'standard',
          'time': seek.minutes.toString(),
          'increment': seek.incrementSeconds.toString(),
        },
      );
      await for (final Map<String, Object?> _ in ndjsonDecoder.decode(
        _redactedResponseBody(openStream.response.stream),
      )) {
        throw const LichessProtocolException(
          LichessProtocolFailureKind.invalidField,
          'The Lichess real-time seek stream contained unexpected data.',
        );
      }
    } finally {
      _finishStream(openStream);
      _finishAbort(seekAbort);
      if (identical(_seekAbort, seekAbort)) _seekAbort = null;
      _seekStreamActive = false;
    }
  }

  Stream<LichessGameStreamEvent> streamGame(String gameId) async* {
    _validateIdentifier(gameId, 'gameId');
    _ensureAvailable();
    if (_gameStreamActive) {
      throw const LichessApiException(
        LichessApiFailureKind.streamOwnership,
        'A Lichess game stream is already active.',
      );
    }
    _gameStreamActive = true;
    final gameAbort = Completer<void>();
    _gameAbort = gameAbort;
    _OpenLichessStream? openStream;
    bool receivedEvent = false;
    try {
      openStream = await _openStream(
        method: 'GET',
        uri: _apiUri(<String>['board', 'game', 'stream', gameId]),
        requestAbort: gameAbort,
      );
      final LichessGameStreamDecoder eventDecoder = LichessGameStreamDecoder();
      await for (final Map<String, Object?> record in ndjsonDecoder.decode(
        _redactedResponseBody(openStream.response.stream),
      )) {
        receivedEvent = true;
        yield eventDecoder.decode(record);
      }
      if (!receivedEvent) {
        throw const LichessProtocolException(
          LichessProtocolFailureKind.unexpectedFirstEvent,
          'The Lichess game stream closed before gameFull.',
        );
      }
    } finally {
      _finishStream(openStream);
      _finishAbort(gameAbort);
      if (identical(_gameAbort, gameAbort)) _gameAbort = null;
      _gameStreamActive = false;
    }
  }

  void cancelGameStream() {
    final abort = _gameAbort;
    if (abort != null && !abort.isCompleted) abort.complete();
  }

  Future<void> makeMove(
    String gameId,
    String uci, {
    bool offeringDraw = false,
    bool Function()? dispatchGuard,
  }) {
    _validateIdentifier(gameId, 'gameId');
    if (!_uciMove.hasMatch(uci)) {
      throw ArgumentError.value(uci, 'uci');
    }
    final Uri uri = _apiUri(<String>['board', 'game', gameId, 'move', uci])
        .replace(
          queryParameters: offeringDraw
              ? const <String, String>{'offeringDraw': 'true'}
              : null,
        );
    return _postCommand(uri, dispatchGuard: dispatchGuard);
  }

  Future<void> abort(String gameId) {
    return _postGameCommand(gameId, <String>['abort']);
  }

  Future<void> resign(String gameId) {
    return _postGameCommand(gameId, <String>['resign']);
  }

  Future<void> handleDraw(String gameId, {required bool accept}) {
    return _postGameCommand(gameId, <String>['draw', accept ? 'yes' : 'no']);
  }

  Future<void> handleTakeback(String gameId, {required bool accept}) {
    return _postGameCommand(gameId, <String>[
      'takeback',
      accept ? 'yes' : 'no',
    ]);
  }

  Future<void> claimVictory(String gameId) {
    return _postGameCommand(gameId, <String>['claim-victory']);
  }

  Future<void> claimDraw(String gameId) {
    return _postGameCommand(gameId, <String>['claim-draw']);
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

  Future<void> _postGameCommand(String gameId, List<String> suffix) {
    _validateIdentifier(gameId, 'gameId');
    return _postCommand(_apiUri(<String>['board', 'game', gameId, ...suffix]));
  }

  Future<void> _postCommand(Uri uri, {bool Function()? dispatchGuard}) {
    _ensureAvailable();
    return _requestCoordinator.runFinite<void>(() async {
      _ensureAvailable();
      final _PreparedLichessRequest prepared = await _authorizedRequest(
        'POST',
        uri,
      );
      final Completer<void> abort = prepared.abort;
      try {
        if (dispatchGuard != null && !dispatchGuard()) {
          throw const LichessApiException(
            LichessApiFailureKind.streamOwnership,
            'The move is no longer admitted by current provider state.',
          );
        }
        final http.StreamedResponse response = await _send(prepared);
        await _checkResponse(response, expectedStatus: HttpStatus.ok);
      } finally {
        _finishAbort(abort);
      }
    });
  }

  Future<_OpenLichessStream> _openStream({
    required String method,
    required Uri uri,
    Map<String, String>? formFields,
    Completer<void>? requestAbort,
  }) {
    return _requestCoordinator.startStream<_OpenLichessStream>(() async {
      _ensureAvailable();
      final _PreparedLichessRequest prepared = await _authorizedRequest(
        method,
        uri,
        formFields: formFields,
        requestAbort: requestAbort,
      );
      final Completer<void> abort = prepared.abort;
      try {
        if (abort.isCompleted) {
          throw const LichessApiException(
            LichessApiFailureKind.transport,
            'Lichess matching was cancelled.',
          );
        }
        final pending = _send(prepared);
        // A late handshake belongs to this request even after matching ended.
        // Drain it without touching any later seek's cancellation handle.
        if (requestAbort != null) {
          unawaited(
            pending.then<void>((response) async {
              if (abort.isCompleted) {
                try {
                  await _discardBody(response);
                } catch (_) {
                  // Late response has no owner; cleanup cannot affect a new seek.
                }
              }
            }, onError: (Object _) {}),
          );
        }
        final http.StreamedResponse response = requestAbort == null
            ? await pending
            : await Future.any<http.StreamedResponse>([
                pending,
                abort.future.then<http.StreamedResponse>((_) {
                  throw const LichessApiException(
                    LichessApiFailureKind.transport,
                    'Lichess matching was cancelled.',
                  );
                }),
              ]);
        if (response.statusCode != HttpStatus.ok) {
          await _checkResponse(response, expectedStatus: HttpStatus.ok);
        }
        return _OpenLichessStream(response: response, abort: abort);
      } catch (_) {
        _finishAbort(abort);
        rethrow;
      }
    });
  }

  Future<_PreparedLichessRequest> _authorizedRequest(
    String method,
    Uri uri, {
    Map<String, String>? formFields,
    Completer<void>? requestAbort,
  }) async {
    final Completer<void> abort = requestAbort ?? Completer<void>();
    if (abort.isCompleted) {
      throw const LichessApiException(
        LichessApiFailureKind.disposed,
        'The Lichess request was cancelled.',
      );
    }
    _activeAborts.add(abort);
    final String token;
    try {
      final Future<String> tokenFuture = _accessTokenProvider();
      token = await Future.any<String>(<Future<String>>[
        tokenFuture,
        abort.future.then<String>((void _) {
          throw const LichessApiException(
            LichessApiFailureKind.disposed,
            'The Lichess client is closed.',
          );
        }),
      ]);
    } catch (_) {
      _finishAbort(abort);
      if (_closed) {
        throw const LichessApiException(
          LichessApiFailureKind.disposed,
          'The Lichess client is closed.',
        );
      }
      throw const LichessApiException(
        LichessApiFailureKind.unavailableCredentials,
        'Lichess authorization is unavailable.',
      );
    }
    try {
      _ensureAvailable();
      if (token.isEmpty ||
          token.length > 4096 ||
          token.codeUnits.any((int unit) => unit <= 0x20 || unit >= 0x7f)) {
        throw const LichessApiException(
          LichessApiFailureKind.unavailableCredentials,
          'Lichess authorization is unavailable.',
        );
      }

      final http.AbortableRequest request = http.AbortableRequest(
        method,
        uri,
        abortTrigger: abort.future,
      )..followRedirects = false;
      request.headers.addAll(<String, String>{
        HttpHeaders.authorizationHeader: 'Bearer $token',
        HttpHeaders.acceptHeader: 'application/x-ndjson, application/json',
      });
      if (formFields != null) {
        request.bodyFields = formFields;
      }
      return _PreparedLichessRequest(request: request, abort: abort);
    } catch (_) {
      _finishAbort(abort);
      rethrow;
    }
  }

  Future<http.StreamedResponse> _send(_PreparedLichessRequest prepared) async {
    _ensureAvailable();
    try {
      final http.StreamedResponse? response = await _requestDeadline(
        _httpClient.send(prepared.request),
        requestStartTimeout,
      );
      if (response == null) {
        if (!prepared.abort.isCompleted) {
          prepared.abort.complete();
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

  Stream<List<int>> _redactedResponseBody(Stream<List<int>> source) async* {
    try {
      await for (final List<int> chunk in source) {
        yield chunk;
      }
    } catch (_) {
      if (_closed) {
        throw const LichessApiException(
          LichessApiFailureKind.disposed,
          'The Lichess client is closed.',
        );
      }
      throw const LichessApiException(
        LichessApiFailureKind.transport,
        'The Lichess response stream was interrupted.',
      );
    }
  }

  Future<Uint8List> _readBoundedBody(
    Stream<List<int>> source,
    _PreparedLichessRequest prepared,
  ) {
    return lichessReadBoundedResponseBody(
      source,
      maximumBytes: maximumPlayingInventoryBytes,
      timeout: responseBodyTimeout,
      deadline: _responseBodyDeadline,
      isClosed: () => _closed,
      abort: () {
        if (!prepared.abort.isCompleted) {
          prepared.abort.complete();
        }
      },
      oversizedMessage: 'Lichess returned an oversized current-game inventory.',
      deadlineMessage:
          'Lichess did not complete the current-game inventory before the deadline.',
    );
  }

  Future<void> _checkResponse(
    http.StreamedResponse response, {
    required int expectedStatus,
  }) async {
    final int statusCode = response.statusCode;
    if (statusCode == expectedStatus) {
      await _discardBody(response);
      return;
    }
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

  Future<void> _discardBody(http.StreamedResponse response) async {
    final StreamSubscription<List<int>> subscription = response.stream.listen(
      (_) {},
      onError: (_) {},
    );
    await subscription.cancel();
  }

  void _finishStream(_OpenLichessStream? openStream) {
    if (openStream != null) {
      _finishAbort(openStream.abort);
    }
  }

  void _finishAbort(Completer<void> abort) {
    _activeAborts.remove(abort);
    if (!abort.isCompleted) {
      abort.complete();
    }
  }

  Uri _apiUri(List<String> segments) {
    return _baseUri.replace(
      pathSegments: <String>[
        ..._baseUri.pathSegments.where((String segment) => segment.isNotEmpty),
        'api',
        ...segments,
      ],
      query: null,
      fragment: null,
    );
  }

  void _ensureAvailable() {
    if (_closed) {
      throw const LichessApiException(
        LichessApiFailureKind.disposed,
        'The Lichess client is closed.',
      );
    }
  }

  static void _validateIdentifier(String value, String name) {
    if (!_identifier.hasMatch(value)) {
      throw ArgumentError.value(value, name);
    }
  }

  static final RegExp _identifier = RegExp(r'^[A-Za-z0-9]{1,64}$');
  static final RegExp _uciMove = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');
}

final class _OpenLichessStream {
  const _OpenLichessStream({required this.response, required this.abort});

  final http.StreamedResponse response;
  final Completer<void> abort;
}

final class _PreparedLichessRequest {
  const _PreparedLichessRequest({required this.request, required this.abort});

  final http.AbortableRequest request;
  final Completer<void> abort;
}
