import 'dart:math' as math;

import 'package:flutter/widgets.dart';

/// Shared canvas breathing space and landscape tool/navigation geometry.
/// [safeSize] excludes each actual system inset exactly once.
class LitchWorkspaceGeometry {
  const LitchWorkspaceGeometry(
    this.safeSize, {
    this.safeInsets = EdgeInsets.zero,
  });

  final Size safeSize;
  final EdgeInsets safeInsets;
  static const outerGap = 8.0;
  static const contentEdgeInset = 12.0;
  static const canvasHorizontalInset = contentEdgeInset;
  static const landscapeGap = 10.0;
  static const landscapeToolGap = 12.0;
  static const navigationHeight = 76.8;
  static const readableContentWidth = 720.0;
  static const tabletBoardGap = 16.0;
  // Room for four destinations and board status with at least 48px per target.
  static const controlGroupWidth = 320.0;
  static const tabletNavigationHeight = 64.0;

  static bool isTabletWindow(Size windowSize) => windowSize.shortestSide >= 600;

  static bool usesCompactLandscape(Size windowSize, Size usableSize) =>
      !isTabletWindow(windowSize) &&
      usableSize.width >= 480 &&
      usableSize.height <= 480 &&
      usableSize.width > usableSize.height;

  static bool usesTabletBoardColumns(Size windowSize, Size paneSize) =>
      isTabletWindow(windowSize) &&
      windowSize.width > windowSize.height &&
      paneSize.width >= 680 &&
      paneSize.height >= 360;

  /// Keep a full touch target per painting cell beside the tools in either orientation.
  static bool fitsTabletCraftDock(Size paneSize) =>
      tabletCanvasSide(paneSize, columns: true) >= 8 * 48;

  /// Fit the square in the pane left by system insets, navigation and controls.
  static double tabletCanvasSide(Size paneSize, {required bool columns}) {
    final width = paneSize.width - contentEdgeInset * 2;
    // Keep the portrait workbench accessible below the canvas on shorter windows.
    final toolsHeight = math.min(440.0, paneSize.height * .45);
    return math.max(
      0.0,
      columns
          ? math.min(
              width - tabletBoardGap - controlGroupWidth,
              paneSize.height - contentEdgeInset * 2,
            )
          : math.min(width, paneSize.height - toolsHeight),
    );
  }

  static double portraitCanvasSide(double availableWidth) =>
      math.max(0, availableWidth - canvasHorizontalInset * 2);

  /// Existing system clearance already provides breathing room at that edge.
  /// The shell consumes it once; add only the remainder of the ordinary gap.
  EdgeInsets get landscapeEdgeInsets {
    final vertical = math.max(safeInsets.top, safeInsets.bottom);
    return EdgeInsets.fromLTRB(
      math.max(0, landscapeGap - safeInsets.left),
      math.max(0, landscapeGap - vertical),
      math.max(0, landscapeGap - safeInsets.right),
      math.max(0, landscapeGap - vertical),
    );
  }

  double get _minimumInspectorWidth => math.max(
    safeSize.height >= 400 ? 248 : 252,
    (safeSize.width - landscapeEdgeInsets.horizontal) / 3,
  );

  double get landscapeCanvasSide => math.max(
    0,
    math.min(
      safeSize.height - landscapeEdgeInsets.vertical,
      safeSize.width -
          landscapeEdgeInsets.horizontal -
          _minimumInspectorWidth -
          76 -
          48 -
          landscapeToolGap * 3,
    ),
  );

  // Give spare width to the inspector, rather than empty wells beside a
  // height-limited canvas. Play and navigation use the same resulting rectangle.
  double get compactNavigationWidth => math.max(
    _minimumInspectorWidth,
    safeSize.width -
        landscapeEdgeInsets.horizontal -
        landscapeCanvasSide -
        76 -
        48 -
        landscapeToolGap * 3,
  );

  double get verticalGap => math.min(
    landscapeEdgeInsets.top,
    math.max(0, (safeSize.height - 5 * 48 - navigationHeight) / 2),
  );
}
