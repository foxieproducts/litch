import 'support/memory_ota_target_store.dart';
import 'dart:async';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _UnusedBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ControlledStore extends LitchcraftArtworkStore {
  _ControlledStore(super.preferences, {required this.failFirst});
  final bool failFirst;
  final firstStarted = Completer<void>();
  final firstGate = Completer<void>();
  int calls = 0, concurrent = 0, maximumConcurrent = 0;

  @override
  Future<void> save(LitchcraftLibrarySnapshot snapshot) async {
    final call = ++calls;
    ++concurrent;
    if (concurrent > maximumConcurrent) maximumConcurrent = concurrent;
    try {
      if (call == 1) {
        firstStarted.complete();
        await firstGate.future;
        if (failFirst) throw StateError('First storage transaction failed');
      }
      await super.save(snapshot);
    } finally {
      --concurrent;
    }
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final disposeBeforeRetry in [false, true]) {
    testWidgets(
      'failed draft save retries latest artwork quietly unless disposed=$disposeBeforeRetry',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _UnusedBle(),
        );
        final controller = LitchcraftController(ble);
        final store = _ControlledStore(
          await SharedPreferences.getInstance(),
          failFirst: true,
        );
        final library = LitchcraftLibrary(controller, store: store);
        await library.ready;
        controller.paintSquares([0]);
        controller.endStroke();
        final first = library.flush();
        await tester.pump();
        expect(store.calls, 1);
        store.firstGate.complete();
        await first;
        expect(library.saveFailed, isTrue);
        if (disposeBeforeRetry) {
          library.dispose();
          await tester.pump(const Duration(seconds: 10));
          expect(store.calls, 1);
        } else {
          controller.paintSquares([63]);
          controller.endStroke();
          final latest = controller.canvas;
          await tester.pump(const Duration(milliseconds: 599));
          expect(store.calls, 1);
          await tester.pump(const Duration(milliseconds: 1));
          expect(store.calls, 2);
          expect(store.maximumConcurrent, 1);
          expect(library.saveFailed, isFalse);
          expect((await store.load()).draft!.canvas, latest);
          library.dispose();
        }
        controller.dispose();
        await ble.dispose();
      },
    );
  }
  for (final failFirst in [false, true]) {
    test(
      'draft writer coalesces newest art and metadata after blocked save (failure=$failFirst)',
      () async {
        SharedPreferences.setMockInitialValues({});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _UnusedBle(),
        );
        final controller = LitchcraftController(ble);
        final store = _ControlledStore(
          await SharedPreferences.getInstance(),
          failFirst: failFirst,
        );
        final library = LitchcraftLibrary(controller, store: store);
        await library.ready;
        controller.paintSquares([0]);
        controller.endStroke();
        final first = library.flush();
        await store.firstStarted.future;
        controller.paintSquares([63]);
        controller.endStroke();
        final latest = controller.canvas;
        final pending = <Future<void>>[
          library.rememberColor(0x123456),
          for (var repeat = 0; repeat < 20; repeat++) library.flush(),
        ];
        expect(store.calls, 1);
        store.firstGate.complete();
        await Future.wait([first, ...pending]);
        expect(store.calls, 2);
        expect(store.maximumConcurrent, 1);
        expect(library.saveFailed, isFalse);
        final persisted = await store.load();
        expect(persisted.draft!.canvas, latest);
        expect(persisted.recentColors, library.favoriteColors);
        expect(persisted.recentColors.first, 0x123456);
        await library.flush();
        expect(
          store.calls,
          2,
          reason: 'unchanged flush does not serialize or rewrite the library',
        );
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }
}
