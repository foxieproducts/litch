import 'dart:async';

import '../../models/game_archive.dart';
import '../../models/provider_game_provenance.dart';
import '../../models/standard_chess_replay.dart';
import '../../services/game_library_repository.dart';
import 'lichess_confirmed_checkpoint.dart';

import 'lichess_board_api_client.dart';
import 'lichess_board_api_events.dart';
import 'lichess_request_coordinator.dart';

part 'lichess_prepared_reconciliation.dart';

enum LichessPreparedSessionStage {
  idle,
  checking,
  preparing,
  matching,
  awaitingGameConfirmation,
  binding,
  boundAwaitingReconciliation,
  boundProviderReconciled,
  uncertain,
  retiring,
  suspended,
  disposed,
}

enum LichessPreparedRecovery {
  notReady,
  storage,
  gate,
  inventory,
  transport,
  accountMismatch,
  staleSelection,
  incompatibleGame,
  matchingStopped,
  keepAwake,
  providerIdentity,
  providerPosition,
  providerStatus,
  moveAmbiguous,
  moveRejected,
  moveConflict,
  moveRegression,
  moveSkipped,
  moveTerminal,
  moveRateLimited,
  moveAuthentication,
  movePermission,
}

enum LichessPreparedGateState { unknown, unarmed, pending, armed }

/// Verified by the external account/credential owner, never inferred from a token.
final class LichessAccountContext {
  const LichessAccountContext(this.accountId);
  final String accountId;
}

/// Supplied correlation values, not an implementation of shared board identity.
final class LichessPreparedIdentity {
  const LichessPreparedIdentity({
    required this.sessionId,
    required this.boardIdentity,
    required this.boardGameId,
    required this.lineRevision,
  });
  final String sessionId;
  final String boardIdentity;
  final String boardGameId;
  final int lineRevision;

  @override
  bool operator ==(Object other) =>
      other is LichessPreparedIdentity &&
      sessionId == other.sessionId &&
      boardIdentity == other.boardIdentity &&
      boardGameId == other.boardGameId &&
      lineRevision == other.lineRevision;
  @override
  int get hashCode =>
      Object.hash(sessionId, boardIdentity, boardGameId, lineRevision);
}

final class LichessPendingLocalMove {
  const LichessPendingLocalMove({
    required this.identity,
    required this.providerGameId,
    required this.targetPly,
    required this.uci,
    required this.localCommandId,
    required this.preStatePly,
    required this.preStatePrefixSha256,
    required this.retryUsed,
    required this.httpAcknowledged,
    required this.rejected,
    this.recoveryHold,
  });

  final LichessPreparedIdentity identity;
  final String providerGameId;
  final int targetPly;
  final String uci;
  final String localCommandId;
  final int preStatePly;
  final String preStatePrefixSha256;
  final bool retryUsed;
  final bool httpAcknowledged;
  final bool rejected;
  final LichessPreparedRecovery? recoveryHold;

  LichessPendingLocalMove next({
    bool? retryUsed,
    bool? httpAcknowledged,
    bool? rejected,
    LichessPreparedRecovery? recoveryHold,
  }) => LichessPendingLocalMove(
    identity: identity,
    providerGameId: providerGameId,
    targetPly: targetPly,
    uci: uci,
    localCommandId: localCommandId,
    preStatePly: preStatePly,
    preStatePrefixSha256: preStatePrefixSha256,
    retryUsed: retryUsed ?? this.retryUsed,
    httpAcknowledged: httpAcknowledged ?? this.httpAcknowledged,
    rejected: rejected ?? this.rejected,
    recoveryHold: recoveryHold ?? this.recoveryHold,
  );
}

final class LichessConfirmedLocalMoveReceipt {
  const LichessConfirmedLocalMoveReceipt({
    required this.identity,
    required this.providerGameId,
    required this.targetPly,
    required this.uci,
    required this.localCommandId,
  });

  final LichessPreparedIdentity identity;
  final String providerGameId;
  final int targetPly;
  final String uci;
  final String localCommandId;
}

void validatePendingLocalMove(LichessPendingLocalMove pending) {
  if (!RegExp(r'^[A-Za-z0-9]{1,64}$').hasMatch(pending.providerGameId) ||
      !RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$').hasMatch(pending.uci) ||
      !RegExp(r'^[A-Za-z0-9._:-]{1,64}$').hasMatch(pending.localCommandId) ||
      !RegExp(r'^[0-9a-f]{64}$').hasMatch(pending.preStatePrefixSha256) ||
      pending.preStatePly < 0 ||
      pending.preStatePly >= gameArchiveMaximumPlies ||
      pending.targetPly != pending.preStatePly + 1) {
    throw const FormatException('Invalid pending local move.');
  }
  if (pending.recoveryHold != null &&
      pending.recoveryHold != LichessPreparedRecovery.moveRateLimited &&
      pending.recoveryHold != LichessPreparedRecovery.moveAuthentication &&
      pending.recoveryHold != LichessPreparedRecovery.movePermission) {
    throw const FormatException('Invalid pending move recovery hold.');
  }
}

void validateConfirmedLocalMove(LichessConfirmedLocalMoveReceipt receipt) {
  validatePendingLocalMove(
    LichessPendingLocalMove(
      identity: receipt.identity,
      providerGameId: receipt.providerGameId,
      targetPly: receipt.targetPly,
      uci: receipt.uci,
      localCommandId: receipt.localCommandId,
      preStatePly: receipt.targetPly - 1,
      preStatePrefixSha256:
          '0000000000000000000000000000000000000000000000000000000000000000',
      retryUsed: false,
      httpAcknowledged: true,
      rejected: false,
    ),
  );
}

enum LichessLocalMoveSubmissionResult {
  pending,
  ambiguous,
  rejected,
  notReady,
  storageFailure,
}

final class _LichessMoveRecoveryException implements Exception {
  const _LichessMoveRecoveryException(this.recovery);
  final LichessPreparedRecovery recovery;
}

/// A single bounded safety record. It contains no credentials or move history.
final class LichessPreparedRecord {
  const LichessPreparedRecord({
    required this.identity,
    required this.accountId,
    required this.seek,
    required this.revision,
    required this.seekMayHaveStarted,
    this.providerGameId,
    this.providerSide,
    this.released = false,
    this.confirmed,
    this.recoveryObservation,
    this.pendingLocalMove,
    this.confirmedLocalMove,
  });
  final LichessPreparedIdentity identity;
  final String accountId;
  final LichessRealtimeSeek seek;
  final int revision;
  final bool seekMayHaveStarted;
  final String? providerGameId;
  final LichessPlayerColor? providerSide;
  final bool released;
  final LichessConfirmedCheckpoint? confirmed;
  final LichessProviderReceipt? recoveryObservation;
  final LichessPendingLocalMove? pendingLocalMove;
  final LichessConfirmedLocalMoveReceipt? confirmedLocalMove;

  LichessPreparedRecord _next({
    bool? seekMayHaveStarted,
    String? providerGameId,
    LichessPlayerColor? providerSide,
    bool? released,
    LichessConfirmedCheckpoint? confirmed,
    LichessProviderReceipt? recoveryObservation,
    bool clearRecovery = false,
    LichessPendingLocalMove? pendingLocalMove,
    bool clearPendingLocalMove = false,
    LichessConfirmedLocalMoveReceipt? confirmedLocalMove,
  }) => LichessPreparedRecord(
    identity: identity,
    accountId: accountId,
    seek: seek,
    revision: revision + 1,
    seekMayHaveStarted: seekMayHaveStarted ?? this.seekMayHaveStarted,
    providerGameId: providerGameId ?? this.providerGameId,
    providerSide: providerSide ?? this.providerSide,
    released: released ?? this.released,
    confirmed: confirmed ?? this.confirmed,
    recoveryObservation: clearRecovery
        ? null
        : recoveryObservation ?? this.recoveryObservation,
    pendingLocalMove: clearPendingLocalMove
        ? null
        : pendingLocalMove ?? this.pendingLocalMove,
    confirmedLocalMove: confirmedLocalMove ?? this.confirmedLocalMove,
  );
}

enum LichessPreparedLoadKind {
  absent,
  current,
  recovered,
  corrupt,
  unavailable,
}

final class LichessPreparedLoad {
  const LichessPreparedLoad(this.kind, {this.record});
  final LichessPreparedLoadKind kind;
  final LichessPreparedRecord? record;
}

abstract interface class LichessPreparedSessionStore {
  /// Recovered older data must not be reported as a current record.
  Future<LichessPreparedLoad> load();

  /// Completes only after durable acknowledgement; an error leaves uncertainty.
  Future<void> write(LichessPreparedRecord record);
}

abstract interface class LichessPreparedSessionGate {
  /// Current durable gate identity, null only for a confirmed unarmed board.
  /// Unavailable/readback failures throw rather than masquerading as unarmed.
  Future<LichessPreparedIdentity?> readback();

  /// Before granting true, verify the expected stable board is synchronized in the
  /// supported standard setup, then durably arm and read back this exact
  /// identity. An identity tuple alone is not readiness; a BLE ACK is not proof.
  /// Atomically reject a different armed identity; arm only an unarmed board
  /// or this same identity. An earlier readback is not a conflict check.
  Future<bool> armAndReadback(LichessPreparedIdentity identity);

  /// Verify the expected stable board and atomically disarm/read back this exact
  /// full identity. An already-unarmed board may confirm unchanged unarmed only
  /// if no prior pending arm can still commit. Reject another armed identity
  /// without mutation. An earlier null readback alone is not this proof.
  Future<bool> disarmPreSeek(LichessPreparedIdentity identity);
}

abstract interface class LichessPreparedKeepAwake {
  Future<bool> acquire();
  Future<void> release();
}

/// Every returned client must use the same root-owned request coordinator and
/// credentials fixed to this account for that client's lifetime.
typedef LichessBoardApiClientFactory =
    LichessBoardApiClient Function(LichessAccountContext account);

final class LichessPreparedSessionState {
  LichessPreparedSessionState({
    required this.stage,
    required this.evidenceRevision,
    required this.candidates,
    this.record,
    this.inventory,
    this.recovery,
    this.gate = LichessPreparedGateState.unknown,
    this.accountReady = false,
    this.storageCertain = true,
    this.gateIdentity,
    this.isRefreshingInventory = false,
    this.providerState,
    this.providerStreamConnected = false,
  });
  final LichessPreparedSessionStage stage;
  final int evidenceRevision;
  final List<LichessPlayingGame> candidates;
  final LichessPreparedRecord? record;
  final LichessPlayingInventory? inventory;
  final LichessPreparedRecovery? recovery;
  final LichessPreparedGateState gate;
  final bool accountReady;
  final bool storageCertain;
  final LichessPreparedIdentity? gateIdentity;
  final bool isRefreshingInventory;

  /// Last provider observation. The record's confirmed checkpoint identifies
  /// durable archive state; unknown recovery observations do not advance it.
  /// No physical acknowledgement or paused provider clock is implied.
  final LichessGameStateEvent? providerState;
  final bool providerStreamConnected;
  bool get inputEnabled => false;
}

/// Root-scoped, injection-only owner. No widget or BLE protocol,
/// move/action submission or production login is wired here. Provider replay
/// and durable confirmation use the injected shared archive repository.
final class LichessPreparedSessionOwner {
  LichessPreparedSessionOwner({
    required LichessBoardApiClientFactory clientFactory,
    required LichessPreparedSessionStore store,
    required LichessPreparedSessionGate gate,
    required LichessPreparedKeepAwake keepAwake,
    required GameLibraryRepository library,
  }) : _clientFactory = clientFactory,
       _store = store,
       _gate = gate,
       _keepAwake = keepAwake,
       _library = library;

  final LichessBoardApiClientFactory _clientFactory;
  final LichessPreparedSessionStore _store;
  final LichessPreparedSessionGate _gate;
  final LichessPreparedKeepAwake _keepAwake;
  final GameLibraryRepository _library;
  Future<void> _reconcileTail = Future<void>.value();
  Future<void> _moveTail = Future<void>.value();
  int _gameStreamEpoch = 0;
  final Stopwatch _receiptClock = Stopwatch()..start();
  Duration? _providerReceiptMonotonic;
  Duration? get providerReceiptMonotonic => _providerReceiptMonotonic;
  final _states = StreamController<LichessPreparedSessionState>.broadcast();
  LichessPreparedSessionState _state = LichessPreparedSessionState(
    stage: LichessPreparedSessionStage.idle,
    evidenceRevision: 0,
    candidates: const <LichessPlayingGame>[],
  );
  LichessPreparedSessionState get state => _state;
  Stream<LichessPreparedSessionState> get states => _states.stream;

  LichessAccountContext? _account;
  LichessBoardApiClient? _client;
  StreamSubscription<LichessAccountEvent>? _accountEvents;
  StreamSubscription<void>? _seekEvents;
  StreamSubscription<LichessGameStreamEvent>? _gameEvents;
  LichessGameStateEvent? _providerState;
  LichessPreparedIdentity? _providerIdentity;
  String? _providerGameId;
  String? _providerAccountId;
  bool _providerStreamConnected = false;
  int _providerEventRevision = 0;
  int _providerReconciledRevision = 0;
  (int, String)? _moveHttpInFlight;
  LichessPreparedRecovery? _gameRecovery;
  Completer<bool>? _opened;
  LichessPreparedRecord? _record;
  LichessPlayingInventory? _inventory;
  LichessPreparedGateState _gateState = LichessPreparedGateState.unknown;
  LichessPreparedIdentity? _gateIdentity;
  bool _storageCertain = true;
  bool _accountReady = false;
  bool _awake = false;
  bool _awakeUncertain = false;
  bool _disposed = false;
  bool _suspended = false;
  int _generation = 0;
  int _evidenceRevision = 0;
  int? _preparingGeneration;
  Future<void> _writeTail = Future<void>.value();
  Future<void> _gateTail = Future<void>.value();
  Future<void> _awakeTail = Future<void>.value();
  Future<void> _retirementTail = Future<void>.value();
  Future<void>? _refresh;
  bool _refreshAgain = false;

  bool _current(int generation) =>
      !_disposed && !_suspended && generation == _generation;
  bool get _hasRecord => _record != null && !_record!.released;

  void _publish(
    LichessPreparedSessionStage stage, [
    LichessPreparedRecovery? reason,
  ]) {
    if (_states.isClosed) return;
    final candidates = (_inventory?.games ?? const <LichessPlayingGame>[])
        .where(_compatible)
        .toList(growable: false);
    _state = LichessPreparedSessionState(
      stage: stage,
      evidenceRevision: _evidenceRevision,
      candidates: List<LichessPlayingGame>.unmodifiable(candidates),
      record: _record,
      inventory: _inventory,
      recovery: reason,
      gate: _gateState,
      accountReady: _accountReady,
      storageCertain: _storageCertain,
      gateIdentity: _gateIdentity,
      isRefreshingInventory: _refresh != null,
      providerState: _providerState,
      providerStreamConnected: _providerStreamConnected,
    );
    _states.add(_state);
  }

  static bool _compatible(LichessPlayingGame game) =>
      game.variantKey == 'standard' &&
      game.speed == 'rapid' &&
      !game.rated &&
      game.opponent.aiLevel == null;

  void _recover(LichessPreparedRecovery reason) =>
      _publish(LichessPreparedSessionStage.uncertain, reason);

  Future<void> restore(LichessAccountContext accountContext) async {
    if (_disposed) throw StateError('The prepared-session owner is disposed.');
    final retiring = _retire();
    final generation = _generation;
    await retiring;
    if (_disposed || generation != _generation) return;
    _suspended = false;
    _account = accountContext;
    _publish(LichessPreparedSessionStage.checking);
    try {
      final loaded = await _store.load();
      if (!_current(generation)) return;
      if (loaded.kind != LichessPreparedLoadKind.absent &&
          loaded.kind != LichessPreparedLoadKind.current) {
        _record ??= loaded.record;
        _storageCertain = false;
        _recover(LichessPreparedRecovery.storage);
        return;
      }
      if ((loaded.kind == LichessPreparedLoadKind.current &&
              loaded.record == null) ||
          (_record != null &&
              (loaded.record == null ||
                  loaded.record!.revision < _record!.revision))) {
        _storageCertain = false;
        _recover(LichessPreparedRecovery.storage);
        return;
      }
      _record = loaded.record;
      if (!_hasRecord ||
          _record!.identity != _providerIdentity ||
          _record!.providerGameId != _providerGameId ||
          _record!.accountId != _providerAccountId) {
        _providerState = null;
        _providerIdentity = null;
        _providerGameId = null;
        _providerAccountId = null;
      }
      _storageCertain = true;
      if (_hasRecord && _record!.accountId != accountContext.accountId) {
        _recover(LichessPreparedRecovery.accountMismatch);
        return;
      }
      if (!await _readGate(generation)) return;
      if (_hasRecord && !await _acquireAwake(generation)) return;
      if (!_current(generation)) return;
      final client = _clientFactory(accountContext);
      _client = client;
      final opened = Completer<bool>();
      _opened = opened;
      _accountEvents = client
          .streamAccountEvents(
            onOpened: () {
              if (!_current(generation)) return;
              _accountReady = true;
              if (!opened.isCompleted) opened.complete(true);
            },
          )
          .listen(
            (event) {
              if (!_current(generation)) return;
              if (event is LichessAccountGameEvent) {
                unawaited(refreshCandidates());
              }
            },
            onError: (Object error) {
              _accountLost(generation, opened);
            },
            onDone: () => _accountLost(generation, opened),
          );
      if (!await opened.future || !_current(generation) || !_accountReady) {
        return;
      }
      await refreshCandidates();
    } catch (_) {
      if (_current(generation)) _recover(LichessPreparedRecovery.storage);
    }
  }

  void _accountLost(int generation, Completer<bool> opened) {
    if (!opened.isCompleted) opened.complete(false);
    if (!_current(generation)) return;
    _accountReady = false;
    _gameStreamEpoch++;
    _providerStreamConnected = false;
    _client?.close();
    _evidenceRevision++;
    _recover(LichessPreparedRecovery.transport);
  }

  Future<void> prepare({
    required LichessPreparedIdentity boardContext,
    required LichessRealtimeSeek seek,
  }) async {
    final generation = _generation;
    if (!_current(generation) ||
        !_accountReady ||
        !_storageCertain ||
        _hasRecord ||
        _preparingGeneration == generation ||
        _inventory == null ||
        _inventory!.isUncertain) {
      _recover(LichessPreparedRecovery.notReady);
      return;
    }
    _preparingGeneration = generation;
    try {
      _publish(LichessPreparedSessionStage.preparing);
      if (!await _acquireAwake(generation)) return;
      if (!await _save(
        LichessPreparedRecord(
          identity: boardContext,
          accountId: _account!.accountId,
          seek: seek,
          revision: (_record?.revision ?? 0) + 1,
          seekMayHaveStarted: false,
        ),
        generation,
      )) {
        return;
      }
      if (!await _arm(boardContext, generation)) return;
      if (!await _maySeek(generation)) return;
      if (!await _save(_record!._next(seekMayHaveStarted: true), generation)) {
        return;
      }
      if (!await _maySeek(generation)) return;
      _publish(LichessPreparedSessionStage.matching);
      _seekEvents = _client!
          .seek(seek)
          .listen(
            (_) {},
            onError: (Object error) {
              if (_current(generation) &&
                  _accountReady &&
                  _record?.providerGameId == null) {
                _seekEvents = null;
                _recover(LichessPreparedRecovery.matchingStopped);
              }
            },
            onDone: () {
              if (_current(generation) &&
                  _accountReady &&
                  _record?.providerGameId == null) {
                _seekEvents = null;
                _recover(LichessPreparedRecovery.matchingStopped);
              }
            },
          );
    } finally {
      if (_preparingGeneration == generation) _preparingGeneration = null;
    }
  }

  Future<bool> _maySeek(int generation) async {
    // Inventory may change while an external gate/write acknowledgement waits.
    // Never admit a request on the earlier empty snapshot during a newer read.
    while (_refresh != null && _current(generation)) {
      await _refresh;
    }
    if (!_current(generation) || !_accountReady) return false;
    if (_inventory == null || _inventory!.isUncertain) {
      _recover(LichessPreparedRecovery.inventory);
      return false;
    }
    if (_inventory!.games.isNotEmpty) {
      _preparingGeneration = null;
      _presentInventory();
      return false;
    }
    return true;
  }

  Future<bool> _save(LichessPreparedRecord record, int generation) {
    final previous = _writeTail;
    final result = () async {
      await previous;
      if (!_current(generation)) return false;
      // Retain attempted evidence even if acknowledgement is lost.
      _record = record;
      _storageCertain = false;
      try {
        await _store.write(record);
        _storageCertain = true;
        return _current(generation);
      } catch (_) {
        if (_current(generation)) _recover(LichessPreparedRecovery.storage);
        return false;
      }
    }();
    _writeTail = result.then((_) {});
    return result;
  }

  Future<bool> _arm(LichessPreparedIdentity identity, int generation) {
    final previous = _gateTail;
    final result = () async {
      await previous;
      if (!_current(generation)) return false;
      _gateState = LichessPreparedGateState.pending;
      _publish(LichessPreparedSessionStage.preparing);
      try {
        final confirmed = await _gate.armAndReadback(identity);
        if (confirmed) _gateIdentity = identity;
        _gateState = confirmed
            ? LichessPreparedGateState.armed
            : LichessPreparedGateState.unknown;
        if (!confirmed && _current(generation)) {
          _recover(LichessPreparedRecovery.gate);
        }
        return confirmed && _current(generation);
      } catch (_) {
        _gateState = LichessPreparedGateState.unknown;
        if (_current(generation)) _recover(LichessPreparedRecovery.gate);
        return false;
      }
    }();
    _gateTail = result.then((_) {});
    return result;
  }

  Future<bool> _readGate(int generation) {
    final previous = _gateTail;
    final result = () async {
      await previous;
      if (!_current(generation)) return false;
      try {
        final observed = await _gate.readback();
        _gateIdentity = observed;
        _gateState = observed == null
            ? LichessPreparedGateState.unarmed
            : LichessPreparedGateState.armed;
        if (!_current(generation)) return false;
        final resettableUnarmed =
            _hasRecord &&
            _storageCertain &&
            !_record!.seekMayHaveStarted &&
            _record!.providerGameId == null &&
            observed == null;
        // This permits fresh evidence for explicit cancel/reset only. It does
        // not arm, release the record, start another seek, or permit binding.
        if ((_hasRecord &&
                observed != _record!.identity &&
                !resettableUnarmed) ||
            (!_hasRecord && observed != null)) {
          _recover(LichessPreparedRecovery.gate);
          return false;
        }
        return true;
      } catch (_) {
        _gateState = LichessPreparedGateState.unknown;
        if (_current(generation)) _recover(LichessPreparedRecovery.gate);
        return false;
      }
    }();
    _gateTail = result.then((_) {});
    return result;
  }

  Future<bool> _acquireAwake(int generation) {
    final previous = _awakeTail;
    final result = () async {
      await previous;
      if (!_current(generation)) return false;
      try {
        if (_awakeUncertain) throw StateError('Unresolved keep-awake release.');
        if (!_awake) _awake = await _keepAwake.acquire();
        if (!_awake && _current(generation)) {
          _recover(LichessPreparedRecovery.keepAwake);
        }
        return _awake && _current(generation);
      } catch (_) {
        if (_current(generation)) _recover(LichessPreparedRecovery.keepAwake);
        return false;
      }
    }();
    _awakeTail = result.then((_) {});
    return result;
  }

  Future<void> refreshCandidates() {
    final pending = _refresh;
    if (pending != null) {
      _refreshAgain = true;
      return pending;
    }
    final generation = _generation;
    final client = _client;
    if (!_current(generation) || client == null || !_accountReady) {
      return Future<void>.value();
    }
    // Invalidate displayed choices when checking begins, not only on success.
    _evidenceRevision++;
    final operation = () async {
      do {
        _refreshAgain = false;
        try {
          final inventory = await client.fetchPlayingInventory();
          if (!_current(generation) || !_accountReady) return;
          _inventory = inventory;
          _evidenceRevision++;
          _presentInventory();
        } catch (_) {
          if (_current(generation) && _accountReady) {
            _inventory = null;
            _evidenceRevision++;
            _recover(LichessPreparedRecovery.inventory);
          }
          return;
        }
      } while (_refreshAgain && _current(generation));
    }();
    _refresh = operation;
    _publish(_state.stage, _state.recovery);
    unawaited(
      operation.whenComplete(() {
        if (identical(_refresh, operation)) {
          _refresh = null;
          if (_current(generation)) _publish(_state.stage, _state.recovery);
        }
      }),
    );
    return operation;
  }

  void _presentInventory() {
    if (!_storageCertain &&
        _state.stage == LichessPreparedSessionStage.binding) {
      _publish(LichessPreparedSessionStage.binding);
    } else if (_preparingGeneration == _generation) {
      _publish(LichessPreparedSessionStage.preparing);
    } else if (!_storageCertain) {
      _recover(LichessPreparedRecovery.storage);
    } else if (_inventory == null || _inventory!.isUncertain) {
      _recover(LichessPreparedRecovery.inventory);
    } else if (_hasRecord && _gateState != LichessPreparedGateState.armed) {
      _recover(LichessPreparedRecovery.gate);
    } else if (_record?.providerGameId != null && _hasRecord) {
      _observeBoundGame();
      if (_gameRecovery case final reason?) {
        _recover(reason);
      } else {
        _publish(
          _providerStreamConnected && _record?.confirmed != null
              ? LichessPreparedSessionStage.boundProviderReconciled
              : LichessPreparedSessionStage.boundAwaitingReconciliation,
        );
      }
    } else if (_hasRecord && _inventory!.games.any(_compatible)) {
      _publish(LichessPreparedSessionStage.awaitingGameConfirmation);
    } else if (_hasRecord && _inventory!.games.isNotEmpty) {
      _recover(LichessPreparedRecovery.incompatibleGame);
    } else {
      if (_hasRecord && _seekEvents == null) {
        _recover(LichessPreparedRecovery.matchingStopped);
      } else {
        _publish(
          _hasRecord
              ? LichessPreparedSessionStage.matching
              : LichessPreparedSessionStage.idle,
        );
      }
    }
  }

  Future<void> confirmGame(String gameId, int evidenceRevision) async {
    final generation = _generation;
    // A delayed UI action must not replace storage/gate/lifecycle recovery with
    // a less useful selection error, or disturb a binding already in progress.
    if (!_current(generation) ||
        !_accountReady ||
        !_storageCertain ||
        !_hasRecord ||
        _gateState != LichessPreparedGateState.armed ||
        _record!.providerGameId != null ||
        _state.stage != LichessPreparedSessionStage.awaitingGameConfirmation) {
      return;
    }
    if (_refresh != null ||
        evidenceRevision != _evidenceRevision ||
        _inventory == null ||
        _inventory!.isUncertain) {
      _recover(LichessPreparedRecovery.staleSelection);
      return;
    }
    final choices = _inventory!.games.where(
      (game) => game.gameId == gameId && _compatible(game),
    );
    if (choices.length != 1) {
      _recover(LichessPreparedRecovery.incompatibleGame);
      return;
    }
    final selected = choices.single;
    _publish(LichessPreparedSessionStage.binding);
    if (!await _save(
      _record!._next(
        providerGameId: selected.gameId,
        providerSide: selected.color,
      ),
      generation,
    )) {
      return;
    }
    // Keep the account stream and awake lease. Only matching has finished;
    // the bound game's provider stream now owns its observable position.
    final bindingClient = _client;
    bindingClient?.cancelSeek();
    final seeking = _seekEvents;
    _seekEvents = null;
    try {
      await seeking?.cancel();
    } catch (_) {
      if (!_current(generation)) return;
      bindingClient?.close();
      _recover(LichessPreparedRecovery.transport);
      return;
    }
    if (!_current(generation)) return;
    if (!_accountReady) {
      _recover(LichessPreparedRecovery.transport);
      return;
    }
    _observeBoundGame();
    _publish(LichessPreparedSessionStage.boundAwaitingReconciliation);
  }

  Future<LichessLocalMoveSubmissionResult> submitLocalMove({
    required String uci,
    required String localCommandId,
  }) {
    final invocationGeneration = _generation;
    final previous = _moveTail;
    final result = () async {
      await previous;
      if (!_current(invocationGeneration)) {
        return LichessLocalMoveSubmissionResult.notReady;
      }
      final int generation = invocationGeneration;
      final int streamEpoch = _gameStreamEpoch;
      final int providerEventRevision = _providerEventRevision;
      final LichessBoardApiClient? client = _client;
      final LichessPreparedRecord? binding = _record;
      final LichessGameStateEvent? preState = _providerState;
      final LichessConfirmedCheckpoint? checkpoint = binding?.confirmed;
      if (!_canSubmitLocalMove(
        generation: generation,
        streamEpoch: streamEpoch,
        client: client,
        binding: binding,
        preState: preState,
        checkpoint: checkpoint,
      )) {
        return LichessLocalMoveSubmissionResult.notReady;
      }
      final int targetPly = preState!.moves.length + 1;
      final pending = LichessPendingLocalMove(
        identity: binding!.identity,
        providerGameId: binding.providerGameId!,
        targetPly: targetPly,
        uci: uci,
        localCommandId: localCommandId,
        preStatePly: preState.moves.length,
        preStatePrefixSha256: checkpoint!.prefixSha256,
        retryUsed: false,
        httpAcknowledged: false,
        rejected: false,
      );
      try {
        validatePendingLocalMove(pending);
      } on FormatException {
        return LichessLocalMoveSubmissionResult.notReady;
      }
      if (!await _save(binding._next(pendingLocalMove: pending), generation)) {
        return LichessLocalMoveSubmissionResult.storageFailure;
      }
      _publish(_state.stage, _state.recovery);
      if (!_moveAdmissionCurrent(
        generation,
        streamEpoch,
        providerEventRevision,
        client!,
        preState,
        pending,
      )) {
        _gameRecovery = LichessPreparedRecovery.providerPosition;
        _recover(LichessPreparedRecovery.providerPosition);
        return LichessLocalMoveSubmissionResult.notReady;
      }
      return _sendPendingMove(
        client: client,
        pending: pending,
        generation: generation,
        isRetry: false,
        dispatchGuard: () => _moveAdmissionCurrent(
          generation,
          streamEpoch,
          providerEventRevision,
          client,
          preState,
          pending,
        ),
      );
    }();
    _moveTail = result.then<void>((_) {});
    return result;
  }

  bool _canSubmitLocalMove({
    required int generation,
    required int streamEpoch,
    required LichessBoardApiClient? client,
    required LichessPreparedRecord? binding,
    required LichessGameStateEvent? preState,
    required LichessConfirmedCheckpoint? checkpoint,
  }) {
    if (!_current(generation) ||
        !_accountReady ||
        !_storageCertain ||
        client == null ||
        binding == null ||
        binding.providerGameId == null ||
        binding.providerSide == null ||
        binding.pendingLocalMove != null ||
        binding.confirmedLocalMove != null ||
        preState == null ||
        checkpoint == null ||
        !_providerStreamConnected ||
        _providerEventRevision != _providerReconciledRevision ||
        streamEpoch != _gameStreamEpoch ||
        preState.status.known?.isTerminal != false ||
        checkpoint.receipt.ply != preState.moves.length) {
      return false;
    }
    final bool whiteToMove = preState.moves.length.isEven;
    return whiteToMove == (binding.providerSide == LichessPlayerColor.white);
  }

  bool _moveAdmissionCurrent(
    int generation,
    int streamEpoch,
    int providerEventRevision,
    LichessBoardApiClient client,
    LichessGameStateEvent preState,
    LichessPendingLocalMove pending,
  ) {
    final record = _record;
    return _current(generation) &&
        _accountReady &&
        _storageCertain &&
        identical(_client, client) &&
        streamEpoch == _gameStreamEpoch &&
        providerEventRevision == _providerEventRevision &&
        providerEventRevision == _providerReconciledRevision &&
        identical(_providerState, preState) &&
        _providerStreamConnected &&
        record?.identity == pending.identity &&
        record?.providerGameId == pending.providerGameId &&
        record?.pendingLocalMove?.localCommandId == pending.localCommandId &&
        record?.confirmed?.receipt.ply == pending.preStatePly &&
        record?.confirmed?.prefixSha256 == pending.preStatePrefixSha256;
  }

  Future<LichessLocalMoveSubmissionResult> _sendPendingMove({
    required LichessBoardApiClient client,
    required LichessPendingLocalMove pending,
    required int generation,
    required bool isRetry,
    required bool Function() dispatchGuard,
  }) async {
    _moveHttpInFlight = (generation, pending.localCommandId);
    try {
      await client.makeMove(
        pending.providerGameId,
        pending.uci,
        dispatchGuard: dispatchGuard,
      );
      if (!_current(generation) ||
          identical(_client, client) == false ||
          _record?.pendingLocalMove?.localCommandId != pending.localCommandId) {
        return LichessLocalMoveSubmissionResult.ambiguous;
      }
      final acknowledged = pending.next(
        retryUsed: isRetry || pending.retryUsed,
        httpAcknowledged: true,
      );
      if (!await _save(
        _record!._next(pendingLocalMove: acknowledged),
        generation,
      )) {
        return LichessLocalMoveSubmissionResult.storageFailure;
      }
      _publish(_state.stage, _state.recovery);
      return LichessLocalMoveSubmissionResult.pending;
    } on LichessApiException catch (error) {
      if (!_current(generation) ||
          identical(_client, client) == false ||
          _record?.pendingLocalMove?.localCommandId != pending.localCommandId) {
        return LichessLocalMoveSubmissionResult.ambiguous;
      }
      final heldRecovery = switch (error.kind) {
        LichessApiFailureKind.rateLimited =>
          LichessPreparedRecovery.moveRateLimited,
        LichessApiFailureKind.unauthorized ||
        LichessApiFailureKind.unavailableCredentials =>
          LichessPreparedRecovery.moveAuthentication,
        LichessApiFailureKind.forbidden =>
          LichessPreparedRecovery.movePermission,
        _ => null,
      };
      if (heldRecovery != null) {
        final held = pending.next(
          retryUsed: isRetry || pending.retryUsed,
          recoveryHold: heldRecovery,
        );
        if (!await _save(_record!._next(pendingLocalMove: held), generation)) {
          return LichessLocalMoveSubmissionResult.storageFailure;
        }
        _publish(_state.stage, heldRecovery);
        _gameRecovery = heldRecovery;
        _recover(heldRecovery);
        return LichessLocalMoveSubmissionResult.ambiguous;
      }
      final bool serverFailure =
          error.statusCode != null &&
          error.statusCode! >= 500 &&
          error.statusCode! <= 599;
      final bool ambiguous =
          error.kind == LichessApiFailureKind.transport ||
          error.kind == LichessApiFailureKind.disposed ||
          error.kind == LichessApiFailureKind.streamOwnership ||
          serverFailure;
      if (ambiguous) {
        _gameRecovery = LichessPreparedRecovery.moveAmbiguous;
        _recover(LichessPreparedRecovery.moveAmbiguous);
        await _restartBoundGameObservation(generation, client);
        return LichessLocalMoveSubmissionResult.ambiguous;
      }
      final rejected = pending.next(
        retryUsed: isRetry || pending.retryUsed,
        rejected: true,
      );
      if (!await _save(
        _record!._next(pendingLocalMove: rejected),
        generation,
      )) {
        return LichessLocalMoveSubmissionResult.storageFailure;
      }
      _publish(_state.stage, _state.recovery);
      _gameRecovery = LichessPreparedRecovery.moveRejected;
      _recover(LichessPreparedRecovery.moveRejected);
      return LichessLocalMoveSubmissionResult.rejected;
    } on LichessCooldownPersistenceException {
      if (_current(generation)) {
        _recover(LichessPreparedRecovery.storage);
      }
      return LichessLocalMoveSubmissionResult.storageFailure;
    } finally {
      if (_moveHttpInFlight == (generation, pending.localCommandId)) {
        _moveHttpInFlight = null;
      }
    }
  }

  Future<void> _restartBoundGameObservation(
    int generation,
    LichessBoardApiClient client,
  ) async {
    if (!_current(generation) || !identical(_client, client)) return;
    final subscription = _gameEvents;
    _gameEvents = null;
    _gameStreamEpoch++;
    _providerStreamConnected = false;
    client.cancelGameStream();
    try {
      await subscription?.cancel();
    } catch (_) {
      if (_current(generation) && identical(_client, client)) {
        _recover(LichessPreparedRecovery.transport);
      }
      return;
    }
    if (!_current(generation) || !identical(_client, client)) return;
    _observeBoundGame();
  }

  void _observeBoundGame() {
    final record = _record;
    final client = _client;
    if (_gameEvents != null ||
        record == null ||
        record.providerGameId == null ||
        record.providerSide == null ||
        client == null ||
        !_accountReady) {
      return;
    }
    if (_providerIdentity != record.identity ||
        _providerGameId != record.providerGameId ||
        _providerAccountId != record.accountId) {
      _providerState = null;
      _providerIdentity = record.identity;
      _providerGameId = record.providerGameId;
      _providerAccountId = record.accountId;
    }
    final generation = _generation;
    final streamEpoch = ++_gameStreamEpoch;
    var rejected = false;
    var identityValidated = false;
    void reject(LichessPreparedRecovery reason) {
      rejected = true;
      _gameStreamEpoch++;
      _providerStreamConnected = false;
      _gameRecovery = reason;
      _recover(reason);
      final rejectedSubscription = _gameEvents;
      unawaited(() async {
        try {
          await rejectedSubscription?.cancel();
        } catch (_) {
          // Rejection already removed callback authority and input eligibility.
        }
      }());
    }

    _gameEvents = client
        .streamGame(record.providerGameId!)
        .listen(
          (event) {
            if (!_current(generation) ||
                rejected ||
                !_accountReady ||
                !identical(_client, client)) {
              return;
            }
            LichessGameStateEvent? next;
            if (event is LichessGameFullEvent) {
              final player = record.providerSide == LichessPlayerColor.white
                  ? event.white
                  : event.black;
              if (event.gameId != record.providerGameId ||
                  player.id != record.accountId) {
                reject(LichessPreparedRecovery.providerIdentity);
                return;
              }
              if (event.variantKey != 'standard' ||
                  event.speed != 'rapid' ||
                  event.rated ||
                  (event.initialFen != 'startpos' &&
                      event.initialFen !=
                          'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1')) {
                reject(LichessPreparedRecovery.incompatibleGame);
                return;
              }
              identityValidated = true;
              next = event.state;
            } else if (event is LichessGameStateEvent) {
              if (!identityValidated) {
                reject(LichessPreparedRecovery.providerIdentity);
                return;
              }
              next = event;
            }
            if (next == null) return;
            final accepted = next;
            final eventRevision = ++_providerEventRevision;
            final receipt = LichessProviderReceipt.fromEvent(
              accepted,
              DateTime.now(),
            );
            final monotonicReceipt = _receiptClock.elapsed;
            final previous = _reconcileTail;
            _reconcileTail = () async {
              await previous;
              if (!_streamCurrent(generation, streamEpoch, client, record)) {
                return;
              }
              try {
                await _reconcileProvider(
                  accepted,
                  receipt,
                  monotonicReceipt,
                  generation,
                  streamEpoch,
                  client,
                  record,
                );
                if (_streamCurrent(generation, streamEpoch, client, record)) {
                  _providerReconciledRevision = eventRevision;
                }
              } on _LichessMoveRecoveryException catch (error) {
                if (_streamCurrent(generation, streamEpoch, client, record)) {
                  reject(error.recovery);
                }
              } on FormatException {
                if (_streamCurrent(generation, streamEpoch, client, record)) {
                  reject(LichessPreparedRecovery.providerPosition);
                }
              } catch (_) {
                if (_streamCurrent(generation, streamEpoch, client, record)) {
                  _storageCertain = false;
                  reject(LichessPreparedRecovery.storage);
                }
              }
            }();
          },
          onError: (Object error) {
            if (!_current(generation) ||
                rejected ||
                !_accountReady ||
                !identical(_client, client)) {
              return;
            }
            _gameStreamEpoch++;
            _providerStreamConnected = false;
            _gameRecovery = LichessPreparedRecovery.transport;
            _recover(LichessPreparedRecovery.transport);
          },
          onDone: () {
            if (!_current(generation) ||
                rejected ||
                !_accountReady ||
                !identical(_client, client)) {
              return;
            }
            _gameStreamEpoch++;
            _providerStreamConnected = false;
            if (_gameRecovery case final reason?) {
              _recover(reason);
            } else if (_providerState?.status.known?.isTerminal == true) {
              _publish(LichessPreparedSessionStage.boundAwaitingReconciliation);
            } else {
              _gameRecovery = LichessPreparedRecovery.transport;
              _recover(LichessPreparedRecovery.transport);
            }
          },
        );
  }

  /// Invalidates callback authority synchronously, before awaiting any cleanup.
  Future<void> _retire() {
    _generation++;
    _gameStreamEpoch++;
    _providerReceiptMonotonic = null;
    _client?.close();
    _client = null;
    _accountReady = false;
    if (_opened case final opened? when !opened.isCompleted) {
      opened.complete(false);
    }
    _opened = null;
    final subscriptions = <StreamSubscription<dynamic>>[
      ?_accountEvents,
      ?_seekEvents,
      ?_gameEvents,
    ];
    _accountEvents = null;
    _seekEvents = null;
    _gameEvents = null;
    _providerStreamConnected = false;
    _gameRecovery = null;
    _moveHttpInFlight = null;
    _moveTail = Future<void>.value();
    _inventory = null;
    _refresh = null;
    _refreshAgain = false;
    _evidenceRevision++;
    _publish(LichessPreparedSessionStage.retiring);
    final previous = _retirementTail;
    final retirement = () async {
      await previous;
      for (final subscription in subscriptions) {
        try {
          await subscription.cancel();
        } catch (_) {
          /* Client already closed. */
        }
      }
      await _reconcileTail;
      await _writeTail;
      await _gateTail;
      await _awakeTail;
      if (_awake) {
        try {
          await _keepAwake.release();
          _awake = false;
          _awakeUncertain = false;
        } catch (_) {
          _awakeUncertain = true;
        }
      }
    }();
    _retirementTail = retirement;
    return retirement;
  }

  Future<void> cancel() async {
    if (_disposed) return;
    // Zero is useful only together with durable proof this owner never admitted
    // a seek. Retirement first closes every pending HTTP/credential operation.
    final zeroBeforeRetirement =
        _accountReady &&
        _refresh == null &&
        _inventory != null &&
        !_inventory!.isUncertain &&
        _inventory!.games.isEmpty;
    final retirement = _retire();
    final generation = _generation;
    await retirement;
    if (!_current(generation)) return;
    if (!_storageCertain) {
      _recover(LichessPreparedRecovery.storage);
      return;
    }
    if (!_hasRecord) {
      _publish(LichessPreparedSessionStage.idle);
      return;
    }
    if (!zeroBeforeRetirement ||
        _record!.seekMayHaveStarted ||
        _record!.providerGameId != null) {
      _recover(LichessPreparedRecovery.matchingStopped);
      return;
    }
    final identity = _record!.identity;
    final previous = _gateTail;
    final disarming = () async {
      await previous;
      if (!_current(generation)) return false;
      _gateState = LichessPreparedGateState.pending;
      _publish(LichessPreparedSessionStage.retiring);
      try {
        final confirmed = await _gate.disarmPreSeek(identity);
        if (confirmed) _gateIdentity = null;
        _gateState = confirmed
            ? LichessPreparedGateState.unarmed
            : LichessPreparedGateState.unknown;
        return confirmed;
      } catch (_) {
        _gateState = LichessPreparedGateState.unknown;
        return false;
      }
    }();
    _gateTail = disarming.then((_) {});
    final confirmed = await disarming;
    if (!_current(generation)) return;
    if (!confirmed) {
      _recover(LichessPreparedRecovery.gate);
      return;
    }
    if (await _save(_record!._next(released: true), generation)) {
      _publish(LichessPreparedSessionStage.idle);
    }
  }

  Future<void> background() async {
    if (_disposed) return;
    _suspended = true;
    final retirement = _retire();
    final generation = _generation;
    await retirement;
    if (!_disposed && generation == _generation) {
      _publish(LichessPreparedSessionStage.suspended);
    }
  }

  Future<void> resume() {
    final account = _account;
    if (account == null) {
      _recover(LichessPreparedRecovery.notReady);
      return Future<void>.value();
    }
    return restore(account);
  }

  Future<void> replaceAccount(LichessAccountContext accountContext) =>
      restore(accountContext);

  Future<void> dispose() async {
    if (_disposed) return _retirementTail;
    _disposed = true;
    await _retire();
    _publish(LichessPreparedSessionStage.disposed);
    await _states.close();
  }
}
