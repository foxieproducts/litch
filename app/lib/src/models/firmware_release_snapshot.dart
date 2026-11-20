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
