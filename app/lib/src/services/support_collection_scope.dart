import 'dart:async';

import '../models/support_board_collection.dart';

/// Cancellation and outstanding SDK work for one support collection only.
/// A returned partial report does not mean its uncancelable native call ended.
class SupportCollectionScope {
  SupportCollectionScope({
    required this.isCurrent,
    required Duration timeout,
    Future<void>? cancelled,
  }) {
    _timer = Timer(timeout, () => stop(SupportQueryOutcome.timedOut));
    cancelled?.then(
      (_) => stop(SupportQueryOutcome.canceled),
      onError: (Object _) => stop(SupportQueryOutcome.canceled),
    );
  }

  final bool Function() isCurrent;
  final Completer<void> _stopped = Completer<void>();
  Timer? _timer;
  SupportQueryOutcome? _reason;
  int _nativePending = 0;
  Completer<void>? _nativeDrained;

  Future<void> get stopped => _stopped.future;
  SupportQueryOutcome? get reason {
    if (!isCurrent()) stop(SupportQueryOutcome.sessionChanged);
    return _reason;
  }

  Future<void> get nativeDrained =>
      _nativeDrained?.future ?? Future<void>.value();

  void stop(SupportQueryOutcome reason) {
    if (_reason != null) return;
    _reason = reason;
    _timer?.cancel();
    _stopped.complete();
  }

  void check() {
    final SupportQueryOutcome? stoppedReason = reason;
    if (stoppedReason != null) throw SupportCollectionStopped(stoppedReason);
  }

  Future<T> wait<T>(Future<T> pending) {
    // Register both continuations even if already stopped, consuming late SDK
    // errors without allowing them to restart the collection.
    final Future<T> stoppedResult = stopped.then<T>((_) {
      throw SupportCollectionStopped(_reason!);
    });
    reason;
    return Future.any(<Future<T>>[stoppedResult, pending]);
  }

  Future<T> native<T>(Future<T> Function() issue) {
    check();
    if (_nativePending++ == 0) _nativeDrained = Completer<void>();
    final Future<T> pending = Future<T>.sync(issue).whenComplete(() {
      if (--_nativePending == 0) _nativeDrained!.complete();
    });
    return wait(pending);
  }

  void finish() {
    _timer?.cancel();
    stop(SupportQueryOutcome.canceled);
  }
}

class SupportCollectionStopped implements Exception {
  const SupportCollectionStopped(this.outcome);
  final SupportQueryOutcome outcome;
}
