import 'dart:async';
import 'dart:ui' show Locale;

import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../../l10n/generated/app_localizations.dart';
import '../models/home_assistance_state.dart';
import '../models/home_state_action.dart';
import '../models/home_state_notification.dart';
import '../models/litch_control_action.dart';
import '../models/support_action.dart';
import '../services/litch_ble_service.dart';

enum BoardActionStatusTone { progress, success, info, warning, error }

enum _ReadbackNotice { syncing, unavailable }

enum _UndoNotice { ready, complete, cancelled, finished }

class _BoardActionIntent {
  _BoardActionIntent(
    this.action,
    this.owner,
    this.desired,
    this.position, {
    this.showMove = false,
    this.hintCoachingOwned = false,
  });

  final LitchControlAction action;
  final LitchControlOwner? owner;
  final bool? desired;
  final String? position;
  final bool showMove;
  final bool hintCoachingOwned;
  int? baselineSequence;
  int supportAction = 0;
  int busyRetries = 0;

  bool sameTarget(_BoardActionIntent other) =>
      action == other.action &&
      owner == other.owner &&
      desired == other.desired &&
      showMove == other.showMove &&
      hintCoachingOwned == other.hintCoachingOwned &&
      (action == LitchControlAction.coaching || position == other.position);
}

class BoardActionsController extends ChangeNotifier {
  // Presentation locale changes never recreate the controller or BLE owner.
  AppLocalizations _strings = lookupAppLocalizations(const Locale('en'));
  void useLocalizations(AppLocalizations strings) {
    _strings = strings;
  }

  static const Duration coachingSyncTimeout = Duration(seconds: 3);
  static const Duration hintSyncTimeout = Duration(seconds: 3);
  BoardActionsController({
    required LitchBleService bleService,
    required String boardId,
  }) : _bleService = bleService,
       _boardId = boardId {
    _bindServiceStreams();
  }

  final LitchBleService _bleService;
  final String _boardId;
  StreamSubscription<LitchControlEvent>? _controlActionSub;
  StreamSubscription<ConnectionStateUpdate>? _connectionSub;
  StreamSubscription<HomeStateNotification>? _homeStateSub;
  StreamSubscription<String>? _playerSettingsSub;
  int _playerSettingsGeneration = 0;
  Future<void>? _playerSettingsRead;
  bool _connectionActive = false;
  int? _cpuLevel;
  LitchControlOwner? _cpuOwner;
  final Map<String, LitchControlEvent> _controlEvents =
      <String, LitchControlEvent>{};
  final Map<String, int> _activeControlRequestIds = <String, int>{};
  final Set<String> _startingControlActions = <String>{};
  final Set<int> _cancelUndoRequestIds = <int>{};
  // At most one intent per existing control. Taps capture the displayed action,
  // so a double Undo tap cannot silently become Cancel Undo after the first ACK.
  final Map<String, _BoardActionIntent> _queuedActions = {};
  _BoardActionIntent? _activeAction;
  bool _pumpScheduled = false;
  int _actionGeneration = 0;
  String? _position;
  Timer? _recoveryTimer;
  Timer? _snapshotRetryTimer;
  Timer? _busyRetryTimer;
  LitchControlEvent? _recoveryEvent;
  _BoardActionIntent? _recoveringAction;

  LitchControlEvent? _lastControlEvent;
  LitchControlActionState _controlActionState = LitchControlActionState.unknown;
  bool _lastControlEventWasCancelUndo = false;
  bool _undoHomeProgressObserved = false;
  bool _undoCompletedFromBoard = false;
  int? _lastHomeStateSequence;
  HomeStateAction? _lastHomeStateAction;
  bool? _lastHomeUndoPending;
  int? _coachingMask;
  HomeAssistanceState? _assistance;
  Timer? _hintSyncTimer;
  _ReadbackNotice? _hintSyncStatus;
  bool _hintRequestTracking = false;
  bool? _hintRequestedValue;
  LitchControlOwner? _coachingRequestOwner;
  bool? _coachingRequestedValue;
  Timer? _coachingSyncTimer;
  _ReadbackNotice? _coachingSyncStatus;
  String? _undoForwardFen;
  _UndoNotice? _boardUndoStatusMessage;
  bool _undoRequestTracking = false;
  int? _undoRequestBaselineSequence;
  ({bool hint, bool undo}) _gameAllows = (hint: true, undo: true);
  bool _setupIncomplete = true;
  bool _disposed = false;

  String get targetId => _bleService.connectedId ?? _boardId;
  int get sessionToken => _actionGeneration;

  LitchControlAvailability get availability =>
      _bleService.controlActionAvailability(targetId);

  bool get hasPendingAction =>
      _recoveryTimer != null ||
      _hintSyncTimer != null ||
      _coachingSyncTimer != null ||
      _startingControlActions.isNotEmpty ||
      _controlEvents.values.any((LitchControlEvent event) => event.isPending);

  bool get canDispatch =>
      !_disposed && availability != LitchControlAvailability.unavailable;

  bool canDispatchAction(
    LitchControlAction action, {
    LitchControlOwner? owner,
  }) {
    if (!canDispatch) {
      return false;
    }
    if (action == LitchControlAction.coaching &&
        (owner == null || coachingUnavailableReason(owner) != null)) {
      return false;
    }
    if (action == LitchControlAction.hint &&
        (_setupIncomplete || _assistance?.manualAvailable == false)) {
      return false;
    }
    return switch (action) {
      LitchControlAction.hint => _gameAllows.hint,
      LitchControlAction.undo => _gameAllows.undo,
      _ => true,
    };
  }

  bool get undoPending =>
      _controlActionState == LitchControlActionState.undoPending;

  String get undoLabel =>
      undoPending ? _strings.cancelUndo : _strings.undoAction;

  bool? get hintRequested => _assistance?.requested;

  bool? get hintSelected =>
      _assistance?.coachingOwned == true ? null : hintRequested;

  bool get canPresentHintGuidance =>
      _hintSyncTimer == null && _assistance != null;

  bool get hintCalculating =>
      !_disposed &&
      _connectionActive &&
      _bleService.isConnected &&
      _bleService.connectedId == _boardId &&
      canPresentHintGuidance &&
      switch (_assistance?.presentation) {
        HomeAssistancePresentation.pending ||
        HomeAssistancePresentation.thinking => true,
        _ => false,
      };

  bool _replaysHint(HomeAssistanceState? state) =>
      state?.coachingOwned == true;

  String get hintLabel {
    final HomeAssistanceState? state = _assistance;
    if (_replaysHint(state)) return _strings.showMoveAction;
    if (state == null || !state.requested) {
      return _strings.hintAction;
    }
    return state.isReady ? _strings.hideHint : _strings.cancelHint;
  }

  String get hintSemanticLabel => _replaysHint(_assistance)
      ? _strings.showMoveHelp
      : hintRequested == true
      ? _strings.hintOnBoard(hintLabel)
      : _strings.askHint;

  bool? coachingEnabled(LitchControlOwner owner) {
    final int? mask = _coachingMask;
    return mask == null
        ? null
        : (mask & (owner == LitchControlOwner.p1 ? 1 : 2)) != 0;
  }

  bool get _hasPlayerRoles =>
      _cpuLevel != null && (_cpuLevel == 0 || _cpuOwner != null);

  Future<void> refreshPlayerRolesIfNeeded() {
    if (_disposed ||
        !_connectionActive ||
        _hasPlayerRoles ||
        !_bleService.isConnected ||
        _bleService.connectedId != _boardId) {
      return Future<void>.value();
    }
    return _playerSettingsRead ?? _refreshPlayerSettings();
  }

  String? coachingUnavailableReason(LitchControlOwner owner) {
    if (!_connectionActive ||
        !_bleService.isConnected ||
        _bleService.connectedId != _boardId ||
        !_hasPlayerRoles) {
      return _strings.playerRoleUnavailable;
    }
    return _cpuLevel != 0 && _cpuOwner == owner
        ? _strings.computerPlayer
        : null;
  }

  bool isPending(LitchControlAction action, [LitchControlOwner? owner]) {
    return (action == LitchControlAction.hint && _hintSyncTimer != null) ||
        (action == LitchControlAction.coaching &&
            owner == _coachingRequestOwner &&
            _coachingSyncTimer != null) ||
        _startingControlActions.contains(_controlKey(action, owner)) ||
        (_controlEvent(action, owner)?.isPending ?? false);
  }

  bool isQueuedOrPending(LitchControlAction action) =>
      _activeAction?.action == action ||
      _queuedActions.values.any((intent) => intent.action == action) ||
      isPending(action);

  String? get statusMessage {
    if (_recoveryTimer != null) return null;
    if (_lastControlEvent case final event?
        when event.isLocalFailure ||
            event.phase == LitchControlPhase.rejected) {
      return _controlStatusMessage(event);
    }
    if (_hintSyncStatus case final status?) {
      return status == _ReadbackNotice.syncing
          ? _strings.hintSyncing
          : _strings.hintStateUnavailable;
    }
    if (_coachingSyncTimer == null && _coachingSyncStatus != null) {
      return _strings.coachingStateUnavailable;
    }
    if (_boardUndoStatusMessage case final status?) {
      return switch (status) {
        _UndoNotice.ready => _strings.undoReady,
        _UndoNotice.complete => _strings.undoComplete,
        _UndoNotice.cancelled => _strings.undoCancelled,
        _UndoNotice.finished => _strings.undoFinished,
      };
    }
    if (_lastControlEvent case final LitchControlEvent event) {
      return _controlStatusMessage(event);
    }
    return _hintPresentationMessage;
  }

  String? get _hintPresentationMessage {
    final HomeAssistanceState? state = _assistance;
    if (state == null) {
      return null;
    }
    return switch (state.presentation) {
      HomeAssistancePresentation.pending => _strings.hintWaiting,
      HomeAssistancePresentation.thinking => _strings.hintThinking,
      HomeAssistancePresentation.ready ||
      HomeAssistancePresentation.refreshing ||
      HomeAssistancePresentation.compoundDuck ||
      HomeAssistancePresentation.duck =>
        state.coachingOwned ? _strings.coachingHintReady : _strings.hintReady,
      HomeAssistancePresentation.off => null,
      HomeAssistancePresentation.suppressed ||
      HomeAssistancePresentation.legal => null,
    };
  }

  BoardActionStatusTone get statusTone {
    if (_recoveryTimer != null) return BoardActionStatusTone.progress;
    if (_lastControlEvent case final event?
        when event.isLocalFailure ||
            event.phase == LitchControlPhase.rejected) {
      return event.phase == LitchControlPhase.unavailable
          ? BoardActionStatusTone.warning
          : BoardActionStatusTone.error;
    }
    if (_hintSyncStatus != null) {
      return _hintSyncTimer != null
          ? BoardActionStatusTone.progress
          : BoardActionStatusTone.warning;
    }
    if (_coachingSyncStatus != null) {
      return _coachingSyncTimer != null
          ? BoardActionStatusTone.progress
          : BoardActionStatusTone.warning;
    }
    if (_boardUndoStatusMessage != null) {
      return undoPending
          ? BoardActionStatusTone.progress
          : BoardActionStatusTone.success;
    }
    if ((_lastControlEvent == null ||
            (_lastControlEvent?.action == LitchControlAction.hint &&
                _lastControlEvent?.phase == LitchControlPhase.applied)) &&
        _hintPresentationMessage != null) {
      return _assistance!.isReady
          ? BoardActionStatusTone.success
          : BoardActionStatusTone.progress;
    }
    return switch (_lastControlEvent?.phase) {
      LitchControlPhase.pending ||
      LitchControlPhase.accepted => BoardActionStatusTone.progress,
      LitchControlPhase.applied => BoardActionStatusTone.success,
      LitchControlPhase.noop => BoardActionStatusTone.info,
      LitchControlPhase.rejected ||
      LitchControlPhase.timeout ||
      LitchControlPhase.sendFailed ||
      LitchControlPhase.interrupted => BoardActionStatusTone.error,
      LitchControlPhase.unavailable => BoardActionStatusTone.warning,
      null => BoardActionStatusTone.info,
    };
  }

  void _bindServiceStreams() {
    _connectionActive =
        _bleService.isConnected && _bleService.connectedId == _boardId;
    _controlActionSub = _bleService.controlActionEvents.listen(
      _onControlActionEvent,
    );
    _connectionSub = _bleService.connectionStream.listen(_onConnectionChanged);
    _homeStateSub = _bleService.homeStateStream.listen(_onHomeState);
    _playerSettingsSub = _bleService.settingsChangedStream.listen((key) {
      if (key == 'VS_CPU_LEVEL' || key == 'VS_CPU_PLAYER') {
        unawaited(_refreshPlayerSettings());
      }
    });
    unawaited(_refreshPlayerSettings());
    if (_bleService.latestHomeState case final HomeStateNotification state) {
      _onHomeState(state);
    }
  }

  Future<void> _refreshPlayerSettings({bool useCache = true}) {
    final int generation = ++_playerSettingsGeneration;
    if (!_connectionActive) return Future<void>.value();
    Object? cached(String key) =>
        useCache ? _bleService.latestSettingValue(key) : null;
    int? level(Object? value) {
      final int? parsed = value is int ? value : int.tryParse('$value');
      return parsed != null && parsed >= 0 && parsed <= 11 ? parsed : null;
    }

    LitchControlOwner? owner(Object? value) => switch (value) {
      1 || '1' => LitchControlOwner.p1,
      2 || '2' => LitchControlOwner.p2,
      _ => null,
    };
    _cpuLevel = level(cached('VS_CPU_LEVEL'));
    _cpuOwner = owner(cached('VS_CPU_PLAYER'));
    _notify();
    Future<Object?> read(String key, Object? value) async {
      if (value != null) return value;
      try {
        return await _bleService.readSettingValue(deviceId: _boardId, key: key);
      } catch (_) {
        return null;
      }
    }

    final Future<void> reading =
        Future.wait<Object?>(<Future<Object?>>[
          read('VS_CPU_LEVEL', _cpuLevel),
          read(
            'VS_CPU_PLAYER',
            _cpuOwner == LitchControlOwner.p1
                ? 1
                : _cpuOwner == null
                ? null
                : 2,
          ),
        ]).then((values) {
          if (_disposed ||
              !_connectionActive ||
              generation != _playerSettingsGeneration ||
              !_bleService.isConnected ||
              _bleService.connectedId != _boardId) {
            return;
          }
          _cpuLevel = level(values[0]);
          _cpuOwner = owner(values[1]);
          _notify();
        });
    _playerSettingsRead = reading;
    return reading.whenComplete(() {
      if (identical(_playerSettingsRead, reading)) _playerSettingsRead = null;
    });
  }

  void _clearPresentation() {
    _clearRecovery();
    _snapshotRetryTimer?.cancel();
    _snapshotRetryTimer = null;
    ++_actionGeneration;
    for (final intent in _queuedActions.values) {
      _bleService.recordSupportActionOutcome(
        intent.supportAction,
        SupportActionOutcome.canceled,
      );
    }
    if (_activeAction case final active?) {
      _bleService.recordSupportActionOutcome(
        active.supportAction,
        SupportActionOutcome.unconfirmed,
      );
    }
    _queuedActions.clear();
    _activeAction = null;
    _position = null;
    _setupIncomplete = true;
    _clearCoachingSync();
    _clearHintSync();
    _assistance = null;
    _hintRequestTracking = false;
    _hintRequestedValue = null;
    _controlEvents.clear();
    _activeControlRequestIds.clear();
    _startingControlActions.clear();
    _cancelUndoRequestIds.clear();
    _lastControlEvent = null;
    _controlActionState = LitchControlActionState.unknown;
    _lastControlEventWasCancelUndo = false;
    _undoHomeProgressObserved = false;
    _undoCompletedFromBoard = false;
    _lastHomeStateSequence = null;
    _lastHomeStateAction = null;
    _lastHomeUndoPending = null;
    _coachingMask = null;
    _undoForwardFen = null;
    _boardUndoStatusMessage = null;
    _undoRequestTracking = false;
    _undoRequestBaselineSequence = null;
    _gameAllows = (hint: true, undo: true);
  }

  void _notify() {
    if (!_disposed) {
      notifyListeners();
      if (!hasPendingAction) {
        _snapshotRetryTimer?.cancel();
        _snapshotRetryTimer = null;
        _activeAction = null;
        if (!_pumpScheduled && _queuedActions.isNotEmpty) {
          _pumpScheduled = true;
          scheduleMicrotask(() {
            _pumpScheduled = false;
            _pumpActions();
          });
        }
      }
    }
  }

  void _onConnectionChanged(ConnectionStateUpdate update) {
    if (update.deviceId != _boardId && _bleService.connectedId == _boardId) {
      return;
    }
    _connectionActive =
        update.deviceId == _boardId &&
        update.connectionState == DeviceConnectionState.connected &&
        _bleService.connectedId == _boardId;
    ++_playerSettingsGeneration;
    _playerSettingsRead = null;
    _cpuLevel = null;
    _cpuOwner = null;
    if (_connectionActive) {
      unawaited(_refreshPlayerSettings(useCache: false));
    } else {
      _clearPresentation();
      _notify();
    }
  }

  void _onHomeState(HomeStateNotification newest) {
    if (!isNewerHomeStateSequence(newest.sequence, _lastHomeStateSequence)) {
      return;
    }
    _position = newest.position.fen;

    // Coarse setup authority remains meaningful when optional assistance is
    // omitted or cleared during hydration. Fresh Setup alone stays reversible
    // after the opening is playable and must not block Hint.
    final bool setupIncomplete =
        newest.physicalStatus.placementMode == true ||
        (newest.gameStatus.variant == 'undecided' &&
            newest.physicalStatus.occupiedSquares !=
                newest.position.occupiedSquares) ||
        newest.kingQueenSwapPromptActive;
    final bool setupChanged = _setupIncomplete != setupIncomplete;
    _setupIncomplete = setupIncomplete;

    final bool wasAuthoritativelyPending = _lastHomeUndoPending == true;
    final bool assistanceChanged = _assistance != newest.assistance;
    _assistance = newest.assistance;
    final bool hintSyncFinished =
        _hintSyncStatus != null &&
        (newest.assistance == null ||
            newest.assistance!.coachingOwned ||
            _hintRequestedValue == null ||
            newest.assistance!.requested == _hintRequestedValue);
    if (hintSyncFinished) {
      if (_hintSyncTimer != null) {
        _bleService.recordControlRecovery(confirmed: true);
      }
      _clearHintSync();
    } else if (_hintSyncTimer != null) {
      // A newer revision may still carry the unconfirmed old Hint latch.
      // Keep remount hydration as uncertain as the currently visible controls.
      _bleService.discardCachedHomeAssistance(
        boardId: _boardId,
        sequence: newest.sequence,
      );
    }
    final bool coachingChanged = _coachingMask != newest.coachingMask;
    _coachingMask = newest.coachingMask;
    final bool coachingSyncFinished =
        _coachingSyncStatus != null && newest.coachingMask != null;
    if (coachingSyncFinished) {
      if (_coachingSyncTimer != null) {
        _bleService.recordControlRecovery(confirmed: true);
      }
      _clearCoachingSync();
    }
    final bool actionPresentationChanged =
        setupChanged ||
        coachingChanged ||
        coachingSyncFinished ||
        assistanceChanged ||
        hintSyncFinished;
    final finished = newest.gameStatus.isFinished;
    final gameAllows = (
      hint: !finished,
      undo:
          !finished ||
          (newest.gameStatus.outcome != 'timeout' &&
              (newest.undoPending == true ||
                  newest.lastMoves?.white != null ||
                  newest.lastMoves?.black != null)),
    );
    final bool gameAvailabilityChanged = _gameAllows != gameAllows;
    _gameAllows = gameAllows;
    _lastHomeStateSequence = newest.sequence;
    _lastHomeStateAction = newest.action;
    if (_recoveryEvent != null) _reconcileRecovery(newest);
    if (newest.undoPending != null) {
      final bool postRequestSnapshot =
          _undoRequestTracking &&
          (_undoRequestBaselineSequence == null ||
              isNewerHomeStateSequence(
                newest.sequence,
                _undoRequestBaselineSequence,
              ));
      final bool wasPending =
          wasAuthoritativelyPending ||
          _controlActionState == LitchControlActionState.undoPending;
      _lastHomeUndoPending = newest.undoPending;
      if (newest.undoPending!) {
        _controlActionState = LitchControlActionState.undoPending;
        _undoHomeProgressObserved = true;
        _undoCompletedFromBoard = false;
        _lastControlEventWasCancelUndo = false;
        _boardUndoStatusMessage = _UndoNotice.ready;
        _notify();
        return;
      }

      final bool finished = wasPending || postRequestSnapshot;
      final bool cancelled =
          finished &&
          _undoForwardFen != null &&
          _undoForwardFen == newest.position.fen;
      final bool completed =
          finished &&
          _undoForwardFen != null &&
          _undoForwardFen != newest.position.fen;
      _controlActionState = LitchControlActionState.idle;
      _undoHomeProgressObserved = false;
      if (finished) {
        _undoCompletedFromBoard = completed;
        _lastControlEventWasCancelUndo = cancelled;
        _boardUndoStatusMessage = completed
            ? _UndoNotice.complete
            : cancelled
            ? _UndoNotice.cancelled
            : _UndoNotice.finished;
      }
      _undoForwardFen = newest.position.fen;
      _notify();
      return;
    }

    if (_controlActionState != LitchControlActionState.undoPending) {
      if (gameAvailabilityChanged || actionPresentationChanged) {
        _notify();
      }
      return;
    }
    if (newest.action != HomeStateAction.idle) {
      _undoHomeProgressObserved = true;
      if (actionPresentationChanged) {
        _notify();
      }
      return;
    }
    if (!_undoHomeProgressObserved || isPending(LitchControlAction.undo)) {
      if (actionPresentationChanged) {
        _notify();
      }
      return;
    }
    _controlActionState = LitchControlActionState.idle;
    _undoHomeProgressObserved = false;
    _undoCompletedFromBoard = true;
    _notify();
  }

  String _controlKey(LitchControlAction action, [LitchControlOwner? owner]) {
    return '${action.name}:${owner?.name ?? '-'}';
  }

  LitchControlEvent? _controlEvent(
    LitchControlAction action, [
    LitchControlOwner? owner,
  ]) {
    return _controlEvents[_controlKey(action, owner)];
  }

  void _onControlActionEvent(LitchControlEvent event) {
    final String key = _controlKey(event.action, event.owner);
    final int? requestId = event.requestId;
    if (event.action == LitchControlAction.hint &&
        event.phase == LitchControlPhase.pending) {
      _hintRequestTracking = true;
      _hintRequestedValue = _activeAction?.action == LitchControlAction.hint
          ? _activeAction!.desired
          : _assistance == null
          ? null
          : _replaysHint(_assistance) || !_assistance!.requested;
    }
    if (event.action == LitchControlAction.undo &&
        event.phase == LitchControlPhase.pending &&
        requestId != null) {
      if (_controlActionState == LitchControlActionState.undoPending) {
        _cancelUndoRequestIds.add(requestId);
      }
      _undoRequestTracking = true;
      _undoRequestBaselineSequence = _lastHomeStateSequence;
    }
    if (event.phase == LitchControlPhase.pending && requestId != null) {
      _activeControlRequestIds[key] = requestId;
    } else {
      final int? activeRequestId = _activeControlRequestIds[key];
      if (requestId != null &&
          activeRequestId != null &&
          requestId != activeRequestId) {
        return;
      }
      if (requestId == null || event.isTerminal) {
        _activeControlRequestIds.remove(key);
      }
    }

    final bool completedCancelUndo =
        event.action == LitchControlAction.undo &&
        event.isTerminal &&
        requestId != null &&
        _cancelUndoRequestIds.remove(requestId);
    LitchControlActionState nextActionState = event.actionState;
    if (nextActionState == LitchControlActionState.unknown &&
        event.action == LitchControlAction.undo &&
        event.phase == LitchControlPhase.applied) {
      nextActionState = undoPending
          ? LitchControlActionState.idle
          : LitchControlActionState.undoPending;
    }
    final bool authoritativeHomeSupersedes =
        event.action == LitchControlAction.undo &&
        (event.isTerminal || event.phase == LitchControlPhase.accepted) &&
        _undoRequestTracking &&
        _lastHomeUndoPending != null &&
        _lastHomeStateSequence != null &&
        (_undoRequestBaselineSequence == null ||
            isNewerHomeStateSequence(
              _lastHomeStateSequence!,
              _undoRequestBaselineSequence,
            ));
    if (authoritativeHomeSupersedes) {
      nextActionState = _lastHomeUndoPending!
          ? LitchControlActionState.undoPending
          : LitchControlActionState.idle;
    }

    _controlEvents[key] = event;
    _startingControlActions.remove(key);
    _lastControlEvent = event;
    if (event.action == LitchControlAction.undo) {
      if (!authoritativeHomeSupersedes) {
        _undoCompletedFromBoard = false;
        _boardUndoStatusMessage = null;
      }
      if (nextActionState == LitchControlActionState.undoPending) {
        _undoHomeProgressObserved =
            _lastHomeStateAction != null &&
            _lastHomeStateAction != HomeStateAction.idle;
      }
    } else if (event.phase == LitchControlPhase.pending) {
      _boardUndoStatusMessage = null;
    }
    // Unrelated outcomes carry an unversioned action-state sample. They must
    // not overwrite Undo authority already received in a Home snapshot.
    if (nextActionState != LitchControlActionState.unknown &&
        (event.action == LitchControlAction.undo ||
            _lastHomeUndoPending == null)) {
      _controlActionState = nextActionState;
    }
    if (!authoritativeHomeSupersedes) {
      _lastControlEventWasCancelUndo = completedCancelUndo;
    }
    if (event.action == LitchControlAction.undo && event.isTerminal) {
      _undoRequestTracking = false;
      _undoRequestBaselineSequence = null;
    }
    final intent = _activeAction;
    if (intent != null &&
        intent.action == event.action &&
        intent.owner == event.owner &&
        (event.phase == LitchControlPhase.timeout ||
            event.phase == LitchControlPhase.sendFailed ||
            (event.phase == LitchControlPhase.interrupted &&
                _connectionActive &&
                _bleService.connectedId == _boardId) ||
            (event.phase == LitchControlPhase.rejected &&
                intent.busyRetries == 0 &&
                (event.reason == LitchMutationReason.busy ||
                    event.reason == LitchMutationReason.queueFull)))) {
      _bleService.recordSupportControlOutcome(
        intent.supportAction,
        event,
        recovering: true,
      );
      _beginRecovery(event);
      _notify();
      return;
    }
    if (intent != null &&
        intent.action == event.action &&
        intent.owner == event.owner) {
      _bleService.recordSupportControlOutcome(intent.supportAction, event);
    }
    if (event.action == LitchControlAction.hint && event.isTerminal) {
      if (_hintRequestTracking &&
          event.phase == LitchControlPhase.applied &&
          _assistance != null &&
          !_assistance!.coachingOwned &&
          _hintRequestedValue != null &&
          _assistance!.requested != _hintRequestedValue) {
        _hintSyncStatus = _ReadbackNotice.syncing;
        if (_lastHomeStateSequence case final int sequence) {
          _bleService.discardCachedHomeAssistance(
            boardId: _boardId,
            sequence: sequence,
          );
        }
        _hintSyncTimer?.cancel();
        _hintSyncTimer = Timer(hintSyncTimeout, () {
          _bleService.recordControlRecovery(confirmed: false);
          _clearHintSync();
          _assistance = null;
          _hintSyncStatus = _ReadbackNotice.unavailable;
          _notify();
        });
        _requestRecoverySnapshots();
      } else {
        _clearHintSync();
      }
      _hintRequestTracking = false;
    }
    if (event.action == LitchControlAction.coaching &&
        event.isTerminal &&
        event.owner == _coachingRequestOwner) {
      final LitchControlOwner? owner = _coachingRequestOwner;
      if (owner != null &&
          _coachingRequestedValue != null &&
          (event.phase == LitchControlPhase.applied ||
              event.phase == LitchControlPhase.noop) &&
          coachingEnabled(owner) != _coachingRequestedValue) {
        _coachingSyncStatus = _ReadbackNotice.syncing;
        _coachingSyncTimer?.cancel();
        _coachingSyncTimer = Timer(coachingSyncTimeout, () {
          _bleService.recordControlRecovery(confirmed: false);
          _clearCoachingSync();
          _coachingMask = null;
          _coachingSyncStatus = _ReadbackNotice.unavailable;
          _notify();
        });
        _requestRecoverySnapshots();
      } else {
        _clearCoachingSync();
      }
    }
    _notify();
  }

  void _clearCoachingSync() {
    _coachingSyncTimer?.cancel();
    _coachingSyncTimer = null;
    _coachingRequestOwner = null;
    _coachingRequestedValue = null;
    _coachingSyncStatus = null;
  }

  void _clearHintSync() {
    _hintSyncTimer?.cancel();
    _hintSyncTimer = null;
    _hintSyncStatus = null;
    _hintRequestedValue = null;
  }

  Future<void> _requestHomeSnapshot() async {
    _bleService.recordControlRecoveryRead();
    try {
      await _bleService.requestCurrentHomeStateSnapshot(_boardId);
    } on Exception {
      // The same bounded deadline handles failed or unsupported readback.
    }
  }

  void _requestRecoverySnapshots() {
    _snapshotRetryTimer?.cancel();
    _snapshotRetryTimer = Timer(const Duration(milliseconds: 1500), () {
      _snapshotRetryTimer = null;
      if (!_disposed && hasPendingAction) unawaited(_requestHomeSnapshot());
    });
    unawaited(_requestHomeSnapshot());
  }

  void _clearRecovery() {
    _recoveryTimer?.cancel();
    _recoveryTimer = null;
    _busyRetryTimer?.cancel();
    _busyRetryTimer = null;
    _recoveryEvent = null;
    _recoveringAction = null;
  }

  void _beginRecovery(LitchControlEvent event) {
    _clearRecovery();
    _clearHintSync();
    _clearCoachingSync();
    _recoveryEvent = event;
    _recoveringAction = _activeAction;
    _recoveryTimer = Timer(coachingSyncTimeout, () {
      _recoveryTimer = null;
      _busyRetryTimer?.cancel();
      _busyRetryTimer = null;
      _bleService.recordControlRecovery(confirmed: false);
      // Keep any newer observed authority. Never turn an uncertain write into
      // an Applied ACK, or resend a toggle because its acknowledgement was lost.
      _notify();
    });
    final retained = _bleService.latestHomeState;
    if (retained != null) _reconcileRecovery(retained);
    if (_recoveryTimer != null) _requestRecoverySnapshots();
  }

  void _reconcileRecovery(HomeStateNotification newest) {
    final intent = _recoveringAction;
    final event = _recoveryEvent;
    if (intent == null ||
        event == null ||
        !isNewerHomeStateSequence(newest.sequence, intent.baselineSequence)) {
      return;
    }
    final confirmed = switch (intent.action) {
      LitchControlAction.coaching =>
        newest.coachingMask != null &&
            (intent.desired == null ||
                ((newest.coachingMask! &
                            (intent.owner == LitchControlOwner.p1 ? 1 : 2)) !=
                        0) ==
                    intent.desired),
      // Home visibility cannot prove that a Show Move acknowledgement replayed.
      LitchControlAction.hint =>
        !intent.showMove &&
            newest.assistance != null &&
            (newest.assistance!.coachingOwned ||
                (intent.desired != null &&
                    newest.assistance!.requested == intent.desired)),
      LitchControlAction.undo => newest.undoPending == intent.desired,
      // The opening position alone cannot prove that New game executed.
      LitchControlAction.newGame => false,
    };
    if (confirmed) {
      _bleService.recordSupportActionOutcome(
        intent.supportAction,
        SupportActionOutcome.observed,
      );
      final reportsThisRecovery = identical(_lastControlEvent, event);
      _clearRecovery();
      if (reportsThisRecovery) _lastControlEvent = null;
      _bleService.recordControlRecovery(confirmed: true);
    } else if (event.phase == LitchControlPhase.rejected &&
        _recoveryTimer != null &&
        _busyRetryTimer == null) {
      // Only a correlated Busy/QueueFull rejection proves nonapplication.
      // Require fresh Home authority and revalidate the captured intent before
      // one retry; timeout/send failure can never enter this path.
      _busyRetryTimer = Timer(const Duration(milliseconds: 200), () {
        _busyRetryTimer = null;
        if (_disposed || _activeAction != intent || _recoveryTimer == null) {
          return;
        }
        if (!canDispatchAction(intent.action, owner: intent.owner) ||
            (intent.action != LitchControlAction.coaching &&
                intent.position != _position) ||
            !_matchesHintMode(intent)) {
          _clearRecovery();
          _lastControlEvent = null;
          _notify();
          return;
        }
        ++intent.busyRetries;
        _bleService.recordControlBusyRetry();
        _clearRecovery();
        _snapshotRetryTimer?.cancel();
        _snapshotRetryTimer = null;
        unawaited(_sendAction(intent));
      });
    }
  }

  Future<void> dispatch(
    LitchControlAction action, {
    LitchControlOwner? owner,
    bool? enabled,
  }) async {
    final String key = _controlKey(action, owner);
    if (!canDispatchAction(action, owner: owner)) return;
    final bool showMove =
        action == LitchControlAction.hint && _replaysHint(_assistance);
    final intent = _BoardActionIntent(
      action,
      owner,
      switch (action) {
        LitchControlAction.coaching => enabled,
        LitchControlAction.hint =>
          _assistance == null ? null : showMove || !_assistance!.requested,
        LitchControlAction.undo => !undoPending,
        LitchControlAction.newGame => null,
      },
      _position,
      showMove: showMove,
      hintCoachingOwned:
          action == LitchControlAction.hint &&
          _assistance?.coachingOwned == true,
    );
    if (_activeAction?.sameTarget(intent) == true) {
      final removed = _queuedActions.remove(key);
      if (removed != null) {
        _bleService.recordSupportActionOutcome(
          removed.supportAction,
          SupportActionOutcome.canceled,
        );
      }
      _bleService.recordControlIntent(coalesced: true);
      return;
    }
    if (_queuedActions[key]?.sameTarget(intent) == true) {
      _bleService.recordControlIntent(coalesced: true);
      return;
    }
    final replaced = _queuedActions[key];
    if (replaced != null) {
      _bleService.recordSupportActionOutcome(
        replaced.supportAction,
        SupportActionOutcome.canceled,
      );
    }
    intent.supportAction = _bleService.recordControlIntent(
      coalesced: false,
      action: action,
    );
    _queuedActions[key] = intent;
    _pumpActions();
  }

  bool _matchesHintMode(_BoardActionIntent intent) =>
      intent.action != LitchControlAction.hint ||
      (intent.showMove == _replaysHint(_assistance) &&
          intent.hintCoachingOwned == (_assistance?.coachingOwned == true));

  void _pumpActions() {
    if (_disposed || _activeAction != null || hasPendingAction) return;
    while (_queuedActions.isNotEmpty) {
      final key = _queuedActions.keys.first;
      final intent = _queuedActions.remove(key)!;
      if (!canDispatchAction(intent.action, owner: intent.owner) ||
          !_matchesHintMode(intent)) {
        _bleService.recordSupportActionOutcome(
          intent.supportAction,
          SupportActionOutcome.canceled,
        );
        continue;
      }
      // Position-bound actions never spill into a different game/turn while
      // waiting behind another control. Coaching remains a per-player setting.
      if (intent.action != LitchControlAction.coaching &&
          intent.position != _position) {
        _bleService.recordSupportActionOutcome(
          intent.supportAction,
          SupportActionOutcome.canceled,
        );
        continue;
      }
      final bool? current = switch (intent.action) {
        LitchControlAction.coaching => coachingEnabled(intent.owner!),
        LitchControlAction.hint => _assistance?.requested,
        LitchControlAction.undo => undoPending,
        LitchControlAction.newGame => null,
      };
      // Show Move replays the board acknowledgement even when already shown.
      if (!intent.showMove &&
          intent.desired != null &&
          current == intent.desired) {
        _bleService.recordSupportActionOutcome(
          intent.supportAction,
          SupportActionOutcome.noChange,
        );
        continue;
      }
      _activeAction = intent;
      unawaited(_sendAction(intent));
      return;
    }
  }

  Future<void> _sendAction(_BoardActionIntent intent) async {
    final action = intent.action;
    final owner = intent.owner;
    final key = _controlKey(action, owner);
    final generation = _actionGeneration;
    intent.baselineSequence = _lastHomeStateSequence;

    _startingControlActions.add(key);
    _clearRecovery();
    _coachingSyncStatus = null;
    _hintSyncStatus = null;
    if (action == LitchControlAction.coaching) {
      _coachingRequestOwner = owner;
      _coachingRequestedValue = intent.desired;
    }
    _notify();
    try {
      await _bleService.dispatchControlAction(
        deviceId: targetId,
        action: action,
        owner: owner,
        enabled: action == LitchControlAction.coaching ? intent.desired : null,
      );
    } on Exception {
      if (_disposed || generation != _actionGeneration) return;
      _lastControlEvent = LitchControlEvent(
        action: action,
        owner: owner,
        phase: LitchControlPhase.sendFailed,
      );
      _bleService.recordSupportControlOutcome(
        intent.supportAction,
        _lastControlEvent!,
      );
    } finally {
      if (generation == _actionGeneration &&
          _startingControlActions.remove(key)) {
        _notify();
      }
    }
  }

  String _controlActionName(LitchControlEvent event) {
    return switch (event.action) {
      LitchControlAction.hint =>
        _replaysHint(_assistance)
            ? _strings.showMoveAction
            : _strings.hintAction,
      LitchControlAction.undo =>
        (_cancelUndoRequestIds.contains(event.requestId) ||
                (identical(event, _lastControlEvent) &&
                    _lastControlEventWasCancelUndo))
            ? _strings.cancelUndo
            : _strings.undoAction,
      LitchControlAction.newGame => _strings.newGame,
      LitchControlAction.coaching => _strings.playerCoaching(
        _strings.playerSpoken(event.owner == LitchControlOwner.p2 ? '2' : '1'),
      ),
    };
  }

  String _controlReasonMessage(LitchControlEvent event) {
    return switch (event.reason) {
      LitchMutationReason.noHistory => _strings.noUndoMove,
      LitchMutationReason.busy ||
      LitchMutationReason.queueFull => _strings.actionBusy,
      LitchMutationReason.notAtStart => _strings.actionBeforePlay,
      LitchMutationReason.unsupported ||
      LitchMutationReason.unknownAction => _strings.controlUnavailable,
      LitchMutationReason.gameOver => _strings.controlNewGame,
      LitchMutationReason.engineRejected => _strings.engineActionFailed,
      LitchMutationReason.storageUnavailable => _strings.actionSaveFailed,
      LitchMutationReason.archiveRequired => _strings.actionArchiveRequired,
      LitchMutationReason.badRequest ||
      LitchMutationReason.unknownKey ||
      LitchMutationReason.typeMismatch ||
      LitchMutationReason.outOfRange ||
      LitchMutationReason.invalidEnumValue => _strings.actionApplyFailed,
      LitchMutationReason.none || null => _strings.actionNoChange,
    };
  }

  String? _controlStatusMessage(LitchControlEvent event) {
    if (event.action == LitchControlAction.coaching &&
        (event.phase == LitchControlPhase.pending ||
            event.phase == LitchControlPhase.accepted ||
            event.phase == LitchControlPhase.applied ||
            event.phase == LitchControlPhase.noop)) {
      return null;
    }
    final String actionName = _controlActionName(event);
    return switch (event.phase) {
      LitchControlPhase.pending => _strings.actionRequested(actionName),
      LitchControlPhase.accepted => _strings.actionAccepted(actionName),
      LitchControlPhase.applied => switch (event.action) {
        LitchControlAction.hint =>
          _hintPresentationMessage ??
              (_assistance?.presentation == HomeAssistancePresentation.off
                  ? _strings.hintHidden
                  : _strings.hintFollowLights),
        LitchControlAction.undo =>
          _undoCompletedFromBoard
              ? _strings.undoComplete
              : _lastControlEventWasCancelUndo
              ? _strings.undoCancelled
              : _strings.undoReady,
        LitchControlAction.newGame => _strings.newGameStarted,
        LitchControlAction.coaching => null,
      },
      LitchControlPhase.noop ||
      LitchControlPhase.rejected => _controlReasonMessage(event),
      LitchControlPhase.timeout => _strings.actionUnconfirmed(actionName),
      LitchControlPhase.sendFailed => _strings.actionSendFailed(actionName),
      LitchControlPhase.unavailable => _strings.actionUnavailable(actionName),
      LitchControlPhase.interrupted => _strings.actionInterrupted(actionName),
    };
  }

  @override
  void dispose() {
    _disposed = true;
    ++_actionGeneration;
    for (final intent in _queuedActions.values) {
      _bleService.recordSupportActionOutcome(
        intent.supportAction,
        SupportActionOutcome.canceled,
      );
    }
    if (_activeAction case final active?) {
      _bleService.recordSupportActionOutcome(
        active.supportAction,
        SupportActionOutcome.unconfirmed,
      );
    }
    _queuedActions.clear();
    _activeAction = null;
    _clearRecovery();
    _snapshotRetryTimer?.cancel();
    ++_playerSettingsGeneration;
    _clearCoachingSync();
    _clearHintSync();
    unawaited(_controlActionSub?.cancel());
    unawaited(_connectionSub?.cancel());
    unawaited(_homeStateSub?.cancel());
    unawaited(_playerSettingsSub?.cancel());
    super.dispose();
  }
}
