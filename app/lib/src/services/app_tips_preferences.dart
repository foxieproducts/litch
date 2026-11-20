import 'package:shared_preferences/shared_preferences.dart';

abstract final class AppTipsPreferences {
  static const String key = 'app_show_tips';

  static Future<bool> load() async {
    final preferences = await SharedPreferences.getInstance();
    return preferences.getBool(key) ?? true;
  }

  static Future<void> save(bool enabled) async {
    final preferences = await SharedPreferences.getInstance();
    if (!await preferences.setBool(key, enabled)) {
      throw StateError('App tips preference could not be saved.');
    }
  }
}
