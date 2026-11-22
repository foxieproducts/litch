enum FirmwareReleaseChannel { stable, beta }

class FirmwareReleaseInfo {
  const FirmwareReleaseInfo({
    required this.channel,
    required this.manifestUrl,
    required this.firmwareUrl,
    required this.versionUrl,
    required this.fileName,
  });

  final FirmwareReleaseChannel channel;
  final Uri manifestUrl;
  final Uri firmwareUrl;
  final Uri versionUrl;
  final String fileName;

  String get label => channel == FirmwareReleaseChannel.beta
      ? 'Beta firmware'
      : 'Stable firmware';

  static final FirmwareReleaseInfo stable = FirmwareReleaseInfo(
    channel: FirmwareReleaseChannel.stable,
    manifestUrl: Uri.parse(
      'https://updates.foxieproducts.com/fw/manifest.json',
    ),
    firmwareUrl: Uri.parse('https://updates.foxieproducts.com/fw/firmware.bin'),
    versionUrl: Uri.parse('https://updates.foxieproducts.com/fw/version.md'),
    fileName: 'firmware.bin',
  );

  static final FirmwareReleaseInfo beta = FirmwareReleaseInfo(
    channel: FirmwareReleaseChannel.beta,
    manifestUrl: Uri.parse(
      'https://updates.foxieproducts.com/fw/manifest_beta.json',
    ),
    firmwareUrl: Uri.parse(
      'https://updates.foxieproducts.com/fw/firmware_beta.bin',
    ),
    versionUrl: Uri.parse(
      'https://updates.foxieproducts.com/fw/version_beta.md',
    ),
    fileName: 'firmware_beta.bin',
  );
}
