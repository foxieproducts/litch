import 'package:flutter/widgets.dart';

import 'litch_workspace_geometry.dart';

/// Keeps a full-page form or article within reading distance on a tablet.
class LitchReadingPane extends StatelessWidget {
  const LitchReadingPane({super.key, required this.child});

  static const proseWidth = 608.0;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    if (!LitchWorkspaceGeometry.isTabletWindow(MediaQuery.sizeOf(context))) {
      return child;
    }
    return Align(
      alignment: Alignment.topCenter,
      child: SizedBox(
        width: LitchWorkspaceGeometry.readableContentWidth,
        child: child,
      ),
    );
  }
}
