import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_surprise_presets.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';

class _NoRadio implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

void main() {
  testWidgets('Rainbow256 playback builds only viewport frame controls', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final preset = LitchcraftSurprisePreset.rainbowRibbon;
    final document = LitchcraftAnimationDocument(
      frames: preset.createFrames(),
      topology: LitchcraftTopology.v3,
      favoriteColors: litchcraftDefaultFavoriteColors,
      sparkleSeed: preset.sparkleSeed,
    );
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoRadio(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    controller.restoreDocument(document);
    var disposed = false;
    Future<void> dispose() async {
      if (disposed) return;
      disposed = true;
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    }

    addTearDown(dispose);
    var thumbnailBuilds = 0;
    final previousObserver = debugOnRebuildDirtyWidget;
    debugOnRebuildDirtyWidget = (element, builtOnce) {
      previousObserver?.call(element, builtOnce);
      if (element.widget is LitchcraftFrameThumbnail) ++thumbnailBuilds;
    };
    addTearDown(() => debugOnRebuildDirtyWidget = previousObserver);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.tap(find.byKey(const ValueKey('craft-animate')));
    await tester.pump();
    final initialBuilds = thumbnailBuilds;
    final viewport = find.byKey(const ValueKey('craft-frame-scroll-viewport'));
    // The normal250 px scroll cache plus two boundary controls bounds work
    // independently of the genuine256-frame artwork's emitter content.
    final itemWidth = tester
        .getSize(find.byKey(const ValueKey('craft-select-frame-0')))
        .width;
    final budget =
        ((tester.getSize(viewport).width + 2 * 250) / itemWidth).ceil() + 2;
    final initialColors = List<int>.of(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .canvas
          .colors,
    );
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    final perTick = <int>[];
    for (var tick = 1; tick <= 3; ++tick) {
      thumbnailBuilds = 0;
      await tester.pump(const Duration(milliseconds: 50));
      perTick.add(thumbnailBuilds);
      final strip = tester.widget<LitchcraftSwipeFrameStrip>(
        find.byType(LitchcraftSwipeFrameStrip),
      );
      expect(strip.playing, isTrue);
      expect(strip.selectedFrameIndex, tick);
      expect(
        find.byKey(ValueKey('craft-select-frame-$tick')).hitTestable(),
        findsOneWidget,
      );
    }
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .canvas
          .colors,
      isNot(orderedEquals(initialColors)),
    );
    await dispose();
    debugPrint(
      'CRAFT_PREVIEW_WORK initial=$initialBuilds playback=$perTick budget=$budget',
    );
    expect(
      initialBuilds,
      lessThanOrEqualTo(budget),
      reason: 'initial=$initialBuilds playback=$perTick',
    );
    for (final builds in perTick) {
      expect(builds, lessThanOrEqualTo(budget), reason: 'playback=$perTick');
    }
  });
  for (final direction in TextDirection.values) {
    testWidgets(
      'six distinct thumbnails stay continuous through held swipe in $direction',
      (tester) async {
        final rig = _FrameStripRig(direction: direction, frameCount: 6)
          ..width = 230;
        await rig.mount(tester);
        void expectContinuous() {
          expect(
            find.byType(LitchcraftFrameThumbnail, skipOffstage: false),
            findsNWidgets(6),
          );
          final thumbs = tester
              .widgetList<LitchcraftFrameThumbnail>(
                find.byType(LitchcraftFrameThumbnail, skipOffstage: false),
              )
              .toList();
          for (var index = 0; index < 6; ++index) {
            expect(thumbs[index].frameIndex, index);
            expect(
              thumbs[index].document.frames[index].emitters.first.color,
              0x110000 * (index + 1),
            );
            expect(
              tester
                  .getSize(
                    find.byKey(
                      ValueKey('craft-select-frame-$index'),
                      skipOffstage: false,
                    ),
                  )
                  .shortestSide,
              greaterThanOrEqualTo(48),
            );
            if (index > 0) {
              expect(
                (tester
                            .getCenter(
                              find.byKey(
                                ValueKey('craft-select-frame-$index'),
                                skipOffstage: false,
                              ),
                            )
                            .dx -
                        tester
                            .getCenter(
                              find.byKey(
                                ValueKey('craft-select-frame-${index - 1}'),
                                skipOffstage: false,
                              ),
                            )
                            .dx)
                    .abs(),
                closeTo(53, .1),
              );
            }
          }
        }

        expectContinuous();
        final start = tester.getCenter(rig.viewport);
        final sign = direction == TextDirection.ltr ? -1.0 : 1.0;
        final drag = await tester.startGesture(start);
        await drag.moveTo(start + Offset(sign * 45, 0));
        await tester.pump(const Duration(milliseconds: 100));
        expectContinuous();
        await drag.moveTo(start + Offset(sign * 90, 0));
        await tester.pump(const Duration(milliseconds: 100));
        expectContinuous();
        await drag.up();
        await tester.pumpAndSettle();
        expectContinuous();
        expect(
          rig.selected,
          0,
          reason: 'Browsing never changes the artwork selection',
        );
      },
    );
    testWidgets(
      'paused first last and Undo selection stay visible in $direction',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        final session = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        );
        for (var frame = 1; frame < 32; frame++) {
          session.addFrame();
        }
        session.selectFrame(0);
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoRadio(),
        );
        final controller = LitchcraftController(ble, documentSession: session);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        var disposed = false;
        Future<void> dispose() async {
          if (disposed) return;
          disposed = true;
          await tester.pumpWidget(const SizedBox());
          library.dispose();
          controller.dispose();
          await ble.dispose();
        }

        addTearDown(dispose);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: Directionality(
                textDirection: direction,
                child: Center(
                  child: SizedBox(
                    width: 390,
                    child: ListenableBuilder(
                      listenable: Listenable.merge([controller, library]),
                      builder: (context, _) => LitchcraftAnimationBar(
                        document: library.animationDocument,
                        selectedFrameIndex: library.selectedFrameIndex,
                        playing: false,
                        enabled: true,
                        frameEditingEnabled: true,
                        motionEnabled: true,
                        playbackVisible: true,
                        expanded: true,
                        motionLabel: 'Effects',
                        playLabel: 'Play',
                        pauseLabel: 'Pause',
                        previousFrameLabel: 'First frame',
                        nextFrameLabel: 'Last frame',
                        addFrameLabel: 'Add',
                        deleteFrameLabel: 'Delete',
                        motionDisabledHint: '',
                        frameLabel: (frame, count) => 'Frame $frame of $count',
                        onOpenEffects: () {},
                        onOpenFrameDetails: () {},
                        onTogglePlayback: () {},
                        onSelectFrame: (index) =>
                            unawaited(controller.selectAnimationFrame(index)),
                        onAddFrame: library.addFrame,
                        onDeleteFrame: library.deleteSelectedFrame,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
        await tester.pump();
        void expectSelectedVisible() {
          final viewport = tester.getRect(
            find.byKey(const ValueKey('craft-frame-scroll-viewport')),
          );
          final selected = find.byKey(
            ValueKey('craft-select-frame-${library.selectedFrameIndex}'),
          );
          final rect = tester.getRect(selected);
          expect(rect.left, greaterThanOrEqualTo(viewport.left - .5));
          expect(rect.right, lessThanOrEqualTo(viewport.right + .5));
          expect(selected.hitTestable(), findsOneWidget);
        }

        await tester.tap(find.byKey(const ValueKey('craft-frame-next')));
        await tester.pump();
        expect(library.selectedFrameIndex, 31);
        expectSelectedVisible();
        await tester.tap(find.byKey(const ValueKey('craft-frame-previous')));
        await tester.pump();
        expect(library.selectedFrameIndex, 0);
        expectSelectedVisible();
        await tester.tap(find.byKey(const ValueKey('craft-frame-next')));
        await tester.pump();
        library.deleteSelectedFrame();
        await tester.pump();
        await tester.pump();
        expect(library.selectedFrameIndex, 30);
        expectSelectedVisible();
        library.undoAnimation();
        await tester.pump();
        await tester.pump();
        expect(library.selectedFrameIndex, 30);
        expectSelectedVisible();
        await tester.tap(find.byKey(const ValueKey('craft-frame-next')));
        await tester.pump();
        expect(library.selectedFrameIndex, 31);
        library.redoAnimation();
        await tester.pump();
        await tester.pump();
        expect(library.selectedFrameIndex, 30);
        expectSelectedVisible();
        await dispose();
      },
    );

    testWidgets(
      'playback centers every 50 ms tick, wrap and Pause in $direction',
      (tester) async {
        final rig = _FrameStripRig(direction: direction, playing: true);
        await rig.mount(tester);
        for (final frame in [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2]) {
          rig.update(() => rig.selected = frame);
          await rig.pumpCentered(
            tester,
            'the first presented frame centers frame $frame',
          );
          rig.expectCentered(tester, 'frame $frame begins at the strip center');
          await tester.pump(const Duration(milliseconds: 50));
          rig.expectCentered(
            tester,
            'frame $frame remains centered until next tick',
          );
        }

        rig.update(() => rig.playing = false);
        await tester.pump();
        rig.expectCentered(tester, 'Pause retains the active frame at center');
        for (var sample = 0; sample < 4; sample++) {
          await tester.pump(const Duration(milliseconds: 50));
          rig.expectCentered(
            tester,
            'a retired reveal cannot move the paused frame',
          );
        }
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets(
      'Play follows selection; a thumbnail tap pauses in place in $direction',
      (tester) async {
        final rig = _FrameStripRig(direction: direction);
        await rig.mount(tester);
        rig.update(() => rig.selected = 6);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 32));
        rig.update(() => rig.playing = true);
        await tester.pump();
        rig.expectCentered(
          tester,
          'Play retires an unfinished selection reveal',
        );
        await tester.pump(const Duration(milliseconds: 50));
        rig.expectCentered(
          tester,
          'Play remains centered before the next tick',
        );
        final beforeTap = rig.offset(tester);
        await tester.tap(rig.frame(7));
        await tester.pump();
        expect(rig.playing, isFalse);
        expect(rig.selected, 7);
        expect(
          rig.offset(tester),
          beforeTap,
          reason: 'A tap stops playback without recentering.',
        );
        for (var sample = 0; sample < 4; sample++) {
          await tester.pump(const Duration(milliseconds: 50));
          expect(
            rig.offset(tester),
            beforeTap,
            reason: 'The paused selection cannot drift afterward.',
          );
        }
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets(
      'playback locks real drags; Pause permits browsing and tap in $direction',
      (tester) async {
        final rig = _FrameStripRig(
          direction: direction,
          selected: 4,
          playing: true,
        );
        await rig.mount(tester);
        final start = tester.getCenter(rig.viewport);
        final delta = direction == TextDirection.ltr ? -53.0 : 53.0;
        final playingDrag = await tester.startGesture(start);
        for (var step = 1; step <= 3; step++) {
          await playingDrag.moveTo(start + Offset(delta * step, 0));
          await tester.pump(const Duration(milliseconds: 50));
          rig.expectCentered(
            tester,
            'playing thumbnail stays locked during drag',
          );
        }
        await playingDrag.up();
        await tester.pumpAndSettle();
        rig.expectCentered(tester, 'release does not fling the playing strip');
        expect(rig.selected, 4);

        rig.update(() => rig.playing = false);
        await tester.pump();
        final pausedDrag = await tester.startGesture(start);
        await pausedDrag.moveTo(start + Offset(delta, 0));
        await tester.pump(const Duration(milliseconds: 150));
        await pausedDrag.moveTo(start + Offset(delta * 2, 0));
        await tester.pump(const Duration(milliseconds: 200));
        await pausedDrag.up();
        await tester.pumpAndSettle();
        final browsedCenter = tester.getCenter(rig.frame(4)).dx;
        expect(
          (browsedCenter - tester.getCenter(rig.viewport).dx).abs(),
          greaterThan(30),
          reason: 'paused swipe browses without selecting a frame',
        );
        expect(rig.selected, 4);
        rig.update(() {});
        await tester.pump();
        expect(tester.getCenter(rig.frame(4)).dx, closeTo(browsedCenter, .5));

        final next = rig.frame(6);
        expect(next.hitTestable(), findsOneWidget);
        await tester.tap(next);
        await tester.pump();
        expect(rig.selected, 6);
        expect(rig.playing, isFalse);
        expect(
          tester.getCenter(rig.frame(4)).dx,
          closeTo(browsedCenter, .5),
          reason: 'A paused thumbnail tap preserves the browsed position.',
        );
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets(
      'playback follows metrics; paused navigation reveals selection in $direction',
      (tester) async {
        final rig = _FrameStripRig(
          direction: direction,
          selected: 9,
          playing: true,
        );
        await rig.mount(tester);
        for (final width in [250.0, 500.0, 700.0, 300.0]) {
          rig.update(() => rig.width = width);
          if (width == 700) {
            await tester.pump();
            await tester.pump();
          } else {
            await rig.pumpCentered(
              tester,
              'first render follows viewport $width',
            );
            rig.expectCentered(
              tester,
              'playing center follows viewport $width',
            );
            await tester.pump(const Duration(milliseconds: 50));
            rig.expectCentered(tester, 'metrics cannot animate off center');
          }
        }
        final beforePause = rig.offset(tester);
        rig.update(() {
          rig.playing = false;
          rig.width = 380;
          rig.frameCount = 16;
        });
        await tester.pump();
        await tester.pump();
        expect(
          rig.offset(tester),
          beforePause,
          reason: 'Pause and document growth preserve the viewport.',
        );
        rig.update(() => rig.selected = 15);
        await tester.pump();
        rig.expectVisible(tester);
        rig.update(() => rig.selected = 0);
        await tester.pump();
        rig.expectVisible(tester);
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets(
      'paused held drag survives async metrics and document changes in $direction',
      (tester) async {
        final rig = _FrameStripRig(direction: direction, selected: 4);
        await rig.mount(tester);
        rig.expectVisible(tester);
        final start = tester.getCenter(rig.viewport);
        final delta = direction == TextDirection.ltr ? -1.0 : 1.0;
        final drag = await tester.startGesture(start);
        await drag.moveTo(start + Offset(delta * 40, 0));
        await tester.pump(const Duration(milliseconds: 50));
        await drag.moveTo(start + Offset(delta * 85, 0));
        await tester.pump(const Duration(milliseconds: 50));
        final held = rig.offset(tester);
        expect(held, greaterThan(30));
        Future<void>.microtask(
          () => rig.update(() {
            rig.width = 430;
            rig.frameCount = 16;
            rig.selected = 1;
          }),
        );
        await tester.pump();
        await tester.pump();
        expect(
          rig.offset(tester),
          held,
          reason:
              'Async resize/growth/selection cannot jump or cancel the drag.',
        );
        await drag.moveTo(start + Offset(delta * 140, 0));
        await tester.pump(const Duration(milliseconds: 50));
        expect(rig.offset(tester), closeTo(held + 55, .01));
        await tester.pump(const Duration(milliseconds: 200));
        await drag.up();
        await tester.pumpAndSettle();
        final browsed = rig.offset(tester);
        rig.update(() => rig.frameCount = 3);
        await tester.pump();
        await tester.pump();
        expect(
          rig.offset(tester),
          0,
          reason: 'A fitting strip has legal zero extent and no scroll.',
        );
        expect(browsed, greaterThan(0));
        rig.update(() {
          rig.frameCount = 16;
          rig.selected = 15;
          rig.playing = true;
        });
        await tester.pump();
        await tester.pump();
        rig.expectCentered(tester, 'Play restores auto-follow after browsing.');
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets('fit-all strip stays fully visible in $direction', (
      tester,
    ) async {
      final semantics = tester.ensureSemantics();
      final rig = _FrameStripRig(
        direction: direction,
        frameCount: 3,
        selected: 2,
        playing: true,
      );
      await rig.mount(tester);
      void expectAllVisible() {
        final viewport = tester.getRect(rig.viewport);
        for (var frame = 0; frame < rig.frameCount; frame++) {
          final target = tester.getRect(rig.frame(frame));
          expect(target.left, greaterThanOrEqualTo(viewport.left));
          expect(target.right, lessThanOrEqualTo(viewport.right));
          expect(target.shortestSide, greaterThanOrEqualTo(48));
          expect(rig.frame(frame).hitTestable(), findsOneWidget);
        }
        final scroll = tester.state<ScrollableState>(
          find.descendant(of: rig.viewport, matching: find.byType(Scrollable)),
        );
        expect(scroll.position.maxScrollExtent, 0);
      }

      expectAllVisible();
      expect(
        tester.getSemantics(rig.frame(2)),
        matchesSemantics(
          label: 'Playing frame 3',
          isButton: true,
          isEnabled: true,
          hasEnabledState: true,
          isSelected: false,
          hasSelectedState: true,
          hasTapAction: true,
          hasLongPressAction: true,
        ),
      );
      rig.update(() {
        rig.frameCount = 12;
        rig.selected = 5;
      });
      await rig.pumpCentered(
        tester,
        'first render after growth centers playback',
      );
      rig.expectCentered(tester, 'overflow restores centered playback');
      rig.update(() {
        rig.frameCount = 3;
        rig.selected = 1;
        rig.playing = false;
      });
      await tester.pump();
      await tester.pump();
      expectAllVisible();
      expect(tester.takeException(), isNull);
      semantics.dispose();
    });
  }
}

class _FrameStripRig {
  _FrameStripRig({
    required this.direction,
    this.frameCount = 12,
    this.selected = 0,
    this.playing = false,
  });

  final TextDirection direction;
  int frameCount;
  int selected;
  bool playing;
  double width = 380;
  late StateSetter update;

  Finder get viewport =>
      find.byKey(const ValueKey('craft-frame-scroll-viewport'));
  Finder frame(int index) => find.byKey(ValueKey('craft-select-frame-$index'));

  double offset(WidgetTester tester) => tester
      .state<ScrollableState>(
        find.descendant(of: viewport, matching: find.byType(Scrollable)),
      )
      .position
      .pixels;

  void expectCentered(WidgetTester tester, String reason) {
    expect(
      tester.getCenter(frame(selected)).dx,
      closeTo(tester.getCenter(viewport).dx, .5),
      reason: reason,
    );
  }

  void expectVisible(WidgetTester tester) {
    final bounds = tester.getRect(viewport);
    final selected = tester.getRect(frame(this.selected));
    expect(selected.left, greaterThanOrEqualTo(bounds.left - .5));
    expect(selected.right, lessThanOrEqualTo(bounds.right + .5));
    expect(frame(this.selected).hitTestable(), findsOneWidget);
  }

  Future<void> pumpCentered(WidgetTester tester, String reason) async {
    double? presentedCenter;
    double? viewportCenter;
    // Register before rebuilding so this observes the geometry that was
    // painted, before the strip can correct its offset in a post-frame callback.
    tester.binding.addPostFrameCallback((_) {
      presentedCenter = tester.getCenter(frame(selected)).dx;
      viewportCenter = tester.getCenter(viewport).dx;
    });
    await tester.pump();
    expect(presentedCenter, closeTo(viewportCenter!, .5), reason: reason);
  }

  Future<void> mount(WidgetTester tester) async {
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: Directionality(
            textDirection: direction,
            child: Center(
              child: StatefulBuilder(
                builder: (context, setState) {
                  update = setState;
                  return SizedBox(
                    width: width,
                    child: LitchcraftSwipeFrameStrip(
                      document: LitchcraftAnimationDocument(
                        topology: LitchcraftTopology.v3,
                        favoriteColors: litchcraftDefaultFavoriteColors,
                        frames: [
                          for (var index = 0; index < frameCount; index++)
                            LitchcraftAnimationFrame(
                              durationMs: 50,
                              emitters: List.generate(
                                256,
                                (_) => LitchcraftAnimatedEmitter(
                                  color: 0x110000 * (index % 8 + 1),
                                ),
                              ),
                            ),
                        ],
                      ),
                      selectedFrameIndex: selected,
                      playing: playing,
                      enabled: true,
                      frameLabel: (frame, count) => 'Frame $frame of $count',
                      onSelectFrame: (index) => setState(() {
                        selected = index;
                        playing = false;
                      }),
                      onAddFrame: () {},
                      onFrameLongPress: (_, _) {},
                    ),
                  );
                },
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
  }
}
