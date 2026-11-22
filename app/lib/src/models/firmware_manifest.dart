import 'dart:convert';

class FirmwareManifestFormatException implements Exception {
  const FirmwareManifestFormatException(this.message);

  final String message;

  @override
  String toString() => message;
}

class FirmwareArtifactInfo {
  const FirmwareArtifactInfo({
    required this.url,
    required this.sizeBytes,
    required this.sha256,
    required this.imageSha256,
  });

  final Uri url;
  final int sizeBytes;
  final String sha256;
  final String imageSha256;
}

class FirmwareNotesInfo {
  const FirmwareNotesInfo({
    required this.url,
    required this.sizeBytes,
    required this.sha256,
  });

  final Uri url;
  final int sizeBytes;
  final String sha256;
}

class FirmwareManifest {
  const FirmwareManifest({
    required this.schemaVersion,
    required this.channel,
    required this.version,
    required this.target,
    required this.chipId,
    required this.artifact,
    required this.notes,
  });

  static const int maxOfficialEsp32s3FirmwareSizeBytes = 2 * 1024 * 1024;
  static const int maxNotesSizeBytes = 256 * 1024;

  final int schemaVersion;
  final String channel;
  final String version;
  final String target;
  final int chipId;
  final FirmwareArtifactInfo artifact;
  final FirmwareNotesInfo notes;

  factory FirmwareManifest.parse(
    String source, {
    required Uri manifestUrl,
    required String expectedChannel,
  }) {
    final Object? decoded;
    try {
      decoded = jsonDecode(source);
    } on FormatException catch (error) {
      throw FirmwareManifestFormatException(
        'Firmware manifest is not valid JSON: $error',
      );
    }
    if (decoded is! Map<String, dynamic>) {
      throw const FirmwareManifestFormatException(
        'Firmware manifest root must be an object.',
      );
    }

    _requireSecureUrl(manifestUrl, 'Manifest URL');
    final int schemaVersion = _requiredInt(decoded, 'schema_version');
    final String channel = _requiredString(decoded, 'channel');
    final String version = _requiredString(decoded, 'version');
    final String target = _requiredString(decoded, 'target');
    final int chipId = _requiredInt(decoded, 'chip_id');
    if (schemaVersion != 1) {
      throw const FirmwareManifestFormatException(
        'Unsupported firmware manifest schema.',
      );
    }
    if (channel != expectedChannel) {
      throw const FirmwareManifestFormatException(
        'Firmware manifest channel does not match the selected release.',
      );
    }
    if (target != 'esp32s3' || chipId != 9) {
      throw const FirmwareManifestFormatException(
        'Firmware manifest does not target ESP32-S3 hardware.',
      );
    }

    final Object? artifactValue = decoded['artifact'];
    if (artifactValue is! Map<String, dynamic>) {
      throw const FirmwareManifestFormatException(
        'Firmware manifest artifact must be an object.',
      );
    }
    final Uri artifactUrl = _requiredUri(artifactValue, 'url');
    final int sizeBytes = _requiredInt(artifactValue, 'size_bytes');
    final String fullSha = _requiredSha(artifactValue, 'sha256');
    final String imageSha = _requiredSha(artifactValue, 'image_sha256');
    if (sizeBytes <= 0 || sizeBytes > maxOfficialEsp32s3FirmwareSizeBytes) {
      throw const FirmwareManifestFormatException(
        'Firmware artifact size is outside the supported range.',
      );
    }
    _requireSameOrigin(artifactUrl, manifestUrl, 'Firmware artifact URL');
    final String artifactName = artifactUrl.pathSegments.isEmpty
        ? ''
        : artifactUrl.pathSegments.last;
    if (artifactName == 'firmware.bin' ||
        artifactName == 'firmware_beta.bin' ||
        !artifactName.endsWith('.bin') ||
        !artifactName.contains(fullSha) ||
        artifactName.contains('/')) {
      throw const FirmwareManifestFormatException(
        'Firmware artifact URL must be immutable and SHA-addressed.',
      );
    }

    final Object? notesValue = decoded['notes'];
    if (notesValue is! Map<String, dynamic>) {
      throw const FirmwareManifestFormatException(
        'Firmware manifest notes must be an object.',
      );
    }
    final Uri notesUrl = _requiredUri(notesValue, 'url');
    final int notesSizeBytes = _requiredInt(notesValue, 'size_bytes');
    final String notesSha = _requiredSha(notesValue, 'sha256');
    if (notesSizeBytes <= 0 || notesSizeBytes > maxNotesSizeBytes) {
      throw const FirmwareManifestFormatException(
        'Firmware notes size is outside the supported range.',
      );
    }
    _requireSameOrigin(notesUrl, manifestUrl, 'Firmware notes URL');
    final String notesName = notesUrl.pathSegments.isEmpty
        ? ''
        : notesUrl.pathSegments.last;
    if (notesName == 'version.md' ||
        notesName == 'version_beta.md' ||
        !notesName.endsWith('.md') ||
        !notesName.contains(notesSha) ||
        notesName.contains('/')) {
      throw const FirmwareManifestFormatException(
        'Firmware notes URL must be immutable and SHA-addressed.',
      );
    }

    return FirmwareManifest(
      schemaVersion: schemaVersion,
      channel: channel,
      version: version,
      target: target,
      chipId: chipId,
      artifact: FirmwareArtifactInfo(
        url: artifactUrl,
        sizeBytes: sizeBytes,
        sha256: fullSha,
        imageSha256: imageSha,
      ),
      notes: FirmwareNotesInfo(
        url: notesUrl,
        sizeBytes: notesSizeBytes,
        sha256: notesSha,
      ),
    );
  }

  static String _requiredString(Map<String, dynamic> source, String key) {
    final Object? value = source[key];
    if (value is! String || value.trim().isEmpty) {
      throw FirmwareManifestFormatException(
        'Firmware manifest field "$key" must be a non-empty string.',
      );
    }
    return value;
  }

  static int _requiredInt(Map<String, dynamic> source, String key) {
    final Object? value = source[key];
    if (value is! int) {
      throw FirmwareManifestFormatException(
        'Firmware manifest field "$key" must be an integer.',
      );
    }
    return value;
  }

  static String _requiredSha(Map<String, dynamic> source, String key) {
    final String value = _requiredString(source, key);
    if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(value)) {
      throw FirmwareManifestFormatException(
        'Firmware manifest field "$key" must be lowercase SHA-256.',
      );
    }
    return value;
  }

  static Uri _requiredUri(Map<String, dynamic> source, String key) {
    final String value = _requiredString(source, key);
    final Uri? uri = Uri.tryParse(value);
    if (uri == null) {
      throw FirmwareManifestFormatException(
        'Firmware manifest field "$key" must be a URL.',
      );
    }
    _requireSecureUrl(uri, 'Firmware manifest field "$key"');
    return uri;
  }

  static void _requireSecureUrl(Uri uri, String label) {
    if (uri.scheme != 'https' ||
        uri.host.isEmpty ||
        uri.userInfo.isNotEmpty ||
        uri.hasQuery ||
        uri.hasFragment) {
      throw FirmwareManifestFormatException(
        '$label must be a plain HTTPS URL.',
      );
    }
  }

  static void _requireSameOrigin(Uri uri, Uri manifestUrl, String label) {
    _requireSecureUrl(uri, label);
    final int uriPort = uri.hasPort ? uri.port : 443;
    final int manifestPort = manifestUrl.hasPort ? manifestUrl.port : 443;
    if (uri.host.toLowerCase() != manifestUrl.host.toLowerCase() ||
        uriPort != manifestPort) {
      throw FirmwareManifestFormatException(
        '$label must use the manifest origin.',
      );
    }
  }
}
