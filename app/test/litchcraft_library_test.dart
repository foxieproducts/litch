import 'support/memory_ota_target_store.dart';
import 'dart:convert';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

LitchcraftArtwork _animatedArtwork() => LitchcraftArtwork.animated(
  id: 'motion',
  name: 'Motion',
  document: LitchcraftAnimationDocument(
    frames: <LitchcraftAnimationFrame>[
      LitchcraftAnimationFrame(
        emitters: List<LitchcraftAnimatedEmitter>.filled(
          256,
          const LitchcraftAnimatedEmitter(color: 0x123456),
        ),
      ),
      LitchcraftAnimationFrame(
        durationMs: 2300,
        emitters: List<LitchcraftAnimatedEmitter>.generate(
          256,
          (index) => LitchcraftAnimatedEmitter(
            color: 0x654321,
            effect: index == 9
                ? LitchcraftEffectStyle.rainbow
                : LitchcraftEffectStyle.off,
            effectDurationMs: index == 9 ? 2700 : 0,
          ),
        ),
      ),
    ],
    topology: LitchcraftTopology.v3,
    favoriteColors: litchcraftDefaultFavoriteColors,
    playbackOnEntry: true,
    sparkleSeed: 42,
  ),
  createdAt: DateTime.utc(2026, 9, 12),
  modifiedAt: DateTime.utc(2026, 9, 12, 1),
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'SupportUserActionJourney: explicit artwork open and one multi-point paint gesture are reviewable actions',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final trace = AppTraceRecorder.disabled();
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
        appTrace: trace,
      );
      final controller = LitchcraftController(ble, appTrace: trace);
      final store = LitchcraftArtworkStore(
        await SharedPreferences.getInstance(),
      );
      final library = LitchcraftLibrary(
        controller,
        store: store,
        appTrace: trace,
      );
      try {
        await library.ready;
        expect(
          trace.userActions.actions,
          isEmpty,
          reason: 'Background library load is not a user action',
        );
        await library.open(_animatedArtwork());
        controller.beginStroke();
        controller.paintSquares([0, 1]);
        controller.paintSquares([1, 2]);
        controller.endStroke();
        final actions = trace.userActions.actions;
        expect(actions.map((a) => a.kind), [
          SupportActionKind.artworkLoad,
          SupportActionKind.paint,
        ]);
        expect(actions.last.count, 3);
        expect(actions.last.outcome, SupportActionOutcome.appliedOnPhone);
        await library.open(_animatedArtwork(), admissionGuard: () => false);
        expect(
          trace.userActions.actions.last.outcome,
          SupportActionOutcome.canceled,
        );
        expect(
          trace.userActions.actions.map((a) => a.description).join(),
          isNot(contains('Motion')),
        );
      } finally {
        library.dispose();
        controller.dispose();
        await ble.dispose();
        await trace.close();
      }
    },
  );
  test(
    'duplicate ordered artwork favorites survive library load and save',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final favorites = [1, 1, 0, 2, 3, 3, 5, 0xffffff];
      final source = _animatedArtwork().copyWith(favoriteColors: favorites);
      final store = LitchcraftArtworkStore(
        await SharedPreferences.getInstance(),
      );
      await store.save(
        LitchcraftLibrarySnapshot(artworks: [source], draft: source),
      );
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller, store: store);
      await library.ready;
      expect(library.saveFailed, isFalse);
      expect(library.favoriteColors, favorites);
      await library.save('Favorites kept');
      expect((await store.load()).draft!.favoriteColors, favorites);
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  test(
    'one session owns offline edits projections history and saved reopen',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await library.open(_animatedArtwork());
      library.selectFrame(1);
      library.setSelectedFrameDuration(3400);
      final generation = controller.documentSession.generation;
      library.selectFrame(0);
      library.selectFrame(1);
      expect(controller.documentSession.generation, generation);
      expect(library.canUndoAnimation, isTrue);
      library.undoAnimation();
      expect(library.selectedFrame.durationMs, 2300);
      expect(library.canUndoAnimation, isFalse);
      library.redoAnimation();
      await library.save('Shared source');
      final saved = library.artworks.single;
      await library.newArtwork();
      await library.open(saved);
      expect(library.animationDocument.frames[1].durationMs, 3400);
      expect(
        library.animationDocument.frames[1].emitters[9].effect,
        LitchcraftEffectStyle.rainbow,
      );
      expect(
        identical(
          library.animationDocument,
          controller.documentSession.document,
        ),
        isTrue,
      );
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  test('replacing a favorite with an existing favorite stays unique', () async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;

    final previous = library.favoriteColors.first;
    final existing = library.favoriteColors[3];
    await library.replaceFavoriteColor(previous, existing);

    expect(library.favoriteColors, hasLength(8));
    expect(library.favoriteColors.toSet(), hasLength(8));
    expect(library.favoriteColors.first, existing);
    expect(
      library.favoriteColors.toSet().containsAll(
        litchcraftDefaultFavoriteColors.take(7),
      ),
      isTrue,
    );

    library.dispose();
    controller.dispose();

    final restoredController = LitchcraftController(ble);
    final restoredLibrary = LitchcraftLibrary(restoredController);
    await restoredLibrary.ready;
    expect(restoredLibrary.favoriteColors, hasLength(8));
    expect(restoredLibrary.favoriteColors.toSet(), hasLength(8));
    expect(restoredLibrary.favoriteColors.first, existing);
    restoredLibrary.dispose();
    restoredController.dispose();
  });

  test('draft save save-as and duplicate never flatten animation', () async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final prefs = await SharedPreferences.getInstance();
    final store = LitchcraftArtworkStore(prefs);
    final original = _animatedArtwork();
    await store.save(
      LitchcraftLibrarySnapshot(
        artworks: <LitchcraftArtwork>[original],
        draft: original,
      ),
    );
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller, store: store);
    await library.ready;

    await library.save('Motion updated');
    await library.save('Motion copy', saveAs: true);
    await library.duplicate(library.artworks.last);
    await library.preserveDraft();

    final restored = await store.load();
    expect(restored.artworks, hasLength(3));
    for (final saved in <LitchcraftArtwork>[
      ...restored.artworks,
      restored.draft!,
      restored.previousDraft!,
    ]) {
      expect(saved.document.frames, hasLength(2));
      expect(
        saved.document.frames[1].emitters[9].effect,
        LitchcraftEffectStyle.rainbow,
      );
      expect(saved.document.frames[1].durationMs, 2300);
      expect(saved.document.sparkleSeed, 42);
      expect(saved.document.playbackOnEntry, isTrue);
    }

    library.dispose();
    controller.dispose();
  });

  test(
    'frame editing stays bounded, undoable, and saves every effect',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller, store: store);
      await library.ready;

      library.setSelectedFrameDuration(1700);
      for (final effect in LitchcraftEffectStyle.values) {
        library.applyEffect(
          square: effect.index,
          mask: 5,
          effect: effect,
          effectDurationMs: 2600,
        );
      }
      library.duplicateSelectedFrame();
      expect(library.animationDocument.frames, hasLength(2));
      expect(library.selectedFrameIndex, 1);
      library.undoAnimation();
      expect(library.animationDocument.frames, hasLength(1));
      library.redoAnimation();
      expect(library.animationDocument.frames, hasLength(2));
      library.deleteSelectedFrame();
      expect(library.animationDocument.frames, hasLength(1));
      expect(
        () => library.deleteSelectedFrame(),
        throwsA(isA<LitchcraftFrameLimit>()),
      );

      while (library.animationDocument.frames.length <
          litchcraftAnimationMaximumFrameCount) {
        library.addFrame();
      }
      expect(library.animationDocument.frames, hasLength(256));
      final fullDocument = library.animationDocument;
      final fullSelection = library.selectedFrameIndex;
      expect(() => library.addFrame(), throwsA(isA<LitchcraftFrameLimit>()));
      expect(library.animationDocument, same(fullDocument));
      expect(library.selectedFrameIndex, fullSelection);
      expect(library.canUndoAnimation, isTrue);
      expect(library.canRedoAnimation, isFalse);
      library.undoAnimation();
      expect(library.animationDocument.frames, hasLength(255));
      expect(library.selectedFrameIndex, 254);
      library.redoAnimation();
      expect(library.animationDocument, same(fullDocument));
      expect(library.selectedFrameIndex, 254);

      await library.save('Full animation');
      final saved = (await store.load()).artworks.single;
      expect(saved.document.frames, hasLength(256));
      expect(saved.document.frames.first.durationMs, 1700);
      for (final effect in LitchcraftEffectStyle.values) {
        final emitter = saved.document.frames.first.emitters[effect.index * 4];
        expect(emitter.effect, effect);
        expect(
          emitter.effectDurationMs,
          effect == LitchcraftEffectStyle.off ? 0 : 2600,
        );
      }

      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  test(
    'V3 source survives V1 and V2 draft color edits without topology adaptation',
    () async {
      for (final target in <LitchcraftTopology>[
        LitchcraftTopology.v1,
        LitchcraftTopology.v2,
      ]) {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final prefs = await SharedPreferences.getInstance();
        final store = LitchcraftArtworkStore(prefs);
        final original = _animatedArtwork();
        await store.save(
          LitchcraftLibrarySnapshot(
            artworks: <LitchcraftArtwork>[original],
            draft: original,
          ),
        );
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller, store: store);
        await library.ready;

        controller.restoreDraft(original.canvas.withTopology(target));
        controller.restoreDraft(controller.canvas.paint(<int>[3], 8, 0xabcdef));
        await library.flush();
        await library.save('Edited on ${target.name}');

        final restored = await store.load();
        final saved = restored.artworks.single;
        expect(saved.document.topology, LitchcraftTopology.v3);
        expect(saved.document.frames, hasLength(2));
        expect(saved.document.frames.first.emitters[15].color, 0xabcdef);
        expect(
          saved.document.frames[1].emitters[9].effect,
          LitchcraftEffectStyle.rainbow,
        );
        expect(restored.draft!.document.topology, LitchcraftTopology.v3);

        library.dispose();
        controller.dispose();
        await ble.dispose();
      }
    },
  );

  test(
    'unavailable current draft survives ordinary writes and recovery until replaced',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final prefs = await SharedPreferences.getInstance();
      final store = LitchcraftArtworkStore(prefs);
      final corruptSource = <String, Object?>{
        ..._animatedArtwork().toJson(),
        'id': 'corrupt-draft',
        'crc32': 0,
      };
      final unavailable = LitchcraftArtwork.fromJson(corruptSource);
      await store.save(
        LitchcraftLibrarySnapshot(
          draft: unavailable,
          previousDraft: _animatedArtwork(),
        ),
      );
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller, store: store);
      await library.ready;

      await library.dismissWelcome();
      await library.rememberColor(0x010101);
      await library.flush();
      var raw =
          jsonDecode(prefs.getString(LitchcraftArtworkStore.currentKey)!)
              as Map<String, dynamic>;
      expect(raw['draft'], corruptSource);
      raw =
          jsonDecode(prefs.getString(LitchcraftArtworkStore.backupKey)!)
              as Map<String, dynamic>;
      expect(raw['draft'], corruptSource);

      controller.restoreDraft(
        LitchcraftCanvas.blank().paint(<int>[0], 1, 0x112233),
      );
      await library.flush();
      await library.dismissWelcome();
      var restored = await store.load();
      expect(restored.draft!.canvas.colors.first, 0x112233);
      expect(restored.previousDraft!.isSupported, isFalse);
      expect(restored.previousDraft!.toJson(), corruptSource);

      await prefs.setString(LitchcraftArtworkStore.currentKey, '{interrupted');
      restored = await store.load();
      expect(restored.recovered, isTrue);
      expect(restored.previousDraft!.toJson(), corruptSource);

      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );
}
