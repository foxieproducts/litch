import 'dart:convert';

import 'package:flutter/services.dart';

import 'litch_setting.dart';

/// Local editing hints. Firmware still validates every submitted command.
class ConsoleCompletion {
  const ConsoleCompletion(this.label, this.value);

  final String label;
  final TextEditingValue value;
}

const _commands = <String>[
  'help',
  'version',
  'uptime',
  'game',
  'board',
  'input',
  'settings',
  'set',
  'ble',
  'ledpower',
  'cc',
  'ambient',
  'motion',
  'perf',
  'heap',
  'support',
  'trace',
  'debug',
  'logs',
  'i2c',
  'setled',
  'rotate',
  'firstplayer',
  'sleep',
  'reboot',
  'safemode',
  'amptest',
  'comptest',
  'hwtest',
  'factory',
  'litchcraft',
  'fen',
  'engine',
  'fox',
];

const _arguments = <String, List<String>>{
  'fox': ['hello', 'fortune', 'quack'],
  'game': [
    'status',
    'new',
    'undo',
    'mode',
    'hint',
    'placement',
    'scrub',
    'bench',
  ],
  'board': ['status', 'resync', 'hallmap', 'stress'],
  'input': ['scan'],
  'settings': [
    'status',
    'keys',
    'get',
    'set',
    'save',
    'dump',
    'paste',
    'clear',
    'format',
  ],
  'ble': ['status', 'on', 'off', 'test'],
  'ledpower': ['status', 'clear', 'latch'],
  'cc': ['read', 'show', 'clear', 'set'],
  'ambient': ['status', 'on', 'off'],
  'motion': ['status', 'sample', 'events', 'calibrate', 'apply'],
  'trace': ['record', 'replay'],
  'debug': ['info', 'trace', 'debug', 'warn', 'error'],
  'i2c': ['status', 'set'],
  'setled': ['board', 'clear'],
  'sleep': ['status', 'enter'],
  'reboot': ['nosave', 'bootloader'],
  'safemode': ['status', 'clear', 'reboot'],
  'amptest': ['status', 'verify', 'start', 'stop', 'restart'],
  'comptest': [
    'status',
    'start',
    'core',
    'rail',
    'led',
    'ble',
    'combined',
    'stop',
  ],
  'hwtest': [
    'status',
    'start',
    'restart',
    'stop',
    'confirm',
    'verifier',
    'fault',
    'forceexit',
  ],
  'factory': ['clear'],
  'litchcraft': ['clear'],
  'engine': ['fen', 'reset'],
};

final _space = RegExp(r'[ \t]+');
final _safeWord = RegExp(r'^[A-Za-z0-9_-]+$');

/// Offers token replacements only for a collapsed caret outside composition.
List<ConsoleCompletion> completeConsoleDraft(
  TextEditingValue draft, {
  Iterable<LitchSetting> settings = const [],
}) {
  final text = draft.text;
  final selection = draft.selection;
  if (text.isEmpty ||
      text.length > 254 ||
      !selection.isValid ||
      !selection.isCollapsed ||
      selection.extentOffset > text.length ||
      (draft.composing.isValid && !draft.composing.isCollapsed) ||
      text.contains('\r') ||
      text.contains('\n') ||
      text.contains('"') ||
      text.contains("'")) {
    return const [];
  }
  final caret = selection.extentOffset;
  var start = caret;
  var end = caret;
  while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '\t') {
    start--;
  }
  while (end < text.length && text[end] != ' ' && text[end] != '\t') {
    end++;
  }
  final prefix = text.substring(start, caret);
  if (prefix.isNotEmpty && !_safeWord.hasMatch(prefix)) return const [];
  final before = text.substring(0, start).trim();
  final words = before.isEmpty ? const <String>[] : before.split(_space);
  final candidates = _candidates(words, settings);
  if (words.isEmpty && prefix.isEmpty) return const [];
  final completions = <ConsoleCompletion>[];
  for (final word in candidates) {
    if (!_safeWord.hasMatch(word) ||
        !word.toLowerCase().startsWith(prefix.toLowerCase())) {
      continue;
    }
    final replacement = end == text.length ? '$word ' : word;
    final completed = text.replaceRange(start, end, replacement);
    if (utf8.encode(completed).length > 254) continue;
    completions.add(
      ConsoleCompletion(
        word,
        TextEditingValue(
          text: completed,
          selection: TextSelection.collapsed(
            offset: start + replacement.length,
          ),
        ),
      ),
    );
    if (completions.length == 3) break;
  }
  return completions;
}

Iterable<String> _candidates(
  List<String> before,
  Iterable<LitchSetting> settings,
) {
  if (before.isEmpty) return _commands;
  // Only the token being replaced may ignore case; preceding literals must
  // already match the firmware's case-sensitive command vocabulary.
  final command = before.first;
  if (before.length == 1) {
    if (command == 'help') return _commands;
    if (command == 'set') return settings.map((setting) => setting.key);
    return _arguments[command] ?? const [];
  }
  final argument = before[1];
  if (command == 'settings' &&
      before.length == 2 &&
      (argument == 'get' || argument == 'set')) {
    return settings.map((setting) => setting.key);
  }
  String? settingKey;
  if (command == 'set' && before.length == 2) settingKey = before[1];
  if (command == 'settings' && argument == 'set' && before.length == 3) {
    settingKey = before[2];
  }
  if (settingKey != null) {
    for (final setting in settings) {
      if (setting.key.toLowerCase() != settingKey.toLowerCase()) continue;
      if (setting.type == 'bool') return const ['false', 'true'];
      if (setting.type == 'enum') return setting.options;
      return const [];
    }
  }
  if (before.length != 2) return const [];
  if (command == 'game' && argument == 'mode') return const ['human', 'cpu'];
  if (command == 'game' && argument == 'hint') return const ['stats'];
  if (command == 'trace' && argument == 'record') return const ['on', 'off'];
  if (command == 'i2c' && argument == 'set') {
    return const ['100k', '400k', '1m'];
  }
  if (command == 'amptest' && argument == 'verify') {
    return const ['status', 'start', 'stop', 'restart'];
  }
  if (command == 'board' && argument == 'hallmap') {
    return const ['status', 'start', 'stop', 'reset'];
  }
  return const [];
}
