import '../../l10n/generated/app_localizations.dart';
import '../models/home_assistance_state.dart';
import '../models/home_board_snapshot.dart';
import '../models/litch_piece_color.dart';
import '../models/support_report.dart';
import '../services/ota/ota_post_reboot_verifier.dart';

Map<int, String> localizedClockPresets(AppLocalizations strings) =>
    <int, String>{
      0: strings.clockOffLabel,
      1: strings.minutesValue(1),
      2: '3 + 2',
      3: strings.minutesValue(5),
      4: strings.minutesValue(10),
      5: '15 + 10',
      6: strings.minutesValue(30),
    };

String localizedPieceColor(AppLocalizations strings, int wheel) =>
    switch (litchClosestPieceColorChoice(wheel).wheel) {
      0 => strings.colorRed,
      43 => strings.colorGold,
      85 => strings.colorGreen,
      128 => strings.colorTeal,
      170 => strings.colorBlue,
      _ => strings.colorViolet,
    };

String localizedPieceName(AppLocalizations strings, String piece) {
  // Logical players keep their chess sides when physical ends are swapped.
  final player = piece == piece.toUpperCase() ? 'P1' : 'P2';
  return switch (piece.toLowerCase()) {
    'k' => strings.playerKing(player),
    'q' => strings.playerQueen(player),
    'r' => strings.playerRook(player),
    'b' => strings.playerBishop(player),
    'n' => strings.playerKnight(player),
    _ => strings.playerPawn(player),
  };
}

({String heading, String title}) localizedHomeResult(
  AppLocalizations strings,
  HomeGameStatus status, {
  required String Function(HomeSide) playerForSide,
}) {
  final outcome = status.outcome.toLowerCase();
  final heading = outcome == 'checkmate'
      ? strings.gameCheckmate
      : strings.gameComplete;
  final winner = status.winner.toLowerCase();
  if (winner == 'white' || winner == 'black') {
    final player = playerForSide(
      winner == 'white' ? HomeSide.white : HomeSide.black,
    );
    return (
      heading: heading,
      title: outcome == 'timeout'
          ? strings.winsOnTime(player)
          : strings.playerWins(player),
    );
  }
  final title = switch (outcome) {
    'draw' => strings.gameDraw,
    'stalemate' => strings.gameStalemate,
    'checkmate' => strings.gameCheckmate,
    'kingcaptured' || 'king_captured' => strings.gameKingCaptured,
    _ =>
      outcome.isEmpty
          ? ''
          : '${outcome[0].toUpperCase()}${outcome.substring(1).replaceAll('_', ' ')}',
  };
  return (heading: heading, title: title);
}

String localizedPersistence(
  AppLocalizations strings,
  HomePersistenceDegradation persistence, {
  bool sessionOnly = false,
}) => sessionOnly && persistence == HomePersistenceDegradation.storageIo
    ? strings.savingSessionOnly
    : persistence == HomePersistenceDegradation.quarantinedRecord
    ? strings.savingDamagedRecord
    : strings.savingUnconfirmed;

String? localizedAssistance(
  AppLocalizations strings,
  HomeBoardSnapshot snapshot,
) {
  final state = snapshot.visibleAssistance;
  if (state == null) return null;
  final capture = snapshot.captureRemovalSquare;
  if (capture != null && state.move != null) {
    final instruction = strings.captureMove(
      algebraicSquare(capture),
      algebraicSquare(state.move!.from),
      algebraicSquare(state.move!.to),
    );
    return state.presentation == HomeAssistancePresentation.compoundDuck
        ? '$instruction ${strings.hintDuckMove(algebraicSquare(state.duckMove!.from), algebraicSquare(state.duckMove!.to))}'
        : instruction;
  }
  return switch (state.presentation) {
    HomeAssistancePresentation.ready ||
    HomeAssistancePresentation.refreshing => strings.hintMove(
      algebraicSquare(state.move!.from),
      algebraicSquare(state.move!.to),
    ),
    HomeAssistancePresentation.compoundDuck => strings.hintCompoundDuck(
      algebraicSquare(state.move!.from),
      algebraicSquare(state.move!.to),
      algebraicSquare(state.duckMove!.from),
      algebraicSquare(state.duckMove!.to),
    ),
    HomeAssistancePresentation.duck => strings.hintDuckMove(
      algebraicSquare(state.duckMove!.from),
      algebraicSquare(state.duckMove!.to),
    ),
    HomeAssistancePresentation.legal => strings.legalFromMarked(
      algebraicSquare(state.legalSource!),
    ),
    _ => null,
  };
}

String? localizedCorrection(
  AppLocalizations strings,
  HomeBoardSnapshot snapshot,
) {
  if (snapshot.isExpectedBoardInteraction ||
      snapshot.mismatchedSquares.length != 1) {
    return null;
  }
  final square = snapshot.mismatchedSquares.single;
  if (snapshot.missingExpectedSquares.contains(square)) {
    final piece = snapshot.physicalPieceAt(square);
    if (piece != null) {
      final name = localizedPieceName(strings, piece);
      return strings.pieceExpected(
        '${name[0].toUpperCase()}${name.substring(1)}',
        algebraicSquare(square),
      );
    }
    if (snapshot.gameStatus?.duckSquare == square) {
      return strings.duckExpected(algebraicSquare(square));
    }
    if (snapshot.pendingPromotionPrompt?.toSquare == square) {
      return strings.promotionPlacePiece(algebraicSquare(square));
    }
  }
  return snapshot.unexpectedOccupiedSquares.contains(square)
      ? strings.unexpectedSquare(algebraicSquare(square))
      : null;
}

bool localizedUsesEnglish(AppLocalizations strings) =>
    strings.localeName == 'en' || strings.localeName == 'en_GB';

String localizedUpdateDetail(
  AppLocalizations strings,
  String message,
  String original,
) => localizedUsesEnglish(strings)
    ? original
    : '$message\n\n${strings.originalDiagnostic(original)}';

String localizedVerification(
  AppLocalizations strings,
  OtaPostRebootVerification result,
) => result is OtaPostRebootVerified
    ? strings.updateVerified
    : localizedUpdateDetail(
        strings,
        result is OtaPostRebootDifferentImage
            ? strings.updateFailedRecovery
            : result is OtaPostRebootMismatch
            ? strings.updateMismatchRecovery
            : strings.updatePendingRecovery,
        result.message,
      );

/// Translate app-owned questions while preserving the existing bounded report's
/// diagnostic suffix exactly. A dense report uses a shorter question prompt;
/// locale expansion never discards evidence or exceeds the email-body limit.
String localizedSupportBody(
  AppLocalizations strings,
  SupportReportDraft draft,
) {
  if (draft.diagnostics == null) return draft.body;
  final questions = <String>[
    strings.supportInstructions,
    '',
    strings.supportGoalQuestion,
    '',
    strings.supportExpectedQuestion,
    '',
    strings.supportReproduceQuestion,
    '',
    strings.supportPowerQuestion,
    '',
    strings.supportLightsQuestion,
    '',
    strings.supportWhenQuestion,
    '',
    strings.supportOtherQuestion,
    '',
    strings.supportDiagnosticHeader,
  ].join('\n');
  final full = '$questions\n${draft.diagnostics}';
  return full.length <= SupportReportDraft.maxBodyCharacters
      ? full
      : '${strings.supportCompactInstructions}\n\n${strings.supportDiagnosticHeader}\n${draft.diagnostics}';
}
