import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/services/firmware_artifact_validator.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';

import 'support/firmware_test_fixture.dart';

// IOClient exposes decompressed bytes while preserving the wire Content-Length.
// HostMetro's real beta notes reproduced this as 399 encoded / 651 decoded bytes.
class DecodedGzipClient extends http.BaseClient {
  DecodedGzipClient(this.bodies);

  final Map<Uri, List<int>> bodies;

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    final List<int> body = bodies[request.url]!;
    final int encodedLength = gzip.encode(body).length;
    expect(encodedLength, isNot(body.length));
    return http.StreamedResponse(
      Stream<List<int>>.value(body),
      HttpStatus.ok,
      request: request,
      contentLength: encodedLength,
      headers: <String, String>{
        'content-encoding': 'gzip',
        'content-length': '$encodedLength',
      },
    );
  }
}

void main() {
  test(
    'loads compressed release details and verifies decoded firmware bytes',
    () async {
      final firmware = makeEsp32s3Firmware();
      const String notes =
          'Version: 0.6.0\n\nVerified official firmware release.\n';
      final manifest = makeFirmwareManifest(firmware, notesMarkdown: notes);
      final Directory cache = await Directory.systemTemp.createTemp(
        'litch-gzip-',
      );
      addTearDown(() => cache.delete(recursive: true));
      final service = FirmwareDownloadService(
        outputDirectory: cache,
        client: DecodedGzipClient(<Uri, List<int>>{
          FirmwareReleaseInfo.stable.manifestUrl: utf8.encode(
            makeFirmwareManifestJson(firmware, notesMarkdown: notes),
          ),
          manifest.notes.url: utf8.encode(notes),
          manifest.artifact.url: firmware,
        }),
      );
      addTearDown(service.dispose);

      final snapshot = await service.fetchReleaseSnapshot(
        FirmwareReleaseInfo.stable,
      );
      expect(snapshot.notesMarkdown, notes);
      final prepared = await service.prepareFirmwareForUpdate(snapshot);
      expect(prepared.bytes, firmware.length);
      expect(prepared.inspection.artifactSha256, manifest.artifact.sha256);
      expect(prepared.inspection.imageSha256, manifest.artifact.imageSha256);
      expect(
        prepared.inspection.bootPolicy.status,
        FirmwareBootPolicyStatus.known,
      );
      expect(prepared.inspection.bootPolicy.epoch, 2);
      expect(await prepared.file.readAsBytes(), firmware);
    },
  );
}
