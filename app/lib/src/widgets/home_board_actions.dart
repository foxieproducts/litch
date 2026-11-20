import 'dart:async';

import 'package:flutter/material.dart';

import 'compact_controls.dart';

import '../controllers/board_actions_controller.dart';
import '../localization/litch_strings.dart';
import '../models/litch_control_action.dart';

class HomeBoardActions extends StatefulWidget {
  const HomeBoardActions({
    super.key,
    required this.controller,
    this.clockControls,
  });

  final BoardActionsController controller;
  final Widget? clockControls;

  @override
  State<HomeBoardActions> createState() => _HomeBoardActionsState();
}

class _HomeBoardActionsState extends State<HomeBoardActions> {
  bool _newGameDialogOpen = false;
  BoardActionsController get controller => widget.controller;
  Widget? get clockControls => widget.clockControls;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    controller.useLocalizations(litchStrings(context));
  }

  @override
  void didUpdateWidget(HomeBoardActions oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.controller != controller) {
      controller.useLocalizations(litchStrings(context));
    }
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: controller,
      builder: (BuildContext context, Widget? child) {
        final tone = controller.statusTone;
        final String? status =
            tone == BoardActionStatusTone.warning ||
                tone == BoardActionStatusTone.error
            ? controller.statusMessage
            : null;
        return Card(
          key: const ValueKey<String>('home-board-actions'),
          margin: EdgeInsets.zero,
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                Semantics(
                  header: true,
                  child: Text(
                    litchStrings(context).boardActions,
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                ),
                const SizedBox(height: 12),
                LitchCompactControls(child: _boardActionControls(context)),
                const SizedBox(height: 16),
                Text(
                  litchStrings(context).coaching,
                  style: Theme.of(context).textTheme.labelLarge,
                ),
                const SizedBox(height: 2),
                Text(
                  litchStrings(context).coachingHelp,
                  style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  children: <Widget>[
                    Expanded(child: _coachingControl(LitchControlOwner.p1)),
                    const SizedBox(width: 8),
                    Expanded(child: _coachingControl(LitchControlOwner.p2)),
                  ],
                ),
                if (clockControls case final Widget clockControls) ...<Widget>[
                  const SizedBox(height: 16),
                  const Divider(height: 1),
                  const SizedBox(height: 16),
                  clockControls,
                ],
                if (status != null ||
                    controller.availability ==
                        LitchControlAvailability.unavailable) ...<Widget>[
                  const SizedBox(height: 12),
                  _BoardActionStatus(
                    message: status ?? litchStrings(context).actionsUnavailable,
                    tone: status == null
                        ? BoardActionStatusTone.warning
                        : controller.statusTone,
                    liveRegion: status != null,
                  ),
                ],
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _boardActionControls(BuildContext context) => LayoutBuilder(
    builder: (BuildContext context, BoxConstraints constraints) {
      final double fontSize = Theme.of(context).textTheme.labelLarge!.fontSize!;
      final double textScale =
          MediaQuery.textScalerOf(context).scale(fontSize) / fontSize;
      final double scaledWidth = constraints.maxWidth / textScale;
      final int columns = scaledWidth >= 270
          ? 3
          : scaledWidth >= 180
          ? 2
          : 1;
      final double width = (constraints.maxWidth - (columns - 1) * 8) / columns;
      return Wrap(
        key: const ValueKey('board-action-controls'),
        spacing: 8,
        runSpacing: 8,
        children: <Widget>[
          SizedBox(
            width: width,
            child: _BoardActionButton(
              controlKey: 'control:hint',
              label: controller.hintLabel,
              semanticLabel: controller.hintSemanticLabel,
              progressLabel: controller.hintCalculating
                  ? litchStrings(context).hintThinking
                  : null,
              selected: controller.hintSelected,
              icon: controller.hintRequested == true
                  ? Icons.lightbulb
                  : Icons.lightbulb_outline,
              onPressed: controller.canDispatchAction(LitchControlAction.hint)
                  ? () =>
                        unawaited(controller.dispatch(LitchControlAction.hint))
                  : null,
            ),
          ),
          SizedBox(
            width: width,
            child: _BoardActionButton(
              controlKey: 'control:undo',
              label: controller.undoLabel,
              semanticLabel: controller.undoPending
                  ? litchStrings(context).cancelUndoHelp
                  : litchStrings(context).undoHelp,
              icon: Icons.undo,
              onPressed: controller.canDispatchAction(LitchControlAction.undo)
                  ? () =>
                        unawaited(controller.dispatch(LitchControlAction.undo))
                  : null,
            ),
          ),
          SizedBox(
            width: width,
            child: _BoardActionButton(
              controlKey: 'control:new-game',
              label: litchStrings(context).newGame,
              semanticLabel: litchStrings(context).newGameHelp,
              icon: Icons.restart_alt,
              onPressed:
                  controller.canDispatchAction(LitchControlAction.newGame)
                  ? () => unawaited(_confirmNewGame(context))
                  : null,
            ),
          ),
        ],
      );
    },
  );

  Future<void> _confirmNewGame(BuildContext context) async {
    if (_newGameDialogOpen ||
        controller.isQueuedOrPending(LitchControlAction.newGame)) {
      return;
    }
    _newGameDialogOpen = true;
    final openingController = controller;
    final openingSession = controller.sessionToken;
    try {
      final bool? confirmed = await showDialog<bool>(
        context: context,
        builder: (BuildContext dialogContext) {
          return AlertDialog(
            title: Text(litchStrings(context).newGameQuestion),
            content: Text(litchStrings(context).newGameWarning),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.of(dialogContext).pop(false),
                child: Text(litchStrings(context).cancelAction),
              ),
              FilledButton(
                onPressed: () => Navigator.of(dialogContext).pop(true),
                child: Text(litchStrings(context).startNewGame),
              ),
            ],
          );
        },
      );
      if (!context.mounted ||
          confirmed != true ||
          controller != openingController ||
          controller.sessionToken != openingSession) {
        return;
      }
      await controller.dispatch(LitchControlAction.newGame);
    } finally {
      _newGameDialogOpen = false;
    }
  }

  Widget _coachingControl(LitchControlOwner owner) {
    final bool? savedEnabled = controller.coachingEnabled(owner);
    final String player = owner == LitchControlOwner.p1 ? '1' : '2';
    final String controlKey = 'control:coaching:${owner.name}';
    final String? unavailable = controller.coachingUnavailableReason(owner);
    final bool? enabled =
        savedEnabled != null &&
            unavailable == litchStrings(context).computerPlayer
        ? false
        : savedEnabled;
    final bool canChange = controller.canDispatchAction(
      LitchControlAction.coaching,
      owner: owner,
    );
    return _BoardActionButton(
      controlKey: controlKey,
      label: 'P$player',
      stateLabel: enabled == null
          ? litchStrings(context).unknownLabel
          : enabled
          ? litchStrings(context).onLabel
          : litchStrings(context).offLabel,
      semanticLabel: unavailable != null
          ? litchStrings(context).coachingUnavailableSemantics(
              player,
              enabled == null
                  ? 'unknown'
                  : enabled
                  ? 'on'
                  : 'off',
              unavailable,
            )
          : enabled == null
          ? litchStrings(context).coachingUnknown(player)
          : litchStrings(context).coachingSemantics(player, enabled.toString()),
      selected: enabled,
      showSelection: true,
      icon: enabled == true ? Icons.school : Icons.school_outlined,
      onPressed: canChange && savedEnabled != null
          ? () => unawaited(
              controller.dispatch(
                LitchControlAction.coaching,
                owner: owner,
                enabled: !savedEnabled,
              ),
            )
          : null,
    );
  }
}

class _BoardActionButton extends StatelessWidget {
  const _BoardActionButton({
    required this.controlKey,
    required this.label,
    required this.semanticLabel,
    required this.icon,
    required this.onPressed,
    this.selected,
    this.stateLabel,
    this.progressLabel,
    this.showSelection = false,
  });

  final String controlKey;
  final String label;
  final String semanticLabel;
  final IconData icon;
  final bool? selected;
  final String? stateLabel;
  final String? progressLabel;
  final bool showSelection;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    final bool emphasized =
        showSelection && selected == true && onPressed != null;
    final TextStyle labelStyle = Theme.of(context).textTheme.labelLarge!;
    final TextStyle contentStyle = onPressed == null
        ? labelStyle.copyWith(color: colors.onSurface.withValues(alpha: 0.38))
        : labelStyle;

    final TextPainter twoLineLabel = TextPainter(
      text: TextSpan(text: 'Ag\nAg', style: labelStyle),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
      maxLines: 2,
    )..layout();
    final double height = 46 + twoLineLabel.height;
    twoLineLabel.dispose();
    return Tooltip(
      message: semanticLabel,
      child: Semantics(
        label: semanticLabel,
        value: progressLabel,
        liveRegion: progressLabel != null,
        button: true,
        selected: showSelection ? null : selected,
        toggled: showSelection ? selected : null,
        enabled: onPressed != null,
        onTap: onPressed,
        excludeSemantics: true,
        child: ConstrainedBox(
          constraints: BoxConstraints(
            minHeight: height,
            minWidth: double.infinity,
          ),
          child: OutlinedButton(
            key: ValueKey<String>(controlKey),
            onPressed: onPressed,
            style: OutlinedButton.styleFrom(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 10),
              backgroundColor: emphasized ? colors.primaryContainer : null,
              foregroundColor: emphasized ? colors.onPrimaryContainer : null,
              side: emphasized ? BorderSide(color: colors.primary) : null,
            ),
            child: Stack(
              clipBehavior: Clip.none,
              alignment: Alignment.center,
              children: <Widget>[
                Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Icon(icon, size: 22),
                    const SizedBox(height: 4),
                    Text(
                      label,
                      style: contentStyle,
                      textAlign: TextAlign.center,
                    ),
                    if (stateLabel case final String stateLabel)
                      Stack(
                        alignment: Alignment.center,
                        children: [
                          // Reserve all localized states so hydration cannot
                          // change the button height at larger text sizes.
                          for (final label in [
                            litchStrings(context).onLabel,
                            litchStrings(context).offLabel,
                            litchStrings(context).unknownLabel,
                          ])
                            Opacity(
                              opacity: 0,
                              child: RichText(
                                textAlign: TextAlign.center,
                                textScaler: MediaQuery.textScalerOf(context),
                                text: TextSpan(
                                  text: label,
                                  style: DefaultTextStyle.of(
                                    context,
                                  ).style.merge(contentStyle),
                                ),
                              ),
                            ),
                          Text(
                            stateLabel,
                            style: contentStyle,
                            textAlign: TextAlign.center,
                          ),
                        ],
                      ),
                  ],
                ),
                // The 1px gap and 8px ring fit the original 10px bottom padding,
                // including when the label wraps and uses the full button height.
                if (progressLabel != null)
                  Positioned(
                    left: 0,
                    right: 0,
                    bottom: -9,
                    child: Center(
                      child: SizedBox.square(
                        dimension: 8,
                        child: CircularProgressIndicator(
                          strokeWidth: 1.5,
                          value: MediaQuery.disableAnimationsOf(context)
                              ? 0.75
                              : null,
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _BoardActionStatus extends StatelessWidget {
  const _BoardActionStatus({
    required this.message,
    required this.tone,
    required this.liveRegion,
  });

  final String message;
  final BoardActionStatusTone tone;
  final bool liveRegion;

  @override
  Widget build(BuildContext context) {
    final IconData icon = switch (tone) {
      BoardActionStatusTone.progress => Icons.hourglass_top,
      BoardActionStatusTone.success => Icons.check_circle_outline,
      BoardActionStatusTone.info => Icons.info_outline,
      BoardActionStatusTone.warning => Icons.block_outlined,
      BoardActionStatusTone.error => Icons.report_gmailerrorred_outlined,
    };
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
