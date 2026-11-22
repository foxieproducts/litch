import 'dart:convert';

import 'package:characters/characters.dart';

/// Display names may contain international text and emoji, but never hidden
/// controls, malformed UTF-16, or directional overrides that disguise a label.
/// Length and empty-value policy belong to each field's contract.
bool isSafeDisplayText(String value) => !value.runes.any(
  (rune) =>
      rune < 0x20 ||
      (rune >= 0x7f && rune <= 0x9f) ||
      (rune >= 0xd800 && rune <= 0xdfff) ||
      (rune >= 0x2028 && rune <= 0x202e) ||
      (rune >= 0x2066 && rune <= 0x2069),
);

/// Both limits are necessary: a grapheme can contain many UTF-8 bytes.
/// Reject oversize values; callers must not silently truncate a saved name.
bool isDisplayTextWithinBounds(
  String value, {
  required int maxCharacters,
  required int maxBytes,
}) =>
    isSafeDisplayText(value) &&
    value.characters.length <= maxCharacters &&
    utf8.encode(value).length <= maxBytes;
