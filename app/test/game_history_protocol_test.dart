import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';

void main() {
  late Map<String, Uint8List> fixtures;

  setUpAll(() {
    fixtures = <String, Uint8List>{};
    for (final String line in File(
      '../test/fixtures/game_history_v1.txt',
    ).readAsLinesSync()) {
      if (line.isEmpty || line.startsWith('#')) continue;
      final int separator = line.indexOf('=');
      fixtures[line.substring(0, separator)] = _hexBytes(
        line.substring(separator + 1),
      );
    }
  });

  for (final bool zeroIdentity in <bool>[true, false]) {
    test(
      'CRC-valid record rejects ${zeroIdentity ? 'zero identity' : 'high-bit revision'}',
      () {
        final Uint8List bytes = Uint8List.fromList(
          fixtures['record_line_base_seq_1']!,
        );
        if (zeroIdentity) {
          bytes.fillRange(7, 23, 0);
        } else {
          bytes[30] = 0x80;
        }
        final int crcOffset = bytes.length - gameHistoryRecordCrcBytes;
        ByteData.sublistView(bytes).setUint32(
          crcOffset,
          GameHistoryCrc32.compute(Uint8List.sublistView(bytes, 0, crcOffset)),
          Endian.little,
        );
        expect(
          () => GameHistoryCodec.decodeRecord(bytes),
          throwsFormatException,
          reason: zeroIdentity ? 'zero game identity' : 'high-bit revision',
        );
      },
    );
  }

  test('position codec matches native fixture and keeps 32-bit counters', () {
    const ArchivedPosition initial = ArchivedPosition(
      ply: 0,
      fen:
          'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
          'RNBQKBNR w KQkq - 0 1',
    );
    final Uint8List encoded = GameHistoryCodec.encodePosition(
      initial,
      GameVariant.classic,
    );
    expect(encoded, fixtures['position_initial_classic']);
    expect(GameHistoryCodec.decodePosition(encoded, ply: 0).fen, initial.fen);

    const ArchivedPosition largeCounters = ArchivedPosition(
      ply: 0,
      fen: '8/8/8/8/8/8/8/8 w - - 4294967295 4294967295',
    );
    expect(
      GameHistoryCodec.decodePosition(
        GameHistoryCodec.encodePosition(largeCounters, GameVariant.classic),
        ply: 0,
      ).fen,
      largeCounters.fen,
    );
  });

  test('records match native fixtures and retain 64-bit line revisions', () {
    const String gameId = '000102030405060708090a0b0c0d0e0f';
    const ArchivedPosition initial = ArchivedPosition(
      ply: 0,
      fen:
          'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
          'RNBQKBNR w KQkq - 0 1',
    );
    const GameHistoryLineBaseRecord lineBase = GameHistoryLineBaseRecord(
      outboxSequence: 1,
      boardGameId: gameId,
      lineRevision: 1,
      variant: GameVariant.classic,
      position: initial,
    );
    final Uint8List lineBaseBytes = GameHistoryCodec.encodeRecord(lineBase);
    expect(lineBaseBytes.length, 81);
    expect(lineBaseBytes, fixtures['record_line_base_seq_1']);
    expect(
      (GameHistoryCodec.decodeRecord(lineBaseBytes)
              as GameHistoryLineBaseRecord)
          .position
          .fen,
      initial.fen,
    );

    const GameHistoryCommittedPlyRecord committed =
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: gameId,
          lineRevision: 2,
          position: ArchivedPosition(
            ply: 1,
            fen:
                'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
                'RNBQKBNR b KQkq e3 0 1',
          ),
          transition: ArchivedTransition(
            side: GameSide.white,
            from: 52,
            to: 36,
          ),
        );
    final Uint8List committedBytes = GameHistoryCodec.encodeRecord(committed);
    expect(committedBytes.length, 83);
    expect(committedBytes, fixtures['record_committed_ply_seq_2']);

    final GameHistoryCommittedPlyRecord largeRevision =
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: gameId,
          lineRevision: 0x0102030405060708,
          position: committed.position,
          transition: const ArchivedTransition(
            side: GameSide.black,
            from: 52,
            to: 60,
            captureSquare: 53,
            promotion: PromotionPiece.queen,
            secondaryFrom: 63,
            secondaryTo: 61,
            duckFrom: 27,
            duckTo: 28,
          ),
        );
    final Uint8List maximumBytes = GameHistoryCodec.encodeRecord(largeRevision);
    expect(maximumBytes.length, gameHistoryMaximumRecordBytes);
    expect(maximumBytes[76], 0x1f);
    expect(
      (GameHistoryCodec.decodeRecord(maximumBytes)
              as GameHistoryCommittedPlyRecord)
          .lineRevision,
      largeRevision.lineRevision,
    );

    final Uint8List corrupted = Uint8List.fromList(committedBytes);
    corrupted[12] ^= 0x40;
    expect(
      () => GameHistoryCodec.decodeRecord(corrupted),
      throwsFormatException,
    );
    expect(
      () => GameHistoryCodec.decodeRecord(
        Uint8List.sublistView(lineBaseBytes, 0, lineBaseBytes.length - 1),
      ),
      throwsFormatException,
    );
  });

  test('minimum-ATT control frames match native fixtures', () {
    final Uint8List beginPayload = const GameHistoryBeginPayload(
      journalId: '0102030405060708',
      appDurableAck: 0,
    ).encode();
    final GameHistoryFrame begin = GameHistoryFrame(
      opcode: GameHistoryOpcode.begin.wireValue,
      transactionId: 0x1234,
      frameSequence: 0,
      payload: beginPayload,
    );
    expect(begin.encode(), fixtures['frame_begin_tx_4660']);
    expect(begin.encode().length, gameHistoryMinimumAttPayloadBytes);
    expect(
      GameHistoryBeginPayload.decode(beginPayload).journalId,
      '0102030405060708',
    );

    final Uint8List segmentBytes = Uint8List.fromList(<int>[
      ...fixtures['record_line_base_seq_1']!,
      ...fixtures['record_committed_ply_seq_2']!,
    ]);
    final GameHistoryCrc32 fragmented = GameHistoryCrc32()
      ..update(Uint8List.sublistView(segmentBytes, 0, 17))
      ..update(Uint8List.sublistView(segmentBytes, 17));
    expect(fragmented.value, 0xca681bc0);
    final GameHistorySegmentDescriptor descriptor =
        GameHistorySegmentDescriptor(
          encodedBytes: segmentBytes.length,
          recordCount: 2,
          firstOutboxSequence: 1,
          crc32: fragmented.value,
        );
    final GameHistoryFrame segmentBegin = GameHistoryFrame(
      opcode: GameHistoryOpcode.segmentBegin.wireValue,
      transactionId: 0x1234,
      frameSequence: 3,
      payload: descriptor.encode(),
    );
    expect(segmentBegin.encode(), fixtures['frame_segment_begin_tx_4660']);
    expect(
      GameHistoryCodec.decodeSegment(segmentBytes, descriptor),
      hasLength(2),
    );

    final Uint8List unknown = Uint8List.fromList(segmentBegin.encode())
      ..[2] = 0x40;
    expect(GameHistoryFrame.decode(unknown).knownOpcode, isNull);
    expect(
      () => GameHistoryFrame.decode(Uint8List.sublistView(unknown, 0, 7)),
      throwsFormatException,
    );
  });

  test('restore controls share native fixtures and canonical opcodes', () {
    final Uint8List cursor = const GameHistoryRestoreCursorPayload(
      journalId: '0102030405060708',
      durableAck: 0x0a0b0c0d,
    ).encode();
    expect(cursor, fixtures['restore_cursor']);
    expect(
      GameHistoryRestoreCursorPayload.decode(cursor).journalId,
      '0102030405060708',
    );
    expect(
      () => const GameHistoryRestoreCursorPayload(
        journalId: '0102030405060708',
        durableAck: 0,
      ).encode(),
      throwsFormatException,
    );

    final GameHistoryRestoreManifestPayload manifest =
        GameHistoryRestoreManifestPayload(
          encodedBytes: gameHistoryMaximumRestoreUploadBytes,
          recordCount: gameHistoryMaximumRestoreRecords,
          variant: GameVariant.duck,
          basePly: 3968,
          currentPly: 4096,
          crc32: 0x12345678,
        );
    expect(manifest.encode(), fixtures['restore_manifest_max']);
    expect(
      GameHistoryRestoreManifestPayload.decode(manifest.encode()).currentPly,
      4096,
    );

    final GameHistoryRestoreAcceptedPayload accepted =
        GameHistoryRestoreAcceptedPayload(
          currentPly: 4096,
          transitionCount: gameHistoryMaximumRestoreTransitions,
          variant: GameVariant.duck,
          encodedBytes: gameHistoryMaximumRestoreUploadBytes,
          crc32: 0x12345678,
        );
    expect(accepted.encode(), fixtures['restore_accepted_max']);
    expect(
      GameHistoryRestoreAcceptedPayload.decode(accepted.encode()).crc32,
      0x12345678,
    );
    final Uint8List reserved = Uint8List.fromList(accepted.encode())..[6] = 1;
    expect(
      () => GameHistoryRestoreAcceptedPayload.decode(reserved),
      throwsFormatException,
    );
    expect(
      GameHistoryOpcode.tryFromWire(0x15),
      GameHistoryOpcode.restoreAccepted,
    );
  });

  test(
    'segment rejects CRC errors, truncation, duplicates, and reordering',
    () {
      final Uint8List base = fixtures['record_line_base_seq_1']!;
      final Uint8List ply = fixtures['record_committed_ply_seq_2']!;
      final Uint8List valid = Uint8List.fromList(<int>[...base, ...ply]);
      final GameHistorySegmentDescriptor validDescriptor =
          GameHistorySegmentDescriptor(
            encodedBytes: valid.length,
            recordCount: 2,
            firstOutboxSequence: 1,
            crc32: GameHistoryCrc32.compute(valid),
          );

      final Uint8List corrupted = Uint8List.fromList(valid)..[20] ^= 1;
      expect(
        () => GameHistoryCodec.decodeSegment(corrupted, validDescriptor),
        throwsFormatException,
      );
      expect(
        () => GameHistoryCodec.decodeSegment(
          Uint8List.sublistView(valid, 0, valid.length - 1),
          validDescriptor,
        ),
        throwsFormatException,
      );

      final GameHistoryRecord committed = GameHistoryCodec.decodeRecord(ply);
      final Uint8List duplicatePly = GameHistoryCodec.encodeRecord(
        GameHistoryCommittedPlyRecord(
          outboxSequence: 1,
          boardGameId: committed.boardGameId,
          lineRevision: committed.lineRevision,
          position: (committed as GameHistoryCommittedPlyRecord).position,
          transition: committed.transition,
        ),
      );
      final Uint8List duplicate = Uint8List.fromList(<int>[
        ...base,
        ...duplicatePly,
      ]);
      expect(
        () => GameHistoryCodec.decodeSegment(
          duplicate,
          GameHistorySegmentDescriptor(
            encodedBytes: duplicate.length,
            recordCount: 2,
            firstOutboxSequence: 1,
            crc32: GameHistoryCrc32.compute(duplicate),
          ),
        ),
        throwsFormatException,
      );

      final Uint8List reordered = Uint8List.fromList(<int>[...ply, ...base]);
      expect(
        () => GameHistoryCodec.decodeSegment(
          reordered,
          GameHistorySegmentDescriptor(
            encodedBytes: reordered.length,
            recordCount: 2,
            firstOutboxSequence: 1,
            crc32: GameHistoryCrc32.compute(reordered),
          ),
        ),
        throwsFormatException,
      );
    },
  );

  test('record bounds admit ply 4096 and reject 4097', () {
    const String gameId = '000102030405060708090a0b0c0d0e0f';
    const ArchivedTransition transition = ArchivedTransition(
      side: GameSide.black,
      from: 8,
      to: 16,
    );
    const GameHistoryCommittedPlyRecord last = GameHistoryCommittedPlyRecord(
      outboxSequence: 0xffffffff,
      boardGameId: gameId,
      lineRevision: 0x7fffffffffffffff,
      position: ArchivedPosition(
        ply: 4096,
        fen: '8/8/8/8/8/8/8/8 w - - 0 2049',
      ),
      transition: transition,
    );
    expect(
      (GameHistoryCodec.decodeRecord(GameHistoryCodec.encodeRecord(last))
              as GameHistoryCommittedPlyRecord)
          .position
          .ply,
      4096,
    );
    expect(
      () => GameHistoryCodec.encodeRecord(
        const GameHistoryCommittedPlyRecord(
          outboxSequence: 1,
          boardGameId: gameId,
          lineRevision: 1,
          position: ArchivedPosition(
            ply: 4097,
            fen: '8/8/8/8/8/8/8/8 w - - 0 2049',
          ),
          transition: transition,
        ),
      ),
      throwsFormatException,
    );
  });

  test('board resignation seal round trips additive native outcome6', () {
    const seal = GameHistorySealRecord(
      outboxSequence: 3,
      boardGameId: '000102030405060708090a0b0c0d0e0f',
      lineRevision: 2,
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.resignation,
      winner: GameWinner.black,
    );
    final bytes = GameHistoryCodec.encodeRecord(seal);
    expect(bytes[32], 6);
    final decoded =
        GameHistoryCodec.decodeRecord(bytes) as GameHistorySealRecord;
    expect(decoded.outcome, GameOutcome.resignation);
    expect(decoded.winner, GameWinner.black);
  });

  test('seal and typed control payloads reject inconsistent bounds', () {
    const GameHistorySealRecord seal = GameHistorySealRecord(
      outboxSequence: 3,
      boardGameId: '000102030405060708090a0b0c0d0e0f',
      lineRevision: 2,
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.checkmate,
      winner: GameWinner.white,
    );
    expect(GameHistoryCodec.encodeRecord(seal), hasLength(38));
    expect(
      () => const GameHistoryStateBoundsPayload(
        newestAllocated: 12,
        boardDurableAck: 13,
        lostGameCount: 0,
      ).encode(),
      throwsFormatException,
    );
    expect(
      () => const GameHistoryCommitPayload(
        firstOutboxSequence: 5,
        lastOutboxSequence: 4,
        crc32: 0,
      ).encode(),
      throwsFormatException,
    );
    final Uint8List reject = const GameHistoryRejectPayload(
      reason: GameHistoryRejectReason.crc,
      rejectedOpcode: 6,
      expectedFrameSequence: 9,
      detail: 0x01020304,
    ).encode();
    final GameHistoryRejectPayload decoded = GameHistoryRejectPayload.decode(
      reject,
    );
    expect(decoded.expectedFrameSequence, 9);
    expect(decoded.detail, 0x01020304);
  });
}

Uint8List _hexBytes(String hex) {
  final Uint8List bytes = Uint8List(hex.length ~/ 2);
  for (int index = 0; index < bytes.length; index += 1) {
    bytes[index] = int.parse(
      hex.substring(index * 2, index * 2 + 2),
      radix: 16,
    );
  }
  return bytes;
}
