import 'dart:async';
import 'dart:typed_data';

import '../models/game_archive.dart';
import 'current_game_receive_store.dart';
import 'current_game_sync_protocol.dart';
import 'game_history_protocol.dart';
import 'game_history_pull_client.dart';

class CurrentGameSyncResult {
  const CurrentGameSyncResult({
    required this.linePresent,
    required this.segmentCount,
    required this.encodedBytes,
    required this.archiveAcknowledged,
    this.cursor,
    this.archive,
  });

  final bool linePresent;
  final int segmentCount;
  final int encodedBytes;
  final bool archiveAcknowledged;
  final CurrentGameReceiveCursor? cursor;

  /// Null for an absent line or a validated line the user durably deleted.
  final GameArchive? archive;
}

/// Pulls the board's pinned current-line snapshot into durable phone storage.
///
/// This client deliberately has no BLE discovery or capability wiring. The
/// connection owner must supply an already-authorized transport and cancel the
/// client when that ownership epoch ends.
class CurrentGamePullClient {
  CurrentGamePullClient({
    required this.boardIdentity,
    required this.transport,
    required this.receiveStore,
    required this.isOwnerReady,
    required this.nextTransactionId,
    GameHistoryTransactionOwner? transactionOwner,
    DateTime Function()? clock,
    Duration Function()? elapsed,
    this.inactivityTimeout = const Duration(seconds: 15),
  }) : clock = clock ?? DateTime.now,
       elapsed = elapsed ?? _newElapsedClock(),
       _transactionOwner = transactionOwner ?? GameHistoryTransactionOwner();

  final String boardIdentity;
  final GameHistoryTransport transport;
  final CurrentGameReceiveStore receiveStore;
  final FutureOr<bool> Function() isOwnerReady;
  final int Function() nextTransactionId;
  final DateTime Function() clock;
  final Duration Function() elapsed;
  final Duration inactivityTimeout;
  final GameHistoryTransactionOwner _transactionOwner;

  StreamIterator<Uint8List>? _iterator;
  bool _running = false;
  bool _cancelled = false;
  int? _lastTransactionId;

  Future<CurrentGameSyncResult> pull() =>
      _transactionOwner.runExclusive(_pullWithConnectionOwnership);

  Future<CurrentGameSyncResult> _pullWithConnectionOwnership() async {
    if (_running) throw StateError('A current-game pull is already active.');
    _running = true;
    _cancelled = false;
    StreamIterator<Uint8List>? iterator;
    bool subscriptionAttempted = false;
    try {
      await _requireOwnerReady();
      CurrentGameReceiveCursor? cursor = await receiveStore.loadCursor(
        boardIdentity,
      );
      await _requireOwnerReady();
      iterator = StreamIterator<Uint8List>(transport.notifications);
      _iterator = iterator;
      subscriptionAttempted = true;
      await transport.subscribe();
      await _requireOwnerReady();

      int segmentCount = 0;
      int encodedBytes = 0;
      _PinnedSnapshot? pinned;
      while (true) {
        final int transactionId = nextTransactionId();
        if (transactionId <= 0 ||
            transactionId > 0xffff ||
            transactionId == _lastTransactionId) {
          throw const FormatException(
            'Current-game transaction identity is invalid or reused.',
          );
        }
        _lastTransactionId = transactionId;
        int nextFrameSequence = 1;
        final Future<bool> firstNotification = iterator.moveNext();
        firstNotification.ignore();
        final GameHistoryFrame begin = GameHistoryFrame(
          opcode: CurrentGameSyncOpcode.begin.wireValue,
          transactionId: transactionId,
          frameSequence: 0,
          payload: (cursor?.toWire() ?? const CurrentGameResumeCursor.empty())
              .encode(),
        );
        // Admission and lost-reply repair belong to the connection transport.
        // A slow semantic reply must not create another Begin operation.
        await transport.write(begin.encode());
        await _requireOwnerReady();
        final GameHistoryFrame stateFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence++,
          expectedOpcode: CurrentGameSyncOpcode.state,
          pendingMove: firstNotification,
        );
        await _requireOwnerReady();
        final CurrentGameDigest digest = CurrentGameDigest.decode(
          stateFrame.payload,
          allowAbsent: true,
        );
        final GameHistoryFrame boundsFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence++,
          expectedOpcode: CurrentGameSyncOpcode.bounds,
        );
        await _requireOwnerReady();
        final CurrentGameBounds bounds = CurrentGameBounds.decode(
          boundsFrame.payload,
        );
        final bool digestAbsent = digest.fingerprint.every(
          (int byte) => byte == 0,
        );
        if (digestAbsent == bounds.present) {
          throw const FormatException(
            'Current-game State and Bounds disagree about line presence.',
          );
        }
        final _PinnedSnapshot received = _PinnedSnapshot(digest, bounds);
        if (pinned != null && !pinned.matches(received)) {
          throw const FormatException(
            'Current-game snapshot changed during the pull.',
          );
        }
        pinned ??= received;

        if (!bounds.present) {
          return CurrentGameSyncResult(
            linePresent: false,
            segmentCount: segmentCount,
            encodedBytes: encodedBytes,
            archiveAcknowledged: false,
            cursor: cursor,
          );
        }

        CurrentGameAcceptedSegment? recovered;
        try {
          recovered = await receiveStore.reconcileComplete(
            boardIdentity: boardIdentity,
            advertisedDigest: digest,
            bounds: bounds,
            observedAt: clock().toUtc(),
          );
        } on FormatException {
          // A once-complete local cursor can describe an older line after Undo
          // or replacement. The board's record-1 segment is authoritative and
          // lets the receive store replace and fence those durable bytes.
          recovered = null;
        }
        await _requireOwnerReady();
        if (recovered != null) {
          return await _finishComplete(
            iterator: iterator,
            transactionId: transactionId,
            nextFrameSequence: nextFrameSequence,
            digest: digest,
            accepted: recovered,
            segmentCount: segmentCount,
            encodedBytes: encodedBytes,
          );
        }
        final GameHistoryFrame descriptorFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence++,
          expectedOpcode: CurrentGameSyncOpcode.segmentBegin,
        );
        await _requireOwnerReady();
        final GameHistorySegmentDescriptor descriptor =
            GameHistorySegmentDescriptor.decode(descriptorFrame.payload);
        final int expectedSequence = cursor?.nextRecordSequence ?? 1;
        if (descriptor.flags != 0 ||
            (descriptor.firstOutboxSequence != expectedSequence &&
                descriptor.firstOutboxSequence != 1) ||
            descriptor.lastOutboxSequence > bounds.recordCount) {
          throw const FormatException(
            'Current-game segment does not continue the advertised line.',
          );
        }

        final BytesBuilder receivedBytes = BytesBuilder(copy: false);
        int remaining = descriptor.encodedBytes;
        while (remaining > 0) {
          final GameHistoryFrame dataFrame = await _nextExpected(
            iterator,
            transactionId: transactionId,
            expectedFrameSequence: nextFrameSequence++,
            expectedOpcode: CurrentGameSyncOpcode.data,
          );
          await _requireOwnerReady();
          if (dataFrame.payload.isEmpty ||
              dataFrame.payload.length > remaining) {
            throw const FormatException(
              'Current-game DATA exceeds its segment bounds.',
            );
          }
          receivedBytes.add(dataFrame.payload);
          remaining -= dataFrame.payload.length;
        }

        final CurrentGameAcceptedSegment accepted = await receiveStore
            .acceptSegment(
              boardIdentity: boardIdentity,
              advertisedDigest: digest,
              bounds: bounds,
              descriptor: descriptor,
              bytes: receivedBytes.takeBytes(),
              observedAt: clock().toUtc(),
              restartFromBeginning:
                  descriptor.firstOutboxSequence == 1 && expectedSequence != 1,
            );
        await _requireOwnerReady();
        final GameHistoryCommitPayload commit = GameHistoryCommitPayload(
          firstOutboxSequence: descriptor.firstOutboxSequence,
          lastOutboxSequence: descriptor.lastOutboxSequence,
          crc32: descriptor.crc32,
        );
        await transport.write(
          GameHistoryFrame(
            opcode: CurrentGameSyncOpcode.segmentCommit.wireValue,
            transactionId: transactionId,
            frameSequence: nextFrameSequence++,
            payload: commit.encode(),
          ).encode(),
        );
        await _requireOwnerReady();
        final GameHistoryFrame committedFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence++,
          expectedOpcode: CurrentGameSyncOpcode.segmentCommitted,
        );
        await _requireOwnerReady();
        final GameHistoryCommitPayload committed =
            GameHistoryCommitPayload.decode(committedFrame.payload);
        if (!_sameCommit(commit, committed)) {
          throw const FormatException(
            'Board committed a different current-game segment.',
          );
        }
        cursor = accepted.cursor;
        segmentCount += 1;
        encodedBytes += descriptor.encodedBytes;
        if (accepted.complete) {
          return await _finishComplete(
            iterator: iterator,
            transactionId: transactionId,
            nextFrameSequence: nextFrameSequence,
            digest: digest,
            accepted: accepted,
            segmentCount: segmentCount,
            encodedBytes: encodedBytes,
          );
        }
        await _requireOwnerReady();
      }
    } finally {
      _iterator = null;
      try {
        await iterator?.cancel();
      } finally {
        try {
          if (subscriptionAttempted) await transport.unsubscribe();
        } finally {
          _running = false;
        }
      }
    }
  }

  Future<CurrentGameSyncResult> _finishComplete({
    required StreamIterator<Uint8List> iterator,
    required int transactionId,
    required int nextFrameSequence,
    required CurrentGameDigest digest,
    required CurrentGameAcceptedSegment accepted,
    required int segmentCount,
    required int encodedBytes,
  }) async {
    final Uint8List acknowledgement =
        accepted.archiveAcknowledgement?.encode(allowAbsent: false) ??
        (throw const FormatException(
          'Complete current-game archive lacks a durable acknowledgement.',
        ));
    if (!_sameBytes(acknowledgement, digest.fingerprint)) {
      throw const FormatException(
        'Durable archive acknowledgement differs from CurrentState.',
      );
    }
    await transport.write(
      GameHistoryFrame(
        opcode: CurrentGameSyncOpcode.archiveAck.wireValue,
        transactionId: transactionId,
        frameSequence: nextFrameSequence++,
        payload: acknowledgement,
      ).encode(),
    );
    await _requireOwnerReady();
    final GameHistoryFrame archivedFrame = await _nextExpected(
      iterator,
      transactionId: transactionId,
      expectedFrameSequence: nextFrameSequence,
      expectedOpcode: CurrentGameSyncOpcode.archived,
    );
    await _requireOwnerReady();
    final CurrentGameDigest archived = CurrentGameDigest.decode(
      archivedFrame.payload,
      allowAbsent: false,
    );
    if (!_sameBytes(archived.fingerprint, digest.fingerprint)) {
      throw const FormatException(
        'Board archived a different current-game line.',
      );
    }
    return CurrentGameSyncResult(
      linePresent: true,
      segmentCount: segmentCount,
      encodedBytes: encodedBytes,
      archiveAcknowledged: true,
      cursor: accepted.cursor,
      archive: accepted.archive,
    );
  }

  Future<void> cancel() async {
    _cancelled = true;
    await _iterator?.cancel();
  }

  Future<GameHistoryFrame> _nextExpected(
    StreamIterator<Uint8List> iterator, {
    required int transactionId,
    required int expectedFrameSequence,
    required CurrentGameSyncOpcode expectedOpcode,
    Future<bool>? pendingMove,
  }) async {
    final Duration limit = inactivityTimeout;
    final Duration deadline = elapsed() + limit;
    while (true) {
      final Duration remaining = deadline - elapsed();
      if (remaining <= Duration.zero) {
        throw TimeoutException(
          'Current-game transfer made no progress.',
          limit,
        );
      }
      _throwIfCancelled();
      final Future<bool> move = pendingMove ?? iterator.moveNext();
      pendingMove = null;
      final bool available = await move.timeout(
        remaining,
        onTimeout: () => throw TimeoutException(
          'Current-game transfer was inactive.',
          limit,
        ),
      );
      _throwIfCancelled();
      if (!available) throw StateError('Current-game transport closed.');
      final GameHistoryFrame frame = GameHistoryFrame.decode(iterator.current);
      if (frame.transactionId != transactionId || frame.flags != 0) {
        throw const FormatException(
          'Current-game frame belongs to another transaction.',
        );
      }
      if (frame.frameSequence < expectedFrameSequence) continue;
      if (frame.frameSequence > expectedFrameSequence) {
        throw const FormatException('Current-game frame sequence has a gap.');
      }
      if (frame.opcode != expectedOpcode.wireValue) {
        throw FormatException(
          'Expected ${expectedOpcode.name}, received opcode ${frame.opcode}.',
        );
      }
      return frame;
    }
  }

  Future<void> _requireOwnerReady() async {
    _throwIfCancelled();
    final bool ready = await isOwnerReady();
    _throwIfCancelled();
    if (!ready) {
      throw StateError(
        'Current-game sync requires the paired owner-ready connection.',
      );
    }
  }

  void _throwIfCancelled() {
    if (_cancelled) throw StateError('Current-game pull was cancelled.');
  }
}

class _PinnedSnapshot {
  const _PinnedSnapshot(this.digest, this.bounds);

  final CurrentGameDigest digest;
  final CurrentGameBounds bounds;

  bool matches(_PinnedSnapshot other) =>
      _sameBytes(digest.fingerprint, other.digest.fingerprint) &&
      bounds.lineRevision == other.bounds.lineRevision &&
      bounds.recordCount == other.bounds.recordCount &&
      bounds.flags == other.bounds.flags;
}

bool _sameCommit(
  GameHistoryCommitPayload left,
  GameHistoryCommitPayload right,
) =>
    left.firstOutboxSequence == right.firstOutboxSequence &&
    left.lastOutboxSequence == right.lastOutboxSequence &&
    left.crc32 == right.crc32;

bool _sameBytes(List<int> left, List<int> right) {
  if (left.length != right.length) return false;
  for (int index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

Duration Function() _newElapsedClock() {
  final Stopwatch stopwatch = Stopwatch()..start();
  return () => stopwatch.elapsed;
}
