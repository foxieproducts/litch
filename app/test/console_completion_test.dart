import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/console_completion.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';

void main() {
  TextEditingValue draft(String text, {int? caret}) => TextEditingValue(
    text: text,
    selection: TextSelection.collapsed(offset: caret ?? text.length),
  );
  List<String> labels(String text, {List<LitchSetting> settings = const []}) =>
      completeConsoleDraft(
        draft(text),
        settings: settings,
      ).map((choice) => choice.label).toList();

  test('command and argument completion edits one token at the caret', () {
    final choice = completeConsoleDraft(draft('fo')).single;
    expect(choice.label, 'fox');
    expect(choice.value.text, 'fox ');
    expect(choice.value.selection, const TextSelection.collapsed(offset: 4));
    expect(labels('fox '), ['hello', 'fortune', 'quack']);
    expect(labels('fox F'), ['fortune']);
    final middle = completeConsoleDraft(
      draft('game moXXX cpu', caret: 7),
    ).single;
    expect(middle.value.text, 'game mode cpu');
    expect(middle.value.selection, const TextSelection.collapsed(offset: 9));
    expect(labels('amptest verify st'), ['status', 'start', 'stop']);
    expect(labels('game mode '), ['human', 'cpu']);
    expect(labels('board\thallmap re'), ['reset']);
  });

  test(
    'completion respects case-sensitive commands and preserves literals',
    () {
      const settings = [
        LitchSetting(
          key: 'BOARD_STYLE',
          type: 'enum',
          category: 'board',
          value: 'Classic',
          defaultValue: 'Classic',
          options: ['Classic', 'Modern'],
        ),
      ];
      expect(completeConsoleDraft(draft('FO')).single.value.text, 'fox ');
      for (final text in [
        'FOX f',
        'GAME mode c',
        'game MODE c',
        'SET board_style M',
        'settings SET BOARD_STYLE M',
      ]) {
        expect(labels(text, settings: settings), isEmpty, reason: text);
      }
      final value = completeConsoleDraft(
        draft('settings set board_style m'),
        settings: settings,
      ).single;
      expect(value.label, 'Modern');
      expect(value.value.text, 'settings set board_style Modern ');
      final middle = completeConsoleDraft(
        draft('game MO human', caret: 7),
      ).single;
      expect(middle.value.text, 'game mode human');
    },
  );

  test('settings use only supplied keys and finite typed choices', () {
    const settings = [
      LitchSetting(
        key: 'LED_ENABLED',
        type: 'bool',
        category: 'led',
        value: true,
        defaultValue: true,
      ),
      LitchSetting(
        key: 'BOARD_STYLE',
        type: 'enum',
        category: 'board',
        value: 'Classic',
        defaultValue: 'Classic',
        options: ['Classic', 'Modern', 'unsafe value'],
      ),
      LitchSetting(
        key: 'LED_BRIGHTNESS',
        type: 'int',
        category: 'led',
        value: 50,
        defaultValue: 50,
      ),
    ];
    expect(labels('settings get LED_', settings: settings), [
      'LED_ENABLED',
      'LED_BRIGHTNESS',
    ]);
    expect(labels('set led_e', settings: settings), ['LED_ENABLED']);
    expect(labels('settings set LED_ENABLED ', settings: settings), [
      'false',
      'true',
    ]);
    expect(labels('set BOARD_STYLE M', settings: settings), ['Modern']);
    expect(labels('set BOARD_STYLE ', settings: settings), [
      'Classic',
      'Modern',
    ]);
    expect(labels('set LED_BRIGHTNESS ', settings: settings), isEmpty);
    expect(labels('settings get '), isEmpty);
  });

  test('selected, composing, multiline and literal input stays untouched', () {
    for (final value in [
      const TextEditingValue(
        text: 'fo',
        selection: TextSelection(baseOffset: 0, extentOffset: 2),
      ),
      draft('fo').copyWith(composing: const TextRange(start: 0, end: 2)),
      const TextEditingValue(text: 'fo'),
      draft('fo').copyWith(selection: const TextSelection.collapsed(offset: 3)),
      draft('fox\nfo'),
      draft('fox\rfo'),
      draft('set KEY "fo'),
      draft("set KEY 'fo"),
      draft('fen rnbqkbnr/pppppppp'),
      draft('123'),
      draft(''),
      draft(' '),
    ]) {
      expect(completeConsoleDraft(value), isEmpty, reason: '$value');
    }
    expect(
      labels('settings format '),
      isEmpty,
      reason: 'completion never supplies a destructive confirmation',
    );
  });

  test('suggestions stay bounded and never produce an oversized UTF8 line', () {
    expect(labels('s').length, lessThanOrEqualTo(3));
    final boundary = '${' ' * 250}fo';
    expect(completeConsoleDraft(draft(boundary)).single.value.text.length, 254);
    expect(completeConsoleDraft(draft(' $boundary')), isEmpty);
    expect(completeConsoleDraft(draft('fo ${'x' * 251}', caret: 2)), isEmpty);
    expect(completeConsoleDraft(draft('fo ${'é' * 126}', caret: 2)), isEmpty);
  });
}
