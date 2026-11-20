import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

void main() {
  test('non-board surfaces use ordinary rounded corners', () {
    final ThemeData theme = buildLitchTheme();

    expect(LitchShapes.panel(), isA<RoundedRectangleBorder>());
    expect(theme.cardTheme.shape, isA<RoundedRectangleBorder>());
    expect(
      theme.navigationBarTheme.indicatorShape,
      isA<RoundedRectangleBorder>(),
    );
    expect(
      theme.navigationBarTheme.labelTextStyle,
      isNull,
      reason: 'persistent navigation should retain Material label hierarchy',
    );
  });

  test('appearance defaults to system and persists explicit modes', () async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    expect(await LitchAppearancePreferences.load(), LitchAppearanceMode.system);

    await LitchAppearancePreferences.save(LitchAppearanceMode.dark);
    expect(await LitchAppearancePreferences.load(), LitchAppearanceMode.dark);
    expect(LitchAppearanceMode.dark.themeMode, ThemeMode.dark);
    expect(LitchAppearanceMode.light.themeMode, ThemeMode.light);
  });

  test('dark theme uses a complete dark color scheme', () {
    final ThemeData dark = buildLitchDarkTheme();

    expect(dark.brightness, Brightness.dark);
    expect(dark.scaffoldBackgroundColor, dark.colorScheme.surface);
    expect(dark.cardTheme.color, dark.colorScheme.surfaceContainerLow);
    expect(dark.colorScheme.onSurface.computeLuminance(), greaterThan(0.5));
  });
}
