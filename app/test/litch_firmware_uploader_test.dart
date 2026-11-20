import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/litch_firmware_uploader.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';
import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';

OtaStatusEvent event(
  OtaStatusKind kind, {
  int? rx,
  String detail = '',
  Map<String, Object?> payload = const <String, Object?>{},
}) {
  return OtaStatusEvent(
    deviceId: 'board-1',
    generation: 1,
    kind: kind,
    state: kind.name,
    rawText: '{}',
    payload: payload,
    detail: detail,
    rxBytes: rx,
  );
}

Map<String, String> metricFields(String summary) => <String, String>{
  for (final Match match in RegExp(r'(\w+)=([^\s]+)').allMatches(summary))
    match.group(1)!: match.group(2)!,
};

void main() {
  for (final kind in [OtaStatusKind.error, OtaStatusKind.aborted]) {
    test(
      'post-End ${kind.name} retains uncertain activation without Abort',
      () async {
        final directory = await Directory.systemTemp.createTemp(
          'litch-end-terminal-',
        );
        addTearDown(() => directory.delete(recursive: true));
        final file = await File(
          '${directory.path}/firmware.bin',
        ).writeAsBytes([1]);
        final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
        addTearDown(events.close);
        final commands = <String>[];
        final uploader = LitchFirmwareUploader(
          connectionToken: 7,
          events: events.stream,
          beforeEnd: () async {},
          sendControl: (payload) async {
            final command = payload['cmd']! as String;
            commands.add(command);
            if (command == 'ota_begin') events.add(event(OtaStatusKind.begin));
            if (command == 'ota_end') {
              events.add(event(kind, detail: 'readback_failed'));
            }
            return true;
          },
          writeChunk: (bytes, {required bool reliable}) async =>
              events.add(event(OtaStatusKind.progress, rx: 1)),
        );
        final result = await uploader.upload(firmwareFile: file, totalBytes: 1);
        expect(result, isA<OtaUploadPendingVerification>());
        expect(commands, ['ota_begin', 'ota_end']);
      },
    );
  }
  for (final persist in [true, false]) {
    test(
      'zero End while durable intent waits; persistence succeeds=$persist',
      () async {
        final directory = await Directory.systemTemp.createTemp(
          'litch-before-end-',
        );
        addTearDown(() => directory.delete(recursive: true));
        final file = await File(
          '${directory.path}/firmware.bin',
        ).writeAsBytes([1, 2, 3, 4]);
        final store = FilePendingOtaTargetStore(
          File('${directory.path}/intent.json'),
        );
        final target = OtaVerificationTarget(
          deviceId: 'board',
          uploadConnectionToken: 7,
          version: '0.9.0-5',
          imageSha256: 'a' * 64,
          artifactSha256: 'b' * 64,
          attempt: const OtaAttemptIdentity(
            preBootNonce: '0123456789abcdef',
            attempt: 1,
            sourceSlot: 0,
            sourceSector: 0,
            sourceSequence: 23,
            targetSlot: 1,
            targetSector: 1,
            expectedSequence: 24,
          ),
        );
        final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
        addTearDown(events.close);
        final held = Completer<void>();
        final entered = Completer<void>();
        final commands = <String>[];
        final uploader = LitchFirmwareUploader(
          connectionToken: 7,
          events: events.stream,
          beforeEnd: () async {
            entered.complete();
            await held.future;
            await store.save(target);
          },
          sendControl: (payload) async {
            final command = payload['cmd']! as String;
            commands.add(command);
            if (command == 'ota_begin') events.add(event(OtaStatusKind.begin));
            if (command == 'ota_end') {
              expect(
                (await FilePendingOtaTargetStore(
                  store.file,
                ).load())!.attempt!.expectedSequence,
                24,
              );
              events.add(event(OtaStatusKind.done));
            }
            return true;
          },
          writeChunk: (bytes, {required bool reliable}) async =>
              events.add(event(OtaStatusKind.progress, rx: bytes.length)),
        );
        final result = uploader.upload(firmwareFile: file, totalBytes: 4);
        await entered.future;
        expect(commands, ['ota_begin']);
        expect(await store.load(), isNull);
        if (persist) {
          held.complete();
        } else {
          held.completeError(const FileSystemException('flush failed'));
        }
        expect(
          await result,
          persist ? isA<OtaUploadFinalized>() : isA<OtaUploadFailed>(),
        );
        expect(
          commands,
          persist ? ['ota_begin', 'ota_end'] : ['ota_begin', 'ota_abort'],
        );
      },
    );
  }

  test(
    'retired status cleanup cannot override pending End ownership',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch-end-cleanup-',
      );
      addTearDown(() => directory.delete(recursive: true));
      final file = await File(
        '${directory.path}/firmware.bin',
      ).writeAsBytes([1]);
      final events = StreamController<OtaStatusEvent>(
        sync: true,
        onCancel: () => Future<void>.error(StateError('native stream retired')),
      );
      addTearDown(events.close);
      final commands = <String>[];
      final uploader = LitchFirmwareUploader(
        connectionToken: 7,
        events: events.stream,
        beforeEnd: () async {},
        sendControl: (payload) async {
          final command = payload['cmd']! as String;
          commands.add(command);
          if (command == 'ota_begin') events.add(event(OtaStatusKind.begin));
          return command != 'ota_end';
        },
        writeChunk: (_, {required bool reliable}) async =>
            events.add(event(OtaStatusKind.progress, rx: 1)),
      );
      final result = await uploader.upload(
        firmwareFile: file,
        totalBytes: 1,
        onStatus: (status) {
          if (status.contains('verification is pending')) {
            throw StateError('presentation disposed');
          }
        },
      );
      expect(result, isA<OtaUploadPendingVerification>());
      expect(commands, ['ota_begin', 'ota_end']);
    },
  );

  test('missing durable owner never submits End', () async {
    final directory = await Directory.systemTemp.createTemp(
      'litch-missing-intent-',
    );
    addTearDown(() => directory.delete(recursive: true));
    final file = await File('${directory.path}/firmware.bin').writeAsBytes([1]);
    final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
    addTearDown(events.close);
    final commands = <String>[];
    final uploader = LitchFirmwareUploader(
      connectionToken: 7,
      events: events.stream,
      sendControl: (payload) async {
        commands.add(payload['cmd']! as String);
        if (payload['cmd'] == 'ota_begin') {
          events.add(event(OtaStatusKind.begin));
        }
        return true;
      },
      writeChunk: (_, {required bool reliable}) async =>
          events.add(event(OtaStatusKind.progress, rx: 1)),
    );
    expect(
      await uploader.upload(firmwareFile: file, totalBytes: 1),
      isA<OtaUploadFailed>(),
    );
    expect(commands, ['ota_begin', 'ota_abort']);
  });

  for (final error in [
    StateError('retired native generation'),
    TimeoutException('native result uncertain'),
    Exception('native failure'),
  ]) {
    test(
      'End exception ${error.runtimeType} remains pending without Abort',
      () async {
        final directory = await Directory.systemTemp.createTemp(
          'litch-end-uncertain-',
        );
        addTearDown(() => directory.delete(recursive: true));
        final file = await File(
          '${directory.path}/firmware.bin',
        ).writeAsBytes([1]);
        final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
        addTearDown(events.close);
        final commands = <String>[];
        final uploader = LitchFirmwareUploader(
          connectionToken: 7,
          events: events.stream,
          beforeEnd: () async {},
          sendControl: (payload) async {
            final command = payload['cmd']! as String;
            commands.add(command);
            if (command == 'ota_begin') events.add(event(OtaStatusKind.begin));
            if (command == 'ota_end') {
              throw error;
            }
            return true;
          },
          writeChunk: (_, {required bool reliable}) async =>
              events.add(event(OtaStatusKind.progress, rx: 1)),
        );
        expect(
          await uploader.upload(firmwareFile: file, totalBytes: 1),
          isA<OtaUploadPendingVerification>(),
        );
        expect(commands, ['ota_begin', 'ota_end']);
      },
    );
  }

  for (final terminal in [
    (
      kind: OtaStatusKind.aborted,
      detail: '',
      message: 'The board canceled the update. Try again.',
    ),
    (
      kind: OtaStatusKind.done,
      detail: '',
      message:
          'The board returned an unexpected update result. Reconnect it and try again.',
    ),
    (
      kind: OtaStatusKind.error,
      detail: 'begin_failed',
      message:
          'The board could not start the update. Contact support for help.',
    ),
  ]) {
    for (final acceptedFirst in [false, true]) {
      test(
        'start terminal ${terminal.kind.name} cannot accept an update acceptedFirst=$acceptedFirst',
        () async {
          final directory = await Directory.systemTemp.createTemp(
            'litch-start-terminal-',
          );
          addTearDown(() => directory.delete(recursive: true));
          final file = await File(
            '${directory.path}/firmware.bin',
          ).writeAsBytes([1, 2, 3, 4]);
          final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
          addTearDown(events.close);
          final commands = <String>[];
          var writes = 0;
          final uploader = LitchFirmwareUploader(
            connectionToken: 7,
            beforeEnd: () async {},
            events: events.stream,
            sendControl: (payload) async {
              final command = payload['cmd']! as String;
              commands.add(command);
              if (command == 'ota_begin') {
                if (acceptedFirst) events.add(event(OtaStatusKind.begin));
                events.add(event(terminal.kind, detail: terminal.detail));
              }
              return true;
            },
            writeChunk: (_, {required bool reliable}) async => ++writes,
          );
          final result = await uploader.upload(
            firmwareFile: file,
            totalBytes: 4,
          );
          expect(result, isA<OtaUploadFailed>());
          expect(result.message, terminal.message);
          expect((result as OtaUploadFailed).detail, terminal.detail);
          expect(writes, 0);
          expect(commands, ['ota_begin', 'ota_abort']);
        },
      );
    }
  }

  test('disconnect before begin attempts abort exactly once', () async {
    final directory = await Directory.systemTemp.createTemp(
      'litch-start-disconnect-cleanup-',
    );
    addTearDown(() => directory.delete(recursive: true));
    final file = await File(
      '${directory.path}/firmware.bin',
    ).writeAsBytes([1, 2, 3, 4]);
    final events = StreamController<OtaStatusEvent>.broadcast(sync: true);
    final disconnects = StreamController<void>.broadcast(sync: true);
    addTearDown(events.close);
    addTearDown(disconnects.close);
    final commands = <String>[];
    var writes = 0;
    final uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      disconnects: disconnects.stream,
      sendControl: (payload) async {
        final command = payload['cmd']! as String;
        commands.add(command);
        if (command == 'ota_begin') disconnects.add(null);
        return true;
      },
      writeChunk: (_, {required bool reliable}) async => ++writes,
    );
    final result = await uploader.upload(firmwareFile: file, totalBytes: 4);
    expect(result, isA<OtaUploadFailed>());
    expect(
      result.message,
      'The board disconnected before the update could start. Reconnect it and try again.',
    );
    expect(writes, 0);
    expect(commands, ['ota_begin', 'ota_abort']);
  });

  test(
    'reports one modern profile with credit waits and reliable split',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      addTearDown(() => temp.delete(recursive: true));
      const int totalBytes = 19;
      final expectedBytes = List<int>.generate(
        totalBytes,
        (int index) => index,
      );
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(expectedBytes);
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      addTearDown(events.close);
      final List<Map<String, Object>> controls = <Map<String, Object>>[];
      final List<bool> reliableWrites = <bool>[];
      final retainedChunks = <List<int>>[];
      final List<String> summaries = <String>[];
      final List<String> statuses = <String>[];
      int acknowledged = 0;
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        sendControl: (Map<String, Object> payload) async {
          controls.add(payload);
          expect(summaries, isEmpty, reason: 'diagnostics wait for cleanup');
          if (payload['cmd'] == 'ota_begin') {
            events.add(
              event(
                OtaStatusKind.begin,
                payload: <String, Object?>{
                  'proto': 2,
                  'chunk': 4,
                  'window': 8,
                  'burst': 2,
                },
              ),
            );
          } else if (payload['cmd'] == 'ota_end') {
            expect(acknowledged, totalBytes);
            // BLE completion arrives after this notification unwinds; a
            // synchronous nested add is rejected by the fixture controller.
            scheduleMicrotask(() {
              events.add(
                event(
                  OtaStatusKind.done,
                  rx: totalBytes,
                  payload: const {'wb': totalBytes, 'wm': 7},
                ),
              );
            });
          }
          return true;
        },
        writeChunk: (List<int> bytes, {required bool reliable}) async {
          reliableWrites.add(reliable);
          retainedChunks.add(bytes);
        },
        options: const LitchFirmwareUploadOptions(
          chunkBytes: 4,
          maximumWindowBytes: 8,
          reliableBurst: 2,
        ),
      );

      expect(
        await uploader.upload(
          firmwareFile: firmware,
          totalBytes: totalBytes,
          onStatus: statuses.add,
          onDiagnostics: (String summary) {
            summaries.add(summary);
            throw StateError(
              'diagnostics cannot turn finalization into failure',
            );
          },
          onProgress: (double progress) {
            final int sent = (progress * totalBytes).round();
            if ((sent % 8 == 0 || sent == totalBytes) && sent > acknowledged) {
              // Let production waitForCapacity observe the full window before
              // the next receive notification restores credit. No timed sleep.
              scheduleMicrotask(() {
                acknowledged = sent;
                events.add(event(OtaStatusKind.progress, rx: sent));
              });
            }
          },
        ),
        isA<OtaUploadFinalized>(),
        reason: statuses.join(' | '),
      );
      expect(reliableWrites, <bool>[false, true, false, true, true]);
      expect(retainedChunks.map((chunk) => chunk.length), [4, 4, 4, 4, 3]);
      expect(retainedChunks.expand((chunk) => chunk).toList(), expectedBytes);
      expect(controls.map((value) => value['cmd']), <String>[
        'ota_begin',
        'ota_end',
      ]);
      expect(controls.last['size'], totalBytes);
      expect(controls.last['crc32'], 'bcb51c15');
      expect(summaries, hasLength(1));
      expect(summaries.single, startsWith('OTA upload metrics: '));
      final Map<String, String> metrics = metricFields(summaries.single);
      expect(metrics, containsPair('outcome', 'finalized'));
      expect(metrics, containsPair('phase', 'terminal'));
      expect(metrics, containsPair('mode', 'negotiated'));
      expect(metrics, containsPair('chunk', '4'));
      expect(metrics, containsPair('window', '8'));
      expect(metrics, containsPair('burst', '2'));
      expect(metrics, containsPair('read_count', '3'));
      expect(metrics, containsPair('sent_bytes', '$totalBytes'));
      expect(metrics, containsPair('rx_bytes', '$totalBytes'));
      expect(metrics, containsPair('fw_write_bytes', '$totalBytes'));
      expect(metrics, containsPair('fw_write_ms', '7'));
      expect(metrics, containsPair('chunk_rsp_count', '3'));
      expect(metrics, containsPair('chunk_no_rsp_count', '2'));
      expect(metrics, containsPair('credit_wait_count', '2'));
      expect(metrics, containsPair('progress_count', '3'));
      for (final MapEntry<String, String> entry in metrics.entries) {
        if (entry.key.endsWith('_ms')) {
          expect(double.parse(entry.value), greaterThanOrEqualTo(0));
        }
      }
    },
  );

  test(
    'failed chunk still reports timings and aborts before diagnostics',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      addTearDown(() => temp.delete(recursive: true));
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      addTearDown(events.close);
      final List<String> order = <String>[];
      final List<String> summaries = <String>[];
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        sendControl: (Map<String, Object> payload) async {
          final String command = payload['cmd']! as String;
          order.add(command);
          if (command == 'ota_begin') events.add(event(OtaStatusKind.begin));
          return true;
        },
        writeChunk: (_, {required bool reliable}) async {
          expect(reliable, isTrue);
          throw StateError('controlled reliable-write failure');
        },
        options: const LitchFirmwareUploadOptions(chunkBytes: 4),
      );
      expect(
        await uploader.upload(
          firmwareFile: firmware,
          totalBytes: 4,
          onDiagnostics: (String summary) {
            order.add('diagnostics');
            summaries.add(summary);
            throw StateError('diagnostics must not mask the upload result');
          },
        ),
        isA<OtaUploadFailed>(),
      );
      expect(order, <String>['ota_begin', 'ota_abort', 'diagnostics']);
      expect(summaries, hasLength(1));
      final Map<String, String> metrics = metricFields(summaries.single);
      expect(metrics, containsPair('outcome', 'failed'));
      expect(metrics, containsPair('phase', 'stream'));
      expect(metrics, containsPair('mode', 'compact'));
      expect(metrics, containsPair('burst', '1'));
      expect(metrics, containsPair('sent_bytes', '0'));
      expect(metrics, containsPair('chunk_rsp_count', '1'));
      expect(metrics, containsPair('chunk_no_rsp_count', '0'));
      expect(metrics, containsPair('credit_wait_count', '0'));
    },
  );

  test(
    'final modern partial burst closes with one reliable boundary',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      addTearDown(() => temp.delete(recursive: true));
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(List<int>.generate(8, (int index) => index));
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      addTearDown(events.close);
      final List<bool> reliableWrites = <bool>[];
      int received = 0;
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        sendControl: (Map<String, Object> payload) async {
          if (payload['cmd'] == 'ota_begin') {
            events.add(
              event(
                OtaStatusKind.begin,
                payload: const <String, Object?>{
                  'proto': 2,
                  'chunk': 4,
                  'window': 12,
                  'burst': 3,
                },
              ),
            );
          } else if (payload['cmd'] == 'ota_end') {
            scheduleMicrotask(
              () => events.add(event(OtaStatusKind.done, rx: received)),
            );
          }
          return true;
        },
        writeChunk: (List<int> bytes, {required bool reliable}) async {
          reliableWrites.add(reliable);
          received += bytes.length;
          events.add(event(OtaStatusKind.progress, rx: received));
        },
        options: const LitchFirmwareUploadOptions(
          chunkBytes: 4,
          maximumWindowBytes: 12,
          reliableBurst: 3,
        ),
      );

      expect(
        await uploader.upload(firmwareFile: firmware, totalBytes: 8),
        isA<OtaUploadFinalized>(),
      );
      expect(reliableWrites, <bool>[false, true]);
    },
  );

  test('waits for begin ack, bounds sent-rx, and waits for final rx', () async {
    final Directory temp = await Directory.systemTemp.createTemp('litch-ota-');
    final File firmware = File('${temp.path}/firmware.bin');
    await firmware.writeAsBytes(List<int>.generate(12, (int index) => index));
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast(sync: true);
    final List<String> commands = <String>[];
    final List<List<int>> writes = <List<int>>[];
    final Completer<void> beginSent = Completer<void>();
    final Completer<void> endSent = Completer<void>();
    final List<Completer<void>> writeObserved = List<Completer<void>>.generate(
      3,
      (_) => Completer<void>(),
    );

    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      sendControl: (Map<String, Object> payload) async {
        final String command = payload['cmd']! as String;
        commands.add(command);
        if (command == 'ota_begin' && !beginSent.isCompleted) {
          beginSent.complete();
        }
        if (command == 'ota_end' && !endSent.isCompleted) {
          endSent.complete();
        }
        return true;
      },
      writeChunk: (List<int> bytes, {required bool reliable}) async {
        writes.add(List<int>.from(bytes));
        final int index = writes.length - 1;
        if (index < writeObserved.length && !writeObserved[index].isCompleted) {
          writeObserved[index].complete();
        }
      },
      options: const LitchFirmwareUploadOptions(
        chunkBytes: 4,
        compactStatusWindowBytes: 4,
        beginTimeout: Duration(milliseconds: 200),
        receiveTimeout: Duration(milliseconds: 200),
        terminalTimeout: Duration(milliseconds: 200),
      ),
    );

    final Future<OtaUploadResult> result = uploader.upload(
      firmwareFile: firmware,
      totalBytes: 12,
    );
    await beginSent.future.timeout(const Duration(seconds: 1));
    expect(commands, <String>['ota_begin']);
    expect(writes, isEmpty);

    events.add(event(OtaStatusKind.begin));
    await writeObserved[0].future.timeout(const Duration(seconds: 1));
    expect(writes.length, 1, reason: 'only one unacknowledged window is sent');

    events.add(event(OtaStatusKind.progress, rx: 4));
    await writeObserved[1].future.timeout(const Duration(seconds: 1));
    expect(writes.length, 2);
    events.add(event(OtaStatusKind.progress, rx: 8));
    await writeObserved[2].future.timeout(const Duration(seconds: 1));
    expect(writes.length, 3);
    expect(commands, isNot(contains('ota_end')));

    events.add(event(OtaStatusKind.progress, rx: 12));
    await endSent.future.timeout(const Duration(seconds: 1));
    expect(commands, contains('ota_end'));
    events.add(event(OtaStatusKind.done, rx: 12));
    expect(await result, isA<OtaUploadFinalized>());

    await events.close();
    await temp.delete(recursive: true);
  });

  test(
    '512-byte sender honors the packed queue receive-credit boundary',
    () async {
      const int chunkBytes = 512;
      const int windowBytes = 15616;
      const int totalBytes = chunkBytes * 31;
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-packed-window-',
      );
      addTearDown(() => temp.delete(recursive: true));
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(
        List<int>.generate(totalBytes, (int index) => index & 0xff),
      );
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      addTearDown(events.close);
      final Completer<void> thirtiethWrite = Completer<void>();
      final Completer<void> thirtyFirstWrite = Completer<void>();
      int writes = 0;
      int buffered = 0;
      int flushes = 0;
      Future<void> flush() async {
        if (buffered == 0) return;
        writes += buffered;
        buffered = 0;
        flushes++;
        if (writes == 30) thirtiethWrite.complete();
        if (writes == 31) thirtyFirstWrite.complete();
      }

      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        flushPendingChunks: flush,
        sendControl: (Map<String, Object> payload) async {
          if (payload['cmd'] == 'ota_begin') {
            events.add(
              event(
                OtaStatusKind.begin,
                payload: const <String, Object?>{
                  'proto': 2,
                  'chunk': chunkBytes,
                  'window': windowBytes,
                  'burst': 48,
                },
              ),
            );
          } else if (payload['cmd'] == 'ota_end') {
            scheduleMicrotask(
              () => events.add(event(OtaStatusKind.done, rx: totalBytes)),
            );
          }
          return true;
        },
        writeChunk: (List<int> bytes, {required bool reliable}) async {
          expect(bytes, hasLength(chunkBytes));
          buffered++;
          if (reliable) await flush();
        },
        options: const LitchFirmwareUploadOptions(
          chunkBytes: chunkBytes,
          maximumWindowBytes: windowBytes,
          reliableBurst: 48,
          beginTimeout: Duration(milliseconds: 200),
          receiveTimeout: Duration(milliseconds: 200),
          terminalTimeout: Duration(milliseconds: 200),
        ),
      );

      final Future<OtaUploadResult> result = uploader.upload(
        firmwareFile: firmware,
        totalBytes: totalBytes,
      );
      await thirtiethWrite.future.timeout(const Duration(seconds: 1));
      await Future<void>.delayed(Duration.zero);
      expect(
        writes,
        30,
        reason: '31st chunk must wait because 31×512 exceeds 15616 bytes',
      );
      expect(
        buffered,
        0,
        reason: 'Partial native batch must flush before credit wait',
      );
      expect(flushes, 1);

      events.add(event(OtaStatusKind.progress, rx: chunkBytes));
      await thirtyFirstWrite.future.timeout(const Duration(seconds: 1));
      events.add(event(OtaStatusKind.progress, rx: totalBytes));
      expect(await result, isA<OtaUploadFinalized>());
      expect(
        flushes,
        2,
        reason: 'Final partial batch retains its reliable boundary',
      );
    },
  );

  test('never writes firmware when begin is not acknowledged', () async {
    final Directory temp = await Directory.systemTemp.createTemp('litch-ota-');
    final File firmware = File('${temp.path}/firmware.bin');
    await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast();
    int writes = 0;
    final List<String> summaries = <String>[];
    final List<String> commands = <String>[];
    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      sendControl: (Map<String, Object> payload) async {
        commands.add(payload['cmd']! as String);
        return true;
      },
      writeChunk: (_, {required bool reliable}) async => writes += 1,
      options: const LitchFirmwareUploadOptions(
        chunkBytes: 4,
        beginTimeout: Duration(milliseconds: 20),
        receiveTimeout: Duration(milliseconds: 20),
        terminalTimeout: Duration(milliseconds: 20),
      ),
    );

    expect(
      await uploader.upload(
        firmwareFile: firmware,
        totalBytes: 4,
        onDiagnostics: summaries.add,
      ),
      isA<OtaUploadFailed>(),
    );
    expect(writes, 0);
    expect(commands, <String>['ota_begin', 'ota_abort']);
    expect(summaries, hasLength(1));
    final Map<String, String> metrics = metricFields(summaries.single);
    expect(metrics, containsPair('outcome', 'failed'));
    expect(metrics, containsPair('phase', 'begin'));
    expect(metrics, containsPair('mode', 'unknown'));
    expect(metrics, containsPair('chunk_rsp_count', '0'));
    expect(metrics, containsPair('chunk_no_rsp_count', '0'));
    await events.close();
    await temp.delete(recursive: true);
  });

  test(
    'uncertain finalization send stays pending without abort after full receipt',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      final List<String> commands = <String>[];
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        sendControl: (Map<String, Object> payload) async {
          final String command = payload['cmd']! as String;
          commands.add(command);
          return command != 'ota_end';
        },
        writeChunk: (_, {required bool reliable}) async {},
        options: const LitchFirmwareUploadOptions(
          chunkBytes: 4,
          compactStatusWindowBytes: 4,
          beginTimeout: Duration(milliseconds: 200),
          receiveTimeout: Duration(milliseconds: 200),
          terminalTimeout: Duration(milliseconds: 200),
        ),
      );

      final Future<OtaUploadResult> result = uploader.upload(
        firmwareFile: firmware,
        totalBytes: 4,
      );
      await Future<void>.delayed(Duration.zero);
      events.add(event(OtaStatusKind.begin));
      await Future<void>.delayed(const Duration(milliseconds: 5));
      events.add(event(OtaStatusKind.progress, rx: 4));

      expect(await result, isA<OtaUploadPendingVerification>());
      expect(commands, <String>['ota_begin', 'ota_end']);
      await events.close();
      await temp.delete(recursive: true);
    },
  );

  test('unconfirmed finalization stays pending without abort', () async {
    final Directory temp = await Directory.systemTemp.createTemp('litch-ota-');
    final File firmware = File('${temp.path}/firmware.bin');
    await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast(sync: true);
    final List<String> commands = <String>[];
    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      sendControl: (Map<String, Object> payload) async {
        commands.add(payload['cmd']! as String);
        return true;
      },
      writeChunk: (_, {required bool reliable}) async {},
      options: const LitchFirmwareUploadOptions(
        chunkBytes: 4,
        compactStatusWindowBytes: 4,
        beginTimeout: Duration(milliseconds: 200),
        receiveTimeout: Duration(milliseconds: 200),
        terminalTimeout: Duration(milliseconds: 20),
      ),
    );

    final Future<OtaUploadResult> result = uploader.upload(
      firmwareFile: firmware,
      totalBytes: 4,
    );
    await Future<void>.delayed(Duration.zero);
    events.add(event(OtaStatusKind.begin));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    events.add(event(OtaStatusKind.progress, rx: 4));

    expect(await result, isA<OtaUploadPendingVerification>());
    expect(commands, <String>['ota_begin', 'ota_end']);
    await events.close();
    await temp.delete(recursive: true);
  });

  test('post-End persistence error retains pending verification', () async {
    final Directory temp = await Directory.systemTemp.createTemp('litch-ota-');
    final File firmware = File('${temp.path}/firmware.bin');
    await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast(sync: true);
    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      sendControl: (Map<String, Object> payload) async {
        if (payload['cmd'] == 'ota_end') {
          scheduleMicrotask(
            () => events.add(
              event(OtaStatusKind.error, detail: 'persist_not_ready'),
            ),
          );
        }
        return true;
      },
      writeChunk: (_, {required bool reliable}) async {},
      options: const LitchFirmwareUploadOptions(
        chunkBytes: 4,
        compactStatusWindowBytes: 4,
        beginTimeout: Duration(milliseconds: 200),
        receiveTimeout: Duration(milliseconds: 200),
        terminalTimeout: Duration(milliseconds: 200),
      ),
    );

    final Future<OtaUploadResult> pending = uploader.upload(
      firmwareFile: firmware,
      totalBytes: 4,
    );
    await Future<void>.delayed(Duration.zero);
    events.add(event(OtaStatusKind.begin));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    events.add(event(OtaStatusKind.progress, rx: 4));

    final OtaUploadResult result = await pending;
    expect(result, isA<OtaUploadPendingVerification>());
    expect(result.message, contains('verification is pending'));
    await events.close();
    await temp.delete(recursive: true);
  });

  test('disconnect releases a backpressured upload immediately', () async {
    final Directory temp = await Directory.systemTemp.createTemp('litch-ota-');
    final File firmware = File('${temp.path}/firmware.bin');
    await firmware.writeAsBytes(List<int>.generate(8, (int index) => index));
    final StreamController<OtaStatusEvent> events =
        StreamController<OtaStatusEvent>.broadcast(sync: true);
    final StreamController<void> disconnects = StreamController<void>.broadcast(
      sync: true,
    );
    final Completer<void> firstWrite = Completer<void>();
    final List<String> commands = <String>[];
    int writes = 0;
    final LitchFirmwareUploader uploader = LitchFirmwareUploader(
      connectionToken: 7,
      beforeEnd: () async {},
      events: events.stream,
      disconnects: disconnects.stream,
      sendControl: (Map<String, Object> payload) async {
        commands.add(payload['cmd']! as String);
        return true;
      },
      writeChunk: (_, {required bool reliable}) async {
        writes += 1;
        firstWrite.complete();
      },
      options: const LitchFirmwareUploadOptions(
        chunkBytes: 4,
        compactStatusWindowBytes: 4,
        maximumWindowBytes: 4,
        beginTimeout: Duration(milliseconds: 200),
        receiveTimeout: Duration(seconds: 5),
        terminalTimeout: Duration(seconds: 5),
      ),
    );

    final Future<OtaUploadResult> result = uploader.upload(
      firmwareFile: firmware,
      totalBytes: 8,
    );
    await Future<void>.delayed(Duration.zero);
    events.add(event(OtaStatusKind.begin));
    await firstWrite.future;
    disconnects.add(null);

    expect(
      await result.timeout(const Duration(milliseconds: 250)),
      isA<OtaUploadFailed>(),
    );
    expect(writes, 1);
    expect(commands, <String>['ota_begin', 'ota_abort']);
    await disconnects.close();
    await events.close();
    await temp.delete(recursive: true);
  });

  test(
    'terminal error prevents another chunk even with window capacity',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(List<int>.generate(12, (int index) => index));
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      final List<String> commands = <String>[];
      int writes = 0;
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        sendControl: (Map<String, Object> payload) async {
          commands.add(payload['cmd']! as String);
          return true;
        },
        writeChunk: (_, {required bool reliable}) async {
          writes += 1;
          if (writes == 1) {
            events.add(event(OtaStatusKind.error));
          }
        },
        options: const LitchFirmwareUploadOptions(
          chunkBytes: 4,
          compactStatusWindowBytes: 8,
          maximumWindowBytes: 8,
          beginTimeout: Duration(milliseconds: 200),
          receiveTimeout: Duration(seconds: 5),
          terminalTimeout: Duration(seconds: 5),
        ),
      );

      final Future<OtaUploadResult> result = uploader.upload(
        firmwareFile: firmware,
        totalBytes: 12,
      );
      await Future<void>.delayed(Duration.zero);
      events.add(event(OtaStatusKind.begin));

      expect(await result, isA<OtaUploadFailed>());
      expect(writes, 1);
      expect(commands, <String>['ota_begin', 'ota_abort']);
      await events.close();
      await temp.delete(recursive: true);
    },
  );

  test(
    'reports reboot pending when disconnect follows final receipt and end',
    () async {
      final Directory temp = await Directory.systemTemp.createTemp(
        'litch-ota-',
      );
      final File firmware = File('${temp.path}/firmware.bin');
      await firmware.writeAsBytes(<int>[1, 2, 3, 4]);
      final StreamController<OtaStatusEvent> events =
          StreamController<OtaStatusEvent>.broadcast(sync: true);
      final StreamController<void> disconnects =
          StreamController<void>.broadcast(sync: true);
      final List<String> commands = <String>[];
      final List<String> statuses = <String>[];
      final List<String> summaries = <String>[];
      final LitchFirmwareUploader uploader = LitchFirmwareUploader(
        connectionToken: 7,
        beforeEnd: () async {},
        events: events.stream,
        disconnects: disconnects.stream,
        sendControl: (Map<String, Object> payload) async {
          commands.add(payload['cmd']! as String);
          return true;
        },
        writeChunk: (_, {required bool reliable}) async {},
        options: const LitchFirmwareUploadOptions(
          chunkBytes: 4,
          compactStatusWindowBytes: 4,
          beginTimeout: Duration(milliseconds: 200),
          receiveTimeout: Duration(milliseconds: 200),
          terminalTimeout: Duration(milliseconds: 200),
        ),
      );

      final Future<OtaUploadResult> result = uploader.upload(
        firmwareFile: firmware,
        totalBytes: 4,
        onStatus: statuses.add,
        onDiagnostics: summaries.add,
      );
      await Future<void>.delayed(Duration.zero);
      events.add(event(OtaStatusKind.begin));
      await Future<void>.delayed(const Duration(milliseconds: 5));
      events.add(event(OtaStatusKind.progress, rx: 4));
      while (!commands.contains('ota_end')) {
        await Future<void>.delayed(Duration.zero);
      }
      disconnects.add(null);

      expect(await result, isA<OtaUploadPendingVerification>());
      expect(summaries, hasLength(1));
      expect(
        metricFields(summaries.single),
        containsPair('outcome', 'pending_verification'),
      );
      expect(commands, <String>['ota_begin', 'ota_end']);
      expect(statuses, contains('Board accepted the update.'));
      expect(statuses.join(' '), isNot(contains('4 bytes')));
      expect(
        statuses.last,
        'Update sent. Board disconnected; verification is pending.',
      );
      await disconnects.close();
      await events.close();
      await temp.delete(recursive: true);
    },
  );
}
