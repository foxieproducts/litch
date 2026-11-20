import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/localization/presentation_strings.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/models/support_board_collection.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/models/support_phone_diagnostics.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';

SupportIncident incident({
  int reset = 6,
  int prior = 1,
  int flags = 5,
  int priorFlags = 10,
  bool maximum = false,
}) => SupportIncident.tryParse(
  jsonEncode(<String, Object>{
    'event': 'support_incident',
    'v': 1,
    for (final key in SupportIncident.scalarKeys) key: maximum ? 0xffffffff : 0,
    'rr': reset,
    'prior': prior,
    'flags': flags,
    'prior_flags': priorFlags,
    if (!maximum) ...<String, Object>{
      'boot': 4,
      'wd': 2,
      'saved': 7,
      'failed': 3,
      'be': 2,
    },
    for (final key in SupportIncident.masks) key: 'FFFFFFFFFFFFFFFF',
    for (final key in <String>['stages', 'now_stages'])
      key: List<int>.filled(4, 14),
    for (final key in <String>['stage_ms', 'now_ms'])
      key: List<int>.filled(4, maximum ? 0xffffffff : 100),
    'recent': List<int>.filled(8, 14),
  }),
)!;

SupportReportDraft incidentDraft(
  SupportIncident? value, {
  bool maximum = false,
  Map<SupportQueryField, SupportQueryOutcome>? outcomes,
}) {
  final String text = maximum ? 'fixture'.padRight(10000, 'x') : 'fixture';
  return const SupportReportBuilder().build(
    app: SupportAppInfo(
      version: text,
      buildNumber: text,
      platform: text,
      osVersion: text,
    ),
    board: SupportBoardInfo(
      connected: value != null,
      incident: value,
      firmwareVersion:
          outcomes == null ||
              outcomes[SupportQueryField.firmwareVersion] ==
                  SupportQueryOutcome.available
          ? text
          : null,
      firmwareDigest:
          outcomes == null ||
              outcomes[SupportQueryField.firmwareDigest] ==
                  SupportQueryOutcome.available
          ? text
          : null,
      queryOutcomes:
          outcomes ??
          <SupportQueryField, SupportQueryOutcome>{
            for (final field in SupportQueryField.values)
              field: maximum
                  ? SupportQueryOutcome.notAttemptedDeadline
                  : SupportQueryOutcome.available,
          },
      capabilities: maximum
          ? const LitchDeviceCapabilities(
              contractVersion: 3,
              hardwareRevision: LitchHardwareRevision.v3,
              capabilityMask: 0xffff,
              explicitlyReported: true,
            )
          : outcomes?[SupportQueryField.capabilities] ==
                SupportQueryOutcome.unavailable
          ? LitchDeviceCapabilities.unavailable
          : null,
      runtimeHealth: maximum
          ? RuntimeHealthAvailable(
              RuntimeHealthSnapshot.fromToken('FFFF3FFFF')!,
            )
          : outcomes == null
          ? null
          : const RuntimeHealthUnavailable(
              RuntimeHealthUnavailableReason.unsupported,
            ),
      boardHealth: maximum
          ? BoardHealthStatus(
              runtimeHealth: RuntimeHealthSnapshot.fromToken('FFFF3FFFF')!,
              persistence: HomePersistenceDegradation.quarantinedRecord,
              resetReason: 0xffffffff,
              bootCount: 0xffffffff,
              watchdogRestarts: 0xffffffff,
            )
          : null,
    ),
    session: LitchSupportSessionDiagnostics(
      settingReads: maximum
          ? const SettingReadDiagnosticsSnapshot(
              operations: 0xffffffff,
              directAttempts: 0xffffffff,
              fallbackEntries: 0xffffffff,
              retryAttempts: 0xffffffff,
              directSuperseded: 0xffffffff,
              succeeded: 0xffffffff,
              recovered: 0xffffffff,
              exhausted: 0xffffffff,
              cancelled: 0xffffffff,
              unknown: 0xffffffff,
              totalDurationMs: 0xffffffff,
              maxDurationMs: 0xffffffff,
              saturated: true,
            )
          : const SettingReadDiagnosticsSnapshot(),
      settingsWriteAttempts: 0xffffffff,
      settingsWriteSucceeded: 0xffffffff,
      settingsWriteFailed: 0xffffffff,
      lastSettingsOperation: text,
      lastSettingsFailureToken: text,
      controlNativeWrites: maximum ? 0xffffffff : 0,
      controlTimeouts: maximum ? 0xffffffff : 0,
      controlIntents: maximum ? 0xffffffff : 0,
      controlCoalescedTaps: maximum ? 0xffffffff : 0,
      controlRecoveryReadRequests: maximum ? 0xffffffff : 0,
      controlBusyRetries: maximum ? 0xffffffff : 0,
      controlRecoveryConfirmed: maximum ? 0xffffffff : 0,
      controlRecoveryUnconfirmed: maximum ? 0xffffffff : 0,
      notificationRepairOperations: maximum ? 0xffffffff : 3,
      notificationRepairAttempts: maximum ? 0xffffffff : 5,
      notificationRepairRecovered: maximum ? 0xffffffff : 1,
      notificationRepairExhausted: maximum ? 0xffffffff : 1,
      notificationRepairCancelled: maximum ? 0xffffffff : 1,
      notificationRepairSaturated: maximum,
    ),
    phone: SupportPhoneDiagnostics(
      platform: 'android',
      cachedBleStatus: text,
      cachedDiscoveryAvailability: text,
      permissions: <SupportPhonePermission, SupportPhonePermissionResult>{
        for (final permission in SupportPhonePermission.values)
          if (permission != SupportPhonePermission.bluetooth)
            permission: SupportPhonePermissionResult.permanentlyDenied,
      },
    ),
  );
}

void main() {
  test(
    'localized support preserves serialized diagnostics and existing body bound',
    () {
      for (final maximum in <bool>[false, true]) {
        final draft = incidentDraft(
          incident(maximum: maximum),
          maximum: maximum,
        );
        final originalDiagnostics = draft.body.substring(
          draft.body.indexOf('diagnostic_schema: 3'),
        );
        expect(draft.diagnostics, originalDiagnostics);
        for (final locale in localizationReviewLocales) {
          final localized = localizedSupportBody(reviewStrings(locale), draft);
          expect(
            localized.substring(localized.indexOf('diagnostic_schema: 3')),
            originalDiagnostics,
            reason:
                '$locale: localized questions must not rewrite diagnostic evidence',
          );
          expect(
            localized.length,
            lessThanOrEqualTo(SupportReportDraft.maxBodyCharacters),
            reason: '$locale maximum=$maximum',
          );
        }
      }
    },
  );

  testWidgets('review decoded recovery diagnostics at phone sizes', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    addTearDown(() => tester.binding.setSurfaceSize(null));
    for (final (double width, double scale, bool dark)
        in <(double, double, bool)>[
          (390, 1, false),
          (390, 1, true),
          (320, 2, false),
        ]) {
      await tester.binding.setSurfaceSize(Size(width, 844));
      await tester.pumpWidget(
        MaterialApp(
          theme: dark ? buildLitchDarkTheme() : buildLitchTheme(),
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: TextScaler.linear(scale)),
            child: child!,
          ),
          home: SupportReportScreen(
            loadDraft: () async => incidentDraft(
              incident(),
              outcomes: const <SupportQueryField, SupportQueryOutcome>{
                SupportQueryField.incident: SupportQueryOutcome.available,
                SupportQueryField.firmwareDigest: SupportQueryOutcome.timedOut,
                SupportQueryField.capabilities:
                    SupportQueryOutcome.unavailable,
                SupportQueryField.runtimeHealth:
                    SupportQueryOutcome.unavailable,
                SupportQueryField.firmwareVersion:
                    SupportQueryOutcome.notAttemptedDeadline,
              },
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final Finder body = find.byKey(
        const ValueKey<String>('support-report-body'),
      );
      await tester.scrollUntilVisible(
        body,
        300,
        scrollable: find.byType(Scrollable).first,
      );
      await tester.pumpAndSettle();
      final EditableTextState editor = tester.state<EditableTextState>(
        find.descendant(of: body, matching: find.byType(EditableText)),
      );
      final String suffix =
          '${width.toInt()}-${scale.toInt()}-${dark ? 'dark' : 'light'}';
      final ScrollPosition scroll = tester
          .state<ScrollableState>(find.byType(Scrollable).first)
          .position;
      final int outcomesOffset = editor.widget.controller.text.indexOf(
        'collection_status:',
      );
      expect(outcomesOffset, greaterThanOrEqualTo(0));
      final Offset outcomesCaret = editor.renderEditable.localToGlobal(
        editor.renderEditable
            .getLocalRectForCaret(TextPosition(offset: outcomesOffset))
            .topLeft,
      );
      scroll.jumpTo(
        (scroll.pixels + outcomesCaret.dy - 100).clamp(
          0,
          scroll.maxScrollExtent,
        ),
      );
      await tester.pumpAndSettle();
      await captureHealthReview(tester, 'support-collection-outcomes-$suffix');
      final int offset = editor.widget.controller.text.indexOf(
        'firmware_reset_reason:',
      );
      expect(offset, greaterThanOrEqualTo(0));
      final Offset caret = editor.renderEditable.localToGlobal(
        editor.renderEditable
            .getLocalRectForCaret(TextPosition(offset: offset))
            .topLeft,
      );
      scroll.jumpTo(
        (scroll.pixels + caret.dy - 100).clamp(0, scroll.maxScrollExtent),
      );
      await tester.pumpAndSettle();
      await captureHealthReview(tester, 'support-recovery-$suffix');
      final Finder open = find.byKey(
        const ValueKey<String>('open-support-email'),
      );
      await tester.scrollUntilVisible(
        open,
        600,
        scrollable: find.byType(Scrollable).first,
      );
      await tester.pumpAndSettle();
      expect(tester.getSize(open).height, greaterThanOrEqualTo(48));
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'support-recovery-actions-$suffix');
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
  });

  test('report explains watchdog and current versus prior incident flags', () {
    final SupportReportDraft draft = incidentDraft(incident());
    expect(draft.body, contains('firmware_reset_reason: task watchdog (6)'));
    expect(draft.body, contains('firmware_watchdog_restarts: 2'));
    expect(draft.body, contains('firmware_boot_count: 4'));
    expect(
      draft.body,
      contains(
        'boot_watchdog_counter_scope: RTC-retained; not lifetime totals',
      ),
    );
    expect(draft.body, isNot(contains('incident_counter_scope:')));
    expect(
      draft.body,
      contains(
        'incident_current_flags: connected=yes, ota_active=no, session_only=yes, safe_mode=no, unknown_bits=0x0',
      ),
    );
    expect(draft.body, contains('incident_prior_context: available'));
    expect(
      draft.body,
      contains(
        'incident_prior_flags: connected=no, ota_active=yes, session_only=no, safe_mode=yes, unknown_bits=0x0',
      ),
    );
    expect(draft.body, contains('firmware_settings_error_count: 3'));
    expect(draft.body, contains('firmware_settings_boot_load_error: 2'));
    expect(draft.body, isNot(contains('incident_flags_or_unavailable')));
  });

  test(
    'missing incident and pre-reset context are not interpreted as clear flags',
    () {
      final SupportReportDraft missing = incidentDraft(null);
      expect(missing.body, contains('firmware_reset_reason: unavailable'));
      expect(missing.body, contains('incident_current_flags: unavailable'));
      expect(missing.body, contains('incident_prior_context: unavailable'));
      final SupportReportDraft brownout = incidentDraft(
        incident(reset: 9, prior: 0, priorFlags: 15),
      );
      expect(brownout.body, contains('firmware_reset_reason: brownout (9)'));
      expect(brownout.body, contains('incident_prior_context: unavailable'));
      expect(brownout.body, contains('incident_prior_flags: unavailable'));
      expect(
        brownout.body,
        isNot(contains('incident_prior_flags: connected=')),
      );
    },
  );

  test(
    'unknown incident codes and maximum diagnostics stay visible within bounds',
    () {
      final SupportReportDraft draft = incidentDraft(
        incident(
          reset: 0xffffffff,
          flags: 0xffffffff,
          priorFlags: 0xffffffff,
          maximum: true,
        ),
        maximum: true,
      );
      expect(
        draft.body,
        contains('firmware_reset_reason: unknown code (4294967295)'),
      );
      expect(draft.body, contains('unknown_bits=0xFFFFFFF0'));
      expect(draft.body, contains('firmware_settings_error_count: 4294967295'));
      expect(draft.body, contains('firmware_sha256:'));
      expect(draft.body, contains('firmwareVersion=notAttemptedDeadline'));
      expect(
        draft.body,
        contains('permission_handler_bluetoothScan: permanentlyDenied'),
      );
      expect(draft.body, contains('incident_snapshot: {'));
      expect(draft.body, contains('capability_mask: 0xFFFF'));
      expect(draft.body, contains('runtime_health_support_code: FFFF-3-FFFF'));
      expect(
        draft.body,
        contains('other_recovery_and_enumeration_io=unmeasured'),
      );
      expect(draft.body, contains('direct_superseded=4294967295'));
      expect(draft.body, contains('ms_max=4294967295, saturated=true'));
      expect(
        draft.body,
        contains('app_control_recovery_unconfirmed: 4294967295'),
      );
      expect(
        draft.body,
        contains(
          'app_notification_repairs: ops=4294967295, pending=0, '
          'attempts=4294967295, recovered=4294967295, '
          'exhausted=4294967295, cancelled=4294967295, saturated=true',
        ),
      );
      expect(
        draft.body,
        contains('no_arbitrary_logs; no_names_colors_exact_squares'),
      );
      expect(draft.body.length, lessThanOrEqualTo(6000));
      expect(draft.subject.length, lessThanOrEqualTo(160));
      final SupportReportDraft unknownPrior = incidentDraft(incident(prior: 2));
      expect(
        unknownPrior.body,
        contains('incident_prior_context: unknown (2)'),
      );
      expect(unknownPrior.body, contains('incident_prior_flags: unavailable'));
    },
  );

  test('builds bounded reviewable health report without private content', () {
    final SupportReportDraft draft = const SupportReportBuilder().build(
      app: const SupportAppInfo(
        version: '0.5.0',
        buildNumber: '17',
        platform: 'android',
        osVersion: 'Android 16',
      ),
      board: SupportBoardInfo(
        connected: true,
        firmwareVersion: '0.64-beta',
        capabilities: const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v2,
          capabilityMask: 127,
          explicitlyReported: true,
        ),
        runtimeHealth: RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00AF10030')!,
        ),
      ),
      session: const LitchSupportSessionDiagnostics(
        settingsWriteAttempts: 3,
        settingsWriteSucceeded: 2,
        settingsWriteFailed: 1,
        lastSettingsOperation: 'setting_write:BRIGHTNESS',
        lastSettingsFailureToken: 'board:storage_unavailable',
      ),
    );

    expect(draft.recipient, 'support@foxieproducts.com');
    expect(draft.subject, contains('Board health'));
    expect(draft.body, contains('What were you trying to do?'));
    expect(draft.body, contains('Which USB host, charger, and cable'));
    expect(draft.body, contains('runtime_health_support_code: 00AF-1-0030'));
    expect(draft.body, contains('capability_mask: 0x7F'));
    expect(
      draft.body,
      contains('last_app_settings_failure: board:storage_unavailable'),
    );
    expect(
      draft.body,
      contains(
        'firmware_settings_retry_count: unavailable_over_structured_ble',
      ),
    );
    expect(draft.body, contains('no_ble_device_id_no_credentials'));
    expect(draft.body.length, lessThanOrEqualTo(6000));
    expect(draft.subject.length, lessThanOrEqualTo(160));
  });

  test('support remains useful while disconnected', () {
    final SupportReportDraft draft = const SupportReportBuilder().build(
      app: const SupportAppInfo(
        version: '0.5.0',
        buildNumber: '1',
        platform: 'ios',
        osVersion: 'iOS',
      ),
      board: const SupportBoardInfo(connected: false),
      session: const LitchSupportSessionDiagnostics(
        settingsWriteAttempts: 0,
        settingsWriteSucceeded: 0,
        settingsWriteFailed: 0,
      ),
    );

    expect(draft.subject, 'Foxie Litch support');
    expect(draft.body, contains('board_connection: disconnected'));
    expect(draft.body, contains('runtime_health: not_collected'));
    expect(draft.body, isNot(contains('What were you trying to do?\nYes')));
  });
}
