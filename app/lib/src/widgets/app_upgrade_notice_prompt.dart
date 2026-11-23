import 'dart:async';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../localization/litch_strings.dart';
import '../services/app_upgrade_notice_service.dart';
import '../services/litch_ble_service.dart';
import 'release_notes_dialog.dart';
import 'firmware_release_notes_card.dart';

/// App notes own the first notice opportunity, including offline launches.
class AppUpgradeNoticePrompt extends StatefulWidget {
  const AppUpgradeNoticePrompt({
    super.key,
    required this.bleService,
    required this.child,
    this.service,
    this.startupReady = true,
    this.canPresent,
  });

  final LitchBleService bleService;
  final AppUpgradeNoticeService? service;
  final bool startupReady;
  final bool Function()? canPresent;
  final Widget child;

  @override
  State<AppUpgradeNoticePrompt> createState() => _AppUpgradeNoticePromptState();
}

class _AppUpgradeNoticePromptState extends State<AppUpgradeNoticePrompt>
    with WidgetsBindingObserver {
  static const _suppressFutureKey = 'app_update_notices_disabled';
  late final _service = widget.service ?? AppUpgradeNoticeService();
  SharedPreferences? _preferences;
  AppUpgradeNotice? _notice;
  DialogRoute<bool>? _dialogRoute;
  final List<StreamSubscription<Object?>> _subscriptions = [];
  bool _loading = true;
  bool _suppressFuture = false;
  bool _foreground = true;
  bool _scheduled = false;
  int _activePointers = 0;

  bool get _ready =>
      widget.startupReady &&
      widget.canPresent?.call() != false &&
      widget.bleService.bleAvailability != BleAvailability.checking &&
      !widget.bleService.ownerPairingBusy &&
      !widget.bleService.identifyConnectionActive &&
      widget.bleService.pairingRecovery == null &&
      widget.bleService.ownerPairingRefusedDeviceId == null &&
      widget.bleService.pendingOtaVerification == null &&
      (!widget.bleService.isConnected || widget.bleService.productReady);

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    final lifecycle = WidgetsBinding.instance.lifecycleState;
    _foreground = lifecycle == null || lifecycle == AppLifecycleState.resumed;
    for (final stream in <Stream<Object?>>[
      widget.bleService.connectionStream,
      widget.bleService.productReadinessStream,
      widget.bleService.ownerPairingStatusStream,
      widget.bleService.pairingRecoveryStream,
      widget.bleService.availabilityStream,
    ]) {
      _subscriptions.add(stream.listen((_) => _scheduleNotice()));
    }
    unawaited(_loadNotice());
  }

  Future<void> _loadNotice() async {
    try {
      final preferences = await SharedPreferences.getInstance();
      final notice = await _service.pendingNotice();
      if (!mounted) return;
      setState(() {
        _preferences = preferences;
        _suppressFuture = preferences.getBool(_suppressFutureKey) ?? false;
        _notice = notice;
        _loading = false;
      });
      _scheduleNotice();
    } catch (_) {
      if (mounted) setState(() => _loading = false);
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _scheduleNotice();
  }

  @override
  void didUpdateWidget(covariant AppUpgradeNoticePrompt oldWidget) {
    super.didUpdateWidget(oldWidget);
    _scheduleNotice();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _foreground = state == AppLifecycleState.resumed;
    if (!_foreground) _activePointers = 0;
    _scheduleNotice();
  }

  void _scheduleNotice() {
    if (_scheduled) return;
    _scheduled = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _scheduled = false;
      if (!mounted) return;
      final route = _dialogRoute;
      if (route != null) {
        if ((!_ready || !_foreground) && route.isActive) {
          // Resume this notice when a connection or lifecycle interruption ends.
          route.navigator?.removeRoute(route, false);
        }
        return;
      }
      if (_loading ||
          _notice == null ||
          _suppressFuture ||
          !_ready ||
          !_foreground ||
          _activePointers != 0 ||
          ModalRoute.isCurrentOf(context) == false ||
          Navigator.of(context).userGestureInProgress) {
        return;
      }
      unawaited(_showNotice(_notice!));
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  Future<bool> _setSuppressFuture(bool value) async {
    final saved = await _preferences!.setBool(_suppressFutureKey, value);
    if (mounted && saved) setState(() => _suppressFuture = value);
    return saved;
  }

  Future<void> _showNotice(AppUpgradeNotice notice) async {
    bool appeared = false;
    final route = DialogRoute<bool>(
      context: context,
      builder: (dialogContext) {
        final strings = litchStrings(dialogContext);
        return ReleaseNotesDialog(
          key: const ValueKey<String>('app-upgrade-notice'),
          title: strings.appUpdateNotesTitle(notice.version),
          notes: FirmwareReleaseNotesCard(
            selectable: false,
            beta: false,
            details: notice.notes,
            showHeading: false,
          ),
          preferenceExplanation: strings.appNoticePreference,
          suppressFuture: _suppressFuture,
          onSuppressFutureChanged: _setSuppressFuture,
          primaryAction: strings.releaseNotesContinue,
        );
      },
    );
    _dialogRoute = route;
    final closed = Navigator.of(context).push<bool>(route);
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      if (mounted &&
          route.isCurrent &&
          identical(_dialogRoute, route) &&
          _ready &&
          _foreground) {
        appeared = true;
        try {
          await _service.acknowledge(notice);
        } catch (_) {}
      }
    });
    final result = await closed;
    await route.completed;
    if (!mounted) return;
    setState(() {
      _dialogRoute = null;
      if (appeared && result != false) _notice = null;
    });
    _scheduleNotice();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    for (final subscription in _subscriptions) {
      unawaited(subscription.cancel());
    }
    final route = _dialogRoute;
    if (route != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (route.isActive) route.navigator?.removeRoute(route);
      });
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => AppUpgradeNoticePriority(
    blocksFirmwareNotice:
        _loading ||
        _dialogRoute != null ||
        (_notice != null && !_suppressFuture),
    onReadinessChanged: _scheduleNotice,
    child: Listener(
      onPointerDown: (_) => _activePointers++,
      onPointerUp: (_) {
        if (_activePointers > 0) _activePointers--;
        _scheduleNotice();
      },
      onPointerCancel: (_) {
        if (_activePointers > 0) _activePointers--;
        _scheduleNotice();
      },
      child: widget.child,
    ),
  );
}

class AppUpgradeNoticePriority extends InheritedWidget {
  const AppUpgradeNoticePriority({
    super.key,
    required this.blocksFirmwareNotice,
    required this.onReadinessChanged,
    required super.child,
  });

  final bool blocksFirmwareNotice;
  final VoidCallback onReadinessChanged;

  static bool blocksFirmware(BuildContext context) =>
      context
          .dependOnInheritedWidgetOfExactType<AppUpgradeNoticePriority>()
          ?.blocksFirmwareNotice ??
      false;

  static void requestCheck(BuildContext context) => context
      .getInheritedWidgetOfExactType<AppUpgradeNoticePriority>()
      ?.onReadinessChanged();

  @override
  bool updateShouldNotify(AppUpgradeNoticePriority oldWidget) =>
      blocksFirmwareNotice != oldWidget.blocksFirmwareNotice;
}
