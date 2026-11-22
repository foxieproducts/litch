import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

enum LitchAppearanceMode { system, dark, light }

extension LitchAppearanceModePresentation on LitchAppearanceMode {
  String get storageValue => name;

  ThemeMode get themeMode => switch (this) {
    LitchAppearanceMode.system => ThemeMode.system,
    LitchAppearanceMode.dark => ThemeMode.dark,
    LitchAppearanceMode.light => ThemeMode.light,
  };
}

abstract final class LitchAppearancePreferences {
  static const String key = 'app_appearance_mode';

  static LitchAppearanceMode parse(Object? value) {
    return LitchAppearanceMode.values.firstWhere(
      (LitchAppearanceMode mode) => mode.storageValue == value,
      orElse: () => LitchAppearanceMode.system,
    );
  }

  static Future<LitchAppearanceMode> load() async {
    final SharedPreferences prefs = await SharedPreferences.getInstance();
    return parse(prefs.getString(key));
  }

  static Future<void> save(LitchAppearanceMode mode) async {
    final SharedPreferences prefs = await SharedPreferences.getInstance();
    await prefs.setString(key, mode.storageValue);
  }
}
