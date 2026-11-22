import 'dart:async';
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:typed_data';

import 'package:flutter/foundation.dart'
    show debugPrint, kReleaseMode, listEquals;
import 'package:flutter/widgets.dart';

import '../models/litchcraft_state.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_codec.dart';
import '../models/litchcraft_animation_renderer.dart';
import '../models/litchcraft_bouncing_balls_preset.dart';
import '../models/litchcraft_document_session.dart';
import '../models/home_state_notification.dart';
import '../models/home_board_snapshot.dart';
import '../models/litch_control_action.dart';
import '../services/app_trace_recorder.dart';
import '../services/connection_session.dart';
import '../services/connection_session_codec.dart';
import '../services/connection_litchcraft_codec.dart';
import '../services/connection_litchcraft_frame_store.dart';
import '../services/litch_ble_service.dart';
import '../services/litchcraft_frame_store_protocol.dart';
import '../services/litchcraft_protocol.dart';

LitchcraftController? _frameStoreDiagnosticOwner;
bool _frameStoreDiagnosticRegistered = false;

void _registerFrameStoreDiagnostic(LitchcraftController owner) {
  if (kReleaseMode) return;
  _frameStoreDiagnosticOwner = owner;
  if (_frameStoreDiagnosticRegistered) return;
  _frameStoreDiagnosticRegistered = true;
  developer.registerExtension('ext.litch.litchcraft.frameStore256', (
    String _,
    Map<String, String> parameters,
  ) async {
    final controller = _frameStoreDiagnosticOwner;
    if (controller == null) {
      return developer.ServiceExtensionResponse.result(
        jsonEncode(<String, Object?>{
          'ok': false,
          'error': 'no_litchcraft_controller',
        }),
      );
    }
    try {
      final corpus = parameters['corpus'] ?? 'repeated';
      final result = switch (corpus) {
        'repeated' => await controller.runRepeated256FrameDiagnostic(),
        'sparse' => await controller.runSparse256FrameDiagnostic(),
        'dense' => await controller.runDense256FrameDiagnostic(),
        _ => throw ArgumentError.value(corpus, 'corpus'),
      };
      return developer.ServiceExtensionResponse.result(
        jsonEncode(<String, Object?>{
          'ok': true,
          'corpus': corpus,
          'frames': result.transfer.state.frameCount,
          'body_bytes': result.transfer.bodyBytes,
          'chunks': result.transfer.chunkPackets,
          'checkpoints': result.transfer.checkpoints,
          'writes': result.transfer.writes,
          'setup_ms': result.setupMilliseconds,
          'preparation_ms': result.preparationMilliseconds,
          'begin_write_ms': result.transfer.beginWriteMilliseconds,
          'maximum_packet_bytes': result.maximumPacketBytes,
          'object_maximum_body_bytes': result.objectMaximumBodyBytes,
          'semantic_window_chunks': result.semanticWindowChunks,
          'bulk_write_ms': result.transfer.bulkWriteMilliseconds,
          'checkpoint_ms': result.transfer.checkpointMilliseconds,
          'commit_write_ms': result.transfer.commitWriteMilliseconds,
          'receipt_wait_ms': result.transfer.receiptWaitMilliseconds,
          'transfer_ms': result.transfer.elapsedMilliseconds,
          'total_ms': result.totalElapsedMilliseconds,
          'generation': result.transfer.state.generation,
          'playing': result.transfer.state.playing,
        }),
      );
    } catch (error) {
      return developer.ServiceExtensionResponse.result(
        jsonEncode(<String, Object?>{
          'ok': false,
          'error': error.runtimeType.toString(),
          'message': error.toString(),
        }),
      );
    }
  });
}

class LitchcraftFrameEditTarget {
  const LitchcraftFrameEditTarget({
    required this.frame,
    required this.session,
    required this.revision,
    required this.generation,
    this.documentGeneration,
    this.sourceGeneration,
    this.linkToken,
    this.deviceId,
    this.bootNonce,
    this.storeGeneration,
    this.localOnly = false,
  }) : _orderedSession = null,
       _beforeDocument = null,
       _predecessor = null;

  LitchcraftFrameEditTarget._store({
    required this.frame,
    required this.session,
    required this.revision,
    required this.generation,
    required this.documentGeneration,
    required this.sourceGeneration,
    required this.linkToken,
    required this.deviceId,
    required this.bootNonce,
    required this.storeGeneration,
    required ConnectionSession orderedSession,
    required LitchcraftAnimationDocument beforeDocument,
    required _StoreEditPosition? predecessor,
  }) : localOnly = false,
       _orderedSession = orderedSession,
       _beforeDocument = beforeDocument,
       _predecessor = predecessor;

  final int frame;
  final int session;
  final int revision;
  final int generation;
  final int? documentGeneration;
  final int? sourceGeneration, linkToken, bootNonce, storeGeneration;
  final String? deviceId;
  final bool localOnly;
  final ConnectionSession? _orderedSession;
  final LitchcraftAnimationDocument? _beforeDocument;
  final _StoreEditPosition? _predecessor;
}

// One result reference over the existing mutation tail, never a second queue.
// An accepted receipt alone does not publish a result: the exact board/common
// document and authority must still be coherent at completion.
typedef _StoreEditResult = ({
  _StoreAuthority authority,
  LitchcraftAnimationDocument before,
  LitchcraftAnimationDocument document,
  int selectedBefore,
  int selectedAfter,
});

// One acceptance witness attached to the existing pointer-stroke lifetime.
// It retains no document, queue or wire payload.
typedef _StoreStrokeAcceptance = ({
  int stroke,
  int frame,
  int generation,
  int sourceGeneration,
  int modeSession,
  int linkToken,
  int bootNonce,
  int paintGeneration,
  int revision,
  String deviceId,
  ConnectionSession session,
});

final class _StoreEffectClockBinding {
  _StoreEffectClockBinding({
    required this.generation,
    required this.source,
    required this.mode,
    required this.linkToken,
    required this.link,
    required this.session,
  }) : bootNonce = session.boardBootNonce;
  final int generation, source, mode, linkToken, bootNonce;
  final LitchcraftLink link;
  final ConnectionSession session;
  bool requested = false;
  bool sampled = false;
}

final class _StoreEditPosition {
  _StoreEditPosition({
    required this.generation,
    required this.sourceGeneration,
    required this.modeSession,
    required this.linkToken,
    required this.deviceId,
    required this.session,
  }) : bootNonce = session.boardBootNonce;

  final int generation, sourceGeneration, modeSession, linkToken, bootNonce;
  final String deviceId;
  final ConnectionSession session;
  final completion = Completer<_StoreEditResult?>();
  _StoreEditResult? result;
}

/// Hidden production-path evidence for schema-4 transfer tuning. This is not
/// app presentation state: it reports the exact negotiated BLE submission and
/// board commit timings used by hardware QA.
class LitchcraftFrameStoreDiagnosticResult {
  const LitchcraftFrameStoreDiagnosticResult({
    required this.setupMilliseconds,
    required this.preparationMilliseconds,
    required this.totalElapsedMilliseconds,
    required this.transfer,
    this.maximumPacketBytes = 0,
    this.objectMaximumBodyBytes = 0,
    this.semanticWindowChunks = 0,
  });

  final int setupMilliseconds;
  final int preparationMilliseconds;
  final int totalElapsedMilliseconds;
  final LitchcraftFrameStoreTransferResult transfer;
  final int maximumPacketBytes;
  final int objectMaximumBodyBytes;
  final int semanticWindowChunks;
}

/// One proven board history boundary. These proofs mirror only app-originated
/// edits; they never replace or guess the board-owned history.
class _KnownAdvancedHistory {
  const _KnownAdvancedHistory(
    this.before,
    this.after,
    this.session,
    this.revision,
    this.generation,
  );
  final LitchcraftAnimationDocument before, after;
  final int session, revision, generation;
}

/// Connection-scoped proof that an exact semantic document has a selectable
/// Store resident. Documents and frames are retained by reference; this cache
/// never duplicates an encoded body or the animation's emitter storage.
class _KnownFrameStoreResident {
  const _KnownFrameStoreResident({
    required this.session,
    required this.linkToken,
    required this.document,
    required this.resident,
  });

  final int session, linkToken;
  final LitchcraftAnimationDocument document;
  final LitchcraftFrameStoreResident resident;
}

/// A Store resident needs an explicit artwork source choice. Keep that
/// decision pending while the chooser obtains or reuses a verified preview.
class _GameEntryChoice {
  _GameEntryChoice(
    this.deviceId,
    this.connectionToken,
    this.selectionIntent,
    this.home,
  );

  final String deviceId;
  final int connectionToken, selectionIntent;
  final HomeStateNotification home;
}

class _PendingFrameStoreConflict {
  _PendingFrameStoreConflict({
    required this.session,
    required this.authoritySession,
    this.observedFrameCount,
    this.entryObservation,
    required this.deviceId,
    required this.generation,
    required this.selectionIntent,
    required this.linkToken,
    required this.sourceGeneration,
    required this.bootNonce,
  });

  final ConnectionSession session;
  // This owner retains source identity, not authority to adopt board bytes.
  // Entry may lend its freshly verified sample once. Every completed preview
  // still checks fresh Inspect/live-revision evidence after reading pixels.
  final int authoritySession;
  final int? observedFrameCount;
  ({LitchcraftFrameStoreState state, _StoreAuthority authority})?
  entryObservation;
  final String deviceId;
  final int generation;
  final int selectionIntent;
  final int linkToken, sourceGeneration, bootNonce;
  Future<void>? previewWork, documentWork;
  Completer<void> previewRetirement = Completer<void>();
  LitchcraftFrameStoreSnapshot? previewSnapshot;
  LitchcraftFrameStoreFramePreview? previewFrame;
  _StoreAuthority? previewAuthority;
  bool previewFailed = false;
}

class _InterruptedStoreReplacement {
  _InterruptedStoreReplacement({
    required this.boardId,
    required this.bootNonce,
    required this.sourceGeneration,
    required this.base,
  });
  final String boardId;
  final int bootNonce, sourceGeneration;
  final LitchcraftFrameStoreResident base;
  Object? failure;
  bool consumed = false;
  ({int generation, int linkToken, int authoritySequence})? committed;
}

typedef _StoreAuthority = ({
  String deviceId,
  int bootNonce,
  int generation,
  int revision,
  bool dirty,
});

/// Page selection owns admission. Firmware orders live edits; detached drafts
/// have bounded history and reconcile against the last proven common canvas.
class LitchcraftController extends ChangeNotifier with WidgetsBindingObserver {
  static const bool _automaticFrameStoreBenchmark = bool.fromEnvironment(
    'LITCH_FRAME_STORE_BENCHMARK',
  );
  static const String _automaticFrameStoreBenchmarkCorpus =
      String.fromEnvironment(
        'LITCH_FRAME_STORE_BENCHMARK_CORPUS',
        defaultValue: 'repeated',
      );
  static const Duration _connectionPreparationTimeout = Duration(seconds: 6);
  static const Duration _playbackClockSyncInterval = Duration(minutes: 1);
  static const int _playbackClockMaximumCorrectionMs = 20;
  static const Duration _playbackClockSlewWindow = Duration(seconds: 4);
  static const Duration physicalColorPickedDisplayDuration = Duration(
    seconds: 3,
  );
  static const Duration physicalShakeEraseDisplayDuration = Duration(
    seconds: 3,
  );

  LitchcraftController(
    this.bleService, {
    LitchcraftCanvas? initialCanvas,
    LitchcraftDocumentSession? documentSession,
    this.monotonicMilliseconds,
    Duration physicalColorPickedDuration = physicalColorPickedDisplayDuration,
    Duration physicalShakeEraseDuration = physicalShakeEraseDisplayDuration,
    Duration frameStoreResponseTimeout = const Duration(seconds: 6),
    AppTraceSink? appTrace,
  }) : _physicalColorPickedDuration = physicalColorPickedDuration,
       _physicalShakeEraseDuration = physicalShakeEraseDuration,
       _frameStoreResponseTimeout = frameStoreResponseTimeout,
       _appTrace = appTrace,
       documentSession =
           documentSession ??
           LitchcraftDocumentSession(
             staticLitchcraftDocument(
               initialCanvas ?? LitchcraftCanvas.blank(),
             ),
             localIntent: initialCanvas != null,
           ),
       _canvas =
           documentSession?.canvas ?? initialCanvas ?? LitchcraftCanvas.blank(),
       _board = initialCanvas ?? LitchcraftCanvas.blank() {
    _offlineDiverged = initialCanvas != null;
    _observedSourceGeneration = this.documentSession.replacementGeneration;
    this.documentSession.addListener(_documentChanged);
    _connection = bleService.connectionStream.listen(
      (_) => _connectionChanged(),
    );
    _home = bleService.homeStateStream.listen((_) {
      _retryEmptyBoard();
      final choice = _gameEntryChoice;
      final home = bleService.latestHomeState;
      if (choice != null &&
          home != null &&
          (home.position.fen != choice.home.position.fen ||
              home.freshSetup ||
              home.physicalStatus.occupiedSquares != BigInt.zero)) {
        _gameEntryChoice = null;
        _notify();
      }
    });
    WidgetsBinding.instance.addObserver(this);
    _registerFrameStoreDiagnostic(this);
  }
  final LitchBleService bleService;
  final int Function()? monotonicMilliseconds;
  int get _advancedTimelineNow =>
      monotonicMilliseconds?.call() ??
      _advancedTimelineClock.elapsedMilliseconds;
  final LitchcraftDocumentSession documentSession;
  // Negotiated per connection; bit 14 keeps older firmware on static recovery.
  bool _projectingDocument = false, _advancedWire = false;
  bool _advancedCanUndo = false, _advancedCanRedo = false;
  bool _advancedPlaying = false;
  bool? _frameStorePlaying;
  bool _acceptedStoreCommitAwaitingRoot = false;
  LitchcraftFrameStoreCommitPending? _pendingStoreCommit;
  bool _storeReplacementAwaitingAdmission = false;
  bool _storeTransferInFlight = false;
  bool _storePlaybackOnEntryRequested = false;
  bool _queuedStorePlay = false;
  bool _queuedStorePlayFailed = false;
  int _storeTransferMarker = 0;
  Completer<void>? _frameStoreTransferRetirement;

  void _retireFrameStoreTransfer() {
    final retirement = _frameStoreTransferRetirement;
    if (retirement != null && !retirement.isCompleted) retirement.complete();
  }

  int? _pendingStoreSelectedFrame;
  bool get animationPlaybackPending => _queuedStorePlay;
  bool get animationTimelinePending {
    final command = _storeTimelineCommand;
    return command != null &&
        command.generation == _generation &&
        command.intent == _playbackIntentGeneration &&
        command.linkToken == _linkToken &&
        _boardActive &&
        _linkUsable;
  }

  bool get animationPlaybackFailed => _queuedStorePlayFailed;
  bool get storeRootPublicationPending => _acceptedStoreCommitAwaitingRoot;
  bool get frameStorePlaybackRequested =>
      _storeTransferInFlight && _storePlaybackOnEntryRequested;
  bool get frameStoreArtworkReadPending => _pendingFrameStoreConflict != null;

  bool _currentConflictPreview(_PendingFrameStoreConflict pending) =>
      !_disposed &&
      identical(_pendingFrameStoreConflict, pending) &&
      pending.generation == _generation &&
      pending.sourceGeneration == documentSession.replacementGeneration &&
      pending.selectionIntent == _selectionIntent &&
      pending.linkToken == _linkToken &&
      pending.bootNonce == pending.session.boardBootNonce &&
      identical(pending.session, _orderedSession) &&
      pending.session.isActivated &&
      pending.deviceId == _deviceId &&
      pending.authoritySession == _documentSessionId &&
      bleService.isConnected &&
      _selected &&
      _boardActive &&
      (_status == LitchcraftStatus.conflict ||
          (_status == LitchcraftStatus.live &&
              _frameStoreReadPending &&
              pending.observedFrameCount == 1));

  bool _liveSnapshotMatchesAuthority(
    LitchcraftFrameStoreSnapshot? snapshot,
    _StoreAuthority? authority,
  ) =>
      snapshot != null &&
      authority != null &&
      snapshot.session == _documentSessionId &&
      snapshot.liveRevision != 0 &&
      snapshot.documentEpoch != 0 &&
      snapshot.generation == authority.generation &&
      snapshot.liveRevision == authority.revision;

  bool _verifiedConflictPreview(_PendingFrameStoreConflict pending) {
    final authority = pending.previewAuthority;
    final observed = _storeAuthority;
    return _currentConflictPreview(pending) &&
        !pending.previewRetirement.isCompleted &&
        (pending.previewSnapshot != null || pending.previewFrame != null) &&
        authority != null &&
        observed != null &&
        (!observed.dirty ||
            _liveSnapshotMatchesAuthority(pending.previewSnapshot, observed)) &&
        observed.bootNonce == authority.bootNonce &&
        observed.generation == authority.generation &&
        observed.revision == authority.revision;
  }

  LitchcraftCanvas? get conflictBoardPreview {
    final pending = _pendingFrameStoreConflict;
    if (pending == null || !_verifiedConflictPreview(pending)) return null;
    final document = pending.previewSnapshot?.document;
    if (document != null) {
      final frame = pending.previewSnapshot!.selectedFrame.clamp(
        0,
        document.frames.length - 1,
      );
      return LitchcraftCanvas(
        colors: document.frames[frame].emitters.map((emitter) => emitter.color),
        topology: document.topology,
      );
    }
    final preview = pending.previewFrame!;
    return LitchcraftCanvas(
      colors: preview.frame.emitters.map((emitter) => emitter.color),
      topology: preview.topology,
    );
  }

  bool get conflictBoardPreviewLoading =>
      _pendingFrameStoreConflict?.previewWork != null ||
      _pendingFrameStoreConflict?.documentWork != null;
  bool get conflictBoardPreviewFailed {
    final pending = _pendingFrameStoreConflict;
    return pending != null &&
        (pending.previewFailed ||
            ((pending.previewSnapshot != null ||
                    pending.previewFrame != null) &&
                !_verifiedConflictPreview(pending)));
  }

  /// A chooser read is local observation. Retiring it does not cancel an upload
  /// or adopt any board bytes; the bounded shared Object stream may still drain.
  void retireConflictBoardPreview() {
    final pending = _pendingFrameStoreConflict;
    if (pending == null) return;
    if (!pending.previewRetirement.isCompleted) {
      pending.previewRetirement.complete();
    }
    pending.previewWork = null;
    pending.previewSnapshot = null;
    pending.previewFrame = null;
    pending.previewAuthority = null;
    pending.entryObservation = null;
    pending.previewFailed = false;
    if (!_disposed) _notify();
  }

  Future<void> prepareConflictBoardPreview() {
    final pending = _pendingFrameStoreConflict;
    if (pending == null || !_currentConflictPreview(pending)) {
      return Future<void>.value();
    }
    final work = pending.previewWork;
    if (work != null) return work;
    if (_verifiedConflictPreview(pending)) return Future<void>.value();
    if (pending.previewRetirement.isCompleted) {
      pending.previewRetirement = Completer<void>();
    }
    final retirement = pending.previewRetirement;
    pending.previewFailed = false;
    pending.previewSnapshot = null;
    pending.previewFrame = null;
    pending.previewAuthority = null;
    late final Future<void> read;
    read = _readConflictBoardPreview(pending, retirement)
        .catchError((Object _) {
          if (_currentConflictPreview(pending) &&
              identical(pending.previewRetirement, retirement) &&
              !retirement.isCompleted) {
            pending.previewFailed = true;
          }
        })
        .whenComplete(() {
          if (identical(pending.previewWork, read)) {
            pending.previewWork = null;
            if (!_disposed) _notify();
          }
        });
    pending.previewWork = read;
    _notify();
    return read;
  }

  Future<void> _readConflictBoardPreview(
    _PendingFrameStoreConflict pending,
    Completer<void> retirement, {
    bool fullDocument = false,
  }) async {
    bool current() =>
        _currentConflictPreview(pending) &&
        identical(pending.previewRetirement, retirement) &&
        !retirement.isCompleted;
    Future<T> wait<T>(Future<T> work) => Future.any<T>([
      work,
      retirement.future.then<T>(
        (_) => throw const LitchcraftTransferCancelled(),
      ),
    ]);
    final client = _frameStoreClient(pending.session);
    final entryObservation = fullDocument ? null : pending.entryObservation;
    pending.entryObservation = null;
    final LitchcraftFrameStoreState before =
        entryObservation?.state ??
        await wait(
          client.inspect(
            session: pending.authoritySession,
            request: _requestId = (_requestId % 0xffff) + 1,
          ),
        );
    final authority =
        entryObservation?.authority ??
        await wait(_requestFreshStoreAuthority(pending.session));
    if (!current()) throw const LitchcraftTransferCancelled();
    if (!before.hasArtwork ||
        before.session != pending.authoritySession ||
        authority == null ||
        authority.dirty ||
        authority.generation != before.generation ||
        authority.bootNonce != pending.bootNonce) {
      throw StateError('Board artwork changed during preview. Try again.');
    }
    final beforeResident = before.residentListing?.residents
        .where(
          (resident) =>
              resident.rootSlot == before.residentListing!.selectedRootSlot &&
              resident.generation == before.generation &&
              resident.frameCount == before.frameCount &&
              resident.topology == before.topology,
        )
        .firstOrNull;
    final priorAuthority = pending.previewAuthority;
    if (fullDocument &&
        (priorAuthority == null ||
            authority.generation != priorAuthority.generation ||
            authority.revision != priorAuthority.revision ||
            authority.bootNonce != priorAuthority.bootNonce)) {
      throw StateError('Board artwork changed before choice. Try again.');
    }
    LitchcraftAnimationDocument? knownDocument;
    final listing = before.residentListing;
    if (listing != null &&
        listing.session == before.session &&
        listing.generation == before.generation) {
      for (final known in _knownFrameStoreResidents) {
        if (known.session == before.session &&
            known.linkToken == pending.linkToken &&
            known.document.frames.length == known.resident.frameCount &&
            known.document.topology.id == known.resident.topology &&
            known.document.playbackOnEntry == known.resident.playbackOnEntry &&
            known.document.sparkleSeed == known.resident.sparkleSeed &&
            listEquals(
              known.document.favoriteColors,
              known.resident.favoriteColors,
            ) &&
            listing.residents.any(
              (resident) =>
                  resident.rootSlot == listing.selectedRootSlot &&
                  resident.generation == before.generation &&
                  _sameFrameStoreResidentIdentity(resident, known.resident),
            )) {
          knownDocument = known.document;
          break;
        }
      }
    }
    final fullSync = Object();
    if (fullDocument && knownDocument == null && before.frameCount > 1) {
      _startFullAnimationSync(
        fullSync,
        LitchcraftFullSyncDirection.readingFromBoard,
      );
    }
    LitchcraftFrameStoreSnapshot? snapshot;
    LitchcraftFrameStoreFramePreview? preview;
    try {
      if (knownDocument != null) {
        _appTrace?.record(
          'litchcraft.frame_store_snapshot_read',
          fields: {
            'read_kind': 'resident_cache',
            'session': before.session,
            'generation': before.generation,
            'frames': knownDocument.frames.length,
            'body_bytes': 0,
            'chunks': 0,
            'cache_hits': knownDocument.frames.length,
          },
        );
        snapshot = LitchcraftFrameStoreSnapshot(
          document: knownDocument,
          session: before.session,
          generation: before.generation,
          selectedFrame: before.selectedFrame,
          active: before.active,
          playing: before.playing,
        );
      } else if (!fullDocument) {
        preview = await client.framePreview(
          session: before.session,
          generation: before.generation,
          allocateRequest: () => _requestId = (_requestId % 0xffff) + 1,
          cancellationFuture: retirement.future,
        );
      } else {
        snapshot = await client.snapshot(
          session: before.session,
          generation: before.generation,
          allocateRequest: () => _requestId = (_requestId % 0xffff) + 1,
          cancellationFuture: retirement.future,
          onProgress: before.frameCount > 1
              ? (completed, total) =>
                    _updateFullAnimationSync(fullSync, completed, total)
              : null,
        );
      }
    } finally {
      _finishFullAnimationSync(fullSync);
    }
    final runtimeSequence = _connectionStateRevision;
    final after = await wait(
      client.inspect(
        session: before.session,
        request: _requestId = (_requestId % 0xffff) + 1,
      ),
    );
    final fresh = await wait(_requestFreshStoreAuthority(pending.session));
    if (!current()) throw const LitchcraftTransferCancelled();
    if ((snapshot?.session ?? preview?.session) != before.session ||
        (snapshot?.generation ?? preview?.generation) != before.generation ||
        !after.hasArtwork ||
        after.session != before.session ||
        after.generation != before.generation ||
        after.active != before.active ||
        fresh == null ||
        fresh.dirty ||
        fresh.bootNonce != authority.bootNonce ||
        fresh.generation != authority.generation ||
        fresh.revision != authority.revision) {
      throw StateError('Board artwork changed during preview. Try again.');
    }
    // Inspect is a fallback only; a newer retained runtime State owns the frame.
    if (_connectionStateRevision == runtimeSequence &&
        after.selectedFrame < before.frameCount) {
      _advancedSelectedFrame = after.selectedFrame;
      _frameStorePlaying = after.playing;
    }
    final matchingResident =
        beforeResident != null &&
        before.residentListing?.session == before.session &&
        before.residentListing?.generation == before.generation &&
        after.residentListing?.session == after.session &&
        after.residentListing?.generation == after.generation &&
        after.residentListing?.selectedRootSlot == beforeResident.rootSlot &&
        after.residentListing!.residents.any(
          (resident) =>
              _sameFrameStoreResidentIdentity(resident, beforeResident),
        );
    if (_frameStoreReadPending &&
        _status == LitchcraftStatus.live &&
        (!matchingResident ||
            before.frameCount != 1 ||
            after.frameCount != 1)) {
      throw StateError('Board artwork changed during preview. Try again.');
    }
    if (snapshot == null &&
        preview != null &&
        preview.frameCount == 1 &&
        before.frameCount == 1 &&
        after.frameCount == 1 &&
        matchingResident) {
      // A complete single-frame read plus its unchanged root manifest is the
      // whole document. Keep its emitter storage by reference for either choice.
      snapshot = LitchcraftFrameStoreSnapshot(
        document: LitchcraftAnimationDocument(
          frames: [preview.frame],
          topology: preview.topology,
          favoriteColors: beforeResident.favoriteColors,
          playbackOnEntry: beforeResident.playbackOnEntry,
          sparkleSeed: beforeResident.sparkleSeed,
        ),
        session: before.session,
        generation: before.generation,
        selectedFrame: 0,
        active: after.active,
        playing: after.playing,
      );
    }
    pending.previewSnapshot = snapshot;
    pending.previewFrame = preview;
    pending.previewAuthority = fresh;
    _notify();
  }

  bool get historyRecoveryPending {
    bool matches(
      int generation,
      int source,
      int mode,
      ConnectionSession session,
    ) =>
        !_disposed &&
        generation == _generation &&
        source == documentSession.replacementGeneration &&
        mode == _documentSessionId &&
        identical(session, _orderedSession) &&
        session.isActivated &&
        _selected &&
        _boardActive;
    final position = _storeUnknownHistoryPosition;
    if (position != null &&
        matches(
          position.generation,
          position.sourceGeneration,
          position.modeSession,
          position.session,
        ) &&
        position.linkToken == _linkToken &&
        position.deviceId == _deviceId &&
        position.bootNonce == position.session.boardBootNonce) {
      return true;
    }
    final repair = _storeHistoryRepairWait;
    if (repair != null &&
        matches(
          repair.generation,
          repair.sourceGeneration,
          repair.modeSession,
          repair.session,
        ) &&
        repair.linkToken == _linkToken &&
        repair.deviceId == _deviceId &&
        repair.bootNonce == repair.session.boardBootNonce) {
      return true;
    }
    final physical = _physicalStoreRepairBinding;
    if (physical != null &&
        (_physicalStoreRepairing || _physicalStoreRepairThroughRevision != 0) &&
        matches(
          physical.generation,
          physical.sourceGeneration,
          physical.modeSession,
          physical.session,
        ) &&
        physical.linkToken == _linkToken &&
        physical.deviceId == _deviceId &&
        physical.bootNonce == physical.session.boardBootNonce &&
        usesFrameStoreDocument &&
        !storeReplacementUnresolved) {
      return true;
    }
    final canvas = _pendingStoreCanvas;
    return canvas?.followingHistory?.isCompleted == false &&
        matches(
          canvas!.generation,
          canvas.sourceGeneration,
          canvas.modeSession,
          canvas.session,
        );
  }

  bool get artworkReadbackPending =>
      _physicalStoreReadbackToken != null && historyRecoveryPending;

  void _cancelPendingStoreCommit() {
    _storeHistoryEcho = null;
    final pending = _pendingStoreCommit;
    final fullSync = _pendingStoreFullSync;
    _pendingStoreCommit = null;
    _pendingStoreFullSync = null;
    _acceptedStoreCommitAwaitingRoot = false;
    _queuedStorePlay = false;
    _queuedStorePlayFailed = false;
    if (pending != null) unawaited(pending.cancel());
    if (fullSync != null) _finishFullAnimationSync(fullSync);
  }

  int _frameStoreGeneration = 0;
  _InterruptedStoreReplacement? _interruptedStoreReplacement;
  _StoreAuthority? _storeAuthority;
  _StoreAuthority? _storeCommonAuthority;
  int? _storeCommonBindingGeneration;
  int _storeAuthorityStateSequence = 0;
  bool _storePaintWaitingForAuthority = false;
  ({
    ConnectionSession session,
    int generation,
    int sourceGeneration,
    int stroke,
    int frame,
  })?
  _pendingStoreStrokeEnd;
  bool _storeStrokeEndInFlight = false;
  LitchcraftV2State? _pendingStoreEntryInspection;
  LitchcraftV2State? _storeEntryInspection;
  bool _resumingStoreEntry = false;
  int _playbackIntentGeneration = 0;
  int? _storePausedIntent;
  ({int generation, int intent, int linkToken})? _storeTimelineCommand;
  LitchcraftV2TimelineSample? _storeTimelineProjection;
  _StoreEditPosition? _storeStructuralEditPosition;
  _StoreEditPosition? _storeToolEditPosition;

  bool _localFrameStoreDocument = false;
  bool _frameStoreReadPending = false;
  ({
    Object owner,
    int generation,
    int selectionIntent,
    int linkToken,
    int sourceGeneration,
  })?
  _frameStoreSnapshotRead;
  final _frameStoreContentIdentities =
      <
        ({
          LitchcraftAnimationDocument document,
          LitchcraftFrameStoreContentIdentity identity,
        })
      >[];
  int _frameStoreStartedAt = 0;
  _PendingFrameStoreConflict? _pendingFrameStoreConflict;
  LitchcraftFrameStoreState? _cachedFrameStoreInspection;
  LitchcraftFrameStoreResidents? _cachedFrameStoreResidents;
  int _cachedFrameStoreLinkToken = 0;
  final List<_KnownFrameStoreResident> _knownFrameStoreResidents = [];
  int? _selectedFrameStoreRootSlot;
  static const int _knownAdvancedHistoryLimit = 20;
  final List<_KnownAdvancedHistory> _knownAdvancedUndo = [];
  final List<_KnownAdvancedHistory> _knownAdvancedRedo = [];
  int _optimisticDocumentEdits = 0;
  int _documentPacketBytes = LitchcraftV2Protocol.minimumPacketBytes;
  LitchcraftV2DocumentClient? _documentClient;
  int _documentRevision = -1, _documentSessionId = 0;
  int _coherentDocumentRevision = -1, _coherentDocumentSession = 0;
  String? _coherentDocumentBoardId;
  Future<void>? _documentSyncFuture;
  Timer? _physicalStoreRepairTimer;
  bool _physicalStoreRepairing = false;
  Object? _physicalStoreReadbackToken;
  // Bounded immutable preimages are reuse evidence, never live authority.
  // Native history holds at most 64 operations; no second document is owned.
  final List<LitchcraftAnimationFrame> _retainedStoreFrames = [];
  Future<void> Function()? _physicalStoreRepairResume;
  ({
    Future<int> position,
    int bootNonce,
    int generation,
    int sourceGeneration,
    int modeSession,
    int linkToken,
    String? deviceId,
    ConnectionSession session,
  })?
  _storeUnknownHistoryPosition;
  _StoreEditPosition? _storeEditPosition;
  int _storePaintWaiters = 0;
  _StoreEffectClockBinding? _storeEffectClockBinding;
  bool _boardTransferSuspended = false;
  LitchcraftV2EffectClockSample? _transferEffectClockSource;
  LitchcraftV2EffectClockSample? _transferEffectClock;
  ({
    int generation,
    int sourceGeneration,
    int modeSession,
    int linkToken,
    int bootNonce,
    String deviceId,
    ConnectionSession session,
    int frame,
    int revision,
    int storeGeneration,
    void Function() recordProjection,
    bool Function() resultProved,
    bool Function() projectionRecorded,
  })?
  _storeHistoryRepairWait;
  int _physicalStoreEditRevision = 0;
  int _physicalStoreBatchRevision = 0;
  int _projectedPhysicalStoreLiveRevision = 0;
  int _physicalStoreRepairThroughRevision = 0;
  ({
    int generation,
    int sourceGeneration,
    int modeSession,
    int linkToken,
    int bootNonce,
    String? deviceId,
    ConnectionSession session,
  })?
  _physicalStoreRepairBinding;
  ({
    int sourceGeneration,
    int generation,
    int revision,
    LitchcraftAnimationDocument document,
    bool confirmed,
    ({int generation, int revision, LitchcraftAnimationDocument document})?
    previous,
  })?
  _storeHistoryEcho;
  ({
    int generation,
    int sourceGeneration,
    int modeSession,
    ConnectionSession session,
    int frame,
    int mask,
    int action,
    BigInt effectEdits,
    bool applied,
    Completer<void>? followingHistory,
    int historyRevision,
    Completer<void> completed,
    Future<void> Function() resume,
  })?
  _pendingStoreCanvas;
  StreamSubscription<LitchcraftV2TimelineSample>? _advancedTimelineSubscription;
  LitchcraftV2TimelineSample? _advancedTimeline;
  int _playbackClockSlewStartedAt = 0;
  int _playbackClockSlewCorrectionMs = 0;
  LitchcraftV2TimelineSample? _pendingAdvancedTimeline;
  LitchcraftV2State? _pendingAdvancedState;
  final Stopwatch _advancedTimelineClock = Stopwatch()..start();
  int _advancedSelectedFrame = 0;
  int _observedSourceGeneration = 0;
  bool _documentDirty = false, _documentSyncing = false;
  bool _inactiveStorageSupported = false;
  Timer? _artworkDeliveryTimer;
  bool _artworkDeliveryRunning = false;
  int _backgroundQueued = 0;
  Object? _artworkTransfer;
  double? _artworkTransferProgress;
  Object? _fullAnimationSync;
  Object? _pendingStoreFullSync;
  LitchcraftFullSyncDirection? _fullAnimationSyncDirection;
  double? _fullAnimationSyncProgress;

  LitchcraftFullSyncDirection? get fullAnimationSyncDirection =>
      _fullAnimationSyncDirection;
  double? get fullAnimationSyncProgress => _fullAnimationSyncProgress;

  void _startFullAnimationSync(
    Object owner,
    LitchcraftFullSyncDirection direction,
  ) {
    _fullAnimationSync = owner;
    _fullAnimationSyncDirection = direction;
    _fullAnimationSyncProgress = 0;
    _notify();
  }

  void _updateFullAnimationSync(Object owner, int completed, int total) {
    if (!identical(_fullAnimationSync, owner) || total <= 0) return;
    // Bytes submitted or received are not a committed, validated artwork.
    final next = (completed / total).clamp(0.0, 0.99);
    final previous = _fullAnimationSyncProgress;
    if (previous != null && (next * 100).floor() == (previous * 100).floor()) {
      return;
    }
    _fullAnimationSyncProgress = next;
    _notify();
  }

  void _finishFullAnimationSync(Object owner) {
    if (!identical(_fullAnimationSync, owner)) return;
    _fullAnimationSync = null;
    _fullAnimationSyncDirection = null;
    _fullAnimationSyncProgress = null;
    _notify();
  }

  bool get artworkDeliveryPending =>
      _artworkDeliveryTimer != null ||
      _artworkDeliveryRunning ||
      _artworkTransfer != null;
  bool get artworkDeliveryFinishing =>
      _artworkTransfer != null && _artworkTransferProgress == 1;
  double? get artworkDeliveryProgress =>
      artworkDeliveryFinishing ? null : _artworkTransferProgress;
  int _deliveryDocumentGeneration = -1, _deliveryLinkToken = -1;
  int _deliberateDeliveryGeneration = -1;
  bool get storeReplacementUnresolved =>
      _deliberateDeliveryGeneration == documentSession.replacementGeneration;

  /// Only the current explicit Open's verified predecessor can own this repair.
  bool get storeReplacementRecoveryPending {
    final interrupted = _interruptedStoreReplacement;
    final transferPending =
        _storeTransferInFlight || _acceptedStoreCommitAwaitingRoot;
    return !_disposed &&
        _selected &&
        (_status == LitchcraftStatus.error ||
            _status == LitchcraftStatus.connecting ||
            (_status == LitchcraftStatus.live && transferPending)) &&
        bleService.isConnected &&
        interrupted != null &&
        interrupted.failure is TimeoutException &&
        interrupted.committed == null &&
        interrupted.boardId == _deviceId &&
        (_orderedSession?.isActivated != true ||
            _orderedSession!.boardBootNonce == interrupted.bootNonce) &&
        interrupted.sourceGeneration == documentSession.replacementGeneration &&
        _deliberateDeliveryGeneration == interrupted.sourceGeneration &&
        !connectionRecoveryRequired &&
        (_recoveryTimer?.isActive == true ||
            _entering != null ||
            (interrupted.consumed && transferPending));
  }

  bool isStoreReplacementFailureHandled(Object error) {
    final interrupted = _interruptedStoreReplacement;
    if (error is! TimeoutException ||
        interrupted == null ||
        !identical(interrupted.failure, error)) {
      return false;
    }
    if (storeReplacementRecoveryPending) return true;
    final committed = interrupted.committed;
    final session = _orderedSession;
    final board = documentSession.boardDocument;
    final authority = _storeAuthority;
    // The verified Commit generation identifies this selected durable root in
    // the same board boot. Retaining its receipt is feedback, not retry proof.
    // Optional State37 discovery cannot delay settlement, but fresh different
    // or dirty authority observed after this receipt invalidates it.
    return !_disposed &&
        _selected &&
        _status == LitchcraftStatus.live &&
        _boardActive &&
        _linkUsable &&
        bleService.isConnected &&
        interrupted.boardId == _deviceId &&
        interrupted.sourceGeneration == documentSession.replacementGeneration &&
        committed != null &&
        committed.generation == _frameStoreGeneration &&
        committed.linkToken == _linkToken &&
        session?.isActivated == true &&
        session!.boardBootNonce == interrupted.bootNonce &&
        !storeReplacementUnresolved &&
        !_storeTransferInFlight &&
        !_acceptedStoreCommitAwaitingRoot &&
        !_frameStoreReadPending &&
        board != null &&
        sameLitchcraftDocument(board, documentSession.document) &&
        (authority == null ||
            (authority.deviceId == interrupted.boardId &&
                authority.bootNonce == interrupted.bootNonce &&
                (authority.generation == committed.generation
                    ? !authority.dirty
                    : authority.generation == interrupted.base.generation &&
                          _storeAuthorityStateSequence ==
                              committed.authoritySequence)));
  }

  bool get storePlaybackFollowsLocal =>
      _storeTransferInFlight ||
      _acceptedStoreCommitAwaitingRoot ||
      !storeReplacementUnresolved;
  LitchcraftAnimationDocument? _deliveryAttempt;
  LitchcraftAnimationDocument? _unconfirmedDocument;
  LitchcraftAnimationDocument? _unconfirmedPaintDocumentBase;
  String? _unconfirmedDocumentBoardId;
  static const _artworkDeliveryDelay = Duration(milliseconds: 600);
  final Duration _physicalColorPickedDuration;
  final Duration _physicalShakeEraseDuration;
  final Duration _frameStoreResponseTimeout;
  final AppTraceSink? _appTrace;
  late final StreamSubscription<Object?> _connection;
  late final StreamSubscription<HomeStateNotification> _home;
  static const Duration _snapshotIdleTimeout = Duration(seconds: 2);
  Timer? _snapshotIdleTimer;
  Timer? _recoveryTimer;
  Timer? _physicalColorPickedTimer;
  Timer? _physicalShakeEraseTimer;
  int _recoveryAttempt = 0;
  bool _storeRootMissing = false;
  String? _storeSessionBoardId;
  bool _notificationRepairActive = false;
  bool _connectionRestartRequested = false;
  static const _recoveryDelays = [500, 1500, 4000, 10000];
  bool get connectionRecoveryRequired =>
      _status == LitchcraftStatus.error &&
      (_storeRootMissing || _recoveryAttempt >= _recoveryDelays.length) &&
      _entering == null;
  bool get storeArtworkUnavailable =>
      _storeRootMissing && _status == LitchcraftStatus.error;
  int? _emptyAdmissionSequence;
  LitchcraftLink? _link;
  ConnectionSession? _orderedSession;
  StreamSubscription<ConnectionSessionPacket>? _connectionState;
  int _connectionStateRevision = 0;
  int _connectionStateObservedAt = 0;
  int _connectionPlaybackStateRevision = 0;
  LitchcraftCanvas _canvas, _board;
  LitchcraftCanvas? _strokeBefore, _detached;
  LitchcraftCanvas? _commonCanvas;
  LitchcraftCanvas? _trustedExitCanvas;
  String? _commonBoardId;
  String? _trustedExitBoardId;
  final List<LitchcraftCanvas> _undo = [], _redo = [];
  final Set<int> _strokeSquares = {};
  int? _strokeFrame;
  bool _strokeStoreSelectRequired = false;
  int? _deferredBoardFrame;
  // At most 256 optimistic pixels. Sequence identity keeps an older ACK from
  // retiring a newer tap on the same emitter.
  final Map<int, ({int sequence, int color})> _pendingPaint = {};
  int _supportStrokeAction = 0;
  final Set<int> _supportStrokeSquares = <int>{};
  int _paintSequence = 0;
  LitchcraftCanvas? _unconfirmedPaintCanvas;
  String? _unconfirmedPaintBoardId;
  final List<LitchcraftPacket> _snapshotDeltas = [];
  LitchcraftPacket? _snapshotStart;
  List<int>? _snapshotColors;
  List<bool>? _snapshotReceived;
  Completer<void>? _snapshotDone;
  int? _snapshotReplySession;
  Future<void> _tail = Future<void>.value();
  Future<void>? _entering;
  _GameEntryChoice? _gameEntryChoice, _gameEntryResetChoice;
  bool _gameEntryAppOnly = false, _gameEntryFailed = false;
  Future<bool>? _gameEntryResetWork;

  bool _currentGameEntryChoice(_GameEntryChoice choice) =>
      !_disposed &&
      _selected &&
      choice.selectionIntent == _selectionIntent &&
      bleService.isConnected &&
      bleService.connectedId == choice.deviceId &&
      bleService.connectionSessionToken == choice.connectionToken;

  Object? get gameEntryChoice =>
      _gameEntryChoice != null && _currentGameEntryChoice(_gameEntryChoice!)
      ? _gameEntryChoice
      : null;

  bool get gameEntryRecoveryRequired =>
      _gameEntryFailed &&
      !_disposed &&
      _selected &&
      bleService.isConnected &&
      _status == LitchcraftStatus.error;

  void keepGameForLocalArtwork(Object choice) {
    if (!identical(choice, gameEntryChoice)) return;
    _gameEntryChoice = null;
    _gameEntryFailed = false;
    _notify();
  }

  Future<bool> discardGameAndEnter(Object choice) {
    if (identical(choice, _gameEntryResetChoice) &&
        _gameEntryResetWork != null) {
      return _gameEntryResetWork!;
    }
    if (!identical(choice, gameEntryChoice)) return Future.value(false);
    final pending = choice as _GameEntryChoice;
    _gameEntryChoice = null;
    _gameEntryResetChoice = pending;
    late final Future<bool> work;
    work = _discardGameAndEnter(pending).whenComplete(() {
      if (identical(_gameEntryResetWork, work)) {
        _gameEntryResetWork = null;
        _gameEntryResetChoice = null;
        _notify();
      }
    });
    _gameEntryResetWork = work;
    return work;
  }

  Future<bool> _discardGameAndEnter(_GameEntryChoice choice) async {
    bool current() =>
        identical(choice, _gameEntryResetChoice) &&
        _currentGameEntryChoice(choice);
    _gameEntryFailed = false;
    _status = LitchcraftStatus.connecting;
    _notify();
    try {
      final refreshed = await bleService.requestCurrentHomeStateSnapshot(
        choice.deviceId,
      );
      if (!current()) return false;
      final before = bleService.latestHomeState;
      if (!refreshed ||
          before == null ||
          before.freshSetup ||
          before.position.fen != choice.home.position.fen ||
          before.physicalStatus.occupiedSquares != BigInt.zero) {
        throw StateError('Game or occupancy changed before discard');
      }
      final result = await bleService.dispatchControlAction(
        deviceId: choice.deviceId,
        action: LitchControlAction.newGame,
      );
      if (!current()) return false;
      if (!result.isSuccess) throw StateError('New game was not acknowledged');
      final confirmed = await bleService.requestCurrentHomeStateSnapshot(
        choice.deviceId,
      );
      if (!current()) return false;
      final after = bleService.latestHomeState;
      if (!confirmed ||
          after == null ||
          !isNewerHomeStateSequence(after.sequence, before.sequence) ||
          !after.freshSetup ||
          after.physicalStatus.placementMode != true ||
          after.physicalStatus.occupiedSquares != BigInt.zero) {
        throw StateError('Empty Setup was not confirmed after new game');
      }
      _gameEntryAppOnly = false;
      await _startEnter();
      if (current() && !_boardActive) {
        throw StateError('Board entry was not acknowledged');
      }
      return current() && _boardActive;
    } catch (error) {
      if (current()) {
        // A missing acknowledgement or Setup proof cannot claim that either
        // the old game or the requested reset is authoritative.
        _gameEntryFailed = true;
        _gameEntryAppOnly = true;
        _status = LitchcraftStatus.error;
        _errorMessage = error.toString();
        _notify();
      }
      return false;
    }
  }

  Future<bool> _offerGameEntryChoice(
    String deviceId,
    int connectionToken,
    int selectionIntent, {
    bool refused = false,
    HomeStateNotification? verifiedRetryHome,
  }) async {
    final prior = verifiedRetryHome ?? bleService.latestHomeState;
    if (_boardActive ||
        (!refused &&
            (prior == null ||
                prior.physicalStatus.occupiedSquares != BigInt.zero ||
                prior.clockStatus?.setupState !=
                    HomeClockSetupState.gameStarted))) {
      return false;
    }
    // Retry already read this immutable Home under the same lifetime fences.
    // Consume that sample here rather than paying for a second authority read.
    final fresh =
        verifiedRetryHome != null ||
        await bleService.requestCurrentHomeStateSnapshot(deviceId);
    if (_disposed ||
        !_selected ||
        selectionIntent != _selectionIntent ||
        !bleService.isConnected ||
        bleService.connectedId != deviceId ||
        connectionToken != bleService.connectionSessionToken) {
      return false;
    }
    final home = verifiedRetryHome ?? bleService.latestHomeState;
    if (!fresh ||
        home == null ||
        home.freshSetup ||
        home.physicalStatus.occupiedSquares != BigInt.zero ||
        (!refused &&
            home.clockStatus?.setupState != HomeClockSetupState.gameStarted)) {
      return false;
    }
    _gameEntryChoice = _GameEntryChoice(
      deviceId,
      connectionToken,
      selectionIntent,
      home,
    );
    _gameEntryAppOnly = true;
    _status = LitchcraftStatus.gameActive;
    _errorMessage = null;
    _notify();
    return true;
  }

  Future<LitchcraftPacket>? _admission;
  Completer<void>? _entryDraftWaitCancellation;
  Future<void>? _draftReady;
  Future<void>? _linkDisposal;
  Future<bool>? _adopting;
  String? _adoptingProbe;
  Future<void>? _exiting;
  String? _exitOnReconnect;
  int _color = 0xff7430, _targetMask = 15, _revision = 0, _sessionId = 0;
  int? _previousColor;
  int? _colorPreviewOrigin;
  ({LitchcraftLink link, int session, int intent, List<int> payload})?
  _pendingBrush;
  ({LitchcraftCanvas canvas, int paintBoundary})? _pendingStaticCanvas;
  bool _brushPreviewWriteQueued = false;
  Future<void>? _brushWriteFuture;
  int _brushObservation = 0;
  int _brushRevision = 0, _brushSession = 0, _brushLinkToken = -1;
  ({int color, bool eraser, int targetMask, bool project})? _confirmedBrush;
  int _strokeId = 0, _queued = 0, _generation = 0, _boardMarkCount = 0;
  int _requestId = 0,
      _linkToken = -1,
      _trustedExitLinkToken = -1,
      _trustedExitRevision = -1,
      _snapshotRequest = 0,
      _stateRevision = -1,
      _boardAuthorityLinkToken = -1;
  bool _eraser = false, _selected = false, _disposed = false, _stroke = false;
  bool? _strokeEraser;
  ({bool active, int connectionToken, String deviceId})? _inspectedBoardMode;
  int? _strokeTargetMask;
  int _selectionIntent = 0;
  bool _liveUndo = false, _liveRedo = false, _offlineDiverged = false;
  bool _waitingForLift = false,
      _physicalColorPicking = false,
      _physicalColorPicked = false,
      _physicalColorPickedLatched = false,
      _boardActive = false,
      _resyncQueued = false;
  bool _physicalShakeErased = false;
  int _lastShakeEraseToken = 0;
  int _lastShakeEraseSession = 0;
  int _lastShakeEraseLinkToken = -1;
  bool _needsResync = false;
  bool _bulkReplace = false;
  bool _advancedCanvasEdit = false;
  bool _advancedCanvasRotate = false;
  bool _advancedCanvasPattern = false;
  bool _efficientSync = false;
  bool _paletteSyncSupported = false;
  bool _shakeEraseSupported = false;
  bool _paletteSyncScheduled = false;
  bool _linkUsable = false;
  List<int> _favoriteColors = List<int>.of(litchcraftDefaultFavoriteColors);
  int _favoriteColorsRevision = 0;
  int _syncedFavoriteColorsRevision = -1;
  int _syncedFavoriteColorsLinkToken = -1;
  int? _knownReplacementRequest;
  bool _flushScheduled = false, _strokeBegun = false;
  int _paintQueueGeneration = 0, _strokePaintGeneration = 0;
  _StoreStrokeAcceptance? _acceptedStoreStroke;
  bool _recoveringRejectedPaint = false;
  String? _deviceId, _errorMessage;
  LitchcraftStatus _status = LitchcraftStatus.offline;
  String _lastTraceState = '';

  /// The selected schema-3 frame is a board-owned presentation while live.
  /// `_canvas` remains the mutable legacy/stroke cache; never write it back
  /// over a different selected document frame merely to display a Timeline.
  LitchcraftCanvas get canvas => animationBoardAuthorityLive
      ? documentSession.canvas.withTopology(_canvas.topology)
      : _canvas;
  LitchcraftCanvas get boardCanvas {
    if (_advancedWire) {
      final board = documentSession.boardDocument;
      if (board != null && board.frames.isNotEmpty) {
        final frame = _advancedSelectedFrame.clamp(0, board.frames.length - 1);
        return LitchcraftCanvas(
          colors: board.frames[frame].emitters.map((emitter) => emitter.color),
          topology: board.topology,
        );
      }
    }
    return _board;
  }

  int get color => _color;
  int? get previousColor => _previousColor;
  bool get canSwapBrushColor =>
      _previousColor != null && _previousColor != _color;
  int get targetMask => _targetMask;
  bool get eraser => _eraser;
  int get revision => _revision;
  int get sessionId => _sessionId;
  int get boardMarkCount => _boardMarkCount;
  bool get waitingForLift => _waitingForLift;
  bool get physicalColorPicking => _physicalColorPicking;
  bool get physicalColorPicked => _physicalColorPickedLatched;
  bool get physicalShakeErased => _physicalShakeErased;
  bool get shakeEraseSupported => _shakeEraseSupported;
  bool get boardActive => _boardActive;

  /// Mode observed on this connection, independent of artwork readiness.
  /// Its own binding cannot be refreshed by unrelated packets on a new link.
  bool? get inspectedBoardActive {
    final observed = _inspectedBoardMode;
    return bleService.isConnected &&
            observed?.deviceId == bleService.connectedId &&
            observed?.connectionToken == bleService.connectionSessionToken
        ? observed?.active
        : null;
  }

  void _recordInspectedMode(bool active) {
    final deviceId = _deviceId;
    if (deviceId == null) return;
    _inspectedBoardMode = (
      active: active,
      connectionToken: _linkToken,
      deviceId: deviceId,
    );
  }

  bool get selected => _selected;
  // The board setting compensates its physical half-turn. Artwork remains in
  // viewer coordinates on the phone, independent of hardware/player placement.
  bool get flipped => false;
  bool get hasMarks => _canvas.hasMarks;
  bool get pending =>
      _queued > _backgroundQueued ||
      _physicalStoreRepairing ||
      _physicalStoreRepairThroughRevision != 0 ||
      _gameEntryResetChoice != null ||
      _entering != null ||
      _exiting != null;
  bool get authorityResolving =>
      _entering != null || _gameEntryResetChoice != null;

  void setFavoriteColors(Iterable<int> colors) {
    final List<int> next = List<int>.of(colors);
    if (next.length != 8 ||
        next.any((color) => color < 0 || color > 0xffffff)) {
      throw ArgumentError('Invalid Litchcraft favorite colors');
    }
    if (listEquals(next, _favoriteColors)) return;
    _favoriteColors = next;
    documentSession.setFavoriteColors(next);
    ++_favoriteColorsRevision;
    _scheduleFavoritePaletteSync();
  }

  /// Waits for mode admission, adoption, or exit work already in flight.
  ///
  /// Connection UI uses this to keep its compact progress cue truthful when
  /// the controller's own connection observer started recovery first.
  Future<void> settleModeResolution() async {
    // A peer connection listener may have scheduled `_resumeConnection`, whose
    // first await admits its single-flight work on the next microtask.
    await Future<void>.value();
    while (!_disposed) {
      final Future<bool>? adopting = _adopting;
      final Future<void>? entering = _entering;
      final Future<void>? exiting = _exiting;
      if (adopting == null && entering == null && exiting == null) return;
      try {
        if (adopting != null) {
          await adopting;
        } else if (entering != null) {
          await entering;
        } else if (exiting != null) {
          await exiting;
        }
      } catch (_) {
        // The owning operation has already translated failure into status.
      }
    }
  }

  bool get advancedBoardAuthorityLive =>
      _advancedWire && _boardActive && _status == LitchcraftStatus.live;
  bool get _confirmedStaticOnlyBoard {
    final id = _deviceId;
    if (id == null) return false;
    final capabilities = bleService.deviceCapabilities(id);
    return capabilities.supportsLitchcraft &&
        !capabilities.supportsLitchcraftAnimation;
  }

  bool get usesFrameStoreDocument =>
      _localFrameStoreDocument ||
      documentSession.document.frames.length >
          litchcraftAnimationLegacyMaximumFrameCount;
  bool get preparingBoardArtwork =>
      _boardActive && _entryDraftWaitCancellation?.isCompleted == false;
  bool get boardArtworkUnresolved =>
      _frameStoreReadPending ||
      preparingBoardArtwork ||
      (_boardActive && _entering != null);
  bool get boardArtworkLoading {
    final read = _frameStoreSnapshotRead;
    return _status == LitchcraftStatus.live &&
        read != null &&
        read.generation == _generation &&
        read.selectionIntent == _selectionIntent &&
        read.linkToken == _linkToken &&
        read.sourceGeneration == documentSession.replacementGeneration;
  }

  bool get animationBoardAuthorityLive =>
      !usesFrameStoreDocument && advancedBoardAuthorityLive;
  bool get animationPlaying {
    if (!_boardActive) return false;
    if (usesFrameStoreDocument && _strokeFrame != null) return false;
    // A committed Store animation can keep running on the board while the
    // app's command/notification link is being repaired. Preserve that last
    // authoritative playback bit so the canvas, play control, and physical
    // board do not disagree just because transport status is temporarily
    // error/connecting.
    final storePlaying = _frameStorePlaying;
    if (storePlaying != null) return storePlaying;
    return _status == LitchcraftStatus.live &&
        animationBoardAuthorityLive &&
        _advancedPlaying;
  }

  bool get canSaveArtworkToBoard =>
      !usesFrameStoreDocument &&
      // Current shared-session firmware stores every artwork through Store.
      // A refused mode entry must not revive retired schema-3 uploads.
      _orderedSession == null &&
      _inactiveStorageSupported &&
      !_boardActive &&
      _documentClient != null &&
      _linkUsable &&
      bleService.isConnected;
  int get animationSelectedFrameIndex {
    if (usesFrameStoreDocument && _strokeFrame != null) return _strokeFrame!;
    if (usesFrameStoreDocument && animationPlaying) {
      return litchcraftAnimationFrameIndexAt(
        documentSession.document,
        animationElapsedMilliseconds,
      );
    }
    if (!animationBoardAuthorityLive) return documentSession.selectedFrameIndex;
    final sample = _advancedTimeline;
    if (sample == null || !sample.playing) return _advancedSelectedFrame;
    return litchcraftAnimationFrameIndexAt(
      documentSession.document,
      animationElapsedMilliseconds,
    );
  }

  int get animationElapsedMilliseconds {
    if (usesFrameStoreDocument) {
      final strokeFrame = _strokeFrame;
      if (strokeFrame != null) {
        return litchcraftAnimationFrameStartOrNull(
              documentSession.document,
              strokeFrame,
            ) ??
            0;
      }
      final sample = _advancedTimeline;
      if (sample != null) {
        return _playbackTimelineElapsedAt(sample, _advancedTimelineNow);
      }
      return animationPlaying
          ? _advancedTimelineNow - _frameStoreStartedAt
          : litchcraftAnimationFrameStartOrNull(
                  documentSession.document,
                  documentSession.selectedFrameIndex,
                ) ??
                0;
    }
    final sample = _advancedTimeline;
    if (!animationBoardAuthorityLive) return 0;
    if (sample == null) {
      // An optimistic atomic edit freezes the selected frame while its ACK is
      // pending. Rendering at zero would show frame zero of a multi-frame work.
      return litchcraftAnimationFrameStartOrNull(
            documentSession.document,
            _advancedSelectedFrame,
          ) ??
          0;
    }
    return _playbackTimelineElapsedAt(sample, _advancedTimelineNow);
  }

  int _playbackTimelineElapsedAt(LitchcraftV2TimelineSample sample, int now) {
    final elapsed = sample.extrapolatedElapsedAt(now);
    if (!sample.playing ||
        sample.transferSuspended ||
        _playbackClockSlewCorrectionMs == 0) {
      return elapsed;
    }
    final slewElapsed = (now - _playbackClockSlewStartedAt).clamp(
      0,
      _playbackClockSlewWindow.inMilliseconds,
    );
    return elapsed +
        (_playbackClockSlewCorrectionMs *
                slewElapsed /
                _playbackClockSlewWindow.inMilliseconds)
            .truncate();
  }

  int? get animationEffectElapsedMilliseconds {
    final sample = _link?.effectClock;
    final store =
        usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        _storeEffectClockBinding?.sampled == true &&
        _currentStoreEffectClockBinding(_storeEffectClockBinding!);
    if ((!animationBoardAuthorityLive && !store) ||
        sample == null ||
        sample.session != _documentSessionId) {
      return null;
    }
    final projected = identical(sample, _transferEffectClockSource)
        ? _transferEffectClock
        : null;
    return projected?.extrapolatedElapsedAt(_advancedTimelineNow) ??
        (_boardTransferSuspended
            ? sample.elapsedMilliseconds
            : sample.extrapolatedElapsedAt(_advancedTimelineNow));
  }

  void _projectTransferEffectClock(bool suspended) {
    final source = _link?.effectClock;
    if (_boardTransferSuspended == suspended &&
        identical(source, _transferEffectClockSource)) {
      return;
    }
    final previous = identical(source, _transferEffectClockSource)
        ? _transferEffectClock
        : null;
    final now = _advancedTimelineNow;
    if (source != null) {
      final clock = previous ?? source;
      // A fresh board sample obtained during the hold already has its exact
      // frozen phase. Otherwise re-anchor the existing projection at the edge.
      final elapsed = _boardTransferSuspended && previous == null
          ? clock.elapsedMilliseconds
          : clock.extrapolatedElapsedAt(now);
      _transferEffectClock = LitchcraftV2EffectClockSample(
        session: clock.session,
        revision: clock.revision,
        sampleId: clock.sampleId,
        elapsedMilliseconds: elapsed,
        receivedAtMonotonicMilliseconds: now,
        transferSuspended: suspended,
      );
    } else {
      _transferEffectClock = null;
    }
    _transferEffectClockSource = source;
    _boardTransferSuspended = suspended;
  }

  bool _currentStoreEffectClockBinding(_StoreEffectClockBinding binding) =>
      !_disposed &&
      identical(binding, _storeEffectClockBinding) &&
      binding.generation == _generation &&
      binding.source == documentSession.replacementGeneration &&
      binding.mode == _documentSessionId &&
      binding.linkToken == _linkToken &&
      identical(binding.link, _link) &&
      identical(binding.session, _orderedSession) &&
      binding.bootNonce == binding.session.boardBootNonce &&
      binding.session.isActivated &&
      _boardActive;

  void _ensureStoreEffectClock() {
    final link = _link;
    final session = _orderedSession;
    if (!usesFrameStoreDocument ||
        _status != LitchcraftStatus.live ||
        !_boardActive ||
        link == null ||
        session == null ||
        !session.isActivated ||
        documentSession.boardDocument == null ||
        _storeTransferInFlight ||
        storeReplacementUnresolved ||
        _optimisticDocumentEdits > 0 ||
        _storeCommonAuthority == null ||
        (_storeEffectClockBinding != null &&
            _currentStoreEffectClockBinding(_storeEffectClockBinding!))) {
      return;
    }
    final binding = _StoreEffectClockBinding(
      generation: _generation,
      source: documentSession.replacementGeneration,
      mode: _documentSessionId,
      linkToken: _linkToken,
      link: link,
      session: session,
    );
    _storeEffectClockBinding = binding;
    unawaited(
      _enqueue(() async {
        if (!_currentStoreEffectClockBinding(binding)) return;
        binding.requested = true;
        try {
          await link.inspectV2(session: binding.mode, refreshEffectClock: true);
          _receiveStoreEffectClock();
        } catch (_) {
          // The paired clock is optional presentation metadata. This source's
          // failed sample never starts polling or retires artwork authority.
        }
      }, background: true),
    );
  }

  void _receiveStoreEffectClock() {
    _projectTransferEffectClock(_boardTransferSuspended);
    final binding = _storeEffectClockBinding;
    if (binding == null ||
        !binding.requested ||
        binding.sampled ||
        !_currentStoreEffectClockBinding(binding) ||
        binding.link.effectClock?.session != binding.mode) {
      return;
    }
    binding.sampled = true;
    _notify();
  }

  bool get canUndo => usesFrameStoreDocument && _status == LitchcraftStatus.live
      ? (_advancedCanUndo ||
            (documentSession.nextUndoDocument != null &&
                _singleStoreHistoryFrame(
                      documentSession.document,
                      documentSession.nextUndoDocument!,
                    ) !=
                    null))
      : advancedBoardAuthorityLive
      ? _advancedCanUndo
      : usesFrameStoreDocument || documentSession.isAdvanced || _advancedWire
      ? documentSession.canUndo
      : _status == LitchcraftStatus.live
      ? _liveUndo
      : _undo.isNotEmpty;
  bool get canRedo => usesFrameStoreDocument && _status == LitchcraftStatus.live
      ? (_advancedCanRedo ||
            (documentSession.nextRedoDocument != null &&
                _singleStoreHistoryFrame(
                      documentSession.document,
                      documentSession.nextRedoDocument!,
                    ) !=
                    null))
      : advancedBoardAuthorityLive
      ? _advancedCanRedo
      : usesFrameStoreDocument || documentSession.isAdvanced || _advancedWire
      ? documentSession.canRedo
      : _status == LitchcraftStatus.live
      ? _liveRedo
      : _redo.isNotEmpty;
  LitchcraftStatus get status => _status;
  String? get errorMessage => _errorMessage;

  bool _automaticFrameStoreBenchmarkStarted = false;
  String? _automaticFrameStoreBenchmarkGuard;

  void _maybeRunAutomaticFrameStoreBenchmark() {
    if (_automaticFrameStoreBenchmark &&
        !_automaticFrameStoreBenchmarkStarted) {
      final guard = <Object?>[
        _status.name,
        _boardActive,
        bleService.isConnected,
        _link != null,
        _linkUsable,
        _errorMessage,
      ].join(':');
      if (_automaticFrameStoreBenchmarkGuard != guard) {
        _automaticFrameStoreBenchmarkGuard = guard;
        debugPrint('LITCH_FRAME_STORE_BENCHMARK_GUARD $guard');
      }
    }
    if (!_automaticFrameStoreBenchmark ||
        _automaticFrameStoreBenchmarkStarted ||
        _disposed ||
        _status != LitchcraftStatus.live ||
        !_boardActive ||
        !bleService.isConnected ||
        _link == null ||
        !_linkUsable) {
      return;
    }
    _automaticFrameStoreBenchmarkStarted = true;
    unawaited(() async {
      try {
        final result = switch (_automaticFrameStoreBenchmarkCorpus) {
          'repeated' => await runRepeated256FrameDiagnostic(),
          'sparse' => await runSparse256FrameDiagnostic(),
          'dense' => await runDense256FrameDiagnostic(),
          _ => throw ArgumentError.value(
            _automaticFrameStoreBenchmarkCorpus,
            'LITCH_FRAME_STORE_BENCHMARK_CORPUS',
          ),
        };
        debugPrint(
          'LITCH_FRAME_STORE_BENCHMARK ${jsonEncode(<String, Object?>{'ok': true, 'build_mode': kReleaseMode ? 'release' : 'debug', 'corpus': _automaticFrameStoreBenchmarkCorpus, 'frames': result.transfer.state.frameCount, 'body_bytes': result.transfer.bodyBytes, 'chunks': result.transfer.chunkPackets, 'writes': result.transfer.writes, 'maximum_packet_bytes': result.maximumPacketBytes, 'object_maximum_body_bytes': result.objectMaximumBodyBytes, 'semantic_window_chunks': result.semanticWindowChunks, 'setup_ms': result.setupMilliseconds, 'preparation_ms': result.preparationMilliseconds, 'begin_write_ms': result.transfer.beginWriteMilliseconds, 'batch_submission_ms': result.transfer.bulkWriteMilliseconds, 'checkpoint_ms': result.transfer.checkpointMilliseconds, 'commit_write_ms': result.transfer.commitWriteMilliseconds, 'receipt_wait_ms': result.transfer.receiptWaitMilliseconds, 'transfer_ms': result.transfer.elapsedMilliseconds, 'total_ms': result.totalElapsedMilliseconds, 'generation': result.transfer.state.generation, 'playing': result.transfer.state.playing})}',
        );
      } catch (error) {
        debugPrint(
          'LITCH_FRAME_STORE_BENCHMARK ${jsonEncode(<String, Object?>{'ok': false, 'build_mode': kReleaseMode ? 'release' : 'debug', 'error': error.runtimeType.toString(), 'message': error.toString()})}',
        );
      }
    }());
  }

  void _notify() {
    _ensureStoreEffectClock();
    // Store documents already own their local preview and confirmed board
    // bytes. A delayed board presentation cache must never write backwards
    // into that document after an ordered edit receipt.
    if (!_projectingDocument &&
        !usesFrameStoreDocument &&
        !(_confirmedStaticOnlyBoard && documentSession.isAdvanced)) {
      _projectingDocument = true;
      documentSession.replaceCanvas(_canvas, history: false);
      _projectingDocument = false;
    }
    if (_status == LitchcraftStatus.live &&
        _boardActive &&
        !_needsResync &&
        _snapshotDone == null &&
        _sameArtwork(_canvas, _board)) {
      _commonCanvas = _board;
      _commonBoardId = _deviceId;
    }
    _scheduleFavoritePaletteSync();
    _scheduleArtworkDelivery();
    _traceState();
    _maybeRunAutomaticFrameStoreBenchmark();
    if (!_disposed) notifyListeners();
  }

  void _traceState() {
    final String signature = <Object?>[
      _status.name,
      _selected,
      _boardActive,
      _needsResync,
      _snapshotDone != null,
      _frameStoreReadPending,
      _linkUsable,
      _linkDisposal != null,
      _recoveryAttempt,
      _generation,
      _linkToken,
    ].join(':');
    if (signature == _lastTraceState) return;
    _lastTraceState = signature;
    _appTrace?.record(
      'litchcraft.state',
      fields: <String, Object?>{
        'status': _status.name,
        'selected': _selected,
        'board_active': _boardActive,
        'needs_resync': _needsResync,
        'snapshot_pending': _snapshotDone != null,
        'frame_store_read_pending': _frameStoreReadPending,
        'link_usable': _linkUsable,
        'cleanup_pending': _linkDisposal != null,
        'generation': _generation,
        'connection_epoch': _linkToken,
        'recovery_attempt': _recoveryAttempt,
      },
    );
  }

  static bool _sameArtwork(LitchcraftCanvas left, LitchcraftCanvas right) =>
      listEquals(left.colors, right.colors);

  static int _documentTransportPacketBytes(int negotiatedPacketBytes) =>
      negotiatedPacketBytes
          .clamp(
            LitchcraftV2Protocol.minimumPacketBytes,
            LitchcraftV2Protocol.maximumPacketBytes,
          )
          .toInt();

  void _documentChanged() {
    if (_projectingDocument || _disposed) return;
    if (_observedSourceGeneration != documentSession.replacementGeneration) {
      _observedSourceGeneration = documentSession.replacementGeneration;
      _storeHistoryEcho = null;
      // A small replacement still belongs to the acknowledged live Store.
      // Keep that owner through this synchronous notification so palette and
      // other observers cannot queue compact artwork commands before Commit.
      _localFrameStoreDocument =
          (_boardActive &&
              _linkUsable &&
              (_localFrameStoreDocument || _frameStoreGeneration != 0)) ||
          documentSession.document.frames.length >
              litchcraftAnimationLegacyMaximumFrameCount;
      if (_localFrameStoreDocument) _advancedWire = false;
      _frameStoreReadPending = false;
      retireConflictBoardPreview();
      _pendingFrameStoreConflict = null;
      _pendingStaticCanvas = null;
      _clearPendingPaint();
      // Until board authority is acknowledged, entry owns only the mode and
      // link. A replacement draft cannot retire that admission; reconciliation
      // reads the latest document after admission and draft readiness settle.
      final retiringBoardWork = _entering == null || _boardActive;
      if (retiringBoardWork) ++_generation;
      _documentClient?.cancel();
      if (_documentSyncing || (_entering != null && retiringBoardWork)) {
        _status = LitchcraftStatus.offline;
      }
    }
    final effect = documentSession.localEffectEdit;
    final canvas = _pendingStoreCanvas;
    if (effect != null &&
        canvas != null &&
        _boardActive &&
        canvas.frame == effect.frame &&
        canvas.generation == _generation &&
        canvas.sourceGeneration == documentSession.replacementGeneration &&
        canvas.modeSession == _documentSessionId &&
        identical(canvas.session, _orderedSession)) {
      var effectEdits = canvas.effectEdits;
      for (var region = 0; region < 4; region++) {
        if (effect.mask & (1 << region) != 0) {
          effectEdits |= BigInt.one << (effect.square * 4 + region);
        }
      }
      _pendingStoreCanvas = (
        generation: canvas.generation,
        sourceGeneration: canvas.sourceGeneration,
        modeSession: canvas.modeSession,
        session: canvas.session,
        frame: canvas.frame,
        mask: canvas.mask,
        action: canvas.action,
        effectEdits: effectEdits,
        applied: canvas.applied,
        followingHistory: canvas.followingHistory,
        historyRevision: canvas.historyRevision,
        completed: canvas.completed,
        resume: canvas.resume,
      );
    }
    _projectingDocument = true;
    _canvas = documentSession.canvas.withTopology(_canvas.topology);
    _detached = _canvas;
    _offlineDiverged = true;
    if (documentSession.isAdvanced &&
        !usesFrameStoreDocument &&
        !_advancedWire &&
        _status == LitchcraftStatus.live) {
      _status = LitchcraftStatus.offline;
    }
    _notify();
    _projectingDocument = false;
  }

  void restoreDocument(LitchcraftAnimationDocument document) {
    _interruptedStoreReplacement = null;
    _pendingStoreSelectedFrame = null;
    documentSession.open(document);
    _frameStoreGeneration = 0;
    _advancedTimeline = null;
  }

  Future<void> replaceDocument(
    LitchcraftAnimationDocument document, {
    bool newCanvas = false,
  }) async {
    if (newCanvas &&
        !sameLitchcraftDocument(
          document,
          staticLitchcraftDocument(
            LitchcraftCanvas.blank(topology: document.topology),
          ),
        )) {
      throw ArgumentError.value(
        document,
        'document',
        'New requires a blank canvas',
      );
    }
    if (newCanvas) _retireOfflineReplacementLineage();
    _interruptedStoreReplacement = null;
    // Opening a source retires its pending choice metadata synchronously.
    // Retain only the fact that this acknowledged Store choice is being
    // replaced; the existing post-Commit fences still protect newer failures.
    final replacingStoreChoice =
        _pendingFrameStoreConflict != null &&
        (_status == LitchcraftStatus.conflict ||
            _currentConflictPreview(_pendingFrameStoreConflict!));
    final replacingLiveStore =
        !_confirmedStaticOnlyBoard &&
        _boardActive &&
        _linkUsable &&
        bleService.isConnected &&
        (usesFrameStoreDocument || _frameStoreGeneration != 0);
    final pauseCurrentPlayback = replacingLiveStore && animationPlaying;
    final compactNewCanvas =
        newCanvas &&
        replacingLiveStore &&
        (_status == LitchcraftStatus.live ||
            (_pendingFrameStoreConflict != null &&
                _currentConflictPreview(_pendingFrameStoreConflict!))) &&
        _orderedSession?.isActivated == true &&
        _orderedSession!.storeGuardEpochSupported &&
        _frameStoreGeneration != 0 &&
        documentSession.boardDocument != null &&
        !_storeTransferInFlight &&
        !_acceptedStoreCommitAwaitingRoot;
    // Idle New keeps its immediate guard. Queued New acquires its replacement
    // base after earlier issued actions settle, without reconstructing pixels
    // that this explicit whole-document action will replace.
    final newCanvasAuthority =
        compactNewCanvas &&
            _queued == 0 &&
            _optimisticDocumentEdits == 0 &&
            _canGuardStorePaint(_orderedSession!)
        ? _storeCommonAuthority
        : null;
    _deliberateDeliveryGeneration = documentSession.replacementGeneration + 1;
    _pendingStoreSelectedFrame = null;
    // The selected artwork is a stopped local preview immediately, including
    // when it replaces a playing multi-frame Store document.
    _frameStorePlaying = replacingLiveStore ? false : null;
    _advancedPlaying = false;
    _advancedTimeline = null;
    documentSession.open(document);
    if (replacingLiveStore) _localFrameStoreDocument = true;
    // A live Store stays the one board authority even when the new document
    // has one frame. Its inactive-bank Commit must own the display handoff.
    if (!replacingLiveStore) _frameStoreGeneration = 0;
    _advancedTimeline = null;
    final sourceGeneration = documentSession.replacementGeneration;
    if (_confirmedStaticOnlyBoard && documentSession.isAdvanced) {
      _localFrameStoreDocument = false;
      _advancedWire = false;
      _status = LitchcraftStatus.unsupported;
      _notify();
      return;
    }
    if (compactNewCanvas) {
      await _replaceStoreWithNewCanvas(document, newCanvasAuthority);
      return;
    }
    if (_entering != null &&
        (!_boardActive ||
            !_linkUsable ||
            _orderedSession?.isActivated != true)) {
      // Open publishes its preview immediately. Admission decides the Store
      // path even when a newer one-frame replacement supersedes this source.
      await _entering;
    }
    if (sourceGeneration != documentSession.replacementGeneration) return;
    if (_selected &&
        _status == LitchcraftStatus.error &&
        _inactiveStorageSupported &&
        _frameStoreGeneration == 0 &&
        !_storeTransferInFlight &&
        !_acceptedStoreCommitAwaitingRoot &&
        _pendingStoreCommit == null &&
        bleService.isConnected &&
        _deviceId != null &&
        bleService.deviceCapabilities(_deviceId!).supportsConnectionSession) {
      if (!await _prepareEmptyStoreReplacement(sourceGeneration)) return;
    }
    if (usesFrameStoreDocument) {
      _advancedWire = false;
      _advancedPlaying = false;
      _notify();
      if (_boardActive && _linkUsable && _orderedSession?.isActivated == true) {
        final generation = _generation;
        final link = _link;
        final entryConflict = _pendingFrameStoreConflict;
        final entryRootMissing = _storeRootMissing;
        final entryStatus = _status;
        final entryError = _errorMessage;
        await _transferFrameStore(
          frames: document.frames,
          playbackOnEntry: document.playbackOnEntry,
          replacementGeneration: sourceGeneration,
          pauseCurrentPlayback: pauseCurrentPlayback,
        );
        if (sourceGeneration == documentSession.replacementGeneration &&
            generation == _generation &&
            identical(link, _link) &&
            _selected &&
            _boardActive &&
            _linkUsable &&
            (entryRootMissing ||
                entryConflict != null ||
                replacingStoreChoice) &&
            _storeRootMissing == entryRootMissing &&
            identical(_pendingFrameStoreConflict, entryConflict) &&
            _status == entryStatus &&
            _errorMessage == entryError) {
          // Commit settles only this entry's recovery/source choice. A newer
          // failure or source still owns its visible recovery action.
          _storeRootMissing = false;
          retireConflictBoardPreview();
          _pendingFrameStoreConflict = null;
          _errorMessage = null;
          _status = LitchcraftStatus.live;
          _notify();
        }
      }
      return;
    }
    if (canSaveArtworkToBoard && _status == LitchcraftStatus.conflict) {
      _status = LitchcraftStatus.offline;
      _notify();
    }
    if (documentSession.isAdvanced || _advancedWire) {
      final sourceGeneration = documentSession.replacementGeneration;
      try {
        await _documentSyncFuture;
      } catch (_) {
        /* Retired source transfer. */
      }
      if (sourceGeneration != documentSession.replacementGeneration) return;
      if (_advancedWire && _boardActive) {
        await _enqueue(() async {
          if (sourceGeneration == documentSession.replacementGeneration &&
              _boardActive) {
            await _sendDocument();
          }
        });
      }
      return;
    }
    await replaceCanvas(documentSession.canvas);
  }

  Future<bool> _prepareEmptyStoreReplacement(int sourceGeneration) async {
    final generation = _generation;
    final selectionIntent = _selectionIntent;
    final deviceId = _deviceId;
    final connectionToken = bleService.connectionSessionToken;
    bool current() =>
        !_disposed &&
        _selected &&
        deviceId != null &&
        generation == _generation &&
        selectionIntent == _selectionIntent &&
        sourceGeneration == documentSession.replacementGeneration &&
        deviceId == _deviceId &&
        bleService.isConnected &&
        deviceId == bleService.connectedId &&
        connectionToken == bleService.connectionSessionToken;
    if (!current()) return false;
    // This explicit source now owns recovery. Keep its read-only proof on the
    // existing command tail instead of racing the scheduled automatic entry.
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    var ready = false;
    await _enqueue(() async {
      if (!current() || _pendingStoreCommit != null) return;
      await _bindLink(deviceId!, generation);
      if (!current() || _orderedSession?.isActivated != true) return;
      final link = _link;
      _clearCachedFrameStoreInspection();
      // The ordinary entry owner proves exact live mode, then reads Store
      // metadata. A newly valid root follows its existing reconciliation.
      await _synchronizeAdvancedEntry();
      if (!current() || !identical(link, _link)) return;
      final state = _cachedFrameStoreInspection;
      final residents = state?.residentListing;
      ready =
          _boardActive &&
          _linkUsable &&
          _orderedSession?.isActivated == true &&
          _cachedFrameStoreLinkToken == _linkToken &&
          state != null &&
          state.active &&
          state.session != 0 &&
          state.session == _sessionId &&
          !state.hasArtwork &&
          state.generation == 0 &&
          state.frameCount == 0 &&
          residents != null &&
          residents.session == state.session &&
          residents.generation == 0 &&
          residents.selectedRootSlot == null &&
          residents.residents.isEmpty &&
          _pendingStoreCommit == null;
    }, background: true);
    return ready && current();
  }

  Future<void> _replaceStoreWithNewCanvas(
    LitchcraftAnimationDocument document,
    _StoreAuthority? initialAuthority,
  ) async {
    final ordered = _orderedSession!;
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final before = documentSession.boardDocument!;
    final position = _newStoreEditPosition(changesLayout: true);
    _StoreEditResult? result;
    var applied = false;
    Object? failure;
    ++_optimisticDocumentEdits;
    _unconfirmedDocument = document;
    _unconfirmedDocumentBoardId = position.deviceId;
    _pauseStoreTimelineAtPhysicalFrame(0);
    try {
      await _enqueue(() async {
        bool current() =>
            _storePositionCurrent(position, allowConflict: true) &&
            _selected &&
            _linkUsable &&
            bleService.isConnected;
        if (!current()) throw const LitchcraftTransferCancelled();
        // Retired predecessor callbacks cannot establish New's common. State
        // supplies only a guard; the applied receipt proves the complete blank.
        _unconfirmedDocument = document;
        final expected =
            initialAuthority ?? await _requestFreshStoreAuthority(ordered);
        if (!current()) throw const LitchcraftTransferCancelled();
        if (expected == null ||
            expected.deviceId != position.deviceId ||
            expected.bootNonce != position.bootNonce ||
            expected.generation == 0 ||
            expected.revision == 0 ||
            expected.revision == 0xffffffff) {
          failure = const LitchcraftResyncRequired();
          return;
        }
        final physicalRevision = _physicalStoreEditRevision;
        final body = Uint8List(8);
        final bytes = ByteData.sublistView(body);
        bytes.setUint32(0, expected.generation, Endian.little);
        bytes.setUint32(4, expected.revision, Endian.little);
        final outcome = await ordered.sendOrdered(
          ConnectionSession.litchcraftStoreNewCanvasOpcode,
          body,
        );
        if (generation != _generation ||
            sourceGeneration != documentSession.replacementGeneration ||
            !current()) {
          throw const LitchcraftTransferCancelled();
        }
        final revision = expected.revision + 1;
        if (outcome.disposition != ConnectionSessionDisposition.applied ||
            outcome.domainRevision != revision) {
          failure = LitchcraftRejected(outcome.reason);
          return;
        }
        applied = true;
        final laterPhysicalEdit =
            (_storeAuthority?.revision ?? 0) > revision ||
            _physicalStoreEditRevision != physicalRevision;
        _storeCommonAuthority = (
          deviceId: expected.deviceId,
          bootNonce: expected.bootNonce,
          generation: expected.generation,
          revision: revision,
          dirty: true,
        );
        _frameStoreGeneration = expected.generation;
        documentSession.confirm(expected.deviceId, document);
        _deliberateDeliveryGeneration = -1;
        _documentDirty = false;
        _storeRootMissing = false;
        _frameStoreReadPending = false;
        if (!laterPhysicalEdit) {
          _physicalStoreRepairThroughRevision = 0;
          _physicalStoreRepairTimer?.cancel();
          _physicalStoreRepairTimer = null;
        }
        retireConflictBoardPreview();
        _pendingFrameStoreConflict = null;
        _status = LitchcraftStatus.live;
        _errorMessage = null;
        _forgetSelectedFrameStoreResident();
        _clearKnownAdvancedHistory();
        _clearCachedFrameStoreInspection();
        _storeHistoryEcho = null;
        // A Play or frame tap may already own a newer local preview while
        // this New receipt settles its document boundary.
        if (!animationTimelinePending) _frameStorePlaying = false;
        _canvas = documentSession.canvas.withTopology(_canvas.topology);
        _board = LitchcraftCanvas.blank(topology: document.topology);
        _detached = _canvas;
        _offlineDiverged = false;
        _unconfirmedDocument = null;
        if (laterPhysicalEdit) {
          _schedulePhysicalStoreRepair(
            throughRevision: _physicalStoreEditRevision,
          );
        }
        result = _storeConfirmedResult(
          before: before,
          document: document,
          selectedBefore: 0,
          selectedAfter: 0,
          acceptedRevision: revision,
        );
        _notify();
      });
      if (!applied) {
        final error = failure ?? const LitchcraftTransferCancelled();
        if (_storePositionCurrent(position, allowConflict: true)) {
          // A refusal is a source choice on the healthy connection, not an
          // accepted New. Retain the draft and reuse explicit recovery.
          retireConflictBoardPreview();
          _pendingFrameStoreConflict = _PendingFrameStoreConflict(
            session: ordered,
            authoritySession: litchcraftFrameStoreAuthoritySession(
              documentSession: _documentSessionId,
              modeSession: _sessionId,
            ),
            deviceId: position.deviceId,
            generation: position.generation,
            selectionIntent: _selectionIntent,
            linkToken: position.linkToken,
            sourceGeneration: position.sourceGeneration,
            bootNonce: position.bootNonce,
          );
          _errorMessage = error.toString();
          _status = LitchcraftStatus.conflict;
        }
        throw error;
      }
    } finally {
      if (identical(_unconfirmedDocument, document)) {
        _unconfirmedDocument = null;
      }
      _finishStoreEditPosition(position, result);
      --_optimisticDocumentEdits;
      _storePaintWaitingForAuthority = true;
      _resumeStorePaintIfReady(ordered);
      _notify();
    }
  }

  Future<LitchcraftAnimationDocument> coherentDocument() async {
    if (_advancedWire) {
      if (_boardActive && _selected && _status == LitchcraftStatus.live) {
        await revalidateSelectedBoard();
      } else if (_boardActive && _documentDirty) {
        await _synchronizeDocument();
      }
    } else if (!_localFrameStoreDocument && !documentSession.isAdvanced) {
      await coherentSnapshot();
    }
    return documentSession.document;
  }

  /// Sends up to 256 supplied frames through the real connection-owned
  /// schema-4 path. It is intentionally not surfaced in the UI yet: hardware
  /// timing must prove the transfer and playback path before the frame limit is
  /// raised for normal artwork editing.
  Future<LitchcraftFrameStoreDiagnosticResult>
  transferFrameStoreForDiagnostics({
    required List<LitchcraftAnimationFrame> frames,
    bool playbackOnEntry = true,
  }) => _transferFrameStore(frames: frames, playbackOnEntry: playbackOnEntry);

  LitchcraftFrameStoreClient _frameStoreClient(ConnectionSession session) =>
      createConnectionLitchcraftFrameStoreClient(
        session: session,
        monotonicMilliseconds: () => _advancedTimelineNow,
        responseTimeout: _frameStoreResponseTimeout,
        appTrace: _appTrace,
      );

  bool _sameFrameStoreResidentIdentity(
    LitchcraftFrameStoreResident left,
    LitchcraftFrameStoreResident right,
  ) =>
      left.rootSlot == right.rootSlot &&
      left.generation == right.generation &&
      left.bodyCrc32 == right.bodyCrc32 &&
      left.bodyBytes == right.bodyBytes &&
      left.frameCount == right.frameCount &&
      left.topology == right.topology &&
      left.active == right.active &&
      left.playbackOnEntry == right.playbackOnEntry &&
      left.predecessorDeltas == right.predecessorDeltas &&
      left.sparkleSeed == right.sparkleSeed &&
      listEquals(left.favoriteColors, right.favoriteColors);

  bool _sameFrameStoreContentSource(
    LitchcraftAnimationDocument left,
    LitchcraftAnimationDocument right,
  ) =>
      identical(left, right) ||
      (left.topology == right.topology &&
          left.playbackOnEntry == right.playbackOnEntry &&
          left.sparkleSeed == right.sparkleSeed &&
          listEquals(left.frames, right.frames));

  LitchcraftFrameStoreContentIdentity? _cachedFrameStoreContentIdentity(
    LitchcraftAnimationDocument document,
  ) {
    for (final cached in _frameStoreContentIdentities.reversed) {
      // Frames and their emitters are immutable. Reference equality avoids
      // encoding or traversing pixels, including a palette-only document copy.
      if (_sameFrameStoreContentSource(cached.document, document)) {
        return cached.identity;
      }
    }
    return null;
  }

  void _rememberFrameStoreContentIdentity(
    LitchcraftAnimationDocument document,
    LitchcraftFrameStoreContentIdentity identity,
  ) {
    _frameStoreContentIdentities.removeWhere(
      (cached) => _sameFrameStoreContentSource(cached.document, document),
    );
    _frameStoreContentIdentities.add((document: document, identity: identity));
    if (_frameStoreContentIdentities.length > 2) {
      _frameStoreContentIdentities.removeAt(0);
    }
  }

  _KnownFrameStoreResident? _knownFrameStoreResidentFor({
    required int session,
    required int linkToken,
    required LitchcraftAnimationDocument document,
  }) {
    for (final known in _knownFrameStoreResidents.reversed) {
      if (known.session == session &&
          known.linkToken == linkToken &&
          sameLitchcraftDocument(known.document, document)) {
        return known;
      }
    }
    return null;
  }

  void _rememberFrameStoreResident({
    required int session,
    required int linkToken,
    required LitchcraftAnimationDocument document,
    required LitchcraftFrameStoreResident resident,
  }) {
    _knownFrameStoreResidents.removeWhere(
      (known) =>
          known.session != session ||
          known.linkToken != linkToken ||
          sameLitchcraftDocument(known.document, document) ||
          _sameFrameStoreResidentIdentity(known.resident, resident),
    );
    _knownFrameStoreResidents.add(
      _KnownFrameStoreResident(
        session: session,
        linkToken: linkToken,
        document: document,
        resident: resident,
      ),
    );
    while (_knownFrameStoreResidents.length > 2) {
      _knownFrameStoreResidents.removeAt(0);
    }
  }

  void _reconcileKnownFrameStoreResidents({
    required int session,
    required int linkToken,
    required LitchcraftFrameStoreResidents state,
    LitchcraftAnimationDocument? document,
    LitchcraftFrameStoreTransfer? transfer,
    LitchcraftFrameStoreContentIdentity? contentIdentity,
  }) {
    _selectedFrameStoreRootSlot = state.selectedRootSlot;
    _knownFrameStoreResidents.removeWhere(
      (known) =>
          known.session != session ||
          known.linkToken != linkToken ||
          !state.residents.any(
            (resident) =>
                _sameFrameStoreResidentIdentity(known.resident, resident),
          ),
    );
    final identity = contentIdentity ?? transfer?.contentIdentity;
    if (document == null || identity == null) return;
    _rememberFrameStoreContentIdentity(document, identity);
    for (final resident in state.residents) {
      if (resident.matchesContent(identity) &&
          listEquals(resident.favoriteColors, document.favoriteColors)) {
        _rememberFrameStoreResident(
          session: session,
          linkToken: linkToken,
          document: document,
          resident: resident,
        );
        return;
      }
    }
  }

  void _reconcileCommittedFrameStoreResidents({
    required LitchcraftFrameStoreState state,
    required int linkToken,
    required LitchcraftAnimationDocument document,
    required LitchcraftFrameStoreTransfer transfer,
  }) {
    _clearCachedFrameStoreInspection();
    final residents = state.residentListing;
    if (residents == null ||
        residents.session != state.session ||
        residents.generation != state.generation) {
      _knownFrameStoreResidents.clear();
      _selectedFrameStoreRootSlot = null;
      return;
    }
    // Durable Commit already owns this proof. A second metadata request must
    // not hold subsequent Play or editing behind an optional reply.
    _reconcileKnownFrameStoreResidents(
      session: state.session,
      linkToken: linkToken,
      state: residents,
      document: document,
      transfer: transfer,
    );
  }

  void _forgetFrameStoreResident(_KnownFrameStoreResident known) {
    _knownFrameStoreResidents.removeWhere(
      (candidate) =>
          candidate.session == known.session &&
          candidate.linkToken == known.linkToken &&
          _sameFrameStoreResidentIdentity(candidate.resident, known.resident),
    );
  }

  void _forgetSelectedFrameStoreResident() {
    final selected = _selectedFrameStoreRootSlot;
    if (selected == null) {
      _knownFrameStoreResidents.clear();
    } else {
      _knownFrameStoreResidents.removeWhere(
        (known) => known.resident.rootSlot == selected,
      );
    }
  }

  bool _retainAcceptedArtworkForUnexpectedResident({
    required ConnectionSession session,
    required LitchcraftFrameStoreState state,
    required String deviceId,
    required LitchcraftAnimationDocument document,
  }) {
    final accepted = _knownFrameStoreResidentFor(
      session: state.session,
      linkToken: _linkToken,
      document: documentSession.document,
    );
    final observed = _knownFrameStoreResidentFor(
      session: state.session,
      linkToken: _linkToken,
      document: _storeArtworkWithLocalPalette(document),
    );
    if (accepted == null ||
        observed == null ||
        accepted.resident.rootSlot == observed.resident.rootSlot ||
        observed.resident.generation != state.generation ||
        state.selectedRootSlot != observed.resident.rootSlot) {
      return false;
    }
    // A coherent different known root is a source change, even when the app
    // has no unsaved edits. Preserve the artwork the user already selected.
    retireConflictBoardPreview();
    _pendingFrameStoreConflict = _PendingFrameStoreConflict(
      session: session,
      authoritySession: state.session,
      observedFrameCount: state.frameCount,
      deviceId: deviceId,
      generation: _generation,
      selectionIntent: _selectionIntent,
      linkToken: _linkToken,
      sourceGeneration: documentSession.replacementGeneration,
      bootNonce: session.boardBootNonce,
    );
    _frameStoreGeneration = state.generation;
    _frameStoreReadPending = false;
    _frameStorePlaying = false;
    _advancedTimeline = null;
    documentSession.retireBoardLineage();
    _storeCommonAuthority = null;
    _physicalStoreRepairThroughRevision = 0;
    _status = LitchcraftStatus.conflict;
    _notify();
    return true;
  }

  // Entry synchronization already paid for authoritative Store metadata.
  // Keep that metadata as a one-shot handoff to the next transfer so the
  // common "enter, then choose artwork" path does not inspect the board twice.
  // This contains metadata only; artwork bodies still require the explicit
  // board-source choice in the conflict path.
  void _cacheFrameStoreInspection({
    required LitchcraftFrameStoreState state,
    required LitchcraftFrameStoreResidents? residents,
    required int linkToken,
  }) {
    if (_disposed || linkToken != _linkToken) return;
    _cachedFrameStoreInspection = state;
    _cachedFrameStoreResidents = residents;
    _cachedFrameStoreLinkToken = linkToken;
  }

  void _clearCachedFrameStoreInspection() {
    _cachedFrameStoreInspection = null;
    _cachedFrameStoreResidents = null;
    _cachedFrameStoreLinkToken = 0;
  }

  LitchcraftFrameStoreTransferResult _residentSelectionResult(
    LitchcraftFrameStoreResidents selected,
    LitchcraftFrameStoreResident resident,
    int startedAt,
  ) {
    final elapsed = _advancedTimelineNow - startedAt;
    return LitchcraftFrameStoreTransferResult(
      state: LitchcraftFrameStoreState(
        session: selected.session,
        generation: selected.generation,
        requestId: selected.requestId,
        status: selected.status,
        active: resident.active,
        playing: false,
        hasArtwork: true,
        frameCount: resident.frameCount,
        selectedFrame: 0,
        topology: resident.topology,
      ),
      bodyBytes: 0,
      chunkPackets: 0,
      checkpoints: 0,
      writes: 1,
      beginWriteMilliseconds: 0,
      bulkWriteMilliseconds: 0,
      checkpointMilliseconds: 0,
      commitWriteMilliseconds: 0,
      receiptWaitMilliseconds: elapsed,
      elapsedMilliseconds: elapsed,
    );
  }

  Future<LitchcraftFrameStoreDiagnosticResult> _transferFrameStore({
    required List<LitchcraftAnimationFrame> frames,
    required bool playbackOnEntry,
    bool playWhenReady = false,
    int? replacementGeneration,
    bool pauseCurrentPlayback = false,
    bool finishCurrentStroke = false,
  }) async {
    final requestStartedAt = _advancedTimelineNow;
    final generation = _generation;
    final selectionIntent = _selectionIntent;
    final previousPlaying = _frameStorePlaying;
    bool superseded() =>
        replacementGeneration != null &&
        replacementGeneration != documentSession.replacementGeneration;
    final transferMarker = ++_storeTransferMarker;
    _storeEffectClockBinding = null;
    _storeTransferInFlight = true;
    _storePlaybackOnEntryRequested = playWhenReady;
    _queuedStorePlay = playWhenReady;
    _queuedStorePlayFailed = false;
    void clearTransferIntent() {
      if (transferMarker != _storeTransferMarker) return;
      _storeTransferInFlight = false;
      _storePlaybackOnEntryRequested = false;
      _notify();
    }

    final completion = Completer<LitchcraftFrameStoreDiagnosticResult>();
    // The queue drains before the diagnostic Future is returned. Attach its
    // error observer now so a prompt refusal cannot escape that caller.
    void ignoreDiagnosticFailure(Object _, StackTrace stackTrace) {}
    unawaited(
      completion.future.then<void>((_) {}, onError: ignoreDiagnosticFailure),
    );
    var ran = false;
    var diagnosticMaximumPacketBytes = 0;
    var diagnosticObjectMaximumBodyBytes = 0;
    var diagnosticSemanticWindowChunks = 0;
    await _enqueue(() async {
      ran = true;
      if (superseded() || selectionIntent != _selectionIntent || !_selected) {
        clearTransferIntent();
        completion.completeError(const LitchcraftTransferCancelled());
        return;
      }
      final link = _link;
      final deviceId = _deviceId;
      final linkToken = _linkToken;
      final caps = deviceId == null
          ? null
          : bleService.deviceCapabilities(deviceId);
      final authoritySession = litchcraftFrameStoreAuthoritySession(
        documentSession: _documentSessionId,
        modeSession: _sessionId,
      );
      if (_disposed ||
          link == null ||
          deviceId == null ||
          !_linkUsable ||
          !bleService.isConnected ||
          !_boardActive ||
          authoritySession == 0) {
        completion.completeError(
          StateError(
            'An active schema-4 Litchcraft session is required '
            '(disposed=$_disposed link=${link != null} device=${deviceId != null} '
            'linkUsable=$_linkUsable connected=${bleService.isConnected} '
            'advancedWire=$_advancedWire boardActive=$_boardActive '
            'documentSession=$_documentSessionId status=$_status '
            'capsExplicit=${caps?.explicitlyReported} '
            'capsContract=${caps?.contractVersion} '
            'capsMask=${caps?.capabilityMask} '
            'capsAnimation=${caps?.supportsLitchcraftAnimation})',
          ),
        );
        if (generation == _generation) _frameStorePlaying = previousPlaying;
        clearTransferIntent();
        return;
      }

      final startedAt = _advancedTimelineNow;
      var capabilityConfirmed = false;
      LitchcraftAnimationDocument? pendingDocument;
      LitchcraftFrameStoreTransfer? pendingTransfer;
      LitchcraftFrameStoreState? observedStoreState;
      var storeAuthorityLost = false;
      ConnectionSession? transferSession;
      _InterruptedStoreReplacement? replacementBase;
      Object? fullSyncAwaitingRoot;
      final playbackIntentAtSubmission = _playbackIntentGeneration;
      var physicalRevisionAtSubmission = _physicalStoreEditRevision;
      try {
        // An accepted Commit owns the Store root even while its receipt is
        // delayed. Resolve that authority before inspecting or selecting a
        // resident for the newer local document.
        final pendingRoot = _pendingStoreCommit;
        if (pendingRoot != null) {
          await pendingRoot.completion;
          if (superseded() ||
              _disposed ||
              generation != _generation ||
              linkToken != _linkToken ||
              !identical(link, _link)) {
            throw const LitchcraftTransferCancelled();
          }
        }
        final objectSession = transferSession = _orderedSession;
        if (objectSession == null ||
            !objectSession.isActivated ||
            objectSession.objectMaximumBody < 5) {
          throw StateError('The shared Store Object stream is unavailable');
        }
        if (finishCurrentStroke) {
          // An explicit Use app choice supersedes an interrupted stroke, but
          // never its pixels. The existing Tool action closes that boundary
          // without repainting a resting footprint or weakening Store fences.
          final outcome = await objectSession.sendOrdered(
            ConnectionSession.litchcraftToolOpcode,
            Uint8List.fromList(<int>[
              _eraser ? 1 : 0,
              _targetMask,
              ...LitchcraftProtocol.rgb(_color),
            ]),
          );
          if (outcome.disposition != ConnectionSessionDisposition.applied) {
            throw LitchcraftRejected(outcome.reason);
          }
          if (superseded() ||
              generation != _generation ||
              linkToken != _linkToken ||
              !identical(link, _link) ||
              !_boardActive) {
            throw const LitchcraftTransferCancelled();
          }
          _pendingStoreStrokeEnd = null;
        }
        if (pauseCurrentPlayback && _frameStoreGeneration != 0) {
          // Stop the current board document on the existing interactive lane
          // before metadata discovery or body staging can delay that action.
          final pause = Uint8List(6);
          pause[1] = _advancedSelectedFrame;
          ByteData.sublistView(
            pause,
          ).setUint32(2, _frameStoreGeneration, Endian.little);
          final outcome = await objectSession.sendOrdered(
            ConnectionSession.litchcraftTimelineOpcode,
            pause,
          );
          if (outcome.disposition != ConnectionSessionDisposition.applied) {
            throw LitchcraftRejected(outcome.reason);
          }
          if (superseded() ||
              generation != _generation ||
              linkToken != _linkToken ||
              !identical(link, _link) ||
              !_boardActive) {
            throw const LitchcraftTransferCancelled();
          }
        }
        // Size Store packets to fit one first-fragment Object on high-MTU
        // shared links. This avoids splitting a 244-byte domain packet into
        // two radio writes just because the Object carries its length header.
        final negotiatedPacketBytes = objectSession.objectMaximumBody
            .clamp(126, ConnectionLitchcraftCodec.maximumPacketBytes)
            .toInt();
        final singleObjectPacketBytes =
            ConnectionLitchcraftCodec.maximumSingleObjectPacketBytes(
              maximumBodyBytes: objectSession.objectMaximumBody,
            );
        final maximumPacketBytes =
            (singleObjectPacketBytes >= 126
                    ? singleObjectPacketBytes
                    : negotiatedPacketBytes)
                .clamp(126, ConnectionLitchcraftCodec.maximumPacketBytes)
                .toInt();
        diagnosticMaximumPacketBytes = maximumPacketBytes;
        diagnosticObjectMaximumBodyBytes = objectSession.objectMaximumBody;
        diagnosticSemanticWindowChunks =
            LitchcraftFrameStoreClient.semanticWindowChunksFor(
              maximumPacketBytes: maximumPacketBytes,
              sharedSession: true,
            );
        final client = _frameStoreClient(objectSession);
        final source = documentSession.document;
        final requestedDocument =
            identical(frames, source.frames) &&
                playbackOnEntry == source.playbackOnEntry
            ? source
            : LitchcraftAnimationDocument(
                frames: frames,
                topology: source.topology,
                favoriteColors: source.favoriteColors,
                playbackOnEntry: playbackOnEntry,
                sparkleSeed: source.sparkleSeed,
              );
        pendingDocument = requestedDocument;
        var preparationStartedAt = startedAt;
        var preparedAt = startedAt;
        LitchcraftFrameStoreTransferResult? sent;
        var residentHit = false;
        var residentCacheHit = false;
        var selectionRoundTrips = 0;
        var canonicalBytes = 0;
        var canonicalCrc32 = 0;
        final known = _knownFrameStoreResidentFor(
          session: authoritySession,
          linkToken: linkToken,
          document: requestedDocument,
        );
        if (known != null) {
          canonicalBytes = known.resident.bodyBytes;
          canonicalCrc32 = known.resident.bodyCrc32;
          capabilityConfirmed = true;
          final selectionStartedAt = _advancedTimelineNow;
          try {
            if (superseded()) throw const LitchcraftTransferCancelled();
            final selected = await client.selectResident(
              session: authoritySession,
              request: _requestId = (_requestId % 0xffff) + 1,
              resident: known.resident,
            );
            LitchcraftFrameStoreResident? selectedResident;
            if (selected.session == authoritySession) {
              for (final candidate in selected.residents) {
                if (candidate.rootSlot == selected.selectedRootSlot) {
                  selectedResident = candidate;
                  break;
                }
              }
            }
            if (selectedResident == null ||
                !_sameFrameStoreResidentIdentity(
                  selectedResident,
                  known.resident,
                )) {
              throw const FormatException(
                'Selected frame-store resident identity changed',
              );
            }
            _reconcileKnownFrameStoreResidents(
              session: authoritySession,
              linkToken: linkToken,
              state: selected,
            );
            residentHit = true;
            residentCacheHit = true;
            selectionRoundTrips = 1;
            sent = _residentSelectionResult(
              selected,
              selectedResident,
              selectionStartedAt,
            );
          } on LitchcraftRejected catch (error) {
            if (error.status == 1) rethrow;
            _forgetFrameStoreResident(known);
            _appTrace?.record(
              'litchcraft.frame_store_resident_cache_stale',
              fields: <String, Object?>{'status': error.status},
            );
          } on FormatException catch (error) {
            _forgetFrameStoreResident(known);
            _appTrace?.record(
              'litchcraft.frame_store_resident_cache_stale',
              fields: <String, Object?>{'kind': error.runtimeType.toString()},
            );
          }
        }
        if (!residentCacheHit) {
          late LitchcraftFrameStoreState inspection;
          LitchcraftFrameStoreResidents? residents;
          final cachedInspection = _cachedFrameStoreInspection;
          final cachedResidents = _cachedFrameStoreResidents;
          final useCachedInspection =
              cachedInspection != null &&
              _cachedFrameStoreLinkToken == linkToken &&
              cachedInspection.session == authoritySession &&
              cachedInspection.generation == _frameStoreGeneration &&
              cachedInspection.generation ==
                  (_storeAuthority?.generation ?? _frameStoreGeneration);
          // An initial metadata timeout can leave the shared session healthy.
          // Retain only our verified selected predecessor for the existing
          // recovery's fresh Inspect; this cannot authorize adoption by itself.
          final previous = documentSession.boardDocument;
          final common = _storeCommonAuthority;
          final known = previous == null
              ? null
              : _knownFrameStoreResidentFor(
                  session: authoritySession,
                  linkToken: linkToken,
                  document: previous,
                );
          if (replacementGeneration != null &&
              replacementGeneration == _deliberateDeliveryGeneration &&
              known != null &&
              known.resident.rootSlot == _selectedFrameStoreRootSlot &&
              known.resident.generation == _frameStoreGeneration &&
              common?.deviceId == deviceId &&
              common?.bootNonce == objectSession.boardBootNonce &&
              common?.generation == known.resident.generation) {
            replacementBase = _InterruptedStoreReplacement(
              boardId: deviceId,
              bootNonce: objectSession.boardBootNonce,
              sourceGeneration: replacementGeneration,
              base: known.resident,
            );
          }
          if (useCachedInspection) {
            inspection = cachedInspection;
            residents = cachedResidents ?? inspection.residentListing;
            _clearCachedFrameStoreInspection();
          } else {
            inspection = await client.inspect(
              session: authoritySession,
              request: _requestId = (_requestId % 0xffff) + 1,
            );
            residents = inspection.residentListing;
          }
          if (inspection.session == 0 ||
              inspection.session != authoritySession ||
              !inspection.active) {
            storeAuthorityLost = true;
            throw StateError('Board frame-store authority is not active');
          }
          observedStoreState = inspection;
          capabilityConfirmed = true;
          preparationStartedAt = _advancedTimelineNow;
          final transfer = LitchcraftFrameStoreProtocol.prepare(
            session: inspection.session,
            transfer: _requestId = (_requestId % 0xffff) + 1,
            expectedGeneration: inspection.generation,
            enableRgbWire: inspection.rgbWireCapable,
            topology: requestedDocument.topology.id,
            active: true,
            playbackOnEntry: requestedDocument.playbackOnEntry,
            sparkleSeed: requestedDocument.sparkleSeed,
            favoriteColors: requestedDocument.favoriteColors,
            frames: requestedDocument.frames,
          );
          pendingTransfer = transfer;
          preparedAt = _advancedTimelineNow;
          canonicalBytes = transfer.canonicalBodyBytes;
          canonicalCrc32 = transfer.canonicalBodyCrc32;
          final listedResidents = residents;
          if (listedResidents == null) {
            throw const FormatException(
              'Capability 7 Store Inspect omitted resident listing',
            );
          }
          if (listedResidents.session != inspection.session) {
            throw const FormatException(
              'Frame-store residents session changed',
            );
          }
          _reconcileKnownFrameStoreResidents(
            session: inspection.session,
            linkToken: linkToken,
            state: listedResidents,
            document: requestedDocument,
            transfer: transfer,
          );
          if (replacementGeneration != null &&
              replacementGeneration == _deliberateDeliveryGeneration) {
            for (final resident in listedResidents.residents) {
              if (resident.rootSlot == listedResidents.selectedRootSlot &&
                  resident.generation == inspection.generation) {
                replacementBase = _InterruptedStoreReplacement(
                  boardId: deviceId,
                  bootNonce: objectSession.boardBootNonce,
                  sourceGeneration: replacementGeneration,
                  base: resident,
                );
                break;
              }
            }
          }
          final matchingResidents = listedResidents.residents.where(
            (resident) => resident.matches(transfer),
          );
          if (matchingResidents.isNotEmpty) {
            final resident = matchingResidents.first;
            final selectionStartedAt = _advancedTimelineNow;
            if (superseded()) throw const LitchcraftTransferCancelled();
            final selected = await client.selectResident(
              session: inspection.session,
              request: _requestId = (_requestId % 0xffff) + 1,
              resident: resident,
            );
            LitchcraftFrameStoreResident? selectedResident;
            if (selected.session == inspection.session) {
              for (final candidate in selected.residents) {
                if (candidate.rootSlot == selected.selectedRootSlot) {
                  selectedResident = candidate;
                  break;
                }
              }
            }
            if (selectedResident == null ||
                !selectedResident.matches(transfer)) {
              throw const FormatException(
                'Selected frame-store resident identity changed',
              );
            }
            _reconcileKnownFrameStoreResidents(
              session: inspection.session,
              linkToken: linkToken,
              state: selected,
              document: requestedDocument,
              transfer: transfer,
            );
            residentHit = true;
            selectionRoundTrips = 1;
            sent = _residentSelectionResult(
              selected,
              selectedResident,
              selectionStartedAt,
            );
          } else {
            physicalRevisionAtSubmission = _physicalStoreEditRevision;
            _storeReplacementAwaitingAdmission =
                _frameStoreGeneration != 0 &&
                documentSession.boardDocument != null &&
                !sameLitchcraftDocument(
                  requestedDocument,
                  documentSession.boardDocument!,
                );
            final fullSync = Object();
            if (frames.length > 1) {
              _startFullAnimationSync(
                fullSync,
                LitchcraftFullSyncDirection.sendingToBoard,
              );
            }
            final transferRetired = Completer<void>();
            _frameStoreTransferRetirement = transferRetired;
            void retireTransferIfObsolete() {
              if (!transferRetired.isCompleted &&
                  (_disposed ||
                      superseded() ||
                      generation != _generation ||
                      linkToken != _linkToken ||
                      !identical(link, _link) ||
                      !_boardActive ||
                      !bleService.isConnected)) {
                transferRetired.complete();
              }
            }

            documentSession.addListener(retireTransferIfObsolete);
            final transferConnection = bleService.connectionStream.listen((_) {
              retireTransferIfObsolete();
            });
            retireTransferIfObsolete();
            try {
              sent = await client.send(
                transfer,
                cancellationFuture: transferRetired.future,
                maximumPacketBytes: maximumPacketBytes,
                onProgress: frames.length > 1
                    ? (completed, total) =>
                          _updateFullAnimationSync(fullSync, completed, total)
                    : null,
                isCancelled: () =>
                    _disposed ||
                    superseded() ||
                    generation != _generation ||
                    linkToken != _linkToken ||
                    !identical(link, _link) ||
                    !_boardActive,
              );
            } on LitchcraftFrameStoreCommitPending {
              if (frames.length > 1) {
                _updateFullAnimationSync(fullSync, 1, 1);
                fullSyncAwaitingRoot = fullSync;
              }
              rethrow;
            } finally {
              if (identical(_frameStoreTransferRetirement, transferRetired)) {
                _frameStoreTransferRetirement = null;
              }
              documentSession.removeListener(retireTransferIfObsolete);
              await transferConnection.cancel();
              if (fullSyncAwaitingRoot == null) {
                _finishFullAnimationSync(fullSync);
              }
            }
          }
        }
        final completedTransfer = sent;
        _storeReplacementAwaitingAdmission = false;
        clearTransferIntent();
        if (completedTransfer == null) {
          throw StateError('Frame-store delivery produced no result');
        }
        if (_disposed ||
            generation != _generation ||
            linkToken != _linkToken ||
            !identical(link, _link)) {
          throw const LitchcraftTransferCancelled();
        }
        if (!residentHit) {
          _reconcileCommittedFrameStoreResidents(
            state: completedTransfer.state,
            linkToken: linkToken,
            document: requestedDocument,
            transfer: pendingTransfer!,
          );
        }
        _frameStorePlaying = completedTransfer.state.playing;
        _frameStoreGeneration = completedTransfer.state.generation;
        documentSession.confirm(deviceId, requestedDocument);
        _reconcileStoreLocalIntentAfterCommit(
          requestedDocument,
          replacementGeneration,
        );
        _storeCommonBindingGeneration = completedTransfer.state.generation;
        _bindStoreCommonAuthorityIfReady();
        unawaited(
          objectSession
              .requestState(
                code: ConnectionSession.litchcraftStoreAuthorityStateCode,
              )
              .catchError((Object _) {}),
        );
        unawaited(
          objectSession
              .requestState(
                code: ConnectionSession.litchcraftStoreEditStateCode,
              )
              .catchError((Object _) {}),
        );
        _settleStoreSelectionAfterCommit(completedTransfer.state.selectedFrame);
        _resumeStorePaintIfReady(objectSession);
        _advancedTimeline = LitchcraftV2TimelineSample(
          session: completedTransfer.state.session,
          revision: completedTransfer.state.generation,
          sampleId: completedTransfer.state.generation,
          playing: completedTransfer.state.playing,
          selectedFrame: documentSession.selectedFrameIndex,
          elapsedMilliseconds:
              litchcraftAnimationFrameStartOrNull(
                documentSession.document,
                documentSession.selectedFrameIndex,
              ) ??
              0,
          receivedAtMonotonicMilliseconds: _advancedTimelineNow,
        );
        if (_queuedStorePlay) {
          unawaited(_playQueuedStore(ordered: objectSession));
        }
        final finishedAt = _advancedTimelineNow;
        final result = LitchcraftFrameStoreDiagnosticResult(
          setupMilliseconds: preparedAt - startedAt,
          preparationMilliseconds: preparedAt - preparationStartedAt,
          totalElapsedMilliseconds: finishedAt - startedAt,
          transfer: completedTransfer,
          maximumPacketBytes: diagnosticMaximumPacketBytes,
          objectMaximumBodyBytes: diagnosticObjectMaximumBodyBytes,
          semanticWindowChunks: diagnosticSemanticWindowChunks,
        );
        final submissionSummary = client.objectSubmissionSummary?.call();
        if (!residentHit &&
            submissionSummary != null &&
            submissionSummary.request == completedTransfer.state.requestId) {
          _appTrace?.record(
            'litchcraft.object_transfer',
            fields: {
              'accepted': true,
              'life': objectSession.clientNonce,
              'request': completedTransfer.state.requestId,
              'submissions': submissionSummary.submissions,
              'objects': submissionSummary.objects,
              'session': completedTransfer.state.session,
              'generation': completedTransfer.state.generation,
            },
          );
        }
        _appTrace?.record(
          'litchcraft.frame_store_delivery',
          fields: <String, Object?>{
            'accepted': true,
            'resident_hit': residentHit,
            'resident_cache_hit': residentCacheHit,
            'selection_round_trips': selectionRoundTrips,
            'frames': frames.length,
            'body_bytes': completedTransfer.bodyBytes,
            'chunks': completedTransfer.chunkPackets,
            'checkpoints': completedTransfer.checkpoints,
            'writes': completedTransfer.writes,
            'maximum_packet_bytes': diagnosticMaximumPacketBytes,
            'object_maximum_body_bytes': diagnosticObjectMaximumBodyBytes,
            'semantic_window_chunks': diagnosticSemanticWindowChunks,
            'session': completedTransfer.state.session,
            'generation': completedTransfer.state.generation,
            'canonical_bytes': canonicalBytes,
            'canonical_crc32': canonicalCrc32,
          },
        );
        _appTrace?.record(
          'litchcraft.frame_store_timing',
          fields: <String, Object?>{
            'frames': frames.length,
            'queue_wait_ms': startedAt - requestStartedAt,
            'setup_ms': result.setupMilliseconds,
            'preparation_ms': result.preparationMilliseconds,
            'begin_write_ms': completedTransfer.beginWriteMilliseconds,
            'bulk_write_ms': completedTransfer.bulkWriteMilliseconds,
            'checkpoint_ms': completedTransfer.checkpointMilliseconds,
            'commit_write_ms': completedTransfer.commitWriteMilliseconds,
            'receipt_wait_ms': completedTransfer.receiptWaitMilliseconds,
            'transfer_ms': completedTransfer.elapsedMilliseconds,
            'total_ms': result.totalElapsedMilliseconds,
            'request_total_ms': finishedAt - requestStartedAt,
            'generation': completedTransfer.state.generation,
            'playing': completedTransfer.state.playing,
            'session': completedTransfer.state.session,
          },
        );
        debugPrint(
          'LITCH_FRAME_STORE_BENCHMARK '
          'body_bytes=${completedTransfer.bodyBytes} '
          'chunks=${completedTransfer.chunkPackets} '
          'packet_bytes=$diagnosticMaximumPacketBytes '
          'object_body=$diagnosticObjectMaximumBodyBytes '
          'window=$diagnosticSemanticWindowChunks '
          'transfer_ms=${completedTransfer.elapsedMilliseconds} '
          'receipt_ms=${completedTransfer.receiptWaitMilliseconds} '
          'total_ms=${result.totalElapsedMilliseconds} '
          'resident_hit=$residentHit',
        );
        completion.complete(result);
      } catch (error, stackTrace) {
        _storeReplacementAwaitingAdmission = false;
        clearTransferIntent();
        if (error is LitchcraftFrameStoreCommitPending) {
          if (_disposed ||
              generation != _generation ||
              linkToken != _linkToken ||
              !identical(link, _link) ||
              deviceId != _deviceId ||
              authoritySession !=
                  litchcraftFrameStoreAuthoritySession(
                    documentSession: _documentSessionId,
                    modeSession: _sessionId,
                  )) {
            if (fullSyncAwaitingRoot != null) {
              _finishFullAnimationSync(fullSyncAwaitingRoot);
            }
            unawaited(error.cancel());
            completion.completeError(const LitchcraftTransferCancelled());
            return;
          }
          final acceptedDocument = pendingDocument!;
          _pendingStoreCommit = error;
          _pendingStoreFullSync = fullSyncAwaitingRoot;
          _acceptedStoreCommitAwaitingRoot = true;
          void clearPendingIfCurrent() {
            if (!identical(_pendingStoreCommit, error)) {
              return;
            }
            _pendingStoreCommit = null;
            _acceptedStoreCommitAwaitingRoot = false;
            final fullSync = _pendingStoreFullSync;
            _pendingStoreFullSync = null;
            if (fullSync != null) _finishFullAnimationSync(fullSync);
          }

          unawaited(
            error.completion.then(
              (lateTransfer) {
                clearPendingIfCurrent();
                if (_disposed ||
                    generation != _generation ||
                    linkToken != _linkToken ||
                    !identical(link, _link) ||
                    deviceId != _deviceId) {
                  if (!completion.isCompleted) {
                    completion.completeError(
                      const LitchcraftTransferCancelled(),
                    );
                  }
                  return;
                }
                _reconcileCommittedFrameStoreResidents(
                  state: lateTransfer.state,
                  linkToken: linkToken,
                  document: acceptedDocument,
                  transfer: pendingTransfer!,
                );
                final physicalPaintAfterSubmission =
                    _physicalStoreEditRevision != physicalRevisionAtSubmission;
                final newerPlaybackIntent =
                    _playbackIntentGeneration != playbackIntentAtSubmission;
                if (!physicalPaintAfterSubmission && !newerPlaybackIntent) {
                  _frameStorePlaying = lateTransfer.state.playing;
                }
                _frameStoreGeneration = lateTransfer.state.generation;
                documentSession.confirm(deviceId, acceptedDocument);
                _reconcileStoreLocalIntentAfterCommit(
                  acceptedDocument,
                  replacementGeneration,
                );
                _storeCommonBindingGeneration = lateTransfer.state.generation;
                _bindStoreCommonAuthorityIfReady();
                final pendingOrdered = _orderedSession;
                if (pendingOrdered != null) {
                  unawaited(
                    pendingOrdered
                        .requestState(
                          code: ConnectionSession
                              .litchcraftStoreAuthorityStateCode,
                        )
                        .catchError((Object _) {}),
                  );
                  _resumeStorePaintIfReady(pendingOrdered);
                }
                if (!physicalPaintAfterSubmission && !newerPlaybackIntent) {
                  _settleStoreSelectionAfterCommit(
                    lateTransfer.state.selectedFrame,
                  );
                }
                final selectedFrame = documentSession.selectedFrameIndex;
                if (!newerPlaybackIntent) {
                  _advancedTimeline = LitchcraftV2TimelineSample(
                    session: lateTransfer.state.session,
                    revision: lateTransfer.state.generation,
                    sampleId: lateTransfer.state.generation,
                    playing:
                        !physicalPaintAfterSubmission &&
                        lateTransfer.state.playing,
                    selectedFrame: selectedFrame,
                    elapsedMilliseconds:
                        litchcraftAnimationFrameStartOrNull(
                          acceptedDocument,
                          selectedFrame,
                        ) ??
                        0,
                    receivedAtMonotonicMilliseconds: _advancedTimelineNow,
                  );
                }
                if (physicalPaintAfterSubmission) {
                  _schedulePhysicalStoreRepair(
                    throughRevision: _physicalStoreEditRevision,
                  );
                }
                if (_queuedStorePlay && pendingOrdered != null) {
                  unawaited(_playQueuedStore(ordered: pendingOrdered));
                }
                _notify();
                if (!completion.isCompleted) {
                  completion.complete(
                    LitchcraftFrameStoreDiagnosticResult(
                      setupMilliseconds: 0,
                      preparationMilliseconds: 0,
                      totalElapsedMilliseconds:
                          _advancedTimelineNow - startedAt,
                      transfer: lateTransfer,
                      maximumPacketBytes: diagnosticMaximumPacketBytes,
                      objectMaximumBodyBytes: diagnosticObjectMaximumBodyBytes,
                      semanticWindowChunks: diagnosticSemanticWindowChunks,
                    ),
                  );
                }
              },
              onError: (Object failure, StackTrace failureStack) {
                clearPendingIfCurrent();
                if (!_disposed &&
                    generation == _generation &&
                    linkToken == _linkToken &&
                    identical(link, _link)) {
                  _frameStorePlaying = null;
                  _transportFailed(failure);
                }
                if (!completion.isCompleted) {
                  completion.completeError(failure, failureStack);
                }
              },
            ),
          );
          return;
        }
        // Keep only the old-root witness of an explicit interrupted Open.
        // A new clean Inspect must prove that exact root before any retry.
        if (!_disposed &&
            _selected &&
            selectionIntent == _selectionIntent &&
            !superseded() &&
            replacementGeneration == _deliberateDeliveryGeneration &&
            (transferSession?.isActivated == false ||
                error is TimeoutException) &&
            error is! LitchcraftFrameStoreCommitPending &&
            replacementBase != null) {
          final previous = _interruptedStoreReplacement;
          // All retries belong to the same caller, including a repair that
          // encounters a different transport error before eventual success.
          replacementBase.failure =
              previous != null &&
                  previous.boardId == replacementBase.boardId &&
                  previous.bootNonce == replacementBase.bootNonce &&
                  previous.sourceGeneration == replacementBase.sourceGeneration
              ? previous.failure ?? error
              : error;
          _interruptedStoreReplacement = replacementBase;
        }
        if (transferMarker == _storeTransferMarker) {
          _queuedStorePlay = false;
        }
        if (!_disposed && generation == _generation && !superseded()) {
          // Cancel Busy after an accepted Commit cannot restore the old board
          // artwork. A late durable receipt may still settle this transfer.
          if (error is! LitchcraftFrameStoreCommitPending) {
            _frameStorePlaying = previousPlaying;
          }
          // Unsupported firmware requires a user action. Transport failures
          // remain diagnostic evidence; the last accepted artwork survives.
          if (!capabilityConfirmed &&
              error is LitchcraftRejected &&
              (error.status == 3 || error.status == 4)) {
            _status = LitchcraftStatus.unsupported;
          } else if (replacementGeneration != null &&
              storeReplacementUnresolved &&
              error is! LitchcraftTransferCancelled &&
              error is! LitchcraftFrameStoreCommitPending &&
              linkToken == _linkToken &&
              identical(link, _link) &&
              deviceId == _deviceId &&
              bleService.isConnected &&
              _selected &&
              _boardActive) {
            final session = _orderedSession;
            if (session == null ||
                !session.isActivated ||
                storeAuthorityLost ||
                (error is TimeoutException &&
                    replacementBase != null &&
                    identical(_interruptedStoreReplacement, replacementBase))) {
              // Reuse the existing mode/connection recovery for a retired
              // transport or eligible interrupted Open; retain local artwork.
              _transportFailed(error);
            } else {
              // The local Open is already visible, but this transfer did not
              // establish matching artwork. Retain the existing source-choice
              // owner so progress draining or later repair cannot imply Live.
              retireConflictBoardPreview();
              _pendingFrameStoreConflict = _PendingFrameStoreConflict(
                session: session,
                authoritySession: authoritySession,
                observedFrameCount: observedStoreState?.frameCount,
                deviceId: deviceId,
                generation: generation,
                selectionIntent: _selectionIntent,
                linkToken: linkToken,
                sourceGeneration: replacementGeneration,
                bootNonce: session.boardBootNonce,
              );
              _errorMessage = error.toString();
              _status = LitchcraftStatus.conflict;
              debugPrint(
                'Litchcraft Store source choice: reason=delivery_error '
                'frames=${frames.length} kind=${error.runtimeType}'
                '${error is LitchcraftRejected ? ' status=${error.status}' : ''} '
                'generation=$_frameStoreGeneration epoch=$linkToken',
              );
            }
          }
          _notify();
        }
        debugPrint(
          'Litchcraft Store delivery failed: frames=${frames.length} '
          'kind=${error.runtimeType}'
          '${error is LitchcraftRejected ? ' status=${error.status}' : ''} '
          'operation_epoch=$linkToken',
        );
        _appTrace?.record(
          'litchcraft.frame_store_delivery',
          fields: <String, Object?>{
            'accepted': false,
            'frames': frames.length,
            'kind': error.runtimeType.toString(),
            if (error is LitchcraftRejected) 'status': error.status,
          },
        );
        completion.completeError(error, stackTrace);
      }
    }, background: true);
    if (!ran) {
      clearTransferIntent();
      throw StateError('Frame-store diagnostic was superseded');
    }
    return completion.future;
  }

  void _settleInterruptedStoreReplacement(int committedGeneration) {
    final interrupted = _interruptedStoreReplacement;
    if (interrupted == null) return;
    if (interrupted.boardId != _deviceId ||
        interrupted.bootNonce != _orderedSession?.boardBootNonce ||
        interrupted.sourceGeneration != documentSession.replacementGeneration ||
        interrupted.failure is! TimeoutException) {
      _interruptedStoreReplacement = null;
      return;
    }
    interrupted.consumed = true;
    interrupted.committed = (
      generation: committedGeneration,
      linkToken: _linkToken,
      authoritySequence: _storeAuthorityStateSequence,
    );
  }

  void _reconcileStoreLocalIntentAfterCommit(
    LitchcraftAnimationDocument accepted,
    int? replacementGeneration,
  ) {
    if (replacementGeneration == null ||
        replacementGeneration != documentSession.replacementGeneration) {
      return;
    }
    final local = documentSession.document;
    if (sameLitchcraftDocument(local, accepted)) {
      _settleInterruptedStoreReplacement(_frameStoreGeneration);
      _deliberateDeliveryGeneration = -1;
      _storePaintWaitingForAuthority = true;
      return;
    }
    if (sameLitchcraftDocumentLayout(local, accepted)) {
      // Commit proves the uploaded preimage. Keep newer pixels in the document
      // and let the existing delta owner send them after fresh root authority
      // binds; they do not require another verified object transfer.
      _settleInterruptedStoreReplacement(_frameStoreGeneration);
      _deliberateDeliveryGeneration = -1;
      _storePaintWaitingForAuthority = true;
      return;
    }
    // A layout change still requires a complete document replacement.
    unawaited(
      _transferFrameStore(
        frames: local.frames,
        playbackOnEntry: local.playbackOnEntry,
        replacementGeneration: replacementGeneration,
      ).then<void>(
        (_) {},
        onError: (Object error, StackTrace _) {
          if (_disposed ||
              replacementGeneration != documentSession.replacementGeneration ||
              error is LitchcraftTransferCancelled) {
            return;
          }
          _errorMessage = error.toString();
          _status = LitchcraftStatus.conflict;
          _notify();
        },
      ),
    );
  }

  void _settleStoreSelectionAfterCommit(int boardSelectedFrame) {
    if (storeReplacementUnresolved) return;
    final pending = _pendingStoreSelectedFrame;
    if (pending == null) {
      documentSession.selectFrame(boardSelectedFrame);
      return;
    }
    _pendingStoreSelectedFrame = null;
    if (pending >= documentSession.document.frames.length) return;
    documentSession.selectFrame(pending);
    unawaited(selectAnimationFrame(pending).catchError((Object _) {}));
  }

  /// Fixed, highly compressible corpus for the first physical latency gate.
  /// All 256 frames remain lossless and independently timed, while identical
  /// emitter content establishes the minimum practical radio workload.
  Future<LitchcraftFrameStoreDiagnosticResult> runRepeated256FrameDiagnostic() {
    final document = documentSession.document;
    final selected = documentSession.selectedFrameIndex.clamp(
      0,
      document.frames.length - 1,
    );
    final first = document.frames[selected];
    return transferFrameStoreForDiagnostics(
      frames: <LitchcraftAnimationFrame>[
        first,
        for (var index = 1; index < 256; index++)
          LitchcraftAnimationFrame(
            durationMs:
                litchcraftAnimationMinimumFrameDurationMs + (index % 256),
            emitters: first.emitters,
          ),
      ],
    );
  }

  /// Sparse lossless corpus: each subsequent frame changes one additional
  /// emitter. This exercises record boundaries and delta decoding with a radio
  /// workload representative of drawing/animation edits rather than identical
  /// frames.
  Future<LitchcraftFrameStoreDiagnosticResult> runSparse256FrameDiagnostic() {
    final document = documentSession.document;
    final selected = documentSession.selectedFrameIndex.clamp(
      0,
      document.frames.length - 1,
    );
    final frames = <LitchcraftAnimationFrame>[];
    var emitters = List<LitchcraftAnimatedEmitter>.of(
      document.frames[selected].emitters,
    );
    frames.add(
      LitchcraftAnimationFrame(
        durationMs: litchcraftAnimationMinimumFrameDurationMs,
        emitters: emitters,
      ),
    );
    for (var index = 1; index < 256; index++) {
      emitters = List<LitchcraftAnimatedEmitter>.of(emitters);
      emitters[index - 1] = LitchcraftAnimatedEmitter(
        color: document.favoriteColors[index % document.favoriteColors.length],
      );
      frames.add(
        LitchcraftAnimationFrame(
          durationMs: litchcraftAnimationMinimumFrameDurationMs + (index % 256),
          emitters: emitters,
        ),
      );
    }
    return transferFrameStoreForDiagnostics(frames: frames);
  }

  /// The production-shaped 256-frame workload used for the physical sync
  /// gate. Keep this behind the diagnostic API so ordinary editing never pays
  /// to decode or transfer the benchmark preset.
  Future<LitchcraftFrameStoreDiagnosticResult> runDense256FrameDiagnostic() =>
      transferFrameStoreForDiagnostics(
        frames: LitchcraftBouncingBallsPreset.decode().frames,
      );

  void _scheduleArtworkDelivery() {
    if (_disposed ||
        usesFrameStoreDocument ||
        !canSaveArtworkToBoard ||
        _entering != null ||
        _exiting != null ||
        _status == LitchcraftStatus.conflict) {
      _artworkDeliveryTimer?.cancel();
      _artworkDeliveryTimer = null;
      return;
    }
    final document = documentSession.document;
    if (_deliveryLinkToken == _linkToken &&
        _deliveryAttempt != null &&
        sameLitchcraftDocument(_deliveryAttempt!, document)) {
      return;
    }
    if (_artworkDeliveryRunning) return;
    if (_artworkDeliveryTimer != null &&
        _deliveryDocumentGeneration == documentSession.generation &&
        _deliveryLinkToken == _linkToken) {
      return;
    }
    _artworkDeliveryTimer?.cancel();
    _deliveryDocumentGeneration = documentSession.generation;
    _deliveryLinkToken = _linkToken;
    _artworkDeliveryTimer = Timer(_artworkDeliveryDelay, () async {
      _artworkDeliveryTimer = null;
      if (_disposed || !canSaveArtworkToBoard) return;
      _deliveryAttempt = documentSession.document;
      _artworkDeliveryRunning = true;
      try {
        await _saveArtworkToBoard(background: true);
      } finally {
        _artworkDeliveryRunning = false;
        _notify();
      }
    });
  }

  /// Uploads the complete artwork into inactive board storage without entering
  /// or displaying Litchcraft. Success confirms accepted storage authority and
  /// an immediate firmware persistence request, not completed flash I/O.
  Future<bool> saveArtworkToBoard() => _saveArtworkToBoard();

  Future<bool> _saveArtworkToBoard({bool background = false}) async {
    if (!background) endStroke();
    if (!canSaveArtworkToBoard) return false;
    final client = _documentClient!;
    final link = _link!;
    final sourceGeneration = documentSession.replacementGeneration;
    final generation = _generation;
    final deviceId = _deviceId!;
    bool current() =>
        !_disposed &&
        generation == _generation &&
        sourceGeneration == documentSession.replacementGeneration &&
        identical(client, _documentClient) &&
        identical(link, _link) &&
        deviceId == bleService.connectedId &&
        canSaveArtworkToBoard;
    var accepted = false;
    await _enqueue(() async {
      if (!current()) return;
      // Passive adoption skips calibration; actual storage traffic needs the
      // connection's negotiated payload before receiving or replacing artwork.
      final packetBytes = await bleService.litchcraftPacketPayloadBytes(
        deviceId,
        connectionSessionToken: _linkToken,
      );
      if (!current()) return;
      _documentPacketBytes = _documentTransportPacketBytes(packetBytes);
      final inspection = await link.inspectV2();
      if (!current() || inspection.active) return;
      var revision = inspection.revision;
      if (background &&
          _deliberateDeliveryGeneration != sourceGeneration &&
          (inspection.schema == 0 ||
              _coherentDocumentRevision != revision ||
              _coherentDocumentBoardId != deviceId ||
              _coherentDocumentSession != inspection.session)) {
        if (documentSession.hasLocalIntent &&
            !documentSession.hasCommonDocument &&
            _status != LitchcraftStatus.gameActive) {
          // Background delivery must not turn an uncertain board authority
          // into an implicit artwork read. Inspect is metadata-only; when it
          // proves that our cached authority is stale, keep the local draft
          // visible and ask before reading the board body. The explicit
          // conflict choice can then either upload this draft or perform the
          // one authoritative snapshot read.
          _status = LitchcraftStatus.conflict;
          _notify();
          return;
        }
        final LitchcraftAnimationDocument board;
        if (inspection.schema == 0) {
          // An older static workspace has no v2 snapshot yet. Preserve its
          // physical RGB authority before promoting it to document storage.
          await _pullSnapshot(inspect: true);
          if (!current()) return;
          board = staticLitchcraftDocument(
            _board,
            favoriteColors: documentSession.document.favoriteColors,
          );
          _coherentDocumentSession = inspection.session;
        } else {
          final snapshot = await client.snapshot(
            session: 0,
            maximumPacketBytes: _documentPacketBytes,
          );
          if (!current()) return;
          board = decodeLitchcraftAnimationDocument(
            snapshot.bytes,
            snapshot.crc32,
          );
          revision = snapshot.revision;
          _coherentDocumentSession = snapshot.session;
        }
        _coherentDocumentRevision = revision;
        _coherentDocumentBoardId = deviceId;
        final unconfirmed = _unconfirmedDocument;
        if (_unconfirmedDocumentBoardId == deviceId &&
            unconfirmed != null &&
            sameLitchcraftDocument(unconfirmed, board)) {
          documentSession.confirm(deviceId, board);
          _unconfirmedDocument = null;
        }
        switch (documentSession.observeBoard(deviceId, board)) {
          case LitchcraftDocumentResolution.conflict:
            _status = LitchcraftStatus.conflict;
            _notify();
            return;
          case LitchcraftDocumentResolution.adoptBoard:
            documentSession.acceptBoard(
              expectedGeneration: documentSession.generation,
            );
          case LitchcraftDocumentResolution.equal:
            documentSession.confirm(deviceId, board);
          case LitchcraftDocumentResolution.sendApp:
          case LitchcraftDocumentResolution.merge:
            if (documentSession.observeBoard(deviceId, board) ==
                LitchcraftDocumentResolution.merge) {
              documentSession.mergeBoard();
            }
            break;
        }
      }
      final source = documentSession.document;
      final board = documentSession.boardDocument;
      if (background &&
          board != null &&
          _coherentDocumentBoardId == deviceId &&
          _coherentDocumentRevision == revision &&
          sameLitchcraftDocument(source, board)) {
        _deliveryAttempt = source;
        _recoveryAttempt = 0;
        return;
      }
      final ack = await _replaceBoardDocument(
        client,
        source,
        session: 0,
        revision: revision,
        inactiveStorage: true,
      );
      if (!current() ||
          (ack.revision != revision &&
              ack.revision != ((revision + 1) & 0xffffffff))) {
        return;
      }
      _documentRevision = ack.revision;
      _coherentDocumentRevision = ack.revision;
      _coherentDocumentSession = ack.session;
      _coherentDocumentBoardId = deviceId;
      _documentDirty = false;
      documentSession.confirm(deviceId, source);
      _deliberateDeliveryGeneration = -1;
      _unconfirmedDocument = null;
      _recoveryAttempt = 0;
      _deliveryAttempt = source;
      _deliveryLinkToken = _linkToken;
      accepted = true;
    }, background: background);
    return accepted;
  }

  void setBoardId(String? id) {
    if (_deviceId == id) return;
    _deviceId = id;
    _connectionChanged();
  }

  void restoreDraft(LitchcraftCanvas draft) {
    if (_status == LitchcraftStatus.live || _boardActive) return;
    _canvas = draft;
    _detached = draft;
    // Even an empty restored draft may represent an intentional clear. Without
    // a common ancestor its provenance cannot be inferred from pixel content.
    _offlineDiverged = true;
    _undo.clear();
    _redo.clear();
    _notify();
  }

  void _connectionChanged() {
    _cancelEntryDraftWait();
    final connected = bleService.isConnected;
    if (!connected) {
      _gameEntryChoice = _gameEntryResetChoice = null;
      _gameEntryFailed = false;
      cancelBrushColorPreview();
      _cancelNotificationRepair();
      _physicalStoreRepairTimer?.cancel();
      _physicalStoreRepairTimer = null;
      _shakeEraseSupported = false;
      _inactiveStorageSupported = false;
      _deliveryAttempt = null;
      _offlineDiverged = _offlineDiverged || !_sameArtwork(_canvas, _board);
      _detached = _canvas;
      endStroke();
      // A canceled board read no longer owns the detached editor. Offline
      // strokes become local intent and reconcile on the next connection.
      _frameStoreReadPending = false;
      _preservePendingStaticCanvas();
      retireConflictBoardPreview();
      _pendingFrameStoreConflict = null;
      ++_generation;
      final old = _link;
      _link = null;
      _linkUsable = false;
      _clearAdvancedTimeline();
      _clearTrustedExit();
      unawaited(_disposeLink(old).catchError((Object _) {}));
      _failSnapshot(StateError('Board disconnected'));
      _status = LitchcraftStatus.offline;
      _clearPhysicalColorPick();
      _clearPhysicalShakeErase(resetDedupe: true);
      _recoveryTimer?.cancel();
      _recoveryTimer = null;
      _recoveryAttempt = 0;
      _emptyAdmissionSequence = null;
      _notify();
    } else if (_exitOnReconnect == bleService.connectedId && !_selected) {
      unawaited(_resumeConnection());
    } else if (_selected) {
      if (_status == LitchcraftStatus.live &&
          _deviceId == bleService.connectedId &&
          _linkToken == bleService.connectionSessionToken) {
        return; // Repeated Connected is not a new transport generation.
      }
      unawaited(_resumeConnection());
    }
  }

  Future<void> _resumeConnection() async {
    // The previous generation can still be unwinding a canceled snapshot.
    // Reconnect must run after that future releases its single-flight slot.
    try {
      await _entering;
      await _exiting;
    } catch (_) {
      // A rejected departure already owns recovery; a Connected callback
      // cannot reopen the editor or leak its acknowledged failure.
      return;
    }
    if (_disposed ||
        !bleService.isConnected ||
        (_gameEntryAppOnly && !_boardActive)) {
      return;
    }
    if (_exitOnReconnect == bleService.connectedId && !_selected) {
      await _startPendingExit();
    } else if (_selected) {
      // A Connected callback received during admission may have waited for
      // that same handshake. Do not replace its now-healthy subscription.
      if (_status == LitchcraftStatus.live &&
          _deviceId == bleService.connectedId &&
          _linkToken == bleService.connectionSessionToken) {
        return;
      }
      await _startEnter(resuming: true);
    }
  }

  static bool? _boardRotation(Object? value) {
    final parsed = switch (value) {
      bool setting => setting ? 1 : 0,
      int setting => setting,
      String text => int.tryParse(text),
      _ => null,
    };
    return switch (parsed) {
      0 => false,
      1 => true,
      _ => null,
    };
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && _selected) {
      unawaited(revalidateSelectedBoard());
    } else if (state == AppLifecycleState.paused ||
        state == AppLifecycleState.hidden) {
      endStroke();
    }
  }

  /// Revalidates retained Litchcraft authority after another warm app surface
  /// had focus. A same-connection page transition is not evidence that the
  /// board session, revision, or document remained coherent.
  Future<void> revalidateSelectedBoard() {
    if (_gameEntryAppOnly && !_boardActive) return Future.value();
    if (_disposed || !_selected || !bleService.isConnected) {
      return Future<void>.value();
    }
    final active = _entering;
    if (active != null) return active;
    if (_status != LitchcraftStatus.live) {
      return _startEnter(resuming: true);
    }
    // Refresh shares the command lane with paint, so an older snapshot cannot
    // reconcile over taps made while another app surface was visible.
    late final Future<void> refresh;
    refresh = _enqueue(_refreshLiveCanvas).whenComplete(() {
      if (identical(_entering, refresh)) _entering = null;
      _notify();
    });
    _entering = refresh;
    _notify();
    return refresh;
  }

  Future<void> _refreshLiveCanvas() async {
    if (_advancedWire || _localFrameStoreDocument) {
      await _synchronizeAdvancedEntry(
        recoverRestart: _selected && _boardActive,
      );
      return;
    }
    if (_efficientSync) {
      final session = _sessionId;
      final revision = _revision;
      final state = await _link!.send(LitchcraftProtocol.inspect, 0);
      if (!_boardActive) return;
      if (state.session == session &&
          state.revision == revision &&
          !_needsResync) {
        return;
      }
    }
    await _pullSnapshot(inspect: true);
  }

  Future<void> enter({Future<void>? draftReady}) =>
      _beginGameEntry(draftReady: draftReady);

  /// Register the local restoration barrier before retained reconnect work can
  /// reach artwork reconciliation. This does not acquire board mode.
  void setDraftReadiness(Future<void> ready) => _draftReady = ready;

  Future<void> retryGameEntry() => gameEntryRecoveryRequired
      ? _beginGameEntry(refreshHome: true)
      : Future.value();

  Future<void> _beginGameEntry({
    Future<void>? draftReady,
    bool refreshHome = false,
  }) async {
    final resetting = _gameEntryResetChoice;
    if (resetting != null && _currentGameEntryChoice(resetting)) {
      await _gameEntryResetWork;
      return;
    }
    if (gameEntryChoice != null) return;
    if (_selected && _entering != null && _adopting == null) {
      await _entering;
      return;
    }
    _appTrace?.record('litchcraft.entry_start');
    _gameEntryFailed = false;
    _gameEntryAppOnly = refreshHome;
    _cancelNotificationRepair();
    _draftReady = draftReady ?? _draftReady;
    ++_selectionIntent;
    _selected = true;
    _recoveryAttempt = 0;
    _emptyAdmissionSequence = null;
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    _exitOnReconnect = null;
    if (_adopting != null) {
      // A connection-time, read-only mode probe must never sit in front of an
      // explicit user admission. Retire its generation and let it unwind in
      // the background; the new Enter owns the visible navigation intent.
      ++_generation;
      _documentClient?.cancel();
      _entering = null;
    }
    if (!_selected || _disposed) return;
    await _startEnter(
      draftReady: draftReady,
      offerGameChoice: true,
      refreshHome: refreshHome,
    );
  }

  /// Adopts a Litchcraft session that the connected board already owns.
  ///
  /// This probe never sends Enter for an inactive board. It lets navigation
  /// follow physical board authority without turning an ordinary Play
  /// connection into Litchcraft as a side effect.
  Future<bool> adoptActiveBoard() async {
    final String? deviceId = bleService.connectedId;
    if (!bleService.isConnected || deviceId == null) return false;
    final String probe = '$deviceId:${bleService.connectionSessionToken}';
    final Future<bool>? activeAdoption = _adopting;
    if (activeAdoption != null) {
      if (_adoptingProbe == probe) return activeAdoption;
      await activeAdoption;
      if (!bleService.isConnected ||
          bleService.connectedId != deviceId ||
          '$deviceId:${bleService.connectionSessionToken}' != probe) {
        return false;
      }
      return adoptActiveBoard();
    }
    late final Future<bool> adoption;
    adoption = _adoptActiveBoard().whenComplete(() {
      if (identical(_adopting, adoption)) {
        _adopting = null;
        _adoptingProbe = null;
      }
    });
    _adoptingProbe = probe;
    _adopting = adoption;
    return adoption;
  }

  Future<bool> _adoptActiveBoard() async {
    if (_disposed || !bleService.isConnected) return false;
    if (_selected &&
        _boardActive &&
        _deviceId == bleService.connectedId &&
        _boardAuthorityLinkToken == bleService.connectionSessionToken &&
        (_status == LitchcraftStatus.live ||
            _status == LitchcraftStatus.conflict)) {
      return true;
    }
    if (_selected) return false;

    final int intent = _selectionIntent;
    _selected = true;
    await _startEnter(resuming: true, activeOnly: true);
    // Inspect can establish physical mode authority before a later snapshot
    // or reconciliation step fails. Keep the selected repair path alive and
    // route to Litchcraft whenever the board is authoritatively active.
    final bool boardOwnsMode =
        _selected &&
        _boardActive &&
        _deviceId == bleService.connectedId &&
        _boardAuthorityLinkToken == bleService.connectionSessionToken;
    if (!boardOwnsMode && intent == _selectionIntent) {
      _selected = false;
      if (_status == LitchcraftStatus.connecting) {
        _status = LitchcraftStatus.offline;
      }
      _notify();
    }
    return boardOwnsMode;
  }

  /// Repairs the currently selected canvas without treating the action as a
  /// new admission. A user-visible retry must replace a suspect notification
  /// subscription, then inspect the retained board authority before deciding
  /// whether any artwork transfer is necessary.
  Future<void> retryConnection() async {
    final entering = _entering;
    if (_selected && entering != null && _adopting == null) {
      final selectionIntent = _selectionIntent;
      final deviceId = bleService.connectedId;
      final connectionToken = bleService.connectionSessionToken;
      try {
        await entering;
      } catch (_) {
        // The existing entry owns its failure presentation. A current explicit
        // Retry can still repair it after that owner releases its slot.
      }
      if (_disposed ||
          selectionIntent != _selectionIntent ||
          !bleService.isConnected ||
          bleService.connectedId != deviceId ||
          bleService.connectionSessionToken != connectionToken) {
        return;
      }
      if (_status == LitchcraftStatus.live &&
          _boardActive &&
          _linkUsable &&
          _deviceId == deviceId &&
          _linkToken == connectionToken) {
        return;
      }
    }
    _cancelNotificationRepair();
    ++_selectionIntent;
    _selected = true;
    _recoveryAttempt = 0;
    _emptyAdmissionSequence = null;
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    _exitOnReconnect = null;
    _linkUsable = false;
    return _startEnter(
      resuming: _boardActive,
      // A retry is an explicit user request to restore Litchcraft authority.
      // The cached active bit may be stale when the board left mode without a
      // notification, so an inactive inspection must be allowed to re-enter
      // even when the old session/revision are nonzero.
      forceEntryIfInactive: true,
    );
  }

  Future<void> _startEnter({
    bool resuming = false,
    bool activeOnly = false,
    bool forceEntryIfInactive = false,
    bool offerGameChoice = false,
    bool refreshHome = false,
    Future<void>? draftReady,
  }) {
    final active = _entering;
    if (active != null) return active;
    late final Future<void> entering;
    entering =
        _enter(
          resuming: resuming,
          activeOnly: activeOnly,
          forceEntryIfInactive: forceEntryIfInactive,
          offerGameChoice: offerGameChoice,
          refreshHome: refreshHome,
          draftReady: draftReady ?? _draftReady,
        ).whenComplete(() {
          if (identical(_entering, entering)) {
            _entering = null;
            _settleNotificationRepair();
            _retryEmptyBoard();
            _notify();
          }
        });
    _entering = entering;
    return entering;
  }

  Future<LitchcraftPacket> _requestAdmission(LitchcraftLink link) {
    final pending = link.send(LitchcraftProtocol.enter, 0);
    _admission = pending;
    return pending.whenComplete(() {
      if (identical(_admission, pending)) _admission = null;
    });
  }

  Future<void> _enter({
    required bool resuming,
    bool activeOnly = false,
    bool forceEntryIfInactive = false,
    bool offerGameChoice = false,
    bool refreshHome = false,
    Future<void>? draftReady,
  }) async {
    final elapsed = Stopwatch()..start();
    if (!bleService.isConnected) {
      _status = LitchcraftStatus.offline;
      _notify();
      return;
    }
    try {
      await _exiting;
    } catch (_) {
      // Failed departure retains its existing recovery owner. A concurrent
      // automatic entry must not reopen artwork or leak that exit failure.
      return;
    }
    if (resuming) await _tail;
    final id = bleService.connectedId;
    if (!_selected || !bleService.isConnected || id == null) return;
    var generation = ++_generation;
    final selectionIntent = _selectionIntent;
    final connectionToken = bleService.connectionSessionToken;
    _shakeEraseSupported = false;
    final resumingActiveSession = resuming && _boardActive && _deviceId == id;
    final previousSession = _sessionId;
    final previousRevision = _revision;
    final refreshingLive =
        resumingActiveSession &&
        _status == LitchcraftStatus.live &&
        _linkToken == bleService.connectionSessionToken;
    _deviceId = id;
    if (!refreshingLive) _status = LitchcraftStatus.connecting;
    _storeRootMissing = false;
    _errorMessage = null;
    final prerequisites = Completer<void>();
    _entryDraftWaitCancellation = prerequisites;
    _notify();
    try {
      HomeStateNotification? retryHome;
      if (refreshHome) {
        final fresh = await bleService.requestCurrentHomeStateSnapshot(id);
        if (generation != _generation ||
            selectionIntent != _selectionIntent ||
            !_selected ||
            !bleService.isConnected ||
            bleService.connectedId != id ||
            connectionToken != bleService.connectionSessionToken) {
          return;
        }
        retryHome = fresh ? bleService.latestHomeState : null;
        if (retryHome == null) {
          _gameEntryFailed = true;
          _gameEntryAppOnly = true;
          _status = LitchcraftStatus.error;
          _notify();
          return;
        }
      }
      if (offerGameChoice &&
          await _offerGameEntryChoice(
            id,
            connectionToken,
            selectionIntent,
            verifiedRetryHome: retryHome,
          )) {
        return;
      }
      if (generation != _generation ||
          selectionIntent != _selectionIntent ||
          !_selected) {
        return;
      }
      _gameEntryAppOnly = false;
      final knownCaps = bleService.deviceCapabilities(id);
      LitchcraftPacket? earlyAdmission;
      final controlResolved =
          !resuming && await bleService.litchcraftControlAvailable(id);
      if (generation != _generation || !_selected) return;
      if (controlResolved) {
        await _bindLink(id, generation);
        if (generation != _generation || !_selected) return;
        if (knownCaps.explicitlyReported &&
            knownCaps.supportsLitchcraftAnimation) {
          _inactiveStorageSupported = true;
          _documentClient ??= LitchcraftV2DocumentClient(_link!);
        }
        final admittedLink = _link!;
        earlyAdmission = await _requestAdmission(admittedLink);
        if (!identical(admittedLink, _link) ||
            !bleService.isConnected ||
            connectionToken != bleService.connectionSessionToken) {
          return;
        }
        _sessionId = earlyAdmission.session;
        if (selectionIntent != _selectionIntent || !_selected) return;
        if (generation != _generation) {
          if (draftReady == null) return;
          generation = _generation;
        }
      }
      final caps = knownCaps.explicitlyReported
          ? knownCaps
          : await _prepareConnection(
              bleService.fetchDeviceCapabilities(deviceId: id),
              'capability_discovery',
            );
      if (generation != _generation || !_selected) return;
      if (!caps.supportsLitchcraft && !controlResolved) {
        _status = LitchcraftStatus.unsupported;
        if (!caps.explicitlyReported) _scheduleRecovery();
        return;
      }
      _bulkReplace = caps.supportsLitchcraftBulk;
      _efficientSync = caps.supportsLitchcraftInspect;
      _paletteSyncSupported = caps.supportsLitchcraftPalette;
      _shakeEraseSupported = caps.supportsLitchcraftShakeErase;
      final advancedSupported =
          caps.explicitlyReported && caps.supportsLitchcraftAnimation;
      _inactiveStorageSupported = advancedSupported;
      if (caps.supportsLitchcraft && !advancedSupported) {
        // A confirmed static destination cannot inherit the previous board's
        // Store route. Keep rich local artwork protected until its existing
        // unsupported-document check below, without rewriting the draft.
        _localFrameStoreDocument = false;
        _advancedWire = false;
      }
      LitchcraftV2State? passiveInspection;
      LitchcraftPacket? passiveStaticInspection;
      if (resuming) {
        await _bindLink(id, generation);
        if (generation != _generation || !_selected) return;
        if (advancedSupported) {
          passiveInspection = await _link!.inspectV2();
          if (generation != _generation ||
              !_selected ||
              selectionIntent != _selectionIntent) {
            return;
          }
          // Inactive passive attachment has no artwork prerequisites. Keep
          // deliberate retry/restart admission under its existing policy.
          final mayEnter =
              forceEntryIfInactive ||
              (!activeOnly && !resumingActiveSession) ||
              (resumingActiveSession &&
                  !activeOnly &&
                  passiveInspection.session == 0 &&
                  passiveInspection.revision == 0);
          if (!passiveInspection.active && !mayEnter) {
            await _synchronizeAdvancedEntry(
              inspected: passiveInspection,
              previousModeSession: previousSession,
            );
            return;
          }
        } else if (caps.supportsLitchcraftInspect) {
          passiveStaticInspection = await _link!.send(
            LitchcraftProtocol.inspect,
            0,
          );
          if (generation != _generation ||
              !_selected ||
              selectionIntent != _selectionIntent) {
            return;
          }
          if (!_boardActive &&
              (activeOnly ||
                  !forceEntryIfInactive &&
                      (previousSession != 0 || previousRevision != 0))) {
            _selected = false;
            _status = LitchcraftStatus.offline;
            return;
          }
        }
      }
      if (!resuming) {
        await _bindLink(id, generation);
        if (generation != _generation || !_selected) return;
        if (advancedSupported) {
          _documentClient ??= LitchcraftV2DocumentClient(_link!);
        }
      }
      // Page admission is a minimum-MTU control request. Orientation and
      // document inspection protect later editing, but cannot delay the board
      // displaying its already-owned canvas when the user selects this mode.
      _detached = _canvas;
      final trustedExit = !resuming ? _trustedExitFor(id) : null;
      final admissionLink = _link;
      final admission = !resuming
          ? earlyAdmission ?? await _requestAdmission(admissionLink!)
          : null;
      if (_disposed ||
          !_selected ||
          !bleService.isConnected ||
          bleService.connectedId != id ||
          connectionToken != bleService.connectionSessionToken ||
          !identical(admissionLink, _link)) {
        return;
      }
      if (admission != null) {
        // Admission belongs to the link, not the document generation: disk
        // restoration may have replaced the draft while this ACK was in flight.
        _sessionId = admission.session;
        _appTrace?.record(
          'litchcraft.entry_acknowledged',
          fields: <String, Object?>{
            'elapsed_ms': elapsed.elapsedMilliseconds,
            'board_active': _boardActive,
          },
        );
      }
      // Leave may have arrived while Enter's acknowledgement was pending,
      // before a draft waiter existed to cancel. Keep acknowledged board
      // authority for Exit, but never install a new disk wait for old intent.
      if (selectionIntent != _selectionIntent) return;
      await _tail;
      if (selectionIntent != _selectionIntent) return;
      Future<bool> restorePendingDraft() async {
        while (true) {
          final localDraftReady = draftReady ?? _draftReady;
          draftReady = null;
          if (localDraftReady == null) return generation == _generation;
          // Disk restoration precedes reconciliation, never mode admission.
          // Restoring the saved document legitimately changes its generation;
          // retain admission only while the same navigation and link still own it.
          final link = _link;
          final sourceGeneration = documentSession.replacementGeneration;
          await Future.any([localDraftReady, prerequisites.future]);
          if (prerequisites.isCompleted ||
              _disposed ||
              !_selected ||
              selectionIntent != _selectionIntent ||
              !identical(link, _link) ||
              !bleService.isConnected ||
              bleService.connectedId != id ||
              connectionToken != bleService.connectionSessionToken) {
            return false;
          }
          if (generation != _generation &&
              sourceGeneration == documentSession.replacementGeneration) {
            passiveInspection = null;
            passiveStaticInspection = null;
          }
          if (identical(_draftReady, localDraftReady)) _draftReady = null;
          generation = _generation;
          _status = LitchcraftStatus.connecting;
          _detached = _canvas;
        }
      }

      if (!await restorePendingDraft()) return;
      if (documentSession.isAdvanced && !advancedSupported) {
        _advancedWire = false;
        _status = LitchcraftStatus.unsupported;
        return;
      }
      final boardRotated =
          _boardRotation(bleService.latestSettingValue('BOARD_ROTATE')) ??
          _boardRotation(
            await Future.any<Object?>([
              _prepareConnection(
                bleService.readSettingValue(deviceId: id, key: 'BOARD_ROTATE'),
                'orientation_read',
              ),
              prerequisites.future.then((_) => null),
            ]),
          );
      if (prerequisites.isCompleted ||
          selectionIntent != _selectionIntent ||
          !_selected) {
        return;
      }
      if (!await restorePendingDraft()) return;
      if (boardRotated == null) {
        throw StateError('Board orientation is not available');
      }
      final observedMode = passiveInspection;
      if (observedMode != null) {
        final latest = _pendingAdvancedState;
        if (latest == null ||
            latest.session != observedMode.session ||
            latest.revision != observedMode.revision ||
            latest.active != observedMode.active ||
            _sessionId != observedMode.session ||
            _boardActive != observedMode.active) {
          passiveInspection = null;
        }
      }
      if (identical(_entryDraftWaitCancellation, prerequisites)) {
        _entryDraftWaitCancellation = null;
      }
      if (advancedSupported &&
          await _synchronizeAdvancedEntry(
            inspected: passiveInspection,
            previousModeSession: previousSession,
            enterIfInactive:
                forceEntryIfInactive ||
                (resuming && !activeOnly && !resumingActiveSession),
            recoverRestart: resumingActiveSession && !activeOnly,
          )) {
        return;
      }
      _advancedWire = false;
      _detached = _canvas;
      if (!resuming) {
        // Deliberate page entry asks firmware for admission first. Rejections
        // are a single round trip; do not make an occupied board wait for the
        // 128-notification artwork snapshot it is not allowed to use.
        final ack = admission!;
        if (generation != _generation) return;
        _sessionId = ack.session;
        if (trustedExit != null &&
            _boardActive &&
            ack.revision == ((trustedExit.revision + 1) & 0xffffffff)) {
          final retained = trustedExit.canvas.withTopology(
            LitchcraftTopology.fromId(ack.payload[2]),
          );
          _board = retained;
          _canvas = retained;
          _detached = retained;
          _revision = ack.revision;
          _stateRevision = ack.revision;
          _needsResync = false;
          _offlineDiverged = false;
          _status = LitchcraftStatus.live;
          _clearTrustedExit();
          return;
        }
        _clearTrustedExit();
        await _pullSnapshot();
        if (generation != _generation || !_selected) return;
        if (!_boardActive) {
          _selected = false;
          _status = LitchcraftStatus.offline;
          return;
        }
        await _reconcileCanvas();
        return;
      }
      // Automatic reconnect/resume must inspect without acquiring ownership:
      // a physical exit remains an explicit exit and is never reversed here.
      if (caps.supportsLitchcraftInspect) {
        final observedStaticMode = passiveStaticInspection;
        final inspection =
            observedStaticMode != null &&
                observedStaticMode.session == _sessionId &&
                observedStaticMode.revision == _stateRevision
            ? observedStaticMode
            : await _link!.send(LitchcraftProtocol.inspect, 0);
        if (generation != _generation || !_selected) return;
        final sameAuthority =
            _boardActive &&
            inspection.session == previousSession &&
            inspection.revision == previousRevision &&
            !_needsResync;
        if (sameAuthority) {
          _board = _board.withTopology(
            LitchcraftTopology.fromId(inspection.payload[2]),
          );
          await _reconcileCanvas(requireChoice: previousSession != _sessionId);

          return;
        }
        if (forceEntryIfInactive &&
            (inspection.payload[1] & LitchcraftProtocol.stateActive) == 0) {
          final ack = await _requestAdmission(_link!);
          if (generation != _generation || !_selected) return;
          _sessionId = ack.session;
          await _pullSnapshot();
          if (generation != _generation || !_selected || !_boardActive) return;
          await _reconcileCanvas(requireChoice: previousSession != _sessionId);
          return;
        }
        if (!_boardActive) {
          if (activeOnly) {
            _status = LitchcraftStatus.offline;
            return;
          }
          final bool boardRestarted =
              resumingActiveSession &&
              previousSession != 0 &&
              inspection.session == 0 &&
              inspection.revision == 0;
          if (resumingActiveSession && !boardRestarted) {
            _canvas = _offlineDiverged || previousSession != inspection.session
                ? _detached!
                : _board;
            _offlineDiverged = !_sameArtwork(_canvas, _board);
            _selected = false;
            _status = LitchcraftStatus.offline;
            return;
          }
          final ack = await _requestAdmission(_link!);
          if (generation != _generation) return;
          _sessionId = ack.session;
          if (boardRestarted) {
            // A power cycle restarts the volatile session/revision sequence.
            // Do not reject its first coherent snapshot as older than the
            // retired pre-reboot session.
            _revision = ack.revision;
            _stateRevision = ack.revision;
            _needsResync = false;
          }
          await _pullSnapshot();
          if (generation != _generation || !_selected) return;
          if (!_boardActive) {
            _selected = false;
            _status = LitchcraftStatus.offline;
            return;
          }
          // A restart retires transport authority, not necessarily artwork.
          // Reconcile the coherent snapshot before writing the phone draft.
          await _reconcileCanvas(requireChoice: previousSession != _sessionId);
          return;
        }
      }
      await _pullSnapshot(inspect: true);
      if (generation != _generation || !_selected) return;
      if (_boardActive) {
        await _reconcileCanvas(requireChoice: previousSession != _sessionId);

        return;
      }
      if (activeOnly) {
        _status = LitchcraftStatus.offline;
        return;
      }
      final bool boardRestarted =
          resumingActiveSession &&
          previousSession != 0 &&
          _sessionId == 0 &&
          _revision == 0;
      if (resumingActiveSession && !boardRestarted) {
        // A physical exit while disconnected is still an explicit exit. A
        // reboot also ends ownership; preserve the app draft in that case.
        _canvas = _offlineDiverged || previousSession != _sessionId
            ? _detached!
            : _board;
        _offlineDiverged = _canvas != _board;
        _selected = false;
        _status = LitchcraftStatus.offline;
        return;
      }
      // The page was explicitly selected while offline, so the first newly
      // connected board may be admitted after inspection.
      final ack = await _requestAdmission(_link!);
      if (generation != _generation) return;
      _sessionId = ack.session;
      if (boardRestarted) {
        _revision = ack.revision;
        _stateRevision = ack.revision;
        _needsResync = false;
      }
      await _pullSnapshot();
      if (generation != _generation || !_selected) return;
      if (!_boardActive) {
        _selected = false;
        _status = LitchcraftStatus.offline;
        return;
      }
      await _reconcileCanvas(requireChoice: previousSession != _sessionId);
    } on LitchcraftTransferCancelled {
      // A newer navigation intent can retire a read without invalidating the
      // healthy notification link or the board's acknowledged mode authority.
      if (selectionIntent == _selectionIntent) rethrow;
    } on LitchcraftModeUnavailable {
      if (generation != _generation) return;
      _status = LitchcraftStatus.unsupported;
      _errorMessage = null;
      if (_detached != null) _canvas = _detached!;
    } on LitchcraftRejected catch (error) {
      if (generation != _generation) return;
      if (offerGameChoice &&
          error.status == 1 &&
          await _offerGameEntryChoice(
            id,
            connectionToken,
            selectionIntent,
            refused: true,
          )) {
        return;
      }
      if (generation != _generation || selectionIntent != _selectionIntent) {
        return;
      }
      _recoveryTimer?.cancel();
      _recoveryTimer = null;
      _status = switch (error.status) {
        1 => LitchcraftStatus.gameActive,
        8 => LitchcraftStatus.boardNotEmpty,
        LitchcraftProtocol.statusEntryPending => LitchcraftStatus.connecting,
        _ => LitchcraftStatus.error,
      };
      _errorMessage = error.status == LitchcraftProtocol.statusEntryPending
          ? null
          : error.toString();
      if (error.status == LitchcraftProtocol.statusEntryPending) {
        _scheduleRecovery();
      }
      if (_detached != null) _canvas = _detached!;
    } catch (error) {
      if (generation == _generation) _transportFailed(error);
    } finally {
      if (identical(_entryDraftWaitCancellation, prerequisites)) {
        _entryDraftWaitCancellation = null;
      }
      _notify();
    }
  }

  Future<void> _reconcileCanvas({bool requireChoice = false}) async {
    final local = _detached ?? _canvas;
    final common = _commonBoardId == _deviceId ? _commonCanvas : null;
    final appUnchanged = common != null
        ? _sameArtwork(local, common)
        : !_offlineDiverged && _commonCanvas == null;
    if (!_board.hasMarks && local.hasMarks) {
      _canvas = local.withTopology(_board.topology);
      _clearPendingPaint();
      _status = LitchcraftStatus.live;
      await replaceCanvas(_canvas);
      return;
    }
    // Selecting Litchcraft while the first connection probe is in flight can
    // establish a new board session before any app-authored/common canvas
    // exists. That session change is not a conflict: the untouched app adopts
    // the board just as it does when inspection finishes before navigation.
    if (_sameArtwork(local, _board) ||
        (appUnchanged && (common == null || !requireChoice))) {
      _clearPendingPaint();
      _canvas = _board;
      _offlineDiverged = false;
      _status = LitchcraftStatus.live;
      _recoveryAttempt = 0;
      _recoveryTimer?.cancel();
      _recoveryTimer = null;
      return;
    }
    _canvas = local.withTopology(_board.topology);
    final acceptedPaint = _unconfirmedPaintCanvas;
    if (!_board.hasMarks ||
        (common != null && _sameArtwork(_board, common)) ||
        (_unconfirmedPaintBoardId == _deviceId &&
            acceptedPaint != null &&
            _sameArtwork(_board, acceptedPaint))) {
      _clearPendingPaint();
      _status = LitchcraftStatus.live;
      await replaceCanvas(_canvas);
      return;
    }
    // Only divergent artwork requires a choice. Transport/revision churn does
    // not erase the last common canvas; a restored draft has no proven ancestor.
    _status = LitchcraftStatus.conflict;
  }

  void _retireChangedStoreLineage(int previousMode, int observedMode) {
    final session = _orderedSession;
    final common = _storeCommonAuthority;
    final deviceId = _deviceId;
    if (deviceId == null ||
        _storeSessionBoardId != deviceId ||
        !documentSession.hasCommonDocument) {
      return;
    }
    final changedMode = previousMode != 0 && previousMode != observedMode;
    final changedBoot =
        session != null &&
        common != null &&
        common.deviceId == deviceId &&
        common.bootNonce != session.boardBootNonce;
    if (!changedMode && !changedBoot) return;
    documentSession.retireBoardLineage();
    _storeCommonAuthority = null;
    _storeCommonBindingGeneration = null;
  }

  Future<bool> _synchronizeAdvancedEntry({
    bool enterIfInactive = false,
    bool recoverRestart = false,
    LitchcraftV2State? inspected,
    int? previousModeSession,
  }) async {
    final link = _link!;
    final generation = _generation;
    final selectionIntent = _selectionIntent;
    final sourceGeneration = documentSession.replacementGeneration;
    // Mode callbacks may publish the new session before Inspect completes.
    final previousMode = previousModeSession ?? _documentSessionId;
    var inspection = inspected ?? await link.inspectV2();
    if (generation != _generation ||
        selectionIntent != _selectionIntent ||
        !identical(link, _link) ||
        sourceGeneration != documentSession.replacementGeneration) {
      return true;
    }
    if (!inspection.active &&
        (enterIfInactive ||
            (recoverRestart &&
                inspection.session == 0 &&
                inspection.revision == 0))) {
      await _requestAdmission(link);
      if (generation != _generation || selectionIntent != _selectionIntent) {
        return true;
      }
      inspection = await link.inspectV2();
      if (generation != _generation || selectionIntent != _selectionIntent) {
        return true;
      }
    }
    _retireChangedStoreLineage(previousMode, inspection.session);
    _boardAuthorityLinkToken = _linkToken;
    // Inspect proves the destination before Store authority or artwork arrives.
    // Keep live/edit authority under the existing reconciliation owner.
    _sessionId = inspection.session;
    _boardActive = inspection.active;
    _recordInspectedMode(inspection.active);
    if (!_disposed) notifyListeners();
    if (generation != _generation ||
        selectionIntent != _selectionIntent ||
        !identical(link, _link) ||
        sourceGeneration != documentSession.replacementGeneration) {
      return true;
    }
    _inactiveStorageSupported = true;
    _documentClient ??= LitchcraftV2DocumentClient(link);
    if (!inspection.active) {
      // A recovered Store root is durable artwork, not live mode ownership.
      // Automatic observation must finish before any resident preparation or
      // snapshot; deliberate admission above establishes a nonzero session.
      _sessionId = _documentSessionId = inspection.session;
      _documentRevision = inspection.revision;
      _boardActive = false;
      _selected = false;
      _advancedWire = false;
      _frameStoreReadPending = false;
      _pendingStoreEntryInspection = null;
      _storeEntryInspection = null;
      _clearAdvancedTimeline(disposeObserver: false);
      _status = LitchcraftStatus.offline;
      return true;
    }
    if (_orderedSession?.isActivated == true) {
      if (await _synchronizeFrameStoreEntry(inspection)) return true;
      // An activated V3 session cannot treat an absent Store root as older
      // schema-3 or static artwork, regardless of the compact Inspect schema.
      // Entry creates that root on the board worker; if it is still absent,
      // keep the failure visible until a retry succeeds.
      _localFrameStoreDocument = true;
      _advancedWire = false;
      _sessionId = _documentSessionId = inspection.session;
      _boardActive = inspection.active;
      _storeRootMissing = inspection.active;
      _status = inspection.active
          ? LitchcraftStatus.error
          : LitchcraftStatus.offline;
      _errorMessage = inspection.active
          ? 'Board artwork is unavailable. Try again.'
          : null;
      _notify();
      return true;
    }
    if (usesFrameStoreDocument) {
      // A retained local Store document does not prove that this connection
      // reopened its shared session or inspected the board's current root.
      // Keep the repair action visible instead of reviving an old canvas as
      // live after the control stream disappears during reconnect.
      final lostStoreSession = _storeSessionBoardId == _deviceId;
      _sessionId = _documentSessionId = inspection.session;
      _boardActive = inspection.active;
      _advancedWire = false;
      if (lostStoreSession) _frameStoreReadPending = false;
      _storeRootMissing = inspection.active && lostStoreSession;
      _status = inspection.active
          ? lostStoreSession
                ? LitchcraftStatus.error
                : LitchcraftStatus.live
          : LitchcraftStatus.offline;
      _errorMessage = _storeRootMissing
          ? 'Board artwork is unavailable. Try again.'
          : null;
      _notify();
      return true;
    }
    if (inspection.schema == 0 && !documentSession.isAdvanced) {
      return false;
    }
    _advancedWire = true;
    _advancedCanUndo = inspection.canUndo;
    _advancedCanRedo = inspection.canRedo;
    _paletteSyncSupported = false;
    _documentSessionId = inspection.session;
    _documentRevision = inspection.revision;
    _sessionId = inspection.session;
    _boardActive = inspection.active;
    _advancedPlaying = inspection.playing;
    _advancedSelectedFrame = inspection.selectedFrame;
    if (_boardActive &&
        inspection.selectedFrame < documentSession.document.frames.length) {
      _selectBoardFrame(inspection.selectedFrame);
    }
    _applyPendingAdvancedTimeline();
    if (!_boardActive) {
      _selected = false;
      _status = LitchcraftStatus.offline;
      return true;
    }
    // An inactive automatic adoption must finish without starting optional
    // bulk calibration. Otherwise a later explicit Enter waits behind it.
    _documentPacketBytes = _documentTransportPacketBytes(
      await bleService.litchcraftPacketPayloadBytes(
        _deviceId!,
        connectionSessionToken: _linkToken,
      ),
    );
    if (generation != _generation ||
        selectionIntent != _selectionIntent ||
        !identical(link, _link)) {
      return true;
    }
    if (_boardActive) {
      // One extension probe admits every current atomic canvas action. Older
      // peers reject it read-only, then retain the narrower compatibility
      // probes and whole-document fallback.
      _advancedCanvasPattern = await link.supportsCanvasPatternV2(
        session: _documentSessionId,
      );
      _advancedCanvasEdit = await link.supportsCanvasEditV2(
        session: _documentSessionId,
      );
      if (selectionIntent != _selectionIntent) return true;
      _advancedCanvasRotate =
          _advancedCanvasEdit &&
          await link.supportsCanvasRotateV2(session: _documentSessionId);
      if (generation != _generation ||
          selectionIntent != _selectionIntent ||
          !identical(link, _link)) {
        return true;
      }
    }
    if (inspection.schema == 0) {
      await _sendDocument();
    } else if (!_documentDirty &&
        _coherentDocumentRevision == _documentRevision &&
        _coherentDocumentSession == _documentSessionId &&
        _coherentDocumentBoardId == _deviceId &&
        documentSession.boardDocument != null) {
      await _reconcileDocument();
    } else {
      await _synchronizeDocument();
    }
    return true;
  }

  Future<bool> _synchronizeFrameStoreEntry(
    LitchcraftV2State inspection, {
    bool retriedAuthority = false,
  }) async {
    final session = _orderedSession;
    final deviceId = _deviceId;
    if (session == null ||
        !session.isActivated ||
        deviceId == null ||
        session.objectMaximumBody < 5) {
      return false;
    }
    _storeSessionBoardId = deviceId;
    final generation = _generation;
    final selectionIntent = _selectionIntent;
    final linkToken = _linkToken;
    final sourceGeneration = documentSession.replacementGeneration;
    _storeEntryInspection = inspection;
    final entryKind = _localFrameStoreDocument ? 'warm' : 'cold';
    void tracePhase(String phase, String stage, {int? startedAt}) {
      _appTrace?.record(
        'litchcraft.store_entry_phase',
        fields: {
          'phase': phase,
          'stage': stage,
          'entry_kind': entryKind,
          'generation': generation,
          'connection_epoch': linkToken,
          'source_generation': sourceGeneration,
          if (startedAt != null) 'elapsed_ms': _advancedTimelineNow - startedAt,
        },
      );
    }

    _frameStoreReadPending = true;
    _notify();
    bool current() =>
        !_disposed &&
        _selected &&
        generation == _generation &&
        selectionIntent == _selectionIntent &&
        linkToken == _linkToken &&
        identical(session, _orderedSession) &&
        deviceId == _deviceId &&
        bleService.isConnected &&
        sourceGeneration == documentSession.replacementGeneration;
    final client = _frameStoreClient(session);
    LitchcraftFrameStoreState state;
    LitchcraftFrameStoreResidents? residents;
    final inspectStartedAt = _advancedTimelineNow;
    tracePhase('inspect', 'start');
    try {
      state = await client.inspect(
        session: inspection.session,
        request: _requestId = (_requestId % 0xffff) + 1,
      );
      tracePhase('inspect', 'complete', startedAt: inspectStartedAt);
      if (!current()) return true;
    } on LitchcraftRejected catch (error) {
      if (!current()) return true;
      if (error.status != 4) rethrow;
      _frameStoreReadPending = false;
      _status = LitchcraftStatus.unsupported;
      _notify();
      return true;
    }
    residents = state.residentListing;
    if (!current()) return true;
    _cacheFrameStoreInspection(
      state: state,
      residents: residents,
      linkToken: linkToken,
    );
    if (!state.hasArtwork) {
      _pendingStoreEntryInspection = null;
      _frameStoreReadPending = false;
      _notify();
      return false;
    }
    _storeRootMissing = false;

    _StoreAuthority? authority;
    if (session.storeGuardEpochSupported) {
      final authorityStartedAt = _advancedTimelineNow;
      tracePhase('fresh_state37', 'start');
      try {
        authority = await _requestFreshStoreAuthority(session);
        tracePhase('fresh_state37', 'complete', startedAt: authorityStartedAt);
      } catch (error) {
        if (current()) _failFrameStoreEntry(error);
        return true;
      }
    } else {
      _frameStoreReadPending = false;
      _status = LitchcraftStatus.unsupported;
      _notify();
      return true;
    }
    if (!current()) return true;
    if (authority != null && authority.generation != state.generation) {
      if (!retriedAuthority) {
        return _synchronizeFrameStoreEntry(inspection, retriedAuthority: true);
      }
      _failFrameStoreEntry(const LitchcraftResyncRequired());
      return true;
    }
    final common = _storeCommonAuthority;
    final confirmed = documentSession.boardDocument;
    final source = documentSession.document;
    if (authority != null &&
        authority.dirty &&
        common != null &&
        confirmed != null &&
        _unconfirmedDocument == null &&
        !_storeTransferInFlight &&
        !_acceptedStoreCommitAwaitingRoot &&
        (_interruptedStoreReplacement == null ||
            _interruptedStoreReplacement!.committed != null) &&
        authority.deviceId == deviceId &&
        common.deviceId == deviceId &&
        authority.bootNonce == session.boardBootNonce &&
        common.bootNonce == authority.bootNonce &&
        common.revision == authority.revision &&
        authority.generation == state.generation &&
        state.frameCount == source.frames.length &&
        state.topology == source.topology.id &&
        state.selectedFrame < source.frames.length &&
        _sameStorePreimage(confirmed, source)) {
      // The retained exact receipt proves these live bytes across this same
      // boot. Saving may still trail the accepted layout; a saved descriptor
      // cannot improve that proof or justify replacing the current artwork.
      _pendingStoreEntryInspection = null;
      _frameStoreReadPending = false;
      _localFrameStoreDocument = true;
      _advancedWire = false;
      _sessionId = _documentSessionId = state.session;
      _documentRevision = inspection.revision;
      _boardActive = state.active;
      _frameStoreGeneration = authority.generation;
      _frameStorePlaying = state.playing;
      _frameStoreStartedAt =
          _advancedTimelineNow -
          (litchcraftAnimationFrameStartOrNull(source, state.selectedFrame) ??
              0);
      _storeHistoryEcho = null;
      _storeCommonAuthority = authority;
      _storeCommonBindingGeneration = null;
      _advancedSelectedFrame = state.selectedFrame;
      documentSession.selectFrame(state.selectedFrame);
      _status = state.active ? LitchcraftStatus.live : LitchcraftStatus.offline;
      _notify();
      unawaited(_requestFrameStoreRuntimeState(session));
      return true;
    }
    if (authority == null ||
        authority.dirty ||
        authority.generation != state.generation ||
        authority.bootNonce != session.boardBootNonce) {
      // Inspect and resident metadata describe the committed root. A fresh
      // clean live authority must follow Inspect before they can decide which
      // artwork the user sees or whether a chooser is warranted.
      _pendingStoreEntryInspection = inspection;
      _sessionId = _documentSessionId = state.session;
      _frameStoreGeneration = state.generation;
      _boardActive = state.active;
      _status = state.active ? LitchcraftStatus.live : LitchcraftStatus.offline;
      if (authority != null &&
          authority.dirty &&
          authority.bootNonce == session.boardBootNonce &&
          authority.generation == state.generation &&
          state.active) {
        // The committed CRC cannot describe unknown live edits. Reuse the
        // live repair owner instead of waiting for flash to become clean.
        retireConflictBoardPreview();
        _pendingFrameStoreConflict = null;
        _localFrameStoreDocument = true;
        _advancedWire = false;
        _documentRevision = inspection.revision;
        unawaited(_repairPhysicalStoreEdits());
      }
      _notify();
      return true;
    }
    _pendingStoreEntryInspection = null;
    if (_storeTransferInFlight &&
        _deliberateDeliveryGeneration == sourceGeneration) {
      // The selected replacement already owns this lane and its root decision.
      // A parallel entry repair cannot turn its predecessor into a chooser.
      _frameStoreReadPending = false;
      _notify();
      return true;
    }

    LitchcraftAnimationDocument? verifiedCommonBoard;
    // A resident listing is metadata only. If it proves that the app's
    // current document is already committed, refresh runtime authority from
    // that proof and skip the expensive artwork snapshot entirely. This is the
    // reconnect case that previously performed a duplicate board read before
    // showing the same artwork the app already owned.
    if (residents != null &&
        residents.session == state.session &&
        residents.generation == state.generation &&
        residents.selectedRootSlot != null &&
        (state.selectedRootSlot == null ||
            residents.selectedRootSlot == state.selectedRootSlot)) {
      final source = documentSession.document;
      final canonicalStartedAt = _advancedTimelineNow;
      final cached = _cachedFrameStoreContentIdentity(source);
      final LitchcraftFrameStoreContentIdentity candidate;
      if (cached == null) {
        tracePhase('canonical_prepare', 'start');
        candidate = LitchcraftFrameStoreProtocol.identify(source);
        _rememberFrameStoreContentIdentity(source, candidate);
        tracePhase(
          'canonical_prepare',
          'complete',
          startedAt: canonicalStartedAt,
        );
      } else {
        candidate = cached;
        tracePhase(
          'canonical_cache',
          'complete',
          startedAt: canonicalStartedAt,
        );
      }
      final selectedRootSlot = residents.selectedRootSlot;
      final matching = residents.residents
          .where(
            (resident) =>
                resident.rootSlot == selectedRootSlot &&
                resident.matchesContent(candidate),
          )
          .toList();
      if (matching.isNotEmpty) {
        _appTrace?.record(
          'litchcraft.frame_store_snapshot_read',
          fields: {
            'read_kind': 'entry_metadata',
            'session': state.session,
            'generation': state.generation,
            'frames': source.frames.length,
            'body_bytes': 0,
            'chunks': 0,
            'cache_hits': source.frames.length,
          },
        );
        // Favorite colors are an editor palette. Different local favorites
        // do not make identical frame bytes into two competing artworks.
        // An explicit full-document transfer may publish the app's palette.
        retireConflictBoardPreview();
        _pendingFrameStoreConflict = null;
        _reconcileKnownFrameStoreResidents(
          session: state.session,
          linkToken: linkToken,
          state: residents,
          document: source,
          contentIdentity: candidate,
        );
        _localFrameStoreDocument = true;
        _advancedWire = false;
        _frameStoreReadPending = false;
        _sessionId = _documentSessionId = state.session;
        _documentRevision = inspection.revision;
        _boardActive = state.active;
        _frameStoreGeneration = state.generation;
        _frameStorePlaying = state.playing;
        _frameStoreStartedAt =
            _advancedTimelineNow -
            (litchcraftAnimationFrameStartOrNull(source, state.selectedFrame) ??
                0);
        _selectedFrameStoreRootSlot = matching.first.rootSlot;
        _storeHistoryEcho = null;
        documentSession.confirm(deviceId, source);
        _settleInterruptedStoreReplacement(state.generation);
        _storeCommonBindingGeneration = state.generation;
        _bindStoreCommonAuthorityIfReady();
        if (state.selectedFrame < state.frameCount) {
          _advancedSelectedFrame = state.selectedFrame;
        }
        if (state.selectedFrame < source.frames.length) {
          documentSession.selectFrame(state.selectedFrame);
        }
        _status = state.active
            ? LitchcraftStatus.live
            : LitchcraftStatus.offline;
        _notify();
        unawaited(_requestFrameStoreRuntimeState(session));
        return true;
      }
      if (common != null &&
          confirmed != null &&
          !_storeTransferInFlight &&
          !_acceptedStoreCommitAwaitingRoot &&
          documentSession.hasCommonDocument &&
          common.deviceId == deviceId &&
          common.bootNonce == authority.bootNonce &&
          common.revision == authority.revision &&
          _documentSessionId == state.session) {
        final confirmedIdentity =
            _cachedFrameStoreContentIdentity(confirmed) ??
            LitchcraftFrameStoreProtocol.identify(confirmed);
        _rememberFrameStoreContentIdentity(confirmed, confirmedIdentity);
        if (residents.residents.any(
          (resident) =>
              resident.rootSlot == selectedRootSlot &&
              resident.matchesContent(confirmedIdentity),
        )) {
          // Fresh clean authority and the selected resident prove the retained
          // board preimage. A failed later Paint does not create board edits;
          // let common-document policy reconcile the preserved phone intent.
          verifiedCommonBoard = confirmed;
        }
      }
    }
    final interrupted = _interruptedStoreReplacement;
    final resumeInterruptedReplacement =
        residents != null &&
        interrupted != null &&
        !interrupted.consumed &&
        interrupted.boardId == deviceId &&
        interrupted.bootNonce == session.boardBootNonce &&
        interrupted.sourceGeneration == sourceGeneration &&
        _deliberateDeliveryGeneration == sourceGeneration &&
        authority.generation == interrupted.base.generation &&
        residents.residents.any(
          (resident) =>
              resident.rootSlot == residents!.selectedRootSlot &&
              _sameFrameStoreResidentIdentity(resident, interrupted.base),
        );
    final sendPreservedEdits =
        documentSession.hasLocalIntent &&
        verifiedCommonBoard != null &&
        documentSession.observeBoard(
              deviceId,
              _storeArtworkWithLocalPalette(verifiedCommonBoard),
            ) ==
            LitchcraftDocumentResolution.sendApp;
    if (resumeInterruptedReplacement || sendPreservedEdits) {
      // Both proofs name an unchanged predecessor. Existing Store admission
      // and Commit still own adoption, not this metadata comparison.
      _localFrameStoreDocument = true;
      _advancedWire = false;
      _sessionId = _documentSessionId = state.session;
      _documentRevision = inspection.revision;
      _boardActive = state.active;
      _frameStoreGeneration = state.generation;
      _frameStorePlaying = state.playing;
      _frameStoreReadPending = sendPreservedEdits;
      if (resumeInterruptedReplacement) {
        _appTrace?.record(
          'litchcraft.interrupted_store_replacement_resumed',
          fields: {
            'source_generation': sourceGeneration,
            'base_generation': interrupted.base.generation,
          },
        );
        interrupted.consumed = true;
      }
      // A warm refresh already owns the command lane. Submit the replacement
      // behind it, then release that owner instead of awaiting its own tail.
      // Consume predecessor proof once. Keep the same operation's first error
      // until phone persistence can observe its pending or settled outcome.
      unawaited(
        _transferFrameStore(
          frames: documentSession.document.frames,
          playbackOnEntry: documentSession.document.playbackOnEntry,
          replacementGeneration: sourceGeneration,
        ).then<void>(
          (_) {
            if (!current()) return;
            _errorMessage = null;
            _frameStoreReadPending = false;
            _status = LitchcraftStatus.live;
            _recoveryAttempt = 0;
            _notify();
          },
          onError: (Object error, StackTrace _) {
            if (current() && error is! LitchcraftTransferCancelled) {
              _transportFailed(error);
            }
          },
        ),
      );
      return true;
    }
    if (residents != null && documentSession.hasLocalIntent) {
      // Different resident metadata cannot prove whether the board is empty.
      // Only a one-frame Store has bounded whole-artwork proof; animations
      // retain their source choice without an implicit full body read.
      retireConflictBoardPreview();
      _pendingFrameStoreConflict = _PendingFrameStoreConflict(
        session: session,
        authoritySession: state.session,
        observedFrameCount: state.frameCount,
        entryObservation: state.frameCount == 1
            ? (state: state, authority: authority)
            : null,
        deviceId: deviceId,
        generation: generation,
        selectionIntent: selectionIntent,
        linkToken: linkToken,
        sourceGeneration: documentSession.replacementGeneration,
        bootNonce: session.boardBootNonce,
      );
      _localFrameStoreDocument = true;
      _advancedWire = false;
      _frameStoreReadPending = false;
      _sessionId = _documentSessionId = state.session;
      _documentRevision = inspection.revision;
      _boardActive = state.active;
      _frameStoreGeneration = state.generation;
      _frameStorePlaying = state.playing;
      _frameStoreStartedAt = _advancedTimelineNow;
      if (state.frameCount == 1 && state.active) {
        final pending = _pendingFrameStoreConflict!;
        _frameStoreReadPending = true;
        _status = LitchcraftStatus.live;
        _notify();
        await prepareConflictBoardPreview();
        if (!current() || !_currentConflictPreview(pending)) return true;
        final board = pending.previewSnapshot?.document;
        if (_verifiedConflictPreview(pending) && board != null) {
          final useBoard = switch (documentSession.observeBoard(
            deviceId,
            _storeArtworkWithLocalPalette(board),
          )) {
            LitchcraftDocumentResolution.equal ||
            LitchcraftDocumentResolution.adoptBoard => true,
            LitchcraftDocumentResolution.sendApp => false,
            LitchcraftDocumentResolution.merge ||
            LitchcraftDocumentResolution.conflict => null,
          };
          if (useBoard != null) {
            // The document session owns common lineage and empty-board policy.
            // Queue behind entry rather than awaiting the command tail we own.
            unawaited(
              _resolvePendingFrameStoreConflict(
                useBoard: useBoard,
                preserveLocalPalette: true,
              ).catchError((Object error) {
                if (current() && error is! LitchcraftTransferCancelled) {
                  _frameStoreReadPending = false;
                  _transportFailed(error);
                }
              }),
            );
            return true;
          }
        }
        _frameStoreReadPending = false;
      }
      _status = LitchcraftStatus.conflict;
      debugPrint(
        'Litchcraft Store source choice: reason=resident_mismatch '
        'generation=${state.generation} frames=${state.frameCount} '
        'local_generation=${documentSession.replacementGeneration} '
        'epoch=$linkToken',
      );
      _notify();
      return true;
    }
    // Inspect is enough to restore runtime authority. Full artwork recovery is
    // bulk reconciliation and must not make an already-playing board appear
    // paused or unusable while the snapshot drains behind interactive traffic.
    _localFrameStoreDocument = true;
    _advancedWire = false;
    _sessionId = _documentSessionId = state.session;
    _documentRevision = inspection.revision;
    _boardActive = state.active;
    _frameStoreGeneration = state.generation;
    _frameStorePlaying = state.playing;
    _frameStoreStartedAt = _advancedTimelineNow;
    if (state.selectedFrame < state.frameCount) {
      _advancedSelectedFrame = state.selectedFrame;
      if (state.frameCount == documentSession.document.frames.length) {
        documentSession.selectFrame(state.selectedFrame);
      }
    }
    _status = state.active ? LitchcraftStatus.live : LitchcraftStatus.offline;
    _notify();
    unawaited(
      _reconcileFrameStoreSnapshot(
        client: client,
        session: session,
        state: state,
        deviceId: deviceId,
        generation: generation,
        selectionIntent: selectionIntent,
        linkToken: linkToken,
        sourceGeneration: sourceGeneration,
      ),
    );
    return true;
  }

  void _failFrameStoreEntry(Object error) {
    // The typed query/read has already exhausted or rejected this operation.
    // Keep the healthy shared session and expose the existing explicit retry.
    _pendingStoreEntryInspection = null;
    _frameStoreReadPending = true;
    _physicalStoreRepairTimer?.cancel();
    _physicalStoreRepairTimer = null;
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    _recoveryAttempt = _recoveryDelays.length;
    _errorMessage = error.toString();
    _status = bleService.isConnected
        ? LitchcraftStatus.error
        : LitchcraftStatus.offline;
    _notify();
  }

  Future<void> _requestFrameStoreRuntimeState(ConnectionSession session) async {
    try {
      // A retained playback sample can precede coherent mode/artwork admission.
      // Replay it now; duplicate sequence preserves its first observation time.
      await session.requestState(code: ConnectionSession.litchcraftStateCode);
      await session.requestState(
        code: ConnectionSession.litchcraftStoreEditStateCode,
      );
    } catch (_) {
      // These retained runtime keys are also published by their existing owner.
    }
  }

  LitchcraftAnimationDocument _storeArtworkWithLocalPalette(
    LitchcraftAnimationDocument board,
  ) {
    final favorites = documentSession.document.favoriteColors;
    if (listEquals(board.favoriteColors, favorites)) return board;
    return LitchcraftAnimationDocument(
      frames: board.frames,
      topology: board.topology,
      favoriteColors: favorites,
      playbackOnEntry: board.playbackOnEntry,
      sparkleSeed: board.sparkleSeed,
    );
  }

  Future<_StoreAuthority?> _requestFreshStoreAuthority(
    ConnectionSession session,
  ) async {
    final seenSequence = _storeAuthorityStateSequence;
    final deviceId = _deviceId;
    if (deviceId == null || !identical(session, _orderedSession)) return null;
    final packet = await session.readState(
      code: ConnectionSession.litchcraftStoreAuthorityStateCode,
      earliestSequence: seenSequence,
      accept: (packet) =>
          packet.body.length == 10 &&
          packet.body[0] == 1 &&
          (packet.body[1] & ~1) == 0,
    );
    final bytes = ByteData.sublistView(packet.body);
    return (
      deviceId: deviceId,
      bootNonce: session.boardBootNonce,
      generation: bytes.getUint32(2, Endian.little),
      revision: bytes.getUint32(6, Endian.little),
      dirty: (packet.body[1] & 1) != 0,
    );
  }

  Future<void> _reconcileFrameStoreSnapshot({
    required LitchcraftFrameStoreClient client,
    required ConnectionSession session,
    required LitchcraftFrameStoreState state,
    required String deviceId,
    required int generation,
    required int selectionIntent,
    required int linkToken,
    required int sourceGeneration,
    bool forceBoard = false,
    bool preserveLocalPalette = false,
    LitchcraftFrameStoreSnapshot? verifiedSnapshot,
  }) async {
    final startedAt = _advancedTimelineNow;
    final reverseStartedAt = session.reverseObjectConfirmedThrough;
    debugPrint(
      'Litchcraft Store snapshot started: generation=${state.generation} '
      'frames=${state.frameCount}',
    );
    late final LitchcraftFrameStoreSnapshot snapshot;
    final authorityBefore = _storeAuthority;
    final previewOwner = verifiedSnapshot == null
        ? null
        : _pendingFrameStoreConflict;
    final fullSync = Object();
    if (verifiedSnapshot == null && state.frameCount > 1) {
      _startFullAnimationSync(
        fullSync,
        LitchcraftFullSyncDirection.readingFromBoard,
      );
    }
    if (verifiedSnapshot == null) {
      _frameStoreSnapshotRead = (
        owner: fullSync,
        generation: generation,
        selectionIntent: selectionIntent,
        linkToken: linkToken,
        sourceGeneration: sourceGeneration,
      );
      _notify();
    }
    try {
      if (verifiedSnapshot != null) {
        _appTrace?.record(
          'litchcraft.frame_store_snapshot_read',
          fields: {
            'read_kind': 'verified_cache',
            'session': verifiedSnapshot.session,
            'generation': verifiedSnapshot.generation,
            'frames': verifiedSnapshot.document.frames.length,
            'body_bytes': 0,
            'chunks': 0,
            'cache_hits': verifiedSnapshot.document.frames.length,
          },
        );
      }
      snapshot =
          verifiedSnapshot ??
          await client.snapshot(
            session: state.session,
            generation: state.generation,
            allocateRequest: () => _requestId = (_requestId % 0xffff) + 1,
            onProgress: state.frameCount > 1
                ? (completed, total) =>
                      _updateFullAnimationSync(fullSync, completed, total)
                : null,
            onRetry: (cursor, attempt) => debugPrint(
              'Litchcraft Store snapshot response timeout: '
              'cursor=$cursor attempt=$attempt '
              'reverse_confirmed='
              '${session.reverseObjectConfirmedThrough - reverseStartedAt} '
              'grant=${session.reverseObjectGrantThrough}',
            ),
          );
    } catch (error) {
      final activeRead =
          !_disposed &&
          generation == _generation &&
          selectionIntent == _selectionIntent &&
          linkToken == _linkToken &&
          sourceGeneration == documentSession.replacementGeneration &&
          identical(session, _orderedSession) &&
          deviceId == _deviceId;
      debugPrint(
        'Litchcraft Store snapshot failed after '
        '${_advancedTimelineNow - startedAt}ms: '
        '${error.runtimeType}: $error; active=$activeRead '
        'reverse_confirmed='
        '${session.reverseObjectConfirmedThrough - reverseStartedAt} '
        'capacity=${session.reverseObjectSelectedCapacity} '
        'grant=${session.reverseObjectGrantThrough}',
      );
      if (activeRead) {
        _appTrace?.record(
          'litchcraft.frame_store_snapshot_deferred',
          fields: <String, Object?>{
            'generation': generation,
            'kind': error.runtimeType.toString(),
          },
        );
        // Runtime Inspect and Timeline remain authoritative. Only an explicit
        // Use board action needs a visible failure and retained retry choice.
        if (forceBoard) rethrow;
        if (_frameStoreReadPending) {
          _transportFailed(error);
        }
        return;
      }
      return;
    } finally {
      if (identical(_frameStoreSnapshotRead?.owner, fullSync)) {
        _frameStoreSnapshotRead = null;
        _notify();
      }
      _finishFullAnimationSync(fullSync);
    }
    if (_disposed ||
        generation != _generation ||
        selectionIntent != _selectionIntent ||
        linkToken != _linkToken ||
        sourceGeneration != documentSession.replacementGeneration ||
        !identical(session, _orderedSession) ||
        deviceId != _deviceId) {
      return;
    }
    if (snapshot.generation != _frameStoreGeneration ||
        snapshot.session != _documentSessionId) {
      if (forceBoard) {
        throw StateError('Board artwork changed during read. Try again.');
      }
      if (_frameStoreReadPending) {
        _transportFailed(StateError('Board artwork changed during read.'));
      }
      return;
    }
    _StoreAuthority? authorityAfter;
    try {
      authorityAfter = await _requestFreshStoreAuthority(session);
    } catch (error) {
      debugPrint(
        'Litchcraft Store post-read authority query failed after '
        '${_advancedTimelineNow - startedAt}ms: $error',
      );
      // A saved-root body cannot stand in for unobserved live artwork.
    }
    if (_disposed ||
        generation != _generation ||
        selectionIntent != _selectionIntent ||
        linkToken != _linkToken ||
        sourceGeneration != documentSession.replacementGeneration ||
        !identical(session, _orderedSession) ||
        deviceId != _deviceId) {
      return;
    }
    if (verifiedSnapshot != null &&
        (previewOwner == null ||
            !identical(_pendingFrameStoreConflict, previewOwner) ||
            !identical(previewOwner.previewSnapshot, verifiedSnapshot) ||
            !_verifiedConflictPreview(previewOwner))) {
      if (previewOwner == null || !_currentConflictPreview(previewOwner)) {
        return;
      }
      throw StateError('Board artwork changed during read. Try again.');
    }
    if (!forceBoard && _pendingFrameStoreConflict != null) return;
    if (authorityBefore == null ||
        (authorityBefore.dirty &&
            !_liveSnapshotMatchesAuthority(
              verifiedSnapshot,
              authorityBefore,
            )) ||
        authorityAfter == null ||
        (authorityAfter.dirty &&
            !_liveSnapshotMatchesAuthority(verifiedSnapshot, authorityAfter)) ||
        authorityBefore.revision != authorityAfter.revision ||
        authorityAfter.generation != snapshot.generation ||
        authorityAfter.bootNonce != session.boardBootNonce) {
      debugPrint(
        'Litchcraft Store post-read authority rejected after '
        '${_advancedTimelineNow - startedAt}ms: '
        'before=$authorityBefore after=$authorityAfter '
        'snapshot=${snapshot.generation} boot=${session.boardBootNonce}',
      );
      if (forceBoard) {
        throw StateError('Board artwork changed during read. Try again.');
      }
      _pendingStoreEntryInspection = _storeEntryInspection;
      _status = _boardActive ? LitchcraftStatus.live : LitchcraftStatus.offline;
      _notify();
      unawaited(
        session
            .requestState(
              code: ConnectionSession.litchcraftStoreAuthorityStateCode,
            )
            .catchError((Object _) {}),
      );
      return;
    }
    if (!forceBoard &&
        _retainAcceptedArtworkForUnexpectedResident(
          session: session,
          state: state,
          deviceId: deviceId,
          document: snapshot.document,
        )) {
      return;
    }
    // Snapshot body is immutable, but its first-chunk runtime fields may be
    // older than a Pause or selection accepted while the body was downloading.
    _forgetSelectedFrameStoreResident();
    _frameStoreReadPending = false;
    _storeHistoryEcho = null;
    final artwork = forceBoard && !preserveLocalPalette
        ? snapshot.document
        : _storeArtworkWithLocalPalette(snapshot.document);
    if (forceBoard) {
      documentSession.observeBoard(deviceId, artwork);
      documentSession.acceptBoard(
        expectedGeneration: documentSession.generation,
      );
      _deliberateDeliveryGeneration = -1;
      _storeRootMissing = false;
      _errorMessage = null;
    } else {
      final resolution = documentSession.observeBoard(deviceId, artwork);
      if (resolution == LitchcraftDocumentResolution.equal) {
        documentSession.confirm(deviceId, artwork);
      } else if (resolution == LitchcraftDocumentResolution.adoptBoard) {
        documentSession.acceptBoard(
          expectedGeneration: documentSession.generation,
        );
      } else {
        _status = LitchcraftStatus.conflict;
        _notify();
        return;
      }
    }
    retireConflictBoardPreview();
    _pendingFrameStoreConflict = null;
    if (_liveSnapshotMatchesAuthority(snapshot, authorityAfter)) {
      _storeCommonAuthority = authorityAfter;
      _storeCommonBindingGeneration = null;
    } else {
      _storeCommonBindingGeneration = snapshot.generation;
      _bindStoreCommonAuthorityIfReady();
    }
    if (_advancedSelectedFrame < snapshot.document.frames.length) {
      documentSession.selectFrame(_advancedSelectedFrame);
    }
    // Keep the known selected frame visible while retained timing is replayed.
    _frameStoreStartedAt =
        _advancedTimelineNow -
        (litchcraftAnimationFrameStartOrNull(
              documentSession.document,
              _advancedSelectedFrame,
            ) ??
            0);
    _board = documentSession.canvas.withTopology(snapshot.document.topology);
    _canvas = _board;
    _detached = _board;
    _offlineDiverged = false;
    _status = _boardActive ? LitchcraftStatus.live : LitchcraftStatus.offline;
    _notify();
    debugPrint(
      'Litchcraft Store snapshot completed in '
      '${_advancedTimelineNow - startedAt}ms: '
      'generation=${snapshot.generation} '
      'frames=${snapshot.document.frames.length} '
      'reverse_confirmed='
      '${session.reverseObjectConfirmedThrough - reverseStartedAt}',
    );
    unawaited(_requestFrameStoreRuntimeState(session));
  }

  Future<void> _synchronizeDocument({bool reconcile = true}) {
    final pending = _documentSyncFuture;
    if (pending != null) return pending;
    late final Future<void> work;
    work = _performDocumentSync(reconcile: reconcile).whenComplete(() {
      if (identical(_documentSyncFuture, work)) _documentSyncFuture = null;
    });
    _documentSyncFuture = work;
    return work;
  }

  Future<void> _performDocumentSync({required bool reconcile}) async {
    if (_documentSyncing) return;
    final client = _documentClient;
    if (client == null || !_advancedWire) return;
    _clearKnownAdvancedHistory();
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    _documentSyncing = true;
    _documentDirty = true;
    try {
      // The transport itself coalesces watermarks and caps snapshot recovery.
      final snapshot = await client.snapshot(
        session: _documentSessionId,
        maximumPacketBytes: _documentPacketBytes,
      );
      if (_disposed ||
          generation != _generation ||
          !identical(client, _documentClient) ||
          sourceGeneration != documentSession.replacementGeneration) {
        return;
      }
      final document = decodeLitchcraftAnimationDocument(
        snapshot.bytes,
        snapshot.crc32,
      );
      _documentRevision = snapshot.revision;
      _documentSessionId = snapshot.session;
      _coherentDocumentRevision = snapshot.revision;
      _coherentDocumentSession = snapshot.session;
      _coherentDocumentBoardId = _deviceId;
      _documentDirty = client.needsResync;
      if (_unconfirmedDocumentBoardId == _deviceId &&
          _unconfirmedDocument != null &&
          (sameLitchcraftDocument(_unconfirmedDocument!, document) ||
              (_unconfirmedPaintDocumentBase != null &&
                  matchesLitchcraftDocumentPatch(
                    _unconfirmedPaintDocumentBase!,
                    _unconfirmedDocument!,
                    document,
                  )))) {
        documentSession.confirm(_deviceId!, _unconfirmedDocument!);
        _unconfirmedDocument = null;
        _unconfirmedPaintDocumentBase = null;
      }
      documentSession.observeBoard(_deviceId!, document);
      if (_documentDirty) throw const LitchcraftResyncRequired();
      if (reconcile) await _reconcileDocument();
      _applyPendingAdvancedRuntimeMetadata();
      if (_advancedSelectedFrame < documentSession.document.frames.length) {
        _selectBoardFrame(_advancedSelectedFrame);
      }
    } finally {
      if (identical(client, _documentClient)) _documentSyncing = false;
    }
  }

  Future<void> _reconcileDocument() async {
    if (!_boardActive || !_selected) return;
    final board = documentSession.boardDocument;
    if (board == null) throw const LitchcraftResyncRequired();
    final result = documentSession.observeBoard(_deviceId!, board);
    switch (result) {
      case LitchcraftDocumentResolution.equal:
        documentSession.confirm(_deviceId!, board);
      case LitchcraftDocumentResolution.adoptBoard:
        documentSession.acceptBoard(
          expectedGeneration: documentSession.generation,
        );
      case LitchcraftDocumentResolution.sendApp:
      case LitchcraftDocumentResolution.merge:
        if (result == LitchcraftDocumentResolution.merge) {
          documentSession.mergeBoard();
        }
        if (_queued > 1 || _strokeSquares.isNotEmpty) {
          // Existing ordered intents still own these overlays. Replaying the
          // merged whole document here would race those accepted writes.
          _documentDirty = true;
        } else if (documentSession.hasCommonDocument &&
            !(_unconfirmedDocument != null &&
                _unconfirmedPaintDocumentBase == null) &&
            sameLitchcraftDocumentLayout(board, documentSession.document)) {
          await _sendDocumentDelta();
        } else {
          await _sendDocument();
          return;
        }
      case LitchcraftDocumentResolution.conflict:
        if (_queued > 1) {
          // An already ordered app write can still settle this field. Its
          // receipt establishes acceptance order before a choice is needed.
          _documentDirty = true;
          break;
        }
        _status = LitchcraftStatus.conflict;
        _notify();
        return;
    }
    _status = LitchcraftStatus.live;
    _recoveryAttempt = 0;
    _notify();
  }

  Future<LitchcraftV2Packet> _replaceBoardDocument(
    LitchcraftV2DocumentClient client,
    LitchcraftAnimationDocument source, {
    required int session,
    required int revision,
    bool inactiveStorage = false,
  }) async {
    final transfer = Object();
    final generation = _generation;
    final elapsed = Stopwatch()..start();
    _artworkTransfer = transfer;
    _artworkTransferProgress = null;
    final encoded = encodeLitchcraftAnimationDocument(source);
    _unconfirmedDocument = source;
    _unconfirmedPaintDocumentBase = null;
    _unconfirmedDocumentBoardId = _deviceId;
    var accepted = false;
    try {
      final ack = await client.replace(
        session: session,
        baseRevision: revision,
        document: encoded.bytes,
        crc32: encoded.crc32,
        maximumPacketBytes: _documentPacketBytes,
        inactiveStorage: inactiveStorage,
        onProgress: (staged, total) {
          if (_disposed ||
              generation != _generation ||
              !identical(_artworkTransfer, transfer)) {
            return;
          }
          _artworkTransferProgress = staged / total;
          notifyListeners();
        },
      );
      accepted = true;
      return ack;
    } finally {
      _appTrace?.record(
        'litchcraft.document_delivery',
        fields: {
          'elapsed_ms': elapsed.elapsedMilliseconds,
          'bytes': encoded.bytes.length,
          'maximum_payload': _documentPacketBytes,
          'accepted': accepted,
          'inactive_storage': inactiveStorage,
        },
      );
      if (identical(_artworkTransfer, transfer)) {
        _artworkTransfer = null;
        _artworkTransferProgress = null;
        if (!_disposed) notifyListeners();
      }
    }
  }

  Future<bool> _sendDocument({LitchcraftFrameEditTarget? target}) async {
    final client = _documentClient;
    if (client == null || !_boardActive) return false;
    if (target != null && !_matchesSelectedFrameEditTarget(target)) {
      return false;
    }
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final source = documentSession.document;
    final ack = await _replaceBoardDocument(
      client,
      source,
      session: target?.session ?? _documentSessionId,
      revision: target?.revision ?? _documentRevision,
    );
    if (_disposed ||
        generation != _generation ||
        !identical(client, _documentClient) ||
        sourceGeneration != documentSession.replacementGeneration) {
      return false;
    }
    _documentRevision = ack.revision;
    _coherentDocumentRevision = ack.revision;
    _coherentDocumentSession = ack.session;
    _coherentDocumentBoardId = _deviceId;
    _documentDirty = false;
    _frameStorePlaying = null;
    documentSession.confirm(_deviceId!, source);
    _unconfirmedDocument = null;
    _status = LitchcraftStatus.live;
    if (!sameLitchcraftDocument(source, documentSession.document)) {
      // The ACK proves an accepted prefix of local edits, not divergence from
      // an independent board edit. Deliver the newer draft in the same lane.
      unawaited(
        _enqueue(() async {
          await _sendDocument();
        }),
      );
    }
    _applyPendingAdvancedRuntimeMetadata();
    _notify();
    return _status == LitchcraftStatus.live;
  }

  Future<void> leave() {
    _interruptedStoreReplacement = null;
    final active = _exiting;
    if (active != null) return active;
    late final Future<void> leaving;
    leaving = _leave()
        .catchError((Object error) {
          _transportFailed(error);
          throw StateError('Board exit was not acknowledged: $error');
        })
        .whenComplete(() {
          if (identical(_exiting, leaving)) _exiting = null;
        });
    return _exiting = leaving;
  }

  Future<void> _leave() async {
    _gameEntryChoice = _gameEntryResetChoice = null;
    _gameEntryFailed = false;
    cancelBrushColorPreview();
    _cancelPendingStoreCommit();
    final Stopwatch elapsed = Stopwatch()..start();
    _appTrace?.record(
      'litchcraft.exit_start',
      fields: <String, Object?>{
        'board_active': _boardActive,
        'link_ready': _link != null && _linkUsable,
      },
    );
    ++_selectionIntent;
    _cancelNotificationRepair();
    _physicalStoreRepairTimer?.cancel();
    _physicalStoreRepairTimer = null;
    _physicalStoreRepairThroughRevision = 0;
    _physicalStoreRepairBinding = null;
    _cancelEntryDraftWait();
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    endStroke();
    // Admission ACK supplies the session identity. Orientation, snapshots and
    // artwork preparation after it cannot stand in front of a departure.
    ++_generation;
    _retireFrameStoreTransfer();
    _preservePendingStaticCanvas();
    _detached = _canvas;
    _offlineDiverged = _offlineDiverged || !_sameArtwork(_canvas, _board);
    _failSnapshot(const LitchcraftTransferCancelled());
    try {
      await _admission;
    } catch (_) {
      // Rejected admission owns no mode; observed active authority still exits.
    }
    _entering = null;
    _artworkDeliveryTimer?.cancel();
    _artworkDeliveryTimer = null;
    if (_boardActive) _exitOnReconnect = _deviceId;
    if (_boardActive && _link == null && bleService.isConnected) {
      await _bindLink(bleService.connectedId!, ++_generation);
    }
    if (_link != null && _boardActive) {
      await (() async {
        // Retain local edits for reconciliation; mode authority outranks
        // queued artwork and never needs a departure snapshot.
        final coherent =
            !_needsResync &&
            _snapshotDone == null &&
            _sameArtwork(_canvas, _board);
        final ack = await _link!.send(LitchcraftProtocol.exit, _sessionId);
        _appTrace?.record(
          'litchcraft.exit_acknowledged',
          fields: <String, Object?>{
            'elapsed_ms': elapsed.elapsedMilliseconds,
            'revision': ack.revision,
          },
        );
        // Exit advances the session revision but cannot mutate artwork. Consume
        // that acknowledged successor here so generic State handling does not
        // force a full canvas read on the next deliberate entry.
        _revision = ack.revision;
        _stateRevision = ack.revision;
        _needsResync = false;
        if (coherent) {
          _trustedExitCanvas = _board;
          _trustedExitBoardId = _deviceId;
          _trustedExitLinkToken = _linkToken;
          _trustedExitRevision = ack.revision;
        } else {
          _clearTrustedExit();
        }
        _boardActive = false;
        _exitOnReconnect = null;
      })();
    } else if (_boardActive && bleService.isConnected) {
      await _startPendingExit();
    }
    if (_boardActive && bleService.isConnected) {
      throw StateError('Board exit was not acknowledged');
    }
    // Retain board ownership until Exit is confirmed. A failed departure keeps
    // its exit token so background recovery retries without reopening the mode.
    _selected = false;
    _frameStoreReadPending = false;
    if (_status != LitchcraftStatus.error) _status = LitchcraftStatus.offline;
    _notify();
    _appTrace?.record(
      'litchcraft.exit_complete',
      fields: <String, Object?>{
        'elapsed_ms': elapsed.elapsedMilliseconds,
        'board_active': _boardActive,
      },
    );
  }

  Future<void> _bindLink(String id, int generation) async {
    final connectionToken = bleService.connectionSessionToken;
    if (_link != null &&
        _linkUsable &&
        _deviceId == id &&
        _linkToken == connectionToken) {
      return;
    }
    cancelBrushColorPreview();
    _cancelPendingStoreCommit();
    final old = _link;
    retireConflictBoardPreview();
    _pendingFrameStoreConflict = null;
    _clearCachedFrameStoreInspection();
    _knownFrameStoreResidents.clear();
    _selectedFrameStoreRootSlot = null;
    _deliveryAttempt = null;
    _documentClient?.cancel();
    _clearAdvancedTimeline(
      retainStoreProjection:
          _localFrameStoreDocument &&
          _boardActive &&
          _linkToken == connectionToken,
    );
    _documentClient = null;
    _documentSyncing = false;
    _documentSyncFuture = null;
    _link = null;
    await _connectionState?.cancel();
    _connectionState = null;
    _connectionStateRevision = 0;
    _connectionStateObservedAt = 0;
    _connectionPlaybackStateRevision = 0;
    _physicalStoreEditRevision = 0;
    _physicalStoreBatchRevision = 0;
    _storeHistoryEcho = null;
    _acceptedStoreStroke = null;
    _advancedCanUndo = _advancedCanRedo = false;
    _projectedPhysicalStoreLiveRevision = 0;
    _physicalStoreRepairThroughRevision = 0;
    _physicalStoreRepairTimer?.cancel();
    _physicalStoreRepairTimer = null;
    _physicalStoreRepairing = false;
    _physicalStoreReadbackToken = null;
    _physicalStoreRepairBinding = null;
    _orderedSession = null;
    _linkUsable = false;
    await _prepareConnection(_disposeLink(old), 'notification_cleanup');
    if (generation != _generation || !bleService.isConnected) return;
    if (_linkToken != connectionToken) {
      _connectionRestartRequested = false;
      _linkToken = connectionToken;
      _requestId = 0;
      _clearPhysicalShakeErase(resetDedupe: true);
    }
    if (bleService.deviceCapabilities(id).supportsConnectionSession) {
      _orderedSession = await _prepareConnection(
        bleService.openConnectionSession(id),
        'connection_session_activation',
      );
      await _bindConnectionState(_orderedSession);
      if (generation != _generation || !bleService.isConnected) return;
    }
    late final LitchcraftLink link;
    link = LitchcraftLink(
      packets: bleService.litchcraftPackets(id),
      allocateRequestId: () => _requestId = (_requestId % 0xffff) + 1,
      monotonicMilliseconds: () => _advancedTimelineNow,
      write: (bytes) {
        final writeIsCurrent = link.v2WriteValidity(bytes);
        return bytes.first == LitchcraftV2Protocol.magic
            ? bleService.writeLitchcraftV2Packet(
                id,
                bytes,
                connectionSessionToken: connectionToken,
                isCurrent: () => identical(_link, link) && writeIsCurrent(),
                cancellationFuture: link.v2WriteRetirement(bytes),
              )
            : bleService.writeLitchcraftPacket(id, bytes);
      },
      writeUnreliable: (bytes) =>
          bleService.writeLitchcraftPacket(id, bytes, withResponse: false),
      writeBulkArtwork: (session, request, colors) =>
          bleService.writeLitchcraftBulkArtwork(id, session, request, colors),
      writeBulkDocumentPacket: (bytes) =>
          bleService.writeLitchcraftBulkDocumentPacket(
            id,
            bytes,
            connectionSessionToken: connectionToken,
          ),
      onPacket: (packet) {
        if (identical(_link, link)) _receive(packet);
      },
      onV2Packet: (packet) {
        if (identical(_link, link)) _receiveAdvancedDocumentPacket(packet);
      },
      onFailure: (error) {
        if (identical(_link, link)) {
          _linkUsable = false;
          _transportFailed(error);
        }
      },
      onDiagnostic: (event, fields) {
        if (!identical(_link, link)) return;
        _appTrace?.record(
          'litchcraft.link_$event',
          fields: <String, Object?>{
            ...fields,
            'generation': _generation,
            'connection_epoch': _linkToken,
          },
        );
      },
    );
    _link = link;
    _advancedTimelineSubscription = link.v2TimelineSamples.listen((sample) {
      if (_disposed ||
          !identical(_link, link) ||
          connectionToken != _linkToken) {
        return;
      }
      // Store playback uses shared State's selected frame and frame-local
      // elapsed anchor. Its legacy Timeline elapsed starts at zero on seek,
      // so it cannot describe the cumulative position in the local document.
      // The separate effect-clock observer remains active for Store artwork.
      if (usesFrameStoreDocument) return;
      _receiveAdvancedTimeline(sample);
    });
    _linkUsable = true;
  }

  void _clearAdvancedTimeline({
    bool disposeObserver = true,
    bool retainStoreProjection = false,
  }) {
    _clearKnownAdvancedHistory();
    if (disposeObserver) {
      unawaited(
        _advancedTimelineSubscription?.cancel() ?? Future<void>.value(),
      );
      _advancedTimelineSubscription = null;
    }
    if (!retainStoreProjection) _advancedTimeline = null;
    _boardTransferSuspended = false;
    _transferEffectClockSource = null;
    _transferEffectClock = null;
    _pendingAdvancedTimeline = null;
    _storeTimelineCommand = null;
    _storeTimelineProjection = null;
    _storePausedIntent = null;
    _advancedPlaying = false;
    if (!retainStoreProjection) {
      _frameStorePlaying = null;
      _frameStoreGeneration = 0;
    }
    _clearCachedFrameStoreInspection();
    // Retain the last displayed/editable frame when board authority goes away.
    // Resetting to zero here made a detached canvas, its selection, and a
    // paused preview disagree after a physical Exit.
    _advancedSelectedFrame = documentSession.selectedFrameIndex;
  }

  bool _receiveAdvancedTimeline(LitchcraftV2TimelineSample sample) {
    if (_pendingFrameStoreConflict != null) return false;
    if (!_advancedWire) {
      if (!usesFrameStoreDocument ||
          sample.session != _documentSessionId ||
          sample.revision != _frameStoreGeneration ||
          sample.selectedFrame >= documentSession.document.frames.length) {
        return false;
      }
      // A known Canvas preview already captured and paused its edit frame.
      // Retained pre-action runtime State cannot restart its playback.
      if (_optimisticDocumentEdits > 0) return false;
      if (animationTimelinePending) {
        _pendingAdvancedTimeline = sample;
        return false;
      }
    } else {
      if (sample.session != _documentSessionId) return false;
      if (sample.revision != _documentRevision ||
          sample.revision != _coherentDocumentRevision) {
        if (_newerDocumentRevision(sample.revision, _documentRevision)) {
          _pendingAdvancedTimeline = sample;
          _observeAdvancedRevisionWatermark(sample.revision);
          return true;
        }
        return false;
      }
      if (sample.selectedFrame >= documentSession.document.frames.length) {
        return false;
      }
      if (_optimisticDocumentEdits > 0) {
        _pendingAdvancedTimeline = sample;
        return true;
      }
    }
    final previous = _advancedTimeline;
    if (usesFrameStoreDocument) {
      // Manual frame selection belongs to the app. An ordered receipt can
      // precede its State, so a delayed paused predecessor must not select the
      // next paint target. Layout edits already update the document selection.
      // Local Pause/Select also fence preceding Play feedback after receipt.
      // A later explicit Play supersedes that intent even if preceding history
      // work still projects a pause. New connections adopt their own timeline.
      final pausedIntent = _storePausedIntent == _playbackIntentGeneration;
      if ((!sample.playing &&
              (pausedIntent || previous?.playing == false) &&
              sample.selectedFrame != documentSession.selectedFrameIndex) ||
          (sample.playing && pausedIntent)) {
        return true;
      }
    }
    if (sample.playing &&
        !sample.transferSuspended &&
        previous != null &&
        previous.playing &&
        !previous.transferSuspended &&
        previous.sampleId != 0 &&
        previous.session == sample.session &&
        previous.revision == sample.revision) {
      final now = _advancedTimelineNow;
      if (now - previous.receivedAtMonotonicMilliseconds <
          _playbackClockSyncInterval.inMilliseconds) {
        return true;
      }
      final projected = _playbackTimelineElapsedAt(previous, now);
      final cycleMilliseconds = documentSession.document.frames.fold<int>(
        0,
        (total, frame) => total + frame.durationMs,
      );
      // A sample retained while its artwork was being admitted keeps its
      // original observation time. Project it before comparing clock phase.
      final observedElapsed =
          sample.elapsedMilliseconds +
          (now - sample.receivedAtMonotonicMilliseconds).clamp(0, now).toInt();
      var phaseError =
          observedElapsed % cycleMilliseconds - projected % cycleMilliseconds;
      if (phaseError > cycleMilliseconds ~/ 2) {
        phaseError -= cycleMilliseconds;
      } else if (phaseError < -(cycleMilliseconds ~/ 2)) {
        phaseError += cycleMilliseconds;
      }
      final correction = phaseError
          .clamp(
            -_playbackClockMaximumCorrectionMs,
            _playbackClockMaximumCorrectionMs,
          )
          .toInt();
      _playbackClockSlewStartedAt = now;
      _playbackClockSlewCorrectionMs = correction;
      sample = LitchcraftV2TimelineSample(
        session: sample.session,
        revision: sample.revision,
        sampleId: sample.sampleId,
        playing: true,
        selectedFrame: sample.selectedFrame,
        elapsedMilliseconds: projected,
        receivedAtMonotonicMilliseconds: now,
      );
    } else {
      _playbackClockSlewStartedAt = 0;
      _playbackClockSlewCorrectionMs = 0;
    }
    _advancedTimeline = sample;
    if (!_advancedWire) {
      _frameStorePlaying = sample.playing;
      _selectStoreBoardFrame(sample.selectedFrame);
    } else {
      _advancedPlaying = sample.playing;
      _advancedSelectedFrame = sample.selectedFrame;
      _selectBoardFrame(sample.selectedFrame);
    }
    _notify();
    return true;
  }

  void _applyPendingAdvancedTimeline() {
    final sample = _pendingAdvancedTimeline;
    if (sample == null ||
        sample.session != _documentSessionId ||
        sample.revision != _documentRevision ||
        sample.revision != _coherentDocumentRevision) {
      return;
    }
    // Consume before application: a later live sample at this revision must
    // never be replaced by a retained pre-snapshot sample on a later refresh.
    _pendingAdvancedTimeline = null;
    _receiveAdvancedTimeline(sample);
  }

  void _applyPendingAdvancedRuntimeMetadata() {
    if (usesFrameStoreDocument) return;
    final state = _pendingAdvancedState;
    if (state != null &&
        state.session == _documentSessionId &&
        state.revision == _documentRevision &&
        state.revision == _coherentDocumentRevision) {
      _pendingAdvancedState = null;
      _advancedCanUndo = state.canUndo;
      _advancedCanRedo = state.canRedo;
      _advancedPlaying = state.playing;
      if (state.selectedFrame < documentSession.document.frames.length) {
        _advancedSelectedFrame = state.selectedFrame;
        _selectBoardFrame(state.selectedFrame);
      }
    }
    _applyPendingAdvancedTimeline();
  }

  void _selectBoardFrame(int frame) {
    if (usesFrameStoreDocument) return;
    // Runtime selection is board authority, not an app edit. Suppressing the
    // session listener prevents it from becoming a divergent local document
    // or starting a replacement/synchronization command. Mirror the selected
    // immutable frame into the presentation cache before notifying: otherwise
    // `_notify` could write the preceding frame RGB into this frame.
    _projectingDocument = true;
    try {
      if (documentSession.selectedFrameIndex != frame) {
        documentSession.selectFrame(frame);
      }
      _canvas = documentSession.canvas.withTopology(_canvas.topology);
    } finally {
      _projectingDocument = false;
    }
  }

  void _showStoreFrame(int frame) {
    if (frame < 0 || frame >= documentSession.document.frames.length) return;
    _advancedSelectedFrame = frame;
    final wasProjecting = _projectingDocument;
    _projectingDocument = true;
    try {
      documentSession.selectFrame(frame);
      _canvas = _withPendingPaint(
        documentSession.canvas.withTopology(documentSession.document.topology),
      );
      final board = documentSession.boardDocument;
      _board = board == null || frame >= board.frames.length
          ? _canvas
          : LitchcraftCanvas(
              colors: board.frames[frame].emitters.map(
                (emitter) => emitter.color,
              ),
              topology: board.topology,
            );
    } finally {
      _projectingDocument = wasProjecting;
    }
  }

  void _selectStoreBoardFrame(int frame) {
    if (_stroke) {
      _deferredBoardFrame = frame;
      _showStoreFrame(_strokeFrame ?? documentSession.selectedFrameIndex);
      return;
    }
    _showStoreFrame(frame);
  }

  void _pauseStoreTimelineAtPhysicalFrame(int frame) {
    _storeTimelineProjection = null;
    _frameStorePlaying = false;
    _advancedTimeline = LitchcraftV2TimelineSample(
      session: _documentSessionId,
      revision: _frameStoreGeneration,
      sampleId: 0,
      playing: false,
      selectedFrame: frame,
      elapsedMilliseconds:
          litchcraftAnimationFrameStartOrNull(
            documentSession.document,
            frame,
          ) ??
          0,
      receivedAtMonotonicMilliseconds: _advancedTimelineNow,
    );
    _playbackClockSlewStartedAt = 0;
    _playbackClockSlewCorrectionMs = 0;
  }

  static bool _newerDocumentRevision(int candidate, int current) {
    final delta = (candidate - current) & 0xffffffff;
    return delta != 0 && delta < 0x80000000;
  }

  void _observeAdvancedRevisionWatermark(int revision) {
    if (!_newerDocumentRevision(revision, _documentRevision)) return;
    _documentDirty = true;
    if (_documentSyncing || _entering != null || _queued > 0 || !_selected) {
      return;
    }
    unawaited(
      _enqueue(() async {
        if (_documentDirty) await _synchronizeDocument();
      }),
    );
  }

  bool _acceptAdvancedRuntimeState(LitchcraftV2State state) {
    if (state.session != _documentSessionId) return false;
    if (state.revision == _documentRevision &&
        state.revision == _coherentDocumentRevision) {
      return true;
    }
    // State is metadata, not proof of a matching document. Older replies must
    // not rewind the runtime; a newer revision is only a snapshot watermark.
    if (_newerDocumentRevision(state.revision, _documentRevision)) {
      _pendingAdvancedState = state;
    }
    _observeAdvancedRevisionWatermark(state.revision);
    return false;
  }

  void _receiveAdvancedDocumentPacket(LitchcraftV2Packet packet) {
    if (packet.event == LitchcraftV2Protocol.effectClockLow ||
        packet.event == LitchcraftV2Protocol.effectClockHigh) {
      // Link publishes a clock only after its coherent requested pair. State
      // can settle Inspect before these separate notifications arrive.
      _receiveStoreEffectClock();
      return;
    }
    if (packet.event == LitchcraftV2Protocol.physicalStoreBatch) {
      if (preparingBoardArtwork) {
        // Retained pixels cannot absorb a patch before this entry has its
        // local draft/orientation. Fresh rich inspection will recover it.
        _pendingAdvancedState = null;
        return;
      }
      _readPhysicalStoreBatch(packet);
      return;
    }
    final modeState =
        packet.event == LitchcraftV2Protocol.state ||
        packet.event == LitchcraftV2Protocol.frameStoreState;
    // Inspection establishes session identity before its following compact
    // Tool event arrives. Document authority still waits for coherent artwork.
    if (packet.event == LitchcraftV2Protocol.state &&
        packet.requestId != 0 &&
        packet.payload[0] == 0 &&
        _entering != null &&
        _status == LitchcraftStatus.connecting) {
      _sessionId = packet.session;
      _documentSessionId = packet.session;
      _boardActive = packet.payload[1] & 1 != 0;
      _boardAuthorityLinkToken = _linkToken;
      _recordInspectedMode(_boardActive);
      _advancedWire = packet.payload[4] == 3;
    }
    if (modeState &&
        packet.requestId == 0 &&
        packet.payload[0] == 0 &&
        packet.payload[1] & 1 != 0 &&
        packet.session != 0 &&
        (!_boardActive || packet.session != _documentSessionId) &&
        _linkUsable) {
      _beginPhysicalEntry(packet.session, packet.revision, advanced: true);
      return;
    }
    // An unsolicited inactive State is a lifecycle event, not document
    // metadata. It must win even when its newer revision would otherwise
    // schedule a snapshot before this handler reaches the lifecycle fence.
    if (_boardActive &&
        modeState &&
        packet.requestId == 0 &&
        packet.payload[0] == 0 &&
        packet.session == _sessionId &&
        packet.payload[1] & 1 == 0) {
      _acceptAdvancedPhysicalExit();
      return;
    }
    if (preparingBoardArtwork) {
      if (packet.event == LitchcraftV2Protocol.state &&
          packet.payload[0] == 0) {
        _pendingAdvancedState = LitchcraftV2State.fromPacket(packet);
      }
      _notify();
      return;
    }
    if (_advancedWire &&
        _boardActive &&
        packet.event == LitchcraftV2Protocol.state &&
        packet.session == _documentSessionId) {
      final state = LitchcraftV2State.fromPacket(packet);
      if (!_acceptAdvancedRuntimeState(state)) return;
      if (_optimisticDocumentEdits > 0) return;
      _advancedCanUndo = state.canUndo;
      _advancedCanRedo = state.canRedo;
      _advancedPlaying = state.playing;
      if (state.selectedFrame < documentSession.document.frames.length) {
        _advancedSelectedFrame = state.selectedFrame;
        _selectBoardFrame(state.selectedFrame);
      }
      // History flags describe the board, independently of local draft history
      // or whether a complete document still needs to be downloaded.
      _notify();
    }
    if (!_advancedWire ||
        packet.event != LitchcraftV2Protocol.changed ||
        packet.session != _documentSessionId) {
      return;
    }
    _observeAdvancedRevisionWatermark(packet.revision);
  }

  void _acceptAdvancedPhysicalExit() {
    _cancelEntryDraftWait();
    _documentClient?.cancel();
    _cancelPendingStoreCommit();
    ++_selectionIntent;
    ++_generation;
    cancelBrushColorPreview();
    _boardActive = false;
    _recordInspectedMode(false);
    _selected = false;
    _interruptedStoreReplacement = null;
    _advancedWire = false;
    _pendingStoreEntryInspection = null;
    _storeEntryInspection = null;
    _frameStoreReadPending = false;
    _clearAdvancedTimeline(disposeObserver: false);
    _status = LitchcraftStatus.offline;
    _notify();
  }

  Future<T> _prepareConnection<T>(Future<T> operation, String stage) async {
    _appTrace?.record(
      'litchcraft.preparation_start',
      fields: <String, Object?>{
        'stage': stage,
        'generation': _generation,
        'recovery_attempt': _recoveryAttempt,
      },
    );
    try {
      final T result = await operation.timeout(_connectionPreparationTimeout);
      _appTrace?.record(
        'litchcraft.preparation_complete',
        fields: <String, Object?>{'stage': stage, 'generation': _generation},
      );
      return result;
    } on TimeoutException {
      _appTrace?.record(
        'litchcraft.preparation_timeout',
        fields: <String, Object?>{'stage': stage, 'generation': _generation},
      );
      if (stage == 'notification_cleanup' &&
          !_connectionRestartRequested &&
          bleService.isConnected) {
        _connectionRestartRequested = true;
        _appTrace?.record(
          'litchcraft.connection_restart',
          fields: <String, Object?>{
            'reason': 'notification_cleanup_timeout',
            'generation': _generation,
          },
        );
        unawaited(bleService.disconnect().catchError((Object _) {}));
      }
      throw TimeoutException(
        'Litchcraft ${stage.replaceAll('_', ' ')} timed out',
        _connectionPreparationTimeout,
      );
    }
  }

  Future<void> _disposeLink(LitchcraftLink? link) {
    if (link != null) {
      final previous = _linkDisposal;
      late final Future<void> disposal;
      disposal =
          () async {
            await previous;
            await link.dispose();
          }().whenComplete(() {
            if (identical(_linkDisposal, disposal)) _linkDisposal = null;
          });
      _linkDisposal = disposal;
    }
    // A UI timeout does not cancel native disable. Every retry must retain
    // this barrier until cleanup settles, or old cleanup can silence its
    // replacement after that replacement has already been declared healthy.
    return _linkDisposal ?? Future<void>.value();
  }

  ({LitchcraftCanvas canvas, int revision})? _trustedExitFor(String id) {
    final canvas = _trustedExitCanvas;
    if (canvas == null ||
        _trustedExitBoardId != id ||
        _trustedExitLinkToken != bleService.connectionSessionToken ||
        _offlineDiverged ||
        !_sameArtwork(_canvas, canvas)) {
      return null;
    }
    return (canvas: canvas, revision: _trustedExitRevision);
  }

  void _clearTrustedExit() {
    _trustedExitCanvas = null;
    _trustedExitBoardId = null;
    _trustedExitLinkToken = -1;
    _trustedExitRevision = -1;
  }

  Future<void> _startPendingExit() =>
      _exiting ??= _finishPendingExit().whenComplete(() {
        _exiting = null;
        _settleNotificationRepair();
        _notify();
      });

  Future<void> _finishPendingExit() async {
    await _tail;
    final id = _exitOnReconnect;
    if (id == null || id != bleService.connectedId) return;
    final selectionIntent = _selectionIntent;
    final generation = ++_generation;
    try {
      await _bindLink(id, generation);
      if (generation != _generation || _exitOnReconnect != id) return;
      try {
        await _link!.send(LitchcraftProtocol.exit, _sessionId);
      } on LitchcraftRejected catch (error) {
        // Reboot/physical exit already ended this ownership. Never exit a
        // different session just because the old app departure was delayed.
        if (error.status != 2) rethrow;
      }
      if (generation != _generation) return;
      _exitOnReconnect = null;
      _boardActive = false;
      if (selectionIntent == _selectionIntent) _selected = false;
      _status = LitchcraftStatus.offline;
      _recoverNotificationRepair();
    } catch (error) {
      if (generation == _generation) _transportFailed(error);
    }
  }

  Future<void> _enqueue(
    Future<void> Function() action, {
    bool recoverBusy = false,
    int? paintGeneration,
    bool background = false,
  }) {
    if (_queued >= 128) {
      _offlineDiverged = true;
      _detached = _canvas;
      _transportFailed(
        StateError('Board edits are backed up; draft preserved'),
      );
      return Future<void>.value();
    }
    ++_queued;
    if (background) ++_backgroundQueued;
    final generation = _generation;
    final selectionIntent = _selectionIntent;
    _notify();
    final result = _tail.then((_) async {
      if (_disposed || generation != _generation) return;
      if (paintGeneration != null && paintGeneration != _paintQueueGeneration) {
        return;
      }
      try {
        await action();
        if (recoverBusy) {
          _waitingForLift = false;
          _errorMessage = null;
        }
        if (!usesFrameStoreDocument &&
            !_advancedWire &&
            _needsResync &&
            _boardActive) {
          await _pullSnapshot();
        }
      } on LitchcraftResyncRequired {
        if (usesFrameStoreDocument) {
          _storePaintWaitingForAuthority = true;
          _schedulePhysicalStoreRepair();
          unawaited(
            _orderedSession
                ?.requestState(
                  code: ConnectionSession.litchcraftStoreAuthorityStateCode,
                )
                .catchError((Object _) {}),
          );
          return;
        }
        if (_advancedWire) rethrow;
        await _pullSnapshot();
        _canvas = _board;
      }
    });
    _tail = result
        .catchError((Object error) async {
          if (generation != _generation) return;
          if (error is LitchcraftTransferCancelled &&
              selectionIntent != _selectionIntent) {
            return;
          }
          if (error is LitchcraftRejected && error.status == 1 && recoverBusy) {
            // A held physical brush is a recoverable board refusal, not a
            // broken transport. Preserve live authority and wait for the
            // person to lift before they deliberately retry.
            _waitingForLift = true;
            _errorMessage = error.toString();
            if (paintGeneration != null) {
              try {
                await _recoverRejectedPaint(generation);
              } catch (recoveryError) {
                if (generation == _generation) _transportFailed(recoveryError);
              }
            }
            return;
          }
          _transportFailed(error);
        })
        .whenComplete(() {
          --_queued;
          if (background) --_backgroundQueued;
          if (_queued == 0 &&
              _advancedWire &&
              !usesFrameStoreDocument &&
              _documentDirty &&
              _status == LitchcraftStatus.live &&
              !_stroke) {
            unawaited(
              _enqueue(() async {
                if (_documentDirty) await _synchronizeDocument();
              }),
            );
          }
          _notify();
          if (!_disposed &&
              _queued == 0 &&
              _status == LitchcraftStatus.live &&
              _stroke &&
              _strokeSquares.isNotEmpty) {
            // Any queued owner can hold the first pointer sample, including
            // optional clock metadata. Resume it when that owner drains.
            _scheduleStrokeFlush();
          }
        });
    return _tail;
  }

  Future<void> _recoverRejectedPaint(int generation) async {
    // Pointer-up can precede the ACK. Retire every already-queued chunk rather
    // than letting its End or a buffered continuation replay after a lift.
    ++_paintQueueGeneration;
    _recoveringRejectedPaint = true;
    _flushScheduled = false;
    _strokeSquares.clear();
    _stroke = false;
    _strokeFrame = null;
    _strokeEraser = null;
    _strokeBefore = null;
    documentSession.endStroke();
    final deferredFrame = _deferredBoardFrame;
    _deferredBoardFrame = null;
    if (deferredFrame != null && usesFrameStoreDocument) {
      _showStoreFrame(deferredFrame);
    }
    final sourceGeneration = documentSession.replacementGeneration;
    _notify();
    try {
      if (_advancedWire) {
        // A refused optimistic stroke is not a new local document to upload.
        // Read current board authority, including any earlier accepted chunks.
        await _synchronizeDocument(reconcile: false);
        if (_disposed ||
            generation != _generation ||
            sourceGeneration != documentSession.replacementGeneration) {
          return;
        }
        documentSession.acceptBoard(
          expectedGeneration: documentSession.generation,
        );
        _selectBoardFrame(_advancedSelectedFrame);
      } else {
        _clearPendingPaint();
        await _pullSnapshot();
        if (_disposed || generation != _generation) return;
        _canvas = _board;
      }
      _offlineDiverged = false;
      _detached = _canvas;
      _waitingForLift = true;
    } finally {
      _recoveringRejectedPaint = false;
    }
  }

  void _scheduleFavoritePaletteSync() {
    if (_disposed ||
        _paletteSyncScheduled ||
        !_paletteSyncSupported ||
        usesFrameStoreDocument ||
        _status != LitchcraftStatus.live ||
        !_boardActive ||
        !_linkUsable ||
        (_syncedFavoriteColorsRevision == _favoriteColorsRevision &&
            _syncedFavoriteColorsLinkToken == _linkToken)) {
      return;
    }
    _paletteSyncScheduled = true;
    final int revision = _favoriteColorsRevision;
    final int linkToken = _linkToken;
    final List<int> colors = List<int>.of(_favoriteColors);
    unawaited(
      _enqueue(() async {
        for (var offset = 0; offset < colors.length; offset += 3) {
          final chunk = colors.skip(offset).take(3).toList();
          await _link!.send(LitchcraftProtocol.paletteChunk, _sessionId, <int>[
            offset,
            chunk.length,
            ...chunk.expand(LitchcraftProtocol.rgb),
          ]);
        }
        if (_linkToken == linkToken && _favoriteColorsRevision == revision) {
          _syncedFavoriteColorsRevision = revision;
          _syncedFavoriteColorsLinkToken = linkToken;
        }
      }).whenComplete(() {
        _paletteSyncScheduled = false;
        _scheduleFavoritePaletteSync();
      }),
    );
  }

  void beginStroke({bool? erasing, int? targetMask}) {
    if (boardArtworkUnresolved) return;
    if (_recoveringRejectedPaint) return;
    if (targetMask != null && (targetMask < 1 || targetMask > 15)) {
      throw RangeError.range(targetMask, 1, 15, 'targetMask');
    }
    if (_stroke) endStroke();
    _queuedStorePlay = false;
    _acceptedStoreStroke = null;
    _strokePaintGeneration = _paintQueueGeneration;
    final frame = usesFrameStoreDocument
        ? animationSelectedFrameIndex
        : documentSession.selectedFrameIndex;
    // Latch this before the local pause projection. A paused board already on
    // the target frame can paint immediately; an unnecessary Timeline Select
    // would make the first pixel wait for a separate receipt.
    _strokeStoreSelectRequired =
        usesFrameStoreDocument &&
        (_frameStorePlaying != false ||
            _advancedTimeline?.selectedFrame != frame);
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        animationPlaying) {
      _projectStoreTimeline(
        frame: frame,
        elapsedMilliseconds:
            litchcraftAnimationFrameStartOrNull(
              documentSession.document,
              frame,
            ) ??
            0,
        playing: false,
      );
    }
    _supportStrokeSquares.clear();
    _supportStrokeAction = _appTrace.beginUserAction(
      (erasing ?? _eraser) ? SupportActionKind.erase : SupportActionKind.paint,
    );
    _stroke = true;
    _strokeFrame = frame;
    _strokeEraser = erasing;
    _strokeTargetMask = targetMask ?? _targetMask;
    _strokeBegun = false;
    _strokeBefore = _canvas;
    if (documentSession.isAdvanced || _advancedWire || usesFrameStoreDocument) {
      documentSession.beginStroke();
    }
    _strokeId = (_strokeId % 0xffff) + 1;
  }

  void paintSquares(Iterable<int> squares) {
    // Inspect can establish board mode before the resident body arrives. A
    // stroke has no safe frame or common document until that read completes.
    if (boardArtworkUnresolved) return;
    if (_recoveringRejectedPaint) return;
    final valid = squares.toSet();
    if (valid.any((v) => v < 0 || v >= 64)) throw RangeError('Invalid square');
    if (!_stroke) beginStroke();
    final strokeFrame = _strokeFrame;
    if (strokeFrame != null &&
        documentSession.selectedFrameIndex != strokeFrame) {
      _deferredBoardFrame = documentSession.selectedFrameIndex;
      _showStoreFrame(strokeFrame);
    }
    final erase = _strokeEraser ?? _eraser;
    final color = erase ? 0 : _color;
    final mask = _strokeTargetMask ?? _targetMask;
    final emitters = documentSession
        .document
        .frames[documentSession.selectedFrameIndex]
        .emitters;
    valid.removeWhere((square) {
      for (var region = 0; region < 4; region++) {
        if (mask & (1 << region) == 0) continue;
        final index = square * 4 + region;
        if (_canvas.colors[index] != color ||
            ((documentSession.isAdvanced ||
                    _advancedWire ||
                    usesFrameStoreDocument) &&
                erase &&
                emitters[index].effect != LitchcraftEffectStyle.off)) {
          return false;
        }
      }
      return true;
    });
    if (valid.isEmpty) return;
    _supportStrokeSquares.addAll(valid);
    if (documentSession.isAdvanced || _advancedWire || usesFrameStoreDocument) {
      documentSession.paintSelectedFrame(
        squares: valid,
        mask: mask,
        color: (_strokeEraser ?? _eraser) ? 0 : _color,
        clearEffects: _strokeEraser ?? _eraser,
        history: false,
      );
      _canvas = documentSession.canvas.withTopology(_canvas.topology);
    } else {
      _canvas = _canvas.paint(valid, mask, color);
    }
    if (_status == LitchcraftStatus.live && !storeReplacementUnresolved) {
      if (!_advancedWire && !documentSession.isAdvanced) {
        final sequence = ++_paintSequence;
        for (final square in valid) {
          for (var emitter = 0; emitter < 4; emitter++) {
            if ((mask & (1 << emitter)) != 0) {
              _pendingPaint[square * 4 + emitter] = (
                sequence: sequence,
                color: (_strokeEraser ?? _eraser) ? 0 : _color,
              );
            }
          }
        }
      }
      _strokeSquares.addAll(valid);
      _scheduleStrokeFlush();
    } else {
      if (_status == LitchcraftStatus.live && storeReplacementUnresolved) {
        // Retain the original gesture until its position on the mutation tail
        // can follow Commit, including a pointer held across publication.
        _strokeSquares.addAll(valid);
      }
      _offlineDiverged = true;
      _detached = _canvas;
    }
    _notify();
  }

  Future<void> fillChecker({required bool dark}) async {
    final mask = _targetMask;
    final clearEffects = _strokeEraser ?? _eraser;
    final color = clearEffects ? 0 : _color;
    final squares = <int>[
      for (var square = 0; square < 64; ++square)
        if ((((square % 8) + (square ~/ 8)).isOdd) == dark) square,
    ];
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        !storeReplacementUnresolved) {
      await _editStoreCanvas(
        action: clearEffects ? (dark ? 8 : 7) : (dark ? 6 : 5),
        mask: mask,
        color: color,
      );
      return;
    }
    if (animationBoardAuthorityLive && _advancedCanvasPattern) {
      final frame = _advancedSelectedFrame;
      await _editAdvancedDocument(
        frame: frame,
        mutate: () => documentSession.paintSelectedFrame(
          squares: squares,
          mask: mask,
          color: color,
          clearEffects: clearEffects,
        ),
        send: (link, session, revision) => link.checkerEditV2(
          session: session,
          baseRevision: revision,
          frame: frame,
          mask: mask,
          color: color,
          dark: dark,
          erase: clearEffects,
        ),
      );
      return;
    }
    if (documentSession.isAdvanced || _advancedWire) {
      await _mutateSelectedAdvancedFrame(
        () => documentSession.paintSelectedFrame(
          squares: squares,
          mask: mask,
          color: color,
          clearEffects: clearEffects,
        ),
      );
      return;
    }
    await _transformCanvas((canvas) => canvas.paint(squares, mask, color));
  }

  Future<void> fillAll() async {
    final mask = _targetMask;
    final color = _color;
    final squares = List<int>.generate(64, (index) => index);
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        !storeReplacementUnresolved) {
      await _editStoreCanvas(action: 0, mask: mask, color: color);
      return;
    }
    if (animationBoardAuthorityLive && _advancedCanvasEdit) {
      await _editAdvancedCanvas(erase: false, mask: mask, color: color);
      return;
    }
    if (documentSession.isAdvanced || _advancedWire) {
      await _mutateSelectedAdvancedFrame(
        () => documentSession.paintSelectedFrame(
          squares: squares,
          mask: mask,
          color: color,
        ),
      );
      return;
    }
    await _transformCanvas((canvas) => canvas.paint(squares, mask, color));
  }

  /// A deliberate swatch hold selects Paint and fills with that exact color.
  /// The atomic Canvas fill carries its own color and must not wait for Tool.
  Future<void> fillAllWithColor(int color) async {
    final mask = _targetMask;
    final brush = _setBrush(
      color: color,
      eraser: false,
      syncBoard: !_advancedWire && !usesFrameStoreDocument,
    );
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        !storeReplacementUnresolved) {
      // The local brush setter does not await transport here. Capture the
      // held swatch frame in this input turn before a physical sample arrives.
      await _editStoreCanvas(action: 0, mask: mask, color: color);
      await brush;
      return;
    }
    await brush;
    await fillAll();
  }

  Future<bool> _editStoreCanvas({
    required int action,
    required int mask,
    required int color,
    LitchcraftFrameEditTarget? target,
    int frameDurationMs = 0,
    int effectSquare = 0,
    LitchcraftEffectStyle effect = LitchcraftEffectStyle.off,
    int effectDurationMs = 0,
  }) async {
    if (_recoveringRejectedPaint) {
      throw StateError(
        'Store edit cannot start during rejected-paint recovery',
      );
    }
    endStroke();
    if (boardArtworkUnresolved) return false;
    final relative =
        target != null || (action >= 2 && action <= 4) || action >= 10;
    final exactTarget = relative
        ? (target ?? captureAnimationFrameEditTarget())
        : null;
    if (relative &&
        (exactTarget == null || !_matchesStoreFrameTarget(exactTarget))) {
      return false;
    }
    final predecessorPosition = exactTarget?._predecessor ?? _storeEditPosition;
    final canvasHistoryPredecessor = _pendingStoreCanvas?.followingHistory;
    var applied = false;
    _StoreEditResult? confirmedResult;
    final ordered = _orderedSession;
    final deviceId = _deviceId;
    if (ordered == null || !ordered.isActivated || deviceId == null) {
      throw const LitchcraftResyncRequired();
    }
    if (_optimisticDocumentEdits >= 128) {
      throw StateError('Board edits are backed up; draft preserved');
    }
    final position = _newStoreEditPosition(
      changesLayout: action == 10 || action == 11,
    );
    if (action == 0) _storeToolEditPosition = position;
    final generation = _generation;
    final modeSession = _documentSessionId;
    final sourceGeneration = documentSession.replacementGeneration;
    var physicalRevision = _physicalStoreEditRevision;
    final playbackIntentGeneration = _playbackIntentGeneration;
    final historyPredecessor = _storeUnknownHistoryPosition?.position;
    final acquired = Completer<void>();
    final eligible = Completer<void>();
    final frame =
        exactTarget?.frame ??
        (animationPlaying
            ? animationSelectedFrameIndex
            : documentSession.selectedFrameIndex);
    final selectRequired =
        _frameStorePlaying != false ||
        (_advancedTimeline?.selectedFrame ?? _advancedSelectedFrame) != frame;
    var before = documentSession.document;
    final erase = action == 1 || action == 7 || action == 8;
    void mutate(LitchcraftDocumentSession edit) {
      if (action >= 2 && action <= 4) {
        edit.rotateSelectedFrame(
          action == 2
              ? 1
              : action == 3
              ? 2
              : 3,
        );
      } else if (action == 10) {
        edit.addFrame();
      } else if (action == 11) {
        edit.deleteSelectedFrame();
      } else if (action == 13) {
        edit.setSelectedFrameDuration(frameDurationMs);
      } else if (action == 14) {
        edit.applyEffect(
          square: effectSquare,
          mask: mask,
          effect: effect,
          effectDurationMs: effectDurationMs,
        );
      } else {
        edit.paintSelectedFrame(
          squares: <int>[
            for (var square = 0; square < 64; square++)
              if (action < 5 ||
                  (((square % 8) + (square ~/ 8)).isOdd) ==
                      (action == 6 || action == 8))
                square,
          ],
          mask: mask,
          color: erase ? 0 : color,
          clearEffects: erase,
        );
      }
    }

    _showStoreFrame(frame);
    _pauseStoreTimelineAtPhysicalFrame(frame);
    mutate(documentSession);
    var intended = documentSession.document;
    var changed = !sameLitchcraftDocument(before, intended);
    final resultFrame = action == 10
        ? frame + 1
        : action == 11
        ? (frame == 0 ? 0 : frame - 1)
        : frame;
    if (action == 10 || action == 11) {
      // Layout changes select a different frame immediately. Keep rendering
      // and pointer validation on that same frame while its receipt is pending.
      _showStoreFrame(resultFrame);
      _pauseStoreTimelineAtPhysicalFrame(resultFrame);
    }
    final previewGeneration = documentSession.generation;
    if (action == 0) {
      // This later domain action owns tool selection; any unsent older Tool
      // preview is superseded by its self-contained Fill.
      _pendingBrush = null;
      _color = color;
      _targetMask = mask;
      _eraser = false;
    }
    ++_optimisticDocumentEdits;
    final completed = Completer<void>();
    var running = false;
    void settleLifetime() {
      if (completed.isCompleted) return;
      if (_disposed ||
          generation != _generation ||
          sourceGeneration != documentSession.replacementGeneration ||
          modeSession != _documentSessionId ||
          !identical(ordered, _orderedSession) ||
          !_selected ||
          !_boardActive ||
          !ordered.isActivated ||
          _status == LitchcraftStatus.offline ||
          _status == LitchcraftStatus.connecting ||
          _status == LitchcraftStatus.conflict) {
        completed.completeError(const LitchcraftTransferCancelled());
      }
    }

    void defer() {
      if (!_storePaintWaitingForAuthority) {
        _storePaintWaitingForAuthority = true;
        unawaited(
          ordered
              .requestState(
                code: ConnectionSession.litchcraftStoreAuthorityStateCode,
              )
              .catchError((Object _) {}),
        );
      }
      _schedulePhysicalStoreRepair();
    }

    Future<void> submit() async {
      var sent = false;
      try {
        settleLifetime();
        if (completed.isCompleted) return;
        if (_status != LitchcraftStatus.live) {
          throw const LitchcraftResyncRequired();
        }
        if (!_canGuardStorePaint(ordered) ||
            _physicalStoreRepairing ||
            documentSession.boardDocument == null) {
          defer();
          return;
        }
        if (relative &&
            !await _admitStoreFrameTarget(exactTarget!, predecessorPosition)) {
          // A foreign repair cannot turn a captured relative edit into a new
          // operation against a different board preimage.
          if (!completed.isCompleted) completed.complete();
          _status = LitchcraftStatus.conflict;
          _offlineDiverged = true;
          _notify();
          throw const LitchcraftResyncRequired();
        }
        before = documentSession.boardDocument!;
        if (frame >= before.frames.length) {
          throw const LitchcraftTransferCancelled();
        }
        final edit = LitchcraftDocumentSession(before)..selectFrame(frame);
        mutate(edit);
        intended = edit.document;
        edit.dispose();
        changed = !sameLitchcraftDocument(before, intended);
        final predecessorResult = predecessorPosition?.result;
        final predecessorSelectedAnotherFrame =
            predecessorResult != null &&
            predecessorResult.authority.revision ==
                _storeCommonAuthority?.revision &&
            _sameStorePreimage(predecessorResult.document, before) &&
            predecessorResult.selectedAfter != frame;
        if (selectRequired ||
            predecessorSelectedAnotherFrame ||
            playbackIntentGeneration != _playbackIntentGeneration ||
            (_advancedTimeline?.selectedFrame ?? _advancedSelectedFrame) !=
                frame) {
          await _selectStoreFrameForEdit(frame);
        }
        settleLifetime();
        if (completed.isCompleted) return;
        if (!_canGuardStorePaint(ordered) ||
            documentSession.boardDocument == null ||
            !sameLitchcraftDocument(documentSession.boardDocument!, before)) {
          defer();
          return;
        }
        if (!eligible.isCompleted) eligible.complete();
        physicalRevision = _physicalStoreEditRevision;
        final common = _storeCommonAuthority!;
        final revision = (common.revision + (changed ? 1 : 0)) & 0xffffffff;
        final previous = _storeHistoryEcho;
        final prior =
            previous != null &&
                previous.confirmed &&
                previous.sourceGeneration == sourceGeneration &&
                previous.generation == common.generation &&
                previous.revision == common.revision &&
                sameLitchcraftDocument(previous.document, before)
            ? previous
            : null;
        final echo = changed
            ? (
                sourceGeneration: sourceGeneration,
                generation: common.generation,
                revision: revision,
                document: intended,
                confirmed: false,
                previous: prior != null
                    ? (
                        generation: prior.generation,
                        revision: prior.revision,
                        document: prior.document,
                      )
                    : null,
              )
            : prior;
        // Canvas has no reverse square echo. This existing exact-result proof
        // still protects a confirmed predecessor's late history tail.
        _storeHistoryEcho = echo;
        _unconfirmedDocument = intended;
        _unconfirmedDocumentBoardId = deviceId;
        sent = true;
        late final List<ConnectionSessionOutcome> outcomes;
        if (action == 14) {
          await _sendAdvancedEmitterCommand(
            frame: frame,
            stroke: _strokeId = (_strokeId % 0xffff) + 1,
            flags: 3,
            mask: mask,
            squares: <int>[effectSquare],
            effect: effect,
            effectDurationMs: effect == LitchcraftEffectStyle.off
                ? 0
                : effectDurationMs,
          );
          outcomes = <ConnectionSessionOutcome>[];
        } else {
          outcomes = await Future.wait(<Future<ConnectionSessionOutcome>>[
            ordered.sendOrdered(
              ConnectionSession.litchcraftStoreGuardOpcode,
              _storeGuardBody(ordered),
            ),
            ordered.sendOrdered(
              action >= 10
                  ? ConnectionSession.litchcraftStoreFrameEditOpcode
                  : ConnectionSession.litchcraftStoreCanvasOpcode,
              Uint8List.fromList(
                action >= 10
                    ? <int>[
                        action - 10,
                        frame,
                        action == 10 ? frame + 1 : 0,
                        frameDurationMs & 0xff,
                        frameDurationMs >> 8,
                      ]
                    : <int>[
                        frame,
                        mask,
                        color >> 16,
                        color >> 8 & 0xff,
                        color & 0xff,
                        action,
                      ],
              ),
            ),
          ]);
        }
        if (generation != _generation ||
            sourceGeneration != documentSession.replacementGeneration ||
            modeSession != _documentSessionId ||
            !_boardActive ||
            _status != LitchcraftStatus.live) {
          throw const LitchcraftTransferCancelled();
        }
        if (outcomes.any(
              (outcome) =>
                  outcome.disposition != ConnectionSessionDisposition.applied,
            ) ||
            (action == 14
                    ? _storeCommonAuthority?.revision
                    : outcomes.last.domainRevision) !=
                revision ||
            (_physicalStoreEditRevision != physicalRevision &&
                (echo == null || _storeHistoryEcho != echo)) ||
            (_storeAuthority?.revision ?? 0) > revision) {
          throw const LitchcraftResyncRequired();
        }
        if (changed) _acceptStoreEditRevision(revision);
        // Applied proves this exact guarded live result. Structural edits
        // remain dirty until the existing worker saves; a clean root is not
        // a prerequisite for confirming the next gesture's common preimage.
        documentSession.confirm(deviceId, intended);
        applied = true;
        if (changed && echo != null && _storeHistoryEcho == echo) {
          _storeHistoryEcho = (
            sourceGeneration: echo.sourceGeneration,
            generation: _storeCommonAuthority!.generation,
            revision: echo.revision,
            document: echo.document,
            confirmed: true,
            previous: echo.previous,
          );
        }
        _forgetSelectedFrameStoreResident();
        _documentDirty = false;
        _canvas = documentSession.canvas.withTopology(_canvas.topology);
        _board = LitchcraftCanvas(
          colors: intended.frames[resultFrame].emitters.map(
            (emitter) => emitter.color,
          ),
          topology: _canvas.topology,
        );
        _detached = _canvas;
        _offlineDiverged = false;
        final selected = documentSession.selectedFrameIndex;
        if (selected != resultFrame &&
            selected < intended.frames.length &&
            identical(_storeEditPosition, position)) {
          await _selectStoreFrameForEdit(selected);
        }
        confirmedResult = _storeConfirmedResult(
          before: before,
          document: intended,
          selectedBefore: frame,
          selectedAfter: resultFrame,
          acceptedRevision: revision,
        );
        final owner = _pendingStoreCanvas;
        if (owner?.completed == completed) {
          _pendingStoreCanvas = (
            generation: owner!.generation,
            sourceGeneration: owner.sourceGeneration,
            modeSession: owner.modeSession,
            session: owner.session,
            frame: owner.frame,
            mask: owner.mask,
            action: owner.action,
            effectEdits: owner.effectEdits,
            applied: true,
            followingHistory: owner.followingHistory,
            historyRevision: owner.historyRevision,
            completed: owner.completed,
            resume: owner.resume,
          );
        }
        _notify();
        if (!completed.isCompleted) completed.complete();
      } on LitchcraftResyncRequired {
        if (!sent) {
          defer();
          return;
        }
        _storeHistoryEcho = null;
        // A rejected canvas operation must not later replay as square paint.
        // Preserve a newer independent draft while normal Store repair runs.
        if (sourceGeneration == documentSession.replacementGeneration &&
            documentSession.generation == previewGeneration &&
            documentSession.boardDocument != null &&
            sameLitchcraftDocument(documentSession.boardDocument!, before)) {
          documentSession.acceptBoard(expectedGeneration: previewGeneration);
          _canvas = documentSession.canvas.withTopology(_canvas.topology);
          _detached = _canvas;
        }
        if (!completed.isCompleted) completed.complete();
        rethrow;
      } catch (error) {
        if (!completed.isCompleted) completed.completeError(error);
        rethrow;
      } finally {
        if (identical(_unconfirmedDocument, intended)) {
          _unconfirmedDocument = null;
        }
      }
    }

    Future<void> resume() async {
      if (running || completed.isCompleted) return;
      running = true;
      try {
        await _enqueue(submit);
      } finally {
        running = false;
      }
    }

    Future<void> acquire() async {
      // Wait outside the command tail for the captured Canvas, paint or
      // history position. Reading the mutable slot here could put an earlier
      // accepted Canvas behind a history barrier registered after it.
      if (predecessorPosition != null) {
        await predecessorPosition.completion.future;
      }
      // The existing bounded edit count also bounds callers awaiting this one
      // domain slot. Accepted Canvas operations never supersede each other.
      if (canvasHistoryPredecessor != null) {
        try {
          await canvasHistoryPredecessor.future;
        } catch (_) {
          // Lifetime checks cancel followers of a departed source.
        }
      }
      // Carry a later history barrier through its earlier Canvas successors.
      final priorOwner = _pendingStoreCanvas;
      final priorHistory = priorOwner?.followingHistory;
      final followingHistory = priorHistory?.isCompleted == false
          ? priorHistory
          : null;
      settleLifetime();
      if (completed.isCompleted) return;
      _pendingStoreCanvas = (
        generation: generation,
        sourceGeneration: sourceGeneration,
        modeSession: modeSession,
        session: ordered,
        frame: frame,
        mask: mask,
        action: action,
        effectEdits:
            priorOwner != null &&
                priorOwner.frame == frame &&
                priorOwner.generation == generation &&
                priorOwner.sourceGeneration == sourceGeneration &&
                priorOwner.modeSession == modeSession &&
                identical(priorOwner.session, ordered)
            ? priorOwner.effectEdits
            : BigInt.zero,
        applied: false,
        followingHistory: followingHistory,
        historyRevision: 0,
        completed: completed,
        resume: resume,
      );
      acquired.complete();
      await resume();
    }

    void release() {
      _finishStoreEditPosition(position, applied ? confirmedResult : null);
      removeListener(settleLifetime);
      if (_pendingStoreCanvas?.completed == completed &&
          _pendingStoreCanvas?.followingHistory == null) {
        _pendingStoreCanvas = null;
      }
      if (!acquired.isCompleted) acquired.complete();
      --_optimisticDocumentEdits;
      scheduleMicrotask(() {
        if (_disposed ||
            !identical(ordered, _orderedSession) ||
            generation != _generation ||
            sourceGeneration != documentSession.replacementGeneration ||
            _status != LitchcraftStatus.live) {
          return;
        }
        // The completed Canvas no longer owns catch-up. Resume a later
        // ordinary stroke through its existing authority/delta owner.
        _storePaintWaitingForAuthority = true;
        _resumeStorePaintIfReady(ordered);
      });
    }

    addListener(settleLifetime);
    unawaited(
      completed.future.then<void>(
        (_) => release(),
        onError: (Object error, StackTrace stack) => release(),
      ),
    );
    unawaited(
      acquire().catchError((Object error) {
        if (!completed.isCompleted) completed.completeError(error);
      }),
    );
    _notify();
    if (historyPredecessor != null) {
      // An older native journal repair keeps its actual ordered position.
      // Departure still settles followers through the existing lifetime owner.
      await Future.any<void>(<Future<void>>[
        historyPredecessor.then<void>((_) {}),
        completed.future,
      ]);
    }
    // Earlier Canvas work does not consume this successor's execution budget.
    await Future.any<void>(<Future<void>>[acquired.future, completed.future]);
    await Future.any<void>(<Future<void>>[
      eligible.future,
      completed.future,
    ]).timeout(_frameStoreResponseTimeout);
    // Admission/receipt and fresh authority surround the common session's
    // fixed head completion allowance. This is a caller bound, not an ACK.
    await completed.future.timeout(LitchBleService.orderedOperationTimeout);
    return applied;
  }

  Future<bool> _mutateSelectedAdvancedFrame(VoidCallback mutate) async {
    endStroke();
    LitchcraftFrameEditTarget? target;
    if (animationBoardAuthorityLive) {
      target = await prepareAnimationFrameEdit();
      if (target == null || !_matchesSelectedFrameEditTarget(target)) {
        return false;
      }
    }
    final sourceGeneration = documentSession.generation;
    mutate();
    if (sourceGeneration == documentSession.generation) return false;
    if (_advancedWire && _status == LitchcraftStatus.live) {
      var accepted = false;
      await _enqueue(() async {
        final exactTarget = target;
        if (exactTarget == null ||
            !_matchesSelectedFrameEditTarget(exactTarget)) {
          return;
        }
        accepted = await _sendDocument(target: exactTarget);
      });
      return accepted;
    }
    return true;
  }

  Future<void> _editAdvancedCanvas({
    required bool erase,
    required int mask,
    required int color,
    int quarterTurns = 0,
  }) async {
    final frame = animationSelectedFrameIndex;
    await _editAdvancedDocument(
      frame: frame,
      mutate: () {
        if (quarterTurns != 0) {
          documentSession.rotateSelectedFrame(quarterTurns);
        } else if (erase) {
          documentSession.clearSelectedFrame();
        } else {
          documentSession.paintSelectedFrame(
            squares: List<int>.generate(64, (index) => index),
            mask: mask,
            color: color,
          );
        }
      },
      send: (link, session, revision) => link.canvasEditV2(
        session: session,
        baseRevision: revision,
        frame: frame,
        mask: erase ? 15 : mask,
        color: erase ? 0 : color,
        erase: erase,
        quarterTurns: quarterTurns,
      ),
    );
  }

  /// The immutable preview is local-first; only an exact ACK advances common
  /// board authority. Queued edits rebase only over their proven predecessor.
  Future<bool> _editAdvancedDocument({
    required int frame,
    required void Function() mutate,
    required Future<LitchcraftV2State> Function(
      LitchcraftLink link,
      int session,
      int revision,
    )
    send,
    bool selectBeforeEdit = false,
    int? historyOpcode,
    int? connectionOpcode,
  }) async {
    endStroke();
    final link = _link!;
    final generation = _generation;
    final session = _documentSessionId;
    var revision = _documentRevision;
    final before = documentSession.document;
    final previousSelection = documentSession.selectedFrameIndex;
    // Freeze the finger-selected preview before any native write or ACK.
    _selectBoardFrame(frame);
    mutate();
    final intended = documentSession.document;
    final selected = documentSession.selectedFrameIndex;
    if (sameLitchcraftDocument(before, intended)) {
      _selectBoardFrame(previousSelection);
      return true;
    }
    _advancedPlaying = false;
    _advancedTimeline = null;
    _advancedSelectedFrame = selected;
    _notify();
    final editGeneration = documentSession.generation;
    var accepted = false;
    ++_optimisticDocumentEdits;
    await _enqueue(() async {
      if (!identical(link, _link) ||
          generation != _generation ||
          session != _documentSessionId ||
          _status != LitchcraftStatus.live) {
        return;
      }
      try {
        final common = documentSession.boardDocument;
        if (common == null || !sameLitchcraftDocument(common, before)) {
          throw const LitchcraftRejected(9);
        }
        if (historyOpcode != null && _documentDirty) {
          // Undo has no base-revision field. Never pop physical history after
          // an unexpected watermark invalidated our exact local inverse.
          throw const LitchcraftRejected(9);
        }
        if (_documentRevision != revision) {
          // A preceding queued app edit may have just become the common
          // document. Its exact bytes prove this operation's new base.
          revision = _documentRevision;
        }
        if (selectBeforeEdit) {
          final state = await link.selectFrameV2(
            session: session,
            frame: frame,
          );
          if (generation != _generation ||
              !identical(link, _link) ||
              state.session != session ||
              state.revision != revision ||
              state.playing ||
              state.selectedFrame != frame) {
            throw const LitchcraftRejected(9);
          }
        }
        LitchcraftV2State? ack;
        int acceptedRevision;
        final ordered = _orderedSession;
        if (connectionOpcode != null &&
            ordered != null &&
            ordered.isActivated) {
          final outcome = await ordered.sendOrdered(
            connectionOpcode,
            Uint8List(0),
          );
          if (outcome.disposition != ConnectionSessionDisposition.applied) {
            throw LitchcraftRejected(outcome.reason);
          }
          acceptedRevision = outcome.domainRevision;
        } else {
          ack = await send(link, session, revision);
          acceptedRevision = ack.revision;
        }
        if (!identical(link, _link) ||
            generation != _generation ||
            session != _documentSessionId) {
          return;
        }
        if (acceptedRevision != ((revision + 1) & 0xffffffff) ||
            (ack != null &&
                (ack.session != session ||
                    !ack.active ||
                    ack.playing ||
                    ack.selectedFrame != selected ||
                    ack.frameCount != intended.frames.length))) {
          throw const LitchcraftResyncRequired();
        }
        _documentRevision = acceptedRevision;
        _coherentDocumentRevision = acceptedRevision;
        _coherentDocumentSession = session;
        _coherentDocumentBoardId = _deviceId;
        _documentDirty = false;
        documentSession.confirm(_deviceId!, intended);
        final history = _KnownAdvancedHistory(
          before,
          intended,
          session,
          acceptedRevision,
          generation,
        );
        if (historyOpcode == LitchcraftProtocol.undo) {
          if (_knownAdvancedUndo.isNotEmpty) {
            _knownAdvancedUndo.removeLast();
          }
          _advanceKnownAdvancedHistory(_knownAdvancedUndo, acceptedRevision);
          _pushKnownAdvancedHistory(_knownAdvancedRedo, history);
        } else if (historyOpcode == LitchcraftProtocol.redo) {
          if (_knownAdvancedRedo.isNotEmpty) {
            _knownAdvancedRedo.removeLast();
          }
          _advanceKnownAdvancedHistory(_knownAdvancedRedo, acceptedRevision);
          _pushKnownAdvancedHistory(_knownAdvancedUndo, history);
        } else {
          _pushKnownAdvancedHistory(_knownAdvancedUndo, history);
          _knownAdvancedRedo.clear();
        }
        if (ack != null && documentSession.generation == editGeneration) {
          _applyAdvancedRuntimeState(ack);
          _applyPendingAdvancedRuntimeMetadata();
        } else if (ack != null) {
          // An older confirmation must not rewind the selected frame of a
          // newer optimistic edit, even though board history flags advance.
          _advancedCanUndo = ack.canUndo;
          _advancedCanRedo = ack.canRedo;
          _pendingAdvancedState = null;
          _pendingAdvancedTimeline = null;
        } else {
          _advancedCanUndo = _knownAdvancedUndo.isNotEmpty;
          _advancedCanRedo = _knownAdvancedRedo.isNotEmpty;
          _pendingAdvancedState = null;
          _pendingAdvancedTimeline = null;
          _notify();
        }
        accepted = true;
      } on LitchcraftRejected {
        // A refused intent must not later auto-upload over the board. Retire
        // only this preview; an independently newer local edit is preserved.
        final canRetirePreview = documentSession.generation == editGeneration;
        if (canRetirePreview) {
          documentSession.confirm(_deviceId!, before);
          documentSession.acceptBoard(expectedGeneration: editGeneration);
        }
        if (canRetirePreview) {
          await _synchronizeDocument();
        } else {
          await _synchronizeDocument(reconcile: false);
          _status = LitchcraftStatus.conflict;
        }
        rethrow;
      }
      // An uncertain ACK keeps the preview and existing common ancestor. The
      // normal reconnect snapshot proves equality, unchanged board, or conflict.
    }, recoverBusy: true).whenComplete(() {
      --_optimisticDocumentEdits;
      if (_optimisticDocumentEdits == 0 && generation == _generation) {
        _applyPendingAdvancedRuntimeMetadata();
      }
    });
    return accepted;
  }

  Future<bool> rotateArtwork(int quarterTurns) async {
    if (boardArtworkUnresolved) return false;
    endStroke();
    final turns = quarterTurns % 4;
    if (turns == 0) return false;
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      return _editStoreCanvas(
        action: turns == 1
            ? 2
            : turns == 2
            ? 3
            : 4,
        mask: 15,
        color: 0,
      );
    }
    if (animationBoardAuthorityLive && _advancedCanvasRotate) {
      final before = documentSession.document;
      await _editAdvancedCanvas(
        erase: false,
        mask: 15,
        color: 0,
        quarterTurns: turns,
      );
      return _status == LitchcraftStatus.live &&
          !sameLitchcraftDocument(before, documentSession.document);
    }
    if (documentSession.isAdvanced || _advancedWire) {
      LitchcraftFrameEditTarget? target;
      if (animationBoardAuthorityLive) {
        // Rotation is a frame edit, so first pause and latch the frame the
        // board is actually displaying. This prevents playback or a Timeline
        // update from redirecting the gesture to a stale local selection.
        target = await prepareAnimationFrameEdit();
        if (target == null || !_matchesSelectedFrameEditTarget(target)) {
          return false;
        }
      }
      final sourceGeneration = documentSession.generation;
      documentSession.rotateSelectedFrame(quarterTurns);
      if (sourceGeneration == documentSession.generation) return false;
      if (_advancedWire && _status == LitchcraftStatus.live) {
        var accepted = false;
        await _enqueue(() async {
          final exactTarget = target;
          if (exactTarget == null ||
              !_matchesSelectedFrameEditTarget(exactTarget)) {
            return;
          }
          accepted = await _sendDocument(target: exactTarget);
        });
        return accepted;
      }
      return true;
    }
    final before = _canvas;
    await _transformCanvas((canvas) => canvas.rotated(quarterTurns));
    return _canvas != before;
  }

  Future<void> _transformCanvas(
    LitchcraftCanvas Function(LitchcraftCanvas canvas) transform,
  ) async {
    if (boardArtworkUnresolved) return;
    endStroke();
    if (storeReplacementUnresolved) {
      final value = transform(_canvas);
      if (value == _canvas) return;
      documentSession.replaceCanvas(value);
      _canvas = value;
      _offlineDiverged = true;
      _detached = value;
      _notify();
      return;
    }
    if (documentSession.isAdvanced || _advancedWire) {
      documentSession.replaceCanvas(transform(_canvas));
      if (_advancedWire && _status == LitchcraftStatus.live) {
        await _enqueue(_sendDocument);
      }
      return;
    }
    if (_status == LitchcraftStatus.live ||
        _status == LitchcraftStatus.conflict) {
      final pending = _pendingStaticCanvas;
      final previous = pending == null
          ? _canvas
          : _withPendingPaint(
              pending.canvas,
              afterSequence: pending.paintBoundary,
            );
      final value = transform(previous);
      if (value == previous) return;
      final intent = (canvas: value, paintBoundary: _paintSequence);
      _pendingStaticCanvas = intent;
      await _enqueue(() async {
        await _replaceLiveCanvas(value, paintBoundary: intent.paintBoundary);
        if (_pendingStaticCanvas == intent) _pendingStaticCanvas = null;
      });
    } else {
      final value = transform(_canvas);
      if (value == _canvas) return;
      _remember(_canvas);
      _canvas = value;
      _offlineDiverged = true;
      _detached = _canvas;
    }
    _notify();
  }

  void _scheduleStrokeFlush() {
    if (_flushScheduled || !_stroke || _strokeSquares.isEmpty) return;
    _flushScheduled = true;
    scheduleMicrotask(() {
      if (_disposed) return;
      _flushScheduled = false;
      _flushStroke();
    });
  }

  _StoreEditPosition _newStoreEditPosition({bool changesLayout = false}) {
    final position = _StoreEditPosition(
      generation: _generation,
      sourceGeneration: documentSession.replacementGeneration,
      modeSession: _documentSessionId,
      linkToken: _linkToken,
      deviceId: _deviceId!,
      session: _orderedSession!,
    );
    _storeEditPosition = position;
    if (changesLayout) _storeStructuralEditPosition = position;
    return position;
  }

  bool _matchesStoreFrameTarget(LitchcraftFrameEditTarget target) =>
      target.storeGeneration != null &&
      !_disposed &&
      target.generation == _generation &&
      target.sourceGeneration == documentSession.replacementGeneration &&
      target.session == _documentSessionId &&
      target.linkToken == _linkToken &&
      target.deviceId == _deviceId &&
      identical(target._orderedSession, _orderedSession) &&
      target.bootNonce == _orderedSession?.boardBootNonce &&
      _orderedSession?.isActivated == true &&
      _boardActive &&
      _status == LitchcraftStatus.live;

  bool _sameStorePreimage(
    LitchcraftAnimationDocument left,
    LitchcraftAnimationDocument right,
  ) => sameLitchcraftDocument(
    _storeArtworkWithLocalPalette(left),
    _storeArtworkWithLocalPalette(right),
  );

  Future<bool> _admitStoreFrameTarget(
    LitchcraftFrameEditTarget target,
    _StoreEditPosition? predecessor,
  ) async {
    if (!_matchesStoreFrameTarget(target)) {
      return false;
    }
    final before = target._beforeDocument;
    final common = _storeCommonAuthority;
    final board = documentSession.boardDocument;
    if (before == null ||
        common == null ||
        board == null ||
        !_canGuardStorePaint(_orderedSession!) ||
        !_sameStorePreimage(board, before)) {
      return false;
    }
    if (common.revision == target.revision) return true;
    final owner = predecessor ?? target._predecessor;
    if (owner == null || !_storePositionCurrent(owner)) {
      return false;
    }
    final result = await owner.completion.future;
    if (!_matchesStoreFrameTarget(target) ||
        !_storePositionCurrent(owner) ||
        result == null ||
        !_canGuardStorePaint(_orderedSession!)) {
      return false;
    }
    final current = _storeCommonAuthority!;
    return current.revision == result.authority.revision &&
        current.deviceId == result.authority.deviceId &&
        current.bootNonce == result.authority.bootNonce &&
        _sameStorePreimage(result.document, before) &&
        documentSession.boardDocument != null &&
        _sameStorePreimage(documentSession.boardDocument!, result.document);
  }

  _StoreEditResult? _storeConfirmedResult({
    required LitchcraftAnimationDocument before,
    required LitchcraftAnimationDocument document,
    required int selectedBefore,
    required int selectedAfter,
    required int acceptedRevision,
  }) {
    _reconcileStoreCommonAuthority();
    final authority = _storeCommonAuthority;
    if (authority == null ||
        authority.revision != acceptedRevision ||
        _orderedSession == null ||
        !_canGuardStorePaint(_orderedSession!) ||
        documentSession.boardDocument == null ||
        !_sameStorePreimage(documentSession.boardDocument!, document)) {
      return null;
    }
    return (
      authority: authority,
      before: before,
      document: document,
      selectedBefore: selectedBefore,
      selectedAfter: selectedAfter,
    );
  }

  void _finishStoreEditPosition(
    _StoreEditPosition position,
    _StoreEditResult? result,
  ) {
    if (position.completion.isCompleted) return;
    if (identical(_storeStructuralEditPosition, position)) {
      _storeStructuralEditPosition = null;
    }
    if (identical(_storeToolEditPosition, position)) {
      _storeToolEditPosition = null;
    }
    _reconcileStoreCommonAuthority();
    final current = _storeCommonAuthority;
    if (!_storePositionCurrent(position) ||
        result == null ||
        current == null ||
        current.revision != result.authority.revision ||
        current.bootNonce != result.authority.bootNonce ||
        current.deviceId != result.authority.deviceId ||
        !_canGuardStorePaint(position.session) ||
        documentSession.boardDocument == null ||
        !_sameStorePreimage(documentSession.boardDocument!, result.document)) {
      result = null;
    } else {
      // The existing same-live-R common binding proves an asynchronous save.
      // Publish its current SavedG without changing the accepted artwork/R.
      result = (
        authority: current,
        before: result.before,
        document: result.document,
        selectedBefore: result.selectedBefore,
        selectedAfter: result.selectedAfter,
      );
    }
    position.result = result;
    position.completion.complete(result);
    if (result == null && identical(_storeEditPosition, position)) {
      _storeEditPosition = null;
    }
    _resumeStorePaintIfReady(position.session);
  }

  Uint8List _storeGuardBody(ConnectionSession session) {
    if (!_canGuardStorePaint(session)) {
      throw const LitchcraftResyncRequired();
    }
    final common = _storeCommonAuthority!;
    final bytes = ByteData(8)
      ..setUint32(0, common.generation, Endian.little)
      ..setUint32(4, common.revision, Endian.little);
    return bytes.buffer.asUint8List();
  }

  void _acceptStoreEditRevision(int revision) {
    final common = _storeCommonAuthority;
    if (common == null || revision == 0) {
      throw const LitchcraftResyncRequired();
    }
    final observed = _storeAuthority;
    final matching =
        observed != null &&
        observed.deviceId == common.deviceId &&
        observed.bootNonce == common.bootNonce &&
        observed.revision == revision;
    // The exact receipt proves the new live bytes even while an earlier save
    // advances their durable fence. Bind matching State before publishing the
    // completed edit position; later gestures must not lose that predecessor.
    _storeCommonAuthority = (
      deviceId: common.deviceId,
      bootNonce: common.bootNonce,
      generation: matching ? observed.generation : common.generation,
      revision: revision,
      dirty: matching ? observed.dirty : true,
    );
    _frameStoreGeneration = _storeCommonAuthority!.generation;
  }

  Future<void> _selectStoreFrameForEdit(int frame) async {
    final ordered = _orderedSession;
    if (ordered == null || !ordered.isActivated || !_boardActive) {
      throw const LitchcraftResyncRequired();
    }
    final body = Uint8List(6);
    body[0] = 2;
    body[1] = frame;
    ByteData.sublistView(
      body,
    ).setUint32(2, _frameStoreGeneration, Endian.little);
    final outcome = await ordered.sendOrdered(
      ConnectionSession.litchcraftTimelineOpcode,
      body,
    );
    if (outcome.disposition != ConnectionSessionDisposition.applied) {
      throw LitchcraftRejected(outcome.reason);
    }
  }

  Future<_StoreEditResult?> _sendAdvancedEmitterCommand({
    required int frame,
    required int stroke,
    required int flags,
    required int mask,
    required List<int> squares,
    int? color,
    LitchcraftEffectStyle? effect,
    int effectDurationMs = 0,
  }) async {
    final link = _link!;
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final before = documentSession.boardDocument!;
    final physicalRevision = _physicalStoreEditRevision;
    if (frame >= before.frames.length) throw const LitchcraftResyncRequired();
    final expected = patchLitchcraftEmitters(
      before,
      frame: frame,
      squares: squares,
      mask: mask,
      color: color,
      effect: effect,
      effectDurationMs: effectDurationMs,
    );
    final revision = usesFrameStoreDocument
        ? _storeCommonAuthority!.revision
        : _documentRevision;
    final coherent = !_documentDirty;
    _unconfirmedPaintDocumentBase = before;
    _unconfirmedDocument = expected;
    _unconfirmedDocumentBoardId = _deviceId;
    final ordered = _orderedSession;
    LitchcraftV2State? ack;
    ConnectionSessionOutcome? orderedOutcome;
    if (ordered != null && ordered.isActivated) {
      if (usesFrameStoreDocument &&
          flags & 1 != 0 &&
          (_frameStorePlaying != false ||
              (_advancedTimeline?.selectedFrame ?? _advancedSelectedFrame) !=
                  frame)) {
        await _selectStoreFrameForEdit(frame);
      }
      final body = effect == null
          ? <int>[
              frame,
              stroke & 0xff,
              stroke >> 8,
              flags,
              mask,
              color! >> 16,
              color >> 8 & 0xff,
              color & 0xff,
              ...squares,
            ]
          : <int>[
              frame,
              effect.id,
              effectDurationMs & 0xff,
              effectDurationMs >> 8,
              mask,
              stroke & 0xff,
              stroke >> 8,
              flags,
              ...squares,
            ];
      final guard = usesFrameStoreDocument
          ? ordered.sendOrdered(
              ConnectionSession.litchcraftStoreGuardOpcode,
              _storeGuardBody(ordered),
            )
          : null;
      final edit = ordered.sendOrdered(
        effect == null ? 3 : 4,
        Uint8List.fromList(body),
      );
      if (guard != null &&
          (await guard).disposition != ConnectionSessionDisposition.applied) {
        await edit;
        throw const LitchcraftResyncRequired();
      }
      orderedOutcome = await edit;
      if (orderedOutcome.disposition != ConnectionSessionDisposition.applied) {
        if (usesFrameStoreDocument) {
          throw const LitchcraftResyncRequired();
        }
        throw LitchcraftRejected(orderedOutcome.reason);
      }
    } else {
      ack = effect == null
          ? await link.paintV2(
              session: _documentSessionId,
              frame: frame,
              stroke: stroke,
              flags: flags,
              mask: mask,
              color: color!,
              squares: squares,
            )
          : await link.effectV2(
              session: _documentSessionId,
              frame: frame,
              effect: effect.id,
              durationMs: effectDurationMs,
              stroke: stroke,
              flags: flags,
              mask: mask,
              squares: squares,
            );
    }
    if (generation != _generation ||
        !identical(link, _link) ||
        sourceGeneration != documentSession.replacementGeneration) {
      throw const LitchcraftTransferCancelled();
    }
    final expectedRevision =
        (revision + (sameLitchcraftDocument(before, expected) ? 0 : 1)) &
        0xffffffff;
    // A receipt proves this footprint in acceptance order. It cannot prove
    // untouched fields when another writer crossed the revision boundary.
    final acceptedRevision = orderedOutcome?.domainRevision ?? ack!.revision;
    if (usesFrameStoreDocument) {
      if (acceptedRevision != expectedRevision) {
        throw const LitchcraftResyncRequired();
      }
      _acceptStoreEditRevision(acceptedRevision);
      if (_physicalStoreEditRevision != physicalRevision ||
          (_storeAuthority?.revision ?? 0) > acceptedRevision) {
        throw const LitchcraftResyncRequired();
      }
      documentSession.confirm(_deviceId!, expected);
      _forgetSelectedFrameStoreResident();
      _documentDirty = false;
    } else {
      documentSession.confirm(_deviceId!, expected);
      _documentRevision = acceptedRevision;
      _documentDirty = !coherent || acceptedRevision != expectedRevision;
      if (!_documentDirty) {
        _coherentDocumentRevision = acceptedRevision;
        _coherentDocumentSession = _documentSessionId;
        _coherentDocumentBoardId = _deviceId;
      }
    }
    _unconfirmedDocument = null;
    _unconfirmedPaintDocumentBase = null;
    if (ack != null) {
      _applyAdvancedRuntimeState(ack);
      _applyPendingAdvancedRuntimeMetadata();
    }
    return usesFrameStoreDocument
        ? _storeConfirmedResult(
            before: before,
            document: expected,
            selectedBefore: frame,
            selectedAfter: frame,
            acceptedRevision: acceptedRevision,
          )
        : null;
  }

  Future<_StoreEditResult?> _sendOrderedEmitterBatch({
    required int frame,
    required int stroke,
    required bool begin,
    required bool selectBeforeBegin,
    required bool end,
    required int mask,
    required int color,
    required bool clearEffects,
    required List<int> squares,
    int? paintGeneration,
  }) async {
    final ordered = _orderedSession!;
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final before = documentSession.boardDocument!;
    final physicalRevision = _physicalStoreEditRevision;
    final storeDocument = usesFrameStoreDocument;
    var revision = _documentRevision;
    var expected = before;
    var changed = 0;
    final pending = <Future<ConnectionSessionOutcome>>[];
    if (storeDocument) {
      if (begin && selectBeforeBegin) {
        await _selectStoreFrameForEdit(frame);
      }
      final guard = _storeGuardBody(ordered);
      revision = ByteData.sublistView(guard).getUint32(4, Endian.little);
      pending.add(
        ordered.sendOrdered(
          ConnectionSession.litchcraftStoreGuardOpcode,
          guard,
        ),
      );
    }
    int changedSquares(
      LitchcraftAnimationDocument before,
      LitchcraftAnimationDocument after,
      List<int> targets,
    ) {
      if (!storeDocument) return sameLitchcraftDocument(before, after) ? 0 : 1;
      var count = 0;
      for (final square in targets.toSet()) {
        for (var region = 0; region < 4; region++) {
          if (mask & (1 << region) == 0) continue;
          final index = square * 4 + region;
          final prior = before.frames[frame].emitters[index];
          final next = after.frames[frame].emitters[index];
          if (prior.color != next.color ||
              prior.effect != next.effect ||
              prior.effectDurationMs != next.effectDurationMs) {
            count++;
            break;
          }
        }
      }
      return count;
    }

    final chunkWidth = 2;
    final chunkCount = squares.isEmpty
        ? 1
        : (squares.length + chunkWidth - 1) ~/ chunkWidth;
    for (var chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
      final chunk = squares
          .skip(chunkIndex * chunkWidth)
          .take(chunkWidth)
          .toList();
      final flags =
          (begin && chunkIndex == 0 ? 1 : 0) |
          (end && chunkIndex == chunkCount - 1 ? 2 : 0);
      final clearChunkEffects =
          clearEffects &&
          chunk.any(
            (square) => List.generate(4, (region) => region).any(
              (region) =>
                  mask & (1 << region) != 0 &&
                  expected.frames[frame].emitters[square * 4 + region].effect !=
                      LitchcraftEffectStyle.off,
            ),
          );
      if (clearChunkEffects) {
        final body = <int>[
          frame,
          LitchcraftEffectStyle.off.id,
          0,
          0,
          mask,
          stroke & 0xff,
          stroke >> 8,
          flags & 1,
          ...chunk,
        ];
        pending.add(ordered.sendOrdered(4, Uint8List.fromList(body)));
        final next = patchLitchcraftEmitters(
          expected,
          frame: frame,
          squares: chunk,
          mask: mask,
          effect: LitchcraftEffectStyle.off,
          effectDurationMs: 0,
        );
        changed += changedSquares(expected, next, chunk);
        expected = next;
      }
      final body = <int>[
        frame,
        stroke & 0xff,
        stroke >> 8,
        clearChunkEffects ? flags & 2 : flags,
        mask,
        color >> 16,
        color >> 8 & 0xff,
        color & 0xff,
        ...chunk,
      ];
      pending.add(ordered.sendOrdered(3, Uint8List.fromList(body)));
      final next = patchLitchcraftEmitters(
        expected,
        frame: frame,
        squares: chunk,
        mask: mask,
        color: color,
      );
      changed += changedSquares(expected, next, chunk);
      expected = next;
    }
    _unconfirmedPaintDocumentBase = before;
    _unconfirmedDocument = expected;
    _unconfirmedDocumentBoardId = _deviceId;
    final outcomes = await Future.wait(pending);
    if (generation != _generation ||
        sourceGeneration != documentSession.replacementGeneration) {
      throw const LitchcraftTransferCancelled();
    }
    for (final outcome in outcomes) {
      if (outcome.disposition != ConnectionSessionDisposition.applied) {
        if (storeDocument) throw const LitchcraftResyncRequired();
        throw LitchcraftRejected(outcome.reason);
      }
    }
    final acceptedRevision = outcomes.last.domainRevision;
    final coherent = !_documentDirty;
    if (storeDocument) {
      // A later coalesced checkpoint cannot prove this batch's untouched fields.
      if (acceptedRevision != ((revision + changed) & 0xffffffff)) {
        throw const LitchcraftResyncRequired();
      }
      if (paintGeneration != null && changed > 0) {
        _acceptedStoreStroke = (
          stroke: stroke,
          frame: frame,
          generation: generation,
          sourceGeneration: sourceGeneration,
          modeSession: _documentSessionId,
          linkToken: _linkToken,
          bootNonce: ordered.boardBootNonce,
          paintGeneration: paintGeneration,
          revision: acceptedRevision,
          deviceId: _deviceId!,
          session: ordered,
        );
        _appTrace?.record(
          'litchcraft.store_history_stroke_prefix_accepted',
          fields: {
            'stroke': stroke,
            'frame': frame,
            'revision': acceptedRevision,
            'paint_generation': paintGeneration,
          },
        );
      }
      _acceptStoreEditRevision(acceptedRevision);
      if (_physicalStoreEditRevision != physicalRevision ||
          (_storeAuthority?.revision ?? 0) > acceptedRevision) {
        throw const LitchcraftResyncRequired();
      }
      documentSession.confirm(_deviceId!, expected);
      _forgetSelectedFrameStoreResident();
      _documentDirty = false;
    } else {
      documentSession.confirm(_deviceId!, expected);
      _documentRevision = acceptedRevision;
      _documentDirty =
          !coherent || acceptedRevision != ((revision + changed) & 0xffffffff);
      if (!_documentDirty) {
        _coherentDocumentRevision = acceptedRevision;
        _coherentDocumentSession = _documentSessionId;
        _coherentDocumentBoardId = _deviceId;
      }
    }
    _unconfirmedDocument = null;
    _unconfirmedPaintDocumentBase = null;
    return storeDocument
        ? _storeConfirmedResult(
            before: before,
            document: expected,
            selectedBefore: frame,
            selectedAfter: frame,
            acceptedRevision: acceptedRevision,
          )
        : null;
  }

  Future<void> _sendDocumentDelta() async {
    if (_pendingStoreCanvas != null) return;
    final intended = documentSession.document;
    final board = documentSession.boardDocument;
    if (board == null || !sameLitchcraftDocumentLayout(board, intended)) {
      _status = LitchcraftStatus.conflict;
      _offlineDiverged = true;
      _notify();
      return;
    }
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    int? lastEditedFrame;
    for (var frame = 0; frame < intended.frames.length; frame++) {
      final stroke = _strokeId = (_strokeId % 0xffff) + 1;
      var begun = false;
      for (var index = 0; index < 256; index++) {
        if (generation != _generation ||
            sourceGeneration != documentSession.replacementGeneration) {
          return;
        }
        final desired = intended.frames[frame].emitters[index];
        final actual =
            documentSession.boardDocument!.frames[frame].emitters[index];
        if (desired.color != actual.color) {
          await _sendAdvancedEmitterCommand(
            frame: frame,
            stroke: stroke,
            flags: begun ? 0 : 1,
            mask: 1 << (index % 4),
            squares: [index ~/ 4],
            color: desired.color,
          );
          begun = true;
        }
        if (desired.effect != actual.effect ||
            desired.effectDurationMs != actual.effectDurationMs) {
          await _sendAdvancedEmitterCommand(
            frame: frame,
            stroke: stroke,
            flags: begun ? 0 : 1,
            mask: 1 << (index % 4),
            squares: [index ~/ 4],
            effect: desired.effect,
            effectDurationMs: desired.effectDurationMs,
          );
          begun = true;
        }
      }
      if (begun) {
        lastEditedFrame = frame;
        await _sendAdvancedEmitterCommand(
          frame: frame,
          stroke: stroke,
          flags: 2,
          mask: 15,
          squares: [],
          color: 0,
        );
      }
    }
    if (usesFrameStoreDocument) {
      final selected = documentSession.selectedFrameIndex;
      if (lastEditedFrame != null && lastEditedFrame != selected) {
        await _selectStoreFrameForEdit(selected);
      }
      _showStoreFrame(documentSession.selectedFrameIndex);
    }
  }

  void _flushStroke({bool end = false}) {
    if (_status != LitchcraftStatus.live || _exiting != null) {
      _flushScheduled = false;
      _strokeSquares.clear();
      return;
    }
    final deferredStoreUpload =
        storeReplacementUnresolved &&
        (_storeTransferInFlight || _acceptedStoreCommitAwaitingRoot) &&
        usesFrameStoreDocument &&
        _orderedSession?.isActivated == true;
    if (storeReplacementUnresolved && !deferredStoreUpload) {
      _flushScheduled = false;
      if (end) _strokeSquares.clear();
      _offlineDiverged = true;
      _detached = _canvas;
      return;
    }
    if (end) {
      _flushScheduled = false;
    } else if (_queued > 0) {
      // The active command's completion schedules the next chunk. Do not poll
      // or pace the stroke with timers while authority is in flight.
      return;
    }
    _StoreEditPosition? deferredStrokePredecessor;
    var waitsForStoreLayout = false;
    final clearEffects = _strokeEraser ?? _eraser;
    final storeDocument = usesFrameStoreDocument;
    final ordered =
        (_advancedWire || storeDocument) &&
        _orderedSession?.isActivated == true;
    if (ordered && storeDocument && !deferredStoreUpload) {
      final session = _orderedSession!;
      final structure = _storeStructuralEditPosition;
      if (structure != null && !structure.completion.isCompleted) {
        // Capture the gesture now, including its frame and pixels. A later
        // Add/Delete must follow this paint rather than capture unsent pixels
        // in a preimage the board can never prove.
        deferredStrokePredecessor = _storeEditPosition;
        waitsForStoreLayout = true;
      }
      final pendingEnd = _pendingStoreStrokeEnd;
      if (pendingEnd != null &&
          (!identical(pendingEnd.session, session) ||
              pendingEnd.generation != _generation ||
              pendingEnd.sourceGeneration !=
                  documentSession.replacementGeneration)) {
        _pendingStoreStrokeEnd = null;
      }
      // State may expose our first batch before its receipt advances common.
      // Keep both its finger-up tail and a later closed gesture on the existing
      // mutation tail, with their original stroke/frame/tool. The later gesture
      // must prove this exact predecessor before it can send its own Guard.
      final predecessor = _storeEditPosition;
      final followsLocalBatch =
          end &&
          _queued > 0 &&
          _unconfirmedDocument != null &&
          identical(
            _unconfirmedPaintDocumentBase,
            documentSession.boardDocument,
          ) &&
          predecessor != null &&
          _storePositionCurrent(predecessor);
      if (!_canGuardStorePaint(session) && followsLocalBatch) {
        deferredStrokePredecessor = predecessor;
      }
      if (!_canGuardStorePaint(session) &&
          !followsLocalBatch &&
          deferredStrokePredecessor == null) {
        _flushScheduled = false;
        if (end) {
          // The document already owns this closed gesture. Its selected frame,
          // erase semantics, and colors are immutable local intent; replaying
          // these squares after endStroke clears the stroke context is unsafe.
          if (_strokeBegun && _pendingStoreStrokeEnd == null) {
            _pendingStoreStrokeEnd = (
              session: session,
              generation: _generation,
              sourceGeneration: documentSession.replacementGeneration,
              stroke: _strokeId,
              frame: _strokeFrame ?? documentSession.selectedFrameIndex,
            );
          }
          _strokeSquares.clear();
        }
        if (!_storePaintWaitingForAuthority) {
          _storePaintWaitingForAuthority = true;
          unawaited(
            session
                .requestState(
                  code: ConnectionSession.litchcraftStoreAuthorityStateCode,
                )
                .catchError((Object _) {}),
          );
        }
        final current = _storeAuthority;
        final common = _storeCommonAuthority;
        if (current != null &&
            common != null &&
            current.bootNonce == common.bootNonce &&
            current.revision > common.revision) {
          _schedulePhysicalStoreRepair();
        }
        return;
      }
      if (_pendingStoreStrokeEnd != null) {
        if (end) {
          // This later closed gesture remains document-owned until catch-up.
          _flushScheduled = false;
          _strokeSquares.clear();
        }
        _storePaintWaitingForAuthority = true;
        _resumeStorePaintIfReady(session);
        return;
      }
      _storePaintWaitingForAuthority = false;
    }
    // The ordered peer accepts two squares per Paint/Effect packet while the
    // Store records each changed square separately. The session owns grant
    // backpressure when an effect-off packet makes the batch exceed one grant.
    final batchSquareLimit = ordered
        ? (storeDocument ? 8 : (clearEffects ? 8 : 16))
        : _advancedWire
        ? 3
        : 9;
    final squares = end
        ? _strokeSquares.toList()
        : _strokeSquares.take(batchSquareLimit).toList();
    _strokeSquares.removeAll(squares);
    final stroke = _strokeId;
    final frame = _strokeFrame ?? documentSession.selectedFrameIndex;
    final color = clearEffects ? 0 : _color;
    final mask = _strokeTargetMask ?? _targetMask;
    final sourceGeneration = documentSession.replacementGeneration;
    final chunkSize = ordered
        ? 2
        : _advancedWire
        ? 3
        : 9;
    final paintGeneration = _paintQueueGeneration;
    if (squares.isEmpty && (!end || !_strokeBegun)) return;
    final begin = !_strokeBegun;
    // The serialized sender may run after another gesture has begun. Preserve
    // this gesture's board-selection requirement with its immutable batch.
    final selectBeforeBegin = begin && _strokeStoreSelectRequired;
    _strokeBegun = true;
    final pixels = {
      for (final entry in _pendingPaint.entries)
        if (squares.contains(entry.key ~/ 4) &&
            (mask & (1 << (entry.key % 4))) != 0)
          entry.key: entry.value,
    };
    if (deferredStrokePredecessor != null &&
        _queued + _storePaintWaiters >= 128) {
      _transportFailed(
        StateError('Board edits are backed up; draft preserved'),
      );
      return;
    }
    final position = storeDocument && ordered ? _newStoreEditPosition() : null;
    Future<void> sendBatch() => _enqueue(
      () async {
        try {
          if (deferredStoreUpload) {
            // Keep this gesture on the existing mutation tail behind Commit,
            // including its delayed durable receipt. Bind the new root before
            // deriving the Guard; the staging-time generation is obsolete.
            final pendingRoot = _pendingStoreCommit;
            if (pendingRoot != null) await pendingRoot.completion;
            if (!_storePositionCurrent(position!) ||
                storeReplacementUnresolved) {
              return;
            }
            if (!_canGuardStorePaint(position.session)) {
              await _requestFreshStoreAuthority(position.session);
            }
            if (!_storePositionCurrent(position) ||
                !_canGuardStorePaint(position.session)) {
              throw const LitchcraftResyncRequired();
            }
          }
          final predecessor = deferredStrokePredecessor;
          if (predecessor != null) {
            final result = await predecessor.completion.future;
            if (!_storePositionCurrent(predecessor) ||
                sourceGeneration != documentSession.replacementGeneration ||
                result == null ||
                !_canGuardStorePaint(predecessor.session) ||
                _storeCommonAuthority?.revision != result.authority.revision ||
                documentSession.boardDocument == null ||
                !_sameStorePreimage(
                  documentSession.boardDocument!,
                  result.document,
                )) {
              throw const LitchcraftResyncRequired();
            }
          }
          if (ordered) {
            final orderedBatchLimit = storeDocument
                ? batchSquareLimit
                : (squares.isEmpty ? 1 : squares.length);
            for (
              var offset = 0;
              offset < squares.length || offset == 0;
              offset += orderedBatchLimit
            ) {
              if (paintGeneration != _paintQueueGeneration) return;
              final chunk = squares
                  .skip(offset)
                  .take(orderedBatchLimit)
                  .toList();
              final result = await _sendOrderedEmitterBatch(
                frame: frame,
                stroke: stroke,
                begin: begin && offset == 0,
                selectBeforeBegin: selectBeforeBegin && offset == 0,
                end: end && offset + orderedBatchLimit >= squares.length,
                mask: mask,
                color: color,
                clearEffects: clearEffects,
                squares: chunk,
                paintGeneration: paintGeneration,
              );
              if (position != null) position.result = result;
            }
            if (sourceGeneration != documentSession.replacementGeneration) {
              return;
            }
            for (final entry in pixels.entries) {
              if (_pendingPaint[entry.key] == entry.value) {
                _pendingPaint.remove(entry.key);
              }
            }
            if (_status == LitchcraftStatus.live && !_offlineDiverged) {
              if (storeDocument) {
                _showStoreFrame(documentSession.selectedFrameIndex);
              } else {
                _canvas = _withPendingPaint(_board);
              }
              _commonCanvas = _board;
              _commonBoardId = _deviceId;
            }
            return;
          }
          // One queued action owns this bounded remainder. Each completed packet
          // admits the next <=3-square packet; no eager tail of chunk closures.
          for (
            var offset = 0;
            offset < squares.length || (end && offset == 0);
            offset += chunkSize
          ) {
            final chunk = squares.skip(offset).take(chunkSize).toList();
            final flags =
                (begin && offset == 0 ? 1 : 0) |
                (end && offset + chunkSize >= squares.length ? 2 : 0);
            if (!_boardActive) {
              // Taps after background departure remain in the local draft.
              _offlineDiverged = true;
              _detached = _canvas;
              return;
            }
            if (_advancedWire) {
              if (clearEffects && chunk.isNotEmpty) {
                await _sendAdvancedEmitterCommand(
                  frame: frame,
                  effect: LitchcraftEffectStyle.off,
                  stroke: stroke,
                  flags: flags & 1,
                  mask: mask,
                  squares: chunk,
                );
              }
              await _sendAdvancedEmitterCommand(
                frame: frame,
                stroke: stroke,
                flags: clearEffects && chunk.isNotEmpty ? flags & 2 : flags,
                mask: mask,
                color: color,
                squares: chunk,
              );
              continue;
            }
            // Only one command can be in flight. If its notifications disappear,
            // this exact result proves an accepted prefix of the local paint stream.
            _unconfirmedPaintCanvas = _board.paint(chunk, mask, color);
            _unconfirmedPaintBoardId = _deviceId;
            try {
              await _link!.send(LitchcraftProtocol.paint, _sessionId, [
                ...LitchcraftProtocol.u16(stroke),
                flags,
                ...chunk,
              ]);
            } on LitchcraftResyncRequired {
              await _pullSnapshot();
            }
            if (sourceGeneration != documentSession.replacementGeneration) {
              return;
            }
            if (_needsResync) await _pullSnapshot();
            for (final entry in pixels.entries) {
              if (chunk.contains(entry.key ~/ 4) &&
                  _pendingPaint[entry.key] == entry.value) {
                _pendingPaint.remove(entry.key);
              }
            }
            _unconfirmedPaintCanvas = null;
            _unconfirmedPaintBoardId = null;
            if (_status == LitchcraftStatus.live && !_offlineDiverged) {
              _canvas = _withPendingPaint(_board);
              _commonCanvas = _board;
              _commonBoardId = _deviceId;
            }
          }
        } catch (_) {
          if (position != null) position.result = null;
          rethrow;
        }
      },
      recoverBusy: true,
      paintGeneration: paintGeneration,
    );
    Future<void> afterPredecessor() async {
      // A preceding layout action may still be waiting for the domain slot.
      // Bound retained batches without treating an authority wait as active
      // transport work or occupying the tail needed to submit its predecessor.
      ++_storePaintWaiters;
      try {
        await deferredStrokePredecessor!.completion.future;
      } finally {
        --_storePaintWaiters;
      }
      await sendBatch();
    }

    // Ordinary State-ahead finger-up batches already follow their predecessor
    // on the tail. Reserve their place now, before a later Undo or Play.
    final write = waitsForStoreLayout && deferredStrokePredecessor != null
        ? afterPredecessor()
        : sendBatch();
    if (position != null) {
      unawaited(
        write.then<void>(
          (_) => _finishStoreEditPosition(position, position.result),
          onError: (Object _, StackTrace _) =>
              _finishStoreEditPosition(position, null),
        ),
      );
    }
  }

  LitchcraftCanvas _withPendingPaint(
    LitchcraftCanvas board, {
    int afterSequence = -1,
  }) {
    if (_pendingPaint.isEmpty) return board;
    final colors = board.colors.toList();
    for (final entry in _pendingPaint.entries) {
      if (entry.value.sequence <= afterSequence) continue;
      colors[entry.key] = entry.value.color;
    }
    return LitchcraftCanvas(colors: colors, topology: board.topology);
  }

  void _clearPendingPaint({int? throughSequence}) {
    _pendingPaint.removeWhere(
      (_, pixel) =>
          throughSequence == null || pixel.sequence <= throughSequence,
    );
    _unconfirmedPaintCanvas = null;
    _unconfirmedPaintBoardId = null;
  }

  void endStroke() {
    if (!_stroke) return;
    _appTrace.finishUserAction(
      _supportStrokeAction,
      _supportStrokeSquares.isEmpty
          ? SupportActionOutcome.noChange
          : SupportActionOutcome.appliedOnPhone,
      count: _supportStrokeSquares.length,
    );
    _supportStrokeAction = 0;
    _supportStrokeSquares.clear();
    _flushStroke(end: true);
    if (_status != LitchcraftStatus.live &&
        _strokeBefore != null &&
        _strokeBefore != _canvas) {
      _remember(_strokeBefore!);
    }
    _strokeBefore = null;
    _stroke = false;
    _strokeFrame = null;
    _strokeEraser = null;
    _strokeTargetMask = null;
    if (documentSession.isAdvanced || _advancedWire || usesFrameStoreDocument) {
      documentSession.endStroke();
    }
    final deferredFrame = _deferredBoardFrame;
    _deferredBoardFrame = null;
    if (deferredFrame != null && usesFrameStoreDocument) {
      _showStoreFrame(deferredFrame);
    }
    _notify();
  }

  void _remember(LitchcraftCanvas previous) {
    if (documentSession.isAdvanced || _advancedWire) return;
    _undo.add(previous);
    if (_undo.length > 64) _undo.removeAt(0);
    _redo.clear();
  }

  Future<void> setBrush({int? color, bool? eraser, int? targetMask}) {
    _colorPreviewOrigin = null;
    return _setBrush(color: color, eraser: eraser, targetMask: targetMask);
  }

  /// A discrete palette choice has the same history semantics as every other
  /// committed color choice.
  Future<void> selectPaletteColor(int color) =>
      setBrush(color: color, eraser: false);

  void beginBrushColorPreview() {
    _colorPreviewOrigin ??= _color;
  }

  Future<void> previewBrushColor(int color) async {
    beginBrushColorPreview();
    await _setBrush(
      color: color,
      eraser: false,
      rememberPreviousColor: false,
      syncBoard: false,
    );
    _queueLatestBrushPreview();
  }

  Future<void> commitBrushColorPreview(int color) async {
    beginBrushColorPreview();
    final origin = _colorPreviewOrigin;
    if (origin != null && origin != color) _previousColor = origin;
    try {
      await _setBrush(
        color: color,
        eraser: false,
        rememberPreviousColor: origin == null,
        syncBoard: false,
      );
      _queueLatestBrushPreview();
    } finally {
      // Capture the final Tool before allowing passive State to project again.
      _colorPreviewOrigin = null;
    }
  }

  /// Release a canceled gesture without sending or notifying during teardown.
  void cancelBrushColorPreview() {
    _colorPreviewOrigin = null;
  }

  /// Keep a hue drag finger-owned: local color changes render immediately,
  /// while BLE carries at most one Tool write plus the newest replacement.
  /// Intermediate pointer samples are presentation state, not transactions.
  Future<void> _queueLatestBrushPreview() {
    final link = _link;
    if (_status != LitchcraftStatus.live || link == null) {
      return Future<void>.value();
    }
    _pendingBrush = (
      link: link,
      session: _sessionId,
      intent: _selectionIntent,
      payload: <int>[
        _eraser ? 1 : 0,
        _targetMask,
        ...LitchcraftProtocol.rgb(_color),
      ],
    );
    final canvas = _storeToolEditPosition;
    final canvasOwnsTool =
        canvas != null &&
        _storePositionCurrent(canvas) &&
        !canvas.completion.isCompleted;
    if (_brushPreviewWriteQueued) {
      return canvasOwnsTool ? Future<void>.value() : _brushWriteFuture!;
    }
    _brushPreviewWriteQueued = true;
    // Legacy paint carries no RGB. Preserve its stroke ordering; document
    // transfers and advanced paint carry their own data and cannot block Tool.
    final precedingPaint = !_advancedWire && _pendingPaint.isNotEmpty
        ? _tail
        : null;
    _brushWriteFuture = (() async {
      try {
        if (precedingPaint != null) await precedingPaint;
        while (_pendingBrush != null) {
          final canvas = _storeToolEditPosition;
          if (canvas != null &&
              _storePositionCurrent(canvas) &&
              !canvas.completion.isCompleted) {
            // A registered Fill owns Tool even before it acquires the Canvas
            // slot. Re-read the newest preview after it completes: a later
            // Fill may have superseded that preview in the meantime.
            await canvas.completion.future;
            continue;
          }
          final pending = _pendingBrush!;
          _pendingBrush = null;
          bool current() =>
              pending.intent == _selectionIntent &&
              identical(pending.link, _link) &&
              pending.session == _sessionId &&
              _boardActive;
          if (!current()) continue;
          try {
            final observation = _brushObservation;
            final ordered = _orderedSession;
            if (ordered != null) {
              final result = await ordered.sendOrdered(
                ConnectionSession.litchcraftToolOpcode,
                Uint8List.fromList(pending.payload),
              );
              if (result.disposition != ConnectionSessionDisposition.applied) {
                throw StateError('Litchcraft tool rejected (${result.reason})');
              }
              if (current() && observation == _brushObservation) {
                // Applied confirms this selection before its passive State
                // may arrive. Preserve a newer physical observation if present.
                _confirmedBrush = (
                  color: LitchcraftProtocol.readRgb(pending.payload, 2),
                  eraser: pending.payload[0] != 0,
                  targetMask: pending.payload[1],
                  project: false,
                );
              } else if (current() && ordered.stateStreamToken != 0) {
                // An observation during the write can precede this Tool or
                // follow it physically. Keep preview ownership until the
                // existing typed query returns current board truth.
                await ordered.readState(
                  code: ConnectionSession.litchcraftStateCode,
                  earliestSequence: _connectionStateRevision,
                  accept: (packet) =>
                      packet.body.length == 10 &&
                      packet.body[5] > 0 &&
                      packet.body[5] <= 15,
                );
              }
              continue;
            }
            final ack = await pending.link.send(
              LitchcraftProtocol.tool,
              pending.session,
              pending.payload,
            );
            if (current() &&
                observation == _brushObservation &&
                _acceptBrushRevision(ack)) {
              _confirmedBrush = (
                color: LitchcraftProtocol.readRgb(pending.payload, 2),
                eraser: ack.payload[5] != 0,
                targetMask: ack.payload[6],
                project: false,
              );
            }
          } on LitchcraftRejected catch (error) {
            if (!current()) continue;
            if (!_advancedWire || error.status != 1) {
              _transportFailed(error);
              continue;
            }
            // Older animation firmware carries selection in paint packets.
            _confirmedBrush = (
              color: LitchcraftProtocol.readRgb(pending.payload, 2),
              eraser: pending.payload[0] != 0,
              targetMask: pending.payload[1],
              project: false,
            );
          } catch (error) {
            if (current()) _transportFailed(error);
          }
        }
      } finally {
        _brushPreviewWriteQueued = false;
        _brushWriteFuture = null;
        _projectConfirmedBrush();
      }
    })();
    return canvasOwnsTool ? Future<void>.value() : _brushWriteFuture!;
  }

  Future<void> _bindConnectionState(ConnectionSession? session) async {
    await _connectionState?.cancel();
    _connectionState = null;
    _connectionStateRevision = 0;
    _connectionStateObservedAt = 0;
    _connectionPlaybackStateRevision = 0;
    _storeAuthority = null;
    _storeAuthorityStateSequence = 0;
    _storeCommonBindingGeneration = null;
    _pendingStoreEntryInspection = null;
    _storeEntryInspection = null;
    _resumingStoreEntry = false;
    if (session == null) return;
    _connectionState = session.states.listen(
      _readConnectionState,
      onError: (Object error) {
        if (identical(session, _orderedSession)) {
          _transportFailed(error);
        }
      },
    );
    try {
      await session.requestState(code: ConnectionSession.litchcraftStateCode);
      await session.requestState(
        code: ConnectionSession.litchcraftStoreAuthorityStateCode,
      );
    } catch (error) {
      if (identical(session, _orderedSession)) {
        _transportFailed(error);
      }
    }
  }

  void _bindStoreCommonAuthorityIfReady() {
    final authority = _storeAuthority;
    if (authority == null ||
        _storeCommonBindingGeneration != authority.generation ||
        authority.dirty ||
        authority.revision == 0 ||
        authority.deviceId != _deviceId ||
        documentSession.boardDocument == null) {
      return;
    }
    _storeCommonAuthority = authority;
    _storeCommonBindingGeneration = null;
  }

  void _reconcileStoreCommonAuthority() {
    final authority = _storeAuthority;
    final common = _storeCommonAuthority;
    final board = documentSession.boardDocument;
    if (authority == null ||
        common == null ||
        board == null ||
        _storeTransferInFlight ||
        _physicalStoreRepairThroughRevision != 0 ||
        _unconfirmedDocument != null ||
        authority.deviceId != common.deviceId ||
        authority.bootNonce != common.bootNonce) {
      return;
    }
    if (authority.revision == common.revision) {
      // A predecessor save can advance generation while other accepted edits
      // remain dirty. The unchanged live revision, device and boot prove the
      // same live document; generation is its durable fence, not its identity.
      _storeCommonAuthority = authority;
      _frameStoreGeneration = authority.generation;
    } else {
      final session = _orderedSession;
      // A same-root older State is already covered by the accepted common
      // revision. Explicit gaps and rejected edits retain their repair owners.
      if (session == null || !_canGuardStorePaint(session)) {
        _schedulePhysicalStoreRepair();
      }
    }
  }

  void _acceptProjectedPhysicalStoreAuthority() {
    final authority = _storeAuthority;
    final common = _storeCommonAuthority;
    if (_projectedPhysicalStoreLiveRevision == 0 ||
        authority == null ||
        common == null ||
        documentSession.boardDocument == null ||
        _unconfirmedDocument != null ||
        _physicalStoreRepairThroughRevision != 0 ||
        _physicalStoreRepairing ||
        authority.deviceId != common.deviceId ||
        authority.bootNonce != common.bootNonce ||
        authority.generation != common.generation ||
        authority.revision != _projectedPhysicalStoreLiveRevision ||
        authority.revision < common.revision) {
      return;
    }
    // The reliable batch names the exact live revision after its last mark.
    // An authority State at that revision proves no unseen Store mutation is
    // being attributed to the projected pixels.
    _storeCommonAuthority = authority;
    _frameStoreGeneration = authority.generation;
  }

  ({int frame, int mask, int action, BigInt effectEdits, bool preview})?
  _pendingCanvasMergeFootprint(int liveRevision) {
    final canvas = _pendingStoreCanvas;
    if (canvas == null ||
        _disposed ||
        !_boardActive ||
        canvas.generation != _generation ||
        canvas.sourceGeneration != documentSession.replacementGeneration ||
        canvas.modeSession != _documentSessionId ||
        !identical(canvas.session, _orderedSession) ||
        (canvas.historyRevision != 0 &&
            canvas.historyRevision != liveRevision)) {
      return null;
    }
    return (
      frame: canvas.frame,
      mask: canvas.mask,
      action: canvas.action,
      effectEdits: canvas.effectEdits,
      preview: !canvas.completed.isCompleted && canvas.followingHistory == null,
    );
  }

  bool _canGuardStorePaint(ConnectionSession session) {
    final authority = _storeAuthority;
    final common = _storeCommonAuthority;
    return !storeReplacementUnresolved &&
        _physicalStoreRepairThroughRevision == 0 &&
        session.storeGuardEpochSupported &&
        authority != null &&
        common != null &&
        common.deviceId == _deviceId &&
        authority.deviceId == common.deviceId &&
        authority.bootNonce == session.boardBootNonce &&
        common.bootNonce == session.boardBootNonce &&
        common.revision != 0 &&
        authority.revision != 0 &&
        authority.revision <= common.revision &&
        authority.generation == common.generation &&
        _frameStoreGeneration == common.generation;
  }

  void _readStoreAuthorityState(ConnectionSessionPacket packet) {
    final session = _orderedSession;
    final deviceId = _deviceId;
    if (session == null ||
        deviceId == null ||
        packet.body.length != 10 ||
        packet.body[0] != 1 ||
        (packet.body[1] & ~1) != 0 ||
        packet.sequence <= _storeAuthorityStateSequence) {
      return;
    }
    final preview = _pendingFrameStoreConflict;
    final previewWasValid =
        preview != null && _verifiedConflictPreview(preview);
    _storeAuthorityStateSequence = packet.sequence;
    final bytes = ByteData.sublistView(packet.body);
    _storeAuthority = (
      deviceId: deviceId,
      bootNonce: session.boardBootNonce,
      generation: bytes.getUint32(2, Endian.little),
      revision: bytes.getUint32(6, Endian.little),
      dirty: (packet.body[1] & 1) != 0,
    );
    _bindStoreCommonAuthorityIfReady();
    _acceptProjectedPhysicalStoreAuthority();
    _reconcileStoreCommonAuthority();
    final pendingEntry = _pendingStoreEntryInspection;
    if (pendingEntry != null &&
        !_storeAuthority!.dirty &&
        !_resumingStoreEntry) {
      _resumingStoreEntry = true;
      scheduleMicrotask(() async {
        try {
          if (!_disposed && identical(session, _orderedSession)) {
            _pendingStoreEntryInspection = null;
            await _synchronizeFrameStoreEntry(pendingEntry);
          }
        } finally {
          _resumingStoreEntry = false;
        }
      });
    }
    if (_physicalStoreRepairThroughRevision != 0 ||
        (_storePaintWaitingForAuthority && !_canGuardStorePaint(session))) {
      _schedulePhysicalStoreRepair();
    }
    _resumeStorePaintIfReady(session);
    if (preview != null &&
        previewWasValid != _verifiedConflictPreview(preview)) {
      _notify();
    }
  }

  void _resumeStorePaintIfReady(ConnectionSession session) {
    final canvas = _pendingStoreCanvas;
    if (canvas != null &&
        !canvas.completed.isCompleted &&
        identical(canvas.session, session) &&
        _status == LitchcraftStatus.live &&
        _storePaintWaitingForAuthority &&
        _canGuardStorePaint(session)) {
      // This Canvas already acquired its predecessor's position. A later
      // incomplete edit waits for it and cannot block its authority retry.
      unawaited(canvas.resume());
      return;
    }
    final edit = _storeEditPosition;
    if (edit != null &&
        !edit.completion.isCompleted &&
        _storePositionCurrent(edit)) {
      return;
    }
    final structure = _storeStructuralEditPosition;
    if (structure != null && !structure.completion.isCompleted) return;
    if (!_storePaintWaitingForAuthority || !_canGuardStorePaint(session)) {
      return;
    }
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    scheduleMicrotask(() {
      if (_disposed ||
          !identical(session, _orderedSession) ||
          generation != _generation ||
          sourceGeneration != documentSession.replacementGeneration ||
          !_storePaintWaitingForAuthority ||
          _status != LitchcraftStatus.live ||
          !_canGuardStorePaint(session)) {
        return;
      }
      _storePaintWaitingForAuthority = false;
      final pendingEnd = _pendingStoreStrokeEnd;
      if (pendingEnd != null) {
        if (!identical(pendingEnd.session, session) ||
            pendingEnd.generation != _generation ||
            pendingEnd.sourceGeneration !=
                documentSession.replacementGeneration) {
          _pendingStoreStrokeEnd = null;
        } else {
          if (_storeStrokeEndInFlight) return;
          _storeStrokeEndInFlight = true;
          unawaited(
            _enqueue(() async {
              // Firmware refuses Select while this accepted App stroke is
              // open. Its saved frame is still displayed; close it first.
              await _sendOrderedEmitterBatch(
                frame: pendingEnd.frame,
                stroke: pendingEnd.stroke,
                begin: false,
                selectBeforeBegin: false,
                end: true,
                mask: 15,
                color: 0,
                clearEffects: false,
                squares: const <int>[],
              );
              if (identical(_pendingStoreStrokeEnd, pendingEnd)) {
                _pendingStoreStrokeEnd = null;
              }
              final selected = documentSession.selectedFrameIndex;
              if (selected != pendingEnd.frame) {
                await _selectStoreFrameForEdit(selected);
              }
            }, recoverBusy: true).then((_) {
              _storeStrokeEndInFlight = false;
              if (_disposed ||
                  !identical(session, _orderedSession) ||
                  _pendingStoreStrokeEnd != null ||
                  _status != LitchcraftStatus.live) {
                return;
              }
              final canvas = _pendingStoreCanvas;
              if (canvas != null) {
                unawaited(canvas.resume());
                return;
              }
              if (_stroke) {
                _flushStroke();
              } else if (documentSession.boardDocument != null &&
                  !sameLitchcraftDocument(
                    documentSession.document,
                    documentSession.boardDocument!,
                  )) {
                unawaited(_enqueue(_sendDocumentDelta, recoverBusy: true));
              } else {
                _notify();
              }
            }),
          );
          return;
        }
      }
      final canvas = _pendingStoreCanvas;
      if (canvas != null) {
        unawaited(canvas.resume());
        return;
      }
      if (_stroke) {
        _flushStroke();
      } else if (documentSession.boardDocument != null &&
          !sameLitchcraftDocument(
            documentSession.document,
            documentSession.boardDocument!,
          )) {
        unawaited(_enqueue(_sendDocumentDelta, recoverBusy: true));
      } else {
        _notify();
      }
    });
  }

  void _readConnectionState(ConnectionSessionPacket packet) {
    if (packet.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      _readStoreAuthorityState(packet);
      return;
    }
    if (packet.code == ConnectionSession.litchcraftStoreEditStateCode) {
      _readPhysicalStoreEdit(packet);
      return;
    }
    if (packet.code != ConnectionSession.litchcraftStateCode ||
        packet.body.length != 10 ||
        packet.sequence < _connectionStateRevision) {
      return;
    }
    final flags = packet.body[4];
    final target = packet.body[5];
    if (target == 0 || target > 15) return;
    if (packet.sequence > _connectionStateRevision) {
      _connectionStateRevision = packet.sequence;
      _connectionStateObservedAt = _advancedTimelineNow;
    }
    ++_brushObservation;
    _brushLinkToken = _linkToken;
    _brushSession = _sessionId;
    _confirmedBrush = (
      color: (packet.body[6] << 16) | (packet.body[7] << 8) | packet.body[8],
      eraser: (flags & 4) != 0,
      targetMask: target,
      project: true,
    );
    final canUndo = (packet.body[9] & 1) != 0;
    final canRedo = (packet.body[9] & 2) != 0;
    // A reconnect can deliver retained tool State before Store Inspect names
    // the artwork owner. Keep its availability for that later admission.
    _advancedCanUndo = canUndo;
    _advancedCanRedo = canRedo;
    if (!_advancedWire && !usesFrameStoreDocument) {
      _liveUndo = canUndo;
      _liveRedo = canRedo;
    }
    // Inactive runtime timing cannot select or start the detached draft.
    // Mode Inspect/admission owns that authority; tool readback above remains
    // independent of artwork and playback projection.
    if (_boardActive &&
        (flags & 1) != 0 &&
        (_advancedWire || usesFrameStoreDocument)) {
      _projectTransferEffectClock((flags & 16) != 0);
      final frameCount = packet.body[1] == 0 ? 256 : packet.body[1];
      final selectedFrame = packet.body[0];
      final conflict = _pendingFrameStoreConflict;
      if (conflict != null &&
          _currentConflictPreview(conflict) &&
          frameCount ==
              (conflict.previewSnapshot?.document.frames.length ??
                  conflict.previewFrame?.frameCount ??
                  conflict.observedFrameCount) &&
          selectedFrame < frameCount) {
        // Runtime authority for the Board preview never selects the draft.
        _advancedSelectedFrame = selectedFrame;
        _frameStorePlaying = (flags & 2) != 0;
      }
      if (frameCount == documentSession.document.frames.length &&
          selectedFrame < frameCount) {
        final frameStart = litchcraftAnimationFrameStartOrNull(
          documentSession.document,
          selectedFrame,
        );
        if (frameStart != null &&
            packet.sequence > _connectionPlaybackStateRevision) {
          final consumed = _receiveAdvancedTimeline(
            LitchcraftV2TimelineSample(
              session: _documentSessionId,
              revision: _advancedWire
                  ? _documentRevision
                  : _frameStoreGeneration,
              sampleId: packet.sequence,
              playing: (flags & 2) != 0,
              transferSuspended: (flags & 16) != 0,
              selectedFrame: selectedFrame,
              elapsedMilliseconds:
                  frameStart +
                  ByteData.sublistView(packet.body).getUint16(2, Endian.little),
              receivedAtMonotonicMilliseconds: _connectionStateObservedAt,
            ),
          );
          if (consumed) _connectionPlaybackStateRevision = packet.sequence;
        }
      }
    }
    _waitingForLift = (flags & 8) != 0;
    _projectConfirmedBrush();
    _notify();
  }

  void _retainStoreFrame(LitchcraftAnimationFrame frame) {
    final signature = LitchcraftFrameStoreFrameSignature.forFrame(frame);
    if (_retainedStoreFrames.any(
      (known) =>
          LitchcraftFrameStoreFrameSignature.forFrame(known) == signature,
    )) {
      return;
    }
    _retainedStoreFrames.add(frame);
    if (_retainedStoreFrames.length > 64) _retainedStoreFrames.removeAt(0);
  }

  void _retainPhysicalStorePreimage(int index) {
    final frames = documentSession.boardDocument?.frames;
    if (frames != null && index >= 0 && index < frames.length) {
      _retainStoreFrame(frames[index]);
    }
  }

  Iterable<LitchcraftAnimationFrame> _knownLiveStoreFrames() sync* {
    yield* documentSession.document.frames;
    final board = documentSession.boardDocument;
    if (board != null) yield* board.frames;
    final undo = documentSession.nextUndoDocument;
    if (undo != null) yield* undo.frames;
    final redo = documentSession.nextRedoDocument;
    if (redo != null) yield* redo.frames;
    yield* _retainedStoreFrames;
    for (final resident in _knownFrameStoreResidents) {
      yield* resident.document.frames;
    }
  }

  // A structural inverse can publish its root on either side of the Ordered
  // receipt. Match only this exact edit position; a reverse record never adopts
  // a new generation or establishes a different layout by itself.
  bool _matchesStoreStructuralGeneration(int generation) {
    final echo = _storeHistoryEcho;
    final common = _storeCommonAuthority;
    final board = documentSession.boardDocument;
    final position = _storeEditPosition;
    if (echo == null ||
        echo.sourceGeneration != documentSession.replacementGeneration ||
        generation <= echo.generation ||
        common == null ||
        board == null ||
        position == null ||
        !_storePositionCurrent(position) ||
        common.deviceId != position.deviceId ||
        common.bootNonce != position.bootNonce) {
      return false;
    }
    if (echo.confirmed) {
      final result = position.result;
      return position.completion.isCompleted &&
          result != null &&
          _canGuardStorePaint(position.session) &&
          common.revision == echo.revision &&
          (common.generation == echo.generation ||
              common.generation == generation) &&
          result.authority.deviceId == common.deviceId &&
          result.authority.bootNonce == common.bootNonce &&
          result.authority.revision == echo.revision &&
          (result.authority.generation == echo.generation ||
              result.authority.generation == common.generation) &&
          result.before.frames.length != result.document.frames.length &&
          sameLitchcraftDocument(result.document, echo.document) &&
          sameLitchcraftDocument(board, echo.document);
    }
    return ((common.generation == echo.generation &&
                common.revision == ((echo.revision - 1) & 0xffffffff)) ||
            (common.generation == generation &&
                common.revision == echo.revision &&
                _storeAuthority?.generation == generation &&
                _storeAuthority?.revision == echo.revision &&
                _storeAuthority?.dirty == false)) &&
        board.frames.length != echo.document.frames.length;
  }

  void _readPhysicalStoreEdit(ConnectionSessionPacket packet) {
    if (_pendingFrameStoreConflict != null) return;
    if (packet.body.length != 10 ||
        packet.sequence <= _physicalStoreEditRevision) {
      return;
    }
    final previousRevision = _physicalStoreEditRevision;
    _physicalStoreEditRevision = packet.sequence;
    final generation = ByteData.sublistView(
      packet.body,
    ).getUint32(0, Endian.little);
    final frame = packet.body[4];
    final square = packet.body[5];
    final flags = packet.body[6];
    final mask = flags & 0x0f;
    final clearEffects = (flags & 0x10) != 0;
    final missedRevision =
        previousRevision != 0 && packet.sequence != previousRevision + 1;
    if (!usesFrameStoreDocument ||
        (_frameStoreGeneration == 0 && !_acceptedStoreCommitAwaitingRoot) ||
        generation < _frameStoreGeneration ||
        frame >= documentSession.document.frames.length ||
        square >= 64 ||
        mask == 0 ||
        (flags & 0xe0) != 0) {
      if (usesFrameStoreDocument &&
          (missedRevision || frame >= documentSession.document.frames.length)) {
        _schedulePhysicalStoreRepair();
      }
      return;
    }
    final color = packet.body[7] << 16 | packet.body[8] << 8 | packet.body[9];
    final echo = _storeHistoryEcho;
    final common = _storeCommonAuthority;
    if (echo != null &&
        echo.sourceGeneration == documentSession.replacementGeneration &&
        (generation == echo.generation ||
            _matchesStoreStructuralGeneration(generation)) &&
        common != null &&
        common.generation == echo.generation &&
        common.revision ==
            (echo.confirmed
                ? echo.revision
                : (echo.revision - 1) & 0xffffffff) &&
        _matchesStoreHistoryPaint(
          echo.document,
          frame: frame,
          squares: <int>[square],
          mask: mask,
          color: color,
          clearEffects: clearEffects,
        )) {
      // This compatibility hint has no live revision. The retained exact
      // inverse owns it even when a later local history preview is visible;
      // the reliable batch still validates the live revision or a foreign edit.
      return;
    }
    final deviceId = _deviceId;
    if (deviceId == null) return;
    if ((storeReplacementUnresolved ||
            _storeReplacementAwaitingAdmission ||
            _acceptedStoreCommitAwaitingRoot) &&
        generation <= _frameStoreGeneration) {
      _observePreCommitBoardPaint(
        deviceId: deviceId,
        frame: frame,
        squares: <int>[square],
        mask: mask,
        color: color,
        clearEffects: clearEffects,
      );
      return;
    }
    if (generation > _frameStoreGeneration) {
      _frameStoreGeneration = generation;
    }
    if (_pendingStoreCanvas == null &&
        documentSession.nextUndoDocument != null &&
        documentSession.hasLocalIntent &&
        ((_unconfirmedPaintDocumentBase != null &&
                _matchesStoreHistoryPaint(
                  documentSession.nextUndoDocument!,
                  frame: frame,
                  squares: <int>[square],
                  mask: mask,
                  color: color,
                  clearEffects: clearEffects,
                )) ||
            _physicalStoreRepairThroughRevision != 0)) {
      // This compatibility hint has no live revision or whole inverse. Keep
      // the pending gesture intact for the reliable batch/readback owner.
      ++_paintQueueGeneration;
      _storePaintWaitingForAuthority = true;
      _schedulePhysicalStoreRepair(throughRevision: packet.sequence);
      return;
    }
    _storeHistoryEcho = null;
    _retainPhysicalStorePreimage(frame);
    documentSession.retireLocalHistory();
    _pauseStoreTimelineAtPhysicalFrame(frame);
    _forgetSelectedFrameStoreResident();
    _projectingDocument = true;
    try {
      if (!documentSession.applyBoardPaintBatch(
        boardId: deviceId,
        frame: frame,
        orderedCanvas: _pendingCanvasMergeFootprint(
          _storeAuthority?.revision ?? 0,
        ),
        edits: <LitchcraftPhysicalPaint>[
          (
            squares: <int>[square],
            mask: mask,
            color: color,
            clearEffects: clearEffects,
          ),
        ],
      )) {
        _schedulePhysicalStoreRepair();
      }
      _selectStoreBoardFrame(frame);
      _detached = _canvas;
      _offlineDiverged = false;
    } finally {
      _projectingDocument = false;
    }
    if (missedRevision) {
      _schedulePhysicalStoreRepair();
    } else {
      _reconcileStoreCommonAuthority();
    }
    _notify();
  }

  void _observePreCommitBoardPaint({
    required String deviceId,
    required int frame,
    required Iterable<int> squares,
    required int mask,
    required int color,
    required bool clearEffects,
  }) {
    final board = documentSession.boardDocument;
    if (board == null || frame >= board.frames.length) {
      _schedulePhysicalStoreRepair(throughRevision: _physicalStoreEditRevision);
      return;
    }
    final painted = patchLitchcraftEmitters(
      board,
      frame: frame,
      squares: squares,
      mask: mask,
      color: color,
      effect: clearEffects ? LitchcraftEffectStyle.off : null,
      effectDurationMs: 0,
    );
    final resolution = documentSession.observeBoard(deviceId, painted);
    if (resolution == LitchcraftDocumentResolution.conflict) {
      _status = LitchcraftStatus.conflict;
    }
    _frameStorePlaying = false;
    _forgetSelectedFrameStoreResident();
    _board = LitchcraftCanvas(
      colors: painted.frames[frame].emitters.map((emitter) => emitter.color),
      topology: painted.topology,
    );
    _notify();
  }

  bool _matchesStoreHistoryPaint(
    LitchcraftAnimationDocument document, {
    required int frame,
    required Iterable<int> squares,
    required int mask,
    required int color,
    required bool clearEffects,
  }) {
    if (frame >= document.frames.length) return false;
    for (final square in squares) {
      for (var region = 0; region < 4; region++) {
        if (mask & (1 << region) == 0) continue;
        final emitter = document.frames[frame].emitters[square * 4 + region];
        if (emitter.color != color ||
            (clearEffects &&
                (emitter.effect != LitchcraftEffectStyle.off ||
                    emitter.effectDurationMs != 0))) {
          return false;
        }
      }
    }
    return true;
  }

  bool _isConfirmedStoreHistoryBase(LitchcraftAnimationDocument document) {
    final echo = _storeHistoryEcho;
    final common = _storeCommonAuthority;
    final board = documentSession.boardDocument;
    final ordered = _orderedSession;
    final authority = _storeAuthority;
    if (echo == null ||
        echo.sourceGeneration != documentSession.replacementGeneration ||
        ordered == null ||
        !ordered.isActivated ||
        common == null ||
        authority == null ||
        common.deviceId != _deviceId ||
        authority.deviceId != common.deviceId ||
        common.bootNonce != ordered.boardBootNonce ||
        authority.bootNonce != common.bootNonce ||
        authority.generation != common.generation ||
        authority.revision > echo.revision ||
        echo.generation != common.generation ||
        _frameStoreGeneration != common.generation ||
        storeReplacementUnresolved ||
        board == null) {
      return false;
    }
    final LitchcraftAnimationDocument confirmed;
    if (echo.confirmed) {
      if (echo.revision != common.revision) return false;
      confirmed = echo.document;
    } else {
      final previous = echo.previous;
      if (previous == null ||
          previous.generation != echo.generation ||
          previous.revision != common.revision ||
          echo.revision != ((previous.revision + 1) & 0xffffffff)) {
        return false;
      }
      confirmed = previous.document;
    }
    return sameLitchcraftDocument(confirmed, board) &&
        sameLitchcraftDocument(confirmed, document);
  }

  bool _matchesPendingStrokeInverse(LitchcraftV2Packet packet) {
    final before = documentSession.nextUndoDocument;
    if (before == null) return false;
    final data = ByteData.sublistView(Uint8List.fromList(packet.payload));
    for (var index = 0; index < packet.payload[1]; index++) {
      final offset = 2 + index * 21;
      final flags = packet.payload[offset + 5];
      final footprint = data.getUint64(offset + 9, Endian.little);
      if (!_matchesStoreHistoryPaint(
        before,
        frame: packet.payload[offset + 4],
        squares: <int>[
          for (var square = 0; square < 64; square++)
            if (footprint & (1 << square) != 0) square,
        ],
        mask: flags & 15,
        color:
            packet.payload[offset + 6] << 16 |
            packet.payload[offset + 7] << 8 |
            packet.payload[offset + 8],
        clearEffects: flags & 16 != 0,
      )) {
        return false;
      }
    }
    return true;
  }

  void _readPhysicalStoreBatch(LitchcraftV2Packet packet) {
    if (_pendingFrameStoreConflict != null ||
        !usesFrameStoreDocument ||
        packet.session != _documentSessionId ||
        packet.revision <= _physicalStoreBatchRevision) {
      return;
    }
    final count = packet.payload[1];
    // Undo may remove only the accepted prefix of a released pointer gesture.
    // Fence its queued tail and retain the existing preimage until readback.
    final interruptedPaint =
        count != 0 &&
        _pendingStoreCanvas == null &&
        documentSession.nextUndoDocument != null &&
        documentSession.hasLocalIntent &&
        ((_unconfirmedPaintDocumentBase != null &&
                _matchesPendingStrokeInverse(packet)) ||
            _physicalStoreRepairThroughRevision != 0);
    if (interruptedPaint) {
      ++_paintQueueGeneration;
      _storePaintWaitingForAuthority = true;
    }
    final firstRevision = (packet.revision - count + 1) & 0xffffffff;
    var needsSnapshot =
        interruptedPaint ||
        packet.payload[0] & 1 != 0 ||
        (_physicalStoreBatchRevision != 0 &&
            firstRevision != (_physicalStoreBatchRevision + 1) & 0xffffffff);
    _physicalStoreBatchRevision = packet.revision;
    if (packet.revision > _physicalStoreEditRevision) {
      _physicalStoreEditRevision = packet.revision;
    }
    if (_frameStoreGeneration == 0 &&
        !_acceptedStoreCommitAwaitingRoot &&
        (storeReplacementUnresolved || _storeReplacementAwaitingAdmission)) {
      // Reconnect has not proved which board document these pixels belong to.
      // Keep the selected replacement intact; mandatory entry reconciliation
      // owns the board preimage before any pre-Commit physical projection.
      return;
    }
    final deviceId = _deviceId;
    if (deviceId == null) return;
    final confirmedEcho = _storeHistoryEcho;
    final confirmedDocument = documentSession.boardDocument;
    var confirmedHistoryOnly =
        count != 0 &&
        confirmedEcho != null &&
        confirmedDocument != null &&
        _physicalStoreRepairThroughRevision == 0 &&
        (_isConfirmedStoreHistoryBase(confirmedDocument) ||
            (confirmedEcho.confirmed &&
                _matchesStoreStructuralGeneration(_frameStoreGeneration)));
    var pendingHistoryOnly =
        count != 0 &&
        confirmedEcho != null &&
        !confirmedEcho.confirmed &&
        confirmedEcho.sourceGeneration ==
            documentSession.replacementGeneration &&
        (_physicalStoreRepairThroughRevision == 0 ||
            _projectedPhysicalStoreLiveRevision == confirmedEcho.revision);
    final unknownWait = _storeHistoryRepairWait;
    var completeUnknownResult =
        count != 0 &&
        !needsSnapshot &&
        _physicalStoreRepairThroughRevision == 0 &&
        unknownWait != null &&
        unknownWait.revision != 0 &&
        _canBorrowStoreHistoryRepair(unknownWait.session) &&
        _storeCommonAuthority?.generation == unknownWait.storeGeneration &&
        _storeCommonAuthority?.revision ==
            ((unknownWait.revision - 1) & 0xffffffff);
    var unknownResultSeen = false;
    if (count != 0) _forgetSelectedFrameStoreResident();
    final data = ByteData.sublistView(Uint8List.fromList(packet.payload));
    int? lastFrame;
    var lastFrameWasPreCommit = false;
    int? pendingFrame;
    var pendingHistoryPreview = false;
    ({int frame, int mask, int action, BigInt effectEdits, bool preview})?
    pendingCanvasFootprint;
    final pendingEdits = <LitchcraftPhysicalPaint>[];
    void flushProjectedEdits() {
      final frame = pendingFrame;
      if (frame != null &&
          pendingEdits.isNotEmpty &&
          !documentSession.applyBoardPaintBatch(
            boardId: deviceId,
            frame: frame,
            edits: pendingEdits,
            historyPreview: pendingHistoryPreview,
            preservePendingStroke: interruptedPaint,
            orderedCanvas: pendingCanvasFootprint,
          )) {
        needsSnapshot = true;
        confirmedHistoryOnly = false;
        pendingHistoryOnly = false;
        completeUnknownResult = false;
      }
      pendingFrame = null;
      pendingHistoryPreview = false;
      pendingCanvasFootprint = null;
      pendingEdits.clear();
    }

    _projectingDocument = true;
    try {
      for (var index = 0; index < count; index++) {
        final offset = 2 + index * 21;
        final generation = data.getUint32(offset, Endian.little);
        final frame = packet.payload[offset + 4];
        final flags = packet.payload[offset + 5];
        final mask = flags & 0x0f;
        final clearEffects = flags & 0x10 != 0;
        final color =
            packet.payload[offset + 6] << 16 |
            packet.payload[offset + 7] << 8 |
            packet.payload[offset + 8];
        final footprint = data.getUint64(offset + 9, Endian.little);
        final liveRevision = data.getUint32(offset + 17, Endian.little);
        final squares = <int>[
          for (var square = 0; square < 64; square++)
            if (footprint & (1 << square) != 0) square,
        ];
        final echo =
            _storeHistoryEcho?.sourceGeneration ==
                documentSession.replacementGeneration
            ? _storeHistoryEcho
            : null;
        final structuralGeneration =
            echo != null &&
            (_matchesStoreStructuralGeneration(generation) ||
                (echo.confirmed &&
                    generation == echo.generation &&
                    _matchesStoreStructuralGeneration(_frameStoreGeneration)));
        final historyPreview =
            echo != null &&
            (generation == echo.generation || structuralGeneration) &&
            liveRevision == echo.revision &&
            _matchesStoreHistoryPaint(
              echo.document,
              frame: frame,
              squares: squares,
              mask: mask,
              color: color,
              clearEffects: clearEffects,
            );
        final previous = echo?.previous;
        final previousHistory =
            previous != null &&
            generation == previous.generation &&
            liveRevision == previous.revision &&
            _matchesStoreHistoryPaint(
              previous.document,
              frame: frame,
              squares: squares,
              mask: mask,
              color: color,
              clearEffects: clearEffects,
            );
        confirmedHistoryOnly =
            confirmedHistoryOnly &&
            echo == confirmedEcho &&
            (generation == _frameStoreGeneration ||
                (echo?.confirmed == true && structuralGeneration)) &&
            mask != 0 &&
            footprint != 0 &&
            ((historyPreview && echo.confirmed) || previousHistory);
        pendingHistoryOnly =
            pendingHistoryOnly &&
            echo == confirmedEcho &&
            generation == _frameStoreGeneration &&
            mask != 0 &&
            footprint != 0 &&
            historyPreview;
        completeUnknownResult =
            completeUnknownResult &&
            generation == unknownWait!.storeGeneration &&
            generation == _frameStoreGeneration &&
            liveRevision >= unknownWait.revision &&
            mask != 0 &&
            footprint != 0;
        if (completeUnknownResult &&
            frame == unknownWait!.frame &&
            liveRevision == unknownWait.revision) {
          unknownResultSeen = true;
        }
        if (generation < _frameStoreGeneration) {
          continue;
        }
        if (historyPreview && echo.confirmed && structuralGeneration) {
          // The completed guarded edit already confirmed this entire layout.
          // Leave generation adoption to State; these pixels are redundant.
          continue;
        }
        if (historyPreview &&
            echo.confirmed &&
            _storeCommonAuthority?.generation == echo.generation &&
            (_storeCommonAuthority?.revision ?? 0) > echo.revision) {
          // A later ordinary app edit can advance common while the confirmed
          // history tail is still draining. Its exact old result is redundant.
          continue;
        }
        if (!historyPreview && previousHistory) {
          // This predecessor's guarded terminal receipt already confirmed the
          // complete inverse before the newer known history preview began.
          continue;
        }
        if (frame >= documentSession.document.frames.length ||
            mask == 0 ||
            footprint == 0) {
          flushProjectedEdits();
          needsSnapshot = true;
          continue;
        }
        if ((storeReplacementUnresolved ||
                _storeReplacementAwaitingAdmission ||
                _acceptedStoreCommitAwaitingRoot) &&
            generation <= _frameStoreGeneration) {
          flushProjectedEdits();
          _observePreCommitBoardPaint(
            deviceId: deviceId,
            frame: frame,
            squares: squares,
            mask: mask,
            color: color,
            clearEffects: clearEffects,
          );
          if (!historyPreview) {
            lastFrame = frame;
            lastFrameWasPreCommit = true;
          }
          continue;
        }
        if (generation > _frameStoreGeneration) {
          flushProjectedEdits();
          _frameStoreGeneration = generation;
        }
        if (!historyPreview) {
          _storeHistoryEcho = null;
          _retainPhysicalStorePreimage(frame);
          if (!interruptedPaint) documentSession.retireLocalHistory();
        }
        final canvasFootprint = _pendingCanvasMergeFootprint(liveRevision);
        if (pendingFrame != null &&
            (pendingFrame != frame ||
                pendingHistoryPreview != historyPreview ||
                pendingCanvasFootprint != canvasFootprint)) {
          flushProjectedEdits();
        }
        pendingFrame = frame;
        pendingHistoryPreview = historyPreview;
        pendingCanvasFootprint = canvasFootprint;
        pendingEdits.add((
          squares: squares,
          mask: mask,
          color: color,
          clearEffects: clearEffects,
        ));
        _projectedPhysicalStoreLiveRevision = liveRevision;
        // An exact retained history echo confirms pixels. It is not a new
        // physical action that can pause or reselect a later local timeline.
        if (!historyPreview) {
          lastFrame = frame;
          lastFrameWasPreCommit = false;
        }
      }
      flushProjectedEdits();
      if (!lastFrameWasPreCommit &&
          lastFrame != null &&
          lastFrame < documentSession.document.frames.length) {
        _pauseStoreTimelineAtPhysicalFrame(lastFrame);
        if (!storeReplacementUnresolved || _acceptedStoreCommitAwaitingRoot) {
          _selectStoreBoardFrame(lastFrame);
        } else {
          documentSession.selectFrame(lastFrame);
        }
      }
      if (!lastFrameWasPreCommit &&
          (!storeReplacementUnresolved || _acceptedStoreCommitAwaitingRoot)) {
        if (lastFrame == null) {
          _showStoreFrame(documentSession.selectedFrameIndex);
        }
        _detached = _canvas;
        _offlineDiverged = false;
      }
    } finally {
      _projectingDocument = false;
    }
    if (completeUnknownResult &&
        unknownResultSeen &&
        !needsSnapshot &&
        unknownWait != null &&
        identical(
          _storeHistoryRepairWait?.recordProjection,
          unknownWait.recordProjection,
        ) &&
        _canBorrowStoreHistoryRepair(unknownWait.session)) {
      // Complete contiguous RGB history proves this admitted action's result,
      // even if a newer State or an already-running repair owns latest common.
      // Overflow and its tail cannot establish this intermediate proof.
      unknownWait.recordProjection();
    }
    if (confirmedHistoryOnly &&
        (_isConfirmedStoreHistoryBase(confirmedDocument!) ||
            (confirmedEcho?.confirmed == true &&
                _matchesStoreStructuralGeneration(_frameStoreGeneration)))) {
      // The guarded receipt already proved the entire inverse at this exact
      // live revision. Its truncated or delayed reverse echo cannot make that
      // confirmed base incomplete. Foreign/mixed records retain normal repair.
      if (needsSnapshot) {
        _appTrace?.record(
          'litchcraft.store_history_batch_confirmed',
          fields: <String, Object?>{
            'batch_revision': packet.revision,
            'generation': confirmedEcho!.generation,
            'base_revision': _storeCommonAuthority?.revision,
            'echo_revision': confirmedEcho.revision,
            'records': count,
          },
        );
      }
      needsSnapshot = false;
    }
    if (needsSnapshot) {
      _appTrace?.record(
        'litchcraft.store_history_batch_repair',
        fields: <String, Object?>{
          'batch_revision': packet.revision,
          'overflow': packet.payload[0] & 1 != 0,
          'records': count,
          'projected_live_revision': _projectedPhysicalStoreLiveRevision,
          'echo_revision': _storeHistoryEcho?.revision,
          'echo_confirmed': _storeHistoryEcho?.confirmed,
          'repair_through': _physicalStoreRepairThroughRevision,
        },
      );
      _schedulePhysicalStoreRepair(throughRevision: packet.revision);
    }
    if (!needsSnapshot && !lastFrameWasPreCommit) {
      _acceptProjectedPhysicalStoreAuthority();
      _reconcileStoreCommonAuthority();
      final session = _orderedSession;
      if (session != null) _resumeStorePaintIfReady(session);
    } else if (!pendingHistoryOnly) {
      // An exact pending inverse can still receive its terminal receipt between
      // overflow and tail Objects. Keep this existing revision proof so that
      // receipt can confirm the whole result; it does not admit edits early.
      _projectedPhysicalStoreLiveRevision = 0;
    }
    _notify();
  }

  void _schedulePhysicalStoreRepair({int throughRevision = 0}) {
    // Store edit repair belongs to the selected, active mode. The healthy
    // shared session survives Exit and can still deliver retired callbacks.
    if (_disposed ||
        !_selected ||
        !_boardActive ||
        _exiting != null ||
        _status == LitchcraftStatus.error) {
      return;
    }
    if (throughRevision > _physicalStoreRepairThroughRevision) {
      _physicalStoreRepairThroughRevision = throughRevision;
    }
    if (_physicalStoreRepairing) return;
    final session = _orderedSession;
    if (session != null && session.isActivated) {
      _physicalStoreRepairBinding = (
        generation: _generation,
        sourceGeneration: documentSession.replacementGeneration,
        modeSession: _documentSessionId,
        linkToken: _linkToken,
        bootNonce: session.boardBootNonce,
        deviceId: _deviceId,
        session: session,
      );
    }
    if (throughRevision != 0) {
      // An explicit overflow already proves missing content; the transport's
      // loss-probe interval is not a prerequisite for reading this live result.
      _physicalStoreRepairTimer?.cancel();
      _physicalStoreRepairTimer = null;
    }
    if (_physicalStoreRepairTimer != null) return;
    final delay = throughRevision != 0
        ? Duration.zero
        : _orderedSession?.recoveryProbeDelay ??
              const Duration(milliseconds: 121);
    _physicalStoreRepairTimer = Timer(delay, () {
      _physicalStoreRepairTimer = null;
      final authority = _storeAuthority, common = _storeCommonAuthority;
      if (_physicalStoreRepairThroughRevision == 0 &&
          authority != null &&
          common != null &&
          authority.generation == common.generation &&
          authority.revision == common.revision) {
        return;
      }
      unawaited(_repairPhysicalStoreEdits());
    });
  }

  bool _acceptedPendingStrokeMatches(
    LitchcraftFrameStoreSnapshot snapshot,
    ConnectionSession session,
  ) {
    final accepted = _acceptedStoreStroke;
    return accepted != null &&
        accepted.stroke == _strokeId &&
        accepted.frame == snapshot.selectedFrame &&
        accepted.generation == _generation &&
        accepted.sourceGeneration == documentSession.replacementGeneration &&
        accepted.modeSession == _documentSessionId &&
        accepted.linkToken == _linkToken &&
        accepted.deviceId == _deviceId &&
        identical(accepted.session, session) &&
        accepted.bootNonce == session.boardBootNonce &&
        accepted.paintGeneration == _strokePaintGeneration &&
        snapshot.liveRevision > accepted.revision;
  }

  Future<void> _repairPhysicalStoreEdits() async {
    final session = _orderedSession;
    final deviceId = _deviceId;
    if (_disposed ||
        !_selected ||
        !_boardActive ||
        _status == LitchcraftStatus.error ||
        _exiting != null ||
        _pendingFrameStoreConflict != null ||
        storeReplacementUnresolved ||
        _physicalStoreRepairing ||
        session == null ||
        deviceId == null ||
        !session.isActivated ||
        !usesFrameStoreDocument) {
      return;
    }
    _physicalStoreRepairing = true;
    _notify();
    var retry = false;
    var needsCatchUp = false;
    final controllerGeneration = _generation;
    final linkToken = _linkToken;
    void defer(Object error) {
      if (_disposed ||
          !identical(session, _orderedSession) ||
          controllerGeneration != _generation) {
        return;
      }
      // A valid snapshot can race newer accepted authority. Retry convergence
      // without treating ordinary artwork churn as a failed channel.
      retry = true;
      _appTrace?.record(
        'litchcraft.physical_store_repair_deferred',
        fields: <String, Object?>{
          'generation': controllerGeneration,
          'kind': error.runtimeType.toString(),
        },
      );
    }

    final repaired = Completer<void>();
    var claimed = false;
    var queued = false;
    Future<void> repair() async {
      // A waiting history action owns its existing mutation-tail position.
      // It may borrow this exact background repair once; the already queued
      // invocation then retires without running the snapshot a second time.
      if (claimed) return;
      claimed = true;
      final historyWait = _storeHistoryRepairWait;
      if (_canBorrowStoreHistoryRepair(session)) {
        _appTrace?.record(
          'litchcraft.store_history_repair_started',
          fields: <String, Object?>{
            'borrowed_queued': queued,
            'expected_revision': historyWait!.revision,
            'generation': _storeAuthority?.generation,
            'revision': _storeAuthority?.revision,
            'dirty': _storeAuthority?.dirty,
          },
        );
      }
      try {
        if (!identical(session, _orderedSession) ||
            _pendingFrameStoreConflict != null ||
            controllerGeneration != _generation ||
            linkToken != _linkToken) {
          return;
        }
        final confirmedBoard = documentSession.boardDocument;
        if (_physicalStoreRepairThroughRevision == 0 &&
            _canGuardStorePaint(session) &&
            confirmedBoard != null &&
            _isConfirmedStoreHistoryBase(confirmedBoard)) {
          // A queued repair can outlive the receipt that proves the complete
          // known inverse. Retire through the same owner and completion path;
          // unknown history and newer foreign edits still require readback.
          return;
        }
        final physicalRevision = _physicalStoreEditRevision;
        final sourceGeneration = documentSession.generation;
        final strokeBefore =
            _physicalStoreRepairThroughRevision != 0 ||
                _unconfirmedPaintDocumentBase != null
            ? documentSession.nextUndoDocument
            : null;
        var undonePendingStroke = false;
        var unprovenStrokeCancellation = false;
        final timelineBefore = _advancedTimeline;
        final runtimeSequenceBefore = _connectionStateRevision;
        final playbackIntentBefore = _playbackIntentGeneration;
        final authorityBefore = _storeAuthority;
        final entryInspection = _pendingStoreEntryInspection;
        if (authorityBefore == null || authorityBefore.revision == 0) return;
        late final LitchcraftFrameStoreSnapshot snapshot;
        late final LitchcraftFrameStoreState state;
        _StoreAuthority? authorityAfter;
        final requestedModeSession = _documentSessionId;
        var readStage = 'live_snapshot';
        try {
          final client = _frameStoreClient(session);
          final readbackToken = Object();
          var fullSyncStarted = false;
          final readRetired = Completer<void>();
          final replacementGeneration = documentSession.replacementGeneration;
          bool readCurrent() =>
              !_disposed &&
              _selected &&
              _boardActive &&
              _exiting == null &&
              identical(session, _orderedSession) &&
              controllerGeneration == _generation &&
              linkToken == _linkToken &&
              requestedModeSession == _documentSessionId &&
              replacementGeneration == documentSession.replacementGeneration;
          void retireRead() {
            if (!readCurrent() && !readRetired.isCompleted) {
              readRetired.complete();
            }
          }

          addListener(retireRead);
          _physicalStoreReadbackToken = readbackToken;
          if (entryInspection != null && _frameStoreReadPending) {
            _frameStoreSnapshotRead = (
              owner: readbackToken,
              generation: controllerGeneration,
              selectionIntent: _selectionIntent,
              linkToken: linkToken,
              sourceGeneration: documentSession.replacementGeneration,
            );
          }
          _notify();
          try {
            snapshot = await client.liveSnapshot(
              session: requestedModeSession,
              generation: authorityBefore.generation,
              liveRevision: authorityBefore.revision,
              allocateRequest: () => _requestId = (_requestId % 0xffff) + 1,
              knownFrames: _knownLiveStoreFrames().toList(growable: false),
              cancellationFuture: readRetired.future,
              onProgress: (completed, total, frameCount) {
                // Only the existing foreground entry/read owner presents
                // progress. Ordinary physical repairs remain silent.
                if (!readCurrent() ||
                    !identical(_frameStoreSnapshotRead?.owner, readbackToken) ||
                    frameCount <= 1) {
                  return;
                }
                if (!fullSyncStarted) {
                  fullSyncStarted = true;
                  _startFullAnimationSync(
                    readbackToken,
                    LitchcraftFullSyncDirection.readingFromBoard,
                  );
                }
                _updateFullAnimationSync(readbackToken, completed, total);
              },
            );
          } finally {
            _finishFullAnimationSync(readbackToken);
            removeListener(retireRead);
            if (identical(_physicalStoreReadbackToken, readbackToken)) {
              _physicalStoreReadbackToken = null;
              _notify();
            }
            if (identical(_frameStoreSnapshotRead?.owner, readbackToken)) {
              _frameStoreSnapshotRead = null;
              _notify();
            }
          }
          if (!readCurrent()) return;
          // The verified live table already contains the complete manifest.
          // Only a changed resident needs the separate resident listing.
          readStage = 'resident_inspect';
          state =
              _storeCommonAuthority?.generation != authorityBefore.generation
              ? await client.inspect(
                  session: requestedModeSession,
                  request: _requestId = (_requestId % 0xffff) + 1,
                )
              : LitchcraftFrameStoreState(
                  session: snapshot.session,
                  generation: snapshot.generation,
                  requestId: 0,
                  status: 0,
                  active: snapshot.active,
                  playing: snapshot.playing,
                  hasArtwork: true,
                  frameCount: snapshot.document.frames.length,
                  selectedFrame: snapshot.selectedFrame,
                  topology: snapshot.document.topology.id,
                  selectedRootSlot: _selectedFrameStoreRootSlot,
                );
          if (!readCurrent()) return;
          if (strokeBefore != null &&
              documentSession.hasLocalIntent &&
              _acceptedPendingStrokeMatches(snapshot, session) &&
              _sameStorePreimage(snapshot.document, strokeBefore)) {
            // A matching paint alone cannot prove Undo. Read existing history
            // state before the unchanged live-revision proof below.
            readStage = 'history_state';
            final historyState = await session.readState(
              code: ConnectionSession.litchcraftStateCode,
              earliestSequence: _connectionStateRevision,
              accept: (packet) =>
                  packet.body.length == 10 &&
                  packet.body[0] == snapshot.selectedFrame &&
                  packet.body[1] == (snapshot.document.frames.length & 0xff) &&
                  packet.body[4] & 3 == 1 &&
                  packet.body[9] & ~3 == 0,
            );
            if (!readCurrent()) return;
            undonePendingStroke = historyState.body[9] & 2 != 0;
            // Equal pre-stroke pixels without fresh Redo are competing paint,
            // not permission to replay this same accepted gesture over them.
            unprovenStrokeCancellation = !undonePendingStroke;
          }
          readStage = 'authority';
          authorityAfter = await _requestFreshStoreAuthority(session);
          if (!readCurrent()) return;
          if (historyWait != null) {
            _appTrace?.record(
              'litchcraft.store_history_repair_snapshot',
              fields: <String, Object?>{
                'snapshot_generation': snapshot.generation,
                'authority_generation': authorityAfter?.generation,
                'authority_revision': authorityAfter?.revision,
                'authority_dirty': authorityAfter?.dirty,
                'expected_revision': historyWait.revision,
              },
            );
          }
        } catch (error) {
          _appTrace?.record(
            'litchcraft.physical_store_read_failed',
            fields: <String, Object?>{
              'operation': readStage,
              'kind': error.runtimeType.toString(),
              if (error is LitchcraftRejected) 'status': error.status,
              'mode_session': requestedModeSession,
              'current_mode_session': _documentSessionId,
              'controller_generation': controllerGeneration,
              'link_token': linkToken,
              'session_activated': session.isActivated,
              'board_active': _boardActive,
              'store_generation': authorityBefore.generation,
              'live_revision': authorityBefore.revision,
              'store_dirty': authorityBefore.dirty,
              'common_revision': _storeCommonAuthority?.revision,
            },
          );
          final channelFailure =
              error is FormatException ||
              error is TimeoutException ||
              (error is LitchcraftRejected &&
                  error.status != 1 &&
                  error.status != 9);
          if (!channelFailure) {
            // Busy and a newer durable generation are coherent refusals.
            // The shared transport owns retirement of native SDK failures.
            defer(error);
            return;
          }
          if (!_disposed &&
              identical(session, _orderedSession) &&
              controllerGeneration == _generation &&
              linkToken == _linkToken &&
              deviceId == _deviceId) {
            // Invalid or incomplete read data cannot establish a common
            // canvas. Reuse finite channel recovery instead of polling a
            // healthy-looking BLE link forever.
            if (entryInspection != null) {
              _failFrameStoreEntry(error);
            } else {
              _transportFailed(error);
            }
          }
          return;
        }
        if (!identical(session, _orderedSession) ||
            _pendingFrameStoreConflict != null ||
            controllerGeneration != _generation ||
            linkToken != _linkToken ||
            deviceId != _deviceId) {
          return;
        }
        if (snapshot.session != _documentSessionId ||
            snapshot.generation != authorityBefore.generation ||
            snapshot.liveRevision != authorityBefore.revision ||
            snapshot.documentEpoch == 0 ||
            authorityAfter == null ||
            authorityAfter.generation != authorityBefore.generation ||
            authorityAfter.revision != authorityBefore.revision ||
            authorityAfter.bootNonce != authorityBefore.bootNonce ||
            _storeAuthority?.generation != authorityBefore.generation ||
            _storeAuthority?.revision != authorityBefore.revision ||
            physicalRevision != _physicalStoreEditRevision ||
            sourceGeneration != documentSession.generation ||
            ((undonePendingStroke || unprovenStrokeCancellation) &&
                !_acceptedPendingStrokeMatches(snapshot, session))) {
          defer(StateError('Store authority changed during physical repair'));
          return;
        }
        if (historyWait != null &&
            _canBorrowStoreHistoryRepair(session) &&
            historyWait.revision != 0 &&
            authorityAfter.revision != historyWait.revision &&
            historyWait.projectionRecorded() &&
            !historyWait.resultProved()) {
          // A complete inverse arrived before its terminal checkpoint. Keep
          // recovery pending until that checkpoint validates the action; a
          // newer verified read cannot approve or disprove it in the meantime.
          defer(StateError('Store history terminal checkpoint pending'));
          return;
        }
        if (!snapshot.playing &&
            snapshot.selectedFrame != _advancedSelectedFrame &&
            _connectionStateRevision == runtimeSequenceBefore &&
            identical(_advancedTimeline, timelineBefore) &&
            _playbackIntentGeneration == playbackIntentBefore) {
          // This verified paused runtime supersedes an old frame anchor. A
          // newer State1 or local timeline intent retains its own authority.
          _advancedTimeline = null;
          _advancedSelectedFrame = snapshot.selectedFrame;
          _frameStorePlaying = false;
        }
        // Preserve removed/replaced frames as bounded reuse evidence for a
        // later native inverse. Their signature must still match its new table.
        final previousBoard = documentSession.boardDocument;
        if (previousBoard != null) {
          final currentSignatures = {
            for (final frame in snapshot.document.frames)
              LitchcraftFrameStoreFrameSignature.forFrame(frame),
          };
          for (final frame in previousBoard.frames) {
            if (!currentSignatures.contains(
              LitchcraftFrameStoreFrameSignature.forFrame(frame),
            )) {
              _retainStoreFrame(frame);
            }
          }
        }
        _frameStoreGeneration = snapshot.generation;
        if (entryInspection != null &&
            identical(_pendingStoreEntryInspection, entryInspection)) {
          _pendingStoreEntryInspection = null;
          _frameStoreReadPending = false;
        }
        if (_retainAcceptedArtworkForUnexpectedResident(
          session: session,
          state: state,
          deviceId: deviceId,
          document: snapshot.document,
        )) {
          return;
        }
        _forgetSelectedFrameStoreResident();
        _storeHistoryEcho = null;
        _projectingDocument = true;
        try {
          final artwork = _storeArtworkWithLocalPalette(snapshot.document);
          if (undonePendingStroke) {
            documentSession.observeBoard(deviceId, artwork);
            documentSession.acceptBoard(expectedGeneration: sourceGeneration);
            _pendingStoreStrokeEnd = null;
            _appTrace?.record(
              'litchcraft.store_history_stroke_physical_undo',
              fields: {
                'stroke': _acceptedStoreStroke!.stroke,
                'frame': snapshot.selectedFrame,
                'accepted_revision': _acceptedStoreStroke!.revision,
                'undo_revision': snapshot.liveRevision,
                'paint_generation': _acceptedStoreStroke!.paintGeneration,
              },
            );
          } else if (strokeBefore != null) {
            documentSession.retireLocalHistory();
          }
          final history = _pendingStoreCanvas;
          final foreignUnknownHistory =
              historyWait != null &&
              historyWait.revision != 0 &&
              _canBorrowStoreHistoryRepair(session) &&
              authorityAfter.revision != historyWait.revision &&
              !historyWait.resultProved();
          if (!unprovenStrokeCancellation &&
              !foreignUnknownHistory &&
              history != null &&
              history.generation == controllerGeneration &&
              history.sourceGeneration ==
                  documentSession.replacementGeneration &&
              history.modeSession == _documentSessionId &&
              identical(history.session, session) &&
              documentSession.hasCommonDocument &&
              (history.historyRevision == 0 ||
                  authorityAfter.revision == history.historyRevision)) {
            // The exact native journal root supplies fields the phone
            // could not preview. Rebase unchanged fields at the existing
            // common ancestor before preserving later local overrides.
            documentSession.observeBoard(deviceId, artwork);
            if (documentSession.mergeBoard(
              orderedCanvas: (
                frame: history.frame,
                mask: history.mask,
                action: history.action,
                effectEdits: history.effectEdits,
                preview:
                    !history.completed.isCompleted &&
                    history.followingHistory == null,
              ),
            )) {
              documentSession.confirm(deviceId, artwork);
            }
          }
          final foreignHistoryRevision =
              history != null &&
              history.followingHistory != null &&
              history.historyRevision != 0 &&
              history.generation == controllerGeneration &&
              history.sourceGeneration ==
                  documentSession.replacementGeneration &&
              history.modeSession == _documentSessionId &&
              identical(history.session, session) &&
              authorityAfter.revision != history.historyRevision;
          final resolution = documentSession.observeBoard(deviceId, artwork);
          final conflicted =
              unprovenStrokeCancellation ||
              foreignUnknownHistory ||
              foreignHistoryRevision ||
              resolution == LitchcraftDocumentResolution.conflict ||
              (resolution == LitchcraftDocumentResolution.sendApp &&
                  !sameLitchcraftDocumentLayout(
                    artwork,
                    documentSession.document,
                  ));
          if (unprovenStrokeCancellation ||
              foreignUnknownHistory ||
              foreignHistoryRevision) {
            // A later foreign edit is not this journal inverse. Keep both
            // the coherent board bytes and later local preview for the
            // existing conflict choice; never replay the preview over it.
          } else if (resolution == LitchcraftDocumentResolution.equal) {
            documentSession.confirm(deviceId, artwork);
          } else if (resolution == LitchcraftDocumentResolution.sendApp &&
              !conflicted) {
            // A physical patch already projected locally may still be in
            // the worker's dirty window when the durable root is read.
            documentSession.confirm(deviceId, artwork);
          } else if (resolution == LitchcraftDocumentResolution.merge) {
            documentSession.mergeBoard();
          } else if (resolution == LitchcraftDocumentResolution.adoptBoard) {
            documentSession.acceptBoard(expectedGeneration: sourceGeneration);
          }
          if (_advancedSelectedFrame < snapshot.document.frames.length) {
            _selectStoreBoardFrame(_advancedSelectedFrame);
          }
          _canvas = documentSession.canvas.withTopology(
            snapshot.document.topology,
          );
          final board = documentSession.boardDocument;
          _board = board == null
              ? _canvas
              : LitchcraftCanvas(
                  colors: board
                      .frames[documentSession.selectedFrameIndex]
                      .emitters
                      .map((emitter) => emitter.color),
                  topology: board.topology,
                );
          _detached = _canvas;
          _offlineDiverged = conflicted;
          if (conflicted) {
            // The choice owns this coherent root until the user resolves it;
            // a later equal repair must not silently choose either artwork.
            retireConflictBoardPreview();
            _pendingFrameStoreConflict =
                _PendingFrameStoreConflict(
                    session: session,
                    authoritySession: state.session,
                    observedFrameCount: state.frameCount,
                    deviceId: deviceId,
                    generation: controllerGeneration,
                    selectionIntent: _selectionIntent,
                    linkToken: linkToken,
                    sourceGeneration: documentSession.replacementGeneration,
                    bootNonce: session.boardBootNonce,
                  )
                  ..previewSnapshot = snapshot
                  ..previewAuthority = authorityAfter;
          }
          _status = conflicted
              ? LitchcraftStatus.conflict
              : _boardActive
              ? LitchcraftStatus.live
              : LitchcraftStatus.offline;
          if (!conflicted) {
            // Verified live bytes establish common authority even while the
            // background save is pending. Saved remains a separate fact.
            _storeCommonAuthority = authorityAfter;
            _storeCommonBindingGeneration = null;
            final board = documentSession.boardDocument;
            needsCatchUp =
                board != null &&
                !sameLitchcraftDocument(documentSession.document, board);
            _storePaintWaitingForAuthority = false;
          }
          // The document session now owns every blocked local square. Do
          // not replay its old stroke with a later brush's color or mask.
          _strokeSquares.clear();
          _strokeBegun = false;
          _clearPendingPaint();
        } finally {
          _projectingDocument = false;
        }
        _appTrace?.record(
          'litchcraft.physical_store_repair_complete',
          fields: <String, Object?>{
            'generation': snapshot.generation,
            'revision': snapshot.liveRevision,
            'needs_catch_up': needsCatchUp,
            'pending_canvas': _pendingStoreCanvas != null,
            'status': _status.name,
          },
        );
        _physicalStoreRepairThroughRevision = 0;
        _notify();
      } catch (error) {
        defer(error);
      } finally {
        if (!repaired.isCompleted) repaired.complete();
      }
    }

    _physicalStoreRepairResume = repair;
    try {
      if (_canBorrowStoreHistoryRepair(session)) {
        unawaited(repair());
      } else {
        queued = true;
        _appTrace?.record('litchcraft.store_history_repair_queued');
        unawaited(
          _enqueue(repair, recoverBusy: true, background: true).then<void>(
            (_) {
              // A canceled source can retire the queued invocation before it
              // runs. Retire its one repair owner without borrowing new work.
              if (!claimed && !repaired.isCompleted) repaired.complete();
            },
            onError: (Object error) {
              defer(error);
              if (!repaired.isCompleted) repaired.complete();
            },
          ),
        );
      }
      await repaired.future;
    } catch (error) {
      defer(error);
    } finally {
      if (identical(_physicalStoreRepairResume, repair)) {
        _physicalStoreRepairResume = null;
      }
      _physicalStoreRepairing = false;
      if (!_disposed &&
          identical(session, _orderedSession) &&
          controllerGeneration == _generation) {
        _notify();
        // A deferred Canvas cannot resume while this owner still repairs.
        // Release it first, then wake the existing canvas/stroke tail once.
        if ((needsCatchUp || _pendingStoreStrokeEnd != null) &&
            _status == LitchcraftStatus.live) {
          // Equal repaired pixels can still have an accepted finger-up tail.
          // Closing that history boundary is required before Store replacement.
          _storePaintWaitingForAuthority = true;
          _resumeStorePaintIfReady(session);
        }
      }
      if (retry) _schedulePhysicalStoreRepair();
    }
  }

  Future<void> _setBrush({
    int? color,
    bool? eraser,
    int? targetMask,
    bool rememberPreviousColor = true,
    bool syncBoard = true,
  }) async {
    endStroke();
    if (color != null && (color < 0 || color > 0xffffff)) {
      throw RangeError('Invalid color');
    }
    if (targetMask != null && (targetMask < 1 || targetMask > 15)) {
      throw RangeError('Invalid target');
    }
    if (color != null && color != _color) {
      if (rememberPreviousColor) _previousColor = _color;
      _color = color;
    }
    _eraser = eraser ?? (color != null ? false : _eraser);
    _targetMask = targetMask ?? _targetMask;
    _notify();
    if (syncBoard && _status == LitchcraftStatus.live) {
      await _queueLatestBrushPreview();
    }
  }

  Future<void> swapBrushColor() async {
    _colorPreviewOrigin = null;
    final int? previous = _previousColor;
    if (previous == null || previous == _color) return;
    await setBrush(color: previous, eraser: _eraser);
  }

  /// Playback and frame selection are runtime commands when an advanced board
  /// owns the document. Detached artwork keeps its local, non-BLE preview.
  LitchcraftV2TimelineSample _projectStoreTimeline({
    required int frame,
    required int elapsedMilliseconds,
    required bool playing,
  }) {
    final now = _advancedTimelineNow;
    final sample = LitchcraftV2TimelineSample(
      session: _documentSessionId,
      revision: _frameStoreGeneration,
      // Zero distinguishes local intent from a board sample. The first
      // accepted State must replace this anchor, even within one minute.
      sampleId: 0,
      playing: playing,
      transferSuspended: _boardTransferSuspended,
      selectedFrame: frame,
      elapsedMilliseconds: elapsedMilliseconds,
      receivedAtMonotonicMilliseconds: now,
    );
    _advancedTimeline = sample;
    _storePausedIntent = playing ? null : _playbackIntentGeneration;
    if (animationTimelinePending) _storeTimelineProjection = sample;
    _frameStorePlaying = playing;
    _frameStoreStartedAt = now;
    _playbackClockSlewCorrectionMs = 0;
    _selectStoreBoardFrame(frame);
    _notify();
    return sample;
  }

  ({int generation, int intent, int linkToken}) _beginStoreTimelineCommand() {
    final command = (
      generation: _generation,
      intent: _playbackIntentGeneration,
      linkToken: _linkToken,
    );
    _storeTimelineCommand = command;
    _notify();
    return command;
  }

  void _finishStoreTimelineCommand(
    ({int generation, int intent, int linkToken}) command,
  ) {
    if (_storeTimelineCommand != command) return;
    _storeTimelineCommand = null;
    final projection = _storeTimelineProjection;
    _storeTimelineProjection = null;
    final pending = _pendingAdvancedTimeline;
    if (usesFrameStoreDocument && pending != null) {
      _pendingAdvancedTimeline = null;
      final matchesIntent =
          projection == null ||
          (pending.playing == projection.playing &&
              (pending.playing ||
                  pending.selectedFrame == projection.selectedFrame));
      if (matchesIntent && _receiveAdvancedTimeline(pending)) {
        _connectionPlaybackStateRevision = pending.sampleId;
      } else if (!matchesIntent) {
        // The mismatch may be a predecessor or a physical action applied
        // after ours. Query current retained State after this receipt; leave
        // its sequence unconsumed so an unchanged physical State can answer.
        final ordered = _orderedSession;
        if (ordered != null && ordered.isActivated && _boardActive) {
          unawaited(
            ordered
                .requestState(code: ConnectionSession.litchcraftStateCode)
                .catchError((Object _) {}),
          );
        }
      }
    }
    _notify();
  }

  Future<void> _playQueuedStore({required ConnectionSession ordered}) async {
    if (!_queuedStorePlay) return;
    _queuedStorePlayFailed = false;
    final controllerGeneration = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final linkToken = _linkToken;
    final intentGeneration = ++_playbackIntentGeneration;
    _notify();
    if (_disposed ||
        !_boardActive ||
        !_linkUsable ||
        !identical(ordered, _orderedSession)) {
      return;
    }
    final command = _beginStoreTimelineCommand();
    try {
      // A pending inverse may still be outside the tail waiting for its Paint.
      // Wait here before submitting Play so neither can overtake the other.
      final predecessor = _storeEditPosition;
      if (predecessor != null &&
          !predecessor.completion.isCompleted &&
          _storePositionCurrent(predecessor)) {
        final result = await predecessor.completion.future;
        if (result == null) throw const LitchcraftResyncRequired();
      }
      if (!_queuedStorePlay ||
          controllerGeneration != _generation ||
          sourceGeneration != documentSession.replacementGeneration ||
          linkToken != _linkToken ||
          !identical(ordered, _orderedSession)) {
        return;
      }
      final board = documentSession.boardDocument;
      if (board != null &&
          !sameLitchcraftDocument(board, documentSession.document)) {
        await _enqueue(_sendDocumentDelta, recoverBusy: true);
      }
      if (!_queuedStorePlay ||
          controllerGeneration != _generation ||
          sourceGeneration != documentSession.replacementGeneration ||
          linkToken != _linkToken ||
          _status != LitchcraftStatus.live ||
          !identical(ordered, _orderedSession)) {
        return;
      }
      ConnectionSessionOutcome? outcome;
      Object? submissionError;
      await _enqueue(() async {
        if (!_queuedStorePlay ||
            controllerGeneration != _generation ||
            sourceGeneration != documentSession.replacementGeneration ||
            linkToken != _linkToken ||
            _status != LitchcraftStatus.live ||
            !identical(ordered, _orderedSession)) {
          return;
        }
        _queuedStorePlay = false;
        _notify();
        final body = Uint8List(6);
        body[0] = 1;
        body[1] = animationSelectedFrameIndex;
        ByteData.sublistView(
          body,
        ).setUint32(2, _frameStoreGeneration, Endian.little);
        try {
          outcome = await ordered.sendOrdered(
            ConnectionSession.litchcraftTimelineOpcode,
            body,
          );
        } catch (error) {
          submissionError = error;
        }
      });
      if (submissionError != null) throw submissionError!;
      final receipt = outcome;
      if (receipt == null) return;
      if (receipt.disposition != ConnectionSessionDisposition.applied) {
        throw LitchcraftRejected(receipt.reason);
      }
      if (!_disposed &&
          controllerGeneration == _generation &&
          linkToken == _linkToken &&
          intentGeneration == _playbackIntentGeneration) {
        // A board Play edge is already authoritative. A delayed paused State
        // from the just-committed root may also arrive during this receipt;
        // the accepted Play still needs a local clock until its edge arrives.
        if (_advancedTimeline?.playing != true) {
          final frame = documentSession.selectedFrameIndex;
          _projectStoreTimeline(
            frame: frame,
            elapsedMilliseconds:
                litchcraftAnimationFrameStartOrNull(
                  documentSession.document,
                  frame,
                ) ??
                0,
            playing: true,
          );
        }
      }
    } catch (error) {
      _appTrace?.record(
        'litchcraft.queued_store_play_failed',
        fields: <String, Object?>{'kind': error.runtimeType.toString()},
      );
      if (!_disposed &&
          controllerGeneration == _generation &&
          linkToken == _linkToken &&
          intentGeneration == _playbackIntentGeneration) {
        _frameStorePlaying = false;
        _queuedStorePlay = false;
        _queuedStorePlayFailed = true;
        _notify();
      }
    } finally {
      _finishStoreTimelineCommand(command);
    }
  }

  Future<void> toggleAnimationPlayback() async {
    if (boardArtworkUnresolved) return;
    endStroke();
    if (_queuedStorePlayFailed) {
      _queuedStorePlayFailed = false;
      _notify();
    }
    if (usesFrameStoreDocument) {
      if (!_boardActive || !_linkUsable) await enter();
      // Refused admission leaves the editor detached. Only an admitted mode
      // may retain a Play intent for an in-flight Store publication.
      if (!_boardActive || !_linkUsable) return;
      final ordered = _orderedSession;
      if (_queuedStorePlay) {
        _queuedStorePlay = false;
        _notify();
        return;
      }
      if (_acceptedStoreCommitAwaitingRoot) {
        _queuedStorePlay = !_queuedStorePlay;
        _notify();
        return;
      }
      if (_storeTransferInFlight && !animationPlaying) {
        _queuedStorePlay = !_queuedStorePlay;
        _notify();
        return;
      }
      if (storeReplacementUnresolved && !_acceptedStoreCommitAwaitingRoot) {
        return;
      }
      if (ordered != null &&
          _frameStoreGeneration != 0 &&
          _boardActive &&
          _linkUsable) {
        final wasPlaying = animationPlaying;
        final intendedPlaying = !wasPlaying;
        final frame = animationSelectedFrameIndex;
        final controllerGeneration = _generation;
        final sourceGeneration = documentSession.replacementGeneration;
        final selectionIntent = _selectionIntent;
        final linkToken = _linkToken;
        final intentGeneration = ++_playbackIntentGeneration;
        final editPosition = _storeEditPosition;
        final predecessor =
            editPosition != null &&
                !editPosition.completion.isCompleted &&
                _storePositionCurrent(editPosition)
            ? editPosition
            : null;
        final command = _beginStoreTimelineCommand();
        final previousTimeline = _advancedTimeline;
        final projected = _projectStoreTimeline(
          frame: frame,
          elapsedMilliseconds: animationElapsedMilliseconds,
          playing: intendedPlaying,
        );
        void rollbackIfCurrent() {
          if (!_disposed &&
              controllerGeneration == _generation &&
              intentGeneration == _playbackIntentGeneration &&
              identical(_advancedTimeline, projected)) {
            _advancedTimeline = previousTimeline;
            if (identical(_storeTimelineProjection, projected)) {
              _storeTimelineProjection = null;
            }
            _frameStorePlaying = wasPlaying;
            _storePausedIntent = wasPlaying ? null : intentGeneration;
            _notify();
          }
        }

        bool current() =>
            !_disposed &&
            controllerGeneration == _generation &&
            sourceGeneration == documentSession.replacementGeneration &&
            selectionIntent == _selectionIntent &&
            linkToken == _linkToken &&
            identical(ordered, _orderedSession) &&
            ordered.isActivated &&
            _selected &&
            _boardActive &&
            _linkUsable &&
            _frameStoreGeneration != 0;

        Future<void> sendPlayback() async {
          // This command already owns its FIFO position. A newer projection
          // cannot omit its Pause and leave the following Play without a seek.
          if (!current()) {
            rollbackIfCurrent();
            return;
          }
          try {
            final body = Uint8List(6);
            body[0] = intendedPlaying ? 1 : 0;
            body[1] = frame;
            ByteData.sublistView(
              body,
            ).setUint32(2, _frameStoreGeneration, Endian.little);
            final result = await ordered.sendOrdered(
              ConnectionSession.litchcraftTimelineOpcode,
              body,
            );
            if (result.disposition != ConnectionSessionDisposition.applied) {
              throw LitchcraftRejected(result.reason);
            }
            // An authoritative board edge may arrive before this checkpoint.
            // Its newer state must keep the displayed clock and playback bit.
            if (intentGeneration == _playbackIntentGeneration &&
                identical(_advancedTimeline, projected)) {
              _frameStorePlaying = intendedPlaying;
              _notify();
            }
          } catch (_) {
            rollbackIfCurrent();
            rethrow;
          }
        }

        try {
          // History can still be waiting for its preceding paint receipt and
          // has not reserved the command tail yet. Keep this later intent
          // behind that exact edit position before admitting its Timeline.
          if (predecessor != null) {
            final result = await predecessor.completion.future;
            if (result == null ||
                !_storePositionCurrent(predecessor) ||
                frame >= result.document.frames.length) {
              rollbackIfCurrent();
              return;
            }
          }
          if (!current() || intentGeneration != _playbackIntentGeneration) {
            rollbackIfCurrent();
            return;
          }
          if (_storeTransferInFlight || _acceptedStoreCommitAwaitingRoot) {
            await sendPlayback();
          } else {
            await _enqueue(sendPlayback, recoverBusy: true);
          }
        } finally {
          _finishStoreTimelineCommand(command);
        }
        return;
      }
      await _transferFrameStore(
        frames: documentSession.document.frames,
        playbackOnEntry: documentSession.document.playbackOnEntry,
        playWhenReady: true,
      );
      return;
    }
    if (!animationBoardAuthorityLive) return;
    final ordered = _orderedSession;
    if (ordered != null) {
      final wasPlaying = _advancedPlaying;
      final intendedPlaying = !wasPlaying;
      final frame = animationSelectedFrameIndex;
      final controllerGeneration = _generation;
      final intentGeneration = ++_playbackIntentGeneration;
      _advancedPlaying = intendedPlaying;
      _notify();
      void rollbackIfCurrent() {
        if (!_disposed &&
            controllerGeneration == _generation &&
            intentGeneration == _playbackIntentGeneration) {
          _advancedPlaying = wasPlaying;
          _notify();
        }
      }

      await _enqueue(() async {
        if (!identical(ordered, _orderedSession) ||
            !animationBoardAuthorityLive) {
          rollbackIfCurrent();
          return;
        }
        try {
          final result = await ordered.sendOrdered(
            ConnectionSession.litchcraftTimelineOpcode,
            Uint8List.fromList([intendedPlaying ? 1 : 0, frame]),
          );
          if (result.disposition != ConnectionSessionDisposition.applied) {
            throw LitchcraftRejected(result.reason);
          }
        } catch (_) {
          rollbackIfCurrent();
          rethrow;
        }
      }, recoverBusy: true);
      return;
    }
    await _enqueue(() async {
      final link = _link;
      if (link == null || !animationBoardAuthorityLive) return;
      final state = _advancedPlaying
          ? await link.pauseV2(
              session: _documentSessionId,
              frame: animationSelectedFrameIndex,
            )
          : await link.playV2(
              session: _documentSessionId,
              frame: _advancedSelectedFrame,
            );
      _applyAdvancedRuntimeState(state);
    }, recoverBusy: true);
  }

  Future<void> selectAnimationFrame(int index) async {
    RangeError.checkValidIndex(index, documentSession.document.frames, 'index');
    endStroke();
    if (usesFrameStoreDocument) {
      final previousFrame = documentSession.selectedFrameIndex;
      documentSession.selectFrame(index);
      if (storeReplacementUnresolved) _pendingStoreSelectedFrame = index;
      if (storeReplacementUnresolved && !_acceptedStoreCommitAwaitingRoot) {
        return;
      }
      final ordered = _orderedSession;
      if (ordered == null ||
          _frameStoreGeneration == 0 ||
          !_boardActive ||
          !_linkUsable) {
        return;
      }
      final previousTimeline = _advancedTimeline;
      final wasPlaying = animationPlaying;
      final controllerGeneration = _generation;
      final intentGeneration = ++_playbackIntentGeneration;
      final structure = _storeStructuralEditPosition;
      final command = _beginStoreTimelineCommand();
      final projected = _projectStoreTimeline(
        frame: index,
        elapsedMilliseconds:
            litchcraftAnimationFrameStartOrNull(
              documentSession.document,
              index,
            ) ??
            0,
        playing: false,
      );
      void rollbackIfCurrent() {
        if (_disposed ||
            controllerGeneration != _generation ||
            intentGeneration != _playbackIntentGeneration ||
            !identical(_advancedTimeline, projected)) {
          return;
        }
        _advancedTimeline = previousTimeline;
        if (identical(_storeTimelineProjection, projected)) {
          _storeTimelineProjection = null;
        }
        _frameStorePlaying = wasPlaying;
        _storePausedIntent = wasPlaying ? null : intentGeneration;
        documentSession.selectFrame(previousFrame);
        _notify();
      }

      try {
        if (structure != null) await structure.completion.future;
        await _enqueue(() async {
          if (!identical(ordered, _orderedSession) ||
              !_boardActive ||
              storeReplacementUnresolved) {
            // A pending replacement owns the local frame choice. Its failure
            // must not resurrect an older frame or select the older board root.
            if (!storeReplacementUnresolved) rollbackIfCurrent();
            return;
          }
          try {
            final body = Uint8List(6);
            body[0] = 2;
            body[1] = index;
            ByteData.sublistView(
              body,
            ).setUint32(2, _frameStoreGeneration, Endian.little);
            final result = await ordered.sendOrdered(
              ConnectionSession.litchcraftTimelineOpcode,
              body,
            );
            if (result.disposition != ConnectionSessionDisposition.applied) {
              throw LitchcraftRejected(result.reason);
            }
          } catch (_) {
            rollbackIfCurrent();
            rethrow;
          }
        }, recoverBusy: true);
      } finally {
        _finishStoreTimelineCommand(command);
      }
      return;
    }
    if (!animationBoardAuthorityLive) {
      documentSession.selectFrame(index);
      return;
    }
    await _enqueue(() async {
      final link = _link;
      if (link == null || !animationBoardAuthorityLive) return;
      final ordered = _orderedSession;
      if (ordered != null) {
        final result = await ordered.sendOrdered(
          ConnectionSession.litchcraftTimelineOpcode,
          Uint8List.fromList([2, index]),
        );
        if (result.disposition != ConnectionSessionDisposition.applied) {
          throw LitchcraftRejected(result.reason);
        }
        return;
      }
      final state = await link.selectFrameV2(
        session: _documentSessionId,
        frame: index,
      );
      _applyAdvancedRuntimeState(state);
    }, recoverBusy: true);
  }

  /// Applies one effect gesture to the selected frame. A live schema-3 board
  /// remains authoritative; detached documents use the same local edit owner.
  Future<bool> applyAnimationEffect({
    required int square,
    required int mask,
    required LitchcraftEffectStyle effect,
    int effectDurationMs = litchcraftAnimationDefaultEffectDurationMs,
    LitchcraftFrameEditTarget? target,
  }) async {
    if (square < 0 || square >= 64 || mask < 1 || mask > 15) {
      throw RangeError('Invalid Litchcraft effect selection');
    }
    endStroke();
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      return _editStoreCanvas(
        action: 14,
        mask: mask,
        color: 0,
        target: target,
        effectSquare: square,
        effect: effect,
        effectDurationMs: effectDurationMs,
      );
    }
    if (!animationBoardAuthorityLive) {
      final local = target ?? captureAnimationFrameEditTarget();
      if (local == null || !_matchesLocalFrameEditTarget(local)) return false;
      documentSession.selectFrame(local.frame);
      documentSession.applyEffect(
        square: square,
        mask: mask,
        effect: effect,
        effectDurationMs: effectDurationMs,
      );
      return true;
    }
    final exactTarget = target ?? captureAnimationFrameEditTarget();
    if (exactTarget == null || !_matchesCapturedFrameEditTarget(exactTarget)) {
      return false;
    }
    final frame = exactTarget.frame;
    final stroke = _strokeId = (_strokeId % 0xffff) + 1;
    return _editAdvancedDocument(
      frame: frame,
      mutate: () => documentSession.applyEffect(
        square: square,
        mask: mask,
        effect: effect,
        effectDurationMs: effectDurationMs,
      ),
      send: (link, session, _) => link.effectV2(
        session: session,
        frame: frame,
        effect: effect.id,
        durationMs: effect == LitchcraftEffectStyle.off ? 0 : effectDurationMs,
        stroke: stroke,
        flags: 3,
        mask: mask,
        squares: <int>[square],
      ),
    );
  }

  /// Pauses and selects the exact displayed frame before a structural edit.
  /// The returned authority token becomes stale on reconnect or document edit.
  Future<LitchcraftFrameEditTarget?> prepareAnimationFrameEdit() async {
    endStroke();
    if (!animationBoardAuthorityLive) {
      return captureAnimationFrameEditTarget();
    }
    final frame = animationSelectedFrameIndex;
    LitchcraftFrameEditTarget? target;
    await _enqueue(() async {
      final link = _link;
      if (link == null || !animationBoardAuthorityLive) return;
      final generation = _generation;
      final session = _documentSessionId;
      final revision = _documentRevision;
      final state = await link.selectFrameV2(session: session, frame: frame);
      _applyAdvancedRuntimeState(state);
      if (generation == _generation &&
          session == _documentSessionId &&
          revision == _documentRevision &&
          state.revision == revision &&
          state.selectedFrame == frame &&
          !state.playing) {
        target = LitchcraftFrameEditTarget(
          frame: frame,
          session: session,
          revision: revision,
          generation: generation,
          documentGeneration: documentSession.generation,
        );
      }
    }, recoverBusy: true);
    return target;
  }

  bool _matchesFrameEditTarget(LitchcraftFrameEditTarget target) =>
      animationBoardAuthorityLive &&
      target.generation == _generation &&
      target.session == _documentSessionId &&
      target.revision == _documentRevision;

  bool _matchesSelectedFrameEditTarget(LitchcraftFrameEditTarget target) =>
      _matchesFrameEditTarget(target) &&
      target.frame == _advancedSelectedFrame &&
      target.frame == documentSession.selectedFrameIndex;

  bool _matchesCapturedFrameEditTarget(LitchcraftFrameEditTarget target) =>
      _matchesFrameEditTarget(target) &&
      (target.documentGeneration == null ||
          target.documentGeneration == documentSession.generation);

  /// Capture the displayed edit target without waiting for transport. Used at
  /// pointer-down/dialog entry; callers keep this token until final commit.
  LitchcraftFrameEditTarget? captureAnimationFrameEditTarget({int? frame}) {
    final displayed = frame ?? animationSelectedFrameIndex;
    if (_disposed ||
        displayed < 0 ||
        displayed >= documentSession.document.frames.length) {
      return null;
    }
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      final ordered = _orderedSession;
      final common = _storeCommonAuthority;
      final predecessor = _storeEditPosition;
      if (ordered == null ||
          common == null ||
          _deviceId == null ||
          (!_canGuardStorePaint(ordered) &&
              (predecessor == null || !_storePositionCurrent(predecessor)))) {
        return null;
      }
      return LitchcraftFrameEditTarget._store(
        frame: displayed,
        session: _documentSessionId,
        revision: common.revision,
        generation: _generation,
        documentGeneration: documentSession.generation,
        sourceGeneration: documentSession.replacementGeneration,
        linkToken: _linkToken,
        deviceId: _deviceId,
        bootNonce: ordered.boardBootNonce,
        storeGeneration: common.generation,
        orderedSession: ordered,
        beforeDocument: documentSession.document,
        predecessor: predecessor,
      );
    }
    return LitchcraftFrameEditTarget(
      frame: displayed,
      session: _documentSessionId,
      revision: _documentRevision,
      generation: _generation,
      documentGeneration: documentSession.generation,
      sourceGeneration: documentSession.replacementGeneration,
      linkToken: _linkToken,
      deviceId: _deviceId,
      localOnly: !animationBoardAuthorityLive,
    );
  }

  bool _matchesLocalFrameEditTarget(LitchcraftFrameEditTarget target) =>
      target.localOnly &&
      !_disposed &&
      target.generation == _generation &&
      target.sourceGeneration == documentSession.replacementGeneration &&
      target.linkToken == _linkToken &&
      target.documentGeneration == documentSession.generation &&
      target.frame >= 0 &&
      target.frame < documentSession.document.frames.length &&
      !animationBoardAuthorityLive &&
      !(usesFrameStoreDocument && _status == LitchcraftStatus.live);

  bool _storePositionCurrent(
    _StoreEditPosition position, {
    bool allowConflict = false,
  }) =>
      !_disposed &&
      position.generation == _generation &&
      position.sourceGeneration == documentSession.replacementGeneration &&
      position.modeSession == _documentSessionId &&
      position.linkToken == _linkToken &&
      position.deviceId == _deviceId &&
      identical(position.session, _orderedSession) &&
      position.bootNonce == position.session.boardBootNonce &&
      position.session.isActivated &&
      _boardActive &&
      (_status == LitchcraftStatus.live ||
          (allowConflict && _status == LitchcraftStatus.conflict));

  /// Copies an exact, board-selected frame through board authority, or locally
  /// while detached. Live callers may provide a target latched before UI work.
  Future<bool> addAnimationFrame({LitchcraftFrameEditTarget? target}) async {
    if (documentSession.document.frames.length ==
        litchcraftAnimationMaximumFrameCount) {
      throw const LitchcraftFrameLimit();
    }
    endStroke();
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      return _editStoreCanvas(action: 10, mask: 0, color: 0, target: target);
    }
    if (documentSession.document.frames.length ==
        litchcraftAnimationLegacyMaximumFrameCount) {
      // Crossing the legacy wire bound is an ordinary local document edit;
      // Play later publishes the document using Store, never invalid V2 bytes.
      final local = target ?? captureAnimationFrameEditTarget();
      if (local == null ||
          !(local.localOnly
              ? _matchesLocalFrameEditTarget(local)
              : _matchesCapturedFrameEditTarget(local))) {
        return false;
      }
      documentSession.selectFrame(local.frame);
      _localFrameStoreDocument = true;
      _advancedWire = false;
      _needsResync = false;
      documentSession.addFrame();
      return true;
    }
    if (!animationBoardAuthorityLive) {
      final local = target ?? captureAnimationFrameEditTarget();
      if (local == null || !_matchesLocalFrameEditTarget(local)) return false;
      documentSession.selectFrame(local.frame);
      documentSession.addFrame();
      return true;
    }
    final exactTarget = target ?? captureAnimationFrameEditTarget();
    if (exactTarget == null || !_matchesCapturedFrameEditTarget(exactTarget)) {
      return false;
    }
    return _editAdvancedDocument(
      frame: exactTarget.frame,
      mutate: documentSession.addFrame,
      // AddCopy requires the selected source. A single queued Select is enough;
      // it never delays the local preview or requires a document readback.
      selectBeforeEdit:
          _advancedPlaying || _advancedSelectedFrame != exactTarget.frame,
      send: (link, session, revision) => link.addCopyFrameV2(
        session: session,
        baseRevision: revision,
        source: exactTarget.frame,
      ),
    );
  }

  /// Deletes an exact, board-selected frame through board authority, or locally
  /// while detached. A live target prevents a dialog/reconnect race.
  Future<bool> deleteAnimationFrame({LitchcraftFrameEditTarget? target}) async {
    endStroke();
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      return _editStoreCanvas(
        action: documentSession.document.frames.length == 1 ? 1 : 11,
        mask: documentSession.document.frames.length == 1 ? 15 : 0,
        color: 0,
        target: target,
      );
    }
    if (!animationBoardAuthorityLive) {
      final local = target ?? captureAnimationFrameEditTarget();
      if (local == null || !_matchesLocalFrameEditTarget(local)) return false;
      documentSession.selectFrame(local.frame);
      if (documentSession.document.frames.length == 1) {
        documentSession.clearSelectedFrame();
      } else {
        documentSession.deleteSelectedFrame();
      }
      return true;
    }
    final exactTarget = target ?? captureAnimationFrameEditTarget();
    if (exactTarget == null || !_matchesCapturedFrameEditTarget(exactTarget)) {
      return false;
    }
    if (documentSession.document.frames.length == 1) {
      if (!_advancedCanvasEdit) {
        await clear();
        return _status == LitchcraftStatus.live;
      }
      return _editAdvancedDocument(
        frame: 0,
        mutate: documentSession.clearSelectedFrame,
        send: (link, session, revision) => link.canvasEditV2(
          session: session,
          baseRevision: revision,
          frame: 0,
          mask: 15,
          color: 0,
          erase: true,
        ),
      );
    }
    return _editAdvancedDocument(
      frame: exactTarget.frame,
      mutate: documentSession.deleteSelectedFrame,
      send: (link, session, revision) => link.deleteFrameV2(
        session: session,
        baseRevision: revision,
        source: exactTarget.frame,
      ),
    );
  }

  /// Commit one finger-owned duration value. No full-document replacement or
  /// readback is needed; the source frame is latched even during playback.
  Future<bool> setAnimationFrameDuration(
    int durationMs, {
    LitchcraftFrameEditTarget? target,
  }) async {
    if (durationMs < litchcraftAnimationMinimumFrameDurationMs ||
        durationMs > litchcraftAnimationMaximumFrameDurationMs) {
      throw RangeError.range(
        durationMs,
        litchcraftAnimationMinimumFrameDurationMs,
        litchcraftAnimationMaximumFrameDurationMs,
        'durationMs',
      );
    }
    if (usesFrameStoreDocument && _status == LitchcraftStatus.live) {
      return _editStoreCanvas(
        action: 13,
        mask: 0,
        color: 0,
        frameDurationMs: durationMs,
        target: target,
      );
    }
    if (!animationBoardAuthorityLive) {
      final local = target ?? captureAnimationFrameEditTarget();
      if (local == null || !_matchesLocalFrameEditTarget(local)) return false;
      documentSession.selectFrame(local.frame);
      documentSession.setSelectedFrameDuration(durationMs);
      return true;
    }
    final exactTarget = target ?? captureAnimationFrameEditTarget();
    if (exactTarget == null || !_matchesCapturedFrameEditTarget(exactTarget)) {
      return false;
    }
    return _editAdvancedDocument(
      frame: exactTarget.frame,
      mutate: () => documentSession.setSelectedFrameDuration(durationMs),
      selectBeforeEdit:
          _advancedPlaying || _advancedSelectedFrame != exactTarget.frame,
      send: (link, session, revision) => link.setFrameDurationV2(
        session: session,
        baseRevision: revision,
        source: exactTarget.frame,
        durationMs: durationMs,
      ),
    );
  }

  void _applyAdvancedRuntimeState(LitchcraftV2State state) {
    if (!_acceptAdvancedRuntimeState(state)) return;
    _advancedCanUndo = state.canUndo;
    _advancedCanRedo = state.canRedo;
    _advancedPlaying = state.playing;
    _boardActive = state.active;
    if (state.selectedFrame < documentSession.document.frames.length) {
      _advancedSelectedFrame = state.selectedFrame;
      _selectBoardFrame(state.selectedFrame);
    }
    _notify();
  }

  Future<void> undo() => _history(LitchcraftProtocol.undo);
  Future<void> redo() => _history(LitchcraftProtocol.redo);

  void _retireOfflineReplacementLineage() {
    if (bleService.isConnected) return;
    // Offline New/Clear chooses a phone source, not permission to replace the
    // board on reconnect. Ordinary field edits retain their common ancestor.
    _interruptedStoreReplacement = null;
    _deliberateDeliveryGeneration = -1;
    documentSession.retireBoardLineage();
    _commonCanvas = null;
    _commonBoardId = null;
    _storeCommonAuthority = null;
    _storeCommonBindingGeneration = null;
  }

  Future<void> clear() async {
    endStroke();
    _retireOfflineReplacementLineage();
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        !storeReplacementUnresolved) {
      await _editStoreCanvas(action: 1, mask: 15, color: 0);
      return;
    }
    if (storeReplacementUnresolved ||
        (usesFrameStoreDocument && !bleService.isConnected)) {
      documentSession.clearSelectedFrame();
      _canvas = documentSession.canvas.withTopology(_canvas.topology);
      _offlineDiverged = true;
      _detached = _canvas;
      _notify();
      return;
    }
    if (animationBoardAuthorityLive && _advancedCanvasEdit) {
      await _editAdvancedCanvas(erase: true, mask: 15, color: 0);
      return;
    }
    if (documentSession.isAdvanced || _advancedWire) {
      await _mutateSelectedAdvancedFrame(documentSession.clearSelectedFrame);
      return;
    }
    if (_status == LitchcraftStatus.live) {
      final cleared = LitchcraftCanvas.blank(topology: _canvas.topology);
      final intent = (canvas: cleared, paintBoundary: _paintSequence);
      _pendingStaticCanvas = intent;
      await _enqueue(() async {
        await _link!.send(LitchcraftProtocol.clear, _sessionId);
        if (_pendingStaticCanvas == intent) _pendingStaticCanvas = null;
      });
    } else {
      _remember(_canvas);
      _canvas = LitchcraftCanvas.blank(topology: _canvas.topology);
      _offlineDiverged = true;
      _detached = _canvas;
      _notify();
    }
  }

  /// Requests the board-owned erase gesture so board and phone motion use the
  /// same selected-frame transaction, animation, event, and one-step Undo.
  Future<void> eraseDisplayedFrameByPhone() async {
    if (boardArtworkUnresolved ||
        storeReplacementUnresolved ||
        !_shakeEraseSupported ||
        !_selected ||
        _status != LitchcraftStatus.live ||
        !_boardActive) {
      return;
    }
    endStroke();
    await _enqueue(() async {
      await _link!.send(
        LitchcraftProtocol.shakeErase,
        _advancedWire ? _documentSessionId : _sessionId,
      );
    });
  }

  Future<void> _history(int opcode) async {
    endStroke();
    final predecessor = _storeEditPosition;
    if (storeReplacementUnresolved &&
        predecessor != null &&
        !predecessor.completion.isCompleted &&
        _storePositionCurrent(predecessor)) {
      await _storeHistory(opcode);
      return;
    }
    if (storeReplacementUnresolved ||
        (usesFrameStoreDocument && !bleService.isConnected)) {
      if (opcode == LitchcraftProtocol.undo) {
        documentSession.undo();
      } else {
        documentSession.redo();
      }
      _canvas = documentSession.canvas.withTopology(_canvas.topology);
      _offlineDiverged = true;
      _detached = _canvas;
      _notify();
      return;
    }
    if (usesFrameStoreDocument &&
        _status == LitchcraftStatus.live &&
        _boardActive) {
      await _storeHistory(opcode);
      return;
    }
    if (documentSession.isAdvanced || _advancedWire) {
      if (_advancedWire && _status == LitchcraftStatus.live) {
        final source = opcode == LitchcraftProtocol.undo
            ? _knownAdvancedUndo
            : _knownAdvancedRedo;
        final inverse = source.isEmpty ? null : source.last;
        final next = opcode == LitchcraftProtocol.undo
            ? documentSession.nextUndoDocument
            : documentSession.nextRedoDocument;
        if (inverse != null &&
            next != null &&
            !_documentDirty &&
            inverse.generation == _generation &&
            inverse.session == _documentSessionId &&
            inverse.revision == _documentRevision &&
            sameLitchcraftDocument(documentSession.document, inverse.after) &&
            sameLitchcraftDocument(next, inverse.before)) {
          await _editAdvancedDocument(
            frame: animationSelectedFrameIndex,
            mutate: opcode == LitchcraftProtocol.undo
                ? documentSession.undo
                : documentSession.redo,
            historyOpcode: opcode,
            connectionOpcode: opcode == LitchcraftProtocol.undo
                ? ConnectionSession.litchcraftUndoOpcode
                : ConnectionSession.litchcraftRedoOpcode,
            send: (link, session, _) => opcode == LitchcraftProtocol.undo
                ? link.undoV2(session: session)
                : link.redoV2(session: session),
          );
          return;
        }
        _clearKnownAdvancedHistory();
        await _enqueue(() async {
          if (opcode == LitchcraftProtocol.undo) {
            await _link!.undoV2(session: _documentSessionId);
          } else {
            await _link!.redoV2(session: _documentSessionId);
          }
          _documentDirty = true;
          await _synchronizeDocument();
        });
      } else if (opcode == LitchcraftProtocol.undo) {
        documentSession.undo();
      } else {
        documentSession.redo();
      }
      return;
    }
    if (_status == LitchcraftStatus.live) {
      await _enqueue(() async {
        await _link!.send(opcode, _sessionId);
      });
    } else {
      final source = opcode == LitchcraftProtocol.undo ? _undo : _redo;
      final destination = opcode == LitchcraftProtocol.undo ? _redo : _undo;
      if (source.isEmpty) return;
      destination.add(_canvas);
      _canvas = source.removeLast();
      _offlineDiverged = true;
      _detached = _canvas;
      _notify();
    }
  }

  int? _singleStoreHistoryFrame(
    LitchcraftAnimationDocument before,
    LitchcraftAnimationDocument after,
  ) {
    if (!sameLitchcraftDocumentLayout(before, after)) {
      final position = _storeEditPosition;
      final result = position?.result;
      if (position != null &&
          _storePositionCurrent(position) &&
          result != null &&
          _storeCommonAuthority?.revision == result.authority.revision) {
        if (_sameStorePreimage(before, result.document) &&
            _sameStorePreimage(after, result.before)) {
          return result.selectedBefore;
        }
        if (_sameStorePreimage(before, result.before) &&
            _sameStorePreimage(after, result.document)) {
          return result.selectedAfter;
        }
      }
      // A duration inverse still changes one existing frame. The next queued
      // history action replaces the completed position, but retains both exact
      // local documents. Admit that inverse through the same preimage/Guard
      // checks below; document merge rules continue to treat duration as layout.
      if (before.frames.length != after.frames.length ||
          before.topology != after.topology ||
          before.playbackOnEntry != after.playbackOnEntry ||
          before.sparkleSeed != after.sparkleSeed ||
          !listEquals(before.favoriteColors, after.favoriteColors)) {
        return null;
      }
    }
    int? changedFrame;
    for (var frame = 0; frame < before.frames.length; frame++) {
      final previous = before.frames[frame].emitters;
      final next = after.frames[frame].emitters;
      var changed =
          before.frames[frame].durationMs != after.frames[frame].durationMs;
      if (!changed && identical(previous, next)) continue;
      for (var emitter = 0; emitter < previous.length; emitter++) {
        final a = previous[emitter], b = next[emitter];
        if (a.color != b.color ||
            a.effect != b.effect ||
            a.effectDurationMs != b.effectDurationMs) {
          changed = true;
          break;
        }
      }
      if (!changed) continue;
      if (changedFrame != null) return null;
      changedFrame = frame;
    }
    return changedFrame;
  }

  Future<void> _storeHistory(int opcode) async {
    // A deferred Canvas has no transport tail yet. Preserve the first history
    // intent until its accepted board result is available. Healthy in-flight
    // edits still use the immediate inverse preview and existing ordered tail.
    final pending = _pendingStoreCanvas;
    if (pending != null &&
        (pending.action >= 10 ||
            pending.followingHistory != null ||
            _storePaintWaitingForAuthority ||
            _physicalStoreRepairing ||
            !_canGuardStorePaint(pending.session) ||
            _optimisticDocumentEdits > 1)) {
      await _retainStoreCanvasHistory(opcode);
      return;
    }
    final ordered = _orderedSession;
    final deviceId = _deviceId;
    final before = documentSession.document;
    final next = opcode == LitchcraftProtocol.undo
        ? documentSession.nextUndoDocument
        : documentSession.nextRedoDocument;
    final frame = next == null ? null : _singleStoreHistoryFrame(before, next);
    _appTrace?.record(
      'litchcraft.store_history_requested',
      fields: <String, Object?>{
        'opcode': opcode,
        'known_inverse': frame != null,
        'frame': documentSession.selectedFrameIndex,
        'repair_through': _physicalStoreRepairThroughRevision,
      },
    );
    if (ordered == null || !ordered.isActivated || deviceId == null) {
      _schedulePhysicalStoreRepair();
      return;
    }
    if (next == null || frame == null) {
      final position = _storeUnknownHistory(opcode, ordered);
      _storeUnknownHistoryPosition = (
        position: position,
        bootNonce: ordered.boardBootNonce,
        generation: _generation,
        sourceGeneration: documentSession.replacementGeneration,
        modeSession: _documentSessionId,
        linkToken: _linkToken,
        deviceId: deviceId,
        session: ordered,
      );
      _notify();
      try {
        await position;
      } finally {
        if (identical(_storeUnknownHistoryPosition?.position, position)) {
          _storeUnknownHistoryPosition = null;
          _notify();
        }
      }
      return;
    }
    final layoutHistory = !sameLitchcraftDocumentLayout(before, next);
    final selectedFrameBeforeHistory = documentSession.selectedFrameIndex;
    if (!layoutHistory && frame != documentSession.selectedFrameIndex) {
      // Selection updates the local canvas before its ordered receipt. Queue
      // the transport seek, then preview the inverse in this same event turn.
      unawaited(selectAnimationFrame(frame));
      if (frame != documentSession.selectedFrameIndex ||
          !sameLitchcraftDocument(documentSession.document, before)) {
        return;
      }
    }
    final predecessor = _storeEditPosition;
    final position = _newStoreEditPosition();
    _StoreEditResult? historyResult;
    final generation = _generation;
    final sourceGeneration = documentSession.replacementGeneration;
    final physicalRevision = _physicalStoreEditRevision;
    if (opcode == LitchcraftProtocol.undo) {
      documentSession.undo();
    } else {
      documentSession.redo();
    }
    if (layoutHistory) documentSession.selectFrame(frame);
    final previewGeneration = documentSession.generation;
    _frameStorePlaying = false;
    // Structural history changes cumulative frame offsets immediately. Keep
    // the paused preview on its resulting frame while publication is pending.
    if (layoutHistory) {
      _showStoreFrame(frame);
      _pauseStoreTimelineAtPhysicalFrame(frame);
    } else if (frame == selectedFrameBeforeHistory) {
      // A pending seek retains its own projection until its receipt.
      _advancedTimeline = null;
    }
    _notify();
    try {
      // Deferred paint/layout owners have not reserved the command tail yet.
      // Keep this inverse behind that exact position while its preview is live.
      final predecessorResult = predecessor == null
          ? null
          : await predecessor.completion.future;
      final predecessorProved =
          predecessor == null ||
          (_storePositionCurrent(predecessor) &&
              predecessorResult != null &&
              _sameStorePreimage(predecessorResult.document, before));
      await _enqueue(() async {
        try {
          // Earlier queued paint receipts establish the common revision. A fast
          // paint then Undo must wait for that receipt, not discard the Undo.
          final baseRevision = _storeCommonAuthority?.revision;
          _appTrace?.record(
            'litchcraft.store_history_admission',
            fields: <String, Object?>{
              'opcode': opcode,
              'can_guard': _canGuardStorePaint(ordered),
              'common_generation': _storeCommonAuthority?.generation,
              'common_revision': baseRevision,
              'observed_generation': _storeAuthority?.generation,
              'observed_revision': _storeAuthority?.revision,
              'observed_dirty': _storeAuthority?.dirty,
              'repair_through': _physicalStoreRepairThroughRevision,
              'physical_revision': _physicalStoreEditRevision,
              'physical_before': physicalRevision,
              'board_matches_before':
                  documentSession.boardDocument != null &&
                  sameLitchcraftDocument(
                    documentSession.boardDocument!,
                    before,
                  ),
            },
          );
          if (!predecessorProved ||
              generation != _generation ||
              sourceGeneration != documentSession.replacementGeneration ||
              !ordered.isActivated ||
              !_canGuardStorePaint(ordered) ||
              baseRevision == null ||
              (_physicalStoreEditRevision != physicalRevision &&
                  !_isConfirmedStoreHistoryBase(before)) ||
              documentSession.selectedFrameIndex != frame ||
              documentSession.boardDocument == null ||
              !sameLitchcraftDocument(documentSession.boardDocument!, before)) {
            throw const LitchcraftResyncRequired();
          }
          final prior = _storeHistoryEcho;
          final common = _storeCommonAuthority!;
          final echo = (
            sourceGeneration: sourceGeneration,
            generation: common.generation,
            revision: (baseRevision + 1) & 0xffffffff,
            document: next,
            confirmed: false,
            previous:
                prior != null &&
                    prior.confirmed &&
                    prior.sourceGeneration == sourceGeneration &&
                    prior.generation == common.generation &&
                    prior.revision == baseRevision &&
                    sameLitchcraftDocument(prior.document, before)
                ? (
                    generation: prior.generation,
                    revision: prior.revision,
                    document: prior.document,
                  )
                : null,
          );
          _storeHistoryEcho = echo;
          final outcomes = await Future.wait(<Future<ConnectionSessionOutcome>>[
            ordered.sendOrdered(
              ConnectionSession.litchcraftStoreGuardOpcode,
              _storeGuardBody(ordered),
            ),
            ordered.sendOrdered(
              opcode == LitchcraftProtocol.undo
                  ? ConnectionSession.litchcraftUndoOpcode
                  : ConnectionSession.litchcraftRedoOpcode,
              Uint8List(0),
            ),
          ]);
          _appTrace?.record(
            'litchcraft.store_history_receipt',
            fields: <String, Object?>{
              'opcode': opcode,
              'guard_applied':
                  outcomes.first.disposition ==
                  ConnectionSessionDisposition.applied,
              'history_applied':
                  outcomes.last.disposition ==
                  ConnectionSessionDisposition.applied,
              'guard_reason': outcomes.first.reason,
              'history_reason': outcomes.last.reason,
              'revision': outcomes.last.domainRevision,
              'projected_live_revision': _projectedPhysicalStoreLiveRevision,
              'repair_through': _physicalStoreRepairThroughRevision,
            },
          );
          if (generation != _generation ||
              sourceGeneration != documentSession.replacementGeneration) {
            throw const LitchcraftTransferCancelled();
          }
          if (outcomes.any(
                (outcome) =>
                    outcome.disposition != ConnectionSessionDisposition.applied,
              ) ||
              outcomes.last.domainRevision !=
                  ((baseRevision + 1) & 0xffffffff) ||
              (_physicalStoreEditRevision != physicalRevision &&
                  _storeHistoryEcho != echo)) {
            throw const LitchcraftResyncRequired();
          }
          _acceptStoreEditRevision(outcomes.last.domainRevision);
          // A known structural inverse has the same exact live receipt as
          // other history. Durable generation converges through normal State.
          documentSession.confirm(deviceId, next);
          if (_storeHistoryEcho == echo &&
              (_projectedPhysicalStoreLiveRevision == echo.revision ||
                  layoutHistory &&
                      (_storeAuthority?.revision ?? 0) <= echo.revision)) {
            // Overflow can truncate a dense history echo. The exact known
            // inverse and its guarded terminal receipt confirm the whole result.
            _physicalStoreRepairThroughRevision = 0;
          }
          if (_storeHistoryEcho == echo) {
            _storeHistoryEcho = (
              sourceGeneration: echo.sourceGeneration,
              generation: _storeCommonAuthority!.generation,
              revision: echo.revision,
              document: echo.document,
              confirmed: true,
              previous: echo.previous,
            );
          }
          _canvas = documentSession.canvas.withTopology(_canvas.topology);
          _board = _canvas;
          _detached = _canvas;
          _offlineDiverged = false;
          historyResult = _storeConfirmedResult(
            before: before,
            document: next,
            selectedBefore: selectedFrameBeforeHistory,
            selectedAfter: frame,
            acceptedRevision: outcomes.last.domainRevision,
          );
          _notify();
        } catch (_) {
          _storeHistoryEcho = null;
          if (documentSession.generation == previewGeneration &&
              sameLitchcraftDocument(documentSession.document, next)) {
            if (opcode == LitchcraftProtocol.undo) {
              documentSession.redo();
            } else {
              documentSession.undo();
            }
            _canvas = documentSession.canvas.withTopology(_canvas.topology);
            _detached = _canvas;
            _notify();
          }
          rethrow;
        }
      });
    } finally {
      _finishStoreEditPosition(position, historyResult);
    }
  }

  Future<void> _retainStoreCanvasHistory(int opcode) async {
    final pending = _pendingStoreCanvas!;
    if (_optimisticDocumentEdits >= 128) {
      throw StateError('Board edits are backed up; draft preserved');
    }
    final predecessorPosition = _storeEditPosition;
    final position = _newStoreEditPosition();
    _StoreEditResult? historyResult;
    final barrier = Completer<void>();
    final predecessor = pending.followingHistory ?? pending.completed;
    _pendingStoreCanvas = (
      generation: pending.generation,
      sourceGeneration: pending.sourceGeneration,
      modeSession: pending.modeSession,
      session: pending.session,
      frame: pending.frame,
      mask: pending.mask,
      action: pending.action,
      effectEdits: BigInt.zero,
      applied: pending.applied,
      followingHistory: barrier,
      historyRevision: pending.historyRevision,
      completed: pending.completed,
      resume: pending.resume,
    );
    ++_optimisticDocumentEdits;
    // Preview this history intent now, before a later Canvas or stroke edits
    // the document. Its eventual inverse remains owned by the native journal.
    if (opcode == LitchcraftProtocol.undo) {
      documentSession.undo();
    } else {
      documentSession.redo();
    }
    _showStoreFrame(documentSession.selectedFrameIndex);
    _pauseStoreTimelineAtPhysicalFrame(documentSession.selectedFrameIndex);
    _detached = _canvas;
    _notify();
    Completer<void>? ready;
    var acceptedRevision = 0;
    bool current() =>
        !_disposed &&
        pending.generation == _generation &&
        pending.sourceGeneration == documentSession.replacementGeneration &&
        pending.modeSession == _documentSessionId &&
        identical(pending.session, _orderedSession) &&
        _selected &&
        _boardActive &&
        _status == LitchcraftStatus.live;
    void inspect() {
      if (!current()) {
        if (!barrier.isCompleted) {
          barrier.completeError(const LitchcraftTransferCancelled());
        }
        if (ready?.isCompleted == false) {
          ready!.completeError(const LitchcraftTransferCancelled());
        }
      } else if (ready?.isCompleted == false &&
          !_physicalStoreRepairing &&
          _canGuardStorePaint(pending.session) &&
          (acceptedRevision == 0 ||
              _storeCommonAuthority?.revision == acceptedRevision)) {
        ready!.complete();
      }
    }

    Future<void> coherent() async {
      ready = Completer<void>();
      inspect();
      if (!ready!.isCompleted) _schedulePhysicalStoreRepair();
      await ready!.future;
      ready = null;
    }

    addListener(inspect);
    unawaited(barrier.future.catchError((Object _) {}));
    final work = (() async {
      try {
        await predecessor.future;
        // Capture the last gesture before history. The mutable Canvas slot can
        // already name a later edit, so it cannot prove this history preimage.
        final precedingResult = await predecessorPosition?.completion.future;
        if (predecessorPosition == null ||
            !_storePositionCurrent(predecessorPosition) ||
            precedingResult == null) {
          throw const LitchcraftResyncRequired();
        }
        await coherent();
        if (!current()) throw const LitchcraftTransferCancelled();
        final before = documentSession.boardDocument!;
        if (!_sameStorePreimage(before, precedingResult.document) ||
            _storeCommonAuthority?.revision !=
                precedingResult.authority.revision) {
          throw const LitchcraftResyncRequired();
        }
        acceptedRevision = await _storeUnknownHistory(
          opcode,
          pending.session,
          pendingCanvasBase: before,
        );
        if (acceptedRevision == 0) throw const LitchcraftResyncRequired();
        final owner = _pendingStoreCanvas;
        if (owner != null) {
          _pendingStoreCanvas = (
            generation: owner.generation,
            sourceGeneration: owner.sourceGeneration,
            modeSession: owner.modeSession,
            session: owner.session,
            frame: owner.frame,
            mask: owner.mask,
            action: owner.action,
            effectEdits: owner.effectEdits,
            applied: owner.applied,
            followingHistory: owner.followingHistory,
            historyRevision: acceptedRevision,
            completed: owner.completed,
            resume: owner.resume,
          );
        }
        // A terminal receipt identifies the journal action, not its bytes.
        // Hold later intents until the normal reverse/saved-root repair proves
        // the actual coherent board result, including every metadata field.
        if ((_storeAuthority?.revision ?? 0) < acceptedRevision) {
          await _requestFreshStoreAuthority(pending.session);
        }
        await coherent();
        historyResult = _storeConfirmedResult(
          before: before,
          document: documentSession.boardDocument!,
          selectedBefore: precedingResult.selectedAfter,
          selectedAfter: _advancedSelectedFrame,
          acceptedRevision: acceptedRevision,
        );
        if (!barrier.isCompleted) barrier.complete();
      } catch (error) {
        if (!barrier.isCompleted) barrier.completeError(error);
        rethrow;
      } finally {
        _finishStoreEditPosition(position, historyResult);
        removeListener(inspect);
        --_optimisticDocumentEdits;
        if (identical(_pendingStoreCanvas?.followingHistory, barrier) &&
            _optimisticDocumentEdits == 0) {
          _pendingStoreCanvas = null;
        }
        if (current()) {
          _storePaintWaitingForAuthority = true;
          _resumeStorePaintIfReady(pending.session);
        }
        if (!_disposed) _notify();
      }
    })();
    // Timeout reports a still-retained intent; its existing listener/repair
    // owner keeps ordering until completion or source/lifetime cancellation.
    await work.timeout(_frameStoreResponseTimeout);
  }

  bool _canBorrowStoreHistoryRepair(ConnectionSession session) {
    final waiting = _storeHistoryRepairWait;
    return waiting != null &&
        !_disposed &&
        waiting.generation == _generation &&
        waiting.sourceGeneration == documentSession.replacementGeneration &&
        waiting.modeSession == _documentSessionId &&
        waiting.linkToken == _linkToken &&
        waiting.bootNonce == session.boardBootNonce &&
        waiting.deviceId == _deviceId &&
        identical(waiting.session, session) &&
        identical(session, _orderedSession) &&
        session.isActivated &&
        _selected &&
        _boardActive &&
        _status == LitchcraftStatus.live &&
        (waiting.revision != 0 || waiting.frame == _advancedSelectedFrame);
  }

  Future<int> _storeUnknownHistory(
    int opcode,
    ConnectionSession ordered, {
    LitchcraftAnimationDocument? pendingCanvasBase,
  }) async {
    final generation = _generation;
    final replacement = documentSession.replacementGeneration;
    final modeSession = _documentSessionId;
    final linkToken = _linkToken;
    final deviceId = _deviceId;
    final frame = _advancedSelectedFrame;
    final captured = documentSession.document;
    final capturedBoard = documentSession.boardDocument;
    final capturedAuthority = _storeAuthority;
    final bootNonce = ordered.boardBootNonce;
    final predecessor = _storeUnknownHistoryPosition;
    var predecessorRevision = 0;
    var coherentResult = false;
    final capturedCoherent =
        capturedBoard != null &&
        sameLitchcraftDocument(captured, capturedBoard);
    var acceptedRevision = 0;
    var admittedStoreGeneration = capturedAuthority?.generation ?? 0;
    var projectedResult = false;
    void recordProjection() => projectedResult = true;
    bool resultProved() => acceptedRevision != 0 && projectedResult;
    bool projectionRecorded() => projectedResult;
    Completer<void>? ready;
    bool sameLifetime() =>
        !_disposed &&
        generation == _generation &&
        replacement == documentSession.replacementGeneration &&
        modeSession == _documentSessionId &&
        linkToken == _linkToken &&
        deviceId == _deviceId &&
        identical(ordered, _orderedSession) &&
        ordered.isActivated &&
        ordered.boardBootNonce == bootNonce &&
        _selected &&
        _boardActive;
    bool current() => sameLifetime() && _status == LitchcraftStatus.live;
    bool ownsBase() =>
        capturedAuthority != null &&
        capturedAuthority.bootNonce == ordered.boardBootNonce &&
        capturedAuthority.deviceId == deviceId &&
        (_storeCommonAuthority?.revision == capturedAuthority.revision ||
            (predecessorRevision != 0 &&
                _storeCommonAuthority?.revision == predecessorRevision) ||
            _isConfirmedStoreHistoryBase(captured));
    bool provedResultAtCurrentCommon() =>
        acceptedRevision != 0 &&
        (_storeCommonAuthority?.revision == acceptedRevision ||
            (resultProved() &&
                (_storeCommonAuthority?.revision ?? 0) > acceptedRevision));
    recordWait(int revision) => (
      generation: generation,
      sourceGeneration: replacement,
      modeSession: modeSession,
      linkToken: linkToken,
      bootNonce: bootNonce,
      deviceId: deviceId!,
      session: ordered,
      frame: frame,
      revision: revision,
      storeGeneration: admittedStoreGeneration,
      recordProjection: recordProjection,
      resultProved: resultProved,
      projectionRecorded: projectionRecorded,
    );
    void inspect() {
      if (ready?.isCompleted != false) return;
      final expected = acceptedRevision == 0
          ? capturedAuthority?.revision
          : acceptedRevision;
      if (!sameLifetime()) {
        ready!.completeError(const LitchcraftTransferCancelled());
      } else if (_status == LitchcraftStatus.conflict) {
        // The same artwork now needs a choice; its transport remains valid.
        ready!.completeError(const LitchcraftResyncRequired());
      } else if (!current()) {
        ready!.completeError(const LitchcraftTransferCancelled());
      } else if (expected == null ||
          ((_storeCommonAuthority?.revision ?? 0) > expected &&
              (acceptedRevision != 0 ? !resultProved() : !ownsBase()))) {
        // A foreign successor cannot become either this journal action's
        // preimage or its result. Only the existing exact owned predecessor
        // proof can advance the captured pre-action live revision.
        ready!.completeError(const LitchcraftResyncRequired());
      } else if (!_physicalStoreRepairing &&
          _canGuardStorePaint(ordered) &&
          documentSession.boardDocument != null &&
          (acceptedRevision == 0
              ? ownsBase()
              : provedResultAtCurrentCommon())) {
        ready!.complete();
      }
    }

    Future<void> coherent() async {
      if (deviceId == null) throw const LitchcraftTransferCancelled();
      ready = Completer<void>();
      _storeHistoryRepairWait = recordWait(acceptedRevision);
      addListener(inspect);
      _notify();
      try {
        inspect();
        if (!ready!.isCompleted) {
          _storePaintWaitingForAuthority = true;
          _schedulePhysicalStoreRepair();
          if (_canBorrowStoreHistoryRepair(ordered)) {
            unawaited(_physicalStoreRepairResume?.call());
          }
        }
        await ready!.future;
      } finally {
        removeListener(inspect);
        _storeHistoryRepairWait = null;
        ready = null;
        _notify();
      }
    }

    await _enqueue(() async {
      if (pendingCanvasBase == null) {
        if (predecessor != null &&
            predecessor.generation == generation &&
            predecessor.sourceGeneration == replacement &&
            predecessor.modeSession == modeSession &&
            predecessor.linkToken == linkToken &&
            predecessor.bootNonce == ordered.boardBootNonce &&
            predecessor.deviceId == deviceId &&
            identical(predecessor.session, ordered)) {
          predecessorRevision = await predecessor.position;
        }
        await coherent();
      }
      final common = _storeCommonAuthority;
      if (!current() ||
          generation != _generation ||
          replacement != documentSession.replacementGeneration ||
          !_selected ||
          !_boardActive ||
          _status != LitchcraftStatus.live ||
          !identical(ordered, _orderedSession) ||
          !_canGuardStorePaint(ordered) ||
          common == null ||
          documentSession.boardDocument == null ||
          (pendingCanvasBase == null
              ? !ownsBase() ||
                    (!capturedCoherent &&
                        !sameLitchcraftDocument(
                          documentSession.document,
                          documentSession.boardDocument!,
                        ))
              : _pendingStoreCanvas?.followingHistory == null ||
                    _pendingStoreCanvas?.generation != generation ||
                    _pendingStoreCanvas?.sourceGeneration != replacement ||
                    _pendingStoreCanvas?.modeSession != _documentSessionId ||
                    !identical(_pendingStoreCanvas?.session, ordered) ||
                    !sameLitchcraftDocument(
                      pendingCanvasBase,
                      documentSession.boardDocument!,
                    ))) {
        throw const LitchcraftResyncRequired();
      }
      _appTrace?.record(
        'litchcraft.store_history_unknown_admission',
        fields: <String, Object?>{
          'opcode': opcode,
          'common_revision': common.revision,
          'can_guard': _canGuardStorePaint(ordered),
          'repair_through': _physicalStoreRepairThroughRevision,
        },
      );
      _storeHistoryEcho = null;
      documentSession.retireLocalHistory();
      admittedStoreGeneration = common.generation;
      _storeHistoryRepairWait = recordWait((common.revision + 1) & 0xffffffff);
      final outcomes =
          await Future.wait(<Future<ConnectionSessionOutcome>>[
            ordered.sendOrdered(
              ConnectionSession.litchcraftStoreGuardOpcode,
              _storeGuardBody(ordered),
            ),
            ordered.sendOrdered(
              opcode == LitchcraftProtocol.undo
                  ? ConnectionSession.litchcraftUndoOpcode
                  : ConnectionSession.litchcraftRedoOpcode,
              Uint8List(0),
            ),
          ]).whenComplete(() {
            if (identical(
              _storeHistoryRepairWait?.recordProjection,
              recordProjection,
            )) {
              _storeHistoryRepairWait = null;
            }
          });
      if (generation != _generation ||
          replacement != documentSession.replacementGeneration) {
        return;
      }
      if (outcomes.any(
            (outcome) =>
                outcome.disposition != ConnectionSessionDisposition.applied,
          ) ||
          outcomes.last.domainRevision !=
              ((common.revision + 1) & 0xffffffff)) {
        throw const LitchcraftResyncRequired();
      }
      acceptedRevision = outcomes.last.domainRevision;
      _appTrace?.record(
        'litchcraft.store_history_unknown_receipt',
        fields: <String, Object?>{
          'opcode': opcode,
          'revision': acceptedRevision,
          'applied': true,
        },
      );
      // The board publishes the inverse through the existing reverse stream.
      // A missing/unsupported patch repairs from a coherent saved root.
      if ((_storeCommonAuthority?.revision ?? 0) <
          outcomes.last.domainRevision) {
        _schedulePhysicalStoreRepair();
      }
      _notify();
      if (pendingCanvasBase == null) {
        // A journal terminal identifies its action, not an unknown preimage.
        // Keep this ordered position until normal reverse/saved-root repair
        // proves its exact result, preserving later local Canvas previews.
        await coherent();
        // A ready notification can race departure or a foreign successor.
        if (!sameLifetime()) return;
        if (!current() ||
            !_canGuardStorePaint(ordered) ||
            !provedResultAtCurrentCommon()) {
          throw const LitchcraftResyncRequired();
        }
        coherentResult = true;
      }
    });
    return pendingCanvasBase != null || coherentResult ? acceptedRevision : 0;
  }

  void _pushKnownAdvancedHistory(
    List<_KnownAdvancedHistory> history,
    _KnownAdvancedHistory entry,
  ) {
    history.add(entry);
    if (history.length > _knownAdvancedHistoryLimit) history.removeAt(0);
  }

  void _advanceKnownAdvancedHistory(
    List<_KnownAdvancedHistory> history,
    int revision,
  ) {
    if (history.isEmpty) return;
    final entry = history.last;
    history[history.length - 1] = _KnownAdvancedHistory(
      entry.before,
      entry.after,
      entry.session,
      revision,
      entry.generation,
    );
  }

  void _clearKnownAdvancedHistory() {
    _knownAdvancedUndo.clear();
    _knownAdvancedRedo.clear();
  }

  Future<void> replaceCanvas(LitchcraftCanvas value) async {
    endStroke();
    if (storeReplacementUnresolved) {
      documentSession.replaceCanvas(value);
      _canvas = value;
      _offlineDiverged = true;
      _detached = value;
      _notify();
      return;
    }
    final paintBoundary = _paintSequence;
    if (_status == LitchcraftStatus.live ||
        _status == LitchcraftStatus.conflict) {
      await _enqueue(
        () => _replaceLiveCanvas(value, paintBoundary: paintBoundary),
      );
    } else {
      _remember(_canvas);
      _canvas = value;
      _offlineDiverged = true;
      _detached = _canvas;
    }
    _notify();
  }

  Future<void> _replaceLiveCanvas(
    LitchcraftCanvas value, {
    int? paintBoundary,
  }) async {
    if (storeReplacementUnresolved) return;
    paintBoundary ??= _paintSequence;
    final sourceGeneration = documentSession.replacementGeneration;
    if (_advancedWire || documentSession.isAdvanced) {
      documentSession.replaceCanvas(value);
      if (_advancedWire) await _sendDocument();
      return;
    }
    if (_bulkReplace) {
      try {
        final ack = await _link!.sendBulkReplace(
          _sessionId,
          value.colors,
          (request) => _knownReplacementRequest = request,
        );
        if (sourceGeneration != documentSession.replacementGeneration) return;
        // Saved documents retain their original topology metadata, but the
        // live editor must expose controls for the connected physical board.
        final committed = value.withTopology(_board.topology);
        _board = committed;
        _canvas = committed;
        _revision = ack.revision;
        _stateRevision = ack.revision;
      } on LitchcraftBulkUnavailable {
        // This is a preflight-only refusal: staging never started, so the
        // deployed minimum-MTU protocol can safely own the same artwork.
        _bulkReplace = false;
      } finally {
        _knownReplacementRequest = null;
      }
    }
    if (!_bulkReplace) {
      await _link!.send(LitchcraftProtocol.replaceBegin, _sessionId);
      const chunkElements = 3;
      for (var offset = 0; offset < 256; offset += chunkElements) {
        await _link!.send(LitchcraftProtocol.replaceChunk, _sessionId, <int>[
          ...LitchcraftProtocol.u16(offset),
          ...value.colors
              .skip(offset)
              .take(chunkElements)
              .expand(LitchcraftProtocol.rgb),
        ]);
      }
      await _link!.send(LitchcraftProtocol.replaceCommit, _sessionId);
      await _pullSnapshot();
      if (sourceGeneration != documentSession.replacementGeneration) return;
      _canvas = _board;
    }
    _clearPendingPaint(throughSequence: paintBoundary);
    _commonCanvas = _board;
    _commonBoardId = _deviceId;
    _canvas = _withPendingPaint(_board);
    _offlineDiverged = false;
    _status = LitchcraftStatus.live;
  }

  Future<void> _resolvePendingFrameStoreConflict({
    required bool useBoard,
    bool preserveLocalPalette = false,
  }) async {
    final pending = _pendingFrameStoreConflict;
    if (pending == null) return;
    if (_disposed ||
        pending.generation != _generation ||
        pending.sourceGeneration != documentSession.replacementGeneration ||
        pending.bootNonce != pending.session.boardBootNonce ||
        pending.selectionIntent != _selectionIntent ||
        pending.linkToken != _linkToken ||
        !identical(pending.session, _orderedSession) ||
        pending.deviceId != _deviceId ||
        !bleService.isConnected ||
        !_boardActive) {
      return;
    }
    if (!useBoard) {
      retireConflictBoardPreview();
      await _transferFrameStore(
        frames: documentSession.document.frames,
        playbackOnEntry: documentSession.document.playbackOnEntry,
        replacementGeneration: documentSession.replacementGeneration,
        finishCurrentStroke: true,
      );
      if (_currentConflictPreview(pending)) {
        retireConflictBoardPreview();
        _pendingFrameStoreConflict = null;
        _frameStoreReadPending = false;
        _storeRootMissing = false;
        _errorMessage = null;
        _status = LitchcraftStatus.live;
        _notify();
      }
      return;
    }
    await prepareConflictBoardPreview();
    if (!_currentConflictPreview(pending)) return;
    if (!pending.previewFailed &&
        pending.previewSnapshot == null &&
        pending.previewFrame != null) {
      final existing = pending.documentWork;
      if (existing != null) {
        await existing;
      } else {
        late final Future<void> read;
        read =
            _readConflictBoardPreview(
                  pending,
                  pending.previewRetirement,
                  fullDocument: true,
                )
                .catchError((Object error) {
                  if (_currentConflictPreview(pending)) {
                    pending.previewSnapshot = null;
                    pending.previewFrame = null;
                    pending.previewAuthority = null;
                    pending.previewFailed = true;
                    _notify();
                  }
                  throw error;
                })
                .whenComplete(() {
                  if (identical(pending.documentWork, read)) {
                    pending.documentWork = null;
                    if (!_disposed) _notify();
                  }
                });
        pending.documentWork = read;
        _notify();
        await read;
      }
    }
    if (!_currentConflictPreview(pending)) return;
    final snapshot = pending.previewSnapshot;
    final authority = pending.previewAuthority;
    if (snapshot == null || authority == null || pending.previewFailed) {
      throw StateError('Board artwork preview unavailable. Try again.');
    }
    final client = _frameStoreClient(pending.session);
    final choiceRuntimeSequence = _connectionStateRevision;
    final currentState = await client.inspect(
      session: pending.authoritySession,
      request: _requestId = (_requestId % 0xffff) + 1,
    );
    final fresh = await _requestFreshStoreAuthority(pending.session);
    if (!_currentConflictPreview(pending)) return;
    if (!currentState.hasArtwork ||
        currentState.session != snapshot.session ||
        currentState.generation != snapshot.generation ||
        fresh == null ||
        (fresh.dirty && !_liveSnapshotMatchesAuthority(snapshot, fresh)) ||
        fresh.bootNonce != authority.bootNonce ||
        fresh.generation != authority.generation ||
        fresh.revision != authority.revision) {
      pending.previewSnapshot = null;
      pending.previewFrame = null;
      pending.previewAuthority = null;
      pending.previewFailed = true;
      _notify();
      throw StateError('Board artwork changed. Try again.');
    }
    Object? refusal;
    await _enqueue(() async {
      if (!_currentConflictPreview(pending)) return;
      try {
        if (!_verifiedConflictPreview(pending)) {
          throw StateError('Board artwork changed. Try again.');
        }
        if (_connectionStateRevision == choiceRuntimeSequence) {
          if (currentState.selectedFrame < snapshot.document.frames.length) {
            _advancedSelectedFrame = currentState.selectedFrame;
          }
          _frameStorePlaying = currentState.playing;
        }
        _frameStoreGeneration = currentState.generation;
        _boardActive = currentState.active;
        await _reconcileFrameStoreSnapshot(
          client: client,
          session: pending.session,
          state: currentState,
          deviceId: pending.deviceId,
          generation: pending.generation,
          selectionIntent: pending.selectionIntent,
          linkToken: pending.linkToken,
          sourceGeneration: pending.sourceGeneration,
          forceBoard: true,
          preserveLocalPalette: preserveLocalPalette,
          verifiedSnapshot: snapshot,
        );
      } on StateError catch (error) {
        // A same-source artwork refusal retains the chooser and connection.
        if (!_currentConflictPreview(pending)) return;
        pending.previewSnapshot = null;
        pending.previewFrame = null;
        pending.previewAuthority = null;
        pending.previewFailed = true;
        refusal = error;
        _notify();
      }
    });
    if (refusal != null) throw refusal!;
  }

  Future<void> resolveConflict({required bool useBoard}) async {
    if (_status != LitchcraftStatus.conflict) return;
    if (_pendingFrameStoreConflict != null) {
      await _resolvePendingFrameStoreConflict(useBoard: useBoard);
      return;
    }
    // A refused relative edit can reach conflict before a readback has made
    // its preview owner. It still belongs to Store; never fall through to
    // schema-3 replacement or the retired static snapshot path.
    if (usesFrameStoreDocument) {
      final session = _orderedSession;
      final deviceId = _deviceId;
      if (session == null || deviceId == null || !_linkUsable) return;
      retireConflictBoardPreview();
      _pendingFrameStoreConflict = _PendingFrameStoreConflict(
        session: session,
        authoritySession: litchcraftFrameStoreAuthoritySession(
          documentSession: _documentSessionId,
          modeSession: _sessionId,
        ),
        deviceId: deviceId,
        generation: _generation,
        selectionIntent: _selectionIntent,
        linkToken: _linkToken,
        sourceGeneration: documentSession.replacementGeneration,
        bootNonce: session.boardBootNonce,
      );
      _notify();
      await _resolvePendingFrameStoreConflict(useBoard: useBoard);
      if (useBoard && _pendingFrameStoreConflict == null) {
        _deliberateDeliveryGeneration = -1;
      }
      return;
    }
    if (canSaveArtworkToBoard) {
      if (!useBoard) {
        if (await saveArtworkToBoard()) _status = LitchcraftStatus.offline;
      } else {
        final client = _documentClient!;
        final generation = _generation;
        final sourceGeneration = documentSession.generation;
        await _enqueue(() async {
          final inspection = await _link!.inspectV2();
          if (_disposed || generation != _generation || inspection.active) {
            return;
          }
          final LitchcraftAnimationDocument board;
          if (inspection.schema == 0) {
            await _pullSnapshot(inspect: true);
            board = staticLitchcraftDocument(
              _board,
              favoriteColors: documentSession.document.favoriteColors,
            );
          } else {
            final snapshot = await client.snapshot(
              session: 0,
              maximumPacketBytes: _documentPacketBytes,
            );
            board = decodeLitchcraftAnimationDocument(
              snapshot.bytes,
              snapshot.crc32,
            );
          }
          if (_disposed ||
              generation != _generation ||
              sourceGeneration != documentSession.generation ||
              !canSaveArtworkToBoard) {
            return;
          }
          documentSession.observeBoard(_deviceId!, board);
          documentSession.acceptBoard(expectedGeneration: sourceGeneration);
          _status = LitchcraftStatus.offline;
        });
      }
      _notify();
      return;
    }
    if (_advancedWire) {
      if (!useBoard) {
        await _enqueue(() async {
          await _sendDocument();
        });
        return;
      }
      final sourceGeneration = documentSession.generation;
      final generation = _generation;
      await _documentSyncFuture;
      await _synchronizeDocument(reconcile: false);
      if (generation != _generation ||
          sourceGeneration != documentSession.generation) {
        return;
      }
      documentSession.acceptBoard(expectedGeneration: sourceGeneration);
      _status = LitchcraftStatus.live;
      _notify();
      return;
    }
    if (!useBoard) {
      await replaceCanvas(_canvas);
      return;
    }
    await _enqueue(() async {
      // The user may leave the conflict sheet open while painting physically.
      // Confirm the newest coherent board revision before adopting it.
      await _pullSnapshot();
      _clearPendingPaint();
      _canvas = _board;
      _status = LitchcraftStatus.live;
      _offlineDiverged = false;
    });
  }

  Future<LitchcraftCanvas> coherentSnapshot() async {
    if (_advancedWire) {
      await coherentDocument();
      return documentSession.canvas;
    }
    endStroke();
    if (bleService.isConnected &&
        _boardActive &&
        (_status == LitchcraftStatus.error ||
            _status == LitchcraftStatus.connecting)) {
      throw StateError('Reconnect artwork before saving the live canvas');
    }
    if (_status == LitchcraftStatus.live) {
      await _enqueue(_pullSnapshot);
      if (_status != LitchcraftStatus.live) {
        throw StateError('Could not confirm board artwork');
      }
    }
    return _canvas;
  }

  Future<void> _pullSnapshot({bool inspect = false}) async {
    for (var attempt = 0; attempt < 2; attempt++) {
      try {
        await _pullSnapshotOnce(inspect: inspect);
        if (!_needsResync) return;
      } on LitchcraftResyncRequired {
        if (attempt == 1) rethrow;
      }
    }
    throw StateError('Canvas changed before a coherent snapshot was received');
  }

  Future<void> _pullSnapshotOnce({bool inspect = false}) async {
    if (_snapshotDone != null) {
      await _snapshotDone!.future;
      return;
    }
    final done = Completer<void>();
    _snapshotDone = done;
    unawaited(
      done.future.then<void>(
        (_) {},
        onError: (Object error, StackTrace stack) {},
      ),
    );
    _snapshotStart = null;
    _snapshotReplySession = null;
    _snapshotDeltas.clear();
    _needsResync = false;
    _appTrace?.record(
      'litchcraft.snapshot_start',
      fields: <String, Object?>{
        'generation': _generation,
        'connection_epoch': _linkToken,
        'inspect': inspect,
        'efficient': _efficientSync,
      },
    );
    try {
      await _link!.send(
        LitchcraftProtocol.snapshot,
        inspect ? 0 : _sessionId,
        _efficientSync ? const [1] : const [],
        (id) => _snapshotRequest = id,
      );
      await done.future.timeout(const Duration(seconds: 20));
      _appTrace?.record(
        'litchcraft.snapshot_complete',
        fields: <String, Object?>{
          'generation': _generation,
          'connection_epoch': _linkToken,
          'request_id': _snapshotRequest,
          'session': _sessionId,
          'revision': _revision,
          'received_cells': _snapshotReceived?.where((value) => value).length,
        },
      );
    } catch (_) {
      // An inspection is metadata, not proof that the cached pixels belong to
      // its authority. Keep recovery on the snapshot path until a complete
      // SnapshotEnd atomically commits the new session and revision.
      _needsResync = true;
      _appTrace?.record(
        'litchcraft.snapshot_failed',
        fields: <String, Object?>{
          'generation': _generation,
          'connection_epoch': _linkToken,
          'request_id': _snapshotRequest,
          'session': _sessionId,
        },
      );
      rethrow;
    } finally {
      _snapshotIdleTimer?.cancel();
      _snapshotIdleTimer = null;
      if (identical(_snapshotDone, done)) _snapshotDone = null;
      _snapshotStart = null;
      _snapshotColors = null;
      _snapshotReceived = null;
    }
  }

  void _receive(LitchcraftPacket packet) {
    if (packet.event == LitchcraftProtocol.gesture) {
      _readGesture(packet);
      return;
    }
    if (packet.event == LitchcraftProtocol.colorPick) {
      // Runtime pick events use the active mode binding even when document
      // traffic is advanced or a newer paint State overtakes the lift result.
      final expectedSession = _advancedWire ? _documentSessionId : _sessionId;
      if (_linkUsable &&
          bleService.isConnected &&
          _boardActive &&
          _boardAuthorityLinkToken == _linkToken &&
          _linkToken == bleService.connectionSessionToken &&
          expectedSession != 0 &&
          packet.session == expectedSession) {
        _readColorPick(packet.payload[0]);
        _notify();
      }
      return;
    }
    // Schema-3 artwork still uses the compact brush event for physical tool
    // changes. Circle toggles paint/erase without changing the document, so
    // consume that runtime event even while the bulk document wire is active.
    if (_advancedWire &&
        packet.event == LitchcraftProtocol.brush &&
        packet.session == _documentSessionId) {
      _readBrush(packet);
      _notify();
      return;
    }
    if (_advancedWire || documentSession.isAdvanced) {
      // Enter/Exit always use the compact control wire, even for an animated
      // draft. Consume mode authority before filtering legacy canvas traffic.
      if (packet.event == LitchcraftProtocol.state && packet.payload[0] == 0) {
        if (packet.requestId == 0 &&
            packet.payload[1] & 1 != 0 &&
            (!_boardActive || packet.session != _sessionId) &&
            _linkUsable) {
          _beginPhysicalEntry(packet.session, packet.revision, advanced: true);
        } else if (packet.session == _sessionId ||
            _entering != null ||
            _status == LitchcraftStatus.connecting) {
          final wasActive = _boardActive;
          _readState(packet);
          _sessionId = packet.session;
          if (wasActive && !_boardActive && packet.requestId == 0) {
            // Compact mode State can arrive before the artwork State. Retire
            // pending entry and notify routing through the same exit owner.
            _acceptAdvancedPhysicalExit();
          }
        }
      }
      return;
    }
    if (packet.event == LitchcraftProtocol.snapshotBegin) {
      final String? rejection = _snapshotDone == null
          ? 'no_snapshot'
          : packet.requestId != _snapshotRequest
          ? 'wrong_request'
          : packet.session != _snapshotReplySession
          ? 'unacknowledged_session'
          : packet.session == _sessionId && packet.revision < _revision
          ? 'older_revision'
          : null;
      if (rejection != null) {
        _appTrace?.record(
          'litchcraft.snapshot_begin_rejected',
          fields: <String, Object?>{
            'reason': rejection,
            'generation': _generation,
            'connection_epoch': _linkToken,
            'request_id': packet.requestId,
            'expected_request_id': _snapshotRequest,
            'session': packet.session,
            'expected_session': _snapshotReplySession,
            'revision': packet.revision,
            'expected_revision': _revision,
          },
        );
        return;
      }
      final previous = _snapshotStart;
      if (previous != null &&
          packet.session == previous.session &&
          packet.revision == previous.revision &&
          packet.requestId == previous.requestId) {
        return;
      }
      _snapshotProgress();
      _snapshotStart = packet;
      _snapshotColors = List<int>.filled(256, 0);
      _snapshotReceived = List<bool>.filled(256, false);
      _snapshotDeltas.clear();
      _readState(packet);
      return;
    }
    final start = _snapshotStart;
    if (start != null &&
        packet.session == start.session &&
        packet.requestId == start.requestId &&
        packet.revision == start.revision) {
      if (packet.event == LitchcraftProtocol.snapshotChunk) {
        final offset = packet.payload[0] | packet.payload[1] << 8;
        for (var i = 2; i < packet.payload.length; i += 3) {
          final index = offset + (i - 2) ~/ 3;
          final value = LitchcraftProtocol.readRgb(packet.payload, i);
          if (_snapshotReceived![index] && _snapshotColors![index] != value) {
            _failSnapshot(const FormatException('Conflicting snapshot chunk'));
            return;
          }
          if (!_snapshotReceived![index]) _snapshotProgress();
          _snapshotColors![index] = value;
          _snapshotReceived![index] = true;
        }
        return;
      }
      if (packet.event == LitchcraftProtocol.brush) {
        _readBrush(packet);
        return;
      }
      if (packet.event == LitchcraftProtocol.snapshotEnd) {
        if (!_snapshotReceived!.every((value) => value)) {
          _failSnapshot(const FormatException('Incomplete snapshot'));
        } else {
          _board = LitchcraftCanvas(
            colors: _snapshotColors!,
            topology: LitchcraftTopology.fromId(start.payload[2]),
          );
          _sessionId = start.session;
          _revision = start.revision;
          _snapshotStart = null;
          for (final delta in _snapshotDeltas.toList()) {
            _applyDelta(delta);
          }
          if (!_offlineDiverged && _status != LitchcraftStatus.conflict) {
            _canvas = _status == LitchcraftStatus.live
                ? _withPendingPaint(_board)
                : _board;
          }
          if (_snapshotDone?.isCompleted == false) _snapshotDone!.complete();
          _notify();
        }
        return;
      }
    }
    if (packet.event == LitchcraftProtocol.resyncRequired) {
      if (packet.session != _sessionId) return;
      _clearPhysicalColorPick();
      if (_snapshotDone != null) {
        _failSnapshot(const LitchcraftResyncRequired());
      }
      if (_queued == 0) _scheduleResync();
      return;
    }
    if (packet.event == LitchcraftProtocol.state) {
      if (_snapshotDone != null &&
          packet.requestId == _snapshotRequest &&
          packet.payload[0] == 0) {
        _snapshotReplySession = packet.session;
        _snapshotProgress();
        _appTrace?.record(
          'litchcraft.snapshot_authority',
          fields: <String, Object?>{
            'generation': _generation,
            'connection_epoch': _linkToken,
            'request_id': packet.requestId,
            'session': packet.session,
            'revision': packet.revision,
          },
        );
      }
      final bool physicalEntry =
          packet.requestId == 0 &&
          packet.payload[0] == 0 &&
          (packet.payload[1] & LitchcraftProtocol.stateActive) != 0 &&
          !_boardActive &&
          _linkUsable &&
          packet.session != 0 &&
          packet.session != _sessionId;
      if (physicalEntry) {
        _readState(packet);
        _beginPhysicalEntry(packet.session, packet.revision);
        return;
      }
      if (_status == LitchcraftStatus.connecting ||
          _entering != null ||
          packet.session == _sessionId) {
        _readState(packet);
      }
      _notify();
      return;
    }
    if (start != null) {
      if (packet.session == start.session && packet.revision > start.revision) {
        if (_snapshotDeltas.length >= 512) {
          _failSnapshot(StateError('Snapshot change stream overflow'));
        } else {
          _snapshotDeltas.add(packet);
        }
      }
      return;
    }
    _applyDelta(packet);
    _notify();
  }

  void _beginPhysicalEntry(int session, int revision, {bool advanced = false}) {
    if (_disposed || !bleService.isConnected) return;
    if (advanced) _retireChangedStoreLineage(_documentSessionId, session);
    _cancelPendingStoreCommit();
    _cancelEntryDraftWait();
    _entryDraftWaitCancellation = Completer<void>();
    _recoverNotificationRepair();
    final previousEntry = _entering;
    // New physical ownership retires reads and queued commands from the old
    // session. Its State is authority immediately; reconciliation then shares
    // the existing command lane after canceled work releases its lease.
    ++_generation;
    _documentClient?.cancel();
    _failSnapshot(const LitchcraftTransferCancelled());
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    _recoveryAttempt = 0;
    ++_selectionIntent;
    _selected = true;
    _boardActive = true;
    _boardAuthorityLinkToken = _linkToken;
    _recordInspectedMode(true);
    _detached = _canvas;
    _sessionId = session;
    if (advanced) {
      _advancedWire = true;
      _documentSessionId = session;
      _documentRevision = revision;
      _documentDirty = true;
    } else {
      _revision = revision;
      _stateRevision = revision;
    }
    _status = LitchcraftStatus.connecting;
    _errorMessage = null;
    final intent = _selectionIntent;
    final linkToken = _linkToken;
    late final Future<void> adoption;
    adoption =
        (() async {
          await previousEntry;
          await _finishPhysicalEntry(session, intent, linkToken);
        })().whenComplete(() {
          if (identical(_entering, adoption)) _entering = null;
          _notify();
        });
    _entering = adoption;
    _notify();
  }

  Future<void> _finishPhysicalEntry(
    int session,
    int intent,
    int linkToken,
  ) async {
    try {
      await _tail;
      if (_disposed ||
          intent != _selectionIntent ||
          linkToken != _linkToken ||
          linkToken != bleService.connectionSessionToken ||
          !_selected ||
          !_boardActive ||
          _sessionId != session) {
        return;
      }
      await _enter(resuming: true, activeOnly: true, draftReady: _draftReady);
    } catch (error) {
      if (intent == _selectionIntent && linkToken == _linkToken) {
        _transportFailed(error);
      }
    }
  }

  void _readState(LitchcraftPacket packet) {
    if (packet.payload[0] != 0) return;
    if (packet.session == _sessionId &&
        packet.revision <
            (_stateRevision > _revision ? _stateRevision : _revision)) {
      return;
    }
    _stateRevision = packet.revision;
    _boardAuthorityLinkToken = _linkToken;
    final int flags = packet.payload[1];
    _boardActive = (flags & LitchcraftProtocol.stateActive) != 0;
    _recordInspectedMode(_boardActive);
    _waitingForLift = (flags & LitchcraftProtocol.stateWaitingForLift) != 0;
    _liveUndo = packet.payload[3] != 0;
    _liveRedo = packet.payload[4] != 0;
    if (_acceptBrushRevision(packet)) {
      _confirmedBrush = (
        color: _confirmedBrush?.color ?? _color,
        eraser: packet.payload[5] != 0,
        targetMask: packet.payload[6],
        project: _confirmedBrush?.project ?? false,
      );
      if (_boardActive && !_brushPreviewWriteQueued && _pendingBrush == null) {
        _eraser = packet.payload[5] != 0;
        _targetMask = packet.payload[6];
      }
    }
    if (!_boardActive &&
        (_status == LitchcraftStatus.live ||
            _status == LitchcraftStatus.conflict ||
            (_status == LitchcraftStatus.connecting &&
                packet.requestId == 0))) {
      _status = LitchcraftStatus.offline;
      cancelBrushColorPreview();
      _clearPhysicalColorPick();
      _selected =
          false; // Deliberate board exit must not be undone by auto-entry.
      // Exit advances the revision but cannot alter artwork. Consume that
      // authoritative successor so returning later does not invent a canvas
      // resynchronization requirement.
      _revision = packet.revision;
      _stateRevision = packet.revision;
      _needsResync = false;
    }
    if (!_advancedWire &&
        !documentSession.isAdvanced &&
        packet.event == LitchcraftProtocol.state &&
        packet.session == _sessionId &&
        packet.revision > _revision &&
        packet.requestId != _knownReplacementRequest &&
        _snapshotDone == null) {
      _scheduleResync();
    }
  }

  bool _acceptBrushRevision(LitchcraftPacket packet) {
    if (_brushLinkToken != _linkToken || _brushSession != packet.session) {
      _brushLinkToken = _linkToken;
      _brushSession = packet.session;
      _brushRevision = packet.revision;
      _confirmedBrush = null;
      return true;
    }
    if (packet.revision != _brushRevision &&
        !_newerDocumentRevision(packet.revision, _brushRevision)) {
      return false;
    }
    _brushRevision = packet.revision;
    return true;
  }

  void _projectConfirmedBrush() {
    final brush = _confirmedBrush;
    if (brush == null ||
        !brush.project ||
        !_boardActive ||
        _brushPreviewWriteQueued ||
        (usesFrameStoreDocument && _optimisticDocumentEdits > 0) ||
        _pendingBrush != null ||
        _colorPreviewOrigin != null ||
        _brushSession != _sessionId ||
        _brushLinkToken != _linkToken) {
      return;
    }
    _color = brush.color;
    _eraser = brush.eraser;
    _targetMask = brush.targetMask;
    _notify();
  }

  void _readBrush(LitchcraftPacket packet) {
    if ((_orderedSession?.stateStreamToken ?? 0) != 0) return;
    if (!_boardActive ||
        packet.session != _sessionId ||
        !_acceptBrushRevision(packet)) {
      return;
    }
    ++_brushObservation;
    _confirmedBrush = (
      color: LitchcraftProtocol.readRgb(packet.payload, 0),
      eraser: packet.payload[3] != 0,
      targetMask: packet.payload[4],
      project: packet.requestId == 0,
    );
    _projectConfirmedBrush();
  }

  void _readColorPick(int phase) {
    if (phase == LitchcraftProtocol.colorPickPicking) {
      _physicalColorPickedTimer?.cancel();
      _physicalColorPickedTimer = null;
      _physicalColorPicking = true;
      _physicalColorPicked = false;
      _physicalColorPickedLatched = false;
      return;
    }
    _physicalColorPicking = false;
    if (phase == LitchcraftProtocol.colorPickPicked) {
      if (_physicalColorPicked) return;
      _physicalColorPicked = true;
      _physicalColorPickedLatched = true;
      _physicalColorPickedTimer?.cancel();
      _physicalColorPickedTimer = Timer(_physicalColorPickedDuration, () {
        _physicalColorPickedTimer = null;
        if (_disposed) return;
        _physicalColorPickedLatched = false;
        _notify();
      });
    }
    if (phase == LitchcraftProtocol.colorPickIdle) {
      _physicalColorPicked = false;
    }
    // Idle cancels authoritative progress immediately but deliberately leaves
    // a completed result latched for its accessibility/readability window.
  }

  void _clearPhysicalColorPick() {
    _physicalColorPickedTimer?.cancel();
    _physicalColorPickedTimer = null;
    _physicalColorPicking = false;
    _physicalColorPicked = false;
    _physicalColorPickedLatched = false;
  }

  void _readGesture(LitchcraftPacket packet) {
    if (packet.payload[0] != LitchcraftProtocol.gestureShakeErase ||
        !_shakeEraseSupported ||
        !_linkUsable ||
        !bleService.isConnected ||
        !_boardActive ||
        _status != LitchcraftStatus.live ||
        _boardAuthorityLinkToken != _linkToken) {
      return;
    }
    final int expectedSession = _advancedWire ? _documentSessionId : _sessionId;
    final int currentRevision = _advancedWire ? _documentRevision : _revision;
    final int expectedRevision = (currentRevision + 1) & 0xffffffff;
    final int token = packet.payload[1] | packet.payload[2] << 8;
    if (packet.session != expectedSession ||
        packet.revision != expectedRevision ||
        (_lastShakeEraseLinkToken == _linkToken &&
            _lastShakeEraseSession == packet.session &&
            _lastShakeEraseToken == token)) {
      return;
    }
    _lastShakeEraseLinkToken = _linkToken;
    _lastShakeEraseSession = packet.session;
    _lastShakeEraseToken = token;
    _physicalShakeEraseTimer?.cancel();
    _physicalShakeErased = true;
    _physicalShakeEraseTimer = Timer(_physicalShakeEraseDuration, () {
      _physicalShakeEraseTimer = null;
      if (_disposed) return;
      _physicalShakeErased = false;
      _notify();
    });
    if (_advancedWire) {
      _observeAdvancedRevisionWatermark(packet.revision);
    } else {
      _scheduleResync();
    }
    _notify();
  }

  void _clearPhysicalShakeErase({bool resetDedupe = false}) {
    _physicalShakeEraseTimer?.cancel();
    _physicalShakeEraseTimer = null;
    _physicalShakeErased = false;
    if (resetDedupe) {
      _lastShakeEraseToken = 0;
      _lastShakeEraseSession = 0;
      _lastShakeEraseLinkToken = -1;
    }
  }

  void _applyDelta(LitchcraftPacket packet) {
    if (packet.session != _sessionId || packet.revision <= _revision) return;
    if (packet.revision != _revision + 1) {
      _scheduleResync();
      return;
    }
    if (packet.event == LitchcraftProtocol.cell) {
      final payload = packet.payload;
      final color = LitchcraftProtocol.readRgb(payload, 2);
      _board = _board.paint([payload[0]], payload[1], color);
      if (_status == LitchcraftStatus.live && !_offlineDiverged) {
        _canvas = _withPendingPaint(_board);
      }
      if (packet.requestId == 0) ++_boardMarkCount;
    } else if (packet.event == LitchcraftProtocol.brush) {
      _readBrush(packet);
    } else {
      return;
    }
    _revision = packet.revision;
  }

  void _scheduleResync() {
    if (usesFrameStoreDocument) return;
    _needsResync = true;
    if (_link == null || _status != LitchcraftStatus.live) return;
    if (_resyncQueued || _queued > 0 || _snapshotDone != null) return;
    _resyncQueued = true;
    unawaited(
      _enqueue(_pullSnapshot).whenComplete(() {
        _resyncQueued = false;
      }),
    );
  }

  void _preservePendingStaticCanvas() {
    final pending = _pendingStaticCanvas;
    if (pending == null) return;
    _clearPendingPaint(throughSequence: pending.paintBoundary);
    _canvas = _withPendingPaint(pending.canvas);
    _pendingStaticCanvas = null;
    _offlineDiverged = true;
    _detached = _canvas;
  }

  void _transportFailed(Object error) {
    _preservePendingStaticCanvas();
    if (_disposed) return;
    cancelBrushColorPreview();
    _cancelPendingStoreCommit();
    // BLE owns the shared session. Native/stream failures retire it there;
    // an artwork refusal must not end the healthy connection used by Play,
    // Setup and other domains. Release only this controller's borrowed handle.
    _orderedSession = null;
    final retiredState = _connectionState;
    _connectionState = null;
    _connectionStateRevision = 0;
    _connectionStateObservedAt = 0;
    _connectionPlaybackStateRevision = 0;
    unawaited(retiredState?.cancel());
    // Retire the whole command generation, including queued but unsent taps.
    // Otherwise they can replace the witness for the one unacknowledged write.
    ++_generation;
    final old = _link;
    _link = null;
    _linkUsable = false;
    _clearTrustedExit();
    unawaited(_disposeLink(old).catchError((Object _) {}));
    _offlineDiverged = _offlineDiverged || !_sameArtwork(_canvas, _board);
    _detached = _canvas;
    _failSnapshot(error);
    _errorMessage = error.toString();
    _status = bleService.isConnected
        ? LitchcraftStatus.error
        : LitchcraftStatus.offline;
    _clearPhysicalColorPick();
    _clearPhysicalShakeErase();
    _appTrace?.record(
      'litchcraft.transport_failed',
      fields: <String, Object?>{
        'generation': _generation,
        'kind': error.runtimeType.toString(),
        if (error is LitchcraftRejected) 'status': error.status,
      },
    );
    _notify();
    _scheduleRecovery(notificationRepair: true);
  }

  void _scheduleRecovery({bool notificationRepair = false}) {
    if (_disposed ||
        _status == LitchcraftStatus.boardNotEmpty ||
        (!_selected &&
            !_inactiveStorageSupported &&
            _exitOnReconnect == null) ||
        !bleService.isConnected ||
        _recoveryAttempt >= _recoveryDelays.length ||
        _recoveryTimer != null) {
      return;
    }
    if (notificationRepair && !_notificationRepairActive) {
      _notificationRepairActive = true;
      bleService.recordNotificationRepairOperation();
    }
    // Transient failures repair silently. Repeating the same exhausted repair
    // forever cannot establish authority; retain the draft and expose the
    // existing connection action after this bounded sequence fails.
    final int delayMs = _recoveryDelays[_recoveryAttempt];
    _appTrace?.record(
      'litchcraft.recovery_scheduled',
      fields: <String, Object?>{
        'attempt': _recoveryAttempt + 1,
        'delay_ms': delayMs,
        'generation': _generation,
      },
    );
    _recoveryTimer = Timer(Duration(milliseconds: delayMs), () {
      _recoveryTimer = null;
      if (_disposed ||
          !bleService.isConnected ||
          _status == LitchcraftStatus.boardNotEmpty) {
        _cancelNotificationRepair();
        return;
      }
      ++_recoveryAttempt;
      if (_notificationRepairActive) {
        bleService.recordNotificationRepairAttempt();
      }
      if (_exitOnReconnect != null) {
        unawaited(_startPendingExit());
      } else if (_selected) {
        unawaited(_startEnter(resuming: _boardActive));
      } else if (_inactiveStorageSupported) {
        unawaited(adoptActiveBoard());
      } else {
        _cancelNotificationRepair();
      }
    });
  }

  void _settleNotificationRepair() {
    if (!_notificationRepairActive || _recoveryTimer != null) return;
    final bool authoritativeUnsupported =
        _status == LitchcraftStatus.unsupported &&
        _deviceId != null &&
        bleService.deviceCapabilities(_deviceId!).explicitlyReported;
    final bool recovered =
        _status == LitchcraftStatus.live ||
        _status == LitchcraftStatus.conflict ||
        _status == LitchcraftStatus.gameActive ||
        _status == LitchcraftStatus.boardNotEmpty ||
        authoritativeUnsupported;
    final bool exhausted =
        _recoveryAttempt >= _recoveryDelays.length &&
        (_status == LitchcraftStatus.error ||
            _status == LitchcraftStatus.connecting ||
            (_status == LitchcraftStatus.unsupported &&
                !authoritativeUnsupported));
    if (exhausted) {
      _notificationRepairActive = false;
      bleService.recordNotificationRepairExhausted();
      return;
    }
    if (recovered) {
      _recoverNotificationRepair();
      return;
    }
    if (_disposed ||
        !_selected ||
        !bleService.isConnected ||
        _status == LitchcraftStatus.offline) {
      _cancelNotificationRepair();
    }
  }

  void _recoverNotificationRepair() {
    if (!_notificationRepairActive) return;
    _notificationRepairActive = false;
    bleService.recordNotificationRepairRecovered();
  }

  void _cancelNotificationRepair() {
    if (!_notificationRepairActive) return;
    _notificationRepairActive = false;
    bleService.recordNotificationRepairCancelled();
  }

  void _retryEmptyBoard() {
    final home = bleService.latestHomeState;
    // A rejected empty snapshot is not a new removal event merely because a
    // heartbeat has a newer sequence. Re-arm after actual occupied evidence.
    if (home != null && home.physicalStatus.occupiedSquares != BigInt.zero) {
      _emptyAdmissionSequence = null;
    }
    if (_disposed ||
        !_selected ||
        _status != LitchcraftStatus.boardNotEmpty ||
        _entering != null ||
        !bleService.isConnected ||
        home == null ||
        !home.freshSetup ||
        _emptyAdmissionSequence != null ||
        home.physicalStatus.placementMode != true ||
        home.physicalStatus.occupiedSquares != BigInt.zero) {
      return;
    }
    _recoveryTimer?.cancel();
    _recoveryTimer = Timer(const Duration(milliseconds: 150), () {
      _recoveryTimer = null;
      final latest = bleService.latestHomeState;
      if (!_selected ||
          _disposed ||
          !bleService.isConnected ||
          _status != LitchcraftStatus.boardNotEmpty ||
          latest == null ||
          !latest.freshSetup ||
          latest.physicalStatus.placementMode != true ||
          latest.physicalStatus.occupiedSquares != BigInt.zero) {
        return;
      }
      unawaited(_startEnter());
      _emptyAdmissionSequence = latest.sequence;
    });
  }

  void _snapshotProgress() {
    if (_snapshotDone == null || _snapshotDone!.isCompleted) return;
    _snapshotIdleTimer?.cancel();
    _snapshotIdleTimer = Timer(_snapshotIdleTimeout, () {
      _failSnapshot(TimeoutException('Artwork snapshot stopped arriving'));
    });
  }

  void _failSnapshot(Object error) {
    if (_snapshotDone?.isCompleted == false) {
      _snapshotDone!.completeError(error);
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    _cancelNotificationRepair();
    _cancelPendingStoreCommit();
    _disposed = true;
    _storeToolEditPosition = null;
    _retireFrameStoreTransfer();
    notifyListeners();
    final canvas = _pendingStoreCanvas;
    if (canvas != null && !canvas.completed.isCompleted) {
      canvas.completed.completeError(const LitchcraftTransferCancelled());
    }
    retireConflictBoardPreview();
    _pendingFrameStoreConflict = null;
    if (identical(_frameStoreDiagnosticOwner, this)) {
      _frameStoreDiagnosticOwner = null;
    }
    _cancelEntryDraftWait();
    documentSession.removeListener(_documentChanged);
    _documentClient?.cancel();
    _clearCachedFrameStoreInspection();
    _knownFrameStoreResidents.clear();
    _selectedFrameStoreRootSlot = null;
    _clearAdvancedTimeline();
    _frameStoreContentIdentities.clear();
    _frameStoreSnapshotRead = null;
    _snapshotIdleTimer?.cancel();
    _physicalStoreRepairTimer?.cancel();
    _artworkDeliveryTimer?.cancel();
    _recoveryTimer?.cancel();
    _physicalColorPickedTimer?.cancel();
    _physicalShakeEraseTimer?.cancel();
    ++_generation;
    WidgetsBinding.instance.removeObserver(this);
    unawaited(_connection.cancel());
    unawaited(_home.cancel());
    unawaited(_connectionState?.cancel());
    final link = _link;
    _link = null;
    unawaited(_disposeLink(link).catchError((Object _) {}));
    super.dispose();
  }

  void _cancelEntryDraftWait() {
    final cancellation = _entryDraftWaitCancellation;
    if (cancellation != null && !cancellation.isCompleted) {
      cancellation.complete();
    }
  }
}
