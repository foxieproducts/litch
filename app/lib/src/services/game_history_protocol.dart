import 'dart:typed_data';

import '../models/game_archive.dart';

const int gameHistoryProtocolVersion = 1;
const int gameHistoryFrameMagic = 0x4c;
const int gameHistoryFrameHeaderBytes = 8;
const int gameHistoryMinimumAttPayloadBytes = 20;
const int gameHistoryMaximumFrameBytes = 512;
const int gameHistoryPositionBytes = 43;
const int gameHistoryRecordHeaderBytes = 7;
const int gameHistoryRecordCrcBytes = 4;
const int gameHistoryMaximumRecordBytes = 89;
const int gameHistoryMaximumSegmentBytes = 2048;
const int gameHistoryMaximumSegmentTransitions = 32;
const int gameHistoryControlPayloadBytes = 12;
const int gameHistoryMaximumRestoreTransitions = 128;
const int gameHistoryMaximumRestoreRecords =
    gameHistoryMaximumRestoreTransitions + 1;
const int gameHistoryLineBaseRecordBytes = 81;
const int gameHistoryMinimumCommittedPlyRecordBytes = 83;
const int gameHistoryMaximumCommittedPlyRecordBytes = 89;
const int gameHistoryMaximumRestoreUploadBytes =
    gameHistoryLineBaseRecordBytes +
    gameHistoryMaximumRestoreTransitions *
        gameHistoryMaximumCommittedPlyRecordBytes;
const int gameHistoryMaximumRestorePly = 4096;
const int gameHistoryInvalidSquare = 0xff;
const int gameHistorySegmentFlagAcknowledgedOverlap = 1;

const int gameHistoryStateFlagJournalMatched = 1;
const int gameHistoryStateFlagCursorReconciled = 1 << 1;
const int gameHistoryStateFlagSegmentFollows = 1 << 2;
const int gameHistoryStateFlagNoRecords = 1 << 3;
const int gameHistoryStateKnownFlags = 0x0f;

enum GameHistoryRecordType { lineBase, committedPly, seal }

enum GameHistoryOpcode {
  begin(0x01),
  state(0x02),
  stateBounds(0x03),
  segmentBegin(0x04),
  data(0x05),
  commit(0x06),
  committed(0x07),
  restoreBegin(0x10),
  restoreReady(0x11),
  restoreManifest(0x12),
  restoreData(0x13),
  restoreCommit(0x14),
  restoreAccepted(0x15),
  currentBegin(0x20),
  currentState(0x21),
  currentBounds(0x22),
  currentSegmentBegin(0x23),
  currentData(0x24),
  currentSegmentCommit(0x25),
  currentSegmentCommitted(0x26),
  currentArchiveAck(0x27),
  currentArchived(0x28),
  reject(0x7f);

  const GameHistoryOpcode(this.wireValue);

  final int wireValue;

  static GameHistoryOpcode? tryFromWire(int value) {
    for (final GameHistoryOpcode opcode in values) {
      if (opcode.wireValue == value) {
        return opcode;
      }
    }
    return null;
  }
}

class GameHistoryRestoreCursorPayload {
  const GameHistoryRestoreCursorPayload({
    required this.journalId,
    required this.durableAck,
  });

  final String journalId;
  final int durableAck;

  Uint8List encode() {
    _validateJournalId(journalId);
    _requireUint32(durableAck, field: 'durable acknowledgement');
    if (durableAck == 0) {
      throw const FormatException(
        'Restore requires a durably archived current board game.',
      );
    }
    final Uint8List bytes = Uint8List(gameHistoryControlPayloadBytes);
    bytes.setRange(0, 8, _encodeJournalId(journalId));
    _writeUint32(bytes, 8, durableAck);
    return bytes;
  }

  factory GameHistoryRestoreCursorPayload.decode(Uint8List bytes) {
    _requireLength(bytes, gameHistoryControlPayloadBytes, 'restore cursor');
    return GameHistoryRestoreCursorPayload(
      journalId: _decodeJournalId(Uint8List.sublistView(bytes, 0, 8)),
      durableAck: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryRestoreManifestPayload {
  const GameHistoryRestoreManifestPayload({
    required this.encodedBytes,
    required this.recordCount,
    required this.variant,
    required this.basePly,
    required this.currentPly,
    required this.crc32,
  });

  final int encodedBytes;
  final int recordCount;
  final GameVariant variant;
  final int basePly;
  final int currentPly;
  final int crc32;

  int get transitionCount => recordCount - 1;

  Uint8List encode() {
    if (recordCount <= 0 || recordCount > gameHistoryMaximumRestoreRecords) {
      throw const FormatException('Restore record count is out of range.');
    }
    if (basePly < 0 ||
        currentPly < basePly ||
        currentPly > gameHistoryMaximumRestorePly ||
        currentPly - basePly != transitionCount) {
      throw const FormatException('Restore ply bounds are inconsistent.');
    }
    final int minimumBytes =
        gameHistoryLineBaseRecordBytes +
        transitionCount * gameHistoryMinimumCommittedPlyRecordBytes;
    final int maximumBytes =
        gameHistoryLineBaseRecordBytes +
        transitionCount * gameHistoryMaximumCommittedPlyRecordBytes;
    if (encodedBytes < minimumBytes ||
        encodedBytes > maximumBytes ||
        encodedBytes > gameHistoryMaximumRestoreUploadBytes) {
      throw const FormatException('Restore upload size is inconsistent.');
    }
    _requireUint32(crc32, field: 'candidate CRC-32');
    final Uint8List bytes = Uint8List(gameHistoryControlPayloadBytes);
    _writeUint16(bytes, 0, encodedBytes);
    bytes[2] = recordCount;
    bytes[3] = variant == GameVariant.classic ? 1 : 2;
    _writeUint16(bytes, 4, basePly);
    _writeUint16(bytes, 6, currentPly);
    _writeUint32(bytes, 8, crc32);
    return bytes;
  }

  factory GameHistoryRestoreManifestPayload.decode(Uint8List bytes) {
    _requireLength(bytes, gameHistoryControlPayloadBytes, 'restore manifest');
    final GameVariant variant = switch (bytes[3]) {
      1 => GameVariant.classic,
      2 => GameVariant.duck,
      _ => throw const FormatException('Restore variant is unknown.'),
    };
    return GameHistoryRestoreManifestPayload(
      encodedBytes: _readUint16(bytes, 0),
      recordCount: bytes[2],
      variant: variant,
      basePly: _readUint16(bytes, 4),
      currentPly: _readUint16(bytes, 6),
      crc32: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryRestoreAcceptedPayload {
  const GameHistoryRestoreAcceptedPayload({
    required this.currentPly,
    required this.transitionCount,
    required this.variant,
    required this.encodedBytes,
    required this.crc32,
  });

  final int currentPly;
  final int transitionCount;
  final GameVariant variant;
  final int encodedBytes;
  final int crc32;

  Uint8List encode() {
    if (currentPly < 0 ||
        currentPly > gameHistoryMaximumRestorePly ||
        transitionCount < 0 ||
        transitionCount > gameHistoryMaximumRestoreTransitions ||
        transitionCount > currentPly) {
      throw const FormatException('Accepted restore ply bounds are invalid.');
    }
    final int minimumBytes =
        gameHistoryLineBaseRecordBytes +
        transitionCount * gameHistoryMinimumCommittedPlyRecordBytes;
    final int maximumBytes =
        gameHistoryLineBaseRecordBytes +
        transitionCount * gameHistoryMaximumCommittedPlyRecordBytes;
    if (encodedBytes < minimumBytes ||
        encodedBytes > maximumBytes ||
        encodedBytes > gameHistoryMaximumRestoreUploadBytes) {
      throw const FormatException('Accepted restore size is inconsistent.');
    }
    _requireUint32(crc32, field: 'accepted candidate CRC-32');
    final Uint8List bytes = Uint8List(gameHistoryControlPayloadBytes);
    _writeUint16(bytes, 0, currentPly);
    bytes[2] = transitionCount;
    bytes[3] = variant == GameVariant.classic ? 1 : 2;
    _writeUint16(bytes, 4, encodedBytes);
    _writeUint32(bytes, 8, crc32);
    return bytes;
  }

  factory GameHistoryRestoreAcceptedPayload.decode(Uint8List bytes) {
    _requireLength(bytes, gameHistoryControlPayloadBytes, 'restore accepted');
    if (bytes[6] != 0 || bytes[7] != 0) {
      throw const FormatException('Accepted restore reserved bytes are set.');
    }
    final GameVariant variant = switch (bytes[3]) {
      1 => GameVariant.classic,
      2 => GameVariant.duck,
      _ => throw const FormatException('Accepted restore variant is unknown.'),
    };
    return GameHistoryRestoreAcceptedPayload(
      currentPly: _readUint16(bytes, 0),
      transitionCount: bytes[2],
      variant: variant,
      encodedBytes: _readUint16(bytes, 4),
      crc32: _readUint32(bytes, 8),
    )..encode();
  }
}

enum GameHistoryRejectReason {
  unsupportedVersion,
  badState,
  staleTransaction,
  unavailableWatermark,
  sequence,
  bounds,
  crc,
  storage,
  busy,
  unsupportedOpcode,
}

class GameHistoryFrame {
  GameHistoryFrame({
    required this.opcode,
    required this.transactionId,
    required this.frameSequence,
    Uint8List? payload,
    this.flags = 0,
  }) : payload = Uint8List.fromList(payload ?? Uint8List(0)) {
    if (opcode < 0 || opcode > 0xff || flags < 0 || flags > 0xff) {
      throw const FormatException('Game-history frame byte is out of range.');
    }
    _requireUint16(transactionId, field: 'transaction identity');
    if (transactionId == 0) {
      throw const FormatException(
        'Game-history transaction identity cannot be zero.',
      );
    }
    _requireUint16(frameSequence, field: 'frame sequence');
    if (this.payload.length >
        gameHistoryMaximumFrameBytes - gameHistoryFrameHeaderBytes) {
      throw const FormatException('Game-history frame is too large.');
    }
  }

  final int opcode;
  final int flags;
  final int transactionId;
  final int frameSequence;
  final Uint8List payload;

  GameHistoryOpcode? get knownOpcode => GameHistoryOpcode.tryFromWire(opcode);

  Uint8List encode() {
    final Uint8List encoded = Uint8List(
      gameHistoryFrameHeaderBytes + payload.length,
    );
    encoded[0] = gameHistoryFrameMagic;
    encoded[1] = gameHistoryProtocolVersion;
    encoded[2] = opcode;
    encoded[3] = flags;
    _writeUint16(encoded, 4, transactionId);
    _writeUint16(encoded, 6, frameSequence);
    encoded.setRange(gameHistoryFrameHeaderBytes, encoded.length, payload);
    return encoded;
  }

  factory GameHistoryFrame.decode(Uint8List bytes) {
    if (bytes.length < gameHistoryFrameHeaderBytes ||
        bytes.length > gameHistoryMaximumFrameBytes ||
        bytes[0] != gameHistoryFrameMagic ||
        bytes[1] != gameHistoryProtocolVersion) {
      throw const FormatException('Invalid game-history frame envelope.');
    }
    return GameHistoryFrame(
      opcode: bytes[2],
      flags: bytes[3],
      transactionId: _readUint16(bytes, 4),
      frameSequence: _readUint16(bytes, 6),
      payload: Uint8List.sublistView(bytes, gameHistoryFrameHeaderBytes),
    );
  }
}

class GameHistoryBeginPayload {
  const GameHistoryBeginPayload({
    required this.journalId,
    required this.appDurableAck,
  });

  final String journalId;
  final int appDurableAck;

  Uint8List encode() {
    _validateJournalId(journalId, allowZero: true);
    _requireUint32(appDurableAck, field: 'app durable acknowledgement');
    final Uint8List bytes = Uint8List(12);
    bytes.setRange(0, 8, _encodeJournalId(journalId));
    _writeUint32(bytes, 8, appDurableAck);
    return bytes;
  }

  factory GameHistoryBeginPayload.decode(Uint8List bytes) {
    _requireLength(bytes, 12, 'begin payload');
    return GameHistoryBeginPayload(
      journalId: _decodeJournalId(Uint8List.sublistView(bytes, 0, 8)),
      appDurableAck: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryStatePayload {
  const GameHistoryStatePayload({
    required this.journalId,
    required this.oldestRetained,
  });

  final String journalId;
  final int oldestRetained;

  Uint8List encode() {
    _validateJournalId(journalId);
    _requireUint32(oldestRetained, field: 'oldest retained sequence');
    final Uint8List bytes = Uint8List(12);
    bytes.setRange(0, 8, _encodeJournalId(journalId));
    _writeUint32(bytes, 8, oldestRetained);
    return bytes;
  }

  factory GameHistoryStatePayload.decode(Uint8List bytes) {
    _requireLength(bytes, 12, 'state payload');
    return GameHistoryStatePayload(
      journalId: _decodeJournalId(Uint8List.sublistView(bytes, 0, 8)),
      oldestRetained: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryStateBoundsPayload {
  const GameHistoryStateBoundsPayload({
    required this.newestAllocated,
    required this.boardDurableAck,
    required this.lostGameCount,
  });

  final int newestAllocated;
  final int boardDurableAck;
  final int lostGameCount;

  Uint8List encode() {
    _requireUint32(newestAllocated, field: 'newest allocated sequence');
    _requireUint32(boardDurableAck, field: 'board durable acknowledgement');
    _requireUint32(lostGameCount, field: 'lost-game count');
    if (boardDurableAck > newestAllocated) {
      throw const FormatException(
        'Board acknowledgement exceeds allocated history.',
      );
    }
    final Uint8List bytes = Uint8List(12);
    _writeUint32(bytes, 0, newestAllocated);
    _writeUint32(bytes, 4, boardDurableAck);
    _writeUint32(bytes, 8, lostGameCount);
    return bytes;
  }

  factory GameHistoryStateBoundsPayload.decode(Uint8List bytes) {
    _requireLength(bytes, 12, 'state-bounds payload');
    return GameHistoryStateBoundsPayload(
      newestAllocated: _readUint32(bytes, 0),
      boardDurableAck: _readUint32(bytes, 4),
      lostGameCount: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistorySegmentDescriptor {
  const GameHistorySegmentDescriptor({
    required this.encodedBytes,
    required this.recordCount,
    required this.firstOutboxSequence,
    required this.crc32,
    this.flags = 0,
  });

  final int encodedBytes;
  final int recordCount;
  final int flags;
  final int firstOutboxSequence;
  final int crc32;

  int get lastOutboxSequence => firstOutboxSequence + recordCount - 1;

  Uint8List encode() {
    if (encodedBytes <= 0 || encodedBytes > gameHistoryMaximumSegmentBytes) {
      throw const FormatException('Game-history segment size is invalid.');
    }
    if (recordCount <= 0 ||
        recordCount > gameHistoryMaximumSegmentTransitions + 2) {
      throw const FormatException('Game-history record count is invalid.');
    }
    if ((flags & ~gameHistorySegmentFlagAcknowledgedOverlap) != 0) {
      throw const FormatException('Unknown game-history segment flags.');
    }
    _requireUint32(
      firstOutboxSequence,
      field: 'first outbox sequence',
      allowZero: false,
    );
    _requireUint32(lastOutboxSequence, field: 'last outbox sequence');
    _requireUint32(crc32, field: 'segment CRC-32');
    final Uint8List bytes = Uint8List(12);
    _writeUint16(bytes, 0, encodedBytes);
    bytes[2] = recordCount;
    bytes[3] = flags;
    _writeUint32(bytes, 4, firstOutboxSequence);
    _writeUint32(bytes, 8, crc32);
    return bytes;
  }

  factory GameHistorySegmentDescriptor.decode(Uint8List bytes) {
    _requireLength(bytes, 12, 'segment descriptor');
    return GameHistorySegmentDescriptor(
      encodedBytes: _readUint16(bytes, 0),
      recordCount: bytes[2],
      flags: bytes[3],
      firstOutboxSequence: _readUint32(bytes, 4),
      crc32: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryCommitPayload {
  const GameHistoryCommitPayload({
    required this.firstOutboxSequence,
    required this.lastOutboxSequence,
    required this.crc32,
  });

  final int firstOutboxSequence;
  final int lastOutboxSequence;
  final int crc32;

  Uint8List encode() {
    _requireUint32(
      firstOutboxSequence,
      field: 'first committed sequence',
      allowZero: false,
    );
    _requireUint32(lastOutboxSequence, field: 'last committed sequence');
    _requireUint32(crc32, field: 'committed segment CRC-32');
    if (lastOutboxSequence < firstOutboxSequence ||
        lastOutboxSequence - firstOutboxSequence >=
            gameHistoryMaximumSegmentTransitions + 2) {
      throw const FormatException('Game-history commit range is invalid.');
    }
    final Uint8List bytes = Uint8List(12);
    _writeUint32(bytes, 0, firstOutboxSequence);
    _writeUint32(bytes, 4, lastOutboxSequence);
    _writeUint32(bytes, 8, crc32);
    return bytes;
  }

  factory GameHistoryCommitPayload.decode(Uint8List bytes) {
    _requireLength(bytes, 12, 'commit payload');
    return GameHistoryCommitPayload(
      firstOutboxSequence: _readUint32(bytes, 0),
      lastOutboxSequence: _readUint32(bytes, 4),
      crc32: _readUint32(bytes, 8),
    )..encode();
  }
}

class GameHistoryRejectPayload {
  const GameHistoryRejectPayload({
    required this.reason,
    required this.rejectedOpcode,
    required this.expectedFrameSequence,
    required this.detail,
  });

  final GameHistoryRejectReason reason;
  final int rejectedOpcode;
  final int expectedFrameSequence;
  final int detail;

  Uint8List encode() {
    if (rejectedOpcode < 0 || rejectedOpcode > 0xff) {
      throw const FormatException('Rejected opcode is out of range.');
    }
    _requireUint16(expectedFrameSequence, field: 'expected frame sequence');
    _requireUint32(detail, field: 'reject detail');
    final Uint8List bytes = Uint8List(8);
    bytes[0] = reason.index + 1;
    bytes[1] = rejectedOpcode;
    _writeUint16(bytes, 2, expectedFrameSequence);
    _writeUint32(bytes, 4, detail);
    return bytes;
  }

  factory GameHistoryRejectPayload.decode(Uint8List bytes) {
    _requireLength(bytes, 8, 'reject payload');
    final int reasonIndex = bytes[0] - 1;
    if (reasonIndex < 0 ||
        reasonIndex >= GameHistoryRejectReason.values.length) {
      throw const FormatException('Unknown game-history rejection reason.');
    }
    return GameHistoryRejectPayload(
      reason: GameHistoryRejectReason.values[reasonIndex],
      rejectedOpcode: bytes[1],
      expectedFrameSequence: _readUint16(bytes, 2),
      detail: _readUint32(bytes, 4),
    );
  }
}

sealed class GameHistoryRecord {
  const GameHistoryRecord({
    required this.outboxSequence,
    required this.boardGameId,
    required this.lineRevision,
  });

  final int outboxSequence;
  final String boardGameId;
  final int lineRevision;

  GameHistoryRecordType get type;
}

class GameHistoryLineBaseRecord extends GameHistoryRecord {
  const GameHistoryLineBaseRecord({
    required super.outboxSequence,
    required super.boardGameId,
    required super.lineRevision,
    required this.variant,
    required this.position,
  });

  final GameVariant variant;
  final ArchivedPosition position;

  @override
  GameHistoryRecordType get type => GameHistoryRecordType.lineBase;
}

class GameHistoryCommittedPlyRecord extends GameHistoryRecord {
  const GameHistoryCommittedPlyRecord({
    required super.outboxSequence,
    required super.boardGameId,
    required super.lineRevision,
    required this.position,
    required this.transition,
  });

  final ArchivedPosition position;
  final ArchivedTransition transition;

  @override
  GameHistoryRecordType get type => GameHistoryRecordType.committedPly;
}

class GameHistorySealRecord extends GameHistoryRecord {
  const GameHistorySealRecord({
    required super.outboxSequence,
    required super.boardGameId,
    required super.lineRevision,
    required this.status,
    required this.outcome,
    required this.winner,
  });

  final GameArchiveStatus status;
  final GameOutcome outcome;
  final GameWinner winner;

  @override
  GameHistoryRecordType get type => GameHistoryRecordType.seal;
}

class GameHistoryCodec {
  const GameHistoryCodec._();

  static Uint8List encodePosition(
    ArchivedPosition position,
    GameVariant variant,
  ) {
    position.validate();
    final List<String> fields = position.fen.split(' ');
    if (fields.length != 6) {
      throw const FormatException('Archive position FEN must have six fields.');
    }
    if (variant == GameVariant.classic && position.duckSquare != null ||
        variant == GameVariant.duck && position.duckSquare == null) {
      throw const FormatException('Position variant and Duck square disagree.');
    }

    final List<int> squares = <int>[];
    for (final String rank in fields[0].split('/')) {
      int rankSquares = 0;
      for (final int token in rank.codeUnits) {
        if (token >= 0x31 && token <= 0x38) {
          final int empty = token - 0x30;
          squares.addAll(List<int>.filled(empty, 0));
          rankSquares += empty;
        } else {
          final int? piece = _pieceToWire[String.fromCharCode(token)];
          if (piece == null) {
            throw const FormatException('Position contains an unknown piece.');
          }
          squares.add(piece);
          rankSquares += 1;
        }
      }
      if (rankSquares != 8) {
        throw const FormatException(
          'Position rank does not contain eight squares.',
        );
      }
    }
    if (squares.length != 64) {
      throw const FormatException('Position does not contain eight ranks.');
    }

    final Uint8List encoded = Uint8List(gameHistoryPositionBytes);
    for (int index = 0; index < 32; index += 1) {
      encoded[index] = squares[index * 2] | (squares[index * 2 + 1] << 4);
    }
    int flags = fields[1] == 'b' ? 1 : 0;
    if (fields[1] != 'w' && fields[1] != 'b') {
      throw const FormatException('Position side to move is invalid.');
    }
    if (fields[2].contains('K')) flags |= 1 << 1;
    if (fields[2].contains('Q')) flags |= 1 << 2;
    if (fields[2].contains('k')) flags |= 1 << 3;
    if (fields[2].contains('q')) flags |= 1 << 4;
    encoded[32] = flags;
    encoded[33] = fields[3] == '-'
        ? gameHistoryInvalidSquare
        : _squareFromName(fields[3]);
    final int halfmove = int.parse(fields[4]);
    final int fullmove = int.parse(fields[5]);
    _requireUint32(halfmove, field: 'halfmove clock');
    _requireUint32(fullmove, field: 'fullmove number', allowZero: false);
    _writeUint32(encoded, 34, halfmove);
    _writeUint32(encoded, 38, fullmove);
    encoded[42] = position.duckSquare ?? gameHistoryInvalidSquare;
    if (position.duckSquare != null && squares[position.duckSquare!] != 0) {
      throw const FormatException('Duck square overlaps a chess piece.');
    }
    return encoded;
  }

  static ArchivedPosition decodePosition(Uint8List bytes, {required int ply}) {
    _requireLength(bytes, gameHistoryPositionBytes, 'position');
    final int flags = bytes[32];
    if ((flags & ~0x1f) != 0) {
      throw const FormatException('Position contains unknown flags.');
    }
    final int enPassant = bytes[33];
    if (enPassant != gameHistoryInvalidSquare &&
        (enPassant >= 64 || (enPassant ~/ 8 != 2 && enPassant ~/ 8 != 5))) {
      throw const FormatException('Position en-passant square is invalid.');
    }
    final int fullmove = _readUint32(bytes, 38);
    if (fullmove == 0) {
      throw const FormatException('Position fullmove number cannot be zero.');
    }
    final int duck = bytes[42];
    if (duck != gameHistoryInvalidSquare && duck >= 64) {
      throw const FormatException('Position Duck square is invalid.');
    }

    final StringBuffer placement = StringBuffer();
    for (int rank = 0; rank < 8; rank += 1) {
      int empty = 0;
      for (int file = 0; file < 8; file += 1) {
        final int square = rank * 8 + file;
        final int packed = bytes[square ~/ 2];
        final int code = square.isEven ? packed & 0x0f : packed >> 4;
        if (code == 0) {
          empty += 1;
          continue;
        }
        if (code > 12 || square == duck) {
          throw const FormatException('Position square encoding is invalid.');
        }
        if (empty != 0) {
          placement.write(empty);
          empty = 0;
        }
        placement.write(_wireToPiece[code]);
      }
      if (empty != 0) placement.write(empty);
      if (rank != 7) placement.write('/');
    }
    final StringBuffer castling = StringBuffer();
    if ((flags & (1 << 1)) != 0) castling.write('K');
    if ((flags & (1 << 2)) != 0) castling.write('Q');
    if ((flags & (1 << 3)) != 0) castling.write('k');
    if ((flags & (1 << 4)) != 0) castling.write('q');
    final String fen = <String>[
      placement.toString(),
      (flags & 1) == 0 ? 'w' : 'b',
      castling.isEmpty ? '-' : castling.toString(),
      enPassant == gameHistoryInvalidSquare ? '-' : _squareName(enPassant),
      _readUint32(bytes, 34).toString(),
      fullmove.toString(),
    ].join(' ');
    final ArchivedPosition position = ArchivedPosition(
      ply: ply,
      fen: fen,
      duckSquare: duck == gameHistoryInvalidSquare ? null : duck,
    );
    position.validate();
    return position;
  }

  static Uint8List encodeRecord(GameHistoryRecord record) {
    _validateRecordIdentity(record);
    final BytesBuilder payload = BytesBuilder(copy: false)
      ..add(_gameIdBytes(record.boardGameId))
      ..add(_encodeUint64(record.lineRevision));
    switch (record) {
      case GameHistoryLineBaseRecord lineBase:
        _requirePly(lineBase.position.ply);
        payload
          ..add(_encodeUint16(lineBase.position.ply))
          ..add(<int>[lineBase.variant == GameVariant.classic ? 1 : 2])
          ..add(encodePosition(lineBase.position, lineBase.variant));
      case GameHistoryCommittedPlyRecord committedPly:
        _requirePly(committedPly.position.ply, allowZero: false);
        payload
          ..add(_encodeUint16(committedPly.position.ply))
          ..add(
            encodePosition(
              committedPly.position,
              committedPly.position.duckSquare == null
                  ? GameVariant.classic
                  : GameVariant.duck,
            ),
          )
          ..add(_encodeTransition(committedPly.transition));
      case GameHistorySealRecord seal:
        _validateSeal(seal.status, seal.outcome, seal.winner);
        payload.add(<int>[
          seal.status == GameArchiveStatus.completed ? 1 : 2,
          _outcomeToWire(seal.outcome),
          _winnerToWire(seal.winner),
        ]);
    }
    final Uint8List payloadBytes = payload.takeBytes();
    if (payloadBytes.length > 0xff) {
      throw const FormatException('Game-history record payload is too large.');
    }
    final Uint8List encoded = Uint8List(
      gameHistoryRecordHeaderBytes +
          payloadBytes.length +
          gameHistoryRecordCrcBytes,
    );
    encoded[0] = gameHistoryProtocolVersion;
    encoded[1] = record.type.index + 1;
    encoded[2] = payloadBytes.length;
    _writeUint32(encoded, 3, record.outboxSequence);
    encoded.setRange(
      gameHistoryRecordHeaderBytes,
      gameHistoryRecordHeaderBytes + payloadBytes.length,
      payloadBytes,
    );
    _writeUint32(
      encoded,
      encoded.length - gameHistoryRecordCrcBytes,
      GameHistoryCrc32.compute(
        Uint8List.sublistView(encoded, 0, encoded.length - 4),
      ),
    );
    if (encoded.length > gameHistoryMaximumRecordBytes) {
      throw const FormatException('Game-history record is too large.');
    }
    return encoded;
  }

  static GameHistoryRecord decodeRecord(Uint8List bytes) {
    if (bytes.length <
            gameHistoryRecordHeaderBytes + gameHistoryRecordCrcBytes ||
        bytes.length > gameHistoryMaximumRecordBytes ||
        bytes[0] != gameHistoryProtocolVersion) {
      throw const FormatException('Invalid game-history record envelope.');
    }
    final int payloadLength = bytes[2];
    final int crcOffset = gameHistoryRecordHeaderBytes + payloadLength;
    if (crcOffset + gameHistoryRecordCrcBytes != bytes.length ||
        _readUint32(bytes, crcOffset) !=
            GameHistoryCrc32.compute(
              Uint8List.sublistView(bytes, 0, crcOffset),
            )) {
      throw const FormatException(
        'Game-history record CRC or length is invalid.',
      );
    }
    final int outboxSequence = _readUint32(bytes, 3);
    _requireUint32(outboxSequence, field: 'outbox sequence', allowZero: false);
    final Uint8List payload = Uint8List.sublistView(
      bytes,
      gameHistoryRecordHeaderBytes,
      crcOffset,
    );
    if (payload.length < 24) {
      throw const FormatException('Game-history record payload is truncated.');
    }
    final String boardGameId = _hex(Uint8List.sublistView(payload, 0, 16));
    final int lineRevision = _readUint64(payload, 16);
    if (boardGameId == '00000000000000000000000000000000') {
      throw const FormatException('Board-game identity is invalid.');
    }
    if (lineRevision < 0 || lineRevision > 0x7fffffffffffffff) {
      throw const FormatException('Line revision exceeds app-safe range.');
    }
    switch (bytes[1]) {
      case 1:
        if (payload.length != 70) {
          throw const FormatException('Line-base record length is invalid.');
        }
        final int ply = _readUint16(payload, 24);
        _requirePly(ply);
        final GameVariant variant = switch (payload[26]) {
          1 => GameVariant.classic,
          2 => GameVariant.duck,
          _ => throw const FormatException('Game variant is unknown.'),
        };
        final ArchivedPosition position = decodePosition(
          Uint8List.sublistView(payload, 27),
          ply: ply,
        );
        if ((variant == GameVariant.classic) != (position.duckSquare == null)) {
          throw const FormatException(
            'Line-base variant does not match position.',
          );
        }
        return GameHistoryLineBaseRecord(
          outboxSequence: outboxSequence,
          boardGameId: boardGameId,
          lineRevision: lineRevision,
          variant: variant,
          position: position,
        );
      case 2:
        if (payload.length < 72 || payload.length > 78) {
          throw const FormatException(
            'Committed-ply record length is invalid.',
          );
        }
        final int ply = _readUint16(payload, 24);
        _requirePly(ply, allowZero: false);
        return GameHistoryCommittedPlyRecord(
          outboxSequence: outboxSequence,
          boardGameId: boardGameId,
          lineRevision: lineRevision,
          position: decodePosition(
            Uint8List.sublistView(payload, 26, 69),
            ply: ply,
          ),
          transition: _decodeTransition(Uint8List.sublistView(payload, 69)),
        );
      case 3:
        if (payload.length != 27) {
          throw const FormatException('Seal record length is invalid.');
        }
        final GameArchiveStatus status = switch (payload[24]) {
          1 => GameArchiveStatus.completed,
          2 => GameArchiveStatus.unfinished,
          _ => throw const FormatException('Archive status is unknown.'),
        };
        final GameOutcome outcome = _outcomeFromWire(payload[25]);
        final GameWinner winner = _winnerFromWire(payload[26]);
        _validateSeal(status, outcome, winner);
        return GameHistorySealRecord(
          outboxSequence: outboxSequence,
          boardGameId: boardGameId,
          lineRevision: lineRevision,
          status: status,
          outcome: outcome,
          winner: winner,
        );
      default:
        throw const FormatException('Game-history record type is unknown.');
    }
  }

  static List<GameHistoryRecord> decodeSegment(
    Uint8List bytes,
    GameHistorySegmentDescriptor descriptor,
  ) {
    descriptor.encode();
    if (bytes.length != descriptor.encodedBytes ||
        GameHistoryCrc32.compute(bytes) != descriptor.crc32) {
      throw const FormatException(
        'Game-history segment CRC or size is invalid.',
      );
    }
    final List<GameHistoryRecord> records = <GameHistoryRecord>[];
    int offset = 0;
    while (offset < bytes.length) {
      if (bytes.length - offset <
          gameHistoryRecordHeaderBytes + gameHistoryRecordCrcBytes) {
        throw const FormatException('Game-history segment ends mid-record.');
      }
      final int recordLength =
          gameHistoryRecordHeaderBytes + bytes[offset + 2] + 4;
      if (recordLength > gameHistoryMaximumRecordBytes ||
          offset + recordLength > bytes.length) {
        throw const FormatException(
          'Game-history segment record is truncated.',
        );
      }
      records.add(
        decodeRecord(
          Uint8List.sublistView(bytes, offset, offset + recordLength),
        ),
      );
      offset += recordLength;
    }
    if (records.length != descriptor.recordCount) {
      throw const FormatException('Game-history segment record count differs.');
    }
    for (int index = 0; index < records.length; index += 1) {
      if (records[index].outboxSequence !=
          descriptor.firstOutboxSequence + index) {
        throw const FormatException(
          'Game-history segment records are duplicated or reordered.',
        );
      }
      if (records[index].boardGameId != records.first.boardGameId) {
        throw const FormatException(
          'Game-history segment crosses game identities.',
        );
      }
    }
    return List<GameHistoryRecord>.unmodifiable(records);
  }
}

class GameHistoryCrc32 {
  GameHistoryCrc32() : _state = 0xffffffff;

  int _state;

  void reset() {
    _state = 0xffffffff;
  }

  void update(List<int> bytes) {
    for (final int byte in bytes) {
      if (byte < 0 || byte > 0xff) {
        throw const FormatException('CRC input byte is out of range.');
      }
      _state ^= byte;
      for (int bit = 0; bit < 8; bit += 1) {
        _state = (_state >>> 1) ^ ((_state & 1) == 0 ? 0 : 0xedb88320);
      }
    }
  }

  int get value => (~_state) & 0xffffffff;

  static int compute(List<int> bytes) {
    final GameHistoryCrc32 crc = GameHistoryCrc32()..update(bytes);
    return crc.value;
  }
}

const Map<String, int> _pieceToWire = <String, int>{
  'P': 1,
  'N': 2,
  'B': 3,
  'R': 4,
  'Q': 5,
  'K': 6,
  'p': 7,
  'n': 8,
  'b': 9,
  'r': 10,
  'q': 11,
  'k': 12,
};
const List<String> _wireToPiece = <String>[
  '',
  'P',
  'N',
  'B',
  'R',
  'Q',
  'K',
  'p',
  'n',
  'b',
  'r',
  'q',
  'k',
];

Uint8List _encodeTransition(ArchivedTransition transition) {
  transition.validate();
  int flags = transition.side == GameSide.black ? 1 : 0;
  final List<int> bytes = <int>[0, transition.from, transition.to];
  if (transition.captureSquare != null) {
    flags |= 1 << 1;
    bytes.add(transition.captureSquare!);
  }
  if (transition.promotion != null) {
    flags |= 1 << 2;
    bytes.add(transition.promotion!.index + 1);
  }
  if (transition.secondaryFrom != null) {
    flags |= 1 << 3;
    bytes
      ..add(transition.secondaryFrom!)
      ..add(transition.secondaryTo!);
  }
  if (transition.duckFrom != null) {
    flags |= 1 << 4;
    bytes
      ..add(transition.duckFrom!)
      ..add(transition.duckTo!);
  }
  bytes[0] = flags;
  return Uint8List.fromList(bytes);
}

ArchivedTransition _decodeTransition(Uint8List bytes) {
  if (bytes.length < 3 || bytes.length > 9 || (bytes[0] & ~0x1f) != 0) {
    throw const FormatException('Transition envelope is invalid.');
  }
  final int flags = bytes[0];
  int offset = 3;
  int? readOptional(int flag, {int length = 1}) {
    if ((flags & flag) == 0) return null;
    if (offset + length > bytes.length) {
      throw const FormatException('Transition is truncated.');
    }
    final int value = bytes[offset];
    offset += length;
    return value;
  }

  final int? capture = readOptional(1 << 1);
  final int? promotionWire = readOptional(1 << 2);
  final int? secondaryFrom = readOptional(1 << 3, length: 2);
  final int? secondaryTo = secondaryFrom == null ? null : bytes[offset - 1];
  final int? duckFrom = readOptional(1 << 4, length: 2);
  final int? duckTo = duckFrom == null ? null : bytes[offset - 1];
  if (offset != bytes.length ||
      promotionWire != null && (promotionWire < 1 || promotionWire > 4)) {
    throw const FormatException('Transition fields are invalid.');
  }
  final ArchivedTransition transition = ArchivedTransition(
    side: (flags & 1) == 0 ? GameSide.white : GameSide.black,
    from: bytes[1],
    to: bytes[2],
    captureSquare: capture,
    promotion: promotionWire == null
        ? null
        : PromotionPiece.values[promotionWire - 1],
    secondaryFrom: secondaryFrom,
    secondaryTo: secondaryTo,
    duckFrom: duckFrom,
    duckTo: duckTo,
  );
  transition.validate();
  return transition;
}

void _validateRecordIdentity(GameHistoryRecord record) {
  _requireUint32(
    record.outboxSequence,
    field: 'outbox sequence',
    allowZero: false,
  );
  if (!RegExp(r'^[0-9a-f]{32}$').hasMatch(record.boardGameId) ||
      record.boardGameId == '00000000000000000000000000000000') {
    throw const FormatException('Board-game identity is invalid.');
  }
  if (record.lineRevision < 0 || record.lineRevision > 0x7fffffffffffffff) {
    throw const FormatException('Line revision exceeds app-safe range.');
  }
}

void _validateSeal(
  GameArchiveStatus status,
  GameOutcome outcome,
  GameWinner winner,
) {
  if (status == GameArchiveStatus.active ||
      status == GameArchiveStatus.unfinished &&
          (outcome != GameOutcome.ongoing || winner != GameWinner.none) ||
      status == GameArchiveStatus.completed &&
          (outcome == GameOutcome.ongoing ||
              outcome == GameOutcome.draw && winner != GameWinner.draw ||
              outcome != GameOutcome.draw &&
                  winner != GameWinner.white &&
                  winner != GameWinner.black)) {
    throw const FormatException('Archive seal is inconsistent.');
  }
}

int _outcomeToWire(GameOutcome value) => switch (value) {
  GameOutcome.ongoing => 0,
  GameOutcome.checkmate => 1,
  GameOutcome.draw => 2,
  GameOutcome.kingCaptured => 3,
  GameOutcome.fowled => 4,
  GameOutcome.timeout => 5,
  GameOutcome.resignation => 6,
  GameOutcome.aborted ||
  GameOutcome.providerDefined => throw const FormatException(
    'Provider outcomes are not native history wire values.',
  ),
};

GameOutcome _outcomeFromWire(int value) => switch (value) {
  0 => GameOutcome.ongoing,
  1 => GameOutcome.checkmate,
  2 => GameOutcome.draw,
  3 => GameOutcome.kingCaptured,
  4 => GameOutcome.fowled,
  5 => GameOutcome.timeout,
  6 => GameOutcome.resignation,
  _ => throw const FormatException('Game outcome is unknown.'),
};

int _winnerToWire(GameWinner value) => switch (value) {
  GameWinner.none => 0,
  GameWinner.white => 1,
  GameWinner.black => 2,
  GameWinner.draw => 3,
};

GameWinner _winnerFromWire(int value) => switch (value) {
  0 => GameWinner.none,
  1 => GameWinner.white,
  2 => GameWinner.black,
  3 => GameWinner.draw,
  _ => throw const FormatException('Game winner is unknown.'),
};

int _squareFromName(String value) {
  if (!RegExp(r'^[a-h][1-8]$').hasMatch(value)) {
    throw const FormatException('Square name is invalid.');
  }
  return (8 - int.parse(value[1])) * 8 + value.codeUnitAt(0) - 0x61;
}

String _squareName(int square) =>
    '${String.fromCharCode(0x61 + square % 8)}${8 - square ~/ 8}';

Uint8List _gameIdBytes(String value) {
  final Uint8List bytes = Uint8List(16);
  for (int index = 0; index < 16; index += 1) {
    bytes[index] = int.parse(
      value.substring(index * 2, index * 2 + 2),
      radix: 16,
    );
  }
  return bytes;
}

String _hex(List<int> bytes) =>
    bytes.map((int byte) => byte.toRadixString(16).padLeft(2, '0')).join();

Uint8List _encodeUint16(int value) {
  final Uint8List bytes = Uint8List(2);
  _writeUint16(bytes, 0, value);
  return bytes;
}

Uint8List _encodeUint64(int value) {
  final Uint8List bytes = Uint8List(8);
  _writeUint64(bytes, 0, value);
  return bytes;
}

void _writeUint16(Uint8List bytes, int offset, int value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
}

void _writeUint32(Uint8List bytes, int offset, int value) {
  for (int index = 0; index < 4; index += 1) {
    bytes[offset + index] = (value >>> (index * 8)) & 0xff;
  }
}

void _writeUint64(Uint8List bytes, int offset, int value) {
  for (int index = 0; index < 8; index += 1) {
    bytes[offset + index] = (value >>> (index * 8)) & 0xff;
  }
}

int _readUint16(Uint8List bytes, int offset) =>
    bytes[offset] | bytes[offset + 1] << 8;

int _readUint32(Uint8List bytes, int offset) {
  int value = 0;
  for (int index = 0; index < 4; index += 1) {
    value |= bytes[offset + index] << (index * 8);
  }
  return value;
}

int _readUint64(Uint8List bytes, int offset) {
  int value = 0;
  for (int index = 0; index < 8; index += 1) {
    value |= bytes[offset + index] << (index * 8);
  }
  return value;
}

void _requireLength(Uint8List bytes, int length, String field) {
  if (bytes.length != length) {
    throw FormatException('Game-history $field must be $length bytes.');
  }
}

void _requirePly(int value, {bool allowZero = true}) {
  if (value < (allowZero ? 0 : 1) || value > gameArchiveMaximumPlies) {
    throw const FormatException('Game-history ply is out of range.');
  }
}

void _requireUint16(int value, {required String field, bool allowZero = true}) {
  if (value < (allowZero ? 0 : 1) || value > 0xffff) {
    throw FormatException('Game-history $field is out of range.');
  }
}

void _requireUint32(int value, {required String field, bool allowZero = true}) {
  if (value < (allowZero ? 0 : 1) || value > 0xffffffff) {
    throw FormatException('Game-history $field is out of range.');
  }
}

void _validateJournalId(String value, {bool allowZero = false}) {
  if (!RegExp(r'^[0-9a-f]{16}$').hasMatch(value) ||
      !allowZero && value == '0000000000000000') {
    throw const FormatException('Game-history journal identity is invalid.');
  }
}

Uint8List _encodeJournalId(String value) {
  final Uint8List bytes = Uint8List(8);
  for (int index = 0; index < 8; index += 1) {
    bytes[7 - index] = int.parse(
      value.substring(index * 2, index * 2 + 2),
      radix: 16,
    );
  }
  return bytes;
}

String _decodeJournalId(Uint8List littleEndianBytes) =>
    _hex(littleEndianBytes.reversed.toList(growable: false));
