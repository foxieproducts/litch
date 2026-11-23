import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/app_release_identity.dart';
import 'package:litch_mobile/src/services/app_upgrade_notice_service.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

PackageInfo package({
  String version = '0.8.3',
  String build = '12',
  DateTime? installed,
  DateTime? updated,
}) {
  return PackageInfo(
    appName: 'Foxie Litch',
    packageName: 'com.foxieproducts.litch_mobile',
    version: version,
    buildNumber: build,
    installTime: installed,
    updateTime: updated,
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'native iOS and Android fields retain the exact human suffix 9 and 10',
    () async {
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

      // A markerless Android installation is an update only when native
      // package history proves that it replaced a prior installed package.
      final installed = DateTime.utc(2026, 11, 20);
      final updated = installed.add(const Duration(days: 1));
      String? observed;
      final metadata = package(
        version: '1.0.2',
        build: '51',
        installed: installed,
        updated: updated,
      );
      AppUpgradeNoticeService service() => AppUpgradeNoticeService(
        platform: TargetPlatform.android,
        loadPackageInfo: () async => metadata,
        readIdentity: () async => observed,
        writeIdentity: (value) async {
          observed = value;
          return true;
        },
      );
      final upgrade = service();
      final notice = await upgrade.pendingNotice();
      expect(notice?.version, '1.0.2');
      expect(notice?.buildNumber, '51');
      expect(notice?.notes, contains('replay saved games move by move'));
      expect(notice?.notes, isNot(contains('1.0.3')));
      expect(await upgrade.acknowledge(notice!), isTrue);
      expect(await upgrade.pendingNotice(), isNull);
      expect(await service().pendingNotice(), isNull);

      // Replacing an already observed package with the same version/build
      // cannot replay its notes, even if lastUpdateTime advances again.
      final reinstalled = AppUpgradeNoticeService(
        platform: TargetPlatform.android,
        loadPackageInfo: () async => package(
          version: '1.0.2',
          build: '51',
          installed: installed,
          updated: updated.add(const Duration(days: 1)),
        ),
        readIdentity: () async => observed,
        writeIdentity: (value) async {
          observed = value;
          return true;
        },
      );
      expect(await reinstalled.pendingNotice(), isNull);
    },
  );

  test(
    'an overridden native package cannot claim the prepared beta identity',
    () async {
      const AppReleaseIdentity identity = AppReleaseIdentity(
        version: '0.8.3-10',
        nativeVersion: '0.8.3',
        buildNumber: '12',
      );
      expect(identity.versionFor(package(build: '11')), '0.8.3');
      expect(identity.versionFor(package(version: '0.8.2')), '0.8.2');

      final notes = await rootBundle.loadString(
        AppUpgradeNoticeService.notesAsset,
      );
      final releases =
          (jsonDecode(notes) as Map<String, dynamic>)['releases']
              as List<dynamic>;
      final preparedNotes = releases.where(
        (release) =>
            release['version'] == litchAppReleaseIdentity.nativeVersion &&
            release['build_number'] == litchAppReleaseIdentity.buildNumber,
      );
      expect(
        preparedNotes,
        hasLength(1),
        reason: 'Store packaging requires exact prepared app release notes',
      );
      expect(preparedNotes.single['notes'], isNot(isEmpty));
      final installed = DateTime.utc(2026, 11, 20);
      String? observed;
      PackageInfo metadata = package(
        version: '1.0.2',
        build: '50',
        installed: installed,
        updated: installed,
      );
      AppUpgradeNoticeService service({String? catalog}) =>
          AppUpgradeNoticeService(
            platform: TargetPlatform.android,
            loadPackageInfo: () async => metadata,
            loadReleaseNotes: () async => catalog ?? notes,
            readIdentity: () async => observed,
            writeIdentity: (value) async {
              observed = value;
              return true;
            },
          );
      expect(await service().pendingNotice(), isNull);
      metadata = package(
        version: '1.0.2',
        build: '51',
        installed: installed,
        updated: installed.add(const Duration(days: 1)),
      );
      final upgrade = service();
      final notice = await upgrade.pendingNotice();
      expect(
        notice?.buildNumber,
        '51',
        reason: 'Same marketing version upgrade',
      );
      final duplicate = jsonDecode(notes) as Map<String, dynamic>;
      (duplicate['releases'] as List<dynamic>).add(
        (duplicate['releases'] as List<dynamic>).firstWhere(
          (release) => release['version'] == '1.0.2',
        ),
      );
      expect(
        await service(catalog: jsonEncode(duplicate)).pendingNotice(),
        isNull,
        reason: 'Ambiguous release records never supply notes',
      );

      // A new installation overrides an older identity restored from backup.
      observed = jsonEncode({
        'schema': 1,
        'platform': 'android',
        'package': metadata.packageName,
        'version': '1.0.1',
        'build': '49',
        'installed': installed.millisecondsSinceEpoch,
      });
      final fresh = DateTime.utc(2026, 11, 22);
      metadata = package(
        version: '1.0.2',
        build: '51',
        installed: fresh,
        updated: fresh,
      );
      expect(await service().pendingNotice(), isNull);
      expect(jsonDecode(observed!)['build'], '51');
      metadata = package(version: '1.0.3', build: '52');
      expect(await service().pendingNotice(), isNull);
    },
  );

  test(
    'legacy packages keep their literal support and compact screen versions',
    () async {
      const AppReleaseIdentity identity = AppReleaseIdentity(
        version: '',
        nativeVersion: '0.6.0',
        buildNumber: '2',
      );
      expect(identity.versionFor(package(version: '0.5.0')), '0.5.0');
      expect(identity.compactVersionFor(package(version: '0.5.0')), '0.5');

      String? installedIdentity;
      var writeSucceeds = true;
      var build = '51';
      SharedPreferences.setMockInitialValues({
        AppUpgradeNoticeService.identityPreferenceKey: jsonEncode({
          'schema': 1,
          'platform': 'iOS',
          'package': 'com.foxieproducts.litch_mobile',
          'version': '1.0.1',
          'build': '49',
          'installed': 1,
        }),
      });
      final catalog = jsonEncode({
        'schema_version': 1,
        'releases': [
          {
            'version': '1.0.2',
            'build_number': '52',
            'notes': 'Shared app text.',
            'platform_notes': {'iOS': 'Recorded iOS app text.'},
          },
        ],
      });
      AppUpgradeNoticeService service() => AppUpgradeNoticeService(
        platform: TargetPlatform.iOS,
        loadPackageInfo: () async => package(
          version: '1.0.2',
          build: build,
          // iOS file dates are intentionally ignored as upgrade proof.
          installed: DateTime.utc(2026, 1),
          updated: DateTime.utc(2026, 11),
        ),
        loadReleaseNotes: () async => catalog,
        readIdentity: () async => installedIdentity,
        writeIdentity: (value) async {
          if (writeSucceeds) installedIdentity = value;
          return writeSucceeds;
        },
      );
      expect(
        await service().pendingNotice(),
        isNull,
        reason:
            'Restored preferences and file dates cannot prove an iOS update',
      );
      build = '52';
      final upgrade = service();
      final notice = await upgrade.pendingNotice();
      expect(notice?.notes, 'Recorded iOS app text.');
      writeSucceeds = false;
      expect(await upgrade.acknowledge(notice!), isFalse);
      expect(await upgrade.pendingNotice(), notice);
      writeSucceeds = true;
      expect(await upgrade.acknowledge(notice), isTrue);
      expect(await service().pendingNotice(), isNull);

      installedIdentity = null;
      expect(
        await service().pendingNotice(),
        isNull,
        reason:
            'Reinstallation without the excluded marker establishes a baseline',
      );
      build = '53';
      expect(
        await service().pendingNotice(),
        isNull,
        reason: 'Unrecorded builds never borrow another build’s release notes',
      );
      build = '51';
      expect(await service().pendingNotice(), isNull, reason: 'Downgrade');

      // Native pre-Flutter bootstrap adopts the exact stock identity only for
      // a used legacy container. Consume that record through the real channel.
      const channel = MethodChannel(
        'com.foxieproducts.litch/app_upgrade_identity',
      );
      String? nativeIdentity = jsonEncode({
        'schema': 1,
        'platform': 'iOS',
        'package': 'com.foxieproducts.litchMobile',
        'version': '1.0.2',
        'build': '51',
        'installed': null,
      });
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            if (call.method == 'read') return nativeIdentity;
            if (call.method == 'write') {
              if (writeSucceeds) nativeIdentity = call.arguments as String;
              return writeSucceeds;
            }
            throw MissingPluginException();
          });
      addTearDown(() {
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null);
      });
      AppUpgradeNoticeService nativeService() => AppUpgradeNoticeService(
        platform: TargetPlatform.iOS,
        loadPackageInfo: () async => PackageInfo(
          appName: 'Foxie Litch',
          packageName: 'com.foxieproducts.litchMobile',
          version: '1.0.3',
          buildNumber: '52',
        ),
      );
      final migrated = nativeService();
      final migrationNotice = await migrated.pendingNotice();
      expect(migrationNotice?.version, '1.0.3');
      expect(migrationNotice?.buildNumber, '52');
      expect(
        migrationNotice?.notes,
        contains('clearer Bluetooth pairing help'),
      );
      expect(jsonDecode(nativeIdentity!)['build'], '51');
      expect(
        (await nativeService().pendingNotice())?.identityKey,
        migrationNotice!.identityKey,
        reason: 'Process death before rendering preserves the pending update',
      );
      writeSucceeds = false;
      expect(await migrated.acknowledge(migrationNotice), isFalse);
      expect(jsonDecode(nativeIdentity!)['build'], '51');
      writeSucceeds = true;
      expect(await migrated.acknowledge(migrationNotice), isTrue);
      expect(jsonDecode(nativeIdentity!)['build'], '52');
      expect(await nativeService().pendingNotice(), isNull);

      // These are the native bootstrap outputs for clean/redownload and
      // restored modern data. They never synthesize stock build 51 from dates.
      nativeIdentity = null;
      expect(await nativeService().pendingNotice(), isNull);
      expect(jsonDecode(nativeIdentity!)['build'], '52');
      expect(await nativeService().pendingNotice(), isNull);
    },
  );
}
