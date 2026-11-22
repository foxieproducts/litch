import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/ota_pending_verification_owner.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';

class _PublishedThenFailedStore implements OtaPendingTargetStore {
  _PublishedThenFailedStore(this.actual);
  final OtaPendingTargetStore actual;
  @override
  Future<OtaVerificationTarget?> load() => actual.load();
  @override
  Future<void> clear(OtaVerificationTarget target) => actual.clear(target);
  @override
  Future<void> save(OtaVerificationTarget target) async {
    await actual.save(target);
    throw const FileSystemException(
      'Publication completed before failure return',
    );
  }
}

void main() {
  test(
    'failed save return reloads published intent before allowing another update',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch-owner-intent-',
      );
      addTearDown(() => directory.delete(recursive: true));
      final file = File('${directory.path}/intent.json');
      final owner = OtaPendingVerificationOwner(
        store: _PublishedThenFailedStore(FilePendingOtaTargetStore(file)),
      );
      final target = OtaVerificationTarget(
        deviceId: 'board',
        uploadConnectionToken: 7,
        version: '0.9.0-5',
        imageSha256: 'a' * 64,
        artifactSha256: 'b' * 64,
        attempt: const OtaAttemptIdentity(
          preBootNonce: 'fedcba9876543210',
          attempt: 1,
          sourceSlot: 0,
          sourceSector: 0,
          sourceSequence: 23,
          targetSlot: 1,
          targetSector: 1,
          expectedSequence: 24,
        ),
      );
      await expectLater(
        owner.remember(target),
        throwsA(isA<FileSystemException>()),
      );
      expect(owner.storageError, isNotNull);
      await owner.restore();
      final restored = owner.pending!;
      expect(restored.attempt!.expectedSequence, 24);
      expect(restored.uploadConnectionToken, -1);
      expect(owner.storageError, isNull);
      await expectLater(owner.remember(target), throwsA(isA<StateError>()));
      expect(await owner.clear(restored), isTrue);
      expect(await file.exists(), isFalse);
    },
  );
}
