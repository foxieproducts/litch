import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

class _Trace implements AppTraceSink {
  final retirements = <Map<String, Object?>>[];
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'ble.connection_session_retired') {
      retirements.add(Map.of(fields));
    }
  }
}

void main() {
  testWidgets(
    'Object timeout renews the peripheral lifetime after local GATT cleanup',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final bootstrapGate = Completer<void>();
      final ble = VirtualLitchGattBle(native)
        ..bootstrapWriteCompletionGate = bootstrapGate;
      final trace = _Trace();
      final service = LitchBleService(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
        appTrace: trace,
      );
      addTearDown(() => ble.disposeJourney(tester, service));
      const board = 'virtual-litch-production';
      int now() => tester.binding.clock.now().millisecondsSinceEpoch;
      Future<void> tick() async {
        await tester.pump();
        if (ble.hasPendingOperations) {
          await ble.flush(tester, maxOperations: 1);
        } else {
          await ble.command(tester, {
            'op': 'advance',
            'ms': 10,
          }, flushOperations: false);
          await tester.pump(const Duration(milliseconds: 10));
        }
      }

      Future<void> finish(
        Future<dynamic> work, {
        int budgetMs = 10000,
        bool realTime = false,
      }) async {
        var done = false;
        Object? failure;
        work.then<void>(
          (_) => done = true,
          onError: (Object error, StackTrace _) {
            failure = error;
            done = true;
          },
        );
        final start = now();
        while (!done && now() - start < budgetMs) {
          await tick();
          if (realTime) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 10)),
            );
          }
        }
        expect(
          done,
          isTrue,
          reason: 'Production operation exceeded $budgetMs ms',
        );
        if (failure != null) throw failure!;
      }

      await finish(service.connect(board));
      ConnectionSession? first;
      final opening = service
          .openConnectionSession(board)
          .then((value) => first = value);
      for (
        var turn = 0;
        turn < 100 && !ble.bootstrapWriteCompletionHeld;
        ++turn
      ) {
        await tick();
      }
      expect(ble.bootstrapWriteCompletionHeld, isTrue);
      expect(
        ble.emitted.any((packet) => packet.channel == 'ota_control'),
        isTrue,
        reason:
            'The real capability response arrives before local write completion',
      );
      expect(
        ble.nativeSessionActivated,
        isFalse,
        reason:
            'Concurrent Hello waits for the in-flight bootstrap write and capability owner',
      );
      expect(ble.nativeDisconnectRequests, 0);
      bootstrapGate.complete();
      ble.bootstrapWriteCompletionGate = null;
      await finish(opening);
      expect(ble.nativeDisconnectRequests, 0);
      debugPrint(
        'NATIVE_CLEANUP checkpoint=bootstrap_race reply_before_write=true premature_hello=false renewal=0',
      );
      expect(first?.isActivated, isTrue);
      final oldEpoch = ble.emitted.last.epoch;
      ble.deliverPhysicalConnectionEvents = false;
      ble.heldObjectProgressToken = first!.objectStreamToken;
      Object? deadline;
      final submission = first!
          .sendObject(
            ConnectionOtaCodec.controlObjectOpcode,
            ConnectionOtaCodec.encodeControl(
              Uint8List.fromList(utf8.encode('{"cmd":"info"}')),
              maximumBodyBytes: 230,
            ).single,
          )
          .then<void>(
            (_) {},
            onError: (Object error, StackTrace _) => deadline = error,
          );
      await finish(submission, realTime: true);
      expect(deadline, isA<TimeoutException>());
      expect(trace.retirements.single['cause'], 'object_deadline');
      expect(
        ble.held,
        isNotEmpty,
        reason: 'Hold real native Progress, not a synthetic failure',
      );
      final lateProgress = ble.held.first;
      expect(lateProgress.epoch, oldEpoch);
      expect(first!.isActivated, isFalse);
      debugPrint(
        'NATIVE_CLEANUP checkpoint=object_deadline peripheral_epoch=$oldEpoch',
      );

      final writesBefore = ble.writes.length;
      final requestedBefore = ble.nativeDisconnectRequests;
      final restartAt = now();
      ble.deliverRequestedDisconnects = true;
      ConnectionSession? localReplacement;
      await finish(
        service
            .openConnectionSession(board)
            .then((value) => localReplacement = value),
      );
      expect(
        localReplacement,
        isNull,
        reason: 'Actual disconnect retires the local bootstrap in progress',
      );
      expect(ble.localCancellations, greaterThanOrEqualTo(1));
      expect(ble.nativeDisconnectRequests, requestedBefore + 1);
      expect(ble.nativeDisconnectsDelivered, 1);
      expect(ble.nativeEpoch, 0);
      final directProbes = ble.writes
          .skip(writesBefore)
          .where(
            (write) =>
                write['channel'] == 'ota_control' &&
                utf8.decode((write['bytes'] as List).cast<int>()) ==
                    '{"cmd":"info"}',
          )
          .length;
      expect(directProbes, 1);
      debugPrint(
        'NATIVE_CLEANUP checkpoint=native_disconnect requested=1 delivered=1 '
        'old_peripheral_epoch=$oldEpoch direct_probes=$directProbes',
      );
      ble.heldObjectProgressToken = null;
      ble.held.clear();
      // The remembered-board caller uses the ordinary connection owner again;
      // the platform now opens a physical link because production closed it.
      await finish(service.connect(board));
      ConnectionSession? renewed;
      await finish(
        service.openConnectionSession(board).then((value) => renewed = value),
      );
      expect(renewed?.isActivated, isTrue);
      expect(ble.nativeEpoch, isNot(oldEpoch));
      expect(
        now() - restartAt,
        lessThan(5000),
        reason:
            'Recovery must not depend on the five-second board idle watchdog',
      );
      final renewedToken = service.connectionSessionToken;
      // Deliver the original callback address, bypassing normal cancellation
      // exactly as the existing adapter permits for late-callback checks.
      lateProgress.pipe!.deliver(lateProgress.bytes, lateCallback: true);
      await tester.pump();
      expect(service.connectionSessionToken, renewedToken);
      expect(renewed!.isActivated, isTrue);
      String? version;
      await finish(
        service
            .fetchBoardFirmwareVersion(deviceId: board)
            .then((value) => version = value),
      );
      expect(version, isNotEmpty);
      debugPrint(
        'NATIVE_CLEANUP checkpoint=physical_renewal activated=true '
        'stale_adopted=false domain_reply=true',
      );
      // Healthy refresh stays on the negotiated Object lane, including info.
      final healthyWrites = ble.writes.length;
      await finish(
        service.fetchDeviceCapabilities(deviceId: board, refresh: true),
      );
      expect(
        ble.writes
            .skip(healthyWrites)
            .where((write) => write['channel'] == 'ota_control'),
        isEmpty,
      );
      expect(ble.nativeDisconnectRequests, requestedBefore + 1);
      await finish(service.disconnect());
      // SDK teardown must also join the cached fake-owned disposal future.
      await finish(service.dispose());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );
}
