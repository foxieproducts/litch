import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'fixtures/litchcraft_v2_functional_peer.dart';

class ProbePeer extends LitchcraftV2FunctionalPeer {
  int delayMs = 0;
  int Function()? clock;
  final trace = <({int at, List<int> packet})>[];
  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (delayMs != 0 && (value[0] == 0x4c || value[0] == 0x4d)) {
      trace.add((at: clock!(), packet: List.of(value)));
      await Future<void>.delayed(Duration(milliseconds: delayMs));
    }
    await super.write(
      serviceId: serviceId,
      characteristicId: characteristicId,
      value: value,
      withResponse: withResponse,
    );
  }
}

void main() {
  for (final rtt in [20, 40]) {
    for (final spec in [
      (name: 'slow', step: 200, rounds: 1, repeat: false, pause: false),
      (name: 'typical', step: 60, rounds: 1, repeat: false, pause: false),
      (name: 'fast_long', step: 10, rounds: 4, repeat: false, pause: false),
      (name: 'repeat_reverse', step: 10, rounds: 4, repeat: true, pause: false),
      (
        name: 'slow_pause_hold',
        step: 400,
        rounds: 1,
        repeat: true,
        pause: true,
      ),
    ]) {
      for (final erase in [false, true]) {
        testWidgets('${spec.name} erase=$erase rtt=$rtt', (tester) async {
          debugDefaultTargetPlatformOverride = TargetPlatform.android;
          SharedPreferences.setMockInitialValues({});
          final peer = ProbePeer();
          final initial = LitchcraftDocumentSession(peer.document);
          while (initial.document.frames.length < 8) {
            initial.addFrame();
          }
          initial.paintSelectedFrame(
            squares: List.generate(64, (i) => i),
            mask: 15,
            color: 0x123456,
          );
          peer.document = initial.document;
          peer.selectedFrame = initial.selectedFrameIndex;
          final service = LitchcraftV2FunctionalService(peer);
          final controller = LitchcraftController(service);
          Future<void> finish(Future<void> work) async {
            var done = false;
            work.then((_) => done = true);
            for (var i = 0; !done && i < 10000; i++) {
              await tester.pump(const Duration(milliseconds: 1));
            }
            expect(done, isTrue);
          }

          await finish(controller.enter());
          await finish(controller.setBrush(eraser: erase, color: 0xabcdef));
          await tester.pumpWidget(
            MaterialApp(
              home: Center(
                child: SizedBox(
                  width: 320,
                  height: 320,
                  child: LitchcraftCanvasView(
                    canvas: controller.canvas,
                    onBegin: controller.beginStroke,
                    onPaint: controller.paintSquares,
                    onEnd: controller.endStroke,
                  ),
                ),
              ),
            ),
          );
          final origin = tester.getTopLeft(find.byType(LitchcraftCanvasView));
          final started = tester.binding.clock.now();
          int now() =>
              tester.binding.clock.now().difference(started).inMilliseconds;
          peer.clock = now;
          peer.delayMs = rtt;
          var changes = 0;
          controller.addListener(() => changes++);
          final firstSeen = <int, int>{};
          Offset pos(int square) =>
              origin +
              Offset((square % 8) * 40 + 20, (7 - square ~/ 8) * 40 + 20);
          final route = <int>[];
          for (var round = 0; round < spec.rounds; round++) {
            final squares = [
              for (var row = 0; row < 8; row++)
                for (var col = 0; col < 8; col++)
                  row * 8 + (row.isEven ? col : 7 - col),
            ];
            for (final sq in round.isEven ? squares : squares.reversed) {
              route.add(sq);
              if (spec.repeat) route.add(sq);
            }
          }
          final gesture = await tester.startGesture(pos(route.first));
          await tester.pump();
          for (var index = 1; index < route.length; index++) {
            await tester.pump(Duration(milliseconds: spec.step));
            final square = route[index];
            firstSeen.putIfAbsent(square, now);
            await gesture.moveTo(pos(square));
            await tester.pump();
            if (spec.pause && index % 16 == 0) {
              await tester.pump(const Duration(milliseconds: 1000));
            }
          }
          final upAt = now();
          await gesture.up();
          await tester.pump();
          for (var i = 0; controller.pending && i < 10000; i++) {
            await tester.pump(const Duration(milliseconds: 1));
          }
          expect(controller.pending, isFalse);
          expect(controller.errorMessage, isNull);
          expect(
            sameLitchcraftDocument(
              peer.document,
              controller.documentSession.document,
            ),
            isTrue,
          );
          final paints = peer.trace
              .where((t) => t.packet[0] == 0x4d && t.packet[2] == 3)
              .toList();
          final effects = peer.trace
              .where((t) => t.packet[0] == 0x4d && t.packet[2] == 4)
              .toList();
          final visits = paints.fold<int>(
            0,
            (n, t) => n + t.packet.length - 17,
          );
          final lengths = <String, int>{};
          for (final packet in paints) {
            lengths.update(
              '${packet.packet.length - 17}',
              (v) => v + 1,
              ifAbsent: () => 1,
            );
          }
          // Fixed fake-clock corpus: counts compare implementations, not radio latency.
          // ignore: avoid_print
          print(
            'SYNTHETIC_BATCH ${jsonEncode({'case': spec.name, 'erase': erase, 'rtt_ms': rtt, 'input_ms': upAt, 'drain_ms': now() - upAt, 'paint_commands': paints.length, 'effect_commands': effects.length, 'square_visits': visits, 'unique_squares': 64, 'chunks_by_squares': lengths, 'controller_notifications': changes, 'first_dispatch_ms': peer.trace.first.at, 'same_final_document': true})}',
          );
          await tester.pumpWidget(const SizedBox());
          controller.dispose();
          await finish(service.dispose());
          await peer.close();
          debugDefaultTargetPlatformOverride = null;
        });
      }
    }
  }
}
