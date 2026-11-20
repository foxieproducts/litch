import 'lichess_protocol.dart';

enum LichessKnownGameStatus {
  created('created', false),
  started('started', false),
  aborted('aborted', true),
  mate('mate', true),
  resign('resign', true),
  stalemate('stalemate', true),
  timeout('timeout', true),
  draw('draw', true),
  outOfTime('outoftime', true),
  cheat('cheat', true),
  noStart('noStart', true),
  unknownFinish('unknownFinish', true),
  insufficientMaterialClaim('insufficientMaterialClaim', true),
  variantEnd('variantEnd', true);

  const LichessKnownGameStatus(this.wireValue, this.isTerminal);

  final String wireValue;
  final bool isTerminal;

  static LichessKnownGameStatus? fromWire(String value) {
    for (final LichessKnownGameStatus status in values) {
      if (status.wireValue == value) {
        return status;
      }
    }
    return null;
  }
}

final class LichessGameStatus {
  const LichessGameStatus({required this.wireValue, required this.known});

  factory LichessGameStatus.parse(Object? value) {
    final String wireValue = _requiredBoundedString(
      value,
      field: 'status',
      maximumLength: 64,
    );
    return LichessGameStatus(
      wireValue: wireValue,
      known: LichessKnownGameStatus.fromWire(wireValue),
    );
  }

  final String wireValue;
  final LichessKnownGameStatus? known;

  bool get isTerminalOrUnknown => known?.isTerminal ?? true;
}

enum LichessPlayerColor {
  white('white'),
  black('black');

  const LichessPlayerColor(this.wireValue);

  final String wireValue;

  static LichessPlayerColor? fromWire(Object? value) {
    for (final LichessPlayerColor color in values) {
      if (color.wireValue == value) {
        return color;
      }
    }
    return null;
  }
}

sealed class LichessGameStreamEvent {
  const LichessGameStreamEvent(this.type);

  final String type;
}

sealed class LichessAccountEvent {
  const LichessAccountEvent(this.type);

  final String type;
}

final class LichessAccountGameEvent extends LichessAccountEvent {
  const LichessAccountGameEvent({
    required String type,
    required this.gameId,
    required this.fullId,
  }) : super(type);

  final String gameId;
  final String fullId;
}

final class LichessAccountChallengeEvent extends LichessAccountEvent {
  const LichessAccountChallengeEvent({
    required String type,
    required this.challengeId,
  }) : super(type);

  final String challengeId;
}

final class LichessUnknownAccountEvent extends LichessAccountEvent {
  const LichessUnknownAccountEvent(super.type);
}

LichessAccountEvent decodeLichessAccountEvent(Map<String, Object?> json) {
  final String type = _requiredBoundedString(
    _required(json, 'type'),
    field: 'type',
    maximumLength: 64,
  );
  if (type == 'gameStart' || type == 'gameFinish') {
    final Map<String, Object?> game = _requiredObject(json, 'game');
    return LichessAccountGameEvent(
      type: type,
      gameId: _requiredBoundedString(
        _required(game, 'gameId'),
        field: 'game.gameId',
        maximumLength: 64,
      ),
      fullId: _requiredBoundedString(
        _required(game, 'fullId'),
        field: 'game.fullId',
        maximumLength: 128,
      ),
    );
  }
  if (type == 'challenge' ||
      type == 'challengeCanceled' ||
      type == 'challengeDeclined') {
    final Map<String, Object?> challenge = _requiredObject(json, 'challenge');
    return LichessAccountChallengeEvent(
      type: type,
      challengeId: _requiredBoundedString(
        _required(challenge, 'id'),
        field: 'challenge.id',
        maximumLength: 64,
      ),
    );
  }
  return LichessUnknownAccountEvent(type);
}

final class LichessGameStateEvent extends LichessGameStreamEvent {
  const LichessGameStateEvent({
    required this.moves,
    required this.whiteTimeMs,
    required this.blackTimeMs,
    required this.whiteIncrementMs,
    required this.blackIncrementMs,
    required this.status,
    required this.winner,
    required this.whiteOffersDraw,
    required this.blackOffersDraw,
    required this.whiteOffersTakeback,
    required this.blackOffersTakeback,
    required this.expiration,
  }) : super('gameState');

  factory LichessGameStateEvent.fromJson(Map<String, Object?> json) {
    _requireType(json, 'gameState');
    final String moveSource = _requiredString(json, 'moves');
    final List<String> moves = moveSource.trim().isEmpty
        ? const <String>[]
        : moveSource.trim().split(RegExp(r'\s+'));
    for (final String move in moves) {
      if (!_uciMove.hasMatch(move)) {
        _invalid('moves');
      }
    }

    final Object? winnerValue = json['winner'];
    final LichessPlayerColor? winner = winnerValue == null
        ? null
        : LichessPlayerColor.fromWire(winnerValue);
    if (winnerValue != null && winner == null) {
      _invalid('winner');
    }

    return LichessGameStateEvent(
      moves: List<String>.unmodifiable(moves),
      whiteTimeMs: _requiredNonNegativeInt(json, 'wtime'),
      blackTimeMs: _requiredNonNegativeInt(json, 'btime'),
      whiteIncrementMs: _requiredNonNegativeInt(json, 'winc'),
      blackIncrementMs: _requiredNonNegativeInt(json, 'binc'),
      status: LichessGameStatus.parse(_required(json, 'status')),
      winner: winner,
      whiteOffersDraw: _optionalFlag(json, 'wdraw'),
      blackOffersDraw: _optionalFlag(json, 'bdraw'),
      whiteOffersTakeback: _optionalFlag(json, 'wtakeback'),
      blackOffersTakeback: _optionalFlag(json, 'btakeback'),
      expiration: _optionalExpiration(json['expiration']),
    );
  }

  static final RegExp _uciMove = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');

  final List<String> moves;
  final int whiteTimeMs;
  final int blackTimeMs;
  final int whiteIncrementMs;
  final int blackIncrementMs;
  final LichessGameStatus status;
  final LichessPlayerColor? winner;
  final bool whiteOffersDraw;
  final bool blackOffersDraw;
  final bool whiteOffersTakeback;
  final bool blackOffersTakeback;
  final LichessGameExpiration? expiration;
}

final class LichessGameExpiration {
  const LichessGameExpiration({
    required this.idleMilliseconds,
    required this.millisecondsToMove,
  });

  final int idleMilliseconds;
  final int millisecondsToMove;
}

final class LichessGameFullEvent extends LichessGameStreamEvent {
  const LichessGameFullEvent({
    required this.gameId,
    required this.variantKey,
    required this.variantName,
    required this.speed,
    required this.rated,
    required this.createdAtMilliseconds,
    required this.white,
    required this.black,
    required this.initialFen,
    required this.state,
  }) : super('gameFull');

  factory LichessGameFullEvent.fromJson(Map<String, Object?> json) {
    _requireType(json, 'gameFull');
    final Map<String, Object?> variant = _requiredObject(json, 'variant');
    _requiredObject(json, 'perf');
    final Map<String, Object?> state = _requiredObject(json, 'state');
    return LichessGameFullEvent(
      gameId: _requiredBoundedString(
        _required(json, 'id'),
        field: 'id',
        maximumLength: 64,
      ),
      variantKey: _requiredBoundedString(
        _required(variant, 'key'),
        field: 'variant.key',
        maximumLength: 64,
      ),
      variantName: _requiredBoundedString(
        _required(variant, 'name'),
        field: 'variant.name',
        maximumLength: 64,
      ),
      speed: _requiredBoundedString(
        _required(json, 'speed'),
        field: 'speed',
        maximumLength: 32,
      ),
      rated: _requiredBool(json, 'rated'),
      createdAtMilliseconds: _requiredNonNegativeInt(json, 'createdAt'),
      white: LichessGamePlayer.fromJson(_requiredObject(json, 'white')),
      black: LichessGamePlayer.fromJson(_requiredObject(json, 'black')),
      initialFen: _requiredBoundedString(
        _required(json, 'initialFen'),
        field: 'initialFen',
        maximumLength: 256,
      ),
      state: LichessGameStateEvent.fromJson(state),
    );
  }

  final String gameId;
  final String variantKey;
  final String variantName;
  final String speed;
  final bool rated;
  final int createdAtMilliseconds;
  final LichessGamePlayer white;
  final LichessGamePlayer black;
  final String initialFen;
  final LichessGameStateEvent state;
}

final class LichessGamePlayer {
  const LichessGamePlayer({required this.id, required this.name});

  factory LichessGamePlayer.fromJson(Map<String, Object?> json) {
    return LichessGamePlayer(
      id: _requiredBoundedString(
        _required(json, 'id'),
        field: 'player.id',
        maximumLength: 64,
      ),
      name: _requiredBoundedString(
        _required(json, 'name'),
        field: 'player.name',
        maximumLength: 128,
      ),
    );
  }

  final String id;
  final String name;
}

final class LichessOpponentGoneEvent extends LichessGameStreamEvent {
  const LichessOpponentGoneEvent({
    required this.gone,
    required this.claimWinInSeconds,
  }) : super('opponentGone');

  factory LichessOpponentGoneEvent.fromJson(Map<String, Object?> json) {
    _requireType(json, 'opponentGone');
    final Object? claimValue = json['claimWinInSeconds'];
    if (claimValue != null && (claimValue is! int || claimValue < 0)) {
      _invalid('claimWinInSeconds');
    }
    return LichessOpponentGoneEvent(
      gone: _requiredBool(json, 'gone'),
      claimWinInSeconds: claimValue as int?,
    );
  }

  final bool gone;
  final int? claimWinInSeconds;
}

final class LichessUnknownGameEvent extends LichessGameStreamEvent {
  const LichessUnknownGameEvent(super.type);
}

final class LichessGameStreamDecoder {
  bool _hasFirstEvent = false;

  LichessGameStreamEvent decode(Map<String, Object?> json) {
    final String type = _requiredBoundedString(
      _required(json, 'type'),
      field: 'type',
      maximumLength: 64,
    );
    final bool isFirstEvent = !_hasFirstEvent;
    if (isFirstEvent) {
      _hasFirstEvent = true;
      if (type != 'gameFull') {
        throw const LichessProtocolException(
          LichessProtocolFailureKind.unexpectedFirstEvent,
          'The Lichess game stream did not begin with gameFull.',
        );
      }
    } else if (type == 'gameFull') {
      throw const LichessProtocolException(
        LichessProtocolFailureKind.invalidField,
        'The Lichess game stream repeated gameFull.',
      );
    }

    return switch (type) {
      'gameFull' => LichessGameFullEvent.fromJson(json),
      'gameState' => LichessGameStateEvent.fromJson(json),
      'opponentGone' => LichessOpponentGoneEvent.fromJson(json),
      _ => LichessUnknownGameEvent(type),
    };
  }
}

Object? _required(Map<String, Object?> json, String field) {
  if (!json.containsKey(field)) {
    throw LichessProtocolException(
      LichessProtocolFailureKind.missingField,
      'Lichess stream record is missing $field.',
    );
  }
  return json[field];
}

String _requiredString(Map<String, Object?> json, String field) {
  final Object? value = _required(json, field);
  if (value is! String) {
    _invalid(field);
  }
  return value;
}

String _requiredBoundedString(
  Object? value, {
  required String field,
  required int maximumLength,
}) {
  if (value is! String ||
      value.isEmpty ||
      value.length > maximumLength ||
      value.codeUnits.any((int unit) => unit < 0x20 || unit > 0x7e)) {
    _invalid(field);
  }
  return value;
}

int _requiredNonNegativeInt(Map<String, Object?> json, String field) {
  final Object? value = _required(json, field);
  if (value is! int || value < 0) {
    _invalid(field);
  }
  return value;
}

bool _requiredBool(Map<String, Object?> json, String field) {
  final Object? value = _required(json, field);
  if (value is! bool) {
    _invalid(field);
  }
  return value;
}

bool _optionalFlag(Map<String, Object?> json, String field) {
  final Object? value = json[field];
  if (value == null) {
    return false;
  }
  if (value is! bool) {
    _invalid(field);
  }
  return value;
}

LichessGameExpiration? _optionalExpiration(Object? value) {
  if (value == null) {
    return null;
  }
  if (value is! Map<String, dynamic>) {
    _invalid('expiration');
  }
  final Map<String, Object?> expiration = Map<String, Object?>.unmodifiable(
    value,
  );
  return LichessGameExpiration(
    idleMilliseconds: _requiredNonNegativeInt(expiration, 'idleMillis'),
    millisecondsToMove: _requiredNonNegativeInt(expiration, 'millisToMove'),
  );
}

Map<String, Object?> _requiredObject(Map<String, Object?> json, String field) {
  final Object? value = _required(json, field);
  if (value is! Map<String, dynamic>) {
    _invalid(field);
  }
  return Map<String, Object?>.unmodifiable(value);
}

void _requireType(Map<String, Object?> json, String expected) {
  if (_required(json, 'type') != expected) {
    _invalid('type');
  }
}

Never _invalid(String field) {
  throw LichessProtocolException(
    LichessProtocolFailureKind.invalidField,
    'Lichess stream record has an invalid $field.',
  );
}
