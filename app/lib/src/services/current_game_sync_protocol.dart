import 'dart:typed_data';

import '../models/game_archive.dart';
import 'game_history_protocol.dart';

const int currentGameSyncPresentFlag = 1 << 0;
const int currentGameSyncArchiveRequiredFlag = 1 << 1;
const int currentGameSyncSealedFlag = 1 << 2;
const int currentGameSyncKnownFlags = 0x07;
const int currentGameSyncMaximumLineBytes =
    81 + gameArchiveMaximumPlies * gameHistoryMaximumRecordBytes + 38;
const int currentGameSyncMaximumLineRecords = gameArchiveMaximumPlies + 2;

enum CurrentGameSyncOpcode {
  begin(0x20),
  state(0x21),
  bounds(0x22),
  segmentBegin(0x23),
  data(0x24),
  segmentCommit(0x25),
  segmentCommitted(0x26),
  archiveAck(0x27),
  archived(0x28);

  const CurrentGameSyncOpcode(this.wireValue);
  final int wireValue;
}

class CurrentGameResumeCursor {
  const CurrentGameResumeCursor({
    required this.nextRecordSequence,
    required this.prefixFingerprint,
  });

  const CurrentGameResumeCursor.empty()
    : nextRecordSequence = 0,
      prefixFingerprint = const <int>[0, 0, 0, 0, 0, 0, 0, 0];

  final int nextRecordSequence;
  final List<int> prefixFingerprint;

  Uint8List encode() {
    _uint32(nextRecordSequence, 'next record sequence');
    _fingerprint(prefixFingerprint, 8, 'prefix fingerprint');
    final bool absent = prefixFingerprint.every((int byte) => byte == 0);
    if ((nextRecordSequence == 0) != absent ||
        nextRecordSequence > currentGameSyncMaximumLineRecords + 1) {
      throw const FormatException('Invalid current-game resume cursor.');
    }
    final Uint8List bytes = Uint8List(12);
    final ByteData data = ByteData.sublistView(bytes);
    data.setUint32(0, nextRecordSequence, Endian.little);
    bytes.setRange(4, 12, prefixFingerprint);
    return bytes;
  }

  factory CurrentGameResumeCursor.decode(Uint8List bytes) {
    _length(bytes, 'resume cursor');
    final ByteData data = ByteData.sublistView(bytes);
    final CurrentGameResumeCursor cursor = CurrentGameResumeCursor(
      nextRecordSequence: data.getUint32(0, Endian.little),
      prefixFingerprint: bytes.sublist(4, 12),
    );
    cursor.encode();
    return cursor;
  }
}

class CurrentGameDigest {
  const CurrentGameDigest({required this.fingerprint});

  const CurrentGameDigest.absent()
    : fingerprint = const <int>[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];

  final List<int> fingerprint;

  Uint8List encode({required bool allowAbsent}) {
    _fingerprint(fingerprint, 12, 'line fingerprint');
    final bool absent = fingerprint.every((int byte) => byte == 0);
    if (absent && !allowAbsent) {
      throw const FormatException('Invalid current-game digest.');
    }
    return Uint8List.fromList(fingerprint);
  }

  factory CurrentGameDigest.decode(
    Uint8List bytes, {
    required bool allowAbsent,
  }) {
    _length(bytes, 'digest');
    final CurrentGameDigest digest = CurrentGameDigest(
      fingerprint: bytes.toList(growable: false),
    );
    digest.encode(allowAbsent: allowAbsent);
    return digest;
  }
}

class CurrentGameBounds {
  const CurrentGameBounds({
    required this.lineRevision,
    required this.recordCount,
    required this.flags,
  });

  const CurrentGameBounds.absent()
    : lineRevision = 0,
      recordCount = 0,
      flags = 0;

  final int lineRevision;
  final int recordCount;
  final int flags;

  bool get present => (flags & currentGameSyncPresentFlag) != 0;
  bool get archiveRequired => (flags & currentGameSyncArchiveRequiredFlag) != 0;

  Uint8List encode() {
    final int maximumRecords = (flags & currentGameSyncSealedFlag) != 0
        ? currentGameSyncMaximumLineRecords
        : currentGameSyncMaximumLineRecords - 1;
    if (lineRevision < 0 ||
        lineRevision > 0x7fffffffffffffff ||
        recordCount < 0 ||
        recordCount > maximumRecords ||
        (flags & ~currentGameSyncKnownFlags) != 0 ||
        (!present && (lineRevision != 0 || recordCount != 0 || flags != 0)) ||
        (present &&
            (lineRevision == 0 ||
                recordCount == 0 ||
                (flags & currentGameSyncSealedFlag) != 0 && recordCount < 2))) {
      throw const FormatException('Invalid current-game bounds.');
    }
    final Uint8List bytes = Uint8List(12);
    final ByteData data = ByteData.sublistView(bytes);
    data.setUint64(0, lineRevision, Endian.little);
    data.setUint16(8, recordCount, Endian.little);
    bytes[10] = flags;
    return bytes;
  }

  factory CurrentGameBounds.decode(Uint8List bytes) {
    _length(bytes, 'bounds');
    if (bytes[11] != 0) {
      throw const FormatException(
        'Current-game bounds reserved byte is nonzero.',
      );
    }
    final ByteData data = ByteData.sublistView(bytes);
    final CurrentGameBounds bounds = CurrentGameBounds(
      lineRevision: data.getUint64(0, Endian.little),
      recordCount: data.getUint16(8, Endian.little),
      flags: bytes[10],
    );
    bounds.encode();
    return bounds;
  }
}

void _length(Uint8List bytes, String field) {
  if (bytes.length != 12) throw FormatException('Invalid $field length.');
}

void _uint32(int value, String field) {
  if (value < 0 || value > 0xffffffff) {
    throw FormatException('Invalid $field.');
  }
}

void _fingerprint(List<int> value, int length, String field) {
  if (value.length != length ||
      value.any((int byte) => byte < 0 || byte > 255)) {
    throw FormatException('Invalid $field.');
  }
}
