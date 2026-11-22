import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../models/litchcraft_artwork.dart';
import '../models/litchcraft_state.dart';
import 'app_litchcraft_preferences.dart';

part 'litchcraft_artwork_file_backend.dart';

enum LitchcraftDraftOrigin { anonymous, external, saved }

LitchcraftDraftOrigin? _decodeDraftOrigin(Object? value) {
  if (value == null) return null; // Older catalogs infer ownership as before.
  for (final origin in LitchcraftDraftOrigin.values) {
    if (value == origin.name) return origin;
  }
  throw const FormatException('Invalid artwork draft origin.');
}

class LitchcraftLibrarySnapshot {
  LitchcraftLibrarySnapshot({
    this.artworks = const <LitchcraftArtwork>[],
    this.draft,
    this.draftOrigin,
    this.previousDraft,
    this.recentColors = const <int>[],
    this.dismissedWelcome = false,
    this.recovered = false,
    this.rejected = false,
  });

  final List<LitchcraftArtwork> artworks;
  final LitchcraftArtwork? draft;
  final LitchcraftDraftOrigin? draftOrigin;
  final LitchcraftArtwork? previousDraft;
  final List<int> recentColors;
  final bool dismissedWelcome;
  final bool recovered;
  final bool rejected;

  Map<String, Object?> toJson() => <String, Object?>{
    'version': 1,
    'artworks': artworks.map((art) => art.toJson()).toList(),
    'draft': draft?.toJson(),
    if (draftOrigin != null) 'draftOrigin': draftOrigin!.name,
    'previousDraft': previousDraft?.toJson(),
    'recentColors': recentColors,
    'dismissedWelcome': dismissedWelcome,
  };
}

/// The configured allowance covers distinct retained artwork payloads. A limit
/// below existing usage never discards artwork; only further growth is rejected.
class LitchcraftStorageLimitExceeded implements Exception {
  const LitchcraftStorageLimitExceeded({
    required this.limitBytes,
    required this.requiredBytes,
  });

  final int limitBytes;
  final int requiredBytes;

  @override
  String toString() =>
      'Artwork storage is full. Delete artwork or increase the allowance in Setup.';
}

/// Production uses immutable per-artwork files and a small atomic catalog.
/// The preferences constructor remains injectable for existing isolated tests.
class LitchcraftArtworkStore {
  LitchcraftArtworkStore(
    this.preferences, {
    int Function()? storageLimitProvider,
  }) : _files = null,
       _storageLimitProvider = storageLimitProvider;
  LitchcraftArtworkStore._(
    this.preferences,
    this._files,
    this._storageLimitProvider,
  );

  static const currentKey = 'litchcraft_artwork_v1';
  static const backupKey = 'litchcraft_artwork_v1_previous';
  static const rejectedKey = 'litchcraft_artwork_v1_rejected';
  final SharedPreferences preferences;
  final _LitchcraftArtworkFiles? _files;
  final int Function()? _storageLimitProvider;
  Future<void> _tail = Future<void>.value();

  static Future<LitchcraftArtworkStore> open({
    SharedPreferences? preferences,
    Directory? directory,
    int Function()? storageLimitProvider,
  }) async {
    final prefs = preferences ?? await SharedPreferences.getInstance();
    final location =
        directory ??
        Directory(
          '${(await getApplicationSupportDirectory()).path}/litchcraft',
        );
    final backend = _LitchcraftArtworkFiles(location);
    final store = LitchcraftArtworkStore._(
      prefs,
      backend,
      storageLimitProvider,
    );
    await backend.initialize(prefs);
    return store;
  }

  // SharedPreferences changes its cache before a platform write completes.
  // Production supplies the app owner's last durably accepted allowance.
  int get storageLimitBytes => AppLitchcraftPreferences.normalizeStorageLimit(
    _storageLimitProvider?.call() ??
        preferences.getInt(AppLitchcraftPreferences.storageLimitKey) ??
        AppLitchcraftPreferences.defaultStorageLimitBytes,
  );

  /// Counters expose actual payload I/O for bounded-storage regression evidence.
  int get artworkPayloadReads => _files?.payloadReads ?? 0;
  int get artworkPayloadWrites => _files?.payloadWrites ?? 0;

  /// Call after the corresponding save succeeds to release a decoded library
  /// animation. The active editor may continue owning its one loaded document.
  LitchcraftArtwork compactArtwork(LitchcraftArtwork art) =>
      _files?.compactArtwork(art) ?? art;

  Future<LitchcraftLibrarySnapshot> load() async {
    if (_files case final files?) return files.load();
    final current = preferences.getString(currentKey);
    if (current != null) {
      try {
        return _decodeLegacyLibrary(current);
      } on FormatException {
        /* Try backup. */
      }
    }
    final backup = preferences.getString(backupKey);
    if (backup != null) {
      try {
        return _decodeLegacyLibrary(
          backup,
          recovered: true,
          rejected: current != null,
        );
      } on FormatException {
        /* Keep rejected bytes for recovery. */
      }
    }
    return LitchcraftLibrarySnapshot(
      rejected: current != null || backup != null,
    );
  }

  Future<void> save(LitchcraftLibrarySnapshot snapshot) {
    _validateLibraryMetadata(snapshot);
    if (_files case final files?) {
      return _serialize(() => files.save(snapshot, storageLimitBytes));
    }
    // The injected preferences backend keeps its original synchronous ingress
    // validation. It shares the dynamic byte allowance and has no count cap.
    final encoded = jsonEncode(snapshot.toJson());
    _decodeLegacyLibrary(encoded);
    return _serialize(() async {
      final current = preferences.getString(currentKey);
      final retained = <String>{
        ?current,
        ?preferences.getString(backupKey),
        ?preferences.getString(rejectedKey),
      };
      final proposed = <String>{encoded};
      if (current != null) {
        try {
          _decodeLegacyLibrary(current);
          proposed.add(current);
        } on FormatException {
          if (preferences.getString(backupKey) case final value?) {
            proposed.add(value);
          }
          proposed.add(preferences.getString(rejectedKey) ?? current);
        }
      }
      if (preferences.getString(rejectedKey) case final value?) {
        proposed.add(value);
      }
      _checkAllowance(
        _stringBytes(retained),
        _stringBytes(proposed),
        storageLimitBytes,
      );
      if (current != null) {
        try {
          _decodeLegacyLibrary(current);
          if (!await preferences.setString(backupKey, current)) {
            throw StateError('Could not preserve artwork backup.');
          }
        } on FormatException {
          if (!preferences.containsKey(rejectedKey) &&
              !await preferences.setString(rejectedKey, current)) {
            throw StateError('Could not preserve unreadable artwork.');
          }
        }
      }
      if (!await preferences.setString(currentKey, encoded)) {
        throw StateError('Could not save artwork.');
      }
    });
  }

  Future<void> _serialize(Future<void> Function() action) {
    final completer = Completer<void>();
    _tail = _tail.then((_) async {
      try {
        await action();
        completer.complete();
      } catch (error, stack) {
        completer.completeError(error, stack);
      }
    });
    return completer.future;
  }
}

void _checkAllowance(int previous, int proposed, int limit) {
  if (proposed > limit && proposed > previous) {
    throw LitchcraftStorageLimitExceeded(
      limitBytes: limit,
      requiredBytes: proposed,
    );
  }
}

int _stringBytes(Set<String> values) =>
    values.fold(0, (sum, value) => sum + utf8.encode(value).length);

void _validateLibraryMetadata(LitchcraftLibrarySnapshot snapshot) {
  if (snapshot.recentColors.length > 8 ||
      snapshot.recentColors.any((color) => color < 0 || color > 0xffffff)) {
    throw const FormatException('Invalid artwork library colors.');
  }
  if (snapshot.artworks.map((art) => art.id).toSet().length !=
      snapshot.artworks.length) {
    throw const FormatException('Duplicate artwork identities.');
  }
}

LitchcraftLibrarySnapshot _decodeLegacyLibrary(
  String source, {
  bool recovered = false,
  bool rejected = false,
}) {
  final raw = jsonDecode(source);
  if (raw is! Map<String, dynamic> ||
      raw['version'] != 1 ||
      raw['artworks'] is! List ||
      raw['recentColors'] is! List ||
      raw['dismissedWelcome'] is! bool) {
    throw const FormatException('Unsupported artwork library.');
  }
  final colors = raw['recentColors'] as List;
  if (colors.length > 8 ||
      colors.any((value) => value is! int || value < 0 || value > 0xffffff)) {
    throw const FormatException('Invalid artwork library colors.');
  }
  final snapshot = LitchcraftLibrarySnapshot(
    artworks: List.unmodifiable(
      (raw['artworks'] as List).map(LitchcraftArtwork.fromJson),
    ),
    draft: raw['draft'] == null
        ? null
        : LitchcraftArtwork.fromJson(raw['draft']),
    draftOrigin: _decodeDraftOrigin(raw['draftOrigin']),
    previousDraft: raw['previousDraft'] == null
        ? null
        : LitchcraftArtwork.fromJson(raw['previousDraft']),
    recentColors: List.unmodifiable(colors.cast<int>()),
    dismissedWelcome: raw['dismissedWelcome'] as bool,
    recovered: recovered,
    rejected: rejected,
  );
  _validateLibraryMetadata(snapshot);
  return snapshot;
}
