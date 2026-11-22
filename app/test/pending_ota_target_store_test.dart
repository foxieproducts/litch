import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';

OtaVerificationTarget target({String deviceId = 'board'}) =>
    OtaVerificationTarget(
      deviceId: deviceId,
      uploadConnectionToken: 42,
      version: '0.9.0-5',
      imageSha256: 'a' * 64,
      artifactSha256: 'b' * 64,
      attempt: const OtaAttemptIdentity(
        preBootNonce: 'fedcba9876543210',
        attempt: 7,
        sourceSlot: 0,
        sourceSector: 1,
        sourceSequence: 45,
        targetSlot: 1,
        targetSector: 0,
        expectedSequence: 48,
      ),
    );

void main() {
  late Directory directory;
  late File file;
  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch-intent-');
    file = File('${directory.path}/pending.json');
  });
  tearDown(() => directory.delete(recursive: true));

  test(
    'recreated store restores exact authority without native tokens',
    () async {
      final store = FilePendingOtaTargetStore(file);
      expect(await store.load(), isNull);
      await store.save(target());
      expect(await File('${file.path}.tmp').exists(), isFalse);
      final encoded =
          jsonDecode(await file.readAsString()) as Map<String, dynamic>;
      expect(encoded.containsKey('uploadConnectionToken'), isFalse);
      final restored = (await FilePendingOtaTargetStore(file).load())!;
      expect(restored.uploadConnectionToken, -1);
      expect(restored.deviceId, target().deviceId);
      expect(restored.artifactSha256, 'b' * 64);
      expect(restored.attempt!.preBootNonce, 'fedcba9876543210');
      expect(restored.attempt!.expectedSequence, 48);
      await store.save(restored); // Idempotent exact intent only.
      await expectLater(
        store.save(target(deviceId: 'other')),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
      await expectLater(
        store.clear(target(deviceId: 'other')),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
      expect(await file.exists(), isTrue);
      await FilePendingOtaTargetStore(file).clear(restored);
      expect(await store.load(), isNull);
    },
  );

  test(
    'corrupt or unknown intent cannot be silently erased or overwritten',
    () async {
      for (final data in ['{', '{"schema":2}', 'x' * 4097]) {
        await file.writeAsString(data);
        final store = FilePendingOtaTargetStore(file);
        for (final operation in [
          store.load,
          () => store.save(target()),
          () => store.clear(target()),
        ]) {
          await expectLater(
            operation(),
            throwsA(isA<PendingOtaTargetStoreException>()),
          );
        }
        expect(await file.readAsString(), data);
      }
    },
  );

  test(
    'sole flushed temporary intent is restored and cleared without End replay',
    () async {
      await FilePendingOtaTargetStore(file).save(target());
      await file.rename('${file.path}.tmp');
      final recreated = FilePendingOtaTargetStore(file);
      final restored = (await recreated.load())!;
      expect(restored.uploadConnectionToken, -1);
      expect(
        await file.exists(),
        isFalse,
        reason: 'Loading never republishes intent',
      );
      await expectLater(
        recreated.save(restored),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
      await recreated.clear(restored);
      expect(await recreated.load(), isNull);
    },
  );

  test(
    'valid primary can verify while unknown temp remains protected and blocks reuse',
    () async {
      final store = FilePendingOtaTargetStore(file);
      await store.save(target());
      final temporary = await File('${file.path}.tmp').writeAsString('{');
      expect((await FilePendingOtaTargetStore(file).load())!.deviceId, 'board');
      await expectLater(
        store.clear(target()),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
      expect(await file.exists(), isFalse);
      expect(await temporary.readAsString(), '{');
      await expectLater(
        store.load(),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
      await expectLater(
        store.save(target()),
        throwsA(isA<PendingOtaTargetStoreException>()),
      );
    },
  );

  test('interrupted prepublication write blocks another transfer', () async {
    await File('${file.path}.tmp').writeAsString('{');
    await expectLater(
      FilePendingOtaTargetStore(file).save(target()),
      throwsA(isA<PendingOtaTargetStoreException>()),
    );
    expect(await File('${file.path}.tmp').readAsString(), '{');
    expect(await file.exists(), isFalse);
  });
}
