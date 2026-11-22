import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' show FlutterView;

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../controllers/litchcraft_controller.dart';
import '../controllers/litchcraft_library.dart';
import '../localization/litch_strings.dart';
import '../models/home_state_notification.dart';
import '../models/litch_board.dart';
import '../models/owner_pairing_session.dart';
import '../models/litch_control_action.dart';
import '../models/litchcraft_state.dart';
import '../models/user_text.dart';
import '../services/app_trace_recorder.dart';
import '../services/connection_session.dart';
import '../services/device_landscape_side.dart';
import '../widgets/orientation_workspace.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/warm_page_view.dart';
import '../services/game_library_repository.dart';
import '../services/litch_ble_service.dart';
import '../services/local_player_profile_store.dart';
import '../services/litchcraft_artwork_store.dart';
import '../services/ota/ota_benchmark_runner.dart';
import '../theme/litch_appearance.dart';
import '../theme/litch_theme.dart';
import '../widgets/app_preferences.dart';
import '../widgets/compact_controls.dart';
import '../widgets/home_notice_presenter.dart';
import '../widgets/litchcraft_tools.dart';
import '../widgets/recent_games_section.dart';
import 'about_screen.dart';
import 'bluetooth_pairing_recovery_screen.dart';
import 'app_console_screen.dart';
import 'firmware_screen.dart';
import 'game_library_screen.dart';
import 'home_screen.dart';
import 'litchcraft_screen.dart';
import 'settings_screen.dart';
import 'support_report_screen.dart';

typedef AppConsolePreferenceLoader = Future<bool> Function();
typedef AppConsolePreferenceSaver = Future<void> Function(bool enabled);
const _enableAppConsolePreferenceKey = 'enable_app_console';

Future<bool> _confirmAboutArtwork(BuildContext context) async {
  // About has just popped. Let that route finish before presenting the artwork
  // decision, so the confirmation is visible above the destination canvas.
  await WidgetsBinding.instance.endOfFrame;
  if (!context.mounted) return false;
  final strings = litchStrings(context);
  return await showDialog<bool>(
        context: context,
        builder: (dialogContext) => AlertDialog(
          title: Text(strings.craftOpenQuestion),
          content: Text(strings.craftOpenHelp),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.of(dialogContext).pop(false),
              child: Text(strings.cancelAction),
            ),
            FilledButton(
              onPressed: () => Navigator.of(dialogContext).pop(true),
              child: Text(strings.craftOpen),
            ),
          ],
        ),
      ) ??
      false;
}

String _shortLitchId(String deviceId) {
  final String hex = deviceId.toUpperCase().replaceAll(
    RegExp(r'[^0-9A-F]'),
    '',
  );
  if (hex.length <= 4) return hex.isEmpty ? deviceId.toUpperCase() : hex;
  return hex.substring(hex.length - 4);
}

Color _bottomNavigationColor(BuildContext context) =>
    MediaQuery.orientationOf(context) == Orientation.portrait
    ? NavigationBarTheme.of(context).backgroundColor ??
          Theme.of(context).colorScheme.surfaceContainer
    : Theme.of(context).scaffoldBackgroundColor;

double _bottomNavigationHeight(Size window) =>
    LitchWorkspaceGeometry.isTabletWindow(window) &&
        window.width > window.height
    ? LitchWorkspaceGeometry.tabletNavigationHeight
    : 48.0;

Widget _bottomNavigationGroup(
  BuildContext context,
  Widget child, {
  bool fallbackOnLeft = false,
}) {
  final tablet = LitchWorkspaceGeometry.isTabletWindow(
    MediaQuery.sizeOf(context),
  );
  final portrait = MediaQuery.orientationOf(context) == Orientation.portrait;
  final safeInsets = MediaQuery.viewPaddingOf(context);
  final sideGap = portrait ? 0.0 : LitchWorkspaceGeometry.contentEdgeInset;
  final theme = Theme.of(context);
  // A 32px selection indicator leaves 16px for a single scaled label.
  final controls = LitchCompactControls(
    child: tablet && portrait
        ? Theme(
            data: theme.copyWith(
              navigationBarTheme: theme.navigationBarTheme.copyWith(
                labelPadding: EdgeInsets.zero,
              ),
              textTheme: theme.textTheme.copyWith(
                labelMedium: theme.textTheme.labelMedium?.copyWith(height: 1),
              ),
            ),
            child: child,
          )
        : child,
  );
  final onLeft =
      OrientationWorkspaceScope.maybeOf(context)?.tabletToolsOnLeft ??
      fallbackOnLeft;
  return MediaQuery.removePadding(
    context: context,
    removeLeft: true,
    removeTop: true,
    removeRight: true,
    removeBottom: true,
    child: Padding(
      padding: EdgeInsets.fromLTRB(
        safeInsets.left + sideGap,
        0,
        safeInsets.right + sideGap,
        safeInsets.bottom,
      ),
      child: Align(
        alignment: tablet && !portrait
            ? (onLeft ? Alignment.centerLeft : Alignment.centerRight)
            : Alignment.center,
        heightFactor: 1,
        child: SizedBox(
          key: const ValueKey('bottom-navigation-controls'),
          width: portrait
              ? double.infinity
              : LitchWorkspaceGeometry.controlGroupWidth,
          child: portrait
              ? controls
              : ClipRRect(
                  borderRadius: BorderRadius.circular(LitchShapes.panelRadius),
                  child: controls,
                ),
        ),
      ),
    ),
  );
}

// Board/session page IDs remain stable while visible destinations follow the
// user's Play, Litchcraft, Setup, Console navigation order.
int _navigationIndexForPage(int page) => switch (page) {
  0 => 1,
  1 => 0,
  4 => 3,
  _ => page,
};

int _pageForNavigationIndex(int index) => switch (index) {
  0 => 1,
  1 => 0,
  3 => 4,
  _ => index,
};

typedef FirmwarePageBuilder =
    Widget Function(
      LitchBleService bleService,
      String boardId,
      VoidCallback onRequestShowFirmwareTab,
      ValueChanged<bool> onUpdateActivityChanged,
      ValueChanged<FirmwareUpdateAvailability> onAvailabilityChanged,
    );

enum _DiscoveryStatus {
  scanningBoards,
  connectedToBoard,
  connectedToNamedBoard,
  connectionLostWaiting,
  disconnectedScanning,
  connectingToBoard,
  connectFailedBoard,
  disconnectedChoose,
  reconnectingBoard,
  reconnectedBoard,
  reconnectFailed,
  previousBoardFound,
  multipleBoardsFound,
  oneBoardFound,
  chooseBoard,
  allowBluetooth,
  bluetoothBlocked,
  allowLocation,
  locationBlocked,
  turnOnBluetooth,
  turnOnLocation,
  bluetoothUnsupported,
  bluetoothUnavailable,
  scanningPrevious,
}

enum _ConnectionMilestone { waiting, linked, identified, ownerStatus, ready }

class ConnectionGateScreen extends StatefulWidget {
  const ConnectionGateScreen({
    super.key,
    this.bleService,
    this.craftController,
    this.craftLibrary,
    this.craftStoreLoader,
    this.firmwarePageBuilder,
    this.appearanceMode = LitchAppearanceMode.system,
    this.onAppearanceModeChanged,
    this.gameLibrary,
    this.gameLibraryOpener,
    this.assignPlayer,
    this.appTrace,
  }) : assert(
         (craftController == null) == (craftLibrary == null),
         'craftController and craftLibrary must be supplied together',
       );

  final LitchBleService? bleService;
  final LitchcraftController? craftController;
  final LitchcraftLibrary? craftLibrary;
  final Future<LitchcraftArtworkStore> Function()? craftStoreLoader;
  final FirmwarePageBuilder? firmwarePageBuilder;
  final LitchAppearanceMode appearanceMode;
  final Future<void> Function(LitchAppearanceMode mode)?
  onAppearanceModeChanged;
  final GameLibraryRepository? gameLibrary;
  final GameLibraryOpener? gameLibraryOpener;
  final LocalPlayerAssignment? assignPlayer;
  final AppTraceSink? appTrace;

  @override
  State<ConnectionGateScreen> createState() => _ConnectionGateScreenState();
}

class _ConnectionGateScreenState extends State<ConnectionGateScreen>
    with WidgetsBindingObserver {
  static const String _lastBoardIdKey = 'last_connected_board_id';

  late final LitchBleService _bleService;
  late final bool _ownsBleService;
  late final bool _ownsCraft;
  late final LitchcraftController _craftController;
  late final LitchcraftLibrary _craftLibrary;
  int _selectedDestination = 1;
  bool _enableAppConsole = false;
  Future<void>? _offlineCraftDeparture;

  StreamSubscription<List<LitchBoard>>? _boardsSub;
  StreamSubscription<BleAvailability>? _availabilitySub;
  StreamSubscription<ConnectionStateUpdate>? _connectionSub;
  StreamSubscription<bool>? _productReadinessSub;
  StreamSubscription<HomeStateNotification>? _homeStateSub;
  StreamSubscription<OwnerPairingSessionStatus?>? _pairingStatusSub;
  StreamSubscription<Object?>? _pairingRecoverySub;
  StreamSubscription<String>? _boardNameSub;
  final Map<String, String> _knownBoardNames = <String, String>{};
  List<LitchBoard> _boards = const <LitchBoard>[];
  Timer? _autoConnectTimer;
  Timer? _scanRetryTimer;
  Timer? _reconnectTimer;
  bool _manualChoiceRequired = false;
  bool _suppressAutoConnect = false;
  bool _manualDisconnectRequested = false;
  bool _hasConnectedSession = false;
  String? _usableDestinationBoardId;
  bool _awaitingDestination = false;
  String? _autoConnectTargetId;
  String? _reconnectTargetId;
  bool _isAppForeground = true;
  bool _idleForLock = false;
  int _lifecycleGeneration = 0;
  String? _connectedDestinationProbe;
  ({String deviceId, int token, int choice, bool usable})? _connectionSetup;
  ({String deviceId, String name, bool nativeDrop})? _connectionEnd;
  bool _connectedDestinationResolved = false;
  bool _connectedDestinationNeedsScaffoldSync = false;
  LitchBoard? _pendingDestinationBoard;
  int _pendingDestinationBoardNumber = 1;
  final _mainScaffoldKey = GlobalKey<_LitchMainScaffoldState>();
  bool _otaBenchmarkEntryStarted = false;
  Future<void>? _otaBenchmarkEntry;
  String? _dismissedOwnerRefusalDeviceId;
  int _connectionChoiceGeneration = 0;

  bool _isConnecting = false;
  Future<bool>? _connectionRequest;
  bool _restartingConnection = false;
  bool _isIdentifying = false;
  bool _identifyingAll = false;
  String? _identifyingBoardId;
  Completer<void>? _identifyCompletion;
  String? _selectedBoardId;
  String? _lastConnectedBoardId;
  String get _statusText {
    final strings = litchStrings(context);
    if (_showConnectionEnd && !_discoveryBlocked && !_isConnecting) {
      final ended = _connectionEnd!;
      return ended.nativeDrop
          ? strings.connectionEndedBeforeReady
          : strings.connectFailedBoard(ended.name);
    }
    return switch (_status.$1) {
      _DiscoveryStatus.scanningBoards => strings.scanningBoards,
      _DiscoveryStatus.connectedToBoard => strings.connectedToBoard,
      _DiscoveryStatus.connectedToNamedBoard => strings.connectedToNamedBoard(
        _status.$2,
      ),
      _DiscoveryStatus.connectionLostWaiting => strings.connectionLostWaiting,
      _DiscoveryStatus.disconnectedScanning => strings.disconnectedScanning,
      _DiscoveryStatus.connectingToBoard => strings.connectingToBoard(
        _status.$2,
      ),
      _DiscoveryStatus.connectFailedBoard => strings.connectFailedBoard(
        _status.$2,
      ),
      _DiscoveryStatus.disconnectedChoose => strings.disconnectedChoose,
      _DiscoveryStatus.reconnectingBoard => strings.reconnectingBoard,
      _DiscoveryStatus.reconnectedBoard => strings.reconnectedBoard,
      _DiscoveryStatus.reconnectFailed => strings.reconnectFailed,
      _DiscoveryStatus.previousBoardFound => strings.previousBoardFound,
      _DiscoveryStatus.multipleBoardsFound => strings.multipleBoardsFound,
      _DiscoveryStatus.oneBoardFound => strings.oneBoardFound,
      _DiscoveryStatus.chooseBoard => strings.chooseBoard,
      _DiscoveryStatus.allowBluetooth => strings.allowBluetooth,
      _DiscoveryStatus.bluetoothBlocked => strings.bluetoothBlocked,
      _DiscoveryStatus.allowLocation => strings.allowLocation,
      _DiscoveryStatus.locationBlocked => strings.locationBlocked,
      _DiscoveryStatus.turnOnBluetooth => strings.turnOnBluetooth,
      _DiscoveryStatus.turnOnLocation => strings.turnOnLocation,
      _DiscoveryStatus.bluetoothUnsupported => strings.bluetoothUnsupported,
      _DiscoveryStatus.bluetoothUnavailable => strings.bluetoothUnavailable,
      _DiscoveryStatus.scanningPrevious => strings.scanningPrevious,
    };
  }

  (_DiscoveryStatus, String) _status = (_DiscoveryStatus.scanningBoards, '');
  BleAvailability _bleAvailability = BleAvailability.checking;
  bool _awaitingFreshDiscovery = false;

  bool get _discoveryBlocked =>
      _bleAvailability != BleAvailability.checking &&
      _bleAvailability != BleAvailability.ready;

  bool _discoveryRefreshCanceled = true;

  bool get _canRefreshDiscovery =>
      _selectedDestination == 1 &&
      _isAppForeground &&
      !_idleForLock &&
      !_isConnecting &&
      !_isIdentifying &&
      _pendingDestinationBoard == null &&
      !_bleService.isConnected &&
      _bleService.pairingRecovery == null &&
      _bleAvailability == BleAvailability.ready;

  bool get _retainingCraft =>
      _hasConnectedSession &&
      _selectedDestination == 0 &&
      _usableDestinationBoardId != null &&
      _usableDestinationBoardId == _reconnectTargetId;

  bool get _waitingForBoard =>
      _awaitingDestination &&
      !_manualDisconnectRequested &&
      !_discoveryBlocked &&
      (_bleService.isConnected
          ? _selectedDestination < 2
          : _reconnectTargetId != null);

  bool get _showConnectionEnd =>
      _connectionEnd != null &&
      _connectionEnd!.deviceId == _selectedBoardId &&
      !_bleService.isConnected;

  bool get _showDiscoverySupport =>
      _showConnectionEnd ||
      _discoveryBlocked ||
      _awaitingFreshDiscovery ||
      _boards.isEmpty ||
      _status.$1 == _DiscoveryStatus.connectFailedBoard ||
      _status.$1 == _DiscoveryStatus.reconnectFailed;

  @override
  void initState() {
    super.initState();
    _ownsBleService = widget.bleService == null;
    _bleService =
        widget.bleService ?? LitchBleService(appTrace: widget.appTrace);
    _rememberConnectedBoardName();
    _boardNameSub = _bleService.settingsChangedStream.listen((key) {
      if (key == 'BLE_DEVICE_NAME' && mounted) {
        setState(_rememberConnectedBoardName);
      }
    });
    _ownsCraft = widget.craftController == null;
    _craftController =
        widget.craftController ??
        LitchcraftController(_bleService, appTrace: widget.appTrace);
    _craftLibrary =
        widget.craftLibrary ??
        LitchcraftLibrary(
          _craftController,
          storeLoader: widget.craftStoreLoader,
          appTrace: widget.appTrace,
        );
    _craftController.setDraftReadiness(_craftLibrary.ready);
    // An already-connected service still needs its first mode inspection.
    // Set this before the first build, not after the preference read completes.
    _connectedDestinationNeedsScaffoldSync = _bleService.isConnected;
    if (_bleService.connectedId case final id?) {
      _pendingDestinationBoard =
          _bleService.discoveredBoard(id) ??
          LitchBoard(id: id, name: '', rssi: 0);
    }
    _bleAvailability = _bleService.bleAvailability;
    WidgetsBinding.instance.addObserver(this);
    _boardsSub = _bleService.boardsStream.listen(_onBoardsChanged);
    _availabilitySub = _bleService.availabilityStream.listen(
      _onBleAvailabilityChanged,
    );
    _connectionSub = _bleService.connectionStream.listen(_onConnectionChanged);
    _productReadinessSub = _bleService.productReadinessStream.listen((ready) {
      if (mounted) {
        setState(() {
          if (ready) _recordUsableConnection();
        });
      }
    });
    _homeStateSub = _bleService.homeStateStream.listen(
      (_) => _observeDestinationEvidence(),
    );
    _craftController.addListener(_observeDestinationEvidence);
    _pairingStatusSub = _bleService.ownerPairingStatusStream.listen((_) {
      if (mounted) setState(() {});
    });
    _pairingRecoverySub = _bleService.pairingRecoveryStream.listen((state) {
      if (!mounted) return;
      if (state != null) {
        _autoConnectTimer?.cancel();
        _reconnectTimer?.cancel();
        _autoConnectTargetId = null;
        _reconnectTargetId = null;
        _suppressAutoConnect = true;
        _manualChoiceRequired = true;
      }
      setState(() {});
    });
    unawaited(_initializeConnectionGate());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    ++_lifecycleGeneration;
    _autoConnectTimer?.cancel();
    _scanRetryTimer?.cancel();
    _reconnectTimer?.cancel();
    _boardsSub?.cancel();
    _availabilitySub?.cancel();
    _connectionSub?.cancel();
    _productReadinessSub?.cancel();
    _homeStateSub?.cancel();
    _craftController.removeListener(_observeDestinationEvidence);
    _pairingStatusSub?.cancel();
    _pairingRecoverySub?.cancel();
    _boardNameSub?.cancel();
    if (_ownsCraft) {
      unawaited(
        _craftLibrary.flush().whenComplete(() {
          _craftLibrary.dispose();
          _craftController.dispose();
        }),
      );
    }
    if (_ownsBleService) {
      unawaited(_bleService.dispose());
    }
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    switch (state) {
      case AppLifecycleState.resumed:
        if (_isAppForeground) {
          return;
        }
        unawaited(_handleAppResumed(++_lifecycleGeneration));
        break;
      case AppLifecycleState.inactive:
        break;
      case AppLifecycleState.hidden:
      case AppLifecycleState.paused:
      case AppLifecycleState.detached:
        if (!_isAppForeground && _idleForLock) {
          return;
        }
        unawaited(_craftLibrary.flush());
        ++_lifecycleGeneration;
        unawaited(_handleAppBackgrounded());
        break;
    }
  }

  Future<void> _initializeConnectionGate() async {
    final savedBoardId = await _loadLastConnectedBoardId();
    try {
      final preferences = await SharedPreferences.getInstance();
      if (!mounted) return;
      setState(() {
        const prefix = 'board_friendly_name:';
        for (final key in preferences.getKeys().where(
          (key) => key.startsWith(prefix),
        )) {
          final value = preferences.get(key);
          final name = value is String ? value.trim() : '';
          if (name.isNotEmpty) {
            _knownBoardNames.putIfAbsent(
              key.substring(prefix.length),
              () => name,
            );
          }
        }
        _enableAppConsole =
            preferences.get(_enableAppConsolePreferenceKey) == true;
      });
    } catch (_) {
      // Optional names and Console visibility must not prevent discovery.
    }
    if (!mounted) return;

    // The preference read can finish after a real connection event, or this
    // gate can attach to an already-connected service. Transport owns identity.
    final connectedId = _bleService.connectedId;
    if (_bleService.isConnected && connectedId != null) {
      _onConnectionChanged(
        ConnectionStateUpdate(
          deviceId: connectedId,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      return;
    }
    _lastConnectedBoardId ??= savedBoardId;
    if (_lastConnectedBoardId != null) {
      _bleService.rememberCompatibleDevice(_lastConnectedBoardId!);
      setState(() {
        _selectedBoardId = _lastConnectedBoardId;
      });
    }

    _bleService.startScan();
    _startScanRetryTimer();
  }

  void _startScanRetryTimer() {
    _scanRetryTimer?.cancel();
    _scanRetryTimer = Timer.periodic(const Duration(milliseconds: 500), (_) {
      if (!_isAppForeground ||
          _idleForLock ||
          _isConnecting ||
          _bleService.isConnected ||
          _boards.isNotEmpty) {
        return;
      }
      _bleService.startScan();
    });
  }

  Future<void> _handleAppBackgrounded() async {
    // Lifecycle callbacks can arrive without a painted frame or scroll event.
    _discoveryRefreshCanceled = true;
    _isAppForeground = false;
    _idleForLock = true;
    setState(() {});
    _awaitingFreshDiscovery = true;
    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;
    _reconnectTimer?.cancel();
    _scanRetryTimer?.cancel();

    // Hiding the app does not retire a healthy native connection. Keep its
    // generation and product owners; actual link loss still drives recovery.
    await _bleService.setAppForegroundActive(false);
  }

  Future<void> _handleAppResumed(int lifecycleGeneration) async {
    _isAppForeground = true;
    _idleForLock = false;
    setState(() {});
    await _bleService.setAppForegroundActive(true);

    if (!mounted ||
        lifecycleGeneration != _lifecycleGeneration ||
        !_isAppForeground ||
        _idleForLock) {
      return;
    }

    if (_bleService.isConnected) {
      return;
    }

    _bleService.startScan();
    _startScanRetryTimer();

    _evaluateBoardsForAutoReconnect();
  }

  void _onConnectionChanged(ConnectionStateUpdate update) {
    if (!mounted || _bleService.identifyConnectionActive) {
      return;
    }

    if (update.connectionState == DeviceConnectionState.connected) {
      if (_manualDisconnectRequested) return;
      _manualDisconnectRequested = false;
      _hasConnectedSession = true;
      _reconnectTimer?.cancel();
      _reconnectTargetId = update.deviceId.isEmpty
          ? _selectedBoardId
          : update.deviceId;
      final String connectedId = _reconnectTargetId ?? _selectedBoardId ?? '';
      LitchBoard? connectedBoard;
      for (final LitchBoard board in _boards) {
        if (board.id == connectedId) {
          connectedBoard = board;
          break;
        }
      }
      final token = _bleService.connectionSessionToken;
      if (_connectionSetup?.deviceId != connectedId ||
          _connectionSetup?.token != token) {
        _connectionSetup = (
          deviceId: connectedId,
          token: token,
          choice: _connectionChoiceGeneration,
          usable: false,
        );
      }
      final String probe = '$connectedId:$token';
      final bool startsDestinationProbe = _connectedDestinationProbe != probe;
      final bool scaffoldResolvesDestination = startsDestinationProbe;
      setState(() {
        if (startsDestinationProbe) {
          _connectedDestinationProbe = probe;
          _awaitingDestination = true;
          // Transport is connected now. Mode reconciliation may continue in
          // the background, but it must not make the BLE status untruthful.
          _connectedDestinationResolved = true;
          _connectedDestinationNeedsScaffoldSync = scaffoldResolvesDestination;
        }
        if (connectedId.isNotEmpty) {
          _selectedBoardId = connectedId;
        }
        if (_bleService.productReady) _recordUsableConnection();
        if (_bleService.deviceCapabilities(connectedId).recoveryOnly) {
          _selectedDestination = 2;
        }
        if (_pendingDestinationBoard == null) {
          _status = connectedBoard != null
              ? (
                  _DiscoveryStatus.connectedToNamedBoard,
                  _connectionBoardName(connectedBoard.id, connectedBoard.name),
                )
              : (_DiscoveryStatus.connectedToBoard, '');
        }
      });
      if (connectedId.isNotEmpty) {
        _lastConnectedBoardId = connectedId;
        unawaited(_saveLastConnectedBoardId(connectedId));
      }
      return;
    }

    if (update.connectionState == DeviceConnectionState.disconnected) {
      final endedTarget = update.deviceId.isNotEmpty
          ? update.deviceId
          : _reconnectTargetId ?? _selectedBoardId;
      final setup = _connectionSetup;
      final token = _bleService.connectionSessionToken;
      // The stream may deliver after disconnect has retired this generation.
      // A replacement connect advances it again and cannot own this result.
      final endedDuringSetup =
          setup != null &&
          setup.deviceId == endedTarget &&
          setup.choice == _connectionChoiceGeneration &&
          !setup.usable &&
          (token == setup.token || token == setup.token + 1) &&
          !_bleService.isConnected &&
          _bleService.pendingOtaVerification?.deviceId != endedTarget &&
          !_manualDisconnectRequested;
      final endedBeforeReady = update.deviceId.isNotEmpty && endedDuringSetup;
      final pendingSetup = _pendingDestinationBoard;
      _connectionSetup = null;
      _connectedDestinationProbe = null;
      _connectedDestinationResolved = false;
      if (_pendingDestinationBoard == null) {
        _connectedDestinationNeedsScaffoldSync = false;
      }
      if (_bleService.ownerPairingRefusedDeviceId != null) {
        _autoConnectTimer?.cancel();
        _reconnectTimer?.cancel();
        setState(() {
          _isConnecting = false;
          _hasConnectedSession = false;
          _reconnectTargetId = null;
          _autoConnectTargetId = null;
          _manualChoiceRequired = true;
          _suppressAutoConnect = true;
        });
        return;
      }
      if (_bleService.pairingRecovery != null) {
        setState(() {
          _isConnecting = false;
          _hasConnectedSession = false;
        });
        return;
      }
      if (_isConnecting && !_bleService.isConnected && !endedBeforeReady) {
        return;
      }

      if (_discoveryBlocked) {
        _awaitingFreshDiscovery = true;
        setState(() => _isConnecting = false);
        return;
      }

      if (endedBeforeReady) {
        // An early native drop can mean the board is busy with another phone.
        // Keep that guidance and the selected journey through the next attempt.
        final keepWaiting =
            _awaitingDestination && _reconnectTargetId == endedTarget;
        _autoConnectTimer?.cancel();
        _reconnectTimer?.cancel();
        _autoConnectTargetId = null;
        if (!keepWaiting) _reconnectTargetId = null;
        _hasConnectedSession = false;
        _manualChoiceRequired = true;
        _suppressAutoConnect = true;
        _awaitingFreshDiscovery = true;
        setState(() {
          _selectedBoardId = endedTarget;
          _connectionEnd = (
            deviceId: endedTarget!,
            name: _connectionBoardName(
              endedTarget,
              pendingSetup?.name ?? 'Litch',
            ),
            nativeDrop: true,
          );
          _pendingDestinationBoard = null;
          _isConnecting = false;
        });
        if (_isAppForeground) unawaited(_bleService.startScan());
        return;
      }

      if (endedDuringSetup && pendingSetup?.id == endedTarget) {
        // Local setup cleanup permits a retry on a fresh advertisement.
        // The same Connecting dialog owns the interval before it arrives.
        _pendingDestinationBoard = null;
      }

      final String? targetId = _reconnectTargetId ?? _selectedBoardId;
      final bool shouldAutoReconnect =
          !_manualDisconnectRequested &&
          _hasConnectedSession &&
          targetId != null;

      if (shouldAutoReconnect) {
        _awaitingFreshDiscovery = true;
        _manualChoiceRequired = false;
        _suppressAutoConnect = false;
        _autoConnectTimer?.cancel();
        _autoConnectTargetId = null;

        setState(() {
          _selectedBoardId = targetId;
          _isConnecting = false;
          _status = (_DiscoveryStatus.connectionLostWaiting, '');
        });

        if (_isAppForeground) {
          _bleService.startScan();
        }
        return;
      }

      _manualDisconnectRequested = false;
      _reconnectTargetId = null;
      _reconnectTimer?.cancel();
      _hasConnectedSession = false;

      setState(() {
        _selectedBoardId = null;
        _isConnecting = false;
        _status = (_DiscoveryStatus.disconnectedScanning, '');
      });

      if (_isAppForeground) {
        _bleService.startScan();
      }
    }
  }

  void _observeDestinationEvidence() {
    if (!mounted ||
        !_awaitingDestination ||
        _connectedDestinationNeedsScaffoldSync ||
        !_bleService.productReady) {
      return;
    }
    if (_craftController.inspectedBoardActive != null ||
        (_selectedDestination == 1 && _bleService.latestHomeState != null)) {
      setState(_recordUsableConnection);
    }
  }

  void _recordUsableConnection() {
    final deviceId = _bleService.connectedId;
    if (!_bleService.isConnected || deviceId == null) return;
    // Native Connected is truthful link state, but is not usable setup proof.
    // Recovery's explicitly discovered public-only surface is usable too.
    final capabilities = _bleService.deviceCapabilities(deviceId);
    if (!_bleService.productReady && !capabilities.recoveryOnly) {
      return;
    }
    // A fallback Play screen is usable when it has received a board position.
    // Mode inspection can finish with an error while the native link is up.
    final modeResolved =
        _connectedDestinationResolved &&
        !_connectedDestinationNeedsScaffoldSync &&
        (!capabilities.explicitlyReported ||
            _craftController.inspectedBoardActive != null ||
            (_selectedDestination == 1 && _bleService.latestHomeState != null));
    if (modeResolved ||
        _selectedDestination >= 2 ||
        capabilities.recoveryOnly ||
        (capabilities.explicitlyReported && !capabilities.supportsLitchcraft)) {
      _usableDestinationBoardId = deviceId;
      _awaitingDestination = false;
    }
    _connectionSetup = (
      deviceId: deviceId,
      token: _bleService.connectionSessionToken,
      choice: _connectionChoiceGeneration,
      usable: true,
    );
    _connectionEnd = null;
  }

  void _onScaffoldModeResolved() {
    final String? deviceId = _bleService.connectedId;
    if (!mounted ||
        _manualDisconnectRequested ||
        !_bleService.isConnected ||
        deviceId == null) {
      return;
    }
    setState(() {
      _connectedDestinationResolved = true;
      _connectedDestinationNeedsScaffoldSync = false;
      _pendingDestinationBoard = null;
      _recordUsableConnection();
    });
  }

  Future<void> _connectToBoard(LitchBoard board) async {
    if (_bleService.pairingRecovery != null ||
        _isConnecting ||
        (_bleService.isConnected && !_bleService.identifyConnectionActive)) {
      return;
    }

    final choiceGeneration = ++_connectionChoiceGeneration;
    _connectionEnd = null;
    _manualChoiceRequired = true;
    _suppressAutoConnect = true;
    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;
    _manualDisconnectRequested = false;
    _dismissedOwnerRefusalDeviceId = null;
    _hasConnectedSession = false;
    _usableDestinationBoardId = null;
    _awaitingDestination = true;
    _reconnectTargetId = board.id;
    _reconnectTimer?.cancel();

    setState(() {
      _discoveryRefreshCanceled = true;
      _isConnecting = true;
      _selectedBoardId = board.id;
      if (_selectedDestination == 1) {
        _pendingDestinationBoard = board;
        _pendingDestinationBoardNumber = math.max(
          1,
          _boards.indexWhere((candidate) => candidate.id == board.id) + 1,
        );
      }
      // The panel owns progress; keep the chooser's reserved text geometry.
      if (_pendingDestinationBoard == null) {
        _status = (
          _DiscoveryStatus.connectingToBoard,
          _connectionBoardName(board.id, board.name),
        );
      }
    });

    await _identifyCompletion?.future;
    final bool requestStillValid =
        mounted &&
        _isConnecting &&
        choiceGeneration == _connectionChoiceGeneration &&
        _selectedBoardId == board.id &&
        _isAppForeground &&
        !_idleForLock &&
        !_discoveryBlocked &&
        !_awaitingFreshDiscovery &&
        _boards.any((candidate) => candidate.id == board.id);
    if (!requestStillValid || _bleService.isConnected) {
      if (mounted &&
          _isConnecting &&
          choiceGeneration == _connectionChoiceGeneration &&
          _selectedBoardId == board.id) {
        setState(() {
          _isConnecting = false;
        });
      }
      return;
    }

    final request = _connectionRequest = _bleService.connect(board.id);
    final token = _bleService.connectionSessionToken;
    final bool connected = await request;

    if (!mounted ||
        _manualDisconnectRequested ||
        choiceGeneration != _connectionChoiceGeneration ||
        _selectedBoardId != board.id) {
      return;
    }
    final currentToken = _bleService.connectionSessionToken;
    if (currentToken != token &&
        (_bleService.isConnected || currentToken != token + 1)) {
      return;
    }
    final live =
        connected &&
        _bleService.isConnected &&
        _bleService.connectedId == board.id;
    final updatePending =
        _bleService.pendingOtaVerification?.deviceId == board.id;
    final refused = _bleService.ownerPairingRefusedDeviceId == board.id;
    // A silent native attempt and a transient drop both remain part of the
    // same choice. A later advertisement or the panel can retry that target.
    final waitingForAdvertisement =
        !live &&
        _awaitingDestination &&
        _reconnectTargetId == board.id &&
        !refused &&
        !updatePending &&
        !_discoveryBlocked &&
        _bleService.pairingRecovery == null;
    setState(() {
      _isConnecting = false;
      if (!live && !refused && !updatePending) {
        _pendingDestinationBoard = null;
        if (!waitingForAdvertisement && _bleService.pairingRecovery == null) {
          if (!_hasConnectedSession) _reconnectTargetId = null;
          _connectionEnd ??= (
            deviceId: board.id,
            name: _connectionBoardName(board.id, board.name),
            nativeDrop: false,
          );
        }
      }
      if (_pendingDestinationBoard == null) {
        _status = live
            ? (
                _DiscoveryStatus.connectedToNamedBoard,
                _connectionBoardName(board.id, board.name),
              )
            : (
                _DiscoveryStatus.connectFailedBoard,
                _connectionBoardName(board.id, board.name),
              );
      }
    });
    if (!live) _autoConnectTargetId = null;
    if (waitingForAdvertisement && !_hasConnectedSession && _isAppForeground) {
      unawaited(_bleService.startScan());
    }
  }

  Future<void> _disconnect({
    bool returnToBoardList = false,
    Future<void>? supportReport,
  }) async {
    // Disconnect keeps the selected main page. An offline status action asks
    // for board choices; the board-only firmware page also returns there.
    final retainDestination =
        !returnToBoardList &&
        _bleService.isConnected &&
        _selectedDestination != 3;
    final choiceGeneration = ++_connectionChoiceGeneration;
    _manualDisconnectRequested = true;
    _connectionEnd = null;
    setState(() {
      _pendingDestinationBoard = null;
      if (!retainDestination) _selectedDestination = 1;
      _isConnecting = false;
    });
    if (!retainDestination) {
      _mainScaffoldKey.currentState?._showConnectionChoices();
    }
    _hasConnectedSession = false;
    _usableDestinationBoardId = null;
    _awaitingDestination = false;
    _reconnectTimer?.cancel();
    _reconnectTargetId = null;

    _manualChoiceRequired = true;
    _suppressAutoConnect = true;
    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;

    // A live report can still collect diagnostics from this session. The
    // user's choice has already stopped every automatic retry behind it.
    if (supportReport != null && _bleService.isConnected) {
      await supportReport;
      if (!mounted || choiceGeneration != _connectionChoiceGeneration) return;
    }
    final request = _bleService.disconnect();
    final token = _bleService.connectionSessionToken;
    final selectedBoardId = _selectedBoardId;
    bool ownsRecovery() =>
        mounted &&
        choiceGeneration == _connectionChoiceGeneration &&
        _bleService.connectionSessionToken == token &&
        (_selectedBoardId == null || _selectedBoardId == selectedBoardId);
    await request;
    if (!ownsRecovery()) return;
    // Our disconnected observation may already have cleared the selection.
    // Release even a silent SDK scan; ordinary starts preserve its subscription.
    await _bleService.stopScan();
    if (!ownsRecovery()) return;

    setState(() {
      _selectedBoardId = null;
      _isConnecting = false;
      _status = (_DiscoveryStatus.disconnectedChoose, '');
    });

    if (_isAppForeground) {
      _bleService.startScan();
    }
  }

  void _scheduleReconnect() {
    if (_bleService.pairingRecovery != null ||
        _reconnectTargetId == null ||
        !_isAppForeground ||
        _idleForLock ||
        _manualDisconnectRequested ||
        _bleService.isConnected) {
      return;
    }

    if (_reconnectTimer?.isActive == true) {
      return;
    }

    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(Duration.zero, () {
      unawaited(_attemptReconnect());
    });
  }

  bool _canRetryWaitingConnection(String? id) {
    if (id == null ||
        id != _reconnectTargetId ||
        _connectionChoiceGeneration == 0 ||
        !_awaitingDestination ||
        !_isAppForeground ||
        _idleForLock ||
        _isIdentifying ||
        _bleService.identifyConnectionActive) {
      return false;
    }
    final presentation = _connectionPresentation;
    // A live owner/security negotiation owns its own deadline and may need a
    // physical confirmation. Help stays available during quiet verification.
    return presentation.showProgress && !presentation.pairingBlocked;
  }

  Future<void> _restartWaitingConnection(
    String? id,
    int sessionToken,
    _ConnectionMilestone milestone,
  ) async {
    if (_restartingConnection || !_canRetryWaitingConnection(id)) return;
    // Native/owner evidence can arrive before the next Flutter frame.
    final current = _connectionMilestone(id);
    if (current.index > milestone.index ||
        (current != _ConnectionMilestone.waiting &&
            _bleService.connectionSessionToken != sessionToken)) {
      return;
    }
    _restartingConnection = true;
    try {
      final choice = ++_connectionChoiceGeneration;
      if (_isConnecting || _bleService.isConnected) {
        final previousRequest = _connectionRequest;
        _connectionSetup = null;
        setState(() => _isConnecting = true);
        await _bleService.disconnect();
        await previousRequest;
      }
      if (!mounted || choice != _connectionChoiceGeneration) return;
      setState(() => _isConnecting = false);
      if (_canRetryWaitingConnection(id)) {
        await _attemptReconnect(retrySelectedBoard: true);
      }
    } finally {
      _restartingConnection = false;
    }
  }

  Future<void> _attemptReconnect({bool retrySelectedBoard = false}) async {
    final String? targetId = _reconnectTargetId;
    if (_bleService.pairingRecovery != null ||
        !mounted ||
        !_isAppForeground ||
        _idleForLock ||
        targetId == null ||
        _discoveryBlocked ||
        (!retrySelectedBoard &&
            (_awaitingFreshDiscovery ||
                !_boards.any((board) => board.id == targetId))) ||
        _manualDisconnectRequested ||
        _bleService.isConnected ||
        _isConnecting) {
      return;
    }

    final choiceGeneration = ++_connectionChoiceGeneration;
    if (!_awaitingDestination) _connectionEnd = null;
    setState(() {
      _discoveryRefreshCanceled = true;
      _isConnecting = true;
      _selectedBoardId = targetId;
      _awaitingDestination = true;
      _status = (_DiscoveryStatus.reconnectingBoard, '');
    });

    final request = _connectionRequest = _bleService.connect(targetId);
    final token = _bleService.connectionSessionToken;
    final bool connected = await request;

    if (!mounted ||
        _manualDisconnectRequested ||
        choiceGeneration != _connectionChoiceGeneration ||
        _selectedBoardId != targetId) {
      return;
    }
    final currentToken = _bleService.connectionSessionToken;
    if (currentToken != token &&
        (_bleService.isConnected || currentToken != token + 1)) {
      return;
    }

    if (connected) {
      setState(() {
        _isConnecting = false;
        _status = (_DiscoveryStatus.reconnectedBoard, '');
      });
      return;
    }

    setState(() {
      _isConnecting = false;
      _status = (_DiscoveryStatus.reconnectFailed, '');
    });

    if (_isAppForeground) {
      _bleService.startScan();
    }
  }

  void _onBoardsChanged(List<LitchBoard> boards) {
    // Identify uses a temporary BLE connection. Its internal scan restart
    // publishes an empty list before advertisements return; preserving the
    // current roster avoids making the user's choices disappear mid-action.
    if (_isIdentifying && boards.isEmpty && _boards.isNotEmpty) {
      return;
    }

    final List<LitchBoard> stableBoards = _reconcileBoardRoster(boards);
    final bool rosterChanged = !_sameBoardRoster(_boards, stableBoards);
    if (rosterChanged) {
      setState(() => _boards = stableBoards);
    }

    if (_discoveryBlocked) {
      return;
    }

    final bool firstFreshRoster = _awaitingFreshDiscovery;
    _awaitingFreshDiscovery = false;
    if (_reconnectTargetId != null && !_bleService.isConnected) {
      _evaluateBoardsForAutoReconnect();
      return;
    }

    if (!rosterChanged && !firstFreshRoster) {
      return;
    }

    if (_isConnecting || _bleService.isConnected) {
      return;
    }

    if (stableBoards.length > 1) {
      _manualChoiceRequired = true;
    }

    final String? rememberedBoardId = _lastConnectedBoardId;
    if (rememberedBoardId != null && !_suppressAutoConnect) {
      final List<LitchBoard> rememberedBoards = stableBoards
          .where((LitchBoard board) => board.id == rememberedBoardId)
          .toList(growable: false);
      if (rememberedBoards.length == 1) {
        _manualChoiceRequired = false;
        if (_autoConnectTargetId != rememberedBoards.first.id) {
          _autoConnectTargetId = rememberedBoards.first.id;
          _scheduleAutoConnect(rememberedBoards.first);
        }
        setState(() {
          _status = (_DiscoveryStatus.previousBoardFound, '');
        });
        return;
      }

      if (stableBoards.length > 1) {
        _autoConnectTimer?.cancel();
        _autoConnectTargetId = null;
        setState(() {
          _status = (_DiscoveryStatus.multipleBoardsFound, '');
        });
        return;
      }
    }

    if (stableBoards.length == 1 &&
        !_manualChoiceRequired &&
        !_suppressAutoConnect &&
        _lastConnectedBoardId == null) {
      if (_autoConnectTargetId != stableBoards.first.id) {
        _autoConnectTargetId = stableBoards.first.id;
        _scheduleAutoConnect(stableBoards.first);
      }
      setState(() {
        _status = (_DiscoveryStatus.oneBoardFound, '');
      });
      return;
    }

    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;
    if (stableBoards.isEmpty) {
      setState(() {
        _status = (_DiscoveryStatus.scanningBoards, '');
      });
      return;
    }

    setState(() {
      _status = (_DiscoveryStatus.chooseBoard, '');
    });
  }

  bool _sameVisibleBoard(LitchBoard first, LitchBoard second) =>
      first.id == second.id && first.name == second.name;

  List<LitchBoard> _reconcileBoardRoster(List<LitchBoard> boards) {
    final Map<String, LitchBoard> incoming = <String, LitchBoard>{
      for (final LitchBoard board in boards) board.id: board,
    };
    final List<LitchBoard> stable = <LitchBoard>[];
    for (final LitchBoard previous in _boards) {
      final LitchBoard? updated = incoming.remove(previous.id);
      if (updated != null) {
        stable.add(_sameVisibleBoard(previous, updated) ? previous : updated);
      }
    }
    for (final LitchBoard board in boards) {
      if (incoming.remove(board.id) != null) {
        stable.add(board);
      }
    }
    return stable;
  }

  bool _sameBoardRoster(List<LitchBoard> first, List<LitchBoard> second) {
    if (first.length != second.length) {
      return false;
    }
    for (int index = 0; index < first.length; ++index) {
      if (!_sameVisibleBoard(first[index], second[index])) {
        return false;
      }
    }
    return true;
  }

  void _onBleAvailabilityChanged(BleAvailability availability) {
    if (!mounted) {
      return;
    }

    setState(() => _bleAvailability = availability);
    if (_discoveryBlocked) {
      _discoveryRefreshCanceled = true;
      _autoConnectTimer?.cancel();
      _autoConnectTargetId = null;
      _reconnectTimer?.cancel();
      setState(() {
        _boards = const <LitchBoard>[];
        _pendingDestinationBoard = null;
        _awaitingFreshDiscovery = true;
        _isConnecting = false;
        _status = switch (availability) {
          BleAvailability.bluetoothPermissionDenied => (
            _DiscoveryStatus.allowBluetooth,
            '',
          ),
          BleAvailability.bluetoothPermissionBlocked => (
            _DiscoveryStatus.bluetoothBlocked,
            '',
          ),
          BleAvailability.locationPermissionDenied => (
            _DiscoveryStatus.allowLocation,
            '',
          ),
          BleAvailability.locationPermissionBlocked => (
            _DiscoveryStatus.locationBlocked,
            '',
          ),
          BleAvailability.poweredOff => (_DiscoveryStatus.turnOnBluetooth, ''),
          BleAvailability.locationServicesDisabled => (
            _DiscoveryStatus.turnOnLocation,
            '',
          ),
          BleAvailability.unsupported => (
            _DiscoveryStatus.bluetoothUnsupported,
            '',
          ),
          BleAvailability.unavailable => (
            _DiscoveryStatus.bluetoothUnavailable,
            '',
          ),
          BleAvailability.checking || BleAvailability.ready => _status,
        };
      });
      return;
    }

    if (_awaitingFreshDiscovery) {
      setState(() {
        _status = (_DiscoveryStatus.scanningBoards, '');
      });
      return;
    }

    if (_reconnectTargetId != null && !_bleService.isConnected) {
      _evaluateBoardsForAutoReconnect();
      return;
    }
    _onBoardsChanged(_boards);
  }

  Future<void> _recoverBluetoothAccess() async {
    if (_bleService.availabilityRecoveryNeedsSettings) {
      await _bleService.openBluetoothSettings();
      return;
    }
    await _bleService.requestBluetoothAccess();
  }

  Widget _buildDiscoveryScrollView({required Widget child}) {
    if (!_canRefreshDiscovery) _discoveryRefreshCanceled = true;
    final scroll = SingleChildScrollView(
      key: const PageStorageKey<String>('disconnected-play-scroll'),
      physics: const AlwaysScrollableScrollPhysics(),
      child: child,
    );
    // Enabling refresh changes input, not the retained discovery subtree.
    return RefreshIndicator(
      key: const ValueKey('discovery-refresh'),
      notificationPredicate: (notification) {
        if (!defaultScrollNotificationPredicate(notification)) return false;
        if (!_canRefreshDiscovery) {
          _discoveryRefreshCanceled = true;
        } else if (notification is ScrollStartNotification &&
            notification.dragDetails != null) {
          _discoveryRefreshCanceled = false;
        }
        // A canceled armed pull may finish its snap only after app resume.
        // Drain its release without authorizing that old gesture to rescan.
        return _canRefreshDiscovery || notification is ScrollEndNotification;
      },
      onRefresh: () async {
        if (!_discoveryRefreshCanceled && _canRefreshDiscovery) {
          await _bleService.rescan();
        }
      },
      child: scroll,
    );
  }

  void _evaluateBoardsForAutoReconnect() {
    final targetId =
        _reconnectTargetId ??
        (_suppressAutoConnect || _manualChoiceRequired
            ? null
            : _lastConnectedBoardId);
    if (_bleService.pairingRecovery != null ||
        !_isAppForeground ||
        _idleForLock ||
        _discoveryBlocked ||
        _awaitingFreshDiscovery ||
        _manualDisconnectRequested ||
        _bleService.isConnected ||
        _isConnecting ||
        (_awaitingDestination && _connectionPresentation.showProgress) ||
        targetId == null) {
      return;
    }

    final List<LitchBoard> matchingBoards = _boards
        .where((LitchBoard board) => board.id == targetId)
        .toList(growable: false);
    if (matchingBoards.length == 1) {
      _reconnectTargetId = targetId;
      _scheduleReconnect();
      return;
    }

    _reconnectTimer?.cancel();
    if (!mounted) {
      return;
    }

    setState(() {
      if (_boards.isEmpty &&
          _status.$1 == _DiscoveryStatus.connectionLostWaiting) {
        return;
      }
      _status = _boards.length > 1
          ? (_DiscoveryStatus.multipleBoardsFound, '')
          : (_DiscoveryStatus.scanningPrevious, '');
    });
  }

  void _scheduleAutoConnect(LitchBoard board) {
    if (_bleService.pairingRecovery != null) return;
    _autoConnectTimer?.cancel();
    // A prior choice identifies the target; first use still gives discovery
    // time to reveal multiple boards. Queue either path so new state can cancel
    // it before connecting, without adding a grace period for a known target.
    final Duration delay = board.id == _lastConnectedBoardId
        ? Duration.zero
        : const Duration(seconds: 2);
    _autoConnectTimer = Timer(delay, () {
      if (mounted &&
          _isAppForeground &&
          !_idleForLock &&
          !_discoveryBlocked &&
          !_awaitingFreshDiscovery &&
          !_suppressAutoConnect &&
          _autoConnectTargetId == board.id &&
          !_isConnecting &&
          !_bleService.isConnected &&
          _boards.any((candidate) => candidate.id == board.id) &&
          (_lastConnectedBoardId == board.id || _boards.length == 1)) {
        _connectToBoard(board);
      }
    });
  }

  Future<String?> _loadLastConnectedBoardId() async {
    try {
      final SharedPreferences prefs = await SharedPreferences.getInstance();
      final String? value = prefs.getString(_lastBoardIdKey)?.trim();
      if (value == null || value.isEmpty) {
        return null;
      }
      return value;
    } catch (_) {
      return null;
    }
  }

  Future<void> _saveLastConnectedBoardId(String boardId) async {
    try {
      final SharedPreferences prefs = await SharedPreferences.getInstance();
      await prefs.setString(_lastBoardIdKey, boardId);
    } catch (_) {}
  }

  Future<void> _identifyAllBoards({
    required int durationMs,
    required bool untilConnected,
    required bool showFeedback,
  }) async {
    if (_boards.isEmpty) {
      return;
    }

    final List<LitchBoard> boardsSnapshot = List<LitchBoard>.from(_boards);
    final choiceGeneration = _connectionChoiceGeneration;
    final lifecycleGeneration = _lifecycleGeneration;
    bool current() =>
        mounted &&
        _isAppForeground &&
        !_idleForLock &&
        !_isConnecting &&
        choiceGeneration == _connectionChoiceGeneration &&
        lifecycleGeneration == _lifecycleGeneration;
    int successCount = 0;
    for (int index = 0; index < boardsSnapshot.length; ++index) {
      // A queued Connect owns the next action once the in-flight Identify
      // releases its native connection. Leave the remaining boards untouched.
      if (!current()) return;
      final LitchBoard board = boardsSnapshot[index];
      final bool ok = await _bleService.sendIdentify(
        board.id,
        index + 1,
        durationMs: durationMs,
        untilConnected: untilConnected,
      );
      if (ok) {
        ++successCount;
      }
    }

    if (!mounted || !current() || !showFeedback) {
      return;
    }

    final String message = successCount == boardsSnapshot.length
        ? litchStrings(context).identifyAllSent
        : litchStrings(
            context,
          ).identifyCountSent(successCount, boardsSnapshot.length.toString());

    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _identifyAllForThreeSeconds() async {
    if (_isConnecting || _isIdentifying) {
      return;
    }

    _manualChoiceRequired = true;
    _suppressAutoConnect = true;
    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;

    _identifyCompletion = Completer<void>();
    setState(() {
      _discoveryRefreshCanceled = true;
      _isIdentifying = true;
      _identifyingAll = true;
    });

    try {
      await _identifyAllBoards(
        durationMs: 3000,
        untilConnected: false,
        showFeedback: true,
      );
    } finally {
      _identifyCompletion?.complete();
      _identifyCompletion = null;
      if (mounted) {
        setState(() {
          _isIdentifying = false;
          _identifyingAll = false;
        });
      }
    }
  }

  Future<void> _identifyBoard(LitchBoard board, int number) async {
    if (_isConnecting || _isIdentifying) {
      return;
    }

    _manualChoiceRequired = true;
    _suppressAutoConnect = true;
    _autoConnectTimer?.cancel();
    _autoConnectTargetId = null;

    _identifyCompletion = Completer<void>();
    setState(() {
      _discoveryRefreshCanceled = true;
      _isIdentifying = true;
      _identifyingBoardId = board.id;
    });

    bool ok = false;
    try {
      ok = await _bleService.sendIdentify(
        board.id,
        number,
        durationMs: 3000,
        untilConnected: false,
      );
    } finally {
      _identifyCompletion?.complete();
      _identifyCompletion = null;
      if (mounted) {
        setState(() {
          _isIdentifying = false;
          _identifyingBoardId = null;
        });
      }
    }

    if (!mounted || _isConnecting) {
      return;
    }

    final String message = ok
        ? litchStrings(context).identifySent(
            number.toString(),
            _connectionBoardName(board.id, board.name),
          )
        : litchStrings(context).identifyUnavailable;

    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _selectDisconnectedDestination(int index) async {
    final main = _mainScaffoldKey.currentState;
    if (main != null) {
      setState(() {
        _selectedDestination = index;
        _pendingDestinationBoard = null;
      });
      await main._selectNavigationPage(index);
      return;
    }
    if (index == _selectedDestination && _pendingDestinationBoard == null) {
      return;
    }
    final leavingCraft = _selectedDestination == 0;
    setState(() {
      _selectedDestination = index;
      _pendingDestinationBoard = null;
    });
    if (_craftController.selected && index == 1) {
      _offlineCraftDeparture ??= _craftController
          .leave()
          .catchError((Object _) {})
          .whenComplete(() => _offlineCraftDeparture = null);
    }
    if (leavingCraft) unawaited(_craftLibrary.flush());
    if (index == 0) {
      await _offlineCraftDeparture;
      await _craftLibrary.ready;
      if (mounted && _selectedDestination == 0 && !_craftController.selected) {
        await _craftController.enter();
      }
    }
  }

  Future<void> _saveAppConsolePreference(bool enabled) async {
    try {
      final preferences = await SharedPreferences.getInstance();
      await preferences.setBool(_enableAppConsolePreferenceKey, enabled);
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _enableAppConsole = enabled;
      if (!enabled && _selectedDestination == 4) _selectedDestination = 2;
    });
  }

  Future<void> _loadVersionArtwork(int taps) async {
    if (!await _confirmAboutArtwork(context)) {
      return;
    }
    try {
      unawaited(_selectDisconnectedDestination(0));
      await _craftLibrary.loadVersionArtwork(taps);
    } catch (error) {
      if (mounted && !_craftLibrary.isOpenFailureHandled(error)) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(litchStrings(context).craftActionFailed)),
        );
      }
    }
  }

  Widget _buildDisconnectedNavigation({
    bool compact = false,
    bool embeddedToolPanel = false,
    int? currentDestination,
  }) {
    final navigationPage = switch (currentDestination ?? _selectedDestination) {
      0 => 0,
      2 => 2,
      4 when _enableAppConsole => 4,
      _ => 1,
    };
    final destinationPending =
        _pendingDestinationBoard != null && navigationPage == 1;
    final connecting =
        _isConnecting || _pendingDestinationBoard != null || _waitingForBoard;
    if (compact) {
      return _CompactAppNavigation(
        key: const ValueKey<String>('disconnected-navigation'),
        currentPage: destinationPending ? -1 : navigationPage,
        consoleEnabled: _enableAppConsole,
        embeddedToolPanel: embeddedToolPanel,
        onLoadVersionArtwork: _loadVersionArtwork,
        onSelected: (page) => unawaited(_selectDisconnectedDestination(page)),
        statusAction: Tooltip(
          message: connecting
              ? litchStrings(context).boardConnecting
              : litchStrings(context).disconnectedChoose,
          child: connecting
              ? const Icon(Icons.sync_rounded, size: 36)
              : IconButton(
                  key: const ValueKey('offline-board-status-action'),
                  onPressed: () => unawaited(_disconnect()),
                  icon: const _DisconnectedStatusIcon(size: 36),
                ),
        ),
      );
    }
    final destinations = <NavigationDestination>[
      NavigationDestination(
        key: const ValueKey('disconnected-destination-play'),
        icon: const LitchPlayGlyph(size: 31),
        label: litchStrings(context).playTab,
      ),
      NavigationDestination(
        key: const ValueKey('disconnected-destination-craft'),
        icon: const LitchcraftIdentityGlyph(size: 31),
        label: litchStrings(context).craftTitle,
      ),
      NavigationDestination(
        key: const ValueKey('disconnected-destination-setup'),
        icon: const Icon(Icons.tune_outlined, size: 31),
        selectedIcon: const Icon(Icons.tune_rounded, size: 31),
        label: litchStrings(context).setupTab,
      ),
      if (_enableAppConsole)
        NavigationDestination(
          key: const ValueKey('disconnected-destination-console'),
          icon: const Icon(Icons.terminal, size: 31),
          label: litchStrings(context).consoleTab,
        ),
    ];
    final tablet = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    final height = _bottomNavigationHeight(MediaQuery.sizeOf(context));
    return ColoredBox(
      key: const ValueKey<String>('disconnected-navigation-surface'),
      color: _bottomNavigationColor(context),
      child: _bottomNavigationGroup(
        context,
        destinationPending
            ? SizedBox(
                height: height,
                child: Row(
                  children: <Widget>[
                    for (var index = 0; index < destinations.length; ++index)
                      Expanded(
                        child: _CompactNavigationAction(
                          key: destinations[index].key,
                          label: destinations[index].label,
                          selected: false,
                          onPressed: () => unawaited(
                            _selectDisconnectedDestination(
                              _pageForNavigationIndex(index),
                            ),
                          ),
                          child: destinations[index].icon,
                        ),
                      ),
                  ],
                ),
              )
            : NavigationBar(
                key: const ValueKey<String>('disconnected-navigation'),
                height: height,
                labelBehavior: tablet
                    ? NavigationDestinationLabelBehavior.alwaysShow
                    : NavigationDestinationLabelBehavior.alwaysHide,
                selectedIndex: _navigationIndexForPage(navigationPage),
                onDestinationSelected: (int index) => unawaited(
                  _selectDisconnectedDestination(
                    _pageForNavigationIndex(index),
                  ),
                ),
                destinations: destinations,
              ),
      ),
    );
  }

  Widget _buildDisconnectedLandscapeRail({
    required String keyName,
    required int currentDestination,
    bool onLeft = false,
  }) {
    final media = MediaQuery.of(context);
    final geometry = LitchWorkspaceGeometry(
      Size(
        media.size.width - media.viewPadding.horizontal,
        media.size.height - media.viewPadding.vertical,
      ),
      safeInsets: media.viewPadding,
    );
    final edge = geometry.landscapeEdgeInsets;
    return SizedBox(
      key: ValueKey<String>(keyName),
      width:
          geometry.compactNavigationWidth + (onLeft ? edge.left : edge.right),
      child: Padding(
        padding: EdgeInsets.only(
          left: onLeft ? edge.left : 0,
          right: onLeft ? 0 : edge.right,
          bottom: edge.bottom,
        ),
        child: Align(
          alignment: onLeft ? Alignment.bottomLeft : Alignment.bottomRight,
          child: SizedBox(
            height: LitchWorkspaceGeometry.navigationHeight,
            child: _buildDisconnectedNavigation(
              compact: true,
              currentDestination: currentDestination,
            ),
          ),
        ),
      ),
    );
  }

  void _openGameLibrary() {
    if (widget.gameLibrary == null && widget.gameLibraryOpener == null) {
      return;
    }
    Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        builder: (_) => GameLibraryRouteScreen(
          repository: widget.gameLibrary,
          openRepository: widget.gameLibraryOpener,
        ),
      ),
    );
  }

  Widget _buildOfflineSetup({required bool landscape}) {
    return SafeArea(
      top: false,
      child: SingleChildScrollView(
        key: const ValueKey<String>('offline-setup-scroll'),
        padding: EdgeInsets.fromLTRB(16, landscape ? 8 : 16, 16, 16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Semantics(
              header: true,
              child: Text(
                litchStrings(context).setupTab,
                style: Theme.of(context).textTheme.headlineSmall,
              ),
            ),
            const SizedBox(height: 16),
            AppPreferences(
              appearanceMode: widget.appearanceMode,
              onAppearanceModeChanged: widget.onAppearanceModeChanged,
            ),
            const SizedBox(height: 20),
            Card(
              margin: EdgeInsets.zero,
              child: ListTile(
                minTileHeight: 64,
                leading: const Icon(Icons.developer_board_outlined),
                title: Text(litchStrings(context).boardSettingsTitle),
                subtitle: Text(litchStrings(context).boardSettingsConnect),
              ),
            ),
            const SizedBox(height: 12),
            Card(
              margin: EdgeInsets.zero,
              child: ListTile(
                key: const ValueKey<String>('open-contact-support'),
                minTileHeight: 56,
                leading: const Icon(Icons.mail_outline),
                title: Text(litchStrings(context).contactSupport),
                subtitle: Text(litchStrings(context).supportReviewHelp),
                trailing: const Icon(Icons.chevron_right_rounded),
                onTap: () =>
                    SupportReportScreen.open(context, bleService: _bleService),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _cancelPairing() async {
    try {
      if (_bleService.ownerPairingStatus?.state ==
          OwnerPairingSessionState.awaitPhysical) {
        await _bleService.cancelOwnerPairing();
      }
    } catch (_) {
      // Physical confirmation may win the race. Disconnect retires that exact
      // claim and lets the board finish its acknowledged rollback.
    }
    await _disconnect();
  }

  Future<void> _retryOwnerConfirmation() async {
    final id = _bleService.connectedId;
    if (id == null || _isConnecting) return;
    if (!_bleService.ownerPairingNeedsFreshConnection) {
      await _bleService.ensureProductReady(id, retry: true);
      return;
    }
    final choice = ++_connectionChoiceGeneration;
    final name =
        _pendingDestinationBoard?.name ??
        _boards.where((board) => board.id == id).firstOrNull?.name ??
        'Litch';
    _autoConnectTimer?.cancel();
    _reconnectTimer?.cancel();
    _autoConnectTargetId = null;
    _reconnectTargetId = id;
    _manualChoiceRequired = true;
    _suppressAutoConnect = true;
    setState(() {
      _connectionEnd = null;
      _selectedBoardId = id;
      _discoveryRefreshCanceled = true;
      _isConnecting = true;
    });
    final retry = _bleService.ensureProductReady(id, retry: true);
    final token = _bleService.connectionSessionToken;
    final ready = await retry;
    if (!mounted ||
        choice != _connectionChoiceGeneration ||
        _selectedBoardId != id) {
      return;
    }
    final currentToken = _bleService.connectionSessionToken;
    if (currentToken != token &&
        (_bleService.isConnected || currentToken != token + 1)) {
      return;
    }
    setState(() {
      _isConnecting = false;
      if (!ready &&
          !_bleService.isConnected &&
          _bleService.ownerPairingRefusedDeviceId == null &&
          _bleService.pendingOtaVerification?.deviceId != id) {
        _pendingDestinationBoard = null;
        _hasConnectedSession = false;
        _connectionEnd ??= (deviceId: id, name: name, nativeDrop: false);
      }
    });
  }

  void _rememberConnectedBoardName() {
    final id = _bleService.connectedId;
    final value = _bleService.latestSettingValue('BLE_DEVICE_NAME');
    if (_bleService.isConnected && id != null && value is String) {
      final name = value.trim();
      if (name.isNotEmpty) _knownBoardNames[id] = name;
    }
  }

  String _connectionBoardName(String? id, String? advertisedName) {
    if (_bleService.isConnected && _bleService.connectedId == id) {
      final value = _bleService.latestSettingValue('BLE_DEVICE_NAME');
      if (value is String && value.trim().isNotEmpty) return value.trim();
    }
    return _knownBoardNames[id] ?? advertisedName?.trim() ?? 'Foxie Litch';
  }

  Widget _buildSupportButton(String? targetId, {bool endJourney = false}) {
    return TextButton(
      key: const ValueKey('owner-pairing-support'),
      style: TextButton.styleFrom(minimumSize: const Size(48, 48)),
      onPressed: () {
        final report = SupportReportScreen.open(
          context,
          bleService: _bleService,
          boardId: targetId,
        );
        if (endJourney) {
          unawaited(
            _disconnect(returnToBoardList: true, supportReport: report),
          );
        }
      },
      child: Text(litchStrings(context).contactSupport),
    );
  }

  Widget _buildPairingGate({String? refusedDeviceId}) {
    final strings = litchStrings(context);
    final status = _bleService.ownerPairingStatus;
    final state = status?.state;
    final physical = state == OwnerPairingSessionState.awaitPhysical;
    final foreignOwnerRefused = status?.foreignOwnerRefused == true;
    final busy = refusedDeviceId == null && _bleService.ownerPairingBusy;
    final quietProofUnavailable =
        state == OwnerPairingSessionState.ownedOther &&
        !foreignOwnerRefused &&
        !busy;
    final needsBoardRestart = state == OwnerPairingSessionState.failed;
    final claimLimitReached = _bleService.ownerPairingClaimLimitReached;
    final clearing = state == OwnerPairingSessionState.clearing;
    final clearPrompt = state == OwnerPairingSessionState.clearPrompt;
    final targetId = refusedDeviceId ?? _bleService.connectedId;
    final support =
        targetId != null &&
            _bleService.isConnected &&
            _bleService.connectedId == targetId &&
            !_bleService.productReady
        ? _buildSupportButton(targetId)
        : null;
    final boardName = _connectionBoardName(
      targetId,
      _pendingDestinationBoard?.id == targetId
          ? _pendingDestinationBoard!.name
          : _boards.where((board) => board.id == targetId).firstOrNull?.name,
    );
    final message = needsBoardRestart
        ? strings.ownerPairingRestartBoard
        : claimLimitReached
        ? strings.ownerPairingClaimLimit
        : switch (state) {
            OwnerPairingSessionState.awaitPhysical =>
              strings.ownerPairingPhysical,
            OwnerPairingSessionState.securing ||
            OwnerPairingSessionState.persistingOwner =>
              strings.ownerPairingConfirmed,
            OwnerPairingSessionState.ownedOther =>
              foreignOwnerRefused
                  ? strings.ownerPairingOtherOwner
                  : busy
                  ? strings.ownerPairingSecuring
                  : strings.ownerPairingFailed,
            OwnerPairingSessionState.clearPrompt =>
              strings.ownerPairingClearPrompt,
            OwnerPairingSessionState.clearing => strings.ownerPairingClearing,
            _ =>
              busy
                  ? strings.connectingToBoard('Litch')
                  : strings.ownerPairingFailed,
          };
    return Scaffold(
      key: const ValueKey('owner-pairing-gate'),
      appBar: LitchCompactAppBar(
        child: AppBar(
          automaticallyImplyLeading: false,
          title: Text(
            refusedDeviceId != null
                ? strings.boardDisconnected
                : physical || claimLimitReached || quietProofUnavailable
                ? strings.ownerPairingTitle
                : strings.boardConnecting,
          ),
        ),
      ),
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    boardName,
                    key: const ValueKey('owner-pairing-board-identity'),
                    textAlign: TextAlign.center,
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                  const SizedBox(height: 20),
                  if (physical) ...[
                    const ExcludeSemantics(
                      child: Text(
                        'X + O',
                        textAlign: TextAlign.center,
                        style: TextStyle(
                          fontSize: 36,
                          fontWeight: FontWeight.w700,
                          color: Colors.blue,
                        ),
                      ),
                    ),
                    const SizedBox(height: 20),
                  ] else if ((busy || clearing) &&
                      !needsBoardRestart &&
                      !claimLimitReached) ...[
                    const Center(child: CircularProgressIndicator()),
                    const SizedBox(height: 24),
                  ],
                  Semantics(
                    container: true,
                    liveRegion: true,
                    child: Text(
                      message,
                      textAlign: TextAlign.center,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                  ),
                  const SizedBox(height: 24),
                  if (!busy &&
                      !claimLimitReached &&
                      !needsBoardRestart &&
                      !clearing &&
                      !clearPrompt &&
                      !foreignOwnerRefused) ...[
                    FilledButton(
                      key: const ValueKey('owner-pairing-retry'),
                      style: FilledButton.styleFrom(
                        minimumSize: const Size(48, 48),
                      ),
                      onPressed: () => unawaited(_retryOwnerConfirmation()),
                      child: Text(strings.tryAgain),
                    ),
                    const SizedBox(height: 8),
                  ],
                  OutlinedButton(
                    key: const ValueKey('owner-pairing-cancel'),
                    style: OutlinedButton.styleFrom(
                      minimumSize: const Size(48, 48),
                    ),
                    onPressed: () {
                      if (refusedDeviceId != null) {
                        setState(() {
                          _dismissedOwnerRefusalDeviceId = refusedDeviceId;
                        });
                      }
                      unawaited(_cancelPairing());
                    },
                    child: Text(
                      physical || refusedDeviceId != null
                          ? strings.cancelAction
                          : strings.disconnectAction,
                    ),
                  ),
                  ?support,
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  _ConnectionMilestone _connectionMilestone(String? id) {
    if (id == null ||
        !_bleService.isConnected ||
        _bleService.connectedId != id ||
        _bleService.identifyConnectionActive) {
      return _ConnectionMilestone.waiting;
    }
    if (_bleService.productReady) return _ConnectionMilestone.ready;
    if (_bleService.ownerPairingStatus != null) {
      return _ConnectionMilestone.ownerStatus;
    }
    if (_bleService.deviceCapabilities(id).explicitlyReported) {
      return _ConnectionMilestone.identified;
    }
    return _ConnectionMilestone.linked;
  }

  ({bool pairingBlocked, bool showPairingGate, bool showProgress})
  get _connectionPresentation {
    final connectedId = _bleService.connectedId;
    final pendingUpdate = _bleService.pendingOtaVerification;
    final capabilities = _bleService.deviceCapabilities(connectedId ?? '');
    final pairingBlocked =
        !_manualDisconnectRequested &&
        !_bleService.identifyConnectionActive &&
        _bleService.isConnected &&
        capabilities.supportsOwnerPairing &&
        !_bleService.productReady &&
        pendingUpdate == null;
    final pairingStatus = _bleService.ownerPairingStatus;
    // Quiet native proof on reconnect needs no new pairing ceremony. Keep the
    // existing page visible but inert; only an actual claim/action or terminal
    // recovery state replaces it with the pairing surface.
    final showPairingGate =
        pairingBlocked &&
        (pairingStatus?.claimPending == true ||
            pairingStatus?.state == OwnerPairingSessionState.clearPrompt ||
            pairingStatus?.state == OwnerPairingSessionState.clearing ||
            pairingStatus?.state == OwnerPairingSessionState.failed ||
            !_bleService.ownerPairingBusy);
    final refusedDeviceId = _bleService.ownerPairingRefusedDeviceId;
    final quietVerification = pairingBlocked && !showPairingGate;
    final showProgress =
        !_manualDisconnectRequested &&
        !_discoveryBlocked &&
        (!_retainingCraft || quietVerification) &&
        !showPairingGate &&
        pendingUpdate == null &&
        (_isConnecting ||
            _pendingDestinationBoard != null ||
            quietVerification ||
            _waitingForBoard) &&
        _bleService.pairingRecovery == null &&
        (refusedDeviceId == null ||
            refusedDeviceId == _dismissedOwnerRefusalDeviceId);
    return (
      pairingBlocked: pairingBlocked,
      showPairingGate: showPairingGate,
      showProgress: showProgress,
    );
  }

  @override
  Widget build(BuildContext context) {
    final connectedId = _bleService.connectedId;
    final presentation = _connectionPresentation;
    final content = _buildConnectionContent(
      context,
      pairingBlocked: presentation.pairingBlocked,
      showPairingGate: presentation.showPairingGate,
      showProgress: presentation.showProgress,
    );
    final targetId =
        _reconnectTargetId ??
        _pendingDestinationBoard?.id ??
        connectedId ??
        _selectedBoardId;
    final board =
        _pendingDestinationBoard ??
        _bleService.discoveredBoard(targetId ?? '') ??
        _boards.where((board) => board.id == targetId).firstOrNull;
    final name = _connectionBoardName(targetId, board?.name);
    // A gate mounted over an existing link can show quiet owner proof without
    // having initiated a connection choice. Its report returns to that proof.
    final selectedJourney =
        _connectionChoiceGeneration > 0 &&
        _awaitingDestination &&
        targetId != null &&
        targetId == _reconnectTargetId;
    final milestone = _connectionMilestone(targetId);
    final sessionToken = _bleService.connectionSessionToken;
    // A usable Craft canvas owns reconnect feedback. Quiet owner verification
    // keeps the gate's accessible progress and Cancel above locked controls.
    return Stack(
      fit: StackFit.expand,
      children: [
        content,
        if (presentation.showProgress)
          SafeArea(
            child: _BoardConnectionProgress(
              key: ValueKey(targetId),
              status: name.isEmpty
                  ? litchStrings(context).boardConnecting
                  : litchStrings(context).connectingToBoard(name),
              hint: _connectionEnd?.nativeDrop == true
                  ? litchStrings(context).connectionRetrySingleDeviceHelp
                  : null,
              retryEnabled: _canRetryWaitingConnection(targetId),
              milestone: milestone,
              sessionToken: sessionToken,
              onCancel: () => unawaited(_disconnect(returnToBoardList: true)),
              onRetry: () =>
                  _restartWaitingConnection(targetId, sessionToken, milestone),
              support: _buildSupportButton(
                targetId,
                endJourney: selectedJourney,
              ),
            ),
          ),
      ],
    );
  }

  Widget _buildConnectionContent(
    BuildContext context, {
    required bool pairingBlocked,
    required bool showPairingGate,
    required bool showProgress,
  }) {
    final connectionBlocksInput = showProgress && !_retainingCraft;
    final pairingRecovery = _bleService.pairingRecovery;
    if (pairingRecovery != null) {
      return BluetoothPairingRecoveryScreen(
        key: ValueKey('pairing-recovery-${pairingRecovery.deviceId}'),
        state: pairingRecovery,
        boardName: _connectionBoardName(
          pairingRecovery.deviceId,
          _boards
              .where((board) => board.id == pairingRecovery.deviceId)
              .map((board) => board.name)
              .firstOrNull,
        ),
        support: _buildSupportButton(pairingRecovery.deviceId),
        onRepair: _bleService.repairPhonePairing,
        onRetry: _bleService.retryPairingConnection,
        onOpenSettings: _bleService.openSystemBluetoothSettings,
        onExit: () => unawaited(() async {
          final choice = ++_connectionChoiceGeneration;
          _manualDisconnectRequested = true;
          _hasConnectedSession = false;
          _selectedBoardId = null;
          await _bleService.dismissPairingRecovery();
          if (mounted && choice == _connectionChoiceGeneration) {
            setState(() => _isConnecting = false);
            if (_isAppForeground) unawaited(_bleService.startScan());
          }
        }()),
      );
    }
    final refusedDeviceId = _bleService.ownerPairingRefusedDeviceId;
    if (refusedDeviceId != null &&
        refusedDeviceId != _dismissedOwnerRefusalDeviceId) {
      return _buildPairingGate(refusedDeviceId: refusedDeviceId);
    }
    final connectedId = _bleService.connectedId;
    // A firmware update owns its reboot interval and must preserve the update
    // surface. Fresh connections keep discovery visible until current-link
    // mode is known; the mounted main shell owns that inspection offstage.
    final pendingUpdate = _bleService.pendingOtaVerification;
    final String? updatingBoardId =
        pendingUpdate != null &&
            _hasConnectedSession &&
            !_manualDisconnectRequested &&
            !_discoveryBlocked &&
            (_selectedBoardId ?? _reconnectTargetId) != null
        ? (_selectedBoardId ?? _reconnectTargetId)
        : null;
    final String? reconnectingBoardId =
        _hasConnectedSession &&
            !_manualDisconnectRequested &&
            !_discoveryBlocked &&
            (_selectedDestination != 1 || _pendingDestinationBoard != null)
        ? (_reconnectTargetId ?? _selectedBoardId)
        : null;
    final String? mainBoardId =
        _bleService.isConnected &&
            !_bleService.identifyConnectionActive &&
            !_manualDisconnectRequested
        ? connectedId
        : updatingBoardId ?? reconnectingBoardId;
    // The existing shell owns the offline canvas and Console's draft/history.
    // An empty ID supplies no board target; transport uses the connected ID.
    final shellBoardId =
        mainBoardId ??
        (_selectedDestination == 0 || _enableAppConsole
            ? _selectedBoardId ?? _lastConnectedBoardId ?? ''
            : null);
    Widget? main;
    if (shellBoardId != null) {
      final bool resolvingConnection =
          _isConnecting ||
          !_bleService.isConnected ||
          _bleService.identifyConnectionActive ||
          !_connectedDestinationResolved ||
          pairingBlocked;
      main = LitchMainScaffold(
        key: _mainScaffoldKey,
        bleService: _bleService,
        boardId: shellBoardId,
        boards: <LitchBoard>[
          ..._boards,
          if (_pendingDestinationBoard case final pending?)
            if (!_boards.any((board) => board.id == pending.id)) pending,
        ],
        onDisconnect: _disconnect,
        craftController: _craftController,
        craftLibrary: _craftLibrary,
        initialPageIndex: _selectedDestination,
        initialCraftModeResolved: !_connectedDestinationNeedsScaffoldSync,
        modeResolving: resolvingConnection,
        connectionPending: _isConnecting || _pendingDestinationBoard != null,
        ownerVerificationPending: pairingBlocked,
        retainDuringReconnect: !_bleService.isConnected,
        onModeResolved: _onScaffoldModeResolved,
        onPageSelected: (index) {
          if (_manualDisconnectRequested && _selectedDestination != 0) return;
          setState(() {
            _selectedDestination = index;
            _pendingDestinationBoard = null;
          });
        },
        firmwarePageBuilder: widget.firmwarePageBuilder,
        appearanceMode: widget.appearanceMode,
        onAppearanceModeChanged: widget.onAppearanceModeChanged,
        gameLibrary: widget.gameLibrary,
        gameLibraryOpener: widget.gameLibraryOpener,
        assignPlayer: widget.assignPlayer,
        enableAppConsole: _enableAppConsole,
        saveAppConsolePreference: _saveAppConsolePreference,
      );
      if (OtaBenchmarkRunner.enabled && !resolvingConnection) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (!mounted ||
              _otaBenchmarkEntryStarted ||
              _otaBenchmarkEntry != null ||
              !OtaBenchmarkRunner.targetReady(_bleService)) {
            return;
          }
          final scaffold = _mainScaffoldKey.currentState;
          if (scaffold == null) return;
          _otaBenchmarkEntryStarted = true;
          _otaBenchmarkEntry = scaffold
              ._runOtaBenchmarkFromFirmware(widget.appTrace)
              .whenComplete(() => _otaBenchmarkEntry = null);
        });
      }
    }
    final showDiscovery =
        main == null ||
        (mainBoardId == null &&
            _selectedDestination != 0 &&
            _selectedDestination != 4) ||
        (!showPairingGate &&
            pendingUpdate == null &&
            _selectedDestination == 1 &&
            _pendingDestinationBoard != null);
    // Keep discovery in the same keyed slot when the mode owner mounts. Its
    // board choices, saved games and scroll position survive the whole attempt.
    // Pairing changes access, not the lifetime of retained product pages.
    return Stack(
      fit: StackFit.expand,
      children: [
        if (main != null)
          ExcludeFocus(
            excluding: pairingBlocked || connectionBlocksInput,
            child: ExcludeSemantics(
              excluding: pairingBlocked || connectionBlocksInput,
              child: AbsorbPointer(
                absorbing: pairingBlocked || connectionBlocksInput,
                child: TickerMode(
                  enabled: !showPairingGate && !showDiscovery,
                  child: Offstage(
                    offstage: showPairingGate || showDiscovery,
                    child: main,
                  ),
                ),
              ),
            ),
          ),
        if (showDiscovery)
          KeyedSubtree(
            key: const ValueKey('connection-discovery-underlay'),
            child: ExcludeFocus(
              excluding: pairingBlocked,
              child: ExcludeSemantics(
                excluding: pairingBlocked,
                child: AbsorbPointer(
                  absorbing: pairingBlocked,
                  child: _buildDisconnectedContent(
                    context,
                    showProgress: showProgress,
                  ),
                ),
              ),
            ),
          ),
        if (showPairingGate) _buildPairingGate(),
      ],
    );
  }

  Widget _disconnectedScaffold({
    required PreferredSizeWidget? appBar,
    required Widget body,
    required bool compactLandscape,
  }) {
    final tablet = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    final readingPane = _selectedDestination != 0;
    return Scaffold(
      appBar: appBar,
      body: SafeArea(
        top: false,
        left: tablet,
        right: tablet,
        bottom: false,
        child: Center(
          child: SizedBox(
            width: tablet && readingPane
                ? LitchWorkspaceGeometry.readableContentWidth
                : double.infinity,
            height: double.infinity,
            child: body,
          ),
        ),
      ),
      bottomNavigationBar: compactLandscape
          ? null
          : _buildDisconnectedNavigation(),
    );
  }

  Widget _buildDisconnectedContent(
    BuildContext context, {
    required bool showProgress,
  }) {
    final tablet = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    if (_selectedDestination == 0) {
      final media = MediaQuery.of(context);
      final landscape = LitchWorkspaceGeometry.usesCompactLandscape(
        media.size,
        Size(
          media.size.width - media.padding.horizontal,
          media.size.height - media.padding.vertical,
        ),
      );
      return _disconnectedScaffold(
        compactLandscape: landscape,
        appBar: landscape
            ? null
            : LitchCompactAppBar(
                child: _mainBrandAppBar(
                  context,
                  onLoadVersionArtwork: _loadVersionArtwork,
                  litchcraft: true,
                  connecting: showProgress && _pendingDestinationBoard == null,
                  status: _BoardStatusLabel(
                    connecting: showProgress,
                    onChooseBoard: () => unawaited(_disconnect()),
                  ),
                ),
              ),
        body: SafeArea(
          top: landscape,
          bottom: landscape,
          child: LitchcraftScreen(
            controller: _craftController,
            library: _craftLibrary,
            rootSafeInsets: landscape || tablet ? media.viewPadding : null,
            landscapeFooterHeight: LitchWorkspaceGeometry.navigationHeight,
            landscapeFooter: landscape
                ? _buildDisconnectedNavigation(
                    compact: true,
                    embeddedToolPanel: true,
                    currentDestination: 0,
                  )
                : null,
          ),
        ),
      );
    }

    if (_selectedDestination == 2) {
      final onLeft =
          OrientationWorkspaceScope.maybeOf(context)?.side ==
          DeviceLandscapeSide.left;
      final MediaQueryData media = MediaQuery.of(context);
      final Size usableSize = Size(
        media.size.width - media.padding.horizontal,
        media.size.height - media.padding.vertical,
      );
      final bool landscape = LitchWorkspaceGeometry.usesCompactLandscape(
        media.size,
        usableSize,
      );
      return _disconnectedScaffold(
        compactLandscape: landscape,
        appBar: landscape
            ? null
            : LitchCompactAppBar(
                child: _mainBrandAppBar(
                  context,
                  onLoadVersionArtwork: _loadVersionArtwork,
                  litchcraft: false,
                  connecting: showProgress && _pendingDestinationBoard == null,
                  status: _BoardStatusLabel(
                    connecting: showProgress,
                    onChooseBoard: () => unawaited(_disconnect()),
                  ),
                ),
              ),
        body: SafeArea(
          top: landscape,
          bottom: landscape,
          child: Row(
            textDirection: onLeft ? TextDirection.rtl : TextDirection.ltr,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Expanded(child: _buildOfflineSetup(landscape: landscape)),
              if (landscape)
                _buildDisconnectedLandscapeRail(
                  keyName: 'disconnected-setup-landscape-action-rail',
                  currentDestination: 2,
                  onLeft: onLeft,
                ),
            ],
          ),
        ),
      );
    }

    // A roster from the closed session is stale until the new scan responds.
    final discoveredBoards = _awaitingFreshDiscovery
        ? const <LitchBoard>[]
        : _boards;
    final availableBoards = <LitchBoard>[
      ...discoveredBoards,
      if (_pendingDestinationBoard case final pending?)
        if (!discoveredBoards.any((board) => board.id == pending.id)) pending,
    ];
    final connecting =
        showProgress || _isConnecting || _pendingDestinationBoard != null;
    // Once chosen, present that real target consistently while scan cleanup
    // changes the available roster. Cancel restores the current manual choices.
    final boards = connecting && _pendingDestinationBoard != null
        ? availableBoards
              .where((board) => board.id == _pendingDestinationBoard!.id)
              .toList()
        : availableBoards;
    final showDiscoverySupport = !connecting && _showDiscoverySupport;
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final MediaQueryData media = MediaQuery.of(context);
        final textScale = media.textScaler.scale(1);
        final cardWidth = tablet
            ? math.min(
                    LitchWorkspaceGeometry.readableContentWidth,
                    constraints.maxWidth - media.padding.horizontal,
                  ) -
                  32
            : constraints.maxWidth;
        final bool compactLargeText =
            textScale > 1.3 &&
            (tablet ? cardWidth / textScale : cardWidth) <= 480;
        final bool compactBoardCards = cardWidth <= 420 || compactLargeText;
        final bool stackBoardActions = cardWidth <= 360 || compactLargeText;
        final Size usableSize = Size(
          constraints.maxWidth - media.padding.horizontal,
          constraints.maxHeight - media.padding.vertical,
        );
        final bool landscape =
            !tablet &&
            usableSize.width >= 480 &&
            usableSize.width > usableSize.height;
        Widget buildBoardCard(LitchBoard? board, int index) {
          Widget label(
            String value, {
            TextStyle? style,
            int? maxLines,
            TextOverflow? overflow,
          }) {
            if (board != null) {
              return Text(
                value,
                style: style,
                maxLines: maxLines,
                overflow: overflow,
              );
            }
            return Builder(
              builder: (context) => RichText(
                text: TextSpan(
                  text: value,
                  style: DefaultTextStyle.of(context).style.merge(style),
                ),
                textScaler: MediaQuery.textScalerOf(context),
                locale: Localizations.localeOf(context),
                maxLines: maxLines,
                overflow: overflow ?? TextOverflow.clip,
              ),
            );
          }

          final int identifyNumber = index + 1;
          final String boardName = board == null
              ? List.filled(
                  _ConnectedBoardSheetState._friendlyNameCharacters,
                  'W',
                ).join()
              : _connectionBoardName(board.id, board.name);
          final String boardIdentity = litchStrings(
            context,
          ).boardIdentity(board == null ? '8888' : _shortLitchId(board.id));
          final VoidCallback? identify =
              board == null || connecting || _isIdentifying
              ? null
              : () => _identifyBoard(board, identifyNumber);
          final VoidCallback? connect = board == null || connecting
              ? null
              : () => _connectToBoard(board);
          final Widget identifyButton = OutlinedButton(
            onPressed: identify,
            style: OutlinedButton.styleFrom(minimumSize: const Size(0, 48)),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                if (board != null &&
                    _identifyingBoardId == board.id) ...<Widget>[
                  const SizedBox.square(
                    dimension: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                  const SizedBox(width: 8),
                ],
                Flexible(
                  child: label(
                    litchStrings(context).identifyAction,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
              ],
            ),
          );
          final Widget connectButton = FilledButton(
            onPressed: connect,
            style: FilledButton.styleFrom(minimumSize: const Size(0, 48)),
            child: label(litchStrings(context).connectAction),
          );
          if (landscape) {
            return Card(
              key: board == null
                  ? null
                  : ValueKey<String>('discovered-board-${board.id}'),
              child: Padding(
                padding: const EdgeInsets.all(14),
                child: Row(
                  children: [
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          label(
                            boardName,
                            style: Theme.of(context).textTheme.titleMedium
                                ?.copyWith(fontSize: 14, height: 1.2),
                          ),
                          const SizedBox(height: 4),
                          label('$boardIdentity · #$identifyNumber'),
                        ],
                      ),
                    ),
                    const SizedBox(width: 16),
                    SizedBox(
                      width: MediaQuery.textScalerOf(context).scale(1) >= 1.5
                          ? 192
                          : 144,
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          identifyButton,
                          const SizedBox(height: 8),
                          connectButton,
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            );
          }
          if (compactBoardCards) {
            return Card(
              key: board == null
                  ? null
                  : ValueKey<String>('discovered-board-${board.id}'),
              child: Padding(
                padding: const EdgeInsets.all(14),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    label(
                      boardName,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 4),
                    label('$boardIdentity · #$identifyNumber'),
                    const SizedBox(height: 12),
                    if (stackBoardActions) ...<Widget>[
                      SizedBox(width: double.infinity, child: connectButton),
                      const SizedBox(height: 8),
                      SizedBox(width: double.infinity, child: identifyButton),
                    ] else
                      Row(
                        children: <Widget>[
                          Expanded(child: identifyButton),
                          const SizedBox(width: 8),
                          Expanded(child: connectButton),
                        ],
                      ),
                  ],
                ),
              ),
            );
          }
          return Card(
            key: board == null
                ? null
                : ValueKey<String>('discovered-board-${board.id}'),
            child: ListTile(
              title: label(boardName),
              subtitle: label('$boardIdentity · #$identifyNumber'),
              trailing: Wrap(
                spacing: 8,
                children: <Widget>[identifyButton, connectButton],
              ),
            ),
          );
        }

        final strings = litchStrings(context);
        final onLeft =
            OrientationWorkspaceScope.maybeOf(context)?.side ==
            DeviceLandscapeSide.left;
        Widget layoutText(String value, TextStyle? style) => RichText(
          text: TextSpan(
            text: value,
            style: DefaultTextStyle.of(context).style.merge(style),
          ),
          textScaler: MediaQuery.textScalerOf(context),
          locale: Localizations.localeOf(context),
        );
        // Measure the existing one-board layout before a radio identity exists.
        // This is never a discovered board, a painted placeholder or an action.
        // The bounded title uses the same name limit as the actual name editor.
        Widget statusFootprint(List<String> labels, {bool scanning = false}) =>
            Row(
              children: [
                if (scanning) const SizedBox(width: 28, height: 18),
                Expanded(
                  child: Stack(
                    children: [
                      for (final text in labels)
                        layoutText(
                          text,
                          landscape
                              ? const TextStyle(fontSize: 12, height: 1.2)
                              : Theme.of(context).textTheme.titleMedium,
                        ),
                    ],
                  ),
                ),
                if (landscape) const SizedBox.square(dimension: 48),
              ],
            );
        final stageFootprint = ExcludeFocus(
          child: ExcludeSemantics(
            child: IgnorePointer(
              child: Opacity(
                key: const ValueKey('discovery-layout-guide'),
                opacity: 0,
                child: Stack(
                  children: [
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        statusFootprint([
                          strings.scanningBoards,
                          strings.scanningPrevious,
                          strings.disconnectedChoose,
                          strings.disconnectedScanning,
                          strings.connectionLostWaiting,
                        ], scanning: true),
                        const SizedBox(height: 12),
                        if (!landscape)
                          TextButton(
                            onPressed: null,
                            child: Builder(
                              builder: (context) => RichText(
                                text: TextSpan(
                                  text: strings.findBoardHelp,
                                  style: DefaultTextStyle.of(context).style,
                                ),
                                textScaler: MediaQuery.textScalerOf(context),
                                locale: Localizations.localeOf(context),
                              ),
                            ),
                          ),
                        const SizedBox(height: 12),
                      ],
                    ),
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        statusFootprint([
                          strings.previousBoardFound,
                          strings.oneBoardFound,
                          strings.multipleBoardsFound,
                        ]),
                        const SizedBox(height: 24),
                        buildBoardCard(null, 0),
                      ],
                    ),
                  ],
                ),
              ),
            ),
          ),
        );
        return _disconnectedScaffold(
          compactLandscape: landscape,
          appBar: landscape
              ? null
              : LitchCompactAppBar(
                  child: _mainBrandAppBar(
                    context,
                    onLoadVersionArtwork: _loadVersionArtwork,
                    litchcraft: false,
                    connecting: connecting,
                    status: _BoardStatusLabel(connecting: connecting),
                  ),
                ),
          body: SafeArea(
            top: landscape,
            child: Row(
              textDirection: onLeft ? TextDirection.rtl : TextDirection.ltr,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                Expanded(
                  child: Padding(
                    key: const ValueKey<String>('connection-gate-content'),
                    padding: const EdgeInsets.all(16),
                    child: LayoutBuilder(
                      builder: (context, bodyBounds) => _buildDiscoveryScrollView(
                        child: ConstrainedBox(
                          constraints: BoxConstraints(
                            minHeight: landscape ? bodyBounds.maxHeight : 0,
                          ),
                          child: Column(
                            mainAxisAlignment: landscape
                                ? MainAxisAlignment.center
                                : MainAxisAlignment.start,
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: <Widget>[
                              Stack(
                                key: const ValueKey(
                                  'discovery-connection-stage',
                                ),
                                children: [
                                  stageFootprint,
                                  Column(
                                    crossAxisAlignment:
                                        CrossAxisAlignment.stretch,
                                    children: [
                                      Row(
                                        crossAxisAlignment:
                                            CrossAxisAlignment.center,
                                        children: <Widget>[
                                          Expanded(
                                            child: Semantics(
                                              key: const ValueKey<String>(
                                                'connection-status-live-region',
                                              ),
                                              container: true,
                                              liveRegion: !showProgress,
                                              label: !showProgress
                                                  ? _statusText
                                                  : '',
                                              excludeSemantics: true,
                                              child: Row(
                                                crossAxisAlignment:
                                                    CrossAxisAlignment.center,
                                                children: <Widget>[
                                                  if (!showProgress &&
                                                      !_discoveryBlocked &&
                                                      boards
                                                          .isEmpty) ...<Widget>[
                                                    const SizedBox.square(
                                                      dimension: 18,
                                                      child:
                                                          CircularProgressIndicator(
                                                            strokeWidth: 2,
                                                          ),
                                                    ),
                                                    const SizedBox(width: 10),
                                                  ],
                                                  Expanded(
                                                    child: Opacity(
                                                      opacity: !showProgress
                                                          ? 1
                                                          : 0,
                                                      child: Text(
                                                        _statusText,
                                                        style: landscape
                                                            ? const TextStyle(
                                                                fontSize: 12,
                                                                height: 1.2,
                                                              )
                                                            : Theme.of(context)
                                                                  .textTheme
                                                                  .titleMedium,
                                                      ),
                                                    ),
                                                  ),
                                                ],
                                              ),
                                            ),
                                          ),
                                          if (landscape && showDiscoverySupport)
                                            IconButton(
                                              key: const ValueKey(
                                                'discovery-support',
                                              ),
                                              tooltip: litchStrings(
                                                context,
                                              ).findBoardHelp,
                                              onPressed: () =>
                                                  SupportReportScreen.open(
                                                    context,
                                                    bleService: _bleService,
                                                  ),
                                              style: IconButton.styleFrom(
                                                minimumSize: const Size(48, 48),
                                              ),
                                              icon: const Icon(
                                                Icons.help_outline,
                                              ),
                                            ),
                                          if (landscape && boards.length > 1)
                                            IconButton(
                                              tooltip: litchStrings(
                                                context,
                                              ).identifyAllAction,
                                              onPressed:
                                                  connecting || _isIdentifying
                                                  ? null
                                                  : _identifyAllForThreeSeconds,
                                              style: IconButton.styleFrom(
                                                minimumSize: const Size(48, 48),
                                              ),
                                              icon: const Icon(
                                                Icons.lightbulb_outline_rounded,
                                              ),
                                            ),
                                        ],
                                      ),
                                      if (_isIdentifying &&
                                          !_isConnecting) ...<Widget>[
                                        const SizedBox(height: 12),
                                        const LinearProgressIndicator(),
                                      ],
                                      const SizedBox(height: 12),
                                      if (showDiscoverySupport && !landscape)
                                        Align(
                                          alignment: Alignment.centerLeft,
                                          child: TextButton(
                                            key: const ValueKey<String>(
                                              'discovery-support',
                                            ),
                                            onPressed: () =>
                                                SupportReportScreen.open(
                                                  context,
                                                  bleService: _bleService,
                                                ),
                                            child: Text(
                                              litchStrings(
                                                context,
                                              ).findBoardHelp,
                                            ),
                                          ),
                                        ),
                                      if (boards.length > 1 && !landscape)
                                        Align(
                                          alignment: Alignment.centerRight,
                                          child: OutlinedButton.icon(
                                            onPressed:
                                                connecting || _isIdentifying
                                                ? null
                                                : _identifyAllForThreeSeconds,
                                            style: OutlinedButton.styleFrom(
                                              minimumSize: const Size(0, 48),
                                            ),
                                            icon: _identifyingAll
                                                ? const SizedBox.square(
                                                    dimension: 18,
                                                    child:
                                                        CircularProgressIndicator(
                                                          strokeWidth: 2,
                                                        ),
                                                  )
                                                : const Icon(
                                                    Icons
                                                        .lightbulb_outline_rounded,
                                                  ),
                                            label: Text(
                                              litchStrings(
                                                context,
                                              ).identifyAllAction,
                                            ),
                                          ),
                                        ),
                                      if (_discoveryBlocked)
                                        Align(
                                          alignment: Alignment.centerLeft,
                                          child: FilledButton(
                                            key: const ValueKey<String>(
                                              'bluetooth-recovery',
                                            ),
                                            onPressed: _recoverBluetoothAccess,
                                            style: FilledButton.styleFrom(
                                              minimumSize: const Size(0, 48),
                                            ),
                                            child: Text(
                                              _bleService
                                                      .availabilityRecoveryNeedsSettings
                                                  ? litchStrings(
                                                      context,
                                                    ).openSettings
                                                  : litchStrings(
                                                      context,
                                                    ).tryAgain,
                                            ),
                                          ),
                                        ),
                                      const SizedBox(height: 12),
                                      if (!_discoveryBlocked)
                                        boards.isEmpty
                                            ? const SizedBox.shrink()
                                            : ListView.builder(
                                                shrinkWrap: true,
                                                physics:
                                                    const NeverScrollableScrollPhysics(),
                                                itemCount: boards.length,
                                                itemBuilder: (context, index) =>
                                                    buildBoardCard(
                                                      boards[index],
                                                      connecting &&
                                                              _pendingDestinationBoard !=
                                                                  null
                                                          ? _pendingDestinationBoardNumber -
                                                                1
                                                          : index,
                                                    ),
                                              ),
                                    ],
                                  ),
                                ],
                              ),
                              if (!landscape &&
                                  (widget.gameLibrary != null ||
                                      widget.gameLibraryOpener !=
                                          null)) ...<Widget>[
                                const SizedBox(height: 16),
                                RecentGamesSection(
                                  key: const ValueKey('discovery-recent-games'),
                                  repository: widget.gameLibrary,
                                  openRepository: widget.gameLibraryOpener,
                                  onOpenAll: _openGameLibrary,
                                ),
                              ],
                              if (landscape &&
                                  (widget.gameLibrary != null ||
                                      widget.gameLibraryOpener != null))
                                Align(
                                  alignment: Alignment.centerLeft,
                                  child: TextButton.icon(
                                    key: const ValueKey(
                                      'discovery-review-games',
                                    ),
                                    onPressed: _openGameLibrary,
                                    style: TextButton.styleFrom(
                                      minimumSize: const Size(48, 48),
                                    ),
                                    icon: const Icon(Icons.history),
                                    label: Text(
                                      litchStrings(context).recentGamesTitle,
                                      style: const TextStyle(fontSize: 12),
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
                if (landscape)
                  _buildDisconnectedLandscapeRail(
                    keyName: 'disconnected-play-landscape-action-rail',
                    currentDestination: 1,
                    onLeft: onLeft,
                  ),
              ],
            ),
          ),
        );
      },
    );
  }
}

class LitchMainScaffold extends StatefulWidget {
  const LitchMainScaffold({
    super.key,
    required this.bleService,
    required this.boardId,
    required this.boards,
    required this.onDisconnect,
    this.craftController,
    this.craftLibrary,
    this.versionTapClock,
    this.initialPageIndex = 1,
    this.initialCraftModeResolved = false,
    this.modeResolving = false,
    this.connectionPending = false,
    this.ownerVerificationPending = false,
    this.retainDuringReconnect = false,
    this.onModeResolved,
    this.onPageSelected,
    this.loadAppConsolePreference,
    this.saveAppConsolePreference,
    this.enableAppConsole,
    this.firmwarePageBuilder,
    this.appearanceMode = LitchAppearanceMode.system,
    this.onAppearanceModeChanged,
    this.gameLibrary,
    this.gameLibraryOpener,
    this.assignPlayer,
    this.landscapeRibbonSide,
    this.deviceLandscapeSideReader,
  });

  final LitchcraftController? craftController;
  final LitchcraftLibrary? craftLibrary;
  final DateTime Function()? versionTapClock;
  final Alignment? landscapeRibbonSide;
  final Future<DeviceLandscapeSide?> Function()? deviceLandscapeSideReader;
  final int initialPageIndex;
  final bool initialCraftModeResolved;
  final bool modeResolving;
  final bool connectionPending;
  final bool ownerVerificationPending;
  final bool retainDuringReconnect;
  final VoidCallback? onModeResolved;
  final ValueChanged<int>? onPageSelected;
  final LitchBleService bleService;
  final String boardId;
  final List<LitchBoard> boards;
  final Future<void> Function() onDisconnect;
  final AppConsolePreferenceLoader? loadAppConsolePreference;
  final AppConsolePreferenceSaver? saveAppConsolePreference;
  // A gate-controlled preference shares one value across discovery and shell.
  // Standalone shell hosts retain their existing loader/saver boundary.
  final bool? enableAppConsole;
  final FirmwarePageBuilder? firmwarePageBuilder;
  final LitchAppearanceMode appearanceMode;
  final Future<void> Function(LitchAppearanceMode mode)?
  onAppearanceModeChanged;
  final GameLibraryRepository? gameLibrary;
  final GameLibraryOpener? gameLibraryOpener;
  final LocalPlayerAssignment? assignPlayer;

  @override
  State<LitchMainScaffold> createState() => _LitchMainScaffoldState();
}

class _LitchMainScaffoldState extends State<LitchMainScaffold>
    with WidgetsBindingObserver {
  final HomeNoticePresenter _notices = HomeNoticePresenter();
  final GlobalKey<ScaffoldMessengerState> _mainMessengerKey =
      GlobalKey<ScaffoldMessengerState>();
  final GlobalKey _boardMenuAnchorKey = GlobalKey();

  late final PageController _mainPageController = PageController(
    initialPage: _navigationIndexForPage(widget.initialPageIndex),
  );
  late final LitchcraftController _craftController;
  late final LitchcraftLibrary _craftLibrary;
  late final StreamSubscription<ConnectionStateUpdate> _craftConnection;
  Future<bool>? _craftDeparture;
  int _navigationIntent = 0;
  bool _craftDepartureRequested = false;
  int _craftReentries = 0;
  bool _craftDepartureInhibitsMode = false;
  int _craftModeSynchronizations = 0;
  int _craftModeSynchronizationGeneration = 0;
  Completer<void>? _craftModeSynchronizationIdle;
  String? _craftConnectionProbe;
  String? _craftModeSynchronizationProbe;
  int _mainPageSelectionRevision = 0;
  int _craftModeSynchronizationSelectionRevision = 0;
  String? _craftModeResolvedProbe;
  String? _passiveCraftDestinationProbe;
  bool _coldDestinationPending = false;
  bool _homeMounted = false;
  int _currentIndex = 1;
  int? _firmwareOriginPage;
  int _firmwareRefreshEpoch = 0;
  int _settingsHealthRefreshEpoch = 0;
  int _settingsHealthFocusEpoch = 0;
  int _settingsOverviewRequestEpoch = 0;
  final ValueNotifier<bool> _healthAttention = ValueNotifier<bool>(false);
  bool _updateInProgress = false;
  OtaBenchmarkProgress? _otaBenchmarkProgress;
  int _otaBenchmarkInvocation = 0;
  bool _recoveryOnly = false;
  bool _enableAppConsole = false;
  bool _boardMenuOpen = false;
  Route<void>? _boardMenuRoute;
  FlutterView? _activeView;
  Size? _lastViewPhysicalSize;
  FirmwareUpdateAvailability _firmwareAvailability =
      const FirmwareUpdateAvailability();
  final GlobalKey<AppConsoleScreenState> _appConsoleScreenKey =
      GlobalKey<AppConsoleScreenState>();
  final GlobalKey<SettingsScreenState> _settingsScreenKey =
      GlobalKey<SettingsScreenState>();
  late Widget _homePage;
  EdgeInsets _rootSafeInsets = EdgeInsets.zero;
  late Widget _settingsPage;
  double _settingsOverlayClearance = 0;
  late Widget _firmwarePage;
  late Widget _appConsolePage;
  DeviceLandscapeSide _landscapeSide = DeviceLandscapeSide.right;
  DeviceLandscapeSide? _workspaceSide;
  bool _workspaceOwnsSide = false;
  int _landscapeSideGeneration = 0;
  Alignment get _landscapeRibbonSide =>
      widget.landscapeRibbonSide ??
      ((_workspaceSide ?? _landscapeSide) == DeviceLandscapeSide.left
          ? Alignment.centerLeft
          : Alignment.centerRight);

  HomeScreen _buildHomePage() => HomeScreen(
    rootSafeInsets: _rootSafeInsets,
    landscapeFooterClearance: LitchWorkspaceGeometry.navigationHeight + 16,
    landscapeDetailsOnLeft: _landscapeRibbonSide.x < 0,
    bleService: widget.bleService,
    boardId: widget.boardId,
    notices: _notices,
    onHealthAttentionChanged: _onHealthAttentionChanged,
    gameLibrary: widget.gameLibrary,
    gameLibraryOpener: widget.gameLibraryOpener,
    assignPlayer: widget.assignPlayer,
    onChoosePlayerColor: ({required whitePiece, required p1AtOppositeEnd}) {
      unawaited(
        _settingsScreenKey.currentState?.showPlayerColorPicker(
          whitePiece: whitePiece,
          p1AtOppositeEnd: p1AtOppositeEnd,
        ),
      );
    },
    onOpenGameLibrary:
        widget.gameLibrary != null || widget.gameLibraryOpener != null
        ? _openGameLibrary
        : null,
  );

  void _readLandscapeSide() {
    if (widget.landscapeRibbonSide != null || _workspaceOwnsSide) return;
    final generation = ++_landscapeSideGeneration;
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      if (!mounted || _workspaceOwnsSide) return;
      final side =
          await (widget.deviceLandscapeSideReader?.call() ??
              const DeviceLandscapeSideReader().read());
      if (!mounted ||
          generation != _landscapeSideGeneration ||
          _workspaceOwnsSide ||
          side == null ||
          side == _landscapeSide) {
        return;
      }
      setState(() {
        _landscapeSide = side;
        _homePage = _buildHomePage();
      });
    });
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _recoveryOnly = widget.bleService
        .deviceCapabilities(widget.boardId)
        .recoveryOnly;
    _craftController =
        widget.craftController ?? LitchcraftController(widget.bleService);
    _craftLibrary = widget.craftLibrary ?? LitchcraftLibrary(_craftController);
    _craftController.setDraftReadiness(_craftLibrary.ready);
    _craftController.addListener(_onCraftAuthorityChanged);
    _craftConnection = widget.bleService.connectionStream.listen(
      _onCraftConnectionChanged,
    );
    if (widget.initialCraftModeResolved && !_recoveryOnly) {
      _craftConnectionProbe =
          '${widget.boardId}:${widget.bleService.connectionSessionToken}';
    }
    _currentIndex = widget.initialPageIndex;
    _enableAppConsole = widget.enableAppConsole ?? false;
    if (_recoveryOnly) _currentIndex = 2;
    if (widget.bleService.pendingOtaVerification != null) {
      if (_recoveryOnly) _firmwareOriginPage = 2;
      _currentIndex = 3;
      _updateInProgress = true;
    }
    _coldDestinationPending =
        !widget.initialCraftModeResolved &&
        !widget.retainDuringReconnect &&
        _currentIndex == 1;
    _homeMounted = _currentIndex == 1 && !_coldDestinationPending;
    _homePage = _buildHomePage();
    _readLandscapeSide();
    _settingsPage = _buildSettingsPage();
    _firmwarePage = _buildFirmwarePage();
    _appConsolePage = AppConsoleScreen(
      key: _appConsoleScreenKey,
      bleService: widget.bleService,
      boardId: widget.boardId,
      notices: _notices,
    );
    if (_currentIndex == 4) _activateConsoleAfterFrame();
    _loadAppConsolePreference();
    if (!widget.initialCraftModeResolved || _recoveryOnly) {
      unawaited(_synchronizeConnectedCraftMode());
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    unawaited(_craftConnection.cancel());
    _craftController.removeListener(_onCraftAuthorityChanged);
    if (widget.craftController == null) {
      unawaited(
        _craftLibrary.flush().whenComplete(() {
          _craftLibrary.dispose();
          _craftController.dispose();
        }),
      );
    }
    _mainPageController.dispose();
    _healthAttention.dispose();
    super.dispose();
  }

  @override
  void deactivate() {
    _activeView = null;
    super.deactivate();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final view = _activeView = View.of(context);
    final sizeChanged =
        _lastViewPhysicalSize != null &&
        _lastViewPhysicalSize != view.physicalSize;
    _lastViewPhysicalSize = view.physicalSize;
    final workspace = OrientationWorkspaceScope.maybeOf(context);
    _workspaceOwnsSide = workspace != null;
    final side = workspace?.side;
    final safeInsets = MediaQuery.viewPaddingOf(context);
    if (!sizeChanged &&
        side == _workspaceSide &&
        safeInsets == _rootSafeInsets) {
      return;
    }
    if (_boardMenuRoute case final route?) {
      // A half-turn can move the anchor without changing the window size.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && identical(route, _boardMenuRoute)) _dismissBoardMenu();
      });
    }
    _workspaceSide = side;
    _rootSafeInsets = safeInsets;
    _homePage = _buildHomePage();
  }

  @override
  void didChangeMetrics() {
    // Platform metrics can arrive while a replaced shell is still mounted
    // but its element is inactive and cannot look up an ancestor.
    final Size? physicalSize = _activeView?.physicalSize;
    if (physicalSize == null) return;
    final Size? previousSize = _lastViewPhysicalSize;
    _lastViewPhysicalSize = physicalSize;
    // Showing the keyboard changes viewInsets and also reports a metrics
    // change. Keep the anchored board menu and its rename dialog alive for
    // that case; only dismiss when the physical viewport actually changes.
    if (previousSize != null && previousSize != physicalSize) {
      _dismissBoardMenu();
    }
    _readLandscapeSide();
  }

  @override
  void didChangeTextScaleFactor() {
    _dismissBoardMenu();
  }

  void _dismissBoardMenu() {
    if (!_boardMenuOpen || !mounted) return;
    final Route<void>? route = _boardMenuRoute;
    _boardMenuOpen = false;
    _boardMenuRoute = null;
    final navigator = route?.navigator;
    if (route == null || navigator == null || !route.isActive) return;
    navigator.popUntil(
      (Route<dynamic> candidate) =>
          identical(candidate, route) || candidate.isFirst,
    );
    if (route.isActive) navigator.removeRoute(route);
  }

  void _onCraftAuthorityChanged() {
    if (!mounted ||
        !widget.bleService.isConnected ||
        widget.ownerVerificationPending ||
        _craftDepartureInhibitsMode ||
        _craftDeparture != null ||
        _craftReentries > 0 ||
        _updateInProgress ||
        _recoveryOnly) {
      return;
    }
    if (_craftModeSynchronizations > 0) {
      final probe =
          '${widget.bleService.connectedId}:${widget.bleService.connectionSessionToken}';
      final mode = _craftController.inspectedBoardActive;
      if (_craftModeSynchronizationProbe == probe &&
          _craftModeSynchronizationSelectionRevision ==
              _mainPageSelectionRevision &&
          mode != null) {
        _showInspectedDestination(mode);
      }
      return;
    }
    if (_currentIndex == 1 &&
        _craftController.selected &&
        _craftController.boardActive &&
        _craftController.inspectedBoardActive == true) {
      unawaited(_selectMainPage(0));
    } else if (_currentIndex == 0 &&
        !_craftController.selected &&
        !_craftController.boardActive) {
      unawaited(_selectMainPage(1));
    }
    // Setup, Console, and Board Update retain their explicit user context.
  }

  void _reportModeResolved() {
    if (!mounted || !widget.bleService.isConnected) return;
    final probe =
        '${widget.bleService.connectedId}:${widget.bleService.connectionSessionToken}';
    if (_craftModeResolvedProbe == probe) return;
    _craftModeResolvedProbe = probe;
    widget.onModeResolved?.call();
  }

  void _showInspectedDestination(bool active) {
    final probe =
        '${widget.bleService.connectedId}:${widget.bleService.connectionSessionToken}';
    final leavingPassiveCraft =
        !active && _currentIndex == 0 && _passiveCraftDestinationProbe == probe;
    if (!leavingPassiveCraft &&
        (_currentIndex != 1 || (!active && !_coldDestinationPending))) {
      return;
    }
    final page = active ? 0 : 1;
    setState(() {
      _coldDestinationPending = false;
      _currentIndex = page;
      _passiveCraftDestinationProbe = active ? probe : null;
      if (!active) _homeMounted = true;
    });
    // This follows board authority; explicit navigation's Enter/Exit path
    // must not run while passive adoption still owns artwork synchronization.
    widget.onPageSelected?.call(page);
    if (_mainPageController.hasClients) {
      _mainPageController.jumpToPage(_navigationIndexForPage(page));
    }
    _reportModeResolved();
  }

  void _finishColdDestination() {
    if (!_coldDestinationPending) return;
    setState(() {
      _coldDestinationPending = false;
      _homeMounted = _currentIndex == 1;
    });
  }

  Future<bool> _runOtaBenchmarkFromFirmware(AppTraceSink? appTrace) async {
    final service = widget.bleService;
    final boardId = widget.boardId;
    final invocation = ++_otaBenchmarkInvocation;
    final token = service.connectionSessionToken;
    var updateActive = false;
    bool ownsTarget() =>
        mounted &&
        invocation == _otaBenchmarkInvocation &&
        identical(service, widget.bleService) &&
        widget.boardId == boardId &&
        (service.connectedId == null || service.connectedId == boardId);
    bool current() =>
        ownsTarget() &&
        !widget.ownerVerificationPending &&
        !widget.modeResolving &&
        identical(service, widget.bleService) &&
        service.connectionSessionToken == token &&
        OtaBenchmarkRunner.targetReady(service);
    try {
      await _craftModeSynchronizationIdle?.future;
      await _craftController.settleModeResolution();
      if (!current() || _craftController.inspectedBoardActive == null) {
        throw StateError('Benchmark target or board mode is unresolved.');
      }
      await _selectMainPage(3);
      if (!current() ||
          _currentIndex != 3 ||
          _craftController.selected ||
          _craftController.boardActive ||
          _craftDeparture != null) {
        throw StateError('Firmware entry did not release Litchcraft.');
      }
      _onUpdateActivityChanged(true);
      updateActive = true;
      await OtaBenchmarkRunner.run(
        service,
        appTrace: appTrace,
        onProgress: (progress) {
          if (!ownsTarget()) return;
          setState(() {
            _otaBenchmarkProgress = progress;
            _firmwarePage = _buildFirmwarePage();
          });
        },
      );
      return true;
    } catch (error) {
      appTrace?.record(
        'ota.benchmark',
        fields: {'stage': 'firmware_entry', 'ok': false, 'error': '$error'},
      );
      return false;
    } finally {
      if (updateActive && ownsTarget()) _onUpdateActivityChanged(false);
    }
  }

  Future<bool> _finishCraftDeparture() {
    if (_craftDeparture != null) return _craftDeparture!;
    _craftDepartureInhibitsMode = true;
    return _craftDeparture = _departCraft()
        .then((exited) {
          if (exited) _craftDepartureInhibitsMode = false;
          return exited;
        })
        .whenComplete(() => _craftDeparture = null);
  }

  SnackBar _mainSnackBar({required String message}) {
    final media = MediaQuery.of(context);
    final usableSize = Size(
      media.size.width - media.padding.horizontal,
      media.size.height - media.padding.vertical,
    );
    final landscapeShell = LitchWorkspaceGeometry.usesCompactLandscape(
      media.size,
      usableSize,
    );
    final double bottom = landscapeShell
        ? math.max(media.viewPadding.bottom, 8.0)
        : _bottomNavigationHeight(media.size) + media.viewPadding.bottom + 8.0;
    return SnackBar(
      behavior: SnackBarBehavior.floating,
      margin: EdgeInsets.fromLTRB(16, 8, 16, bottom),
      content: Text(message),
    );
  }

  Future<bool> _departCraft() async {
    // Persist the local preview independently of the radio's ordered Exit.
    // Repeated tab taps share this transaction and the library's single writer.
    final saving = _craftLibrary.flush();
    final session = _craftController.sessionId;
    final board = widget.bleService.connectedId;
    void showFailure(String message) {
      if (!mounted ||
          !_craftDepartureRequested ||
          _currentIndex == 0 ||
          _craftController.sessionId != session ||
          widget.bleService.connectedId != board) {
        return;
      }
      _mainMessengerKey.currentState?.showSnackBar(
        _mainSnackBar(message: message),
      );
    }

    var exited = false;
    try {
      await _craftController.leave();
      exited = true;
    } catch (_) {
      // The controller retains ownership/retry intent until Exit is confirmed.
      if (mounted) showFailure(litchStrings(context).craftExitFailed);
    }
    unawaited(
      saving.then((_) {
        if (mounted && exited && _craftLibrary.saveFailed) {
          showFailure(litchStrings(context).craftSaveFailed);
        }
      }),
    );
    return exited;
  }

  Future<void> _selectNavigationPage(int pageIndex) {
    if (mounted && !_updateInProgress && pageIndex == 2) {
      ++_settingsOverviewRequestEpoch;
    }
    if (mounted &&
        !_recoveryOnly &&
        !_updateInProgress &&
        !_coldDestinationPending &&
        _currentIndex == 2 &&
        pageIndex == 2) {
      ++_navigationIntent;
      setState(() {
        _settingsPage = _buildSettingsPage();
      });
      widget.onPageSelected?.call(pageIndex);
      return Future<void>.value();
    }
    return _selectMainPage(pageIndex);
  }

  void _showConnectionChoices() {
    if (!mounted || _recoveryOnly || _updateInProgress) return;
    if (_currentIndex == 4) {
      _appConsoleScreenKey.currentState?.deactivateConsole();
    }
    ++_navigationIntent;
    ++_mainPageSelectionRevision;
    _retireCraftModeInspection();
    _passiveCraftDestinationProbe = null;
    _craftModeResolvedProbe = null;
    setState(() {
      // Discovery is passive: keep the retained shell in sync without treating
      // its Play slot as an Exit request or mounting an unproved Home board.
      _currentIndex = 1;
      _coldDestinationPending = true;
    });
    if (_mainPageController.hasClients) {
      _mainPageController.jumpToPage(_navigationIndexForPage(1));
    }
  }

  void _activateConsoleAfterFrame() {
    final revision = _mainPageSelectionRevision;
    // Paint the retained page before native keyboard preparation can run.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted &&
          _currentIndex == 4 &&
          revision == _mainPageSelectionRevision) {
        _appConsoleScreenKey.currentState?.activateConsole();
      }
    });
  }

  void _setSelectedPage(int pageIndex) {
    if (pageIndex == 3 && _currentIndex != 3) {
      _firmwareOriginPage = _currentIndex;
    }
    _currentIndex = pageIndex;
  }

  Future<void> _selectMainPage(int pageIndex) async {
    if (!mounted) return;
    if (_recoveryOnly) {
      if (_updateInProgress || (pageIndex != 2 && pageIndex != 3)) return;
      setState(() {
        _setSelectedPage(pageIndex);
        // Cold pending updates prewarm Setup with deferred loading. Activate
        // that existing page on entry; its session guard keeps the read once.
        if (pageIndex == 2) _settingsPage = _buildSettingsPage();
      });
      widget.onPageSelected?.call(pageIndex);
      return;
    }
    _passiveCraftDestinationProbe = null;
    final int selectionRevision =
        _currentIndex == pageIndex && !_coldDestinationPending
        ? _mainPageSelectionRevision
        : ++_mainPageSelectionRevision;
    if (_updateInProgress) {
      return;
    }
    if (_currentIndex == 4 && pageIndex != 4) {
      _appConsoleScreenKey.currentState?.deactivateConsole();
    }
    final intent = ++_navigationIntent;
    if (pageIndex == 0) {
      _craftDepartureRequested = false;
      _craftDepartureInhibitsMode = false;
      _mainMessengerKey.currentState?.hideCurrentSnackBar();
    } else if (pageIndex == 1 || pageIndex == 3) {
      _craftDepartureRequested = true;
    }
    final exitsCraftMode =
        (_craftController.selected || _craftController.boardActive) &&
        (pageIndex == 1 || pageIndex == 3);
    // Firmware must await board release; ordinary pages render immediately.
    if (exitsCraftMode && pageIndex == 3) {
      if (!await _finishCraftDeparture() ||
          !mounted ||
          intent != _navigationIntent) {
        return;
      }
    }
    final leavingCraftPage = _currentIndex == 0 && pageIndex != 0;
    final enteringCraft =
        pageIndex == 0 &&
        (!_craftController.selected ||
            _currentIndex != 0 ||
            _craftDeparture != null);
    final craftModeGeneration = _craftModeSynchronizationGeneration;
    if (enteringCraft) ++_craftReentries;
    if (_currentIndex != pageIndex ||
        _coldDestinationPending ||
        (pageIndex == 1 && !_homeMounted)) {
      setState(() {
        _coldDestinationPending = false;
        if (pageIndex == 1) _homeMounted = true;
        _setSelectedPage(pageIndex);
        if (pageIndex == 2) {
          ++_settingsHealthRefreshEpoch;
          _settingsPage = _buildSettingsPage();
        }
      });
    }
    // Reconnect routing needs the actual page, including Console and Update,
    // so their explicit context is not mistaken for the last selected Play tab.
    widget.onPageSelected?.call(pageIndex);
    if (pageIndex != 3 && _mainPageController.hasClients) {
      _mainPageController.jumpToPage(_navigationIndexForPage(pageIndex));
    }
    if (pageIndex == 4) _activateConsoleAfterFrame();
    if (exitsCraftMode && pageIndex != 3) {
      unawaited(_finishCraftDeparture());
    } else if (leavingCraftPage) {
      _persistCraftDraftInBackground();
    }
    if (enteringCraft) {
      try {
        await _craftDeparture;
        if (!mounted ||
            intent != _navigationIntent ||
            selectionRevision != _mainPageSelectionRevision ||
            craftModeGeneration != _craftModeSynchronizationGeneration ||
            _currentIndex != 0) {
          return;
        }
        // Explicit mode intent outranks passive adoption and rich metadata.
        if (_craftModeSynchronizations > 0 || !_craftController.selected) {
          await _craftController.enter(draftReady: _craftLibrary.ready);
        } else if (_craftController.status == LitchcraftStatus.error) {
          await _craftController.retryConnection();
        } else {
          await _craftController.revalidateSelectedBoard();
        }
      } finally {
        --_craftReentries;
        // Reconcile the completed navigation without blocking local editing.
        if (mounted) setState(() {});
      }
    }
  }

  Future<void> _loadVersionArtwork(int taps) async {
    if (!await _confirmAboutArtwork(context)) {
      return;
    }
    try {
      unawaited(_selectMainPage(0));
      await _craftLibrary.loadVersionArtwork(taps);
    } catch (error) {
      if (mounted && !_craftLibrary.isOpenFailureHandled(error)) {
        _mainMessengerKey.currentState?.showSnackBar(
          _mainSnackBar(message: litchStrings(context).craftActionFailed),
        );
      }
    }
  }

  void _persistCraftDraftInBackground() {
    unawaited(
      _craftLibrary.flush().then((_) {
        if (!mounted || !_craftLibrary.saveFailed) return;
        _mainMessengerKey.currentState?.showSnackBar(
          _mainSnackBar(message: litchStrings(context).actionApplyFailed),
        );
      }),
    );
  }

  void _retireCraftModeInspection() {
    _craftConnectionProbe = null;
    _craftModeSynchronizationProbe = null;
    ++_craftModeSynchronizationGeneration;
    _craftModeSynchronizations = 0;
    _craftModeSynchronizationIdle?.complete();
    _craftModeSynchronizationIdle = null;
  }

  void _onCraftConnectionChanged(ConnectionStateUpdate update) {
    if (update.connectionState != DeviceConnectionState.connected) {
      _dismissBoardMenu();
      if (update.deviceId == widget.bleService.connectedId ||
          !widget.bleService.isConnected) {
        _retireCraftModeInspection();
        if (mounted) setState(() {});
      }
      return;
    }
    unawaited(_synchronizeConnectedCraftMode());
  }

  Future<void> _synchronizeConnectedCraftMode() async {
    final String? deviceId = widget.bleService.connectedId;
    if (!widget.bleService.isConnected ||
        deviceId == null ||
        widget.ownerVerificationPending) {
      return;
    }
    final int connectionToken = widget.bleService.connectionSessionToken;
    final String probe = '$deviceId:$connectionToken';
    if (_craftConnectionProbe == probe) return;
    _craftConnectionProbe = probe;
    _craftModeSynchronizationProbe = probe;
    final int selectionRevision = _mainPageSelectionRevision;
    final bool craftInitiallySelected = _currentIndex == 0;
    if (craftInitiallySelected &&
        _craftController.selected &&
        _craftController.boardActive &&
        _craftController.inspectedBoardActive == null) {
      // A retained tab belongs to the previous link, not fresh Enter intent.
      // Current-link inspection must honor a physical Exit while absent.
      // Explicit navigation clears this passive binding and keeps precedence.
      _passiveCraftDestinationProbe = probe;
    }
    _craftModeSynchronizationSelectionRevision = selectionRevision;
    final generation = _craftModeSynchronizationGeneration;
    bool current() =>
        mounted &&
        generation == _craftModeSynchronizationGeneration &&
        _craftConnectionProbe == probe &&
        !widget.ownerVerificationPending &&
        widget.bleService.isConnected &&
        widget.bleService.connectedId == deviceId &&
        widget.bleService.connectionSessionToken == connectionToken;

    _craftModeSynchronizationIdle ??= Completer<void>();
    ++_craftModeSynchronizations;
    if (mounted) setState(() {});
    try {
      // Discovery owns this connection's domain admission. Recovery must reach
      // health and support even when the local Craft library is unavailable.
      final capabilities = await widget.bleService.fetchDeviceCapabilities(
        deviceId: deviceId,
      );
      if (!current()) return;
      final recoveryOnly =
          capabilities.recoveryOnly ||
          (_recoveryOnly && !capabilities.explicitlyReported);
      if (_recoveryOnly != recoveryOnly ||
          (recoveryOnly && _currentIndex != 2 && _currentIndex != 3)) {
        if (recoveryOnly && _currentIndex == 4) {
          _appConsoleScreenKey.currentState?.deactivateConsole();
        }
        setState(() {
          _recoveryOnly = recoveryOnly;
          if (recoveryOnly) {
            // Cold OTA can appear before this link reports recovery mode.
            if (_currentIndex == 3) _firmwareOriginPage ??= 2;
            _currentIndex = _updateInProgress ? 3 : 2;
            _coldDestinationPending = false;
          }
          _settingsPage = _buildSettingsPage();
        });
      }
      if (recoveryOnly) widget.onPageSelected?.call(_currentIndex);
      if (recoveryOnly ||
          _updateInProgress ||
          widget.bleService.pendingOtaVerification != null) {
        _reportModeResolved();
        return;
      }
      // Capabilities can arrive before the parent paints its owner gate. Use
      // the session's readiness owner before any product-mode inspection.
      if (capabilities.supportsOwnerPairing &&
          !widget.bleService.productReady) {
        final ready = await widget.bleService.ensureProductReady(deviceId);
        if (!ready || !current()) return;
      }
      bool boardActive = false;
      try {
        boardActive = await _craftController.adoptActiveBoard();
      } catch (_) {
        // Keep the current surface usable when the mode probe cannot complete.
      }
      if (!current()) return;
      if (selectionRevision == _mainPageSelectionRevision &&
          !_craftDepartureInhibitsMode &&
          _craftDeparture == null &&
          _craftReentries == 0) {
        final mode = _craftController.inspectedBoardActive;
        if (mode != null) _showInspectedDestination(mode);
      }
      try {
        await _craftController.settleModeResolution();
        boardActive = _craftController.boardActive;
      } catch (_) {
        // Artwork recovery does not own the established destination.
      }
      if (!current()) return;
      // A retained controller may already own reconnect inspection. Adoption
      // returning false is not a terminal result until that work has settled.
      // Current-link mode notifications can still reveal Craft before artwork.
      if (_coldDestinationPending) {
        final mode = _craftController.inspectedBoardActive;
        if (mode != null && selectionRevision == _mainPageSelectionRevision) {
          _showInspectedDestination(mode);
        } else {
          _finishColdDestination();
          _reportModeResolved();
        }
      }
      if (mounted &&
          _craftDepartureRequested &&
          _currentIndex != 0 &&
          boardActive) {
        final exited = await _finishCraftDeparture();
        boardActive = exited ? false : _craftController.boardActive;
      }
      if (!current()) return;

      if (selectionRevision == _mainPageSelectionRevision &&
          !_craftDepartureInhibitsMode &&
          _craftDeparture == null &&
          _craftReentries == 0) {
        if (boardActive &&
            _craftController.inspectedBoardActive == true &&
            !_updateInProgress) {
          if (_currentIndex == 1) {
            setState(() => _currentIndex = 0);
            widget.onPageSelected?.call(0);
            if (_mainPageController.hasClients) {
              _mainPageController.jumpToPage(_navigationIndexForPage(0));
            }
          }
        } else if (!boardActive &&
            _currentIndex == 0 &&
            craftInitiallySelected) {
          try {
            await _craftController.enter();
          } catch (_) {
            // The controller exposes its repair state on the retained canvas.
          }
        }
      }
      if (!current()) return;
      _reportModeResolved();
    } catch (_) {
      // Failed capability discovery cannot authorize automatic domain entry.
    } finally {
      if (current() && _coldDestinationPending) {
        _finishColdDestination();
        _reportModeResolved();
      }
      if (generation == _craftModeSynchronizationGeneration) {
        --_craftModeSynchronizations;
        if (_craftModeSynchronizations == 0) {
          _craftModeSynchronizationIdle?.complete();
          _craftModeSynchronizationIdle = null;
        }
      }
      if (mounted) setState(() {});
    }
  }

  void _onHealthAttentionChanged(bool needsAttention) {
    if (!mounted || _healthAttention.value == needsAttention) return;
    setState(() => _healthAttention.value = needsAttention);
  }

  Future<void> _loadAppConsolePreference() async {
    if (widget.enableAppConsole != null) return;
    bool enabled = false;
    try {
      final AppConsolePreferenceLoader loader =
          widget.loadAppConsolePreference ?? _loadAppConsolePreferenceFromPrefs;
      enabled = await loader();
    } catch (_) {}

    if (!mounted) {
      return;
    }

    if (_enableAppConsole == enabled) {
      return;
    }

    setState(() {
      _enableAppConsole = enabled;
      _settingsPage = _buildSettingsPage();
    });
  }

  Future<void> _setEnableAppConsole(bool enabled) async {
    try {
      final AppConsolePreferenceSaver saver =
          widget.saveAppConsolePreference ?? _saveAppConsolePreferenceToPrefs;
      await saver(enabled);
    } catch (_) {}

    if (!mounted) {
      return;
    }

    final bool removingCurrentPage = !enabled && _currentIndex == 4;
    setState(() {
      _enableAppConsole = enabled;
      if (!_enableAppConsole && _currentIndex == 4) {
        _currentIndex = 2;
      }
      _settingsPage = _buildSettingsPage();
    });
    if (removingCurrentPage && _mainPageController.hasClients) {
      _mainPageController.jumpToPage(2);
    }
  }

  Widget _buildFirmwarePage() {
    final FirmwarePageBuilder builder =
        widget.firmwarePageBuilder ?? _buildDefaultFirmwarePage;
    return KeyedSubtree(
      key: ValueKey<int>(_firmwareRefreshEpoch),
      child: builder(
        widget.bleService,
        widget.boardId,
        _showFirmwareTab,
        _onUpdateActivityChanged,
        _onFirmwareAvailabilityChanged,
      ),
    );
  }

  Widget _buildSettingsPage() {
    return SettingsScreen(
      key: _settingsScreenKey,
      // Prewarm layout, not hidden BLE enumeration/runtime-health work.
      deferInitialLoad: _currentIndex != 2,
      recoveryOnly: _recoveryOnly,
      bleService: widget.bleService,
      boardId: widget.boardId,
      notices: _notices,
      enableAppConsole: _enableAppConsole,
      onEnableAppConsoleChanged: _setEnableAppConsole,
      appearanceMode: widget.appearanceMode,
      onAppearanceModeChanged: widget.onAppearanceModeChanged,
      healthRefreshEpoch: _settingsHealthRefreshEpoch,
      healthFocusEpoch: _settingsHealthFocusEpoch,
      overviewRequestEpoch: _settingsOverviewRequestEpoch,
      onOpenFirmware: _showFirmwareTab,
      landscapeOverlayClearance: _settingsOverlayClearance,
    );
  }

  Future<bool> _loadAppConsolePreferenceFromPrefs() async {
    final SharedPreferences prefs = await SharedPreferences.getInstance();
    return prefs.getBool(_enableAppConsolePreferenceKey) ?? false;
  }

  Future<void> _saveAppConsolePreferenceToPrefs(bool enabled) async {
    final SharedPreferences prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_enableAppConsolePreferenceKey, enabled);
  }

  Widget _buildDefaultFirmwarePage(
    LitchBleService bleService,
    String boardId,
    VoidCallback onRequestShowFirmwareTab,
    ValueChanged<bool> onUpdateActivityChanged,
    ValueChanged<FirmwareUpdateAvailability> onAvailabilityChanged,
  ) {
    return FirmwareScreen(
      bleService: bleService,
      boardId: boardId,
      benchmarkProgress: _otaBenchmarkProgress,
      onRequestShowFirmwareTab: onRequestShowFirmwareTab,
      onUpdateActivityChanged: onUpdateActivityChanged,
      onAvailabilityChanged: onAvailabilityChanged,
    );
  }

  void _onFirmwareAvailabilityChanged(FirmwareUpdateAvailability availability) {
    if (!mounted || availability == _firmwareAvailability) {
      return;
    }
    setState(() => _firmwareAvailability = availability);
  }

  void _onUpdateActivityChanged(bool active) {
    if (!mounted) {
      return;
    }

    if (active && _currentIndex == 4) {
      _appConsoleScreenKey.currentState?.deactivateConsole();
    }
    setState(() {
      _updateInProgress = active;
      if (active) {
        _otaBenchmarkProgress = null;
        _firmwarePage = _buildFirmwarePage();
        _setSelectedPage(3);
      }
    });
  }

  void _showFirmwareTab() {
    if (mounted) unawaited(_selectMainPage(3));
  }

  void _returnFromFirmware() {
    if (_updateInProgress || _currentIndex != 3) return;
    final origin = _firmwareOriginPage ?? 1;
    final page = _recoveryOnly || (origin == 4 && !_enableAppConsole)
        ? 2
        : origin;
    unawaited(_selectMainPage(page));
  }

  void _showBoardHealth() {
    if (!mounted || _updateInProgress) return;
    ++_settingsHealthFocusEpoch;
    if (_currentIndex == 2) {
      setState(() {
        ++_settingsHealthRefreshEpoch;
        _settingsPage = _buildSettingsPage();
      });
    } else {
      unawaited(_selectMainPage(2));
    }
  }

  void _openGameLibrary() {
    if (widget.gameLibrary == null && widget.gameLibraryOpener == null) {
      return;
    }
    Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        builder: (_) => GameLibraryRouteScreen(
          repository: widget.gameLibrary,
          openRepository: widget.gameLibraryOpener,
        ),
      ),
    );
  }

  Future<void> _showBoardMenu() async {
    if (_boardMenuOpen || !widget.bleService.isConnected) return;
    final BuildContext? anchorContext = _boardMenuAnchorKey.currentContext;
    final RenderBox? anchor = anchorContext?.findRenderObject() as RenderBox?;
    final RenderBox? overlay =
        Overlay.of(context).context.findRenderObject() as RenderBox?;
    if (anchor == null || overlay == null) return;
    final Offset anchorOrigin = anchor.localToGlobal(
      Offset.zero,
      ancestor: overlay,
    );
    final Rect anchorRect = anchorOrigin & anchor.size;
    final MediaQueryData media = MediaQuery.of(context);
    final double horizontalInset = media.padding.left + 8;
    final double rightInset = media.padding.right + 8;
    final availableWidth = media.size.width - horizontalInset - rightInset;
    if (availableWidth <= 0) return;
    final width = math.min(360.0, availableWidth);
    final left = (anchorRect.right - width).clamp(
      horizontalInset,
      horizontalInset + (availableWidth - width),
    );
    final double safeTop = media.padding.top + 8;
    final double safeBottom = media.size.height - media.padding.bottom - 8;
    final double availableBelow = safeBottom - anchorRect.bottom - 4;
    final double availableAbove = anchorRect.top - 4 - safeTop;
    final bool placeBelow =
        availableBelow >= 220 || availableBelow >= availableAbove;
    final double availableHeight = placeBelow ? availableBelow : availableAbove;
    final double maximumHeight = availableHeight.clamp(0.0, 480.0);
    final double? top = placeBelow ? anchorRect.bottom + 4 : null;
    final double? bottom = placeBelow
        ? null
        : overlay.size.height - anchorRect.top + 4;
    final openingSide = _workspaceSide;
    _boardMenuOpen = true;
    try {
      await showGeneralDialog<void>(
        context: context,
        barrierDismissible: true,
        barrierLabel: MaterialLocalizations.of(
          context,
        ).modalBarrierDismissLabel,
        barrierColor: Colors.black26,
        transitionDuration: const Duration(milliseconds: 180),
        transitionBuilder: (context, animation, secondaryAnimation, child) =>
            FadeTransition(
              opacity: animation,
              child: SlideTransition(
                position:
                    Tween<Offset>(
                      begin: const Offset(0, -.04),
                      end: Offset.zero,
                    ).animate(
                      CurvedAnimation(parent: animation, curve: Curves.easeOut),
                    ),
                child: child,
              ),
            ),
        pageBuilder: (dialogContext, _, _) {
          _boardMenuRoute ??= ModalRoute.of<void>(dialogContext);
          if (OrientationWorkspaceScope.maybeOf(dialogContext)?.side !=
                  openingSide ||
              MediaQuery.viewPaddingOf(dialogContext) != media.viewPadding) {
            return const SizedBox.shrink();
          }
          return Stack(
            children: <Widget>[
              Positioned(
                key: const ValueKey('connected-board-popover'),
                left: left,
                top: top,
                bottom: bottom,
                width: width,
                child: ConstrainedBox(
                  constraints: BoxConstraints(maxHeight: maximumHeight),
                  child: Material(
                    elevation: 8,
                    clipBehavior: Clip.antiAlias,
                    color: Theme.of(dialogContext).colorScheme.surface,
                    borderRadius: BorderRadius.circular(16),
                    // The popover position already clears these insets.
                    // Prevent ListTile from consuming them again.
                    child: MediaQuery.removePadding(
                      context: dialogContext,
                      removeLeft: true,
                      removeRight: true,
                      child: ValueListenableBuilder<bool>(
                        valueListenable: _healthAttention,
                        builder: (context, needsAttention, _) =>
                            _ConnectedBoardSheet(
                              bleService: widget.bleService,
                              boardId: widget.boardId,
                              boardName: widget.boards
                                  .where(
                                    (LitchBoard board) =>
                                        board.id == widget.boardId,
                                  )
                                  .map((LitchBoard board) => board.name)
                                  .firstOrNull,
                              healthNeedsAttention: needsAttention,
                              updateAvailable:
                                  _firmwareAvailability.updateAvailable,
                              installedVersion:
                                  _firmwareAvailability.installedVersion,
                              latestVersion:
                                  _firmwareAvailability.latestVersion,
                              updateInProgress: _updateInProgress,
                              onShowHealth: () {
                                Navigator.of(dialogContext).pop();
                                _showBoardHealth();
                              },
                              onShowFirmware: () {
                                Navigator.of(dialogContext).pop();
                                _showFirmwareTab();
                              },
                              onDisconnect: () {
                                _boardMenuOpen = false;
                                Navigator.of(dialogContext).pop();
                                unawaited(widget.onDisconnect());
                              },
                            ),
                      ),
                    ),
                  ),
                ),
              ),
            ],
          );
        },
      );
    } finally {
      _boardMenuOpen = false;
      _boardMenuRoute = null;
    }
  }

  @override
  void didUpdateWidget(covariant LitchMainScaffold oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.enableAppConsole != null &&
        oldWidget.enableAppConsole != widget.enableAppConsole) {
      _enableAppConsole = widget.enableAppConsole!;
      if (!_enableAppConsole && _currentIndex == 4) {
        _currentIndex = 2;
        if (_mainPageController.hasClients) _mainPageController.jumpToPage(2);
      }
      _settingsPage = _buildSettingsPage();
    }
    if (!oldWidget.ownerVerificationPending &&
        widget.ownerVerificationPending) {
      _retireCraftModeInspection();
    } else if (oldWidget.ownerVerificationPending &&
        !widget.ownerVerificationPending) {
      unawaited(_synchronizeConnectedCraftMode());
    }
    if (oldWidget.landscapeRibbonSide != widget.landscapeRibbonSide) {
      ++_landscapeSideGeneration;
      _homePage = _buildHomePage();
      _readLandscapeSide();
    }
    if (oldWidget.initialCraftModeResolved &&
        !widget.initialCraftModeResolved &&
        widget.bleService.isConnected) {
      final String probe =
          '${widget.bleService.connectedId}:${widget.bleService.connectionSessionToken}';
      if (_craftModeSynchronizationProbe != probe) {
        _craftConnectionProbe = null;
        unawaited(_synchronizeConnectedCraftMode());
      }
    }
    if (oldWidget.appearanceMode != widget.appearanceMode ||
        oldWidget.onAppearanceModeChanged != widget.onAppearanceModeChanged) {
      _settingsPage = _buildSettingsPage();
    }
    if (oldWidget.boardId != widget.boardId ||
        oldWidget.bleService != widget.bleService ||
        oldWidget.gameLibrary != widget.gameLibrary) {
      if (oldWidget.boardId != widget.boardId ||
          oldWidget.bleService != widget.bleService) {
        ++_otaBenchmarkInvocation;
        _otaBenchmarkProgress = null;
        _firmwareOriginPage = null;
        _updateInProgress = false;
        _recoveryOnly = widget.bleService
            .deviceCapabilities(widget.boardId)
            .recoveryOnly;
        if (_recoveryOnly) _currentIndex = 2;
        _coldDestinationPending =
            !widget.initialCraftModeResolved && _currentIndex == 1;
        _homeMounted = _currentIndex == 1 && !_coldDestinationPending;
        _craftModeResolvedProbe = null;
      }
      _healthAttention.value = false;
      _homePage = _buildHomePage();
      _settingsPage = _buildSettingsPage();
      _firmwareRefreshEpoch += 1;
      _firmwarePage = _buildFirmwarePage();
      _appConsolePage = AppConsoleScreen(
        key: _appConsoleScreenKey,
        bleService: widget.bleService,
        boardId: widget.boardId,
        notices: _notices,
      );
    }
    // Transport readiness cannot turn the gate's default page into navigation
    // intent. Current-link inspection owns passive mode restoration; explicit
    // page choices already enter through _selectNavigationPage.
  }

  @override
  Widget build(BuildContext context) {
    final EdgeInsets rootSafeInsets = MediaQuery.viewPaddingOf(context);
    final List<NavigationDestination> destinations = <NavigationDestination>[
      NavigationDestination(
        key: const ValueKey('main-destination-play'),
        icon: const LitchPlayGlyph(size: 31),
        label: litchStrings(context).playTab,
      ),
      NavigationDestination(
        key: const ValueKey('main-destination-craft'),
        icon: const LitchcraftIdentityGlyph(size: 31),
        label: litchStrings(context).craftTitle,
      ),
      NavigationDestination(
        key: const ValueKey('main-destination-setup'),
        icon: const Icon(Icons.tune_outlined, size: 31),
        selectedIcon: const Icon(Icons.tune_rounded, size: 31),
        label: litchStrings(context).setupTab,
      ),
      if (_enableAppConsole)
        NavigationDestination(
          key: const ValueKey('main-destination-console'),
          icon: const Icon(Icons.terminal, size: 31),
          label: litchStrings(context).consoleTab,
        ),
    ];

    final bool showingFirmware = _currentIndex == 3;
    // Firmware is a transient fourth page and has no navigation destination.
    // Keep constructing the shared navigation safe while that page is visible;
    // its selection becomes relevant again only after returning to the shell.
    final int selectedNavigationIndex = _navigationIndexForPage(
      _currentIndex,
    ).clamp(0, destinations.length - 1);
    final bool largeToolbar = MediaQuery.textScalerOf(context).scale(14) >= 21;
    final double screenWidth = MediaQuery.sizeOf(context).width;
    final TextStyle titleStyle =
        Theme.of(context).appBarTheme.titleTextStyle ??
        Theme.of(context).textTheme.titleLarge!;
    final double statusWidth = _mainStatusWidth(context);
    // Keep the existing horizontal hierarchy; let translated/scaled text use
    // the height it actually needs instead of clipping or reducing font size.
    final double titleHeight = showingFirmware || _recoveryOnly
        ? _toolbarTextHeight(
            context,
            showingFirmware
                ? litchStrings(context).boardUpdateTitle
                : litchStrings(context).setupTab,
            titleStyle,
            screenWidth -
                88 -
                (widget.ownerVerificationPending ? statusWidth : 0),
            maxLines: 3,
          )
        : 0;
    final bool connected = widget.bleService.isConnected;
    final bool connecting =
        widget.connectionPending || widget.ownerVerificationPending;
    // Connection health is independent of background document reconciliation.
    final String boardStatusLabel = connecting
        ? litchStrings(context).boardConnecting
        : !connected
        ? litchStrings(context).boardDisconnected
        : litchStrings(context).boardConnected;
    final String? statusSubtitle = !connected || connecting
        ? null
        : _healthAttention.value
        ? litchStrings(context).needsAttention
        : _firmwareAvailability.updateAvailable
        ? litchStrings(context).updateAvailable
        : null;
    final double statusHeight =
        _boardStatusTextHeight(
          context,
          boardStatusLabel,
          hasChevron: connected && !connecting,
        ) +
        (statusSubtitle != null
            ? _toolbarTextHeight(
                context,
                statusSubtitle,
                _BoardStatusAction.updateStyle(context),
                statusWidth,
                maxLines: 2,
              )
            : 0);
    final double toolbarHeight = <double>[
      _mainToolbarHeight(context),
      titleHeight,
      if (!showingFirmware || widget.ownerVerificationPending) statusHeight,
    ].reduce((a, b) => a > b ? a : b);
    final MediaQueryData media = MediaQuery.of(context);
    final Size usableSize = Size(
      media.size.width - media.padding.horizontal,
      media.size.height - media.padding.vertical,
    );
    final bool landscapeShell =
        !showingFirmware &&
        LitchWorkspaceGeometry.usesCompactLandscape(media.size, usableSize);
    final bool landscapeCraft = landscapeShell && _currentIndex == 0;
    final Alignment landscapeControlSide = _currentIndex == 4
        ? Alignment.centerRight
        : _landscapeRibbonSide;
    final tablet = LitchWorkspaceGeometry.isTabletWindow(media.size);
    final bottomNavigationHeight = _bottomNavigationHeight(media.size);
    final Widget navigation = _coldDestinationPending
        ? SizedBox(
            height: bottomNavigationHeight,
            child: Row(
              children: <Widget>[
                for (var index = 0; index < destinations.length; ++index)
                  Expanded(
                    child: _CompactNavigationAction(
                      key: destinations[index].key,
                      label: destinations[index].label,
                      selected: false,
                      onPressed: () =>
                          _selectNavigationPage(_pageForNavigationIndex(index)),
                      child: destinations[index].icon,
                    ),
                  ),
              ],
            ),
          )
        : NavigationBar(
            height: bottomNavigationHeight,
            labelBehavior: tablet
                ? NavigationDestinationLabelBehavior.alwaysShow
                : NavigationDestinationLabelBehavior.alwaysHide,
            selectedIndex: selectedNavigationIndex,
            onDestinationSelected: (int index) =>
                _selectNavigationPage(_pageForNavigationIndex(index)),
            destinations: destinations,
          );
    Widget compactStatusAction({bool iconOnly = true}) => ListenableBuilder(
      listenable: _craftController,
      builder: (context, _) {
        if (connecting) {
          return Semantics(
            liveRegion: true,
            child: Tooltip(
              message: boardStatusLabel,
              child: const SizedBox.square(
                dimension: 48,
                child: Icon(Icons.sync_rounded, size: 36),
              ),
            ),
          );
        }
        final bool offline = !connected;
        final String label = offline
            ? litchStrings(context).disconnectedChoose
            : <String>[boardStatusLabel, ?statusSubtitle].join('. ');
        return Tooltip(
          message: label,
          excludeFromSemantics: true,
          child: Semantics(
            key: const ValueKey('compact-board-status-live-region'),
            button: true,
            container: true,
            liveRegion: true,
            excludeSemantics: true,
            label: label,
            onTap: connected ? _showBoardMenu : widget.onDisconnect,
            child: SizedBox(
              width: iconOnly ? 48 : 92,
              key: _boardMenuAnchorKey,
              child: TextButton(
                key: const ValueKey<String>('connected-board-status-action'),
                style: TextButton.styleFrom(
                  padding: const EdgeInsets.symmetric(horizontal: 2),
                  minimumSize: Size(iconOnly ? 48 : 92, 48),
                  foregroundColor: Theme.of(context).colorScheme.onSurface,
                ),
                onPressed: connected ? _showBoardMenu : widget.onDisconnect,
                child: Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: <Widget>[
                    if (offline)
                      const _DisconnectedStatusIcon(size: 36)
                    else
                      Icon(
                        _healthAttention.value
                            ? Icons.warning_amber_rounded
                            : Icons.sensors,
                        size: 36,
                      ),
                    if (!iconOnly) ...[
                      const SizedBox(width: 3),
                      Flexible(
                        child: Text(
                          boardStatusLabel,
                          maxLines: 1,
                          overflow: TextOverflow.fade,
                          softWrap: false,
                          style: const TextStyle(fontSize: 11),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ),
          ),
        );
      },
    );
    Widget compactNavigationActions(
      int currentPage, {
      bool iconOnlyStatus = true,
      bool embeddedToolPanel = false,
    }) => _CompactAppNavigation(
      currentPage: currentPage,
      embeddedToolPanel: embeddedToolPanel,
      consoleEnabled: _enableAppConsole,
      onLoadVersionArtwork: _loadVersionArtwork,
      versionTapClock: widget.versionTapClock,
      onSelected: (page) => _selectNavigationPage(page),
      statusAction: compactStatusAction(iconOnly: iconOnlyStatus),
    );
    const Widget? craftHeader = null;
    final landscapeGeometry = LitchWorkspaceGeometry(
      Size(
        media.size.width - rootSafeInsets.horizontal,
        media.size.height - rootSafeInsets.vertical,
      ),
      safeInsets: rootSafeInsets,
    );
    const craftFooterHeight = LitchWorkspaceGeometry.navigationHeight;
    final navigationWidth = landscapeGeometry.compactNavigationWidth;
    final landscapeEdges = landscapeGeometry.landscapeEdgeInsets;
    final setupOverlayClearance =
        landscapeShell && !_recoveryOnly && media.viewInsets.bottom == 0
        ? craftFooterHeight + landscapeEdges.bottom + 8
        : 0.0;
    if (setupOverlayClearance != _settingsOverlayClearance) {
      _settingsOverlayClearance = setupOverlayClearance;
      _settingsPage = _buildSettingsPage();
    }
    // Leave the remaining height available to the focused search field.
    final setupPanePadding = landscapeShell && media.viewInsets.bottom > 0
        ? _landscapeRibbonSide.x < 0
              ? EdgeInsets.only(left: navigationWidth + landscapeEdges.left)
              : EdgeInsets.only(right: navigationWidth + landscapeEdges.right)
        : EdgeInsets.zero;
    final playLandscapeVerticalMargin = landscapeShell
        ? landscapeGeometry.landscapeEdgeInsets.top
        : 0.0;
    // The body already excludes portrait navigation and the protected bottom
    // inset. Consume only the keyboard portion that actually enters Console.
    final consoleBottomClearance = _currentIndex == 4
        ? (media.viewInsets.bottom -
                  (landscapeShell
                      ? 0
                      : rootSafeInsets.bottom + bottomNavigationHeight))
              .clamp(0.0, double.infinity)
        : 0.0;
    final Widget? craftFooter = landscapeCraft
        ? SizedBox(
            key: const ValueKey('craft-landscape-navigation'),
            height: craftFooterHeight,
            // A current-destination menu keeps the full app order available in
            // the narrow document pane without displacing the canvas.
            child: compactNavigationActions(
              0,
              iconOnlyStatus: true,
              embeddedToolPanel: true,
            ),
          )
        : null;
    final landscapeFooter = SizedBox(
      key: const ValueKey('main-landscape-navigation'),
      height: craftFooterHeight,
      child: compactNavigationActions(
        _coldDestinationPending ? -1 : _currentIndex,
      ),
    );

    return PopScope<Object?>(
      canPop: !showingFirmware && !_recoveryOnly,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (didPop || _updateInProgress) return;
        if (showingFirmware) {
          _returnFromFirmware();
        } else if (_recoveryOnly) {
          unawaited(widget.onDisconnect());
        }
      },
      child: ScaffoldMessenger(
        key: _mainMessengerKey,
        child: Scaffold(
          backgroundColor: _currentIndex == 4 ? Colors.black : null,
          // A Console IME frame must not relayout every warm page. Only the
          // active Console child consumes keyboard intrusion below.
          resizeToAvoidBottomInset: _currentIndex != 4,
          // Portrait Litchcraft uses the same app header as Play and Setup.
          // Only landscape omits the header.
          appBar: landscapeShell && !_recoveryOnly
              ? null
              : !showingFirmware && !_recoveryOnly
              ? LitchCompactAppBar(
                  child: _mainBrandAppBar(
                    context,
                    onLoadVersionArtwork: _loadVersionArtwork,
                    versionTapClock: widget.versionTapClock,
                    litchcraft: _currentIndex == 0,
                    connecting: connecting,
                    statusHeight: statusHeight,
                    notificationPredicate: (notification) =>
                        notification.metrics.axis == Axis.vertical &&
                        notification.depth <= 1,
                    status: SizedBox(
                      key: _boardMenuAnchorKey,
                      child: connecting
                          ? const _BoardStatusLabel(connecting: true)
                          : _BoardStatusAction(
                              connected: connected,
                              largeText: largeToolbar,
                              updateAvailable:
                                  connected &&
                                  _firmwareAvailability.updateAvailable,
                              healthNeedsAttention:
                                  connected && _healthAttention.value,
                              onTap: connected
                                  ? _showBoardMenu
                                  : widget.onDisconnect,
                            ),
                    ),
                  ),
                )
              : LitchCompactAppBar(
                  child: AppBar(
                    notificationPredicate: (notification) =>
                        notification.metrics.axis == Axis.vertical &&
                        notification.depth <= 1,
                    toolbarHeight: toolbarHeight,
                    leading: _recoveryOnly && !showingFirmware
                        ? IconButton(
                            key: const ValueKey('recovery-disconnect'),
                            tooltip: litchStrings(context).disconnectAction,
                            onPressed: _updateInProgress
                                ? null
                                : () => unawaited(widget.onDisconnect()),
                            icon: const Icon(Icons.link_off_rounded),
                          )
                        : showingFirmware
                        ? IconButton(
                            key: const ValueKey<String>('firmware-back'),
                            tooltip: MaterialLocalizations.of(
                              context,
                            ).backButtonTooltip,
                            onPressed: _updateInProgress
                                ? null
                                : _returnFromFirmware,
                            icon: const BackButtonIcon(),
                          )
                        : null,
                    title: Text(
                      showingFirmware
                          ? litchStrings(context).boardUpdateTitle
                          : litchStrings(context).setupTab,
                      maxLines: 3,
                      style: titleStyle,
                    ),
                    actions:
                        _recoveryOnly ||
                            showingFirmware && !widget.ownerVerificationPending
                        ? null
                        : <Widget>[
                            Padding(
                              padding: const EdgeInsets.only(right: 12),
                              child: SizedBox(
                                key: _boardMenuAnchorKey,
                                width: statusWidth,
                                child: connecting
                                    ? const _BoardStatusLabel(connecting: true)
                                    : _BoardStatusAction(
                                        connected: connected,
                                        largeText: largeToolbar,
                                        updateAvailable:
                                            connected &&
                                            _firmwareAvailability
                                                .updateAvailable,
                                        healthNeedsAttention:
                                            connected && _healthAttention.value,
                                        onTap: connected
                                            ? _showBoardMenu
                                            : widget.onDisconnect,
                                      ),
                              ),
                            ),
                          ],
                  ),
                ),
          body: Stack(
            children: [
              SafeArea(
                // The OS owns every unsafe edge, independent of ribbon side.
                // All pager pages consume these insets once so navigation and
                // board geometry cannot change on a page switch.
                maintainBottomViewPadding: _currentIndex == 4,
                top: landscapeShell,
                bottom: landscapeShell,
                child: AbsorbPointer(
                  absorbing: false,
                  child: IndexedStack(
                    index: showingFirmware ? 1 : 0,
                    children: <Widget>[
                      if (_recoveryOnly)
                        _RetainedMainPage(
                          key: const ValueKey<String>('setup-page'),
                          active: _currentIndex == 2,
                          child: _settingsPage,
                        )
                      else
                        WarmPageView(
                          key: const ValueKey<String>('main-pages'),
                          controller: _mainPageController,
                          children: <Widget>[
                            _RetainedMainPage(
                              key: const ValueKey<String>('play-page'),
                              active: _currentIndex == 1,
                              child: Padding(
                                padding: EdgeInsets.only(
                                  top: playLandscapeVerticalMargin,
                                  bottom: playLandscapeVerticalMargin,
                                ),
                                child: Column(
                                  children: <Widget>[
                                    Expanded(
                                      child: _homeMounted
                                          ? _homePage
                                          : const SizedBox.expand(),
                                    ),
                                  ],
                                ),
                              ),
                            ),
                            _RetainedMainPage(
                              key: const ValueKey('craft-page'),
                              active: _currentIndex == 0,
                              child: LitchcraftScreen(
                                active: _currentIndex == 0,
                                landscapeRibbonSide: _landscapeRibbonSide,
                                deviceLandscapeSideReader:
                                    widget.deviceLandscapeSideReader,
                                rootSafeInsets: rootSafeInsets,
                                controller: _craftController,
                                library: _craftLibrary,
                                landscapeHeader: craftHeader,
                                landscapeFooter: craftFooter,
                                landscapeFooterHeight: craftFooterHeight,
                              ),
                            ),
                            _RetainedMainPage(
                              key: const ValueKey<String>('setup-page'),
                              active: _currentIndex == 2,
                              child: Padding(
                                padding: setupPanePadding,
                                child: _settingsPage,
                              ),
                            ),
                            if (_enableAppConsole)
                              _RetainedMainPage(
                                key: const ValueKey<String>('console-page'),
                                active: _currentIndex == 4,
                                child: Padding(
                                  padding: EdgeInsets.only(
                                    bottom: consoleBottomClearance,
                                  ),
                                  child: _appConsolePage,
                                ),
                              ),
                          ],
                        ),
                      _RetainedMainPage(
                        key: const ValueKey<String>('firmware-page'),
                        active: showingFirmware,
                        child: _firmwarePage,
                      ),
                    ],
                  ),
                ),
              ),
              if (landscapeShell && !landscapeCraft && !_recoveryOnly)
                Positioned(
                  left: landscapeControlSide.x < 0
                      ? rootSafeInsets.left + landscapeEdges.left
                      : null,
                  right: landscapeControlSide.x < 0
                      ? null
                      : rootSafeInsets.right +
                            landscapeGeometry.landscapeEdgeInsets.right,
                  bottom: rootSafeInsets.bottom + landscapeEdges.bottom,
                  width: navigationWidth,
                  child: landscapeFooter,
                ),
            ],
          ),
          bottomNavigationBar: showingFirmware || _recoveryOnly
              ? null
              : landscapeShell
              ? null
              : ColoredBox(
                  key: const ValueKey<String>('main-navigation-surface'),
                  color: _bottomNavigationColor(context),
                  child: _bottomNavigationGroup(
                    context,
                    navigation,
                    fallbackOnLeft: _landscapeRibbonSide.x < 0,
                  ),
                ),
        ),
      ),
    );
  }
}

class _BoardConnectionProgress extends StatefulWidget {
  const _BoardConnectionProgress({
    super.key,
    required this.status,
    required this.hint,
    required this.retryEnabled,
    required this.milestone,
    required this.sessionToken,
    required this.onCancel,
    required this.onRetry,
    required this.support,
  });
  final String status;
  final String? hint;
  final bool retryEnabled;
  final _ConnectionMilestone milestone;
  final int sessionToken;
  final VoidCallback onCancel;
  final Future<void> Function() onRetry;
  final Widget support;

  @override
  State<_BoardConnectionProgress> createState() =>
      _BoardConnectionProgressState();
}

class _BoardConnectionProgressState extends State<_BoardConnectionProgress> {
  static const _stallInterval = Duration(seconds: 3);
  bool _prolonged = false;
  bool _retryPending = false;
  late final Timer _supportTimer;
  Timer? _retryTimer;

  @override
  void initState() {
    super.initState();
    _supportTimer = Timer(
      _stallInterval,
      () => setState(() => _prolonged = true),
    );
    _scheduleRetry();
  }

  @override
  void didUpdateWidget(_BoardConnectionProgress oldWidget) {
    super.didUpdateWidget(oldWidget);
    final advanced =
        widget.milestone.index > oldWidget.milestone.index ||
        (widget.sessionToken != oldWidget.sessionToken &&
            widget.milestone != _ConnectionMilestone.waiting);
    if (advanced || widget.retryEnabled != oldWidget.retryEnabled) {
      _scheduleRetry();
    }
  }

  void _scheduleRetry() {
    _retryTimer?.cancel();
    if (!widget.retryEnabled || _retryPending) return;
    _retryTimer = Timer(_stallInterval, () async {
      _retryPending = true;
      try {
        // Let the replacement finish native admission and SDK fallback.
        // There is only one outstanding attempt for the selected board.
        await widget.onRetry();
      } finally {
        _retryPending = false;
        if (mounted) _scheduleRetry();
      }
    });
  }

  @override
  void dispose() {
    _supportTimer.cancel();
    _retryTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, bounds) {
      final colors = Theme.of(context).colorScheme;
      final compact =
          bounds.maxWidth > 480 && bounds.maxWidth > bounds.maxHeight;
      final identity = Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Semantics(
            liveRegion: true,
            child: Text(
              widget.hint == null
                  ? widget.status
                  : '${widget.status}\n\n${widget.hint}',
              key: const ValueKey('chosen-board-phase'),
              textAlign: TextAlign.center,
            ),
          ),
          const SizedBox(height: 12),
          const LinearProgressIndicator(),
        ],
      );
      final cancel = OutlinedButton(
        key: const ValueKey('chosen-board-cancel'),
        style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
        onPressed: widget.onCancel,
        child: Text(litchStrings(context).cancelAction),
      );
      return Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: ConstrainedBox(
            constraints: BoxConstraints(maxWidth: compact ? 480 : 320),
            child: Material(
              key: const ValueKey('connection-progress-overlay'),
              color: colors.surfaceContainerHigh.withValues(alpha: 0.94),
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(20),
                side: BorderSide(color: colors.outlineVariant),
              ),
              child: Padding(
                key: const ValueKey('chosen-board-connection'),
                padding: const EdgeInsets.all(16),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    compact
                        ? Row(
                            children: <Widget>[
                              Expanded(child: identity),
                              const SizedBox(width: 24),
                              cancel,
                            ],
                          )
                        : Column(
                            mainAxisSize: MainAxisSize.min,
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: <Widget>[
                              identity,
                              const SizedBox(height: 12),
                              cancel,
                            ],
                          ),
                    if (_prolonged) widget.support,
                  ],
                ),
              ),
            ),
          ),
        ),
      );
    },
  );
}

class _DisconnectedStatusIcon extends StatelessWidget {
  const _DisconnectedStatusIcon({this.size = 24});

  final double size;

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: size,
    child: Stack(
      fit: StackFit.expand,
      children: <Widget>[
        Icon(Icons.sensors_off, size: size),
        CustomPaint(
          painter: _DisconnectedSlashPainter(
            color: Theme.of(context).colorScheme.error,
          ),
        ),
      ],
    ),
  );
}

class _DisconnectedSlashPainter extends CustomPainter {
  const _DisconnectedSlashPainter({required this.color});

  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawLine(
      Offset(size.width * .23, size.height * .20),
      Offset(size.width * .78, size.height * .80),
      Paint()
        ..color = color
        ..strokeWidth = size.width * .095
        ..strokeCap = StrokeCap.round,
    );
  }

  @override
  bool shouldRepaint(_DisconnectedSlashPainter oldDelegate) =>
      oldDelegate.color != color;
}

class _CompactAppNavigation extends StatelessWidget {
  const _CompactAppNavigation({
    super.key,
    required this.currentPage,
    required this.onSelected,
    this.statusAction,
    this.consoleEnabled = false,
    this.embeddedToolPanel = false,
    this.onLoadVersionArtwork,
    this.versionTapClock,
  });

  final int currentPage;
  final ValueChanged<int> onSelected;
  final Widget? statusAction;
  final bool consoleEnabled;
  final bool embeddedToolPanel;
  final ValueChanged<int>? onLoadVersionArtwork;
  final DateTime Function()? versionTapClock;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final entries = <(int, String, String, Widget)>[
      (1, 'play', strings.playTab, const LitchPlayGlyph(size: 28)),
      (0, 'craft', strings.craftTitle, const LitchcraftIdentityGlyph(size: 28)),
      (2, 'setup', strings.setupTab, const Icon(Icons.tune_rounded, size: 28)),
      if (consoleEnabled)
        (
          4,
          'console',
          strings.consoleTab,
          const Icon(Icons.terminal, size: 28),
        ),
    ];
    return MediaQuery.removePadding(
      context: context,
      removeTop: true,
      removeBottom: true,
      removeLeft: true,
      removeRight: true,
      child: Padding(
        // Transparent breathing room makes this a floating capsule rather
        // than another full-width screen edge. Its allocated height remains
        // exactly 20% below the former 96 px navigator.
        padding: EdgeInsets.zero,
        child: Material(
          key: ValueKey<String>(
            embeddedToolPanel
                ? 'craft-landscape-navigation-surface'
                : 'main-landscape-navigation-surface',
          ),
          // Keep selected-pill contrast identical inside the Craft inspector.
          // A transparent embedded surface can equal secondaryContainer and
          // make only Craft's selected destination appear unhighlighted.
          color: Theme.of(context).colorScheme.surfaceContainer,
          borderRadius: BorderRadius.circular(LitchShapes.panelRadius),
          clipBehavior: Clip.antiAlias,
          child: Stack(
            key: const ValueKey('landscape-navigation-switcher'),
            children: [
              Column(
                children: [
                  SizedBox(
                    height: 48,
                    child: Row(
                      children: [
                        Expanded(
                          child: Row(
                            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                            children: [
                              for (final entry in entries)
                                Expanded(
                                  child: _CompactNavigationAction(
                                    key: ValueKey(
                                      'landscape-${entry.$2}-action',
                                    ),
                                    label: entry.$3,
                                    selected: entry.$1 == currentPage,
                                    onPressed: () => onSelected(entry.$1),
                                    child: OrientationContentTurn(
                                      child: entry.$4,
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ),
                        if (statusAction != null)
                          SizedBox(width: 48, child: statusAction),
                      ],
                    ),
                  ),
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 10),
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: LitchAboutBrandButton(
                          onLoadVersionArtwork: onLoadVersionArtwork,
                          versionTapClock: versionTapClock,
                          key: currentPage == 0
                              ? const ValueKey('craft-landscape-wordmark')
                              : null,
                          litchcraft: currentPage == 0,
                          wordmarkHeight: 16.25,
                          interactive: true,
                        ),
                      ),
                    ),
                  ),
                ],
              ),
              if (embeddedToolPanel)
                Positioned(
                  left: 0,
                  right: 0,
                  top: 0,
                  child: Divider(
                    height: 1,
                    color: Theme.of(
                      context,
                    ).colorScheme.onSurface.withValues(alpha: .16),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _CompactNavigationAction extends StatelessWidget {
  const _CompactNavigationAction({
    super.key,
    required this.label,
    required this.selected,
    required this.onPressed,
    required this.child,
  });

  final String label;
  final bool selected;
  final VoidCallback onPressed;
  final Widget child;

  @override
  Widget build(BuildContext context) => Semantics(
    button: true,
    selected: selected,
    label: label,
    onTap: onPressed,
    excludeSemantics: true,
    child: Tooltip(
      message: label,
      child: SizedBox.square(
        dimension: 48,
        child: Center(
          child: Material(
            color: Colors.transparent,
            child: InkWell(
              borderRadius: BorderRadius.circular(16),
              onTap: onPressed,
              child: SizedBox.square(
                dimension: 48,
                child: Center(
                  child: Ink(
                    width: 44,
                    height: 36,
                    decoration: BoxDecoration(
                      color: selected
                          ? Theme.of(context).colorScheme.secondaryContainer
                          : Colors.transparent,
                      borderRadius: BorderRadius.circular(16),
                    ),
                    child: Center(child: child),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  );
}

// Retain each screen's feed, scroll and in-progress controls across page changes.
class _RetainedMainPage extends StatefulWidget {
  const _RetainedMainPage({
    super.key,
    required this.child,
    required this.active,
  });
  final Widget child;
  final bool active;

  @override
  State<_RetainedMainPage> createState() => _RetainedMainPageState();
}

class _RetainedMainPageState extends State<_RetainedMainPage>
    with AutomaticKeepAliveClientMixin<_RetainedMainPage> {
  @override
  bool get wantKeepAlive => true;

  @override
  Widget build(BuildContext context) {
    super.build(context);
    // Retain the last painted page as well as its State. In particular this
    // keeps the large Litchcraft canvas/tool composition from joining the app
    // navigation repaint when it is revealed from Play.
    return RepaintBoundary(
      // A retained nested route must not block another page or the shell's
      // navigation semantics. Keep inactive pages warm without exposing them.
      child: Semantics(
        container: true,
        explicitChildNodes: true,
        child: ExcludeSemantics(
          excluding: !widget.active,
          child: TickerMode(enabled: widget.active, child: widget.child),
        ),
      ),
    );
  }
}

class _ConnectedBoardSheet extends StatefulWidget {
  const _ConnectedBoardSheet({
    required this.bleService,
    required this.boardId,
    required this.boardName,
    required this.healthNeedsAttention,
    required this.updateAvailable,
    required this.installedVersion,
    required this.latestVersion,
    required this.updateInProgress,
    required this.onShowHealth,
    required this.onShowFirmware,
    required this.onDisconnect,
  });

  final LitchBleService bleService;
  final String boardId;
  final String? boardName;
  final bool healthNeedsAttention;
  final bool updateAvailable;
  final String? installedVersion;
  final String? latestVersion;
  final bool updateInProgress;
  final VoidCallback onShowHealth;
  final VoidCallback onShowFirmware;
  final VoidCallback onDisconnect;

  @override
  State<_ConnectedBoardSheet> createState() => _ConnectedBoardSheetState();
}

class _ConnectedBoardSheetState extends State<_ConnectedBoardSheet> {
  static const String _boardNameSetting = 'BLE_DEVICE_NAME';
  static const String _defaultBoardName = 'Foxie Litch';
  static const int _friendlyNameCharacters = 20;
  static const int _friendlyNameBytes = 31;
  static const Duration _brightnessReportDeadline = Duration(seconds: 2);
  bool? _automaticBrightness;
  bool? _rotated;
  double? _brightnessDraft;
  ({bool healthNeedsAttention, bool? automaticBrightness})? _brightnessDrag;
  int? _brightnessAwaitingReport;
  bool _brightnessReportTimedOut = false;
  String? _inlineError;
  final Set<String> _writing = <String>{};
  Timer? _brightnessPreviewTimer;
  Timer? _brightnessReportTimer;
  Future<void>? _brightnessPreviewWrite;
  int? _pendingBrightnessPreview;
  String? _friendlyName;
  bool _boardNameResolved = false;
  bool _sleepPending = false;
  Timer? _sleepConfirmationTimer;
  Completer<void>? _sleepConfirmation;
  String? _sleepError;
  final GlobalKey _sleepRecoveryKey = GlobalKey();
  StreamSubscription<String>? _settingsChanges;

  String get _friendlyNameKey => 'board_friendly_name:${widget.boardId}';

  @override
  void dispose() {
    _brightnessPreviewTimer?.cancel();
    _brightnessReportTimer?.cancel();
    _sleepConfirmationTimer?.cancel();
    final sleepConfirmation = _sleepConfirmation;
    if (sleepConfirmation != null && !sleepConfirmation.isCompleted) {
      sleepConfirmation.complete();
    }
    _pendingBrightnessPreview = null;
    unawaited(_settingsChanges?.cancel());
    super.dispose();
  }

  @override
  void initState() {
    super.initState();
    _syncCachedSettings();
    _settingsChanges = widget.bleService.settingsChangedStream.listen((key) {
      if (key == 'BRIGHTNESS' ||
          key == 'AUTO_BRIGHTNESS' ||
          key == 'BOARD_ROTATE') {
        _syncCachedSettings();
      } else if (key == _boardNameSetting) {
        unawaited(_loadFriendlyName());
      }
    });
    unawaited(_loadFriendlyName());
  }

  void _syncCachedSettings() {
    final int? brightness = _valueInt(
      widget.bleService.latestSettingValue('BRIGHTNESS'),
    );
    final bool? automatic = _valueBool(
      widget.bleService.latestSettingValue('AUTO_BRIGHTNESS'),
    );
    final bool? rotated = _valueBool(
      widget.bleService.latestSettingValue('BOARD_ROTATE'),
    );
    if (!mounted) return;
    setState(() {
      if (_brightnessDrag == null &&
          !_writing.contains('BRIGHTNESS') &&
          brightness != null) {
        final pending = _brightnessAwaitingReport;
        if (pending == null || brightness == pending) {
          if (pending != null) {
            _brightnessReportTimer?.cancel();
            _brightnessReportTimer = null;
            _brightnessReportTimedOut = false;
            final strings = litchStrings(context);
            if (_inlineError ==
                strings.settingUnconfirmed(strings.brightness)) {
              _inlineError = null;
            }
          }
          _brightnessAwaitingReport = null;
          _brightnessDraft = brightness.toDouble();
        } else if (_brightnessReportTimedOut) {
          _brightnessDraft = brightness.toDouble();
        }
      }
      if (!_writing.contains('AUTO_BRIGHTNESS') && automatic != null) {
        _automaticBrightness = automatic;
      }
      if (!_writing.contains('BOARD_ROTATE') && rotated != null) {
        _rotated = rotated;
      }
    });
  }

  Future<void> _loadFriendlyName() async {
    final preferences = await SharedPreferences.getInstance();
    final localCandidate = _validatedBoardName(
      preferences.getString(_friendlyNameKey),
    );
    Object? rawBoardName = widget.bleService.latestSettingValue(
      _boardNameSetting,
    );
    if (rawBoardName == null) {
      try {
        rawBoardName = await widget.bleService.readSettingValue(
          deviceId: widget.boardId,
          key: _boardNameSetting,
        );
      } catch (_) {}
    }
    if (!mounted) return;
    final boardCandidate = _validatedBoardName(rawBoardName);
    if (boardCandidate != null &&
        (localCandidate == boardCandidate ||
            boardCandidate != _defaultBoardName)) {
      await preferences.remove(_friendlyNameKey);
    }
    if (!mounted) return;
    setState(() {
      _friendlyName = _displayBoardName(boardCandidate ?? localCandidate);
      _boardNameResolved = true;
    });
  }

  String? _validatedBoardName(Object? value) {
    final candidate = value?.toString();
    if (candidate == null ||
        candidate.isEmpty ||
        !isDisplayTextWithinBounds(
          candidate,
          maxCharacters: _friendlyNameCharacters,
          maxBytes: _friendlyNameBytes,
        )) {
      return null;
    }
    return candidate;
  }

  String? _displayBoardName(String? value) =>
      value == null || value == _defaultBoardName ? null : value;

  Future<void> _renameBoard() async {
    final strings = litchStrings(context);
    String draft = _friendlyName ?? strings.boardFriendlyNameSuggestion;
    String? error;
    final name = await showDialog<String?>(
      context: context,
      builder: (dialogContext) => StatefulBuilder(
        builder: (dialogContext, setDialogState) {
          void submit() {
            final candidate = draft.trim();
            if (candidate.isEmpty ||
                !isDisplayTextWithinBounds(
                  candidate,
                  maxCharacters: _friendlyNameCharacters,
                  maxBytes: _friendlyNameBytes,
                )) {
              setDialogState(() => error = strings.boardFriendlyNameInvalid);
              return;
            }
            Navigator.of(dialogContext).pop(candidate);
          }

          return AlertDialog(
            title: Text(strings.boardFriendlyNameTitle),
            content: TextFormField(
              key: const ValueKey('board-friendly-name-field'),
              initialValue: draft,
              autofocus: true,
              maxLines: 1,
              maxLength: _friendlyNameCharacters,
              inputFormatters: <TextInputFormatter>[
                LengthLimitingTextInputFormatter(_friendlyNameCharacters),
              ],
              decoration: InputDecoration(
                labelText: strings.boardFriendlyNameLabel,
                errorText: error,
              ),
              onChanged: (value) => draft = value,
              textInputAction: TextInputAction.done,
              onFieldSubmitted: (_) => submit(),
            ),
            actions: <Widget>[
              TextButton(
                key: const ValueKey('board-friendly-name-reset'),
                onPressed: () => Navigator.of(dialogContext).pop(''),
                child: Text(strings.boardFriendlyNameReset),
              ),
              TextButton(
                onPressed: () => Navigator.of(dialogContext).pop(),
                child: Text(strings.cancelAction),
              ),
              FilledButton(
                key: const ValueKey('board-friendly-name-save'),
                onPressed: submit,
                child: Text(strings.craftSave),
              ),
            ],
          );
        },
      ),
    );
    if (name == null) return;
    final preferences = await SharedPreferences.getInstance();
    final target = name.isEmpty ? _defaultBoardName : name;
    // This preference is only a reconnect-safe pending write. The board is the
    // authority and a confirmed write removes the local copy.
    await preferences.setString(_friendlyNameKey, target);
    if (!mounted) return;
    setState(() {
      _friendlyName = _displayBoardName(target);
      _boardNameResolved = true;
      _writing.add(_boardNameSetting);
      _inlineError = null;
    });
    final result = await widget.bleService.writeSettingAcknowledged(
      deviceId: widget.boardId,
      key: _boardNameSetting,
      value: target,
    );
    bool applied = switch (result) {
      LitchSettingWriteAcknowledged(:final outcome) =>
        outcome.ok &&
            (!outcome.hasValue || outcome.value?.toString() == target),
      LitchSettingWriteNotAcknowledged() => false,
    };
    if (!applied) {
      try {
        applied =
            (await widget.bleService.readSettingValue(
              deviceId: widget.boardId,
              key: _boardNameSetting,
            ))?.toString() ==
            target;
      } catch (_) {}
    }
    if (applied) await preferences.remove(_friendlyNameKey);
    if (!mounted) return;
    setState(() {
      _writing.remove(_boardNameSetting);
      if (!applied && result is LitchSettingWriteAcknowledged) {
        _inlineError = litchStrings(context).boardFriendlyNameApplyFailed;
      }
      // A rename restarts BLE. Missing the final acknowledgement during that
      // expected reconnect is pending, not a user-visible apply failure; the
      // next exact-key read reconciles the board-owned name.
    });
  }

  int? _valueInt(Object? value) {
    return value is int ? value : int.tryParse(value?.toString() ?? '');
  }

  bool? _valueBool(Object? value) {
    if (value is bool) return value;
    if (value == null) return null;
    if (value.toString().toLowerCase() == 'true' || value == 1) return true;
    if (value.toString().toLowerCase() == 'false' || value == 0) return false;
    return null;
  }

  Future<({bool ok, bool authoritative, Object? value})> _write(
    String key,
    Object value, {
    Future<void> Function()? beforeWrite,
  }) async {
    if (_writing.contains(key)) {
      return (ok: false, authoritative: false, value: null);
    }
    setState(() {
      _writing.add(key);
      _inlineError = null;
    });
    await beforeWrite?.call();
    if (!mounted) {
      return (ok: false, authoritative: false, value: null);
    }
    final LitchSettingWriteResult result = await widget.bleService
        .writeSettingAcknowledged(
          deviceId: widget.boardId,
          key: key,
          value: value,
        );
    final LitchSettingWriteAcknowledged? acknowledgedResult =
        result is LitchSettingWriteAcknowledged ? result : null;
    final bool acknowledged = acknowledgedResult?.outcome.ok ?? false;
    bool stateConfirmed = true;
    if (acknowledgedResult?.boardStateConfirmation case final confirmation?) {
      try {
        stateConfirmed = await confirmation;
      } catch (_) {
        stateConfirmed = false;
      }
    }
    Object? effectiveValue;
    bool authoritative = false;
    if (acknowledged && stateConfirmed) {
      effectiveValue = acknowledgedResult!.outcome.hasValue
          ? acknowledgedResult.outcome.value
          : value;
      if (acknowledgedResult.boardStateConfirmation != null &&
          widget.bleService.hasBoardStateSnapshot) {
        effectiveValue =
            widget.bleService.latestSettingValue(key) ?? effectiveValue;
      }
      authoritative = effectiveValue != null;
    } else if (acknowledged && !stateConfirmed) {
      if (widget.bleService.hasBoardStateSnapshot &&
          (key == 'AUTO_BRIGHTNESS' || key == 'BOARD_ROTATE')) {
        effectiveValue = widget.bleService.latestSettingValue(key);
        authoritative = effectiveValue != null;
      }
    } else {
      try {
        effectiveValue = await widget.bleService.readSettingValue(
          deviceId: widget.boardId,
          key: key,
        );
        authoritative = effectiveValue != null;
      } catch (_) {
        // The optimistic value remains visible with an explicit unconfirmed
        // message until a fresh board-owned value becomes available.
      }
    }
    final bool ok =
        (acknowledged && stateConfirmed && authoritative) ||
        (authoritative && effectiveValue == value);
    if (!mounted) {
      return (ok: ok, authoritative: authoritative, value: effectiveValue);
    }
    setState(() {
      _writing.remove(key);
      if (!ok) {
        final String name = switch (key) {
          'BRIGHTNESS' => litchStrings(context).brightness,
          'AUTO_BRIGHTNESS' => litchStrings(context).automaticBrightness,
          'BOARD_ROTATE' => litchStrings(context).rotateBoard,
          _ => key,
        };
        _inlineError = authoritative
            ? litchStrings(context).settingApplyFailed(name)
            : litchStrings(context).settingUnconfirmed(name);
      }
    });
    return (ok: ok, authoritative: authoritative, value: effectiveValue);
  }

  Future<void> _setAutomaticBrightness(bool value) async {
    final bool? previous = _automaticBrightness;
    setState(() => _automaticBrightness = value);
    final result = await _write('AUTO_BRIGHTNESS', value);
    if (!mounted) return;
    setState(() {
      _automaticBrightness = result.authoritative
          ? _valueBool(result.value) ?? previous
          : value;
    });
  }

  Future<void> _drainBrightnessPreviews() async {
    _brightnessPreviewTimer?.cancel();
    while (mounted &&
        (_brightnessPreviewWrite != null ||
            _pendingBrightnessPreview != null)) {
      if (_brightnessPreviewWrite == null) _flushBrightnessPreview();
      final Future<void>? preview = _brightnessPreviewWrite;
      if (preview != null) await preview;
    }
  }

  Future<void> _finishBrightness(double value) async {
    _brightnessDrag = null;
    _brightnessReportTimer?.cancel();
    _brightnessReportTimer = null;
    _brightnessReportTimedOut = false;
    final int rounded = value.round();
    _brightnessAwaitingReport = rounded;
    final result = await _write(
      'BRIGHTNESS',
      rounded,
      beforeWrite: _drainBrightnessPreviews,
    );
    if (!mounted) return;
    if (_brightnessAwaitingReport != rounded) return;
    final int? effective = _valueInt(result.value);
    // A state receipt may still carry an old or intermediate board value.
    // Only the requested level settles this optimistic preview.
    if (result.ok && effective != rounded) {
      _brightnessReportTimer = Timer(_brightnessReportDeadline, () {
        if (!mounted || _brightnessAwaitingReport != rounded) return;
        final current = _valueInt(
          widget.bleService.latestSettingValue('BRIGHTNESS'),
        );
        setState(() {
          _brightnessReportTimer = null;
          if (current != null) _brightnessDraft = current.toDouble();
          if (current != rounded) {
            _brightnessReportTimedOut = true;
            final strings = litchStrings(context);
            _inlineError = strings.settingUnconfirmed(strings.brightness);
          } else {
            _brightnessAwaitingReport = null;
            _brightnessReportTimedOut = false;
          }
        });
      });
      return;
    }
    _brightnessAwaitingReport = null;
    _brightnessReportTimedOut = false;
    if (effective == null) return;
    setState(() => _brightnessDraft = effective.toDouble());
  }

  void _previewBrightness(double value) {
    setState(() => _brightnessDraft = value);
    _pendingBrightnessPreview = value.round();
    _brightnessPreviewTimer?.cancel();
    _brightnessPreviewTimer = Timer(
      const Duration(milliseconds: 30),
      _flushBrightnessPreview,
    );
  }

  void _flushBrightnessPreview() {
    if (_brightnessPreviewWrite != null) return;
    final int? value = _pendingBrightnessPreview;
    if (value == null) return;
    _pendingBrightnessPreview = null;
    final Future<void> write = widget.bleService
        .writeSetting(
          deviceId: widget.boardId,
          key: 'BRIGHTNESS',
          value: value,
        )
        .then<void>((_) {});
    _brightnessPreviewWrite = write;
    unawaited(
      write.whenComplete(() {
        if (!identical(_brightnessPreviewWrite, write)) return;
        _brightnessPreviewWrite = null;
        if (mounted && _pendingBrightnessPreview != null) {
          _flushBrightnessPreview();
        }
      }),
    );
  }

  Future<void> _sleep() async {
    if (_sleepPending || widget.updateInProgress) return;
    final requestRoute = ModalRoute.of(context);
    final boardId = widget.boardId;
    final token = widget.bleService.connectionSessionToken;
    setState(() {
      _sleepPending = true;
      _sleepError = null;
    });
    final outcome = await widget.bleService.requestSleep(
      deviceId: widget.boardId,
    );
    if (!mounted ||
        requestRoute?.isActive != true ||
        widget.boardId != boardId ||
        !widget.bleService.isConnected ||
        widget.bleService.connectedId != boardId ||
        widget.bleService.connectionSessionToken != token) {
      return;
    }
    if (outcome?.disposition == ConnectionSessionDisposition.applied) {
      // Applied starts the board's sleep transition. Its actual disconnect
      // retires this menu through the existing connection lifecycle owner.
      // Remaining connected past this presentation bound establishes only
      // that sleep is unconfirmed; it cannot establish success or failure.
      final confirmation = Completer<void>();
      _sleepConfirmation = confirmation;
      _sleepConfirmationTimer = Timer(
        const Duration(seconds: 6),
        confirmation.complete,
      );
      await confirmation.future;
      _sleepConfirmation = null;
      _sleepConfirmationTimer = null;
      if (!mounted ||
          requestRoute?.isActive != true ||
          widget.boardId != boardId ||
          !widget.bleService.isConnected ||
          widget.bleService.connectedId != boardId ||
          widget.bleService.connectionSessionToken != token) {
        return;
      }
    }
    final strings = litchStrings(context);
    final reason = LitchMutationReason.fromWireCode(outcome?.reason ?? -1);
    setState(() {
      _sleepPending = false;
      _sleepError = switch (reason) {
        LitchMutationReason.busy ||
        LitchMutationReason.queueFull => strings.actionBusy,
        LitchMutationReason.unsupported || LitchMutationReason.unknownAction =>
          strings.actionUnavailable(strings.sleepAction),
        _ =>
          outcome?.disposition == ConnectionSessionDisposition.rejected
              ? strings.actionApplyFailed
              : strings.actionUnconfirmed(strings.sleepAction),
      };
    });
    // Keep the action and its recovery message together in the existing menu
    // scroller, including when enlarged text makes the message taller.
    await WidgetsBinding.instance.endOfFrame;
    final recoveryContext = _sleepRecoveryKey.currentContext;
    if (requestRoute?.isCurrent == true &&
        recoveryContext != null &&
        recoveryContext.mounted) {
      await Scrollable.ensureVisible(
        recoveryContext,
        alignment: 1,
        duration: const Duration(milliseconds: 150),
      );
    }
  }

  bool get _expandedMenuLabels =>
      MediaQuery.textScalerOf(context).scale(16) > 32;

  Widget _menuSwitch(SwitchListTile tile) => LayoutBuilder(
    builder: (context, constraints) {
      final title = tile.title;
      final contentPadding =
          (tile.contentPadding ??
                  ListTileTheme.of(context).contentPadding ??
                  const EdgeInsetsDirectional.only(start: 16, end: 24))
              .resolve(Directionality.of(context));
      final titleWidth = math.max(
        1.0,
        constraints.maxWidth -
            contentPadding.horizontal -
            (tile.secondary == null ? 0 : 56) -
            76,
      );
      final style =
          ListTileTheme.of(context).titleTextStyle ??
          Theme.of(context).textTheme.bodyLarge!;
      bool wordFits(String word) {
        final painter = TextPainter(
          text: TextSpan(text: word, style: style),
          textDirection: Directionality.of(context),
          textScaler: MediaQuery.textScalerOf(context),
          locale: Localizations.localeOf(context),
        )..layout();
        final fits = painter.width <= titleWidth;
        painter.dispose();
        return fits;
      }

      final needsFullWidth =
          _expandedMenuLabels ||
          (title is Text &&
              (title.data ?? '')
                  .split(RegExp(r'\s+'))
                  .any((word) => !wordFits(word)));
      if (!needsFullWidth) return tile;
      final onChanged = tile.onChanged;
      return MergeSemantics(
        child: ListTile(
          key: tile.key,
          enabled: onChanged != null,
          visualDensity: tile.visualDensity,
          title: tile.title,
          subtitle: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              ?tile.subtitle,
              Row(
                children: [
                  ?tile.secondary,
                  const Spacer(),
                  ExcludeFocus(
                    child: Switch.adaptive(
                      value: tile.value,
                      onChanged: onChanged,
                    ),
                  ),
                ],
              ),
            ],
          ),
          onTap: onChanged == null ? null : () => onChanged(!tile.value),
        ),
      );
    },
  );

  Widget _connectionAction({
    required Key key,
    required IconData icon,
    required String label,
    required VoidCallback? onPressed,
    bool pending = false,
    bool alignEnd = false,
  }) {
    final iconWidget = pending
        ? const SizedBox.square(
            dimension: 24,
            child: CircularProgressIndicator(strokeWidth: 2),
          )
        : Icon(icon, size: 24);
    final text = Flexible(
      child: Text(label, textAlign: alignEnd ? TextAlign.end : TextAlign.start),
    );
    return TextButton(
      key: key,
      style: TextButton.styleFrom(
        minimumSize: const Size(48, 48),
        padding: const EdgeInsets.symmetric(vertical: 8),
        alignment: alignEnd
            ? AlignmentDirectional.centerEnd
            : AlignmentDirectional.centerStart,
      ),
      onPressed: onPressed,
      child: Semantics(
        label: label,
        liveRegion: pending,
        child: ExcludeSemantics(
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: alignEnd
                ? [text, const SizedBox(width: 12), iconWidget]
                : [iconWidget, const SizedBox(width: 12), text],
          ),
        ),
      ),
    );
  }

  Widget _connectionActions() => LitchCompactControls(
    child: LayoutBuilder(
      builder: (context, constraints) {
        final strings = litchStrings(context);
        final style = Theme.of(context).textTheme.labelLarge!;
        final scaler = LitchCompactControls.textScalerOf(context);
        Size measure(String text, double width) {
          final painter = TextPainter(
            text: TextSpan(text: text, style: style),
            textDirection: Directionality.of(context),
            textScaler: scaler,
            locale: Localizations.localeOf(context),
          )..layout(maxWidth: math.max(1, width));
          final size = painter.size;
          painter.dispose();
          return size;
        }

        const iconExtent = 36.0;
        final disconnectWidth = measure(
          strings.disconnectAction,
          double.infinity,
        ).width;
        final normalSleepWidth = measure(
          strings.sleepAction,
          double.infinity,
        ).width;
        final pendingSleepWidth = measure(
          strings.sleeping,
          double.infinity,
        ).width;
        final sleepWidth =
            disconnectWidth + pendingSleepWidth + iconExtent * 2 <=
                constraints.maxWidth
            ? math.max(normalSleepWidth, pendingSleepWidth)
            : normalSleepWidth;
        final naturalWidth = disconnectWidth + sleepWidth + iconExtent * 2;
        final gap = (constraints.maxWidth - naturalWidth).clamp(0.0, 16.0);
        final available = math.max(0.0, constraints.maxWidth - gap);
        final spare = math.max(
          0.0,
          available - disconnectWidth - sleepWidth - iconExtent * 2,
        );
        final firstWidth = spare > 0
            ? disconnectWidth + iconExtent + spare / 2
            : available * (disconnectWidth + iconExtent) / naturalWidth;
        final secondWidth = math.max(0.0, available - firstWidth);
        final height = math.max(
          48.0,
          [
                measure(
                  strings.disconnectAction,
                  firstWidth - iconExtent,
                ).height,
                measure(strings.sleepAction, secondWidth - iconExtent).height,
                measure(strings.sleeping, secondWidth - iconExtent).height,
              ].reduce(math.max) +
              16,
        );
        return SizedBox(
          height: height,
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              SizedBox(
                width: firstWidth,
                child: _connectionAction(
                  key: const ValueKey('board-menu-disconnect'),
                  icon: Icons.link_off_rounded,
                  label: strings.disconnectAction,
                  onPressed: widget.updateInProgress || _sleepPending
                      ? null
                      : widget.onDisconnect,
                ),
              ),
              SizedBox(width: gap),
              SizedBox(
                width: secondWidth,
                child: _connectionAction(
                  key: const ValueKey('board-menu-sleep'),
                  icon: Icons.bedtime_outlined,
                  label: _sleepPending ? strings.sleeping : strings.sleepAction,
                  pending: _sleepPending,
                  alignEnd: true,
                  onPressed: widget.updateInProgress || _sleepPending
                      ? null
                      : () => unawaited(_sleep()),
                ),
              ),
            ],
          ),
        );
      },
    ),
  );

  Future<void> _setRotated(bool next) async {
    final bool? previous = _rotated;
    if (previous == null) return;
    setState(() => _rotated = next);
    final result = await _write('BOARD_ROTATE', next);
    if (!mounted) return;
    setState(() {
      _rotated = result.authoritative
          ? _valueBool(result.value) ?? previous
          : next;
    });
  }

  @override
  Widget build(BuildContext context) {
    int min = 0;
    int max = 8;
    for (final setting in widget.bleService.immediateSettingsSnapshot(
      widget.boardId,
    )) {
      if (setting.key != 'BRIGHTNESS') continue;
      final int advertisedMin = setting.min ?? min;
      final int advertisedMax = setting.max ?? max;
      if (advertisedMax > advertisedMin) {
        min = advertisedMin;
        max = advertisedMax;
      }
      break;
    }
    final String? visibleError = _inlineError;
    final menuContentPadding =
        ListTileTheme.of(context).contentPadding ??
        (Theme.of(context).useMaterial3
            ? const EdgeInsetsDirectional.only(start: 16, end: 24)
            : const EdgeInsets.symmetric(horizontal: 16));
    final double value = (_brightnessDraft ?? min.toDouble()).clamp(
      min.toDouble(),
      max.toDouble(),
    );
    String brightnessLabel(double level) => level.round() == 0
        ? litchStrings(context).nearDark
        : level.round().toString();
    final automaticForLabel =
        _brightnessDrag?.automaticBrightness ?? _automaticBrightness;
    final brightnessTitle = automaticForLabel == true
        ? litchStrings(context).maximumBrightness
        : litchStrings(context).brightness;
    final brightnessIcon = Icon(
      Icons.brightness_6_outlined,
      color: _sleepPending ? Theme.of(context).disabledColor : null,
    );
    final brightnessHeading = Text(
      brightnessTitle,
      style: _sleepPending
          ? TextStyle(color: Theme.of(context).disabledColor)
          : null,
    );
    final brightnessValue = Text(
      brightnessLabel(value),
      textAlign: TextAlign.end,
      style: _sleepPending
          ? TextStyle(color: Theme.of(context).disabledColor)
          : null,
    );
    final brightnessHeader = LayoutBuilder(
      builder: (context, constraints) {
        final style = DefaultTextStyle.of(context).style;
        Size measure(String text, [double width = double.infinity]) {
          final painter = TextPainter(
            text: TextSpan(text: text, style: style),
            textDirection: Directionality.of(context),
            textScaler: MediaQuery.textScalerOf(context),
            locale: Localizations.localeOf(context),
          )..layout(maxWidth: width);
          final size = painter.size;
          painter.dispose();
          return size;
        }

        final nearDark = litchStrings(context).nearDark;
        final valueWidth = math.max(
          measure(nearDark).width,
          measure('$max').width,
        );
        final stacked =
            _expandedMenuLabels ||
            measure(brightnessTitle).width + valueWidth + 48 >
                constraints.maxWidth;
        // Reserve the zero label even while a numeric value is showing. A
        // held drag must not move the slider as Near dark appears/disappears.
        final valueExtent = stacked
            ? math.max(0.0, constraints.maxWidth - 36)
            : valueWidth;
        final heldValue = SizedBox(
          width: valueExtent,
          height: math.max(
            measure(nearDark, valueExtent).height,
            measure('$max', valueExtent).height,
          ),
          child: Align(
            alignment: AlignmentDirectional.centerEnd,
            child: brightnessValue,
          ),
        );
        return stacked
            ? Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  brightnessHeading,
                  Row(
                    children: [
                      brightnessIcon,
                      const SizedBox(width: 12),
                      Expanded(child: heldValue),
                    ],
                  ),
                ],
              )
            : Row(
                children: [
                  brightnessIcon,
                  const SizedBox(width: 12),
                  Expanded(child: brightnessHeading),
                  const SizedBox(width: 12),
                  heldValue,
                ],
              );
      },
    );
    return ConstrainedBox(
      constraints: const BoxConstraints(maxHeight: 640),
      child: ListView(
        key: const ValueKey<String>('board-menu-scroll'),
        primary: false,
        shrinkWrap: true,
        padding: EdgeInsets.zero,
        children: <Widget>[
          Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              ListTile(
                key: const ValueKey('board-menu-name'),
                enabled: !_sleepPending,
                minTileHeight: 56,
                visualDensity: VisualDensity.compact,
                leading: _expandedMenuLabels
                    ? null
                    : const Icon(Icons.developer_board_outlined),
                title: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    if (_expandedMenuLabels)
                      const Row(
                        children: [
                          Icon(Icons.developer_board_outlined),
                          Spacer(),
                          Icon(Icons.edit_outlined),
                        ],
                      ),
                    Text(
                      _boardNameResolved
                          ? _friendlyName ??
                                litchStrings(context).boardConnected
                          : widget.boardName ??
                                litchStrings(context).boardConnected,
                    ),
                  ],
                ),
                subtitle: Text(
                  litchStrings(
                    context,
                  ).boardIdentity(_shortLitchId(widget.boardId)),
                ),
                trailing: _expandedMenuLabels
                    ? null
                    : const Icon(Icons.edit_outlined),
                onTap: _sleepPending || _writing.contains(_boardNameSetting)
                    ? null
                    : _renameBoard,
              ),
              if (_brightnessDrag?.healthNeedsAttention ??
                  widget.healthNeedsAttention) ...<Widget>[
                const Divider(height: 1),
                ListTile(
                  key: const ValueKey<String>('board-menu-health'),
                  enabled: !_sleepPending && !widget.updateInProgress,
                  minTileHeight: 48,
                  visualDensity: VisualDensity.compact,
                  leading: Icon(
                    Icons.warning_amber_rounded,
                    color: _sleepPending
                        ? Theme.of(context).disabledColor
                        : Theme.of(context).colorScheme.secondary,
                  ),
                  title: Text(litchStrings(context).boardHealthTitle),
                  subtitle: Text(
                    litchStrings(context).boardHealthNeedsAttention,
                  ),
                  onTap: _sleepPending || widget.updateInProgress
                      ? null
                      : widget.onShowHealth,
                ),
              ],
              ...<Widget>[
                if (visibleError case final error?)
                  Semantics(
                    key: const ValueKey('board-menu-inline-error'),
                    container: true,
                    liveRegion: true,
                    child: ListTile(
                      leading: Icon(
                        Icons.error_outline_rounded,
                        color: Theme.of(context).colorScheme.error,
                      ),
                      title: Text(error),
                    ),
                  ),
                if (_brightnessDraft != null) ...<Widget>[
                  Padding(
                    key: const ValueKey('board-menu-brightness'),
                    padding: menuContentPadding.add(
                      const EdgeInsets.only(top: 4),
                    ),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: <Widget>[
                        brightnessHeader,
                        Semantics(
                          label: brightnessTitle,
                          child: SizedBox(
                            height: 48,
                            child: Slider(
                              padding: EdgeInsets.zero,
                              value: value,
                              min: min.toDouble(),
                              max: max.toDouble(),
                              divisions: max > min ? max - min : null,
                              label: brightnessLabel(value),
                              semanticFormatterCallback: (double rawValue) =>
                                  '$brightnessTitle, ${brightnessLabel(rawValue)}',
                              onChanged:
                                  _sleepPending ||
                                      _writing.contains('BRIGHTNESS')
                                  ? null
                                  : _previewBrightness,
                              onChangeStart: (_) {
                                // Incoming health/mode reports must not reflow
                                // the slider under a held pointer. Publish their
                                // current layout when the gesture commits.
                                _brightnessDrag = (
                                  healthNeedsAttention:
                                      widget.healthNeedsAttention,
                                  automaticBrightness: _automaticBrightness,
                                );
                                _brightnessReportTimer?.cancel();
                                _brightnessReportTimer = null;
                                _brightnessAwaitingReport = null;
                                _brightnessReportTimedOut = false;
                              },
                              onChangeEnd: _finishBrightness,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
                if (_automaticBrightness != null)
                  _menuSwitch(
                    SwitchListTile.adaptive(
                      key: const ValueKey('board-menu-auto-brightness'),
                      visualDensity: VisualDensity.compact,
                      secondary: const Icon(Icons.brightness_auto_outlined),
                      title: Text(litchStrings(context).automaticBrightness),
                      subtitle: value.round() == 0
                          ? Text(litchStrings(context).automaticBrightnessHelp)
                          : null,
                      value: _automaticBrightness!,
                      onChanged:
                          _sleepPending ||
                              value.round() == 0 ||
                              _writing.contains('AUTO_BRIGHTNESS')
                          ? null
                          : _setAutomaticBrightness,
                    ),
                  ),
                const Divider(height: 1),
                _menuSwitch(
                  SwitchListTile.adaptive(
                    key: const ValueKey('board-menu-rotate'),
                    visualDensity: VisualDensity.compact,
                    secondary: const Icon(Icons.rotate_right_outlined),
                    title: Text(litchStrings(context).rotateBoard),
                    value: _rotated ?? false,
                    onChanged:
                        _sleepPending ||
                            _rotated == null ||
                            _writing.contains('BOARD_ROTATE')
                        ? null
                        : _setRotated,
                  ),
                ),
              ],
              const Divider(height: 1),
              ListTile(
                enabled: !_sleepPending,
                minTileHeight: 48,
                visualDensity: VisualDensity.compact,
                leading: Icon(
                  widget.updateAvailable
                      ? Icons.system_update_alt_rounded
                      : Icons.system_update_outlined,
                ),
                title: Text(
                  litchStrings(context).boardSoftware,
                  textScaler: LitchCompactControls.textScalerOf(context),
                ),
                subtitle:
                    widget.updateAvailable &&
                        widget.installedVersion != null &&
                        widget.latestVersion != null
                    ? Text(
                        '${litchStrings(context).currentFirmwareVersion(widget.installedVersion!)} → ${widget.latestVersion}',
                      )
                    : widget.installedVersion != null
                    ? Text(
                        litchStrings(
                          context,
                        ).currentFirmwareVersion(widget.installedVersion!),
                      )
                    : null,
                onTap: _sleepPending ? null : widget.onShowFirmware,
              ),
              const Divider(height: 1),
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 6),
                child: Column(
                  key: _sleepRecoveryKey,
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    if (_sleepError case final error?)
                      Semantics(
                        key: const ValueKey('board-menu-sleep-error'),
                        container: true,
                        liveRegion: true,
                        child: ListTile(
                          leading: Icon(
                            Icons.error_outline_rounded,
                            color: Theme.of(context).colorScheme.error,
                          ),
                          title: Text(error),
                        ),
                      ),
                    Padding(
                      padding: menuContentPadding,
                      child: _connectionActions(),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

double _toolbarTextHeight(
  BuildContext context,
  String text,
  TextStyle style,
  double maxWidth, {
  int? maxLines,
}) {
  final painter = TextPainter(
    text: TextSpan(text: text, style: style),
    textDirection: Directionality.of(context),
    textScaler: LitchCompactControls.textScalerOf(context),
    locale: Localizations.localeOf(context),
    maxLines: maxLines,
  )..layout(maxWidth: maxWidth > 0 ? maxWidth : 1);
  final height = painter.height;
  painter.dispose();
  return height;
}

class _BoardStatusAction extends StatefulWidget {
  const _BoardStatusAction({
    required this.connected,
    required this.largeText,
    required this.updateAvailable,
    required this.healthNeedsAttention,
    required this.onTap,
  });

  final bool largeText;
  final bool connected;
  final bool updateAvailable;
  final bool healthNeedsAttention;
  final VoidCallback onTap;

  static TextStyle connectedStyle(BuildContext context) => Theme.of(
    context,
  ).textTheme.labelLarge!.copyWith(fontWeight: FontWeight.w700);

  static TextStyle updateStyle(BuildContext context) => Theme.of(
    context,
  ).textTheme.labelSmall!.copyWith(fontSize: 10, letterSpacing: 0.15);

  @override
  State<_BoardStatusAction> createState() => _BoardStatusActionState();
}

class _BoardStatusActionState extends State<_BoardStatusAction> {
  // InkWell focus/hover highlights use this same default duration. Keeping
  // the text transition on the frame ticker avoids a wall-clock handoff.
  static const Duration _attentionForegroundTransitionDuration = Duration(
    milliseconds: 50,
  );

  final FocusNode _focusNode = FocusNode(
    debugLabel: 'Board connection options',
  );
  bool _pressed = false;
  bool _focused = false;
  bool _hovered = false;

  @override
  void dispose() {
    _focusNode.dispose();
    super.dispose();
  }

  void _setPressed(bool pressed) {
    if (_pressed == pressed) {
      return;
    }
    setState(() => _pressed = pressed);
  }

  void _setAttention({bool? focused, bool? hovered}) {
    final bool nextFocused = focused ?? _focused;
    final bool nextHovered = hovered ?? _hovered;
    if (_focused == nextFocused && _hovered == nextHovered) {
      return;
    }
    setState(() {
      _focused = nextFocused;
      _hovered = nextHovered;
    });
  }

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    final Color statusColor = !widget.connected
        ? colors.onSurfaceVariant
        : widget.healthNeedsAttention || widget.updateAvailable
        ? colors.secondary
        : colors.tertiary;
    // Retain the status hue while keeping text readable over InkWell's
    // keyboard-focus and hover fills, including the scrolled app-bar tint.
    final Color color = _focused || _hovered
        ? Color.lerp(statusColor, colors.onSurface, 0.30)!
        : statusColor;
    return Semantics(
      key: ValueKey<String>(
        widget.connected
            ? 'connected-board-status-live-region'
            : 'disconnected-board-status-live-region',
      ),
      button: true,
      container: true,
      liveRegion: true,
      excludeSemantics: true,
      label: !widget.connected
          ? litchStrings(context).disconnectedChoose
          : widget.healthNeedsAttention
          ? widget.updateAvailable
                ? litchStrings(context).connectedHealthUpdateBoardOptions
                : litchStrings(context).connectedHealthBoardOptions
          : widget.updateAvailable
          ? litchStrings(context).connectedUpdateBoardOptions
          : litchStrings(context).connectedBoardOptions,
      onTap: widget.onTap,
      child: InkWell(
        key: ValueKey<String>(
          widget.connected
              ? 'connected-board-status-action'
              : 'disconnected-board-status-action',
        ),
        focusNode: _focusNode,
        borderRadius: BorderRadius.circular(12),
        // The label fade owns touch feedback; keep InkWell's keyboard focus.
        splashFactory: NoSplash.splashFactory,
        highlightColor: Colors.transparent,
        onTap: widget.onTap,
        onHighlightChanged: _setPressed,
        onFocusChange: (bool focused) => _setAttention(focused: focused),
        onHover: (bool hovered) => _setAttention(hovered: hovered),
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 48),
          child: AnimatedOpacity(
            key: const ValueKey<String>('connected-board-status-feedback'),
            duration: const Duration(milliseconds: 80),
            curve: Curves.easeOut,
            opacity: _pressed ? 0.95 : 1,
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.end,
              children: <Widget>[
                _BoardStatusText(connected: widget.connected, color: color),
                if (widget.healthNeedsAttention || widget.updateAvailable)
                  AnimatedDefaultTextStyle(
                    duration: _attentionForegroundTransitionDuration,
                    curve: Curves.linear,
                    style: _BoardStatusAction.updateStyle(
                      context,
                    ).copyWith(color: color),
                    child: _oneLineBoardStatus(
                      context,
                      widget.healthNeedsAttention
                          ? litchStrings(context).needsAttention
                          : litchStrings(context).updateAvailable,
                      _BoardStatusAction.updateStyle(context),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

// Artwork owns its full designed size; connection text fits the space beside it.
bool _compactBrandSpacing(double width) =>
    width < LitchAboutBrandButton.widthForHeight(23) + 28 + 16 + 48;

double _mainStatusWidth(BuildContext context, {double? availableWidth}) {
  final width = availableWidth ?? MediaQuery.sizeOf(context).width;
  final compact = _compactBrandSpacing(width);
  return math.max(
    0.0,
    width -
        LitchAboutBrandButton.widthForHeight(23) -
        (compact ? 16 + 4 : 28 + 16),
  );
}

double _mainToolbarHeight(BuildContext context) => kToolbarHeight;

double _boardStatusTextHeight(
  BuildContext context,
  String label, {
  bool hasChevron = false,
}) => _toolbarTextHeight(
  context,
  label,
  _BoardStatusAction.connectedStyle(context),
  _mainStatusWidth(context) - (hasChevron ? _BoardStatusText.trailingWidth : 0),
  maxLines: _BoardStatusText.maxLines,
);

// Brand and connection state share one portrait row at every text preference.
AppBar _mainBrandAppBar(
  BuildContext context, {
  required ValueChanged<int> onLoadVersionArtwork,
  DateTime Function()? versionTapClock,
  required Widget status,
  bool litchcraft = false,
  bool connecting = false,
  double? statusHeight,
  ScrollNotificationPredicate notificationPredicate =
      defaultScrollNotificationPredicate,
}) {
  final brandWidth = LitchAboutBrandButton.widthForHeight(23);
  final statusExtent = math.max(
    48.0,
    statusHeight ??
        _boardStatusTextHeight(
          context,
          connecting
              ? litchStrings(context).boardConnecting
              : litchStrings(context).boardDisconnected,
        ),
  );
  return AppBar(
    automaticallyImplyLeading: false,
    centerTitle: false,
    // The brand and board status each own a distinct accessible target.
    excludeHeaderSemantics: true,
    notificationPredicate: notificationPredicate,
    titleSpacing: 0,
    toolbarHeight: math.max(kToolbarHeight, statusExtent),
    title: LayoutBuilder(
      builder: (context, constraints) {
        // Safe insets and embedded viewports can make the title narrower
        // than MediaQuery. Reserve the artwork in the actual title width.
        final compact = _compactBrandSpacing(constraints.maxWidth);
        final statusWidth = _mainStatusWidth(
          context,
          availableWidth: constraints.maxWidth,
        );
        return Padding(
          padding: EdgeInsetsDirectional.only(
            start: compact ? 8 : 16,
            end: compact ? 8 : 12,
          ),
          child: Row(
            children: [
              SizedBox(
                width: brandWidth,
                height: 48,
                child: LitchAboutBrandButton(
                  onLoadVersionArtwork: onLoadVersionArtwork,
                  versionTapClock: versionTapClock,
                  litchcraft: litchcraft,
                  fitToWidth: false,
                ),
              ),
              SizedBox(width: compact ? 4 : 16),
              SizedBox(width: statusWidth, height: statusExtent, child: status),
            ],
          ),
        );
      },
    ),
  );
}

Widget _oneLineBoardStatus(
  BuildContext context,
  String label,
  TextStyle style,
) => LayoutBuilder(
  builder: (context, constraints) {
    final painter = TextPainter(
      text: TextSpan(text: label, style: style),
      textDirection: Directionality.of(context),
      locale: Localizations.localeOf(context),
    )..layout();
    final naturalWidth = painter.width;
    painter.dispose();
    final preferredScale =
        LitchCompactControls.textScalerOf(context).scale(style.fontSize!) /
        style.fontSize!;
    final scale = math.min(
      preferredScale,
      math.max(0.0, constraints.maxWidth - .5) / math.max(1.0, naturalWidth),
    );
    return Text(
      label,
      textScaler: TextScaler.linear(scale),
      maxLines: 1,
      softWrap: false,
      overflow: TextOverflow.clip,
      textAlign: TextAlign.right,
    );
  },
);

class _BoardStatusText extends StatelessWidget {
  const _BoardStatusText({
    required this.connected,
    required this.color,
    this.connecting = false,
  });

  static const int maxLines = 1;
  static const double trailingWidth = 18;

  final bool connected;
  final Color color;
  final bool connecting;

  @override
  Widget build(BuildContext context) {
    final style = _BoardStatusAction.connectedStyle(
      context,
    ).copyWith(fontSize: connected || connecting ? null : 16, color: color);
    return Row(
      mainAxisAlignment: MainAxisAlignment.end,
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Flexible(
          child: AnimatedDefaultTextStyle(
            duration:
                _BoardStatusActionState._attentionForegroundTransitionDuration,
            curve: Curves.linear,
            style: style,
            child: _oneLineBoardStatus(
              context,
              connecting
                  ? litchStrings(context).boardConnecting
                  : connected
                  ? litchStrings(context).boardConnected
                  : litchStrings(context).boardDisconnected,
              style,
            ),
          ),
        ),
        if (connected)
          Icon(
            Icons.keyboard_arrow_down_rounded,
            key: const ValueKey<String>('connected-board-status-chevron'),
            size: trailingWidth,
            color: color,
          ),
      ],
    );
  }
}

class _BoardStatusLabel extends StatelessWidget {
  const _BoardStatusLabel({required this.connecting, this.onChooseBoard});

  final bool connecting;
  final VoidCallback? onChooseBoard;

  @override
  Widget build(BuildContext context) {
    if (!connecting && onChooseBoard != null) {
      return KeyedSubtree(
        key: const ValueKey('offline-board-status-action'),
        child: _BoardStatusAction(
          connected: false,
          largeText: MediaQuery.textScalerOf(context).scale(14) >= 21,
          updateAvailable: false,
          healthNeedsAttention: false,
          onTap: onChooseBoard!,
        ),
      );
    }
    final colors = Theme.of(context).colorScheme;
    return Semantics(
      label: connecting
          ? litchStrings(context).boardConnecting
          : litchStrings(context).boardDisconnected,
      excludeSemantics: true,
      child: ConstrainedBox(
        constraints: BoxConstraints(
          minHeight: 48,
          maxWidth: MediaQuery.sizeOf(context).width - 84,
        ),
        child: Align(
          alignment: AlignmentDirectional.centerEnd,
          widthFactor: 1,
          child: _BoardStatusText(
            connected: false,
            connecting: connecting,
            color: connecting ? colors.primary : colors.onSurfaceVariant,
          ),
        ),
      ),
    );
  }
}
