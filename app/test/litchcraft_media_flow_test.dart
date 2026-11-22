import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as image;
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/litchcraft_media.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:litch_mobile/src/widgets/litchcraft_motion_editor.dart';
import 'package:share_plus/share_plus.dart' show XFile;
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';

// The native radio is absent. No protocol, import, document or library behavior
// is replaced. The epoch accessor models a connection boundary while offline.
class _NoRadio implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ConnectionBoundaryBle extends LitchBleService {
  _ConnectionBoundaryBle()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _NoRadio());
  int epoch = 1;
  String? retainedDeviceId;
  @override
  String? get connectedId => retainedDeviceId ?? super.connectedId;
  @override
  int get connectionSessionToken => epoch;
}

// Only the native orientation projection is supplied. Every mutation and
// library/decoder path is the production offline controller implementation.
class _RotatedController extends LitchcraftController {
  _RotatedController(super.bleService);
  @override
  bool get flipped => true;
}

/// Hold only the async boundary before the real library admission guard.
class _DeferredImportLibrary extends LitchcraftLibrary {
  _DeferredImportLibrary(super.controller);
  final entered = Completer<void>();
  final release = Completer<void>();

  @override
  Future<void> open(
    LitchcraftArtwork artwork, {
    bool Function()? admissionGuard,
    LitchcraftOpenSource source = LitchcraftOpenSource.library,
  }) async {
    entered.complete();
    await release.future;
    await super.open(artwork, admissionGuard: admissionGuard, source: source);
  }
}

LitchcraftArtwork _artwork(String id, {int frames = 1, bool effects = true}) =>
    LitchcraftArtwork.animated(
      id: id,
      name: id,
      createdAt: DateTime.utc(2026, 10, 9),
      modifiedAt: DateTime.utc(2026, 10, 9),
      document: LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors.reversed,
        sparkleSeed: 9182,
        frames: [
          for (var frame = 0; frame < frames; frame++)
            LitchcraftAnimationFrame(
              durationMs: frame == 0 ? 700 : 1250,
              emitters: List.generate(
                256,
                (index) => LitchcraftAnimatedEmitter(
                  color: (0x123456 + frame * 0x23456 + index * 101) & 0xffffff,
                  effect: effects && index == frame
                      ? LitchcraftEffectStyle.breathe
                      : LitchcraftEffectStyle.off,
                  effectDurationMs: effects && index == frame ? 2300 : 0,
                ),
              ),
            ),
        ],
      ),
    );

Uint8List _png() {
  final raster = image.Image(width: 8, height: 8, numChannels: 4);
  for (var y = 0; y < 8; y++) {
    for (var x = 0; x < 8; x++) {
      raster.setPixelRgba(x, y, x * 31, y * 31, (x + y) * 17, 255);
    }
  }
  return image.encodePng(raster);
}

Uint8List _gif() {
  final first = image.Image(width: 8, height: 8, numChannels: 3)
    ..frameDuration = 100;
  final second = image.Image(width: 8, height: 8, numChannels: 3)
    ..frameDuration = 200;
  image.fill(first, color: image.ColorRgb8(255, 0, 0));
  image.fill(second, color: image.ColorRgb8(0, 0, 255));
  first.addFrame(second);
  return image.encodeGif(first);
}

Uint8List _longGif() {
  final frames = <image.Image>[];
  for (final color in [0xff0000, 0x00ff00, 0x0000ff]) {
    final frame = image.Image(width: 8, height: 8, numChannels: 3)
      ..frameDuration = 5000;
    image.fill(
      frame,
      color: image.ColorRgb8(color >> 16, (color >> 8) & 255, color & 255),
    );
    frames.add(frame);
  }
  frames.first.addFrame(frames[1]);
  frames.first.addFrame(frames[2]);
  return image.encodeGif(frames.first);
}

Future<void> _until(WidgetTester tester, bool Function() ready) async {
  for (var turn = 0; turn < 120 && !ready(); turn++) {
    await tester.pump(const Duration(milliseconds: 10));
    // Advance widget timers, then give native file/isolate work real time.
    // A real-only wait would leave widget-zone continuations unpumped.
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
  }
  expect(
    ready(),
    isTrue,
    reason: 'The production asynchronous flow must finish.',
  );
  await tester.pump(const Duration(milliseconds: 250));
}

Future<void> _tap(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pump();
  await tester.tap(target);
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 250));
}

Future<void> _capture(WidgetTester tester, String name) => captureHealthReview(
  tester,
  name,
  boundaryFinder: find.byKey(const ValueKey('media-flow-capture')),
);

class _Flow {
  _Flow(this.ble, this.controller, this.library, this.oldDocument);
  final _ConnectionBoundaryBle ble;
  final LitchcraftController controller;
  final LitchcraftLibrary library;
  final LitchcraftAnimationDocument oldDocument;

  static Future<_Flow> mount(
    WidgetTester tester, {
    required Future<XFile?> Function() picker,
    bool rotated = false,
    Size size = const Size(393, 852),
    double scale = 1,
    bool fonts = false,
  }) async {
    SharedPreferences.setMockInitialValues({});
    // The editor and the media dialog both read the real logical window.
    tester.view.physicalSize = size * tester.view.devicePixelRatio;
    addTearDown(tester.view.resetPhysicalSize);
    if (fonts) await loadHealthReviewFonts(tester, useSdkFonts: true);
    final ble = _ConnectionBoundaryBle();
    final controller = rotated
        ? _RotatedController(ble)
        : LitchcraftController(ble);
    final preferences = await SharedPreferences.getInstance();
    final library = LitchcraftLibrary(
      controller,
      store: LitchcraftArtworkStore(preferences),
    );
    await library.ready;
    library.dismissedWelcome = true;
    final oldDocument = _artwork('old-draft', effects: false).document;
    controller.restoreDocument(oldDocument);
    await library.flush();
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    });
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: buildLitchTheme(),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: TextScaler.linear(scale)),
          child: RepaintBoundary(
            key: const ValueKey('media-flow-capture'),
            child: child!,
          ),
        ),
        home: Scaffold(
          body: LitchcraftScreen(
            controller: controller,
            library: library,
            artworkFilePicker: picker,
            phoneMotionSamples: const Stream.empty(),
          ),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 300));
    await _tap(tester, find.byKey(const ValueKey('craft-artwork-menu')));
    return _Flow(ble, controller, library, oldDocument);
  }

  Future<void> beginImport(WidgetTester tester) =>
      _tap(tester, find.byKey(const ValueKey('craft-import')));

  Future<void> preview(WidgetTester tester) => _until(
    tester,
    () =>
        find.byKey(const ValueKey('craft-import-accept')).evaluate().isNotEmpty,
  );

  Future<void> accept(WidgetTester tester) =>
      _tap(tester, find.byKey(const ValueKey('craft-import-accept')));

  void expectUnchanged() {
    expect(
      sameLitchcraftDocument(library.animationDocument, oldDocument),
      isTrue,
    );
    expect(library.previousDraft, isNull);
  }
}

void main() {
  final strings = lookupAppLocalizations(const Locale('en'));

  for (final frames in [1, 3]) {
    testWidgets(
      'editable $frames-frame Import retains complete data and old draft',
      (tester) async {
        final imported = _artwork('editable-$frames', frames: frames);
        var pickerCalls = 0;
        final flow = await _Flow.mount(
          tester,
          picker: () async {
            pickerCalls++;
            return XFile.fromData(
              encodeLitchcraftEditableArtwork(imported),
              name: 'editable.litchcraft.json',
              mimeType: 'application/json',
            );
          },
        );
        await flow.beginImport(tester);
        await flow.preview(tester);
        expect(pickerCalls, 1);
        expect(find.text(imported.name), findsOneWidget);
        expect(find.text(strings.craftEditableNotice), findsOneWidget);
        flow.expectUnchanged();
        await flow.accept(tester);
        await _until(
          tester,
          () => sameLitchcraftDocument(
            flow.library.animationDocument,
            imported.document,
          ),
        );
        expect(flow.library.animationDocument.frames, hasLength(frames));
        expect(flow.library.animationDocument.frames.first.durationMs, 700);
        expect(
          flow.library.animationDocument.frames.first.emitters.first.effect,
          LitchcraftEffectStyle.breathe,
        );
        final previous = await flow.library.previousDraft!.resolve();
        expect(
          sameLitchcraftDocument(previous.document, flow.oldDocument),
          isTrue,
        );
        expect(tester.takeException(), isNull);
      },
    );
  }

  for (final frames in [1, 3]) {
    testWidgets(
      'native Share ingress retains imported $frames-frame envelope metadata',
      (tester) async {
        final imported = _artwork('named-editable-$frames', frames: frames);
        final temporary = await tester.runAsync(
          () => Directory.systemTemp.createTemp('litch-editable-share-'),
        );
        const pathChannel = MethodChannel('plugins.flutter.io/path_provider');
        const shareChannel = MethodChannel('dev.fluttercommunity.plus/share');
        Map<String, dynamic>? shared;
        final outgoing = Completer<void>();
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        messenger.setMockMethodCallHandler(
          pathChannel,
          (_) async => temporary!.path,
        );
        messenger.setMockMethodCallHandler(shareChannel, (call) async {
          final args = call.arguments as Map;
          final path = (args['paths'] as List).single as String;
          shared =
              jsonDecode(await File(path).readAsString())
                  as Map<String, dynamic>;
          outgoing.complete();
          return 'local-files';
        });
        addTearDown(() async {
          messenger.setMockMethodCallHandler(pathChannel, null);
          messenger.setMockMethodCallHandler(shareChannel, null);
          await tester.runAsync(() => temporary!.delete(recursive: true));
        });
        final flow = await _Flow.mount(
          tester,
          picker: () async => XFile.fromData(
            encodeLitchcraftEditableArtwork(imported),
            name: 'named.litchcraft.json',
          ),
        );
        await flow.beginImport(tester);
        await flow.preview(tester);
        await flow.accept(tester);
        await _until(
          tester,
          () => sameLitchcraftDocument(
            flow.library.animationDocument,
            imported.document,
          ),
        );
        await _tap(tester, find.byKey(const ValueKey('craft-share')));
        await _tap(
          tester,
          find.byWidgetPredicate(
            (w) =>
                w is DropdownButtonFormField &&
                w.key.toString().contains('craft-export-format'),
          ),
        );
        await _tap(tester, find.text(strings.craftExportEditableArtwork).last);
        await _tap(tester, find.byKey(const ValueKey('craft-export-share')));
        await _until(tester, () => outgoing.isCompleted);
        expect(
          shared,
          imported.toJson(),
          reason:
              'This exercises actual Import→Share, including native outgoing bytes, metadata and canonical document CRC.',
        );
        expect(flow.library.dirty, isTrue);
        expect(flow.library.artworks, isEmpty);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'Previous Canvas with imported saved-ID collision has no destructive actions and opens as external',
    (tester) async {
      late LitchcraftArtwork imported;
      final flow = await _Flow.mount(
        tester,
        picker: () async => XFile.fromData(
          encodeLitchcraftEditableArtwork(imported),
          name: 'colliding.json',
        ),
      );
      await flow.library.open(_artwork('saved-source', frames: 3));
      await flow.library.save('Saved local artwork');
      final saved = await flow.library.artworks.single.resolve();
      imported = _artwork(
        'imported-title',
        frames: 3,
      ).copyWith(id: saved.id, name: 'Imported title');
      await flow.beginImport(tester);
      await flow.preview(tester);
      await flow.accept(tester);
      await _until(tester, () => flow.library.activeName == imported.name);
      await flow.library.open(
        _artwork('Replacement'),
        source: LitchcraftOpenSource.imported,
      );
      await tester.pumpAndSettle();
      await _tap(tester, find.byKey(const ValueKey('craft-library')));
      final previous = find.byKey(
        ValueKey('craft-artwork-previous-${saved.id}'),
      );
      final stored = find.byKey(ValueKey('craft-artwork-${saved.id}'));
      expect(previous, findsOneWidget);
      expect(stored, findsOneWidget);
      expect(
        find.descendant(
          of: previous,
          matching: find.byTooltip(strings.craftRename),
        ),
        findsNothing,
      );
      expect(
        find.descendant(
          of: previous,
          matching: find.byTooltip(strings.craftDelete),
        ),
        findsNothing,
      );
      expect(
        find.descendant(
          of: stored,
          matching: find.byTooltip(strings.craftRename),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: stored,
          matching: find.byTooltip(strings.craftDelete),
        ),
        findsOneWidget,
      );
      expect(
        find.byKey(ValueKey('craft-preview-artwork-previous-${saved.id}')),
        findsOneWidget,
      );
      expect(
        find.byKey(ValueKey('craft-preview-artwork-${saved.id}')),
        findsOneWidget,
      );
      await _tap(tester, previous);
      await _tap(tester, find.text(strings.craftOpen).last);
      await _until(tester, () => flow.library.activeName == imported.name);
      expect(flow.library.dirty, isTrue);
      expect(
        flow.library.currentArtworkForSharing('Untitled').toJson(),
        imported.toJson(),
      );
      await _tap(tester, find.byKey(const ValueKey('craft-save')));
      await _until(tester, () => flow.library.artworks.length == 2);
      expect(
        (await flow.library.artworks
                .firstWhere((art) => art.id == saved.id)
                .resolve())
            .toJson(),
        saved.toJson(),
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'unchanged saved artwork and Previous Canvas keep separate row roles when storage shares their source object',
    (tester) async {
      final flow = await _Flow.mount(tester, picker: () async => null);
      await flow.library.save('Saved local artwork');
      final saved = flow.library.artworks.single;
      await flow.library.newArtwork();
      expect(identical(flow.library.previousDraft, saved), isTrue);
      await tester.pumpAndSettle();
      await _tap(tester, find.byKey(const ValueKey('craft-library')));
      final previous = find.byKey(
        ValueKey('craft-artwork-previous-${saved.id}'),
      );
      final stored = find.byKey(ValueKey('craft-artwork-${saved.id}'));
      expect(previous, findsOneWidget);
      expect(stored, findsOneWidget);
      expect(
        find.descendant(
          of: previous,
          matching: find.text(strings.craftRecoveryDraft),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: stored, matching: find.text(saved.name)),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: previous,
          matching: find.byTooltip(strings.craftRename),
        ),
        findsNothing,
      );
      expect(
        find.descendant(
          of: previous,
          matching: find.byTooltip(strings.craftDelete),
        ),
        findsNothing,
      );
      expect(
        find.descendant(
          of: stored,
          matching: find.byTooltip(strings.craftDelete),
        ),
        findsOneWidget,
      );
      await _tap(tester, previous);
      await _tap(tester, find.text(strings.craftOpen).last);
      await _until(tester, () => flow.library.activeName == saved.name);
      expect(flow.library.dirty, isTrue);
      expect(
        flow.library.currentArtworkForSharing('Untitled').toJson(),
        saved.toJson(),
      );
      await _tap(tester, find.byKey(const ValueKey('craft-save')));
      await _until(tester, () => flow.library.artworks.length == 2);
      expect(flow.library.artworks.last.toJson(), saved.toJson());
      expect(tester.takeException(), isNull);
    },
  );

  for (final change in [
    'same peer',
    'picker returns offline',
    'other peer',
    'later epoch',
    'after acceptance epoch',
    'source',
    'replacement',
    'session',
  ]) {
    testWidgets('connected Import picker return: $change', (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(393, 852);
      addTearDown(tester.view.reset);
      SharedPreferences.setMockInitialValues({});
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final radio = VirtualLitchGattBle(native);
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: radio,
        gattSessionFactory: ReactiveBleGattSessionFactory(radio),
      );
      final controller = LitchcraftController(ble);
      final library = change == 'after acceptance epoch'
          ? _DeferredImportLibrary(controller)
          : LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      final pending = Completer<XFile?>();
      var pickerCalls = 0;
      Future<void> step() async {
        await tester.pump();
        if (radio.hasPendingOperations) {
          await radio.flush(tester, maxOperations: 1);
        } else {
          await radio.command(tester, {
            'op': 'advance',
            'ms': 10,
          }, flushOperations: false);
          await tester.pump(const Duration(milliseconds: 10));
        }
      }

      Future<void> until(bool Function() done) async {
        for (var turn = 0; turn < 1000 && !done(); turn++) {
          await step();
        }
        expect(
          done(),
          isTrue,
          reason: 'The production connected Import flow must settle.',
        );
      }

      Future<void> finish(Future<dynamic> work) async {
        var done = false;
        Object? failure;
        work.then<void>(
          (_) => done = true,
          onError: (Object error, StackTrace _) {
            failure = error;
            done = true;
          },
        );
        await until(() => done);
        if (failure != null) throw failure!;
      }

      addTearDown(() async {
        try {
          await radio.close();
        } finally {
          await tester.runAsync(native.close);
        }
      });
      try {
        await radio.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await radio.advance(tester, 1000);
        await finish(ble.connect('import-board'));
        await finish(controller.enter());
        await finish(
          controller.replaceDocument(
            _artwork('original', frames: 32, effects: false).document,
          ),
        );
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.usesFrameStoreDocument, isTrue);
        final original = library.animationDocument;
        final generation = library.session.replacementGeneration;
        final connection = ble.connectionSessionToken;
        final session = controller.sessionId;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            theme: buildLitchTheme(),
            home: Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                artworkFilePicker: () {
                  pickerCalls++;
                  return pending.future;
                },
                phoneMotionSamples: const Stream.empty(),
              ),
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 300));
        await _tap(tester, find.byKey(const ValueKey('craft-artwork-menu')));
        await _tap(tester, find.byKey(const ValueKey('craft-import')));
        expect(pickerCalls, 1);
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.inactive,
        );
        tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
        tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
        await finish(ble.setAppForegroundActive(false));
        await finish(ble.disconnect());
        expect(controller.status, LitchcraftStatus.offline);
        // The existing SDK adapter supplies native callbacks; real firmware owns
        // Store, and real controller lifecycle revalidation precedes selection.
        final offlineSession = controller.sessionId;
        if (change != 'picker returns offline') {
          await finish(
            ble.connect(
              change == 'other peer' ? 'other-board' : 'import-board',
            ),
          );
          await finish(ble.setAppForegroundActive(true));
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
          await until(() => controller.status == LitchcraftStatus.live);
          expect(
            ble.connectedId,
            change == 'other peer' ? 'other-board' : 'import-board',
          );
        } else {
          await finish(ble.setAppForegroundActive(true));
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
          expect(ble.isConnected, isFalse);
        }
        expect(ble.connectionSessionToken, isNot(connection));
        expect(controller.sessionId, session);
        expect(library.animationDocument, same(original));
        expect(library.session.replacementGeneration, generation);
        expect(
          sameLitchcraftDocument(library.animationDocument, original),
          isTrue,
        );
        if (change == 'source') {
          controller.restoreDocument(
            _artwork('new-source', frames: 32).document,
          );
        } else if (change == 'replacement') {
          library.session.open(original);
          expect(library.animationDocument, same(original));
          expect(library.session.replacementGeneration, isNot(generation));
          expect(controller.sessionId, session);
        } else if (change == 'session') {
          await finish(controller.leave());
          await finish(controller.enter());
          expect(controller.sessionId, isNot(session));
        }
        final expectedRetained = library.animationDocument;
        final oldPrevious = library.previousDraft;
        final incoming = _artwork('connected-picker-import', frames: 32);
        pending.complete(
          XFile.fromData(
            encodeLitchcraftEditableArtwork(incoming),
            name: 'connected.litchcraft.json',
          ),
        );
        await _until(
          tester,
          () => find
              .byKey(const ValueKey('craft-import-accept'))
              .evaluate()
              .isNotEmpty,
        );
        expect(find.text(incoming.name), findsOneWidget);
        if (change == 'picker returns offline') {
          expect(
            ble.isConnected,
            isFalse,
            reason: 'The actual preview must open before the board reconnects.',
          );
          await finish(ble.connect('import-board'));
          await finish(controller.revalidateSelectedBoard());
          expect(ble.connectedId, 'import-board');
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.sessionId, session);
          expect(library.animationDocument, same(original));
          expect(library.session.replacementGeneration, generation);
          expect(
            offlineSession,
            session,
            reason: 'Transport loss retains meaningful mode-session authority.',
          );
        }
        if (change == 'later epoch') {
          await finish(ble.disconnect());
          await finish(ble.connect('import-board'));
          await finish(controller.revalidateSelectedBoard());
          expect(library.animationDocument, same(original));
          expect(controller.sessionId, session);
          expect(library.session.replacementGeneration, generation);
        }
        await _tap(tester, find.byKey(const ValueKey('craft-import-accept')));
        if (library is _DeferredImportLibrary) {
          await until(() => library.entered.isCompleted);
          await finish(ble.disconnect());
          await finish(ble.connect('import-board'));
          await finish(controller.revalidateSelectedBoard());
          expect(library.animationDocument, same(original));
          expect(controller.sessionId, session);
          expect(library.session.replacementGeneration, generation);
          library.release.complete();
        }
        await until(
          () =>
              sameLitchcraftDocument(
                library.animationDocument,
                incoming.document,
              ) ||
              find.text(strings.craftImportChanged).evaluate().isNotEmpty,
        );
        if (change == 'same peer' ||
            change == 'picker returns offline' ||
            change == 'later epoch') {
          expect(
            find.text(strings.craftImportChanged),
            findsNothing,
            reason:
                'Returning from the native picker to the same unchanged board must not reject the confirmed import.',
          );
          expect(
            sameLitchcraftDocument(
              library.animationDocument,
              incoming.document,
            ),
            isTrue,
          );
          await until(
            () =>
                !controller.artworkDeliveryPending &&
                !controller.storeReplacementUnresolved &&
                !controller.pending,
          );
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.errorMessage, isNull);
          expect(library.saveFailed, isFalse);
          expect(find.text(strings.craftActionFailed), findsNothing);
          expect(find.text(strings.craftSaveFailed), findsNothing);
          // Read every clean frame through the existing native Store observer;
          // a cleared pending flag alone does not establish accepted delivery.
          await radio.advance(tester, 6000);
          await until(() => !controller.pending && !radio.hasPendingOperations);
          for (
            var index = 0;
            index < incoming.document.frames.length;
            index++
          ) {
            final frame =
                (await radio.command(tester, {
                      'op': 'store_frame',
                      'index': index,
                    }))['store_frame']
                    as Map<String, dynamic>;
            expect(frame['dirty'], isFalse);
            expect(frame['live_generation'], frame['root_generation']);
            expect(frame['frame_count'], 32);
            expect(frame['frame_index'], index);
            expect(
              frame['duration_ms'],
              incoming.document.frames[index].durationMs,
            );
            expect(frame['emitters'], [
              for (final emitter in incoming.document.frames[index].emitters)
                [emitter.color, emitter.effect.id, emitter.effectDurationMs],
            ]);
            expect(frame['favorite_colors'], incoming.document.favoriteColors);
            expect(frame['topology'], incoming.document.topology.id);
            expect(frame['sparkle_seed'], incoming.document.sparkleSeed);
            expect(
              frame['playback_on_entry'],
              incoming.document.playbackOnEntry,
            );
          }
          expect(library.previousDraft, isNotNull);
          expect(
            sameLitchcraftDocument(library.previousDraft!.document, original),
            isTrue,
          );
        } else {
          expect(find.text(strings.craftImportChanged), findsOneWidget);
          expect(library.animationDocument, same(expectedRetained));
          expect(library.previousDraft, same(oldPrevious));
        }
        expect(tester.takeException(), isNull);
      } finally {
        if (library is _DeferredImportLibrary && !library.release.isCompleted) {
          library.release.complete();
        }
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await finish(ble.disconnect());
        await finish(ble.dispose());
      }
    });
  }

  for (final changeConnection in [true, false]) {
    testWidgets(
      'held picker rejects ${changeConnection ? 'new BLE epoch with same session' : 'changed source document'}',
      (tester) async {
        final pending = Completer<XFile?>();
        final flow = await _Flow.mount(tester, picker: () => pending.future);
        final session = flow.controller.sessionId;
        await flow.beginImport(tester);
        final changed = _artwork(
          'new-source',
          frames: 2,
          effects: false,
        ).document;
        if (changeConnection) {
          flow.ble.epoch++;
        } else {
          flow.controller.restoreDocument(changed);
        }
        expect(flow.controller.sessionId, session);
        pending.complete(
          XFile.fromData(
            encodeLitchcraftEditableArtwork(_artwork('late-picker')),
            name: 'late.litchcraft.json',
          ),
        );
        await flow.preview(tester);
        await flow.accept(tester);
        await _until(
          tester,
          () => find.text(strings.craftImportChanged).evaluate().isNotEmpty,
        );
        expect(
          sameLitchcraftDocument(
            flow.library.animationDocument,
            changeConnection ? flow.oldDocument : changed,
          ),
          isTrue,
        );
        expect(flow.library.previousDraft, isNull);
        expect(tester.takeException(), isNull);
      },
    );
  }

  for (final renewEpoch in [false, true]) {
    testWidgets(
      'offline retained peer ID ${renewEpoch ? 'cannot rebind a changed' : 'preserves an unchanged'} picker epoch',
      (tester) async {
        final pending = Completer<XFile?>();
        final flow = await _Flow.mount(tester, picker: () => pending.future);
        flow.ble.retainedDeviceId = 'old-disconnected-board';
        expect(flow.ble.isConnected, isFalse);
        await flow.beginImport(tester);
        if (renewEpoch) flow.ble.epoch++;
        final incoming = _artwork('offline-retained-id');
        pending.complete(
          XFile.fromData(
            encodeLitchcraftEditableArtwork(incoming),
            name: 'offline.litchcraft.json',
          ),
        );
        await flow.preview(tester);
        await flow.accept(tester);
        if (renewEpoch) {
          await _until(
            tester,
            () => find.text(strings.craftImportChanged).evaluate().isNotEmpty,
          );
          flow.expectUnchanged();
        } else {
          await _until(
            tester,
            () => sameLitchcraftDocument(
              flow.library.animationDocument,
              incoming.document,
            ),
          );
          expect(flow.library.previousDraft, isNotNull);
          expect(find.text(strings.craftImportChanged), findsNothing);
        }
        expect(tester.takeException(), isNull);
      },
    );
  }

  for (final dismissal in ['picker cancel', 'preview cancel', 'preview back']) {
    testWidgets(
      '$dismissal leaves the current document and prior draft untouched',
      (tester) async {
        final flow = await _Flow.mount(
          tester,
          picker: () async => dismissal == 'picker cancel'
              ? null
              : XFile.fromData(
                  encodeLitchcraftEditableArtwork(_artwork('cancelled')),
                  name: 'cancelled.json',
                ),
        );
        await flow.beginImport(tester);
        if (dismissal != 'picker cancel') {
          await flow.preview(tester);
          if (dismissal == 'preview back') {
            await tester.binding.handlePopRoute();
            await tester.pump(const Duration(milliseconds: 300));
          } else {
            await _tap(tester, find.text(strings.cancelAction).last);
          }
        }
        await tester.pumpAndSettle();
        flow.expectUnchanged();
        expect(find.byType(AlertDialog), findsNothing);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'deferred library admission rechecks the guard before retiring draft',
    (tester) async {
      final flow = await _Flow.mount(tester, picker: () async => null);
      final resolved = _artwork('deferred-new', frames: 2);
      final deferred = Completer<LitchcraftArtwork>();
      final art = LitchcraftArtwork.deferred(
        id: resolved.id,
        name: resolved.name,
        createdAt: resolved.createdAt,
        modifiedAt: resolved.modifiedAt,
        frameCount: resolved.frameCount,
        preview: resolved.canvas,
        favoriteColors: resolved.favoriteColors,
        storageReference: Object(),
        resolver: () => deferred.future,
      );
      final epoch = flow.ble.connectionSessionToken;
      final opening = flow.library.open(
        art,
        admissionGuard: () => epoch == flow.ble.connectionSessionToken,
      );
      await tester.pump();
      flow.ble.epoch++;
      deferred.complete(resolved);
      await opening;
      flow.expectUnchanged();
    },
  );

  for (final width in [320.0, 393.0]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'rotated PNG preview matches accepted view at $width / ${scale}x',
        (tester) async {
          final flow = await _Flow.mount(
            tester,
            picker: () async => XFile.fromData(_png(), name: 'asymmetric.png'),
            rotated: true,
            size: Size(width, 852),
            scale: scale,
            fonts: true,
          );
          await _capture(tester, 'media-$width-${scale}x-artwork');
          await flow.beginImport(tester);
          await _until(
            tester,
            () => find
                .byKey(const ValueKey('craft-import-summary'))
                .evaluate()
                .isNotEmpty,
          );
          final thumbnail = tester.widget<LitchcraftThumbnail>(
            find.descendant(
              of: find.byType(AlertDialog),
              matching: find.byType(LitchcraftThumbnail),
            ),
          );
          expect(thumbnail.flipped, isTrue);
          expect(
            thumbnail.canvas.colors,
            isNot(orderedEquals(thumbnail.canvas.colors.reversed)),
          );
          flow.expectUnchanged();
          await _capture(tester, 'media-$width-${scale}x-png-import');
          await flow.accept(tester);
          await _until(tester, () => flow.library.previousDraft != null);
          final accepted = tester.widget<LitchcraftCanvasView>(
            find.byType(LitchcraftCanvasView),
          );
          expect(accepted.flipped, thumbnail.flipped);
          expect(
            accepted.canvas.colors,
            orderedEquals(thumbnail.canvas.colors),
          );
          expect(flow.library.animationDocument.frames, hasLength(1));
          expect(tester.takeException(), isNull);
          await _tap(tester, find.byKey(const ValueKey('craft-share')));
          expect(
            find.byKey(const ValueKey('craft-export-share')),
            findsOneWidget,
          );
          await tester.pumpAndSettle();
          await _capture(tester, 'media-$width-${scale}x-share-options');
          await _tap(tester, find.text(strings.cancelAction).last);
        },
      );
    }
  }

  testWidgets(
    'GIF invalid range explains bounds then recovers converted output',
    (tester) async {
      final flow = await _Flow.mount(
        tester,
        picker: () async => XFile.fromData(_gif(), name: 'two-frames.gif'),
      );
      await flow.beginImport(tester);
      await _until(
        tester,
        () => find
            .byKey(const ValueKey('craft-import-summary'))
            .evaluate()
            .isNotEmpty,
      );
      expect(
        find.text(strings.craftImportSourceSummary('0.3', 2)),
        findsOneWidget,
      );
      expect(
        find.text(strings.craftImportConvertedSummary('0.3', 2)),
        findsOneWidget,
      );
      final first = find.byKey(const ValueKey('craft-import-first'));
      final last = find.byKey(const ValueKey('craft-import-last'));
      await tester.ensureVisible(first);
      await tester.enterText(first, '3');
      await tester.pump();
      expect(
        find.text(strings.craftImportFrameRangeInvalid(2)),
        findsOneWidget,
      );
      expect(
        tester
            .widget<FilledButton>(
              find.byKey(const ValueKey('craft-import-accept')),
            )
            .onPressed,
        isNull,
      );
      flow.expectUnchanged();
      await tester.enterText(first, '2');
      await tester.ensureVisible(last);
      await tester.enterText(last, '2');
      await _until(
        tester,
        () => find
            .text(strings.craftImportConvertedSummary('0.2', 1))
            .evaluate()
            .isNotEmpty,
      );
      expect(find.text(strings.craftImportFirstFramePreview), findsOneWidget);
      await flow.accept(tester);
      await _until(tester, () => flow.library.previousDraft != null);
      expect(flow.library.animationDocument.frames, hasLength(1));
      expect(flow.library.animationDocument.frames.single.durationMs, 200);
      expect(
        flow.library.animationDocument.frames.single.emitters.every(
          (emitter) => emitter.color == 0x0000ff,
        ),
        isTrue,
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'high frame rate keeps conversion error visible and recovers at lower rate',
    (tester) async {
      final flow = await _Flow.mount(
        tester,
        picker: () async => XFile.fromData(_longGif(), name: 'long.gif'),
        size: const Size(320, 852),
        scale: 2,
        fonts: true,
      );
      final semantics = tester.ensureSemantics();
      try {
        await flow.beginImport(tester);
        await _until(
          tester,
          () => find
              .byKey(const ValueKey('craft-import-summary'))
              .evaluate()
              .isNotEmpty,
        );
        await _tap(
          tester,
          find.byKey(
            const ValueKey(
              'craft-import-timing:LitchcraftMediaTiming.chosenFps',
            ),
          ),
        );
        final error = find.byKey(const ValueKey('craft-import-error'));
        await _until(tester, () => error.evaluate().isNotEmpty);
        expect(find.text(strings.craftImportFrameLimitHelp), findsOneWidget);
        expect(
          error.hitTestable(),
          findsOneWidget,
          reason:
              'Recovery instructions must remain visible beside Import after scrolling to frame rate.',
        );
        expect(
          tester
              .getSemantics(error)
              .getSemanticsData()
              .flagsCollection
              .isLiveRegion,
          isTrue,
        );
        expect(
          tester
              .widget<FilledButton>(
                find.byKey(const ValueKey('craft-import-accept')),
              )
              .onPressed,
          isNull,
        );
        flow.expectUnchanged();
        await _capture(tester, 'media-320-2x-import-frame-limit');
        await _tap(tester, find.byKey(const ValueKey('craft-import-fps:5')));
        await _until(
          tester,
          () => find
              .text(strings.craftImportConvertedSummary('15.0', 75))
              .evaluate()
              .isNotEmpty,
        );
        expect(error, findsNothing);
        await _capture(tester, 'media-320-2x-import-frame-limit-recovered');
        await flow.accept(tester);
        await _until(tester, () => flow.library.previousDraft != null);
        expect(flow.library.animationDocument.frames, hasLength(75));
        expect(
          flow.library.animationDocument.frames.fold<int>(
            0,
            (sum, frame) => sum + frame.durationMs,
          ),
          15000,
        );
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets('animated Share shows configurable APNG and GIF at 200% text', (
    tester,
  ) async {
    final flow = await _Flow.mount(
      tester,
      picker: () async => null,
      size: const Size(320, 852),
      scale: 2,
      fonts: true,
    );
    final document = _artwork('animated-share', frames: 3).document;
    flow.controller.restoreDocument(document);
    await tester.pump();
    await _tap(tester, find.byKey(const ValueKey('craft-share')));
    expect(find.text(strings.craftAnimatedPng), findsOneWidget);
    expect(find.text('512 × 512'), findsOneWidget);
    expectReviewTextFits(
      tester,
      find.text(strings.craftAnimatedPng),
      'Selected Animated PNG wraps completely at 200%',
    );
    await _capture(tester, 'media-320-2x-apng-options-top');
    await tester.ensureVisible(
      find.byKey(const ValueKey('craft-export-summary')),
    );
    await tester.pump();
    await _capture(tester, 'media-320-2x-apng-options-summary');
    await _tap(
      tester,
      find.byKey(
        const ValueKey('craft-export-format:LitchcraftShareFormat.apng'),
      ),
    );
    await tester.pump(const Duration(milliseconds: 100));
    await _capture(tester, 'media-320-2x-format-menu');
    expectReviewTextFits(
      tester,
      find.text(strings.craftExportEditableArtwork).last,
      'Editable artwork menu label wraps completely at 200%',
    );
    expectReviewTextFits(
      tester,
      find.text(strings.craftAnimatedPng).last,
      'Animated PNG menu label wraps completely at 200%',
    );
    await _tap(tester, find.text(strings.craftGif).last);
    await tester.ensureVisible(find.text(strings.craftGifPaletteNotice));
    await tester.pump();
    await _capture(tester, 'media-320-2x-gif-options-summary');
    await _tap(
      tester,
      find.byKey(
        const ValueKey('craft-export-format:LitchcraftShareFormat.gif'),
      ),
    );
    await _tap(tester, find.text(strings.craftExportEditableArtwork).last);
    await tester.pump(const Duration(milliseconds: 100));
    expectReviewTextFits(
      tester,
      find.descendant(
        of: find.byKey(
          const ValueKey('craft-export-format:LitchcraftShareFormat.editable'),
        ),
        matching: find.text(strings.craftExportEditableArtwork),
      ),
      'Selected Editable artwork wraps completely at 200%',
    );
    await _capture(tester, 'media-320-2x-editable-options');
    expect(tester.takeException(), isNull);
    await _tap(tester, find.text(strings.cancelAction).last);
    expect(
      sameLitchcraftDocument(flow.library.animationDocument, document),
      isTrue,
    );
    expect(flow.library.previousDraft, isNull);
  });

  for (final size in [const Size(320, 640), const Size(640, 320)]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'actual timing panels fit $size at ${scale}x with exact targets',
        (tester) async {
          final flow = await _Flow.mount(
            tester,
            picker: () async => null,
            size: size,
            scale: scale,
            fonts: true,
          );
          final semantics = tester.ensureSemantics();
          try {
            await _capture(
              tester,
              'media-timing-${size.width}x${size.height}-${scale}x-artwork',
            );
            await _tap(tester, find.byKey(const ValueKey('craft-animate')));
            final frameSlider = find.byKey(
              const ValueKey('craft-frame-duration'),
            );
            final frameValue = find.byKey(
              const ValueKey('craft-frame-duration-context'),
            );
            expect(frameSlider, findsOneWidget);
            expect(frameValue, findsOneWidget);
            await tester.ensureVisible(frameValue);
            await tester.pump();
            expect(frameValue.hitTestable(), findsOneWidget);
            expect(tester.getSize(frameValue).height, greaterThanOrEqualTo(48));
            expect(tester.getSize(frameValue).width, greaterThanOrEqualTo(48));
            expect(
              tester.getSemantics(frameValue).getSemanticsData().label,
              strings.craftFrameDuration('0.7'),
            );
            expect(
              find.descendant(of: frameValue, matching: find.text('0.7 s')),
              findsOneWidget,
            );
            await _capture(
              tester,
              'media-timing-${size.width}x${size.height}-${scale}x-frames',
            );
            expect(tester.takeException(), isNull);
            await _tap(tester, frameValue);
            expect(
              tester
                  .widget<TextFormField>(
                    find.byKey(const ValueKey('craft-exact-duration-field')),
                  )
                  .controller!
                  .text,
              '700',
            );
            await _tap(tester, find.text(strings.cancelAction).last);
            expect(
              sameLitchcraftDocument(
                flow.library.animationDocument,
                flow.oldDocument,
              ),
              isTrue,
            );

            await _tap(tester, find.byKey(const ValueKey('craft-motion')));
            final effectSelector = find.byKey(
              const ValueKey('craft-effect-selector'),
            );
            if (effectSelector.evaluate().isNotEmpty) {
              await _tap(tester, effectSelector);
            }
            await _tap(
              tester,
              find.byKey(const ValueKey('craft-effect-breathe')),
            );
            final effectSlider = find.byKey(
              const ValueKey('craft-effect-duration'),
            );
            final effectValue = find.byKey(
              const ValueKey('craft-effect-duration-context'),
            );
            expect(effectSlider, findsOneWidget);
            final effectPanel = tester.widget<LitchcraftEffectEditorPanel>(
              find.byType(LitchcraftEffectEditorPanel),
            );
            expect(
              tester.widget<Slider>(effectSlider).onChanged,
              isNotNull,
              reason:
                  'Armed ${effectPanel.selectedEffect}, enabled ${effectPanel.enabled} must expose cycle editing.',
            );
            await tester.ensureVisible(effectValue);
            await tester.pump();
            expect(effectValue.hitTestable(), findsOneWidget);
            expect(
              tester.getSize(effectValue).height,
              greaterThanOrEqualTo(48),
            );
            expect(tester.getSize(effectValue).width, greaterThanOrEqualTo(48));
            expect(
              tester.getSemantics(effectValue).getSemanticsData().label,
              strings.craftEffectDuration('2.0'),
            );
            expect(tester.widget<TextButton>(effectValue).onPressed, isNotNull);
            await _tap(tester, effectValue);
            expect(
              tester
                  .widget<TextFormField>(
                    find.byKey(const ValueKey('craft-exact-duration-field')),
                  )
                  .controller!
                  .text,
              '2000',
            );
            await _tap(tester, find.text(strings.cancelAction).last);
            await tester.pump(const Duration(milliseconds: 250));
            await _capture(
              tester,
              'media-timing-${size.width}x${size.height}-${scale}x-effects',
            );
            expect(tester.takeException(), isNull);
          } finally {
            semantics.dispose();
          }
        },
      );
    }
  }

  for (final (format, frames) in [
    ('editable', 1),
    ('editable', 3),
    ('gif', 3),
    ('apng', 3),
  ]) {
    testWidgets(
      '$frames-frame $format Share emits valid native-boundary bytes',
      (tester) async {
        final nativeFile = Completer<Uint8List>();
        final directory = (await tester.runAsync(
          () => Directory.systemTemp.createTemp('litch-media-share-flow-'),
        ))!;
        const paths = MethodChannel('plugins.flutter.io/path_provider');
        const share = MethodChannel('dev.fluttercommunity.plus/share');
        final messenger = tester.binding.defaultBinaryMessenger;
        messenger.setMockMethodCallHandler(
          paths,
          (call) async => directory.path,
        );
        messenger.setMockMethodCallHandler(share, (call) async {
          expect(call.method, 'share');
          final args = call.arguments as Map<Object?, Object?>;
          final files = (args['paths'] as List<Object?>).cast<String>();
          expect(files, hasLength(1));
          expect(
            files.single,
            endsWith(
              format == 'editable'
                  ? '.litchcraft.json'
                  : format == 'gif'
                  ? '.gif'
                  : '.png',
            ),
          );
          nativeFile.complete(await File(files.single).readAsBytes());
          return ''; // Native share sheet was dismissed; nothing was sent.
        });
        addTearDown(() async {
          messenger.setMockMethodCallHandler(paths, null);
          messenger.setMockMethodCallHandler(share, null);
          await tester.runAsync(() => directory.delete(recursive: true));
        });
        final flow = await _Flow.mount(tester, picker: () async => null);
        final document = _artwork(
          'rich-$frames-frame',
          frames: frames,
        ).document;
        flow.controller.restoreDocument(document);
        await tester.pump();
        await _tap(tester, find.byKey(const ValueKey('craft-share')));
        if (format != 'apng') {
          await _tap(
            tester,
            find.byKey(
              const ValueKey('craft-export-format:LitchcraftShareFormat.apng'),
            ),
          );
          await _tap(
            tester,
            find
                .text(
                  format == 'editable'
                      ? strings.craftExportEditableArtwork
                      : strings.craftGif,
                )
                .last,
          );
        }
        if (format != 'editable') {
          await _tap(
            tester,
            find.byKey(const ValueKey('craft-export-size:512')),
          );
          await _tap(tester, find.text('64 × 64').last);
          await _tap(tester, find.byKey(const ValueKey('craft-export-fps:5')));
          await _tap(
            tester,
            find.byKey(const ValueKey('craft-export-duration:0')),
          );
          await _tap(tester, find.text('1 s').last);
          await _tap(
            tester,
            find.byKey(
              const ValueKey('craft-export-loop:LitchcraftMediaLoop.once'),
            ),
          );
        }
        await _tap(tester, find.byKey(const ValueKey('craft-export-share')));
        await _until(tester, () => nativeFile.isCompleted);
        final bytes = await nativeFile.future;
        if (format == 'editable') {
          final reopened = decodeLitchcraftEditableArtwork(bytes);
          expect(sameLitchcraftDocument(reopened.document, document), isTrue);
          expect(reopened.document.frames, hasLength(frames));
          expect(reopened.document.frames.first.durationMs, 700);
        } else {
          final metadata = inspectLitchcraftMedia(bytes);
          expect(
            metadata.format,
            format == 'gif'
                ? LitchcraftMediaFormat.gif
                : LitchcraftMediaFormat.apng,
          );
          expect(metadata.width, 64);
          expect(metadata.height, 64);
          expect(metadata.frameCount, 5);
          expect(metadata.durationMs, 1000);
          expect(metadata.loopCount, 1);
        }
        expect(
          sameLitchcraftDocument(flow.library.animationDocument, document),
          isTrue,
        );
        expect(flow.library.previousDraft, isNull);
        expect(tester.takeException(), isNull);
      },
    );
  }
}
