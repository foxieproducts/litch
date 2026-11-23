import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart'
    show debugDefaultTargetPlatformOverride;
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/firmware_update_card.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/firmware_test_fixture.dart';
import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

// The HTTP transport alone is controlled: preparation, image validation and
// immutable cache promotion/reuse stay in the production download service.
class HeldFirmwareClient extends http.BaseClient {
  final firmware = makeEsp32s3Firmware();
  final body = StreamController<List<int>>();
  int artifactGets = 0;
  bool bodyFinished = false;
  static const notes = 'Version: 0.6.0\n';

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    final manifest = makeFirmwareManifest(firmware);
    if (request.url == manifest.artifact.url) {
      ++artifactGets;
      body.add(firmware.sublist(0, firmware.length ~/ 2));
      return http.StreamedResponse(
        body.stream,
        200,
        contentLength: firmware.length,
        request: request,
      );
    }
    final String content;
    if (request.url == FirmwareReleaseInfo.stable.manifestUrl) {
      content = makeFirmwareManifestJson(firmware);
    } else if (request.url == manifest.notes.url) {
      content = notes;
    } else {
      throw StateError('Unexpected HTTP request: ${request.url}');
    }
    final bytes = utf8.encode(content);
    return http.StreamedResponse(
      Stream<List<int>>.value(bytes),
      200,
      contentLength: bytes.length,
      request: request,
    );
  }

  Future<void> finishBody() async {
    if (bodyFinished) return;
    bodyFinished = true;
    // A failed earlier checkpoint must also be able to close an unconsumed body.
    if (!body.hasListener) {
      unawaited(body.stream.drain<void>());
    }
    body.add(firmware.sublist(firmware.length ~/ 2));
    await body.close();
  }
}

// File IO runs in runAsync; bounded event-loop turns drain production futures
// without wall-clock sleeps or advancing HTTP/OTA deadlines to force outcomes.
Future<void> pumpUntil(WidgetTester tester, bool Function() complete) async {
  for (int turn = 0; turn < 1000 && !complete(); ++turn) {
    await tester.pump();
    await Future<void>.delayed(Duration.zero);
  }
  expect(complete(), isTrue, reason: 'Journey checkpoint did not complete.');
  await tester.pump();
}

void main() {
  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final recovery in ['ready', 'restored', 'stale', 'refused']) {
      testWidgets(
        'production reboot verification joins delayed owner proof $platform $recovery',
        (tester) async {
          debugDefaultTargetPlatformOverride = platform;
          PackageInfo.setMockInitialValues(
            appName: 'Litch',
            packageName: 'litch.test',
            version: '0.5.0',
            buildNumber: '1',
            buildSignature: '',
          );
          SharedPreferences.setMockInitialValues({'allow_beta_updates': false});
          await tester.binding.setSurfaceSize(const Size(390, 844));
          addTearDown(() async {
            debugDefaultTargetPlatformOverride = null;
            await tester.binding.setSurfaceSize(null);
          });
          await tester.runAsync(() async {
            final cache = await Directory.systemTemp.createTemp(
              'litch-owner-verify-',
            );
            final httpClient = HeldFirmwareClient();
            final restoredHttpClient = HeldFirmwareClient();
            final download = FirmwareDownloadService(
              client: httpClient,
              outputDirectory: cache,
            );
            final board = OwnerPairingBoard()
              ..dropFirstVersionReply = false
              ..runningVersion = '0.5.0'
              ..state = 5
              ..flags = 7;
            final pendingFile = File('${cache.path}/pending.json');
            var service = LitchBleService(
              ble: board,
              pendingOtaStore: FilePendingOtaTargetStore(pendingFile),
            );
            FirmwareUpdateCard card() => tester.widget<FirmwareUpdateCard>(
              find.byType(FirmwareUpdateCard),
            );
            try {
              await connectBoard(service, board);
              expect(await service.ensureProductReady('board-1'), isTrue);
              Future<void> mountFirmware() => tester.pumpWidget(
                MaterialApp(
                  theme: buildLitchTheme(),
                  home: Scaffold(
                    body: FirmwareScreen(
                      bleService: service,
                      boardId: 'board-1',
                      onRequestShowFirmwareTab: () {},
                      onUpdateActivityChanged: (_) {},
                      downloadService: FirmwareDownloadService(
                        client: restoredHttpClient,
                        outputDirectory: cache,
                      ),
                    ),
                  ),
                ),
              );
              await tester.pumpWidget(
                MaterialApp(
                  theme: buildLitchTheme(),
                  home: Scaffold(
                    body: FirmwareScreen(
                      bleService: service,
                      boardId: 'board-1',
                      onRequestShowFirmwareTab: () {},
                      onUpdateActivityChanged: (_) {},
                      downloadService: download,
                    ),
                  ),
                ),
              );
              await pumpUntil(tester, () => !card().updateDisabled);
              await tester.ensureVisible(
                find.widgetWithText(FilledButton, 'Update Board'),
              );
              await tester.tap(
                find.widgetWithText(FilledButton, 'Update Board'),
              );
              await pumpUntil(tester, () => httpClient.artifactGets == 1);
              await httpClient.finishBody();
              await pumpUntil(
                tester,
                () =>
                    board.uploadCommands.contains('ota_end') &&
                    service.pendingOtaVerification != null &&
                    !card().uploading,
              );
              final target = service.pendingOtaVerification!;
              expect(board.received, httpClient.firmware.length);
              expect(board.uploadCommands, ['ota_begin', 'ota_end']);

              // Native reboot is controlled only at the existing SDK leaf. The
              // real screen receives done, expected disconnect and raw reconnect.
              board.connections.last.add(
                const ConnectionStateUpdate(
                  deviceId: 'board-1',
                  connectionState: DeviceConnectionState.disconnected,
                  failure: null,
                ),
              );
              await pumpUntil(
                tester,
                () => !service.isConnected && card().verifying,
              );
              expect(board.connections, hasLength(1));
              expect(service.pendingOtaRebootExpected, isTrue);
              expect(card().channelChangeDisabled, isTrue);
              expect(card().uploadProgress, lessThan(1));
              board.activateAcceptedTarget(
                digest: target.imageSha256,
                version: target.version,
              );
              board.state = 6;
              board.flags = 5;
              board.revision++;
              final versionReads = board.versionRequests;
              if (recovery == 'restored') {
                await tester.pumpWidget(const SizedBox.shrink());
                await service.dispose();
                service = LitchBleService(
                  ble: board,
                  pendingOtaStore: FilePendingOtaTargetStore(pendingFile),
                );
                // The recreated screen owns this reconnect. Complete its SDK
                // callback instead of asking connectBoard for a second attempt.
                await mountFirmware();
                await pumpUntil(tester, () => board.connections.length == 2);
                board.connections.last.add(
                  const ConnectionStateUpdate(
                    deviceId: 'board-1',
                    connectionState: DeviceConnectionState.connected,
                    failure: null,
                  ),
                );
              } else {
                // This standalone screen has no ConnectionGate to reconnect
                // after the reboot; the verifier joins the ordinary new link.
                await connectBoard(service, board);
              }
              await pumpUntil(
                tester,
                () => service.ownerPairingStatus != null && card().verifying,
              );
              expect(board.connections, hasLength(2));
              final generation = service.connectionSessionToken;
              expect(service.connectedId, target.deviceId);
              expect(service.productReady, isFalse);
              // Longer than three empty version reads at 350ms intervals. No
              // timeout is widened; the existing owner is deliberately held.
              await Future<void>.delayed(const Duration(milliseconds: 900));
              await tester.pump();
              expect(
                card().verifying,
                isTrue,
                reason:
                    'raw BLE connected must wait for the existing owner proof',
              );
              expect(
                service.pendingOtaVerification?.imageSha256,
                target.imageSha256,
              );
              expect(
                service.pendingOtaVerification?.attempt!.preBootNonce,
                target.attempt!.preBootNonce,
              );
              expect(await pendingFile.exists(), isTrue);
              expect(
                board.versionRequests,
                versionReads,
                reason: 'private version reads remain behind owner readiness',
              );
              expect(board.uploadCommands, ['ota_begin', 'ota_end']);
              expect(
                find.text(
                  'The reconnected board did not provide a firmware version yet.',
                ),
                findsNothing,
              );

              if (recovery == 'refused' || recovery == 'stale') {
                if (recovery == 'stale') {
                  final oldPreparation = service.ensureProductReady('board-1');
                  await service.disconnect();
                  expect(await oldPreparation, isFalse);
                  await pumpUntil(tester, () => !card().verifying);
                  await connectBoard(service, board);
                  expect(board.connections, hasLength(3));
                  expect(service.connectionSessionToken, isNot(generation));
                  await pumpUntil(
                    tester,
                    () =>
                        service.ownerPairingStatus != null && card().verifying,
                  );
                  expect(service.productReady, isFalse);
                  expect(await oldPreparation, isFalse);
                  expect(
                    service.pendingOtaVerification?.imageSha256,
                    target.imageSha256,
                  );
                }
                board.setState(6, nextFlags: 5, nextReason: 8);
                await pumpUntil(tester, () => !card().verifying);
                expect(
                  service.pendingOtaVerification?.imageSha256,
                  target.imageSha256,
                );
                expect(await pendingFile.exists(), isTrue);
                expect(board.versionRequests, versionReads);
                expect(board.uploadCommands, ['ota_begin', 'ota_end']);
                expect(
                  find.text('Selected firmware verified on this board.'),
                  findsNothing,
                );
                expect(board.pairingCommands, isEmpty);
                return;
              }
              board.setState(5, nextFlags: 7);
              await pumpUntil(
                tester,
                () =>
                    service.pendingOtaVerification == null && !card().verifying,
              );
              expect(service.connectionSessionToken, generation);
              expect(service.productReady, isTrue);
              expect(
                board.bootNonce,
                int.parse(target.attempt!.preBootNonce, radix: 16) + 1,
              );
              expect(board.runningDigest, target.imageSha256);
              expect(board.uploadCommands, ['ota_begin', 'ota_end']);
              expect(board.pairingCommands, isEmpty);
              expect(await pendingFile.exists(), isFalse);
              expect(
                find.text('Selected firmware verified on this board.'),
                findsOneWidget,
              );
            } finally {
              await tester.pumpWidget(const SizedBox.shrink());
              await disposeBoard(service, board);
              await httpClient.finishBody();
              await restoredHttpClient.finishBody();
              await cache.delete(recursive: true);
              debugDefaultTargetPlatformOverride = null;
            }
          });
        },
      );
    }
  }

  testWidgets(
    'download disconnect fails before Begin and explicit retry uses cache on a fresh connection',
    (tester) async {
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
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));

      await tester.runAsync(() async {
        final cache = await Directory.systemTemp.createTemp(
          'litch-download-disconnect-',
        );
        final httpClient = HeldFirmwareClient();
        final download = FirmwareDownloadService(
          client: httpClient,
          outputDirectory: cache,
        );
        final ble = RebootReplyBle()
          ..dropFirstVersionReply = false
          ..beginReply = null;
        final originalBootNonce = ble.bootNonce;
        final service = LitchBleService(
          ble: ble,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        final activity = <bool>[];
        int verificationEntries = 0;
        FirmwareUpdateCard card() =>
            tester.widget<FirmwareUpdateCard>(find.byType(FirmwareUpdateCard));
        FilledButton updateButton() => tester.widget<FilledButton>(
          find.widgetWithText(FilledButton, 'Update Board'),
        );
        void expectNoUpload() {
          expect(ble.uploadCommands, isEmpty);
          expect(ble.imageWrites, 0);
          expect(ble.received, 0);
          expect(service.pendingOtaVerification, isNull);
          expect(service.failedOtaVerification, isNull);
          // Runtime acceptance can be read for ordinary firmware provenance.
          // The update-specific attempt proof must still wait for Update.
          expect(ble.proofRequests, 0);
          expect(ble.admissionPhase, 0);
          expect(ble.bootNonce, originalBootNonce);
          expect(verificationEntries, 0);
          expect(
            find.text('Selected firmware verified on this board.'),
            findsNothing,
          );
        }

        try {
          await connectBoard(service, ble);
          final originalGeneration = service.connectionSessionToken;
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              home: Scaffold(
                body: FirmwareScreen(
                  bleService: service,
                  boardId: 'board-1',
                  onRequestShowFirmwareTab: () => ++verificationEntries,
                  onUpdateActivityChanged: activity.add,
                  downloadService: download,
                ),
              ),
            ),
          );
          await pumpUntil(tester, () => !card().updateDisabled);
          expect(updateButton().onPressed, isNotNull);
          await tester.ensureVisible(
            find.widgetWithText(FilledButton, 'Update Board'),
          );
          await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
          await pumpUntil(
            tester,
            () => httpClient.artifactGets == 1 && card().downloadProgress > 0,
          );
          expect(activity, [true]);
          expect(card().downloading, isTrue);
          expect(card().channelChangeDisabled, isTrue);
          expect(updateButton().onPressed, isNull);
          expectNoUpload();

          // SDK disconnect while the valid website body is still incomplete.
          ble.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
          await pumpUntil(tester, () => !service.isConnected);
          expect(card().downloading, isTrue);
          expect(activity, [true]);
          expectNoUpload();
          await httpClient.finishBody();
          await pumpUntil(tester, () => activity.length == 2);

          expect(activity, [true, false]);
          expect(card().downloading, isFalse);
          expect(card().uploading, isFalse);
          expect(card().verifying, isFalse);
          expect(card().channelChangeDisabled, isFalse);
          expect(find.widgetWithText(FilledButton, 'Update Board'), findsNothing);
          expect(
            find.text('Connect to a board before starting update.'),
            findsOneWidget,
          );
          expect(
            find.text('Board is not connected for BLE upload.'),
            findsOneWidget,
          );
          expectNoUpload();
          expect(httpClient.artifactGets, 1);
          final cached = await cache
              .list()
              .where((entry) => entry is File)
              .toList();
          expect(cached, hasLength(1));
          expect(
            await (cached.single as File).readAsBytes(),
            httpClient.firmware,
          );
          expect(cached.single.path, isNot(endsWith('.part')));

          final acceptanceBeforeReconnect = ble.acceptanceRequests;
          await connectBoard(service, ble);
          final retryGeneration = service.connectionSessionToken;
          expect(retryGeneration, isNot(originalGeneration));
          await service.fetchDeviceCapabilities(deviceId: 'board-1');
          await service.openConnectionSession('board-1');
          await pumpUntil(tester, () => !card().updateDisabled);
          await pumpUntil(
            tester,
            () => ble.acceptanceRequests > acceptanceBeforeReconnect,
          );
          expect(updateButton().onPressed, isNotNull);
          expect(card().verifying, isFalse);
          expectNoUpload();
          expect(httpClient.artifactGets, 1);
          expect(activity, [
            true,
            false,
          ], reason: 'Reconnect must not restart the update.');

          await tester.tap(find.widgetWithText(FilledButton, 'Update Board'));
          await pumpUntil(
            tester,
            () => ble.uploadCommands.contains('ota_begin'),
          );
          expect(
            httpClient.artifactGets,
            1,
            reason: 'Explicit retry revalidates the cached package.',
          );
          expect(ble.uploadCommands, ['ota_begin']);
          expect(service.connectionSessionToken, retryGeneration);
          expect(
            ble.imageWrites,
            0,
            reason: 'The controlled board has not acknowledged Begin.',
          );
          expect(service.pendingOtaVerification, isNull);
          expect(card().uploading, isTrue);

          // End the bounded journey with a controlled board refusal. Successful
          // transfer/reboot is covered by the existing service journeys.
          ble.emitStatus('{"e":"begin_failed"}');
          await pumpUntil(tester, () => activity.length == 4);
          expect(activity, [true, false, true, false]);
          expect(ble.uploadCommands, ['ota_begin', 'ota_abort']);
          expect(ble.imageWrites, 0);
          expect(service.pendingOtaVerification, isNull);
          expect(verificationEntries, 0);
          expect(
            find.text('Selected firmware verified on this board.'),
            findsNothing,
          );
          expect(card().uploading, isFalse);
          expect(card().verifying, isFalse);
          expect(card().channelChangeDisabled, isFalse);
          expect(
            find.text(
              'The board could not start the update. Contact support for help.',
            ),
            findsOneWidget,
          );
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await disposeBoard(service, ble);
          await httpClient.finishBody();
          await cache.delete(recursive: true);
        }
      });
    },
  );
}
