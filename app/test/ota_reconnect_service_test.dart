import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart'
    show debugPrint, debugDefaultTargetPlatformOverride, TargetPlatform;
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/models/firmware_manifest.dart';
import 'package:litch_mobile/src/services/firmware_status_text.dart';
import 'package:litch_mobile/src/services/ota/litch_firmware_uploader.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';

import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/firmware_test_fixture.dart';

class _HeldStore extends MemoryOtaTargetStore {
  final entered = Completer<void>();
  final release = Completer<void>();
  bool fail = false;

  @override
  Future<void> save(OtaVerificationTarget value) async {
    entered.complete();
    await release.future;
    if (fail) throw const FileSystemException('injected durable write failure');
    await super.save(value);
  }
}

class _Journey {
  _Journey(this.ble, this.service, this.directory, this.file, this.manifest);
  final RebootReplyBle ble;
  final LitchBleService service;
  final Directory directory;
  final File file;
  final FirmwareManifest manifest;

  static Future<_Journey> create({
    RebootReplyBle? board,
    OtaPendingTargetStore? store,
    Uint8List? firmwareBytes,
    AppTraceSink? trace,
  }) async {
    final ble = board ?? (RebootReplyBle()..dropFirstVersionReply = false);
    final service = LitchBleService(
      ble: ble,
      pendingOtaStore: store ?? MemoryOtaTargetStore(),
      appTrace: trace,
    );
    final directory = await Directory.systemTemp.createTemp('litch-attempt-');
    final bytes = firmwareBytes ?? makeEsp32s3Firmware();
    final manifest = makeFirmwareManifest(bytes, version: '0.8');
    ble.runningDigest = manifest.artifact.imageSha256;
    final file = await File(
      '${directory.path}/firmware.bin',
    ).writeAsBytes(bytes);
    await connectBoard(service, ble);
    return _Journey(ble, service, directory, file, manifest);
  }

  Future<OtaUploadResult> upload() => service.uploadFirmwareFile(
    deviceId: 'board-1',
    firmwareFile: file,
    manifest: manifest,
  );

  Future<OtaPostRebootVerification> verify() => service.verifyFirmwareAfterOta(
    deviceId: 'board-1',
    uploadConnectionToken:
        service.pendingOtaVerification?.uploadConnectionToken ?? -1,
    expectedVersion: manifest.version,
    expectedImageSha256: manifest.artifact.imageSha256,
  );

  Future<void> reconnect() async {
    await service.disconnect();
    await connectBoard(service, ble);
  }

  Future<void> dispose() async {
    await disposeBoard(service, ble);
    await directory.delete(recursive: true);
  }
}

class _RetirementTrace implements AppTraceSink {
  final retirements = <Map<String, Object?>>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'ble.connection_session_retired') {
      retirements.add(Map.of(fields));
    }
  }
}

void main() {
  test(
    'OTA missing Begin admission retires shared owner with zero image bytes',
    () async {
      final trace = _RetirementTrace();
      final gate = Completer<void>();
      final ble = RebootReplyBle()
        ..dropFirstVersionReply = false
        ..holdBeginAdmission = true
        ..beginWorkGate = gate;
      final journey = await _Journey.create(board: ble, trace: trace);
      final firstConnectionToken = journey.service.connectionSessionToken;
      final firstBootNonce = ble.bootNonce;
      final hostLines = <String>[];
      final previousDebugPrint = debugPrint;
      debugPrint = (message, {wrapWidth}) {
        if (message != null) hostLines.add(message);
        previousDebugPrint(message, wrapWidth: wrapWidth);
      };
      try {
        final upload = journey.upload();
        await ble.beginWorkEntered.future.timeout(const Duration(seconds: 3));
        final result = await upload.timeout(const Duration(seconds: 10));
        expect(result, isA<OtaUploadFailed>());
        expect(result.message, contains('Failed to send OTA start'));
        expect(ble.imageWrites, 0);
        expect(ble.uploadCommands, isNot(contains('ota_end')));
        final retired = trace.retirements.single;
        expect(retired['cause'], 'object_deadline');
        expect(retired['object_pending'], 1);
        expect(retired['head_op'], 0x32);
        expect(retired['head_seq'], (retired['obj_accepted'] as int) + 1);
        expect(retired['obj_query'], isTrue);
        expect(retired['query_write'], isFalse);
        final hostLine = hostLines
            .where((line) => line.contains('BLE shared session retired:'))
            .single;
        expect(hostLine.length, lessThanOrEqualTo(525));
        expect(
          hostLine,
          matches(
            RegExp(
              r'^\d{2}:\d{2}:\d{2}\.\d{3} BLE shared session retired: '
              r'generation=\d+ cause=object_deadline kind=TimeoutException '
              r'ordered_pending=\d+ ord_head_seq=(?:\d+|-) '
              r'ord_head_op=(?:\d+|-) ord_terminal=\d+ ord_ahead=\d+ '
              r'object_pending=1 head_op=50 '
              r'head_seq=\d+ obj_accepted=\d+ obj_grant=\d+ '
              r'reverse_confirmed=\d+ reverse_grant=\d+ '
              r'obj_query=1 query_write=0$',
            ),
          ),
        );
        expect(hostLine, contains(' head_seq=${retired['head_seq']} '));
        expect(hostLine, contains(' obj_accepted=${retired['obj_accepted']} '));
        for (final field in [
          'ord_head_seq',
          'ord_head_op',
          'ord_terminal',
          'ord_ahead',
        ]) {
          expect(hostLine, contains(' $field=${retired[field] ?? '-'} '));
        }
        expect(journey.service.consoleLinesSnapshot, contains(hostLine));
        expect(ble.disconnectBeforeBeginReply, isFalse);
        expect(ble.disconnectBeforeFinalReply, isFalse);
        expect(journey.service.pendingOtaVerification, isNull);
        expect(ble.proofRequests, 1);
        ble.holdBeginAdmission = false;
        ble.beginWorkGate = null;
        gate.complete();
        await Future<void>.delayed(Duration.zero);
        expect(ble.imageWrites, 0);
        final retryCommandsFrom = ble.uploadCommands.length;
        final connectionIndex = ble.connections.length;
        final retry = journey.upload();
        // Only the SDK callback is supplied here. The retry must ask the
        // production connection owner to replace its retired session itself.
        for (
          var turn = 0;
          turn < 50 && ble.connections.length == connectionIndex;
          ++turn
        ) {
          await Future<void>.delayed(Duration.zero);
        }
        expect(ble.connections.length, connectionIndex + 1);
        ble.connections[connectionIndex].add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        final retryResult = await retry.timeout(const Duration(seconds: 5));
        expect(retryResult, isA<OtaUploadFinalized>());
        final retryToken = journey.service.connectionSessionToken;
        expect(retryToken, greaterThan(firstConnectionToken));
        expect((retryResult as OtaUploadFinalized).connectionToken, retryToken);
        expect(
          ble.bootNonce,
          firstBootNonce,
          reason: 'No board reboot occurred',
        );
        expect(ble.uploadCommands.skip(retryCommandsFrom), [
          'ota_begin',
          'ota_end',
        ]);
        expect(
          ble.uploadCommands.where((command) => command == 'ota_begin'),
          hasLength(2),
        );
        expect(ble.received, journey.manifest.artifact.sizeBytes);
        expect(ble.proofRequests, 3);
        expect(ble.imageBytesAtProof, [
          0,
          0,
          journey.manifest.artifact.sizeBytes,
        ]);
        final pending = journey.service.pendingOtaVerification!;
        expect(pending.uploadConnectionToken, retryToken);
        expect(pending.attempt!.preBootNonce, '0000000000000002');
        expect(pending.attempt!.attempt, ble.admissionAttempt);
      } finally {
        debugPrint = previousDebugPrint;
        if (!gate.isCompleted) gate.complete();
        await Future<void>.delayed(Duration.zero);
        await journey.dispose();
      }
    },
  );

  test(
    'OTA admission survives slow Begin work before any image bytes',
    () async {
      final trace = _RetirementTrace();
      final gate = Completer<void>();
      final ble = RebootReplyBle()
        ..dropFirstVersionReply = false
        ..beginWorkGate = gate;
      final journey = await _Journey.create(board: ble, trace: trace);
      try {
        final upload = journey.upload();
        await ble.beginWorkEntered.future.timeout(const Duration(seconds: 3));
        await Future<void>.delayed(const Duration(milliseconds: 2100));
        expect(trace.retirements, isEmpty);
        expect(ble.imageWrites, 0);
        expect(ble.uploadCommands, isNot(contains('ota_end')));
        gate.complete();
        final result = await upload.timeout(const Duration(seconds: 5));
        expect(result, isA<OtaUploadFinalized>());
        expect(
          ble.uploadCommands.where((cmd) => cmd == 'ota_begin'),
          hasLength(1),
        );
        expect(
          ble.uploadCommands.where((cmd) => cmd == 'ota_end'),
          hasLength(1),
        );
        expect(ble.imageWrites, greaterThan(0));
      } finally {
        if (!gate.isCompleted) gate.complete();
        await journey.dispose();
      }
    },
  );

  for (final rejection in [
    (
      detail: 'begin_failed',
      message:
          'The board could not start the update. Contact support for help.',
    ),
    (
      detail: 'busy_digest',
      message:
          'The board is checking its installed software. Wait a moment and try again.',
    ),
    (
      detail: 'busy_store_clear',
      message: 'The board is busy. Wait a moment and try again.',
    ),
    (
      detail: 'image_too_large',
      message:
          'This update does not fit on this board. Contact support for help.',
    ),
    (
      detail: 'future_board_error',
      message:
          'The board could not start the update. Contact support for help.',
    ),
  ]) {
    test('OTA start preserves shared refusal ${rejection.detail}', () async {
      final ble = RebootReplyBle()
        ..beginReply = jsonEncode({'e': rejection.detail});
      final service = LitchBleService(
        ble: ble,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      final directory = await Directory.systemTemp.createTemp(
        'litch-start-refused-',
      );
      try {
        await connectBoard(service, ble);
        final file = await File(
          '${directory.path}/firmware.bin',
        ).writeAsBytes(makeEsp32s3Firmware());
        final statuses = <String>[];
        final result = await service
            .uploadFirmwareFile(
              deviceId: 'board-1',
              firmwareFile: file,
              manifest: makeFirmwareManifest(
                await file.readAsBytes(),
                version: '0.8',
              ),
              onStatus: statuses.add,
            )
            .timeout(const Duration(seconds: 2));
        expect(ble.sessionActivated, isTrue);
        expect(result, isA<OtaUploadFailed>());
        final failure = result as OtaUploadFailed;
        expect(failure.detail, rejection.detail);
        expect(failure.message, rejection.message);
        expect(failure.message, isNot(contains(rejection.detail)));
        expect(statuses, [rejection.message]);
        expect(
          simplifyFirmwareStatus(statuses.single),
          failure.message,
          reason: 'The live Firmware screen must preserve the refusal meaning.',
        );
        expect(ble.imageWrites, 0);
        expect(ble.received, 0);
        expect(ble.uploadCommands, ['ota_begin', 'ota_abort']);
        expect(
          service.consoleLinesSnapshot,
          contains(contains('OTA: error (${rejection.detail})')),
          reason: 'The fragmented status must reach the production channel.',
        );
      } finally {
        await disposeBoard(service, ble);
        await directory.delete(recursive: true);
      }
    });
  }

  test(
    'OTA start distinguishes lost connection from missing acceptance',
    () async {
      final ble = RebootReplyBle()..disconnectBeforeBeginReply = true;
      final service = LitchBleService(
        ble: ble,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      final directory = await Directory.systemTemp.createTemp(
        'litch-start-disconnected-',
      );
      try {
        await connectBoard(service, ble);
        final file = await File(
          '${directory.path}/firmware.bin',
        ).writeAsBytes(makeEsp32s3Firmware());
        final result = await service.uploadFirmwareFile(
          deviceId: 'board-1',
          firmwareFile: file,
          manifest: makeFirmwareManifest(
            await file.readAsBytes(),
            version: '0.8',
          ),
        );
        expect(result, isA<OtaUploadFailed>());
        expect(
          result.message,
          'The board disconnected before the update could start. Reconnect it and try again.',
        );
        expect((result as OtaUploadFailed).detail, isEmpty);
        expect(ble.imageWrites, 0);
        expect(ble.uploadCommands, ['ota_begin']);
      } finally {
        await disposeBoard(service, ble);
        await directory.delete(recursive: true);
      }
    },
  );

  test('OTA start keeps the six-second silence timeout', () async {
    final ble = RebootReplyBle()..beginReply = null;
    final service = LitchBleService(
      ble: ble,
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    final directory = await Directory.systemTemp.createTemp(
      'litch-start-silent-',
    );
    try {
      await connectBoard(service, ble);
      final file = await File(
        '${directory.path}/firmware.bin',
      ).writeAsBytes(makeEsp32s3Firmware());
      final clock = Stopwatch()..start();
      final result = await service.uploadFirmwareFile(
        deviceId: 'board-1',
        firmwareFile: file,
        manifest: makeFirmwareManifest(
          await file.readAsBytes(),
          version: '0.8',
        ),
      );
      clock.stop();
      expect(result, isA<OtaUploadFailed>());
      expect(result.message, 'Board did not acknowledge OTA start.');
      expect((result as OtaUploadFailed).detail, isEmpty);
      expect(clock.elapsed, greaterThanOrEqualTo(const Duration(seconds: 6)));
      expect(ble.imageWrites, 0);
      expect(ble.uploadCommands, ['ota_begin', 'ota_abort']);
    } finally {
      await disposeBoard(service, ble);
      await directory.delete(recursive: true);
    }
  });

  test('shared upload has no starting digest prerequisite', () async {
    for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
      debugDefaultTargetPlatformOverride = platform;
      final journey = await _Journey.create(
        board: RebootReplyBle()
          ..digestAvailable = false
          ..mtu = platform == TargetPlatform.android ? 517 : 512,
        firmwareBytes: makeEsp32s3Firmware(
          payloadBytes: List.filled(2048, 0x5a),
        ),
      );
      try {
        final session = await journey.service.openConnectionSession('board-1');
        expect(session!.selectedPacketMaximum, 495);
        final reads = journey.ble.digestRequests;
        final mtuRequests = journey.ble.mtuRequests;
        expect(await journey.upload(), isA<OtaUploadFinalized>());
        expect(journey.ble.digestRequests, reads);
        expect(journey.ble.mtuRequests, mtuRequests);
        expect(journey.ble.imageChunkLengths.first, 485);
        expect(
          journey.ble.nativeWriteLengths,
          everyElement(lessThanOrEqualTo(495)),
        );
        expect(journey.ble.received, journey.manifest.artifact.sizeBytes);
        expect(journey.ble.proofRequests, 2);
        expect(journey.ble.proofRequestIds, [1, 2]);
        expect(journey.ble.imageBytesAtProof, [
          0,
          journey.manifest.artifact.sizeBytes,
        ]);
        expect(journey.ble.admissionPhasesAtProof, [0, 1]);
        final pending = journey.service.pendingOtaVerification!;
        expect(pending.artifactSha256, journey.manifest.artifact.sha256);
        expect(pending.attempt!.sourceSequence, 23);
        expect(pending.attempt!.expectedSequence, 24);
        expect(journey.ble.uploadCommands, ['ota_begin', 'ota_end']);
      } finally {
        await journey.dispose();
        debugDefaultTargetPlatformOverride = null;
      }
    }
  });

  test('unsupported capability contract is refused before Begin', () async {
    final journey = await _Journey.create(
      board: RebootReplyBle()..supportsSession = false,
    );
    try {
      expect(await journey.upload(), isA<OtaUploadFailed>());
      expect(journey.ble.uploadCommands, isEmpty);
      expect(journey.ble.imageWrites, 0);
    } finally {
      await journey.dispose();
    }
  });

  test('recovery VALID source uploads without runtime acceptance', () async {
    final journey = await _Journey.create(
      board: RebootReplyBle()
        ..recoveryOnly = true
        ..runtimeAccepted = 0,
    );
    try {
      final result = await journey.upload();
      expect(
        journey.service.deviceCapabilities('board-1').recoveryOnly,
        isTrue,
      );
      expect(result, isA<OtaUploadFinalized>());
      expect(journey.ble.uploadCommands, ['ota_begin', 'ota_end']);
      expect(journey.ble.received, journey.manifest.artifact.sizeBytes);
      expect(journey.ble.proofRequests, 2);
      expect(
        journey.service.pendingOtaVerification!.attempt!.sourceSequence,
        23,
      );
    } finally {
      await journey.dispose();
    }
  });

  test('wrong same-size manifest is refused before Begin', () async {
    final store = _HeldStore();
    store.release.complete();
    final journey = await _Journey.create(store: store);
    try {
      final otherManifest = makeFirmwareManifest(
        makeEsp32s3Firmware(payloadBytes: List<int>.filled(16, 0x7a)),
        version: '0.8',
      );
      expect(
        otherManifest.artifact.sizeBytes,
        journey.manifest.artifact.sizeBytes,
      );
      expect(
        otherManifest.artifact.sha256,
        isNot(journey.manifest.artifact.sha256),
      );
      final result = await journey.service.uploadFirmwareFile(
        deviceId: 'board-1',
        firmwareFile: journey.file,
        manifest: otherManifest,
      );
      expect(result, isA<OtaUploadFailed>());
      expect(
        result.message,
        'This firmware package cannot be installed safely. Check for a newer update or contact support.',
      );
      expect(result.message, isNot(contains('Restart')));
      _expectNoOtaMutation(journey, store);
      expect(journey.ble.proofRequests, 0);
    } finally {
      await journey.dispose();
    }
  });

  final policyCorpus =
      jsonDecode(
            File(
              '../test/fixtures/firmware_boot_policy.json',
            ).readAsStringSync(),
          )
          as Map<String, dynamic>;
  final policyCases = (policyCorpus['cases'] as List<dynamic>)
      .cast<Map<String, dynamic>>();
  final refusedImages = <String, Uint8List>{
    for (final name in [
      'record_policy_1',
      'legacy_absent',
      'unknown_epoch',
      'reserved_nonzero',
    ])
      name: _bytesFromHex(
        policyCases.firstWhere((entry) => entry['name'] == name)['image_hex']
            as String,
      ),
    'unavailable': makeEsp32s3Firmware(segmentBytes: [1, 2, 3, 4]),
  };
  for (final entry in refusedImages.entries) {
    test('actual service refuses selected ${entry.key} before Begin', () async {
      final store = _HeldStore();
      store.release.complete();
      final journey = await _Journey.create(
        store: store,
        firmwareBytes: entry.value,
      );
      try {
        final result = await journey.upload();
        expect(result, isA<OtaUploadFailed>());
        expect(
          result.message,
          'This firmware package cannot be installed safely. Check for a newer update or contact support.',
        );
        expect(result.message, isNot(contains('Restart')));
        _expectNoOtaMutation(journey, store);
        expect(journey.ble.proofRequests, 0);
      } finally {
        await journey.dispose();
      }
    });
  }

  for (final fault in [
    'unaccepted',
    'unavailable',
    'running_selected_mismatch',
    'newer_NEW_selector',
    'newer_PENDING_selector',
    'old_acceptance_without_proof',
  ]) {
    test('actual service refuses $fault preflight before Begin', () async {
      final store = _HeldStore();
      store.release.complete();
      final journey = await _Journey.create(store: store);
      try {
        switch (fault) {
          case 'unaccepted':
            journey.ble.runtimeAccepted = 0;
          case 'unavailable':
            journey.ble.transformProof = (payload) => {
              'op': 1,
              'q': payload['q'],
              'e': 'unavailable',
            };
          case 'running_selected_mismatch':
            journey.ble.selectedSlot = 1;
          case 'newer_NEW_selector':
            journey.ble.selectorSequences[1] = 24;
            journey.ble.selectorStates[1] = 0;
          case 'newer_PENDING_selector':
            journey.ble.selectorSequences[1] = 24;
            journey.ble.selectorStates[1] = 1;
          case 'old_acceptance_without_proof':
            journey.ble.transformProof = (_) => {'ov': 'v'};
        }
        final result = await journey.upload();
        expect(result, isA<OtaUploadFailed>());
        if (fault == 'unaccepted') {
          expect(
            result.message,
            'The board has not confirmed it is ready for an update. Keep it powered on, then try again. If this continues, open Help for support.',
          );
        }
        _expectNoOtaMutation(journey, store);
        expect(journey.ble.proofRequests, 1);
      } finally {
        await journey.dispose();
      }
    });
  }

  for (final fault in [
    'running_selected_mismatch',
    'newer_PENDING_selector',
    'running_PENDING',
    'unavailable',
  ]) {
    test('recovery refuses $fault source before Begin', () async {
      final store = _HeldStore();
      store.release.complete();
      final journey = await _Journey.create(
        board: RebootReplyBle()
          ..recoveryOnly = true
          ..runtimeAccepted = 0,
        store: store,
      );
      try {
        switch (fault) {
          case 'running_selected_mismatch':
            journey.ble.selectedSlot = 1;
          case 'newer_PENDING_selector':
            journey.ble.selectorSequences[1] = 24;
            journey.ble.selectorStates[1] = 1;
          case 'running_PENDING':
            journey.ble.runningState = 1;
            journey.ble.selectorStates[0] = 1;
          case 'unavailable':
            journey.ble.transformProof = (payload) => {
              'op': 1,
              'q': payload['q'],
              'e': 'unavailable',
            };
        }
        expect(await journey.upload(), isA<OtaUploadFailed>());
        expect(
          journey.service.deviceCapabilities('board-1').recoveryOnly,
          isTrue,
        );
        _expectNoOtaMutation(journey, store);
        expect(journey.ble.proofRequests, 1);
      } finally {
        await journey.dispose();
      }
    });
  }

  test('durable store completion precedes the actual service End', () async {
    final store = _HeldStore();
    final journey = await _Journey.create(store: store);
    try {
      final upload = journey.upload();
      await store.entered.future;
      expect(journey.ble.received, journey.manifest.artifact.sizeBytes);
      expect(journey.ble.uploadCommands, ['ota_begin']);
      expect(journey.service.pendingOtaVerification, isNull);
      store.release.complete();
      expect(await upload, isA<OtaUploadFinalized>());
      expect(store.target, same(journey.service.pendingOtaVerification));
      expect(journey.ble.uploadCommands, ['ota_begin', 'ota_end']);
    } finally {
      await journey.dispose();
    }
  });

  test('durable store failure permits zero End submissions', () async {
    final store = _HeldStore()..fail = true;
    final journey = await _Journey.create(store: store);
    try {
      final upload = journey.upload();
      await store.entered.future;
      expect(journey.ble.uploadCommands, ['ota_begin']);
      store.release.complete();
      expect(await upload, isA<OtaUploadFailed>());
      expect(journey.ble.uploadCommands, ['ota_begin', 'ota_abort']);
      expect(store.target, isNull);
    } finally {
      await journey.dispose();
    }
  });

  test(
    'lost End reply without activation never verifies matching bytes',
    () async {
      final journey = await _Journey.create(
        board: RebootReplyBle()
          ..dropFirstVersionReply = false
          ..disconnectBeforeFinalReply = true,
      );
      try {
        final nonce = journey.ble.bootNonce;
        expect(await journey.upload(), isA<OtaUploadPendingVerification>());
        expect(journey.service.pendingOtaVerification, isNotNull);
        expect(journey.ble.uploadCommands, ['ota_begin', 'ota_end']);
        await connectBoard(journey.service, journey.ble);
        expect(journey.ble.bootNonce, nonce);
        expect(await journey.verify(), isA<OtaPostRebootPending>());
        journey.ble.failTarget(reboot: false);
        final failure = await journey.verify();
        expect(failure, isA<OtaPostRebootFailedAttempt>());
        expect(journey.ble.uploadCommands, ['ota_begin', 'ota_end']);
      } finally {
        await journey.dispose();
      }
    },
  );

  test(
    'Reinstall requires explicit target activation and a changed Hello nonce',
    () async {
      final journey = await _Journey.create();
      try {
        expect(await journey.upload(), isA<OtaUploadFinalized>());
        final token = journey.service.connectionSessionToken;
        final nonce = journey.ble.bootNonce;
        await journey.service.setAppForegroundActive(false);
        await journey.service.setAppForegroundActive(true);
        expect(journey.service.connectionSessionToken, token);
        final previousReads = journey.ble.versionRequests;
        final verification = journey.verify();
        await Future<void>.delayed(Duration.zero);
        expect(journey.ble.versionRequests, previousReads);
        await journey.reconnect();
        expect(journey.ble.bootNonce, nonce);
        expect(await verification, isA<OtaPostRebootPending>());
        await journey.service.disconnect();
        journey.ble.activateAcceptedTarget();
        await connectBoard(journey.service, journey.ble);
        expect(await journey.verify(), isA<OtaPostRebootVerified>());
        expect(journey.ble.runningSlot, 1);
        expect(journey.ble.selectedSequence, 24);
      } finally {
        await journey.dispose();
      }
    },
  );

  test('actual service rejects same-image ABORTED fallback', () async {
    final journey = await _Journey.create();
    try {
      expect(await journey.upload(), isA<OtaUploadFinalized>());
      await journey.service.disconnect();
      journey.ble.failTarget();
      await connectBoard(journey.service, journey.ble);
      final result = await journey.verify();
      expect(result, isA<OtaPostRebootFailedAttempt>());
      final failed = result as OtaPostRebootFailedAttempt;
      expect(failed.expected.expectedSequence, 24);
      expect(failed.observed.selectedSequence, 23);
      expect(failed.observed.selectors[1].state, 4);
      expect(failed.imageSha256, journey.manifest.artifact.imageSha256);
    } finally {
      await journey.dispose();
    }
  });

  for (final fault in ['request', 'hello', 'malformed']) {
    test('actual service rejects $fault proof on the fresh channel', () async {
      final journey = await _Journey.create();
      try {
        expect(await journey.upload(), isA<OtaUploadFinalized>());
        await journey.service.disconnect();
        journey.ble.activateAcceptedTarget();
        await connectBoard(journey.service, journey.ble);
        journey.ble.transformProof = (payload) => {
          ...payload,
          if (fault == 'request') 'q': (payload['q']! as int) - 1,
          if (fault == 'hello') 'n': '0000000000000009',
          if (fault == 'malformed') 's': [1],
        };
        expect(await journey.verify(), isA<OtaPostRebootPending>());
        expect(journey.service.pendingOtaVerification, isNotNull);
      } finally {
        await journey.dispose();
      }
    });
  }

  test(
    'disconnect invalidates in-flight proof and old status fragments',
    () async {
      final journey = await _Journey.create();
      try {
        await journey.upload();
        await journey.service.disconnect();
        journey.ble.activateAcceptedTarget();
        await connectBoard(journey.service, journey.ble);
        journey.ble.onProofRequest = (_) {
          journey.ble.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
        };
        expect(await journey.verify(), isA<OtaPostRebootPending>());
        expect(journey.service.pendingOtaVerification, isNotNull);
      } finally {
        await journey.dispose();
      }
    },
  );

  test('another board cannot verify or replace the pending target', () async {
    final journey = await _Journey.create();
    try {
      await journey.upload();
      final target = journey.service.pendingOtaVerification;
      await journey.service.disconnect();
      await connectBoard(journey.service, journey.ble, deviceId: 'board-2');
      final result = await journey.service.verifyFirmwareAfterOta(
        deviceId: 'board-2',
        uploadConnectionToken: -1,
        expectedVersion: journey.manifest.version,
        expectedImageSha256: journey.manifest.artifact.imageSha256,
      );
      expect(result, isA<OtaPostRebootPending>());
      expect(journey.service.connectedId, 'board-2');
      expect(journey.service.pendingOtaVerification, same(target));
    } finally {
      await journey.dispose();
    }
  });

  for (final acceptedTarget in [false, true]) {
    test(
      'new service restores file intent and exact outcome accepted=$acceptedTarget',
      () async {
        final directory = await Directory.systemTemp.createTemp(
          'litch-process-',
        );
        final file = File('${directory.path}/pending.json');
        final ble = RebootReplyBle()..dropFirstVersionReply = false;
        final first = await _Journey.create(
          board: ble,
          store: FilePendingOtaTargetStore(file),
        );
        LitchBleService? second;
        try {
          expect(await first.upload(), isA<OtaUploadFinalized>());
          final nonce =
              first.service.pendingOtaVerification!.attempt!.preBootNonce;
          await first.service.dispose();
          if (acceptedTarget) {
            ble.activateAcceptedTarget();
          } else {
            ble.failTarget();
          }
          second = LitchBleService(
            ble: ble,
            pendingOtaStore: FilePendingOtaTargetStore(file),
          );
          await second.restorePendingOtaVerification();
          final restored = second.pendingOtaVerification!;
          expect(restored.attempt!.preBootNonce, nonce);
          expect(restored.uploadConnectionToken, -1);
          await connectBoard(second, ble);
          final result = await second.verifyFirmwareAfterOta(
            deviceId: 'board-1',
            uploadConnectionToken: -1,
            expectedVersion: first.manifest.version,
            expectedImageSha256: first.manifest.artifact.imageSha256,
          );
          expect(
            result,
            acceptedTarget
                ? isA<OtaPostRebootVerified>()
                : isA<OtaPostRebootFailedAttempt>(),
          );
          expect(ble.uploadCommands, ['ota_begin', 'ota_end']);
          expect(await file.exists(), isTrue);
          if (result is OtaPostRebootDifferentImage) {
            await second.resolveFailedOtaVerification(restored, result);
          } else {
            await second.clearPendingOtaVerification(restored);
          }
          expect(await file.exists(), isFalse);
        } finally {
          if (second != null) {
            await disposeBoard(second, ble);
          } else {
            await disposeBoard(first.service, ble);
          }
          await first.directory.delete(recursive: true);
          await directory.delete(recursive: true);
        }
      },
    );
  }
}

void _expectNoOtaMutation(_Journey journey, _HeldStore store) {
  expect(journey.ble.uploadCommands, isEmpty);
  expect(journey.ble.imageWrites, 0);
  expect(journey.ble.received, 0);
  expect(journey.service.pendingOtaVerification, isNull);
  expect(store.entered.isCompleted, isFalse);
}

Uint8List _bytesFromHex(String text) => Uint8List.fromList([
  for (int offset = 0; offset < text.length; offset += 2)
    int.parse(text.substring(offset, offset + 2), radix: 16),
]);
