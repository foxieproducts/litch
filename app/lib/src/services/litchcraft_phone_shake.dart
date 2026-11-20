import 'dart:math' as math;

/// Deterministic, monotonic qualification for one deliberate continuous shake.
///
/// A shake is a run of energetic, alternating lobes rather than sustained
/// acceleration. Brief low-energy valleys between lobes are expected. After a
/// trigger, continuously quiet motion must rearm the detector before another
/// shake can trigger.
class LitchcraftPhoneShakeDetector {
  static const int qualificationMs = 1000;
  static const int maximumSampleGapMs = 180;
  static const int maximumPeakGapMs = 350;
  static const int quietRearmMs = 600;
  static const double peakAcceleration = 7.5;
  static const double quietAcceleration = 2.5;
  static const double reversalCosine = -0.25;
  static const int minimumReversals = 3;

  int? _startedAtMs;
  int? _lastSampleAtMs;
  int? _lastPeakAtMs;
  int? _quietStartedAtMs;
  _AccelerationVector? _lastPeak;
  int _reversals = 0;
  bool _latched = false;

  bool observe({
    required double x,
    required double y,
    required double z,
    required int monotonicMs,
    required bool eligible,
  }) {
    if (!eligible) {
      reset();
      return false;
    }
    final vector = _AccelerationVector(x, y, z);
    final magnitude = vector.magnitude;
    final gap = _lastSampleAtMs == null ? 0 : monotonicMs - _lastSampleAtMs!;
    _lastSampleAtMs = monotonicMs;
    if (gap < 0 || gap > maximumSampleGapMs) {
      _resetQualification();
      _quietStartedAtMs = null;
      return false;
    }

    if (_latched) {
      if (magnitude <= quietAcceleration) {
        _quietStartedAtMs ??= monotonicMs;
        if (monotonicMs - _quietStartedAtMs! >= quietRearmMs) {
          _latched = false;
          _quietStartedAtMs = null;
        }
      } else {
        _quietStartedAtMs = null;
      }
      return false;
    }

    if (_lastPeakAtMs != null &&
        monotonicMs - _lastPeakAtMs! > maximumPeakGapMs) {
      _resetQualification();
    }
    if (magnitude < peakAcceleration) return false;

    if (_startedAtMs == null || _lastPeak == null) {
      _startedAtMs = monotonicMs;
      _lastPeakAtMs = monotonicMs;
      _lastPeak = vector;
      return false;
    }

    final previous = _lastPeak!;
    final cosine = vector.dot(previous) / (magnitude * previous.magnitude);
    if (cosine <= reversalCosine) {
      _reversals++;
      _lastPeakAtMs = monotonicMs;
      _lastPeak = vector;
    } else if (cosine > 0 && magnitude > previous.magnitude) {
      // Keep the strongest sample within one lobe without mistaking a run of
      // same-direction samples for repeated shake peaks.
      _lastPeakAtMs = monotonicMs;
      _lastPeak = vector;
    }

    if (monotonicMs - _startedAtMs! < qualificationMs ||
        _reversals < minimumReversals) {
      return false;
    }
    _latched = true;
    _quietStartedAtMs = null;
    _resetQualification();
    return true;
  }

  void reset() {
    _resetQualification();
    _lastSampleAtMs = null;
    _quietStartedAtMs = null;
    _latched = false;
  }

  void _resetQualification() {
    _startedAtMs = null;
    _lastPeakAtMs = null;
    _lastPeak = null;
    _reversals = 0;
  }
}

class _AccelerationVector {
  const _AccelerationVector(this.x, this.y, this.z);

  final double x;
  final double y;
  final double z;

  double get magnitude => math.sqrt(x * x + y * y + z * z);

  double dot(_AccelerationVector other) =>
      x * other.x + y * other.y + z * other.z;
}

class LitchcraftMotionSample {
  const LitchcraftMotionSample(this.x, this.y, this.z);
  final double x;
  final double y;
  final double z;
}
