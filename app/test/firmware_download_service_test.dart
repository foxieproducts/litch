import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/models/firmware_release_snapshot.dart';
import 'package:litch_mobile/src/services/firmware_artifact_validator.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/firmware_http_request_runner.dart';

import 'support/firmware_test_fixture.dart';

class HangingArtifactClient extends http.BaseClient {
  HangingArtifactClient(this.bytes);

  final Uint8List bytes;

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    final StreamController<List<int>> body = StreamController<List<int>>();
    body.add(bytes.sublist(0, bytes.length ~/ 2));
    return http.StreamedResponse(
      body.stream,
      200,
      contentLength: bytes.length,
      request: request,
    );
  }
}

void main() {
  final corpus =
      jsonDecode(
            File(
              '../test/fixtures/firmware_boot_policy.json',
            ).readAsStringSync(),
          )
          as Map<String, dynamic>;
  final policyCases = (corpus['cases'] as List<dynamic>)
      .cast<Map<String, dynamic>>();
  final refusedPolicies = <String, Uint8List>{
    for (final name in [
      'record_policy_1',
      'legacy_absent',
      'reserved_nonzero',
      'unknown_epoch',
    ])
      name: _bytesFromHex(
        policyCases.firstWhere((entry) => entry['name'] == name)['image_hex']
            as String,
      ),
    'unavailable': makeEsp32s3Firmware(segmentBytes: [1, 2, 3, 4]),
  };
  for (final entry in refusedPolicies.entries) {
    for (final origin in ['fresh', 'cache', 'part', 'previous']) {
      test('selected policy refuses ${entry.key} from $origin', () async {
        final directory = await Directory.systemTemp.createTemp(
          'litch-selected-policy-',
        );
        addTearDown(() => directory.delete(recursive: true));
        final priorBytes = makeEsp32s3Firmware();
        final priorCache = await File(
          '${directory.path}/${FirmwareReleaseInfo.stable.fileName}',
        ).writeAsBytes(priorBytes, flush: true);
        final manifest = makeFirmwareManifest(entry.value);
        final inspection = FirmwareArtifactValidator.inspectBytes(
          entry.value,
          manifest,
        );
        expect(
          inspection.bootPolicy.status == FirmwareBootPolicyStatus.known &&
              inspection.bootPolicy.epoch == 2,
          isFalse,
        );
        final selected = File(
          '${directory.path}/firmware-stable-${manifest.artifact.sha256}.bin',
        );
        File? retained;
        if (origin != 'fresh') {
          retained = switch (origin) {
            'cache' => selected,
            'part' => File('${selected.path}.part'),
            _ => File(
              '${directory.path}/${FirmwareReleaseInfo.stable.fileName}.previous',
            ),
          };
          await retained.writeAsBytes(entry.value, flush: true);
        }
        int artifactGets = 0;
        final service = FirmwareDownloadService(
          outputDirectory: directory,
          client: MockClient((request) async {
            expect(request.url, manifest.artifact.url);
            ++artifactGets;
            return http.Response.bytes(entry.value, 200, request: request);
          }),
        );
        addTearDown(service.dispose);

        final Matcher rejection = origin == 'fresh'
            ? isA<FirmwareFetchException>()
                  .having(
                    (error) => error.message,
                    'message',
                    contains('SDK-only boot policy'),
                  )
                  .having(
                    (error) => error.cause,
                    'cause',
                    isA<FirmwareArtifactValidationException>(),
                  )
            : isA<FirmwareArtifactValidationException>().having(
                (error) => error.message,
                'message',
                contains('SDK-only boot policy'),
              );
        await expectLater(
          service.prepareFirmwareForUpdate(
            FirmwareReleaseSnapshot(
              release: FirmwareReleaseInfo.stable,
              manifest: manifest,
              notesMarkdown: 'Version: 0.6.0\n',
            ),
          ),
          throwsA(rejection),
        );
        expect(artifactGets, origin == 'fresh' ? 1 : 0);
        expect(await priorCache.readAsBytes(), priorBytes);
        if (retained != null) {
          expect(await retained.readAsBytes(), entry.value);
        }
        if (origin != 'cache') expect(selected.existsSync(), isFalse);
        if (origin != 'part') {
          expect(File('${selected.path}.part').existsSync(), isFalse);
        }
      });
    }
  }

  for (final (FirmwareReleaseInfo release, String manifestName)
      in <(FirmwareReleaseInfo, String)>[
        (FirmwareReleaseInfo.stable, 'manifest.json'),
        (FirmwareReleaseInfo.beta, 'manifest_beta.json'),
      ]) {
    test(
      'downloads verified ${release.channel.name} firmware from the update host',
      () async {
        final Directory cacheDirectory = await Directory.systemTemp.createTemp(
          'litch-firmware-public-host-test-',
        );
        addTearDown(() => cacheDirectory.delete(recursive: true));
        final Uint8List firmware = makeEsp32s3Firmware();
        const String notes = 'Version: 0.6.0\n\nExact release notes.';
        final Uri manifestUrl = Uri.parse(
          'https://updates.foxieproducts.com/fw/$manifestName',
        );
        final FirmwareManifest manifest = makeFirmwareManifest(
          firmware,
          channel: release.channel.name,
          manifestUrl: manifestUrl,
          notesMarkdown: notes,
        );
        final String manifestJson = makeFirmwareManifestJson(
          firmware,
          channel: release.channel.name,
          notesMarkdown: notes,
        );
        bool catalogCorrected = false;
        final List<Uri> requests = <Uri>[];
        final MockClient httpClient = MockClient((http.Request request) async {
          requests.add(request.url);
          if (request.url == manifestUrl) {
            return http.Response(
              catalogCorrected
                  ? manifestJson
                  : manifestJson.replaceAll(
                      'https://updates.foxieproducts.com/fw/',
                      'https://foxielitch.com/fw/',
                    ),
              200,
              request: request,
            );
          }
          if (request.url == manifest.notes.url) {
            return http.Response(notes, 200, request: request);
          }
          if (request.url == manifest.artifact.url) {
            return http.Response.bytes(firmware, 200, request: request);
          }
          return http.Response('not found', 404, request: request);
        });
        final FirmwareDownloadService service = FirmwareDownloadService(
          client: httpClient,
          outputDirectory: cacheDirectory,
        );
        addTearDown(service.dispose);

        await expectLater(
          service.fetchReleaseSnapshot(release),
          throwsA(isA<FirmwareFetchException>()),
        );
        expect(requests, <Uri>[manifestUrl]);
        catalogCorrected = true;
        requests.clear();

        final snapshot = await service.fetchReleaseSnapshot(release);
        final FirmwarePreparedFile prepared = await service
            .prepareFirmwareForUpdate(snapshot);

        expect(snapshot.release.channel, release.channel);
        expect(snapshot.manifest.version, '0.6.0');
        expect(snapshot.notesMarkdown, notes);
        expect(await prepared.file.readAsBytes(), firmware);
        _expectSelectedInspection(prepared, manifest);
        expect(requests, <Uri>[
          manifestUrl,
          manifest.notes.url,
          manifest.artifact.url,
        ]);
        expect(
          requests.every((Uri url) => url.host == 'updates.foxieproducts.com'),
          isTrue,
        );
      },
    );
  }

  test('rejects notes whose bytes do not match the manifest', () async {
    final Uint8List firmware = makeEsp32s3Firmware();
    const String notes = 'Version: 0.6.0\n\nRelease notes A.';
    const String tamperedNotes = 'Version: 0.6.0\n\nRelease notes B.';
    final FirmwareManifest manifest = makeFirmwareManifest(
      firmware,
      notesMarkdown: notes,
    );
    final FirmwareDownloadService service = FirmwareDownloadService(
      client: MockClient((http.Request request) async {
        if (request.url == FirmwareReleaseInfo.stable.manifestUrl) {
          return http.Response(
            makeFirmwareManifestJson(firmware, notesMarkdown: notes),
            200,
            request: request,
          );
        }
        if (request.url == manifest.notes.url) {
          return http.Response(tamperedNotes, 200, request: request);
        }
        return http.Response('not found', 404, request: request);
      }),
    );
    addTearDown(service.dispose);

    await expectLater(
      service.fetchReleaseSnapshot(FirmwareReleaseInfo.stable),
      throwsA(isA<FirmwareFetchException>()),
    );
  });

  test(
    'prepares the exact snapshot without refetching a changed manifest',
    () async {
      final Directory cacheDirectory = await Directory.systemTemp.createTemp(
        'litch-firmware-snapshot-test-',
      );
      addTearDown(() => cacheDirectory.delete(recursive: true));
      final Uint8List consentedFirmware = makeEsp32s3Firmware(
        payloadBytes: <int>[1, 2, 3, 4],
      );
      final Uint8List laterFirmware = makeEsp32s3Firmware(
        payloadBytes: <int>[5, 6, 7, 8],
      );
      const String notes = 'Version: 0.6.0\n';
      final FirmwareManifest consentedManifest = makeFirmwareManifest(
        consentedFirmware,
        notesMarkdown: notes,
      );
      int manifestRequests = 0;
      final MockClient httpClient = MockClient((http.Request request) async {
        if (request.url == FirmwareReleaseInfo.stable.manifestUrl) {
          manifestRequests += 1;
          final Uint8List selected = manifestRequests == 1
              ? consentedFirmware
              : laterFirmware;
          return http.Response(
            makeFirmwareManifestJson(selected, notesMarkdown: notes),
            200,
            request: request,
          );
        }
        if (request.url == consentedManifest.notes.url) {
          return http.Response(notes, 200, request: request);
        }
        if (request.url == consentedManifest.artifact.url) {
          return http.Response.bytes(consentedFirmware, 200, request: request);
        }
        return http.Response('not found', 404, request: request);
      });
      final FirmwareDownloadService service = FirmwareDownloadService(
        client: httpClient,
        outputDirectory: cacheDirectory,
      );
      addTearDown(service.dispose);

      final snapshot = await service.fetchReleaseSnapshot(
        FirmwareReleaseInfo.stable,
      );
      final FirmwarePreparedFile prepared = await service
          .prepareFirmwareForUpdate(snapshot);

      expect(manifestRequests, 1);
      expect(await prepared.file.readAsBytes(), consentedFirmware);
    },
  );

  test('invalid fresh download preserves the prior valid cache', () async {
    final Directory cacheDirectory = await Directory.systemTemp.createTemp(
      'litch-firmware-cache-test-',
    );
    addTearDown(() => cacheDirectory.delete(recursive: true));

    final Uint8List priorFirmware = makeEsp32s3Firmware(
      payloadBytes: <int>[1, 2, 3, 4],
    );
    final File cachedFile = File(
      '${cacheDirectory.path}/${FirmwareReleaseInfo.stable.fileName}',
    );
    await cachedFile.writeAsBytes(priorFirmware, flush: true);

    final Uint8List publishedFirmware = makeEsp32s3Firmware(
      payloadBytes: <int>[5, 6, 7, 8],
    );
    final Uint8List corruptDownload = Uint8List.fromList(publishedFirmware);
    corruptDownload[corruptDownload.length - 1] ^= 1;
    final Uri artifactUrl = makeFirmwareManifest(
      publishedFirmware,
    ).artifact.url;
    final FirmwareReleaseSnapshot snapshot = FirmwareReleaseSnapshot(
      release: FirmwareReleaseInfo.stable,
      manifest: makeFirmwareManifest(publishedFirmware),
      notesMarkdown: 'Version: 0.6.0\n',
    );
    int artifactRequests = 0;
    final MockClient httpClient = MockClient((http.Request request) async {
      if (request.url == FirmwareReleaseInfo.stable.manifestUrl) {
        return http.Response(
          makeFirmwareManifestJson(publishedFirmware),
          200,
          request: request,
        );
      }
      if (request.url == artifactUrl) {
        artifactRequests += 1;
        return http.Response.bytes(corruptDownload, 200, request: request);
      }
      return http.Response('not found', 404, request: request);
    });
    final FirmwareDownloadService service = FirmwareDownloadService(
      client: httpClient,
      outputDirectory: cacheDirectory,
    );

    await expectLater(
      service.prepareFirmwareForUpdate(snapshot),
      throwsA(isA<FirmwareFetchException>()),
    );

    expect(await cachedFile.readAsBytes(), priorFirmware);
    expect(artifactRequests, 1);
    expect(File('${cachedFile.path}.part').existsSync(), isFalse);
  });

  test(
    'verified download is promoted once and then reused from cache',
    () async {
      final Directory cacheDirectory = await Directory.systemTemp.createTemp(
        'litch-firmware-cache-test-',
      );
      addTearDown(() => cacheDirectory.delete(recursive: true));

      final Uint8List publishedFirmware = makeEsp32s3Firmware();
      final Uri artifactUrl = makeFirmwareManifest(
        publishedFirmware,
      ).artifact.url;
      final FirmwareReleaseSnapshot snapshot = FirmwareReleaseSnapshot(
        release: FirmwareReleaseInfo.stable,
        manifest: makeFirmwareManifest(publishedFirmware),
        notesMarkdown: 'Version: 0.6.0\n',
      );
      int manifestRequests = 0;
      int artifactRequests = 0;
      final MockClient httpClient = MockClient((http.Request request) async {
        if (request.url == FirmwareReleaseInfo.stable.manifestUrl) {
          manifestRequests += 1;
          return http.Response(
            makeFirmwareManifestJson(publishedFirmware),
            200,
            request: request,
          );
        }
        if (request.url == artifactUrl) {
          artifactRequests += 1;
          return http.Response.bytes(publishedFirmware, 200, request: request);
        }
        return http.Response('not found', 404, request: request);
      });
      final FirmwareDownloadService service = FirmwareDownloadService(
        client: httpClient,
        outputDirectory: cacheDirectory,
      );

      final FirmwarePreparedFile first = await service.prepareFirmwareForUpdate(
        snapshot,
      );
      final FirmwarePreparedFile second = await service
          .prepareFirmwareForUpdate(snapshot);

      expect(first.downloaded, isTrue);
      expect(second.downloaded, isFalse);
      _expectSelectedInspection(first, snapshot.manifest);
      _expectSelectedInspection(second, snapshot.manifest);
      expect(first.file.path, second.file.path);
      expect(
        first.file.path,
        endsWith('firmware-stable-${snapshot.manifest.artifact.sha256}.bin'),
      );
      expect(await second.file.readAsBytes(), publishedFirmware);
      expect(manifestRequests, 0);
      expect(artifactRequests, 1);
      expect(File('${second.file.path}.part').existsSync(), isFalse);
      expect(File('${second.file.path}.previous').existsSync(), isFalse);
    },
  );

  test('promotes a complete verified part after an interrupted write', () async {
    final Directory cacheDirectory = await Directory.systemTemp.createTemp(
      'litch-firmware-part-recovery-test-',
    );
    addTearDown(() => cacheDirectory.delete(recursive: true));

    final Uint8List publishedFirmware = makeEsp32s3Firmware();
    final FirmwareManifest manifest = makeFirmwareManifest(publishedFirmware);
    final FirmwareReleaseSnapshot snapshot = FirmwareReleaseSnapshot(
      release: FirmwareReleaseInfo.stable,
      manifest: manifest,
      notesMarkdown: 'Version: 0.6.0\n',
    );
    final File expectedFile = File(
      '${cacheDirectory.path}/firmware-stable-${manifest.artifact.sha256}.bin',
    );
    final File partialFile = File('${expectedFile.path}.part');
    await partialFile.writeAsBytes(publishedFirmware, flush: true);
    int artifactRequests = 0;
    final FirmwareDownloadService service = FirmwareDownloadService(
      client: MockClient((http.Request request) async {
        artifactRequests += 1;
        return http.Response('unexpected', 500, request: request);
      }),
      outputDirectory: cacheDirectory,
    );
    addTearDown(service.dispose);

    final FirmwarePreparedFile prepared = await service
        .prepareFirmwareForUpdate(snapshot);

    expect(prepared.downloaded, isFalse);
    _expectSelectedInspection(prepared, manifest);
    expect(prepared.file.path, expectedFile.path);
    expect(await expectedFile.readAsBytes(), publishedFirmware);
    expect(partialFile.existsSync(), isFalse);
    expect(artifactRequests, 0);
  });

  test('recovers a verified legacy backup without using the network', () async {
    final Directory cacheDirectory = await Directory.systemTemp.createTemp(
      'litch-firmware-legacy-recovery-test-',
    );
    addTearDown(() => cacheDirectory.delete(recursive: true));

    final Uint8List publishedFirmware = makeEsp32s3Firmware();
    final FirmwareManifest manifest = makeFirmwareManifest(publishedFirmware);
    final FirmwareReleaseSnapshot snapshot = FirmwareReleaseSnapshot(
      release: FirmwareReleaseInfo.stable,
      manifest: manifest,
      notesMarkdown: 'Version: 0.6.0\n',
    );
    final File legacyBackup = File(
      '${cacheDirectory.path}/${FirmwareReleaseInfo.stable.fileName}.previous',
    );
    await legacyBackup.writeAsBytes(publishedFirmware, flush: true);
    int artifactRequests = 0;
    final FirmwareDownloadService service = FirmwareDownloadService(
      client: MockClient((http.Request request) async {
        artifactRequests += 1;
        return http.Response('unexpected', 500, request: request);
      }),
      outputDirectory: cacheDirectory,
    );
    addTearDown(service.dispose);

    final FirmwarePreparedFile prepared = await service
        .prepareFirmwareForUpdate(snapshot);

    expect(prepared.downloaded, isFalse);
    _expectSelectedInspection(prepared, manifest);
    expect(
      prepared.file.path,
      endsWith('firmware-stable-${manifest.artifact.sha256}.bin'),
    );
    expect(await prepared.file.readAsBytes(), publishedFirmware);
    expect(artifactRequests, 0);
  });

  test(
    'artifact body deadline cleans partial and preserves prior cache',
    () async {
      final Directory cacheDirectory = await Directory.systemTemp.createTemp(
        'litch-firmware-timeout-test-',
      );
      addTearDown(() => cacheDirectory.delete(recursive: true));
      final Uint8List priorFirmware = makeEsp32s3Firmware(
        payloadBytes: <int>[1, 2, 3, 4],
      );
      final File priorCache = File(
        '${cacheDirectory.path}/${FirmwareReleaseInfo.stable.fileName}',
      );
      await priorCache.writeAsBytes(priorFirmware, flush: true);
      final Uint8List firmware = makeEsp32s3Firmware(
        payloadBytes: <int>[5, 6, 7, 8],
      );
      final FirmwareManifest manifest = makeFirmwareManifest(firmware);
      final FirmwareReleaseSnapshot snapshot = FirmwareReleaseSnapshot(
        release: FirmwareReleaseInfo.stable,
        manifest: manifest,
        notesMarkdown: 'Version: 0.6.0\n',
      );
      final FirmwareDownloadService service = FirmwareDownloadService(
        client: HangingArtifactClient(firmware),
        outputDirectory: cacheDirectory,
        deadlines: const FirmwareRequestDeadlines(
          connect: Duration(milliseconds: 10),
          bodyInactivity: Duration(milliseconds: 10),
          metadataOverall: Duration(milliseconds: 30),
          artifactOverall: Duration(milliseconds: 30),
        ),
      );
      addTearDown(service.dispose);

      await expectLater(
        service
            .prepareFirmwareForUpdate(snapshot)
            .timeout(const Duration(seconds: 1)),
        throwsA(isA<FirmwareFetchException>()),
      );

      final File expectedCache = File(
        '${cacheDirectory.path}/firmware-stable-'
        '${manifest.artifact.sha256}.bin',
      );
      expect(File('${expectedCache.path}.part').existsSync(), isFalse);
      expect(await priorCache.readAsBytes(), priorFirmware);
    },
  );
}

Uint8List _bytesFromHex(String text) => Uint8List.fromList([
  for (int offset = 0; offset < text.length; offset += 2)
    int.parse(text.substring(offset, offset + 2), radix: 16),
]);

void _expectSelectedInspection(
  FirmwarePreparedFile prepared,
  FirmwareManifest manifest,
) {
  expect(prepared.inspection.sizeBytes, manifest.artifact.sizeBytes);
  expect(prepared.inspection.artifactSha256, manifest.artifact.sha256);
  expect(prepared.inspection.imageSha256, manifest.artifact.imageSha256);
  expect(prepared.inspection.bootPolicy.status, FirmwareBootPolicyStatus.known);
  expect(prepared.inspection.bootPolicy.schema, 1);
  expect(prepared.inspection.bootPolicy.epoch, 2);
}
