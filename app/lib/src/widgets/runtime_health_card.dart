import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/home_board_snapshot.dart';
import '../models/board_storage_recovery.dart';
import '../models/runtime_health_snapshot.dart';
import '../models/support_snapshot.dart';

class RuntimeHealthCard extends StatefulWidget {
  const RuntimeHealthCard({
    super.key,
    required this.result,
    required this.refreshing,
    required this.onRetry,
    this.recoveryOnly = false,
    this.recoveryHealth,
    this.supportsRecheck = false,
    this.rechecking = false,
    this.onRecheck,
    this.recheckMessage,
    this.persistence,
    this.watchdogRestart = false,
    this.persistenceSessionOnly = false,
    this.storageRecovery,
    this.reformattingStorage = false,
    this.checkingStorageRecovery = false,
    this.onReformatStorage,
    this.onRefreshStorageRecovery,
  });

  final RuntimeHealthResult? result;
  final bool refreshing;
  final VoidCallback? onRetry;
  final bool recoveryOnly;
  final SupportPublicHealth? recoveryHealth;
  final bool supportsRecheck;
  final bool rechecking;
  final VoidCallback? onRecheck;
  final String? recheckMessage;
  final HomePersistenceDegradation? persistence;
  final bool watchdogRestart;
  final bool persistenceSessionOnly;
  final BoardStorageRecoverySnapshot? storageRecovery;
  final bool reformattingStorage;
  final bool checkingStorageRecovery;
  final VoidCallback? onReformatStorage;
  final VoidCallback? onRefreshStorageRecovery;

  @override
  State<RuntimeHealthCard> createState() => _RuntimeHealthCardState();
}

class _RuntimeHealthCardState extends State<RuntimeHealthCard> {
  late bool _expanded =
      widget.recoveryOnly ||
      widget.persistence != null ||
      widget.persistenceSessionOnly ||
      widget.watchdogRestart ||
      _defaultExpanded(widget.result);
  bool _userSetExpansion = false;

  @override
  void didUpdateWidget(covariant RuntimeHealthCard oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.recoveryOnly && !oldWidget.recoveryOnly) {
      _expanded = true;
      _userSetExpansion = false;
    }
    if (!_userSetExpansion) {
      final bool hasIncident =
          widget.recoveryOnly ||
          widget.persistence != null ||
          widget.persistenceSessionOnly ||
          widget.watchdogRestart;
      if (widget.result == null || oldWidget.result == null) {
        _expanded = hasIncident || _defaultExpanded(widget.result);
      } else if (hasIncident || _defaultExpanded(widget.result)) {
        _expanded = true;
      }
    }
  }

  void _toggleExpanded() {
    setState(() {
      _expanded = !_expanded;
      _userSetExpansion = true;
    });
  }

  @override
  Widget build(BuildContext context) {
    final RuntimeHealthSnapshot? snapshot = switch (widget.result) {
      RuntimeHealthAvailable(:final snapshot) => snapshot,
      _ => null,
    };
    final _HealthPresentation sensorPresentation = snapshot == null
        ? _unavailablePresentation(widget.result)
        : _snapshotPresentation(snapshot);
    final List<String> incidents = <String>[
      if (widget.persistence != null)
        localizedPersistence(
          litchStrings(context),
          widget.persistence!,
          sessionOnly: widget.persistenceSessionOnly,
        ),
      if (widget.persistence != HomePersistenceDegradation.storageIo &&
          widget.persistenceSessionOnly)
        litchStrings(context).savingSessionOnly,
      if (widget.watchdogRestart) litchStrings(context).healthRestartGuidance,
    ];
    final String? incident = incidents.isEmpty ? null : incidents.join('\n\n');
    final bool paused = snapshot?.state == RuntimeHealthState.inputPaused;
    final bool retainSensorTitle =
        paused ||
        (widget.persistence != HomePersistenceDegradation.storageIo &&
            snapshot != null &&
            snapshot.state != RuntimeHealthState.healthy &&
            snapshot.state != RuntimeHealthState.unknown);
    final _HealthPresentation presentation = widget.recoveryOnly
        ? _HealthPresentation(
            icon: Icons.build_circle_outlined,
            title: litchStrings(context).boardRecoveryTitle,
            message: <String>[
              litchStrings(context).boardRecoveryReason(
                widget.recoveryHealth?.recoveryReason ?? 'other',
              ),
              if (!widget.refreshing && widget.recoveryHealth == null)
                litchStrings(context).boardRecoveryDetailsUnavailable,
              litchStrings(context).boardRecoveryNextAction,
            ].join('\n\n'),
          )
        : incident == null
        ? sensorPresentation
        : _HealthPresentation(
            icon: retainSensorTitle
                ? sensorPresentation.icon
                : Icons.info_outline,
            title: retainSensorTitle
                ? sensorPresentation.title
                : widget.persistence == HomePersistenceDegradation.storageIo
                ? litchStrings(context).savingAttention
                : widget.persistenceSessionOnly
                ? litchStrings(context).savingUnsaved
                : widget.persistence ==
                      HomePersistenceDegradation.quarantinedRecord
                ? litchStrings(context).savingGameRecovery
                : litchStrings(context).boardRestarted,
            message: litchStrings(
              context,
            ).healthWithIncident(incident, sensorPresentation.message),
          );
    final bool showRecheck =
        !widget.recoveryOnly &&
        snapshot != null &&
        widget.supportsRecheck &&
        _hasLatchedPowerCause(snapshot);
    final RuntimeHealthUnavailableReason? unavailableReason =
        switch (widget.result) {
          RuntimeHealthUnavailable(:final reason) => reason,
          _ => null,
        };
    final bool showRetry =
        !widget.recoveryOnly &&
        !widget.refreshing &&
        (unavailableReason == RuntimeHealthUnavailableReason.timedOut ||
            unavailableReason == RuntimeHealthUnavailableReason.sendFailed ||
            unavailableReason == RuntimeHealthUnavailableReason.malformed);
    final Color accent = widget.recoveryOnly
        ? Theme.of(context).colorScheme.error
        : _accentColor(context, snapshot, unavailableReason);

    return Semantics(
      container: true,
      liveRegion:
          widget.recoveryOnly ||
          incident != null ||
          (snapshot?.isAbnormal ?? false),
      label: litchStrings(context).boardHealthTitle,
      child: Card(
        key: const ValueKey<String>('runtime-health-card'),
        margin: EdgeInsets.zero,
        clipBehavior: Clip.antiAlias,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(14),
          side: BorderSide(color: accent.withValues(alpha: 0.32)),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Semantics(
              button: true,
              expanded: _expanded,
              label: litchStrings(
                context,
              ).healthToggleSemantics(presentation.title, _expanded.toString()),
              child: InkWell(
                key: const ValueKey<String>('runtime-health-toggle'),
                onTap: _toggleExpanded,
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(16, 14, 8, 14),
                  child: Row(
                    children: <Widget>[
                      Icon(
                        presentation.icon,
                        color: accent,
                        semanticLabel: presentation.title,
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            Text(
                              litchStrings(context).boardHealthTitle,
                              style: Theme.of(context).textTheme.titleMedium,
                            ),
                            const SizedBox(height: 2),
                            Text(
                              presentation.title,
                              maxLines: 2,
                              overflow: TextOverflow.ellipsis,
                              style: Theme.of(
                                context,
                              ).textTheme.bodySmall?.copyWith(color: accent),
                            ),
                          ],
                        ),
                      ),
                      if (widget.refreshing)
                        const Padding(
                          padding: EdgeInsets.only(right: 8),
                          child: SizedBox.square(
                            dimension: 18,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          ),
                        ),
                      Icon(
                        _expanded
                            ? Icons.expand_less_rounded
                            : Icons.expand_more_rounded,
                        semanticLabel: _expanded
                            ? litchStrings(context).collapseAction
                            : litchStrings(context).expandAction,
                      ),
                    ],
                  ),
                ),
              ),
            ),
            if (_expanded)
              Padding(
                key: const ValueKey<String>('runtime-health-details'),
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Divider(color: accent.withValues(alpha: 0.22)),
                    const SizedBox(height: 8),
                    Text(presentation.message),
                    if (!widget.recoveryOnly && snapshot != null) ...<Widget>[
                      const SizedBox(height: 8),
                      Text(
                        litchStrings(context).supportCode(snapshot.supportCode),
                        style: Theme.of(context).textTheme.labelMedium,
                      ),
                    ],
                    if (!widget.recoveryOnly &&
                        widget.recheckMessage != null) ...<Widget>[
                      const SizedBox(height: 8),
                      Semantics(
                        liveRegion: true,
                        child: Text(widget.recheckMessage!),
                      ),
                    ],
                    if (widget.storageRecovery case final recovery?
                        when !widget.recoveryOnly &&
                            recovery.status !=
                                BoardStorageRecoveryStatus.unavailable)
                      _buildStorageRecovery(recovery),
                    const SizedBox(height: 12),
                    Wrap(
                      spacing: 10,
                      runSpacing: 10,
                      children: <Widget>[
                        if (showRecheck)
                          FilledButton.icon(
                            key: const ValueKey<String>(
                              'runtime-health-recheck',
                            ),
                            onPressed: widget.rechecking
                                ? null
                                : widget.onRecheck,
                            icon: widget.rechecking
                                ? const SizedBox.square(
                                    dimension: 18,
                                    child: CircularProgressIndicator(
                                      strokeWidth: 2,
                                    ),
                                  )
                                : const Icon(Icons.usb_outlined),
                            label: Text(litchStrings(context).checkPowerAgain),
                          ),
                        if (showRetry)
                          OutlinedButton.icon(
                            key: const ValueKey<String>('runtime-health-retry'),
                            onPressed: widget.onRetry,
                            icon: const Icon(Icons.sync),
                            label: Text(litchStrings(context).tryAgain),
                          ),
                      ],
                    ),
                  ],
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildStorageRecovery(BoardStorageRecoverySnapshot recovery) {
    final strings = litchStrings(context);
    final inProgress = widget.reformattingStorage || recovery.isInProgress;
    final message = switch (recovery.status) {
      BoardStorageRecoveryStatus.idle => null,
      BoardStorageRecoveryStatus.busy => strings.storageReformatBusy,
      BoardStorageRecoveryStatus.pending ||
      BoardStorageRecoveryStatus.running => strings.storageReformatProgress,
      BoardStorageRecoveryStatus.succeeded => strings.storageReformatSucceeded,
      BoardStorageRecoveryStatus.failed => strings.storageReformatFailed,
      BoardStorageRecoveryStatus.unknown => strings.storageReformatUnknown,
      BoardStorageRecoveryStatus.unavailable => null,
    };
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        if (message != null) ...<Widget>[
          const SizedBox(height: 12),
          Semantics(
            liveRegion: true,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                if (inProgress)
                  const Padding(
                    padding: EdgeInsets.only(top: 2, right: 10),
                    child: SizedBox.square(
                      dimension: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    ),
                  ),
                Expanded(child: Text(message)),
              ],
            ),
          ),
        ],
        if (recovery.canReformat ||
            recovery.status == BoardStorageRecoveryStatus.busy) ...<Widget>[
          const SizedBox(height: 12),
          OutlinedButton.icon(
            key: const ValueKey<String>('storage-reformat-open'),
            style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
            onPressed: !inProgress && recovery.canReformat
                ? widget.onReformatStorage
                : null,
            icon: const Icon(Icons.storage_outlined),
            label: Text(strings.storageReformatAction),
          ),
        ],
        if (recovery.status == BoardStorageRecoveryStatus.unknown ||
            recovery.status == BoardStorageRecoveryStatus.busy) ...<Widget>[
          const SizedBox(height: 12),
          OutlinedButton.icon(
            key: const ValueKey<String>('storage-reformat-status'),
            style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
            onPressed: widget.checkingStorageRecovery
                ? null
                : widget.onRefreshStorageRecovery,
            icon: const Icon(Icons.sync),
            label: Text(strings.storageReformatCheckStatus),
          ),
        ],
      ],
    );
  }

  bool _defaultExpanded(RuntimeHealthResult? result) {
    return switch (result) {
      RuntimeHealthAvailable(:final snapshot) => snapshot.isAbnormal,
      RuntimeHealthUnavailable() => true,
      null => false,
    };
  }

  Color _accentColor(
    BuildContext context,
    RuntimeHealthSnapshot? snapshot,
    RuntimeHealthUnavailableReason? unavailableReason,
  ) {
    final ColorScheme colors = Theme.of(context).colorScheme;
    if (snapshot == null) {
      return unavailableReason == null ? colors.primary : colors.secondary;
    }
    return switch (snapshot.state) {
      RuntimeHealthState.healthy => colors.tertiary,
      RuntimeHealthState.degradedUsable => colors.secondary,
      RuntimeHealthState.inputPaused ||
      RuntimeHealthState.protectionLatched => colors.error,
      RuntimeHealthState.unknown => colors.primary,
    };
  }

  _HealthPresentation _snapshotPresentation(RuntimeHealthSnapshot snapshot) {
    final String causes = _causeText(snapshot);
    if (_hasPowerCause(snapshot) &&
        (snapshot.state == RuntimeHealthState.degradedUsable ||
            snapshot.state == RuntimeHealthState.protectionLatched)) {
      return _HealthPresentation(
        icon: Icons.power_outlined,
        title: litchStrings(context).powerLimitedTitle,
        message: litchStrings(context).powerLimitedHelp(
          _hasLatchedPowerCause(snapshot)
              ? litchStrings(context).powerProtectionRemains
              : '',
          causes,
        ),
      );
    }
    return switch (snapshot.state) {
      RuntimeHealthState.healthy => _HealthPresentation(
        icon: Icons.check_circle_outline,
        title: litchStrings(context).healthyLabel,
        message: litchStrings(context).healthyMessage,
      ),
      RuntimeHealthState.degradedUsable => _HealthPresentation(
        icon: Icons.info_outline,
        title: litchStrings(context).degradedTitle,
        message: causes.isEmpty
            ? litchStrings(context).degradedMessage
            : litchStrings(context).degradedCauses(causes),
      ),
      RuntimeHealthState.inputPaused => _HealthPresentation(
        icon: Icons.pause_circle_outline,
        title: litchStrings(context).inputPausedTitle,
        message: causes.isEmpty
            ? litchStrings(context).inputPausedHelp
            : litchStrings(context).inputPausedCauses(causes),
      ),
      RuntimeHealthState.protectionLatched => _HealthPresentation(
        icon: Icons.shield_outlined,
        title: litchStrings(context).protectionTitle,
        message: causes.isEmpty
            ? litchStrings(context).protectionMessage
            : litchStrings(context).protectionCauses(causes),
      ),
      RuntimeHealthState.unknown => _HealthPresentation(
        icon: Icons.help_outline,
        title: litchStrings(context).unknownHealthTitle,
        message: litchStrings(context).unknownHealthMessage,
      ),
    };
  }

  bool _hasPowerCause(RuntimeHealthSnapshot snapshot) {
    return snapshot.knownCauses.any(
      (RuntimeHealthCause cause) =>
          cause == RuntimeHealthCause.sourceUnstable ||
          cause == RuntimeHealthCause.brownoutLatched ||
          cause == RuntimeHealthCause.severeProtectionLatched,
    );
  }

  bool _hasLatchedPowerCause(RuntimeHealthSnapshot snapshot) {
    return snapshot.knownCauses.any(
      (RuntimeHealthCause cause) =>
          cause == RuntimeHealthCause.brownoutLatched ||
          cause == RuntimeHealthCause.severeProtectionLatched,
    );
  }

  String _causeText(RuntimeHealthSnapshot snapshot) {
    final List<String> labels = snapshot.knownCauses
        .map(
          (RuntimeHealthCause cause) => switch (cause) {
            RuntimeHealthCause.oneAmbientSensor => litchStrings(
              context,
            ).oneAmbientMissing,
            RuntimeHealthCause.noAmbientSensors => litchStrings(
              context,
            ).allAmbientMissing,
            RuntimeHealthCause.boardScanUncertain => litchStrings(
              context,
            ).boardInputUncertain,
            RuntimeHealthCause.sourceUnstable => litchStrings(
              context,
            ).powerUnstable,
            RuntimeHealthCause.brownoutLatched => litchStrings(
              context,
            ).brownoutDetected,
            RuntimeHealthCause.severeProtectionLatched => litchStrings(
              context,
            ).powerProtectionLatched,
          },
        )
        .toList(growable: true);
    if (snapshot.unknownCauseMask != 0) {
      labels.add(litchStrings(context).unknownHealthCause);
    }
    return labels.join(', ');
  }

  _HealthPresentation _unavailablePresentation(RuntimeHealthResult? result) {
    final RuntimeHealthUnavailableReason? reason = switch (result) {
      RuntimeHealthUnavailable(:final reason) => reason,
      _ => null,
    };
    final String message = switch (reason) {
      RuntimeHealthUnavailableReason.unsupported => litchStrings(
        context,
      ).healthUnsupported,
      RuntimeHealthUnavailableReason.unknownCommand => litchStrings(
        context,
      ).healthUnknownRequest,
      RuntimeHealthUnavailableReason.malformed => litchStrings(
        context,
      ).healthUnreadable,
      RuntimeHealthUnavailableReason.timedOut => litchStrings(
        context,
      ).healthNoReply,
      RuntimeHealthUnavailableReason.disconnected => litchStrings(
        context,
      ).healthConnect,
      RuntimeHealthUnavailableReason.sendFailed => litchStrings(
        context,
      ).healthSendFailed,
      null => litchStrings(context).healthCheckingMessage,
    };
    return _HealthPresentation(
      icon: reason == null ? Icons.hourglass_top_outlined : Icons.info_outline,
      title: reason == null
          ? litchStrings(context).healthCheckingTitle
          : litchStrings(context).healthUnavailableTitle,
      message: message,
    );
  }
}

class _HealthPresentation {
  const _HealthPresentation({
    required this.icon,
    required this.title,
    required this.message,
  });

  final IconData icon;
  final String title;
  final String message;
}
