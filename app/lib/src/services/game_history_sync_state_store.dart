import 'dart:convert';
import 'dart:io';

import 'game_history_protocol.dart';

const int gameHistorySyncStateSchemaVersion = 1;
const int _maximumSyncStateBytes = 1024;

class GameHistorySyncCursor {
  const GameHistorySyncCursor({
    required this.journalId,
    required this.durableAck,
    this.lastCommit,
    this.lastSegmentSha256,
  });

  final String journalId;
  final int durableAck;
  final GameHistoryCommitPayload? lastCommit;
  final String? lastSegmentSha256;

  void validate() {
    if (!RegExp(r'^[0-9a-f]{16}$').hasMatch(journalId) ||
        journalId == '0000000000000000') {
      throw const FormatException(
        'Game-history journal identity must be eight-byte lowercase hex.',
      );
    }
    if (durableAck < 0 || durableAck > 0xffffffff) {
      throw const FormatException(
        'Game-history durable acknowledgement is out of range.',
      );
    }
    if ((lastCommit == null) != (lastSegmentSha256 == null) ||
        lastSegmentSha256 != null &&
            !RegExp(r'^[0-9a-f]{64}$').hasMatch(lastSegmentSha256!)) {
      throw const FormatException('Invalid history receipt fingerprint.');
    }
    final GameHistoryCommitPayload? receipt = lastCommit;
    if (receipt != null) {
      receipt.encode();
      if (receipt.lastOutboxSequence != durableAck) {
        throw const FormatException(
          'History receipt does not match its cursor.',
        );
      }
    }
  }

  Map<String, Object> toJson() {
    validate();
    return <String, Object>{
      'schemaVersion': gameHistorySyncStateSchemaVersion,
      'journalId': journalId,
      'durableAck': durableAck,
      'lastSegmentSha256': ?lastSegmentSha256,
      if (lastCommit case final GameHistoryCommitPayload receipt)
        'lastCommit': <String, int>{
          'firstOutboxSequence': receipt.firstOutboxSequence,
          'lastOutboxSequence': receipt.lastOutboxSequence,
          'crc32': receipt.crc32,
        },
    };
  }

  factory GameHistorySyncCursor.fromJson(Map<String, Object?> json) {
    if (json['schemaVersion'] != gameHistorySyncStateSchemaVersion ||
        json['journalId'] is! String ||
        json['durableAck'] is! int) {
      throw const FormatException('Invalid game-history sync state.');
    }
    final Object? digest = json['lastSegmentSha256'];
    if (json.containsKey('lastSegmentSha256') && digest is! String) {
      throw const FormatException('Invalid history receipt fingerprint.');
    }
    GameHistoryCommitPayload? receipt;
    if (json.containsKey('lastCommit')) {
      final Object? raw = json['lastCommit'];
      if (raw is! Map<String, Object?> ||
          raw['firstOutboxSequence'] is! int ||
          raw['lastOutboxSequence'] is! int ||
          raw['crc32'] is! int) {
        throw const FormatException('Invalid history commit receipt.');
      }
      receipt = GameHistoryCommitPayload(
        firstOutboxSequence: raw['firstOutboxSequence']! as int,
        lastOutboxSequence: raw['lastOutboxSequence']! as int,
        crc32: raw['crc32']! as int,
      );
    }
    final GameHistorySyncCursor cursor = GameHistorySyncCursor(
      journalId: json['journalId']! as String,
      durableAck: json['durableAck']! as int,
      lastCommit: receipt,
      lastSegmentSha256: digest as String?,
    );
    cursor.validate();
    return cursor;
  }
}

class GameHistorySyncStateRead {
  const GameHistorySyncStateRead({this.cursor, this.recovered = false});

  final GameHistorySyncCursor? cursor;
  final bool recovered;
}

class GameHistorySyncStateStore {
  const GameHistorySyncStateStore(this.directory);

  final Directory directory;

  Future<GameHistorySyncStateRead> load(String boardIdentity) async {
    _validateBoardIdentity(boardIdentity);
    if (!await directory.exists()) {
      return const GameHistorySyncStateRead();
    }

    final File current = _currentFile(boardIdentity);
    try {
      return GameHistorySyncStateRead(cursor: await _read(current));
    } on FormatException {
      // Recover the last validated cursor without trusting the current file.
    } on FileSystemException {
      // Recover the last validated cursor without trusting the current file.
    }
    final File previous = _previousFile(boardIdentity);
    try {
      return GameHistorySyncStateRead(
        cursor: await _read(previous),
        recovered: true,
      );
    } on FormatException {
      return const GameHistorySyncStateRead();
    } on FileSystemException {
      return const GameHistorySyncStateRead();
    }
  }

  Future<void> save(String boardIdentity, GameHistorySyncCursor cursor) async {
    _validateBoardIdentity(boardIdentity);
    cursor.validate();
    final GameHistorySyncStateRead existing = await load(boardIdentity);
    final GameHistorySyncCursor? prior = existing.cursor;
    if (prior != null &&
        prior.journalId == cursor.journalId &&
        cursor.durableAck < prior.durableAck) {
      throw const FormatException(
        'Game-history durable acknowledgement cannot move backward.',
      );
    }

    await directory.create(recursive: true);
    final File target = _currentFile(boardIdentity);
    final File temporary = _temporaryFile(boardIdentity);
    final File previous = _previousFile(boardIdentity);
    final String encoded = '${jsonEncode(cursor.toJson())}\n';
    if (utf8.encode(encoded).length > _maximumSyncStateBytes) {
      throw const FileSystemException('Game-history sync state is too large.');
    }

    try {
      await temporary.writeAsString(encoded, flush: true);
      await _read(temporary);
      if (await target.exists()) {
        try {
          await _read(target);
          await target.copy(previous.path);
        } on FormatException {
          // Keep the earlier validated backup.
        } on FileSystemException {
          // Keep the earlier validated backup.
        }
      }
      await temporary.rename(target.path);
    } finally {
      if (await temporary.exists()) {
        await temporary.delete();
      }
    }
  }

  Future<void> clear(String boardIdentity) async {
    _validateBoardIdentity(boardIdentity);
    for (final File file in <File>[
      _currentFile(boardIdentity),
      _temporaryFile(boardIdentity),
      _previousFile(boardIdentity),
    ]) {
      if (await file.exists()) {
        await file.delete();
      }
    }
  }

  Future<GameHistorySyncCursor> _read(File file) async {
    if (!await file.exists()) {
      throw FileSystemException(
        'Game-history sync state is unavailable.',
        file.path,
      );
    }
    final int length = await file.length();
    if (length <= 0 || length > _maximumSyncStateBytes) {
      throw const FormatException('Game-history sync state size is invalid.');
    }
    final Object? decoded = jsonDecode(await file.readAsString());
    if (decoded is! Map<String, Object?>) {
      throw const FormatException('Game-history sync state must be an object.');
    }
    return GameHistorySyncCursor.fromJson(decoded);
  }

  File _currentFile(String boardIdentity) {
    return File('${directory.path}/history-sync-$boardIdentity.json');
  }

  File _temporaryFile(String boardIdentity) {
    return File('${_currentFile(boardIdentity).path}.tmp');
  }

  File _previousFile(String boardIdentity) {
    return File('${_currentFile(boardIdentity).path}.previous');
  }
}

void _validateBoardIdentity(String value) {
  if (value.length > 80 || !RegExp(r'^[a-z0-9][a-z0-9._-]*$').hasMatch(value)) {
    throw const FormatException('Invalid paired-board identity.');
  }
}
