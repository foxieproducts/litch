import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'health_render_capture.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

// Only the external SDK/radio and time are controlled. Admission, Home,
// gameplay, Store and typed session acknowledgements run in production native
// owners. This test does not establish phone rendering or physical LED quality.
void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  testWidgets('occupied board keeps authored playback local until admission', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    const id = 'virtual-litch-production';
    final native = (await tester.runAsync(
      () => VirtualLitchProcess.start(1, false, hardwareV3: true),
    ))!;
    final ble = VirtualLitchGattBle(native, operationIntervalMs: 8);
    final service = LitchBleService(
      ble: ble,
      gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    final controller = LitchcraftController(service);
    final library = LitchcraftLibrary(controller);
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    Future<void> tick() async {
      await tester.pump();
      if (ble.hasPendingOperations) {
        await ble.flush(tester, maxOperations: 1);
      } else {
        await ble.command(tester, {
          'op': 'advance',
          'ms': 10,
        }, flushOperations: false);
        await tester.pump(const Duration(milliseconds: 10));
      }
    }

    Future<void> until(bool Function() done) async {
      for (var i = 0; i < 2000 && !done(); i++) {
        await tick();
      }
      expect(
        done(),
        isTrue,
        reason: '${controller.status}: ${controller.errorMessage}',
      );
    }

    Future<void> finish(Future<dynamic> work) async {
      var done = false;
      Object? failure;
      work.then<void>(
        (_) => done = true,
        onError: (Object error) {
          failure = error;
          done = true;
        },
      );
      await until(() => done);
      if (failure != null) throw failure!;
    }

    var disposed = false;
    Future<void> settle() async {
      if (disposed) return;
      disposed = true;
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await finish(service.disconnect());
    }

    addTearDown(() async {
      await settle();
      await ble.disposeJourney(tester, service);
    });

    List<ConnectionSessionPacket> mutationsSince(int start) => [
      for (final write in ble.writes.skip(start))
        if (write['channel'] == 'connection_session')
          if (ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              )
              case final packet?)
            if (packet.kind == ConnectionSessionKind.ordered ||
                packet.kind == ConnectionSessionKind.object)
              packet,
    ];
    int canvasColor() => tester
        .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
        .canvas
        .colors
        .first;

    await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
    await ble.advance(tester, 1000);
    await finish(service.connect(id));
    await finish(library.ready);
    await finish(service.openConnectionSession(id));
    expect(service.productReady, isTrue);
    final artwork = LitchcraftAnimationDocument(
      topology: LitchcraftTopology.v3,
      favoriteColors: litchcraftDefaultFavoriteColors,
      frames: List.generate(
        9,
        (index) => LitchcraftAnimationFrame(
          durationMs: index == 0
              ? 200
              : index == 1
              ? 350
              : 500,
          emitters: List.generate(
            256,
            (_) => LitchcraftAnimatedEmitter(color: (index + 1) * 0x110000),
          ),
        ),
      ),
    );
    // Establish a resident through explicit production Open before returning
    // to Home. A brand-new bridge has no stored artwork to re-adopt later.
    await finish(controller.enter());
    await finish(controller.replaceDocument(artwork));
    await until(
      () => controller.status == LitchcraftStatus.live && !controller.pending,
    );
    await finish(controller.leave());
    await ble.command(tester, {'op': 'hall', 'mask': 'FFFF00000000FFFF'});
    await ble.advance(tester, 1000);
    await finish(controller.enter());
    expect(service.isConnected, isTrue);
    expect(controller.status, LitchcraftStatus.boardNotEmpty);
    expect(controller.boardActive, isFalse);
    expect(controller.usesFrameStoreDocument, isTrue);
    final before = await ble.frame(tester);
    debugPrint(
      'DETACHED occupied mode=${before['litchcraft_active']} '
      'occupancy=${before['occupancy']} display=${before['display']} '
      'next=Play-preview; board remains in Home',
    );
    library.dismissedWelcome = true;
    Widget studio({bool active = true}) => MaterialApp(
      theme: buildLitchTheme(),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      home: Scaffold(
        body: RepaintBoundary(
          key: const ValueKey('detached-screen'),
          child: LitchcraftScreen(
            controller: controller,
            library: library,
            active: active,
          ),
        ),
      ),
    );
    await tester.pumpWidget(studio());
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(find.byKey(const ValueKey('craft-animate')));
    await tester.pump(const Duration(milliseconds: 300));
    final writes = ble.writes.length;
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 150));
    expect(canvasColor(), 0x110000);
    await tester.pump(const Duration(milliseconds: 50));
    await captureHealthReview(
      tester,
      Platform.environment['LITCH_DETACHED_CAPTURE_NAME'] ?? 'after',
      boundaryFinder: find.byKey(const ValueKey('detached-screen')),
    );
    expect(
      canvasColor(),
      0x220000,
      reason: 'Play must advance authored local frames while pieces remain',
    );
    await tester.pump(const Duration(milliseconds: 300));
    expect(canvasColor(), 0x220000);
    await tester.pump(const Duration(milliseconds: 50));
    expect(canvasColor(), 0x330000);
    expect(find.byTooltip('Pause preview on this device'), findsOneWidget);
    expect(controller.animationPlaybackPending, isFalse);
    expect(mutationsSince(writes), isEmpty);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump(const Duration(milliseconds: 100));
    expect(library.selectedFrameIndex, 2);
    expect(canvasColor(), 0x330000);
    expect(find.byTooltip('Preview on this device'), findsOneWidget);
    final after = await ble.frame(tester);
    for (final key in [
      'occupancy',
      'litchcraft_active',
      'display',
      'required_action',
      'board',
    ]) {
      expect(after[key], before[key], reason: 'local playback preserves $key');
    }
    expect(identical(library.animationDocument, artwork), isTrue);
    final localWrites = ble.writes.length;
    await tester.tap(find.byKey(const ValueKey('craft-select-frame-1')));
    await tester.pump();
    expect(library.selectedFrameIndex, 1);
    expect(canvasColor(), 0x220000);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(
      canvasColor(),
      0x220000,
      reason: 'Play begins at the selected authored frame',
    );
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await tester.pumpWidget(studio(active: false));
    await tester.pump(const Duration(milliseconds: 600));
    expect(
      find.byTooltip('Preview on this device'),
      findsOneWidget,
      reason: 'Leaving the destination retires local playback',
    );
    await tester.pumpWidget(studio());
    await tester.pump();
    expect(find.byTooltip('Preview on this device'), findsOneWidget);
    final editFrame = library.selectedFrameIndex;
    final canvas = tester.getRect(find.byType(LitchcraftCanvasView));
    await tester.tapAt(
      canvas.topLeft + Offset(canvas.width * .42, canvas.height * .42),
    );
    await tester.pump();
    expect(
      library.animationDocument.frames[editFrame],
      isNot(same(artwork.frames[editFrame])),
    );
    expect(
      library.animationDocument.frames.map((frame) => frame.durationMs),
      artwork.frames.map((frame) => frame.durationMs),
    );
    expect(
      mutationsSince(localWrites),
      isEmpty,
      reason: 'Detached selection, playback, navigation and paint stay local',
    );
    await finish(controller.toggleAnimationPlayback());
    expect(controller.status, LitchcraftStatus.boardNotEmpty);
    expect(
      controller.animationPlaybackPending,
      isFalse,
      reason:
          'Even a direct board Play cannot queue intent after refused admission',
    );

    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await finish(service.disconnect());
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await finish(service.connect(id));
    await until(() => controller.status == LitchcraftStatus.boardNotEmpty);
    expect(controller.animationPlaybackPending, isFalse);
    expect(find.byTooltip('Preview on this device'), findsOneWidget);
    debugPrint(
      'DETACHED local Play/Pause variable frames=0,1,2, selection, paint, '
      'navigation and reconnect remain local; next=remove-pieces for automatic admission',
    );

    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
    await ble.advance(tester, 1000);
    await until(
      () =>
          controller.boardActive &&
          controller.status == LitchcraftStatus.conflict &&
          !controller.pending,
    );
    // Present completed admission before reading or tapping its controls;
    // native work and the session clock have already settled above.
    await tester.pump();
    expect(
      tester
          .widget<IconButton>(find.byKey(const ValueKey('craft-preview')))
          .onPressed,
      isNull,
      reason: 'A real artwork conflict still requires an explicit choice',
    );
    expect(controller.animationPlaybackPending, isFalse);
    expect(
      find.byKey(const ValueKey('craft-resolve-conflict')),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.ensureVisible(
      find.byKey(const ValueKey('craft-use-app-artwork')),
    );
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
    await until(
      () => controller.status == LitchcraftStatus.live && !controller.pending,
    );
    await tester.pump(const Duration(milliseconds: 300));
    expect(controller.animationPlaybackPending, isFalse);
    expect(controller.animationPlaying, isFalse);
    expect(find.byTooltip('Play board'), findsOneWidget);
    final admitted = await ble.frame(tester);
    expect(admitted['litchcraft_active'], isTrue);
    debugPrint(
      'DETACHED admitted display=${admitted['display']} '
      'occupancy=${admitted['occupancy']} next=explicit-Play-on-board',
    );

    await finish(controller.leave());
    expect(
      find.byTooltip('Preview on this device'),
      findsOneWidget,
      reason: 'The preview running before admission cannot resume after Exit',
    );
    final mapping = (admitted['logical_to_physical'] as List).cast<int>();
    Future<void> hall(BigInt logical) => ble.command(tester, {
      'op': 'hall',
      'mask': List.generate(64, (square) => square)
          .where((square) => (logical & (BigInt.one << square)) != BigInt.zero)
          .fold<BigInt>(
            BigInt.zero,
            (physical, square) => physical | (BigInt.one << mapping[square]),
          )
          .toRadixString(16)
          .padLeft(16, '0'),
    });
    final opening = BigInt.parse('FFFF00000000FFFF', radix: 16);
    await hall(opening);
    await ble.advance(tester, 2500);
    final lifted = opening & ~(BigInt.one << 52);
    await hall(lifted);
    await ble.advance(tester, 30);
    await hall(lifted | (BigInt.one << 36));
    await ble.advance(tester, 2550);
    await finish(service.requestCurrentHomeStateSnapshot(id));
    final game = service.latestHomeState!;
    expect(game.position.fen, contains('4P3/8/PPPP1PPP/RNBQKBNR b'));
    await finish(controller.enter());
    expect(controller.status, LitchcraftStatus.boardNotEmpty);
    final gameWrites = ble.writes.length;
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.byTooltip('Pause preview on this device'), findsOneWidget);
    expect(mutationsSince(gameWrites), isEmpty);
    expect(service.latestHomeState!.position.fen, game.position.fen);
    expect(
      service.latestHomeState!.clockStatus?.running,
      game.clockStatus?.running,
    );
    expect(
      service.latestHomeState!.clockStatus?.paused,
      game.clockStatus?.paused,
    );
    await hall(BigInt.zero);
    await ble.advance(tester, 1000);
    expect(
      controller.boardActive,
      isFalse,
      reason: 'Empty occupancy cannot auto-admit an in-progress game',
    );
    await finish(controller.enter());
    expect(controller.status, LitchcraftStatus.gameActive);
    expect(controller.boardActive, isFalse);
    expect(controller.animationPlaybackPending, isFalse);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    expect(find.byTooltip('Preview on this device'), findsOneWidget);
    debugPrint(
      'DETACHED active-game e2-e4 preserved; empty retry rejected; '
      'next=finish-game-in-Play or continue-phone-preview',
    );
    await settle();
  });
}
