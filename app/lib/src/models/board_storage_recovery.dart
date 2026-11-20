import 'dart:typed_data';

/// Board-owned storage recovery truth. Unknown is local uncertainty and never
/// a firmware status or permission to replay the destructive action.
enum BoardStorageRecoveryStatus {
  unavailable,
  idle,
  busy,
  pending,
  running,
  succeeded,
  failed,
  unknown,
}

class BoardStorageRecoverySnapshot {
  const BoardStorageRecoverySnapshot({
    required this.status,
    this.rebootRequired = false,
  });

  static const int stateCode = 38;
  static const unavailable = BoardStorageRecoverySnapshot(
    status: BoardStorageRecoveryStatus.unavailable,
  );
  static const unknown = BoardStorageRecoverySnapshot(
    status: BoardStorageRecoveryStatus.unknown,
  );

  final BoardStorageRecoveryStatus status;
  final bool rebootRequired;

  bool get isInProgress =>
      status == BoardStorageRecoveryStatus.pending ||
      status == BoardStorageRecoveryStatus.running;

  /// Both paths require a fresh explicit destructive confirmation.
  bool get canReformat =>
      status == BoardStorageRecoveryStatus.idle ||
      status == BoardStorageRecoveryStatus.failed;

  bool get provesSupport =>
      status != BoardStorageRecoveryStatus.unavailable &&
      status != BoardStorageRecoveryStatus.unknown;

  static BoardStorageRecoverySnapshot? tryParse(Uint8List body) {
    if (body.length != 3 ||
        body[0] != 1 ||
        body[1] > 6 ||
        body[2] != (body[1] == 5 ? 1 : 0)) {
      return null;
    }
    return BoardStorageRecoverySnapshot(
      status: BoardStorageRecoveryStatus.values[body[1]],
      rebootRequired: body[2] == 1,
    );
  }
}
