import '../models/board_health_status.dart';
import '../models/runtime_health_snapshot.dart';
import 'dart:async';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../models/home_board_snapshot.dart';
import '../models/home_state_notification.dart';
import '../models/litch_control_action.dart';
import '../models/litch_end_palette.dart';
import 'litch_ble_service.dart';

class HomeBoardFeed {
  HomeBoardFeed({
    required LitchBleService bleService,
    required String boardId,
    this.refreshTimeout = const Duration(seconds: 3),
    this.automaticRetryDelay = const Duration(milliseconds: 350),
  }) : _bleService = bleService,
       _boardId = boardId {
    _connectionSessionToken = _bleService.connectionSessionToken;
    _connectionActive = _canUseBoard;
    _cpuConfiguration = _readCpuConfiguration();
    _consoleSubscription = _bleService.consoleStream.listen(
      _handleConsoleNotices,
    );
    _homeStateSubscription = _bleService.homeStateStream.listen(
      _handleHomeState,
    );
    _settingsSubscription = _bleService.settingsChangedStream.listen(
      _handleSettingChanged,
    );
    _connectionSubscription = _bleService.connectionStream.listen(
      _handleConnection,
    );
    _readinessSubscription = _bleService.productReadinessStream.listen((ready) {
      if (!_disposed && _bleService.connectedId == _boardId) {
        _setConnectionActive(ready && _canUseBoard);
      }
    });
    _controlActionSubscription = _bleService.controlActionEvents.listen(
      _handleControlAction,
    );
    _healthSubscription = _bleService.boardHealthStream.listen(
      _handleBoardHealth,
    );
    if (_connectionActive) {
      final BoardHealthStatus? retainedHealth = _bleService.latestBoardHealth;
      if (retainedHealth != null) _handleBoardHealth(retainedHealth);
      unawaited(_refreshHealth());
      _primePlayerEnd();
      _primePlayerColors();
      _hydrateRetainedHome();
    } else {
      _current = _current.copyWith(isRefreshing: true);
    }
    // Retained Console lines are diagnostics, not current board authority.
    _previousConsoleLines = _bleService.consoleLinesSnapshot;
  }

  static const String _gameFirstPlayerKey = 'GAME_FIRST_PLAYER';
  static const String _boardRotateKey = 'BOARD_ROTATE';
  static const String _p1PieceWheelKey = 'P1_PIECE_WHEEL';
  static const String _p2PieceWheelKey = 'P2_PIECE_WHEEL';
  static const String _p1EndPieceWheelKey = 'P1_END_PIECE_WHEEL';
  static const String _p2EndPieceWheelKey = 'P2_END_PIECE_WHEEL';
  static const String _cpuLevelKey = 'VS_CPU_LEVEL';
  static const String _cpuPlayerKey = 'VS_CPU_PLAYER';
  static const Duration _cpuEstimateStaleAfter = Duration(seconds: 3);
  final LitchBleService _bleService;
  final String _boardId;
  final Duration refreshTimeout;
  final Duration automaticRetryDelay;
  final StreamController<HomeBoardSnapshot> _stateController =
      StreamController<HomeBoardSnapshot>.broadcast();
  final StreamController<BoardNotice> _noticeController =
      StreamController<BoardNotice>.broadcast();

  StreamSubscription<List<String>>? _consoleSubscription;
  StreamSubscription<HomeStateNotification>? _homeStateSubscription;
  StreamSubscription<String>? _settingsSubscription;
  StreamSubscription<ConnectionStateUpdate>? _connectionSubscription;
  StreamSubscription<bool>? _readinessSubscription;
  StreamSubscription<LitchControlEvent>? _controlActionSubscription;
  StreamSubscription<BoardHealthStatus>? _healthSubscription;
  Timer? _watchdogNoticeTimer;
  Timer? _cpuEstimateTimer;
  late (int?, int?) _cpuConfiguration;
  int _healthGeneration = 0;
  int? _shownWatchdogBoot;
  Future<void> _refreshHealth() async {
    final int generation = ++_healthGeneration;
    RuntimeHealthResult result;
    try {
      result = await _bleService.fetchRuntimeHealth(deviceId: _boardId);
    } catch (_) {
      result = const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.sendFailed,
      );
    }
    if (!_disposed &&
        _connectionActive &&
        generation == _healthGeneration &&
        _bleService.latestBoardHealth == null) {
      _emit(_current.copyWith(runtimeHealth: result));
    }
  }

  void _handleBoardHealth(BoardHealthStatus status) {
    if (_disposed || !_connectionActive) return;
    ++_healthGeneration;
    final bool announceRestart =
        status.watchdogRestart && _shownWatchdogBoot != status.bootCount;
    if (announceRestart) {
      _shownWatchdogBoot = status.bootCount;
      _watchdogNoticeTimer?.cancel();
      _watchdogNoticeTimer = Timer(const Duration(seconds: 12), () {
        if (!_disposed) _emit(_current.copyWith(watchdogRestart: false));
      });
    }
    _emit(
      _current.copyWith(
        runtimeHealth: RuntimeHealthAvailable(status.runtimeHealth),
        persistenceSessionOnly: status.sessionOnly,
        persistenceDegradation: status.persistence,
        clearPersistenceDegradation: status.persistence == null,
        watchdogRestart: announceRestart || _current.watchdogRestart,
      ),
    );
  }

  Timer? _refreshTimer;
  Timer? _clockRetryTimer;
  Completer<void>? _clockRetryDelay;
  List<String> _previousConsoleLines = const <String>[];
  HomeBoardSnapshot _current = const HomeBoardSnapshot(
    playerColorsConfirmed: false,
  );
  bool _hasCoherentHomeSnapshot = false;
  int _refreshGeneration = 0;
  int _automaticRetriesSent = 0;
  int? _lastHomeStateSequence;
  late int _connectionSessionToken;
  bool _connectionActive = false;
  bool _authoritativeSnapshotRetrySent = false;
  bool? _p1AtOppositeEnd;
  HomeStateNotification? _pendingLivePlayerEnd;
  int _playerEndReadGeneration = 0;
  int _paletteReadGeneration = 0;
  bool _disposed = false;

  HomeBoardSnapshot get current => _current;

  Stream<HomeBoardSnapshot> get states => _stateController.stream;

  Stream<BoardNotice> get notices => _noticeController.stream;

  Future<void> refresh() => _beginRefresh(replaceInFlight: false);

  Future<void> reconcileClockAfterStartRejection() async {
    if (_disposed || !_connectionActive) {
      return;
    }

    // A start-only write can race the notification that changes the clock
    // from pregame to in-game. Invalidate only that stale projection and ask
    // firmware for the authoritative live clock; retaining the old value would
    // immediately re-enable a control the board has already rejected.
    final int generation = ++_refreshGeneration;
    _cancelClockRetryDelay();
    _refreshTimer?.cancel();
    _refreshTimer = null;
    _authoritativeSnapshotRetrySent = true;
    _emit(
      _current.copyWith(
        isRefreshing: false,
        isClockRefreshing: true,
        clearClockStatus: true,
        clearRefreshMessage: true,
      ),
    );
    await _recoverAuthoritativeClock(generation);
  }

  bool get hasAuthoritativeLiveState => _lastHomeStateSequence != null;
  bool get hasCurrentBoardState =>
      _connectionActive && _hasCoherentHomeSnapshot;

  Future<void> _beginRefresh({required bool replaceInFlight}) async {
    if (_disposed ||
        !_connectionActive ||
        (_current.isRefreshing && !replaceInFlight)) {
      return;
    }

    final int generation = ++_refreshGeneration;
    _cancelClockRetryDelay();
    _refreshTimer?.cancel();
    _automaticRetriesSent = 0;
    _authoritativeSnapshotRetrySent = false;
    _emit(_current.copyWith(isRefreshing: true, clearRefreshMessage: true));

    _scheduleRefreshTimeout(
      generation,
      _lastHomeStateSequence == null ? refreshTimeout : automaticRetryDelay,
    );
    final bool requested = await _requestHomeSnapshot(generation);
    if (_isRefreshActive(generation) && !requested) {
      _scheduleRefreshTimeout(generation, automaticRetryDelay);
    }
  }

  Future<bool> _requestHomeSnapshot(int generation) async {
    if (!_isRefreshActive(generation)) {
      return false;
    }

    final String targetId = _bleService.connectedId ?? _boardId;
    if (!_bleService.productReady &&
        !await _bleService.ensureProductReady(targetId)) {
      return false;
    }
    if (!_isRefreshActive(generation) || _bleService.connectedId != targetId) {
      return false;
    }
    return _bleService.requestCurrentHomeStateSnapshot(targetId);
  }

  bool _isRefreshActive(int generation) {
    return !_disposed &&
        _connectionActive &&
        generation == _refreshGeneration &&
        _current.isRefreshing;
  }

  void _scheduleRefreshTimeout(int generation, Duration delay) {
    _refreshTimer?.cancel();
    _refreshTimer = Timer(delay, () => _finishRefreshAfterTimeout(generation));
  }

  Future<void> _runAutomaticRetry(int generation) async {
    await _requestHomeSnapshot(generation);
    if (_isRefreshActive(generation)) {
      _scheduleRefreshTimeout(generation, automaticRetryDelay);
    }
  }

  void _primePlayerColors() {
    final int generation = ++_paletteReadGeneration;
    final Object? p1 =
        _bleService.latestSettingValue(_p1EndPieceWheelKey) ??
        _bleService.latestSettingValue(_p1PieceWheelKey);
    final Object? p2 =
        _bleService.latestSettingValue(_p2EndPieceWheelKey) ??
        _bleService.latestSettingValue(_p2PieceWheelKey);
    if (p1 is int &&
        p2 is int &&
        p1 >= 0 &&
        p1 <= 255 &&
        p2 >= 0 &&
        p2 <= 255) {
      _emit(
        _current.copyWith(
          p1PieceWheel: p1,
          p2PieceWheel: p2,
          playerColorsConfirmed: true,
        ),
      );
      return;
    }
    unawaited(_readPhysicalEndPalette(generation));
  }

  void _primePlayerEnd() {
    final bool? cached = _playerEnd(
      _bleService.latestSettingValue(_gameFirstPlayerKey),
    );
    if (cached != null) {
      ++_playerEndReadGeneration;
      _applyPlayerEnd(cached);
      return;
    }
    _startPlayerEndRead();
  }

  void _startPlayerEndRead() {
    final int generation = ++_playerEndReadGeneration;
    unawaited(_readPlayerEnd(generation));
  }

  Future<void> _readPlayerEnd(int generation) async {
    final String targetId = _bleService.connectedId ?? _boardId;
    Object? value;
    try {
      value = await _bleService.readSettingValue(
        deviceId: targetId,
        key: _gameFirstPlayerKey,
      );
    } catch (_) {
      // Keep orientation unresolved until the typed setting arrives. A
      // transient read failure must not pin the live board to the wrong end.
    }
    if (_disposed || generation != _playerEndReadGeneration) {
      return;
    }
    final bool? opposite = _playerEnd(value);
    if (opposite != null) {
      _applyPlayerEnd(opposite);
    }
  }

  void _applyPlayerEnd(bool opposite) {
    if (_disposed) {
      return;
    }
    _p1AtOppositeEnd = opposite;
    final HomeStateNotification? pending = _pendingLivePlayerEnd;
    if (pending != null) {
      _pendingLivePlayerEnd = null;
      _applyLiveSnapshot(pending, opposite);
      return;
    }
    final HomePhysicalBoardStatus? physical = _current.physicalStatus;
    if (physical == null || physical.p1AtOppositeEnd == opposite) {
      return;
    }
    _emit(
      _current.copyWith(
        physicalStatus: physical.copyWith(p1AtOppositeEnd: opposite),
      ),
    );
  }

  void _handleSettingChanged(String key) {
    if (_disposed || !_connectionActive) return;
    if (key == _boardRotateKey) {
      final raw = _bleService.latestSettingValue(_boardRotateKey);
      final rotated = switch (raw) {
        true || 1 || '1' => true,
        false || 0 || '0' => false,
        _ => null,
      };
      final physical = _current.physicalStatus;
      if (rotated != null && physical != null && physical.rotated != rotated) {
        _emit(
          _current.copyWith(
            physicalStatus: physical.copyWith(rotated: rotated),
          ),
        );
      }
      return;
    }
    if (key == _cpuLevelKey || key == _cpuPlayerKey) {
      final (int?, int?) configuration = _readCpuConfiguration();
      final bool changed =
          (_cpuConfiguration.$1 != null &&
              configuration.$1 != null &&
              _cpuConfiguration.$1 != configuration.$1) ||
          (_cpuConfiguration.$2 != null &&
              configuration.$2 != null &&
              _cpuConfiguration.$2 != configuration.$2);
      _cpuConfiguration = configuration;
      if (changed) {
        _retireCpuThinking();
      }
      return;
    }
    if (key == _gameFirstPlayerKey) {
      _primePlayerEnd();
      _primePlayerColors();
      return;
    }
    if (key == _p1PieceWheelKey ||
        key == _p2PieceWheelKey ||
        key == _p1EndPieceWheelKey ||
        key == _p2EndPieceWheelKey) {
      _primePlayerColors();
    }
  }

  (int?, int?) _readCpuConfiguration() => (
    int.tryParse(
      _bleService.latestSettingValue(_cpuLevelKey)?.toString() ?? '',
    ),
    int.tryParse(
      _bleService.latestSettingValue(_cpuPlayerKey)?.toString() ?? '',
    ),
  );

  void _retireCpuThinking() {
    _cpuEstimateTimer?.cancel();
    _cpuEstimateTimer = null;
    _pendingLivePlayerEnd = _pendingLivePlayerEnd?.withCpuThinking(null);
    if (_current.cpuThinking != null) {
      _emit(_current.copyWith(clearCpuThinking: true));
    }
  }

  void _handleConnection(ConnectionStateUpdate update) {
    if (_disposed ||
        (update.deviceId.isNotEmpty && update.deviceId != _boardId)) {
      return;
    }

    if (update.connectionState == DeviceConnectionState.disconnected) {
      _setConnectionActive(false);
    } else if (update.connectionState == DeviceConnectionState.connected) {
      _setConnectionActive(_canUseBoard);
    }
  }

  bool get _canUseBoard =>
      _bleService.isConnected &&
      _bleService.deviceCapabilities(_boardId).supportsConnectionSession &&
      !_bleService.deviceCapabilities(_boardId).recoveryOnly &&
      _bleService.productReady;

  void _setConnectionActive(bool active) {
    if (!active) {
      _connectionActive = false;
      ++_refreshGeneration;
      _cancelClockRetryDelay();
      _lastHomeStateSequence = null;
      ++_playerEndReadGeneration;
      ++_paletteReadGeneration;
      _p1AtOppositeEnd = null;
      _pendingLivePlayerEnd = null;
      _hasCoherentHomeSnapshot = false;
      _authoritativeSnapshotRetrySent = false;
      _previousConsoleLines = const <String>[];
      _refreshTimer?.cancel();
      _refreshTimer = null;
      ++_healthGeneration;
      _watchdogNoticeTimer?.cancel();
      _cpuEstimateTimer?.cancel();
      _cpuEstimateTimer = null;
      _emit(
        _current.copyWith(
          isRefreshing: false,
          clearLiveState: true,
          clearRefreshMessage: true,
        ),
      );
      return;
    }
    if (active) {
      final int sessionToken = _bleService.connectionSessionToken;
      if (_connectionActive && sessionToken == _connectionSessionToken) {
        return;
      }
      _connectionSessionToken = sessionToken;
      _cpuEstimateTimer?.cancel();
      _cpuEstimateTimer = null;
      _cpuConfiguration = _readCpuConfiguration();
      _connectionActive = true;
      _lastHomeStateSequence = null;
      _hasCoherentHomeSnapshot = false;
      _authoritativeSnapshotRetrySent = false;
      // Console history survives for diagnostics. Fence retained notices at
      // the connection boundary so they are not announced again.
      _previousConsoleLines = _bleService.consoleLinesSnapshot;
      _emit(
        _current.copyWith(
          clearLiveState: true,
          clearRefreshMessage: true,
          playerColorsConfirmed: false,
        ),
      );
      unawaited(_refreshHealth());
      _primePlayerEnd();
      _primePlayerColors();
      // The first Home can arrive before readiness. An unchanged replay is
      // deduplicated, so consume the current link's retained Home here too.
      if (_bleService.connectedId == _boardId) _hydrateRetainedHome();
      unawaited(_beginRefresh(replaceInFlight: true));
    }
  }

  void _hydrateRetainedHome() {
    final home = _bleService.latestHomeState;
    if (home != null) _handleHomeState(home, retained: true);
  }

  void _handleControlAction(LitchControlEvent event) {
    if (_disposed ||
        event.action != LitchControlAction.newGame ||
        !event.isTerminal ||
        !event.isSuccess) {
      return;
    }

    // The live home-state notification remains authoritative and normally
    // arrives first. This bounded readback covers an app-originated New Game
    // whose notification was delayed or missed while screens were changing.
    _retireCpuThinking();
    unawaited(refresh());
  }

  Future<void> _readPhysicalEndPalette(int generation) async {
    final String targetId = _bleService.connectedId ?? _boardId;
    LitchEndPalette? palette;
    try {
      palette = await _bleService.readPhysicalEndPalette(deviceId: targetId);
    } catch (_) {
      return;
    }
    if (_disposed || generation != _paletteReadGeneration || palette == null) {
      return;
    }
    final int p1 = palette.p1ControlsWheel;
    final int p2 = palette.p2ControlsWheel;
    if (_current.playerColorsConfirmed &&
        p1 == _current.p1PieceWheel &&
        p2 == _current.p2PieceWheel) {
      return;
    }
    _emit(
      _current.copyWith(
        p1PieceWheel: p1,
        p2PieceWheel: p2,
        playerColorsConfirmed: true,
      ),
    );
  }

  void _handleConsoleNotices(List<String> lines) {
    if (_disposed || !_connectionActive) {
      return;
    }

    final List<String> newLines = _findNewLines(_previousConsoleLines, lines);
    _previousConsoleLines = List<String>.from(lines);
    for (final String line in newLines) {
      final BoardNotice? notice = BoardNotice.tryParse(line);
      if (notice != null) {
        _noticeController.add(notice);
      }
    }
  }

  Future<void> _recoverAuthoritativeClock(int generation) async {
    final String targetId = _bleService.connectedId ?? _boardId;
    for (int attempt = 0; attempt < 2; attempt += 1) {
      if (!_isClockRecoveryActive(generation)) {
        return;
      }
      await _bleService.requestCurrentHomeStateSnapshot(targetId);
      if (!_isClockRecoveryActive(generation)) {
        return;
      }
      if (attempt == 0) {
        await _waitForClockRetryDelay();
      }
    }

    if (!_isClockRecoveryActive(generation)) {
      return;
    }
    _emit(_current.copyWith(isClockRefreshing: false));
  }

  bool _isClockRecoveryActive(int generation) {
    return !_disposed &&
        _connectionActive &&
        generation == _refreshGeneration &&
        _current.isClockRefreshing &&
        _current.clockStatus == null;
  }

  Future<void> _waitForClockRetryDelay() {
    _cancelClockRetryDelay();
    final Completer<void> completer = Completer<void>();
    _clockRetryDelay = completer;
    _clockRetryTimer = Timer(automaticRetryDelay, () {
      if (!identical(_clockRetryDelay, completer)) {
        return;
      }
      _clockRetryTimer = null;
      _clockRetryDelay = null;
      completer.complete();
    });
    return completer.future;
  }

  void _cancelClockRetryDelay() {
    _clockRetryTimer?.cancel();
    _clockRetryTimer = null;
    final Completer<void>? completer = _clockRetryDelay;
    _clockRetryDelay = null;
    if (completer != null && !completer.isCompleted) {
      completer.complete();
    }
  }

  void _handleHomeState(HomeStateNotification live, {bool retained = false}) {
    if (_disposed || !_connectionActive) {
      return;
    }
    if (!isNewerHomeStateSequence(live.sequence, _lastHomeStateSequence)) {
      return;
    }
    // The service retains authority across screen changes, but does not retain
    // an arrival age. A remount must not restart an old estimate's lifetime.
    if (retained && live.cpuThinking != null) {
      live = live.withCpuThinking(live.cpuThinking!.withoutEstimate());
    }

    final bool? opposite = _p1AtOppositeEnd;
    if (opposite == null) {
      final HomeStateNotification? pending = _pendingLivePlayerEnd;
      if (pending == null ||
          isNewerHomeStateSequence(live.sequence, pending.sequence)) {
        // The orientation read may finish after this estimate is stale. Keep
        // the search authority, but wait for a fresh Home estimate afterward.
        _pendingLivePlayerEnd = live.withCpuThinking(
          live.cpuThinking?.withoutEstimate(),
        );
      }
      return;
    }
    _applyLiveSnapshot(live, opposite);
  }

  void _applyLiveSnapshot(HomeStateNotification live, bool p1AtOppositeEnd) {
    if (!isNewerHomeStateSequence(live.sequence, _lastHomeStateSequence)) {
      return;
    }

    _lastHomeStateSequence = live.sequence;
    _hasCoherentHomeSnapshot = true;
    _refreshTimer?.cancel();
    _refreshTimer = null;
    final bool startClockRecovery =
        live.clockStatus == null &&
        !_authoritativeSnapshotRetrySent &&
        !_disposed &&
        _connectionActive;
    if (startClockRecovery) {
      _authoritativeSnapshotRetrySent = true;
    } else if (live.clockStatus != null) {
      _cancelClockRetryDelay();
    }
    final bool clockRecoveryInProgress =
        live.clockStatus == null &&
        (_current.isClockRefreshing || startClockRecovery);
    _emit(
      _current.copyWith(
        position: live.position,
        gameStatus: live.gameStatus,
        physicalStatus: live.physicalStatus.copyWith(
          p1AtOppositeEnd: p1AtOppositeEnd,
        ),
        isRefreshing: false,
        isClockRefreshing: clockRecoveryInProgress,
        refreshedAt: DateTime.now(),
        clearRefreshMessage: true,
        liveSequence: live.sequence,
        homeStateAction: live.action,
        freshSetup: live.freshSetup,
        vexMoveFirst: live.vexMoveFirst,
        kingQueenSwapPromptActive: live.kingQueenSwapPromptActive,
        cpuThinking: live.cpuThinking,
        clearCpuThinking: live.cpuThinking == null,
        physicalMovePrompt: live.physicalMovePrompt,
        clearPhysicalMovePrompt: live.physicalMovePrompt == null,
        duckRelocationPrompt: live.duckRelocationPrompt,
        clearDuckRelocationPrompt: live.duckRelocationPrompt == null,
        duckLandingRemovalMask: live.duckLandingRemovalMask,
        clearDuckLandingRemovalMask: live.duckLandingRemovalMask == null,
        assistance: live.assistance,
        clearAssistance: live.assistance == null,
        lastMoves: live.lastMoves,
        clearLastMoves: live.lastMoves == null,
        promotionConfirmation: live.promotionConfirmation,
        clearPromotionConfirmation: live.promotionConfirmation == null,
        clockStatus: live.clockStatus,
        clearClockStatus: live.clockStatus == null,
        persistenceDegradation: live.persistenceDegradation,
        clearPersistenceDegradation: live.persistenceDegradation == null,
      ),
    );
    _cpuEstimateTimer?.cancel();
    _cpuEstimateTimer = null;
    final HomeCpuThinking? thinking = live.cpuThinking;
    if (thinking?.remainingSeconds case final int remaining) {
      final Duration lifetime = remaining < _cpuEstimateStaleAfter.inSeconds
          ? Duration(seconds: remaining)
          : _cpuEstimateStaleAfter;
      _cpuEstimateTimer = Timer(lifetime, () {
        _cpuEstimateTimer = null;
        if (!_disposed &&
            _connectionActive &&
            identical(_current.cpuThinking, thinking)) {
          _emit(_current.copyWith(cpuThinking: thinking!.withoutEstimate()));
        }
      });
    }
    if (startClockRecovery) {
      unawaited(_recoverAuthoritativeClock(_refreshGeneration));
    }
  }

  void _finishRefreshAfterTimeout(int generation) {
    if (!_isRefreshActive(generation)) {
      return;
    }

    if (_automaticRetriesSent < 2) {
      ++_automaticRetriesSent;
      _refreshTimer = null;
      unawaited(_runAutomaticRetry(generation));
      return;
    }

    if (_hasCoherentHomeSnapshot) {
      // A missed readback cannot invalidate the prior typed snapshot.
      _finishRefresh(null, markRefreshed: false);
      return;
    }
    _finishRefresh(
      'Board details could not be read. Pull down to try again.',
      markRefreshed: false,
    );
  }

  void _finishRefresh(String? message, {bool markRefreshed = true}) {
    _refreshTimer?.cancel();
    _refreshTimer = null;
    _emit(
      _current.copyWith(
        isRefreshing: false,
        refreshedAt: markRefreshed ? DateTime.now() : null,
        refreshMessage: message,
        clearRefreshMessage: message == null,
      ),
    );
  }

  void _emit(HomeBoardSnapshot next) {
    if (_disposed) {
      return;
    }
    _current = next;
    _stateController.add(next);
  }

  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _cpuEstimateTimer?.cancel();
    _watchdogNoticeTimer?.cancel();
    ++_healthGeneration;
    ++_refreshGeneration;
    ++_playerEndReadGeneration;
    ++_paletteReadGeneration;
    _pendingLivePlayerEnd = null;
    _cancelClockRetryDelay();
    _refreshTimer?.cancel();
    await _healthSubscription?.cancel();
    await _consoleSubscription?.cancel();
    await _homeStateSubscription?.cancel();
    await _settingsSubscription?.cancel();
    await _connectionSubscription?.cancel();
    await _readinessSubscription?.cancel();
    await _controlActionSubscription?.cancel();
    await _stateController.close();
    await _noticeController.close();
  }
}

bool? _playerEnd(Object? value) {
  final int? parsed = switch (value) {
    int setting => setting,
    String text => int.tryParse(text),
    _ => null,
  };
  return switch (parsed) {
    1 => false,
    2 => true,
    _ => null,
  };
}

List<String> _findNewLines(List<String> previous, List<String> current) {
  if (previous.isEmpty) {
    return List<String>.from(current);
  }

  final int maxOverlap = previous.length < current.length
      ? previous.length
      : current.length;
  for (int overlap = maxOverlap; overlap > 0; overlap -= 1) {
    bool matches = true;
    for (int index = 0; index < overlap; index += 1) {
      if (previous[previous.length - overlap + index] != current[index]) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return current.sublist(overlap);
    }
  }
  return List<String>.from(current);
}
