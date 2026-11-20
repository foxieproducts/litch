import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';

void main() {
  testWidgets('stalled platform app info yields an editable support draft', (
    WidgetTester tester,
  ) async {
    const MethodChannel channel = MethodChannel(
      'dev.fluttercommunity.plus/package_info',
    );
    final Completer<Map<String, Object>> platformReply =
        Completer<Map<String, Object>>();
    int requests = 0;
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, (
      MethodCall call,
    ) async {
      expect(call.method, 'getAll');
      requests++;
      return platformReply.future;
    });
    addTearDown(() {
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        channel,
        null,
      );
    });

    // No injected draft/collector: use the real screen, collector and plugin.
    // Only the external native package-info response is held.
    await tester.pumpWidget(const MaterialApp(home: SupportReportScreen()));
    await tester.pump();
    expect(requests, 1);
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 2999));
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 1));
    await tester.pump();

    final Finder body = find.byKey(
      const ValueKey<String>('support-report-body'),
    );
    expect(find.byType(CircularProgressIndicator), findsNothing);
    expect(body, findsOneWidget);
    final TextEditingController controller = tester
        .widget<TextField>(body)
        .controller!;
    expect(controller.text, contains('app_version: unavailable'));
    expect(controller.text, contains('app_build: unavailable'));
    expect(controller.text, contains('platform: ${Platform.operatingSystem}'));
    expect(
      controller.text,
      contains('os_version: ${Platform.operatingSystemVersion}'),
    );
    expect(controller.text, contains('board_connection: disconnected'));
    expect(controller.text.length, lessThanOrEqualTo(6000));

    await tester.enterText(
      body,
      'My description must survive late diagnostics.',
    );
    platformReply.complete(<String, Object>{
      'appName': 'Litch',
      'packageName': 'com.foxieproducts.litchMobile',
      'version': '99.0.0',
      'buildNumber': '999',
    });
    await tester.pump();
    await tester.pump();
    expect(controller.text, 'My description must survive late diagnostics.');
    expect(requests, 1);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}
