import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/user_text.dart';

void main() {
  test('display name limits enforce bytes independently of grapheme count', () {
    expect(
      isDisplayTextWithinBounds('棋' * 20, maxCharacters: 20, maxBytes: 60),
      isTrue,
    );
    expect(
      isDisplayTextWithinBounds('棋' * 20, maxCharacters: 20, maxBytes: 59),
      isFalse,
    );
    expect(
      isDisplayTextWithinBounds('a' * 21, maxCharacters: 20, maxBytes: 60),
      isFalse,
    );
  });
  test('display names retain international text and joined emoji', () {
    expect(isSafeDisplayText('José 棋盤 لوحة 👨‍👩‍👧'), isTrue);
  });

  test('domain names reject controls through API as well as UI', () {
    for (final name in <String>[
      'A\u0000B',
      'A\u001b[31m',
      'A\u0085B',
      'A\u202eB',
      'A\ud800B',
    ]) {
      expect(isGameArchiveDisplayNameWithinBounds(name), isFalse);
      expect(
        () => LitchcraftArtwork(
          id: 'safe-id',
          name: name,
          canvas: LitchcraftCanvas.blank(topology: LitchcraftTopology.v1),
          createdAt: DateTime.utc(2026),
          modifiedAt: DateTime.utc(2026),
        ),
        throwsFormatException,
      );
    }
  });
}
