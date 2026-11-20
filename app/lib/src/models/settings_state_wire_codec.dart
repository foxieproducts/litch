import 'dart:typed_data';

final class SettingsStateSnapshot {
  const SettingsStateSnapshot({
    required this.settingsRevision,
    required this.values,
  });

  final int settingsRevision;
  final Map<String, Object> values;
}

final class SettingsStateWireCodec {
  const SettingsStateWireCodec._();

  static const int version = 2;
  static const int visibleCount = 39;
  static const int payloadBytes = 84;
  static const int fragmentBytes = 10;
  static const int firstStateCode = 14;
  static const int fragmentCount = 9;
  static const int lastStateCode = firstStateCode + 7;
  static const int styleStateCode = 39;
  static const int playerPaletteStateCode = 46;

  static SettingsStateSnapshot? tryDecodePlayerPalette(Uint8List bytes) {
    if (bytes.length != 8 ||
        bytes[0] != 1 ||
        (bytes[7] != 1 && bytes[7] != 2)) {
      return null;
    }
    return SettingsStateSnapshot(
      settingsRevision: ByteData.sublistView(bytes).getUint32(1, Endian.little),
      values: Map.unmodifiable(<String, Object>{
        'P1_END_PIECE_WHEEL': bytes[5],
        'P2_END_PIECE_WHEEL': bytes[6],
        'P1_PIECE_WHEEL': bytes[5],
        'P2_PIECE_WHEEL': bytes[6],
        'GAME_FIRST_PLAYER': bytes[7],
      }),
    );
  }

  // Separate additive State; the deployed 84-byte snapshot stays unchanged.
  static const int bootDisplayStateCode = 44;
  static const List<String> bootDisplayValues = <String>[
    'fox',
    'artwork',
    'off',
  ];
  static const int assistanceStateCode = 43;
  static const List<String> assistanceKeys = <String>[
    'P1_LEGAL_MOVES',
    'P2_LEGAL_MOVES',
    'P1_CHECK_CUE',
    'P2_CHECK_CUE',
    'P1_AUTO_HINTS',
    'P2_AUTO_HINTS',
  ];
  static const List<String> extensionKeys = <String>[
    ...assistanceKeys,
    'BOOT_DISPLAY',
  ];
  static const List<String> hintStyleValues = <String>[
    'foxie_sweep',
    'player_clockwise',
    'none',
    'beachball',
    'cauldron_brewing',
  ];
  static int fragmentIndex(int code) => code == styleStateCode
      ? 8
      : code >= firstStateCode && code <= lastStateCode
      ? code - firstStateCode
      : -1;
  static int fragmentCode(int index) =>
      index < 8 ? firstStateCode + index : styleStateCode;
  static int fragmentLength(int index) => index < 8 ? fragmentBytes : 4;
  static bool acceptsCode(int code) =>
      code == styleStateCode ||
      (code >= firstStateCode && code <= lastStateCode);
  static const int mutationOpcode = 24;
  static const Set<int> retiredVisibleIndices = <int>{26, 27, 32, 33};

  static const List<String> _prefixKeys = <String>[
    'BRIGHTNESS',
    'WHITE_SQUARES_ON',
    'P1_PIECE_WHEEL',
    'P2_PIECE_WHEEL',
    'TURN_ANIM_STYLE',
    'CHECKMATE_ANIM_STYLE',
    'MOVE_CONFIRM',
    'VS_CPU_LEVEL',
    'HINT_LEVEL',
    'VS_CPU_PLAYER',
    'BOARD_ROTATE',
    'GAME_FIRST_PLAYER',
    'TIMER',
    'SLEEP_TIMEOUT',
    'RGBW_COOL_TO_WARM_THRESHOLD',
    'USB_CC_THRESH_500',
    'USB_CC_THRESH_1500',
    'USB_CC_THRESH_3000',
    'BLE_ENABLED',
    'TAKEBACK_GRACE_SECONDS',
    'MOTION_WAKE_ENABLED',
    'MOTION_WAKE_MODE',
    'MOTION_WAKE_THRESHOLD_MG',
    'MOTION_WAKE_DURATION_MS',
    'TAP_WAKE_THRESHOLD_MG',
    'TAP_WAKE_SHOCK_MS',
    '', // Reserved retired P1-X enable; supported S3 wake is hardware policy.
    '', // Reserved retired hold setting; release sampling owns wake timing.
    'BOARD_SETUP_ASSISTANCE',
    'MINIMAL_ANIMATIONS',
    'SETTLE_ANIM_STYLE',
    'AUTO_BRIGHTNESS',
    '', // Reserved retired field; later settings keep their wire indices.
    '', // Reserved retired button-emulation field.
    'LIGHT_SQUARE_MODE',
    'LIGHT_SQUARE_HUE',
    'LIGHT_SQUARE_BRIGHTNESS',
  ];
  static const List<String> visibleKeys = <String>[
    ..._prefixKeys,
    'HINT_THINKING_STYLE',
    'PROMOTION_MODE',
  ];
  static const List<String> mutationKeys = <String>[
    ..._prefixKeys,
    'P1_END_PIECE_WHEEL',
    'P2_END_PIECE_WHEEL',
    'HINT_THINKING_STYLE',
    'PROMOTION_MODE',
    ...assistanceKeys,
    'BOOT_DISPLAY',
  ];

  static const Map<int, List<String>> _enumValues = <int, List<String>>{
    4: <String>[
      'active_end_only',
      'active_end_pingpong_sides',
      'both_ends',
      'side_middle_band',
      'away_from_active_end',
      'none',
    ],
    5: <String>[
      'fireworks',
      'fire-lob',
      'ripple-bloom',
      'winner-sweep',
      'winner-constellation',
    ],
    6: <String>['settle_2s', 'settle_5s', 'press_circle_to_confirm'],
    13: <String>['5m', '30m', '60m', 'disabled'],
    21: <String>['motion', 'tap', 'motion_and_tap'],
    30: <String>[
      'landing_flash',
      'mini_orbit',
      'ripple',
      'none',
      'rainbow_orbit',
    ],
    34: <String>['white', 'color'],
    39: hintStyleValues,
    40: <String>['queen', 'choose'],
    47: bootDisplayValues,
  };

  static const Set<int> _boolIndices = <int>{
    1,
    10,
    18,
    20,
    26,
    28,
    29,
    31,
    41,
    42,
    43,
    44,
    45,
    46,
  };

  static SettingsStateSnapshot? tryDecode(Uint8List bytes) {
    if (bytes.length != payloadBytes || visibleKeys.length != visibleCount) {
      return null;
    }
    final data = ByteData.sublistView(bytes);
    final values = <String, Object>{};
    for (var index = 0; index < 37; index += 1) {
      final normalized = data.getUint16(4 + index * 2, Endian.little);
      if (retiredVisibleIndices.contains(index)) {
        if (normalized != 0) return null;
        continue;
      }
      final enumValues = _enumValues[index];
      if (enumValues != null) {
        if (normalized >= enumValues.length) return null;
        values[visibleKeys[index]] = enumValues[normalized];
      } else if (_boolIndices.contains(index)) {
        if (normalized > 1) return null;
        values[visibleKeys[index]] = normalized != 0;
      } else {
        values[visibleKeys[index]] = normalized;
      }
    }
    final style = data.getUint16(80, Endian.little);
    if (style >= hintStyleValues.length) return null;
    values['HINT_THINKING_STYLE'] = hintStyleValues[style];
    final promotion = data.getUint16(82, Endian.little);
    if (promotion > 1) return null;
    values['PROMOTION_MODE'] = promotion == 0 ? 'queen' : 'choose';
    values['P1_END_PIECE_WHEEL'] = bytes[78];
    values['P2_END_PIECE_WHEEL'] = bytes[79];
    return SettingsStateSnapshot(
      settingsRevision: data.getUint32(0, Endian.little),
      values: Map.unmodifiable(values),
    );
  }

  static Uint8List? tryEncodeMutation(String key, Object value) {
    final index = mutationKeys.indexOf(key);
    if (index < 0 || key.isEmpty) return null;
    int? normalized;
    final enumValues = _enumValues[index];
    if (enumValues != null) {
      if (value is String) {
        final option = enumValues.indexOf(value);
        if (option >= 0) normalized = option;
      }
    } else if (_boolIndices.contains(index)) {
      if (value is bool) normalized = value ? 1 : 0;
    } else if (value is int && value >= 0 && value <= 0xffff) {
      normalized = value;
    }
    if (normalized == null) return null;
    final body = Uint8List(4);
    body[0] = version;
    body[1] = index;
    ByteData.sublistView(body).setUint16(2, normalized, Endian.little);
    return body;
  }
}

final class SettingsStateWireAssembler {
  int? _revision;
  final List<Uint8List?> _fragments = List<Uint8List?>.filled(
    SettingsStateWireCodec.fragmentCount,
    null,
  );
  int? _publishedRevision;
  SettingsStateSnapshot? _base;
  SettingsStateSnapshot? _assistance;
  int? _publishedAssistanceIdentity;
  SettingsStateSnapshot? _boot;
  int? _publishedBootIdentity;

  bool get isIncomplete =>
      (_revision != null && _fragments.any((fragment) => fragment == null)) ||
      (_base != null &&
          _assistance != null &&
          _base!.settingsRevision != _assistance!.settingsRevision) ||
      (_base != null &&
          _boot != null &&
          _base!.settingsRevision != _boot!.settingsRevision);

  SettingsStateSnapshot? accept({
    required int code,
    required int revision,
    required Uint8List body,
  }) {
    if (code == SettingsStateWireCodec.bootDisplayStateCode) {
      if (revision <= 0 ||
          body.length != 6 ||
          body[0] != 1 ||
          body[5] >= SettingsStateWireCodec.bootDisplayValues.length ||
          revision == _publishedBootIdentity) {
        return null;
      }
      final settingsRevision = ByteData.sublistView(
        body,
      ).getUint32(1, Endian.little);
      final prior = _boot;
      if (prior != null &&
          settingsRevision != prior.settingsRevision &&
          ((settingsRevision - prior.settingsRevision) & 0xffffffff) >=
              0x80000000) {
        return null;
      }
      _boot = SettingsStateSnapshot(
        settingsRevision: settingsRevision,
        values: <String, Object>{
          'BOOT_DISPLAY': SettingsStateWireCodec.bootDisplayValues[body[5]],
        },
      );
      _publishedBootIdentity = revision;
      return _joined();
    }
    if (code == SettingsStateWireCodec.assistanceStateCode) {
      if (revision <= 0 ||
          body.length != 6 ||
          body[0] != 1 ||
          (body[5] & 0xc0) != 0 ||
          revision == _publishedAssistanceIdentity) {
        return null;
      }
      final settingsRevision = ByteData.sublistView(
        body,
      ).getUint32(1, Endian.little);
      final prior = _assistance;
      if (prior != null &&
          settingsRevision != prior.settingsRevision &&
          ((settingsRevision - prior.settingsRevision) & 0xffffffff) >=
              0x80000000) {
        return null;
      }
      _assistance = SettingsStateSnapshot(
        settingsRevision: settingsRevision,
        values: <String, Object>{
          for (
            var bit = 0;
            bit < SettingsStateWireCodec.assistanceKeys.length;
            bit++
          )
            SettingsStateWireCodec.assistanceKeys[bit]:
                (body[5] & (1 << bit)) != 0,
        },
      );
      _publishedAssistanceIdentity = revision;
      return _joined();
    }
    final index = SettingsStateWireCodec.fragmentIndex(code);
    if (index < 0 ||
        index >= SettingsStateWireCodec.fragmentCount ||
        revision <= 0 ||
        body.length != SettingsStateWireCodec.fragmentLength(index) ||
        (revision == _publishedRevision && _revision == null)) {
      return null;
    }
    if (_revision != revision) {
      _clearFragments();
      _revision = revision;
    }
    _fragments[index] = Uint8List.fromList(body);
    if (_fragments.any((fragment) => fragment == null)) return null;
    final payload = Uint8List.fromList([
      for (final fragment in _fragments) ...fragment!,
    ]);
    final decoded = SettingsStateWireCodec.tryDecode(payload);
    if (decoded == null) {
      _clearFragments();
      // A rejected complete payload still needs an authoritative replacement.
      _revision = revision;
      return null;
    }
    final prior = _base;
    if (prior != null &&
        decoded.settingsRevision != prior.settingsRevision &&
        ((decoded.settingsRevision - prior.settingsRevision) & 0xffffffff) >=
            0x80000000) {
      _clearFragments();
      return null;
    }
    final replay = _publishedRevision == revision;
    _publishedRevision = revision;
    _base = decoded;
    _clearFragments();
    return replay ? null : _joined() ?? decoded;
  }

  SettingsStateSnapshot? _joined() {
    final base = _base;
    final assistance = _assistance;
    if (base == null ||
        assistance == null ||
        base.settingsRevision != assistance.settingsRevision ||
        (_boot != null && base.settingsRevision != _boot!.settingsRevision)) {
      return null;
    }
    return SettingsStateSnapshot(
      settingsRevision: base.settingsRevision,
      values: Map.unmodifiable(<String, Object>{
        ...base.values,
        ...assistance.values,
        if (_boot != null) ..._boot!.values,
      }),
    );
  }

  void clear() {
    _publishedRevision = null;
    _base = null;
    _assistance = null;
    _publishedAssistanceIdentity = null;
    _boot = null;
    _publishedBootIdentity = null;
    _clearFragments();
  }

  void _clearFragments() {
    _revision = null;
    for (var index = 0; index < _fragments.length; index += 1) {
      _fragments[index] = null;
    }
  }
}
