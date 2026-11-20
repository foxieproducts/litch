import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

// Controlled transport faults only: this child contains no board, Home or ACK
// model. Tests exercise the real process wrapper and SDK future ownership.
const _childSource = r'''
import json, signal, sys
hang = sys.argv[1] == 'hang'
if hang:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
late = None
for line in sys.stdin:
    request = json.loads(line)
    if request['op'] == 'exit':
        sys.exit(3)
    if request['op'] == 'silent':
        late = request['id']
        continue
    if late is not None:
        print(json.dumps({'id': late, 'ok': True, 'packets': []}), flush=True)
        late = None
    print(json.dumps({'id': request['id'], 'ok': request['op'] != 'write',
                      'error': 'controlled native envelope failure',
                      'packets': []}), flush=True)
if hang:
    while True:
        signal.pause()
''';

Future<({Process process, VirtualLitchProcess native})> _startChild({
  bool hangOnEof = false,
}) async {
  final process = await Process.start('python3', <String>[
    '-u',
    '-c',
    _childSource,
    hangOnEof ? 'hang' : 'exit',
  ]);
  final native = VirtualLitchProcess.fromProcess(
    process,
    requestTimeout: const Duration(milliseconds: 250),
    shutdownTimeout: const Duration(milliseconds: 100),
    terminateTimeout: const Duration(milliseconds: 100),
  );
  return (process: process, native: native);
}

// Independent final resource guard also makes an intentionally broken wrapper
// safe to test. Reaping assertions below happen before this fallback cleanup.
Future<void> _reapAfterTest(Process process) async {
  process.kill(ProcessSignal.sigkill);
  await process.exitCode;
}

class _FailingDisposeService extends LitchBleService {
  _FailingDisposeService(VirtualLitchBle ble, this.failure)
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: ble);
  final StateError failure;

  @override
  Future<void> dispose() async {
    await super.dispose();
    throw failure;
  }
}

void main() {
  test(
    'request timeout releases its ID and ignores a late child response',
    () async {
      final child = await _startChild();
      addTearDown(() => _reapAfterTest(child.process));
      await child.native.request({'op': 'ready'});
      await expectLater(
        child.native.request({'op': 'silent'}),
        throwsA(isA<TimeoutException>()),
      );
      expect(child.native.pendingRequestCount, 0);
      // The child now sends the timed-out ID before the current request's reply.
      expect((await child.native.request({'op': 'echo'}))['ok'], isTrue);
      expect(child.native.pendingRequestCount, 0);
      await child.native.close();
      expect(await child.native.exitCode, 0);
    },
  );

  test(
    'shutdown timeout kills and reaps a child that ignores SIGTERM',
    () async {
      final child = await _startChild(hangOnEof: true);
      addTearDown(() => _reapAfterTest(child.process));
      await child.native.request({'op': 'ready'});
      bool reaped = false;
      unawaited(
        child.native.exitCode.then((_) {
          reaped = true;
        }),
      );
      await expectLater(child.native.close(), throwsA(isA<TimeoutException>()));
      expect(reaped, isTrue, reason: 'close must await exit after SIGKILL');
      expect(child.native.pendingRequestCount, 0);
    },
  );

  testWidgets(
    'direct command reports child exit once and rejects later writes',
    (tester) async {
      final child = (await tester.runAsync(_startChild))!;
      addTearDown(() => tester.runAsync(() => _reapAfterTest(child.process)));
      final ble = VirtualLitchBle(child.native);
      await expectLater(
        ble.command(tester, {'op': 'exit'}),
        throwsA(
          isA<StateError>().having(
            (error) => error.message,
            'native cause',
            'VirtualLitch stdout closed',
          ),
        ),
      );
      await expectLater(
        ble.command(tester, {'op': 'echo'}),
        throwsA(isA<StateError>()),
      );
      expect(child.native.pendingRequestCount, 0);
      await tester.runAsync(() async {
        await expectLater(
          child.native.close(),
          throwsA(
            isA<StateError>().having(
              (error) => error.message,
              'exit status',
              'VirtualLitch exited 3',
            ),
          ),
        );
        expect(await child.native.exitCode, 3);
      });
      expect(
        tester.takeException(),
        isNull,
        reason:
            'the direct path must not turn an exit into a null assertion or broken pipe',
      );
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  testWidgets(
    'service dispose failure still closes and reaps the native child',
    (tester) async {
      final child = (await tester.runAsync(_startChild))!;
      addTearDown(() => tester.runAsync(() => _reapAfterTest(child.process)));
      final ble = VirtualLitchBle(child.native);
      final failure = StateError('controlled service dispose failure');
      final service = _FailingDisposeService(ble, failure);
      bool reaped = false;
      unawaited(
        child.native.exitCode.then((_) {
          reaped = true;
        }),
      );
      await expectLater(
        ble.disposeJourney(tester, service),
        throwsA(same(failure)),
      );
      await tester.pump();
      expect(reaped, isTrue);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  testWidgets(
    'native envelope failure settles the service future and reaps after flush failure',
    (tester) async {
      final child = (await tester.runAsync(_startChild))!;
      addTearDown(() => tester.runAsync(() => _reapAfterTest(child.process)));
      // Console first discovers board capabilities via the real bootstrap write.
      // The controlled child rejects that SDK operation before any command runs.
      final ble = VirtualLitchBle(child.native, enableOtaControl: true);
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
      )..debugSetConnectedDevice('failure-fixture');
      bool? sent;
      unawaited(
        service.sendConsoleLine('failure-fixture', 'help').then((value) {
          sent = value;
        }),
      );
      await tester.pump();
      expect(
        ble.writes.where((write) => write['channel'] == 'ota_control'),
        isNotEmpty,
      );
      bool reaped = false;
      unawaited(
        child.native.exitCode.then((_) {
          reaped = true;
        }),
      );
      await expectLater(
        ble.disposeJourney(tester, service),
        throwsA(
          isA<StateError>().having(
            (error) => error.message,
            'native cause',
            contains('controlled native envelope failure'),
          ),
        ),
      );
      await tester.pump();
      expect(
        sent,
        isFalse,
        reason:
            'the production service await must settle from the failed SDK operation',
      );
      expect(child.native.pendingRequestCount, 0);
      expect(reaped, isTrue);
      expect(
        tester.takeException(),
        isNull,
        reason:
            'the original error crosses the test boundary once, not as a null assertion',
      );
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );
}
