import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:file_selector/file_selector.dart' show openFile, XTypeGroup;
import 'package:flutter/foundation.dart' show compute;
import 'package:flutter/gestures.dart' show kLongPressTimeout;
import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/semantics.dart';
import 'package:flutter/services.dart';
import 'package:share_plus/share_plus.dart';
import 'package:path_provider/path_provider.dart';
import 'package:sensors_plus/sensors_plus.dart';

import '../../l10n/generated/app_localizations.dart';
import '../controllers/litchcraft_controller.dart';
import '../controllers/litchcraft_library.dart';
import '../localization/litch_strings.dart';
import '../models/litch_piece_color.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_renderer.dart';
import '../models/litchcraft_artwork.dart';
import '../models/litchcraft_document_session.dart';
import '../models/litchcraft_state.dart';
import '../models/user_text.dart';
import '../services/litchcraft_phone_shake.dart';
import '../services/litchcraft_artwork_store.dart';
import '../services/litchcraft_media.dart';
import '../services/device_landscape_side.dart';
import '../widgets/orientation_workspace.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/app_preferences.dart';
import '../widgets/litchcraft_canvas.dart';
import '../widgets/litchcraft_motion_editor.dart';
import '../widgets/litchcraft_media_sheet.dart';
import '../widgets/litchcraft_tools.dart';

typedef _ArtworkSelection = ({
  String action,
  LitchcraftArtwork? art,
  LitchcraftOpenSource source,
});

class _QuickActionWell extends StatefulWidget {
  const _QuickActionWell({
    super.key,
    required this.child,
    required this.enabled,
    this.selected = false,
    this.size = 72,
    this.visualSize,
    this.baseColor,
  });
  final Widget child;
  final bool enabled;
  final bool selected;
  final double size;
  final double? visualSize;
  final Color? baseColor;

  @override
  State<_QuickActionWell> createState() => _QuickActionWellState();
}

class _QuickActionWellState extends State<_QuickActionWell> {
  bool _pressed = false;

  void _setPressed(bool pressed) {
    if (_pressed != pressed) setState(() => _pressed = pressed);
  }

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: widget.size,
    child: Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: widget.enabled ? (_) => _setPressed(true) : null,
      onPointerUp: (_) => _setPressed(false),
      onPointerCancel: (_) => _setPressed(false),
      onPointerMove: (event) {
        final box = context.findRenderObject()! as RenderBox;
        if (!(Offset.zero & box.size).contains(
          box.globalToLocal(event.position),
        )) {
          _setPressed(false);
        }
      },
      child: Center(
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 100),
          width: widget.visualSize ?? widget.size,
          height: widget.visualSize ?? widget.size,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            color: _pressed
                ? const Color(0xffc6e5e4)
                : widget.baseColor ?? Colors.transparent,
          ),
          child: Center(
            child: IconTheme(
              data: IconThemeData(
                color: _pressed ? const Color(0xff263c40) : Colors.white,
              ),
              child: Opacity(
                opacity: widget.enabled ? 1 : .4,
                child: widget.child,
              ),
            ),
          ),
        ),
      ),
    ),
  );
}

class _PreviousColorEyedropperGlyph extends StatelessWidget {
  const _PreviousColorEyedropperGlyph({
    required this.currentColor,
    required this.previousColor,
    this.size = 46,
  });

  final int currentColor;
  final int? previousColor;
  final double size;

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: size,
    child: Stack(
      clipBehavior: Clip.none,
      children: size >= 60
          ? [
              Positioned(
                left: 0,
                top: 8,
                child: _colorDot(
                  previousColor ?? currentColor,
                  25,
                  current: false,
                ),
              ),
              Positioned(
                left: 8,
                bottom: 0,
                child: _colorDot(currentColor, 34, current: true),
              ),
              const Positioned(
                right: 0,
                top: 0,
                child: Icon(
                  Icons.colorize_outlined,
                  key: ValueKey('craft-quick-eyedropper'),
                  size: 25.6,
                ),
              ),
              const Positioned(
                right: 0,
                bottom: 3,
                child: Icon(
                  Icons.swap_horiz_rounded,
                  key: ValueKey('craft-quick-swap-indicator'),
                  size: 18,
                ),
              ),
            ]
          : [
              Positioned(
                right: -2,
                top: -4,
                child: Icon(
                  Icons.colorize_outlined,
                  key: const ValueKey('craft-quick-eyedropper'),
                  size: size * .78,
                  color: IconTheme.of(context).color,
                ),
              ),
              Positioned(
                left: -2,
                bottom: -1,
                child: Icon(
                  Icons.swap_horiz_rounded,
                  key: const ValueKey('craft-quick-swap-indicator'),
                  size: size * .43,
                  color: IconTheme.of(context).color,
                ),
              ),
              Positioned(
                left: 1,
                top: 1,
                child: _colorDot(
                  previousColor ?? currentColor,
                  size * .52,
                  current: false,
                ),
              ),
              Positioned(
                right: 0,
                bottom: 0,
                child: _colorDot(currentColor, size * .68, current: true),
              ),
            ],
    ),
  );

  Widget _colorDot(int rgb, double size, {required bool current}) => Container(
    key: ValueKey(
      current ? 'craft-current-color-dot' : 'craft-previous-color-dot',
    ),
    width: size,
    height: size,
    decoration: BoxDecoration(
      color: litchcraftPreviewColor(rgb),
      shape: BoxShape.circle,
      border: Border.all(color: Colors.white, width: current ? 3 : 2),
      boxShadow: [
        BoxShadow(
          color: const Color(0x66000000),
          blurRadius: current ? 4 : 2,
          spreadRadius: current ? 1 : 0,
        ),
      ],
    ),
  );
}

class LitchcraftScreen extends StatefulWidget {
  const LitchcraftScreen({
    super.key,
    required this.controller,
    required this.library,
    this.landscapeHeader,
    this.landscapeFooter,
    this.landscapeFooterHeight = 48,
    this.portraitBoardStatus,
    this.phoneMotionSamples,
    this.monotonicMilliseconds,
    this.phoneShakeAction,
    this.landscapeRibbonSide,
    this.deviceLandscapeSideReader,
    this.rootSafeInsets,
    this.active = true,
    this.artworkFilePicker,
  });
  final LitchcraftController controller;
  final LitchcraftLibrary library;
  final Future<XFile?> Function()? artworkFilePicker;
  final Widget? landscapeHeader;
  final Widget? landscapeFooter;
  final double landscapeFooterHeight;

  /// Shell-owned connection/status access, shown once in the portrait footer.
  final Widget? portraitBoardStatus;

  /// Mode admission and save progress owned by the navigation shell.
  final Stream<LitchcraftMotionSample>? phoneMotionSamples;
  final int Function()? monotonicMilliseconds;
  final Future<void> Function()? phoneShakeAction;

  /// Optional deterministic override. Production reads the native interface
  /// orientation because window aspect ratio cannot identify a landscape side.
  final Alignment? landscapeRibbonSide;
  final Future<DeviceLandscapeSide?> Function()? deviceLandscapeSideReader;

  /// Root-window insets for menus rendered in the root overlay. Shells that
  /// consume local SafeArea padding must preserve the original value here.
  final EdgeInsets? rootSafeInsets;

  /// Whether this retained screen is the user's visible app destination.
  /// Controller reconciliation continues while hidden, but modal Litchcraft UI
  /// must never escape onto Play, Setup, or Console.
  final bool active;
  @override
  State<LitchcraftScreen> createState() => _LitchcraftScreenState();
}

class _LitchcraftScreenState extends State<LitchcraftScreen>
    with WidgetsBindingObserver, TickerProviderStateMixin {
  bool _working = false;
  bool _readingBoardArtwork = false;
  bool _boardArtworkReadFailed = false;
  final Set<String> _localActions = <String>{};
  bool _artworkContext = false;
  final _tabletJumpAvailable = ValueNotifier<bool>(false);
  final _tabletBrushKey = GlobalKey();
  final _tabletFramesKey = GlobalKey();
  final _tabletLedsKey = GlobalKey();
  final _tabletEffectsKey = GlobalKey();
  bool _tabletDrawerOpen = false;
  LitchcraftSecondaryTray _tabletPortraitSection =
      LitchcraftSecondaryTray.palette;
  bool _newArtworkPending = false;
  bool _sharing = false;
  bool _importing = false;
  bool _pickingColor = false;
  bool _hexPadVisible = false;
  String? _toolStatus;
  bool _toolStatusShowsUndo = false;
  Timer? _toolStatusTimer;
  String? _frameTimingStatus;
  bool _frameTimingStatusAnnounces = false;
  Timer? _frameTimingStatusTimer;
  Timer? _idleTipDelayTimer;
  Timer? _idleTipVisibleTimer;
  int _idleTipIndex = 0;
  bool _idleTipVisible = false;
  bool _tipsEnabled = true;
  Object? _lastTipActivity;
  Timer? _physicalPickInstructionTimer;
  Object? _physicalPickInstructionBinding;
  bool _physicalPickInstructionVisible = false;
  int _activePointers = 0;
  bool _conflictSheetVisible = false;
  bool _reopenConflictSheet = false;
  bool _gameEntryDialogVisible = false;
  BuildContext? _gameEntryDialogContext;
  late final Ticker _previewTicker;
  final ValueNotifier<bool> _localPreviewPlaying = ValueNotifier<bool>(false);
  Timer? _authoritativePreviewTimer;
  late final int _effectStartedAtMs;
  int get _effectClockMs =>
      widget.monotonicMilliseconds?.call() ??
      _phoneShakeClock.elapsedMilliseconds;
  int get _effectElapsedMs => _effectClockMs - _effectStartedAtMs;
  bool _occupiedBoardNoticeShown = false;
  int _previewElapsedMs = 0, _previewStartElapsedMs = 0;
  LitchcraftEffectStyle? _effectTool;
  int _effectDurationMs = litchcraftAnimationDefaultEffectDurationMs;
  BuildContext? _conflictSheetContext;
  LitchcraftSecondaryTray _activeSecondaryTray =
      LitchcraftSecondaryTray.palette;
  bool _deleteFrameDialogVisible = false;
  BuildContext? _deleteFrameDialogContext;
  LitchcraftFrameEditTarget? _frameDurationTarget;
  late bool _animationWasEditable;
  bool _wasBoardAuthoritative = false;
  late bool _wasFlipped;
  late final AnimationController _presentationFlip = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 220),
    value: 1,
  );
  final GlobalKey _canvasKey = GlobalKey();
  final ValueNotifier<int> _gestureCancellation = ValueNotifier<int>(0);
  final LitchcraftPhoneShakeDetector _phoneShakeDetector =
      LitchcraftPhoneShakeDetector();
  final Stopwatch _phoneShakeClock = Stopwatch()..start();
  StreamSubscription<LitchcraftMotionSample>? _phoneMotionSubscription;
  int _phoneMotionGeneration = 0;
  DeviceLandscapeSide _deviceLandscapeSide = DeviceLandscapeSide.right;
  bool? _standaloneTabletToolsOnLeft;
  int _deviceLandscapeSideGeneration = 0;
  bool _appResumed = true;
  late LitchcraftStatus _lastStatus;
  LitchcraftController get controller => widget.controller;
  LitchcraftLibrary get library => widget.library;
  bool get _compactPortraitAccessibility {
    final media = MediaQuery.of(context);
    return media.size.width <= 360 &&
        media.size.height <= 640 &&
        media.textScaler.scale(10) >= 18;
  }

  bool get _canDirectlyEditAnimation =>
      !controller.boardArtworkUnresolved &&
      (library.canEditAnimation || controller.animationBoardAuthorityLive);

  bool get _usesBoardPlayback =>
      !controller.boardArtworkUnresolved &&
      controller.status != LitchcraftStatus.conflict &&
      controller.status != LitchcraftStatus.unsupported &&
      !controller.storeArtworkUnavailable &&
      (controller.animationBoardAuthorityLive ||
          (controller.usesFrameStoreDocument &&
              controller.boardActive &&
              controller.bleService.isConnected &&
              controller.storePlaybackFollowsLocal));

  bool get _animationPlaying => _usesBoardPlayback
      ? controller.animationPlaying
      : _previewTicker.isActive;

  bool get _tightPortraitLayout {
    final media = MediaQuery.of(context);
    final safeInsets = widget.rootSafeInsets ?? media.viewPadding;
    // The nominal board + fixed controls fit by reclaiming decoration before
    // reducing touch targets or the full-width canvas on notched phones.
    return media.size.height > media.size.width &&
        media.size.height - safeInsets.vertical <
            media.size.width - safeInsets.horizontal + 390;
  }

  @override
  void initState() {
    super.initState();
    _effectStartedAtMs = _effectClockMs;
    _previewTicker = createTicker((elapsed) {
      final next = _previewStartElapsedMs + elapsed.inMilliseconds;
      if (next - _previewElapsedMs < 50) return;
      setState(() => _previewElapsedMs = next);
    });
    WidgetsBinding.instance.addObserver(this);
    controller.addListener(_handleControllerStatus);
    _lastStatus = controller.status;
    _animationWasEditable = _canDirectlyEditAnimation;
    _wasBoardAuthoritative = _usesBoardPlayback;
    _wasFlipped = controller.flipped;
    if (!_wasBoardAuthoritative) _seekPreviewToSelectedFrame(notify: false);
    _syncAuthoritativePreview();
    _syncPhoneMotion();
    _syncPhysicalPickInstruction();
    _scheduleDeviceLandscapeSideRead();
    _showBoardNotEmptyToastAfterFrame();
    _syncGameEntryChoice();
  }

  void _syncGameEntryChoice() {
    final owner = controller;
    final choice = owner.gameEntryChoice;
    if (!widget.active || choice == null) {
      final dialogContext = _gameEntryDialogContext;
      if (dialogContext != null) {
        _gameEntryDialogContext = null;
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (dialogContext.mounted &&
              ModalRoute.of(dialogContext)?.isCurrent == true) {
            Navigator.of(dialogContext).pop();
          }
        });
      }
      return;
    }
    if (_gameEntryDialogVisible) return;
    _gameEntryDialogVisible = true;
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      try {
        if (!mounted ||
            !widget.active ||
            !identical(owner, controller) ||
            !identical(choice, owner.gameEntryChoice)) {
          return;
        }
        final strings = litchStrings(context);
        final discard = await showDialog<bool>(
          context: context,
          routeSettings: const RouteSettings(
            name: 'litchcraft-game-entry-choice',
          ),
          builder: (dialogContext) {
            _gameEntryDialogContext = dialogContext;
            return AlertDialog(
              key: const ValueKey('craft-game-entry-choice'),
              title: Text(strings.craftGameEntryQuestion),
              content: SingleChildScrollView(
                child: Text(strings.craftGameEntryBody),
              ),
              actions: [
                TextButton(
                  key: const ValueKey('craft-game-entry-keep'),
                  onPressed: () => Navigator.of(dialogContext).pop(false),
                  child: Text(strings.craftKeepGameUseApp),
                ),
                FilledButton(
                  key: const ValueKey('craft-game-entry-discard'),
                  onPressed: () => Navigator.of(dialogContext).pop(true),
                  child: Text(strings.craftDiscardGameUseBoard),
                ),
              ],
            );
          },
        );
        _gameEntryDialogContext = null;
        if (!mounted ||
            !widget.active ||
            !identical(owner, controller) ||
            !identical(choice, owner.gameEntryChoice)) {
          return;
        }
        if (discard != true) {
          owner.keepGameForLocalArtwork(choice);
          return;
        }
        await owner.discardGameAndEnter(choice);
      } finally {
        _gameEntryDialogContext = null;
        _gameEntryDialogVisible = false;
        if (mounted &&
            widget.active &&
            controller.gameEntryChoice != null &&
            !identical(choice, controller.gameEntryChoice)) {
          _syncGameEntryChoice();
        }
      }
    });
  }

  void _showBoardNotEmptyToastAfterFrame() {
    if (!widget.active ||
        controller.status != LitchcraftStatus.boardNotEmpty ||
        _occupiedBoardNoticeShown) {
      return;
    }
    _occupiedBoardNoticeShown = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted &&
          widget.active &&
          controller.status == LitchcraftStatus.boardNotEmpty) {
        _showToolStatus(_connectionText(controller.status));
      }
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final enabled = AppPreferencesScope.maybeOf(context)?.showTips ?? true;
    if (_tipsEnabled == enabled && _idleTipDelayTimer != null) return;
    _tipsEnabled = enabled;
    _idleTipDelayTimer?.cancel();
    _idleTipVisibleTimer?.cancel();
    if (_previewTicker.isActive) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) _stopPreview();
      });
    } else {
      _stopPreview(notify: false);
    }
    if (_idleTipVisible) _idleTipVisible = false;
    if (enabled) _scheduleIdleTip();
  }

  @override
  void didUpdateWidget(covariant LitchcraftScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.landscapeRibbonSide != widget.landscapeRibbonSide) {
      didChangeMetrics();
    }
    if (oldWidget.deviceLandscapeSideReader !=
        widget.deviceLandscapeSideReader) {
      _scheduleDeviceLandscapeSideRead();
    }
    if (oldWidget.controller != controller) {
      _retireConflictSheet(owner: oldWidget.controller);
      oldWidget.controller.removeListener(_handleControllerStatus);
      controller.addListener(_handleControllerStatus);
      _lastStatus = controller.status;
      _animationWasEditable = _canDirectlyEditAnimation;
      _wasFlipped = controller.flipped;
      _syncAuthoritativePreview();
      _syncPhoneMotion();
      _lastTipActivity = null;
      _clearIdleTip();
      _syncPhysicalPickInstruction();
    }
    if (oldWidget.active && !widget.active) {
      _retireConflictSheet();
    } else if (!oldWidget.active && widget.active) {
      _occupiedBoardNoticeShown = false;
      _showBoardNotEmptyToastAfterFrame();
    }
    _syncGameEntryChoice();
    if (oldWidget.active != widget.active) {
      _syncPhoneMotion();
      _syncAuthoritativePreview();
      _syncPhysicalPickInstruction();
      _clearIdleTip();
      _scheduleIdleTip();
    }
  }

  void _handleControllerStatus() {
    _syncGameEntryChoice();
    if (controller.status != LitchcraftStatus.conflict &&
        _conflictSheetVisible) {
      _retireConflictSheet(
        reopen: widget.active && controller.boardActive && controller.selected,
      );
    }
    if (controller.status == LitchcraftStatus.offline ||
        (controller.status == LitchcraftStatus.live &&
            !controller.boardArtworkUnresolved)) {
      _reopenConflictSheet = false;
    }
    _maybeReopenConflictSheet();
    if (controller.status == LitchcraftStatus.live) {
      _occupiedBoardNoticeShown = false;
    }
    final statusChanged = _lastStatus != controller.status;
    _lastStatus = controller.status;
    if (statusChanged) _showBoardNotEmptyToastAfterFrame();
    if (_wasFlipped != controller.flipped) {
      _wasFlipped = controller.flipped;
      if (MediaQuery.disableAnimationsOf(context)) {
        _presentationFlip.value = 1;
      } else {
        _presentationFlip.forward(from: 0);
      }
    }
    final boardAuthority = _usesBoardPlayback;
    final lostBoardAuthority = _wasBoardAuthoritative && !boardAuthority;
    if (lostBoardAuthority) {
      _seekPreviewToSelectedFrame(notify: false);
    }
    _wasBoardAuthoritative = boardAuthority;
    _syncAuthoritativePreview();
    _syncPhysicalPickInstruction();
    final activity = (
      controller.revision,
      controller.documentSession.generation,
      controller.status,
      _artworkSyncActive,
      controller.physicalColorPicking,
    );
    if (_lastTipActivity != activity) {
      _lastTipActivity = activity;
      _clearIdleTip();
    }
    if (_animationPlaying ||
        _artworkSyncActive ||
        controller.physicalColorPicking) {
      _clearIdleTip();
    } else if (_idleTipDelayTimer == null && !_idleTipVisible) {
      _scheduleIdleTip();
    }
    if (_pickingColor &&
        (controller.physicalColorPicking ||
            controller.physicalColorPicked ||
            controller.physicalShakeErased) &&
        mounted) {
      setState(() => _pickingColor = false);
    }
    final animationEditable = _canDirectlyEditAnimation;
    final becameUnavailable = _animationWasEditable && !animationEditable;
    _animationWasEditable = animationEditable;
    // A confirmation route may outlive the notification that first removed
    // edit authority (for example while its entrance transition is running).
    // Enforce the current authority invariant on every status notification;
    // do not rely solely on the cached true -> false edge.
    final deleteContext = _deleteFrameDialogContext;
    if (!animationEditable &&
        _deleteFrameDialogVisible &&
        deleteContext?.mounted == true) {
      Navigator.of(deleteContext!).pop(false);
    }
    if (becameUnavailable || lostBoardAuthority) {
      _stopPreview();
      if (_effectTool != null && mounted) {
        setState(() => _effectTool = null);
      }
    }
    _syncPhoneMotion();
  }

  void _retireConflictSheet({
    bool reopen = false,
    LitchcraftController? owner,
  }) {
    _reopenConflictSheet = reopen;
    final sheetContext = _conflictSheetContext;
    final route = sheetContext?.mounted == true
        ? ModalRoute.of(sheetContext!)
        : null;
    if (route == null) return;
    _conflictSheetContext = null;
    (owner ?? controller).retireConflictBoardPreview();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final navigator = route.navigator;
      if (navigator != null && route.isActive) navigator.removeRoute(route);
    });
  }

  void _maybeReopenConflictSheet() {
    if (!mounted || !widget.active) return;
    final pageCurrent = ModalRoute.isCurrentOf(context) ?? true;
    if (!_reopenConflictSheet ||
        _conflictSheetVisible ||
        !pageCurrent ||
        controller.status != LitchcraftStatus.conflict ||
        controller.boardArtworkUnresolved) {
      return;
    }
    _reopenConflictSheet = false;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !widget.active || _conflictSheetVisible) return;
      if (controller.status == LitchcraftStatus.conflict &&
          !controller.boardArtworkUnresolved) {
        if (ModalRoute.isCurrentOf(context) ?? true) {
          unawaited(_showConflict());
        } else {
          _reopenConflictSheet = true;
        }
      }
    });
  }

  Stream<LitchcraftMotionSample> _defaultPhoneMotionSamples() async* {
    try {
      // Probe the plugin before subscribing: sensors_plus intentionally does
      // not await its sampling-period call, so a missing implementation would
      // otherwise escape the stream's onError handler.
      await const MethodChannel(
        'dev.fluttercommunity.plus/sensors/method',
      ).invokeMethod<void>(
        'setUserAccelerometerSamplingPeriod',
        SensorInterval.gameInterval.inMicroseconds,
      );
      await for (final event in userAccelerometerEventStream(
        samplingPeriod: SensorInterval.gameInterval,
      )) {
        yield LitchcraftMotionSample(event.x, event.y, event.z);
      }
    } catch (_) {
      // Motion is optional; unsupported hardware/platforms fail soft.
    }
  }

  void _syncPhoneMotion() {
    final eligible =
        widget.active &&
        _appResumed &&
        controller.selected &&
        !controller.boardArtworkUnresolved &&
        controller.shakeEraseSupported;
    if (!eligible) {
      _phoneMotionGeneration++;
      unawaited(_phoneMotionSubscription?.cancel() ?? Future<void>.value());
      _phoneMotionSubscription = null;
      _phoneShakeDetector.reset();
      return;
    }
    if (_phoneMotionSubscription != null) return;
    final generation = ++_phoneMotionGeneration;
    final samples = widget.phoneMotionSamples ?? _defaultPhoneMotionSamples();
    _phoneMotionSubscription = samples.listen(
      (sample) {
        if (generation != _phoneMotionGeneration) return;
        final trigger = _phoneShakeDetector.observe(
          x: sample.x,
          y: sample.y,
          z: sample.z,
          monotonicMs:
              widget.monotonicMilliseconds?.call() ??
              _phoneShakeClock.elapsedMilliseconds,
          eligible: _phoneShakeInteractionEligible,
        );
        if (trigger) {
          unawaited(
            widget.phoneShakeAction?.call() ??
                controller.eraseDisplayedFrameByPhone(),
          );
        }
      },
      onError: (_) {
        if (generation != _phoneMotionGeneration) return;
        _phoneShakeDetector.reset();
        _phoneMotionSubscription = null;
      },
      cancelOnError: true,
    );
  }

  bool get _phoneShakeInteractionEligible =>
      mounted &&
      widget.active &&
      _appResumed &&
      controller.selected &&
      !controller.boardArtworkUnresolved &&
      controller.shakeEraseSupported &&
      controller.status == LitchcraftStatus.live &&
      _activePointers == 0 &&
      !_pickingColor &&
      !_hexPadVisible &&
      !controller.physicalColorPicking &&
      !_conflictSheetVisible &&
      (ModalRoute.of(context)?.isCurrent ?? true);

  void _cancelPhoneShakeQualification() => _phoneShakeDetector.reset();

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _appResumed = state == AppLifecycleState.resumed;
    _syncPhoneMotion();
    _syncAuthoritativePreview();
    setState(() {
      _syncPhysicalPickInstruction();
      _clearIdleTip();
      _scheduleIdleTip();
    });
    if (_appResumed) _scheduleDeviceLandscapeSideRead();
  }

  @override
  void dispose() {
    _deviceLandscapeSideGeneration++;
    _tabletJumpAvailable.dispose();
    WidgetsBinding.instance.removeObserver(this);
    controller.removeListener(_handleControllerStatus);
    _toolStatusTimer?.cancel();
    _frameTimingStatusTimer?.cancel();
    _idleTipDelayTimer?.cancel();
    _idleTipVisibleTimer?.cancel();
    _physicalPickInstructionTimer?.cancel();
    _previewTicker.dispose();
    _presentationFlip.dispose();
    _localPreviewPlaying.dispose();
    _authoritativePreviewTimer?.cancel();
    unawaited(_phoneMotionSubscription?.cancel() ?? Future<void>.value());
    _gestureCancellation.dispose();
    super.dispose();
  }

  void _clearIdleTip() {
    _idleTipDelayTimer?.cancel();
    _idleTipDelayTimer = null;
    _idleTipVisibleTimer?.cancel();
    _idleTipVisibleTimer = null;
    _idleTipVisible = false;
  }

  bool get _storeOpenRecoveryPending =>
      !library.saveFailed && controller.storeReplacementRecoveryPending;

  bool get _artworkSyncActive =>
      controller.preparingBoardArtwork ||
      (controller.authorityResolving && controller.boardActive) ||
      _storeOpenRecoveryPending ||
      (controller.status == LitchcraftStatus.connecting &&
          controller.usesFrameStoreDocument) ||
      controller.fullAnimationSyncDirection != null ||
      controller.storeRootPublicationPending ||
      _readingBoardArtwork ||
      controller.boardArtworkLoading ||
      controller.artworkReadbackPending;

  bool get _showPhysicalPickInstruction =>
      _physicalPickInstructionVisible && controller.physicalColorPicking;

  void _syncPhysicalPickInstruction() {
    final binding =
        widget.active &&
            _appResumed &&
            controller.status == LitchcraftStatus.live &&
            controller.physicalColorPicking
        ? (
            controller,
            controller.documentSession.replacementGeneration,
            controller.bleService.connectionSessionToken,
            controller.sessionId,
          )
        : null;
    if (_physicalPickInstructionBinding == binding) return;
    _physicalPickInstructionBinding = binding;
    _physicalPickInstructionTimer?.cancel();
    _physicalPickInstructionTimer = null;
    _physicalPickInstructionVisible = false;
    if (binding == null) return;
    // Phase one starts on initial Hall contact. Match the board's one-second
    // ordinary-paint qualification before showing optional pick instructions;
    // raw physicalColorPicking still owns every input/safety guard immediately.
    _physicalPickInstructionTimer = Timer(const Duration(seconds: 1), () {
      _physicalPickInstructionTimer = null;
      if (!mounted || _physicalPickInstructionBinding != binding) return;
      setState(() => _physicalPickInstructionVisible = true);
    });
  }

  void _scheduleIdleTip() {
    _idleTipDelayTimer?.cancel();
    if (!_tipsEnabled ||
        !widget.active ||
        !_appResumed ||
        _artworkSyncActive ||
        controller.physicalColorPicking ||
        _animationPlaying ||
        _activePointers != 0 ||
        _toolStatus != null) {
      return;
    }
    _idleTipDelayTimer = Timer(const Duration(seconds: 10), () {
      _idleTipDelayTimer = null;
      if (!mounted ||
          !_tipsEnabled ||
          _animationPlaying ||
          !widget.active ||
          !_appResumed ||
          _artworkSyncActive ||
          controller.physicalColorPicking) {
        return;
      }
      setState(() => _idleTipVisible = true);
      _idleTipVisibleTimer?.cancel();
      _idleTipVisibleTimer = Timer(const Duration(seconds: 4), () {
        _idleTipVisibleTimer = null;
        if (!mounted) return;
        setState(() {
          _idleTipVisible = false;
          _idleTipIndex = (_idleTipIndex + 1) % _idleTips().length;
        });
        _scheduleIdleTip();
      });
    });
  }

  void _beginInteraction(PointerDownEvent _) {
    if (!mounted) return;
    _cancelPhoneShakeQualification();
    _activePointers++;
    _toolStatusTimer?.cancel();
    if (_toolStatus != null) {
      setState(() {
        _toolStatus = null;
        _toolStatusShowsUndo = false;
      });
    }
    _idleTipDelayTimer?.cancel();
    _idleTipVisibleTimer?.cancel();
    if (_idleTipVisible) setState(() => _idleTipVisible = false);
  }

  void _endInteraction(PointerEvent _) {
    if (!mounted) return;
    if (_activePointers > 0) _activePointers--;
    if (_activePointers == 0) {
      _scheduleIdleTip();
    }
  }

  void _recordCommandInteraction() {
    _idleTipDelayTimer?.cancel();
    _idleTipVisibleTimer?.cancel();
    if (_idleTipVisible) setState(() => _idleTipVisible = false);
    _scheduleIdleTip();
  }

  @override
  void didChangeMetrics() {
    _finishCanvasGestureForReflow();
    _scheduleDeviceLandscapeSideRead();
  }

  void _finishCanvasGestureForReflow() {
    // A rotation can move the canvas between layout branches while a pointer is
    // down. Finish that stroke once before reflow so undo and BLE framing stay
    // coherent even if the platform later delivers a cancel or up event.
    controller.endStroke();
    library.endCanvasGesture();
    _gestureCancellation.value++;
  }

  void _scheduleDeviceLandscapeSideRead() {
    if (widget.landscapeRibbonSide != null) return;
    final generation = ++_deviceLandscapeSideGeneration;
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      final side =
          await (widget.deviceLandscapeSideReader?.call() ??
              const DeviceLandscapeSideReader().read());
      if (!mounted ||
          generation != _deviceLandscapeSideGeneration ||
          side == null) {
        return;
      }
      if (_deviceLandscapeSide == side) return;
      setState(() => _deviceLandscapeSide = side);
    });
  }

  Future<void> _run(Future<void> Function() action) async {
    if (_working) return;
    _recordCommandInteraction();
    setState(() => _working = true);
    try {
      await action();
    } catch (error) {
      if (mounted) {
        if (_readingBoardArtwork) {
          setState(() => _boardArtworkReadFailed = true);
        }
        _showToolStatus(
          _readingBoardArtwork
              ? litchStrings(context).craftReadBoardFailed
              : error is LitchcraftStorageLimitExceeded
              ? litchStrings(context).craftLibraryFull
              : litchStrings(context).craftActionFailed,
        );
      }
    } finally {
      if (mounted) setState(() => _working = false);
    }
  }

  void _runInBackground(
    Future<void> Function() action, {
    bool Function(Object error)? isRecoveringFailure,
  }) {
    _recordCommandInteraction();
    unawaited(
      action().catchError((Object error) {
        if (!mounted || isRecoveringFailure?.call(error) == true) return;
        _showToolStatus(
          error is LitchcraftStorageLimitExceeded
              ? litchStrings(context).craftLibraryFull
              : litchStrings(context).craftActionFailed,
        );
      }),
    );
  }

  Future<void> _runLocal(String key, Future<void> Function() action) async {
    if (_localActions.contains(key)) return;
    _recordCommandInteraction();
    setState(() => _localActions.add(key));
    try {
      await action();
    } catch (error) {
      if (mounted) {
        _showToolStatus(
          error is LitchcraftStorageLimitExceeded
              ? litchStrings(context).craftLibraryFull
              : litchStrings(context).craftActionFailed,
        );
      }
    } finally {
      if (mounted) {
        setState(() => _localActions.remove(key));
      } else {
        _localActions.remove(key);
      }
    }
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: Listenable.merge(<Listenable>[controller, library]),
    builder: (context, _) {
      _maybeReopenConflictSheet();
      // Brush changes are local-first and the controller serializes transport.
      // Keep the editor usable while BLE catches up instead of exposing command
      // latency by dimming the whole interface.
      final editingEnabled = !_working && !controller.boardArtworkUnresolved;
      final workspace = FocusTraversalGroup(
        policy: OrderedTraversalPolicy(),
        child: LayoutBuilder(
          builder: (context, bounds) =>
              _ribbonWorkspace(bounds, editingEnabled),
        ),
      );
      return Listener(
        behavior: HitTestBehavior.translucent,
        onPointerDown: _beginInteraction,
        onPointerUp: _endInteraction,
        onPointerCancel: _endInteraction,
        child: widget.rootSafeInsets != null
            ? MediaQuery.removePadding(
                context: context,
                removeLeft: true,
                removeTop: true,
                removeRight: true,
                removeBottom: true,
                child: workspace,
              )
            : SafeArea(
                top: MediaQuery.orientationOf(context) == Orientation.landscape,
                bottom:
                    MediaQuery.orientationOf(context) == Orientation.landscape,
                child: workspace,
              ),
      );
    },
  );

  Widget _ribbonWorkspace(BoxConstraints bounds, bool editingEnabled) {
    final window = MediaQuery.sizeOf(context);
    if (LitchWorkspaceGeometry.isTabletWindow(window) &&
        bounds.maxWidth >= 300 &&
        bounds.maxHeight >= 320) {
      return _tabletWorkspace(bounds.biggest, editingEnabled);
    }
    final landscape =
        MediaQuery.orientationOf(context) == Orientation.landscape &&
        bounds.maxWidth > bounds.maxHeight &&
        bounds.maxWidth >= 480;
    final compactPortrait = !landscape && _compactPortraitAccessibility;
    final portraitScrollFallback = !landscape && bounds.maxHeight < 480;
    final onLeft =
        widget.landscapeRibbonSide?.x.isNegative ??
        (OrientationWorkspaceScope.maybeOf(context)?.side ??
                _deviceLandscapeSide) ==
            DeviceLandscapeSide.left;
    final geometry = LitchWorkspaceGeometry(
      Size(bounds.maxWidth, bounds.maxHeight),
      safeInsets: widget.rootSafeInsets ?? MediaQuery.viewPaddingOf(context),
    );
    final landscapeInspectorWidth = geometry.compactNavigationWidth;
    final gutter = landscape
        ? LitchWorkspaceGeometry.landscapeToolGap
        : LitchWorkspaceGeometry.outerGap;
    final minimumLandscapeInspectorHeight =
        5 * 48 +
        (widget.landscapeFooter == null ? 0 : widget.landscapeFooterHeight);
    // Reclaim decorative outer padding before starving labeled tab targets.
    final requestedLandscapeMargin = geometry.landscapeEdgeInsets.top;
    final verticalMargin = landscape
        ? math.min(
            requestedLandscapeMargin,
            math.max(
              0.0,
              (bounds.maxHeight - minimumLandscapeInspectorHeight) / 2,
            ),
          )
        : 0.0;
    final availableHeight = math.max(
      0.0,
      bounds.maxHeight - verticalMargin * 2,
    );
    const boardCenterShift = 0.0;
    final centeredBoardHeight = availableHeight;
    final innerWidth = landscape
        ? math.max(
            0.0,
            bounds.maxWidth - geometry.landscapeEdgeInsets.horizontal,
          )
        : LitchWorkspaceGeometry.portraitCanvasSide(bounds.maxWidth);
    const quickRailWidth = 76.0;
    const paletteRailWidth = 48.0;
    const portraitPaletteDragHeight = 64.0;
    // The compact inspector's measured minimum (including its toolbar/history)
    // is 205 px. Reserving the former 218 px needlessly shrank standard iPhone
    // canvases below the width shared by Play.
    const portraitInspectorMinimumHeight = 205.0;
    final largePortraitText =
        !landscape && MediaQuery.textScalerOf(context).scale(1) >= 1.6;
    final boardSide = landscape
        ? math.min(centeredBoardHeight, geometry.landscapeCanvasSide)
        : math.min(
            innerWidth,
            math.max(
              0.0,
              availableHeight -
                  (compactPortrait
                      ? 252
                      : largePortraitText
                      ? 218
                      : portraitInspectorMinimumHeight) -
                  portraitPaletteDragHeight -
                  gutter * 3,
            ),
          );
    // Portrait tools fill the same workspace as the canvas, including when
    // the square itself is height-limited. Only the landscape dock is capped.
    final inspectorWidth = landscape ? landscapeInspectorWidth : innerWidth;
    final inspectorHeight = landscape
        ? availableHeight
        : math.max(
            portraitScrollFallback ? 252.0 : 0.0,
            availableHeight -
                boardSide -
                portraitPaletteDragHeight -
                gutter * 3,
          );
    final portraitControlWidth = inspectorWidth;
    final portraitQuickActionWidth = math.min(144.0, portraitControlWidth);
    final board = SizedBox.square(
      key: const ValueKey('craft-ribbon-board'),
      dimension: boardSide,
      child: _ordered(2, _canvasWithStatus(editingEnabled)),
    );
    final inspector = SizedBox(
      key: const ValueKey('craft-persistent-inspector'),
      width: inspectorWidth,
      height: inspectorHeight,
      child: _ordered(
        1,
        _studioInspector(
          editingEnabled,
          landscape: landscape,
          compactLandscape: false,
          compactTabsHorizontal: false,
          tabsOnLeft: landscape && onLeft,
        ),
      ),
    );
    final quickActions = SizedBox(
      key: const ValueKey('craft-quick-actions'),
      width: quickRailWidth,
      height: inspectorHeight,
      child: _quickActions(editingEnabled, axis: Axis.vertical),
    );
    final paletteRail = SizedBox(
      key: const ValueKey('craft-color-wheel-rail'),
      width: landscape ? paletteRailWidth : null,
      height: landscape
          ? math.min(inspectorHeight, 48 * 8)
          : portraitPaletteDragHeight,
      child: _paletteRail(
        editingEnabled,
        landscape ? Axis.vertical : Axis.horizontal,
      ),
    );
    final workspace = SizedBox(
      key: ValueKey(
        landscape ? 'craft-landscape-shell' : 'craft-portrait-shell',
      ),
      width: bounds.maxWidth,
      child: Padding(
        padding: landscape
            ? EdgeInsets.only(
                left: geometry.landscapeEdgeInsets.left,
                right: geometry.landscapeEdgeInsets.right,
                top: verticalMargin,
                bottom: verticalMargin,
              )
            : EdgeInsets.symmetric(
                horizontal: LitchWorkspaceGeometry.contentEdgeInset,
                vertical: verticalMargin,
              ),
        child: landscape
            ? Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: onLeft
                    ? [
                        inspector,
                        SizedBox(width: gutter),
                        paletteRail,
                        SizedBox(width: gutter),
                        Expanded(
                          child: Center(
                            child: Transform.translate(
                              offset: Offset(0, boardCenterShift),
                              child: board,
                            ),
                          ),
                        ),
                        SizedBox(width: gutter),
                        quickActions,
                      ]
                    : [
                        quickActions,
                        SizedBox(width: gutter),
                        Expanded(
                          child: Center(
                            child: Transform.translate(
                              offset: Offset(0, boardCenterShift),
                              child: board,
                            ),
                          ),
                        ),
                        SizedBox(width: gutter),
                        paletteRail,
                        SizedBox(width: gutter),
                        inspector,
                      ],
              )
            : Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  board,
                  SizedBox(height: gutter),
                  SizedBox(
                    width: portraitControlWidth,
                    height: portraitPaletteDragHeight,
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.center,
                      children: [
                        SizedBox(
                          width: portraitQuickActionWidth,
                          child: _quickActions(
                            editingEnabled,
                            axis: Axis.horizontal,
                          ),
                        ),
                        if (portraitControlWidth - portraitQuickActionWidth >=
                            8)
                          const SizedBox(width: 8),
                        if (portraitControlWidth - portraitQuickActionWidth > 8)
                          Expanded(child: paletteRail),
                      ],
                    ),
                  ),
                  SizedBox(height: gutter),
                  inspector,
                  SizedBox(height: gutter),
                ],
              ),
      ),
    );
    if (portraitScrollFallback) {
      return SingleChildScrollView(
        key: const ValueKey('craft-short-portrait-scroll'),
        child: workspace,
      );
    }
    return workspace;
  }

  Widget _paletteRail(bool editingEnabled, Axis axis) => LitchcraftPaletteRail(
    axis: axis,
    color: controller.color,
    enabled: editingEnabled,
    onSelected: (color) {
      _recordCommandInteraction();
      setState(() {
        _pickingColor = false;
        _effectTool = null;
      });
      unawaited(controller.selectPaletteColor(color));
    },
    onLongPress: (color) {
      _recordCommandInteraction();
      setState(() {
        _pickingColor = false;
        _effectTool = null;
      });
      unawaited(controller.fillAllWithColor(color));
    },
  );

  Widget _tabletWorkspace(Size pane, bool editingEnabled) {
    const inset = LitchWorkspaceGeometry.contentEdgeInset;
    final workspace = OrientationWorkspaceScope.maybeOf(context);
    final toolsOnLeft =
        workspace?.tabletToolsOnLeft ??
        _standaloneTabletToolsOnLeft ??
        widget.landscapeRibbonSide?.x.isNegative ??
        (_deviceLandscapeSide == DeviceLandscapeSide.left);
    if (MediaQuery.orientationOf(context) == Orientation.portrait) {
      return _tabletPortraitWorkspace(pane, editingEnabled, toolsOnLeft);
    }
    final columns = LitchWorkspaceGeometry.fitsTabletCraftDock(pane);
    final side = columns
        ? LitchWorkspaceGeometry.tabletCanvasSide(pane, columns: true)
        : math.max(
            0.0,
            math.min(pane.height - 2 * inset, pane.width - 3 * inset - 48),
          );
    void moveControls() {
      _finishCanvasGestureForReflow();
      if (workspace != null) {
        workspace.moveTabletToolsToOtherSide();
      } else {
        setState(() => _standaloneTabletToolsOnLeft = !toolsOnLeft);
      }
    }

    final board = SizedBox.square(
      key: const ValueKey('craft-ribbon-board'),
      dimension: side,
      child: _ordered(1, _canvasWithStatus(editingEnabled)),
    );
    void showDrawer(bool open) {
      _finishCanvasGestureForReflow();
      setState(() => _tabletDrawerOpen = open);
    }

    final alignment = toolsOnLeft
        ? Alignment.centerLeft
        : Alignment.centerRight;
    final order = toolsOnLeft ? TextDirection.rtl : TextDirection.ltr;
    final panel = SizedBox(
      width: math.min(
        LitchWorkspaceGeometry.controlGroupWidth,
        pane.width - 2 * inset,
      ),
      height: double.infinity,
      child: _ordered(
        2,
        _tabletWorkbench(
          editingEnabled,
          toolsOnLeft,
          moveControls,
          closeDrawer: columns ? null : () => showDrawer(false),
        ),
      ),
    );
    final drawerOpen = !columns && _tabletDrawerOpen;
    return Padding(
      padding: const EdgeInsets.all(inset),
      child: PopScope(
        canPop: !widget.active || !drawerOpen,
        onPopInvokedWithResult: (didPop, _) {
          if (!didPop && widget.active && drawerOpen) showDrawer(false);
        },
        child: Stack(
          key: const ValueKey('craft-tablet-workspace'),
          fit: StackFit.expand,
          children: [
            ExcludeSemantics(
              excluding: drawerOpen,
              child: ExcludeFocus(
                excluding: drawerOpen,
                child: Row(
                  textDirection: order,
                  children: [
                    Expanded(child: Center(child: board)),
                    SizedBox(
                      width: columns
                          ? LitchWorkspaceGeometry.tabletBoardGap
                          : inset,
                    ),
                    if (columns)
                      panel
                    else
                      SizedBox(
                        width: 48,
                        child: Center(
                          child: IconButton.filledTonal(
                            key: const ValueKey('craft-tablet-open-tools'),
                            tooltip: litchStrings(context).craftOpenTools,
                            onPressed: () => showDrawer(true),
                            style: IconButton.styleFrom(
                              minimumSize: const Size.square(48),
                            ),
                            icon: const Icon(Icons.tune),
                          ),
                        ),
                      ),
                  ],
                ),
              ),
            ),
            if (!columns) ...[
              if (drawerOpen)
                ModalBarrier(
                  color: Colors.black26,
                  onDismiss: () => showDrawer(false),
                  semanticsLabel: litchStrings(context).craftCloseTools,
                ),
              Offstage(
                offstage: !drawerOpen,
                child: ExcludeFocus(
                  excluding: !drawerOpen,
                  child: TickerMode(
                    enabled: drawerOpen,
                    child: Align(alignment: alignment, child: panel),
                  ),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _tabletPortraitWorkspace(
    Size pane,
    bool editingEnabled,
    bool toolsOnLeft,
  ) {
    const inset = LitchWorkspaceGeometry.contentEdgeInset;
    const divider = VerticalDivider(
      width: 1,
      thickness: 1,
      color: Colors.white24,
    );
    final availableWidth = pane.width - 2 * inset;
    // Each column needs five 48dp LED targets, four 4dp gaps and two 8dp margins.
    // Keep at least a 48dp target for each of the eight canvas rows in wide mode.
    // The tools then take their actual height, including any wrapped labels.
    final headingHeight = MediaQuery.textScalerOf(context).scale(16);
    final wide =
        availableWidth >= 3 * 272 + 2 &&
        pane.height >= 8 * 48 + 3 * inset + 360 + 3 * (headingHeight - 16);
    // Reclaim decorative gaps before shrinking the paint targets on a short
    // tablet with enlarged controls. Keep the ordinary horizontal clearance.
    final verticalInset =
        !wide && headingHeight >= 24 && pane.height < 8 * 48 + 3 * inset + 360
        ? inset / 2
        : inset;
    final strings = litchStrings(context);
    final sections = [
      (
        tray: LitchcraftSecondaryTray.palette,
        id: 'color',
        label: strings.craftColor,
        anchor: _tabletBrushKey,
      ),
      (
        tray: LitchcraftSecondaryTray.leds,
        id: 'leds',
        label: strings.craftLeds,
        anchor: _tabletLedsKey,
      ),
      (
        tray: LitchcraftSecondaryTray.effects,
        id: 'effects',
        label: strings.craftEffectsTab,
        anchor: _tabletEffectsKey,
      ),
      (
        tray: LitchcraftSecondaryTray.animation,
        id: 'frames',
        label: strings.craftFramesTab,
        anchor: _tabletFramesKey,
      ),
    ];
    Widget content(LitchcraftSecondaryTray tray) => switch (tray) {
      LitchcraftSecondaryTray.palette => Column(
        children: [
          SizedBox(
            height: 48,
            child: _quickActions(
              editingEnabled,
              axis: Axis.horizontal,
              tabletDockOnLeft: toolsOnLeft,
            ),
          ),
          const SizedBox(height: 8),
          SizedBox(
            height: wide ? 176 : 96,
            child: _colorInspector(editingEnabled, tablet: true),
          ),
        ],
      ),
      LitchcraftSecondaryTray.leds => _ledInspector(editingEnabled),
      LitchcraftSecondaryTray.effects => _effectsInspector(
        editingEnabled,
        tablet: true,
      ),
      LitchcraftSecondaryTray.animation => _animationInspector(
        editingEnabled,
        tablet: true,
        landscape: true,
        dockOnLeft: wide ? !toolsOnLeft : toolsOnLeft,
      ),
    };
    Widget section(LitchcraftSecondaryTray tray) {
      final item = sections.firstWhere((item) => item.tray == tray);
      return KeyedSubtree(
        key: item.anchor,
        child: Column(
          key: ValueKey('craft-tablet-section-${item.id}'),
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (wide) ...[
              Semantics(
                header: true,
                child: Text(
                  item.label,
                  style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    color: Colors.white,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ),
              const SizedBox(height: 6),
            ],
            content(tray),
          ],
        ),
      );
    }

    Widget column(String id, List<Widget> children) => Padding(
      key: ValueKey('craft-portrait-column-$id'),
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: children,
      ),
    );
    final toolColumns = wide
        ? Stack(
            children: [
              Row(
                textDirection: toolsOnLeft
                    ? TextDirection.ltr
                    : TextDirection.rtl,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Expanded(
                    child: column('color', [
                      section(LitchcraftSecondaryTray.palette),
                    ]),
                  ),
                  const SizedBox(width: 1),
                  Expanded(
                    child: column('lighting', [
                      section(LitchcraftSecondaryTray.leds),
                      const Divider(height: 16, color: Colors.white24),
                      section(LitchcraftSecondaryTray.effects),
                    ]),
                  ),
                  const SizedBox(width: 1),
                  Expanded(
                    child: column('frames', [
                      section(LitchcraftSecondaryTray.animation),
                    ]),
                  ),
                ],
              ),
              const Positioned.fill(
                child: IgnorePointer(
                  child: Row(
                    children: [Spacer(), divider, Spacer(), divider, Spacer()],
                  ),
                ),
              ),
            ],
          )
        : Column(
            children: [
              Row(
                children: [
                  for (final item in sections)
                    Expanded(
                      child: Semantics(
                        selected: _tabletPortraitSection == item.tray,
                        child: TextButton(
                          key: ValueKey('craft-portrait-select-${item.id}'),
                          onPressed: () => setState(
                            () => _tabletPortraitSection = item.tray,
                          ),
                          style: TextButton.styleFrom(
                            minimumSize: const Size(48, 48),
                            padding: const EdgeInsets.symmetric(horizontal: 4),
                            backgroundColor: _tabletPortraitSection == item.tray
                                ? Colors.white12
                                : null,
                            foregroundColor: Colors.white,
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(8),
                            ),
                          ),
                          child: Text(item.label, textAlign: TextAlign.center),
                        ),
                      ),
                    ),
                ],
              ),
              // Keep the board still when switching tools. Only the selected
              // section is painted, reachable or announced to accessibility.
              IndexedStack(
                index: LitchcraftSecondaryTray.values.indexOf(
                  _tabletPortraitSection,
                ),
                sizing: StackFit.passthrough,
                children: [
                  for (final item in sections)
                    ExcludeFocus(
                      excluding: item.tray != _tabletPortraitSection,
                      child: TickerMode(
                        enabled: item.tray == _tabletPortraitSection,
                        child: column(item.id, [section(item.tray)]),
                      ),
                    ),
                ],
              ),
            ],
          );
    return Padding(
      padding: EdgeInsets.symmetric(horizontal: inset, vertical: verticalInset),
      child: Column(
        key: const ValueKey('craft-tablet-workspace'),
        children: [
          Expanded(
            child: LayoutBuilder(
              builder: (context, bounds) => Center(
                child: SizedBox.square(
                  key: const ValueKey('craft-ribbon-board'),
                  dimension: bounds.biggest.shortestSide,
                  child: _ordered(1, _canvasWithStatus(editingEnabled)),
                ),
              ),
            ),
          ),
          SizedBox(height: verticalInset),
          SizedBox(
            key: const ValueKey('craft-portrait-tools-tray'),
            width: double.infinity,
            child: _ordered(
              2,
              Material(
                key: const ValueKey('craft-persistent-inspector'),
                color: const Color(0xff33494e),
                borderRadius: BorderRadius.circular(12),
                clipBehavior: Clip.antiAlias,
                child: ConstrainedBox(
                  constraints: BoxConstraints(minHeight: wide ? 360 : 0),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 8),
                        child: SizedBox(
                          height: 48,
                          child: Row(
                            textDirection: toolsOnLeft
                                ? TextDirection.rtl
                                : TextDirection.ltr,
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              Flexible(
                                child: ConstrainedBox(
                                  constraints: const BoxConstraints(
                                    maxWidth: 376,
                                  ),
                                  child: _artworkInspector(
                                    portrait: false,
                                    tablet: true,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 8),
                              SizedBox(
                                width: 144,
                                child: _historyFooter(
                                  editingEnabled,
                                  tabletDockOnLeft: toolsOnLeft,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                      const Divider(height: 1, color: Colors.white24),
                      toolColumns,
                    ],
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  void _revealTabletSection(GlobalKey key) {
    final section = key.currentContext;
    if (section == null) return;
    final renderObject = section.findRenderObject();
    final scrollable = Scrollable.maybeOf(section, axis: Axis.vertical);
    if (renderObject == null || scrollable == null) return;
    // Only the inspector should move; its parent may be the horizontal app pager.
    unawaited(
      scrollable.position.ensureVisible(
        renderObject,
        duration: const Duration(milliseconds: 180),
      ),
    );
  }

  Widget _tabletWorkbench(
    bool editingEnabled,
    bool toolsOnLeft,
    VoidCallback moveControls, {
    VoidCallback? closeDrawer,
  }) {
    final strings = litchStrings(context);
    final sections = [
      (id: 'color', label: strings.craftColor, anchor: _tabletBrushKey),
      (id: 'leds', label: strings.craftLeds, anchor: _tabletLedsKey),
      (
        id: 'effects',
        label: strings.craftEffectsTab,
        anchor: _tabletEffectsKey,
      ),
      (id: 'frames', label: strings.craftFramesTab, anchor: _tabletFramesKey),
    ];
    Widget heading(String label) => Semantics(
      header: true,
      child: Text(
        label,
        style: Theme.of(context).textTheme.titleMedium?.copyWith(
          color: Colors.white,
          fontWeight: FontWeight.w700,
        ),
      ),
    );
    Widget section(
      ({String id, String label, GlobalKey anchor}) item,
      Widget content,
    ) => Padding(
      key: item.anchor,
      padding: const EdgeInsets.symmetric(vertical: 12),
      child: Column(
        key: ValueKey('craft-tablet-section-${item.id}'),
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [heading(item.label), const SizedBox(height: 8), content],
      ),
    );
    const line = Divider(height: 1, color: Colors.white24);
    return Material(
      key: const ValueKey('craft-persistent-inspector'),
      color: const Color(0xff33494e),
      borderRadius: BorderRadius.circular(12),
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 10),
            child: ConstrainedBox(
              constraints: const BoxConstraints(minHeight: 48),
              child: Row(
                children: [
                  Expanded(child: heading(strings.craftLibrary)),
                  ValueListenableBuilder<bool>(
                    valueListenable: _tabletJumpAvailable,
                    builder: (context, available, _) => !available
                        ? const SizedBox.shrink()
                        : PopupMenuButton<GlobalKey>(
                            key: const ValueKey('craft-jump-to-tool-section'),
                            tooltip: strings.craftJumpToSection,
                            icon: const Icon(Icons.list, color: Colors.white),
                            onSelected: _revealTabletSection,
                            itemBuilder: (_) => [
                              for (final item in sections)
                                PopupMenuItem(
                                  key: ValueKey('craft-jump-${item.id}'),
                                  value: item.anchor,
                                  child: Text(item.label),
                                ),
                            ],
                          ),
                  ),
                  if (closeDrawer != null)
                    IconButton(
                      key: const ValueKey('craft-tablet-close-tools'),
                      tooltip: strings.craftCloseTools,
                      onPressed: closeDrawer,
                      style: IconButton.styleFrom(
                        minimumSize: const Size.square(48),
                        foregroundColor: Colors.white,
                      ),
                      icon: const Icon(Icons.close),
                    ),
                ],
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 8),
            child: _artworkInspector(portrait: false, tablet: true),
          ),
          line,
          Expanded(
            child: NotificationListener<ScrollMetricsNotification>(
              onNotification: (notice) {
                if (notice.depth == 0 && notice.metrics.axis == Axis.vertical) {
                  _tabletJumpAvailable.value =
                      notice.metrics.maxScrollExtent >
                      notice.metrics.minScrollExtent + 0.5;
                }
                return false;
              },
              child: LayoutBuilder(
                builder: (context, bounds) => SingleChildScrollView(
                  key: const PageStorageKey('craft-tablet-tools-scroll'),
                  primary: false,
                  padding: const EdgeInsets.symmetric(horizontal: 12),
                  child: ConstrainedBox(
                    constraints: BoxConstraints(minHeight: bounds.maxHeight),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                      children: [
                        section(
                          sections[0],
                          Column(
                            children: [
                              SizedBox(
                                height: 48,
                                child: _quickActions(
                                  editingEnabled,
                                  axis: Axis.horizontal,
                                  tabletDockOnLeft: toolsOnLeft,
                                ),
                              ),
                              const SizedBox(height: 8),
                              SizedBox(
                                height: 176,
                                child: _colorInspector(
                                  editingEnabled,
                                  tablet: true,
                                ),
                              ),
                            ],
                          ),
                        ),
                        line,
                        section(sections[1], _ledInspector(editingEnabled)),
                        line,
                        section(
                          sections[2],
                          _effectsInspector(editingEnabled, tablet: true),
                        ),
                        line,
                        section(
                          sections[3],
                          _animationInspector(
                            editingEnabled,
                            landscape: true,
                            tablet: true,
                            dockOnLeft: toolsOnLeft,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
          line,
          _historyFooter(
            editingEnabled,
            tabletDockOnLeft: toolsOnLeft,
            moveControls: moveControls,
          ),
        ],
      ),
    );
  }

  Widget _quickActions(
    bool editingEnabled, {
    required Axis axis,
    bool? tabletDockOnLeft,
  }) {
    final strings = litchStrings(context);
    Widget well({
      required Key key,
      required Widget child,
      bool selected = false,
    }) => _QuickActionWell(
      key: key,
      enabled: editingEnabled,
      selected: selected,
      size: axis == Axis.horizontal ? 48 : 72,
      visualSize: axis == Axis.horizontal ? 44 : 72,
      baseColor: axis == Axis.horizontal
          ? Colors.transparent
          : Colors.transparent,
      child: child,
    );
    final armedEffect = _effectTool != null;
    final erase = controller.eraser && !_pickingColor && !armedEffect;
    final VoidCallback? brushTap = editingEnabled
        ? () => _selectBrushMode(armedEffect ? false : !erase)
        : null;
    final brush = Semantics(
      key: const ValueKey('craft-quick-brush-semantics'),
      label: armedEffect
          ? strings.craftArmedEffect(_effectLabel(strings, _effectTool!))
          : erase
          ? strings.craftErase
          : strings.craftPaint,
      hint: armedEffect || erase
          ? strings.craftPaintMode
          : strings.craftEraseMode,
      button: true,
      enabled: editingEnabled,
      selected: !_pickingColor && !armedEffect,
      onTap: brushTap,
      excludeSemantics: true,
      child: LitchcraftHoldAction(
        holdLabel: erase ? strings.craftPaint : strings.craftErase,
        completedLabel: '',
        showCompletionSnackBar: false,
        reserveProgressSpace: axis != Axis.horizontal,
        onTap: brushTap,
        child: well(
          key: const ValueKey('craft-quick-brush'),
          selected: !_pickingColor && !armedEffect,
          child: Icon(
            armedEffect
                ? Icons.auto_awesome_outlined
                : erase
                ? Icons.cleaning_services_outlined
                : Icons.brush_outlined,
            size: 28,
          ),
        ),
      ),
    );
    final VoidCallback? previousTap = !editingEnabled
        ? null
        : _pickingColor
        ? () {
            _recordCommandInteraction();
            _toolStatusTimer?.cancel();
            setState(() {
              _pickingColor = false;
              _toolStatus = null;
            });
          }
        : controller.canSwapBrushColor
        ? () => unawaited(controller.swapBrushColor())
        : null;
    final VoidCallback? previousHold = editingEnabled
        ? () {
            _recordCommandInteraction();
            setState(() => _pickingColor = true);
            _showToolStatus(strings.craftPickColorHelp);
          }
        : null;
    final previous = Semantics(
      key: const ValueKey('craft-quick-previous-color-semantics'),
      label: strings.craftSwapColors,
      hint: strings.craftPickColor,
      button: true,
      enabled: previousTap != null || previousHold != null,
      selected: _pickingColor,
      onTap: previousTap,
      onLongPress: LitchcraftHoldAction.withCompletionFeedback(
        context,
        previousHold,
      ),
      excludeSemantics: true,
      child: LitchcraftHoldAction(
        duration: kLongPressTimeout,
        holdLabel: strings.craftPickColor,
        completedLabel: '',
        showCompletionSnackBar: false,
        reserveProgressSpace: axis != Axis.horizontal,
        onTap: previousTap,
        onHold: previousHold,
        child: well(
          key: const ValueKey('craft-quick-previous-color'),
          selected: _pickingColor,
          child: _pickingColor
              ? Icon(
                  Icons.colorize_outlined,
                  key: const ValueKey('craft-quick-eyedropper-active'),
                  size: axis == Axis.horizontal ? 29 : 25.6,
                )
              : _PreviousColorEyedropperGlyph(
                  currentColor: controller.color,
                  previousColor: controller.previousColor,
                  size: axis == Axis.horizontal ? 36 : 64,
                ),
        ),
      ),
    );
    final VoidCallback? fillTap = editingEnabled
        ? () {
            _recordCommandInteraction();
            unawaited(controller.fillAll());
          }
        : null;
    final VoidCallback? fillHold = editingEnabled
        ? () {
            _recordCommandInteraction();
            unawaited(controller.clear());
            _showToolStatus(strings.craftClear);
          }
        : null;
    final fill = Semantics(
      key: const ValueKey('craft-quick-fill-semantics'),
      label: strings.craftFillCanvas,
      hint: strings.craftClear,
      button: true,
      enabled: editingEnabled,
      onTap: fillTap,
      onLongPress: LitchcraftHoldAction.withCompletionFeedback(
        context,
        fillHold,
      ),
      excludeSemantics: true,
      child: LitchcraftHoldAction(
        duration: kLongPressTimeout,
        holdLabel: strings.craftClear,
        completedLabel: '',
        showCompletionSnackBar: false,
        reserveProgressSpace: axis != Axis.horizontal,
        onTap: fillTap,
        onHold: fillHold,
        child: well(
          key: const ValueKey('craft-quick-fill'),
          child: const Icon(Icons.format_color_fill_outlined, size: 28),
        ),
      ),
    );
    if (tabletDockOnLeft != null) {
      return Row(
        textDirection: tabletDockOnLeft ? TextDirection.ltr : TextDirection.rtl,
        children: [
          SizedBox(width: 48, child: brush),
          Expanded(child: _paletteRail(editingEnabled, Axis.horizontal)),
          SizedBox(width: 48, child: previous),
          SizedBox(width: 48, child: fill),
        ],
      );
    }
    final actions = <Widget>[brush, previous, fill];
    return Semantics(
      container: true,
      child: axis == Axis.horizontal
          ? LayoutBuilder(
              builder: (context, bounds) => DecoratedBox(
                key: const ValueKey('craft-quick-actions-capsule-portrait'),
                decoration: BoxDecoration(
                  color: const Color(0xff33494e),
                  borderRadius: BorderRadius.circular(24),
                ),
                child: Stack(
                  children: [
                    Positioned.fill(
                      child: Row(
                        children: [
                          for (final action in actions)
                            Expanded(child: Center(child: action)),
                        ],
                      ),
                    ),
                    for (final fraction in const [1 / 3, 2 / 3])
                      Positioned(
                        left: bounds.maxWidth * fraction,
                        top: 10,
                        bottom: 10,
                        width: 1,
                        child: const IgnorePointer(
                          child: ColoredBox(color: Colors.white12),
                        ),
                      ),
                  ],
                ),
              ),
            )
          : DecoratedBox(
              decoration: BoxDecoration(
                color: const Color(0xff33494e),
                borderRadius: BorderRadius.circular(38),
              ),
              child: Column(
                children: [
                  Expanded(child: Center(child: actions[0])),
                  const SizedBox(
                    width: 28,
                    height: 1,
                    child: ColoredBox(color: Colors.white12),
                  ),
                  Expanded(child: Center(child: actions[1])),
                  const SizedBox(
                    width: 28,
                    height: 1,
                    child: ColoredBox(color: Colors.white12),
                  ),
                  Expanded(child: Center(child: actions[2])),
                ],
              ),
            ),
    );
  }

  Widget _ordered(double order, Widget child) => FocusTraversalOrder(
    order: NumericFocusOrder(order),
    child: Semantics(
      container: true,
      sortKey: OrdinalSortKey(order),
      child: child,
    ),
  );

  Widget _canvas(bool editingEnabled, {bool showStatus = false}) {
    final boardAuthority = _usesBoardPlayback;
    final previewing = boardAuthority
        ? controller.animationPlaying
        : _previewTicker.isActive;
    final document = library.animationDocument;
    // Frame playback selects the frame; effects have their own live clock.
    final renderAnimation =
        controller.status != LitchcraftStatus.conflict &&
        _hasAnimationMotion(document);
    final preview = renderAnimation
        ? composeLitchcraftAnimationDocument(
            document,
            boardAuthority
                ? controller.animationElapsedMilliseconds
                : _previewElapsedMs,
            effectElapsedMs:
                controller.animationEffectElapsedMilliseconds ??
                _effectElapsedMs,
          )
        : null;
    final canvas = preview == null
        ? controller.canvas
        : LitchcraftCanvas(
            colors: preview.colors,
            topology: controller.canvas.topology,
          );
    final frameIndex = (preview?.frameIndex ?? library.selectedFrameIndex)
        .clamp(0, document.frames.length - 1);
    final activeFrame = document.frames[frameIndex];
    final selectedFrame = library.selectedFrameIndex;
    final replacementGeneration =
        controller.documentSession.replacementGeneration;
    final color = controller.color;
    final mask = controller.targetMask;
    final erasing = controller.eraser;
    final flipped = controller.flipped;
    final pickingColor = _pickingColor;
    final effectTool = _effectTool;
    LitchcraftCanvas? previewPaint(LitchcraftCanvas base, int square) {
      // A pending touch owns its tool and source, but not a paused timeline or
      // history entry. Recheck live state even if no widget rebuild has run.
      final currentDocument = library.animationDocument;
      final currentRenderAnimation =
          controller.status != LitchcraftStatus.conflict &&
          _hasAnimationMotion(currentDocument);
      final currentFrame = currentRenderAnimation
          ? litchcraftAnimationFrameIndexAt(
              currentDocument,
              _usesBoardPlayback
                  ? controller.animationElapsedMilliseconds
                  : _previewElapsedMs,
            )
          : library.selectedFrameIndex;
      if (controller.color != color ||
          controller.targetMask != mask ||
          controller.eraser != erasing ||
          controller.flipped != flipped ||
          controller.canvas.topology != canvas.topology ||
          _pickingColor != pickingColor ||
          _effectTool != effectTool ||
          _usesBoardPlayback != boardAuthority ||
          controller.documentSession.replacementGeneration !=
              replacementGeneration ||
          library.selectedFrameIndex != selectedFrame ||
          currentFrame != frameIndex ||
          (controller.animationSelectedFrameIndex != frameIndex &&
              !(!boardAuthority && previewing && _previewTicker.isActive)) ||
          !identical(currentDocument.frames[currentFrame], activeFrame) ||
          currentDocument.sparkleSeed != document.sparkleSeed) {
        return null;
      }
      final emitters = <LitchcraftAnimatedEmitter>[
        for (var region = 0; region < 4; region++)
          if (mask & (1 << region) != 0)
            LitchcraftAnimatedEmitter(
              color: erasing ? 0 : color,
              effect: erasing
                  ? LitchcraftEffectStyle.off
                  : activeFrame.emitters[square * 4 + region].effect,
              effectDurationMs: erasing
                  ? 0
                  : activeFrame.emitters[square * 4 + region].effectDurationMs,
            )
          else
            activeFrame.emitters[square * 4 + region],
      ];
      final composed = composeLitchcraftAnimatedSquare(
        emitters: emitters,
        elapsedMs:
            controller.animationEffectElapsedMilliseconds ?? _effectElapsedMs,
        sparkleSeed: document.sparkleSeed,
        frameIndex: frameIndex,
        emitterBaseIndex: square * 4,
      );
      final colors = base.colors.toList();
      for (var region = 0; region < 4; region++) {
        colors[square * 4 + region] = composed[region];
      }
      return LitchcraftCanvas(colors: colors, topology: base.topology);
    }

    final effectLabels = <int, String>{};
    for (var square = 0; square < 64; square++) {
      final names = activeFrame.emitters
          .skip(square * 4)
          .take(4)
          .where((emitter) => emitter.effect != LitchcraftEffectStyle.off)
          .map((emitter) => _effectLabel(litchStrings(context), emitter.effect))
          .toSet();
      if (names.isNotEmpty) effectLabels[square] = names.join(', ');
    }
    return Align(
      alignment: Alignment.topCenter,
      child: AspectRatio(
        aspectRatio: 1,
        child: Stack(
          children: <Widget>[
            Positioned.fill(
              child: ClipRect(
                child: _presentationFlipTransition(
                  key: const ValueKey('craft-canvas-orientation-transition'),
                  child: LitchcraftCanvasView(
                    key: _canvasKey,
                    canvas: canvas,
                    previewPaint: previewPaint,
                    paintLabel: erasing
                        ? litchStrings(context).craftErase
                        : litchStrings(context).craftPaint,
                    effectLabels: effectLabels,
                    flipped: controller.flipped,
                    enabled: editingEnabled,
                    onRotateQuarterTurn: editingEnabled ? _rotateArtwork : null,
                    onRotationGestureChanged: null,
                    onInspectSquare: _pickingColor
                        ? _pickSquareColor
                        : _effectTool == null
                        ? null
                        : _applyEffectToSquare,
                    inspectLabel: _pickingColor
                        ? litchStrings(context).craftPickColor
                        : litchStrings(context).craftArmedEffect(
                            _effectLabel(
                              litchStrings(context),
                              _effectTool ?? LitchcraftEffectStyle.off,
                            ),
                          ),
                    cancelGestures: _gestureCancellation,
                    onBegin: () {
                      _stopPreview();
                      library.beginCanvasGesture();
                      controller.beginStroke();
                    },
                    onPaint: controller.paintSquares,
                    onEraseSquare: (square) {
                      _stopPreview();
                      library.beginCanvasGesture();
                      controller.beginStroke(erasing: true);
                      controller.paintSquares(<int>[square]);
                      controller.endStroke();
                      library.endCanvasGesture();
                      _recordCommandInteraction();
                    },
                    onEnd: () {
                      controller.endStroke();
                      library.endCanvasGesture();
                      _recordCommandInteraction();
                    },
                  ),
                ),
              ),
            ),
            if ((_pickingColor || _effectTool != null) &&
                !_hasPersistentStatus() &&
                _toolStatus == null &&
                _frameTimingStatus == null)
              Positioned(
                left: 8,
                right: previewing ? 56 : 8,
                top: 8,
                child: IgnorePointer(
                  child: Align(
                    alignment: AlignmentDirectional.topStart,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        color: const Color(0xe6283d42),
                        borderRadius: BorderRadius.circular(8),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 4,
                        ),
                        child: Text(
                          _pickingColor
                              ? litchStrings(context).craftPickColor
                              : litchStrings(context).craftArmedEffect(
                                  _effectLabel(
                                    litchStrings(context),
                                    _effectTool!,
                                  ),
                                ),
                          key: const ValueKey('craft-armed-action'),
                          style: const TextStyle(
                            color: Colors.white,
                            fontSize: 12,
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            if (previewing)
              Positioned(
                key: const ValueKey('craft-animation-playing-indicator'),
                top: 8,
                right: 8,
                child: IgnorePointer(
                  child: Semantics(
                    label: litchStrings(
                      context,
                    ).craftPlayingFrame(frameIndex + 1),
                    child: DecoratedBox(
                      decoration: const BoxDecoration(
                        color: Color(0xd9283d42),
                        shape: BoxShape.circle,
                      ),
                      child: Padding(
                        padding: EdgeInsets.all(6),
                        child: Icon(
                          Icons.play_arrow_rounded,
                          size: 18,
                          color: Colors.white,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            if (showStatus)
              Positioned(
                left: 12,
                right: 12,
                top: 12,
                child: IgnorePointer(
                  ignoring: !_hasPersistentStatus() && !_toolStatusShowsUndo,
                  child: _statusOverlay(elevation: 0),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _canvasWithStatus(bool editingEnabled) =>
      _canvas(editingEnabled, showStatus: true);

  Widget _presentationFlipTransition({
    required Key key,
    required Widget child,
  }) {
    // The retained screen owns one finite transition. Reparenting canvas/icons
    // must never recreate a half-turn merely because a layout rebuilt.
    return AnimatedBuilder(
      animation: _presentationFlip,
      builder: (context, child) => Transform.rotate(
        angle:
            (1 - Curves.easeOutCubic.transform(_presentationFlip.value)) *
            math.pi,
        child: child,
      ),
      child: OrientationContentTurn(
        child: KeyedSubtree(key: key, child: child),
      ),
    );
  }

  // One playback dispatch for the Frames button and its toolbar hold action.
  void _toggleAnimationPlayback() {
    if (controller.status == LitchcraftStatus.conflict ||
        !_hasAnimationMotion(library.animationDocument)) {
      return;
    }
    if (_usesBoardPlayback) {
      _recordCommandInteraction();
      unawaited(_toggleBoardAnimationPlayback());
    } else {
      _togglePreview();
    }
  }

  Future<void> _toggleBoardAnimationPlayback() async {
    try {
      await controller.toggleAnimationPlayback();
    } catch (_) {
      if (mounted) {
        _showToolStatus(litchStrings(context).craftActionFailed);
      }
    }
  }

  void _togglePreview() {
    if (!library.canEditAnimation) return;
    if (_previewTicker.isActive) {
      _stopPreview();
      return;
    }
    _seekPreviewToSelectedFrame(notify: false);
    setState(() {
      _pickingColor = false;
      _effectTool = null;
      _previewStartElapsedMs = _previewElapsedMs;
      _previewTicker.start();
      _localPreviewPlaying.value = true;
    });
  }

  void _seekPreviewToSelectedFrame({bool notify = true}) {
    _stopPreview(notify: false, selectDisplayedFrame: false);
    final start = litchcraftAnimationFrameStartOrNull(
      library.animationDocument,
      library.selectedFrameIndex,
    );
    if (start == null) return;
    void seek() {
      _previewElapsedMs = start;
      _previewStartElapsedMs = start;
    }

    if (notify && mounted) {
      setState(seek);
    } else {
      seek();
    }
  }

  bool _hasAnimationMotion(LitchcraftAnimationDocument document) =>
      document.frames.length > 1 ||
      document.frames.any(
        (frame) => frame.emitters.any(
          (emitter) => emitter.effect != LitchcraftEffectStyle.off,
        ),
      );

  void _syncAuthoritativePreview() {
    if (_usesBoardPlayback) {
      // Admission hands both frame selection and playback to the board. A
      // detached preview must not seek that authority or resume on later loss.
      _stopPreview(notify: false, selectDisplayedFrame: false);
    } else if (!widget.active || !_appResumed) {
      _stopPreview(notify: false);
    }
    final shouldTick =
        widget.active &&
        _appResumed &&
        ((_usesBoardPlayback && controller.animationPlaying) ||
            library.animationDocument.frames.any(
              (frame) => frame.emitters.any(
                (emitter) => emitter.effect != LitchcraftEffectStyle.off,
              ),
            ));
    if (!shouldTick) {
      _authoritativePreviewTimer?.cancel();
      _authoritativePreviewTimer = null;
      return;
    }
    if (_authoritativePreviewTimer != null) return;
    _authoritativePreviewTimer = Timer.periodic(
      const Duration(milliseconds: 50),
      (_) {
        if (mounted) setState(() {});
      },
    );
  }

  void _stopPreview({bool notify = true, bool selectDisplayedFrame = true}) {
    if (!_previewTicker.isActive) return;
    final displayed = litchcraftAnimationFrameIndexAt(
      library.animationDocument,
      _previewElapsedMs,
    );
    if (selectDisplayedFrame && displayed != library.selectedFrameIndex) {
      // Pausing or opening another editor makes the visibly displayed frame
      // the next edit target; never leave a hidden different frame selected.
      library.selectFrame(displayed);
    }
    _previewTicker.stop();
    _localPreviewPlaying.value = false;
    if (mounted && notify) {
      setState(() {});
      _scheduleIdleTip();
    }
  }

  void _applyEffectToSquare(int square) {
    final effect = _effectTool;
    if (effect == null || !_canDirectlyEditAnimation) return;
    if (!_usesBoardPlayback) _stopPreview();
    final target = controller.captureAnimationFrameEditTarget();
    if (target == null) return;
    final mask = controller.targetMask;
    final duration = _effectDurationMs;
    _runInBackground(() async {
      await controller.applyAnimationEffect(
        square: square,
        mask: mask,
        effect: effect,
        effectDurationMs: duration,
        target: target,
      );
    });
  }

  Future<bool> _confirmDeleteFrame(BuildContext sheetContext, int frame) async {
    if (!_canDirectlyEditAnimation) return false;
    _cancelPhoneShakeQualification();
    final strings = litchStrings(sheetContext);
    _deleteFrameDialogVisible = true;
    try {
      return await showDialog<bool>(
            context: sheetContext,
            routeSettings: const RouteSettings(
              name: 'litchcraft-delete-frame-confirmation',
            ),
            builder: (dialogContext) {
              _deleteFrameDialogContext = dialogContext;
              return AlertDialog(
                title: Text(strings.craftDeleteFrameQuestion(frame)),
                content: Text(strings.craftDeleteFrameBody(frame)),
                actions: <Widget>[
                  TextButton(
                    onPressed: () => Navigator.of(dialogContext).pop(false),
                    child: Text(strings.cancelAction),
                  ),
                  FilledButton(
                    key: const ValueKey('craft-confirm-delete-frame'),
                    onPressed: _canDirectlyEditAnimation
                        ? () => Navigator.of(dialogContext).pop(true)
                        : null,
                    child: Text(strings.craftDeleteFrame),
                  ),
                ],
              );
            },
          ) ??
          false;
    } finally {
      _deleteFrameDialogVisible = false;
      _deleteFrameDialogContext = null;
    }
  }

  String _effectLabel(AppLocalizations strings, LitchcraftEffectStyle effect) =>
      switch (effect) {
        LitchcraftEffectStyle.off => strings.craftEffectOff,
        LitchcraftEffectStyle.breathe => strings.craftEffectBreathe,
        LitchcraftEffectStyle.flash => strings.craftEffectFlash,
        LitchcraftEffectStyle.fadeToBlack => strings.craftEffectFade,
        LitchcraftEffectStyle.rainbow => strings.craftEffectRainbow,
        LitchcraftEffectStyle.orbitClockwise =>
          strings.craftEffectOrbitClockwise,
        LitchcraftEffectStyle.orbitCounterclockwise =>
          strings.craftEffectOrbitCounterclockwise,
        LitchcraftEffectStyle.bounce => strings.craftEffectBounce,
        LitchcraftEffectStyle.sparkle => strings.craftEffectSparkle,
      };

  void _rotateArtwork(int direction) {
    if (controller.status == LitchcraftStatus.live) {
      _showToolStatus(litchStrings(context).craftRotatingArtwork);
    }
    _runInBackground(() async {
      final applied = await controller.rotateArtwork(direction);
      if (!mounted || !applied) return;
      _showToolStatus(
        litchStrings(context).craftArtworkRotatedUndo,
        showUndo: true,
      );
    });
  }

  Future<void> _share(BuildContext shareContext) async {
    await library.ready;
    if (!mounted || !shareContext.mounted) return;
    // The library owns metadata as well as the immutable editor document.
    await _shareArtwork(
      shareContext,
      library.currentArtworkForSharing(litchStrings(context).craftDefaultName),
      stillCanvas: controller.canvas,
    );
  }

  Future<void> _shareArtwork(
    BuildContext shareContext,
    LitchcraftArtwork artwork, {
    LitchcraftCanvas? stillCanvas,
  }) async {
    if (_sharing || !widget.active) return;
    _recordCommandInteraction();
    setState(() => _sharing = true);
    Directory? exportDirectory;
    File? exportFile;
    Future<LitchcraftMediaExportResult>? conversion;
    try {
      final resolved = await artwork.resolve();
      if (!mounted || !widget.active || !shareContext.mounted) return;
      final choice = await showLitchcraftShareOptions(
        context,
        resolved.document,
        flipped: controller.flipped,
      );
      if (choice == null ||
          !mounted ||
          !widget.active ||
          !shareContext.mounted) {
        return;
      }
      Uint8List? bytes;
      String extension;
      String mimeType;
      if (choice.format == LitchcraftShareFormat.editable) {
        bytes = encodeLitchcraftEditableArtwork(resolved);
        extension = 'litchcraft.json';
        mimeType = 'application/json';
      } else if (choice.format == LitchcraftShareFormat.still) {
        bytes = await renderLitchcraftArtworkPng(
          stillCanvas ?? resolved.canvas,
          size: choice.settings.size,
          flipped: choice.settings.flipped,
        );
        extension = 'png';
        mimeType = 'image/png';
      } else {
        final result = await runLitchcraftMediaJob<LitchcraftMediaExportResult>(
          context,
          (token, progress) {
            conversion = () async {
              token.check();
              if (exportDirectory != null) {
                await exportDirectory!.delete(recursive: true);
                exportDirectory = null;
                exportFile = null;
              }
              final temporaryRoot = await getTemporaryDirectory();
              exportDirectory = await temporaryRoot.createTemp(
                'litchcraft-export-',
              );
              exportFile = File(
                '${exportDirectory!.path}/artwork.${choice.format == LitchcraftShareFormat.gif ? 'gif' : 'png'}',
              );
              final sink = await exportFile!.open(mode: FileMode.write);
              try {
                return await exportLitchcraftMedia(
                  resolved.document,
                  settings: choice.settings,
                  renderRgba: renderLitchcraftArtworkRgba,
                  token: token,
                  onProgress: progress,
                  encodedSink: (chunk) async {
                    await sink.writeFrom(chunk);
                  },
                );
              } finally {
                await sink.close();
              }
            }();
            return conversion!;
          },
        );
        if (result == null) return;
        extension = result.extension;
        mimeType = result.mimeType;
      }
      if (!mounted || !widget.active || !shareContext.mounted) return;
      final box = shareContext.findRenderObject() as RenderBox?;
      final safeName = resolved.name
          .trim()
          .replaceAll(RegExp(r'[^A-Za-z0-9._-]+'), '-')
          .replaceAll(RegExp(r'^[-.]+|[-.]+$'), '');
      await SharePlus.instance.share(
        ShareParams(
          title: resolved.name,
          files: [
            exportFile != null
                ? XFile(exportFile!.path, mimeType: mimeType)
                : XFile.fromData(bytes!, mimeType: mimeType),
          ],
          fileNameOverrides: [
            '${safeName.isEmpty ? 'litchcraft-artwork' : safeName}.$extension',
          ],
          sharePositionOrigin: box == null
              ? null
              : box.localToGlobal(Offset.zero) & box.size,
        ),
      );
    } catch (_) {
      if (mounted && widget.active) {
        _showToolStatus(litchStrings(context).craftShareFailed);
      }
    } finally {
      // A cancelled dialog can close before its worker observes cancellation.
      // Wait for that job to release the file before deleting only our output.
      if (conversion != null) {
        try {
          await conversion;
        } catch (_) {
          /* The dialog already reports failure. */
        }
      }
      if (exportDirectory != null) {
        try {
          await exportDirectory!.delete(recursive: true);
        } catch (_) {
          /* Temporary cleanup must not replace a share result. */
        }
      }
      if (mounted) setState(() => _sharing = false);
    }
  }

  Future<void> _importArtwork() async {
    if (_importing || _working || !widget.active) return;
    _recordCommandInteraction();
    setState(() => _importing = true);
    var applying = false;
    try {
      await library.ready;
      if (!mounted || !widget.active) return;
      final source = library.animationDocument;
      final sourceLibrary = library;
      final sourceController = controller;
      final generation = library.session.replacementGeneration;
      final session = controller.sessionId;
      String? connectedBoardId() => controller.bleService.isConnected
          ? controller.bleService.connectedId
          : null;
      final boardId = connectedBoardId();
      var connection = controller.bleService.connectionSessionToken;
      bool sameSource() =>
          mounted &&
          widget.active &&
          generation == library.session.replacementGeneration &&
          identical(sourceLibrary, library) &&
          identical(sourceController, controller) &&
          identical(source, library.animationDocument) &&
          session == controller.sessionId;
      final file =
          await (widget.artworkFilePicker?.call() ??
              openFile(
                acceptedTypeGroups: [
                  const XTypeGroup(
                    label: 'Litchcraft',
                    extensions: ['gif', 'png', 'apng', 'json'],
                    mimeTypes: [
                      'image/gif',
                      'image/png',
                      'image/apng',
                      'application/json',
                    ],
                    uniformTypeIdentifiers: [
                      'com.compuserve.gif',
                      'public.png',
                      'public.json',
                    ],
                  ),
                ],
              ));
      if (file == null || !mounted || !widget.active) return;
      final fileLength = await file.length();
      if (fileLength > litchcraftMediaMaximumEncodedBytes) {
        throw const LitchcraftMediaException(
          LitchcraftMediaErrorCode.inputTooLarge,
          'File exceeds the import limit.',
        );
      }
      final buffer = BytesBuilder(copy: false);
      await for (final chunk in file.openRead(0, fileLength)) {
        if (buffer.length + chunk.length > litchcraftMediaMaximumEncodedBytes) {
          throw const LitchcraftMediaException(
            LitchcraftMediaErrorCode.inputTooLarge,
            'File exceeds the import limit.',
          );
        }
        buffer.add(chunk);
      }
      final bytes = buffer.takeBytes();
      if (!mounted || !widget.active) return;
      final first = bytes.firstWhere(
        (value) => value != 32 && value != 10 && value != 13 && value != 9,
        orElse: () => 0,
      );
      LitchcraftArtwork artwork;
      if (first == 123) {
        artwork = await compute(decodeLitchcraftEditableArtwork, bytes);
        if (!mounted || !widget.active) return;
        final accepted = await showDialog<bool>(
          context: context,
          builder: (dialogContext) {
            final strings = litchStrings(dialogContext);
            return AlertDialog(
              title: Text(strings.craftImportOptions),
              content: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(artwork.name),
                    const SizedBox(height: 12),
                    SizedBox.square(
                      dimension: 144,
                      child: LitchcraftThumbnail(
                        canvas: artwork.canvas,
                        flipped: controller.flipped,
                      ),
                    ),
                    const SizedBox(height: 12),
                    Text(strings.craftEditableNotice),
                  ],
                ),
              ),
              actions: [
                TextButton(
                  onPressed: () => Navigator.of(dialogContext).pop(false),
                  child: Text(strings.cancelAction),
                ),
                FilledButton(
                  key: const ValueKey('craft-import-accept'),
                  onPressed: () => Navigator.of(dialogContext).pop(true),
                  child: Text(strings.craftImport),
                ),
              ],
            );
          },
        );
        if (accepted != true) return;
      } else {
        final metadata = await inspectLitchcraftMediaAsync(bytes);
        if (!mounted || !widget.active) return;
        final result = await showLitchcraftImportOptions(
          context,
          bytes,
          metadata,
          topology: source.topology,
          favoriteColors: source.favoriteColors,
          flipped: controller.flipped,
        );
        if (result == null) return;
        if (!mounted || !widget.active) return;
        final candidateName = file.name
            .replaceFirst(
              RegExp(r'\.(gif|apng|png)$', caseSensitive: false),
              '',
            )
            .trim();
        final name =
            candidateName.isNotEmpty &&
                candidateName.length <= 80 &&
                isSafeDisplayText(candidateName)
            ? candidateName
            : litchStrings(context).craftDefaultName;
        final now = DateTime.now().toUtc();
        artwork = LitchcraftArtwork.animated(
          id: 'import-${now.microsecondsSinceEpoch}',
          name: name,
          document: result.document,
          createdAt: now,
          modifiedAt: now,
        );
      }
      if (!mounted || !widget.active) return;
      // Picker return can precede the same board's reconnect. The explicit
      // preview acceptance owns this one rebind; library admission and later
      // asynchronous work keep it frozen against another connection change.
      if (boardId != null && connectedBoardId() == boardId && sameSource()) {
        connection = controller.bleService.connectionSessionToken;
      }
      bool canAdmit() =>
          sameSource() &&
          boardId == connectedBoardId() &&
          connection == controller.bleService.connectionSessionToken;
      if (!canAdmit()) {
        _showToolStatus(litchStrings(context).craftImportChanged);
        return;
      }
      // The existing library preserves the old draft and owns acknowledged
      // document delivery. No separate import authority or storage is created.
      applying = true;
      await library.open(
        artwork,
        source: LitchcraftOpenSource.imported,
        admissionGuard: () {
          final admitted = canAdmit();
          if (!admitted && mounted && widget.active) {
            _showToolStatus(litchStrings(context).craftImportChanged);
          }
          return admitted;
        },
      );
    } on LitchcraftMediaException catch (error) {
      if (!mounted || !widget.active) return;
      final strings = litchStrings(context);
      _showToolStatus(
        const [
              LitchcraftMediaErrorCode.inputTooLarge,
              LitchcraftMediaErrorCode.dimensionsTooLarge,
              LitchcraftMediaErrorCode.frameLimit,
            ].contains(error.code)
            ? strings.craftMediaTooLarge
            : error.code == LitchcraftMediaErrorCode.unsupported
            ? strings.craftMediaUnsupported
            : strings.craftMediaFailed,
      );
    } catch (_) {
      if (mounted && widget.active) {
        _showToolStatus(
          applying
              ? library.saveFailed
                    ? litchStrings(context).craftSaveFailed
                    : litchStrings(context).craftActionFailed
              : litchStrings(context).craftMediaFailed,
        );
      }
    } finally {
      if (mounted) setState(() => _importing = false);
    }
  }

  void _toggleSecondaryTray(LitchcraftSecondaryTray tray) {
    _cancelPhoneShakeQualification();
    _recordCommandInteraction();
    if (_activeSecondaryTray == tray && !_artworkContext) return;
    _artworkContext = false;
    if (_activeSecondaryTray == LitchcraftSecondaryTray.animation) {
      _stopPreview(notify: false);
    }
    setState(() => _activeSecondaryTray = tray);
  }

  void _selectAnimationFrame(int index) {
    if (_usesBoardPlayback) {
      _runInBackground(() => controller.selectAnimationFrame(index));
    } else {
      library.selectFrame(index);
      _seekPreviewToSelectedFrame();
    }
  }

  Widget _studioInspector(
    bool editingEnabled, {
    required bool landscape,
    required bool compactLandscape,
    required bool compactTabsHorizontal,
    bool tabsOnLeft = false,
  }) {
    const toolRailWidth = 48.0;
    final tabs = _inspectorTabs(
      editingEnabled,
      vertical: landscape,
      verticalWidth: toolRailWidth,
    );
    final content = _inspectorContent(
      editingEnabled,
      portrait: !landscape,
      includeHistoryFooter: landscape,
    );
    return Material(
      key: const ValueKey('craft-inspector'),
      color: const Color(0xff33494e),
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.all(Radius.circular(12)),
      ),
      clipBehavior: Clip.antiAlias,
      child: landscape
          ? Column(
              children: [
                Expanded(
                  child: Row(
                    children: tabsOnLeft
                        ? [tabs, Expanded(child: content)]
                        : [Expanded(child: content), tabs],
                  ),
                ),
                if (widget.landscapeFooter != null)
                  SizedBox(
                    height: widget.landscapeFooterHeight,
                    child: widget.landscapeFooter,
                  ),
              ],
            )
          : Column(
              children: [
                Expanded(child: content),
                _historyFooter(editingEnabled, portrait: true),
                tabs,
              ],
            ),
    );
  }

  Widget _rotateTool() {
    final strings = litchStrings(context);
    return Tooltip(
      message: strings.craftRotateCanvas,
      child: LitchcraftHoldAction(
        holdLabel: strings.craftRotateCanvas,
        completedLabel: strings.craftArtworkRotatedUndo,
        showCompletionSnackBar: false,
        reserveProgressSpace: false,
        onTap: _working || controller.boardArtworkUnresolved
            ? null
            : () => _rotateArtwork(1),
        onHold: _working || controller.boardArtworkUnresolved
            ? null
            : () => _rotateArtwork(-1),
        child: SizedBox(
          key: const ValueKey('craft-rotate-artwork'),
          width: 48,
          height: 48,
          child: Center(
            child: _presentationFlipTransition(
              key: const ValueKey('craft-rotate-icon-transition'),
              child: const Icon(Icons.rotate_right, color: Colors.white),
            ),
          ),
        ),
      ),
    );
  }

  Widget _inspectorTabs(
    bool editingEnabled, {
    required bool vertical,
    double verticalWidth = 76,
  }) {
    final strings = litchStrings(context);
    final tabs = <(LitchcraftSecondaryTray?, String, Widget, Key)>[
      (
        LitchcraftSecondaryTray.palette,
        strings.craftColor,
        const LitchcraftIdentityGlyph(size: 28),
        const ValueKey('craft-palette'),
      ),
      (
        LitchcraftSecondaryTray.leds,
        strings.craftLeds,
        const LitchcraftLedsGlyph(size: 28),
        const ValueKey('craft-leds'),
      ),
      (
        LitchcraftSecondaryTray.effects,
        strings.craftEffectsTab,
        const LitchcraftEffectsGlyph(size: 28),
        const ValueKey('craft-motion'),
      ),
      (
        LitchcraftSecondaryTray.animation,
        strings.categoryAnimation,
        LitchcraftAnimationGlyph(
          size: 28,
          playing: _usesBoardPlayback
              ? controller.animationPlaying
              : _previewTicker.isActive,
        ),
        const ValueKey('craft-animate'),
      ),
      (
        null,
        strings.craftLibrary,
        const LitchcraftArtworkGlyph(size: 28),
        const ValueKey('craft-artwork-menu'),
      ),
    ];
    Widget tabButton(
      LitchcraftSecondaryTray? tab,
      String label,
      Widget icon,
      Key key, {
      bool compactVertical = false,
    }) {
      final artwork = tab == null;
      final selected = artwork
          ? _artworkContext
          : !_artworkContext && _activeSecondaryTray == tab;
      void activate() {
        if (artwork) {
          setState(() => _artworkContext = true);
        } else {
          _toggleSecondaryTray(tab);
        }
      }

      final VoidCallback? hold = !editingEnabled
          ? null
          : artwork
          ? () => unawaited(_newArtwork())
          : tab == LitchcraftSecondaryTray.animation
          ? _toggleAnimationPlayback
          : null;
      Widget interaction({required Widget child}) =>
          artwork || tab == LitchcraftSecondaryTray.animation
          ? LitchcraftHoldAction(
              duration: artwork
                  ? const Duration(milliseconds: 750)
                  : kLongPressTimeout,
              holdLabel: artwork
                  ? strings.craftArtworkHoldHint
                  : strings.craftFramesHoldHint,
              completedLabel: '',
              showCompletionSnackBar: false,
              reserveProgressSpace: false,
              onTap: activate,
              onHold: hold,
              child: child,
            )
          : InkWell(onTap: activate, child: child);

      return Semantics(
        selected: selected,
        button: true,
        enabled: true,
        onTap: activate,
        onLongPress: LitchcraftHoldAction.withCompletionFeedback(context, hold),
        hint: hold == null
            ? null
            : artwork
            ? strings.craftArtworkHoldHint
            : tab == LitchcraftSecondaryTray.animation
            ? strings.craftFramesHoldHint
            : null,
        label: label,
        excludeSemantics: true,
        child: Tooltip(
          message: label,
          triggerMode: artwork || tab == LitchcraftSecondaryTray.animation
              ? TooltipTriggerMode.manual
              : null,
          excludeFromSemantics: true,
          child: KeyedSubtree(
            key: key,
            child: SizedBox(
              key: ValueKey(
                artwork
                    ? 'craft-inspector-tab-artwork'
                    : 'craft-inspector-tab-${tab.name}',
              ),
              width: vertical ? verticalWidth : null,
              height: vertical
                  ? null
                  : _tightPortraitLayout
                  ? 48
                  : 54,
              child: Material(
                color: Colors.transparent,
                child: SizedBox.expand(
                  child: interaction(
                    child: Padding(
                      padding: EdgeInsets.symmetric(
                        horizontal: vertical ? 3 : 0,
                        vertical: !vertical && _tightPortraitLayout ? 1 : 2,
                      ),
                      child: LayoutBuilder(
                        builder: (context, cell) {
                          final preferredWell = vertical
                              ? (compactVertical ? 34.0 : 48.0)
                              : 40.0;
                          final well = math.min(
                            preferredWell,
                            math.max(30.0, cell.maxHeight - 4),
                          );
                          return Column(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: <Widget>[
                              Container(
                                width: well,
                                height: well,
                                alignment: Alignment.center,
                                decoration: BoxDecoration(
                                  color: selected
                                      ? const Color(0xffc6e5e4)
                                      : Colors.transparent,
                                  borderRadius: BorderRadius.circular(7),
                                ),
                                child: IconTheme(
                                  data: IconThemeData(
                                    color: selected
                                        ? const Color(0xff263c40)
                                        : Colors.white,
                                  ),
                                  child: _presentationFlipTransition(
                                    key: ValueKey(
                                      'craft-toolbar-icon-transition-${artwork ? 'artwork' : tab.name}',
                                    ),
                                    child: icon,
                                  ),
                                ),
                              ),
                            ],
                          );
                        },
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

    if (vertical) {
      return LayoutBuilder(
        builder: (context, bounds) {
          final compactVertical = bounds.maxHeight < 272;
          return GestureDetector(
            behavior: HitTestBehavior.opaque,
            onHorizontalDragStart: (_) {},
            onHorizontalDragUpdate: (_) {},
            child: Material(
              key: const ValueKey('craft-toolbar'),
              color: const Color(0xff33494e),
              child: Column(
                key: const ValueKey('craft-tool-grid'),
                children: <Widget>[
                  for (final tab in tabs)
                    Expanded(
                      child: tabButton(
                        tab.$1,
                        tab.$2,
                        tab.$3,
                        tab.$4,
                        compactVertical: compactVertical,
                      ),
                    ),
                ],
              ),
            ),
          );
        },
      );
    }
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onHorizontalDragStart: (_) {},
      onHorizontalDragUpdate: (_) {},
      child: Material(
        key: const ValueKey('craft-toolbar'),
        color: const Color(0xff33494e),
        child: Row(
          key: const ValueKey('craft-tool-grid'),
          children: <Widget>[
            for (final tab in tabs)
              Expanded(child: tabButton(tab.$1, tab.$2, tab.$3, tab.$4)),
          ],
        ),
      ),
    );
  }

  Widget _inspectorContent(
    bool editingEnabled, {
    bool portrait = false,
    bool includeHistoryFooter = false,
  }) {
    return Column(
      children: [
        Expanded(
          child: Padding(
            padding: portrait
                ? EdgeInsets.symmetric(
                    horizontal: _compactPortraitAccessibility ? 0 : 6,
                  )
                : const EdgeInsets.fromLTRB(6, 6, 6, 10),
            child: KeyedSubtree(
              key: const ValueKey('craft-inspector-content'),
              child: _artworkContext
                  ? _artworkInspector(portrait: portrait)
                  : _secondaryToolbar(editingEnabled, portrait: portrait),
            ),
          ),
        ),
        if (_artworkContext ||
            _activeSecondaryTray != LitchcraftSecondaryTray.palette)
          Padding(
            padding: EdgeInsets.symmetric(
              horizontal: _compactPortraitAccessibility ? 0 : 6,
            ),
            child: const Divider(
              key: ValueKey('craft-inspector-divider'),
              height: 1,
              color: Colors.white24,
            ),
          ),
        if (includeHistoryFooter)
          _historyFooter(editingEnabled, portrait: portrait),
      ],
    );
  }

  Widget _historyFooter(
    bool editingEnabled, {
    bool portrait = false,
    bool? tabletDockOnLeft,
    VoidCallback? moveControls,
  }) {
    final strings = litchStrings(context);
    Widget historyAction({required bool undo, double extent = 48}) =>
        IconButton(
          key: ValueKey(undo ? 'craft-undo' : 'craft-redo'),
          tooltip: undo ? strings.undoAction : strings.craftRedo,
          onPressed:
              editingEnabled &&
                  (controller.advancedBoardAuthorityLive ||
                          (controller.usesFrameStoreDocument &&
                              controller.status == LitchcraftStatus.live)
                      ? (undo ? controller.canUndo : controller.canRedo)
                      : (undo
                            ? library.canUndoAnimation || controller.canUndo
                            : library.canRedoAnimation || controller.canRedo))
              ? (undo ? _undo : _redo)
              : null,
          icon: Icon(undo ? Icons.undo : Icons.redo),
          style: IconButton.styleFrom(
            minimumSize: Size(extent, 48),
            foregroundColor: Colors.white,
            disabledForegroundColor: Colors.white38,
          ),
        );
    return LayoutBuilder(
      builder: (context, bounds) {
        final count =
            3 +
            (portrait && widget.portraitBoardStatus != null ? 1 : 0) +
            (moveControls != null ? 1 : 0);
        final extent = math.min(48.0, bounds.maxWidth / count);
        return SizedBox(
          key: const ValueKey('craft-history-footer'),
          height: 48,
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            textDirection: tabletDockOnLeft == null
                ? null
                : tabletDockOnLeft
                ? TextDirection.ltr
                : TextDirection.rtl,
            children: [
              SizedBox(
                width: extent,
                child: historyAction(undo: true, extent: extent),
              ),
              SizedBox(
                width: extent,
                child: historyAction(undo: false, extent: extent),
              ),
              SizedBox(
                width: extent,
                child: FittedBox(fit: BoxFit.scaleDown, child: _rotateTool()),
              ),
              if (moveControls != null && tabletDockOnLeft != null)
                IconButton(
                  key: const ValueKey('craft-move-tablet-controls'),
                  tooltip: tabletDockOnLeft
                      ? strings.craftDockRight
                      : strings.craftDockLeft,
                  onPressed: moveControls,
                  icon: Icon(
                    tabletDockOnLeft ? Icons.arrow_forward : Icons.arrow_back,
                    textDirection: TextDirection.ltr,
                  ),
                  style: IconButton.styleFrom(
                    minimumSize: const Size(48, 48),
                    foregroundColor: Colors.white,
                  ),
                ),
              if (portrait && widget.portraitBoardStatus != null)
                SizedBox.square(
                  dimension: extent,
                  child: widget.portraitBoardStatus,
                ),
            ],
          ),
        );
      },
    );
  }

  Widget _artworkInspector({required bool portrait, bool tablet = false}) {
    final strings = litchStrings(context);
    final name = library.activeName ?? strings.craftDefaultName;
    Widget action(
      String key,
      String label,
      IconData icon,
      VoidCallback? onPressed, {
      bool inline = false,
    }) => LitchCompactControls(
      child: LayoutBuilder(
        builder: (context, bounds) {
          return TextButton(
            key: ValueKey(key),
            onPressed: onPressed,
            style: TextButton.styleFrom(
              minimumSize: const Size(48, 48),
              foregroundColor: Colors.white,
              padding: const EdgeInsets.symmetric(horizontal: 8),
              alignment: Alignment.center,
            ),
            child: tablet
                ? Tooltip(message: label, child: Icon(icon, size: 22))
                : inline
                ? Row(
                    children: [
                      Icon(icon, size: 20),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(
                          label,
                          style: const TextStyle(fontSize: 12, height: 1),
                        ),
                      ),
                    ],
                  )
                : Column(
                    mainAxisSize: MainAxisSize.min,
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(icon, size: 20),
                      const SizedBox(height: 2),
                      Text(
                        label,
                        textAlign: TextAlign.center,
                        style: const TextStyle(fontSize: 12, height: 1),
                      ),
                    ],
                  ),
          );
        },
      ),
    );
    return LayoutBuilder(
      builder: (context, bounds) {
        final labelWidth = math.max(1.0, bounds.maxWidth / 3 - 16);
        bool labelFits(String label) =>
            label.split(RegExp(r'\s+')).every((word) {
              final painter = TextPainter(
                text: TextSpan(
                  text: word,
                  style: Theme.of(
                    context,
                  ).textTheme.labelLarge!.copyWith(fontSize: 12, height: 1),
                ),
                textDirection: Directionality.of(context),
                textScaler: LitchCompactControls.textScalerOf(context),
                locale: Localizations.localeOf(context),
              )..layout();
              final fits = painter.width <= labelWidth;
              painter.dispose();
              return fits;
            });
        final stackActions =
            (!portrait && bounds.maxHeight >= 200) ||
            ![
              strings.craftSave,
              strings.craftExportShare,
              strings.craftImport,
            ].every(labelFits);
        final selector = Tooltip(
          message: '${strings.craftOpenSaved}: $name',
          child: TextButton(
            key: const ValueKey('craft-library'),
            onPressed: _working ? null : () => unawaited(_showLibrary()),
            onLongPress:
                tablet && !_working && !controller.boardArtworkUnresolved
                ? () => unawaited(_newArtwork())
                : null,
            style: TextButton.styleFrom(
              minimumSize: const Size(48, 48),
              foregroundColor: Colors.white,
              backgroundColor: Colors.white.withValues(alpha: .08),
              padding: const EdgeInsets.symmetric(horizontal: 10),
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(12),
              ),
            ),
            child: Semantics(
              label: '$name, ${strings.craftOpenSaved}',
              excludeSemantics: true,
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 14,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                  const SizedBox(width: 4),
                  const Icon(Icons.unfold_more, size: 20),
                ],
              ),
            ),
          ),
        );
        final actions = [
          action(
            'craft-save',
            strings.craftSave,
            Icons.save_outlined,
            _localActions.contains('save') ? null : () => unawaited(_save()),
            inline: stackActions,
          ),
          Builder(
            builder: (shareContext) => action(
              'craft-share',
              strings.craftExportShare,
              Icons.ios_share_outlined,
              _sharing ? null : () => unawaited(_share(shareContext)),
              inline: stackActions,
            ),
          ),
          action(
            'craft-import',
            strings.craftImport,
            Icons.file_open_outlined,
            _importing || _working || !library.canEditAnimation
                ? null
                : () => unawaited(_importArtwork()),
            inline: stackActions,
          ),
        ];
        if (tablet) {
          return Row(
            key: const ValueKey('craft-artwork-panel'),
            children: [
              Expanded(child: selector),
              ...actions.map((action) => SizedBox(width: 48, child: action)),
            ],
          );
        }
        return SingleChildScrollView(
          child: Column(
            key: const ValueKey('craft-artwork-panel'),
            mainAxisAlignment: stackActions
                ? MainAxisAlignment.start
                : MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              selector,
              SizedBox(height: stackActions ? 8 : 4),
              if (stackActions)
                ...actions
              else
                Row(
                  children: [for (final item in actions) Expanded(child: item)],
                ),
            ],
          ),
        );
      },
    );
  }

  Widget _colorInspector(bool editingEnabled, {bool tablet = false}) {
    return Column(
      key: const ValueKey('craft-color-panel'),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Expanded(
          child: LitchcraftColorInspector(
            color: controller.color,
            previousColor: controller.previousColor,
            recentColors: library.favoriteColors,
            enabled: editingEnabled,
            spacious:
                tablet ||
                MediaQuery.sizeOf(context).width >
                    MediaQuery.sizeOf(context).height,
            onPreviousColor: controller.canSwapBrushColor
                ? () => unawaited(controller.swapBrushColor())
                : null,
            onPreviewStarted: controller.beginBrushColorPreview,
            onPreviewCancelled: controller.cancelBrushColorPreview,
            onPreviewChanged: (color) {
              _recordCommandInteraction();
              setState(() {
                _pickingColor = false;
                _effectTool = null;
              });
              unawaited(controller.previewBrushColor(color));
            },
            onChanged: (color) {
              _recordCommandInteraction();
              setState(() {
                _pickingColor = false;
                _effectTool = null;
              });
              unawaited(controller.commitBrushColorPreview(color));
            },
            onHexPadVisibilityChanged: (visible) {
              _hexPadVisible = visible;
              _cancelPhoneShakeQualification();
            },
          ),
        ),
      ],
    );
  }

  Widget _ledInspector(bool editingEnabled) {
    Widget target(int mask) {
      final selected = controller.targetMask == mask;
      return SizedBox.square(
        dimension: 48,
        child: IconButton(
          key: ValueKey('craft-target-$mask'),
          tooltip: litchcraftTargetLabel(
            context,
            mask,
            flipped: controller.flipped,
          ),
          onPressed: editingEnabled
              ? () => unawaited(_applyTarget(mask))
              : null,
          isSelected: selected,
          style: IconButton.styleFrom(
            minimumSize: const Size.square(48),
            padding: const EdgeInsets.all(6),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(7),
            ),
            foregroundColor: selected ? const Color(0xff162629) : Colors.white,
            backgroundColor: selected ? const Color(0xffc6e5e4) : null,
          ),
          icon: Stack(
            clipBehavior: Clip.none,
            children: [
              LitchcraftTargetIcon(
                mask: mask,
                topology: controller.canvas.topology,
                flipped: controller.flipped,
                color: controller.color,
                size: 36,
              ),
              if (selected)
                const Positioned(
                  right: -3,
                  bottom: -3,
                  child: Icon(
                    Icons.check_rounded,
                    size: 14,
                    color: Color(0xff162629),
                  ),
                ),
            ],
          ),
        ),
      );
    }

    return Center(
      child: Column(
        key: const ValueKey('craft-led-panel'),
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            litchStrings(context).craftTargetPrompt,
            key: const ValueKey('craft-target-prompt'),
            textAlign: TextAlign.center,
            style: const TextStyle(
              color: Colors.white70,
              fontSize: 11,
              height: 1,
            ),
          ),
          const SizedBox(height: 6),
          Wrap(
            alignment: WrapAlignment.center,
            spacing: 4,
            runSpacing: 4,
            children: [
              for (final mask in [...controller.canvas.topology.targets, 15])
                target(mask),
            ],
          ),
        ],
      ),
    );
  }

  Widget _effectsInspector(bool editingEnabled, {bool tablet = false}) =>
      LitchcraftEffectEditorPanel(
        selectedEffect: tablet
            ? _effectTool
            : (_effectTool ?? LitchcraftEffectStyle.off),
        effectDurationMs: _effectDurationMs,
        enabled:
            editingEnabled &&
            _canDirectlyEditAnimation &&
            !controller.waitingForLift,
        onEffectChanged: (effect) => setState(() {
          _pickingColor = false;
          _effectTool = effect;
        }),
        onDurationChanged: (duration) =>
            setState(() => _effectDurationMs = duration),
      );

  Widget _animationInspector(
    bool editingEnabled, {
    required bool landscape,
    bool tablet = false,
    bool? dockOnLeft,
  }) {
    final strings = litchStrings(context);
    final document = library.animationDocument;
    final playing = _usesBoardPlayback
        ? controller.animationPlaying
        : _previewTicker.isActive;
    final displayedFrameIndex = playing
        ? litchcraftAnimationFrameIndexAt(
            document,
            _usesBoardPlayback
                ? controller.animationElapsedMilliseconds
                : _previewElapsedMs,
          )
        : library.selectedFrameIndex;
    void addFrame() {
      final target = controller.captureAnimationFrameEditTarget(
        frame: displayedFrameIndex,
      );
      if (target == null) return;
      _runInBackground(() async {
        final applied = await controller.addAnimationFrame(target: target);
        if (mounted && applied && !_usesBoardPlayback) {
          _seekPreviewToSelectedFrame();
        }
      });
    }

    final content = Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        if (tablet)
          Padding(
            padding: const EdgeInsets.fromLTRB(4, 0, 4, 6),
            child: Semantics(
              header: true,
              child: Text(
                strings.craftFrameLabel(
                  displayedFrameIndex + 1,
                  document.frames.length,
                ),
                style: Theme.of(
                  context,
                ).textTheme.titleSmall?.copyWith(color: Colors.white),
              ),
            ),
          ),
        LitchcraftAnimationBar(
          document: document,
          selectedFrameIndex: displayedFrameIndex,
          playing: playing,
          playbackPending: controller.animationPlaybackPending,
          enabled: editingEnabled,
          frameEditingEnabled: _canDirectlyEditAnimation,
          motionEnabled: editingEnabled && _canDirectlyEditAnimation,
          playbackVisible:
              controller.status != LitchcraftStatus.conflict &&
              _hasAnimationMotion(document),
          motionLabel: strings.craftEffectsTab,
          playLabel: _usesBoardPlayback
              ? controller.animationPlaybackPending
                    ? strings.craftCancelQueuedPlay
                    : strings.craftPlayBoard
              : strings.craftPlayPreview,
          pauseLabel: _usesBoardPlayback
              ? strings.craftPauseBoard
              : strings.craftPausePreview,
          previousFrameLabel: MaterialLocalizations.of(
            context,
          ).previousPageTooltip,
          nextFrameLabel: MaterialLocalizations.of(context).nextPageTooltip,
          addFrameLabel: strings.craftAddFrame,
          deleteFrameLabel: strings.craftDeleteFrame,
          motionDisabledHint: strings.craftLiveMotionEditDisabled,
          frameLabel: strings.craftFrameLabel,
          onOpenEffects: () => tablet
              ? _revealTabletSection(_tabletEffectsKey)
              : _toggleSecondaryTray(LitchcraftSecondaryTray.effects),
          onOpenFrameDetails: () {},
          onTogglePlayback: _toggleAnimationPlayback,
          onSelectFrame: _selectAnimationFrame,
          onAddFrame: addFrame,
          onDeleteFrame: () => unawaited(_deleteSelectedFrame()),
          onFrameLongPress: (index, _) => unawaited(_deleteFrameAt(index)),
          expanded: landscape,
          spacious: tablet,
          dockOnLeft: dockOnLeft,
          motionKey: const ValueKey('craft-animation-effects'),
        ),
        LitchcraftFrameEditorPanel(
          extendTrack: landscape,
          document: document,
          selectedFrameIndex: displayedFrameIndex,
          enabled: _canDirectlyEditAnimation,
          onDurationChangeStarted: () {
            _frameDurationTarget = controller.captureAnimationFrameEditTarget(
              frame: displayedFrameIndex,
            );
          },
          onDurationChanged: _setFrameDuration,
          onDurationPreview: (duration) =>
              _showFrameDurationStatus(duration, announce: false),
          onDuplicate: addFrame,
          onDelete: () => unawaited(_deleteSelectedFrame()),
          onOpenEffects: () => tablet
              ? _revealTabletSection(_tabletEffectsKey)
              : _toggleSecondaryTray(LitchcraftSecondaryTray.effects),
        ),
      ],
    );
    if (tablet) {
      return KeyedSubtree(
        key: const ValueKey('craft-animation-panel'),
        child: content,
      );
    }
    return Center(
      child: KeyedSubtree(
        key: const ValueKey('craft-animation-panel'),
        child: SingleChildScrollView(child: content),
      ),
    );
  }

  Future<void> _deleteFrameAt(int index) async {
    if (_working || !_canDirectlyEditAnimation) return;
    _stopPreview();
    final target = controller.captureAnimationFrameEditTarget(frame: index);
    if (target == null || !mounted) return;
    if (!_usesBoardPlayback) {
      library.selectFrame(index);
      _seekPreviewToSelectedFrame();
    }
    await _deleteSelectedFrame(target: target);
  }

  Widget _secondaryToolbar(bool editingEnabled, {required bool portrait}) {
    return switch (_activeSecondaryTray) {
      LitchcraftSecondaryTray.palette => _colorInspector(editingEnabled),
      LitchcraftSecondaryTray.leds => _ledInspector(editingEnabled),
      LitchcraftSecondaryTray.effects => _effectsInspector(editingEnabled),
      LitchcraftSecondaryTray.animation => _animationInspector(
        editingEnabled,
        landscape: !portrait,
      ),
    };
  }

  Future<void> _deleteSelectedFrame({LitchcraftFrameEditTarget? target}) async {
    final exactTarget = target ?? controller.captureAnimationFrameEditTarget();
    if (exactTarget == null || !mounted) return;
    final deleted = exactTarget.frame + 1;
    if (!await _confirmDeleteFrame(context, deleted) || !mounted) return;
    final before = library.animationDocument.frames.length;
    final clearingOnly = before == 1;
    var accepted = false;
    await _runLocal('frame-delete', () async {
      accepted = await controller.deleteAnimationFrame(target: exactTarget);
    });
    if (mounted && accepted && !_usesBoardPlayback) {
      _seekPreviewToSelectedFrame();
    }
    if (!mounted || !accepted) {
      return;
    }
    if (clearingOnly) {
      _showToolStatus(litchStrings(context).craftAllErased);
      return;
    }
    if (library.animationDocument.frames.length >= before) return;
    _showToolStatus(
      litchStrings(context).craftFrameDeleted(
        deleted,
        library.selectedFrameIndex + 1,
        library.animationDocument.frames.length,
      ),
    );
  }

  void _undo() {
    _recordCommandInteraction();
    setState(() => _effectTool = null);
    if (!controller.advancedBoardAuthorityLive &&
        !(controller.usesFrameStoreDocument &&
            controller.status == LitchcraftStatus.live) &&
        library.canUndoAnimation) {
      library.undoAnimation();
    } else {
      unawaited(controller.undo());
    }
  }

  void _redo() {
    _recordCommandInteraction();
    setState(() => _effectTool = null);
    if (!controller.advancedBoardAuthorityLive &&
        !(controller.usesFrameStoreDocument &&
            controller.status == LitchcraftStatus.live) &&
        library.canRedoAnimation) {
      library.redoAnimation();
    } else {
      unawaited(controller.redo());
    }
  }

  void _selectBrushMode(bool selectingErase) {
    _recordCommandInteraction();
    setState(() {
      _pickingColor = false;
      _effectTool = null;
    });
    unawaited(controller.setBrush(eraser: selectingErase));
    _showToolStatus(
      selectingErase
          ? litchStrings(context).craftEraseMode
          : litchStrings(context).craftPaintMode,
    );
  }

  void _pickSquareColor(int square) {
    final colors = controller.canvas.colors.sublist(square * 4, square * 4 + 4);
    final color = switch (controller.targetMask) {
      1 => colors[0],
      2 => colors[1],
      4 => colors[2],
      8 => colors[3],
      10 => litchcraftLegacyCenterColor(colors[1], colors[3]),
      _ => colors.firstWhere((value) => value != 0, orElse: () => 0),
    };
    setState(() {
      _pickingColor = false;
      _effectTool = null;
    });
    unawaited(
      controller.setBrush(color: color == 0 ? null : color, eraser: color == 0),
    );
  }

  Future<void> _applyTarget(int mask) async {
    _recordCommandInteraction();
    await controller.setBrush(targetMask: mask);
    if (!mounted) return;
    _showToolStatus(
      '${litchcraftTargetLabel(context, mask, flipped: controller.flipped)}. '
      '${litchStrings(context).craftDetailHelp}',
    );
  }

  String _connectionText(LitchcraftStatus status) {
    final strings = litchStrings(context);
    return switch (status) {
      LitchcraftStatus.live => '',
      LitchcraftStatus.unsupported => strings.craftUnsupported,
      LitchcraftStatus.gameActive => strings.craftGameActive,
      LitchcraftStatus.boardNotEmpty => strings.craftBoardNotEmpty,
      LitchcraftStatus.connecting => '',
      LitchcraftStatus.conflict => strings.craftConflict,
      LitchcraftStatus.error => strings.craftSyncError,
      LitchcraftStatus.offline => strings.craftOffline,
    };
  }

  List<String> _idleTips() {
    final strings = litchStrings(context);
    return <String>[
      strings.craftIdleTip1,
      strings.craftIdleTip2,
      strings.craftIdleTip3,
      strings.craftIdleTip4,
      strings.craftIdleTip5,
      strings.craftIdleTip6,
      strings.craftIdleTip7,
      strings.craftIdleTip8,
      strings.craftIdleTip9,
      strings.craftIdleTip10,
      strings.craftIdleTip11,
      strings.craftIdleTip12,
      strings.craftIdleTip13,
      strings.craftIdleTip14,
      strings.craftIdleTip15,
      strings.craftIdleTip16,
      strings.craftIdleTip17,
      strings.craftIdleTip18,
      strings.craftIdleTip19,
      strings.craftIdleTip20,
      strings.craftIdleTip21,
      strings.craftIdleTip22,
      strings.craftIdleTip23,
      strings.craftIdleTip24,
      strings.craftIdleTip25,
      strings.craftIdleTip26,
      strings.craftIdleTip27,
      strings.craftIdleTip28,
      strings.craftIdleTip29,
      strings.craftIdleTip30,
      strings.craftIdleTip31,
      strings.craftIdleTip32,
      strings.craftIdleTip33,
      strings.craftIdleTip34,
      strings.craftIdleTip35,
      strings.craftIdleTip36,
      strings.craftIdleTip37,
      strings.craftIdleTip38,
      strings.craftIdleTip39,
      strings.craftIdleTip40,
      if (controller.shakeEraseSupported) strings.craftIdleTipShake,
    ];
  }

  void _showToolStatus(String message, {bool showUndo = false}) {
    _toolStatusTimer?.cancel();
    _idleTipDelayTimer?.cancel();
    _idleTipVisibleTimer?.cancel();
    setState(() {
      _idleTipVisible = false;
      _toolStatus = message;
      _toolStatusShowsUndo = showUndo;
    });
    _toolStatusTimer = Timer(const Duration(seconds: 3), () {
      _toolStatusTimer = null;
      if (!mounted) return;
      setState(() {
        _toolStatus = null;
        _toolStatusShowsUndo = false;
      });
      _scheduleIdleTip();
    });
  }

  void _setFrameDuration(int durationMs) {
    final target = _frameDurationTarget;
    _frameDurationTarget = null;
    // A refused pointer-down target must never be recaptured at release.
    if (target == null) return;
    _runInBackground(() async {
      final applied = await controller.setAnimationFrameDuration(
        durationMs,
        target: target,
      );
      if (mounted && applied) _showFrameDurationStatus(durationMs);
    });
  }

  void _showFrameDurationStatus(int durationMs, {bool announce = true}) {
    _frameTimingStatusTimer?.cancel();
    final value = litchcraftDurationSeconds(durationMs);
    setState(() {
      _frameTimingStatus = litchStrings(context).craftFrameDuration(value);
      _frameTimingStatusAnnounces = announce;
    });
    _frameTimingStatusTimer = Timer(const Duration(milliseconds: 1400), () {
      _frameTimingStatusTimer = null;
      if (!mounted) return;
      setState(() => _frameTimingStatus = null);
    });
  }

  // The chooser stays unresolved until verified completion, but its active
  // transfer has already received the user's source decision.
  bool get _artworkChoiceSyncInProgress =>
      controller.status == LitchcraftStatus.conflict &&
      _working &&
      controller.fullAnimationSyncDirection != null;

  bool _hasPersistentStatus() {
    if (!widget.active) return false;
    final status = controller.status;
    if (status == LitchcraftStatus.connecting &&
        controller.usesFrameStoreDocument) {
      return true;
    }
    if (status == LitchcraftStatus.conflict && _conflictSheetVisible) {
      return false;
    }
    final requiredConnection =
        !_artworkChoiceSyncInProgress &&
        !_storeOpenRecoveryPending &&
        ((controller.boardArtworkUnresolved &&
                status != LitchcraftStatus.live &&
                status != LitchcraftStatus.connecting) ||
            (status != LitchcraftStatus.live &&
                status != LitchcraftStatus.offline &&
                status != LitchcraftStatus.connecting &&
                (status != LitchcraftStatus.error ||
                    controller.connectionRecoveryRequired ||
                    controller.storeArtworkUnavailable) &&
                status != LitchcraftStatus.boardNotEmpty));
    final libraryWarning =
        library.saveFailed || library.recovered || library.rejected;
    return controller.gameEntryRecoveryRequired ||
        _storeOpenRecoveryPending ||
        controller.artworkReadbackPending ||
        controller.boardArtworkLoading ||
        controller.storeRootPublicationPending ||
        controller.animationPlaybackPending ||
        controller.animationPlaybackFailed ||
        _pickingColor ||
        requiredConnection ||
        (status == LitchcraftStatus.live &&
            (controller.waitingForLift ||
                _showPhysicalPickInstruction ||
                controller.physicalColorPicked ||
                controller.physicalShakeErased)) ||
        libraryWarning;
  }

  bool get _urgentCanvasStatus =>
      (!_artworkChoiceSyncInProgress &&
          !_storeOpenRecoveryPending &&
          controller.status != LitchcraftStatus.live &&
          controller.status != LitchcraftStatus.connecting) ||
      controller.connectionRecoveryRequired ||
      controller.animationPlaybackPending ||
      (controller.fullAnimationSyncDirection == null && _readingBoardArtwork) ||
      controller.storeArtworkUnavailable ||
      controller.animationPlaybackFailed ||
      _boardArtworkReadFailed ||
      _pickingColor ||
      controller.waitingForLift ||
      _showPhysicalPickInstruction ||
      controller.physicalColorPicked ||
      controller.physicalShakeErased ||
      library.saveFailed ||
      library.recovered ||
      library.rejected;

  Widget _statusOverlay({double elevation = 4}) {
    final duration = MediaQuery.disableAnimationsOf(context)
        ? Duration.zero
        : const Duration(milliseconds: 180);
    return FocusTraversalOrder(
      order: const NumericFocusOrder(3),
      child: LayoutBuilder(
        builder: (context, bounds) => _ArtworkSyncPresentation(
          key: ValueKey((
            controller,
            controller.documentSession.replacementGeneration,
            controller.bleService.connectedId,
            controller.bleService.connectionSessionToken,
          )),
          active: widget.active,
          isUrgent: () => _urgentCanvasStatus,
          isSyncing: () => _artworkSyncActive,
          syncChild: _status(
            elevation: elevation,
            compact: bounds.maxWidth < 160,
          ),
          regularChild: _status(
            elevation: elevation,
            compact: bounds.maxWidth < 160,
            showSync: false,
          ),
          builder: (child) => child == null
              ? const SizedBox.shrink()
              : KeyedSubtree(
                  key: const ValueKey('craft-canvas-toast-position'),
                  child: AnimatedSwitcher(
                    duration: duration,
                    reverseDuration: Duration.zero,
                    transitionBuilder: (child, animation) {
                      final faded = FadeTransition(
                        opacity: animation,
                        child: child,
                      );
                      return SlideTransition(
                        position: Tween<Offset>(
                          begin: const Offset(0, .16),
                          end: Offset.zero,
                        ).animate(animation),
                        child: faded,
                      );
                    },
                    child: child,
                  ),
                ),
        ),
      ),
    );
  }

  Widget? _status({
    int? maxLines,
    double elevation = 4,
    bool compact = false,
    bool showSync = true,
  }) {
    final strings = litchStrings(context);
    final status = controller.status;
    final includeSync = showSync && !_urgentCanvasStatus;
    final rootPublicationPending =
        includeSync && controller.storeRootPublicationPending;
    final openRecoveryPending = includeSync && _storeOpenRecoveryPending;
    final fullSyncDirection = includeSync
        ? controller.fullAnimationSyncDirection
        : null;
    final fullSyncProgress = controller.fullAnimationSyncProgress;
    final libraryWarning =
        library.saveFailed || library.recovered || library.rejected;
    final waitingForLift =
        status == LitchcraftStatus.live && controller.waitingForLift;
    final artworkReadbackPending =
        includeSync &&
        status == LitchcraftStatus.live &&
        controller.artworkReadbackPending &&
        !waitingForLift;
    final physicalColorPicking =
        status == LitchcraftStatus.live && _showPhysicalPickInstruction;
    final physicalColorPicked =
        status == LitchcraftStatus.live && controller.physicalColorPicked;
    final physicalShakeErased =
        status == LitchcraftStatus.live && controller.physicalShakeErased;
    final requiredConnection =
        !_artworkChoiceSyncInProgress &&
        !openRecoveryPending &&
        ((controller.boardArtworkUnresolved &&
                status != LitchcraftStatus.live &&
                status != LitchcraftStatus.connecting) ||
            (status != LitchcraftStatus.live &&
                status != LitchcraftStatus.offline &&
                status != LitchcraftStatus.connecting &&
                (status != LitchcraftStatus.error ||
                    controller.connectionRecoveryRequired ||
                    controller.storeArtworkUnavailable) &&
                status != LitchcraftStatus.boardNotEmpty));
    final showLibraryWarning =
        libraryWarning &&
        !physicalColorPicking &&
        !physicalColorPicked &&
        !physicalShakeErased &&
        !waitingForLift &&
        !requiredConnection;
    final readingArtwork =
        (includeSync || _readingBoardArtwork) &&
        (_readingBoardArtwork || controller.boardArtworkLoading) &&
        (status == LitchcraftStatus.live ||
            status == LitchcraftStatus.connecting ||
            (status == LitchcraftStatus.conflict && _readingBoardArtwork)) &&
        (!requiredConnection ||
            (status == LitchcraftStatus.conflict && _readingBoardArtwork)) &&
        !showLibraryWarning &&
        !controller.storeArtworkUnavailable &&
        !controller.animationPlaybackFailed;
    final String? urgentMessage = controller.gameEntryRecoveryRequired
        ? strings.craftGameEntryFailed
        : controller.storeArtworkUnavailable
        ? strings.craftStoreUnavailable
        : status == LitchcraftStatus.conflict && _boardArtworkReadFailed
        ? strings.craftReadBoardFailed
        : requiredConnection &&
              !(status == LitchcraftStatus.conflict && _readingBoardArtwork)
        ? _connectionText(status)
        : controller.animationPlaybackFailed
        ? strings.craftActionFailed
        : showLibraryWarning
        ? library.saveFailed
              ? library.storageLimitExceeded
                    ? strings.craftLibraryFull
                    : strings.craftSaveFailed
              : library.recovered
              ? strings.craftRecovered
              : strings.craftCorrupt
        : null;
    final String? message =
        urgentMessage ??
        (fullSyncDirection == LitchcraftFullSyncDirection.sendingToBoard
            ? strings.craftSendingAnimationToBoard
            : fullSyncDirection == LitchcraftFullSyncDirection.readingFromBoard
            ? strings.craftReadingAnimationFromBoard
            : rootPublicationPending || openRecoveryPending
            ? strings.craftSendingArtwork
            : includeSync && controller.preparingBoardArtwork
            ? strings.craftMediaBusy
            : readingArtwork
            ? strings.craftReadingBoardArtwork
            : controller.storeArtworkUnavailable
            ? strings.craftStoreUnavailable
            : status == LitchcraftStatus.conflict && _boardArtworkReadFailed
            ? strings.craftReadBoardFailed
            : controller.animationPlaybackPending
            ? strings.craftPlayWhenReady
            : controller.animationPlaybackFailed
            ? strings.craftActionFailed
            : artworkReadbackPending
            ? strings.syncingLabel
            : includeSync &&
                  status == LitchcraftStatus.connecting &&
                  controller.usesFrameStoreDocument
            ? strings.craftConnecting
            : null) ??
        (_hasPersistentStatus() ? null : _toolStatus) ??
        (requiredConnection
            ? _connectionText(status)
            : physicalShakeErased
            ? strings.craftAllErasedUndo
            : physicalColorPicking
            ? strings.craftPhysicalColorPicking
            : physicalColorPicked
            ? controller.eraser
                  ? strings.craftPhysicalErasePicked
                  : strings.craftPhysicalColorPicked
            : waitingForLift
            ? controller.animationBoardAuthorityLive
                  ? strings.craftBoardBusyRecovery
                  : strings.craftLiftBrush
            : library.saveFailed
            ? library.storageLimitExceeded
                  ? strings.craftLibraryFull
                  : strings.craftSaveFailed
            : library.recovered
            ? strings.craftRecovered
            : library.rejected
            ? strings.craftCorrupt
            : _pickingColor
            ? strings.craftPickColorHelp
            : _frameTimingStatus ??
                  (_tipsEnabled && !_animationPlaying && _idleTipVisible
                      ? !library.dismissedWelcome
                            ? strings.craftWelcome
                            : _idleTips()[_idleTipIndex]
                      : null));
    if (message == null) return null;
    final activityPending =
        !showLibraryWarning &&
        !controller.storeArtworkUnavailable &&
        !controller.animationPlaybackFailed &&
        status != LitchcraftStatus.offline &&
        status != LitchcraftStatus.boardNotEmpty &&
        (fullSyncDirection != null ||
            rootPublicationPending ||
            openRecoveryPending ||
            readingArtwork ||
            artworkReadbackPending ||
            status == LitchcraftStatus.connecting);
    final Widget? statusIcon = activityPending
        ? const SizedBox.square(
            key: ValueKey('craft-sync-activity'),
            dimension: 20,
            child: CircularProgressIndicator(strokeWidth: 2),
          )
        : showLibraryWarning || status == LitchcraftStatus.error
        ? const Icon(Icons.info_outline, size: 20)
        : fullSyncDirection == LitchcraftFullSyncDirection.sendingToBoard
        ? const Icon(Icons.upload_outlined, size: 20)
        : fullSyncDirection == LitchcraftFullSyncDirection.readingFromBoard
        ? const Icon(Icons.download_outlined, size: 20)
        : rootPublicationPending
        ? const Icon(Icons.upload_outlined, size: 20)
        : readingArtwork
        ? const Icon(Icons.download_outlined, size: 20)
        : controller.storeArtworkUnavailable
        ? const Icon(Icons.sync_problem_outlined, size: 20)
        : controller.animationPlaybackPending
        ? const Icon(Icons.hourglass_top_outlined, size: 20)
        : controller.animationPlaybackFailed
        ? const Icon(Icons.info_outline, size: 20)
        : artworkReadbackPending
        ? const Icon(Icons.hourglass_top_outlined, size: 20)
        : showLibraryWarning || status == LitchcraftStatus.error
        ? const Icon(Icons.info_outline, size: 20)
        : physicalShakeErased
        ? const LitchcraftEraserIcon(size: 20)
        : physicalColorPicking || physicalColorPicked
        ? const Icon(Icons.colorize, size: 20)
        : waitingForLift
        ? const Icon(Icons.pan_tool_alt_outlined, size: 20)
        : status == LitchcraftStatus.conflict && !_working
        ? const Icon(Icons.compare_arrows, size: 20)
        : _toolStatus != null
        ? _toolStatusShowsUndo
              ? const Icon(Icons.undo, size: 20)
              : controller.eraser
              ? const LitchcraftEraserIcon(size: 20)
              : const Icon(Icons.brush_outlined, size: 20)
        : _idleTipVisible
        ? const Icon(Icons.tips_and_updates_outlined, size: 20)
        : status == LitchcraftStatus.live
        ? null
        : const Icon(Icons.edit_outlined, size: 20);
    final announceChange =
        controller.gameEntryRecoveryRequired ||
        fullSyncDirection != null ||
        (status == LitchcraftStatus.connecting &&
            controller.usesFrameStoreDocument) ||
        rootPublicationPending ||
        openRecoveryPending ||
        readingArtwork ||
        controller.boardArtworkUnresolved ||
        controller.animationPlaybackPending ||
        controller.animationPlaybackFailed ||
        artworkReadbackPending ||
        requiredConnection ||
        physicalShakeErased ||
        physicalColorPicking ||
        physicalColorPicked ||
        waitingForLift ||
        showLibraryWarning ||
        _toolStatus != null ||
        (_frameTimingStatus != null && _frameTimingStatusAnnounces);
    final Widget? statusAction = controller.gameEntryRecoveryRequired
        ? TextButton(
            key: const ValueKey('craft-game-entry-retry'),
            onPressed: () => unawaited(controller.retryGameEntry()),
            child: Text(strings.tryAgain),
          )
        : controller.animationPlaybackPending
        ? IconButton.filledTonal(
            key: const ValueKey('craft-cancel-queued-play'),
            tooltip: strings.craftCancelQueuedPlay,
            onPressed: _toggleAnimationPlayback,
            icon: const Icon(Icons.close),
          )
        : controller.animationPlaybackFailed
        ? IconButton.filledTonal(
            key: const ValueKey('craft-retry-queued-play'),
            tooltip: strings.craftPlayBoard,
            onPressed: _toggleAnimationPlayback,
            icon: const Icon(Icons.play_arrow),
          )
        : controller.connectionRecoveryRequired
        ? IconButton.filledTonal(
            key: const ValueKey('craft-reconnect'),
            tooltip: strings.tryAgain,
            onPressed: () => unawaited(controller.retryConnection()),
            icon: const Icon(Icons.refresh),
          )
        : showLibraryWarning
        ? IconButton(
            tooltip: library.saveFailed
                ? strings.craftSave
                : strings.craftLibrary,
            onPressed: _working || _localActions.contains('save')
                ? null
                : library.saveFailed
                ? () => _save()
                : _showLibrary,
            icon: Icon(
              library.saveFailed
                  ? Icons.save_outlined
                  : Icons.collections_bookmark_outlined,
            ),
          )
        : status == LitchcraftStatus.conflict &&
              !_working &&
              fullSyncDirection == null &&
              !_readingBoardArtwork &&
              !controller.boardArtworkLoading
        ? IconButton.filledTonal(
            key: const ValueKey('craft-resolve-conflict'),
            tooltip: strings.craftConflict,
            onPressed: _showConflict,
            icon: const Icon(Icons.chevron_right),
          )
        : null;
    return Semantics(
      key: const ValueKey('craft-status-card'),
      container: true,
      explicitChildNodes: statusAction != null,
      liveRegion: announceChange,
      sortKey: const OrdinalSortKey(3),
      child: Stack(
        children: <Widget>[
          IgnorePointer(
            child: Material(
              elevation: elevation,
              shadowColor: Theme.of(context).colorScheme.shadow,
              color: Theme.of(
                context,
              ).colorScheme.surfaceContainerHigh.withValues(alpha: .92),
              borderRadius: BorderRadius.circular(12),
              child: ConstrainedBox(
                constraints: const BoxConstraints(minHeight: 48),
                child: Padding(
                  padding: EdgeInsets.fromLTRB(
                    compact ? 6 : 12,
                    6,
                    compact ? 6 : 12,
                    6,
                  ),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.center,
                    children: <Widget>[
                      if (statusIcon != null &&
                          (activityPending ||
                              (!compact &&
                                  (_toolStatus == null ||
                                      _toolStatusShowsUndo ||
                                      artworkReadbackPending)))) ...<Widget>[
                        IconTheme(
                          data: IconThemeData(
                            size: 20,
                            color:
                                showLibraryWarning ||
                                    status == LitchcraftStatus.error
                                ? Theme.of(context).colorScheme.error
                                : Theme.of(context).colorScheme.onSurface,
                          ),
                          child: statusIcon,
                        ),
                        const SizedBox(width: 8),
                      ],
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          mainAxisSize: MainAxisSize.min,
                          children: <Widget>[
                            Text(
                              message,
                              maxLines: compact ? 1 : maxLines,
                              overflow: !compact && maxLines == null
                                  ? TextOverflow.visible
                                  : TextOverflow.ellipsis,
                              style: Theme.of(context).textTheme.bodySmall,
                            ),
                            if (fullSyncDirection != null) ...<Widget>[
                              const SizedBox(height: 4),
                              LinearProgressIndicator(
                                key: const ValueKey(
                                  'craft-full-animation-sync',
                                ),
                                value: fullSyncProgress,
                                semanticsLabel: message,
                              ),
                            ] else if (rootPublicationPending ||
                                artworkReadbackPending) ...<Widget>[
                              const SizedBox(height: 4),
                              SizedBox(
                                height: 2,
                                child: LinearProgressIndicator(
                                  key: ValueKey(
                                    rootPublicationPending
                                        ? 'craft-store-root-pending'
                                        : 'craft-interactive-sync-pending',
                                  ),
                                  semanticsLabel: message,
                                ),
                              ),
                            ],
                          ],
                        ),
                      ),
                      if (statusAction != null)
                        SizedBox(width: compact ? 0 : 48),
                    ],
                  ),
                ),
              ),
            ),
          ),
          if (!compact && statusAction != null)
            Positioned(
              right: 12,
              top: 0,
              bottom: 0,
              width: 48,
              child: Center(child: statusAction),
            ),
        ],
      ),
    );
  }

  Future<void> _showConflict() async {
    if (_conflictSheetVisible || !mounted || !widget.active) return;
    final sheetController = controller;
    _reopenConflictSheet = false;
    _cancelPhoneShakeQualification();
    setState(() => _conflictSheetVisible = true);
    if (controller.frameStoreArtworkReadPending) {
      unawaited(controller.prepareConflictBoardPreview());
    }
    bool? useBoard;
    try {
      useBoard = await showModalBottomSheet<bool>(
        context: context,
        isScrollControlled: true,
        useSafeArea: true,
        showDragHandle: true,
        builder: (context) {
          _conflictSheetContext = context;
          return ListenableBuilder(
            listenable: controller,
            builder: (context, _) {
              final strings = litchStrings(context);
              final pending = controller.frameStoreArtworkReadPending;
              final preview = pending
                  ? controller.conflictBoardPreview
                  : controller.boardCanvas;
              final loading = pending && controller.conflictBoardPreviewLoading;
              final failed = pending && controller.conflictBoardPreviewFailed;
              final retry = pending && preview == null && !loading;
              final choiceReady =
                  identical(sheetController, controller) &&
                  controller.status == LitchcraftStatus.conflict &&
                  !controller.boardArtworkUnresolved;
              Widget choice({
                required Key key,
                required LitchcraftCanvas? canvas,
                required String label,
                required String explanation,
                required bool value,
              }) => Card(
                clipBehavior: Clip.antiAlias,
                margin: EdgeInsets.zero,
                child: InkWell(
                  key: key,
                  onTap: choiceReady
                      ? () {
                          if (identical(sheetController, controller) &&
                              controller.status == LitchcraftStatus.conflict &&
                              !controller.boardArtworkUnresolved) {
                            Navigator.of(context).pop(value);
                          }
                        }
                      : null,
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.center,
                      children: <Widget>[
                        SizedBox(
                          width: 112,
                          height: 112,
                          child: canvas != null
                              ? LitchcraftThumbnail(canvas: canvas)
                              : DecoratedBox(
                                  decoration: BoxDecoration(
                                    color: Theme.of(
                                      context,
                                    ).colorScheme.surfaceContainerHighest,
                                    borderRadius: BorderRadius.circular(12),
                                  ),
                                  child: Center(
                                    child: failed
                                        ? const Icon(Icons.error_outline)
                                        : loading
                                        ? const ExcludeSemantics(
                                            child: CircularProgressIndicator(),
                                          )
                                        : const Icon(Icons.image_outlined),
                                  ),
                                ),
                        ),
                        const SizedBox(width: 16),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: <Widget>[
                              Text(
                                label,
                                style: Theme.of(context).textTheme.titleMedium,
                              ),
                              const SizedBox(height: 6),
                              Text(explanation),
                              if (value && retry)
                                TextButton(
                                  onPressed: () => unawaited(
                                    controller.prepareConflictBoardPreview(),
                                  ),
                                  style: TextButton.styleFrom(
                                    minimumSize: const Size(48, 48),
                                  ),
                                  child: Text(strings.tryAgain),
                                ),
                            ],
                          ),
                        ),
                        const SizedBox(width: 8),
                        const Icon(Icons.chevron_right),
                      ],
                    ),
                  ),
                ),
              );
              return SafeArea(
                top: false,
                left: false,
                right: false,
                child: SingleChildScrollView(
                  padding: const EdgeInsets.fromLTRB(20, 0, 20, 24),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: <Widget>[
                      Text(
                        strings.craftConflict,
                        style: Theme.of(context).textTheme.headlineSmall,
                      ),
                      const SizedBox(height: 8),
                      if (!pending) Text(strings.craftConflictHelp),
                      const SizedBox(height: 16),
                      choice(
                        key: const ValueKey('craft-use-board-artwork'),
                        canvas: preview,
                        label: strings.craftKeepBoard,
                        explanation: pending && preview == null
                            ? failed
                                  ? strings.craftReadBoardFailed
                                  : loading
                                  ? strings.craftReadingBoardArtwork
                                  : strings.craftKeepBoardHelp
                            : strings.craftKeepBoardHelp,
                        value: true,
                      ),
                      const SizedBox(height: 12),
                      choice(
                        key: const ValueKey('craft-use-app-artwork'),
                        canvas: controller.canvas,
                        label: strings.craftSendApp,
                        explanation: strings.craftSendAppHelp,
                        value: false,
                      ),
                    ],
                  ),
                ),
              );
            },
          );
        },
      );
    } finally {
      if (useBoard != null) _reopenConflictSheet = false;
      if (_conflictSheetContext != null &&
          !_reopenConflictSheet &&
          (useBoard != true || !mounted || !widget.active)) {
        sheetController.retireConflictBoardPreview();
      }
      if (mounted) {
        setState(() => _conflictSheetVisible = false);
      } else {
        _conflictSheetVisible = false;
      }
      _conflictSheetContext = null;
      _maybeReopenConflictSheet();
    }
    if (useBoard == null ||
        !mounted ||
        !widget.active ||
        !identical(sheetController, controller)) {
      return;
    }
    final resolvedUseBoard = useBoard;
    setState(() => _boardArtworkReadFailed = false);
    try {
      await _run(() async {
        if (resolvedUseBoard) {
          try {
            await library.preserveDraft();
            if (library.saveFailed) {
              throw StateError('Could not preserve app draft.');
            }
          } catch (_) {
            controller.retireConflictBoardPreview();
            rethrow;
          }
          if (!mounted || !widget.active) {
            controller.retireConflictBoardPreview();
            return;
          }
          setState(() {
            _readingBoardArtwork = true;
            _boardArtworkReadFailed = false;
          });
        }
        await controller.resolveConflict(useBoard: resolvedUseBoard);
      });
    } finally {
      if (mounted && _readingBoardArtwork) {
        setState(() => _readingBoardArtwork = false);
      }
    }
  }

  Future<bool> _save({bool saveAs = false}) async {
    var saved = false;
    final strings = litchStrings(context);
    final name = !saveAs && library.activeName != null
        ? library.activeName
        : await _nameDialog(
            strings.craftSaveAs,
            library.activeName ??
                '${strings.craftDefaultName} ${library.artworks.length + 1}',
          );
    if (name != null) {
      await _runLocal('save', () async {
        await library.save(name, saveAs: saveAs);
        saved = true;
        if (mounted) _showToolStatus(strings.craftSaved);
      });
    }
    return saved;
  }

  Future<void> _newArtwork() async {
    if (_working || _newArtworkPending) return;
    _newArtworkPending = true;
    try {
      await library.ready;
      if (!mounted) return;
      final strings = litchStrings(context);
      final changed =
          library.dirty &&
          (library.activeName != null ||
              !sameLitchcraftDocument(
                library.animationDocument,
                staticLitchcraftDocument(
                  LitchcraftCanvas.blank(topology: controller.canvas.topology),
                ),
              ));
      if (changed) {
        _cancelPhoneShakeQualification();
        final save = await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            title: Text(strings.craftSaveBeforeNew),
            content: Text(strings.craftSaveBeforeNewHelp),
            actions: [
              TextButton(
                onPressed: () => Navigator.of(context).pop(),
                child: Text(strings.cancelAction),
              ),
              TextButton(
                onPressed: () => Navigator.of(context).pop(false),
                child: Text(strings.craftDontSave),
              ),
              FilledButton(
                onPressed: () => Navigator.of(context).pop(true),
                child: Text(strings.craftSave),
              ),
            ],
          ),
        );
        if (!mounted || save == null) return;
        if (save && (!await _save() || !mounted || library.dirty)) return;
      }
      if (!mounted) return;
      _stopPreview();
      await _runLocal('new-artwork', library.newArtwork);
    } finally {
      _newArtworkPending = false;
    }
  }

  Future<String?> _nameDialog(String title, String initial) async {
    _cancelPhoneShakeQualification();
    var name = initial;
    return showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextFormField(
          initialValue: initial,
          autofocus: true,
          maxLength: 80,
          decoration: InputDecoration(
            labelText: litchStrings(context).craftName,
          ),
          onChanged: (value) => name = value,
          onFieldSubmitted: (value) {
            if (value.trim().isNotEmpty) {
              Navigator.of(context).pop(value.trim());
            }
          },
        ),
        actions: <Widget>[
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: Text(litchStrings(context).cancelAction),
          ),
          FilledButton(
            onPressed: () {
              if (name.trim().isNotEmpty) {
                Navigator.of(context).pop(name.trim());
              }
            },
            child: Text(litchStrings(context).craftSave),
          ),
        ],
      ),
    );
  }

  Future<bool> _confirm(String title, String help, String action) async {
    _cancelPhoneShakeQualification();
    return await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            title: Text(title),
            content: Text(help),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.of(context).pop(false),
                child: Text(litchStrings(context).cancelAction),
              ),
              FilledButton(
                onPressed: () => Navigator.of(context).pop(true),
                child: Text(action),
              ),
            ],
          ),
        ) ??
        false;
  }

  Future<void> _showLibrary() async {
    await library.ready;
    if (!mounted) return;
    _cancelPhoneShakeQualification();
    final strings = litchStrings(context);
    // Recovery is a distinct row even when storage shares its immutable source
    // object with an unchanged saved artwork. Keep metadata untouched.
    final recoveryArtwork = library.previousDraft?.copyWith();
    final entries = <LitchcraftArtwork>[?recoveryArtwork, ...library.artworks];
    final selection = await Navigator.of(context).push<_ArtworkSelection>(
      MaterialPageRoute<_ArtworkSelection>(
        fullscreenDialog: true,
        builder: (_) => _ArtworkLibraryPage(
          entries: entries,
          savedArtworks: library.artworks.toSet(),
          recoveryArtwork: recoveryArtwork,
          onShare: _shareArtwork,
        ),
      ),
    );
    if (selection == null || !mounted) return;
    if (selection.action == 'new') {
      await _newArtwork();
      return;
    }
    if (selection.action == 'saveAs') {
      await _save(saveAs: true);
      return;
    }
    final art = selection.art!;
    switch (selection.action) {
      case 'open':
        if (!library.dirty ||
            await _confirm(
              strings.craftOpenQuestion,
              strings.craftOpenHelp,
              strings.craftOpen,
            )) {
          _runInBackground(
            () => library.open(art, source: selection.source),
            isRecoveringFailure: library.isOpenFailureHandled,
          );
        }
      case 'rename':
        final name = await _nameDialog(strings.craftRename, art.name);
        if (name != null) {
          await _runLocal('rename:${art.id}', () => library.rename(art, name));
        }
      case 'duplicate':
        await _runLocal('duplicate:${art.id}', () => library.duplicate(art));
      case 'delete':
        if (await _confirm(
          strings.craftDeleteQuestion,
          '${art.name}\n\n${strings.craftDeleteHelp}',
          strings.craftDelete,
        )) {
          await _run(() => library.delete(art));
        }
    }
  }
}

class _ArtworkLibraryPage extends StatefulWidget {
  const _ArtworkLibraryPage({
    required this.entries,
    required this.savedArtworks,
    required this.recoveryArtwork,
    required this.onShare,
  });

  final List<LitchcraftArtwork> entries;
  final Set<LitchcraftArtwork> savedArtworks;
  final LitchcraftArtwork? recoveryArtwork;
  final Future<void> Function(BuildContext context, LitchcraftArtwork artwork)
  onShare;

  @override
  State<_ArtworkLibraryPage> createState() => _ArtworkLibraryPageState();
}

class _ArtworkLibraryPageState extends State<_ArtworkLibraryPage> {
  String _query = '';
  LitchcraftArtwork? _sharingArtwork;
  LitchcraftArtwork? _playingArtwork;
  LitchcraftArtwork? _loadingPreviewArtwork;
  LitchcraftArtwork? _previewArtwork;
  int _previewGeneration = 0;
  Timer? _previewTimer;
  int _previewElapsedMs = 0;

  @override
  void dispose() {
    ++_previewGeneration;
    _previewTimer?.cancel();
    super.dispose();
  }

  Future<void> _togglePreview(LitchcraftArtwork art) async {
    final generation = ++_previewGeneration;
    _previewTimer?.cancel();
    if (identical(_playingArtwork, art) ||
        identical(_loadingPreviewArtwork, art)) {
      setState(() {
        _playingArtwork = null;
        _loadingPreviewArtwork = null;
        _previewArtwork = null;
      });
      return;
    }
    setState(() {
      _playingArtwork = null;
      _loadingPreviewArtwork = art;
      _previewArtwork = null;
    });
    try {
      final resolved = await art.resolve();
      if (!mounted || generation != _previewGeneration) return;
      setState(() {
        _playingArtwork = art;
        _loadingPreviewArtwork = null;
        _previewArtwork = resolved;
        _previewElapsedMs = 0;
      });
      _previewTimer = Timer.periodic(const Duration(milliseconds: 50), (_) {
        if (!mounted || generation != _previewGeneration) return;
        setState(() => _previewElapsedMs += 50);
      });
    } catch (_) {
      if (!mounted || generation != _previewGeneration) return;
      setState(() => _loadingPreviewArtwork = null);
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text(litchStrings(context).craftCorrupt)),
      );
    }
  }

  LitchcraftCanvas _previewCanvas(LitchcraftArtwork art) {
    final resolved = _previewArtwork;
    if (!identical(_playingArtwork, art) ||
        resolved == null ||
        art.frameCount == 1) {
      return art.canvas;
    }
    final composed = composeLitchcraftAnimationDocument(
      resolved.document,
      _previewElapsedMs,
    );
    return LitchcraftCanvas(
      colors: composed.colors,
      topology: resolved.document.topology,
    );
  }

  String _displayName(LitchcraftArtwork art) =>
      identical(art, widget.recoveryArtwork)
      ? litchStrings(context).craftRecoveryDraft
      : art.name;

  String _rowKey(String prefix, LitchcraftArtwork art) =>
      '$prefix${identical(art, widget.recoveryArtwork) ? 'previous-' : ''}${art.id}';

  void _finish(String action, LitchcraftArtwork art) {
    ++_previewGeneration;
    _previewTimer?.cancel();
    Navigator.of(context).pop((
      action: action,
      art: art,
      source: widget.savedArtworks.contains(art)
          ? LitchcraftOpenSource.library
          : LitchcraftOpenSource.imported,
    ));
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final query = _query.trim().toLowerCase();
    final entries = query.isEmpty
        ? widget.entries
        : widget.entries
              .where(
                (art) =>
                    art.name.toLowerCase().contains(query) ||
                    _displayName(art).toLowerCase().contains(query),
              )
              .toList();
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(
          title: Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              const LitchcraftArtworkGlyph(),
              const SizedBox(width: 8),
              Text(strings.craftLibrary),
            ],
          ),
          actions: <Widget>[
            PopupMenuButton<String>(
              key: const ValueKey('craft-add-artwork'),
              tooltip: strings.craftSaveAs,
              icon: const Icon(Icons.add),
              onSelected: (String action) => Navigator.of(context).pop((
                action: action,
                art: null,
                source: LitchcraftOpenSource.library,
              )),
              itemBuilder: (_) => <PopupMenuEntry<String>>[
                PopupMenuItem(
                  value: 'new',
                  child: Text(strings.craftNewArtwork),
                ),
                PopupMenuItem(
                  value: 'saveAs',
                  child: Text(strings.craftSaveAs),
                ),
              ],
            ),
          ],
        ),
      ),
      body: SafeArea(
        child: Column(
          children: <Widget>[
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
              child: SearchBar(
                key: const ValueKey('craft-library-search'),
                hintText: MaterialLocalizations.of(context).searchFieldLabel,
                leading: const Icon(Icons.search),
                onChanged: (value) => setState(() => _query = value),
              ),
            ),
            Expanded(
              child: entries.isEmpty
                  ? Center(child: Text(strings.craftEmptyLibrary))
                  : _galleryEntries(entries),
            ),
          ],
        ),
      ),
    );
  }

  Widget _galleryEntries(List<LitchcraftArtwork> entries) => LayoutBuilder(
    builder: (context, bounds) {
      final tablet = LitchWorkspaceGeometry.isTabletWindow(
        MediaQuery.sizeOf(context),
      );
      final width = tablet
          ? math.min(1280.0, bounds.maxWidth)
          : bounds.maxWidth;
      // Match the title's actual type scale. A row grows with translated or
      // multiline names instead of clipping at a fixed grid tile extent.
      final cardWidth = 224.0 + MediaQuery.textScalerOf(context).scale(48);
      final columns = tablet ? (width / cardWidth).floor().clamp(1, 4) : 1;
      return Center(
        child: SizedBox(
          width: width,
          child: ListView.builder(
            padding: const EdgeInsets.fromLTRB(16, 4, 16, 24),
            itemCount: (entries.length / columns).ceil(),
            itemBuilder: (context, row) => Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                for (var column = 0; column < columns; column++) ...[
                  if (column != 0) const SizedBox(width: 12),
                  Expanded(
                    child: row * columns + column < entries.length
                        ? _artworkCard(
                            entries[row * columns + column],
                            tile: tablet,
                          )
                        : const SizedBox.shrink(),
                  ),
                ],
              ],
            ),
          ),
        ),
      );
    },
  );

  Widget _artworkCard(LitchcraftArtwork art, {required bool tile}) {
    final strings = litchStrings(context);
    final animated = art.isSupported && art.frameCount > 1;
    final saved = widget.savedArtworks.contains(art);
    final title = _displayName(art);
    final VoidCallback? open = art.isSupported
        ? () => _finish('open', art)
        : null;
    final VoidCallback? rename = saved ? () => _finish('rename', art) : null;
    Widget thumbnail(double dimension) => SizedBox.square(
      dimension: dimension,
      child: art.isSupported
          ? LitchcraftThumbnail(
              canvas: _previewCanvas(art),
              squarePreview: true,
            )
          : const Icon(Icons.broken_image_outlined),
    );
    final date = MaterialLocalizations.of(
      context,
    ).formatMediumDate(art.modifiedAt.toLocal());
    final description = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(title, style: Theme.of(context).textTheme.titleMedium),
        const SizedBox(height: 4),
        Text(
          art.isSupported
              ? '${tile ? strings.craftFrameLabel(1, art.frameCount) : art.frameCount} · $date'
              : strings.craftCorrupt,
        ),
      ],
    );
    return Semantics(
      button: art.isSupported,
      label:
          '$title, ${art.isSupported ? strings.craftFrameLabel(1, art.frameCount) : strings.craftCorrupt}',
      child: Card(
        margin: const EdgeInsets.symmetric(vertical: 6),
        clipBehavior: Clip.antiAlias,
        child: LitchcraftHoldAction(
          duration: kLongPressTimeout,
          holdLabel: saved ? strings.craftRename : '',
          completedLabel: '',
          showCompletionSnackBar: false,
          reserveProgressSpace: false,
          onTap: open,
          onHold: rename,
          child: InkWell(
            key: ValueKey(_rowKey('craft-artwork-', art)),
            canRequestFocus: false,
            excludeFromSemantics: true,
            onTap: open,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 12, 8, 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: <Widget>[
                  if (tile)
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        Center(child: thumbnail(112)),
                        const SizedBox(height: 10),
                        description,
                      ],
                    )
                  else
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        thumbnail(56),
                        const SizedBox(width: 12),
                        Expanded(child: description),
                      ],
                    ),
                  const SizedBox(height: 4),
                  Align(
                    alignment: AlignmentDirectional.centerEnd,
                    child: Wrap(
                      spacing: 2,
                      children: <Widget>[
                        if (animated)
                          IconButton(
                            key: ValueKey(
                              _rowKey('craft-preview-artwork-', art),
                            ),
                            tooltip: identical(_loadingPreviewArtwork, art)
                                ? strings.cancelAction
                                : identical(_playingArtwork, art)
                                ? strings.craftPausePreview
                                : strings.craftPlayPreview,
                            onPressed: () => unawaited(_togglePreview(art)),
                            icon: identical(_loadingPreviewArtwork, art)
                                ? const SizedBox.square(
                                    dimension: 20,
                                    child: CircularProgressIndicator(
                                      strokeWidth: 2,
                                    ),
                                  )
                                : Icon(
                                    identical(_playingArtwork, art)
                                        ? Icons.pause
                                        : Icons.play_arrow,
                                  ),
                            style: IconButton.styleFrom(
                              minimumSize: const Size(48, 48),
                            ),
                          ),
                        if (saved)
                          IconButton(
                            key: ValueKey('craft-rename-artwork-${art.id}'),
                            tooltip: strings.craftRename,
                            onPressed: () => _finish('rename', art),
                            icon: const Icon(Icons.edit_outlined),
                            style: IconButton.styleFrom(
                              minimumSize: const Size(48, 48),
                            ),
                          ),
                        if (saved)
                          IconButton(
                            key: ValueKey('craft-delete-artwork-${art.id}'),
                            tooltip: strings.craftDelete,
                            onPressed: () => _finish('delete', art),
                            icon: const Icon(Icons.delete_outline_rounded),
                            style: IconButton.styleFrom(
                              minimumSize: const Size(48, 48),
                            ),
                          ),
                        Builder(
                          builder: (shareContext) => IconButton(
                            key: ValueKey(_rowKey('craft-share-artwork-', art)),
                            tooltip: strings.craftExportShare,
                            onPressed: _sharingArtwork == null
                                ? () async {
                                    setState(() => _sharingArtwork = art);
                                    await widget.onShare(shareContext, art);
                                    if (mounted) {
                                      setState(() => _sharingArtwork = null);
                                    }
                                  }
                                : null,
                            icon: identical(_sharingArtwork, art)
                                ? const SizedBox.square(
                                    dimension: 20,
                                    child: CircularProgressIndicator(
                                      strokeWidth: 2,
                                    ),
                                  )
                                : const Icon(Icons.ios_share_outlined),
                            style: IconButton.styleFrom(
                              minimumSize: const Size(48, 48),
                            ),
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

/// Filters brief work and retires its activity with completion. This owns only
/// presentation: controller completion, input and failure/recovery stay immediate.
class _ArtworkSyncPresentation extends StatefulWidget {
  const _ArtworkSyncPresentation({
    super.key,
    required this.active,
    required this.isUrgent,
    required this.isSyncing,
    required this.syncChild,
    required this.regularChild,
    required this.builder,
  });
  final bool active;
  final bool Function() isUrgent, isSyncing;
  final Widget? syncChild, regularChild;
  final Widget Function(Widget? child) builder;
  @override
  State<_ArtworkSyncPresentation> createState() =>
      _ArtworkSyncPresentationState();
}

class _ArtworkSyncPresentationState extends State<_ArtworkSyncPresentation>
    with WidgetsBindingObserver {
  static const _noticeInterval = Duration(milliseconds: 500);
  Timer? _delay;
  Widget? _visible;
  bool _resumed = true;
  bool get _eligible => widget.active && _resumed && !widget.isUrgent();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    _resumed = lifecycle == null || lifecycle == AppLifecycleState.resumed;
    _synchronize();
  }

  @override
  void didUpdateWidget(covariant _ArtworkSyncPresentation oldWidget) {
    super.didUpdateWidget(oldWidget);
    _synchronize();
  }

  void _clear() {
    _delay?.cancel();
    _delay = null;
    _visible = null;
  }

  void _synchronize() {
    if (!_eligible) {
      _clear();
    } else if (widget.isSyncing()) {
      if (_visible != null) {
        _visible = widget.syncChild;
      } else {
        _delay ??= Timer(_noticeInterval, () {
          _delay = null;
          if (!mounted || !_eligible || !widget.isSyncing()) return;
          setState(() {
            _visible = widget.syncChild;
          });
        });
      }
    } else {
      _clear();
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _resumed = state == AppLifecycleState.resumed;
    setState(_synchronize);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _clear();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.active
      ? widget.builder(_visible ?? widget.regularChild)
      : const SizedBox.shrink();
}
