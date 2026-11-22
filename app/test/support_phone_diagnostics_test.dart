import 'support/memory_ota_target_store.dart';
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:litch_mobile/src/models/support_phone_diagnostics.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/support_report_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';

class _Ble implements FlutterReactiveBle {
  final List<Symbol> calls = <Symbol>[];
  @override
  Stream<BleStatus> get statusStream {
    calls.add(#statusStream);
    return Stream<BleStatus>.value(BleStatus.poweredOff);
  }

  @override
  dynamic noSuchMethod(Invocation invocation) {
    calls.add(invocation.memberName);
    return super.noSuchMethod(invocation);
  }
}

class _Permissions implements BlePermissionGateway {
  final List<Permission> reads = <Permission>[];
  final Map<Permission, Future<PermissionStatus> Function()> replies =
      <Permission, Future<PermissionStatus> Function()>{};
  int requests = 0;
  int settingsOpens = 0;

  @override
  Future<PermissionStatus> status(Permission permission) {
    reads.add(permission);
    return replies[permission]?.call() ??
        Future.value(PermissionStatus.granted);
  }

  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async {
    requests++;
    return <Permission, PermissionStatus>{};
  }

  @override
  Future<bool> openSettings() async {
    settingsOpens++;
    return false;
  }
}

void main() {
  late _Ble ble;
  late _Permissions permissions;
  late LitchBleService service;

  setUp(() {
    ble = _Ble();
    permissions = _Permissions();
    service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: ble,
      permissionGateway: permissions,
    );
  });
  tearDown(() async {
    await service.dispose();
    debugDefaultTargetPlatformOverride = null;
  });

  void expectNonPrompting() {
    expect(permissions.requests, 0);
    expect(permissions.settingsOpens, 0);
    expect(ble.calls, isEmpty);
  }

  SupportReportService collector({SupportAppInfoLoader? loadAppInfo}) =>
      SupportReportService(
        bleService: service,
        loadAppInfo:
            loadAppInfo ??
            () async => const SupportAppInfo(
              version: 'fixture',
              buildNumber: '1',
              platform: 'fixture',
              osVersion: 'fixture',
            ),
      );

  for (final PermissionStatus status in PermissionStatus.values) {
    test(
      'iOS reports plugin $status without interpreting OS authorization',
      () async {
        debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
        permissions.replies[Permission.bluetooth] = () async => status;
        final SupportReportDraft draft = await collector().createDraft();
        expect(permissions.reads, <Permission>[Permission.bluetooth]);
        expect(
          draft.body,
          contains('permission_handler_bluetooth: ${status.name}'),
        );
        expect(draft.body, contains('iOS13.0_denied_is_synthetic'));
        expect(
          draft.body,
          isNot(contains('permission_handler_bluetoothScan:')),
        );
        expect(draft.body, contains('ble_status_cached: not_observed'));
        expect(draft.body, contains('ble_observation_age: unknown'));
        expectNonPrompting();
      },
    );
  }

  test(
    'Android samples only current app policy permissions and can resample',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      permissions.replies[Permission.bluetoothConnect] = () async =>
          PermissionStatus.denied;
      permissions.replies[Permission.locationWhenInUse] = () async =>
          PermissionStatus.restricted;
      final SupportReportDraft first = await collector().createDraft();
      expect(permissions.reads, <Permission>[
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.locationWhenInUse,
      ]);
      expect(first.body, contains('permission_handler_bluetoothScan: granted'));
      expect(
        first.body,
        contains('permission_handler_bluetoothConnect: denied'),
      );
      expect(
        first.body,
        contains('permission_handler_locationWhenInUse: restricted'),
      );
      expect(
        first.body,
        contains('scan_connect_below_API31_are_manifest_checks'),
      );
      permissions.replies[Permission.bluetoothConnect] = () async =>
          PermissionStatus.granted;
      final SupportReportDraft second = await collector().createDraft();
      expect(
        second.body,
        contains('permission_handler_bluetoothConnect: granted'),
      );
      expect(
        first.body,
        contains('permission_handler_bluetoothConnect: denied'),
      );
      expectNonPrompting();
    },
  );

  test(
    'unsupported platform is explicit and makes no permission calls',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.linux;
      final SupportReportDraft draft = await collector().createDraft();
      expect(draft.body, contains('phone_permissions: unsupported_platform'));
      expect(permissions.reads, isEmpty);
      expectNonPrompting();
    },
  );

  test(
    'snapshot uses existing cached BLE observation without a fresh query',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      // The normal discovery owner already observed the adapter before the report.
      await service.startScan();
      expect(service.bleAvailability, BleAvailability.poweredOff);
      ble.calls.clear();
      permissions.reads.clear();
      final SupportPhoneDiagnostics snapshot = await service
          .collectSupportPhoneDiagnostics();
      expect(snapshot.cachedBleStatus, 'poweredOff');
      expect(snapshot.cachedDiscoveryAvailability, 'poweredOff');
      expect(() => snapshot.permissions.clear(), throwsUnsupportedError);
      expectNonPrompting();
    },
  );

  testWidgets(
    'hung and failed permission queries share metadata deadline; late replies cannot edit draft',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final Completer<PermissionStatus> scan = Completer<PermissionStatus>();
      final Completer<SupportAppInfo> app = Completer<SupportAppInfo>();
      permissions.replies[Permission.bluetoothScan] = () => scan.future;
      permissions.replies[Permission.bluetoothConnect] = () =>
          throw StateError('private error');
      await tester.pumpWidget(
        MaterialApp(
          home: SupportReportScreen(
            loadDraft: collector(loadAppInfo: () => app.future).createDraft,
          ),
        ),
      );
      await tester.pump();
      expect(permissions.reads.length, 3);
      await tester.pump(const Duration(milliseconds: 2999));
      expect(find.byType(CircularProgressIndicator), findsOneWidget);
      await tester.pump(const Duration(milliseconds: 1));
      await tester.pump();
      final Finder body = find.byKey(
        const ValueKey<String>('support-report-body'),
      );
      final TextEditingController editor = tester
          .widget<TextField>(body)
          .controller!;
      expect(
        editor.text,
        contains('permission_handler_bluetoothScan: timedOut'),
      );
      expect(
        editor.text,
        contains('permission_handler_bluetoothConnect: unavailable'),
      );
      expect(
        editor.text,
        contains('permission_handler_locationWhenInUse: granted'),
      );
      expect(editor.text, contains('app_version: unavailable'));
      expect(editor.text, isNot(contains('private error')));
      expect(editor.text.length, lessThanOrEqualTo(6000));
      await tester.enterText(body, 'Keep my description.');
      scan.complete(PermissionStatus.granted);
      app.complete(
        const SupportAppInfo(
          version: 'late',
          buildNumber: 'late',
          platform: 'late',
          osVersion: 'late',
        ),
      );
      await tester.pump();
      expect(editor.text, 'Keep my description.');
      expectNonPrompting();
      await tester.pumpWidget(const SizedBox.shrink());
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets('leaving report during permission sampling is safe', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
    final Completer<PermissionStatus> pending = Completer<PermissionStatus>();
    permissions.replies[Permission.bluetooth] = () => pending.future;
    await tester.pumpWidget(
      MaterialApp(
        home: SupportReportScreen(loadDraft: collector().createDraft),
      ),
    );
    await tester.pump();
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump(const Duration(seconds: 3));
    pending.complete(PermissionStatus.granted);
    await tester.pump();
    expect(tester.takeException(), isNull);
    expectNonPrompting();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets('review generated phone diagnostics and privacy at phone sizes', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    permissions.replies[Permission.bluetoothScan] = () async =>
        PermissionStatus.denied;
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
          home: SupportReportScreen(loadDraft: collector().createDraft),
        ),
      );
      await tester.pumpAndSettle();
      final String suffix =
          '${width.toInt()}-${scale.toInt()}-${dark ? 'dark' : 'light'}';
      await captureHealthReview(tester, 'support-phone-start-$suffix');
      // Position a review viewport at the actual generated permission block.
      // This is render evidence, not a claim about a user's scrolling gesture.
      final EditableTextState editor = tester.state<EditableTextState>(
        find.descendant(
          of: find.byKey(const ValueKey<String>('support-report-body')),
          matching: find.byType(EditableText),
        ),
      );
      final int permissionOffset = editor.widget.controller.text.indexOf(
        'phone_permissions_source:',
      );
      expect(permissionOffset, greaterThanOrEqualTo(0));
      final Offset caret = editor.renderEditable.localToGlobal(
        editor.renderEditable
            .getLocalRectForCaret(TextPosition(offset: permissionOffset))
            .topLeft,
      );
      final ScrollPosition scroll = tester
          .state<ScrollableState>(find.byType(Scrollable).first)
          .position;
      scroll.jumpTo(
        (scroll.pixels + caret.dy - 100).clamp(0, scroll.maxScrollExtent),
      );
      await tester.pumpAndSettle();
      await captureHealthReview(tester, 'support-phone-permissions-$suffix');
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
      await captureHealthReview(tester, 'support-phone-actions-$suffix');
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
    expectNonPrompting();
    debugDefaultTargetPlatformOverride = null;
  });
}
