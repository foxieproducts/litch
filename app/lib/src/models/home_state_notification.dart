import 'dart:convert';

import 'home_assistance_state.dart';
import 'home_board_snapshot.dart';
import 'litch_clock_preset.dart';

class HomeStateNotification {
  const HomeStateNotification({
    required this.sequence,
    required this.action,
    required this.position,
    required this.gameStatus,
    required this.physicalStatus,
    required this.freshSetup,
    required this.vexMoveFirst,
    this.kingQueenSwapPromptActive = false,
    this.undoPending,
    this.coachingMask,
    this.assistance,
    this.physicalMovePrompt,
    this.cpuThinking,
    this.duckRelocationPrompt,
    this.duckLandingRemovalMask,
    this.lastMoves,
    this.promotionConfirmation,
    this.clockStatus,
    this.persistenceDegradation,
  });

  static const int protocolVersion = 1;
  static const int maxSequence = 0xffffffff;
  static const int maxPayloadLength = 319;

  final int sequence;
  final HomeStateAction action;
  final HomeChessPosition position;
  final HomeGameStatus gameStatus;
  final HomePhysicalBoardStatus physicalStatus;
  // Additive v1 field. Older publishers omit it and are conservatively
  // treated as ordinary gameplay/correction.
  final bool freshSetup;
  // Additive v1 authority. Absence from an older publisher is false.
  final bool vexMoveFirst;
  final bool kingQueenSwapPromptActive;
  // Null means an older v1 publisher omitted this additive field.
  final bool? undoPending;
  // Optional v1 authority: bits 0/1 are logical P1/P2, not physical ends.
  final int? coachingMask;
  final HomeAssistanceState? assistance;
  final HomePhysicalMovePrompt? physicalMovePrompt;
  final HomeCpuThinking? cpuThinking;
  final HomeDuckRelocationPrompt? duckRelocationPrompt;
  final BigInt? duckLandingRemovalMask;
  final HomeLastMoves? lastMoves;
  final HomePromotionConfirmation? promotionConfirmation;
  final HomeClockStatus? clockStatus;
  final HomePersistenceDegradation? persistenceDegradation;

  /// Retain board position/order while an acknowledged Hint change makes the
  /// cached assistance uncertain. This is not a new firmware revision.
  HomeStateNotification withoutAssistance() =>
      _withTransientState(null, cpuThinking);

  /// Retire an estimate or search locally without changing board authority.
  HomeStateNotification withCpuThinking(HomeCpuThinking? thinking) =>
      _withTransientState(assistance, thinking);

  HomeStateNotification _withTransientState(
    HomeAssistanceState? assistance,
    HomeCpuThinking? thinking,
  ) => HomeStateNotification(
    sequence: sequence,
    action: action,
    position: position,
    gameStatus: gameStatus,
    physicalStatus: physicalStatus,
    freshSetup: freshSetup,
    vexMoveFirst: vexMoveFirst,
    kingQueenSwapPromptActive: kingQueenSwapPromptActive,
    undoPending: undoPending,
    coachingMask: coachingMask,
    assistance: assistance,
    physicalMovePrompt: physicalMovePrompt,
    cpuThinking: thinking,
    duckRelocationPrompt: duckRelocationPrompt,
    duckLandingRemovalMask: duckLandingRemovalMask,
    lastMoves: lastMoves,
    promotionConfirmation: promotionConfirmation,
    clockStatus: clockStatus,
    persistenceDegradation: persistenceDegradation,
  );

  static HomeStateNotification? tryParse(String input) {
    final String payload = normalizeFirmwareConsolePayload(input);
    if (!payload.startsWith('{') ||
        utf8.encode(payload).length > maxPayloadLength) {
      return null;
    }

    try {
      final Object? decoded = jsonDecode(payload);
      if (decoded is! Map<String, dynamic> ||
          decoded['event'] != 'home_state' ||
          decoded['v'] != protocolVersion) {
        return null;
      }

      final Object? sequenceValue = decoded['seq'];
      if (sequenceValue is! int ||
          sequenceValue < 1 ||
          sequenceValue > maxSequence) {
        return null;
      }
      final HomeStateAction? action = _tryParseAction(decoded['a']);
      final Object? fenValue = decoded['fen'];
      final HomeChessPosition? position = fenValue is String
          ? HomeChessPosition.tryParse(fenValue)
          : null;
      final Object? occupancyValue = decoded['occ'];
      final BigInt? occupancy =
          occupancyValue is String &&
              RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(occupancyValue)
          ? BigInt.tryParse(occupancyValue, radix: 16)
          : null;
      final String? variant = _tryParseVariant(decoded['g']);
      final Object? duckValue = decoded['duck'];
      final int? duckSquare = duckValue == '--'
          ? null
          : duckValue is String
          ? tryParseAlgebraicSquare(duckValue)
          : null;
      final bool duckValid = duckValue == '--' || duckSquare != null;
      final String? outcome = _tryParseOutcome(decoded['out']);
      final String? winner = _tryParseWinner(decoded['win']);
      final bool timeoutWinnerPresent = decoded.containsKey('tw');
      final String? timeoutWinner = timeoutWinnerPresent
          ? _tryParseWinner(decoded['tw'])
          : 'none';
      final bool outcomeValid =
          (outcome == 'none' && winner == 'none') ||
          (outcome != null &&
              outcome != 'none' &&
              winner != null &&
              winner != 'none');
      final bool? rotated = _tryParseBinaryFlag(decoded['rot']);
      final bool? placementMode = _tryParseBinaryFlag(decoded['pm']);
      final bool freshSetupPresent = decoded.containsKey('fs');
      final bool? parsedFreshSetup = freshSetupPresent
          ? _tryParseBinaryFlag(decoded['fs'])
          : false;
      final bool vexMoveFirstPresent = decoded.containsKey('vx');
      final bool? parsedVexMoveFirst = vexMoveFirstPresent
          ? _tryParseBinaryFlag(decoded['vx'])
          : false;
      final bool? kingQueenSwapPromptActive = decoded.containsKey('kq')
          ? _tryParseBinaryFlag(decoded['kq'])
          : false;
      final bool undoPendingPresent = decoded.containsKey('up');
      final bool? undoPending = undoPendingPresent
          ? _tryParseBinaryFlag(decoded['up'])
          : null;
      final String? whiteOwner = _tryParseWhiteOwner(decoded['white']);
      final Object? coachingValue = decoded['co'];
      final bool coachingValid =
          !decoded.containsKey('co') ||
          (coachingValue is int && coachingValue >= 0 && coachingValue <= 3);
      final _PhysicalPromptParse physicalPrompt = _tryParsePhysicalPrompt(
        decoded,
        action,
      );
      final HomeAssistanceState? assistance = _tryParseAssistance(
        decoded['as'],
      );
      final bool assistanceVariantValid = switch (assistance?.presentation) {
        HomeAssistancePresentation.compoundDuck ||
        HomeAssistancePresentation.duck => variant == 'duck',
        HomeAssistancePresentation.refreshing => variant != 'duck',
        _ => true,
      };
      final prompt = physicalPrompt.prompt;
      final pendingPromotionValid =
          prompt != null &&
          position != null &&
          (action == HomeStateAction.promote ||
              action == HomeStateAction.correction) &&
          prompt.isPromotionFor(position) &&
          decoded.containsKey('c') &&
          !decoded.containsKey('cf') &&
          !decoded.containsKey('ct') &&
          !decoded.containsKey('cr') &&
          outcome == 'none' &&
          timeoutWinner == 'none' &&
          placementMode == false &&
          undoPending != true &&
          kingQueenSwapPromptActive == false &&
          parsedFreshSetup == false &&
          parsedVexMoveFirst == false &&
          !decoded.containsKey('dr') &&
          !decoded.containsKey('th');
      final humanCaptureValid =
          prompt == null ||
          pendingPromotionValid ||
          action == HomeStateAction.cpu ||
          (decoded.containsKey('c') &&
              prompt.captureSquare != null &&
              occupancy != null &&
              (occupancy & (BigInt.one << prompt.captureSquare!)) !=
                  BigInt.zero &&
              outcome == 'none' &&
              timeoutWinner == 'none' &&
              placementMode == false);
      final _LastMovesParse lastMoves = _tryParseLastMovesFields(decoded);
      final _ClockParse clock = _tryParseClock(decoded);
      final _PersistenceDegradationParse persistenceDegradation =
          _tryParsePersistenceDegradation(decoded);
      final bool timeoutValid =
          timeoutWinner != null &&
          (timeoutWinner == 'none' || (outcome == 'none' && winner == 'none'));

      if (action == null ||
          position == null ||
          occupancy == null ||
          variant == null ||
          !duckValid ||
          !outcomeValid ||
          !timeoutValid ||
          rotated == null ||
          placementMode == null ||
          parsedFreshSetup == null ||
          parsedVexMoveFirst == null ||
          kingQueenSwapPromptActive == null ||
          (kingQueenSwapPromptActive &&
              (action != HomeStateAction.correction ||
                  placementMode ||
                  outcome != 'none' ||
                  timeoutWinner != 'none')) ||
          (undoPendingPresent && undoPending == null) ||
          whiteOwner == null ||
          !coachingValid ||
          (decoded.containsKey('as') && assistance == null) ||
          !assistanceVariantValid ||
          !physicalPrompt.valid ||
          !humanCaptureValid ||
          !lastMoves.valid ||
          !clock.valid ||
          !persistenceDegradation.valid) {
        return null;
      }

      final HomeDuckRelocationPrompt? duckRelocation =
          (action == HomeStateAction.duck ||
                  action == HomeStateAction.correction) &&
              variant == 'duck' &&
              outcome == 'none' &&
              timeoutWinner == 'none' &&
              !placementMode
          ? _tryParseDuckRelocation(decoded['dr'])
          : null;
      return HomeStateNotification(
        sequence: sequenceValue,
        action: action,
        position: position,
        gameStatus: HomeGameStatus(
          variant: variant,
          duckSquare: duckSquare,
          duckMovePending:
              action == HomeStateAction.duck ||
              (action == HomeStateAction.correction && duckRelocation != null),
          outcome: timeoutWinner == 'none' ? outcome! : 'timeout',
          winner: timeoutWinner == 'none' ? winner! : timeoutWinner,
        ),
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: occupancy,
          placementMode: placementMode,
          rotated: rotated,
          whiteOwner: whiteOwner,
        ),
        freshSetup: parsedFreshSetup,
        vexMoveFirst: parsedVexMoveFirst,
        kingQueenSwapPromptActive: kingQueenSwapPromptActive,
        undoPending: undoPending,
        coachingMask: coachingValue as int?,
        assistance: assistance,
        physicalMovePrompt: physicalPrompt.prompt,
        cpuThinking:
            outcome == 'none' &&
                timeoutWinner == 'none' &&
                (action == HomeStateAction.progress ||
                    action == HomeStateAction.idle ||
                    action == HomeStateAction.correction)
            ? _tryParseCpuThinking(decoded['th'], position.sideToMove)
            : null,
        duckRelocationPrompt: duckRelocation,
        lastMoves: lastMoves.moves,
        clockStatus: clock.status,
        persistenceDegradation: persistenceDegradation.degradation,
      );
    } on FormatException {
      return null;
    }
  }
}

HomeCpuThinking? _tryParseCpuThinking(Object? value, HomeSide sideToMove) {
  if (value is! List || value.length != 3) return null;
  final HomeSide? side = switch (value[0]) {
    'w' => HomeSide.white,
    'b' => HomeSide.black,
    _ => null,
  };
  final Object? elapsed = value[1];
  final Object? remaining = value[2];
  if (side == null ||
      side != sideToMove ||
      elapsed is! int ||
      elapsed < 0 ||
      elapsed > 0xffffffff ||
      (remaining != null &&
          (remaining is! int || remaining < 1 || remaining > 65535))) {
    return null;
  }
  return HomeCpuThinking(
    side: side,
    elapsedHalfSeconds: elapsed,
    remainingSeconds: remaining as int?,
  );
}

HomeDuckRelocationPrompt? _tryParseDuckRelocation(Object? value) {
  // Optional guidance must never discard a valid core Home/clock snapshot.
  if (value is! String || value.length != 5) return null;
  final HomeDuckRelocationOwner? owner = switch (value[0]) {
    'c' => HomeDuckRelocationOwner.cpu,
    'h' => HomeDuckRelocationOwner.human,
    'u' => HomeDuckRelocationOwner.neutral,
    _ => null,
  };
  final int? from = tryParseAlgebraicSquare(value.substring(1, 3));
  final String destination = value.substring(3, 5);
  final int? to = tryParseAlgebraicSquare(destination);
  if (owner == null ||
      from == null ||
      (owner == HomeDuckRelocationOwner.human
          ? destination != '--'
          : to == null || from == to)) {
    return null;
  }
  return HomeDuckRelocationPrompt(owner: owner, fromSquare: from, toSquare: to);
}

HomeAssistanceState? _tryParseAssistance(Object? value) {
  if (value is! String || value.length < 2) {
    return null;
  }
  final int? flags = int.tryParse(value[0]);
  if (flags == null || flags < 0 || flags > 7) {
    return null;
  }
  final HomeAssistancePresentation? presentation = switch (value[1]) {
    'x' => HomeAssistancePresentation.suppressed,
    'o' => HomeAssistancePresentation.off,
    'p' => HomeAssistancePresentation.pending,
    't' => HomeAssistancePresentation.thinking,
    'h' => HomeAssistancePresentation.ready,
    'r' => HomeAssistancePresentation.refreshing,
    'b' => HomeAssistancePresentation.compoundDuck,
    'd' => HomeAssistancePresentation.duck,
    'l' => HomeAssistancePresentation.legal,
    _ => null,
  };
  if (presentation == null) {
    return null;
  }
  final bool requested = (flags & 1) != 0;
  final bool requiresRequest = switch (presentation) {
    HomeAssistancePresentation.suppressed ||
    HomeAssistancePresentation.off ||
    HomeAssistancePresentation.legal => false,
    _ => true,
  };
  if ((requiresRequest && !requested) ||
      (presentation == HomeAssistancePresentation.off && requested)) {
    return null;
  }
  final int expectedLength = switch (presentation) {
    HomeAssistancePresentation.ready ||
    HomeAssistancePresentation.refreshing ||
    HomeAssistancePresentation.duck => 6,
    HomeAssistancePresentation.compoundDuck => 10,
    HomeAssistancePresentation.legal => value.length == 22 ? 22 : 20,
    _ => 2,
  };
  if (value.length != expectedLength) {
    return null;
  }
  ({int from, int to})? parseMove(int start) {
    final int? from = tryParseAlgebraicSquare(
      value.substring(start, start + 2),
    );
    final int? to = tryParseAlgebraicSquare(
      value.substring(start + 2, start + 4),
    );
    return from == null || to == null || from == to
        ? null
        : (from: from, to: to);
  }

  ({int from, int to})? move;
  ({int from, int to})? duckMove;
  int? legalSource;
  BigInt? legalDestinations;
  if (presentation == HomeAssistancePresentation.legal) {
    legalSource = tryParseAlgebraicSquare(value.substring(2, 4));
    final String mask = value.substring(4, 20);
    if (legalSource == null || !RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(mask)) {
      return null;
    }
    legalDestinations = BigInt.parse(mask, radix: 16);
    if (legalDestinations == BigInt.zero ||
        (legalDestinations & (BigInt.one << legalSource)) != BigInt.zero) {
      return null;
    }
    if (value.length == 22) {
      final target = tryParseAlgebraicSquare(value.substring(20));
      if (!requested ||
          target == null ||
          (legalDestinations & (BigInt.one << target)) == BigInt.zero) {
        return null;
      }
      move = (from: legalSource, to: target);
    }
  } else if (expectedLength >= 6) {
    final ({int from, int to})? first = parseMove(2);
    if (first == null) {
      return null;
    }
    if (presentation == HomeAssistancePresentation.duck) {
      duckMove = first;
    } else {
      move = first;
    }
    if (presentation == HomeAssistancePresentation.compoundDuck) {
      duckMove = parseMove(6);
      if (duckMove == null) {
        return null;
      }
    }
  }
  return HomeAssistanceState(
    requested: requested,
    manualAvailable: (flags & 2) != 0,
    coachingOwned: (flags & 4) != 0,
    presentation: presentation,
    move: move,
    duckMove: duckMove,
    legalSource: legalSource,
    legalDestinations: legalDestinations,
  );
}

class _PersistenceDegradationParse {
  const _PersistenceDegradationParse(this.valid, this.degradation);

  final bool valid;
  final HomePersistenceDegradation? degradation;
}

_PersistenceDegradationParse _tryParsePersistenceDegradation(
  Map<String, dynamic> decoded,
) {
  if (!decoded.containsKey('ps')) {
    return const _PersistenceDegradationParse(true, null);
  }
  return switch (decoded['ps']) {
    'q' => const _PersistenceDegradationParse(
      true,
      HomePersistenceDegradation.quarantinedRecord,
    ),
    'i' => const _PersistenceDegradationParse(
      true,
      HomePersistenceDegradation.storageIo,
    ),
    _ => const _PersistenceDegradationParse(false, null),
  };
}

class _ClockParse {
  const _ClockParse(this.valid, this.status);

  final bool valid;
  final HomeClockStatus? status;
}

_ClockParse _tryParseClock(Map<String, dynamic> decoded) {
  if (!decoded.containsKey('cl')) {
    return const _ClockParse(true, null);
  }
  final Object? value = decoded['cl'];
  if (value is! List<dynamic> || value.length != 8) {
    return const _ClockParse(false, null);
  }

  final bool? enabled = _tryParseBinaryFlag(value[0]);
  final Object? whiteValue = value[1];
  final Object? blackValue = value[2];
  final HomeSide? activeSide = switch (value[3]) {
    'w' => HomeSide.white,
    'b' => HomeSide.black,
    _ => null,
  };
  final bool? running = _tryParseBinaryFlag(value[4]);
  final bool? paused = _tryParseBinaryFlag(value[5]);
  final Object? timerSettingValue = value[6];
  final HomeClockSetupState? setupState = switch (value[7]) {
    'y' => HomeClockSetupState.available,
    'b' => HomeClockSetupState.busy,
    'n' => HomeClockSetupState.gameStarted,
    _ => null,
  };
  const int maxRemainingSeconds = 4294968;
  final bool remainingValid =
      whiteValue is int &&
      whiteValue >= 0 &&
      whiteValue <= maxRemainingSeconds &&
      blackValue is int &&
      blackValue >= 0 &&
      blackValue <= maxRemainingSeconds;
  if (enabled == null ||
      running == null ||
      paused == null ||
      activeSide == null ||
      timerSettingValue is! int ||
      !LitchClockPresets.contains(timerSettingValue) ||
      setupState == null ||
      !remainingValid ||
      (paused && !running) ||
      (!enabled && (running || paused || whiteValue != 0 || blackValue != 0))) {
    return const _ClockParse(false, null);
  }
  return _ClockParse(
    true,
    HomeClockStatus(
      enabled: enabled,
      running: running,
      paused: paused,
      activeSide: activeSide,
      timerSetting: timerSettingValue,
      setupState: setupState,
      whiteRemainingSeconds: whiteValue,
      blackRemainingSeconds: blackValue,
    ),
  );
}

bool isNewerHomeStateSequence(int candidate, int? previous) {
  if (previous == null) {
    return true;
  }
  final int difference =
      (candidate - previous) & HomeStateNotification.maxSequence;
  return difference != 0 && difference < 0x80000000;
}

class _LastMovesParse {
  const _LastMovesParse(this.valid, this.moves);

  final bool valid;
  final HomeLastMoves? moves;
}

_LastMovesParse _tryParseLastMoves(Object? value) {
  if (value == null) {
    return const _LastMovesParse(true, null);
  }
  if (value is! String || value.length != 9) {
    return const _LastMovesParse(false, null);
  }

  HomeMoveSquares? parseMove(String packed) {
    if (packed == '----') {
      return null;
    }
    final int? from = tryParseAlgebraicSquare(packed.substring(0, 2));
    final int? to = tryParseAlgebraicSquare(packed.substring(2, 4));
    return from == null || to == null
        ? null
        : HomeMoveSquares(fromSquare: from, toSquare: to);
  }

  final String whitePacked = value.substring(0, 4);
  final String blackPacked = value.substring(4, 8);
  final HomeMoveSquares? white = parseMove(whitePacked);
  final HomeMoveSquares? black = parseMove(blackPacked);
  if ((whitePacked != '----' && white == null) ||
      (blackPacked != '----' && black == null)) {
    return const _LastMovesParse(false, null);
  }
  final HomeSide? mostRecent = switch (value[8]) {
    'w' when white != null => HomeSide.white,
    'b' when black != null => HomeSide.black,
    'n' => null,
    _ => null,
  };
  if (value[8] != 'n' && mostRecent == null) {
    return const _LastMovesParse(false, null);
  }
  return _LastMovesParse(
    true,
    HomeLastMoves(white: white, black: black, mostRecent: mostRecent),
  );
}

_LastMovesParse _tryParseLastMovesFields(Map<String, dynamic> decoded) {
  final bool compactPresent = decoded.containsKey('m');
  final bool legacyPresent = decoded.containsKey('lm');
  if (compactPresent && legacyPresent && decoded['m'] != decoded['lm']) {
    return const _LastMovesParse(false, null);
  }
  return _tryParseLastMoves(compactPresent ? decoded['m'] : decoded['lm']);
}

class _PhysicalPromptParse {
  const _PhysicalPromptParse(this.valid, this.prompt);

  final bool valid;
  final HomePhysicalMovePrompt? prompt;
}

_PhysicalPromptParse _tryParsePhysicalPrompt(
  Map<String, dynamic> decoded,
  HomeStateAction? action,
) {
  final bool compactPresent = decoded.containsKey('c');
  final bool legacyPresent =
      decoded.containsKey('cf') ||
      decoded.containsKey('ct') ||
      decoded.containsKey('cr');
  if (!compactPresent && !legacyPresent) {
    // Some version 1 firmware published only the coarse `cpu` action.
    return const _PhysicalPromptParse(true, null);
  }

  final Object? compactValue = decoded['c'];
  if (compactPresent &&
      (compactValue is! List<dynamic> || compactValue.length != 3)) {
    return const _PhysicalPromptParse(false, null);
  }
  if (legacyPresent &&
      (!decoded.containsKey('cf') ||
          !decoded.containsKey('ct') ||
          !decoded.containsKey('cr'))) {
    return const _PhysicalPromptParse(false, null);
  }

  final List<dynamic> values = compactPresent
      ? compactValue as List<dynamic>
      : <dynamic>[decoded['cf'], decoded['ct'], decoded['cr']];
  if (compactPresent &&
      legacyPresent &&
      (values[0] != decoded['cf'] ||
          values[1] != decoded['ct'] ||
          values[2] != decoded['cr'])) {
    return const _PhysicalPromptParse(false, null);
  }

  final Object? fromValue = values[0];
  final Object? toValue = values[1];
  final Object? captureValue = values[2];
  final int? from = fromValue is String
      ? tryParseAlgebraicSquare(fromValue)
      : null;
  final int? to = toValue is String ? tryParseAlgebraicSquare(toValue) : null;
  final int? capture = captureValue == '--'
      ? null
      : captureValue is String
      ? tryParseAlgebraicSquare(captureValue)
      : null;
  final bool captureValid = captureValue == '--' || capture != null;
  final humanCapture =
      action == HomeStateAction.lift || action == HomeStateAction.settle;
  final promotion =
      action == HomeStateAction.promote || action == HomeStateAction.correction;
  if ((action != HomeStateAction.cpu && !humanCapture && !promotion) ||
      (promotion && (!compactPresent || legacyPresent || capture != null)) ||
      from == null ||
      to == null ||
      from == to ||
      !captureValid ||
      (humanCapture &&
          (!compactPresent || capture == null || capture == from))) {
    return const _PhysicalPromptParse(false, null);
  }
  return _PhysicalPromptParse(
    true,
    HomePhysicalMovePrompt(
      fromSquare: from,
      toSquare: to,
      captureSquare: capture,
    ),
  );
}

HomeStateAction? _tryParseAction(Object? value) => switch (value) {
  'idle' => HomeStateAction.idle,
  'lift' => HomeStateAction.lift,
  'progress' => HomeStateAction.progress,
  'duck' => HomeStateAction.duck,
  'corr' => HomeStateAction.correction,
  'settle' => HomeStateAction.settle,
  'promote' => HomeStateAction.promote,
  'cpu' => HomeStateAction.cpu,
  _ => null,
};

String? _tryParseVariant(Object? value) => switch (value) {
  'u' => 'undecided',
  'c' => 'classic',
  'd' => 'duck',
  _ => null,
};

String? _tryParseOutcome(Object? value) => switch (value) {
  'n' => 'none',
  'k' => 'king_captured',
  'f' => 'fowled',
  'r' => 'resignation',
  _ => null,
};

String? _tryParseWinner(Object? value) => switch (value) {
  'n' => 'none',
  'w' => 'white',
  'b' => 'black',
  _ => null,
};

String? _tryParseWhiteOwner(Object? value) => switch (value) {
  1 => 'P1',
  2 => 'P2',
  _ => null,
};

bool? _tryParseBinaryFlag(Object? value) => switch (value) {
  0 => false,
  1 => true,
  _ => null,
};
