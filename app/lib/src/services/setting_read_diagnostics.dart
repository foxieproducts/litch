import '../models/support_diagnostics.dart';

enum SettingReadOutcome { succeeded, recovered, exhausted, cancelled, unknown }

/// Measurement only; the BLE service remains the operation/lifecycle owner.
class SettingReadDiagnostics {
  static const int _maximum = 0xffffffff;
  int _operations = 0;
  int _directAttempts = 0;
  int _fallbackEntries = 0;
  int _retryAttempts = 0;
  int _directSuperseded = 0;
  final List<int> _outcomes = List<int>.filled(
    SettingReadOutcome.values.length,
    0,
  );
  int _totalDurationMs = 0;
  int _maxDurationMs = 0;
  bool _saturated = false;

  int _add(int current, int amount) {
    final total = current + amount;
    if (total > _maximum) {
      _saturated = true;
      return _maximum;
    }
    return total;
  }

  void started() => _operations = _add(_operations, 1);
  void directAttempted() => _directAttempts = _add(_directAttempts, 1);
  void directSuperseded() => _directSuperseded = _add(_directSuperseded, 1);

  void fallbackEntered({required bool retry}) {
    _fallbackEntries = _add(_fallbackEntries, 1);
    if (retry) _retryAttempts = _add(_retryAttempts, 1);
  }

  void completed(SettingReadOutcome outcome, Duration elapsed) {
    _outcomes[outcome.index] = _add(_outcomes[outcome.index], 1);
    final ms = elapsed.inMilliseconds.clamp(0, _maximum).toInt();
    if (elapsed.inMilliseconds > _maximum) _saturated = true;
    _totalDurationMs = _add(_totalDurationMs, ms);
    if (ms > _maxDurationMs) _maxDurationMs = ms;
  }

  SettingReadDiagnosticsSnapshot get snapshot => SettingReadDiagnosticsSnapshot(
    operations: _operations,
    directAttempts: _directAttempts,
    fallbackEntries: _fallbackEntries,
    retryAttempts: _retryAttempts,
    directSuperseded: _directSuperseded,
    succeeded: _outcomes[SettingReadOutcome.succeeded.index],
    recovered: _outcomes[SettingReadOutcome.recovered.index],
    exhausted: _outcomes[SettingReadOutcome.exhausted.index],
    cancelled: _outcomes[SettingReadOutcome.cancelled.index],
    unknown: _outcomes[SettingReadOutcome.unknown.index],
    totalDurationMs: _totalDurationMs,
    maxDurationMs: _maxDurationMs,
    saturated: _saturated,
  );
}
