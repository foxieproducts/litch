import 'board_health_status.dart';
import 'litch_device_capabilities.dart';
import 'runtime_health_snapshot.dart';
import 'support_snapshot.dart';

enum SupportQueryOutcome {
  available,
  ownerNotReady,
  unavailable,
  timedOut,
  notAttemptedDeadline,
  sessionChanged,
  busy,
  canceled,
}

class SupportQueryResult<T> {
  const SupportQueryResult({required this.outcome, this.value});

  final SupportQueryOutcome outcome;
  final T? value;
}

/// One bounded observation of one connection, never a report history.
class SupportBoardCollection {
  const SupportBoardCollection({
    required this.incident,
    required this.firmwareDigest,
    required this.capabilities,
    required this.runtimeHealth,
    required this.firmwareVersion,
    this.publicHealth = const SupportQueryResult(
      outcome: SupportQueryOutcome.unavailable,
    ),
    this.actionHistory = const SupportQueryResult(
      outcome: SupportQueryOutcome.unavailable,
    ),
    this.boardHealth,
    this.sessionCurrent = false,
    this.deadlineReached = false,
  });

  factory SupportBoardCollection.unavailable({
    SupportQueryOutcome outcome = SupportQueryOutcome.unavailable,
  }) => SupportBoardCollection(
    publicHealth: SupportQueryResult(outcome: outcome),
    actionHistory: SupportQueryResult(outcome: outcome),
    incident: SupportQueryResult(outcome: outcome),
    firmwareDigest: SupportQueryResult(outcome: outcome),
    capabilities: SupportQueryResult(outcome: outcome),
    runtimeHealth: SupportQueryResult(outcome: outcome),
    firmwareVersion: SupportQueryResult(outcome: outcome),
  );

  final SupportQueryResult<SupportPublicHealth> publicHealth;
  final SupportQueryResult<SupportBoardHistory> actionHistory;
  final SupportQueryResult<SupportIncident> incident;
  final SupportQueryResult<String> firmwareDigest;
  final SupportQueryResult<LitchDeviceCapabilities> capabilities;
  final SupportQueryResult<RuntimeHealthResult> runtimeHealth;
  final SupportQueryResult<String> firmwareVersion;
  final BoardHealthStatus? boardHealth;
  final bool sessionCurrent;
  final bool deadlineReached;
}
