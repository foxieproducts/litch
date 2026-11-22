import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';

Map<String, dynamic> _fixture(String name) =>
    jsonDecode(File('test/fixtures/ota_attempt/$name').readAsStringSync())
        as Map<String, dynamic>;

ByteData _selector(String hex) => ByteData.sublistView(
  Uint8List.fromList([
    for (int offset = 0; offset < hex.length; offset += 2)
      int.parse(hex.substring(offset, offset + 2), radix: 16),
  ]),
);

// The board observations and selector bytes come from hardware. The device ID,
// boot nonces, and native connection tokens are synthetic.
Future<OtaPostRebootVerification> _verifyCapturedAttempt(
  Map<String, dynamic> fixture,
) async {
  final artifact = fixture['artifact'] as Map<String, dynamic>;
  final before = fixture['before'] as Map<String, dynamic>;
  final post = fixture['post'] as Map<String, dynamic>;
  final admission = fixture['admission'] as Map<String, dynamic>;
  final lifecycle = fixture['native_lifecycle_model'] as Map<String, dynamic>;

  // Same selected image, an explicit acceptance, and a genuinely new modeled
  // native connection all hold. Missing freshness must not explain rejection.
  for (final observation in [before, post]) {
    expect(observation['version'], artifact['version']);
    expect(observation['image_sha256'], artifact['image_sha256']);
    expect(observation['accepted'], isTrue);
  }
  expect(post['boot_nonce'], isNot(before['boot_nonce']));
  expect(admission['source_slot'], before['active_slot']);
  expect(admission['source_sequence'], before['selected_sequence']);
  final sourceSector = admission['source_sector'] as int;
  final targetSector = admission['target_sector'] as int;
  expect(
    post['raw_selectors'][sourceSector],
    before['raw_selectors'][sourceSector],
  );
  final source = _selector(post['raw_selectors'][sourceSector] as String);
  final target = _selector(post['raw_selectors'][targetSector] as String);
  expect(source.lengthInBytes, 32);
  expect(target.lengthInBytes, 32);
  expect(source.getUint32(0, Endian.little), admission['source_sequence']);
  expect(source.getUint32(24, Endian.little), 2); // SDK VALID.
  expect(target.getUint32(0, Endian.little), admission['expected_sequence']);
  final fallback = fixture['expected_outcome'] == 'same_image_fallback';
  expect(target.getUint32(24, Endian.little), fallback ? 4 : 2);
  expect(
    post['active_slot'],
    admission[fallback ? 'source_slot' : 'target_slot'],
  );
  expect(
    post['selected_sequence'],
    admission[fallback ? 'source_sequence' : 'expected_sequence'],
  );

  final uploadChannel = LitchOtaControlChannel(
    deviceId: fixture['device_id'] as String,
    connectionToken: lifecycle['upload_connection_token'] as int,
    generation: 1,
    subscribe: () => const Stream<List<int>>.empty(),
  );
  final verificationTarget = OtaVerificationTarget(
    deviceId: uploadChannel.deviceId,
    uploadConnectionToken: uploadChannel.connectionToken,
    version: artifact['version'] as String,
    imageSha256: artifact['image_sha256'] as String,
    artifactSha256: artifact['sha256'] as String,
    attempt: OtaAttemptIdentity(
      preBootNonce: before['boot_nonce'] as String,
      // USB does not report a BLE adapter attempt number.
      attempt: admission['adapter_attempt'] as int? ?? 1,
      sourceSlot: admission['source_slot'] as int,
      sourceSector: sourceSector,
      sourceSequence: admission['source_sequence'] as int,
      targetSlot: admission['target_slot'] as int,
      targetSector: targetSector,
      expectedSequence: admission['expected_sequence'] as int,
    ),
  );
  await uploadChannel.dispose();
  final freshChannel = LitchOtaControlChannel(
    deviceId: fixture['device_id'] as String,
    connectionToken: lifecycle['post_connection_token'] as int,
    generation: 2,
    subscribe: () => const Stream<List<int>>.empty(),
  );
  expect(
    freshChannel.connectionToken,
    greaterThan(verificationTarget.uploadConnectionToken),
  );
  try {
    final sdkLine = (post['captured_version_lines'] as List<dynamic>)
        .cast<String>()
        .firstWhere((line) => line.startsWith('ota_boot:'));
    final sample = int.parse(
      RegExp(r'sample=(\d+)').firstMatch(sdkLine)!.group(1)!,
    );
    final age = int.parse(
      RegExp(r'age_ms=(\d+)').firstMatch(sdkLine)!.group(1)!,
    );
    final records = (post['raw_selectors'] as List<dynamic>)
        .cast<String>()
        .map(_selector)
        .toList();
    final observed = OtaAttemptProof.tryParse({
      'op': 1,
      'q': 1,
      'n': post['boot_nonce'],
      'v': post['accepted'] == true ? 1 : 0,
      's': [
        sample,
        age,
        post['active_slot'],
        post['active_slot'],
        post['selected_sector'],
        post['selected_sequence'],
        2,
        for (final record in records) ...[
          record.getUint32(0, Endian.little),
          record.getUint32(24, Endian.little),
          _validSelectorCrc(record) ? 1 : 0,
        ],
      ],
      'a': [0, 0, 255, 255, 0, 0],
    });
    expect(observed, isNotNull);
    return await const OtaPostRebootVerifier().verify(
      target: verificationTarget,
      readAttemptProof: (_) async => observed,
      openFreshChannel: () async => freshChannel,
      readVersion: (_) async => post['version'] as String,
      readDigest: (_) async => post['image_sha256'] as String,
      readAcceptance: (_) async => post['accepted'] as bool,
      isChannelFresh: (channel) => identical(channel, freshChannel),
    );
  } finally {
    await freshChannel.dispose();
  }
}

bool _validSelectorCrc(ByteData record) {
  int crc = 0;
  for (int offset = 0; offset < 4; ++offset) {
    crc ^= record.getUint8(offset);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >>> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) == record.getUint32(28, Endian.little);
}

void main() {
  for (final entry in [
    ('usb_23_valid_24_aborted.json', '23 VALID / 24 ABORTED'),
    ('usb_24_valid_25_aborted.json', '24 VALID / 25 ABORTED opposite slot'),
  ]) {
    test('${entry.$2} same-image fallback must not verify an update', () async {
      final fixture = _fixture(entry.$1);
      final result = await _verifyCapturedAttempt(fixture);
      expect(
        result,
        isA<OtaPostRebootFailedAttempt>(),
        reason:
            'The board rebooted into the source slot after the target slot '
            'was marked ABORTED.',
      );
    });
  }

  test('captured BLE exact target 25 VALID remains verifiable', () async {
    final result = await _verifyCapturedAttempt(
      _fixture('ble_24_to_25_valid.json'),
    );
    expect(result, isA<OtaPostRebootVerified>());
  });
}
