import 'game_archive.dart';
import 'local_player_profile.dart';

class LocalPlayerResultCounts {
  const LocalPlayerResultCounts({
    required this.completedGames,
    required this.wins,
    required this.draws,
    required this.losses,
  });

  final int completedGames;
  final int wins;
  final int draws;
  final int losses;
}

class LocalPlayerStatistics {
  const LocalPlayerStatistics({
    required this.playerId,
    required this.allVariants,
    required this.classic,
    required this.duck,
    required this.excludedIncompleteOrPartialGames,
    required this.excludedSelfPlayGames,
  });

  final String playerId;

  /// The explicitly combined Classic and Duck result. User-facing views must
  /// label this as an all-variant total and keep both variant rows available.
  final LocalPlayerResultCounts allVariants;
  final LocalPlayerResultCounts classic;
  final LocalPlayerResultCounts duck;

  /// Games carrying this profile identity that are not both complete and
  /// terminal. They remain in the library but cannot support result stats.
  final int excludedIncompleteOrPartialGames;

  /// Games attributing both chess sides to this same resolved profile. They are
  /// intentionally omitted rather than inventing a personal win or loss.
  final int excludedSelfPlayGames;
}

class LocalPlayerStatisticsReport {
  LocalPlayerStatisticsReport._({
    required Map<String, LocalPlayerStatistics> byPlayerId,
    required this.eligibleCompletedGames,
    required this.excludedIncompleteOrPartialGames,
    required this.gamesWithUnknownOrUnlinkedPlayers,
  }) : byPlayerId = Map<String, LocalPlayerStatistics>.unmodifiable(byPlayerId);

  factory LocalPlayerStatisticsReport.fromArchives({
    required LocalPlayerDirectory directory,
    required Iterable<GameArchive> games,
  }) {
    directory.validate();
    final Map<String, _MutableLocalPlayerStatistics> builders =
        <String, _MutableLocalPlayerStatistics>{
          for (final LocalPlayerProfile profile in directory.profiles)
            profile.id: _MutableLocalPlayerStatistics(profile.id),
        };

    int eligibleCompletedGames = 0;
    int excludedIncompleteOrPartialGames = 0;
    int gamesWithUnknownOrUnlinkedPlayers = 0;
    for (final GameArchive game in games) {
      final String? whitePlayerId = _resolvedProfileId(
        game.whitePlayer,
        directory,
      );
      final String? blackPlayerId = _resolvedProfileId(
        game.blackPlayer,
        directory,
      );
      if (_isUnknownOrUnlinked(game.whitePlayer, whitePlayerId) ||
          _isUnknownOrUnlinked(game.blackPlayer, blackPlayerId)) {
        gamesWithUnknownOrUnlinkedPlayers += 1;
      }

      final bool eligible =
          game.captureQuality == GameCaptureQuality.complete &&
          game.status == GameArchiveStatus.completed;
      if (!eligible) {
        excludedIncompleteOrPartialGames += 1;
        for (final String playerId in <String?>{
          whitePlayerId,
          blackPlayerId,
        }.whereType<String>()) {
          builders[playerId]?.excludedIncompleteOrPartialGames += 1;
        }
        continue;
      }
      eligibleCompletedGames += 1;

      if (whitePlayerId != null && whitePlayerId == blackPlayerId) {
        builders[whitePlayerId]?.excludedSelfPlayGames += 1;
        continue;
      }
      if (whitePlayerId != null) {
        builders[whitePlayerId]?.record(
          variant: game.variant,
          playerSide: GameWinner.white,
          winner: game.winner,
        );
      }
      if (blackPlayerId != null) {
        builders[blackPlayerId]?.record(
          variant: game.variant,
          playerSide: GameWinner.black,
          winner: game.winner,
        );
      }
    }

    return LocalPlayerStatisticsReport._(
      byPlayerId: <String, LocalPlayerStatistics>{
        for (final MapEntry<String, _MutableLocalPlayerStatistics> entry
            in builders.entries)
          entry.key: entry.value.freeze(),
      },
      eligibleCompletedGames: eligibleCompletedGames,
      excludedIncompleteOrPartialGames: excludedIncompleteOrPartialGames,
      gamesWithUnknownOrUnlinkedPlayers: gamesWithUnknownOrUnlinkedPlayers,
    );
  }

  final Map<String, LocalPlayerStatistics> byPlayerId;

  /// Complete terminal archives, whether or not their players are attributable.
  final int eligibleCompletedGames;

  /// Active, unfinished, or partial archives omitted from every result total.
  final int excludedIncompleteOrPartialGames;

  /// Archives with at least one legacy, board-only, deleted, or foreign player.
  /// Intentional Guest attribution is not included in this diagnostic.
  final int gamesWithUnknownOrUnlinkedPlayers;
}

String? _resolvedProfileId(
  ArchivedPlayerAttribution attribution,
  LocalPlayerDirectory directory,
) {
  if (attribution.kind != ArchivedPlayerKind.profile ||
      attribution.playerId == null) {
    return null;
  }
  return directory.resolveProfileId(attribution.playerId!);
}

bool _isUnknownOrUnlinked(
  ArchivedPlayerAttribution attribution,
  String? resolvedPlayerId,
) {
  return attribution.kind == ArchivedPlayerKind.unknown ||
      (attribution.kind == ArchivedPlayerKind.profile &&
          resolvedPlayerId == null);
}

class _MutableLocalPlayerStatistics {
  _MutableLocalPlayerStatistics(this.playerId);

  final String playerId;
  final _MutableResultCounts classic = _MutableResultCounts();
  final _MutableResultCounts duck = _MutableResultCounts();
  int excludedIncompleteOrPartialGames = 0;
  int excludedSelfPlayGames = 0;

  void record({
    required GameVariant variant,
    required GameWinner playerSide,
    required GameWinner winner,
  }) {
    final _MutableResultCounts counts = variant == GameVariant.classic
        ? classic
        : duck;
    counts.completedGames += 1;
    if (winner == GameWinner.draw) {
      counts.draws += 1;
    } else if (winner == playerSide) {
      counts.wins += 1;
    } else {
      counts.losses += 1;
    }
  }

  LocalPlayerStatistics freeze() {
    final LocalPlayerResultCounts classicCounts = classic.freeze();
    final LocalPlayerResultCounts duckCounts = duck.freeze();
    return LocalPlayerStatistics(
      playerId: playerId,
      allVariants: LocalPlayerResultCounts(
        completedGames:
            classicCounts.completedGames + duckCounts.completedGames,
        wins: classicCounts.wins + duckCounts.wins,
        draws: classicCounts.draws + duckCounts.draws,
        losses: classicCounts.losses + duckCounts.losses,
      ),
      classic: classicCounts,
      duck: duckCounts,
      excludedIncompleteOrPartialGames: excludedIncompleteOrPartialGames,
      excludedSelfPlayGames: excludedSelfPlayGames,
    );
  }
}

class _MutableResultCounts {
  int completedGames = 0;
  int wins = 0;
  int draws = 0;
  int losses = 0;

  LocalPlayerResultCounts freeze() {
    return LocalPlayerResultCounts(
      completedGames: completedGames,
      wins: wins,
      draws: draws,
      losses: losses,
    );
  }
}
