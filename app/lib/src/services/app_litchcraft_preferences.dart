import 'package:shared_preferences/shared_preferences.dart';

abstract final class AppLitchcraftPreferences {
  static const String _tabletControlsLeftKey = 'app_tablet_controls_on_left';
  static const String storageLimitKey = 'app_litchcraft_storage_limit_bytes';
  static const int defaultStorageLimitBytes = 1_000_000_000;
  static const int minimumStorageLimitBytes = 1_000_000;
  // Keep byte budgets exact on native and web preference implementations.
  static const int maximumStorageLimitBytes = 0x1fffffffffffff;

  static bool isValidStorageLimit(int bytes) =>
      bytes >= minimumStorageLimitBytes && bytes <= maximumStorageLimitBytes;

  static int normalizeStorageLimit(int bytes) =>
      isValidStorageLimit(bytes) ? bytes : defaultStorageLimitBytes;

  static Future<bool?> loadTabletControlsOnLeft() async =>
      (await SharedPreferences.getInstance()).getBool(_tabletControlsLeftKey);

  static Future<void> saveTabletControlsOnLeft(bool onLeft) async {
    final preferences = await SharedPreferences.getInstance();
    await preferences.setBool(_tabletControlsLeftKey, onLeft);
  }

  static Future<int> loadStorageLimit() async {
    final preferences = await SharedPreferences.getInstance();
    final value = preferences.get(storageLimitKey);
    return value is int
        ? normalizeStorageLimit(value)
        : defaultStorageLimitBytes;
  }

  static Future<void> saveStorageLimit(int bytes) async {
    if (!isValidStorageLimit(bytes)) {
      throw RangeError.range(
        bytes,
        minimumStorageLimitBytes,
        maximumStorageLimitBytes,
        'bytes',
      );
    }
    final preferences = await SharedPreferences.getInstance();
    final previous = preferences.get(storageLimitKey);
    try {
      if (!await preferences.setInt(storageLimitKey, bytes)) {
        throw StateError('Artwork storage limit could not be saved.');
      }
    } catch (_) {
      // SharedPreferences changes its cache before persistence completes. Restore
      // the old effective budget for the file store, including on a failed write.
      try {
        if (previous is int) {
          await preferences.setInt(storageLimitKey, previous);
        } else {
          await preferences.remove(storageLimitKey);
        }
      } catch (_) {
        // The cache is restored before the rollback platform write is attempted.
      }
      rethrow;
    }
  }
}
