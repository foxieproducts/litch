import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

// Only OS radio and time are controlled. Hall input, Store, protocol events,
// controller, library and screen all use their production owners.
class _FeedbackJourney {
  _FeedbackJourney(this.tester, this.native)
    : radio = VirtualLitchGattBle(native, operationIntervalMs: 20) {
    service = LitchBleService(
      ble: radio,
      gattSessionFactory: ReactiveBleGattSessionFactory(radio),
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    controller = LitchcraftController(
      service,
      monotonicMilliseconds: () => now,
    );
    library = LitchcraftLibrary(controller);
  }

  final WidgetTester tester;
  final VirtualLitchProcess native;
  final VirtualLitchGattBle radio;
  late final LitchBleService service;
  late final LitchcraftController controller;
  late final LitchcraftLibrary library;
  bool _closed = false;
  int get now => tester.binding.clock.now().millisecondsSinceEpoch;

  Future<void> tick() async {
    await tester.pump();
    if (radio.hasPendingOperations) {
      await radio.flush(tester, maxOperations: 1);
    } else {
      await radio.advance(tester, 10);
    }
  }

  Future<void> until(bool Function() done) async {
    final start = now;
    while (!done() && now - start < 10000) {
      await tick();
    }
    expect(done(), isTrue, reason: 'Production work did not settle in 10 s');
  }

  Future<void> finish(Future<dynamic> work) async {
    var completed = false;
    Object? failure;
    work.then<void>(
      (_) => completed = true,
      onError: (Object error, StackTrace _) {
        failure = error;
        completed = true;
      },
    );
    await until(() => completed);
    if (failure != null) throw failure!;
  }

  Future<void> hall(BigInt mask) async {
    await radio.command(tester, {
      'op': 'hall',
      'mask': mask.toRadixString(16).padLeft(16, '0'),
    });
  }

  Future<void> dispose() async {
    if (_closed) return;
    _closed = true;
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await finish(service.dispose());
    await radio.flush(tester);
    await tester.runAsync(radio.close);
    await tester.runAsync(native.close);
  }

  static Future<_FeedbackJourney> mount(WidgetTester tester) async {
    SharedPreferences.setMockInitialValues({});
    final native = (await tester.runAsync(
      () => VirtualLitchProcess.start(1, false, hardwareV3: true),
    ))!;
    final journey = _FeedbackJourney(tester, native);
    addTearDown(journey.dispose);
    await journey.hall(BigInt.zero);
    await journey.radio.advance(tester, 1000);
    await journey.finish(journey.service.connect('virtual-litch-production'));
    await journey.finish(journey.library.ready);
    await journey.finish(journey.controller.enter());
    await journey.finish(
      journey.controller.transferFrameStoreForDiagnostics(
        frames: journey.controller.documentSession.document.frames,
        playbackOnEntry: false,
      ),
    );
    await journey.finish(journey.controller.enter());
    await journey.finish(journey.controller.setBrush(color: 0x13579b));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(
            controller: journey.controller,
            library: journey.library,
          ),
        ),
      ),
    );
    await tester.pump();
    expect(
      journey.controller.status,
      LitchcraftStatus.live,
      reason: journey.controller.errorMessage,
    );
    return journey;
  }
}

void main() {
  testWidgets(
    'ordinary Hall paint qualifies quietly before intentional pick',
    (tester) async {
      final journey = await _FeedbackJourney.mount(tester);
      await journey.hall(BigInt.one << 27);
      await journey.radio.advance(tester, 100);
      expect(
        journey.controller.physicalColorPicking,
        isTrue,
        reason:
            'Input and shake guards still see the physical phase immediately',
      );
      expect(
        find.text('Hold the piece still to pick this square’s color.'),
        findsNothing,
        reason:
            'The first second is ordinary painting, not an intentional pick',
      );
      await journey.hall(BigInt.zero);
      await journey.radio.advance(tester, 400);
      await journey.until(() => !journey.controller.pending);
      expect(journey.controller.canvas.colors[27 * 4], 0x13579b);
      expect(journey.controller.color, 0x13579b);
      expect(journey.controller.physicalColorPicked, isFalse);
      await journey.radio.advance(tester, 700);
      expect(
        find.text('Hold the piece still to pick this square’s color.'),
        findsNothing,
        reason: 'Lift cancels the pending instruction timer',
      );

      await journey.finish(journey.controller.setBrush(color: 0xec1357));
      await journey.hall(BigInt.one << 27);
      await journey.radio.advance(tester, 1100);
      expect(
        find.text('Hold the piece still to pick this square’s color.'),
        findsOneWidget,
      );
      await journey.radio.advance(tester, 1000);
      expect(journey.controller.physicalColorPicked, isTrue);
      expect(
        find.text('Color picked. Move the piece to paint somewhere else.'),
        findsOneWidget,
      );
      await journey.hall(BigInt.zero);
      await journey.radio.advance(tester, 400);
      await journey.until(() => !journey.controller.pending);
      expect(journey.controller.color, 0x13579b);
      expect(
        journey.controller.canvas.colors[27 * 4],
        0x13579b,
        reason: 'Picking restores the artwork and selects its pre-touch color',
      );
      expect(
        find.text('Color picked. Move the piece to paint somewhere else.'),
        findsOneWidget,
      );
      await journey.dispose();
    },
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'board and app paint restart idle guidance after real sync',
    (tester) async {
      final journey = await _FeedbackJourney.mount(tester);
      await journey.radio.advance(tester, 9800);
      await journey.hall(BigInt.one << 27);
      await journey.radio.advance(tester, 50);
      await journey.hall(BigInt.zero);
      await journey.radio.advance(tester, 400);
      await journey.until(() => !journey.controller.pending);
      await tester.pump(const Duration(milliseconds: 700));
      expect(
        find.byIcon(Icons.tips_and_updates_outlined),
        findsNothing,
        reason:
            'A board stroke counts as activity while its real readback ends',
      );
      expect(journey.controller.canvas.colors[27 * 4], 0x13579b);
      final canvas = tester.getRect(find.byType(LitchcraftCanvasView));
      final pointer = await tester.startGesture(
        canvas.topLeft +
            Offset(canvas.width * 4.5 / 8, canvas.height * 3.5 / 8),
      );
      await tester.pump(const Duration(milliseconds: 50));
      await pointer.up();
      await journey.until(() => !journey.controller.pending);
      await journey.radio.advance(tester, 2000);
      expect(find.byIcon(Icons.tips_and_updates_outlined), findsNothing);
      expect(journey.controller.canvas.colors[28 * 4], 0x13579b);
      expect(journey.controller.physicalColorPicking, isFalse);
      expect(journey.controller.physicalColorPicked, isFalse);
      await journey.radio.advance(tester, 8100);
      expect(
        find.byIcon(Icons.tips_and_updates_outlined),
        findsOneWidget,
        reason: 'Optional tips remain available after a fresh idle interval',
      );
      await journey.dispose();
    },
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );
}
