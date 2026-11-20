import 'dart:convert';

import '../../models/runtime_health_snapshot.dart';
import 'ota_attempt_proof.dart';

enum OtaStatusKind {
  begin,
  progress,
  done,
  aborted,
  error,
  version,
  probe,
  digest,
  attemptProof,
  attemptProofMalformed,
  runtimeAcceptance,
  runtimeAcceptanceMalformed,
  runtimeHealth,
  runtimeHealthMalformed,
  runtimeHealthRecheckSucceeded,
  runtimeHealthRecheckFailed,
  runtimeHealthRecheckMalformed,
  deviceInfo,
  unknown,
}

class OtaStatusEvent {
  const OtaStatusEvent({
    required this.deviceId,
    required this.generation,
    required this.kind,
    required this.state,
    required this.rawText,
    required this.payload,
    this.detail = '',
    this.progressPercent,
    this.rxBytes,
    this.totalBytes,
  });

  final String deviceId;
  final int generation;
  final OtaStatusKind kind;
  final String state;
  final String detail;
  final int? progressPercent;
  final int? rxBytes;
  final int? totalBytes;
  final String rawText;
  final Map<String, Object?> payload;

  // Optional firmware write-path observations; never completion authority.
  int? get firmwareWriteBytes =>
      kind == OtaStatusKind.done ? _uint32(payload['wb']) : null;
  int? get firmwareWriteMilliseconds =>
      kind == OtaStatusKind.done ? _uint32(payload['wm']) : null;

  bool get isTerminal =>
      kind == OtaStatusKind.done ||
      kind == OtaStatusKind.aborted ||
      kind == OtaStatusKind.error;

  OtaStatusEvent withRxBytes(int value) {
    return OtaStatusEvent(
      deviceId: deviceId,
      generation: generation,
      kind: kind,
      state: state,
      rawText: rawText,
      payload: payload,
      detail: detail,
      progressPercent: progressPercent,
      rxBytes: value,
      totalBytes: totalBytes,
    );
  }

  String get summary {
    final String suffix = detail.isEmpty ? '' : ' ($detail)';
    final bool showCounts =
        (rxBytes != null || totalBytes != null) &&
        kind != OtaStatusKind.version &&
        kind != OtaStatusKind.error;
    final String counts = showCounts
        ? ' (${rxBytes ?? 0}/${totalBytes ?? 0})'
        : '';
    return 'OTA: $state$suffix$counts';
  }

  static OtaStatusEvent? decode(
    List<int> packet, {
    required String deviceId,
    required int generation,
  }) {
    final String rawText = utf8.decode(packet, allowMalformed: true);
    final String text = rawText.trim();
    if (text.isEmpty) {
      return null;
    }

    Object? decoded;
    try {
      decoded = jsonDecode(text);
    } catch (_) {
      return OtaStatusEvent(
        deviceId: deviceId,
        generation: generation,
        kind: OtaStatusKind.unknown,
        state: text,
        rawText: text,
        payload: const <String, Object?>{},
      );
    }
    if (decoded is! Map<String, Object?>) {
      return null;
    }

    final String fullState = _string(decoded['st']);
    final String compactState = _string(decoded['s']);
    String state = fullState;
    String detail = _string(
      decoded['detail'] ?? decoded['det'] ?? decoded['d'],
    );
    OtaStatusKind kind = _kindForState(fullState);

    final String version = _string(decoded['v']);
    final String error = _string(decoded['e']);
    final Object? deviceInfo = decoded['i'];
    final Object? rawHealthValue = decoded['h'];
    final Object? rawHealthRecheck = decoded['c'];
    final String? rawHealthToken = rawHealthValue is String
        ? rawHealthValue
        : null;
    final String digestToken = rawHealthToken ?? '';
    String digestFragment = '';
    for (int index = 0; index < 8; ++index) {
      final String value = _string(decoded['h$index']);
      if (value.isNotEmpty) {
        digestFragment = '$index:$value';
        break;
      }
    }
    if (decoded.containsKey('op')) {
      kind =
          packet.length <= 223 &&
              (OtaAttemptProof.tryParse(decoded) != null ||
                  OtaAttemptProof.isUnavailable(decoded))
          ? OtaStatusKind.attemptProof
          : OtaStatusKind.attemptProofMalformed;
      state = 'attempt_proof';
      detail = '';
    } else if (decoded.containsKey('ov')) {
      final Object? acceptance = decoded['ov'];
      kind = decoded.length == 1 && (acceptance == 'p' || acceptance == 'v')
          ? OtaStatusKind.runtimeAcceptance
          : OtaStatusKind.runtimeAcceptanceMalformed;
      state = 'runtime_acceptance';
      detail = _string(acceptance);
    } else if (decoded.length == 1 && rawHealthRecheck == 'ok') {
      kind = OtaStatusKind.runtimeHealthRecheckSucceeded;
      state = 'runtime_health_recheck_succeeded';
      detail = 'ok';
    } else if (decoded.length == 1 && rawHealthRecheck == 'fail') {
      kind = OtaStatusKind.runtimeHealthRecheckFailed;
      state = 'runtime_health_recheck_failed';
      detail = 'fail';
    } else if (decoded.length == 1 && rawHealthRecheck != null) {
      kind = OtaStatusKind.runtimeHealthRecheckMalformed;
      state = 'runtime_health_recheck_malformed';
      detail = _string(rawHealthRecheck);
    } else if (deviceInfo is List<Object?> && deviceInfo.length >= 3) {
      kind = OtaStatusKind.deviceInfo;
      state = 'device_info';
      detail = '';
    } else if (_isExactRuntimeHealthFrame(
      packet: packet,
      rawText: rawText,
      payload: decoded,
      token: rawHealthToken,
    )) {
      kind = OtaStatusKind.runtimeHealth;
      state = 'runtime_health';
      detail = digestToken;
    } else if (rawHealthToken != null &&
        decoded.length == 1 &&
        RegExp(r'^[0-9A-F\s]+$').hasMatch(rawHealthToken)) {
      kind = OtaStatusKind.runtimeHealthMalformed;
      state = 'runtime_health_malformed';
      detail = digestToken;
    } else if (digestToken.isNotEmpty || digestFragment.isNotEmpty) {
      kind = OtaStatusKind.digest;
      detail = digestFragment;
      state = switch (digestToken) {
        'p' => 'digest_pending',
        'r' => 'digest_ready',
        'e' => 'digest_error',
        'u' => 'digest_unavailable',
        _ => 'digest_fragment',
      };
    } else if (version.isNotEmpty) {
      kind = OtaStatusKind.version;
      state = 'version';
      detail = version;
    } else if (error.isNotEmpty) {
      kind = OtaStatusKind.error;
      state = 'error';
      detail = error;
    } else if (_truthy(decoded['b'])) {
      kind = OtaStatusKind.begin;
      state = 'begin';
    } else if (decoded.containsKey('p') || decoded.containsKey('r')) {
      kind = OtaStatusKind.progress;
      state = 'progress';
    } else if (_truthy(decoded['a'])) {
      kind = OtaStatusKind.aborted;
      state = 'aborted';
    } else if (fullState.isEmpty && _truthy(decoded['d'])) {
      kind = OtaStatusKind.done;
      state = 'done';
      detail = '';
    } else if (fullState.isEmpty && compactState.isNotEmpty) {
      kind = _kindForCompactState(compactState);
      state = _stateForKind(kind, compactState);
    }

    if (state.isEmpty) {
      state = 'unknown';
    }
    final int? progress = _integer(decoded['pct'] ?? decoded['p']);
    int? rx = _integer(decoded['rx'] ?? decoded['r']);
    if (kind == OtaStatusKind.begin && rx == null) {
      rx = 0;
    }

    return OtaStatusEvent(
      deviceId: deviceId,
      generation: generation,
      kind: kind,
      state: state,
      detail: detail,
      progressPercent: progress,
      rxBytes: rx,
      totalBytes: _integer(decoded['total'] ?? decoded['t']),
      rawText: text,
      payload: Map<String, Object?>.unmodifiable(decoded),
    );
  }

  static OtaStatusKind _kindForState(String state) {
    return switch (state) {
      'begin' => OtaStatusKind.begin,
      'progress' => OtaStatusKind.progress,
      'done' => OtaStatusKind.done,
      'aborted' => OtaStatusKind.aborted,
      'error' => OtaStatusKind.error,
      'version' => OtaStatusKind.version,
      'probe' => OtaStatusKind.probe,
      _ => OtaStatusKind.unknown,
    };
  }

  static bool _isExactRuntimeHealthFrame({
    required List<int> packet,
    required String rawText,
    required Map<String, Object?> payload,
    required String? token,
  }) {
    if (token == null ||
        payload.length != 1 ||
        packet.length != 17 ||
        RuntimeHealthSnapshot.fromToken(token) == null) {
      return false;
    }
    return rawText == '{"h":"$token"}';
  }

  static OtaStatusKind _kindForCompactState(String state) {
    return switch (state) {
      'b' => OtaStatusKind.begin,
      'p' => OtaStatusKind.progress,
      'd' => OtaStatusKind.done,
      'a' => OtaStatusKind.aborted,
      'e' => OtaStatusKind.error,
      _ => OtaStatusKind.unknown,
    };
  }

  static String _stateForKind(OtaStatusKind kind, String fallback) {
    return switch (kind) {
      OtaStatusKind.begin => 'begin',
      OtaStatusKind.progress => 'progress',
      OtaStatusKind.done => 'done',
      OtaStatusKind.aborted => 'aborted',
      OtaStatusKind.error => 'error',
      _ => fallback,
    };
  }

  static String _string(Object? value) => (value ?? '').toString().trim();

  static int? _uint32(Object? value) =>
      value is int && value >= 0 && value <= 0xffffffff ? value : null;

  static int? _integer(Object? value) {
    if (value is int) {
      return value;
    }
    if (value is num) {
      return value.toInt();
    }
    return int.tryParse(_string(value));
  }

  static bool _truthy(Object? value) {
    if (value is bool) {
      return value;
    }
    if (value is num) {
      return value != 0;
    }
    final String token = _string(value).toLowerCase();
    return token == 'true' || token == 'yes' || token == '1';
  }
}
