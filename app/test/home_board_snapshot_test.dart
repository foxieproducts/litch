import 'package:flutter_test/flutter_test.dart';

import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';

void main() {
  test(
    'explicit resignation token remains distinct from timeout and capture',
    () {
      final home = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":1,"a":"idle",'
        '"fen":"4k3/8/8/8/8/8/8/4K3 w - - 0 1",'
        '"occ":"1000000000000010","g":"c","duck":"--","out":"r","win":"b",'
        '"rot":0,"white":1,"pm":0}',
      );
      expect(home, isNotNull);
      expect(home!.gameStatus.outcome, 'resignation');
      expect(home.gameStatus.winner, 'black');
    },
  );

  test(
    'Hint removal derives opposing victims and en passant from canonical FEN',
    () {
      for (final (fen, from, to, expected)
          in <(String, String, String, String?)>[
            ('7k/8/3p4/4P3/8/8/8/K7 w - - 0 1', 'e5', 'd6', 'd6'),
            ('7k/8/8/3pP3/8/8/8/K7 w - d6 0 1', 'e5', 'd6', 'd5'),
            ('7k/8/8/8/3p4/4P3/8/K7 b - - 0 1', 'd4', 'e3', 'e3'),
            ('7k/8/8/8/3pP3/8/8/K7 b - e3 0 1', 'd4', 'e3', 'e4'),
            ('7k/8/3P4/4P3/8/8/8/K7 w - - 0 1', 'e5', 'd6', null),
            ('7k/8/8/3pP3/8/8/8/K7 w - - 0 1', 'e5', 'd6', null),
            ('7k/8/8/3pN3/8/8/8/K7 w - d6 0 1', 'e5', 'd6', null),
            ('7k/8/8/3nP3/8/8/8/K7 w - d6 0 1', 'e5', 'd6', null),
            ('7k/8/3p4/4P3/8/8/8/K7 b - - 0 1', 'e5', 'd6', null),
          ]) {
        final position = HomeChessPosition.tryParse(fen)!;
        final assistance = HomeAssistanceState(
          requested: true,
          manualAvailable: true,
          coachingOwned: false,
          presentation: HomeAssistancePresentation.ready,
          move: (
            from: tryParseAlgebraicSquare(from)!,
            to: tryParseAlgebraicSquare(to)!,
          ),
        );
        final snapshot = HomeBoardSnapshot(
          position: position,
          homeStateAction: HomeStateAction.idle,
          physicalStatus: HomePhysicalBoardStatus(
            occupiedSquares: position.occupiedSquares,
          ),
          assistance: assistance,
        );
        final victim = tryParseAlgebraicSquare(expected);
        expect(snapshot.captureRemovalSquare, victim, reason: fen);
        expect(
          snapshot
              .copyWith(physicalStatus: const HomePhysicalBoardStatus())
              .captureRemovalSquare,
          isNull,
        );
        expect(
          snapshot.copyWith(clearAssistance: true).captureRemovalSquare,
          isNull,
        );
        expect(
          snapshot
              .copyWith(homeStateAction: HomeStateAction.correction)
              .captureRemovalSquare,
          isNull,
        );
        if (victim == null) continue;
        expect(
          snapshot
              .copyWith(
                homeStateAction: HomeStateAction.progress,
                physicalStatus: HomePhysicalBoardStatus(
                  occupiedSquares:
                      position.occupiedSquares & ~(BigInt.one << victim),
                ),
              )
              .captureRemovalSquare,
          isNull,
        );
        expect(
          snapshot
              .copyWith(
                assistance: HomeAssistanceState(
                  requested: false,
                  manualAvailable: true,
                  coachingOwned: false,
                  presentation: HomeAssistancePresentation.off,
                  move: assistance.move,
                ),
              )
              .captureRemovalSquare,
          isNull,
        );
      }
      expect(
        HomeChessPosition.tryParse(
          '7k/8/8/3pP3/8/8/8/K7 w - d6 0 1',
        )!.enPassantTarget,
        tryParseAlgebraicSquare('d6'),
      );
    },
  );

  const String initialFen =
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

  test(
    'optional CPU thinking preserves core authority and physical priority',
    () {
      String core({String side = 'w', String action = 'progress'}) =>
          '{"event":"home_state","v":1,"seq":7,"a":"$action",'
          '"fen":"8/8/8/8/8/8/8/K6k $side - - 0 1",'
          '"occ":"8100000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"co":3,"cl":[1,299,300,"$side",1,0,3,"n"]';

      for (final side in ['w', 'b']) {
        final live = HomeStateNotification.tryParse(
          '${core(side: side)},"th":["$side",6,8]}',
        )!;
        expect(live.cpuThinking?.elapsedHalfSeconds, 6);
        expect(live.cpuThinking?.remainingSeconds, 8);
        expect(live.cpuThinking?.side, live.position.sideToMove);
        expect(live.withoutAssistance().cpuThinking, same(live.cpuThinking));
        expect(live.withCpuThinking(null).clockStatus, same(live.clockStatus));
        final snapshot = HomeBoardSnapshot(
          position: live.position,
          gameStatus: live.gameStatus,
          physicalStatus: live.physicalStatus,
          homeStateAction: live.action,
          cpuThinking: live.cpuThinking,
        );
        expect(snapshot.showsCpuThinking, isTrue);
        for (final action in [
          HomeStateAction.correction,
          HomeStateAction.cpu,
          HomeStateAction.duck,
          HomeStateAction.promote,
          HomeStateAction.settle,
        ]) {
          expect(
            snapshot.copyWith(homeStateAction: action).showsCpuThinking,
            isFalse,
          );
        }
        expect(snapshot.copyWith(isRefreshing: true).showsCpuThinking, isFalse);
        expect(
          snapshot.copyWith(watchdogRestart: true).showsCpuThinking,
          isFalse,
        );
        expect(
          snapshot
              .copyWith(
                persistenceDegradation: HomePersistenceDegradation.storageIo,
              )
              .showsCpuThinking,
          isTrue,
        );
        expect(
          snapshot
              .copyWith(
                runtimeHealth: RuntimeHealthAvailable(
                  RuntimeHealthSnapshot.fromToken('000120004')!,
                ),
              )
              .showsCpuThinking,
          isFalse,
        );
        expect(snapshot.copyWith(clearLiveState: true).cpuThinking, isNull);
        expect(
          snapshot
              .copyWith(
                physicalStatus: live.physicalStatus.copyWith(
                  placementMode: true,
                ),
              )
              .showsCpuThinking,
          isFalse,
        );
        // Firmware's action owns correction. A progress FEN mismatch alone must
        // not turn the active worker into a fabricated physical correction.
        expect(
          snapshot
              .copyWith(
                physicalStatus: live.physicalStatus.copyWith(
                  occupiedSquares: BigInt.zero,
                ),
              )
              .showsCpuThinking,
          isTrue,
        );
      }
      for (final value in [
        'null',
        '{}',
        '["w",true,4]',
        '["w",-1,4]',
        '["w",4294967296,4]',
        '["b",6,8]',
        '["x",6,8]',
        '["w",6,0]',
        '["w",6,65536]',
        '["w",6,1.5]',
        '["w",6,"8"]',
        '["w",6]',
        '["w",6,8,9]',
      ]) {
        final parsed = HomeStateNotification.tryParse('${core()},"th":$value}');
        expect(
          parsed,
          isNotNull,
          reason: 'Invalid optional $value must preserve Home.',
        );
        expect(parsed!.clockStatus?.whiteRemainingSeconds, 299);
        expect(parsed.coachingMask, 3);
        expect(parsed.cpuThinking, isNull);
      }
      expect(HomeStateNotification.tryParse('${core()}}')!.cpuThinking, isNull);
      expect(
        HomeStateNotification.tryParse(
          '${core()},"th":["w",4294967295,null]}',
        )!.cpuThinking?.remainingSeconds,
        isNull,
      );
      for (final extra in ['"tw":"b"', '"out":"k","win":"b"']) {
        final terminal = core().replaceFirst(
          '"out":"n","win":"n"',
          extra.startsWith('"tw"') ? '"out":"n","win":"n",$extra' : extra,
        );
        expect(
          HomeStateNotification.tryParse(
            '$terminal,"th":["w",6,8]}',
          )!.cpuThinking,
          isNull,
        );
      }
      expect(
        HomeStateNotification.tryParse(
          '${core(action: 'cpu')},"th":["w",6,8]}',
        )!.cpuThinking,
        isNull,
      );
    },
  );

  test(
    'optional Duck plan preserves core authority and clears with live state',
    () {
      const core =
          '{"event":"home_state","v":1,"seq":7,"a":"duck",'
          '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
          '"occ":"8100000008000000","g":"d","duck":"d5",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"up":0,"co":3,"ps":"i","cl":[1,299,300,"w",1,0,3,"n"]';
      for (final entry in {
        'cd5e4': HomeDuckRelocationOwner.cpu,
        'hd5--': HomeDuckRelocationOwner.human,
        'ud5e4': HomeDuckRelocationOwner.neutral,
      }.entries) {
        final live = HomeStateNotification.tryParse(
          '$core,"dr":"${entry.key}"}',
        )!;
        expect(live.duckRelocationPrompt?.owner, entry.value);
        expect(live.duckRelocationPrompt?.fromSquare, 27);
        expect(
          live.duckRelocationPrompt?.toSquare,
          entry.value == HomeDuckRelocationOwner.human ? null : 36,
        );
        expect(
          live.withoutAssistance().duckRelocationPrompt,
          live.duckRelocationPrompt,
        );
        final projected = HomeBoardSnapshot(
          position: live.position,
          gameStatus: live.gameStatus,
          homeStateAction: live.action,
          duckRelocationPrompt: live.duckRelocationPrompt,
        );
        expect(projected.visibleDuckRelocation, isNotNull);
        expect(
          projected.copyWith(clearLiveState: true).visibleDuckRelocation,
          isNull,
        );
        final correction = HomeStateNotification.tryParse(
          '${core.replaceFirst('"a":"duck"', '"a":"corr"')},"dr":"${entry.key}"}',
        )!;
        expect(correction.gameStatus.duckMovePending, isTrue);
        final correcting = HomeBoardSnapshot(
          position: correction.position,
          gameStatus: correction.gameStatus,
          homeStateAction: correction.action,
          duckRelocationPrompt: correction.duckRelocationPrompt,
        );
        expect(correcting.isDuckRelocationCorrection, isTrue);
        expect(correcting.visibleDuckRelocation, isNull);
        expect(
          correcting.copyWith(clearLiveState: true).isDuckRelocationCorrection,
          isFalse,
        );
      }
      for (final token in [
        'null',
        '4',
        '{}',
        '"xd5e4"',
        '"cd5--"',
        '"c--e4"',
        '"cd5d5"',
        '"hd5e4"',
        '"cd9e4"',
        '"cd5e4extra"',
      ]) {
        final live = HomeStateNotification.tryParse('$core,"dr":$token}');
        expect(
          live,
          isNotNull,
          reason: 'Optional $token cannot lose valid Home.',
        );
        expect(live!.duckRelocationPrompt, isNull);
        expect(live.clockStatus?.whiteRemainingSeconds, 299);
        expect(live.coachingMask, 3);
        expect(
          live.persistenceDegradation,
          HomePersistenceDegradation.storageIo,
        );
      }
      expect(
        HomeStateNotification.tryParse('$core}')!.duckRelocationPrompt,
        isNull,
      );
      for (final plan in ['', ',"dr":"bad"']) {
        final correction = HomeStateNotification.tryParse(
          '${core.replaceFirst('"a":"duck"', '"a":"corr"')}$plan}',
        )!;
        expect(correction.gameStatus.duckMovePending, isFalse);
        expect(correction.duckRelocationPrompt, isNull);
      }
      for (final retired in [
        core.replaceFirst('"a":"duck"', '"a":"idle"'),
        core.replaceFirst('"g":"d"', '"g":"c"'),
        core.replaceFirst('"pm":0', '"pm":1'),
        '$core,"tw":"w"',
        core.replaceFirst('"out":"n","win":"n"', '"out":"k","win":"w"'),
      ]) {
        expect(
          HomeStateNotification.tryParse(
            '$retired,"dr":"cd5e4"}',
          )!.duckRelocationPrompt,
          isNull,
        );
      }
    },
  );

  test('parses a timestamped firmware FEN into a logical position', () {
    final HomeChessPosition? position = HomeChessPosition.tryParse(
      '09:41:12.012 [FW] $initialFen',
    );

    expect(position, isNotNull);
    expect(position!.sideToMove, HomeSide.white);
    expect(position.pieces, hasLength(32));
    expect(position.pieces[0], 'r');
    expect(position.pieces[60], 'K');
    expect(
      position.occupiedSquares,
      BigInt.parse('ffff00000000ffff', radix: 16),
    );
  });

  test('rejects malformed FEN instead of showing invented game state', () {
    expect(HomeChessPosition.tryParse('8/8/8/8/8/8/8 w - - 0 1'), isNull);
    expect(HomeChessPosition.tryParse('not a chess position'), isNull);
  });

  test('composes hardware rotation with player end', () {
    const HomePhysicalBoardStatus board = HomePhysicalBoardStatus(
      rotated: true,
      p1AtOppositeEnd: true,
    );

    expect(board.squareRotated, isFalse);
    expect(board.copyWith(p1AtOppositeEnd: false).squareRotated, isTrue);
    expect(board.copyWith(rotated: false).squareRotated, isTrue);
    expect(
      board.copyWith(rotated: false, p1AtOppositeEnd: false).squareRotated,
      isFalse,
    );
  });

  test('computes logical versus physical occupancy mismatches', () {
    final HomeChessPosition position = HomeChessPosition.tryParse(initialFen)!;
    final BigInt physical = position.occupiedSquares ^ BigInt.one;
    final HomeBoardSnapshot snapshot = HomeBoardSnapshot(
      position: position,
      physicalStatus: HomePhysicalBoardStatus(occupiedSquares: physical),
    );

    expect(snapshot.physicalPositionMatches, isFalse);
    expect(snapshot.mismatchedSquares, <int>{0});
    expect(algebraicSquare(0), 'a8');
  });

  test('counts the Duck as expected physical occupancy', () {
    final HomeChessPosition position = HomeChessPosition.tryParse(
      '8/8/8/8/8/8/8/K6k w - - 0 1',
    )!;
    final int duckSquare = tryParseAlgebraicSquare('e4')!;
    final HomeBoardSnapshot snapshot = HomeBoardSnapshot(
      position: position,
      gameStatus: HomeGameStatus(
        variant: 'duck',
        duckSquare: duckSquare,
        duckMovePending: false,
        outcome: 'none',
        winner: 'none',
      ),
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares: position.occupiedSquares | (BigInt.one << duckSquare),
      ),
    );

    expect(snapshot.physicalPositionMatches, isTrue);
  });

  test('accepts only bounded structured firmware notices', () {
    final BoardNotice? notice = BoardNotice.tryParse(
      '10:00:00.000 [FW] '
      '{"event":"notice","tone":"success","message":"Ready to play."}',
    );

    expect(notice, isNotNull);
    expect(notice!.message, 'Ready to play.');
    expect(notice.tone, BoardNoticeTone.success);
    expect(BoardNotice.tryParse('{"event":"debug","message":"no"}'), isNull);
    final String oversizedMessage = List<String>.filled(241, 'x').join();
    expect(
      BoardNotice.tryParse('{"event":"notice","message":"$oversizedMessage"}'),
      isNull,
    );
  });

  test('parses one complete versioned live Home snapshot', () {
    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":7,"a":"lift",'
      '"fen":"8/8/8/8/8/8/8/4K2k w - - 0 1",'
      '"occ":"0000000000000080","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":1,"white":1,"pm":0}',
    );

    expect(notification, isNotNull);
    expect(notification!.sequence, 7);
    expect(notification.action, HomeStateAction.lift);
    expect(notification.position.pieces[60], 'K');
    expect(
      notification.physicalStatus.occupiedSquares,
      BigInt.parse('80', radix: 16),
    );
    expect(notification.physicalStatus.rotated, isTrue);
    expect(notification.physicalStatus.whiteOwner, 'P1');
    expect(notification.physicalStatus.p1AtOppositeEnd, isFalse);
    expect(notification.physicalStatus.squareRotated, isTrue);
    final HomeBoardSnapshot projected = HomeBoardSnapshot(
      position: notification.position,
      gameStatus: notification.gameStatus,
      physicalStatus: notification.physicalStatus,
    );
    expect(projected.playerForSide(HomeSide.white), 'P1');
    expect(projected.playerForSide(HomeSide.black), 'P2');
    expect(notification.gameStatus.variant, 'classic');
    expect(notification.freshSetup, isFalse);
    expect(notification.undoPending, isNull);
  });

  test(
    'king and queen guidance requires explicit authority and clears with live state',
    () {
      const state =
          '{"event":"home_state","v":1,"seq":8,"a":"corr",'
          '"fen":"$initialFen","occ":"EFFF00000000FFFF","g":"u",'
          '"duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0}';
      final lifted = HomeStateNotification.tryParse(state)!;
      final snapshot = HomeBoardSnapshot(
        position: lifted.position,
        physicalStatus: lifted.physicalStatus,
        homeStateAction: lifted.action,
        kingQueenSwapPromptActive: lifted.kingQueenSwapPromptActive,
      );
      expect(snapshot.isGuidedKingQueenSwap, isFalse);
      final prompt = HomeStateNotification.tryParse(
        state.replaceFirst('"pm":0', '"pm":0,"kq":1'),
      )!;
      final active = snapshot.copyWith(
        kingQueenSwapPromptActive: prompt.kingQueenSwapPromptActive,
      );
      expect(active.isGuidedKingQueenSwap, isTrue);
      expect(active.copyWith(isRefreshing: true).isGuidedKingQueenSwap, isTrue);
      expect(
        active.copyWith(clearLiveState: true).isGuidedKingQueenSwap,
        isFalse,
      );
      expect(
        HomeStateNotification.tryParse(
          state.replaceFirst('"pm":0', '"pm":0,"kq":2'),
        ),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(
          state.replaceFirst('"pm":0', '"pm":1,"kq":1'),
        ),
        isNull,
      );
    },
  );

  test('parses additive Fresh Setup lifecycle and rejects malformed flags', () {
    const String setup =
        '{"event":"home_state","v":1,"seq":8,"a":"corr",'
        '"fen":"$initialFen","occ":"8386040308882780","g":"u",'
        '"duck":"--","out":"n","win":"n","rot":0,"white":1,'
        '"pm":1,"fs":1}';

    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      setup,
    );
    expect(notification, isNotNull);
    expect(notification!.freshSetup, isTrue);
    expect(
      HomeStateNotification.tryParse(setup.replaceFirst('"fs":1', '"fs":2')),
      isNull,
    );
  });

  test(
    'parses additive Vex move-first authority and rejects malformed flags',
    () {
      const String prompt =
          '{"event":"home_state","v":1,"seq":9,"a":"corr",'
          '"fen":"$initialFen","occ":"FFFF10000000FFFF","g":"d",'
          '"duck":"e3","out":"n","win":"n","rot":0,"white":1,'
          '"pm":0,"vx":1}';

      final HomeStateNotification? notification =
          HomeStateNotification.tryParse(prompt);
      expect(notification, isNotNull);
      expect(notification!.vexMoveFirst, isTrue);
      expect(
        HomeStateNotification.tryParse(prompt.replaceFirst('"vx":1', '"vx":2')),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(
          prompt.replaceFirst(',"vx":1', ''),
        )!.vexMoveFirst,
        isFalse,
      );
    },
  );

  test(
    'parses additive persistence degradation and rejects unknown tokens',
    () {
      const String snapshot =
          '{"event":"home_state","v":1,"seq":8,"a":"idle",'
          '"fen":"$initialFen","occ":"FFFF00000000FFFF","g":"c",'
          '"duck":"--","out":"n","win":"n","rot":0,"white":1,'
          '"pm":0,"ps":"q"}';

      expect(
        HomeStateNotification.tryParse(snapshot)?.persistenceDegradation,
        HomePersistenceDegradation.quarantinedRecord,
      );
      expect(
        HomeStateNotification.tryParse(
          snapshot.replaceFirst('"q"', '"i"'),
        )?.persistenceDegradation,
        HomePersistenceDegradation.storageIo,
      );
      expect(
        HomeStateNotification.tryParse(snapshot.replaceFirst('"q"', '"x"')),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(
          snapshot.replaceFirst(',"ps":"q"', ''),
        )?.persistenceDegradation,
        isNull,
      );
    },
  );

  test(
    'Fresh Setup guides leftover removals before missing starting pieces',
    () {
      final HomeChessPosition position = HomeChessPosition.tryParse(
        initialFen,
      )!;
      final HomeBoardSnapshot leftovers = HomeBoardSnapshot(
        position: position,
        freshSetup: true,
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: BigInt.parse('8386040308882780', radix: 16),
          placementMode: true,
        ),
      );

      expect(leftovers.mismatchedSquares, hasLength(27));
      expect(leftovers.unexpectedOccupiedSquares, hasLength(6));
      expect(leftovers.missingExpectedSquares, hasLength(21));
      expect(leftovers.guidedSquares, leftovers.unexpectedOccupiedSquares);
      expect(leftovers.isFreshSetupInProgress, isTrue);

      final HomeBoardSnapshot missingOnly = leftovers.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: position.occupiedSquares & ~BigInt.one,
          placementMode: true,
        ),
      );
      expect(missingOnly.guidedSquares, <int>{0});

      final HomeBoardSnapshot ready = leftovers.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: position.occupiedSquares,
          placementMode: false,
        ),
      );
      expect(ready.isFreshSetupInProgress, isFalse);

      final HomeBoardSnapshot wrongSidePickup = ready.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: position.occupiedSquares & ~BigInt.one,
          placementMode: false,
        ),
      );
      expect(wrongSidePickup.isFreshSetupInProgress, isFalse);
      expect(wrongSidePickup.guidedSquares, <int>{0});
    },
  );

  test('parses additive authoritative Undo state and rejects bad flags', () {
    const String pending =
        '{"event":"home_state","v":1,"seq":7,"a":"progress",'
        '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
        '"occ":"8100000000000000","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,"up":1}';

    expect(HomeStateNotification.tryParse(pending)?.undoPending, isTrue);
    expect(
      HomeStateNotification.tryParse(pending.replaceFirst('"up":1', '"up":2')),
      isNull,
    );
  });

  test('orders Home sequences with unsigned wrap and stale rejection', () {
    expect(isNewerHomeStateSequence(1, null), isTrue);
    expect(isNewerHomeStateSequence(1, 1), isFalse);
    expect(isNewerHomeStateSequence(2, 1), isTrue);
    expect(isNewerHomeStateSequence(1, 0xffffffff), isTrue);
    expect(isNewerHomeStateSequence(0xffffffff, 1), isFalse);
  });

  test(
    'Coaching is optional strict two-player authority, including both off',
    () {
      const String payload =
          '{"event":"home_state","v":1,"seq":7,"a":"idle",'
          '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
          '"occ":"8100000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0';
      expect(HomeStateNotification.tryParse('$payload}')?.coachingMask, isNull);
      for (int mask = 0; mask <= 3; ++mask) {
        expect(
          HomeStateNotification.tryParse('$payload,"co":$mask}')?.coachingMask,
          mask,
        );
      }
      for (final String invalid in <String>[
        '-1',
        '4',
        'true',
        'null',
        '"1"',
        '1.0',
        '[]',
      ]) {
        expect(
          HomeStateNotification.tryParse('$payload,"co":$invalid}'),
          isNull,
        );
      }
    },
  );

  test('parses an additive queued CPU move prompt', () {
    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":8,"a":"cpu",'
      '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
      '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
      '"c":["g8","f6","--"]}',
    );

    expect(notification, isNotNull);
    expect(notification!.action, HomeStateAction.cpu);
    expect(notification.physicalMovePrompt?.fromSquare, 6);
    expect(notification.physicalMovePrompt?.toSquare, 21);
    expect(notification.physicalMovePrompt?.captureSquare, isNull);
  });

  test(
    'human exact capture prompts require live physical removal authority',
    () {
      const pending =
          '{"event":"home_state","v":1,"seq":1,"a":"settle",'
          '"fen":"4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1",'
          '"occ":"1000080800000010","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
          '"as":"1x","c":["e4","d3","d4"]}';
      final state = HomeStateNotification.tryParse(pending)!;
      expect(state.physicalMovePrompt?.fromSquare, 36);
      expect(state.physicalMovePrompt?.toSquare, 43);
      expect(state.physicalMovePrompt?.captureSquare, 35);
      for (final invalid in [
        pending.replaceFirst('"settle"', '"corr"'),
        pending.replaceFirst('"settle"', '"idle"'),
        pending.replaceFirst('"settle"', '"progress"'),
        pending.replaceFirst('"d4"]', '"--"]'),
        pending.replaceFirst('"d4"]', '"e4"]'),
        pending.replaceFirst('1000080800000010', '1000080000000010'),
        pending.replaceFirst('"pm":0', '"pm":1'),
        pending.replaceFirst('"out":"n","win":"n"', '"out":"k","win":"b"'),
        pending.replaceFirst('"win":"n"', '"win":"n","tw":"w"'),
      ]) {
        expect(
          HomeStateNotification.tryParse(invalid),
          isNull,
          reason: invalid,
        );
      }
      expect(
        HomeStateNotification.tryParse(
          pending.replaceFirst('"settle"', '"lift"'),
        ),
        isNotNull,
      );
    },
  );

  test('accepts original v1 CPU and move aliases and rejects conflicts', () {
    const String legacy =
        '{"event":"home_state","v":1,"seq":9,"a":"cpu",'
        '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
        '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
        '"cf":"g8","ct":"f6","cr":"--","lm":"e2e4e7e5b"}';

    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      legacy,
    );
    expect(notification?.physicalMovePrompt?.fromSquare, 6);
    expect(notification?.physicalMovePrompt?.toSquare, 21);
    expect(notification?.lastMoves?.mostRecent, HomeSide.black);

    expect(
      HomeStateNotification.tryParse(
        legacy.replaceFirst('"cf":"g8"', '"c":["a1","a2","--"],"cf":"g8"'),
      ),
      isNull,
    );
    expect(
      HomeStateNotification.tryParse(
        legacy.replaceFirst(
          '"lm":"e2e4e7e5b"',
          '"m":"a2a4----w","lm":"e2e4e7e5b"',
        ),
      ),
      isNull,
    );
  });

  test('keeps coordinate-less version 1 CPU state compatible', () {
    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":8,"a":"cpu",'
      '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
      '"occ":"8100000000000000","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0}',
    );

    expect(notification, isNotNull);
    expect(notification!.physicalMovePrompt, isNull);
  });

  test('parses additive clock telemetry and timeout authority', () {
    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":9,"a":"idle",'
      '"fen":"8/8/8/8/8/8/8/K6k b - - 0 1",'
      '"occ":"8100000000000000","g":"c","duck":"--",'
      '"out":"n","win":"n","tw":"w",'
      '"rot":0,"white":1,"pm":0,'
      '"cl":[1,23,0,"b",0,0,3,"n"]}',
    );

    expect(notification, isNotNull);
    expect(notification!.gameStatus.outcome, 'timeout');
    expect(notification.gameStatus.winner, 'white');
    expect(notification.gameStatus.isFinished, isTrue);
    expect(notification.clockStatus?.enabled, isTrue);
    expect(notification.clockStatus?.whiteRemainingSeconds, 23);
    expect(notification.clockStatus?.blackRemainingSeconds, 0);
    expect(notification.clockStatus?.activeSide, HomeSide.black);
    expect(notification.clockStatus?.timerSetting, 3);
    expect(
      notification.clockStatus?.setupState,
      HomeClockSetupState.gameStarted,
    );
    expect(notification.clockStatus?.running, isFalse);
    expect(notification.clockStatus?.paused, isFalse);
  });

  test(
    'keeps legacy clock state unknown and rejects conflicting terminals',
    () {
      const String legacy =
          '{"event":"home_state","v":1,"seq":9,"a":"idle",'
          '"fen":"8/8/8/8/8/8/8/K6k b - - 0 1",'
          '"occ":"8100000000000000","g":"c","duck":"--",'
          '"out":"n","win":"n","rot":0,"white":1,"pm":0}';
      expect(HomeStateNotification.tryParse(legacy)?.clockStatus, isNull);

      expect(
        HomeStateNotification.tryParse(
          legacy.replaceFirst(
            '"out":"n","win":"n"',
            '"out":"k","win":"w","tw":"b"',
          ),
        ),
        isNull,
      );
    },
  );

  test('parses the latest move for each side and recency', () {
    final HomeStateNotification? notification = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":9,"a":"idle",'
      '"fen":"rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",'
      '"occ":"FFEF00101000EFFF","g":"c","duck":"--",'
      '"out":"n","win":"n","rot":0,"white":1,"pm":0,'
      '"m":"e2e4e7e5b"}',
    );

    expect(notification, isNotNull);
    expect(notification!.lastMoves?.white?.fromSquare, 52);
    expect(notification.lastMoves?.white?.toSquare, 36);
    expect(notification.lastMoves?.black?.fromSquare, 12);
    expect(notification.lastMoves?.black?.toSquare, 28);
    expect(notification.lastMoves?.mostRecent, HomeSide.black);
  });

  test('rejects unsupported, malformed, and oversized live snapshots', () {
    const String valid =
        '{"event":"home_state","v":1,"seq":7,"a":"idle",'
        '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1",'
        '"occ":"8100000000000000","g":"c","duck":"--",'
        '"out":"n","win":"n","rot":0,"white":1,"pm":0}';

    expect(
      HomeStateNotification.tryParse(valid.replaceFirst('"v":1', '"v":2')),
      isNull,
    );
    expect(
      HomeStateNotification.tryParse(
        valid.replaceFirst('8100000000000000', 'not-a-mask'),
      ),
      isNull,
    );
    expect(
      HomeStateNotification.tryParse(
        valid.replaceFirst('}', ',"c":["--","--","--"]}'),
      ),
      isNull,
    );
    final String oversized = valid.replaceFirst(
      '}',
      ',"future":"${List<String>.filled(320, 'x').join()}"}',
    );
    expect(HomeStateNotification.tryParse(oversized), isNull);
  });

  test('names only correction misses, not legal move progress', () {
    final HomeChessPosition position = HomeChessPosition.tryParse(
      '8/8/8/8/8/8/8/6Nk w - - 0 1',
    )!;
    final int g1 = tryParseAlgebraicSquare('g1')!;
    final BigInt lifted = position.occupiedSquares & ~(BigInt.one << g1);
    final HomeBoardSnapshot correction = HomeBoardSnapshot(
      position: position,
      physicalStatus: HomePhysicalBoardStatus(occupiedSquares: lifted),
      liveSequence: 8,
      homeStateAction: HomeStateAction.correction,
    );

    expect(correction.primaryAssistanceText, 'P1 knight expected on g1');
    expect(
      correction
          .copyWith(homeStateAction: HomeStateAction.lift)
          .primaryAssistanceText,
      isNull,
    );
  });
}
