import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import '../models/game_archive.dart';
import '../models/local_player_profile.dart';
import 'game_history_protocol.dart';
import 'game_history_sync_state_store.dart';
import 'game_library_repository.dart';

class GameHistorySegmentImporter {
  const GameHistorySegmentImporter(this.repository, {this.loadPlayers});

  final GameLibraryRepository repository;
  final Future<LocalPlayerDirectory> Function()? loadPlayers;

  Future<GameArchive?> importRecords(
    List<GameHistoryRecord> records, {
    required DateTime observedAt,
    bool firstRecordIsAcknowledgedOverlap = false,
  }) async {
    if (records.isEmpty) {
      throw const FormatException('Game-history segment contains no records.');
    }
    for (final GameHistoryRecord record in records) {
      GameHistoryCodec.encodeRecord(record);
    }
    final String boardGameId = records.first.boardGameId;
    int priorSequence = records.first.outboxSequence - 1;
    int? priorRevision;
    for (final GameHistoryRecord record in records) {
      if (record.boardGameId != boardGameId) {
        throw const FormatException(
          'Game-history segment crosses game identities.',
        );
      }
      if (record.outboxSequence != priorSequence + 1) {
        throw const FormatException(
          'Game-history records are duplicated or reordered.',
        );
      }
      if (priorRevision != null && record.lineRevision < priorRevision) {
        throw const FormatException(
          'Game-history line revisions cannot regress within a segment.',
        );
      }
      priorSequence = record.outboxSequence;
      priorRevision = record.lineRevision;
    }

    final GameArchive? existing = await repository.findByBoardGameId(
      boardGameId,
    );
    final List<ArchivedPosition> positions = <ArchivedPosition>[];
    final List<ArchivedTransition> transitions = <ArchivedTransition>[];
    late GameVariant variant;
    GameArchiveStatus status = GameArchiveStatus.active;
    GameOutcome outcome = GameOutcome.ongoing;
    GameWinner winner = GameWinner.none;

    final GameHistoryRecord first = records.first;
    if (firstRecordIsAcknowledgedOverlap &&
        (records.length < 2 || first is GameHistorySealRecord)) {
      throw const FormatException(
        'An overlap segment needs a position and unacknowledged record.',
      );
    }
    if (firstRecordIsAcknowledgedOverlap) {
      final ArchivedPosition overlap = switch (first) {
        GameHistoryLineBaseRecord record => record.position,
        GameHistoryCommittedPlyRecord record => record.position,
        GameHistorySealRecord() => throw const FormatException(
          'A seal cannot be an acknowledged position overlap.',
        ),
      };
      final ArchivedPosition? durable = existing == null
          ? null
          : _positionAt(existing, overlap.ply);
      if (durable == null ||
          durable.fen != overlap.fen ||
          durable.duckSquare != overlap.duckSquare ||
          first is GameHistoryLineBaseRecord &&
              first.variant != existing!.variant) {
        throw const FormatException(
          'Acknowledged overlap differs from durable local history.',
        );
      }
    }
    if (first case final GameHistoryLineBaseRecord lineBase) {
      positions.add(lineBase.position);
      variant = lineBase.variant;
    } else {
      if (existing == null) {
        throw const FormatException(
          'A continuation segment has no durable local base.',
        );
      }
      variant = existing.variant;
      if (first case final GameHistoryCommittedPlyRecord committed) {
        final ArchivedPosition? base = firstRecordIsAcknowledgedOverlap
            ? _positionAt(existing, committed.position.ply)
            : _positionAt(existing, committed.position.ply - 1);
        if (base == null) {
          throw const FormatException(
            'A continuation segment does not overlap durable history.',
          );
        }
        positions.add(
          firstRecordIsAcknowledgedOverlap ? committed.position : base,
        );
      } else {
        positions.addAll(existing.positions);
        transitions.addAll(existing.transitions);
      }
    }

    bool sealed = false;
    for (int index = 0; index < records.length; index += 1) {
      final GameHistoryRecord record = records[index];
      if (index == 0 && firstRecordIsAcknowledgedOverlap) {
        continue;
      }
      switch (record) {
        case GameHistoryLineBaseRecord():
          if (index != 0) {
            throw const FormatException(
              'Line-base record must begin its segment.',
            );
          }
        case GameHistoryCommittedPlyRecord committed:
          if (sealed ||
              committed.position.ply != positions.last.ply + 1 ||
              (variant == GameVariant.classic) !=
                  (committed.position.duckSquare == null)) {
            throw const FormatException(
              'Committed-ply record does not continue its line.',
            );
          }
          positions.add(committed.position);
          transitions.add(committed.transition);
        case GameHistorySealRecord seal:
          if (sealed || index != records.length - 1) {
            throw const FormatException(
              'Archive seal must be the final segment record.',
            );
          }
          sealed = true;
          status = seal.status;
          outcome = seal.outcome;
          winner = seal.winner;
      }
    }

    final AuthoritativeLineSegment segment = AuthoritativeLineSegment(
      boardGameId: boardGameId,
      lineRevision: records.last.lineRevision,
      status: status,
      variant: variant,
      outcome: outcome,
      winner: winner,
      positions: positions,
      transitions: transitions,
    );
    final players = await loadPlayers?.call();
    ArchivedPlayerAttribution attribution(LocalPlayerChessSide side) {
      if (players == null) return const ArchivedPlayerAttribution.unknown();
      final profile = players.selectedPlayer(side);
      return profile == null
          ? const ArchivedPlayerAttribution.guest()
          : ArchivedPlayerAttribution.profile(profile.id);
    }

    return repository.mergeAuthoritativeSegment(
      segment,
      observedAt: observedAt,
      whitePlayer: attribution(LocalPlayerChessSide.white),
      blackPlayer: attribution(LocalPlayerChessSide.black),
    );
  }
}

class GameHistoryDurableSegmentResult {
  const GameHistoryDurableSegmentResult({
    required this.commit,
    required this.archive,
    required this.alreadyDurable,
  });

  final GameHistoryCommitPayload commit;
  final GameArchive? archive;
  final bool alreadyDurable;
}

/// Enforces the transport's critical ordering: validate, atomically save the
/// archive, persist the cumulative phone cursor, then permit an ACK write.
class GameHistoryDurableSegmentProcessor {
  const GameHistoryDurableSegmentProcessor({
    required this.importer,
    required this.stateStore,
  });

  final GameHistorySegmentImporter importer;
  final GameHistorySyncStateStore stateStore;

  Future<GameHistoryDurableSegmentResult> process({
    required String boardIdentity,
    required String journalId,
    required GameHistorySegmentDescriptor descriptor,
    required Uint8List bytes,
    required DateTime observedAt,
    int? retainedStartAfterGap,
  }) async {
    final List<GameHistoryRecord> records = GameHistoryCodec.decodeSegment(
      bytes,
      descriptor,
    );
    final GameHistoryCommitPayload commit = GameHistoryCommitPayload(
      firstOutboxSequence: descriptor.firstOutboxSequence,
      lastOutboxSequence: descriptor.lastOutboxSequence,
      crc32: descriptor.crc32,
    );
    final GameHistorySyncCursor candidateCursor = GameHistorySyncCursor(
      journalId: journalId,
      durableAck: descriptor.lastOutboxSequence,
      lastCommit: commit,
      lastSegmentSha256: sha256.convert(bytes).toString(),
    );
    candidateCursor.validate();

    final GameHistorySyncStateRead saved = await stateStore.load(boardIdentity);
    final GameHistorySyncCursor? cursor = saved.cursor;
    final bool acknowledgedOverlap =
        descriptor.flags == gameHistorySegmentFlagAcknowledgedOverlap;
    if (cursor != null && cursor.journalId == journalId) {
      if (descriptor.lastOutboxSequence <= cursor.durableAck) {
        final GameHistoryCommitPayload? receipt = cursor.lastCommit;
        if (receipt == null ||
            receipt.firstOutboxSequence != commit.firstOutboxSequence ||
            receipt.lastOutboxSequence != commit.lastOutboxSequence ||
            receipt.crc32 != commit.crc32 ||
            cursor.lastSegmentSha256 != candidateCursor.lastSegmentSha256) {
          throw const FormatException(
            'History replay does not match the retained durable receipt.',
          );
        }
        return GameHistoryDurableSegmentResult(
          commit: commit,
          archive: null,
          alreadyDurable: true,
        );
      }
    } else if (acknowledgedOverlap) {
      throw const FormatException(
        'A new journal cannot begin with an acknowledged overlap.',
      );
    }

    final int priorAck = cursor?.journalId == journalId
        ? cursor!.durableAck
        : 0;
    final int expectedFirst = acknowledgedOverlap ? priorAck : priorAck + 1;
    if (retainedStartAfterGap != null) {
      if (acknowledgedOverlap ||
          retainedStartAfterGap <= expectedFirst ||
          descriptor.firstOutboxSequence != retainedStartAfterGap ||
          records.first is! GameHistoryLineBaseRecord) {
        throw const FormatException(
          'Retained history gap does not match its base.',
        );
      }
    } else if (descriptor.firstOutboxSequence != expectedFirst) {
      throw const FormatException(
        'Segment does not continue from the last history record saved by the app.',
      );
    }

    final GameArchive? archive = await importer.importRecords(
      records,
      observedAt: observedAt,
      firstRecordIsAcknowledgedOverlap: acknowledgedOverlap,
    );
    await stateStore.save(boardIdentity, candidateCursor);
    return GameHistoryDurableSegmentResult(
      commit: commit,
      archive: archive,
      alreadyDurable: false,
    );
  }
}

ArchivedPosition? _positionAt(GameArchive archive, int ply) {
  final int index = ply - archive.positions.first.ply;
  if (index < 0 || index >= archive.positions.length) {
    return null;
  }
  return archive.positions[index];
}
