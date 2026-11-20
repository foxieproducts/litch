import 'dart:convert';
import 'dart:typed_data';
import 'board_health_status.dart';
import 'runtime_health_snapshot.dart';
import 'support_action.dart';

class SupportPublicHealth {
  const SupportPublicHealth({
    required this.request,
    required this.validity,
    required this.healthRevision,
    required this.healthState,
    required this.healthCauses,
    required this.resetReason,
    required this.storageError,
    required this.bootLoadError,
    required this.uptimeMs,
    required this.hardwareMajor,
    required this.hardwareMinor,
    required this.digestAvailability,
    required this.firmwareDigest,
    required this.releaseLabel,
    required this.recoveryMode,
    required this.recoveryCause,
    required this.recoveryState,
    required this.recoveryFailure,
    required this.boots,
    required this.earlyCrashes,
    required this.updater,
    required this.storageAccess,
    required this.priorStage,
    required this.priorStageAtMs,
    required this.priorStageValid,
  });
  final int request,
      validity,
      healthRevision,
      healthState,
      healthCauses,
      resetReason,
      storageError,
      bootLoadError,
      uptimeMs,
      hardwareMajor,
      hardwareMinor,
      digestAvailability;
  final int recoveryMode,
      recoveryCause,
      recoveryState,
      recoveryFailure,
      boots,
      earlyCrashes,
      updater,
      storageAccess,
      priorStage,
      priorStageAtMs;
  final bool priorStageValid;
  String get recoveryReason => switch (recoveryCause) {
    1 => 'earlyCrash',
    2 => 'storage',
    3 || 4 => 'update',
    _ => 'other',
  };
  String get recoveryCauseToken => switch (recoveryCause) {
    0 => 'none',
    1 => 'early_crash',
    2 => 'persisted_state_corrupt',
    3 => 'update_pending',
    4 => 'update_failed',
    5 => 'manual_factory_reset',
    _ => 'unavailable',
  };
  final String? firmwareDigest;
  final String releaseLabel;
  RuntimeHealthResult? get runtimeHealth => validity & 1 == 0
      ? null
      : RuntimeHealthAvailable(
          RuntimeHealthSnapshot(
            revision: healthRevision,
            stateCode: healthState,
            state: RuntimeHealthState.values[healthState],
            causeMask: healthCauses,
          ),
        );
  static SupportPublicHealth decode(Uint8List bytes, int request) {
    if (bytes.length != 106 || bytes[0] != 2) {
      throw const FormatException('Invalid public support snapshot');
    }
    final d = ByteData.sublistView(bytes);
    if (d.getUint32(82, Endian.little) != request ||
        bytes[1] & ~15 != 0 ||
        bytes[4] > 3 ||
        bytes[20] > 3 ||
        bytes[53] > 28 ||
        bytes[86] > 1 ||
        bytes[87] > 5 ||
        bytes[88] > 8 ||
        bytes[89] > 2 ||
        bytes[98] > 3 ||
        bytes[99] > 2 ||
        bytes[105] > 1 ||
        (bytes[105] == 1 && bytes[100] > 18)) {
      throw const FormatException('Invalid public support fields');
    }
    if (d.getUint64(10, Endian.little) < 0) {
      throw const FormatException('Unsupported support uptime');
    }
    final label = bytes.sublist(54, 54 + bytes[53]);
    if (label.any((b) => b < 32 || b > 126)) {
      throw const FormatException('Invalid firmware label');
    }
    return SupportPublicHealth(
      request: request,
      validity: bytes[1],
      healthRevision: d.getUint16(2, Endian.little),
      healthState: bytes[4],
      healthCauses: d.getUint16(5, Endian.little),
      resetReason: bytes[7],
      storageError: bytes[8],
      bootLoadError: bytes[9],
      uptimeMs: d.getUint64(10, Endian.little),
      hardwareMajor: bytes[18],
      hardwareMinor: bytes[19],
      digestAvailability: bytes[20],
      firmwareDigest: bytes[20] == 2
          ? bytes
                .sublist(21, 53)
                .map((b) => b.toRadixString(16).padLeft(2, '0'))
                .join()
          : null,
      releaseLabel: ascii.decode(label),
      recoveryMode: bytes[86],
      recoveryCause: bytes[87],
      recoveryState: bytes[88],
      recoveryFailure: bytes[89],
      boots: d.getUint32(90, Endian.little),
      earlyCrashes: d.getUint32(94, Endian.little),
      updater: bytes[98],
      storageAccess: bytes[99],
      priorStage: bytes[100],
      priorStageAtMs: d.getUint32(101, Endian.little),
      priorStageValid: bytes[105] == 1,
    );
  }
}

class SupportBoardHistory {
  const SupportBoardHistory({
    required this.available,
    required this.boots,
    required this.priorRuntimeMs,
    required this.uptimeMs,
    required this.savedCheckpointMs,
    required this.automaticSleeps,
    required this.manualSleeps,
    required this.otherSleeps,
    required this.saveTotal,
    required this.savesThisBoot,
    required this.errorsThisBoot,
    required this.omitted,
    required this.actions,
    this.appRequestedMs,
    this.appReceivedMs,
    this.bootNonce,
  });
  final bool available;
  final int boots,
      priorRuntimeMs,
      uptimeMs,
      savedCheckpointMs,
      automaticSleeps,
      manualSleeps,
      otherSleeps,
      saveTotal,
      savesThisBoot,
      errorsThisBoot,
      omitted;
  final List<SupportAction> actions;
  final int? appRequestedMs, appReceivedMs, bootNonce;
  static SupportBoardHistory decode(
    Uint8List bytes,
    int request, {
    int? appRequestedMs,
    int? appReceivedMs,
    int? bootNonce,
  }) {
    if (bytes.length < 80 ||
        bytes.length > 480 ||
        bytes[0] != 1 ||
        bytes[1] != 0) {
      throw const FormatException('Invalid support history');
    }
    final d = ByteData.sublistView(bytes);
    if (d.getUint32(2, Endian.little) != request ||
        bytes[6] > 1 ||
        bytes[7] > 20 ||
        bytes.length != 80 + bytes[7] * 20) {
      throw const FormatException('Invalid support history fields');
    }
    // Native Dart integers cannot represent uint64 values above INT64_MAX.
    // Reject an unrepresentable snapshot rather than print negative metrics.
    for (final at in [12, 20, 28, 36, 44, 52, 60]) {
      if (d.getUint64(at, Endian.little) < 0) {
        throw const FormatException('Unsupported support counter');
      }
    }
    final actions = <SupportAction>[];
    for (var i = 0; i < bytes[7]; i++) {
      final p = 80 + i * 20;
      final kind = bytes[p + 16], outcome = bytes[p + 17] & 15;
      if (kind < 1 ||
          kind > SupportActionKind.values.length ||
          outcome > 8 ||
          bytes[p + 17] >> 4 > 2) {
        throw const FormatException('Unknown support action');
      }
      final at = d.getUint64(p, Endian.little);
      final sequence = d.getUint32(p + 8, Endian.little);
      if (at < 0 ||
          at > d.getUint64(28, Endian.little) ||
          sequence == 0 ||
          actions.any((a) => a.sequence == sequence) ||
          (actions.isNotEmpty && at < actions.last.uptimeMs)) {
        throw const FormatException('Invalid support chronology');
      }
      final duration = d.getUint32(p + 12, Endian.little);
      actions.add(
        SupportAction(
          sequence: sequence,
          uptimeMs: at,
          durationMs: duration == 0xffffffff ? null : duration,
          kind: SupportActionKind.values[kind - 1],
          outcome: SupportActionOutcome.values[outcome],
          count: bytes[p + 18],
          secondCount: bytes[p + 19],
        ),
      );
    }
    return SupportBoardHistory(
      available: bytes[6] == 1,
      omitted: d.getUint32(8, Endian.little),
      boots: d.getUint64(12, Endian.little),
      priorRuntimeMs: d.getUint64(20, Endian.little),
      uptimeMs: d.getUint64(28, Endian.little),
      savedCheckpointMs: d.getUint64(36, Endian.little),
      automaticSleeps: d.getUint64(44, Endian.little),
      manualSleeps: d.getUint64(52, Endian.little),
      otherSleeps: d.getUint64(60, Endian.little),
      saveTotal: d.getUint32(68, Endian.little),
      savesThisBoot: d.getUint32(72, Endian.little),
      errorsThisBoot: d.getUint32(76, Endian.little),
      actions: List.unmodifiable(actions),
      appRequestedMs: appRequestedMs,
      appReceivedMs: appReceivedMs,
      bootNonce: bootNonce,
    );
  }
}

SupportIncident decodeSupportIncident(Uint8List bytes, int request) {
  if (bytes.length != 136 || bytes[0] != 1 || bytes[1] != 1) {
    throw const FormatException('Invalid support incident');
  }
  final d = ByteData.sublistView(bytes);
  if (d.getUint32(2, Endian.little) != request || bytes[18] > 1) {
    throw const FormatException('Invalid incident request');
  }
  final fields = <String, Object>{
    'event': 'support_incident',
    'v': 1,
    'rr': d.getUint32(6, Endian.little),
    'boot': d.getUint32(10, Endian.little),
    'wd': d.getUint32(14, Endian.little),
    'prior': bytes[18],
  };
  for (var record = 0; record < 2; record++) {
    final p = 19 + record * 54, prefix = record == 0 ? '' : 'prior_';
    fields['${prefix}up'] = d.getUint32(p, Endian.little);
    for (final (name, at) in [('expected', 4), ('observed', 12)]) {
      // Render exact LE bytes rather than a potentially signed Dart uint64.
      fields['$prefix$name'] = bytes
          .sublist(p + at, p + at + 8)
          .reversed
          .map((b) => b.toRadixString(16).padLeft(2, '0'))
          .join()
          .toUpperCase();
    }
    fields['${prefix}hc'] = d.getUint16(p + 20, Endian.little);
    fields['${prefix}phase'] = bytes[p + 22];
    fields['${prefix}variant'] = bytes[p + 23];
    fields['${prefix}se'] = bytes[p + 24];
    fields['${prefix}flags'] = bytes[p + 25];
    fields[record == 0 ? 'now_stages' : 'stages'] = bytes.sublist(
      p + 26,
      p + 30,
    );
    fields[record == 0 ? 'now_ms' : 'stage_ms'] = [
      for (var i = 0; i < 4; i++) d.getUint32(p + 30 + i * 4, Endian.little),
    ];
    if (record == 1) fields['recent'] = bytes.sublist(p + 46, p + 54);
  }
  fields['saved'] = d.getUint32(127, Endian.little);
  fields['failed'] = d.getUint32(131, Endian.little);
  fields['be'] = bytes[135];
  final result = SupportIncident.tryParse(jsonEncode(fields));
  if (result == null) throw const FormatException('Invalid incident field');
  return result;
}
