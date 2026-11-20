import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:crypto/crypto.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';

/// Immutable generated test evidence; prior artwork copies require explicit opt-in.
/// Checkpoints are append-only: a torn newest checkpoint fails closed on resume.
class CoreBackup {
  CoreBackup._(this.directory, this.manifest, this._sequence);

  static const maximumPhoneBytes = 1100000000;
  static const maximumFileBytes = 64 * 1024 * 1024;
  static const maximumFiles = 10000;
  final Directory directory;
  Map<String, dynamic> manifest;
  int _sequence;
  // Older saved preservation runs predate the explicit mode field.
  bool get preservesArtwork => manifest['backupArtwork'] != false;

  static Future<CoreBackup> create({
    required Directory support,
    required String target,
    required String firmwareSha256,
    bool backupArtwork = false,
    Future<Map<String, Object>> Function()? readPreferences,
  }) async {
    if (backupArtwork && readPreferences == null) {
      throw ArgumentError('Explicit artwork backup requires preference access');
    }
    final nonce = Random.secure().nextInt(0x7fffffff).toRadixString(16);
    final folder = backupArtwork
        ? 'core-acceptance-backups'
        : 'core-acceptance-runs';
    final run = Directory(
      '${support.path}/$folder/'
      '${DateTime.now().toUtc().microsecondsSinceEpoch}-$nonce',
    );
    await run.create(recursive: true);
    final backup = CoreBackup._(run, {
      'version': 1,
      'backupArtwork': backupArtwork,
      'target': target,
      'firmwareSha256': firmwareSha256,
      'phone': <Map<String, dynamic>>[],
      'documents': <Map<String, dynamic>>[],
      'originals': <String>[],
      'stage': backupArtwork ? 'phone_backup' : 'evidence_ready',
    }, 0);
    if (!backupArtwork) {
      await backup.checkpoint();
      return backup;
    }
    final source = Directory('${support.path}/litchcraft');
    final wasPresent = await source.exists();
    if (wasPresent &&
        await FileSystemEntity.type(source.path, followLinks: false) !=
            FileSystemEntityType.directory) {
      throw StateError('Phone library root must be a real directory');
    }
    final before = await _inventory(source);
    final prefsBefore = _preferences(await readPreferences!());
    var total = utf8.encode(prefsBefore).length;
    if (total > maximumFileBytes) {
      throw StateError('Phone preferences exceed backup allowance');
    }
    final entries = <Map<String, dynamic>>[];
    for (final path in before.keys) {
      final file = File('${source.path}/$path');
      final stat = before[path]!;
      total += stat.size;
      if (stat.size > maximumFileBytes || total > maximumPhoneBytes) {
        throw StateError('Phone backup exceeds bounded diagnostic allowance');
      }
      final saved = File('${run.path}/phone/litchcraft/$path');
      await saved.parent.create(recursive: true);
      final output = await saved.open(mode: FileMode.writeOnly);
      try {
        await for (final bytes in file.openRead()) {
          await output.writeFrom(bytes);
        }
        await output.flush();
      } finally {
        await output.close();
      }
      final digest = await _hash(saved);
      if (await saved.length() != stat.size || digest != await _hash(file)) {
        throw StateError('Phone artwork changed or backup copy is incomplete');
      }
      entries.add({
        'path': 'phone/litchcraft/$path',
        'bytes': stat.size,
        'sha256': digest,
      });
    }
    final after = await _inventory(source);
    for (final entry in entries) {
      final relative = (entry['path'] as String).substring(
        'phone/litchcraft/'.length,
      );
      if (await _hash(File('${source.path}/$relative')) != entry['sha256']) {
        throw StateError('Phone artwork changed before backup completed');
      }
    }
    if (wasPresent != await source.exists() ||
        !_sameInventory(before, after) ||
        prefsBefore != _preferences(await readPreferences())) {
      throw StateError('Phone library/preferences changed during backup');
    }
    final preferences = await backup._saveBytes(
      'phone/preferences.json',
      utf8.encode(prefsBefore),
    );
    entries.add(preferences);
    backup.manifest['phone'] = entries;
    backup.manifest['phoneLibraryPresent'] = await source.exists();
    backup.manifest['stage'] = 'phone_saved';
    await backup.checkpoint();
    await backup.validate();
    return backup;
  }

  static String _preferences(Map<String, Object> values) {
    final keys =
        values.keys
            .where((key) => key.toLowerCase().contains('litchcraft'))
            .toList()
          ..sort();
    return jsonEncode({for (final key in keys) key: values[key]});
  }

  static Future<Map<String, FileStat>> _inventory(Directory directory) async {
    final result = <String, FileStat>{};
    if (!await directory.exists()) return result;
    await for (final entity in directory.list(
      recursive: true,
      followLinks: false,
    )) {
      final type = await FileSystemEntity.type(entity.path, followLinks: false);
      if (type == FileSystemEntityType.link) {
        throw StateError('Phone artwork backup refuses symbolic links');
      }
      if (type == FileSystemEntityType.directory) continue;
      if (type != FileSystemEntityType.file || result.length >= maximumFiles) {
        throw StateError('Phone artwork inventory is unsupported or too large');
      }
      final relative = entity.path.substring(directory.path.length + 1);
      _safePath(relative);
      result[relative] = await entity.stat();
    }
    return Map.fromEntries(
      result.entries.toList()..sort((a, b) => a.key.compareTo(b.key)),
    );
  }

  static bool _sameInventory(
    Map<String, FileStat> a,
    Map<String, FileStat> b,
  ) =>
      a.length == b.length &&
      a.entries.every((entry) {
        final other = b[entry.key];
        return other != null &&
            entry.value.size == other.size &&
            entry.value.modified == other.modified &&
            entry.value.changed == other.changed;
      });

  static void _safePath(String path) {
    if (path.isEmpty ||
        path.startsWith('/') ||
        path.contains('\\') ||
        path
            .split('/')
            .any((part) => part.isEmpty || part == '.' || part == '..')) {
      throw const FormatException('Unsafe diagnostic backup path');
    }
  }

  static Future<String> _hash(File file) async =>
      (await sha256.bind(file.openRead()).first).toString();

  Future<Map<String, dynamic>> _saveBytes(String path, List<int> bytes) async {
    _safePath(path);
    final file = File('${directory.path}/$path');
    if (await file.exists()) {
      throw StateError('Immutable diagnostic file already exists');
    }
    await file.parent.create(recursive: true);
    await file.writeAsBytes(bytes, flush: true);
    final digest = sha256.convert(bytes).toString();
    if (await file.length() != bytes.length || await _hash(file) != digest) {
      throw StateError('Diagnostic evidence failed reread/hash verification');
    }
    return {'path': path, 'bytes': bytes.length, 'sha256': digest};
  }

  Future<String> saveDocument(
    LitchcraftAnimationDocument document,
    String role,
  ) async {
    final encoded = encodeLitchcraftAnimationDocument(
      document,
      localStorage: true,
    );
    final decoded = decodeLitchcraftAnimationDocument(
      encoded.bytes,
      encoded.crc32,
      localStorage: true,
    );
    if (!sameLitchcraftDocument(document, decoded)) {
      throw StateError('Document evidence codec changed semantics');
    }
    final index = (manifest['documents'] as List).length;
    final entry = await _saveBytes('documents/$index.bin', encoded.bytes);
    entry.addAll({'crc32': encoded.crc32, 'role': role});
    (manifest['documents'] as List).add(entry);
    await checkpoint();
    return entry['path'] as String;
  }

  Future<LitchcraftAnimationDocument> document(String path) async {
    final entry = (manifest['documents'] as List).cast<Map>().singleWhere(
      (item) => item['path'] == path,
    );
    final bytes = await _verifiedBytes(entry);
    return decodeLitchcraftAnimationDocument(
      bytes,
      entry['crc32'] as int,
      localStorage: true,
    );
  }

  Future<List<int>> _verifiedBytes(Map entry) async {
    final path = entry['path'] as String;
    _safePath(path);
    final file = File('${directory.path}/$path');
    if (await FileSystemEntity.type(file.path, followLinks: false) !=
            FileSystemEntityType.file ||
        entry['bytes'] is! int ||
        (entry['bytes'] as int) > maximumFileBytes ||
        await file.length() != entry['bytes'] ||
        await _hash(file) != entry['sha256']) {
      throw const FormatException(
        'Diagnostic evidence is missing, incomplete or corrupt',
      );
    }
    return file.readAsBytes();
  }

  Future<void> validate() async {
    if (!preservesArtwork &&
        ((manifest['phone'] as List).isNotEmpty ||
            (manifest['originals'] as List).isNotEmpty)) {
      throw const FormatException(
        'Default run cannot contain prior-art backups',
      );
    }
    var total = 0;
    for (final entry in [
      ...manifest['phone'] as List,
      ...manifest['documents'] as List,
    ]) {
      final bytes = (entry as Map)['bytes'];
      if (bytes is! int || bytes < 0 || bytes > maximumFileBytes) {
        throw const FormatException('Invalid diagnostic file byte bound');
      }
      total += bytes;
      if (total > maximumPhoneBytes + 32 * maximumFileBytes) {
        throw const FormatException(
          'Diagnostic evidence exceeds bounded allowance',
        );
      }
      // Phone payloads can be large; validate without retaining them in memory.
      final path = entry['path'] as String;
      _safePath(path);
      final file = File('${directory.path}/$path');
      if (await FileSystemEntity.type(file.path, followLinks: false) !=
              FileSystemEntityType.file ||
          await file.length() != entry['bytes'] ||
          await _hash(file) != entry['sha256']) {
        throw const FormatException('Diagnostic evidence hash/length mismatch');
      }
    }
    for (final path in (manifest['originals'] as List).cast<String>()) {
      await document(path);
    }
  }

  Future<void> checkpoint() async {
    final content = jsonEncode(manifest);
    final envelope = utf8.encode(
      jsonEncode({
        'content': content,
        'sha256': sha256.convert(utf8.encode(content)).toString(),
      }),
    );
    if (envelope.length > 1024 * 1024 || _sequence >= 200) {
      throw StateError('Diagnostic checkpoint allowance exhausted');
    }
    await _saveBytes(
      'checkpoint-${(_sequence++).toString().padLeft(4, '0')}.json',
      envelope,
    );
  }

  static Future<CoreBackup> resume(Directory directory) async {
    final candidates = <File>[];
    await for (final entity in directory.list(followLinks: false)) {
      if (RegExp(r'/checkpoint-\d{4}\.json$').hasMatch(entity.path)) {
        if (entity is! File || candidates.length >= 200) {
          throw const FormatException(
            'Invalid diagnostic checkpoint inventory',
          );
        }
        candidates.add(entity);
      }
    }
    candidates.sort((a, b) => a.path.compareTo(b.path));
    if (candidates.isEmpty || await candidates.last.length() > 1024 * 1024) {
      throw const FormatException('No bounded saved diagnostic checkpoint');
    }
    final envelope = jsonDecode(await candidates.last.readAsString()) as Map;
    final content = envelope['content'] as String;
    if (sha256.convert(utf8.encode(content)).toString() != envelope['sha256']) {
      throw const FormatException('Saved diagnostic checkpoint is corrupt');
    }
    final manifest = jsonDecode(content) as Map<String, dynamic>;
    if (manifest['version'] != 1 ||
        (manifest['backupArtwork'] != null &&
            manifest['backupArtwork'] is! bool) ||
        manifest['phone'] is! List ||
        manifest['documents'] is! List ||
        manifest['originals'] is! List ||
        (manifest['phone'] as List).length > maximumFiles + 1 ||
        (manifest['documents'] as List).length > 32) {
      throw const FormatException('Unsupported diagnostic checkpoint');
    }
    final sequence =
        int.parse(
          candidates.last.path.substring(
            candidates.last.path.length - 9,
            candidates.last.path.length - 5,
          ),
        ) +
        1;
    final backup = CoreBackup._(directory, manifest, sequence);
    await backup.validate();
    return backup;
  }
}
