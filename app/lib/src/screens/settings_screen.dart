import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/board_health_status.dart';
import '../models/board_storage_recovery.dart';
import '../models/home_board_snapshot.dart';
import '../models/home_state_notification.dart';
import '../models/litch_clock_preset.dart';
import '../models/litch_control_action.dart';
import '../models/litch_device_capabilities.dart';
import '../models/litch_end_palette.dart';
import '../models/litch_piece_color.dart';
import '../models/litch_setting.dart';
import '../models/runtime_health_snapshot.dart';
import '../models/support_snapshot.dart';
import '../models/settings_state_wire_codec.dart';
import '../services/litch_ble_service.dart';
import '../services/owner_pairing_session.dart';
import '../theme/litch_appearance.dart';
import '../widgets/app_preferences.dart';
import '../widgets/color_slider.dart';
import '../widgets/board_controls_glyph.dart';
import '../widgets/home_chessboard.dart';
import '../widgets/litchcraft_tools.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/reading_pane.dart';
import '../widgets/home_notice_presenter.dart';
import '../widgets/runtime_health_card.dart';
import '../widgets/settings_ui.dart';
import '../widgets/compact_controls.dart';
import 'manual_screen.dart';
import 'support_report_screen.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({
    super.key,
    required this.bleService,
    required this.boardId,
    this.notices,
    required this.enableAppConsole,
    required this.onEnableAppConsoleChanged,
    this.appearanceMode = LitchAppearanceMode.system,
    this.onAppearanceModeChanged,
    this.healthRefreshEpoch = 0,
    this.healthFocusEpoch = 0,
    this.overviewRequestEpoch = 0,
    this.onOpenFirmware,
    this.deferInitialLoad = false,
    this.recoveryOnly = false,
    this.landscapeOverlayClearance = 0,
  });

  final LitchBleService bleService;
  final String boardId;
  final HomeNoticePresenter? notices;
  final bool enableAppConsole;
  final Future<void> Function(bool enabled) onEnableAppConsoleChanged;
  final LitchAppearanceMode appearanceMode;
  final Future<void> Function(LitchAppearanceMode mode)?
  onAppearanceModeChanged;
  final int healthRefreshEpoch;
  final int healthFocusEpoch;
  final int overviewRequestEpoch;
  final VoidCallback? onOpenFirmware;
  final bool deferInitialLoad;
  final bool recoveryOnly;
  final double landscapeOverlayClearance;

  @override
  State<SettingsScreen> createState() => SettingsScreenState();
}

enum _SetupPage { overview, lights, behavior, phone, advanced, developer }

class SettingsScreenState extends State<SettingsScreen> {
  final HomeNoticePresenter _localNotices = HomeNoticePresenter();
  HomeNoticePresenter get _notices => widget.notices ?? _localNotices;

  static const Duration _sliderWriteInterval = Duration(milliseconds: 30);
  static const String _p1PieceWheelKey = 'P1_PIECE_WHEEL';
  static const String _p2PieceWheelKey = 'P2_PIECE_WHEEL';
  static const String _p1EndPieceWheelKey = 'P1_END_PIECE_WHEEL';
  static const String _p2EndPieceWheelKey = 'P2_END_PIECE_WHEEL';
  static const String _takebackGraceKey = 'TAKEBACK_GRACE_SECONDS';
  static const String _boardSetupAssistanceKey = 'BOARD_SETUP_ASSISTANCE';
  static const String _timerKey = 'TIMER';
  static const String _settleStyleKey = 'SETTLE_ANIM_STYLE';
  static const String _lightSquareModeKey = 'LIGHT_SQUARE_MODE';
  static const String _lightSquareHueKey = 'LIGHT_SQUARE_HUE';
  static const String _lightSquareBrightnessKey = 'LIGHT_SQUARE_BRIGHTNESS';
  static const Map<int, String> _timerPresetLabels = LitchClockPresets.labels;
  static final _wheelPreviewGradient = LinearGradient(
    colors: [
      for (var wheel = 0; wheel <= 255; wheel++) litchWheelPreviewColor(wheel),
    ],
  );
  static const double _wideLayoutBreakpoint = 760;
  static const double _landscapeOverviewBreakpoint = 700;
  static const double _maxContentWidth = 960;
  static const Set<String> _quickSettingKeys = <String>{
    'VS_CPU_LEVEL',
    'VS_CPU_PLAYER',
    'GAME_FIRST_PLAYER',
    _p1PieceWheelKey,
    _p2PieceWheelKey,
    'WHITE_SQUARES_ON',
    _lightSquareModeKey,
    _lightSquareHueKey,
    _lightSquareBrightnessKey,
    _settleStyleKey,
    'BOARD_ROTATE',
  };
  static const Set<String> _lightSquareAppearanceKeys = <String>{
    _lightSquareModeKey,
    _lightSquareHueKey,
    _lightSquareBrightnessKey,
  };
  static const List<String> _hiddenSettingKeyPrefixes = <String>[
    'RGBW_COOL',
    'USB_CC_',
  ];

  bool _isLoading = true;
  List<LitchSetting> _settings = const <LitchSetting>[];
  bool _bootDisplayUnavailable = false;
  final Set<String> _writingKeys = <String>{};
  final Map<String, Object> _queuedSettingValues = <String, Object>{};
  final Map<String, double> _sliderDraftValues = <String, double>{};
  final Map<String, int> _sliderPendingValues = <String, int>{};
  final Map<String, bool> _sliderWriteInFlight = <String, bool>{};
  final Map<String, DateTime> _sliderLastWriteAt = <String, DateTime>{};
  final Map<String, Timer> _sliderThrottleTimers = <String, Timer>{};
  final Set<String> _sliderPendingAckKeys = <String>{};
  final Map<String, int> _editorRevisionByKey = <String, int>{};
  final TextEditingController _searchController = TextEditingController();
  final FocusNode _searchFocusNode = FocusNode(debugLabel: 'Settings search');
  final GlobalKey _searchFieldKey = GlobalKey();
  final ScrollController _searchResultsScrollController = ScrollController();
  final GlobalKey _boardHealthSectionKey = GlobalKey();
  final FocusNode _boardHealthFocusNode = FocusNode(
    debugLabel: 'Board health section',
  );
  GlobalKey<NavigatorState> _setupNavigatorKey = GlobalKey<NavigatorState>();
  PageStorageBucket _pageStorageBucket = PageStorageBucket();
  final Map<_SetupPage, ScrollController> _pageScrollControllers =
      <_SetupPage, ScrollController>{};
  final List<_SetupPage> _pageTrail = <_SetupPage>[_SetupPage.overview];
  _SetupPage get _page => _pageTrail.last;
  static const Set<String> _behaviorKeys = <String>{
    'MOVE_CONFIRM',
    'PROMOTION_MODE',
    _boardSetupAssistanceKey,
    _takebackGraceKey,
  };
  StreamSubscription<String>? _settingsChangedSub;
  StreamSubscription<HomeStateNotification>? _homeStateSub;
  StreamSubscription<BoardHealthStatus>? _boardHealthSub;
  BoardHealthStatus? _boardHealth;
  bool _healthIsPrioritized = false;
  double? _heldBoardHealthHeight;
  StreamSubscription<ConnectionStateUpdate>? _healthConnectionSub;
  bool _writingAppConsoleSetting = false;
  bool? _appConsoleDraft;
  bool _homeClockProjectionObserved = false;
  bool _usesDirectPhysicalPaletteKeys = false;
  bool _orientationCorrectionActive = false;
  SupportPublicHealth? _recoverySupport;
  int? _recoverySupportGeneration;
  bool get _recoveryOnly =>
      widget.recoveryOnly ||
      widget.bleService
          .deviceCapabilities(_connectedBoardId ?? widget.boardId)
          .recoveryOnly;
  RuntimeHealthResult? _runtimeHealth;
  bool _refreshingRuntimeHealth = false;
  bool _supportsRuntimeHealthRecheck = false;
  bool _recheckingRuntimeHealth = false;
  String Function()? _runtimeHealthRecheckMessage;
  BoardStorageRecoverySnapshot? _storageRecovery;
  BoardStorageRecoverySnapshot? _visibleStorageRecovery;
  StreamSubscription<BoardStorageRecoverySnapshot>? _storageRecoverySub;
  bool _checkingStorageRecovery = false;
  bool _reformattingStorage = false;
  bool _storageReformatAttempted = false;
  DialogRoute<bool>? _storageConfirmationRoute;
  String _searchQuery = '';
  int _viewGeneration = 0;
  int _connectionEpoch = 0;
  String? _placementEffectDraft;
  int _settingsLoadGeneration = 0;
  bool _settingsLoadStarted = false;

  @override
  void initState() {
    super.initState();
    _bindServiceStreams();
    _onHomeState(widget.bleService.latestHomeState);
    if (!widget.deferInitialLoad) {
      _loadSettings();
      unawaited(_refreshRuntimeHealth());
    }
    _scheduleBoardHealthFocus();
  }

  void _bindServiceStreams() {
    final int generation = _viewGeneration;
    String connectionDeviceId = _connectedBoardId ?? widget.boardId;
    final bool healthConnected = _connectedBoardId != null;
    _boardHealth = healthConnected ? widget.bleService.latestBoardHealth : null;
    if (!healthConnected) {
      _runtimeHealth = const RuntimeHealthUnavailable(
        RuntimeHealthUnavailableReason.disconnected,
      );
    }
    _storageRecovery = healthConnected
        ? widget.bleService.latestBoardStorageRecovery
        : null;
    _storageRecoverySub?.cancel();
    _storageRecoverySub = widget.bleService.boardStorageRecoveryStream.listen((
      BoardStorageRecoverySnapshot snapshot,
    ) {
      if (mounted &&
          generation == _viewGeneration &&
          _connectedBoardId != null) {
        setState(() => _storageRecovery = snapshot);
      }
    });
    _boardHealthSub?.cancel();
    _boardHealthSub = widget.bleService.boardHealthStream.listen((
      BoardHealthStatus status,
    ) {
      if (mounted &&
          generation == _viewGeneration &&
          _connectedBoardId != null) {
        setState(() {
          _boardHealth = status;
          _runtimeHealth = RuntimeHealthAvailable(status.runtimeHealth);
        });
        if (_hasSavingIncident && _storageRecovery == null) {
          unawaited(_refreshStorageRecovery());
        }
      }
    });

    _healthConnectionSub?.cancel();
    _healthConnectionSub = widget.bleService.connectionStream.listen((
      ConnectionStateUpdate update,
    ) {
      if (!mounted ||
          generation != _viewGeneration ||
          (update.deviceId.isNotEmpty &&
              update.deviceId != connectionDeviceId &&
              update.deviceId != _connectedBoardId)) {
        return;
      }
      if (_connectedBoardId case final deviceId?) {
        connectionDeviceId = deviceId;
      }
      ++_settingsLoadGeneration;
      ++_connectionEpoch;
      _cancelStorageConfirmation();
      _clearSliderOwnership();
      setState(() {
        _isLoading = false;
        _retireBootDisplayControl();
        _writingKeys.clear();
        _queuedSettingValues.clear();
        _placementEffectDraft = null;
        _boardHealth = _connectedBoardId != null
            ? widget.bleService.latestBoardHealth
            : null;
        _runtimeHealth = _connectedBoardId != null
            ? null
            : const RuntimeHealthUnavailable(
                RuntimeHealthUnavailableReason.disconnected,
              );
        _recoverySupport = null;
        _recoverySupportGeneration = null;
        _refreshingRuntimeHealth = false;
        _supportsRuntimeHealthRecheck = false;
        _recheckingRuntimeHealth = false;
        _runtimeHealthRecheckMessage = null;
        _checkingStorageRecovery = false;
        _reformattingStorage = false;
        _storageRecovery = _storageReformatAttempted
            ? BoardStorageRecoverySnapshot.unknown
            : null;
      });
      if (_settingsLoadStarted &&
          update.connectionState == DeviceConnectionState.connected) {
        _loadSettings();
        unawaited(_refreshRuntimeHealth());
      }
    });
    _settingsChangedSub = widget.bleService.settingsChangedStream.listen((
      String key,
    ) {
      if (generation == _viewGeneration) {
        _onExternalSettingChanged(key);
      }
    });
    _homeStateSub = widget.bleService.homeStateStream.listen((
      HomeStateNotification state,
    ) {
      if (generation == _viewGeneration) {
        _onHomeState(state);
      }
    });
  }

  void _onHomeState(HomeStateNotification? latest) {
    if (!mounted) {
      return;
    }
    final bool nextClockObserved =
        _homeClockProjectionObserved || latest?.clockStatus != null;
    final bool nextOrientationCorrection = latest == null
        ? false
        : HomeBoardSnapshot(
            position: latest.position,
            gameStatus: latest.gameStatus,
            physicalStatus: latest.physicalStatus,
            homeStateAction: latest.action,
            freshSetup: latest.freshSetup,
            kingQueenSwapPromptActive: latest.kingQueenSwapPromptActive,
          ).isGuidedKingQueenSwap;
    if (nextClockObserved == _homeClockProjectionObserved &&
        nextOrientationCorrection == _orientationCorrectionActive) {
      return;
    }
    setState(() {
      _homeClockProjectionObserved = nextClockObserved;
      _orientationCorrectionActive = nextOrientationCorrection;
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    // The shell retains this widget while its page is hidden.
    if (!TickerMode.valuesOf(context).enabled) {
      _searchFocusNode.unfocus();
    }
  }

  @override
  void didUpdateWidget(covariant SettingsScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    final bool contextChanged =
        oldWidget.bleService != widget.bleService ||
        oldWidget.boardId != widget.boardId ||
        oldWidget.recoveryOnly != widget.recoveryOnly;
    if (contextChanged) {
      _healthIsPrioritized = false;
      ++_viewGeneration;
      ++_connectionEpoch;
      ++_settingsLoadGeneration;
      _settingsChangedSub?.cancel();
      _homeStateSub?.cancel();
      _boardHealthSub?.cancel();
      _healthConnectionSub?.cancel();
      _homeClockProjectionObserved = false;
      _retireBootDisplayControl();
      _clearSliderOwnership();
      _editorRevisionByKey.clear();
      _writingKeys.clear();
      _queuedSettingValues.clear();
      _placementEffectDraft = null;
      _usesDirectPhysicalPaletteKeys = false;
      _orientationCorrectionActive = false;
      _resetSettingsSearch();
      _setupNavigatorKey = GlobalKey<NavigatorState>();
      _pageStorageBucket = PageStorageBucket();
      final previousScrollControllers = _pageScrollControllers.values.toList();
      _pageScrollControllers.clear();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        for (final controller in previousScrollControllers) {
          controller.dispose();
        }
      });
      _pageTrail.clear();
      _pageTrail.add(_SetupPage.overview);
      _runtimeHealth = null;
      _recoverySupport = null;
      _recoverySupportGeneration = null;
      _refreshingRuntimeHealth = false;
      _supportsRuntimeHealthRecheck = false;
      _recheckingRuntimeHealth = false;
      _runtimeHealthRecheckMessage = null;
      _cancelStorageConfirmation();
      _storageRecovery = null;
      _visibleStorageRecovery = null;
      _checkingStorageRecovery = false;
      _reformattingStorage = false;
      _storageReformatAttempted = false;
      _bindServiceStreams();
      _onHomeState(widget.bleService.latestHomeState);
      _settingsLoadStarted = false;
      if (!widget.deferInitialLoad) {
        _loadSettings();
        unawaited(_refreshRuntimeHealth());
      }
      return;
    }
    if (oldWidget.overviewRequestEpoch != widget.overviewRequestEpoch) {
      _openPage(_SetupPage.overview);
    }
    if (!widget.deferInitialLoad && !_settingsLoadStarted) {
      _loadSettings();
      unawaited(_refreshRuntimeHealth());
      if (oldWidget.healthFocusEpoch != widget.healthFocusEpoch) {
        _scheduleBoardHealthFocus();
      }
      return;
    }
    if (oldWidget.healthRefreshEpoch != widget.healthRefreshEpoch) {
      unawaited(_refreshRuntimeHealth());
    }
    if (oldWidget.healthFocusEpoch != widget.healthFocusEpoch) {
      _scheduleBoardHealthFocus();
    }
  }

  void _scheduleBoardHealthFocus({bool afterRecheck = false}) {
    if (widget.healthFocusEpoch == 0 && !afterRecheck) {
      return;
    }
    if (_page != _SetupPage.overview) {
      _openPage(_SetupPage.overview);
    } else if (_searchQuery.isNotEmpty) {
      setState(_resetSettingsSearch);
    }
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      if (!mounted) {
        return;
      }
      final BuildContext? targetContext = _boardHealthSectionKey.currentContext;
      if (targetContext == null) {
        return;
      }
      await Scrollable.ensureVisible(
        targetContext,
        alignment: 0.05,
        duration: const Duration(milliseconds: 300),
        curve: Curves.easeOutCubic,
      );
      if (mounted) {
        _boardHealthFocusNode.requestFocus();
      }
    });
  }

  Future<void> _refreshRuntimeHealth() async {
    final String? targetId = _connectedBoardId;
    if (targetId == null) {
      setState(() {
        _boardHealth = null;
        _runtimeHealth = const RuntimeHealthUnavailable(
          RuntimeHealthUnavailableReason.disconnected,
        );
        _refreshingRuntimeHealth = false;
        _supportsRuntimeHealthRecheck = false;
      });
      return;
    }
    if (_refreshingRuntimeHealth) {
      return;
    }
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final int sessionToken = service.connectionSessionToken;
    setState(() {
      _refreshingRuntimeHealth = true;
    });
    SupportPublicHealth? recoveryHealth;
    final RuntimeHealthResult result;
    if (_recoveryOnly) {
      if (_recoverySupportGeneration == sessionToken) {
        setState(() => _refreshingRuntimeHealth = false);
        return;
      }
      _recoverySupportGeneration = sessionToken;
      try {
        recoveryHealth = (await service.collectSupportBoardDiagnostics(
          deviceId: targetId,
          connectionSessionToken: sessionToken,
        )).publicHealth.value;
      } catch (_) {
        // Keep Setup and the editable report available with partial evidence.
      }
      result =
          recoveryHealth?.runtimeHealth ??
          const RuntimeHealthUnavailable(
            RuntimeHealthUnavailableReason.sendFailed,
          );
    } else {
      result = await service.fetchRuntimeHealth(deviceId: targetId);
    }
    final LitchDeviceCapabilities capabilities = service.deviceCapabilities(
      targetId,
    );
    if (!mounted ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId ||
        targetId != _connectedBoardId ||
        sessionToken != service.connectionSessionToken) {
      return;
    }
    setState(() {
      _runtimeHealth = result;
      _recoverySupport = recoveryHealth;
      _supportsRuntimeHealthRecheck =
          !capabilities.recoveryOnly &&
          capabilities.supportsRuntimeHealthRecheck;
      _refreshingRuntimeHealth = false;
    });
    if (_hasSavingIncident || _storageReformatAttempted) {
      unawaited(_refreshStorageRecovery());
    }
  }

  bool get _hasSavingIncident =>
      !_recoveryOnly &&
      _connectedBoardId != null &&
      _currentPersistence == HomePersistenceDegradation.storageIo;

  Future<void> _refreshStorageRecovery() async {
    final targetId = _connectedBoardId;
    if (_checkingStorageRecovery || targetId == null) return;
    final service = widget.bleService;
    final boardId = widget.boardId;
    final generation = _viewGeneration;
    final connectionEpoch = _connectionEpoch;
    final sessionToken = service.connectionSessionToken;
    setState(() => _checkingStorageRecovery = true);
    final snapshot = await service.fetchBoardStorageRecovery(
      deviceId: targetId,
    );
    if (!mounted ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId ||
        targetId != _connectedBoardId ||
        sessionToken != service.connectionSessionToken) {
      return;
    }
    setState(() {
      _checkingStorageRecovery = false;
      if (snapshot.provesSupport ||
          _storageReformatAttempted ||
          snapshot.status == BoardStorageRecoveryStatus.unavailable) {
        _storageRecovery = snapshot;
      }
    });
  }

  void _cancelStorageConfirmation() {
    final route = _storageConfirmationRoute;
    _storageConfirmationRoute = null;
    if (route != null) {
      scheduleMicrotask(() {
        if (route.isActive && route.navigator?.mounted == true) {
          route.navigator?.removeRoute(route);
        }
      });
    }
  }

  Future<void> _confirmStorageReformat() async {
    final targetId = _connectedBoardId;
    if (targetId == null ||
        _storageConfirmationRoute != null ||
        _reformattingStorage ||
        !(_storageRecovery?.canReformat ?? false)) {
      return;
    }
    final service = widget.bleService;
    final boardId = widget.boardId;
    final generation = _viewGeneration;
    final connectionEpoch = _connectionEpoch;
    final sessionToken = service.connectionSessionToken;
    bool ownsConnection() =>
        mounted &&
        generation == _viewGeneration &&
        connectionEpoch == _connectionEpoch &&
        service == widget.bleService &&
        boardId == widget.boardId &&
        sessionToken == service.connectionSessionToken &&
        targetId == _connectedBoardId;
    if (!ownsConnection()) return;
    final route = DialogRoute<bool>(
      context: context,
      builder: (dialogContext) => AlertDialog(
        key: const ValueKey<String>('storage-reformat-confirmation'),
        scrollable: true,
        title: Text(litchStrings(dialogContext).storageReformatTitle),
        content: Text(litchStrings(dialogContext).storageReformatWarning),
        actions: <Widget>[
          TextButton(
            key: const ValueKey<String>('storage-reformat-cancel'),
            style: TextButton.styleFrom(minimumSize: const Size(48, 48)),
            onPressed: () => Navigator.of(dialogContext).pop(false),
            child: Text(litchStrings(dialogContext).cancelAction),
          ),
          FilledButton(
            key: const ValueKey<String>('storage-reformat-confirm'),
            style: FilledButton.styleFrom(minimumSize: const Size(48, 48)),
            onPressed: () => Navigator.of(dialogContext).pop(true),
            child: Text(litchStrings(dialogContext).storageReformatConfirm),
          ),
        ],
      ),
    );
    setState(() => _storageConfirmationRoute = route);
    final confirmed = await Navigator.of(context).push<bool>(route);
    if (identical(_storageConfirmationRoute, route)) {
      _storageConfirmationRoute = null;
    }
    if (!ownsConnection()) return;
    if (confirmed != true) {
      setState(() {});
      return;
    }
    setState(() {
      _storageReformatAttempted = true;
      _reformattingStorage = true;
      _storageRecovery = const BoardStorageRecoverySnapshot(
        status: BoardStorageRecoveryStatus.pending,
      );
    });
    final result = await service.reformatBoardStorage(deviceId: targetId);
    if (!ownsConnection()) return;
    setState(() {
      _reformattingStorage = false;
      // The typed owner resolves terminal truth against this submission.
      _storageRecovery = result;
    });
    _scheduleBoardHealthFocus(afterRecheck: true);
  }

  Future<void> _recheckRuntimeHealth() async {
    final String? targetId = _connectedBoardId;
    if (_recheckingRuntimeHealth || targetId == null) {
      return;
    }
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final int sessionToken = service.connectionSessionToken;
    setState(() {
      _recheckingRuntimeHealth = true;
      _runtimeHealthRecheckMessage = null;
    });
    final RuntimeHealthRecheckResult result = await service
        .recheckRuntimeHealth(deviceId: targetId);
    if (!mounted ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId ||
        targetId != _connectedBoardId ||
        sessionToken != service.connectionSessionToken) {
      return;
    }
    setState(() {
      _runtimeHealth = result.health;
      _recheckingRuntimeHealth = false;
      _runtimeHealthRecheckMessage = () => switch (result.outcome) {
        RuntimeHealthRecheckOutcome.succeeded => litchStrings(
          context,
        ).healthPowerAccepted,
        RuntimeHealthRecheckOutcome.validationFailed => litchStrings(
          context,
        ).healthPowerValidationFailed,
        RuntimeHealthRecheckOutcome.unsupported => litchStrings(
          context,
        ).healthRecheckUnsupported,
        RuntimeHealthRecheckOutcome.timedOut => litchStrings(
          context,
        ).healthRecheckTimeout,
        RuntimeHealthRecheckOutcome.disconnected => litchStrings(
          context,
        ).healthRecheckDisconnected,
        RuntimeHealthRecheckOutcome.sendFailed => litchStrings(
          context,
        ).healthRecheckSendFailed,
        RuntimeHealthRecheckOutcome.malformed => litchStrings(
          context,
        ).healthRecheckMalformed,
      };
    });
    _scheduleBoardHealthFocus(afterRecheck: true);
  }

  void _openSupportReport() {
    unawaited(
      SupportReportScreen.open(
        context,
        bleService: widget.bleService,
        boardId: widget.boardId,
      ),
    );
  }

  @override
  void dispose() {
    _cancelStorageConfirmation();
    _storageRecoverySub?.cancel();
    ++_viewGeneration;
    ++_connectionEpoch;
    ++_settingsLoadGeneration;
    for (final Timer timer in _sliderThrottleTimers.values) {
      timer.cancel();
    }
    _sliderThrottleTimers.clear();
    for (final controller in _pageScrollControllers.values) {
      controller.dispose();
    }
    _searchController.dispose();
    _searchFocusNode.dispose();
    _searchResultsScrollController.dispose();
    _boardHealthFocusNode.dispose();
    _settingsChangedSub?.cancel();
    _homeStateSub?.cancel();
    _boardHealthSub?.cancel();
    _healthConnectionSub?.cancel();
    super.dispose();
  }

  List<LitchSetting> _retainBootDisplayControl(List<LitchSetting> incoming) {
    // A completed metadata read may carry the retained descriptor while the
    // newer binary revision is still waiting for State44. Cache authority,
    // rather than descriptor presence, decides whether the editor can act.
    final available =
        !widget.bleService.hasBoardStateSnapshot ||
        widget.bleService.latestSettingValue('BOOT_DISPLAY') != null;
    if (available && incoming.any((setting) => setting.key == 'BOOT_DISPLAY')) {
      _bootDisplayUnavailable = false;
      return incoming;
    }
    final previous = _settingByKey('BOOT_DISPLAY');
    if (previous == null) {
      return incoming
          .where((setting) => setting.key != 'BOOT_DISPLAY')
          .toList();
    }
    _bootDisplayUnavailable = true;
    return <LitchSetting>[
      ...incoming.where((setting) => setting.key != 'BOOT_DISPLAY'),
      previous,
    ];
  }

  void _retireBootDisplayControl() {
    _bootDisplayUnavailable = false;
    _settings = _settings
        .where((setting) => setting.key != 'BOOT_DISPLAY')
        .toList();
  }

  void _onExternalSettingChanged(String key) {
    if (!_settingsLoadStarted) return;
    if (!mounted || key.isEmpty) {
      return;
    }
    if (SettingsStateWireCodec.extensionKeys.contains(key)) {
      final current = widget.bleService.immediateSettingsSnapshot(
        widget.boardId,
      );
      setState(() {
        _settings = _retainBootDisplayControl(<LitchSetting>[
          ..._settings.where(
            (setting) =>
                !SettingsStateWireCodec.extensionKeys.contains(setting.key),
          ),
          for (final setting in current)
            if (SettingsStateWireCodec.extensionKeys.contains(setting.key))
              setting,
        ]);
      });
      return;
    }

    if (key == _p1PieceWheelKey ||
        key == _p2PieceWheelKey ||
        key == _p1EndPieceWheelKey ||
        key == _p2EndPieceWheelKey) {
      if (!_pieceColorWriteInFlight) {
        unawaited(_reloadPhysicalEndPalette());
      }
      return;
    }

    if (_writingKeys.contains(key) || _hasLocalSliderOwnership(key)) {
      return;
    }

    bool appliedFromCache = false;
    final int currentIndex = _findSettingIndex(key);
    if (currentIndex >= 0) {
      final LitchSetting current = _settings[currentIndex];
      final Object? cached = widget.bleService.latestSettingValue(key);
      if (cached != null) {
        final Object value = _coerceIncomingSettingValue(current, cached);
        if (current.value != value) {
          setState(() {
            _settings = List<LitchSetting>.from(_settings)
              ..[currentIndex] = _copyWithValue(current, value);
            _sliderDraftValues.remove(key);
          });
        }
        appliedFromCache = current.value != value;
      }
    }

    if (appliedFromCache) {
      if (key == 'GAME_FIRST_PLAYER') {
        unawaited(_reloadPhysicalEndPalette());
      }
      return;
    }

    unawaited(_reloadSingleSettingFromBoard(key));
    if (key == 'GAME_FIRST_PLAYER') {
      unawaited(_reloadPhysicalEndPalette());
    }
  }

  LitchEndPalette? _rebasePhysicalPalette(
    LitchBleService service,
    LitchEndPalette? fallback,
  ) {
    if (!service.hasBoardStateSnapshot) return fallback;
    final p1 = service.latestSettingValue(_p1EndPieceWheelKey);
    final p2 = service.latestSettingValue(_p2EndPieceWheelKey);
    return p1 is int && p2 is int
        ? LitchEndPalette(
            p1ControlsWheel: p1,
            p2ControlsWheel: p2,
            usesDirectPhysicalKeys: true,
          )
        : fallback;
  }

  Future<void> _reloadPhysicalEndPalette() async {
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final String targetId = service.connectedId ?? boardId;
    LitchEndPalette? palette;
    try {
      palette = await service.readPhysicalEndPalette(deviceId: targetId);
    } catch (_) {
      return;
    }
    if (!mounted ||
        palette == null ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId ||
        _pieceColorWriteInFlight) {
      return;
    }
    palette = _rebasePhysicalPalette(service, palette);
    setState(() {
      _usesDirectPhysicalPaletteKeys = palette!.usesDirectPhysicalKeys;
      _settings = _settingsWithPhysicalPalette(_settings, palette);
    });
  }

  Future<void> _reloadSingleSettingFromBoard(String key) async {
    if (!mounted) {
      return;
    }

    final int currentIndex = _findSettingIndex(key);
    if (currentIndex < 0) {
      return;
    }

    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final String targetId = service.connectedId ?? boardId;
    final Object? rawValue = await service.readSettingValue(
      deviceId: targetId,
      key: key,
    );
    if (!mounted ||
        rawValue == null ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId ||
        _writingKeys.contains(key) ||
        _hasLocalSliderOwnership(key)) {
      return;
    }

    final int freshIndex = _findSettingIndex(key);
    if (freshIndex < 0) {
      return;
    }
    final LitchSetting fresh = _settings[freshIndex];
    final Object value = _coerceIncomingSettingValue(fresh, rawValue);
    final LitchSetting incoming = _copyWithValue(fresh, value);
    setState(() {
      _settings = List<LitchSetting>.from(_settings)..[freshIndex] = incoming;
      _sliderDraftValues.remove(key);
    });
  }

  Object _coerceIncomingSettingValue(LitchSetting setting, Object rawValue) {
    final String type = setting.type.toLowerCase();
    if (type == 'bool') {
      if (rawValue is bool) {
        return rawValue;
      }
      if (rawValue is num) {
        return rawValue != 0;
      }
      final String token = rawValue.toString().trim().toLowerCase();
      return token == 'true' || token == '1' || token == 'on';
    }

    if (type == 'u8' || type == 'u32' || type == 'uint32' || type == 'int') {
      if (rawValue is int) {
        return rawValue;
      }
      if (rawValue is num) {
        return rawValue.round();
      }
      return int.tryParse(rawValue.toString()) ?? setting.value;
    }

    return rawValue.toString();
  }

  Future<void> _loadSettings() async {
    _settingsLoadStarted = true;
    if (_recoveryOnly) {
      _isLoading = false;
      return;
    }
    final int loadGeneration = ++_settingsLoadGeneration;
    final int viewGeneration = _viewGeneration;
    final Map<String, int> editorRevisions = Map<String, int>.of(
      _editorRevisionByKey,
    );
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final String? connectedDeviceId = _connectedBoardId;
    final String targetId = connectedDeviceId ?? boardId;
    final List<LitchSetting> immediate = service.immediateSettingsSnapshot(
      targetId,
    );
    final LitchEndPalette? immediatePalette = _rebasePhysicalPalette(
      service,
      null,
    );
    setState(() {
      if (immediate.isNotEmpty) {
        _settings = _retainBootDisplayControl(
          immediatePalette == null
              ? immediate
              : _settingsWithPhysicalPalette(immediate, immediatePalette),
        );
        _usesDirectPhysicalPaletteKeys =
            immediatePalette?.usesDirectPhysicalKeys ?? false;
      }
      _isLoading = connectedDeviceId != null && immediate.isEmpty;
    });

    if (connectedDeviceId == null) {
      return;
    }

    final Future<List<LitchSetting>> settingsRead = service.readSettings(
      targetId,
    );
    final Future<LitchEndPalette?> paletteRead = service
        .readPhysicalEndPalette(deviceId: targetId)
        .catchError((_) => null);
    bool ownsLoad() =>
        mounted &&
        loadGeneration == _settingsLoadGeneration &&
        viewGeneration == _viewGeneration &&
        service == widget.bleService &&
        boardId == widget.boardId;
    List<LitchSetting> loaded;
    try {
      loaded = await settingsRead;
    } on OwnerPairingNotReady {
      // Disconnect or GATT quarantine can retire authority during this read.
      // Preserve cached settings, and never settle a replacement load.
      if (ownsLoad()) setState(() => _isLoading = false);
      return;
    }
    LitchEndPalette? palette = await paletteRead;
    if (!ownsLoad()) return;

    final bool hasBoardStateSnapshot = service.hasBoardStateSnapshot;
    if (hasBoardStateSnapshot) {
      loaded = loaded
          .map(
            (setting) => _copyWithValue(
              setting,
              service.latestSettingValue(setting.key) ?? setting.value,
            ),
          )
          .toList();
      palette = _rebasePhysicalPalette(service, palette);
    }
    if (palette != null) {
      loaded = _settingsWithPhysicalPalette(loaded, palette);
    }
    if (hasBoardStateSnapshot) {
      // Apply all delayed transport projections before the ownership merge.
      // Otherwise a palette result could overwrite P1/P2 immediately after
      // the generic merge had correctly preserved a newer local edit.
      loaded = _mergeSettings(_settings, loaded, editorRevisions);
    }
    setState(() {
      _settings = _retainBootDisplayControl(loaded);
      _usesDirectPhysicalPaletteKeys = palette?.usesDirectPhysicalKeys ?? false;
      _isLoading = false;
    });
  }

  List<LitchSetting> _mergeSettings(
    List<LitchSetting> immediate,
    List<LitchSetting> confirmed,
    Map<String, int> editorRevisionsAtLoad,
  ) {
    final Map<String, LitchSetting> currentByKey = <String, LitchSetting>{
      for (final LitchSetting setting in immediate) setting.key: setting,
    };
    final List<LitchSetting> merged = confirmed.map((LitchSetting setting) {
      final LitchSetting? current = currentByKey[setting.key];
      final bool editedDuringLoad =
          (_editorRevisionByKey[setting.key] ?? 0) !=
          (editorRevisionsAtLoad[setting.key] ?? 0);
      final bool locallyOwned =
          _writingKeys.contains(setting.key) ||
          _hasLocalSliderOwnership(setting.key) ||
          (setting.key == _settleStyleKey && _placementEffectDraft != null);
      return current != null && (editedDuringLoad || locallyOwned)
          ? _copyWithValue(setting, current.value)
          : setting;
    }).toList();
    final Set<String> confirmedKeys = confirmed
        .map((LitchSetting setting) => setting.key)
        .toSet();
    return <LitchSetting>[
      ...merged,
      ...immediate.where(
        (LitchSetting setting) => !confirmedKeys.contains(setting.key),
      ),
    ];
  }

  List<LitchSetting> _settingsWithPhysicalPalette(
    List<LitchSetting> source,
    LitchEndPalette palette,
  ) {
    return source.map((LitchSetting setting) {
      if (setting.key == _p1PieceWheelKey) {
        return _copyWithValue(setting, palette.p1ControlsWheel);
      }
      if (setting.key == _p2PieceWheelKey) {
        return _copyWithValue(setting, palette.p2ControlsWheel);
      }
      return setting;
    }).toList();
  }

  bool _isHiddenSetting(LitchSetting setting) {
    // Reserved descriptor indices and controls owned by other screens stay hidden here.
    if (setting.key == 'BLE_CONNECTION_INDICATOR' ||
        setting.key == 'V3_BUTTON_LED_EMULATION' ||
        setting.key == 'P1_X_DEEP_WAKE_ENABLED' ||
        setting.key == 'P1_X_WAKE_HOLD_MS' ||
        setting.key == 'MOTION_WAKE_THRESHOLD_MG' ||
        setting.key == 'MOTION_WAKE_DURATION_MS' ||
        setting.key == 'TAP_WAKE_THRESHOLD_MG' ||
        setting.key == 'TAP_WAKE_SHOCK_MS' ||
        setting.key == 'HINT_LEVEL' ||
        setting.key == 'P1_AUTO_HINTS' ||
        setting.key == 'P2_AUTO_HINTS' ||
        setting.key == 'BRIGHTNESS' ||
        setting.key == 'AUTO_BRIGHTNESS') {
      return true;
    }
    for (final String prefix in _hiddenSettingKeyPrefixes) {
      if (setting.key.startsWith(prefix)) {
        return true;
      }
    }
    return false;
  }

  String _friendlySettingName(LitchSetting setting) {
    if (SettingsStateWireCodec.assistanceKeys.contains(setting.key)) {
      final strings = litchStrings(context);
      final feature = setting.key.endsWith('_CHECK_CUE')
          ? strings.checkCue
          : strings.legalMoves;
      return '${_playerRoleLabel(setting.key.startsWith('P1_') ? 1 : 2)}: $feature';
    }
    final String? named = switch (setting.key) {
      'BOOT_DISPLAY' => litchStrings(context).bootDisplay,
      'TURN_ANIM_STYLE' => litchStrings(context).turnAnimation,
      'HINT_THINKING_STYLE' => litchStrings(context).hintThinkingStyle,
      'CHECKMATE_ANIM_STYLE' => litchStrings(context).mateAnimation,
      'SLEEP_TIMEOUT' => litchStrings(context).sleepTimeout,
      'BLE_ENABLED' => litchStrings(context).bluetoothBoot,
      'MINIMAL_ANIMATIONS' => litchStrings(context).minimalAnimations,
      _settleStyleKey => litchStrings(context).placementEffect,
      'TIMER' => litchStrings(context).gameClock,
      'MOTION_WAKE_ENABLED' => litchStrings(context).motionWakeEnabled,
      'MOTION_WAKE_MODE' => litchStrings(context).motionWakeMode,
      'MOTION_WAKE_THRESHOLD_MG' => litchStrings(context).motionThreshold,
      'MOTION_WAKE_DURATION_MS' => litchStrings(context).motionDuration,
      'TAP_WAKE_THRESHOLD_MG' => litchStrings(context).tapThreshold,
      'TAP_WAKE_SHOCK_MS' => litchStrings(context).tapShock,
      'VS_CPU_LEVEL' => litchStrings(context).opponent,
      'VS_CPU_PLAYER' => litchStrings(context).computerPlays,
      'GAME_FIRST_PLAYER' => litchStrings(context).players,
      'BRIGHTNESS' => litchStrings(context).brightness,
      'AUTO_BRIGHTNESS' => litchStrings(context).automaticBrightness,
      'WHITE_SQUARES_ON' => litchStrings(context).lightSquares,
      _lightSquareModeKey => litchStrings(context).craftColor,
      _lightSquareHueKey => litchStrings(context).craftHue,
      _lightSquareBrightnessKey => litchStrings(context).brightness,
      'BOARD_ROTATE' => litchStrings(context).rotateBoard,
      'P1_PIECE_WHEEL' => litchStrings(context).p1EndColor,
      'P2_PIECE_WHEEL' => litchStrings(context).p2EndColor,
      'MOVE_CONFIRM' => litchStrings(context).confirmMoves,
      'PROMOTION_MODE' => litchStrings(context).promotionSetting,
      _boardSetupAssistanceKey => litchStrings(context).setupGuidance,
      _takebackGraceKey => litchStrings(context).takebackGrace,
      _ => null,
    };
    if (named != null) {
      return named;
    }
    final String desc = setting.description.trim();
    if (desc.isNotEmpty) {
      return desc.endsWith('.') ? desc.substring(0, desc.length - 1) : desc;
    }

    final List<String> parts = setting.key
        .split('_')
        .where((String token) => token.isNotEmpty)
        .toList();
    if (parts.isEmpty) {
      return setting.key;
    }

    return parts
        .map((String token) {
          final String lower = token.toLowerCase();
          return '${lower[0].toUpperCase()}${lower.substring(1)}';
        })
        .join(' ');
  }

  String _friendlyCategoryName(String category) {
    final String? localized = switch (category.toLowerCase()) {
      'display' => litchStrings(context).categoryDisplay,
      'animation' => litchStrings(context).categoryAnimation,
      'behavior' => litchStrings(context).categoryBehavior,
      'clock' => litchStrings(context).categoryClock,
      'hardware' => litchStrings(context).categoryHardware,
      'internal' => litchStrings(context).categoryInternal,
      'other' => litchStrings(context).otherCategory,
      _ => null,
    };
    if (localized != null) return localized;
    final String trimmed = category.trim();
    if (trimmed.isEmpty) {
      return litchStrings(context).otherCategory;
    }
    return trimmed
        .split(RegExp(r'[_\s]+'))
        .where((String token) => token.isNotEmpty)
        .map((String token) {
          final String lower = token.toLowerCase();
          return '${lower[0].toUpperCase()}${lower.substring(1)}';
        })
        .join(' ');
  }

  String _quickSettingLabel(String key) {
    return switch (key) {
      'VS_CPU_LEVEL' =>
        (_sliderDraftValues[key]?.round() ??
                    _sliderPendingValues[key] ??
                    (_settingByKey(key) == null
                        ? 0
                        : _pieceColorValue(_settingByKey(key)!))) ==
                0
            ? litchStrings(context).playAgainstAnotherPlayer
            : litchStrings(context).playAgainstComputer(
                _sliderDraftValues[key]?.round() ??
                    _sliderPendingValues[key] ??
                    _pieceColorValue(_settingByKey(key)!),
              ),
      'VS_CPU_PLAYER' => litchStrings(context).computerPlays,
      'GAME_FIRST_PLAYER' => litchStrings(context).players,
      _p1PieceWheelKey => litchStrings(context).p1Controls,
      _p2PieceWheelKey => litchStrings(context).p2Controls,
      'BRIGHTNESS' => litchStrings(context).brightness,
      'AUTO_BRIGHTNESS' => litchStrings(context).automaticBrightness,
      'WHITE_SQUARES_ON' => litchStrings(context).lightSquares,
      _lightSquareModeKey => litchStrings(context).craftColor,
      _lightSquareHueKey => litchStrings(context).craftHue,
      _lightSquareBrightnessKey => litchStrings(context).brightness,
      _settleStyleKey => litchStrings(context).placementEffect,
      'BOARD_ROTATE' => litchStrings(context).rotateBoard,
      _timerKey => litchStrings(context).gameClock,
      _ => key,
    };
  }

  String _settingSearchText(LitchSetting setting) {
    return <String>[
      _friendlySettingName(setting),
      setting.key,
      setting.category,
      _friendlyCategoryName(setting.category),
      setting.description,
    ].join(' ').toLowerCase();
  }

  bool _matchesSearch(LitchSetting setting) {
    final String query = _searchQuery.trim().toLowerCase();
    return query.isEmpty || _settingSearchText(setting).contains(query);
  }

  Map<String, List<LitchSetting>> _groupSettingsByCategory(
    List<LitchSetting> settings,
  ) {
    final Map<String, List<LitchSetting>> grouped =
        <String, List<LitchSetting>>{};
    for (final LitchSetting setting in settings) {
      final String category = setting.category.trim().isEmpty
          ? 'other'
          : setting.category.trim();
      grouped.putIfAbsent(category, () => <LitchSetting>[]).add(setting);
    }
    return grouped;
  }

  LitchSetting? _settingByKey(String key) {
    for (final LitchSetting setting in _settings) {
      if (setting.key == key && !_isHiddenSetting(setting)) {
        return setting;
      }
    }
    return null;
  }

  String _formatIntegerSettingValue(LitchSetting setting, int value) {
    if (setting.key == 'BRIGHTNESS' && value == 0) {
      return litchStrings(context).nearDark;
    }
    if (setting.key == 'VS_CPU_LEVEL') {
      return value == 0
          ? litchStrings(context).anotherPlayer
          : litchStrings(context).computerLevel(value);
    }
    if (setting.key == 'VS_CPU_PLAYER') {
      return value == 1 ? 'P1' : 'P2';
    }
    if (setting.key == 'GAME_FIRST_PLAYER') {
      return _physicalPlayerEndLabel(value);
    }
    if (setting.key == _takebackGraceKey) {
      if (value == 0) {
        return litchStrings(context).offLabel;
      }
      return litchStrings(context).secondsValue(value);
    }
    return '$value';
  }

  String _settingHelpText(LitchSetting setting) {
    if (setting.key == 'BOOT_DISPLAY') {
      return litchStrings(context).bootDisplayHelp;
    }
    if (setting.key == 'AUTO_BRIGHTNESS') {
      return _tryReadIntSettingValue('BRIGHTNESS') == 0
          ? litchStrings(context).automaticBrightnessHelp
          : '';
    }
    if (setting.key == 'BRIGHTNESS' && setting.min == 0) {
      return litchStrings(context).brightnessHelp;
    }
    if (setting.key == _takebackGraceKey) {
      return litchStrings(context).takebackHelp;
    }
    if (setting.key == _boardSetupAssistanceKey) {
      return litchStrings(context).setupGuidanceHelp;
    }
    if (setting.key == 'VS_CPU_PLAYER') {
      return litchStrings(context).computerPlaysHelp;
    }
    if (setting.key == 'GAME_FIRST_PLAYER') {
      return litchStrings(context).playerEndHelp;
    }
    return '';
  }

  String _physicalPlayerEndLabel(int firstPlayer) {
    final bool rotated = _tryReadBoolSettingValue('BOARD_ROTATE') ?? false;
    final bool playerOneOnUsbRight = (firstPlayer != 2) != rotated;
    return playerOneOnUsbRight
        ? litchStrings(context).usbRightPlayer1
        : litchStrings(context).usbLeftPlayer1;
  }

  Map<int, String>? _namedIntegerChoices(LitchSetting setting) {
    return switch (setting.key) {
      'VS_CPU_PLAYER' => const <int, String>{1: 'P1', 2: 'P2'},
      _ => null,
    };
  }

  bool? _tryReadBoolSettingValue(String key) {
    final int index = _findSettingIndex(key);
    if (index < 0) return null;
    final Object value = _settings[index].value;
    if (value is bool) return value;
    if (value is int) return value != 0;
    return switch (value.toString().toLowerCase()) {
      'true' || '1' => true,
      'false' || '0' => false,
      _ => null,
    };
  }

  Widget _buildPlayerSidesEditor(
    LitchSetting setting, {
    required bool enabled,
  }) {
    final int current = setting.value is int
        ? setting.value as int
        : int.tryParse(setting.value.toString()) ?? 1;
    final bool rotated = _tryReadBoolSettingValue('BOARD_ROTATE') ?? false;
    final bool playerOneOnUsbRight = (current != 2) != rotated;
    final strings = litchStrings(context);
    Widget assignment(String side, String player, String spokenLabel) =>
        Semantics(
          container: true,
          label: spokenLabel,
          excludeSemantics: true,
          child: Container(
            constraints: const BoxConstraints(minHeight: 48),
            alignment: Alignment.center,
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
            decoration: BoxDecoration(
              border: Border.all(
                color: Theme.of(context).colorScheme.outlineVariant,
              ),
              borderRadius: BorderRadius.circular(10),
            ),
            child: Text(
              '$side\n$player',
              textAlign: TextAlign.center,
              softWrap: false,
            ),
          ),
        );
    final right = assignment(
      strings.usbRightShort,
      strings.playerSpoken(playerOneOnUsbRight ? '1' : '2'),
      playerOneOnUsbRight ? strings.usbRightPlayer1 : strings.usbRightPlayer2,
    );
    final left = assignment(
      strings.usbLeftShort,
      strings.playerSpoken(playerOneOnUsbRight ? '2' : '1'),
      playerOneOnUsbRight ? strings.usbLeftPlayer2 : strings.usbLeftPlayer1,
    );
    return Column(
      key: const ValueKey<String>('setting-editor:GAME_FIRST_PLAYER'),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        LayoutBuilder(
          builder: (context, bounds) {
            double labelWidth(String label) {
              final painter = TextPainter(
                text: TextSpan(
                  text: label,
                  style: Theme.of(context).textTheme.bodyMedium,
                ),
                textDirection: Directionality.of(context),
                textScaler: MediaQuery.textScalerOf(context),
                locale: Localizations.localeOf(context),
              )..layout();
              final width = painter.width;
              painter.dispose();
              return width;
            }

            final available = (bounds.maxWidth - 8) / 2 - 20;
            final labels = [
              strings.usbRightShort,
              strings.usbLeftShort,
              strings.playerSpoken('1'),
              strings.playerSpoken('2'),
            ];
            return labels.any((label) => labelWidth(label) > available)
                ? Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [right, const SizedBox(height: 8), left],
                  )
                : Row(
                    children: [
                      Expanded(child: right),
                      const SizedBox(width: 8),
                      Expanded(child: left),
                    ],
                  );
          },
        ),
        const SizedBox(height: 8),
        OutlinedButton.icon(
          key: const ValueKey<String>('swap-players'),
          onPressed: !enabled
              ? null
              : () => _writeSetting(setting, current == 2 ? 1 : 2),
          icon: const Icon(Icons.swap_horiz_rounded),
          label: Text(strings.swapPlayers),
        ),
      ],
    );
  }

  String _formatEnumSettingValue(LitchSetting setting, String value) {
    if (setting.key == 'MOVE_CONFIRM') {
      return switch (value) {
        'settle_2s' => litchStrings(context).afterTwoSeconds,
        'settle_5s' => litchStrings(context).afterFiveSeconds,
        'press_circle_to_confirm' => litchStrings(context).confirmWithCircle,
        _ => _humanizeSettingToken(value),
      };
    }
    final String? localized = switch ((setting.key, value)) {
      ('BOOT_DISPLAY', 'fox') => litchStrings(context).bootDisplayFox,
      ('BOOT_DISPLAY', 'artwork') => litchStrings(context).bootDisplayArtwork,
      ('BOOT_DISPLAY', 'off') => litchStrings(context).bootDisplayOff,
      (_settleStyleKey, 'none') => litchStrings(context).hintThinkingNone,
      (_settleStyleKey, 'landing_flash') => litchStrings(
        context,
      ).placementEffectLandingFlash,
      (_settleStyleKey, 'mini_orbit') => litchStrings(
        context,
      ).placementEffectMiniOrbit,
      (_settleStyleKey, 'rainbow_orbit') => litchStrings(
        context,
      ).placementEffectRainbowOrbit,
      (_settleStyleKey, 'ripple') => litchStrings(
        context,
      ).placementEffectRipple,
      ('PROMOTION_MODE', 'queen') => litchStrings(context).promotionQueen,
      ('PROMOTION_MODE', 'choose') => litchStrings(context).promotionChoose,
      ('HINT_THINKING_STYLE', 'none') => litchStrings(context).hintThinkingNone,
      ('HINT_THINKING_STYLE', 'cauldron_brewing') => litchStrings(
        context,
      ).hintThinkingCauldronBrewing,
      ('HINT_THINKING_STYLE', 'beachball') => litchStrings(
        context,
      ).hintThinkingBeachball,
      ('HINT_THINKING_STYLE', 'foxie_sweep') => litchStrings(
        context,
      ).foxieThinkingSweep,
      ('HINT_THINKING_STYLE', 'player_clockwise') => litchStrings(
        context,
      ).playerThinkingClockwise,
      ('TURN_ANIM_STYLE', 'none') => litchStrings(context).hintThinkingNone,
      ('TURN_ANIM_STYLE', 'active_end_only') => litchStrings(
        context,
      ).activeEndOnly,
      ('TURN_ANIM_STYLE', 'active_end_pingpong_sides') => litchStrings(
        context,
      ).activeEndPingpong,
      ('TURN_ANIM_STYLE', 'away_from_active_end') => litchStrings(
        context,
      ).awayFromActiveEnd,
      ('TURN_ANIM_STYLE', 'both_ends') => litchStrings(context).bothEnds,
      ('TURN_ANIM_STYLE', 'side_middle_band') => litchStrings(
        context,
      ).sideMiddleBand,
      ('CHECKMATE_ANIM_STYLE', 'fireworks') => litchStrings(context).fireworks,
      ('CHECKMATE_ANIM_STYLE', 'fire-lob') => litchStrings(context).fireLob,
      ('CHECKMATE_ANIM_STYLE', 'ripple-bloom') => litchStrings(
        context,
      ).rippleBloom,
      ('CHECKMATE_ANIM_STYLE', 'winner-sweep') => litchStrings(
        context,
      ).winnerSweep,
      ('CHECKMATE_ANIM_STYLE', 'winner-constellation') => litchStrings(
        context,
      ).winnerConstellation,
      ('MOTION_WAKE_MODE', 'motion') => litchStrings(context).motionMode,
      ('MOTION_WAKE_MODE', 'tap') => litchStrings(context).tapMode,
      ('MOTION_WAKE_MODE', 'motion_and_tap') => litchStrings(
        context,
      ).motionAndTapMode,
      ('SLEEP_TIMEOUT', 'disabled') => litchStrings(context).neverSleep,
      _ => null,
    };
    if (localized != null) return localized;
    if (setting.key == 'SLEEP_TIMEOUT' && RegExp(r'^\d+m$').hasMatch(value)) {
      return litchStrings(
        context,
      ).minutesValue(int.parse(value.substring(0, value.length - 1)));
    }
    return _humanizeSettingToken(value);
  }

  String _humanizeSettingToken(String value) {
    final List<String> words = value
        .split(RegExp(r'[_\s-]+'))
        .where((String word) => word.isNotEmpty)
        .toList();
    if (words.isEmpty) {
      return value;
    }
    final String joined = words.join(' ').toLowerCase();
    return '${joined[0].toUpperCase()}${joined.substring(1)}';
  }

  List<LitchSetting> _orderVisibleSettings(List<LitchSetting> settings) {
    final List<LitchSetting> ordered = List<LitchSetting>.from(settings);
    final int graceIndex = ordered.indexWhere(
      (LitchSetting setting) => setting.key == _takebackGraceKey,
    );
    final int confirmationIndex = ordered.indexWhere(
      (LitchSetting setting) => setting.key == 'MOVE_CONFIRM',
    );
    if (graceIndex < 0 || confirmationIndex < 0) {
      return ordered;
    }

    final LitchSetting grace = ordered.removeAt(graceIndex);
    final int updatedConfirmationIndex = ordered.indexWhere(
      (LitchSetting setting) => setting.key == 'MOVE_CONFIRM',
    );
    ordered.insert(updatedConfirmationIndex + 1, grace);
    return ordered;
  }

  int _findSettingIndex(String key) {
    return _settings.indexWhere((LitchSetting item) => item.key == key);
  }

  LitchSetting _copyWithValue(LitchSetting source, Object value) {
    return LitchSetting(
      key: source.key,
      type: source.type,
      category: source.category,
      value: value,
      defaultValue: source.defaultValue,
      min: source.min,
      max: source.max,
      options: source.options,
      description: source.description,
    );
  }

  Future<bool> _writeSetting(LitchSetting setting, Object value) async {
    if (_writingKeys.contains(setting.key)) {
      setState(() {
        _queuedSettingValues[setting.key] = value;
        _setLocalSettingValue(setting.key, value);
      });
      return true;
    }

    final int index = _findSettingIndex(setting.key);
    if (index < 0) {
      return false;
    }

    final LitchSetting previous = _settings[index];
    final LitchSetting updated = _copyWithValue(previous, value);
    final bool pieceColor = _isPieceWheelKey(setting.key);
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;

    setState(() {
      _settings = List<LitchSetting>.from(_settings)..[index] = updated;
      _writingKeys.add(setting.key);
    });

    final String targetId = service.connectedId ?? boardId;
    final String transportKey = pieceColor
        ? _pieceColorTransportKey(setting.key)
        : setting.key;

    final LitchSettingWriteResult result = await service
        .writeSettingAcknowledged(
          deviceId: targetId,
          key: transportKey,
          value: value,
        );
    final bool acknowledged =
        result is LitchSettingWriteAcknowledged && result.outcome.ok;
    final bool stateConfirmed =
        result is! LitchSettingWriteAcknowledged ||
        !result.outcome.ok ||
        await (result.boardStateConfirmation ?? Future.value(true));

    if (!mounted ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId) {
      return false;
    }

    Object effectiveValue = value;
    if (result is LitchSettingWriteAcknowledged &&
        result.outcome.ok &&
        result.outcome.hasValue &&
        result.outcome.value != null) {
      effectiveValue = _coerceIncomingSettingValue(
        previous,
        result.outcome.value!,
      );
    }
    bool effectiveValueConfirmed =
        !pieceColor ||
        (result is LitchSettingWriteAcknowledged &&
            result.outcome.hasValue &&
            result.outcome.value != null);
    if (pieceColor &&
        acknowledged &&
        !effectiveValueConfirmed &&
        !service.hasBoardStateSnapshot) {
      final Object? readback = await service.readSettingValue(
        deviceId: targetId,
        key: transportKey,
      );
      if (!mounted ||
          generation != _viewGeneration ||
          connectionEpoch != _connectionEpoch ||
          service != widget.bleService ||
          boardId != widget.boardId) {
        return false;
      }
      if (readback != null) {
        effectiveValue = _coerceIncomingSettingValue(previous, readback);
        effectiveValueConfirmed = true;
      }
    }
    final bool hasSettingAuthority = service.hasBoardStateSnapshot;
    final Object? boardValue = hasSettingAuthority
        ? service.latestSettingValue(transportKey)
        : null;
    if (hasSettingAuthority) {
      // A newer board value can arrive while the editor owns its preview.
      if (boardValue != null) {
        effectiveValue = _coerceIncomingSettingValue(previous, boardValue);
      }
      effectiveValueConfirmed = true;
    }
    effectiveValueConfirmed = effectiveValueConfirmed && stateConfirmed;
    final bool ok = acknowledged && effectiveValueConfirmed;
    final Object? queuedValue = _queuedSettingValues.remove(setting.key);
    final LitchSetting confirmed = _copyWithValue(
      previous,
      ok ? effectiveValue : boardValue ?? previous.value,
    );

    setState(() {
      _writingKeys.remove(setting.key);
      _editorRevisionByKey.update(
        setting.key,
        (int revision) => revision + 1,
        ifAbsent: () => 1,
      );
      final int appliedIndex = _findSettingIndex(setting.key);
      if (appliedIndex >= 0) {
        _settings = List<LitchSetting>.from(_settings)
          ..[appliedIndex] = confirmed;
      }
    });

    // Only the newest intent survives an outstanding acknowledgement. Establish
    // its rollback value from that acknowledgement before sending it next.
    if (queuedValue != null) {
      if (queuedValue == confirmed.value) return ok;
      return _writeSetting(confirmed, queuedValue);
    }

    final bool supersededPlacement =
        setting.key == _settleStyleKey &&
        _placementEffectDraft != null &&
        _placementEffectDraft != value;
    if (!ok && !pieceColor && !supersededPlacement) {
      _notices.showNotice(
        context,
        SnackBar(
          content: Text(
            litchStrings(context).settingPreviousShown(
              stateConfirmed
                  ? _settingWriteMessage(setting, result)
                  : litchStrings(
                      context,
                    ).settingUnconfirmed(_friendlySettingName(setting)),
            ),
          ),
        ),
      );
    }
    if (ok && setting.key == 'GAME_FIRST_PLAYER') {
      unawaited(_reloadPhysicalEndPalette());
    }
    return ok;
  }

  String _pieceColorTransportKey(String uiKey) {
    if (_usesDirectPhysicalPaletteKeys) {
      return uiKey == _p1PieceWheelKey
          ? _p1EndPieceWheelKey
          : _p2EndPieceWheelKey;
    }
    return uiKey;
  }

  String _settingWriteMessage(
    LitchSetting setting,
    LitchSettingWriteResult result,
  ) {
    final String name = _friendlySettingName(setting);
    return switch (result) {
      LitchSettingWriteAcknowledged(outcome: final outcome) =>
        switch (outcome.reason) {
          LitchMutationReason.notAtStart => litchStrings(
            context,
          ).settingBeforePlay,
          LitchMutationReason.busy
              when _orientationCorrectionActive &&
                  (setting.key == 'GAME_FIRST_PLAYER' ||
                      setting.key == 'BOARD_ROTATE') =>
            litchStrings(context).finishKingQueenSwap,
          LitchMutationReason.busy ||
          LitchMutationReason.queueFull => litchStrings(context).boardBusyRetry,
          LitchMutationReason.storageUnavailable => litchStrings(
            context,
          ).settingSaveFailed(name),
          LitchMutationReason.unknownKey ||
          LitchMutationReason.typeMismatch ||
          LitchMutationReason.outOfRange ||
          LitchMutationReason.invalidEnumValue ||
          LitchMutationReason.badRequest => litchStrings(
            context,
          ).settingApplyFailed(name),
          _ => litchStrings(context).settingApplyFailed(name),
        },
      LitchSettingWriteNotAcknowledged(
        failure: LitchSettingWriteFailure.timedOut ||
            LitchSettingWriteFailure.malformedAcknowledgement,
      ) =>
        litchStrings(context).settingUnconfirmed(name),
      LitchSettingWriteNotAcknowledged() => litchStrings(
        context,
      ).settingSendFailed(name),
    };
  }

  void _setLocalSettingValue(String key, Object value) {
    final int index = _findSettingIndex(key);
    if (index < 0) {
      return;
    }
    final LitchSetting current = _settings[index];
    final LitchSetting updated = _copyWithValue(current, value);
    _settings = List<LitchSetting>.from(_settings)..[index] = updated;
  }

  bool _hasLocalSliderOwnership(String key) {
    return _sliderDraftValues.containsKey(key) ||
        _sliderPendingValues.containsKey(key) ||
        _sliderWriteInFlight[key] == true;
  }

  void _clearSliderOwnership() {
    _sliderDraftValues.clear();
    _sliderPendingValues.clear();
    for (final Timer timer in _sliderThrottleTimers.values) {
      timer.cancel();
    }
    _sliderThrottleTimers.clear();
    _sliderWriteInFlight.clear();
    _sliderLastWriteAt.clear();
    _sliderPendingAckKeys.clear();
  }

  bool _isPieceWheelKey(String key) {
    return key == _p1PieceWheelKey || key == _p2PieceWheelKey;
  }

  String? _otherPieceWheelKey(String key) {
    if (key == _p1PieceWheelKey) {
      return _p2PieceWheelKey;
    }
    if (key == _p2PieceWheelKey) {
      return _p1PieceWheelKey;
    }
    return null;
  }

  int? _tryReadIntSettingValue(String key) {
    final int index = _findSettingIndex(key);
    if (index < 0) {
      return null;
    }

    final Object value = _settings[index].value;
    if (value is int) {
      return value;
    }
    return int.tryParse(value.toString());
  }

  int _resolvePieceWheelValue(
    LitchSetting setting,
    int proposed, {
    int? dragFrom,
  }) {
    final otherKey = _otherPieceWheelKey(setting.key);
    if (otherKey == null) return proposed;
    final min = setting.min ?? 0;
    final max = setting.max ?? 255;
    final other = _tryReadIntSettingValue(otherKey);
    if (other == null || max <= min) return proposed.clamp(min, max);
    return LitchEndPalette.resolveWheel(
      proposed,
      other,
      min: min,
      max: max,
      dragFrom: dragFrom,
    );
  }

  Future<void> _flushSliderWrite(LitchSetting setting) async {
    final String key = setting.key;
    final String transportKey = _isPieceWheelKey(key)
        ? _pieceColorTransportKey(key)
        : key;
    if (_sliderWriteInFlight[key] == true) {
      return;
    }

    final int? pending = _sliderPendingValues.remove(key);
    if (pending == null) {
      return;
    }

    final bool awaitAck = _sliderPendingAckKeys.remove(key);
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;

    _sliderWriteInFlight[key] = true;
    _sliderLastWriteAt[key] = DateTime.now();

    if (awaitAck && _isPieceWheelKey(key)) {
      await _writeSetting(setting, pending);
      if (!mounted ||
          generation != _viewGeneration ||
          connectionEpoch != _connectionEpoch ||
          service != widget.bleService ||
          boardId != widget.boardId) {
        return;
      }
      setState(() => _sliderWriteInFlight[key] = false);
      if (_sliderPendingValues.containsKey(key)) {
        _scheduleSliderWrite(setting, immediate: false);
      } else {
        unawaited(_reloadPhysicalEndPalette());
      }
      return;
    }

    final String targetId = service.connectedId ?? boardId;
    final LitchSettingWriteResult? acknowledgement = awaitAck
        ? await service.writeSettingAcknowledged(
            deviceId: targetId,
            key: transportKey,
            value: pending,
          )
        : null;
    final bool stateConfirmed =
        acknowledgement is! LitchSettingWriteAcknowledged ||
        !acknowledgement.outcome.ok ||
        await (acknowledgement.boardStateConfirmation ?? Future.value(true));
    final bool ok = awaitAck
        ? acknowledgement is LitchSettingWriteAcknowledged &&
              acknowledgement.outcome.ok &&
              stateConfirmed
        : await service.writeSetting(
            deviceId: targetId,
            key: transportKey,
            value: pending,
          );

    if (!mounted ||
        generation != _viewGeneration ||
        connectionEpoch != _connectionEpoch ||
        service != widget.bleService ||
        boardId != widget.boardId) {
      return;
    }

    _sliderWriteInFlight[key] = false;

    if (awaitAck) {
      final Object? confirmed =
          acknowledgement is LitchSettingWriteAcknowledged &&
              acknowledgement.outcome.ok &&
              acknowledgement.outcome.hasValue
          ? acknowledgement.outcome.value
          : null;
      setState(() {
        if (ok && !_sliderPendingValues.containsKey(key)) {
          _setLocalSettingValue(key, confirmed ?? pending);
        }
      });
    }

    if (_sliderPendingValues.containsKey(key)) {
      _scheduleSliderWrite(setting, immediate: false);
      return;
    }

    if (awaitAck) {
      if (!ok && acknowledgement != null) {
        _notices.showNotice(
          context,
          SnackBar(
            content: Text(
              litchStrings(context).settingCheckingValue(
                stateConfirmed
                    ? _settingWriteMessage(setting, acknowledgement)
                    : litchStrings(
                        context,
                      ).settingUnconfirmed(_friendlySettingName(setting)),
              ),
            ),
          ),
        );
      }
      unawaited(_reloadSingleSettingFromBoard(key));
    }
  }

  void _scheduleSliderWrite(LitchSetting setting, {required bool immediate}) {
    final String key = setting.key;
    _sliderThrottleTimers[key]?.cancel();

    if (immediate) {
      _flushSliderWrite(setting);
      return;
    }

    final DateTime now = DateTime.now();
    final DateTime? lastWrite = _sliderLastWriteAt[key];
    final Duration elapsed = lastWrite == null
        ? _sliderWriteInterval
        : now.difference(lastWrite);
    final Duration wait = elapsed >= _sliderWriteInterval
        ? Duration.zero
        : (_sliderWriteInterval - elapsed);

    if (wait == Duration.zero) {
      _flushSliderWrite(setting);
      return;
    }

    _sliderThrottleTimers[key] = Timer(wait, () {
      _flushSliderWrite(setting);
    });
  }

  void _onSliderChanged(LitchSetting setting, double next) {
    final int rounded = _resolvePieceWheelValue(setting, next.round());
    setState(() {
      _sliderDraftValues[setting.key] = rounded.toDouble();
      _sliderPendingValues[setting.key] = rounded;
    });

    _scheduleSliderWrite(setting, immediate: false);
  }

  void _onSliderChangeEnd(LitchSetting setting, double next) {
    final int rounded = _resolvePieceWheelValue(setting, next.round());
    setState(() {
      _sliderDraftValues.remove(setting.key);
      _sliderPendingValues[setting.key] = rounded;
      _sliderPendingAckKeys.add(setting.key);
      if (!_isPieceWheelKey(setting.key)) {
        _setLocalSettingValue(setting.key, rounded);
      }
    });

    _scheduleSliderWrite(setting, immediate: true);
  }

  Future<void> _flushPlacementEffect(LitchSetting setting) async {
    final int generation = _viewGeneration;
    final int connectionEpoch = _connectionEpoch;
    while (mounted &&
        generation == _viewGeneration &&
        _placementEffectDraft != null) {
      final String requested = _placementEffectDraft!;
      await _writeSetting(setting, requested);
      if (!mounted ||
          generation != _viewGeneration ||
          connectionEpoch != _connectionEpoch) {
        return;
      }
      if (_placementEffectDraft == requested) {
        setState(() => _placementEffectDraft = null);
        return;
      }
    }
  }

  List<String> _enumOptions(LitchSetting setting) {
    final usesDropdown =
        setting.options.length > 4 &&
        !(setting.key == 'HINT_THINKING_STYLE' && setting.options.length == 5);
    if (setting.key == 'TURN_ANIM_STYLE' || usesDropdown) {
      return [...setting.options]..sort((a, b) {
        if (a == b) return 0;
        if (a == 'none') return -1;
        if (b == 'none') return 1;
        return compareLitchLabels(
          _formatEnumSettingValue(setting, a),
          _formatEnumSettingValue(setting, b),
        );
      });
    }
    final leading = switch (setting.key) {
      'HINT_THINKING_STYLE' => const ['none', 'player_clockwise'],
      _settleStyleKey => const [
        'none',
        'landing_flash',
        'mini_orbit',
        'rainbow_orbit',
        'ripple',
      ],
      _ => const <String>[],
    };
    return [
      for (final value in leading)
        if (setting.options.contains(value)) value,
      ...setting.options.where((value) => !leading.contains(value)),
    ];
  }

  Widget _buildShortEnumEditor(LitchSetting setting, {required bool enabled}) {
    final List<String> choices = _enumOptions(setting);
    final bool promotion = setting.key == 'PROMOTION_MODE';
    final String selected =
        (promotion ? null : _placementEffectDraft) ??
        (choices.contains(setting.value)
            ? setting.value.toString()
            : choices.contains('mini_orbit')
            ? 'mini_orbit'
            : choices.first);
    final bool canChange = enabled;
    void select(String? value) {
      if (canChange && value != null && value != selected) {
        if (promotion) {
          unawaited(_writeSetting(setting, value));
        } else {
          setState(() => _placementEffectDraft = value);
          if (!_writingKeys.contains(setting.key)) {
            unawaited(_flushPlacementEffect(setting));
          }
        }
      }
    }

    return SettingsChoicePills<String>(
      key: ValueKey<String>('setting-editor:${setting.key}'),
      optionKey: (value) => ValueKey(
        promotion ? 'promotion-mode:$value' : 'placement-effect:$value',
      ),
      segments: [
        for (final value in choices)
          ButtonSegment(
            value: value,
            label: Text(_formatEnumSettingValue(setting, value)),
          ),
      ],
      selected: {selected},
      onSelectionChanged: canChange ? (values) => select(values.single) : null,
    );
  }

  Widget _buildPlacementEffectDropdown(
    LitchSetting setting, {
    required bool enabled,
  }) {
    final choices = _enumOptions(setting);
    final selected = _placementEffectDraft ?? setting.value.toString();
    final revision = _editorRevisionByKey[setting.key] ?? 0;
    return LayoutBuilder(
      builder: (context, constraints) {
        final style = Theme.of(context).textTheme.titleMedium;
        final wraps = choices.any((value) {
          final text =
              TextPainter(
                text: TextSpan(
                  text: _formatEnumSettingValue(setting, value),
                  style: style,
                ),
                textDirection: Directionality.of(context),
                textScaler: MediaQuery.textScalerOf(context),
                maxLines: 1,
              )..layout(
                maxWidth: constraints.maxWidth > 48
                    ? constraints.maxWidth - 48
                    : 1,
              );
          final wraps = text.didExceedMaxLines;
          text.dispose();
          return wraps;
        });
        return KeyedSubtree(
          key: ValueKey<String>('setting-editor:${setting.key}'),
          child: DropdownButtonFormField<String>(
            key: ValueKey('setting-editor:${setting.key}:$selected:$revision'),
            isExpanded: true,
            isDense: !wraps,
            itemHeight: null,
            initialValue: choices.contains(selected) ? selected : choices.first,
            decoration: const InputDecoration(
              border: OutlineInputBorder(),
              contentPadding: EdgeInsets.symmetric(
                horizontal: 12,
                vertical: 12,
              ),
            ),
            items: [
              for (final value in choices)
                DropdownMenuItem(
                  value: value,
                  child: Text(_formatEnumSettingValue(setting, value)),
                ),
            ],
            onChanged: enabled
                ? (value) {
                    if (value == null) return;
                    // Re-selecting the current style is an intentional fresh preview.
                    setState(() => _placementEffectDraft = value);
                    if (!_writingKeys.contains(setting.key)) {
                      unawaited(_flushPlacementEffect(setting));
                    }
                  }
                : null,
          ),
        );
      },
    );
  }

  Widget _buildEditor(LitchSetting setting, {bool enabled = true}) {
    if (setting.key == 'GAME_FIRST_PLAYER') {
      return _buildPlayerSidesEditor(setting, enabled: enabled);
    }
    if (setting.key == _settleStyleKey && setting.options.length > 4) {
      return _buildPlacementEffectDropdown(setting, enabled: enabled);
    }
    if ((setting.key == _settleStyleKey || setting.key == 'PROMOTION_MODE') &&
        setting.options.isNotEmpty &&
        setting.options.length <= 4) {
      return _buildShortEnumEditor(setting, enabled: enabled);
    }
    if (setting.key == _timerKey) {
      final int current = setting.value is int
          ? setting.value as int
          : int.tryParse(setting.value.toString()) ?? 0;
      final int resolved = _timerPresetLabels.containsKey(current)
          ? current
          : 0;
      final int editorRevision = _editorRevisionByKey[setting.key] ?? 0;
      return KeyedSubtree(
        key: ValueKey<String>('setting-editor:${setting.key}'),
        child: DropdownButtonFormField<int>(
          itemHeight: null,
          key: ValueKey<String>(
            'setting-editor:${setting.key}:$resolved:$editorRevision',
          ),
          isExpanded: true,
          initialValue: resolved,
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 12),
          ),
          items: localizedClockPresets(litchStrings(context)).entries
              .map(
                (MapEntry<int, String> preset) => DropdownMenuItem<int>(
                  value: preset.key,
                  child: Text(preset.value),
                ),
              )
              .toList(),
          onChanged: !enabled
              ? null
              : (int? next) {
                  if (next != null) {
                    _writeSetting(setting, next);
                  }
                },
        ),
      );
    }

    switch (setting.type.toLowerCase()) {
      case 'bool':
        {
          final bool value = _effectiveBooleanValue(setting);
          final canChange =
              enabled &&
              !(setting.key == 'AUTO_BRIGHTNESS' &&
                  _tryReadIntSettingValue('BRIGHTNESS') == 0);
          return Switch.adaptive(
            key: ValueKey<String>('setting-editor:${setting.key}'),
            value: value,
            onChanged: !canChange
                ? null
                : (bool? next) {
                    if (next == null) {
                      return;
                    }
                    _writeSetting(setting, next);
                  },
          );
        }

      case 'enum':
        {
          final String current = setting.value.toString();
          final List<String> options = _enumOptions(setting);
          final String resolved = options.contains(current)
              ? current
              : (options.isNotEmpty ? options.first : current);
          final canChange =
              enabled &&
              !(setting.key == 'HINT_THINKING_STYLE' &&
                  _writingKeys.contains(setting.key));
          if (options.length >= 2 &&
              (options.length <= 4 ||
                  (setting.key == 'HINT_THINKING_STYLE' &&
                      options.length == 5))) {
            return SettingsChoicePills<String>(
              key: ValueKey<String>('setting-editor:${setting.key}'),
              trailingIcons: setting.key == 'BOOT_DISPLAY'
                  ? {
                      'fox': Image.asset(
                        'assets/branding/foxie-fox.png',
                        key: const ValueKey('boot-display-foxie-fox'),
                        width: 18,
                        height: 18,
                        fit: BoxFit.contain,
                        excludeFromSemantics: true,
                        filterQuality: FilterQuality.high,
                      ),
                    }
                  : const {},
              segments: [
                for (final value in options)
                  ButtonSegment(
                    value: value,
                    label: Text(_formatEnumSettingValue(setting, value)),
                  ),
              ],
              selected: {resolved},
              onSelectionChanged: canChange
                  ? (values) => _writeSetting(setting, values.single)
                  : null,
            );
          }
          final int editorRevision = _editorRevisionByKey[setting.key] ?? 0;
          return LayoutBuilder(
            builder: (context, constraints) {
              final style = Theme.of(context).textTheme.titleMedium;
              final wraps = options.any((option) {
                final text =
                    TextPainter(
                      text: TextSpan(
                        text: _formatEnumSettingValue(setting, option),
                        style: style,
                      ),
                      textDirection: Directionality.of(context),
                      textScaler: MediaQuery.textScalerOf(context),
                      maxLines: 1,
                    )..layout(
                      maxWidth: constraints.maxWidth > 48
                          ? constraints.maxWidth - 48
                          : 1,
                    );
                final wraps = text.didExceedMaxLines;
                text.dispose();
                return wraps;
              });
              return KeyedSubtree(
                key: ValueKey<String>('setting-editor:${setting.key}'),
                child: DropdownButtonFormField<String>(
                  key: ValueKey<String>(
                    'setting-editor:${setting.key}:$resolved:$editorRevision',
                  ),
                  isExpanded: true,
                  isDense: !wraps,
                  itemHeight: null,
                  initialValue: options.isEmpty ? null : resolved,
                  decoration: const InputDecoration(
                    border: OutlineInputBorder(),
                    contentPadding: EdgeInsets.symmetric(
                      horizontal: 12,
                      vertical: 12,
                    ),
                  ),
                  items: options
                      .map(
                        (String option) => DropdownMenuItem<String>(
                          value: option,
                          child: Text(_formatEnumSettingValue(setting, option)),
                        ),
                      )
                      .toList(),
                  onChanged:
                      (!enabled ||
                          options.isEmpty ||
                          (setting.key == 'HINT_THINKING_STYLE' &&
                              _writingKeys.contains(setting.key)))
                      ? null
                      : (String? next) {
                          if (next != null) {
                            _writeSetting(setting, next);
                          }
                        },
                ),
              );
            },
          );
        }

      case 'u8':
      case 'u32':
        {
          final int current = setting.value is int
              ? setting.value as int
              : int.tryParse(setting.value.toString()) ?? 0;
          final int min = setting.min ?? 0;
          final int max = setting.max ?? (current > 0 ? current : 100);
          final Map<int, String>? namedChoices = _namedIntegerChoices(setting);

          if (namedChoices != null && namedChoices.isNotEmpty) {
            final int selected = namedChoices.containsKey(current)
                ? current
                : namedChoices.keys.first;
            return SettingsChoicePills<int>(
              key: ValueKey<String>('setting-editor:${setting.key}'),
              segments: [
                for (final choice in namedChoices.entries)
                  ButtonSegment(value: choice.key, label: Text(choice.value)),
              ],
              selected: {selected},
              onSelectionChanged: enabled
                  ? (values) => _writeSetting(setting, values.single)
                  : null,
            );
          }

          if (max > min) {
            final double draft =
                _sliderDraftValues[setting.key] ??
                current.clamp(min, max).toDouble();
            final double clamped = draft.clamp(min.toDouble(), max.toDouble());
            final int range = max - min;
            final int? divisions = range > 0 && range <= 256 ? range : null;
            final String displayValue = _formatIntegerSettingValue(
              setting,
              clamped.round(),
            );
            final Widget sliderControl = Slider(
              key: ValueKey<String>('setting-editor:${setting.key}'),
              value: clamped,
              min: min.toDouble(),
              max: max.toDouble(),
              divisions: divisions,
              label: displayValue,
              semanticFormatterCallback: (double value) =>
                  _formatIntegerSettingValue(setting, value.round()),
              onChangeStart: !enabled
                  ? null
                  : (double value) => setState(() {
                      _sliderDraftValues[setting.key] = value;
                    }),
              onChanged: !enabled
                  ? null
                  : (double next) => _onSliderChanged(setting, next),
              onChangeEnd: !enabled
                  ? null
                  : (double next) => _onSliderChangeEnd(setting, next),
            );
            final Widget slider = setting.key == _lightSquareBrightnessKey
                ? MergeSemantics(
                    child: Semantics(
                      label:
                          '${litchStrings(context).lightSquares}, ${litchStrings(context).brightness}',
                      child: sliderControl,
                    ),
                  )
                : sliderControl;
            if (setting.key == 'VS_CPU_LEVEL') return slider;
            return LayoutBuilder(
              builder: (BuildContext context, BoxConstraints constraints) {
                final bool stack =
                    constraints.maxWidth < 320 ||
                    MediaQuery.textScalerOf(context).scale(1) > 1.3;
                final Widget valueLabel = Text(
                  displayValue,
                  style: Theme.of(context).textTheme.bodyMedium,
                );
                if (stack) {
                  return Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[valueLabel, slider],
                  );
                }
                return Row(
                  children: <Widget>[
                    SizedBox(
                      width: 144,
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: valueLabel,
                      ),
                    ),
                    const SizedBox(width: 8),
                    Expanded(child: slider),
                  ],
                );
              },
            );
          }

          return Text('$current');
        }

      case 'str':
      case 'string':
      default:
        return TextFormField(
          key: ValueKey<String>(
            'setting-editor:${setting.key}:${setting.value}',
          ),
          initialValue: setting.value.toString(),
          enabled: enabled,
          onFieldSubmitted: (String value) {
            _writeSetting(setting, value);
          },
          decoration: const InputDecoration(
            border: OutlineInputBorder(),
            isDense: true,
            contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 14),
          ),
        );
    }
  }

  bool _isInlineSettingControl(LitchSetting setting) {
    return setting.type.toLowerCase() == 'bool';
  }

  Future<void> _setEnableAppConsole(bool enabled) async {
    setState(() => _appConsoleDraft = enabled);
    if (_writingAppConsoleSetting) {
      return;
    }
    _writingAppConsoleSetting = true;
    try {
      while (mounted && _appConsoleDraft != null) {
        final bool requested = _appConsoleDraft!;
        await widget.onEnableAppConsoleChanged(requested);
        if (_appConsoleDraft == requested) break;
      }
    } finally {
      if (mounted) {
        setState(() {
          _writingAppConsoleSetting = false;
          _appConsoleDraft = null;
        });
      }
    }
  }

  Widget _buildSettingCard(LitchSetting setting, {required bool enabled}) {
    final canChange =
        enabled && !(setting.key == 'BOOT_DISPLAY' && _bootDisplayUnavailable);
    final bool inlineControl = _isInlineSettingControl(setting);
    final String friendlyName = _friendlySettingName(setting);
    final String helpText = _settingHelpText(setting);
    return Card(
      key: ValueKey<String>('setting:${setting.key}'),
      margin: const EdgeInsets.only(bottom: 8),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: inlineControl
            ? MergeSemantics(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    SettingsToggleRow(
                      title: _SettingTitle(title: friendlyName, helpText: ''),
                      control: _buildEditor(setting, enabled: canChange),
                    ),
                    if (helpText.isNotEmpty)
                      _SettingTitle(title: '', helpText: helpText),
                  ],
                ),
              )
            : Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  _SettingTitle(title: friendlyName, helpText: helpText),
                  const SizedBox(height: 12),
                  _buildEditor(setting, enabled: canChange),
                ],
              ),
      ),
    );
  }

  Widget _buildQuickSettingControl(String key, {required bool enabled}) {
    final String label = _quickSettingLabel(key);
    final LitchSetting? setting = _quickSettingKeys.contains(key)
        ? _settingByKey(key)
        : null;
    if (setting == null ||
        (!enabled && !_isLoading && _connectedBoardId != null)) {
      return SettingsUnavailableControl(
        key: ValueKey<String>('quick:$key:unavailable'),
        label: label,
      );
    }

    final Widget editor = _buildEditor(setting, enabled: enabled);
    if (_isInlineSettingControl(setting)) {
      return MergeSemantics(
        key: ValueKey<String>('quick:$key'),
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: SettingsToggleRow(
            title: Text(label, style: Theme.of(context).textTheme.bodyLarge),
            control: editor,
          ),
        ),
      );
    }

    return Semantics(
      key: ValueKey<String>('quick:$key'),
      container: true,
      label: litchStrings(context).settingSemantics(label),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(label),
          if (key == 'BRIGHTNESS' && setting.min == 0) ...<Widget>[
            const SizedBox(height: 4),
            Text(
              _settingHelpText(setting),
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
          if (key == _settleStyleKey &&
              (_placementEffectDraft ?? setting.value) != 'none' &&
              _settingByKey('MINIMAL_ANIMATIONS')?.value == true) ...<Widget>[
            const SizedBox(height: 4),
            Text(
              litchStrings(context).placementEffectMinimalHelp,
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
          const SizedBox(height: 8),
          editor,
        ],
      ),
    );
  }

  Widget _buildLightSquareAppearance({required bool enabled}) {
    final LitchSetting? mode = _settingByKey(_lightSquareModeKey);
    final LitchSetting? hue = _settingByKey(_lightSquareHueKey);
    final LitchSetting? brightness = _settingByKey(_lightSquareBrightnessKey);
    if (mode == null || hue == null || brightness == null) {
      return const SizedBox.shrink();
    }

    final bool usesColor = mode.value.toString() == 'color';
    final int hueValue =
        (_sliderDraftValues[_lightSquareHueKey]?.round() ??
                _pieceColorValue(hue))
            .clamp(hue.min ?? 0, hue.max ?? 255);
    final double brightnessValue =
        (_sliderDraftValues[_lightSquareBrightnessKey] ??
                _pieceColorValue(brightness).toDouble())
            .clamp(
              (brightness.min ?? 1).toDouble(),
              (brightness.max ?? 8).toDouble(),
            );

    return Column(
      key: const ValueKey<String>('light-square-appearance'),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        const SizedBox(height: 16),
        SettingsChoicePills<String>(
          key: const ValueKey<String>('light-square-mode'),
          segments: <ButtonSegment<String>>[
            ButtonSegment<String>(
              value: 'white',
              icon: const Icon(Icons.light_mode_outlined),
              label: Text(litchStrings(context).craftColorWhite),
            ),
            ButtonSegment<String>(
              value: 'color',
              icon: const Icon(Icons.palette_outlined),
              label: Text(litchStrings(context).craftColor),
            ),
          ],
          selected: <String>{usesColor ? 'color' : 'white'},
          onSelectionChanged: enabled
              ? (Set<String> values) {
                  if (values.isNotEmpty && values.single != mode.value) {
                    _writeSetting(mode, values.single);
                  }
                }
              : null,
        ),
        const SizedBox(height: 8),
        _buildLightSquareSliderRow(
          label: litchStrings(context).brightness,
          value: Text(
            '${brightnessValue.round()}',
            textAlign: TextAlign.center,
          ),
          slider: MergeSemantics(
            child: Semantics(
              label:
                  '${litchStrings(context).lightSquares}, ${litchStrings(context).brightness}',
              child: Slider(
                key: const ValueKey<String>(
                  'setting-editor:LIGHT_SQUARE_BRIGHTNESS',
                ),
                value: brightnessValue,
                min: (brightness.min ?? 1).toDouble(),
                max: (brightness.max ?? 8).toDouble(),
                divisions: (brightness.max ?? 8) - (brightness.min ?? 1),
                label: '${brightnessValue.round()}',
                semanticFormatterCallback: (value) => '${value.round()}',
                onChangeStart: enabled
                    ? (value) => setState(() {
                        _sliderDraftValues[brightness.key] = value;
                      })
                    : null,
                onChanged: enabled
                    ? (value) => _onSliderChanged(brightness, value)
                    : null,
                onChangeEnd: enabled
                    ? (value) => _onSliderChangeEnd(brightness, value)
                    : null,
              ),
            ),
          ),
        ),
        if (usesColor) ...<Widget>[
          _buildLightSquareSliderRow(
            label: litchStrings(context).craftHue,
            value: Container(
              key: const ValueKey<String>('light-square-hue-preview'),
              width: 12,
              height: 12,
              decoration: BoxDecoration(
                color: litchWheelPreviewColor(hueValue),
                shape: BoxShape.circle,
                border: Border.all(
                  color: Theme.of(context).colorScheme.outline,
                ),
              ),
            ),
            slider: ColorSlider(
              sliderKey: const ValueKey<String>('light-square-hue'),
              value: hueValue / 255 * 360,
              divisions: 255,
              label: litchStrings(context).craftHue,
              gradient: _wheelPreviewGradient,
              onChanged: !enabled
                  ? null
                  : (double value) => _onSliderChanged(
                      hue,
                      (value / 360 * 255).roundToDouble(),
                    ),
              onChangeEnd: !enabled
                  ? null
                  : (double value) => _onSliderChangeEnd(
                      hue,
                      (value / 360 * 255).roundToDouble(),
                    ),
            ),
          ),
        ],
      ],
    );
  }

  Widget _buildLightSquareSliderRow({
    required String label,
    required Widget slider,
    required Widget value,
  }) => LayoutBuilder(
    builder: (context, constraints) {
      final bool stack =
          constraints.maxWidth < 280 ||
          MediaQuery.textScalerOf(context).scale(1) > 1.3;
      if (stack) {
        return Column(
          children: <Widget>[
            Row(
              children: <Widget>[
                Expanded(child: Text(label)),
                Flexible(child: value),
              ],
            ),
            slider,
          ],
        );
      }
      return Row(
        children: <Widget>[
          SizedBox(width: 80, child: Text(label)),
          Expanded(child: slider),
          SizedBox(width: 64, child: value),
        ],
      );
    },
  );

  int _pieceColorValue(LitchSetting setting) {
    final Object raw = setting.value;
    final int parsed = raw is int ? raw : int.tryParse(raw.toString()) ?? 0;
    return parsed.clamp(setting.min ?? 0, setting.max ?? 255);
  }

  String _pieceColorPlayerLabel(String key) {
    return key == _p1PieceWheelKey
        ? litchStrings(context).p1Controls
        : litchStrings(context).p2Controls;
  }

  String _pieceColorPickerTitle(String key) {
    return key == _p1PieceWheelKey
        ? litchStrings(context).chooseP1Color
        : litchStrings(context).chooseP2Color;
  }

  String _pieceColorSpokenPlayerLabel(String key) {
    return key == _p1PieceWheelKey
        ? litchStrings(context).physicalP1End
        : litchStrings(context).physicalP2End;
  }

  bool get _pieceColorWriteInFlight =>
      _writingKeys.any(_isPieceWheelKey) ||
      <String>[
        _p1PieceWheelKey,
        _p2PieceWheelKey,
      ].any(_hasLocalSliderOwnership);

  Future<void> showPlayerColorPicker({
    required bool whitePiece,
    required bool p1AtOppositeEnd,
  }) async {
    if (_recoveryOnly || _connectedBoardId == null) return;
    final generation = _viewGeneration;
    final epoch = _connectionEpoch;
    final useP2 = !whitePiece != p1AtOppositeEnd;
    final key = useP2 ? _p2PieceWheelKey : _p1PieceWheelKey;
    Future<void>? loading;
    if (!_settingsLoadStarted) loading = _loadSettings();
    if (_settingByKey(key) == null) await (loading ?? _loadSettings());
    if (!mounted ||
        generation != _viewGeneration ||
        epoch != _connectionEpoch ||
        _connectedBoardId == null) {
      return;
    }
    final setting = _settingByKey(key);
    if (setting == null) return;
    _showPieceColorPicker(
      setting,
      title: whitePiece
          ? litchStrings(context).chooseP1Color
          : litchStrings(context).chooseP2Color,
    );
  }

  void _showPieceColorPicker(LitchSetting setting, {String? title}) {
    final int current = _pieceColorValue(setting);
    var draft = current;
    showModalBottomSheet<void>(
      context: context,
      useSafeArea: true,
      isScrollControlled: true,
      showDragHandle: true,
      builder: (BuildContext sheetContext) => StatefulBuilder(
        builder: (sheetContext, setSheetState) {
          final bool useSingleColumn =
              MediaQuery.textScalerOf(sheetContext).scale(1) > 1.3;
          return SafeArea(
            top: false,
            left: false,
            right: false,
            child: SingleChildScrollView(
              key: const ValueKey<String>('piece-color-picker-scroll'),
              padding: const EdgeInsets.fromLTRB(20, 0, 20, 24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: <Widget>[
                  Semantics(
                    header: true,
                    child: Text(
                      title ?? _pieceColorPickerTitle(setting.key),
                      style: Theme.of(sheetContext).textTheme.headlineSmall,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    litchStrings(context).colorPreviewHelp,
                    style: Theme.of(sheetContext).textTheme.bodyMedium
                        ?.copyWith(
                          color: Theme.of(
                            sheetContext,
                          ).colorScheme.onSurfaceVariant,
                        ),
                  ),
                  const SizedBox(height: 16),
                  Text(litchStrings(context).craftHue),
                  ColorSlider(
                    sliderKey: ValueKey('piece-color-hue:${setting.key}'),
                    value: draft / 255 * 360,
                    divisions: 255,
                    label: litchStrings(context).craftHue,
                    gradient: _wheelPreviewGradient,
                    onChanged: (value) {
                      final proposed = (value / 360 * 255).round();
                      final resolved = _resolvePieceWheelValue(
                        setting,
                        proposed,
                        dragFrom: draft,
                      );
                      setSheetState(() => draft = resolved);
                      _onSliderChanged(setting, resolved.toDouble());
                    },
                    onChangeEnd: (value) {
                      final proposed = (value / 360 * 255).round();
                      final resolved = _resolvePieceWheelValue(
                        setting,
                        proposed,
                        dragFrom: draft,
                      );
                      setSheetState(() => draft = resolved);
                      _onSliderChangeEnd(setting, resolved.toDouble());
                    },
                  ),
                  Wrap(
                    spacing: 12,
                    runSpacing: 8,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: [
                      Container(
                        key: const ValueKey('piece-color-hue-preview'),
                        width: 32,
                        height: 32,
                        decoration: BoxDecoration(
                          color: litchWheelPreviewColor(draft),
                          shape: BoxShape.circle,
                          border: Border.all(
                            color: Theme.of(sheetContext).colorScheme.outline,
                          ),
                        ),
                      ),
                      Text(localizedPieceColor(litchStrings(context), draft)),
                    ],
                  ),
                  const SizedBox(height: 16),
                  LayoutBuilder(
                    builder: (BuildContext context, BoxConstraints constraints) {
                      final double itemWidth = useSingleColumn
                          ? constraints.maxWidth
                          : (constraints.maxWidth - 12) / 2;
                      return Wrap(
                        spacing: 12,
                        runSpacing: 12,
                        children: litchPieceColorChoices.map((
                          LitchPieceColorChoice choice,
                        ) {
                          final bool selected =
                              litchClosestPieceColorChoice(draft).wheel ==
                              choice.wheel;
                          return SizedBox(
                            width: itemWidth,
                            child: _PieceColorChoiceTile(
                              key: ValueKey<String>(
                                'piece-color-choice:${setting.key}:${choice.wheel}',
                              ),
                              choice: choice,
                              selected: selected,
                              available: true,
                              unavailableReason: '',
                              onTap: () {
                                Navigator.of(sheetContext).pop();
                                final LitchSetting fresh =
                                    _settingByKey(setting.key) ?? setting;
                                final resolved = _resolvePieceWheelValue(
                                  fresh,
                                  choice.wheel,
                                );
                                if (resolved == _pieceColorValue(fresh) &&
                                    !_hasLocalSliderOwnership(fresh.key)) {
                                  return;
                                }
                                _onSliderChangeEnd(fresh, resolved.toDouble());
                              },
                            ),
                          );
                        }).toList(),
                      );
                    },
                  ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }

  Widget _buildPlayerColorControl(String key, {required bool enabled}) {
    final LitchSetting? setting = _settingByKey(key);
    final String playerLabel = _pieceColorPlayerLabel(key);
    if (setting == null) {
      return SettingsUnavailableControl(label: playerLabel);
    }
    final int value =
        _sliderDraftValues[key]?.round() ??
        _sliderPendingValues[key] ??
        _pieceColorValue(setting);
    return _PlayerColorRow(
      key: ValueKey<String>('quick:$key'),
      playerLabel: playerLabel,
      spokenPlayerLabel: _pieceColorSpokenPlayerLabel(key),
      colorName: localizedPieceColor(litchStrings(context), value),
      previewColor: litchWheelPreviewColor(value),
      available: enabled,
      enabled: enabled,
      onTap: enabled ? () => _showPieceColorPicker(setting) : null,
    );
  }

  void _openPage(_SetupPage page) {
    if (page == _page) return;
    setState(() {
      _resetSettingsSearch();
      final existing = _pageTrail.indexOf(page);
      if (existing >= 0) {
        _pageTrail.removeRange(existing + 1, _pageTrail.length);
      } else {
        _pageTrail.add(page);
      }
    });
  }

  void _goBack() => _setupNavigatorKey.currentState?.pop();

  void _didRemovePage(Page<Object?> page) {
    final index = _pageTrail.indexWhere(
      (candidate) => _pageKey(candidate) == page.key,
    );
    if (index > 0) {
      setState(() {
        _resetSettingsSearch();
        _pageTrail.removeRange(index, _pageTrail.length);
      });
    }
  }

  ValueKey<String> _pageKey(_SetupPage page) =>
      ValueKey<String>('setup:$_viewGeneration:${page.name}');

  String _pageTitle(_SetupPage page) => switch (page) {
    _SetupPage.overview => litchStrings(context).setupTab,
    _SetupPage.lights => litchStrings(context).boardLights,
    _SetupPage.behavior => litchStrings(context).setupGameBehavior,
    _SetupPage.phone => litchStrings(context).setupThisPhone,
    _SetupPage.advanced => litchStrings(context).advancedSettings,
    _SetupPage.developer => litchStrings(context).developerTools,
  };

  Widget _destination(_SetupPage page, Widget icon, {String? subtitle}) =>
      ListTile(
        key: ValueKey<String>('setup-open:${page.name}'),
        contentPadding: EdgeInsets.zero,
        minTileHeight: 64,
        leading: icon,
        title: Text(_pageTitle(page)),
        subtitle: subtitle == null ? null : Text(subtitle),
        trailing: const Icon(Icons.chevron_right_rounded),
        onTap: () => _openPage(page),
      );

  Widget _boardPreview({required double maxSize}) {
    final p1 = _settingByKey(_p1PieceWheelKey);
    final p2 = _settingByKey(_p2PieceWheelKey);
    final confirmedRotation = _confirmedRoleValue('BOARD_ROTATE');
    final rotated = confirmedRotation is bool ? confirmedRotation : null;
    if (p1 == null || p2 == null || rotated == null) {
      return const SizedBox.shrink();
    }
    final theme = Theme.of(context);
    final portWidth = 16 + MediaQuery.textScalerOf(context).scale(12);
    Color color(LitchSetting setting) => litchWheelPreviewColor(
      _sliderDraftValues[setting.key]?.round() ??
          _sliderPendingValues[setting.key] ??
          _pieceColorValue(setting),
    );
    final first = _confirmedRoleValue('GAME_FIRST_PLAYER');
    int? playerAtEnd(LitchSetting setting) => first is! int
        ? null
        : ((setting.key == _p1PieceWheelKey) != (first == 2) ? 1 : 2);
    Widget end(LitchSetting setting, {required bool opposite}) => Semantics(
      label:
          '${_playerRoleLabel(playerAtEnd(setting))}, '
          '${localizedPieceColor(litchStrings(context), _pieceColorValue(setting))}'
          '${_cpuPlayer(playerAtEnd(setting)) == true ? ', ${litchStrings(context).hintAction}/${litchStrings(context).coaching}: ${litchStrings(context).unavailableLabel}' : ''}',
      child: ExcludeSemantics(
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 8),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              RotatedBox(
                key: ValueKey<String>(
                  'setup-board-controls:${opposite ? 'opposite' : 'near'}',
                ),
                quarterTurns: opposite ? 2 : 0,
                child: BoardControlsGlyph(
                  width: 60,
                  color: color(setting),
                  optionalOAvailable: _cpuPlayer(playerAtEnd(setting)) == false,
                ),
              ),
              const SizedBox(width: 10),
              Flexible(
                child: Text(
                  _playerRoleLabel(playerAtEnd(setting)),
                  style: theme.textTheme.bodyLarge,
                ),
              ),
            ],
          ),
        ),
      ),
    );
    Widget port({required bool left}) => ExcludeSemantics(
      child: SizedBox(
        width: portWidth,
        child: Row(
          textDirection: TextDirection.ltr,
          mainAxisAlignment: MainAxisAlignment.center,
          children: <Widget>[
            if (left)
              RotatedBox(
                key: const ValueKey<String>('setup-usb-label'),
                quarterTurns: 3,
                child: Text('USB', style: theme.textTheme.bodySmall),
              ),
            if (left) const SizedBox(width: 4),
            Container(
              key: const ValueKey<String>('setup-usb-port'),
              width: 4,
              height: 10,
              decoration: BoxDecoration(
                border: Border.all(color: theme.colorScheme.onSurfaceVariant),
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            if (!left) const SizedBox(width: 4),
            if (!left)
              RotatedBox(
                key: const ValueKey<String>('setup-usb-label'),
                quarterTurns: 1,
                child: Text('USB', style: theme.textTheme.bodySmall),
              ),
          ],
        ),
      ),
    );
    return Semantics(
      key: const ValueKey<String>('setup-board-preview'),
      container: true,
      label: litchStrings(context).setupBoardPreview,
      child: Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: maxSize + 2 * portWidth),
          child: Column(
            children: <Widget>[
              end(rotated ? p1 : p2, opposite: true),
              Row(
                textDirection: TextDirection.ltr,
                children: <Widget>[
                  if (!rotated)
                    port(left: true)
                  else
                    SizedBox(width: portWidth),
                  Expanded(
                    child: ExcludeSemantics(
                      // This is geometry, not a fabricated game or live position.
                      child: HomeChessboard(
                        snapshot: const HomeBoardSnapshot(),
                        liveGuidanceAvailable: false,
                        viewFromOppositeEnd: false,
                        maxSize: maxSize,
                      ),
                    ),
                  ),
                  if (rotated)
                    port(left: false)
                  else
                    SizedBox(width: portWidth),
                ],
              ),
              end(rotated ? p2 : p1, opposite: false),
              Text(
                '${litchStrings(context).rotateBoard}: '
                '${rotated ? litchStrings(context).onLabel : litchStrings(context).offLabel}',
                style: theme.textTheme.bodyMedium,
              ),
            ],
          ),
        ),
      ),
    );
  }

  Object? _confirmedRoleValue(String key) {
    final current = widget.bleService.latestSettingValue(key);
    if (current != null) return current;
    if (_writingKeys.contains(key) || _hasLocalSliderOwnership(key)) {
      return null;
    }
    return _settingByKey(key)?.value;
  }

  bool? _cpuPlayer(int? player) {
    if (player == null) return null;
    final level = _confirmedRoleValue('VS_CPU_LEVEL');
    if (level is! int) return null;
    if (level == 0) return false;
    final owner = _confirmedRoleValue('VS_CPU_PLAYER');
    return owner is! int ? null : owner == player;
  }

  bool _effectiveBooleanValue(LitchSetting setting) {
    return setting.value is bool
        ? setting.value as bool
        : setting.value.toString().toLowerCase() == 'true';
  }

  String _playerRoleLabel(int? player) {
    final strings = litchStrings(context);
    if (player == null) return strings.playerRoleUnavailable;
    return _cpuPlayer(player) == true
        ? (player == 1 ? strings.cpuPlayer1 : strings.cpuPlayer2)
        : strings.playerSpoken('$player');
  }

  Widget _assistanceControls({required bool enabled}) {
    final strings = litchStrings(context);
    final labels = <String, String>{
      'LEGAL_MOVES': strings.legalMoves,
      'CHECK_CUE': strings.checkCue,
    };
    return Card(
      key: const ValueKey<String>('setup-assistance'),
      margin: const EdgeInsets.only(bottom: 8),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            SettingsSectionHeading(title: strings.assistance),
            for (final player in <int>[1, 2]) ...<Widget>[
              const SizedBox(height: 16),
              Text(
                _playerRoleLabel(player),
                style: Theme.of(context).textTheme.titleMedium,
              ),
              for (final feature in labels.entries)
                Builder(
                  builder: (context) {
                    final key = 'P${player}_${feature.key}';
                    final setting = _settingByKey(key);
                    if (setting == null) {
                      return SettingsUnavailableControl(label: feature.value);
                    }
                    return MergeSemantics(
                      child: Semantics(
                        label: _playerRoleLabel(player),
                        child: SwitchListTile.adaptive(
                          key: ValueKey<String>('assistance:$key'),
                          contentPadding: EdgeInsets.zero,
                          title: Text(feature.value),
                          value: _effectiveBooleanValue(setting),
                          onChanged: enabled && !_writingKeys.contains(key)
                              ? (value) =>
                                    unawaited(_writeSetting(setting, value))
                              : null,
                        ),
                      ),
                    );
                  },
                ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _commonControls({required bool enabled}) {
    final opponent = _settingByKey('VS_CPU_LEVEL');
    final level =
        _sliderDraftValues['VS_CPU_LEVEL']?.round() ??
        (opponent == null ? 0 : _pieceColorValue(opponent));
    return Column(
      key: const ValueKey<String>('setup-common-controls'),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _buildQuickSettingControl('VS_CPU_LEVEL', enabled: enabled),
        if (opponent != null)
          Visibility(
            key: const ValueKey<String>('quick:VS_CPU_PLAYER:visibility'),
            visible: level > 0,
            maintainState: _sliderDraftValues.containsKey('VS_CPU_LEVEL'),
            maintainAnimation: _sliderDraftValues.containsKey('VS_CPU_LEVEL'),
            maintainSize: _sliderDraftValues.containsKey('VS_CPU_LEVEL'),
            child: Padding(
              padding: const EdgeInsets.only(top: 16),
              child: _buildQuickSettingControl(
                'VS_CPU_PLAYER',
                enabled: enabled,
              ),
            ),
          ),
        const Divider(height: 32),
        _buildQuickSettingControl('GAME_FIRST_PLAYER', enabled: enabled),
      ],
    );
  }

  Widget _overview({
    required bool enabled,
    required double previewSize,
    required bool split,
    Widget? support,
  }) {
    final controls = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _buildPlayerColorControl(
          (_confirmedRoleValue('BOARD_ROTATE') == true)
              ? _p1PieceWheelKey
              : _p2PieceWheelKey,
          enabled: enabled,
        ),
        const SizedBox(height: 12),
        _boardPreview(maxSize: previewSize),
        const SizedBox(height: 12),
        _buildPlayerColorControl(
          (_confirmedRoleValue('BOARD_ROTATE') == true)
              ? _p2PieceWheelKey
              : _p1PieceWheelKey,
          enabled: enabled,
        ),
        const SizedBox(height: 20),
        _commonControls(enabled: enabled),
      ],
    );
    final destinations = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _destination(
          _SetupPage.lights,
          const Icon(Icons.light_mode_outlined),
          subtitle: litchStrings(context).boardLightsDescription,
        ),
        _destination(
          _SetupPage.behavior,
          const LitchPlayGlyph(),
          subtitle: litchStrings(context).setupGameBehaviorDescription,
        ),
        const SizedBox(height: 20),
        _destination(
          _SetupPage.phone,
          const Icon(Icons.phone_android_outlined),
        ),
        _destination(_SetupPage.advanced, const Icon(Icons.settings_outlined)),
        _destination(_SetupPage.developer, const Icon(Icons.code_rounded)),
        if (support != null) ...[const SizedBox(height: 20), support],
      ],
    );
    if (!split) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [controls, const SizedBox(height: 20), destinations],
      );
    }
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Expanded(child: controls),
        const SizedBox(width: 24),
        Expanded(child: destinations),
      ],
    );
  }

  bool _browseSetting(LitchSetting setting) =>
      (setting.key != _timerKey || !_homeClockProjectionObserved) &&
      !_isPieceWheelKey(setting.key);

  bool _advancedSetting(LitchSetting setting) =>
      _browseSetting(setting) &&
      !SettingsStateWireCodec.assistanceKeys.contains(setting.key) &&
      !_quickSettingKeys.contains(setting.key) &&
      !_behaviorKeys.contains(setting.key) &&
      !const {
        'BOOT_DISPLAY',
        'MINIMAL_ANIMATIONS',
        'HINT_THINKING_STYLE',
        'CHECKMATE_ANIM_STYLE',
        'TURN_ANIM_STYLE',
      }.contains(setting.key);

  Widget _settingsList(
    List<LitchSetting> settings, {
    required bool enabled,
    bool showCategories = true,
  }) {
    final groups = _groupSettingsByCategory(settings);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        if (settings.isEmpty) Text(litchStrings(context).noSettings),
        for (final entry in groups.entries) ...<Widget>[
          if (showCategories) ...<Widget>[
            SettingsSectionHeading(title: _friendlyCategoryName(entry.key)),
            const SizedBox(height: 8),
          ],
          for (final setting in entry.value)
            _buildSettingCard(setting, enabled: enabled),
          const SizedBox(height: 20),
        ],
      ],
    );
  }

  void _resetSettingsSearch() {
    _searchFocusNode.unfocus();
    _searchController.clear();
    _searchQuery = '';
    if (_searchResultsScrollController.hasClients) {
      _searchResultsScrollController.jumpTo(0);
    }
  }

  void _clearSettingsSearch() {
    _searchController.clear();
    setState(() => _searchQuery = '');
    _searchFocusNode.requestFocus();
  }

  Widget _settingsSearchField() => TextField(
    key: const ValueKey<String>('advanced-search'),
    controller: _searchController,
    focusNode: _searchFocusNode,
    textInputAction: TextInputAction.search,
    onSubmitted: (_) => _searchFocusNode.unfocus(),
    onChanged: (value) {
      if (_searchResultsScrollController.hasClients) {
        _searchResultsScrollController.jumpTo(0);
      }
      setState(() => _searchQuery = value);
    },
    decoration: _settingsSearchDecoration(),
  );

  InputDecoration _settingsSearchDecoration() => InputDecoration(
    labelText: litchStrings(context).searchSettings,
    floatingLabelBehavior: FloatingLabelBehavior.always,
    isDense: true,
    contentPadding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
    hintText: litchStrings(context).searchSettingsHint,
    prefixIcon: const Icon(Icons.search),
    suffixIcon: _searchQuery.isNotEmpty
        ? IconButton(
            key: const ValueKey<String>('advanced-search-clear'),
            tooltip: litchStrings(context).clearSettingsSearch,
            constraints: const BoxConstraints(minWidth: 48, minHeight: 48),
            onPressed: _clearSettingsSearch,
            icon: const Icon(Icons.clear),
          )
        : null,
    border: const OutlineInputBorder(),
  );

  Widget _searchSettings(List<LitchSetting> settings, {required bool enabled}) {
    final hasQuery = _searchQuery.trim().isNotEmpty;
    final matches = settings
        .where(
          (setting) =>
              setting.key != 'BOARD_ROTATE' &&
              (setting.key != _timerKey || !_homeClockProjectionObserved) &&
              (!_isPieceWheelKey(setting.key) ||
                  setting.key == _p1PieceWheelKey ||
                  setting.key == _p2PieceWheelKey) &&
              (setting.key != 'VS_CPU_PLAYER' ||
                  (_settingByKey('VS_CPU_LEVEL') != null &&
                      _pieceColorValue(_settingByKey('VS_CPU_LEVEL')!) > 0)) &&
              hasQuery &&
              _matchesSearch(setting),
        )
        .toList();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        if (hasQuery && matches.isEmpty)
          Text(litchStrings(context).noSettingsMatch),
        for (final entry in _groupSettingsByCategory(
          matches,
        ).entries) ...<Widget>[
          SettingsSectionHeading(title: _friendlyCategoryName(entry.key)),
          const SizedBox(height: 8),
          for (final setting in entry.value)
            if (_isPieceWheelKey(setting.key))
              _buildPlayerColorControl(setting.key, enabled: enabled)
            else if (setting.key == _settleStyleKey)
              _buildQuickSettingControl(setting.key, enabled: enabled)
            else
              _buildSettingCard(setting, enabled: enabled),
          const SizedBox(height: 20),
        ],
      ],
    );
  }

  Widget _pageBody(
    _SetupPage page,
    List<LitchSetting> settings, {
    required bool enabled,
    required double previewSize,
    required bool splitOverview,
    Widget? overviewSupport,
  }) => switch (page) {
    _SetupPage.overview => _overview(
      enabled: enabled,
      previewSize: previewSize,
      split: splitOverview,
      support: overviewSupport,
    ),
    _SetupPage.lights => Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Card(
          margin: const EdgeInsets.only(bottom: 8),
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _buildQuickSettingControl('WHITE_SQUARES_ON', enabled: enabled),
                if ((_tryReadBoolSettingValue('WHITE_SQUARES_ON') ?? false) &&
                    _lightSquareAppearanceKeys.every(
                      (key) => _settingByKey(key) != null,
                    ))
                  _buildLightSquareAppearance(enabled: enabled),
              ],
            ),
          ),
        ),
        if (_settingByKey('MINIMAL_ANIMATIONS') case final setting?)
          _buildSettingCard(setting, enabled: enabled),
        if (_settingByKey(_settleStyleKey) != null)
          Card(
            margin: const EdgeInsets.only(bottom: 8),
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: _buildQuickSettingControl(
                _settleStyleKey,
                enabled: enabled,
              ),
            ),
          ),
        if (_settingByKey('TURN_ANIM_STYLE') case final setting?)
          _buildSettingCard(setting, enabled: enabled),
        if (_settingByKey('CHECKMATE_ANIM_STYLE') case final setting?)
          _buildSettingCard(setting, enabled: enabled),
        if (_settingByKey('HINT_THINKING_STYLE') case final setting?)
          _buildSettingCard(setting, enabled: enabled),
        if (_settingByKey('BOOT_DISPLAY') case final setting?)
          _buildSettingCard(setting, enabled: enabled),
        TextButton.icon(
          key: const ValueKey<String>('board-lights-manual'),
          style: TextButton.styleFrom(minimumSize: const Size(48, 48)),
          onPressed: () => Navigator.of(
            context,
          ).push(ManualScreen.route(initialTopic: 'lights')),
          icon: const Icon(Icons.help_outline),
          label: Text(litchStrings(context).boardLightsManualLink),
        ),
      ],
    ),
    _SetupPage.behavior => Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        _settingsList(
          settings.where((s) => _behaviorKeys.contains(s.key)).toList(),
          enabled: enabled,
          showCategories: false,
        ),
        _assistanceControls(enabled: enabled),
      ],
    ),
    _SetupPage.phone => AppPreferences(
      appearanceMode: widget.appearanceMode,
      onAppearanceModeChanged: widget.onAppearanceModeChanged,
      onNotice: (notice) => _notices.showNotice(context, notice),
    ),
    _SetupPage.advanced => _settingsList(
      settings.where(_advancedSetting).toList(),
      enabled: enabled,
    ),
    _SetupPage.developer => _buildDeveloperSettings(),
  };

  Widget _buildDeveloperSettings() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        Text(
          litchStrings(context).phonePreferencesHelp,
          style: Theme.of(context).textTheme.bodyLarge,
        ),
        const SizedBox(height: 16),
        Card(
          margin: EdgeInsets.zero,
          child: ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 48),
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: MergeSemantics(
                child: SettingsToggleRow(
                  title: Text(
                    litchStrings(context).showConsole,
                    style: Theme.of(context).textTheme.bodyLarge,
                  ),
                  help: Text(
                    litchStrings(context).showConsoleHelp,
                    style: Theme.of(context).textTheme.bodyMedium,
                  ),
                  control: Switch.adaptive(
                    key: const ValueKey<String>('enable-app-console'),
                    value: _appConsoleDraft ?? widget.enableAppConsole,
                    onChanged: (value) =>
                        unawaited(_setEnableAppConsole(value)),
                  ),
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }

  String? get _connectedBoardId {
    final LitchBleService service = widget.bleService;
    final String? deviceId = service.connectedId;
    if (!service.isConnected ||
        deviceId == null ||
        deviceId.isEmpty ||
        (widget.boardId.isNotEmpty && widget.boardId != deviceId)) {
      return null;
    }
    return deviceId;
  }

  RuntimeHealthResult? get _currentRuntimeHealth => _boardHealth != null
      ? RuntimeHealthAvailable(_boardHealth!.runtimeHealth)
      : _runtimeHealth;

  HomePersistenceDegradation? get _currentPersistence =>
      _connectedBoardId == null
      ? null
      : _boardHealth != null
      ? _boardHealth!.persistence
      : widget.bleService.latestHomeState?.persistenceDegradation;

  bool get _healthNeedsAttention => boardHealthNeedsAttention(
    _currentRuntimeHealth,
    persistence: _currentPersistence,
    sessionOnly: _boardHealth?.sessionOnly ?? false,
  );

  Widget _buildBoardHealth({bool active = true}) => Focus(
    key: active ? _boardHealthSectionKey : null,
    focusNode: active ? _boardHealthFocusNode : null,
    canRequestFocus: active,
    child: ConstrainedBox(
      constraints: BoxConstraints(minHeight: _heldBoardHealthHeight ?? 0),
      child: RuntimeHealthCard(
        result: _currentRuntimeHealth,
        recoveryOnly: _recoveryOnly,
        recoveryHealth: _recoverySupport,
        persistence: _currentPersistence,
        persistenceSessionOnly: _boardHealth?.sessionOnly ?? false,
        watchdogRestart: _boardHealth?.watchdogRestart ?? false,
        refreshing: _refreshingRuntimeHealth,
        onRetry: _refreshRuntimeHealth,
        supportsRecheck: _supportsRuntimeHealthRecheck,
        rechecking: _recheckingRuntimeHealth,
        onRecheck: _recheckRuntimeHealth,
        recheckMessage: _runtimeHealthRecheckMessage?.call(),
        storageRecovery: _hasSavingIncident || _storageReformatAttempted
            ? _visibleStorageRecovery
            : null,
        reformattingStorage: _reformattingStorage,
        checkingStorageRecovery: _checkingStorageRecovery,
        onReformatStorage:
            _storageConfirmationRoute == null && _connectedBoardId != null
            ? _confirmStorageReformat
            : null,
        onRefreshStorageRecovery: _connectedBoardId != null
            ? _refreshStorageRecovery
            : null,
      ),
    ),
  );

  Widget _buildBoardSupport({required bool active}) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: <Widget>[
      if (!_healthIsPrioritized) ...[
        _buildBoardHealth(active: active),
        const SizedBox(height: 12),
      ],
      Card(
        margin: EdgeInsets.zero,
        child: Column(
          children: <Widget>[
            ListTile(
              key: const ValueKey<String>('open-contact-support'),
              minTileHeight: 56,
              leading: const Icon(Icons.mail_outline),
              title: Text(litchStrings(context).contactSupport),
              subtitle: Text(litchStrings(context).supportReviewHelp),
              trailing: const Icon(Icons.chevron_right_rounded),
              onTap: _openSupportReport,
            ),
            const Divider(height: 20),
            ListTile(
              key: const ValueKey<String>('open-board-software'),
              minTileHeight: 56,
              leading: const Icon(Icons.system_update_outlined),
              title: Text(litchStrings(context).boardSoftware),
              trailing: const Icon(Icons.chevron_right_rounded),
              onTap: widget.onOpenFirmware,
            ),
          ],
        ),
      ),
    ],
  );

  @override
  Widget build(BuildContext context) {
    if (_sliderDraftValues.isEmpty) {
      _visibleStorageRecovery = _storageRecovery;
      _heldBoardHealthHeight = null;
    } else if (_heldBoardHealthHeight == null) {
      // A recovered health message can shrink before the card relocates.
      // Keep that space under the held finger while still showing live health.
      final healthBox = _boardHealthSectionKey.currentContext
          ?.findRenderObject();
      if (healthBox is RenderBox && healthBox.hasSize) {
        _heldBoardHealthHeight = healthBox.size.height;
      }
    }
    _healthIsPrioritized =
        _recoveryOnly ||
        (_sliderDraftValues.isNotEmpty
            ? _healthIsPrioritized
            : _healthNeedsAttention);
    final visibleSettings = _orderVisibleSettings(
      _settings.where((s) => !_isHiddenSetting(s)).toList(),
    );
    final boardId = _connectedBoardId;
    final hasSettings = boardId != null &&
        widget.bleService.settingsReadAuthority(boardId) ==
            LitchSettingsReadAuthority.confirmed;
    final enabled = hasSettings && !_isLoading;
    final active =
        !widget.deferInitialLoad && TickerMode.valuesOf(context).enabled;
    if (_recoveryOnly) {
      return _buildSetupPage(
        _SetupPage.overview,
        const <LitchSetting>[],
        settingsUnavailable: false,
        enabled: false,
      );
    }
    return NavigatorPopHandler<Object?>(
      enabled: active,
      onPopWithResult: (_) {
        if (active && _pageTrail.length > 1) _goBack();
      },
      child: Navigator(
        key: _setupNavigatorKey,
        pages: <Page<Object?>>[
          for (final page in _pageTrail)
            MaterialPage<Object?>(
              key: _pageKey(page),
              name: page.name,
              child: ColoredBox(
                color: Theme.of(context).scaffoldBackgroundColor,
                child: _buildSetupPage(
                  page,
                  visibleSettings,
                  settingsUnavailable: boardId != null && !_isLoading && !hasSettings,
                  enabled: enabled,
                ),
              ),
            ),
        ],
        onDidRemovePage: _didRemovePage,
      ),
    );
  }

  Widget _buildSetupPage(
    _SetupPage page,
    List<LitchSetting> visibleSettings, {
    required bool settingsUnavailable,
    required bool enabled,
  }) {
    final strings = litchStrings(context);
    return LayoutBuilder(
      builder: (context, constraints) {
        final tabletWindow = LitchWorkspaceGeometry.isTabletWindow(
          MediaQuery.sizeOf(context),
        );
        final inLandscape =
            MediaQuery.orientationOf(context) == Orientation.landscape;
        final landscape = constraints.maxHeight <= 480 && inLandscape;
        final overlayClearance = widget.landscapeOverlayClearance;
        final previewSize = (constraints.maxHeight - 8 - overlayClearance)
            .clamp(1.0, 280.0);
        final filtering =
            page == _SetupPage.overview && _searchQuery.trim().isNotEmpty;
        final loadingBoard =
            _isLoading &&
            _settingsLoadStarted &&
            _settings.isEmpty &&
            page != _SetupPage.phone &&
            page != _SetupPage.developer;
        final textScale = MediaQuery.textScalerOf(context).scale(16) / 16;
        final overviewBreakpoint = tabletWindow
            ? _landscapeOverviewBreakpoint * textScale.clamp(1.0, 2.0)
            : inLandscape
            ? _landscapeOverviewBreakpoint
            : _wideLayoutBreakpoint;
        final splitOverview =
            page == _SetupPage.overview &&
            !filtering &&
            !_recoveryOnly &&
            !loadingBoard &&
            textScale < 1.6 &&
            (constraints.maxWidth - 32).clamp(0.0, _maxContentWidth) >=
                overviewBreakpoint;
        final supportInColumn = tabletWindow && splitOverview;
        final support = page == _SetupPage.overview && !filtering
            ? _buildBoardSupport(active: _recoveryOnly || page == _page)
            : null;
        final contentWidth = tabletWindow && !splitOverview
            ? LitchWorkspaceGeometry.readableContentWidth
            : _maxContentWidth;
        final compactHeader =
            constraints.maxHeight <
            LitchCompactControls.toolbarHeightOf(context) +
                kMinInteractiveDimension +
                overlayClearance;
        final heading = LitchCompactControls(
          child: Row(
            children: [
              BackButton(
                key: const ValueKey<String>('setup-back'),
                onPressed: _goBack,
              ),
              Expanded(
                child: Semantics(
                  header: true,
                  child: Text(
                    _pageTitle(page),
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                ),
              ),
            ],
          ),
        );
        return Column(
          children: <Widget>[
            if (page != _SetupPage.overview && !compactHeader)
              Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 16,
                  vertical: 4,
                ),
                child: Center(
                  child: ConstrainedBox(
                    constraints: BoxConstraints(maxWidth: contentWidth),
                    child: heading,
                  ),
                ),
              ),
            Expanded(
              child: KeyedSubtree(
                key: const ValueKey<String>('settings-scroll'),
                // Reading positions belong to this board context, not a
                // transient route's default storage bucket.
                child: PageStorage(
                  bucket: _pageStorageBucket,
                  child: ListView(
                    // Results must not overwrite the retained page's reading position.
                    key: PageStorageKey<String>(
                      'setup:${widget.boardId}:${page.name}${filtering ? ':search' : ''}',
                    ),
                    controller: filtering
                        ? _searchResultsScrollController
                        : _pageScrollControllers.putIfAbsent(
                            page,
                            ScrollController.new,
                          ),
                    primary: false,
                    keyboardDismissBehavior:
                        ScrollViewKeyboardDismissBehavior.onDrag,
                    padding: EdgeInsets.fromLTRB(
                      16,
                      8,
                      16,
                      (landscape && constraints.maxHeight < 240 ? 8 : 28) +
                          overlayClearance,
                    ),
                    children: <Widget>[
                      Center(
                        child: SizedBox(
                          width: contentWidth,
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: <Widget>[
                              if (page != _SetupPage.overview &&
                                  compactHeader) ...[
                                heading,
                                const SizedBox(height: 8),
                              ],
                              if (_healthIsPrioritized &&
                                  page != _SetupPage.phone &&
                                  page != _SetupPage.developer) ...<Widget>[
                                KeyedSubtree(
                                  key: const ValueKey<String>(
                                    'board-health-priority',
                                  ),
                                  child: _buildBoardHealth(
                                    active: _recoveryOnly || page == _page,
                                  ),
                                ),
                                const SizedBox(height: 20),
                              ],
                              if (_connectedBoardId == null &&
                                  page != _SetupPage.phone &&
                                  page != _SetupPage.developer)
                                SettingsActionStatus(
                                  message: strings.boardSettingsConnect,
                                  icon: Icons.bluetooth_disabled_rounded,
                                ),
                              if (settingsUnavailable &&
                                  page != _SetupPage.phone &&
                                  page != _SetupPage.developer) ...<Widget>[
                                SettingsActionStatus(
                                  message: strings.boardSettingsUnavailable,
                                  icon: Icons.info_outline,
                                ),
                                const SizedBox(height: 12),
                              ],
                              if (!_recoveryOnly &&
                                  _orientationCorrectionActive &&
                                  page != _SetupPage.phone &&
                                  page != _SetupPage.developer) ...<Widget>[
                                SettingsActionStatus(
                                  key: const ValueKey<String>(
                                    'orientation-correction-guidance',
                                  ),
                                  message: strings.orientationCorrection,
                                  icon: Icons.swap_horiz_rounded,
                                  liveRegion: true,
                                ),
                                const SizedBox(height: 12),
                              ],
                              if (_recoveryOnly)
                                const SizedBox.shrink()
                              else if (loadingBoard)
                                const Padding(
                                  padding: EdgeInsets.symmetric(vertical: 32),
                                  child: Center(
                                    child: CircularProgressIndicator(),
                                  ),
                                )
                              else if (filtering)
                                _searchSettings(
                                  visibleSettings,
                                  enabled: enabled,
                                )
                              else
                                _pageBody(
                                  page,
                                  visibleSettings,
                                  enabled: enabled,
                                  previewSize: previewSize,
                                  splitOverview: splitOverview,
                                  overviewSupport: supportInColumn
                                      ? support
                                      : null,
                                ),
                              if (support != null && !supportInColumn)
                                Padding(
                                  padding: const EdgeInsets.only(top: 20),
                                  child: support,
                                ),
                              if (!_recoveryOnly && page == _SetupPage.overview)
                                Padding(
                                  padding: const EdgeInsets.only(top: 20),
                                  child: LitchReadingPane(
                                    child: KeyedSubtree(
                                      key: _searchFieldKey,
                                      child: _settingsSearchField(),
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class _SettingTitle extends StatelessWidget {
  const _SettingTitle({required this.title, required this.helpText});

  final String title;
  final String helpText;

  @override
  Widget build(BuildContext context) {
    final Color secondary = Theme.of(context).colorScheme.onSurfaceVariant;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        if (title.isNotEmpty)
          Text(
            title,
            style: Theme.of(
              context,
            ).textTheme.bodyLarge?.copyWith(fontWeight: FontWeight.w600),
          ),
        if (helpText.isNotEmpty) ...<Widget>[
          const SizedBox(height: 4),
          Text(
            helpText,
            style: Theme.of(
              context,
            ).textTheme.bodyMedium?.copyWith(color: secondary),
          ),
        ],
      ],
    );
  }
}

class _PlayerColorRow extends StatelessWidget {
  const _PlayerColorRow({
    super.key,
    required this.playerLabel,
    required this.spokenPlayerLabel,
    required this.colorName,
    required this.previewColor,
    required this.available,
    required this.enabled,
    required this.onTap,
  });

  final String playerLabel;
  final String spokenPlayerLabel;
  final String colorName;
  final Color previewColor;
  final bool available;
  final bool enabled;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final bool stack = MediaQuery.textScalerOf(context).scale(1) > 1.3;
    final String action = !available
        ? litchStrings(context).unavailableLabel
        : litchStrings(context).changeAction;
    final strings = litchStrings(context);
    final String semanticsLabel = strings.colorControlSemantics(
      spokenPlayerLabel,
      colorName,
      !available
          ? strings.colorUnavailableSemantics
          : strings.colorChangeSemantics,
    );
    final Widget identity = Row(
      crossAxisAlignment: CrossAxisAlignment.center,
      children: <Widget>[
        _PieceColorSwatch(color: previewColor),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(playerLabel, style: Theme.of(context).textTheme.labelLarge),
              const SizedBox(height: 2),
              Text(colorName, style: Theme.of(context).textTheme.bodyMedium),
            ],
          ),
        ),
      ],
    );

    return Semantics(
      button: enabled,
      enabled: enabled,
      label: semanticsLabel,
      onTap: onTap,
      excludeSemantics: true,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(12),
          child: ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 56),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 8),
              child: stack
                  ? Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: <Widget>[
                        identity,
                        const SizedBox(height: 8),
                        Align(
                          alignment: Alignment.centerRight,
                          child: Text(
                            action,
                            style: Theme.of(context).textTheme.labelLarge
                                ?.copyWith(
                                  color: enabled
                                      ? Theme.of(context).colorScheme.primary
                                      : Theme.of(context).disabledColor,
                                ),
                          ),
                        ),
                      ],
                    )
                  : Row(
                      children: <Widget>[
                        Expanded(child: identity),
                        const SizedBox(width: 12),
                        Text(
                          action,
                          style: Theme.of(context).textTheme.labelLarge
                              ?.copyWith(
                                color: enabled
                                    ? Theme.of(context).colorScheme.primary
                                    : Theme.of(context).disabledColor,
                              ),
                        ),
                      ],
                    ),
            ),
          ),
        ),
      ),
    );
  }
}

class _PieceColorChoiceTile extends StatelessWidget {
  const _PieceColorChoiceTile({
    super.key,
    required this.choice,
    required this.selected,
    required this.available,
    required this.unavailableReason,
    required this.onTap,
  });

  final LitchPieceColorChoice choice;
  final bool selected;
  final bool available;
  final String unavailableReason;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final Color preview = litchWheelPreviewColor(choice.wheel);
    final String state = !available
        ? litchStrings(context).colorUnavailableSuffix(unavailableReason)
        : '';
    return Semantics(
      button: true,
      inMutuallyExclusiveGroup: true,
      selected: selected,
      enabled: available,
      label:
          '${localizedPieceColor(litchStrings(context), choice.wheel)}$state',
      onTap: onTap,
      excludeSemantics: true,
      child: Material(
        color: selected
            ? Theme.of(context).colorScheme.primaryContainer
            : Theme.of(context).colorScheme.surfaceContainerLow,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(12),
          side: BorderSide(
            color: selected
                ? Theme.of(context).colorScheme.primary
                : Theme.of(context).colorScheme.outlineVariant,
            width: selected ? 2 : 1,
          ),
        ),
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(12),
          child: ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 56),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              child: Row(
                children: <Widget>[
                  _PieceColorSwatch(
                    color: preview,
                    selected: selected,
                    enabled: available,
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: <Widget>[
                        Text(
                          localizedPieceColor(
                            litchStrings(context),
                            choice.wheel,
                          ),
                        ),
                        if (!available)
                          Text(
                            unavailableReason,
                            style: Theme.of(context).textTheme.bodySmall
                                ?.copyWith(
                                  color: Theme.of(
                                    context,
                                  ).colorScheme.onSurfaceVariant,
                                ),
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _PieceColorSwatch extends StatelessWidget {
  const _PieceColorSwatch({
    required this.color,
    this.selected = false,
    this.enabled = true,
  });

  final Color color;
  final bool selected;
  final bool enabled;

  @override
  Widget build(BuildContext context) {
    final Color checkColor = litchSwatchMarkColor(color);
    return Opacity(
      opacity: enabled ? 1 : 0.38,
      child: Container(
        key: ValueKey<String>('piece-color-swatch:${color.toARGB32()}'),
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: color,
          shape: BoxShape.circle,
          border: Border.all(
            color: Theme.of(context).colorScheme.outline,
            width: 2,
          ),
        ),
        alignment: Alignment.center,
        child: selected
            ? Icon(Icons.check_rounded, color: checkColor, size: 24)
            : null,
      ),
    );
  }
}
