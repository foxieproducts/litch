import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/services/firmware_artifact_validator.dart';

import 'support/firmware_test_fixture.dart';

void main() {
  final Map<String, dynamic> corpus =
      jsonDecode(
            File(
              '../test/fixtures/firmware_boot_policy.json',
            ).readAsStringSync(),
          )
          as Map<String, dynamic>;
  final List<dynamic> cases = corpus['cases'] as List<dynamic>;

  for (final Map<String, dynamic> entry in cases.cast<Map<String, dynamic>>()) {
    test('verified boot policy corpus: ${entry['name']}', () {
      final Uint8List firmware = _bytesFromHex(entry['image_hex'] as String);
      final FirmwareManifest manifest = makeFirmwareManifest(firmware);

      if (entry['valid_integrity'] == false) {
        FirmwareArtifactInspection? inspection;
        expect(
          () => inspection = FirmwareArtifactValidator.inspectBytes(
            firmware,
            manifest,
          ),
          throwsA(isA<FirmwareArtifactValidationException>()),
        );
        expect(inspection, isNull);
        expect(
          () => FirmwareArtifactValidator.validateBytes(firmware, manifest),
          throwsA(isA<FirmwareArtifactValidationException>()),
        );
        return;
      }

      final FirmwareArtifactInspection inspection =
          FirmwareArtifactValidator.inspectBytes(firmware, manifest);
      FirmwareArtifactValidator.validateBytes(firmware, manifest);
      final Map<String, dynamic> expected =
          entry['expected'] as Map<String, dynamic>;
      expect(inspection.bootPolicy.status.name, expected['status']);
      expect(inspection.bootPolicy.schema, expected['schema']);
      expect(inspection.bootPolicy.sizeBytes, expected['size']);
      expect(inspection.bootPolicy.epoch, expected['epoch']);
      expect(inspection.sizeBytes, firmware.length);
      expect(inspection.artifactSha256, entry['artifact_sha256']);
      expect(inspection.imageSha256, entry['image_sha256']);
      if (expected['status'] == 'known' && expected['epoch'] == 2) {
        FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
      } else {
        expect(
          () => FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection),
          throwsA(
            isA<FirmwareArtifactValidationException>().having(
              (error) => error.message,
              'message',
              contains('SDK-only boot policy'),
            ),
          ),
        );
      }
    });
  }

  test(
    'file inspection returns the same verified image and capability',
    () async {
      final Map<String, dynamic> entry = cases
          .cast<Map<String, dynamic>>()
          .firstWhere((entry) => entry['name'] == 'sdk_policy_2');
      final Uint8List firmware = _bytesFromHex(entry['image_hex'] as String);
      final FirmwareManifest manifest = makeFirmwareManifest(firmware);
      final Directory directory = await Directory.systemTemp.createTemp(
        'litch-artifact-inspection-',
      );
      addTearDown(() => directory.delete(recursive: true));
      final File file = await File(
        '${directory.path}/firmware.bin',
      ).writeAsBytes(firmware);

      final FirmwareArtifactInspection inspection =
          await FirmwareArtifactValidator.inspectFile(file, manifest);
      await FirmwareArtifactValidator.validateFile(file, manifest);
      expect(inspection.bootPolicy.status, FirmwareBootPolicyStatus.known);
      expect(inspection.bootPolicy.epoch, 2);
      expect(inspection.imageSha256, entry['image_sha256']);
      expect(inspection.artifactSha256, entry['artifact_sha256']);
    },
  );

  test('unproved app descriptor location yields no boot capability', () {
    for (final Uint8List segment in <Uint8List>[
      Uint8List.fromList(utf8.encode('FXBT')),
      Uint8List(272)..setRange(256, 272, <int>[
        0x46,
        0x58,
        0x42,
        0x54,
        1,
        0,
        16,
        0,
        2,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
      ]),
    ]) {
      final Uint8List firmware = makeEsp32s3Firmware(segmentBytes: segment);
      final FirmwareManifest manifest = makeFirmwareManifest(firmware);
      FirmwareArtifactValidator.validateBytes(firmware, manifest);
      final inspection = FirmwareArtifactValidator.inspectBytes(
        firmware,
        manifest,
      );
      expect(
        inspection.bootPolicy.status,
        FirmwareBootPolicyStatus.unavailable,
      );
      expect(inspection.bootPolicy.schema, 0);
      expect(inspection.bootPolicy.sizeBytes, 0);
      expect(inspection.bootPolicy.epoch, 0);
      expect(
        () => FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection),
        throwsA(isA<FirmwareArtifactValidationException>()),
      );
    }
  });

  test('inspection binds capability to both exact manifest hashes', () {
    final Map<String, dynamic> entry = cases
        .cast<Map<String, dynamic>>()
        .firstWhere((entry) => entry['name'] == 'sdk_policy_2');
    final Uint8List firmware = _bytesFromHex(entry['image_hex'] as String);
    final FirmwareManifest valid = makeFirmwareManifest(firmware);
    for (final bool mismatchFileHash in <bool>[true, false]) {
      final FirmwareManifest manifest = FirmwareManifest(
        schemaVersion: valid.schemaVersion,
        channel: valid.channel,
        version: valid.version,
        target: valid.target,
        chipId: valid.chipId,
        artifact: FirmwareArtifactInfo(
          url: valid.artifact.url,
          sizeBytes: valid.artifact.sizeBytes,
          sha256: mismatchFileHash ? '0' * 64 : valid.artifact.sha256,
          imageSha256: mismatchFileHash ? valid.artifact.imageSha256 : '0' * 64,
        ),
        notes: valid.notes,
      );
      FirmwareArtifactInspection? inspection;
      expect(
        () => inspection = FirmwareArtifactValidator.inspectBytes(
          firmware,
          manifest,
        ),
        throwsA(isA<FirmwareArtifactValidationException>()),
      );
      expect(inspection, isNull);
    }
  });

  test('accepts bytes matching the manifest and embedded S3 image hash', () {
    final Uint8List firmware = makeEsp32s3Firmware();
    final FirmwareManifest manifest = makeFirmwareManifest(firmware);

    FirmwareArtifactValidator.validateBytes(firmware, manifest);
    final inspection = FirmwareArtifactValidator.inspectBytes(
      firmware,
      manifest,
    );
    expect(inspection.bootPolicy.status, FirmwareBootPolicyStatus.known);
    expect(inspection.bootPolicy.epoch, 2);
    FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
  });

  test('explicit old-policy fixture stays generically inspectable', () {
    final firmware = makeEsp32s3Firmware(bootPolicyEpoch: 1);
    final inspection = FirmwareArtifactValidator.inspectBytes(
      firmware,
      makeFirmwareManifest(firmware),
    );
    expect(inspection.bootPolicy.status, FirmwareBootPolicyStatus.known);
    expect(inspection.bootPolicy.epoch, 1);
    expect(
      () => FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection),
      throwsA(isA<FirmwareArtifactValidationException>()),
    );
  });

  test(
    'rejects malformed or non-S3 image structure with matching file SHA',
    () {
      final Uint8List valid = makeEsp32s3Firmware();
      final Map<String, Uint8List> invalidImages = <String, Uint8List>{};

      Uint8List changed(void Function(Uint8List bytes) mutate) {
        final Uint8List bytes = Uint8List.fromList(valid);
        mutate(bytes);
        return bytes;
      }

      invalidImages['magic'] = changed((Uint8List bytes) => bytes[0] = 0);
      invalidImages['segment count'] = changed(
        (Uint8List bytes) => bytes[1] = 0,
      );
      invalidImages['segment size'] = changed((Uint8List bytes) {
        bytes[28] = 0;
        bytes[29] = 0;
        bytes[30] = 0;
        bytes[31] = 0;
      });
      invalidImages['ESP32-S3'] = changed((Uint8List bytes) => bytes[12] = 5);
      invalidImages['appended SHA-256'] = changed(
        (Uint8List bytes) => bytes[23] = 0,
      );
      invalidImages['checksum'] = changed(
        (Uint8List bytes) => bytes[bytes.length - 33] ^= 1,
      );
      invalidImages['trailing image SHA-256'] = changed(
        (Uint8List bytes) => bytes[bytes.length - 1] ^= 1,
      );
      invalidImages['exact size'] = Uint8List.fromList(<int>[...valid, 0]);

      for (final MapEntry<String, Uint8List> entry in invalidImages.entries) {
        expect(
          () => FirmwareArtifactValidator.validateBytes(
            entry.value,
            makeFirmwareManifest(entry.value),
          ),
          throwsA(
            isA<FirmwareArtifactValidationException>().having(
              (FirmwareArtifactValidationException error) => error.message,
              'message',
              contains(entry.key),
            ),
          ),
          reason: entry.key,
        );
      }
    },
  );

  test('rejects bytes whose exact full-file SHA differs from the manifest', () {
    final Uint8List firmware = makeEsp32s3Firmware();
    final FirmwareManifest manifest = makeFirmwareManifest(firmware);
    final Uint8List changed = Uint8List.fromList(firmware);
    changed[30] ^= 1;

    expect(
      () => FirmwareArtifactValidator.validateBytes(changed, manifest),
      throwsA(
        isA<FirmwareArtifactValidationException>().having(
          (FirmwareArtifactValidationException error) => error.message,
          'message',
          contains('full-file SHA-256'),
        ),
      ),
    );
  });
}

Uint8List _bytesFromHex(String value) => Uint8List.fromList(<int>[
  for (int index = 0; index < value.length; index += 2)
    int.parse(value.substring(index, index + 2), radix: 16),
]);
