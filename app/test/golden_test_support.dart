import 'dart:io';

// The same pinned Flutter test fonts use CoreText on macOS and FreeType
// on Linux. Keep exact comparisons against independently reviewed host renders.
String hostGoldenPath(String name) {
  if (Platform.isMacOS) return 'goldens/$name';
  if (Platform.isLinux) return 'goldens/linux/$name';
  throw UnsupportedError(
    'No reviewed golden baseline for ${Platform.operatingSystem}',
  );
}
