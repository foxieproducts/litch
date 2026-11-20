import 'package:flutter/widgets.dart';

import '../localization/litch_strings.dart';
import '../models/game_archive.dart';

String gameArchiveVariantLabel(BuildContext context, GameVariant variant) =>
    variant == GameVariant.classic
    ? litchStrings(context).gameLibraryClassic
    : litchStrings(context).gameLibraryDuck;

String gameArchiveResultLabel(BuildContext context, GameArchive archive) {
  final strings = litchStrings(context);
  final provider = archive.provider;
  if (provider != null) {
    final String result = switch (provider.rawStatus) {
      'created' || 'started' => strings.gameLibraryInProgress,
      'mate' => strings.gameCheckmate,
      'resign' => strings.gameLibraryLichessResignation,
      'stalemate' => strings.gameLibraryLichessStalemate,
      'draw' => strings.gameDraw,
      'insufficientMaterialClaim' =>
        strings.gameLibraryLichessInsufficientMaterial,
      'timeout' => strings.gameLibraryLichessTimeout,
      'outoftime' => strings.gameLibraryLichessOutOfTime,
      'aborted' => strings.gameLibraryLichessAborted,
      'noStart' => strings.gameLibraryLichessNoStart,
      'cheat' => strings.gameLibraryLichessFairPlay,
      'unknownFinish' => strings.gameLibraryLichessUnknownFinish,
      'variantEnd' => strings.gameLibraryLichessVariantEnd,
      _ => strings.gameLibraryCompleted,
    };
    final String? winner = switch (archive.winner) {
      GameWinner.white => strings.gameLibraryWhiteWon,
      GameWinner.black => strings.gameLibraryBlackWon,
      GameWinner.none || GameWinner.draw => null,
    };
    return winner == null
        ? result
        : strings.gameLibraryResultWithWinner(result, winner);
  }
  if (archive.status == GameArchiveStatus.active) {
    return strings.gameLibraryInProgress;
  }
  if (archive.status == GameArchiveStatus.unfinished) {
    return strings.gameLibraryUnfinished;
  }
  return switch (archive.winner) {
    GameWinner.white => strings.gameLibraryWhiteWon,
    GameWinner.black => strings.gameLibraryBlackWon,
    GameWinner.draw => strings.gameDraw,
    GameWinner.none => strings.gameLibraryCompleted,
  };
}
