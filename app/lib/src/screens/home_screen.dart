import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../controllers/board_actions_controller.dart';
import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/board_health_status.dart';
import '../models/home_board_snapshot.dart';
import '../models/home_assistance_state.dart';
import '../models/local_player_profile.dart';
import '../services/local_player_profile_store.dart';
import '../widgets/home_player_name.dart';
import '../widgets/orientation_workspace.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../models/litch_clock_preset.dart';
import '../models/litch_control_action.dart';
import '../models/runtime_health_snapshot.dart';
import '../services/home_board_feed.dart';
import '../services/game_library_repository.dart';
import '../services/litch_ble_service.dart';
import '../theme/litch_theme.dart';
import 'game_library_screen.dart';
import '../widgets/cpu_thinking_robot.dart';
import '../widgets/home_board_actions.dart';
import '../widgets/home_chessboard.dart';
import '../widgets/home_notice_presenter.dart';
import '../widgets/recent_games_section.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({
    super.key,
    required this.bleService,
    required this.boardId,
    this.notices,
    this.onHealthAttentionChanged,
    this.feedBuilder,
    this.gameLibrary,
    this.gameLibraryOpener,
    this.assignPlayer,
    this.playerDirectoryLoader,
    this.onChoosePlayerColor,
    this.onOpenGameLibrary,
    this.landscapeFooterClearance = 0,
    this.landscapeDetailsOnLeft = false,
    this.rootSafeInsets = EdgeInsets.zero,
  });

  final LitchBleService bleService;
  final String boardId;
  final HomeNoticePresenter? notices;
  final ValueChanged<bool>? onHealthAttentionChanged;
  final HomeBoardFeed Function(LitchBleService service, String boardId)?
  feedBuilder;
  final LocalPlayerAssignment? assignPlayer;
  final Future<LocalPlayerDirectory> Function()? playerDirectoryLoader;
  final void Function({
    required bool whitePiece,
    required bool p1AtOppositeEnd,
  })?
  onChoosePlayerColor;
  final GameLibraryRepository? gameLibrary;
  final GameLibraryOpener? gameLibraryOpener;
  final VoidCallback? onOpenGameLibrary;

  /// Recoverable scroll clearance for a shell control floating over the details.
  final double landscapeFooterClearance;
  final bool landscapeDetailsOnLeft;
  final EdgeInsets rootSafeInsets;

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen>
    with SingleTickerProviderStateMixin {
  static const String _timerKey = 'TIMER';
  static const Map<int, String> _timerPresetLabels = LitchClockPresets.labels;

  final HomeNoticePresenter _localNotices = HomeNoticePresenter();
  HomeNoticePresenter get _notices => widget.notices ?? _localNotices;
  late HomeBoardFeed _feed;
  late HomeBoardSnapshot _snapshot;
  late final AnimationController _promotionMotion;
  HomePromotionConfirmation? _promotionNotice;
  late BoardActionsController _actionsController;
  StreamSubscription<HomeBoardSnapshot>? _stateSubscription;
  StreamSubscription<BoardNotice>? _noticeSubscription;
  bool? _reportedHealthAttention;
  LocalPlayerDirectory _players = LocalPlayerDirectory.empty();
  bool _nameOperation = false;
  LocalPlayerChessSide? _nameOperationSide;
  int _nameLoadGeneration = 0;
  bool _clockWriteInFlight = false;
  int? _clockDraftPreset;
  int _clockWriteGeneration = 0;
  ScaffoldFeatureController<SnackBar, SnackBarClosedReason>?
  _kingQueenSwapNotice;
  bool get _connectedToBoard =>
      widget.bleService.isConnected &&
      widget.bleService.connectedId == widget.boardId;
  bool get _canShowCurrentBoardGuidance =>
      _connectedToBoard && _feed.hasCurrentBoardState;
  HomePromotionConfirmation? get _visiblePromotion {
    if (!_canShowCurrentBoardGuidance ||
        _healthPausesPlay(_snapshot) ||
        _snapshot.homeStateAction == HomeStateAction.correction ||
        _snapshot.homeStateAction == HomeStateAction.promote ||
        _snapshot.physicalStatus?.placementMode == true) {
      return null;
    }
    return _promotionNotice;
  }

  int? get _promotionHighlightSquare {
    final square = _visiblePromotion?.to;
    final occupied = _snapshot.physicalStatus?.occupiedSquares;
    if (square == null ||
        _snapshot.captureRemovalSquare == square ||
        occupied == null) {
      return null;
    }
    return (occupied & (BigInt.one << square)) == BigInt.zero ? null : square;
  }

  bool get _steadyPromotion =>
      MediaQuery.disableAnimationsOf(context) ||
      widget.bleService.latestSettingValue('MINIMAL_ANIMATIONS') == true;

  @override
  void initState() {
    super.initState();
    _promotionMotion = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1100),
    );
    _actionsController = BoardActionsController(
      bleService: widget.bleService,
      boardId: widget.boardId,
    );
    _attachFeed();
    unawaited(_loadPlayerNames());
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && !_feed.hasAuthoritativeLiveState) {
        unawaited(_feed.refresh());
      }
    });
  }

  @override
  void didUpdateWidget(covariant HomeScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.boardId == widget.boardId &&
        oldWidget.bleService == widget.bleService) {
      return;
    }
    ++_clockWriteGeneration;
    _retireKingQueenSwapNotice();
    _clockWriteInFlight = false;
    _clockDraftPreset = null;
    unawaited(_releaseCurrentFeed());
    _actionsController.dispose();
    _actionsController = BoardActionsController(
      bleService: widget.bleService,
      boardId: widget.boardId,
    );
    _attachFeed();
    unawaited(_feed.refresh());
  }

  Future<void> _loadPlayerNames() async {
    if (widget.assignPlayer == null) return;
    final generation = ++_nameLoadGeneration;
    try {
      final loader = widget.playerDirectoryLoader;
      final directory = loader != null
          ? await loader()
          : (await (await LocalPlayerProfileStore.open()).load()).directory;
      if (mounted && generation == _nameLoadGeneration) {
        setState(() => _players = directory);
      }
    } catch (_) {
      // An explicit assignment can retry the existing store.
    }
  }

  Future<void> _editPlayer(
    LocalPlayerChessSide side, {
    bool clear = false,
  }) async {
    final assign = widget.assignPlayer;
    if (assign == null || _nameOperation) return;
    final boardId = widget.boardId;
    setState(() {
      _nameOperation = true;
      _nameOperationSide = side;
    });
    try {
      final LocalPlayerDirectory? directory;
      if (clear) {
        directory = await assign(boardId: boardId, side: side);
      } else {
        directory = await showHomePlayerAssignment(
          context: context,
          side: side,
          currentName: _players.selectedPlayer(side)?.displayName,
          assign: (name, {cancellation, beginCommit}) => assign(
            boardId: boardId,
            side: side,
            displayName: name,
            cancellation: cancellation,
            beginCommit: beginCommit,
          ),
        );
      }
      if (mounted && boardId == widget.boardId && directory != null) {
        ++_nameLoadGeneration;
        setState(() => _players = directory!);
      }
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(litchStrings(context).playerAssignmentFailed)),
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _nameOperation = false;
          _nameOperationSide = null;
        });
      }
    }
  }

  Widget _playerName(LocalPlayerChessSide side) => HomePlayerName(
    side: side,
    pending: _nameOperation && _nameOperationSide == side,
    name: _players.selectedPlayer(side)?.displayName,
    accent: litchPieceAccentColor(
      _snapshot,
      whitePiece: side == LocalPlayerChessSide.white,
    ),
    onChooseColor:
        widget.onChoosePlayerColor != null &&
            _connectedToBoard &&
            _snapshot.playerColorsConfirmed
        ? () => widget.onChoosePlayerColor!(
            whitePiece: side == LocalPlayerChessSide.white,
            p1AtOppositeEnd: _snapshot.physicalStatus?.p1AtOppositeEnd == true,
          )
        : null,
    onAssign:
        widget.assignPlayer != null &&
            _connectedToBoard &&
            widget.bleService.productReady &&
            !_nameOperation
        ? () => _editPlayer(side)
        : null,
    onClear:
        widget.assignPlayer != null &&
            _connectedToBoard &&
            widget.bleService.productReady &&
            !_nameOperation
        ? () => _editPlayer(side, clear: true)
        : null,
  );

  LocalPlayerChessSide get _topPlayer =>
      _snapshot.physicalStatus?.p1AtOppositeEnd == true
      ? LocalPlayerChessSide.white
      : LocalPlayerChessSide.black;
  LocalPlayerChessSide get _bottomPlayer =>
      _topPlayer == LocalPlayerChessSide.white
      ? LocalPlayerChessSide.black
      : LocalPlayerChessSide.white;
  String _playerForSide(HomeSide side) =>
      _players
          .selectedPlayer(
            side == HomeSide.white
                ? LocalPlayerChessSide.white
                : LocalPlayerChessSide.black,
          )
          ?.displayName ??
      _snapshot.playerForSide(side);

  void _attachFeed() {
    final HomeBoardFeed Function(LitchBleService, String) builder =
        widget.feedBuilder ??
        (LitchBleService service, String boardId) =>
            HomeBoardFeed(bleService: service, boardId: boardId);
    _feed = builder(widget.bleService, widget.boardId);
    _snapshot = _feed.current;
    _promotionNotice = null;
    _reportedHealthAttention = null;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _reportHealthAttention();
    });
    _stateSubscription = _feed.states.listen((HomeBoardSnapshot snapshot) {
      if (!mounted) {
        return;
      }
      final bool announceKingQueenSwap = snapshot.liveSequence != null
          ? widget.bleService.registerKingQueenSwapPromptState(
              boardId: widget.boardId,
              active: snapshot.isGuidedKingQueenSwap,
            )
          : false;
      final promotion = snapshot.promotionConfirmation;
      final previousPromotion = _snapshot.promotionConfirmation;
      final reveal = promotion?.reveal == true;
      if (!reveal || snapshot.liveSequence == null) {
        _promotionNotice = null;
        _promotionMotion.stop();
      } else if (_snapshot.liveSequence != null &&
          promotion != previousPromotion) {
        _promotionNotice = promotion;
        if (!_steadyPromotion) _promotionMotion.forward(from: 0);
      }
      setState(() {
        _snapshot = snapshot;
        if (!_clockWriteInFlight) {
          _clockDraftPreset = null;
        }
      });
      if (!_canShowCurrentBoardGuidance || !snapshot.isGuidedKingQueenSwap) {
        _retireKingQueenSwapNotice();
      }
      _reportHealthAttention();
      if (announceKingQueenSwap) {
        _showKingQueenSwapAnnouncement();
      }
    });
    _noticeSubscription = _feed.notices.listen(_showNotice);
  }

  void _reportHealthAttention() {
    final bool attention = boardHealthNeedsAttention(
      _snapshot.runtimeHealth,
      persistence: _snapshot.persistenceDegradation,
      sessionOnly: _snapshot.persistenceSessionOnly,
    );
    if (_reportedHealthAttention == attention) return;
    _reportedHealthAttention = attention;
    widget.onHealthAttentionChanged?.call(attention);
  }

  Future<void> _releaseCurrentFeed() async {
    final HomeBoardFeed feed = _feed;
    final StreamSubscription<HomeBoardSnapshot>? stateSubscription =
        _stateSubscription;
    final StreamSubscription<BoardNotice>? noticeSubscription =
        _noticeSubscription;
    _stateSubscription = null;
    _noticeSubscription = null;
    final Future<void> feedDisposal = feed.dispose();
    await stateSubscription?.cancel();
    await noticeSubscription?.cancel();
    await feedDisposal;
  }

  void _showNotice(BoardNotice notice) {
    if (!mounted) {
      return;
    }
    final ColorScheme colors = Theme.of(context).colorScheme;
    final Color background = switch (notice.tone) {
      BoardNoticeTone.success => const Color(0xFF245C35),
      BoardNoticeTone.warning => const Color(0xFF6A4A00),
      BoardNoticeTone.error => colors.errorContainer,
      BoardNoticeTone.info => colors.inverseSurface,
    };
    final Color foreground = switch (notice.tone) {
      BoardNoticeTone.error => colors.onErrorContainer,
      _ => colors.onInverseSurface,
    };
    _notices.showNotice(
      context,
      SnackBar(
        content: Text(notice.message, style: TextStyle(color: foreground)),
        backgroundColor: background,
      ),
    );
  }

  void _showKingQueenSwapAnnouncement() {
    final String message = litchStrings(context).swapAnnouncement;
    final ColorScheme colors = Theme.of(context).colorScheme;
    _kingQueenSwapNotice = _notices.showNotice(
      context,
      SnackBar(
        duration: const Duration(seconds: 6),
        backgroundColor: colors.inverseSurface,
        content: Row(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: <Widget>[
            _KingQueenSwapShuttle(color: colors.onInverseSurface),
            const SizedBox(width: 10),
            Expanded(child: Text(message)),
          ],
        ),
      ),
    );
  }

  void _retireKingQueenSwapNotice({bool afterUnmount = false}) {
    final notice = _kingQueenSwapNotice;
    final notices = _notices;
    _kingQueenSwapNotice = null;
    if (afterUnmount) {
      // Scaffold registration must finish before its messenger removes a notice.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        notices.removeNotice(notice);
      });
    } else {
      notices.removeNotice(notice);
    }
  }

  @override
  void dispose() {
    ++_clockWriteGeneration;
    _retireKingQueenSwapNotice(afterUnmount: true);
    _actionsController.dispose();
    _promotionMotion.dispose();
    unawaited(_releaseCurrentFeed());
    super.dispose();
  }

  Future<void> _writeClockPreset(int preset) async {
    final HomeClockStatus? clock = _snapshot.clockStatus;
    if (clock == null ||
        clock.setupState != HomeClockSetupState.available ||
        _clockWriteInFlight ||
        !_timerPresetLabels.containsKey(preset)) {
      return;
    }

    final int previous = _clockDraftPreset ?? clock.timerSetting;
    if (preset == previous) {
      return;
    }

    final int generation = ++_clockWriteGeneration;
    final LitchBleService service = widget.bleService;
    final String boardId = widget.boardId;
    final String targetId = service.connectedId ?? boardId;
    setState(() {
      _clockDraftPreset = preset;
      _clockWriteInFlight = true;
    });

    final LitchSettingWriteResult result = await service
        .writeSettingAcknowledged(
          deviceId: targetId,
          key: _timerKey,
          value: preset,
        );
    if (!mounted ||
        generation != _clockWriteGeneration ||
        service != widget.bleService ||
        boardId != widget.boardId) {
      return;
    }

    final bool ok =
        result is LitchSettingWriteAcknowledged && result.outcome.ok;
    setState(() {
      _clockWriteInFlight = false;
      if (!ok) {
        _clockDraftPreset = previous;
      }
    });
    if (!ok) {
      final String message;
      if (result is LitchSettingWriteAcknowledged) {
        if (result.outcome.reason == LitchMutationReason.notAtStart) {
          message = litchStrings(context).clockStartedFailure;
          unawaited(_feed.reconcileClockAfterStartRejection());
        } else if (result.outcome.reason == LitchMutationReason.busy ||
            result.outcome.reason == LitchMutationReason.queueFull) {
          message = litchStrings(context).boardFinishingAction;
        } else {
          message = litchStrings(context).clockApplyFailed;
        }
      } else {
        message = litchStrings(context).clockUnconfirmed;
      }
      if (mounted) {
        _notices.showNotice(context, SnackBar(content: Text(message)));
      }
    }
  }

  Future<void> _refreshHome() async {
    await Future.wait<void>(<Future<void>>[
      _feed.refresh(),
      _actionsController.refreshPlayerRolesIfNeeded(),
    ]);
  }

  @override
  Widget build(BuildContext context) {
    return RefreshIndicator(
      onRefresh: _refreshHome,
      child: LayoutBuilder(
        builder: (BuildContext context, BoxConstraints constraints) {
          const landscapeGap = LitchWorkspaceGeometry.landscapeGap;
          final window = MediaQuery.sizeOf(context);
          final tablet = LitchWorkspaceGeometry.isTabletWindow(window);
          final landscape = tablet
              ? LitchWorkspaceGeometry.usesTabletBoardColumns(
                  window,
                  constraints.biggest,
                )
              : constraints.maxWidth >= 480 &&
                    constraints.maxHeight <= 480 &&
                    window.width > window.height;
          final edges = LitchWorkspaceGeometry(
            constraints.biggest,
            safeInsets: widget.rootSafeInsets,
          ).landscapeEdgeInsets;
          final boardSlotSize = tablet
              ? LitchWorkspaceGeometry.tabletCanvasSide(
                  constraints.biggest,
                  columns: landscape,
                )
              : LitchWorkspaceGeometry(
                  Size(
                    constraints.maxWidth,
                    constraints.maxHeight + edges.vertical,
                  ),
                  safeInsets: widget.rootSafeInsets,
                ).landscapeCanvasSide;
          final landscapeBoardTop =
              (constraints.maxHeight - boardSlotSize).clamp(
                0.0,
                double.infinity,
              ) /
              2;
          final contentWidth = math.min(
            constraints.maxWidth,
            LitchWorkspaceGeometry.readableContentWidth,
          );
          Widget playerAtBoardEdge(LocalPlayerChessSide side) =>
              tablet && !landscape
              ? Center(
                  child: SizedBox(
                    width: boardSlotSize,
                    child: _playerName(side),
                  ),
                )
              : _playerName(side);
          final details = ListView(
            key: const ValueKey<String>('home-details-scroll'),
            physics: const AlwaysScrollableScrollPhysics(),
            padding: EdgeInsets.fromLTRB(
              landscape && !tablet ? landscapeGap : 0,
              landscape ? landscapeBoardTop : 4,
              landscape && !tablet ? landscapeGap : 0,
              landscape && !tablet
                  ? (widget.landscapeFooterClearance > 0
                        ? widget.landscapeFooterClearance + landscapeGap
                        : 64)
                  : 28,
            ),
            // Keep the compact Play sections mounted so accessibility and
            // test journeys can discover Moves and Recent games before a
            // scroll without changing the fixed landscape board ownership.
            cacheExtent: 5000,
            children: <Widget>[
              Padding(
                padding: EdgeInsets.symmetric(horizontal: landscape ? 0 : 12),
                child: Center(
                  child: SizedBox(
                    width: contentWidth,
                    child: AnimatedBuilder(
                      animation: _actionsController,
                      builder: (BuildContext context, Widget? child) =>
                          _GameSurface(
                            showBoard: false,
                            playerName: _playerForSide,
                            connected: _connectedToBoard,
                            currentBoardState: _feed.hasCurrentBoardState,
                            promotionNotice: _visiblePromotion,
                            snapshot: _actionsController.canPresentHintGuidance
                                ? _snapshot
                                : _snapshot.copyWith(clearAssistance: true),
                          ),
                    ),
                  ),
                ),
              ),
              if (landscape) ...[
                _playerName(_topPlayer),
                _playerName(_bottomPlayer),
              ],
              if (!landscape) ...<Widget>[
                const SizedBox(height: LitchWorkspaceGeometry.outerGap),
                playerAtBoardEdge(_topPlayer),
                Padding(
                  key: const ValueKey('home-board-breathing-space'),
                  padding: const EdgeInsets.symmetric(
                    horizontal: LitchWorkspaceGeometry.canvasHorizontalInset,
                  ),
                  child: AnimatedBuilder(
                    animation: _actionsController,
                    builder: (context, _) => HomeChessboard(
                      showCoordinates: true,
                      liveGuidanceAvailable: _canShowCurrentBoardGuidance,
                      promotionSquare: _promotionHighlightSquare,
                      promotionMotion: _steadyPromotion
                          ? null
                          : _promotionMotion,
                      snapshot: _actionsController.canPresentHintGuidance
                          ? _snapshot
                          : _snapshot.copyWith(clearAssistance: true),
                      maxSize: tablet
                          ? boardSlotSize
                          : LitchWorkspaceGeometry.portraitCanvasSide(
                              constraints.maxWidth,
                            ),
                    ),
                  ),
                ),
                playerAtBoardEdge(_bottomPlayer),
              ],
              const SizedBox(height: 12),
              Padding(
                padding: EdgeInsets.symmetric(horizontal: landscape ? 0 : 12),
                child: Center(
                  key: const ValueKey<String>('home-board-actions-section'),
                  child: SizedBox(
                    width: contentWidth,
                    child: HomeBoardActions(
                      controller: _actionsController,
                      clockControls: _HomeClockControls(
                        snapshot: _snapshot,
                        selectedPreset: _clockDraftPreset,
                        writeInFlight: _clockWriteInFlight,
                        onChanged: _writeClockPreset,
                      ),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 12),
              Padding(
                padding: EdgeInsets.symmetric(horizontal: landscape ? 0 : 12),
                child: Center(
                  key: const ValueKey<String>('home-moves-section'),
                  child: SizedBox(
                    width: contentWidth,
                    child: _MoveHistoryPanel(snapshot: _snapshot),
                  ),
                ),
              ),
              if (widget.onOpenGameLibrary != null &&
                  (widget.gameLibrary != null ||
                      widget.gameLibraryOpener != null)) ...<Widget>[
                const SizedBox(height: 12),
                Padding(
                  padding: EdgeInsets.symmetric(horizontal: landscape ? 0 : 12),
                  child: Center(
                    key: const ValueKey<String>('recent-games-home-section'),
                    child: SizedBox(
                      width: contentWidth,
                      child: RecentGamesSection(
                        repository: widget.gameLibrary,
                        openRepository: widget.gameLibraryOpener,
                        onOpenAll: widget.onOpenGameLibrary!,
                      ),
                    ),
                  ),
                ),
              ],
            ],
          );
          if (!landscape) return details;
          // Play has no tool rails beside the board. Keep the full-size board
          // vertically centered, place it at the outside edge with one shared
          // breathing gap, and give every remaining pixel back to game status.
          final boardInsets = tablet
              ? EdgeInsets.zero
              : widget.landscapeDetailsOnLeft
              ? EdgeInsets.only(left: landscapeGap, right: edges.right)
              : EdgeInsets.only(left: edges.left, right: landscapeGap);
          final boardRegionWidth = (boardSlotSize + boardInsets.horizontal)
              .clamp(0.0, constraints.maxWidth);
          final workspace = Row(
            key: const ValueKey('home-landscape-workspace'),
            mainAxisAlignment: tablet
                ? MainAxisAlignment.center
                : MainAxisAlignment.start,
            textDirection: widget.landscapeDetailsOnLeft
                ? TextDirection.rtl
                : TextDirection.ltr,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SizedBox(
                key: const ValueKey('home-board-breathing-space'),
                width: boardRegionWidth,
                child: Padding(
                  padding: boardInsets,
                  child: AnimatedBuilder(
                    animation: _actionsController,
                    builder: (context, _) => OrientationContentTurn(
                      child: HomeChessboard(
                        showCoordinates: true,
                        liveGuidanceAvailable: _canShowCurrentBoardGuidance,
                        promotionSquare: _promotionHighlightSquare,
                        promotionMotion: _steadyPromotion
                            ? null
                            : _promotionMotion,
                        snapshot: _actionsController.canPresentHintGuidance
                            ? _snapshot
                            : _snapshot.copyWith(clearAssistance: true),
                        maxSize: boardSlotSize,
                      ),
                    ),
                  ),
                ),
              ),
              if (tablet)
                const SizedBox(width: LitchWorkspaceGeometry.tabletBoardGap),
              if (tablet)
                SizedBox(
                  width: LitchWorkspaceGeometry.controlGroupWidth,
                  child: details,
                )
              else
                Expanded(child: details),
            ],
          );
          return tablet
              ? Padding(
                  padding: const EdgeInsets.symmetric(
                    horizontal: LitchWorkspaceGeometry.contentEdgeInset,
                  ),
                  child: workspace,
                )
              : workspace;
        },
      ),
    );
  }
}

class _GameSurface extends StatefulWidget {
  const _GameSurface({
    required this.snapshot,
    required this.connected,
    required this.currentBoardState,
    this.showBoard = true,
    this.playerName,
    this.promotionNotice,
  });

  final HomeBoardSnapshot snapshot;
  final bool connected;
  final bool currentBoardState;
  final bool showBoard;
  final String Function(HomeSide)? playerName;
  final HomePromotionConfirmation? promotionNotice;

  @override
  State<_GameSurface> createState() => _GameSurfaceState();
}

class _GameSurfaceState extends State<_GameSurface> {
  HomeAssistanceState? _lastHint;
  HomeAssistanceState? _updatedFrom;
  String? _hintPosition;
  late String _hintStatusSquareReservation;

  HomeBoardSnapshot get snapshot => widget.snapshot;
  bool get connected => widget.connected;
  bool get currentBoardState => widget.currentBoardState;

  @override
  void initState() {
    super.initState();
    _observeHint();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    // Square labels are a fixed set across rotation. Measure only when their
    // typography dependencies change, rather than on each Home notification.
    final style = Theme.of(context).textTheme.bodySmall;
    final painter = TextPainter(
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
    );
    _hintStatusSquareReservation = algebraicSquare(0);
    var widest = 0.0;
    for (var square = 0; square < 64; ++square) {
      final label = algebraicSquare(square);
      painter.text = TextSpan(text: label, style: style);
      painter.layout();
      if (painter.width > widest) {
        widest = painter.width;
        _hintStatusSquareReservation = label;
      }
    }
    painter.dispose();
  }

  @override
  void didUpdateWidget(_GameSurface oldWidget) {
    super.didUpdateWidget(oldWidget);
    _observeHint();
  }

  void _observeHint() {
    final hint = connected && currentBoardState
        ? snapshot.visibleAssistance
        : null;
    // This is explanatory presentation only. Physical handling, a hidden Hint
    // or uncertain authority retires it; it never supplies a board move.
    if (hint == null ||
        !hint.isReady ||
        snapshot.watchdogRestart ||
        snapshot.homeStateAction != HomeStateAction.idle) {
      _lastHint = null;
      _updatedFrom = null;
      _hintPosition = null;
      return;
    }
    final position =
        '${snapshot.position!.fen}|${snapshot.gameStatus?.variant}|'
        '${snapshot.gameStatus?.duckSquare}';
    if (_hintPosition != position) {
      _updatedFrom = null;
    } else if (_lastHint case final previous?
        when previous.move != hint.move || previous.duckMove != hint.duckMove) {
      _updatedFrom = previous;
    }
    _hintPosition = position;
    _lastHint = hint;
  }

  String _hintMoveText(HomeAssistanceState hint) {
    final strings = litchStrings(context);
    String moveText(({int from, int to}) move) => strings.hintMoveCoordinates(
      algebraicSquare(move.from),
      algebraicSquare(move.to),
    );
    final move = hint.move;
    final duck = hint.duckMove;
    if (move == null) return strings.hintDuckCoordinates(moveText(duck!));
    return duck == null
        ? moveText(move)
        : strings.hintCompoundCoordinates(moveText(move), moveText(duck));
  }

  @override
  Widget build(BuildContext context) {
    final HomeChessPosition? position = snapshot.position;
    final HomeGameStatus? game = snapshot.gameStatus;
    final awaitingBoard = connected && !currentBoardState;
    final readingBoard =
        snapshot.isRefreshing ||
        (awaitingBoard && snapshot.refreshMessage == null);
    final String? guidance = connected && !awaitingBoard
        ? _syncDescription(context, snapshot)
        : null;
    final promotion = widget.promotionNotice;
    final strings = litchStrings(context);
    final promotionSelection =
        switch (snapshot.pendingPromotionPrompt?.promotionSelection) {
          'q' => ('queen', strings.reviewPieceQueen),
          'r' => ('rook', strings.reviewPieceRook),
          'b' => ('bishop', strings.reviewPieceBishop),
          'n' => ('knight', strings.reviewPieceKnight),
          _ => null,
        };
    String? selectionArtwork;
    final promotionTitle = switch (promotion?.piece.toLowerCase()) {
      'q' => strings.promotedToQueen,
      'r' => strings.promotedToRook,
      'b' => strings.promotedToBishop,
      'n' => strings.promotedToKnight,
      _ => null,
    };
    final bool cpuRecovery =
        snapshot.cpuThinking != null && snapshot.watchdogRestart;
    final String eyebrow;
    final String title;
    final Color signal;
    if (!connected) {
      eyebrow = '';
      title = litchStrings(context).boardDisconnected;
    } else if (position == null || awaitingBoard) {
      eyebrow = readingBoard ? '' : litchStrings(context).boardEyebrow;
      title = readingBoard
          ? litchStrings(context).readingBoard
          : litchStrings(context).positionUnavailable;
    } else if (_healthPausesPlay(snapshot)) {
      eyebrow = litchStrings(context).boardInputPaused;
      title = litchStrings(context).keepPiecesStill;
    } else if (game?.isFinished ?? false) {
      final result = localizedHomeResult(
        strings,
        game!,
        playerForSide: snapshot.playerForSide,
      );
      eyebrow = result.heading;
      title = result.title;
    } else if (snapshot.vexMoveFirst) {
      eyebrow = litchStrings(context).yourMove;
      title = litchStrings(context).vexMoveFirst;
    } else if (snapshot.isDuckRelocationCorrection ||
        snapshot.isDuckLandingCorrection) {
      eyebrow = litchStrings(context).needsCorrection;
      title = litchStrings(context).followBoardGuidance;
    } else if (snapshot.homeStateAction == HomeStateAction.settle) {
      eyebrow = litchStrings(context).moveProgressEyebrow;
      title = litchStrings(context).moveInProgress;
    } else if (game?.duckMovePending ?? false) {
      eyebrow = switch (snapshot.visibleDuckRelocation?.owner) {
        HomeDuckRelocationOwner.cpu => litchStrings(context).computerMove,
        HomeDuckRelocationOwner.human => litchStrings(context).yourMove,
        _ => litchStrings(context).duckMove,
      };
      title = litchStrings(context).placeDuck;
    } else if (snapshot.homeStateAction == HomeStateAction.cpu) {
      eyebrow = litchStrings(context).computerMove;
      title = litchStrings(context).computerMoveReady;
    } else if (snapshot.homeStateAction == HomeStateAction.promote) {
      eyebrow = litchStrings(context).promotionEyebrow;
      title = promotionSelection != null
          ? strings.promotionSelectedPiece(promotionSelection.$2)
          : snapshot.promotionPlacementPending
          ? strings.moveInProgress
          : strings.choosePromotion;
      selectionArtwork = promotionSelection?.$1;
    } else if (snapshot.isGuidedKingQueenSwap) {
      eyebrow = litchStrings(context).boardSetupEyebrow;
      title = litchStrings(context).swapKingQueen;
    } else if (snapshot.showsCpuThinking) {
      eyebrow = '';
      title = litchStrings(context).cpuIsThinking;
    } else if (snapshot.isLegalLiveProgress && !cpuRecovery) {
      eyebrow = litchStrings(context).moveProgressEyebrow;
      title = litchStrings(context).moveInProgress;
    } else if (snapshot.isFreshSetupInProgress) {
      eyebrow = litchStrings(context).newGameSetup;
      title = snapshot.unexpectedOccupiedSquares.isNotEmpty
          ? litchStrings(context).removeLeftovers
          : snapshot.missingExpectedSquares.isNotEmpty
          ? litchStrings(context).setStartingPosition
          : litchStrings(context).almostReady;
    } else if (snapshot.mismatchedSquares.isNotEmpty) {
      eyebrow = litchStrings(context).needsCorrection;
      title = litchStrings(context).followBoardGuidance;
    } else if (snapshot.watchdogRestart) {
      eyebrow = litchStrings(context).boardRestartedEyebrow;
      title = litchStrings(context).playerToMove(
        widget.playerName?.call(position.sideToMove) ??
            snapshot.playerForSide(position.sideToMove),
      );
    } else {
      eyebrow = '';
      title = litchStrings(context).playerToMove(
        widget.playerName?.call(position.sideToMove) ??
            snapshot.playerForSide(position.sideToMove),
      );
    }
    signal = !connected || awaitingBoard
        ? Theme.of(context).colorScheme.onSurfaceVariant
        : snapshot.watchdogRestart || _healthPausesPlay(snapshot)
        ? const Color(0xFFE8BD83)
        : (snapshot.isDuckLandingCorrection ||
                  snapshot.mismatchedSquares.isNotEmpty) &&
              !snapshot.isExpectedBoardInteraction &&
              !snapshot.isFreshSetupInProgress
        ? const Color(0xFFDBA525)
        : game?.isFinished ?? false
        ? const Color(0xFFC89A36)
        : Theme.of(context).colorScheme.primary;
    final String? quietContext =
        connected &&
            !awaitingBoard &&
            eyebrow.isEmpty &&
            guidance == null &&
            position != null
        ? _gameContextLabel(context, snapshot)
        : null;

    final hint = _lastHint;
    final previous = _updatedFrom;
    final promotionHasPrimarySlot =
        promotionTitle != null &&
        snapshot.homeStateAction == HomeStateAction.idle &&
        !(game?.isFinished ?? false);
    final replacementGuidance = promotion != null && promotion.automatic
        ? strings.promotionReplacePawn(algebraicSquare(promotion.to))
        : null;
    final resolvedGuidance = promotionTitle == null
        ? guidance
        : promotionHasPrimarySlot
        ? replacementGuidance ?? title
        : [?guidance, promotionTitle, ?replacementGuidance].join('\n');
    final analysing =
        hint?.presentation == HomeAssistancePresentation.refreshing;
    final updated = hint != null && previous != null
        ? strings.hintUpdated(_hintMoveText(previous), _hintMoveText(hint))
        : null;
    final String? hintStatus = hint == null
        ? null
        : updated != null
        ? analysing
              ? '$updated ${strings.hintAnalysing}'
              : updated
        : analysing
        ? strings.hintStillAnalysing
        : strings.hintPlayable;
    final reserveHintStatus =
        hint != null ||
        (connected &&
            currentBoardState &&
            !snapshot.watchdogRestart &&
            snapshot.homeStateAction == HomeStateAction.idle &&
            snapshot.visibleAssistance?.manualAvailable == true);
    final reservedMove = strings.hintMoveCoordinates(
      _hintStatusSquareReservation,
      _hintStatusSquareReservation,
    );
    final reservedHint = game?.variant.toLowerCase() == 'duck'
        ? strings.hintCompoundCoordinates(reservedMove, reservedMove)
        : reservedMove;
    final hintStatusReservations = reserveHintStatus
        ? [
            strings.hintPlayable,
            strings.hintStillAnalysing,
            '${strings.hintUpdated(reservedHint, reservedHint)} '
                '${strings.hintAnalysing}',
            ?hintStatus,
          ]
        : const <String>[];

    return Material(
      key: const ValueKey('home-game-surface'),
      color: const Color(0xFF202A2C),
      shape: LitchShapes.panel(),
      clipBehavior: Clip.antiAlias,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 8, 14, 14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            _GuidanceHeader(
              eyebrow: promotionHasPrimarySlot
                  ? strings.promotionEyebrow
                  : eyebrow,
              title: promotionHasPrimarySlot ? promotionTitle : title,
              titleArtwork: selectionArtwork == null
                  ? null
                  : Image.asset(
                      'assets/chess_pieces/cameos_v3/$selectionArtwork.png',
                      key: ValueKey('promotion-selection-$selectionArtwork'),
                      width: 40,
                      height: 40,
                      fit: BoxFit.contain,
                      color: Colors.white,
                      colorBlendMode: BlendMode.srcIn,
                      excludeFromSemantics: true,
                    ),
              guidance: resolvedGuidance,
              quietContext: promotionTitle != null ? null : quietContext,
              hintStatus: hintStatus,
              hintAnnouncement: updated,
              hintStatusReservations: hintStatusReservations,
              quietContextWidget: quietContext == null || promotionTitle != null
                  ? null
                  : _GameContext(snapshot: snapshot),
              signal: signal,
              liveRegion: connected && !snapshot.isGuidedKingQueenSwap,
              thinking:
                  promotionTitle == null &&
                  connected &&
                  !awaitingBoard &&
                  snapshot.showsCpuThinking,
            ),
            if (snapshot.refreshMessage != null) ...<Widget>[
              const SizedBox(height: 8),
              Text(
                litchStrings(context).boardRefreshFailed,
                style: Theme.of(
                  context,
                ).textTheme.bodySmall?.copyWith(color: const Color(0xFFD6DEDD)),
              ),
            ],
            if (widget.showBoard) ...[
              Padding(
                key: const ValueKey('home-board-breathing-space'),
                padding: const EdgeInsets.all(LitchWorkspaceGeometry.outerGap),
                child: HomeChessboard(
                  showCoordinates: true,
                  snapshot: snapshot,
                  maxSize: 430,
                  liveGuidanceAvailable: connected && currentBoardState,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _GuidanceHeader extends StatelessWidget {
  const _GuidanceHeader({
    required this.eyebrow,
    required this.title,
    required this.guidance,
    required this.quietContext,
    required this.quietContextWidget,
    required this.signal,
    required this.liveRegion,
    this.titleArtwork,
    this.hintStatus,
    this.hintAnnouncement,
    this.hintStatusReservations = const [],
    this.thinking = false,
  });

  final String eyebrow;
  final String title;
  final String? guidance;
  final String? quietContext;
  final Widget? quietContextWidget;
  final Color signal;
  final bool liveRegion;
  final Widget? titleArtwork;
  final bool thinking;
  final String? hintStatus;
  final String? hintAnnouncement;
  final List<String> hintStatusReservations;

  @override
  Widget build(BuildContext context) {
    final bool hasEyebrow = eyebrow.isNotEmpty;
    final titleText = Text(
      title,
      textAlign: TextAlign.start,
      style: Theme.of(context).textTheme.headlineSmall?.copyWith(
        color: Colors.white,
        fontWeight: FontWeight.w700,
      ),
    );
    final String semanticsLabel = <String>[
      if (hasEyebrow) eyebrow,
      _spokenSideText(context, title),
      if (guidance case final String guidance)
        _spokenSideText(context, guidance),
      if (quietContext case final String quietContext) quietContext,
      if (hintAnnouncement case final String announcement) announcement,
    ].join('. ');
    Widget reservedHintStatus() => ExcludeSemantics(
      child: Opacity(
        opacity: 0,
        child: Stack(
          alignment: AlignmentDirectional.topStart,
          children: [
            for (final text in hintStatusReservations)
              RichText(
                textScaler: MediaQuery.textScalerOf(context),
                text: TextSpan(
                  text: text,
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ),
          ],
        ),
      ),
    );
    // Hidden notes still reserve their complete height. Split that space
    // around the visible guidance so it stays centered beside the rail.
    Widget halfHintSpace() => Padding(
      padding: const EdgeInsets.symmetric(vertical: 1),
      child: Align(
        alignment: AlignmentDirectional.topStart,
        heightFactor: 0.5,
        child: reservedHintStatus(),
      ),
    );
    final hiddenHintSpace =
        hintStatus == null && hintStatusReservations.isNotEmpty;
    return Semantics(
      key: const ValueKey<String>('home-guidance-live-region'),
      container: true,
      // A changing estimate is available when explored, but only the stable
      // thinking title is a live region. Ready/correction retain one announcement.
      liveRegion: liveRegion && !thinking,
      label: thinking ? null : semanticsLabel,
      explicitChildNodes: !thinking,
      child: ConstrainedBox(
        constraints: const BoxConstraints(minHeight: 90),
        child: LayoutBuilder(
          builder: (context, constraints) {
            final bool stackRobot =
                thinking &&
                _usesLargeTextLayout(context) &&
                constraints.maxWidth < 320;
            return Row(
              crossAxisAlignment: CrossAxisAlignment.center,
              children: <Widget>[
                Container(
                  key: const ValueKey<String>('home-guidance-rail'),
                  width: 5,
                  height: guidance == null ? 40 : 56,
                  decoration: BoxDecoration(
                    color: signal,
                    borderRadius: BorderRadius.circular(99),
                  ),
                ),
                const SizedBox(width: 12),
                if (thinking && !stackRobot) ...<Widget>[
                  const CpuThinkingRobot(
                    key: ValueKey<String>('cpu-thinking-robot'),
                  ),
                  const SizedBox(width: 12),
                ],
                Expanded(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      if (hiddenHintSpace) halfHintSpace(),
                      if (stackRobot) ...<Widget>[
                        const CpuThinkingRobot(
                          key: ValueKey<String>('cpu-thinking-robot'),
                        ),
                        const SizedBox(height: 8),
                      ],
                      if (hasEyebrow) ...<Widget>[
                        ExcludeSemantics(
                          child: Text(
                            eyebrow,
                            textAlign: TextAlign.start,
                            style: Theme.of(context).textTheme.labelMedium
                                ?.copyWith(
                                  color: signal,
                                  fontWeight: FontWeight.w800,
                                  letterSpacing: 1.1,
                                ),
                          ),
                        ),
                        const SizedBox(height: 2),
                      ],
                      ExcludeSemantics(
                        excluding: !thinking,
                        child: Semantics(
                          key: thinking
                              ? const ValueKey<String>(
                                  'cpu-thinking-announcement',
                                )
                              : null,
                          container: thinking,
                          liveRegion: thinking,
                          child: titleArtwork == null
                              ? titleText
                              : Row(
                                  children: [
                                    titleArtwork!,
                                    const SizedBox(width: 8),
                                    Expanded(child: titleText),
                                  ],
                                ),
                        ),
                      ),
                      if (guidance case final String guidance) ...<Widget>[
                        const SizedBox(height: 4),
                        ExcludeSemantics(
                          excluding: !thinking,
                          child: Text(
                            guidance,
                            textAlign: TextAlign.start,
                            style: Theme.of(context).textTheme.bodyMedium
                                ?.copyWith(color: const Color(0xFFD6DEDD)),
                          ),
                        ),
                      ] else if (quietContextWidget
                          case final Widget quietContextWidget) ...<Widget>[
                        const SizedBox(height: 4),
                        ExcludeSemantics(child: quietContextWidget),
                      ],
                      if (hiddenHintSpace) halfHintSpace(),
                      if (hintStatus != null) ...[
                        const SizedBox(height: 4),
                        Stack(
                          alignment: AlignmentDirectional.topStart,
                          children: [
                            if (hintStatusReservations.isNotEmpty)
                              reservedHintStatus(),
                            if (hintStatus case final String hintStatus)
                              Semantics(
                                key: const ValueKey(
                                  'home-hint-status-semantics',
                                ),
                                container: true,
                                liveRegion: false,
                                label: hintStatus,
                                excludeSemantics: true,
                                child: Text(
                                  hintStatus,
                                  key: const ValueKey('home-hint-status'),
                                  style: Theme.of(context).textTheme.bodySmall
                                      ?.copyWith(
                                        color: const Color(0xFFD6DEDD),
                                      ),
                                ),
                              ),
                          ],
                        ),
                      ],
                    ],
                  ),
                ),
              ],
            );
          },
        ),
      ),
    );
  }
}

class _HomeClockControls extends StatelessWidget {
  const _HomeClockControls({
    required this.snapshot,
    required this.selectedPreset,
    required this.writeInFlight,
    required this.onChanged,
  });

  static const Map<int, String> _presetLabels = LitchClockPresets.labels;

  final HomeBoardSnapshot snapshot;
  final int? selectedPreset;
  final bool writeInFlight;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    final HomeClockStatus? clock = snapshot.clockStatus;
    final int selected = _presetLabels.containsKey(selectedPreset)
        ? selectedPreset!
        : _presetLabels.containsKey(clock?.timerSetting)
        ? clock!.timerSetting
        : 0;
    final bool hasOptimisticSelection = selectedPreset != null;
    final bool setupAvailable =
        clock?.setupState == HomeClockSetupState.available ||
        (clock == null && hasOptimisticSelection);
    final bool enabled = setupAvailable && !writeInFlight;
    final bool reading =
        clock == null &&
        !hasOptimisticSelection &&
        (snapshot.isRefreshing || snapshot.isClockRefreshing);
    final String helperText;
    if (clock == null) {
      helperText = hasOptimisticSelection
          ? selected == 0
                ? litchStrings(context).clockChooseTime
                : litchStrings(context).clockReady
          : reading
          ? litchStrings(context).clockSyncing
          : litchStrings(context).clockRetryHelp;
    } else if (writeInFlight) {
      // Routine acknowledged writes stay quiet. The selected value is already
      // shown optimistically; keep ordinary ready/off guidance here and surface
      // copy only if the board ultimately rejects or cannot confirm the write.
      helperText = selected == 0
          ? litchStrings(context).clockChooseTime
          : litchStrings(context).clockReady;
    } else if (clock.setupState == HomeClockSetupState.busy) {
      helperText = litchStrings(context).clockFinishAction;
    } else if (clock.setupState == HomeClockSetupState.gameStarted) {
      helperText = clock.enabled
          ? litchStrings(context).clockNewGameChange
          : litchStrings(context).clockNewGameChoose;
    } else if (clock.enabled) {
      helperText = litchStrings(context).clockReady;
    } else {
      helperText = litchStrings(context).clockChooseTime;
    }

    final Color p1Accent = !snapshot.playerColorsConfirmed
        ? Theme.of(context).colorScheme.onSurfaceVariant
        : litchPieceAccentColor(snapshot, whitePiece: true);
    final Color p2Accent = !snapshot.playerColorsConfirmed
        ? Theme.of(context).colorScheme.onSurfaceVariant
        : litchPieceAccentColor(snapshot, whitePiece: false);
    final Widget clocks = _HomeClockStrip(
      snapshot: snapshot,
      p1Accent: p1Accent,
      p2Accent: p2Accent,
    );
    final Widget selector = _HomeClockSelector(
      selected: selected,
      enabled: enabled,
      statusAvailable: clock != null || hasOptimisticSelection,
      reading: reading,
      pending: writeInFlight,
      busy: clock?.setupState == HomeClockSetupState.busy,
      statusHint: helperText,
      onChanged: onChanged,
    );

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final bool stack =
            _usesLargeTextLayout(context) || constraints.maxWidth < 300;
        final TextStyle? helperStyle = Theme.of(context).textTheme.bodySmall
            ?.copyWith(color: Theme.of(context).colorScheme.onSurfaceVariant);
        final TextScaler textScaler = MediaQuery.textScalerOf(context);
        final double helperLineHeight = textScaler.scale(
          (helperStyle?.fontSize ?? 12) * (helperStyle?.height ?? 1.25),
        );
        final TextStyle? selectorLabelStyle = Theme.of(
          context,
        ).textTheme.labelSmall;
        final double selectorLabelHeight = textScaler.scale(
          (selectorLabelStyle?.fontSize ?? 11) *
              (selectorLabelStyle?.height ?? 1.45),
        );
        final double minimumHeight = constraints.maxWidth < 320 ? 150 : 126;
        final double measuredRowHeight =
            selectorLabelHeight +
            48 + // dropdown/static value target
            12 + // selector vertical padding
            4; // fractional glyph/layout rounding below the stack breakpoint
        final double nonStackRowHeight = measuredRowHeight > 84
            ? measuredRowHeight
            : 84;
        final double measuredHelperHeight = helperLineHeight * 2;
        final double minimumHelperHeight =
            minimumHeight - nonStackRowHeight - 6;
        final double nonStackHelperHeight =
            measuredHelperHeight > minimumHelperHeight
            ? measuredHelperHeight
            : minimumHelperHeight;
        final Widget helper = Text(
          helperText,
          key: const ValueKey<String>('home-game-clock-helper'),
          maxLines: stack ? null : 2,
          overflow: stack ? TextOverflow.visible : TextOverflow.ellipsis,
          style: helperStyle,
        );
        final Widget content = Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            if (stack) ...<Widget>[
              clocks,
              const SizedBox(height: 8),
              selector,
            ] else
              ConstrainedBox(
                constraints: BoxConstraints(minHeight: nonStackRowHeight),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Expanded(flex: 2, child: clocks),
                    const SizedBox(width: 8),
                    Expanded(child: selector),
                  ],
                ),
              ),
            const SizedBox(height: 6),
            if (stack)
              helper
            else
              SizedBox(
                height: nonStackHelperHeight,
                child: Align(alignment: Alignment.topLeft, child: helper),
              ),
          ],
        );
        // Keep a minimum row reservation while allowing translated clock
        // values to wrap. The helper retains its independent reservation.
        return content;
      },
    );
  }
}

class _KingQueenSwapShuttle extends StatefulWidget {
  const _KingQueenSwapShuttle({required this.color});

  final Color color;

  @override
  State<_KingQueenSwapShuttle> createState() => _KingQueenSwapShuttleState();
}

class _KingQueenSwapShuttleState extends State<_KingQueenSwapShuttle>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 850),
  );
  bool? _animationsDisabled;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final bool disabled = MediaQuery.disableAnimationsOf(context);
    if (_animationsDisabled == disabled) {
      return;
    }
    _animationsDisabled = disabled;
    if (disabled) {
      _controller
        ..stop()
        ..value = 0.5;
    } else {
      _controller.repeat(reverse: true);
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: SizedBox(
        key: const ValueKey<String>('king-queen-swap-shuttle'),
        width: 58,
        height: 30,
        child: Stack(
          alignment: Alignment.center,
          children: <Widget>[
            Align(
              alignment: Alignment.centerLeft,
              child: Image.asset(
                'assets/chess_pieces/cameos_v3/queen.png',
                width: 24,
                height: 24,
                color: widget.color,
                colorBlendMode: BlendMode.srcIn,
              ),
            ),
            Align(
              alignment: Alignment.centerRight,
              child: Image.asset(
                'assets/chess_pieces/cameos_v3/king.png',
                width: 24,
                height: 24,
                color: widget.color,
                colorBlendMode: BlendMode.srcIn,
              ),
            ),
            AnimatedBuilder(
              animation: _controller,
              builder: (BuildContext context, Widget? child) {
                return Transform.translate(
                  offset: Offset((_controller.value * 2 - 1) * 11, 0),
                  child: child,
                );
              },
              child: Container(
                width: 6,
                height: 6,
                decoration: BoxDecoration(
                  color: widget.color,
                  shape: BoxShape.circle,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _HomeClockSelector extends StatelessWidget {
  const _HomeClockSelector({
    required this.selected,
    required this.enabled,
    required this.statusAvailable,
    required this.reading,
    required this.pending,
    required this.busy,
    required this.statusHint,
    required this.onChanged,
  });

  final int selected;
  final bool enabled;
  final bool statusAvailable;
  final bool reading;
  final bool pending;
  final bool busy;
  final String statusHint;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    final String selectedLabel = localizedClockPresets(
      litchStrings(context),
    )[selected]!;
    final String staticValue = statusAvailable
        ? selectedLabel
        : reading
        ? litchStrings(context).syncingLabel
        : litchStrings(context).syncFailedLabel;
    final IconData staticIcon = pending || busy
        ? Icons.hourglass_top_rounded
        : statusAvailable
        ? Icons.lock_outline_rounded
        : Icons.info_outline_rounded;
    final TextStyle valueStyle = Theme.of(context).textTheme.titleMedium!
        .copyWith(color: colors.onSurface, fontWeight: FontWeight.w700);
    final Widget content = Column(
      mainAxisAlignment: MainAxisAlignment.center,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Text(
          litchStrings(context).gameClockEyebrow,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: Theme.of(context).textTheme.labelSmall?.copyWith(
            color: colors.onSurfaceVariant,
            fontWeight: FontWeight.w700,
            letterSpacing: 0.4,
          ),
        ),
        if (enabled)
          DropdownButtonHideUnderline(
            child: DropdownButton<int>(
              itemHeight: null,
              key: ValueKey<String>('home-game-clock-dropdown:$selected'),
              isExpanded: true,
              value: selected,
              style: valueStyle,
              iconEnabledColor: colors.onSurfaceVariant,
              items: localizedClockPresets(litchStrings(context)).entries
                  .map(
                    (MapEntry<int, String> preset) => DropdownMenuItem<int>(
                      value: preset.key,
                      child: Text(preset.value, style: valueStyle),
                    ),
                  )
                  .toList(),
              onChanged: (int? value) =>
                  value == null ? null : onChanged(value),
            ),
          )
        else
          ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 48),
            child: Row(
              children: <Widget>[
                Expanded(
                  child: Text(
                    staticValue,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: valueStyle,
                  ),
                ),
                Icon(staticIcon, size: 20, color: colors.onSurfaceVariant),
              ],
            ),
          ),
      ],
    );
    return Container(
      key: const ValueKey<String>('home-game-clock'),
      constraints: const BoxConstraints(minHeight: 76),
      padding: const EdgeInsets.fromLTRB(10, 7, 6, 5),
      decoration: BoxDecoration(
        color: colors.surfaceContainerLow,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: colors.outlineVariant),
      ),
      child: enabled
          ? Semantics(container: true, child: MergeSemantics(child: content))
          : Semantics(
              key: const ValueKey<String>('home-game-clock-static'),
              container: true,
              label: litchStrings(context).gameClock,
              value: staticValue,
              hint: statusHint,
              liveRegion: !statusAvailable,
              child: ExcludeSemantics(child: content),
            ),
    );
  }
}

class _HomeClockStrip extends StatelessWidget {
  const _HomeClockStrip({
    required this.snapshot,
    required this.p1Accent,
    required this.p2Accent,
  });

  final HomeBoardSnapshot snapshot;
  final Color p1Accent;
  final Color p2Accent;

  @override
  Widget build(BuildContext context) {
    final HomeClockStatus? clock = snapshot.clockStatus;
    final bool useLargeTextLayout = _usesLargeTextLayout(context);
    final HomeSide p1Side = _sideForPlayer(snapshot, 'P1');
    final HomeSide p2Side = _sideForPlayer(snapshot, 'P2');
    final Widget p1Clock = _PlayerClock(
      clockKey: 'home-clock-p1',
      label: 'P1',
      value: _clockValue(context, clock, p1Side),
      accent: p1Accent,
      active:
          clock?.enabled == true &&
          clock?.running == true &&
          clock?.activeSide == p1Side,
      paused: clock?.paused ?? false,
    );
    final Widget p2Clock = _PlayerClock(
      clockKey: 'home-clock-p2',
      label: 'P2',
      value: _clockValue(context, clock, p2Side),
      accent: p2Accent,
      active:
          clock?.enabled == true &&
          clock?.running == true &&
          clock?.activeSide == p2Side,
      paused: clock?.paused ?? false,
    );
    return Semantics(
      key: const ValueKey<String>('home-clock-strip'),
      container: true,
      label: _clockSemantics(context, snapshot),
      excludeSemantics: true,
      child: useLargeTextLayout
          ? Column(
              key: const ValueKey<String>('home-clock-strip-large-text'),
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[p1Clock, const SizedBox(height: 8), p2Clock],
            )
          : Row(
              key: const ValueKey<String>('home-clock-strip-compact'),
              children: <Widget>[
                Expanded(child: p1Clock),
                const SizedBox(width: 8),
                Expanded(child: p2Clock),
              ],
            ),
    );
  }
}

bool _usesLargeTextLayout(BuildContext context) {
  final TextScaler scaler = MediaQuery.textScalerOf(context);
  // Three clock columns cease to be readable before the conventional 150%
  // accessibility breakpoint on phone widths; stack while their labels still
  // fit rather than allowing the values to wrap inside 48 px targets.
  return scaler.scale(14) / 14 >= 1.3;
}

class _PlayerClock extends StatelessWidget {
  const _PlayerClock({
    required this.clockKey,
    required this.label,
    required this.value,
    required this.accent,
    required this.active,
    required this.paused,
  });

  final String clockKey;
  final String label;
  final String value;
  final Color accent;
  final bool active;
  final bool paused;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    final Color background = Color.alphaBlend(
      accent.withValues(alpha: 0.10),
      colors.surfaceContainerLow,
    );
    final Color visibleAccent = _clockAccentAgainst(accent, background);
    return Container(
      key: ValueKey<String>(clockKey),
      constraints: const BoxConstraints(minHeight: 76),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: background,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: visibleAccent, width: active ? 2.5 : 1.5),
      ),
      child: Row(
        children: <Widget>[
          Expanded(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  label,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: Theme.of(context).textTheme.labelSmall?.copyWith(
                    color: colors.onSurfaceVariant,
                    fontWeight: FontWeight.w700,
                    letterSpacing: 0.5,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  value,
                  style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    color: colors.onSurface,
                    fontFeatures: const <FontFeature>[
                      FontFeature.tabularFigures(),
                    ],
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ],
            ),
          ),
          if (active)
            Icon(
              paused ? Icons.pause_rounded : Icons.play_arrow_rounded,
              size: 18,
              color: visibleAccent,
            ),
        ],
      ),
    );
  }
}

Color _clockAccentAgainst(Color accent, Color background) {
  if (_contrastRatio(accent, background) >= 3) {
    return accent;
  }
  final Color target =
      _contrastRatio(Colors.black, background) >=
          _contrastRatio(Colors.white, background)
      ? Colors.black
      : Colors.white;
  for (int step = 1; step <= 10; step += 1) {
    final Color candidate = Color.lerp(accent, target, step / 10)!;
    if (_contrastRatio(candidate, background) >= 3) {
      return candidate;
    }
  }
  return target;
}

double _contrastRatio(Color first, Color second) {
  final double firstLuminance = first.computeLuminance();
  final double secondLuminance = second.computeLuminance();
  final double brighter = firstLuminance > secondLuminance
      ? firstLuminance
      : secondLuminance;
  final double darker = firstLuminance > secondLuminance
      ? secondLuminance
      : firstLuminance;
  return (brighter + 0.05) / (darker + 0.05);
}

String _clockValue(
  BuildContext context,
  HomeClockStatus? clock,
  HomeSide side,
) {
  if (clock == null) {
    return '—';
  }
  if (!clock.enabled) {
    return litchStrings(context).noClock;
  }
  final int seconds = side == HomeSide.white
      ? clock.whiteRemainingSeconds
      : clock.blackRemainingSeconds;
  final int minutes = seconds ~/ 60;
  final int remainder = seconds % 60;
  return '$minutes:${remainder.toString().padLeft(2, '0')}';
}

HomeSide _sideForPlayer(HomeBoardSnapshot snapshot, String player) =>
    snapshot.playerForSide(HomeSide.white) == player
    ? HomeSide.white
    : HomeSide.black;

String _clockSemantics(BuildContext context, HomeBoardSnapshot snapshot) {
  final HomeClockStatus? clock = snapshot.clockStatus;
  if (clock == null) {
    return snapshot.isRefreshing || snapshot.isClockRefreshing
        ? litchStrings(context).clockSyncingSemantics
        : litchStrings(context).clockSyncFailedSemantics;
  }
  if (!clock.enabled) {
    return litchStrings(context).noGameClock;
  }
  final String state = !clock.running
      ? litchStrings(context).clockStopped
      : clock.paused
      ? litchStrings(context).clockPaused
      : litchStrings(context).clockRunning;
  final HomeSide p1Side = _sideForPlayer(snapshot, 'P1');
  final HomeSide p2Side = _sideForPlayer(snapshot, 'P2');
  return litchStrings(context).clockSemantics(
    _clockValue(context, clock, p1Side),
    _clockValue(context, clock, p2Side),
    _spokenSideText(context, snapshot.playerForSide(clock.activeSide)),
    state,
  );
}

class _GameContext extends StatelessWidget {
  const _GameContext({required this.snapshot});

  final HomeBoardSnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    final HomeGameStatus? game = snapshot.gameStatus;
    return Row(
      key: const ValueKey<String>('home-quiet-game-context'),
      mainAxisAlignment: MainAxisAlignment.start,
      children: <Widget>[
        if (game?.variant.toLowerCase() == 'duck') ...<Widget>[
          Image.asset(
            HomeChessboard.duckAsset,
            key: const ValueKey<String>('duck-variant-icon'),
            width: 18,
            height: 18,
            cacheWidth: 64,
            excludeFromSemantics: true,
          ),
          const SizedBox(width: 6),
        ],
        Flexible(
          child: Text(
            _gameContextLabel(context, snapshot),
            textAlign: TextAlign.start,
            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
              color: const Color(0xFFD6DEDD),
              fontWeight: FontWeight.w600,
            ),
          ),
        ),
      ],
    );
  }
}

String _gameContextLabel(BuildContext context, HomeBoardSnapshot snapshot) {
  final String variant = switch (snapshot.gameStatus?.variant.toLowerCase()) {
    'classic' => litchStrings(context).classicVariant,
    'duck' => litchStrings(context).duckVariant,
    _ => litchStrings(context).unknownVariant,
  };
  final int move = snapshot.position?.fullmoveNumber ?? 1;
  return litchStrings(context).gameContext(variant, move);
}

class _MoveHistoryPanel extends StatelessWidget {
  const _MoveHistoryPanel({required this.snapshot});

  final HomeBoardSnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    final int? moveNumber = snapshot.position?.fullmoveNumber;
    final HomeLastMoves? lastMoves = snapshot.lastMoves;
    final bool hasMoves = lastMoves?.white != null || lastMoves?.black != null;
    return Material(
      color: colors.surfaceContainerLow,
      shape: LitchShapes.panel(outline: colors.outlineVariant),
      clipBehavior: Clip.antiAlias,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 14, 14, 16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              litchStrings(context).movesTitle,
              style: Theme.of(
                context,
              ).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w700),
            ),
            const SizedBox(height: 2),
            Text(
              hasMoves
                  ? litchStrings(context).latestBoardMoves
                  : moveNumber == null
                  ? litchStrings(context).currentGame
                  : litchStrings(context).currentGameMove(moveNumber),
              style: Theme.of(
                context,
              ).textTheme.bodySmall?.copyWith(color: colors.onSurfaceVariant),
            ),
            const SizedBox(height: 12),
            if (hasMoves)
              LayoutBuilder(
                builder: (BuildContext context, BoxConstraints constraints) {
                  final bool stack =
                      constraints.maxWidth < 320 ||
                      MediaQuery.textScalerOf(context).scale(14) >= 21;
                  final List<Widget> moves = <Widget>[
                    if (lastMoves?.white case final HomeMoveSquares move)
                      _LatestMove(
                        side: snapshot.playerForSide(HomeSide.white),
                        from: algebraicSquare(move.fromSquare),
                        to: algebraicSquare(move.toSquare),
                        mostRecent: lastMoves?.mostRecent == HomeSide.white,
                      ),
                    if (lastMoves?.black case final HomeMoveSquares move)
                      _LatestMove(
                        side: snapshot.playerForSide(HomeSide.black),
                        from: algebraicSquare(move.fromSquare),
                        to: algebraicSquare(move.toSquare),
                        mostRecent: lastMoves?.mostRecent == HomeSide.black,
                      ),
                  ];
                  if (stack) {
                    return Column(
                      children: moves
                          .map(
                            (Widget move) => Padding(
                              padding: const EdgeInsets.only(bottom: 8),
                              child: move,
                            ),
                          )
                          .toList(),
                    );
                  }
                  return Row(
                    children: <Widget>[
                      for (
                        int index = 0;
                        index < moves.length;
                        index += 1
                      ) ...<Widget>[
                        if (index > 0) const SizedBox(width: 8),
                        Expanded(child: moves[index]),
                      ],
                    ],
                  );
                },
              )
            else
              Text(
                litchStrings(context).noMoveHistory,
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                  color: colors.onSurfaceVariant,
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _LatestMove extends StatelessWidget {
  const _LatestMove({
    required this.side,
    required this.from,
    required this.to,
    required this.mostRecent,
  });

  final String side;
  final String from;
  final String to;
  final bool mostRecent;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    return Semantics(
      container: true,
      label: litchStrings(context).latestMoveSemantics(
        _spokenSideText(context, side),
        from,
        to,
        mostRecent ? litchStrings(context).mostRecentSuffix : '',
      ),
      excludeSemantics: true,
      child: Material(
        color: mostRecent
            ? colors.primary.withValues(alpha: 0.1)
            : colors.surfaceContainer,
        shape: LitchShapes.panel(),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
          child: Row(
            children: <Widget>[
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      mostRecent
                          ? litchStrings(context).latestMoveLabel(side)
                          : side,
                      style: Theme.of(context).textTheme.labelSmall?.copyWith(
                        color: mostRecent
                            ? colors.primary
                            : colors.onSurfaceVariant,
                        fontWeight: FontWeight.w700,
                        letterSpacing: 0.5,
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      '$from → $to',
                      style: Theme.of(context).textTheme.titleMedium?.copyWith(
                        fontWeight: FontWeight.w700,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

String _spokenSideText(BuildContext context, String text) => text
    .replaceAll(RegExp(r'\bP1\b'), litchStrings(context).playerSpoken('1'))
    .replaceAll(RegExp(r'\bP2\b'), litchStrings(context).playerSpoken('2'));

bool _healthPausesPlay(HomeBoardSnapshot snapshot) =>
    switch (snapshot.runtimeHealth) {
      RuntimeHealthAvailable(:final snapshot) =>
        snapshot.state == RuntimeHealthState.inputPaused,
      _ => false,
    };

String? _syncDescription(BuildContext context, HomeBoardSnapshot snapshot) {
  if (snapshot.gameStatus?.isFinished ?? false) {
    return null;
  }
  if (_healthPausesPlay(snapshot)) {
    return litchStrings(context).healthPauseGuidance;
  }

  if (snapshot.position == null) {
    return snapshot.isRefreshing
        ? null
        : litchStrings(context).positionRetryFailed;
  }

  if (snapshot.isDuckRelocationCorrection) {
    return null;
  }

  if (snapshot.isDuckLandingCorrection) {
    return litchStrings(context).removePieces(snapshot.guidedSquares.length);
  }

  if (snapshot.vexMoveFirst) {
    return null;
  }

  final capture = snapshot.captureRemovalSquare;
  if (snapshot.homeStateAction != HomeStateAction.cpu &&
      snapshot.physicalMovePrompt != null &&
      capture != null) {
    return litchStrings(context).removeCapturePiece(algebraicSquare(capture));
  }

  if (snapshot.homeStateAction == HomeStateAction.settle) {
    return null;
  }

  if (snapshot.showsCpuThinking) {
    final int? remaining = snapshot.cpuThinking?.remainingSeconds;
    return remaining == null
        ? null
        : litchStrings(context).cpuThinkingRemaining(remaining);
  }

  if (snapshot.gameStatus?.duckMovePending ?? false) {
    final HomeDuckRelocationPrompt? prompt = snapshot.visibleDuckRelocation;
    if (prompt?.toSquare case final int destination) {
      return litchStrings(context).duckMoveGuidance(
        algebraicSquare(prompt!.fromSquare),
        algebraicSquare(destination),
      );
    }
    if (prompt?.owner == HomeDuckRelocationOwner.human) {
      return localizedAssistance(litchStrings(context), snapshot) ??
          litchStrings(context).duckChooseDestination;
    }
    return localizedAssistance(litchStrings(context), snapshot) ??
        litchStrings(context).followBoardGuidance;
  }

  if (snapshot.homeStateAction == HomeStateAction.cpu) {
    final HomePhysicalMovePrompt? prompt = snapshot.physicalMovePrompt;
    if (prompt == null) {
      return litchStrings(context).cpuFollowLights;
    }
    final String from = algebraicSquare(prompt.fromSquare);
    final String to = algebraicSquare(prompt.toSquare);
    final int? captureSquare = snapshot.captureRemovalSquare;
    if (captureSquare != null) {
      return litchStrings(
        context,
      ).captureMove(algebraicSquare(captureSquare), from, to);
    }
    return litchStrings(context).cpuMoveGuidance(from, to);
  }

  if (snapshot.homeStateAction == HomeStateAction.promote) {
    final prompt = snapshot.pendingPromotionPrompt;
    if (prompt != null && snapshot.promotionPlacementPending) {
      return litchStrings(
        context,
      ).promotionPlacePiece(algebraicSquare(prompt.toSquare));
    }
    return litchStrings(context).promotionGuidance;
  }

  if (snapshot.isGuidedKingQueenSwap) {
    return litchStrings(context).swapGuidance;
  }

  if (snapshot.isFreshSetupInProgress) {
    final int extraCount = snapshot.unexpectedOccupiedSquares.length;
    if (extraCount != 0) {
      return litchStrings(context).removePieces(extraCount);
    }
    final int missingCount = snapshot.missingExpectedSquares.length;
    if (missingCount != 0) {
      return litchStrings(context).placePieces(missingCount);
    }
    if (snapshot.physicalStatus?.placementMode == true) {
      return litchStrings(context).setupChecking;
    }
    return null;
  }

  if (snapshot.watchdogRestart &&
      (!snapshot.isLegalLiveProgress || snapshot.cpuThinking != null) &&
      snapshot.physicalPositionMatches != false) {
    return litchStrings(context).restartGuidance;
  }
  if (localizedAssistance(litchStrings(context), snapshot)
      case final String guidance) {
    return guidance;
  }
  final bool? matches = snapshot.physicalPositionMatches;
  if (matches == null) {
    return litchStrings(context).positionReceived;
  }
  if (matches) {
    return null;
  }
  if (snapshot.isExpectedBoardInteraction) {
    return null;
  }
  final String? primaryAssistance = snapshot.liveSequence == null
      ? null
      : localizedCorrection(litchStrings(context), snapshot);
  if (primaryAssistance != null) {
    return '$primaryAssistance.';
  }
  final int count = snapshot.mismatchedSquares.length;
  return litchStrings(context).guidedSquares(count);
}
