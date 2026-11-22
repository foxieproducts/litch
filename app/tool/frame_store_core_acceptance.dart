// Local diagnostic entry; device execution requires separate exact-target approval.
// See frame_store_core_acceptance.md for development and opt-in preservation use.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_frame_store.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'frame_store_core_backup.dart';
import 'frame_store_core_runner.dart';

const _target = String.fromEnvironment('CORE_TARGET');
const _firmwareSha256 = String.fromEnvironment('CORE_FIRMWARE_SHA256');
const _identityOnly = bool.fromEnvironment('CORE_IDENTITY_ONLY');
const _backupArtwork = bool.fromEnvironment('CORE_BACKUP_ARTWORK');
const _resumePath = String.fromEnvironment('CORE_RESUME');
const _rgbWire = bool.fromEnvironment('CORE_RGB_WIRE', defaultValue: true);

void _emit(String event, [Map<String, Object?> fields = const {}]) =>
    debugPrintSynchronously(
      'CORE_ACCEPTANCE ${jsonEncode({'event': event, ...fields})}',
    );

Future<Map<String, Object>> _phonePreferences() async {
  final prefs = await SharedPreferences.getInstance();
  await prefs.reload();
  return {
    for (final key in prefs.getKeys())
      if (prefs.get(key) case final Object value) key: value,
  };
}

String coreOutcomeMessage({
  required bool passed,
  required bool identityOnly,
  required bool resumed,
  required bool restored,
  required bool fullRestoration,
  required bool hasOriginals,
  String? backupPath,
  bool backupArtwork = false,
  CoreRunResult? runResult,
  Object? cleanupFailure,
  Object? testFailure,
  Object? restorationFailure,
}) {
  if (identityOnly) {
    return passed
        ? 'Firmware check passed.'
        : 'Firmware check failed.\n$testFailure';
  }
  final transferFailure = runResult == null
      ? testFailure
      : runResult.testFailure;
  final restoreFailure = runResult?.restorationFailure ?? restorationFailure;
  final transfersVerified =
      runResult?.verifiedUploads == 5 && transferFailure == null;
  final test = resumed
      ? 'Transfer test: not run (recovery only).'
      : transfersVerified
      ? 'Transfer test: five complete transfers verified.'
      : !backupArtwork &&
            transferFailure is CoreSendFailure &&
            transferFailure.commitSubmitted
      ? 'Transfer test: uncertain Commit outcome.\n$transferFailure'
      : 'Transfer test: failed.\n${transferFailure ?? 'Transfer verification incomplete.'}';
  final restoration = !backupArtwork
      ? 'Restoration: not requested.'
      : restored
      ? fullRestoration
            ? 'Restoration: both original artworks restored and checked.'
            : 'Restoration: original selection restored; no body writes.'
      : hasOriginals || restoreFailure != null || resumed
      ? 'Restoration: pending.\n${restoreFailure ?? testFailure ?? 'Restoration has not completed.'}'
      : 'Restoration: not needed; no board artwork writes started.';
  return '$test\n\n$restoration'
      '${cleanupFailure == null ? '' : '\n\nConnection cleanup: failed.\n$cleanupFailure'}'
      '${backupPath == null ? '' : '\n\n${backupArtwork ? 'Saved backup' : 'Test evidence'}: $backupPath'}'
      '${resumed ? '' : '\n\nEditor adoption and board presentation still need QA.'}';
}

String coreTransferProgress(
  int frameCount, {
  bool interrupt = false,
  bool restoring = false,
}) {
  if (interrupt) return 'Checking interruption before artwork admission…';
  final frames = '$frameCount ${frameCount == 1 ? 'frame' : 'frames'}';
  return restoring
      ? 'Restoring original artwork ($frames)…'
      : 'Sending complete artwork ($frames)…';
}

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final status = ValueNotifier('Preparing the transfer test…');
  runApp(
    MaterialApp(
      home: Scaffold(
        body: SafeArea(
          child: Center(
            child: SingleChildScrollView(
              padding: const EdgeInsets.all(24),
              child: ValueListenableBuilder<String>(
                valueListenable: status,
                builder: (context, message, child) => Semantics(
                  liveRegion: true,
                  child: Text(
                    'Litch transfer test\n\n$message',
                    style: const TextStyle(fontSize: 20),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  );
  final board = _ProductionCoreBoard((message) => status.value = message);
  CoreBackup? backup;
  CoreRunResult? result;
  Object? failure, cleanupFailure;
  var restored = false;
  try {
    validateCoreTarget(Platform.operatingSystem, _target, _firmwareSha256);
    if (_resumePath.isNotEmpty && !_backupArtwork) {
      throw ArgumentError(
        'CORE_RESUME restores only an explicitly requested artwork-preservation run; default run evidence authorizes no recovery writes',
      );
    }
    if (_identityOnly && _resumePath.isNotEmpty) {
      throw ArgumentError('Identity smoke cannot restore artwork');
    }
    if (!_identityOnly) {
      status.value = _backupArtwork
          ? 'Preserving saved phone artwork…'
          : 'Preparing test evidence…';
      final support = await getApplicationSupportDirectory();
      if (_resumePath.isNotEmpty &&
          !_resumePath.startsWith('/') &&
          !RegExp(r'^\d+-[0-9a-f]+$').hasMatch(_resumePath)) {
        throw ArgumentError(
          'CORE_RESUME must be a saved run directory or run ID',
        );
      }
      backup = _resumePath.isEmpty
          ? await CoreBackup.create(
              backupArtwork: _backupArtwork,
              support: support,
              target: _target,
              firmwareSha256: _firmwareSha256,
              readPreferences: _backupArtwork ? _phonePreferences : null,
            )
          : await CoreBackup.resume(
              Directory(
                _resumePath.startsWith('/')
                    ? _resumePath
                    : '${support.path}/core-acceptance-backups/$_resumePath',
              ),
            );
      if (backup.preservesArtwork != _backupArtwork) {
        throw StateError(
          'Saved run artwork-preservation mode differs from the explicit request',
        );
      }
      if (backup.manifest['target'] != _target ||
          backup.manifest['firmwareSha256'] != _firmwareSha256) {
        throw StateError(
          'Saved backup target/image differs from the explicitly selected target',
        );
      }
      if (_resumePath.isNotEmpty &&
          ((backup.manifest['originals'] as List).isEmpty ||
              backup.manifest['fence'] is! Map)) {
        throw StateError(
          'Saved run has no complete resident backup; no board restoration is authorized',
        );
      }
      board.expectedBoot = (backup.manifest['fence'] as Map?)?['boot'] as int?;
      _emit(_backupArtwork ? 'phone_backup_verified' : 'test_evidence_ready', {
        'path': backup.directory.path,
        'run_id': backup.directory.path.split('/').last,
        'files': (backup.manifest['phone'] as List).length,
      });
    }
    await board.connect();
    if (!_identityOnly) {
      final runner = CoreProtectedRunner(board, backup!, (event, fields) {
        _emit(event, fields);
        if (event == 'both_originals_restored') {
          status.value = 'Both original artworks restored and checked.';
        }
      });
      if (_resumePath.isNotEmpty) {
        status.value = 'Restoring saved board artwork…';
        await runner.restore();
        restored = true;
      } else {
        result = await runner.run();
        restored =
            result.restorationFailure == null &&
            (backup.manifest['originals'] as List).isNotEmpty;
      }
    }
  } catch (error) {
    failure = error;
  } finally {
    try {
      await board.close().timeout(const Duration(seconds: 15));
    } catch (error) {
      cleanupFailure = error;
      failure ??= error;
      _emit('connection_cleanup_failed', {'error': error.toString()});
    }
  }
  final passed =
      failure == null &&
      (_identityOnly || _resumePath.isNotEmpty || result?.passed == true);
  final testFailure = result == null ? failure : result.testFailure;
  final fullRestoration = backup?.manifest['stage'] == 'restored';
  status.value = coreOutcomeMessage(
    passed: passed,
    backupArtwork: _backupArtwork,
    runResult: result,
    cleanupFailure: cleanupFailure,
    identityOnly: _identityOnly,
    resumed: _resumePath.isNotEmpty,
    restored: restored,
    fullRestoration: fullRestoration,
    hasOriginals: (backup?.manifest['originals'] as List?)?.isNotEmpty == true,
    backupPath: backup?.directory.path,
    testFailure: testFailure,
    restorationFailure:
        result?.restorationFailure ??
        (_resumePath.isNotEmpty && !restored ? failure : null),
  );
  _emit(passed ? 'complete' : 'failed', {
    'ok': passed,
    'verification': _identityOnly
        ? 'firmware_identity'
        : _resumePath.isNotEmpty
        ? 'restoration'
        : _backupArtwork
        ? 'protected_core_roundtrip'
        : 'core_roundtrip',
    'target': _target,
    'verified_uploads': result?.verifiedUploads ?? 0,
    'evidence_path': backup?.directory.path,
    'artwork_backup_requested': _backupArtwork,
    'restoration_scope': !_backupArtwork
        ? 'not_requested'
        : fullRestoration
        ? 'both_documents'
        : 'selection_only_or_unstarted',
    'test_failure': testFailure?.toString(),
    'cleanup_failure': cleanupFailure?.toString(),
    'restoration_failure': result?.restorationFailure?.toString(),
    'controller_adoption': 'pending',
    'physical_presentation': 'pending',
  });
}

class _CoreTrace implements AppTraceSink {
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'ble.connection_session_retired' ||
        event == 'ble.connection_state' ||
        event == 'ble.connection_error') {
      _emit('transport', {'trace_event': event, ...fields});
    }
  }
}

class _ProductionCoreBoard implements CoreBoard {
  _ProductionCoreBoard(this.status);
  final void Function(String) status;
  final clock = Stopwatch()..start();
  LitchBleService? _ble;
  ConnectionSession? _session;
  StreamSubscription? _authoritySubscription;
  CoreAuthority? _latestAuthority;
  LitchcraftFrameStoreClient? _client;
  int? expectedBoot;
  int _token = -1, _request = 0, _packetBytes = 0, _readRetries = 0;
  int _allocate() => _request = (_request % 0xffff) + 1;
  bool get current =>
      _ble?.isConnected == true &&
      _ble?.connectedId == _target &&
      _ble?.connectionSessionToken == _token &&
      _session?.isActivated == true;
  void _require(bool condition, String message) {
    if (!condition) throw StateError(message);
  }

  Future<void> connect() async {
    final ble = _ble ??= LitchBleService(appTrace: _CoreTrace());
    status('Connecting and checking the exact firmware…');
    _require(await ble.connect(_target), 'BLE connection failed');
    _token = ble.connectionSessionToken;
    final capabilities = await ble.fetchDeviceCapabilities(deviceId: _target);
    _require(
      capabilities.supportsConnectionSession &&
          capabilities.hardwareRevision == LitchHardwareRevision.v3,
      'The explicitly selected board must advertise the V3 shared session',
    );
    final session = _session = await ble.openConnectionSession(_target);
    _require(session != null && current, 'Shared-session activation failed');
    final provenance = await ble.fetchFirmwareProvenance(deviceId: _target);
    _emit('firmware_identity_result', {
      'expected_sha256': _firmwareSha256,
      'actual_sha256': provenance.sha256,
      'boot_nonce': session!.boardBootNonce,
    });
    _require(
      current && provenance.sha256 == _firmwareSha256,
      'Expected exact V3 firmware identity not observed',
    );
    _require(
      expectedBoot == null || expectedBoot == session.boardBootNonce,
      'Board boot changed; retain run evidence for review before further writes',
    );
    expectedBoot ??= session.boardBootNonce;
    if (_identityOnly) {
      return; // No Inspect, selection, mode admission or artwork transfer.
    }
    final negotiated = await ble.litchcraftPacketPayloadBytes(
      _target,
      connectionSessionToken: _token,
    );
    final single = ConnectionLitchcraftCodec.maximumSingleObjectPacketBytes(
      maximumBodyBytes: session.objectMaximumBody,
    );
    _packetBytes = (single >= 126 ? single : negotiated)
        .clamp(126, ConnectionLitchcraftCodec.maximumPacketBytes)
        .toInt();
    _client = createConnectionLitchcraftFrameStoreClient(
      session: session,
      monotonicMilliseconds: () => clock.elapsedMilliseconds,
    );
    _latestAuthority = null;
    _authoritySubscription = session.states.listen((packet) {
      if (packet.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
        try {
          _latestAuthority = CoreAuthority.decode(
            packet,
            session.boardBootNonce,
          );
        } catch (_) {
          _latestAuthority = CoreAuthority(session.boardBootNonce, 0, 0, true);
        }
      }
    });
  }

  @override
  Future<void> reconnect() async {
    status('Reconnecting and checking board authority…');
    await _authoritySubscription?.cancel();
    _authoritySubscription = null;
    await _ble?.disconnect();
    _session = null;
    _client = null;
    await connect();
  }

  Future<CoreAuthority> _authority() async {
    final session = _session!;
    final packet = await session.readState(
      code: ConnectionSession.litchcraftStoreAuthorityStateCode,
    );
    _require(current, 'Connection changed during Store authority read');
    return CoreAuthority.decode(packet, session.boardBootNonce);
  }

  @override
  Future<CoreBoardView> read() async {
    _require(current, 'No active exact-target connection');
    final before = await _authority();
    final state = await _client!.inspect(session: 0, request: _allocate());
    if (!state.active || !state.hasArtwork) {
      throw CoreUnsupportedBaseline(
        'Existing active Litchcraft artwork is required; this diagnostic does not enter/admit the mode.',
      );
    }
    final tool = await _session!.readState(
      code: ConnectionSession.litchcraftStateCode,
    );
    final after = await _authority();
    _require(
      current && before.same(after),
      'Store live authority changed across Inspect/Tool read',
    );
    final body = tool.body;
    _require(
      body.length == 10 &&
          body[5] >= 1 &&
          body[5] <= 15 &&
          (body[4] & ~15) == 0 &&
          (body[9] & ~3) == 0,
      'Malformed shared Tool state',
    );
    _require(
      body[0] == state.selectedFrame &&
          (body[1] == 0 ? 256 : body[1]) == state.frameCount &&
          (body[4] & 1 != 0) == state.active &&
          (body[4] & 2 != 0) == state.playing,
      'Tool runtime and Store Inspect disagree',
    );
    return CoreBoardView(
      state,
      after,
      elapsedMs: body[2] | body[3] << 8,
      history: body[9] != 0,
    );
  }

  @override
  Future<LitchcraftFrameStoreSnapshot> snapshot(CoreBoardView view) async {
    status('Reading every frame and document setting…');
    final started = Stopwatch()..start();
    final retriesBefore = _readRetries;
    final snapshot = await _client!.snapshot(
      session: view.state.session,
      generation: view.state.generation,
      allocateRequest: _allocate,
      onRetry: (cursor, attempt) {
        _readRetries++;
        _emit('snapshot_retry', {'cursor': cursor, 'attempt': attempt});
      },
    );
    _emit('full_snapshot_complete', {
      'read_ms': started.elapsedMilliseconds,
      'snapshot_retries': _readRetries - retriesBefore,
      'frames': snapshot.document.frames.length,
      'generation': snapshot.generation,
    });
    return snapshot;
  }

  @override
  Future<CoreBoardView> select(LitchcraftFrameStoreResident resident) async {
    final state = await _client!.inspect(session: 0, request: _allocate());
    final receipt = await _client!.selectResident(
      session: state.session,
      request: _allocate(),
      resident: resident,
    );
    _require(
      current &&
          receipt.selectedRootSlot == resident.rootSlot &&
          receipt.generation == resident.generation,
      'Selection did not acknowledge the exact resident',
    );
    return read();
  }

  @override
  Future<CoreSendEvidence> send(
    LitchcraftAnimationDocument document,
    CoreBoardView view, {
    bool interrupt = false,
    bool nonresident = false,
  }) async {
    final preparation = Stopwatch()..start();
    final transfer = LitchcraftFrameStoreProtocol.prepare(
      session: view.state.session,
      transfer: _allocate(),
      expectedGeneration: view.state.generation,
      enableRgbWire: _rgbWire && view.state.rgbWireCapable,
      topology: document.topology.id,
      active: true,
      playbackOnEntry: document.playbackOnEntry,
      sparkleSeed: document.sparkleSeed,
      favoriteColors: document.favoriteColors,
      frames: document.frames,
    );
    final preparationMs = preparation.elapsedMilliseconds;
    if (nonresident &&
        view.state.residents.any((resident) => resident.matches(transfer))) {
      throw CoreSendFailure(
        StateError(
          'Diagnostic corpus is already resident; full-transfer sample rejected',
        ),
        commitSubmitted: false,
      );
    }
    final core = _client!;
    var commitPackets = 0, bodyPackets = 0, bodyBytes = 0, receiptQueries = 0;
    var disconnected = false;
    void observe(List<int> packet) {
      if (packet[2] == LitchcraftFrameStoreProtocol.storeCommit) {
        commitPackets++;
      }
      if (packet[2] == LitchcraftFrameStoreProtocol.storeChunk) {
        bodyPackets++;
        bodyBytes += packet.length - 13;
      }
    }

    final window = LitchcraftFrameStoreClient.semanticWindowChunksFor(
      maximumPacketBytes: _packetBytes,
      sharedSession: true,
    );
    final chunkCount =
        (transfer.bodyBytes + _packetBytes - 14) ~/ (_packetBytes - 13);
    if (interrupt && chunkCount <= window + 2) {
      throw CoreSendFailure(
        StateError(
          'Interruption corpus must exceed the first production admission window',
        ),
        commitSubmitted: false,
      );
    }
    final client = LitchcraftFrameStoreClient(
      packets: core.packets,
      writeControl: (packet) {
        observe(packet);
        return core.writeControl(packet);
      },
      writeBulk: (packet) {
        observe(packet);
        return core.writeBulk(packet);
      },
      writeBulkBatch: (packets) async {
        for (final packet in packets) {
          observe(packet);
        }
        await core.writeBulkBatch!(packets);
        if (interrupt && bodyPackets > 0 && !disconnected) {
          _require(
            commitPackets == 0 && bodyBytes < transfer.bodyBytes,
            'Commit or complete body reached interruption boundary',
          );
          await _ble!.disconnect();
          disconnected = true;
        }
      },
      requestReceipt: (request) {
        receiptQueries++;
        return core.requestReceipt(request);
      },
      cancelWriteIsAuthoritative: core.cancelWriteIsAuthoritative,
      monotonicMilliseconds: core.monotonicMilliseconds,
      responseTimeout: core.responseTimeout,
    );
    status(
      coreTransferProgress(
        document.frames.length,
        interrupt: interrupt,
        restoring: !nonresident,
      ),
    );
    final started = Stopwatch()..start();
    Map<String, Object?> metrics([
      LitchcraftFrameStoreTransferResult? result,
    ]) => {
      'wire_preparation_ms': preparationMs,
      'client_send_ms': started.elapsedMilliseconds,
      'wire_bytes': transfer.bodyBytes,
      'canonical_bytes': transfer.canonicalBodyBytes,
      'canonical_crc32': transfer.canonicalBodyCrc32,
      'packet_bytes': _packetBytes,
      'chunks': result?.chunkPackets ?? bodyPackets, 'writes': result?.writes,
      'body_packets': bodyPackets,
      'submitted_body_bytes': bodyBytes,
      'commit_packets': commitPackets,
      'receipt_queries': receiptQueries,
      'snapshot_retries_so_far': _readRetries,
      // Production owner does not expose its transport retransmission count.
      'transport_retries': null,
      'transport_retry_evidence': 'not_exposed_by_session',
      'begin_write_ms': result?.beginWriteMilliseconds,
      'bulk_write_ms': result?.bulkWriteMilliseconds,
      'checkpoint_ms': result?.checkpointMilliseconds,
      'commit_write_ms': result?.commitWriteMilliseconds,
      'receipt_wait_ms': result?.receiptWaitMilliseconds,
      'checkpoints': result?.checkpoints,
      'disconnected': disconnected,
    };
    try {
      final result = await client.send(
        transfer,
        maximumPacketBytes: _packetBytes,
        isCancelled: () {
          final live = _latestAuthority;
          return !current ||
              live?.dirty == true ||
              (commitPackets == 0 &&
                  live != null &&
                  !live.same(view.authority));
        },
      );
      return CoreSendEvidence(result, metrics(result));
    } on LitchcraftTransferCancelled catch (error) {
      if (interrupt && disconnected && bodyPackets > 0 && commitPackets == 0) {
        return CoreSendEvidence(null, metrics());
      }
      throw CoreSendFailure(error, commitSubmitted: commitPackets != 0);
    } on LitchcraftFrameStoreCommitPending catch (pending) {
      await pending
          .cancel(); // Retires the local observer; never asserts rollback.
      throw CoreSendFailure(
        StateError('Durable publication outcome needs authoritative readback'),
        commitSubmitted: true,
      );
    } catch (error) {
      throw CoreSendFailure(error, commitSubmitted: commitPackets != 0);
    }
  }

  Future<void> close() async {
    await _authoritySubscription?.cancel();
    final ble = _ble;
    if (ble == null) return;
    try {
      await ble.disconnect();
    } finally {
      await ble.dispose();
    }
  }
}
