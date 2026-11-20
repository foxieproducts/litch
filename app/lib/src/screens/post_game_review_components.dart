part of 'post_game_review_screen.dart';

class _PositionChooser extends StatelessWidget {
  const _PositionChooser({required this.timeline, required this.selectedIndex});

  final PostGameReviewTimeline timeline;
  final int selectedIndex;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: CustomScrollView(
        key: const ValueKey<String>('review-position-list'),
        slivers: <Widget>[
          SliverToBoxAdapter(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(20, 4, 20, 12),
              child: Semantics(
                header: true,
                child: Text(
                  litchStrings(context).reviewChoosePositionTitle,
                  style: Theme.of(
                    context,
                  ).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700),
                ),
              ),
            ),
          ),
          const SliverToBoxAdapter(child: Divider(height: 1)),
          SliverList.builder(
            itemCount: timeline.length,
            itemBuilder: (BuildContext context, int index) {
              final PostGameReviewFrame frame = timeline.frameAt(index);
              return ListTile(
                key: ValueKey<String>('review-position-$index'),
                minTileHeight: 56,
                selected: index == selectedIndex,
                leading: Text('${index + 1}'),
                title: Text(_frameMoveDescription(context, frame)),
                subtitle: Text(_positionReference(context, frame.position.ply)),
                trailing: index == selectedIndex
                    ? const Icon(Icons.check_rounded)
                    : null,
                onTap: () => Navigator.of(context).pop(index),
              );
            },
          ),
        ],
      ),
    );
  }
}

class _KeyMomentsCard extends StatelessWidget {
  const _KeyMomentsCard({
    required this.analysis,
    required this.selectedPly,
    required this.onSelectPly,
  });

  final PostGameReviewAnalysis analysis;
  final int selectedPly;
  final ValueChanged<int> onSelectPly;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(8, 18, 8, 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Text(
                litchStrings(context).reviewKeyMoments,
                style: theme.textTheme.titleLarge?.copyWith(
                  fontWeight: FontWeight.w700,
                ),
              ),
            ),
            const SizedBox(height: 4),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Text(
                analysis.insights.isEmpty
                    ? litchStrings(context).reviewNoTurningPoint
                    : litchStrings(context).reviewMomentsIntro,
                style: theme.textTheme.bodyMedium?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ),
            if (analysis.insights.isNotEmpty) const SizedBox(height: 8),
            for (int index = 0; index < analysis.insights.length; index += 1)
              _MomentTile(
                key: ValueKey<String>('review-moment-$index'),
                insight: analysis.insights[index],
                selected: analysis.insights[index].ply == selectedPly,
                onTap: () => onSelectPly(analysis.insights[index].ply),
              ),
          ],
        ),
      ),
    );
  }
}

class _MomentTile extends StatelessWidget {
  const _MomentTile({
    super.key,
    required this.insight,
    required this.selected,
    required this.onTap,
  });

  final PostGameReviewInsight insight;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final String continuation = switch (insight.kind) {
      PostGameReviewInsightKind.candidateLine =>
        litchStrings(context).reviewCandidateLine(
          insight.continuation.map(_formatCoordinateMove).join(' · '),
        ),
      PostGameReviewInsightKind.mateInOne => litchStrings(
        context,
      ).reviewMateInOneMove(_formatCoordinateMove(insight.continuation.single)),
      PostGameReviewInsightKind.materialChange => _materialDetail(
        context,
        insight,
      ),
    };
    return ListTile(
      selected: selected,
      minTileHeight: 72,
      leading: Icon(_momentIcon(insight.kind)),
      title: Text(
        '${_momentLabel(context, insight)} · '
        '${_positionReference(context, insight.ply)}',
      ),
      subtitle: Text(continuation),
      trailing: selected
          ? Icon(
              Icons.check_circle_rounded,
              semanticLabel: litchStrings(context).reviewCurrentPosition,
            )
          : const Icon(Icons.chevron_right_rounded),
      onTap: onTap,
    );
  }
}

String _frameHeading(
  BuildContext context,
  PostGameReviewFrame frame,
  int total,
) {
  if (frame.incomingTransition == null && frame.position.ply == 0) {
    return litchStrings(context).reviewStartingPositionCount(total);
  }
  return litchStrings(context).reviewPositionCount(frame.index + 1, total);
}

String _frameMoveDescription(BuildContext context, PostGameReviewFrame frame) {
  final ArchivedTransition? move = frame.incomingTransition;
  if (move == null) {
    return frame.position.ply == 0
        ? litchStrings(context).reviewBeforeFirstMove
        : litchStrings(context).reviewArchiveBegins(
            _positionReference(context, frame.position.ply).toLowerCase(),
          );
  }
  final int moveNumber = (frame.position.ply + 1) ~/ 2;
  return litchStrings(context).reviewMoveDescription(
    _gameSideName(context, move.side),
    moveNumber,
    _formatArchivedTransition(context, move),
  );
}

String _spokenFrameDescription(
  BuildContext context,
  PostGameReviewFrame frame,
) {
  final ArchivedTransition? move = frame.incomingTransition;
  if (move == null) {
    final int? duckSquare = frame.position.duckSquare;
    return duckSquare == null
        ? _frameMoveDescription(context, frame)
        : '${_frameMoveDescription(context, frame)}. '
              '${litchStrings(context).reviewDuckOn(algebraicSquare(duckSquare))}.';
  }
  final StringBuffer description = StringBuffer(
    '${litchStrings(context).reviewLastMove(_gameSideName(context, move.side), algebraicSquare(move.from), algebraicSquare(move.to))}.',
  );
  if (move.duckFrom != null && move.duckTo != null) {
    description.write(
      ' ${litchStrings(context).reviewDuckMoved(algebraicSquare(move.duckFrom!), algebraicSquare(move.duckTo!))}.',
    );
  } else if (frame.position.duckSquare != null) {
    description.write(
      ' ${litchStrings(context).reviewDuckOn(algebraicSquare(frame.position.duckSquare!))}.',
    );
  }
  return description.toString();
}

String _formatArchivedTransition(
  BuildContext context,
  ArchivedTransition move,
) {
  final StringBuffer value = StringBuffer(
    '${algebraicSquare(move.from)}–${algebraicSquare(move.to)}',
  );
  if (move.captureSquare != null) {
    value.write(
      ' · ${litchStrings(context).reviewCapture(algebraicSquare(move.captureSquare!))}',
    );
  }
  if (move.promotion != null) {
    value.write(' · ${_promotionLabel(context, move.promotion!)}');
  }
  if (move.secondaryFrom != null && move.secondaryTo != null) {
    value.write(
      ' · ${algebraicSquare(move.secondaryFrom!)}–'
      '${algebraicSquare(move.secondaryTo!)}',
    );
  }
  if (move.duckFrom != null && move.duckTo != null) {
    value.write(
      ' · Duck ${algebraicSquare(move.duckFrom!)}–'
      '${algebraicSquare(move.duckTo!)}',
    );
  }
  return value.toString();
}

String _positionReference(BuildContext context, int ply) {
  if (ply == 0) {
    return litchStrings(context).reviewStartingPosition;
  }
  final String side = ply.isOdd
      ? litchStrings(context).reviewWhite
      : litchStrings(context).reviewBlack;
  final int moveNumber = (ply + 1) ~/ 2;
  return litchStrings(context).reviewAfterMove(side, moveNumber);
}

String _formatCoordinateMove(String move) {
  final StringBuffer value = StringBuffer(
    '${move.substring(0, 2)}–${move.substring(2, 4)}',
  );
  if (move.length == 5) {
    value.write('=${move[4].toUpperCase()}');
  }
  return value.toString();
}

String _sideName(BuildContext context, HomeSide side) => side == HomeSide.white
    ? litchStrings(context).reviewWhite
    : litchStrings(context).reviewBlack;

String _gameSideName(BuildContext context, GameSide side) =>
    side == GameSide.white
    ? litchStrings(context).reviewWhite
    : litchStrings(context).reviewBlack;

String _promotionLabel(BuildContext context, PromotionPiece promotion) =>
    switch (promotion) {
      PromotionPiece.queen => litchStrings(context).reviewPromotesQueen,
      PromotionPiece.rook => litchStrings(context).reviewPromotesRook,
      PromotionPiece.bishop => litchStrings(context).reviewPromotesBishop,
      PromotionPiece.knight => litchStrings(context).reviewPromotesKnight,
    };

String _momentLabel(BuildContext context, PostGameReviewInsight insight) =>
    switch (insight.kind) {
      PostGameReviewInsightKind.materialChange => litchStrings(
        context,
      ).reviewMaterialChange,
      PostGameReviewInsightKind.mateInOne => litchStrings(
        context,
      ).reviewMateInOne(_gameSideName(context, insight.side!)),
      PostGameReviewInsightKind.candidateLine => litchStrings(
        context,
      ).reviewCandidateLineLabel,
    };

IconData _momentIcon(PostGameReviewInsightKind kind) => switch (kind) {
  PostGameReviewInsightKind.materialChange => Icons.swap_horiz_rounded,
  PostGameReviewInsightKind.mateInOne => Icons.flag_outlined,
  PostGameReviewInsightKind.candidateLine => Icons.route_outlined,
};

String _materialDetail(BuildContext context, PostGameReviewInsight insight) {
  final details = <String>[];
  if (insight.capturedPiece != null) {
    details.add(
      litchStrings(context).reviewMaterialCapture(
        _gameSideName(context, insight.side!),
        _reviewPieceLabel(context, insight.capturedPiece!),
        algebraicSquare(insight.captureSquare!),
      ),
    );
  }
  if (insight.promotion != null) {
    details.add(
      litchStrings(context).reviewMaterialPromotion(
        _gameSideName(context, insight.side!),
        _promotionPieceLabel(context, insight.promotion!),
      ),
    );
  }
  return details.join('\n');
}

String _reviewPieceLabel(BuildContext context, PostGameReviewPiece piece) =>
    switch (piece) {
      PostGameReviewPiece.pawn => litchStrings(context).reviewPiecePawn,
      PostGameReviewPiece.knight => litchStrings(context).reviewPieceKnight,
      PostGameReviewPiece.bishop => litchStrings(context).reviewPieceBishop,
      PostGameReviewPiece.rook => litchStrings(context).reviewPieceRook,
      PostGameReviewPiece.queen => litchStrings(context).reviewPieceQueen,
    };

String _promotionPieceLabel(BuildContext context, PromotionPiece piece) =>
    switch (piece) {
      PromotionPiece.knight => litchStrings(context).reviewPieceKnight,
      PromotionPiece.bishop => litchStrings(context).reviewPieceBishop,
      PromotionPiece.rook => litchStrings(context).reviewPieceRook,
      PromotionPiece.queen => litchStrings(context).reviewPieceQueen,
    };
