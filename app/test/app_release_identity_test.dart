import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/app_release_identity.dart';
import 'package:package_info_plus/package_info_plus.dart';

PackageInfo package({String version = '0.8.3', String build = '12'}) {
  return PackageInfo(
    appName: 'Foxie Litch',
    packageName: 'com.foxieproducts.litch_mobile',
    version: version,
    buildNumber: build,
  );
}

void main() {
  test(
    'native iOS and Android fields retain the exact human suffix 9 and 10',
    () {
      for (final int n in <int>[9, 10]) {
        final AppReleaseIdentity identity = AppReleaseIdentity(
          version: '0.8.3-$n',
          nativeVersion: '0.8.3',
          buildNumber: '12',
        );
        // PackageInfo reads these same marketing/build fields on both platforms.
        expect(identity.versionFor(package()), '0.8.3-$n');
        expect(identity.compactVersionFor(package()), '0.8.3-$n');
        expect(package().version, '0.8.3');
        expect(package().buildNumber, '12');
      }
    },
  );

  test(
    'an overridden native package cannot claim the prepared beta identity',
    () {
      const AppReleaseIdentity identity = AppReleaseIdentity(
        version: '0.8.3-10',
        nativeVersion: '0.8.3',
        buildNumber: '12',
      );
      expect(identity.versionFor(package(build: '11')), '0.8.3');
      expect(identity.versionFor(package(version: '0.8.2')), '0.8.2');
    },
  );

  test(
    'legacy packages keep their literal support and compact screen versions',
    () {
      const AppReleaseIdentity identity = AppReleaseIdentity(
        version: '',
        nativeVersion: '0.6.0',
        buildNumber: '2',
      );
      expect(identity.versionFor(package(version: '0.5.0')), '0.5.0');
      expect(identity.compactVersionFor(package(version: '0.5.0')), '0.5');
    },
  );
}
