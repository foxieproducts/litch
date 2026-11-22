import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/services/app_tips_preferences.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  test('tips default on and persist an explicit choice', () async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    expect(await AppTipsPreferences.load(), isTrue);

    await AppTipsPreferences.save(false);

    expect(await AppTipsPreferences.load(), isFalse);
    final preferences = await SharedPreferences.getInstance();
    expect(preferences.getBool(AppTipsPreferences.key), isFalse);
  });

  testWidgets('app settings exposes an accessible shared tips switch', (
    tester,
  ) async {
    bool? changedTo;
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => AppPreferencesScope(
          appearanceMode: LitchAppearanceMode.system,
          onAppearanceModeChanged: (_) async {},
          localePreference: const LitchLocalePreference.system(),
          onLocalePreferenceChanged: (_) async {},
          showTips: true,
          onShowTipsChanged: (enabled) async => changedTo = enabled,
          child: child!,
        ),
        home: const Scaffold(
          body: SingleChildScrollView(child: AppPreferences()),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final switchTile = find.byKey(const ValueKey<String>('app-show-tips'));
    await tester.ensureVisible(switchTile);
    await tester.pumpAndSettle();
    expect(tester.getSize(switchTile).height, greaterThanOrEqualTo(48));
    expect(find.text('Show tips'), findsOneWidget);
    expect(tester.getSemantics(switchTile), isSemantics(isToggled: true));

    await tester.tap(switchTile);
    await tester.pump();
    expect(changedTo, isFalse);
  });
}
