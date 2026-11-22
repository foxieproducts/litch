import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';

void main() {
  Uint8List fixture() {
    final row = File(
      '../test/fixtures/settings_state_wire_v3.csv',
    ).readAsLinesSync().firstWhere((line) => line.startsWith('defaults,'));
    final hex = row.substring(row.indexOf(',') + 1);
    return Uint8List.fromList(<int>[
      for (var index = 0; index < hex.length; index += 2)
        int.parse(hex.substring(index, index + 2), radix: 16),
    ]);
  }

  test('Settings epoch 2 rejects epoch 1 snapshots and encodes promotion', () {
    final old = Uint8List.sublistView(fixture(), 0, 82);
    expect(SettingsStateWireCodec.tryDecode(old), isNull);
    expect(SettingsStateWireCodec.version, 2);
    expect(SettingsStateWireCodec.fragmentLength(8), 4);
    expect(
      SettingsStateWireCodec.tryEncodeMutation('PROMOTION_MODE', 'choose'),
      <int>[2, 40, 1, 0],
    );
    final current = Uint8List(84)..setRange(0, 82, old);
    expect(
      SettingsStateWireCodec.tryDecode(current)?.values['PROMOTION_MODE'],
      'queen',
    );
    current[82] = 1;
    expect(
      SettingsStateWireCodec.tryDecode(current)?.values['PROMOTION_MODE'],
      'choose',
    );
    current[82] = 2;
    expect(SettingsStateWireCodec.tryDecode(current), isNull);
  });

  test('additive assistance preserves deployed Settings bytes and indices', () {
    expect(fixture().length, 84);
    expect(SettingsStateWireCodec.visibleKeys.length, 39);
    expect(
      SettingsStateWireCodec.acceptsCode(43),
      isFalse,
      reason: 'The deployed nine-fragment domain never consumes the extension.',
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('P1_END_PIECE_WHEEL', 17),
      <int>[2, 37, 17, 0],
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('P2_END_PIECE_WHEEL', 18),
      <int>[2, 38, 18, 0],
    );
    for (var index = 0; index < 6; index++) {
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          SettingsStateWireCodec.assistanceKeys[index],
          true,
        ),
        <int>[2, 41 + index, 1, 0],
      );
    }
  });

  test(
    'assistance joins both arrival orders, repairs revision changes and retires',
    () {
      final bytes = fixture();
      ByteData.sublistView(bytes).setUint32(0, 7, Endian.little);
      Uint8List extension(int revision, int bits) {
        final body = Uint8List.fromList(<int>[1, 0, 0, 0, 0, bits]);
        ByteData.sublistView(body).setUint32(1, revision, Endian.little);
        return body;
      }

      SettingsStateSnapshot? base(
        SettingsStateWireAssembler assembler,
        int identity,
      ) {
        SettingsStateSnapshot? result;
        for (var index = 0; index < 9; index++) {
          result =
              assembler.accept(
                code: SettingsStateWireCodec.fragmentCode(index),
                revision: identity,
                body: Uint8List.sublistView(
                  bytes,
                  index * 10,
                  index * 10 + SettingsStateWireCodec.fragmentLength(index),
                ),
              ) ??
              result;
        }
        return result;
      }

      for (final extensionFirst in <bool>[false, true]) {
        final assembler = SettingsStateWireAssembler();
        SettingsStateSnapshot? joined;
        if (extensionFirst) {
          expect(
            assembler.accept(code: 43, revision: 100, body: extension(7, 0x1f)),
            isNull,
          );
          joined = base(assembler, 77);
        } else {
          expect(
            base(assembler, 77)!.values.containsKey('P1_AUTO_HINTS'),
            isFalse,
            reason:
                'Old firmware supplies usable existing settings, never fabricated new preferences.',
          );
          joined = assembler.accept(
            code: 43,
            revision: 100,
            body: extension(7, 0x1f),
          );
        }
        expect(joined!.values['P1_AUTO_HINTS'], isTrue);
        expect(joined.values['P2_AUTO_HINTS'], isFalse);
        expect(
          assembler.accept(code: 43, revision: 100, body: extension(7, 0x1f)),
          isNull,
        );
        ByteData.sublistView(bytes).setUint32(0, 8, Endian.little);
        expect(
          base(assembler, 78)!.values.containsKey('P1_AUTO_HINTS'),
          isFalse,
        );
        expect(assembler.isIncomplete, isTrue);
        joined = assembler.accept(
          code: 43,
          revision: 101,
          body: extension(8, 0x2f),
        );
        expect(joined!.values['P1_AUTO_HINTS'], isFalse);
        expect(joined.values['P2_AUTO_HINTS'], isTrue);
        expect(assembler.isIncomplete, isFalse);
        assembler.clear();
        expect(
          base(assembler, 78)!.values.containsKey('P2_AUTO_HINTS'),
          isFalse,
          reason: 'A reconnect cannot borrow another generation’s extension.',
        );
        ByteData.sublistView(bytes).setUint32(0, 7, Endian.little);
      }
    },
  );

  test(
    'startup choice appends mutation 47 and joins revision-matched State 44',
    () {
      for (var option = 0; option < 3; option++) {
        expect(
          SettingsStateWireCodec.tryEncodeMutation(
            'BOOT_DISPLAY',
            SettingsStateWireCodec.bootDisplayValues[option],
          ),
          <int>[2, 47, option, 0],
        );
      }
      expect(
        SettingsStateWireCodec.tryEncodeMutation('BOOT_DISPLAY', 'unknown'),
        isNull,
      );
      Uint8List extension(int revision, int value) {
        final body = Uint8List.fromList([1, 0, 0, 0, 0, value]);
        ByteData.sublistView(body).setUint32(1, revision, Endian.little);
        return body;
      }

      SettingsStateSnapshot? base(
        SettingsStateWireAssembler assembler,
        int revision,
      ) {
        final bytes = fixture();
        ByteData.sublistView(bytes).setUint32(0, revision, Endian.little);
        SettingsStateSnapshot? result;
        for (var i = 0; i < 9; i++) {
          result =
              assembler.accept(
                code: SettingsStateWireCodec.fragmentCode(i),
                revision: revision + 100,
                body: Uint8List.sublistView(
                  bytes,
                  i * 10,
                  i * 10 + SettingsStateWireCodec.fragmentLength(i),
                ),
              ) ??
              result;
        }
        return result;
      }

      for (final order in ['bas', 'bsa', 'abs', 'asb', 'sab', 'sba']) {
        final assembler = SettingsStateWireAssembler();
        SettingsStateSnapshot? result;
        for (final event in order.split('')) {
          result =
              (switch (event) {
                'b' => base(assembler, 7),
                'a' => assembler.accept(
                  code: 43,
                  revision: 201,
                  body: extension(7, 0x3f),
                ),
                _ => assembler.accept(
                  code: 44,
                  revision: 202,
                  body: extension(7, 1),
                ),
              }) ??
              result;
        }
        expect(result!.values['BOOT_DISPLAY'], 'artwork', reason: order);
        expect(result.values['P1_AUTO_HINTS'], true);
        expect(
          assembler.accept(code: 44, revision: 202, body: extension(7, 1)),
          isNull,
        );
        expect(base(assembler, 8)!.values.containsKey('BOOT_DISPLAY'), false);
        expect(assembler.isIncomplete, true);
        expect(
          assembler.accept(code: 44, revision: 203, body: extension(8, 2)),
          isNull,
        );
        final fresh = assembler.accept(
          code: 43,
          revision: 204,
          body: extension(8, 0),
        );
        expect(fresh!.values['BOOT_DISPLAY'], 'off');
        expect(assembler.isIncomplete, false);
        expect(
          base(assembler, 7),
          isNull,
          reason:
              'Late base replay cannot roll back the extension join frontier.',
        );
        expect(
          assembler.accept(code: 44, revision: 205, body: extension(7, 0)),
          isNull,
        );
        expect(
          assembler.accept(code: 44, revision: 206, body: extension(8, 3)),
          isNull,
        );
        final invalidVersion = extension(8, 0)..[0] = 2;
        expect(
          assembler.accept(code: 44, revision: 207, body: invalidVersion),
          isNull,
        );
        expect(
          assembler.accept(code: 44, revision: 208, body: Uint8List(5)),
          isNull,
        );
        assembler.clear();
        base(assembler, 9);
        final reconnected = assembler.accept(
          code: 43,
          revision: 209,
          body: extension(9, 0),
        );
        expect(
          reconnected!.values.containsKey('BOOT_DISPLAY'),
          false,
          reason: 'Another connection never borrows a startup preference.',
        );
      }
    },
  );

  test('Turn styles keep historical wire indices and append away motion', () {
    const choices = [
      'active_end_only',
      'active_end_pingpong_sides',
      'both_ends',
      'side_middle_band',
      'away_from_active_end',
    ];
    for (var i = 0; i < choices.length; i++) {
      final bytes = fixture();
      ByteData.sublistView(bytes).setUint16(4 + 4 * 2, i, Endian.little);
      expect(
        SettingsStateWireCodec.tryDecode(bytes)!.values['TURN_ANIM_STYLE'],
        choices[i],
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation('TURN_ANIM_STYLE', choices[i]),
        isNotNull,
      );
    }
  });
  test('Checkmate choices retain their deployed wire values', () {
    const choices = <String>[
      'fireworks',
      'fire-lob',
      'ripple-bloom',
      'winner-sweep',
      'winner-constellation',
    ];
    for (var index = 0; index < choices.length; ++index) {
      final bytes = fixture();
      ByteData.sublistView(bytes).setUint16(14, index, Endian.little);
      expect(
        SettingsStateWireCodec.tryDecode(bytes)!.values['CHECKMATE_ANIM_STYLE'],
        choices[index],
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          'CHECKMATE_ANIM_STYLE',
          choices[index],
        ),
        <int>[2, 5, index, 0],
      );
    }
  });

  test(
    'Cauldron appends Hint style 4 without changing existing values or shape',
    () {
      const styles = [
        'foxie_sweep',
        'player_clockwise',
        'none',
        'beachball',
        'cauldron_brewing',
      ];
      for (var index = 0; index < styles.length; ++index) {
        final bytes = fixture();
        ByteData.sublistView(bytes).setUint16(80, index, Endian.little);
        expect(
          SettingsStateWireCodec.tryDecode(
            bytes,
          )?.values['HINT_THINKING_STYLE'],
          styles[index],
        );
        expect(
          SettingsStateWireCodec.tryEncodeMutation(
            'HINT_THINKING_STYLE',
            styles[index],
          ),
          <int>[2, 39, index, 0],
        );
        expect(bytes.length, 84);
      }
      final invalid = fixture();
      ByteData.sublistView(invalid).setUint16(80, styles.length, Endian.little);
      expect(SettingsStateWireCodec.tryDecode(invalid), isNull);
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          'HINT_THINKING_STYLE',
          'unknown',
        ),
        isNull,
      );
      expect(
        SettingsStateWireCodec.tryDecode(
          fixture(),
        )?.values['HINT_THINKING_STYLE'],
        'cauldron_brewing',
      );
    },
  );

  test('placement defaults to Ripple and preserves historical values', () {
    const styles = [
      'landing_flash',
      'mini_orbit',
      'ripple',
      'none',
      'rainbow_orbit',
    ];
    for (var index = 0; index < styles.length; ++index) {
      final bytes = fixture();
      ByteData.sublistView(bytes).setUint16(64, index, Endian.little);
      expect(
        SettingsStateWireCodec.tryDecode(bytes)?.values['SETTLE_ANIM_STYLE'],
        styles[index],
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          'SETTLE_ANIM_STYLE',
          styles[index],
        ),
        <int>[2, 30, index, 0],
      );
    }
    final invalid = fixture();
    ByteData.sublistView(invalid).setUint16(64, 5, Endian.little);
    expect(SettingsStateWireCodec.tryDecode(invalid), isNull);
    expect(
      SettingsStateWireCodec.tryDecode(fixture())?.values['SETTLE_ANIM_STYLE'],
      'ripple',
    );
  });

  test('decodes the shared complete settings snapshot', () {
    final state = SettingsStateWireCodec.tryDecode(fixture());

    expect(state, isNotNull);
    expect(state!.settingsRevision, 0);
    expect(state.values.length, 37);
    expect(state.values.containsKey('BLE_CONNECTION_INDICATOR'), isFalse);
    expect(state.values.containsKey('P1_X_DEEP_WAKE_ENABLED'), isFalse);
    expect(state.values.containsKey('P1_X_WAKE_HOLD_MS'), isFalse);
    expect(fixture().sublist(56, 60), <int>[0, 0, 0, 0]);
    expect(
      SettingsStateWireCodec.tryEncodeMutation('P1_X_DEEP_WAKE_ENABLED', true),
      isNull,
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('P1_X_WAKE_HOLD_MS', 1000),
      isNull,
    );
    expect(state.values.containsKey('V3_BUTTON_LED_EMULATION'), isFalse);
    expect(SettingsStateWireCodec.visibleKeys[33], isEmpty);
    expect(fixture().sublist(70, 72), <int>[0, 0]);
    expect(state.values['BRIGHTNESS'], 5);
    expect(state.values['WHITE_SQUARES_ON'], isTrue);
    expect(state.values['TURN_ANIM_STYLE'], 'active_end_pingpong_sides');
    expect(state.values['CHECKMATE_ANIM_STYLE'], 'fire-lob');
    expect(state.values['SLEEP_TIMEOUT'], '60m');
    expect(state.values['LIGHT_SQUARE_MODE'], 'white');
    expect(state.values['P1_END_PIECE_WHEEL'], 170);
  });

  test('rejects nonzero values in either retired reservation', () {
    for (final index in SettingsStateWireCodec.retiredVisibleIndices) {
      final bytes = fixture();
      ByteData.sublistView(bytes).setUint16(4 + index * 2, 1, Endian.little);
      expect(SettingsStateWireCodec.tryDecode(bytes), isNull);
    }
  });

  test('assembles one revision atomically and suppresses replay', () {
    final bytes = fixture();
    final assembler = SettingsStateWireAssembler();
    SettingsStateSnapshot? state;
    for (var index = 8; index >= 0; index -= 1) {
      state = assembler.accept(
        code: SettingsStateWireCodec.fragmentCode(index),
        revision: 77,
        body: Uint8List.sublistView(
          bytes,
          index * 10,
          index * 10 + SettingsStateWireCodec.fragmentLength(index),
        ),
      );
      expect(state == null, index != 0);
    }
    expect(state?.values['VS_CPU_PLAYER'], 2);
    expect(
      assembler.accept(
        code: SettingsStateWireCodec.firstStateCode,
        revision: 77,
        body: Uint8List.sublistView(bytes, 0, 10),
      ),
      isNull,
    );
  });

  test('clear retires Settings publication identity between connections', () {
    final assembler = SettingsStateWireAssembler();
    final bytes = fixture();
    SettingsStateSnapshot? deliver() {
      SettingsStateSnapshot? result;
      for (
        var index = 0;
        index < SettingsStateWireCodec.fragmentCount;
        ++index
      ) {
        result = assembler.accept(
          code: SettingsStateWireCodec.fragmentCode(index),
          revision: 77,
          body: Uint8List.sublistView(
            bytes,
            index * 10,
            index * 10 + SettingsStateWireCodec.fragmentLength(index),
          ),
        );
      }
      return result;
    }

    expect(deliver(), isNotNull);
    expect(
      deliver(),
      isNull,
      reason: 'same-lifetime replay remains suppressed',
    );
    assembler.clear();
    expect(
      deliver()?.values['BRIGHTNESS'],
      5,
      reason: 'a new connection may publish the identical content identity',
    );
  });

  test(
    'invalid complete Settings remains incomplete until valid authority',
    () {
      final assembler = SettingsStateWireAssembler();
      final bytes = fixture();
      void deliver(Uint8List payload, int revision) {
        for (
          var index = 0;
          index < SettingsStateWireCodec.fragmentCount;
          ++index
        ) {
          assembler.accept(
            code: SettingsStateWireCodec.fragmentCode(index),
            revision: revision,
            body: Uint8List.sublistView(
              payload,
              index * 10,
              index * 10 + SettingsStateWireCodec.fragmentLength(index),
            ),
          );
        }
      }

      expect(assembler.isIncomplete, isFalse);
      deliver(bytes, 3);
      expect(assembler.isIncomplete, isFalse);
      final malformed = Uint8List.fromList(bytes)..[70] = 1;
      deliver(malformed, 2);
      expect(
        assembler.isIncomplete,
        isTrue,
        reason: 'rejected bytes must not retire recovery',
      );
      deliver(bytes, 3);
      expect(assembler.isIncomplete, isFalse);
      assembler.clear();
      expect(assembler.isIncomplete, isFalse);
    },
  );

  test(
    'requires style tail, rejects wrong code and never mixes identities',
    () {
      final bytes = fixture();
      final assembler = SettingsStateWireAssembler();
      for (var i = 0; i < 8; ++i) {
        expect(
          assembler.accept(
            code: 14 + i,
            revision: 11,
            body: Uint8List.sublistView(bytes, i * 10, i * 10 + 10),
          ),
          isNull,
        );
      }
      expect(assembler.isIncomplete, isTrue);
      expect(
        SettingsStateWireCodec.tryDecode(Uint8List.sublistView(bytes, 0, 80)),
        isNull,
      );
      expect(
        assembler.accept(
          code: 22,
          revision: 11,
          body: Uint8List.sublistView(bytes, 80),
        ),
        isNull,
      );
      expect(assembler.isIncomplete, isTrue);
      expect(
        assembler.accept(
          code: 39,
          revision: 12,
          body: Uint8List.sublistView(bytes, 80),
        ),
        isNull,
      );
      SettingsStateSnapshot? state;
      for (var i = 0; i < 8; ++i) {
        state = assembler.accept(
          code: 14 + i,
          revision: 12,
          body: Uint8List.sublistView(bytes, i * 10, i * 10 + 10),
        );
      }
      expect(state?.values['HINT_THINKING_STYLE'], 'cauldron_brewing');
      final invalid = Uint8List.fromList(bytes)..[80] = 5;
      expect(SettingsStateWireCodec.tryDecode(invalid), isNull);
      invalid[80] = 2;
      expect(
        SettingsStateWireCodec.tryDecode(
          invalid,
        )?.values['HINT_THINKING_STYLE'],
        'none',
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation('HINT_THINKING_STYLE', 'none'),
        <int>[2, 39, 2, 0],
      );
      invalid[80] = 1;
      expect(
        SettingsStateWireCodec.tryDecode(
          invalid,
        )?.values['HINT_THINKING_STYLE'],
        'player_clockwise',
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          'HINT_THINKING_STYLE',
          'player_clockwise',
        ),
        <int>[2, 39, 1, 0],
      );
      expect(
        SettingsStateWireCodec.tryEncodeMutation(
          'HINT_THINKING_STYLE',
          'other',
        ),
        isNull,
      );
    },
  );

  test('encodes typed mutations and rejects mismatched types', () {
    expect(
      SettingsStateWireCodec.tryEncodeMutation('BOARD_ROTATE', true),
      <int>[2, 10, 1, 0],
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('LIGHT_SQUARE_MODE', 'color'),
      <int>[2, 34, 1, 0],
    );
    expect(SettingsStateWireCodec.tryEncodeMutation('BOARD_ROTATE', 1), isNull);
    expect(
      SettingsStateWireCodec.tryEncodeMutation(
        'BLE_CONNECTION_INDICATOR',
        true,
      ),
      isNull,
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('V3_BUTTON_LED_EMULATION', true),
      isNull,
    );
    expect(SettingsStateWireCodec.tryEncodeMutation('', 0), isNull);
    expect(
      SettingsStateWireCodec.tryEncodeMutation('P1_END_PIECE_WHEEL', 42),
      <int>[2, 37, 42, 0],
    );
    expect(
      SettingsStateWireCodec.tryEncodeMutation('LIGHT_SQUARE_BRIGHTNESS', 7),
      <int>[2, 36, 7, 0],
    );
  });
}
