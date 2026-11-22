import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litchcraft_phone_shake.dart';

void main() {
  bool sample(
    LitchcraftPhoneShakeDetector detector,
    int at,
    double acceleration, {
    bool eligible = true,
  }) => detector.observe(
    x: acceleration,
    y: 0,
    z: 0,
    monotonicMs: at,
    eligible: eligible,
  );

  List<int> feedShake(
    LitchcraftPhoneShakeDetector detector, {
    required int startMs,
    required int halfPeriodMs,
    required int samplePeriodMs,
    int durationMs = 1200,
    double amplitude = 15,
  }) {
    final triggers = <int>[];
    for (var elapsed = 0; elapsed <= durationMs; elapsed += samplePeriodMs) {
      final withinLobe = elapsed % halfPeriodMs;
      final acceleration = withinLobe >= halfPeriodMs - samplePeriodMs
          ? 0.0
          : (elapsed ~/ halfPeriodMs).isEven
          ? amplitude
          : -amplitude;
      if (sample(detector, startMs + elapsed, acceleration)) {
        triggers.add(startMs + elapsed);
      }
    }
    return triggers;
  }

  test('2 Hz reversals qualify despite short valleys between lobes', () {
    final detector = LitchcraftPhoneShakeDetector();
    expect(
      feedShake(detector, startMs: 0, halfPeriodMs: 250, samplePeriodMs: 50),
      <int>[1000],
    );
  });

  test('4 Hz production-cadence reversals qualify exactly once', () {
    final detector = LitchcraftPhoneShakeDetector();
    expect(
      feedShake(
        detector,
        startMs: 0,
        halfPeriodMs: 125,
        samplePeriodMs: 25,
        durationMs: 1600,
      ),
      <int>[1000],
    );
  });

  for (final amplitude in <double>[8, 15]) {
    test('three reversals at $amplitude qualify after a full second', () {
      final detector = LitchcraftPhoneShakeDetector();
      expect(
        feedShake(
          detector,
          startMs: 0,
          halfPeriodMs: 350,
          samplePeriodMs: 50,
          durationMs: 1100,
          amplitude: amplitude,
        ),
        <int>[1050],
      );
    });
  }

  test(
    'two energetic reversals or a short three-reversal run stay rejected',
    () {
      final twoReversals = LitchcraftPhoneShakeDetector();
      for (var at = 0; at <= 1200; at += 50) {
        final lobe = math.min(at ~/ 350, 2);
        final amplitude = 15 + (at % 350) / 100;
        expect(
          sample(twoReversals, at, lobe.isEven ? amplitude : -amplitude),
          isFalse,
        );
      }
      expect(
        feedShake(
          LitchcraftPhoneShakeDetector(),
          startMs: 0,
          halfPeriodMs: 250,
          samplePeriodMs: 50,
          durationMs: 950,
          amplitude: 8,
        ),
        isEmpty,
      );
    },
  );

  test('ordinary carrying and rotation stay below shake qualification', () {
    final carry = LitchcraftPhoneShakeDetector();
    final rotation = LitchcraftPhoneShakeDetector();
    for (var at = 0; at <= 1800; at += 50) {
      expect(sample(carry, at, (at ~/ 50).isEven ? 4 : -4), isFalse);
      final angle = at * math.pi / 1800;
      expect(
        rotation.observe(
          x: 4 * math.cos(angle),
          y: 4 * math.sin(angle),
          z: 0,
          monotonicMs: at,
          eligible: true,
        ),
        isFalse,
      );
    }
  });

  test('steady acceleration and a single jolt never qualify', () {
    final constant = LitchcraftPhoneShakeDetector();
    for (var at = 0; at <= 1800; at += 50) {
      expect(sample(constant, at, 15), isFalse);
    }

    final jolt = LitchcraftPhoneShakeDetector();
    expect(sample(jolt, 0, 15), isFalse);
    for (var at = 50; at <= 1500; at += 50) {
      expect(sample(jolt, at, 0), isFalse);
    }
  });

  test('long quiet interruption retires partial qualification', () {
    final detector = LitchcraftPhoneShakeDetector();
    feedShake(
      detector,
      startMs: 0,
      halfPeriodMs: 250,
      samplePeriodMs: 50,
      durationMs: 500,
    );
    for (var at = 550; at <= 950; at += 50) {
      expect(sample(detector, at, 0), isFalse);
    }
    expect(
      feedShake(detector, startMs: 1000, halfPeriodMs: 250, samplePeriodMs: 50),
      <int>[2000],
    );
  });

  test(
    'one-shot latch requires sustained quiet and ignores interrupted quiet',
    () {
      final detector = LitchcraftPhoneShakeDetector();
      expect(
        feedShake(detector, startMs: 0, halfPeriodMs: 250, samplePeriodMs: 50),
        <int>[1000],
      );
      expect(
        feedShake(
          detector,
          startMs: 1250,
          halfPeriodMs: 250,
          samplePeriodMs: 50,
          durationMs: 1000,
        ),
        isEmpty,
      );
      for (var at = 2300; at <= 2800; at += 50) {
        expect(sample(detector, at, 0), isFalse);
      }
      expect(sample(detector, 2850, 4), isFalse);
      for (var at = 2900; at <= 3500; at += 50) {
        expect(sample(detector, at, 0), isFalse);
      }
      expect(
        feedShake(
          detector,
          startMs: 3550,
          halfPeriodMs: 250,
          samplePeriodMs: 50,
        ),
        <int>[4550],
      );
    },
  );

  test('ineligible lifecycle and sensor gaps retire partial progress', () {
    final detector = LitchcraftPhoneShakeDetector();
    feedShake(
      detector,
      startMs: 0,
      halfPeriodMs: 250,
      samplePeriodMs: 50,
      durationMs: 750,
    );
    expect(sample(detector, 800, -15, eligible: false), isFalse);
    expect(
      feedShake(detector, startMs: 850, halfPeriodMs: 250, samplePeriodMs: 50),
      <int>[1850],
    );

    detector.reset();
    expect(sample(detector, 0, 15), isFalse);
    expect(sample(detector, 500, -15), isFalse);
  });
}
