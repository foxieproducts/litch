import '../constants/firmware_release.dart';
import 'firmware_manifest.dart';

class FirmwareReleaseSnapshot {
  FirmwareReleaseSnapshot({
    required this.release,
    required this.manifest,
    required this.notesMarkdown,
  }) {
    if (manifest.channel != release.channel.name) {
      throw ArgumentError('Release snapshot channel does not match manifest.');
    }
  }

  final FirmwareReleaseInfo release;
  final FirmwareManifest manifest;
  final String notesMarkdown;
}

/// Litch releases use a numeric series and an optional increasing release N,
/// not SemVer prerelease ordering. The bare series and series-0 are identical.
String? firmwareVersionIdentity(String version) {
  final value = version.trim();
  if (value.length > 64) return null;
  if (RegExp(r'^0\.(0|[1-9][0-9]*)(?:-beta)?$').hasMatch(value)) return value;
  final match = RegExp(
    r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(0|[1-9][0-9]*))?$',
  ).firstMatch(value);
  if (match == null) return null;
  final parts = <int>[];
  for (var index = 1; index <= 4; index++) {
    final part = int.tryParse(match[index] ?? '0');
    if (part == null) return null;
    parts.add(part);
  }
  final series = parts.take(3).join('.');
  return parts[3] == 0 ? series : '$series-${parts[3]}';
}

bool isNewerFirmwareVersion(String candidate, String installed) {
  final candidateIdentity = firmwareVersionIdentity(candidate);
  final installedIdentity = firmwareVersionIdentity(installed);
  if (candidateIdentity == null || installedIdentity == null) return false;
  final historicalVersion = RegExp(r'^0\.[0-9]+(?:-beta)?$');
  if (historicalVersion.hasMatch(installedIdentity)) {
    // These shipped pre-1.0 labels do not establish an ordering with each
    // other, but any official 1.x series is unambiguously newer.
    return !historicalVersion.hasMatch(candidateIdentity) &&
        int.parse(candidateIdentity.split('.').first) >= 1;
  }
  if (historicalVersion.hasMatch(candidateIdentity)) return false;
  final candidateParts = candidateIdentity
      .split(RegExp(r'[.-]'))
      .map(int.parse)
      .toList();
  final installedParts = installedIdentity
      .split(RegExp(r'[.-]'))
      .map(int.parse)
      .toList();
  for (var index = 0; index < 4; index++) {
    final next = index < candidateParts.length ? candidateParts[index] : 0;
    final current = index < installedParts.length ? installedParts[index] : 0;
    if (next != current) return next > current;
  }
  return false;
}

class FirmwareUpdateAvailability {
  const FirmwareUpdateAvailability({
    this.installedVersion,
    this.latestVersion,
    this.supportsUpdate = false,
    this.loading = true,
    this.snapshot,
    this.boardId,
    this.connectionToken,
  });

  final String? installedVersion;
  final String? latestVersion;
  final bool supportsUpdate;
  final bool loading;
  final FirmwareReleaseSnapshot? snapshot;
  final String? boardId;
  final int? connectionToken;

  bool get updateAvailable =>
      !loading &&
      supportsUpdate &&
      installedVersion != null &&
      latestVersion != null &&
      isNewerFirmwareVersion(latestVersion!, installedVersion!);

  @override
  bool operator ==(Object other) =>
      other is FirmwareUpdateAvailability &&
      other.installedVersion == installedVersion &&
      other.latestVersion == latestVersion &&
      other.supportsUpdate == supportsUpdate &&
      other.loading == loading &&
      identical(other.snapshot, snapshot) &&
      other.boardId == boardId &&
      other.connectionToken == connectionToken;

  @override
  int get hashCode => Object.hash(
    installedVersion,
    latestVersion,
    supportsUpdate,
    loading,
    snapshot,
    boardId,
    connectionToken,
  );
}
