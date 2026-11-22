import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

// Hold only genuine State37 packets; mode admission and all other SDK traffic
// still use the production shared session. No authority bytes are synthesized.
class _AuthorityProbeBle extends VirtualLitchGattBle {
  _AuthorityProbeBle(super.native) : super(operationIntervalMs: 8);
  bool holdAuthority = false;
  int heldAuthorities = 0;

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final decoded = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdAuthority &&
        decoded?.kind == ConnectionSessionKind.state &&
        decoded?.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      heldAuthorities++;
      held.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

List<Uint8List> _storeRequests(VirtualLitchBle ble, int from) {
  final assembler = ConnectionLitchcraftAssembler();
  final result = <Uint8List>[];
  for (final write in ble.writes.skip(from)) {
    if (write['channel'] != 'connection_session') continue;
    final packet = ConnectionSessionCodec.decode(
      Uint8List.fromList(write['bytes'] as List<int>),
    );
    if (packet?.kind != ConnectionSessionKind.object ||
        packet?.code != ConnectionLitchcraftCodec.objectOpcode) {
      continue;
    }
    final body = assembler.accept(packet!.body);
    if (body != null) result.add(body);
  }
  return result;
}

LitchcraftAnimationDocument _artwork(
  int rgb, {
  bool effect = false,
  int frames = 1,
}) => LitchcraftAnimationDocument(
  topology: LitchcraftTopology.v3,
  favoriteColors: litchcraftDefaultFavoriteColors,
  frames: List.generate(
    frames,
    (frame) => LitchcraftAnimationFrame(
      emitters: List.generate(
        256,
        (emitter) => LitchcraftAnimatedEmitter(
          color: emitter >= 36 && emitter < 40 && frame == frames - 1 ? rgb : 0,
          effect: effect && emitter == 0
              ? LitchcraftEffectStyle.breathe
              : LitchcraftEffectStyle.off,
          effectDurationMs: effect && emitter == 0 ? 2000 : 0,
        ),
      ),
    ),
  ),
);

void main() {
  for (final scenario in [
    'empty board receives restored app',
    'intentional blank versus painted board',
    'black effect remains artwork',
    'multiframe blank first frame needs choice',
    'equal blanks need no read',
    'fresh app adopts painted board',
    'physical paint during proof',
    'local paint during proof',
    'new source during proof',
    'read failure',
    'unknown authority',
    'departure during proof',
  ]) {
    testWidgets(
      'single-frame Store reconciliation: $scenario',
      (tester) async {
        const id = 'virtual-litch-production';
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        var ble = _AuthorityProbeBle(native);
        LitchBleService createService() => LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        var service = createService();
        var controller = LitchcraftController(service);
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

        Future<void> until(bool Function() done, {int budgetMs = 15000}) async {
          final start = tester.binding.clock.now();
          while (!done() &&
              tester.binding.clock.now().difference(start).inMilliseconds <
                  budgetMs) {
            await tick();
          }
          expect(
            done(),
            isTrue,
            reason:
                '$scenario: status=${controller.status} '
                'error=${controller.errorMessage}',
          );
        }

        Future<void> finish(Future<dynamic> work) async {
          var done = false;
          Object? error;
          work.then(
            (_) {
              done = true;
            },
            onError: (Object failure, StackTrace _) {
              error = failure;
              done = true;
            },
          );
          await until(() => done);
          if (error != null) throw error!;
        }

        Future<Map<String, dynamic>> stored() async =>
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        addTearDown(() async {
          ble.holdAuthority = false;
          ble.heldChannels.clear();
          ble.release('connection_session');
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await ble.command(tester, {
          'op': 'hall',
          'mask': '0000000000000000',
        }, flushOperations: false);
        await ble.advance(tester, 1000);
        await finish(service.connect(id));
        await finish(controller.enter());
        final painted =
            scenario == 'intentional blank versus painted board' ||
            scenario == 'fresh app adopts painted board';
        final boardDocument = _artwork(
          painted
              ? 0x123456
              : scenario == 'multiframe blank first frame needs choice'
              ? 0x223344
              : 0,
          effect: scenario == 'black effect remains artwork',
          frames: scenario == 'multiframe blank first frame needs choice'
              ? 2
              : 1,
        );
        await finish(controller.replaceDocument(boardDocument));
        await ble.advance(tester, 100);
        expect((await stored())['dirty'], isFalse);
        controller.dispose();
        await finish(service.dispose());
        await ble.flush(tester);
        await finish(ble.close());
        ble = _AuthorityProbeBle(native);
        service = createService();
        controller = LitchcraftController(service);
        final fresh =
            scenario == 'fresh app adopts painted board' ||
            scenario == 'equal blanks need no read';
        if (!fresh) {
          controller.restoreDocument(_artwork(painted ? 0 : 0xabcdef));
          // Use the real document owner to establish an undoable local edit.
          if (!painted) {
            controller.documentSession.paintSelectedFrame(
              squares: const [1],
              mask: 1,
              color: 0x765432,
            );
          }
        }
        var phoneBefore = controller.documentSession.document;
        final undoBefore = controller.documentSession.canUndo;
        await finish(service.connect(id));
        if (scenario == 'unknown authority') ble.holdAuthority = true;
        final from = ble.writes.length;
        List<Uint8List> requests() => _storeRequests(ble, from);
        int begins() => requests()
            .where(
              (packet) => packet[2] == LitchcraftV2Protocol.frameStoreBegin,
            )
            .length;
        int reads() => requests()
            .where(
              (packet) =>
                  packet[2] == LitchcraftV2Protocol.frameStoreSnapshot &&
                  ByteData.sublistView(packet).getUint32(13, Endian.little) ==
                      0,
            )
            .length;
        final transitions = <LitchcraftStatus>[];
        controller.addListener(() => transitions.add(controller.status));
        final entering = controller.enter();
        final interrupted =
            scenario.endsWith('during proof') || scenario == 'read failure';
        Future<void>? replacement;
        Future<void>? leaving;
        if (interrupted) {
          await until(() => reads() == 1);
          expect(begins(), 0);
          if (scenario == 'physical paint during proof') {
            await ble.command(tester, {
              'op': 'hall',
              'mask': '0000000000000200',
            }, flushOperations: false);
            await ble.advance(tester, 120);
            expect(
              (await stored())['dirty'],
              isTrue,
              reason: 'The fault crosses actual accepted physical paint',
            );
          } else if (scenario == 'local paint during proof') {
            controller.documentSession.paintSelectedFrame(
              squares: const [2],
              mask: 1,
              color: 0x1a2b3c,
            );
            phoneBefore = controller.documentSession.document;
          } else if (scenario == 'new source during proof') {
            phoneBefore = _artwork(0x442288);
            replacement = controller.replaceDocument(phoneBefore);
          } else if (scenario == 'read failure') {
            ble.failNextConnectionSessionWrite = true;
          } else {
            leaving = controller.leave();
          }
        }
        if (scenario == 'unknown authority') {
          await until(() => ble.heldAuthorities > 0 && controller.boardActive);
          expect(reads(), 0);
          expect(begins(), 0);
          expect(
            controller.pending,
            isTrue,
            reason: 'Withheld fresh authority keeps entry pending',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              phoneBefore,
            ),
            isTrue,
          );
          // Release the exact retained packets after proving that unknown
          // authority admitted neither a frame read nor an automatic upload.
          ble.holdAuthority = false;
          ble.release('connection_session');
        }
        await finish(entering);
        if (replacement != null) await finish(replacement);
        if (leaving != null) await finish(leaving);
        final automatic =
            scenario == 'empty board receives restored app' ||
            scenario == 'local paint during proof' ||
            scenario == 'new source during proof' ||
            scenario == 'unknown authority';
        if (automatic) {
          await until(
            () => !controller.boardArtworkUnresolved && !controller.pending,
          );
          expect(controller.status, LitchcraftStatus.live);
          expect(transitions, isNot(contains(LitchcraftStatus.conflict)));
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              phoneBefore,
            ),
            isTrue,
          );
          expect(begins(), 1);
          final saved = await stored();
          expect(saved['dirty'], isFalse);
          final emitters = saved['emitters'] as List<dynamic>;
          for (var i = 0; i < 256; i++) {
            expect(emitters[i], [
              phoneBefore.frames.single.emitters[i].color,
              phoneBefore.frames.single.emitters[i].effect.id,
              phoneBefore.frames.single.emitters[i].effectDurationMs,
            ]);
          }
          if (scenario != 'new source during proof') {
            expect(controller.documentSession.canUndo, undoBefore);
          }
          await ble.advance(tester, 30);
          final shown =
              (await ble.command(tester, {
                    'op': 'frame',
                  }, flushOperations: false))['frame']
                  as Map<String, dynamic>;
          expect(shown['litchcraft_active'], isTrue);
          final color = phoneBefore.frames.single.emitters[36].color;
          expect(
            (shown['board'] as List<dynamic>)[9][0],
            [(color >> 16) & 255, (color >> 8) & 255, color & 255, 0],
            reason: 'The admitted app artwork is actually visible on square9',
          );
        } else if (fresh) {
          await until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.boardArtworkUnresolved,
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              boardDocument,
            ),
            isTrue,
          );
          expect(begins(), 0);
          if (scenario == 'equal blanks need no read') expect(reads(), 0);
        } else if (leaving != null) {
          expect(begins(), 0);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              phoneBefore,
            ),
            isTrue,
          );
        } else {
          expect(
            controller.status,
            scenario == 'read failure'
                ? LitchcraftStatus.error
                : LitchcraftStatus.conflict,
          );
          expect(controller.frameStoreArtworkReadPending, isTrue);
          expect(begins(), 0);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              phoneBefore,
            ),
            isTrue,
          );
          expect(controller.documentSession.canUndo, undoBefore);
          if (scenario == 'multiframe blank first frame needs choice') {
            expect(
              reads(),
              0,
              reason: 'No animation body read to open a chooser',
            );
          } else if (scenario == 'intentional blank versus painted board' ||
              scenario == 'black effect remains artwork') {
            final beforePreview = reads();
            await finish(controller.prepareConflictBoardPreview());
            expect(controller.conflictBoardPreview, isNotNull);
            expect(
              reads(),
              beforePreview,
              reason: 'Reuse the exact verified single frame',
            );
            await finish(controller.resolveConflict(useBoard: true));
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                boardDocument,
              ),
              isTrue,
            );
            expect(
              reads(),
              beforePreview,
              reason: 'The verified single frame is the whole document',
            );
          } else if (scenario == 'read failure') {
            expect(ble.failNextConnectionSessionWrite, isFalse);
            // This faults a real shared-session write, so connection recovery
            // owns the visible error rather than a valid source choice.
            expect(controller.errorMessage, isNotNull);
          }
        }
        expect(reads(), lessThanOrEqualTo(1));
        // ignore: avoid_print
        print(
          'EMPTY_STORE_RECONCILIATION scenario=$scenario frame_reads=${reads()} '
          'Begin=${begins()} status=${controller.status.name} draft=preserved',
        );
        await finish(service.disconnect());
      },
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
