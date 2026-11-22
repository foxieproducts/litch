import 'dart:convert';
import 'dart:io';

import 'lichess_board_api_client.dart';
import 'lichess_board_api_events.dart';
import 'lichess_confirmed_checkpoint.dart';
import 'lichess_prepared_session.dart';

/// One fixed bounded record; no credentials, positions or second move log.
final class FileLichessPreparedSessionStore
    implements LichessPreparedSessionStore {
  FileLichessPreparedSessionStore(this.directory);
  final Directory directory;
  static const maximumBytes = 64 * 1024;
  File get _current => File('${directory.path}/prepared-session.json');
  File get _previous =>
      File('${directory.path}/prepared-session.previous.json');
  File get _temporary => File('${directory.path}/prepared-session.tmp');
  Future<LichessPreparedRecord> _read(File file) async {
    final handle = await file.open();
    late final List<int> bytes;
    try {
      bytes = await handle.read(maximumBytes + 1);
    } finally {
      await handle.close();
    }
    if (bytes.length > maximumBytes) {
      throw const FormatException('Prepared record too large.');
    }
    return decodePreparedRecord(preparedMap(jsonDecode(utf8.decode(bytes))));
  }

  @override
  Future<LichessPreparedLoad> load() async {
    try {
      return LichessPreparedLoad(
        LichessPreparedLoadKind.current,
        record: await _read(_current),
      );
    } on FileSystemException catch (error) {
      if (error.osError?.errorCode != 2) {
        return const LichessPreparedLoad(LichessPreparedLoadKind.unavailable);
      }
      try {
        return LichessPreparedLoad(
          LichessPreparedLoadKind.recovered,
          record: await _read(_previous),
        );
      } on FileSystemException catch (backupError) {
        return LichessPreparedLoad(
          backupError.osError?.errorCode == 2
              ? LichessPreparedLoadKind.absent
              : LichessPreparedLoadKind.unavailable,
        );
      } on FormatException {
        return const LichessPreparedLoad(LichessPreparedLoadKind.corrupt);
      }
    } on FormatException {
      try {
        return LichessPreparedLoad(
          LichessPreparedLoadKind.recovered,
          record: await _read(_previous),
        );
      } on FileSystemException {
        return const LichessPreparedLoad(LichessPreparedLoadKind.corrupt);
      } on FormatException {
        return const LichessPreparedLoad(LichessPreparedLoadKind.corrupt);
      }
    }
  }

  @override
  Future<void> write(LichessPreparedRecord record) async {
    final prior = await load();
    if (prior.kind != LichessPreparedLoadKind.absent &&
        prior.kind != LichessPreparedLoadKind.current) {
      throw const FileSystemException(
        'Prepared record requires explicit recovery.',
      );
    }
    if (prior.record != null && record.revision <= prior.record!.revision) {
      if (jsonEncode(encodePreparedRecord(record)) ==
          jsonEncode(encodePreparedRecord(prior.record!))) {
        return;
      }
      throw const FormatException(
        'Prepared record revision cannot regress or conflict.',
      );
    }
    final encoded = jsonEncode(encodePreparedRecord(record));
    if (utf8.encode(encoded).length > maximumBytes) {
      throw const FormatException('Prepared record too large.');
    }
    await directory.create(recursive: true);
    try {
      await _temporary.writeAsString(encoded, flush: true);
      await _read(_temporary);
      try {
        await _read(_current);
        await _current.copy(_previous.path);
      } on FormatException {
        /* Preserve validated backup and rejected current. */
      } on FileSystemException catch (error) {
        if (error.osError?.errorCode != 2) rethrow;
      }
      await _temporary.rename(_current.path);
    } finally {
      if (await _temporary.exists()) await _temporary.delete();
    }
  }
}

Map<String, Object?> encodePreparedRecord(LichessPreparedRecord record) {
  final id = record.identity;
  boundedPreparedAscii(id.sessionId, 80);
  boundedPreparedAscii(record.accountId, 64);
  if (!RegExp(r'^[a-z0-9][a-z0-9._-]{0,79}$').hasMatch(id.boardIdentity) ||
      !RegExp(r'^[0-9a-f]{32}$').hasMatch(id.boardGameId)) {
    throw const FormatException('Invalid persistent board identity.');
  }
  preparedCounter(id.lineRevision);
  preparedCounter(record.revision);
  if ((record.providerGameId == null) != (record.providerSide == null) ||
      record.providerGameId != null &&
          !RegExp(r'^[A-Za-z0-9]{1,64}$').hasMatch(record.providerGameId!)) {
    throw const FormatException('Invalid prepared provider binding.');
  }
  record.confirmed?.validate();
  record.recoveryObservation?.validate();
  if ((record.confirmed != null || record.recoveryObservation != null) &&
      record.providerGameId == null) {
    throw const FormatException('Unbound provider receipt.');
  }
  if (record.confirmed != null &&
      record.confirmed!.libraryId != 'board-${id.boardGameId}') {
    throw const FormatException('Checkpoint archive identity mismatch.');
  }
  final pending = record.pendingLocalMove;
  if (pending != null) {
    validatePendingLocalMove(pending);
    if (pending.identity != id ||
        pending.providerGameId != record.providerGameId ||
        record.confirmed == null ||
        pending.preStatePly != record.confirmed!.receipt.ply ||
        pending.preStatePrefixSha256 != record.confirmed!.prefixSha256 ||
        pending.targetPly.isOdd !=
            (record.providerSide == LichessPlayerColor.white) ||
        (pending.rejected && pending.httpAcknowledged)) {
      throw const FormatException('Pending move identity mismatch.');
    }
  }
  final confirmedMove = record.confirmedLocalMove;
  if (confirmedMove != null) {
    validateConfirmedLocalMove(confirmedMove);
    if (confirmedMove.identity != id ||
        confirmedMove.providerGameId != record.providerGameId ||
        record.confirmed == null ||
        confirmedMove.targetPly > record.confirmed!.receipt.ply) {
      throw const FormatException('Confirmed move identity mismatch.');
    }
  }
  LichessRealtimeSeek.casualRapid(
    minutes: record.seek.minutes,
    incrementSeconds: record.seek.incrementSeconds,
  );
  return {
    'schemaVersion': 2,
    'identity': {
      'sessionId': id.sessionId,
      'boardIdentity': id.boardIdentity,
      'boardGameId': id.boardGameId,
      'lineRevision': id.lineRevision,
    },
    'accountId': record.accountId,
    'minutes': record.seek.minutes,
    'incrementSeconds': record.seek.incrementSeconds,
    'revision': record.revision,
    'seekMayHaveStarted': record.seekMayHaveStarted,
    'released': record.released,
    'providerGameId': record.providerGameId,
    'providerSide': record.providerSide?.name,
    'confirmed': record.confirmed?.toJson(),
    'recoveryObservation': record.recoveryObservation?.toJson(),
    'pendingLocalMove': pending == null ? null : _encodePendingMove(pending),
    'confirmedLocalMove': confirmedMove == null
        ? null
        : _encodeConfirmedMove(confirmedMove),
  };
}

LichessPreparedRecord decodePreparedRecord(Map<String, Object?> json) {
  final schemaVersion = json['schemaVersion'];
  if (schemaVersion != 1 && schemaVersion != 2) {
    throw const FormatException('Unsupported prepared schema.');
  }
  if (schemaVersion == 1 &&
      (json.containsKey('pendingLocalMove') ||
          json.containsKey('confirmedLocalMove'))) {
    throw const FormatException('Prepared schema downgrade is not allowed.');
  }
  bool flag(String key) {
    final v = json[key];
    if (v is! bool) throw FormatException('Invalid $key.');
    return v;
  }

  final id = preparedMap(json['identity']);
  final side = json['providerSide'];
  if (side != null && side != 'white' && side != 'black') {
    throw const FormatException('Invalid prepared side.');
  }
  final r = LichessPreparedRecord(
    identity: LichessPreparedIdentity(
      sessionId: preparedString(id, 'sessionId'),
      boardIdentity: preparedString(id, 'boardIdentity'),
      boardGameId: preparedString(id, 'boardGameId'),
      lineRevision: preparedInt(id, 'lineRevision'),
    ),
    accountId: preparedString(json, 'accountId'),
    seek: _decodeSeek(json),
    revision: preparedInt(json, 'revision'),
    seekMayHaveStarted: flag('seekMayHaveStarted'),
    released: flag('released'),
    providerGameId: json['providerGameId'] == null
        ? null
        : preparedString(json, 'providerGameId'),
    providerSide: side == null
        ? null
        : side == 'white'
        ? LichessPlayerColor.white
        : LichessPlayerColor.black,
    confirmed: json['confirmed'] == null
        ? null
        : LichessConfirmedCheckpoint.fromJson(preparedMap(json['confirmed'])),
    recoveryObservation: json['recoveryObservation'] == null
        ? null
        : LichessProviderReceipt.fromJson(
            preparedMap(json['recoveryObservation']),
          ),
    pendingLocalMove: schemaVersion == 2 && json['pendingLocalMove'] != null
        ? _decodePendingMove(preparedMap(json['pendingLocalMove']))
        : null,
    confirmedLocalMove: schemaVersion == 2 && json['confirmedLocalMove'] != null
        ? _decodeConfirmedMove(preparedMap(json['confirmedLocalMove']))
        : null,
  );
  encodePreparedRecord(r);
  return r;
}

Map<String, Object?> _encodeIdentity(LichessPreparedIdentity identity) => {
  'sessionId': identity.sessionId,
  'boardIdentity': identity.boardIdentity,
  'boardGameId': identity.boardGameId,
  'lineRevision': identity.lineRevision,
};

LichessPreparedIdentity _decodeIdentity(Map<String, Object?> json) =>
    LichessPreparedIdentity(
      sessionId: preparedString(json, 'sessionId'),
      boardIdentity: preparedString(json, 'boardIdentity'),
      boardGameId: preparedString(json, 'boardGameId'),
      lineRevision: preparedInt(json, 'lineRevision'),
    );

Map<String, Object?> _encodePendingMove(LichessPendingLocalMove pending) => {
  'identity': _encodeIdentity(pending.identity),
  'providerGameId': pending.providerGameId,
  'targetPly': pending.targetPly,
  'uci': pending.uci,
  'localCommandId': pending.localCommandId,
  'preStatePly': pending.preStatePly,
  'preStatePrefixSha256': pending.preStatePrefixSha256,
  'retryUsed': pending.retryUsed,
  'httpAcknowledged': pending.httpAcknowledged,
  'rejected': pending.rejected,
  'recoveryHold': pending.recoveryHold?.name,
};

LichessPendingLocalMove _decodePendingMove(Map<String, Object?> json) {
  bool flag(String key) {
    final value = json[key];
    if (value is! bool) throw FormatException('Invalid $key.');
    return value;
  }

  final recoveryName = json['recoveryHold'];
  final recoveryHold = recoveryName == null
      ? null
      : LichessPreparedRecovery.values
            .where((value) => value.name == recoveryName)
            .firstOrNull;
  if (recoveryName != null && recoveryHold == null) {
    throw const FormatException('Invalid pending move recovery hold.');
  }
  return LichessPendingLocalMove(
    identity: _decodeIdentity(preparedMap(json['identity'])),
    providerGameId: preparedString(json, 'providerGameId'),
    targetPly: preparedInt(json, 'targetPly'),
    uci: preparedString(json, 'uci'),
    localCommandId: preparedString(json, 'localCommandId'),
    preStatePly: preparedInt(json, 'preStatePly'),
    preStatePrefixSha256: preparedString(json, 'preStatePrefixSha256'),
    retryUsed: flag('retryUsed'),
    httpAcknowledged: flag('httpAcknowledged'),
    rejected: flag('rejected'),
    recoveryHold: recoveryHold,
  );
}

Map<String, Object?> _encodeConfirmedMove(
  LichessConfirmedLocalMoveReceipt receipt,
) => {
  'identity': _encodeIdentity(receipt.identity),
  'providerGameId': receipt.providerGameId,
  'targetPly': receipt.targetPly,
  'uci': receipt.uci,
  'localCommandId': receipt.localCommandId,
};

LichessConfirmedLocalMoveReceipt _decodeConfirmedMove(
  Map<String, Object?> json,
) => LichessConfirmedLocalMoveReceipt(
  identity: _decodeIdentity(preparedMap(json['identity'])),
  providerGameId: preparedString(json, 'providerGameId'),
  targetPly: preparedInt(json, 'targetPly'),
  uci: preparedString(json, 'uci'),
  localCommandId: preparedString(json, 'localCommandId'),
);

LichessRealtimeSeek _decodeSeek(Map<String, Object?> json) {
  try {
    return LichessRealtimeSeek.casualRapid(
      minutes: preparedInt(json, 'minutes'),
      incrementSeconds: preparedInt(json, 'incrementSeconds'),
    );
  } on ArgumentError {
    throw const FormatException('Invalid persisted seek.');
  }
}
