import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../widgets/litch_workspace_geometry.dart';

/// Places a square board beside its controls when both panes can stay usable.
class GamePositionLayout extends StatelessWidget {
  const GamePositionLayout({
    super.key,
    required this.board,
    required this.controls,
    required this.singlePaneBuilder,
    required this.horizontalPadding,
    this.columnsKey,
  });

  final Widget board;
  final Widget controls;
  final Widget Function(BuildContext, Size) singlePaneBuilder;
  final double horizontalPadding;
  final Key? columnsKey;

  @override
  Widget build(BuildContext context) {
    final window = MediaQuery.sizeOf(context);
    return LayoutBuilder(
      builder: (context, constraints) {
        final pane = constraints.biggest;
        if (!LitchWorkspaceGeometry.usesTabletBoardColumns(window, pane)) {
          return singlePaneBuilder(context, pane);
        }
        const gap = LitchWorkspaceGeometry.tabletBoardGap;
        // Leave room for localized, scaled controls as well as eight board files.
        final controlMinWidth = math.max(
          264.0,
          MediaQuery.textScalerOf(context).scale(16) * 14,
        );
        final side = math.min(
          LitchWorkspaceGeometry.tabletCanvasSide(pane, columns: true),
          pane.width - 2 * horizontalPadding - gap - controlMinWidth,
        );
        if (side < 320) return singlePaneBuilder(context, pane);
        return Padding(
          padding: EdgeInsets.symmetric(horizontal: horizontalPadding),
          child: Center(
            child: ConstrainedBox(
              constraints: BoxConstraints(
                maxWidth:
                    side + gap + LitchWorkspaceGeometry.readableContentWidth,
              ),
              child: Row(
                key: columnsKey,
                children: <Widget>[
                  SizedBox.square(dimension: side, child: board),
                  const SizedBox(width: gap),
                  Expanded(child: controls),
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Keeps a reviewed position visible while its moves or FEN details scroll.
class GamePositionReviewLayout extends StatelessWidget {
  const GamePositionReviewLayout({
    super.key,
    required this.scrollKey,
    required this.heading,
    required this.board,
    required this.details,
    required this.padding,
    required this.boardTopGap,
    required this.boardBottomGap,
  });

  final Key scrollKey;
  final Widget heading;
  final Widget board;
  final List<Widget> details;
  final EdgeInsets padding;
  final double boardTopGap;
  final double boardBottomGap;

  @override
  Widget build(BuildContext context) {
    final tablet = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    return GamePositionLayout(
      board: board,
      horizontalPadding: padding.left,
      controls: SingleChildScrollView(
        key: scrollKey,
        padding: padding.copyWith(left: 0, right: 0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            heading,
            SizedBox(height: boardBottomGap),
            ...details,
          ],
        ),
      ),
      singlePaneBuilder: (context, pane) {
        final textWidth = tablet
            ? LitchWorkspaceGeometry.readableContentWidth
            : 620.0;
        final boardWidth = tablet
            ? LitchWorkspaceGeometry.tabletCanvasSide(pane, columns: false)
            : 430.0;
        return SingleChildScrollView(
          key: scrollKey,
          padding: padding,
          child: SizedBox(
            width: double.infinity,
            child: Column(
              children: <Widget>[
                SizedBox(width: textWidth, child: heading),
                SizedBox(height: boardTopGap),
                SizedBox(width: boardWidth, child: board),
                SizedBox(height: boardBottomGap),
                SizedBox(
                  width: textWidth,
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: details,
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }
}
