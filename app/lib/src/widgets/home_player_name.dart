import 'dart:async';

import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../models/local_player_profile.dart';

/// One assignment control for both portrait edges and landscape details.
class HomePlayerName extends StatelessWidget {
  const HomePlayerName({
    super.key,
    required this.side,
    required this.name,
    required this.accent,
    required this.onAssign,
    required this.onClear,
    this.onChooseColor,
    this.pending = false,
  });

  final LocalPlayerChessSide side;
  final String? name;
  final Color accent;
  final VoidCallback? onAssign;
  final VoidCallback? onClear;
  final VoidCallback? onChooseColor;
  final bool pending;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final white = side == LocalPlayerChessSide.white;
    final slot = white ? 'P1' : 'P2';
    final spoken = strings.playerSpoken(white ? '1' : '2');
    final colors = Theme.of(context).colorScheme;
    // Preserve the hue while giving text enough contrast in either appearance.
    Color textColor = accent;
    final background = colors.surface;
    double contrast(Color color) {
      final a = color.computeLuminance();
      final b = background.computeLuminance();
      return (a > b ? a + .05 : b + .05) / (a > b ? b + .05 : a + .05);
    }

    for (var step = 1; step <= 10 && contrast(textColor) < 4.5; step++) {
      textColor = Color.lerp(accent, colors.onSurface, step / 10)!;
    }
    return Row(
      key: ValueKey('home-player-name-${side.name}'),
      children: [
        IconButton(
          key: ValueKey('home-player-color-${side.name}'),
          tooltip: white ? strings.chooseP1Color : strings.chooseP2Color,
          onPressed: onChooseColor,
          constraints: const BoxConstraints(minWidth: 48, minHeight: 48),
          icon: ExcludeSemantics(
            child: Container(
              key: ValueKey('home-player-color-swatch-${side.name}'),
              width: 24,
              height: 24,
              decoration: BoxDecoration(color: accent, shape: BoxShape.circle),
            ),
          ),
        ),
        Expanded(
          child: TextButton(
            key: ValueKey('home-player-name-edit-${side.name}'),
            onPressed: onAssign,
            style: TextButton.styleFrom(
              minimumSize: const Size(48, 48),
              padding: const EdgeInsets.symmetric(horizontal: 4),
              alignment: Alignment.centerLeft,
            ),
            child: Semantics(
              label: strings.assignPlayerTitle(spoken),
              value: name ?? strings.playerNameEmpty,
              excludeSemantics: true,
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(slot, style: TextStyle(color: textColor, fontSize: 12)),
                  const SizedBox(width: 10),
                  Flexible(
                    child: Text(
                      name ?? strings.playerNameEmpty,
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        color: textColor,
                        fontSize: 20.4,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Icon(Icons.edit_outlined, size: 16, color: textColor),
                ],
              ),
            ),
          ),
        ),
        IconButton(
          key: ValueKey('home-player-name-reset-${side.name}'),
          tooltip: strings.clearPlayerAssignment(spoken),
          onPressed: name == null ? null : onClear,
          icon: pending
              ? const SizedBox(
                  width: 20,
                  height: 20,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              : const Icon(Icons.close),
          constraints: const BoxConstraints(minWidth: 48, minHeight: 48),
        ),
      ],
    );
  }
}

Future<LocalPlayerDirectory?> showHomePlayerAssignment({
  required BuildContext context,
  required LocalPlayerChessSide side,
  required String? currentName,
  required Future<LocalPlayerDirectory> Function(
    String, {
    Future<void>? cancellation,
    bool Function()? beginCommit,
  })
  assign,
}) => showDialog<LocalPlayerDirectory>(
  context: context,
  builder: (_) => _PlayerAssignmentDialog(
    side: side,
    currentName: currentName,
    assign: assign,
  ),
);

class _PlayerAssignmentDialog extends StatefulWidget {
  const _PlayerAssignmentDialog({
    required this.side,
    required this.currentName,
    required this.assign,
  });
  final LocalPlayerChessSide side;
  final String? currentName;
  final Future<LocalPlayerDirectory> Function(
    String, {
    Future<void>? cancellation,
    bool Function()? beginCommit,
  })
  assign;
  @override
  State<_PlayerAssignmentDialog> createState() =>
      _PlayerAssignmentDialogState();
}

class _PlayerAssignmentDialogState extends State<_PlayerAssignmentDialog> {
  late final controller = TextEditingController(text: widget.currentName ?? '');
  bool busy = false;
  bool committing = false;
  final cancellation = Completer<void>();
  String? error;
  void cancel() {
    if (committing) return;
    if (!cancellation.isCompleted) cancellation.complete();
  }

  void cancelAndClose() {
    if (committing) return;
    cancel();
    Navigator.of(context).pop();
  }

  @override
  void dispose() {
    cancel();
    controller.dispose();
    super.dispose();
  }

  Future<void> submit() async {
    if (busy) return;
    setState(() {
      busy = true;
      error = null;
    });
    try {
      normalizeLocalPlayerName(controller.text);
      final directory = await widget.assign(
        controller.text,
        cancellation: cancellation.future,
        beginCommit: () {
          if (!mounted || cancellation.isCompleted) return false;
          setState(() => committing = true);
          return true;
        },
      );
      if (mounted) Navigator.of(context).pop(directory);
    } catch (failure) {
      if (!mounted) return;
      setState(() {
        busy = false;
        committing = false;
        error = failure is LocalPlayerProfileLimitException
            ? litchStrings(context).playerNameCapacity
            : failure is FormatException
            ? litchStrings(context).playerNameInvalid
            : litchStrings(context).playerAssignmentFailed;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return PopScope(
      canPop: !busy,
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) {
          cancel();
        } else if (!committing) {
          cancelAndClose();
        }
      },
      child: AlertDialog(
        title: Text(
          strings.assignPlayerTitle(
            strings.playerSpoken(
              widget.side == LocalPlayerChessSide.white ? '1' : '2',
            ),
          ),
        ),
        content: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              TextField(
                key: const ValueKey('home-player-name-input'),
                controller: controller,
                autofocus: true,
                enabled: !busy,
                textCapitalization: TextCapitalization.words,
                textInputAction: TextInputAction.done,
                onSubmitted: (_) => submit(),
                decoration: InputDecoration(
                  labelText: strings.playerNameLabel,
                  errorText: error,
                  errorMaxLines: 4,
                ),
              ),
              const SizedBox(height: 12),
              Text(strings.playerNameScope),
              if (busy) ...[
                const SizedBox(height: 16),
                Semantics(
                  key: const ValueKey('home-player-name-progress'),
                  liveRegion: true,
                  label: committing
                      ? strings.playerAssignmentSaving
                      : strings.playerAssignmentReading,
                  child: ExcludeSemantics(
                    child: Row(
                      children: [
                        const SizedBox(
                          width: 20,
                          height: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Text(
                            committing
                                ? strings.playerAssignmentSaving
                                : strings.playerAssignmentReading,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ],
            ],
          ),
        ),
        actions: [
          TextButton(
            key: const ValueKey('home-player-name-cancel'),
            onPressed: committing ? null : cancelAndClose,
            child: Text(strings.cancelAction),
          ),
          FilledButton(
            key: const ValueKey('home-player-name-submit'),
            onPressed: busy ? null : submit,
            child: Text(strings.assignPlayerAction),
          ),
        ],
      ),
    );
  }
}
