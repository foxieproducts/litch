import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/services/firmware_http_request_runner.dart';
import 'package:litch_mobile/src/services/firmware_manifest_client.dart';

import 'support/firmware_test_fixture.dart';

class NeverSendingManifestClient extends http.BaseClient {
  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) {
    return Completer<http.StreamedResponse>().future;
  }
}

void main() {
  test('fetches and parses the selected channel manifest', () async {
    final Uint8List firmware = makeEsp32s3Firmware();
    final MockClient httpClient = MockClient((http.Request request) async {
      expect(request.url, FirmwareReleaseInfo.stable.manifestUrl);
      return http.Response(
        makeFirmwareManifestJson(firmware),
        200,
        request: request,
      );
    });
    final FirmwareManifestClient client = FirmwareManifestClient(httpClient);

    final FirmwareManifest manifest = await client.fetch(
      FirmwareReleaseInfo.stable,
    );

    expect(manifest.channel, 'stable');
    expect(manifest.artifact.sizeBytes, firmware.length);
  });

  test('does not follow manifest redirects', () async {
    final MockClient httpClient = MockClient((http.Request request) async {
      expect(request.followRedirects, isFalse);
      return http.Response(
        '',
        302,
        headers: <String, String>{
          'location': 'https://updates.example/manifest.json',
        },
        request: request,
      );
    });
    final FirmwareManifestClient client = FirmwareManifestClient(httpClient);

    await expectLater(
      client.fetch(FirmwareReleaseInfo.stable),
      throwsA(isA<FirmwareManifestFetchException>()),
    );
  });

  test('rejects oversized manifest bodies', () async {
    final MockClient httpClient = MockClient((http.Request request) async {
      return http.Response(
        List<String>.filled(32 * 1024 + 1, 'x').join(),
        200,
        request: request,
      );
    });
    final FirmwareManifestClient client = FirmwareManifestClient(httpClient);

    await expectLater(
      client.fetch(FirmwareReleaseInfo.stable),
      throwsA(isA<FirmwareManifestFetchException>()),
    );
  });

  test(
    'returns after the connect deadline when manifest fetch stalls',
    () async {
      final FirmwareManifestClient client = FirmwareManifestClient(
        NeverSendingManifestClient(),
        deadlines: const FirmwareRequestDeadlines(
          connect: Duration(milliseconds: 10),
          bodyInactivity: Duration(milliseconds: 10),
          metadataOverall: Duration(milliseconds: 30),
          artifactOverall: Duration(milliseconds: 30),
        ),
      );

      await expectLater(
        client
            .fetch(FirmwareReleaseInfo.stable)
            .timeout(const Duration(seconds: 1)),
        throwsA(isA<FirmwareManifestFetchException>()),
      );
    },
  );
}
