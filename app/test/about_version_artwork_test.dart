import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:package_info_plus/package_info_plus.dart';

void main() {
  testWidgets('settled version counts dispatch once, reset, clamp and cancel', (
    tester,
  ) async {
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'test.litch',
      version: '1',
      buildNumber: '1',
      buildSignature: '',
    );
    final calls = <int>[];
    var clock = DateTime.utc(2026);
    final navigator = GlobalKey<NavigatorState>();
    await tester.pumpWidget(
      MaterialApp(
        navigatorKey: navigator,
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: const Scaffold(body: Text('Canvas')),
      ),
    );
    Future<void> open({bool enabled = true}) async {
      navigator.currentState!.push(
        AboutScreen.route(
          versionTapClock: () => clock,
          onLoadVersionArtwork: enabled ? calls.add : null,
        ),
      );
      await tester.pumpAndSettle();
    }

    final version = find.byKey(const ValueKey('about-version'));
    for (var count = 1; count <= 10; count++) {
      await open();
      final previous = calls.length;
      for (var tap = 0; tap < count; tap++) {
        await tester.tap(version);
        clock = clock.add(const Duration(milliseconds: 400));
        await tester.pump(const Duration(milliseconds: 400));
        expect(
          calls.length,
          previous,
          reason: 'No intermediate count can load artwork',
        );
        expect(find.byType(AboutScreen), findsOneWidget);
      }
      await tester.pump(const Duration(milliseconds: 901));
      await tester.pumpAndSettle();
      expect(calls, hasLength(previous + 1));
      expect(calls.last, count.clamp(1, 8));
      expect(find.text('Canvas'), findsOneWidget);
    }
    await open();
    await tester.tap(version);
    clock = clock.add(const Duration(seconds: 2));
    await tester.tap(version);
    await tester.pump(const Duration(milliseconds: 1301));
    await tester.pumpAndSettle();
    expect(calls.last, 1, reason: 'A stale tap clock starts a new sequence');
    final previous = calls.length;
    await open();
    await tester.tap(version);
    navigator.currentState!.pop();
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 2));
    expect(
      calls,
      hasLength(previous),
      reason: 'Leaving About cancels the timer',
    );
    await open();
    await tester.tap(version);
    navigator.currentState!.push(
      MaterialPageRoute<void>(
        builder: (_) => const Scaffold(body: Text('Manual')),
      ),
    );
    await tester.pumpAndSettle();
    await tester.pump(const Duration(milliseconds: 1301));
    expect(find.text('Manual'), findsOneWidget);
    expect(
      calls,
      hasLength(previous),
      reason: 'An inactive About must not pop another route or load artwork',
    );
    navigator.currentState!.pop();
    await tester.pumpAndSettle();
    navigator.currentState!.pop();
    await tester.pumpAndSettle();
    final enabled = ValueNotifier<bool>(true);
    navigator.currentState!.push(
      MaterialPageRoute<void>(
        builder: (_) => ValueListenableBuilder<bool>(
          valueListenable: enabled,
          builder: (_, active, _) =>
              AboutScreen(onLoadVersionArtwork: active ? calls.add : null),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(version);
    enabled.value = false;
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 1301));
    expect(
      calls,
      hasLength(previous),
      reason: 'Removing the action cancels an already pending sequence',
    );
    navigator.currentState!.pop();
    await tester.pumpAndSettle();
    enabled.dispose();
    await open(enabled: false);
    await tester.tap(version);
    await tester.pump(const Duration(seconds: 2));
    expect(calls, hasLength(previous));
    expect(find.byType(AboutScreen), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    await tester.pump(const Duration(seconds: 2));
    expect(calls, hasLength(previous));
  });

  for (final interruption in <String>['Manual', 'callback']) {
    testWidgets(
      'pending version taps stay canceled after restoring $interruption',
      (tester) async {
        PackageInfo.setMockInitialValues(
          appName: 'Litch',
          packageName: 'test.litch',
          version: '1',
          buildNumber: '1',
          buildSignature: '',
        );
        final calls = <int>[];
        final enabled = ValueNotifier<bool>(true);
        final navigator = GlobalKey<NavigatorState>();
        await tester.pumpWidget(
          MaterialApp(
            navigatorKey: navigator,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: const Scaffold(body: Text('Canvas')),
          ),
        );
        navigator.currentState!.push(
          MaterialPageRoute<void>(
            builder: (_) => ValueListenableBuilder<bool>(
              valueListenable: enabled,
              builder: (_, active, _) =>
                  AboutScreen(onLoadVersionArtwork: active ? calls.add : null),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final version = find.byKey(const ValueKey('about-version'));
        await tester.tap(version);
        await tester.pump();
        if (interruption == 'Manual') {
          await tester.tap(find.byKey(const ValueKey('about-manual')));
          await tester.pumpAndSettle();
          navigator.currentState!.pop();
          await tester.pumpAndSettle();
        } else {
          enabled.value = false;
          await tester.pump();
          enabled.value = true;
          await tester.pump();
        }
        await tester.pump(const Duration(milliseconds: 1301));
        await tester.pumpAndSettle();
        expect(calls, isEmpty);
        expect(find.byType(AboutScreen), findsOneWidget);
        expect(
          find.byKey(const ValueKey('about-version-tap-feedback')),
          findsNothing,
        );
        await tester.tap(version);
        await tester.pump(const Duration(milliseconds: 1301));
        await tester.pumpAndSettle();
        expect(calls, <int>[1], reason: 'A fresh sequence remains available');
        await tester.pumpWidget(const SizedBox());
        enabled.dispose();
      },
    );
  }
}
