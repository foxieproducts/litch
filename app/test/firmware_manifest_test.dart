import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';

void main() {
  const String fullSha =
      'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
  const String imageSha =
      'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb';
  const String notesSha =
      'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc';
  final Uri manifestUrl = Uri.parse('https://foxielitch.com/fw/manifest.json');

  Map<String, Object> validManifest() => <String, Object>{
    'schema_version': 1,
    'channel': 'stable',
    'version': '0.6.0',
    'target': 'esp32s3',
    'chip_id': 9,
    'artifact': <String, Object>{
      'url': 'https://foxielitch.com/fw/firmware_0.6.0_$fullSha.bin',
      'size_bytes': 123456,
      'sha256': fullSha,
      'image_sha256': imageSha,
    },
    'notes': <String, Object>{
      'url': 'https://foxielitch.com/fw/version_0.6.0_$notesSha.md',
      'size_bytes': 456,
      'sha256': notesSha,
    },
  };

  test('parses a same-origin immutable S3 release manifest', () {
    final FirmwareManifest manifest = FirmwareManifest.parse(
      jsonEncode(validManifest()),
      manifestUrl: manifestUrl,
      expectedChannel: 'stable',
    );

    expect(manifest.schemaVersion, 1);
    expect(manifest.version, '0.6.0');
    expect(manifest.target, 'esp32s3');
    expect(manifest.chipId, 9);
    expect(manifest.artifact.sizeBytes, 123456);
    expect(manifest.artifact.sha256, fullSha);
    expect(manifest.artifact.imageSha256, imageSha);
    expect(manifest.notes.sizeBytes, 456);
    expect(manifest.notes.sha256, notesSha);
    expect(
      manifest.artifact.url,
      Uri.parse('https://foxielitch.com/fw/firmware_0.6.0_$fullSha.bin'),
    );
  });

  test(
    'rejects insecure, cross-origin, mutable, or decorated artifact URLs',
    () {
      final List<String> rejectedUrls = <String>[
        'http://foxielitch.com/fw/firmware_0.6.0_$fullSha.bin',
        'https://updates.example/fw/firmware_0.6.0_$fullSha.bin',
        'https://foxielitch.com/fw/firmware.bin',
        'https://foxielitch.com/fw/firmware_0.6.0.bin',
        'https://foxielitch.com/fw/firmware_0.6.0_$fullSha.bin?new=1',
      ];

      for (final String rejectedUrl in rejectedUrls) {
        final Map<String, Object> source = validManifest();
        final Map<String, Object> artifact = Map<String, Object>.from(
          source['artifact']! as Map<String, Object>,
        );
        artifact['url'] = rejectedUrl;
        source['artifact'] = artifact;

        expect(
          () => FirmwareManifest.parse(
            jsonEncode(source),
            manifestUrl: manifestUrl,
            expectedChannel: 'stable',
          ),
          throwsA(isA<FirmwareManifestFormatException>()),
          reason: rejectedUrl,
        );
      }
    },
  );

  test('rejects wrong schema, channel, target, chip, and artifact size', () {
    expect(FirmwareManifest.maxOfficialEsp32s3FirmwareSizeBytes, 0x200000);
    final List<MapEntry<String, Object>> invalidFields =
        <MapEntry<String, Object>>[
          const MapEntry<String, Object>('schema_version', 2),
          const MapEntry<String, Object>('channel', 'beta'),
          const MapEntry<String, Object>('target', 'esp32c3'),
          const MapEntry<String, Object>('chip_id', 5),
        ];
    for (final MapEntry<String, Object> field in invalidFields) {
      final Map<String, Object> source = validManifest();
      source[field.key] = field.value;
      expect(
        () => FirmwareManifest.parse(
          jsonEncode(source),
          manifestUrl: manifestUrl,
          expectedChannel: 'stable',
        ),
        throwsA(isA<FirmwareManifestFormatException>()),
        reason: field.key,
      );
    }

    final Map<String, Object> oversized = validManifest();
    final Map<String, Object> artifact = Map<String, Object>.from(
      oversized['artifact']! as Map<String, Object>,
    );
    artifact['size_bytes'] =
        FirmwareManifest.maxOfficialEsp32s3FirmwareSizeBytes + 1;
    oversized['artifact'] = artifact;
    expect(
      () => FirmwareManifest.parse(
        jsonEncode(oversized),
        manifestUrl: manifestUrl,
        expectedChannel: 'stable',
      ),
      throwsA(isA<FirmwareManifestFormatException>()),
    );
  });

  test('rejects insecure manifest and untrusted notes URLs', () {
    expect(
      () => FirmwareManifest.parse(
        jsonEncode(validManifest()),
        manifestUrl: Uri.parse('http://foxielitch.com/fw/manifest.json'),
        expectedChannel: 'stable',
      ),
      throwsA(isA<FirmwareManifestFormatException>()),
    );

    final List<String> rejectedUrls = <String>[
      'https://updates.example/fw/version_0.6.0_$notesSha.md',
      'https://foxielitch.com/fw/version.md',
      'https://foxielitch.com/fw/version_0.6.0.md',
      'https://foxielitch.com/fw/version_0.6.0_$notesSha.md?new=1',
    ];
    for (final String rejectedUrl in rejectedUrls) {
      final Map<String, Object> source = validManifest();
      source['notes'] = <String, Object>{
        'url': rejectedUrl,
        'size_bytes': 456,
        'sha256': notesSha,
      };
      expect(
        () => FirmwareManifest.parse(
          jsonEncode(source),
          manifestUrl: manifestUrl,
          expectedChannel: 'stable',
        ),
        throwsA(isA<FirmwareManifestFormatException>()),
        reason: rejectedUrl,
      );
    }
  });
}
