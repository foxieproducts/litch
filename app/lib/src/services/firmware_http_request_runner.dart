import 'dart:async';

import 'package:http/http.dart' as http;

enum FirmwareRequestKind { metadata, artifact }

class FirmwareRequestDeadlines {
  const FirmwareRequestDeadlines({
    this.connect = const Duration(seconds: 10),
    this.bodyInactivity = const Duration(seconds: 15),
    this.metadataOverall = const Duration(seconds: 30),
    this.artifactOverall = const Duration(minutes: 3),
  });

  final Duration connect;
  final Duration bodyInactivity;
  final Duration metadataOverall;
  final Duration artifactOverall;

  Duration overallFor(FirmwareRequestKind kind) {
    return kind == FirmwareRequestKind.artifact
        ? artifactOverall
        : metadataOverall;
  }
}

class FirmwareHttpTimeoutException implements Exception {
  const FirmwareHttpTimeoutException(this.message);

  final String message;

  @override
  String toString() => message;
}

class FirmwareHttpRequestRunner {
  const FirmwareHttpRequestRunner(
    this._client, {
    this.deadlines = const FirmwareRequestDeadlines(),
  });

  final http.Client _client;
  final FirmwareRequestDeadlines deadlines;

  Future<http.StreamedResponse> get(
    Uri url, {
    required FirmwareRequestKind kind,
    Map<String, String> headers = const <String, String>{},
  }) async {
    final Completer<void> abort = Completer<void>();
    final Stopwatch elapsed = Stopwatch()..start();
    final Duration overall = deadlines.overallFor(kind);
    final http.AbortableRequest request =
        http.AbortableRequest('GET', url, abortTrigger: abort.future)
          ..followRedirects = false
          ..maxRedirects = 0
          ..headers.addAll(headers);

    final Duration connectLimit = deadlines.connect < overall
        ? deadlines.connect
        : overall;
    final http.StreamedResponse response = await _client
        .send(request)
        .timeout(
          connectLimit,
          onTimeout: () {
            _abort(abort);
            throw FirmwareHttpTimeoutException(
              'Connection timed out while fetching $url.',
            );
          },
        );
    final Duration remaining = overall - elapsed.elapsed;
    if (remaining <= Duration.zero) {
      _abort(abort);
      throw FirmwareHttpTimeoutException(
        'Request timed out while fetching $url.',
      );
    }

    return http.StreamedResponse(
      _boundedBody(
        response.stream,
        url: url,
        abort: abort,
        inactivityLimit: deadlines.bodyInactivity,
        overallLimit: remaining,
      ),
      response.statusCode,
      contentLength: response.contentLength,
      request: response.request,
      headers: response.headers,
      isRedirect: response.isRedirect,
      persistentConnection: response.persistentConnection,
      reasonPhrase: response.reasonPhrase,
    );
  }

  Stream<List<int>> _boundedBody(
    Stream<List<int>> source, {
    required Uri url,
    required Completer<void> abort,
    required Duration inactivityLimit,
    required Duration overallLimit,
  }) {
    late final StreamController<List<int>> controller;
    StreamSubscription<List<int>>? subscription;
    Timer? inactivityTimer;
    Timer? overallTimer;
    bool finished = false;

    void cancelTimers() {
      inactivityTimer?.cancel();
      overallTimer?.cancel();
    }

    void resetInactivityTimer() {
      inactivityTimer?.cancel();
      inactivityTimer = Timer(inactivityLimit, () {
        if (finished) {
          return;
        }
        finished = true;
        _abort(abort);
        cancelTimers();
        unawaited(subscription?.cancel());
        controller.addError(
          FirmwareHttpTimeoutException(
            'Response body stalled while fetching $url.',
          ),
        );
        unawaited(controller.close());
      });
    }

    void failOverall() {
      if (finished) {
        return;
      }
      finished = true;
      _abort(abort);
      cancelTimers();
      unawaited(subscription?.cancel());
      controller.addError(
        FirmwareHttpTimeoutException('Request timed out while fetching $url.'),
      );
      unawaited(controller.close());
    }

    controller = StreamController<List<int>>(
      onListen: () {
        overallTimer = Timer(overallLimit, failOverall);
        resetInactivityTimer();
        subscription = source.listen(
          (List<int> chunk) {
            if (finished) {
              return;
            }
            resetInactivityTimer();
            controller.add(chunk);
          },
          onError: (Object error, StackTrace stackTrace) {
            if (finished) {
              return;
            }
            finished = true;
            cancelTimers();
            controller.addError(error, stackTrace);
            unawaited(controller.close());
          },
          onDone: () {
            if (finished) {
              return;
            }
            finished = true;
            cancelTimers();
            unawaited(controller.close());
          },
        );
      },
      onPause: () {
        inactivityTimer?.cancel();
        subscription?.pause();
      },
      onResume: () {
        subscription?.resume();
        resetInactivityTimer();
      },
      onCancel: () async {
        finished = true;
        _abort(abort);
        cancelTimers();
        await subscription?.cancel();
      },
    );
    return controller.stream;
  }

  static void _abort(Completer<void> abort) {
    if (!abort.isCompleted) {
      abort.complete();
    }
  }
}
