import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import '../../models/firmware_manifest.dart';
import '../../models/litch_device_capabilities.dart';
import '../app_trace_recorder.dart';
import '../litch_ble_service.dart';
import 'litch_firmware_uploader.dart';
import 'ota_post_reboot_verifier.dart';
import 'ota_notify_benchmark.dart';
import 'ota_ram_benchmark.dart';

enum OtaBenchmarkPhase {
  preparing,
  measuring,
  uploading,
  reconnecting,
  verifying,
  ramComplete,
  updateComplete,
  failed,
}

@immutable
class OtaBenchmarkProgress {
  const OtaBenchmarkProgress(
    this.phase, {
    this.receivedBytes = 0,
    this.totalBytes = OtaRamBenchmark.totalBytes,
  });

  final OtaBenchmarkPhase phase;
  final int receivedBytes;
  final int totalBytes;

  bool get isRunning => switch (phase) {
    OtaBenchmarkPhase.ramComplete ||
    OtaBenchmarkPhase.updateComplete ||
    OtaBenchmarkPhase.failed => false,
    _ => true,
  };
}

/// Opt-in physical benchmark. Before installation the host must verify the
/// declared iPhone UDID or Android physical serial (not an ADB transport alias).
/// This Dart process cannot attest either install target.
class OtaBenchmarkRunner {
  static const enabled = bool.fromEnvironment('LITCH_OTA_BENCHMARK');
  static const mode = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_MODE',
    defaultValue: 'flash',
  );
  static const _iosUdid = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_IOS_UDID',
  );
  static const _androidSerial = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_ANDROID_SERIAL',
  );
  static const _boardIdentity = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_BOARD_ID',
  );
  static const _bleDeviceId = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_BLE_DEVICE_ID',
  );
  static const _manifestAsset = String.fromEnvironment(
    'LITCH_OTA_BENCHMARK_MANIFEST_ASSET',
  );
  static const _notifyPacketCount = int.fromEnvironment(
    'LITCH_NOTIFY_BENCHMARK_COUNT',
    defaultValue: 200,
  );
  static bool _started = false;

  static bool get _installTargetConfigured =>
      (Platform.isIOS &&
          RegExp(r'^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}$').hasMatch(_iosUdid)) ||
      (Platform.isAndroid &&
          RegExp(r'^[A-Za-z0-9]{6,64}$').hasMatch(_androidSerial));

  static bool get _configured =>
      kReleaseMode &&
      _installTargetConfigured &&
      _boardIdentity.isNotEmpty &&
      _bleDeviceId.isNotEmpty &&
      (mode == 'ram_reference' ||
          mode == 'ram_reverse' ||
          (mode == 'notify_receive' &&
              _notifyPacketCount >= 1 &&
              _notifyPacketCount <= 10000) ||
          ((mode == 'flash' || mode == 'ram_then_flash') &&
              _manifestAsset.isNotEmpty));

  /// Read-only guard before the opt-in hook enters the normal Firmware page.
  static bool targetReady(LitchBleService service) {
    if (!enabled || _started || !_configured) return false;
    final deviceId = service.connectedId;
    if (deviceId == null) return false;
    try {
      _requireTarget(service, deviceId);
      return service.pendingOtaVerification == null;
    } on StateError {
      return false;
    }
  }

  static Future<void> run(
    LitchBleService service, {
    AppTraceSink? appTrace,
    ValueChanged<OtaBenchmarkProgress>? onProgress,
  }) async {
    if (!enabled || _started) return;
    _started = true;
    final clock = Stopwatch()..start();
    final result = <String, Object?>{
      'ok': false,
      'install_platform': Platform.operatingSystem,
      if (Platform.isIOS) 'ios_install_target': _iosUdid,
      if (Platform.isAndroid) 'android_install_target': _androidSerial,
      'expected_board': _boardIdentity,
      'stage': 'guard',
      'mode': mode,
    };
    Directory? temporary;
    StreamSubscription<List<String>>? logs;
    StreamSubscription<ConnectionStateUpdate>? connection;
    final uploader = <String, Object?>{};
    final object = <String, Object?>{};
    var ramReceivedBytes = 0;
    var measurementTotalBytes = OtaRamBenchmark.totalBytes;
    var reverseMilestone = -1;
    try {
      onProgress?.call(const OtaBenchmarkProgress(OtaBenchmarkPhase.preparing));
      if (!_configured) {
        result['error_code'] = !kReleaseMode
            ? 'not_release'
            : !Platform.isIOS && !Platform.isAndroid
            ? 'unsupported_platform'
            : 'missing_targets';
        throw StateError(
          'Release iOS or Android build and explicit targets are required.',
        );
      }
      result['stage'] = 'waiting_for_target';
      _emit(result, appTrace);
      if (!service.isConnected && _bleDeviceId.isNotEmpty) {
        // A foreground route may already own the connection attempt. Wait on
        // the same readiness owner, then enforce the exact identity below.
        await service.connect(_bleDeviceId);
      }
      await _waitForReady(service);
      final deviceId = service.connectedId!;
      _requireTarget(service, deviceId);
      final generation = service.connectionSessionToken;
      result.addAll({
        'device_id': deviceId,
        'source_generation': generation,
        'stage': 'preparation',
      });
      await service.restorePendingOtaVerification();
      if (service.pendingOtaVerification != null) {
        throw StateError('An earlier update still requires verification.');
      }

      if (mode == 'notify_receive') {
        result['stage'] = 'notify_receive';
        final measurement = await OtaNotifyBenchmark.run(
          service,
          packetCount: _notifyPacketCount,
          onProgress: (receivedBytes, totalBytes) {
            ramReceivedBytes = receivedBytes;
            measurementTotalBytes = totalBytes;
            onProgress?.call(
              OtaBenchmarkProgress(
                OtaBenchmarkPhase.measuring,
                receivedBytes: receivedBytes,
                totalBytes: totalBytes,
              ),
            );
          },
        );
        ramReceivedBytes = measurement['rx_bytes']! as int;
        measurementTotalBytes =
            measurement['bytes'] as int? ?? measurementTotalBytes;
        result.addAll({
          'notify_receive': measurement,
          'bytes': measurement['bytes'],
          'rx_bytes': measurement['rx_bytes'],
          'stage': 'complete',
          if (measurement['ok'] != true) ...{
            'error_kind': measurement['error_kind'],
            'error_code': 'notify_receive_failed',
          },
        });
        if (measurement['link'] case final Map<String, Object?> link) {
          appTrace?.record(
            'ota.notify_link',
            fields: {'generation': generation, ...link},
          );
        }
        final runs = measurement['runs']! as List<Map<String, Object?>>;
        for (final run in runs) {
          appTrace?.record(
            'ota.notify_receive',
            fields: {
              'generation': generation,
              for (final key in const [
                'ok',
                'run',
                'requested_size',
                'packet_size',
                'count',
                'rx_packets',
                'rx_bytes',
                'observed_bytes',
                'first_us',
                'last_us',
                'missing_packets',
                'corrupt_packets',
                'duplicate_packets',
                'gap_packets',
                'old_run_packets',
              ])
                key: run[key],
            },
          );
          final sdk = run['sdk'] as Map<String, int>?;
          appTrace?.record(
            'ota.notify_sdk',
            fields: {
              'run': run['run'],
              'ok': run['ok'],
              if (sdk != null)
                for (final key in const [
                  'size',
                  'count',
                  'ok',
                  'fail',
                  'elapsed_ms',
                  'rate_Bps',
                ])
                  'sdk_$key': sdk[key],
            },
          );
        }
        if (measurement['ok'] != true) {
          final failedRun = runs.isEmpty ? null : runs.last;
          appTrace?.record(
            'ota.notify_error',
            fields: {
              'generation': generation,
              if (failedRun != null) 'run': failedRun['run'],
              'rx_bytes': measurement['rx_bytes'],
              'error_kind': measurement['error_kind'],
              'reason': measurement['error_code'],
              if (failedRun?['packet_error'] case final String packetError)
                'packet_reason': packetError,
            },
          );
        }
        if (measurement['ok'] == true) {
          _requireTarget(service, deviceId, generation: generation);
          result['ok'] = true;
        }
        return;
      }

      if (mode == 'ram_reference' ||
          mode == 'ram_reverse' ||
          mode == 'ram_then_flash') {
        final reverse = mode == 'ram_reverse';
        final referenceName = reverse
            ? 'ram_reverse_reference'
            : 'ram_reference';
        result['stage'] = referenceName;
        onProgress?.call(
          const OtaBenchmarkProgress(OtaBenchmarkPhase.measuring),
        );
        final reference = await OtaRamBenchmark.run(
          service,
          direction: reverse
              ? OtaRamBenchmarkDirection.reverse
              : OtaRamBenchmarkDirection.forward,
          onProgress: (receivedBytes) {
            ramReceivedBytes = receivedBytes;
            final milestone = receivedBytes * 4 ~/ OtaRamBenchmark.totalBytes;
            if (reverse && milestone != reverseMilestone) {
              reverseMilestone = milestone;
              appTrace?.record(
                'ota.ram_reverse_progress',
                fields: {'rx_bytes': receivedBytes},
              );
            }
            onProgress?.call(
              OtaBenchmarkProgress(
                OtaBenchmarkPhase.measuring,
                receivedBytes: receivedBytes,
              ),
            );
          },
        );
        _requireTarget(service, deviceId, generation: generation);
        result[referenceName] = reference;
        appTrace?.record(
          'ota.benchmark_link',
          fields: reference['link']! as Map<String, Object?>,
        );
        appTrace?.record(
          'ota.$referenceName',
          fields: {'generation': generation, ...reference}..remove('link'),
        );
        if (mode == 'ram_reference' || reverse) {
          result.addAll({
            'bytes': reference['bytes'],
            'goodput_Bps': reference['goodput_bytes_sec'],
            'finalize_ms': reference['finalize_ms'],
            'ok': true,
            'stage': 'complete',
          });
          return;
        }
      }

      final prepared = await _loadManifest();
      final manifest = prepared.manifest;
      final bytes = await rootBundle.load(prepared.asset);
      if (bytes.lengthInBytes != manifest.artifact.sizeBytes) {
        throw StateError(
          'Bundled firmware length does not match its manifest.',
        );
      }
      temporary = await Directory.systemTemp.createTemp('litch-ota-benchmark-');
      final file = File('${temporary.path}/firmware.bin');
      await file.writeAsBytes(
        bytes.buffer.asUint8List(bytes.offsetInBytes, bytes.lengthInBytes),
        flush: true,
      );
      _requireTarget(service, deviceId, generation: generation);
      result.addAll({
        'version': manifest.version,
        'artifact_sha256': manifest.artifact.sha256,
        'image_sha256': manifest.artifact.imageSha256,
        'bytes': manifest.artifact.sizeBytes,
        'preparation_ms': clock.elapsedMilliseconds,
      });

      // Consume only new lines; console snapshots can retain older attempts.
      final baseline = service.consoleLinesSnapshot;
      String? lastLine = baseline.isEmpty ? null : baseline.last;
      void captureLines(List<String> lines) {
        final start = lastLine == null ? 0 : lines.lastIndexOf(lastLine!) + 1;
        for (final line in lines.skip(start)) {
          _capture(line, 'OTA upload metrics:', uploader);
          _capture(line, 'OTA shared Object:', object);
        }
        if (lines.isNotEmpty) lastLine = lines.last;
      }

      logs = service.consoleStream.listen(captureLines);
      final disconnected = Completer<void>();
      connection = service.connectionStream.listen((event) {
        if (event.deviceId == deviceId &&
            event.connectionState == DeviceConnectionState.disconnected &&
            !disconnected.isCompleted) {
          disconnected.complete();
        }
      });
      result['stage'] = 'upload';
      onProgress?.call(const OtaBenchmarkProgress(OtaBenchmarkPhase.uploading));
      final uploadClock = Stopwatch()..start();
      // Production owns validation, source admission, the durable beforeEnd
      // proof, final receive acknowledgement and every transport deadline.
      final upload = await service.uploadFirmwareFile(
        deviceId: deviceId,
        firmwareFile: file,
        manifest: manifest,
      );
      captureLines(service.consoleLinesSnapshot);
      result.addAll({
        'upload_ms': uploadClock.elapsedMilliseconds,
        'upload_result': upload.runtimeType.toString(),
        'upload_message': upload.message,
      });
      if (upload is OtaUploadFailed) return;
      final target = service.pendingOtaVerification;
      if (target == null ||
          target.deviceId != deviceId ||
          target.version != manifest.version ||
          target.artifactSha256 != manifest.artifact.sha256 ||
          target.imageSha256 != manifest.artifact.imageSha256 ||
          target.attempt?.isValid != true) {
        throw StateError('The exact admitted update target is unavailable.');
      }
      if (mode == 'ram_then_flash') {
        result['paired_generation_match'] =
            target.uploadConnectionToken == generation;
      }

      result['stage'] = 'reconnect';
      onProgress?.call(
        const OtaBenchmarkProgress(OtaBenchmarkPhase.reconnecting),
      );
      final reconnectClock = Stopwatch()..start();
      await disconnected.future.timeout(const Duration(seconds: 20));
      if (service.isConnected && service.connectedId != deviceId) {
        throw StateError('Another board is connected; pending proof retained.');
      }
      if (!service.isConnected && !await service.connect(deviceId)) {
        throw StateError('The original board did not reconnect.');
      }
      await _waitForReady(service);
      _requireTarget(service, deviceId);
      result['reconnect_ms'] = reconnectClock.elapsedMilliseconds;
      result['stage'] = 'verification';
      onProgress?.call(const OtaBenchmarkProgress(OtaBenchmarkPhase.verifying));
      final verificationClock = Stopwatch()..start();
      final verification = await service.verifyFirmwareAfterOta(
        deviceId: deviceId,
        uploadConnectionToken: target.uploadConnectionToken,
        expectedVersion: target.version,
        expectedImageSha256: target.imageSha256,
      );
      _requireTarget(service, deviceId);
      result.addAll({
        'verification_ms': verificationClock.elapsedMilliseconds,
        'verification_result': verification.runtimeType.toString(),
        'verification_message': verification.message,
        'verified_generation': service.connectionSessionToken,
      });
      if (verification is OtaPostRebootVerified) {
        if (!await service.clearPendingOtaVerification(target)) {
          throw StateError(
            'The verified target changed before durable cleanup.',
          );
        }
        if (result['paired_generation_match'] == false) {
          throw StateError('RAM and flash used different connection sessions.');
        }
        result['ok'] = true;
      } else if (verification is OtaPostRebootDifferentImage) {
        await service.resolveFailedOtaVerification(target, verification);
      }
      result['stage'] = 'complete';
    } catch (error) {
      result['error_kind'] = switch (error) {
        StateError() => 'StateError',
        TimeoutException() => 'TimeoutException',
        FormatException() => 'FormatException',
        FileSystemException() => 'FileSystemException',
        PlatformException() => 'PlatformException',
        FlutterError() => 'FlutterError',
        _ => 'other',
      };
      result['error_code'] ??= _errorCode(error);
      result['error'] = error.toString();
    } finally {
      await connection?.cancel();
      await logs?.cancel();
      if (temporary != null) {
        try {
          await temporary.delete(recursive: true);
        } catch (error) {
          result['cleanup_error'] = error.toString();
        }
      }
      final streamMs = uploader['stream_ms'];
      final received = uploader['rx_bytes'];
      final endMs = uploader['end_ms'];
      final terminalMs = uploader['terminal_ms'];
      result.addAll({
        'elapsed_ms': clock.elapsedMilliseconds,
        if (mode == 'ram_reverse') 'rx_bytes': ramReceivedBytes,
        'pending_verification': service.pendingOtaVerification != null,
        'uploader': uploader,
        'object': object,
        if (endMs is num && terminalMs is num)
          'finalize_ms': endMs + terminalMs,
        if (streamMs is num && streamMs > 0 && received is num)
          'goodput_Bps': received * 1000 / streamMs,
      });
      _emit(result, appTrace);
      onProgress?.call(
        OtaBenchmarkProgress(
          result['ok'] != true
              ? OtaBenchmarkPhase.failed
              : mode == 'ram_reference' ||
                    mode == 'ram_reverse' ||
                    mode == 'notify_receive'
              ? OtaBenchmarkPhase.ramComplete
              : OtaBenchmarkPhase.updateComplete,
          receivedBytes: mode == 'notify_receive'
              ? ramReceivedBytes
              : result['ok'] == true &&
                    (mode == 'ram_reference' ||
                        mode == 'ram_reverse' ||
                        mode == 'notify_receive')
              ? measurementTotalBytes
              : 0,
          totalBytes: measurementTotalBytes,
        ),
      );
    }
  }

  static Future<void> _waitForReady(LitchBleService service) async {
    if (!service.productReady) {
      await service.productReadinessStream
          .firstWhere((ready) => ready)
          .timeout(const Duration(seconds: 30));
    }
  }

  static void _requireTarget(
    LitchBleService service,
    String deviceId, {
    int? generation,
  }) {
    if (!service.isConnected ||
        !service.productReady ||
        service.connectedId != deviceId ||
        (generation != null && service.connectionSessionToken != generation)) {
      throw StateError('Connected identity, V3.3 or generation guard failed.');
    }
    if (_bleDeviceId.isNotEmpty && deviceId != _bleDeviceId) {
      throw StateError('Configured BLE identity does not match.');
    }
    if (service.durableBoardIdentity != _boardIdentity) {
      throw StateError('Durable board identity does not match.');
    }
    if (service.deviceCapabilities(deviceId).hardwareRevision !=
        LitchHardwareRevision.v3_3) {
      throw StateError('Connected board is not V3.3.');
    }
  }

  static Future<({FirmwareManifest manifest, String asset})>
  _loadManifest() async {
    final source = await rootBundle.load(_manifestAsset);
    if (source.lengthInBytes > 8192) {
      throw const FormatException('Private benchmark manifest exceeds 8 KiB.');
    }
    final json = jsonDecode(
      utf8.decode(
        source.buffer.asUint8List(source.offsetInBytes, source.lengthInBytes),
      ),
    );
    if (json is! Map<String, dynamic> ||
        json['schema_version'] != 1 ||
        json['target'] != 'esp32s3' ||
        json['chip_id'] != 9 ||
        json['version'] is! String ||
        (json['version'] as String).isEmpty ||
        json['artifact'] is! Map<String, dynamic>) {
      throw const FormatException('Invalid private benchmark manifest.');
    }
    final artifact = json['artifact'] as Map<String, dynamic>;
    final asset = artifact['asset'];
    final size = artifact['size_bytes'];
    final sha = artifact['sha256'];
    final imageSha = artifact['image_sha256'];
    final hash = RegExp(r'^[0-9a-f]{64}$');
    if (asset is! String ||
        !asset.startsWith('assets/ota_benchmark/') ||
        asset.contains('..') ||
        size is! int ||
        size <= 0 ||
        size > FirmwareManifest.maxOfficialEsp32s3FirmwareSizeBytes ||
        sha is! String ||
        !hash.hasMatch(sha) ||
        imageSha is! String ||
        !hash.hasMatch(imageSha)) {
      throw const FormatException('Invalid private benchmark artifact.');
    }
    return (
      asset: asset,
      manifest: FirmwareManifest(
        schemaVersion: 1,
        channel: 'private_benchmark',
        version: json['version'] as String,
        target: 'esp32s3',
        chipId: 9,
        artifact: FirmwareArtifactInfo(
          url: Uri(scheme: 'asset', path: asset),
          sizeBytes: size,
          sha256: sha,
          imageSha256: imageSha,
        ),
        // This local source has no release notes or published catalog URL.
        notes: FirmwareNotesInfo(
          url: Uri.parse('data:text/plain,'),
          sizeBytes: 0,
          sha256:
              'e3b0c44298fc1c149afbf4c8996fb924'
              '27ae41e4649b934ca495991b7852b855',
        ),
      ),
    );
  }

  static void _capture(String line, String prefix, Map<String, Object?> into) {
    final index = line.indexOf(prefix);
    if (index < 0 || line.substring(0, index).contains('[FW]')) return;
    into.clear();
    for (final field
        in line.substring(index + prefix.length).trim().split(' ')) {
      final separator = field.indexOf('=');
      if (separator <= 0) continue;
      final value = field.substring(separator + 1);
      into[field.substring(0, separator)] = num.tryParse(value) ?? value;
    }
  }

  static void _emit(Map<String, Object?> result, AppTraceSink? appTrace) {
    final upload = result['uploader'] as Map<String, Object?>?;
    final object = result['object'] as Map<String, Object?>?;
    // Keep the existing trace's 16-field/512-byte budget. Full precision and
    // nested diagnostics remain on stdout; trace timings use whole milliseconds.
    final fields = <String, Object?>{
      'stage': result['stage'],
      'ok': result['ok'],
      'mode': mode,
      if (result.containsKey('error_kind')) 'error_kind': result['error_kind'],
      if (result.containsKey('error_code')) 'error_code': result['error_code'],
      for (final key in const [
        'bytes',
        'rx_bytes',
        'upload_ms',
        'finalize_ms',
        'reconnect_ms',
        'verification_ms',
      ])
        if (result.containsKey(key)) key: result[key],
      if (upload?['stream_ms'] != null) 'stream_ms': upload!['stream_ms'],
      if (upload?['credit_wait_ms'] != null)
        'credit_wait_ms': upload!['credit_wait_ms'],
      if (object?['native_ms'] != null) 'native_ms': object!['native_ms'],
      if (object?['grant_wait_ms'] != null)
        'grant_wait_ms': object!['grant_wait_ms'],
      if (result.containsKey('pending_verification'))
        'pending': result['pending_verification'],
      if (result.containsKey('paired_generation_match'))
        'paired_generation_match': result['paired_generation_match'],
      if (result.containsKey('goodput_Bps'))
        'goodput_bytes_sec': result['goodput_Bps'],
    };
    for (final key in fields.keys.toList()) {
      final value = fields[key];
      if (value is double && value.isFinite) fields[key] = value.round();
    }
    appTrace?.record('ota.benchmark', fields: fields);
    final writeBytes = upload?['fw_write_bytes'];
    final writeMs = upload?['fw_write_ms'];
    if (writeBytes is num || writeMs is num) {
      appTrace?.record(
        'ota.write_path',
        fields: {
          if (writeBytes is num) 'fw_write_bytes': writeBytes,
          if (writeMs is num) 'fw_write_ms': writeMs,
        },
      );
    }
    debugPrint('LITCH_OTA_BENCHMARK ${jsonEncode(result)}');
  }

  static String _errorCode(Object error) => switch (error) {
    StateError(:final message) => switch (message) {
      'Configured BLE identity does not match.' => 'wrong_native_identity',
      'Durable board identity does not match.' => 'wrong_identity',
      'Connected board is not V3.3.' => 'not_v33',
      'Connected identity, V3.3 or generation guard failed.' =>
        'connection_changed',
      'The exact configured BLE target did not connect.' => 'connect_failed',
      'An earlier update still requires verification.' => 'pending_update',
      'Bundled firmware length does not match its manifest.' =>
        'artifact_length',
      'The exact admitted update target is unavailable.' => 'admission_missing',
      'Another board is connected; pending proof retained.' =>
        'wrong_board_reconnect',
      'The original board did not reconnect.' => 'reconnect_failed',
      'The verified target changed before durable cleanup.' =>
        'pending_changed',
      _ => 'state',
    },
    TimeoutException() => 'deadline',
    FormatException() =>
      mode == 'ram_reference' ||
              mode == 'ram_reverse' ||
              mode == 'notify_receive'
          ? 'ram_protocol'
          : 'manifest',
    FileSystemException() => 'file',
    PlatformException() => 'platform',
    FlutterError() => 'asset',
    _ => 'other',
  };
}
