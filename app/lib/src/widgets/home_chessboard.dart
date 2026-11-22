import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/home_assistance_state.dart';
import '../models/home_board_snapshot.dart';
import '../models/litch_piece_color.dart';
import '../theme/litch_theme.dart';

const _squareChamferClipper = ShapeBorderClipper(
  shape: BeveledRectangleBorder(
    borderRadius: BorderRadius.all(Radius.circular(LitchShapes.squareChamfer)),
  ),
);

class HomeChessboard extends StatefulWidget {
  const HomeChessboard({
    super.key,
    required this.snapshot,
    this.maxSize = 430,
    this.showCoordinates = false,
    this.viewFromOppositeEnd,
    this.selectedSquare,
    this.legalDestinations = const <int>{},
    this.onSquarePressed,
    this.liveGuidanceAvailable = true,
    this.promotionSquare,
    this.promotionMotion,
  });

  final HomeBoardSnapshot snapshot;
  final double maxSize;
  final bool showCoordinates;
  final bool? viewFromOppositeEnd;
  final int? selectedSquare;
  final Set<int> legalDestinations;
  final ValueChanged<int>? onSquarePressed;
  final bool liveGuidanceAvailable;
  final int? promotionSquare;
  final Animation<double>? promotionMotion;

  static const Map<String, String> _pieceAssets = <String, String>{
    'p': 'assets/chess_pieces/cameos_v3/pawn.png',
    'r': 'assets/chess_pieces/cameos_v3/rook.png',
    'n': 'assets/chess_pieces/cameos_v3/knight.png',
    'b': 'assets/chess_pieces/cameos_v3/bishop.png',
    'q': 'assets/chess_pieces/cameos_v3/queen.png',
    'k': 'assets/chess_pieces/cameos_v3/king.png',
  };
  static const String duckAsset = 'assets/chess_pieces/cameos_v2/duck.png';

  @override
  State<HomeChessboard> createState() => _HomeChessboardState();
}

class _HomeChessboardState extends State<HomeChessboard>
    with SingleTickerProviderStateMixin {
  // One slow cycle is 2.4 seconds. Only amber border layers listen to it.
  late final _amberPulse = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1200),
  );

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _syncAmberPulse();
  }

  @override
  void didUpdateWidget(HomeChessboard oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncAmberPulse();
  }

  void _syncAmberPulse() {
    final snapshot = widget.snapshot;
    final hasAmberGuidance =
        widget.liveGuidanceAvailable &&
        !snapshot.isExpectedBoardInteraction &&
        !snapshot.isDuckRelocationCorrection &&
        !snapshot.isDuckLandingCorrection &&
        snapshot.guidedSquares.any(
          (square) =>
              square != widget.selectedSquare &&
              !widget.legalDestinations.contains(square) &&
              square != snapshot.captureRemovalSquare,
        );
    if (!hasAmberGuidance || MediaQuery.disableAnimationsOf(context)) {
      _amberPulse.stop();
      _amberPulse.value = 0;
    } else if (!TickerMode.valuesOf(context).enabled) {
      _amberPulse.stop();
    } else if (!_amberPulse.isAnimating) {
      _amberPulse.repeat(reverse: true);
    }
  }

  @override
  void dispose() {
    _amberPulse.dispose();
    super.dispose();
  }

  int _squareAtDisplayIndex(int index) => switch (widget.viewFromOppositeEnd) {
    true => 63 - index,
    false => index,
    null => widget.snapshot.viewSquare(index),
  };

  @override
  Widget build(BuildContext context) {
    final snapshot = widget.snapshot;
    final liveGuidanceAvailable = widget.liveGuidanceAvailable;
    final HomeChessPosition? position = snapshot.position;
    final Set<int> mismatches = liveGuidanceAvailable
        ? snapshot.guidedSquares
        : const <int>{};
    final int? duckSquare = snapshot.gameStatus?.duckSquare;
    final Color whiteAccent = litchPieceAccentColor(snapshot, whitePiece: true);
    final Color blackAccent = litchPieceAccentColor(
      snapshot,
      whitePiece: false,
    );
    final grid = _buildGrid(
      position: position,
      mismatches: mismatches,
      duckSquare: duckSquare,
      whiteAccent: whiteAccent,
      blackAccent: blackAccent,
    );
    final game = snapshot.gameStatus;
    final strings = litchStrings(context);
    final result = game != null && game.isFinished
        ? localizedHomeResult(
            strings,
            game,
            playerForSide: (side) => strings.playerSpoken(
              snapshot.playerForSide(side) == 'P2' ? '2' : '1',
            ),
          )
        : null;
    return Semantics(
      container: true,
      label: position == null || !liveGuidanceAvailable
          ? litchStrings(context).boardPositionUnavailable
          : result != null
          ? '${result.heading}. ${result.title}.'
          : snapshot.isFreshSetupInProgress
          ? mismatches.isEmpty
                ? litchStrings(context).boardSetupChecking
                : litchStrings(context).boardSetupSquares(mismatches.length)
          : litchStrings(context).boardPositionSemantics(
              litchStrings(context).playerSpoken(
                snapshot.playerForSide(position.sideToMove) == 'P2' ? '2' : '1',
              ),
              position.pieces.length,
            ),
      child: Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(
            maxWidth: widget.maxSize,
            maxHeight: widget.maxSize,
          ),
          child: AspectRatio(
            aspectRatio: 1,
            child: Material(
              key: const ValueKey<String>('home-chessboard-surface'),
              color: widget.showCoordinates
                  ? Theme.of(context).colorScheme.surfaceContainerHighest
                  : const Color(0xFF526980),
              shape: BeveledRectangleBorder(
                borderRadius: BorderRadius.circular(LitchShapes.boardChamfer),
                side: BorderSide(
                  color: Theme.of(context).colorScheme.outlineVariant,
                ),
              ),
              clipBehavior: Clip.antiAlias,
              child: widget.showCoordinates
                  ? _ChessCoordinateFrame(
                      squareAtDisplayIndex: _squareAtDisplayIndex,
                      child: grid,
                    )
                  : grid,
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildGrid({
    required HomeChessPosition? position,
    required Set<int> mismatches,
    required int? duckSquare,
    required Color whiteAccent,
    required Color blackAccent,
  }) {
    final snapshot = widget.snapshot;
    final liveGuidanceAvailable = widget.liveGuidanceAvailable;
    final duckLandingCorrection =
        liveGuidanceAvailable && snapshot.isDuckLandingCorrection;
    final occupancy = liveGuidanceAvailable
        ? snapshot.physicalStatus?.occupiedSquares
        : null;
    final expected = liveGuidanceAvailable
        ? snapshot.expectedOccupiedSquares
        : null;
    final HomeMoveSquares? whiteLastMove = snapshot.lastMoves?.white;
    final HomeMoveSquares? blackLastMove = snapshot.lastMoves?.black;
    final HomeSide? mostRecentMoveSide = snapshot.lastMoves?.mostRecent;
    final HomeAssistanceState? assistance = liveGuidanceAvailable
        ? snapshot.visibleAssistance
        : null;
    final int? captureRemovalSquare = liveGuidanceAvailable
        ? snapshot.captureRemovalSquare
        : null;

    return GridView.builder(
      physics: const NeverScrollableScrollPhysics(),
      padding: EdgeInsets.zero,
      gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 8,
      ),
      itemCount: 64,
      itemBuilder: (BuildContext context, int displayIndex) {
        // The physical/view square stays fixed. Engine coordinates project
        // into that frame according to the selected logical P1 end. Hardware
        // pose is already compensated by BOARD_ROTATE at the board boundary.
        final bool oppositePlayerEnd =
            snapshot.physicalStatus?.p1AtOppositeEnd == true;
        final int square = _squareAtDisplayIndex(displayIndex);
        final int viewSquare =
            widget.showCoordinates || widget.viewFromOppositeEnd != null
            ? square
            : displayIndex;
        final bit = BigInt.one << square;
        final bool? physicalOccupied = occupancy == null
            ? null
            : (occupancy & bit) != BigInt.zero;
        final landingRemoval =
            duckLandingCorrection && mismatches.contains(square);
        // A marked landing has unknown piece identity, including on an origin.
        final showCanonicalPiece =
            !landingRemoval &&
            (!duckLandingCorrection || physicalOccupied != false);
        final String? piece = !showCanonicalPiece
            ? null
            : liveGuidanceAvailable
            ? snapshot.physicalPieceAt(square)
            : position?.pieces[square];
        return _BoardSquare(
          key: ValueKey<String>('square-${algebraicSquare(viewSquare)}'),
          square: square,
          viewSquare: viewSquare,
          oppositePlayerEnd: oppositePlayerEnd,
          piece: piece,
          pieceAsset: piece == null
              ? null
              : HomeChessboard._pieceAssets[piece.toLowerCase()]!,
          expectedOccupied:
              showCanonicalPiece &&
              (expected == null
                  ? piece != null || duckSquare == square
                  : (expected & bit) != BigInt.zero),
          isDuckSquare: showCanonicalPiece && duckSquare == square,
          duckAsset: HomeChessboard.duckAsset,
          amberPulse: _amberPulse,
          pieceAccent: piece == null
              ? null
              : (_isWhitePiece(piece) ? whiteAccent : blackAccent),
          isMismatch: mismatches.contains(square),
          isDuckLandingRemoval: landingRemoval,
          isLegalProgress:
              liveGuidanceAvailable &&
              (snapshot.isExpectedBoardInteraction ||
                  snapshot.isDuckRelocationCorrection),
          isFreshSetup:
              liveGuidanceAvailable && snapshot.isFreshSetupInProgress,
          physicalOccupied: physicalOccupied,
          isCpuMoveFrom:
              liveGuidanceAvailable &&
              ((snapshot.homeStateAction == HomeStateAction.cpu &&
                      snapshot.physicalMovePrompt?.fromSquare == square) ||
                  (snapshot.visibleDuckRelocation?.owner ==
                          HomeDuckRelocationOwner.cpu &&
                      snapshot.visibleDuckRelocation?.fromSquare == square)),
          isCpuMoveTo:
              liveGuidanceAvailable &&
              ((snapshot.homeStateAction == HomeStateAction.cpu &&
                      snapshot.physicalMovePrompt?.toSquare == square) ||
                  (snapshot.visibleDuckRelocation?.owner ==
                          HomeDuckRelocationOwner.cpu &&
                      snapshot.visibleDuckRelocation?.toSquare == square)),
          isCaptureRemoval: captureRemovalSquare == square,
          assistance: assistance,
          isWhiteLastFrom: whiteLastMove?.fromSquare == square,
          isWhiteLastTo: whiteLastMove?.toSquare == square,
          isBlackLastFrom: blackLastMove?.fromSquare == square,
          isBlackLastTo: blackLastMove?.toSquare == square,
          whiteLastIsMostRecent: mostRecentMoveSide == HomeSide.white,
          blackLastIsMostRecent: mostRecentMoveSide == HomeSide.black,
          whiteLastColor: whiteAccent,
          blackLastColor: blackAccent,
          isSelected: widget.selectedSquare == square,
          isInteractiveDestination: widget.legalDestinations.contains(square),
          promotionConfirmed:
              liveGuidanceAvailable && square == widget.promotionSquare,
          promotionMotion: widget.promotionMotion,
          onPressed: widget.onSquarePressed == null
              ? null
              : () => widget.onSquarePressed!(square),
        );
      },
    );
  }
}

class _ChessCoordinateFrame extends StatelessWidget {
  const _ChessCoordinateFrame({
    required this.squareAtDisplayIndex,
    required this.child,
  });

  final int Function(int) squareAtDisplayIndex;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final side = constraints.maxWidth;
        final fontSize = (side / 32).clamp(12.0, 18.0);
        final scaledFontSize = MediaQuery.textScalerOf(context).scale(fontSize);
        final gutter = math.min(side / 10, scaledFontSize * 1.25 + 4);
        final style = TextStyle(
          color: Theme.of(context).colorScheme.onSurface,
          fontSize: fontSize,
          fontWeight: FontWeight.w600,
          height: 1,
        );
        Widget labels(Axis axis, String edge) => ExcludeSemantics(
          child: Flex(
            key: ValueKey(
              'play-${axis == Axis.horizontal ? 'files' : 'ranks'}-$edge',
            ),
            direction: axis,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              for (var i = 0; i < 8; ++i)
                Expanded(
                  child: Center(
                    child: FittedBox(
                      fit: BoxFit.scaleDown,
                      child: Text(
                        axis == Axis.horizontal
                            ? algebraicSquare(squareAtDisplayIndex(i))[0]
                            : algebraicSquare(squareAtDisplayIndex(i * 8))[1],
                        style: style,
                      ),
                    ),
                  ),
                ),
            ],
          ),
        );
        Widget files(String edge) => SizedBox(
          height: gutter,
          child: Padding(
            padding: EdgeInsets.symmetric(horizontal: gutter),
            child: labels(Axis.horizontal, edge),
          ),
        );

        // Chess files and the grid have a shared left-to-right display index,
        // independent of the application's reading direction.
        return Directionality(
          textDirection: TextDirection.ltr,
          child: Column(
            children: [
              files('top'),
              Expanded(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    SizedBox(
                      width: gutter,
                      child: labels(Axis.vertical, 'left'),
                    ),
                    Expanded(
                      child: ClipPath(
                        // The rails show through all four exterior cuts, including
                        // the dark backing behind the individually beveled cells.
                        clipper: _squareChamferClipper,
                        child: child,
                      ),
                    ),
                    SizedBox(
                      width: gutter,
                      child: labels(Axis.vertical, 'right'),
                    ),
                  ],
                ),
              ),
              files('bottom'),
            ],
          ),
        );
      },
    );
  }
}

class _BoardSquare extends StatelessWidget {
  const _BoardSquare({
    super.key,
    required this.square,
    required this.viewSquare,
    required this.oppositePlayerEnd,
    required this.piece,
    required this.pieceAsset,
    required this.expectedOccupied,
    required this.isDuckSquare,
    required this.duckAsset,
    required this.amberPulse,
    required this.pieceAccent,
    required this.isMismatch,
    required this.isDuckLandingRemoval,
    required this.isLegalProgress,
    required this.isFreshSetup,
    required this.physicalOccupied,
    required this.isCpuMoveFrom,
    required this.isCpuMoveTo,
    required this.isCaptureRemoval,
    required this.assistance,
    required this.isWhiteLastFrom,
    required this.isWhiteLastTo,
    required this.isBlackLastFrom,
    required this.isBlackLastTo,
    required this.whiteLastIsMostRecent,
    required this.blackLastIsMostRecent,
    required this.whiteLastColor,
    required this.blackLastColor,
    required this.isSelected,
    required this.isInteractiveDestination,
    required this.promotionConfirmed,
    required this.promotionMotion,
    required this.onPressed,
  });

  final int square;
  final int viewSquare;
  final bool oppositePlayerEnd;
  final String? piece;
  final String? pieceAsset;
  final bool expectedOccupied;
  final bool isDuckSquare;
  final String duckAsset;
  final Animation<double> amberPulse;
  final Color? pieceAccent;
  final bool isMismatch;
  final bool isDuckLandingRemoval;
  final bool isLegalProgress;
  final bool isFreshSetup;
  final bool? physicalOccupied;
  final bool isCpuMoveFrom;
  final bool isCpuMoveTo;
  final bool isCaptureRemoval;
  final HomeAssistanceState? assistance;
  final bool isWhiteLastFrom;
  final bool isWhiteLastTo;
  final bool isBlackLastFrom;
  final bool isBlackLastTo;
  final bool whiteLastIsMostRecent;
  final bool blackLastIsMostRecent;
  final Color whiteLastColor;
  final Color blackLastColor;
  final bool isSelected;
  final bool isInteractiveDestination;
  final bool promotionConfirmed;
  final Animation<double>? promotionMotion;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final bool isLight = ((viewSquare ~/ 8) + (viewSquare % 8)).isEven;
    final Color background = isLight
        ? const Color(0xFFE8EEF3)
        : const Color(0xFF526980);
    final bool physicallyMissing =
        isMismatch && expectedOccupied && physicalOccupied == false;
    final bool unexpectedlyOccupied =
        isMismatch && !expectedOccupied && physicalOccupied == true;
    final String squareName = algebraicSquare(viewSquare);
    final bool hintFrom = assistance?.move?.from == square;
    final bool hintTo = assistance?.move?.to == square;
    final bool duckHintFrom = assistance?.duckMove?.from == square;
    final bool duckHintTo = assistance?.duckMove?.to == square;
    final bool legalSource = assistance?.legalSource == square;
    final BigInt? legalMask = assistance?.legalDestinations;
    final bool legalDestination =
        legalMask != null &&
        (legalMask & (BigInt.one << square)) != BigInt.zero;
    final bool chessGuideFrom = hintFrom || legalSource;
    final bool chessGuideTo = hintTo || legalDestination;
    final bool hasChessAssistance = chessGuideFrom || chessGuideTo;
    final bool hasAssistance = hasChessAssistance || duckHintFrom || duckHintTo;
    const duckGuideColor = Color(0xFF8B6500);

    String semanticsLabel = litchStrings(context).emptySquare(squareName);
    if (isDuckLandingRemoval) {
      semanticsLabel = litchStrings(context).removeCapturePiece(squareName);
    } else if (physicallyMissing && piece != null) {
      semanticsLabel = isLegalProgress
          ? litchStrings(context).pieceLifted(
              _capitalize(localizedPieceName(litchStrings(context), piece!)),
              squareName,
            )
          : litchStrings(context).pieceExpected(
              _capitalize(localizedPieceName(litchStrings(context), piece!)),
              squareName,
            );
    } else if (physicallyMissing && isDuckSquare) {
      semanticsLabel = isLegalProgress
          ? litchStrings(context).duckLifted(squareName)
          : litchStrings(context).duckExpected(squareName);
    } else if (physicallyMissing) {
      semanticsLabel = litchStrings(context).promotionPlacePiece(squareName);
    } else if (expectedOccupied && piece == null && !isDuckSquare) {
      semanticsLabel = litchStrings(context).occupiedSquare(squareName);
    } else if (unexpectedlyOccupied) {
      semanticsLabel = isFreshSetup
          ? litchStrings(context).removeLeftoverSquare(squareName)
          : isLegalProgress
          ? litchStrings(context).squareMoveProgress(squareName)
          : litchStrings(context).unexpectedSquare(squareName);
    } else if (piece != null) {
      semanticsLabel =
          '$squareName, ${localizedPieceName(litchStrings(context), piece!)}';
    } else if (isDuckSquare) {
      semanticsLabel = '$semanticsLabel, ${litchStrings(context).duckPiece}';
    }
    if (isCpuMoveFrom) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).cpuStartsHere}';
    }
    if (isCpuMoveTo) {
      semanticsLabel = '$semanticsLabel. ${litchStrings(context).cpuEndsHere}';
    }
    if (isCaptureRemoval) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).removeThisPiece}';
    }
    if (hintFrom) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).hintStartsHere}';
    }
    if (hintTo) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).hintDestination}';
    }
    if (duckHintFrom) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).duckHintStartsHere}';
    }
    if (duckHintTo) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).duckHintDestination}';
    }
    if (legalSource) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).legalMovesShown}';
    }
    if (legalDestination) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).legalDestination}';
    }
    if (!isSelected && isInteractiveDestination) {
      semanticsLabel =
          '$semanticsLabel. ${litchStrings(context).legalDestination}';
    }
    if (isWhiteLastFrom) {
      semanticsLabel = '$semanticsLabel. ${litchStrings(context).p1LastStart}';
    }
    if (isWhiteLastTo) {
      semanticsLabel = '$semanticsLabel. ${litchStrings(context).p1LastEnd}';
    }
    if (isBlackLastFrom) {
      semanticsLabel = '$semanticsLabel. ${litchStrings(context).p2LastStart}';
    }
    if (isBlackLastTo) {
      semanticsLabel = '$semanticsLabel. ${litchStrings(context).p2LastEnd}';
    }
    final bool hasMovePrompt = isCpuMoveFrom || isCpuMoveTo || isCaptureRemoval;
    final bool hasAmberGuidance =
        isMismatch &&
        !isLegalProgress &&
        !isSelected &&
        !isInteractiveDestination &&
        !hasMovePrompt;

    final contents = DecoratedBox(
      decoration: BoxDecoration(
        color: background,
        border: isSelected
            ? Border.all(color: Theme.of(context).colorScheme.primary, width: 3)
            : isInteractiveDestination
            ? Border.all(color: const Color(0xFF3A7D52), width: 3)
            : hasMovePrompt
            ? Border.all(color: const Color(0xFF2F6173), width: 3)
            : isMismatch && isLegalProgress
            ? Border.all(color: const Color(0xFF3A7D52), width: 2)
            : hasAmberGuidance
            ? null
            : hasAssistance
            ? Border.all(
                color: hasChessAssistance
                    ? const Color(0xFF3A7D52)
                    : duckGuideColor,
                width: 2,
              )
            : null,
      ),
      child: Stack(
        alignment: Alignment.center,
        children: <Widget>[
          if (promotionConfirmed)
            Positioned.fill(
              child: _PromotionHighlight(
                key: ValueKey<String>('promotion-confirmed-$squareName'),
                color: pieceAccent ?? Theme.of(context).colorScheme.primary,
                motion: promotionMotion,
              ),
            ),
          if (piece != null)
            Padding(
              padding: const EdgeInsets.all(1),
              child: Opacity(
                key: physicallyMissing
                    ? ValueKey<String>('expected-missing-$squareName')
                    : null,
                opacity: physicallyMissing ? 0.42 : 1,
                child: _withPromotionMotion(
                  _BoardArtwork(
                    key: ValueKey<String>('piece-$squareName'),
                    assetPath: pieceAsset!,
                    accentColor: pieceAccent,
                    invertNeutral: !_isWhitePiece(piece!) != oppositePlayerEnd,
                  ),
                ),
              ),
            ),
          if (isDuckSquare)
            Padding(
              padding: const EdgeInsets.all(1),
              child: Opacity(
                key: physicallyMissing
                    ? ValueKey<String>('expected-missing-$squareName')
                    : null,
                opacity: physicallyMissing ? 0.42 : 1,
                child: _BoardArtwork(
                  key: ValueKey<String>('duck-marker-$squareName'),
                  assetPath: duckAsset,
                ),
              ),
            ),
          if (expectedOccupied && piece == null && !isDuckSquare)
            Opacity(
              opacity: physicallyMissing ? 0.42 : 1,
              child: Container(
                key: ValueKey<String>('physical-piece-$squareName'),
                width: 24,
                height: 24,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  border: Border.all(
                    color: isLight
                        ? const Color(0xFF526980)
                        : const Color(0xFFE8EEF3),
                    width: 2,
                  ),
                ),
              ),
            ),
          if (isMismatch && !expectedOccupied)
            Container(
              key: ValueKey<String>('unexpected-occupied-$squareName'),
              width: 12,
              height: 12,
              decoration: BoxDecoration(
                color: isLegalProgress
                    ? const Color(0xFF3A7D52)
                    : const Color(0xFFFFB300),
                shape: BoxShape.circle,
              ),
            ),
          if (isInteractiveDestination)
            Container(
              key: ValueKey<String>('legal-target-$squareName'),
              width: piece == null ? 12 : 30,
              height: piece == null ? 12 : 30,
              decoration: BoxDecoration(
                color: piece == null
                    ? const Color(0xFF3A7D52)
                    : Colors.transparent,
                shape: BoxShape.circle,
                border: piece == null
                    ? null
                    : Border.all(color: const Color(0xFF3A7D52), width: 3),
              ),
            ),
          if (isCpuMoveFrom)
            Positioned(
              left: 2,
              top: 2,
              child: _MoveGuideMarker(
                key: ValueKey<String>('cpu-move-from-$squareName'),
                icon: Icons.arrow_outward_rounded,
              ),
            ),
          if (isCpuMoveTo)
            Positioned(
              right: 2,
              bottom: 2,
              child: _MoveGuideMarker(
                key: ValueKey<String>('cpu-move-to-$squareName'),
                icon: Icons.my_location_rounded,
              ),
            ),
          if (isCaptureRemoval)
            Positioned(
              right: 2,
              top: 2,
              child: _MoveGuideMarker(
                key: ValueKey<String>('capture-removal-$squareName'),
                icon: Icons.remove_circle_outline,
                color: const Color(0xFF8B6500),
              ),
            ),
          if (!hasMovePrompt && (chessGuideFrom || duckHintFrom))
            Positioned(
              left: 2,
              top: 2,
              child: _MoveGuideMarker(
                key: ValueKey<String>('assistance-from-$squareName'),
                icon: Icons.arrow_outward_rounded,
                color: chessGuideFrom
                    ? const Color(0xFF3A7D52)
                    : duckGuideColor,
              ),
            ),
          if (!hasMovePrompt && chessGuideTo)
            Positioned(
              right: 2,
              bottom: 2,
              child: _MoveGuideMarker(
                key: ValueKey<String>('assistance-to-$squareName'),
                icon: Icons.my_location_rounded,
                color: const Color(0xFF3A7D52),
              ),
            ),
          if (!hasMovePrompt && duckHintTo)
            Positioned(
              right: 2,
              top: 2,
              child: _MoveGuideMarker.duckDestination(
                key: ValueKey<String>('duck-assistance-to-$squareName'),
                duckAsset: duckAsset,
              ),
            ),
          if (!hasMovePrompt && !hasAssistance && isWhiteLastFrom)
            Positioned(
              left: 2,
              bottom: 2,
              child: _LastMoveMarker(
                key: ValueKey<String>('last-move-white-from-$squareName'),
                color: whiteLastColor,
                destination: false,
                mostRecent: whiteLastIsMostRecent,
              ),
            ),
          if (!hasMovePrompt && !hasAssistance && isWhiteLastTo)
            Positioned(
              left: 2,
              top: 2,
              child: _LastMoveMarker(
                key: ValueKey<String>('last-move-white-to-$squareName'),
                color: whiteLastColor,
                destination: true,
                mostRecent: whiteLastIsMostRecent,
              ),
            ),
          if (!hasMovePrompt && !hasAssistance && isBlackLastFrom)
            Positioned(
              right: 2,
              bottom: 2,
              child: _LastMoveMarker(
                key: ValueKey<String>('last-move-black-from-$squareName'),
                color: blackLastColor,
                destination: false,
                mostRecent: blackLastIsMostRecent,
              ),
            ),
          if (!hasMovePrompt && !hasAssistance && isBlackLastTo)
            Positioned(
              right: 2,
              top: 2,
              child: _LastMoveMarker(
                key: ValueKey<String>('last-move-black-to-$squareName'),
                color: blackLastColor,
                destination: true,
                mostRecent: blackLastIsMostRecent,
              ),
            ),
        ],
      ),
    );
    final Widget guidedContents = hasAmberGuidance
        ? AnimatedBuilder(
            animation: amberPulse,
            child: contents.child,
            builder: (context, child) => DecoratedBox(
              decoration: BoxDecoration(
                color: background,
                border: Border.all(
                  color: const Color(0xFFFFB300).withValues(
                    alpha:
                        1 - .28 * Curves.easeInOut.transform(amberPulse.value),
                  ),
                  width: 3,
                ),
              ),
              child: child,
            ),
          )
        : contents;
    final Widget squareWidget = isLight || hasAmberGuidance
        ? ClipPath(
            clipper: _squareChamferClipper,
            child: guidedContents,
          )
        : guidedContents;
    final Widget interactiveSquare = onPressed == null
        ? ColoredBox(color: const Color(0xFF526980), child: squareWidget)
        : GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: onPressed,
            child: ColoredBox(
              color: const Color(0xFF526980),
              child: squareWidget,
            ),
          );
    return Semantics(
      label: semanticsLabel,
      button: onPressed != null,
      selected: isSelected,
      onTap: onPressed,
      child: interactiveSquare,
    );
  }

  Widget _withPromotionMotion(Widget child) {
    final motion = promotionMotion;
    if (!promotionConfirmed || motion == null) return child;
    return AnimatedBuilder(
      animation: motion,
      child: child,
      builder: (context, child) => Transform.scale(
        scale: 1 + 0.16 * math.sin(math.pi * motion.value),
        child: child,
      ),
    );
  }
}

class _PromotionHighlight extends StatelessWidget {
  const _PromotionHighlight({super.key, required this.color, this.motion});

  final Color color;
  final Animation<double>? motion;

  @override
  Widget build(BuildContext context) {
    Widget highlight(double strength) => DecoratedBox(
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.34 + strength * 0.32),
        border: Border.all(color: const Color(0xFF15252C), width: 1.5),
      ),
      child: Padding(
        padding: const EdgeInsets.all(1.5),
        child: DecoratedBox(
          decoration: BoxDecoration(
            border: Border.all(
              color: Color.lerp(color, Colors.white, 0.68)!,
              width: 2 + strength * 2,
            ),
          ),
        ),
      ),
    );
    final animation = motion;
    return animation == null
        ? highlight(1)
        : AnimatedBuilder(
            animation: animation,
            builder: (context, _) =>
                highlight(0.4 + 0.6 * math.sin(math.pi * animation.value)),
          );
  }
}

class _LastMoveMarker extends StatelessWidget {
  const _LastMoveMarker({
    super.key,
    required this.color,
    required this.destination,
    required this.mostRecent,
  });

  final Color color;
  final bool destination;
  final bool mostRecent;

  @override
  Widget build(BuildContext context) {
    return ExcludeSemantics(
      child: Opacity(
        opacity: mostRecent ? 0.9 : 0.55,
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: Theme.of(
              context,
            ).colorScheme.surface.withValues(alpha: 0.82),
            shape: BoxShape.circle,
          ),
          child: Padding(
            padding: const EdgeInsets.all(1),
            child: Icon(
              destination ? Icons.adjust_rounded : Icons.circle_outlined,
              size: mostRecent ? 12 : 10,
              color: color,
            ),
          ),
        ),
      ),
    );
  }
}

class _MoveGuideMarker extends StatelessWidget {
  const _MoveGuideMarker({
    super.key,
    required IconData this.icon,
    this.color = const Color(0xFF2F6173),
  }) : duckAsset = null;

  const _MoveGuideMarker.duckDestination({
    super.key,
    required String this.duckAsset,
  }) : icon = null,
       color = const Color(0xFFF4B942);

  final IconData? icon;
  final String? duckAsset;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final asset = duckAsset;
    return ExcludeSemantics(
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: asset == null
              ? Theme.of(context).colorScheme.surface.withValues(alpha: 0.9)
              : const Color(0xFF203342),
          shape: BoxShape.circle,
          border: Border.all(color: color, width: 1.5),
        ),
        child: Padding(
          padding: const EdgeInsets.all(2),
          child: asset == null
              ? Icon(icon, size: 13, color: color)
              : SizedBox.square(
                  dimension: 14,
                  child: _BoardArtwork(assetPath: asset),
                ),
        ),
      ),
    );
  }
}

class _BoardArtwork extends StatelessWidget {
  const _BoardArtwork({
    super.key,
    required this.assetPath,
    this.accentColor,
    this.invertNeutral = false,
  });

  final String assetPath;
  final Color? accentColor;
  final bool invertNeutral;

  @override
  Widget build(BuildContext context) {
    Widget artwork = Image.asset(
      assetPath,
      excludeFromSemantics: true,
      fit: BoxFit.contain,
      filterQuality: FilterQuality.high,
      gaplessPlayback: true,
      cacheWidth: 256,
    );
    if (accentColor case final Color accent) {
      artwork = ColorFiltered(
        colorFilter: _pieceToneFilter(accent, invertNeutral: invertNeutral),
        child: artwork,
      );
    }
    return SizedBox.expand(child: artwork);
  }
}

Color litchPieceAccentColor(
  HomeBoardSnapshot snapshot, {
  required bool whitePiece,
}) {
  // The stored wheels belong to fixed near/far ends. Logical players select
  // their assigned end's hue; changing assignment never moves the end colors.
  if (!snapshot.playerColorsConfirmed) return const Color(0xFF808080);
  final bool useP2 =
      !whitePiece != (snapshot.physicalStatus?.p1AtOppositeEnd == true);
  return litchWheelPreviewColor(
    useP2 ? snapshot.p2PieceWheel : snapshot.p1PieceWheel,
  );
}

ColorFilter _pieceToneFilter(Color accent, {required bool invertNeutral}) {
  final List<double> rows = <double>[];
  for (final double target in <double>[accent.r, accent.g, accent.b]) {
    if (invertNeutral) {
      rows.addAll(<double>[-target / 2, target - 1, -target / 2, 0, 255]);
    } else {
      final double neutral = (1 - target) / 2;
      rows.addAll(<double>[neutral, target, neutral, 0, 0]);
    }
  }
  rows.addAll(const <double>[0, 0, 0, 1, 0]);
  return ColorFilter.matrix(rows);
}

bool _isWhitePiece(String piece) => piece == piece.toUpperCase();

String _capitalize(String value) {
  if (value.isEmpty) {
    return value;
  }
  return '${value[0].toUpperCase()}${value.substring(1)}';
}
