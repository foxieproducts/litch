import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/app_chess_game_session.dart';
import 'package:litch_mobile/src/models/game_archive.dart';

void main() {
  test(
    'monotonic clock charges moves, increment, and exact undo checkpoint',
    () {
      var wall = DateTime.utc(2026, 9, 13, 8);
      var monotonic = 0;
      final AppChessGameSession session = _newClockSession(
        now: () => wall,
        monotonicMicros: () => monotonic,
        base: const Duration(seconds: 5),
        increment: const Duration(seconds: 2),
      );

      expect(session.clock!.runState, ArchivedGameClockRunState.ready);
      expect(session.startClock(), isTrue);
      monotonic = 1500000;
      expect(session.clock!.whiteRemaining, const Duration(milliseconds: 3500));
      expect(session.playUci('e2e4'), AppChessMoveResult.accepted);
      expect(session.clock!.whiteRemaining, const Duration(milliseconds: 5500));
      expect(session.clock!.blackRemaining, const Duration(seconds: 5));

      monotonic = 2000000;
      expect(session.playUci('e7e5'), AppChessMoveResult.accepted);
      expect(session.clock!.blackRemaining, const Duration(milliseconds: 6500));
      expect(session.archive.clock!.turnStartCheckpoints, hasLength(3));

      expect(session.undo(), isTrue);
      expect(session.clock!.whiteRemaining, const Duration(milliseconds: 5500));
      expect(session.clock!.blackRemaining, const Duration(seconds: 5));
      expect(session.archive.clock!.turnStartCheckpoints, hasLength(2));
      session.dispose();
    },
  );

  test('exact zero seals casual flag-fall before a move can commit', () {
    var monotonic = 0;
    final AppChessGameSession session = _newClockSession(
      monotonicMicros: () => monotonic,
      base: const Duration(seconds: 1),
    );
    session.startClock();
    monotonic = 1000000;

    expect(session.settleClock(), isTrue);
    expect(session.archive.status, GameArchiveStatus.completed);
    expect(session.archive.outcome, GameOutcome.timeout);
    expect(session.archive.winner, GameWinner.black);
    expect(session.clock!.whiteRemaining, Duration.zero);
    expect(session.playUci('e2e4'), AppChessMoveResult.gameOver);
    expect(session.undo(), isFalse);
    session.dispose();
  });

  test(
    'suspension charges its boundary and ignores background elapsed time',
    () {
      var monotonic = 0;
      final AppChessGameSession session = _newClockSession(
        monotonicMicros: () => monotonic,
        base: const Duration(seconds: 5),
      );
      session.startClock();
      monotonic = 500000;
      session.suspendClock();
      expect(session.clock!.whiteRemaining, const Duration(milliseconds: 4500));
      expect(session.clock!.runState, ArchivedGameClockRunState.suspended);
      expect(session.playUci('e2e4'), AppChessMoveResult.clockPaused);

      monotonic = 10500000;
      expect(session.clock!.whiteRemaining, const Duration(milliseconds: 4500));
      session.resumeClock();
      monotonic = 11000000;
      expect(session.clock!.whiteRemaining, const Duration(seconds: 4));
      session.dispose();
    },
  );

  test('running relaunch charges one bounded UTC delta and reanchors', () {
    var wall = DateTime.utc(2026, 9, 13, 8);
    final AppChessGameSession original = _newClockSession(
      now: () => wall,
      base: const Duration(seconds: 5),
    );
    original.startClock();
    final GameArchive running = original.archive;
    original.dispose();

    wall = wall.add(const Duration(seconds: 2));
    final AppChessGameSession restored = AppChessGameSession.restore(
      archive: running,
      now: () => wall,
      monotonicMicros: () => 0,
      persist: (_) async {},
    );
    expect(restored.clock!.whiteRemaining, const Duration(seconds: 3));
    expect(restored.clock!.runState, ArchivedGameClockRunState.running);
    expect(restored.clock!.recoveryRequired, isFalse);
    restored.dispose();
  });

  test('relaunch clock anomaly preserves balances and requires resume', () {
    final DateTime sampled = DateTime.utc(2026, 9, 13, 8);
    final AppChessGameSession original = _newClockSession(
      now: () => sampled,
      base: const Duration(minutes: 5),
    );
    original.startClock();
    final GameArchive running = original.archive;
    original.dispose();

    for (final DateTime anomalousNow in <DateTime>[
      sampled.subtract(const Duration(seconds: 1)),
      sampled.add(const Duration(hours: 25)),
    ]) {
      final AppChessGameSession restored = AppChessGameSession.restore(
        archive: running,
        now: () => anomalousNow,
        monotonicMicros: () => 0,
        persist: (_) async {},
      );
      expect(restored.clock!.whiteRemaining, const Duration(minutes: 5));
      expect(restored.clock!.runState, ArchivedGameClockRunState.suspended);
      expect(restored.clock!.recoveryRequired, isTrue);
      restored.dispose();
    }
  });
}

AppChessGameSession _newClockSession({
  DateTime Function()? now,
  int Function()? monotonicMicros,
  required Duration base,
  Duration increment = Duration.zero,
}) {
  return AppChessGameSession.newGame(
    identity: const AppChessGameIdentity(
      gameId: 'app_clock_test',
      libraryId: 'app-clock-test',
    ),
    now: now ?? () => DateTime.utc(2026, 9, 13, 8),
    monotonicMicros: monotonicMicros,
    persist: (_) async {},
    clockConfiguration: AppChessClockConfiguration(
      base: base,
      increment: increment,
    ),
  );
}
