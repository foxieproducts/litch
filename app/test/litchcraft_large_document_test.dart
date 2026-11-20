import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_rickroll_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';

void main() {
  for (final count in [32, 256]) {
    test(
      '$count frames save/recover/edit locally; legacy transport refuses',
      () {
        final preset = LitchcraftRickrollPreset.decode();
        final document = LitchcraftAnimationDocument(
          frames: List.generate(count, (i) => preset.frames[i % 32]),
          topology: LitchcraftTopology.v3,
          favoriteColors: litchcraftDefaultFavoriteColors,
        );
        final session = LitchcraftDocumentSession(document);
        expect(session.document.playbackOnEntry, isFalse);
        session.selectFrame(count - 1);
        session.beginStroke();
        session.paintSelectedFrame(
          squares: [7],
          mask: 15,
          color: 0xabcdef,
          history: false,
        );
        session.endStroke();
        expect(session.document.frames.last.emitters[28].color, 0xabcdef);
        session.undo();
        expect(sameLitchcraftDocument(session.document, document), isTrue);
        session.redo();
        expect(session.document.frames.last.emitters[28].color, 0xabcdef);
        final now = DateTime.utc(2026);
        final artwork = LitchcraftArtwork.animated(
          id: 'large-$count',
          name: 'Large artwork',
          document: session.document,
          createdAt: now,
          modifiedAt: now,
        );
        final saved = artwork.toJson();
        expect(saved['version'], 4);
        final restored = LitchcraftArtwork.fromJson(saved);
        expect(restored.isSupported, isTrue);
        expect(
          sameLitchcraftDocument(restored.document, session.document),
          isTrue,
        );
        expect(
          () => encodeLitchcraftAnimationDocument(document),
          throwsFormatException,
        );
        final encoded = encodeLitchcraftAnimationDocument(
          document,
          localStorage: true,
        );
        expect(
          () => decodeLitchcraftAnimationDocument(encoded.bytes, encoded.crc32),
          throwsFormatException,
        );
        if (count == 256) {
          expect(session.addFrame, throwsA(isA<LitchcraftFrameLimit>()));
        } else {
          session.addFrame();
          expect(session.document.frames, hasLength(33));
        }
      },
    );
  }
}
