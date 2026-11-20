import 'dart:async';
import 'dart:typed_data';

import 'game_history_protocol.dart';
import 'game_history_segment_importer.dart';
import 'game_history_sync_state_store.dart';

abstract interface class GameHistoryTransport {
  Stream<Uint8List> get notifications;

  Future<void> subscribe();

  Future<void> write(Uint8List value);

  Future<void> unsubscribe();
}

/// One connection-scoped owner must be shared by pull and restore clients.
/// A second operation fails before subscribing or writing to the transport.
class GameHistoryTransactionOwner {
  bool _active = false;

  bool get isActive => _active;

  Future<T> runExclusive<T>(Future<T> Function() operation) async {
    if (_active) {
      throw StateError('Another game-history transaction owns the connection.');
    }
    _active = true;
    try {
      return await operation();
    } finally {
      _active = false;
    }
  }
}

class GameHistorySyncResult {
  const GameHistorySyncResult({
    required this.journalId,
    required this.durableAck,
    required this.segmentCount,
    required this.encodedBytes,
    required this.lostGameCount,
    required this.gapObserved,
    required this.cursorRecovered,
  });

  final String journalId;
  final int durableAck;
  final int segmentCount;
  final int encodedBytes;
  final int lostGameCount;
  final bool gapObserved;
  final bool cursorRecovered;
}

class GameHistoryRejectedException implements Exception {
  const GameHistoryRejectedException(this.rejection);

  final GameHistoryRejectPayload rejection;

  @override
  String toString() =>
      'Game history rejected: ${rejection.reason.name} '
      '(opcode ${rejection.rejectedOpcode}, detail ${rejection.detail}).';
}

/// Pulls board-owned archive segments without taking connection ownership.
/// The caller supplies the existing owner-ready authorization fact and must
/// cancel this client on connection epoch, background, OTA, or teardown.
class GameHistoryPullClient {
  GameHistoryPullClient({
    required this.boardIdentity,
    required this.transport,
    required this.stateStore,
    required this.processor,
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
  final GameHistorySyncStateStore stateStore;
  final GameHistoryDurableSegmentProcessor processor;
  final FutureOr<bool> Function() isOwnerReady;
  final int Function() nextTransactionId;
  final DateTime Function() clock;
  final Duration Function() elapsed;
  final Duration inactivityTimeout;
  final GameHistoryTransactionOwner _transactionOwner;

  StreamIterator<Uint8List>? _iterator;
  bool _running = false;
  bool _cancelled = false;

  Future<GameHistorySyncResult> pull() =>
      _transactionOwner.runExclusive(_pullWithConnectionOwnership);

  Future<GameHistorySyncResult> _pullWithConnectionOwnership() async {
    if (_running) {
      throw StateError('A game-history pull is already active.');
    }
    _running = true;
    _cancelled = false;
    StreamIterator<Uint8List>? iterator;
    bool subscriptionAttempted = false;
    try {
      await _requireOwnerReady();
      final GameHistorySyncStateRead prior = await stateStore.load(
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
      int lostGameCount = 0;
      bool gapObserved = false;
      GameHistorySyncCursor? currentCursor = prior.cursor;
      int? previousTransactionId;

      while (true) {
        final int transactionId = nextTransactionId();
        if (transactionId <= 0 ||
            transactionId > 0xffff ||
            transactionId == previousTransactionId) {
          throw const FormatException(
            'Game-history transaction identity is invalid or reused.',
          );
        }
        previousTransactionId = transactionId;
        int nextFrameSequence = 1;
        final Future<bool> firstNotification = iterator.moveNext();
        firstNotification.ignore();
        await transport.write(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.begin.wireValue,
            transactionId: transactionId,
            frameSequence: 0,
            payload: GameHistoryBeginPayload(
              journalId: currentCursor?.journalId ?? '0000000000000000',
              appDurableAck: currentCursor?.durableAck ?? 0,
            ).encode(),
          ).encode(),
        );

        final GameHistoryFrame stateFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence,
          expectedOpcode: GameHistoryOpcode.state,
          allowedFlags: gameHistoryStateKnownFlags,
          pendingMove: firstNotification,
        );
        nextFrameSequence += 1;
        final GameHistoryStatePayload state = GameHistoryStatePayload.decode(
          stateFrame.payload,
        );
        final GameHistoryFrame boundsFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence,
          expectedOpcode: GameHistoryOpcode.stateBounds,
        );
        nextFrameSequence += 1;
        final GameHistoryStateBoundsPayload bounds =
            GameHistoryStateBoundsPayload.decode(boundsFrame.payload);
        final bool journalMatched =
            (stateFrame.flags & gameHistoryStateFlagJournalMatched) != 0;
        final bool cursorReconciled =
            (stateFrame.flags & gameHistoryStateFlagCursorReconciled) != 0;
        final bool segmentFollows =
            (stateFrame.flags & gameHistoryStateFlagSegmentFollows) != 0;
        final bool noRecords =
            (stateFrame.flags & gameHistoryStateFlagNoRecords) != 0;
        final bool expectedJournalMatch =
            currentCursor != null && currentCursor.journalId == state.journalId;
        if (journalMatched != expectedJournalMatch ||
            !cursorReconciled ||
            segmentFollows == noRecords ||
            state.oldestRetained > bounds.newestAllocated) {
          throw const FormatException(
            'Board history state flags and bounds are inconsistent.',
          );
        }

        final int durableAck = expectedJournalMatch
            ? currentCursor.durableAck
            : 0;
        if (expectedJournalMatch && bounds.boardDurableAck < durableAck ||
            segmentFollows && state.oldestRetained == 0 ||
            noRecords && state.oldestRetained > durableAck) {
          throw const FormatException(
            'Board history state contradicts its reconciled cursor.',
          );
        }
        if (durableAck > bounds.newestAllocated) {
          throw const FormatException(
            'Local history cursor exceeds the board journal.',
          );
        }
        final bool transactionGap = noRecords
            ? durableAck < bounds.newestAllocated
            : durableAck + 1 < state.oldestRetained;
        gapObserved = gapObserved || transactionGap;
        lostGameCount = bounds.lostGameCount;

        if (noRecords) {
          return GameHistorySyncResult(
            journalId: state.journalId,
            durableAck: durableAck,
            segmentCount: segmentCount,
            encodedBytes: encodedBytes,
            lostGameCount: lostGameCount,
            gapObserved: gapObserved,
            cursorRecovered: prior.recovered,
          );
        }

        final GameHistoryFrame descriptorFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence,
          expectedOpcode: GameHistoryOpcode.segmentBegin,
        );
        nextFrameSequence += 1;
        final GameHistorySegmentDescriptor descriptor =
            GameHistorySegmentDescriptor.decode(descriptorFrame.payload);
        final bool acknowledgedOverlap =
            descriptor.flags == gameHistorySegmentFlagAcknowledgedOverlap;
        final int expectedOutboxSequence = acknowledgedOverlap
            ? durableAck
            : transactionGap
            ? state.oldestRetained
            : durableAck + 1;
        if (acknowledgedOverlap && (!expectedJournalMatch || transactionGap) ||
            descriptor.firstOutboxSequence != expectedOutboxSequence ||
            descriptor.lastOutboxSequence > bounds.newestAllocated) {
          throw const FormatException(
            'Board segment does not continue the advertised journal.',
          );
        }

        final BytesBuilder received = BytesBuilder(copy: false);
        int remaining = descriptor.encodedBytes;
        while (remaining > 0) {
          final GameHistoryFrame dataFrame = await _nextExpected(
            iterator,
            transactionId: transactionId,
            expectedFrameSequence: nextFrameSequence,
            expectedOpcode: GameHistoryOpcode.data,
          );
          nextFrameSequence += 1;
          if (dataFrame.payload.isEmpty ||
              dataFrame.payload.length > remaining) {
            throw const FormatException(
              'Game-history data frame exceeds its segment bounds.',
            );
          }
          received.add(dataFrame.payload);
          remaining -= dataFrame.payload.length;
        }

        await _requireOwnerReady();
        final GameHistoryDurableSegmentResult durable = await processor.process(
          boardIdentity: boardIdentity,
          journalId: state.journalId,
          descriptor: descriptor,
          bytes: received.takeBytes(),
          retainedStartAfterGap: transactionGap ? state.oldestRetained : null,
          observedAt: clock().toUtc(),
        );
        await _requireOwnerReady();
        await transport.write(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.commit.wireValue,
            transactionId: transactionId,
            frameSequence: nextFrameSequence,
            payload: durable.commit.encode(),
          ).encode(),
        );
        nextFrameSequence += 1;

        final GameHistoryFrame committedFrame = await _nextExpected(
          iterator,
          transactionId: transactionId,
          expectedFrameSequence: nextFrameSequence,
          expectedOpcode: GameHistoryOpcode.committed,
        );
        final GameHistoryCommitPayload committed =
            GameHistoryCommitPayload.decode(committedFrame.payload);
        if (committed.firstOutboxSequence !=
                durable.commit.firstOutboxSequence ||
            committed.lastOutboxSequence != durable.commit.lastOutboxSequence ||
            committed.crc32 != durable.commit.crc32) {
          throw const FormatException(
            'Board committed a different history segment.',
          );
        }
        currentCursor = GameHistorySyncCursor(
          journalId: state.journalId,
          durableAck: committed.lastOutboxSequence,
        );
        segmentCount += 1;
        encodedBytes += descriptor.encodedBytes;
        if (currentCursor.durableAck >= bounds.newestAllocated) {
          return GameHistorySyncResult(
            journalId: state.journalId,
            durableAck: currentCursor.durableAck,
            segmentCount: segmentCount,
            encodedBytes: encodedBytes,
            lostGameCount: lostGameCount,
            gapObserved: gapObserved,
            cursorRecovered: prior.recovered,
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

  Future<void> cancel() async {
    _cancelled = true;
    await _iterator?.cancel();
  }

  Future<GameHistoryFrame> _nextExpected(
    StreamIterator<Uint8List> iterator, {
    required int transactionId,
    required int expectedFrameSequence,
    required GameHistoryOpcode expectedOpcode,
    int allowedFlags = 0,
    Future<bool>? pendingMove,
  }) async {
    final Duration deadline = elapsed() + inactivityTimeout;
    while (true) {
      final Duration remaining = deadline - elapsed();
      if (remaining <= Duration.zero) {
        throw TimeoutException(
          'Game-history transfer made no progress.',
          inactivityTimeout,
        );
      }
      if (_cancelled) {
        throw StateError('Game-history pull was cancelled.');
      }
      final Future<bool> move = pendingMove ?? iterator.moveNext();
      pendingMove = null;
      final bool available = await move.timeout(
        remaining,
        onTimeout: () => throw TimeoutException(
          'Game-history transfer was inactive.',
          inactivityTimeout,
        ),
      );
      if (!available || _cancelled) {
        throw StateError('Game-history transport closed.');
      }
      final GameHistoryFrame frame = GameHistoryFrame.decode(iterator.current);
      final int receivedAllowedFlags =
          frame.frameSequence < expectedFrameSequence &&
              frame.knownOpcode == GameHistoryOpcode.state
          ? gameHistoryStateKnownFlags
          : allowedFlags;
      if (frame.transactionId != transactionId ||
          (frame.flags & ~receivedAllowedFlags) != 0) {
        throw const FormatException(
          'Game-history frame belongs to another transaction.',
        );
      }
      if (frame.frameSequence < expectedFrameSequence) {
        continue;
      }
      if (frame.frameSequence > expectedFrameSequence) {
        throw const FormatException('Game-history frame sequence has a gap.');
      }
      if (frame.knownOpcode == GameHistoryOpcode.reject) {
        throw GameHistoryRejectedException(
          GameHistoryRejectPayload.decode(frame.payload),
        );
      }
      if (frame.knownOpcode != expectedOpcode) {
        throw FormatException(
          'Expected ${expectedOpcode.name}, received opcode ${frame.opcode}.',
        );
      }
      return frame;
    }
  }

  Future<void> _requireOwnerReady() async {
    if (_cancelled) throw StateError('Game-history pull was cancelled.');
    final bool ready = await isOwnerReady();
    if (_cancelled) throw StateError('Game-history pull was cancelled.');
    if (!ready) {
      throw StateError(
        'Game-history sync requires the paired owner-ready connection.',
      );
    }
  }
}

Duration Function() _newElapsedClock() {
  final Stopwatch stopwatch = Stopwatch()..start();
  return () => stopwatch.elapsed;
}
