enum LitchHardwareRevision { unknown, v1, v2, v3, v3_3 }

class LitchDeviceCapabilities {
  const LitchDeviceCapabilities({
    required this.contractVersion,
    required this.hardwareRevision,
    required this.capabilityMask,
    required this.explicitlyReported,
    this.recoveryOnly = false,
  });

  static const int consoleBit = 1 << 0;
  static const int settingsBit = 1 << 1;
  static const int identifyBit = 1 << 2;
  static const int firmwareDigestBit = 1 << 3;
  static const int firmwareUpdateBit = 1 << 4;
  static const int runtimeHealthBit = 1 << 5;
  static const int runtimeHealthRecheckBit = 1 << 6;
  static const int gameHistoryBit = 1 << 7;
  static const int coachInsightBit = 1 << 8;
  static const int ownerPairingBit = 1 << 9;
  static const int litchcraftBit = 1 << 10;
  static const int litchcraftBulkBit = 1 << 11;
  static const int litchcraftInspectBit = 1 << 12;
  static const int litchcraftPaletteBit = 1 << 13;
  static const int litchcraftAnimationBit = 1 << 14;
  static const int litchcraftShakeEraseBit = 1 << 15;

  static const int _connectionSessionContractVersion = 3;
  static const int _maskMax = 0xffff;

  // Missing discovery never grants product access or an update path.
  static const LitchDeviceCapabilities unavailable = LitchDeviceCapabilities(
    contractVersion: 0,
    hardwareRevision: LitchHardwareRevision.unknown,
    capabilityMask: 0,
    explicitlyReported: false,
  );

  final int contractVersion;
  final LitchHardwareRevision hardwareRevision;
  final int capabilityMask;
  final bool explicitlyReported;
  final bool recoveryOnly;

  bool get supportsConsole =>
      supportsConnectionSession && (capabilityMask & consoleBit) != 0;
  bool get supportsConnectionSession =>
      explicitlyReported &&
      contractVersion == _connectionSessionContractVersion;
  bool get supportsLitchcraft =>
      supportsConnectionSession && (capabilityMask & litchcraftBit) != 0;
  bool get supportsLitchcraftBulk =>
      supportsLitchcraft && (capabilityMask & litchcraftBulkBit) != 0;
  bool get supportsLitchcraftInspect =>
      supportsLitchcraft && (capabilityMask & litchcraftInspectBit) != 0;
  bool get supportsLitchcraftPalette =>
      supportsLitchcraft && (capabilityMask & litchcraftPaletteBit) != 0;
  bool get supportsLitchcraftAnimation =>
      supportsLitchcraftInspect &&
      (capabilityMask & litchcraftAnimationBit) != 0;
  bool get supportsLitchcraftShakeErase =>
      supportsLitchcraft &&
      (hardwareRevision == LitchHardwareRevision.v3 ||
          hardwareRevision == LitchHardwareRevision.v3_3) &&
      (capabilityMask & litchcraftShakeEraseBit) != 0;
  bool get supportsSettings =>
      supportsConnectionSession && (capabilityMask & settingsBit) != 0;
  bool get supportsIdentify =>
      supportsConnectionSession && (capabilityMask & identifyBit) != 0;
  bool get supportsFirmwareDigest =>
      supportsConnectionSession && (capabilityMask & firmwareDigestBit) != 0;
  bool get supportsFirmwareUpdate =>
      supportsConnectionSession && (capabilityMask & firmwareUpdateBit) != 0;
  bool get supportsRuntimeHealth =>
      supportsConnectionSession && (capabilityMask & runtimeHealthBit) != 0;
  bool get supportsRuntimeHealthRecheck =>
      supportsConnectionSession &&
      (capabilityMask & runtimeHealthRecheckBit) != 0;
  bool get supportsGameHistory =>
      supportsConnectionSession && (capabilityMask & gameHistoryBit) != 0;
  bool get supportsCoachInsight =>
      supportsConnectionSession && (capabilityMask & coachInsightBit) != 0;
  bool get supportsOwnerPairing =>
      supportsConnectionSession && (capabilityMask & ownerPairingBit) != 0;

  String get hardwareLabel => switch (hardwareRevision) {
    LitchHardwareRevision.v1 => 'V1 ESP32-C3',
    LitchHardwareRevision.v2 => 'V2 board',
    LitchHardwareRevision.v3 => 'V3 board',
    LitchHardwareRevision.v3_3 => 'V3.3 board',
    LitchHardwareRevision.unknown => 'Litch board',
  };

  static LitchDeviceCapabilities? fromPayload(Map<String, Object?> payload) {
    final Object? rawInfo = payload['i'];
    if (rawInfo is! List<Object?> ||
        (rawInfo.length != 3 && rawInfo.length != 4) ||
        rawInfo.any((value) => value is! int)) {
      return null;
    }

    final int? contractVersion = _toInt(rawInfo[0]);
    final int? revisionId = _toInt(rawInfo[1]);
    final int? capabilityMask = _toInt(rawInfo[2]);
    final int? mode = rawInfo.length >= 4 ? _toInt(rawInfo[3]) : 0;
    if ((rawInfo.length >= 4 && rawInfo[3] is! int) ||
        mode == null ||
        mode < 0 ||
        mode > 1 ||
        (mode == 1 &&
            (contractVersion != _connectionSessionContractVersion ||
                capabilityMask == null ||
                (capabilityMask & ~(firmwareDigestBit | firmwareUpdateBit)) !=
                    0))) {
      return null;
    }
    if (contractVersion == null ||
        contractVersion != _connectionSessionContractVersion ||
        revisionId == null ||
        revisionId < 0 ||
        revisionId > 0xff ||
        capabilityMask == null ||
        capabilityMask < 0 ||
        capabilityMask > _maskMax) {
      return null;
    }

    final LitchHardwareRevision revision = switch (revisionId) {
      1 => LitchHardwareRevision.v1,
      2 => LitchHardwareRevision.v2,
      3 => LitchHardwareRevision.v3,
      4 => LitchHardwareRevision.v3_3,
      _ => LitchHardwareRevision.unknown,
    };
    return LitchDeviceCapabilities(
      contractVersion: contractVersion,
      hardwareRevision: revision,
      capabilityMask: capabilityMask,
      explicitlyReported: true,
      recoveryOnly: mode == 1,
    );
  }

  static int? _toInt(Object? value) {
    if (value is int) {
      return value;
    }
    if (value is num) {
      return value.toInt();
    }
    return int.tryParse((value ?? '').toString());
  }
}
