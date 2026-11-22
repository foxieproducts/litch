import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';

/// Explicit [segmentBytes] stays a raw first segment for generic/old fixtures.
Uint8List makeEsp32s3Firmware({
  List<int>? segmentBytes,
  List<int>? payloadBytes,
  int bootPolicyEpoch = 2,
}) {
  if (segmentBytes != null && payloadBytes != null) {
    throw ArgumentError('Choose raw segmentBytes or SDK payloadBytes.');
  }
  final descriptor = Uint8List(272);
  ByteData.sublistView(descriptor)
    ..setUint32(0, 0xABCD5432, Endian.little)
    ..setUint32(256, 0x54425846, Endian.little)
    ..setUint16(260, 1, Endian.little)
    ..setUint16(262, 16, Endian.little)
    ..setUint32(264, bootPolicyEpoch, Endian.little);
  descriptor.setRange(16, 21, utf8.encode('0.6.0'));
  descriptor.setRange(48, 58, utf8.encode('litch-test'));
  final List<int> segment = List<int>.from(
    segmentBytes ??
        <int>[
          ...descriptor,
          ...?payloadBytes,
          if (payloadBytes == null) ...utf8.encode('litch-test-image'),
        ],
  );
  while (segment.length % 4 != 0) {
    segment.add(0);
  }

  final Uint8List header = Uint8List(24);
  header[0] = 0xE9;
  header[1] = 1;
  header[12] = 9;
  header[13] = 0;
  header[23] = 1;

  final ByteData segmentHeader = ByteData(8)
    ..setUint32(0, 0x3C000020, Endian.little)
    ..setUint32(4, segment.length, Endian.little);
  final BytesBuilder prefix = BytesBuilder(copy: false)
    ..add(header)
    ..add(segmentHeader.buffer.asUint8List())
    ..add(segment);

  int checksum = 0xEF;
  for (final int value in segment) {
    checksum ^= value;
  }
  final int padding = (15 - prefix.length) % 16;
  prefix.add(Uint8List(padding));
  prefix.add(<int>[checksum]);

  final Uint8List prefixBytes = prefix.takeBytes();
  return Uint8List.fromList(<int>[
    ...prefixBytes,
    ...sha256.convert(prefixBytes).bytes,
  ]);
}

FirmwareManifest makeFirmwareManifest(
  Uint8List firmware, {
  String channel = 'stable',
  Uri? manifestUrl,
  String notesMarkdown = 'Version: 0.6.0\n',
  String version = '0.6.0',
}) {
  final Uri resolvedManifestUrl =
      manifestUrl ??
      Uri.parse('https://updates.foxieproducts.com/fw/manifest.json');
  return FirmwareManifest.parse(
    makeFirmwareManifestJson(
      firmware,
      channel: channel,
      notesMarkdown: notesMarkdown,
      version: version,
    ),
    manifestUrl: resolvedManifestUrl,
    expectedChannel: channel,
  );
}

String makeFirmwareManifestJson(
  Uint8List firmware, {
  String channel = 'stable',
  String notesMarkdown = 'Version: 0.6.0\n',
  String version = '0.6.0',
}) {
  final String fullSha = sha256.convert(firmware).toString();
  final String imageSha = sha256
      .convert(firmware.sublist(0, firmware.length - 32))
      .toString();
  final List<int> notesBytes = utf8.encode(notesMarkdown);
  final String notesSha = sha256.convert(notesBytes).toString();
  return jsonEncode(<String, Object>{
    'schema_version': 1,
    'channel': channel,
    'version': version,
    'target': 'esp32s3',
    'chip_id': 9,
    'artifact': <String, Object>{
      'url':
          'https://updates.foxieproducts.com/fw/firmware_${version}_$fullSha.bin',
      'size_bytes': firmware.length,
      'sha256': fullSha,
      'image_sha256': imageSha,
    },
    'notes': <String, Object>{
      'url':
          'https://updates.foxieproducts.com/fw/version_${version}_$notesSha.md',
      'size_bytes': notesBytes.length,
      'sha256': notesSha,
    },
  });
}
