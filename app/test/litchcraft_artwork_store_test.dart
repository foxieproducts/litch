import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:crypto/crypto.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/app_litchcraft_preferences.dart';
import 'package:shared_preferences/shared_preferences.dart';

LitchcraftArtwork artwork({String id = 'corners', String name = 'Corners'}) =>
    LitchcraftArtwork(
      id: id,
      name: name,
      canvas: LitchcraftCanvas.blank(topology: LitchcraftTopology.v1)
          .paint(<int>[0], 1, 0xff1234)
          .paint(<int>[7], 2, 0x123456)
          .paint(<int>[56], 4, 0xabcdef)
          .paint(<int>[63], 8, 0x111111),
      createdAt: DateTime.utc(2026, 9, 9),
      modifiedAt: DateTime.utc(2026, 9, 9, 1),
    );

Map<String, Object?> legacyArtworkJson(int version) {
  final source = artwork();
  return <String, Object?>{
    'version': version,
    'id': source.id,
    'name': source.name,
    'createdAt': source.createdAt.toIso8601String(),
    'modifiedAt': source.modifiedAt.toIso8601String(),
    'topology': source.canvas.topology.name,
    'mapping': 'a8-first-left-top-right-bottom',
    'colors': source.canvas.colors,
    if (version == 2)
      'favoriteColors': const <int>[
        0x010203,
        0x111213,
        0x212223,
        0x313233,
        0x414243,
        0x515253,
        0x616263,
        0x717273,
      ],
  };
}

LitchcraftArtwork animatedArtwork({
  String id = 'motion',
  String name = 'Motion',
}) {
  LitchcraftAnimationFrame frame(
    int color,
    LitchcraftEffectStyle effect,
    int frameDuration,
    int effectDuration,
  ) => LitchcraftAnimationFrame(
    durationMs: frameDuration,
    emitters: List<LitchcraftAnimatedEmitter>.generate(
      256,
      (index) => LitchcraftAnimatedEmitter(
        color: color + index,
        effect: index == 7 ? effect : LitchcraftEffectStyle.off,
        effectDurationMs: index == 7 ? effectDuration : 0,
      ),
    ),
  );

  return LitchcraftArtwork.animated(
    id: id,
    name: name,
    document: LitchcraftAnimationDocument(
      frames: <LitchcraftAnimationFrame>[
        frame(0x010000, LitchcraftEffectStyle.sparkle, 900, 1700),
        frame(0x020000, LitchcraftEffectStyle.bounce, 2300, 3100),
      ],
      topology: LitchcraftTopology.v3,
      favoriteColors: const <int>[
        0x010203,
        0x111213,
        0x212223,
        0x313233,
        0x414243,
        0x515253,
        0x616263,
        0x717273,
      ],
      playbackOnEntry: true,
      sparkleSeed: 0xfedcba98,
    ),
    createdAt: DateTime.utc(2026, 9, 9),
    modifiedAt: DateTime.utc(2026, 9, 9, 1),
  );
}

void main() {
  test(
    'schema2 duplicate ordered favorites survive migration and storage',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final favorites = [0x123456, 0x123456, 0, 0xabcdef, 0, 5, 5, 0xffffff];
      final source = legacyArtworkJson(2)..['favoriteColors'] = favorites;
      final migrated = LitchcraftArtwork.fromJson(source);
      expect(migrated.favoriteColors, favorites);
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      await store.save(
        LitchcraftLibrarySnapshot(artworks: [migrated], draft: migrated),
      );
      final restored = await store.load();
      expect(restored.draft!.favoriteColors, favorites);
      expect(restored.artworks.single.favoriteColors, favorites);
      expect(restored.rejected, isFalse);
    },
  );
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() => SharedPreferences.setMockInitialValues(<String, Object>{}));

  test(
    'editable source round trip preserves all semantic detail and metadata',
    () {
      final original = artwork();
      final restored = LitchcraftArtwork.fromJson(
        jsonDecode(jsonEncode(original.toJson())),
      );
      expect(restored.canvas, original.canvas);
      expect(restored.createdAt, original.createdAt);
      expect(restored.modifiedAt, original.modifiedAt);
      expect(restored.canvas.colors[7 * 4 + 1], 0x123456);
      expect(restored.canvas.colors[63 * 4 + 3], 0x111111);
    },
  );

  test('schema 3 round trip preserves the complete animated document', () {
    final original = animatedArtwork();

    final restored = LitchcraftArtwork.fromJson(
      jsonDecode(jsonEncode(original.toJson())),
    );

    expect(restored.toJson(), original.toJson());
    expect(restored.document.frames, hasLength(2));
    expect(
      restored.document.frames[1].emitters[7].effect,
      LitchcraftEffectStyle.bounce,
    );
    expect(restored.document.frames[1].emitters[7].effectDurationMs, 3100);
    expect(restored.document.frames[1].durationMs, 2300);
    expect(restored.favoriteColors, original.favoriteColors);
    expect(restored.document.sparkleSeed, 0xfedcba98);
    expect(restored.document.playbackOnEntry, isTrue);
    expect(restored.document.topology, LitchcraftTopology.v3);
  });

  test(
    'schema 3 preserves every supported frame count from one through eight',
    () {
      for (var frameCount = 1; frameCount <= 8; frameCount++) {
        final original = LitchcraftArtwork.animated(
          id: 'frames-$frameCount',
          name: '$frameCount frames',
          document: LitchcraftAnimationDocument(
            frames: List<LitchcraftAnimationFrame>.generate(
              frameCount,
              (frameIndex) => LitchcraftAnimationFrame(
                durationMs: 100 * (frameIndex + 1),
                emitters: List<LitchcraftAnimatedEmitter>.generate(
                  256,
                  (emitterIndex) => LitchcraftAnimatedEmitter(
                    color: frameIndex << 16 | emitterIndex,
                    effect: emitterIndex == frameIndex
                        ? LitchcraftEffectStyle.values[frameIndex + 1]
                        : LitchcraftEffectStyle.off,
                    effectDurationMs: emitterIndex == frameIndex
                        ? 500 + frameIndex * 100
                        : 0,
                  ),
                ),
              ),
            ),
            topology: LitchcraftTopology.v2,
            favoriteColors: litchcraftDefaultFavoriteColors,
            sparkleSeed: frameCount,
          ),
          createdAt: DateTime.utc(2026, 9, 9),
          modifiedAt: DateTime.utc(2026, 9, 9, 1),
        );

        final envelope = original.toJson();
        final restored = LitchcraftArtwork.fromJson(
          jsonDecode(jsonEncode(envelope)),
        );

        expect(restored.toJson(), envelope, reason: 'frameCount=$frameCount');
        expect(restored.document.frames, hasLength(frameCount));
        expect(
          utf8.encode(jsonEncode(envelope)).length,
          lessThanOrEqualTo(litchcraftArtworkMaximumEnvelopeBytes),
        );
      }
    },
  );

  test(
    'rejects unknown schema topology malformed color and mismatched mapping',
    () {
      for (final change in <Map<String, Object?>>[
        <String, Object?>{'topology': 'v99'},
        <String, Object?>{
          'colors': <int>[1],
        },
        <String, Object?>{'colors': List<int>.filled(256, -1)},
        <String, Object?>{'mapping': 'reversed'},
        <String, Object?>{'createdAt': 'not a date'},
      ]) {
        expect(
          () => LitchcraftArtwork.fromJson(<String, Object?>{
            ...legacyArtworkJson(2),
            ...change,
          }),
          throwsFormatException,
        );
      }
    },
  );

  test('version 1 artwork migrates with the bounded default favourites', () {
    final restored = LitchcraftArtwork.fromJson(legacyArtworkJson(1));
    expect(restored.favoriteColors, litchcraftDefaultFavoriteColors);
    expect(restored.document.frames, hasLength(1));
    expect(
      restored.document.frames.single.emitters.map((emitter) => emitter.color),
      artwork().canvas.colors,
    );
    expect(
      restored.document.frames.single.emitters,
      everyElement(
        isA<LitchcraftAnimatedEmitter>()
            .having(
              (emitter) => emitter.effect,
              'effect',
              LitchcraftEffectStyle.off,
            )
            .having((emitter) => emitter.effectDurationMs, 'duration', 0),
      ),
    );
  });

  test('version 2 artwork migration preserves its eight favourites', () {
    final restored = LitchcraftArtwork.fromJson(legacyArtworkJson(2));

    expect(restored.document.frames, hasLength(1));
    expect(restored.favoriteColors, legacyArtworkJson(2)['favoriteColors']);
    expect(restored.document.topology, artwork().canvas.topology);
  });

  test(
    'unknown and corrupt advanced artwork retain their source as unavailable',
    () async {
      final valid = animatedArtwork().toJson();
      final unknown = <String, Object?>{
        ...valid,
        'id': 'future-motion',
        'version': 5,
      };
      final corrupt = <String, Object?>{
        ...valid,
        'id': 'corrupt-motion',
        'crc32': 0,
      };

      final unknownResult = LitchcraftArtwork.fromJson(unknown);
      final corruptResult = LitchcraftArtwork.fromJson(corrupt);

      expect(unknownResult.isSupported, isFalse);
      expect(
        unknownResult.unavailableReason,
        LitchcraftArtworkUnavailableReason.unsupportedSchema,
      );
      expect(unknownResult.toJson(), unknown);
      expect(corruptResult.isSupported, isFalse);
      expect(
        corruptResult.unavailableReason,
        LitchcraftArtworkUnavailableReason.corruptAdvancedDocument,
      );
      expect(corruptResult.toJson(), corrupt);
      expect(
        () => corruptResult.document,
        throwsA(isA<LitchcraftUnsupportedArtwork>()),
      );

      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      await store.save(
        LitchcraftLibrarySnapshot(
          artworks: <LitchcraftArtwork>[
            artwork(),
            unknownResult,
            corruptResult,
          ],
        ),
      );
      final restored = await store.load();
      expect(restored.rejected, isFalse);
      expect(restored.artworks, hasLength(3));
      expect(restored.artworks[1].toJson(), unknown);
      expect(restored.artworks[2].toJson(), corrupt);
    },
  );

  test('truncated advanced body is retained and never restored as a still', () {
    final source = animatedArtwork().toJson();
    final bytes = base64Decode(source['document']! as String);
    final truncated = <String, Object?>{
      ...source,
      'document': base64Encode(bytes.sublist(0, bytes.length - 1)),
    };

    final restored = LitchcraftArtwork.fromJson(truncated);

    expect(restored.isSupported, isFalse);
    expect(
      restored.unavailableReason,
      LitchcraftArtworkUnavailableReason.corruptAdvancedDocument,
    );
    expect(restored.toJson(), truncated);
    expect(() => restored.canvas, throwsA(isA<LitchcraftUnsupportedArtwork>()));
  });

  test('oversized advanced source is rejected before library adoption', () {
    final oversized = <String, Object?>{
      ...animatedArtwork().toJson(),
      'version': 4,
      'future': 'x' * litchcraftArtworkMaximumEnvelopeBytes,
    };

    expect(() => LitchcraftArtwork.fromJson(oversized), throwsFormatException);
  });

  test('byte allowance admits more than 64 works plus both drafts', () async {
    final maximum = LitchcraftArtwork.animated(
      id: 'maximum',
      name: 'Maximum',
      document: LitchcraftAnimationDocument(
        frames: List<LitchcraftAnimationFrame>.generate(
          8,
          (frameIndex) => LitchcraftAnimationFrame(
            durationMs: 1000 + frameIndex * 100,
            emitters: List<LitchcraftAnimatedEmitter>.generate(
              256,
              (emitterIndex) => LitchcraftAnimatedEmitter(
                color: frameIndex << 16 | emitterIndex,
                effect: LitchcraftEffectStyle.values[frameIndex + 1],
                effectDurationMs: 500 + frameIndex * 100,
              ),
            ),
          ),
        ),
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        sparkleSeed: 0xffffffff,
      ),
      createdAt: DateTime.utc(2026, 9, 9),
      modifiedAt: DateTime.utc(2026, 9, 9, 1),
    );
    final snapshot = LitchcraftLibrarySnapshot(
      artworks: List<LitchcraftArtwork>.generate(
        80,
        (index) =>
            maximum.copyWith(id: 'maximum-$index', name: 'Maximum $index'),
      ),
      draft: maximum.copyWith(id: 'draft'),
      previousDraft: maximum.copyWith(id: 'previous'),
    );
    final prefs = await SharedPreferences.getInstance();
    final store = LitchcraftArtworkStore(prefs);

    await store.save(snapshot);

    expect((await store.load()).artworks, hasLength(80));
    final currentBytes = utf8
        .encode(prefs.getString(LitchcraftArtworkStore.currentKey)!)
        .length;
    expect(currentBytes, lessThan(store.storageLimitBytes));
  });

  test(
    'oversized unreadable current stays in place without filling recovery',
    () async {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setInt(AppLitchcraftPreferences.storageLimitKey, 1000000);
      final oversized = '{"future":"${'x' * 1000000}"}';
      await prefs.setString(LitchcraftArtworkStore.currentKey, oversized);
      final store = LitchcraftArtworkStore(prefs);

      await expectLater(
        store.save(LitchcraftLibrarySnapshot(draft: artwork())),
        throwsA(isA<LitchcraftStorageLimitExceeded>()),
      );

      expect(prefs.getString(LitchcraftArtworkStore.currentKey), oversized);
      expect(prefs.containsKey(LitchcraftArtworkStore.rejectedKey), isFalse);
    },
  );

  test(
    'library saves artwork draft recovery canvas and recent colors together',
    () async {
      final store = LitchcraftArtworkStore(
        await SharedPreferences.getInstance(),
      );
      await store.save(
        LitchcraftLibrarySnapshot(
          artworks: <LitchcraftArtwork>[artwork()],
          draft: artwork(id: 'draft'),
          previousDraft: artwork(id: 'previous'),
          recentColors: <int>[0x123456],
          dismissedWelcome: true,
        ),
      );
      final loaded = await store.load();
      expect(loaded.artworks.single.canvas, artwork().canvas);
      expect(loaded.draft!.id, 'draft');
      expect(loaded.previousDraft!.id, 'previous');
      expect(loaded.recentColors, <int>[0x123456]);
      expect(loaded.dismissedWelcome, isTrue);
    },
  );

  test(
    'interrupted or corrupt current write recovers previous valid document',
    () async {
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      await store.save(
        LitchcraftLibrarySnapshot(
          artworks: <LitchcraftArtwork>[artwork(name: 'First')],
        ),
      );
      await store.save(
        LitchcraftLibrarySnapshot(
          artworks: <LitchcraftArtwork>[artwork(name: 'Second')],
        ),
      );
      await prefs.setString(LitchcraftArtworkStore.currentKey, '{interrupted');
      final loaded = await store.load();
      expect(loaded.recovered, isTrue);
      expect(loaded.rejected, isTrue);
      expect(loaded.artworks.single.name, 'First');
      await store.save(loaded);
      expect(
        prefs.getString(LitchcraftArtworkStore.rejectedKey),
        '{interrupted',
      );
    },
  );

  test(
    'invalid sole record stays untouched until an explicit validated write',
    () async {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(
        LitchcraftArtworkStore.currentKey,
        '{"version":99}',
      );
      final store = LitchcraftArtworkStore(prefs);
      final result = await store.load();
      expect(result.rejected, isTrue);
      expect(result.artworks, isEmpty);
      expect(
        prefs.getString(LitchcraftArtworkStore.currentKey),
        '{"version":99}',
      );
    },
  );

  test(
    'serialized saves leave newest whole document with last validated backup',
    () async {
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      await Future.wait(<Future<void>>[
        store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'One'))),
        store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'Two'))),
        store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'Three'))),
      ]);
      expect((await store.load()).draft!.name, 'Three');
      final backup = jsonDecode(
        prefs.getString(LitchcraftArtworkStore.backupKey)!,
      );
      expect(backup['draft']['name'], 'Two');
    },
  );

  test(
    'duplicate identities fail before overwriting current document',
    () async {
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      await store.save(LitchcraftLibrarySnapshot(draft: artwork()));
      final previous = prefs.getString(LitchcraftArtworkStore.currentKey);
      expect(
        () => store.save(
          LitchcraftLibrarySnapshot(
            artworks: <LitchcraftArtwork>[artwork(), artwork()],
          ),
        ),
        throwsFormatException,
      );
      expect(prefs.getString(LitchcraftArtworkStore.currentKey), previous);
    },
  );

  group('per-artwork file storage', () {
    late Directory directory;
    late SharedPreferences preferences;
    setUp(() async {
      directory = await Directory.systemTemp.createTemp('litchcraft-store-');
      preferences = await SharedPreferences.getInstance();
    });
    tearDown(() async {
      await directory.delete(recursive: true);
    });

    Future<LitchcraftArtworkStore> open() => LitchcraftArtworkStore.open(
      preferences: preferences,
      directory: directory,
    );
    File index(String role) => File('${directory.path}/$role.json');
    Future<List<File>> blobs() async =>
        (await Directory('${directory.path}/artworks').list().toList())
            .whereType<File>()
            .where((file) => file.path.endsWith('.json'))
            .toList();

    test('physical five-work shape saves a sixth 256-frame copy', () async {
      final store = await open();
      final maximum = maximumArtwork();
      final first = LitchcraftLibrarySnapshot(
        artworks: [
          for (var i = 0; i < 3; i++) maximum.copyWith(id: 'large-$i'),
          artwork(id: 'small-1'),
          artwork(id: 'small-2'),
        ],
        draft: maximum.copyWith(id: 'draft'),
        previousDraft: maximumArtwork(id: 'previous', frames: 255),
      );
      final sixth = maximum.copyWith(id: 'large-3');
      final enlarged = LitchcraftLibrarySnapshot(
        artworks: [...first.artworks, sixth],
        draft: first.draft,
        previousDraft: first.previousDraft,
      );
      const oldSnapshotBound = 8 * 1024 * 1024 ~/ 3;
      expect(
        utf8.encode(jsonEncode(first.toJson())).length,
        lessThan(oldSnapshotBound),
      );
      expect(
        utf8.encode(jsonEncode(enlarged.toJson())).length,
        greaterThan(oldSnapshotBound),
      );
      await store.save(first);
      await store.save(enlarged);
      final restarted = await open();
      final loaded = await restarted.load();
      expect(loaded.artworks, hasLength(6));
      expect(loaded.draft!.frameCount, 256);
      expect(loaded.previousDraft!.frameCount, 255);
      expect(loaded.artworks.last.frameCount, 256);
      expect(restarted.artworkPayloadReads, 0);
      expect(
        (await loaded.artworks.last.resolve()).document.frames,
        hasLength(256),
      );
      expect(restarted.artworkPayloadReads, 1);
      expect(store.storageLimitBytes, 1000000000);
    });

    test(
      'catalog over 64 entries stays lazy and edits only one payload',
      () async {
        final store = await open();
        final arts = [
          for (var i = 0; i < 100; i++) animatedArtwork(id: 'art-$i'),
        ];
        await store.save(LitchcraftLibrarySnapshot(artworks: arts));
        expect(store.compactArtwork(arts.first).isDeferred, isTrue);
        final restarted = await open();
        final loaded = await restarted.load();
        expect(loaded.artworks, hasLength(100));
        expect(loaded.artworks.every((art) => art.isDeferred), isTrue);
        expect(restarted.artworkPayloadReads, 0);
        expect(() => loaded.artworks.first.document, throwsStateError);
        expect(() => loaded.artworks.first.toJson(), throwsStateError);
        final selected = await loaded.artworks[42].resolve();
        expect(selected.isDeferred, isFalse);
        expect(restarted.artworkPayloadReads, 1);
        expect(loaded.artworks[42].isDeferred, isTrue);
        final edited = selected.copyWith(
          canvas: selected.canvas.paint([0], 1, 0xffaaaa),
        );
        final entries = [...loaded.artworks]..[42] = edited;
        await restarted.save(LitchcraftLibrarySnapshot(artworks: entries));
        expect(restarted.artworkPayloadReads, 1);
        expect(restarted.artworkPayloadWrites, 1);
        expect(restarted.compactArtwork(edited).isDeferred, isTrue);
        final renamed = restarted
            .compactArtwork(edited)
            .copyWith(name: 'Renamed');
        entries[42] = renamed;
        final firstBlobNames = (await blobs()).map((file) => file.path).toSet();
        await restarted.save(LitchcraftLibrarySnapshot(artworks: entries));
        expect(restarted.artworkPayloadReads, 1);
        expect(restarted.artworkPayloadWrites, 1);
        final afterNames = (await blobs()).map((file) => file.path).toSet();
        expect(afterNames.difference(firstBlobNames), isEmpty);
        final resolvedRename = await (await restarted.load()).artworks[42]
            .resolve();
        expect(resolvedRename.name, 'Renamed');
        expect(resolvedRename.canvas.colors, edited.canvas.colors);
      },
    );

    test(
      'lower allowance retains works permits deletion and rejects growth',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(
            artworks: [
              maximumArtwork(id: 'one'),
              maximumArtwork(id: 'two'),
              maximumArtwork(id: 'three'),
            ],
          ),
        );
        final preserved = await store.load();
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          1000000,
        );
        await store.save(preserved);
        expect((await store.load()).artworks, hasLength(3));
        await expectLater(
          store.save(
            LitchcraftLibrarySnapshot(
              artworks: [
                ...preserved.artworks,
                artwork(id: 'growth'),
              ],
            ),
          ),
          throwsA(isA<LitchcraftStorageLimitExceeded>()),
        );
        expect((await store.load()).artworks, hasLength(3));
        final deleted = LitchcraftLibrarySnapshot(
          artworks: [preserved.artworks.first],
        );
        await store.save(deleted);
        await store.save(await store.load());
        expect(await blobs(), hasLength(1));
        await store.save(
          LitchcraftLibrarySnapshot(
            artworks: [
              ...(await store.load()).artworks,
              artwork(id: 'after-deletion'),
            ],
          ),
        );
        expect((await store.load()).artworks, hasLength(2));
      },
    );

    test(
      'limit change is read during serialized save and failure stays atomic',
      () async {
        final store = await open();
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          1000000,
        );
        await store.save(LitchcraftLibrarySnapshot(draft: maximumArtwork()));
        final oldIndex = await index('current').readAsString();
        await expectLater(
          store.save(
            LitchcraftLibrarySnapshot(
              draft: maximumArtwork(id: 'next'),
              previousDraft: maximumArtwork(id: 'previous'),
            ),
          ),
          throwsA(isA<LitchcraftStorageLimitExceeded>()),
        );
        expect(await index('current').readAsString(), oldIndex);
        expect(await blobs(), hasLength(1));
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          2000000,
        );
        await store.save(
          LitchcraftLibrarySnapshot(
            draft: maximumArtwork(id: 'next'),
            previousDraft: maximumArtwork(id: 'previous'),
          ),
        );
        expect((await store.load()).draft!.id, 'next');
      },
    );

    for (final fileBacked in [false, true]) {
      test(
        'committed allowance provider overrides pending preferences cache ($fileBacked)',
        () async {
          var committed = 1000000;
          await preferences.setInt(
            AppLitchcraftPreferences.storageLimitKey,
            committed,
          );
          final store = fileBacked
              ? await LitchcraftArtworkStore.open(
                  preferences: preferences,
                  directory: directory,
                  storageLimitProvider: () => committed,
                )
              : LitchcraftArtworkStore(
                  preferences,
                  storageLimitProvider: () => committed,
                );
          await store.save(
            LitchcraftLibrarySnapshot(draft: maximumArtwork(id: 'first')),
          );
          // Model the SharedPreferences cache during a held increase. The root
          // integration test supplies the actual delayed platform-write seam.
          await preferences.setInt(
            AppLitchcraftPreferences.storageLimitKey,
            2000000,
          );
          expect(store.storageLimitBytes, 1000000);
          final growth = LitchcraftLibrarySnapshot(
            draft: maximumArtwork(id: 'next'),
            previousDraft: maximumArtwork(id: 'previous'),
          );
          await expectLater(
            store.save(growth),
            throwsA(isA<LitchcraftStorageLimitExceeded>()),
          );
          expect((await store.load()).draft!.id, 'first');
          committed = 2000000;
          expect(store.storageLimitBytes, 2000000);
          await store.save(growth);
          expect((await store.load()).draft!.id, 'next');
        },
      );
    }

    test(
      'migrates each legacy recovery slot without deleting original preferences',
      () async {
        final sources = <String, String>{
          LitchcraftArtworkStore.currentKey: jsonEncode(
            LitchcraftLibrarySnapshot(
              artworks: [animatedArtwork(id: 'current')],
              draft: artwork(id: 'draft'),
            ).toJson(),
          ),
          LitchcraftArtworkStore.backupKey: jsonEncode(
            LitchcraftLibrarySnapshot(
              artworks: [artwork(id: 'previous')],
            ).toJson(),
          ),
          LitchcraftArtworkStore.rejectedKey: jsonEncode(
            LitchcraftLibrarySnapshot(
              artworks: [
                LitchcraftArtwork.fromJson({
                  ...animatedArtwork().toJson(),
                  'version': 5,
                }),
              ],
            ).toJson(),
          ),
        };
        for (final source in sources.entries) {
          await preferences.setString(source.key, source.value);
        }
        final store = await open();
        final loaded = await store.load();
        expect(loaded.artworks.single.id, 'current');
        expect(loaded.artworks.single.isDeferred, isTrue);
        expect(store.artworkPayloadReads, 0);
        expect(await index('backup').exists(), isTrue);
        expect(await index('rejected').exists(), isTrue);
        for (final source in sources.entries) {
          expect(preferences.getString(source.key), source.value);
        }
        final firstFiles = (await blobs()).map((file) => file.path).toSet();
        final reopened = await open();
        await reopened.save(await reopened.load());
        expect(
          (await blobs())
              .map((file) => file.path)
              .toSet()
              .difference(firstFiles),
          isEmpty,
        );
      },
    );

    test(
      'failed migration preserves every legacy slot and retries safely',
      () async {
        final keys = [
          LitchcraftArtworkStore.currentKey,
          LitchcraftArtworkStore.backupKey,
          LitchcraftArtworkStore.rejectedKey,
        ];
        final sources = <String, String>{};
        for (var i = 0; i < keys.length; i++) {
          final source = jsonEncode(
            LitchcraftLibrarySnapshot(
              artworks: [animatedArtwork(id: 'legacy-$i')],
            ).toJson(),
          );
          sources[keys[i]] = source;
          await preferences.setString(keys[i], source);
        }
        final blocked = Directory('${index('current').path}.tmp');
        await blocked.create();
        await expectLater(open(), throwsA(isA<FileSystemException>()));
        expect(
          await File('${directory.path}/migration.json').exists(),
          isFalse,
        );
        for (final source in sources.entries) {
          expect(preferences.getString(source.key), source.value);
        }
        await blocked.delete();
        final migrated = await open();
        expect((await migrated.load()).artworks.single.id, 'legacy-0');
        expect(await index('backup').exists(), isTrue);
        expect(await index('rejected').exists(), isTrue);
        for (final source in sources.entries) {
          expect(preferences.getString(source.key), source.value);
        }
      },
    );

    test(
      'corrupt migrated current recovers backup and retains rejected bytes',
      () async {
        await preferences.setString(
          LitchcraftArtworkStore.currentKey,
          '{interrupted',
        );
        await preferences.setString(
          LitchcraftArtworkStore.backupKey,
          jsonEncode(
            LitchcraftLibrarySnapshot(
              artworks: [artwork(name: 'Backup')],
            ).toJson(),
          ),
        );
        final store = await open();
        final loaded = await store.load();
        expect(loaded.recovered, isTrue);
        expect(loaded.rejected, isTrue);
        expect(loaded.artworks.single.name, 'Backup');
        await store.save(loaded);
        final files = await directory
            .list()
            .where(
              (file) => file is File && file.path.contains('legacy-rejected-'),
            )
            .toList();
        expect(await (files.single as File).readAsString(), '{interrupted');
        expect(
          preferences.getString(LitchcraftArtworkStore.currentKey),
          '{interrupted',
        );
      },
    );

    test('corrupt current index never overwrites validated backup', () async {
      final store = await open();
      await store.save(
        LitchcraftLibrarySnapshot(artworks: [artwork(name: 'First')]),
      );
      await store.save(
        LitchcraftLibrarySnapshot(artworks: [artwork(name: 'Second')]),
      );
      final backup = await index('backup').readAsString();
      await index('current').writeAsString('{interrupted');
      final restarted = await open();
      final loaded = await restarted.load();
      expect(loaded.recovered, isTrue);
      expect(loaded.rejected, isTrue);
      expect(loaded.artworks.single.name, 'First');
      await restarted.save(loaded);
      expect(await index('backup').readAsString(), backup);
      final preserved = await directory
          .list()
          .where((file) => file.path.contains('rejected-source-'))
          .toList();
      expect(await (preserved.single as File).readAsString(), '{interrupted');
    });

    test(
      'invalid UTF-8 catalog recovers backup and preserves exact source bytes',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'First')),
        );
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Second')),
        );
        final rejected = [0xff, 0x00, 0x80, 0xc0];
        await index('current').writeAsBytes(rejected, flush: true);
        final restarted = await open();
        final loaded = await restarted.load();
        expect(loaded.recovered, isTrue);
        expect(loaded.rejected, isTrue);
        expect(loaded.draft!.name, 'First');
        await restarted.save(loaded);
        final files = await directory
            .list()
            .where((file) => file.path.contains('rejected-source-'))
            .toList();
        expect(await (files.single as File).readAsBytes(), rejected);
      },
    );

    test(
      'damaged chosen blob remains referenced and unrelated edits stay lazy',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(
            artworks: [
              artwork(id: 'bad'),
              artwork(id: 'good'),
            ],
          ),
        );
        final catalog = jsonDecode(await index('current').readAsString());
        final badPath =
            '${directory.path}/artworks/${catalog['artworks'][0]['blob']}.json';
        final bad = File(badPath);
        final original = await bad.readAsBytes();
        final corrupted = [...original]..[20] ^= 1;
        await bad.writeAsBytes(corrupted, flush: true);
        final restarted = await open();
        final loaded = await restarted.load();
        expect(restarted.artworkPayloadReads, 0);
        await expectLater(
          loaded.artworks.first.resolve(),
          throwsFormatException,
        );
        expect(loaded.artworks.first.isDeferred, isTrue);
        final renamed = loaded.artworks.last.copyWith(name: 'Still works');
        await restarted.save(
          LitchcraftLibrarySnapshot(artworks: [loaded.artworks.first, renamed]),
        );
        expect(await bad.readAsBytes(), corrupted);
        expect((await restarted.load()).artworks, hasLength(2));
        expect((await renamed.resolve()).name, 'Still works');
        await expectLater(
          restarted.save(
            LitchcraftLibrarySnapshot(artworks: [artwork(id: 'bad')]),
          ),
          throwsFormatException,
        );
        expect(await bad.readAsBytes(), corrupted);
      },
    );

    test(
      'inventory failed C cleans the displaced A backup before restart',
      () async {
        final store = await open();
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'A')));
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'B')));
        expect(await blobs(), hasLength(2));
        final blocked = Directory('${index('current').path}.tmp');
        await blocked.create();
        await expectLater(
          store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'C'))),
          throwsA(isA<FileSystemException>()),
        );
        expect(
          await blobs(),
          hasLength(1),
          reason:
              'Backup publication displaced A even though current C failed.',
        );
        await blocked.delete();
        final restarted = await open();
        final loaded = await restarted.load();
        expect(loaded.draft!.name, 'B');
        final published = jsonDecode(await index('current').readAsString());
        final backup = jsonDecode(await index('backup').readAsString());
        expect(backup['draft']['blob'], published['draft']['blob']);
        final retained = File(
          '${directory.path}/artworks/${published['draft']['blob']}.json',
        );
        expect((await blobs()).map((file) => file.path), [retained.path]);
        expect(await retained.length(), published['draft']['bytes']);
        expect(restarted.artworkPayloadReads, 0);
        expect((await loaded.draft!.resolve()).name, 'B');
        await restarted.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'D')),
        );
        expect(await blobs(), hasLength(2));
      },
    );

    test(
      'inventory restart removes process death payload and temporary orphans',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Published')),
        );
        final payload = utf8.encode(jsonEncode(artwork(id: 'orphan').toJson()));
        final digest = sha256.convert(payload).toString();
        final orphan = File('${directory.path}/artworks/$digest.json');
        await orphan.writeAsBytes(payload, flush: true);
        final temporary = File(
          '${directory.path}/artworks/${'a' * 64}.json.tmp',
        );
        await temporary.writeAsString('interrupted write', flush: true);
        final restarted = await open();
        expect(restarted.artworkPayloadReads, 0);
        expect((await restarted.load()).draft!.name, 'Published');
        expect(await orphan.exists(), isFalse);
        expect(await temporary.exists(), isFalse);
        expect(await blobs(), hasLength(1));
      },
    );

    test(
      'inventory failed cleanup remains counted and retries when writable',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Published')),
        );
        for (final id in ['orphan-one', 'orphan-two']) {
          final bytes = utf8.encode(
            jsonEncode(maximumArtwork(id: id).toJson()),
          );
          final digest = sha256.convert(bytes).toString();
          await File(
            '${directory.path}/artworks/$digest.json',
          ).writeAsBytes(bytes, flush: true);
        }
        final artworkDirectory = '${directory.path}/artworks';
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          1000000,
        );
        final denied = await Process.run('chmod', ['555', artworkDirectory]);
        expect(denied.exitCode, 0);
        late LitchcraftArtworkStore restarted;
        try {
          restarted = await open();
          expect(await blobs(), hasLength(3));
          await expectLater(
            restarted.save(
              LitchcraftLibrarySnapshot(
                artworks: [artwork(id: 'growth')],
                draft: (await restarted.load()).draft,
              ),
            ),
            throwsA(isA<LitchcraftStorageLimitExceeded>()),
          );
          expect(restarted.artworkPayloadReads, 0);
          expect(restarted.artworkPayloadWrites, 0);
        } finally {
          await Process.run('chmod', ['755', artworkDirectory]);
        }
        await restarted.save(
          LitchcraftLibrarySnapshot(
            artworks: [artwork(id: 'growth')],
            draft: (await restarted.load()).draft,
          ),
        );
        expect(await blobs(), hasLength(2));
      },
      skip: Platform.isWindows
          ? 'Requires POSIX filesystem permissions.'
          : false,
    );

    test(
      'inventory pending cleanup rejects equal-size successive replacements',
      () async {
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          1600000,
        );
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: maximumArtwork(id: 'A')),
        );
        final first = jsonDecode(await index('current').readAsString());
        final fileA =
            '${directory.path}/artworks/${first['draft']['blob']}.json';
        await store.save(
          LitchcraftLibrarySnapshot(draft: maximumArtwork(id: 'B')),
        );
        final second = jsonDecode(await index('current').readAsString());
        final fileB =
            '${directory.path}/artworks/${second['draft']['blob']}.json';
        // Deny only old-backup cleanup so new payload writes still exercise
        // the real postpublication storage boundary on every host platform.
        final deletionDenial = _SelectiveDeleteDenial({fileA, fileB});
        late LitchcraftArtworkStore restarted;
        await IOOverrides.runWithIOOverrides(() async {
          await store.save(
            LitchcraftLibrarySnapshot(draft: maximumArtwork(id: 'C')),
          );
          expect(await blobs(), hasLength(3));
          restarted = await open();
          final growth = LitchcraftLibrarySnapshot(
            draft: maximumArtwork(id: 'D'),
          );
          await expectLater(
            restarted.save(growth),
            throwsA(isA<LitchcraftStorageLimitExceeded>()),
          );
          expect((await restarted.load()).draft!.id, 'C');
          expect(await blobs(), hasLength(3));
          expect(restarted.artworkPayloadWrites, 0);
          final renamed = (await restarted.load()).draft!.copyWith(
            name: 'Renamed C',
          );
          await restarted.save(LitchcraftLibrarySnapshot(draft: renamed));
          await expectLater(
            restarted.save(growth),
            throwsA(isA<LitchcraftStorageLimitExceeded>()),
          );
          await restarted.save(LitchcraftLibrarySnapshot());
          await restarted.save(await restarted.load());
          expect(await blobs(), hasLength(2));
        }, deletionDenial);
        expect(deletionDenial.attemptedDeletes, containsAll([fileA, fileB]));
        await restarted.save(
          LitchcraftLibrarySnapshot(draft: maximumArtwork(id: 'D')),
        );
        expect(await blobs(), hasLength(1));
        expect((await restarted.load()).draft!.id, 'D');
      },
    );

    test(
      'inventory historical rejected catalog pins incident payloads only',
      () async {
        final store = await open();
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'A')));
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'B')));
        final incidentFiles = (await blobs()).map((file) => file.path).toSet();
        await index('current').writeAsString('{damaged');
        final recovered = await open();
        await recovered.save(await recovered.load());
        await recovered.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Later C')),
        );
        await recovered.save(await recovered.load());
        final current = jsonDecode(await index('current').readAsString());
        final later = File(
          '${directory.path}/artworks/${current['draft']['blob']}.json',
        );
        final restarted = await open();
        await restarted.save(LitchcraftLibrarySnapshot());
        await restarted.save(await restarted.load());
        expect(
          await later.exists(),
          isFalse,
          reason: 'Old rejected bytes cannot acquire ownership of later C.',
        );
        expect((await blobs()).map((file) => file.path).toSet(), incidentFiles);
        final preserved = await directory
            .list()
            .where((file) => file.path.contains('rejected-source-'))
            .toList();
        expect(await (preserved.single as File).readAsString(), '{damaged');
        expect(restarted.artworkPayloadReads, 0);
      },
    );

    test(
      'inventory legacy rejected bytes do not pin later file artworks',
      () async {
        await preferences.setString(
          LitchcraftArtworkStore.rejectedKey,
          '{legacy-invalid',
        );
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Later')),
        );
        await store.save(await store.load());
        final restarted = await open();
        await restarted.save(LitchcraftLibrarySnapshot());
        await restarted.save(await restarted.load());
        expect(await blobs(), isEmpty);
        expect(
          preferences.getString(LitchcraftArtworkStore.rejectedKey),
          '{legacy-invalid',
        );
        final preserved = await directory
            .list()
            .where((file) => file.path.contains('legacy-rejected-'))
            .toList();
        expect(
          await (preserved.single as File).readAsString(),
          '{legacy-invalid',
        );
      },
    );

    test(
      'inventory malformed ownership preserves possible recovery payloads',
      () async {
        final store = await open();
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'A')));
        final bytes = utf8.encode(
          jsonEncode(artwork(id: 'possible-recovery').toJson()),
        );
        final digest = sha256.convert(bytes).toString();
        final possible = File('${directory.path}/artworks/$digest.json');
        await possible.writeAsBytes(bytes, flush: true);
        final before = (await blobs()).map((file) => file.path).toSet();
        await File(
          '${directory.path}/recovery-payloads.json',
        ).writeAsString('{broken ownership');
        final restarted = await open();
        await restarted.save(LitchcraftLibrarySnapshot());
        await restarted.save(await restarted.load());
        expect((await blobs()).map((file) => file.path).toSet(), before);
        expect(await possible.readAsBytes(), bytes);
        expect(restarted.artworkPayloadReads, 0);
      },
    );

    test(
      'inventory ownership publication failure blocks damaged index replacement',
      () async {
        final store = await open();
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'A')));
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'B')));
        await index('current').writeAsString('{damaged');
        final backup = await index('backup').readAsString();
        final before = (await blobs()).map((file) => file.path).toSet();
        final blocked = Directory(
          '${directory.path}/recovery-payloads.json.tmp',
        );
        await blocked.create();
        final restarted = await open();
        expect((await restarted.load()).draft!.name, 'A');
        await expectLater(
          restarted.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'C'))),
          throwsA(isA<FileSystemException>()),
        );
        expect(await index('current').readAsString(), '{damaged');
        expect(await index('backup').readAsString(), backup);
        expect((await blobs()).map((file) => file.path).toSet(), before);
        expect(restarted.artworkPayloadWrites, 0);
        await blocked.delete();
        await restarted.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'C')),
        );
        expect((await restarted.load()).draft!.name, 'C');
        final manifest = jsonDecode(
          await File('${directory.path}/recovery-payloads.json').readAsString(),
        );
        expect((manifest['payloads'] as Map).length, 2);
      },
    );

    test(
      'inventory overwritten invalid backup is preserved and counted once',
      () async {
        final store = await open();
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'A')));
        await store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'B')));
        final invalid = '{"garbage":"${'x' * 600000}"}';
        await index('backup').writeAsString(invalid);
        final restarted = await open();
        await preferences.setInt(
          AppLitchcraftPreferences.storageLimitKey,
          1000000,
        );
        await restarted.save(await restarted.load());
        await restarted.save(
          LitchcraftLibrarySnapshot(
            artworks: [artwork(id: 'growth')],
            draft: (await restarted.load()).draft,
          ),
        );
        expect((await restarted.load()).artworks.single.id, 'growth');
        final preserved = await directory
            .list()
            .where((file) => file.path.contains('rejected-source-'))
            .toList();
        expect(await (preserved.single as File).readAsString(), invalid);
      },
    );

    for (final damage in ['missing', 'truncated']) {
      test(
        'inventory $damage chosen payload leaves intact library entries visible',
        () async {
          final store = await open();
          await store.save(
            LitchcraftLibrarySnapshot(
              artworks: [
                artwork(id: 'bad'),
                artwork(id: 'good'),
              ],
            ),
          );
          final catalog = jsonDecode(await index('current').readAsString());
          final bad = File(
            '${directory.path}/artworks/${catalog['artworks'][0]['blob']}.json',
          );
          if (damage == 'missing') {
            await bad.delete();
          } else {
            await bad.writeAsString('{short');
          }
          final restarted = await open();
          final loaded = await restarted.load();
          expect(loaded.artworks.map((art) => art.id), ['bad', 'good']);
          expect(restarted.artworkPayloadReads, 0);
          await expectLater(
            loaded.artworks.first.resolve(),
            throwsA(
              damage == 'missing'
                  ? isA<FileSystemException>()
                  : isA<FormatException>(),
            ),
          );
          expect((await loaded.artworks.last.resolve()).id, 'good');
          await restarted.save(
            LitchcraftLibrarySnapshot(
              artworks: [
                loaded.artworks.first,
                loaded.artworks.last.copyWith(name: 'Still here'),
              ],
            ),
          );
          expect((await restarted.load()).artworks.map((art) => art.id), [
            'bad',
            'good',
          ]);
          expect(await bad.exists(), damage == 'truncated');
        },
      );
    }

    test(
      'interrupted publication leaves current usable and queued retry succeeds',
      () async {
        final store = await open();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'First')),
        );
        final previous = await index('current').readAsString();
        final blocked = Directory('${index('current').path}.tmp');
        await blocked.create();
        await expectLater(
          store.save(LitchcraftLibrarySnapshot(draft: artwork(name: 'Second'))),
          throwsA(isA<FileSystemException>()),
        );
        expect(await index('current').readAsString(), previous);
        expect(
          await blobs(),
          hasLength(1),
          reason: 'Failed publications do not accumulate payload files.',
        );
        expect((await (await open()).load()).draft!.name, 'First');
        await blocked.delete();
        await store.save(
          LitchcraftLibrarySnapshot(draft: artwork(name: 'Third')),
        );
        expect((await (await open()).load()).draft!.name, 'Third');
      },
    );

    test('unsupported source stays exact when deferred and migrated', () async {
      final source = {
        ...animatedArtwork().toJson(),
        'version': 5,
        'future': 'retained',
      };
      final store = await open();
      await store.save(
        LitchcraftLibrarySnapshot(
          artworks: [LitchcraftArtwork.fromJson(source)],
        ),
      );
      final restarted = await open();
      final entry = (await restarted.load()).artworks.single;
      expect(entry.isSupported, isFalse);
      expect(entry.isDeferred, isTrue);
      expect(entry.frameCount, 0);
      expect((await entry.resolve()).toJson(), source);
      final renamed = entry.copyWith(
        name: 'Future',
        modifiedAt: DateTime.utc(2026, 9, 10),
      );
      await restarted.save(LitchcraftLibrarySnapshot(artworks: [renamed]));
      expect(restarted.artworkPayloadWrites, 0);
      final resolved = await (await (await open()).load()).artworks.single
          .resolve();
      expect(resolved.toJson(), {
        ...source,
        'name': 'Future',
        'modifiedAt': DateTime.utc(2026, 9, 10).toIso8601String(),
      });
    });
  });
}

LitchcraftArtwork maximumArtwork({String id = 'maximum', int frames = 256}) =>
    LitchcraftArtwork.animated(
      id: id,
      name: 'Maximum',
      document: LitchcraftAnimationDocument(
        frames: [
          for (var frame = 0; frame < frames; frame++)
            LitchcraftAnimationFrame(
              emitters: [
                for (var emitter = 0; emitter < 256; emitter++)
                  LitchcraftAnimatedEmitter(color: (frame << 16) | emitter),
              ],
            ),
        ],
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
      ),
      createdAt: DateTime.utc(2026, 9, 9),
      modifiedAt: DateTime.utc(2026, 9, 9, 1),
    );

final class _SelectiveDeleteDenial extends IOOverrides {
  _SelectiveDeleteDenial(this.deniedPaths);

  final Set<String> deniedPaths;
  final Set<String> attemptedDeletes = <String>{};

  @override
  File createFile(String path) {
    final file = super.createFile(path);
    return deniedPaths.contains(path)
        ? _DeleteDeniedFile(file, attemptedDeletes)
        : file;
  }
}

class _DeleteDeniedFile implements File {
  _DeleteDeniedFile(this.delegate, this.attemptedDeletes);

  final File delegate;
  final Set<String> attemptedDeletes;

  @override
  String get path => delegate.path;

  @override
  Future<bool> exists() => delegate.exists();

  @override
  Future<File> delete({bool recursive = false}) async {
    attemptedDeletes.add(path);
    throw FileSystemException(
      'Injected artwork cleanup deletion denial.',
      path,
    );
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}
