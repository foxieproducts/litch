import 'dart:math' as math;

import 'package:flutter/material.dart';

/// Navigation and compact action labels keep their designed row geometry.
/// Reading content outside this boundary retains the system text scaler.
class LitchCompactControls extends StatelessWidget {
  const LitchCompactControls({super.key, required this.child});

  static const maximumTextScale = 1.3;
  final Widget child;

  static TextScaler textScalerOf(BuildContext context) =>
      MediaQuery.textScalerOf(context).clamp(maxScaleFactor: maximumTextScale);

  static double toolbarHeightOf(BuildContext context) =>
      math.max(kToolbarHeight, textScalerOf(context).scale(kToolbarHeight));

  @override
  Widget build(BuildContext context) => MediaQuery.withClampedTextScaling(
    maxScaleFactor: maximumTextScale,
    child: child,
  );
}

class LitchCompactAppBar extends StatelessWidget
    implements PreferredSizeWidget {
  const LitchCompactAppBar({super.key, required this.child});

  final AppBar child;

  @override
  Size get preferredSize => child.preferredSize;

  @override
  Widget build(BuildContext context) => LitchCompactControls(child: child);
}
