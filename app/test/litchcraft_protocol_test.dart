import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';

List<int> _event(
  int event, {
  int session = 0x12345678,
  int revision = 9,
  int requestId = 1,
  List<int> payload = const [],
}) {
  final bytes = Uint8List(12 + payload.length);
  final data = ByteData.sublistView(bytes);
  bytes[0] = LitchcraftProtocol.magic;
  bytes[1] = event;
  data.setUint32(2, session, Endian.little);
  data.setUint32(6, revision, Endian.little);
  data.setUint16(10, requestId, Endian.little);
  bytes.setRange(12, bytes.length, payload);
  return bytes;
}

List<int> _state({
  int session = 0x12345678,
  int requestId = 1,
  int status = 0,
  int undo = 0,
  int redo = 0,
}) => _event(
  LitchcraftProtocol.state,
  session: session,
  requestId: requestId,
  payload: [status, 1, 3, undo, redo, 0, 15, 0],
);

List<int> _v2Event(
  int event, {
  int session = 0x12345678,
  int revision = 9,
  int requestId = 1,
  List<int> payload = const [],
}) {
  final bytes = Uint8List(13 + payload.length);
  final data = ByteData.sublistView(bytes);
  bytes[0] = LitchcraftV2Protocol.magic;
  bytes[1] = LitchcraftV2Protocol.version;
  bytes[2] = event;
  data.setUint32(3, session, Endian.little);
  data.setUint32(7, revision, Endian.little);
  data.setUint16(11, requestId, Endian.little);
  bytes.setRange(13, bytes.length, payload);
  return bytes;
}

LitchcraftAnimationEncoding _v2Document() {
  final frame = LitchcraftAnimationFrame(
    durationMs: 100,
    emitters: List<LitchcraftAnimatedEmitter>.filled(
      256,
      const LitchcraftAnimatedEmitter(color: 0),
    ),
  );
  return encodeLitchcraftAnimationDocument(
    LitchcraftAnimationDocument(
      frames: [frame],
      topology: LitchcraftTopology.v3,
      favoriteColors: const [1, 2, 3, 4, 5, 6, 7, 8],
      sparkleSeed: 1,
    ),
  );
}

int _requestId(List<int> bytes) =>
    ByteData.sublistView(Uint8List.fromList(bytes)).getUint16(7, Endian.little);

void _v2Ack(
  StreamController<List<int>> packets,
  List<int> request, {
  int revision = 9,
  int status = 0,
  int session = 7,
}) {
  packets.add(
    _v2Event(
      LitchcraftV2Protocol.state,
      requestId: _requestId(request),
      session: session,
      revision: revision,
      payload: [status, 1, 1, 0, 3],
    ),
  );
}

void _v2SnapshotBody(
  StreamController<List<int>> packets,
  List<int> request, {
  int revision = 9,
  int chunkSize = 5,
  void Function()? beforeEnd,
  int? crcOverride,
}) {
  if (request[2] != LitchcraftV2Protocol.snapshot) return;
  final document = _v2Document();
  final crc = crcOverride ?? document.crc32;
  void emit(int event, [List<int> payload = const []]) => packets.add(
    _v2Event(
      event,
      session: 7,
      revision: revision,
      requestId: _requestId(request),
      payload: payload,
    ),
  );
  emit(LitchcraftV2Protocol.snapshotBegin, [
    3,
    document.bytes.length & 255,
    document.bytes.length >> 8,
    crc & 255,
    crc >> 8 & 255,
    crc >> 16 & 255,
    crc >> 24 & 255,
  ]);
  for (var offset = 0; offset < document.bytes.length; offset += chunkSize) {
    emit(LitchcraftV2Protocol.snapshotChunk, [
      offset & 255,
      offset >> 8,
      ...document.bytes.skip(offset).take(chunkSize),
    ]);
  }
  beforeEnd?.call();
  emit(LitchcraftV2Protocol.snapshotEnd);
}

class _ManualTimer implements Timer {
  _ManualTimer(this.duration, this.callback);
  final Duration duration;
  final void Function() callback;
  bool _active = true;
  int _tick = 0;
  @override
  bool get isActive => _active;
  @override
  int get tick => _tick;
  @override
  void cancel() => _active = false;
  void fire() {
    if (!_active) return;
    _active = false;
    _tick++;
    callback();
  }
}

void main() {
  test(
    'atomic Canvas Edit has exact minimum-MTU vectors and rejects malformed erasure',
    () {
      final fill = LitchcraftV2Protocol.request(13, 0x12345678, 0x4321, [
        4,
        3,
        2,
        1,
        3,
        5,
        0x34,
        0x56,
        0x78,
        0,
      ]);
      expect(fill, [
        0x4d,
        2,
        13,
        0x78,
        0x56,
        0x34,
        0x12,
        0x21,
        0x43,
        4,
        3,
        2,
        1,
        3,
        5,
        0x34,
        0x56,
        0x78,
        0,
      ]);
      expect(LitchcraftV2Protocol.request(13, 7, 1), hasLength(9));
      expect(
        LitchcraftV2Protocol.request(13, 7, 1, [4, 3, 2, 1, 3, 15, 0, 0, 0, 1]),
        hasLength(19),
      );
      for (final payload in [
        [4, 3, 2, 1, 8, 15, 0, 0, 0, 1],
        [4, 3, 2, 1, 3, 1, 0, 0, 0, 1],
        [4, 3, 2, 1, 3, 15, 1, 0, 0, 1],
        [4, 3, 2, 1, 3, 5, 1, 0, 0, 7],
        [4, 3, 2, 1, 3, 5, 0, 1, 0, 8],
        [4, 3, 2, 1, 3, 15, 0, 0, 0, 9],
      ]) {
        expect(
          () => LitchcraftV2Protocol.request(13, 7, 1, payload),
          throwsArgumentError,
        );
      }
      for (final rotation in [2, 3, 4]) {
        expect(
          LitchcraftV2Protocol.request(13, 7, 1, [
            4,
            3,
            2,
            1,
            3,
            15,
            0,
            0,
            0,
            rotation,
          ]),
          hasLength(19),
        );
        expect(
          () => LitchcraftV2Protocol.request(13, 7, 1, [
            4,
            3,
            2,
            1,
            3,
            1,
            0,
            0,
            0,
            rotation,
          ]),
          throwsArgumentError,
        );
      }
    },
  );
  test('v2 frame edits admit exact 50 through 5000 millisecond values', () {
    List<int> request(int duration) => LitchcraftV2Protocol.request(
      LitchcraftV2Protocol.frameEdit,
      7,
      1,
      <int>[0, 0, 0, 0, 3, 0, 0, duration & 0xff, duration >> 8],
    );

    expect(request(50).sublist(16), <int>[50, 0]);
    expect(request(137).sublist(16), <int>[137, 0]);
    expect(request(5000).sublist(16), <int>[0x88, 0x13]);
    expect(() => request(49), throwsArgumentError);
    expect(() => request(5001), throwsArgumentError);
  });

  test(
    'v2 app codecs and Cancel replay match all shared native packet fixtures',
    () async {
      List<int> decodeHex(String hex) => [
        for (var offset = 0; offset < hex.length; offset += 2)
          int.parse(hex.substring(offset, offset + 2), radix: 16),
      ];
      final rows = <String, List<int>>{};
      for (final line in File(
        '../test/fixtures/litchcraft_v2_protocol_bytes.csv',
      ).readAsLinesSync()) {
        if (line.isEmpty || line.startsWith('#') || line == 'name,hex') {
          continue;
        }
        final fields = line.split(',');
        expect(fields, hasLength(2));
        expect(rows.containsKey(fields[0]), isFalse);
        rows[fields[0]] = decodeHex(fields[1]);
      }
      expect(rows, hasLength(14));

      late List<int> document;
      late int crc;
      for (final line in File(
        '../test/fixtures/litchcraft_animation_codec_bytes.csv',
      ).readAsLinesSync()) {
        if (line.isEmpty || line.startsWith('#')) continue;
        final fields = line.split(',');
        if (fields[0] == 'length') {
          document = List<int>.filled(int.parse(fields[1]), 0);
        } else if (fields[0] == 'crc32') {
          crc = int.parse(fields[1], radix: 16);
        } else {
          final offset = int.parse(fields[0]);
          final bytes = decodeHex(fields[1]);
          document.setRange(offset, offset + bytes.length, bytes);
        }
      }
      final canonical = encodeLitchcraftAnimationDocument(
        decodeLitchcraftAnimationDocument(document, crc),
      );
      expect(canonical.bytes, document);
      expect(canonical.crc32, crc);

      final requests = <String, List<int>>{
        'replace_begin_no_document_request': LitchcraftV2Protocol.replaceBegin(
          session: 1001,
          request: 2,
          baseRevision: 0,
          length: canonical.bytes.length,
          crc32: canonical.crc32,
        ),
        'replace_begin_conflict_request': LitchcraftV2Protocol.replaceBegin(
          session: 1001,
          request: 4,
          baseRevision: 0,
          length: canonical.bytes.length,
          crc32: canonical.crc32,
        ),
        'snapshot_session_zero_request': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.snapshot,
          0,
          2,
        ),
        'cancel_session_zero_request': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.cancelTransfer,
          0,
          3,
          [2, 0],
        ),
      };
      final events = <String, (int, int, int, List<int>)>{
        'replace_begin_no_document_ack': (0x81, 0, 2, [0, 1, 0, 0, 0]),
        'unsupported_no_document_state': (0x81, 0, 2, [4, 1, 0, 0, 0]),
        'busy_no_document_state': (0x81, 0, 3, [1, 1, 0, 0, 0]),
        'snapshot_inactive_ack': (0x81, 1, 2, [0, 0, 1, 0, 3]),
        'snapshot_begin': (
          0x84,
          1,
          2,
          [
            3,
            canonical.bytes.length & 255,
            canonical.bytes.length >> 8,
            crc & 255,
            crc >> 8 & 255,
            crc >> 16 & 255,
            crc >> 24 & 255,
          ],
        ),
        'snapshot_chunk_att20': (
          0x85,
          1,
          2,
          [0, 0, ...canonical.bytes.take(5)],
        ),
        'snapshot_chunk_att244': (
          0x85,
          1,
          2,
          [0, 0, ...canonical.bytes.take(229)],
        ),
        'snapshot_end': (0x86, 1, 2, []),
        'cancel_inactive_ack': (0x81, 1, 3, [0, 0, 1, 0, 3]),
        'revision_conflict_state': (0x81, 1, 4, [9, 1, 1, 0, 3]),
      };
      expect(rows.keys.toSet(), {...requests.keys, ...events.keys});
      for (final row in rows.entries) {
        final request = requests[row.key];
        if (request != null) {
          expect(request, row.value, reason: row.key);
        } else {
          final expected = events[row.key]!;
          final packet = LitchcraftV2Packet.decode(row.value);
          expect(packet, isNotNull, reason: row.key);
          expect(packet!.event, expected.$1, reason: row.key);
          expect(packet.session, 1001, reason: row.key);
          expect(packet.revision, expected.$2, reason: row.key);
          expect(packet.requestId, expected.$3, reason: row.key);
          expect(packet.payload, expected.$4, reason: row.key);
        }
      }
      expect(rows['snapshot_chunk_att20'], hasLength(20));
      expect(rows['snapshot_chunk_att244'], hasLength(244));

      final packets = StreamController<List<int>>.broadcast(sync: true);
      var writes = 0;
      final link = LitchcraftLink(
        packets: packets.stream,
        allocateRequestId: () => 3,
        timeout: const Duration(milliseconds: 5),
        write: (bytes) async {
          expect(bytes, rows['cancel_session_zero_request']);
          if (++writes == 2) packets.add(rows['cancel_inactive_ack']!);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      try {
        final ack = await link.sendV2(LitchcraftV2Protocol.cancelTransfer, 0, [
          2,
          0,
        ]);
        expect(writes, 2);
        expect(ack.payload, [0, 0, 1, 0, 3]);
        expect(ack.revision, 1);
      } finally {
        await link.dispose();
        await packets.close();
      }
    },
  );

  test('v2 no-document State is a truthful bounded ACK or refusal', () {
    for (var status = 0; status <= 11; status++) {
      final bytes = <int>[
        0x4d,
        2,
        0x81,
        7,
        0,
        0,
        0,
        9,
        0,
        0,
        0,
        1,
        0,
        status,
        1,
        0,
        0,
        0,
      ];
      expect(LitchcraftV2Packet.decode(bytes)?.payload, [status, 1, 0, 0, 0]);
      for (final inconsistent in [
        [status, 1, 0, 0, 3],
        [status, 1, 1, 0, 0],
        [status, 1, 0, 1, 0],
      ]) {
        expect(
          LitchcraftV2Packet.decode([...bytes.take(13), ...inconsistent]),
          isNull,
        );
      }
    }
  });

  test(
    'v2 first replacement accepts no-document begin/chunk ACKs and explicit refusals',
    () async {
      for (final status in [0, 4, 9, 10, 11]) {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        final operations = <int>[];
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            operations.add(bytes[2]);
            final committed = bytes[2] == LitchcraftV2Protocol.replaceCommitOp;
            packets.add(
              _v2Event(
                LitchcraftV2Protocol.state,
                session: 7,
                revision: committed ? 10 : 9,
                requestId: _requestId(bytes),
                payload: [
                  bytes[2] == 9 ? status : 0,
                  1,
                  committed ? 1 : 0,
                  0,
                  committed ? 3 : 0,
                ],
              ),
            );
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final document = _v2Document();
        final result = LitchcraftV2DocumentClient(link).replace(
          session: 7,
          baseRevision: 9,
          document: document.bytes,
          crc32: document.crc32,
          maximumPacketBytes: 244,
        );
        if (status == 0) {
          expect((await result).revision, 10);
          expect(operations.last, 11);
        } else {
          await expectLater(
            result,
            throwsA(
              isA<LitchcraftRejected>().having(
                (error) => error.status,
                'status',
                status,
              ),
            ),
          );
          expect(operations, [9, 12]);
        }
        await link.dispose();
        await packets.close();
      }
    },
  );

  test(
    'v2 End then Changed on a separate event turn requires Inspect and resnapshot',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final operations = <int>[];
      var revision = 9;
      final changed = Completer<void>();
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          // SDK ingress is an event turn. A following Changed notification can
          // arrive before Inspect's native response, without a client delay.
          await Future<void>.delayed(Duration.zero);
          operations.add(bytes[2]);
          _v2Ack(packets, bytes, revision: revision);
          if (bytes[2] != LitchcraftV2Protocol.snapshot) return;
          _v2SnapshotBody(packets, bytes, revision: revision, chunkSize: 229);
          if (revision == 9) {
            unawaited(
              Future<void>.delayed(Duration.zero, () {
                revision = 10;
                packets.add(
                  _v2Event(
                    LitchcraftV2Protocol.changed,
                    session: 7,
                    revision: revision,
                    requestId: 0,
                  ),
                );
                changed.complete();
              }),
            );
          }
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final snapshot = await client.snapshot(maximumPacketBytes: 244);
      await changed.future;
      expect(snapshot.revision, 10);
      expect(operations, [8, 1, 8, 1]);
      expect(client.needsResync, isFalse);
      await Future<void>.delayed(Duration.zero);
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.changed,
          session: 7,
          revision: 11,
          requestId: 0,
        ),
      );
      expect(link.documentChangedRevision(7), 11);
      expect(client.needsResync, isTrue);
      expect(client.lastSnapshot!.revision, 10);
      await link.dispose();
      await packets.close();
    },
  );

  for (final session in [0, 7]) {
    for (final next in ['snapshot', 'replace']) {
      test(
        'v2 paused snapshot session $session cancels before immediate $next',
        () async {
          final packets = StreamController<List<int>>.broadcast(sync: true);
          final paused = Completer<void>(), cancelling = Completer<void>();
          final releaseCancel = Completer<void>();
          final operations = <int>[];
          var snapshots = 0, snapshotId = 0;
          var staged = false;
          final link = LitchcraftLink(
            packets: packets.stream,
            write: (bytes) async {
              operations.add(bytes[2]);
              if (bytes[2] == LitchcraftV2Protocol.cancelTransfer) {
                expect(bytes.sublist(9), LitchcraftProtocol.u16(snapshotId));
                expect(
                  ByteData.sublistView(
                    Uint8List.fromList(bytes),
                  ).getUint32(3, Endian.little),
                  session,
                );
                cancelling.complete();
                await releaseCancel.future;
                staged = false;
                _v2Ack(packets, bytes);
                return;
              }
              expect(
                staged,
                isFalse,
                reason: 'outgoing body must be cancelled before new admission',
              );
              _v2Ack(packets, bytes, revision: bytes[2] == 11 ? 10 : 9);
              if (bytes[2] == LitchcraftV2Protocol.snapshot) {
                snapshots++;
                if (snapshots == 1) {
                  snapshotId = _requestId(bytes);
                  final document = _v2Document();
                  final begin = ByteData(7)
                    ..setUint8(0, 3)
                    ..setUint16(1, document.bytes.length, Endian.little)
                    ..setUint32(3, document.crc32, Endian.little);
                  packets.add(
                    _v2Event(
                      LitchcraftV2Protocol.snapshotBegin,
                      session: 7,
                      requestId: snapshotId,
                      payload: begin.buffer.asUint8List(),
                    ),
                  );
                  staged = true;
                  paused.complete();
                } else {
                  _v2SnapshotBody(packets, bytes, chunkSize: 229);
                }
              }
            },
            onPacket: (_) {},
            onFailure: (_) {},
          );
          final client = LitchcraftV2DocumentClient(link);
          final aborted = expectLater(
            client.snapshot(session: session),
            throwsA(isA<LitchcraftTransferCancelled>()),
          );
          await paused.future;
          client.cancel();
          await cancelling.future;
          await expectLater(
            LitchcraftV2DocumentClient(link).snapshot(),
            throwsStateError,
          );
          releaseCancel.complete();
          await aborted;
          if (next == 'snapshot') {
            expect(
              (await client.snapshot(maximumPacketBytes: 244)).revision,
              9,
            );
          } else {
            final document = _v2Document();
            expect(
              (await client.replace(
                session: 7,
                baseRevision: 9,
                document: document.bytes,
                crc32: document.crc32,
                maximumPacketBytes: 244,
              )).revision,
              10,
            );
          }
          expect(operations.take(3), [8, 12, next == 'snapshot' ? 8 : 9]);
          await link.dispose();
          await packets.close();
        },
      );
    }
  }

  test(
    'v2 replacement freezes source and retries only identical unconfirmed chunk',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final encoding = _v2Document();
      final source = List<int>.of(encoding.bytes);
      final writes = <List<int>>[];
      final staged = <int>[];
      final progress = <int>[];
      List<int>? retry;
      final link = LitchcraftLink(
        packets: packets.stream,
        timeout: const Duration(milliseconds: 5),
        write: (bytes) async {
          writes.add(List.of(bytes));
          if (bytes[2] == LitchcraftV2Protocol.replaceBeginOp) {
            source.fillRange(0, source.length, 0);
            _v2Ack(packets, bytes);
          } else if (bytes[2] == LitchcraftV2Protocol.replaceChunkOp) {
            if (retry == null) {
              retry = List.of(bytes);
              return;
            }
            if (staged.isEmpty) {
              expect(bytes, retry);
              expect(progress, [0]);
            }
            staged.addAll(bytes.sublist(13));
            _v2Ack(packets, bytes);
          } else {
            _v2Ack(packets, bytes, revision: 10);
          }
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      await LitchcraftV2DocumentClient(link).replace(
        session: 7,
        baseRevision: 9,
        document: source,
        crc32: encoding.crc32,
        maximumPacketBytes: 244,
        onProgress: (confirmed, total) => progress.add(confirmed),
      );
      expect(staged, encoding.bytes);
      expect(writes[1], writes[2]);
      expect(progress.last, encoding.bytes.length);
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 invalid schema and packet budgets reject before any write',
    () async {
      var writes = 0;
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (_) async {
          writes++;
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final source = List<int>.of(_v2Document().bytes)..[0] = 2;
      await expectLater(
        client.replace(
          session: 7,
          baseRevision: 9,
          document: source,
          crc32: litchcraftAnimationCrc32(source),
        ),
        throwsFormatException,
      );
      for (final budget in [0, 13, 19, 245]) {
        await expectLater(
          client.snapshot(maximumPacketBytes: budget),
          throwsArgumentError,
        );
        await expectLater(
          client.replace(
            session: 7,
            baseRevision: 9,
            document: _v2Document().bytes,
            crc32: _v2Document().crc32,
            maximumPacketBytes: budget,
          ),
          throwsArgumentError,
        );
      }
      expect(writes, 0);
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 shares one outstanding owner with static requests and competing clients',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final written = Completer<void>();
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          _v2Ack(packets, bytes);
          if (!written.isCompleted) written.complete();
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final cancelled = expectLater(
        client.snapshot(),
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      await written.future;
      await expectLater(
        LitchcraftV2DocumentClient(link).snapshot(),
        throwsStateError,
      );
      await expectLater(
        link.send(LitchcraftProtocol.inspect, 0),
        throwsStateError,
      );
      await expectLater(
        link.sendV2(LitchcraftV2Protocol.inspect, 0),
        throwsStateError,
      );
      client.cancel();
      await cancelled;
      await link.dispose();
      await packets.close();
    },
  );

  test('v2 disconnect wakes a snapshot after its ACK immediately', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final written = Completer<void>();
    final link = LitchcraftLink(
      packets: packets.stream,
      write: (bytes) async {
        _v2Ack(packets, bytes);
        if (!written.isCompleted) written.complete();
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    final client = LitchcraftV2DocumentClient(link);
    final disconnected = expectLater(client.snapshot(), throwsStateError);
    await written.future;
    await packets.close();
    await disconnected;
    expect(client.lastSnapshot, isNull);
    await link.dispose();
  });

  test(
    'v2 pre-ACK watermark waits for its matching discovered session',
    () async {
      for (final watermarkSession in [7, 8]) {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        var snapshots = 0;
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            if (bytes[2] != LitchcraftV2Protocol.snapshot) {
              _v2Ack(packets, bytes, revision: snapshots == 1 ? 9 : 10);
              return;
            }
            snapshots++;
            if (snapshots == 1) {
              packets.add(
                _v2Event(
                  LitchcraftV2Protocol.changed,
                  session: watermarkSession,
                  revision: 10,
                  requestId: 0,
                ),
              );
            }
            final revision = snapshots == 1 ? 9 : 10;
            _v2Ack(packets, bytes, revision: revision);
            _v2SnapshotBody(packets, bytes, revision: revision, chunkSize: 229);
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final snapshot = await LitchcraftV2DocumentClient(
          link,
        ).snapshot(maximumPacketBytes: 244);
        expect(snapshots, watermarkSession == 7 ? 2 : 1);
        expect(snapshot.revision, watermarkSession == 7 ? 10 : 9);
        await link.dispose();
        await packets.close();
      }
    },
  );

  for (final packetBytes in [20, 244]) {
    test(
      'v2 snapshot validates complete frozen ATT $packetBytes document',
      () async {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        final progress = <int>[];
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            _v2Ack(packets, bytes);
            _v2SnapshotBody(packets, bytes, chunkSize: packetBytes - 15);
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final client = LitchcraftV2DocumentClient(link);
        final snapshot = await client.snapshot(
          maximumPacketBytes: packetBytes,
          onProgress: (confirmed, total) => progress.add(confirmed),
        );
        expect(snapshot.bytes, _v2Document().bytes);
        expect(snapshot.revision, 9);
        expect(progress.last, snapshot.bytes.length);
        expect(
          progress.length,
          (snapshot.bytes.length / (packetBytes - 15)).ceil() + 1,
        );
        expect(() => snapshot.bytes[0] = 0, throwsUnsupportedError);
        await link.dispose();
        await packets.close();
      },
    );

    test(
      'v2 replacement succeeds with confirmed ATT $packetBytes progress',
      () async {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        final document = _v2Document();
        final writes = <List<int>>[];
        final staged = <int>[];
        final progress = <int>[];
        var transferId = 0;
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            writes.add(List.of(bytes));
            expect(bytes.length, lessThanOrEqualTo(packetBytes));
            switch (bytes[2]) {
              case LitchcraftV2Protocol.replaceBeginOp:
                transferId = _requestId(bytes);
                expect(bytes.length, 19);
                _v2Ack(packets, bytes);
              case LitchcraftV2Protocol.replaceChunkOp:
                expect(bytes[9] | bytes[10] << 8, transferId);
                expect(bytes[11] | bytes[12] << 8, staged.length);
                expect(
                  progress.last,
                  staged.length,
                  reason: 'unsent/unacknowledged bytes are never progress',
                );
                staged.addAll(bytes.sublist(13));
                _v2Ack(packets, bytes);
              case LitchcraftV2Protocol.replaceCommitOp:
                expect(staged, document.bytes);
                _v2Ack(packets, bytes, revision: 10);
                packets.add(
                  _v2Event(
                    LitchcraftV2Protocol.changed,
                    session: 7,
                    revision: 10,
                    requestId: 0,
                  ),
                );
              default:
                fail('Unexpected operation ${bytes[2]}');
            }
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final result = await LitchcraftV2DocumentClient(link).replace(
          session: 7,
          baseRevision: 9,
          document: document.bytes,
          crc32: document.crc32,
          maximumPacketBytes: packetBytes,
          onProgress: (confirmed, total) => progress.add(confirmed),
        );
        expect(result.revision, 10);
        expect(progress.last, document.bytes.length);
        expect(
          writes.length,
          (document.bytes.length / (packetBytes - 13)).ceil() + 2,
        );
        expect(writes.map(_requestId).toSet().length, writes.length);
        await link.dispose();
        await packets.close();
      },
    );
  }

  test(
    'v2 Changed coalesces into one resync without adopting a watermark',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var snapshots = 0;
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          if (bytes[2] != LitchcraftV2Protocol.snapshot) {
            _v2Ack(packets, bytes, revision: snapshots == 1 ? 9 : 12);
            return;
          }
          snapshots++;
          final revision = snapshots == 1 ? 9 : 12;
          // The zero-session request has not learned its owner yet. Retain this
          // watermark until the ACK can validate its session.
          if (snapshots == 1) {
            packets.add(
              _v2Event(
                LitchcraftV2Protocol.changed,
                session: 7,
                revision: 12,
                requestId: 0,
              ),
            );
          }
          _v2Ack(packets, bytes, revision: revision);
          _v2SnapshotBody(
            packets,
            bytes,
            revision: revision,
            chunkSize: 229,
            beforeEnd: () {
              if (snapshots == 1) {
                for (final revision in [10, 12, 11, 12]) {
                  packets.add(
                    _v2Event(
                      LitchcraftV2Protocol.changed,
                      session: 7,
                      revision: revision,
                      requestId: 0,
                    ),
                  );
                }
              }
            },
          );
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final snapshot = await LitchcraftV2DocumentClient(
        link,
      ).snapshot(maximumPacketBytes: 244);
      expect(snapshots, 2);
      expect(snapshot.revision, 12);
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 continuously changing snapshots stop at three and retain coherent bytes',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var moving = false, snapshots = 0;
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          if (bytes[2] != LitchcraftV2Protocol.snapshot) {
            _v2Ack(packets, bytes, revision: snapshots);
            return;
          }
          snapshots++;
          _v2Ack(packets, bytes, revision: snapshots);
          _v2SnapshotBody(
            packets,
            bytes,
            revision: snapshots,
            chunkSize: 229,
            beforeEnd: () {
              if (moving) {
                packets.add(
                  _v2Event(
                    LitchcraftV2Protocol.changed,
                    session: 7,
                    revision: snapshots + 1,
                    requestId: 0,
                  ),
                );
              }
            },
          );
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final coherent = await client.snapshot(maximumPacketBytes: 244);
      moving = true;
      await expectLater(
        client.snapshot(maximumPacketBytes: 244),
        throwsA(isA<LitchcraftResyncRequired>()),
      );
      expect(snapshots, 4);
      expect(identical(client.lastSnapshot, coherent), isTrue);
      await link.dispose();
      await packets.close();
    },
  );

  for (final deadline in [
    const Duration(seconds: 6),
    const Duration(seconds: 180),
  ]) {
    test(
      'v2 snapshot actual $deadline timer fires despite malformed traffic',
      () async {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        final timers = <_ManualTimer>[];
        final written = Completer<List<int>>();
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            _v2Ack(packets, bytes);
            if (!written.isCompleted) written.complete(bytes);
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final client = LitchcraftV2DocumentClient(
          link,
          timer: (duration, callback) {
            final timer = _ManualTimer(duration, callback);
            timers.add(timer);
            return timer;
          },
        );
        final rejected = expectLater(
          client.snapshot(),
          throwsA(isA<TimeoutException>()),
        );
        final request = await written.future;
        packets.add(
          _v2Event(
            LitchcraftV2Protocol.snapshotChunk,
            session: 7,
            requestId: _requestId(request),
            payload: [0, 0],
          ),
        );
        expect(timers.where((timer) => timer.isActive), hasLength(2));
        timers.singleWhere((timer) => timer.duration == deadline).fire();
        await rejected;
        expect(client.lastSnapshot, isNull);
        expect(timers.any((timer) => timer.isActive), isFalse);
        await link.dispose();
        await packets.close();
      },
    );
  }

  for (final failure in ['conflict', 'cancel', 'inactivity', 'absolute']) {
    test('v2 replacement $failure retires only its staging', () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final timers = <_ManualTimer>[];
      final writes = <List<int>>[];
      final chunk = Completer<void>();
      late LitchcraftV2DocumentClient client;
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          writes.add(List.of(bytes));
          if (bytes[2] == LitchcraftV2Protocol.replaceChunkOp) {
            if (failure == 'conflict') _v2Ack(packets, bytes, status: 9);
            chunk.complete();
          } else {
            _v2Ack(packets, bytes);
          }
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      client = LitchcraftV2DocumentClient(
        link,
        timer: (duration, callback) {
          final timer = _ManualTimer(duration, callback);
          timers.add(timer);
          return timer;
        },
      );
      final document = _v2Document();
      final rejected = expectLater(
        client.replace(
          session: 7,
          baseRevision: 9,
          document: document.bytes,
          crc32: document.crc32,
        ),
        throwsA(
          failure == 'conflict'
              ? isA<LitchcraftRejected>()
              : failure == 'cancel'
              ? isA<LitchcraftTransferCancelled>()
              : isA<TimeoutException>(),
        ),
      );
      await chunk.future;
      if (failure == 'cancel') client.cancel();
      if (failure == 'inactivity' || failure == 'absolute') {
        timers
            .singleWhere(
              (timer) =>
                  timer.isActive &&
                  timer.duration ==
                      Duration(seconds: failure == 'inactivity' ? 6 : 180),
            )
            .fire();
      }
      await rejected;
      expect(writes.map((bytes) => bytes[2]), [9, 10, 12]);
      expect(
        writes.last.sublist(9),
        LitchcraftProtocol.u16(_requestId(writes.first)),
      );
      expect(timers.any((timer) => timer.isActive), isFalse);
      await link.dispose();
      await packets.close();
    });
  }

  test(
    'v2 snapshot requires ACK before Begin and immutable frozen authority',
    () async {
      for (final scenario in [
        'before_ack',
        'wrong_revision',
        'crc',
        'oversize',
      ]) {
        final packets = StreamController<List<int>>.broadcast(sync: true);
        final timers = <_ManualTimer>[];
        final emitted = Completer<void>();
        final link = LitchcraftLink(
          packets: packets.stream,
          write: (bytes) async {
            if (scenario != 'before_ack') _v2Ack(packets, bytes);
            _v2SnapshotBody(
              packets,
              bytes,
              chunkSize: 229,
              revision: scenario == 'wrong_revision' ? 10 : 9,
              crcOverride: scenario == 'crc' ? 0 : null,
            );
            if (scenario == 'before_ack') _v2Ack(packets, bytes);
            if (!emitted.isCompleted) emitted.complete();
          },
          onPacket: (_) {},
          onFailure: (_) {},
        );
        final client = LitchcraftV2DocumentClient(
          link,
          timer: (duration, callback) {
            final timer = _ManualTimer(duration, callback);
            timers.add(timer);
            return timer;
          },
        );
        final rejected = expectLater(
          client.snapshot(
            maximumPacketBytes: scenario == 'oversize' ? 20 : 244,
          ),
          scenario == 'wrong_revision'
              ? throwsA(isA<TimeoutException>())
              : throwsFormatException,
        );
        await emitted.future;
        if (scenario == 'wrong_revision') timers.last.fire();
        await rejected;
        expect(client.lastSnapshot, isNull);
        await link.dispose();
        await packets.close();
      }
    },
  );

  test(
    'v2 disposing old link cannot write cancel into its replacement',
    () async {
      final oldPackets = StreamController<List<int>>.broadcast(sync: true);
      final newPackets = StreamController<List<int>>.broadcast(sync: true);
      final delayedNative = Completer<void>();
      final oldChunk = Completer<void>();
      final oldWrites = <int>[];
      final oldLink = LitchcraftLink(
        packets: oldPackets.stream,
        write: (bytes) async {
          oldWrites.add(bytes[2]);
          if (bytes[2] == LitchcraftV2Protocol.replaceChunkOp) {
            oldChunk.complete();
            await delayedNative.future;
          } else {
            _v2Ack(oldPackets, bytes);
          }
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final document = _v2Document();
      final rejected = expectLater(
        LitchcraftV2DocumentClient(oldLink).replace(
          session: 7,
          baseRevision: 9,
          document: document.bytes,
          crc32: document.crc32,
        ),
        throwsStateError,
      );
      await oldChunk.future;
      await oldLink.dispose();
      var newWrites = 0;
      final newLink = LitchcraftLink(
        packets: newPackets.stream,
        write: (bytes) async {
          newWrites++;
          _v2Ack(newPackets, bytes);
          _v2SnapshotBody(newPackets, bytes, chunkSize: 229);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final snapshot = await LitchcraftV2DocumentClient(
        newLink,
      ).snapshot(maximumPacketBytes: 244);
      delayedNative.complete();
      await rejected;
      expect(oldWrites, [9, 10]);
      expect(newWrites, 2);
      expect(snapshot.bytes, document.bytes);
      await newLink.dispose();
      await oldPackets.close();
      await newPackets.close();
    },
  );

  test('v2 control fixtures preserve the minimum ATT envelope', () {
    expect(
      LitchcraftV2Protocol.replaceBegin(
        session: 0x89abcdef,
        request: 0x1234,
        baseRevision: 0x01020304,
        length: 0x1234,
        crc32: 0xaabbccdd,
      ),
      <int>[
        0x4d,
        2,
        9,
        0xef,
        0xcd,
        0xab,
        0x89,
        0x34,
        0x12,
        4,
        3,
        2,
        1,
        0x34,
        0x12,
        0xdd,
        0xcc,
        0xbb,
        0xaa,
      ],
    );
    final chunks = LitchcraftV2Protocol.replaceChunks(
      session: 1,
      requestStart: 7,
      transfer: 7,
      document: List<int>.generate(15, (int index) => index),
      maximumPacketBytes: 20,
    );
    expect(chunks.map((chunk) => chunk.length), <int>[20, 20, 14]);
    expect(chunks[0].sublist(9), <int>[7, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6]);
    expect(chunks[2].sublist(9), <int>[7, 0, 14, 0, 14]);
  });

  test(
    'v2 typed editing APIs emit exact minimum-ATT control packets',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final writes = <List<int>>[];
      var revision = 7;
      final link = LitchcraftLink(
        packets: packets.stream,
        allocateRequestId: () => writes.length + 1,
        write: (bytes) async {
          writes.add(List<int>.of(bytes));
          final operation = bytes[2];
          if (operation == LitchcraftV2Protocol.paint ||
              operation == LitchcraftV2Protocol.effect ||
              operation == LitchcraftV2Protocol.frameEdit ||
              operation == LitchcraftV2Protocol.undo ||
              operation == LitchcraftV2Protocol.redo) {
            revision++;
          }
          _v2Ack(packets, bytes, session: 7, revision: revision);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      expect((await link.inspectV2()).revision, 7);
      expect((await link.pauseV2(session: 7, frame: 1)).revision, 7);
      expect(
        (await link.paintV2(
          session: 7,
          frame: 1,
          stroke: 0x1234,
          flags: 3,
          mask: 15,
          color: 0x112233,
          squares: const [0, 31, 63],
        )).revision,
        8,
      );
      await link.effectV2(
        session: 7,
        frame: 1,
        effect: 8,
        durationMs: 2000,
        stroke: 0x1235,
        flags: 1,
        mask: 10,
        squares: const [8, 9, 10],
      );
      await link.addCopyFrameV2(session: 7, baseRevision: 9, source: 1);
      await link.deleteFrameV2(session: 7, baseRevision: 10, source: 1);
      await link.moveFrameV2(
        session: 7,
        baseRevision: 11,
        source: 1,
        destination: 2,
      );
      await link.setFrameDurationV2(
        session: 7,
        baseRevision: 12,
        source: 1,
        durationMs: 137,
      );
      await link.undoV2(session: 7);
      await link.redoV2(session: 7);
      expect(writes[0], <int>[0x4d, 2, 1, 0, 0, 0, 0, 1, 0, 1]);
      expect(writes[2], <int>[
        0x4d,
        2,
        3,
        7,
        0,
        0,
        0,
        3,
        0,
        1,
        0x34,
        0x12,
        3,
        15,
        0x11,
        0x22,
        0x33,
        0,
        31,
        63,
      ]);
      expect(writes[3], <int>[
        0x4d,
        2,
        4,
        7,
        0,
        0,
        0,
        4,
        0,
        1,
        8,
        0xd0,
        7,
        10,
        0x35,
        0x12,
        1,
        8,
        9,
        10,
      ]);
      expect(
        writes.where((bytes) => bytes[2] == LitchcraftV2Protocol.frameEdit),
        everyElement(hasLength(18)),
      );
      expect(writes[7].sublist(9), <int>[12, 0, 0, 0, 3, 1, 0, 0x89, 0]);
      expect(writes.last[2], LitchcraftV2Protocol.redo);
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 State exposes authoritative history flags and rejects reserved bits',
    () {
      final packet = LitchcraftV2Packet.decode(
        _v2Event(LitchcraftV2Protocol.state, payload: const [0, 0x0f, 1, 0, 3]),
      );
      expect(packet, isNotNull);
      final state = LitchcraftV2State.fromPacket(packet!);
      expect(
        (state.active, state.playing, state.canUndo, state.canRedo),
        (true, true, true, true),
      );
      expect(
        LitchcraftV2Packet.decode(
          _v2Event(
            LitchcraftV2Protocol.state,
            payload: const [0, 0x10, 1, 0, 3],
          ),
        ),
        isNull,
      );
    },
  );

  test('v2 history availability survives an identical Undo replay', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final writes = <List<int>>[];
    final link = LitchcraftLink(
      packets: packets.stream,
      timeout: const Duration(milliseconds: 5),
      write: (bytes) async {
        writes.add(List<int>.of(bytes));
        if (writes.length == 2) {
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.state,
              session: 7,
              revision: 12,
              requestId: _requestId(bytes),
              payload: const [0, 0x0d, 1, 0, 3],
            ),
          );
        }
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    final state = await link.undoV2(session: 7);
    expect(writes, hasLength(2));
    expect(writes[1], writes[0]);
    expect(
      (
        state.revision,
        state.active,
        state.playing,
        state.canUndo,
        state.canRedo,
      ),
      (12, true, false, true, true),
    );
    await link.dispose();
    await packets.close();
  });

  test('v2 typed edit validation fails before a write', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    var writes = 0;
    final link = LitchcraftLink(
      packets: packets.stream,
      write: (_) async {
        writes++;
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    expect(
      () => link.paintV2(
        session: 0,
        frame: 8,
        stroke: 0,
        flags: 4,
        mask: 0,
        color: -1,
        squares: const [64],
      ),
      throwsArgumentError,
    );
    expect(
      () => link.effectV2(
        session: 7,
        frame: 0,
        effect: 1,
        durationMs: 501,
        stroke: 1,
        flags: 0,
        mask: 1,
        squares: const [],
      ),
      throwsArgumentError,
    );
    for (final duration in const [49, 5001]) {
      expect(
        () => link.setFrameDurationV2(
          session: 7,
          baseRevision: 0,
          source: 0,
          durationMs: duration,
        ),
        throwsArgumentError,
      );
    }
    expect(writes, 0);
    await link.dispose();
    await packets.close();
  });

  test(
    'v2 missing timeline half triggers one Inspect recovery and fences busy work',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final timers = <_ManualTimer>[];
      final writes = <List<int>>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        timelineTimer: (duration, callback) {
          final timer = _ManualTimer(duration, callback);
          timers.add(timer);
          return timer;
        },
        write: (bytes) async {
          writes.add(List<int>.of(bytes));
          _v2Ack(packets, bytes, session: 7, revision: 4);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.state,
          session: 7,
          revision: 4,
          requestId: 0,
          payload: const [0, 3, 1, 0, 3],
        ),
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineLow,
          session: 7,
          revision: 4,
          payload: const [9, 0, 1, 0, 0, 0, 0],
        ),
      );
      timers.single.fire();
      await Future<void>.delayed(Duration.zero);
      expect(writes.single[2], LitchcraftV2Protocol.inspect);
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineHigh,
          session: 9,
          revision: 4,
          payload: const [9, 0, 0, 0, 0, 0, 0],
        ),
      );
      expect(writes, hasLength(1));
      await link.dispose();
      await packets.close();
    },
  );

  test('new mode session retires old timeline recovery intent', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final timers = <_ManualTimer>[];
    final requestedSessions = <int>[];
    final link = LitchcraftLink(
      packets: packets.stream,
      timelineTimer: (duration, callback) {
        final timer = _ManualTimer(duration, callback);
        timers.add(timer);
        return timer;
      },
      write: (bytes) async {
        final session = ByteData.sublistView(
          Uint8List.fromList(bytes),
        ).getUint32(3, Endian.little);
        requestedSessions.add(session);
        _v2Ack(packets, bytes, session: session, revision: 4);
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    void state(int session) => packets.add(
      _v2Event(
        LitchcraftV2Protocol.state,
        session: session,
        revision: 4,
        requestId: 0,
        payload: const [0, 3, 1, 0, 3],
      ),
    );
    state(7);
    packets.add(
      _v2Event(
        LitchcraftV2Protocol.timelineLow,
        session: 7,
        revision: 4,
        payload: const [9, 0, 1, 0, 0, 0, 0],
      ),
    );
    timers.single.fire();
    await Future<void>.delayed(Duration.zero);
    expect(requestedSessions, [7]);
    state(8);
    await link.inspectV2(session: 8);
    await Future<void>.delayed(Duration.zero);
    expect(
      requestedSessions,
      [7, 8],
      reason: 'new entry cannot restart an Inspect for the departed mode',
    );
    await link.dispose();
    await packets.close();
  });

  test(
    'v2 write cancellation lifetimes settle per request after native and ACK',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var settledScopes = 0;
      var writes = 0;
      var lastScopeSettled = false;
      var lastWriteIsCurrent = false;
      Completer<void>? nativeReturn;
      late final LitchcraftLink link;
      link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          ++writes;
          lastScopeSettled = false;
          final current = link.v2WriteValidity(bytes);
          unawaited(
            link.v2WriteRetirement(bytes).then((_) {
              ++settledScopes;
              lastScopeSettled = true;
            }),
          );
          // ACK may precede native return and remove the request from the map.
          _v2Ack(packets, bytes);
          lastWriteIsCurrent = current();
          await nativeReturn?.future;
          expect(current(), isTrue);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      addTearDown(() async {
        await link.dispose();
        await packets.close();
      });
      for (var request = 0; request < 32; ++request) {
        await link.inspectV2();
        await Future<void>.delayed(Duration.zero);
        expect(
          settledScopes,
          request + 1,
          reason:
              'Completed requests cannot retain Session cancellation listeners',
        );
      }
      nativeReturn = Completer<void>();
      var inspectionComplete = false;
      final inspection = link.inspectV2().then(
        (_) => inspectionComplete = true,
      );
      await Future<void>.delayed(Duration.zero);
      expect(writes, 33);
      expect(lastWriteIsCurrent, isTrue);
      expect(lastScopeSettled, isFalse);
      expect(inspectionComplete, isFalse);
      expect(settledScopes, 32);
      nativeReturn.complete();
      await inspection;
      await Future<void>.delayed(Duration.zero);
      expect(settledScopes, 33);
    },
  );

  test(
    'v2 has one shared outstanding request owner across typed operations',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final writes = <List<int>>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async => writes.add(List<int>.of(bytes)),
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final inspection = link.inspectV2();
      await Future<void>.delayed(Duration.zero);
      await expectLater(link.playV2(session: 7, frame: 0), throwsStateError);
      expect(writes, hasLength(1));
      final rejected = expectLater(inspection, throwsStateError);
      await link.dispose();
      await rejected;
      await packets.close();
    },
  );

  test(
    'v2 timeline pairs one matching sample, wraps nonzero IDs, and extrapolates',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var monotonic = 100;
      final link = LitchcraftLink(
        packets: packets.stream,
        monotonicMilliseconds: () => monotonic,
        write: (_) async {},
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final samples = <LitchcraftV2TimelineSample>[];
      final subscription = link.v2TimelineSamples.listen(samples.add);
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.state,
          session: 7,
          revision: 4,
          requestId: 0,
          payload: const [0, 3, 2, 1, 3],
        ),
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineLow,
          session: 7,
          revision: 4,
          payload: const [0xff, 0xff, 1, 5, 0, 0, 0],
        ),
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineHigh,
          session: 7,
          revision: 4,
          payload: const [0xff, 0xff, 1, 2, 0, 0, 0],
        ),
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineLow,
          session: 7,
          revision: 4,
          payload: const [1, 0, 0, 9, 0, 0, 0],
        ),
      );
      packets.add(
        _v2Event(
          LitchcraftV2Protocol.timelineHigh,
          session: 7,
          revision: 4,
          payload: const [1, 0, 0, 0, 0, 0, 0],
        ),
      );
      expect(samples, hasLength(2));
      expect(samples.first.sampleId, 0xffff);
      expect(samples.first.elapsedMilliseconds, 0x200000005);
      monotonic = 125;
      expect(samples.first.extrapolatedElapsedAt(monotonic), 0x20000001e);
      expect(samples.last.sampleId, 1);
      expect(samples.last.extrapolatedElapsedAt(999), 9);
      await subscription.cancel();
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'effect clock pairs one epoch sample and preserves 64-bit elapsed',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var monotonic = 100;
      final writes = <List<int>>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        monotonicMilliseconds: () => monotonic,
        write: (bytes) async => writes.add(bytes),
        onPacket: (_) {},
        onFailure: (_) {},
      );
      void state(int session) => packets.add(
        _v2Event(
          LitchcraftV2Protocol.state,
          session: session,
          revision: 4,
          requestId: 0,
          payload: const [0, 1, 1, 0, 3],
        ),
      );
      void half(int event, int sample, int value, {int session = 7}) =>
          packets.add(
            _v2Event(
              event,
              session: session,
              revision: 4,
              payload: [sample & 255, sample >> 8, 0, value, 0, 0, 0],
            ),
          );
      state(7);
      half(LitchcraftV2Protocol.effectClockLow, 0xffff, 5);
      half(LitchcraftV2Protocol.effectClockHigh, 1, 2);
      expect(link.effectClock, isNull, reason: 'different samples cannot pair');
      half(LitchcraftV2Protocol.effectClockLow, 1, 9);
      expect(link.effectClock!.elapsedMilliseconds, 0x200000009);
      monotonic = 125;
      expect(link.effectClock!.extrapolatedElapsedAt(monotonic), 0x200000022);
      final held = LitchcraftV2EffectClockSample(
        session: 7,
        revision: 4,
        sampleId: 1,
        elapsedMilliseconds: 0x200000022,
        receivedAtMonotonicMilliseconds: monotonic,
        transferSuspended: true,
      );
      expect(held.extrapolatedElapsedAt(monotonic + 5000), 0x200000022);
      state(8);
      expect(
        link.effectClock,
        isNull,
        reason: 'new entry must discard old phase',
      );
      half(LitchcraftV2Protocol.effectClockLow, 2, 9);
      half(LitchcraftV2Protocol.effectClockHigh, 2, 2);
      expect(link.effectClock, isNull, reason: 'old session is stale');
      half(LitchcraftV2Protocol.effectClockLow, 2, 3, session: 8);
      half(LitchcraftV2Protocol.effectClockHigh, 2, 0, session: 8);
      expect(link.effectClock!.elapsedMilliseconds, 3);
      expect(writes, isEmpty, reason: 'clock samples never start polling');
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'effect clock refresh anchors delayed pairs and admits later source samples',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final writes = <List<int>>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        monotonicMilliseconds: () => 100,
        write: (bytes) async {
          writes.add(bytes);
          // State and clock notifications are independent native deliveries.
          _v2Ack(packets, bytes, revision: 4);
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      addTearDown(() async {
        await link.dispose();
        await packets.close();
      });
      void half(int event, int sample, int value, int request) => packets.add(
        _v2Event(
          event,
          session: 7,
          revision: 4,
          requestId: request,
          payload: [sample & 255, sample >> 8, 0, value, 0, 0, 0],
        ),
      );
      void pair(int sample, int elapsed, int request) {
        half(LitchcraftV2Protocol.effectClockLow, sample, elapsed, request);
        half(LitchcraftV2Protocol.effectClockHigh, sample, 0, request);
      }

      await link.inspectV2(session: 7, refreshEffectClock: true);
      final first = _requestId(writes.last);
      expect(link.effectClock, isNull, reason: 'State is not a clock pair');
      pair(0xffff, 9, first);
      expect(link.effectClock!.elapsedMilliseconds, 9);

      await link.inspectV2(session: 7);
      final later = _requestId(writes.last);
      pair(1, 29, later);
      expect(
        link.effectClock!.elapsedMilliseconds,
        29,
        reason: 'Opt-in includes later Inspect samples across sample-ID wrap',
      );
      pair(0xffff, 99, first);
      expect(
        link.effectClock!.elapsedMilliseconds,
        29,
        reason: 'A delayed predecessor cannot reanchor the current source',
      );

      await link.inspectV2(session: 7, refreshEffectClock: true);
      final replacement = _requestId(writes.last);
      pair(1, 39, later);
      expect(
        link.effectClock,
        isNull,
        reason: 'Replacement waits for its own frozen pair',
      );
      pair(2, 0, replacement);
      expect(link.effectClock!.elapsedMilliseconds, 0);

      await link.inspectV2(session: 7);
      final current = _requestId(writes.last);
      half(LitchcraftV2Protocol.effectClockLow, 3, 49, current);
      half(LitchcraftV2Protocol.effectClockHigh, 3, 0, replacement);
      expect(
        link.effectClock!.elapsedMilliseconds,
        0,
        reason: 'Halves from different requests cannot form one clock',
      );
      half(LitchcraftV2Protocol.effectClockLow, 3, 49, current);
      half(LitchcraftV2Protocol.effectClockHigh, 3, 0, current);
      expect(link.effectClock!.elapsedMilliseconds, 49);
      pair(2, 89, replacement);
      expect(link.effectClock!.elapsedMilliseconds, 49);
      expect(writes, hasLength(4), reason: 'No clock polling or repair writes');
    },
  );

  test('v2 high-MTU chunks and notifications fail closed', () {
    final chunks = LitchcraftV2Protocol.replaceChunks(
      session: 1,
      requestStart: 1,
      transfer: 1,
      document: List<int>.generate(462, (int index) => index & 0xff),
      maximumPacketBytes: 244,
    );
    expect(chunks, hasLength(2));
    expect(chunks.first.length, 244);
    expect(chunks.last.length, 244);
    expect(
      LitchcraftV2Packet.decode(
        _v2Event(
          LitchcraftV2Protocol.snapshotBegin,
          payload: [3, 32, 0, 1, 2, 3, 4],
        ),
      ),
      isNotNull,
    );
    expect(
      LitchcraftV2Packet.decode(
        _v2Event(LitchcraftV2Protocol.snapshotChunk, payload: [0, 0]),
      ),
      isNull,
    );
    expect(
      LitchcraftV2Packet.decode(
        _v2Event(LitchcraftV2Protocol.state, payload: [0, 0x80, 1, 0, 3]),
      ),
      isNull,
    );
  });

  test('v2 retries byte-identically and fences a stale session ACK', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final writes = <List<int>>[];
    final link = LitchcraftLink(
      packets: packets.stream,
      timeout: const Duration(milliseconds: 10),
      write: (List<int> bytes) async {
        writes.add(List<int>.of(bytes));
        if (writes.length == 2) {
          const ack = <int>[0, 1, 1, 0, 3];
          packets.add(
            _v2Event(LitchcraftV2Protocol.state, session: 9, payload: ack),
          );
          packets.add(
            _v2Event(LitchcraftV2Protocol.state, session: 1, payload: ack),
          );
        }
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    final ack = await link.sendV2(
      LitchcraftV2Protocol.replaceCommitOp,
      1,
      <int>[1, 0],
    );
    expect(writes, hasLength(2));
    expect(writes[1], writes[0]);
    expect(ack.session, 1);
    await link.dispose();
    await packets.close();
  });

  test(
    'v2 snapshot accepts synchronous ACK then frozen minimum-MTU body',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final encoding = _v2Document();
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          final request = ByteData.sublistView(Uint8List.fromList(bytes));
          final id = request.getUint16(7, Endian.little);
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.state,
              session: 7,
              revision: 4,
              requestId: id,
              payload: const [0, 1, 1, 0, 3],
            ),
          );
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.snapshotBegin,
              session: 7,
              revision: 4,
              requestId: id,
              payload: [
                3,
                encoding.bytes.length & 255,
                encoding.bytes.length >> 8,
                encoding.crc32 & 255,
                encoding.crc32 >> 8 & 255,
                encoding.crc32 >> 16 & 255,
                encoding.crc32 >> 24 & 255,
              ],
            ),
          );
          for (var offset = 0; offset < encoding.bytes.length; offset += 5) {
            packets.add(
              _v2Event(
                LitchcraftV2Protocol.snapshotChunk,
                session: 7,
                revision: 4,
                requestId: id,
                payload: [
                  offset & 255,
                  offset >> 8,
                  ...encoding.bytes.skip(offset).take(5),
                ],
              ),
            );
          }
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.snapshotEnd,
              session: 7,
              revision: 4,
              requestId: id,
            ),
          );
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final snapshot = await LitchcraftV2DocumentClient(link).snapshot();
      expect(snapshot.bytes, encoding.bytes);
      expect(snapshot.revision, 4);
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 snapshot rejects corrupt frozen bytes without returning a body',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final encoding = _v2Document();
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          final id = ByteData.sublistView(
            Uint8List.fromList(bytes),
          ).getUint16(7, Endian.little);
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.state,
              session: 7,
              requestId: id,
              payload: const [0, 1, 1, 0, 3],
            ),
          );
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.snapshotBegin,
              session: 7,
              requestId: id,
              payload: [
                3,
                encoding.bytes.length & 255,
                encoding.bytes.length >> 8,
                0,
                0,
                0,
                0,
              ],
            ),
          );
          for (var offset = 0; offset < encoding.bytes.length; offset += 229) {
            packets.add(
              _v2Event(
                LitchcraftV2Protocol.snapshotChunk,
                session: 7,
                requestId: id,
                payload: [
                  offset & 255,
                  offset >> 8,
                  ...encoding.bytes.skip(offset).take(229),
                ],
              ),
            );
          }
          packets.add(
            _v2Event(
              LitchcraftV2Protocol.snapshotEnd,
              session: 7,
              requestId: id,
            ),
          );
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      await expectLater(
        LitchcraftV2DocumentClient(link).snapshot(),
        throwsFormatException,
      );
      await link.dispose();
      await packets.close();
    },
  );
  test('all append-only admission statuses remain valid state packets', () {
    expect(LitchcraftPacket.decode(_state(status: 8)), isNotNull);
    expect(LitchcraftPacket.decode(_state(status: 9)), isNotNull);
    expect(
      LitchcraftPacket.decode(
        _state(status: LitchcraftProtocol.statusEntryPending),
      ),
      isNotNull,
    );
    expect(LitchcraftPacket.decode(_state(status: 13)), isNull);
  });

  test('requests preserve full width little endian IDs at minimum ATT MTU', () {
    final bytes = LitchcraftProtocol.request(
      LitchcraftProtocol.paint,
      0x89abcdef,
      0x1234,
      [0x78, 0x56, 3, 0, 7, 8, 15, 31, 32, 55, 56, 63],
    );
    expect(bytes, [
      0x4c,
      4,
      0xef,
      0xcd,
      0xab,
      0x89,
      0x34,
      0x12,
      0x78,
      0x56,
      3,
      0,
      7,
      8,
      15,
      31,
      32,
      55,
      56,
      63,
    ]);
    expect(bytes.length, 20);
  });

  test(
    'every app interaction has its exact one-packet minimum-ATT envelope',
    () {
      final v1 = <String, Uint8List>{
        'enter': LitchcraftProtocol.request(LitchcraftProtocol.enter, 0, 1),
        'exit': LitchcraftProtocol.request(LitchcraftProtocol.exit, 7, 2),
        'tool paint/color/mask': LitchcraftProtocol.request(
          LitchcraftProtocol.tool,
          7,
          3,
          const <int>[0, 15, 0x11, 0x22, 0x33],
        ),
        'tool erase/swap': LitchcraftProtocol.request(
          LitchcraftProtocol.tool,
          7,
          4,
          const <int>[1, 5, 0x33, 0x22, 0x11],
        ),
        'paint one square': LitchcraftProtocol.request(
          LitchcraftProtocol.paint,
          7,
          5,
          const <int>[1, 0, 1, 15, 63],
        ),
        'paint nine squares': LitchcraftProtocol.request(
          LitchcraftProtocol.paint,
          7,
          6,
          const <int>[1, 0, 3, 0, 7, 8, 15, 31, 32, 55, 56, 63],
        ),
        'undo': LitchcraftProtocol.request(LitchcraftProtocol.undo, 7, 7),
        'redo': LitchcraftProtocol.request(LitchcraftProtocol.redo, 7, 8),
        'clear': LitchcraftProtocol.request(LitchcraftProtocol.clear, 7, 9),
        'inspect': LitchcraftProtocol.request(
          LitchcraftProtocol.inspect,
          7,
          10,
        ),
        'palette final two colors': LitchcraftProtocol.request(
          LitchcraftProtocol.paletteChunk,
          7,
          11,
          const <int>[6, 2, 1, 2, 3, 4, 5, 6],
        ),
        'palette three colors': LitchcraftProtocol.request(
          LitchcraftProtocol.paletteChunk,
          7,
          12,
          const <int>[0, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9],
        ),
        'shake erase': LitchcraftProtocol.request(
          LitchcraftProtocol.shakeErase,
          7,
          13,
        ),
      };
      expect(
        v1.map((name, bytes) => MapEntry(name, bytes.length)),
        <String, int>{
          'enter': 8,
          'exit': 8,
          'tool paint/color/mask': 13,
          'tool erase/swap': 13,
          'paint one square': 13,
          'paint nine squares': 20,
          'undo': 8,
          'redo': 8,
          'clear': 8,
          'inspect': 8,
          'palette final two colors': 16,
          'palette three colors': 19,
          'shake erase': 8,
        },
      );

      final v2 = <String, Uint8List>{
        'inspect': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.inspect,
          7,
          1,
        ),
        'pause': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.timeline,
          7,
          2,
          const <int>[0, 0],
        ),
        'play': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.timeline,
          7,
          3,
          const <int>[1, 0],
        ),
        'select frame': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.timeline,
          7,
          4,
          const <int>[2, 7],
        ),
        'paint/erase one square': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.paint,
          7,
          5,
          const <int>[7, 1, 0, 3, 15, 0, 0, 0, 63],
        ),
        'paint/erase three squares': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.paint,
          7,
          6,
          const <int>[0, 1, 0, 3, 15, 0x11, 0x22, 0x33, 0, 1, 2],
        ),
        'effect one square': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.effect,
          7,
          7,
          const <int>[7, 8, 0x10, 0x27, 15, 1, 0, 3, 63],
        ),
        'effect three squares': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.effect,
          7,
          8,
          const <int>[0, 1, 0xf4, 1, 15, 1, 0, 3, 0, 1, 2],
        ),
        'add/copy frame': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.frameEdit,
          7,
          9,
          const <int>[1, 0, 0, 0, 0, 0, 0, 0, 0],
        ),
        'delete frame': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.frameEdit,
          7,
          10,
          const <int>[1, 0, 0, 0, 1, 7, 0, 0, 0],
        ),
        'move frame': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.frameEdit,
          7,
          11,
          const <int>[1, 0, 0, 0, 2, 7, 0, 0, 0],
        ),
        'frame duration': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.frameEdit,
          7,
          12,
          const <int>[1, 0, 0, 0, 3, 7, 0, 0x88, 0x13],
        ),
        'undo': LitchcraftV2Protocol.request(LitchcraftV2Protocol.undo, 7, 13),
        'redo': LitchcraftV2Protocol.request(LitchcraftV2Protocol.redo, 7, 14),
        'cancel transfer': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.cancelTransfer,
          7,
          15,
          const <int>[1, 0],
        ),
        'fill canvas': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          16,
          const <int>[1, 0, 0, 0, 0, 15, 0x11, 0x22, 0x33, 0],
        ),
        'erase canvas': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          17,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 1],
        ),
        'rotate clockwise': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          18,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 2],
        ),
        'rotate half-turn': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          19,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 3],
        ),
        'rotate counterclockwise': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          20,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 4],
        ),
        'checker light fill': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          21,
          const <int>[1, 0, 0, 0, 0, 15, 0x11, 0x22, 0x33, 5],
        ),
        'checker dark fill': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          22,
          const <int>[1, 0, 0, 0, 0, 15, 0x11, 0x22, 0x33, 6],
        ),
        'checker light erase': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          23,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 7],
        ),
        'checker dark erase': LitchcraftV2Protocol.request(
          LitchcraftV2Protocol.canvasEdit,
          7,
          24,
          const <int>[1, 0, 0, 0, 0, 15, 0, 0, 0, 8],
        ),
      };
      expect(
        v2.map((name, bytes) => MapEntry(name, bytes.length)),
        <String, int>{
          'inspect': 9,
          'pause': 11,
          'play': 11,
          'select frame': 11,
          'paint/erase one square': 18,
          'paint/erase three squares': 20,
          'effect one square': 18,
          'effect three squares': 20,
          'add/copy frame': 18,
          'delete frame': 18,
          'move frame': 18,
          'frame duration': 18,
          'undo': 9,
          'redo': 9,
          'cancel transfer': 11,
          'fill canvas': 19,
          'erase canvas': 19,
          'rotate clockwise': 19,
          'rotate half-turn': 19,
          'rotate counterclockwise': 19,
          'checker light fill': 19,
          'checker dark fill': 19,
          'checker light erase': 19,
          'checker dark erase': 19,
        },
      );
      expect(
        <Uint8List>[...v1.values, ...v2.values],
        everyElement(hasLength(lessThanOrEqualTo(20))),
        reason: 'no user interaction may require BLE fragmentation at MTU 23',
      );
    },
  );

  test('every board interaction receipt fits one minimum-ATT packet', () {
    final v1 = <String, List<int>>{
      'mode/state/undo authority': _state(),
      'brush/color/mask authority': _event(
        LitchcraftProtocol.brush,
        payload: const <int>[0x11, 0x22, 0x33, 1, 15],
      ),
      'physical paint/erase cell': _event(
        LitchcraftProtocol.cell,
        payload: const <int>[63, 15, 0x11, 0x22, 0x33],
      ),
      'physical color pick pending': _event(
        LitchcraftProtocol.colorPick,
        payload: const <int>[LitchcraftProtocol.colorPickPicking],
      ),
      'physical color pick complete': _event(
        LitchcraftProtocol.colorPick,
        payload: const <int>[LitchcraftProtocol.colorPickPicked],
      ),
      'physical shake erase': _event(
        LitchcraftProtocol.gesture,
        payload: const <int>[LitchcraftProtocol.gestureShakeErase, 1, 0],
      ),
      'resync watermark': _event(LitchcraftProtocol.resyncRequired),
    };
    final v2 = <String, List<int>>{
      'animation state': _v2Event(
        LitchcraftV2Protocol.state,
        payload: const <int>[0, 15, 8, 7, LitchcraftV2Protocol.schema],
      ),
      'document changed watermark': _v2Event(LitchcraftV2Protocol.changed),
      'timeline low': _v2Event(
        LitchcraftV2Protocol.timelineLow,
        payload: const <int>[1, 0, 1, 0, 0, 0, 0],
      ),
      'timeline high': _v2Event(
        LitchcraftV2Protocol.timelineHigh,
        payload: const <int>[1, 0, 7, 0, 0, 0, 0],
      ),
      'effect clock low': _v2Event(
        LitchcraftV2Protocol.effectClockLow,
        payload: const <int>[1, 0, 0, 0, 0, 0, 0],
      ),
      'effect clock high': _v2Event(
        LitchcraftV2Protocol.effectClockHigh,
        payload: const <int>[1, 0, 0, 0, 0, 0, 0],
      ),
    };
    expect(v1.map((name, bytes) => MapEntry(name, bytes.length)), <String, int>{
      'mode/state/undo authority': 20,
      'brush/color/mask authority': 17,
      'physical paint/erase cell': 17,
      'physical color pick pending': 13,
      'physical color pick complete': 13,
      'physical shake erase': 15,
      'resync watermark': 12,
    });
    expect(v2.map((name, bytes) => MapEntry(name, bytes.length)), <String, int>{
      'animation state': 18,
      'document changed watermark': 13,
      'timeline low': 20,
      'timeline high': 20,
      'effect clock low': 20,
      'effect clock high': 20,
    });
    expect(
      v1.values.every((bytes) => LitchcraftPacket.decode(bytes) != null),
      isTrue,
    );
    expect(
      v2.values.every((bytes) => LitchcraftV2Packet.decode(bytes) != null),
      isTrue,
    );
    expect(
      <List<int>>[...v1.values, ...v2.values],
      everyElement(hasLength(lessThanOrEqualTo(20))),
      reason: 'board-originated interaction authority must never fragment',
    );
  });

  test('compact State accepts bounded firmware history counts', () {
    for (final count in [20, 21, 64, 65, 66, 255]) {
      for (final redo in [false, true]) {
        for (final event in [
          LitchcraftProtocol.state,
          LitchcraftProtocol.snapshotBegin,
        ]) {
          final bytes = _state(undo: redo ? 0 : count, redo: redo ? count : 0);
          bytes[1] = event;
          expect(
            LitchcraftPacket.decode(bytes),
            count <= (redo ? 64 : 65) ? isNotNull : isNull,
            reason:
                'event=$event undo=${redo ? 0 : count} redo=${redo ? count : 0}',
          );
        }
      }
    }
  });

  test(
    'mode ACK with full history survives its following resync watermark',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async {
          final id = ByteData.sublistView(
            Uint8List.fromList(bytes),
          ).getUint16(6, Endian.little);
          final state = _state(requestId: id, undo: 64);
          state[13] = bytes[1] == LitchcraftProtocol.enter ? 1 : 0;
          packets.add(state);
          packets.add(_event(LitchcraftProtocol.resyncRequired, requestId: id));
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      try {
        final entered = await link.send(LitchcraftProtocol.enter, 0);
        expect(entered.payload[1], 1);
        expect(entered.payload[3], 64);
        final exited = await link.send(
          LitchcraftProtocol.exit,
          entered.session,
        );
        expect(exited.payload[1], 0);
        expect(exited.payload[3], 64);
      } finally {
        await link.dispose();
        await packets.close();
      }
    },
  );

  test(
    'request builder rejects invalid IDs opcodes and overlength payloads',
    () {
      expect(
        LitchcraftProtocol.request(LitchcraftProtocol.shakeErase, 1, 1)[1],
        LitchcraftProtocol.shakeErase,
      );
      for (final opcode in [-1, 0, 15, 256]) {
        expect(
          () => LitchcraftProtocol.request(opcode, 1, 1),
          throwsArgumentError,
        );
      }
      for (final session in [-1, 0x100000000]) {
        expect(
          () => LitchcraftProtocol.request(1, session, 1),
          throwsArgumentError,
        );
      }
      for (final id in [-1, 0, 0x10000]) {
        expect(() => LitchcraftProtocol.request(1, 1, id), throwsArgumentError);
      }
      expect(
        () => LitchcraftProtocol.request(4, 1, 1, List.filled(13, 0)),
        throwsArgumentError,
      );
      expect(
        () => LitchcraftProtocol.request(4, 1, 1, [256]),
        throwsArgumentError,
      );
    },
  );

  test('high-MTU artwork framing sends an arbitrary canvas in four writes', () {
    final colors = List<int>.generate(256, (index) => index * 7919);
    final frames = LitchcraftProtocol.bulkReplaceFrames(
      session: 0x12345678,
      request: 0x4321,
      colors: colors,
      maximumPacketBytes: 244,
    );
    expect(frames, hasLength(4));
    expect(frames.every((frame) => frame.length <= 244), isTrue);
    final decoded = <int>[];
    var expectedOffset = 0;
    for (var frameIndex = 0; frameIndex < frames.length; ++frameIndex) {
      final frame = frames[frameIndex];
      final data = ByteData.sublistView(frame);
      expect(frame[0], LitchcraftProtocol.magic);
      expect(frame[1], LitchcraftProtocol.bulkReplace);
      expect(data.getUint32(2, Endian.little), 0x12345678);
      expect(data.getUint16(6, Endian.little), 0x4321);
      expect(data.getUint16(8, Endian.little), expectedOffset);
      expect(frame[10], frameIndex + 1 == frames.length ? 1 : 0);
      for (var index = 0; index < frame[11]; ++index) {
        decoded.add(LitchcraftProtocol.readRgb(frame, 12 + index * 3));
      }
      expectedOffset += frame[11];
    }
    expect(decoded, colors);
  });

  test('negotiated snapshot chunks decode across the full ATT payload', () {
    final colors = List<int>.generate(76, (index) => index * 7919);
    final packet = LitchcraftPacket.decode(
      _event(
        LitchcraftProtocol.snapshotChunk,
        payload: <int>[0, 0, ...colors.expand(LitchcraftProtocol.rgb)],
      ),
    );
    expect(packet, isNotNull);
    expect(packet!.payload.length, 230);
    expect(
      List<int>.generate(
        colors.length,
        (index) => LitchcraftProtocol.readRgb(packet.payload, 2 + index * 3),
      ),
      colors,
    );
  });

  test('state accepts firmware undo redo counts after multiple strokes', () {
    // Firmware sends actual history counts, including a changed open stroke,
    // rather than booleans. Two marks used to make every ACK undecodable.
    for (final count in [0, 1, 2, 19, 20]) {
      final packet = LitchcraftPacket.decode(_state(undo: count, redo: count));
      expect(packet, isNotNull, reason: 'history count $count is a valid byte');
      expect(packet!.payload[3], count);
    }
  });

  test(
    'snapshot accepts last logical emitter and rejects overflow/truncation',
    () {
      final last = LitchcraftPacket.decode(
        _event(
          LitchcraftProtocol.snapshotChunk,
          payload: [255, 0, 0x12, 0x34, 0x56],
        ),
      );
      expect(last, isNotNull);
      for (final payload in <List<int>>[
        [0, 1, 1, 2, 3],
        [255, 0, 1, 2, 3, 4, 5, 6],
        [0, 0],
        [0, 0, 1, 2],
        [0, 0, 1, 2, 3, 4],
      ]) {
        expect(
          LitchcraftPacket.decode(
            _event(LitchcraftProtocol.snapshotChunk, payload: payload),
          ),
          isNull,
          reason: '$payload',
        );
      }
    },
  );

  test(
    'malformed event addresses masks enums and packet sizes fail closed',
    () {
      for (final payload in <List<int>>[
        [64, 15, 1, 2, 3],
        [0, 0, 1, 2, 3],
        [0, 16, 1, 2, 3],
        [0, 15, 1, 2],
      ]) {
        expect(
          LitchcraftPacket.decode(
            _event(LitchcraftProtocol.cell, payload: payload),
          ),
          isNull,
        );
      }
      for (final payload in <List<int>>[
        [0, 1, 0, 0, 0, 0, 15, 0],
        [0, 1, 4, 0, 0, 0, 15, 0],
        [0, 4, 3, 0, 0, 0, 15, 0],
        [0, 1, 3, 0, 0, 2, 15, 0],
      ]) {
        expect(
          LitchcraftPacket.decode(
            _event(LitchcraftProtocol.state, payload: payload),
          ),
          isNull,
        );
      }
      expect(LitchcraftPacket.decode(_event(0xff)), isNull);
      expect(LitchcraftPacket.decode(List.filled(11, 0)), isNull);
      expect(LitchcraftPacket.decode([..._state(), 0]), isNull);
      expect(LitchcraftPacket.decode([-1, ..._state().skip(1)]), isNull);
    },
  );

  test('physical color-pick event is additive and bounded', () {
    for (final int phase in <int>[
      LitchcraftProtocol.colorPickIdle,
      LitchcraftProtocol.colorPickPicking,
      LitchcraftProtocol.colorPickPicked,
    ]) {
      expect(
        LitchcraftPacket.decode(
          _event(LitchcraftProtocol.colorPick, payload: <int>[phase]),
        ),
        isNotNull,
      );
    }
    expect(
      LitchcraftPacket.decode(
        _event(LitchcraftProtocol.colorPick, payload: const <int>[3]),
      ),
      isNull,
    );
  });

  test('shake erase event carries one nonzero correlated token', () {
    final packet = LitchcraftPacket.decode(
      _event(
        LitchcraftProtocol.gesture,
        revision: 0x78563412,
        requestId: 0,
        payload: const <int>[1, 0x34, 0x12],
      ),
    );
    expect(packet, isNotNull);
    expect(packet!.payload[0], LitchcraftProtocol.gestureShakeErase);
    expect(packet.payload[1] | packet.payload[2] << 8, 0x1234);
    expect(packet.revision, 0x78563412);

    for (final payload in <List<int>>[
      <int>[1, 0, 0],
      <int>[2, 1, 0],
      <int>[1, 1],
      <int>[1, 1, 0, 0],
    ]) {
      expect(
        LitchcraftPacket.decode(
          _event(LitchcraftProtocol.gesture, payload: payload),
        ),
        isNull,
        reason: '$payload',
      );
    }
  });

  test('timeouts retry identical operation bytes and accept one ACK', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final writes = <List<int>>[];
    final link = LitchcraftLink(
      packets: packets.stream,
      timeout: const Duration(milliseconds: 10),
      write: (bytes) async {
        writes.add(List.of(bytes));
        if (writes.length == 2) packets.add(_state());
      },
      onPacket: (_) {},
      onFailure: (_) {},
    );
    LitchcraftPacket? result;
    final sending = link
        .send(LitchcraftProtocol.paint, 0x12345678, [1, 0, 3, 0])
        .then((value) {
          result = value;
        });
    await Future<void>.delayed(Duration.zero);
    expect(writes, hasLength(1));
    await sending;
    expect(writes, hasLength(2));
    expect(writes[1], writes[0]);
    expect(result!.requestId, 1);
    await link.dispose();
    await packets.close();
  });

  test(
    'notification setup race retries admission before the user-visible timeout',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final writes = <List<int>>[];
      final stopwatch = Stopwatch()..start();
      final link = LitchcraftLink(
        packets: packets.stream,
        timeout: const Duration(milliseconds: 500),
        write: (bytes) async {
          writes.add(List.of(bytes));
          // The first firmware reply is lost when iOS has not finished
          // installing the notification CCCD. The identical retry is safe.
          if (writes.length == 2) packets.add(_state());
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );

      await link.send(LitchcraftProtocol.enter, 0);
      stopwatch.stop();

      expect(writes, hasLength(2));
      expect(writes[1], writes[0]);
      expect(stopwatch.elapsed, lessThan(const Duration(milliseconds: 400)));
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'exhausted silent notifications retire the link before repair',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var writes = 0;
      Object? failure;
      final link = LitchcraftLink(
        packets: packets.stream,
        timeout: const Duration(milliseconds: 5),
        write: (_) async => ++writes,
        onPacket: (_) {},
        onFailure: (error) => failure = error,
      );

      await expectLater(
        link.send(LitchcraftProtocol.inspect, 0),
        throwsA(isA<TimeoutException>()),
      );
      expect(writes, 3);
      expect(failure, isA<TimeoutException>());
      await expectLater(
        link.send(LitchcraftProtocol.inspect, 0),
        throwsStateError,
      );
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'wrong request or stale session success cannot acknowledge mutation',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final diagnostics = <(String, Map<String, Object?>)>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (_) async {},
        onPacket: (_) {},
        onFailure: (_) {},
        onDiagnostic: (event, fields) => diagnostics.add((event, fields)),
      );
      var complete = false;
      final sending = link
          .send(LitchcraftProtocol.tool, 0x12345678, [0, 15, 1, 2, 3])
          .then((_) {
            complete = true;
          });
      await Future<void>.delayed(Duration.zero);
      packets.add(_state(requestId: 55));
      packets.add(_state(session: 0x12345677));
      await Future<void>.delayed(Duration.zero);
      final incorrectlyCompleted = complete;
      packets.add(_state());
      await Future<void>.delayed(Duration.zero);
      await sending;
      await link.dispose();
      await packets.close();
      expect(
        incorrectlyCompleted,
        isFalse,
        reason:
            'a delayed ACK from an earlier art session is not this mutation',
      );
      expect(
        diagnostics.where(
          (record) =>
              record.$1 == 'packet_rejected' &&
              record.$2['reason'] == 'no_pending_request' &&
              record.$2['request_id'] == 55,
        ),
        hasLength(1),
      );
      expect(
        diagnostics.where(
          (record) =>
              record.$1 == 'packet_rejected' &&
              record.$2['reason'] == 'wrong_session' &&
              record.$2['request_id'] == 1,
        ),
        hasLength(1),
      );
      expect(
        diagnostics.map((record) => record.$1),
        containsAllInOrder(<String>['request_start', 'request_terminal']),
      );
      expect(diagnostics.last.$2, containsPair('outcome', 'acknowledged'));
    },
  );

  test(
    'v2 rejection diagnostics identify the exact operation and status',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final diagnostics = <(String, Map<String, Object?>)>[];
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (bytes) async => _v2Ack(packets, bytes, status: 11),
        onPacket: (_) {},
        onFailure: (_) {},
        onDiagnostic: (event, fields) => diagnostics.add((event, fields)),
      );

      await expectLater(
        link.inspectV2(),
        throwsA(
          isA<LitchcraftRejected>().having(
            (error) => error.status,
            'status',
            11,
          ),
        ),
      );

      expect(diagnostics.map((record) => record.$1), <String>[
        'request_start',
        'request_terminal',
      ]);
      expect(diagnostics.first.$2, containsPair('protocol', 2));
      expect(
        diagnostics.first.$2,
        containsPair('operation', LitchcraftV2Protocol.inspect),
      );
      expect(diagnostics.last.$2, containsPair('outcome', 'rejected'));
      expect(diagnostics.last.$2, containsPair('status', 11));
      await link.dispose();
      await packets.close();
    },
  );

  test(
    'v2 retry diagnostics preserve one request through terminal ACK',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final diagnostics = <(String, Map<String, Object?>)>[];
      var writes = 0;
      final link = LitchcraftLink(
        packets: packets.stream,
        timeout: const Duration(milliseconds: 10),
        write: (bytes) async {
          writes++;
          if (writes == 2) _v2Ack(packets, bytes);
        },
        onPacket: (_) {},
        onFailure: (_) {},
        onDiagnostic: (event, fields) => diagnostics.add((event, fields)),
      );

      final state = await link.inspectV2();

      expect(state.session, 7);
      expect(writes, 2);
      expect(diagnostics.map((record) => record.$1), <String>[
        'request_start',
        'request_retry',
        'request_terminal',
      ]);
      expect(diagnostics[1].$2, containsPair('attempt', 2));
      expect(diagnostics.last.$2, containsPair('outcome', 'acknowledged'));
      await link.dispose();
      await packets.close();
    },
  );

  test('explicit stale-session rejection is surfaced to caller', () async {
    final packets = StreamController<List<int>>.broadcast(sync: true);
    final link = LitchcraftLink(
      packets: packets.stream,
      write: (_) async {},
      onPacket: (_) {},
      onFailure: (_) {},
    );
    final rejected = expectLater(
      link.send(LitchcraftProtocol.undo, 8),
      throwsA(isA<LitchcraftRejected>().having((e) => e.status, 'status', 2)),
    );
    await Future<void>.delayed(Duration.zero);
    packets.add(_state(session: 9, status: 2));
    await Future<void>.delayed(Duration.zero);
    await rejected;
    await link.dispose();
    await packets.close();
  });

  test(
    'closed notification stream fails pending and prevents more writes',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      var writes = 0;
      final link = LitchcraftLink(
        packets: packets.stream,
        timeout: const Duration(seconds: 1),
        write: (_) async {
          ++writes;
        },
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final rejected = expectLater(
        link.send(LitchcraftProtocol.undo, 9),
        throwsStateError,
      );
      await Future<void>.delayed(Duration.zero);
      await packets.close();
      await Future<void>.delayed(Duration.zero);
      await rejected;
      Object? secondError;
      final second = link.send(LitchcraftProtocol.redo, 9).catchError((
        Object error,
      ) {
        secondError = error;
        return const LitchcraftPacket(0, 0, 0, 0, []);
      });
      await Future<void>.delayed(Duration.zero);
      await link.dispose();
      await Future<void>.delayed(Duration.zero);
      await second;
      expect(secondError, isA<StateError>());
      expect(
        writes,
        1,
        reason:
            'a dead notification channel must not send an unacknowledgeable mutation',
      );
    },
  );

  test(
    'stale-session congestion signal cannot cancel a current operation',
    () async {
      final packets = StreamController<List<int>>.broadcast(sync: true);
      final link = LitchcraftLink(
        packets: packets.stream,
        write: (_) async {},
        onPacket: (_) {},
        onFailure: (_) {},
      );
      Object? failure;
      final sending = link.send(LitchcraftProtocol.undo, 0x12345678).catchError(
        (Object error) {
          failure = error;
          return const LitchcraftPacket(0, 0, 0, 0, []);
        },
      );
      await Future<void>.delayed(Duration.zero);
      packets.add(
        _event(LitchcraftProtocol.resyncRequired, session: 0x12345677),
      );
      await Future<void>.delayed(Duration.zero);
      packets.add(_state());
      final result = await sending;
      await link.dispose();
      await packets.close();
      expect(failure, isNull);
      expect(result.event, LitchcraftProtocol.state);
    },
  );

  test('physical Store batch validates bounded complete footprint records', () {
    final bytes = Uint8List(57);
    final data = ByteData.sublistView(bytes);
    bytes[0] = LitchcraftV2Protocol.magic;
    bytes[1] = LitchcraftV2Protocol.version;
    bytes[2] = LitchcraftV2Protocol.physicalStoreBatch;
    data.setUint32(3, 41, Endian.little);
    data.setUint32(7, 12, Endian.little);
    data.setUint16(11, 12, Endian.little);
    bytes[13] = 0;
    bytes[14] = 2;
    for (var index = 0; index < 2; index++) {
      final offset = 15 + index * 21;
      data.setUint32(offset, 7, Endian.little);
      bytes[offset + 4] = 3;
      bytes[offset + 5] = 0x1f;
      bytes[offset + 6] = 0x11;
      bytes[offset + 7] = 0x22;
      bytes[offset + 8] = 0x33;
      data.setUint64(offset + 9, 1 << (8 + index), Endian.little);
      data.setUint32(offset + 17, 20 + index, Endian.little);
    }
    final packet = LitchcraftV2Packet.decode(bytes);
    expect(packet?.event, LitchcraftV2Protocol.physicalStoreBatch);
    expect(packet?.revision, 12);

    final badCount = Uint8List.fromList(bytes)..[14] = 3;
    expect(LitchcraftV2Packet.decode(badCount), isNull);
    final badFlags = Uint8List.fromList(bytes)..[20] = 0x20;
    expect(LitchcraftV2Packet.decode(badFlags), isNull);
    final emptyFootprint = Uint8List.fromList(bytes);
    ByteData.sublistView(emptyFootprint).setUint64(24, 0, Endian.little);
    expect(LitchcraftV2Packet.decode(emptyFootprint), isNull);
    final missingLiveRevision = Uint8List.fromList(bytes);
    ByteData.sublistView(missingLiveRevision).setUint32(32, 0, Endian.little);
    expect(LitchcraftV2Packet.decode(missingLiveRevision), isNull);
  });
}
