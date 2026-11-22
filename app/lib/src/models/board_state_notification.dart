import 'dart:convert';

import 'board_health_status.dart';
import 'home_state_notification.dart';

/// A validated wire record. The BLE service remains the retained-state owner.
class BoardStateNotification {
  const BoardStateNotification._({
    required this.settingsRevision,
    required this.receipt,
    required this.home,
    required this.health,
    required this.values,
    required this.keysByIndex,
  });

  final int settingsRevision;
  final int? receipt;
  final HomeStateNotification home;
  final BoardHealthStatus health;
  final Map<String, Object> values;
  final Map<int, String> keysByIndex;

  static BoardStateNotification? tryParse(String line) {
    if (utf8.encode(line).length > 2047) return null;
    try {
      final Object? decoded = jsonDecode(line);
      if (decoded is! Map<String, dynamic> ||
          decoded['event'] != 'board_state' ||
          decoded['v'] != 1) {
        return null;
      }
      final Object? revision = decoded['sr'];
      final Object? receipt = decoded['q'];
      if (receipt != null && (!_isUint32(receipt) || receipt == 0)) return null;
      final Object? entries = decoded['settings'];
      final Object? ends = decoded['ends'];
      if (!_isUint32(revision) ||
          entries is! List ||
          entries.isEmpty ||
          entries.length > 64 ||
          ends is! List ||
          ends.length != 2 ||
          !ends.every(_isWheel)) {
        return null;
      }
      // Reuse the existing bounded Home and health validators. Increasing the
      // outer record limit does not relax either nested protocol contract.
      final HomeStateNotification? home = HomeStateNotification.tryParse(
        jsonEncode(decoded['home']),
      );
      final BoardHealthStatus? health = BoardHealthStatus.tryParse(
        jsonEncode(decoded['health']),
      );
      if (home == null || health == null) return null;
      final values = <String, Object>{};
      final keysByIndex = <int, String>{};
      for (int index = 0; index < entries.length; index++) {
        final Object? entry = entries[index];
        if (entry is! List || entry.length != 2) return null;
        final Object? key = entry[0];
        final Object? value = entry[1];
        if (key is! String ||
            !RegExp(r'^[A-Z][A-Z0-9_]{0,47}$').hasMatch(key) ||
            values.containsKey(key) ||
            !(value is bool ||
                _isUint32(value) ||
                (value is String && utf8.encode(value).length <= 96))) {
          return null;
        }
        keysByIndex[index] = key;
        values[key] = value!;
      }
      final Object? firstPlayer = values['GAME_FIRST_PLAYER'];
      if ((firstPlayer != 1 && firstPlayer != 2) ||
          !_isUint32(values['VS_CPU_LEVEL']) ||
          (values['VS_CPU_PLAYER'] != 1 && values['VS_CPU_PLAYER'] != 2) ||
          !_isWheel(values['P1_PIECE_WHEEL']) ||
          !_isWheel(values['P2_PIECE_WHEEL'])) {
        return null;
      }
      // Settings aliases retain their fixed end slots across player assignment.
      if (values['P1_PIECE_WHEEL'] != ends[0] ||
          values['P2_PIECE_WHEEL'] != ends[1]) {
        return null;
      }
      values['P1_END_PIECE_WHEEL'] = ends[0] as int;
      values['P2_END_PIECE_WHEEL'] = ends[1] as int;
      return BoardStateNotification._(
        settingsRevision: revision as int,
        receipt: receipt as int?,
        home: home,
        health: health,
        values: Map.unmodifiable(values),
        keysByIndex: Map.unmodifiable(keysByIndex),
      );
    } on FormatException {
      return null;
    }
  }

  static bool _isUint32(Object? value) =>
      value is int && value >= 0 && value <= 0xffffffff;
  static bool _isWheel(Object? value) =>
      value is int && value >= 0 && value <= 255;
}
