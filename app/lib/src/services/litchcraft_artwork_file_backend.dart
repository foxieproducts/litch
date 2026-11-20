part of 'litchcraft_artwork_store.dart';

class _ArtworkFileEntry {
  _ArtworkFileEntry({
    required this.blob,
    required this.bytes,
    required this.sourceId,
    required this.art,
  });

  final String blob;
  final int bytes;
  final String sourceId;
  final LitchcraftArtwork art;

  Map<String, Object?> toJson() => <String, Object?>{
    'blob': blob,
    'bytes': bytes,
    'sourceId': sourceId,
    'id': art.id,
    'name': art.name,
    'createdAt': art.createdAt.toIso8601String(),
    'modifiedAt': art.modifiedAt.toIso8601String(),
    'frameCount': art.frameCount,
    'unavailableReason': art.unavailableReason?.name,
    'topology': art.isSupported ? art.canvas.topology.name : null,
    'preview': art.isSupported ? art.canvas.colors : null,
    'favoriteColors': art.isSupported ? art.favoriteColors : <int>[],
  };
}

class _ArtworkFileReference {
  _ArtworkFileReference(this.owner);
  final _LitchcraftArtworkFiles owner;
  late final _ArtworkFileEntry entry;
}

class _ArtworkCatalog {
  _ArtworkCatalog(this.snapshot, this.entries);
  final LitchcraftLibrarySnapshot snapshot;
  final List<_ArtworkFileEntry> entries;

  Map<String, Object?> toJson() {
    var index = 0;
    return <String, Object?>{
      'version': 1,
      'artworks': <Map<String, Object?>>[
        for (final _ in snapshot.artworks) entries[index++].toJson(),
      ],
      'draft': snapshot.draft == null ? null : entries[index++].toJson(),
      if (snapshot.draftOrigin != null)
        'draftOrigin': snapshot.draftOrigin!.name,
      'previousDraft': snapshot.previousDraft == null
          ? null
          : entries[index++].toJson(),
      'recentColors': snapshot.recentColors,
      'dismissedWelcome': snapshot.dismissedWelcome,
    };
  }
}

/// Immutable bounded payloads plus current/previous catalog references. All
/// mutations are serialized by LitchcraftArtworkStore, with catalog publication
/// last. Recovery catalogs share unchanged files instead of duplicating libraries.
class _LitchcraftArtworkFiles {
  _LitchcraftArtworkFiles(this.directory);
  final Directory directory;
  Directory get _artworkDirectory => Directory('${directory.path}/artworks');
  File _index(String role) => File('${directory.path}/$role.json');
  File _blob(String digest) => File('${_artworkDirectory.path}/$digest.json');
  final Map<String, _ArtworkCatalog?> _catalogs = <String, _ArtworkCatalog?>{};
  final Map<String, List<int>> _invalidIndexes = <String, List<int>>{};
  final Set<String> _unreadableIndexes = <String>{};
  final Map<String, int> _rejectedSources = <String, int>{};
  final Expando<LitchcraftArtwork> _compacted = Expando<LitchcraftArtwork>();
  final Map<String, int> _unknownPayloads = <String, int>{};
  final Map<String, int> _payloadInventory = <String, int>{};
  final Set<String> _pendingCleanup = <String>{};
  bool _recoveryManifestDirty = false;
  bool _legacyCurrentRejected = false;
  int payloadReads = 0;
  int payloadWrites = 0;

  Future<void> initialize(SharedPreferences preferences) async {
    await _artworkDirectory.create(recursive: true);
    final marker = File('${directory.path}/migration.json');
    if (!await marker.exists()) {
      await _migrate(preferences);
      await _atomicWrite(
        marker,
        jsonEncode(<String, Object?>{
          'version': 1,
          'legacyCurrentRejected': _legacyCurrentRejected,
        }),
      );
    } else {
      try {
        final raw = jsonDecode(await marker.readAsString());
        _legacyCurrentRejected =
            raw is Map && raw['legacyCurrentRejected'] == true;
      } on FormatException {
        // Original preferences remain available even if the migration marker is
        // damaged. Existing file catalogs are independently validated below.
      }
    }
    for (final role in <String>['current', 'backup', 'rejected']) {
      final file = _index(role);
      if (!await file.exists()) {
        _catalogs[role] = null;
        continue;
      }
      List<int>? source;
      try {
        source = await file.readAsBytes();
        _catalogs[role] = await _decodeCatalog(utf8.decode(source));
      } on FormatException {
        _invalidIndexes[role] = source!;
        _catalogs[role] = null;
      } on FileSystemException {
        if (source != null) {
          _invalidIndexes[role] = source;
        } else {
          _unreadableIndexes.add(role);
        }
        _catalogs[role] = null;
      }
    }
    var historicalIndexSource = false;
    await for (final file in directory.list()) {
      if (file is File &&
          RegExp(
            r'/(?:rejected-source|legacy-rejected)-[0-9a-f]{64}\.json$',
          ).hasMatch(file.path)) {
        final name = file.uri.pathSegments.last;
        final digest = name.substring(name.length - 69, name.length - 5);
        _rejectedSources[digest] = await file.length();
        historicalIndexSource |= name.startsWith('rejected-source-');
      }
    }
    // Inventory file sizes, never decoded artwork. Process termination can leave
    // payloads with no catalog reference even when every catalog is healthy.
    await for (final file in _artworkDirectory.list()) {
      if (file is! File) continue;
      final name = file.uri.pathSegments.last;
      if (RegExp(r'^[0-9a-f]{64}\.json$').hasMatch(name)) {
        _payloadInventory[name.substring(0, 64)] = await file.length();
      } else if (RegExp(r'^[0-9a-f]{64}\.json\.tmp$').hasMatch(name)) {
        // Catalogs reference only final immutable filenames. This store owns
        // the writer, so an initialization-time temporary is unpublished.
        await file.delete();
      }
    }
    final manifestExists = await _readRecoveryManifest();
    if (_invalidIndexes.isNotEmpty ||
        _unreadableIndexes.isNotEmpty ||
        _catalogs['current'] == null && _payloadInventory.isNotEmpty ||
        historicalIndexSource && !manifestExists) {
      // Freeze potentially owned blobs at this recovery incident. Historical
      // rejected bytes must never pin every later successful artwork on restart.
      _unknownPayloads.addAll(_payloadInventory);
      _recoveryManifestDirty = true;
      try {
        await _writeRecoveryManifest();
      } on FileSystemException {
        // Keep recovery readable. A later mutation must publish this ownership
        // before it can replace the damaged catalog.
      }
    }
    await _collectUnreferenced(_payloadInventory.keys.toSet());
  }

  Future<bool> _readRecoveryManifest() async {
    final file = File('${directory.path}/recovery-payloads.json');
    if (!await file.exists()) return false;
    try {
      final raw = jsonDecode(await file.readAsString());
      if (raw is! Map || raw['version'] != 1 || raw['payloads'] is! Map) {
        throw const FormatException('Invalid recovery payload ownership.');
      }
      final entries = raw['payloads'] as Map;
      for (final entry in entries.entries) {
        if (entry.key is! String ||
            !RegExp(r'^[0-9a-f]{64}$').hasMatch(entry.key as String) ||
            entry.value is! int ||
            (entry.value as int) < 0) {
          throw const FormatException('Invalid recovery payload ownership.');
        }
      }
      _unknownPayloads.addAll(entries.cast<String, int>());
    } on FormatException {
      _unknownPayloads.addAll(_payloadInventory);
      _recoveryManifestDirty = true;
    } on FileSystemException {
      _unknownPayloads.addAll(_payloadInventory);
      _recoveryManifestDirty = true;
    }
    return true;
  }

  Future<void> _writeRecoveryManifest() async {
    if (!_recoveryManifestDirty) return;
    await _atomicWrite(
      File('${directory.path}/recovery-payloads.json'),
      jsonEncode({'version': 1, 'payloads': _unknownPayloads}),
    );
    _recoveryManifestDirty = false;
  }

  Future<void> _migrate(SharedPreferences preferences) async {
    // Decode one envelope at a time; never retain a fully decoded legacy
    // animation library. Original current/backup/rejected preferences remain.
    for (final role in <String>['current', 'backup', 'rejected']) {
      final key = switch (role) {
        'current' => LitchcraftArtworkStore.currentKey,
        'backup' => LitchcraftArtworkStore.backupKey,
        _ => LitchcraftArtworkStore.rejectedKey,
      };
      final source = preferences.getString(key);
      if (source == null) continue;
      try {
        final raw = jsonDecode(source);
        if (raw is! Map<String, dynamic> ||
            raw['version'] != 1 ||
            raw['artworks'] is! List ||
            raw['recentColors'] is! List ||
            raw['dismissedWelcome'] is! bool) {
          throw const FormatException('Unsupported legacy artwork library.');
        }
        final entries = <_ArtworkFileEntry>[];
        Future<LitchcraftArtwork> migrate(Object? value) async {
          final art = LitchcraftArtwork.fromJson(value);
          final prepared = _prepare(art);
          await _writePayload(prepared.entry, prepared.payload!);
          final entry = _deferredEntry(prepared.entry);
          entries.add(entry);
          return entry.art;
        }

        final arts = <LitchcraftArtwork>[];
        for (final value in raw['artworks'] as List) {
          arts.add(await migrate(value));
        }
        final draft = raw['draft'] == null ? null : await migrate(raw['draft']);
        final previous = raw['previousDraft'] == null
            ? null
            : await migrate(raw['previousDraft']);
        final colors = raw['recentColors'] as List;
        if (colors.any((value) => value is! int)) {
          throw const FormatException('Invalid legacy artwork colors.');
        }
        final snapshot = LitchcraftLibrarySnapshot(
          artworks: arts,
          draft: draft,
          draftOrigin: _decodeDraftOrigin(raw['draftOrigin']),
          previousDraft: previous,
          recentColors: colors.cast<int>(),
          dismissedWelcome: raw['dismissedWelcome'] as bool,
        );
        _validateLibraryMetadata(snapshot);
        final catalog = _ArtworkCatalog(snapshot, entries);
        final encoded = jsonEncode(catalog.toJson());
        await _decodeCatalog(encoded);
        await _atomicWrite(_index(role), encoded);
      } on FormatException {
        await _preserveSource(source, legacy: true);
        if (role == 'current') _legacyCurrentRejected = true;
      }
    }
  }

  Future<LitchcraftLibrarySnapshot> load() async {
    final current = _catalogs['current'];
    if (current != null) return current.snapshot;
    final backup = _catalogs['backup'];
    if (backup != null) {
      final snapshot = backup.snapshot;
      return LitchcraftLibrarySnapshot(
        artworks: snapshot.artworks,
        draft: snapshot.draft,
        draftOrigin: snapshot.draftOrigin,
        previousDraft: snapshot.previousDraft,
        recentColors: snapshot.recentColors,
        dismissedWelcome: snapshot.dismissedWelcome,
        recovered: true,
        rejected:
            _invalidIndexes.containsKey('current') ||
            _unreadableIndexes.contains('current') ||
            _legacyCurrentRejected,
      );
    }
    return LitchcraftLibrarySnapshot(
      rejected:
          _invalidIndexes.isNotEmpty ||
          _unreadableIndexes.isNotEmpty ||
          _legacyCurrentRejected,
    );
  }

  LitchcraftArtwork compactArtwork(LitchcraftArtwork art) =>
      _compacted[art] ?? art;

  Future<void> save(LitchcraftLibrarySnapshot snapshot, int limit) async {
    await _collectUnreferenced(_pendingCleanup.toSet());
    final prepared = <_PreparedArtwork>[];
    for (final art in <LitchcraftArtwork>[
      ...snapshot.artworks,
      ?snapshot.draft,
      ?snapshot.previousDraft,
    ]) {
      prepared.add(_prepare(art));
    }
    final next = _ArtworkCatalog(
      snapshot,
      prepared.map((value) => value.entry).toList(),
    );
    final prior = _catalogs['current'] ?? _catalogs['backup'];
    final retained = <_ArtworkCatalog?>[..._catalogs.values];
    final proposed = <_ArtworkCatalog?>[next, prior, _catalogs['rejected']];
    final sourceBytes = <String, int>{..._rejectedSources};
    for (final source in _invalidIndexes.values) {
      sourceBytes[sha256.convert(source).toString()] = source.length;
    }
    final rejectedBytes = sourceBytes.values.fold<int>(
      0,
      (sum, bytes) => sum + bytes,
    );
    final oldPayloads = _catalogPayloads(retained);
    // After cleanup failure, another accepted replacement cannot assume its
    // displaced backup will be reclaimable. Admit against actual retained files.
    final orphans = _pendingCleanup.isEmpty
        ? _payloadInventory.keys.toSet().difference(oldPayloads)
        : _payloadInventory.keys.toSet();
    _checkAllowance(
      _payloadBytes(retained, orphans: orphans) + rejectedBytes,
      _payloadBytes(proposed, orphans: orphans) + rejectedBytes,
      limit,
    );
    final written = <String>{};
    try {
      await _writeRecoveryManifest();
      for (final value in prepared) {
        if (value.payload case final payload?) {
          if (await _writePayload(value.entry, payload)) {
            written.add(value.entry.blob);
          }
        }
      }
      // Preserve unreadable catalogs before replacing their paths. A failed
      // preservation or backup write leaves the published current untouched.
      for (final source in _invalidIndexes.values) {
        await _preserveBytes(source);
      }
      for (final role in _unreadableIndexes) {
        // If reading is still unavailable, fail without replacing the source.
        await _preserveBytes(await _index(role).readAsBytes());
      }
      if (_catalogs['current'] case final current?) {
        await _atomicWrite(_index('backup'), jsonEncode(current.toJson()));
        _catalogs['backup'] = current;
        _invalidIndexes.remove('backup');
        _unreadableIndexes.remove('backup');
      }
      await _atomicWrite(_index('current'), jsonEncode(next.toJson()));
      final compactEntries = prepared
          .map((value) => _deferredEntry(value.entry))
          .toList();
      var index = 0;
      final compactSnapshot = LitchcraftLibrarySnapshot(
        artworks: <LitchcraftArtwork>[
          for (final art in snapshot.artworks)
            _remember(art, compactEntries[index++].art),
        ],
        draft: snapshot.draft == null
            ? null
            : _remember(snapshot.draft!, compactEntries[index++].art),
        draftOrigin: snapshot.draftOrigin,
        previousDraft: snapshot.previousDraft == null
            ? null
            : _remember(snapshot.previousDraft!, compactEntries[index++].art),
        recentColors: List.unmodifiable(snapshot.recentColors),
        dismissedWelcome: snapshot.dismissedWelcome,
      );
      _catalogs['backup'] = prior;
      _catalogs['current'] = _ArtworkCatalog(compactSnapshot, compactEntries);
      _invalidIndexes.remove('current');
      _unreadableIndexes.remove('current');
      _legacyCurrentRejected = false;
      await _collectUnreferenced(oldPayloads);
    } catch (_) {
      await _collectUnreferenced({...written, ...oldPayloads});
      rethrow;
    }
  }

  LitchcraftArtwork _remember(
    LitchcraftArtwork source,
    LitchcraftArtwork compact,
  ) {
    _compacted[source] = compact;
    return compact;
  }

  _PreparedArtwork _prepare(LitchcraftArtwork art) {
    final reference = art.storageReference;
    if (art.isDeferred &&
        reference is _ArtworkFileReference &&
        identical(reference.owner, this)) {
      final source = reference.entry;
      if (art.frameCount != source.art.frameCount ||
          art.unavailableReason != source.art.unavailableReason ||
          art.isSupported &&
              (!sameLitchcraftArtwork(art.canvas, source.art.canvas) ||
                  art.canvas.topology != source.art.canvas.topology ||
                  !_sameInts(art.favoriteColors, source.art.favoriteColors))) {
        throw const FormatException(
          'Stored artwork preview changed without resolving its payload.',
        );
      }
      return _PreparedArtwork(
        _ArtworkFileEntry(
          blob: source.blob,
          bytes: source.bytes,
          sourceId: source.sourceId,
          art: art,
        ),
      );
    }
    final payload = utf8.encode(jsonEncode(art.toJson()));
    if (payload.length > litchcraftArtworkMaximumEnvelopeBytes) {
      throw const FormatException('Artwork exceeds storage bound.');
    }
    // Validate the production ingress even when callers constructed a document
    // directly; unknown/corrupt advanced envelopes retain their exact source.
    LitchcraftArtwork.fromJson(jsonDecode(utf8.decode(payload)));
    return _PreparedArtwork(
      _ArtworkFileEntry(
        blob: sha256.convert(payload).toString(),
        bytes: payload.length,
        sourceId: art.id,
        art: art,
      ),
      payload,
    );
  }

  _ArtworkFileEntry _deferredEntry(_ArtworkFileEntry source) {
    final art = source.art;
    late final _ArtworkFileEntry entry;
    final reference = _ArtworkFileReference(this);
    entry = _ArtworkFileEntry(
      blob: source.blob,
      bytes: source.bytes,
      sourceId: source.sourceId,
      art: LitchcraftArtwork.deferred(
        id: art.id,
        name: art.name,
        createdAt: art.createdAt,
        modifiedAt: art.modifiedAt,
        frameCount: art.frameCount,
        preview: art.isSupported ? art.canvas : null,
        favoriteColors: art.isSupported ? art.favoriteColors : <int>[],
        unavailableReason: art.unavailableReason,
        storageReference: reference,
        resolver: () => _resolve(entry),
      ),
    );
    reference.entry = entry;
    return entry;
  }

  Future<LitchcraftArtwork> _resolve(_ArtworkFileEntry entry) async {
    final payload = await _readPayload(entry);
    final art = LitchcraftArtwork.fromJson(jsonDecode(utf8.decode(payload)));
    if (art.id != entry.sourceId ||
        art.unavailableReason != entry.art.unavailableReason ||
        art.frameCount != entry.art.frameCount ||
        art.isSupported &&
            (!sameLitchcraftArtwork(art.canvas, entry.art.canvas) ||
                art.canvas.topology != entry.art.canvas.topology ||
                !_sameInts(art.favoriteColors, entry.art.favoriteColors))) {
      throw const FormatException('Stored artwork does not match its catalog.');
    }
    return art;
  }

  Future<List<int>> _readPayload(_ArtworkFileEntry entry) async {
    final file = _blob(entry.blob);
    if (await file.length() != entry.bytes) {
      throw const FormatException('Stored artwork size changed.');
    }
    payloadReads++;
    final bytes = await file.readAsBytes();
    if (bytes.length != entry.bytes ||
        sha256.convert(bytes).toString() != entry.blob) {
      throw const FormatException('Stored artwork checksum changed.');
    }
    return bytes;
  }

  Future<bool> _writePayload(_ArtworkFileEntry entry, List<int> bytes) async {
    final file = _blob(entry.blob);
    if (await file.exists()) {
      await _readPayload(entry);
      _payloadInventory[entry.blob] = entry.bytes;
      return false;
    }
    final temporary = File('${file.path}.tmp');
    try {
      payloadWrites++;
      await temporary.writeAsBytes(bytes, flush: true);
      if (await temporary.length() != bytes.length) {
        throw const FileSystemException('Incomplete artwork write.');
      }
      await temporary.rename(file.path);
      _payloadInventory[entry.blob] = bytes.length;
      return true;
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Future<_ArtworkCatalog> _decodeCatalog(String source) async {
    final raw = jsonDecode(source);
    if (raw is! Map<String, dynamic> ||
        raw['version'] != 1 ||
        raw['artworks'] is! List ||
        raw['recentColors'] is! List ||
        raw['dismissedWelcome'] is! bool) {
      throw const FormatException('Unsupported artwork catalog.');
    }
    final entries = <_ArtworkFileEntry>[];
    Future<LitchcraftArtwork> read(Object? value) async {
      final entry = _decodeEntry(value);
      entries.add(entry);
      return entry.art;
    }

    final arts = <LitchcraftArtwork>[];
    for (final value in raw['artworks'] as List) {
      arts.add(await read(value));
    }
    final draft = raw['draft'] == null ? null : await read(raw['draft']);
    final previous = raw['previousDraft'] == null
        ? null
        : await read(raw['previousDraft']);
    final colors = raw['recentColors'] as List;
    if (colors.any((value) => value is! int)) {
      throw const FormatException('Invalid artwork catalog colors.');
    }
    final snapshot = LitchcraftLibrarySnapshot(
      artworks: List.unmodifiable(arts),
      draft: draft,
      draftOrigin: _decodeDraftOrigin(raw['draftOrigin']),
      previousDraft: previous,
      recentColors: List.unmodifiable(colors.cast<int>()),
      dismissedWelcome: raw['dismissedWelcome'] as bool,
    );
    _validateLibraryMetadata(snapshot);
    return _ArtworkCatalog(snapshot, entries);
  }

  _ArtworkFileEntry _decodeEntry(Object? value) {
    if (value is! Map<String, dynamic> ||
        value['blob'] is! String ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(value['blob'] as String) ||
        value['bytes'] is! int ||
        (value['bytes'] as int) <= 0 ||
        (value['bytes'] as int) > litchcraftArtworkMaximumEnvelopeBytes ||
        value['sourceId'] is! String ||
        !RegExp(
          r'^[a-zA-Z0-9_-]{1,80}$',
        ).hasMatch(value['sourceId'] as String) ||
        value['id'] is! String ||
        value['name'] is! String ||
        value['createdAt'] is! String ||
        value['modifiedAt'] is! String ||
        value['frameCount'] is! int ||
        value['favoriteColors'] is! List) {
      throw const FormatException('Invalid artwork catalog entry.');
    }
    final created = DateTime.tryParse(value['createdAt'] as String);
    final modified = DateTime.tryParse(value['modifiedAt'] as String);
    if (created == null || modified == null) {
      throw const FormatException('Invalid artwork catalog time.');
    }
    final reason = value['unavailableReason'];
    LitchcraftArtworkUnavailableReason? unavailable;
    if (reason != null) {
      unavailable = LitchcraftArtworkUnavailableReason.values
          .where((item) => item.name == reason)
          .firstOrNull;
      if (unavailable == null) {
        throw const FormatException('Invalid artwork availability.');
      }
    }
    LitchcraftCanvas? preview;
    if (unavailable == null) {
      final topology = LitchcraftTopology.values
          .where((item) => item.name == value['topology'])
          .firstOrNull;
      final colors = value['preview'];
      if (topology == null ||
          colors is! List ||
          colors.any((color) => color is! int)) {
        throw const FormatException('Invalid artwork thumbnail.');
      }
      preview = LitchcraftCanvas(
        colors: colors.cast<int>(),
        topology: topology,
      );
    } else if (value['preview'] != null || value['topology'] != null) {
      throw const FormatException('Invalid unavailable artwork thumbnail.');
    }
    final favorites = value['favoriteColors'] as List;
    if (favorites.any((color) => color is! int)) {
      throw const FormatException('Invalid artwork favorites.');
    }
    late final _ArtworkFileEntry entry;
    final reference = _ArtworkFileReference(this);
    entry = _ArtworkFileEntry(
      blob: value['blob'] as String,
      bytes: value['bytes'] as int,
      sourceId: value['sourceId'] as String,
      art: LitchcraftArtwork.deferred(
        id: value['id'] as String,
        name: value['name'] as String,
        createdAt: created,
        modifiedAt: modified,
        frameCount: value['frameCount'] as int,
        preview: preview,
        favoriteColors: favorites.cast<int>(),
        unavailableReason: unavailable,
        storageReference: reference,
        resolver: () => _resolve(entry),
      ),
    );
    reference.entry = entry;
    return entry;
  }

  Future<void> _preserveSource(String source, {bool legacy = false}) =>
      _preserveBytes(utf8.encode(source), legacy: legacy);

  Future<void> _preserveBytes(List<int> bytes, {bool legacy = false}) async {
    final digest = sha256.convert(bytes).toString();
    final file = File(
      '${directory.path}/${legacy ? 'legacy-rejected' : 'rejected-source'}-$digest.json',
    );
    if (!await file.exists()) await _atomicWriteBytes(file, bytes);
    _rejectedSources[digest] = bytes.length;
  }

  Future<void> _atomicWrite(File target, String source) =>
      _atomicWriteBytes(target, utf8.encode(source));

  Future<void> _atomicWriteBytes(File target, List<int> bytes) async {
    final temporary = File('${target.path}.tmp');
    try {
      await temporary.writeAsBytes(bytes, flush: true);
      if (await temporary.length() != bytes.length) {
        throw const FileSystemException('Incomplete artwork catalog write.');
      }
      await temporary.rename(target.path);
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Set<String> _catalogPayloads(Iterable<_ArtworkCatalog?> catalogs) => {
    for (final catalog in catalogs)
      if (catalog != null)
        for (final entry in catalog.entries) entry.blob,
  };

  int _payloadBytes(
    Iterable<_ArtworkCatalog?> catalogs, {
    Set<String> orphans = const {},
  }) {
    final bytes = <String, int>{};
    for (final digest in {..._unknownPayloads.keys, ...orphans}) {
      final size = _payloadInventory[digest];
      if (size != null) bytes[digest] = size;
    }
    for (final catalog in catalogs) {
      if (catalog == null) continue;
      for (final entry in catalog.entries) {
        bytes[entry.blob] = entry.bytes;
      }
    }
    return bytes.values.fold(0, (sum, value) => sum + value);
  }

  Future<void> _collectUnreferenced(Set<String> candidates) async {
    // An unreadable recovery catalog may reference files we cannot safely
    // identify. Preserve its payloads until recovery explicitly replaces it.
    final retained = <String>{
      for (final catalog in _catalogs.values)
        if (catalog != null)
          for (final entry in catalog.entries) entry.blob,
    };
    for (final digest
        in candidates
            .difference(retained)
            .difference(_unknownPayloads.keys.toSet())) {
      final file = _blob(digest);
      try {
        if (await file.exists()) await file.delete();
        _payloadInventory.remove(digest);
        _pendingCleanup.remove(digest);
      } on FileSystemException {
        // Retain the actual inventoried size in subsequent allowance checks;
        // failed cleanup is garbage to retry, not permanent recovery ownership.
        _pendingCleanup.add(digest);
        // Cleanup is optional after a durable publication. Retrying on the next
        // mutation must never turn an accepted save into a reported failure.
      }
    }
  }
}

class _PreparedArtwork {
  _PreparedArtwork(this.entry, [this.payload]);
  final _ArtworkFileEntry entry;
  final List<int>? payload;
}

bool _sameInts(List<int> left, List<int> right) =>
    left.length == right.length &&
    Iterable<int>.generate(
      left.length,
    ).every((index) => left[index] == right[index]);
