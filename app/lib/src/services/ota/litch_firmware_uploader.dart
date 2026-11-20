import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'ota_status_event.dart';

typedef OtaControlSender = Future<bool> Function(Map<String, Object> payload);
typedef OtaChunkSender =
    Future<void> Function(List<int> bytes, {required bool reliable});

/// The sender only establishes that the board accepted the write transaction.
/// A rebooting board must be checked again before the UI can call an update
/// successful.
sealed class OtaUploadResult {
  const OtaUploadResult(this.message);

  final String message;
}

class OtaUploadFinalized extends OtaUploadResult {
  const OtaUploadFinalized(super.message, {required this.connectionToken});

  final int connectionToken;
}

class OtaUploadPendingVerification extends OtaUploadResult {
  const OtaUploadPendingVerification(
    super.message, {
    required this.connectionToken,
  });

  final int connectionToken;
}

class OtaUploadFailed extends OtaUploadResult {
  const OtaUploadFailed(super.message, {this.detail = ''});

  final String detail;
}

class LitchFirmwareUploadOptions {
  const LitchFirmwareUploadOptions({
    this.chunkBytes = 96,
    this.compactStatusWindowBytes = 4096,
    this.maximumWindowBytes = 12288,
    this.reliableBurst = 12,
    this.beginTimeout = const Duration(seconds: 6),
    this.receiveTimeout = const Duration(seconds: 12),
    // End may spend 10 seconds saving and up to 5 more acquiring flash.
    this.terminalTimeout = const Duration(seconds: 20),
  });

  final int chunkBytes;
  final int compactStatusWindowBytes;
  final int maximumWindowBytes;
  final int reliableBurst;
  final Duration beginTimeout;
  final Duration receiveTimeout;
  final Duration terminalTimeout;
}

class LitchFirmwareUploader {
  LitchFirmwareUploader({
    required this.connectionToken,
    required Stream<OtaStatusEvent> events,
    required OtaControlSender sendControl,
    required OtaChunkSender writeChunk,
    Stream<void>? disconnects,
    this.beforeEnd,
    this.flushPendingChunks,
    this.options = const LitchFirmwareUploadOptions(),
  }) : _events = events,
       _sendControl = sendControl,
       _writeChunk = writeChunk,
       _disconnects = disconnects;

  final Stream<OtaStatusEvent> _events;
  final int connectionToken;
  final OtaControlSender _sendControl;
  final OtaChunkSender _writeChunk;
  final Stream<void>? _disconnects;
  final LitchFirmwareUploadOptions options;
  // Must durably retain the exact admitted attempt before End can be submitted.
  // Nullable only for source compatibility: absence prevents finalization.
  final Future<void> Function()? beforeEnd;
  // Shared adapters may buffer chunks until a native batch boundary.
  final Future<void> Function()? flushPendingChunks;

  Future<OtaUploadResult> upload({
    required File firmwareFile,
    required int totalBytes,
    void Function(double progress)? onProgress,
    void Function(String status)? onStatus,
    // One bounded diagnostic record; never a user-facing status or success gate.
    void Function(String summary)? onDiagnostics,
  }) async {
    final _OtaUploadMetrics metrics = _OtaUploadMetrics(totalBytes);
    final _OtaUploadSignals signals = _OtaUploadSignals(
      _events,
      metrics,
      disconnects: _disconnects,
    );
    await signals.start();
    RandomAccessFile? reader;
    bool beginAttempted = false;
    bool transferCompleted = false;
    bool endMayBeSubmitted = false;
    OtaUploadPendingVerification pendingVerification(String message) {
      metrics.outcome = 'pending_verification';
      try {
        onStatus?.call(message);
      } catch (_) {
        // A retired presentation consumer cannot discard uncertain End proof.
      }
      return OtaUploadPendingVerification(
        message,
        connectionToken: connectionToken,
      );
    }

    try {
      beginAttempted = true;
      final bool beginSent = await _sendControl(<String, Object>{
        'cmd': 'ota_begin',
        'size': totalBytes,
        'chunk': options.chunkBytes,
        'burst': options.reliableBurst,
        'window': options.maximumWindowBytes,
      });
      metrics.beginControlUs = metrics.now;
      if (!beginSent) {
        onStatus?.call('Failed to send OTA start request.');
        return const OtaUploadFailed('Failed to send OTA start request.');
      }

      final OtaStatusEvent? begin = await signals.waitForBegin(
        options.beginTimeout,
      );
      final OtaStatusEvent? beginTerminal = signals.terminal;
      if (beginTerminal != null) {
        final String message = _beginTerminalMessage(beginTerminal);
        onStatus?.call(message);
        return OtaUploadFailed(message, detail: beginTerminal.detail);
      }
      if (signals.transportDisconnected) {
        const String message =
            'The board disconnected before the update could start. Reconnect it and try again.';
        onStatus?.call(message);
        return const OtaUploadFailed(message);
      }
      if (begin == null) {
        onStatus?.call('Board did not acknowledge OTA start.');
        return const OtaUploadFailed('Board did not acknowledge OTA start.');
      }

      metrics.beginUs = metrics.now;
      metrics.streamStartedUs = metrics.now;
      metrics.phase = 'stream';
      final bool modern = _asInt(begin.payload['proto']) >= 2;
      final int chunkBytes = _boundedChunkBytes(begin);
      final int windowBytes = _boundedWindowBytes(begin, modern, chunkBytes);
      final int burst = _boundedBurst(begin);
      metrics.mode = modern ? 'negotiated' : 'compact';
      metrics.chunkBytes = chunkBytes;
      metrics.windowBytes = windowBytes;
      metrics.burst = modern ? burst : 1;
      onStatus?.call('Board accepted the update.');

      reader = await firmwareFile.open();
      // Read one bounded receive window at a time. Each batch has its own
      // storage so a sender retaining a chunk never sees a later read replace it.
      final int fileReadBatchBytes = (windowBytes ~/ chunkBytes) * chunkBytes;
      Uint8List readBatch = Uint8List(0);
      int readOffset = 0;
      int sentBytes = 0;
      int crc32 = 0xFFFFFFFF;
      int burstIndex = 0;
      while (sentBytes < totalBytes) {
        final int readBytes = (totalBytes - sentBytes < chunkBytes)
            ? totalBytes - sentBytes
            : chunkBytes;
        final flush = flushPendingChunks;
        if (flush != null &&
            !signals.transportDisconnected &&
            signals.terminal == null &&
            sentBytes + readBytes - signals.rxBytes > windowBytes) {
          // Buffered chunks already count against sent-rx. Hand them to the
          // native owner before waiting for credit they may need to release.
          await flush();
          burstIndex = 0;
        }
        await signals.waitForCapacity(
          sentBytes: sentBytes,
          nextBytes: readBytes,
          windowBytes: windowBytes,
          timeout: options.receiveTimeout,
        );
        if (signals.transportDisconnected) {
          throw StateError('board disconnected during upload');
        }
        if (signals.terminal != null) {
          throw StateError('board reported OTA ${signals.terminal!.state}');
        }
        if (sentBytes + readBytes - signals.rxBytes > windowBytes) {
          throw TimeoutException('board receive window stopped advancing');
        }

        if (readOffset == readBatch.length) {
          final int remaining = totalBytes - sentBytes;
          final int batchBytes = remaining < fileReadBatchBytes
              ? remaining
              : fileReadBatchBytes;
          final int readStartedUs = metrics.now;
          try {
            metrics.readCount += 1;
            readBatch = await reader.read(batchBytes);
          } finally {
            metrics.readUs += metrics.now - readStartedUs;
          }
          if (readBatch.length != batchBytes) {
            throw StateError('firmware file ended before $totalBytes bytes');
          }
          readOffset = 0;
        }
        final chunk = Uint8List.sublistView(
          readBatch,
          readOffset,
          readOffset + readBytes,
        );
        readOffset += readBytes;
        burstIndex += 1;
        final bool reliable =
            !modern ||
            burstIndex >= burst ||
            sentBytes + readBytes == totalBytes;
        final int writeStartedUs = metrics.now;
        try {
          await _writeChunk(chunk, reliable: reliable);
        } finally {
          (reliable ? metrics.responseWrites : metrics.noResponseWrites).add(
            metrics.now - writeStartedUs,
          );
        }
        if (reliable) {
          burstIndex = 0;
        }

        sentBytes += chunk.length;
        metrics.sentBytes = sentBytes;
        final int crcStartedUs = metrics.now;
        crc32 = _updateCrc32(crc32, chunk);
        metrics.crcUs += metrics.now - crcStartedUs;
        onProgress?.call(sentBytes / totalBytes);
      }

      final int finalRxStartedUs = metrics.now;
      try {
        await signals.waitForRx(totalBytes, options.receiveTimeout);
      } finally {
        metrics.finalRxUs = metrics.now - finalRxStartedUs;
      }
      if (signals.transportDisconnected) {
        throw StateError('board disconnected before finalization');
      }
      if (signals.terminal != null) {
        throw StateError('board reported OTA ${signals.terminal!.state}');
      }
      if (signals.rxBytes < totalBytes) {
        throw TimeoutException('board did not acknowledge the final byte');
      }

      try {
        final prepare = beforeEnd;
        if (prepare == null) {
          throw StateError('missing durable update intent owner');
        }
        await prepare();
      } catch (_) {
        const message =
            'Could not preserve update verification intent. Finalization was not submitted.';
        onStatus?.call(message);
        return const OtaUploadFailed(message);
      }
      if (signals.transportDisconnected || signals.terminal != null) {
        throw StateError('board connection changed before finalization');
      }
      metrics.streamUs = metrics.now - metrics.streamStartedUs!;
      metrics.phase = 'end';
      metrics.endStartedUs = metrics.now;
      final String finalCrc = _formatCrc32(crc32 ^ 0xFFFFFFFF);
      signals.armForRebootDisconnect();
      // A false/throwing native submission cannot establish that no bytes left.
      endMayBeSubmitted = true;
      final bool endSent = await _sendControl(<String, Object>{
        'cmd': 'ota_end',
        'size': totalBytes,
        'crc32': finalCrc,
      });
      if (!endSent) {
        return pendingVerification(
          'Finalization may have reached the board; verification is pending.',
        );
      }

      metrics.endUs = metrics.now - metrics.endStartedUs!;
      metrics.phase = 'terminal';
      metrics.terminalStartedUs = metrics.now;
      final OtaStatusEvent? terminal = await signals.waitForTerminal(
        options.terminalTimeout,
      );
      if (terminal == null && signals.rebootDisconnectObserved) {
        transferCompleted = true;
        onProgress?.call(1);
        onStatus?.call(
          'Update sent. Board disconnected; verification is pending.',
        );
        metrics.outcome = 'pending_verification';
        return OtaUploadPendingVerification(
          'Board disconnected after the update was sent; verification is pending.',
          connectionToken: connectionToken,
        );
      }
      if (terminal == null) {
        return pendingVerification(
          'Finalization has not been confirmed; verification is pending.',
        );
      }
      if (terminal.kind != OtaStatusKind.done) {
        // Activation can precede a failed readback or status. Only correlated
        // SDK evidence can resolve that outcome after End may be submitted.
        return pendingVerification(
          'The board reported a finalization problem; verification is pending.',
        );
      }
      onProgress?.call(1);
      onStatus?.call(
        'Board accepted finalization. Waiting to verify after reboot.',
      );
      transferCompleted = true;
      metrics.outcome = 'finalized';
      return OtaUploadFinalized(
        'Board accepted finalization; post-reboot verification is required.',
        connectionToken: connectionToken,
      );
    } on TimeoutException {
      if (endMayBeSubmitted) {
        return pendingVerification(
          'Finalization timed out; verification is pending.',
        );
      }
      const String message = 'OTA timed out before finalization completed.';
      onStatus?.call(message);
      return const OtaUploadFailed(message);
    } on StateError {
      if (endMayBeSubmitted) {
        return pendingVerification(
          'The update connection ended during finalization; verification is pending.',
        );
      }
      const String message =
          'OTA connection ended before finalization completed.';
      onStatus?.call(message);
      return const OtaUploadFailed(message);
    } on FileSystemException {
      if (endMayBeSubmitted) {
        return pendingVerification(
          'Finalization could not be confirmed; verification is pending.',
        );
      }
      const String message = 'Could not read the firmware package.';
      onStatus?.call(message);
      return const OtaUploadFailed(message);
    } catch (_) {
      if (endMayBeSubmitted) {
        return pendingVerification(
          'Finalization could not be confirmed; verification is pending.',
        );
      }
      const message =
          'The update could not be sent. Reconnect the board and try again.';
      onStatus?.call(message);
      return const OtaUploadFailed(message);
    } finally {
      // Stop transfer timing before close/abort cleanup or diagnostic formatting.
      metrics.stop();
      final String? summary = onDiagnostics == null
          ? null
          : metrics.summary(rxBytes: signals.rxBytes);
      try {
        try {
          await reader?.close();
        } catch (_) {
          // Closing the local file cannot change the board's End outcome.
        }
        if (beginAttempted && !transferCompleted && !endMayBeSubmitted) {
          try {
            await _sendControl(<String, Object>{'cmd': 'ota_abort'});
          } catch (_) {}
        }
        await signals.dispose();
      } finally {
        try {
          if (summary != null) onDiagnostics?.call(summary);
        } catch (_) {
          // Diagnostic consumers cannot change transfer/cleanup outcomes.
        }
      }
    }
  }

  String _beginTerminalMessage(OtaStatusEvent terminal) {
    if (terminal.kind == OtaStatusKind.aborted) {
      return 'The board canceled the update. Try again.';
    }
    if (terminal.kind != OtaStatusKind.error) {
      return 'The board returned an unexpected update result. Reconnect it and try again.';
    }
    return switch (terminal.detail) {
      'busy_digest' =>
        'The board is checking its installed software. Wait a moment and try again.',
      'busy_store_clear' ||
      'busy_test' => 'The board is busy. Wait a moment and try again.',
      'reboot_pending' ||
      'reboot_wait' => 'The board is restarting. Reconnect it, then try again.',
      'image_too_large' || 'image_large' =>
        'This update does not fit on this board. Contact support for help.',
      _ => 'The board could not start the update. Contact support for help.',
    };
  }

  int _boundedChunkBytes(OtaStatusEvent begin) {
    final int advertised = _asInt(begin.payload['chunk']);
    final int preferred = advertised > 0 ? advertised : options.chunkBytes;
    return preferred.clamp(1, options.chunkBytes).toInt();
  }

  int _boundedWindowBytes(OtaStatusEvent begin, bool modern, int chunkBytes) {
    final int advertised = _asInt(begin.payload['window']);
    final int preferred = modern && advertised > 0
        ? advertised
        : options.compactStatusWindowBytes;
    return preferred.clamp(chunkBytes, options.maximumWindowBytes).toInt();
  }

  int _boundedBurst(OtaStatusEvent begin) {
    final int advertised = _asInt(begin.payload['burst']);
    final int preferred = advertised > 0 ? advertised : options.reliableBurst;
    return preferred.clamp(1, options.reliableBurst).toInt();
  }

  static int _asInt(Object? value) {
    if (value is num) {
      return value.toInt();
    }
    return int.tryParse(value?.toString() ?? '') ?? 0;
  }

  static int _updateCrc32(int current, List<int> bytes) {
    int crc = current & 0xFFFFFFFF;
    for (final int byte in bytes) {
      crc ^= byte & 0xFF;
      for (int bit = 0; bit < 8; ++bit) {
        final bool lowBit = (crc & 1) != 0;
        crc = crc >>> 1;
        if (lowBit) {
          crc ^= 0xEDB88320;
        }
      }
      crc &= 0xFFFFFFFF;
    }
    return crc;
  }

  static String _formatCrc32(int crc) {
    return (crc & 0xFFFFFFFF).toRadixString(16).padLeft(8, '0');
  }
}

class _OtaUploadSignals {
  _OtaUploadSignals(this._events, this._metrics, {Stream<void>? disconnects})
    : _disconnects = disconnects;

  final Stream<OtaStatusEvent> _events;
  final _OtaUploadMetrics _metrics;
  final Stream<void>? _disconnects;
  final StreamController<void> _changes = StreamController<void>.broadcast(
    sync: true,
  );
  StreamSubscription<OtaStatusEvent>? _subscription;
  StreamSubscription<void>? _disconnectSubscription;
  OtaStatusEvent? _begin;
  OtaStatusEvent? _terminal;
  int rxBytes = 0;
  bool transportDisconnected = false;
  bool rebootDisconnectObserved = false;
  bool _rebootDisconnectArmed = false;

  OtaStatusEvent? get terminal => _terminal;

  Future<void> start() async {
    _subscription = _events.listen(
      (OtaStatusEvent event) {
        if (event.kind == OtaStatusKind.begin) {
          _begin = event;
          rxBytes = event.rxBytes ?? 0;
          _metrics.lastProgressUs = _metrics.now;
        } else if (event.rxBytes != null && event.rxBytes! > rxBytes) {
          rxBytes = event.rxBytes!;
        }
        if (event.kind == OtaStatusKind.progress && event.rxBytes != null) {
          final int now = _metrics.now;
          final int? previous = _metrics.lastProgressUs;
          if (previous != null) {
            _metrics.progressGaps.add(now - previous);
          }
          _metrics.lastProgressUs = now;
        }
        if (event.isTerminal) {
          _terminal = event;
          if (event.kind == OtaStatusKind.done) {
            _metrics.firmwareWriteBytes = event.firmwareWriteBytes;
            _metrics.firmwareWriteMs = event.firmwareWriteMilliseconds;
          }
        }
        _notifyChanged();
      },
      onError: (Object _) {
        transportDisconnected = true;
        _notifyChanged();
      },
      onDone: () {
        transportDisconnected = true;
        _notifyChanged();
      },
    );
    _disconnectSubscription = _disconnects?.listen((_) {
      if (_rebootDisconnectArmed) {
        rebootDisconnectObserved = true;
      } else {
        transportDisconnected = true;
      }
      _notifyChanged();
    });
  }

  void armForRebootDisconnect() {
    _rebootDisconnectArmed = true;
  }

  Future<OtaStatusEvent?> waitForBegin(Duration timeout) async {
    await _waitUntil(
      () => _begin != null || _terminal != null || transportDisconnected,
      timeout,
    );
    return _begin;
  }

  Future<OtaStatusEvent?> waitForTerminal(Duration timeout) async {
    await _waitUntil(
      () =>
          _terminal != null ||
          rebootDisconnectObserved ||
          transportDisconnected,
      timeout,
    );
    return _terminal;
  }

  Future<void> waitForRx(int target, Duration timeout) {
    return _waitUntil(
      () => rxBytes >= target || _terminal != null || transportDisconnected,
      timeout,
    );
  }

  Future<void> waitForCapacity({
    required int sentBytes,
    required int nextBytes,
    required int windowBytes,
    required Duration timeout,
  }) {
    return _waitUntil(
      () =>
          sentBytes + nextBytes - rxBytes <= windowBytes ||
          _terminal != null ||
          transportDisconnected,
      timeout,
      waitingTime: _metrics.creditWaits,
    );
  }

  Future<void> _waitUntil(
    bool Function() ready,
    Duration timeout, {
    _OtaTiming? waitingTime,
  }) async {
    if (ready()) {
      return;
    }
    final int startedUs = _metrics.now;
    try {
      await _changes.stream.firstWhere((_) => ready()).timeout(timeout);
    } on TimeoutException {
      return;
    } finally {
      waitingTime?.add(_metrics.now - startedUs);
    }
  }

  void _notifyChanged() {
    if (!_changes.isClosed) {
      _changes.add(null);
    }
  }

  Future<void> dispose() async {
    try {
      await _subscription?.cancel();
    } catch (_) {
      // A retired upstream owner must not override the transfer result.
    }
    try {
      await _disconnectSubscription?.cancel();
    } catch (_) {
      // Still release the local change owner if native retirement throws.
    }
    await _changes.close();
  }
}

// Timings use one monotonic clock. Chunk-write spans include sender retries;
// stream time includes file reads, CRC, callbacks and receive-window waits.
// This measures upload only, not download or post-reboot verification.
class _OtaUploadMetrics {
  _OtaUploadMetrics(this.totalBytes);

  final int totalBytes;
  final Stopwatch _clock = Stopwatch()..start();
  final _OtaTiming responseWrites = _OtaTiming();
  final _OtaTiming noResponseWrites = _OtaTiming();
  final _OtaTiming creditWaits = _OtaTiming();
  final _OtaTiming progressGaps = _OtaTiming();
  String outcome = 'failed';
  String phase = 'begin';
  String mode = 'unknown';
  int chunkBytes = 0, windowBytes = 0, burst = 0, sentBytes = 0;
  int beginUs = 0, streamUs = 0, finalRxUs = 0, endUs = 0, terminalUs = 0;
  int beginControlUs = 0;
  int readCount = 0, readUs = 0, crcUs = 0;
  int? firmwareWriteBytes, firmwareWriteMs;
  int? streamStartedUs, endStartedUs, terminalStartedUs, lastProgressUs;

  int get now => _clock.elapsedMicroseconds;

  void stop() {
    _clock.stop();
    if (phase == 'begin') beginUs = now;
    if (phase == 'stream') streamUs = now - streamStartedUs!;
    if (phase == 'end') endUs = now - endStartedUs!;
    if (phase == 'terminal') terminalUs = now - terminalStartedUs!;
  }

  String summary({required int rxBytes}) =>
      'OTA upload metrics: outcome=$outcome phase=$phase mode=$mode '
      'chunk=$chunkBytes window=$windowBytes burst=$burst '
      'total_bytes=$totalBytes sent_bytes=$sentBytes rx_bytes=$rxBytes '
      'elapsed_ms=${_ms(now)} begin_ms=${_ms(beginUs)} '
      'begin_control_ms=${_ms(beginControlUs)} '
      'begin_status_ms=${_ms(beginUs - beginControlUs)} '
      'stream_ms=${_ms(streamUs)} final_rx_ms=${_ms(finalRxUs)} '
      'end_ms=${_ms(endUs)} terminal_ms=${_ms(terminalUs)} '
      'read_count=$readCount read_ms=${_ms(readUs)} crc_ms=${_ms(crcUs)} '
      'chunk_rsp_count=${responseWrites.count} '
      'chunk_rsp_ms=${_ms(responseWrites.totalUs)} '
      'chunk_rsp_max_ms=${_ms(responseWrites.maxUs)} '
      'chunk_no_rsp_count=${noResponseWrites.count} '
      'chunk_no_rsp_ms=${_ms(noResponseWrites.totalUs)} '
      'chunk_no_rsp_max_ms=${_ms(noResponseWrites.maxUs)} '
      'credit_wait_count=${creditWaits.count} '
      'credit_wait_ms=${_ms(creditWaits.totalUs)} '
      'credit_wait_max_ms=${_ms(creditWaits.maxUs)} '
      'progress_count=${progressGaps.count} '
      'progress_gap_max_ms=${_ms(progressGaps.maxUs)}'
      '${firmwareWriteBytes == null ? '' : ' fw_write_bytes=$firmwareWriteBytes'}'
      '${firmwareWriteMs == null ? '' : ' fw_write_ms=$firmwareWriteMs'}';

  static String _ms(int microseconds) =>
      (microseconds / 1000).toStringAsFixed(3);
}

class _OtaTiming {
  int count = 0, totalUs = 0, maxUs = 0;

  void add(int elapsedUs) {
    count += 1;
    totalUs += elapsedUs;
    if (elapsedUs > maxUs) maxUs = elapsedUs;
  }
}
