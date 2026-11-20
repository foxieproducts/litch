import 'dart:typed_data';

import 'home_assistance_state.dart';
import 'home_board_snapshot.dart';
import 'home_state_notification.dart';

final class HomeStateWireCodec {
  const HomeStateWireCodec._();

  static const int version = 1;
  static const int promotionVersion = 2;
  static const int promotionSelectionVersion = 3;
  static const int payloadBytes = 100;
  static const int fragmentBytes = 10;
  static const int firstStateCode = 4;
  static const int fragmentCount = 10;
  static const int lastStateCode = firstStateCode + fragmentCount - 1;

  static HomeStateNotification? tryDecode(
    Uint8List bytes, {
    required int sequence,
  }) {
    if (bytes.length != payloadBytes ||
        sequence <= 0 ||
        sequence > HomeStateNotification.maxSequence ||
        (bytes[0] != version &&
            bytes[0] != promotionVersion &&
            bytes[0] != promotionSelectionVersion) ||
        bytes[1] >= HomeStateAction.values.length ||
        bytes[2] > 2 ||
        bytes[3] > 3 ||
        bytes[4] > 2 ||
        bytes[5] > 2 ||
        (bytes[7] & 0x80) != 0 ||
        ((bytes[7] >> 3) & 0x03) > 2 ||
        ((bytes[8] >> 4) & 0x03) > 2) {
      return null;
    }
    final clockSetup = bytes[10] & 0x03;
    final promotionCode = (bytes[10] >> 2) & 0x07;
    final promotionReveal = (bytes[10] & 0x20) != 0;
    final promotionAutomatic = (bytes[10] & 0x40) != 0;
    final choosingPromotion = bytes[0] == promotionSelectionVersion;
    if (clockSetup > 2 ||
        (bytes[0] == version && bytes[10] != clockSetup) ||
        (bytes[0] != version &&
            ((bytes[10] & 0x80) != 0 ||
                promotionCode < 1 ||
                promotionCode > 4)) ||
        (choosingPromotion && (promotionReveal || promotionAutomatic)) ||
        (promotionAutomatic && (!promotionReveal || promotionCode != 1))) {
      return null;
    }
    final position = _tryDecodePosition(bytes.sublist(11, 54), bytes[2]);
    if (position == null) return null;

    final action = HomeStateAction.values[bytes[1]];
    final variant = switch (bytes[2]) {
      0 => 'undecided',
      1 => 'classic',
      2 => 'duck',
      _ => null,
    };
    final outcome = switch (bytes[3]) {
      0 => 'none',
      1 => 'king_captured',
      2 => 'fowled',
      3 => 'resignation',
      _ => null,
    };
    final winner = _winner(bytes[4]);
    final clockWinner = _winner(bytes[5]);
    if (variant == null ||
        outcome == null ||
        winner == null ||
        clockWinner == null ||
        ((outcome == 'none') != (winner == 'none')) ||
        (clockWinner != 'none' && outcome != 'none')) {
      return null;
    }

    final flags0 = bytes[6];
    final flags1 = bytes[7];
    final flags2 = bytes[8];
    final flags3 = bytes[9];
    final kingQueenSwapPromptActive = (flags0 & 0x80) != 0;
    if (kingQueenSwapPromptActive &&
        (action != HomeStateAction.correction ||
            (flags0 & 0x04) != 0 ||
            outcome != 'none' ||
            clockWinner != 'none')) {
      return null;
    }
    final clockValid = (flags0 & 0x40) != 0;
    final physicalValid = (flags1 & 0x20) != 0;
    final captureValid = (flags1 & 0x40) != 0;
    final duckOwnerCode = flags2 & 0x03;
    final whiteLastValid = (flags2 & 0x04) != 0;
    final blackLastValid = (flags2 & 0x08) != 0;
    final recentCode = (flags2 >> 4) & 0x03;
    final cpuValid = (flags2 & 0x40) != 0;
    // Without CPU thinking the board uses the otherwise idle elapsed slot for
    // its Classic result. Older firmware leaves this slot zero.
    final classicResult = cpuValid ? 0 : bytes[71];
    if (classicResult != 0 &&
        (classicResult > 3 ||
            variant != 'classic' ||
            action != HomeStateAction.idle ||
            outcome != 'none' ||
            clockWinner != 'none')) {
      return null;
    }
    final assistanceValid = (flags2 & 0x80) != 0;
    final timerSetting = (flags3 >> 5) & 0x07;
    if (timerSetting > 6 ||
        (captureValid && !physicalValid) ||
        !_validSquareOrAbsent(bytes[62], physicalValid) ||
        !_validSquareOrAbsent(bytes[63], physicalValid) ||
        !_validSquareOrAbsent(bytes[64], captureValid) ||
        !_validSquareOrAbsent(bytes[65], duckOwnerCode != 0) ||
        !_validSquareOrAbsent(bytes[66], duckOwnerCode >= 2) ||
        !_validSquareOrAbsent(bytes[67], whiteLastValid) ||
        !_validSquareOrAbsent(bytes[68], whiteLastValid) ||
        !_validSquareOrAbsent(bytes[69], blackLastValid) ||
        !_validSquareOrAbsent(bytes[70], blackLastValid)) {
      return null;
    }

    if (choosingPromotion &&
        (action != HomeStateAction.promote || !physicalValid)) {
      return null;
    }
    final physicalMove = physicalValid
        ? HomePhysicalMovePrompt(
            fromSquare: bytes[62],
            toSquare: bytes[63],
            captureSquare: captureValid ? bytes[64] : null,
            promotionSelection: choosingPromotion
                ? ' qrbn'[promotionCode]
                : null,
          )
        : null;
    if (physicalMove != null &&
        (action == HomeStateAction.promote ||
            action == HomeStateAction.correction)) {
      if (!physicalMove.isPromotionFor(position.position) ||
          (flags0 & 0xbc) != 0 ||
          duckOwnerCode != 0 ||
          cpuValid ||
          outcome != 'none' ||
          clockWinner != 'none') {
        return null;
      }
    }
    final duckOwner = switch (duckOwnerCode) {
      0 => null,
      1 => HomeDuckRelocationOwner.human,
      2 => HomeDuckRelocationOwner.cpu,
      3 => HomeDuckRelocationOwner.neutral,
      _ => null,
    };
    final duckRelocation = duckOwner == null
        ? null
        : HomeDuckRelocationPrompt(
            owner: duckOwner,
            fromSquare: bytes[65],
            toSquare: duckOwner == HomeDuckRelocationOwner.human
                ? null
                : bytes[66],
          );
    final whiteLast = whiteLastValid
        ? HomeMoveSquares(fromSquare: bytes[67], toSquare: bytes[68])
        : null;
    final blackLast = blackLastValid
        ? HomeMoveSquares(fromSquare: bytes[69], toSquare: bytes[70])
        : null;
    final mostRecent = switch (recentCode) {
      0 => null,
      1 when whiteLast != null => HomeSide.white,
      2 when blackLast != null => HomeSide.black,
      _ => null,
    };
    if (recentCode != 0 && mostRecent == null) return null;
    HomePromotionConfirmation? promotion;
    if (bytes[0] == promotionVersion) {
      final white = mostRecent == HomeSide.white;
      final move = white ? whiteLast : blackLast;
      final staged =
          variant == 'duck' && duckOwner == HomeDuckRelocationOwner.human;
      final suffix = ' qrbn'[promotionCode];
      final piece = white ? suffix.toUpperCase() : suffix;
      if (mostRecent == null ||
          move == null ||
          move.toSquare ~/ 8 != (white ? 0 : 7) ||
          move.fromSquare ~/ 8 != (white ? 1 : 6) ||
          (move.fromSquare % 8 - move.toSquare % 8).abs() > 1 ||
          (!promotionReveal && !staged) ||
          (staged
              ? position.position.pieces[move.fromSquare] != (white ? 'P' : 'p')
              : position.position.pieces[move.toSquare] != piece)) {
        return null;
      }
      promotion = (
        from: move.fromSquare,
        to: move.toSquare,
        piece: piece,
        reveal: promotionReveal,
        automatic: promotionAutomatic,
      );
    }

    final data = ByteData.sublistView(bytes);
    final occupancy = _uint64(data, 54);
    final squareMask = _uint64(data, 92);
    final legalGuide =
        assistanceValid && bytes[85] == HomeAssistancePresentation.legal.index;
    final landingRemoval = !legalGuide && squareMask != BigInt.zero
        ? squareMask
        : null;
    if (landingRemoval != null &&
        (action != HomeStateAction.correction ||
            variant != 'duck' ||
            (flags0 & 0x84) != 0 ||
            physicalValid ||
            duckOwnerCode != 0 ||
            cpuValid ||
            outcome != 'none' ||
            clockWinner != 'none' ||
            (landingRemoval & ~occupancy) != BigInt.zero)) {
      return null;
    }
    final assistance = assistanceValid
        ? _tryDecodeAssistance(
            bytes,
            legalDestinations: legalGuide ? squareMask : BigInt.zero,
            homeVariant: bytes[2],
          )
        : null;
    if (assistanceValid && assistance == null) return null;
    final cpuThinking = cpuValid
        ? HomeCpuThinking(
            side: (flags3 & 0x01) != 0 ? HomeSide.black : HomeSide.white,
            elapsedHalfSeconds: data.getUint32(71, Endian.little),
            remainingSeconds: data.getUint16(75, Endian.little) == 0
                ? null
                : data.getUint16(75, Endian.little),
          )
        : null;
    final clockStatus = clockValid
        ? HomeClockStatus(
            enabled: (flags3 & 0x02) != 0,
            running: (flags3 & 0x04) != 0,
            paused: (flags3 & 0x08) != 0,
            activeSide: (flags3 & 0x10) != 0 ? HomeSide.black : HomeSide.white,
            timerSetting: timerSetting,
            setupState: HomeClockSetupState.values[clockSetup],
            whiteRemainingSeconds: data.getUint32(77, Endian.little),
            blackRemainingSeconds: data.getUint32(81, Endian.little),
          )
        : null;
    if (clockStatus != null &&
        (!clockStatus.enabled &&
                (clockStatus.running ||
                    clockStatus.paused ||
                    clockStatus.whiteRemainingSeconds != 0 ||
                    clockStatus.blackRemainingSeconds != 0) ||
            clockStatus.paused && !clockStatus.running)) {
      return null;
    }

    final persistence = switch ((flags1 >> 3) & 0x03) {
      0 => null,
      1 => HomePersistenceDegradation.quarantinedRecord,
      2 => HomePersistenceDegradation.storageIo,
      _ => null,
    };
    final timeout = clockWinner != 'none';
    return HomeStateNotification(
      sequence: sequence,
      action: action,
      position: position.position,
      gameStatus: HomeGameStatus(
        variant: variant,
        duckSquare: position.duckSquare,
        duckMovePending:
            action == HomeStateAction.duck ||
            (action == HomeStateAction.correction && duckRelocation != null),
        outcome: switch (classicResult) {
          1 => 'checkmate',
          2 => 'stalemate',
          3 => 'draw',
          _ => timeout ? 'timeout' : outcome,
        },
        winner: classicResult == 1
            ? (position.position.sideToMove == HomeSide.white
                  ? 'black'
                  : 'white')
            : timeout
            ? clockWinner
            : winner,
      ),
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares: occupancy,
        placementMode: (flags0 & 0x04) != 0,
        rotated: (flags0 & 0x01) != 0,
        whiteOwner: (flags0 & 0x02) != 0 ? 'P2' : 'P1',
      ),
      freshSetup: (flags0 & 0x08) != 0,
      kingQueenSwapPromptActive: kingQueenSwapPromptActive,
      vexMoveFirst: (flags0 & 0x10) != 0,
      undoPending: (flags0 & 0x20) != 0,
      coachingMask: (flags1 & 0x04) != 0 ? flags1 & 0x03 : null,
      assistance: assistance,
      physicalMovePrompt: physicalMove,
      cpuThinking: cpuThinking,
      duckRelocationPrompt: duckRelocation,
      duckLandingRemovalMask: landingRemoval,
      lastMoves: whiteLast != null || blackLast != null
          ? HomeLastMoves(
              white: whiteLast,
              black: blackLast,
              mostRecent: mostRecent,
            )
          : null,
      promotionConfirmation: promotion,
      clockStatus: clockStatus,
      persistenceDegradation: persistence,
    );
  }

  static bool _validSquareOrAbsent(int value, bool present) =>
      present ? value < 64 : value == 0xff;

  static String? _winner(int value) => switch (value) {
    0 => 'none',
    1 => 'white',
    2 => 'black',
    _ => null,
  };

  static ({HomeChessPosition position, int? duckSquare})? _tryDecodePosition(
    Uint8List bytes,
    int homeVariant,
  ) {
    if (bytes.length != 43 || (bytes[32] & 0xe0) != 0) return null;
    const pieces = ' PNBRQKpnbrqk';
    final ranks = <String>[];
    for (var rank = 0; rank < 8; rank += 1) {
      var empty = 0;
      final row = StringBuffer();
      for (var file = 0; file < 8; file += 1) {
        final square = rank * 8 + file;
        final packed = bytes[square ~/ 2];
        final code = square.isEven ? packed & 0x0f : packed >> 4;
        if (code == 0) {
          empty += 1;
        } else {
          if (code >= pieces.length) return null;
          if (empty != 0) row.write(empty);
          empty = 0;
          row.write(pieces[code]);
        }
      }
      if (empty != 0) row.write(empty);
      ranks.add(row.toString());
    }
    final flags = bytes[32];
    final castles = StringBuffer();
    if ((flags & 0x02) != 0) castles.write('K');
    if ((flags & 0x04) != 0) castles.write('Q');
    if ((flags & 0x08) != 0) castles.write('k');
    if ((flags & 0x10) != 0) castles.write('q');
    final enPassant = bytes[33];
    if (enPassant != 0xff &&
        (enPassant >= 64 || (enPassant ~/ 8 != 2 && enPassant ~/ 8 != 5))) {
      return null;
    }
    final duck = bytes[42];
    if (homeVariant == 1 && duck != 0xff ||
        homeVariant == 2 && duck >= 64 ||
        homeVariant == 0 && duck != 0xff && duck >= 64) {
      return null;
    }
    final data = ByteData.sublistView(bytes);
    final fullmove = data.getUint32(38, Endian.little);
    if (fullmove == 0) return null;
    final ep = enPassant == 0xff
        ? '-'
        : '${String.fromCharCode(97 + enPassant % 8)}${8 - enPassant ~/ 8}';
    final fen =
        '${ranks.join('/')} ${(flags & 0x01) != 0 ? 'b' : 'w'} '
        '${castles.isEmpty ? '-' : castles} $ep '
        '${data.getUint32(34, Endian.little)} $fullmove';
    final position = HomeChessPosition.tryParse(fen);
    return position == null
        ? null
        : (position: position, duckSquare: duck == 0xff ? null : duck);
  }

  static HomeAssistanceState? _tryDecodeAssistance(
    Uint8List bytes, {
    required BigInt legalDestinations,
    required int homeVariant,
  }) {
    final presentation = bytes[85];
    final assistanceFlags = bytes[91];
    if (presentation >= HomeAssistancePresentation.values.length ||
        (assistanceFlags & 0xf8) != 0) {
      return null;
    }
    final requested = (assistanceFlags & 0x01) != 0;
    final decodedPresentation = HomeAssistancePresentation.values[presentation];
    final move = bytes[86] < 64 && bytes[87] < 64
        ? (from: bytes[86], to: bytes[87])
        : null;
    final duckMove = bytes[88] < 64 && bytes[89] < 64
        ? (from: bytes[88], to: bytes[89])
        : null;
    final legalSource = bytes[90] < 64 ? bytes[90] : null;
    final requiresRequest = switch (decodedPresentation) {
      HomeAssistancePresentation.suppressed ||
      HomeAssistancePresentation.off ||
      HomeAssistancePresentation.legal => false,
      _ => true,
    };
    final requiresMove = switch (decodedPresentation) {
      HomeAssistancePresentation.ready ||
      HomeAssistancePresentation.refreshing ||
      HomeAssistancePresentation.compoundDuck => true,
      _ => false,
    };
    final requiresDuckMove = switch (decodedPresentation) {
      HomeAssistancePresentation.compoundDuck ||
      HomeAssistancePresentation.duck => true,
      _ => false,
    };
    final legalDuckMove =
        decodedPresentation == HomeAssistancePresentation.legal &&
        (bytes[88] != 0xff || bytes[89] != 0xff);
    final legalMoveValid =
        move == null ||
        requested &&
            move.from == legalSource &&
            (legalDestinations & (BigInt.one << move.to)) != BigInt.zero;
    if (requiresRequest && !requested ||
        decodedPresentation == HomeAssistancePresentation.off && requested ||
        requiresMove && (move == null || move.from == move.to) ||
        decodedPresentation == HomeAssistancePresentation.refreshing &&
            homeVariant == 2 ||
        (requiresDuckMove || legalDuckMove) &&
            (homeVariant != 2 ||
                duckMove == null ||
                duckMove.from == duckMove.to) ||
        legalDuckMove && (!requested || move == null) ||
        decodedPresentation == HomeAssistancePresentation.legal &&
            (legalSource == null ||
                legalDestinations == BigInt.zero ||
                (legalDestinations & (BigInt.one << legalSource)) !=
                    BigInt.zero ||
                !legalMoveValid)) {
      return null;
    }
    return HomeAssistanceState(
      requested: requested,
      manualAvailable: (assistanceFlags & 0x02) != 0,
      coachingOwned: (assistanceFlags & 0x04) != 0,
      presentation: decodedPresentation,
      move: move,
      duckMove: duckMove,
      legalSource: legalSource,
      legalDestinations: decodedPresentation == HomeAssistancePresentation.legal
          ? legalDestinations
          : null,
    );
  }

  static BigInt _uint64(ByteData data, int offset) =>
      (BigInt.from(data.getUint32(offset + 4, Endian.little)) << 32) |
      BigInt.from(data.getUint32(offset, Endian.little));
}

final class HomeStateWireAssembler {
  int? _revision;
  final Map<int, Uint8List> _fragments = <int, Uint8List>{};
  Uint8List? _lastPayload;
  bool _currentPublished = false;

  bool get isIncomplete => _revision != null && !_currentPublished;

  HomeStateNotification? accept({
    required int code,
    required int revision,
    required Uint8List body,
    required int sequence,
  }) {
    if (code < HomeStateWireCodec.firstStateCode ||
        code > HomeStateWireCodec.lastStateCode ||
        revision <= 0 ||
        body.length != HomeStateWireCodec.fragmentBytes) {
      return null;
    }
    if (_revision != revision) {
      _revision = revision;
      _fragments.clear();
    }
    _currentPublished = false;
    _fragments[code] = Uint8List.fromList(body);
    if (_fragments.length != HomeStateWireCodec.fragmentCount) return null;
    final payload = Uint8List(HomeStateWireCodec.payloadBytes);
    for (var index = 0; index < HomeStateWireCodec.fragmentCount; index += 1) {
      payload.setRange(
        index * HomeStateWireCodec.fragmentBytes,
        (index + 1) * HomeStateWireCodec.fragmentBytes,
        _fragments[HomeStateWireCodec.firstStateCode + index]!,
      );
    }
    if (_lastPayload != null && _equal(_lastPayload!, payload)) {
      _currentPublished = true;
      return null;
    }
    final decoded = HomeStateWireCodec.tryDecode(payload, sequence: sequence);
    if (decoded == null) return null;
    _lastPayload = payload;
    _currentPublished = true;
    return decoded;
  }

  void clear() {
    _revision = null;
    _fragments.clear();
    _lastPayload = null;
    _currentPublished = false;
  }

  static bool _equal(Uint8List left, Uint8List right) {
    if (left.length != right.length) return false;
    for (var index = 0; index < left.length; index += 1) {
      if (left[index] != right[index]) return false;
    }
    return true;
  }
}
