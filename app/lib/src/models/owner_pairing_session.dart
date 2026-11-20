import 'dart:typed_data';

/// Lightweight consent state carried by the shared connection session.
/// The dormant pattern protocol has a separate, incompatible state numbering.
enum OwnerPairingSessionState {
  unavailable,
  unpaired,
  awaitPhysical,
  securing,
  persistingOwner,
  ownerReady,
  ownedOther,
  clearing,
  failed,
  clearPrompt,
}

enum OwnerPairingSessionReason {
  none,
  malformed,
  unsupportedVersion,
  staleSession,
  physicalActionRequired,
  busy,
  mismatch,
  timeout,
  wrongConnection,
  securityFailed,
  storageFailed,
  bondDeleteFailed,
}

final class OwnerPairingSessionStatus {
  const OwnerPairingSessionStatus({
    required this.state,
    this.reason = OwnerPairingSessionReason.none,
    this.flags = 0,
    this.attempt = 0,
    this.physicalEnd = 0,
  });

  static const stateCode = 40;
  static const identityLowCode = 41;
  static const identityHighCode = 42;
  static const orderedOpcode = 27;
  static const eitherEnd = 2;
  final OwnerPairingSessionState state;
  final OwnerPairingSessionReason reason;
  final int flags;
  final int attempt;
  final int physicalEnd;
  bool get ownerRetained => flags & 1 != 0;
  bool get connectionOwnerReady => flags & 2 != 0;
  bool get identityDurable => flags & 4 != 0;
  bool get canAddPhone => flags & 8 != 0;
  bool get nativeBondAllowed => flags & 16 != 0;
  bool get canBegin =>
      state == OwnerPairingSessionState.unpaired || canAddPhone;
  bool get foreignOwnerRefused =>
      state == OwnerPairingSessionState.ownedOther &&
      reason == OwnerPairingSessionReason.wrongConnection &&
      ownerRetained &&
      !canAddPhone &&
      !connectionOwnerReady;
  bool get claimPending => switch (state) {
    OwnerPairingSessionState.awaitPhysical ||
    OwnerPairingSessionState.securing ||
    OwnerPairingSessionState.persistingOwner => true,
    _ => false,
  };

  static OwnerPairingSessionStatus? tryParse(List<int> body) {
    if (body.length != 8 ||
        body.any((value) => value < 0 || value > 255) ||
        body[0] != 1 ||
        body[1] >= OwnerPairingSessionState.values.length ||
        body[2] >= OwnerPairingSessionReason.values.length ||
        body[3] & ~31 != 0 ||
        body[6] > eitherEnd ||
        (body[6] == eitherEnd &&
            body[1] != OwnerPairingSessionState.awaitPhysical.index) ||
        body[7] != 0) {
      return null;
    }
    final status = OwnerPairingSessionStatus(
      state: OwnerPairingSessionState.values[body[1]],
      reason: OwnerPairingSessionReason.values[body[2]],
      flags: body[3],
      attempt: body[4] | body[5] << 8,
      physicalEnd: body[6],
    );
    if (status.nativeBondAllowed &&
        (status.state != OwnerPairingSessionState.securing ||
            !status.identityDurable)) {
      return null;
    }
    if (status.canAddPhone &&
        (status.state != OwnerPairingSessionState.ownedOther ||
            !status.ownerRetained ||
            !status.identityDurable)) {
      return null;
    }
    if (status.connectionOwnerReady &&
        (!status.ownerRetained ||
            !status.identityDurable ||
            (status.state != OwnerPairingSessionState.ownerReady &&
                status.state != OwnerPairingSessionState.clearPrompt))) {
      return null;
    }
    if (status.state == OwnerPairingSessionState.ownerReady &&
        !status.connectionOwnerReady) {
      return null;
    }
    if ((status.claimPending ||
            status.state == OwnerPairingSessionState.clearPrompt) &&
        status.attempt == 0) {
      return null;
    }
    return status;
  }

  static Uint8List command({
    required bool cancel,
    required int attempt,
    int physicalEnd = eitherEnd,
    bool phoneInitiated = false,
  }) {
    if (attempt < 1 ||
        attempt > 0xffff ||
        physicalEnd < 0 ||
        physicalEnd > eitherEnd ||
        (cancel && phoneInitiated)) {
      throw const FormatException('Invalid owner pairing command.');
    }
    return Uint8List.fromList([
      1,
      cancel
          ? 2
          : phoneInitiated
          ? 3
          : 1,
      attempt & 255,
      attempt >> 8,
      physicalEnd,
    ]);
  }
}
