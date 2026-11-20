import 'package:package_info_plus/package_info_plus.dart';

// Source metadata prepared by tools/release/release_identity.py before QA.
// Preparation requires reconciled publication evidence for the selected series.
const AppReleaseIdentity litchAppReleaseIdentity = AppReleaseIdentity(
  version: '1.0.1-0',
  nativeVersion: '1.0.1',
  buildNumber: '49',
);

/// Human release identity is separate from native package version/build fields.
class AppReleaseIdentity {
  const AppReleaseIdentity({
    required this.version,
    required this.nativeVersion,
    required this.buildNumber,
  });

  final String version;
  final String nativeVersion;
  final String buildNumber;

  String versionFor(PackageInfo package) {
    if (version.isNotEmpty &&
        package.version == nativeVersion &&
        package.buildNumber == buildNumber) {
      return version;
    }
    // An overridden or legacy native package must not claim this release's N.
    return package.version.trim();
  }

  String compactVersionFor(PackageInfo package) {
    final String value = versionFor(package);
    final RegExpMatch? match = RegExp(r'^(\d+)\.(\d+)\.0$').firstMatch(value);
    return match == null ? value : '${match[1]}.${match[2]}';
  }
}
