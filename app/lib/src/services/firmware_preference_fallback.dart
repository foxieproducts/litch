import 'dart:convert';
import 'dart:io';

class FirmwarePreferenceFallback {
  const FirmwarePreferenceFallback({this.fileName = 'litch_mobile_prefs.json'});

  final String fileName;

  Future<bool> readBool(String key) async {
    try {
      final File file = await _file();
      if (!await file.exists()) {
        return false;
      }
      final Object? parsed = jsonDecode(await file.readAsString());
      if (parsed is! Map<String, Object?>) {
        return false;
      }
      final Object? value = parsed[key];
      if (value is bool) {
        return value;
      }
      if (value is num) {
        return value != 0;
      }
      final String token = (value ?? '').toString().trim().toLowerCase();
      return token == 'true' || token == '1' || token == 'yes';
    } catch (_) {
      return false;
    }
  }

  Future<bool> writeBool(String key, bool enabled) async {
    try {
      final File file = await _file();
      await file.writeAsString(jsonEncode(<String, Object>{key: enabled}));
      return true;
    } catch (_) {
      return false;
    }
  }

  Future<File> _file() async {
    final String? userHome = Platform.environment['HOME'];
    final Directory baseDir = userHome != null && userHome.isNotEmpty
        ? Directory(userHome)
        : Directory.systemTemp;
    final Directory preferencesDir = Directory('${baseDir.path}/.litch_mobile');
    if (!await preferencesDir.exists()) {
      await preferencesDir.create(recursive: true);
    }
    return File('${preferencesDir.path}/$fileName');
  }
}
