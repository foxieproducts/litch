import 'dart:async';

import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';

import '../localization/litch_strings.dart';
import '../models/game_archive.dart';
import '../models/home_board_snapshot.dart';
import '../services/post_game_review_analyzer.dart';
import '../widgets/home_chessboard.dart';
import 'game_archive_labels.dart';
import 'post_game_review_layout.dart';

part 'post_game_review_components.dart';

/// Replays a durable library archive and, when supplied, bounded local analysis.
class PostGameReviewScreen extends StatefulWidget {
  const PostGameReviewScreen({
    super.key,
    required this.archive,
    this.reviewOwner,
  });

  final GameArchive archive;
  final PostGameReviewJobOwner? reviewOwner;

  @override
  State<PostGameReviewScreen> createState() => _PostGameReviewScreenState();
}

enum _AnalysisState { unavailable, loading, ready, failed }

class _PostGameReviewScreenState extends State<PostGameReviewScreen> {
  final GlobalKey _positionSummaryKey = GlobalKey();
  late int _selectedIndex;
  bool _rotated = false;
  _AnalysisState _analysisState = _AnalysisState.unavailable;
  PostGameReviewAnalysis? _analysis;
  PostGameReviewJob? _analysisJob;

  PostGameReviewTimeline get _timeline =>
      PostGameReviewTimeline(widget.archive);

  bool get _analysisEligible =>
      widget.archive.variant == GameVariant.classic &&
      widget.archive.status != GameArchiveStatus.active &&
      widget.archive.captureQuality == GameCaptureQuality.complete;

  @override
  void initState() {
    super.initState();
    _selectedIndex = _timeline.lastIndex;
    unawaited(_loadAnalysis());
  }

  @override
  void didUpdateWidget(PostGameReviewScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    final bool archiveChanged =
        oldWidget.archive.libraryId != widget.archive.libraryId ||
        oldWidget.archive.gameId != widget.archive.gameId ||
        oldWidget.archive.origin != widget.archive.origin ||
        oldWidget.archive.lineRevision != widget.archive.lineRevision ||
        oldWidget.archive.currentPly != widget.archive.currentPly ||
        oldWidget.archive.status != widget.archive.status ||
        oldWidget.archive.captureQuality != widget.archive.captureQuality ||
        oldWidget.archive.variant != widget.archive.variant;
    if (!archiveChanged &&
        identical(oldWidget.reviewOwner, widget.reviewOwner)) {
      return;
    }
    if (archiveChanged) {
      _selectedIndex = _timeline.lastIndex;
      _rotated = false;
    }
    unawaited(_loadAnalysis());
  }

  Future<void> _loadAnalysis() async {
    _analysisJob?.cancel();
    _analysisJob = null;
    final PostGameReviewJobOwner? reviewOwner = widget.reviewOwner;
    if (!_analysisEligible || reviewOwner == null) {
      if (mounted) {
        setState(() {
          _analysisState = _AnalysisState.unavailable;
          _analysis = null;
        });
      }
      return;
    }
    final PostGameReviewJob job = reviewOwner.start(widget.archive);
    _analysisJob = job;

    if (mounted) {
      setState(() {
        _analysisState = _AnalysisState.loading;
        _analysis = null;
      });
    }
    try {
      final PostGameReviewAnalysis result = await job.result;
      if (!mounted || !identical(job, _analysisJob)) {
        return;
      }
      if (!result.matches(widget.archive)) {
        setState(() => _analysisState = _AnalysisState.failed);
        return;
      }
      setState(() {
        _analysisState = _AnalysisState.ready;
        _analysis = result;
      });
    } catch (_) {
      if (!mounted || !identical(job, _analysisJob)) {
        return;
      }
      setState(() {
        _analysisState = _AnalysisState.failed;
        _analysis = null;
      });
    }
  }

  @override
  void dispose() {
    _analysisJob?.cancel();
    super.dispose();
  }

  void _goToIndex(int index, {bool revealPosition = false}) {
    if (index < 0 || index > _timeline.lastIndex) {
      return;
    }
    if (index != _selectedIndex) {
      setState(() => _selectedIndex = index);
    }
    if (revealPosition) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        final BuildContext? positionContext =
            _positionSummaryKey.currentContext;
        if (!mounted || positionContext == null) {
          return;
        }
        Scrollable.ensureVisible(
          positionContext,
          duration: const Duration(milliseconds: 250),
          curve: Curves.easeOutCubic,
          alignment: 0.72,
        );
      });
    }
  }

  void _goToPly(int ply) {
    final int? index = _timeline.indexForPly(ply);
    if (index != null) {
      _goToIndex(index, revealPosition: true);
    }
  }

  void _toggleBoardOrientation() {
    setState(() => _rotated = !_rotated);
  }

  Future<void> _choosePosition() async {
    final int? selected = await showModalBottomSheet<int>(
      context: context,
      showDragHandle: true,
      builder: (BuildContext context) =>
          _PositionChooser(timeline: _timeline, selectedIndex: _selectedIndex),
    );
    if (!mounted || selected == null) {
      return;
    }
    _goToIndex(selected);
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final PostGameReviewFrame frame = _timeline.frameAt(_selectedIndex);
    final HomeBoardSnapshot boardSnapshot = _timeline.boardSnapshotAt(
      _selectedIndex,
    );
    final HomeChessPosition boardPosition = boardSnapshot.position!;

    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(
          title: Text(litchStrings(context).gameReviewTitle),
          actions: <Widget>[
            Semantics(
              key: const ValueKey<String>('review-rotate-board'),
              label: litchStrings(context).reviewBoardOrientation,
              value: _rotated
                  ? litchStrings(context).reviewBlackAtBottom
                  : litchStrings(context).reviewWhiteAtBottom,
              button: true,
              toggled: _rotated,
              onTap: _toggleBoardOrientation,
              child: ExcludeSemantics(
                child: IconButton(
                  tooltip: litchStrings(context).reviewRotateBoard,
                  onPressed: _toggleBoardOrientation,
                  icon: const Icon(Icons.screen_rotation_alt_outlined),
                ),
              ),
            ),
          ],
        ),
      ),
      body: SafeArea(
        child: GamePositionReviewLayout(
          scrollKey: const ValueKey<String>('post-game-review-scroll'),
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 28),
          boardTopGap: 12,
          boardBottomGap: 14,
          heading: _ReviewSummary(archive: widget.archive),
          board: Semantics(
            container: true,
            label:
                '${litchStrings(context).reviewBoard}. '
                '${litchStrings(context).reviewSideToMove(_sideName(context, boardPosition.sideToMove))}. '
                '${litchStrings(context).reviewPiecesShown(boardPosition.pieces.length)}. '
                '${_spokenFrameDescription(context, frame)}',
            child: HomeChessboard(
              snapshot: boardSnapshot,
              showCoordinates: true,
              maxSize: double.infinity,
              viewFromOppositeEnd: _rotated,
            ),
          ),
          details: <Widget>[
            Semantics(
              key: _positionSummaryKey,
              container: true,
              liveRegion: true,
              label:
                  '${_frameHeading(context, frame, _timeline.length)}. '
                  '${_frameMoveDescription(context, frame)}',
              excludeSemantics: true,
              child: Column(
                children: <Widget>[
                  Text(
                    _frameHeading(context, frame, _timeline.length),
                    key: const ValueKey<String>('review-position-heading'),
                    textAlign: TextAlign.center,
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    _frameMoveDescription(context, frame),
                    key: const ValueKey<String>('review-move-description'),
                    textAlign: TextAlign.center,
                    style: theme.textTheme.bodyLarge?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 10),
            _ReplayControls(
              selectedIndex: _selectedIndex,
              lastIndex: _timeline.lastIndex,
              choosePosition: _choosePosition,
              goToIndex: _goToIndex,
            ),
            const SizedBox(height: 22),
            _buildAnalysisCard(context),
          ],
        ),
      ),
    );
  }

  Widget _buildAnalysisCard(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    if (_analysisState == _AnalysisState.loading) {
      return Semantics(
        liveRegion: true,
        label: litchStrings(context).reviewAnalysisLoading,
        child: Card(
          child: Padding(
            padding: const EdgeInsets.all(20),
            child: Row(
              children: <Widget>[
                const SizedBox.square(
                  dimension: 22,
                  child: CircularProgressIndicator(strokeWidth: 2.5),
                ),
                const SizedBox(width: 16),
                Expanded(
                  child: Text(
                    litchStrings(context).reviewAnalysisLoadingEllipsis,
                    style: theme.textTheme.bodyLarge,
                  ),
                ),
              ],
            ),
          ),
        ),
      );
    }

    final PostGameReviewAnalysis? analysis = _analysis;
    if (_analysisState == _AnalysisState.ready && analysis != null) {
      final int count = analysis.insights.length;
      return Semantics(
        liveRegion: true,
        label: count == 0
            ? litchStrings(context).reviewReadyNoMoments
            : litchStrings(context).reviewReadyMoments(count),
        child: _KeyMomentsCard(
          analysis: analysis,
          selectedPly: _timeline.frameAt(_selectedIndex).position.ply,
          onSelectPly: _goToPly,
        ),
      );
    }

    final String message = switch ((
      widget.archive.variant,
      widget.archive.status,
      widget.archive.captureQuality,
      _analysisState,
    )) {
      (GameVariant.duck, _, _, _) => litchStrings(
        context,
      ).reviewDuckAnalysisUnavailable,
      (_, GameArchiveStatus.active, _, _) => litchStrings(
        context,
      ).reviewActiveAnalysisUnavailable,
      (_, _, GameCaptureQuality.partial, _) => litchStrings(
        context,
      ).reviewPartialAnalysisUnavailable,
      (_, _, _, _AnalysisState.failed) => litchStrings(
        context,
      ).reviewAnalysisFailed,
      _ => litchStrings(context).reviewReplayAvailable,
    };
    final Widget card = Card(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Icon(
              Icons.lightbulb_outline_rounded,
              color: theme.colorScheme.primary,
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text(
                    litchStrings(context).reviewNotes,
                    style: theme.textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  const SizedBox(height: 6),
                  Text(message),
                  if (_analysisState == _AnalysisState.failed) ...<Widget>[
                    const SizedBox(height: 12),
                    LitchCompactControls(
                      child: FilledButton.tonalIcon(
                        key: const ValueKey<String>('review-retry-analysis'),
                        onPressed: _loadAnalysis,
                        icon: const Icon(Icons.refresh_rounded),
                        label: Text(litchStrings(context).reviewRetryAnalysis),
                      ),
                    ),
                  ],
                ],
              ),
            ),
          ],
        ),
      ),
    );
    if (_analysisState != _AnalysisState.failed) {
      return card;
    }
    return Semantics(liveRegion: true, label: message, child: card);
  }
}

class _ReviewSummary extends StatelessWidget {
  const _ReviewSummary({required this.archive});

  final GameArchive archive;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Text(
          gameArchiveResultLabel(context, archive),
          style: theme.textTheme.headlineSmall?.copyWith(
            fontWeight: FontWeight.w700,
          ),
        ),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: <Widget>[
            Chip(
              label: Text(gameArchiveVariantLabel(context, archive.variant)),
            ),
            Chip(
              label: Text(
                archive.captureQuality == GameCaptureQuality.complete
                    ? litchStrings(context).gameLibraryCompleteReplay
                    : litchStrings(context).gameLibraryPartialReplay,
              ),
            ),
            Chip(
              label: Text(
                litchStrings(
                  context,
                ).gameLibraryMoveCount(archive.transitions.length),
              ),
            ),
          ],
        ),
      ],
    );
  }
}

class _ReplayControls extends StatelessWidget {
  const _ReplayControls({
    required this.selectedIndex,
    required this.lastIndex,
    required this.choosePosition,
    required this.goToIndex,
  });

  final int selectedIndex;
  final int lastIndex;
  final VoidCallback choosePosition;
  final ValueChanged<int> goToIndex;

  @override
  Widget build(BuildContext context) {
    return LitchCompactControls(
      child: Column(
        children: <Widget>[
          Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              IconButton(
                key: const ValueKey<String>('review-first-position'),
                tooltip: litchStrings(context).reviewFirstPosition,
                onPressed: selectedIndex == 0 ? null : () => goToIndex(0),
                icon: const Icon(Icons.first_page_rounded),
              ),
              IconButton(
                key: const ValueKey<String>('review-previous-position'),
                tooltip: litchStrings(context).reviewPreviousPosition,
                onPressed: selectedIndex == 0
                    ? null
                    : () => goToIndex(selectedIndex - 1),
                icon: const Icon(Icons.chevron_left_rounded),
              ),
              const SizedBox(width: 8),
              IconButton(
                key: const ValueKey<String>('review-next-position'),
                tooltip: litchStrings(context).reviewNextPosition,
                onPressed: selectedIndex == lastIndex
                    ? null
                    : () => goToIndex(selectedIndex + 1),
                icon: const Icon(Icons.chevron_right_rounded),
              ),
              IconButton(
                key: const ValueKey<String>('review-last-position'),
                tooltip: litchStrings(context).reviewLastPosition,
                onPressed: selectedIndex == lastIndex
                    ? null
                    : () => goToIndex(lastIndex),
                icon: const Icon(Icons.last_page_rounded),
              ),
            ],
          ),
          const SizedBox(height: 4),
          ConstrainedBox(
            constraints: const BoxConstraints(minHeight: 48),
            child: OutlinedButton.icon(
              key: const ValueKey<String>('review-choose-position'),
              onPressed: choosePosition,
              icon: const Icon(Icons.list_rounded),
              label: Text(litchStrings(context).reviewChoosePosition),
            ),
          ),
        ],
      ),
    );
  }
}
