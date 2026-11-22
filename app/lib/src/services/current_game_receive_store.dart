import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import '../models/game_archive.dart';
import 'current_game_sync_protocol.dart';
import 'game_history_protocol.dart';
import 'game_history_segment_importer.dart';

const int _currentGameCursorSchemaVersion = 1;
const int _currentGameSyncMetadataVersion = 2;
const int _maximumStateBytes = 2048;

class CurrentGameReceiveCursor {
  const CurrentGameReceiveCursor({
    required this.nextRecordSequence,
    required this.prefixFingerprint,
  });

  final int nextRecordSequence;
  final List<int> prefixFingerprint;

  CurrentGameResumeCursor toWire() => CurrentGameResumeCursor(
    nextRecordSequence: nextRecordSequence,
    prefixFingerprint: prefixFingerprint,
  );
}

class CurrentGameAcceptedSegment {
  const CurrentGameAcceptedSegment({
    required this.cursor,
    required this.alreadyDurable,
    this.archive,
    this.archiveAcknowledgement,
  });

  final CurrentGameReceiveCursor cursor;
  final bool alreadyDurable;
  final GameArchive? archive;
  final CurrentGameDigest? archiveAcknowledgement;

  bool get complete => archiveAcknowledgement != null;
}

/// Owns one bounded raw current-line prefix per paired board.
///
/// Record bytes are flushed before the metadata cursor is promoted. A crash
/// between those operations leaves only an uncommitted suffix, which load
/// truncates back to the last validated metadata boundary. The caller may send
/// a segment commit only after [acceptSegment] returns, and may send an archive
/// acknowledgement only when [CurrentGameAcceptedSegment.complete] is true.
/// The same metadata document retains a completed-game identity independently
/// of cursor validity. One pending complete import must reconcile before a new
/// line may replace staged bytes; it is never exposed as a completed cursor.
class CurrentGameReceiveStore {
  CurrentGameReceiveStore({required this.directory, required this.importer});

  final Directory directory;
  final GameHistorySegmentImporter importer;
  Future<void> _mutationTail = Future<void>.value();

  Future<CurrentGameReceiveCursor?> loadCursor(String boardIdentity) {
    return _serialize<CurrentGameReceiveCursor?>(() async {
      await _prepareMetadata(boardIdentity);
      final _ReceiveState? state = await _loadState(boardIdentity);
      return state?.cursor;
    });
  }

  /// Replays the exact archive acknowledgement after a restart or lost reply.
  /// A complete cursor exists only after durable merge or intentional deletion.
  Future<CurrentGameAcceptedSegment?> reconcileComplete({
    required String boardIdentity,
    required CurrentGameDigest advertisedDigest,
    required CurrentGameBounds bounds,
    required DateTime observedAt,
  }) {
    return _serialize<CurrentGameAcceptedSegment?>(() async {
      _validateBoardIdentity(boardIdentity);
      advertisedDigest.encode(allowAbsent: false);
      bounds.encode();
      if (!bounds.present) return null;
      final metadata = await _prepareMetadata(boardIdentity);
      final _ReceiveState? state = await _loadState(boardIdentity);
      if (state == null || state.nextRecordSequence != bounds.recordCount + 1) {
        return null;
      }
      final Uint8List bytes = await _readAcceptedData(boardIdentity, state);
      final List<GameHistoryRecord> records = _decodeRecordStream(bytes);
      _validateCompleteLine(
        records,
        sha256.convert(bytes),
        advertisedDigest,
        bounds,
      );
      final GameArchive? archive = await _importComplete(
        boardIdentity,
        metadata,
        state,
        records,
        observedAt,
      );
      return CurrentGameAcceptedSegment(
        cursor: state.cursor,
        alreadyDurable: true,
        archive: archive,
        archiveAcknowledgement: advertisedDigest,
      );
    });
  }

  Future<CurrentGameAcceptedSegment> acceptSegment({
    required String boardIdentity,
    required CurrentGameDigest advertisedDigest,
    required CurrentGameBounds bounds,
    required GameHistorySegmentDescriptor descriptor,
    required Uint8List bytes,
    required DateTime observedAt,
    bool restartFromBeginning = false,
  }) {
    return _serialize<CurrentGameAcceptedSegment>(() async {
      _validateBoardIdentity(boardIdentity);
      advertisedDigest.encode(allowAbsent: false);
      bounds.encode();
      if (!bounds.present) {
        throw const FormatException('Current-game line is absent.');
      }
      final List<GameHistoryRecord> incoming = GameHistoryCodec.decodeSegment(
        bytes,
        descriptor,
      );
      if (restartFromBeginning && descriptor.firstOutboxSequence != 1) {
        throw const FormatException(
          'A current-game restart must begin at record 1.',
        );
      }
      if (descriptor.lastOutboxSequence > bounds.recordCount) {
        throw const FormatException('Segment exceeds current-line bounds.');
      }

      await directory.create(recursive: true);
      final metadata = await _prepareMetadata(boardIdentity);
      final _ReceiveState? prior = await _loadState(boardIdentity);
      final File dataFile = _dataFile(boardIdentity);
      bool alreadyDurable = false;
      if (prior == null && descriptor.firstOutboxSequence != 1) {
        throw const FormatException(
          'A current-game receive without a cursor must begin at record 1.',
        );
      }
      if (prior == null || descriptor.firstOutboxSequence == 1) {
        if (restartFromBeginning) {
          await _replaceData(boardIdentity, bytes);
        } else if (prior != null &&
            descriptor.lastOutboxSequence < prior.cursor.nextRecordSequence) {
          alreadyDurable = await _matchesDurableRange(
            dataFile,
            incoming,
            bytes,
          );
          if (!alreadyDurable) {
            await _replaceData(boardIdentity, bytes);
          }
        } else {
          await _replaceData(boardIdentity, bytes);
        }
      } else if (descriptor.firstOutboxSequence ==
          prior.cursor.nextRecordSequence) {
        final RandomAccessFile append = await dataFile.open(
          mode: FileMode.writeOnlyAppend,
        );
        try {
          await append.writeFrom(bytes);
          await append.flush();
        } finally {
          await append.close();
        }
      } else if (descriptor.lastOutboxSequence <
          prior.cursor.nextRecordSequence) {
        alreadyDurable = await _matchesDurableRange(dataFile, incoming, bytes);
        if (!alreadyDurable) {
          throw const FormatException(
            'Replayed current-game segment differs from durable bytes.',
          );
        }
      } else {
        throw const FormatException(
          'Segment does not continue the durable current-game prefix.',
        );
      }

      final int acceptedLength = await dataFile.length();
      if (acceptedLength > currentGameSyncMaximumLineBytes) {
        throw const FormatException(
          'Current-game prefix exceeds its byte bound.',
        );
      }
      final Uint8List acceptedBytes = await dataFile.readAsBytes();
      final List<GameHistoryRecord> accepted = _decodeRecordStream(
        acceptedBytes,
      );
      if (accepted.length > bounds.recordCount ||
          accepted.last.lineRevision > bounds.lineRevision) {
        throw const FormatException(
          'Current-game prefix exceeds advertised bounds.',
        );
      }
      final Digest acceptedSha = sha256.convert(acceptedBytes);
      final _ReceiveState candidate = _ReceiveState(
        acceptedBytes: acceptedBytes.length,
        nextRecordSequence: accepted.length + 1,
        prefixSha256: acceptedSha.toString(),
        advertisedFingerprint: _hex(advertisedDigest.fingerprint),
        lineRevision: bounds.lineRevision,
        recordCount: bounds.recordCount,
        flags: bounds.flags,
      );
      final bool complete = accepted.length == bounds.recordCount;
      if (complete) {
        _validateCompleteLine(accepted, acceptedSha, advertisedDigest, bounds);
      }
      GameArchive? archive;
      CurrentGameDigest? acknowledgement;
      if (complete) {
        archive = await _importComplete(
          boardIdentity,
          metadata,
          candidate,
          accepted,
          observedAt,
        );
      }
      if (!complete ||
          metadata?.completedGameId == accepted.first.boardGameId) {
        await _saveMetadata(
          boardIdentity,
          _SyncMetadata(
            cursor: candidate,
            completedGameId: metadata?.completedGameId,
          ),
        );
      }
      if (complete) acknowledgement = advertisedDigest;
      return CurrentGameAcceptedSegment(
        cursor: candidate.cursor,
        alreadyDurable: alreadyDurable,
        archive: archive,
        archiveAcknowledgement: acknowledgement,
      );
    });
  }

  Future<_SyncMetadata?> _prepareMetadata(String boardIdentity) async {
    var metadata = await _loadMetadata(boardIdentity);
    if (metadata?.pending case final pending?) {
      final archive = await importer.repository.findByBoardGameId(
        pending.gameId,
      );
      if (archive != null &&
          archive.origin == GameArchiveOrigin.board &&
          _archiveFingerprint(archive) == pending.archiveFingerprint) {
        // Repair only the proven identity association. The staged prefix may be
        // missing or still have an older cursor; normal content validation owns it.
        metadata = await _finishAssociation(
          boardIdentity,
          metadata!,
          cursor: metadata.cursor,
        );
      } else {
        // Missing, deleted or different archives are not recreated from staging.
        // A later content import can establish a new proven association.
        metadata = metadata!.cursor == null && metadata.completedGameId == null
            ? null
            : _SyncMetadata(
                cursor: metadata.cursor,
                completedGameId: metadata.completedGameId,
              );
      }
    }
    if (metadata?.completedGameId != null) return metadata;
    final state = await _loadState(boardIdentity);
    if (state == null || state.nextRecordSequence != state.recordCount + 1) {
      return metadata;
    }
    final records = _decodeRecordStream(
      await _readAcceptedData(boardIdentity, state),
    );
    final archive = await importer.repository.findByBoardGameId(
      records.first.boardGameId,
    );
    if (archive == null || !_matchesArchive(records, state, archive)) {
      return metadata;
    }
    metadata = _SyncMetadata(
      cursor: state,
      completedGameId: archive.boardGameId,
    );
    await _saveMetadata(boardIdentity, metadata);
    return metadata;
  }

  Future<GameArchive?> _importComplete(
    String boardIdentity,
    _SyncMetadata? metadata,
    _ReceiveState state,
    List<GameHistoryRecord> records,
    DateTime observedAt,
  ) async {
    final gameId = records.first.boardGameId;
    if (metadata?.completedGameId == gameId) {
      return importer.importRecords(records, observedAt: observedAt);
    }
    final existing = await importer.repository.findByBoardGameId(gameId);
    final pending = _SyncMetadata(
      cursor: metadata?.cursor,
      completedGameId: metadata?.completedGameId,
      pending: _PendingCompleteImport(
        gameId,
        _recordsFingerprint(
          records,
          existing: existing,
          observedAt: observedAt,
        ),
        observedAt.toUtc(),
      ),
    );
    // Record the complete replacement before import can create a durable archive.
    // A later failure cannot orphan C when restart observes a still-newer D.
    await _saveMetadata(boardIdentity, pending);
    final archive = await importer.importRecords(
      records,
      observedAt: observedAt,
    );
    await _finishAssociation(boardIdentity, pending, cursor: state);
    return archive;
  }

  Future<_SyncMetadata> _finishAssociation(
    String boardIdentity,
    _SyncMetadata metadata, {
    required _ReceiveState? cursor,
  }) async {
    final pending = metadata.pending!;
    final previous = metadata.completedGameId;
    if (previous != null && previous != pending.gameId) {
      await importer.repository.markBoardGameUnfinished(
        previous,
        observedAt: pending.observedAt,
      );
    }
    final completed = _SyncMetadata(
      cursor: cursor,
      completedGameId: pending.gameId,
    );
    await _saveMetadata(boardIdentity, completed);
    return completed;
  }

  Future<_SyncMetadata?> _loadMetadata(String boardIdentity) async {
    _validateBoardIdentity(boardIdentity);
    for (final file in [
      _stateFile(boardIdentity),
      _previousStateFile(boardIdentity),
    ]) {
      try {
        return await _readMetadata(file);
      } on FormatException {
        // Preserve uncertain associations; only a validated receipt can classify.
      } on FileSystemException {
        // Existing atomic metadata backup remains a possible recovery source.
      }
    }
    return null;
  }

  Future<T> _serialize<T>(Future<T> Function() action) {
    final Completer<T> result = Completer<T>();
    _mutationTail = _mutationTail.then<void>((_) async {
      try {
        result.complete(await action());
      } catch (error, stackTrace) {
        result.completeError(error, stackTrace);
      }
    });
    return result.future;
  }

  Future<_ReceiveState?> _loadState(String boardIdentity) async {
    _validateBoardIdentity(boardIdentity);
    if (!await directory.exists()) return null;
    for (final File stateFile in <File>[
      _stateFile(boardIdentity),
      _previousStateFile(boardIdentity),
    ]) {
      try {
        final _ReceiveState? state = (await _readMetadata(stateFile)).cursor;
        if (state == null) continue;
        final File data = _dataFile(boardIdentity);
        if (!await data.exists()) {
          continue;
        }
        final int dataLength = await data.length();
        if (dataLength < state.acceptedBytes) continue;
        final Uint8List accepted = await _readAcceptedData(
          boardIdentity,
          state,
        );
        final List<GameHistoryRecord> records = _decodeRecordStream(accepted);
        if (records.length + 1 != state.nextRecordSequence ||
            sha256.convert(accepted).toString() != state.prefixSha256 ||
            records.length > state.recordCount ||
            records.last.lineRevision > state.lineRevision ||
            (records.length == state.recordCount &&
                (records.last.lineRevision != state.lineRevision ||
                    (records.last is GameHistorySealRecord) !=
                        ((state.flags & currentGameSyncSealedFlag) != 0)))) {
          continue;
        }
        if (dataLength > state.acceptedBytes) {
          final RandomAccessFile suffix = await data.open(
            mode: FileMode.append,
          );
          try {
            await suffix.truncate(state.acceptedBytes);
            await suffix.flush();
          } finally {
            await suffix.close();
          }
        }
        return state;
      } on FormatException {
        // Try the previous metadata receipt.
      } on FileSystemException {
        // Try the previous metadata receipt.
      }
    }
    return null;
  }

  Future<Uint8List> _readAcceptedData(
    String boardIdentity,
    _ReceiveState state,
  ) async {
    final RandomAccessFile reader = await _dataFile(boardIdentity).open();
    try {
      final Uint8List bytes = await reader.read(state.acceptedBytes);
      if (bytes.length != state.acceptedBytes) {
        throw const FormatException('Current-game staged prefix is truncated.');
      }
      return bytes;
    } finally {
      await reader.close();
    }
  }

  Future<void> _saveMetadata(String boardIdentity, _SyncMetadata state) async {
    state.validate();
    final File target = _stateFile(boardIdentity);
    final File temporary = _temporaryStateFile(boardIdentity);
    final File previous = _previousStateFile(boardIdentity);
    final String encoded = '${jsonEncode(state.toJson())}\n';
    if (utf8.encode(encoded).length > _maximumStateBytes) {
      throw const FileSystemException(
        'Current-game receive state is too large.',
      );
    }
    try {
      await temporary.writeAsString(encoded, flush: true);
      await _readMetadata(temporary);
      if (await target.exists()) {
        try {
          await _readMetadata(target);
          await target.copy(previous.path);
        } on FormatException {
          // Preserve the earlier validated backup.
        } on FileSystemException {
          // Preserve the earlier validated backup.
        }
      }
      await temporary.rename(target.path);
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  Future<_SyncMetadata> _readMetadata(File file) async {
    if (!await file.exists()) {
      throw FileSystemException(
        'Current-game receive state is absent.',
        file.path,
      );
    }
    final int length = await file.length();
    if (length <= 0 || length > _maximumStateBytes) {
      throw const FormatException(
        'Current-game receive state size is invalid.',
      );
    }
    final Object? decoded = jsonDecode(await file.readAsString());
    if (decoded is! Map<String, Object?>) {
      throw const FormatException(
        'Current-game receive state must be an object.',
      );
    }
    return _SyncMetadata.fromJson(decoded);
  }

  Future<bool> _matchesDurableRange(
    File data,
    List<GameHistoryRecord> records,
    Uint8List bytes,
  ) async {
    if (!await data.exists()) return false;
    final Uint8List durable = await data.readAsBytes();
    int offset = 0;
    for (
      int sequence = 1;
      sequence < records.first.outboxSequence;
      sequence += 1
    ) {
      if (offset + gameHistoryRecordHeaderBytes > durable.length) return false;
      offset += gameHistoryRecordHeaderBytes + durable[offset + 2] + 4;
    }
    return offset + bytes.length <= durable.length &&
        _bytesEqual(
          Uint8List.sublistView(durable, offset, offset + bytes.length),
          bytes,
        );
  }

  Future<void> _replaceData(String boardIdentity, Uint8List bytes) async {
    final File temporary = _temporaryDataFile(boardIdentity);
    final File target = _dataFile(boardIdentity);
    try {
      await temporary.writeAsBytes(bytes, flush: true);
      await temporary.rename(target.path);
    } finally {
      if (await temporary.exists()) await temporary.delete();
    }
  }

  File _dataFile(String boardIdentity) =>
      File('${directory.path}/current-game-$boardIdentity.bin');
  File _temporaryDataFile(String boardIdentity) =>
      File('${_dataFile(boardIdentity).path}.tmp');
  File _stateFile(String boardIdentity) =>
      File('${directory.path}/current-game-$boardIdentity.json');
  File _temporaryStateFile(String boardIdentity) =>
      File('${_stateFile(boardIdentity).path}.tmp');
  File _previousStateFile(String boardIdentity) =>
      File('${_stateFile(boardIdentity).path}.previous');
}

class _SyncMetadata {
  const _SyncMetadata({this.cursor, this.completedGameId, this.pending});
  final _ReceiveState? cursor;
  final String? completedGameId;
  final _PendingCompleteImport? pending;

  void validate() {
    if (cursor == null && completedGameId == null && pending == null) {
      throw const FormatException('Current-game sync metadata is empty.');
    }
    cursor?.validate();
    if (completedGameId != null && !_isReceiptGameId(completedGameId!)) {
      throw const FormatException('Invalid completed archive identity.');
    }
    pending?.validate();
    if (pending?.gameId == completedGameId && pending != null) {
      throw const FormatException(
        'An associated game cannot require a new identity receipt.',
      );
    }
  }

  Map<String, Object> toJson() {
    validate();
    return {
      'schemaVersion': _currentGameSyncMetadataVersion,
      if (cursor != null) 'cursor': cursor!.toJson(),
      'completedArchiveGameId': ?completedGameId,
      if (pending != null) 'pendingCompleteImport': pending!.toJson(),
    };
  }

  factory _SyncMetadata.fromJson(Map<String, Object?> json) {
    if (json['schemaVersion'] == _currentGameCursorSchemaVersion) {
      return _SyncMetadata(cursor: _ReceiveState.fromJson(json));
    }
    if (json['schemaVersion'] != _currentGameSyncMetadataVersion ||
        (json.containsKey('cursor') &&
            json['cursor'] is! Map<String, Object?>) ||
        (json.containsKey('completedArchiveGameId') &&
            json['completedArchiveGameId'] is! String) ||
        (json.containsKey('pendingCompleteImport') &&
            json['pendingCompleteImport'] is! Map<String, Object?>)) {
      throw const FormatException('Invalid current-game sync metadata.');
    }
    final value = _SyncMetadata(
      cursor: json['cursor'] is Map<String, Object?>
          ? _ReceiveState.fromJson(json['cursor']! as Map<String, Object?>)
          : null,
      completedGameId: json['completedArchiveGameId'] as String?,
      pending: json['pendingCompleteImport'] is Map<String, Object?>
          ? _PendingCompleteImport.fromJson(
              json['pendingCompleteImport']! as Map<String, Object?>,
            )
          : null,
    );
    value.validate();
    return value;
  }
}

class _PendingCompleteImport {
  const _PendingCompleteImport(
    this.gameId,
    this.archiveFingerprint,
    this.observedAt,
  );
  final String gameId;
  final String archiveFingerprint;
  final DateTime observedAt;

  void validate() {
    if (!_isReceiptGameId(gameId) ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(archiveFingerprint) ||
        RegExp(r'^0+$').hasMatch(archiveFingerprint) ||
        !observedAt.isUtc) {
      throw const FormatException('Invalid pending complete archive receipt.');
    }
  }

  Map<String, Object> toJson() {
    validate();
    return {
      'gameId': gameId,
      'archiveFingerprint': archiveFingerprint,
      'observedAt': observedAt.toIso8601String(),
    };
  }

  factory _PendingCompleteImport.fromJson(Map<String, Object?> json) {
    final date = json['observedAt'] is String
        ? DateTime.tryParse(json['observedAt']! as String)
        : null;
    if (json['gameId'] is! String ||
        json['archiveFingerprint'] is! String ||
        date == null) {
      throw const FormatException('Invalid pending archive receipt fields.');
    }
    final value = _PendingCompleteImport(
      json['gameId']! as String,
      json['archiveFingerprint']! as String,
      date,
    );
    value.validate();
    return value;
  }
}

bool _isReceiptGameId(String value) =>
    RegExp(r'^[0-9a-f]{32}$').hasMatch(value) &&
    !RegExp(r'^0+$').hasMatch(value);

bool _matchesArchive(
  List<GameHistoryRecord> records,
  _ReceiveState state,
  GameArchive archive,
) =>
    state.prefixSha256.substring(0, 24) == state.advertisedFingerprint &&
    archive.origin == GameArchiveOrigin.board &&
    _recordsFingerprint(records) == _archiveFingerprint(archive);

String _recordsFingerprint(
  List<GameHistoryRecord> records, {
  GameArchive? existing,
  DateTime? observedAt,
}) {
  final positions = <ArchivedPosition>[];
  final transitions = <ArchivedTransition>[];
  GameHistorySealRecord? seal;
  for (final record in records) {
    switch (record) {
      case GameHistoryLineBaseRecord record:
        positions.add(record.position);
      case GameHistoryCommittedPlyRecord record:
        positions.add(record.position);
        transitions.add(record.transition);
      case GameHistorySealRecord record:
        seal = record;
    }
  }
  final segment = AuthoritativeLineSegment(
    boardGameId: records.first.boardGameId,
    lineRevision: records.last.lineRevision,
    variant: (records.first as GameHistoryLineBaseRecord).variant,
    status: seal?.status ?? GameArchiveStatus.active,
    outcome: seal?.outcome ?? GameOutcome.ongoing,
    winner: seal?.winner ?? GameWinner.none,
    positions: positions,
    transitions: transitions,
  );
  if (existing != null) {
    // The production model preserves an earlier captured prefix when this
    // complete board offer starts later. Prove the actual merged archive.
    return _archiveFingerprint(
      existing.mergeAuthoritativeSegment(segment, observedAt: observedAt!),
    );
  }
  return _lineFingerprint(
    segment.boardGameId,
    segment.lineRevision,
    segment.variant,
    segment.status,
    segment.outcome,
    segment.winner,
    segment.positions,
    segment.transitions,
  );
}

String _archiveFingerprint(GameArchive archive) => _lineFingerprint(
  archive.boardGameId!,
  archive.lineRevision,
  archive.variant,
  archive.status,
  archive.outcome,
  archive.winner,
  archive.positions,
  archive.transitions,
);

// Editable library metadata and observation timestamps do not identify the
// authoritative board line. This bounded proof survives loss of staged bytes.
String _lineFingerprint(
  String gameId,
  int lineRevision,
  GameVariant variant,
  GameArchiveStatus status,
  GameOutcome outcome,
  GameWinner winner,
  List<ArchivedPosition> positions,
  List<ArchivedTransition> transitions,
) => sha256
    .convert(
      utf8.encode(
        jsonEncode({
          'gameId': gameId,
          'lineRevision': lineRevision,
          'variant': variant.name,
          'status': status.name,
          'outcome': outcome.name,
          'winner': winner.name,
          'positions': [for (final position in positions) position.toJson()],
          'transitions': [
            for (final transition in transitions) transition.toJson(),
          ],
        }),
      ),
    )
    .toString();

class _ReceiveState {
  const _ReceiveState({
    required this.acceptedBytes,
    required this.nextRecordSequence,
    required this.prefixSha256,
    required this.advertisedFingerprint,
    required this.lineRevision,
    required this.recordCount,
    required this.flags,
  });

  final int acceptedBytes;
  final int nextRecordSequence;
  final String prefixSha256;
  final String advertisedFingerprint;
  final int lineRevision;
  final int recordCount;
  final int flags;

  CurrentGameReceiveCursor get cursor => CurrentGameReceiveCursor(
    nextRecordSequence: nextRecordSequence,
    prefixFingerprint: _hexBytes(prefixSha256.substring(0, 16)),
  );

  void validate() {
    if (acceptedBytes <= 0 ||
        acceptedBytes > currentGameSyncMaximumLineBytes ||
        nextRecordSequence < 2 ||
        nextRecordSequence > currentGameSyncMaximumLineRecords + 1 ||
        nextRecordSequence > recordCount + 1 ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(prefixSha256) ||
        RegExp(r'^0{64}$').hasMatch(prefixSha256) ||
        !RegExp(r'^[0-9a-f]{24}$').hasMatch(advertisedFingerprint) ||
        RegExp(r'^0{24}$').hasMatch(advertisedFingerprint)) {
      throw const FormatException('Invalid current-game receive cursor.');
    }
    CurrentGameBounds(
      lineRevision: lineRevision,
      recordCount: recordCount,
      flags: flags,
    ).encode();
  }

  Map<String, Object> toJson() {
    validate();
    return <String, Object>{
      'schemaVersion': _currentGameCursorSchemaVersion,
      'acceptedBytes': acceptedBytes,
      'nextRecordSequence': nextRecordSequence,
      'prefixSha256': prefixSha256,
      'advertisedFingerprint': advertisedFingerprint,
      'lineRevision': lineRevision,
      'recordCount': recordCount,
      'flags': flags,
    };
  }

  factory _ReceiveState.fromJson(Map<String, Object?> json) {
    if (json['schemaVersion'] != _currentGameCursorSchemaVersion ||
        json['acceptedBytes'] is! int ||
        json['nextRecordSequence'] is! int ||
        json['prefixSha256'] is! String ||
        json['advertisedFingerprint'] is! String ||
        json['lineRevision'] is! int ||
        json['recordCount'] is! int ||
        json['flags'] is! int) {
      throw const FormatException('Invalid current-game receive state.');
    }
    final _ReceiveState state = _ReceiveState(
      acceptedBytes: json['acceptedBytes']! as int,
      nextRecordSequence: json['nextRecordSequence']! as int,
      prefixSha256: json['prefixSha256']! as String,
      advertisedFingerprint: json['advertisedFingerprint']! as String,
      lineRevision: json['lineRevision']! as int,
      recordCount: json['recordCount']! as int,
      flags: json['flags']! as int,
    );
    state.validate();
    return state;
  }
}

List<GameHistoryRecord> _decodeRecordStream(Uint8List bytes) {
  if (bytes.isEmpty || bytes.length > currentGameSyncMaximumLineBytes) {
    throw const FormatException('Invalid current-game record stream size.');
  }
  final List<GameHistoryRecord> records = <GameHistoryRecord>[];
  int offset = 0;
  while (offset < bytes.length) {
    if (bytes.length - offset <
        gameHistoryRecordHeaderBytes + gameHistoryRecordCrcBytes) {
      throw const FormatException('Current-game stream ends mid-record.');
    }
    final int length = gameHistoryRecordHeaderBytes + bytes[offset + 2] + 4;
    if (length > gameHistoryMaximumRecordBytes ||
        offset + length > bytes.length) {
      throw const FormatException('Current-game record is truncated.');
    }
    final GameHistoryRecord record = GameHistoryCodec.decodeRecord(
      Uint8List.sublistView(bytes, offset, offset + length),
    );
    if (record.outboxSequence != records.length + 1 ||
        records.isNotEmpty && record.boardGameId != records.first.boardGameId ||
        records.isNotEmpty && record.lineRevision < records.last.lineRevision) {
      throw const FormatException(
        'Current-game records are not one ordered line.',
      );
    }
    records.add(record);
    offset += length;
  }
  if (records.length > currentGameSyncMaximumLineRecords ||
      records.first is! GameHistoryLineBaseRecord) {
    throw const FormatException(
      'Current-game stream has invalid record bounds.',
    );
  }
  return List<GameHistoryRecord>.unmodifiable(records);
}

bool _startsWith(List<int> full, List<int> prefix) {
  if (prefix.length > full.length) return false;
  for (int index = 0; index < prefix.length; index += 1) {
    if (full[index] != prefix[index]) return false;
  }
  return true;
}

void _validateCompleteLine(
  List<GameHistoryRecord> records,
  Digest digest,
  CurrentGameDigest advertisedDigest,
  CurrentGameBounds bounds,
) {
  if (records.length != bounds.recordCount ||
      records.last.lineRevision != bounds.lineRevision ||
      (records.last is GameHistorySealRecord) !=
          ((bounds.flags & currentGameSyncSealedFlag) != 0) ||
      !_startsWith(digest.bytes, advertisedDigest.fingerprint)) {
    throw const FormatException(
      'Complete current-game bytes differ from advertised state.',
    );
  }
}

bool _bytesEqual(Uint8List left, Uint8List right) {
  if (left.length != right.length) return false;
  for (int index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

String _hex(List<int> bytes) =>
    bytes.map((int byte) => byte.toRadixString(16).padLeft(2, '0')).join();

Uint8List _hexBytes(String value) => Uint8List.fromList(<int>[
  for (int index = 0; index < value.length; index += 2)
    int.parse(value.substring(index, index + 2), radix: 16),
]);

void _validateBoardIdentity(String value) {
  if (value.length > 80 || !RegExp(r'^[a-z0-9][a-z0-9._-]*$').hasMatch(value)) {
    throw const FormatException('Invalid paired-board identity.');
  }
}
