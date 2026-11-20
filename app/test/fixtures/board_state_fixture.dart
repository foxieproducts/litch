import 'dart:convert';

Map<String, Object?> boardStateRecord() => <String, Object?>{
  'event': 'board_state',
  'v': 1,
  'sr': 14,
  'home': jsonDecode(
    '{"event":"home_state","v":1,"seq":42,"a":"idle",'
    '"fen":"8/8/8/8/8/8/8/K6k w - - 0 1","occ":"8100000000000000",'
    '"g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0}',
  ),
  'health': <String, Object>{
    'event': 'board_health',
    'v': 1,
    'h': '000000000',
    'ps': 'n',
    'rr': 0,
    'boot': 1,
    'wd': 0,
  },
  'settings': <List<Object>>[
    <Object>['GAME_FIRST_PLAYER', 2],
    <Object>['P1_PIECE_WHEEL', 40],
    <Object>['P2_PIECE_WHEEL', 200],
    <Object>['VS_CPU_LEVEL', 4],
    <Object>['VS_CPU_PLAYER', 1],
  ],
  'ends': <int>[40, 200],
};
