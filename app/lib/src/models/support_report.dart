import 'board_health_status.dart';
import 'litch_device_capabilities.dart';
import 'runtime_health_snapshot.dart';
import 'support_board_collection.dart';
import 'support_diagnostics.dart';
import 'support_phone_diagnostics.dart';
import 'support_action.dart';
import 'support_snapshot.dart';

class SupportAppInfo {
  const SupportAppInfo({
    required this.version,
    required this.buildNumber,
    required this.platform,
    required this.osVersion,
  });

  final String version;
  final String buildNumber;
  final String platform;
  final String osVersion;
}

enum SupportQueryField {
  publicHealth,
  actionHistory,
  incident,
  firmwareDigest,
  capabilities,
  runtimeHealth,
  firmwareVersion,
}

class SupportBoardInfo {
  const SupportBoardInfo({
    required this.connected,
    this.firmwareVersion,
    this.capabilities,
    this.runtimeHealth,
    this.publicHealth,
    this.actionHistory,
    this.ownerState,
    this.ownerReason,
    this.incident,
    this.boardHealth,
    this.firmwareDigest,
    this.collectionStatus = 'current_session',
    this.queryOutcomes = const <SupportQueryField, SupportQueryOutcome>{},
  });

  final bool connected;
  final String? firmwareVersion;
  final LitchDeviceCapabilities? capabilities;
  final RuntimeHealthResult? runtimeHealth;
  final SupportPublicHealth? publicHealth;
  final SupportBoardHistory? actionHistory;
  final String? ownerState, ownerReason;
  final SupportIncident? incident;
  final BoardHealthStatus? boardHealth;
  final String? firmwareDigest;
  final String collectionStatus;
  final Map<SupportQueryField, SupportQueryOutcome> queryOutcomes;
}

class SupportReportDraft {
  const SupportReportDraft({
    required this.recipient,
    required this.subject,
    required this.body,
    this.diagnostics,
    this.healthContext = false,
  });

  static const int maxSubjectCharacters = 160;
  static const int maxBodyCharacters = 6000;

  final String recipient;
  final String subject;
  final String body;
  // Structured source boundary for app-owned default questions. User-supplied
  // drafts omit it and are never rewritten by locale changes.
  final String? diagnostics;
  final bool healthContext;
}

class SupportReportBuilder {
  const SupportReportBuilder();

  static const String recipient = 'support@foxieproducts.com';

  SupportReportDraft build({
    required SupportAppInfo app,
    required SupportBoardInfo board,
    required LitchSupportSessionDiagnostics session,
    SupportPhoneDiagnostics? phone,
    SupportAppActionSnapshot? appActions,
  }) {
    final RuntimeHealthSnapshot? health = switch (board.runtimeHealth) {
      RuntimeHealthAvailable(:final snapshot) => snapshot,
      _ => null,
    };
    final bool healthContext = health?.isAbnormal ?? false;
    final String subject = healthContext
        ? 'Foxie Litch support - Board health'
        : 'Foxie Litch support';

    final List<String> lines = <String>[
      'Please add your answers below. Leave any question unanswered if you are not sure.',
      '',
      'What were you trying to do?',
      '',
      'What happened, and what did you expect?',
      '',
      'Can you reproduce it?',
      '',
      'Which USB host, charger, and cable are connected?',
      '',
      'What LED pattern do you see?',
      '',
      'When did it start, or what changed?',
      '',
      'Anything else that may be relevant?',
      '',
      '--- Litch diagnostics (review or remove anything before sending) ---',
      'diagnostic_schema: 3',
      'collection_status: ${_oneLine(board.collectionStatus, 48)}',
      'board_query_outcomes: ${SupportQueryField.values.map((field) => '${field.name}=${board.queryOutcomes[field]?.name ?? 'unavailable'}').join(', ')}',
      'firmware_sha256: ${_optional(board.firmwareDigest, 64)}',
      'board_persistence: ${board.boardHealth?.persistence?.name ?? (board.boardHealth == null ? 'unavailable' : 'no_active_failure')}',
      'incident_snapshot: ${board.incident?.diagnosticText ?? 'unavailable'}',
      ..._incidentSummary(board.incident, board.publicHealth),
      ..._runtimeLines(board),
      ..._actionLines(appActions, board.actionHistory),
      'privacy: reviewable_recent_actions_no_ble_device_id_no_credentials_no_fen_or_move_history_no_arbitrary_logs; no_names_colors_exact_squares',
      'app_version: ${_oneLine(app.version, 64)}',
      'app_build: ${_oneLine(app.buildNumber, 32)}',
      'platform: ${_oneLine(app.platform, 32)}',
      'os_version: ${_oneLine(app.osVersion, 240)}',
      ..._phoneLines(phone),
      'board_connection: ${board.connected ? 'connected' : 'disconnected'}',
      'board_firmware: ${_optional(board.firmwareVersion, 96)}',
      ..._capabilityLines(board.capabilities),
      ..._healthLines(board.runtimeHealth),
      ..._settingReadLines(session.settingReads),
      'app_settings_write_attempts: ${session.settingsWriteAttempts}',
      'app_settings_write_completed: ${session.settingsWriteCompleted}',
      'app_settings_write_succeeded: ${session.settingsWriteSucceeded}',
      'app_settings_write_failed: ${session.settingsWriteFailed}',
      'last_app_settings_operation: ${_optional(session.lastSettingsOperation, 96)}',
      'last_app_settings_failure: ${_optional(session.lastSettingsFailureToken, 96)}',
      'app_control_counter_scope: service_lifetime',
      'app_control_native_writes: ${session.controlNativeWrites}',
      'app_control_timeouts: ${session.controlTimeouts}',
      'app_control_intents: ${session.controlIntents}',
      'app_control_coalesced_taps: ${session.controlCoalescedTaps}',
      'app_control_recovery_read_requests: ${session.controlRecoveryReadRequests}',
      'app_control_busy_retries: ${session.controlBusyRetries}',
      'app_control_recovery_confirmed: ${session.controlRecoveryConfirmed}',
      'app_control_recovery_unconfirmed: ${session.controlRecoveryUnconfirmed}',
      'app_notification_repair_scope: service_lifetime; automatic_litchcraft_sequences; retry_attempts_only',
      'app_notification_repairs: ops=${session.notificationRepairOperations}, pending=${session.notificationRepairPending}, attempts=${session.notificationRepairAttempts}, recovered=${session.notificationRepairRecovered}, exhausted=${session.notificationRepairExhausted}, cancelled=${session.notificationRepairCancelled}, saturated=${session.notificationRepairSaturated}',
      'firmware_settings_storage_error: ${board.incident?.fields['se'] ?? 'unavailable'}',
      'firmware_settings_retry_count: unavailable_over_structured_ble',
      'firmware_settings_completion_count: ${board.incident?.fields['saved'] ?? 'unavailable'}',
      'usb_cc_raw_and_confirmation: unavailable_over_structured_ble',
    ];
    // Drop complete low-priority tail lines, with an explicit omission count;
    // never cut an action, required field or Unicode character mid-line.
    var omittedLines = 0;
    const omissionReserve = 60;
    while (lines.join('\n').length >
        SupportReportDraft.maxBodyCharacters - omissionReserve) {
      lines.removeLast();
      omittedLines++;
    }
    if (omittedLines > 0) {
      lines.add(
        'optional_diagnostic_lines_omitted: $omittedLines (report size limit)',
      );
    }
    final body = lines.join('\n');
    return SupportReportDraft(
      recipient: recipient,
      subject: _bounded(subject, SupportReportDraft.maxSubjectCharacters),
      body: body,
      diagnostics: body.substring(body.indexOf('diagnostic_schema: 3')),
      healthContext: healthContext,
    );
  }

  List<String> _settingReadLines(
    SettingReadDiagnosticsSnapshot reads,
  ) => <String>[
    'app_setting_read_scope: service_lifetime; logical_routes; other_recovery_and_enumeration_io=unmeasured',
    'app_setting_reads: ops=${reads.operations}, pending=${reads.pending}, direct=${reads.directAttempts}, fallback=${reads.fallbackEntries}, retry=${reads.retryAttempts}, direct_superseded=${reads.directSuperseded}, succeeded=${reads.succeeded}, recovered=${reads.recovered}, exhausted=${reads.exhausted}, cancelled=${reads.cancelled}, unknown=${reads.unknown}, ms_sum=${reads.totalDurationMs}, ms_max=${reads.maxDurationMs}, saturated=${reads.saturated}',
  ];

  List<String> _runtimeLines(SupportBoardInfo board) {
    final h = board.publicHealth, s = board.actionHistory;
    return [
      'firmware_uptime_ms64: ${h != null && h.validity & 4 != 0 ? h.uptimeMs : s?.uptimeMs ?? 'unavailable'}',
      if (h == null)
        'public_support: unavailable${board.ownerState == 'clearing' ? '; full_snapshot_unavailable_until_session_renewed' : ''}'
      else
        'public_support: health=${h.validity & 1 != 0}, hardware=${h.validity & 2 != 0 ? '${h.hardwareMajor}.${h.hardwareMinor}' : 'unavailable'}, reset=${h.resetReason}, storage=${h.validity & 8 != 0 ? '${h.storageError}, boot=${h.bootLoadError}' : 'unavailable'}, image_digest_state=${h.digestAvailability}',
      if (h != null)
        'board_startup: mode=${h.recoveryMode == 1 ? 'recovery' : 'normal'}, cause=${h.recoveryCauseToken}, recovery_state=${h.recoveryState}, action_failure=${h.recoveryFailure}, boots_rtc=${h.boots}, early_crashes_rtc=${h.earlyCrashes}',
      if (h != null)
        'recovery_services: updater=${const ['unknown', 'unavailable', 'ready', 'active'][h.updater]}, settings=${const ['not_attempted', 'not_loaded_in_recovery', 'load_attempted'][h.storageAccess]}; disabled_before_load_does_not_prove_corruption',
      if (h?.priorStageValid == true)
        'prior_loop_stage: ${h!.priorStage} at ${h.priorStageAtMs}ms; checkpoint_only_not_crash_cause',
      'owner_status: ${board.ownerState ?? 'unavailable'}, reason=${board.ownerReason ?? 'unavailable'}',
      'firmware_settings_saved_total: ${s?.saveTotal ?? 'unavailable'}; successful promoted saves',
      'firmware_runtime_metrics: ${s?.available == true ? 'boots=${s!.boots}, runtime_minutes_approx=${(s.priorRuntimeMs + s.uptimeMs) ~/ 60000}, last_saved_uptime_ms=${s.savedCheckpointMs}, sleep_wakes_auto=${s.automaticSleeps}, manual_menu_app=${s.manualSleeps}, other_console=${s.otherSleeps}' : 'unavailable'}',
      if (s?.available == true)
        'runtime_metric_scope: since_introduction_or_settings_reset; unsaved_boots_runtime_tail_and_sleep_counts_may_be_lost; no_boot_or_periodic_metric_saves',
    ];
  }

  List<String> _actionLines(
    SupportAppActionSnapshot? app,
    SupportBoardHistory? board,
  ) {
    if (app == null && board == null) {
      return [
        'Recent actions: unavailable (up to20 total)',
        'App actions: unavailable; Board actions: unavailable',
        'clock_sample: unavailable; source timelines cannot be synchronized',
      ];
    }
    final appRows = app?.actions ?? const <SupportAction>[];
    final boardRows = board?.actions ?? const <SupportAction>[];
    var appCount = appRows.length.clamp(0, boardRows.isEmpty ? 20 : 10);
    var boardCount = boardRows.length.clamp(0, 20 - appCount);
    appCount = appRows.length.clamp(0, 20 - boardCount);
    final selectedApp = appRows.skip(appRows.length - appCount).toList();
    final selectedBoard = boardRows
        .skip(boardRows.length - boardCount)
        .toList();
    final omitted =
        (app?.omitted ?? 0) +
        (board?.omitted ?? 0) +
        appRows.length -
        appCount +
        boardRows.length -
        boardCount;
    final lines = <String>[
      'Recent actions: ${appCount + boardCount}/20 max; separate app/board monotonic timelines; omitted=$omitted',
      'Requested or accepted actions may still be pending. Applied means the change took effect at its source. A durable save needs separate confirmation.',
    ];
    if (board?.appRequestedMs != null &&
        board?.appReceivedMs != null &&
        board!.appReceivedMs! >= board.appRequestedMs!) {
      lines.add(
        'clock_sample: board snapshot uptime ${board.uptimeMs}ms occurred within app ${board.appRequestedMs}..${board.appReceivedMs}ms; RTT uncertainty=${board.appReceivedMs! - board.appRequestedMs!}ms; past clock drift unbounded; no synchronized order claimed',
      );
    } else {
      lines.add(
        'clock_sample: unavailable; source timelines cannot be synchronized',
      );
    }
    void append(String source, List<SupportAction> rows, bool available) {
      if (!available) {
        lines.add('$source actions: unavailable');
        return;
      }
      if (rows.isEmpty) {
        lines.add('$source actions: none recorded this session');
        return;
      }
      final zero = rows.first.uptimeMs;
      lines.add(
        '$source t=0: first shown action; ${source == 'Board' ? 'board boot uptime' : 'app process age'}=$zero ms',
      );
      for (final row in rows) {
        final relative = row.uptimeMs - zero;
        final settled = row.durationMs == null
            ? ''
            : '; result after ${row.durationMs}ms';
        lines.add(
          '$source +${relative}ms: ${row.description} — ${row.outcomeLabel}$settled',
        );
      }
    }

    append('App', selectedApp, app != null);
    append('Board', selectedBoard, board != null);
    return lines;
  }

  List<String> _incidentSummary(
    SupportIncident? incident,
    SupportPublicHealth? publicHealth,
  ) {
    final Map<String, Object>? fields = incident?.fields;
    final publicFallback = fields == null && publicHealth != null;
    final source = publicFallback ? '; source=public_boot_snapshot' : '';
    final int? reset = (fields?['rr'] as int?) ?? publicHealth?.resetReason;
    final int? prior = fields?['prior'] as int?;
    final int? priorFlags = prior == 1
        ? (fields?['prior_flags'] as int?)
        : null;
    // ESP-IDF reset ABI used by the deployed S3 SDK. Preserve future values
    // rather than guessing that an unrecognized reset was benign.
    final String resetName = switch (reset) {
      null => 'unavailable',
      0 => 'unknown',
      1 => 'power on',
      2 => 'external pin',
      3 => 'software',
      4 => 'panic',
      5 => 'interrupt watchdog',
      6 => 'task watchdog',
      7 => 'other watchdog',
      8 => 'deep sleep wake',
      9 => 'brownout',
      10 => 'SDIO',
      _ => 'unknown code',
    };
    return <String>[
      'firmware_reset_reason: $resetName${reset == null ? '' : ' ($reset)'}$source',
      'firmware_boot_count: ${fields?['boot'] ?? publicHealth?.boots ?? 'unavailable'}$source',
      'firmware_watchdog_restarts: ${fields?['wd'] ?? 'unavailable'}',
      'boot_watchdog_counter_scope: RTC-retained; not lifetime totals',
      'incident_current_flags: ${_incidentFlags(fields?['flags'] as int?)}',
      'incident_prior_context: ${prior == null || prior == 0
          ? 'unavailable'
          : prior == 1
          ? 'available'
          : 'unknown ($prior)'}',
      'incident_prior_flags: ${_incidentFlags(priorFlags)}',
      'firmware_settings_error_count: ${fields?['failed'] ?? 'unavailable'}',
      'firmware_settings_boot_load_error: ${fields?['be'] ?? 'unavailable'}',
    ];
  }

  String _incidentFlags(int? flags) {
    if (flags == null) return 'unavailable';
    String bit(int mask) => flags & mask != 0 ? 'yes' : 'no';
    return 'connected=${bit(1)}, ota_active=${bit(2)}, '
        'session_only=${bit(4)}, safe_mode=${bit(8)}, '
        'unknown_bits=0x${(flags & ~15).toRadixString(16).toUpperCase()}';
  }

  List<String> _phoneLines(SupportPhoneDiagnostics? phone) {
    if (phone == null) return const <String>['phone_diagnostics: unavailable'];
    return <String>[
      'phone_diagnostics_platform: ${_oneLine(phone.platform, 32)}',
      'phone_permissions_source: permission_handler_non_prompting_status_for_this_draft',
      if (phone.permissions.isEmpty) 'phone_permissions: unsupported_platform',
      for (final entry in phone.permissions.entries)
        'permission_handler_${entry.key.name}: ${entry.value.name}',
      if (phone.platform == 'iOS')
        'permission_interpretation: iOS13.0_denied_is_synthetic; iOS13.1plus_denied_is_not_determined_and_permanentlyDenied_is_OS_denial',
      if (phone.platform == 'android')
        'permission_interpretation: scan_connect_below_API31_are_manifest_checks; denial_classification_depends_on_plugin_history; location_required_by_current_app_policy',
      'ble_status_cached: ${phone.cachedBleStatus == null ? 'not_observed' : _oneLine(phone.cachedBleStatus!, 32)}',
      'discovery_availability_cached: ${_optional(phone.cachedDiscoveryAvailability, 32)}',
      'ble_observation_age: unknown; cached_state_is_not_a_fresh_adapter_query',
    ];
  }

  List<String> _capabilityLines(LitchDeviceCapabilities? capabilities) {
    if (capabilities == null) {
      return const <String>[
        'board_hardware: unavailable',
        'capability_contract: unavailable',
        'capability_mask: unavailable',
      ];
    }
    return <String>[
      'board_hardware: ${_oneLine(capabilities.hardwareLabel, 64)}',
      'hardware_revision: ${capabilities.hardwareRevision.name}',
      'capability_contract: ${capabilities.contractVersion}',
      'board_reported_mode: ${capabilities.recoveryOnly ? 'recovery' : 'normal'}',
      'capability_mask: 0x${capabilities.capabilityMask.toRadixString(16).toUpperCase()}',
      'capabilities_explicit: ${capabilities.explicitlyReported}',
      'health_recheck_supported: ${capabilities.supportsRuntimeHealthRecheck}',
    ];
  }

  List<String> _healthLines(RuntimeHealthResult? result) {
    return switch (result) {
      RuntimeHealthAvailable(:final snapshot) => <String>[
        'runtime_health_state: ${snapshot.state.name}',
        'runtime_health_state_code: ${snapshot.stateCode}',
        'runtime_health_revision: ${snapshot.revision}',
        'runtime_health_cause_mask: 0x${snapshot.causeMask.toRadixString(16).toUpperCase().padLeft(4, '0')}',
        'runtime_health_causes: ${snapshot.knownCauses.map((RuntimeHealthCause cause) => cause.name).join(',')}',
        'runtime_health_unknown_causes: 0x${snapshot.unknownCauseMask.toRadixString(16).toUpperCase()}',
        'runtime_health_support_code: ${snapshot.supportCode}',
      ],
      RuntimeHealthUnavailable(:final reason) => <String>[
        'runtime_health: unavailable',
        'runtime_health_unavailable_reason: ${reason.name}',
      ],
      null => const <String>['runtime_health: not_collected'],
    };
  }

  String _optional(String? value, int maxCharacters) {
    final String trimmed = value?.trim() ?? '';
    return trimmed.isEmpty ? 'unavailable' : _oneLine(trimmed, maxCharacters);
  }

  String _oneLine(String value, int maxCharacters) {
    return _bounded(
      value.replaceAll(RegExp(r'[\r\n\t]+'), ' ').trim(),
      maxCharacters,
    );
  }

  String _bounded(String value, int maxCharacters) {
    if (value.length <= maxCharacters) {
      return value;
    }
    return value.substring(0, maxCharacters);
  }
}
