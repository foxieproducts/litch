import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/widgets.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../models/current_game_line_state.dart';
import '../models/game_archive.dart';
import '../models/local_player_profile.dart';
import 'local_player_profile_store.dart';
import '../models/litch_control_action.dart';
import 'app_trace_recorder.dart';
import 'current_game_pull_client.dart';
import 'current_game_receive_store.dart';
import 'game_history_pull_client.dart';
import 'game_history_segment_importer.dart';
import 'game_library_repository.dart';
import 'litch_ble_service.dart';

/// App-lifetime activation owner for the board's current game line.
///
/// A connection or changed durable current-line publication requests an
/// idempotent pull. A newer request coalesces behind the active transfer, while
/// disconnect, background, and teardown cancel the connection-scoped client.
/// Once a valid line publication proves support for this connection, Home is
/// not a capture trigger: it may precede the durable line. Until then, retain
/// the deployed terminal/refusal best-effort triggers.
class PhysicalGameHistoryIngestion with WidgetsBindingObserver {
  PhysicalGameHistoryIngestion({
    required this.bleService,
    required GameLibraryRepository repository,
    required Directory receiveDirectory,
    this.playerProfiles,
    AppTraceSink? appTrace,
    DateTime Function()? clock,
  }) : _repository = repository,
       _receiveStore = CurrentGameReceiveStore(
         directory: receiveDirectory,
         importer: GameHistorySegmentImporter(
           repository,
           loadPlayers: playerProfiles == null
               ? null
               : () async => (await playerProfiles.load()).directory,
         ),
       ),
       _appTrace = appTrace,
       _clock = clock ?? DateTime.now;

  final LitchBleService bleService;
  final LocalPlayerProfileStore? playerProfiles;
  final GameLibraryRepository _repository;
  final CurrentGameReceiveStore _receiveStore;
  final AppTraceSink? _appTrace;
  final DateTime Function() _clock;
  final GameHistoryTransactionOwner _transactionOwner =
      GameHistoryTransactionOwner();

  StreamSubscription<ConnectionStateUpdate>? _connectionSubscription;
  StreamSubscription<bool>? _readinessSubscription;
  StreamSubscription<CurrentGameLineState>? _lineSubscription;
  StreamSubscription<Object>? _homeSubscription;
  StreamSubscription<LitchControlEvent>? _controlSubscription;
  CurrentGamePullClient? _activeClient;
  Future<void> _drainFuture = Future<void>.value();
  String? _deviceId;
  int _sessionToken = -1;
  int _nextTransaction = 0;
  bool _foreground = true;
  bool _durableNotificationsSupported = false;
  bool _pullRequested = false;
  bool _drainRunning = false;
  bool _assignmentReadRunning = false;
  bool _started = false;
  bool _closed = false;
  int _authorityFence = 0;
  int _completedPulls = 0;
  CurrentGameSyncResult? _lastPull;
  String? _activeBoardIdentity;
  final Map<String, String> _retiredBoardGames = <String, String>{};
  ({bool Function() cancelled, void Function(CurrentGamePullClient) bind})?
  _nextAssignmentRead;

  /// Home assigns the current and future slot only after a fresh board pull.
  /// The existing transfer owner excludes imports during the two-store update.
  Future<LocalPlayerDirectory> assignPlayer({
    required String boardId,
    required LocalPlayerChessSide side,
    String? displayName,
    Future<void>? cancellation,
    bool Function()? beginCommit,
  }) async {
    if (displayName != null) normalizeLocalPlayerName(displayName);
    final store = playerProfiles;
    final token = _sessionToken;
    if (store == null ||
        boardId != _deviceId ||
        !_ownsSession(boardId, token)) {
      throw StateError('Reconnect to this board before assigning a player.');
    }
    bool cancelled = false, committing = false, done = false;
    final cancelledSignal = Completer<void>();
    CurrentGamePullClient? assignmentClient;
    ({bool Function() cancelled, void Function(CurrentGamePullClient) bind})?
    readScope;
    void cancel() {
      if (done || committing || cancelled) return;
      cancelled = true;
      cancelledSignal.complete();
      unawaited(assignmentClient?.cancel());
    }

    if (cancellation != null) {
      unawaited(
        cancellation.then((_) => cancel(), onError: (Object _) => cancel()),
      );
    }
    void requireIntent() {
      if (cancelled || !_ownsSession(boardId, token)) {
        throw StateError(
          'Player assignment was cancelled or the connection changed.',
        );
      }
    }

    Future<void> waitForRead(Future<void> work) async {
      await Future.any<void>([work, cancelledSignal.future]);
      requireIntent();
    }

    try {
      // Capacity errors have an actionable local remedy; do not wait for a
      // board read to discover them. Recompute under the owner before writing.
      final available = (await store.load()).directory;
      available.assignName(side: side, displayName: displayName);
      requireIntent();
      await waitForRead(idle);
      final before = _completedPulls;
      readScope = (
        cancelled: () => cancelled,
        bind: (client) => assignmentClient = client,
      );
      _nextAssignmentRead = readScope;
      _requestPull();
      await waitForRead(idle);
      final result = _lastPull;
      final identity = _activeBoardIdentity;
      final fence = _authorityFence;
      if (_completedPulls <= before ||
          result == null ||
          identity == null ||
          (result.linePresent &&
              result.archive == null &&
              !result.archiveAcknowledged) ||
          (result.archive != null &&
              result.archive!.gameId == _retiredBoardGames[identity])) {
        throw StateError('Could not read the current game. Try again.');
      }
      return await _transactionOwner.runExclusive(() async {
        bool current() =>
            !cancelled &&
            _ownsSession(boardId, token) &&
            fence == _authorityFence &&
            identity == _activeBoardIdentity;
        if (!current()) throw StateError('The current game changed.');
        final previous = (await store.load()).directory;
        final candidate = previous.assignName(
          side: side,
          displayName: displayName,
        );
        bool selectionAttempted = false;
        try {
          if (!current() || beginCommit?.call() == false) {
            throw StateError('Player assignment was cancelled.');
          }
          committing = true;
          selectionAttempted = true;
          await store.save(candidate);
          if (!current()) throw StateError('The current game changed.');
          final archive = result.archive;
          if (archive != null) {
            final person = candidate.selectedPlayer(side);
            await _repository.assignCurrentBoardPlayer(
              expected: archive,
              white: side == LocalPlayerChessSide.white,
              player: person == null
                  ? const ArchivedPlayerAttribution.guest()
                  : ArchivedPlayerAttribution.profile(person.id),
              isCurrent: current,
            );
          }
          return candidate;
        } catch (_) {
          if (selectionAttempted) {
            final prior = previous.selectedPlayer(side);
            await store.save(
              candidate.selectPlayer(side: side, profileId: prior?.id),
            );
          }
          rethrow;
        }
      });
    } finally {
      done = true;
      if (identical(_nextAssignmentRead, readScope)) _nextAssignmentRead = null;
      if (_pullRequested && !_drainRunning) _requestPull();
    }
  }

  Future<void> get idle async {
    while (true) {
      // Connection and line streams are asynchronous in production. Give an
      // already-delivered event one turn to install its drain before taking a
      // quiescence snapshot, then verify no successor replaced that snapshot.
      await Future<void>.delayed(Duration.zero);
      final Future<void> observed = _drainFuture;
      await observed;
      await Future<void>.delayed(Duration.zero);
      if (!_drainRunning && identical(observed, _drainFuture)) return;
    }
  }

  void start() {
    if (_started || _closed) return;
    _started = true;
    final AppLifecycleState? lifecycle = WidgetsBinding.instance.lifecycleState;
    _foreground = lifecycle == null || lifecycle == AppLifecycleState.resumed;
    WidgetsBinding.instance.addObserver(this);
    _connectionSubscription = bleService.connectionStream.listen(
      _onConnectionChanged,
    );
    _readinessSubscription = bleService.productReadinessStream.listen((ready) {
      final id = bleService.connectedId;
      if (ready && id != null) {
        _adoptConnection(id);
      } else if (!ready) {
        _pullRequested = false;
        unawaited(_activeClient?.cancel());
      }
    });
    _lineSubscription = bleService.currentGameLineStream.listen((state) {
      final id = _deviceId;
      if (id == null || !_ownsSession(id, _sessionToken)) return;
      ++_authorityFence;
      _durableNotificationsSupported = true;
      if (state.present) _requestPull();
    });
    _homeSubscription = bleService.homeStateStream.listen((state) {
      if (!_durableNotificationsSupported &&
          state.gameStatus.outcome != 'none') {
        _requestPull();
      }
    });
    _controlSubscription = bleService.controlActionEvents.listen(
      _onControlAction,
    );
    final String? connected = bleService.connectedId;
    if (bleService.productReady && connected != null) {
      _adoptConnection(connected);
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    switch (state) {
      case AppLifecycleState.resumed:
        setForegroundActive(true);
      case AppLifecycleState.inactive:
      case AppLifecycleState.hidden:
      case AppLifecycleState.paused:
      case AppLifecycleState.detached:
        setForegroundActive(false);
    }
  }

  void setForegroundActive(bool active) {
    if (_closed || _foreground == active) return;
    _foreground = active;
    if (active) {
      final String? connected = bleService.connectedId;
      if (bleService.productReady && connected != null) {
        _adoptConnection(connected);
      }
      return;
    }
    _pullRequested = false;
    unawaited(_activeClient?.cancel());
  }

  void _onControlAction(LitchControlEvent event) {
    if (event.action == LitchControlAction.newGame &&
        event.phase == LitchControlPhase.applied) {
      ++_authorityFence;
      final identity = _activeBoardIdentity;
      final gameId = _lastPull?.archive?.gameId;
      if (identity != null && gameId != null) {
        _retiredBoardGames[identity] = gameId;
      }
      _lastPull = null;
    }
    if (_durableNotificationsSupported ||
        event.action != LitchControlAction.newGame ||
        event.phase != LitchControlPhase.rejected ||
        event.reason != LitchMutationReason.archiveRequired) {
      return;
    }
    final String? deviceId = _deviceId;
    if (deviceId != null && _ownsSession(deviceId, _sessionToken)) {
      _requestPull();
    }
  }

  void _onConnectionChanged(ConnectionStateUpdate update) {
    if (update.connectionState == DeviceConnectionState.connected &&
        bleService.productReady) {
      _adoptConnection(update.deviceId);
      return;
    }
    if (update.deviceId == _deviceId || !bleService.isConnected) {
      ++_authorityFence;
      _lastPull = null;
      _activeBoardIdentity = null;
      _deviceId = null;
      _sessionToken = -1;
      _durableNotificationsSupported = false;
      _pullRequested = false;
      unawaited(_activeClient?.cancel());
    }
  }

  void _adoptConnection(String deviceId) {
    final int token = bleService.connectionSessionToken;
    if (_deviceId != deviceId || _sessionToken != token) {
      unawaited(_activeClient?.cancel());
      ++_authorityFence;
      _lastPull = null;
      _activeBoardIdentity = null;
      _deviceId = deviceId;
      _sessionToken = token;
      _durableNotificationsSupported = false;
      if (_drainRunning) _pullRequested = true;
    }
    _requestPull();
  }

  void _requestPull() {
    if (_closed ||
        !_foreground ||
        _deviceId == null ||
        !bleService.productReady) {
      return;
    }
    if (!_drainRunning && _transactionOwner.isActive) {
      // Name assignment borrows this same owner. Preserve one ordinary drain
      // request until its directory/archive update commits or rolls back.
      _pullRequested = true;
      return;
    }
    if (_drainRunning) {
      // Capability discovery has not observed the line yet, so repeated
      // line publications are already covered by this pull. Once transfer I/O
      // begins, retain one follow-up request for a genuinely newer line.
      if (_activeClient != null || _assignmentReadRunning) {
        _pullRequested = true;
      }
      return;
    }
    _pullRequested = true;
    _drainRunning = true;
    _drainFuture = _runDrain();
  }

  Future<void> _runDrain() async {
    try {
      do {
        await _drain();
      } while (_pullRequested && !_closed && _foreground && _deviceId != null);
    } finally {
      _drainRunning = false;
    }
  }

  Future<void> _drain() async {
    while (_pullRequested && !_closed && _foreground) {
      _pullRequested = false;
      final String? deviceId = _deviceId;
      final int sessionToken = _sessionToken;
      final assignmentRead = _nextAssignmentRead;
      _nextAssignmentRead = null;
      if (deviceId == null || !_ownsSession(deviceId, sessionToken)) return;
      _assignmentReadRunning = assignmentRead != null;
      try {
        final capabilities = await bleService.fetchDeviceCapabilities(
          deviceId: deviceId,
        );
        if (!_ownsSession(deviceId, sessionToken) ||
            !capabilities.supportsGameHistory ||
            (assignmentRead?.cancelled() ?? false)) {
          return;
        }
        final identity = await bleService.historyStorageIdentity(deviceId);
        if (!_ownsSession(deviceId, sessionToken) ||
            (assignmentRead?.cancelled() ?? false)) {
          return;
        }
        _activeBoardIdentity = identity;
        final CurrentGamePullClient client = CurrentGamePullClient(
          boardIdentity: identity,
          transport: _LitchGameHistoryTransport(bleService, deviceId),
          receiveStore: _receiveStore,
          isOwnerReady: () => _ownsSession(deviceId, sessionToken),
          nextTransactionId: _nextTransactionId,
          transactionOwner: _transactionOwner,
          clock: _clock,
        );
        assignmentRead?.bind(client);
        if (assignmentRead?.cancelled() ?? false) return;
        _activeClient = client;
        final CurrentGameSyncResult result = await client.pull();
        if (_ownsSession(deviceId, sessionToken)) {
          _lastPull = result;
          final gameId = result.archive?.gameId;
          if (gameId != null && _retiredBoardGames[identity] != gameId) {
            _retiredBoardGames.remove(identity);
          }
          ++_completedPulls;
        }
        _appTrace?.record(
          'game_history.pull_complete',
          fields: <String, Object?>{
            'line_present': result.linePresent,
            'segments': result.segmentCount,
            'archive_acknowledged': result.archiveAcknowledged,
          },
        );
      } catch (error) {
        _lastPull = null;
        _appTrace?.record(
          'game_history.pull_failed',
          fields: <String, Object?>{'error': error.runtimeType.toString()},
        );
      } finally {
        _assignmentReadRunning = false;
        _activeClient = null;
      }
    }
  }

  bool _ownsSession(String deviceId, int sessionToken) =>
      !_closed &&
      _foreground &&
      bleService.productReady &&
      bleService.connectedId == deviceId &&
      bleService.connectionSessionToken == sessionToken;

  int _nextTransactionId() {
    _nextTransaction = (_nextTransaction % 0xffff) + 1;
    return _nextTransaction;
  }

  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    _pullRequested = false;
    WidgetsBinding.instance.removeObserver(this);
    await _activeClient?.cancel();
    await _connectionSubscription?.cancel();
    await _readinessSubscription?.cancel();
    await _lineSubscription?.cancel();
    await _homeSubscription?.cancel();
    await _controlSubscription?.cancel();
    await _drainFuture;
  }
}

class _LitchGameHistoryTransport implements GameHistoryTransport {
  _LitchGameHistoryTransport(this._bleService, this._deviceId);

  final LitchBleService _bleService;
  final String _deviceId;
  final StreamController<Uint8List> _notifications =
      StreamController<Uint8List>.broadcast(sync: true);
  StreamSubscription<List<int>>? _subscription;

  @override
  Stream<Uint8List> get notifications => _notifications.stream;

  @override
  Future<void> subscribe() async {
    if (_subscription != null) {
      throw StateError('Game-history transport is already subscribed.');
    }
    _subscription = _bleService
        .gameHistoryPackets(_deviceId)
        .listen(
          (List<int> bytes) => _notifications.add(Uint8List.fromList(bytes)),
          onError: _notifications.addError,
          onDone: _notifications.close,
        );
  }

  @override
  Future<void> write(Uint8List value) =>
      _bleService.writeGameHistoryPacket(_deviceId, value);

  @override
  Future<void> unsubscribe() async {
    final StreamSubscription<List<int>>? subscription = _subscription;
    _subscription = null;
    await subscription?.cancel();
    if (!_notifications.isClosed) await _notifications.close();
  }
}
