import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/online/lichess/lichess_board_api_events.dart';
import 'package:litch_mobile/src/online/lichess/lichess_protocol.dart';

void main() {
  group('Lichess game stream', () {
    test('requires gameFull as the first record', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();

      expect(
        () => decoder.decode(_gameState()),
        throwsA(
          isA<LichessProtocolException>().having(
            (LichessProtocolException error) => error.kind,
            'kind',
            LichessProtocolFailureKind.unexpectedFirstEvent,
          ),
        ),
      );
    });

    test('decodes a standard casual Rapid gameFull fixture', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();

      final LichessGameFullEvent event =
          decoder.decode(<String, Object?>{
                'type': 'gameFull',
                'id': 'BEOucQJo',
                'variant': <String, Object?>{
                  'key': 'standard',
                  'name': 'Standard',
                  'future': 'ignored',
                },
                'speed': 'rapid',
                'perf': <String, Object?>{'name': 'Rapid'},
                'rated': false,
                'createdAt': 1788426000000,
                'white': <String, Object?>{'id': 'white-id', 'name': 'White'},
                'black': <String, Object?>{'id': 'black-id', 'name': 'Black'},
                'initialFen': 'startpos',
                'future': <String, Object?>{'ignored': true},
                'state': _gameState(moves: 'e2e4 c7c5'),
              })
              as LichessGameFullEvent;

      expect(event.gameId, 'BEOucQJo');
      expect(event.variantKey, 'standard');
      expect(event.speed, 'rapid');
      expect(event.rated, isFalse);
      expect(event.white.id, 'white-id');
      expect(event.black.name, 'Black');
      expect(event.initialFen, 'startpos');
      expect(event.state.moves, <String>['e2e4', 'c7c5']);
    });

    test('absent and false optional offers both decode as cleared', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();
      decoder.decode(_gameFull());

      final LichessGameStateEvent offered =
          decoder.decode(
                _gameState(
                  extra: <String, Object?>{'wdraw': true, 'btakeback': true},
                ),
              )
              as LichessGameStateEvent;
      final LichessGameStateEvent absent =
          decoder.decode(_gameState()) as LichessGameStateEvent;
      final LichessGameStateEvent explicitlyFalse =
          decoder.decode(
                _gameState(
                  extra: <String, Object?>{'wdraw': false, 'btakeback': false},
                ),
              )
              as LichessGameStateEvent;

      expect(offered.whiteOffersDraw, isTrue);
      expect(offered.blackOffersTakeback, isTrue);
      expect(absent.whiteOffersDraw, isFalse);
      expect(absent.blackOffersTakeback, isFalse);
      expect(explicitlyFalse.whiteOffersDraw, isFalse);
      expect(explicitlyFalse.blackOffersTakeback, isFalse);
    });

    test('maps every pinned status and preserves a future value', () {
      for (final LichessKnownGameStatus known
          in LichessKnownGameStatus.values) {
        final LichessGameStatus status = LichessGameStatus.parse(
          known.wireValue,
        );
        expect(status.known, known);
        expect(status.isTerminalOrUnknown, known.isTerminal);
      }

      final LichessGameStatus future = LichessGameStatus.parse(
        'futureTerminal',
      );
      expect(future.known, isNull);
      expect(future.wireValue, 'futureTerminal');
      expect(future.isTerminalOrUnknown, isTrue);
    });

    test('preserves an unknown additive event type without its payload', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();
      decoder.decode(_gameFull());

      final LichessUnknownGameEvent event =
          decoder.decode(<String, Object?>{
                'type': 'futureEvent',
                'privatePayload': 'not retained',
              })
              as LichessUnknownGameEvent;

      expect(event.type, 'futureEvent');
    });

    test('rejects missing required state fields and malformed offers', () {
      final LichessGameStreamDecoder missingFieldDecoder =
          LichessGameStreamDecoder();
      missingFieldDecoder.decode(_gameFull());
      final Map<String, Object?> missingClock = _gameState()..remove('wtime');

      expect(
        () => missingFieldDecoder.decode(missingClock),
        throwsA(
          isA<LichessProtocolException>().having(
            (LichessProtocolException error) => error.kind,
            'kind',
            LichessProtocolFailureKind.missingField,
          ),
        ),
      );

      final LichessGameStreamDecoder malformedOfferDecoder =
          LichessGameStreamDecoder();
      malformedOfferDecoder.decode(_gameFull());
      expect(
        () => malformedOfferDecoder.decode(
          _gameState(extra: <String, Object?>{'wdraw': 'yes'}),
        ),
        throwsA(isA<LichessProtocolException>()),
      );
    });

    test('decodes opponentGone with an optional claim window', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();
      decoder.decode(_gameFull());

      final LichessOpponentGoneEvent event =
          decoder.decode(<String, Object?>{
                'type': 'opponentGone',
                'gone': true,
                'claimWinInSeconds': 12,
              })
              as LichessOpponentGoneEvent;

      expect(event.gone, isTrue);
      expect(event.claimWinInSeconds, 12);
    });

    test('decodes and clears optional first-move expiration state', () {
      final LichessGameStreamDecoder decoder = LichessGameStreamDecoder();
      decoder.decode(_gameFull());

      final LichessGameStateEvent expiring =
          decoder.decode(
                _gameState(
                  extra: <String, Object?>{
                    'expiration': <String, Object?>{
                      'idleMillis': 3000,
                      'millisToMove': 12000,
                    },
                  },
                ),
              )
              as LichessGameStateEvent;
      final LichessGameStateEvent absent =
          decoder.decode(_gameState()) as LichessGameStateEvent;

      expect(expiring.expiration!.idleMilliseconds, 3000);
      expect(expiring.expiration!.millisecondsToMove, 12000);
      expect(absent.expiration, isNull);
    });
  });
}

Map<String, Object?> _gameFull() {
  return <String, Object?>{
    'type': 'gameFull',
    'id': 'BEOucQJo',
    'variant': <String, Object?>{'key': 'standard', 'name': 'Standard'},
    'speed': 'rapid',
    'perf': <String, Object?>{'name': 'Rapid'},
    'rated': false,
    'createdAt': 1788426000000,
    'white': <String, Object?>{'id': 'white-id', 'name': 'White'},
    'black': <String, Object?>{'id': 'black-id', 'name': 'Black'},
    'initialFen': 'startpos',
    'state': _gameState(),
  };
}

Map<String, Object?> _gameState({
  String moves = '',
  Map<String, Object?> extra = const <String, Object?>{},
}) {
  return <String, Object?>{
    'type': 'gameState',
    'moves': moves,
    'wtime': 300000,
    'btime': 300000,
    'winc': 3000,
    'binc': 3000,
    'status': 'started',
    ...extra,
  };
}
