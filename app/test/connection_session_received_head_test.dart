import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_frame_store.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';

void main() {
  test(
    'closed Object credit reselects interactive before unstarted bulk',
    () async {
      final peer = _PendingPeer(objectCapacity: 2, batchObjects: true);
      addTearDown(peer.close);
      await peer.session.activate();
      await peer.session.sendObjectBatch([
        (opcode: 0x60, body: Uint8List.fromList([0])),
      ], completion: ConnectionSessionObjectCompletion.submitted);
      expect(peer.nativeReturns, 1);
      expect(peer.objects, hasLength(1));
      // Neither submission receives cumulative peer acceptance. Native return
      // releases the producer while the shared owner retains those obligations.
      await peer.session.sendObjectBatch([
        (opcode: 0x60, body: Uint8List.fromList([1])),
      ], completion: ConnectionSessionObjectCompletion.submitted);
      expect(peer.nativeReturns, 2);
      expect(peer.objects.map((packet) => packet.sequence), [1, 2]);

      final bulk = peer.session.sendObjectBatch([
        (opcode: 0x70, body: Uint8List.fromList([7])),
      ], priority: ConnectionSessionObjectPriority.bulk);
      bulk.ignore();
      // Bulk is queued at a genuine closed admission boundary. It has not
      // issued any Object and cannot reserve the next burst ahead of user work.
      await Future<void>.delayed(Duration.zero);
      final interactive = peer.session.sendObject(
        0x71,
        Uint8List.fromList([9]),
      );
      interactive.ignore();
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(2));
      expect(
        peer.nativeReturns,
        2,
        reason: 'Submitted completion cannot create credit beyond grant 2',
      );
      // Global codec bounds must not widen this stream's negotiated credit2.
      peer.objectProgress(0, grant: 3);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(2));
      peer.objectProgress(2, grant: 3);
      await Future<void>.delayed(Duration.zero);
      expect(
        peer.objects.last.code,
        0x71,
        reason: 'Priority is selected when credit permits an actual burst',
      );
      expect(peer.objects.last.sequence, 3);
      peer.objectProgress(3, grant: 3);
      await interactive;
      peer.objectProgress(3, grant: 4);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects.last.code, 0x70);
      expect(peer.objects.last.sequence, 4);
      peer.objectProgress(4, grant: 4);
      await bulk;
      expect(peer.retirement, isEmpty);
    },
  );

  test(
    'scoped Object cancellation joins issued work and preserves other producers',
    () async {
      final nativeReturn = Completer<void>();
      final peer = _PendingPeer(
        objectCapacity: 2,
        batchObjects: true,
        beforeNativeBatch: (index) =>
            index == 1 ? nativeReturn.future : Future<void>.value(),
      );
      addTearDown(peer.close);
      await peer.session.activate();
      final retirement = Completer<void>();
      var cancelled = false;
      final old = peer.session.sendObjectBatch(
        [
          for (var i = 0; i < 3; ++i)
            (opcode: 0x70, body: Uint8List.fromList([i])),
        ],
        atomicMessage: true,
        messageEnds: [1, 2, 3],
        cancellationFuture: retirement.future,
      );
      final oldResult = expectLater(
        old,
        throwsA(isA<ConnectionSessionObjectCancelled>()),
      ).then((_) => cancelled = true);
      await Future<void>.delayed(Duration.zero);
      expect(peer.nativeBatchSizes, [
        2,
      ], reason: 'The issued native batch is held');
      final other = peer.session.sendObject(0x71, Uint8List.fromList([9]));
      other.ignore();
      retirement.complete();
      await Future<void>.delayed(Duration.zero);
      expect(cancelled, isFalse, reason: 'Issued native write is still owned');
      nativeReturn.complete();
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects.map((p) => p.sequence), [1, 2]);
      expect(cancelled, isFalse, reason: 'Native return is not admission');
      peer.objectProgress(2, grant: 2);
      await oldResult;
      expect(peer.objects.map((p) => p.code), [0x70, 0x70]);
      expect(peer.session.isActivated, isTrue);
      // An already-retired producer queued behind another capacity waiter
      // must settle without cancelling that waiter or consuming a sequence.
      await expectLater(
        peer.session.sendObjectBatch([
          (opcode: 0x72, body: Uint8List.fromList([8])),
        ], cancellationFuture: retirement.future),
        throwsA(isA<ConnectionSessionObjectCancelled>()),
      );
      expect(peer.objects, hasLength(2));
      peer.onOrdered = (packet) => peer.publish(packet.sequence);
      final cancel = await peer.session.sendOrdered(
        LitchcraftFrameStoreProtocol.storeCancel,
        _windowTransfer().cancelPacket(),
      );
      expect(cancel.disposition, ConnectionSessionDisposition.applied);
      peer.objectProgress(2, grant: 3);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects.last.code, 0x71);
      expect(peer.objects.last.sequence, 3);
      peer.objectProgress(3, grant: 3);
      await other;
      final nativeCalls = peer.nativeBatchSizes.length;
      await expectLater(
        peer.session.sendObjectBatch([
          (opcode: 0x73, body: Uint8List.fromList([7])),
        ], cancellationFuture: retirement.future),
        throwsA(isA<ConnectionSessionObjectCancelled>()),
      );
      expect(
        peer.nativeBatchSizes,
        hasLength(nativeCalls),
        reason: 'An already-retired lifetime must not enter native writing',
      );
      expect(peer.objects, hasLength(3));
      expect(peer.retirement, isEmpty);
    },
  );

  test(
    'scoped cancellation finishes an issued logical fragment before dropping its suffix',
    () async {
      final peer = _PendingPeer(objectCapacity: 2, batchObjects: true);
      addTearDown(peer.close);
      await peer.session.activate();
      final retirement = Completer<void>();
      var cancelled = false;
      final old = peer.session.sendObjectBatch(
        [
          for (var i = 0; i < 4; ++i)
            (opcode: 0x70, body: Uint8List.fromList([i])),
        ],
        atomicMessage: true,
        messageEnds: [3, 4],
        cancellationFuture: retirement.future,
      );
      final result = expectLater(
        old,
        throwsA(isA<ConnectionSessionObjectCancelled>()),
      ).then((_) => cancelled = true);
      await Future<void>.delayed(Duration.zero);
      retirement.complete();
      peer.objectProgress(2, grant: 2);
      await Future<void>.delayed(Duration.zero);
      expect(cancelled, isFalse);
      expect(peer.objects, hasLength(2));
      peer.objectProgress(2, grant: 3);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects.map((p) => p.body.single), [0, 1, 2]);
      peer.objectProgress(3, grant: 3);
      await result;
      expect(
        peer.objects,
        hasLength(3),
        reason: 'Unstarted logical message was retired',
      );
      expect(peer.retirement, isEmpty);
    },
  );

  test(
    'Store cancellation exits a fully accepted closed grant without sending stale tail',
    () async {
      final upload = _StoreWindowPeer();
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      final retirement = Completer<void>();
      final work = upload.client.send(
        upload.transfer,
        maximumPacketBytes: 230,
        cancellationFuture: retirement.future,
      );
      final result = expectLater(
        work,
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      await Future<void>.delayed(Duration.zero);
      expect(upload.bodyPackets, hasLength(54));
      upload.peer.objectProgress(55, grant: 55);
      await Future<void>.delayed(Duration.zero);
      retirement.complete();
      await result;
      expect(
        upload.peer.ordered.single.code,
        LitchcraftFrameStoreProtocol.storeCancel,
      );
      expect(upload.bodyPackets, hasLength(54));
      expect(
        upload.operations,
        isNot(contains(LitchcraftFrameStoreProtocol.storeCommit)),
      );
      expect(upload.peer.session.isActivated, isTrue);
      expect(upload.peer.retirement, isEmpty);
    },
  );

  test(
    'normal Store sender uses six remaining grants before first48 acceptance',
    () async {
      final upload = _StoreWindowPeer();
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      var progress = 0, done = false;
      final work = upload.client
          .send(
            upload.transfer,
            maximumPacketBytes: 230,
            onProgress: (_, _) => progress++,
          )
          .then((value) {
            done = true;
            return value;
          });
      final observed = work.then<void>((_) {}, onError: (Object _) {});
      await Future<void>.delayed(Duration.zero);
      // Begin is accepted. Body48 returned natively but has no admission
      // feedback; six initial grants remain. OLD stops at49 total Objects.
      expect(upload.peer.nativeBatchSizes, [1, 48, 6]);
      expect(upload.bodyPackets, hasLength(54));
      expect(upload.peer.objects.last.sequence, 55);
      expect(done, isFalse);
      expect(progress, 0);
      expect(upload.operations, isNot(contains(21)));
      upload.acceptAll();
      final result = await work;
      await observed;
      expect(result.state.generation, 10);
      expect(result.state.frameCount, 32);
      expect(result.checkpoints, 0);
      expect(upload.operations.last, LitchcraftFrameStoreProtocol.storeCommit);
      final body = BytesBuilder(copy: false);
      var cursor = 0;
      for (final packet in upload.bodyPackets) {
        expect(
          ByteData.sublistView(packet).getUint32(9, Endian.little),
          cursor,
        );
        body.add(packet.sublist(13));
        cursor += packet.length - 13;
      }
      final bytes = body.takeBytes();
      expect(cursor, upload.transfer.bodyBytes);
      expect(_crc32(bytes), upload.transfer.bodyCrc32);
      final expectedPackets = upload.transfer
          .chunkPackets(maximumPacketBytes: 230)
          .toList();
      expect(upload.bodyPackets, hasLength(expectedPackets.length));
      for (var index = 0; index < expectedPackets.length; index++) {
        expect(
          upload.bodyPackets[index],
          orderedEquals(expectedPackets[index]),
          reason: 'Store body packet $index must be byte-exact',
        );
      }
      expect(upload.peer.nativeBatchSizes, everyElement(lessThanOrEqualTo(48)));
    },
  );

  test(
    'normal Store sender drains one54 window before Cancel and replacement',
    () async {
      final upload = _StoreWindowPeer();
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      var cancelled = false, progress = 0;
      final work = upload.client.send(
        upload.transfer,
        maximumPacketBytes: 230,
        isCancelled: () => cancelled,
        onProgress: (_, _) => progress++,
      );
      final failure = expectLater(
        work,
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      await Future<void>.delayed(Duration.zero);
      expect(upload.bodyPackets, hasLength(54));
      cancelled = true;
      upload.acceptAll();
      await failure;
      expect(progress, 0);
      expect(upload.operations.where((op) => op == 21), isEmpty);
      expect(upload.cancelAtObjectCount, 55);
      expect(upload.peer.ordered, hasLength(1));
      final oldPackets = upload.storePackets.length;
      await Future<void>.delayed(Duration.zero);
      expect(upload.storePackets, hasLength(oldPackets));
      final replacement = _windowTransfer(request: 0x4322);
      final result = await upload.client.send(
        replacement,
        maximumPacketBytes: 230,
      );
      expect(result.state.generation, 10);
      expect(
        upload.storePackets.skip(oldPackets),
        everyElement(
          predicate<Uint8List>(
            (packet) =>
                ByteData.sublistView(packet).getUint16(7, Endian.little) ==
                0x4322,
            'only replacement bytes may follow priority Cancel',
          ),
        ),
      );
    },
  );

  test(
    'queued Store tail yields to interactive Object at native batch boundary',
    () async {
      final held = Completer<void>();
      final upload = _StoreWindowPeer(
        beforeNativeBatch: (index) {
          return index == 2 ? held.future : Future<void>.value();
        },
      );
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      upload.autoAccept = true;
      final work = upload.client.send(upload.transfer, maximumPacketBytes: 230);
      await Future<void>.delayed(Duration.zero);
      final interactive = upload.peer.session.sendObject(
        0x7e,
        Uint8List.fromList([1]),
      );
      held.complete();
      await Future.wait([work, interactive]);
      final codes = upload.peer.objects.map((packet) => packet.code).toList();
      expect(codes.indexOf(0x7e), 49);
      expect(codes.sublist(50, 56), List.filled(6, 0x40));
      expect(upload.peer.retirement, isEmpty);
    },
  );

  test(
    'native batch failure retires both observed Store submissions',
    () async {
      final held = Completer<void>();
      final upload = _StoreWindowPeer(
        beforeNativeBatch: (index) {
          return index == 2 ? held.future : Future<void>.value();
        },
      );
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      final work = upload.client.send(upload.transfer, maximumPacketBytes: 230);
      final failure = expectLater(work, throwsA(isA<StateError>()));
      await Future<void>.delayed(Duration.zero);
      held.completeError(StateError('controlled native body failure'));
      await failure;
      expect(upload.peer.session.isActivated, isFalse);
      expect(upload.trace.rows, hasLength(2));
      expect(upload.trace.rows.map((row) => row['outcome']), [
        'failed',
        'failed',
      ]);
      expect(upload.operations, [LitchcraftFrameStoreProtocol.storeBegin]);
      expect(upload.peer.ordered, isEmpty);
    },
  );

  test(
    'disconnect retires queued Store tail without late acceptance or Commit',
    () async {
      final upload = _StoreWindowPeer();
      addTearDown(upload.peer.close);
      await upload.peer.session.activate();
      final work = upload.client.send(upload.transfer, maximumPacketBytes: 230);
      final failure = expectLater(work, throwsA(isA<StateError>()));
      await Future<void>.delayed(Duration.zero);
      expect(upload.bodyPackets, hasLength(54));
      await upload.peer.session.dispose();
      await failure;
      upload.peer.objectProgress(55);
      await Future<void>.delayed(Duration.zero);
      expect(upload.bodyPackets, hasLength(54));
      expect(upload.trace.rows, hasLength(2));
      expect(upload.trace.rows.map((row) => row['outcome']), [
        'failed',
        'failed',
      ]);
      expect(upload.operations, isNot(contains(21)));
    },
  );

  test(
    'Store submission observes acceptance after native return with six grants left',
    () async {
      var now = 100;
      final peer = _PendingPeer(
        objectCapacity: 54,
        batchObjects: true,
        observationMicroseconds: () => now,
        onNativeReturn: () => now += 100,
      );
      addTearDown(peer.close);
      await peer.session.activate();
      final trace = _SubmissionTrace();
      final client = createConnectionLitchcraftFrameStoreClient(
        session: peer.session,
        monotonicMilliseconds: () => 0,
        appTrace: trace,
      );
      List<int> storePacket(int operation) => [
        0x4d,
        3,
        operation,
        7,
        0,
        0,
        0,
        11,
        0,
      ];
      var done = false;
      final sending = () async {
        await client.writeBulkBatch!(List.generate(48, (_) => storePacket(19)));
        await client.writeBulkBatch!(List.generate(6, (_) => storePacket(19)));
        await client.writeControl(storePacket(21));
        done = true;
      }();
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(48));
      expect(peer.nativeReturns, 1);
      expect(done, isFalse);
      // No grant exhaustion: the initial54 positions still have six unused.
      // Only real cumulative acceptance is withheld, not the SDK return.
      expect(peer.objects.last.sequence, 48);
      now = 9000200; // Observation clock only; no real deadline changed.
      peer.objectProgress(48);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(54));
      expect(
        done,
        isFalse,
        reason: 'Commit stays behind the remaining six accepts',
      );
      peer.objectProgress(54);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(55));
      expect(done, isFalse);
      peer.objectProgress(55);
      await sending;
      expect(trace.rows, hasLength(2));
      expect(trace.rows.first['first'], 1);
      expect(trace.rows.first['last'], 48);
      expect(trace.rows.first['count'], 48);
      expect(trace.rows.first['request'], 11);
      expect(trace.rows.first['life'], 7);
      expect(trace.rows.first['outcome'], 'accepted');
      expect(trace.rows.first['queued_us'], 100);
      expect(trace.rows.first['first_send_us'], 100);
      expect(trace.rows.first['last_return_us'], 200);
      expect(trace.rows.first['accepted_us'], 9000200);
      expect(trace.rows.first['done_us'], 9000200);
      expect(trace.rows.first['native_us'], 100);
      expect(trace.rows.first['grant_wait_us'], 0);
      expect(trace.rows.last['queued_us'], 9000200);
      expect(trace.rows.last['first_send_us'], 9000200);

      expect(
        trace.rows.first['accepted_us'],
        greaterThanOrEqualTo(trace.rows.first['last_return_us'] as int),
      );
      expect(trace.rows.last['first'], 49);
      expect(trace.rows.last['last'], 54);
      expect(client.objectSubmissionSummary!(), (
        request: 11,
        submissions: 2,
        objects: 54,
      ));
      final directory = Directory.systemTemp.createTempSync(
        'craft-comm-observation-',
      );
      try {
        final recorder = await AppTraceRecorder.open(directory);
        recorder.record(
          'litchcraft.object_submission',
          fields: {
            ...trace.rows.first,
            'life': 0x7fffffffffffffff,
            'first': 0xffffffc0,
            'last': 0xfffffff5,
            for (final key in [
              'queued_us',
              'first_send_us',
              'last_return_us',
              'accepted_us',
              'done_us',
              'grant_wait_us',
              'native_us',
            ])
              key: 0x7fffffffffffffff,
          },
        );
        final snapshot = await recorder.snapshot();
        final retained =
            jsonDecode(snapshot.jsonl.trim()) as Map<String, dynamic>;
        expect(retained['fields_omitted'], isNull);
        expect(retained['request'], 11);
        expect(retained['accepted_us'], 0x7fffffffffffffff);
        expect(snapshot.droppedEvents, 0);
        await recorder.close();
      } finally {
        directory.deleteSync(recursive: true);
      }
    },
  );

  test(
    'Object observation distinguishes a genuine grant wait and retires exactly once',
    () async {
      var now = 10;
      final peer = _PendingPeer(
        objectCapacity: 2,
        batchObjects: true,
        observationMicroseconds: () => now,
      );
      addTearDown(peer.close);
      await peer.session.activate();
      final rows = <Map<String, Object?>>[];
      final work = peer.session.sendObjectBatch(
        List.generate(3, (_) => (opcode: 40, body: Uint8List.fromList([1]))),
        atomicMessage: true,
        onObservation: rows.add,
      );
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(2));
      now = 1000;
      peer.objectProgress(2);
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(3));
      now = 1100;
      peer.objectProgress(3);
      await work;
      expect(rows.single['grant_wait_us'], 990);
      expect(rows.single['outcome'], 'accepted');
      rows.clear();
      final pending = peer.session.sendObjectBatch([
        (opcode: 40, body: Uint8List.fromList([1])),
      ], onObservation: rows.add);
      final failure = expectLater(pending, throwsA(isA<StateError>()));
      await Future<void>.delayed(Duration.zero);
      await peer.session.dispose();
      await failure;
      expect(rows, hasLength(1));
      expect(rows.single['outcome'], 'failed');
      expect(rows.single['accepted_us'], isNull);
      peer.objectProgress(4);
      await Future<void>.delayed(Duration.zero);
      expect(
        rows,
        hasLength(1),
        reason: 'Late old-life progress cannot report success',
      );
    },
  );

  test(
    'accepted Object window queries lost credit while control stays usable',
    () async {
      const silence = Duration(milliseconds: 250);
      final peer = _PendingPeer(
        silence: silence,
        objectCapacity: 54,
        batchObjects: true,
      )..holdQuery = true;
      addTearDown(peer.close);
      await peer.session.activate();
      final payloads = List.generate(
        55,
        (index) => Uint8List.fromList([index]),
      );
      var completions = 0;
      final work = peer.session
          .sendObjectBatch([
            for (final body in payloads) (opcode: 40, body: body),
          ], atomicMessage: true)
          .then((_) => completions++);
      unawaited(work.then<void>((_) {}, onError: (Object _) {}));
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(54));

      // Truthful pressure accepts every issued Object but grants no new slot.
      // This peer checks app policy, not the firmware's credit calculation.
      peer.objectProgress(54, grant: 54);
      await peer.queryEntered.future.timeout(silence);
      expect(peer.session.isActivated, isTrue);
      expect(peer.retirement, isEmpty);
      expect(peer.queries, 1);
      expect(peer.objects, hasLength(54));
      expect(completions, 0);

      await peer.session.heartbeat();
      expect(peer.heartbeats, 1);
      final cancelPacket = _windowTransfer().cancelPacket();
      peer.onOrdered = (packet) {
        expect(packet.code, LitchcraftFrameStoreProtocol.storeCancel);
        expect(packet.body, orderedEquals(cancelPacket));
        peer.publish(packet.sequence);
      };
      final cancel = await peer.session.sendOrdered(
        LitchcraftFrameStoreProtocol.storeCancel,
        cancelPacket,
      );
      expect(cancel.disposition, ConnectionSessionDisposition.applied);
      expect(peer.ordered, hasLength(1));
      expect(peer.objects, hasLength(54));
      expect(completions, 0);

      // Query recovers a lost updated grant and resumes the same producer.
      // Native return alone cannot complete its final Object before acceptance.
      peer.holdQuery = false;
      peer.objectProgress(54, grant: 55);
      peer.heldQuery.complete();
      await Future<void>.delayed(Duration.zero);
      expect(peer.objects, hasLength(55));
      expect(completions, 0);
      peer.objectProgress(55, grant: 55);
      await work;
      expect(completions, 1);
      expect(
        peer.objects.map((packet) => packet.sequence),
        orderedEquals(List.generate(55, (index) => index + 1)),
      );
      for (var index = 0; index < payloads.length; index++) {
        expect(peer.objects[index].body, orderedEquals(payloads[index]));
      }
      expect(peer.session.isActivated, isTrue);
      expect(peer.retirement, isEmpty);
    },
  );

  test(
    'passive State read sends no Query after event-loop stall exhausts deadline',
    () async {
      final peer = _PendingPeer(silence: const Duration(milliseconds: 60));
      addTearDown(peer.close);
      await peer.session.activate();
      final reading = peer.session.readStateSnapshot<bool>(
        mode: ConnectionSessionStateReadMode.observeFirst,
        accept: (_) => null,
      );
      final failure = expectLater(reading, throwsA(isA<TimeoutException>()));
      // Block delivery past both the observation timer and total deadline.
      sleep(const Duration(milliseconds: 90));
      await failure;
      expect(peer.stateQueries, 0);
    },
  );

  test('passive State read retires its lifetime on cancellation', () async {
    final peer = _PendingPeer();
    addTearDown(peer.close);
    await peer.session.activate();
    final reading = peer.session.readStateSnapshot<bool>(
      mode: ConnectionSessionStateReadMode.observeFirst,
      accept: (_) => null,
    );
    final failure = expectLater(reading, throwsA(isA<StateError>()));
    await peer.session.dispose();
    await failure;
    expect(peer.stateQueries, 0);
    await expectLater(
      peer.session.readStateSnapshot<bool>(accept: (_) => true),
      throwsA(isA<StateError>()),
    );
  });

  test(
    'retained Ordered head has a fixed limit despite replies and tail traffic',
    () async {
      final peer = _PendingPeer(completion: const Duration(milliseconds: 800));
      addTearDown(peer.close);
      await peer.session.activate();
      final clock = Stopwatch()..start();
      final work = <Future<ConnectionSessionOutcome>>[peer.action()];
      final noise = Timer.periodic(const Duration(milliseconds: 80), (_) {
        if (work.length < 8) work.add(peer.action());
        peer.state();
      });
      addTearDown(noise.cancel);
      final failure = expectLater(work.first, throwsA(isA<TimeoutException>()));
      await failure;
      noise.cancel();
      for (final tail in work.skip(1)) {
        await expectLater(tail, throwsA(isA<TimeoutException>()));
      }
      expect(clock.elapsedMilliseconds, inInclusiveRange(700, 1600));
      expect(peer.queries, greaterThanOrEqualTo(3));
      expect(peer.retirement.single['cause'], 'ordered_completion_deadline');
      expect(peer.retirement.single['ordered_pending'], work.length);
      expect(peer.session.isActivated, isFalse);
    },
  );

  test(
    'retained Ordered head still detects two-second reply silence amid State',
    () async {
      final peer = _PendingPeer(silence: const Duration(seconds: 2));
      addTearDown(peer.close);
      await peer.session.activate();
      peer.answerQueries = false;
      final clock = Stopwatch()..start();
      final work = peer.action();
      final noise = Timer.periodic(
        const Duration(milliseconds: 60),
        (_) => peer.state(),
      );
      addTearDown(noise.cancel);
      await expectLater(work, throwsA(isA<TimeoutException>()));
      expect(clock.elapsedMilliseconds, inInclusiveRange(1900, 3200));
      expect(peer.queries, greaterThan(0));
      expect(peer.retirement.single['cause'], 'ordered_deadline');
    },
  );

  test(
    'unreceived Ordered head keeps ordinary recovery despite Query replies',
    () async {
      final peer = _PendingPeer()..retainActions = false;
      addTearDown(peer.close);
      await peer.session.activate();
      final clock = Stopwatch()..start();
      await expectLater(peer.action(), throwsA(isA<TimeoutException>()));
      expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
      expect(
        peer.ordered.where((p) => p.sequence == 1).length,
        greaterThanOrEqualTo(2),
      );
      expect(peer.retirement.single['cause'], 'ordered_deadline');
    },
  );

  test('two lost Ordered copies repair without a new session', () async {
    final peer = _PendingPeer()..retainActions = false;
    addTearDown(peer.close);
    await peer.session.activate();
    final action = peer.action();
    final observed = action.then<Object>(
      (value) => value,
      onError: (Object error) => error,
    );
    await Future<void>.delayed(const Duration(milliseconds: 100));
    peer.retainActions = true;
    await Future<void>.delayed(const Duration(milliseconds: 180));
    if (peer.retained.contains(1)) peer.publish(1);
    expect(await observed, isA<ConnectionSessionOutcome>());
    expect(
      peer.ordered.where((p) => p.sequence == 1).length,
      greaterThanOrEqualTo(3),
    );
    expect(peer.session.isActivated, isTrue);
  });

  test(
    'retirement settles caller before held Ordered native write returns',
    () async {
      final peer = _PendingPeer()..holdAction = true;
      addTearDown(peer.close);
      await peer.session.activate();
      var settled = false;
      final action = peer.action().then<void>(
        (_) {},
        onError: (Object _) => settled = true,
      );
      await peer.actionEntered.future;
      await peer.session.dispose();
      await Future<void>.delayed(Duration.zero);
      expect(settled, isTrue);
      peer.heldAction.complete();
      await action;
    },
  );

  test('lost terminal Checkpoint returns to ordinary recovery', () async {
    final peer = _PendingPeer();
    addTearDown(peer.close);
    await peer.session.activate();
    final work = peer.action();
    final failure = expectLater(work, throwsA(isA<TimeoutException>()));
    await Future<void>.delayed(const Duration(milliseconds: 80));
    final clock = Stopwatch()..start();
    peer.publish(1, checkpoint: false);
    await failure;
    expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
    expect(peer.retirement.single['cause'], 'ordered_deadline');
  });

  test(
    'lost Confirm feedback remains bounded after actual terminal success',
    () async {
      final peer = _PendingPeer()..acknowledgeConfirm = false;
      addTearDown(peer.close);
      await peer.session.activate();
      final work = peer.action();
      await Future<void>.delayed(Duration.zero);
      final clock = Stopwatch()..start();
      peer.publish(1);
      final result = await work;
      expect(result.disposition, ConnectionSessionDisposition.applied);
      await peer.retired.future.timeout(const Duration(seconds: 2));
      expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
      expect(peer.confirmations, greaterThanOrEqualTo(2));
      expect(peer.retirement.single['cause'], 'ordered_deadline');
      expect(peer.retirement.single['ordered_pending'], 0);
    },
  );

  test(
    'a successor earns its own allowance only after it becomes head',
    () async {
      final peer = _PendingPeer(completion: const Duration(milliseconds: 800));
      addTearDown(peer.close);
      await peer.session.activate();
      final first = peer.action();
      final second = peer.action();
      await Future<void>.delayed(const Duration(milliseconds: 550));
      peer.publish(1);
      expect((await first).sequence, 1);
      await Future<void>.delayed(const Duration(milliseconds: 350));
      expect(peer.session.isActivated, isTrue);
      expect(peer.retirement, isEmpty);
      peer.publish(2);
      expect((await second).sequence, 2);
      expect(peer.session.isActivated, isTrue);
    },
  );

  test(
    'Object silence keeps its short bound while Ordered head is retained',
    () async {
      final peer = _PendingPeer();
      addTearDown(peer.close);
      await peer.session.activate();
      final ordered = peer.action();
      final orderedFailure = expectLater(
        ordered,
        throwsA(isA<TimeoutException>()),
      );
      final clock = Stopwatch()..start();
      await expectLater(
        peer.session.sendObject(18, Uint8List.fromList([1])),
        throwsA(isA<TimeoutException>()),
      );
      await orderedFailure;
      expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
      expect(peer.retirement.single['cause'], 'object_deadline');
    },
  );

  test(
    'disconnect cancels retained Ordered work without completing it',
    () async {
      final peer = _PendingPeer();
      addTearDown(peer.close);
      await peer.session.activate();
      final failure = expectLater(peer.action(), throwsA(isA<StateError>()));
      await Future<void>.delayed(Duration.zero);
      await peer.session.dispose();
      await failure;
      expect(peer.session.isActivated, isFalse);
      expect(peer.retirement, isEmpty);
    },
  );

  test(
    'unanswered Query cannot borrow liveness from unsolicited tail receipts',
    () async {
      final peer = _PendingPeer()..answerQueries = false;
      addTearDown(peer.close);
      await peer.session.activate();
      final tails = <Timer>[];
      addTearDown(() {
        for (final timer in tails) {
          timer.cancel();
        }
      });
      final work = <Future<ConnectionSessionOutcome>>[];
      peer.onQuery = () {
        if (work.length < 8) {
          tails.add(
            Timer(const Duration(milliseconds: 5), () {
              work.add(peer.action());
            }),
          );
        }
      };
      final clock = Stopwatch()..start();
      work.add(peer.action());
      await expectLater(work.first, throwsA(isA<TimeoutException>()));
      for (final tail in work.skip(1)) {
        await expectLater(tail, throwsA(isA<TimeoutException>()));
      }
      expect(clock.elapsedMilliseconds, inInclusiveRange(220, 450));
      expect(work.length, greaterThan(1));
      expect(peer.retirement.single['cause'], 'ordered_deadline');
    },
  );

  for (final confirm in ['first', 'replay']) {
    test(
      'held SDK $confirm Confirm remains ordinary finite recovery',
      () async {
        final peer = _PendingPeer()
          ..acknowledgeConfirm = false
          ..holdConfirm = confirm == 'first'
          ..holdConfirmAfterFirst = confirm == 'replay';
        addTearDown(peer.close);
        await peer.session.activate();
        final work = peer.action();
        await Future<void>.delayed(Duration.zero);
        peer.publish(1);
        expect((await work).disposition, ConnectionSessionDisposition.applied);
        await peer.confirmEntered.future;
        // A real peer confirmation can precede the replay SDK Future's return.
        // The remaining held write still belongs to this connection lifetime.
        if (confirm == 'replay') {
          peer.confirmed = 1;
          peer.progress();
        }
        final clock = Stopwatch()..start();
        await peer.retired.future.timeout(const Duration(seconds: 2));
        expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
        expect(peer.retirement.single['cause'], 'ordered_deadline');
        expect(peer.retirement.single['ordered_pending'], 0);
        expect(peer.session.isActivated, isFalse);
        peer.heldConfirm.complete();
        await Future<void>.delayed(Duration.zero);
        expect(peer.retirement, hasLength(1));
        expect(peer.session.isActivated, isFalse);
      },
    );
  }

  test(
    'late held Confirm cannot complete successors or write after retirement',
    () async {
      final peer = _PendingPeer()
        ..holdConfirm = true
        ..answerQueries = false;
      addTearDown(peer.close);
      await peer.session.activate();
      final first = peer.action();
      final second = peer.action();
      final successor = second.then((_) => 'applied', onError: (_) => 'failed');
      await Future<void>.delayed(Duration.zero);
      peer.publish(1);
      expect((await first).sequence, 1);
      await peer.confirmEntered.future;
      peer.publish(2);
      await peer.retired.future.timeout(const Duration(seconds: 2));
      expect(await successor, 'failed');
      expect(peer.confirmations, 1);
      peer.heldConfirm.complete();
      await Future<void>.delayed(Duration.zero);
      expect(
        peer.confirmations,
        1,
        reason: 'No stale Confirm(2) after retirement',
      );
      expect(peer.retirement, hasLength(1));
      expect(peer.session.isActivated, isFalse);
    },
  );

  for (final lane in ['Ordered', 'Object']) {
    test(
      'a held SDK $lane recovery Query cannot suspend reply-silence retirement',
      () async {
        final peer = _PendingPeer()..holdQuery = true;
        addTearDown(peer.close);
        await peer.session.activate();
        final clock = Stopwatch()..start();
        final work = lane == 'Ordered'
            ? peer.action()
            : peer.session.sendObject(18, Uint8List.fromList([1]));
        final failure = expectLater(work, throwsA(isA<TimeoutException>()));
        await peer.queryEntered.future;
        await peer.retired.future.timeout(const Duration(seconds: 2));
        await failure;
        expect(clock.elapsedMilliseconds, inInclusiveRange(220, 650));
        expect(
          peer.queries,
          1,
          reason: 'Do not queue more SDK writes behind a held Query',
        );
        expect(
          peer.retirement.single['cause'],
          lane == 'Ordered' ? 'ordered_deadline' : 'object_deadline',
        );
        peer.heldQuery.complete();
        await Future<void>.delayed(Duration.zero);
        expect(peer.retirement, hasLength(1));
      },
    );
  }
}

// A controlled typed peer exercises shared recovery policy only. The separate
// mounted VirtualLitch Delete journey establishes the real app/storage seam.
class _PendingPeer {
  _PendingPeer({
    Duration silence = const Duration(milliseconds: 250),
    Duration completion = const Duration(seconds: 20),
    this.objectCapacity = 8,
    bool batchObjects = false,
    int Function()? observationMicroseconds,
    void Function()? onNativeReturn,
    Future<void> Function(int index)? beforeNativeBatch,
  }) {
    session = ConnectionSession(
      packets: packets.stream,
      send: send,
      sendBatch: batchObjects
          ? (values) async {
              nativeBatchSizes.add(values.length);
              if (beforeNativeBatch != null) {
                await beforeNativeBatch(nativeBatchSizes.length);
              }
              for (final value in values) {
                await send(value);
              }
              nativeReturns++;
              onNativeReturn?.call();
            }
          : null,
      clientNonce: 7,
      observationMicroseconds: observationMicroseconds,
      packetMaximum: 244,
      recoveryProbeDelay: const Duration(milliseconds: 20),
      recoveryFailureTimeout: silence,
      receivedHeadCompletionTimeout: completion,
      onRetired: (fields) {
        retirement.add(fields);
        if (!retired.isCompleted) retired.complete();
      },
    );
  }

  final packets = StreamController<List<int>>(sync: true);
  late final ConnectionSession session;
  final int objectCapacity;
  final objects = <ConnectionSessionPacket>[];
  int nativeReturns = 0;
  final nativeBatchSizes = <int>[];
  void Function(ConnectionSessionPacket)? onObject;
  void Function(ConnectionSessionPacket)? onOrdered;
  final ordered = <ConnectionSessionPacket>[];
  final retirement = <Map<String, Object?>>[];
  final retired = Completer<void>();
  final queryEntered = Completer<void>();
  final heldQuery = Completer<void>();
  final confirmEntered = Completer<void>();
  final heldConfirm = Completer<void>();
  final retained = <int>{};
  Uint8List? hello;
  Uint8List? welcome;
  Uint8List? identity;
  int terminal = 0;
  int confirmed = 0;
  int queries = 0;
  int heartbeats = 0;
  int stateQueries = 0;
  int confirmations = 0;
  int stateSequence = 0;
  bool answerQueries = true;
  bool retainActions = true;
  bool acknowledgeConfirm = true;
  bool holdAction = false;
  final actionEntered = Completer<void>();
  final heldAction = Completer<void>();
  bool holdQuery = false;
  bool holdConfirm = false;
  bool holdConfirmAfterFirst = false;
  void Function()? onQuery;

  Uint8List control(
    int code, {
    int token = 1,
    int sequence = 0,
    required Uint8List body,
  }) => ConnectionSessionCodec.encode(
    ConnectionSessionPacket(
      kind: ConnectionSessionKind.control,
      code: code,
      token: token,
      sequence: sequence,
      body: body,
    ),
  );

  Future<void> send(Uint8List bytes) async {
    final packet = ConnectionSessionCodec.decode(bytes)!;
    if (packet.kind == ConnectionSessionKind.object) {
      objects.add(packet);
      onObject?.call(packet);
      return;
    }
    if (packet.kind == ConnectionSessionKind.ordered) {
      ordered.add(packet);
      if (onOrdered != null) {
        onOrdered!(packet);
        return;
      }
      if (holdAction) {
        actionEntered.complete();
        await heldAction.future;
        return;
      }
      if (retainActions) retained.add(packet.sequence);
      progress();
      return;
    }
    if (packet.kind != ConnectionSessionKind.control) return;
    switch (packet.code) {
      case ConnectionSessionControl.hello:
        hello = bytes;
        final welcomeBody = Uint8List(10);
        ByteData.sublistView(welcomeBody)
          ..setUint64(0, 7, Endian.little)
          ..setUint16(8, 244, Endian.little);
        welcome = control(ConnectionSessionControl.welcome, body: welcomeBody);
        final identityBody = Uint8List(10);
        ByteData.sublistView(identityBody)
          ..setUint64(0, 9, Endian.little)
          ..setUint16(8, 1 | 2 | 8 | 16, Endian.little);
        identity = control(
          ConnectionSessionControl.identity,
          body: identityBody,
        );
        packets
          ..add(welcome!)
          ..add(identity!);
      case ConnectionSessionControl.ready:
        final body = Uint8List(4);
        ByteData.sublistView(body).setUint32(
          0,
          _crc32(Uint8List.fromList([...hello!, ...welcome!, ...identity!])),
          Endian.little,
        );
        packets.add(control(ConnectionSessionControl.activated, body: body));
        for (final lane in [0, 1]) {
          final open = Uint8List.fromList([
            lane == 0 ? 1 : 2,
            0,
            1,
            lane == 0 ? 36 : 8,
            lane == 0 ? 10 : 234,
            0,
            40,
            0,
          ]);
          packets.add(
            control(
              ConnectionSessionControl.open,
              sequence: lane + 1,
              body: open,
            ),
          );
        }
      case ConnectionSessionControl.open:
        final lane = packet.body[2];
        final body = Uint8List(10);
        ByteData.sublistView(body)
          ..setUint32(0, lane == 0 ? 2 : 3, Endian.little)
          ..setUint16(4, lane == 0 ? 10 : 234, Endian.little);
        body[6] = lane == 0 ? 8 : objectCapacity;
        body[7] = lane;
        packets.add(
          control(
            ConnectionSessionControl.opened,
            sequence: packet.sequence,
            body: body,
          ),
        );
        if (lane == 0) progress();
      case ConnectionSessionControl.query:
        if (packet.token == 0x80000001) stateQueries++;
        if (packet.token != 2 && packet.token != 3) return;
        queries++;
        if (holdQuery) {
          if (!queryEntered.isCompleted) queryEntered.complete();
          await heldQuery.future;
          return;
        }
        if (packet.token == 2 && !answerQueries) onQuery?.call();
        if (packet.token == 2 && answerQueries) progress();
      case ConnectionSessionControl.confirm:
        if (packet.token != 2) return;
        confirmations++;
        if (holdConfirm || (holdConfirmAfterFirst && confirmations > 1)) {
          if (!confirmEntered.isCompleted) confirmEntered.complete();
          await heldConfirm.future;
          return;
        }
        if (acknowledgeConfirm) {
          confirmed = packet.sequence;
          progress();
        }
      case ConnectionSessionControl.heartbeat:
        heartbeats++;
      default:
        break;
    }
  }

  void objectProgress(int through, {int? grant}) {
    final body = Uint8List(10);
    ByteData.sublistView(body)
      ..setUint32(0, through, Endian.little)
      ..setUint32(4, grant ?? through + objectCapacity, Endian.little);
    packets.add(
      control(
        ConnectionSessionControl.progress,
        token: 3,
        sequence: through,
        body: body,
      ),
    );
  }

  void progress() {
    if (packets.isClosed) return;
    var bitmap = 0;
    for (final sequence in retained) {
      final bit = sequence - terminal - 1;
      if (bit >= 0 && bit < 16) bitmap |= 1 << bit;
    }
    final body = Uint8List(10);
    ByteData.sublistView(body)
      ..setUint32(0, confirmed, Endian.little)
      ..setUint32(4, confirmed + 8, Endian.little)
      ..setUint16(8, bitmap, Endian.little);
    packets.add(
      control(
        ConnectionSessionControl.progress,
        token: 2,
        sequence: terminal,
        body: body,
      ),
    );
  }

  void publish(int through, {bool checkpoint = true}) {
    terminal = through;
    if (checkpoint) {
      final body = Uint8List(10);
      ByteData.sublistView(body)
        ..setUint32(0, 777, Endian.little)
        ..setUint32(4, 888, Endian.little);
      packets.add(
        control(
          ConnectionSessionControl.checkpoint,
          token: 2,
          sequence: through,
          body: body,
        ),
      );
    }
    progress();
  }

  void state() {
    if (packets.isClosed) return;
    packets.add(
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: 1,
          token: 0x80000001,
          sequence: ++stateSequence,
          body: Uint8List(10),
        ),
      ),
    );
  }

  Future<ConnectionSessionOutcome> action() =>
      session.sendOrdered(10, Uint8List.fromList([1, 108]));
  Future<void> close() async {
    if (!heldAction.isCompleted) heldAction.complete();
    if (!heldQuery.isCompleted) heldQuery.complete();
    if (!heldConfirm.isCompleted) heldConfirm.complete();
    await session.dispose();
    await packets.close();
  }
}

int _crc32(Uint8List bytes) {
  var crc = 0xffffffff;
  for (final byte in bytes) {
    crc ^= byte;
    for (var bit = 0; bit < 8; bit++) {
      crc = (crc >>> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
    }
  }
  return (~crc) & 0xffffffff;
}

class _SubmissionTrace implements AppTraceSink {
  final rows = <Map<String, Object?>>[];
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'litchcraft.object_submission') rows.add(Map.of(fields));
  }
}

LitchcraftFrameStoreTransfer _windowTransfer({int request = 0x4321}) =>
    LitchcraftFrameStoreProtocol.prepare(
      session: 0x12345678,
      transfer: request,
      expectedGeneration: 9,
      topology: 3,
      active: true,
      playbackOnEntry: true,
      sparkleSeed: 7,
      favoriteColors: List.filled(8, 0),
      frames: List.generate(
        32,
        (frame) => LitchcraftAnimationFrame(
          durationMs: 50,
          emitters: List.generate(
            256,
            (index) => LitchcraftAnimatedEmitter(
              color:
                  ((frame + index * 11) & 255) << 16 |
                  ((frame * 7 + index * 17) & 255) << 8 |
                  ((frame * 13 + index * 23) & 255),
            ),
          ),
        ),
      ),
    );

// Production Store sender, adapter, codecs and Session; only SDK completion,
// admission feedback and the final Store authority reply are controlled here.
// Existing mounted VirtualGatt journeys retain real firmware adoption coverage.
class _StoreWindowPeer {
  _StoreWindowPeer({Future<void> Function(int index)? beforeNativeBatch}) {
    peer = _PendingPeer(
      objectCapacity: 54,
      batchObjects: true,
      silence: const Duration(seconds: 2),
      beforeNativeBatch: beforeNativeBatch,
    );
    client = createConnectionLitchcraftFrameStoreClient(
      session: peer.session,
      monotonicMilliseconds: () => 0,
      appTrace: trace,
    );
    final assembler = ConnectionLitchcraftAssembler();
    peer.onObject = (packet) {
      if (packet.code != ConnectionLitchcraftCodec.objectOpcode) {
        peer.objectProgress(packet.sequence);
        return;
      }
      final bytes = assembler.accept(packet.body);
      if (bytes == null) return;
      storePackets.add(bytes);
      final operation = bytes[2];
      if (autoAccept || operation == LitchcraftFrameStoreProtocol.storeBegin) {
        peer.objectProgress(packet.sequence);
      }
      if (operation == LitchcraftFrameStoreProtocol.storeCommit) {
        final state = Uint8List(19);
        final data = ByteData.sublistView(state);
        state[0] = LitchcraftV2Protocol.magic;
        state[1] = LitchcraftV2Protocol.version;
        state[2] = LitchcraftFrameStoreProtocol.storeState;
        state.setRange(3, 7, bytes.sublist(3, 7));
        data.setUint32(7, 10, Endian.little);
        state.setRange(11, 13, bytes.sublist(7, 9));
        state[14] = 23;
        data.setUint16(15, 32, Endian.little);
        final body = ConnectionLitchcraftCodec.encodePacket(
          state,
          maximumBodyBytes: 234,
        ).single;
        peer.packets.add(
          ConnectionSessionCodec.encode(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.object,
              code: ConnectionLitchcraftCodec.receiptCode,
              token: 0x80000002,
              sequence: ++reverseSequence,
              body: body,
            ),
          ),
        );
      }
    };
    peer.onOrdered = (packet) {
      expect(packet.code, LitchcraftV2Protocol.frameStoreCancel);
      cancelAtObjectCount = peer.objects.length;
      peer.publish(packet.sequence);
    };
  }

  final transfer = _windowTransfer();
  final trace = _SubmissionTrace();
  late final _PendingPeer peer;
  late final LitchcraftFrameStoreClient client;
  final storePackets = <Uint8List>[];
  bool autoAccept = false;
  int? cancelAtObjectCount;
  int reverseSequence = 0;

  List<Uint8List> get bodyPackets => storePackets
      .where((packet) => packet[2] == LitchcraftFrameStoreProtocol.storeChunk)
      .toList();
  List<int> get operations => storePackets.map((packet) => packet[2]).toList();

  void acceptAll() {
    autoAccept = true;
    peer.objectProgress(peer.objects.last.sequence);
  }
}
