enum LitchControlAction {
  hint('hint'),
  undo('undo'),
  newGame('new_game'),
  coaching('coaching');

  const LitchControlAction(this.wireToken);

  final String wireToken;

  static LitchControlAction? fromWireToken(String token) {
    for (final LitchControlAction action in values) {
      if (action.wireToken == token) {
        return action;
      }
    }
    return null;
  }
}

enum LitchControlActionState {
  unknown(null),
  idle('idle'),
  undoPending('undo_pending');

  const LitchControlActionState(this.wireToken);

  final String? wireToken;

  static LitchControlActionState? fromWireToken(String token) {
    for (final LitchControlActionState state in values) {
      if (state.wireToken == token) {
        return state;
      }
    }
    return null;
  }
}

enum LitchControlOwner {
  p1('p1'),
  p2('p2');

  const LitchControlOwner(this.wireToken);

  final String wireToken;
}

enum LitchMutationStatus {
  accepted(0, 'accepted'),
  applied(1, 'applied'),
  rejected(2, 'rejected'),
  noop(3, 'noop');

  const LitchMutationStatus(this.wireCode, this.wireToken);

  final int wireCode;
  final String wireToken;

  bool get isTerminal => this != LitchMutationStatus.accepted;

  static LitchMutationStatus? fromWireCode(int code) {
    for (final LitchMutationStatus status in values) {
      if (status.wireCode == code) {
        return status;
      }
    }
    return null;
  }

  static LitchMutationStatus? fromWireToken(String token) {
    for (final LitchMutationStatus status in values) {
      if (status.wireToken == token) {
        return status;
      }
    }
    return null;
  }
}

enum LitchMutationReason {
  none(0, 'none'),
  badRequest(1, 'bad_request'),
  unknownAction(2, 'unknown_action'),
  queueFull(3, 'queue_full'),
  unknownKey(4, 'unknown_key'),
  typeMismatch(5, 'type_mismatch'),
  outOfRange(6, 'out_of_range'),
  invalidEnumValue(7, 'invalid_enum_value'),
  notAtStart(8, 'not_at_start'),
  busy(9, 'busy'),
  noHistory(10, 'no_history'),
  gameOver(11, 'game_over'),
  unsupported(12, 'unsupported'),
  engineRejected(13, 'engine_rejected'),
  storageUnavailable(14, 'storage_unavailable'),
  archiveRequired(15, 'archive_required');

  const LitchMutationReason(this.wireCode, this.wireToken);

  final int wireCode;
  final String wireToken;

  static LitchMutationReason? fromWireCode(int code) {
    for (final LitchMutationReason reason in values) {
      if (reason.wireCode == code) {
        return reason;
      }
    }
    return null;
  }

  static LitchMutationReason? fromWireToken(String token) {
    for (final LitchMutationReason reason in values) {
      if (reason.wireToken == token) {
        return reason;
      }
    }
    return null;
  }
}

class LitchControlOutcome {
  const LitchControlOutcome({
    required this.requestId,
    required this.action,
    required this.status,
    required this.reason,
    this.actionState = LitchControlActionState.unknown,
  });

  final int requestId;
  final LitchControlAction action;
  final LitchMutationStatus status;
  final LitchMutationReason reason;
  final LitchControlActionState actionState;

  bool get isTerminal => status.isTerminal;
}

enum LitchControlPhase {
  pending,
  accepted,
  applied,
  rejected,
  noop,
  timeout,
  sendFailed,
  unavailable,
  interrupted,
}

enum LitchControlAvailability { unknown, available, unavailable }

class LitchControlEvent {
  const LitchControlEvent({
    required this.action,
    required this.phase,
    this.requestId,
    this.owner,
    this.reason,
    this.actionState = LitchControlActionState.unknown,
  });

  factory LitchControlEvent.fromOutcome(
    LitchControlOutcome outcome, {
    LitchControlOwner? owner,
  }) {
    final LitchControlPhase phase = switch (outcome.status) {
      LitchMutationStatus.accepted => LitchControlPhase.accepted,
      LitchMutationStatus.applied => LitchControlPhase.applied,
      LitchMutationStatus.rejected => LitchControlPhase.rejected,
      LitchMutationStatus.noop => LitchControlPhase.noop,
    };
    return LitchControlEvent(
      requestId: outcome.requestId,
      action: outcome.action,
      owner: owner,
      phase: phase,
      reason: outcome.reason,
      actionState: outcome.actionState,
    );
  }

  final int? requestId;
  final LitchControlAction action;
  final LitchControlOwner? owner;
  final LitchControlPhase phase;
  final LitchMutationReason? reason;
  final LitchControlActionState actionState;

  bool get isPending =>
      phase == LitchControlPhase.pending || phase == LitchControlPhase.accepted;

  bool get isTerminal => !isPending;

  bool get isSuccess =>
      phase == LitchControlPhase.applied || phase == LitchControlPhase.noop;

  bool get isLocalFailure =>
      phase == LitchControlPhase.timeout ||
      phase == LitchControlPhase.sendFailed ||
      phase == LitchControlPhase.unavailable ||
      phase == LitchControlPhase.interrupted;

  String? get reasonToken => reason?.wireToken;
}
