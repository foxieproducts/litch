import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';

enum ReleaseNotesDialogStyle { standard, firmwareOffer }

/// Shared presentation for app notes and the board's available update notes.
class ReleaseNotesDialog extends StatefulWidget {
  const ReleaseNotesDialog({
    super.key,
    required this.title,
    required this.notes,
    this.preferenceExplanation,
    this.preferenceLabel,
    this.style = ReleaseNotesDialogStyle.standard,
    required this.suppressFuture,
    required this.onSuppressFutureChanged,
    required this.primaryAction,
    this.secondaryAction,
    this.subtitle,
  });

  final String title;
  final Widget notes;
  final String? subtitle;
  final String? preferenceExplanation;
  final String? preferenceLabel;
  final ReleaseNotesDialogStyle style;
  final bool suppressFuture;
  final Future<bool> Function(bool) onSuppressFutureChanged;
  final String primaryAction;
  final String? secondaryAction;

  @override
  State<ReleaseNotesDialog> createState() => _ReleaseNotesDialogState();
}

class _ReleaseNotesDialogState extends State<ReleaseNotesDialog> {
  late bool _suppressFuture = widget.suppressFuture;
  bool _saving = false;
  bool _saveFailed = false;

  Future<void> _changePreference(bool? value) async {
    if (value == null || _saving) return;
    final previous = _suppressFuture;
    setState(() {
      _suppressFuture = value;
      _saving = true;
      _saveFailed = false;
    });
    bool saved = false;
    try {
      saved = await widget.onSuppressFutureChanged(value);
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _saving = false;
      _saveFailed = !saved;
      if (!saved) _suppressFuture = previous;
    });
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final theme = Theme.of(context);
    final compact = widget.style == ReleaseNotesDialogStyle.firmwareOffer;
    final preference = CheckboxListTile(
      key: const ValueKey<String>('release-notice-opt-out'),
      contentPadding: EdgeInsets.zero,
      controlAffinity: ListTileControlAffinity.leading,
      dense: compact ? true : null,
      title: Text(
        widget.preferenceLabel ?? strings.dontShowReleaseNoticeAgain,
        style: compact
            ? theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurface,
              )
            : null,
      ),
      subtitle: widget.preferenceExplanation == null
          ? null
          : Text(widget.preferenceExplanation!),
      value: _suppressFuture,
      onChanged: _saving ? null : _changePreference,
    );
    final error = Text(
      strings.releaseNoticePreferenceFailed,
      style: TextStyle(color: theme.colorScheme.error),
    );
    final actions = OverflowBar(
      alignment: MainAxisAlignment.end,
      spacing: 8,
      overflowSpacing: 8,
      children: <Widget>[
        if (widget.secondaryAction case final action?)
          TextButton(
            style: compact
                ? TextButton.styleFrom(minimumSize: const Size(48, 48))
                : null,
            onPressed: _saving ? null : () => Navigator.of(context).pop(false),
            child: Text(action),
          ),
        FilledButton(
          style: compact
              ? FilledButton.styleFrom(minimumSize: const Size(48, 48))
              : null,
          onPressed: _saving ? null : () => Navigator.of(context).pop(true),
          child: Text(widget.primaryAction),
        ),
      ],
    );
    return PopScope<bool>(
      canPop: !_saving,
      child: AlertDialog(
        clipBehavior: Clip.antiAlias,
        backgroundColor: compact
            ? theme.colorScheme.surfaceContainerHigh.withValues(
                alpha: MediaQuery.highContrastOf(context) ? 1 : .96,
              )
            : null,
        surfaceTintColor: compact ? Colors.transparent : null,
        insetPadding: EdgeInsets.symmetric(
          horizontal: 16,
          vertical: compact ? 16 : 24,
        ),
        titlePadding: compact ? const EdgeInsets.fromLTRB(20, 16, 20, 0) : null,
        contentPadding: compact
            ? const EdgeInsets.fromLTRB(20, 10, 20, 8)
            : null,
        titleTextStyle: compact
            ? theme.textTheme.titleMedium?.copyWith(
                color: theme.colorScheme.onSurface,
                fontWeight: FontWeight.w700,
              )
            : null,
        contentTextStyle: compact
            ? theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.onSurface,
              )
            : null,
        scrollable: true,
        title: Text(widget.title),
        content: SizedBox(
          width: double.maxFinite,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              if (widget.subtitle case final subtitle?) ...<Widget>[
                Text(subtitle),
                SizedBox(height: compact ? 8 : 12),
              ],
              widget.notes,
            ],
          ),
        ),
        actionsPadding: compact
            ? const EdgeInsets.fromLTRB(12, 0, 12, 8)
            : const EdgeInsets.fromLTRB(16, 0, 16, 16),
        actions: <Widget>[
          SizedBox(
            width: double.maxFinite,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: compact
                  ? <Widget>[
                      actions,
                      if (_saveFailed) error,
                      const SizedBox(height: 4),
                      preference,
                    ]
                  : <Widget>[
                      preference,
                      if (_saveFailed) error,
                      const SizedBox(height: 8),
                      actions,
                    ],
            ),
          ),
        ],
      ),
    );
  }
}
