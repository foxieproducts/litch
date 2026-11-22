import 'package:flutter/painting.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'about_screen_test.dart' show registerAboutLocalizationTests;
import 'home_screen_test.dart' show registerHomeLocalizationTests;
import 'localization_test_support.dart';
import 'settings_screen_test.dart'
    show registerSettingsLocalizationTests, registerFirmwareLocalizationTests;
import 'support_report_screen_test.dart' show registerSupportLocalizationTests;

// Real-font localization layout checks get their own Flutter test isolate.
// The imported files retain their existing fixtures and independent main suites.
void main() {
  setUp(() {
    PackageInfo.setMockInitialValues(
      appName: 'Foxie Litch',
      packageName: 'com.foxie.litch',
      version: '0.5.0',
      buildNumber: '7',
      buildSignature: '',
    );
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': false,
    });
  });
  testWidgets('localization review fonts use variable-width Cupertino glyphs', (
    tester,
  ) async {
    await prepareLocalizationReview(tester);
    for (final family in ['CupertinoSystemText', 'CupertinoSystemDisplay']) {
      double width(String text) {
        final painter = TextPainter(
          text: TextSpan(
            text: text,
            style: TextStyle(fontFamily: family, fontSize: 22),
          ),
          textDirection: TextDirection.ltr,
        )..layout();
        final measured = painter.width;
        painter.dispose();
        return measured;
      }

      expect(
        width('iiii'),
        lessThan(width('WWWW')),
        reason:
            '$family must resolve to the requested review font or portable SDK layout proxy, not square test glyphs.',
      );
    }
  });
  registerHomeLocalizationTests();
  registerSettingsLocalizationTests();
  registerAboutLocalizationTests();
  registerFirmwareLocalizationTests();
  registerSupportLocalizationTests();
}
