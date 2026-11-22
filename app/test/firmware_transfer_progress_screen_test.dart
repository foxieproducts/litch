import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/models/firmware_release_snapshot.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/ota/litch_firmware_uploader.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/firmware_update_card.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'firmware_screen_test.dart' as fixtures;
import 'health_render_capture.dart';

class _ProgressService extends fixtures.OtaFirmwareBleService {
  Completer<void> release = Completer<void>();
  void Function(double)? progress;
  int uploadCalls = 0;

  @override
  Future<OtaUploadResult> uploadFirmwareFile({
    required String deviceId,
    required File firmwareFile,
    required FirmwareManifest manifest,
    void Function(double)? onProgress,
    void Function(String)? onStatus,
  }) async {
    uploadCalls += 1;
    progress = onProgress;
    await release.future;
    return super.uploadFirmwareFile(
      deviceId: deviceId,
      firmwareFile: firmwareFile,
      manifest: manifest,
      onProgress: onProgress,
      onStatus: onStatus,
    );
  }
}

class _ProgressDownload extends fixtures.OtaFirmwareDownloadService {
  final release = Completer<void>();

  @override
  Future<FirmwarePreparedFile> prepareFirmwareForUpdate(
    FirmwareReleaseSnapshot snapshot, {
    void Function(double)? onProgress,
  }) async {
    onProgress?.call(.5);
    await release.future;
    return super.prepareFirmwareForUpdate(snapshot, onProgress: onProgress);
  }
}

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.9.5',
      buildNumber: '22',
      buildSignature: '',
    );
  });

  testWidgets(
    'actual screen excludes download/handshake, expires stalled ETA, resets and verifies',
    (tester) async {
      final service = _ProgressService()
        ..capabilities = const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.firmwareDigestBit |
              LitchDeviceCapabilities.firmwareUpdateBit,
          explicitlyReported: true,
        );
      final download = _ProgressDownload();
      addTearDown(service.dispose);
      var elapsed = Duration.zero;
      final semantics = tester.ensureSemantics();
      try {
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: Scaffold(
              body: FirmwareScreen(
                bleService: service,
                boardId: 'board-1',
                downloadService: download,
                transferElapsed: () => elapsed,
                onRequestShowFirmwareTab: () {},
                onUpdateActivityChanged: (_) {},
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.ensureVisible(
          find.widgetWithText(FilledButton, 'Update Board'),
        );
        await tester.pump();
        await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
        await tester.pump();
        final warning = find.byKey(
          const ValueKey<String>('firmware-transfer-open-warning'),
        );
        FirmwareUpdateCard card() =>
            tester.widget(find.byType(FirmwareUpdateCard));
        expect(card().downloading, isTrue);
        expect(warning, findsNothing);
        elapsed = const Duration(minutes: 1);
        download.release.complete();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        expect(card().uploading, isTrue);
        expect(service.uploadCalls, 1);
        expect(card().transferInProgress, isFalse);
        expect(card().estimatedRemaining, isNull);
        expect(warning, findsNothing);
        // One minute spent before any bytes must never enter the rate.
        service.progress!(.125);
        await tester.pump();
        expect(warning, findsOneWidget);
        expect(card().estimatedRemaining, isNull);
        elapsed += const Duration(seconds: 1);
        service.progress!(.1875);
        await tester.pump();
        expect(card().estimatedRemaining, isNull);
        elapsed += const Duration(seconds: 1);
        service.progress!(.25);
        await tester.pump();
        expect(card().estimatedRemaining, const Duration(seconds: 12));
        final live = tester.getSemantics(
          find.byKey(
            const ValueKey<String>('firmware-update-status-live-region'),
          ),
        );
        expect(live.label, 'Installing update, 25 percent');
        expect(live.label, isNot(contains('remaining')));
        expect(
          tester.getSemantics(warning).label,
          'Keep app open while updating',
        );
        expect(
          find.bySemanticsLabel(RegExp('Estimated time remaining, 00:12')),
          findsOneWidget,
        );
        final exact = find.byKey(
          const ValueKey<String>('firmware-transfer-exact-progress'),
        );
        service.progress!(.37);
        await tester.pump();
        expect(
          tester.getSemantics(exact).flagsCollection.isLiveRegion,
          isFalse,
        );
        expect(
          tester
              .getSemantics(
                find.byKey(
                  const ValueKey<String>('firmware-update-status-live-region'),
                ),
              )
              .label,
          'Installing update, 25 percent',
        );
        final etaBefore = tester
            .getSemantics(exact)
            .label
            .split('Estimated time remaining, ')
            .last;
        elapsed += const Duration(seconds: 1);
        service.progress!(.38);
        await tester.pump();
        final etaAfter = tester
            .getSemantics(exact)
            .label
            .split('Estimated time remaining, ')
            .last;
        expect(etaAfter, isNot(etaBefore));
        expect(
          tester
              .getSemantics(
                find.byKey(
                  const ValueKey<String>('firmware-update-status-live-region'),
                ),
              )
              .label,
          'Installing update, 25 percent',
        );
        service.progress!(.5);
        await tester.pump();
        expect(
          tester
              .getSemantics(
                find.byKey(
                  const ValueKey<String>('firmware-update-status-live-region'),
                ),
              )
              .label,
          'Installing update, 50 percent',
        );
        elapsed += const Duration(seconds: 4);
        await tester.pump(const Duration(seconds: 4));
        expect(card().estimatedRemaining, isNull);
        expect(find.textContaining('eta:'), findsNothing);
        expect(warning, findsOneWidget);
        service.progress!(.3);
        await tester.pump();
        expect(card().estimatedRemaining, isNull);
        elapsed += const Duration(seconds: 1);
        service.progress!(.35);
        await tester.pump();
        elapsed += const Duration(seconds: 1);
        service.progress!(.4);
        await tester.pump();
        expect(card().estimatedRemaining, isNotNull);
        for (final state in [
          AppLifecycleState.inactive,
          AppLifecycleState.hidden,
          AppLifecycleState.paused,
          AppLifecycleState.hidden,
          AppLifecycleState.inactive,
          AppLifecycleState.resumed,
        ]) {
          tester.binding.handleAppLifecycleStateChanged(state);
        }
        await tester.pump();
        expect(card().estimatedRemaining, isNull);
        expect(warning, findsOneWidget);
        for (final progress in [.45, .5, .55]) {
          elapsed += const Duration(seconds: 1);
          service.progress!(progress);
          await tester.pump();
        }
        expect(card().estimatedRemaining, isNotNull);
        service.progress!(.1);
        await tester.pump();
        expect(card().estimatedRemaining, isNull);
        service.uploadResult = const OtaUploadFailed('Canceled transfer.');
        service.release.complete();
        await tester.pumpAndSettle();
        expect(warning, findsNothing);
        expect(card().estimatedRemaining, isNull);
        expect(card().supportsFirmwareUpdate, isFalse);
        // A new attempt cannot inherit a previous rate or warning.
        service.connected = true;
        service.currentConnectedId = 'board-1';
        service.connectionEvents.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        // The reconnect refreshes capabilities before rebuilding the Update action.
        await tester.pumpAndSettle();
        expect(card().updateDisabled, isFalse);
        service.release = Completer<void>();
        service.verificationCompleter = Completer<OtaPostRebootVerification>();
        service.uploadResult = const OtaUploadPendingVerification(
          'Pending',
          connectionToken: 7,
        );
        await tester.ensureVisible(
          find.widgetWithText(FilledButton, 'Update Board'),
        );
        await tester.pump();
        await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        expect(warning, findsNothing);
        expect(service.uploadCalls, 2);
        service.progress!(.2);
        await tester.pump();
        expect(card().estimatedRemaining, isNull);
        service.progress!(1);
        await tester.pump();
        expect(warning, findsNothing);
        expect(find.textContaining('eta:'), findsNothing);
        service.release.complete();
        await tester.pumpAndSettle();
        expect(card().verifying, isTrue);
        expect(warning, findsNothing);
        service.verificationCompleter!.complete(const OtaPostRebootVerified());
        await tester.pumpAndSettle();
        expect(warning, findsNothing);
        expect(
          find.text('Selected firmware verified on this board.'),
          findsOneWidget,
        );
      } finally {
        semantics.dispose();
      }
    },
  );

  for (final dark in [false, true]) {
    for (final scale in [1.0, 2.0]) {
      for (final pose in ['portrait', 'landscape-left', 'landscape-right']) {
        testWidgets(
          'Update entry reveals progress once $pose scale=$scale dark=$dark',
          (tester) async {
            await loadHealthReviewFonts(tester, useSdkFonts: true);
            final service = _ProgressService()
              ..capabilities = const LitchDeviceCapabilities(
                contractVersion: 3,
                hardwareRevision: LitchHardwareRevision.v3,
                capabilityMask:
                    LitchDeviceCapabilities.firmwareDigestBit |
                    LitchDeviceCapabilities.firmwareUpdateBit,
                explicitlyReported: true,
              );
            final download = _ProgressDownload()..release.complete();
            addTearDown(service.dispose);
            addTearDown(() => tester.binding.setSurfaceSize(null));
            var elapsed = Duration.zero;
            final portrait = pose == 'portrait';
            final insets = portrait
                ? const EdgeInsets.fromLTRB(0, 44, 0, 34)
                : pose == 'landscape-left'
                ? const EdgeInsets.fromLTRB(44, 0, 0, 21)
                : const EdgeInsets.fromLTRB(0, 0, 44, 21);
            final size = portrait ? const Size(390, 844) : const Size(844, 390);
            final theme = dark ? buildLitchDarkTheme() : buildLitchTheme();
            await tester.binding.setSurfaceSize(size);
            await tester.pumpWidget(
              MaterialApp(
                theme: theme,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context).copyWith(
                    textScaler: TextScaler.linear(scale),
                    padding: insets,
                    viewPadding: insets,
                  ),
                  child: child!,
                ),
                home: Scaffold(
                  body: SafeArea(
                    left: false,
                    right: false,
                    bottom: false,
                    child: FirmwareScreen(
                      bleService: service,
                      boardId: 'board-1',
                      downloadService: download,
                      transferElapsed: () => elapsed,
                      onRequestShowFirmwareTab: () {},
                      onUpdateActivityChanged: (_) {},
                    ),
                  ),
                ),
              ),
            );
            await tester.pumpAndSettle();
            final update = find.widgetWithText(FilledButton, 'Update Board');
            await Scrollable.ensureVisible(
              tester.element(update),
              alignment: 1,
            );
            await tester.pump();
            final entryPosition = Scrollable.of(
              tester.element(update),
            ).position;
            final entryOffset = entryPosition.pixels;
            final naturallyVisible = portrait && scale == 1;
            if (naturallyVisible) {
              // At normal portrait size the genuine button is already above
              // mid-screen at minimum scroll; do not fabricate extra content.
              expect(entryOffset, entryPosition.minScrollExtent);
            } else {
              expect(
                tester.getRect(update).center.dy,
                greaterThan(size.height * .7),
              );
            }
            await tester.tap(update);
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 100));
            expect(service.uploadCalls, 1);
            final focusBefore = FocusManager.instance.primaryFocus;
            service.progress!(.125);
            await tester.pump();
            await tester.pump();
            final warning = find.text('Keep app open while updating');
            final color = tester.widget<Text>(warning).style!.color!;
            final fg = color.computeLuminance(),
                bg = theme.colorScheme.surfaceContainerLow.computeLuminance();
            expect(
              fg > bg ? (fg + .05) / (bg + .05) : (bg + .05) / (fg + .05),
              greaterThanOrEqualTo(4.5),
            );
            final scroll = Scrollable.of(tester.element(warning)).position;
            final revealedOffset = scroll.pixels;
            if (naturallyVisible) expect(revealedOffset, entryOffset);
            void visible(Finder content) {
              final rect = tester.getRect(content);
              expect(rect.left, greaterThanOrEqualTo(insets.left));
              expect(rect.right, lessThanOrEqualTo(size.width - insets.right));
              expect(rect.top, greaterThanOrEqualTo(insets.top));
              expect(
                rect.bottom,
                lessThanOrEqualTo(size.height - insets.bottom),
              );
            }

            final exact = find.byKey(
              const ValueKey<String>('firmware-transfer-exact-progress'),
            );
            visible(exact);
            visible(warning);
            elapsed = const Duration(seconds: 1);
            service.progress!(.1875);
            await tester.pump();
            elapsed = const Duration(seconds: 2);
            service.progress!(.37);
            await tester.pump();
            await tester.pump();
            visible(exact);
            visible(warning);
            expect(scroll.pixels, revealedOffset);
            expect(FocusManager.instance.primaryFocus, same(focusBefore));
            expect(tester.takeException(), isNull);
            await captureHealthReview(
              tester,
              'firmware-eta-v2-$pose-${dark ? 'dark' : 'light'}-${(scale * 100).round()}',
            );
            service.uploadResult = const OtaUploadFailed('Canceled transfer.');
            service.release.complete();
            await tester.pumpAndSettle();
            expect(warning, findsNothing);
          },
        );
      }
    }
  }

  for (final held in [true, false]) {
    testWidgets(
      'one-time reveal yields to user gesture held=$held and never follows later progress',
      (tester) async {
        final service = _ProgressService()
          ..capabilities = const LitchDeviceCapabilities(
            contractVersion: 3,
            hardwareRevision: LitchHardwareRevision.v3,
            capabilityMask:
                LitchDeviceCapabilities.firmwareDigestBit |
                LitchDeviceCapabilities.firmwareUpdateBit,
            explicitlyReported: true,
          );
        final download = _ProgressDownload()..release.complete();
        addTearDown(service.dispose);
        addTearDown(() => tester.binding.setSurfaceSize(null));
        await tester.binding.setSurfaceSize(const Size(390, 844));
        var elapsed = Duration.zero;
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: FirmwareScreen(
                bleService: service,
                boardId: 'board-1',
                downloadService: download,
                transferElapsed: () => elapsed,
                onRequestShowFirmwareTab: () {},
                onUpdateActivityChanged: (_) {},
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final update = find.widgetWithText(FilledButton, 'Update Board');
        await Scrollable.ensureVisible(tester.element(update), alignment: 1);
        await tester.pump();
        await tester.tap(update);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        final position = Scrollable.of(tester.element(update)).position;
        final before = position.pixels;
        final gesture = await tester.startGesture(const Offset(100, 200));
        if (!held) {
          await gesture.up();
          await tester.pump();
        }
        service.progress!(.125);
        await tester.pump();
        await tester.pump();
        expect(position.pixels, before);
        final warning = find.text('Keep app open while updating');
        expect(tester.getRect(warning).bottom, greaterThan(844));
        if (held) await gesture.up();
        await tester.pump();
        elapsed = const Duration(seconds: 1);
        service.progress!(.1875);
        await tester.pump();
        elapsed = const Duration(seconds: 2);
        service.progress!(.25);
        await tester.pump();
        expect(position.pixels, before);
        await tester.drag(find.byType(ListView), const Offset(0, -160));
        await tester.pumpAndSettle();
        final userOffset = position.pixels;
        elapsed = const Duration(seconds: 3);
        service.progress!(.37);
        await tester.pump();
        await tester.pump();
        expect(position.pixels, userOffset);
        service.uploadResult = const OtaUploadFailed('Canceled transfer.');
        service.release.complete();
        await tester.pumpAndSettle();
      },
    );
  }
}
