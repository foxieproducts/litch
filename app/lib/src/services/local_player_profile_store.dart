import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

import '../models/local_player_profile.dart';

const String localPlayerProfileExportFormat =
    'foxie-litch-local-player-profiles';
const int localPlayerProfileExportSchemaVersion = 1;

typedef LocalPlayerAssignment =
    Future<LocalPlayerDirectory> Function({
      required String boardId,
      required LocalPlayerChessSide side,
      String? displayName,
      Future<void>? cancellation,
      bool Function()? beginCommit,
    });

class LocalPlayerProfileLoadResult {
  const LocalPlayerProfileLoadResult({
    required this.directory,
    this.recoveredFromBackup = false,
    this.rejectedStoredData = false,
  });

  final LocalPlayerDirectory directory;
  final bool recoveredFromBackup;
  final bool rejectedStoredData;
}

class LocalPlayerProfileStore {
  const LocalPlayerProfileStore(this.preferences);

  static const String _currentKey = 'local_player_profiles_v1';
  static const String _backupKey = 'local_player_profiles_v1_previous';

  final SharedPreferences preferences;

  static Future<LocalPlayerProfileStore> open() async =>
      LocalPlayerProfileStore(await SharedPreferences.getInstance());

  Future<LocalPlayerProfileLoadResult> load() async {
    final String? current = preferences.getString(_currentKey);
    if (current != null) {
      try {
        return LocalPlayerProfileLoadResult(
          directory: _decodeDirectory(current),
        );
      } on FormatException {
        // Preserve the rejected value and try the last validated snapshot.
      }
    }

    final String? backup = preferences.getString(_backupKey);
    if (backup != null) {
      try {
        return LocalPlayerProfileLoadResult(
          directory: _decodeDirectory(backup),
          recoveredFromBackup: true,
          rejectedStoredData: current != null,
        );
      } on FormatException {
        // Fall through to a safe empty directory without overwriting evidence.
      }
    }

    return LocalPlayerProfileLoadResult(
      directory: LocalPlayerDirectory.empty(),
      rejectedStoredData: current != null || backup != null,
    );
  }

  Future<void> save(LocalPlayerDirectory directory) async {
    directory.validate();
    final String encoded = jsonEncode(directory.toJson());
    _decodeDirectory(encoded);

    final String? current = preferences.getString(_currentKey);
    if (current != null) {
      try {
        _decodeDirectory(current);
        if (!await preferences.setString(_backupKey, current)) {
          throw StateError('Could not preserve the previous player profiles.');
        }
      } on FormatException {
        // Never replace a previously validated backup with corrupt current data.
      }
    }
    if (!await preferences.setString(_currentKey, encoded)) {
      throw StateError('Could not save local player profiles.');
    }
  }

  Future<void> clear() async {
    await preferences.remove(_currentKey);
    await preferences.remove(_backupKey);
    if (preferences.containsKey(_currentKey) ||
        preferences.containsKey(_backupKey)) {
      throw StateError('Could not delete all local player profiles.');
    }
  }

  String export({
    required LocalPlayerDirectory directory,
    required DateTime exportedAt,
  }) {
    directory.validate();
    final Map<String, Object?> document = <String, Object?>{
      'format': localPlayerProfileExportFormat,
      'schemaVersion': localPlayerProfileExportSchemaVersion,
      'exportedAt': exportedAt.toUtc().toIso8601String(),
      'players': directory.toJson(),
    };
    return '${jsonEncode(document)}\n';
  }

  LocalPlayerDirectory validateExport(String source) {
    final Object? decoded = jsonDecode(source);
    if (decoded is! Map<String, Object?> ||
        decoded['format'] != localPlayerProfileExportFormat ||
        decoded['schemaVersion'] != localPlayerProfileExportSchemaVersion) {
      throw const FormatException('Unsupported local player profile export.');
    }
    final Object? rawExportedAt = decoded['exportedAt'];
    final DateTime? exportedAt = rawExportedAt is String
        ? DateTime.tryParse(rawExportedAt)
        : null;
    if (exportedAt == null || !exportedAt.isUtc) {
      throw const FormatException('Invalid local player profile export time.');
    }
    final Object? rawPlayers = decoded['players'];
    if (rawPlayers is! Map<String, Object?>) {
      throw const FormatException('Invalid local player profile export.');
    }
    return LocalPlayerDirectory.fromJson(rawPlayers);
  }

  LocalPlayerDirectory _decodeDirectory(String source) {
    final Object? decoded = jsonDecode(source);
    if (decoded is! Map<String, Object?>) {
      throw const FormatException(
        'Local player profile root must be an object.',
      );
    }
    return LocalPlayerDirectory.fromJson(decoded);
  }
}
