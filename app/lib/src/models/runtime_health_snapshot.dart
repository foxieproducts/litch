/// The board-owned runtime-health state encoded in the BLE health reply.
enum RuntimeHealthState {
  healthy,
  degradedUsable,
  inputPaused,
  protectionLatched,
  unknown,
}

/// Append-only runtime-health causes defined by the board contract.
enum RuntimeHealthCause {
  oneAmbientSensor,
  noAmbientSensors,
  boardScanUncertain,
  sourceUnstable,
  brownoutLatched,
  severeProtectionLatched,
}

/// Immutable board snapshot. The app presents this value; it does not derive
/// health state, debounce facts, or apply recovery policy.
class RuntimeHealthSnapshot {
  const RuntimeHealthSnapshot({
    required this.revision,
    required this.stateCode,
    required this.state,
    required this.causeMask,
  });

  static const Map<RuntimeHealthCause, int> _causeBits =
      <RuntimeHealthCause, int>{
        RuntimeHealthCause.oneAmbientSensor: 1 << 0,
        RuntimeHealthCause.noAmbientSensors: 1 << 1,
        RuntimeHealthCause.boardScanUncertain: 1 << 2,
        RuntimeHealthCause.sourceUnstable: 1 << 3,
        RuntimeHealthCause.brownoutLatched: 1 << 4,
        RuntimeHealthCause.severeProtectionLatched: 1 << 5,
      };

  final int revision;
  final int stateCode;
  final RuntimeHealthState state;
  final int causeMask;

  /// Stable raw support code: `RRRR-S-CCCC`.
  String get supportCode =>
      '${_hex(revision)}-${stateCode.toRadixString(16).toUpperCase()}-${_hex(causeMask)}';

  List<RuntimeHealthCause> get knownCauses => _causeBits.entries
      .where(
        (MapEntry<RuntimeHealthCause, int> entry) =>
            (causeMask & entry.value) != 0,
      )
      .map((MapEntry<RuntimeHealthCause, int> entry) => entry.key)
      .toList(growable: false);

  int get unknownCauseMask => causeMask & ~_knownCauseMask;

  bool get isAbnormal => state != RuntimeHealthState.healthy;

  static RuntimeHealthSnapshot? fromToken(String token) {
    if (!RegExp(r'^[0-9A-F]{9}$').hasMatch(token)) {
      return null;
    }
    final int revision = int.parse(token.substring(0, 4), radix: 16);
    final int stateCode = int.parse(token.substring(4, 5), radix: 16);
    final int causeMask = int.parse(token.substring(5), radix: 16);
    return RuntimeHealthSnapshot(
      revision: revision,
      stateCode: stateCode,
      state: switch (stateCode) {
        0 => RuntimeHealthState.healthy,
        1 => RuntimeHealthState.degradedUsable,
        2 => RuntimeHealthState.inputPaused,
        3 => RuntimeHealthState.protectionLatched,
        _ => RuntimeHealthState.unknown,
      },
      causeMask: causeMask,
    );
  }

  static int get _knownCauseMask =>
      _causeBits.values.fold(0, (int mask, int bit) => mask | bit);

  static String _hex(int value) =>
      value.toRadixString(16).toUpperCase().padLeft(4, '0');
}

enum RuntimeHealthUnavailableReason {
  unsupported,
  timedOut,
  malformed,
  unknownCommand,
  disconnected,
  sendFailed,
}

sealed class RuntimeHealthResult {
  const RuntimeHealthResult();
}

final class RuntimeHealthAvailable extends RuntimeHealthResult {
  const RuntimeHealthAvailable(this.snapshot);

  final RuntimeHealthSnapshot snapshot;
}

final class RuntimeHealthUnavailable extends RuntimeHealthResult {
  const RuntimeHealthUnavailable(this.reason);

  final RuntimeHealthUnavailableReason reason;
}

enum RuntimeHealthRecheckOutcome {
  succeeded,
  validationFailed,
  unsupported,
  timedOut,
  disconnected,
  sendFailed,
  malformed,
}

class RuntimeHealthRecheckResult {
  const RuntimeHealthRecheckResult({
    required this.outcome,
    required this.health,
  });

  final RuntimeHealthRecheckOutcome outcome;
  final RuntimeHealthResult health;

  bool get succeeded => outcome == RuntimeHealthRecheckOutcome.succeeded;
}
