import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../constants/firmware_release.dart';
import '../localization/litch_strings.dart';
import '../models/firmware_release_snapshot.dart';
import '../localization/presentation_strings.dart';
import 'release_notes_dialog.dart';
import 'app_upgrade_notice_prompt.dart';

/// Presents a verified offer once per installation, after the shell is ready.
class FirmwareUpdatePrompt extends StatefulWidget {
  const FirmwareUpdatePrompt({
    super.key,
    required this.availability,
    required this.eligible,
    required this.showingUpdate,
    required this.onReviewUpdate,
    required this.child,
    this.canPresent,
  });

  final FirmwareUpdateAvailability availability;
  final bool eligible;
  final bool showingUpdate;
  final VoidCallback onReviewUpdate;
  final bool Function()? canPresent;
  final Widget child;

  @override
  State<FirmwareUpdatePrompt> createState() => _FirmwareUpdatePromptState();
}

class _FirmwareUpdatePromptState extends State<FirmwareUpdatePrompt>
    with WidgetsBindingObserver {
  static const _seenVersionsKey = 'firmware_update_seen_versions';
  static const _suppressFutureKey = 'firmware_update_notices_disabled';
  SharedPreferences? _preferences;
  final Set<String> _seenVersions = <String>{};
  bool _suppressFuture = false;
  bool _scheduled = false;
  bool _foreground = true;
  int _activePointers = 0;
  DialogRoute<bool>? _dialogRoute;
  FirmwareUpdateAvailability? _presentedOffer;

  bool _sameBoard(
    FirmwareUpdateAvailability left,
    FirmwareUpdateAvailability right,
  ) =>
      left.boardId == right.boardId &&
      left.connectionToken == right.connectionToken;

  bool _sameOffer(
    FirmwareUpdateAvailability left,
    FirmwareUpdateAvailability right,
  ) =>
      _sameBoard(left, right) &&
      left.updateAvailable &&
      right.updateAvailable &&
      firmwareVersionIdentity(left.latestVersion!) ==
          firmwareVersionIdentity(right.latestVersion!) &&
      left.snapshot?.release.channel == right.snapshot?.release.channel &&
      left.snapshot?.manifest.artifact.sha256 ==
          right.snapshot?.manifest.artifact.sha256 &&
      left.snapshot?.manifest.notes.sha256 ==
          right.snapshot?.manifest.notes.sha256;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    _foreground = lifecycle == null || lifecycle == AppLifecycleState.resumed;
    unawaited(_loadSeenVersions());
  }

  Future<void> _loadSeenVersions() async {
    try {
      final preferences = await SharedPreferences.getInstance();
      if (!mounted) return;
      _seenVersions.addAll(
        (preferences.getStringList(_seenVersionsKey) ?? const <String>[])
            .map(firmwareVersionIdentity)
            .whereType<String>(),
      );
      _preferences = preferences;
      _suppressFuture = preferences.getBool(_suppressFutureKey) ?? false;
      _scheduleOffer();
    } catch (_) {
      // Without durable history, keep the ordinary Update entry available
      // rather than repeatedly interrupting the user after every restart.
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _scheduleOffer();
  }

  @override
  void didUpdateWidget(covariant FirmwareUpdatePrompt oldWidget) {
    super.didUpdateWidget(oldWidget);
    _scheduleOffer();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _foreground = state == AppLifecycleState.resumed;
    _scheduleOffer();
  }

  void _scheduleOffer() {
    if (_scheduled) return;
    _scheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _scheduled = false;
      if (!mounted) return;
      final offer = widget.availability;
      final snapshot = offer.snapshot;
      final valid =
          widget.eligible &&
          offer.updateAvailable &&
          snapshot != null &&
          snapshot.manifest.version == offer.latestVersion;
      final route = _dialogRoute;
      if (route != null) {
        final presented = _presentedOffer;
        if (!widget.eligible ||
            !_foreground ||
            presented == null ||
            !_sameBoard(offer, presented) ||
            (!offer.loading && !_sameOffer(offer, presented))) {
          if (route.isActive) route.navigator?.removeRoute(route);
        }
        return;
      }
      if (!valid ||
          !_foreground ||
          _preferences == null ||
          _suppressFuture ||
          AppUpgradeNoticePriority.blocksFirmware(context) ||
          _activePointers != 0 ||
          widget.canPresent?.call() == false ||
          !TickerMode.valuesOf(context).enabled ||
          ModalRoute.isCurrentOf(context) == false ||
          Navigator.of(context).userGestureInProgress) {
        return;
      }
      final version = firmwareVersionIdentity(offer.latestVersion!);
      if (version == null || _seenVersions.contains(version)) return;
      if (widget.showingUpdate) {
        // The ready Update page already shows this exact snapshot's notes.
        _recordSeen(version);
      } else {
        unawaited(_showOffer(offer, version));
      }
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  void _recordSeen(String version) {
    if (!_seenVersions.add(version)) return;
    unawaited(
      _preferences!
          .setStringList(_seenVersionsKey, _seenVersions.toList())
          .then<void>((_) {}, onError: (Object _, StackTrace _) {}),
    );
  }

  Future<bool> _setSuppressFuture(bool value) async {
    final saved = await _preferences!.setBool(_suppressFutureKey, value);
    if (saved) _suppressFuture = value;
    return saved;
  }

  Future<void> _showOffer(
    FirmwareUpdateAvailability offer,
    String version,
  ) async {
    final snapshot = offer.snapshot!;
    final route = DialogRoute<bool>(
      context: context,
      builder: (dialogContext) {
        final strings = litchStrings(dialogContext);
        return ReleaseNotesDialog(
          key: const ValueKey<String>('firmware-update-offer'),
          title: strings.boardFirmwareAvailable(offer.latestVersion!),
          style: ReleaseNotesDialogStyle.firmwareOffer,
          subtitle: strings.firmwareReadyToInstall(offer.installedVersion!),
          notes: _offerNotes(dialogContext, snapshot),
          preferenceLabel: strings.firmwareNoticePreference,
          suppressFuture: _suppressFuture,
          onSuppressFutureChanged: _setSuppressFuture,
          primaryAction: strings.reviewFirmwareUpdate,
          secondaryAction: strings.firmwareUpdateLater,
        );
      },
    );
    _presentedOffer = offer;
    _dialogRoute = route;
    final result = Navigator.of(context).push<bool>(route);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted &&
          route.isCurrent &&
          identical(route, _dialogRoute) &&
          widget.eligible &&
          (_sameOffer(widget.availability, offer) ||
              (widget.availability.loading &&
                  _sameBoard(widget.availability, offer))) &&
          _foreground) {
        // Save on appearance, not on a button tap: closing the app here must
        // not repeat this offer after restart.
        _recordSeen(version);
      }
    });
    final review = await result;
    if (!mounted) return;
    _dialogRoute = null;
    _presentedOffer = null;
    if (review == true &&
        widget.eligible &&
        (_sameOffer(widget.availability, offer) ||
            (widget.availability.loading &&
                _sameBoard(widget.availability, offer)))) {
      widget.onReviewUpdate();
    }
    _scheduleOffer();
  }

  Widget _offerNotes(BuildContext context, FirmwareReleaseSnapshot snapshot) {
    final strings = litchStrings(context);
    final lines = snapshot.notesMarkdown.trim().split('\n');
    final metadata = RegExp(
      r'^Version:\s*(\S+)\s*$',
      caseSensitive: false,
    ).firstMatch(lines.first.trim());
    if (metadata != null &&
        firmwareVersionIdentity(metadata[1]!) ==
            firmwareVersionIdentity(snapshot.manifest.version)) {
      lines.removeAt(0);
    }
    final details = lines.join('\n').trim();
    final style = Theme.of(context).textTheme.bodySmall?.copyWith(
      color: Theme.of(context).colorScheme.onSurface,
    );
    return DefaultTextStyle.merge(
      style: style,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          if (snapshot.release.channel ==
              FirmwareReleaseChannel.beta) ...<Widget>[
            Text(
              strings.betaFirmware,
              style: style?.copyWith(fontWeight: FontWeight.w700),
            ),
            const SizedBox(height: 6),
          ],
          if (details.isNotEmpty && !localizedUsesEnglish(strings)) ...<Widget>[
            Text(strings.originalNotes),
            const SizedBox(height: 6),
          ],
          Text(details.isEmpty ? strings.notesEmpty : details),
        ],
      ),
    );
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    final route = _dialogRoute;
    if (route != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (route.isActive) route.navigator?.removeRoute(route);
      });
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Listener(
    onPointerDown: (_) => _activePointers++,
    onPointerUp: (_) {
      if (_activePointers > 0) _activePointers--;
      _scheduleOffer();
    },
    onPointerCancel: (_) {
      if (_activePointers > 0) _activePointers--;
      _scheduleOffer();
    },
    child: widget.child,
  );
}
