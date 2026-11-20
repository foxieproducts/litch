import 'package:flutter/material.dart';

abstract final class LitchShapes {
  static const double panelRadius = 12;
  static const double boardChamfer = 8;
  static const double squareChamfer = 3;

  static ShapeBorder panel({Color? outline}) {
    return RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(panelRadius),
      side: outline == null ? BorderSide.none : BorderSide(color: outline),
    );
  }
}

ThemeData buildLitchTheme() => _buildLitchTheme(Brightness.light);

ThemeData buildLitchDarkTheme() => _buildLitchTheme(Brightness.dark);

ThemeData _buildLitchTheme(Brightness brightness) {
  final bool dark = brightness == Brightness.dark;
  final ColorScheme colors =
      ColorScheme.fromSeed(
        seedColor: const Color(0xFF2E6266),
        brightness: brightness,
        surface: dark ? const Color(0xFF111819) : const Color(0xFFF8F7F2),
      ).copyWith(
        primary: dark ? const Color(0xFF91CDD0) : const Color(0xFF285D62),
        onPrimary: dark ? const Color(0xFF00373A) : Colors.white,
        secondary: dark ? const Color(0xFFE8BD83) : const Color(0xFF8A5A22),
        tertiary: dark ? const Color(0xFFA6D0AE) : const Color(0xFF456B50),
        error: dark ? const Color(0xFFFFB4A9) : const Color(0xFF9C3D32),
        surfaceContainer: dark
            ? const Color(0xFF1B2526)
            : const Color(0xFFF0EEE7),
        surfaceContainerLow: dark
            ? const Color(0xFF151E1F)
            : const Color(0xFFF5F3ED),
        outline: dark ? const Color(0xFF899393) : const Color(0xFF74766F),
        outlineVariant: dark
            ? const Color(0xFF3E494A)
            : const Color(0xFFC7C8C0),
      );

  return ThemeData(
    colorScheme: colors,
    scaffoldBackgroundColor: colors.surface,
    appBarTheme: AppBarTheme(
      backgroundColor: colors.surface,
      foregroundColor: colors.onSurface,
      elevation: 0,
      scrolledUnderElevation: 1,
    ),
    cardTheme: CardThemeData(
      color: colors.surfaceContainerLow,
      elevation: 0,
      margin: EdgeInsets.zero,
      shape: LitchShapes.panel(outline: colors.outlineVariant),
    ),
    navigationBarTheme: NavigationBarThemeData(
      backgroundColor: colors.surfaceContainer,
      indicatorColor: colors.primary.withValues(alpha: 0.14),
      indicatorShape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
      ),
      height: 76,
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        minimumSize: const Size(48, 48),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        minimumSize: const Size(48, 48),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(14)),
      ),
    ),
    useMaterial3: true,
  );
}
