import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/services/support_email_launcher.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';
import 'support_report_test.dart' show incident, incidentDraft;

void main() {
  testWidgets('keeps support content above platform navigation', (
    WidgetTester tester,
  ) async {
    const double bottomSystemInset = 34;
    final EdgeInsets systemPadding = EdgeInsets.only(bottom: bottomSystemInset);
    await tester.pumpWidget(
      MaterialApp(
        builder: (BuildContext context, Widget? child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(padding: systemPadding, viewPadding: systemPadding),
          child: child!,
        ),
        home: SupportReportScreen(
          loadDraft: () async => const SupportReportDraft(
            recipient: 'support@foxieproducts.com',
            subject: 'Support',
            body: 'Report',
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final Rect scaffold = tester.getRect(find.byType(Scaffold));
    final Rect content = tester.getRect(
      find.byKey(const ValueKey<String>('support-report-list')),
    );
    expect(
      content.bottom,
      moreOrLessEquals(scaffold.bottom - bottomSystemInset),
    );
  });

  testWidgets('requires review gesture and opens edited user-owned draft', (
    WidgetTester tester,
  ) async {
    final List<({String recipient, String subject, String body})> opened =
        <({String recipient, String subject, String body})>[];
    final SupportEmailLauncher launcher = SupportEmailLauncher(
      invoke: (String recipient, String subject, String body) async {
        opened.add((recipient: recipient, subject: subject, body: body));
        return true;
      },
    );
    const SupportReportDraft draft = SupportReportDraft(
      recipient: 'support@foxieproducts.com',
      subject: 'Foxie Litch support',
      body: 'What were you trying to do?\n\n--- diagnostics ---\napp: 1',
    );

    await tester.pumpWidget(
      MaterialApp(
        home: SupportReportScreen(
          loadDraft: () async => draft,
          emailLauncher: launcher,
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(opened, isEmpty);
    expect(find.text('Contact Support'), findsOneWidget);
    expect(
      find.textContaining('Nothing is sent automatically'),
      findsOneWidget,
    );
    expect(find.text('To: support@foxieproducts.com'), findsOneWidget);

    await tester.enterText(
      find.byKey(const ValueKey<String>('support-report-body')),
      'Edited report body',
    );
    await tester.enterText(
      find.byKey(const ValueKey<String>('support-subject')),
      'Edited subject',
    );
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('open-support-email')),
      300,
      scrollable: find
          .descendant(
            of: find.byKey(const ValueKey<String>('support-report-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('open-support-email')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('open-support-email')));
    await tester.pumpAndSettle();

    expect(opened, hasLength(1));
    expect(opened.single.recipient, 'support@foxieproducts.com');
    expect(opened.single.subject, 'Edited subject');
    expect(opened.single.body, 'Edited report body');
    expect(find.textContaining('Email draft opened'), findsOneWidget);
  });

  testWidgets('offers copy fallback and remains usable at large text', (
    WidgetTester tester,
  ) async {
    final TestDefaultBinaryMessenger messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const MethodChannel supportChannel = MethodChannel(
      'com.foxieproducts.litch/support',
    );
    final List<String> copiedReports = <String>[];
    messenger.setMockMethodCallHandler(SystemChannels.platform, (
      MethodCall call,
    ) async {
      if (call.method == 'Clipboard.setData') {
        copiedReports.add((call.arguments as Map)['text'] as String);
      }
      return null;
    });
    addTearDown(
      () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
    );
    final List<MethodCall> emailInvocations = <MethodCall>[];
    messenger.setMockMethodCallHandler(supportChannel, (MethodCall call) async {
      emailInvocations.add(call);
      return false;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(supportChannel, null));
    final SupportEmailLauncher launcher = SupportEmailLauncher();
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await tester.pumpWidget(
      MaterialApp(
        home: MediaQuery(
          data: const MediaQueryData(textScaler: TextScaler.linear(2)),
          child: SupportReportScreen(
            loadDraft: () async => const SupportReportDraft(
              recipient: 'support@foxieproducts.com',
              subject: 'Support',
              body: 'Report',
            ),
            emailLauncher: launcher,
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('open-support-email')),
      200,
      scrollable: find
          .descendant(
            of: find.byKey(const ValueKey<String>('support-report-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('open-support-email')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('open-support-email')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 200));

    expect(emailInvocations, hasLength(1));
    expect(emailInvocations.single.method, 'openEmail');
    expect(emailInvocations.single.arguments, <String, String>{
      'recipient': 'support@foxieproducts.com',
      'subject': 'Support',
      'body': 'Report',
    });
    const String copiedReport =
        'To: support@foxieproducts.com\nSubject: Support\n\nReport';
    expect(copiedReports, <String>[copiedReport]);
    expect(find.textContaining('report was copied'), findsOneWidget);
    expect(find.textContaining('Email draft opened'), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('copy-support-report')),
      findsOneWidget,
    );
    // An unavailable handler starts no cooldown; only another user tap retries.
    await tester.drag(find.byType(SnackBar), const Offset(0, 300));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('open-support-email')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 200));
    expect(emailInvocations, hasLength(2));
    expect(copiedReports, <String>[copiedReport, copiedReport]);
    expect(tester.takeException(), isNull);
  });
}

// Registered in localization_screens_test.dart so SDK fonts cannot alter
// the existing Ahem goldens or interaction fixtures in this file.
void registerSupportLocalizationTests() {
  testWidgets(
    'localization support updates untouched questions and preserves user edits',
    (tester) async {
      await prepareLocalizationReview(tester);
      final locale = ValueNotifier(const Locale('de'));
      addTearDown(locale.dispose);
      final draft = incidentDraft(incident());
      await tester.pumpWidget(
        ValueListenableBuilder<Locale>(
          valueListenable: locale,
          builder: (_, selected, _) => localizationHarness(
            locale: selected,
            scale: 2,
            home: SupportReportScreen(loadDraft: () async => draft),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final body = find.byKey(const ValueKey<String>('support-report-body'));
      final subject = find.byKey(const ValueKey<String>('support-subject'));
      Future<TextEditingController> visibleController(Finder target) async {
        FocusManager.instance.primaryFocus?.unfocus();
        await tester.pumpAndSettle();
        final scrollable = find
            .descendant(
              of: find.byKey(const ValueKey<String>('support-report-list')),
              matching: find.byType(Scrollable),
            )
            .first;
        // The translated consent and report can put either field outside the
        // lazy list's built area. Start at the known top and reveal the field.
        tester.state<ScrollableState>(scrollable).position.jumpTo(0);
        await tester.pumpAndSettle();
        await tester.scrollUntilVisible(target, 250, scrollable: scrollable);
        await tester.pumpAndSettle();
        return tester.widget<TextField>(target).controller!;
      }

      final initialBody = await visibleController(body);
      expect(
        initialBody.text,
        startsWith(reviewStrings(const Locale('de')).supportInstructions),
      );
      expect(initialBody.text, endsWith(draft.diagnostics!));
      final state = tester.state(find.byType(SupportReportScreen));
      locale.value = const Locale('fr');
      await tester.pumpAndSettle();
      expect(tester.state(find.byType(SupportReportScreen)), same(state));
      final frenchBody = await visibleController(body);
      expect(
        frenchBody.text,
        startsWith(reviewStrings(const Locale('fr')).supportInstructions),
      );
      expect(frenchBody.text, endsWith(draft.diagnostics!));
      await captureHealthReview(tester, 'locale-fr-2x-ios-support');
      // A user owns even a partial answer; neither field is rewritten after edit.
      await tester.enterText(body, 'Mes réponses\n${draft.diagnostics}');
      await visibleController(subject);
      await tester.enterText(subject, 'Mon sujet');
      locale.value = const Locale('it');
      await tester.pumpAndSettle();
      expect(
        (await visibleController(body)).text,
        'Mes réponses\n${draft.diagnostics}',
      );
      expect((await visibleController(subject)).text, 'Mon sujet');
      expect(
        find.text(reviewStrings(const Locale('it')).contactSupport),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );
}
