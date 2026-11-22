/// Presentation-only rate estimate from recent firmware byte progress.
/// Handshake/download time is excluded because the first sample is a byte write.
class FirmwareTransferEstimate {
  static const _window = Duration(seconds: 8);
  static const _staleAfter = Duration(seconds: 4);
  static const _sampleInterval = Duration(milliseconds: 500);
  static const _minimumSpan = Duration(seconds: 2);
  final _samples = <({Duration time, double progress})>[];
  double? _progress;
  Duration? _lastAdvance;

  void reset() {
    _samples.clear();
    _progress = null;
    _lastAdvance = null;
  }

  void add(double progress, Duration now) {
    if (!progress.isFinite || progress <= 0 || progress >= 1) {
      reset();
      return;
    }
    final last = _lastAdvance;
    if (last != null &&
        (now < last || now - last >= _staleAfter || progress < _progress!)) {
      // A resumed or rolled-back transfer must earn a fresh estimate.
      reset();
    }
    if (_progress == progress) return;
    _progress = progress;
    _lastAdvance = now;
    if (_samples.isEmpty || now - _samples.last.time >= _sampleInterval) {
      _samples.add((time: now, progress: progress));
    }
    while (_samples.isNotEmpty &&
        (now - _samples.first.time > _window || _samples.length > 17)) {
      _samples.removeAt(0);
    }
  }

  Duration? remainingAt(Duration now) {
    final last = _lastAdvance;
    if (last == null ||
        now < last ||
        now - last >= _staleAfter ||
        _samples.length < 3 ||
        last - _samples.first.time < _minimumSpan) {
      return null;
    }
    final first = _samples.first;
    final advance = _progress! - first.progress;
    if (advance <= 0) return null;
    // The bounded rolling window smooths burst/jitter without averaging in
    // download, admission, or a previous attempt. Waiting slows the estimate.
    final seconds =
        (1 - _progress!) *
        (now - first.time).inMicroseconds /
        advance /
        Duration.microsecondsPerSecond;
    return Duration(seconds: seconds.ceil().clamp(1, 1 << 31));
  }
}
