class LitchSupportSessionDiagnostics {
  const LitchSupportSessionDiagnostics({
    required this.settingsWriteAttempts,
    required this.settingsWriteSucceeded,
    required this.settingsWriteFailed,
    this.lastSettingsOperation,
    this.lastSettingsFailureToken,
    this.settingReads = const SettingReadDiagnosticsSnapshot(),
    this.controlNativeWrites = 0,
    this.controlTimeouts = 0,
    this.controlIntents = 0,
    this.controlCoalescedTaps = 0,
    this.controlRecoveryReadRequests = 0,
    this.controlBusyRetries = 0,
    this.controlRecoveryConfirmed = 0,
    this.controlRecoveryUnconfirmed = 0,
    this.notificationRepairOperations = 0,
    this.notificationRepairAttempts = 0,
    this.notificationRepairRecovered = 0,
    this.notificationRepairExhausted = 0,
    this.notificationRepairCancelled = 0,
    this.notificationRepairSaturated = false,
  });

  final SettingReadDiagnosticsSnapshot settingReads;
  final int settingsWriteAttempts;
  final int settingsWriteSucceeded;
  final int settingsWriteFailed;
  final String? lastSettingsOperation;
  final String? lastSettingsFailureToken;

  /// Counts since this app service started, across its connections.
  final int controlNativeWrites;
  final int controlTimeouts;
  final int controlIntents;
  final int controlCoalescedTaps;
  final int controlRecoveryReadRequests;
  final int controlBusyRetries;
  final int controlRecoveryConfirmed;
  final int controlRecoveryUnconfirmed;

  /// Bounded automatic Litchcraft notification-repair sequences since this
  /// app BLE service was created. Outcomes are disjoint and terminal.
  final int notificationRepairOperations;
  final int notificationRepairAttempts;
  final int notificationRepairRecovered;
  final int notificationRepairExhausted;
  final int notificationRepairCancelled;
  final bool notificationRepairSaturated;

  int get settingsWriteCompleted =>
      settingsWriteSucceeded + settingsWriteFailed;

  int get notificationRepairCompleted =>
      notificationRepairRecovered +
      notificationRepairExhausted +
      notificationRepairCancelled;

  int get notificationRepairPending =>
      (notificationRepairOperations - notificationRepairCompleted)
          .clamp(0, 0xffffffff)
          .toInt();
}

/// Fixed counters since the owning BLE service was constructed.
class SettingReadDiagnosticsSnapshot {
  const SettingReadDiagnosticsSnapshot({
    this.operations = 0,
    this.directAttempts = 0,
    this.fallbackEntries = 0,
    this.retryAttempts = 0,
    this.directSuperseded = 0,
    this.succeeded = 0,
    this.recovered = 0,
    this.exhausted = 0,
    this.cancelled = 0,
    this.unknown = 0,
    this.totalDurationMs = 0,
    this.maxDurationMs = 0,
    this.saturated = false,
  });

  final int operations;
  final int directAttempts;
  final int fallbackEntries;
  final int retryAttempts;
  final int directSuperseded;
  final int succeeded;
  final int recovered;
  final int exhausted;
  final int cancelled;
  final int unknown;
  final int totalDurationMs;
  final int maxDurationMs;
  final bool saturated;

  int get completed => succeeded + recovered + exhausted + cancelled + unknown;
  int get pending => (operations - completed).clamp(0, 0xffffffff).toInt();
}
