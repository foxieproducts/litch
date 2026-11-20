import 'dart:convert';
import 'dart:io';

import 'ota_post_reboot_verifier.dart';

abstract interface class OtaPendingTargetStore {
  Future<OtaVerificationTarget?> load();
  Future<void> save(OtaVerificationTarget target);
  Future<void> clear(OtaVerificationTarget target);
}

class PendingOtaTargetStoreException implements Exception {
  const PendingOtaTargetStoreException(this.message);
  final String message;
  @override
  String toString() => 'PendingOtaTargetStoreException: $message';
}

/// One unresolved intent; native connection tokens are never durable authority.
/// A failed or interrupted temporary write remains evidence and blocks replay.
class FilePendingOtaTargetStore implements OtaPendingTargetStore {
  FilePendingOtaTargetStore(this.file);

  final File file;
  File get _temporary => File('${file.path}.tmp');
  static const int _maximumBytes = 4096;

  @override
  Future<OtaVerificationTarget?> load() async {
    if (await file.exists()) return _read(file);
    if (await _temporary.exists()) return _read(_temporary);
    return null;
  }

  Future<OtaVerificationTarget> _read(File input) async {
    final reader = await input.open();
    final List<int> bytes;
    try {
      bytes = await reader.read(_maximumBytes + 1);
    } finally {
      await reader.close();
    }
    if (bytes.length > _maximumBytes) {
      throw const PendingOtaTargetStoreException(
        'Stored update intent exceeds its bound.',
      );
    }
    try {
      final target = _decode(jsonDecode(utf8.decode(bytes)));
      if (target != null) return target;
    } on FormatException {
      // Retain malformed evidence; another transfer cannot replace its owner.
    }
    throw const PendingOtaTargetStoreException(
      'Stored update intent is corrupt or unsupported.',
    );
  }

  @override
  Future<void> save(OtaVerificationTarget target) async {
    final payload = _encode(target);
    if (_decode(payload) == null) {
      throw const PendingOtaTargetStoreException(
        'Update intent is incomplete.',
      );
    }
    if (await _temporary.exists()) {
      throw const PendingOtaTargetStoreException(
        'An interrupted intent must be resolved before another transfer.',
      );
    }
    final existing = await load();
    if (existing != null) {
      if (_same(existing, target)) return;
      throw const PendingOtaTargetStoreException(
        'Another unresolved update owns verification.',
      );
    }
    await file.parent.create(recursive: true);
    final output = await _temporary.open(mode: FileMode.writeOnly);
    try {
      await output.writeString(jsonEncode(payload));
      await output.flush();
    } finally {
      await output.close();
    }
    await _temporary.rename(file.path);
  }

  @override
  Future<void> clear(OtaVerificationTarget target) async {
    // Validate all surviving paths before changing any of them. A malformed or
    // conflicting temporary record is retained even if the primary resolves.
    final matching = <File>[];
    bool retained = false;
    for (final candidate in [file, _temporary]) {
      if (!await candidate.exists()) continue;
      try {
        if (_same(await _read(candidate), target)) {
          matching.add(candidate);
        } else {
          retained = true;
        }
      } on PendingOtaTargetStoreException {
        retained = true;
      }
    }
    for (final candidate in matching) {
      await candidate.delete();
    }
    if (retained) {
      throw const PendingOtaTargetStoreException(
        'Unresolved intent evidence was retained and blocks another transfer.',
      );
    }
  }

  static bool _same(OtaVerificationTarget left, OtaVerificationTarget right) =>
      jsonEncode(_encode(left)) == jsonEncode(_encode(right));

  static Map<String, Object?> _encode(OtaVerificationTarget target) {
    final attempt = target.attempt;
    return {
      'schema': 1,
      'deviceId': target.deviceId,
      'version': target.version,
      'imageSha256': target.imageSha256,
      'artifactSha256': target.artifactSha256,
      'attempt': attempt == null
          ? null
          : {
              'preBootNonce': attempt.preBootNonce,
              'attempt': attempt.attempt,
              'sourceSlot': attempt.sourceSlot,
              'sourceSector': attempt.sourceSector,
              'sourceSequence': attempt.sourceSequence,
              'targetSlot': attempt.targetSlot,
              'targetSector': attempt.targetSector,
              'expectedSequence': attempt.expectedSequence,
            },
    };
  }

  static OtaVerificationTarget? _decode(Object? value) {
    if (value is! Map<String, Object?> ||
        value.length != 6 ||
        value['schema'] is! int ||
        value['schema'] != 1 ||
        !_text(value['deviceId'], 256) ||
        !_text(value['version'], 128) ||
        !_digest(value['imageSha256']) ||
        !_digest(value['artifactSha256'])) {
      return null;
    }
    final a = value['attempt'];
    if (a is! Map<String, Object?> ||
        a.length != 8 ||
        a['preBootNonce'] is! String ||
        ![
          'attempt',
          'sourceSlot',
          'sourceSector',
          'sourceSequence',
          'targetSlot',
          'targetSector',
          'expectedSequence',
        ].every((key) => a[key] is int)) {
      return null;
    }
    final attempt = OtaAttemptIdentity(
      preBootNonce: a['preBootNonce']! as String,
      attempt: a['attempt']! as int,
      sourceSlot: a['sourceSlot']! as int,
      sourceSector: a['sourceSector']! as int,
      sourceSequence: a['sourceSequence']! as int,
      targetSlot: a['targetSlot']! as int,
      targetSector: a['targetSector']! as int,
      expectedSequence: a['expectedSequence']! as int,
    );
    if (!attempt.isValid) return null;
    return OtaVerificationTarget(
      deviceId: value['deviceId']! as String,
      uploadConnectionToken: -1,
      version: value['version']! as String,
      imageSha256: value['imageSha256']! as String,
      artifactSha256: value['artifactSha256']! as String,
      attempt: attempt,
    );
  }

  static bool _text(Object? value, int maximumBytes) =>
      value is String &&
      value.isNotEmpty &&
      utf8.encode(value).length <= maximumBytes &&
      !RegExp(
        r'[\x00-\x1f\x7f-\x9f\u202a-\u202e\u2066-\u2069]',
      ).hasMatch(value);
  static bool _digest(Object? value) =>
      value is String && RegExp(r'^[0-9a-fA-F]{64}$').hasMatch(value);
}
