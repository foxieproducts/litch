import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import '../connection_ota_codec.dart';
import '../connection_ota_status_codec.dart';
import '../connection_session.dart';
import '../litch_ble_service.dart';
import 'ota_status_event.dart';

/// Measures actual phone delivery of the existing Console notify diagnostic.
/// SDK admission counts are retained separately and never prove receipt.
final class OtaNotifyBenchmark {
  static const deadline = Duration(seconds: 120);
  static const packetSizes = [244, 495];

  static Future<Map<String, Object?>> run(
    LitchBleService service, {
    int packetCount = 200,
    void Function(int receivedBytes, int totalBytes)? onProgress,
    Duration timeout = deadline,
  }) async {
    if (packetCount < 1 || packetCount > 10000 || timeout <= Duration.zero) {
      throw ArgumentError('Invalid notify measurement bounds.');
    }
    final deviceId = service.connectedId;
    final generation = service.connectionSessionToken;
    if (deviceId == null || !service.productReady) {
      throw StateError('Notify measurement requires a ready connection.');
    }
    final session = await service.openConnectionSession(deviceId);
    if (session == null || !session.isActivated) {
      throw StateError('Notify measurement shared session is unavailable.');
    }
    final clock = Stopwatch()..start();
    final changes = StreamController<void>.broadcast(sync: true);
    final assembler = ConnectionOtaStatusAssembler();
    final runs = <Map<String, Object?>>[];
    final result = <String, Object?>{'ok': false, 'runs': runs};
    final random = Random.secure();
    final firstRun = random.nextInt(0xffffffff) + 1;
    final query = (firstRun & 0xffff) == 0 ? 1 : firstRun & 0xffff;
    Map<String, Object?>? probe;
    _NotifyReceiveBatch? batch;
    Object? failure;
    var stopped = false;
    var completedBytes = 0;
    var totalBytes = 0;
    var lastProgressUs = 0;
    void fail(Object error) {
      failure ??= error;
      if (!stopped) changes.add(null);
    }

    bool current() =>
        !stopped &&
        service.isConnected &&
        service.productReady &&
        service.connectedId == deviceId &&
        service.connectionSessionToken == generation &&
        session.isActivated;
    void check() {
      if (!current()) throw StateError('notify_connection_changed');
      if (failure != null) throw failure!;
    }

    Duration remaining() {
      final value = timeout - clock.elapsed;
      if (value <= Duration.zero) throw TimeoutException('notify_deadline');
      return value;
    }

    Future<void> waitFor(bool Function() ready) async {
      check();
      if (!ready()) {
        await changes.stream
            .firstWhere((_) => ready() || failure != null || !current())
            .timeout(remaining());
      }
      check();
    }

    Future<void> control(Map<String, Object> command) async {
      check();
      await session
          .sendObjectBatch(
            ConnectionOtaCodec.encodeControl(
                  Uint8List.fromList(utf8.encode(jsonEncode(command))),
                  maximumBodyBytes: session.objectMaximumBody,
                )
                .map(
                  (body) => (
                    opcode: ConnectionOtaCodec.controlObjectOpcode,
                    body: body,
                  ),
                )
                .toList(growable: false),
            priority: ConnectionSessionObjectPriority.interactive,
            atomicMessage: true,
          )
          .timeout(remaining());
      check();
    }

    final status = session.objectReceipts.listen(
      (packet) {
        if (!current() ||
            packet.code != ConnectionSession.otaStatusReceiptCode) {
          return;
        }
        try {
          final bytes = assembler.accept(packet.body);
          if (bytes == null) return;
          final event = OtaStatusEvent.decode(
            bytes,
            deviceId: deviceId,
            generation: generation,
          );
          if (event == null) throw const FormatException('notify_status');
          if (event.kind == OtaStatusKind.error ||
              event.kind == OtaStatusKind.aborted) {
            throw StateError('notify_board_error: ${event.detail}');
          }
          if (event.payload['s'] == 'object_bench_probe' &&
              event.payload['q'] == query) {
            probe = event.payload;
          } else if (event.state == 'bench_notify' && batch != null) {
            batch.acceptTerminal(event.detail);
          }
        } catch (error) {
          failure ??= error;
        }
        changes.add(null);
      },
      onError: fail,
      onDone: () => fail(StateError('notify_session_retired')),
    );
    final connection = service.connectionStream.listen((_) {
      if (!current()) fail(StateError('notify_connection_changed'));
    }, onError: fail);
    final readiness = service.productReadinessStream.listen((ready) {
      if (!ready || !current()) fail(StateError('notify_readiness_changed'));
    }, onError: fail);
    void Function()? cancelObserver;
    try {
      cancelObserver = service.observeConsolePackets(
        deviceId: deviceId,
        connectionToken: generation,
        onError: fail,
        onPacket: (packet) {
          if (!current()) {
            fail(StateError('notify_connection_changed'));
            return;
          }
          final active = batch;
          if (active == null || failure != null) return;
          try {
            active.acceptPacket(packet);
            final progressUs = clock.elapsedMicroseconds;
            if (active.receivedPackets == packetCount ||
                progressUs - lastProgressUs >= 200000) {
              lastProgressUs = progressUs;
              onProgress?.call(
                completedBytes + active.receivedBytes,
                totalBytes,
              );
            }
          } catch (error) {
            failure ??= error;
          }
          changes.add(null);
        },
      );
      await control({'cmd': 'object_bench_probe', 'q': query});
      await waitFor(() => probe != null);
      final mtu = probe!['mtu'];
      if (probe!['schema'] != 1 || mtu is! int || mtu < 23 || mtu > 517) {
        throw const FormatException('notify_probe');
      }
      final maximum = min(mtu - 3, 512);
      totalBytes = packetSizes.fold(
        0,
        (sum, size) => sum + min(size, maximum) * packetCount,
      );
      result.addAll({
        'mtu': mtu,
        'bytes': totalBytes,
        'link': <String, Object?>{
          'mtu': mtu,
          'interval_units': probe!['interval'],
          for (final key in const [
            'phy_known',
            'tx_phy',
            'rx_phy',
            'dle_known',
          ])
            key: probe![key],
        },
      });
      onProgress?.call(0, totalBytes);
      for (final requestedSize in packetSizes) {
        check();
        final run = requestedSize == packetSizes.first
            ? firstRun
            : firstRun == 0xffffffff
            ? 1
            : firstRun + 1;
        final active = _NotifyReceiveBatch(
          run,
          requestedSize,
          min(requestedSize, maximum),
          packetCount,
        );
        batch = active;
        await control({
          'cmd': 'notify_test',
          'size': requestedSize,
          'count': packetCount,
          'delay_ms': 0,
          'run': run,
        });
        await waitFor(
          () =>
              active.terminal != null &&
              (active.packetError != null ||
                  active.receivedPackets == packetCount),
        );
        active.verify();
        runs.add(active.snapshot(ok: true));
        completedBytes += active.receivedBytes;
        batch = null;
      }
      result['ok'] = true;
    } catch (error) {
      if (batch case final active?) runs.add(active.snapshot(ok: false));
      result['error'] = error.toString();
      result['error_code'] = _errorReason(error);
      result['error_kind'] = error is TimeoutException
          ? 'deadline'
          : 'rejected';
    } finally {
      stopped = true;
      cancelObserver?.call();
      await status.cancel();
      await connection.cancel();
      await readiness.cancel();
      await changes.close();
    }
    result['rx_bytes'] = runs.fold<int>(
      0,
      (sum, run) => sum + (run['rx_bytes']! as int),
    );
    return result;
  }

  static String _errorReason(Object error) {
    final message = switch (error) {
      TimeoutException() => 'notify_deadline',
      FormatException(:final message) => message,
      StateError(:final message) => message.toString(),
      _ => error.toString(),
    };
    // AppTrace accepts only bounded ASCII tokens; preserve the actual reason
    // instead of letting spaces/native error prose become "redacted".
    final token = message.replaceAll(RegExp(r'[^A-Za-z0-9_.:-]+'), '_');
    return token.substring(0, min(token.length, 64));
  }
}

class _NotifyReceiveBatch {
  _NotifyReceiveBatch(
    this.run,
    this.requestedSize,
    this.packetSize,
    this.count,
  );

  final int run, requestedSize, packetSize, count;
  final seen = <int>{}; // Only validated ordinals below count enter this set.
  final clock = Stopwatch()..start();
  Map<String, int>? terminal;
  String? packetError;
  var highestOrdinal = -1;
  int receivedPackets = 0, receivedBytes = 0;
  int observedPackets = 0, observedBytes = 0;
  int corruptPackets = 0,
      duplicatePackets = 0,
      gapPackets = 0,
      oldRunPackets = 0;
  int? firstUs, lastUs;

  void acceptPacket(List<int> bytes) {
    final now = clock.elapsedMicroseconds;
    if (bytes.isEmpty || bytes[0] != 1) return; // Ordinary Console text.
    observedPackets++;
    observedBytes += bytes.length;
    if (bytes.length < 16 ||
        bytes[1] != 0x1e ||
        bytes[2] != 0x1f ||
        bytes[3] != 1) {
      corruptPackets++;
      packetError ??= 'notify_header';
      return;
    }
    var packetRun = 0, ordinal = 0;
    for (var index = 4; index < 16; index++) {
      if ((bytes[index] & 0xf0) != 0x10) {
        corruptPackets++;
        packetError ??= 'notify_header';
        return;
      }
      if (index < 12) {
        packetRun |= (bytes[index] & 15) << ((index - 4) * 4);
      } else {
        ordinal |= (bytes[index] & 15) << ((index - 12) * 4);
      }
    }
    if (packetRun != run) {
      oldRunPackets++;
      return;
    }
    if (bytes.length != packetSize ||
        ordinal >= count ||
        bytes.skip(16).any((byte) => byte != 1)) {
      corruptPackets++;
      packetError ??= 'notify_payload';
      return;
    }
    if (!seen.add(ordinal)) {
      duplicatePackets++;
      packetError ??= 'notify_duplicate';
      return;
    }
    if (ordinal > highestOrdinal + 1) {
      gapPackets += ordinal - highestOrdinal - 1;
      packetError ??= 'notify_gap';
    }
    highestOrdinal = max(highestOrdinal, ordinal);
    receivedPackets++;
    receivedBytes += bytes.length;
    firstUs ??= now;
    lastUs = now;
  }

  void acceptTerminal(String detail) {
    final values = <String, int>{};
    for (final field in detail.split(' ')) {
      final pair = field.split('=');
      if (pair.length != 2 || values.containsKey(pair[0])) {
        throw const FormatException('notify_terminal');
      }
      final value = int.tryParse(pair[1]);
      if (value == null || value < 0) {
        throw const FormatException('notify_terminal');
      }
      values[pair[0]] = value;
    }
    if (values['run'] != run) return;
    if (terminal != null) {
      throw const FormatException('notify_duplicate_terminal');
    }
    terminal = values;
    if (values['size'] != packetSize ||
        values['count'] != count ||
        values['ok'] != count ||
        values['fail'] == null ||
        values['elapsed_ms'] == null ||
        values['rate_Bps'] == null) {
      throw const FormatException('notify_sdk_result');
    }
  }

  void verify() {
    if (packetError case final error?) throw FormatException(error);
    if (receivedPackets != count ||
        receivedBytes != count * packetSize ||
        terminal == null) {
      throw const FormatException('notify_receipt_count');
    }
  }

  Map<String, Object?> snapshot({required bool ok}) => {
    'ok': ok,
    'run': run,
    'requested_size': requestedSize,
    'packet_size': packetSize,
    'count': count,
    'rx_packets': receivedPackets,
    'rx_bytes': receivedBytes,
    'observed_packets': observedPackets,
    'observed_bytes': observedBytes,
    'missing_packets': count - receivedPackets,
    'corrupt_packets': corruptPackets,
    'duplicate_packets': duplicatePackets,
    'gap_packets': gapPackets,
    'old_run_packets': oldRunPackets,
    if (packetError != null) 'packet_error': packetError,
    'first_us': firstUs,
    'last_us': lastUs,
    if (firstUs != null && lastUs != null)
      'receive_span_us': lastUs! - firstUs!,
    if (lastUs != null && lastUs! > 0)
      'start_to_last_Bps': receivedBytes * 1000000 / lastUs!,
    if (firstUs != null && lastUs! > firstUs!)
      'first_to_last_Bps':
          (receivedBytes - packetSize) * 1000000 / (lastUs! - firstUs!),
    if (terminal != null) 'sdk': terminal,
  };
}
