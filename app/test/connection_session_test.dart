import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';

void main() {
  test(
    'activates, opens one bounded stream, and pipelines within its grant',
    () async {
      expect(ConnectionSession.reverseObjectCapacity, 8);
      const clientNonce = 0x1122334455667788;
      const bootNonce = 0x8899aabbccddeeff;
      final packets = StreamController<List<int>>(sync: true);
      final sentActions = <ConnectionSessionPacket>[];
      final sentObjects = <ConnectionSessionPacket>[];
      final confirmations = <int>[];
      final queries = <ConnectionSessionPacket>[];
      final objectQueries = <ConnectionSessionPacket>[];
      final objectReceiptQueries = <ConnectionSessionPacket>[];
      final reverseProgress = <ConnectionSessionPacket>[];
      Completer<void>? heldReverseProgress;
      final stateQueries = <ConnectionSessionPacket>[];
      final heartbeats = <ConnectionSessionPacket>[];
      final retirements = <Map<String, Object?>>[];
      final openFeedbackHints = <int>[];
      final openAttempts = <int, int>{};
      var reportOrderedHole = false;
      Uint8List? helloBytes;
      Uint8List? welcomeBytes;
      Uint8List? identityBytes;
      var helloAttempts = 0;
      var readyAttempts = 0;
      var bulkPackets = 0;
      final bulkWindowIssued = Completer<void>();
      Completer<void>? heldBulkWindow;
      final atomicWindowIssued = Completer<void>();
      Completer<void>? heldAtomicWindow;
      var atomicPackets = 0;

      Uint8List encodeControl(
        int code, {
        required int token,
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
        if (packet.kind == ConnectionSessionKind.ordered) {
          sentActions.add(packet);
          return;
        }
        if (packet.kind == ConnectionSessionKind.object) {
          sentObjects.add(packet);
          if (sentObjects.length <= 2) return;
          final progressBody = Uint8List(10);
          final progressData = ByteData.sublistView(progressBody);
          progressData.setUint32(0, packet.sequence, Endian.little);
          progressData.setUint32(4, packet.sequence + 26, Endian.little);
          packets.add(
            encodeControl(
              ConnectionSessionControl.progress,
              token: 3,
              sequence: packet.sequence,
              body: progressBody,
            ),
          );
          if (packet.code == 0x70) {
            bulkPackets += 1;
            if (bulkPackets == 26) {
              bulkWindowIssued.complete();
              await heldBulkWindow!.future;
            }
          }
          if (packet.code == 0x72) {
            atomicPackets += 1;
            if (atomicPackets == 26) {
              atomicWindowIssued.complete();
              await heldAtomicWindow!.future;
            }
          }
          return;
        }
        switch (packet.code) {
          case ConnectionSessionControl.hello:
            helloAttempts += 1;
            if (helloAttempts == 1) return;
            helloBytes = bytes;
            final welcomeBody = Uint8List(10);
            final welcomeData = ByteData.sublistView(welcomeBody);
            welcomeData.setUint64(0, clientNonce, Endian.little);
            welcomeData.setUint16(8, 244, Endian.little);
            welcomeBytes = encodeControl(
              ConnectionSessionControl.welcome,
              token: 1,
              body: welcomeBody,
            );
            final identityBody = Uint8List(10);
            final identityData = ByteData.sublistView(identityBody);
            identityData.setUint64(0, bootNonce, Endian.little);
            identityData.setUint16(
              8,
              ConnectionSession.orderedFeature |
                  ConnectionSession.stateFeature |
                  ConnectionSession.objectFeature |
                  ConnectionSession.reverseObjectFeature,
              Endian.little,
            );
            identityBytes = encodeControl(
              ConnectionSessionControl.identity,
              token: 1,
              body: identityBody,
            );
            packets.add(welcomeBytes!);
            if (helloAttempts > 2) packets.add(identityBytes!);
            break;
          case ConnectionSessionControl.ready:
            readyAttempts += 1;
            final transcript = Uint8List.fromList([
              ...helloBytes!,
              ...welcomeBytes!,
              ...identityBytes!,
            ]);
            expect(_u32(packet.body), _crc32(transcript));
            packets.add(
              encodeControl(
                ConnectionSessionControl.activated,
                token: 1,
                body: Uint8List.fromList(packet.body),
              ),
            );
            if (readyAttempts > 1) {
              final stateOpenBody = Uint8List(8);
              final stateOpenData = ByteData.sublistView(stateOpenBody);
              stateOpenData.setUint16(
                0,
                ConnectionSession.litchcraftStateTopic,
                Endian.little,
              );
              stateOpenBody[2] = 1;
              stateOpenBody[3] = ConnectionSession.stateCapacity;
              stateOpenData.setUint16(4, 4, Endian.little);
              packets.add(
                encodeControl(
                  ConnectionSessionControl.open,
                  token: 1,
                  sequence: 1,
                  body: stateOpenBody,
                ),
              );
              final reverseOpenBody = Uint8List(8);
              final reverseOpenData = ByteData.sublistView(reverseOpenBody);
              reverseOpenData.setUint16(
                0,
                ConnectionSession.litchcraftReverseObjectTopic,
                Endian.little,
              );
              reverseOpenBody[2] = 1;
              reverseOpenBody[3] = ConnectionSession.reverseObjectCapacity;
              reverseOpenData.setUint16(4, 30, Endian.little);
              reverseOpenData.setUint16(
                6,
                ConnectionSession.expectedFeedbackMs,
                Endian.little,
              );
              packets.add(
                encodeControl(
                  ConnectionSessionControl.open,
                  token: 1,
                  sequence: 2,
                  body: reverseOpenBody,
                ),
              );
            }
            break;
          case ConnectionSessionControl.open:
            openFeedbackHints.add(_u16(packet.body, 6));
            final attempts = (openAttempts[packet.sequence] ?? 0) + 1;
            openAttempts[packet.sequence] = attempts;
            if (attempts == 1) break;
            final openedBody = Uint8List(10);
            final openedData = ByteData.sublistView(openedBody);
            final lane = packet.body[2];
            openedData.setUint32(0, lane == 0 ? 2 : 3, Endian.little);
            openedData.setUint16(4, lane == 0 ? 4 : 40, Endian.little);
            openedBody[6] = lane == 0 ? packet.body[3] : 26;
            openedBody[7] = lane;
            packets.add(
              encodeControl(
                ConnectionSessionControl.opened,
                token: 1,
                sequence: packet.sequence,
                body: openedBody,
              ),
            );
            if (lane == 0) {
              final progressBody = Uint8List(10);
              ByteData.sublistView(progressBody).setUint32(4, 8, Endian.little);
              packets.add(
                encodeControl(
                  ConnectionSessionControl.progress,
                  token: 2,
                  body: progressBody,
                ),
              );
            }
            break;
          case ConnectionSessionControl.opened:
            expect(packet.token, 1);
            if (packet.sequence == 1) {
              expect(_u32(packet.body), 0x80000001);
              expect(packet.body[6], ConnectionSession.stateCapacity);
            } else {
              expect(packet.sequence, 2);
              expect(_u32(packet.body), 0x80000002);
              expect(packet.body[6], ConnectionSession.reverseObjectCapacity);
            }
            break;
          case ConnectionSessionControl.progress:
            if (packet.token == 0x80000002) {
              reverseProgress.add(packet);
              if (heldReverseProgress != null) {
                await heldReverseProgress.future;
              }
              break;
            }
            fail('Unexpected client Progress token ${packet.token}');
          case ConnectionSessionControl.confirm:
            expect(
              packet.token,
              2,
              reason: 'Confirm belongs to the opened stream',
            );
            confirmations.add(packet.sequence);
            if (packet.sequence == 9) break;
            final progressBody = Uint8List(10);
            final progressData = ByteData.sublistView(progressBody);
            progressData.setUint32(0, packet.sequence, Endian.little);
            progressData.setUint32(4, packet.sequence + 8, Endian.little);
            packets.add(
              encodeControl(
                ConnectionSessionControl.progress,
                token: 2,
                sequence: packet.sequence,
                body: progressBody,
              ),
            );
            break;
          case ConnectionSessionControl.query:
            if (packet.token == 0x80000001) {
              stateQueries.add(packet);
              final stateCode = packet.sequence == 0
                  ? ConnectionSession.litchcraftStateCode
                  : packet.sequence;
              packets.add(
                ConnectionSessionCodec.encode(
                  ConnectionSessionPacket(
                    kind: ConnectionSessionKind.state,
                    code: stateCode,
                    token: 0x80000001,
                    sequence: stateCode == 2 ? 8 : 7,
                    body: Uint8List.fromList(
                      stateCode == 2 ? [9, 8, 7] : [1, 2, 3, 4],
                    ),
                  ),
                ),
              );
              break;
            }
            if (packet.token == 3) {
              if (packet.sequence != 0) {
                objectReceiptQueries.add(packet);
                break;
              }
              objectQueries.add(packet);
              final progressBody = Uint8List(10);
              ByteData.sublistView(
                progressBody,
              ).setUint32(4, 26, Endian.little);
              packets.add(
                encodeControl(
                  ConnectionSessionControl.progress,
                  token: 3,
                  body: progressBody,
                ),
              );
              break;
            }
            expect(
              packet.token,
              2,
              reason: 'Query belongs to the opened stream',
            );
            queries.add(packet);
            if (reportOrderedHole) {
              reportOrderedHole = false;
              final progressBody = Uint8List(10);
              final progressData = ByteData.sublistView(progressBody);
              progressData.setUint32(4, 8, Endian.little);
              progressData.setUint16(8, 0x001e, Endian.little);
              packets.add(
                encodeControl(
                  ConnectionSessionControl.progress,
                  token: 2,
                  sequence: 3,
                  body: progressBody,
                ),
              );
              break;
            }
            final progressBody = Uint8List(10);
            final progressData = ByteData.sublistView(progressBody);
            progressData.setUint32(0, 9, Endian.little);
            progressData.setUint32(4, 17, Endian.little);
            packets.add(
              encodeControl(
                ConnectionSessionControl.progress,
                token: 2,
                sequence: 9,
                body: progressBody,
              ),
            );
            break;
          case ConnectionSessionControl.heartbeat:
            heartbeats.add(packet);
            break;
          default:
            fail('Unexpected control ${packet.code}');
        }
      }

      final session = ConnectionSession(
        packets: packets.stream,
        send: send,
        clientNonce: clientNonce,
        packetMaximum: 512,
        recoveryProbeDelay: const Duration(milliseconds: 20),
        recoveryFailureTimeout: const Duration(milliseconds: 90),
        onRetired: retirements.add,
      );
      final states = <ConnectionSessionPacket>[];
      final stateSubscription = session.states.listen(states.add);
      await session.activate();
      expect(helloAttempts, 3);
      expect(session.connectionToken, 1);
      expect(session.streamToken, 2);
      expect(session.objectStreamToken, 3);
      expect(session.objectMaximumBody, 40);
      expect(session.objectSelectedCapacity, 26);
      expect(session.orderedMaximumBody, 4);
      expect(session.orderedSelectedCapacity, 8);
      expect(
        () => session.sendOrdered(1, Uint8List(5)),
        throwsA(isA<StateError>()),
      );
      expect(
        () => session.sendObject(18, Uint8List(41)),
        throwsA(isA<StateError>()),
      );
      expect(session.stateStreamToken, 0x80000001);
      expect(session.reverseObjectStreamToken, 0x80000002);
      expect(session.grantThrough, 8);
      expect(readyAttempts, 2);
      expect(openAttempts, <int, int>{1: 2, 2: 2});
      expect(openFeedbackHints, [40, 40, 40, 40]);
      await session.heartbeat();
      expect(heartbeats, hasLength(1));
      expect(heartbeats.single.token, session.connectionToken);
      expect(heartbeats.single.sequence, 0);
      expect(heartbeats.single.body, isEmpty);
      await session.requestObjectReceipt(77);
      expect(objectReceiptQueries, hasLength(1));
      expect(objectReceiptQueries.single.token, session.objectStreamToken);
      expect(objectReceiptQueries.single.sequence, 77);
      await session.sendObject(18, Uint8List.fromList([0x4d, 2, 18]));
      expect(sentObjects, hasLength(3));
      expect(sentObjects.every((packet) => packet.sequence == 1), isTrue);
      expect(sentObjects.every((packet) => packet.code == 18), isTrue);
      expect(
        sentObjects.map((packet) => packet.body),
        everyElement([0x4d, 2, 18]),
      );
      expect(objectQueries, hasLength(2));

      heldBulkWindow = Completer<void>();
      final bulkProgress = <(int, int)>[];
      var bulkCompleted = false;
      final bulk = session
          .sendObjectBatch(
            List.generate(
              26 + 4,
              (index) => (opcode: 0x70, body: Uint8List.fromList([index])),
            ),
            onAdmissionProgress: (admitted, total) =>
                bulkProgress.add((admitted, total)),
          )
          .then((_) => bulkCompleted = true);
      await bulkWindowIssued.future.timeout(const Duration(seconds: 1));
      await Future<void>.value();
      expect(bulkCompleted, isFalse);
      expect(bulkProgress.last, (26, 30));
      final interactive = session.sendObject(0x71, Uint8List.fromList([0xaa]));
      heldBulkWindow.complete();
      await Future.wait([bulk, interactive]);
      expect(bulkProgress, [for (var i = 1; i <= 30; i++) (i, 30)]);
      final scheduledCodes = sentObjects
          .where((packet) => packet.code == 0x70 || packet.code == 0x71)
          .map((packet) => packet.code)
          .toList(growable: false);
      expect(scheduledCodes, [
        ...List.filled(26, 0x70),
        0x71,
        ...List.filled(4, 0x70),
      ]);

      heldAtomicWindow = Completer<void>();
      final retiredProgress = Completer<void>();
      final atomicProgress = <int>[];
      final atomicMessage = session.sendObjectBatch(
        List.generate(
          26 + 4,
          (index) => (opcode: 0x72, body: Uint8List.fromList([index])),
        ),
        atomicMessage: true,
        cancellationFuture: retiredProgress.future,
        onAdmissionProgress: (admitted, _) => atomicProgress.add(admitted),
      );
      await atomicWindowIssued.future.timeout(const Duration(seconds: 1));
      await Future<void>.value();
      expect(atomicProgress.last, 26);
      retiredProgress.complete();
      await Future<void>.value();
      final progressBeforeRetirement = List<int>.of(atomicProgress);
      final nextMessage = session.sendObject(0x73, Uint8List.fromList([0xbb]));
      heldAtomicWindow.complete();
      await Future.wait([atomicMessage, nextMessage]);
      expect(atomicProgress, progressBeforeRetirement);
      final atomicCodes = sentObjects
          .where((packet) => packet.code == 0x72 || packet.code == 0x73)
          .map((packet) => packet.code)
          .toList(growable: false);
      expect(atomicCodes, [...List.filled(26 + 4, 0x72), 0x73]);

      final receipts = <ConnectionSessionPacket>[];
      final receiptSubscription = session.objectReceipts.listen(receipts.add);
      Uint8List reverseReceipt(int sequence, int request) {
        final receipt = Uint8List(19)
          ..[0] = 0x4d
          ..[1] = 2
          ..[2] = 0x8d
          ..[11] = request;
        return ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.object,
            code: 0x8d,
            token: 0x80000002,
            sequence: sequence,
            body: receipt,
          ),
        );
      }

      // Selected packet and stream bounds both constrain incoming receipts.
      for (final length in [31, 235]) {
        packets.add(
          ConnectionSessionCodec.encode(
            ConnectionSessionPacket(
              kind: ConnectionSessionKind.object,
              code: 0x8d,
              token: 0x80000002,
              sequence: 1,
              body: Uint8List(length),
            ),
          ),
        );
      }
      expect(receipts, isEmpty);
      packets.add(reverseReceipt(1, 7));
      await Future<void>.delayed(Duration.zero);
      expect(receipts, hasLength(1));
      expect(receipts.single.sequence, 1);
      expect(receipts.single.body.sublist(0, 3), [0x4d, 2, 0x8d]);
      expect(reverseProgress, hasLength(1));
      expect(reverseProgress.single.sequence, 1);
      expect(_u32(reverseProgress.single.body), 1);
      expect(_u32(reverseProgress.single.body, 4), 9);
      packets.add(reverseReceipt(3, 9));
      packets.add(reverseReceipt(4, 10));
      await Future<void>.delayed(Duration.zero);
      expect(receipts.map((packet) => packet.sequence), [1]);
      expect(_u16(reverseProgress.last.body, 8), 0x0006);
      packets.add(reverseReceipt(2, 8));
      await Future<void>.delayed(Duration.zero);
      expect(receipts.map((packet) => packet.sequence), [1, 2, 3, 4]);
      expect(reverseProgress.last.sequence, 4);
      expect(_u32(reverseProgress.last.body), 4);
      expect(_u32(reverseProgress.last.body, 4), 12);
      expect(_u16(reverseProgress.last.body, 8), 0);
      final priorReverseProgress = reverseProgress.length;
      heldReverseProgress = Completer<void>();
      packets.add(reverseReceipt(5, 11));
      for (var duplicate = 0; duplicate < 20; duplicate++) {
        packets.add(reverseReceipt(5, 11));
      }
      packets.add(reverseReceipt(6, 12));
      packets.add(reverseReceipt(7, 13));
      await Future<void>.delayed(Duration.zero);
      expect(reverseProgress.skip(priorReverseProgress), hasLength(1));
      heldReverseProgress.complete();
      heldReverseProgress = null;
      await Future<void>.delayed(const Duration(milliseconds: 5));
      expect(
        reverseProgress
            .skip(priorReverseProgress)
            .map((packet) => packet.sequence)
            .toList(),
        [5, 7],
        reason:
            'Duplicate reverse packets coalesce behind one send and publish the newest cursor',
      );
      for (var sequence = 9; sequence <= 15; sequence++) {
        packets.add(reverseReceipt(sequence, sequence + 6));
      }
      await Future<void>.delayed(Duration.zero);
      expect(receipts.last.sequence, 7);
      expect(reverseProgress.last.sequence, 7);
      expect(_u16(reverseProgress.last.body, 8), 0x00fe);
      packets.add(reverseReceipt(8, 14));
      await Future<void>.delayed(Duration.zero);
      expect(
        receipts.map((packet) => packet.sequence),
        List.generate(15, (i) => i + 1),
      );
      expect(reverseProgress.last.sequence, 15);
      expect(_u32(reverseProgress.last.body, 4), 23);
      expect(_u16(reverseProgress.last.body, 8), 0);
      packets.add(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.state,
            code: 1,
            token: session.stateStreamToken,
            sequence: 6,
            body: Uint8List(5),
          ),
        ),
      );
      expect(states, isEmpty);
      await session.requestState(code: ConnectionSession.litchcraftStateCode);
      expect(stateQueries, hasLength(1));
      expect(stateQueries.single.sequence, 1);
      expect(states, hasLength(1));
      expect(states.single.sequence, 7);
      expect(states.single.body, [1, 2, 3, 4]);
      await session.requestState(code: 2);
      expect(stateQueries, hasLength(2));
      expect(states, hasLength(2));
      expect(states.last.code, 2);
      expect(states.last.sequence, 8);
      expect(states.last.body, [9, 8, 7]);

      final invalidGrant = Uint8List(10);
      ByteData.sublistView(invalidGrant).setUint32(4, 9, Endian.little);
      packets.add(
        encodeControl(
          ConnectionSessionControl.progress,
          token: session.streamToken,
          body: invalidGrant,
        ),
      );
      expect(session.grantThrough, 8);
      final actions = List.generate(
        9,
        (index) =>
            session.sendOrdered(9 + index, Uint8List.fromList([index + 1])),
      );
      await Future<void>.delayed(Duration.zero);
      expect(sentActions.map((packet) => packet.sequence), [
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
      ]);
      expect(sentActions.map((packet) => packet.code), [
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
      ]);
      expect(sentActions.first.body, [1]);
      final missingProgressBody = Uint8List(10);
      final missingProgressData = ByteData.sublistView(missingProgressBody);
      missingProgressData.setUint32(4, 8, Endian.little);
      missingProgressData.setUint16(8, 0x001e, Endian.little);
      packets.add(
        encodeControl(
          ConnectionSessionControl.progress,
          token: 2,
          sequence: 3,
          body: missingProgressBody,
        ),
      );
      reportOrderedHole = true;
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(
        sentActions.where((packet) => packet.sequence == 4),
        hasLength(2),
        reason: 'the exact missing packet is resent under its existing grant',
      );
      expect(
        sentActions.where((packet) => packet.sequence != 4),
        hasLength(7),
        reason: 'already received positions are not retransmitted',
      );
      final checkpointBody = Uint8List(10);
      final checkpointData = ByteData.sublistView(checkpointBody);
      checkpointData.setUint32(0, 42, Endian.little);
      checkpointData.setUint32(4, 0x12345678, Endian.little);
      packets.add(
        encodeControl(
          ConnectionSessionControl.checkpoint,
          token: 2,
          sequence: 8,
          body: checkpointBody,
        ),
      );
      await Future<void>.delayed(Duration.zero);
      expect(confirmations, [8]);
      await Future<void>.delayed(Duration.zero);
      expect(sentActions.map((packet) => packet.sequence), [
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        4,
        9,
      ]);
      checkpointData.setUint32(0, 43, Endian.little);
      packets.add(
        encodeControl(
          ConnectionSessionControl.checkpoint,
          token: 2,
          sequence: 9,
          body: checkpointBody,
        ),
      );
      final results = await Future.wait(actions);
      expect(results.map((result) => result.sequence), [
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
      ]);
      expect(
        results.map((result) => result.disposition),
        everyElement(ConnectionSessionDisposition.applied),
      );
      expect(results.last.domainRevision, 43);
      await Future<void>.delayed(Duration.zero);
      expect(confirmations, [8, 9]);
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(queries, hasLength(2));
      expect(queries.every((query) => query.sequence == 0), isTrue);
      expect(session.grantThrough, 17);

      // The earlier fixture intentionally withholds Confirm(9) feedback.
      // Settle that real advancement before measuring probes for a separate
      // unchanged position 10; otherwise its first Query legitimately resets
      // the deadline and makes the wall-clock probe count load-dependent.
      final settledProgress = Uint8List(10);
      ByteData.sublistView(settledProgress)
        ..setUint32(0, 9, Endian.little)
        ..setUint32(4, 17, Endian.little);
      packets.add(
        encodeControl(
          ConnectionSessionControl.progress,
          token: 2,
          sequence: 9,
          body: settledProgress,
        ),
      );
      await Future<void>.delayed(Duration.zero);

      final queriesBeforeStall = queries.length;
      final stalled = session.sendOrdered(18, Uint8List.fromList([10]));
      await expectLater(stalled, throwsA(isA<TimeoutException>()));
      expect(retirements, hasLength(1));
      expect(retirements.single['cause'], 'ordered_deadline');
      expect(retirements.single['error_kind'], 'TimeoutException');
      expect(retirements.single['ordered_pending'], 1);
      expect(
        sentActions.where((packet) => packet.sequence == 10),
        hasLength(greaterThanOrEqualTo(2)),
        reason: 'exact retransmissions remain bounded by the recovery deadline',
      );
      expect(
        queries.length - queriesBeforeStall,
        inInclusiveRange(2, 3),
        reason: 'unchanged progress backs probes off until the fixed deadline',
      );

      await session.dispose();
      expect(
        session.isActivated,
        isFalse,
        reason:
            'a disposed shared session cannot remain selectable by keepalive',
      );
      expect(session.heartbeat, throwsA(isA<StateError>()));
      await receiptSubscription.cancel();
      await stateSubscription.cancel();
      await packets.close();
    },
  );

  for (final holdHelloWrite in [false, true]) {
    test('disconnect during handshake observes unused replies '
        '(Hello write held: $holdHelloWrite)', () async {
      final packets = StreamController<List<int>>(sync: true);
      final writeEntered = Completer<void>();
      final heldWrite = Completer<void>();
      final retirements = <Map<String, Object?>>[];
      final session = ConnectionSession(
        packets: packets.stream,
        send: (_) async {
          if (!writeEntered.isCompleted) writeEntered.complete();
          if (holdHelloWrite) await heldWrite.future;
        },
        clientNonce: 1,
        onRetired: retirements.add,
      );
      final failure = expectLater(
        session.activate(),
        throwsA(
          isA<StateError>().having(
            (error) => error.message,
            'message',
            'Connection session ended',
          ),
        ),
      );
      await writeEntered.future;
      await Future<void>.delayed(Duration.zero);
      await packets.close();
      // The stream can end before later handshake phases are ever awaited,
      // or before the native write returns. Neither may emit an uncaught
      // internal failure, while activation must still fail for its caller.
      await Future<void>.delayed(Duration.zero);
      heldWrite.complete();
      await failure;
      expect(session.isActivated, isFalse);
      expect(retirements, hasLength(1));
      expect(retirements.single['cause'], 'notification_done');
      await session.dispose();
    });
  }

  for (final (failureKind, disconnectDuringWrite) in [
    ('Object', false),
    ('Ordered', false),
    ('Ordered replay', false),
    ('Ordered', true),
    ('Query', true),
  ]) {
    final orderedFailure = failureKind.startsWith('Ordered');
    final replayFailure = failureKind == 'Ordered replay';
    test(
      disconnectDuringWrite
          ? 'disconnect during pending SDK $failureKind write reports one failure'
          : 'native $failureKind failure retires the shared session',
      () async {
        const clientNonce = 0x1122334455667788;
        const bootNonce = 0x8899aabbccddeeff;
        const sessionStateToken = 0x80000001;
        final packets = StreamController<List<int>>(sync: true);
        var failNextObjectBatch = false;
        final writeEntered = Completer<void>();
        var holdWrite = false;
        final heldWrite = Completer<void>();
        final sent = <ConnectionSessionPacket>[];
        final retirements = <Map<String, Object?>>[];
        Uint8List? helloBytes;
        Uint8List? welcomeBytes;
        Uint8List? identityBytes;

        Uint8List control(
          int code, {
          required int token,
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
          sent.add(packet);
          if (holdWrite &&
              ((orderedFailure &&
                      packet.kind == ConnectionSessionKind.ordered) ||
                  (failureKind == 'Query' &&
                      packet.kind == ConnectionSessionKind.control &&
                      packet.code == ConnectionSessionControl.query &&
                      packet.token == sessionStateToken))) {
            if (!writeEntered.isCompleted) writeEntered.complete();
            await heldWrite.future;
            return;
          }
          if (packet.kind == ConnectionSessionKind.ordered) return;
          if (replayFailure &&
              packet.kind == ConnectionSessionKind.control &&
              packet.code == ConnectionSessionControl.query &&
              packet.token == 2) {
            final progress = Uint8List(10);
            ByteData.sublistView(progress).setUint32(4, 8, Endian.little);
            packets.add(
              control(
                ConnectionSessionControl.progress,
                token: 2,
                body: progress,
              ),
            );
            return;
          }
          switch (packet.code) {
            case ConnectionSessionControl.hello:
              helloBytes = bytes;
              final welcomeBody = Uint8List(10);
              ByteData.sublistView(welcomeBody)
                ..setUint64(0, clientNonce, Endian.little)
                ..setUint16(8, 244, Endian.little);
              welcomeBytes = control(
                ConnectionSessionControl.welcome,
                token: 1,
                body: welcomeBody,
              );
              final identityBody = Uint8List(10);
              ByteData.sublistView(identityBody)
                ..setUint64(0, bootNonce, Endian.little)
                ..setUint16(
                  8,
                  ConnectionSession.orderedFeature |
                      ConnectionSession.stateFeature |
                      ConnectionSession.objectFeature |
                      ConnectionSession.reverseObjectFeature,
                  Endian.little,
                );
              identityBytes = control(
                ConnectionSessionControl.identity,
                token: 1,
                body: identityBody,
              );
              packets
                ..add(welcomeBytes!)
                ..add(identityBytes!);
            case ConnectionSessionControl.ready:
              final transcript = Uint8List.fromList([
                ...helloBytes!,
                ...welcomeBytes!,
                ...identityBytes!,
              ]);
              final crc = _crc32(transcript);
              final activatedBody = Uint8List(4);
              ByteData.sublistView(
                activatedBody,
              ).setUint32(0, crc, Endian.little);
              packets
                ..add(
                  control(
                    ConnectionSessionControl.activated,
                    token: 1,
                    body: activatedBody,
                  ),
                )
                ..add(
                  control(
                    ConnectionSessionControl.open,
                    token: 1,
                    sequence: 1,
                    body: Uint8List.fromList([
                      1,
                      0,
                      1,
                      ConnectionSession.stateCapacity,
                      10,
                      0,
                      40,
                      0,
                    ]),
                  ),
                )
                ..add(
                  control(
                    ConnectionSessionControl.open,
                    token: 1,
                    sequence: 2,
                    body: Uint8List.fromList([
                      2,
                      0,
                      1,
                      ConnectionSession.reverseObjectCapacity,
                      234,
                      0,
                      40,
                      0,
                    ]),
                  ),
                );
            case ConnectionSessionControl.open:
              final lane = packet.body[2];
              final openedBody = Uint8List(10);
              ByteData.sublistView(openedBody)
                ..setUint32(0, lane == 0 ? 2 : 3, Endian.little)
                ..setUint16(4, lane == 0 ? 10 : 234, Endian.little);
              openedBody[6] = lane == 0
                  ? 8
                  : ConnectionSession.reverseObjectCapacity;
              openedBody[7] = lane;
              ByteData.sublistView(openedBody).setUint16(8, 0, Endian.little);
              packets.add(
                control(
                  ConnectionSessionControl.opened,
                  token: 1,
                  sequence: packet.sequence,
                  body: openedBody,
                ),
              );
              if ((disconnectDuringWrite || orderedFailure) && lane == 0) {
                final grant = Uint8List(10);
                ByteData.sublistView(grant).setUint32(4, 8, Endian.little);
                packets.add(
                  control(
                    ConnectionSessionControl.progress,
                    token: 2,
                    body: grant,
                  ),
                );
              }
            default:
              break;
          }
        }

        Future<void> sendBatch(List<Uint8List> values) async {
          if (failNextObjectBatch) {
            failNextObjectBatch = false;
            throw StateError('simulated native Object batch failure');
          }
          for (final value in values) {
            await send(value);
          }
        }

        final session = ConnectionSession(
          packets: packets.stream,
          send: send,
          sendBatch: sendBatch,
          clientNonce: clientNonce,
          packetMaximum: 244,
          onRetired: retirements.add,
        );
        addTearDown(() async {
          await session.dispose();
          await packets.close();
        });
        await session.activate();
        expect(session.isActivated, isTrue);

        if (disconnectDuringWrite) {
          holdWrite = true;
          final action = failureKind == 'Query'
              ? session.readState(code: ConnectionSession.litchcraftStateCode)
              : session.sendOrdered(5, Uint8List.fromList([0, 15, 1, 2, 3]));
          final failure = expectLater(
            action,
            throwsA(
              isA<StateError>().having(
                (error) => error.message,
                'message',
                'Connection session disposed',
              ),
            ),
          );
          await writeEntered.future;
          await session.dispose();
          // Let the early domain outcome fail before the SDK returns. It must
          // have an observer already and still reach the caller exactly once.
          await Future<void>.delayed(Duration.zero);
          heldWrite.complete();
          await failure;
        } else if (orderedFailure) {
          final sentBeforeInvalid = sent.length;
          await expectLater(
            session.sendOrdered(5, Uint8List(11)),
            throwsA(isA<StateError>()),
          );
          expect(session.isActivated, isTrue);
          expect(sent, hasLength(sentBeforeInvalid));
          expect(retirements, isEmpty);

          holdWrite = !replayFailure;
          final nativeFailure = StateError('simulated native Ordered failure');
          final failure = expectLater(
            session.sendOrdered(5, Uint8List.fromList([0, 15, 1, 2, 3])),
            throwsA(same(nativeFailure)),
          );
          final queuedFailure = expectLater(
            session.sendOrdered(6, Uint8List(0)),
            throwsA(same(nativeFailure)),
          );
          if (replayFailure) {
            // Let two initial writes submit. The real recovery Query then
            // observes two holes and queues both exact retransmissions.
            await Future<void>.delayed(Duration.zero);
            expect(
              sent.where(
                (packet) => packet.kind == ConnectionSessionKind.ordered,
              ),
              hasLength(2),
            );
            holdWrite = true;
          }
          await writeEntered.future;
          heldWrite.completeError(nativeFailure);
          await Future.wait([failure, queuedFailure]);
          holdWrite = false;
          expect(packets.isClosed, isFalse);

          final sentAtFailure = sent.length;
          await Future<void>.delayed(session.recoveryProbeDelay * 2);
          final progress = Uint8List(10);
          ByteData.sublistView(progress).setUint32(4, 8, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.progress,
              token: 2,
              body: progress,
            ),
          );
          await Future<void>.delayed(Duration.zero);
          expect(
            sent,
            hasLength(sentAtFailure),
            reason: 'retirement cancels probes and exact retransmissions',
          );

          // Native failure does not end the notification stream. A late board
          // result must not complete the already-failed caller a second time.
          final checkpoint = Uint8List(10);
          ByteData.sublistView(checkpoint).setUint32(0, 42, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: 2,
              sequence: 1,
              body: checkpoint,
            ),
          );
          await Future<void>.delayed(Duration.zero);
          expect(session.isActivated, isFalse);
          expect(retirements, hasLength(1));
          expect(retirements.single['ordered_pending'], replayFailure ? 2 : 1);
          expect(retirements.single['ord_head_seq'], 1);
          await expectLater(
            session.sendOrdered(6, Uint8List(0)),
            throwsA(isA<StateError>()),
          );
          expect(
            sent.where(
              (packet) => packet.kind == ConnectionSessionKind.ordered,
            ),
            hasLength(replayFailure ? 3 : 1),
            reason: 'queued actions and repairs cannot enter a retired session',
          );
        } else {
          failNextObjectBatch = true;
          await expectLater(
            session.sendObject(0x74, Uint8List.fromList([0xcc])),
            throwsA(isA<StateError>()),
          );
          expect(
            session.isActivated,
            isFalse,
            reason:
                'a native Object batch failure retires the session instead of '
                'leaving a connected-looking dead transport',
          );
        }
        await session.dispose();
        await packets.close();
      },
    );
  }
}

int _u32(Uint8List bytes, [int offset = 0]) =>
    ByteData.sublistView(bytes).getUint32(offset, Endian.little);
int _u16(Uint8List bytes, [int offset = 0]) =>
    ByteData.sublistView(bytes).getUint16(offset, Endian.little);

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
