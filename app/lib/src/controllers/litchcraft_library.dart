import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../models/litchcraft_artwork.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_document_session.dart';
import '../models/litchcraft_rickroll_preset.dart';
import '../models/litchcraft_bouncing_balls_preset.dart';
import '../models/litchcraft_surprise_presets.dart';
export '../models/litchcraft_document_session.dart' show LitchcraftFrameLimit;
import '../models/litchcraft_state.dart';
import '../services/app_trace_recorder.dart';
import '../services/litchcraft_artwork_store.dart';
import 'litchcraft_controller.dart';

List<int> _uniqueFavoriteColors(Iterable<int> colors) {
  final unique = <int>{};
  for (final color in colors) {
    if (color == 0) continue;
    unique.add(color);
    if (unique.length == 8) break;
  }
  return unique.toList(growable: false);
}

bool _sameArtworkContent(
  LitchcraftAnimationDocument left,
  LitchcraftAnimationDocument right,
) =>
    identical(left, right) ||
    sameLitchcraftDocument(
      LitchcraftAnimationDocument(
        frames: left.frames,
        topology: left.topology,
        favoriteColors: right.favoriteColors,
        playbackOnEntry: left.playbackOnEntry,
        sparkleSeed: left.sparkleSeed,
      ),
      right,
    );

enum LitchcraftOpenSource { library, imported }

/// Phone-owned editable library and recoverable draft. Live ordering stays in
/// LitchcraftController; explicit saves always ask it for a coherent revision.
class LitchcraftLibrary extends ChangeNotifier {
  LitchcraftLibrary(
    this.controller, {
    LitchcraftArtworkStore? store,
    Future<LitchcraftArtworkStore> Function()? storeLoader,
    AppTraceSink? appTrace,
  }) : _appTrace = appTrace,
       _store = store,
       _storeLoader =
           storeLoader ??
           (() async =>
               LitchcraftArtworkStore(await SharedPreferences.getInstance())) {
    _appTrace?.record('litchcraft.library_load_start');
    _observedDocument = session.document;
    _observedReplacementGeneration = session.replacementGeneration;
    ready = _load();
    controller.addListener(_canvasChanged);
    session.addListener(_documentChanged);
  }
  final LitchcraftController controller;
  final AppTraceSink? _appTrace;
  late final Future<void> ready;
  LitchcraftArtworkStore? _store;
  final Future<LitchcraftArtworkStore> Function() _storeLoader;
  Future<void>? _reopening;
  bool _catalogLoaded = false;
  bool _draftTouched = false;
  bool _artworkEdited = false;
  bool _favoritesEdited = false;
  bool _restoringDraft = false;
  late int _observedReplacementGeneration;
  Timer? _draftTimer;
  Future<void>? _flushing;
  bool _flushRequested = false;
  int _saveRetryAttempt = 0;
  LitchcraftArtwork? _cachedDraft;
  LitchcraftDraftOrigin _draftOrigin = LitchcraftDraftOrigin.anonymous;
  LitchcraftLibrarySnapshot? _lastSaved;
  bool _closed = false;
  bool _loaded = false;
  bool _canvasGestureActive = false;
  bool _dismissWelcomeAfterGesture = false;
  int _identitySequence = 0;
  LitchcraftCanvas? _observed;
  LitchcraftAnimationDocument? _observedDocument;
  LitchcraftArtwork? _active;
  LitchcraftArtwork? _unavailableDraft;
  bool _mirrorRecoveryState = false;
  LitchcraftDocumentSession get session => controller.documentSession;
  int get selectedFrameIndex => session.selectedFrameIndex;
  LitchcraftArtwork? previousDraft;
  List<LitchcraftArtwork> artworks = <LitchcraftArtwork>[];
  List<int> get favoriteColors => session.document.favoriteColors;
  set favoriteColors(List<int> colors) => session.setFavoriteColors(colors);
  bool dismissedWelcome = false;
  bool recovered = false;
  bool rejected = false;
  bool saveFailed = false;
  Object? lastSaveError;
  bool get storageLimitExceeded =>
      lastSaveError is LitchcraftStorageLimitExceeded;

  bool get dirty =>
      _active == null ||
      !_active!.isSupported ||
      !sameLitchcraftDocument(_active!.document, session.document);
  String? get activeName => _draftOrigin == LitchcraftDraftOrigin.anonymous
      ? null
      : (_active ?? _cachedDraft)?.name;

  /// Capture metadata and the current immutable document before a Share sheet.
  LitchcraftArtwork currentArtworkForSharing(String defaultName) {
    final draft = _draft();
    return _draftOrigin == LitchcraftDraftOrigin.anonymous
        ? draft.copyWith(name: defaultName)
        : draft;
  }

  bool get adapted =>
      _active != null &&
      _active!.isSupported &&
      _active!.canvas.topology != controller.canvas.topology;
  bool get canEditAnimation =>
      controller.usesFrameStoreDocument ||
      controller.status != LitchcraftStatus.live;
  bool get canUndoAnimation => session.canUndo;
  bool get canRedoAnimation => session.canRedo;
  LitchcraftAnimationDocument get animationDocument => session.document;
  LitchcraftAnimationFrame get selectedFrame =>
      animationDocument.frames[selectedFrameIndex];

  bool get _hasDraftIntent =>
      _draftTouched || _artworkEdited || controller.hasMarks;

  Future<void> _load({bool retry = false}) async {
    try {
      _store ??= await _storeLoader();
      final snapshot = await _store!.load();
      if (_closed) return;
      var preserveCurrentDraft = _hasDraftIntent;
      artworks = snapshot.artworks.toList();
      // Only an explicit edit owns the visible canvas after a failed open.
      // A lifecycle flush must restore an untouched durable draft, not publish
      // the temporary blank canvas over it and its rolling recovery slot.
      previousDraft = preserveCurrentDraft
          ? snapshot.draft ?? snapshot.previousDraft
          : snapshot.previousDraft;
      _catalogLoaded = true;
      final storedOrigin =
          snapshot.draftOrigin ??
          (artworks.any((art) => art.id == snapshot.draft?.id)
              ? LitchcraftDraftOrigin.saved
              : LitchcraftDraftOrigin.anonymous);
      final storedRecentColors = {
        ...snapshot.recentColors,
        ...litchcraftDefaultFavoriteColors,
      }.take(8).toList();
      dismissedWelcome = snapshot.dismissedWelcome || dismissedWelcome;
      recovered = snapshot.recovered;
      rejected = snapshot.rejected;
      if (preserveCurrentDraft) {
        rejected |= snapshot.draft?.isSupported == false;
      } else if (snapshot.draft != null && !snapshot.draft!.isSupported) {
        _unavailableDraft = snapshot.draft;
        _draftOrigin = storedOrigin;
        rejected = true;
        // Palette-only intent may keep an unreadable current source. Real
        // artwork edits during startup must instead own the current draft.
        if (_hasDraftIntent) {
          _retireUnavailableDraft();
        }
      } else if (snapshot.draft != null) {
        final LitchcraftArtwork draft;
        try {
          draft = await snapshot.draft!.resolve();
        } catch (_) {
          // Lifecycle flushes still run after a failed read. Keep its catalog
          // reference authoritative until it can load or the user replaces it.
          _unavailableDraft = snapshot.draft;
          _draftOrigin = storedOrigin;
          rejected = true;
          if (_hasDraftIntent) {
            _retireUnavailableDraft();
          }
          rethrow;
        }
        final saved = artworks.where((art) => art.id == draft.id).firstOrNull;
        final origin = storedOrigin;
        LitchcraftArtwork? baseline;
        if (origin == LitchcraftDraftOrigin.saved && saved != null) {
          try {
            baseline = await saved.resolve();
          } catch (_) {
            // A damaged saved copy cannot veto a healthy working draft. Without
            // its baseline the draft remains dirty and Save creates a new copy.
            rejected = true;
          }
        }
        if (_closed) return;
        preserveCurrentDraft = _hasDraftIntent;
        if (preserveCurrentDraft) {
          previousDraft = snapshot.draft;
        } else {
          final colors = _favoritesEdited
              ? List<int>.of(favoriteColors)
              : List<int>.of(draft.favoriteColors);
          _restoringDraft = true;
          try {
            controller.restoreDocument(draft.document);
            favoriteColors = colors;
          } finally {
            _restoringDraft = false;
          }
          _draftOrigin = origin;
          _active = baseline;
          _cachedDraft = draft;
        }
      }
      if (!preserveCurrentDraft &&
          (snapshot.draft == null || !snapshot.draft!.isSupported) &&
          !_closed &&
          !_favoritesEdited) {
        _restoringDraft = true;
        try {
          favoriteColors = storedRecentColors;
        } finally {
          _restoringDraft = false;
        }
      }
      controller.setFavoriteColors(favoriteColors);
      saveFailed = false;
      lastSaveError = null;
      if (retry &&
          !preserveCurrentDraft &&
          !_favoritesEdited &&
          snapshot.dismissedWelcome == dismissedWelcome) {
        // The recovered catalog already contains this exact state. In
        // particular, pausing an untouched app must not rotate its backup.
        _lastSaved = _snapshot();
      }
      _appTrace?.record(
        'litchcraft.library_load_complete',
        fields: <String, Object?>{
          'artworks': artworks.length,
          'has_draft': snapshot.draft != null,
          'recovered': recovered,
          'rejected': rejected,
        },
      );
    } catch (error) {
      saveFailed = true;
      lastSaveError = error;
      _appTrace?.record(
        'litchcraft.library_load_failed',
        fields: <String, Object?>{'kind': error.runtimeType.toString()},
      );
    }
    _loaded = true;
    _observed = controller.canvas;
    _observedDocument = session.document;
    if (!_closed) notifyListeners();
  }

  Future<void> _ensureLibraryLoaded() async {
    await ready;
    final pending = _reopening;
    if (pending != null) {
      await pending;
    } else if (!_catalogLoaded) {
      await (_reopening = _load(
        retry: true,
      ).whenComplete(() => _reopening = null));
    }
    if (!_catalogLoaded) {
      throw lastSaveError ?? StateError('Artwork storage is unavailable.');
    }
  }

  void _canvasChanged() {
    if (_closed || !_loaded || _restoringDraft) {
      return;
    }
    final changed = !sameLitchcraftArtwork(
      _observed ?? controller.canvas,
      controller.canvas,
    );
    if (!changed) return;
    _retireUnavailableDraft();
    _observed = controller.canvas;

    if (controller.hasMarks && !dismissedWelcome) {
      if (_canvasGestureActive) {
        _dismissWelcomeAfterGesture = true;
      } else {
        dismissedWelcome = true;
      }
    }
    _scheduleDraftFlush();
    notifyListeners();
  }

  void _documentChanged() {
    final previous = _observedDocument;
    _observedDocument = session.document;
    if (!_restoringDraft &&
        (!_loaded || !_catalogLoaded || _reopening != null) &&
        previous != null) {
      if (!_artworkEdited &&
          (_observedReplacementGeneration != session.replacementGeneration ||
              !_sameArtworkContent(previous, session.document))) {
        _artworkEdited = true;
      }
      if (!_favoritesEdited &&
          !listEquals(
            previous.favoriteColors,
            session.document.favoriteColors,
          )) {
        _favoritesEdited = true;
      }
    }
    _observedReplacementGeneration = session.replacementGeneration;
    if (_closed || !_loaded || _restoringDraft) return;
    // Frame/timing edits can leave the selected canvas unchanged. They still
    // own the new draft; favorites and mere frame selection do not replace it.
    if (_unavailableDraft != null &&
        previous != null &&
        !_sameArtworkContent(previous, session.document)) {
      _retireUnavailableDraft();
    }
    _scheduleDraftFlush();
    notifyListeners();
  }

  void beginCanvasGesture() {
    _draftTouched = true;
    _canvasGestureActive = true;
  }

  void endCanvasGesture() {
    _canvasGestureActive = false;
    if (!_dismissWelcomeAfterGesture || dismissedWelcome) return;
    _dismissWelcomeAfterGesture = false;
    dismissedWelcome = true;
    _scheduleDraftFlush();
    notifyListeners();
  }

  void _scheduleDraftFlush() {
    _draftTimer?.cancel();
    _draftTimer = Timer(
      const Duration(milliseconds: 600),
      () => unawaited(flush()),
    );
  }

  void _retireUnavailableDraft() {
    final unavailable = _unavailableDraft;
    if (unavailable == null) return;
    previousDraft = unavailable;
    _unavailableDraft = null;
    _draftOrigin = LitchcraftDraftOrigin.anonymous;
    // The first write necessarily moves the old current snapshot into the
    // backup slot. Mirror the replacement once so a subsequent interrupted
    // current write still recovers both the new draft and the unavailable
    // original retained as previousDraft.
    _mirrorRecoveryState = true;
  }

  Future<void> dismissWelcome() async {
    dismissedWelcome = true;
    notifyListeners();
    await flush();
  }

  Future<void> rememberColor(int color) async {
    if (color == 0) return;
    final next = _uniqueFavoriteColors(<int>[
      color,
      ...favoriteColors,
      ...litchcraftDefaultFavoriteColors,
    ]);
    if (listEquals(next, favoriteColors)) return;
    favoriteColors = next;
    controller.setFavoriteColors(favoriteColors);
    notifyListeners();
    await flush();
  }

  Future<void> replaceFavoriteColor(int previous, int color) async {
    if (color == 0) return;
    final next = _uniqueFavoriteColors(<int>[
      color,
      ...favoriteColors.where((c) => c != previous),
      ...litchcraftDefaultFavoriteColors,
    ]);
    controller.setFavoriteColors(next);
    favoriteColors = next;
    notifyListeners();
    await flush();
  }

  String _newId() =>
      'art-${DateTime.now().toUtc().microsecondsSinceEpoch}-${_identitySequence++}';

  LitchcraftArtwork _draft() {
    final cached = _cachedDraft;
    final source = _active ?? cached;
    if (cached != null &&
        cached.id == (source?.id ?? 'draft') &&
        cached.name == (source?.name ?? 'Litchcraft') &&
        sameLitchcraftDocument(cached.document, session.document)) {
      return cached;
    }
    final now = DateTime.now().toUtc();
    final document = session.document;
    return _cachedDraft = LitchcraftArtwork.animated(
      id: source?.id ?? 'draft',
      name: source?.name ?? 'Litchcraft',
      document: document,
      createdAt: source?.createdAt ?? now,
      modifiedAt: _latestTimestamp(now, source?.modifiedAt, cached?.modifiedAt),
    );
  }

  DateTime _latestTimestamp(DateTime now, DateTime? source, DateTime? cached) {
    var result = now;
    for (final time in [source, cached]) {
      if (time != null && time.isAfter(result)) result = time;
    }
    return result;
  }

  Future<void> flush() {
    _draftTimer?.cancel();
    _flushRequested = true;
    return _flushing ??= _flushPending().whenComplete(() => _flushing = null);
  }

  LitchcraftLibrarySnapshot _snapshot() => LitchcraftLibrarySnapshot(
    artworks: List.of(artworks),
    draft: _unavailableDraft ?? _draft(),
    draftOrigin: _draftOrigin,
    previousDraft: previousDraft,
    recentColors: List.of(favoriteColors),
    dismissedWelcome: dismissedWelcome,
  );

  Future<void> _flushPending() async {
    await ready;
    while (_flushRequested) {
      _flushRequested = false;
      try {
        await _ensureLibraryLoaded();
      } catch (error) {
        saveFailed = true;
        lastSaveError = error;
        break;
      }
      final snapshot = _snapshot();
      final saved = _lastSaved;
      if (saved != null &&
          identical(snapshot.draft, saved.draft) &&
          snapshot.draftOrigin == saved.draftOrigin &&
          identical(snapshot.previousDraft, saved.previousDraft) &&
          listEquals(snapshot.artworks, saved.artworks) &&
          listEquals(snapshot.recentColors, saved.recentColors) &&
          snapshot.dismissedWelcome == saved.dismissedWelcome) {
        saveFailed = false;
        lastSaveError = null;
        continue;
      }
      try {
        await _store!.save(snapshot);
        if (_mirrorRecoveryState) {
          await _store!.save(snapshot);
          _mirrorRecoveryState = false;
        }
        final compactSaved = snapshot.artworks
            .map(_store!.compactArtwork)
            .toList(growable: false);
        final compactBySource = <LitchcraftArtwork, LitchcraftArtwork>{
          for (var index = 0; index < snapshot.artworks.length; index++)
            snapshot.artworks[index]: compactSaved[index],
        };
        artworks = artworks.map((art) => compactBySource[art] ?? art).toList();
        final compactPrevious = snapshot.previousDraft == null
            ? null
            : _store!.compactArtwork(snapshot.previousDraft!);
        if (identical(previousDraft, snapshot.previousDraft)) {
          previousDraft = compactPrevious;
        }
        _lastSaved = LitchcraftLibrarySnapshot(
          artworks: compactSaved,
          draft: snapshot.draft,
          draftOrigin: snapshot.draftOrigin,
          previousDraft: compactPrevious,
          recentColors: snapshot.recentColors,
          dismissedWelcome: snapshot.dismissedWelcome,
        );
        saveFailed = false;
        lastSaveError = null;
        _saveRetryAttempt = 0;
      } catch (error) {
        saveFailed = true;
        lastSaveError = error;
      }
    }
    if (saveFailed && !_closed) {
      // The existing single writer retries the latest complete draft. A
      // bounded backoff keeps transient storage failures out of the user flow.
      const delays = [500, 1500, 4000, 10000];
      final delay = delays[_saveRetryAttempt];
      if (_saveRetryAttempt < delays.length - 1) _saveRetryAttempt++;
      _draftTimer?.cancel();
      _draftTimer = Timer(
        Duration(milliseconds: delay),
        () => unawaited(flush()),
      );
    }
    if (!_closed) notifyListeners();
  }

  Future<void> save(String name, {bool saveAs = false}) async {
    await _ensureLibraryLoaded();
    await controller.coherentDocument();
    final source = _active;
    final unavailableDraft = _unavailableDraft;
    final oldPreviousDraft = previousDraft;
    final oldOrigin = _draftOrigin;
    final now = DateTime.now().toUtc();
    final draft = _draft();
    final artwork = draft.copyWith(
      id: saveAs || source == null ? _newId() : source.id,
      name: name.trim(),
      createdAt: saveAs || source == null ? now : source.createdAt,
      modifiedAt: saveAs || source == null
          ? now
          : _latestTimestamp(now, source.modifiedAt, draft.modifiedAt),
    );
    _retireUnavailableDraft();
    final previous = artworks.toList();
    final oldActive = _active;
    artworks = <LitchcraftArtwork>[
      artwork,
      ...artworks.where((art) => art.id != artwork.id),
    ];
    _active = artwork;
    _cachedDraft = artwork;
    _draftOrigin = LitchcraftDraftOrigin.saved;
    await flush();
    if (saveFailed) {
      artworks = previous;
      _active = oldActive;
      _draftOrigin = oldOrigin;
      _cachedDraft = draft;
      _unavailableDraft = unavailableDraft;
      previousDraft = oldPreviousDraft;
      _mirrorRecoveryState = false;
      throw lastSaveError ?? StateError('Artwork save failed.');
    }
    notifyListeners();
  }

  Future<void> preserveDraft() async {
    await _ensureLibraryLoaded();
    final oldPrevious = previousDraft;
    previousDraft = _unavailableDraft ?? _draft();
    await flush();
    if (saveFailed) {
      previousDraft = oldPrevious;
      throw lastSaveError ?? StateError('Could not preserve current canvas.');
    }
  }

  /// Open still returns its original delivery error. Feedback is handled only
  /// by that exact operation's pending repair or verified current settlement,
  /// after the phone draft has saved successfully.
  bool isOpenFailureHandled(Object error) =>
      !saveFailed &&
      lastSaveError == null &&
      controller.isStoreReplacementFailureHandled(error);

  Future<void> open(
    LitchcraftArtwork artwork, {
    bool Function()? admissionGuard,
    LitchcraftOpenSource source = LitchcraftOpenSource.library,
  }) async {
    final action = _appTrace.beginUserAction(SupportActionKind.artworkLoad);
    try {
      final applied = await _openArtwork(
        artwork,
        admissionGuard: admissionGuard,
        source: source,
      );
      _appTrace.finishUserAction(
        action,
        applied
            ? SupportActionOutcome.appliedOnPhone
            : SupportActionOutcome.canceled,
      );
    } catch (_) {
      _appTrace.finishUserAction(action, SupportActionOutcome.failed);
      rethrow;
    }
  }

  Future<bool> _openArtwork(
    LitchcraftArtwork artwork, {
    bool Function()? admissionGuard,
    LitchcraftOpenSource source = LitchcraftOpenSource.library,
  }) async {
    await _ensureLibraryLoaded();
    if (!artwork.isSupported) {
      throw LitchcraftUnsupportedArtwork(artwork.unavailableReason!);
    }
    final sourceDocument = session.document;
    final sourceGeneration = session.replacementGeneration;
    final resolved = await artwork.resolve();
    final saved = source == LitchcraftOpenSource.library
        ? artworks.where((art) => art.id == artwork.id).firstOrNull
        : null;
    final baseline = saved == null
        ? null
        : identical(saved, artwork)
        ? resolved
        : await saved.resolve();
    if (_closed) return false;
    // External async selection may outlive its view or connection. Recheck at
    // the actual replacement boundary, before retiring any retained draft.
    if (admissionGuard != null && !admissionGuard()) return false;
    if (sourceGeneration != session.replacementGeneration ||
        !identical(sourceDocument, session.document)) {
      throw StateError('Artwork changed while opening. Try again.');
    }
    // Selecting from the phone library is a local UI action. Publish the
    // document immediately, then let phone persistence and the board's
    // acknowledged whole-document replacement finish independently.
    if (_unavailableDraft != null) {
      _retireUnavailableDraft();
    } else {
      previousDraft = _draft();
    }
    final delivery = controller.replaceDocument(resolved.document);
    favoriteColors = List<int>.of(resolved.favoriteColors);
    controller.setFavoriteColors(favoriteColors);
    _active = baseline;
    _cachedDraft = resolved;
    _draftOrigin = _active == null
        ? LitchcraftDraftOrigin.external
        : LitchcraftDraftOrigin.saved;
    notifyListeners();
    await Future.wait(<Future<void>>[flush(), delivery]);
    if (saveFailed) {
      throw lastSaveError ?? StateError('Could not preserve current canvas.');
    }
    return true;
  }

  Future<void> loadVersionArtwork(int taps) async {
    if (taps == 3) return loadBouncingBallsBenchmark();
    if (taps == 8) return loadRickroll();
    final preset = LitchcraftSurprisePreset.values.firstWhere(
      (preset) => preset.taps == taps,
      orElse: () => throw ArgumentError.value(taps, 'taps'),
    );
    await ready;
    if (_closed) return;
    final now = DateTime.now().toUtc();
    await open(
      LitchcraftArtwork.animated(
        id: 'about-preset-${preset.taps}',
        name: 'Artwork',
        document: LitchcraftAnimationDocument(
          frames: preset.createFrames(),
          topology: controller.canvas.topology,
          favoriteColors: favoriteColors,
          sparkleSeed: preset.sparkleSeed,
        ),
        createdAt: now,
        modifiedAt: now,
      ),
    );
  }

  Future<void> loadRickroll() async {
    await ready;
    if (_closed) return;
    final now = DateTime.now().toUtc();
    await open(
      LitchcraftArtwork.animated(
        id: 'rickroll-preset',
        name: 'Artwork',
        document: LitchcraftAnimationDocument(
          frames: LitchcraftRickrollPreset.decode().frames,
          topology: controller.canvas.topology,
          favoriteColors: favoriteColors,
          playbackOnEntry: true,
        ),
        createdAt: now,
        modifiedAt: now,
      ),
    );
  }

  Future<void> loadBouncingBallsBenchmark() async {
    await ready;
    if (_closed) return;
    final now = DateTime.now().toUtc();
    await open(
      LitchcraftArtwork.animated(
        id: 'bouncing-balls-benchmark',
        name: 'Artwork',
        document: LitchcraftAnimationDocument(
          frames: LitchcraftBouncingBallsPreset.decode().frames,
          topology: controller.canvas.topology,
          favoriteColors: favoriteColors,
          playbackOnEntry: true,
        ),
        createdAt: now,
        modifiedAt: now,
      ),
    );
  }

  Future<void> newArtwork() async {
    await _ensureLibraryLoaded();
    previousDraft = _unavailableDraft ?? _draft();
    await flush();
    if (saveFailed) {
      throw lastSaveError ?? StateError('Could not preserve current canvas.');
    }
    _retireUnavailableDraft();
    final delivery = controller.replaceDocument(
      staticLitchcraftDocument(
        LitchcraftCanvas.blank(topology: controller.canvas.topology),
      ),
      newCanvas: true,
    );
    favoriteColors = List<int>.of(litchcraftDefaultFavoriteColors);
    controller.setFavoriteColors(favoriteColors);
    _active = null;
    _cachedDraft = null;
    _draftOrigin = LitchcraftDraftOrigin.anonymous;
    notifyListeners();
    await Future.wait(<Future<void>>[flush(), delivery]);
    if (saveFailed) {
      throw lastSaveError ?? StateError('Could not preserve current canvas.');
    }
  }

  Future<void> rename(LitchcraftArtwork artwork, String name) async {
    await _ensureLibraryLoaded();
    final before = artworks;
    final oldActive = _active;
    final modifiedAt = DateTime.now().toUtc();
    final renamed = artwork.copyWith(name: name.trim(), modifiedAt: modifiedAt);
    artworks = artworks
        .map((art) => art.id == artwork.id ? renamed : art)
        .toList();
    if (_active?.id == artwork.id) {
      _active = _active!.copyWith(name: name.trim(), modifiedAt: modifiedAt);
    }
    await flush();
    if (saveFailed) {
      artworks = before;
      _active = oldActive;
      throw lastSaveError ?? StateError('Artwork rename failed.');
    }
  }

  Future<void> duplicate(LitchcraftArtwork artwork) async {
    await _ensureLibraryLoaded();
    final before = artworks;
    final now = DateTime.now().toUtc();
    artworks = <LitchcraftArtwork>[
      artwork.copyWith(id: _newId(), createdAt: now, modifiedAt: now),
      ...artworks,
    ];
    await flush();
    if (saveFailed) {
      artworks = before;
      throw lastSaveError ?? StateError('Artwork copy failed.');
    }
  }

  Future<void> delete(LitchcraftArtwork artwork) async {
    await _ensureLibraryLoaded();
    final before = artworks;
    final oldActive = _active;
    final oldOrigin = _draftOrigin;
    artworks = artworks.where((art) => art.id != artwork.id).toList();
    if (_active?.id == artwork.id) {
      _cachedDraft = _draft();
      _active = null;
      _draftOrigin = LitchcraftDraftOrigin.external;
    }
    await flush();
    if (saveFailed) {
      artworks = before;
      _active = oldActive;
      _draftOrigin = oldOrigin;
      throw lastSaveError ?? StateError('Artwork delete failed.');
    }
  }

  void selectFrame(int index) => session.selectFrame(index);
  void addFrame() => session.addFrame();
  void duplicateSelectedFrame() => session.addFrame();
  void deleteSelectedFrame() => session.deleteSelectedFrame();
  void setSelectedFrameDuration(int durationMs) =>
      session.setSelectedFrameDuration(durationMs);
  void applyEffect({
    required int square,
    required int mask,
    required LitchcraftEffectStyle effect,
    int effectDurationMs = litchcraftAnimationDefaultEffectDurationMs,
  }) => session.applyEffect(
    square: square,
    mask: mask,
    effect: effect,
    effectDurationMs: effectDurationMs,
  );
  void undoAnimation() => session.undo();
  void redoAnimation() => session.redo();

  @override
  void dispose() {
    _closed = true;
    _draftTimer?.cancel();
    controller.removeListener(_canvasChanged);
    session.removeListener(_documentChanged);
    // Owners flush before disposing; avoid asynchronous reads of a dead controller.
    super.dispose();
  }
}
