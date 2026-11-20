import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';

LitchOtaControlChannel channel(
  int connectionToken, {
  String deviceId = 'board-1',
}) {
  return LitchOtaControlChannel(
    deviceId: deviceId,
    connectionToken: connectionToken,
    generation: 1,
    subscribe: () => const Stream<List<int>>.empty(),
  );
}

const attempt = OtaAttemptIdentity(
  preBootNonce: '0123456789abcdef',
  attempt: 1,
  sourceSlot: 0,
  sourceSector: 0,
  sourceSequence: 23,
  targetSlot: 1,
  targetSector: 1,
  expectedSequence: 24,
);

OtaAttemptProof proof({
  String nonce = 'fedcba9876543210',
  int sourceSequence = 23,
  int targetSequence = 24,
  int targetState = 2,
  bool fallback = false,
}) => OtaAttemptProof.tryParse({
  'op': 1,
  'q': 1,
  'n': nonce,
  'v': 1,
  's': [
    1,
    6000,
    fallback ? 0 : 1,
    fallback ? 0 : 1,
    fallback ? 0 : 1,
    fallback ? sourceSequence : targetSequence,
    2,
    sourceSequence,
    2,
    1,
    targetSequence,
    targetState,
    1,
  ],
  'a': [0, 0, 255, 255, 0, 0],
})!;

void main() {
  const OtaVerificationTarget target = OtaVerificationTarget(
    deviceId: 'board-1',
    uploadConnectionToken: 4,
    attempt: attempt,

    version: '2.0.0',
    imageSha256:
        'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
  );
  const OtaPostRebootVerifier verifier = OtaPostRebootVerifier(
    timeout: Duration(milliseconds: 20),
  );

  test(
    'exact accepted target verifies after a changed board boot nonce',
    () async {
      final result = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),
        readVersion: (_) async => target.version,
        readDigest: (_) async => target.imageSha256,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      expect(result, isA<OtaPostRebootVerified>());
      expect(result.message, 'Selected firmware verified on this board.');
    },
  );

  test(
    'same-boot exact aborted admission resolves a pre-End interrupted intent',
    () async {
      for (final phase in [1, 3, 4]) {
        final observed = OtaAttemptProof.tryParse({
          'op': 1,
          'q': 1,
          'n': attempt.preBootNonce,
          'v': 0,
          's': [1, 6000, 0, 0, 0, 23, 2, 23, 2, 1, 22, 2, 1],
          'a': [1, phase, 0, 1, 23, 24],
        })!;
        final result = await verifier.verify(
          target: target,
          openFreshChannel: () async => channel(4),
          readVersion: (_) async => 'recovery',
          readDigest: (_) async => 'b' * 64,
          readAcceptance: (_) async => null,
          readAttemptProof: (_) async => observed,
          isChannelFresh: (_) => true,
        );
        expect(
          result,
          phase == 1
              ? isA<OtaPostRebootPending>()
              : isA<OtaPostRebootFailedAttempt>(),
        );
      }
    },
  );

  test('admitted skipped sequence 45 to 48 verifies exactly', () async {
    const skipped = OtaAttemptIdentity(
      preBootNonce: '0123456789abcdef',
      attempt: 1,
      sourceSlot: 0,
      sourceSector: 0,
      sourceSequence: 45,
      targetSlot: 1,
      targetSector: 1,
      expectedSequence: 48,
    );
    expect(skipped.isValid, isTrue);
    final result = await verifier.verify(
      target: OtaVerificationTarget(
        deviceId: target.deviceId,
        uploadConnectionToken: 4,
        version: target.version,
        imageSha256: target.imageSha256,
        attempt: skipped,
      ),
      openFreshChannel: () async => channel(5),
      readVersion: (_) async => target.version,
      readDigest: (_) async => target.imageSha256,
      readAcceptance: (_) async => true,
      readAttemptProof: (_) async =>
          proof(sourceSequence: 45, targetSequence: 48),
      isChannelFresh: (_) => true,
    );
    expect(result, isA<OtaPostRebootVerified>());
  });

  test(
    'missing attempt evidence cannot verify from matching image alone',
    () async {
      final result = await verifier.verify(
        target: OtaVerificationTarget(
          deviceId: target.deviceId,
          uploadConnectionToken: 4,
          version: target.version,
          imageSha256: target.imageSha256,
        ),
        openFreshChannel: () async => channel(5),
        readVersion: (_) async => target.version,
        readDigest: (_) async => target.imageSha256,
        readAcceptance: (_) async => true,
        readAttemptProof: (_) async => proof(),
        isChannelFresh: (_) => true,
      );
      expect(result, isA<OtaPostRebootPending>());
    },
  );

  test(
    'same nonce, newer unrelated sequence, and changed source stay pending',
    () async {
      for (final observed in [
        proof(nonce: attempt.preBootNonce),
        proof(targetSequence: 26),
        proof(sourceSequence: 21),
      ]) {
        final result = await verifier.verify(
          target: target,
          openFreshChannel: () async => channel(5),
          readVersion: (_) async => target.version,
          readDigest: (_) async => target.imageSha256,
          readAcceptance: (_) async => true,
          readAttemptProof: (_) async => observed,
          isChannelFresh: (_) => true,
        );
        expect(result, isA<OtaPostRebootPending>());
      }
    },
  );

  test(
    'exact same-image failed trial resolves with expected and observed evidence',
    () async {
      for (final state in [3, 4]) {
        final observed = proof(fallback: true, targetState: state);
        final result = await verifier.verify(
          target: target,
          openFreshChannel: () async => channel(5),
          readVersion: (_) async => target.version,
          readDigest: (_) async => target.imageSha256,
          readAcceptance: (_) async => true,
          readAttemptProof: (_) async => observed,
          isChannelFresh: (_) => true,
        );
        expect(result, isA<OtaPostRebootFailedAttempt>());
        expect((result as OtaPostRebootFailedAttempt).expected, same(attempt));
        expect(result.observed, same(observed));
      }
    },
  );

  test(
    'different image resolves only after explicit runtime acceptance',
    () async {
      for (final accepted in <bool?>[null, false, true]) {
        final result = await verifier.verify(
          target: target,
          readAttemptProof: (_) async => proof(),
          openFreshChannel: () async => channel(5),
          readVersion: (_) async => 'rescue',
          readDigest: (_) async => 'b' * 64,
          readAcceptance: (_) async => accepted,
          isChannelFresh: (_) => true,
        );
        expect(
          result,
          accepted == true
              ? isA<OtaPostRebootDifferentImage>()
              : isA<OtaPostRebootPending>(),
        );
      }
    },
  );

  test('verification requires the complete selected running digest', () async {
    for (final digest in <String?>[null, '', 'broken', 'a' * 64, 'b' * 64]) {
      final result = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),
        readVersion: (_) async => target.version,
        readDigest: (_) async => digest,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      expect(result is OtaPostRebootVerified, digest == 'a' * 64);
      if (digest == null || digest.length != 64) {
        expect(result, isA<OtaPostRebootPending>());
      }
    }
  });

  test(
    'verifies exact identity and acceptance on a new connection lifetime',
    () async {
      final OtaPostRebootVerification result = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) async => '2.0.0',
        readDigest: (_) async => target.imageSha256,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );

      expect(result, isA<OtaPostRebootVerified>());
    },
  );

  test(
    'rejects stale channels, another board, rollback, and digest mismatch',
    () async {
      final List<OtaPostRebootVerification> results =
          <OtaPostRebootVerification>[
            await verifier.verify(
              target: target,
              readAttemptProof: (_) async => proof(),
              openFreshChannel: () async => channel(4),

              readVersion: (_) async => '2.0.0',
              readDigest: (_) async => target.imageSha256,
              readAcceptance: (_) async => true,
              isChannelFresh: (_) => true,
            ),
            await verifier.verify(
              target: target,
              readAttemptProof: (_) async => proof(),
              openFreshChannel: () async => channel(5, deviceId: 'board-2'),

              readVersion: (_) async => '2.0.0',
              readDigest: (_) async => target.imageSha256,
              readAcceptance: (_) async => true,
              isChannelFresh: (_) => true,
            ),
            await verifier.verify(
              target: target,
              readAttemptProof: (_) async => proof(),
              openFreshChannel: () async => channel(5),

              readVersion: (_) async => '1.9.0',
              readDigest: (_) async => target.imageSha256,
              readAcceptance: (_) async => true,
              isChannelFresh: (_) => true,
            ),
            await verifier.verify(
              target: target,
              readAttemptProof: (_) async => proof(),
              openFreshChannel: () async => channel(5),

              readVersion: (_) async => '2.0.0',
              readDigest: (_) async => 'b' * 64,
              readAcceptance: (_) async => true,
              isChannelFresh: (_) => true,
            ),
          ];

      expect(results[0], isA<OtaPostRebootPending>());
      expect(results[1], isA<OtaPostRebootPending>());
      expect(results[2], isA<OtaPostRebootMismatch>());
      expect(results[3], isA<OtaPostRebootMismatch>());
    },
  );

  test(
    'unavailable digest stays retryable and a later retry can verify',
    () async {
      final OtaPostRebootVerification pending = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) async => '2.0.0',
        readDigest: (_) async => null,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      final OtaPostRebootVerification verified = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(6),

        readVersion: (_) async => '2.0.0',
        readDigest: (_) async => target.imageSha256,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );

      expect(pending, isA<OtaPostRebootPending>());
      expect(verified, isA<OtaPostRebootVerified>());
    },
  );

  test('timeout or canceled channel work remains unverified', () async {
    final OtaPostRebootVerification reconnectTimeout = await verifier.verify(
      target: target,
      readAttemptProof: (_) async => proof(),
      openFreshChannel: () => Future<LitchOtaControlChannel?>.delayed(
        const Duration(seconds: 1),
        () => channel(5),
      ),

      readVersion: (_) async => '2.0.0',
      readDigest: (_) async => target.imageSha256,
      readAcceptance: (_) async => true,
      isChannelFresh: (_) => true,
    );
    final OtaPostRebootVerification canceledRead = await verifier.verify(
      target: target,
      readAttemptProof: (_) async => proof(),
      openFreshChannel: () async => channel(5),

      readVersion: (_) =>
          Future<String?>.error(StateError('connection generation changed')),
      readDigest: (_) async => target.imageSha256,
      readAcceptance: (_) async => true,
      isChannelFresh: (_) => true,
    );

    expect(reconnectTimeout, isA<OtaPostRebootPending>());
    expect(canceledRead, isA<OtaPostRebootPending>());
  });

  test('channel replacement during digest read stays unverified', () async {
    LitchOtaControlChannel activeChannel = channel(5);
    final OtaPostRebootVerification result = await verifier.verify(
      target: target,
      readAttemptProof: (_) async => proof(),
      openFreshChannel: () async => activeChannel,

      readVersion: (_) async => '2.0.0',
      readDigest: (_) async {
        activeChannel = channel(6);
        return target.imageSha256;
      },
      readAcceptance: (_) async => true,
      isChannelFresh: (LitchOtaControlChannel candidate) =>
          identical(candidate, activeChannel),
    );

    expect(result, isA<OtaPostRebootPending>());
  });

  test(
    'matching identity cannot replace missing or pending acceptance',
    () async {
      for (final accepted in <bool?>[null, false]) {
        final result = await verifier.verify(
          target: target,
          readAttemptProof: (_) async => proof(),
          openFreshChannel: () async => channel(5),

          readVersion: (_) async => target.version,
          readDigest: (_) async => target.imageSha256,
          readAcceptance: (_) async => accepted,
          isChannelFresh: (_) => true,
        );
        expect(result, isA<OtaPostRebootPending>());
        expect(result.message, contains('Keep it powered on'));
      }
    },
  );

  test(
    'completed empty reads retry boundedly; missing digest recovers automatically',
    () async {
      const recovery = OtaPostRebootVerifier(timeout: Duration(seconds: 2));
      int versions = 0;
      int digests = 0;
      final result = await recovery.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) async {
          ++versions;
          return target.version;
        },
        readDigest: (_) async => ++digests == 1 ? null : target.imageSha256,
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      expect(result, isA<OtaPostRebootVerified>());
      expect(versions, 1);
      expect(digests, 2);
      versions = 0;
      final missing = await recovery.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) async {
          ++versions;
          return null;
        },
        readDigest: (_) async =>
            fail('Missing version cannot authorize digest verification'),
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      expect(missing, isA<OtaPostRebootPending>());
      expect(versions, 3);
    },
  );

  test(
    'a replaced channel never becomes a mismatch or receives a retry',
    () async {
      bool fresh = true;
      int versions = 0;
      final result = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) async {
          ++versions;
          fresh = false;
          return 'old';
        },
        readDigest: (_) async =>
            fail('Retired channel cannot authorize digest verification'),
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => fresh,
      );
      expect(result, isA<OtaPostRebootPending>());
      expect(versions, 1);
    },
  );

  test(
    'a timed out native read is not overlapped by another attempt',
    () async {
      int versions = 0;
      final pendingRead = Completer<String?>();
      final result = await verifier.verify(
        target: target,
        readAttemptProof: (_) async => proof(),
        openFreshChannel: () async => channel(5),

        readVersion: (_) {
          ++versions;
          return pendingRead.future;
        },
        readDigest: (_) async =>
            fail('Timed out version cannot authorize verification'),
        readAcceptance: (_) async => true,
        isChannelFresh: (_) => true,
      );
      expect(result, isA<OtaPostRebootPending>());
      expect(versions, 1);
      pendingRead.complete(target.version);
    },
  );
}
