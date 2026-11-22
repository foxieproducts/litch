import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/models/firmware_provenance.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/models/firmware_release_snapshot.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/firmware_artifact_validator.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/ota/litch_firmware_uploader.dart';
import 'package:litch_mobile/src/services/ota/ota_benchmark_runner.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/firmware_provenance_card.dart';
import 'package:litch_mobile/src/widgets/firmware_update_card.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/firmware_test_fixture.dart';
import 'health_render_capture.dart';
import 'support/memory_ota_target_store.dart';

class FakeFlutterReactiveBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}

class FakeFirmwareDownloadService extends FirmwareDownloadService {
  FakeFirmwareDownloadService();

  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    const String notes = 'Version: 1.2.3\n\nBoard software release notes.';
    final firmware = makeEsp32s3Firmware();
    return FirmwareReleaseSnapshot(
      release: release,
      manifest: makeFirmwareManifest(
        firmware,
        channel: release.channel.name,
        manifestUrl: release.manifestUrl,
        notesMarkdown: notes,
        version: '1.2.3',
      ),
      notesMarkdown: notes,
    );
  }
}

class FlakyFirmwareDownloadService extends FakeFirmwareDownloadService {
  int fetchCount = 0;

  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    fetchCount += 1;
    if (fetchCount == 1) {
      throw const FirmwareFetchException(
        'GET https://updates.example.invalid/manifest.json failed: 503',
      );
    }
    return super.fetchReleaseSnapshot(release);
  }
}

class ControlledFirmwareDownloadService extends FirmwareDownloadService {
  final Map<FirmwareReleaseChannel, Completer<FirmwareReleaseSnapshot>>
  _responses = <FirmwareReleaseChannel, Completer<FirmwareReleaseSnapshot>>{};
  final List<FirmwareReleaseChannel> requested = <FirmwareReleaseChannel>[];

  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) {
    requested.add(release.channel);
    return _responses
        .putIfAbsent(
          release.channel,
          () => Completer<FirmwareReleaseSnapshot>(),
        )
        .future;
  }

  void complete(
    FirmwareReleaseInfo release, {
    required String version,
    required String notes,
  }) {
    final firmware = makeEsp32s3Firmware(
      payloadBytes: <int>[version.codeUnitAt(0), 2, 3, 4],
    );
    _responses[release.channel]!.complete(
      FirmwareReleaseSnapshot(
        release: release,
        manifest: makeFirmwareManifest(
          firmware,
          channel: release.channel.name,
          manifestUrl: release.manifestUrl,
          notesMarkdown: notes,
          version: version,
        ),
        notesMarkdown: notes,
      ),
    );
  }
}

class RecordingFirmwareDownloadService extends FakeFirmwareDownloadService {
  int fetchCount = 0;
  int prepareCount = 0;
  FirmwareReleaseSnapshot? displayedSnapshot;
  FirmwareReleaseSnapshot? preparedSnapshot;

  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    fetchCount += 1;
    displayedSnapshot = await super.fetchReleaseSnapshot(release);
    return displayedSnapshot!;
  }

  @override
  Future<FirmwarePreparedFile> prepareFirmwareForUpdate(
    FirmwareReleaseSnapshot snapshot, {
    void Function(double progress)? onProgress,
  }) async {
    prepareCount += 1;
    preparedSnapshot = snapshot;
    throw const FirmwareFetchException('stop after snapshot capture');
  }
}

class OtaFirmwareDownloadService extends FakeFirmwareDownloadService {
  OtaFirmwareDownloadService({FirmwareReleaseInfo? release}) {
    release ??= FirmwareReleaseInfo.stable;
    _directory = Directory.systemTemp.createTempSync('litch-screen-ota-');
    final Uint8List firmware = makeEsp32s3Firmware();
    snapshot = FirmwareReleaseSnapshot(
      release: release,
      manifest: makeFirmwareManifest(
        firmware,
        channel: release.channel.name,
        manifestUrl: release.manifestUrl,
        notesMarkdown: 'Version: 1.2.3\n',
        version: '1.2.3',
      ),
      notesMarkdown: 'Version: 1.2.3\n',
    );
    _inspection = FirmwareArtifactValidator.inspectBytes(
      firmware,
      snapshot.manifest,
    );
    _file = File('${_directory.path}/firmware.bin')..writeAsBytesSync(firmware);
  }

  late final Directory _directory;
  late final File _file;
  late final FirmwareArtifactInspection _inspection;
  late final FirmwareReleaseSnapshot snapshot;
  int fetchCount = 0;
  Object? preparationError;

  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    fetchCount += 1;
    return snapshot;
  }

  @override
  Future<FirmwarePreparedFile> prepareFirmwareForUpdate(
    FirmwareReleaseSnapshot releaseSnapshot, {
    void Function(double progress)? onProgress,
  }) async {
    if (preparationError case final Object error) throw error;
    onProgress?.call(1);
    return FirmwarePreparedFile(
      file: _file,
      bytes: _file.lengthSync(),
      downloaded: false,
      inspection: _inspection,
    );
  }

  @override
  void dispose() {
    super.dispose();
    _directory.deleteSync(recursive: true);
  }
}

class FakeFirmwareBleService extends LitchBleService {
  FakeFirmwareBleService()
    : super(
        ble: FakeFlutterReactiveBle(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );

  static const updateCapabilities = LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3_3,
    capabilityMask: LitchDeviceCapabilities.firmwareUpdateBit |
        LitchDeviceCapabilities.firmwareDigestBit,
    explicitlyReported: true,
  );
  LitchDeviceCapabilities capabilities = updateCapabilities;
  String? boardVersion;

  @override
  bool get isConnected => true;

  @override
  String? get connectedId => 'board-1';

  @override
  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) async {
    return boardVersion;
  }

  @override
  Future<FirmwareProvenance> fetchFirmwareProvenance({
    required String deviceId,
  }) async {
    return isConnected
        ? FirmwareProvenance.custom('a' * 64)
        : FirmwareProvenance.offline;
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    return capabilities;
  }
}

class OtaFirmwareBleService extends FakeFirmwareBleService {
  final connectionEvents = StreamController<ConnectionStateUpdate>.broadcast(
    sync: true,
  );
  Completer<OtaUploadResult>? uploadCompleter;
  @override
  Stream<ConnectionStateUpdate> get connectionStream => connectionEvents.stream;
  @override
  Future<void> dispose() async {
    await connectionEvents.close();
    await super.dispose();
  }

  OtaUploadResult uploadResult = const OtaUploadPendingVerification(
    'Board rebooted after finalization; verification is pending.',
    connectionToken: 7,
  );
  OtaPostRebootVerification verification = const OtaPostRebootVerified();
  Completer<OtaPostRebootVerification>? verificationCompleter;
  String? currentConnectedId = 'board-1';
  bool connected = true;
  bool reconnectResult = true;
  bool keepConnectionAfterUpload = false;
  bool useProductionVerification = false;
  @override
  bool get pendingOtaRebootExpected =>
      pendingOtaVerification != null && uploadResult is OtaUploadFinalized;
  String? reconnectedDeviceId = 'board-1';
  String? initialInstalledVersion = '1.2.2';
  String? installedVersion = '1.2.3';
  int reconnectAttempts = 0;
  int capabilityRefreshes = 0;
  int versionReads = 0;
  int verificationAttempts = 0;
  String? verifiedDeviceId;
  int? verifiedConnectionToken;
  String? verifiedVersion;
  String? verifiedImageSha256;

  @override
  bool get isConnected => connected;

  @override
  String? get connectedId => currentConnectedId;

  @override
  Future<bool> connect(String deviceId) async {
    reconnectAttempts += 1;
    connected = reconnectResult;
    currentConnectedId = reconnectResult ? reconnectedDeviceId : null;
    return reconnectResult;
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    if (refresh) {
      capabilityRefreshes += 1;
    }
    return capabilities;
  }

  @override
  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) async {
    if (!connected || currentConnectedId != deviceId) return null;
    if (reconnectAttempts == 0) {
      return initialInstalledVersion;
    }
    versionReads += 1;
    return installedVersion;
  }

  @override
  Future<OtaUploadResult> uploadFirmwareFile({
    required String deviceId,
    required File firmwareFile,
    required FirmwareManifest manifest,
    void Function(double progress)? onProgress,
    void Function(String status)? onStatus,
  }) async {
    onProgress?.call(1);
    if (uploadResult is OtaUploadFinalized ||
        uploadResult is OtaUploadPendingVerification) {
      await rememberPendingOtaVerification(
        OtaVerificationTarget(
          deviceId: deviceId,
          uploadConnectionToken: 7,
          version: manifest.version,
          imageSha256: manifest.artifact.imageSha256,
          artifactSha256: manifest.artifact.sha256,
          attempt: const OtaAttemptIdentity(
            preBootNonce: '0000000000000002',
            attempt: 1,
            sourceSlot: 0,
            sourceSector: 0,
            sourceSequence: 23,
            targetSlot: 1,
            targetSector: 1,
            expectedSequence: 24,
          ),
        ),
      );
    }
    if (!keepConnectionAfterUpload) {
      connected = false;
      currentConnectedId = null;
    }
    return uploadCompleter?.future ?? uploadResult;
  }

  @override
  Future<OtaPostRebootVerification> verifyFirmwareAfterOta({
    required String deviceId,
    required int uploadConnectionToken,

    required String expectedVersion,
    required String expectedImageSha256,
  }) async {
    verificationAttempts += 1;
    verifiedDeviceId = deviceId;
    verifiedConnectionToken = uploadConnectionToken;
    verifiedVersion = expectedVersion;
    verifiedImageSha256 = expectedImageSha256;
    if (useProductionVerification) {
      return super.verifyFirmwareAfterOta(
        deviceId: deviceId,
        uploadConnectionToken: uploadConnectionToken,

        expectedVersion: expectedVersion,
        expectedImageSha256: expectedImageSha256,
      );
    }
    return verificationCompleter?.future ?? verification;
  }
}

Future<void> startOtaUpdate(
  WidgetTester tester,
  OtaFirmwareBleService bleService,
  OtaFirmwareDownloadService downloadService, {
  String buttonLabel = 'Update Board',
  bool settleAfterUpload = true,
}) async {
  await loadHealthReviewFonts(tester);
  await tester.binding.setSurfaceSize(const Size(390, 844));
  addTearDown(() => tester.binding.setSurfaceSize(null));
  await tester.pumpWidget(
    MaterialApp(
      theme: buildLitchTheme(),
      builder: (context, child) => RepaintBoundary(child: child!),
      home: Scaffold(
        body: FirmwareScreen(
          bleService: bleService,
          boardId: 'board-1',
          onRequestShowFirmwareTab: () {},
          onUpdateActivityChanged: (_) {},
          downloadService: downloadService,
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
  final FilledButton button = tester.widget<FilledButton>(
    find.widgetWithText(FilledButton, buttonLabel),
  );
  expect(button.onPressed, isNotNull);
  button.onPressed!();
  if (settleAfterUpload) {
    await tester.pumpAndSettle();
  } else {
    await tester.pump();
  }
}

void main() {
  setUp(() {
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.5.0',
      buildNumber: '1',
      buildSignature: '',
    );
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': false,
    });
  });

  testWidgets('OTA live region uses coarse milestones and terminal outcomes', (
    WidgetTester tester,
  ) async {
    final SemanticsHandle semantics = tester.ensureSemantics();

    Widget harness({
      bool downloading = false,
      double downloadProgress = 0,
      bool uploading = false,
      double uploadProgress = 0,
      bool verifying = false,
      required String status,
    }) {
      return MaterialApp(
        home: Scaffold(
          body: FirmwareUpdateCard(
            allowBeta: false,
            channelChangeDisabled: false,
            updateDisabled: true,
            supportsFirmwareUpdate: true,
            boardVersion: '1.2.2',
            latestVersion: '1.2.3',
            downloading: downloading,
            downloadProgress: downloadProgress,
            uploading: uploading,
            uploadProgress: uploadProgress,
            transferInProgress:
                uploading &&
                !verifying &&
                uploadProgress > 0 &&
                uploadProgress < 1,
            verifying: verifying,
            status: status,
            onAllowBetaChanged: (_) {},
            onUpdate: () {},
          ),
        ),
      );
    }

    final Finder liveRegion = find.byKey(
      const ValueKey<String>('firmware-update-status-live-region'),
    );
    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .01,
        status: 'Uploading update to board... 0%',
      ),
    );
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(label: 'Installing update', isLiveRegion: true),
    );
    expect(
      find.bySemanticsLabel('Uploading update to board... 0%'),
      findsNothing,
    );

    for (final double progress in <double>[.12, .24]) {
      await tester.pumpWidget(
        harness(
          uploading: true,
          uploadProgress: progress,
          status: 'Uploading update to board.',
        ),
      );
      expect(tester.getSemantics(liveRegion).label, 'Installing update');
      final exactLabel = 'Installing, ${(progress * 100).floor()}%';
      expect(find.text(exactLabel), findsOneWidget);
      final exactProgress = tester.getSemantics(
        find.byKey(const ValueKey<String>('firmware-transfer-exact-progress')),
      );
      expect(exactProgress.label, exactLabel);
      expect(exactProgress.flagsCollection.isLiveRegion, isFalse);
    }

    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .25,
        status: 'Uploading update to board.',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Installing update, 25 percent',
    );
    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .49,
        status: 'Uploading update to board.',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Installing update, 25 percent',
    );
    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .50,
        status: 'Uploading update to board.',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Installing update, 50 percent',
    );
    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .75,
        status: 'Uploading update to board.',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Installing update, 75 percent',
    );
    await tester.pumpWidget(
      harness(
        downloading: true,
        downloadProgress: 1,
        status: 'Download complete.',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Downloading update, 100 percent',
    );
    await tester.pumpWidget(
      harness(
        uploading: true,
        uploadProgress: .99,
        verifying: true,
        status: 'Waiting for the board to reboot and verify the update...',
      ),
    );
    expect(
      tester.getSemantics(liveRegion).label,
      'Waiting for the board to reboot and verify the update...',
    );

    await tester.pumpWidget(
      harness(status: 'Selected firmware verified on this board.'),
    );
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label: 'Selected firmware verified on this board.',
        isLiveRegion: true,
      ),
    );

    await tester.pumpWidget(
      harness(status: 'The original board did not reconnect.'),
    );
    expect(
      tester.getSemantics(liveRegion),
      isSemantics(
        label: 'The original board did not reconnect.',
        isLiveRegion: true,
      ),
    );

    // The guarded runner supplies these snapshots to the real Firmware page.
    // Presentation coverage bypasses the physical transfer and target guards.
    final bleService = FakeFirmwareBleService()..boardVersion = '1.2.2';
    final downloadService = FakeFirmwareDownloadService();
    addTearDown(bleService.dispose);
    OtaBenchmarkProgress? benchmark;
    late StateSetter rebuildBenchmark;
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: Scaffold(
          body: StatefulBuilder(
            builder: (context, setState) {
              rebuildBenchmark = setState;
              return FirmwareScreen(
                bleService: bleService,
                boardId: 'board-1',
                benchmarkProgress: benchmark,
                downloadService: downloadService,
                onRequestShowFirmwareTab: () {},
                onUpdateActivityChanged: (_) {},
              );
            },
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.byType(LinearProgressIndicator), findsNothing);

    Future<void> present(OtaBenchmarkProgress progress) async {
      rebuildBenchmark(() => benchmark = progress);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
    }

    await present(const OtaBenchmarkProgress(OtaBenchmarkPhase.preparing));
    expect(
      find.text('Starting connection test… Keep the app open.'),
      findsOneWidget,
    );
    expect(find.byType(LinearProgressIndicator), findsOneWidget);
    expect(
      tester
          .widget<LinearProgressIndicator>(find.byType(LinearProgressIndicator))
          .value,
      isNull,
      reason: 'The single existing status bar stays animated while measuring.',
    );
    for (final received in <int>[75000, 90000, 149000, 150000, 300000]) {
      await present(
        OtaBenchmarkProgress(
          OtaBenchmarkPhase.measuring,
          receivedBytes: received,
        ),
      );
      final exactStatus = tester.getSemantics(
        find.byKey(const ValueKey<String>('firmware-activity-exact-status')),
      );
      final status =
          'Measuring connection… ${received ~/ 1000} of 300 kB received. '
          'Keep the app open.';
      expect(find.text(status), findsOneWidget);
      expect(exactStatus.label, status);
      expect(exactStatus.flagsCollection.isLiveRegion, isFalse);
      final milestone = received * 4 ~/ 300000 * 75;
      expect(
        tester.getSemantics(liveRegion).label,
        'Measuring connection… $milestone of 300 kB received. Keep the app open.',
      );
      expect(find.byType(LinearProgressIndicator), findsOneWidget);
      final card = tester.widget<FirmwareUpdateCard>(
        find.byType(FirmwareUpdateCard),
      );
      expect(card.updateDisabled, isTrue);
      expect(card.channelChangeDisabled, isTrue);
      expect(find.textContaining('test complete.'), findsNothing);
    }
    await present(const OtaBenchmarkProgress(OtaBenchmarkPhase.ramComplete));
    await tester.pumpAndSettle();
    expect(
      find.text('Connection test complete. 300 kB received and verified.'),
      findsOneWidget,
    );
    expect(find.byType(LinearProgressIndicator), findsNothing);
    await tester.pump(const Duration(seconds: 10));
    expect(
      tester.getSemantics(liveRegion).label,
      'Connection test complete. 300 kB received and verified.',
      reason: 'The result remains after the activity stops.',
    );
    await present(const OtaBenchmarkProgress(OtaBenchmarkPhase.failed));
    expect(
      find.text('Test failed. Check the board connection.'),
      findsOneWidget,
    );
    expect(find.byType(LinearProgressIndicator), findsNothing);
    expect(tester.takeException(), isNull);
    semantics.dispose();
  });

  testWidgets(
    'metadata failure is calm and retries to healthy at 200 percent text',
    (WidgetTester tester) async {
      final SemanticsHandle semantics = tester.ensureSemantics();
      final FakeFirmwareBleService bleService = FakeFirmwareBleService()
        ..boardVersion = '1.2.2';
      final FlakyFirmwareDownloadService downloadService =
          FlakyFirmwareDownloadService();
      addTearDown(bleService.dispose);
      await tester.binding.setSurfaceSize(const Size(320, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));

      await tester.pumpWidget(
        MaterialApp(
          builder: (BuildContext context, Widget? child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: FirmwareScreen(
            bleService: bleService,
            boardId: 'board-1',
            onRequestShowFirmwareTab: () {},
            onUpdateActivityChanged: (_) {},
            downloadService: downloadService,
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(find.text('Could not load board update details.'), findsOneWidget);
      expect(find.textContaining('updates.example.invalid'), findsNothing);
      expect(
        find.byKey(const ValueKey<String>('retry-update-details')),
        findsOneWidget,
      );
      expect(
        tester
            .getSemantics(
              find.byKey(
                const ValueKey<String>('firmware-update-status-live-region'),
              ),
            )
            .label,
        'Could not load board update details.',
      );

      final Finder retry = find.byKey(
        const ValueKey<String>('retry-update-details'),
      );
      await tester.ensureVisible(retry);
      await tester.pumpAndSettle();
      await tester.tap(retry);
      await tester.pumpAndSettle();

      expect(downloadService.fetchCount, 2);
      expect(find.text('Version 1.2.3 is ready'), findsOneWidget);
      expect(
        find.byKey(const ValueKey<String>('retry-update-details')),
        findsNothing,
      );
      expect(tester.takeException(), isNull);
      semantics.dispose();
    },
  );

  testWidgets('firmware screen offers Update only after the connected board reports support', (
    WidgetTester tester,
  ) async {
    final OtaFirmwareBleService bleService = OtaFirmwareBleService()
      ..connected = false
      ..currentConnectedId = null;
    FirmwareUpdateAvailability? availability;

    await tester.pumpWidget(
      MaterialApp(
        home: FirmwareScreen(
          bleService: bleService,
          boardId: 'board-1',
          onRequestShowFirmwareTab: () {},
          onUpdateActivityChanged: (_) {},
          onAvailabilityChanged: (FirmwareUpdateAvailability value) {
            availability = value;
          },
          downloadService: FakeFirmwareDownloadService(),
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Installed version unknown'), findsOneWidget);
    expect(
      tester
          .widget<FirmwareProvenanceCard>(find.byType(FirmwareProvenanceCard))
          .provenance
          .state,
      FirmwareProvenanceState.offline,
    );
    expect(find.widgetWithText(FilledButton, 'Update Board'), findsNothing);
    bleService.capabilities = LitchDeviceCapabilities.unavailable;
    bleService.connected = true;
    bleService.currentConnectedId = 'board-1';
    bleService.connectionEvents.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Board details could not be read. Pull down to try again.'), findsOneWidget);
    expect(find.textContaining('manually over USB'), findsNothing);
    expect(find.widgetWithText(FilledButton, 'Update Board'), findsNothing);
    expect(availability?.updateAvailable, isFalse);

    bleService.capabilities = FakeFirmwareBleService.updateCapabilities;
    final refresh = tester.state<RefreshIndicatorState>(find.byType(RefreshIndicator));
    final refreshed = refresh.show();
    await tester.pumpAndSettle();
    await refreshed;
    final FirmwareProvenanceCard provenanceCard = tester.widget(
      find.byType(FirmwareProvenanceCard),
    );
    expect(provenanceCard.provenance.state, FirmwareProvenanceState.custom);
    expect(find.textContaining('rated or online play'), findsOneWidget);
    expect(find.textContaining('Makers rule.'), findsOneWidget);
    expect(find.text('UPDATE AVAILABLE'), findsOneWidget);
    final Text updateTitle = tester.widget<Text>(
      find.text('Version 1.2.3 is ready'),
    );
    expect(updateTitle.style?.fontSize, 24);
    final Text installedVersion = tester.widget<Text>(
      find.text('Installed version 1.2.2'),
    );
    expect(installedVersion.style?.fontSize, 14);
    expect(find.widgetWithText(FilledButton, 'Update Board'), findsOneWidget);
    expect(availability?.updateAvailable, isTrue);
    expect(find.byType(AlertDialog), findsNothing);

    bleService.connected = false;
    bleService.currentConnectedId = null;
    bleService.connectionEvents.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Installed version unknown'), findsOneWidget);
    expect(
      tester
          .widget<FirmwareProvenanceCard>(find.byType(FirmwareProvenanceCard))
          .provenance
          .state,
      FirmwareProvenanceState.offline,
    );
    expect(availability?.updateAvailable, isFalse);
  });

  testWidgets('same-version board offers deliberate exact-snapshot Reinstall', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final FakeFirmwareBleService bleService = FakeFirmwareBleService()
      ..boardVersion = '1.2.3';
    FirmwareUpdateAvailability? availability;
    final downloadService = RecordingFirmwareDownloadService();

    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        builder: (context, child) => RepaintBoundary(child: child!),
        home: Scaffold(
          body: FirmwareScreen(
            bleService: bleService,
            boardId: 'board-1',
            onRequestShowFirmwareTab: () {},
            onUpdateActivityChanged: (_) {},
            onAvailabilityChanged: (FirmwareUpdateAvailability value) {
              availability = value;
            },
            downloadService: downloadService,
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('CHECK FOR UPDATES'), findsNothing);
    final Text routineTitle = tester.widget<Text>(
      find.text('Check for updates'),
    );
    expect(routineTitle.style?.fontSize, 22);
    final Text installedVersion = tester.widget<Text>(
      find.text('Installed version 1.2.3'),
    );
    expect(installedVersion.style?.fontSize, 14);
    final FilledButton updateButton = tester.widget<FilledButton>(
      find.widgetWithText(FilledButton, 'Reinstall'),
    );
    expect(updateButton.onPressed, isNotNull);
    expect(availability?.updateAvailable, isFalse);
    expect(downloadService.prepareCount, 0);
    await captureHealthReview(tester, 'ota-same-version-reinstall-390');
    updateButton.onPressed!();
    updateButton.onPressed!();
    await tester.pumpAndSettle();
    expect(downloadService.prepareCount, 1);
    expect(
      identical(
        downloadService.preparedSnapshot,
        downloadService.displayedSnapshot,
      ),
      isTrue,
    );
  });

  testWidgets('pending OTA remount reloads beta notes after verification', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': true,
    });
    final completion = Completer<OtaPostRebootVerification>();
    final bleService = OtaFirmwareBleService()
      ..verificationCompleter = completion
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v2,
        capabilityMask:
            LitchDeviceCapabilities.firmwareDigestBit |
            LitchDeviceCapabilities.firmwareUpdateBit,
        explicitlyReported: true,
      );
    final target = OtaVerificationTarget(
      deviceId: 'board-1',
      uploadConnectionToken: 7,

      version: '1.2.3',
      imageSha256: 'a' * 64,
    );
    await bleService.rememberPendingOtaVerification(target);
    final downloadService = ControlledFirmwareDownloadService();
    final activities = <bool>[];
    addTearDown(bleService.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        builder: (context, child) => RepaintBoundary(
          child: MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
        ),
        home: Scaffold(
          body: FirmwareScreen(
            bleService: bleService,
            boardId: 'board-1',
            onRequestShowFirmwareTab: () {},
            onUpdateActivityChanged: activities.add,
            downloadService: downloadService,
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(bleService.verificationAttempts, 1);
    expect(
      downloadService.requested,
      isEmpty,
      reason: 'Verify the exact pending transaction before ordinary metadata.',
    );
    completion.complete(
      const OtaPostRebootPending(
        'The reconnected board did not provide a firmware version yet.',
      ),
    );
    await tester.pumpAndSettle();
    expect(downloadService.requested, [FirmwareReleaseChannel.beta]);
    expect(
      activities.last,
      isFalse,
      reason: 'Notes loading is not modal post-reboot verification.',
    );
    final secondVerification = Completer<OtaPostRebootVerification>();
    bleService.verificationCompleter = secondVerification;
    final verifyButton = tester.widget<OutlinedButton>(
      find.widgetWithText(OutlinedButton, 'Verify installed update'),
    );
    expect(verifyButton.onPressed, isNotNull);
    verifyButton.onPressed!();
    await tester.pumpAndSettle();
    expect(bleService.verificationAttempts, 2);
    expect(activities.last, isTrue);
    downloadService.complete(
      FirmwareReleaseInfo.beta,
      version: '1.2.3',
      notes: 'BETA RECOVERY NOTES',
    );
    await tester.pumpAndSettle();
    expect(
      activities.last,
      isTrue,
      reason: 'Older notes completion must not release the active verifier.',
    );
    expect(
      find.textContaining('Waiting for the board to reboot'),
      findsOneWidget,
    );
    secondVerification.complete(
      const OtaPostRebootPending(
        'The reconnected board did not provide a firmware version yet.',
      ),
    );
    await tester.pumpAndSettle();
    expect(activities.last, isFalse);
    expect(find.text('BETA RECOVERY NOTES'), findsOneWidget);
    expect(
      find.textContaining('did not provide a firmware version yet'),
      findsOneWidget,
    );
    expect(find.text('Installed version unknown'), findsOneWidget);
    expect(identical(bleService.pendingOtaVerification, target), isTrue);
    expect(
      bleService.versionReads,
      0,
      reason: 'Notes loading must not start an unrelated board identity read.',
    );
    final card = tester.widget<FirmwareUpdateCard>(
      find.byType(FirmwareUpdateCard),
    );
    expect(card.updateDisabled, isTrue);
    expect(card.channelChangeDisabled, isTrue);
    await captureHealthReview(tester, 'ota-pending-top-320-200');
    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('firmware-update-status-live-region')),
    );
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'ota-pending-status-320-200');
    final refresh = tester.widget<RefreshIndicator>(
      find.byType(RefreshIndicator),
    );
    final refreshing = refresh.onRefresh();
    await tester.pumpAndSettle();
    await refreshing;
    expect(
      find.textContaining('did not provide a firmware version yet'),
      findsOneWidget,
    );
    expect(bleService.versionReads, 0);
  });

  testWidgets(
    'V1 ESP32-C3 shows manual USB update guidance',
    (WidgetTester tester) async {
      final FakeFirmwareBleService bleService = FakeFirmwareBleService()
        ..capabilities = const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v1,
          capabilityMask: 15,
          explicitlyReported: true,
        );

      await tester.pumpWidget(
        MaterialApp(
          home: FirmwareScreen(
            bleService: bleService,
            boardId: 'board-1',
            onRequestShowFirmwareTab: () {},
            onUpdateActivityChanged: (_) {},
            downloadService: FakeFirmwareDownloadService(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final FirmwareUpdateCard updateCard = tester.widget(
        find.byType(FirmwareUpdateCard),
      );
      expect(updateCard.supportsFirmwareUpdate, isFalse);
      await tester.drag(find.byType(ListView), const Offset(0, -400));
      await tester.pumpAndSettle();

      expect(find.textContaining('updated manually over USB'), findsWidgets);
      expect(find.widgetWithText(FilledButton, 'Update Board'), findsNothing);
      expect(find.textContaining('V1 ESP32-C3'), findsWidgets);
    },
  );

  testWidgets('new channel wins when old release details finish late', (
    WidgetTester tester,
  ) async {
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.5.0',
      buildNumber: '1',
      buildSignature: '',
    );
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': false,
    });
    final ControlledFirmwareDownloadService downloadService =
        ControlledFirmwareDownloadService();

    await tester.pumpWidget(
      MaterialApp(
        home: FirmwareScreen(
          bleService: FakeFirmwareBleService(),
          boardId: 'board-1',
          onRequestShowFirmwareTab: () {},
          onUpdateActivityChanged: (_) {},
          downloadService: downloadService,
        ),
      ),
    );
    for (
      int attempt = 0;
      attempt < 20 && downloadService.requested.isEmpty;
      ++attempt
    ) {
      await tester.pump(const Duration(milliseconds: 50));
    }
    expect(downloadService.requested, <FirmwareReleaseChannel>[
      FirmwareReleaseChannel.stable,
    ]);

    // The restored-intent owner may finish after the last frame. Rebuild its
    // enabled controls while the old metadata request remains pending.
    await tester.pump();
    final CheckboxListTile channelToggle = tester.widget<CheckboxListTile>(
      find.byType(CheckboxListTile),
    );
    expect(channelToggle.onChanged, isNotNull);
    channelToggle.onChanged!(true);
    for (
      int attempt = 0;
      attempt < 20 &&
          !downloadService.requested.contains(FirmwareReleaseChannel.beta);
      ++attempt
    ) {
      await tester.pump(const Duration(milliseconds: 50));
    }

    expect(
      downloadService.requested,
      contains(FirmwareReleaseChannel.beta),
      reason: 'changing channel must start a distinct metadata transaction',
    );
    downloadService.complete(
      FirmwareReleaseInfo.beta,
      version: '2.0.0-beta.1',
      notes: 'BETA SNAPSHOT',
    );
    await tester.pump();
    expect(find.text('BETA SNAPSHOT'), findsOneWidget);
    expect(find.text('Latest version: 2.0.0-beta.1'), findsOneWidget);

    downloadService.complete(
      FirmwareReleaseInfo.stable,
      version: '1.0.0',
      notes: 'STALE STABLE SNAPSHOT',
    );
    await tester.pump();

    expect(find.text('BETA SNAPSHOT'), findsOneWidget);
    expect(find.text('STALE STABLE SNAPSHOT'), findsNothing);
    expect(find.text('Latest version: 2.0.0-beta.1'), findsOneWidget);
  });

  testWidgets('update uses the exact snapshot whose notes were displayed', (
    WidgetTester tester,
  ) async {
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.5.0',
      buildNumber: '1',
      buildSignature: '',
    );
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': false,
    });
    final RecordingFirmwareDownloadService downloadService =
        RecordingFirmwareDownloadService();
    final FakeFirmwareBleService bleService = FakeFirmwareBleService()
      ..boardVersion = '1.2.2';

    await tester.pumpWidget(
      MaterialApp(
        home: FirmwareScreen(
          bleService: bleService,
          boardId: 'board-1',
          onRequestShowFirmwareTab: () {},
          onUpdateActivityChanged: (_) {},
          downloadService: downloadService,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.textContaining('Board software release notes.'), findsOneWidget);

    await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
    await tester.pumpAndSettle();

    expect(downloadService.fetchCount, 1);
    expect(
      identical(
        downloadService.preparedSnapshot,
        downloadService.displayedSnapshot,
      ),
      isTrue,
    );
  });

  for (final wrapped in [false, true]) {
    testWidgets(
      'invalid firmware package gives a useful action wrapped=$wrapped',
      (tester) async {
        const detail = FirmwareArtifactValidationException(
          'Firmware updates require SDK-only boot policy epoch 2.',
        );
        final download = OtaFirmwareDownloadService()
          ..preparationError = wrapped
              ? const FirmwareFetchException(
                  'Download rejected: SDK-only boot policy epoch 2.',
                  cause: detail,
                )
              : detail;
        final service = OtaFirmwareBleService();
        addTearDown(service.dispose);
        await startOtaUpdate(tester, service, download);
        expect(service.pendingOtaVerification, isNull);
        expect(service.verificationAttempts, 0);
        expect(
          find.textContaining(
            'This firmware package cannot be installed safely.',
          ),
          findsOneWidget,
        );
        expect(find.textContaining('SDK-only'), findsNothing);
        expect(find.textContaining('Restart the app'), findsNothing);
        expect(
          find.text('Selected firmware verified on this board.'),
          findsNothing,
        );
      },
    );
  }

  testWidgets('OTA reconnects the same board and verifies the exact release', (
    WidgetTester tester,
  ) async {
    final OtaFirmwareDownloadService downloadService =
        OtaFirmwareDownloadService();
    final OtaFirmwareBleService bleService = OtaFirmwareBleService()
      ..installedVersion = null
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v2,
        capabilityMask:
            LitchDeviceCapabilities.firmwareDigestBit |
            LitchDeviceCapabilities.firmwareUpdateBit,
        explicitlyReported: true,
      );

    await startOtaUpdate(tester, bleService, downloadService);

    expect(bleService.reconnectAttempts, 1);
    expect(bleService.capabilityRefreshes, 1);
    expect(bleService.verificationAttempts, 1);
    expect(bleService.verifiedDeviceId, 'board-1');
    expect(bleService.verifiedConnectionToken, 7);
    expect(
      bleService.verifiedVersion,
      downloadService.snapshot.manifest.version,
    );
    expect(
      bleService.verifiedImageSha256,
      downloadService.snapshot.manifest.artifact.imageSha256,
    );
    expect(bleService.pendingOtaVerification, isNull);
    expect(
      find.text('Selected firmware verified on this board.'),
      findsOneWidget,
    );
    expect(
      downloadService.fetchCount,
      2,
      reason: 'Completion refreshes the selected release notes automatically.',
    );
    expect(find.text('Installed version 1.2.3'), findsOneWidget);
    expect(
      bleService.versionReads,
      0,
      reason:
          'Do not replace a verified version with a redundant empty reread.',
    );
    await captureHealthReview(tester, 'ota-verified-390');
    await tester.ensureVisible(find.text('Version: 1.2.3\n'));
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'ota-verified-notes-390');
  });

  testWidgets(
    'lost final reply verifies selected same image without reboot claim',
    (tester) async {
      // The production service transfer/verification seam is covered by
      // ota_reconnect_service_test; here its typed results drive the actual UI.
      final service = OtaFirmwareBleService()
        ..initialInstalledVersion = '1.2.3'
        ..uploadResult = const OtaUploadPendingVerification(
          'Board disconnected after the update was sent; verification is pending.',
          connectionToken: 7,
        );
      addTearDown(service.dispose);
      await startOtaUpdate(
        tester,
        service,
        OtaFirmwareDownloadService(),
        buttonLabel: 'Reinstall',
      );
      expect(service.reconnectAttempts, 1);
      expect(
        find.text('Selected firmware verified on this board.'),
        findsOneWidget,
      );
      expect(find.textContaining('verified after reboot'), findsNothing);
      expect(service.pendingOtaVerification, isNull);
      expect(service.uploadResult, isA<OtaUploadPendingVerification>());
    },
  );

  testWidgets(
    'failed transfer never creates verification or clears it as success',
    (tester) async {
      final service = OtaFirmwareBleService()
        ..initialInstalledVersion = '1.2.3'
        ..uploadResult = const OtaUploadFailed(
          'Transfer interrupted before final receipt.',
        );
      addTearDown(service.dispose);
      await startOtaUpdate(
        tester,
        service,
        OtaFirmwareDownloadService(),
        buttonLabel: 'Reinstall',
      );
      expect(service.verificationAttempts, 0);
      expect(service.pendingOtaVerification, isNull);
      expect(
        find.text('Selected firmware verified on this board.'),
        findsNothing,
      );
      expect(find.textContaining('Transfer interrupted'), findsOneWidget);
    },
  );

  for (final beta in [false, true]) {
    testWidgets(
      'accepted different image permits retry after remount beta=$beta',
      (tester) async {
        SharedPreferences.setMockInitialValues({'allow_beta_updates': beta});
        final service = OtaFirmwareBleService()
          ..verification = OtaPostRebootDifferentImage(
            version: '1.2.2',
            imageSha256: 'b' * 64,
          );
        addTearDown(service.dispose);
        await startOtaUpdate(
          tester,
          service,
          OtaFirmwareDownloadService(
            release: beta
                ? FirmwareReleaseInfo.beta
                : FirmwareReleaseInfo.stable,
          ),
        );
        expect(service.pendingOtaVerification, isNull);
        expect(service.failedOtaVerification?.result.version, '1.2.2');
        expect(
          find.textContaining('You can try the update again'),
          findsOneWidget,
        );
        expect(
          tester
              .widget<FirmwareUpdateCard>(find.byType(FirmwareUpdateCard))
              .updateDisabled,
          isFalse,
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: Scaffold(
              body: FirmwareScreen(
                bleService: service,
                boardId: 'board-1',
                onRequestShowFirmwareTab: () {},
                onUpdateActivityChanged: (_) {},
                downloadService: OtaFirmwareDownloadService(
                  release: beta
                      ? FirmwareReleaseInfo.beta
                      : FirmwareReleaseInfo.stable,
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          find.textContaining('You can try the update again'),
          findsOneWidget,
        );
        expect(find.text('Installed version 1.2.2'), findsOneWidget);
        final card = tester.widget<FirmwareUpdateCard>(
          find.byType(FirmwareUpdateCard),
        );
        expect(card.updateDisabled, isFalse);
        expect(card.channelChangeDisabled, isFalse);
        final attempts = service.verificationAttempts;
        service.verification = const OtaPostRebootPending(
          'Waiting for new boot',
        );
        tester
            .widget<FilledButton>(
              find.widgetWithText(FilledButton, 'Update Board'),
            )
            .onPressed!();
        await tester.pumpAndSettle();
        expect(service.verificationAttempts, attempts + 1);
        expect(service.pendingOtaVerification, isNotNull);
        expect(service.failedOtaVerification, isNull);
      },
    );
  }

  testWidgets('wrong-board reconnect stays explicit and retryable', (
    WidgetTester tester,
  ) async {
    final OtaFirmwareDownloadService downloadService =
        OtaFirmwareDownloadService();
    final OtaFirmwareBleService bleService = OtaFirmwareBleService()
      ..reconnectedDeviceId = 'board-2';

    await startOtaUpdate(tester, bleService, downloadService);

    expect(bleService.reconnectAttempts, 1);
    expect(bleService.capabilityRefreshes, 0);
    expect(bleService.verificationAttempts, 0);
    expect(
      find.textContaining('original board did not reconnect'),
      findsOneWidget,
    );
    expect(
      bleService.pendingOtaVerification,
      isNotNull,
      reason: 'failed verification must retain the exact recovery target',
    );
    await tester.drag(find.byType(ListView), const Offset(0, -300));
    await tester.pumpAndSettle();
    expect(
      find.widgetWithText(OutlinedButton, 'Verify installed update'),
      findsOneWidget,
    );
    final FilledButton updateButton = tester.widget<FilledButton>(
      find.widgetWithText(FilledButton, 'Update Board'),
    );
    expect(
      updateButton.onPressed,
      isNull,
      reason: 'pending verification must leave one OTA recovery action',
    );
    final CheckboxListTile betaUpdates = tester.widget<CheckboxListTile>(
      find.widgetWithText(CheckboxListTile, 'Allow beta updates'),
    );
    expect(
      betaUpdates.onChanged,
      isNull,
      reason: 'pending verification must lock the selected release channel',
    );
  });

  testWidgets('persistence failures show their exact recovery action', (
    WidgetTester tester,
  ) async {
    for (final MapEntry<String, String> scenario in <String, String>{
      'persist_failed':
          'The board couldn’t save its current state. Keep it powered on and retry. If this repeats, send a support report.',
      'persist_timeout':
          'The board was still saving when the update timed out. Keep it powered on for a moment, then retry.',
      'persist_not_ready':
          'Finish the current board action, such as promotion or undo, then retry the update.',
    }.entries) {
      final OtaFirmwareDownloadService downloadService =
          OtaFirmwareDownloadService();
      final OtaFirmwareBleService bleService = OtaFirmwareBleService()
        ..uploadResult = OtaUploadFailed(
          'Board reported OTA error (${scenario.key}).',
          detail: scenario.key,
        );
      addTearDown(bleService.dispose);
      await startOtaUpdate(tester, bleService, downloadService);
      expect(find.text(scenario.value), findsOneWidget);
      expect(find.textContaining('Check the connection'), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
    }
  });

  testWidgets(
    'board replacement before verification delivery cannot clear recovery',
    (tester) async {
      final verification = Completer<OtaPostRebootVerification>();
      final bleService = OtaFirmwareBleService()
        ..verificationCompleter = verification;
      addTearDown(bleService.dispose);
      await startOtaUpdate(tester, bleService, OtaFirmwareDownloadService());
      expect(bleService.verificationAttempts, 1);
      bleService.currentConnectedId = 'board-2';
      verification.complete(const OtaPostRebootVerified());
      await tester.pumpAndSettle();
      expect(bleService.pendingOtaVerification, isNotNull);
      expect(
        find.text('Selected firmware verified on this board.'),
        findsNothing,
      );
    },
  );

  testWidgets(
    'Reinstall with capability silence and no reboot stays unverified',
    (WidgetTester tester) async {
      final OtaFirmwareDownloadService downloadService =
          OtaFirmwareDownloadService();
      final OtaFirmwareBleService bleService = OtaFirmwareBleService()
        ..initialInstalledVersion = '1.2.3'
        ..keepConnectionAfterUpload = true
        ..useProductionVerification = true
        ..uploadResult = const OtaUploadFinalized(
          'Finalized.',
          connectionToken: 7,
        );
      addTearDown(bleService.dispose);

      await startOtaUpdate(
        tester,
        bleService,
        downloadService,
        buttonLabel: 'Reinstall',
        settleAfterUpload: false,
      );
      await tester.pump(const Duration(seconds: 1));
      expect(
        find.textContaining('Waiting for the board to reboot'),
        findsOneWidget,
      );
      expect(find.textContaining('board connection changed'), findsNothing);
      await tester.pump(const Duration(seconds: 21));
      await tester.pumpAndSettle();

      expect(bleService.capabilityRefreshes, 0);
      expect(bleService.reconnectAttempts, 0);
      expect(bleService.verificationAttempts, 1);
      expect(bleService.pendingOtaVerification, isNotNull);
      expect(find.textContaining('Selected firmware verified'), findsNothing);
      expect(
        tester
            .widget<FirmwareUpdateCard>(find.byType(FirmwareUpdateCard))
            .uploadProgress,
        lessThan(1),
      );
      expect(
        find.textContaining('Waiting for this board to reconnect'),
        findsOneWidget,
      );
      final verify = find.widgetWithText(
        OutlinedButton,
        'Verify installed update',
      );
      await tester.ensureVisible(verify);
      await tester.pumpAndSettle();
      await tester.tap(verify);
      await tester.pump();
      expect(
        find.textContaining('Waiting for the board to reboot'),
        findsOneWidget,
      );
      expect(bleService.capabilityRefreshes, 0);
      expect(bleService.verificationAttempts, 2);
      await tester.pump(const Duration(seconds: 21));
      await tester.pumpAndSettle();
    },
  );
  testWidgets('upload completion cannot release reconnect-owned verification', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final upload = Completer<OtaUploadResult>();
    final verify = Completer<OtaPostRebootVerification>();
    final service = OtaFirmwareBleService()
      ..initialInstalledVersion = '1.2.2'
      ..uploadCompleter = upload
      ..verificationCompleter = verify
      ..capabilities = const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask:
            LitchDeviceCapabilities.firmwareDigestBit |
            LitchDeviceCapabilities.firmwareUpdateBit,
        explicitlyReported: true,
      );
    final download = OtaFirmwareDownloadService();
    final activities = <bool>[];
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: Scaffold(
          body: FirmwareScreen(
            bleService: service,
            boardId: 'board-1',
            downloadService: download,
            onRequestShowFirmwareTab: () {},
            onUpdateActivityChanged: activities.add,
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
    await tester.pumpAndSettle();
    expect(service.pendingOtaVerification, isNotNull);
    service.connected = true;
    service.currentConnectedId = 'board-1';
    service.connectionEvents.add(
      const ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await tester.pump();
    expect(service.verificationAttempts, 1);
    expect(activities.last, isTrue);
    upload.complete(service.uploadResult);
    await tester.pump();
    await tester.pump();
    expect(
      activities.last,
      isTrue,
      reason: 'Verification still owns the shell',
    );
    verify.complete(const OtaPostRebootPending('Keep this update pending.'));
    await tester.pumpAndSettle();
    expect(activities.last, isFalse);
    expect(service.pendingOtaVerification, isNotNull);
  });
}
