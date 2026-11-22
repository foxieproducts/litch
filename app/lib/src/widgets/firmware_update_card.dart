import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';

class FirmwareUpdateCard extends StatelessWidget {
  const FirmwareUpdateCard({
    super.key,
    required this.allowBeta,
    required this.channelChangeDisabled,
    required this.updateDisabled,
    required this.supportsFirmwareUpdate,
    this.updateGuidance,
    required this.boardVersion,
    required this.latestVersion,
    required this.downloading,
    required this.downloadProgress,
    required this.uploading,
    required this.uploadProgress,
    this.transferInProgress = false,
    this.estimatedRemaining,
    this.progressKey,
    required this.verifying,
    required this.status,
    this.statusInProgress = false,
    this.statusAnnouncement,
    required this.onAllowBetaChanged,
    required this.onUpdate,
  });

  final bool allowBeta;
  final bool channelChangeDisabled;
  final bool updateDisabled;
  final bool supportsFirmwareUpdate;
  final String? updateGuidance;
  final String? boardVersion;
  final String? latestVersion;
  final bool downloading;
  final double downloadProgress;
  final bool uploading;
  final double uploadProgress;
  final bool transferInProgress;
  final Duration? estimatedRemaining;
  final Key? progressKey;
  final bool verifying;
  final String status;
  final bool statusInProgress;
  final String? statusAnnouncement;
  final ValueChanged<bool> onAllowBetaChanged;
  final VoidCallback onUpdate;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final remaining = estimatedRemaining;
    final remainingLabel = remaining == null
        ? null
        : '${(remaining.inSeconds ~/ 60).toString().padLeft(2, '0')}:'
              '${(remaining.inSeconds % 60).toString().padLeft(2, '0')}';
    final transferring =
        transferInProgress && uploading && !verifying && !statusInProgress;
    final percent = (uploadProgress.clamp(0, .99) * 100).floor().toString();
    final int? progressMilestone = _coarseProgressMilestone(
      downloading ? downloadProgress : uploadProgress,
    );
    final String? progressAnnouncement = downloading
        ? progressMilestone == null
              ? litchStrings(context).downloadingUpdate
              : litchStrings(
                  context,
                ).downloadingPercent(progressMilestone.toString())
        : uploading
        ? progressMilestone == null
              ? litchStrings(context).installingUpdate
              : litchStrings(
                  context,
                ).installingPercent(progressMilestone.toString())
        : null;
    final String statusSemanticsLabel =
        statusAnnouncement ??
        (verifying || statusInProgress
            ? status
            : progressAnnouncement ?? status);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              title: Text(litchStrings(context).allowBeta),
              value: allowBeta,
              onChanged: channelChangeDisabled
                  ? null
                  : (bool? value) => onAllowBetaChanged(value ?? false),
            ),
            if (boardVersion != null || latestVersion != null) ...<Widget>[
              const SizedBox(height: 6),
              Text(
                litchStrings(context).boardVersionLabel(
                  boardVersion ??
                      litchStrings(context).unknownLabel.toLowerCase(),
                ),
              ),
              const SizedBox(height: 4),
              Text(
                litchStrings(context).latestVersionLabel(
                  latestVersion ??
                      litchStrings(context).unknownLabel.toLowerCase(),
                ),
              ),
            ],
            const SizedBox(height: 12),
            if (supportsFirmwareUpdate)
              FilledButton(
                onPressed: updateDisabled ? null : onUpdate,
                child: Text(
                  boardVersion != null && boardVersion == latestVersion
                      ? litchStrings(context).reinstallBoardAction
                      : litchStrings(context).updateBoardAction,
                ),
              )
            else if (updateGuidance case final guidance?)
              Text(guidance),
            Column(
              key: progressKey,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Semantics(
                  key: const ValueKey<String>(
                    'firmware-update-status-live-region',
                  ),
                  container: true,
                  liveRegion: true,
                  label: statusSemanticsLabel,
                  excludeSemantics: true,
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      const SizedBox(height: 12),
                      if (statusInProgress) const LinearProgressIndicator(),
                      if (!statusInProgress && downloading)
                        LinearProgressIndicator(
                          value: downloadProgress > 0 ? downloadProgress : null,
                        ),
                      if (!statusInProgress && downloading) ...<Widget>[
                        const SizedBox(height: 6),
                        Text(
                          strings.downloadPercent(
                            (100 * downloadProgress).round().toString(),
                          ),
                        ),
                      ],
                      if (!statusInProgress && uploading) ...<Widget>[
                        const SizedBox(height: 8),
                        LinearProgressIndicator(
                          value: uploadProgress > 0 ? uploadProgress : null,
                        ),
                      ],
                      const SizedBox(height: 8),
                      if (!transferring && statusAnnouncement == null)
                        Text(status),
                    ],
                  ),
                ),
                if (!transferring && statusAnnouncement != null)
                  Semantics(
                    key: const ValueKey<String>(
                      'firmware-activity-exact-status',
                    ),
                    container: true,
                    liveRegion: false,
                    label: status,
                    excludeSemantics: true,
                    child: Text(status),
                  ),
                if (transferring)
                  Semantics(
                    key: const ValueKey<String>(
                      'firmware-transfer-exact-progress',
                    ),
                    container: true,
                    liveRegion: false,
                    label: remainingLabel == null
                        ? strings.installingPercentComplete(percent)
                        : '${strings.installingPercentComplete(percent)}. '
                              '${strings.firmwareEstimatedRemaining(remainingLabel)}',
                    excludeSemantics: true,
                    child: Text(
                      remainingLabel == null
                          ? strings.installingPercentComplete(percent)
                          : strings.installingPercentRemaining(
                              percent,
                              remainingLabel,
                            ),
                    ),
                  ),
                if (transferring) ...<Widget>[
                  const SizedBox(height: 12),
                  Semantics(
                    key: const ValueKey<String>(
                      'firmware-transfer-open-warning',
                    ),
                    container: true,
                    liveRegion: true,
                    label: strings.keepAppOpenWhileUpdating,
                    excludeSemantics: true,
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: <Widget>[
                        Icon(
                          Icons.info_outline,
                          color: Theme.of(context).colorScheme.secondary,
                          size: 20,
                        ),
                        const SizedBox(width: 8),
                        Expanded(
                          child: Text(
                            strings.keepAppOpenWhileUpdating,
                            style: Theme.of(context).textTheme.bodyMedium
                                ?.copyWith(
                                  color: Theme.of(
                                    context,
                                  ).colorScheme.secondary,
                                  fontWeight: FontWeight.w700,
                                ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ],
            ),
          ],
        ),
      ),
    );
  }
}

int? _coarseProgressMilestone(double progress) {
  final int percent = (progress.clamp(0, 1) * 100).floor();
  if (percent >= 100) {
    return 100;
  }
  if (percent >= 75) {
    return 75;
  }
  if (percent >= 50) {
    return 50;
  }
  if (percent >= 25) {
    return 25;
  }
  return null;
}
