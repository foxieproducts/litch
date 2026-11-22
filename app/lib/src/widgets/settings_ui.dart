import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import 'compact_controls.dart';

class SettingsSectionHeading extends StatelessWidget {
  const SettingsSectionHeading({
    super.key,
    required this.title,
    this.description,
  });

  final String title;
  final String? description;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      header: true,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(title, style: Theme.of(context).textTheme.titleLarge),
          if (description case final String description) ...<Widget>[
            const SizedBox(height: 4),
            Text(
              description,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                color: Theme.of(context).colorScheme.onSurfaceVariant,
              ),
            ),
          ],
        ],
      ),
    );
  }
}

class SettingsActionButton extends StatelessWidget {
  const SettingsActionButton({
    super.key,
    required this.label,
    required this.tooltip,
    required this.icon,
    required this.onPressed,
    this.pending = false,
  });

  final String label;
  final String tooltip;
  final IconData icon;
  final VoidCallback? onPressed;
  final bool pending;

  @override
  Widget build(BuildContext context) {
    return LitchCompactControls(
      child: Tooltip(
        message: tooltip,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 48, minWidth: 48),
          child: OutlinedButton.icon(
            onPressed: onPressed,
            icon: pending
                ? const SizedBox.square(
                    dimension: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : Icon(icon),
            label: Text(label),
          ),
        ),
      ),
    );
  }
}

class SettingsActionStatus extends StatelessWidget {
  const SettingsActionStatus({
    super.key,
    required this.message,
    required this.icon,
    this.liveRegion = false,
  });

  final String message;
  final IconData icon;
  final bool liveRegion;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      liveRegion: liveRegion,
      label: message,
      excludeSemantics: true,
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        decoration: BoxDecoration(
          color: Theme.of(context).colorScheme.surfaceContainerLow,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Icon(icon, size: 20),
            const SizedBox(width: 10),
            Expanded(child: Text(message)),
          ],
        ),
      ),
    );
  }
}

class SettingsUnavailableControl extends StatelessWidget {
  const SettingsUnavailableControl({super.key, required this.label});

  final String label;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      label: litchStrings(context).unavailableControlSemantics(label),
      excludeSemantics: true,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            const Icon(Icons.info_outline, size: 20),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                litchStrings(context).unavailableControl(label),
                style: Theme.of(context).textTheme.bodyMedium,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// Shared label/control geometry for Setup's on/off settings.
class SettingsToggleRow extends StatelessWidget {
  const SettingsToggleRow({
    super.key,
    required this.title,
    required this.control,
    this.help,
    this.leading,
  });

  final Widget title;
  final Widget control;
  final Widget? help;
  final Widget? leading;

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: <Widget>[
          if (leading != null) ...[leading!, const SizedBox(width: 12)],
          Expanded(child: title),
          const SizedBox(width: 12),
          ConstrainedBox(
            constraints: const BoxConstraints(minWidth: 48, minHeight: 48),
            child: control,
          ),
        ],
      ),
      if (help != null) ...[const SizedBox(height: 4), help!],
    ],
  );
}

/// One joined, single-choice control for short named Setup alternatives.
class SettingsChoicePills<T> extends StatelessWidget {
  const SettingsChoicePills({
    super.key,
    required this.segments,
    required this.selected,
    this.onSelectionChanged,
    this.optionKey,
    this.trailingIcons = const {},
  }) : assert(selected.length == 1);

  final List<ButtonSegment<T>> segments;
  final Set<T> selected;
  final ValueChanged<Set<T>>? onSelectionChanged;
  final Key Function(T value)? optionKey;
  final Map<T, Widget> trailingIcons;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) {
      const padding = EdgeInsets.symmetric(horizontal: 6, vertical: 8);
      const iconSize = 18.0;
      const iconGap = 4.0;
      final textStyle = Theme.of(context).textTheme.labelLarge!;
      final textScaler = MediaQuery.textScalerOf(context);
      final textDirection = Directionality.of(context);
      Size measureLabel(Widget? label, double width) {
        if (label is! Text) return Size.zero;
        final painter = TextPainter(
          text:
              label.textSpan ??
              TextSpan(text: label.data, style: textStyle.merge(label.style)),
          textDirection: textDirection,
          textScaler: textScaler,
          textWidthBasis: TextWidthBasis.longestLine,
          locale: Localizations.maybeLocaleOf(context),
        )..layout(maxWidth: width);
        final size = painter.size;
        painter.dispose();
        return size;
      }

      // Reserve the check for every option before choosing direction, so a
      // selection change cannot turn a readable row into broken words.
      final trailingWidth = trailingIcons.isEmpty ? 0.0 : iconSize + iconGap;
      final adornmentWidth =
          padding.horizontal + iconSize + iconGap + trailingWidth;
      final horizontalLabelWidth =
          constraints.maxWidth / segments.length - adornmentWidth;
      final stack = segments.any(
        (segment) =>
            measureLabel(segment.label, double.infinity).width >
            horizontalLabelWidth,
      );
      final labelWidth =
          ((stack
                      ? constraints.maxWidth
                      : constraints.maxWidth / segments.length) -
                  adornmentWidth)
              .clamp(0.0, double.infinity);
      return SizedBox(
        width: constraints.maxWidth,
        // The SDK reverses vertical segments in RTL as well as horizontal
        // segments. Keep a vertical choice list top-to-bottom in every locale,
        // while its label content still follows the ambient text direction.
        child: Directionality(
          textDirection: stack ? TextDirection.ltr : textDirection,
          child: SegmentedButton<T>(
            // The full-width parent already sizes horizontal segments. Keep
            // native intrinsic sizing on both axes: the SDK retains its old
            // expanded flag when direction changes, which otherwise expands a
            // new vertical list to infinite height during phone rotation.
            direction: stack ? Axis.vertical : Axis.horizontal,
            // The SDK adds icon padding that breaks short four-option rows.
            // Keep its native buttons/semantics and draw the check in the label.
            showSelectedIcon: false,
            style: ButtonStyle(
              padding: const WidgetStatePropertyAll(padding),
              textStyle: WidgetStatePropertyAll(textStyle),
              minimumSize: const WidgetStatePropertyAll(Size(48, 48)),
              tapTargetSize: MaterialTapTargetSize.padded,
              shape: stack
                  ? WidgetStatePropertyAll(
                      RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(24),
                      ),
                    )
                  : null,
            ),
            segments: [
              for (final segment in segments)
                ButtonSegment<T>(
                  value: segment.value,
                  tooltip: segment.tooltip,
                  enabled: segment.enabled,
                  label: KeyedSubtree(
                    key:
                        optionKey?.call(segment.value) ??
                        ValueKey<T>(segment.value),
                    child: Directionality(
                      textDirection: textDirection,
                      child: SizedBox(
                        width: labelWidth + iconSize + iconGap + trailingWidth,
                        child: Row(
                          mainAxisSize: MainAxisSize.min,
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            if (selected.contains(segment.value) ||
                                segment.icon != null) ...[
                              ExcludeSemantics(
                                child: SizedBox.square(
                                  dimension: iconSize,
                                  child: selected.contains(segment.value)
                                      ? const Icon(Icons.check, size: iconSize)
                                      : segment.icon,
                                ),
                              ),
                              const SizedBox(width: iconGap),
                            ],
                            // Native vertical layout asks for height at infinite
                            // width. Supply the height measured at the real label
                            // width, including space for the selected check.
                            SizedBox(
                              // Center the visible check/icon and text together;
                              // a short label must not fill the wrap allowance.
                              width: measureLabel(
                                segment.label,
                                labelWidth,
                              ).width,
                              height: measureLabel(
                                segment.label,
                                labelWidth,
                              ).height.clamp(32.0, double.infinity),
                              child: Center(
                                child: DefaultTextStyle.merge(
                                  textAlign: TextAlign.center,
                                  softWrap: true,
                                  overflow: TextOverflow.visible,
                                  child:
                                      segment.label ?? const SizedBox.shrink(),
                                ),
                              ),
                            ),
                            if (trailingIcons[segment.value]
                                case final trailing?) ...[
                              const SizedBox(width: iconGap),
                              ExcludeSemantics(
                                child: SizedBox.square(
                                  dimension: iconSize,
                                  child: trailing,
                                ),
                              ),
                            ],
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
            ],
            selected: selected,
            onSelectionChanged: onSelectionChanged,
          ),
        ),
      );
    },
  );
}
