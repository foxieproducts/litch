import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';
import '../widgets/litch_workspace_geometry.dart';

import '../localization/litch_strings.dart';
import '../services/bluetooth_pairing_recovery.dart';

class _BluetoothConnectionRecoverySteps extends StatelessWidget {
  const _BluetoothConnectionRecoverySteps();

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          defaultTargetPlatform == TargetPlatform.iOS
              ? strings.pairingRecoveryIosSteps
              : strings.pairingRecoveryAndroidSteps,
          key: const ValueKey('phone-pairing-recovery-steps'),
        ),
        const SizedBox(height: 20),
        Text(
          strings.ownerVerificationRecoverySteps,
          key: const ValueKey('owner-verification-recovery-steps'),
        ),
      ],
    );
  }
}

/// The same optional manual procedure remains reachable before a connection.
class BluetoothConnectionHelpScreen extends StatelessWidget {
  const BluetoothConnectionHelpScreen({
    super.key,
    required this.boardName,
    required this.support,
  });

  final String boardName;
  final Widget support;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return Scaffold(
      key: const ValueKey('bluetooth-connection-help'),
      appBar: LitchCompactAppBar(
        child: AppBar(
          toolbarHeight: LitchCompactControls.toolbarHeightOf(context),
          title: Text(strings.findBoardHelp, softWrap: true, maxLines: 2),
        ),
      ),
      body: SafeArea(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: Align(
            alignment: Alignment.topCenter,
            child: ConstrainedBox(
              constraints: const BoxConstraints(
                maxWidth: LitchWorkspaceGeometry.readableContentWidth,
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(
                    boardName,
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                  const SizedBox(height: 20),
                  Text(strings.connectionHelpBasics),
                  const SizedBox(height: 24),
                  const _BluetoothConnectionRecoverySteps(),
                  const SizedBox(height: 24),
                  support,
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class BluetoothPairingRecoveryScreen extends StatefulWidget {
  const BluetoothPairingRecoveryScreen({
    super.key,
    required this.state,
    required this.boardName,
    required this.onRepair,
    required this.onRetry,
    required this.onOpenSettings,
    required this.onExit,
    this.support,
  });

  final BluetoothPairingRecoveryState state;
  final String boardName;
  final Future<bool> Function() onRepair;
  final Future<bool> Function() onRetry;
  final Future<bool> Function() onOpenSettings;
  final VoidCallback onExit;
  final Widget? support;

  @override
  State<BluetoothPairingRecoveryScreen> createState() =>
      _BluetoothPairingRecoveryScreenState();
}

class _BluetoothPairingRecoveryScreenState
    extends State<BluetoothPairingRecoveryScreen> {
  bool _manual = false;

  Future<void> _openSettings() async {
    final opened = await widget.onOpenSettings();
    if (!mounted) return;
    setState(() => _manual = true);
    if (!opened) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(litchStrings(context).pairingSettingsUnavailable),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final state = widget.state;
    final verificationFailed =
        state.reason == BluetoothPairingRecoveryReason.ownerVerificationFailed;
    final freshBondFailed =
        state.reason == BluetoothPairingRecoveryReason.freshBondFailed;
    final savedEncryptionKeyMissing =
        state.reason ==
        BluetoothPairingRecoveryReason.savedEncryptionKeyMissing;
    final retryOnly = verificationFailed || freshBondFailed;
    final ios = defaultTargetPlatform == TargetPlatform.iOS;
    final automatic = !ios && state.canRepair && !_manual;
    final settingsLabel = strings.openBluetoothSettings;
    final primaryLabel = retryOnly || ios
        ? strings.tryAgain
        : automatic
        ? strings.pairingRepairAction
        : _manual
        ? strings.tryAgain
        : settingsLabel;
    final secondaryLabel = !automatic && !_manual
        ? strings.tryAgain
        : settingsLabel;
    Future<void> primary() async {
      if (retryOnly) {
        await widget.onRetry();
      } else if (automatic) {
        await widget.onRepair();
      } else if (ios || _manual) {
        await widget.onRetry();
      } else {
        await _openSettings();
      }
    }

    Future<void> secondary() async {
      if (!automatic && !_manual) {
        await widget.onRetry();
      } else {
        await _openSettings();
      }
    }

    final tabletWindow = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    final actions = <Widget>[
      FilledButton(
        key: const ValueKey('pairing-recovery-primary'),
        style: FilledButton.styleFrom(minimumSize: const Size(48, 48)),
        onPressed: state.busy ? null : () => unawaited(primary()),
        child: Text(primaryLabel),
      ),
      if (!ios && !retryOnly)
        OutlinedButton(
          key: const ValueKey('pairing-recovery-secondary'),
          style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
          onPressed: state.busy ? null : () => unawaited(secondary()),
          child: Text(secondaryLabel),
        ),
      if (retryOnly) ...[
        TextButton(
          key: const ValueKey('pairing-recovery-cancel'),
          style: TextButton.styleFrom(minimumSize: const Size(48, 48)),
          onPressed: widget.onExit,
          child: Text(strings.cancelAction),
        ),
        if (widget.support != null) widget.support!,
      ],
    ];
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, result) {
        if (!didPop) widget.onExit();
      },
      child: Scaffold(
        key: const ValueKey('bluetooth-pairing-recovery'),
        appBar: LitchCompactAppBar(
          child: AppBar(
            toolbarHeight: LitchCompactControls.toolbarHeightOf(context),
            leading: BackButton(onPressed: widget.onExit),
            title: Text(
              strings.pairingRecoveryTitle,
              softWrap: true,
              maxLines: 2,
            ),
          ),
        ),
        body: SafeArea(
          child: Align(
            alignment: tabletWindow ? Alignment.topCenter : Alignment.center,
            child: SingleChildScrollView(
              padding: const EdgeInsets.all(24),
              child: ConstrainedBox(
                constraints: BoxConstraints(
                  maxWidth: tabletWindow
                      ? LitchWorkspaceGeometry.readableContentWidth
                      : 420,
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Text(
                      widget.boardName,
                      style: Theme.of(context).textTheme.headlineSmall,
                    ),
                    const SizedBox(height: 20),
                    Text(
                      savedEncryptionKeyMissing
                          ? strings.savedEncryptionKeyMissingHelp
                          : freshBondFailed
                          ? strings.freshPairingDidNotFinish
                          : verificationFailed
                          ? strings.ownerVerificationRecoveryHelp
                          : strings.pairingRecoveryHelp,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 20),
                    if (!retryOnly && automatic)
                      Text(strings.pairingRecoveryAutomaticHelp),
                    const SizedBox(height: 24),
                    if (state.busy) ...[
                      const Center(child: CircularProgressIndicator()),
                      const SizedBox(height: 16),
                      Semantics(
                        liveRegion: true,
                        child: Text(
                          state.stage == BluetoothPairingRecoveryStage.repairing
                              ? strings.pairingRepairing
                              : strings.boardConnecting,
                          textAlign: TextAlign.center,
                        ),
                      ),
                      const SizedBox(height: 24),
                    ],
                    if (state.failed) ...[
                      Semantics(
                        liveRegion: true,
                        child: Text(
                          retryOnly
                              ? strings.ownerVerificationRecoveryRetryFailed
                              : strings.pairingRepairFailed,
                        ),
                      ),
                      const SizedBox(height: 20),
                    ],
                    if (tabletWindow)
                      Wrap(spacing: 8, runSpacing: 8, children: actions)
                    else
                      Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        spacing: 8,
                        children: actions,
                      ),
                    if (!savedEncryptionKeyMissing && !freshBondFailed) ...[
                      const SizedBox(height: 24),
                      const _BluetoothConnectionRecoverySteps(),
                    ],
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
