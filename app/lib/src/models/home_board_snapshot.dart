import 'runtime_health_snapshot.dart';
import 'dart:convert';

import 'home_state_action.dart';
import 'home_assistance_state.dart';

export 'home_state_action.dart';

enum HomeSide { white, black }

enum BoardNoticeTone { info, success, warning, error }

class HomeChessPosition {
  const HomeChessPosition({
    required this.fen,
    required this.pieces,
    required this.sideToMove,
    required this.halfmoveClock,
    required this.fullmoveNumber,
    this.enPassantTarget,
  });

  final String fen;
  final Map<int, String> pieces;
  final HomeSide sideToMove;
  final int halfmoveClock;
  final int fullmoveNumber;
  final int? enPassantTarget;

  BigInt get occupiedSquares {
    BigInt mask = BigInt.zero;
    for (final int square in pieces.keys) {
      mask |= BigInt.one << square;
    }
    return mask;
  }

  static HomeChessPosition? tryParse(String input) {
    final String payload = normalizeFirmwareConsolePayload(input);
    final List<String> fields = payload.split(RegExp(r'\s+'));
    if (fields.length != 6 || (fields[1] != 'w' && fields[1] != 'b')) {
      return null;
    }

    final List<String> ranks = fields.first.split('/');
    if (ranks.length != 8) {
      return null;
    }

    final Map<int, String> pieces = <int, String>{};
    const String supportedPieces = 'prnbqkPRNBQK';
    for (int rank = 0; rank < ranks.length; rank += 1) {
      int file = 0;
      for (final int codePoint in ranks[rank].runes) {
        final String token = String.fromCharCode(codePoint);
        final int? emptyCount = int.tryParse(token);
        if (emptyCount != null) {
          if (emptyCount < 1 || emptyCount > 8) {
            return null;
          }
          file += emptyCount;
          continue;
        }

        if (!supportedPieces.contains(token) || file >= 8) {
          return null;
        }
        pieces[(rank * 8) + file] = token;
        file += 1;
      }
      if (file != 8) {
        return null;
      }
    }

    final int? halfmoveClock = int.tryParse(fields[4]);
    final int? fullmoveNumber = int.tryParse(fields[5]);
    if (halfmoveClock == null ||
        halfmoveClock < 0 ||
        fullmoveNumber == null ||
        fullmoveNumber < 1) {
      return null;
    }

    return HomeChessPosition(
      fen: payload,
      pieces: Map<int, String>.unmodifiable(pieces),
      sideToMove: fields[1] == 'w' ? HomeSide.white : HomeSide.black,
      halfmoveClock: halfmoveClock,
      fullmoveNumber: fullmoveNumber,
      enPassantTarget: tryParseAlgebraicSquare(fields[3]),
    );
  }
}

class HomeGameStatus {
  const HomeGameStatus({
    required this.variant,
    required this.duckSquare,
    required this.duckMovePending,
    required this.outcome,
    required this.winner,
  });

  final String variant;
  final int? duckSquare;
  final bool duckMovePending;
  final String outcome;
  final String winner;

  bool get isFinished {
    final String normalized = outcome.toLowerCase();
    return normalized != 'none' &&
        normalized != 'ongoing' &&
        normalized != 'in_progress';
  }
}

class HomeClockStatus {
  const HomeClockStatus({
    required this.enabled,
    required this.running,
    required this.paused,
    required this.activeSide,
    required this.timerSetting,
    required this.setupState,
    required this.whiteRemainingSeconds,
    required this.blackRemainingSeconds,
  });

  final bool enabled;
  final bool running;
  final bool paused;
  final HomeSide activeSide;
  final int timerSetting;
  final HomeClockSetupState setupState;
  final int whiteRemainingSeconds;
  final int blackRemainingSeconds;
}

enum HomeClockSetupState { available, busy, gameStarted }

class HomePhysicalBoardStatus {
  const HomePhysicalBoardStatus({
    this.occupiedSquares,
    this.placementMode,
    this.rotated,
    this.whiteOwner,
    this.p1AtOppositeEnd = false,
  });

  final BigInt? occupiedSquares;
  final bool? placementMode;
  final bool? rotated;
  // Deployed compact Home compatibility field. Current firmware reports P1;
  // player-end assignment is carried by GAME_FIRST_PLAYER instead.
  final String? whiteOwner;
  final bool p1AtOppositeEnd;

  /// Gameplay logical-to-raw squares compose hardware pose and player end.
  bool get squareRotated => (rotated ?? false) != p1AtOppositeEnd;

  HomePhysicalBoardStatus copyWith({
    BigInt? occupiedSquares,
    bool? placementMode,
    bool? rotated,
    String? whiteOwner,
    bool? p1AtOppositeEnd,
    bool clearOccupiedSquares = false,
  }) {
    return HomePhysicalBoardStatus(
      occupiedSquares: clearOccupiedSquares
          ? null
          : (occupiedSquares ?? this.occupiedSquares),
      placementMode: placementMode ?? this.placementMode,
      rotated: rotated ?? this.rotated,
      whiteOwner: whiteOwner ?? this.whiteOwner,
      p1AtOppositeEnd: p1AtOppositeEnd ?? this.p1AtOppositeEnd,
    );
  }
}

/// Exact board-owned CPU, human capture, or pending promotion move.
class HomePhysicalMovePrompt {
  const HomePhysicalMovePrompt({
    required this.fromSquare,
    required this.toSquare,
    this.captureSquare,
    this.promotionSelection,
  });

  final int fromSquare;
  final int toSquare;
  final int? captureSquare;
  final String? promotionSelection;

  bool isPromotionFor(HomeChessPosition position) {
    if (captureSquare != null ||
        fromSquare < 0 ||
        fromSquare >= 64 ||
        toSquare < 0 ||
        toSquare >= 64) {
      return false;
    }
    final white = position.sideToMove == HomeSide.white;
    if (position.pieces[fromSquare] != (white ? 'P' : 'p') ||
        fromSquare ~/ 8 != (white ? 1 : 6) ||
        toSquare ~/ 8 != (white ? 0 : 7)) {
      return false;
    }
    final fileDelta = (toSquare % 8 - fromSquare % 8).abs();
    final target = position.pieces[toSquare];
    return fileDelta == 0
        ? target == null
        : fileDelta == 1 &&
              target != null &&
              (white
                  ? target == target.toLowerCase()
                  : target == target.toUpperCase());
  }
}

enum HomeDuckRelocationOwner { human, cpu, neutral }

/// The current physical plan, independent of CPU settings and the FEN turn.
class HomeDuckRelocationPrompt {
  const HomeDuckRelocationPrompt({
    required this.owner,
    required this.fromSquare,
    this.toSquare,
  });

  final HomeDuckRelocationOwner owner;
  final int fromSquare;
  // Null only for a human-selectable destination.
  final int? toSquare;
}

class HomeMoveSquares {
  const HomeMoveSquares({required this.fromSquare, required this.toSquare});

  final int fromSquare;
  final int toSquare;
}

class HomeLastMoves {
  const HomeLastMoves({this.white, this.black, this.mostRecent});

  final HomeMoveSquares? white;
  final HomeMoveSquares? black;
  final HomeSide? mostRecent;
}

/// Board-owned persistence degradation reported by the additive Home snapshot.
enum HomePersistenceDegradation { quarantinedRecord, storageIo }

/// Optional board-owned status for the currently admitted CPU search.
class HomeCpuThinking {
  const HomeCpuThinking({
    required this.side,
    required this.elapsedHalfSeconds,
    this.remainingSeconds,
  });

  final HomeSide side;
  final int elapsedHalfSeconds;
  final int? remainingSeconds;

  HomeCpuThinking withoutEstimate() =>
      HomeCpuThinking(side: side, elapsedHalfSeconds: elapsedHalfSeconds);
}

typedef HomePromotionConfirmation = ({
  int from,
  int to,
  String piece,
  bool reveal,
  bool automatic,
});

class HomeBoardSnapshot {
  const HomeBoardSnapshot({
    this.position,
    this.gameStatus,
    this.physicalStatus,
    this.p1PieceWheel = 170,
    this.p2PieceWheel = 0,
    this.playerColorsConfirmed = true,
    this.isRefreshing = false,
    this.isClockRefreshing = false,
    this.refreshedAt,
    this.refreshMessage,
    this.liveSequence,
    this.homeStateAction,
    this.freshSetup = false,
    this.vexMoveFirst = false,
    this.kingQueenSwapPromptActive = false,
    this.physicalMovePrompt,
    this.cpuThinking,
    this.duckRelocationPrompt,
    this.duckLandingRemovalMask,
    this.assistance,
    this.lastMoves,
    this.promotionConfirmation,
    this.clockStatus,
    this.persistenceDegradation,
    this.runtimeHealth,
    this.watchdogRestart = false,
    this.persistenceSessionOnly = false,
  });

  final HomeChessPosition? position;
  final HomeGameStatus? gameStatus;
  final HomePhysicalBoardStatus? physicalStatus;
  final int p1PieceWheel;
  final int p2PieceWheel;
  final bool playerColorsConfirmed;
  final bool isRefreshing;
  final bool isClockRefreshing;
  final DateTime? refreshedAt;
  final String? refreshMessage;
  final int? liveSequence;
  final HomeStateAction? homeStateAction;
  final bool freshSetup;
  final bool vexMoveFirst;
  final bool kingQueenSwapPromptActive;
  final HomePhysicalMovePrompt? physicalMovePrompt;
  final HomeCpuThinking? cpuThinking;
  final HomeDuckRelocationPrompt? duckRelocationPrompt;
  final BigInt? duckLandingRemovalMask;
  final HomeAssistanceState? assistance;
  final HomeLastMoves? lastMoves;
  final HomePromotionConfirmation? promotionConfirmation;
  final HomeClockStatus? clockStatus;
  final HomePersistenceDegradation? persistenceDegradation;
  final RuntimeHealthResult? runtimeHealth;
  final bool watchdogRestart;
  final bool persistenceSessionOnly;

  bool get hasPosition => position != null;

  bool get showsCpuThinking {
    if (cpuThinking == null ||
        position == null ||
        cpuThinking!.side != position!.sideToMove ||
        isRefreshing ||
        (gameStatus?.isFinished ?? false) ||
        watchdogRestart ||
        physicalStatus?.placementMode == true ||
        isGuidedKingQueenSwap ||
        (homeStateAction != HomeStateAction.progress &&
            homeStateAction != HomeStateAction.idle) ||
        (physicalPositionMatches == false && !isExpectedBoardInteraction)) {
      return false;
    }
    if (runtimeHealth case RuntimeHealthAvailable(:final snapshot)) {
      if (snapshot.state == RuntimeHealthState.inputPaused) return false;
    }
    return true;
  }

  // The retained plan identifies a staged relocation during correction.
  // Canonical FEN differences cannot identify the offending physical square.
  bool get isDuckRelocationCorrection =>
      homeStateAction == HomeStateAction.correction &&
      duckRelocationPrompt != null &&
      (gameStatus?.duckMovePending ?? false) &&
      !(gameStatus?.isFinished ?? false) &&
      physicalStatus?.placementMode != true &&
      !isGuidedKingQueenSwap;

  bool get isDuckLandingCorrection =>
      homeStateAction == HomeStateAction.correction &&
      duckLandingRemovalMask != null;

  HomeDuckRelocationPrompt? get visibleDuckRelocation {
    if (homeStateAction != HomeStateAction.duck ||
        !(gameStatus?.duckMovePending ?? false) ||
        (gameStatus?.isFinished ?? false) ||
        physicalStatus?.placementMode == true) {
      return null;
    }
    if (runtimeHealth case RuntimeHealthAvailable(:final snapshot)) {
      if (snapshot.state == RuntimeHealthState.inputPaused) return null;
    }
    return duckRelocationPrompt;
  }

  HomeAssistanceState? get visibleAssistance {
    final settlingDuckHint =
        gameStatus?.variant == 'duck' &&
        assistance?.presentation == HomeAssistancePresentation.duck &&
        assistance?.requested == true &&
        assistance?.duckMove != null &&
        assistance!.duckMove!.from == gameStatus?.duckSquare;
    if (position == null ||
        (gameStatus?.isFinished ?? false) ||
        physicalStatus?.placementMode == true ||
        isGuidedKingQueenSwap ||
        isFreshSetupInProgress ||
        homeStateAction == HomeStateAction.cpu ||
        (visibleDuckRelocation != null &&
            visibleDuckRelocation!.owner != HomeDuckRelocationOwner.human) ||
        homeStateAction == HomeStateAction.promote ||
        homeStateAction == HomeStateAction.correction ||
        (homeStateAction == HomeStateAction.settle && !settlingDuckHint) ||
        (physicalPositionMatches == false && !isExpectedBoardInteraction)) {
      return null;
    }
    if (runtimeHealth case RuntimeHealthAvailable(:final snapshot)) {
      if (snapshot.state == RuntimeHealthState.inputPaused) {
        return null;
      }
    }
    return assistance;
  }

  /// An outstanding removal uses accepted presence, not just a queued move.
  /// An exact physical plan outranks the suggested Hint capture from its FEN.
  int? get captureRemovalSquare {
    final occupied = physicalStatus?.occupiedSquares;
    if (occupied == null ||
        (gameStatus?.isFinished ?? false) ||
        physicalStatus?.placementMode == true) {
      return null;
    }
    if (runtimeHealth case RuntimeHealthAvailable(:final snapshot)) {
      if (snapshot.state == RuntimeHealthState.inputPaused) return null;
    }
    final physicalMoveEligible =
        homeStateAction == HomeStateAction.cpu ||
        homeStateAction == HomeStateAction.lift ||
        homeStateAction == HomeStateAction.settle;
    final capture = physicalMoveEligible && physicalMovePrompt != null
        ? physicalMovePrompt!.captureSquare
        : _hintCaptureSquare;
    return capture != null &&
            (occupied & (BigInt.one << capture)) != BigInt.zero
        ? capture
        : null;
  }

  int? get _hintCaptureSquare {
    final state = visibleAssistance;
    final move =
        state?.isReady == true ||
            state?.presentation == HomeAssistancePresentation.legal
        ? state?.move
        : null;
    final logical = position;
    if (move == null || logical == null) return null;
    final mover = logical.pieces[move.from];
    if (mover == null) return null;
    final white = mover == mover.toUpperCase();
    if (white != (logical.sideToMove == HomeSide.white)) return null;
    final victim = logical.pieces[move.to];
    if (victim != null) {
      return (victim == victim.toUpperCase()) != white ? move.to : null;
    }
    if (mover.toLowerCase() != 'p' ||
        move.to != logical.enPassantTarget ||
        move.to ~/ 8 != (white ? 2 : 5)) {
      return null;
    }
    final direction = white ? -1 : 1;
    if (move.to ~/ 8 - move.from ~/ 8 != direction ||
        (move.to % 8 - move.from % 8).abs() != 1) {
      return null;
    }
    final capturedPawn = move.to - direction * 8;
    return logical.pieces[capturedPawn] == (white ? 'p' : 'P')
        ? capturedPawn
        : null;
  }

  String? get assistanceGuidance {
    final HomeAssistanceState? state = visibleAssistance;
    if (state == null) {
      return null;
    }
    String moveText(({int from, int to}) move) =>
        '${algebraicSquare(move.from)} to ${algebraicSquare(move.to)}';
    final capture = captureRemovalSquare;
    if (capture != null && state.move != null) {
      final instruction =
          'Remove the piece on ${algebraicSquare(capture)}, '
          'then move the piece from ${moveText(state.move!)}.';
      return state.presentation == HomeAssistancePresentation.compoundDuck
          ? '$instruction Duck hint: ${moveText(state.duckMove!)}.'
          : instruction;
    }
    return switch (state.presentation) {
      HomeAssistancePresentation.ready ||
      HomeAssistancePresentation.refreshing =>
        'Hint: ${moveText(state.move!)}.',
      HomeAssistancePresentation.compoundDuck =>
        'Hint: ${moveText(state.move!)}, then duck ${moveText(state.duckMove!)}.',
      HomeAssistancePresentation.duck =>
        'Duck hint: ${moveText(state.duckMove!)}.',
      HomeAssistancePresentation.legal =>
        'Legal moves from ${algebraicSquare(state.legalSource!)} are marked.',
      _ => null,
    };
  }

  HomePhysicalMovePrompt? get pendingPromotionPrompt {
    final prompt = physicalMovePrompt;
    final logical = position;
    return prompt != null &&
            logical != null &&
            (homeStateAction == HomeStateAction.promote ||
                homeStateAction == HomeStateAction.correction) &&
            physicalStatus?.placementMode != true &&
            !kingQueenSwapPromptActive &&
            !freshSetup &&
            !vexMoveFirst &&
            gameStatus?.isFinished != true &&
            duckRelocationPrompt == null &&
            prompt.isPromotionFor(logical)
        ? prompt
        : null;
  }

  bool get promotionPlacementPending {
    final prompt = pendingPromotionPrompt;
    final physical = physicalStatus?.occupiedSquares;
    return prompt != null &&
        physical != null &&
        (physical & (BigInt.one << prompt.toSquare)) == BigInt.zero;
  }

  /// Physical presence comes from the pending plan; its replacement type is unknown.
  String? physicalPieceAt(int square) {
    if (_stagedPromotion case final promotion?) {
      if (square == promotion.from) return null;
      if (square == promotion.to) return promotion.piece;
    }
    final prompt = pendingPromotionPrompt;
    if (prompt != null &&
        (square == prompt.fromSquare || square == prompt.toSquare)) {
      return null;
    }
    return position?.pieces[square];
  }

  HomePromotionConfirmation? get _stagedPromotion =>
      gameStatus?.duckMovePending == true &&
          duckRelocationPrompt?.owner == HomeDuckRelocationOwner.human
      ? promotionConfirmation
      : null;

  BigInt? get expectedOccupiedSquares {
    final HomeChessPosition? logical = position;
    if (logical == null) {
      return null;
    }
    BigInt expected = logical.occupiedSquares;
    final staged = _stagedPromotion;
    final pending = pendingPromotionPrompt;
    if (staged != null) {
      expected =
          (expected & ~(BigInt.one << staged.from)) | (BigInt.one << staged.to);
    } else if (pending != null) {
      expected =
          (expected & ~(BigInt.one << pending.fromSquare)) |
          (BigInt.one << pending.toSquare);
    }
    final int? duckSquare = gameStatus?.duckSquare;
    if (duckSquare != null) {
      expected |= BigInt.one << duckSquare;
    } else {
      final int? provisionalDuck = _provisionalOpeningDuckSquare(
        logical,
        physicalStatus?.occupiedSquares,
        gameStatus?.variant,
      );
      if (provisionalDuck != null) {
        expected |= BigInt.one << provisionalDuck;
      }
    }
    return expected;
  }

  Set<int> get mismatchedSquares {
    final BigInt? expected = expectedOccupiedSquares;
    final BigInt? physical = physicalStatus?.occupiedSquares;
    if (expected == null || physical == null) {
      return const <int>{};
    }

    final BigInt mismatch = expected ^ physical;
    final Set<int> squares = <int>{};
    for (int square = 0; square < 64; square += 1) {
      if ((mismatch & (BigInt.one << square)) != BigInt.zero) {
        squares.add(square);
      }
    }
    return Set<int>.unmodifiable(squares);
  }

  Set<int> get missingExpectedSquares {
    final BigInt? expected = expectedOccupiedSquares;
    final BigInt? physical = physicalStatus?.occupiedSquares;
    if (expected == null || physical == null) {
      return const <int>{};
    }
    return _squaresInMask(expected & ~physical);
  }

  Set<int> get unexpectedOccupiedSquares {
    final BigInt? expected = expectedOccupiedSquares;
    final BigInt? physical = physicalStatus?.occupiedSquares;
    if (expected == null || physical == null) {
      return const <int>{};
    }
    return _squaresInMask(physical & ~expected);
  }

  /// Squares that are actionable in the board's current presentation phase.
  /// Fresh Setup removes leftover pieces before revealing missing targets so
  /// a New Game never turns the whole app board into one large warning.
  Set<int> get guidedSquares {
    if (isDuckLandingCorrection) {
      return _squaresInMask(duckLandingRemovalMask!);
    }
    if (!isFreshSetupInProgress) {
      return mismatchedSquares;
    }
    final Set<int> extras = unexpectedOccupiedSquares;
    return extras.isNotEmpty ? extras : missingExpectedSquares;
  }

  /// Whether Fresh Setup currently owns the board interaction. The broader
  /// lifecycle remains reversible after Play Ready, but placement mode ends
  /// before ordinary move correction resumes.
  bool get isFreshSetupInProgress =>
      freshSetup && physicalStatus?.placementMode == true;

  bool get isLegalLiveProgress => switch (homeStateAction) {
    HomeStateAction.lift ||
    HomeStateAction.progress ||
    HomeStateAction.duck ||
    HomeStateAction.settle ||
    HomeStateAction.promote ||
    HomeStateAction.cpu => true,
    _ => false,
  };

  bool get isGuidedKingQueenSwap => kingQueenSwapPromptActive;

  bool get isExpectedBoardInteraction =>
      isLegalLiveProgress || isGuidedKingQueenSwap;

  String playerForSide(HomeSide side) {
    // Logical game ownership is fixed in the current protocol contract.
    // Physical P1/P2 control-bank labels are a separate hardware concept.
    return side == HomeSide.white ? 'P1' : 'P2';
  }

  /// Engine coordinates project into a fixed near/far viewer frame.
  int viewSquare(int logicalSquare) => physicalStatus?.p1AtOppositeEnd == true
      ? 63 - logicalSquare
      : logicalSquare;

  String? get primaryAssistanceText {
    if (isExpectedBoardInteraction ||
        isDuckRelocationCorrection ||
        isDuckLandingCorrection) {
      return null;
    }
    if (mismatchedSquares.length != 1) {
      return null;
    }
    final int square = mismatchedSquares.single;
    if (missingExpectedSquares.contains(square)) {
      final String? piece = physicalPieceAt(square);
      if (piece != null) {
        return '${_homePieceName(piece)} expected on ${algebraicSquare(square)}';
      }
      if (gameStatus?.duckSquare == square) {
        return 'Duck expected on ${algebraicSquare(square)}';
      }
      if (pendingPromotionPrompt?.toSquare == square) {
        return 'Piece expected on ${algebraicSquare(square)}';
      }
    }
    if (unexpectedOccupiedSquares.contains(square)) {
      return 'Unexpected occupied square on ${algebraicSquare(square)}';
    }
    return null;
  }

  bool? get physicalPositionMatches {
    if (position == null || physicalStatus?.occupiedSquares == null) {
      return null;
    }
    return mismatchedSquares.isEmpty;
  }

  HomeBoardSnapshot copyWith({
    HomeChessPosition? position,
    HomeGameStatus? gameStatus,
    HomePhysicalBoardStatus? physicalStatus,
    int? p1PieceWheel,
    int? p2PieceWheel,
    bool? playerColorsConfirmed,
    bool? isRefreshing,
    bool? isClockRefreshing,
    DateTime? refreshedAt,
    String? refreshMessage,
    bool clearRefreshMessage = false,
    int? liveSequence,
    HomeStateAction? homeStateAction,
    bool? freshSetup,
    bool? vexMoveFirst,
    bool? kingQueenSwapPromptActive,
    HomePhysicalMovePrompt? physicalMovePrompt,
    HomeCpuThinking? cpuThinking,
    HomeDuckRelocationPrompt? duckRelocationPrompt,
    BigInt? duckLandingRemovalMask,
    HomeAssistanceState? assistance,
    HomeLastMoves? lastMoves,
    HomePromotionConfirmation? promotionConfirmation,
    HomeClockStatus? clockStatus,
    HomePersistenceDegradation? persistenceDegradation,
    RuntimeHealthResult? runtimeHealth,
    bool? watchdogRestart,
    bool? persistenceSessionOnly,
    bool clearLiveState = false,
    bool clearPhysicalMovePrompt = false,
    bool clearCpuThinking = false,
    bool clearDuckRelocationPrompt = false,
    bool clearDuckLandingRemovalMask = false,
    bool clearAssistance = false,
    bool clearLastMoves = false,
    bool clearPromotionConfirmation = false,
    bool clearClockStatus = false,
    bool clearPersistenceDegradation = false,
  }) {
    return HomeBoardSnapshot(
      position: position ?? this.position,
      gameStatus: gameStatus ?? this.gameStatus,
      physicalStatus: physicalStatus ?? this.physicalStatus,
      p1PieceWheel: p1PieceWheel ?? this.p1PieceWheel,
      p2PieceWheel: p2PieceWheel ?? this.p2PieceWheel,
      playerColorsConfirmed:
          playerColorsConfirmed ?? this.playerColorsConfirmed,
      isRefreshing: isRefreshing ?? this.isRefreshing,
      isClockRefreshing: clearLiveState
          ? false
          : (isClockRefreshing ?? this.isClockRefreshing),
      refreshedAt: refreshedAt ?? this.refreshedAt,
      refreshMessage: clearRefreshMessage
          ? null
          : (refreshMessage ?? this.refreshMessage),
      liveSequence: clearLiveState ? null : (liveSequence ?? this.liveSequence),
      homeStateAction: clearLiveState
          ? null
          : (homeStateAction ?? this.homeStateAction),
      freshSetup: clearLiveState ? false : (freshSetup ?? this.freshSetup),
      vexMoveFirst: clearLiveState
          ? false
          : (vexMoveFirst ?? this.vexMoveFirst),
      kingQueenSwapPromptActive: clearLiveState
          ? false
          : (kingQueenSwapPromptActive ?? this.kingQueenSwapPromptActive),
      cpuThinking: clearLiveState || clearCpuThinking
          ? null
          : (cpuThinking ?? this.cpuThinking),
      physicalMovePrompt: clearLiveState || clearPhysicalMovePrompt
          ? null
          : (physicalMovePrompt ?? this.physicalMovePrompt),
      duckRelocationPrompt: clearLiveState || clearDuckRelocationPrompt
          ? null
          : (duckRelocationPrompt ?? this.duckRelocationPrompt),
      duckLandingRemovalMask: clearLiveState || clearDuckLandingRemovalMask
          ? null
          : (duckLandingRemovalMask ?? this.duckLandingRemovalMask),
      assistance: clearLiveState || clearAssistance
          ? null
          : (assistance ?? this.assistance),
      lastMoves: clearLiveState || clearLastMoves
          ? null
          : (lastMoves ?? this.lastMoves),
      promotionConfirmation: clearLiveState || clearPromotionConfirmation
          ? null
          : promotionConfirmation ?? this.promotionConfirmation,
      clockStatus: clearLiveState || clearClockStatus
          ? null
          : (clockStatus ?? this.clockStatus),
      runtimeHealth: clearLiveState
          ? null
          : (runtimeHealth ?? this.runtimeHealth),
      persistenceSessionOnly: clearLiveState
          ? false
          : (persistenceSessionOnly ?? this.persistenceSessionOnly),
      watchdogRestart: clearLiveState
          ? false
          : (watchdogRestart ?? this.watchdogRestart),
      persistenceDegradation: clearLiveState || clearPersistenceDegradation
          ? null
          : (persistenceDegradation ?? this.persistenceDegradation),
    );
  }
}

int? _provisionalOpeningDuckSquare(
  HomeChessPosition position,
  BigInt? physical,
  String? variant,
) {
  if (physical == null ||
      variant?.toLowerCase() != 'undecided' ||
      !position.fen.startsWith(
        'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR ',
      )) {
    return null;
  }
  final BigInt extras = physical & ~position.occupiedSquares;
  if (extras == BigInt.zero ||
      (extras & (extras - BigInt.one)) != BigInt.zero) {
    return null;
  }
  for (int square = 16; square < 48; square += 1) {
    if ((extras & (BigInt.one << square)) != BigInt.zero) {
      return square;
    }
  }
  return null;
}

class BoardNotice {
  const BoardNotice({required this.message, required this.tone});

  final String message;
  final BoardNoticeTone tone;

  static BoardNotice? tryParse(String input) {
    final String payload = normalizeFirmwareConsolePayload(input);
    if (!payload.startsWith('{')) {
      return null;
    }

    try {
      final Object? decoded = jsonDecode(payload);
      if (decoded is! Map<String, dynamic> || decoded['event'] != 'notice') {
        return null;
      }
      final String message = (decoded['message'] ?? '').toString().trim();
      if (message.isEmpty || message.length > 240) {
        return null;
      }

      final BoardNoticeTone tone = switch (decoded['tone']) {
        'success' => BoardNoticeTone.success,
        'warning' => BoardNoticeTone.warning,
        'error' => BoardNoticeTone.error,
        _ => BoardNoticeTone.info,
      };
      return BoardNotice(message: message, tone: tone);
    } on FormatException {
      return null;
    }
  }
}

String normalizeFirmwareConsolePayload(String input) {
  final String line = input.trim();
  final int firmwarePrefix = line.indexOf('[FW] ');
  if (firmwarePrefix >= 0) {
    return line.substring(firmwarePrefix + '[FW] '.length).trim();
  }
  return line;
}

int? tryParseAlgebraicSquare(String? value) {
  if (value == null || !RegExp(r'^[a-h][1-8]$').hasMatch(value)) {
    return null;
  }
  final int file = value.codeUnitAt(0) - 'a'.codeUnitAt(0);
  final int rank = int.parse(value[1]);
  return ((8 - rank) * 8) + file;
}

String algebraicSquare(int square) {
  if (square < 0 || square >= 64) {
    return '';
  }
  final String file = String.fromCharCode('a'.codeUnitAt(0) + (square % 8));
  final int rank = 8 - (square ~/ 8);
  return '$file$rank';
}

Set<int> _squaresInMask(BigInt mask) {
  final Set<int> squares = <int>{};
  for (int square = 0; square < 64; square += 1) {
    if ((mask & (BigInt.one << square)) != BigInt.zero) {
      squares.add(square);
    }
  }
  return Set<int>.unmodifiable(squares);
}

String _homePieceName(String piece) {
  final String player = piece == piece.toUpperCase() ? 'P1' : 'P2';
  final String name = switch (piece.toLowerCase()) {
    'k' => 'king',
    'q' => 'queen',
    'r' => 'rook',
    'b' => 'bishop',
    'n' => 'knight',
    _ => 'pawn',
  };
  return '$player $name';
}
