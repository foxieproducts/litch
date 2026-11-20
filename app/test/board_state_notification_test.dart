import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'fixtures/board_state_fixture.dart';
import 'package:litch_mobile/src/models/board_state_notification.dart';

void main() {
  test(
    'decodes firmware end aliases across player assignments without changing indices',
    () {
      for (final first in [1, 2]) {
        final record = boardStateRecord();
        (record['settings'] as List<List<Object>>)[0][1] = first;
        final state = BoardStateNotification.tryParse(jsonEncode(record));
        expect(state, isNotNull);
        expect(state!.settingsRevision, 14);
        expect(state.home.sequence, 42);
        expect(state.values['P1_PIECE_WHEEL'], 40);
        expect(state.values['P1_END_PIECE_WHEEL'], 40);
        expect(state.keysByIndex[1], 'P1_PIECE_WHEEL');
        expect(state.keysByIndex.length, 5);
      }
    },
  );

  test('accepts the production bootstrap within the 64-setting transport bound', () {
    // Captured from the production VirtualLitch bridge: 34 visible settings.
    const wire =
        '{"event":"board_state","v":1,"sr":6,"home":{"event":"home_state","v":1,"seq":1,"a":"lift'
        '","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"FFEF00000000FF'
        'FF","g":"u","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":1,"up":0,"co":'
        '0,"cl":[1,60,60,"w",1,0,1,"n"],"as":"0le20000101000000000"},"settings":[["BRIGHTNESS",5]'
        ',["WHITE_SQUARES_ON",true],["P1_PIECE_WHEEL",40],["P2_PIECE_WHEEL",200],["TURN_ANIM_STYL'
        'E","active_end_pingpong_sides"],["CHECKMATE_ANIM_STYLE","fireworks"],["MOVE_CONFIRM","se'
        'ttle_2s"],["VS_CPU_LEVEL",4],["HINT_LEVEL",8],["VS_CPU_PLAYER",2],["BOARD_ROTATE",false]'
        ',["GAME_FIRST_PLAYER",2],["TIMER",1],["SLEEP_TIMEOUT","60m"],["RGBW_COOL_TO_WARM_THRESHO'
        'LD",32],["USB_CC_THRESH_500",320],["USB_CC_THRESH_1500",580],["USB_CC_THRESH_3000",850],'
        '["BLE_ENABLED",true],["TAKEBACK_GRACE_SECONDS",0],["MOTION_WAKE_ENABLED",true],["MOTION_'
        'WAKE_MODE","motion"],["MOTION_WAKE_THRESHOLD_MG",125],["MOTION_WAKE_DURATION_MS",80],["T'
        'AP_WAKE_THRESHOLD_MG",563],["TAP_WAKE_SHOCK_MS",40],["P1_X_DEEP_WAKE_ENABLED",true],["P1'
        '_X_WAKE_HOLD_MS",1000],["BOARD_SETUP_ASSISTANCE",true],["MINIMAL_ANIMATIONS",false],["SE'
        'TTLE_ANIM_STYLE","mini_orbit"],["AUTO_BRIGHTNESS",true],["BLE_CONNECTION_INDICATOR",true],["V'
        '3_BUTTON_LED_EMULATION",true]],"ends":[40,200],"health":{"event":"board_health","v":1,"h'
        '":"000000000","ps":"n","rr":0,"boot":0,"wd":0,"sd":0}}';
    final state = BoardStateNotification.tryParse(wire);
    expect(state, isNotNull);
    expect(state!.keysByIndex.length, 34);
    expect(state.keysByIndex[33], 'V3_BUTTON_LED_EMULATION');
    expect(state.values['P1_END_PIECE_WHEEL'], 40);
    expect(state.values['P2_END_PIECE_WHEEL'], 200);

    final record = jsonDecode(wire) as Map<String, dynamic>;
    final entries = record['settings'] as List<dynamic>;
    for (var index = entries.length; index < 64; ++index) {
      entries.add(['X$index', 0]);
    }
    expect(BoardStateNotification.tryParse(jsonEncode(record)), isNotNull);
    entries.add(['X64', 0]);
    final oversized = jsonEncode(record);
    expect(utf8.encode(oversized).length, lessThanOrEqualTo(2047));
    expect(BoardStateNotification.tryParse(oversized), isNull);
  });

  test('correlated receipt accepts only nonzero uint32 tokens', () {
    for (final value in [1, 0xffffffff]) {
      final record = boardStateRecord()..['q'] = value;
      expect(
        BoardStateNotification.tryParse(jsonEncode(record))?.receipt,
        value,
      );
    }
    for (final value in <Object>[0, -1, 0x100000000, '1', true]) {
      final record = boardStateRecord()..['q'] = value;
      expect(BoardStateNotification.tryParse(jsonEncode(record)), isNull);
    }
  });

  test('rejects partial or contradictory authority as a whole', () {
    final cases = <Map<String, Object?>>[
      boardStateRecord()..['sr'] = -1,
      boardStateRecord()..['ends'] = <int>[200, 40],
      boardStateRecord()
        ..['health'] = <String, Object>{'event': 'board_health'},
      boardStateRecord()..['home'] = <String, Object>{'event': 'home_state'},
      boardStateRecord()
        ..['settings'] = <List<Object>>[
          <Object>['GAME_FIRST_PLAYER', 1],
          <Object>['GAME_FIRST_PLAYER', 2],
        ],
      boardStateRecord()
        ..['settings'] = <List<Object>>[
          <Object>[
            'GAME_FIRST_PLAYER',
            <String, int>{'v': 2},
          ],
        ],
    ];
    for (final input in cases) {
      expect(BoardStateNotification.tryParse(jsonEncode(input)), isNull);
    }
    final encoded = jsonEncode(boardStateRecord());
    expect(
      BoardStateNotification.tryParse(encoded.substring(0, encoded.length - 1)),
      isNull,
    );
    expect(BoardStateNotification.tryParse(' ' * 2048 + encoded), isNull);
  });
}
