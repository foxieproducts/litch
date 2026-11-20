import 'support/memory_ota_target_store.dart';
import 'dart:convert';
import 'dart:io';

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

class _NoRadio implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _RejectingStore extends LitchcraftArtworkStore {
  _RejectingStore(super.preferences);
  bool reject = false;
  @override
  Future<void> save(LitchcraftLibrarySnapshot snapshot) {
    if (reject) throw StateError('Controlled storage write failure');
    return super.save(snapshot);
  }
}

LitchcraftArtwork _artwork({
  String id = 'imported',
  String name = 'Named imported artwork',
  int durationMs = 701,
  DateTime? time,
}) {
  final timestamp = time ?? DateTime.utc(2026, 9, 1);
  return LitchcraftArtwork.animated(
    id: id,
    name: name,
    document: LitchcraftAnimationDocument(
      frames: [
        LitchcraftAnimationFrame(
          durationMs: durationMs,
          emitters: List.generate(
            256,
            (i) => LitchcraftAnimatedEmitter(
              color: 0x123456 + i,
              effect: i == 0
                  ? LitchcraftEffectStyle.breathe
                  : LitchcraftEffectStyle.off,
              effectDurationMs: i == 0 ? 1700 : 0,
            ),
          ),
        ),
      ],
      topology: LitchcraftTopology.v3,
      favoriteColors: litchcraftDefaultFavoriteColors,
    ),
    createdAt: timestamp,
    modifiedAt: timestamp,
  );
}

class _Editor {
  _Editor(this.ble, this.controller, this.library);
  final LitchBleService ble;
  final LitchcraftController controller;
  final LitchcraftLibrary library;
  static Future<_Editor> open(LitchcraftArtworkStore store) async {
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoRadio(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller, store: store);
    await library.ready;
    return _Editor(ble, controller, library);
  }

  Future<void> close() async {
    library.dispose();
    controller.dispose();
    await ble.dispose();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final files in [false, true]) {
    test(
      'external metadata, colliding IDs and anonymous-looking names survive ${files ? 'file' : 'preferences'} restart without saved binding',
      () async {
        SharedPreferences.setMockInitialValues({});
        final preferences = await SharedPreferences.getInstance();
        final directory = await Directory.systemTemp.createTemp(
          'litch-metadata-',
        );
        addTearDown(() => directory.delete(recursive: true));
        Future<LitchcraftArtworkStore> store() async => files
            ? await LitchcraftArtworkStore.open(
                preferences: preferences,
                directory: directory,
              )
            : LitchcraftArtworkStore(preferences);
        final saved = _artwork(
          id: 'draft',
          name: 'Saved local work',
          durationMs: 1100,
        );
        final imported = _artwork(id: 'draft', name: 'Litchcraft');
        final initialStore = await store();
        await initialStore.save(LitchcraftLibrarySnapshot(artworks: [saved]));
        var editor = await _Editor.open(initialStore);
        await editor.library.open(
          imported,
          source: LitchcraftOpenSource.imported,
        );
        expect(editor.library.activeName, imported.name);
        expect(editor.library.dirty, isTrue);
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          imported.toJson(),
        );
        await editor.close();
        editor = await _Editor.open(await store());
        expect(editor.library.activeName, imported.name);
        expect(editor.library.dirty, isTrue);
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          imported.toJson(),
        );
        await editor.library.save(imported.name);
        expect(editor.library.artworks, hasLength(2));
        expect(
          (await editor.library.artworks
                  .firstWhere((art) => art.id == saved.id)
                  .resolve())
              .toJson(),
          saved.toJson(),
        );
        final firstSaved = await editor.library.artworks.first.resolve();
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          firstSaved.toJson(),
        );
        await editor.close();
        editor = await _Editor.open(await store());
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          firstSaved.toJson(),
        );
        final localId = editor.library.currentArtworkForSharing('Untitled').id;
        expect(localId, isNot(imported.id));
        await editor.library.save('Another copy', saveAs: true);
        final savedCopy = await editor.library.artworks.first.resolve();
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          savedCopy.toJson(),
        );
        await editor.close();
        editor = await _Editor.open(await store());
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          savedCopy.toJson(),
        );
        expect(
          editor.library.currentArtworkForSharing('Untitled').id,
          isNot(localId),
        );
        await editor.library.newArtwork();
        expect(editor.library.activeName, isNull);
        expect(
          editor.library.currentArtworkForSharing('Untitled').name,
          'Untitled',
        );
        await editor.close();
        editor = await _Editor.open(await store());
        expect(editor.library.activeName, isNull);
        expect(
          editor.library.currentArtworkForSharing('Untitled').name,
          'Untitled',
        );
        await editor.close();
      },
    );

    test(
      'saved dirty draft keeps its baseline and identity after ${files ? 'file' : 'preferences'} restart',
      () async {
        SharedPreferences.setMockInitialValues({});
        final preferences = await SharedPreferences.getInstance();
        final directory = await Directory.systemTemp.createTemp(
          'litch-metadata-dirty-',
        );
        addTearDown(() => directory.delete(recursive: true));
        Future<LitchcraftArtworkStore> store() async => files
            ? await LitchcraftArtworkStore.open(
                preferences: preferences,
                directory: directory,
              )
            : LitchcraftArtworkStore(preferences);
        final saved = _artwork();
        final initialStore = await store();
        await initialStore.save(
          LitchcraftLibrarySnapshot(artworks: [saved], draft: saved),
        );
        var editor = await _Editor.open(initialStore);
        expect(editor.library.dirty, isFalse);
        editor.library.setSelectedFrameDuration(1200);
        await editor.library.flush();
        await editor.close();
        editor = await _Editor.open(await store());
        expect(
          editor.library.dirty,
          isTrue,
          reason:
              'The saved baseline must not be replaced with the unsaved draft.',
        );
        expect(editor.library.animationDocument.frames.single.durationMs, 1200);
        expect(
          (await editor.library.artworks.single.resolve())
              .document
              .frames
              .single
              .durationMs,
          701,
        );
        await editor.library.save(saved.name);
        expect(editor.library.artworks.single.id, saved.id);
        expect(editor.library.artworks.single.createdAt, saved.createdAt);
        expect(editor.library.dirty, isFalse);
        final resaved = await editor.library.artworks.single.resolve();
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          resaved.toJson(),
        );
        await editor.close();
        editor = await _Editor.open(await store());
        expect(
          editor.library.currentArtworkForSharing('Untitled').toJson(),
          resaved.toJson(),
        );
        await editor.close();
      },
    );
  }

  test(
    'failed Save rolls imported metadata and unsaved ownership back; future timestamps remain valid after editing',
    () async {
      SharedPreferences.setMockInitialValues({});
      final store = _RejectingStore(await SharedPreferences.getInstance());
      final editor = await _Editor.open(store);
      final imported = _artwork(time: DateTime.utc(2030, 1, 1));
      await editor.library.open(
        imported,
        source: LitchcraftOpenSource.imported,
      );
      store.reject = true;
      await expectLater(editor.library.save('Local copy'), throwsStateError);
      expect(editor.library.artworks, isEmpty);
      expect(editor.library.dirty, isTrue);
      expect(
        editor.library.currentArtworkForSharing('Untitled').toJson(),
        imported.toJson(),
      );
      store.reject = false;
      editor.library.setSelectedFrameDuration(1450);
      await editor.library.flush();
      final edited = editor.library.currentArtworkForSharing('Untitled');
      expect(edited.id, imported.id);
      expect(edited.name, imported.name);
      expect(edited.createdAt, imported.createdAt);
      expect(edited.modifiedAt.isBefore(imported.modifiedAt), isFalse);
      expect(edited.document.frames.single.durationMs, 1450);
      await editor.close();
    },
  );

  test(
    'file recovery retains external origin and invalid origin rejects current catalog',
    () async {
      SharedPreferences.setMockInitialValues({});
      final preferences = await SharedPreferences.getInstance();
      final directory = await Directory.systemTemp.createTemp(
        'litch-metadata-recovery-',
      );
      addTearDown(() => directory.delete(recursive: true));
      final store = await LitchcraftArtworkStore.open(
        preferences: preferences,
        directory: directory,
      );
      final imported = _artwork(id: 'draft', name: 'Litchcraft');
      await store.save(
        LitchcraftLibrarySnapshot(
          draft: imported,
          draftOrigin: LitchcraftDraftOrigin.external,
        ),
      );
      await store.save(
        LitchcraftLibrarySnapshot(
          draft: imported,
          draftOrigin: LitchcraftDraftOrigin.external,
          dismissedWelcome: true,
        ),
      );
      final current = File('${directory.path}/current.json');
      final invalid =
          jsonDecode(await current.readAsString()) as Map<String, dynamic>;
      invalid['draftOrigin'] = 'untrusted';
      await current.writeAsString(jsonEncode(invalid));
      final editor = await _Editor.open(
        await LitchcraftArtworkStore.open(
          preferences: preferences,
          directory: directory,
        ),
      );
      expect(editor.library.recovered, isTrue);
      expect(editor.library.dirty, isTrue);
      expect(
        editor.library.currentArtworkForSharing('Untitled').toJson(),
        imported.toJson(),
      );
      await editor.close();
    },
  );
}
