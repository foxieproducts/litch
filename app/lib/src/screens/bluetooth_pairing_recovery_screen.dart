import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';
import '../widgets/litch_workspace_geometry.dart';

import '../localization/litch_strings.dart';
import '../services/bluetooth_pairing_recovery.dart';

class BluetoothPairingRecoveryScreen extends StatefulWidget {
  const BluetoothPairingRecoveryScreen({
    super.key,
    required this.state,
    required this.boardName,
    required this.onRepair,
    required this.onRetry,
    required this.onOpenSettings,
    required this.onExit,
  });

  final BluetoothPairingRecoveryState state;
  final String boardName;
  final Future<bool> Function() onRepair;
  final Future<bool> Function() onRetry;
  final Future<bool> Function() onOpenSettings;
  final VoidCallback onExit;

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
    final ios = defaultTargetPlatform == TargetPlatform.iOS;
    final automatic = !ios && state.canRepair && !_manual;
    final settingsLabel = strings.openBluetoothSettings;
    final primaryLabel = ios
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
      if (automatic) {
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
      if (!ios)
        OutlinedButton(
          key: const ValueKey('pairing-recovery-secondary'),
          style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
          onPressed: state.busy ? null : () => unawaited(secondary()),
          child: Text(secondaryLabel),
        ),
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
                      strings.pairingRecoveryHelp,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 20),
                    Text(
                      ios
                          ? strings.pairingRecoveryIosSteps
                          : automatic
                          ? strings.pairingRecoveryAutomaticHelp
                          : strings.pairingRecoveryAndroidSteps,
                    ),
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
                        child: Text(strings.pairingRepairFailed),
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
