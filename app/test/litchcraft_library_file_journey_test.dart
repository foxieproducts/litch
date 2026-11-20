import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/app_litchcraft_preferences.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

LitchcraftArtwork _art(String id, {int frames = 1}) =>
    LitchcraftArtwork.animated(
      id: id,
      name: id,
      createdAt: DateTime.utc(2026, 10, 1),
      modifiedAt: DateTime.utc(2026, 10, 1),
      document: LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        playbackOnEntry: frames > 1,
        sparkleSeed: 42,
        frames: [
          for (var frame = 0; frame < frames; frame++)
            LitchcraftAnimationFrame(
              durationMs: frame == 1 ? 2000 : 100,
              emitters: List.generate(
                256,
                (index) => LitchcraftAnimatedEmitter(
                  color: (0x123456 + frame * 7919 + index * 53) & 0xffffff,
                ),
              ),
            ),
        ],
      ),
    );

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late Directory directory;
  late SharedPreferences preferences;
  late LitchBleService ble;
  late LitchcraftController controller;
  LitchcraftLibrary? library;

  setUp(() async {
    SharedPreferences.setMockInitialValues({});
    preferences = await SharedPreferences.getInstance();
    directory = await Directory.systemTemp.createTemp('litch-library-journey-');
    ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    controller = LitchcraftController(ble);
  });
  tearDown(() async {
    library?.dispose();
    library = null;
    controller.dispose();
    await ble.dispose();
    await directory.delete(recursive: true);
  });

  Future<LitchcraftArtworkStore> openStore() => LitchcraftArtworkStore.open(
    preferences: preferences,
    directory: directory,
  );

  test(
    'sixth full artwork saves through library and reopens without flattening',
    () async {
      final initialStore = await openStore();
      final draft = _art('draft', frames: 256);
      await initialStore.save(
        LitchcraftLibrarySnapshot(
          artworks: [
            for (var i = 0; i < 3; i++) _art('large-$i', frames: 256),
            _art('small-1'),
            _art('small-2'),
          ],
          draft: draft,
          previousDraft: _art('previous', frames: 255),
        ),
      );
      final store = await openStore();
      library = LitchcraftLibrary(controller, storeLoader: () async => store);
      await library!.ready;
      expect(library!.animationDocument.frames, hasLength(256));
      expect(library!.artworks, hasLength(5));
      expect(
        store.artworkPayloadReads,
        1,
        reason: 'only the active draft is loaded',
      );
      await library!.save('Recovered full board copy', saveAs: true);
      expect(library!.saveFailed, isFalse);
      expect(library!.dirty, isFalse);
      expect(library!.artworks, hasLength(6));
      expect(library!.artworks.every((art) => art.isDeferred), isTrue);
      expect(library!.previousDraft!.isDeferred, isTrue);
      final restarted = await openStore();
      final restored = await restarted.load();
      expect(restarted.artworkPayloadReads, 0);
      expect(restored.artworks, hasLength(6));
      expect(restored.previousDraft!.frameCount, 255);
      final saved = await restored.artworks.first.resolve();
      expect(saved.name, 'Recovered full board copy');
      expect(sameLitchcraftDocument(saved.document, draft.document), isTrue);
      expect(saved.document.frames[1].durationMs, 2000);
    },
  );

  test(
    'over 64 saved entries browse lazily then open rename duplicate and delete',
    () async {
      final initialStore = await openStore();
      await initialStore.save(
        LitchcraftLibrarySnapshot(
          artworks: [for (var i = 0; i < 100; i++) _art('art-$i', frames: 2)],
        ),
      );
      final store = await openStore();
      library = LitchcraftLibrary(controller, store: store);
      await library!.ready;
      expect(library!.artworks, hasLength(100));
      expect(store.artworkPayloadReads, 0);
      await library!.open(library!.artworks[72]);
      expect(
        store.artworkPayloadReads,
        2,
        reason:
            'the selected payload is decoded once and its identical current-draft blob is checksum-verified before reuse',
      );
      expect(library!.dirty, isFalse);
      await library!.rename(library!.artworks[72], 'Renamed selected artwork');
      expect(library!.activeName, 'Renamed selected artwork');
      expect(library!.dirty, isFalse);
      expect(library!.artworks.every((art) => art.isDeferred), isTrue);
      await library!.duplicate(library!.artworks[72]);
      expect(library!.artworks, hasLength(101));
      expect(
        store.artworkPayloadReads,
        3,
        reason:
            'only the chosen artwork and unchanged active payload checksums are read; '
            'metadata operations do not decode other saved artwork',
      );
      final copy = library!.artworks.first;
      expect(copy.isDeferred, isTrue);
      await library!.delete(copy);
      expect(library!.artworks, hasLength(100));
      expect(library!.dirty, isFalse);
      final restored = await (await openStore()).load();
      expect(restored.artworks, hasLength(100));
      expect(restored.artworks[72].name, 'Renamed selected artwork');
      expect(
        (await restored.artworks[72].resolve()).document.frames,
        hasLength(2),
      );
    },
  );

  test(
    'lowered budget rejects growing Save without losing library or draft',
    () async {
      final initialStore = await openStore();
      final draft = _art('draft', frames: 256);
      await initialStore.save(
        LitchcraftLibrarySnapshot(
          artworks: [_art('saved', frames: 256)],
          draft: draft,
        ),
      );
      final store = await openStore();
      library = LitchcraftLibrary(controller, store: store);
      await library!.ready;
      await preferences.setInt(
        AppLitchcraftPreferences.storageLimitKey,
        1000000,
      );
      await expectLater(
        library!.save('Copy', saveAs: true),
        throwsA(isA<LitchcraftStorageLimitExceeded>()),
      );
      expect(library!.storageLimitExceeded, isTrue);
      expect(library!.artworks, hasLength(1));
      expect(
        sameLitchcraftDocument(library!.animationDocument, draft.document),
        isTrue,
      );
      final restored = await (await openStore()).load();
      expect(restored.artworks, hasLength(1));
      expect(restored.draft!.frameCount, 256);
      await preferences.setInt(
        AppLitchcraftPreferences.storageLimitKey,
        1000000000,
      );
      await library!.save('Copy', saveAs: true);
      expect(library!.artworks, hasLength(2));
      expect(library!.saveFailed, isFalse);
      expect(library!.storageLimitExceeded, isFalse);
    },
  );

  test('an edit during deferred open wins over a late payload read', () async {
    final store = await openStore();
    library = LitchcraftLibrary(controller, store: store);
    await library!.ready;
    final source = _art('slow', frames: 2);
    final entered = Completer<void>();
    final read = Completer<LitchcraftArtwork>();
    final deferred = LitchcraftArtwork.deferred(
      id: source.id,
      name: source.name,
      createdAt: source.createdAt,
      modifiedAt: source.modifiedAt,
      frameCount: source.frameCount,
      preview: source.canvas,
      favoriteColors: source.favoriteColors,
      storageReference: Object(),
      resolver: () {
        entered.complete();
        return read.future;
      },
    );
    final pending = library!.open(deferred);
    final refused = expectLater(pending, throwsStateError);
    await entered.future;
    library!.session.paintSelectedFrame(squares: [0], mask: 1, color: 0xff7430);
    final edited = library!.animationDocument;
    read.complete(source);
    await refused;
    expect(identical(library!.animationDocument, edited), isTrue);
    expect(library!.previousDraft, isNull);
    expect(library!.activeName, isNull);
  });
  test('restart restores a draft with a nondefault recent palette', () async {
    final palette = [
      0x110001,
      0x220002,
      0x330003,
      0x440004,
      0x550005,
      0x660006,
      0x770007,
      0x880008,
    ];
    final draft = _art('custom', frames: 2).copyWith(favoriteColors: palette);
    final initialStore = await openStore();
    await initialStore.save(
      LitchcraftLibrarySnapshot(
        artworks: [draft],
        draft: draft,
        recentColors: palette,
      ),
    );
    library = LitchcraftLibrary(controller, store: await openStore());
    await library!.ready;
    expect(
      sameLitchcraftDocument(library!.animationDocument, draft.document),
      isTrue,
    );
    expect(library!.favoriteColors, palette);
    expect(library!.activeName, 'custom');
    expect(library!.dirty, isFalse);
  });

  for (final missingBaseline in [true, false]) {
    test(
      'healthy edited draft survives unreadable saved baseline and lifecycle flush (missing=$missingBaseline)',
      () async {
        final saved = _art('saved', frames: 2);
        final draft = saved.copyWith(
          document: _art('edited', frames: 3).document,
        );
        final initialStore = await openStore();
        await initialStore.save(
          LitchcraftLibrarySnapshot(
            artworks: [saved],
            draft: draft,
            draftOrigin: LitchcraftDraftOrigin.saved,
          ),
        );
        final catalog = jsonDecode(
          await File('${directory.path}/current.json').readAsString(),
        );
        final baselineFile = File(
          '${directory.path}/artworks/${catalog['artworks'][0]['blob']}.json',
        );
        final draftFile = File(
          '${directory.path}/artworks/${catalog['draft']['blob']}.json',
        );
        final draftBytes = await draftFile.readAsBytes();
        if (missingBaseline) {
          await baselineFile.delete();
        } else {
          final bytes = await baselineFile.readAsBytes();
          bytes[0] ^= 1;
          await baselineFile.writeAsBytes(bytes, flush: true);
        }

        library = LitchcraftLibrary(controller, store: await openStore());
        await library!.ready;
        // Backgrounding uses this production writer even after hydration fails.
        await library!.flush();
        library!.dispose();
        library = null;
        controller.dispose();
        controller = LitchcraftController(ble);

        final restartedStore = await openStore();
        library = LitchcraftLibrary(controller, store: restartedStore);
        await library!.ready;
        await library!.flush();
        final restored = await restartedStore.load();
        expect(
          sameLitchcraftDocument(
            (await restored.draft!.resolve()).document,
            draft.document,
          ),
          isTrue,
          reason:
              'Lifecycle saves must never publish an unhydrated blank draft.',
        );
        expect(
          sameLitchcraftDocument(library!.animationDocument, draft.document),
          isTrue,
        );
        expect(library!.dirty, isTrue);
        expect(library!.rejected, isTrue);
        expect(await draftFile.readAsBytes(), draftBytes);
        await library!.save('Recovered artwork');
        expect(library!.dirty, isFalse);
        expect(library!.artworks, hasLength(2));
        expect(library!.artworks.first.id, isNot(saved.id));
        expect(library!.artworks.last.id, saved.id);
      },
    );
  }

  for (final editAfterReadFailure in [false, true]) {
    test(
      'unreadable current draft survives lifecycle flush until next launch (new frame=$editAfterReadFailure)',
      () async {
        final saved = _art('saved', frames: 2);
        final draft = saved.copyWith(
          document: _art('edited', frames: 3).document,
        );
        final initialStore = await openStore();
        await initialStore.save(
          LitchcraftLibrarySnapshot(
            artworks: [saved],
            draft: draft,
            draftOrigin: LitchcraftDraftOrigin.saved,
          ),
        );
        final catalog = jsonDecode(
          await File('${directory.path}/current.json').readAsString(),
        );
        final draftFile = File(
          '${directory.path}/artworks/${catalog['draft']['blob']}.json',
        );
        final bytes = await draftFile.readAsBytes();
        await draftFile.delete();
        library = LitchcraftLibrary(controller, store: await openStore());
        await library!.ready;
        if (editAfterReadFailure) library!.addFrame();
        final expectedDocument = editAfterReadFailure
            ? library!.animationDocument
            : draft.document;
        // The read failure is transient; the lifecycle writer must retain the
        // source it could not load rather than treating its blank view as intent.
        await draftFile.writeAsBytes(bytes, flush: true);
        await library!.flush();
        library!.dispose();
        library = null;
        controller.dispose();
        controller = LitchcraftController(ble);

        final restartedStore = await openStore();
        library = LitchcraftLibrary(controller, store: restartedStore);
        await library!.ready;
        await library!.flush();
        final restored = await restartedStore.load();
        expect(
          sameLitchcraftDocument(
            (await restored.draft!.resolve()).document,
            expectedDocument,
          ),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(library!.animationDocument, expectedDocument),
          isTrue,
        );
        expect(library!.activeName, editAfterReadFailure ? null : saved.name);
        expect(library!.dirty, isTrue);
        expect(await draftFile.readAsBytes(), bytes);
        if (editAfterReadFailure) {
          expect(
            sameLitchcraftDocument(
              (await restored.previousDraft!.resolve()).document,
              draft.document,
            ),
            isTrue,
          );
        }
        await library!.save(saved.name);
        expect(library!.dirty, isFalse);
        expect(library!.artworks, hasLength(editAfterReadFailure ? 2 : 1));
        expect(library!.artworks.last.id, saved.id);
        expect(library!.artworks.last.name, saved.name);
      },
    );
  }

  for (final edit in ['paint', 'undo', 'gesture']) {
    test(
      'failed artwork open retries Save with catalog and local $edit intent',
      () async {
        final original = _art('saved-original', frames: 2);
        final oldDraft = _art('old-draft', frames: 3);
        final initialStore = await openStore();
        await initialStore.save(
          LitchcraftLibrarySnapshot(
            artworks: [original],
            draft: oldDraft,
            previousDraft: _art('old-previous', frames: 4),
          ),
        );
        var opens = 0;
        final firstOpen = Completer<LitchcraftArtworkStore>();
        final retryOpen = Completer<LitchcraftArtworkStore>();
        library = LitchcraftLibrary(
          controller,
          storeLoader: () => ++opens == 1 ? firstOpen.future : retryOpen.future,
        );
        if (edit != 'paint') {
          firstOpen.completeError(
            const FileSystemException('Unavailable once'),
          );
          await library!.ready;
          expect(library!.saveFailed, isTrue);
        }
        if (edit != 'gesture') {
          library!.session.paintSelectedFrame(
            squares: [0],
            mask: 1,
            color: 0xff7430,
          );
          if (edit == 'undo') library!.undoAnimation();
        }
        final saved = library!.save('Recovered current');
        final pending = Future.wait([
          saved,
          for (var i = 0; i < 3; i++) library!.flush(),
        ]);
        if (edit == 'paint') {
          // Save and lifecycle writes already wait for the first open when it
          // fails. They must join one recovery before mutating the catalog.
          firstOpen.completeError(
            const FileSystemException('Unavailable once'),
          );
        }
        await Future<void>.delayed(Duration.zero);
        expect(opens, 2);
        if (edit == 'paint') {
          library!.session.paintSelectedFrame(
            squares: [63],
            mask: 2,
            color: 0x22aacc,
          );
        } else if (edit == 'gesture') {
          // The pointer owns this blank canvas even before its first edit.
          library!.beginCanvasGesture();
        }
        final edited = library!.animationDocument;
        retryOpen.complete(await openStore());
        await pending;
        if (edit == 'gesture') library!.endCanvasGesture();
        expect(opens, 2);
        expect(library!.saveFailed, isFalse);
        expect(identical(library!.animationDocument, edited), isTrue);
        final restarted = await (await openStore()).load();
        expect(restarted.artworks, hasLength(2));
        final prior = await restarted.artworks
            .singleWhere((art) => art.id == original.id)
            .resolve();
        expect(prior.toJson(), original.toJson());
        final newlySaved = await restarted.artworks
            .singleWhere((art) => art.name == 'Recovered current')
            .resolve();
        expect(sameLitchcraftDocument(newlySaved.document, edited), isTrue);
        expect(
          sameLitchcraftDocument(
            (await restarted.draft!.resolve()).document,
            edited,
          ),
          isTrue,
        );
        expect(restarted.previousDraft!.id, oldDraft.id);
        expect(
          (await restarted.previousDraft!.resolve()).toJson(),
          oldDraft.toJson(),
        );
      },
    );
  }

  for (final paletteOnly in [false, true]) {
    test(
      'failed artwork open lifecycle flush preserves prior drafts (palette=$paletteOnly)',
      () async {
        final oldDraft = _art('old-draft', frames: 3);
        final oldPrevious = _art('old-previous', frames: 4);
        final initialStore = await openStore();
        await initialStore.save(
          LitchcraftLibrarySnapshot(
            artworks: [_art('saved-original')],
            draft: oldDraft,
            previousDraft: oldPrevious,
          ),
        );
        final currentBytes = await File(
          '${directory.path}/current.json',
        ).readAsBytes();
        final backup = File('${directory.path}/backup.json');
        final backupBytes = await backup.exists()
            ? await backup.readAsBytes()
            : null;
        var opens = 0;
        final retryOpen = Completer<LitchcraftArtworkStore>();
        LitchcraftArtworkStore? recoveredStore;
        library = LitchcraftLibrary(
          controller,
          storeLoader: () async {
            if (++opens == 1) {
              throw const FileSystemException('Unavailable once');
            }
            return retryOpen.future;
          },
        );
        await library!.ready;
        expect(library!.saveFailed, isTrue);
        final expectedColors = paletteOnly
            ? [0x112233, ...litchcraftDefaultFavoriteColors].take(8).toList()
            : oldDraft.favoriteColors;
        final writes = [library!.flush(), library!.flush()];
        await Future<void>.delayed(Duration.zero);
        expect(opens, 2);
        final colorWrite = paletteOnly
            ? library!.rememberColor(0x112233)
            : Future<void>.value();
        recoveredStore = await openStore();
        retryOpen.complete(recoveredStore);
        await Future.wait([...writes, colorWrite]);
        expect(opens, 2);
        expect(library!.saveFailed, isFalse);
        expect(
          sameLitchcraftDocument(
            library!.animationDocument,
            oldDraft.copyWith(favoriteColors: expectedColors).document,
          ),
          isTrue,
        );
        if (!paletteOnly) {
          expect(recoveredStore.artworkPayloadWrites, 0);
          expect(
            await File('${directory.path}/current.json').readAsBytes(),
            currentBytes,
          );
          expect(await backup.exists(), backupBytes != null);
          if (backupBytes != null) {
            expect(await backup.readAsBytes(), backupBytes);
          }
        }
        final restarted = await (await openStore()).load();
        expect(restarted.artworks, hasLength(1));
        final restoredDraft = await restarted.draft!.resolve();
        expect(restoredDraft.id, oldDraft.id);
        expect(
          sameLitchcraftDocument(
            restoredDraft.document,
            oldDraft.copyWith(favoriteColors: expectedColors).document,
          ),
          isTrue,
        );
        expect(
          (await restarted.previousDraft!.resolve()).toJson(),
          oldPrevious.toJson(),
        );
      },
    );
  }

  test(
    'palette and paint chosen during library startup survive late loading',
    () async {
      final initialStore = await openStore();
      await initialStore.save(
        LitchcraftLibrarySnapshot(
          draft: _art('old', frames: 2),
          recentColors: [
            0x110001,
            0x220002,
            0x330003,
            0x440004,
            0x550005,
            0x660006,
            0x770007,
            0x880008,
          ],
        ),
      );
      final loading = Completer<LitchcraftArtworkStore>();
      library = LitchcraftLibrary(
        controller,
        storeLoader: () => loading.future,
      );
      library!.session.paintSelectedFrame(
        squares: [0],
        mask: 1,
        color: 0xff7430,
      );
      final palette = [1, 2, 3, 4, 5, 6, 7, 8];
      library!.favoriteColors = palette;
      final edited = library!.animationDocument;
      loading.complete(await openStore());
      await library!.ready;
      expect(identical(library!.animationDocument, edited), isTrue);
      expect(library!.favoriteColors, palette);
    },
  );

  test(
    'startup paint stays current when an unreadable old draft arrives',
    () async {
      final badSource = <String, Object?>{
        ..._art('unreadable', frames: 2).toJson(),
        'crc32': 0,
      };
      final initialStore = await openStore();
      await initialStore.save(
        LitchcraftLibrarySnapshot(draft: LitchcraftArtwork.fromJson(badSource)),
      );
      final loading = Completer<LitchcraftArtworkStore>();
      library = LitchcraftLibrary(
        controller,
        storeLoader: () => loading.future,
      );
      library!.session.paintSelectedFrame(
        squares: [0],
        mask: 1,
        color: 0xff7430,
      );
      final edited = library!.animationDocument;
      loading.complete(await openStore());
      await library!.ready;
      await library!.flush();
      final restarted = await (await openStore()).load();
      expect(restarted.draft!.isSupported, isTrue);
      expect(
        sameLitchcraftDocument(
          (await restarted.draft!.resolve()).document,
          edited,
        ),
        isTrue,
      );
      expect(restarted.previousDraft!.isSupported, isFalse);
      expect((await restarted.previousDraft!.resolve()).toJson(), badSource);
    },
  );

  testWidgets(
    'saved artwork cards use deferred metadata without payload reads',
    (tester) async {
      tester.view.physicalSize =
          const Size(1024, 768) * tester.view.devicePixelRatio;
      addTearDown(tester.view.resetPhysicalSize);
      late LitchcraftArtworkStore store;
      await tester.runAsync(() async {
        final initialStore = await openStore();
        await initialStore.save(
          LitchcraftLibrarySnapshot(
            artworks: [_art('first', frames: 256), _art('second', frames: 2)],
          ),
        );
        store = await openStore();
        library = LitchcraftLibrary(controller, store: store);
        await library!.ready;
      });
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library!),
          ),
        ),
      );
      expect(library!.artworks, hasLength(2));
      await tester.runAsync(() async {
        await tester.tap(find.byKey(const ValueKey('craft-library')));
      });
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-library-search')),
        findsOneWidget,
      );
      expect(find.byKey(const ValueKey('craft-artwork-first')), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-artwork-second')),
        findsOneWidget,
      );
      expect(store.artworkPayloadReads, 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );
}
