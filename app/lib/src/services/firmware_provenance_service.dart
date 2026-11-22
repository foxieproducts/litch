import '../models/firmware_provenance.dart';

abstract interface class FirmwareProvenanceVerifier {
  Future<bool?> isOfficialDigest(String sha256);
}

class UnavailableFirmwareProvenanceVerifier
    implements FirmwareProvenanceVerifier {
  const UnavailableFirmwareProvenanceVerifier();

  @override
  Future<bool?> isOfficialDigest(String sha256) async => null;
}

class FirmwareProvenanceService {
  const FirmwareProvenanceService({
    this.verifier = const UnavailableFirmwareProvenanceVerifier(),
  });

  final FirmwareProvenanceVerifier verifier;

  Future<FirmwareProvenance> inspect({
    required bool connected,
    required Future<String?> Function() readDigest,
  }) async {
    if (!connected) {
      return FirmwareProvenance.offline;
    }

    String? digest;
    try {
      digest = (await readDigest())?.trim().toLowerCase();
    } catch (_) {
      digest = null;
    }
    if (digest == null || !RegExp(r'^[0-9a-f]{64}$').hasMatch(digest)) {
      return FirmwareProvenance.unverified;
    }

    bool? official;
    try {
      official = await verifier.isOfficialDigest(digest);
    } catch (_) {
      official = null;
    }
    if (official == true) {
      return FirmwareProvenance.official(digest);
    }
    if (official == false) {
      return FirmwareProvenance.custom(digest);
    }
    return FirmwareProvenance(
      state: FirmwareProvenanceState.unverified,
      title: FirmwareProvenance.unverified.title,
      message: FirmwareProvenance.unverified.message,
      sha256: digest,
    );
  }
}
