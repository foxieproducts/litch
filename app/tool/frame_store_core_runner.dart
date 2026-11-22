import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';

import 'frame_store_core_backup.dart';

void validateCoreTarget(String platform, String target, String digest) {
  final validTarget = switch (platform) {
    'android' => RegExp(r'^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$').hasMatch(target),
    'ios' => RegExp(
      r'^[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}$',
    ).hasMatch(target),
    _ => false,
  };
  if (!validTarget || !RegExp(r'^[0-9a-f]{64}$').hasMatch(digest)) {
    throw ArgumentError(
      'Explicit Android uppercase MAC or iOS UUID and exact lowercase SHA-256 required',
    );
  }
}

class CoreUnsupportedBaseline implements Exception {
  CoreUnsupportedBaseline(this.reason);
  final String reason;
  @override
  String toString() => 'Unsupported protection baseline: $reason';
}

class CoreAuthority {
  CoreAuthority(this.boot, this.generation, this.revision, this.dirty);
  factory CoreAuthority.decode(ConnectionSessionPacket packet, int boot) {
    if (packet.code != ConnectionSession.litchcraftStoreAuthorityStateCode ||
        packet.body.length != 10 ||
        packet.body[0] != 1 ||
        (packet.body[1] & ~1) != 0) {
      throw const FormatException('Malformed Store State37 authority');
    }
    final bytes = ByteData.sublistView(packet.body);
    return CoreAuthority(
      boot,
      bytes.getUint32(2, Endian.little),
      bytes.getUint32(6, Endian.little),
      (packet.body[1] & 1) != 0,
    );
  }
  final int boot, generation, revision;
  final bool dirty;
  bool same(CoreAuthority other) =>
      boot == other.boot &&
      generation == other.generation &&
      revision == other.revision &&
      dirty == other.dirty;
  Map<String, Object> toJson() => {
    'boot': boot,
    'generation': generation,
    'revision': revision,
  };
}

class CoreBoardView {
  CoreBoardView(
    this.state,
    this.authority, {
    required this.elapsedMs,
    required this.history,
  });
  final LitchcraftFrameStoreState state;
  final CoreAuthority authority;
  final int elapsedMs;
  final bool history;
  void requireBaseline({bool backupArtwork = true}) {
    if (!state.active ||
        state.session == 0 ||
        authority.dirty ||
        authority.generation != state.generation ||
        authority.revision == 0xffffffff ||
        (backupArtwork &&
            (!state.hasArtwork ||
                authority.revision == 0 ||
                state.playing ||
                state.selectedFrame != 0 ||
                elapsedMs != 0 ||
                history))) {
      throw CoreUnsupportedBaseline(
        backupArtwork
            ? 'Use active, clean, stable Litchcraft at paused frame 0 with zero elapsed phase and no Undo/Redo history. History and elapsed phase cannot be restored.'
            : 'Use active, clean, stable Litchcraft authority for the transfer test.',
      );
    }
  }

  Map<String, Object?> toJson() => {
    ...authority.toJson(),
    'root': state.selectedRootSlot,
  };
  bool matches(Map fence) =>
      authority.boot == fence['boot'] &&
      state.generation == fence['generation'] &&
      authority.revision == fence['revision'] &&
      state.selectedRootSlot == fence['root'];
}

class CoreSendEvidence {
  CoreSendEvidence(this.result, this.fields);
  final LitchcraftFrameStoreTransferResult? result;
  final Map<String, Object?> fields;
}

/// This boundary is implemented with the production BLE/session/Store clients.
/// Sequencer fixtures support preservation tests; they are not physical proof.
abstract interface class CoreBoard {
  Future<CoreBoardView> read();
  Future<LitchcraftFrameStoreSnapshot> snapshot(CoreBoardView view);
  Future<CoreBoardView> select(LitchcraftFrameStoreResident resident);
  Future<CoreSendEvidence> send(
    LitchcraftAnimationDocument document,
    CoreBoardView view, {
    bool interrupt = false,
    bool nonresident = false,
  });
  Future<void> reconnect();
}

class CoreSendFailure implements Exception {
  CoreSendFailure(this.error, {required this.commitSubmitted});
  final Object error;
  final bool commitSubmitted;
  @override
  String toString() =>
      'Store transfer failed (Commit submitted: $commitSubmitted): $error';
}

typedef CoreEmit = void Function(String event, Map<String, Object?> fields);

class CoreRunResult {
  CoreRunResult(
    this.verifiedUploads,
    this.testFailure,
    this.restorationFailure,
  );
  final int verifiedUploads;
  final Object? testFailure, restorationFailure;
  bool get passed =>
      verifiedUploads == 5 && testFailure == null && restorationFailure == null;
}

class CoreProtectedRunner {
  CoreProtectedRunner(this.board, this.backup, this.emit);
  final CoreBoard board;
  final CoreBackup backup;
  final CoreEmit emit;
  int verifiedUploads = 0;
  Map<String, dynamic> get _saved => backup.manifest;

  Future<void> _saveFence(CoreBoardView view) async {
    view.requireBaseline(backupArtwork: backup.preservesArtwork);
    _saved['fence'] = view.toJson();
    await backup.checkpoint();
  }

  Future<CoreBoardView> _guard() async {
    final view = await board.read();
    view.requireBaseline(backupArtwork: backup.preservesArtwork);
    final fence = _saved['fence'] as Map;
    final pending = _saved['pending'] as Map?;
    if (pending?['kind'] == 'transfer' && pending?['outcome'] == 'unknown') {
      throw StateError(
        'Unknown admitted Commit outcome; retain run evidence. No further writes authorized.',
      );
    }
    if (!view.matches(fence)) {
      // A saved selection intent does not prove who applied it. Only the fence
      // persisted after a validated owned terminal may authorize changed state.
      throw StateError(
        pending?['kind'] == 'selection'
            ? 'Unresolved resident selection changed authority; restoration pending. Preserve external backups.'
            : 'Foreign/changed authority; retaining run evidence without further writes',
      );
    }
    return view;
  }

  static Map<String, Object> _resident(LitchcraftFrameStoreResident resident) =>
      {
        'rootSlot': resident.rootSlot,
        'generation': resident.generation,
        'bodyCrc32': resident.bodyCrc32,
        'bodyBytes': resident.bodyBytes,
      };
  static bool _sameResident(LitchcraftFrameStoreResident resident, Map saved) =>
      resident.rootSlot == saved['rootSlot'] &&
      resident.generation == saved['generation'] &&
      resident.bodyCrc32 == saved['bodyCrc32'] &&
      resident.bodyBytes == saved['bodyBytes'];

  Future<CoreBoardView> _select(LitchcraftFrameStoreResident resident) async {
    final before = await _guard();
    _saved['pending'] = {'kind': 'selection', 'resident': _resident(resident)};
    await backup.checkpoint();
    final selected = await board.select(resident);
    selected.requireBaseline();
    if (selected.state.selectedRootSlot != resident.rootSlot ||
        selected.state.generation != resident.generation ||
        selected.authority.boot != (_saved['fence'] as Map)['boot'] ||
        selected.authority.revision != before.authority.revision + 1) {
      throw StateError('Resident selection changed expected authority');
    }
    await _saveFence(selected);
    _saved.remove('pending');
    await backup.checkpoint();
    return selected;
  }

  Future<LitchcraftAnimationDocument> _snapshot(CoreBoardView view) async {
    if (view.state.playing ||
        view.state.selectedFrame != 0 ||
        view.elapsedMs != 0) {
      throw StateError(
        'Full verification requires the uploaded artwork paused at frame 0',
      );
    }
    final snapshot = await board.snapshot(view);
    final after = await _guard();
    if (!view.authority.same(after.authority) ||
        after.state.playing ||
        after.state.selectedFrame != 0 ||
        after.elapsedMs != 0 ||
        snapshot.session != view.state.session ||
        snapshot.generation != view.state.generation ||
        !snapshot.active ||
        snapshot.playing ||
        snapshot.selectedFrame != 0) {
      throw StateError('Snapshot authority changed or is not paused frame 0');
    }
    return snapshot.document;
  }

  Future<void> _collect() async {
    await backup.validate();
    final initial = await board.read();
    initial.requireBaseline(backupArtwork: backup.preservesArtwork);
    if (!backup.preservesArtwork) {
      await _saveFence(initial);
      _saved['stage'] = 'ready';
      await backup.checkpoint();
      emit('test_authority_ready', {'path': backup.directory.path});
      return;
    }
    if (initial.state.residents.length != 2 ||
        initial.state.selectedRootSlot == null ||
        initial.state.residents.any(
          (resident) => !resident.active || resident.topology != 3,
        )) {
      throw CoreUnsupportedBaseline(
        'Exactly two populated resident roots required. Empty-slot deletion is unavailable; resident inventory must be observed on the QA board before this tool is ready.',
      );
    }
    final original = initial.state.residents.singleWhere(
      (r) => r.rootSlot == initial.state.selectedRootSlot,
    );
    _saved['initialResident'] = _resident(original);
    _saved['stage'] = 'board_backup';
    await _saveFence(initial);
    // Save the selected complete document durably before switching away.
    final first = await backup.saveDocument(
      await _snapshot(initial),
      'original-selected',
    );
    _saved['originals'] = [first];
    _saved['selectedDocument'] = first;
    await backup.checkpoint();
    final other = initial.state.residents.singleWhere(
      (r) => r.rootSlot != original.rootSlot,
    );
    final selected = await _select(other);
    final second = await backup.saveDocument(
      await _snapshot(selected),
      'original-other',
    );
    _saved['originals'] = [first, second];
    _saved['selectedDocument'] = second;
    await backup.checkpoint();
    await _select(original);
    _saved['selectedDocument'] = first;
    await backup.validate();
    _saved['stage'] = 'ready';
    await backup.checkpoint();
    emit('backup_ready', {
      'path': backup.directory.path,
      'documents': 2,
      'phone_files': (_saved['phone'] as List).length,
      'hashes': [..._saved['phone'] as List, ..._saved['documents'] as List]
          .map(
            (entry) => {
              'path': (entry as Map)['path'],
              'sha256': entry['sha256'],
            },
          )
          .toList(),
    });
  }

  Future<void> _transfer(
    LitchcraftAnimationDocument document,
    String role, {
    bool interrupt = false,
    Stopwatch? actionStarted,
  }) async {
    final action = actionStarted ?? (Stopwatch()..start());
    final view = await _guard();
    final path = await backup.saveDocument(document, role);
    final priorDocument = _saved['selectedDocument'];
    if (backup.preservesArtwork && priorDocument is! String) {
      throw StateError(
        'No exact prior selected-document proof; retain backups',
      );
    }
    final pending = <String, Object?>{
      'kind': 'transfer',
      'document': path,
      'outcome': 'unknown',
      'priorDocument': priorDocument,
      'fence': view.toJson(),
      'priorResidents': view.state.residents.map(_resident).toList(),
    };
    _saved['pending'] = pending;
    await backup.checkpoint();
    // Every body write requires a verified saved test proposal. Explicit
    // preservation mode additionally requires its validated originals.
    await backup.validate();
    CoreSendEvidence evidence;
    try {
      evidence = await board.send(
        document,
        view,
        interrupt: interrupt,
        nonresident: role.startsWith('corpus-') || interrupt,
      );
    } on CoreSendFailure catch (failure) {
      if (!failure.commitSubmitted) {
        _saved['pending'] = {...pending, 'outcome': 'pre_commit'};
        await backup.checkpoint();
      }
      rethrow;
    }
    if (interrupt) {
      if (evidence.result != null ||
          evidence.fields['commit_packets'] != 0 ||
          (evidence.fields['body_packets'] as int? ?? 0) == 0 ||
          evidence.fields['disconnected'] != true) {
        throw StateError(
          'Interruption did not prove nonzero body admission without Commit and real disconnect',
        );
      }
      _saved['pending'] = {...pending, 'outcome': 'pre_commit'};
      await backup.checkpoint();
      await board.reconnect();
      final reacquired = await _guard();
      if (reacquired.state.residents.any(
            (resident) => !view.state.residents.any(
              (prior) => _sameResident(resident, _resident(prior)),
            ),
          ) ||
          view.state.residents
              .where(
                (resident) => resident.rootSlot == view.state.selectedRootSlot,
              )
              .any(
                (prior) => !reacquired.state.residents.any(
                  (resident) => _sameResident(resident, _resident(prior)),
                ),
              )) {
        throw StateError('Interruption changed selected resident descriptors');
      }
      if (backup.preservesArtwork) {
        final original = await backup.document(
          (_saved['originals'] as List).first as String,
        );
        verifyCoreDocument(original, await _snapshot(reacquired));
      }
      _saved.remove('pending');
      await backup.checkpoint();
      _saved['interruptionEvidence'] = evidence.fields;
      await backup.checkpoint();
      emit('pre_commit_interruption_verified', evidence.fields);
      return;
    }
    final committed = evidence.result;
    if (committed == null) throw StateError('Missing durable Commit receipt');
    final commitMs = action.elapsedMilliseconds;
    final live = await board.read();
    live.requireBaseline(backupArtwork: backup.preservesArtwork);
    if (live.authority.boot != view.authority.boot ||
        live.state.generation != committed.state.generation ||
        live.authority.revision != view.authority.revision + 1) {
      throw StateError(
        'Durable Commit did not retain clean expected authority',
      );
    }
    _saved['selectedDocument'] = path;
    await _saveFence(live);
    _saved['pending'] = {...pending, 'outcome': 'committed'};
    await backup.checkpoint();
    final read = Stopwatch()..start();
    await board.reconnect();
    final fresh = await _guard();
    verifyCoreDocument(document, await _snapshot(fresh));
    final fullReadbackMs = read.elapsedMilliseconds;
    _saved.remove('pending');
    await backup.checkpoint();
    final sample = <String, Object?>{
      'role': role,
      'action_to_commit_ms': commitMs,
      'fresh_connection_full_readback_ms': fullReadbackMs,
      'semantic_sha256': coreDocumentSha256(document),
      ...evidence.fields,
      'controller_adoption': 'pending',
      'physical_presentation': 'pending',
    };
    _saved.putIfAbsent('samples', () => <Map<String, Object?>>[]);
    (_saved['samples'] as List).add(sample);
    await backup.checkpoint();
    emit('full_document_verified', sample);
  }

  Future<CoreRunResult> run() async {
    Object? testFailure, restorationFailure;
    try {
      await _collect();
      // Dense payload is intentionally larger than the first production window.
      await _transfer(coreCorpus(99), 'interruption', interrupt: true);
      for (var index = 0; index < 5; index++) {
        final started = Stopwatch()..start();
        final document = coreCorpus(index);
        emit('corpus_prepared', {
          'sample': index + 1,
          'preparation_ms': started.elapsedMilliseconds,
        });
        await _transfer(
          document,
          'corpus-${index + 1}',
          actionStarted: started,
        );
        verifiedUploads++;
      }
    } catch (error) {
      testFailure = error;
      emit('test_failed', {
        'error': error.toString(),
        'kind': error.runtimeType.toString(),
      });
    }
    try {
      if (backup.preservesArtwork && (_saved['originals'] as List).isNotEmpty) {
        await restore();
      } else if (!backup.preservesArtwork && testFailure == null) {
        _saved['stage'] = 'complete';
        await backup.checkpoint();
      }
    } catch (error) {
      restorationFailure = error;
      emit('restoration_failed', {
        'error': error.toString(),
        'backup_path': backup.directory.path,
      });
    }
    return CoreRunResult(verifiedUploads, testFailure, restorationFailure);
  }

  Future<void> _settleUnknown() async {
    final pending = _saved['pending'] as Map?;
    if (pending?['kind'] != 'transfer' || pending?['outcome'] != 'unknown') {
      return;
    }
    if (pending!['fence'] is! Map ||
        pending['priorDocument'] is! String ||
        pending['priorResidents'] is! List) {
      throw StateError(
        'Unknown Commit lacks exact prior-selection proof; recovery pending',
      );
    }
    final fence = pending['fence'] as Map;
    // Inspect rejects while durable publication is pending. Its existing bounded
    // deadline is the only observation budget; no timeout authorizes overwrite.
    final before = await board.read();
    before.requireBaseline();
    if (before.authority.boot != fence['boot'] ||
        (before.authority.revision != fence['revision'] &&
            before.authority.revision != (fence['revision'] as int) + 1)) {
      throw StateError(
        'Unknown Commit has foreign boot/live revision; recovery pending',
      );
    }
    final snapshot = await board.snapshot(before);
    final after = await board.read();
    after.requireBaseline();
    if (!before.authority.same(after.authority) ||
        snapshot.session != before.state.session ||
        snapshot.generation != before.state.generation ||
        !snapshot.active ||
        snapshot.playing ||
        snapshot.selectedFrame != 0) {
      throw StateError(
        'Unknown Commit snapshot changed authority; recovery pending',
      );
    }
    final resident = before.state.residents.singleWhere(
      (r) => r.rootSlot == before.state.selectedRootSlot,
    );
    final unchanged = before.authority.revision == fence['revision'];
    final generationDelta =
        (before.state.generation - (fence['generation'] as int)) & 0xffffffff;
    if (unchanged
        ? !before.matches(fence)
        : (generationDelta == 0 ||
              generationDelta >= 0x80000000 ||
              before.state.selectedRootSlot == fence['root'] ||
              (pending['priorResidents'] as List).cast<Map>().any(
                (prior) => _sameResident(resident, prior),
              ))) {
      throw StateError(
        'Unknown Commit changed an unrelated selection; recovery pending',
      );
    }
    // +1 belongs only to this exact proposal's complete Commit. An unchanged
    // revision belongs only to the exact saved selection and its prior body.
    final selectedPath =
        (unchanged ? pending['priorDocument'] : pending['document']) as String;
    final expected = await backup.document(selectedPath);
    verifyCoreDocument(expected, snapshot.document);
    // Exact full readback plus an acknowledged same-resident selection settles
    // storage quiescence. It uses the production owner and writes no body.
    final selected = await board.select(resident);
    selected.requireBaseline();
    if (selected.authority.boot != before.authority.boot ||
        selected.state.generation != before.state.generation ||
        selected.state.selectedRootSlot != resident.rootSlot ||
        selected.authority.revision != before.authority.revision + 1) {
      throw StateError(
        'Unknown Commit settlement changed authority; recovery pending',
      );
    }
    _saved['selectedDocument'] = selectedPath;
    await _saveFence(selected);
    _saved.remove('pending');
    await backup.checkpoint();
    emit('unknown_commit_settled', {
      'semantic_sha256': coreDocumentSha256(snapshot.document),
      'generation': selected.state.generation,
      'revision': selected.authority.revision,
    });
  }

  /// Resume performs restoration only. Unknown Commit or foreign authority stops
  /// without writes; the original documents remain available for inspection.
  Future<void> restore() async {
    if (!backup.preservesArtwork) {
      throw StateError(
        'Artwork restoration not requested; only generated test evidence retained',
      );
    }
    await backup.validate();
    await board.reconnect();
    await _settleUnknown();
    await _guard();
    final paths = (_saved['originals'] as List).cast<String>();
    if (paths.length == 1 || _saved['stage'] == 'board_backup') {
      final initial = _saved['initialResident'] as Map;
      final current = await _guard();
      final resident = current.state.residents.singleWhere(
        (r) => _sameResident(r, initial),
      );
      final selected = await _select(resident);
      verifyCoreDocument(
        await backup.document(paths.first),
        await _snapshot(selected),
      );
      _saved['selectedDocument'] = paths.first;
      await backup.checkpoint();
      emit('original_selection_restored', {'body_writes': 0});
      return;
    }
    if (paths.length != 2) throw StateError('Incomplete resident backups');
    _saved['stage'] = 'restoring';
    await backup.checkpoint();
    // Other first, original selected last: both bodies survive the replacement.
    for (final path in paths.reversed) {
      await _transfer(await backup.document(path), 'restore');
    }
    // Check both actual resident bodies again after the second replacement.
    final originalSelected = await backup.document(paths.first);
    final view = await _guard();
    final originalRoot = view.state.residents.singleWhere(
      (r) => r.rootSlot == view.state.selectedRootSlot,
    );
    final otherRoot = view.state.residents.singleWhere(
      (r) => r.rootSlot != view.state.selectedRootSlot,
    );
    verifyCoreDocument(
      await backup.document(paths.last),
      await _snapshot(await _select(otherRoot)),
    );
    verifyCoreDocument(
      originalSelected,
      await _snapshot(await _select(originalRoot)),
    );
    _saved['selectedDocument'] = paths.first;
    _saved['stage'] = 'restored';
    await backup.checkpoint();
    emit('both_originals_restored', {
      'selected_frame': 0,
      'playing': false,
      'generation_and_slot_identity': 'may_change',
      'undo_history': 'not_restorable',
    });
  }
}

String coreDocumentSha256(LitchcraftAnimationDocument document) => sha256
    .convert(
      encodeLitchcraftAnimationDocument(document, localStorage: true).bytes,
    )
    .toString();

void verifyCoreDocument(
  LitchcraftAnimationDocument expected,
  LitchcraftAnimationDocument actual,
) {
  if (!sameLitchcraftDocument(expected, actual) ||
      coreDocumentSha256(expected) != coreDocumentSha256(actual)) {
    throw StateError(
      'Full document differs in ordered frames, RGB, effects, durations, topology, seed, favorites or playback-on-entry',
    );
  }
  LitchcraftFrameStoreTransfer canonical(
    LitchcraftAnimationDocument document,
  ) => LitchcraftFrameStoreProtocol.prepare(
    session: 1,
    transfer: 1,
    expectedGeneration: 1,
    topology: document.topology.id,
    active: true,
    playbackOnEntry: document.playbackOnEntry,
    sparkleSeed: document.sparkleSeed,
    favoriteColors: document.favoriteColors,
    frames: document.frames,
  );
  final left = canonical(expected), right = canonical(actual);
  if (left.canonicalBodyBytes != right.canonicalBodyBytes ||
      left.canonicalBodyCrc32 != right.canonicalBodyCrc32) {
    throw StateError('Independent canonical Store body identity differs');
  }
}

/// Five dense, nonresident 256-frame samples exercise every semantic field.
LitchcraftAnimationDocument coreCorpus(int sample) =>
    LitchcraftAnimationDocument(
      topology: LitchcraftTopology.v3,
      sparkleSeed: 0x10203040 + sample,
      favoriteColors: List.generate(
        8,
        (i) => (0x124578 + i * 0x10203 + sample * 0x701) & 0xffffff,
      ),
      playbackOnEntry: sample.isOdd,
      frames: List.generate(
        256,
        (frame) => LitchcraftAnimationFrame(
          durationMs: 50 + ((frame * (sample + 3) + sample) % 4900),
          emitters: List.generate(256, (emitter) {
            final effect =
                LitchcraftEffectStyle.values[(frame + emitter + sample) % 9];
            return LitchcraftAnimatedEmitter(
              color:
                  (0x123456 +
                      emitter * 0x10307 +
                      frame * 0x070301 +
                      sample * 0x314159) &
                  0xffffff,
              effect: effect,
              effectDurationMs: effect == LitchcraftEffectStyle.off
                  ? 0
                  : 500 + ((frame + emitter + sample) % 96) * 100,
            );
          }),
        ),
      ),
    );
