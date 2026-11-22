import 'dart:async';

import 'litch_ota_control_channel.dart';
import 'ota_attempt_proof.dart';

export 'ota_attempt_proof.dart';

class OtaVerificationTarget {
  const OtaVerificationTarget({
    required this.deviceId,
    required this.uploadConnectionToken,
    required this.version,
    required this.imageSha256,
    this.artifactSha256,
    this.attempt,
  });

  final String deviceId;
  // Captured from the connection owner before upload, not its notifications.
  final int uploadConnectionToken;
  final String version;
  final String imageSha256;
  final String? artifactSha256;
  final OtaAttemptIdentity? attempt;
}

sealed class OtaPostRebootVerification {
  const OtaPostRebootVerification(this.message);

  final String message;
}

class OtaPostRebootVerified extends OtaPostRebootVerification {
  const OtaPostRebootVerified()
    : super('Selected firmware verified on this board.');
}

class OtaPostRebootPending extends OtaPostRebootVerification {
  const OtaPostRebootPending(super.message);
}

class OtaPostRebootMismatch extends OtaPostRebootVerification {
  const OtaPostRebootMismatch(super.message);
}

/// The same board reports a different accepted running image.
class OtaPostRebootDifferentImage extends OtaPostRebootMismatch {
  OtaPostRebootDifferentImage({
    required this.version,
    required this.imageSha256,
  }) : super(
         'The selected firmware is not running. The board is running $version. '
         'You can try the update again.',
       );

  final String version;
  final String imageSha256;
}

/// The admitted trial failed while its exact preserved source is accepted.
class OtaPostRebootFailedAttempt extends OtaPostRebootDifferentImage {
  OtaPostRebootFailedAttempt({
    required this.expected,
    required this.observed,
    required super.version,
    required super.imageSha256,
  });

  final OtaAttemptIdentity expected;
  final OtaAttemptProof observed;

  @override
  String get message => observed.bootNonce == expected.preBootNonce
      ? 'The update stopped before the new firmware was activated. '
            'You can try the update again.'
      : 'The board returned to its previous firmware after this update failed. '
            'You can try the update again.';
}

typedef OtaAttemptProofReader =
    Future<OtaAttemptProof?> Function(LitchOtaControlChannel);
typedef OtaFreshChannel = Future<LitchOtaControlChannel?> Function();
typedef OtaVersionReader = Future<String?> Function(LitchOtaControlChannel);
typedef OtaDigestReader = Future<String?> Function(LitchOtaControlChannel);
// True is accepted, false is explicitly pending, and null is unavailable.
typedef OtaAcceptanceReader = Future<bool?> Function(LitchOtaControlChannel);
typedef OtaChannelFreshness = bool Function(LitchOtaControlChannel);

/// Verifies selected running firmware on a new connection to the same board.
/// Neither a reconnect nor renewing notifications establishes a reboot.
/// It deliberately treats missing data as retryable rather than guessing that a
/// board installed the release without proof.
class OtaPostRebootVerifier {
  const OtaPostRebootVerifier({this.timeout = const Duration(seconds: 20)});

  final Duration timeout;

  Future<OtaPostRebootVerification> verify({
    required OtaVerificationTarget target,
    required OtaFreshChannel openFreshChannel,
    required OtaVersionReader readVersion,
    required OtaDigestReader readDigest,
    required OtaAcceptanceReader readAcceptance,
    required OtaChannelFreshness isChannelFresh,
    OtaAttemptProofReader? readAttemptProof,
  }) async {
    final attempt = target.attempt;
    if (attempt == null || !attempt.isValid || readAttemptProof == null) {
      return const OtaPostRebootPending(
        'Exact update-attempt evidence is unavailable; update remains unverified.',
      );
    }
    final LitchOtaControlChannel? channel;
    try {
      channel = await openFreshChannel().timeout(timeout);
    } on TimeoutException {
      return const OtaPostRebootPending(
        'Waiting for this board to reconnect before verification.',
      );
    } on StateError {
      return const OtaPostRebootPending(
        'Waiting for this board to reconnect before verification.',
      );
    }
    if (channel == null ||
        channel.deviceId != target.deviceId ||
        channel.connectionToken < target.uploadConnectionToken ||
        !isChannelFresh(channel)) {
      return const OtaPostRebootPending(
        'Waiting for this board to reconnect before verification.',
      );
    }

    final String? version;
    try {
      version = await _readWithRecovery(
        channel,
        readVersion,
        isChannelFresh,
        retryWhile: (value) => value == null || value.isEmpty,
      );
    } on TimeoutException {
      return const OtaPostRebootPending(
        'The reconnected board did not provide a firmware version yet.',
      );
    } on StateError {
      return const OtaPostRebootPending(
        'The reconnected board did not provide a firmware version yet.',
      );
    }
    if (!isChannelFresh(channel)) {
      return const OtaPostRebootPending(
        'The board connection changed during verification; retry after reconnecting.',
      );
    }
    if (version == null || version.isEmpty) {
      return const OtaPostRebootPending(
        'The reconnected board did not provide a firmware version yet.',
      );
    }

    const pendingAcceptance = OtaPostRebootPending(
      'The board has not confirmed this update is safe to keep yet. '
      'Keep it powered on, then retry verification.',
    );
    final OtaAttemptProof? proof;
    try {
      proof = await _readWithRecovery(
        channel,
        readAttemptProof,
        isChannelFresh,
        retryWhile: (value) =>
            value == null ||
            (!value.runtimeAccepted &&
                !_isExactAdmissionFailure(attempt, value)),
        maximumAttempts: null,
      );
    } on TimeoutException {
      return pendingAcceptance;
    } on StateError {
      return const OtaPostRebootPending(
        'Exact update-attempt evidence is unavailable; update remains unverified.',
      );
    }
    if (proof == null || !isChannelFresh(channel)) {
      return const OtaPostRebootPending(
        'Exact update-attempt evidence is unavailable; update remains unverified.',
      );
    }
    // The accepted boot's digest reuses its complete health integrity proof.
    // Asking before that proof completes can start a second image scan.
    final String? digest;
    try {
      digest = await _readWithRecovery(
        channel,
        readDigest,
        isChannelFresh,
        retryWhile: (value) => value == null || value.isEmpty,
      );
    } on TimeoutException {
      return const OtaPostRebootPending(
        'This firmware cannot provide its image digest yet; update remains unverified.',
      );
    } on StateError {
      return const OtaPostRebootPending(
        'This firmware cannot provide its image digest yet; update remains unverified.',
      );
    }
    if (!isChannelFresh(channel)) {
      return const OtaPostRebootPending(
        'The board connection changed during verification; retry after reconnecting.',
      );
    }
    if (digest == null || !RegExp(r'^[0-9a-fA-F]{64}$').hasMatch(digest)) {
      return const OtaPostRebootPending(
        'This firmware cannot provide its image digest yet; update remains unverified.',
      );
    }
    if (isChannelFresh(channel) && _isExactAdmissionFailure(attempt, proof)) {
      return OtaPostRebootFailedAttempt(
        expected: attempt,
        observed: proof,
        version: version,
        imageSha256: digest,
      );
    }
    if (channel.connectionToken <= target.uploadConnectionToken) {
      return const OtaPostRebootPending(
        'Waiting for this board to reconnect before verification.',
      );
    }
    try {
      final accepted = await _readWithRecovery(
        channel,
        readAcceptance,
        isChannelFresh,
        retryWhile: (value) => value == false,
        maximumAttempts: null,
      );
      if (accepted != true) {
        return pendingAcceptance;
      }
    } on TimeoutException {
      return pendingAcceptance;
    } on StateError {
      return pendingAcceptance;
    }
    if (!isChannelFresh(channel) ||
        proof.bootNonce == attempt.preBootNonce ||
        !proof.runtimeAccepted ||
        !proof.hasCoherentSelection ||
        proof.runningState != 2) {
      return const OtaPostRebootPending(
        'Waiting for a new boot with exact update-attempt evidence.',
      );
    }
    final source = proof.selectors[attempt.sourceSector];
    final trial = proof.selectors[attempt.targetSector];
    if (!source.crcValid ||
        source.state != 2 ||
        source.sequence != attempt.sourceSequence ||
        !trial.crcValid ||
        trial.sequence != attempt.expectedSequence) {
      return const OtaPostRebootPending(
        'The board has not provided the exact preserved update selectors.',
      );
    }
    if ((trial.state == 3 || trial.state == 4) &&
        proof.runningSlot == attempt.sourceSlot &&
        proof.selectedSlot == attempt.sourceSlot &&
        proof.selectedSector == attempt.sourceSector &&
        proof.selectedSequence == attempt.sourceSequence) {
      return OtaPostRebootFailedAttempt(
        expected: attempt,
        observed: proof,
        version: version,
        imageSha256: digest,
      );
    }
    if (trial.state != 2 ||
        proof.runningSlot != attempt.targetSlot ||
        proof.selectedSlot != attempt.targetSlot ||
        proof.selectedSector != attempt.targetSector ||
        proof.selectedSequence != attempt.expectedSequence) {
      return const OtaPostRebootPending(
        'The board has not confirmed the exact selected update attempt.',
      );
    }
    if (version != target.version ||
        digest.toLowerCase() != target.imageSha256.toLowerCase()) {
      return OtaPostRebootDifferentImage(version: version, imageSha256: digest);
    }
    return const OtaPostRebootVerified();
  }

  bool _isExactAdmissionFailure(
    OtaAttemptIdentity expected,
    OtaAttemptProof proof,
  ) {
    if (proof.bootNonce != expected.preBootNonce ||
        !proof.hasCoherentSelection ||
        proof.runningState != 2 ||
        proof.runningSlot != expected.sourceSlot ||
        proof.selectedSlot != expected.sourceSlot ||
        proof.selectedSector != expected.sourceSector ||
        proof.selectedSequence != expected.sourceSequence) {
      return false;
    }
    final source = proof.selectors[expected.sourceSector];
    final admission = proof.admission;
    return source.crcValid &&
        source.state == 2 &&
        source.sequence == expected.sourceSequence &&
        (admission.phase == 3 || admission.phase == 4) &&
        admission.attempt == expected.attempt &&
        admission.sourceSlot == expected.sourceSlot &&
        admission.targetSlot == expected.targetSlot &&
        admission.sourceSequence == expected.sourceSequence &&
        admission.expectedSequence == expected.expectedSequence;
  }

  // A successful GATT write does not prove its notification was received.
  // Recover empty identity reads or explicit pending acceptance on this exact
  // post-reboot channel only, under the same bounded recovery owner.
  // An unresolved timed-out read is not retried: it may still own native work.
  Future<T?> _readWithRecovery<T>(
    LitchOtaControlChannel channel,
    Future<T?> Function(LitchOtaControlChannel) read,
    OtaChannelFreshness isChannelFresh, {
    required bool Function(T?) retryWhile,
    int? maximumAttempts = 3,
  }) async {
    const Duration retryDelay = Duration(milliseconds: 350);
    bool active = true;
    Future<T?> recover() async {
      for (int attempt = 0; ; ++attempt) {
        if (!active || !isChannelFresh(channel)) {
          return null;
        }
        final T? value = await read(channel);
        if (!active || !isChannelFresh(channel)) {
          return null;
        }
        if (!retryWhile(value)) {
          return value;
        }
        if (maximumAttempts != null && attempt + 1 >= maximumAttempts) {
          return null;
        }
        await Future<void>.delayed(retryDelay);
      }
    }

    try {
      return await recover().timeout(timeout);
    } finally {
      // A timed-out native read may still finish; it cannot start another read.
      active = false;
    }
  }
}
