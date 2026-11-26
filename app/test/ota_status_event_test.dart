import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';

void main() {
  test('Bluetooth suffix accepts only the exact board response', () {
    OtaStatusEvent? read(Object? value) => OtaStatusEvent.decode(
      utf8.encode(jsonEncode({'bs': value})),
      deviceId: 'board',
      generation: 1,
    );
    expect(read('3F81')?.bluetoothSuffix, '3F81');
    final unrelatedError = OtaStatusEvent.decode(
      utf8.encode(jsonEncode({'e': 'busy', 'bs': '3F81'})),
      deviceId: 'board',
      generation: 1,
    );
    expect(unrelatedError?.kind, OtaStatusKind.error);
    expect(unrelatedError?.bluetoothSuffix, isNull);
    for (final value in <Object?>[null, '3f81', 'AA:BB', '12345', 0x3f81, '']) {
      expect(read(value)?.bluetoothSuffix, isNull);
    }
  });

  OtaStatusEvent decode(Map<String, Object?> payload) {
    return OtaStatusEvent.decode(
      utf8.encode(jsonEncode(payload)),
      deviceId: 'board-1',
      generation: 3,
    )!;
  }

  test('decodes full and compact OTA control payloads', () {
    expect(
      decode(<String, Object?>{'st': 'begin', 'rx': 0}).kind,
      OtaStatusKind.begin,
    );
    expect(decode(<String, Object?>{'b': 1}).kind, OtaStatusKind.begin);
    expect(
      decode(<String, Object?>{'p': 37, 'r': 8192}).kind,
      OtaStatusKind.progress,
    );
    expect(decode(<String, Object?>{'p': 37, 'r': 8192}).rxBytes, 8192);
    expect(decode(<String, Object?>{'s': 'd'}).kind, OtaStatusKind.done);
    expect(decode(<String, Object?>{'d': 1}).kind, OtaStatusKind.done);
    final measured = decode({'s': 'd', 'wb': 0xffffffff, 'wm': 0});
    expect(measured.firmwareWriteBytes, 0xffffffff);
    expect(measured.firmwareWriteMilliseconds, 0);
    final malformedMetrics = decode({'s': 'd', 'wb': -1, 'wm': 0x100000000});
    expect(malformedMetrics.kind, OtaStatusKind.done);
    expect(malformedMetrics.firmwareWriteBytes, isNull);
    expect(malformedMetrics.firmwareWriteMilliseconds, isNull);
    expect(decode(<String, Object?>{'s': 'a'}).kind, OtaStatusKind.aborted);
    expect(decode(<String, Object?>{'a': 1}).kind, OtaStatusKind.aborted);
  });

  test('decodes compact version and error without confusing detail', () {
    final OtaStatusEvent version = decode(<String, Object?>{'v': '1.2.3'});
    expect(version.kind, OtaStatusKind.version);
    expect(version.detail, '1.2.3');

    final OtaStatusEvent error = decode(<String, Object?>{'e': 'unknown_cmd'});
    expect(error.kind, OtaStatusKind.error);
    expect(error.detail, 'unknown_cmd');
  });

  test('decodes digest lifecycle and fragments on the OTA channel', () {
    expect(decode(<String, Object?>{'h': 'p'}).state, 'digest_pending');
    final OtaStatusEvent fragment = decode(<String, Object?>{'h3': '89abcdef'});
    expect(fragment.kind, OtaStatusKind.digest);
    expect(fragment.detail, '3:89abcdef');
    expect(decode(<String, Object?>{'h': 'r'}).state, 'digest_ready');
  });

  test('acceptance proof requires one exact pending or valid state', () {
    for (final token in <String>['p', 'v']) {
      final event = decode(<String, Object?>{'ov': token});
      expect(event.kind, OtaStatusKind.runtimeAcceptance);
      expect(event.detail, token);
    }
    for (final payload in <Map<String, Object?>>[
      <String, Object?>{'ov': 'valid'},
      <String, Object?>{'ov': true},
      <String, Object?>{'ov': null},
      <String, Object?>{'ov': 'v', 'extra': 1},
    ]) {
      expect(decode(payload).kind, OtaStatusKind.runtimeAcceptanceMalformed);
    }
  });

  test('classifies only the fixed health token before digest traffic', () {
    final OtaStatusEvent health = decode(<String, Object?>{'h': '000120034'});
    expect(health.kind, OtaStatusKind.runtimeHealth);
    expect(health.detail, '000120034');
    expect(
      RuntimeHealthSnapshot.fromToken(health.detail)?.state,
      RuntimeHealthState.inputPaused,
    );
    expect(
      decode(<String, Object?>{'h': '00012003'}).kind,
      OtaStatusKind.runtimeHealthMalformed,
    );
    expect(
      decode(<String, Object?>{'h0': '89abcdef'}).kind,
      OtaStatusKind.digest,
    );
  });

  test('classifies only exact health recheck result frames', () {
    expect(
      decode(<String, Object?>{'c': 'ok'}).kind,
      OtaStatusKind.runtimeHealthRecheckSucceeded,
    );
    expect(
      decode(<String, Object?>{'c': 'fail'}).kind,
      OtaStatusKind.runtimeHealthRecheckFailed,
    );
    expect(
      decode(<String, Object?>{'c': 'maybe'}).kind,
      OtaStatusKind.runtimeHealthRecheckMalformed,
    );
    expect(
      decode(<String, Object?>{'c': 'ok', 'x': 1}).kind,
      OtaStatusKind.unknown,
    );
  });

  test('requires an exact raw health frame without coercion or padding', () {
    OtaStatusEvent decodeRaw(String raw) {
      return OtaStatusEvent.decode(
        utf8.encode(raw),
        deviceId: 'board-1',
        generation: 3,
      )!;
    }

    expect(decodeRaw('{"h":"000120034"}').kind, OtaStatusKind.runtimeHealth);
    expect(
      decodeRaw('{"h":" 000120034"}').kind,
      isNot(OtaStatusKind.runtimeHealth),
    );
    expect(decodeRaw('{"h":120034}').kind, isNot(OtaStatusKind.runtimeHealth));
    expect(
      decodeRaw('{"h":"000120034","x":1}').kind,
      isNot(OtaStatusKind.runtimeHealth),
    );
    expect(
      decodeRaw(' {"h":"000120034"}').kind,
      isNot(OtaStatusKind.runtimeHealth),
    );
  });

  test('parses health states and preserves future cause information', () {
    final RuntimeHealthSnapshot healthy = RuntimeHealthSnapshot.fromToken(
      '00AF00000',
    )!;
    final RuntimeHealthSnapshot paused = RuntimeHealthSnapshot.fromToken(
      '00AF20004',
    )!;
    final RuntimeHealthSnapshot protected = RuntimeHealthSnapshot.fromToken(
      '00AF30030',
    )!;
    final RuntimeHealthSnapshot future = RuntimeHealthSnapshot.fromToken(
      '00AF90041',
    )!;

    expect(healthy.state, RuntimeHealthState.healthy);
    expect(paused.state, RuntimeHealthState.inputPaused);
    expect(protected.state, RuntimeHealthState.protectionLatched);
    expect(future.state, RuntimeHealthState.unknown);
    expect(future.unknownCauseMask, 0x40);
    expect(future.supportCode, '00AF-9-0041');
    expect(RuntimeHealthSnapshot.fromToken('00af00000'), isNull);
    expect(RuntimeHealthSnapshot.fromToken('00AF0000'), isNull);
  });

  test('decodes minimum-MTU device capability envelope', () {
    final OtaStatusEvent event = decode(<String, Object?>{
      'i': <Object?>[1, 1, 15],
    });

    expect(event.kind, OtaStatusKind.deviceInfo);
    expect(event.state, 'device_info');
    expect(event.rawText.length, lessThanOrEqualTo(20));
  });

  test('decodes receive-only progress used by minimum-MTU large images', () {
    final OtaStatusEvent event = decode(<String, Object?>{'r': 1000000});

    expect(event.kind, OtaStatusKind.progress);
    expect(event.rxBytes, 1000000);
  });
}
