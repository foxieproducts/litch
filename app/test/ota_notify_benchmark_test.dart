import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/ota/ota_notify_benchmark.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';

Uint8List _packet(int size, int run, int ordinal) {
  final bytes = Uint8List.fromList(List<int>.filled(size, 1));
  bytes[1] = 0x1e;
  bytes[2] = 0x1f;
  for (var index = 4; index < 16; index++) {
    bytes[index] =
        0x10 |
        (index < 12
            ? (run >> ((index - 4) * 4)) & 15
            : (ordinal >> ((index - 12) * 4)) & 15);
  }
  return bytes;
}

class _NotifyBle extends RebootReplyBle {
  final console = StreamController<List<int>>.broadcast(sync: true);
  final requests = <Map<String, dynamic>>[];
  final probeFacts = <String, int>{};
  int consoleSubscriptions = 0;
  void Function(Map<String, dynamic>)? onNotify;

  @override
  Stream<List<int>> subscribeToCharacteristic(QualifiedCharacteristic c) {
    if (c.characteristicId == BleProtocol.consoleStreamCharacteristic) {
      consoleSubscriptions++;
      return console.stream;
    }
    return super.subscribeToCharacteristic(c);
  }

  void terminal(Map<String, dynamic> request, {int? ok, int fail = 0}) {
    final size = (request['size'] as int).clamp(1, mtu - 3).clamp(1, 512);
    emitStatus(
      jsonEncode({
        'st': 'bench_notify',
        'detail':
            'size=$size ok=${ok ?? request['count']} fail=$fail '
            'elapsed_ms=200 rate_Bps=1000 run=${request['run']} '
            'count=${request['count']}',
      }),
    );
  }

  void send(Map<String, dynamic> request, {int? limit, int fail = 0}) {
    final size = (request['size'] as int).clamp(1, mtu - 3).clamp(1, 512);
    for (
      var ordinal = 0;
      ordinal < (limit ?? request['count'] as int);
      ordinal++
    ) {
      console.add(_packet(size, request['run'] as int, ordinal));
    }
    terminal(request, fail: fail);
  }

  @override
  Future<void> commandWrite(List<int> bytes) async {
    final request = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    if (request['cmd'] == 'object_bench_probe') {
      emitStatus(
        jsonEncode({
          's': 'object_bench_probe',
          'q': request['q'],
          'schema': 1,
          'mtu': mtu,
          ...probeFacts,
        }),
      );
    } else if (request['cmd'] == 'notify_test') {
      requests.add(request);
      if (onNotify != null) {
        onNotify!(request);
      } else {
        send(request);
      }
    } else {
      await super.commandWrite(bytes);
    }
  }
}

Future<LitchBleService> _connect(_NotifyBle ble) async {
  final service = LitchBleService(
    ble: ble,
    pendingOtaStore: MemoryOtaTargetStore(),
  );
  await connectBoard(service, ble);
  await service.openConnectionSession('board-1');
  await Future<void>.delayed(Duration.zero);
  addTearDown(() async {
    await disposeBoard(service, ble);
    await ble.console.close();
  });
  return service;
}

void main() {
  test(
    'Console byte observer precedes parser without another subscription or binary emission',
    () async {
      final ble = _NotifyBle()..dropFirstVersionReply = false;
      final service = await _connect(ble);
      expect(ble.consoleSubscriptions, 1);
      final observed = <List<int>>[];
      final errors = <Object>[];
      final snapshots = <List<String>>[];
      final textSub = service.consoleStream.listen(snapshots.add);
      final cancel = service.observeConsolePackets(
        deviceId: 'board-1',
        connectionToken: service.connectionSessionToken,
        onPacket: (bytes) {
          expect(
            service.consoleLinesSnapshot.where(
              (line) => line.contains('tap order'),
            ),
            isEmpty,
          );
          expect(() => bytes[0] = 0, throwsUnsupportedError);
          observed.add(bytes);
        },
        onError: errors.add,
      );
      final before = snapshots.length;
      ble.console.add(_packet(244, 0xffffffff, 199));
      await Future<void>.delayed(Duration.zero);
      expect(observed.single.length, 244);
      expect(snapshots.length, before);
      ble.console.add(utf8.encode('tap order\n'));
      await Future<void>.delayed(Duration.zero);
      expect(
        service.consoleLinesSnapshot.any((line) => line.contains('tap order')),
        isTrue,
      );
      cancel();
      ble.console.add(_packet(244, 1, 0));
      expect(observed.length, 2);
      expect(errors, isEmpty);
      expect(ble.consoleSubscriptions, 1);
      await textSub.cancel();
    },
  );

  for (final (mtu, count) in [(517, 200), (247, 200), (23, 200), (517, 300)]) {
    test(
      'phone receipt validates both $count-packet sizes at MTU $mtu and ignores old run',
      () async {
        final ble = _NotifyBle()
          ..mtu = mtu
          ..dropFirstVersionReply = false;
        final service = await _connect(ble);
        Map<String, dynamic>? prior;
        ble.onNotify = (request) {
          if (prior case final old?) {
            ble.console.add(_packet(20, old['run'] as int, 0));
            ble.terminal(old);
          }
          ble.send(request);
          prior = request;
        };
        final progress = <(int, int)>[];
        final result = await OtaNotifyBenchmark.run(
          service,
          packetCount: count,
          onProgress: (received, total) => progress.add((received, total)),
        );
        expect(result['ok'], isTrue, reason: '$result');
        expect(result['link'], {
          'mtu': mtu,
          'interval_units': null,
          'phy_known': null,
          'tx_phy': null,
          'rx_phy': null,
          'dle_known': null,
        });
        final runs = result['runs']! as List<Map<String, Object?>>;
        expect(runs.length, 2);
        expect(runs.first['run'], isNot(runs.last['run']));
        for (var index = 0; index < runs.length; index++) {
          final run = runs[index];
          final expected = OtaNotifyBenchmark.packetSizes[index].clamp(
            1,
            mtu - 3,
          );
          expect(run['packet_size'], expected);
          expect(run['rx_packets'], count);
          expect(run['rx_bytes'], expected * count);
          expect(run['first_us'], lessThanOrEqualTo(run['last_us']!));
          expect(run['missing_packets'], 0);
        }
        expect(runs.last['old_run_packets'], 1);
        expect(progress.last.$1, result['bytes']);
        expect(progress.last.$2, result['bytes']);
        for (var index = 1; index < progress.length; index++) {
          expect(
            progress[index].$1,
            greaterThanOrEqualTo(progress[index - 1].$1),
          );
          expect(progress[index].$2, progress.first.$2);
        }
        expect(ble.consoleSubscriptions, 1);
        expect(OtaNotifyBenchmark.deadline, const Duration(seconds: 120));
      },
    );
  }

  test(
    'SDK retry refusals preserve exact receipt and reported link facts',
    () async {
      final ble = _NotifyBle()
        ..mtu = 517
        ..dropFirstVersionReply = false;
      ble.probeFacts.addAll({
        'interval': 12,
        'phy_known': 1,
        'tx_phy': 2,
        'rx_phy': 1,
        'dle_known': 1,
      });
      final service = await _connect(ble);
      ble.onNotify = (request) => ble.send(request, fail: 450);
      final result = await OtaNotifyBenchmark.run(service);
      expect(result['ok'], isTrue, reason: '$result');
      expect(result['link'], {
        'mtu': 517,
        'interval_units': 12,
        'phy_known': 1,
        'tx_phy': 2,
        'rx_phy': 1,
        'dle_known': 1,
      });
      for (final run in result['runs']! as List<Map<String, Object?>>) {
        expect(run['rx_packets'], 200);
        expect(run['rx_bytes'], (run['packet_size']! as int) * 200);
        expect(run['missing_packets'], 0);
        expect((run['sdk'] as Map)['ok'], 200);
        expect((run['sdk'] as Map)['fail'], 450);
      }
      expect(ble.requests.length, 2);
      expect(ble.consoleSubscriptions, 1);
    },
  );

  for (final fault in ['corrupt', 'length', 'duplicate', 'gap']) {
    test(
      'notify $fault retains unique receipt and SDK evidence before failing',
      () async {
        final ble = _NotifyBle()..dropFirstVersionReply = false;
        final service = await _connect(ble);
        ble.onNotify = (request) {
          final run = request['run'] as int;
          ble.console.add(_packet(244, run, 0));
          final bytes = _packet(
            fault == 'length' ? 243 : 244,
            run,
            fault == 'duplicate'
                ? 0
                : fault == 'gap'
                ? 2
                : 1,
          );
          if (fault == 'corrupt') bytes[20] = 2;
          ble.console.add(bytes);
          for (
            var ordinal = fault == 'duplicate'
                ? 1
                : fault == 'gap'
                ? 3
                : 2;
            ordinal < 200;
            ordinal++
          ) {
            ble.console.add(_packet(244, run, ordinal));
          }
          ble.terminal(
            request,
            ok: fault == 'gap' ? 199 : 200,
            fail: fault == 'gap' ? 1 : 0,
          );
        };
        final result = await OtaNotifyBenchmark.run(service);
        expect(result['ok'], isFalse);
        final run = (result['runs']! as List).single as Map;
        final unique = fault == 'duplicate' ? 200 : 199;
        expect(run['rx_packets'], unique);
        expect(run['rx_bytes'], unique * 244);
        expect(result['rx_bytes'], unique * 244);
        expect(
          run['observed_packets'],
          fault == 'duplicate'
              ? 201
              : fault == 'gap'
              ? 199
              : 200,
        );
        expect(run['missing_packets'], 200 - unique);
        expect((run['sdk'] as Map)['ok'], fault == 'gap' ? 199 : 200);
        expect((run['sdk'] as Map)['fail'], fault == 'gap' ? 1 : 0);
        expect(
          run['packet_error'],
          fault == 'gap'
              ? 'notify_gap'
              : fault == 'duplicate'
              ? 'notify_duplicate'
              : 'notify_payload',
        );
        expect(
          result['error_code'],
          fault == 'gap'
              ? 'notify_sdk_result'
              : fault == 'duplicate'
              ? 'notify_duplicate'
              : 'notify_payload',
        );
        expect(
          run[fault == 'duplicate'
              ? 'duplicate_packets'
              : fault == 'gap'
              ? 'gap_packets'
              : 'corrupt_packets'],
          1,
        );
        expect(ble.requests.length, 1);
      },
    );
  }

  test('SDK success cannot replace a missing last phone packet', () async {
    final ble = _NotifyBle()..dropFirstVersionReply = false;
    final service = await _connect(ble);
    ble.onNotify = (request) => ble.send(request, limit: 199);
    final result = await OtaNotifyBenchmark.run(
      service,
      timeout: const Duration(milliseconds: 200),
    );
    expect(result['ok'], isFalse);
    expect(result['error_kind'], 'deadline');
    expect(result['error_code'], 'notify_deadline');
    final run = (result['runs']! as List).single as Map;
    expect(run['rx_packets'], 199);
    expect(run['missing_packets'], 1);
    expect((run['sdk'] as Map)['ok'], 200);
    final before = Map.of(run);
    ble.console.add(_packet(244, ble.requests.single['run'] as int, 199));
    expect(
      run,
      before,
      reason: 'Late delivery cannot revise a frozen failed result.',
    );
  });

  test('matching SDK success waits for a late final Console packet', () async {
    final ble = _NotifyBle()..dropFirstVersionReply = false;
    final service = await _connect(ble);
    ble.onNotify = (request) {
      ble.send(request, limit: 199);
      scheduleMicrotask(() {
        final size = (request['size'] as int).clamp(1, ble.mtu - 3);
        ble.console.add(_packet(size, request['run'] as int, 199));
      });
    };
    final result = await OtaNotifyBenchmark.run(service);
    expect(result['ok'], isTrue, reason: '$result');
    for (final run in result['runs']! as List<Map<String, Object?>>) {
      expect(run['rx_packets'], 200);
      expect(run['missing_packets'], 0);
      expect(run['gap_packets'], 0);
    }
  });

  for (final fault in [
    'sdk',
    'board_error',
    'stream_error',
    'replacement',
    'epoch',
    'readiness',
  ]) {
    test(
      'notify $fault retires observation without accepting admission as receipt',
      () async {
        final ble = _NotifyBle()..dropFirstVersionReply = false;
        final service = await _connect(ble);
        ble.onNotify = (request) {
          switch (fault) {
            case 'sdk':
              ble.terminal(request, ok: 199, fail: 1);
            case 'board_error':
              ble.emitStatus('{"st":"error","detail":"notify_test_failed"}');
            case 'stream_error':
              ble.console.addError(StateError('injected native error'));
            case 'replacement':
              service.debugStartConsoleMirror('board-1');
            case 'epoch':
              ble.console.add(_packet(244, request['run'] as int, 0));
              ble.connections.last.add(
                const ConnectionStateUpdate(
                  deviceId: 'board-1',
                  connectionState: DeviceConnectionState.disconnected,
                  failure: null,
                ),
              );
            case 'readiness':
              service.debugSetDeviceCapabilities(
                'board-1',
                const LitchDeviceCapabilities(
                  contractVersion: 3,
                  hardwareRevision: LitchHardwareRevision.v3_3,
                  capabilityMask: LitchDeviceCapabilities.ownerPairingBit,
                  explicitlyReported: true,
                ),
              );
              ble.console.add(_packet(244, request['run'] as int, 0));
          }
        };
        final result = await OtaNotifyBenchmark.run(service);
        expect(result['ok'], isFalse);
        expect((result['runs']! as List).length, 1);
        expect(result['rx_bytes'], fault == 'epoch' ? 244 : 0);
        expect(ble.requests.length, 1);
        expect(
          result['error_code'],
          matches(RegExp(r'^[A-Za-z0-9_.:-]{1,64}$')),
        );
        if (fault == 'board_error') {
          expect(
            result['error_code'],
            'notify_board_error:_notify_test_failed',
          );
        } else if (fault == 'stream_error') {
          expect(result['error_code'], 'injected_native_error');
        } else if (fault == 'replacement') {
          expect(result['error_code'], 'Console_subscription_retired.');
        }
        if (fault == 'sdk') {
          final run = (result['runs']! as List).single as Map;
          expect((run['sdk'] as Map)['fail'], 1);
        }
      },
    );
  }
}
