import 'dart:convert';

import 'package:crypto/crypto.dart';

import '../../models/game_archive.dart';
import 'lichess_board_api_events.dart';

/// Bounded provider receipt metadata. Positions live only in GameArchive.
final class LichessProviderReceipt {
  const LichessProviderReceipt({
    required this.rawStatus,
    this.winner,
    required this.ply,
    required this.movesSha256,
    required this.whiteTimeMs,
    required this.blackTimeMs,
    required this.whiteIncrementMs,
    required this.blackIncrementMs,
    required this.receivedAt,
  });
  factory LichessProviderReceipt.fromEvent(
    LichessGameStateEvent event,
    DateTime now,
  ) => LichessProviderReceipt(
    rawStatus: event.status.wireValue,
    winner: event.winner?.name,
    ply: event.moves.length,
    movesSha256: sha256.convert(utf8.encode(event.moves.join(' '))).toString(),
    whiteTimeMs: event.whiteTimeMs,
    blackTimeMs: event.blackTimeMs,
    whiteIncrementMs: event.whiteIncrementMs,
    blackIncrementMs: event.blackIncrementMs,
    receivedAt: now.toUtc(),
  );
  final String rawStatus;
  final String? winner;
  final int ply;
  final String movesSha256;
  final int whiteTimeMs, blackTimeMs, whiteIncrementMs, blackIncrementMs;
  final DateTime receivedAt;
  void validate() {
    boundedPreparedAscii(rawStatus, 64);
    if (winner != null && winner != 'white' && winner != 'black') {
      throw const FormatException('Invalid receipt winner.');
    }
    if (ply < 0 || ply > gameArchiveMaximumPlies) {
      throw const FormatException('Invalid receipt ply.');
    }
    preparedDigest(movesSha256);
    for (final value in [
      whiteTimeMs,
      blackTimeMs,
      whiteIncrementMs,
      blackIncrementMs,
    ]) {
      preparedCounter(value);
    }
  }

  Map<String, Object?> toJson() => {
    'rawStatus': rawStatus,
    'winner': winner,
    'ply': ply,
    'movesSha256': movesSha256,
    'whiteTimeMs': whiteTimeMs,
    'blackTimeMs': blackTimeMs,
    'whiteIncrementMs': whiteIncrementMs,
    'blackIncrementMs': blackIncrementMs,
    'receivedAt': receivedAt.toUtc().toIso8601String(),
  };
  factory LichessProviderReceipt.fromJson(Map<String, Object?> json) {
    final receipt = LichessProviderReceipt(
      rawStatus: preparedString(json, 'rawStatus'),
      winner: json['winner'] == null ? null : preparedString(json, 'winner'),
      ply: preparedInt(json, 'ply'),
      movesSha256: preparedString(json, 'movesSha256'),
      whiteTimeMs: preparedInt(json, 'whiteTimeMs'),
      blackTimeMs: preparedInt(json, 'blackTimeMs'),
      whiteIncrementMs: preparedInt(json, 'whiteIncrementMs'),
      blackIncrementMs: preparedInt(json, 'blackIncrementMs'),
      receivedAt: DateTime.parse(preparedString(json, 'receivedAt')).toUtc(),
    );
    receipt.validate();
    return receipt;
  }
}

final class LichessConfirmedCheckpoint {
  const LichessConfirmedCheckpoint({
    required this.libraryId,
    required this.prefixSha256,
    required this.receipt,
  });
  final String libraryId;
  final String prefixSha256;
  final LichessProviderReceipt receipt;
  void validate() {
    if (!isValidGameArchiveLibraryIdentifier(libraryId)) {
      throw const FormatException('Invalid checkpoint archive.');
    }
    preparedDigest(prefixSha256);
    receipt.validate();
    if (const {
          'created',
          'started',
          'draw',
          'stalemate',
          'insufficientMaterialClaim',
        }.contains(receipt.rawStatus) &&
        receipt.winner != null) {
      throw const FormatException(
        'Checkpoint status contradicts provider winner.',
      );
    }

    if (LichessKnownGameStatus.fromWire(receipt.rawStatus) == null) {
      throw const FormatException('Unknown status is recovery evidence only.');
    }
  }

  Map<String, Object?> toJson() => {
    'libraryId': libraryId,
    'prefixSha256': prefixSha256,
    'receipt': receipt.toJson(),
  };
  factory LichessConfirmedCheckpoint.fromJson(Map<String, Object?> json) {
    final checkpoint = LichessConfirmedCheckpoint(
      libraryId: preparedString(json, 'libraryId'),
      prefixSha256: preparedString(json, 'prefixSha256'),
      receipt: LichessProviderReceipt.fromJson(preparedMap(json['receipt'])),
    );
    checkpoint.validate();
    return checkpoint;
  }
}

String confirmedArchivePrefixDigest(GameArchive archive, int ply) {
  if (archive.positions.first.ply != 0 || ply < 0 || ply > archive.currentPly) {
    throw const FormatException('Missing confirmed archive prefix.');
  }
  return sha256
      .convert(
        utf8.encode(
          jsonEncode({
            'boardGameId': archive.boardGameId,
            'positions': archive.positions
                .take(ply + 1)
                .map((p) => p.toJson())
                .toList(),
            'transitions': archive.transitions
                .take(ply)
                .map((p) => p.toJson())
                .toList(),
          }),
        ),
      )
      .toString();
}

void boundedPreparedAscii(String value, int maximum) {
  if (value.isEmpty ||
      value.length > maximum ||
      value.codeUnits.any((c) => c < 32 || c > 126)) {
    throw const FormatException('Invalid bounded prepared value.');
  }
}

void preparedCounter(int value) {
  if (value < 0 || value > 0x7fffffffffffffff) {
    throw const FormatException('Invalid prepared counter.');
  }
}

void preparedDigest(String value) {
  if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(value)) {
    throw const FormatException('Invalid prepared digest.');
  }
}

String preparedString(Map<String, Object?> json, String key) {
  final v = json[key];
  if (v is! String) throw FormatException('Invalid $key.');
  return v;
}

int preparedInt(Map<String, Object?> json, String key) {
  final v = json[key];
  if (v is! int) throw FormatException('Invalid $key.');
  preparedCounter(v);
  return v;
}

Map<String, Object?> preparedMap(Object? value) {
  if (value is! Map<String, Object?>) {
    throw const FormatException('Invalid prepared object.');
  }
  return value;
}
