import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import '../connection_ota_codec.dart';
import '../connection_ota_status_codec.dart';
import '../connection_session.dart';
import '../connection_session_codec.dart';
import '../game_history_protocol.dart' show GameHistoryCrc32;
import '../litch_ble_service.dart';
import 'litch_firmware_uploader.dart';
import 'ota_status_event.dart';

enum OtaRamBenchmarkDirection { forward, reverse }

/// RAM reference for the opt-in runner's single physical-session run.
/// Session owns native backpressure, credits, ordering, repair and retirement.
final class OtaRamBenchmark {
  static const totalBytes = 300000;
  static const _run = 1;
  static const _seed = 0x51a7;
  static const _progressIntervalMs = 200;
  static const _reverseTimeout = Duration(minutes: 2);
  static const _deadlines = LitchFirmwareUploadOptions();

  static Future<Map<String, Object?>> run(
    LitchBleService service, {
    OtaRamBenchmarkDirection direction = OtaRamBenchmarkDirection.forward,
    void Function(int receivedBytes)? onProgress,
  }) async {
    final deviceId = service.connectedId;
    final generation = service.connectionSessionToken;
    if (deviceId == null || !service.productReady) {
      throw StateError('RAM reference requires a ready connection.');
    }
    final session = await service.openConnectionSession(deviceId);
    bool current() =>
        service.isConnected &&
        service.productReady &&
        service.connectedId == deviceId &&
        service.connectionSessionToken == generation &&
        session?.isActivated == true;
    if (!current() || session == null) {
      throw StateError('RAM reference shared session is unavailable.');
    }
    final changes = StreamController<void>.broadcast(sync: true);
    final assembler = ConnectionOtaStatusAssembler();
    Map<String, Object?>? probe, detail, begin, done;
    Object? failure;
    var received = 0;
    var beginSubmitted = false;
    var verified = false;
    final reverse = direction == OtaRamBenchmarkDirection.reverse;
    final reverseCrc = GameHistoryCrc32();
    final reverseClock = Stopwatch();
    int? firstDataMs, lastDataMs;
    var lastProgressMs = 0;
    void check() {
      if (!current()) throw StateError('RAM reference connection changed.');
      if (failure != null) throw failure!;
    }

    final subscription = session.objectReceipts.listen(
      (packet) {
        if (!current()) return;
        try {
          if (reverse && packet.code == 0x33 && beginSubmitted) {
            final body = packet.body;
            if (body.length < 3 ||
                ByteData.sublistView(body).getUint16(1, Endian.little) !=
                    _run ||
                done != null) {
              throw const FormatException('Invalid reverse RAM record.');
            }
            if (body[0] == 0x11 && body.length > 3) {
              final payload = Uint8List.sublistView(body, 3);
              if (payload.length > totalBytes - received) {
                throw const FormatException('Reverse RAM exceeds its size.');
              }
              for (var index = 0; index < payload.length; ++index) {
                final position = received + index;
                if (payload[index] !=
                    ((position * 33 + _seed + (position >> 8)) & 0xff)) {
                  throw const FormatException('Reverse RAM pattern mismatch.');
                }
              }
              reverseCrc.update(payload);
              final progressMs = reverseClock.elapsedMilliseconds;
              firstDataMs ??= progressMs;
              lastDataMs = progressMs;
              received += payload.length;
              // Presentation must not rebuild the screen for every packet.
              // Session credits and receive processing remain unthrottled.
              if (received == totalBytes ||
                  progressMs - lastProgressMs >= _progressIntervalMs) {
                lastProgressMs = progressMs;
                onProgress?.call(received);
              }
            } else if (body[0] == 0x12 && body.length == 10) {
              final total = body[3] | body[4] << 8 | body[5] << 16;
              final crc = ByteData.sublistView(
                body,
              ).getUint32(6, Endian.little);
              if (received != totalBytes ||
                  total != totalBytes ||
                  crc != reverseCrc.value) {
                throw const FormatException(
                  'Reverse RAM final proof mismatch.',
                );
              }
              done = {'crc32': crc.toRadixString(16).padLeft(8, '0')};
            } else {
              throw const FormatException('Unknown reverse RAM record.');
            }
            changes.add(null);
            return;
          }
          if (packet.code != ConnectionSession.otaStatusReceiptCode) return;
          final bytes = assembler.accept(packet.body);
          if (bytes == null) return;
          final event = OtaStatusEvent.decode(
            bytes,
            deviceId: deviceId,
            generation: generation,
          );
          if (event == null) throw const FormatException('Missing RAM status.');
          if (event.payload['s'] == 'object_bench_probe' &&
              event.payload['q'] == 1) {
            probe = event.payload;
          }
          if (event.payload['s'] == 'object_bench_detail' &&
              event.payload['q'] == 2) {
            detail = event.payload;
          }
          if (reverse &&
              beginSubmitted &&
              event.payload['s'] == 'object_bench_begin' &&
              event.payload['q'] == 3 &&
              event.payload['run'] == _run) {
            begin = event.payload;
          }
          if (beginSubmitted && !reverse) {
            if (event.kind == OtaStatusKind.begin) begin = event.payload;
            if (event.rxBytes case final int count) {
              if (count < 0 || count > totalBytes) {
                throw const FormatException('Invalid RAM receive count.');
              }
              if (count > received) {
                received = count;
                onProgress?.call(received);
              }
            }
            if (event.kind == OtaStatusKind.done) done = event.payload;
          }
          if (event.kind == OtaStatusKind.error ||
              event.kind == OtaStatusKind.aborted) {
            throw StateError('RAM reference rejected: ${event.rawText}');
          }
        } catch (error) {
          failure ??= error;
        }
        changes.add(null);
      },
      onError: (Object error) {
        failure ??= error;
        changes.add(null);
      },
      onDone: () {
        failure ??= StateError('RAM reference session retired.');
        changes.add(null);
      },
    );

    Future<void> waitFor(bool Function() ready, Duration timeout) async {
      check();
      if (!ready()) {
        await changes.stream
            .firstWhere((_) => ready() || failure != null || !current())
            .timeout(timeout);
      }
      check();
    }

    Future<void> control(Map<String, Object> command) async {
      check();
      final fragments = ConnectionOtaCodec.encodeControl(
        Uint8List.fromList(utf8.encode(jsonEncode(command))),
        maximumBodyBytes: session.objectMaximumBody,
      );
      await session.sendObjectBatch(
        fragments
            .map(
              (body) =>
                  (opcode: ConnectionOtaCodec.controlObjectOpcode, body: body),
            )
            .toList(growable: false),
        priority: ConnectionSessionObjectPriority.interactive,
        atomicMessage: true,
      );
      check();
    }

    try {
      await control({'cmd': 'object_bench_probe', 'q': 1});
      await waitFor(() => probe != null, _deadlines.beginTimeout);
      if (probe!['schema'] != 1 ||
          probe!['max_bytes'] != totalBytes ||
          probe!['code'] != 0x33 ||
          probe!['pattern'] != 1) {
        throw StateError('RAM reference schema is unsupported.');
      }
      await control({'cmd': 'object_bench_probe', 'q': 2, 'detail': 1});
      await waitFor(() => detail != null, _deadlines.beginTimeout);
      if (detail!['schema'] != 1) {
        throw StateError('RAM reference link schema is unsupported.');
      }
      final link = <String, Object?>{
        'mtu': probe!['mtu'],
        'interval_units': probe!['interval'],
        'phy_known': detail!['pk'],
        'tx_phy': detail!['tp'],
        'rx_phy': detail!['rp'],
        'dle_known': detail!['dk'],
        'tx_octets': detail!['txo'],
        'tx_time_us': detail!['txt'],
        'rx_octets': detail!['rxo'],
        'rx_time_us': detail!['rxt'],
        'notify_attempts': detail!['na'],
        'notify_failures': detail!['nf'],
        'reverse_replays': detail!['rr'],
      };
      beginSubmitted = true;
      if (reverse) {
        // Include Begin and the first data latency: this is a conservative
        // received-byte rate, never the SDK's queue-admission rate.
        reverseClock.start();
        await control({
          'cmd': 'object_bench_begin',
          'q': 3,
          'run': _run,
          'seed': _seed,
          'size': totalBytes,
        });
        await waitFor(() => begin != null, _deadlines.beginTimeout);
        onProgress?.call(received);
        await waitFor(() => done != null, _reverseTimeout);
        final streamMs = lastDataMs!;
        verified = true;
        return {
          'link': link,
          'direction': 'board_to_phone',
          'run': _run,
          'bytes': totalBytes,
          'rx_bytes': received,
          'crc32': done!['crc32'],
          'stream_ms': streamMs,
          'first_data_ms': firstDataMs,
          'finalize_ms': reverseClock.elapsedMilliseconds - streamMs,
          if (streamMs > 0) 'goodput_bytes_sec': totalBytes * 1000 / streamMs,
        };
      }
      await control({
        'cmd': 'ota_sim_begin',
        'bench_schema': 1,
        'run': _run,
        'seed': _seed,
        'size': totalBytes,
        'chunk': session.objectMaximumBody,
      });
      await waitFor(() => begin != null, _deadlines.beginTimeout);
      // Minimum-ATT peers use the existing compact {"b":1} acknowledgement.
      if (begin!['b'] != 1 &&
          (begin!['sim'] != 1 || begin!['total'] != totalBytes)) {
        throw StateError('RAM reference begin does not match.');
      }
      final advertisedChunk = begin!['chunk'];
      final chunkBytes = advertisedChunk is int && advertisedChunk > 0
          ? advertisedChunk.clamp(1, session.objectMaximumBody)
          : session.objectMaximumBody;
      final crc = GameHistoryCrc32();
      final streamClock = Stopwatch()..start();
      var offset = 0;
      var nativeUs = 0, grantWaitUs = 0;
      while (offset < totalBytes) {
        check();
        final batch = <ConnectionSessionObject>[];
        while (batch.length < ConnectionSessionCodec.maximumWindow &&
            offset < totalBytes) {
          final count = (totalBytes - offset).clamp(1, chunkBytes);
          final body = Uint8List(count);
          for (var index = 0; index < count; ++index) {
            final position = offset + index;
            body[index] = (position * 33 + _seed + (position >> 8)) & 0xff;
          }
          crc.update(body);
          batch.add((opcode: ConnectionOtaCodec.dataObjectOpcode, body: body));
          offset += count;
        }
        // Release the producer after native issue; peer admission and repair
        // remain Session-owned while the next bounded batch is generated.
        await session.sendObjectBatch(
          batch,
          completion: ConnectionSessionObjectCompletion.submitted,
          onObservation: (fields) {
            nativeUs += fields['native_us'] as int;
            grantWaitUs += fields['grant_wait_us'] as int;
          },
        );
      }
      final receiveClock = Stopwatch()..start();
      await waitFor(() => received == totalBytes, _deadlines.receiveTimeout);
      final finalRxMs = receiveClock.elapsedMilliseconds;
      final streamMs = streamClock.elapsedMilliseconds;
      final expectedCrc = crc.value.toRadixString(16).padLeft(8, '0');
      final endClock = Stopwatch()..start();
      await control({
        'cmd': 'ota_end',
        'size': totalBytes,
        'crc32': expectedCrc,
      });
      await waitFor(() => done != null, _deadlines.terminalTimeout);
      if (done!['bench'] != 1 ||
          done!['run'] != _run ||
          done!['rx'] != totalBytes ||
          done!['total'] != totalBytes ||
          done!['pattern'] != 1 ||
          done!['crc32'] != expectedCrc) {
        throw StateError('RAM reference final byte/CRC proof does not match.');
      }
      verified = true;
      return {
        'link': link,
        'run': _run,
        'bytes': totalBytes,
        'rx_bytes': received,
        'crc32': expectedCrc,
        'stream_ms': streamMs,
        'final_rx_ms': finalRxMs,
        'finalize_ms': endClock.elapsedMilliseconds,
        'native_ms': nativeUs ~/ 1000,
        'grant_wait_ms': grantWaitUs ~/ 1000,
        if (streamMs > 0) 'goodput_bytes_sec': totalBytes * 1000 / streamMs,
      };
    } finally {
      if (beginSubmitted && !verified && current()) {
        // Begin may have been admitted even if its acknowledgement was lost.
        // Cancel only this exact run; never retry or reconnect.
        try {
          // Prior rejection remains the result; allow the matching cancel to
          // reach the existing owner even after an error status was observed.
          failure = null;
          await control({
            'cmd': 'object_bench_cancel',
            'q': reverse ? 4 : 3,
            'run': _run,
          });
        } catch (_) {
          // The shared session remains responsible for its retained packets.
        }
      }
      await subscription.cancel();
      await changes.close();
      if (reverse && !verified) onProgress?.call(received);
    }
  }
}
