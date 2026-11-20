part of 'lichess_prepared_session.dart';

extension on LichessPreparedSessionOwner {
  bool _streamCurrent(
    int generation,
    int streamEpoch,
    LichessBoardApiClient client,
    LichessPreparedRecord binding,
  ) =>
      _current(generation) &&
      _accountReady &&
      streamEpoch == _gameStreamEpoch &&
      identical(client, _client) &&
      _record?.identity == binding.identity &&
      _record?.accountId == binding.accountId &&
      _record?.providerGameId == binding.providerGameId;

  Future<void> _reconcileProvider(
    LichessGameStateEvent next,
    LichessProviderReceipt receipt,
    Duration monotonicReceipt,
    int generation,
    int streamEpoch,
    LichessBoardApiClient client,
    LichessPreparedRecord binding,
  ) async {
    receipt.validate();
    if (next.status.known == null) {
      if (!await _save(
        _record!._next(recoveryObservation: receipt),
        generation,
      )) {
        return;
      }
      if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
      _providerState = next;
      _providerStreamConnected = false;
      _gameRecovery = LichessPreparedRecovery.providerStatus;
      _gameStreamEpoch++;
      _recover(LichessPreparedRecovery.providerStatus);
      return;
    }

    LichessPendingLocalMove? pending = _record!.pendingLocalMove;
    if (pending != null) {
      validatePendingLocalMove(pending);
      if (pending.identity != binding.identity ||
          pending.providerGameId != binding.providerGameId) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveConflict,
        );
      }
      if (next.moves.length < pending.preStatePly) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveRegression,
        );
      }
      if (next.moves.length >= pending.targetPly &&
          next.moves[pending.targetPly - 1] != pending.uci) {
        final bool appearsLater = next.moves
            .skip(pending.targetPly)
            .contains(pending.uci);
        throw _LichessMoveRecoveryException(
          appearsLater
              ? LichessPreparedRecovery.moveSkipped
              : LichessPreparedRecovery.moveConflict,
        );
      }
      if (next.moves.length == pending.preStatePly &&
          next.status.known!.isTerminal) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveTerminal,
        );
      }
    }

    final snapshot = await _library.load();
    if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
    if (pending != null) {
      final currentPending = _record!.pendingLocalMove;
      if (currentPending == null) {
        // A newer exact provider observation already confirmed this command.
        return;
      }
      if (currentPending.identity != pending.identity ||
          currentPending.providerGameId != pending.providerGameId ||
          currentPending.targetPly != pending.targetPly ||
          currentPending.uci != pending.uci ||
          currentPending.localCommandId != pending.localCommandId ||
          currentPending.preStatePly != pending.preStatePly ||
          currentPending.preStatePrefixSha256 != pending.preStatePrefixSha256) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveConflict,
        );
      }
      pending = currentPending;
    }
    // Do not silently reconcile from an older recovered archive or ignore a
    // malformed file whose identity cannot be established.
    if (snapshot.recoveredFiles.isNotEmpty ||
        snapshot.rejectedFiles.isNotEmpty) {
      throw const FormatException(
        'Library requires explicit storage recovery.',
      );
    }
    final libraryId = 'board-${binding.identity.boardGameId}';
    final matches = snapshot.games
        .where(
          (g) =>
              g.boardGameId == binding.identity.boardGameId ||
              g.libraryId == libraryId,
        )
        .toList();
    if (matches.length > 1) {
      throw const FormatException('Archive identity collision.');
    }
    final archive = matches.isEmpty ? null : matches.single;
    final checkpoint = _record!.confirmed;
    if (archive != null) {
      final provider = archive.provider;
      if (archive.libraryId != libraryId ||
          archive.boardGameId != binding.identity.boardGameId ||
          archive.lineRevision != binding.identity.lineRevision ||
          provider == null ||
          provider.provider != 'lichess' ||
          provider.gameId != binding.providerGameId ||
          provider.accountId != binding.accountId ||
          provider.localSide != binding.providerSide!.name) {
        throw const FormatException(
          'Uncorrelated archive cannot be overwritten.',
        );
      }
    }
    if (checkpoint != null &&
        (archive == null ||
            checkpoint.libraryId != libraryId ||
            checkpoint.prefixSha256 !=
                confirmedArchivePrefixDigest(
                  archive,
                  checkpoint.receipt.ply,
                ))) {
      throw const FormatException(
        'Durable checkpoint prefix is missing or inconsistent.',
      );
    }
    if (checkpoint != null &&
        LichessKnownGameStatus.fromWire(
              checkpoint.receipt.rawStatus,
            )?.isTerminal ==
            true &&
        (archive!.provider!.rawStatus != checkpoint.receipt.rawStatus ||
            archive.provider!.winner != checkpoint.receipt.winner)) {
      throw const FormatException(
        'Terminal checkpoint evidence does not match its archive.',
      );
    }
    final base =
        archive ??
        GameArchive(
          libraryId: libraryId,
          boardGameId: binding.identity.boardGameId,
          createdAt: receipt.receivedAt,
          updatedAt: receipt.receivedAt,
          status: GameArchiveStatus.active,
          captureQuality: GameCaptureQuality.complete,
          variant: GameVariant.classic,
          outcome: GameOutcome.ongoing,
          winner: GameWinner.none,
          lineRevision: binding.identity.lineRevision,
          positions: const [
            ArchivedPosition(
              ply: 0,
              fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
            ),
          ],
          transitions: const [],
        );
    // Archive-ahead after interrupted checkpoint remains evidence. A fresh full
    // must match/extend it; a shorter list never authorizes an implicit takeback.
    final replayed = replayStandardChessExtension(base, next.moves);
    if (archive?.status == GameArchiveStatus.completed &&
        (next.status.wireValue != archive!.provider!.rawStatus ||
            next.winner?.name != archive.provider!.winner)) {
      throw const FormatException('Terminal provider evidence cannot regress.');
    }
    final provider = ProviderGameProvenance(
      provider: 'lichess',
      gameId: binding.providerGameId!,
      accountId: binding.accountId,
      localSide: binding.providerSide!.name,
      rawStatus: next.status.wireValue,
      winner: next.winner?.name,
    );
    provider.validate();
    final outcome = switch (provider.category) {
      ProviderResultCategory.ongoing => GameOutcome.ongoing,
      ProviderResultCategory.checkmate => GameOutcome.checkmate,
      ProviderResultCategory.resignation => GameOutcome.resignation,
      ProviderResultCategory.draw => GameOutcome.draw,
      ProviderResultCategory.timeout => GameOutcome.timeout,
      ProviderResultCategory.aborted => GameOutcome.aborted,
      ProviderResultCategory.providerDefined => GameOutcome.providerDefined,
    };
    final winner = outcome == GameOutcome.draw
        ? GameWinner.draw
        : switch (provider.winner) {
            'white' => GameWinner.white,
            'black' => GameWinner.black,
            _ => GameWinner.none,
          };
    final candidate = GameArchive(
      libraryId: libraryId,
      boardGameId: base.boardGameId,
      createdAt: base.createdAt,
      updatedAt: receipt.receivedAt.isBefore(base.updatedAt)
          ? base.updatedAt
          : receipt.receivedAt,
      status: outcome == GameOutcome.ongoing
          ? GameArchiveStatus.active
          : GameArchiveStatus.completed,
      captureQuality: base.captureQuality,
      variant: base.variant,
      outcome: outcome,
      winner: winner,
      lineRevision: base.lineRevision,
      positions: replayed.positions,
      transitions: replayed.transitions,
      displayName: base.displayName,
      whitePlayer: base.whitePlayer,
      blackPlayer: base.blackPlayer,
      provenance: base.provenance,
      provider: provider,
    );
    if (pending != null && next.moves.length == pending.preStatePly) {
      final checkpoint = _record!.confirmed!;
      if (checkpoint.receipt.ply != pending.preStatePly ||
          checkpoint.prefixSha256 != pending.preStatePrefixSha256 ||
          confirmedArchivePrefixDigest(base, pending.preStatePly) !=
              pending.preStatePrefixSha256) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveConflict,
        );
      }
      if (_moveHttpInFlight == (generation, pending.localCommandId)) {
        _providerState = next;
        _providerReceiptMonotonic = monotonicReceipt;
        _providerStreamConnected = true;
        return;
      }
      if (!_storageCertain) {
        _gameRecovery = LichessPreparedRecovery.storage;
        _recover(LichessPreparedRecovery.storage);
        return;
      }
      if (pending.rejected || pending.recoveryHold != null) {
        final recovery =
            pending.recoveryHold ?? LichessPreparedRecovery.moveRejected;
        _providerState = next;
        _providerReceiptMonotonic = monotonicReceipt;
        _providerStreamConnected = true;
        _gameRecovery = recovery;
        _recover(recovery);
        return;
      }
      final bool localTurn =
          pending.targetPly.isOdd ==
          (binding.providerSide == LichessPlayerColor.white);
      if (!pending.httpAcknowledged && !localTurn) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveConflict,
        );
      }
      if (!pending.httpAcknowledged && pending.retryUsed) {
        throw const _LichessMoveRecoveryException(
          LichessPreparedRecovery.moveAmbiguous,
        );
      }
      if (!pending.httpAcknowledged && !pending.retryUsed && localTurn) {
        final int retryEventRevision = _providerEventRevision;
        final retrying = pending.next(retryUsed: true);
        if (!await _save(
          _record!._next(pendingLocalMove: retrying),
          generation,
        )) {
          return;
        }
        _publish(_state.stage, _state.recovery);
        if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
        if (retryEventRevision != _providerEventRevision) {
          throw const _LichessMoveRecoveryException(
            LichessPreparedRecovery.moveConflict,
          );
        }
        final retry = _sendPendingMove(
          client: client,
          pending: retrying,
          generation: generation,
          isRetry: true,
          dispatchGuard: () =>
              _streamCurrent(generation, streamEpoch, client, binding) &&
              retryEventRevision == _providerEventRevision &&
              _record?.pendingLocalMove?.localCommandId ==
                  retrying.localCommandId &&
              _record?.pendingLocalMove?.retryUsed == true,
        );
        _moveTail = retry.then<void>((_) {});
        return;
      }
      _providerState = next;
      _providerReceiptMonotonic = monotonicReceipt;
      _providerStreamConnected = true;
      _gameRecovery = null;
      _publish(LichessPreparedSessionStage.boundProviderReconciled);
      return;
    }
    if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
    _storageCertain = false;
    await _library.saveProviderConfirmation(candidate, expected: archive);
    if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
    final confirmed = LichessConfirmedCheckpoint(
      libraryId: libraryId,
      prefixSha256: confirmedArchivePrefixDigest(
        candidate,
        candidate.currentPly,
      ),
      receipt: receipt,
    );
    final LichessConfirmedLocalMoveReceipt? commandReceipt = pending == null
        ? null
        : LichessConfirmedLocalMoveReceipt(
            identity: pending.identity,
            providerGameId: pending.providerGameId,
            targetPly: pending.targetPly,
            uci: pending.uci,
            localCommandId: pending.localCommandId,
          );
    if (!await _save(
      _record!._next(
        confirmed: confirmed,
        clearRecovery: true,
        clearPendingLocalMove: pending != null,
        confirmedLocalMove: commandReceipt,
      ),
      generation,
    )) {
      _gameStreamEpoch++;
      _providerStreamConnected = false;
      return;
    }
    if (!_streamCurrent(generation, streamEpoch, client, binding)) return;
    _providerState = next;
    _providerReceiptMonotonic = monotonicReceipt;
    _providerStreamConnected = true;
    _gameRecovery = null;
    _publish(LichessPreparedSessionStage.boundProviderReconciled);
  }
}
