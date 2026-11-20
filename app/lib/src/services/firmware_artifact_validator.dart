import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import '../models/firmware_manifest.dart';

class FirmwareArtifactValidationException implements Exception {
  const FirmwareArtifactValidationException(this.message);

  static const String userMessage =
      'This firmware package cannot be installed safely. Check for a newer update or contact support.';

  final String message;

  @override
  String toString() => message;
}

enum FirmwareBootPolicyStatus {
  unavailable,
  absent,
  known,
  unsupported,
  malformed,
}

/// Descriptor evidence from verified image bytes, never activation permission.
class FirmwareBootPolicy {
  const FirmwareBootPolicy._(
    this.status, {
    this.schema = 0,
    this.sizeBytes = 0,
    this.epoch = 0,
  });

  final FirmwareBootPolicyStatus status;
  final int schema;
  final int sizeBytes;
  final int epoch;
}

/// Immutable evidence bound to one completely validated artifact and manifest.
class FirmwareArtifactInspection {
  const FirmwareArtifactInspection._({
    required this.sizeBytes,
    required this.artifactSha256,
    required this.imageSha256,
    required this.bootPolicy,
  });

  final int sizeBytes;
  final String artifactSha256;
  final String imageSha256;
  final FirmwareBootPolicy bootPolicy;
}

class FirmwareArtifactValidator {
  static const int _headerSize = 24;
  static const int _segmentHeaderSize = 8;
  static const int _imageHashSize = 32;
  static const int _maximumSegmentCount = 16;
  static const int _checksumSeed = 0xEF;
  static const int _appDescriptorSize = 256;
  static const int _appDescriptorMagic = 0xABCD5432;
  static const int _bootPolicyMagic = 0x54425846; // FXBT, little-endian.
  static const int _bootPolicySize = 16;

  static Future<void> validateFile(File file, FirmwareManifest manifest) async {
    await inspectFile(file, manifest);
  }

  /// Reads once and publishes capability only after complete validation.
  static Future<FirmwareArtifactInspection> inspectFile(
    File file,
    FirmwareManifest manifest,
  ) async {
    final int length = await file.length();
    if (length != manifest.artifact.sizeBytes ||
        length > FirmwareManifest.maxOfficialEsp32s3FirmwareSizeBytes) {
      throw const FirmwareArtifactValidationException(
        'Firmware file size does not match the release manifest.',
      );
    }
    return inspectBytes(await file.readAsBytes(), manifest);
  }

  static void validateBytes(Uint8List bytes, FirmwareManifest manifest) {
    inspectBytes(bytes, manifest);
  }

  /// Selected update admission is stricter than generic fallback inspection.
  static void requireSdkOnlyUpdate(FirmwareArtifactInspection inspection) {
    if (inspection.bootPolicy.status != FirmwareBootPolicyStatus.known ||
        inspection.bootPolicy.epoch != 2) {
      throw const FirmwareArtifactValidationException(
        'Firmware updates require SDK-only boot policy epoch 2.',
      );
    }
  }

  /// Preserves generic integrity acceptance, including legacy fallback images.
  static FirmwareArtifactInspection inspectBytes(
    Uint8List bytes,
    FirmwareManifest manifest,
  ) {
    if (bytes.length != manifest.artifact.sizeBytes ||
        bytes.length > FirmwareManifest.maxOfficialEsp32s3FirmwareSizeBytes) {
      throw const FirmwareArtifactValidationException(
        'Firmware bytes do not match the manifest size.',
      );
    }
    final String fullSha = sha256.convert(bytes).toString();
    if (fullSha != manifest.artifact.sha256) {
      throw const FirmwareArtifactValidationException(
        'Firmware full-file SHA-256 does not match the manifest.',
      );
    }
    if (bytes.length < _headerSize + _segmentHeaderSize + 1 + _imageHashSize) {
      throw const FirmwareArtifactValidationException(
        'Firmware image is too short.',
      );
    }
    if (bytes[0] != 0xE9) {
      throw const FirmwareArtifactValidationException(
        'Firmware image magic is not 0xe9.',
      );
    }

    final int segmentCount = bytes[1];
    if (segmentCount < 1 || segmentCount > _maximumSegmentCount) {
      throw const FirmwareArtifactValidationException(
        'Firmware segment count is outside the supported range.',
      );
    }
    final ByteData view = ByteData.sublistView(bytes);
    if (view.getUint16(12, Endian.little) != 9) {
      throw const FirmwareArtifactValidationException(
        'Firmware image does not target ESP32-S3.',
      );
    }
    if (bytes[23] != 1) {
      throw const FirmwareArtifactValidationException(
        'Firmware image does not contain an appended SHA-256.',
      );
    }

    int offset = _headerSize;
    int checksum = _checksumSeed;
    int firstSegmentOffset = 0;
    int firstSegmentSize = 0;
    for (int index = 0; index < segmentCount; ++index) {
      if (offset + _segmentHeaderSize > bytes.length) {
        throw const FirmwareArtifactValidationException(
          'Firmware segment header is truncated.',
        );
      }
      final int segmentSize = view.getUint32(offset + 4, Endian.little);
      offset += _segmentHeaderSize;
      if (segmentSize == 0 || segmentSize % 4 != 0) {
        throw const FirmwareArtifactValidationException(
          'Firmware segment size is invalid.',
        );
      }
      final int segmentEnd = offset + segmentSize;
      if (segmentEnd > bytes.length) {
        throw const FirmwareArtifactValidationException(
          'Firmware segment data is truncated.',
        );
      }
      if (index == 0) {
        firstSegmentOffset = offset;
        firstSegmentSize = segmentSize;
      }
      for (int cursor = offset; cursor < segmentEnd; ++cursor) {
        checksum ^= bytes[cursor];
      }
      offset = segmentEnd;
    }

    final int checksumOffset = offset + ((15 - offset) % 16);
    final int prefixEnd = checksumOffset + 1;
    if (bytes.length != prefixEnd + _imageHashSize) {
      throw const FirmwareArtifactValidationException(
        'Firmware exact size does not match its parsed segments.',
      );
    }
    for (int cursor = offset; cursor < checksumOffset; ++cursor) {
      if (bytes[cursor] != 0) {
        throw const FirmwareArtifactValidationException(
          'Firmware checksum padding is invalid.',
        );
      }
    }
    if (bytes[checksumOffset] != checksum) {
      throw const FirmwareArtifactValidationException(
        'Firmware segment checksum does not match.',
      );
    }

    final Uint8List prefix = Uint8List.sublistView(bytes, 0, prefixEnd);
    final Digest calculatedImageDigest = sha256.convert(prefix);
    final Uint8List trailingImageHash = Uint8List.sublistView(bytes, prefixEnd);
    if (!_bytesEqual(calculatedImageDigest.bytes, trailingImageHash)) {
      throw const FirmwareArtifactValidationException(
        'Firmware trailing image SHA-256 does not match its prefix.',
      );
    }
    if (calculatedImageDigest.toString() != manifest.artifact.imageSha256) {
      throw const FirmwareArtifactValidationException(
        'Firmware image SHA-256 does not match the manifest.',
      );
    }
    return FirmwareArtifactInspection._(
      sizeBytes: bytes.length,
      artifactSha256: fullSha,
      imageSha256: calculatedImageDigest.toString(),
      bootPolicy: _inspectBootPolicy(
        view,
        firstSegmentOffset,
        firstSegmentSize,
      ),
    );
  }

  static FirmwareBootPolicy _inspectBootPolicy(
    ByteData view,
    int segmentOffset,
    int segmentSize,
  ) {
    if (segmentSize < _appDescriptorSize ||
        view.getUint32(segmentOffset, Endian.little) != _appDescriptorMagic) {
      return const FirmwareBootPolicy._(FirmwareBootPolicyStatus.unavailable);
    }
    // Only this fixed location inside the first segment establishes evidence.
    // Later segments cannot complete a short recognized descriptor.
    final int offset = segmentOffset + _appDescriptorSize;
    final int available = segmentSize - _appDescriptorSize;
    if (available < 4 ||
        view.getUint32(offset, Endian.little) != _bootPolicyMagic) {
      return const FirmwareBootPolicy._(FirmwareBootPolicyStatus.absent);
    }
    final int schema = available >= 8
        ? view.getUint16(offset + 4, Endian.little)
        : 0;
    final int size = available >= 8
        ? view.getUint16(offset + 6, Endian.little)
        : 0;
    if (available < _bootPolicySize) {
      return FirmwareBootPolicy._(
        FirmwareBootPolicyStatus.malformed,
        schema: schema,
        sizeBytes: size,
      );
    }
    if (schema != 1) {
      return FirmwareBootPolicy._(
        FirmwareBootPolicyStatus.unsupported,
        schema: schema,
        sizeBytes: size,
      );
    }
    if (size != _bootPolicySize ||
        view.getUint32(offset + 12, Endian.little) != 0) {
      return FirmwareBootPolicy._(
        FirmwareBootPolicyStatus.malformed,
        schema: schema,
        sizeBytes: size,
      );
    }
    final int epoch = view.getUint32(offset + 8, Endian.little);
    return FirmwareBootPolicy._(
      epoch == 1 || epoch == 2
          ? FirmwareBootPolicyStatus.known
          : FirmwareBootPolicyStatus.unsupported,
      schema: schema,
      sizeBytes: size,
      epoch: epoch,
    );
  }

  static bool _bytesEqual(List<int> lhs, List<int> rhs) {
    if (lhs.length != rhs.length) {
      return false;
    }
    int difference = 0;
    for (int index = 0; index < lhs.length; ++index) {
      difference |= lhs[index] ^ rhs[index];
    }
    return difference == 0;
  }
}
