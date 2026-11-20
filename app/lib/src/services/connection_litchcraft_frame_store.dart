import 'dart:typed_data';

import 'app_trace_recorder.dart';
import 'connection_litchcraft_codec.dart';
import 'connection_session.dart';
import 'connection_session_codec.dart';
import 'litchcraft_frame_store_protocol.dart';
import 'litchcraft_protocol.dart';

/// Store domain packets share the connection-owned Object and Ordered lanes.
LitchcraftFrameStoreClient createConnectionLitchcraftFrameStoreClient({
  required ConnectionSession session,
  required int Function() monotonicMilliseconds,
  Duration responseTimeout = const Duration(seconds: 6),
  AppTraceSink? appTrace,
}) {
  var observedRequest = 0, completedSubmissions = 0, completedObjects = 0;
  Future<void> sendStorePacket(
    List<int> packet,
    ConnectionSessionObjectPriority priority, {
    Future<void>? cancellationFuture,
  }) {
    final fragments = ConnectionLitchcraftCodec.encodePacket(
      Uint8List.fromList(packet),
      maximumBodyBytes: session.objectMaximumBody,
    );
    return session
        .sendObjectBatch(
          fragments
              .map(
                (body) => (
                  opcode: ConnectionLitchcraftCodec.objectOpcode,
                  body: body,
                ),
              )
              .toList(growable: false),
          priority: priority,
          atomicMessage: true,
          cancellationFuture: cancellationFuture,
          messageEnds: [fragments.length],
        )
        .onError<ConnectionSessionObjectCancelled>(
          (error, stack) => throw const LitchcraftTransferCancelled(),
        );
  }

  final assembler = ConnectionLitchcraftAssembler();
  Future<void> writePacket(List<int> packet) async {
    final operation = packet.length > 2 ? packet[2] : -1;
    if (operation == LitchcraftV2Protocol.frameStoreCancel) {
      final outcome = await session.sendOrdered(
        operation,
        Uint8List.fromList(packet),
      );
      if (outcome.disposition != ConnectionSessionDisposition.applied) {
        throw LitchcraftRejected(outcome.reason);
      }
      return;
    }
    await sendStorePacket(packet, ConnectionSessionObjectPriority.interactive);
  }

  Future<void> writeBatch(
    List<List<int>> packets, {
    Future<void>? cancellationFuture,
    void Function(int admittedPackets)? onProgress,
  }) async {
    if (packets.isEmpty) return;
    var completedMessages = 0;
    // Store packets are sized to one Object on ATT244 links; smaller links
    // may still fragment. Fill the negotiated 54-Object admission window
    // based on the actual fragment count. Keep each aggregate atomic so a
    // control producer can still preempt between bounded windows.
    var objects = <ConnectionSessionObject>[];
    var messageEnds = <int>[];
    // Every Store batch retains its existing request header; no payload enters
    // the trace. The session contributes the activation lifetime and sequence.
    final first = packets.first;
    final request = first.length >= 9 ? first[7] | first[8] << 8 : null;
    if (request != observedRequest) {
      observedRequest = request ?? 0;
      completedSubmissions = 0;
      completedObjects = 0;
    }
    Future<void> flush() async {
      if (objects.isEmpty) return;
      final batch = objects;
      final ends = messageEnds;
      var admittedMessages = 0;
      objects = <ConnectionSessionObject>[];
      messageEnds = <int>[];
      await session
          .sendObjectBatch(
            batch,
            priority: ConnectionSessionObjectPriority.bulk,
            atomicMessage: true,
            cancellationFuture: cancellationFuture,
            messageEnds: ends,
            onAdmissionProgress: onProgress == null
                ? null
                : (admitted, _) {
                    final previous = admittedMessages;
                    while (admittedMessages < ends.length &&
                        ends[admittedMessages] <= admitted) {
                      admittedMessages += 1;
                    }
                    if (admittedMessages != previous) {
                      onProgress(completedMessages + admittedMessages);
                    }
                  },
            onObservation: appTrace == null
                ? null
                : (fields) => appTrace.record(
                    'litchcraft.object_submission',
                    fields: {...fields, 'request': request},
                  ),
          )
          .onError<ConnectionSessionObjectCancelled>(
            (error, stack) => throw const LitchcraftTransferCancelled(),
          );
      completedMessages += ends.length;
      // Count completed producer operations independently of retained trace
      // rows, so a missing observation cannot become complete timing evidence.
      if (request == observedRequest) {
        ++completedSubmissions;
        completedObjects += batch.length;
      }
    }

    for (final packet in packets) {
      final fragments = ConnectionLitchcraftCodec.encodePacket(
        Uint8List.fromList(packet),
        maximumBodyBytes: session.objectMaximumBody,
      );
      if (objects.isNotEmpty &&
          objects.length + fragments.length >
              ConnectionSessionCodec.maximumWindow) {
        await flush();
      }
      objects.addAll(
        fragments.map(
          (body) =>
              (opcode: ConnectionLitchcraftCodec.objectOpcode, body: body),
        ),
      );
      messageEnds.add(objects.length);
    }
    await flush();
  }

  return LitchcraftFrameStoreClient(
    packets: session.objectReceipts
        .map((receipt) {
          if (receipt.code != ConnectionLitchcraftCodec.receiptCode) {
            return receipt.body;
          }
          try {
            return assembler.accept(receipt.body);
          } on FormatException {
            return null;
          }
        })
        .where((bytes) => bytes != null)
        .cast<Uint8List>()
        .map(LitchcraftV2Packet.decode)
        .where((packet) => packet != null)
        .cast<LitchcraftV2Packet>(),
    writeControl: writePacket,
    writeBulk: (packet) =>
        sendStorePacket(packet, ConnectionSessionObjectPriority.bulk),
    writeBulkBatch: writeBatch,
    writeTransferPackets:
        (packets, {required bulk, cancellationFuture, onProgress}) {
          if (!bulk) {
            return sendStorePacket(
              packets.single,
              ConnectionSessionObjectPriority.interactive,
              cancellationFuture: cancellationFuture,
            );
          }
          return writeBatch(
            packets,
            cancellationFuture: cancellationFuture,
            onProgress: onProgress,
          );
        },
    objectSubmissionSummary: () => (
      request: observedRequest,
      submissions: completedSubmissions,
      objects: completedObjects,
    ),
    requestReceipt: session.requestObjectReceipt,
    // Ordered completion is the authoritative Cancel acknowledgement. A
    // second reverse-State wait would turn a successful intent change into
    // an avoidable multi-second UI stall.
    cancelWriteIsAuthoritative: true,
    monotonicMilliseconds: monotonicMilliseconds,
    responseTimeout: responseTimeout,
    appTrace: appTrace,
  );
}
