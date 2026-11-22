import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';

void main() {
  late Map<String, Uint8List> fixtures;
  late Map<String, Uint8List> historyFixtures;

  setUpAll(() {
    fixtures = <String, Uint8List>{};
    for (final String line in File(
      '../test/fixtures/current_game_sync_v1.txt',
    ).readAsLinesSync()) {
      if (line.isEmpty || line.startsWith('#')) continue;
      final int separator = line.indexOf('=');
      final String hex = line.substring(separator + 1);
      fixtures[line.substring(0, separator)] = Uint8List.fromList(<int>[
        for (int index = 0; index < hex.length; index += 2)
          int.parse(hex.substring(index, index + 2), radix: 16),
      ]);
    }
    historyFixtures = <String, Uint8List>{};
    for (final String line in File(
      '../test/fixtures/game_history_v1.txt',
    ).readAsLinesSync()) {
      if (line.isEmpty || line.startsWith('#')) continue;
      final int separator = line.indexOf('=');
      final String hex = line.substring(separator + 1);
      historyFixtures[line.substring(0, separator)] = Uint8List.fromList(<int>[
        for (int index = 0; index < hex.length; index += 2)
          int.parse(hex.substring(index, index + 2), radix: 16),
      ]);
    }
  });

  test('resume cursor is bounded, resumable, and little endian', () {
    const CurrentGameResumeCursor cursor = CurrentGameResumeCursor(
      nextRecordSequence: 2,
      prefixFingerprint: <int>[0x66, 0x03, 0x73, 0xe8, 0x1a, 0x8c, 0x0e, 0xca],
    );
    final Uint8List bytes = cursor.encode();
    expect(bytes, fixtures['resume_seq_2_base_sha256_64']);
    expect(CurrentGameResumeCursor.decode(bytes).nextRecordSequence, 2);
    expect(
      () => const CurrentGameResumeCursor(
        nextRecordSequence: 1,
        prefixFingerprint: <int>[0, 0, 0, 0, 0, 0, 0, 0],
      ).encode(),
      throwsFormatException,
    );
    expect(
      () => const CurrentGameResumeCursor(
        nextRecordSequence: currentGameSyncMaximumLineRecords + 2,
        prefixFingerprint: <int>[1, 2, 3, 4, 5, 6, 7, 8],
      ).encode(),
      throwsFormatException,
    );
  });

  test('state permits absence while archive ACK requires an exact digest', () {
    expect(
      const CurrentGameDigest.absent().encode(allowAbsent: true),
      Uint8List(12),
    );
    expect(
      () => const CurrentGameDigest.absent().encode(allowAbsent: false),
      throwsFormatException,
    );
    const CurrentGameDigest digest = CurrentGameDigest(
      fingerprint: <int>[
        0xc6,
        0xed,
        0xe8,
        0xe1,
        0x14,
        0x70,
        0x33,
        0x54,
        0x8a,
        0xb6,
        0x39,
        0x58,
      ],
    );
    expect(
      digest.encode(allowAbsent: false),
      fixtures['digest_base_move_sha256_96'],
    );
    expect(
      CurrentGameDigest.decode(
        digest.encode(allowAbsent: false),
        allowAbsent: false,
      ).fingerprint,
      digest.fingerprint,
    );
    expect(
      () => const CurrentGameDigest(
        fingerprint: <int>[1, 2, 3],
      ).encode(allowAbsent: false),
      throwsFormatException,
    );
  });

  test('real record fingerprints preserve append and fence rewrites', () {
    final Uint8List base = historyFixtures['record_line_base_seq_1']!;
    final Uint8List move = historyFixtures['record_committed_ply_seq_2']!;
    final Uint8List appended = Uint8List.fromList(<int>[...base, ...move]);

    List<int> leadingSha(Uint8List value, int length) =>
        sha256.convert(value).bytes.take(length).toList(growable: false);

    expect(
      CurrentGameResumeCursor(
        nextRecordSequence: 1,
        prefixFingerprint: leadingSha(Uint8List(0), 8),
      ).encode(),
      fixtures['resume_seq_1_empty_sha256_64'],
    );
    expect(
      CurrentGameResumeCursor(
        nextRecordSequence: 2,
        prefixFingerprint: leadingSha(base, 8),
      ).encode(),
      fixtures['resume_seq_2_base_sha256_64'],
    );
    expect(
      CurrentGameResumeCursor(
        nextRecordSequence: 3,
        prefixFingerprint: leadingSha(appended, 8),
      ).encode(),
      fixtures['resume_seq_3_base_move_sha256_64'],
    );
    expect(
      CurrentGameDigest(
        fingerprint: leadingSha(appended, 12),
      ).encode(allowAbsent: false),
      fixtures['digest_base_move_sha256_96'],
    );

    Uint8List validMutation(void Function(Uint8List bytes) mutate) {
      final Uint8List bytes = Uint8List.fromList(base);
      mutate(bytes);
      final int crc = GameHistoryCrc32.compute(
        Uint8List.sublistView(bytes, 0, bytes.length - 4),
      );
      ByteData.sublistView(
        bytes,
      ).setUint32(bytes.length - 4, crc, Endian.little);
      return bytes;
    }

    final Uint8List undoRewrite = validMutation((Uint8List bytes) {
      ByteData.sublistView(bytes).setUint64(23, 2, Endian.little);
    });
    final Uint8List replacement = validMutation((Uint8List bytes) {
      bytes[7] = 0x44;
    });
    expect(GameHistoryCodec.decodeRecord(undoRewrite).lineRevision, 2);
    expect(
      GameHistoryCodec.decodeRecord(replacement).boardGameId,
      startsWith('44'),
    );
    expect(
      GameHistoryCrc32.compute(base),
      GameHistoryCrc32.compute(replacement),
    );
    expect(
      CurrentGameResumeCursor(
        nextRecordSequence: 2,
        prefixFingerprint: leadingSha(undoRewrite, 8),
      ).encode(),
      fixtures['resume_seq_2_undo_base_sha256_64'],
    );
    expect(
      CurrentGameResumeCursor(
        nextRecordSequence: 2,
        prefixFingerprint: leadingSha(replacement, 8),
      ).encode(),
      fixtures['resume_seq_2_replacement_base_sha256_64'],
    );
  });

  test('current controls fill one minimum-ATT frame', () {
    final Uint8List beginPayload = CurrentGameResumeCursor(
      nextRecordSequence: 2,
      prefixFingerprint: fixtures['resume_seq_2_base_sha256_64']!.sublist(4),
    ).encode();
    final GameHistoryFrame begin = GameHistoryFrame(
      opcode: CurrentGameSyncOpcode.begin.wireValue,
      transactionId: 0x1234,
      frameSequence: 0,
      payload: beginPayload,
    );
    expect(begin.encode(), fixtures['frame_begin_tx_4660_seq_2']);
    expect(begin.encode().length, gameHistoryMinimumAttPayloadBytes);

    final GameHistoryFrame state = GameHistoryFrame(
      opcode: CurrentGameSyncOpcode.state.wireValue,
      transactionId: 0x1234,
      frameSequence: 0,
      payload: fixtures['digest_base_move_sha256_96']!,
    );
    expect(state.encode(), fixtures['frame_state_tx_4660_base_move']);
  });

  test('bounds carry exact revision, record count, and flags', () {
    const CurrentGameBounds bounds = CurrentGameBounds(
      lineRevision: 0x0102030405060708,
      recordCount: currentGameSyncMaximumLineRecords,
      flags:
          currentGameSyncPresentFlag |
          currentGameSyncArchiveRequiredFlag |
          currentGameSyncSealedFlag,
    );
    expect(
      bounds.encode(),
      fixtures['bounds_revision_72623859790382856_records_4098'],
    );
    final CurrentGameBounds decoded = CurrentGameBounds.decode(bounds.encode());
    expect(decoded.lineRevision, bounds.lineRevision);
    expect(decoded.recordCount, currentGameSyncMaximumLineRecords);
    expect(decoded.present, isTrue);
    expect(decoded.archiveRequired, isTrue);
    expect(
      () => const CurrentGameBounds(
        lineRevision: 1,
        recordCount: 0,
        flags: currentGameSyncArchiveRequiredFlag,
      ).encode(),
      throwsFormatException,
    );
    expect(
      const CurrentGameBounds(
        lineRevision: 1,
        recordCount: currentGameSyncMaximumLineRecords - 1,
        flags: currentGameSyncPresentFlag,
      ).encode(),
      hasLength(12),
    );
    expect(
      () => const CurrentGameBounds(
        lineRevision: 1,
        recordCount: currentGameSyncMaximumLineRecords,
        flags: currentGameSyncPresentFlag,
      ).encode(),
      throwsFormatException,
    );
    expect(
      () => const CurrentGameBounds(
        lineRevision: 1,
        recordCount: 1,
        flags: currentGameSyncPresentFlag | currentGameSyncSealedFlag,
      ).encode(),
      throwsFormatException,
    );
    final Uint8List reserved = bounds.encode()..[11] = 1;
    expect(() => CurrentGameBounds.decode(reserved), throwsFormatException);
  });

  test('current-line opcodes occupy one disjoint range', () {
    expect(CurrentGameSyncOpcode.begin.wireValue, 0x20);
    expect(CurrentGameSyncOpcode.archived.wireValue, 0x28);
    expect(
      CurrentGameSyncOpcode.values
          .map((CurrentGameSyncOpcode value) => value.wireValue)
          .toSet(),
      hasLength(CurrentGameSyncOpcode.values.length),
    );
  });
}
