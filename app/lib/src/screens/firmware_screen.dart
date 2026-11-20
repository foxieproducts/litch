import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter/services.dart';
import 'package:intl/intl.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../l10n/generated/app_localizations.dart';
import '../constants/firmware_release.dart';
import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';
import '../models/app_release_identity.dart';
import '../models/firmware_provenance.dart';
import '../models/firmware_release_snapshot.dart';
import '../models/litch_device_capabilities.dart';
import '../services/firmware_artifact_validator.dart';
import '../services/firmware_download_service.dart';
import '../services/firmware_preference_fallback.dart';
import '../services/firmware_status_text.dart';
import '../services/firmware_transfer_estimate.dart';
import '../services/litch_ble_service.dart';
import '../services/ota/litch_firmware_uploader.dart';
import '../services/ota/ota_benchmark_runner.dart';
import '../services/ota/ota_post_reboot_verifier.dart';
import '../widgets/firmware_provenance_card.dart';
import '../widgets/firmware_release_notes_card.dart';
import '../widgets/firmware_update_card.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/reading_pane.dart';

@immutable
class FirmwareUpdateAvailability {
  const FirmwareUpdateAvailability({
    this.installedVersion,
    this.latestVersion,
    this.supportsUpdate = false,
    this.loading = true,
  });

  final String? installedVersion;
  final String? latestVersion;
  final bool supportsUpdate;
  final bool loading;

  bool get updateAvailable =>
      supportsUpdate &&
      installedVersion != null &&
      latestVersion != null &&
      installedVersion != latestVersion;

  @override
  bool operator ==(Object other) {
    return other is FirmwareUpdateAvailability &&
        other.installedVersion == installedVersion &&
        other.latestVersion == latestVersion &&
        other.supportsUpdate == supportsUpdate &&
        other.loading == loading;
  }

  @override
  int get hashCode =>
      Object.hash(installedVersion, latestVersion, supportsUpdate, loading);
}

class FirmwareScreen extends StatefulWidget {
  const FirmwareScreen({
    super.key,
    required this.bleService,
    required this.boardId,
    required this.onRequestShowFirmwareTab,
    required this.onUpdateActivityChanged,
    this.onAvailabilityChanged,
    this.transferElapsed,
    this.benchmarkProgress,
    FirmwareDownloadService? downloadService,
  }) : _downloadService = downloadService;

  final FirmwareDownloadService? _downloadService;

  /// Monotonic presentation clock; injectable for deterministic progress tests.
  @visibleForTesting
  final Duration Function()? transferElapsed;

  final LitchBleService bleService;
  final String boardId;
  final VoidCallback onRequestShowFirmwareTab;
  final ValueChanged<bool> onUpdateActivityChanged;
  final ValueChanged<FirmwareUpdateAvailability>? onAvailabilityChanged;
  final OtaBenchmarkProgress? benchmarkProgress;

  @override
  State<FirmwareScreen> createState() => _FirmwareScreenState();
}

class _FirmwareScreenState extends State<FirmwareScreen>
    with WidgetsBindingObserver {
  static const String allowBetaUpdatesKey = 'allow_beta_updates';
  static const FirmwarePreferenceFallback _fallbackPreferences =
      FirmwarePreferenceFallback();
  late final FirmwareDownloadService _downloadService =
      widget._downloadService ?? FirmwareDownloadService();

  bool _useBetaFirmware = false;
  bool _loadingVersion = false;
  bool _releaseLoadFailed = false;
  bool _downloadingFirmware = false;
  bool _uploadingFirmware = false;
  bool _uploadFlowActive = false;
  bool _verifyingFirmware = false;
  bool _restoringOtaVerification = true;
  StreamSubscription<ConnectionStateUpdate>? _connectionSub;
  bool _hasVerificationStatus = false;
  String _versionDetails = '';
  String Function(AppLocalizations) _status = (strings) =>
      strings.updateInfoAuto;
  double _downloadProgress = 0;
  double _uploadProgress = 0;
  bool _transferringFirmware = false;
  final _transferEstimate = FirmwareTransferEstimate();
  final _transferClock = Stopwatch()..start();
  Timer? _transferUiTick;
  final _transferDisplayKey = GlobalKey();
  bool _transferRevealAttempted = false;
  int _transferRevealEpoch = 0;
  int _activeTouchCount = 0;

  void _trackTransferPointer() {
    _activeTouchCount += 1;
    if (_uploadFlowActive || widget.benchmarkProgress?.isRunning == true) {
      // A gesture during activity owns the viewport, even before bytes
      // arrive. It also retires a queued reveal before its next layout frame.
      _transferRevealAttempted = true;
      _transferRevealEpoch += 1;
    }
  }

  void _releaseTransferPointer() {
    if (_activeTouchCount > 0) _activeTouchCount -= 1;
  }

  void _revealTransferOnce() {
    if (_transferRevealAttempted) return;
    _transferRevealAttempted = true;
    final attempt = _transferRevealEpoch;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted ||
          (!_transferringFirmware &&
              widget.benchmarkProgress?.isRunning != true) ||
          attempt != _transferRevealEpoch ||
          _activeTouchCount != 0) {
        return;
      }
      final context = _transferDisplayKey.currentContext;
      if (context == null) return;
      final scrollable = Scrollable.maybeOf(context);
      final box = context.findRenderObject();
      final viewport = scrollable?.context.findRenderObject();
      if (scrollable == null ||
          box is! RenderBox ||
          viewport is! RenderBox ||
          scrollable.position.isScrollingNotifier.value) {
        return;
      }
      final content = box.localToGlobal(Offset.zero) & box.size;
      final bounds = viewport.localToGlobal(Offset.zero) & viewport.size;
      if (content.left >= bounds.left &&
          content.right <= bounds.right &&
          content.top >= bounds.top &&
          content.bottom <= bounds.bottom) {
        return;
      }
      // One layout-owned reveal after entry, never per-progress scrolling or
      // a focus request. An active user gesture always wins this opportunity.
      unawaited(scrollable.position.ensureVisible(box, alignment: 0));
    });
  }

  Duration get _transferElapsed =>
      widget.transferElapsed?.call() ?? _transferClock.elapsed;

  void _resetTransferPresentation() {
    _transferringFirmware = false;
    _transferEstimate.reset();
    _transferUiTick?.cancel();
    _transferUiTick = null;
  }

  void _recordTransferProgress(double progress) {
    _transferEstimate.add(progress, _transferElapsed);
    _transferringFirmware = progress.isFinite && progress > 0 && progress < 1;
    if (!_transferringFirmware) {
      _transferUiTick?.cancel();
      _transferUiTick = null;
      return;
    }
    _revealTransferOnce();
    // Only the active byte-transfer surface refreshes once a second. This
    // expires a stalled estimate even when no progress callback arrives.
    _transferUiTick ??= Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  String? _websiteVersion;
  String? _boardVersion;
  bool _preferencesAvailable = true;
  String? _appVersion;
  File? _downloadedFile;
  FirmwareReleaseSnapshot? _releaseSnapshot;
  int _releaseLoadGeneration = 0;
  FirmwareProvenance _provenance = FirmwareProvenance.offline;
  LitchDeviceCapabilities? _capabilities;
  OtaVerificationTarget? get _pendingOtaVerification =>
      widget.bleService.pendingOtaVerification;
  bool get _isUpdateModal =>
      _uploadFlowActive ||
      _uploadingFirmware ||
      _downloadingFirmware ||
      _verifyingFirmware ||
      widget.benchmarkProgress?.isRunning == true;

  String get _targetBoardId => widget.bleService.connectedId ?? widget.boardId;

  bool get _supportsUpdate =>
      widget.bleService.isConnected && (_capabilities?.supportsFirmwareUpdate ?? false);

  String? _updateGuidance(AppLocalizations strings) {
    if (_isUpdateModal || _pendingOtaVerification != null) return null;
    if (!widget.bleService.isConnected) return strings.connectBeforeUpdate;
    final capabilities = _capabilities;
    if (capabilities == null || capabilities.supportsFirmwareUpdate) return null;
    if (!capabilities.explicitlyReported) return strings.boardRefreshFailed;
    return strings.manualUsbUpdate(capabilities.hardwareLabel);
  }

  FirmwareReleaseInfo get _selectedRelease =>
      _useBetaFirmware ? FirmwareReleaseInfo.beta : FirmwareReleaseInfo.stable;

  String _releaseLabel(AppLocalizations strings) =>
      _selectedRelease.channel == FirmwareReleaseChannel.beta
      ? strings.betaFirmware.toLowerCase()
      : strings.stableFirmware.toLowerCase();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _connectionSub = widget.bleService.connectionStream.listen(
      _onConnectionChanged,
    );
    if (widget.benchmarkProgress?.isRunning == true) _revealTransferOnce();
    _initializeScreen();
  }

  @override
  void didUpdateWidget(covariant FirmwareScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.benchmarkProgress?.isRunning != true &&
        widget.benchmarkProgress?.isRunning == true) {
      _transferRevealEpoch += 1;
      _transferRevealAttempted = false;
      _revealTransferOnce();
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && _transferringFirmware) {
      // Background time cannot lend confidence to a newly visible estimate.
      setState(_transferEstimate.reset);
    }
  }

  void _onConnectionChanged(ConnectionStateUpdate update) {
    if (update.connectionState == DeviceConnectionState.disconnected) {
      if (_transferringFirmware) setState(_resetTransferPresentation);
      if (!widget.bleService.isConnected && !_isUpdateModal) {
        setState(() {
          _provenance = FirmwareProvenance.offline;
          _capabilities = null;
          if (!_hasVerificationStatus) _boardVersion = null;
        });
        _notifyAvailability();
      }
    }
    if (update.connectionState == DeviceConnectionState.connected) {
      if (_pendingOtaVerification != null) {
        unawaited(_verifyPendingUpdate());
      } else if (!_isUpdateModal) {
        unawaited(_refreshConnectedBoard());
      }
    }
  }

  bool _isCurrentBoardRead(String boardId, int connectionToken) =>
      mounted &&
      _targetBoardId == boardId &&
      widget.bleService.connectionSessionToken == connectionToken;

  Future<void> _refreshConnectedBoard() async {
    final boardId = _targetBoardId;
    final connectionToken = widget.bleService.connectionSessionToken;
    await _refreshCapabilities(refresh: true);
    if (!_isCurrentBoardRead(boardId, connectionToken) || _isUpdateModal) {
      return;
    }
    await Future.wait(<Future<void>>[
      if (!_hasVerificationStatus) _refreshBoardVersion(),
      _refreshProvenance(),
    ]);
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _resetTransferPresentation();
    _transferClock.stop();
    unawaited(_connectionSub?.cancel());
    _downloadService.dispose();
    super.dispose();
  }

  Future<void> _initializeScreen() async {
    try {
      await widget.bleService.restorePendingOtaVerification();
    } catch (_) {
      if (mounted) {
        setState(() {
          _restoringOtaVerification = false;
          _hasVerificationStatus = true;
          _status = (strings) => localizedVerification(
            strings,
            const OtaPostRebootPending(
              'The saved update could not be read. Restart the app and retry. '
              'If this continues, open Help and send a support report.',
            ),
          );
        });
      }
      return;
    }
    if (!mounted) return;
    setState(() => _restoringOtaVerification = false);
    if (_pendingOtaVerification == null) {
      // Optional phone metadata and a slow capability query do not own the
      // independent provenance read. Pending updates retain verification first.
      unawaited(_refreshProvenance());
    }
    await _loadAppVersion();
    await _loadPersistedPreferences();
    if (!mounted) {
      return;
    }
    if (_pendingOtaVerification != null) {
      await _verifyPendingUpdate();
      return;
    }
    final failed = widget.bleService.failedOtaVerification;
    if (failed != null && failed.target.deviceId == _targetBoardId) {
      _hasVerificationStatus = true;
      _boardVersion = failed.result.version;
      _status = (strings) => localizedVerification(strings, failed.result);
    }
    await _refreshCapabilities();
    if (!mounted) return;
    await _loadVersionDetails();
  }

  Future<void> _refreshCapabilities({bool refresh = false}) async {
    final boardId = _targetBoardId;
    final connectionToken = widget.bleService.connectionSessionToken;
    final LitchDeviceCapabilities capabilities = await widget.bleService
        .fetchDeviceCapabilities(deviceId: boardId, refresh: refresh);
    if (!_isCurrentBoardRead(boardId, connectionToken)) {
      return;
    }
    setState(() => _capabilities = capabilities);
    _notifyAvailability();
  }

  Future<void> _loadAppVersion() async {
    String? label;
    try {
      final PackageInfo packageInfo = await PackageInfo.fromPlatform();
      final String version = litchAppReleaseIdentity.compactVersionFor(
        packageInfo,
      );
      if (version.isNotEmpty) label = version;
    } catch (_) {}

    if (!mounted) {
      return;
    }

    setState(() {
      _appVersion = label;
    });
  }

  Future<void> _loadVersionDetails() async {
    final FirmwareReleaseInfo release = _selectedRelease;
    final int generation = ++_releaseLoadGeneration;

    setState(() {
      _loadingVersion = true;
      _releaseLoadFailed = false;
      _releaseSnapshot = null;
      if (!_hasVerificationStatus) {
        _status = (strings) => strings.loadingUpdateDetails;
      }
    });
    _notifyAvailability();

    try {
      final FirmwareReleaseSnapshot snapshot = await _downloadService
          .fetchReleaseSnapshot(release);
      final String markdown = snapshot.notesMarkdown;
      if (!_isCurrentReleaseLoad(release, generation)) {
        return;
      }
      setState(() {
        _releaseLoadFailed = false;
        _versionDetails = markdown;
        _releaseSnapshot = snapshot;
        _websiteVersion = snapshot.manifest.version;
        if (!_hasVerificationStatus) {
          _status = (strings) => _websiteVersion == null
              ? strings.updateDetailsLoaded
              : strings.latestVersionIs(_websiteVersion!);
        }
      });

      // Verification already owns the installed identity. A notes refresh
      // must neither erase it on an empty reread nor query a different board.
      if (!_hasVerificationStatus) {
        await _refreshBoardVersion();
      }
    } catch (error) {
      if (!_isCurrentReleaseLoad(release, generation)) {
        return;
      }
      setState(() {
        _releaseLoadFailed = true;
        _versionDetails = '';
        if (!_hasVerificationStatus) {
          _status = (strings) => strings.updateLoadFailed;
        }
      });
    } finally {
      if (_isCurrentReleaseLoad(release, generation)) {
        setState(() {
          _loadingVersion = false;
        });
        _notifyAvailability();
      }
    }
  }

  bool _isCurrentReleaseLoad(FirmwareReleaseInfo release, int generation) {
    return mounted &&
        generation == _releaseLoadGeneration &&
        release.channel == _selectedRelease.channel;
  }

  Future<void> _refreshProvenance() async {
    final boardId = _targetBoardId;
    final connectionToken = widget.bleService.connectionSessionToken;
    final FirmwareProvenance provenance = await widget.bleService
        .fetchFirmwareProvenance(deviceId: boardId);
    if (!_isCurrentBoardRead(boardId, connectionToken) || _isUpdateModal) {
      return;
    }
    setState(() {
      _provenance = provenance;
    });
  }

  Future<void> _loadPersistedPreferences() async {
    bool allowBetaUpdates = false;
    try {
      final SharedPreferences prefs = await SharedPreferences.getInstance();
      allowBetaUpdates = prefs.getBool(allowBetaUpdatesKey) ?? false;
      _preferencesAvailable = true;
    } on MissingPluginException {
      _preferencesAvailable = false;
      allowBetaUpdates = await _fallbackPreferences.readBool(
        allowBetaUpdatesKey,
      );
    } catch (_) {
      _preferencesAvailable = false;
      allowBetaUpdates = await _fallbackPreferences.readBool(
        allowBetaUpdatesKey,
      );
    }

    if (!mounted) {
      return;
    }

    if (_useBetaFirmware == allowBetaUpdates) {
      return;
    }

    setState(() {
      _useBetaFirmware = allowBetaUpdates;
      _versionDetails = '';
      _websiteVersion = null;
      _boardVersion = null;
      _downloadedFile = null;
      _releaseSnapshot = null;
      _status = (strings) => strings.channelChanged(_releaseLabel(strings));
    });
  }

  Future<void> _setAllowBetaUpdates(bool enabled) async {
    if (_isUpdateModal || _pendingOtaVerification != null) {
      return;
    }

    setState(() {
      _hasVerificationStatus = false;
      _useBetaFirmware = enabled;
      _versionDetails = '';
      _websiteVersion = null;
      _boardVersion = null;
      _downloadedFile = null;
      _releaseSnapshot = null;
      _status = (strings) => strings.channelChanged(_releaseLabel(strings));
    });
    final Future<void> versionLoad = _loadVersionDetails();
    final Future<void> provenanceRefresh = _refreshProvenance();

    bool persisted = false;
    try {
      final SharedPreferences prefs = await SharedPreferences.getInstance();
      await prefs.setBool(allowBetaUpdatesKey, enabled);
      _preferencesAvailable = true;
      persisted = true;
    } on MissingPluginException {
      _preferencesAvailable = false;
    } catch (_) {
      _preferencesAvailable = false;
    }

    if (!_preferencesAvailable) {
      persisted = await _fallbackPreferences.writeBool(
        allowBetaUpdatesKey,
        enabled,
      );
    } else {
      await _fallbackPreferences.writeBool(allowBetaUpdatesKey, enabled);
    }

    if (!mounted) {
      return;
    }

    if (!_preferencesAvailable &&
        !persisted &&
        _loadingVersion &&
        _selectedRelease.channel ==
            (enabled
                ? FirmwareReleaseChannel.beta
                : FirmwareReleaseChannel.stable)) {
      setState(() {
        _status = (strings) =>
            strings.channelChangedUnsaved(_releaseLabel(strings));
      });
    }

    await Future.wait(<Future<void>>[versionLoad, provenanceRefresh]);
  }

  Future<void> _refreshBoardVersion() async {
    final boardId = _targetBoardId;
    final connectionToken = widget.bleService.connectionSessionToken;
    final String? boardVersion;
    try {
      boardVersion = await widget.bleService.fetchBoardFirmwareVersion(
        deviceId: boardId,
      );
    } catch (_) {
      // Reconnect can retire the session while this optional read is opening.
      // Keep unknown/current metadata without resolving an OTA outcome.
      return;
    }

    if (!_isCurrentBoardRead(boardId, connectionToken) ||
        _hasVerificationStatus ||
        _isUpdateModal) {
      return;
    }

    setState(() {
      _boardVersion = boardVersion;
    });
    _notifyAvailability();
  }

  void _notifyAvailability() {
    widget.onAvailabilityChanged?.call(
      FirmwareUpdateAvailability(
        installedVersion: _boardVersion,
        latestVersion: _websiteVersion,
        supportsUpdate: _supportsUpdate,
        loading: _loadingVersion,
      ),
    );
  }

  Future<void> _uploadFirmware({FirmwareReleaseSnapshot? snapshot}) async {
    if (_isUpdateModal ||
        _restoringOtaVerification ||
        widget.bleService.otaVerificationStorageUnavailable ||
        _pendingOtaVerification != null ||
        _loadingVersion ||
        _boardVersion == null ||
        _websiteVersion == null ||
        _releaseSnapshot == null) {
      return;
    }

    if (!widget.bleService.isConnected) {
      setState(() {
        _status = (strings) => strings.connectBeforeUpdate;
      });
      return;
    }
    if (!_supportsUpdate) return;

    widget.onUpdateActivityChanged(true);
    _uploadFlowActive = true;
    _transferRevealEpoch += 1;
    _transferRevealAttempted = false;
    final String boardId = _targetBoardId;

    try {
      setState(() {
        _resetTransferPresentation();
        _hasVerificationStatus = false;
        _downloadingFirmware = true;
        _status = (strings) => strings.preparingUpdate;
        _downloadProgress = 0;
      });

      snapshot ??= _releaseSnapshot;
      if (snapshot == null ||
          snapshot.release.channel != _selectedRelease.channel ||
          snapshot.manifest.version != _websiteVersion) {
        throw const FirmwareFetchException(
          'Refresh update details before starting the update.',
        );
      }
      final FirmwarePreparedFile prepared;
      prepared = await _downloadService.prepareFirmwareForUpdate(
        snapshot,
        onProgress: (double progress) {
          if (!mounted) {
            return;
          }
          setState(() {
            _downloadProgress = progress.clamp(0, 1);
          });
        },
      );
      if (!mounted) {
        return;
      }

      setState(() {
        _downloadingFirmware = false;
        _downloadedFile = prepared.file;
        _downloadProgress = 0;
        _uploadingFirmware = true;
        _uploadProgress = 0;
        _status = (strings) => strings.startingInstallation;
      });

      final FirmwareReleaseSnapshot selectedSnapshot = snapshot;
      final OtaUploadResult upload = await widget.bleService.uploadFirmwareFile(
        deviceId: boardId,
        firmwareFile: _downloadedFile!,
        manifest: selectedSnapshot.manifest,
        onProgress: (double progress) {
          if (!mounted) {
            return;
          }
          setState(() {
            // Transport completion is not post-boot success. Reserve 100% for
            // the exact image and runtime acceptance verification below.
            _uploadProgress = progress.clamp(0, .99);
            _recordTransferProgress(progress);
            if (progress >= 1) {
              _status = (strings) => strings.installingUpdate;
            }
          });
        },
        onStatus: (String message) {
          if (!mounted) {
            return;
          }
          setState(() {
            _status = (strings) => localizedUsesEnglish(strings)
                ? simplifyFirmwareStatus(message)
                : strings.installingUpdate;
          });
        },
      );

      if (upload is OtaUploadFinalized ||
          upload is OtaUploadPendingVerification) {
        // The service already retained the exact admitted target before End.
        // A screen may disappear during finalization without losing ownership.
        await _verifyPendingUpdate();
      } else if (mounted) {
        setState(() {
          _uploadingFirmware = false;
          _status = (strings) {
            if (upload case OtaUploadFailed(:final String detail)) {
              switch (detail) {
                case 'persist_failed':
                  return strings.updatePersistFailed;
                case 'persist_timeout':
                  return strings.updatePersistTimeout;
                case 'persist_not_ready':
                  return strings.updatePersistNotReady;
              }
            }
            return localizedUpdateDetail(
              strings,
              strings.updateFailedRecovery,
              upload.message,
            );
          };
        });
      }
    } catch (error) {
      final invalidPackage =
          error is FirmwareArtifactValidationException ||
          error is FirmwareFetchException &&
              error.cause is FirmwareArtifactValidationException;
      if (invalidPackage) debugPrint('Firmware package rejected: $error');
      if (mounted) {
        setState(() {
          _downloadingFirmware = false;
          _uploadingFirmware = false;
          _status = (strings) => localizedUpdateDetail(
            strings,
            strings.updateBeforeUploadFailed,
            invalidPackage
                ? FirmwareArtifactValidationException.userMessage
                : 'Update canceled before upload: ${simplifyFirmwareStatus(error.toString())}',
          );
        });
      }
    } finally {
      _resetTransferPresentation();
      _uploadFlowActive = false;
      if (mounted) {
        setState(() {});
        if (!_verifyingFirmware) widget.onUpdateActivityChanged(false);
      }
    }
  }

  Future<void> _verifyPendingUpdate() async {
    final OtaVerificationTarget? target = _pendingOtaVerification;
    if (target == null ||
        !mounted ||
        _verifyingFirmware ||
        widget.benchmarkProgress?.isRunning == true) {
      return;
    }
    final waitForReboot = widget.bleService.pendingOtaRebootExpected;
    widget.onRequestShowFirmwareTab();
    widget.onUpdateActivityChanged(true);
    if (mounted) {
      setState(() {
        _resetTransferPresentation();
        _uploadingFirmware = false;
        _verifyingFirmware = true;
        _hasVerificationStatus = true;
        _status = (strings) => strings.waitingUpdateVerification;
      });
    }
    try {
      OtaPostRebootVerification verification = await _verifyPendingTarget(
        target,
        waitForReboot: waitForReboot,
      );
      if (!mounted) {
        return;
      }
      if ((verification is OtaPostRebootVerified ||
              verification is OtaPostRebootDifferentImage) &&
          !_isVerificationBoardConnected(target.deviceId)) {
        verification = const OtaPostRebootPending(
          'The board connection changed during verification; reconnect the original board and try again.',
        );
      }
      try {
        if (verification is OtaPostRebootVerified) {
          if (!await widget.bleService.clearPendingOtaVerification(target)) {
            throw StateError('The pending update changed during verification.');
          }
        } else if (verification is OtaPostRebootDifferentImage) {
          await widget.bleService.resolveFailedOtaVerification(
            target,
            verification,
          );
        }
      } catch (_) {
        verification = const OtaPostRebootPending(
          'The result could not be saved. Retry verification before another update.',
        );
      }
      if (!mounted) return;
      setState(() {
        _verifyingFirmware = false;
        _status = (strings) => localizedVerification(strings, verification);
        if (verification is OtaPostRebootVerified) {
          _uploadProgress = 1;
          _boardVersion = target.version;
        } else if (verification is OtaPostRebootDifferentImage) {
          _uploadProgress = 0;
          _boardVersion = verification.version;
        }
      });
      _notifyAvailability();
      // Metadata is not part of the modal verification owner. Its completion
      // must not release a later verification's activity lock.
      if (waitForReboot && verification is! OtaPostRebootPending) {
        unawaited(_refreshCapabilities());
      }
      unawaited(_loadVersionDetails());
    } finally {
      if (mounted && !_uploadFlowActive) {
        widget.onUpdateActivityChanged(false);
      }
    }
  }

  Future<OtaPostRebootVerification> _verifyPendingTarget(
    OtaVerificationTarget target, {
    required bool waitForReboot,
  }) async {
    try {
      if (widget.bleService.isConnected &&
          widget.bleService.connectedId != target.deviceId) {
        return const OtaPostRebootPending(
          'A different board is connected. Reconnect the original board to verify this update.',
        );
      }
      final bool reconnected =
          _isVerificationBoardConnected(target.deviceId) ||
          await widget.bleService.connect(target.deviceId);
      if (!mounted) {
        return const OtaPostRebootPending(
          'Post-reboot verification was canceled.',
        );
      }
      if (!waitForReboot) {
        if (!reconnected || !_isVerificationBoardConnected(target.deviceId)) {
          return const OtaPostRebootPending(
            'The original board did not reconnect. The update remains unverified; reconnect it and try again.',
          );
        }
        final capabilities = await widget.bleService.fetchDeviceCapabilities(
          deviceId: target.deviceId,
          refresh: true,
        );
        if (!mounted || !_isVerificationBoardConnected(target.deviceId)) {
          return const OtaPostRebootPending(
            'The board connection changed during verification; reconnect the original board and try again.',
          );
        }
        _capabilities = capabilities;
      }

      return await widget.bleService.verifyFirmwareAfterOta(
        deviceId: target.deviceId,
        uploadConnectionToken: target.uploadConnectionToken,
        expectedVersion: target.version,
        expectedImageSha256: target.imageSha256,
      );
    } catch (_) {
      return const OtaPostRebootPending(
        'Could not verify the installed update. Reconnect the original board and try again.',
      );
    }
  }

  bool _isVerificationBoardConnected(String deviceId) {
    return widget.bleService.isConnected &&
        widget.bleService.connectedId == deviceId;
  }

  String _benchmarkStatus(
    AppLocalizations strings,
    OtaBenchmarkProgress progress, {
    bool coarseProgress = false,
  }) {
    final kilobytes = NumberFormat('0.#', strings.localeName);
    final receivedBytes = coarseProgress && progress.totalBytes > 0
        ? (progress.receivedBytes * 4 ~/ progress.totalBytes) *
              progress.totalBytes ~/
              4
        : progress.receivedBytes;
    return switch (progress.phase) {
      OtaBenchmarkPhase.preparing => strings.connectionTestPreparing,
      OtaBenchmarkPhase.measuring => strings.connectionTestMeasuring(
        kilobytes.format(receivedBytes / 1000),
        kilobytes.format(progress.totalBytes / 1000),
      ),
      OtaBenchmarkPhase.uploading => strings.connectionTestUploading,
      OtaBenchmarkPhase.reconnecting => strings.connectionTestReconnecting,
      OtaBenchmarkPhase.verifying => strings.connectionTestVerifying,
      OtaBenchmarkPhase.ramComplete => strings.connectionTestComplete(
        kilobytes.format(progress.totalBytes / 1000),
      ),
      OtaBenchmarkPhase.updateComplete => strings.boardUpdateTestComplete,
      OtaBenchmarkPhase.failed => strings.connectionTestFailed,
    };
  }

  Future<void> _handlePullToRefresh() async {
    if (_isUpdateModal) {
      return;
    }
    if (_pendingOtaVerification != null) {
      await _verifyPendingUpdate();
      return;
    }

    await Future.wait(<Future<void>>[
      _loadVersionDetails(),
      _refreshProvenance(),
      _refreshCapabilities(refresh: true),
    ]);
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final theme = Theme.of(context);
    final benchmark = widget.benchmarkProgress;
    final bool updateAvailable =
        _supportsUpdate &&
        _boardVersion != null &&
        _websiteVersion != null &&
        _boardVersion != _websiteVersion;
    final String title = updateAvailable
        ? strings.versionReady(_websiteVersion!)
        : _loadingVersion
        ? strings.checkingSoftware
        : strings.boardSoftware;
    final updatePanel = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        FirmwareProvenanceCard(provenance: _provenance),
        const SizedBox(height: 12),
        FirmwareUpdateCard(
          allowBeta: _useBetaFirmware,
          channelChangeDisabled:
              _isUpdateModal ||
              _pendingOtaVerification != null ||
              _restoringOtaVerification ||
              widget.bleService.otaVerificationStorageUnavailable,
          updateDisabled:
              _isUpdateModal ||
              _pendingOtaVerification != null ||
              _restoringOtaVerification ||
              widget.bleService.otaVerificationStorageUnavailable ||
              _boardVersion == null ||
              _websiteVersion == null ||
              _loadingVersion ||
              _releaseSnapshot == null ||
              !_supportsUpdate,
          supportsFirmwareUpdate: _supportsUpdate,
          updateGuidance: _updateGuidance(strings),
          boardVersion: _boardVersion,
          latestVersion: _websiteVersion,
          downloading: benchmark == null && _downloadingFirmware,
          downloadProgress: _downloadProgress,
          uploading: benchmark == null && _uploadingFirmware,
          uploadProgress: _uploadProgress,
          transferInProgress: benchmark == null && _transferringFirmware,
          progressKey: _transferDisplayKey,
          estimatedRemaining: _transferringFirmware
              ? _transferEstimate.remainingAt(_transferElapsed)
              : null,
          verifying: _verifyingFirmware,
          status: benchmark == null
              ? _status(strings)
              : _benchmarkStatus(strings, benchmark),
          statusInProgress: benchmark?.isRunning ?? false,
          statusAnnouncement: benchmark?.isRunning == true
              ? _benchmarkStatus(strings, benchmark!, coarseProgress: true)
              : null,
          onAllowBetaChanged: _setAllowBetaUpdates,
          onUpdate: _uploadFirmware,
        ),
        if (_releaseLoadFailed && _pendingOtaVerification == null) ...<Widget>[
          const SizedBox(height: 8),
          Align(
            alignment: Alignment.centerLeft,
            child: OutlinedButton.icon(
              key: const ValueKey<String>('retry-update-details'),
              onPressed: _loadingVersion
                  ? null
                  : () => unawaited(_handlePullToRefresh()),
              icon: const Icon(Icons.refresh_rounded),
              label: Text(strings.tryAgain),
            ),
          ),
        ],
        if (_pendingOtaVerification != null) ...<Widget>[
          const SizedBox(height: 8),
          Semantics(
            button: true,
            label: strings.verifyUpdateSemantics,
            child: OutlinedButton.icon(
              onPressed: _isUpdateModal ? null : _verifyPendingUpdate,
              icon: const Icon(Icons.verified_outlined),
              label: Text(strings.verifyInstalledUpdate),
            ),
          ),
        ],
      ],
    );
    final notes = FirmwareReleaseNotesCard(
      beta: _selectedRelease.channel == FirmwareReleaseChannel.beta,
      originalContent:
          !_releaseLoadFailed &&
          _releaseSnapshot != null &&
          _versionDetails.trim().isNotEmpty,
      details: _releaseLoadFailed
          ? strings.notesUnavailable
          : _releaseSnapshot == null
          ? strings.notesNotLoaded
          : _versionDetails.trim().isEmpty
          ? strings.notesEmpty
          : _versionDetails,
    );
    final header = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (updateAvailable) ...<Widget>[
          Text(
            strings.updateAvailableEyebrow,
            style: theme.textTheme.labelLarge?.copyWith(
              color: theme.colorScheme.secondary,
              fontWeight: FontWeight.w800,
              letterSpacing: 1,
            ),
          ),
          const SizedBox(height: 4),
        ],
        Semantics(
          header: true,
          child: Text(
            title,
            style:
                (updateAvailable
                        ? theme.textTheme.headlineSmall
                        : theme.textTheme.titleLarge)
                    ?.copyWith(fontWeight: FontWeight.w700),
          ),
        ),
        const SizedBox(height: 6),
        Text(
          strings.installedVersion(
            _boardVersion ?? strings.unknownLabel.toLowerCase(),
          ),
          style: theme.textTheme.bodyMedium?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
        const SizedBox(height: 16),
      ],
    );
    final footer = Padding(
      padding: const EdgeInsets.only(top: 12),
      child: Text(
        strings.appVersion(_appVersion ?? strings.unknownLabel.toLowerCase()),
        textAlign: TextAlign.center,
        style: theme.textTheme.bodySmall,
      ),
    );
    return Listener(
      onPointerDown: (_) => _trackTransferPointer(),
      onPointerUp: (_) => _releaseTransferPointer(),
      onPointerCancel: (_) => _releaseTransferPointer(),
      child: SafeArea(
        top: false,
        child: RefreshIndicator(
          key: const ValueKey<String>('firmware-safe-content'),
          onRefresh: _handlePullToRefresh,
          notificationPredicate: (ScrollNotification notification) {
            return !_isUpdateModal &&
                defaultScrollNotificationPredicate(notification);
          },
          child: ListView(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 28),
            physics: const AlwaysScrollableScrollPhysics(),
            children: [
              LayoutBuilder(
                builder: (context, constraints) {
                  final tablet = LitchWorkspaceGeometry.isTabletWindow(
                    MediaQuery.sizeOf(context),
                  );
                  final textScale =
                      MediaQuery.textScalerOf(context).scale(16) / 16;
                  const maxPanelWidth = 1080.0;
                  const columnGap = 24.0;
                  final columns =
                      tablet &&
                      constraints.maxWidth.clamp(0.0, maxPanelWidth) >=
                          2 * 384 * textScale.clamp(1.0, 2.0) + columnGap;
                  final content = Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      header,
                      if (columns)
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Expanded(child: updatePanel),
                            const SizedBox(width: columnGap),
                            Expanded(child: notes),
                          ],
                        )
                      else ...[
                        updatePanel,
                        const SizedBox(height: 12),
                        notes,
                      ],
                      footer,
                    ],
                  );
                  if (!columns) return LitchReadingPane(child: content);
                  return Align(
                    alignment: Alignment.topCenter,
                    child: SizedBox(width: maxPanelWidth, child: content),
                  );
                },
              ),
            ],
          ),
        ),
      ),
    );
  }
}
