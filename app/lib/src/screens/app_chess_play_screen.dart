import 'dart:async';

import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';

import '../controllers/app_chess_game_session.dart';
import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/game_archive.dart';
import '../models/home_board_snapshot.dart';
import '../models/litch_clock_preset.dart';
import '../services/game_library_repository.dart';
import '../widgets/home_chessboard.dart';
import '../widgets/litch_workspace_geometry.dart';
import 'post_game_review_layout.dart';

typedef AppChessGameLibraryOpener = Future<GameLibraryRepository> Function();

class AppChessPlayRouteScreen extends StatefulWidget {
  const AppChessPlayRouteScreen({
    super.key,
    this.repository,
    this.openRepository,
    this.initialArchive,
  }) : assert(repository != null || openRepository != null);

  final GameLibraryRepository? repository;
  final AppChessGameLibraryOpener? openRepository;
  final GameArchive? initialArchive;

  @override
  State<AppChessPlayRouteScreen> createState() =>
      _AppChessPlayRouteScreenState();
}

class _AppChessPlayRouteScreenState extends State<AppChessPlayRouteScreen> {
  GameLibraryRepository? _repository;
  Object? _openError;
  bool _opening = false;
  int _generation = 0;

  @override
  void initState() {
    super.initState();
    _repository = widget.repository;
    if (_repository == null) unawaited(_open());
  }

  Future<void> _open() async {
    if (_opening || widget.openRepository == null) return;
    final int generation = ++_generation;
    setState(() {
      _opening = true;
      _openError = null;
    });
    try {
      final GameLibraryRepository repository = await widget.openRepository!();
      if (!mounted || generation != _generation) return;
      setState(() {
        _repository = repository;
        _opening = false;
      });
    } catch (error) {
      if (!mounted || generation != _generation) return;
      setState(() {
        _opening = false;
        _openError = error;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final GameLibraryRepository? repository = _repository;
    if (repository != null) {
      return AppChessPlayScreen(
        repository: repository,
        initialArchive: widget.initialArchive,
      );
    }
    final strings = litchStrings(context);
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(title: Text(strings.appChessTitle)),
      ),
      body: SafeArea(
        child: Center(
          child: _opening && _openError == null
              ? Semantics(
                  liveRegion: true,
                  label: strings.gameLibraryLoading,
                  child: const CircularProgressIndicator(),
                )
              : SingleChildScrollView(
                  padding: const EdgeInsets.all(24),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: <Widget>[
                      Text(
                        strings.gameLibraryUnavailableTitle,
                        style: Theme.of(context).textTheme.titleLarge,
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 8),
                      Text(
                        strings.gameLibraryUnavailableBody,
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 16),
                      LitchCompactControls(
                        child: FilledButton(
                          onPressed: _open,
                          child: Text(strings.tryAgain),
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

class AppChessPlayScreen extends StatefulWidget {
  const AppChessPlayScreen({
    super.key,
    required this.repository,
    this.initialArchive,
  });

  final GameLibraryRepository repository;
  final GameArchive? initialArchive;

  @override
  State<AppChessPlayScreen> createState() => _AppChessPlayScreenState();
}

class _AppChessPlayScreenState extends State<AppChessPlayScreen>
    with WidgetsBindingObserver {
  late AppChessGameSession _session;
  int? _selectedSquare;
  Map<int, List<String>> _movesByDestination = const <int, List<String>>{};
  bool _allowPop = false;
  bool _exitPending = false;
  bool _newGamePending = false;
  bool _retryingSave = false;
  bool _showIllegalMove = false;
  Timer? _clockTimer;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _session = _createSession(widget.initialArchive);
    _session.addListener(_onSessionChanged);
    scheduleMicrotask(_syncClockTimer);
  }

  AppChessGameSession _createSession(GameArchive? archive) {
    if (archive != null) {
      return AppChessGameSession.restore(
        archive: archive,
        now: DateTime.now,
        persist: widget.repository.save,
      );
    }
    return AppChessGameSession.newGame(
      identity: AppChessGameIdentity.generate(),
      now: DateTime.now,
      persist: widget.repository.save,
    );
  }

  void _onSessionChanged() {
    if (!mounted) return;
    setState(() {});
    _syncClockTimer();
    if (_session.saveFailed &&
        _session.clock?.runState == ArchivedGameClockRunState.running) {
      scheduleMicrotask(() {
        if (mounted && _session.saveFailed) _session.suspendClock();
      });
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _clockTimer?.cancel();
    _session.removeListener(_onSessionChanged);
    _session.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.resumed) _session.suspendClock();
  }

  HomeChessPosition get _position =>
      HomeChessPosition.tryParse(_session.archive.positions.last.fen)!;

  bool get _clockSuspended =>
      _session.clock?.runState == ArchivedGameClockRunState.suspended;

  Future<void> _pressSquare(int square) async {
    if (_session.isGameOver || _clockSuspended) {
      return;
    }
    final int? selected = _selectedSquare;
    if (selected == square) {
      setState(_clearSelection);
      return;
    }

    final List<String>? destinationMoves = _movesByDestination[square];
    if (selected != null && destinationMoves != null) {
      String? move;
      if (destinationMoves.length == 1) {
        move = destinationMoves.single;
      } else {
        move = await _choosePromotion(destinationMoves);
      }
      if (!mounted || move == null) return;
      final AppChessMoveResult result = _session.playUci(move);
      setState(() {
        _clearSelection();
        _showIllegalMove = result == AppChessMoveResult.illegal;
      });
      return;
    }

    final List<String> legal = _session.legalUciMovesFrom(square);
    if (legal.isNotEmpty) {
      _session.startClock();
      final Map<int, List<String>> grouped = <int, List<String>>{};
      for (final String move in legal) {
        final int destination = _squareIndex(move.substring(2, 4));
        grouped.putIfAbsent(destination, () => <String>[]).add(move);
      }
      setState(() {
        _selectedSquare = square;
        _movesByDestination = grouped;
        _showIllegalMove = false;
      });
      return;
    }

    setState(() => _showIllegalMove = selected != null);
  }

  Future<String?> _choosePromotion(List<String> moves) {
    final strings = litchStrings(context);
    return showModalBottomSheet<String>(
      context: context,
      showDragHandle: true,
      builder: (BuildContext context) => SafeArea(
        child: LitchCompactControls(
          child: ListView(
            shrinkWrap: true,
            padding: EdgeInsets.zero,
            children: <Widget>[
              Semantics(
                header: true,
                child: ListTile(title: Text(strings.appChessPromotionTitle)),
              ),
              for (final String move in moves)
                ListTile(
                  key: ValueKey<String>('app-chess-promotion-${move[4]}'),
                  minTileHeight: 48,
                  title: Text(switch (move[4]) {
                    'q' => strings.reviewPieceQueen,
                    'r' => strings.reviewPieceRook,
                    'b' => strings.reviewPieceBishop,
                    'n' => strings.reviewPieceKnight,
                    _ => move[4],
                  }),
                  onTap: () => Navigator.of(context).pop(move),
                ),
            ],
          ),
        ),
      ),
    );
  }

  void _clearSelection() {
    _selectedSquare = null;
    _movesByDestination = const <int, List<String>>{};
    _showIllegalMove = false;
  }

  void _undo() {
    if (!_session.undo()) return;
    setState(_clearSelection);
  }

  bool get _canUndoHumanDecision => _session.canUndo;

  int _selectedClockPreset() {
    final ArchivedGameClock? clock = _session.archive.clock;
    if (clock == null) return 0;
    for (final int preset in LitchClockPresets.baseSeconds.keys) {
      if (LitchClockPresets.baseSeconds[preset]! * 1000 !=
          clock.baseMilliseconds) {
        continue;
      }
      final int increment = switch (preset) {
        2 => 2,
        5 => 10,
        _ => 0,
      };
      if (increment * 1000 == clock.incrementMilliseconds) return preset;
    }
    return 0;
  }

  AppChessClockConfiguration? _configurationForPreset(int preset) {
    final int seconds = LitchClockPresets.baseSeconds[preset] ?? 0;
    if (seconds == 0) return null;
    return AppChessClockConfiguration(
      base: Duration(seconds: seconds),
      increment: Duration(
        seconds: switch (preset) {
          2 => 2,
          5 => 10,
          _ => 0,
        },
      ),
    );
  }

  Future<void> _chooseClock() async {
    final ArchivedGameClockRunState? runState =
        _session.archive.clock?.runState;
    if (!_session.canChangePlayers ||
        (runState != null && runState != ArchivedGameClockRunState.ready)) {
      return;
    }
    final strings = litchStrings(context);
    final Map<int, String> labels = localizedClockPresets(strings);
    final int selected = _selectedClockPreset();
    final int? choice = await showModalBottomSheet<int>(
      context: context,
      showDragHandle: true,
      builder: (BuildContext context) => SafeArea(
        child: LitchCompactControls(
          child: ListView(
            shrinkWrap: true,
            padding: EdgeInsets.zero,
            children: <Widget>[
              Semantics(
                header: true,
                child: ListTile(title: Text(strings.gameClock)),
              ),
              for (final MapEntry<int, String> entry in labels.entries)
                Semantics(
                  selected: entry.key == selected,
                  child: ListTile(
                    key: ValueKey<String>('app-chess-clock-${entry.key}'),
                    selected: entry.key == selected,
                    minTileHeight: 48,
                    leading: Icon(
                      entry.key == selected
                          ? Icons.radio_button_checked
                          : Icons.radio_button_unchecked,
                    ),
                    title: Text(entry.key == 0 ? strings.noClock : entry.value),
                    onTap: () => Navigator.pop(context, entry.key),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
    if (!mounted || choice == null || choice == selected) return;
    _session.setClockConfiguration(_configurationForPreset(choice));
  }

  void _resumeGame() {
    _session.resumeClock();
  }

  String _formatClock(Duration duration) {
    final int totalSeconds = (duration.inMilliseconds / 1000).ceil();
    final int minutes = totalSeconds ~/ 60;
    final int seconds = totalSeconds.remainder(60);
    return '$minutes:${seconds.toString().padLeft(2, '0')}';
  }

  Widget _clockStrip(BuildContext context) {
    final strings = litchStrings(context);
    final AppChessClockView? clock = _session.clock;
    final Map<int, String> labels = localizedClockPresets(strings);
    if (clock == null) {
      return const SizedBox.shrink();
    }
    final bool running = clock.runState == ArchivedGameClockRunState.running;
    final bool suspended =
        clock.runState == ArchivedGameClockRunState.suspended;
    final String state = switch (clock.runState) {
      ArchivedGameClockRunState.ready => strings.clockReady,
      ArchivedGameClockRunState.running => strings.clockRunning,
      ArchivedGameClockRunState.suspended => strings.clockPaused,
      ArchivedGameClockRunState.stopped => strings.clockStopped,
    };
    final TextStyle playerStyle = DefaultTextStyle.of(context).style;
    final TextStyle timeStyle = playerStyle.merge(
      Theme.of(context).textTheme.titleMedium?.copyWith(
        fontFeatures: const <FontFeature>[FontFeature.tabularFigures()],
      ),
    );
    double textWidth(String text, TextStyle style) {
      final painter = TextPainter(
        text: TextSpan(text: text, style: style),
        textDirection: Directionality.of(context),
        textScaler: MediaQuery.textScalerOf(context),
        locale: Localizations.maybeLocaleOf(context),
      )..layout();
      final width = painter.width;
      painter.dispose();
      return width;
    }

    Widget cell(GameSide side, String player, Duration remaining) {
      final bool active = clock.sideToMove == side && running;
      final ColorScheme colors = Theme.of(context).colorScheme;
      final time = _formatClock(remaining);
      return Semantics(
        label: '$player, ${_formatClock(remaining)}, $state',
        child: Container(
          key: ValueKey<String>(
            side == GameSide.white
                ? 'app-chess-clock-p1'
                : 'app-chess-clock-p2',
          ),
          constraints: const BoxConstraints(minHeight: 56),
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 7),
          decoration: BoxDecoration(
            color: active ? colors.primaryContainer : colors.surfaceContainer,
            border: Border.all(
              color: active ? colors.primary : colors.outlineVariant,
              width: active ? 2 : 1,
            ),
            borderRadius: BorderRadius.circular(12),
          ),
          child: LayoutBuilder(
            builder: (context, constraints) {
              final playerLabel = Row(
                children: <Widget>[
                  if (active) ...<Widget>[
                    const Icon(Icons.play_arrow_rounded, size: 18),
                    const SizedBox(width: 2),
                  ],
                  Expanded(child: Text(player)),
                ],
              );
              final timeLabel = Text(time, style: timeStyle);
              final inlineWidth =
                  textWidth(player, playerStyle) +
                  (active ? 20 : 0) +
                  8 +
                  textWidth(time, timeStyle);
              if (inlineWidth <= constraints.maxWidth) {
                return Row(
                  children: <Widget>[
                    Expanded(child: playerLabel),
                    const SizedBox(width: 8),
                    timeLabel,
                  ],
                );
              }
              return Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: <Widget>[
                  playerLabel,
                  const SizedBox(height: 4),
                  Align(
                    alignment: AlignmentDirectional.centerEnd,
                    child: timeLabel,
                  ),
                ],
              );
            },
          ),
        ),
      );
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        LayoutBuilder(
          builder: (context, constraints) {
            final white = cell(
              GameSide.white,
              strings.reviewWhite,
              clock.whiteRemaining,
            );
            final black = cell(
              GameSide.black,
              strings.reviewBlack,
              clock.blackRemaining,
            );
            final halfCellContentWidth = (constraints.maxWidth - 8) / 2 - 28;
            final timesFit =
                textWidth(_formatClock(clock.whiteRemaining), timeStyle) <=
                    halfCellContentWidth &&
                textWidth(_formatClock(clock.blackRemaining), timeStyle) <=
                    halfCellContentWidth;
            return timesFit
                ? Row(
                    children: <Widget>[
                      Expanded(child: white),
                      const SizedBox(width: 8),
                      Expanded(child: black),
                    ],
                  )
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: <Widget>[white, const SizedBox(height: 8), black],
                  );
          },
        ),
        if (suspended) ...<Widget>[
          const SizedBox(height: 8),
          LitchCompactControls(
            child: FilledButton.icon(
              key: const ValueKey<String>('app-chess-clock-resume'),
              onPressed: _resumeGame,
              icon: const Icon(Icons.play_arrow_rounded),
              label: Text(strings.resumeGame),
            ),
          ),
        ] else if (_session.canChangePlayers &&
            clock.runState == ArchivedGameClockRunState.ready) ...<Widget>[
          const SizedBox(height: 4),
          LitchCompactControls(
            child: TextButton.icon(
              key: const ValueKey<String>('app-chess-clock-picker'),
              onPressed: _chooseClock,
              icon: const Icon(Icons.timer_outlined),
              label: Text(labels[_selectedClockPreset()] ?? strings.gameClock),
            ),
          ),
        ],
      ],
    );
  }

  Future<void> _newGame() async {
    if (_newGamePending) return;
    final strings = litchStrings(context);
    final bool start =
        await showDialog<bool>(
          context: context,
          builder: (BuildContext context) => AlertDialog(
            title: Text(strings.newGameQuestion),
            content: Text(strings.appChessNewGameBody),
            actions: <Widget>[
              LitchCompactControls(
                child: TextButton(
                  onPressed: () => Navigator.of(context).pop(false),
                  child: Text(strings.cancelAction),
                ),
              ),
              LitchCompactControls(
                child: FilledButton(
                  onPressed: () => Navigator.of(context).pop(true),
                  child: Text(strings.startNewGame),
                ),
              ),
            ],
          ),
        ) ??
        false;
    if (!mounted || !start) return;
    final AppChessGameSession previous = _session;
    setState(() => _newGamePending = true);
    previous.suspendClock();
    await previous.settlePersistence();
    if (!mounted) return;
    if (previous.saveFailed) {
      setState(() => _newGamePending = false);
      return;
    }
    previous.removeListener(_onSessionChanged);
    setState(() {
      _session = _createSession(null);
      _session.addListener(_onSessionChanged);
      _newGamePending = false;
      _clearSelection();
    });
    previous.dispose();
  }

  Future<void> _retrySave() async {
    if (_retryingSave) return;
    setState(() => _retryingSave = true);
    await _session.retryPersistence();
    if (mounted) setState(() => _retryingSave = false);
  }

  Future<void> _handleBlockedPop() async {
    if (_exitPending) return;
    _session.suspendClock();
    setState(() => _exitPending = true);
    await _session.settlePersistence();
    if (!mounted) return;
    if (_session.saveFailed) {
      setState(() => _exitPending = false);
      return;
    }
    setState(() => _allowPop = true);
    Navigator.of(context).pop();
  }

  void _requestExit() {
    unawaited(_handleBlockedPop());
  }

  void _syncClockTimer() {
    _clockTimer?.cancel();
    _clockTimer = null;
    if (!mounted) return;
    final Duration? remaining = _session.timeUntilTimeout;
    if (remaining == null) return;
    final Duration delay = remaining <= Duration.zero
        ? Duration.zero
        : remaining < const Duration(seconds: 1)
        ? remaining
        : const Duration(seconds: 1);
    _clockTimer = Timer(delay, _clockTick);
  }

  void _clockTick() {
    if (!mounted) return;
    final Duration? remaining = _session.timeUntilTimeout;
    if (remaining != null && remaining <= Duration.zero) {
      _session.settleClock();
      return;
    }
    setState(() {});
    _syncClockTimer();
  }

  String _guidance(BuildContext context) {
    final strings = litchStrings(context);
    if (_session.isGameOver) {
      return switch (_session.archive.winner) {
        GameWinner.white => strings.gameLibraryWhiteWon,
        GameWinner.black => strings.gameLibraryBlackWon,
        _ => strings.gameDraw,
      };
    }
    if (_showIllegalMove) return strings.appChessIllegalMove;
    if (_selectedSquare case final int square) {
      return strings.appChessChooseDestination(algebraicSquare(square));
    }
    return strings.appChessChoosePiece(
      _position.sideToMove == HomeSide.white
          ? strings.reviewWhite
          : strings.reviewBlack,
    );
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final Size screenSize = MediaQuery.sizeOf(context);
    final tablet = LitchWorkspaceGeometry.isTabletWindow(screenSize);
    final compactLandscape =
        screenSize.width > screenSize.height && screenSize.height <= 480;
    final Set<int> destinations = _movesByDestination.keys.toSet();
    final Widget board = HomeChessboard(
      snapshot: HomeBoardSnapshot(position: _position),
      maxSize: tablet ? double.infinity : 520,
      selectedSquare: _selectedSquare,
      legalDestinations: destinations,
      onSquarePressed: _clockSuspended ? null : _pressSquare,
    );
    final Widget controls = SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          if (compactLandscape) ...<Widget>[
            LitchCompactControls(
              child: Row(
                children: <Widget>[
                  IconButton(
                    key: const ValueKey<String>('app-chess-landscape-back'),
                    tooltip: MaterialLocalizations.of(
                      context,
                    ).backButtonTooltip,
                    onPressed: _requestExit,
                    icon: const Icon(Icons.arrow_back_rounded),
                  ),
                  const SizedBox(width: 4),
                  Expanded(
                    child: Text(
                      strings.appChessTitle,
                      style: Theme.of(context).textTheme.titleLarge,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 4),
          ],
          if (_session.clock != null) ...<Widget>[
            _clockStrip(context),
            const SizedBox(height: 12),
          ],
          Semantics(
            liveRegion: true,
            child: Text(
              _guidance(context),
              key: const ValueKey<String>('app-chess-guidance'),
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ),
          if (_session.saveFailed) ...<Widget>[
            const SizedBox(height: 12),
            Card(
              color: Theme.of(context).colorScheme.errorContainer,
              child: Padding(
                padding: const EdgeInsets.all(12),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    Text(strings.appChessSaveFailed),
                    const SizedBox(height: 8),
                    Align(
                      alignment: Alignment.centerLeft,
                      child: LitchCompactControls(
                        child: FilledButton.tonal(
                          onPressed: _retryingSave ? null : _retrySave,
                          child: Text(strings.appChessRetrySave),
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
          const SizedBox(height: 12),
          LitchCompactControls(
            child: Wrap(
              spacing: 8,
              runSpacing: 8,
              children: <Widget>[
                OutlinedButton.icon(
                  key: const ValueKey<String>('app-chess-undo'),
                  onPressed: _canUndoHumanDecision ? _undo : null,
                  icon: const Icon(Icons.undo_rounded),
                  label: Text(strings.undoAction),
                ),
                if (_session.archive.clock == null && _session.canChangePlayers)
                  OutlinedButton.icon(
                    key: const ValueKey<String>('app-chess-clock-picker'),
                    onPressed: _chooseClock,
                    icon: const Icon(Icons.timer_outlined),
                    label: Text('${strings.gameClock} · ${strings.noClock}'),
                  ),
                TextButton.icon(
                  key: const ValueKey<String>('app-chess-new-game'),
                  onPressed: _newGamePending ? null : _newGame,
                  icon: const Icon(Icons.add_rounded),
                  label: Text(strings.newGame),
                ),
              ],
            ),
          ),
        ],
      ),
    );

    return PopScope<Object?>(
      canPop: _allowPop,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (!didPop) unawaited(_handleBlockedPop());
      },
      child: Scaffold(
        appBar: compactLandscape
            ? null
            : LitchCompactAppBar(
                child: AppBar(title: Text(strings.appChessTitle)),
              ),
        body: SafeArea(
          child: GamePositionLayout(
            board: board,
            controls: controls,
            horizontalPadding: LitchWorkspaceGeometry.contentEdgeInset,
            columnsKey: const ValueKey<String>('app-chess-landscape'),
            singlePaneBuilder: (context, pane) {
              if (compactLandscape) {
                return Row(
                  key: const ValueKey<String>('app-chess-landscape'),
                  children: <Widget>[
                    SizedBox.square(dimension: pane.height, child: board),
                    Expanded(child: controls),
                  ],
                );
              }
              return Column(
                key: const ValueKey<String>('app-chess-portrait'),
                children: <Widget>[
                  if (tablet)
                    SizedBox.square(
                      dimension: LitchWorkspaceGeometry.tabletCanvasSide(
                        pane,
                        columns: false,
                      ),
                      child: board,
                    )
                  else
                    Flexible(flex: 3, child: board),
                  Flexible(
                    flex: 2,
                    child: tablet
                        ? ConstrainedBox(
                            constraints: const BoxConstraints(
                              maxWidth:
                                  LitchWorkspaceGeometry.readableContentWidth,
                            ),
                            child: controls,
                          )
                        : controls,
                  ),
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}

int _squareIndex(String square) =>
    (8 - int.parse(square[1])) * 8 + square.codeUnitAt(0) - 97;
