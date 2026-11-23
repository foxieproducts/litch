import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Notes for the exact installed package, independent of board firmware.
class AppUpgradeNotice {
  const AppUpgradeNotice({
    required this.version,
    required this.buildNumber,
    required this.notes,
    required this.platform,
    required this.packageName,
  });

  final String version, buildNumber, notes, packageName;
  final TargetPlatform platform;

  String get identityKey =>
      jsonEncode([platform.name, packageName, version, buildNumber]);
}

/// Remembers an observed app identity and offers bundled notes on an upgrade.
///
/// Android's native installation times distinguish a first install from an
/// update, including migration from versions without this marker. iOS reports
/// Documents/bundle file dates instead; those are not update evidence. Its
/// marker lives in Application Support with backup exclusion. Before Flutter
/// starts, iOS adopts a validated stock 1.0.2 startup footprint for the 1.0.3
/// migration, and marks this app generation in backed-up storage. Clean installs
/// and modern restores without an observed identity establish a silent baseline.
/// Neither Apple purchase history nor file dates establish a previous run.
class AppUpgradeNoticeService {
  AppUpgradeNoticeService({
    Future<PackageInfo> Function()? loadPackageInfo,
    Future<SharedPreferences> Function()? loadPreferences,
    Future<String> Function()? loadReleaseNotes,
    TargetPlatform? platform,
    Future<String?> Function()? readIdentity,
    Future<bool> Function(String)? writeIdentity,
  }) : _loadPackageInfo = loadPackageInfo ?? PackageInfo.fromPlatform,
       _loadPreferences = loadPreferences ?? SharedPreferences.getInstance,
       _loadReleaseNotes =
           loadReleaseNotes ?? (() => rootBundle.loadString(notesAsset)),
       _platform = platform ?? defaultTargetPlatform,
       _readIdentityOverride = readIdentity,
       _writeIdentityOverride = writeIdentity;

  static const notesAsset = 'assets/releases/app_store_notes.json';
  static const identityPreferenceKey = 'app_upgrade_last_observed_v1';
  static const _identityChannel = MethodChannel(
    'com.foxieproducts.litch/app_upgrade_identity',
  );
  final Future<PackageInfo> Function() _loadPackageInfo;
  final Future<SharedPreferences> Function() _loadPreferences;
  final Future<String> Function() _loadReleaseNotes;
  final TargetPlatform _platform;
  final Future<String?> Function()? _readIdentityOverride;
  final Future<bool> Function(String)? _writeIdentityOverride;
  Future<AppUpgradeNotice?>? _loading;
  String? _currentIdentity;

  Future<AppUpgradeNotice?> pendingNotice() =>
      _loading ??= _findPendingNotice();

  /// Call after the notes have actually rendered. A failed write stays pending.
  Future<bool> acknowledge(AppUpgradeNotice notice) async {
    final pending = await pendingNotice();
    final current = _currentIdentity;
    if (pending?.identityKey != notice.identityKey || current == null) {
      return false;
    }
    try {
      if (!await _writeIdentity(current)) return false;
      _loading = Future<AppUpgradeNotice?>.value();
      return true;
    } catch (_) {
      return false;
    }
  }

  Future<AppUpgradeNotice?> _findPendingNotice() async {
    if (_platform != TargetPlatform.android &&
        _platform != TargetPlatform.iOS) {
      return null;
    }
    try {
      final package = await _loadPackageInfo();
      final version = package.version.trim();
      final build = package.buildNumber.trim();
      final buildCode = int.tryParse(build);
      if (version.isEmpty ||
          package.packageName.isEmpty ||
          buildCode == null ||
          buildCode <= 0) {
        return null;
      }
      final current = <String, Object?>{
        'schema': 1,
        'platform': _platform.name,
        'package': package.packageName,
        'version': version,
        'build': build,
        'installed': package.installTime?.millisecondsSinceEpoch,
      };
      _currentIdentity = jsonEncode(current);
      final previous = _decodeIdentity(await _readIdentity());

      // Android firstInstallTime == lastUpdateTime means a new installation.
      // This native fact wins even when backup restored an older preference.
      final installed = package.installTime;
      final updated = package.updateTime;
      final androidUpdate =
          _platform == TargetPlatform.android &&
          installed != null &&
          updated != null &&
          installed.millisecondsSinceEpoch > 0 &&
          updated.isAfter(installed);
      final sameInstallation =
          previous != null &&
          previous['platform'] == _platform.name &&
          previous['package'] == package.packageName &&
          (_platform != TargetPlatform.android ||
              previous['installed'] == current['installed']);
      final previousBuild = previous != null && sameInstallation
          ? int.tryParse(previous['build'] as String)
          : null;
      final upgrade = _platform == TargetPlatform.android
          ? androidUpdate &&
                (previous == null ||
                    (sameInstallation &&
                        previousBuild != null &&
                        buildCode > previousBuild))
          : sameInstallation &&
                previousBuild != null &&
                buildCode > previousBuild;
      if (!upgrade) {
        if (!mapEquals(previous, current)) {
          await _writeIdentity(_currentIdentity!);
        }
        return null;
      }

      final notes = _notesFor(await _loadReleaseNotes(), version, build);
      if (notes == null) {
        await _writeIdentity(_currentIdentity!);
        return null;
      }
      return AppUpgradeNotice(
        version: version,
        buildNumber: build,
        notes: notes,
        platform: _platform,
        packageName: package.packageName,
      );
    } catch (_) {
      // Metadata/storage failures must never interrupt ordinary app startup.
      return null;
    }
  }

  Map<String, Object?>? _decodeIdentity(String? encoded) {
    if (encoded == null || encoded.length > 1024) return null;
    try {
      final value = jsonDecode(encoded);
      if (value is! Map<String, dynamic> ||
          value['schema'] != 1 ||
          value['platform'] is! String ||
          value['package'] is! String ||
          value['version'] is! String ||
          value['build'] is! String ||
          (value['installed'] != null && value['installed'] is! int)) {
        return null;
      }
      return value;
    } catch (_) {
      return null;
    }
  }

  String? _notesFor(String encoded, String version, String build) {
    if (encoded.length > 64 * 1024) return null;
    final value = jsonDecode(encoded);
    if (value is! Map<String, dynamic> ||
        value['schema_version'] != 1 ||
        value['releases'] is! List<dynamic>) {
      return null;
    }
    String? matchingNotes;
    for (final release in value['releases'] as List<dynamic>) {
      if (release is! Map<String, dynamic> ||
          release['version'] != version ||
          release['build_number'] != build) {
        continue;
      }
      // Shared Foxie app text may have a recorded platform-specific override.
      final overrides = release['platform_notes'];
      final notes =
          overrides is Map<String, dynamic> &&
              overrides.containsKey(_platform.name)
          ? overrides[_platform.name]
          : release['notes'];
      if (matchingNotes != null || notes is! String || notes.trim().isEmpty) {
        return null;
      }
      matchingNotes = notes.trim();
    }
    return matchingNotes;
  }

  Future<String?> _readIdentity() async {
    if (_readIdentityOverride case final reader?) return reader();
    if (_platform == TargetPlatform.android) {
      return (await _loadPreferences()).getString(identityPreferenceKey);
    }
    return _identityChannel.invokeMethod<String>('read');
  }

  Future<bool> _writeIdentity(String value) async {
    if (_writeIdentityOverride case final writer?) return writer(value);
    if (_platform == TargetPlatform.android) {
      return (await _loadPreferences()).setString(identityPreferenceKey, value);
    }
    return await _identityChannel.invokeMethod<bool>('write', value) ?? false;
  }
}
