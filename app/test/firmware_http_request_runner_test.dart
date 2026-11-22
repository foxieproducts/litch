import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/services/firmware_http_request_runner.dart';

class HangingSendClient extends http.BaseClient {
  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) {
    return Completer<http.StreamedResponse>().future;
  }
}

class HangingBodyClient extends http.BaseClient {
  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    return http.StreamedResponse(
      StreamController<List<int>>().stream,
      200,
      request: request,
    );
  }
}

class TrickleBodyClient extends http.BaseClient {
  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    Timer? timer;
    late final StreamController<List<int>> body;
    body = StreamController<List<int>>(
      onListen: () {
        timer = Timer.periodic(
          const Duration(milliseconds: 5),
          (_) => body.add(<int>[1]),
        );
      },
      onCancel: () => timer?.cancel(),
    );
    return http.StreamedResponse(body.stream, 200, request: request);
  }
}

void main() {
  const FirmwareRequestDeadlines shortDeadlines = FirmwareRequestDeadlines(
    connect: Duration(milliseconds: 10),
    bodyInactivity: Duration(milliseconds: 10),
    metadataOverall: Duration(milliseconds: 30),
    artifactOverall: Duration(milliseconds: 30),
  );

  test(
    'connect deadline returns when the HTTP client never responds',
    () async {
      final FirmwareHttpRequestRunner runner = FirmwareHttpRequestRunner(
        HangingSendClient(),
        deadlines: shortDeadlines,
      );

      await expectLater(
        runner
            .get(
              Uri.parse('https://foxielitch.com/fw/manifest.json'),
              kind: FirmwareRequestKind.metadata,
            )
            .timeout(const Duration(seconds: 1)),
        throwsA(isA<FirmwareHttpTimeoutException>()),
      );
    },
  );

  test('body deadline returns when the response stream stalls', () async {
    final FirmwareHttpRequestRunner runner = FirmwareHttpRequestRunner(
      HangingBodyClient(),
      deadlines: shortDeadlines,
    );
    final http.StreamedResponse response = await runner.get(
      Uri.parse('https://foxielitch.com/fw/version.md'),
      kind: FirmwareRequestKind.metadata,
    );

    await expectLater(
      response.stream.drain<void>().timeout(const Duration(seconds: 1)),
      throwsA(isA<FirmwareHttpTimeoutException>()),
    );
  });

  test('overall deadline stops a body that keeps trickling forever', () async {
    final FirmwareHttpRequestRunner runner = FirmwareHttpRequestRunner(
      TrickleBodyClient(),
      deadlines: shortDeadlines,
    );
    final http.StreamedResponse response = await runner.get(
      Uri.parse('https://foxielitch.com/fw/version.md'),
      kind: FirmwareRequestKind.metadata,
    );

    await expectLater(
      response.stream.drain<void>().timeout(const Duration(seconds: 1)),
      throwsA(isA<FirmwareHttpTimeoutException>()),
    );
  });
}
