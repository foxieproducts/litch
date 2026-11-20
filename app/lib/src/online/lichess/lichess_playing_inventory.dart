import 'lichess_board_api_events.dart';
import 'lichess_http.dart';

/// A bounded point-in-time account-game listing, never attachment authority.
final class LichessPlayingInventory {
  const LichessPlayingInventory({
    required this.games,
    required this.nbMyTurn,
    required this.potentiallyCapped,
    required this.turnCountUncertain,
  });

  final List<LichessPlayingGame> games;
  final int nbMyTurn;
  final bool potentiallyCapped;
  final bool turnCountUncertain;

  bool get isUncertain => potentiallyCapped || turnCountUncertain;
}

final class LichessPlayingGame {
  const LichessPlayingGame({
    required this.gameId,
    required this.color,
    required this.variantKey,
    required this.speed,
    required this.rated,
    required this.isMyTurn,
    required this.opponent,
  });

  final String gameId;
  final LichessPlayerColor color;
  final String variantKey;
  final String speed;
  final bool rated;
  final bool isMyTurn;
  final LichessPlayingOpponent opponent;
}

final class LichessPlayingOpponent {
  const LichessPlayingOpponent({required this.player, required this.aiLevel});

  final LichessGamePlayer player;
  final int? aiLevel;
}

LichessPlayingInventory decodeLichessPlayingInventory(Object? source) {
  if (source is! Map<String, Object?>) {
    _invalid();
  }
  final Object? gamesValue = source['nowPlaying'];
  final Object? turnCountValue = source['nbMyTurn'];
  if (gamesValue is! List<Object?> ||
      gamesValue.length > _maximumGames ||
      turnCountValue is! int ||
      turnCountValue < 0 ||
      turnCountValue > _maximumTurnCount) {
    _invalid();
  }

  final Set<String> ids = <String>{};
  final List<LichessPlayingGame> games = <LichessPlayingGame>[];
  int listedTurns = 0;
  for (final Object? value in gamesValue) {
    if (value is! Map<String, Object?>) {
      _invalid();
    }
    final LichessPlayingGame game = _decodeGame(value);
    if (!ids.add(game.gameId)) {
      _invalid();
    }
    if (game.isMyTurn) {
      listedTurns += 1;
    }
    games.add(game);
  }
  return LichessPlayingInventory(
    games: List<LichessPlayingGame>.unmodifiable(games),
    nbMyTurn: turnCountValue,
    potentiallyCapped: games.length == _maximumGames,
    turnCountUncertain: listedTurns != turnCountValue,
  );
}

LichessPlayingGame _decodeGame(Map<String, Object?> json) {
  final String gameId = _boundedString(json['gameId'], maximumLength: 64);
  final LichessPlayerColor? color = LichessPlayerColor.fromWire(json['color']);
  if (color == null) {
    _invalid();
  }
  final Object? variantValue = json['variant'];
  if (variantValue is! Map<String, Object?>) {
    _invalid();
  }
  final Object? opponentValue = json['opponent'];
  if (opponentValue is! Map<String, Object?>) {
    _invalid();
  }
  final Object? aiValue = opponentValue['ai'];
  if (aiValue != null && (aiValue is! int || aiValue < 0 || aiValue > 9)) {
    _invalid();
  }
  final Object? rated = json['rated'];
  final Object? isMyTurn = json['isMyTurn'];
  if (rated is! bool || isMyTurn is! bool) {
    _invalid();
  }
  return LichessPlayingGame(
    gameId: gameId,
    color: color,
    variantKey: _boundedString(variantValue['key'], maximumLength: 64),
    speed: _boundedString(json['speed'], maximumLength: 32),
    rated: rated,
    isMyTurn: isMyTurn,
    opponent: LichessPlayingOpponent(
      player: LichessGamePlayer(
        id: _boundedString(opponentValue['id'], maximumLength: 64),
        name: _boundedString(opponentValue['username'], maximumLength: 128),
      ),
      aiLevel: aiValue as int?,
    ),
  );
}

String _boundedString(Object? value, {required int maximumLength}) {
  if (value is! String || value.isEmpty || value.length > maximumLength) {
    _invalid();
  }
  return value;
}

Never _invalid() {
  throw const LichessApiException(
    LichessApiFailureKind.invalidResponse,
    'Lichess returned an unusable current-game inventory.',
  );
}

const int _maximumGames = 50;
const int _maximumTurnCount = 1000000;
