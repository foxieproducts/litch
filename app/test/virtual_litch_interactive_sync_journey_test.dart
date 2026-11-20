import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_bouncing_balls_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_rickroll_preset.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_frame_store.dart';
import 'package:litch_mobile/src/services/connection_game_history_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/litchcraft_packet_observers.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';
import 'package:litch_mobile/src/models/litchcraft_surprise_presets.dart';

const _boardId = 'virtual-litch-production';

LitchcraftArtwork _surpriseArtwork(LitchcraftAnimationDocument document) =>
    LitchcraftArtwork.animated(
      id: 'surprise-journey',
      name: 'Artwork',
      document: document,
      createdAt: DateTime.utc(2026),
      modifiedAt: DateTime.utc(2026),
    );

void _registerSurpriseArtworkJourneys() {
  for (final (preset, editFrames) in [
    for (final preset in LitchcraftSurprisePreset.values) (preset, false),
    (LitchcraftSurprisePreset.pocketSunrise, true),
    (LitchcraftSurprisePreset.rainbowRibbon, true),
  ]) {
    testWidgets(
      editFrames
          ? 'About surprises: ${preset.frameCount}-frame structural edits preserve selected frame'
          : 'About surprise ${preset.taps}: import play switch exit reconnect',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(
          service,
          monotonicMilliseconds: () => journey.now,
        );
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        await journey.finish(library.loadVersionArtwork(preset.taps));
        final original = library.animationDocument;
        expect(original.frames, hasLength(preset.frameCount));
        expect(controller.animationPlaying, isFalse);
        if (original.frames.length == 256) {
          await expectLater(
            controller.addAnimationFrame(),
            throwsA(isA<LitchcraftFrameLimit>()),
          );
          expect(library.animationDocument.frames, hasLength(256));
        }
        final imported = LitchcraftArtwork.fromJson(
          jsonDecode(jsonEncode(_surpriseArtwork(original).toJson())),
        );
        await journey.finish(library.open(imported));
        await _expectCleanStoreParity(journey, controller);
        final last = original.frames.length - 1;
        await journey.finish(controller.selectAnimationFrame(last));
        await journey.finish(controller.toggleAnimationPlayback());
        final playingStart = await journey.frame();
        final startMs = playingStart['now_ms'] as int;
        for (final delta in [
          original.frames.last.durationMs - 1,
          1,
          original.frames.first.durationMs,
        ]) {
          await ble.advance(tester, delta);
          final frame = await journey.frame();
          final elapsed = (frame['now_ms'] as int) - startMs;
          final metadata =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map;
          expect(metadata['playing'], isTrue);
          expect(
            metadata['selected_frame'],
            litchcraftAnimationFrameIndexAt(
              original,
              litchcraftAnimationFrameStartOrNull(original, last)! + elapsed,
            ),
          );
        }
        await journey.finish(controller.toggleAnimationPlayback());
        final paused = controller.animationSelectedFrameIndex;
        await ble.advance(tester, 1100);
        expect(controller.animationSelectedFrameIndex, paused);
        expect(controller.animationPlaying, isFalse);
        if (editFrames) {
          // Real AddCopy/Delete at first, middle and last. Replace/move use the
          // existing whole-document Open seam: there is no Store move UI/API.
          for (final position in [
            0,
            original.frames.length ~/ 2,
            original.frames.length - 1,
          ]) {
            if (library.animationDocument.frames.length == 256) {
              await journey.finish(controller.selectAnimationFrame(255));
              await journey.finish(controller.deleteAnimationFrame());
            }
            final sourceIndex = position.clamp(
              0,
              library.animationDocument.frames.length - 1,
            );
            await journey.finish(controller.selectAnimationFrame(sourceIndex));
            final source = library.animationDocument.frames[sourceIndex];
            final before = library.animationDocument.frames.length;
            bool? added;
            await journey.finish(
              controller.addAnimationFrame().then(
                (accepted) => added = accepted,
              ),
            );
            expect(added, isTrue);
            await ble.advance(tester, 16);
            expect(library.animationDocument.frames, hasLength(before + 1));
            expect(library.selectedFrameIndex, sourceIndex + 1);
            expect(
              library.animationDocument.frames[sourceIndex + 1].durationMs,
              source.durationMs,
            );
            expect(
              library.animationDocument.frames[sourceIndex + 1].emitters.map(
                (e) => [e.color, e.effect.id, e.effectDurationMs],
              ),
              source.emitters.map(
                (e) => [e.color, e.effect.id, e.effectDurationMs],
              ),
            );
            bool? durationApplied, effectApplied;
            await journey.finish(
              controller
                  .setAnimationFrameDuration(137 + sourceIndex)
                  .then((value) => durationApplied = value),
            );
            expect(
              durationApplied,
              isTrue,
              reason: 'Duration must apply to the newly copied frame',
            );
            await journey.finish(
              controller
                  .applyAnimationEffect(
                    square: sourceIndex % 64,
                    mask: 15,
                    effect: LitchcraftEffectStyle.orbitCounterclockwise,
                    effectDurationMs: 1700,
                  )
                  .then((value) => effectApplied = value),
            );
            expect(effectApplied, isTrue);
            final edited = library.animationDocument.frames[sourceIndex + 1];
            expect(edited.durationMs, 137 + sourceIndex);
            expect(
              edited.emitters[sourceIndex % 64 * 4].effect,
              LitchcraftEffectStyle.orbitCounterclockwise,
            );
            await _expectCleanStoreParity(journey, controller);
            await journey.finish(controller.selectAnimationFrame(sourceIndex));
            bool? deleted;
            await journey.finish(
              controller.deleteAnimationFrame().then(
                (value) => deleted = value,
              ),
            );
            expect(deleted, isTrue);
            expect(
              library.selectedFrameIndex,
              sourceIndex == 0 ? 0 : sourceIndex - 1,
            );
            await _expectCleanStoreParity(journey, controller);
          }
          // Whole-document Open covers insert-before-first, replacement and
          // all three move positions; the live Store API only offers AddCopy/Delete.
          var frames = library.animationDocument.frames.toList();
          for (var step = 0; step < 3; step++) {
            final index = [0, frames.length ~/ 2, frames.length - 1][step];
            final moved = frames.removeAt(index);
            frames.insert([frames.length ~/ 2, frames.length, 0][step], moved);
            final emitters = frames[index].emitters.toList();
            emitters[index % 256] = LitchcraftAnimatedEmitter(
              color: 0xffffff,
              effect: LitchcraftEffectStyle.bounce,
              effectDurationMs: 10000,
            );
            frames[index] = LitchcraftAnimationFrame(
              durationMs: 251 + step,
              emitters: emitters,
            );
            if (step == 0 && frames.length < 256) frames.insert(0, moved);
            final replacement = LitchcraftAnimationDocument(
              frames: frames,
              topology: original.topology,
              favoriteColors: original.favoriteColors,
              sparkleSeed: original.sparkleSeed,
            );
            final portable = LitchcraftArtwork.fromJson(
              _surpriseArtwork(replacement).toJson(),
            );
            expect(
              sameLitchcraftDocument(portable.document, replacement),
              isTrue,
            );
            await journey.finish(library.open(portable));
            expect(library.selectedFrameIndex, 0);
            expect(controller.animationPlaying, isFalse);
            await _expectCleanStoreParity(journey, controller);
            frames = library.animationDocument.frames.toList();
          }
        }
        await journey.finish(
          library.loadVersionArtwork(preset.taps == 1 ? 2 : 1),
        );
        await journey.finish(library.open(imported));
        expect(
          sameLitchcraftDocument(library.animationDocument, original),
          isTrue,
        );
        await journey.finish(controller.leave());
        final exited = await journey.frame();
        expect(exited['litchcraft_active'], isFalse);
        await journey.finish(controller.enter());
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(service.disconnect());
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.enter());
        await _expectCleanStoreParity(journey, controller);
        journey.checkpoint(
          'surprise-${preset.taps}-reconnected',
          await journey.frame(),
          'frames=${preset.frameCount} accepted=true paused=true next=Play-or-edit; '
              'import/load/wrap/switch/Exit/re-entry/reconnect exact; structural=$editFrames',
        );
        await journey.finish(service.disconnect());
      },
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'About surprises: maximum values and dense information survive bounded mixed edits',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      // Use the same maximum-size artwork as the protocol test. Each frame changes every emitter.
      final fixturePath = Platform.environment['LITCH_MAX_ARTWORK_FIXTURE'];
      final dense = fixturePath == null
          ? LitchcraftAnimationDocument(
              topology: LitchcraftTopology.v3,
              favoriteColors: litchcraftDefaultFavoriteColors,
              sparkleSeed: 7,
              playbackOnEntry: true,
              frames: List.generate(
                256,
                (seed) => LitchcraftAnimationFrame(
                  durationMs: 50 + seed,
                  emitters: List.generate(
                    256,
                    (index) => LitchcraftAnimatedEmitter(
                      color:
                          (((seed + index * 11) & 255) << 16) |
                          (((seed + index * 17) & 255) << 8) |
                          ((seed + index * 23) & 255),
                    ),
                  ),
                ),
              ),
            )
          : LitchcraftArtwork.fromJson(
              jsonDecode(File(fixturePath).readAsStringSync()),
            ).document;
      LitchcraftFrameStoreTransfer prepare(
        LitchcraftAnimationDocument document,
        bool rgb,
      ) => LitchcraftFrameStoreProtocol.prepare(
        session: 1,
        transfer: 1,
        expectedGeneration: 0,
        enableRgbWire: rgb,
        topology: 3,
        active: true,
        playbackOnEntry: document.playbackOnEntry,
        sparkleSeed: document.sparkleSeed,
        favoriteColors: document.favoriteColors,
        frames: document.frames,
      );
      expect(prepare(dense, false).bodyBytes, 289024);
      expect(prepare(dense, true).bodyBytes, 198912);
      await journey.finish(library.open(_surpriseArtwork(dense)));
      await _expectCleanStoreParity(journey, controller);
      final rich = LitchcraftAnimationDocument(
        topology: dense.topology,
        favoriteColors: dense.favoriteColors,
        sparkleSeed: 7,
        frames: [
          for (var frame = 0; frame < dense.frames.length; frame++)
            LitchcraftAnimationFrame(
              durationMs: dense.frames[frame].durationMs,
              emitters: [
                for (var index = 0; index < 256; index++)
                  LitchcraftAnimatedEmitter(
                    color: dense.frames[frame].emitters[index].color,
                    effect:
                        LitchcraftEffectStyle.values[1 + (index + frame) % 8],
                    effectDurationMs: 500 + (index * 7 + frame * 11) % 96 * 100,
                  ),
              ],
            ),
        ],
      );
      expect(
        prepare(rich, true).bodyBytes,
        289024,
        reason:
            'Effects prevent RGB-only compression; all 256 records reach the legal wire ceiling',
      );
      final richJson = jsonEncode(_surpriseArtwork(rich).toJson());
      final richImported = LitchcraftArtwork.fromJson(jsonDecode(richJson));
      expect(sameLitchcraftDocument(richImported.document, rich), isTrue);
      await journey.finish(library.open(richImported));
      await _expectCleanStoreParity(journey, controller);
      debugPrint(
        'SURPRISE_RICH canonical=289024B negotiatedWire=289024B json=${utf8.encode(richJson).length}B',
      );
      // Maximum RGB/effect duration on every square, all eight supported effects.
      // This is a value-boundary case; compression means it is not max information.
      final maximum = LitchcraftAnimationDocument(
        topology: dense.topology,
        favoriteColors: dense.favoriteColors,
        sparkleSeed: 0xffffffff,
        frames: List.generate(
          32,
          (frame) => LitchcraftAnimationFrame(
            durationMs: frame == 0 ? 5000 : 50 + frame,
            emitters: List.generate(
              256,
              (index) => LitchcraftAnimatedEmitter(
                color: 0xffffff,
                effect:
                    LitchcraftEffectStyle.values[1 + (index ~/ 4 + frame) % 8],
                effectDurationMs: 10000,
              ),
            ),
          ),
        ),
      );
      await journey.finish(library.open(_surpriseArtwork(maximum)));
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(controller.toggleAnimationPlayback());
      for (final ms in [499, 1, 1999, 1, 2500, 4999, 1]) {
        await ble.advance(tester, ms);
        final frame = await journey.frame();
        expect(
          (frame['physical'] as List).any(
            (pixel) => (pixel as List).take(3).any((v) => v != 0),
          ),
          isTrue,
        );
      }
      await journey.finish(controller.toggleAnimationPlayback());
      var seed = 0x4c495443;
      // Exactly 24 mixed command bursts. No await between deterministic local
      // selection and following edit; every burst must settle before the next.
      for (var step = 0; step < 24; step++) {
        seed = (seed * 1664525 + 1013904223) & 0xffffffff;
        final index = seed % library.animationDocument.frames.length;
        final selection = controller.selectAnimationFrame(index);
        final operation = switch (step % 4) {
          0 => controller.addAnimationFrame(),
          1 => controller.setAnimationFrameDuration(50 + seed % 4951),
          2 => controller.applyAnimationEffect(
            square: seed % 64,
            mask: 15,
            effect: LitchcraftEffectStyle.values[1 + seed % 8],
            effectDurationMs: 10000,
          ),
          _ => controller.deleteAnimationFrame(),
        };
        bool? applied;
        await journey.finish(
          Future.wait([selection, operation.then((value) => applied = value)]),
        );
        expect(
          applied,
          isTrue,
          reason:
              'Mixed operation $step must be applied, not silently rejected',
        );
        expect(controller.pending, isFalse);
        expect(controller.animationPlaying, isFalse);
      }
      await _expectCleanStoreParity(journey, controller);
      final roundTrip = LitchcraftArtwork.fromJson(
        _surpriseArtwork(library.animationDocument).toJson(),
      );
      expect(
        sameLitchcraftDocument(roundTrip.document, library.animationDocument),
        isTrue,
      );
      debugPrint(
        'SURPRISE_MAX canonical=289024B RGBwire=198912B fixedSlots=${256 * 1097}B '
        'maximumValuesWire=${prepare(maximum, true).bodyBytes}B seed=0x4c495443 bursts=24',
      );
      journey.checkpoint(
        'surprise-max-mixed',
        await journey.frame(),
        'accepted=true exact-durable=true next=Play-or-edit',
      );
      await journey.finish(service.disconnect());
    },
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );
}

final class _StoreDeliveryProbe implements AppTraceSink {
  final deliveries = <Map<String, Object?>>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'litchcraft.frame_store_delivery') {
      deliveries.add(Map<String, Object?>.of(fields));
    }
  }
}

final class _StoreHistoryProbe implements AppTraceSink {
  _StoreHistoryProbe({this.clock});
  final int Function()? clock;
  final events = <({String event, Map<String, Object?> fields})>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event.startsWith('litchcraft.store_history_')) {
      events.add((event: event, fields: Map<String, Object?>.of(fields)));
      debugPrint(
        'RR_HISTORY $event ${jsonEncode(<String, Object?>{if (clock != null) 't_ms': clock!(), ...fields})}',
      );
    }
  }
}

final class _SyncRecoveryTrace implements AppTraceSink {
  _SyncRecoveryTrace(this.clock);
  final int Function() clock;
  final events = <Map<String, Object?>>[];
  VoidCallback? onSessionRetired;
  VoidCallback? onStoreInspectStarted;
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event.startsWith('ble.connection') ||
        event == 'litchcraft.state' ||
        event == 'litchcraft.transport_failed' ||
        event == 'litchcraft.recovery_scheduled' ||
        event == 'litchcraft.store_entry_phase' ||
        event == 'litchcraft.frame_store_delivery' ||
        event == 'litchcraft.interrupted_store_replacement_resumed') {
      final row = {'event': event, 't_ms': clock(), ...fields};
      events.add(row);
      debugPrint('RR_SYNC_RECOVERY ${jsonEncode(row)}');
      if (event == 'ble.connection_session_retired') {
        final callback = onSessionRetired;
        onSessionRetired = null;
        callback?.call();
      }
      if (event == 'litchcraft.store_entry_phase' &&
          fields['phase'] == 'inspect' &&
          fields['stage'] == 'start') {
        final callback = onStoreInspectStarted;
        onStoreInspectStarted = null;
        callback?.call();
      }
    }
  }
}

// Withhold only genuine clean Store authority at the SDK callback seam.
// Native Store metadata, Progress, admission and connection remain unchanged.
class _DelayedStoreAuthorityBle extends VirtualLitchBle {
  _DelayedStoreAuthorityBle(super.native, {super.operationIntervalMs});
  bool holdStoreAuthority = false;
  final heldStoreAuthority = <VirtualPacket>[];

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final observed = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdStoreAuthority &&
        observed?.kind == ConnectionSessionKind.state &&
        observed?.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      heldStoreAuthority.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Fail the real phone persistence owner without changing Open/delivery behavior.
class _OpenFeedbackDraftStore extends LitchcraftArtworkStore {
  _OpenFeedbackDraftStore(super.preferences);
  bool failSave = false;
  Completer<void>? holdNewDocumentSave;
  int heldSaves = 0, completedHeldSaves = 0;
  final failure = StateError('controlled phone draft save failure');
  @override
  Future<void> save(LitchcraftLibrarySnapshot snapshot) async {
    final held = snapshot.draft?.frameCount == 32 ? holdNewDocumentSave : null;
    if (held != null) {
      ++heldSaves;
      await held.future;
    }
    if (failSave) throw failure;
    await super.save(snapshot);
    if (held != null) ++completedHeldSaves;
  }
}

// Delay genuine Store metadata at the same SDK callback seam while ordinary
// connection health remains available. Reuse the SDK fixture's retained list.
class _DelayedOpenMetadataBle extends VirtualLitchBle {
  _DelayedOpenMetadataBle(super.native, {super.operationIntervalMs});
  bool holdStoreMetadata = false;
  bool holdStoreResidents = false;
  bool holdModeMetadata = false;
  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final envelope = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    final body = envelope?.body;
    final reply =
        envelope?.kind == ConnectionSessionKind.object &&
            envelope?.code == ConnectionLitchcraftCodec.receiptCode &&
            body != null &&
            body.length > 4 &&
            body[1] ==
                (ConnectionLitchcraftCodec.beginFlag |
                    ConnectionLitchcraftCodec.endFlag)
        ? LitchcraftV2Packet.decode(body.sublist(4))
        : null;
    if ((holdStoreMetadata &&
            reply?.event == LitchcraftV2Protocol.frameStoreState) ||
        (holdStoreResidents &&
            reply?.event == LitchcraftV2Protocol.frameStoreResidentsState) ||
        (holdModeMetadata && reply?.event == LitchcraftV2Protocol.state)) {
      held.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Cold routing may use Mode Inspect while clean Store authority is still in
// flight. Withhold real State37 SDK callbacks, never a controller answer.
class _ColdAttachGattBle extends VirtualLitchGattBle {
  _ColdAttachGattBle(super.native, {super.operationIntervalMs});
  bool holdStoreAuthority = false;
  final heldStoreAuthority = <VirtualPacket>[];
  final observedModeStates = <LitchcraftV2Packet>[];
  final _modePackets = ConnectionLitchcraftAssembler();

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final observed = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (observed?.kind == ConnectionSessionKind.object &&
        observed?.code == ConnectionLitchcraftCodec.objectOpcode) {
      final bytes = _modePackets.accept(observed!.body);
      final state = bytes == null ? null : LitchcraftV2Packet.decode(bytes);
      if (state?.event == LitchcraftV2Protocol.state) {
        observedModeStates.add(state!);
      }
    }
    if (holdStoreAuthority &&
        observed?.kind == ConnectionSessionKind.state &&
        observed?.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      heldStoreAuthority.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }

  void releaseStoreAuthority() {
    holdStoreAuthority = false;
    final pending = List<VirtualPacket>.of(heldStoreAuthority);
    heldStoreAuthority.clear();
    for (final packet in pending) {
      deliver(packet);
    }
  }
}

// The existing bridge does not expose the OTA-control capability probe. Supply
// its fixed V3 profile only; settings, commands and snapshots remain production.
class _BridgeBoard extends LitchBleService {
  _BridgeBoard(
    VirtualLitchBle ble, {
    this.motionCapable = false,
    super.appTrace,
  }) : super(pendingOtaStore: MemoryOtaTargetStore(), ble: ble);
  final bool motionCapable;

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask:
            (1 << 7) |
            (1 << 10) |
            (1 << 11) |
            (1 << 12) |
            (1 << 13) |
            (1 << 14) |
            (motionCapable ? (1 << 15) : 0),
        explicitlyReported: true,
      );
}

// Hold one genuine Play submission before native admission. Every released
// byte and all notifications still pass through the existing production owners.
class _DelayedPlayAdmissionBle extends VirtualLitchBle {
  _DelayedPlayAdmissionBle(super.native);
  bool holdNextPlay = false;
  bool playHeld = false;
  final _playGate = Completer<void>();

  Future<void> _beforeWrite(List<int> value) async {
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value));
    if (holdNextPlay &&
        packet?.kind == ConnectionSessionKind.ordered &&
        packet?.code == ConnectionSession.litchcraftTimelineOpcode &&
        packet!.body.isNotEmpty &&
        packet.body[0] == 1) {
      holdNextPlay = false;
      playHeld = true;
      await _playGate.future;
      playHeld = false;
    }
  }

  void releasePlay() {
    if (!_playGate.isCompleted) _playGate.complete();
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    await _beforeWrite(value);
    await super.writeCharacteristicWithResponse(characteristic, value: value);
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    await _beforeWrite(value);
    await super.writeCharacteristicWithoutResponse(
      characteristic,
      value: value,
    );
  }
}

// Delay only the production clock notifications at the existing SDK seam. State
// still completes Inspect first; retained Objects are released in native order.
class _DelayedEffectClockBle extends VirtualLitchBle {
  _DelayedEffectClockBle(super.native);

  bool holdEffectClocks = false;
  final seenEffectClocks = <VirtualPacket>[];
  final heldEffectClocks = <VirtualPacket>[];

  static LitchcraftV2Packet? clockPacket(VirtualPacket packet) {
    if (packet.channel != 'connection_session') return null;
    final envelope = ConnectionSessionCodec.decode(
      Uint8List.fromList(packet.bytes),
    );
    if (envelope?.kind != ConnectionSessionKind.object ||
        envelope?.code != ConnectionLitchcraftCodec.receiptCode ||
        envelope!.body.length < 4 ||
        envelope.body[1] !=
            ConnectionLitchcraftCodec.beginFlag |
                ConnectionLitchcraftCodec.endFlag) {
      return null;
    }
    final domain = LitchcraftV2Packet.decode(envelope.body.sublist(4));
    return domain?.event == LitchcraftV2Protocol.effectClockLow ||
            domain?.event == LitchcraftV2Protocol.effectClockHigh
        ? domain
        : null;
  }

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (clockPacket(packet) != null) {
      seenEffectClocks.add(packet);
      if (holdEffectClocks) {
        heldEffectClocks.add(packet);
        return;
      }
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Hold a real Play receipt after native admission without withholding State.
class _DelayedTimelineCheckpointBle extends VirtualLitchBle {
  _DelayedTimelineCheckpointBle(super.native, {super.operationIntervalMs});
  bool holdPlayCheckpoint = false;
  int timelineWritesFrom = 0;
  final heldPlayCheckpoints = <VirtualPacket>[];

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final observed = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdPlayCheckpoint &&
        observed?.kind == ConnectionSessionKind.control &&
        observed?.code == ConnectionSessionControl.checkpoint &&
        writes.skip(timelineWritesFrom).any((write) {
          if (write['channel'] != 'connection_session') return false;
          final request = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return request?.kind == ConnectionSessionKind.ordered &&
              request?.code == ConnectionSession.litchcraftTimelineOpcode &&
              request!.body[0] == 1 &&
              request.token == observed?.token &&
              request.sequence <= observed!.sequence;
        })) {
      heldPlayCheckpoints.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Delay one genuine history checkpoint while native State and inverse records
// remain deliverable. This exercises fast mounted taps ahead of their receipt.
class _DelayedHistoryCheckpointBle extends VirtualLitchBle {
  _DelayedHistoryCheckpointBle(super.native, {super.operationIntervalMs});

  bool holdHistoryCheckpoints = false;
  int historyWritesFrom = 0;
  final heldHistoryCheckpoints = <VirtualPacket>[];

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (holdHistoryCheckpoints && packet.channel == 'connection_session') {
      final observed = ConnectionSessionCodec.decode(
        Uint8List.fromList(packet.bytes),
      );
      if (observed?.kind == ConnectionSessionKind.control &&
          observed?.code == ConnectionSessionControl.checkpoint) {
        final hasHistory = writes.skip(historyWritesFrom).any((write) {
          if (write['channel'] != 'connection_session') return false;
          final request = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return request?.kind == ConnectionSessionKind.ordered &&
              request?.code == ConnectionSession.litchcraftUndoOpcode &&
              request?.token == observed?.token &&
              request!.sequence <= observed!.sequence;
        });
        if (hasHistory) {
          heldHistoryCheckpoints.add(packet);
          return;
        }
      }
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Retain genuine Paint checkpoints at the SDK delivery seam while native
// State and ordinary Progress continue. No outcomes or authority are fabricated.
class _DelayedPaintCheckpointBle extends VirtualLitchBle {
  _DelayedPaintCheckpointBle(super.native);
  bool holdPaintCheckpoints = false;
  final heldPaintCheckpoints = <VirtualPacket>[];
  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (holdPaintCheckpoints && packet.channel == 'connection_session') {
      final observed = ConnectionSessionCodec.decode(
        Uint8List.fromList(packet.bytes),
      );
      if (observed?.kind == ConnectionSessionKind.control &&
          observed?.code == ConnectionSessionControl.checkpoint &&
          writes.any((write) {
            if (write['channel'] != 'connection_session') return false;
            final request = ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            );
            return request?.kind == ConnectionSessionKind.ordered &&
                request?.code == ConnectionSession.litchcraftPaintOpcode &&
                request?.token == observed?.token &&
                request!.sequence <= observed!.sequence;
          })) {
        heldPaintCheckpoints.add(packet);
        return;
      }
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

class _DelayedLiveLayoutBle extends _DelayedHistoryCheckpointBle {
  _DelayedLiveLayoutBle(super.native, {super.operationIntervalMs});
  bool holdLiveLayouts = false;
  final heldLiveLayouts = <VirtualPacket>[];
  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (holdLiveLayouts && packet.channel == 'connection_session') {
      final p = ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes));
      if (p?.kind == ConnectionSessionKind.object &&
          p?.code == ConnectionLitchcraftCodec.receiptCode &&
          p!.body.length > 4 + 78 &&
          p.body[4] == 0x4d &&
          p.body[6] == LitchcraftV2Protocol.frameStoreSnapshotChunk &&
          p.body[4 + 13 + 1] & 0xc0 == 0xc0) {
        heldLiveLayouts.add(packet);
        return;
      }
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Controlled corruption of a COPY of a genuine SnapshotChunk body signature.
// Native state, metadata, admission and shared token/sequence are untouched.
class _MalformedStoreBodyBle extends VirtualLitchBle {
  _MalformedStoreBodyBle(super.native);
  bool corruptStoreBody = false;
  final corruptedCopies = <Map<String, Object?>>[];
  final _byEnvelope = <String, List<int>>{};
  final originalBodyChunks = <String, Map<int, List<int>>>{};
  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (packet.channel != 'connection_session') {
      super.deliver(packet, lateCallback: lateCallback);
      return;
    }
    final p = ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes));
    if (p == null ||
        p.kind != ConnectionSessionKind.object ||
        p.code != ConnectionLitchcraftCodec.receiptCode) {
      super.deliver(packet, lateCallback: lateCallback);
      return;
    }
    final key = '${packet.epoch}:${p.token}:${p.sequence}';
    var copy = _byEnvelope[key];
    if (corruptStoreBody &&
        p.body.length > 66 &&
        p.body[0] == ConnectionLitchcraftCodec.version &&
        p.body[1] & ConnectionLitchcraftCodec.beginFlag != 0 &&
        p.body[4] == 0x4d &&
        p.body[5] == 2 &&
        p.body[6] == LitchcraftV2Protocol.frameStoreSnapshotChunk) {
      final payloadOffset = 4 + 13;
      final payload = ByteData.sublistView(p.body, payloadOffset);
      final cursor = payload.getUint32(15, Endian.little);
      final status = p.body[payloadOffset];
      final flags = p.body[payloadOffset + 1];
      final singleFrame = flags & 32 != 0;
      final live = flags & 64 != 0;
      final headerBytes = live ? 65 : 47;
      final request = ByteData.sublistView(
        p.body,
      ).getUint16(4 + 11, Endian.little);
      final bodyKey = '${packet.epoch}:${p.token}:$request';
      if (status == 0 &&
          !singleFrame &&
          p.body.length > payloadOffset + headerBytes + 2) {
        (originalBodyChunks[bodyKey] ??= {})[cursor] = p.body.sublist(
          payloadOffset + headerBytes,
        );
        if (copy == null && cursor == 0) {
          // Saved record or LIVE layout row: length2 is followed by CRC32.
          final offset =
              ConnectionSessionCodec.headerBytes +
              payloadOffset +
              headerBytes +
              2;
          copy = List<int>.of(packet.bytes);
          copy[offset] ^= 1;
          _byEnvelope[key] = copy;
          corruptedCopies.add({
            'epoch': packet.epoch,
            'token': p.token,
            'sequence': p.sequence,
            'request': request,
            'cursor': cursor,
            'bodyKey': bodyKey,
            'headerBytes': headerBytes,
            'bodyBytes': payload.getUint32(7, Endian.little),
            'bodyCrc32': payload.getUint32(11, Endian.little),
            'offset': offset,
            'original': packet.bytes,
            'copy': copy,
          });
        }
      }
    }
    if (copy != null) {
      super.deliver(
        VirtualPacket(packet.channel, copy, packet.epoch, packet.pipe),
        lateCallback: lateCallback,
      );
    } else {
      super.deliver(packet, lateCallback: lateCallback);
    }
  }
}

class _MalformedBodyTrace implements AppTraceSink {
  _MalformedBodyTrace(this.clock);
  final int Function() clock;
  final events = <Map<String, Object?>>[];
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (event == 'litchcraft.transport_failed' ||
        event == 'litchcraft.recovery_scheduled' ||
        event == 'litchcraft.physical_store_repair_deferred') {
      events.add({'event': event, 'at': clock(), ...fields});
    }
  }
}

// Time is explicitly advanced at both existing owners. These are scheduling
// bounds, not claims about physical radio, sensor debounce or optical latency.
class _Journey {
  _Journey(this.tester, this.ble, this.service, {this.beforeAdvance});
  final void Function(int milliseconds)? beforeAdvance;
  final WidgetTester tester;
  final VirtualLitchBle ble;
  final LitchBleService service;

  int get now => tester.binding.clock.now().millisecondsSinceEpoch;

  Future<void> tick() async {
    await tester.pump();
    if (ble.hasPendingOperations) {
      await ble.flush(tester, maxOperations: 1);
    } else {
      beforeAdvance?.call(10);
      await ble.command(tester, {
        'op': 'advance',
        'ms': 10,
      }, flushOperations: false);
      await tester.pump(const Duration(milliseconds: 10));
    }
  }

  Future<int> until(
    bool Function() done, {
    int budgetMs = 250,
    String? reason,
  }) async {
    final start = now;
    while (!done() && now - start < budgetMs) {
      await tick();
    }
    expect(
      done(),
      isTrue,
      reason: reason ?? 'Convergence exceeded $budgetMs ms',
    );
    return now - start;
  }

  Future<void> finish(Future<dynamic> work, {int budgetMs = 10000}) async {
    var done = false;
    Object? failure;
    void completed(dynamic _) {
      done = true;
    }

    void failed(Object error, StackTrace _) {
      failure = error;
      done = true;
    }

    work.then<void>(completed, onError: failed);
    await until(() => done, budgetMs: budgetMs);
    if (failure != null) throw failure!;
  }

  Future<Map<String, dynamic>> frame() async =>
      (await ble.command(tester, {
            'op': 'frame',
          }, flushOperations: false))['frame']
          as Map<String, dynamic>;

  Future<Map<String, dynamic>> untilFrame(
    bool Function(Map<String, dynamic> frame) done, {
    int budgetMs = 250,
  }) async {
    final start = now;
    var current = await frame();
    while (!done(current) && now - start < budgetMs) {
      await tick();
      current = await frame();
    }
    expect(
      done(current),
      isTrue,
      reason:
          'Frame convergence exceeded $budgetMs ms; frame=$current writes=${ble.writes.map((write) => write['channel']).toList()} held=${ble.held.map((packet) => packet.channel).toList()}',
    );
    return current;
  }

  Future<void> hall(BigInt mask) => ble.command(tester, {
    'op': 'hall',
    'mask': mask.toRadixString(16).padLeft(16, '0'),
  }, flushOperations: false);

  void checkpoint(String story, Map<String, dynamic> frame, String timing) {
    debugPrint(
      'INTERACTIVE_SYNC $story $timing '
      'native=${frame['now_ms']} occupancy=${frame['occupancy']} '
      'action=${frame['required_action']} display="${frame['display']}" '
      'canvas=${frame['litchcraft_active']} revision=${frame['litchcraft_revision']}',
    );
  }
}

Future<int> _expectCleanStoreParity(
  _Journey journey,
  LitchcraftController controller,
) async {
  // Store snapshots describe a durable root, not a dirty live frame. Let the
  // production storage worker settle before comparing every canonical field.
  await journey.ble.advance(journey.tester, 6000);
  final document = controller.documentSession.document;
  final boardFrames = <Map<String, Object?>>[];
  Map<String, dynamic>? metadata;
  for (var index = 0; index < document.frames.length; index++) {
    final observed =
        (await journey.ble.command(journey.tester, {
              'op': 'store_frame',
              'index': index,
            }))['store_frame']
            as Map<String, dynamic>;
    metadata ??= observed;
    expect(observed['dirty'], isFalse, reason: 'Live Store is not quiescent');
    expect(observed['live_generation'], observed['root_generation']);
    expect(
      observed['root_generation'],
      metadata['root_generation'],
      reason: 'Store root changed during whole-document comparison',
    );
    expect(
      observed['live_revision'],
      metadata['live_revision'],
      reason: 'Live artwork changed during whole-document comparison',
    );
    expect(observed['frame_index'], index);
    boardFrames.add({
      'duration_ms': observed['duration_ms'],
      'emitters': observed['emitters'],
    });
  }
  final app = <String, Object?>{
    'frame_count': document.frames.length,
    'topology': document.topology.id,
    'playback_on_entry': document.playbackOnEntry,
    'sparkle_seed': document.sparkleSeed,
    'favorite_colors': document.favoriteColors,
    'selected_frame': controller.documentSession.selectedFrameIndex,
    'active': controller.boardActive,
    'playing': controller.animationPlaying,
    'frames': [
      for (final frame in document.frames)
        {
          'duration_ms': frame.durationMs,
          'emitters': [
            for (final emitter in frame.emitters)
              [emitter.color, emitter.effect.id, emitter.effectDurationMs],
          ],
        },
    ],
  };
  final board = <String, Object?>{
    'frame_count': metadata!['frame_count'],
    'topology': metadata['topology'],
    'playback_on_entry': metadata['playback_on_entry'],
    'sparkle_seed': metadata['sparkle_seed'],
    'favorite_colors': metadata['favorite_colors'],
    'selected_frame': metadata['selected_frame'],
    'active': metadata['active'],
    'playing': metadata['playing'],
    'frames': boardFrames,
  };
  for (final key in <String>[
    'frame_count',
    'topology',
    'playback_on_entry',
    'sparkle_seed',
    'favorite_colors',
    'selected_frame',
    'active',
    'playing',
  ]) {
    expect(board[key], app[key], reason: 'Store metadata $key diverged');
  }
  if (!controller.animationPlaying) {
    expect(
      controller.canvas.colors,
      document.frames[controller.documentSession.selectedFrameIndex].emitters
          .map((emitter) => emitter.color)
          .toList(),
      reason: 'The visible paused canvas must match the canonical document',
    );
  }
  final expectedFrames = app['frames']! as List<Map<String, Object?>>;
  for (var frame = 0; frame < expectedFrames.length; frame++) {
    expect(
      boardFrames[frame]['duration_ms'],
      expectedFrames[frame]['duration_ms'],
      reason: 'Store frame $frame duration diverged',
    );
    final observed = boardFrames[frame]['emitters']! as List<dynamic>;
    final expected = expectedFrames[frame]['emitters']! as List<dynamic>;
    expect(observed.length, expected.length);
    for (var emitter = 0; emitter < expected.length; emitter++) {
      expect(
        observed[emitter],
        expected[emitter],
        reason:
            'Store frame $frame emitter $emitter diverged at clean '
            'generation ${metadata['live_generation']} '
            'revision ${metadata['live_revision']}; '
            'status=${controller.status} pending=${controller.pending}',
      );
    }
  }
  return metadata['root_generation'] as int;
}

Future<void> _expectAnimatedBoardOutput(_Journey journey) async {
  final before = await journey.frame();
  final beforeStore =
      (await journey.ble.command(journey.tester, {
            'op': 'store_frame',
            'index': 0,
          }))['store_frame']
          as Map<String, dynamic>;
  expect(before['litchcraft_active'], isTrue);
  expect(before['litchcraft_visible'], isTrue);
  final pixels = before['physical'] as List<dynamic>;
  expect(
    pixels.any((pixel) => (pixel as List<dynamic>).take(3).any((v) => v != 0)),
    isTrue,
    reason: 'Bouncing Balls must produce lit physical pixels',
  );
  final firstOutput = jsonEncode(pixels);
  await journey.ble.advance(journey.tester, 100);
  final after = await journey.frame();
  final afterStore =
      (await journey.ble.command(journey.tester, {
            'op': 'store_frame',
            'index': 0,
          }))['store_frame']
          as Map<String, dynamic>;
  expect(
    afterStore['selected_frame'],
    isNot(beforeStore['selected_frame']),
    reason:
        'Playing=${beforeStore['playing']} at ${before['now_ms']}ms; '
        'after playing=${afterStore['playing']} at ${after['now_ms']}ms',
  );
  expect(
    jsonEncode(after['physical']) != firstOutput,
    isTrue,
    reason: 'The physical animation must advance across two 50 ms frames',
  );
}

List<Uint8List> _litchcraftRequests(
  VirtualLitchBle transport, [
  int start = 0,
]) {
  final assembler = ConnectionLitchcraftAssembler();
  final result = <Uint8List>[];
  for (final write in transport.writes.skip(start)) {
    if (write['channel'] != 'connection_session') continue;
    final packet = ConnectionSessionCodec.decode(
      Uint8List.fromList(write['bytes'] as List<int>),
    );
    if (packet?.kind == ConnectionSessionKind.ordered &&
        packet?.code == ConnectionSession.litchcraftModeOpcode &&
        packet!.body.length == 8 &&
        packet.body[0] == LitchcraftProtocol.magic &&
        (packet.body[1] == LitchcraftProtocol.enter ||
            packet.body[1] == LitchcraftProtocol.exit)) {
      result.add(packet.body);
      continue;
    }
    if (packet?.kind != ConnectionSessionKind.object ||
        packet?.code != ConnectionLitchcraftCodec.objectOpcode) {
      continue;
    }
    final request = assembler.accept(packet!.body);
    if (request != null) result.add(request);
  }
  return result;
}

bool _isArtworkBodyRead(Uint8List packet) =>
    packet.length >= 9 &&
    packet[0] == LitchcraftV2Protocol.magic &&
    packet[2] == LitchcraftV2Protocol.frameStoreSnapshot &&
    !_isLiveLayoutRead(packet);

bool _isLiveLayoutRead(Uint8List packet) =>
    packet.length == 23 &&
    packet[0] == LitchcraftV2Protocol.magic &&
    packet[2] == LitchcraftV2Protocol.frameStoreSnapshot &&
    ByteData.sublistView(packet).getUint16(17, Endian.little) == 0xfffe;

Future<Finder> _revealFrame(WidgetTester tester, int index) async {
  final target = find.byKey(ValueKey('craft-select-frame-$index'));
  final viewport = find.byKey(const ValueKey('craft-frame-scroll-viewport'));
  final mountedIndices =
      tester
          .widgetList<Widget>(
            find.byWidgetPredicate((widget) {
              final key = widget.key;
              return key is ValueKey<String> &&
                  key.value.startsWith('craft-select-frame-');
            }),
          )
          .map(
            (widget) => int.parse(
              (widget.key! as ValueKey<String>).value.split('-').last,
            ),
          )
          .toList()
        ..sort();
  final backwards = mountedIndices.isNotEmpty && index < mountedIndices.first;
  await tester.scrollUntilVisible(
    target,
    backwards ? -120 : 120,
    scrollable: find.descendant(
      of: viewport,
      matching: find.byType(Scrollable),
    ),
  );
  return target;
}

// Inspect is a mode fact; an unmounted Home feed must not issue a legacy or
// ordered console position refresh before that fact selects Play.
int _coldPositionQueries(VirtualLitchBle ble, int start) {
  var count = 0;
  var record = <int>[];
  for (final write in ble.writes.skip(start)) {
    final bytes = write['bytes'] as List<int>;
    if (write['channel'] == 'command') {
      if (utf8.decode(bytes).trim() == 'engine fen') ++count;
      continue;
    }
    if (write['channel'] != 'connection_session') continue;
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(bytes));
    if (packet?.kind != ConnectionSessionKind.ordered ||
        packet?.code != LitchControlProtocol.connectionConsoleOpcode) {
      continue;
    }
    final body = packet!.body;
    if (body.length < 2) continue;
    final begins = (body[1] & LitchControlProtocol.connectionConsoleBegin) != 0;
    final ends = (body[1] & LitchControlProtocol.connectionConsoleEnd) != 0;
    if (begins) record = <int>[];
    record.addAll(body.sublist(begins ? 3 : 2));
    if (ends && utf8.decode(record).trim() == 'engine fen') ++count;
  }
  return count;
}

void main() {
  _registerSurpriseArtworkJourneys();
  _registerPhysicalUndoInterruptedStrokeJourneys();
  _registerQueuedNewJourneys();
  _registerFrameEditPointerJourneys();
  setUp(() {
    SharedPreferences.setMockInitialValues({});
  });

  testWidgets(
    'Litchcraft: retained phone source survives raw mode change and physical empty',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(service);
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      Future<void> physicalModeChord() async {
        for (final button in <int>[5, 6]) {
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': button,
            'down': true,
          }, flushOperations: false);
          await ble.advance(tester, 30);
        }
        await ble.advance(tester, 1500);
        for (final button in <int>[5, 6]) {
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': button,
            'down': false,
          }, flushOperations: false);
          await ble.advance(tester, 30);
        }
      }

      Future<Map<String, dynamic>> stored() async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']
              as Map<String, dynamic>;
      Future<void> placeBrush(int square) async {
        await journey.hall(BigInt.one << square);
        await ble.advance(tester, 80);
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 400);
      }

      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(library.ready);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      final selected = staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint([0], 15, 0x12ab34),
      );
      await journey.finish(controller.replaceDocument(selected));
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(
        controller.setBrush(color: 0xabcdef, targetMask: 15),
      );
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();

      for (final empty in <bool>[false, true]) {
        final previousMode = controller.sessionId;
        final source = controller.documentSession.replacementGeneration;
        final sourceDocument = controller.documentSession.document;
        await journey.finish(service.disconnect());
        expect(controller.status, LitchcraftStatus.offline);
        if (empty) {
          // O hold selects Erase; Hall input removes A's sole painted square.
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 6,
            'down': true,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          await ble.advance(tester, 1000);
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 6,
            'down': false,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          expect((await journey.frame())['litchcraft_erasing'], isTrue);
          await placeBrush(0);
        }
        await physicalModeChord();
        expect((await journey.frame())['litchcraft_active'], isFalse);
        await physicalModeChord();
        final entered = await journey.frame();
        expect(entered['litchcraft_active'], isTrue);
        expect(entered['litchcraft_session'], isNot(previousMode));
        if (!empty) await placeBrush(7);
        await ble.advance(tester, 6000);
        final board = await stored();
        final colors = (board['emitters'] as List).map((e) => e[0]).toList();
        expect(board['dirty'], isFalse);
        expect(colors.take(4), everyElement(empty ? 0 : 0x12ab34));
        if (empty) {
          expect(colors, everyElement(0));
        } else {
          expect(colors.skip(28).take(4), everyElement(0xabcdef));
        }
        expect(controller.documentSession.document, same(sourceDocument));
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.settleModeResolution());
        await journey.until(
          () => !controller.pending && !controller.boardArtworkUnresolved,
          budgetMs: 10000,
        );
        journey.checkpoint(
          'retained-source-${empty ? 'empty' : 'different'}',
          await journey.frame(),
          'previous_mode=$previousMode status=${controller.status.name} '
              'phone_preserved=${sameLitchcraftDocument(controller.documentSession.document, selected)}',
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, selected),
          isTrue,
          reason:
              'A new physical mode cannot silently replace selected phone A',
        );
        expect(controller.documentSession.replacementGeneration, source);
        if (empty) {
          expect(controller.status, LitchcraftStatus.live);
          expect(find.text('Choose artwork'), findsNothing);
        } else {
          expect(controller.status, LitchcraftStatus.conflict);
          await tester.tap(
            find.byKey(const ValueKey('craft-resolve-conflict')),
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.pending,
            budgetMs: 10000,
          );
        }
        await _expectCleanStoreParity(journey, controller);
      }
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: Play bypasses optional resident discovery after confirmed Commit',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _DelayedOpenMetadataBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        ble.holdStoreResidents = false;
        ble.release('connection_session');
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      Future<Map<String, dynamic>> stored() async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']
              as Map<String, dynamic>;
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      final predecessor = await stored();

      // Hold only the optional listing, preserving the genuine Commit reply,
      // Object progress, State and the shared session's Ordered control lane.
      ble.holdStoreResidents = true;
      final uploadWritesFrom = ble.writes.length;
      await journey.finish(library.loadVersionArtwork(1));
      final published = await stored();
      expect(
        published['root_generation'],
        greaterThan(predecessor['root_generation'] as int),
      );
      expect(published['frame_count'], 32);
      expect(published['playing'], isFalse);
      expect(controller.storeReplacementUnresolved, isFalse);
      expect(controller.animationPlaying, isFalse);

      final writesFrom = ble.writes.length;
      final started = journey.now;
      final play = controller.toggleAnimationPlayback();
      unawaited(play.catchError((Object _) {}));
      while (journey.now - started < 1200) {
        await journey.tick();
      }
      final timelineBeforeRelease = ble.writes.skip(writesFrom).where((write) {
        if (write['channel'] != 'connection_session') return false;
        final packet = ConnectionSessionCodec.decode(
          Uint8List.fromList(write['bytes'] as List<int>),
        );
        return packet?.kind == ConnectionSessionKind.ordered &&
            packet?.code == ConnectionSession.litchcraftTimelineOpcode &&
            packet!.body[0] == 1;
      }).length;
      final residentLookups = _litchcraftRequests(ble, uploadWritesFrom)
          .where(
            (packet) =>
                packet[0] == LitchcraftV2Protocol.magic &&
                packet[2] == LitchcraftV2Protocol.frameStoreResidents,
          )
          .length;
      final beforeRelease = await stored();
      final phonePlayingBeforeRelease = controller.animationPlaying;
      journey.checkpoint(
        'post-commit-residents-held-Play',
        await journey.frame(),
        'held=${ble.held.length} lookups=$residentLookups '
            'elapsed=${journey.now - started}ms '
            'timelineWrites=$timelineBeforeRelease '
            'phonePlaying=${controller.animationPlaying} '
            'nativePlaying=${beforeRelease['playing']} '
            'root=${beforeRelease['root_generation']} next=release-only-cache-reply',
      );

      ble.holdStoreResidents = false;
      ble.release('connection_session');
      await journey.finish(play);
      final afterRelease = await stored();
      expect(afterRelease['playing'], isTrue);
      expect(afterRelease['root_generation'], published['root_generation']);
      journey.checkpoint(
        'post-commit-residents-released-Play',
        await journey.frame(),
        'nativePlaying=${afterRelease['playing']} '
            'root=${afterRelease['root_generation']} next=ordinary-playback',
      );
      // Flutter checks pending timers before package:test tearDown callbacks.
      // End the live BLE journey here so its ordinary keepalive is retired.
      await journey.finish(library.flush());
      await journey.finish(service.disconnect());
      expect(
        <String, Object?>{
          'lookups': residentLookups,
          'sent': timelineBeforeRelease,
          'native': beforeRelease['playing'],
          'phone': phonePlayingBeforeRelease,
        },
        <String, Object?>{
          'lookups': 0,
          'sent': 1,
          'native': true,
          'phone': true,
        },
        reason:
            'Commit must supply resident cache proof without another lookup; '
            'Play must reach native without another upload or second tap',
      );
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final scenario in <String>[
    'Pause captures visible frame across admission boundary',
    'Rapid playback intent survives predecessor State',
    'Physical Pause supersedes a receipted Play without its State',
    'Physical Pause before delayed Play receipt survives retained Query',
    'Predecessor Pause before delayed Play receipt queries actual Play',
    'New artwork frame burst retains corner paint',
    'New artwork uses one immediate ordered command',
  ]) {
    testWidgets(
      'Owner QA: $scenario',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = scenario.contains('delayed')
            ? _DelayedTimelineCheckpointBle(native, operationIntervalMs: 20)
            : VirtualLitchBle(native, operationIntervalMs: 20);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final document = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        );
        if (!scenario.startsWith('New artwork frame')) {
          document.setSelectedFrameDuration(100);
          document.addFrame();
          document.paintSelectedFrame(
            squares: <int>[7],
            mask: 15,
            color: 0x11cc33,
          );
          document.setSelectedFrameDuration(100);
          document.selectFrame(0);
        }
        final controller = LitchcraftController(
          service,
          documentSession: document,
          monotonicMilliseconds: () => journey.now,
        );
        final library = LitchcraftLibrary(controller);
        var controllerDisposed = false;
        addTearDown(() async {
          if (!controllerDisposed) {
            library.dispose();
            controller.dispose();
          }
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await journey.finish(
          controller.transferFrameStoreForDiagnostics(
            frames: document.document.frames,
            playbackOnEntry: false,
          ),
        );
        // Diagnostic seeding publishes bytes; normal entry resolves the
        // initial missing-root status before the reported user interaction.
        await journey.finish(controller.enter());
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        await journey.finish(controller.selectAnimationFrame(0));
        if (scenario.startsWith('Pause')) {
          await journey.finish(controller.toggleAnimationPlayback());
          await ble.flush(tester);
          // Position one transport turn before the displayed frame boundary.
          while (controller.animationElapsedMilliseconds % 100 < 85) {
            await ble.advance(tester, 1);
          }
          final visible = controller.animationSelectedFrameIndex;
          final pause = controller.toggleAnimationPlayback();
          expect(controller.animationSelectedFrameIndex, visible);
          await journey.finish(pause);
          final metadata =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': visible,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(
            metadata['selected_frame'],
            visible,
            reason:
                'Pause must retain the frame visible at button press, even when delivery crosses a boundary',
          );
          expect(controller.animationSelectedFrameIndex, visible);
          expect(metadata['playing'], isFalse);
          journey.checkpoint(
            'pause-visible-frame',
            await journey.frame(),
            'visible=$visible native_selected=${metadata['selected_frame']} app_selected=${controller.animationSelectedFrameIndex}',
          );
        } else if (scenario.contains('delayed')) {
          final delayed = ble as _DelayedTimelineCheckpointBle;
          final physical = scenario.startsWith('Physical');
          if (!physical) {
            await journey.finish(controller.toggleAnimationPlayback());
            ble.holdConnectionObservations = true;
            await journey.finish(controller.toggleAnimationPlayback());
            await ble.flush(tester);
          }
          final predecessor = ble.emitted.lastWhere((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.state &&
                decoded?.code == ConnectionSession.litchcraftStateCode &&
                decoded!.body[4] & 2 == 0;
          });
          delayed.timelineWritesFrom = ble.writes.length;
          delayed.holdPlayCheckpoint = true;
          ble.holdConnectionObservations = true;
          final play = controller.toggleAnimationPlayback();
          await journey.until(() => delayed.heldPlayCheckpoints.isNotEmpty);
          expect(controller.animationTimelinePending, isTrue);
          expect(controller.animationPlaying, isTrue);
          if (physical) {
            ble.heldChannels.addAll(<String>[
              'connection_session',
              'litchcraft',
            ]);
            await journey.hall(BigInt.one);
            await ble.advance(tester, 40);
            final pause = ble.held.lastWhere((packet) {
              final decoded = ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              );
              return decoded?.kind == ConnectionSessionKind.state &&
                  decoded?.code == ConnectionSession.litchcraftStateCode &&
                  decoded!.body[4] & 2 == 0;
            });
            ble.deliver(pause);
          } else {
            ble.deliver(predecessor);
          }
          await tester.pump();
          expect(controller.animationPlaying, isTrue);
          final writesBeforeReceipt = ble.writes.length;
          delayed.holdPlayCheckpoint = false;
          ble.holdConnectionObservations = false;
          ble.heldChannels.remove('connection_session');
          ble.deliver(delayed.heldPlayCheckpoints.last);
          await journey.finish(play);
          await ble.flush(tester);
          final metadata =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(metadata['playing'], !physical);
          expect(
            controller.animationPlaying,
            !physical,
            reason:
                'The post-receipt retained State Query must resolve current native playback',
          );
          expect(
            ble.writes.skip(writesBeforeReceipt).where((write) {
              if (write['channel'] != 'connection_session') return false;
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind == ConnectionSessionKind.control &&
                  packet?.code == ConnectionSessionControl.query &&
                  packet?.sequence == ConnectionSession.litchcraftStateCode;
            }),
            hasLength(1),
          );
          journey.checkpoint(
            physical
                ? 'physical-pause-before-play-receipt'
                : 'predecessor-pause-resolved-after-play-receipt',
            await journey.frame(),
            'native_playing=${!physical} app_playing=${controller.animationPlaying} causalQuery=1 next=${physical ? 'continue-painting' : 'Pause'}',
          );
          ble.heldChannels.clear();
          ble.release('connection_session');
          ble.release('litchcraft');
          if (physical) await journey.hall(BigInt.zero);
        } else if (scenario.startsWith('Physical')) {
          ble.holdConnectionObservations = true;
          await journey.finish(controller.toggleAnimationPlayback());
          await ble.flush(tester);
          expect(controller.animationPlaying, isTrue);
          expect(controller.animationTimelinePending, isFalse);
          // Omit the genuine Play edge, then pause through the physical Hall
          // stroke owner. Retain its square echo so State alone must win.
          ble.heldChannels.addAll(<String>['connection_session', 'litchcraft']);
          await journey.hall(BigInt.one);
          await ble.advance(tester, 40);
          final shared = await service.openConnectionSession(_boardId);
          await journey.finish(
            shared!.requestState(code: ConnectionSession.litchcraftStateCode),
          );
          await ble.flush(tester);
          final pauseState = ble.held.lastWhere((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.state &&
                decoded?.code == ConnectionSession.litchcraftStateCode &&
                decoded!.body[4] & 2 == 0;
          });
          ble.deliver(pauseState);
          await tester.pump();
          final metadata =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(metadata['playing'], isFalse);
          expect(
            controller.animationPlaying,
            isFalse,
            reason:
                'A fresh physical Pause State must supersede completed Play intent even when its semantic edge was coalesced',
          );
          ble.holdConnectionObservations = false;
          ble.heldChannels.clear();
          ble.release('connection_session');
          ble.release('litchcraft');
          await tester.pump();
          expect(controller.animationPlaying, isFalse);
          await journey.hall(BigInt.zero);
          journey.checkpoint(
            'physical-pause-after-play-receipt',
            await journey.frame(),
            'physical-Hall Pause accepted Hall=0 native_paused=true app_paused=true next=continue-painting',
          );
        } else if (scenario.startsWith('Rapid')) {
          await journey.finish(controller.toggleAnimationPlayback());
          ble.holdConnectionObservations = true;
          await journey.finish(controller.toggleAnimationPlayback());
          await ble.flush(tester);
          final oldState = ble.held.where((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.state &&
                decoded?.code == ConnectionSession.litchcraftStateCode;
          }).last;
          final play = controller.toggleAnimationPlayback();
          expect(controller.animationPlaying, isTrue);
          ble.deliver(oldState);
          await tester.pump();
          expect(
            controller.animationPlaying,
            isTrue,
            reason:
                'A predecessor Pause State cannot overwrite the latest visible Play intent',
          );
          ble.holdConnectionObservations = false;
          ble.release('connection_session');
          await journey.finish(play);
          for (var tap = 0; tap < 6; tap++) {
            await journey.finish(controller.toggleAnimationPlayback());
            expect(
              controller.status,
              LitchcraftStatus.live,
              reason: 'tap=$tap error=${controller.errorMessage}',
            );
          }
          final toggles = <Future<void>>[];
          for (var tap = 0; tap < 12; tap++) {
            final intendedPlaying = !controller.animationPlaying;
            toggles.add(controller.toggleAnimationPlayback());
            expect(controller.animationPlaying, intendedPlaying);
          }
          await journey.finish(Future.wait<void>(toggles));
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          expect(controller.errorMessage, isNull);
          expect(service.isConnected, isTrue);
          expect(ble.connections, hasLength(1));
          journey.checkpoint(
            'rapid-playback-drained',
            await journey.frame(),
            '18 toggles live=true connected=true reconnects=0',
          );
        } else if (scenario.startsWith('New artwork uses')) {
          await ble.flush(tester);
          final writesBefore = ble.writes.length;
          final start = journey.now;
          final newArtwork = library.newArtwork();
          await tester.pump();
          expect(document.document.frames, hasLength(1));
          expect(controller.canvas.hasMarks, isFalse);
          await journey.finish(newArtwork);
          final receiptMilliseconds = journey.now - start;
          final packets = ble.writes
              .skip(writesBefore)
              .where((write) => write['channel'] == 'connection_session')
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where((packet) => packet.kind == ConnectionSessionKind.ordered)
              .toList();
          expect(packets, hasLength(1));
          expect(packets.single.body, hasLength(8));
          expect(
            packets.where(
              (packet) =>
                  packet.code ==
                  ConnectionSession.litchcraftStoreNewCanvasOpcode,
            ),
            hasLength(1),
          );
          expect(receiptMilliseconds, lessThan(50));
          journey.checkpoint(
            'compact-new-receipt',
            await journey.frame(),
            'local_preview_ms=0 receipt_ms=$receiptMilliseconds frames=1 paused=true next=draw',
          );
          expect(document.document.frames, hasLength(1));
          expect(controller.canvas.hasMarks, isFalse);
          controller.beginStroke();
          controller.paintSquares(<int>[0, 63]);
          controller.endStroke();
          await journey.until(() => !controller.pending, budgetMs: 10000);
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          await _expectCleanStoreParity(journey, controller);
        } else {
          await journey.finish(library.newArtwork());
          final firstAdd = controller.addAnimationFrame();
          final secondAdd = controller.addAnimationFrame();
          expect(document.document.frames, hasLength(3));
          final selection = controller.selectAnimationFrame(1);
          final brush = controller.setBrush(color: 0xdd2244);
          controller.beginStroke();
          controller.paintSquares(<int>[0, 7, 56, 63]);
          controller.endStroke();
          await journey.finish(
            Future.wait<dynamic>([firstAdd, secondAdd, selection, brush]),
          );
          await journey.until(
            () => !controller.pending,
            budgetMs: 10000,
            reason:
                'Insert/select/corner-paint burst must drain without artwork conflict',
          );
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          expect(controller.errorMessage, isNull);
          expect(document.document.frames, hasLength(3));
          expect(controller.animationSelectedFrameIndex, 1);
          await _expectCleanStoreParity(journey, controller);
          for (final square in <int>[0, 7, 56, 63]) {
            expect(
              document.document.frames[1].emitters[square * 4].color,
              0xdd2244,
            );
            expect(document.document.frames[0].emitters[square * 4].color, 0);
            expect(document.document.frames[2].emitters[square * 4].color, 0);
          }
          final firstDelete = controller.deleteAnimationFrame();
          final secondDelete = controller.deleteAnimationFrame();
          controller.beginStroke();
          controller.paintSquares(<int>[0, 7, 56, 63]);
          controller.endStroke();
          await journey.finish(
            Future.wait<dynamic>([firstDelete, secondDelete]),
          );
          await journey.until(() => !controller.pending, budgetMs: 10000);
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          expect(document.document.frames, hasLength(1));
          await _expectCleanStoreParity(journey, controller);
          for (final square in <int>[0, 7, 56, 63]) {
            expect(
              document.document.frames[0].emitters[square * 4].color,
              0xdd2244,
            );
          }
        }
        library.dispose();
        controller.dispose();
        controllerDisposed = true;
        await ble.flush(tester);
        await journey.finish(service.disconnect());
      },
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final scenario in [
    'unmodified play',
    'paint undo play',
    'rapid history play',
    'delete burst play',
    'rotate play',
    'ordinary Play status',
  ]) {
    testWidgets(
      'Owner QA: immediate $scenario before quiet save',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native, operationIntervalMs: 10);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final probe = _StoreHistoryProbe(clock: () => journey.now);
        final controller = LitchcraftController(
          service,
          monotonicMilliseconds: () => journey.now,
          appTrace: probe,
        );
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
        final original = LitchcraftAnimationDocument(
          frames: List.generate(
            256,
            (i) => LitchcraftAnimationFrame(
              durationMs: 300,
              emitters: List.generate(
                256,
                (e) => LitchcraftAnimatedEmitter(
                  color: 0x330000 | i << 8 | ((i * 17 + e) & 255),
                ),
              ),
            ),
          ),
          topology: blank.topology,
          favoriteColors: blank.favoriteColors,
        );
        await journey.finish(
          controller.replaceDocument(original),
          budgetMs: 60000,
        );
        await journey.finish(controller.selectAnimationFrame(37));
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                monotonicMilliseconds: () => journey.now,
              ),
            ),
          ),
        );
        await ble.flush(tester);
        final mutationStart = journey.now;
        final writesBefore = ble.writes.length;
        if (scenario == 'delete burst play') {
          for (var i = 0; i < 8; i++) {
            final target = controller.captureAnimationFrameEditTarget();
            expect(target, isNotNull);
            await journey.finish(
              controller.deleteAnimationFrame(target: target),
              budgetMs: 1500,
            );
          }
        } else if (scenario == 'rotate play') {
          await tester.tap(find.byKey(const ValueKey('craft-rotate-artwork')));
          await tester.pump();
        } else if (scenario != 'unmodified play' &&
            scenario != 'ordinary Play status') {
          await journey.finish(
            controller.setBrush(color: 0x884411, targetMask: 15),
          );
          final rect = tester.getRect(find.byType(LitchcraftCanvasView));
          final finger = await tester.startGesture(
            rect.topLeft + Offset(rect.width * .5 / 8, rect.height * .5 / 8),
          );
          for (var i = 1; i < 8; i++) {
            await finger.moveTo(
              rect.topLeft +
                  Offset(rect.width * (i + .5) / 8, rect.height * .5 / 8),
            );
            await journey.tick();
          }
          await finger.up();
          await tester.pump();
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          await tester.pump();
          if (scenario == 'rapid history play') {
            for (var i = 0; i < 6; i++) {
              final button = find.byKey(
                ValueKey(i.isEven ? 'craft-redo' : 'craft-undo'),
              );
              await journey.until(
                () => tester.widget<IconButton>(button).onPressed != null,
                budgetMs: 500,
              );
              await tester.tap(button);
              await tester.pump();
            }
          }
        }
        // The player presses Play, before the five-second persistence boundary.
        final playStart = journey.now;
        final playing = controller.toggleAnimationPlayback();
        await tester.pump();
        if (scenario == 'ordinary Play status') {
          expect(
            find.text('Syncing…'),
            findsNothing,
            reason: 'A normal Play control does not transfer artwork',
          );
        }
        await journey.finish(playing, budgetMs: 1000);
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 1000,
        );
        final requests = _litchcraftRequests(ble, writesBefore);
        expect(
          requests.where(
            (p) =>
                p[2] == LitchcraftV2Protocol.frameStoreBegin ||
                p[2] == LitchcraftV2Protocol.frameStoreSnapshot,
          ),
          isEmpty,
          reason:
              'Owned edit/history→Play must not read or replace the whole artwork',
        );
        expect(
          find.text('Syncing…'),
          findsNothing,
          reason:
              'Ordinary known actions must not show artwork synchronization',
        );
        debugPrint(
          'OWNER_QA $scenario mutation_ms=${journey.now - mutationStart} play_ms=${journey.now - playStart} appPlaying=${controller.animationPlaying}',
        );
        expect(controller.animationPlaying, isTrue);
        // Observe rendered output only: reading stored frames could disturb the
        // same Store page cursor the production animation worker is using.
        for (var i = 0; i < 100; i++) {
          await journey.tick();
          final frame = await journey.frame();
          final boardColors = <int>[
            for (final square in frame['board'] as List)
              for (final rgb in square as List)
                (rgb[0] as int) << 16 | (rgb[1] as int) << 8 | (rgb[2] as int),
          ];
          final elapsed = controller.animationElapsedMilliseconds;
          final low = litchcraftAnimationFrameIndexAt(
            controller.documentSession.document,
            elapsed - 20,
          );
          final high = litchcraftAnimationFrameIndexAt(
            controller.documentSession.document,
            elapsed + 20,
          );
          // The public native capture projects Top/Bottom to an averaged
          // legacy slot. Real-worker tests observe all256 emitters separately.
          int averagedColor(int top, int bottom) =>
              (((((top >> 16) & 255) + ((bottom >> 16) & 255) + 1) ~/ 2) <<
                  16) |
              (((((top >> 8) & 255) + ((bottom >> 8) & 255) + 1) ~/ 2) << 8) |
              (((top & 255) + (bottom & 255) + 1) ~/ 2);
          bool matchesCompleteProjection(int index) {
            final emitters =
                controller.documentSession.document.frames[index].emitters;
            final projected = <int>[
              for (var square = 0; square < 64; square++) ...[
                emitters[square * 4].color,
                averagedColor(
                  emitters[square * 4 + 1].color,
                  emitters[square * 4 + 3].color,
                ),
                emitters[square * 4 + 2].color,
              ],
            ];
            return boardColors.length == projected.length &&
                List.generate(
                  projected.length,
                  (i) => boardColors[i] == projected[i],
                ).every((matches) => matches);
          }

          expect(
            matchesCompleteProjection(low) || matchesCompleteProjection(high),
            isTrue,
            reason:
                'The complete rendered projection must match one expected frame at tick $i',
          );
        }
        await journey.finish(controller.toggleAnimationPlayback());
        if (scenario != 'delete burst play' && scenario != 'rotate play') {
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              original,
            ),
            isTrue,
          );
        }
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      timeout: const Timeout(Duration(minutes: 2)),
    );
  }

  testWidgets(
    'Litchcraft: board-adopted RR survives persisted cold Play Craft entry without body read',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
      final service = LitchBleService(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      final journey = _Journey(tester, ble, service);
      var controller = LitchcraftController(service);
      var library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(service.disconnect());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
        await tester.pump(const Duration(milliseconds: 200));
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(library.loadRickroll());
      await _expectCleanStoreParity(journey, controller);
      final original = controller.documentSession.document;
      expect(original.frames, hasLength(32));
      // Controlled setup: a fresh phone adopts genuine native Store bytes.
      // This first required artwork read is outside the unchanged-entry probe.
      await journey.finish(service.disconnect());
      library.dispose();
      controller.dispose();
      await ble.flush(tester);
      SharedPreferences.setMockInitialValues({});
      controller = LitchcraftController(service);
      library = LitchcraftLibrary(controller);
      await journey.finish(library.ready);
      expect(controller.documentSession.hasLocalIntent, isFalse);
      await journey.finish(service.connect(_boardId));
      final adoption = controller.adoptActiveBoard();
      await journey.finish(adoption);
      expect(await adoption, isTrue);
      await journey.until(
        () => !controller.boardArtworkUnresolved && !controller.pending,
        budgetMs: 10000,
      );
      expect(
        sameLitchcraftDocument(controller.documentSession.document, original),
        isTrue,
      );
      expect(controller.documentSession.hasLocalIntent, isFalse);
      await journey.finish(library.flush());
      final adopted = controller.documentSession.document;
      final results = <Map<String, Object?>>[];
      for (final cold in [false, true]) {
        await journey.finish(controller.leave());
        await journey.finish(library.flush());
        if (cold) {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await journey.finish(service.disconnect());
          controller = LitchcraftController(service);
          library = LitchcraftLibrary(controller);
          await journey.finish(library.ready);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              adopted,
            ),
            isTrue,
            reason:
                'The real library must restore the native-adopted document.',
          );
          debugPrint(
            'RR_ENTRY_RESTORE frames=${library.animationDocument.frames.length} local_intent=${controller.documentSession.hasLocalIntent}',
          );
          await journey.finish(service.connect(_boardId));
          await tester.pumpWidget(
            MaterialApp(
              home: ConnectionGateScreen(
                bleService: service,
                craftController: controller,
                craftLibrary: library,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
              ),
            ),
          );
          await journey.until(
            () => find.byType(HomeScreen).evaluate().isNotEmpty,
            budgetMs: 10000,
          );
          expect(controller.inspectedBoardActive, isFalse);
        } else {
          await tester.pumpWidget(
            MaterialApp(
              home: LitchMainScaffold(
                bleService: service,
                boardId: _boardId,
                boards: const [],
                onDisconnect: service.disconnect,
                craftController: controller,
                craftLibrary: library,
                initialPageIndex: 1,
                initialCraftModeResolved: true,
                loadAppConsolePreference: () async => false,
              ),
            ),
          );
          await tester.pump();
        }
        final before = controller.documentSession.document;
        final candidate = LitchcraftFrameStoreProtocol.prepare(
          session: 1,
          transfer: 1,
          expectedGeneration: 0,
          topology: before.topology.id,
          active: true,
          playbackOnEntry: before.playbackOnEntry,
          sparkleSeed: before.sparkleSeed,
          favoriteColors: before.favoriteColors,
          frames: before.frames,
        );
        final writesFrom = ble.writes.length;
        final emittedFrom = ble.emitted.length;
        final started = journey.now;
        final phases = <String, int>{};
        final assembler = ConnectionLitchcraftAssembler();
        final storeStates = <LitchcraftFrameStoreState>[];
        var seen = emittedFrom;
        var seenWrites = writesFrom;
        var authorityQueries = 0;
        var authorityReplies = 0;
        void sample() {
          for (final write in ble.writes.skip(seenWrites)) {
            if (write['channel'] != 'connection_session') continue;
            final packet = ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            );
            if (packet?.kind == ConnectionSessionKind.control &&
                packet?.code == ConnectionSessionControl.query &&
                packet?.sequence ==
                    ConnectionSession.litchcraftStoreAuthorityStateCode) {
              ++authorityQueries;
              phases.putIfAbsent(
                'authority_query',
                () => journey.now - started,
              );
            }
          }
          seenWrites = ble.writes.length;
          final requests = _litchcraftRequests(ble, writesFrom);
          for (final request in requests.where(
            (p) => p[0] == LitchcraftV2Protocol.magic,
          )) {
            final label =
                request[2] == LitchcraftV2Protocol.inspect &&
                    request.length == 10 &&
                    request[9] == 7
                ? 'store_inspect_write'
                : request[2] == LitchcraftV2Protocol.frameStoreSnapshot
                ? 'snapshot_write'
                : request[2] == LitchcraftV2Protocol.frameStoreBegin
                ? 'begin_write'
                : null;
            if (label != null) {
              phases.putIfAbsent(label, () => journey.now - started);
            }
          }
          for (final packet in ble.emitted.skip(seen)) {
            final envelope = packet.channel == 'connection_session'
                ? ConnectionSessionCodec.decode(
                    Uint8List.fromList(packet.bytes),
                  )
                : null;
            if (envelope?.kind == ConnectionSessionKind.state &&
                envelope?.code ==
                    ConnectionSession.litchcraftStoreAuthorityStateCode) {
              ++authorityReplies;
              phases.putIfAbsent(
                'authority_state',
                () => journey.now - started,
              );
            }
            if (envelope?.kind != ConnectionSessionKind.object ||
                envelope?.code != ConnectionLitchcraftCodec.receiptCode) {
              continue;
            }
            final body = assembler.accept(envelope!.body);
            final decoded = body == null
                ? null
                : LitchcraftV2Packet.decode(body);
            if (decoded?.event == LitchcraftV2Protocol.frameStoreState) {
              final state = LitchcraftFrameStoreState.fromPacket(decoded!);
              if (state.residentListing != null) {
                storeStates.add(state);
                phases.putIfAbsent(
                  'store_inspect_reply',
                  () => journey.now - started,
                );
              }
            }
          }
          seen = ble.emitted.length;
        }

        debugPrint(
          'RR_ENTRY_BEFORE cold=$cold frames=${before.frames.length} local_intent=${controller.documentSession.hasLocalIntent} crc=${candidate.canonicalBodyCrc32} bytes=${candidate.canonicalBodyBytes} playback=${before.playbackOnEntry} seed=${before.sparkleSeed} topology=${before.topology.id}',
        );
        await tester.tap(find.byKey(const ValueKey('main-destination-craft')));
        await tester.pump();
        sample();
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pump();
        expect(
          find.byKey(const ValueKey('craft-animation-bar')),
          findsOneWidget,
        );
        phases['frame_controls'] = journey.now - started;
        while ((controller.pending ||
                controller.boardArtworkUnresolved ||
                controller.status != LitchcraftStatus.live) &&
            journey.now - started < 10000) {
          await journey.tick();
          sample();
        }
        phases['ready'] = journey.now - started;
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        expect(controller.boardArtworkUnresolved, isFalse);
        expect(storeStates, isNotEmpty);
        final state = storeStates.first;
        final selected = state.residents.singleWhere(
          (resident) => resident.rootSlot == state.selectedRootSlot,
        );
        final requests = _litchcraftRequests(ble, writesFrom);
        final result = <String, Object?>{
          'cold': cold,
          'phases_ms': phases,
          'authority_queries': authorityQueries,
          'authority_replies': authorityReplies,
          'snapshots': requests
              .where(
                (p) =>
                    p[0] == LitchcraftV2Protocol.magic &&
                    p[2] == LitchcraftV2Protocol.frameStoreSnapshot,
              )
              .length,
          'begins': requests
              .where(
                (p) =>
                    p[0] == LitchcraftV2Protocol.magic &&
                    p[2] == LitchcraftV2Protocol.frameStoreBegin,
              )
              .length,
          'resident_match': selected.matchesArtworkExceptFavorites(candidate),
          'local_intent_after': controller.documentSession.hasLocalIntent,
          'app_frames': candidate.frameCount,
          'native_frames': selected.frameCount,
          'app_crc': candidate.canonicalBodyCrc32,
          'native_crc': selected.bodyCrc32,
          'app_bytes': candidate.canonicalBodyBytes,
          'native_bytes': selected.bodyBytes,
          'app_playback': candidate.playbackOnEntry,
          'native_playback': selected.playbackOnEntry,
          'app_seed': candidate.sparkleSeed,
          'native_seed': selected.sparkleSeed,
          'app_topology': candidate.topology,
          'native_topology': selected.topology,
          'native_active': selected.active,
          'native_predecessor_deltas': selected.predecessorDeltas,
        };
        debugPrint('RR_ENTRY_RESULT ${jsonEncode(result)}');
        results.add(result);
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pump();
        await _revealFrame(tester, 31);
        expect(
          find.byKey(const ValueKey('craft-select-frame-31')),
          findsOneWidget,
        );
        await tester.tap(find.byKey(const ValueKey('main-destination-play')));
        await tester.pump();
        await journey.until(
          () => !controller.boardActive && !controller.pending,
          budgetMs: 1000,
        );
      }
      for (final result in results) {
        expect(result['snapshots'], 0, reason: jsonEncode(result));
        expect(result['begins'], 0, reason: jsonEncode(result));
        expect(result['resident_match'], isTrue, reason: jsonEncode(result));
      }
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(service.disconnect());
      await tester.pump(const Duration(milliseconds: 200));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final frameCount in [2, 32]) {
    testWidgets(
      'Litchcraft: ordinary app Play Craft Play keeps truthful Store feedback frames=$frameCount',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
        final service = LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          await journey.finish(service.disconnect());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
          // Link cancellation retires the Play feed's health owner, while an
          // already-created 180ms retry Future still needs its final pump.
          await tester.pump(const Duration(milliseconds: 200));
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        if (frameCount == 32) {
          await journey.finish(library.loadRickroll());
        } else {
          final artwork = LitchcraftDocumentSession(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0], 15, 0x00aaff),
            ),
          )..addFrame();
          addTearDown(artwork.dispose);
          await journey.finish(controller.replaceDocument(artwork.document));
        }
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(controller.leave());
        await journey.finish(library.dismissWelcome());
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: _boardId,
              boards: const [],
              onDisconnect: service.disconnect,
              craftController: controller,
              craftLibrary: library,
              initialPageIndex: 1,
              initialCraftModeResolved: true,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pump();
        final feedback = <String>{};
        var sustainedNotices = 0;
        void inspectFeedback(String phase, int elapsed) {
          final card = find.byKey(const ValueKey('craft-status-card'));
          if (card.evaluate().isEmpty) return;
          final messages = tester
              .widgetList<Text>(
                find.descendant(of: card, matching: find.byType(Text)),
              )
              .map((text) => text.data)
              .toList();
          final working =
              controller.status == LitchcraftStatus.connecting ||
              controller.boardArtworkLoading ||
              controller.artworkReadbackPending ||
              controller.fullAnimationSyncDirection != null ||
              controller.storeRootPublicationPending;
          final description =
              '$phase status=${controller.status.name} '
              'unresolved=${controller.boardArtworkUnresolved} '
              'readback=${controller.artworkReadbackPending} '
              'activity=${find.byKey(const ValueKey('craft-sync-activity')).evaluate().length} '
              'messages=$messages';
          if (!working || elapsed < 500 || messages.isEmpty) {
            feedback.add(description);
          } else {
            sustainedNotices++;
          }
        }

        for (var cycle = 0; cycle < 2; cycle++) {
          final started = journey.now;
          final writesFrom = ble.writes.length;
          final noticesBefore = sustainedNotices;
          await tester.tap(
            find.byKey(const ValueKey('main-destination-craft')),
          );
          await tester.pump();
          inspectFeedback('entry-$cycle', journey.now - started);
          while ((controller.status != LitchcraftStatus.live ||
                  !controller.boardActive ||
                  controller.boardArtworkUnresolved ||
                  controller.pending) &&
              journey.now - started < 10000) {
            await journey.tick();
            inspectFeedback('handoff-$cycle', journey.now - started);
          }
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.boardArtworkUnresolved, isFalse);
          final entryMs = journey.now - started;
          if (frameCount == 2) expect(entryMs, lessThan(500));
          if (entryMs >= 500) {
            expect(sustainedNotices, greaterThan(noticesBefore));
          }
          inspectFeedback('complete-$cycle', entryMs);
          final requests = _litchcraftRequests(ble, writesFrom);
          final uploads = requests
              .where((p) => p[2] == LitchcraftV2Protocol.frameStoreBegin)
              .length;
          final fullReads = requests
              .where(
                (p) =>
                    p[2] == LitchcraftV2Protocol.frameStoreSnapshot &&
                    p.length == 17,
              )
              .length;
          expect(
            uploads,
            0,
            reason: 'Mode entry never uploads retained artwork',
          );
          debugPrint(
            'CRAFT_ENTRY_TIMING frames=$frameCount cycle=$cycle '
            'entry_ms=$entryMs sustained_notices=${sustainedNotices - noticesBefore} '
            'whole_uploads=$uploads whole_reads=$fullReads',
          );
          await ble.advance(tester, 200);
          inspectFeedback('healthy-$cycle', journey.now - started);
          final active = await journey.frame();
          expect(active['litchcraft_active'], isTrue);
          expect(active['litchcraft_visible'], isTrue);
          journey.checkpoint(
            'quiet-app-Craft-$cycle',
            active,
            'accepted_enter=true live=true frames=$frameCount next=Draw entry_ms=$entryMs',
          );
          await tester.tap(find.byKey(const ValueKey('main-destination-play')));
          await tester.pump();
          inspectFeedback('exit-$cycle', journey.now - started);
          await journey.until(
            () => !controller.boardActive && !controller.pending,
            budgetMs: 1000,
          );
          await ble.advance(tester, 700);
          inspectFeedback('departed-$cycle', journey.now - started);
          final exited = await journey.frame();
          expect(exited['litchcraft_active'], isFalse);
          expect(
            find.byKey(const ValueKey('craft-sync-activity')),
            findsNothing,
          );
          journey.checkpoint(
            'quiet-app-Play-$cycle',
            exited,
            'accepted_exit=true Craft_feedback=absent next=PlayBoardSetup',
          );
        }
        debugPrint('CRAFT_ENTRY_FEEDBACK $feedback');
        expect(
          feedback,
          isEmpty,
          reason:
              'Only sustained real work may render a named activity surface',
        );
        // Flutter checks pending timers before addTearDown runs. Retire the
        // mounted Play feed and its already-created health retry first.
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(service.disconnect());
        await tester.pump(const Duration(milliseconds: 200));
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final driftPpm in [100, -100]) {
    testWidgets(
      'Litchcraft: five-minute native playback with $driftPpm ppm phone clock',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedPlayAdmissionBle(native);
        final service = _BridgeBoard(ble);
        var phoneNow = 0;
        final journey = _Journey(
          tester,
          ble,
          service,
          beforeAdvance: (milliseconds) => phoneNow += milliseconds,
        );
        final controller = LitchcraftController(
          service,
          monotonicMilliseconds: () => phoneNow,
        );
        final library = LitchcraftLibrary(controller);
        var disposed = false;
        Future<void> disposePhone() async {
          if (disposed) return;
          disposed = true;
          ble.releasePlay();
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await journey.finish(service.dispose());
          await ble.flush(tester);
          await journey.finish(ble.close());
        }

        addTearDown(native.close);
        var maximumError = 0;
        var periodicSamples = 0;
        try {
          await journey.hall(BigInt.zero);
          phoneNow += 1000;
          await ble.advance(tester, 1000);
          await journey.finish(service.connect(_boardId));
          await journey.finish(library.ready);
          await journey.finish(controller.enter());
          final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
          final document = LitchcraftAnimationDocument(
            frames: List.generate(
              256,
              (index) => LitchcraftAnimationFrame(
                durationMs: 500 + index,
                emitters: List.filled(
                  256,
                  LitchcraftAnimatedEmitter(
                    color: 0x220000 | index << 8 | (index * 17 & 255),
                  ),
                ),
              ),
            ),
            topology: blank.topology,
            favoriteColors: blank.favoriteColors,
          );
          await journey.finish(
            controller.replaceDocument(document),
            budgetMs: 60000,
          );
          await journey.finish(controller.selectAnimationFrame(37));
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                  monotonicMilliseconds: () => phoneNow,
                ),
              ),
            ),
          );
          await ble.flush(tester);
          expect(controller.animationSelectedFrameIndex, 37);
          expect(controller.animationPlaying, isFalse);
          ble.holdNextPlay = true;
          final play = controller.toggleAnimationPlayback();
          await journey.until(() => ble.playHeld);
          await tester.pump();
          expect(controller.pending, isTrue);
          expect(
            find.textContaining('Syncing'),
            findsNothing,
            reason: 'Pending Play delivery does not transfer artwork',
          );
          phoneNow += 250;
          await ble.advance(tester, 250);
          final held =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 37,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(held['playing'], isFalse);
          expect(held['selected_frame'], 37);
          ble.releasePlay();
          await journey.finish(play);
          await ble.flush(tester);
          final startFrame = await journey.frame();
          final nativeStart = startFrame['now_ms'] as int;
          final startPhone = phoneNow;
          final startMetadata =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 37,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(startMetadata['playing'], isTrue);
          final cycle = document.frames.fold<int>(
            0,
            (a, f) => a + f.durationMs,
          );
          int phaseError(int app, int board) {
            var error = (app - board) % cycle;
            if (error > cycle ~/ 2) error -= cycle;
            return error;
          }

          List<ConnectionSessionPacket> runtimeStates(int from) => ble.emitted
              .skip(from)
              .where((p) => p.channel == 'connection_session')
              .map(
                (p) =>
                    ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes)),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (p) =>
                    p.kind == ConnectionSessionKind.state &&
                    p.code == ConnectionSession.litchcraftStateCode,
              )
              .toList();
          final initialStates = runtimeStates(0);
          expect(initialStates, isNotEmpty);
          final playPacket = ble.emitted.lastWhere((p) {
            if (p.channel != 'connection_session') return false;
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(p.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.state &&
                decoded?.code == ConnectionSession.litchcraftStateCode &&
                decoded!.body[4] & 2 != 0;
          });
          final initialSequence = initialStates.last.sequence;
          final firstObservation = ble.emitted.length;
          final writesBefore = ble.writes.length;
          var elapsed = 0;
          var lastSequence = initialSequence;
          while (elapsed < 300000) {
            final minutePhase = elapsed % 60000;
            var step = minutePhase < 200 || minutePhase >= 59800 ? 10 : 1000;
            if (minutePhase < 59800) {
              step = step.clamp(1, 59800 - minutePhase);
            }
            step = step.clamp(1, 30000 - elapsed % 30000);
            final next = (elapsed + step).clamp(0, 300000);
            phoneNow = startPhone + next + next * driftPpm ~/ 1000000;
            final before = ble.emitted.length;
            await ble.command(tester, {'op': 'advance', 'ms': next - elapsed});
            await tester.pump(Duration(milliseconds: next - elapsed));
            for (final state in runtimeStates(before)) {
              expect(state.sequence, greaterThan(lastSequence));
              expect(state.body[4] & 3, 3);
              lastSequence = state.sequence;
              periodicSamples++;
            }
            elapsed = next;
            if (elapsed % 30000 != 0) continue;
            final frame = await journey.frame();
            final metadata =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }))['store_frame']
                    as Map<String, dynamic>;
            expect(frame['now_ms'], nativeStart + elapsed);
            final index = metadata['selected_frame'] as int;
            final localPhase = metadata['frame_elapsed_ms'] as int;
            final nativePhase =
                litchcraftAnimationFrameStartOrNull(document, index)! +
                localPhase;
            final appElapsed = controller.animationElapsedMilliseconds;
            final error = phaseError(appElapsed, nativePhase).abs();
            maximumError = error > maximumError ? error : maximumError;
            debugPrint(
              'PLAYBACK_CLOCK ppm=$driftPpm t=$elapsed '
              'nativePhase=$nativePhase appPhase=${appElapsed % cycle} error=$error '
              'nativeFrame=$index appFrame=${controller.animationSelectedFrameIndex} '
              'freshSamples=$periodicSamples',
            );
            expect(
              error,
              lessThanOrEqualTo(12),
              reason:
                  'Actual native phase must stay bounded through five minutes',
            );
            expect(
              metadata['root_generation'],
              startMetadata['root_generation'],
            );
            expect(metadata['live_revision'], startMetadata['live_revision']);
            if (localPhase > 20 &&
                document.frames[index].durationMs - localPhase > 20) {
              expect(controller.animationSelectedFrameIndex, index);
              final color = document.frames[index].emitters.first.color;
              final canvas = tester
                  .widget<LitchcraftCanvasView>(
                    find.byType(LitchcraftCanvasView),
                  )
                  .canvas;
              expect(canvas.colors, everyElement(color));
              expect(
                (frame['board'] as List).first,
                everyElement([color >> 16, color >> 8 & 255, color & 255, 0]),
              );
            }
            expect(controller.pending, isFalse);
          }
          expect(periodicSamples, inInclusiveRange(4, 5));
          expect(runtimeStates(firstObservation), hasLength(periodicSamples));
          expect(
            ble.writes.skip(writesBefore).where((write) {
              if (write['channel'] != 'connection_session') return true;
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind != ConnectionSessionKind.control ||
                  packet?.code != ConnectionSessionControl.heartbeat;
            }),
            isEmpty,
            reason:
                'Only existing session heartbeats are allowed; clock samples must not create artwork transfers or queries',
          );
          // Replay genuine retained Play after minutes of periodic correction.
          final beforeReplay = controller.animationElapsedMilliseconds;
          ble.deliver(playPacket);
          await tester.pump();
          expect(controller.animationElapsedMilliseconds, beforeReplay);
          await journey.finish(controller.toggleAnimationPlayback());
          await ble.flush(tester);
          final paused = controller.animationElapsedMilliseconds;
          ble.deliver(playPacket);
          await tester.pump();
          expect(controller.animationPlaying, isFalse);
          expect(controller.animationElapsedMilliseconds, paused);
          await journey.finish(controller.selectAnimationFrame(211));
          expect(controller.animationSelectedFrameIndex, 211);
          expect(
            controller.animationElapsedMilliseconds,
            litchcraftAnimationFrameStartOrNull(document, 211),
          );
          final oldPacket = playPacket;
          await journey.finish(service.disconnect());
          await journey.finish(service.connect(_boardId));
          await journey.finish(controller.enter());
          ble.deliver(oldPacket, lateCallback: true);
          await tester.pump();
          expect(controller.animationPlaying, isFalse);
          expect(controller.animationSelectedFrameIndex, 211);
          await ble.flush(tester);
          await journey.finish(library.flush());
        } finally {
          await disposePhone();
        }
        debugPrint(
          'PLAYBACK_CLOCK COMPLETE ppm=$driftPpm maxError=$maximumError '
          'samples=$periodicSamples nonzeroSeek/delayedPlay/staleState/Pause/reconnect=PASS',
        );
      },
    );
  }

  for (final recovery in ['drain', 'disconnect', 'deadline']) {
    testWidgets('Litchcraft: eight Add draw intents under cache pressure $recovery', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
      final service = LitchBleService(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
        receivedHeadCompletionTimeout: recovery == 'deadline'
            ? const Duration(seconds: 4)
            : LitchBleService.orderedHeadCompletionTimeout,
      );
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await journey.finish(service.dispose(), budgetMs: 1000);
        await ble.flush(tester);
        await tester.runAsync(ble.close);
        await tester.runAsync(native.close);
        await tester.pump();
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(library.loadRickroll());
      await journey.finish(controller.selectAnimationFrame(15));
      await journey.finish(
        controller.setBrush(color: 0xff7430, targetMask: 15),
      );
      final expected = LitchcraftDocumentSession(
        controller.documentSession.document,
      )..selectFrame(15);
      addTearDown(expected.dispose);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              monotonicMilliseconds: () => journey.now,
            ),
          ),
        ),
      );
      await tester.pump();
      await ble.command(tester, {'op': 'store_worker', 'enabled': false});
      final writesFrom = ble.writes.length;
      Iterable<ConnectionSessionPacket> ordered() => ble.writes
          .skip(writesFrom)
          .where((w) => w['channel'] == 'connection_session')
          .map(
            (w) => ConnectionSessionCodec.decode(
              Uint8List.fromList(w['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((p) => p.kind == ConnectionSessionKind.ordered);
      Future<Map<String, dynamic>> stored() async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }, flushOperations: false))['store_frame']
              as Map<String, dynamic>;
      for (var insert = 0; insert < 8; ++insert) {
        final add = find.byKey(const ValueKey('craft-add-frame'));
        if (add.hitTestable().evaluate().isEmpty) {
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 200));
        }
        await tester.ensureVisible(add);
        await tester.tap(add);
        await tester.pump();
        expected.addFrame();
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        Offset at(int col) =>
            rect.topLeft +
            Offset(
              rect.width * (col + .5) / 8,
              rect.height * (insert + .5) / 8,
            );
        final finger = await tester.startGesture(at(1));
        await finger.moveTo(at(2));
        await tester.pump(const Duration(milliseconds: 16));
        await finger.up();
        await tester.pump();
        expected.paintSelectedFrame(
          squares: [1 + 8 * insert, 2 + 8 * insert],
          mask: 15,
          color: 0xff7430,
        );
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
          reason:
              'Every captured pointer drag must retain its exact optimistic frame, including beyond cache capacity',
        );
        if (insert < 3) {
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 1000,
            reason:
                'The first three pinned copies and draws are Applied before the real pager worker runs',
          );
        }
        if (insert == 0) {
          for (final frame in [15, 16]) {
            final tile = find.byKey(ValueKey('craft-select-frame-$frame'));
            await tester.ensureVisible(tile);
            await tester.tap(tile);
            await tester.pump();
            expected.selectFrame(frame);
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 1000,
            );
          }
        }
      }
      Stopwatch? receivedHeadWaiting;
      ConnectionSessionPacket? retainedHead;
      var observedPackets = ble.emitted.length;
      var repliesFrom = observedPackets;
      var sameHeadPending = true;
      var pendingBeyondSilence = false;
      Future<void> observeReceivedHead() async {
        // Complete delivery before observing the genuine retained-head receipt.
        // Sampling after delivery gives a conservative elapsed lower bound.
        await tester.pump();
        final adds = ordered().where((packet) => packet.code == 10).toList();
        if (adds.length < 4) return;
        final head = adds[3];
        for (var index = observedPackets; index < ble.emitted.length; ++index) {
          final packet = ble.emitted[index];
          final progress = packet.channel == 'connection_session'
              ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
              : null;
          if (progress?.kind != ConnectionSessionKind.control ||
              progress!.token != head.token) {
            continue;
          }
          if (progress.code == ConnectionSessionControl.progress) {
            final retained =
                progress.sequence == head.sequence - 1 &&
                ByteData.sublistView(
                          progress.body,
                        ).getUint16(8, Endian.little) &
                        1 !=
                    0;
            if (receivedHeadWaiting == null && retained) {
              receivedHeadWaiting = Stopwatch()..start();
              retainedHead = head;
              repliesFrom = index + 1;
            } else if (receivedHeadWaiting != null && !retained) {
              sameHeadPending = false;
            }
          } else if (receivedHeadWaiting != null &&
              (progress.code == ConnectionSessionControl.checkpoint &&
                      progress.sequence >= head.sequence ||
                  progress.code == ConnectionSessionControl.outcome &&
                      progress.sequence == head.sequence)) {
            sameHeadPending = false;
          }
        }
        observedPackets = ble.emitted.length;
        if (receivedHeadWaiting != null) {
          sameHeadPending =
              sameHeadPending &&
              controller.pending &&
              retainedHead!.token == head.token &&
              retainedHead!.sequence == head.sequence;
          if (sameHeadPending &&
              receivedHeadWaiting!.elapsed >
                  const Duration(milliseconds: 2100)) {
            pendingBeyondSilence = true;
          }
        }
      }

      if (recovery == 'deadline') {
        // Observe the fourth Add's first retained Progress before draining
        // later SDK work; a slow host cannot move this observation to the end.
        for (var pass = 0; pass < 200; ++pass) {
          await ble.flush(tester, maxOperations: 1);
          await observeReceivedHead();
          if (!ble.hasPendingOperations) break;
        }
        expect(ble.hasPendingOperations, isFalse);
        expect(receivedHeadWaiting, isNotNull);
      } else {
        await ble.flush(tester);
      }
      await ble.advance(tester, 600);
      if (recovery == 'deadline') await observeReceivedHead();
      final held = await stored();
      expect(held['frame_count'], 32);
      expect(held['dirty'], isTrue);
      expect(
        held['structural_pending'],
        isTrue,
        reason:
            'The fourth actual native Add must be waiting for an existing pinned slot',
      );
      expect(ordered().where((p) => p.code == 10), hasLength(4));
      final revisionHeld = held['live_revision'];
      await ble.advance(tester, 1000);
      if (recovery == 'deadline') await observeReceivedHead();
      expect(
        (await stored())['live_revision'],
        revisionHeld,
        reason: 'Pending fourth Add must not partially advance live revision',
      );
      expect(controller.pending, isTrue);
      expect(controller.documentSession.document.frames, hasLength(40));
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      expect(
        find.byKey(const ValueKey('craft-resolve-conflict')),
        findsNothing,
      );
      debugPrint(
        'CACHE_PRESSURE branch=$recovery first3=Applied fourth=nativePending app_frames=40 saved_frames=32 revision=$revisionHeld next=ReleaseOrRecover',
      );
      if (recovery == 'deadline') {
        // Step native time and Flutter timers; queries cannot extend
        // the existing head deadline. No injected Busy/result or new retries.
        for (var step = 0; step < 36; ++step) {
          await ble.advance(tester, 1000);
          await observeReceivedHead();
        }
        // Keep the real peer responsive while the same shared-session owner
        // expires its shortened test-instance Stopwatch deadline.
        expect(
          LitchBleService.orderedHeadCompletionTimeout,
          const Duration(seconds: 20),
        );
        expect(
          LitchBleService.orderedOperationTimeout,
          const Duration(seconds: 30),
        );
        for (var step = 0; step < 25 && controller.pending; ++step) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 200)),
          );
          await ble.advance(tester, 200);
          await observeReceivedHead();
        }
        expect(
          pendingBeyondSilence,
          isTrue,
          reason:
              'Real peer replies keep the blocked head alive beyond silence recovery',
        );
        expect(
          ble.emitted
              .skip(repliesFrom)
              .where((packet) => packet.channel == 'connection_session')
              .map(
                (packet) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(packet.bytes),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (packet) =>
                    packet.kind == ConnectionSessionKind.control &&
                    (packet.code == ConnectionSessionControl.progress ||
                        packet.code == ConnectionSessionControl.checkpoint),
              ),
          isNotEmpty,
          reason:
              'The genuine native peer must answer while storage remains held',
        );
        await journey.until(
          () => !controller.pending,
          budgetMs: 5000,
          reason: 'The real received-head deadline must end the blocked action',
        );
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
          reason:
              'Failure beyond the existing deadline retains all eight local frames and marks',
        );
        expect(
          controller.pending,
          isFalse,
          reason: 'The existing deadline must terminate the blocked operation',
        );
        expect(
          controller.errorMessage,
          contains('received-head completion deadline'),
          reason:
              'Responsive Query replies must not turn this into silence recovery',
        );
        debugPrint(
          'CACHE_PRESSURE_DEADLINE status=${controller.status.name} error=${controller.errorMessage} draft_frames=40',
        );
      }
      if (recovery != 'drain') {
        final draft = controller.documentSession.document;
        await journey.finish(service.disconnect());
        final retiredWrites = ble.writes.length;
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        await ble.advance(tester, 6000);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, draft),
          isTrue,
        );
        expect(
          ble.writes.skip(retiredWrites).where((w) {
            if (w['channel'] != 'connection_session') return false;
            final p = ConnectionSessionCodec.decode(
              Uint8List.fromList(w['bytes'] as List<int>),
            );
            return p?.kind == ConnectionSessionKind.ordered &&
                (p?.code == 10 || p?.code == 3);
          }),
          isEmpty,
          reason:
              'Retired fourth Add and dependent paint must never write after departure',
        );
        await journey.finish(service.connect(_boardId));
        await journey.until(
          () =>
              (controller.status == LitchcraftStatus.conflict ||
                  controller.status == LitchcraftStatus.live) &&
              !controller.authorityResolving &&
              !controller.pending &&
              !controller.boardArtworkUnresolved &&
              controller.fullAnimationSyncDirection == null &&
              !ble.hasPendingOperations,
          budgetMs: 15000,
          reason:
              'Reconnect must finish authoritative artwork reconciliation',
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, draft),
          isTrue,
          reason:
              'Reconnect cannot silently replace the unconfirmed retained phone draft',
        );
        debugPrint(
          'CACHE_PRESSURE_RECOVERY branch=$recovery status=${controller.status.name} draft_frames=40 retired_writes=0 unresolved=${controller.boardArtworkUnresolved}',
        );
        if (controller.status == LitchcraftStatus.conflict) {
          await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.pending,
            budgetMs: 15000,
            reason:
                'The mounted artwork choice must restore the retained phone draft',
          );
        } else {
          // Fresh clean authority and the selected resident can prove the
          // confirmed common root, allowing ordinary retained-tail delivery.
          expect(controller.documentSession.hasCommonDocument, isTrue);
          expect(controller.documentSession.boardDocument, isNotNull);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.boardDocument!,
              draft,
            ),
            isTrue,
            reason:
                'Automatic recovery must confirm the complete retained draft',
          );
          expect(
            find.byKey(const ValueKey('craft-resolve-conflict')),
            findsNothing,
          );
        }
      } else {
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 15000,
          reason:
              'Existing ordered head and captured phone mutation tail drain all eight intents within their unchanged deadlines',
        );
        expect(
          ordered().where((p) => p.code == 10).map((p) => p.body[1]),
          List.generate(8, (i) => 15 + i),
          reason:
              'Each of the eight Add intents reaches the native board exactly once and in order',
        );
      }
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          expected.document,
        ),
        isTrue,
      );
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      await _expectCleanStoreParity(journey, controller);
      debugPrint(
        'CACHE_PRESSURE_PASS branch=$recovery all8_exact=true saved_frames=40 full_field_parity=true',
      );
      await journey.finish(service.disconnect());
    });
  }

  for (final scenario in [
    (source: 0, mixed: false, departed: false),
    (source: 15, mixed: false, departed: false),
    (source: 31, mixed: false, departed: false),
    (source: 15, mixed: true, departed: false),
    (source: 15, mixed: true, departed: true),
  ]) {
    final source = scenario.source;
    final mixed = scenario.mixed;
    final departed = scenario.departed;
    final exactChain = source == 15 && !mixed && !departed;
    testWidgets(
      'Litchcraft: RR ${exactChain
          ? 'Add draw x3 Undo Delete Add'
          : mixed
          ? 'mixed Add draw Add Add'
          : 'rapid Add draw chain'} at $source ${departed ? 'disconnect preserves draft' : 'preserves every frame'}',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
        final service = LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(
          service,
          monotonicMilliseconds: () => journey.now,
        );
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await ble.command(tester, {'op': 'store_worker', 'enabled': true});
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await journey.finish(service.dispose(), budgetMs: 1000);
          await ble.flush(tester);
          await tester.runAsync(ble.close);
          await tester.runAsync(native.close);
          await tester.pump();
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await journey.finish(library.loadRickroll());
        await journey.finish(controller.selectAnimationFrame(source));
        await journey.finish(
          controller.setBrush(color: 0xff7430, targetMask: 15),
        );
        await ble.flush(tester);
        final original = controller.documentSession.document;
        final expected = LitchcraftDocumentSession(original)
          ..selectFrame(source);
        addTearDown(expected.dispose);
        final screenStarted = journey.now;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                monotonicMilliseconds: () => journey.now,
              ),
            ),
          ),
        );
        await tester.pump();
        await ble.command(tester, {'op': 'store_worker', 'enabled': false});
        final writesFrom = ble.writes.length;
        final colors = mixed
            ? const [0x00aaff, 0x00aaff, 0x00aaff]
            : const [0xff7430, 0x54ff00, 0x00aaff];
        final chainStarted = journey.now;
        LitchcraftAnimationDocument? thirdDrawPreimage;
        void expectImmediatePreview(String action, int started) {
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              expected.document,
            ),
            isTrue,
            reason: '$action must update the complete local document next pump',
          );
          expect(
            controller.documentSession.selectedFrameIndex,
            expected.selectedFrameIndex,
          );
          final composed = composeLitchcraftAnimationDocument(
            expected.document,
            litchcraftAnimationFrameStartOrNull(
              expected.document,
              expected.selectedFrameIndex,
            )!,
            effectElapsedMs:
                controller.animationEffectElapsedMilliseconds ??
                journey.now - screenStarted,
          );
          expect(composed.frameIndex, expected.selectedFrameIndex);
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .canvas
                .colors,
            composed.colors,
            reason: '$action must render its selected frame next pump',
          );
          expect(journey.now - started, lessThanOrEqualTo(16));
          for (final key in [
            'craft-sync-activity',
            'craft-full-animation-sync',
            'craft-resolve-conflict',
          ]) {
            expect(find.byKey(ValueKey(key)), findsNothing);
          }
          expect(find.text('Choose artwork'), findsNothing);
          expect(find.byIcon(Icons.tips_and_updates_outlined), findsNothing);
          debugPrint(
            'RR_EXACT_CHAIN action=$action t=${journey.now - chainStarted}ms '
            'preview=${journey.now - started}ms '
            'frames=${expected.document.frames.length} selected=${expected.selectedFrameIndex} '
            'worker=held pending=${controller.pending} next=ContinueGesture',
          );
        }

        for (var insert = 0; insert < 3; insert++) {
          final add = find.byKey(const ValueKey('craft-add-frame'));
          if (add.hitTestable().evaluate().isEmpty) {
            await tester.tap(find.byKey(const ValueKey('craft-animate')));
            await tester.pump(const Duration(milliseconds: 200));
          }
          await tester.ensureVisible(add);
          final addStarted = journey.now;
          await tester.tap(add);
          await tester.pump();
          expected.addFrame();
          expect(
            controller.documentSession.document.frames,
            hasLength(33 + insert),
          );
          expect(
            controller.documentSession.selectedFrameIndex,
            source + insert + 1,
          );
          if (exactChain) {
            expectImmediatePreview('Add${insert + 1}', addStarted);
          }
          if (insert == 0) {
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 500,
              reason:
                  'Middle Add receives Applied while durable storage is held',
            );
            expect(
              controller.status,
              LitchcraftStatus.live,
              reason:
                  'An exact live receipt does not wait for a clean saved root',
            );
            await ble.flush(tester);
            final held =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': source,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            expect(
              held['structural_pending'],
              isFalse,
              reason:
                  'The native Add has completed its live ordered position before flash publication',
            );
            expect(held['frame_count'], 32);
            expect(held['selected_frame'], source + 1);
            expect(held['dirty'], isTrue);
            final liveState = ble.emitted
                .map(
                  (packet) => packet.channel == 'connection_session'
                      ? ConnectionSessionCodec.decode(
                          Uint8List.fromList(packet.bytes),
                        )
                      : null,
                )
                .whereType<ConnectionSessionPacket>()
                .lastWhere(
                  (packet) =>
                      packet.kind == ConnectionSessionKind.state &&
                      packet.code == ConnectionSession.litchcraftStateCode,
                );
            expect(liveState.body[1], 33);
            expect(liveState.body[0], source + 1);
            debugPrint(
              'RAM_ADD_ACK source=$source live_frames=33 saved_frames=32 worker=held Applied=true',
            );
            if (exactChain) {
              final beforeReconnect = controller.documentSession.document;
              await journey.finish(service.disconnect());
              await journey.finish(service.connect(_boardId));
              await journey.until(
                () =>
                    controller.status == LitchcraftStatus.live &&
                    !controller.pending &&
                    !controller.authorityResolving &&
                    !controller.boardArtworkUnresolved &&
                    tester
                        .widget<LitchcraftCanvasView>(
                          find.byType(LitchcraftCanvasView),
                        )
                        .enabled,
                budgetMs: 15000,
                reason:
                    'Reconnect preserves the accepted live Add while its saved root is held',
              );
              expect(
                sameLitchcraftDocument(
                  controller.documentSession.document,
                  beforeReconnect,
                ),
                isTrue,
              );
              expect(controller.documentSession.selectedFrameIndex, source + 1);
              expect(
                find.byKey(const ValueKey('craft-resolve-conflict')),
                findsNothing,
              );
              expect(
                _litchcraftRequests(
                  ble,
                  writesFrom,
                ).where((p) => p.length > 2 && [18, 19, 21].contains(p[2])),
                isEmpty,
                reason:
                    'Known accepted live identity reconnects without replacing the artwork',
              );
              debugPrint(
                'RAM_ADD_RECONNECT saved_frames=32 live_frames=33 worker=held exact=true full_transfers=0',
              );
            }
          }
          if (mixed && insert > 0) {
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                expected.document,
              ),
              isTrue,
              reason: 'Consecutive copies inherit the earlier pending paint',
            );
            continue;
          }
          controller.selectPaletteColor(colors[insert]).ignore();
          await tester.pump();
          final rect = tester.getRect(find.byType(LitchcraftCanvasView));
          Offset at(int column) =>
              rect.topLeft +
              Offset(
                rect.width * (column + .5) / 8,
                rect.height * (insert + 3.5) / 8,
              );
          if (exactChain && insert == 2) thirdDrawPreimage = expected.document;
          final firstDrawPhysicalBefore = exactChain && insert == 0
              ? (await journey.frame())['physical'] as List
              : null;
          final drawStarted = journey.now;
          final finger = await tester.startGesture(at(3));
          await finger.moveTo(at(4));
          await tester.pump(const Duration(milliseconds: 16));
          await finger.up();
          await tester.pump();
          expected.paintSelectedFrame(
            squares: [27 + insert * 8, 28 + insert * 8],
            mask: 15,
            color: colors[insert],
          );
          final visible = tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas;
          for (final square in [27 + insert * 8, 28 + insert * 8]) {
            expect(
              visible.colors.skip(square * 4).take(4),
              everyElement(colors[insert]),
              reason:
                  'The first rendered pointer preview edits the inserted frame',
            );
          }
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              expected.document,
            ),
            isTrue,
          );
          expect(
            controller.pending,
            isTrue,
            reason:
                'Native publication is held while the next real gesture starts',
          );
          if (exactChain) {
            expectImmediatePreview('Draw${insert + 1}', drawStarted);
            if (insert == 0) {
              await journey.until(
                () => !controller.pending && !ble.hasPendingOperations,
                budgetMs: 500,
                reason:
                    'Add then real pointer paint reaches the board before any save worker service',
              );
              final displayed = await journey.frame();
              for (final square in [27, 28]) {
                expect(
                  (displayed['board'] as List)[square],
                  everyElement([255, 116, 48, 0]),
                  reason:
                      'The board itself displays the first new-frame draw while storage is held',
                );
                final physicalSquare =
                    (displayed['logical_to_physical'] as List)[square] as int;
                final pixels = (displayed['physical'] as List).sublist(
                  physicalSquare * 4,
                  physicalSquare * 4 + 4,
                );
                expect(
                  pixels,
                  isNot(
                    equals(
                      firstDrawPhysicalBefore!.sublist(
                        physicalSquare * 4,
                        physicalSquare * 4 + 4,
                      ),
                    ),
                  ),
                  reason:
                      'The mapped physical LED output changes on the new frame before saving',
                );
                expect(
                  pixels,
                  everyElement(equals(pixels.first)),
                  reason:
                      'All four physical emitters receive the uniform paint',
                );
                expect(
                  (pixels.first as List)[0] as int,
                  greaterThan((pixels.first as List)[1] as int),
                  reason:
                      'The physical output carries the new orange paint, not the previous artwork',
                );
              }
              final unsaved =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': source,
                      }, flushOperations: false))['store_frame']
                      as Map<String, dynamic>;
              expect(unsaved['frame_count'], 32);
              expect(unsaved['dirty'], isTrue);
              expect(unsaved['selected_frame'], source + 1);
              debugPrint(
                'ADD_DRAW_VISIBLE source=$source elapsed_ms=${journey.now - drawStarted} worker=held saved_frames=32 live_frames=33 semantic_and_physical_squares=27,28',
              );
            }
          }
        }
        if (exactChain) {
          expect(controller.documentSession.document.frames, hasLength(35));
          final undo = find.byKey(const ValueKey('craft-undo'));
          expect(tester.widget<IconButton>(undo).onPressed, isNotNull);
          final undoStarted = journey.now;
          await tester.tap(undo);
          await tester.pump();
          expected.undo();
          expect(expected.document.frames, hasLength(35));
          expect(
            sameLitchcraftDocument(expected.document, thirdDrawPreimage!),
            isTrue,
            reason: 'Undo removes only the third draw, retaining its new copy',
          );
          expectImmediatePreview('UndoDraw3', undoStarted);
          expect(controller.pending, isTrue);

          if (find
              .byKey(const ValueKey('craft-add-frame'))
              .hitTestable()
              .evaluate()
              .isEmpty) {
            await tester.tap(find.byKey(const ValueKey('craft-animate')));
            await tester.pump(const Duration(milliseconds: 200));
          }
          final thirdCopy = await _revealFrame(tester, source + 3);
          await tester.longPress(thirdCopy);
          await tester.pump(const Duration(milliseconds: 300));
          final deleteStarted = journey.now;
          await tester.tap(
            find.byKey(const ValueKey('craft-confirm-delete-frame')),
          );
          await tester.pump();
          expected.deleteSelectedFrame();
          expect(expected.document.frames, hasLength(34));
          expect(expected.selectedFrameIndex, source + 2);
          expectImmediatePreview('DeleteCopy3', deleteStarted);
          expect(controller.pending, isTrue);

          // Finish the dialog's exit animation, never the queued Store work.
          await tester.pump(const Duration(milliseconds: 300));
          final add = find.byKey(const ValueKey('craft-add-frame'));
          await tester.ensureVisible(add);
          final addStarted = journey.now;
          await tester.tap(add);
          await tester.pump();
          expected.addFrame();
          expect(expected.document.frames, hasLength(35));
          expect(expected.selectedFrameIndex, source + 3);
          expectImmediatePreview('AddCopy2', addStarted);
          expect(controller.pending, isTrue);
          final complete = controller.documentSession.document;
          final survivingOriginals = LitchcraftAnimationDocument(
            frames: [
              for (var frame = 0; frame < original.frames.length; ++frame)
                complete.frames[frame <= source ? frame : frame + 3],
            ],
            topology: complete.topology,
            favoriteColors: complete.favoriteColors,
            playbackOnEntry: complete.playbackOnEntry,
            sparkleSeed: complete.sparkleSeed,
          );
          expect(
            sameLitchcraftDocument(survivingOriginals, original),
            isTrue,
            reason: 'The chain must retain all 32 original RR frames exactly',
          );
          journey.checkpoint(
            'RR-exact-chain-local-complete',
            await journey.frame(),
            'app_frames=35 selected=${source + 3} worker=held native_add_pending=true '
                'last_draw=undone prior_marks=retained next=ReleaseWorker',
          );
        }
        await ble.advance(tester, 600);
        expect(
          find.byKey(const ValueKey('craft-sync-activity')),
          findsNothing,
          reason: 'Ordinary queued edits do not become artwork synchronization',
        );
        if (departed) {
          expect(
            controller.pending,
            isFalse,
            reason:
                'Every rapid operation has an exact live receipt before departure',
          );
          final beforeSave =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': source + 3,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          expect(beforeSave['frame_count'], 32);
          expect(beforeSave['dirty'], isTrue);
          await journey.finish(service.disconnect());
          final retiredWrites = ble.writes.length;
          await ble.command(tester, {'op': 'store_worker', 'enabled': true});
          await ble.advance(tester, 6000);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              expected.document,
            ),
            isTrue,
            reason: 'Departure preserves every optimistic frame and mark',
          );
          expect(
            ble.writes.skip(retiredWrites).where((write) {
              if (write['channel'] != 'connection_session') return false;
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind == ConnectionSessionKind.ordered &&
                  (packet?.code == 10 || packet?.code == 3);
            }),
            isEmpty,
            reason: 'Retired Canvas waiters cannot submit late Add or paint',
          );
          await journey.finish(service.connect(_boardId));
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.pending &&
                !controller.authorityResolving,
            budgetMs: 15000,
            reason:
                'Reconnect reuses the exact accepted live document after its background save',
          );
          expect(
            find.byKey(const ValueKey('craft-resolve-conflict')),
            findsNothing,
          );
          expect(
            _litchcraftRequests(
              ble,
              writesFrom,
            ).where((p) => p.length > 2 && [18, 19, 21].contains(p[2])),
            isEmpty,
            reason:
                'Accepted edits reconnect without source choice or whole-artwork replacement',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              expected.document,
            ),
            isTrue,
          );
          await _expectCleanStoreParity(journey, controller);
          journey.checkpoint(
            'RR-mixed-disconnect-recovery',
            await journey.frame(),
            'frames=35 retained_draft=true late_edits=0 app_choice_taps=0 full_transfers=0 exact_stored_parity=true next=ContinueDrawing',
          );
          await journey.finish(service.disconnect());
          return;
        }
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        await journey.until(
          () => !controller.pending,
          budgetMs: 10000,
          reason:
              'The captured Add and pointer sequence drains without choosing artwork',
        );
        if (exactChain) {
          final stored =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': source + 3,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          final sent = ble.writes
              .skip(writesFrom)
              .where((write) => write['channel'] == 'connection_session')
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where((packet) => packet.kind == ConnectionSessionKind.ordered)
              .map(
                (packet) => {
                  'opcode': packet.code,
                  'body': packet.body.toList(),
                },
              )
              .toList();
          debugPrint(
            'RR_EXACT_CHAIN_RESULT ${jsonEncode({'t_ms': journey.now - chainStarted, 'status': controller.status.name, 'error': controller.errorMessage, 'app_frames': controller.documentSession.document.frames.length, 'app_selected': controller.documentSession.selectedFrameIndex, 'native_frames': stored['frame_count'], 'native_selected': stored['selected_frame'], 'native_revision': stored['live_revision'], 'native_dirty': stored['dirty'], 'native_structural_pending': stored['structural_pending'], 'ordered': sent})}',
          );
        }
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        expect(
          find.byKey(const ValueKey('craft-resolve-conflict')),
          findsNothing,
        );
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
        final order = ble.writes
            .skip(writesFrom)
            .where((w) => w['channel'] == 'connection_session')
            .map(
              (w) => ConnectionSessionCodec.decode(
                Uint8List.fromList(w['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where((p) => p.kind == ConnectionSessionKind.ordered)
            .where(
              (p) =>
                  p.code == 10 ||
                  (exactChain &&
                      p.code == ConnectionSession.litchcraftUndoOpcode) ||
                  (p.code == 3 && p.body.length > 8),
            )
            .map(
              (p) => p.code == 10
                  ? '${p.body[0] == 0 ? 'Add' : 'Delete'}:${p.body[1]}'
                  : p.code == ConnectionSession.litchcraftUndoOpcode
                  ? 'Undo'
                  : 'Paint:${p.body[0]}',
            )
            .toList();
        expect(order, [
          for (var i = 0; i < 3; i++) ...[
            'Add:${source + i}',
            if (!mixed || i == 0) 'Paint:${source + i + 1}',
          ],
          if (exactChain) ...[
            'Undo',
            'Delete:${source + 3}',
            'Add:${source + 2}',
          ],
        ]);
        debugPrint('RR_CHAIN_WIRE source=$source mixed=$mixed actions=$order');
        final requests = _litchcraftRequests(ble, writesFrom);
        expect(
          requests.where((p) => p.length > 2 && [18, 19, 21].contains(p[2])),
          isEmpty,
          reason:
              'Ordinary frame operations must not transfer the artwork body',
        );
        final paintedBoard = await journey.frame();
        expect(
          (paintedBoard['board'] as List)[mixed
              ? 27
              : exactChain
              ? 35
              : 43],
          everyElement(exactChain ? [84, 255, 0, 0] : [0, 170, 255, 0]),
          reason: exactChain
              ? 'Native LEDs retain the preceding green mark after Undo Delete Add'
              : 'Native LEDs show the last inserted frame and its blue mark',
        );
        journey.checkpoint(
          'RR-${exactChain
              ? 'exact-AddDraw3-Undo-Delete-Add'
              : mixed
              ? 'mixed'
              : 'rapid'}-add-draw-$source',
          paintedBoard,
          'frames=35 selected=${source + 3} exact_stored_parity=true full_transfers=0 next=ContinueDrawing',
        );
        if (mixed) {
          await journey.finish(service.disconnect());
          return;
        }
        await ble.command(tester, {'op': 'store_worker', 'enabled': false});
        if (find
            .byKey(const ValueKey('craft-add-frame'))
            .hitTestable()
            .evaluate()
            .isEmpty) {
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 200));
        }
        final deleted = await _revealFrame(tester, source + 3);
        await tester.longPress(deleted);
        await tester.pump(const Duration(milliseconds: 300));
        await tester.tap(
          find.byKey(const ValueKey('craft-confirm-delete-frame')),
        );
        await tester.pump(const Duration(milliseconds: 300));
        expected.deleteSelectedFrame();
        final selected = source + 2;
        expect(controller.documentSession.selectedFrameIndex, selected);
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        final finger = await tester.startGesture(
          rect.topLeft + Offset(rect.width * 3.5 / 8, rect.height * 6.5 / 8),
        );
        await finger.moveTo(
          rect.topLeft + Offset(rect.width * 4.5 / 8, rect.height * 6.5 / 8),
        );
        await tester.pump(const Duration(milliseconds: 16));
        await finger.up();
        await tester.pump();
        expected.paintSelectedFrame(
          squares: [51, 52],
          mask: 15,
          color: colors.last,
        );
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
        );
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        await journey.until(() => !controller.pending, budgetMs: 10000);
        expect(controller.status, LitchcraftStatus.live);
        await _expectCleanStoreParity(journey, controller);
        expect(
          _litchcraftRequests(
            ble,
            writesFrom,
          ).where((p) => p.length > 2 && [18, 19, 21].contains(p[2])),
          isEmpty,
        );
        final deletedBoard = await journey.frame();
        expect(
          (deletedBoard['board'] as List)[51],
          everyElement([0, 170, 255, 0]),
          reason:
              'Native LEDs show paint on the preceding survivor after Delete',
        );
        journey.checkpoint(
          'RR-delete-draw-$source',
          deletedBoard,
          'frames=34 selected=$selected exact_stored_parity=true full_transfers=0 next=ContinueDrawing',
        );
        // A captured relative action must not overwrite a physical edit that
        // happened while its dialog/slider was open. Exercise the real
        // refusal producer, then resolve that Store conflict once in the UI.
        if (source != 0) {
          final target = controller.captureAnimationFrameEditTarget()!;
          await journey.hall(BigInt.one << 56);
          await ble.advance(tester, 80);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 6000);
          await journey.until(() => !controller.pending, budgetMs: 10000);
          expect(
            controller
                .documentSession
                .document
                .frames[selected]
                .emitters[56 * 4]
                .color,
            colors.last,
          );
          final physical = controller.documentSession.document;
          await journey.finish(
            controller.setAnimationFrameDuration(250, target: target),
          );
          expect(controller.status, LitchcraftStatus.conflict);
          expect(
            controller.frameStoreArtworkReadPending,
            isFalse,
            reason:
                'The refused relative edit is the production bare-conflict path',
          );
          final keptDraft = controller.documentSession.document;
          expect(keptDraft.frames[selected].durationMs, 250);
          final choiceWrites = ble.writes.length;
          await tester.tap(
            find.byKey(const ValueKey('craft-resolve-conflict')),
          );
          await tester.pumpAndSettle();
          final useBoard = source == 31;
          await tester.tap(
            find.byKey(
              ValueKey(
                useBoard ? 'craft-use-board-artwork' : 'craft-use-app-artwork',
              ),
            ),
          );
          await tester.pump();
          if (!useBoard) {
            await journey.until(
              () =>
                  controller.fullAnimationSyncDirection ==
                  LitchcraftFullSyncDirection.sendingToBoard,
              budgetMs: 5000,
            );
            // Leave the real SDK transfer pending while its notice deadline
            // expires, then prove that progress replaces the resolved choice.
            await tester.pump(const Duration(milliseconds: 600));
            await tester.pump();
            expect(
              find.byKey(const ValueKey('craft-full-animation-sync')),
              findsOneWidget,
            );
            expect(
              find.byKey(const ValueKey('craft-resolve-conflict')),
              findsNothing,
            );
          }
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.pending,
            budgetMs: 30000,
            reason: 'One source choice must verify the chosen Store artwork',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              useBoard ? physical : keptDraft,
            ),
            isTrue,
          );
          if (useBoard) {
            expect(
              sameLitchcraftDocument(
                library.previousDraft!.document,
                keptDraft,
              ),
              isTrue,
              reason: 'Choosing Litch preserves the complete refused app draft',
            );
          }
          await _expectCleanStoreParity(journey, controller);
          expect(
            _litchcraftRequests(
              ble,
              choiceWrites,
            ).where((p) => p.length > 2 && p[2] == 18),
            hasLength(useBoard ? 0 : 1),
          );
          journey.checkpoint(
            'RR-relative-refusal-choice-$source',
            await journey.frame(),
            'choice=${useBoard ? 'Litch' : 'app'} taps=1 verified_parity=true next=ContinueDrawing',
          );
        }
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final drawSource in ['app', 'Hall']) {
    for (final undoTiming in [
      'after copy',
      'during copy',
      'slow history',
      'foreign before guard',
    ]) {
      testWidgets(
        'Litchcraft: RR frame32 copy $drawSource paint copy Undo $undoTiming preserves painted source',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final native = (await tester.runAsync(
            () => VirtualLitchProcess.start(1, false, hardwareV3: true),
          ))!;
          final ble = _DelayedFrameEditGattBle(native, operationIntervalMs: 20);
          final service = LitchBleService(
            ble: ble,
            gattSessionFactory: ReactiveBleGattSessionFactory(ble),
            pendingOtaStore: MemoryOtaTargetStore(),
          );
          final journey = _Journey(tester, ble, service);
          final probe = _StoreHistoryProbe(clock: () => journey.now);
          final controller = LitchcraftController(
            service,
            monotonicMilliseconds: () => journey.now,
            appTrace: probe,
          );
          final library = LitchcraftLibrary(controller);
          addTearDown(() async {
            await ble.command(tester, {'op': 'store_worker', 'enabled': true});
            ble.holdOpcode = null;
            for (final packet in ble.heldCheckpoints) {
              ble.deliver(packet);
            }
            await tester.pumpWidget(const SizedBox.shrink());
            library.dispose();
            controller.dispose();
            await journey.finish(service.dispose(), budgetMs: 1000);
            await ble.flush(tester);
            await tester.runAsync(ble.close);
            await tester.runAsync(native.close);
            await tester.pump();
          });
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 1000);
          await journey.finish(service.connect(_boardId));
          await journey.finish(library.ready);
          await journey.finish(controller.enter());
          await journey.finish(library.loadRickroll());
          await journey.finish(controller.selectAnimationFrame(31));
          await journey.finish(
            controller.setBrush(color: 0x13579b, targetMask: 15),
          );
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                  monotonicMilliseconds: () => journey.now,
                ),
              ),
            ),
          );
          await tester.pump();
          expect(controller.documentSession.document.frames, hasLength(32));
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 200));
          final firstAdd = find.byKey(const ValueKey('craft-add-frame'));
          await tester.ensureVisible(firstAdd);
          await tester.tap(firstAdd);
          await tester.pump();
          await journey.until(() => !controller.pending, budgetMs: 10000);
          expect(controller.documentSession.selectedFrameIndex, 32);
          if (drawSource == 'app') {
            final rect = tester.getRect(find.byType(LitchcraftCanvasView));
            final at =
                rect.topLeft +
                Offset(rect.width * 3.5 / 8, rect.height * 3.5 / 8);
            final finger = await tester.startGesture(at);
            await tester.pump(const Duration(milliseconds: 16));
            await finger.up();
            await journey.until(() => !controller.pending, budgetMs: 10000);
          } else {
            await journey.hall(BigInt.one << 27);
            await ble.advance(tester, 50);
            await journey.hall(BigInt.zero);
            await ble.advance(tester, 400);
            await journey.until(() => !controller.pending, budgetMs: 10000);
          }
          final painted = controller.documentSession.document;
          expect(painted.frames, hasLength(33));
          expect(painted.frames[32].emitters[27 * 4].color, 0x13579b);
          final beforeSecondCopy = await journey.frame();
          final paintedSquare =
              (beforeSecondCopy['board'] as List<dynamic>)[27];
          final from = ble.writes.length;
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 200));
          final add = find.byKey(const ValueKey('craft-add-frame'));
          await tester.ensureVisible(add);
          await tester.tap(add);
          await tester.pump();
          if (undoTiming != 'during copy') {
            await journey.until(() => !controller.pending, budgetMs: 10000);
          }
          expect(controller.documentSession.document.frames, hasLength(34));
          expect(controller.documentSession.selectedFrameIndex, 33);
          if (undoTiming == 'foreign before guard') {
            await ble.flush(tester);
          }
          if (undoTiming == 'slow history') {
            await ble.command(tester, {'op': 'store_worker', 'enabled': false});
          }
          final start = journey.now;
          final undoWritesFrom = ble.writes.length;
          final undoButton = find.byKey(const ValueKey('craft-undo'));
          expect(tester.widget<IconButton>(undoButton).onPressed, isNotNull);
          await tester.tap(undoButton);
          await tester.pump();
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              painted,
            ),
            isTrue,
            reason:
                'Undo immediately restores the painted source, never the older unpainted frame',
          );
          expect(controller.documentSession.selectedFrameIndex, 32);
          if (undoTiming == 'foreign before guard') {
            int? capturedGuardRevision;
            bool guardSubmitted() =>
                ble.writes.skip(undoWritesFrom).any((write) {
                  if (write['channel'] != 'connection_session') return false;
                  final packet = ConnectionSessionCodec.decode(
                    Uint8List.fromList(write['bytes'] as List<int>),
                  );
                  if (packet?.kind != ConnectionSessionKind.ordered ||
                      packet?.code !=
                          ConnectionSession.litchcraftStoreGuardOpcode) {
                    return false;
                  }
                  capturedGuardRevision = ByteData.sublistView(
                    packet!.body,
                  ).getUint32(4, Endian.little);
                  return true;
                });
            for (var turn = 0; turn < 100 && !guardSubmitted(); ++turn) {
              await tester.pump(const Duration(milliseconds: 1));
            }
            expect(
              guardSubmitted(),
              isTrue,
              reason: 'Capture real Guard before forwarding its SDK write',
            );
            await journey.hall(BigInt.one << 28);
            await ble.command(tester, {
              'op': 'advance',
              'ms': 50,
            }, flushOperations: false);
            await journey.hall(BigInt.zero);
            await ble.command(tester, {
              'op': 'advance',
              'ms': 400,
            }, flushOperations: false);
            final advanced =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 33,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            expect(
              advanced['live_revision'],
              greaterThan(capturedGuardRevision!),
              reason:
                  'Real native paint invalidates the captured Guard before SDK forwarding',
            );
            await journey.until(
              () =>
                  !controller.pending &&
                  controller.status == LitchcraftStatus.live &&
                  controller.documentSession.document.frames.length == 34,
              budgetMs: 10000,
              reason:
                  'Refused structural Undo repairs the unchanged layout with the competing physical edit',
            );
            expect(
              probe.events.any(
                (event) =>
                    event.event == 'litchcraft.store_history_receipt' &&
                    event.fields['guard_applied'] == false &&
                    event.fields['history_applied'] == false,
              ),
              isTrue,
            );
            expect(controller.documentSession.selectedFrameIndex, 33);
            await tester.pump();
            expect(
              tester
                  .widget<LitchcraftCanvasView>(
                    find.byType(LitchcraftCanvasView),
                  )
                  .canvas
                  .colors[28 * 4],
              0x13579b,
              reason:
                  'Recovery restores selection and preview anchor to the physical edit',
            );
            await journey.finish(
              controller.setBrush(color: 0x2468ac, targetMask: 15),
            );
            final rect = tester.getRect(find.byType(LitchcraftCanvasView));
            final finger = await tester.startGesture(
              rect.topLeft +
                  Offset(rect.width * 5.5 / 8, rect.height * 3.5 / 8),
            );
            await finger.up();
            await journey.until(() => !controller.pending, budgetMs: 10000);
            expect(
              controller
                  .documentSession
                  .document
                  .frames[33]
                  .emitters[29 * 4]
                  .color,
              0x2468ac,
              reason:
                  'The next app gesture uses the restored physical frame, not the rejected inverse target',
            );
            await _expectCleanStoreParity(journey, controller);
            journey.checkpoint(
              'RR refused structural Undo recovery',
              await journey.frame(),
              'guard=rejected layout=34 selected=33 next-paint=33 accepted=applied',
            );
            await journey.finish(service.disconnect());
            return;
          }
          var samples = 0;
          var workerHeld = undoTiming == 'slow history';
          String? previousCheckpoint;
          do {
            if (workerHeld && journey.now - start >= 500) {
              workerHeld = false;
              await ble.command(tester, {
                'op': 'store_worker',
                'enabled': true,
              });
            }
            expect(
              journey.now - start,
              lessThan(5000),
              reason: 'History must converge after the controlled worker stall',
            );
            final visible = tester.widget<LitchcraftCanvasView>(
              find.byType(LitchcraftCanvasView),
            );
            expect(
              visible.canvas.colors[27 * 4],
              0x13579b,
              reason:
                  'Every rendered Undo turn preserves the painted frame at ${journey.now - start}ms',
            );
            expect(library.selectedFrameIndex, 32);
            final rendered = await journey.frame();
            final square =
                (rendered['board'] as List<dynamic>)[27] as List<dynamic>;
            expect(
              square,
              paintedSquare,
              reason:
                  'Native rendered paint cannot flash an old frame during Undo',
            );
            final checkpoint =
                '${library.selectedFrameIndex}:${controller.pending}:${visible.canvas.colors[27 * 4]}';
            if (checkpoint != previousCheckpoint) {
              debugPrint(
                'RR_COPY_RENDER t=${journey.now - start} frames=${controller.documentSession.document.frames.length} '
                'selected=${library.selectedFrameIndex} pending=${controller.pending} color=${visible.canvas.colors[27 * 4]} native=$square',
              );
              previousCheckpoint = checkpoint;
            }
            await journey.tick();
            ++samples;
          } while (controller.pending || journey.now - start < 500);
          expect(samples, greaterThan(1));
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              painted,
            ),
            isTrue,
            reason:
                'Layout publication and history receipts cannot replace the painted source',
          );
          expect(controller.documentSession.selectedFrameIndex, 32);
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          debugPrint(
            'RR_COPY_UNDO source=$drawSource timing=$undoTiming elapsed=${journey.now - start} '
            'selected=${controller.documentSession.selectedFrameIndex} '
            'frames=${controller.documentSession.document.frames.length} '
            'paint=${controller.canvas.colors[27 * 4]}',
          );
          await _expectCleanStoreParity(journey, controller);
          if (undoTiming != 'during copy') {
            expect(
              _litchcraftRequests(ble, from).where(
                (p) =>
                    _isArtworkBodyRead(p) ||
                    p[0] == LitchcraftV2Protocol.magic &&
                        p[2] == LitchcraftV2Protocol.frameStoreBegin,
              ),
              isEmpty,
              reason:
                  'Known structural Undo must not download or upload artwork',
            );
            expect(
              _litchcraftRequests(ble, from).where(_isLiveLayoutRead),
              isEmpty,
              reason:
                  'Known structural Undo must not fall into live-layout repair',
            );
          }
          journey.checkpoint(
            'RR copy paint copy Undo',
            await journey.frame(),
            'source=$drawSource timing=$undoTiming frames=33 selected=32 accepted=applied next=paint-or-Redo',
          );
          await journey.finish(service.disconnect());
        },
        variant: TargetPlatformVariant({
          TargetPlatform.android,
          TargetPlatform.iOS,
        }),
        skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      );
    }
  }

  testWidgets(
    'Litchcraft: mounted RR duration Undo Redo burst finishes before quiet save',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _DelayedHistoryCheckpointBle(native, operationIntervalMs: 20);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final probe = _StoreHistoryProbe(clock: () => journey.now);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
        appTrace: probe,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(library.loadRickroll());
      await journey.finish(controller.selectAnimationFrame(5));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              monotonicMilliseconds: () => journey.now,
            ),
          ),
        ),
      );
      await ble.flush(tester);
      expect(controller.documentSession.document.frames, hasLength(32));
      final baseline =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 5,
              }, flushOperations: false))['store_frame']
              as Map<String, dynamic>;
      await journey.finish(controller.setAnimationFrameDuration(2000));
      final edited = controller.documentSession.document;
      expect(edited.frames[5].durationMs, 2000);
      final writesBefore = ble.writes.length;
      final start = journey.now;
      ble.holdHistoryCheckpoints = true;
      // Real mounted taps obey the same button eligibility as human spamming.
      // Pump rebuilds between taps, without waiting for a history receipt.
      for (var index = 0; index < 20; index++) {
        final undo = index.isEven;
        final button = find.byKey(ValueKey(undo ? 'craft-undo' : 'craft-redo'));
        await journey.until(
          () => tester.widget<IconButton>(button).onPressed != null,
          budgetMs: 2000 - (journey.now - start),
          reason: 'The next owned duration inverse must remain usable promptly',
        );
        await tester.tap(button);
        await tester.pump();
        if (index == 0) {
          await journey.until(
            () => ble.heldHistoryCheckpoints.isNotEmpty,
            budgetMs: 250,
            reason: 'Native Undo applies before its held terminal checkpoint',
          );
          expect(controller.pending, isTrue);
        }
        expect(
          controller.documentSession.document.frames[5].durationMs,
          undo ? 50 : 2000,
          reason: 'Each accepted tap previews its exact duration immediately',
        );
      }
      ble.holdHistoryCheckpoints = false;
      for (final packet in ble.heldHistoryCheckpoints) {
        ble.deliver(packet);
      }
      await tester.pump();
      await journey.until(
        () =>
            probe.events
                    .where(
                      (entry) =>
                          entry.event == 'litchcraft.store_history_receipt',
                    )
                    .length ==
                20 &&
            !controller.pending &&
            !controller.historyRecoveryPending &&
            !ble.hasPendingOperations,
        budgetMs: 2000 - (journey.now - start),
        reason: 'All 20 history actions must complete before the 5-second save',
      );
      final elapsed = journey.now - start;
      final requests = probe.events
          .where((entry) => entry.event == 'litchcraft.store_history_requested')
          .toList();
      expect(requests, hasLength(20));
      expect(
        requests.every((entry) => entry.fields['known_inverse'] == true),
        isTrue,
      );
      expect(
        probe.events.where(
          (entry) => entry.event == 'litchcraft.store_history_unknown_receipt',
        ),
        isEmpty,
      );
      final live =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 5,
              }, flushOperations: false))['store_frame']
              as Map<String, dynamic>;
      expect(live['root_generation'], baseline['root_generation']);
      expect(live['live_revision'], (baseline['live_revision'] as int) + 21);
      expect(
        live['dirty'],
        isTrue,
        reason: 'Completion must precede durability',
      );
      expect(live['selected_frame'], 5);
      expect(live['playing'], isFalse);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, edited),
        isTrue,
      );
      expect(
        sameLitchcraftDocument(
          controller.documentSession.boardDocument!,
          edited,
        ),
        isTrue,
      );
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.storeReplacementUnresolved, isFalse);
      expect(
        _litchcraftRequests(ble, writesBefore).where(
          (packet) => packet[2] == LitchcraftV2Protocol.frameStoreSnapshot,
        ),
        isEmpty,
        reason: 'Owned history must not reconstruct the saved root',
      );
      // Sample the mounted canvas on its 50 ms effect repaint. A receipt can
      // settle between repaints while the paused frame's backdrop keeps moving.
      await ble.advance(tester, 50);
      final rendered = composeLitchcraftAnimationDocument(
        edited,
        controller.animationElapsedMilliseconds,
        effectElapsedMs: controller.animationEffectElapsedMilliseconds,
      );
      expect(rendered.frameIndex, 5);
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors,
        rendered.colors,
        reason:
            'The RR backdrop breathes while the selected frame stays aligned',
      );
      debugPrint(
        'RR_DURATION_BURST elapsed_ms=$elapsed taps=20 '
        'root=${live['root_generation']} revision=${live['live_revision']} '
        'dirty=${live['dirty']} selected=5 duration=2000 result=live-before-save',
      );
      // Whole stored-content parity is separate from the latency proof above.
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(library.flush());
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    timeout: const Timeout(Duration(minutes: 2)),
  );

  testWidgets(
    'Litchcraft: latest frame tap and draw survive delayed selection State',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
      final document = LitchcraftAnimationDocument(
        frames: [
          for (final entry in <({int color, int duration})>[
            (color: 0xaa0000, duration: 250),
            (color: 0x00bb00, duration: 2000),
            (color: 0x0000cc, duration: 500),
          ])
            LitchcraftAnimationFrame(
              durationMs: entry.duration,
              emitters: List.filled(
                256,
                LitchcraftAnimatedEmitter(color: entry.color),
              ),
            ),
        ],
        topology: blank.topology,
        favoriteColors: blank.favoriteColors,
      );
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(
        library.open(
          LitchcraftArtwork.animated(
            id: 'paused-inspect',
            name: 'Paused Inspect',
            document: document,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          ),
        ),
      );
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              monotonicMilliseconds: () => journey.now,
            ),
          ),
        ),
      );
      await journey.finish(controller.selectAnimationFrame(2));
      await ble.flush(tester);
      Future<void> aligned(String checkpoint, {int elapsed = 2250}) async {
        final frame = await journey.frame();
        final metadata =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 2,
                }))['store_frame']
                as Map<String, dynamic>;
        final canvas = tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas;
        debugPrint(
          'PAUSED_INSPECT $checkpoint '
          'native_ms=${frame['now_ms']} native_selected=${metadata['selected_frame']} '
          'app_selected=${controller.animationSelectedFrameIndex} '
          'elapsed=${controller.animationElapsedMilliseconds} '
          'native_rgb=${(frame['board'] as List).first} app_rgb=${canvas.colors.first} '
          'status=${controller.status} pending=${controller.pending}',
        );
        expect(metadata['selected_frame'], 2);
        expect(metadata['playing'], isFalse);
        expect(controller.animationSelectedFrameIndex, 2);
        expect(controller.animationElapsedMilliseconds, elapsed);
        expect((frame['board'] as List).first, everyElement([0, 0, 204, 0]));
        expect(
          canvas.colors,
          everyElement(0x0000cc),
          reason:
              '$checkpoint: the mounted paused canvas must match the native selected frame',
        );
      }

      await aligned('seek');
      await journey.finish(controller.setBrush(color: 0xee7744));
      await tester.tap(find.byKey(const ValueKey('craft-animate')));
      await tester.pump(const Duration(milliseconds: 200));
      final writesBeforeManual = ble.writes.length;
      VirtualPacket heldPlaybackState({required bool playing, int? frame}) =>
          ble.held.lastWhere((packet) {
            final state = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return state?.kind == ConnectionSessionKind.state &&
                state?.code == ConnectionSession.litchcraftStateCode &&
                (state!.body[4] & 2 != 0) == playing &&
                (frame == null || state.body[0] == frame);
          });
      ble.holdConnectionObservations = true;
      await journey.finish(controller.toggleAnimationPlayback());
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-1')));
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-2')));
      await tester.pump();
      expect(controller.animationSelectedFrameIndex, 2);
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        reason: 'Both frame taps receive their native ordered receipts',
      );
      final predecessor = heldPlaybackState(playing: false, frame: 1);
      final predecessorPlay = heldPlaybackState(playing: true);
      ble.deliver(predecessorPlay);
      await tester.pump();
      expect(controller.animationPlaying, isFalse);
      expect(controller.animationSelectedFrameIndex, 2);
      ble.deliver(predecessor);
      await tester.pump();
      expect(
        controller.animationSelectedFrameIndex,
        2,
        reason:
            'A delayed predecessor State cannot replace the newest manual frame after its receipt',
      );
      expect(controller.documentSession.selectedFrameIndex, 2);
      final rect = tester.getRect(find.byType(LitchcraftCanvasView));
      Offset point(int column) =>
          rect.topLeft +
          Offset(rect.width * (column + .5) / 8, rect.height * 3.5 / 8);
      final finger = await tester.startGesture(point(3));
      await finger.moveTo(point(4));
      await tester.pump(const Duration(milliseconds: 16));
      ble.deliver(predecessor);
      await tester.pump();
      await finger.up();
      await tester.pump();
      expect(controller.animationSelectedFrameIndex, 2);
      for (final square in <int>[27, 28]) {
        expect(
          controller.documentSession.document.frames[2].emitters
              .skip(square * 4)
              .take(4)
              .map((emitter) => emitter.color),
          everyElement(0xee7744),
        );
      }
      expect(
        controller.documentSession.document.frames[1].emitters.map(
          (emitter) => emitter.color,
        ),
        everyElement(0x00bb00),
        reason: 'The predecessor frame must not receive the next gesture',
      );
      ble.holdConnectionObservations = false;
      ble.release('connection_session');
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
      );
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.animationSelectedFrameIndex, 2);
      await _expectCleanStoreParity(journey, controller);
      expect(
        _litchcraftRequests(ble, writesBeforeManual).where(
          (packet) =>
              packet[2] == LitchcraftV2Protocol.frameStoreBegin ||
              packet[2] == LitchcraftV2Protocol.frameStoreSnapshot,
        ),
        isEmpty,
        reason: 'Manual selection and drawing need no whole artwork transfer',
      );
      journey.checkpoint(
        'latest-manual-frame-and-draw',
        await journey.frame(),
        'Hall=0 mode=Litchcraft selected=2 predecessor=1 delayed-after-receipt '
            'draw=27,28 exact_stored_parity=true full_transfers=0 next=continue-drawing',
      );
      await journey.finish(controller.undo());
      expect(
        sameLitchcraftDocument(controller.documentSession.document, document),
        isTrue,
      );
      await journey.finish(controller.revalidateSelectedBoard());
      await ble.flush(tester);
      await aligned('Inspect');
      await ble.advance(tester, 60000);
      await aligned('one-minute-paused');
      Future<void> playingAligned(
        int index,
        int phase,
        String checkpoint,
      ) async {
        await journey.finish(controller.revalidateSelectedBoard());
        await ble.flush(tester);
        final frame = await journey.frame();
        final metadata =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': index,
                }))['store_frame']
                as Map<String, dynamic>;
        final color = document.frames[index].emitters.first.color;
        final rgb = [color >> 16, color >> 8 & 255, color & 255, 0];
        final canvas = tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas;
        debugPrint(
          'PAUSED_INSPECT $checkpoint '
          'native_ms=${frame['now_ms']} native_selected=${metadata['selected_frame']} '
          'app_selected=${controller.animationSelectedFrameIndex} '
          'elapsed=${controller.animationElapsedMilliseconds} app_rgb=${canvas.colors.first} '
          'status=${controller.status} pending=${controller.pending}',
        );
        expect(metadata['playing'], isTrue);
        expect(metadata['selected_frame'], index);
        expect(controller.animationSelectedFrameIndex, index);
        expect(controller.animationElapsedMilliseconds % 2750, phase);
        expect((frame['board'] as List).first, everyElement(rgb));
        expect(canvas.colors, everyElement(color));
      }

      await journey.finish(controller.toggleAnimationPlayback());
      await ble.advance(tester, 100);
      await playingAligned(2, 2350, 'Play-Inspect');
      await ble.advance(tester, 500);
      await playingAligned(0, 100, 'wrapped-Inspect');
      await journey.finish(controller.selectAnimationFrame(1));
      await journey.finish(controller.toggleAnimationPlayback());
      await ble.advance(tester, 1500);
      await playingAligned(1, 1750, 'two-second-frame-Inspect');
      await ble.advance(tester, 600);
      await playingAligned(2, 2350, 'two-second-boundary-Inspect');
      ble.holdConnectionObservations = true;
      await journey.finish(controller.toggleAnimationPlayback());
      await journey.finish(controller.toggleAnimationPlayback());
      final delayedPlay = heldPlaybackState(playing: true);
      await journey.finish(controller.toggleAnimationPlayback());
      expect(controller.animationTimelinePending, isFalse);
      expect(controller.animationPlaying, isFalse);
      ble.deliver(delayedPlay);
      await tester.pump();
      expect(
        controller.animationPlaying,
        isFalse,
        reason:
            'Delayed Play State cannot restart playback after Pause Applied',
      );
      expect(controller.animationSelectedFrameIndex, 2);
      await journey.finish(controller.toggleAnimationPlayback());
      final playBeforeHistory = heldPlaybackState(playing: true);
      await journey.finish(controller.selectAnimationFrame(2));
      await tester.tapAt(point(3));
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
      );
      ble.holdPhysicalStoreObjects = true;
      await journey.finish(controller.undo());
      expect(
        sameLitchcraftDocument(controller.documentSession.document, document),
        isTrue,
      );
      ble.deliver(playBeforeHistory);
      await tester.pump();
      expect(
        controller.animationPlaying,
        isFalse,
        reason: 'Undo Applied cannot retire the newest manual pause intent',
      );
      expect(controller.animationSelectedFrameIndex, 2);
      ble.holdPhysicalStoreObjects = false;
      ble.holdConnectionObservations = false;
      ble.release('connection_session');
      await ble.flush(tester);
      expect(controller.animationPlaying, isFalse);
      journey.checkpoint(
        'pause-and-undo-receipts-fence-delayed-play',
        await journey.frame(),
        'Hall=0 mode=Litchcraft frame=2 paused=true receipt=Applied '
            'Undo=Applied delayed_Play=ignored next=continue-drawing-or-Play',
      );
      await journey.finish(service.disconnect());
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.settleModeResolution());
      await ble.flush(tester);
      await aligned('reconnect');
      await journey.finish(library.flush());
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Play: repeated piece lift, adjacent placement and return stay synchronized',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, operationIntervalMs: 20);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final opening = BigInt.parse('FFFF00000000FFFF', radix: 16);
      await journey.hall(opening);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(service.requestCurrentHomeStateSnapshot(_boardId));
      await journey.until(
        () => service.latestHomeState != null,
        budgetMs: 3000,
      );

      final lifted = opening & ~(BigInt.one << 52); // e2.
      // e3, back to e2, then a repeated lift/return. No FEN/state injection.
      for (final mask in [
        lifted,
        lifted | (BigInt.one << 44),
        lifted,
        opening,
        lifted,
        opening,
      ]) {
        final start = journey.now;
        await journey.hall(mask);
        var board = await journey.frame();
        expect(BigInt.parse(board['occupancy'] as String, radix: 16), mask);
        final boardMs = journey.now - start;
        await journey.until(
          () => service.latestHomeState?.physicalStatus.occupiedSquares == mask,
        );
        final appMs = journey.now - start;
        if (mask == lifted) {
          final destinations = BigInt.parse(
            board['destinations'] as String,
            radix: 16,
          );
          expect(destinations & (BigInt.one << 44), isNot(BigInt.zero));
          // An adjacent legal destination takes 50 ms to begin its reveal
          // and a further 50 ms to reach the player's guidance color.
          board = await journey.untilFrame((frame) {
            final slots = (frame['board'] as List)[44] as List;
            return [
              slots.first,
              slots.last,
            ].every((rail) => (rail as List).any((channel) => channel != 0));
          }, budgetMs: 100);
          expect(
            (board['board'] as List)[44][1],
            [0, 0, 0, 0],
            reason: 'The empty e3 center stays dark between the lit rails.',
          );
        }
        journey.checkpoint(
          'Play-piece-path',
          board,
          'input_to_board_ms=$boardMs input_to_app_ms=$appMs',
        );
      }
      expect(service.latestHomeState!.physicalStatus.occupiedSquares, opening);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'OTA status: shared Object round trip replaces direct GATT at MTU 23',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(
        native,
        negotiatedMtu: 23,
        enableOtaControl: true,
      );
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));

      await journey.finish(service.connect(_boardId));
      await journey.finish(service.openConnectionSession(_boardId));
      final int writesBefore = ble.writes.length;
      final int packetsBefore = ble.emitted.length;
      String? version;
      await journey.finish(
        service
            .fetchBoardFirmwareVersion(deviceId: _boardId)
            .then((String? value) => version = value),
      );
      LitchDeviceCapabilities? refreshed;
      await journey.finish(
        service
            .fetchDeviceCapabilities(deviceId: _boardId, refresh: true)
            .then((value) => refreshed = value),
      );

      expect(version, isNotNull);
      expect(version, isNotEmpty);
      expect(refreshed?.supportsConnectionSession, isTrue);
      final writes = ble.writes.skip(writesBefore).toList();
      expect(
        writes.where((write) => write['channel'] == 'ota_control'),
        isEmpty,
        reason:
            'Version and refreshed capability reads use the active session.',
      );
      final objectControls = writes
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList((write['bytes'] as List).cast<int>()),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.object &&
                packet.code == ConnectionOtaCodec.controlObjectOpcode,
          )
          .toList();
      expect(objectControls, isNotEmpty);
      final statusFragments = ble.emitted
          .skip(packetsBefore)
          .where((packet) => packet.channel == 'connection_session')
          .map(
            (packet) =>
                ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes)),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.object &&
                packet.code == ConnectionSession.otaStatusReceiptCode,
          )
          .toList();
      expect(statusFragments.length, greaterThan(1));
      expect(
        statusFragments.every((packet) => packet.body.length <= 10),
        isTrue,
      );
      journey.checkpoint(
        'OTA-status-Object-round-trip',
        await journey.frame(),
        'mtu=23 control_fragments=${objectControls.length} '
            'status_fragments=${statusFragments.length} version=$version',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Capability refresh during shared activation never uses retired direct OTA ingress',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, enableOtaControl: true)
        ..heldChannels.add('connection_session');
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));

      await journey.finish(service.connect(_boardId));
      final opening = service.openConnectionSession(_boardId);
      bool activatedHeld() => ble.held.any((packet) {
        if (packet.channel != 'connection_session') return false;
        final decoded = ConnectionSessionCodec.decode(
          Uint8List.fromList(packet.bytes),
        );
        return decoded?.kind == ConnectionSessionKind.control &&
            decoded?.code == ConnectionSessionControl.activated;
      });
      for (var stage = 0; stage < 6 && !activatedHeld(); ++stage) {
        await journey.until(() => ble.held.isNotEmpty, budgetMs: 1000);
        if (!activatedHeld()) ble.release('connection_session');
      }
      expect(activatedHeld(), isTrue);
      final directBefore = ble.writes
          .where((write) => write['channel'] == 'ota_control')
          .length;
      final refresh = service.fetchDeviceCapabilities(
        deviceId: _boardId,
        refresh: true,
      );
      for (var i = 0; i < 25; ++i) {
        await journey.tick();
      }
      final directWrites =
          ble.writes
              .where((write) => write['channel'] == 'ota_control')
              .length -
          directBefore;
      ble.heldChannels.remove('connection_session');
      ble.release('connection_session');
      await journey.finish(opening);
      LitchDeviceCapabilities? capabilities;
      await journey.finish(refresh.then((value) => capabilities = value));
      expect(capabilities?.supportsConnectionSession, isTrue);
      expect(
        directWrites,
        0,
        reason: 'The board has accepted Hello and retired direct OTA ingress.',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Retired shared session reconnects before another capability request',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, enableOtaControl: true);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));

      await journey.finish(service.connect(_boardId));
      ConnectionSession? firstSession;
      await journey.finish(
        service.openConnectionSession(_boardId).then((value) {
          firstSession = value;
        }),
      );
      expect(firstSession?.isActivated, isTrue);
      for (var i = 0; i < 25; ++i) {
        await journey.tick();
      }
      expect(ble.connections, hasLength(1));
      ble.failNextConnectionSessionWrite = true;
      var nativeFailureSettled = false;
      Object? nativeFailure;
      unawaited(
        firstSession!
            .sendObject(0x74, Uint8List.fromList(<int>[0xcc]))
            .then(
              (_) => nativeFailureSettled = true,
              onError: (Object error, StackTrace _) {
                nativeFailure = error;
                nativeFailureSettled = true;
              },
            ),
      );
      for (var i = 0; i < 25 && !nativeFailureSettled; ++i) {
        try {
          await journey.tick();
        } on StateError catch (error) {
          expect(error.message, contains('scheduled native'));
        }
      }
      expect(nativeFailure, isA<StateError>());
      expect(firstSession?.isActivated, isFalse);
      expect(service.isConnected, isTrue);
      expect(ble.connections, hasLength(1));

      final directBefore = ble.writes
          .where((write) => write['channel'] == 'ota_control')
          .length;
      final staleRefresh = service.fetchDeviceCapabilities(
        deviceId: _boardId,
        refresh: true,
      );
      await journey.until(
        () => ble.connections.length == 2,
        budgetMs: 500,
        reason:
            'A retired session needs a new BLE generation, not silent retries.',
      );
      expect(
        ble.writes.where((write) => write['channel'] == 'ota_control').length,
        directBefore,
        reason: 'The retired generation cannot fall back to direct OTA.',
      );
      await journey.finish(staleRefresh);
      await journey.finish(service.openConnectionSession(_boardId));
      String? version;
      await journey.finish(
        service.fetchBoardFirmwareVersion(deviceId: _boardId).then((value) {
          version = value;
        }),
      );
      expect(version, isNotEmpty);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Modern keepalive never sends a legacy ping after session retirement',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, enableOtaControl: true);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));

      await journey.finish(service.connect(_boardId));
      ConnectionSession? session;
      await journey.finish(
        service.openConnectionSession(_boardId).then((value) {
          session = value;
        }),
      );
      expect(session?.isActivated, isTrue);
      for (var i = 0; i < 25; ++i) {
        await journey.tick();
      }
      ble.failNextConnectionSessionWrite = true;
      var settled = false;
      unawaited(
        session!
            .sendObject(0x74, Uint8List.fromList(<int>[0xcc]))
            .then(
              (_) => settled = true,
              onError: (Object _, StackTrace _) => settled = true,
            ),
      );
      for (var i = 0; i < 25 && !settled; ++i) {
        try {
          await journey.tick();
        } on StateError catch (error) {
          expect(error.message, contains('scheduled native'));
        }
      }
      expect(settled, isTrue);
      expect(session?.isActivated, isFalse);
      expect(ble.connections, hasLength(1));
      final directBefore = ble.writes
          .where((write) => write['channel'] == 'command')
          .length;

      await journey.until(
        () => ble.connections.length == 2,
        budgetMs: 4500,
        reason:
            'Modern keepalive repairs the session through a new generation.',
      );
      expect(
        ble.writes.where((write) => write['channel'] == 'command').length,
        directBefore,
        reason: 'A retired modern session must not fall back to direct ping.',
      );
      await journey.finish(service.openConnectionSession(_boardId));
      for (var i = 0; i < 35; ++i) {
        await journey.tick();
      }
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final mtu in <int>[23, 247]) {
    testWidgets(
      'Home: shared State replaces unavailable Console at MTU $mtu',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native, negotiatedMtu: mtu)
          ..dropConsole = true;
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        addTearDown(() => ble.disposeJourney(tester, service));
        final opening = BigInt.parse('FFFF00000000FFFF', radix: 16);
        await journey.hall(opening);

        await journey.finish(service.connect(_boardId));
        await journey.finish(service.openConnectionSession(_boardId));
        await journey.until(
          () => service.latestHomeState != null,
          budgetMs: 3000,
        );
        expect(
          service.latestHomeState!.physicalStatus.occupiedSquares,
          opening,
        );

        final lifted = opening & ~(BigInt.one << 52);
        ble.heldChannels.add('connection_session');
        await journey.hall(lifted);
        expect(
          service.latestHomeState!.physicalStatus.occupiedSquares,
          opening,
          reason: 'A partial held Home revision cannot leak to consumers.',
        );
        await journey.until(() {
          final byRevision = <int, Set<int>>{};
          for (final held in ble.held.where(
            (packet) => packet.channel == 'connection_session',
          )) {
            final packet = ConnectionSessionCodec.decode(
              Uint8List.fromList(held.bytes),
            );
            if (packet?.kind != ConnectionSessionKind.state ||
                packet!.code < HomeStateWireCodec.firstStateCode ||
                packet.code > HomeStateWireCodec.lastStateCode) {
              continue;
            }
            byRevision
                .putIfAbsent(packet.sequence, () => <int>{})
                .add(packet.code);
          }
          return byRevision.values.any(
            (codes) => codes.length == HomeStateWireCodec.fragmentCount,
          );
        }, budgetMs: 1500);
        expect(
          ble.held
              .where((packet) => packet.channel == 'connection_session')
              .every((packet) => packet.bytes.length <= mtu - 3),
          isTrue,
        );

        ble.heldChannels.remove('connection_session');
        ble.release('connection_session');
        await journey.until(
          () =>
              service.latestHomeState?.physicalStatus.occupiedSquares == lifted,
          budgetMs: 1000,
        );
        journey.checkpoint(
          'Home-State-console-unavailable',
          await journey.frame(),
          'mtu=$mtu fragments=${HomeStateWireCodec.fragmentCount}',
        );
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );

    testWidgets(
      'Health: shared State survives unavailable Console at MTU $mtu',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native, negotiatedMtu: mtu);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        addTearDown(() => ble.disposeJourney(tester, service));

        await journey.finish(service.connect(_boardId));
        await journey.finish(service.openConnectionSession(_boardId));
        await journey.until(
          () => service.latestBoardHealth != null,
          budgetMs: 3000,
        );
        expect(
          service.latestBoardHealth!.runtimeHealth.state,
          RuntimeHealthState.healthy,
        );

        ble.dropConsole = true;
        bool isRuntimeHealth(ConnectionSessionPacket packet) =>
            packet.kind == ConnectionSessionKind.state &&
            packet.code == ConnectionSession.runtimeHealthStateCode;
        ble.holdSessionPacket = isRuntimeHealth;
        await ble.command(tester, <String, Object>{
          'op': 'health',
          'brownout': true,
        });
        await journey.until(
          () => ble.held.isNotEmpty,
          budgetMs: 1500,
          reason: 'The board publishes the brownout on its paced State lane.',
        );
        expect(
          service.latestBoardHealth!.runtimeHealth.state,
          RuntimeHealthState.healthy,
          reason: 'A held State notification cannot update app health.',
        );
        final healthPacketBytes = ble.held.first.bytes.length;
        expect(
          ble.held.every((packet) => packet.bytes.length <= mtu - 3),
          isTrue,
          reason: 'Every health State must fit the negotiated ATT payload.',
        );

        ble.holdSessionPacket = null;
        ble.release('connection_session');
        await journey.until(
          () =>
              service.latestBoardHealth?.runtimeHealth.state ==
              RuntimeHealthState.degradedUsable,
        );
        expect(
          service.latestBoardHealth!.runtimeHealth.causeMask & 0x10,
          isNot(0),
        );
        journey.checkpoint(
          'Health-State-console-unavailable',
          await journey.frame(),
          'mtu=$mtu state_packet_bytes=$healthPacketBytes',
        );
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Setup: shared State and Ordered write replace Console/settings mutation at MTU 23',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchGattBle(native, negotiatedMtu: 23)
        ..dropConsole = true;
      final service = ble.createService();
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));

      await journey.finish(service.connect(_boardId));
      expect(
        await ble.completeTransaction(
          tester,
          service.ensureProductReady(_boardId),
        ),
        isTrue,
      );
      await journey.until(
        () => service.latestSettingValue('BRIGHTNESS') == 5,
        budgetMs: 3000,
      );
      bool isSettingsFragment(ConnectionSessionPacket? packet) =>
          packet != null &&
          packet.kind == ConnectionSessionKind.state &&
          SettingsStateWireCodec.acceptsCode(packet.code);
      final staleSettings = ble.emitted
          .where((packet) => isSettingsFragment(packet.sessionPacket))
          .toList();
      expect(
        staleSettings.map((packet) => packet.sessionPacket!.code).toSet(),
        hasLength(SettingsStateWireCodec.fragmentCount),
      );
      final directSettingsWritesBefore = ble.writes
          .where((write) => write['channel'] == 'settings')
          .length;
      int stateQueryWrites() => ble.sessionWrites
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.control &&
                packet.code == ConnectionSessionControl.query,
          )
          .length;
      LitchSettingWriteResult? result;
      ble.holdSessionPacket = isSettingsFragment;
      final mutation = service
          .writeSettingAcknowledged(
            deviceId: _boardId,
            key: 'BRIGHTNESS',
            value: 6,
          )
          .then((value) => result = value);
      await ble.flush(tester);
      await journey.until(() => ble.held.isNotEmpty, budgetMs: 500);
      ble.deliver(ble.held.removeAt(0));
      await tester.pump();
      expect(
        service.latestSettingValue('BRIGHTNESS'),
        5,
        reason: 'A partial settings revision cannot leak to Setup.',
      );
      final queriesBeforeRepair = stateQueryWrites();
      await ble.advance(tester, 600);
      expect(
        stateQueryWrites(),
        greaterThan(queriesBeforeRepair),
        reason:
            'A torn settings revision requests bounded shared-State repair.',
      );
      expect(ble.held.every((packet) => packet.bytes.length <= 20), isTrue);
      ble.holdSessionPacket = null;
      ble.release('connection_session');
      await journey.finish(mutation);
      expect(result, isA<LitchSettingWriteAcknowledged>());
      final acknowledged = result! as LitchSettingWriteAcknowledged;
      expect(acknowledged.outcome.ok, isTrue);
      expect(
        await ble.completeTransaction(
          tester,
          acknowledged.boardStateConfirmation!,
        ),
        isTrue,
      );
      expect(service.latestSettingValue('BRIGHTNESS'), 6);
      for (final packet in staleSettings) {
        ble.deliver(packet);
      }
      await tester.pump();
      expect(
        service.latestSettingValue('BRIGHTNESS'),
        6,
        reason: 'Replaying the previous board revision cannot undo the edit.',
      );
      expect(
        ble.writes.where((write) => write['channel'] == 'settings').length,
        directSettingsWritesBefore,
        reason: 'Modern Setup writes cannot bypass shared Ordered admission.',
      );
      journey.checkpoint(
        'Setup-State-Ordered-console-unavailable',
        await journey.frame(),
        'mtu=23 fragments=${SettingsStateWireCodec.fragmentCount} brightness=6',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Actions: Ordered rejects invalid Undo then recovers with asynchronous New Game',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, negotiatedMtu: 23)
        ..heldChannels.add('console');
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.parse('FFFF00000000FFFF', radix: 16));
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(service.openConnectionSession(_boardId));
      await ble.flush(tester);
      final settingsSubscriptions = ble.subscriptionEvents
          .where((event) => event['channel'] == 'settings')
          .toList();
      expect(
        settingsSubscriptions.isEmpty ||
            settingsSubscriptions.last['enabled'] == false,
        isTrue,
        reason:
            'An activated shared session must retire the legacy settings subscription.',
      );

      final events = <LitchControlEvent>[];
      final subscription = service.controlActionEvents.listen(events.add);
      addTearDown(subscription.cancel);
      final legacyWritesBefore = ble.writes
          .where((write) => write['channel'] == 'settings')
          .length;

      LitchControlEvent? invalidUndo;
      final undo = service
          .dispatchControlAction(
            deviceId: _boardId,
            action: LitchControlAction.undo,
          )
          .then((value) => invalidUndo = value);
      await journey.finish(undo);
      expect(invalidUndo?.phase, LitchControlPhase.rejected);
      expect(invalidUndo?.reason, LitchMutationReason.noHistory);

      LitchControlEvent? terminal;
      final newGame = service
          .dispatchControlAction(
            deviceId: _boardId,
            action: LitchControlAction.newGame,
          )
          .then((value) => terminal = value);
      await journey.finish(newGame);
      expect(terminal?.phase, LitchControlPhase.applied);
      expect(
        events.map((event) => event.phase),
        containsAllInOrder(<LitchControlPhase>[
          LitchControlPhase.pending,
          LitchControlPhase.rejected,
          LitchControlPhase.pending,
          LitchControlPhase.applied,
        ]),
      );
      expect(
        ble.writes.where((write) => write['channel'] == 'settings').length,
        legacyWritesBefore,
        reason: 'Modern board actions cannot use the settings characteristic.',
      );
      final actionPackets = ble.writes
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes']! as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.ordered &&
                packet.code == LitchControlProtocol.connectionActionOpcode,
          )
          .toList();
      expect(actionPackets, hasLength(2));
      expect(actionPackets[0].body, <int>[1, 2, 0, 0, 1]);
      expect(actionPackets[1].body, <int>[1, 3, 0, 0, 1]);
      journey.checkpoint(
        'Actions-Undo-reject-NewGame-recovery-Ordered',
        await journey.frame(),
        'mtu=23 invalid_reason=${invalidUndo?.reasonToken} ordered_packets=${actionPackets.length}',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Console: Ordered fragments commit one complete command at MTU 23',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, negotiatedMtu: 23);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));

      final legacyWritesBefore = ble.writes
          .where((write) => write['channel'] == 'command')
          .length;
      bool? sent;
      await journey.finish(
        service
            .sendConsoleLine(_boardId, 'settings status')
            .then((value) => sent = value),
      );
      expect(sent, isTrue);
      expect(
        ble.writes.where((write) => write['channel'] == 'command').length,
        legacyWritesBefore,
        reason:
            'A modern Console command cannot use the command characteristic.',
      );
      final packets = ble.writes
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes']! as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.ordered &&
                packet.code == LitchControlProtocol.connectionConsoleOpcode,
          )
          .toList();
      expect(packets, hasLength(3));
      expect(packets[0].body, <int>[1, 1, 16, ...utf8.encode('setting')]);
      expect(packets[1].body, <int>[1, 0, ...utf8.encode('s status')]);
      expect(packets[2].body, <int>[1, 2, 10]);
      journey.checkpoint(
        'Console-Ordered-complete-record',
        await journey.frame(),
        'mtu=23 fragments=${packets.length} legacy_writes=0',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'History: subscription joins session activation before MTU 23 Object round trip',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, negotiatedMtu: 23);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));

      List<int>? responseBytes;
      final Future<void> response = service
          .gameHistoryPackets(_boardId)
          .first
          .then((value) => responseBytes = value);

      final Uint8List begin = GameHistoryFrame(
        opcode: CurrentGameSyncOpcode.begin.wireValue,
        transactionId: 7,
        frameSequence: 0,
        payload: const CurrentGameResumeCursor.empty().encode(),
      ).encode();
      await journey.finish(service.writeGameHistoryPacket(_boardId, begin));

      final List<ConnectionSessionPacket> packets = ble.writes
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes']! as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.object &&
                packet.code == ConnectionGameHistoryCodec.objectOpcode,
          )
          .toList();
      expect(packets, hasLength(3));
      final ConnectionGameHistoryAssembler assembler =
          ConnectionGameHistoryAssembler();
      Uint8List? reconstructed;
      for (final ConnectionSessionPacket packet in packets) {
        reconstructed = assembler.accept(packet.body) ?? reconstructed;
      }
      expect(reconstructed, begin);
      await journey.finish(response);
      final GameHistoryFrame state = GameHistoryFrame.decode(
        Uint8List.fromList(responseBytes!),
      );
      expect(state.opcode, CurrentGameSyncOpcode.state.wireValue);
      expect(state.transactionId, 7);
      expect(state.frameSequence, 1);
      expect(
        CurrentGameDigest.decode(state.payload, allowAbsent: true).fingerprint,
        everyElement(0),
      );
      journey.checkpoint(
        'History-Object-round-trip',
        await journey.frame(),
        'mtu=23 request_fragments=${packets.length} response=CurrentState '
            'dedicated_writes=0',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: 256-frame Use board recovers lost reverse credit',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));

      final first = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      await journey.finish(first.enter());
      expect(first.boardActive, isTrue, reason: first.errorMessage);
      final frames = List<LitchcraftAnimationFrame>.generate(
        256,
        (frame) => LitchcraftAnimationFrame(
          durationMs: 50 + frame,
          emitters: List<LitchcraftAnimatedEmitter>.generate(
            256,
            (index) => LitchcraftAnimatedEmitter(
              color:
                  (((frame * 17 + index * 11) & 0xff) << 16) |
                  (((frame * 23 + index * 7) & 0xff) << 8) |
                  ((frame * 31 + index * 3) & 0xff),
            ),
          ),
        ),
      );
      LitchcraftFrameStoreDiagnosticResult? transfer;
      await journey.finish(
        first
            .transferFrameStoreForDiagnostics(
              frames: frames,
              playbackOnEntry: false,
            )
            .then((result) => transfer = result),
        budgetMs: 60000,
      );
      expect(transfer?.transfer.state.frameCount, 256);
      first.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));

      final local = LitchcraftDocumentSession(
        staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x123456),
        ),
      );
      final second = LitchcraftController(service, documentSession: local);
      addTearDown(second.dispose);
      await journey.finish(second.enter(), budgetMs: 20000);
      expect(second.status, LitchcraftStatus.conflict);
      ble.reverseProgressDropsRemaining = 4;
      await journey.finish(
        second.resolveConflict(useBoard: true),
        budgetMs: 60000,
      );
      expect(ble.droppedReverseProgress, 4);
      expect(second.boardActive, isTrue, reason: second.errorMessage);
      expect(second.status, LitchcraftStatus.live);
      expect(local.document.frames, hasLength(256));
      expect(
        local.document.frames[255].emitters[255].color,
        frames[255].emitters[255].color,
      );
      // Finish this owner's optional clock work while its native link is alive.
      second.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant(<TargetPlatform>{
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: State between independent taps preserves captured Delete and stroke history',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _DelayedPaintCheckpointBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
      final original = LitchcraftAnimationDocument(
        frames: LitchcraftBouncingBallsPreset.decode().frames,
        topology: blank.topology,
        favoriteColors: blank.favoriteColors,
      );
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      final seed = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(original),
      );
      addTearDown(seed.dispose);
      await journey.finish(seed.enter());
      await journey.finish(
        seed.transferFrameStoreForDiagnostics(
          frames: original.frames,
          playbackOnEntry: false,
        ),
      );
      seed.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await journey.finish(service.connect(_boardId));
      final local = LitchcraftDocumentSession(original, localIntent: false);
      final controller = LitchcraftController(service, documentSession: local);
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
      });
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      expect(controller.usesFrameStoreDocument, isTrue);
      await journey.finish(controller.selectAnimationFrame(108));
      await _expectCleanStoreParity(journey, controller);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.tap(find.byKey(const ValueKey('craft-animate')));
      await tester.pump(const Duration(milliseconds: 300));
      final neighbor = find.byKey(const ValueKey('craft-select-frame-108'));
      await tester.ensureVisible(neighbor);
      await tester.pumpAndSettle();
      // Preserve the successful Delete/Undo prelude from physical QA; target108 is warm.
      await tester.longPress(neighbor);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Delete frame 109?'), findsOneWidget);
      await tester.tap(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
      );
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      expect(local.document.frames, hasLength(255));
      await _expectCleanStoreParity(journey, controller);
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      expect(local.document.frames, hasLength(256));
      await _expectCleanStoreParity(journey, controller);
      expect(sameLitchcraftDocument(local.document, original), isTrue);
      await journey.finish(controller.selectAnimationFrame(107));
      await journey.finish(
        controller.setBrush(color: 0xff7430, targetMask: 15),
      );
      await tester.ensureVisible(neighbor);
      await tester.pumpAndSettle();
      expect(neighbor.hitTestable(), findsOneWidget);
      final expected = LitchcraftDocumentSession(original)..selectFrame(107);
      addTearDown(expected.dispose);
      expected.paintSelectedFrame(squares: [56, 57], mask: 15, color: 0xff7430);
      final painted = expected.document;
      final rect = tester.getRect(find.byType(LitchcraftCanvasView));
      Offset at(int column) =>
          rect.topLeft +
          Offset(rect.width * (column + .5) / 8, rect.height * 7.5 / 8);
      ble.holdPaintCheckpoints = true;
      final writesBefore = ble.writes.length;
      for (final column in [0, 1]) {
        final finger = await tester.startGesture(at(column));
        await tester.pump(Duration(milliseconds: column == 0 ? 114 : 89));
        await finger.up();
        await tester.pump();
        if (column == 0) {
          await journey.until(
            () => ble.heldPaintCheckpoints.isNotEmpty,
            budgetMs: 250,
          );
          await ble.command(tester, {
            'op': 'advance',
            'ms': 10,
          }, flushOperations: false);
          await tester.pump();
          final mid =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 107,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          final states = ble.emitted
              .where((p) => p.channel == 'connection_session')
              .map(
                (p) =>
                    ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes)),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (p) =>
                    p.kind == ConnectionSessionKind.state &&
                    p.code ==
                        ConnectionSession.litchcraftStoreAuthorityStateCode,
              )
              .map((p) => {'sequence': p.sequence, 'body': p.body.toList()})
              .toList();
          debugPrint(
            'PREIMAGE betweenTaps ${jsonEncode({'pending': controller.pending, 'held': ble.heldPaintCheckpoints.length, 'capturedCommonRevision': controller.captureAnimationFrameEditTarget(frame: 108)?.revision, 'nativeRevision': mid['live_revision'], 'nativeGeneration': mid['root_generation'], 'dirty': mid['dirty'], 'authorityStates': states})}',
          );
          expect(controller.pending, isTrue);
          final commonRevision = controller
              .captureAnimationFrameEditTarget(frame: 108)!
              .revision;
          expect(mid['live_revision'], commonRevision + 1);
          final latestAuthority = ble.emitted
              .where((p) => p.channel == 'connection_session')
              .map(
                (p) =>
                    ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes)),
              )
              .whereType<ConnectionSessionPacket>()
              .lastWhere(
                (p) =>
                    p.kind == ConnectionSessionKind.state &&
                    p.code ==
                        ConnectionSession.litchcraftStoreAuthorityStateCode,
              );
          expect(latestAuthority.body[1], 1);
          expect(
            ByteData.sublistView(
              latestAuthority.body,
            ).getUint32(6, Endian.little),
            commonRevision + 1,
            reason:
                'The first native State must arrive before tap two while its receipt is held',
          );
        }
      }
      final lastTap = journey.now;
      expect(sameLitchcraftDocument(local.document, painted), isTrue);
      await journey.until(
        () => ble.heldPaintCheckpoints.isNotEmpty,
        budgetMs: 250,
        reason:
            'The first real native Paint checkpoint must be retained before dialog entry',
      );
      expect(controller.pending, isTrue);
      debugPrint(
        'PREIMAGE beforeCapture ${jsonEncode({'pending': controller.pending, 'held': ble.heldPaintCheckpoints.length, 'localFrames': local.document.frames.length, 'selected': local.selectedFrameIndex})}',
      );
      // No receipt settlement/SDK drain before real mounted long-press capture.
      await tester.longPress(neighbor);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Delete frame 109?'), findsOneWidget);
      await journey.until(
        () => ble.heldPaintCheckpoints.isNotEmpty,
        budgetMs: 250,
      );
      await ble.command(tester, {
        'op': 'advance',
        'ms': 1000,
      }, flushOperations: false);
      final heldClock = Stopwatch()..start();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 1250)),
      );
      debugPrint(
        'PREIMAGE captured ${jsonEncode({'pending': controller.pending, 'held': ble.heldPaintCheckpoints.length, 'elapsedFromTap': journey.now - lastTap, 'heldRealMs': heldClock.elapsedMilliseconds})}',
      );
      ble.holdPaintCheckpoints = false;
      for (final packet in List<VirtualPacket>.of(ble.heldPaintCheckpoints)) {
        ble.deliver(packet);
      }
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 1500,
        reason: 'Owned paint settles after capture, before Confirm',
      );
      final afterPaint =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 107,
              }, flushOperations: false))['store_frame']
              as Map<String, dynamic>;
      debugPrint(
        'PREIMAGE paintSettled ${jsonEncode({'status': controller.status.name, 'pending': controller.pending, 'dirty': afterPaint['dirty'], 'revision': afterPaint['live_revision'], 'nativeFrames': afterPaint['frame_count']})}',
      );
      expect(sameLitchcraftDocument(local.document, painted), isTrue);
      final remaining = 3729 - (journey.now - lastTap);
      if (remaining > 0) {
        await ble.command(tester, {
          'op': 'advance',
          'ms': remaining,
        }, flushOperations: false);
        await tester.pump(Duration(milliseconds: remaining));
      }
      await tester.tap(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
      );
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      final result =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 107,
              }, flushOperations: false))['store_frame']
              as Map<String, dynamic>;
      final ordered = ble.writes
          .skip(writesBefore)
          .where((w) => w['channel'] == 'connection_session')
          .map(
            (w) => ConnectionSessionCodec.decode(
              Uint8List.fromList(w['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((p) => p.kind == ConnectionSessionKind.ordered)
          .map(
            (p) => {
              'opcode': p.code,
              'sequence': p.sequence,
              'body': p.body.toList(),
            },
          )
          .toList();
      debugPrint(
        'PREIMAGE result ${jsonEncode({
          'status': controller.status.name,
          'error': controller.errorMessage,
          'appFrames': local.document.frames.length,
          'selected': local.selectedFrameIndex,
          'native': {for (final e in result.entries)
            if (e.key != 'emitters') e.key: e.value},
          'ordered': ordered,
        })}',
      );
      expect(
        local.document.frames,
        hasLength(255),
        reason:
            'Captured direct neighbor Delete must commit, preserving owned paint',
      );
      expected.selectFrame(108);
      expected.deleteSelectedFrame();
      expect(sameLitchcraftDocument(local.document, expected.document), isTrue);
      expect(local.selectedFrameIndex, 107);
      expect(result['frame_count'], 255);
      expect(result['selected_frame'], 107);
      expect(controller.status, LitchcraftStatus.live);
      expect(find.text('Choose artwork'), findsNothing);
      final paints = ordered
          .where((p) => p['opcode'] == ConnectionSession.litchcraftPaintOpcode)
          .toList();
      expect(
        paints,
        hasLength(2),
        reason: 'Each tap retains its own accepted Paint',
      );
      expect(paints[0]['body'], [107, 1, 0, 3, 15, 255, 116, 48, 56]);
      expect(paints[1]['body'], [107, 2, 0, 3, 15, 255, 116, 48, 57]);
      expect(
        ordered.last['opcode'],
        ConnectionSession.litchcraftStoreFrameEditOpcode,
      );
      await _expectCleanStoreParity(journey, controller);
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      expect(local.document.frames, hasLength(256));
      expect(sameLitchcraftDocument(local.document, painted), isTrue);
      expect(local.selectedFrameIndex, 108);
      await _expectCleanStoreParity(journey, controller);
      // Undo Delete, then each independent tap. A deferred gesture must never
      // merge into its predecessor or become a silent whole-document upload.
      final firstOnly = LitchcraftDocumentSession(original)..selectFrame(107);
      addTearDown(firstOnly.dispose);
      firstOnly.paintSelectedFrame(squares: [56], mask: 15, color: 0xff7430);
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      expect(
        sameLitchcraftDocument(local.document, firstOnly.document),
        isTrue,
      );
      await _expectCleanStoreParity(journey, controller);
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pump();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      expect(sameLitchcraftDocument(local.document, original), isTrue);
      await _expectCleanStoreParity(journey, controller);
      debugPrint(
        'INTERTAP PASS exact255/Undo256 and independent Undo of both taps; full native parity',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant(<TargetPlatform>{
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final scenario
      in <({bool dirty, bool neighboringFrame, bool coldTarget})>[
        (dirty: false, neighboringFrame: false, coldTarget: false),
        (dirty: true, neighboringFrame: false, coldTarget: false),
        (dirty: true, neighboringFrame: true, coldTarget: false),
        (dirty: true, neighboringFrame: true, coldTarget: true),
      ]) {
    final dirtyBeforeDelete = scenario.dirty;
    final hotResidentDelete = scenario.dirty && !scenario.coldTarget;
    testWidgets(
      scenario.coldTarget
          ? 'Litchcraft: cold neighboring-frame Delete retains source preparation'
          : scenario.neighboringFrame
          ? 'Litchcraft: hot dirty neighboring-frame Delete applies before deferred save'
          : dirtyBeforeDelete
          ? 'Litchcraft: hot dirty Delete applies before deferred save'
          : 'Litchcraft: received Delete remains pending across a slow Store worker',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final wall = Stopwatch()..start();
        final trace = _SyncRecoveryTrace(() => wall.elapsedMilliseconds);
        final service = _BridgeBoard(ble, appTrace: trace);
        final journey = _Journey(tester, ble, service);
        addTearDown(() => ble.disposeJourney(tester, service));
        final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
        final original = LitchcraftAnimationDocument(
          frames: LitchcraftBouncingBallsPreset.decode().frames,
          topology: blank.topology,
          favoriteColors: blank.favoriteColors,
        );
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        final seed = LitchcraftController(
          service,
          documentSession: LitchcraftDocumentSession(original),
        );
        addTearDown(seed.dispose);
        await journey.finish(seed.enter());
        await journey.finish(
          seed.transferFrameStoreForDiagnostics(
            frames: original.frames,
            playbackOnEntry: false,
          ),
        );
        seed.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
        await journey.finish(service.connect(_boardId));
        // Adopt the authentic saved Store through the same cold-entry path as
        // the existing mounted tool journeys, before testing a guarded edit.
        final local = LitchcraftDocumentSession(original, localIntent: false);
        final controller = LitchcraftController(
          service,
          documentSession: local,
        );
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
        });
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        expect(controller.usesFrameStoreDocument, isTrue);
        expect(controller.status, LitchcraftStatus.live);
        await journey.finish(
          controller.selectAnimationFrame(scenario.coldTarget ? 106 : 108),
        );
        final baselineGeneration = await _expectCleanStoreParity(
          journey,
          controller,
        );
        final baseline =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': scenario.coldTarget ? 106 : 108,
                }))['store_frame']
                as Map<String, dynamic>;
        final baselineRevision = baseline['live_revision'] as int;
        var pendingRevision = baselineRevision;
        final opened = await service.openConnectionSession(_boardId);
        expect(opened, isNotNull);
        final session = opened!;
        final expected = LitchcraftDocumentSession(original)..selectFrame(108);
        addTearDown(expected.dispose);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pump(const Duration(milliseconds: 300));
        final selected = find.byKey(const ValueKey('craft-select-frame-108'));
        await tester.ensureVisible(selected);
        await tester.pumpAndSettle();
        expect(selected.hitTestable(), findsOneWidget);

        if (dirtyBeforeDelete) {
          final color = scenario.coldTarget ? 0xff7430 : 0xe430ab;
          if (scenario.coldTarget) {
            //106 prefetched107. Stop before107 can prefetch the Delete target108.
            await ble.command(tester, {'op': 'store_worker', 'enabled': false});
          }
          await journey.finish(controller.selectAnimationFrame(107));
          await journey.finish(
            controller.setBrush(color: color, targetMask: 15),
          );
          await ble.command(tester, {'op': 'store_worker', 'enabled': false});
          final rect = tester.getRect(find.byType(LitchcraftCanvasView));
          Offset at(int column) =>
              rect.topLeft +
              Offset(
                rect.width * (column + .5) / 8,
                rect.height * (scenario.coldTarget ? 7.5 : .5) / 8,
              );
          final writesBeforeDirtyStroke = ble.writes.length;
          if (scenario.coldTarget) {
            // Preserve the two independent orange a1/b1 taps from physical QA.
            for (final column in [0, 1]) {
              final finger = await tester.startGesture(at(column));
              await tester.pump(Duration(milliseconds: column == 0 ? 114 : 89));
              await finger.up();
              await tester.pump();
            }
          } else {
            final finger = await tester.startGesture(at(0));
            await finger.moveTo(at(1));
            await tester.pump(const Duration(milliseconds: 16));
            await finger.up();
            await tester.pump();
          }
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
          );
          expected.selectFrame(107);
          expected.paintSelectedFrame(
            squares: scenario.coldTarget ? [56, 57] : [0, 1],
            mask: 15,
            color: color,
          );
          expect(
            sameLitchcraftDocument(local.document, expected.document),
            isTrue,
          );
          final dirty =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 107,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(dirty['dirty'], isTrue);
          expect(dirty['root_generation'], baselineGeneration);
          final paintPositions = ble.writes
              .skip(writesBeforeDirtyStroke)
              .where((write) => write['channel'] == 'connection_session')
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (packet) =>
                    packet.kind == ConnectionSessionKind.ordered &&
                    packet.code == ConnectionSession.litchcraftPaintOpcode,
              )
              .map((packet) => packet.sequence)
              .toSet();
          expect(paintPositions.length, inInclusiveRange(1, 2));
          // Store liveR advances for each changed square, independently of
          // whether those two squares coalesce into one transport packet.
          expect(
            original.frames[107].emitters
                .skip((scenario.coldTarget ? 56 : 0) * 4)
                .take(8)
                .map((e) => e.color),
            everyElement(isNot(color)),
          );
          expect(dirty['live_revision'], baselineRevision + 2);
          pendingRevision = dirty['live_revision'] as int;
          if (scenario.coldTarget) {
            await ble.advance(tester, 20);
            final displayed = await journey.frame();
            expect(
              (displayed['board'] as List)[56],
              everyElement([255, 116, 48, 0]),
            );
            expect(
              (displayed['board'] as List)[57],
              everyElement([255, 116, 48, 0]),
            );
          }
          if (!scenario.neighboringFrame) {
            await journey.finish(controller.selectAnimationFrame(108));
          }
          // The human can long-press a neighboring thumbnail directly. That
          // target must survive the dirty selected frame's save/selection.
          expected.selectFrame(108);
        }
        expected.deleteSelectedFrame();
        await tester.ensureVisible(selected);
        await tester.pumpAndSettle();
        expect(selected.hitTestable(), findsOneWidget);

        // Pause the native storage worker to exercise the queued write.
        // FrameStream still owns admission, mutation, cancellation and publication.
        await ble.command(tester, {'op': 'store_worker', 'enabled': false});
        final writesBefore = ble.writes.length;
        final emittedBeforeDelete = ble.emitted.length;
        await tester.longPress(selected);
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.text('Delete frame 109?'), findsOneWidget);
        await tester.tap(
          find.byKey(const ValueKey('craft-confirm-delete-frame')),
        );
        await tester.pump();
        await journey.until(
          () => ble.writes.skip(writesBefore).any((write) {
            if (write['channel'] != 'connection_session') return false;
            final packet = ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            );
            return packet?.kind == ConnectionSessionKind.ordered &&
                packet?.code ==
                    ConnectionSession.litchcraftStoreFrameEditOpcode;
          }),
          budgetMs: 1000,
        );
        await ble.flush(tester);
        final delete = ble.writes
            .skip(writesBefore)
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .singleWhere(
              (packet) =>
                  packet.kind == ConnectionSessionKind.ordered &&
                  packet.code ==
                      ConnectionSession.litchcraftStoreFrameEditOpcode,
            );
        expect(delete.body.toList(), [1, 108, 0, 0, 0]);
        expect(local.document.frames, hasLength(255));
        expect(local.selectedFrameIndex, 107);

        List<ConnectionSessionPacket> receivedPendingReplies() => ble.emitted
            .where((packet) => packet.channel == 'connection_session')
            .map(
              (packet) => ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where(
              (packet) =>
                  packet.kind == ConnectionSessionKind.control &&
                  packet.token == session.streamToken &&
                  packet.code == ConnectionSessionControl.progress &&
                  packet.sequence == delete.sequence - 1 &&
                  (ByteData.sublistView(
                            packet.body,
                          ).getUint16(8, Endian.little) &
                          1) !=
                      0,
            )
            .toList();

        if (hotResidentDelete) {
          // Both the source and preceding survivor are resident. The existing
          // Ordered owner can apply in RAM while the worker still holds saves.
          final packets = ble.emitted
              .skip(emittedBeforeDelete)
              .where((packet) => packet.channel == 'connection_session')
              .map(
                (packet) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(packet.bytes),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .toList();
          final applied = packets.where(
            (packet) =>
                packet.kind == ConnectionSessionKind.control &&
                packet.token == session.streamToken &&
                packet.code == ConnectionSessionControl.checkpoint &&
                packet.sequence == delete.sequence,
          );
          expect(
            applied,
            isNotEmpty,
            reason: 'The actual native Delete checkpoint owns RAM admission',
          );
          for (final receipt in applied) {
            final data = ByteData.sublistView(receipt.body);
            expect(data.getUint32(0, Endian.little), pendingRevision + 1);
            expect(data.getUint16(8, Endian.little) & 1, 0);
          }
          final live = packets.where(
            (packet) =>
                packet.kind == ConnectionSessionKind.state &&
                packet.code == ConnectionSession.litchcraftStateCode &&
                packet.body.length == 10 &&
                packet.body[1] == 255,
          );
          expect(live, isNotEmpty);
          expect(live.last.body[0], 107);
          expect(live.last.body[4] & 3, 1);
          final saved =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 108,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(session.isActivated, isTrue);
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.pending, isFalse);
          expect(controller.boardArtworkUnresolved, isFalse);
          expect(controller.animationPlaying, isFalse);
          expect(saved['live_revision'], pendingRevision + 1);
          expect(saved['selected_frame'], 107);
          expect(saved['structural_pending'], isFalse);
          expect(saved['dirty'], isTrue);
          expect(saved['root_generation'], baselineGeneration);
          expect(saved['frame_count'], 256);
          expect(
            sameLitchcraftDocument(local.document, expected.document),
            isTrue,
          );
          debugPrint(
            'LIVE_DELETE AppliedR=${saved['live_revision']} liveFrames=255 '
            'selected=107 savedFrames=256 root=${saved['root_generation']} '
            'dirty=true next=ResumeWorkerThenVerifySavedParity',
          );
        }

        final holdClock = Stopwatch()..start();
        var advancedHoldMs = 0;
        while (!hotResidentDelete &&
            holdClock.elapsed < const Duration(seconds: 7)) {
          // Stopwatch is the real production deadline clock. Match native and
          // Flutter time to elapsed wall time at each step, including earlier
          // bridge I/O, so the controller's completion ceiling is exercised too.
          final waitStarted = holdClock.elapsedMilliseconds;
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 350)),
          );
          final elapsed = holdClock.elapsedMilliseconds;
          debugPrint(
            'PENDING_DELETE_PHASE wait_start_ms=$waitStarted wait_end_ms=$elapsed '
            'wall_ms=${wall.elapsedMilliseconds}',
          );
          await ble.advance(tester, elapsed - advancedHoldMs);
          advancedHoldMs = elapsed;
          debugPrint(
            'PENDING_DELETE_PHASE advance_end_ms=${holdClock.elapsedMilliseconds} '
            'advanced_ms=$advancedHoldMs wall_ms=${wall.elapsedMilliseconds}',
          );
          final held =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 108,
                  }))['store_frame']
                  as Map<String, dynamic>;
          debugPrint(
            'PENDING_DELETE_PHASE read_end_ms=${holdClock.elapsedMilliseconds} '
            'wall_ms=${wall.elapsedMilliseconds}',
          );
          final queryCount = ble.writes
              .skip(writesBefore)
              .where((write) => write['channel'] == 'connection_session')
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (packet) =>
                    packet.kind == ConnectionSessionKind.control &&
                    packet.token == session.streamToken &&
                    packet.code == ConnectionSessionControl.query,
              )
              .length;
          final replyCount = receivedPendingReplies().length;
          debugPrint(
            'PENDING_DELETE elapsed_ms=${holdClock.elapsedMilliseconds} '
            'nativeFrames=${held['frame_count']} root=${held['root_generation']} '
            'structuralPending=${held['structural_pending']} appFrames=${local.document.frames.length} '
            'selected=${local.selectedFrameIndex} status=${controller.status} '
            'sessionLive=${session.isActivated} queries=$queryCount receivedPendingReplies=$replyCount next=WaitForExactPublication',
          );
          expect(
            session.isActivated,
            isTrue,
            reason:
                'Responsive received structural work must not retire at the transport recovery deadline',
          );
          expect(held['structural_pending'], !scenario.coldTarget);
          expect(held['frame_count'], 256);
          expect(held['selected_frame'], 108);
          expect(held['root_generation'], baselineGeneration);
          expect(held['live_revision'], pendingRevision);
          expect(held['dirty'], dirtyBeforeDelete);
          if (scenario.coldTarget) {
            final displayed = await journey.frame();
            expect(
              (displayed['board'] as List)[56],
              everyElement([255, 116, 48, 0]),
              reason:
                  '108 is requested while the still displayed107 awaits worker preparation',
            );
            expect(
              (displayed['board'] as List)[57],
              everyElement([255, 116, 48, 0]),
            );
          }
          expect(controller.pending, isTrue);
          expect(controller.status, LitchcraftStatus.live);
          expect(local.document.frames, hasLength(255));
          expect(find.text('Choose artwork'), findsNothing);
        }
        final pendingReplies = receivedPendingReplies();
        if (!hotResidentDelete) {
          expect(
            pendingReplies.length,
            greaterThanOrEqualTo(2),
            reason:
                'The native owner must answer Query while structural preparation is pending',
          );
        }
        await ble.command(tester, {
          'op': 'store_worker',
          'enabled': true,
        }, flushOperations: false);
        // Hot edits are already Applied; cold preparation must now receive
        // its own terminal receipt. Both paths still verify the exact saved
        // document and generation below after the worker resumes.
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 2000,
          reason: 'Only exact publication completes Delete',
        );
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        expect(session.isActivated, isTrue);
        expect(
          sameLitchcraftDocument(local.document, expected.document),
          isTrue,
        );
        expect(local.selectedFrameIndex, 107);
        final publishedGeneration = await _expectCleanStoreParity(
          journey,
          controller,
        );
        final published =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 107,
                }))['store_frame']
                as Map<String, dynamic>;
        expect(published['frame_count'], 255);
        expect(published['selected_frame'], 107);
        expect(published['structural_pending'], isFalse);
        expect(
          publishedGeneration,
          baselineGeneration + (dirtyBeforeDelete ? 2 : 1),
        );
        expect(published['live_revision'], pendingRevision + 1);
        expect(find.text('Choose artwork'), findsNothing);
        debugPrint(
          'PENDING_DELETE applied nativeFrames=255 selected=107 '
          'pendingReplies=${pendingReplies.length} root=${published['root_generation']} '
          'next=ContinueEditing',
        );
        if (scenario.coldTarget) {
          final restored = LitchcraftDocumentSession(original)
            ..selectFrame(107);
          addTearDown(restored.dispose);
          restored.paintSelectedFrame(
            squares: [56, 57],
            mask: 15,
            color: 0xff7430,
          );
          final undo = find.byKey(const ValueKey('craft-undo'));
          expect(undo.hitTestable(), findsOneWidget);
          await tester.tap(undo);
          await tester.pump();
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 2000,
            reason: 'Structural Undo must restore the exact removed frame',
          );
          expect(
            sameLitchcraftDocument(local.document, restored.document),
            isTrue,
          );
          expect(local.document.frames, hasLength(256));
          expect(local.selectedFrameIndex, 108);
          await _expectCleanStoreParity(journey, controller);
          expect(find.text('Choose artwork'), findsNothing);
        }
        await tester.pumpWidget(const SizedBox.shrink());
        controller.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
      },
      variant: scenario.coldTarget
          ? TargetPlatformVariant(<TargetPlatform>{
              TargetPlatform.android,
              TargetPlatform.iOS,
            })
          : TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final tool in <String>[
    'Rotate',
    'New frame',
    'Delete frame',
    'Duration',
    'Off',
    'Rotate three',
    'New frame three',
    'Paint then Breathe',
    'Delete cancel',
  ]) {
    testWidgets(
      'Litchcraft: Store mounted tool $tool follows native artwork',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        addTearDown(() => ble.disposeJourney(tester, service));
        final seeded = LitchcraftDocumentSession(
          staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint(<int>[0, 8], 15, 0xf03010),
          ),
        );
        if (!tool.startsWith('Rotate')) {
          seeded.setSelectedFrameDuration(150);
          seeded.addFrame();
          seeded.paintSelectedFrame(
            squares: <int>[7],
            mask: 15,
            color: 0x10d030,
          );
          seeded.setSelectedFrameDuration(350);
          if (tool == 'Off') {
            seeded.applyEffect(
              square: 7,
              mask: 15,
              effect: LitchcraftEffectStyle.breathe,
              effectDurationMs: 1600,
            );
          }
          seeded.addFrame();
          seeded.paintSelectedFrame(
            squares: <int>[63],
            mask: 15,
            color: 0x1030f0,
          );
          seeded.setSelectedFrameDuration(550);
          seeded.selectFrame(1);
        }
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        final seed = LitchcraftController(service, documentSession: seeded);
        await journey.finish(seed.enter());
        await journey.finish(
          seed.transferFrameStoreForDiagnostics(
            frames: seeded.document.frames,
            playbackOnEntry: false,
          ),
        );
        seed.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
        await journey.finish(service.connect(_boardId));
        final controller = LitchcraftController(
          service,
          documentSession: LitchcraftDocumentSession(
            seeded.document,
            localIntent: false,
          ),
        );
        await journey.finish(controller.enter());
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.usesFrameStoreDocument, isTrue);
        final source = tool.startsWith('Rotate') ? 0 : 1;
        await journey.finish(controller.selectAnimationFrame(source));
        final library = LitchcraftLibrary(controller);
        await journey.finish(library.ready);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
        });
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await _expectCleanStoreParity(journey, controller);
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 10000,
          reason: 'The rendered tool starts in healthy authority',
        );
        final before = controller.documentSession.document;
        final expected = LitchcraftDocumentSession(before)..selectFrame(source);
        var undoExpected = before;
        final writesBefore = ble.writes.length;
        if (tool.startsWith('Rotate')) {
          for (var turn = 0; turn < (tool == 'Rotate' ? 1 : 3); turn++) {
            undoExpected = expected.document;
            expected.rotateSelectedFrame(1);
            await tester.tap(
              find.byKey(const ValueKey('craft-rotate-artwork')),
            );
          }
        } else if (tool == 'Paint then Breathe') {
          await journey.finish(
            controller.setBrush(color: 0x664422, targetMask: 5),
          );
          final boardRect = tester.getRect(find.byType(LitchcraftCanvasView));
          await tester.tapAt(
            boardRect.topLeft +
                Offset(boardRect.width * 7.5 / 8, boardRect.height * .5 / 8),
          );
          expected.paintSelectedFrame(
            squares: <int>[7],
            mask: 5,
            color: 0x664422,
          );
          undoExpected = expected.document;
          await tester.tap(find.byKey(const ValueKey('craft-motion')));
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(find.byKey(const ValueKey('craft-effect-breathe')));
          await tester.pump();
          expected.applyEffect(
            square: 7,
            mask: 5,
            effect: LitchcraftEffectStyle.breathe,
            effectDurationMs: 2000,
          );
          await tester.tapAt(
            boardRect.topLeft +
                Offset(boardRect.width * 7.5 / 8, boardRect.height * .5 / 8),
          );
        } else if (tool == 'Off') {
          const effect = LitchcraftEffectStyle.off;
          expected.applyEffect(square: 7, mask: 15, effect: effect);
          await tester.tap(find.byKey(const ValueKey('craft-motion')));
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(find.byKey(ValueKey('craft-effect-${effect.name}')));
          await tester.pump();
          final boardRect = tester.getRect(find.byType(LitchcraftCanvasView));
          await tester.tapAt(
            boardRect.topLeft +
                Offset(boardRect.width * 7.5 / 8, boardRect.height * .5 / 8),
          );
        } else {
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 300));
          if (tool.startsWith('New frame')) {
            for (
              var index = 0;
              index < (tool == 'New frame' ? 1 : 3);
              index++
            ) {
              undoExpected = expected.document;
              expected.addFrame();
              await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
              // Rebuild the accepted local preview without draining native writes.
              await tester.pump();
            }
          } else if (tool == 'Delete cancel') {
            final target = await _revealFrame(tester, 0);
            await tester.pumpAndSettle();
            expect(target.hitTestable(), findsOneWidget);
            await tester.longPress(target);
            await tester.pump(const Duration(milliseconds: 300));
            await tester.tap(find.text('Cancel').last);
          } else if (tool == 'Delete frame') {
            expected.deleteSelectedFrame();
            await tester.longPress(
              find.byKey(ValueKey('craft-select-frame-$source')),
            );
            await tester.pump(const Duration(milliseconds: 300));
            await tester.tap(
              find.byKey(const ValueKey('craft-confirm-delete-frame')),
            );
          } else {
            expected.setSelectedFrameDuration(1000);
            final slider = find.byKey(const ValueKey('craft-frame-duration'));
            final renderSlider = tester.renderObject<RenderBox>(
              find.descendant(
                of: slider,
                matching: find.byWidgetPredicate(
                  (widget) =>
                      widget.runtimeType.toString() ==
                      '_SliderRenderObjectWidget',
                ),
              ),
            );
            // Use Flutter's resolved track geometry, including its default
            // thumb/overlay insets; the old coarse stops hid inset errors.
            final resolvedTheme =
                (renderSlider as dynamic).sliderTheme as SliderThemeData;
            final track = resolvedTheme.trackShape!.getPreferredRect(
              parentBox: renderSlider,
              sliderTheme: resolvedTheme,
              isDiscrete: false,
            );
            // The common control runs from Slower to Faster; its continuous
            // coordinate is the inverse of the exact duration in milliseconds.
            double valueForDuration(int durationMs) =>
                (litchcraftAnimationMinimumFrameDurationMs +
                        litchcraftAnimationMaximumFrameDurationMs -
                        durationMs)
                    .toDouble();
            Offset atDuration(int durationMs) {
              final currentTrack = resolvedTheme.trackShape!.getPreferredRect(
                parentBox: renderSlider,
                sliderTheme: resolvedTheme,
                isDiscrete: false,
              );
              debugPrint(
                'NATIVE_DURATION target=$durationMs initial=$track current=$currentTrack origin=${renderSlider.localToGlobal(Offset.zero)}',
              );
              return renderSlider.localToGlobal(
                Offset(
                  currentTrack.left +
                      currentTrack.width *
                          (valueForDuration(durationMs) -
                              litchcraftAnimationMinimumFrameDurationMs) /
                          (litchcraftAnimationMaximumFrameDurationMs -
                              litchcraftAnimationMinimumFrameDurationMs),
                  currentTrack.center.dy,
                ),
              );
            }

            final finger = await tester.startGesture(atDuration(350));
            await finger.moveTo(atDuration(2000));
            await journey.tick();
            expect(tester.widget<Slider>(slider).value, valueForDuration(2000));
            expect(find.text('2.0 s'), findsOneWidget);
            expect(
              controller.documentSession.document.frames[source].durationMs,
              350,
            );
            // A genuine accepted Tool State rebuilds the mounted screen while
            // the finger still owns the duration preview.
            await journey.finish(controller.setBrush(targetMask: 5));
            expect(controller.targetMask, 5);
            expect(tester.widget<Slider>(slider).value, valueForDuration(2000));
            expect(find.text('2.0 s'), findsOneWidget);
            expect(
              controller.documentSession.document.frames[source].durationMs,
              350,
            );
            await finger.moveTo(atDuration(1000));
            await journey.tick();
            expect(tester.widget<Slider>(slider).value, valueForDuration(1000));
            expect(find.text('1.0 s'), findsOneWidget);
            expect(
              controller.documentSession.document.frames[source].durationMs,
              350,
            );
            await finger.up();
          }
        }
        await tester.pump();
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 10000,
          reason: 'Tool $tool must settle its original invocation',
        );
        final nativeState =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        final packets = ble.writes
            .skip(writesBefore)
            .where((w) => w['channel'] == 'connection_session')
            .map(
              (w) => ConnectionSessionCodec.decode(
                Uint8List.fromList(w['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where((p) => p.kind == ConnectionSessionKind.ordered)
            .toList();
        debugPrint(
          'STORE_TOOL $tool source=$source appSelected=${controller.documentSession.selectedFrameIndex} '
          'nativeDisplayed=${nativeState['selected_frame']} appFrames=${controller.documentSession.document.frames.length} '
          'nativeFrames=${nativeState['frame_count']} status=${controller.status} '
          'ordered=${packets.map((p) => '${p.code}:${p.body.toList()}').toList()} '
          'requests=${_litchcraftRequests(ble, writesBefore).map((p) => p.take(12).toList()).toList()} '
          'legacyWrites=${ble.writes.skip(writesBefore).where((w) => w['channel'] == 'litchcraft').length} '
          'resultExpected=applied oneUndo=entire-tool next=Undo',
        );
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        expect(
          nativeState['frame_count'],
          expected.document.frames.length,
          reason: '$tool must change the board, not just its local preview',
        );
        expect(
          nativeState['selected_frame'],
          expected.selectedFrameIndex,
          reason:
              '$tool must display the same surviving/inserted frame as the phone',
        );
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
        if (tool == 'Delete cancel') {
          expect(
            packets,
            isEmpty,
            reason: 'Cancel must not select or mutate the native frame',
          );
          expect(controller.documentSession.selectedFrameIndex, source);
          await tester.pumpWidget(const SizedBox.shrink());
          await journey.finish(service.disconnect());
          return;
        }
        if (tool.endsWith('three')) {
          expect(
            packets.where(
              (p) => p.code == (tool.startsWith('Rotate') ? 9 : 10),
            ),
            hasLength(3),
          );
          expect(packets.where((p) => p.code == 3 || p.code == 4), isEmpty);
        }
        final structuralHistory =
            tool.startsWith('New frame') || tool == 'Delete frame';
        final historyWritesBefore = ble.writes.length;
        await journey.finish(controller.undo());
        if (structuralHistory) {
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 500,
            reason: 'Known structural Undo must retire without readback',
          );
        }
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            undoExpected,
          ),
          isTrue,
          reason: 'One Undo restores the complete tool preimage',
        );
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(controller.redo());
        if (structuralHistory) {
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 500,
            reason: 'Known structural Redo must retire without readback',
          );
          expect(
            _litchcraftRequests(ble, historyWritesBefore).where(
              (packet) =>
                  packet[0] == LitchcraftV2Protocol.magic &&
                  (packet[2] == LitchcraftV2Protocol.frameStoreSnapshot ||
                      packet[2] == LitchcraftV2Protocol.frameStoreBegin),
            ),
            isEmpty,
            reason:
                'Exact structural history needs no Store read or replacement',
          );
        }
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            expected.document,
          ),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: Store mounted nonempty replacement resets effects after delayed clock halves',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _DelayedEffectClockBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() => ble.disposeJourney(tester, service));
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
      });
      LitchcraftArtwork artwork(String id, int square, int rgb) {
        final draft =
            LitchcraftDocumentSession(
              staticLitchcraftDocument(
                LitchcraftCanvas.blank().paint([square], 15, rgb),
              ),
            )..applyEffect(
              square: square,
              mask: 15,
              effect: LitchcraftEffectStyle.flash,
              effectDurationMs: 2000,
            );
        return LitchcraftArtwork.animated(
          id: id,
          name: id,
          document: draft.document,
          createdAt: DateTime.utc(2026),
          modifiedAt: DateTime.utc(2026),
        );
      }

      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      await journey.finish(library.ready);
      final oldArtwork = artwork('old_flash', 7, 0xcc3300);
      final newArtwork = artwork('new_flash', 0, 0x00cc66);
      await journey.finish(library.open(oldArtwork));
      await ble.flush(tester);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              monotonicMilliseconds: () => journey.now,
            ),
          ),
        ),
      );
      await ble.advance(tester, 1250);
      final oldFrame = await journey.frame();
      final oldElapsed = controller.animationEffectElapsedMilliseconds;
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors
            .skip(28)
            .take(4),
        everyElement(0),
      );
      expect((oldFrame['board'] as List)[7], everyElement([0, 0, 0, 0]));
      final oldSource = controller.documentSession.replacementGeneration;
      final predecessorClocks = List<VirtualPacket>.of(ble.seenEffectClocks);
      debugPrint(
        'STORE_CLOCK old source=$oldSource nativeEffectAdvanceMs=1250 appEffectMs=$oldElapsed '
        'nativeMs=${oldFrame['now_ms']} occupancy=${oldFrame['occupancy']} '
        'nativeLED7=${oldFrame['board'][7]} appRGB7=0 paused=true '
        'next=open-New-flash',
      );

      ble.holdEffectClocks = true;
      await journey.finish(library.open(newArtwork));
      await journey.until(
        () => ble.heldEffectClocks.length >= 2 && !ble.hasPendingOperations,
        budgetMs: 1000,
        reason: 'New Inspect State completes before its native clock pair',
      );
      final clocks = List<VirtualPacket>.of(ble.heldEffectClocks);
      final low = clocks
          .map(_DelayedEffectClockBle.clockPacket)
          .whereType<LitchcraftV2Packet>()
          .singleWhere((p) => p.event == LitchcraftV2Protocol.effectClockLow);
      final high = clocks
          .map(_DelayedEffectClockBle.clockPacket)
          .whereType<LitchcraftV2Packet>()
          .singleWhere((p) => p.event == LitchcraftV2Protocol.effectClockHigh);
      int half(LitchcraftV2Packet packet) => ByteData.sublistView(
        Uint8List.fromList(packet.payload),
      ).getUint32(3, Endian.little);
      final nativeResetElapsed = half(low) | half(high) << 32;
      expect(
        nativeResetElapsed,
        0,
        reason: 'Commit starts the new effect phase',
      );
      expect(low.session, high.session);
      expect(low.requestId, high.requestId);
      expect(low.payload.take(2), high.payload.take(2));
      expect(
        controller.documentSession.replacementGeneration,
        greaterThan(oldSource),
      );
      expect(controller.pending, isFalse);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.animationEffectElapsedMilliseconds, isNull);
      final reset = await journey.frame();
      expect((reset['board'] as List)[0], everyElement([0, 204, 102, 0]));
      expect((reset['board'] as List)[7], everyElement([0, 0, 0, 0]));
      debugPrint(
        'STORE_CLOCK committed source=${controller.documentSession.replacementGeneration} '
        'nativeEffectMs=$nativeResetElapsed nativeMs=${reset['now_ms']} '
        'nativeLED0=${reset['board'][0]} '
        'appRGB0=${tester.widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView)).canvas.colors.first} '
        'request=${low.requestId} heldHalves=${clocks.length} '
        'State=accepted clock=pending next=deliver-low-then-high',
      );

      ble.holdEffectClocks = false;
      ble.heldEffectClocks.clear();
      for (final clock in clocks) {
        ble.deliver(clock);
        await tester.pump();
      }
      await ble.advance(tester, 50);
      final rebound = await journey.frame();
      final app = tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .canvas;
      debugPrint(
        'STORE_CLOCK rebound source=${controller.documentSession.replacementGeneration} '
        'appEffectMs=${controller.animationEffectElapsedMilliseconds} '
        'nativeMs=${rebound['now_ms']} nativeLED0=${rebound['board'][0]} '
        'appRGB0=${app.colors.first} result=clock-pair-delivered '
        'next=ignore-predecessor-clock-work',
      );
      expect(
        app.colors.take(4),
        everyElement(0x00cc66),
        reason:
            'The mounted new artwork must use its reset native phase after delayed halves',
      );
      expect((rebound['board'] as List)[0], everyElement([0, 204, 102, 0]));
      final reboundEffectElapsed =
          controller.animationEffectElapsedMilliseconds;
      expect(reboundEffectElapsed, 50);
      expect(predecessorClocks, isNotEmpty);
      // Already queued predecessor notifications cannot reanchor this source.
      for (final clock in predecessorClocks) {
        ble.deliver(clock);
        await tester.pump();
      }
      await ble.advance(tester, 50);
      final afterPredecessor = await journey.frame();
      expect(
        controller.animationEffectElapsedMilliseconds,
        reboundEffectElapsed! + 50,
        reason: 'Predecessor callbacks cannot seek the current effect clock',
      );
      expect(
        (afterPredecessor['board'] as List)[0],
        everyElement([0, 204, 102, 0]),
      );
      debugPrint(
        'STORE_CLOCK predecessor-rejected source=${controller.documentSession.replacementGeneration} '
        'oldCallbacks=${predecessorClocks.length} appEffectMs=${controller.animationEffectElapsedMilliseconds} '
        'nativeMs=${afterPredecessor['now_ms']} nativeLED0=${afterPredecessor['board'][0]} '
        'result=current-phase-advanced-50-to-100 next=continue-new-artwork',
      );
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors
            .take(4),
        everyElement(0x00cc66),
        reason: 'Rebinding must not apply clock work from the old artwork',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final historyPath in <String>[
    'currentFrame256',
    'known',
    'unknown',
    'unknownReverseBeforeReceipt',
    'unknownMixedObject',
    'unknownThenFill',
    'unknownSplit',
    'unknownSplitForeign',
    'unknownSplitForeignUseBoard',
    'unknownSplitForeignUseApp',
    'unknownSplitForeignChangedBoard',
  ]) {
    final currentFrame256 = historyPath == 'currentFrame256';
    final retainsNativeBaselineHistory =
        historyPath != 'known' && !currentFrame256;
    final laterFill = historyPath == 'unknownThenFill';
    final reverseBeforeReceipt = historyPath == 'unknownReverseBeforeReceipt';
    final mixedObject = historyPath == 'unknownMixedObject';
    final splitForeign = historyPath.startsWith('unknownSplitForeign');
    final chooseBoard = historyPath == 'unknownSplitForeignUseBoard';
    final chooseApp = historyPath == 'unknownSplitForeignUseApp';
    final changedBoard = historyPath == 'unknownSplitForeignChangedBoard';
    final splitUnknown = historyPath == 'unknownSplit' || splitForeign;
    testWidgets(
      currentFrame256
          ? 'Litchcraft: cold 256-frame chooser observes current frame then explicitly adopts Board'
          : changedBoard
          ? 'Litchcraft: cold Board choice split foreign preview rejects dirty Hall then retries Board'
          : chooseApp
          ? 'Litchcraft: cold Board choice split foreign repair explicitly keeps App'
          : chooseBoard
          ? 'Litchcraft: cold Board choice split foreign repair explicitly adopts Board'
          : splitForeign
          ? 'Litchcraft: cold Board choice split unknown inverse retains choice after foreign Hall'
          : splitUnknown
          ? 'Litchcraft: cold Board choice split unknown inverse waits for complete LIVE repair'
          : laterFill
          ? 'Litchcraft: cold Board choice held unknown Redo precedes later mounted Fill'
          : mixedObject
          ? 'Litchcraft: cold Board choice complete mixed inverse and Hall Object converges'
          : reverseBeforeReceipt
          ? 'Litchcraft: cold Board choice unknown uniform reverse before receipt retains pending then converges'
          : retainsNativeBaselineHistory
          ? 'Litchcraft: cold Board choice unknown uniform history completes before quiet save'
          : 'Litchcraft: cold Board choice fast circle stays one stroke through authority ahead',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedLiveLayoutBle(native, operationIntervalMs: 20);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        addTearDown(() => ble.disposeJourney(tester, service));
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        final seed = LitchcraftController(service);
        await journey.finish(seed.enter());
        final singleBaseline = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint(
            List.generate(24, (i) => i),
            15,
            0xff3b00,
          ),
        );
        final baseline = currentFrame256
            ? LitchcraftAnimationDocument(
                frames: List.generate(
                  256,
                  (index) => LitchcraftAnimationFrame(
                    durationMs: 100 + index,
                    emitters: List.generate(
                      256,
                      (emitter) => emitter == 200
                          ? LitchcraftAnimatedEmitter(color: 0x440000 | index)
                          : singleBaseline.frames.single.emitters[emitter],
                    ),
                  ),
                ),
                topology: singleBaseline.topology,
                favoriteColors: singleBaseline.favoriteColors,
                playbackOnEntry: false,
                sparkleSeed: singleBaseline.sparkleSeed,
              )
            : singleBaseline;
        // Ten distinct Off squares plus the remaining blank footprint become
        // exactly eleven inverse groups after one real native Canvas Fill.
        final nativePreimage = staticLitchcraftDocument(
          splitUnknown
              ? List.generate(10, (i) => i).fold<LitchcraftCanvas>(
                  LitchcraftCanvas.blank(),
                  (canvas, square) =>
                      canvas.paint([square], 15, (square + 1) << 16 | 0x2244),
                )
              : LitchcraftCanvas.blank(),
        );
        await journey.finish(
          seed.transferFrameStoreForDiagnostics(
            frames: retainsNativeBaselineHistory
                ? nativePreimage.frames
                : baseline.frames,
            playbackOnEntry: false,
          ),
        );
        if (currentFrame256) {
          await journey.finish(seed.replaceDocument(baseline));
          await journey.finish(seed.selectAnimationFrame(108));
        }
        if (retainsNativeBaselineHistory) {
          await journey.finish(seed.replaceDocument(nativePreimage));
          expect(seed.usesFrameStoreDocument, isTrue);
          await journey.finish(seed.setBrush(color: 0xff3b00, targetMask: 15));
          if (splitUnknown) {
            await journey.finish(seed.fillAll());
          } else {
            seed.beginStroke();
            seed.paintSquares(List.generate(24, (i) => i));
            seed.endStroke();
          }
          await journey.until(
            () => !seed.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          await _expectCleanStoreParity(journey, seed);
        }
        final persistedPreimage = seed.documentSession.document;
        await journey.finish(seed.setBrush(color: 0xff7430, targetMask: 15));
        seed.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
        await journey.finish(service.connect(_boardId));
        final traceStart = journey.now;
        final probe = _StoreHistoryProbe(clock: () => journey.now - traceStart);
        final controller = LitchcraftController(service, appTrace: probe);
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
        });
        await journey.finish(library.ready);
        controller.restoreDocument(
          staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([63], 15, 0x123456),
          ),
        );
        final previewWrites = ble.writes.length;
        await journey.finish(controller.enter());
        expect(controller.status, LitchcraftStatus.conflict);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        final previewDraft = controller.documentSession.document;
        final previewBoard = controller.documentSession.boardDocument;
        int previewReads() => _litchcraftRequests(ble, previewWrites)
            .where(
              (packet) => packet[2] == LitchcraftV2Protocol.frameStoreSnapshot,
            )
            .length;
        expect(
          previewReads(),
          currentFrame256 ? 0 : 1,
          reason:
              'single-frame entry already proves whether the board is empty',
        );
        await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        await journey.until(
          () =>
              controller.conflictBoardPreview != null &&
              !controller.conflictBoardPreviewLoading,
          budgetMs: 10000,
        );
        expect(controller.documentSession.document, same(previewDraft));
        expect(controller.documentSession.boardDocument, same(previewBoard));
        expect(previewReads(), 1);
        final previewRequests = _litchcraftRequests(ble, previewWrites)
            .where(
              (packet) => packet[2] == LitchcraftV2Protocol.frameStoreSnapshot,
            )
            .toList();
        expect(
          previewRequests.single,
          hasLength(19),
          reason:
              'chooser must request the bounded frame projection, never the full root',
        );
        if (currentFrame256) {
          expect(controller.conflictBoardPreview!.colors[200], 0x44006c);
          final nativeFrame =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 108,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(nativeFrame['selected_frame'], 108);
          expect(
            controller.documentSession.selectedFrameIndex,
            0,
            reason:
                'unresolved preview cannot select the one-frame local draft',
          );
        }
        expect(
          controller.conflictBoardPreview!.colors.take(96),
          everyElement(0xff3b00),
        );
        final boardCard = find.byKey(const ValueKey('craft-use-board-artwork'));
        final boardThumbnail = tester.widget<LitchcraftThumbnail>(
          find.descendant(
            of: boardCard,
            matching: find.byType(LitchcraftThumbnail),
          ),
        );
        expect(
          boardThumbnail.canvas.colors,
          controller.conflictBoardPreview!.colors,
        );
        debugPrint(
          'IOS_CONFLICT_PREVIEW beforeChoice selected=${controller.animationSelectedFrameIndex} boardTop24=FF3B00 appSquare63=123456 reads=1 draftUnchanged=true next=ChooseArtwork',
        );
        await tester.tap(find.byKey(const ValueKey('craft-use-board-artwork')));
        await journey.until(
          () =>
              controller.status == LitchcraftStatus.live &&
              !controller.boardArtworkUnresolved &&
              !controller.pending,
          budgetMs: 10000,
        );
        await tester.pump(const Duration(milliseconds: 300));
        expect(controller.usesFrameStoreDocument, isTrue);
        expect(
          controller.documentSession.document.frames,
          hasLength(currentFrame256 ? 256 : 1),
        );
        expect(
          previewReads(),
          currentFrame256 ? 2 : 1,
          reason: currentFrame256
              ? 'Board choice alone reads the animation after its frame preview'
              : 'the verified one-frame preview already is the complete document',
        );
        final adoptedReads = _litchcraftRequests(ble, previewWrites)
            .where(
              (packet) => packet[2] == LitchcraftV2Protocol.frameStoreSnapshot,
            )
            .toList();
        expect(
          adoptedReads.where((packet) => packet.length == 17),
          hasLength(currentFrame256 ? 1 : 0),
        );
        if (currentFrame256) {
          expect(controller.animationSelectedFrameIndex, 108);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              baseline,
            ),
            isTrue,
          );
          await _expectCleanStoreParity(journey, controller);
          debugPrint(
            'CURRENT_FRAME_PREVIEW coldFrames=256 selected=108 boundedFrameReads=1 fullReadsBeforeChoice=0 fullReadsAfterChoice=1 draftPreserved=true nativeParity=true',
          );
          await tester.pumpWidget(const SizedBox.shrink());
          await journey.finish(service.disconnect());
          return;
        }
        if (splitUnknown) {
          expect(controller.documentSession.nextUndoDocument, isNull);
          expect(controller.canUndo, isTrue);
          final filled = controller.documentSession.document;
          final initial =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          expect(initial['dirty'], isFalse);
          final writeStart = ble.writes.length;
          ble.holdPhysicalStoreObjects = true;
          ble.holdLiveLayouts = true;
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          List<VirtualPacket> inverseObjects() => ble.held.where((p) {
            final q = ConnectionSessionCodec.decode(
              Uint8List.fromList(p.bytes),
            );
            return q?.kind == ConnectionSessionKind.object &&
                q?.code == LitchcraftV2Protocol.physicalStoreBatch;
          }).toList();
          await journey.until(
            () =>
                inverseObjects().length == 2 &&
                probe.events.any(
                  (e) => e.event == 'litchcraft.store_history_unknown_receipt',
                ),
            budgetMs: 1000,
          );
          final pieces = inverseObjects();
          final prefix = LitchcraftV2Packet.decode(
            ConnectionSessionCodec.decode(
              Uint8List.fromList(pieces[0].bytes),
            )!.body,
          )!;
          final tail = LitchcraftV2Packet.decode(
            ConnectionSessionCodec.decode(
              Uint8List.fromList(pieces[1].bytes),
            )!.body,
          )!;
          expect(
            prefix.payload[0],
            1,
            reason: 'Same-liveR split prefix needs repair',
          );
          expect(prefix.payload[1], 10);
          expect(tail.payload[0], 0);
          expect(tail.payload[1], 1);
          for (final packet in [prefix, tail]) {
            final data = ByteData.sublistView(
              Uint8List.fromList(packet.payload),
            );
            for (var record = 0; record < packet.payload[1]; record++) {
              final offset = 2 + record * 21;
              expect(
                data.getUint32(offset, Endian.little),
                initial['live_generation'],
              );
              expect(packet.payload[offset + 4], 0);
              expect(packet.payload[offset + 5], 0x1f);
              expect(
                data.getUint32(offset + 17, Endian.little),
                (initial['live_revision'] as int) + 1,
              );
            }
          }
          expect(tail.revision, prefix.revision + 1);
          expect(
            tester
                .widget<IconButton>(find.byKey(const ValueKey('craft-redo')))
                .onPressed,
            isNotNull,
          );
          await tester.tap(find.byKey(const ValueKey('craft-redo')));
          await tester.pump(const Duration(milliseconds: 16));
          for (var piece = 0; piece < pieces.length; piece++) {
            ble.held.remove(pieces[piece]);
            ble.deliver(pieces[piece]);
            final deliveredAt = journey.now;
            await journey.until(
              () => journey.now - deliveredAt >= 500,
              budgetMs: 600,
            );
            final live =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            final ordered = ble.writes
                .skip(writeStart)
                .where((w) => w['channel'] == 'connection_session')
                .map(
                  (w) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(w['bytes'] as List<int>),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .where((q) => q.kind == ConnectionSessionKind.ordered)
                .toList();
            expect(
              controller.pending,
              isTrue,
              reason: 'Neither prefix nor final tail completes unknown history',
            );
            expect(live['dirty'], isTrue);
            expect(live['root_generation'], initial['root_generation']);
            expect(
              live['live_revision'],
              (initial['live_revision'] as int) + 1,
            );
            expect(
              ordered.where(
                (q) => q.code == ConnectionSession.litchcraftRedoOpcode,
              ),
              isEmpty,
              reason: 'Early Redo cannot guard a partial same-liveR common',
            );
            expect(
              probe.events.where(
                (e) => e.event == 'litchcraft.store_history_repair_snapshot',
              ),
              isEmpty,
            );
            debugPrint(
              'IOS_SPLIT ${piece == 0 ? 'prefix' : 'tail'} '
              'records=${piece == 0 ? 10 : 1} nativeR=${live['live_revision']} '
              'SavedG=${live['root_generation']} dirty=true pending=true '
              'RedoUnadmitted=true completeCommon=${controller.documentSession.boardDocument != null && sameLitchcraftDocument(controller.documentSession.boardDocument!, nativePreimage)} '
              'next=complete-LIVE-repair',
            );
          }
          expect(ble.heldLiveLayouts, isNotEmpty);
          if (splitForeign) {
            // The flagged prefix still owns repair after its exact tail. A
            // later real Hall revision cannot substitute for that inverse.
            final shared = await service.openConnectionSession(_boardId);
            expect(shared?.isActivated, isTrue);
            await journey.hall(BigInt.one << 40);
            await ble.advance(tester, 400);
            await journey.hall(BigInt.zero);
            await ble.advance(tester, 200);
            final foreignAuthority =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            expect(foreignAuthority['dirty'], isTrue);
            expect(
              foreignAuthority['root_generation'],
              initial['root_generation'],
            );
            expect(
              foreignAuthority['live_revision'],
              (initial['live_revision'] as int) + 2,
            );
            expect(controller.pending, isTrue);
            final keptDraft = controller.documentSession.document;
            final latest = patchLitchcraftEmitters(
              nativePreimage,
              frame: 0,
              squares: [40],
              mask: 15,
              color: 0xff7430,
            );
            ble.holdPhysicalStoreObjects = false;
            ble.release('connection_session');
            ble.holdLiveLayouts = false;
            for (final packet in ble.heldLiveLayouts) {
              ble.deliver(packet);
            }
            ble.heldLiveLayouts.clear();
            await journey.until(
              () =>
                  controller.status == LitchcraftStatus.conflict &&
                  !controller.pending &&
                  !ble.hasPendingOperations,
              budgetMs: 12000,
              reason:
                  'Repair must retain the choice when foreign Hall follows '
                  'a split unknown inverse',
            );
            final repaired = probe.events
                .lastWhere(
                  (e) => e.event == 'litchcraft.store_history_repair_snapshot',
                )
                .fields;
            expect(
              repaired['expected_revision'],
              (initial['live_revision'] as int) + 1,
            );
            expect(
              repaired['authority_revision'],
              (initial['live_revision'] as int) + 2,
            );
            expect(repaired['authority_dirty'], isTrue);
            expect(controller.conflictBoardPreview, isNotNull);
            expect(shared?.isActivated, isTrue);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                keptDraft,
              ),
              isTrue,
            );
            expect(
              sameLitchcraftDocument(
                controller.documentSession.boardDocument!,
                latest,
              ),
              isTrue,
            );
            List<int> journalActions() => ble.writes
                .skip(writeStart)
                .where((w) => w['channel'] == 'connection_session')
                .map(
                  (w) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(w['bytes'] as List<int>),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .where(
                  (q) =>
                      q.kind == ConnectionSessionKind.ordered &&
                      (q.code == ConnectionSession.litchcraftUndoOpcode ||
                          q.code == ConnectionSession.litchcraftRedoOpcode ||
                          q.code == ConnectionSession.litchcraftPaintOpcode ||
                          q.code == ConnectionSession.litchcraftEffectOpcode ||
                          q.code ==
                              ConnectionSession.litchcraftStoreCanvasOpcode),
                )
                .map((q) => q.code)
                .toList();
            expect(journalActions(), [ConnectionSession.litchcraftUndoOpcode]);
            final cleanSaved = await _expectCleanStoreParity(
              journey,
              controller,
            );
            await ble.advance(tester, 500);
            expect(controller.status, LitchcraftStatus.conflict);
            expect(shared?.isActivated, isTrue);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                keptDraft,
              ),
              isTrue,
            );
            expect(journalActions(), [ConnectionSession.litchcraftUndoOpcode]);
            Future<Map<String, dynamic>> expectCleanNative(
              LitchcraftAnimationDocument expected,
            ) async {
              final observed =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': 0,
                      }, flushOperations: false))['store_frame']
                      as Map<String, dynamic>;
              expect(expected.frames, hasLength(1));
              expect(observed['dirty'], isFalse);
              expect(observed['live_generation'], observed['root_generation']);
              expect(observed['frame_count'], expected.frames.length);
              expect(observed['topology'], expected.topology.id);
              expect(observed['playback_on_entry'], expected.playbackOnEntry);
              expect(observed['sparkle_seed'], expected.sparkleSeed);
              expect(observed['favorite_colors'], expected.favoriteColors);
              expect(observed['frame_index'], 0);
              expect(observed['selected_frame'], 0);
              expect(observed['active'], isTrue);
              expect(observed['playing'], isFalse);
              expect(
                observed['duration_ms'],
                expected.frames.single.durationMs,
              );
              expect(observed['emitters'], [
                for (final emitter in expected.frames.single.emitters)
                  [emitter.color, emitter.effect.id, emitter.effectDurationMs],
              ]);
              return observed;
            }

            // Read the actual clean root after the retention interval without
            // another quiet-save wait; all 256 emitters and metadata must stay.
            final retainedNative = await expectCleanNative(latest);
            expect(retainedNative['root_generation'], cleanSaved);
            expect(
              retainedNative['live_revision'],
              repaired['authority_revision'],
            );
            debugPrint(
              'IOS_SPLIT_FOREIGN prefix/tailRedoUnadmitted=true '
              'expectedJournalR=${repaired['expected_revision']} '
              'verifiedLiveAuthorityR=${repaired['authority_revision']} '
              'retainedChoice=true draftPreserved=true sessionActivated=true '
              'allMetadataParity=exact next=ChooseArtwork',
            );
            if (chooseBoard || chooseApp || changedBoard) {
              // Quiet save advances G and invalidates the earlier dirty LIVE
              // preview. The chooser reads current proof before either choice.
              await journey.finish(controller.prepareConflictBoardPreview());
              var chosen = chooseApp ? keptDraft : latest;
              var choiceNative = retainedNative;
              if (changedBoard) {
                // A real new Hall edit invalidates the seeded clean preview.
                // Its dirty root cannot satisfy Use Board until a clean retry.
                expect(controller.conflictBoardPreview, isNotNull);
                await journey.hall(BigInt.one << 41);
                await ble.advance(tester, 400);
                await journey.hall(BigInt.zero);
                await ble.advance(tester, 200);
                final changedAuthority =
                    (await ble.command(tester, {
                          'op': 'store_frame',
                          'index': 0,
                        }, flushOperations: false))['store_frame']
                        as Map<String, dynamic>;
                expect(changedAuthority['dirty'], isTrue);
                expect(
                  changedAuthority['root_generation'],
                  retainedNative['root_generation'],
                );
                expect(
                  changedAuthority['live_revision'],
                  (retainedNative['live_revision'] as int) + 1,
                );
                expect(controller.conflictBoardPreview, isNull);
                expect(controller.conflictBoardPreviewFailed, isTrue);
                expect(controller.status, LitchcraftStatus.conflict);
                expect(controller.frameStoreArtworkReadPending, isTrue);
                expect(
                  sameLitchcraftDocument(
                    controller.documentSession.document,
                    keptDraft,
                  ),
                  isTrue,
                );
                Object? refusal;
                try {
                  await journey.finish(
                    controller.resolveConflict(useBoard: true),
                  );
                } catch (error) {
                  refusal = error;
                }
                expect(refusal, isA<StateError>());
                expect(controller.status, LitchcraftStatus.conflict);
                expect(controller.frameStoreArtworkReadPending, isTrue);
                expect(controller.conflictBoardPreview, isNull);
                expect(controller.conflictBoardPreviewFailed, isTrue);
                expect(
                  sameLitchcraftDocument(
                    controller.documentSession.document,
                    keptDraft,
                  ),
                  isTrue,
                );
                expect(shared?.isActivated, isTrue);
                expect(journalActions(), [
                  ConnectionSession.litchcraftUndoOpcode,
                ]);
                debugPrint(
                  'IOS_SPLIT_FOREIGN_DIRTY Hall=41 '
                  'G=${changedAuthority['root_generation']} R=${changedAuthority['live_revision']} '
                  'previewInvalidated=true UseBoard=rejected draftPreserved=true '
                  'retainedChoice=true retainedRedo=notReplayed next=Retry',
                );
                chosen = patchLitchcraftEmitters(
                  latest,
                  frame: 0,
                  squares: [41],
                  mask: 15,
                  color: 0xff7430,
                );
                await ble.advance(tester, 6000);
                await journey.finish(controller.prepareConflictBoardPreview());
                expect(controller.conflictBoardPreview, isNotNull);
                expect(
                  controller.conflictBoardPreview!.colors,
                  chosen.frames.single.emitters.map((e) => e.color),
                );
                expect(controller.conflictBoardPreviewFailed, isFalse);
                expect(controller.status, LitchcraftStatus.conflict);
                expect(
                  sameLitchcraftDocument(
                    controller.documentSession.document,
                    keptDraft,
                  ),
                  isTrue,
                );
                choiceNative = await expectCleanNative(chosen);
                expect(
                  choiceNative['root_generation'],
                  (retainedNative['root_generation'] as int) + 1,
                );
                expect(
                  choiceNative['live_revision'],
                  changedAuthority['live_revision'],
                );
              }
              expect(controller.frameStoreArtworkReadPending, isTrue);
              expect(controller.conflictBoardPreview, isNotNull);
              await journey.finish(
                controller.resolveConflict(useBoard: !chooseApp),
              );
              await journey.until(
                () =>
                    controller.status == LitchcraftStatus.live &&
                    !controller.pending &&
                    !ble.hasPendingOperations,
                budgetMs: 10000,
              );
              expect(controller.frameStoreArtworkReadPending, isFalse);
              expect(controller.conflictBoardPreview, isNull);
              expect(controller.documentSession.hasCommonDocument, isTrue);
              expect(
                sameLitchcraftDocument(
                  controller.documentSession.document,
                  chosen,
                ),
                isTrue,
              );
              expect(
                sameLitchcraftDocument(
                  controller.documentSession.boardDocument!,
                  chosen,
                ),
                isTrue,
              );
              final chosenNative = await expectCleanNative(chosen);
              if (!chooseApp) {
                expect(
                  chosenNative['root_generation'],
                  choiceNative['root_generation'],
                );
                expect(
                  chosenNative['live_revision'],
                  choiceNative['live_revision'],
                );
              } else {
                expect(
                  chosenNative['root_generation'],
                  retainedNative['root_generation'],
                );
                expect(
                  chosenNative['live_revision'],
                  (retainedNative['live_revision'] as int) + 1,
                );
                final selection = _litchcraftRequests(ble, writeStart);
                expect(
                  selection.where(
                    (p) => p[2] == LitchcraftV2Protocol.frameStoreSelect,
                  ),
                  hasLength(1),
                );
                expect(
                  selection.where(
                    (p) =>
                        p[2] == LitchcraftV2Protocol.frameStoreBegin ||
                        p[2] == LitchcraftV2Protocol.frameStoreChunk ||
                        p[2] == LitchcraftV2Protocol.frameStoreCommit,
                  ),
                  isEmpty,
                );
              }
              await ble.advance(tester, 500);
              expect(controller.status, LitchcraftStatus.live);
              expect(shared?.isActivated, isTrue);
              expect(journalActions(), [
                ConnectionSession.litchcraftUndoOpcode,
              ]);
              expect(await expectCleanNative(chosen), chosenNative);
              debugPrint(
                'IOS_SPLIT_FOREIGN_CHOICE explicit=${chooseApp ? 'App' : 'Board'} status=live '
                'G=${chosenNative['root_generation']} R=${chosenNative['live_revision']} '
                'native/app/boardCommon=exact all256Metadata=exact retainedRedo=notReplayed',
              );
            }
            await journey.finish(library.flush());
            await journey.finish(service.disconnect());
            return;
          }
          ble.holdPhysicalStoreObjects = false;
          ble.release('connection_session');
          ble.holdLiveLayouts = false;
          for (final packet in ble.heldLiveLayouts) {
            ble.deliver(packet);
          }
          ble.heldLiveLayouts.clear();
          await journey.until(
            () =>
                !controller.pending &&
                !ble.hasPendingOperations &&
                sameLitchcraftDocument(
                  controller.documentSession.document,
                  filled,
                ) &&
                controller.documentSession.boardDocument != null &&
                sameLitchcraftDocument(
                  controller.documentSession.boardDocument!,
                  filled,
                ),
            budgetMs: 12000,
            reason:
                'Genuine split inverse repairs before the one retained Redo',
          );
          expect(
            probe.events.any(
              (e) => e.event == 'litchcraft.store_history_repair_snapshot',
            ),
            isTrue,
          );
          final repaired = probe.events
              .lastWhere(
                (e) => e.event == 'litchcraft.store_history_repair_snapshot',
              )
              .fields;
          expect(repaired['authority_dirty'], isTrue);
          expect(
            repaired['authority_revision'],
            (initial['live_revision'] as int) + 1,
          );
          final beforeSave =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          expect(beforeSave['dirty'], isTrue);
          expect(beforeSave['root_generation'], initial['root_generation']);
          expect(
            beforeSave['live_revision'],
            (initial['live_revision'] as int) + 2,
          );
          final actions = ble.writes
              .skip(writeStart)
              .where((w) => w['channel'] == 'connection_session')
              .map(
                (w) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(w['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (q) =>
                    q.kind == ConnectionSessionKind.ordered &&
                    q.code != ConnectionSession.litchcraftStoreGuardOpcode,
              )
              .map((q) => q.code)
              .toList();
          expect(actions, [
            ConnectionSession.litchcraftUndoOpcode,
            ConnectionSession.litchcraftRedoOpcode,
          ]);
          await _expectCleanStoreParity(journey, controller);
          expect(controller.status, LitchcraftStatus.live);
          debugPrint(
            'IOS_SPLIT repaired=true native/common/metadata=exact '
            'sameEarlyRedo=once next=continue-drawing',
          );
          await journey.finish(service.disconnect());
          return;
        }
        final before = controller.documentSession.document;
        if (retainsNativeBaselineHistory) {
          expect(
            sameLitchcraftDocument(before, persistedPreimage),
            isTrue,
            reason:
                'The adopted preimage is the complete document saved by seed',
          );
        }
        final writesBefore = ble.writes.length;
        List<ConnectionSessionPacket> paints() => ble.writes
            .skip(writesBefore)
            .where((w) => w['channel'] == 'connection_session')
            .map(
              (w) => ConnectionSessionCodec.decode(
                Uint8List.fromList(w['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where(
              (p) =>
                  p.kind == ConnectionSessionKind.ordered &&
                  p.code == ConnectionSession.litchcraftPaintOpcode,
            )
            .toList();
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        Offset at(int col, int row) =>
            rect.topLeft +
            Offset(rect.width * (col + .5) / 8, rect.height * (row + .5) / 8);
        final start = journey.now;
        final pointer = await tester.startGesture(at(2, 4));
        await pointer.moveTo(at(5, 4));
        await tester.pump(const Duration(milliseconds: 16));
        await journey.until(() => paints().isNotEmpty, budgetMs: 1000);
        ble.heldChannels.add('connection_session');
        await journey.until(
          () => ble.held.any((p) {
            final q = ConnectionSessionCodec.decode(
              Uint8List.fromList(p.bytes),
            );
            return q?.kind == ConnectionSessionKind.state &&
                q?.code ==
                    ConnectionSession.litchcraftStoreAuthorityStateCode &&
                q!.body.length == 10 &&
                q.body[1] & 1 != 0;
          }),
          budgetMs: 500,
          reason:
              'Native dirty authority after first Paint; '
              'held=${ble.held.map((p) {
                final q = ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes));
                return "${q?.kind}/${q?.code}/${q?.body}";
              }).toList()}',
        );
        // Deliver an authentic authority observation while its terminal proof is
        // delayed for the bounded remainder of this one fast finger gesture.
        final states = ble.held.where((p) {
          final q = ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes));
          return q?.kind == ConnectionSessionKind.state;
        }).toList();
        for (final p in states) {
          ble.held.remove(p);
          ble.deliver(p);
        }
        await tester.pump();
        await pointer.moveTo(at(6, 6));
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveTo(at(5, 7));
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveTo(at(3, 7));
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveTo(at(2, 6));
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveTo(at(2, 4));
        await pointer.up();
        await tester.pump();
        final painted = controller.documentSession.document;
        final changed = List.generate(64, (i) => i)
            .where(
              (sq) =>
                  painted.frames[0].emitters[sq * 4].color !=
                  before.frames[0].emitters[sq * 4].color,
            )
            .toList();
        expect(changed.length, greaterThan(10));
        ble.heldChannels.remove('connection_session');
        ble.release('connection_session');
        await journey.until(
          () =>
              !controller.pending &&
              !ble.hasPendingOperations &&
              controller.documentSession.boardDocument != null &&
              sameLitchcraftDocument(
                controller.documentSession.boardDocument!,
                painted,
              ),
          budgetMs: 10000,
          reason: 'One circle tail must fully reach board common',
        );
        debugPrint(
          'IOS_CIRCLE drain_ms=${journey.now - start} changed=$changed '
          'paint=${paints().map((p) => p.body.toList()).toList()}',
        );
        await tester.tap(find.byKey(const ValueKey('craft-undo')));
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 10000,
        );
        final boardUndo =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        debugPrint(
          'IOS_CIRCLE oneUndo revision=${boardUndo['live_revision']} '
          'local_changed=${changed.where((sq) => controller.canvas.colors[sq * 4] != 0).toList()} '
          'document_changed=${changed.where((sq) => controller.documentSession.canvas.colors[sq * 4] != 0).toList()} '
          'canUndo=${controller.canUndo} canRedo=${controller.canRedo}',
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, before),
          isTrue,
          reason: 'One Undo removes the entire one-finger circle',
        );
        expect(
          controller.canvas.colors,
          before.frames[0].emitters.map((e) => e.color),
        );
        await _expectCleanStoreParity(journey, controller);
        expect(
          paints().where((p) => p.body[3] & 1 != 0),
          hasLength(1),
          reason:
              'Authority-ahead finger-up must not split the gesture into catch-up strokes',
        );
        expect(paints().where((p) => p.body[3] & 2 != 0), hasLength(1));
        if (retainsNativeBaselineHistory) {
          // The cold phone knows its circle, but the original24-square native
          // journal entry remains unknown. Return to the baseline while dirty.
          await tester.tap(find.byKey(const ValueKey('craft-redo')));
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          expect(controller.documentSession.nextUndoDocument, isNull);
          expect(controller.canUndo, isTrue);
          final beforeUnknown =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          expect(beforeUnknown['dirty'], isTrue);
          final writesBeforeUnknown = ble.writes.length;
          final emittedBeforeUnknown = ble.emitted.length;
          final unknownStarted = journey.now;
          ble.holdPhysicalStoreObjects = true;
          if (laterFill) ble.heldChannels.add('connection_session');
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          await journey.until(
            () => ble.held.any((p) {
              final q = ConnectionSessionCodec.decode(
                Uint8List.fromList(p.bytes),
              );
              return q?.kind == ConnectionSessionKind.object &&
                  q?.code == LitchcraftV2Protocol.physicalStoreBatch;
            }),
            budgetMs: 1000,
          );
          if (laterFill) {
            // Release genuine inverse/State observations while retaining the
            // actual Undo checkpoint. Uniform history must not need a saved
            // root, and the single early Redo keeps its occupied ordered tail.
            for (var pass = 0; pass < 30; pass++) {
              final observations = ble.held.where((p) {
                final q = ConnectionSessionCodec.decode(
                  Uint8List.fromList(p.bytes),
                );
                return !(q?.kind == ConnectionSessionKind.control &&
                    q?.code == ConnectionSessionControl.checkpoint);
              }).toList();
              for (final p in observations) {
                ble.held.remove(p);
                ble.deliver(p);
              }
              await journey.tick();
              if (ble.held.any((p) {
                final q = ConnectionSessionCodec.decode(
                  Uint8List.fromList(p.bytes),
                );
                return q?.kind == ConnectionSessionKind.control &&
                    q?.code == ConnectionSessionControl.checkpoint;
              })) {
                break;
              }
            }
            expect(
              ble.held.any((p) {
                final q = ConnectionSessionCodec.decode(
                  Uint8List.fromList(p.bytes),
                );
                return q?.kind == ConnectionSessionKind.control &&
                    q?.code == ConnectionSessionControl.checkpoint;
              }),
              isTrue,
              reason: 'The genuine Undo receipt remains held before early Redo',
            );
          }
          expect(
            probe.events
                .lastWhere(
                  (e) => e.event == 'litchcraft.store_history_requested',
                )
                .fields['known_inverse'],
            isFalse,
          );
          debugPrint(
            'IOS_UNKNOWN pending dirty=${beforeUnknown['dirty']} '
            'localUndo=${controller.documentSession.canUndo} nativeUndo=${controller.canUndo} '
            'heldInverse=${ble.held.length}',
          );
          expect(
            tester
                .widget<IconButton>(find.byKey(const ValueKey('craft-redo')))
                .onPressed,
            isNotNull,
          );
          await tester.tap(find.byKey(const ValueKey('craft-redo')));
          await tester.pump(const Duration(milliseconds: 16));
          debugPrint('IOS_UNKNOWN oneRedoTappedBeforeCoherence=true');
          var expected = before;
          if (laterFill) {
            await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
            await tester.pump(const Duration(milliseconds: 16));
            expected = controller.documentSession.document;
            expect(
              expected.frames[0].emitters.every((e) => e.color == 0xff7430),
              isTrue,
            );
            debugPrint(
              'IOS_UNKNOWN laterFill preview=FF7430 selected=0 order=Undo,Redo,Fill',
            );
          }
          ble.holdPhysicalStoreObjects = false;
          ble.heldChannels.remove('connection_session');
          ble.release('connection_session');
          final unknownStart = journey.now;
          try {
            await journey.until(
              () =>
                  sameLitchcraftDocument(
                    controller.documentSession.document,
                    expected,
                  ) &&
                  controller.documentSession.boardDocument != null &&
                  sameLitchcraftDocument(
                    controller.documentSession.boardDocument!,
                    expected,
                  ) &&
                  !controller.pending &&
                  !ble.hasPendingOperations,
              budgetMs: 1000,
              reason:
                  'The SAME early Redo and later Fill must become exact common '
                  'before the 5-second quiet save, without a saved-root snapshot',
            );
          } finally {
            final observed =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            debugPrint(
              'IOS_UNKNOWN preQuiet elapsed_ms=${journey.now - unknownStarted} '
              'drain_ms=${journey.now - unknownStart} '
              'G=${observed['root_generation']} R=${observed['live_revision']} '
              'dirty=${observed['dirty']} pending=${controller.pending} '
              'sameCommon=${controller.documentSession.boardDocument != null && sameLitchcraftDocument(controller.documentSession.boardDocument!, expected)} '
              'snapshots=${_litchcraftRequests(ble, writesBeforeUnknown).where((p) => p[2] == LitchcraftV2Protocol.frameStoreSnapshot).length}',
            );
          }
          final live =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          expect(journey.now - unknownStarted, lessThan(5000));
          expect(live['dirty'], isTrue);
          expect(live['root_generation'], beforeUnknown['root_generation']);
          expect(live['live_generation'], beforeUnknown['live_generation']);
          expect(
            live['live_revision'],
            (beforeUnknown['live_revision'] as int) + (laterFill ? 3 : 2),
          );
          expect(
            _litchcraftRequests(
              ble,
              writesBeforeUnknown,
            ).where((p) => p[2] == LitchcraftV2Protocol.frameStoreSnapshot),
            isEmpty,
            reason:
                'Uniform 24-square inverses use live records, not saved bytes',
          );
          expect(
            probe.events.where(
              (e) => e.event == 'litchcraft.store_history_repair_snapshot',
            ),
            isEmpty,
          );
          for (final entry in <String, Object?>{
            'frame_count': expected.frames.length,
            'topology': expected.topology.id,
            'playback_on_entry': expected.playbackOnEntry,
            'sparkle_seed': expected.sparkleSeed,
            'favorite_colors': expected.favoriteColors,
            'selected_frame': controller.documentSession.selectedFrameIndex,
            'active': controller.boardActive,
            'playing': controller.animationPlaying,
          }.entries) {
            expect(live[entry.key], entry.value, reason: entry.key);
          }
          final nativeFrame = await journey.frame();
          expect(
            nativeFrame['board'],
            List.generate(64, (square) {
              final colors = List.generate(4, (slot) {
                final color =
                    expected.frames[0].emitters[square * 4 + slot].color;
                return [color >> 16 & 255, color >> 8 & 255, color & 255];
              });
              // The legacy ABI exposes Left, averaged Top/Bottom, Right.
              // V3 retains all four independent semantic emitters internally.
              return [
                [...colors[0], 0],
                [
                  for (var channel = 0; channel < 3; channel++)
                    (colors[1][channel] + colors[3][channel] + 1) ~/ 2,
                  0,
                ],
                [...colors[2], 0],
              ];
            }),
            reason:
                'Native legacy RGB projection must match the pre-save common',
          );
          final inverseBatches = ble.emitted
              .skip(emittedBeforeUnknown)
              .where((p) => p.channel == 'connection_session')
              .map(
                (p) =>
                    ConnectionSessionCodec.decode(Uint8List.fromList(p.bytes)),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (p) =>
                    p.kind == ConnectionSessionKind.object &&
                    p.code == LitchcraftV2Protocol.physicalStoreBatch,
              )
              .map((p) => LitchcraftV2Packet.decode(p.body))
              .whereType<LitchcraftV2Packet>()
              .toList();
          expect(inverseBatches, hasLength(2));
          for (var index = 0; index < inverseBatches.length; index++) {
            final payload = inverseBatches[index].payload;
            final data = ByteData.sublistView(Uint8List.fromList(payload));
            expect(payload[0], 0, reason: 'Uniform inverse has no overflow');
            expect(payload[1], 1, reason: 'One exact 24-square footprint');
            expect(
              data.getUint32(2, Endian.little),
              beforeUnknown['live_generation'],
            );
            expect(payload[6], 0);
            expect(
              payload[7],
              0x1f,
              reason: 'All emitters restore Off/zero duration',
            );
            expect(
              payload.sublist(8, 11),
              index == 0 ? [0, 0, 0] : [255, 59, 0],
            );
            expect(data.getUint64(11, Endian.little), 0xffffff);
            expect(
              data.getUint32(19, Endian.little),
              (beforeUnknown['live_revision'] as int) + index + 1,
            );
          }
          expect(controller.status, LitchcraftStatus.live);
          debugPrint(
            'IOS_UNKNOWN liveCommonExact=true nativeLegacyRGBProjectionExact=true '
            'OffMetadata=authoritative-clear-mask frameDuration=${expected.frames[0].durationMs} '
            'SavedGUnchanged=true noSnapshot=true order=Undo,Redo${laterFill ? ',Fill' : ''} '
            'next=quiet-durable-save',
          );
          // Seed's whole-document parity established this saved preimage.
          // Undo/early Redo returns to it; a later mounted Fill differs and
          // must publish exactly one new generation after the quiet deadline.
          final restoresSavedPreimage = sameLitchcraftDocument(
            before,
            expected,
          );
          expect(restoresSavedPreimage, !laterFill);
          final saved = await _expectCleanStoreParity(journey, controller);
          expect(
            saved,
            (beforeUnknown['root_generation'] as int) +
                (restoresSavedPreimage ? 0 : 1),
          );
          expect(
            controller.canvas.colors,
            expected.frames[0].emitters.map((e) => e.color),
          );
          final unknownActions = ble.writes
              .skip(writesBeforeUnknown)
              .where((w) => w['channel'] == 'connection_session')
              .map(
                (w) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(w['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where((q) => q.kind == ConnectionSessionKind.ordered)
              .toList();
          expect(
            unknownActions.where(
              (q) => q.code == ConnectionSession.litchcraftUndoOpcode,
            ),
            hasLength(1),
          );
          expect(
            unknownActions.where(
              (q) => q.code == ConnectionSession.litchcraftRedoOpcode,
            ),
            hasLength(1),
          );
          expect(
            unknownActions.where(
              (q) =>
                  q.code == ConnectionSession.litchcraftPaintOpcode ||
                  q.code == ConnectionSession.litchcraftEffectOpcode,
            ),
            isEmpty,
            reason:
                'Unknown inverse repair cannot replay artwork as paint/effect',
          );
          expect(
            unknownActions
                .where(
                  (q) => q.code != ConnectionSession.litchcraftStoreGuardOpcode,
                )
                .map((q) => q.code),
            <int>[
              ConnectionSession.litchcraftUndoOpcode,
              ConnectionSession.litchcraftRedoOpcode,
              if (laterFill) ConnectionSession.litchcraftStoreCanvasOpcode,
            ],
            reason:
                'The held journal intent keeps its ordered position before later Fill',
          );
          debugPrint(
            'IOS_UNKNOWN nativeActions=${unknownActions.map((q) => q.code).toList()} '
            'allMetadataParity=exact next=Undo',
          );
          if (!laterFill) {
            // Both complete FIFO patches describe ordinary same-source artwork.
            // R45 Hall paint is a successor, never confirmation of inverse R44.
            final shared = await service.openConnectionSession(_boardId);
            expect(shared?.isActivated, isTrue);
            final receiptsBefore = probe.events
                .where(
                  (e) =>
                      e.event == 'litchcraft.store_history_unknown_receipt' &&
                      e.fields['opcode'] == LitchcraftProtocol.undo,
                )
                .length;
            ble.holdPhysicalStoreObjects = true;
            ble.historyWritesFrom = ble.writes.length;
            ble.holdHistoryCheckpoints = reverseBeforeReceipt;
            await tester.tap(find.byKey(const ValueKey('craft-undo')));
            await journey.until(
              () => reverseBeforeReceipt
                  ? ble.heldHistoryCheckpoints.isNotEmpty
                  : probe.events
                            .where(
                              (e) =>
                                  e.event ==
                                      'litchcraft.store_history_unknown_receipt' &&
                                  e.fields['opcode'] == LitchcraftProtocol.undo,
                            )
                            .length >
                        receiptsBefore,
              budgetMs: 1000,
            );
            if (!reverseBeforeReceipt) {
              final receipt = probe.events.lastWhere(
                (e) =>
                    e.event == 'litchcraft.store_history_unknown_receipt' &&
                    e.fields['opcode'] == LitchcraftProtocol.undo,
              );
              expect(receipt.fields['revision'], 44);
            }
            if (reverseBeforeReceipt) {
              final inverse = ble.held.firstWhere((p) {
                final q = ConnectionSessionCodec.decode(
                  Uint8List.fromList(p.bytes),
                );
                return q?.kind == ConnectionSessionKind.object &&
                    q?.code == LitchcraftV2Protocol.physicalStoreBatch;
              });
              ble.deliver(inverse);
              await journey.until(
                () =>
                    controller.documentSession.boardDocument != null &&
                    sameLitchcraftDocument(
                      controller.documentSession.boardDocument!,
                      nativePreimage,
                    ),
                budgetMs: 200,
                reason: 'Complete inverse projects before its terminal receipt',
              );
              expect(controller.pending, isTrue);
              expect(controller.status, LitchcraftStatus.live);
              expect(
                probe.events
                    .where(
                      (e) =>
                          e.event ==
                              'litchcraft.store_history_unknown_receipt' &&
                          e.fields['opcode'] == LitchcraftProtocol.undo,
                    )
                    .length,
                receiptsBefore,
              );
              ble.holdHistoryCheckpoints = false;
              for (final packet in ble.heldHistoryCheckpoints) {
                ble.deliver(packet);
              }
              ble.heldHistoryCheckpoints.clear();
              await journey.until(
                () =>
                    probe.events
                        .where(
                          (e) =>
                              e.event ==
                                  'litchcraft.store_history_unknown_receipt' &&
                              e.fields['opcode'] == LitchcraftProtocol.undo,
                        )
                        .length >
                    receiptsBefore,
                budgetMs: 1000,
              );
            }
            await journey.hall(BigInt.one << 40);
            await ble.advance(tester, 400);
            await journey.hall(BigInt.zero);
            await ble.advance(tester, 200);
            final foreignAuthority =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }, flushOperations: false))['store_frame']
                    as Map<String, dynamic>;
            expect(foreignAuthority['live_revision'], 45);
            expect(foreignAuthority['dirty'], isTrue);
            final fifo = <int, LitchcraftV2Packet>{};
            for (final packet in ble.held) {
              if (packet.channel != 'connection_session') continue;
              final envelope = ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              );
              if (envelope?.kind != ConnectionSessionKind.object ||
                  envelope?.code != LitchcraftV2Protocol.physicalStoreBatch) {
                continue;
              }
              final batch = LitchcraftV2Packet.decode(envelope!.body)!;
              final prior = fifo[envelope.sequence];
              if (prior != null) expect(batch.payload, prior.payload);
              fifo[envelope.sequence] = batch;
            }
            expect(fifo, hasLength(2));
            final sequence = fifo.keys.toList();
            expect(sequence[1], sequence[0] + 1);
            for (var index = 0; index < 2; index++) {
              final batch = fifo[sequence[index]]!;
              final data = ByteData.sublistView(
                Uint8List.fromList(batch.payload),
              );
              expect(batch.payload[0], 0);
              expect(batch.payload[1], 1);
              expect(data.getUint32(2, Endian.little), saved);
              expect(batch.payload[6], 0);
              expect(batch.payload[7], index == 0 ? 0x1f : 0x0f);
              expect(
                batch.payload.sublist(8, 11),
                index == 0 ? [0, 0, 0] : [255, 116, 48],
              );
              expect(
                data.getUint64(11, Endian.little),
                index == 0 ? 0xffffff : 1 << 40,
              );
              expect(data.getUint32(19, Endian.little), 44 + index);
            }
            final latest = patchLitchcraftEmitters(
              nativePreimage,
              frame: 0,
              squares: [40],
              mask: 15,
              color: 0xff7430,
            );
            if (mixedObject) {
              // Controlled packaging of two genuine native records into the
              // one complete Object that the native FIFO can also emit. Keep
              // the real transport sequence/token and deliver its second
              // original envelope too, so native Progress remains unchanged.
              // Native authority, field bytes and final physical R are intact.
              final first = ble.held.firstWhere((p) {
                final q = ConnectionSessionCodec.decode(
                  Uint8List.fromList(p.bytes),
                );
                return q?.kind == ConnectionSessionKind.object &&
                    q?.code == LitchcraftV2Protocol.physicalStoreBatch;
              });
              final q = ConnectionSessionCodec.decode(
                Uint8List.fromList(first.bytes),
              )!;
              final body = Uint8List.fromList([
                ...q.body.take(13),
                0,
                2,
                ...fifo[sequence[0]]!.payload.skip(2),
                ...fifo[sequence[1]]!.payload.skip(2),
              ]);
              ByteData.sublistView(
                body,
              ).setUint32(7, fifo[sequence[1]]!.revision, Endian.little);
              final combined = ConnectionSessionCodec.encode(
                ConnectionSessionPacket(
                  kind: q.kind,
                  code: q.code,
                  token: q.token,
                  sequence: q.sequence,
                  body: body,
                ),
              );
              expect(LitchcraftV2Packet.decode(body)!.payload[1], 2);
              ble.held[ble.held.indexOf(first)] = VirtualPacket(
                first.channel,
                combined,
                first.epoch,
                first.pipe,
              );
            }
            ble.holdPhysicalStoreObjects = false;
            ble.release('connection_session');
            await ble.advance(tester, 6000);
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 1500,
            );
            expect(controller.status, LitchcraftStatus.live);
            expect(shared?.isActivated, isTrue);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                latest,
              ),
              isTrue,
            );
            expect(
              sameLitchcraftDocument(
                controller.documentSession.boardDocument!,
                latest,
              ),
              isTrue,
            );
            await _expectCleanStoreParity(journey, controller);
            await ble.advance(tester, 500);
            expect(controller.status, LitchcraftStatus.live);
            expect(shared?.isActivated, isTrue);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                latest,
              ),
              isTrue,
            );
            debugPrint(
              'IOS_UNKNOWN completeFIFO inverseR=44 ordinaryHallR=45 '
              'latestArtworkExact=true allMetadataParity=exact '
              'sessionActivated=true next=continue-drawing',
            );
          }
          await journey.finish(service.disconnect());
          return;
        }
        for (var repeat = 0; repeat < 2; repeat++) {
          await tester.tap(find.byKey(const ValueKey('craft-redo')));
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              painted,
            ),
            isTrue,
          );
          await _expectCleanStoreParity(journey, controller);
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          expect(
            sameLitchcraftDocument(controller.documentSession.document, before),
            isTrue,
          );
          await _expectCleanStoreParity(journey, controller);
        }
        final historyBeforeRapid = probe.events
            .where((e) => e.event == 'litchcraft.store_history_receipt')
            .length;
        for (var repeat = 0; repeat < 3; repeat++) {
          expect(
            tester
                .widget<IconButton>(find.byKey(const ValueKey('craft-redo')))
                .onPressed,
            isNotNull,
            reason: 'Redo is actually enabled before the fast tap',
          );
          await tester.tap(find.byKey(const ValueKey('craft-redo')));
          await tester.pump(const Duration(milliseconds: 16));
          expect(
            tester
                .widget<IconButton>(find.byKey(const ValueKey('craft-undo')))
                .onPressed,
            isNotNull,
            reason: 'Undo is actually enabled before the fast tap',
          );
          await tester.tap(find.byKey(const ValueKey('craft-undo')));
          await tester.pump(const Duration(milliseconds: 16));
        }
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
          budgetMs: 10000,
        );
        final rapidReceipts = probe.events
            .where((e) => e.event == 'litchcraft.store_history_receipt')
            .skip(historyBeforeRapid)
            .toList();
        debugPrint(
          'IOS_CIRCLE rapidHistory receipts=${rapidReceipts.map((e) => e.fields).toList()}',
        );
        expect(
          rapidReceipts,
          hasLength(6),
          reason: 'Every enabled rapid Undo/Redo reaches its native receipt',
        );
        expect(
          rapidReceipts.map((e) => e.fields['history_applied']),
          everyElement(true),
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, before),
          isTrue,
        );
        expect(
          controller.canvas.colors,
          before.frames[0].emitters.map((e) => e.color),
        );
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: resident read gates drawing until board baseline is ready',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));

      final first = LitchcraftController(service);
      await journey.finish(first.enter());
      final frames = List<LitchcraftAnimationFrame>.generate(
        2,
        (index) => LitchcraftAnimationFrame(
          durationMs: 100,
          emitters: List<LitchcraftAnimatedEmitter>.generate(
            256,
            (emitter) => LitchcraftAnimatedEmitter(
              color: emitter == 0 ? 0x102030 + index : 0,
            ),
          ),
        ),
      );
      await journey.finish(
        first.transferFrameStoreForDiagnostics(
          frames: frames,
          playbackOnEntry: false,
        ),
      );
      first.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));

      final second = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
          localIntent: false,
        ),
      );
      addTearDown(second.dispose);
      await journey.finish(second.enter());
      expect(second.status, LitchcraftStatus.live);
      expect(second.boardArtworkLoading, isTrue);
      final before = second.canvas;
      second.beginStroke();
      second.paintSquares(const [7]);
      second.endStroke();
      expect(await second.rotateArtwork(1), isFalse);
      expect(second.canvas, before);
      expect(second.status, LitchcraftStatus.live);
      await journey.until(
        () =>
            !second.boardArtworkUnresolved ||
            second.status != LitchcraftStatus.live,
        budgetMs: 10000,
      );
      expect(second.status, LitchcraftStatus.live);
      expect(second.boardArtworkUnresolved, isFalse);
      expect(second.boardArtworkLoading, isFalse);
      expect(second.documentSession.boardDocument?.frames, hasLength(2));
      second.beginStroke();
      second.paintSquares(const [7]);
      second.endStroke();
      await journey.until(
        () =>
            second
                .documentSession
                .boardDocument
                ?.frames[0]
                .emitters[28]
                .color ==
            second.color,
        budgetMs: 10000,
      );
      expect(second.status, LitchcraftStatus.live);
      expect(
        second.documentSession.boardDocument!.frames[0].emitters[28].color,
        second.color,
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: disconnect during resident read restores offline drawing',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      final first = LitchcraftController(service);
      await journey.finish(first.enter());
      await journey.finish(
        first.transferFrameStoreForDiagnostics(
          frames: List<LitchcraftAnimationFrame>.generate(
            2,
            (index) => LitchcraftAnimationFrame(
              durationMs: 100,
              emitters: List<LitchcraftAnimatedEmitter>.generate(
                256,
                (emitter) => LitchcraftAnimatedEmitter(
                  color: emitter == 0 ? 0x102030 + index : 0,
                ),
              ),
            ),
          ),
          playbackOnEntry: false,
        ),
      );
      first.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));

      final local = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
        localIntent: false,
      );
      final second = LitchcraftController(service, documentSession: local);
      addTearDown(second.dispose);
      await journey.finish(second.enter());
      expect(second.boardArtworkUnresolved, isTrue);
      await journey.finish(service.disconnect());
      expect(second.status, LitchcraftStatus.offline);
      expect(second.boardArtworkUnresolved, isFalse);
      final before = second.canvas;
      second.beginStroke();
      second.paintSquares(const [7]);
      second.endStroke();
      expect(second.canvas, isNot(before));
      expect(local.hasLocalIntent, isTrue);
      await tester.pump(const Duration(seconds: 7));
      expect(second.status, LitchcraftStatus.offline);
      expect(second.canvas, isNot(before));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: mounted Fill held Clear and swatch are single Store Canvas actions',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final document =
          LitchcraftDocumentSession(
              staticLitchcraftDocument(LitchcraftCanvas.blank()),
            )
            ..applyEffect(
              square: 7,
              mask: 15,
              effect: LitchcraftEffectStyle.breathe,
              effectDurationMs: 1600,
            )
            ..addFrame();
      final first = LitchcraftController(service, documentSession: document);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(first.enter());
      expect(first.boardActive, isTrue, reason: first.errorMessage);
      await journey.finish(
        first.transferFrameStoreForDiagnostics(
          frames: document.document.frames,
          playbackOnEntry: false,
        ),
      );
      first.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final controller = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          document.document,
          localIntent: false,
        ),
      );
      addTearDown(controller.dispose);
      await journey.finish(controller.enter());
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      await journey.finish(controller.selectAnimationFrame(1));
      final library = LitchcraftLibrary(controller);
      await journey.finish(library.ready);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await _expectCleanStoreParity(journey, controller);
      final before = controller.documentSession.document;
      final fillStart = ble.writes.length;
      Future<int> liveRevision() async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 1,
              }))['store_frame']['live_revision']
              as int;
      final fillRevision = await liveRevision();
      void previewCheckpoint(String action) {
        final frame = controller
            .documentSession
            .document
            .frames[controller.animationSelectedFrameIndex];
        debugPrint(
          'CANVAS_JOURNEY $action.preview selected=${controller.animationSelectedFrameIndex} '
          'RGB0=${frame.emitters.first.color.toRadixString(16)} '
          'effect7=${frame.emitters[28].effect.id} duration7=${frame.emitters[28].effectDurationMs} '
          'frameDuration=${frame.durationMs} undo=${controller.canUndo} redo=${controller.canRedo} '
          'next=wait-for-receipt',
        );
      }

      Future<void> acceptedCheckpoint(String action, String next) async {
        final selected = controller.animationSelectedFrameIndex;
        final board =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': selected,
                }))['store_frame']
                as Map<String, dynamic>;
        final rendered = await journey.frame();
        final frame = controller.documentSession.document.frames[selected];
        debugPrint(
          'CANVAS_JOURNEY $action.accepted selected=$selected displayed=${board['selected_frame']} '
          'playing=${board['playing']} result=applied revision=${board['live_revision']} '
          'RGB0=${frame.emitters.first.color.toRadixString(16)} LED0=${rendered['board'][0]} '
          'effect7=${frame.emitters[28].effect.id} duration7=${frame.emitters[28].effectDurationMs} '
          'frameDuration=${frame.durationMs} all256-RGB/effect/duration=exact '
          'toolMask=${controller.targetMask} undo=${controller.canUndo} redo=${controller.canRedo} next=$next',
        );
      }

      ble.holdPhysicalStoreObjects = true;
      void expectOneCanvas(int start, int action) {
        final ordered = ble.writes
            .skip(start)
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              ),
            )
            .where((packet) => packet?.kind == ConnectionSessionKind.ordered)
            .cast<ConnectionSessionPacket>()
            .toList();
        final canvas = ordered.where((packet) => packet.code == 9).toList();
        expect(canvas, hasLength(1));
        expect(canvas.single.body, hasLength(6));
        expect(canvas.single.body[0], 1);
        expect(canvas.single.body[5], action);
        expect(
          ordered.where((packet) => packet.code == 3 || packet.code == 4),
          isEmpty,
        );
        expect(
          ordered.where((packet) => packet.code == 5),
          isEmpty,
          reason:
              'Canvas carries Fill tool selection without a physical footprint',
        );
        expect(
          ble.held.where((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.object &&
                decoded?.code == LitchcraftV2Protocol.physicalStoreBatch;
          }),
          isEmpty,
          reason: 'Phone Canvas has no reverse square replay',
        );
      }

      ble.holdConnectionObservations = true;
      ble.heldChannels.add('litchcraft');
      ble.heldChannels.add('console');
      await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
      previewCheckpoint('Fill');
      expect(controller.canvas.colors, everyElement(controller.color));
      await journey.until(() => !controller.pending, budgetMs: 1000);
      expectOneCanvas(fillStart, 0);
      expect(await liveRevision(), fillRevision + 1);
      final filled = controller.documentSession.document;
      for (var index = 0; index < 256; index++) {
        expect(
          filled.frames[1].emitters[index].effect,
          before.frames[1].emitters[index].effect,
        );
        expect(
          filled.frames[1].emitters[index].effectDurationMs,
          before.frames[1].emitters[index].effectDurationMs,
        );
      }
      expect(filled.frames[1].durationMs, before.frames[1].durationMs);
      expect(
        ble.held.any((packet) {
          if (packet.channel != 'connection_session') return false;
          final decoded = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return decoded?.kind == ConnectionSessionKind.state ||
              decoded?.kind == ConnectionSessionKind.patch;
        }),
        isTrue,
        reason: 'The receipt must overtake a held board observation',
      );
      expect(
        controller.canvas.colors,
        everyElement(controller.color),
        reason:
            'Visible Fill must survive a receipt ahead of board observations',
      );
      expect(
        controller.canvas.colors,
        controller.documentSession.canvas.colors,
        reason: 'Fill preview must not roll back after its ordered receipt',
      );
      ble.holdConnectionObservations = false;
      ble.heldChannels.remove('litchcraft');
      ble.heldChannels.remove('console');
      ble.release('connection_session');
      ble.release('litchcraft');
      ble.release('console');
      await ble.flush(tester);
      await _expectCleanStoreParity(journey, controller);
      await acceptedCheckpoint('Fill', 'Undo');
      await journey.finish(controller.setBrush(color: 0x916271, targetMask: 5));
      ble.holdConnectionObservations = true;
      ble.heldChannels.add('litchcraft');
      ble.heldChannels.add('console');
      final clearStart = ble.writes.length;
      final clearRevision = await liveRevision();
      final pointer = await tester.startGesture(
        tester.getCenter(find.byKey(const ValueKey('craft-quick-fill'))),
      );
      await tester.pump(const Duration(milliseconds: 600));
      await pointer.up();
      await tester.pump();
      previewCheckpoint('held-Clear');
      expect(controller.canvas.colors, everyElement(0));
      await journey.until(() => !controller.pending, budgetMs: 1000);
      expectOneCanvas(clearStart, 1);
      expect(await liveRevision(), clearRevision + 1);
      expect(
        controller.documentSession.document.frames[1].emitters.every(
          (emitter) =>
              emitter.color == 0 &&
              emitter.effect == LitchcraftEffectStyle.off &&
              emitter.effectDurationMs == 0,
        ),
        isTrue,
      );
      expect(
        controller.documentSession.document.frames[1].durationMs,
        filled.frames[1].durationMs,
      );
      expect(
        controller.canvas.colors,
        everyElement(0),
        reason:
            'Visible Clear must survive a receipt ahead of board observations',
      );
      expect(
        controller.canvas.colors,
        controller.documentSession.canvas.colors,
        reason: 'Clear preview must not roll back after its ordered receipt',
      );
      ble.holdConnectionObservations = false;
      ble.heldChannels.remove('litchcraft');
      ble.heldChannels.remove('console');
      ble.release('connection_session');
      ble.release('litchcraft');
      ble.release('console');
      await ble.flush(tester);
      await _expectCleanStoreParity(journey, controller);
      expect(controller.color, 0x916271);
      expect(controller.targetMask, 5);
      void expectNativeTool(int color) {
        final state = ble.emitted
            .where((packet) => packet.channel == 'connection_session')
            .map(
              (packet) => ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              ),
            )
            .where(
              (packet) =>
                  packet?.kind == ConnectionSessionKind.state &&
                  packet?.code == ConnectionSession.litchcraftStateCode &&
                  packet?.body.length == 10,
            )
            .cast<ConnectionSessionPacket>()
            .last;
        expect(state.body[4] & 4, 0);
        expect(state.body[5], 5);
        expect(LitchcraftProtocol.readRgb(state.body, 6), color);
      }

      expectNativeTool(0x916271);
      await acceptedCheckpoint('held-Clear', 'Undo');
      ble.holdPhysicalStoreObjects = false;
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await journey.until(() => !controller.pending, budgetMs: 1500);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, filled),
        isTrue,
      );
      await _expectCleanStoreParity(journey, controller);
      await acceptedCheckpoint('Undo', 'Redo');
      await tester.tap(find.byKey(const ValueKey('craft-redo')));
      await journey.until(() => !controller.pending, budgetMs: 1500);
      await _expectCleanStoreParity(journey, controller);
      await acceptedCheckpoint('Redo', 'Undo');
      ble.holdPhysicalStoreObjects = true;
      final swatchStart = ble.writes.length;
      final swatches = find
          .byWidgetPredicate(
            (widget) =>
                widget.key is ValueKey<String> &&
                (widget.key! as ValueKey<String>).value.startsWith(
                  'craft-wheel-color-',
                ),
          )
          .hitTestable();
      final swatch = swatches.first;
      final hold = await tester.startGesture(tester.getCenter(swatch));
      await tester.pump(const Duration(milliseconds: 800));
      await hold.up();
      await tester.pump();
      previewCheckpoint('held-swatch');
      await journey.until(() => !controller.pending, budgetMs: 1000);
      expectOneCanvas(swatchStart, 0);
      expect(controller.eraser, isFalse);
      expect(controller.targetMask, 5);
      for (var index = 0; index < 256; index++) {
        expect(
          controller.canvas.colors[index],
          5 & (1 << (index % 4)) != 0 ? controller.color : 0,
        );
      }
      await _expectCleanStoreParity(journey, controller);
      expectNativeTool(controller.color);
      await acceptedCheckpoint('held-swatch', 'Undo');
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(library.flush());
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: mounted no-op Clear keeps absolute intent after hidden same-frame Hall paint',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(service);
      addTearDown(() async {
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      final draft = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      )..addFrame();
      await journey.finish(controller.replaceDocument(draft.document));
      await journey.finish(controller.selectAnimationFrame(0));
      await journey.finish(
        controller.setBrush(color: 0xaabbcc, targetMask: 15),
      );
      await _expectCleanStoreParity(journey, controller);
      final blank = controller.documentSession.document;
      final library = LitchcraftLibrary(controller);
      await journey.finish(library.ready);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      ble.holdConnectionObservations = true;
      ble.holdPhysicalStoreObjects = true;
      await journey.hall(BigInt.one << 7);
      await ble.advance(tester, 400);
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 6400);
      final marked =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']
              as Map<String, dynamic>;
      expect(
        (marked['emitters'] as List).skip(28).take(4),
        everyElement([0xaabbcc, 0, 0]),
      );
      expect(
        sameLitchcraftDocument(controller.documentSession.document, blank),
        isTrue,
        reason:
            'The phone still shows blank while the same-frame Hall mark is genuinely withheld',
      );
      final authority = ble.held.where((packet) {
        final decoded = ConnectionSessionCodec.decode(
          Uint8List.fromList(packet.bytes),
        );
        return decoded?.kind == ConnectionSessionKind.state &&
            decoded?.code ==
                ConnectionSession.litchcraftStoreAuthorityStateCode;
      }).toList();
      expect(authority, isNotEmpty);
      for (final packet in authority) {
        ble.held.remove(packet);
        ble.deliver(packet);
      }
      await tester.pump();
      final start = ble.writes.length;
      List<ConnectionSessionPacket> actions() => ble.writes
          .skip(start)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .where(
            (packet) =>
                packet?.kind == ConnectionSessionKind.ordered &&
                [3, 4, 6, 9].contains(packet?.code),
          )
          .cast<ConnectionSessionPacket>()
          .toList();
      final hold = await tester.startGesture(
        tester.getCenter(find.byKey(const ValueKey('craft-quick-fill'))),
      );
      await tester.pump(const Duration(milliseconds: 600));
      await hold.up();
      await tester.pump();
      expect(
        sameLitchcraftDocument(controller.documentSession.document, blank),
        isTrue,
        reason:
            'Actual held Clear is locally a no-op, but retains an absolute erase intent',
      );
      expect(
        actions(),
        isEmpty,
        reason: 'Canvas waits for the actual ahead Hall authority',
      );
      ble.holdConnectionObservations = false;
      ble.holdPhysicalStoreObjects = false;
      ble.release('connection_session');
      await ble.flush(tester);
      await journey.until(
        () =>
            actions().any((packet) => packet.code == 9) && !controller.pending,
        budgetMs: 10000,
      );
      await ble.advance(tester, 6000);
      await ble.flush(tester);
      final cleared =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']
              as Map<String, dynamic>;
      debugPrint(
        'CANVAS_NOOP_CLEAR actions=${actions().map((packet) => '${packet.code}/${packet.body}').toList()} '
        'phone7=${controller.canvas.colors[28].toRadixString(16)} native7=${(cleared['emitters'] as List)[28]}',
      );
      expect(
        sameLitchcraftDocument(controller.documentSession.document, blank),
        isTrue,
        reason:
            'The hidden Hall RGB must not replace the held Clear preview or catch up after Canvas',
      );
      expect(actions().map((packet) => packet.code), [9]);
      expect(actions().single.body, [0, 15, 0, 0, 0, 1]);
      expect(cleared['live_revision'], (marked['live_revision'] as int) + 1);
      expect(cleared['emitters'], everyElement([0, 0, 0]));
      await _expectCleanStoreParity(journey, controller);
      debugPrint(
        'CANVAS_JOURNEY locally-noop-heldClear selected=0 displayed=0 result=applied '
        'RGB=blank effect=Off duration=0 all512-fields=exact Canvas=1 Paint/Effect=0 '
        'undo=${controller.canUndo} redo=${controller.canRedo} next=Undo-restore-hidden-Hall',
      );
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await journey.until(
        () =>
            actions().any((packet) => packet.code == 6) && !controller.pending,
        budgetMs: 10000,
      );
      await _expectCleanStoreParity(journey, controller);
      final restored =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']
              as Map<String, dynamic>;
      expect(restored['emitters'], marked['emitters']);
      expect(restored['live_revision'], (cleared['live_revision'] as int) + 1);
      expect(actions().map((packet) => packet.code), [9, 6]);
      debugPrint(
        'CANVAS_JOURNEY locally-noop-heldClear-Undo selected=0 displayed=0 result=applied '
        'RGB7=aabbcc effect=Off duration=0 all512-fields=exact wire=Canvas,Undo '
        'next=Redo',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: mounted blocked Fill preserves newer Store library effect',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(service);
      addTearDown(() async {
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      final draft = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      )..addFrame();
      await journey.finish(controller.replaceDocument(draft.document));
      await journey.finish(controller.selectAnimationFrame(0));
      await journey.finish(
        controller.setBrush(color: 0xaabbcc, targetMask: 15),
      );
      await _expectCleanStoreParity(journey, controller);
      final library = LitchcraftLibrary(controller);
      await journey.finish(library.ready);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      });
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      ble.holdConnectionObservations = true;
      ble.holdPhysicalStoreObjects = true;
      await journey.hall(BigInt.one << 63);
      await ble.advance(tester, 400);
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 6400);
      final authority = ble.held.where((packet) {
        final decoded = ConnectionSessionCodec.decode(
          Uint8List.fromList(packet.bytes),
        );
        return decoded?.kind == ConnectionSessionKind.state &&
            decoded?.code ==
                ConnectionSession.litchcraftStoreAuthorityStateCode;
      }).toList();
      expect(authority, isNotEmpty);
      for (final packet in authority) {
        ble.held.remove(packet);
        ble.deliver(packet);
      }
      await tester.pump();
      await journey.finish(controller.setBrush(color: 0x112233, targetMask: 1));
      final start = ble.writes.length;
      List<ConnectionSessionPacket> actions() => ble.writes
          .skip(start)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .where(
            (packet) =>
                packet?.kind == ConnectionSessionKind.ordered &&
                [3, 4, 9].contains(packet?.code),
          )
          .cast<ConnectionSessionPacket>()
          .toList();
      await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
      await tester.pump();
      expect(
        actions(),
        isEmpty,
        reason: 'Fill is retained behind the real Hall authority gap',
      );
      expect(library.canEditAnimation, isTrue);
      expect(controller.animationBoardAuthorityLive, isFalse);
      // This is the enabled screen Effects canvas branch's synchronous owner
      // (library.applyEffect), after its actual unawaited quick Fill callback.
      // The inspector selection gesture itself is bypassed in this seam test.
      library.applyEffect(
        square: 7,
        mask: 1,
        effect: LitchcraftEffectStyle.breathe,
        effectDurationMs: 1600,
      );
      expect(
        library.animationDocument.frames[0].emitters[28].effect,
        LitchcraftEffectStyle.breathe,
      );
      ble.holdConnectionObservations = false;
      ble.holdPhysicalStoreObjects = false;
      ble.release('connection_session');
      await journey.until(
        () =>
            controller
                .documentSession
                .boardDocument
                ?.frames[0]
                .emitters[0]
                .color ==
            0x112233,
        budgetMs: 10000,
      );
      expect(
        library.animationDocument.frames[0].emitters[28].effect,
        LitchcraftEffectStyle.breathe,
        reason:
            'A newer independently chosen effect survives Canvas RGB repair',
      );
      expect(
        library.animationDocument.frames[0].emitters[28].effectDurationMs,
        1600,
      );
      await journey.until(
        () =>
            controller
                .documentSession
                .boardDocument
                ?.frames[0]
                .emitters[28]
                .effect ==
            LitchcraftEffectStyle.breathe,
        budgetMs: 10000,
      );
      await _expectCleanStoreParity(journey, controller);
      expect(actions().map((packet) => packet.code), [9, 4, 3]);
      expect(actions().first.body, [0, 1, 0x11, 0x22, 0x33, 0]);
      expect(
        actions().last.body.last,
        0,
        reason:
            'Only the existing empty ordinary stroke close follows the new effect',
      );
      expect(
        controller.documentSession.document.frames[0].emitters[29].effect,
        LitchcraftEffectStyle.off,
      );
      expect(
        controller.documentSession.document.frames[0].emitters[253].color,
        0xaabbcc,
      );
      debugPrint(
        'CANVAS_JOURNEY blocked-Fill/newer-effect occupancy=none selected=0 displayed=0 '
        'result=applied RGB=112233 effect7=breathe duration=1600 other-effects=Off '
        'all512-RGB/effect/duration=exact Canvas=1 Effect=1 Paint=empty-close-only next=Undo',
      );
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final outside in <({int square, int mask, String name})>[
    (square: 1, mask: 1, name: 'checker parity'),
    (square: 0, mask: 2, name: 'emitter mask'),
  ]) {
    testWidgets(
      'Litchcraft: pending Canvas preserves hidden Hall conflict outside ${outside.name}',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        addTearDown(() async {
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.enter());
        final draft = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..addFrame();
        await journey.finish(controller.replaceDocument(draft.document));
        await journey.finish(controller.selectAnimationFrame(0));
        await journey.finish(
          controller.setBrush(color: 0xaabbcc, targetMask: 15),
        );
        await _expectCleanStoreParity(journey, controller);
        ble.holdConnectionObservations = true;
        ble.holdPhysicalStoreObjects = true;
        await journey.hall(BigInt.one << outside.square);
        await ble.advance(tester, 400);
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 6400);
        final authority = ble.held.where((packet) {
          final decoded = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return decoded?.kind == ConnectionSessionKind.state &&
              decoded?.code ==
                  ConnectionSession.litchcraftStoreAuthorityStateCode;
        }).toList();
        expect(authority, isNotEmpty);
        for (final packet in authority) {
          ble.held.remove(packet);
          ble.deliver(packet);
        }
        await tester.pump();
        await journey.finish(
          controller.setBrush(color: 0x112233, targetMask: 5),
        );
        final start = ble.writes.length;
        Object? failed;
        final canvas = controller
            .fillChecker(dark: false)
            .then<void>(
              (_) {},
              onError: (Object error) {
                failed = error;
              },
            );
        await journey.finish(
          controller.setBrush(color: 0x445566, targetMask: outside.mask),
        );
        controller.beginStroke();
        controller.paintSquares([outside.square]);
        controller.endStroke();
        await ble.flush(tester);
        ble.holdConnectionObservations = false;
        ble.holdPhysicalStoreObjects = false;
        ble.release('connection_session');
        await journey.until(
          () => controller.status == LitchcraftStatus.conflict,
          budgetMs: 10000,
        );
        await journey.finish(canvas, budgetMs: 1000);
        expect(failed, isA<LitchcraftTransferCancelled>());
        final domain = ble.writes
            .skip(start)
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              ),
            )
            .where(
              (packet) =>
                  packet?.kind == ConnectionSessionKind.ordered &&
                  [3, 4, 9].contains(packet?.code),
            );
        expect(
          domain,
          isEmpty,
          reason:
              'No Canvas/Paint/Effect overrides hidden foreign RGB outside the footprint',
        );
        final frame =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        expect(
          (frame['emitters'] as List).skip(outside.square * 4).take(4),
          everyElement([0xaabbcc, 0, 0]),
        );
        final local = controller.documentSession.document.frames[0].emitters;
        for (var region = 0; region < 4; region++) {
          if (outside.mask & (1 << region) != 0) {
            expect(
              local[outside.square * 4 + region].color,
              0x445566,
              reason: 'Conflict preserves the newer ordinary paint draft',
            );
          }
        }
        debugPrint(
          'CANVAS_JOURNEY outside-${outside.name} occupancy=none selected=0 '
          'result=conflict board-RGB=aabbcc draft-RGB=445566 Canvas/Paint/Effect=0 '
          'next=existing-artwork-conflict-choice',
        );
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final schedule in <({int frame, bool dense, bool removeEffect})>[
    (frame: 1, dense: false, removeEffect: false),
    (frame: 0, dense: false, removeEffect: false),
    (frame: 0, dense: true, removeEffect: false),
    (frame: 0, dense: false, removeEffect: true),
  ]) {
    final laterFrame = schedule.frame;
    final dense = schedule.dense;
    final removeEffect = schedule.removeEffect;
    testWidgets(
      'Litchcraft: physical Erase no-op Clear learns native effect inverse before later Fill frame $laterFrame dense=$dense removeEffect=$removeEffect',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        addTearDown(() async {
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.enter());
        final draft = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..addFrame();
        draft.selectFrame(0);
        draft.paintSelectedFrame(squares: [0], mask: 15, color: 0x123456);
        draft.applyEffect(
          square: 0,
          mask: 15,
          effect: LitchcraftEffectStyle.breathe,
          effectDurationMs: 1600,
        );
        final source = dense
            ? LitchcraftAnimationDocument(
                frames: [
                  LitchcraftAnimationFrame(
                    durationMs: draft.document.frames.first.durationMs,
                    emitters: List.generate(
                      256,
                      (index) => LitchcraftAnimatedEmitter(
                        color: 0x123456 + index,
                        effect: LitchcraftEffectStyle.breathe,
                        effectDurationMs: 1600,
                      ),
                    ),
                  ),
                  draft.document.frames[1],
                ],
                topology: draft.document.topology,
                favoriteColors: draft.document.favoriteColors,
                playbackOnEntry: false,
                sparkleSeed: draft.document.sparkleSeed,
              )
            : draft.document;
        await journey.finish(controller.replaceDocument(source));
        await journey.finish(controller.selectAnimationFrame(0));
        await journey.finish(controller.setBrush(eraser: true, targetMask: 15));
        await _expectCleanStoreParity(journey, controller);
        ble.holdConnectionObservations = true;
        ble.holdPhysicalStoreObjects = true;
        // A dense production Hall footprint erases the complete frame in
        // one physical stroke; the native journal retains every emitter.
        await journey.hall(
          dense ? (BigInt.one << 64) - BigInt.one : BigInt.one,
        );
        await ble.advance(tester, 400);
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 6400);
        final erased =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        expect((erased['emitters'] as List).take(4), everyElement([0, 0, 0]));
        expect(
          controller.documentSession.document.frames[0].emitters[0].effect,
          LitchcraftEffectStyle.breathe,
          reason:
              'Physical reverse metadata is genuinely withheld from the phone',
        );
        // Deliver only the authentic ahead authority; its effect-bearing reverse
        // objects remain held so Clear must rebase through the ordinary repair.
        final authority = ble.held.where((packet) {
          final decoded = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return decoded?.kind == ConnectionSessionKind.state &&
              decoded?.code ==
                  ConnectionSession.litchcraftStoreAuthorityStateCode;
        }).toList();
        expect(authority, isNotEmpty);
        for (final packet in authority) {
          ble.held.remove(packet);
          ble.deliver(packet);
        }
        await tester.pump();
        final start = ble.writes.length;
        final clear = controller.clear();
        final undo = controller.undo();
        unawaited(undo.catchError((Object _) {}));
        final selection = controller.selectAnimationFrame(laterFrame);
        final fill = controller.fillAllWithColor(0x224466);
        unawaited(fill.catchError((Object _) {}));
        if (removeEffect) {
          final library = LitchcraftLibrary(controller);
          addTearDown(library.dispose);
          expect(library.canEditAnimation, isTrue);
          expect(
            library.selectedFrame.emitters[0].effect,
            LitchcraftEffectStyle.breathe,
          );
          library.applyEffect(
            square: 0,
            mask: 1,
            effect: LitchcraftEffectStyle.off,
          );
          expect(
            library.selectedFrame.emitters[0].effect,
            LitchcraftEffectStyle.off,
          );
        }
        expect(controller.canvas.colors, everyElement(0x224466));
        await ble.flush(tester);
        List<ConnectionSessionPacket> actions() => ble.writes
            .skip(start)
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              ),
            )
            .where(
              (packet) =>
                  packet?.kind == ConnectionSessionKind.ordered &&
                  [3, 4, 6, 7, 9].contains(packet?.code),
            )
            .cast<ConnectionSessionPacket>()
            .toList();
        expect(
          actions(),
          isEmpty,
          reason: 'All canvas/history intents await held authority',
        );
        ble.holdConnectionObservations = false;
        ble.release('connection_session');
        await journey.finish(selection);
        await journey.finish(clear);
        await journey.until(
          () => actions().any((packet) => packet.code == 6),
          budgetMs: 10000,
        );
        expect(
          actions().map((packet) => packet.code),
          [9, 6],
          reason: 'Later Fill waits for the coherent native Undo inverse',
        );
        final noOp = actions().first;
        expect(noOp.body, [0, 15, 0, 0, 0, 1]);
        await ble.command(tester, {'op': 'advance', 'ms': 6000});
        await tester.pump(const Duration(milliseconds: 150));
        await ble.flush(tester);
        final undone =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        expect(
          undone['live_revision'],
          (erased['live_revision'] as int) + 1,
          reason:
              'No-op Clear does not advance revision; Undo restores the physical Erase',
        );
        for (var index = 0; index < (dense ? 256 : 4); index++) {
          expect((undone['emitters'] as List)[index], [
            0x123456 + (dense ? index : 0),
            1,
            1600,
          ]);
        }
        if (dense) {
          final inverses = ble.held
              .map(
                (packet) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(packet.bytes),
                ),
              )
              .where(
                (packet) =>
                    packet?.kind == ConnectionSessionKind.object &&
                    packet?.code == LitchcraftV2Protocol.physicalStoreBatch,
              )
              .cast<ConnectionSessionPacket>()
              .map((packet) => LitchcraftV2Packet.decode(packet.body))
              .where((packet) => packet != null)
              .cast<LitchcraftV2Packet>();
          expect(
            inverses.any((packet) => packet.payload[0] & 1 != 0),
            isTrue,
            reason:
                'Dense native inverse exceeds the physical ring and omits common RGB/effect fields',
          );
        }
        // Release the authentic RGB reverse predecessor so the same reliable
        // Object lane can deliver its saved-root repair. RGB cannot reconstruct
        // the journal's non-off effect/duration inverse; snapshot must learn it.
        ble.holdPhysicalStoreObjects = false;
        ble.release('connection_session');
        await ble.flush(tester);
        await journey.finish(undo, budgetMs: 10000);
        await journey.finish(fill, budgetMs: 10000);
        await ble.flush(tester);
        if (removeEffect) {
          expect(
            controller.documentSession.document.frames[0].emitters[0].effect,
            LitchcraftEffectStyle.off,
            reason:
                'Explicit later Off remains owned even when common was also Off',
          );
          await journey.until(
            () =>
                actions().any((packet) => packet.code == 4) &&
                controller
                        .documentSession
                        .boardDocument
                        ?.frames[0]
                        .emitters[0]
                        .effect ==
                    LitchcraftEffectStyle.off,
            budgetMs: 10000,
          );
        }
        debugPrint(
          'CANVAS_NATIVE_ORDER ${actions().map((packet) => '${packet.code}/${packet.body}').toList()}',
        );
        expect(
          actions().map((packet) => packet.code),
          removeEffect ? [9, 6, 9, 4, 3] : [9, 6, 9],
          reason: removeEffect
              ? 'Only the explicitly newer Off and existing empty stroke close follow Canvas'
              : 'There is no frame0 Paint/Effect catch-up after the later Fill',
        );
        expect(actions().where((packet) => packet.code == 9).last.body, [
          laterFrame,
          15,
          0x22,
          0x44,
          0x66,
          0,
        ]);
        await _expectCleanStoreParity(journey, controller);
        expect(
          controller.documentSession.document.frames[0].emitters
              .take(dense ? 256 : 4)
              .indexed
              .every(
                (entry) =>
                    entry.$2.color == (laterFrame == 0 ? 0x224466 : 0x123456) &&
                    entry.$2.effect ==
                        (removeEffect && entry.$1 == 0
                            ? LitchcraftEffectStyle.off
                            : LitchcraftEffectStyle.breathe) &&
                    entry.$2.effectDurationMs ==
                        (removeEffect && entry.$1 == 0 ? 0 : 1600),
              ),
          isTrue,
        );
        expect(
          controller.documentSession.document.frames[1].emitters.every(
            (emitter) =>
                emitter.color == (laterFrame == 1 ? 0x224466 : 0) &&
                emitter.effect == LitchcraftEffectStyle.off &&
                emitter.effectDurationMs == 0,
          ),
          isTrue,
        );
        debugPrint(
          'CANVAS_JOURNEY physical-Erase/noop-Clear/Undo/later-Fill dense=$dense '
          'occupancy=none selected=$laterFrame displayed=$laterFrame result=applied '
          'noOp-revision=${erased['live_revision']} Undo-revision=${undone['live_revision']} '
          'frame0-RGB=${laterFrame == 0 ? '224466' : '123456'} effect0=${removeEffect ? 'Off' : 'breathe'} duration0=${removeEffect ? 0 : 1600} remaining-styled=breathe1600 '
          'frame1-RGB=${laterFrame == 1 ? '224466' : '0'} '
          'all512-RGB/effect/duration=exact wire=Canvas0,Undo,Canvas$laterFrame explicit-Off=$removeEffect '
          'undo=${controller.canUndo} redo=${controller.canRedo} next=Undo-later-Fill',
        );
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final tailTiming in <String>['pending Canvas', 'confirmed Canvas']) {
    testWidgets(
      'Litchcraft: prior Undo overflow tail is exact during $tailTiming',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        addTearDown(() async {
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.enter());
        final draft = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..addFrame();
        await journey.finish(controller.replaceDocument(draft.document));
        await journey.finish(controller.selectAnimationFrame(1));
        await journey.finish(controller.setBrush(color: 0x224466));
        await journey.finish(controller.fillAll());
        await _expectCleanStoreParity(journey, controller);
        ble.holdPhysicalStoreObjects = true;
        await journey.finish(controller.undo());
        final predecessor = controller.documentSession.document;
        final inverse = ble.held.where((packet) {
          final decoded = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return decoded?.kind == ConnectionSessionKind.object &&
              decoded?.code == LitchcraftV2Protocol.physicalStoreBatch;
        }).toList();
        expect(
          inverse,
          isNotEmpty,
          reason: 'Real dense Undo produced its bounded reverse tail',
        );
        await journey.finish(controller.setBrush(color: 0x778899));
        final start = ble.writes.length;
        if (tailTiming == 'pending Canvas') {
          ble.heldChannels.add('connection_session');
        }
        final fill = controller.fillAll();
        final intended = controller.documentSession.document;
        expect(sameLitchcraftDocument(intended, predecessor), isFalse);
        if (tailTiming == 'pending Canvas') {
          await journey.until(
            () => ble.writes.skip(start).any((write) {
              if (write['channel'] != 'connection_session') return false;
              final decoded = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return decoded?.kind == ConnectionSessionKind.ordered &&
                  decoded?.code == 9;
            }),
            budgetMs: 1000,
          );
          await journey.tick();
          expect(controller.pending, isTrue);
        } else {
          await journey.finish(fill);
        }
        for (final packet in inverse) {
          ble.held.remove(packet);
          ble.deliver(packet);
        }
        await tester.pump();
        expect(
          sameLitchcraftDocument(controller.documentSession.document, intended),
          isTrue,
          reason:
              'An exact predecessor cannot rebase away a newer Canvas preview',
        );
        ble.heldChannels.remove('connection_session');
        ble.holdPhysicalStoreObjects = false;
        ble.release('connection_session');
        await journey.finish(fill);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, intended),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
        expect(
          _litchcraftRequests(ble, start).where(
            (packet) =>
                packet[0] == LitchcraftV2Protocol.magic &&
                (_isArtworkBodyRead(packet) ||
                    packet[2] == LitchcraftV2Protocol.frameStoreBegin),
          ),
          isEmpty,
          reason:
              'Exact prior history overflow does not request a repair or transfer',
        );
        await journey.finish(controller.undo());
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            predecessor,
          ),
          isTrue,
        );
        await journey.finish(controller.redo());
        expect(
          sameLitchcraftDocument(controller.documentSession.document, intended),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final startsWithStoreChoice in <bool>[false, true]) {
    testWidgets(
      startsWithStoreChoice
          ? 'Litchcraft: real About Open settles an already-active Store artwork choice'
          : 'Litchcraft: real About Open overlaps admission before canonical 256-frame commit',
      (tester) async {
        PackageInfo.setMockInitialValues(
          appName: 'Litch',
          packageName: 'test.litch',
          version: '1.0.0',
          buildNumber: '1',
          buildSignature: '',
        );
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        var ble = VirtualLitchBle(native);
        var service = _BridgeBoard(ble);
        var journey = _Journey(tester, ble, service);
        var controller = LitchcraftController(service);
        var library = LitchcraftLibrary(controller);
        var phoneDisposed = false;
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          if (!phoneDisposed) {
            library.dispose();
            controller.dispose();
          }
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        var resolved = false;
        final selectedPages = <int>[];
        Future<void> mountApp() => tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: service,
              boardId: _boardId,
              boards: const [],
              onDisconnect: service.disconnect,
              craftController: controller,
              craftLibrary: library,
              initialPageIndex: startsWithStoreChoice ? 0 : 1,
              onModeResolved: () => resolved = true,
              onPageSelected: selectedPages.add,
              loadAppConsolePreference: () async => false,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );

        if (startsWithStoreChoice) {
          await journey.finish(controller.enter());
          final seed = LitchcraftDocumentSession(
            staticLitchcraftDocument(LitchcraftCanvas.blank()),
          )..addFrame();
          await journey.finish(controller.replaceDocument(seed.document));
          await journey.finish(controller.setBrush(color: 0x4488cc));
          await ble.flush(tester);
          await ble.advance(tester, 100);
          final predecessor =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          await journey.finish(service.disconnect());
          final offlineDraft = staticLitchcraftDocument(
            LitchcraftCanvas.blank(),
          );
          controller.restoreDocument(offlineDraft);
          // A changed phone against an unchanged common board must auto-send.
          // Make genuine board divergence while disconnected to require choice.
          await journey.hall(BigInt.one);
          await ble.advance(tester, 120);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 6000);
          final changedBoard =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(changedBoard['dirty'], isFalse);
          expect(changedBoard['frame_count'], 2);
          expect(
            changedBoard['root_generation'],
            greaterThan(predecessor['root_generation'] as int),
          );
          expect((changedBoard['emitters'] as List).first[0], 0x4488cc);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              offlineDraft,
            ),
            isTrue,
            reason: 'Disconnected physical work cannot edit the phone source',
          );
          await journey.finish(service.connect(_boardId));
          await journey.until(
            () => controller.status == LitchcraftStatus.conflict,
            budgetMs: 6000,
            reason: 'Begin with a genuine Store source choice',
          );
        }
        await mountApp();
        await journey.until(() => resolved, budgetMs: 6000);
        expect(controller.boardActive, startsWithStoreChoice);
        expect(
          tester
              .widget<LitchAboutBrandButton>(find.byType(LitchAboutBrandButton))
              .litchcraft,
          startsWithStoreChoice,
          reason: 'The real first destination follows acknowledged board mode',
        );
        await tester.tap(find.byKey(const ValueKey('open-about')));
        await journey.until(
          () =>
              find.byKey(const ValueKey('about-version')).evaluate().isNotEmpty,
          budgetMs: 1000,
          reason: 'The real About page must finish loading the version control',
        );
        await tester.pump(const Duration(milliseconds: 400));
        for (var tap = 0; tap < 3; tap++) {
          await tester.tap(find.byKey(const ValueKey('about-version')));
          await tester.pump();
        }
        expect(
          tester
              .widget<Text>(
                find.byKey(const ValueKey('about-version-tap-feedback')),
              )
              .data,
          '• • •',
        );
        await tester.pump(const Duration(seconds: 2));
        await journey.until(
          () =>
              find.byType(AboutScreen).evaluate().isEmpty &&
              find.byType(AlertDialog).evaluate().isNotEmpty,
          budgetMs: 1000,
          reason:
              'Three real About taps must settle into the Open confirmation',
        );
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.byType(AboutScreen), findsNothing);
        expect(find.byType(AlertDialog), findsOneWidget);
        expect(library.animationDocument.frames, hasLength(1));
        final uploadStart = ble.writes.length;
        var admissionOverlappedReplacement = false;
        void observeOverlap() {
          if (controller.authorityResolving &&
              controller.documentSession.document.frames.length == 256) {
            admissionOverlappedReplacement = true;
          }
        }

        controller.addListener(observeOverlap);
        await tester.tap(find.widgetWithText(FilledButton, 'Open'));
        await tester.pump();
        expect(library.animationDocument.frames, hasLength(256));
        // Observe the overlap at the production notification seam, since normal
        // microtasks may finish or cancel admission before the next test frame.
        await tester.pump();
        controller.removeListener(observeOverlap);
        expect(
          admissionOverlappedReplacement,
          !startsWithStoreChoice,
          reason: 'The About replacement must overlap real board admission',
        );
        await journey.until(
          () =>
              controller.boardActive &&
              controller.status == LitchcraftStatus.live &&
              !controller.pending &&
              !controller.storeReplacementUnresolved,
          budgetMs: 20000,
          reason:
              'About upload failed: status=${controller.status} '
              'error=${controller.errorMessage}',
        );
        final uploaded = _litchcraftRequests(ble, uploadStart);
        if (startsWithStoreChoice) {
          expect(
            uploaded.where(
              (packet) =>
                  packet[0] == LitchcraftProtocol.magic &&
                  packet[1] == LitchcraftProtocol.enter,
            ),
            isEmpty,
            reason: 'Opening on an active Store must not reacquire board mode',
          );
        } else {
          expect(uploaded.first[0], LitchcraftProtocol.magic);
          expect(uploaded.first[1], LitchcraftProtocol.enter);
        }
        // Present the completed Commit before inspecting the old chooser label.
        await tester.pump();
        expect(find.text('Choose artwork'), findsNothing);
        expect(
          uploaded
              .where((packet) => packet[0] == LitchcraftV2Protocol.magic)
              .map((packet) => packet[2]),
          containsAllInOrder([
            LitchcraftV2Protocol.frameStoreBegin,
            LitchcraftV2Protocol.frameStoreChunk,
            LitchcraftV2Protocol.frameStoreCommit,
          ]),
          reason: 'Real Enter must precede the single canonical Store transfer',
        );
        expect(
          uploaded.where(
            (packet) =>
                packet[0] == LitchcraftV2Protocol.magic &&
                packet[2] == LitchcraftV2Protocol.frameStoreCommit,
          ),
          hasLength(1),
        );
        expect(
          controller.animationPlaying,
          isFalse,
          reason:
              'About artwork opens paused until the user explicitly plays it',
        );
        await journey.finish(controller.toggleAnimationPlayback());
        await _expectAnimatedBoardOutput(journey);
        // Freeze the live playhead for exact comparison of all 65,536 emitters.
        await journey.finish(controller.toggleAnimationPlayback());
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(controller.toggleAnimationPlayback());
        await journey.finish(library.flush());
        final beforeRestart = await journey.frame();
        journey.checkpoint(
          'about-three-taps-open',
          beforeRestart,
          'frames=256',
        );
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        phoneDisposed = true;
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final scenario in <String>[
    'inactive Inspect selects Play without Enter',
    'Inspect selects Craft before State37',
    'retained inactive controller follows physical entry on a new link',
    'physical Exit supersedes early Craft routing while State37 is held',
  ]) {
    testWidgets(
      'Litchcraft cold attach: $scenario',
      (tester) async {
        final retainedController = scenario.startsWith('retained');
        final physicalExit = scenario.startsWith('physical Exit');
        final activeBoard = !scenario.startsWith('inactive');
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        var ble = _ColdAttachGattBle(native, operationIntervalMs: 20);
        LitchBleService makeService() => LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        var service = makeService();
        var journey = _Journey(tester, ble, service);
        var controller = LitchcraftController(service);
        var library = LitchcraftLibrary(controller);
        addTearDown(() async {
          ble.releaseStoreAuthority();
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          try {
            // Failure may retire initial session activation before it finishes.
            await journey.finish(service.dispose());
            await ble.flush(tester);
            await journey.finish(ble.close());
          } finally {
            await tester.runAsync(native.close);
          }
        });
        Future<void> physicalModeChord() async {
          // Raw production button sampling, chord qualification and release.
          for (final button in <int>[5, 6]) {
            await ble.command(tester, <String, Object>{
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': true,
            }, flushOperations: false);
            await ble.advance(tester, 30);
          }
          await ble.advance(tester, 1500);
          for (final button in <int>[5, 6]) {
            await ble.command(tester, <String, Object>{
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': false,
            }, flushOperations: false);
            await ble.advance(tester, 30);
          }
        }

        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(library.ready);
        await journey.finish(service.connect(_boardId));
        if (retainedController) {
          // Establish the durable artwork retained by a normal physical entry,
          // then keep this same controller's newer inactive mode observation.
          await journey.finish(controller.enter());
          final seed = LitchcraftDocumentSession(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint(<int>[0], 1, 0x123456),
            ),
          )..addFrame();
          await journey.finish(controller.replaceDocument(seed.document));
          seed.dispose();
          await _expectCleanStoreParity(journey, controller);
          await journey.finish(controller.leave());
          await journey.finish(controller.adoptActiveBoard());
          expect(controller.inspectedBoardActive, isFalse);
          await journey.finish(service.disconnect());
          await physicalModeChord();
          expect((await journey.frame())['litchcraft_active'], isTrue);
          ble.holdStoreAuthority = true;
          await journey.finish(service.connect(_boardId));
          expect(
            controller.inspectedBoardActive,
            isNull,
            reason: 'The prior inactive fact is bound to its original link',
          );
        } else if (activeBoard) {
          await journey.finish(controller.enter());
          final seed = LitchcraftDocumentSession(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint(<int>[0], 1, 0x123456),
            ),
          )..addFrame();
          await journey.finish(controller.replaceDocument(seed.document));
          seed.dispose();
          await _expectCleanStoreParity(journey, controller);
          await journey.finish(library.flush());
          // Retire every phone owner without Exit, keeping the native board.
          library.dispose();
          controller.dispose();
          await journey.finish(service.dispose());
          await ble.flush(tester);
          await journey.finish(ble.close());
          expect((await journey.frame())['litchcraft_active'], isTrue);
          ble = _ColdAttachGattBle(native, operationIntervalMs: 20);
          service = makeService();
          journey = _Journey(tester, ble, service);
          controller = LitchcraftController(service);
          library = LitchcraftLibrary(controller);
          await journey.finish(library.ready);
          await journey.finish(service.connect(_boardId));
        }
        if (physicalExit) {
          // A cold phone may have only a blank draft while the board retains
          // animated artwork. Mode authority must not depend on that draft.
          controller.restoreDocument(
            staticLitchcraftDocument(LitchcraftCanvas.blank()),
          );
        }
        final before = await journey.frame();
        final beforeSession = before['litchcraft_session'];
        final writesFrom = ble.writes.length;
        ble.observedModeStates.clear();
        ble.holdStoreAuthority = activeBoard;
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: controller,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        void expectNoHome() {
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(find.text('Position not available'), findsNothing);
          expect(find.text('Reading position'), findsNothing);
          expect(_coldPositionQueries(ble, writesFrom), 0);
        }

        bool? observedMode() {
          final requests = _litchcraftRequests(ble, writesFrom)
              .where(
                (packet) =>
                    packet[0] == LitchcraftV2Protocol.magic &&
                    packet[2] == LitchcraftV2Protocol.inspect,
              )
              .map(
                (packet) =>
                    ByteData.sublistView(packet).getUint16(7, Endian.little),
              )
              .toSet();
          for (final packet in ble.observedModeStates) {
            if (requests.contains(packet.requestId) && packet.payload[0] == 0) {
              return LitchcraftV2State.fromPacket(packet).active;
            }
          }
          return null;
        }

        int modeInspections() => _litchcraftRequests(
          ble,
          writesFrom,
        ).where(isLitchcraftModeDiscovery).length;
        bool craftVisible() {
          final brand = find.byType(LitchAboutBrandButton).hitTestable();
          return brand.evaluate().length == 1 &&
              tester.widget<LitchAboutBrandButton>(brand).litchcraft;
        }

        // Check every intermediate frame, including before the typed Inspect
        // Future accepts the SDK callback's complete shared Object receipt.
        expectNoHome();
        final started = journey.now;
        while ((observedMode() == null ||
                controller.inspectedBoardActive == null) &&
            journey.now - started < 10000) {
          await journey.tick();
          if (controller.inspectedBoardActive == null || activeBoard) {
            expectNoHome();
          }
        }
        expect(observedMode(), activeBoard);
        expect(controller.inspectedBoardActive, activeBoard);
        final inspectedRequests = modeInspections();
        expect(inspectedRequests, greaterThan(0));
        await tester.pump();
        expect(craftVisible(), activeBoard);
        if (activeBoard) {
          expectNoHome();
          await journey.until(
            () => ble.heldStoreAuthority.isNotEmpty,
            budgetMs: 10000,
          );
          expect(controller.boardArtworkUnresolved, isTrue);
          expect(
            modeInspections(),
            inspectedRequests,
            reason: 'Mode routing reuses Inspect while Store authority is held',
          );
          journey.checkpoint(
            retainedController
                ? 'retained-inactive-new-link-inspect-active'
                : 'cold-inspect-active-store-held',
            await journey.frame(),
            'Craft visible; Home unmounted; genuine State37 held; edit authority pending; '
            'draftFrames=${controller.documentSession.document.frames.length} '
            'draftAdvanced=${controller.documentSession.isAdvanced} '
            'store=${controller.usesFrameStoreDocument} '
            'mode=${controller.boardActive} session=${controller.sessionId}',
          );
          if (physicalExit) {
            final draft = controller.documentSession.document;
            await physicalModeChord();
            expect((await journey.frame())['litchcraft_active'], isFalse);
            await journey.until(
              () =>
                  !craftVisible() &&
                  find.byType(HomeScreen).evaluate().isNotEmpty,
              budgetMs: 10000,
            );
            expect(controller.inspectedBoardActive, isFalse);
            expect(controller.boardActive, isFalse);
            expect(controller.selected, isFalse);
            expect(controller.boardArtworkUnresolved, isFalse);
            final exitedWrites = ble.writes.length;
            ble.releaseStoreAuthority();
            await ble.advance(tester, 200);
            expect(craftVisible(), isFalse);
            expect(controller.inspectedBoardActive, isFalse);
            expect(controller.boardActive, isFalse);
            expect(controller.selected, isFalse);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                draft,
              ),
              isTrue,
              reason: 'Late Store authority must preserve the detached draft',
            );
            expect(
              _litchcraftRequests(ble, exitedWrites).where(
                (packet) => packet[0] == LitchcraftProtocol.magic
                    ? packet[1] == LitchcraftProtocol.enter
                    : const <int>{
                        LitchcraftV2Protocol.frameStoreSnapshot,
                        LitchcraftV2Protocol.frameStoreBegin,
                        LitchcraftV2Protocol.frameStoreCommit,
                      }.contains(packet[2]),
              ),
              isEmpty,
              reason: 'Late authority cannot restart entry or deliver artwork',
            );
            journey.checkpoint(
              'physical-exit-before-store-authority',
              await journey.frame(),
              'raw X+O hold/release; Play visible; delayed State37 cannot revive Craft',
            );
          } else {
            ble.releaseStoreAuthority();
            await journey.until(
              () =>
                  controller.status == LitchcraftStatus.live &&
                  !controller.boardArtworkUnresolved &&
                  !controller.pending,
              budgetMs: 10000,
            );
            // The parity helper advances six seconds for persistence; normal
            // Store clock sampling remains owned by the existing controller.
            await _expectCleanStoreParity(journey, controller);
            expectNoHome();
          }
        } else {
          await journey.until(
            () => find.byType(HomeScreen).evaluate().isNotEmpty,
            budgetMs: 10000,
          );
          expect(controller.boardActive, isFalse);
        }
        final after = await journey.frame();
        expect(after['litchcraft_active'], activeBoard && !physicalExit);
        if (activeBoard) expect(after['litchcraft_session'], beforeSession);
        expect(
          _litchcraftRequests(ble, writesFrom).where(
            (packet) =>
                packet[0] == LitchcraftProtocol.magic &&
                (packet[1] == LitchcraftProtocol.enter ||
                    packet[1] == LitchcraftProtocol.exit),
          ),
          isEmpty,
          reason: 'Passive mode following never sends app Enter or Exit',
        );
        journey.checkpoint(
          activeBoard
              ? 'cold-store-authority-terminal'
              : 'cold-inspect-inactive-play',
          after,
          physicalExit
              ? 'physical Exit retained; no app Enter/Exit'
              : activeBoard
              ? 'original Store coherent; no Enter/Exit'
              : 'Play first-mounted after inactive Inspect; no Enter',
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(library.flush());
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: cold relaunch follows a healthy 256-frame active board without Enter or Exit',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      var ble = VirtualLitchBle(native);
      var service = _BridgeBoard(ble);
      var journey = _Journey(tester, ble, service);
      var controller = LitchcraftController(service);
      var library = LitchcraftLibrary(controller);
      var phoneDisposed = false;
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        if (!phoneDisposed) {
          library.dispose();
          controller.dispose();
        }
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      var resolved = false;
      final selectedPages = <int>[];
      Future<void> mountApp({int initialPage = 1}) => tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: service,
            boardId: _boardId,
            boards: const [],
            onDisconnect: service.disconnect,
            craftController: controller,
            craftLibrary: library,
            initialPageIndex: initialPage,
            onModeResolved: () => resolved = true,
            onPageSelected: selectedPages.add,
            loadAppConsolePreference: () async => false,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
          ),
        ),
      );
      // Seed healthy board authority through the real selected scaffold. This
      // test isolates cold adoption from the separate About-entry regression.
      await mountApp(initialPage: 0);
      await journey.until(
        () => resolved && controller.boardActive && !controller.pending,
        budgetMs: 6000,
      );
      await journey.finish(library.loadBouncingBallsBenchmark());
      if (controller.animationPlaying) {
        await journey.finish(controller.toggleAnimationPlayback());
      }
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(controller.toggleAnimationPlayback());
      await _expectAnimatedBoardOutput(journey);
      await journey.finish(library.flush());
      final beforeRestart = await journey.frame();

      // A cold app restart retires every phone-side owner, retaining the same
      // native board process and saved phone draft. It sends no mode departure.
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      phoneDisposed = true;
      await journey.finish(service.dispose());
      await ble.flush(tester);
      await journey.finish(ble.close());
      expect(
        _litchcraftRequests(ble).where(
          (packet) =>
              packet[0] == LitchcraftProtocol.magic &&
              packet[1] == LitchcraftProtocol.exit,
        ),
        isEmpty,
        reason: 'Destroying the app must leave physical Litchcraft running',
      );
      expect((await journey.frame())['litchcraft_active'], isTrue);
      ble = VirtualLitchBle(native);
      service = _BridgeBoard(ble);
      journey = _Journey(tester, ble, service);
      controller = LitchcraftController(service);
      library = LitchcraftLibrary(controller);
      phoneDisposed = false;
      resolved = false;
      selectedPages.clear();
      await journey.finish(library.ready);
      expect(library.animationDocument.frames, hasLength(256));
      await journey.finish(service.connect(_boardId));
      await mountApp();
      await journey.until(
        () => resolved && selectedPages.contains(0),
        budgetMs: 10000,
        reason:
            'Cold relaunch must follow active board into Litchcraft '
            'without explicit Enter; pages=$selectedPages '
            'status=${controller.status} active=${controller.boardActive} '
            'error=${controller.errorMessage}',
      );
      expect(controller.selected, isTrue);
      expect(controller.inspectedBoardActive, isTrue);
      // Observe the frame scheduled by the mode-resolution route callback.
      await tester.pump();
      expect(
        tester
            .widget<LitchAboutBrandButton>(find.byType(LitchAboutBrandButton))
            .litchcraft,
        isTrue,
      );
      await journey.until(
        () =>
            controller.status == LitchcraftStatus.live &&
            !controller.boardArtworkUnresolved &&
            controller.animationPlaying,
        budgetMs: 10000,
      );
      expect(controller.boardActive, isTrue);
      expect(
        _litchcraftRequests(ble).where(
          (packet) =>
              packet[0] == LitchcraftProtocol.magic &&
              (packet[1] == LitchcraftProtocol.enter ||
                  packet[1] == LitchcraftProtocol.exit),
        ),
        isEmpty,
        reason: 'Cold adoption inspects authority without Enter or Exit',
      );
      final relaunched = await journey.frame();
      expect(relaunched['now_ms'], greaterThan(beforeRestart['now_ms'] as int));
      await _expectAnimatedBoardOutput(journey);
      journey.checkpoint(
        'cold-relaunch-follows-board',
        relaunched,
        'frames=256',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(library.flush());
      library.dispose();
      controller.dispose();
      phoneDisposed = true;
      await journey.finish(service.disconnect());
      expect((await journey.frame())['litchcraft_active'], isTrue);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: five New Artwork and Bouncing Balls reloads remain in sync across Play',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final seed = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      )..addFrame();
      final first = LitchcraftController(service, documentSession: seed);
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(first.enter());
      await journey.finish(
        first.transferFrameStoreForDiagnostics(
          frames: seed.document.frames,
          playbackOnEntry: false,
        ),
      );
      first.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final controller = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          seed.document,
          localIntent: false,
        ),
      );
      final library = LitchcraftLibrary(controller);
      var initialControllerDisposed = false;
      addTearDown(() {
        if (!initialControllerDisposed) {
          library.dispose();
          controller.dispose();
        }
      });
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      expect(controller.boardActive, isTrue, reason: controller.errorMessage);
      var previousRoot = await _expectCleanStoreParity(journey, controller);
      await ble.advance(tester, 10);
      final blankBoardOutput = jsonEncode((await journey.frame())['board']);

      for (var cycle = 1; cycle <= 5; cycle++) {
        final clearStart = ble.writes.length;
        await journey.finish(library.newArtwork());
        expect(
          _litchcraftRequests(ble, clearStart).where(
            (packet) =>
                packet[0] == LitchcraftProtocol.magic &&
                packet[1] == LitchcraftProtocol.paletteChunk,
          ),
          isEmpty,
          reason:
              'A one-frame Store replacement keeps its Store owner '
              'through notifications; no compact palette is queued',
        );
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: 'New Artwork cycle $cycle: ${controller.errorMessage}',
        );
        expect(controller.documentSession.document.frames, hasLength(1));
        expect(
          controller.documentSession.document.frames.single.emitters.map(
            (emitter) => emitter.color,
          ),
          everyElement(0),
          reason: 'New Artwork cycle $cycle must actually clear Bouncing Balls',
        );
        await ble.advance(tester, 10);
        final clearedBoard = await journey.frame();
        expect(clearedBoard['litchcraft_visible'], isTrue);
        expect(
          jsonEncode(clearedBoard['board']),
          blankBoardOutput,
          reason:
              'New Artwork cycle $cycle must clear the displayed board '
              'on its next render turn, not only after persistence settles',
        );
        final blankRoot = await _expectCleanStoreParity(journey, controller);
        expect(blankRoot, isNot(previousRoot));
        await journey.finish(library.loadBouncingBallsBenchmark());
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: 'Bouncing Balls cycle $cycle: ${controller.errorMessage}',
        );
        expect(controller.documentSession.document.frames.length, 256);
        expect(
          controller.documentSession.document.frames.first.emitters.map(
            (emitter) => emitter.color,
          ),
          contains(isNot(0)),
          reason: 'Bouncing Balls cycle $cycle must load actual artwork',
        );
        final ballsRoot = await _expectCleanStoreParity(journey, controller);
        expect(ballsRoot, isNot(blankRoot));
        previousRoot = ballsRoot;
        expect(controller.animationPlaying, isFalse);
        await journey.finish(controller.toggleAnimationPlayback());
        expect(controller.animationPlaying, isTrue);
        await _expectAnimatedBoardOutput(journey);
        debugPrint('LITCH_CYCLE $cycle new_artwork_and_balls_clean');

        if (cycle == 3) {
          await journey.finish(controller.leave());
          expect(
            controller.boardActive,
            isFalse,
            reason: 'Play mode must own the board after Exit',
          );
          expect((await journey.frame())['litchcraft_active'], isFalse);
          await journey.finish(controller.enter());
          expect(
            controller.boardActive,
            isTrue,
            reason:
                'Litchcraft re-entry after Play: ${controller.errorMessage}',
          );
          await ble.advance(tester, 300);
          final visible = await journey.frame();
          expect(visible['litchcraft_active'], isTrue);
          expect(
            visible['litchcraft_visible'],
            isTrue,
            reason: 'The re-entered board must show its own animation canvas',
          );
          await _expectCleanStoreParity(journey, controller);
          expect(controller.animationPlaying, isFalse);
          await journey.finish(controller.toggleAnimationPlayback());
          await _expectAnimatedBoardOutput(journey);
        }
      }
      await journey.finish(library.flush());
      library.dispose();
      controller.dispose();
      initialControllerDisposed = true;
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final fresh = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
          localIntent: false,
        ),
      );
      addTearDown(fresh.dispose);
      await journey.finish(fresh.enter());
      expect(fresh.status, LitchcraftStatus.live, reason: fresh.errorMessage);
      await journey.until(
        () => fresh.documentSession.document.frames.length == 256,
        budgetMs: 6000,
        reason:
            'A fresh phone must finish reading the resident 256-frame '
            'artwork, not just enter the live mode',
      );
      expect(fresh.documentSession.document.frames, hasLength(256));
      expect(
        fresh.animationPlaying,
        isTrue,
        reason: 'A fresh phone must recover the running board playback state',
      );
      await journey.finish(fresh.toggleAnimationPlayback());
      expect(fresh.animationPlaying, isFalse);
      await _expectCleanStoreParity(journey, fresh);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final scenario in [
    'About while playing',
    'About metadata stall',
    'About delayed clean save',
    'About delayed retry write failure',
    'About delayed failed save',
    'Library metadata stall',
    'About phone flush failure',
    'About terminal failure',
    'metadata exhausted',
    'live metadata stall',
    'settled source change',
    'settled departure',
    'settled board change',
    'settled root change',
    'settled physical edit',
    'metadata physical edit',
    'metadata newer Open',
    'metadata departure',
  ]) {
    final about = scenario.startsWith('About ');
    final libraryRow = scenario == 'Library metadata stall';
    final phoneFailure = scenario == 'About phone flush failure';
    final terminalFailure = scenario == 'About terminal failure';
    final settledFence = scenario.startsWith('settled ');
    final delayedSave = scenario.startsWith('About delayed ');
    final delayedFailure = scenario == 'About delayed failed save';
    final retryFailure = scenario == 'About delayed retry write failure';
    final metadataStall =
        scenario == 'live metadata stall' ||
        scenario == 'About metadata stall' ||
        libraryRow ||
        phoneFailure ||
        delayedSave ||
        settledFence ||
        scenario.startsWith('metadata ');
    testWidgets(
      'Litchcraft: Balls256 to RR32 explicit Open $scenario',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedOpenMetadataBle(native, operationIntervalMs: 8);
        late final _Journey journey;
        final trace = _SyncRecoveryTrace(() => journey.now);
        final service = _BridgeBoard(ble, appTrace: trace);
        journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service, appTrace: trace);
        final draftStore = _OpenFeedbackDraftStore(
          await SharedPreferences.getInstance(),
        );
        final library = LitchcraftLibrary(controller, store: draftStore);
        var phoneCleaned = false;
        Future<void> cleanupJourney() async {
          if (phoneCleaned) return;
          phoneCleaned = true;
          try {
            await tester.pumpWidget(const SizedBox.shrink());
            draftStore.failSave = false;
            final heldSave = draftStore.holdNewDocumentSave;
            if (heldSave != null && !heldSave.isCompleted) heldSave.complete();
            await journey.finish(library.flush());
          } finally {
            library.dispose();
            controller.dispose();
            await ble.disposeJourney(tester, service);
          }
        }

        addTearDown(cleanupJourney);
        try {
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 1000);
          await journey.finish(service.connect(_boardId));
          await journey.finish(library.ready);
          await journey.finish(controller.enter());
          if (about) {
            PackageInfo.setMockInitialValues(
              appName: 'Litch',
              packageName: 'test.litch',
              version: '1.0.0',
              buildNumber: '1',
              buildSignature: '',
            );
            await tester.pumpWidget(
              MaterialApp(
                home: LitchMainScaffold(
                  bleService: service,
                  boardId: _boardId,
                  boards: const [],
                  onDisconnect: service.disconnect,
                  craftController: controller,
                  craftLibrary: library,
                  initialPageIndex: 0,
                  loadAppConsolePreference: () async => false,
                  firmwarePageBuilder: (_, _, _, _, _) =>
                      const SizedBox.shrink(),
                ),
              ),
            );
          } else {
            await tester.pumpWidget(
              MaterialApp(
                home: Scaffold(
                  body: LitchcraftScreen(
                    controller: controller,
                    library: library,
                  ),
                ),
              ),
            );
          }
          await journey.finish(
            library.loadBouncingBallsBenchmark(),
            budgetMs: 15000,
          );
          expect(library.animationDocument.frames, hasLength(256));
          await _expectCleanStoreParity(journey, controller);
          await journey.finish(controller.toggleAnimationPlayback());
          await ble.advance(tester, 2500);
          final shared = await service.openConnectionSession(_boardId);
          ble.holdStoreMetadata = metadataStall;
          draftStore.failSave = phoneFailure;
          if (delayedSave) draftStore.holdNewDocumentSave = Completer<void>();
          if (libraryRow) {
            library.artworks = [
              LitchcraftArtwork.animated(
                id: 'rr-feedback',
                name: 'Retry RR',
                document: LitchcraftAnimationDocument(
                  frames: LitchcraftRickrollPreset.decode().frames,
                  topology: controller.canvas.topology,
                  favoriteColors: library.favoriteColors,
                  playbackOnEntry: true,
                ),
                createdAt: DateTime.utc(2026),
                modifiedAt: DateTime.utc(2026),
              ),
            ];
            await journey.finish(library.flush());
          }
          var settled = false;
          Object? loadError;
          if (about || libraryRow) {
            if (about) {
              await tester.tap(find.byKey(const ValueKey('open-about')));
              await journey.until(
                () => find
                    .byKey(const ValueKey('about-version'))
                    .evaluate()
                    .isNotEmpty,
                budgetMs: 1000,
              );
              await tester.pump(const Duration(milliseconds: 400));
              for (var tap = 0; tap < 8; ++tap) {
                await tester.tap(find.byKey(const ValueKey('about-version')));
                await tester.pump();
              }
              await tester.pump(const Duration(seconds: 2));
            } else {
              await tester.tap(
                find.byKey(const ValueKey('craft-artwork-menu')),
              );
              await tester.pump(const Duration(milliseconds: 400));
              await tester.tap(find.byKey(const ValueKey('craft-library')));
              await journey.until(
                () => find
                    .byKey(const ValueKey('craft-artwork-rr-feedback'))
                    .evaluate()
                    .isNotEmpty,
                budgetMs: 1000,
              );
              await tester.pump(const Duration(milliseconds: 400));
              await tester.tap(
                find.byKey(const ValueKey('craft-artwork-rr-feedback')),
              );
            }
            await journey.until(
              () => find.byType(AlertDialog).evaluate().isNotEmpty,
              budgetMs: 1000,
            );
            await tester.pump(const Duration(milliseconds: 400));
            if (terminalFailure) ble.failNextConnectionSessionWrite = true;
            await tester.tap(find.widgetWithText(FilledButton, 'Open'));
            await journey.until(
              () =>
                  (controller.status == LitchcraftStatus.live &&
                      library.animationDocument.frames.length == 32 &&
                      !controller.storeReplacementUnresolved) ||
                  find.byType(SnackBar).evaluate().isNotEmpty ||
                  ((metadataStall || terminalFailure) &&
                      controller.status == LitchcraftStatus.error),
              budgetMs: 10000,
            );
            await tester.pump();
            if (phoneFailure || terminalFailure) {
              expect(
                find.byType(SnackBar),
                findsOneWidget,
                reason:
                    'Phone persistence and terminal delivery failures remain visible',
              );
              expect(
                find.text(
                  'That action could not finish. Your canvas is still here. Try again.',
                ),
                findsOneWidget,
              );
              if (phoneFailure) {
                expect(library.saveFailed, isTrue);
                expect(library.lastSaveError, same(draftStore.failure));
                expect(controller.storeReplacementRecoveryPending, isTrue);
              } else {
                expect(controller.storeReplacementRecoveryPending, isFalse);
              }
              draftStore.failSave = false;
              await journey.finish(library.flush());
              await journey.finish(service.disconnect());
              return;
            }
            if (metadataStall) {
              ble.holdModeMetadata = true;
              await ble.advance(tester, 650);
              expect(controller.storeReplacementRecoveryPending, isTrue);
              expect(find.text('Sending artwork…'), findsOneWidget);
              expect(
                find.byKey(const ValueKey('craft-sync-activity')),
                findsOneWidget,
              );
            }
            expect(
              find.text(
                'That action could not finish. Your canvas is still here. Try again.',
              ),
              findsNothing,
            );
            expect(
              find.byType(SnackBar),
              findsNothing,
              reason:
                  'Actual About-to-RR callbacks must not report a failed action during eligible recovery',
            );
            settled = true;
          } else {
            unawaited(
              library.loadRickroll().then(
                (_) => settled = true,
                onError: (Object error, StackTrace _) {
                  loadError = error;
                  settled = true;
                },
              ),
            );
          }
          await journey.until(() => settled, budgetMs: 10000);
          if (loadError != null) {
            expect(loadError, isA<TimeoutException>());
            expect(library.isOpenFailureHandled(loadError!), isTrue);
            expect(
              library.isOpenFailureHandled(TimeoutException('unrelated')),
              isFalse,
            );
            expect(
              library.isOpenFailureHandled(StateError('terminal')),
              isFalse,
            );
          }
          if (scenario == 'metadata exhausted') {
            final recoveryStarted = journey.now;
            while (journey.now - recoveryStarted < 50000 &&
                !controller.connectionRecoveryRequired) {
              if (ble.hasPendingOperations) {
                await journey.tick();
              } else {
                // Keep production deadlines; batch only idle clock advancement.
                await ble.advance(tester, 250);
              }
            }
            expect(
              controller.connectionRecoveryRequired,
              isTrue,
              reason:
                  'The existing bounded repair must expose exhausted recovery',
            );
            await tester.pump();
            expect(controller.storeReplacementRecoveryPending, isFalse);
            expect(library.isOpenFailureHandled(loadError!), isFalse);
            expect(controller.errorMessage, isNotNull);
            await ble.advance(tester, 200);
            expect(
              find.byKey(const ValueKey('craft-reconnect')),
              findsOneWidget,
            );
            expect(find.text('Sending artwork…'), findsNothing);
            expect(library.animationDocument.frames, hasLength(32));
            debugPrint(
              'OPEN_FEEDBACK exhausted status=${controller.status.name} error=${controller.errorMessage}',
            );
            await journey.finish(service.disconnect());
            return;
          }
          Future<void>? departure;
          if (metadataStall) {
            debugPrint(
              'BALLS_RR_METADATA held=${ble.held.length} session_active=${shared?.isActivated} '
              'connected=${service.isConnected} error=$loadError status=${controller.status.name}',
            );
            expect(
              ble.held,
              isNotEmpty,
              reason:
                  'The controlled fault must affect a real native metadata reply',
            );
            expect(
              shared?.isActivated,
              isTrue,
              reason: 'This probe must preserve the live shared session',
            );
            if (scenario == 'metadata physical edit') {
              await journey.finish(controller.setBrush(color: 0x12abcd));
              await journey.hall(BigInt.one);
              await ble.advance(tester, 120);
              await journey.hall(BigInt.zero);
              await ble.advance(tester, 120);
              final changed =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': 0,
                      }))['store_frame']
                      as Map<String, dynamic>;
              expect(
                changed['dirty'],
                isTrue,
                reason: 'Real Hall input must change the predecessor',
              );
            } else if (scenario == 'metadata newer Open') {
              await journey.finish(library.newArtwork());
            } else if (scenario == 'metadata departure') {
              departure = controller.leave();
              unawaited(departure.catchError((Object _) {}));
            }
            final retryWritesFrom = ble.writes.length;
            ble.holdStoreMetadata = false;
            ble.holdModeMetadata = false;
            ble.release('connection_session');
            if (retryFailure) {
              await journey.until(
                () => _litchcraftRequests(ble, retryWritesFrom).any(
                  (p) =>
                      p.length > 2 &&
                      p[2] == LitchcraftV2Protocol.frameStoreBegin,
                ),
                budgetMs: 6000,
                reason:
                    'The automatic retry must reach its real Begin before the SDK write fault',
              );
              ble.failNextConnectionSessionWrite = true;
            }
            if (departure != null) await journey.finish(departure);
          }
          debugPrint(
            'BALLS_RR_OPEN scenario=$scenario error=$loadError '
            'status=${controller.status.name} unresolved=${controller.storeReplacementUnresolved} '
            'frames=${library.animationDocument.frames.length} connected=${service.isConnected} '
            'connections=${ble.connections.length}',
          );
          if (scenario.startsWith('metadata ')) {
            await ble.advance(tester, 6500);
            if (scenario == 'metadata physical edit') {
              await journey.until(
                () => controller.status == LitchcraftStatus.conflict,
                budgetMs: 2000,
                reason:
                    'An independently changed predecessor requires a source choice',
              );
            }
            expect(controller.storeReplacementRecoveryPending, isFalse);
            expect(library.isOpenFailureHandled(loadError!), isFalse);
            final retained =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }))['store_frame']
                    as Map<String, dynamic>;
            expect(
              retained['frame_count'],
              scenario == 'metadata newer Open' ? 1 : 256,
              reason:
                  'A stale timed-out RR Open cannot replace the newer blank artwork or a changed or retired predecessor',
            );
            expect(
              trace.events.where(
                (e) =>
                    e['event'] ==
                    'litchcraft.interrupted_store_replacement_resumed',
              ),
              isEmpty,
              reason:
                  'The timeout witness cannot cross edit, source or departure fences',
            );
            if (scenario == 'metadata newer Open') {
              expect(library.animationDocument.frames, hasLength(1));
            }
            debugPrint(
              'BALLS_RR_FENCE scenario=$scenario status=${controller.status.name} '
              'board_frames=${retained['frame_count']} app_frames=${library.animationDocument.frames.length}',
            );
            await journey.finish(service.disconnect());
            return;
          }
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.storeReplacementUnresolved &&
                library.animationDocument.frames.length == 32,
            budgetMs: 6000,
            reason:
                'Explicit RR must recover automatically without manual resend',
          );
          await _expectCleanStoreParity(journey, controller);
          if (delayedSave) {
            expect(
              draftStore.heldSaves,
              greaterThan(0),
              reason:
                  'The actual new-document Store.save leaf must still be held',
            );
            expect(draftStore.completedHeldSaves, 0);
            expect(controller.status, LitchcraftStatus.live);
            expect(controller.storeReplacementRecoveryPending, isFalse);
            expect(find.byType(SnackBar), findsNothing);
            draftStore.failSave = delayedFailure;
            draftStore.holdNewDocumentSave!.complete();
            await journey.until(
              () => delayedFailure
                  ? library.saveFailed
                  : draftStore.completedHeldSaves > 0,
              budgetMs: 1000,
            );
            await tester.pump(const Duration(milliseconds: 400));
            expect(
              find.byType(SnackBar),
              delayedFailure ? findsOneWidget : findsNothing,
              reason:
                  'A delayed clean save must not report failure after verified delivery; a delayed failed save remains visible',
            );
            expect(library.saveFailed, delayedFailure);
            if (delayedFailure) {
              expect(library.lastSaveError, same(draftStore.failure));
              draftStore.failSave = false;
              await journey.finish(library.flush());
              await journey.finish(service.disconnect());
              return;
            }
          }
          expect(
            find.text(
              'That action could not finish. Your canvas is still here. Try again.',
            ),
            findsNothing,
          );
          expect(find.byType(SnackBar), findsNothing);
          expect(controller.storeReplacementRecoveryPending, isFalse);
          if (loadError != null) {
            expect(library.isOpenFailureHandled(loadError!), isTrue);
          }
          expect(controller.frameStoreArtworkReadPending, isFalse);
          if (metadataStall) {
            expect(
              trace.events.where(
                (e) =>
                    e['event'] ==
                    'litchcraft.interrupted_store_replacement_resumed',
              ),
              hasLength(retryFailure ? 2 : 1),
              reason:
                  'Each automatic attempt consumes its own unchanged-root proof',
            );
            if (retryFailure) {
              expect(
                trace.events.where(
                  (e) => e['event'] == 'ble.connection_session_retired',
                ),
                isNotEmpty,
              );
            } else {
              expect(
                ble.connections,
                hasLength(1),
                reason: 'Repair reuses the healthy shared connection',
              );
            }
          }
          debugPrint(
            'BALLS_RR_OPEN recovered=$metadataStall status=${controller.status.name} '
            'error=${controller.errorMessage} frames=${library.animationDocument.frames.length}',
          );
          if (settledFence) {
            expect(library.isOpenFailureHandled(loadError!), isTrue);
            if (scenario == 'settled source change') {
              await journey.finish(library.newArtwork());
              await _expectCleanStoreParity(journey, controller);
            } else if (scenario == 'settled physical edit') {
              await journey.finish(controller.setBrush(color: 0x12abcd));
              await journey.hall(BigInt.one);
              await ble.advance(tester, 120);
              await journey.hall(BigInt.zero);
              await ble.advance(tester, 120);
              final changed =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': 0,
                      }))['store_frame']
                      as Map<String, dynamic>;
              expect(
                changed['dirty'],
                isTrue,
                reason: 'Actual Hall paint must dirty the settled root',
              );
            } else if (scenario == 'settled departure') {
              await journey.finish(controller.leave());
            } else if (scenario == 'settled board change') {
              await journey.finish(service.disconnect());
              await journey.finish(service.connect('virtual-other-board'));
              await journey.finish(controller.enter());
            } else {
              final before =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': 0,
                      }))['store_frame']
                      as Map<String, dynamic>;
              final connection = await service.openConnectionSession(_boardId);
              final nativeState = await journey.frame();
              final competing = LitchcraftFrameStoreProtocol.prepare(
                session: nativeState['litchcraft_session'] as int,
                transfer: 0x7001,
                expectedGeneration: before['root_generation'] as int,
                topology: 3,
                active: true,
                playbackOnEntry: false,
                sparkleSeed: 1,
                favoriteColors: litchcraftDefaultFavoriteColors,
                frames: [
                  LitchcraftAnimationFrame(
                    emitters: List.filled(
                      256,
                      const LitchcraftAnimatedEmitter(color: 0x334455),
                    ),
                  ),
                ],
              );
              final client = createConnectionLitchcraftFrameStoreClient(
                session: connection!,
                monotonicMilliseconds: () => journey.now,
              );
              await journey.finish(
                client.send(competing, maximumPacketBytes: 230),
              );
              await journey.until(
                () => !library.isOpenFailureHandled(loadError!),
                budgetMs: 2000,
                reason:
                    'Fresh different root authority retires feedback settlement',
              );
            }
            await ble.advance(tester, 200);
            expect(controller.storeReplacementRecoveryPending, isFalse);
            expect(
              library.isOpenFailureHandled(loadError!),
              isFalse,
              reason:
                  'A settled error cannot cross source, exit, board or root fences',
            );
            debugPrint(
              'OPEN_FEEDBACK settled_fence=$scenario status=${controller.status.name} handled=false',
            );
          }
          await journey.finish(service.disconnect());
        } finally {
          // Early returns also finish producers before Flutter timer checks.
          await cleanupJourney();
        }
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final scenario in [
    'healthy Open',
    'failed-entry Open',
    'failed-entry New',
    'departure',
    'newer source',
    'unknown mode',
    'changed root',
  ]) {
    final stallEntry = scenario != 'healthy Open';
    final heldMode = [
      'departure',
      'newer source',
      'unknown mode',
      'changed root',
    ].contains(scenario);
    testWidgets(
      'Litchcraft: corrupt empty Store explicit replacement bootstrap $scenario',
      (tester) async {
        final native = (await tester.runAsync(
          () async => VirtualLitchProcess.fromProcess(
            await Process.start(Platform.environment['LITCH_VIRTUAL_BRIDGE']!, [
              '1',
              '0',
              'v3',
              'corrupt-store',
            ]),
          ),
        ))!;
        final ble = _DelayedOpenMetadataBle(native, operationIntervalMs: 8);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          ble.holdStoreMetadata = false;
          ble.holdModeMetadata = false;
          ble.release('connection_session');
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        final original = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..paintSelectedFrame(squares: [0], mask: 15, color: 0x12abcd);
        addTearDown(original.dispose);
        controller.restoreDocument(original.document);
        ble.holdStoreMetadata = stallEntry;
        await journey.finish(controller.enter(), budgetMs: 10000);
        ble.holdStoreMetadata = false;
        ble.release('connection_session');
        final before = await journey.frame();
        expect(before['litchcraft_active'], isTrue);
        expect(before['litchcraft_session'], isNot(0));
        expect(controller.status, LitchcraftStatus.error);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        final writesFrom = ble.writes.length;
        Object? openError;
        var settled = false;
        ble.holdModeMetadata = heldMode;
        final opening = scenario == 'failed-entry New'
            ? library.newArtwork()
            : library.loadRickroll();
        unawaited(
          opening.then(
            (_) => settled = true,
            onError: (Object error, StackTrace _) {
              openError = error;
              settled = true;
            },
          ),
        );
        Future<void>? following;
        if (heldMode) {
          await journey.until(
            () => ble.held.isNotEmpty,
            budgetMs: 1000,
            reason: 'Bootstrap must reach its real read-only mode inspection',
          );
          expect(
            _litchcraftRequests(ble, writesFrom).where(
              (p) =>
                  p.length > 2 &&
                  p[0] == LitchcraftV2Protocol.magic &&
                  p[2] == LitchcraftV2Protocol.frameStoreBegin,
            ),
            isEmpty,
            reason: 'No replacement is authorized before mode and Store proof',
          );
          if (scenario == 'departure') {
            following = controller.leave();
            unawaited(following.catchError((Object _) {}));
          } else if (scenario == 'newer source') {
            following = library.newArtwork();
            unawaited(following.catchError((Object _) {}));
            await journey.until(
              () => library.animationDocument.frames.length == 1,
              budgetMs: 1000,
            );
          } else if (scenario == 'changed root') {
            final connection = await service.openConnectionSession(_boardId);
            final nativeSession = before['litchcraft_session'] as int;
            final competing = LitchcraftFrameStoreProtocol.prepare(
              session: nativeSession,
              transfer: 0x7000,
              expectedGeneration: 0,
              topology: 3,
              active: true,
              playbackOnEntry: false,
              sparkleSeed: 1,
              favoriteColors: litchcraftDefaultFavoriteColors,
              frames: [
                LitchcraftAnimationFrame(
                  emitters: List.filled(
                    256,
                    const LitchcraftAnimatedEmitter(color: 0x334455),
                  ),
                ),
              ],
            );
            final client = createConnectionLitchcraftFrameStoreClient(
              session: connection!,
              monotonicMilliseconds: () => journey.now,
            );
            following = client
                .send(competing, maximumPacketBytes: 230)
                .then((_) {});
            unawaited(following.catchError((Object _) {}));
            var changed = false;
            final changeStart = journey.now;
            while (!changed && journey.now - changeStart < 3000) {
              await journey.tick();
              final frame =
                  (await ble.command(tester, {
                        'op': 'store_frame',
                        'index': 0,
                      }, flushOperations: false))['store_frame']
                      as Map<String, dynamic>;
              changed =
                  frame['frame_count'] == 1 &&
                  (frame['root_generation'] as int? ?? 0) != 0;
            }
            expect(
              changed,
              isTrue,
              reason:
                  'The competing typed producer commits before inspection resumes',
            );
          }
          if (scenario == 'unknown mode') {
            await journey.until(() => settled, budgetMs: 10000);
          }
          ble.holdModeMetadata = false;
          ble.release('connection_session');
        }
        await journey.until(() => settled, budgetMs: 10000);
        if (following != null) await journey.finish(following);
        final begins = _litchcraftRequests(ble, writesFrom)
            .where(
              (p) =>
                  p.length > 2 &&
                  p[0] == LitchcraftV2Protocol.magic &&
                  p[2] == LitchcraftV2Protocol.frameStoreBegin,
            )
            .length;
        final ownBegins = _litchcraftRequests(ble, writesFrom)
            .where(
              (p) =>
                  p.length == 53 &&
                  p[0] == LitchcraftV2Protocol.magic &&
                  p[2] == LitchcraftV2Protocol.frameStoreBegin &&
                  (p[21] | p[22] << 8) ==
                      library.animationDocument.frames.length,
            )
            .length;
        debugPrint(
          'CORRUPT_OPEN_BOOTSTRAP scenario=$scenario entry_stall=$stallEntry status=${controller.status.name} '
          'board_active=${controller.boardActive} source_frames=${library.animationDocument.frames.length} '
          'total_begin=$begins own_begin=$ownBegins error=$openError',
        );
        if (scenario != 'newer source') {
          expect(
            sameLitchcraftDocument(
              library.previousDraft!.document,
              original.document,
            ),
            isTrue,
            reason:
                'Explicit replacement preserves the original app draft even if admission failed',
          );
        }
        if (['departure', 'unknown mode', 'changed root'].contains(scenario)) {
          expect(ownBegins, 0, reason: 'No unproven app body can be uploaded');
          expect(
            begins,
            scenario == 'changed root' ? 1 : 0,
            reason:
                'Only the independent producer may write; the unproven replacement cannot bootstrap',
          );
          if (scenario == 'departure') {
            expect(controller.status, LitchcraftStatus.offline);
          }
          if (scenario == 'changed root') {
            await journey.until(
              () => controller.status == LitchcraftStatus.conflict,
              budgetMs: 2000,
              reason:
                  'A newly committed different root follows existing source reconciliation',
            );
            final frame =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }))['store_frame']
                    as Map<String, dynamic>;
            expect(frame['frame_count'], 1);
          }
          await journey.finish(service.disconnect());
          return;
        }
        await journey.until(
          () =>
              controller.status == LitchcraftStatus.live &&
              !controller.storeReplacementUnresolved,
          budgetMs: 6000,
          reason:
              'A current explicit Open must bootstrap through fresh mode and empty Store proof',
        );
        expect(
          _litchcraftRequests(ble, writesFrom).where(
            (p) =>
                p.length > 2 &&
                p[0] == LitchcraftV2Protocol.magic &&
                p[2] == LitchcraftV2Protocol.frameStoreBegin,
          ),
          hasLength(1),
          reason:
              'Exactly one verified replacement may bootstrap the absent root',
        );
        await _expectCleanStoreParity(journey, controller);
        await ble.advance(tester, 20);
        final displayed = await journey.frame();
        expect(displayed['litchcraft_active'], isTrue);
        expect(displayed['litchcraft_visible'], isTrue);
        final first = library.animationDocument.frames.first;
        for (var square = 0; square < 64; ++square) {
          final expectedLit = first.emitters
              .skip(square * 4)
              .take(4)
              .any((emitter) => emitter.color != 0);
          final semantic = (displayed['board'] as List)[square] as List;
          expect(
            semantic.any(
              (pixel) => (pixel as List).take(3).any((channel) => channel != 0),
            ),
            expectedLit,
            reason:
                'The semantic canvas presents the exact first-frame square mask',
          );
          expect(
            semantic.every((pixel) => (pixel as List)[3] == 0),
            isTrue,
            reason: 'Litchcraft keeps the white die off',
          );
          // Physical quantization may turn very dark calibrated RGB off.
          // Prove mapped bright artwork, and separately prove an all-off New.
          if (first.emitters
              .skip(square * 4)
              .take(4)
              .any(
                (emitter) =>
                    (emitter.color >> 16 & 255) >= 128 ||
                    (emitter.color >> 8 & 255) >= 128 ||
                    (emitter.color & 255) >= 128,
              )) {
            final mapped =
                (displayed['logical_to_physical'] as List)[square] as int;
            final pixels = (displayed['physical'] as List).sublist(
              mapped * 4,
              mapped * 4 + 4,
            );
            expect(
              pixels.any(
                (pixel) =>
                    (pixel as List).take(3).any((channel) => channel != 0),
              ),
              isTrue,
              reason:
                  'The mapped board square presents its bright verified artwork',
            );
          }
        }
        final physicalBoard = (displayed['physical'] as List).take(256);
        expect(
          physicalBoard.any(
            (pixel) => (pixel as List).take(3).any((channel) => channel != 0),
          ),
          first.emitters.any((emitter) => emitter.color != 0),
          reason:
              'Mapped board pixels show the new artwork or a fully blank New, excluding status LEDs',
        );
        expect(physicalBoard.every((pixel) => (pixel as List)[3] == 0), isTrue);
        journey.checkpoint(
          'corrupt-store-$scenario',
          displayed,
          'accepted one-Begin verified-Commit semantic+mapped first-frame next=draw-or-play',
        );
        expect(
          library.animationDocument.frames,
          hasLength(
            ['failed-entry New', 'newer source'].contains(scenario) ? 1 : 32,
          ),
        );
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final recoveryPath in [
    'reconnect',
    'physical edit',
    'foreground',
    'foreground delayed initial authority',
    'departure',
  ]) {
    final boardChanges = recoveryPath == 'physical edit';
    final authorityGap = recoveryPath.startsWith('foreground');
    final delayedInitialAuthority =
        recoveryPath == 'foreground delayed initial authority';
    final departed = recoveryPath == 'departure';
    testWidgets(
      'Litchcraft: interrupted RR upload ${boardChanges
          ? 'preserves physical board edits'
          : authorityGap
          ? delayedInitialAuthority
                ? 'retains selected RR before foreground refresh when physical edits precede initial authority'
                : 'resumes after queued foreground authority refresh'
          : departed
          ? 'does not revive Open after failed departure'
          : 'reconnects and preserves local artwork'}',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedStoreAuthorityBle(native, operationIntervalMs: 8);
        late final _Journey journey;
        final trace = _SyncRecoveryTrace(() => journey.now);
        final service = _BridgeBoard(ble, appTrace: trace);
        journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service, appTrace: trace);
        final library = LitchcraftLibrary(controller);
        var storeWorkerPaused = false;
        addTearDown(() async {
          try {
            if (storeWorkerPaused) {
              await ble.command(tester, {
                'op': 'store_worker',
                'enabled': true,
              }, flushOperations: false);
            }
          } finally {
            library.dispose();
            controller.dispose();
            await ble.disposeJourney(tester, service);
          }
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await journey.finish(library.newArtwork());
        await _expectCleanStoreParity(journey, controller);
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        debugPrint(
          'RR_SYNC_RECOVERY seeded status=${controller.status.name} error=${controller.errorMessage}',
        );
        await journey.finish(controller.setBrush(color: 0xabcdef));
        final predecessor = authorityGap
            ? (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>
            : null;
        final before = ble.writes.length;
        var settled = false;
        Object? loadError;
        unawaited(
          library.loadRickroll().then(
            (_) => settled = true,
            onError: (Object error, StackTrace _) {
              loadError = error;
              settled = true;
            },
          ),
        );
        await journey.until(
          () => ble.writes.length > before + 20,
          budgetMs: 10000,
          reason: 'Interrupt the real full RR transfer after native admission',
        );
        final intended = controller.documentSession.document;
        expect(intended.frames, hasLength(32));
        var departureSettled = false;
        Object? departureError;
        int? departureWrites;
        if (departed) {
          // Schedule the real page departure at the actual shared-session
          // retirement callback, before the interrupted transfer's catch.
          trace.onSessionRetired = () {
            departureWrites = ble.writes.length;
            // Fail the next shared write as departure races connection
            // recovery; that new lifetime cannot acknowledge Exit.
            ble.failNextConnectionSessionWrite = true;
            unawaited(
              controller.leave().then(
                (_) => departureSettled = true,
                onError: (Object error, StackTrace _) {
                  departureError = error;
                  departureSettled = true;
                },
              ),
            );
          };
        }
        ble.holdStoreAuthority = authorityGap;
        ble.failNextConnectionSessionWrite = true;
        await journey.until(
          () => !ble.failNextConnectionSessionWrite,
          budgetMs: 3000,
          reason: 'The existing SDK fault must interrupt actual transmission',
        );
        await journey.until(() => settled, budgetMs: 12000);
        debugPrint(
          'RR_SYNC_RECOVERY load_error=$loadError status=${controller.status.name} error=${controller.errorMessage} connected=${service.isConnected} connections=${ble.connections.length}',
        );
        if (authorityGap) {
          final expectedPredecessor = predecessor!;
          final intendedSourceGeneration =
              controller.documentSession.replacementGeneration;
          expect(loadError, isA<StateError>());
          var predecessorPublished = false;
          if (!delayedInitialAuthority) {
            trace.onStoreInspectStarted = () {
              // Ordinary initial publication restores the known root generation.
              // Deliver it before the explicit post-Inspect read exists, so it
              // cannot satisfy that fresh proof or consume the Open witness.
              final publication = ble.heldStoreAuthority.lastWhere((packet) {
                if (packet.epoch != ble.nativeEpoch) return false;
                final state = ConnectionSessionCodec.decode(
                  Uint8List.fromList(packet.bytes),
                )!;
                return state.body[1] & 1 == 0 &&
                    ByteData.sublistView(
                          state.body,
                        ).getUint32(2, Endian.little) ==
                        expectedPredecessor['root_generation'];
              });
              ble.holdStoreAuthority = false;
              ble.deliver(publication);
              ble.holdStoreAuthority = true;
              predecessorPublished = true;
            };
          }
          storeWorkerPaused = true;
          await ble.command(tester, {
            'op': 'store_worker',
            'enabled': false,
          }, flushOperations: false);
          int freshAuthorityReads({String stage = 'start'}) => trace.events
              .where(
                (event) =>
                    event['event'] == 'litchcraft.store_entry_phase' &&
                    event['phase'] == 'fresh_state37' &&
                    event['stage'] == stage &&
                    event['connection_epoch'] ==
                        service.connectionSessionToken &&
                    event['source_generation'] == intendedSourceGeneration,
              )
              .length;
          await journey.until(
            () => ble.connections.length > 1 && freshAuthorityReads() > 0,
            budgetMs: 12000,
            reason: 'Reconnect reaches its actual post-Inspect authority read',
          );
          expect(predecessorPublished, !delayedInitialAuthority);
          expect(controller.storeReplacementUnresolved, isTrue);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
            reason:
                'Initial publication cannot replace the selected RR preview',
          );
          // A genuine dirty authority can finish entry as live but unresolved.
          // Paint then physical Undo restores the exact predecessor bytes while
          // the paused existing worker keeps its durable root unchanged.
          await journey.hall(BigInt.one);
          await ble.advance(tester, 120);
          final painted = await journey.frame();
          expect(painted['board'][0], everyElement([0xab, 0xcd, 0xef, 0]));
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 400);
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 5,
            'down': true,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 5,
            'down': false,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          final undone =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(undone['dirty'], isTrue);
          expect(
            undone['root_generation'],
            expectedPredecessor['root_generation'],
          );
          final restored = await journey.frame();
          expect(
            (restored['board'] as List).expand((square) => square as List),
            everyElement([0, 0, 0, 0]),
            reason: 'Physical Undo restores every predecessor emitter',
          );
          final dirtyAuthority = ble.heldStoreAuthority.last;
          final dirtyPacket = ConnectionSessionCodec.decode(
            Uint8List.fromList(dirtyAuthority.bytes),
          )!;
          expect(dirtyAuthority.epoch, ble.nativeEpoch);
          expect(dirtyPacket.body[1] & 1, 1);
          expect(
            ByteData.sublistView(dirtyPacket.body).getUint32(2, Endian.little),
            expectedPredecessor['root_generation'],
          );
          expect(
            ByteData.sublistView(dirtyPacket.body).getUint32(6, Endian.little),
            undone['live_revision'],
          );
          ble.holdStoreAuthority = false;
          ble.deliver(dirtyAuthority);
          ble.holdStoreAuthority = true;
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.authorityResolving,
            budgetMs: 10000,
            reason: 'Real dirty proof settles entry before warm revalidation',
          );
          expect(controller.boardArtworkUnresolved, isTrue);
          expect(controller.storeReplacementUnresolved, isTrue);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
            reason:
                'Physical predecessor repair cannot mutate the selected RR preview',
          );
          final beforeRefresh = freshAuthorityReads();
          final refreshWritesFrom = ble.writes.length;
          var refreshed = false;
          unawaited(
            controller.revalidateSelectedBoard().then((_) => refreshed = true),
          );
          await journey.until(
            () => freshAuthorityReads() > beforeRefresh,
            budgetMs: 10000,
            reason:
                'Warm queued refresh reaches its own post-Inspect authority read',
          );
          expect(controller.authorityResolving, isTrue);
          expect(refreshed, isFalse);
          expect(
            trace.events.where(
              (event) =>
                  event['event'] ==
                  'litchcraft.interrupted_store_replacement_resumed',
            ),
            isEmpty,
            reason: 'Dirty authority cannot consume the predecessor witness',
          );
          final cleanAuthorityFrom = ble.heldStoreAuthority.length;
          await ble.command(tester, {
            'op': 'store_worker',
            'enabled': true,
          }, flushOperations: false);
          storeWorkerPaused = false;
          // Keep the active Query/replay owner's SDK writes draining between
          // timer ticks while the existing no-op save reaches clean authority.
          // One bulk Flutter pump would strand its retry writes until timeout.
          await journey.until(
            () => ble.heldStoreAuthority.skip(cleanAuthorityFrom).any((packet) {
              if (packet.epoch != ble.nativeEpoch) return false;
              final state = ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              )!;
              return state.body[1] & 1 == 0;
            }),
            budgetMs: 6000,
            reason: 'The normal Store save publishes genuine clean authority',
          );
          final clean =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(clean['dirty'], isFalse);
          for (final key in [
            'root_generation',
            'frame_count',
            'topology',
            'playback_on_entry',
            'sparkle_seed',
            'favorite_colors',
            'duration_ms',
            'emitters',
          ]) {
            expect(
              clean[key],
              expectedPredecessor[key],
              reason: 'Exact predecessor $key',
            );
          }
          expect(
            controller.documentSession.replacementGeneration,
            intendedSourceGeneration,
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
            reason: 'Authority refresh retains the selected RR source',
          );
          final cleanAuthority = ble.heldStoreAuthority.last;
          final cleanPacket = ConnectionSessionCodec.decode(
            Uint8List.fromList(cleanAuthority.bytes),
          )!;
          expect(cleanAuthority.epoch, ble.nativeEpoch);
          expect(cleanPacket.body[1] & 1, 0);
          expect(
            ByteData.sublistView(cleanPacket.body).getUint32(2, Endian.little),
            expectedPredecessor['root_generation'],
          );
          ble.holdStoreAuthority = false;
          ble.deliver(cleanAuthority);
          await journey.until(
            () => refreshed,
            budgetMs: 25000,
            reason:
                'Warm refresh releases its lane before replacement delivery',
          );
          expect(
            controller.errorMessage,
            isNull,
            reason:
                'Settling a failed authority read is not a successful refresh',
          );
          expect(
            freshAuthorityReads(stage: 'complete'),
            greaterThan(beforeRefresh),
            reason:
                'The warm refresh must complete its actual fresh authority read',
          );
          expect(
            trace.events.where(
              (event) =>
                  event['event'] ==
                  'litchcraft.interrupted_store_replacement_resumed',
            ),
            hasLength(1),
          );
          expect(
            _litchcraftRequests(ble, refreshWritesFrom).where(
              (packet) =>
                  packet[0] == LitchcraftProtocol.magic &&
                  packet[1] == LitchcraftProtocol.enter,
            ),
            isEmpty,
            reason:
                'Foreground revalidation uses the warm lane without admission',
          );
        }
        if (departed) {
          await journey.until(() => departureSettled, budgetMs: 10000);
          expect(
            departureError,
            isNotNull,
            reason: 'The retired native lifetime cannot acknowledge departure',
          );
          await journey.until(
            () => !controller.authorityResolving,
            budgetMs: 10000,
            reason:
                'Concurrent automatic entry settles the failed-departure barrier',
          );
          expect(loadError, isNotNull);
          expect(service.isConnected, isFalse);
          expect(controller.status, LitchcraftStatus.offline);
          expect(controller.errorMessage, isNotNull);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
            reason: 'Failed departure preserves the latest phone draft',
          );
          expect(departureWrites, isNotNull);
          expect(
            _litchcraftRequests(ble, departureWrites!).where(
              (packet) => packet[2] == LitchcraftV2Protocol.frameStoreBegin,
            ),
            isEmpty,
            reason: 'The canceled Open cannot begin again before a new Retry',
          );
          expect(
            trace.events.where(
              (event) =>
                  event['event'] ==
                  'litchcraft.interrupted_store_replacement_resumed',
            ),
            isEmpty,
            reason: 'Departure retires the prior replacement recovery witness',
          );
          final predecessor = (await ble.command(tester, {
            'op': 'store_frame',
            'index': 0,
          }))['store_frame'];
          expect(predecessor['frame_count'], 1);
          expect(
            (predecessor['emitters'] as List).every(
              (e) => (e as List)[0] == 0 && e[1] == 0,
            ),
            isTrue,
            reason: 'The original empty committed predecessor is unchanged',
          );
          final retryWrites = ble.writes.length;
          final retryStarted = journey.now;
          await journey.finish(service.connect(_boardId));
          await journey.finish(
            controller.retryConnection(),
            budgetMs: (10000 - (journey.now - retryStarted))
                .clamp(0, 10000)
                .toInt(),
          );
          await journey.until(
            () =>
                !controller.boardArtworkUnresolved &&
                controller.fullAnimationSyncDirection == null &&
                !controller.pending &&
                !ble.hasPendingOperations,
            budgetMs: (10000 - (journey.now - retryStarted))
                .clamp(0, 10000)
                .toInt(),
            reason:
                'Explicit Retry must settle authoritative artwork readiness '
                'within its existing budget before checking source resolution',
          );
          final statusAfterRetry = controller.status;
          final retryPackets = _litchcraftRequests(ble, retryWrites);
          expect(
            statusAfterRetry,
            LitchcraftStatus.live,
            reason:
                'A new explicit Retry proves the empty predecessor and sends '
                'the selected app artwork through ordinary common-canvas policy',
          );
          expect(
            trace.events.where(
              (event) =>
                  event['event'] ==
                  'litchcraft.interrupted_store_replacement_resumed',
            ),
            isEmpty,
            reason:
                'Even the new Retry must not reuse the canceled Open witness',
          );
          for (final opcode in [
            LitchcraftV2Protocol.frameStoreBegin,
            LitchcraftV2Protocol.frameStoreCommit,
          ]) {
            expect(
              retryPackets.where((packet) => packet[2] == opcode),
              hasLength(1),
              reason: 'The new user Retry owns one verified replacement',
            );
          }
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
          );
          final retained =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(
            retained['frame_count'],
            32,
            reason: 'Only the new explicit Retry may commit the selected draft',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.boardDocument!,
              intended,
            ),
            isTrue,
          );
          await journey.finish(library.flush());
          final persisted = await LitchcraftArtworkStore(
            await SharedPreferences.getInstance(),
          ).load();
          expect(persisted.draft, isNotNull);
          expect(
            sameLitchcraftDocument(persisted.draft!.document, intended),
            isTrue,
            reason: 'The exact latest draft survives phone persistence',
          );
        } else if (boardChanges) {
          await journey.hall(BigInt.one);
          await ble.advance(tester, 120);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 120);
          final changed =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          debugPrint('RR_SYNC_RECOVERY physical_changed=$changed');
          expect(
            changed['dirty'],
            isTrue,
            reason:
                'The production Hall stroke must dirty live board artwork; clean frame bodies are unavailable until saved',
          );
          await journey.until(
            () => controller.status == LitchcraftStatus.conflict,
            budgetMs: 25000,
            reason:
                'An independently changed board still needs an explicit source choice',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              intended,
            ),
            isTrue,
          );
          await ble.advance(tester, 6000);
          expect(controller.status, LitchcraftStatus.conflict);
          final retained =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect((retained['emitters'] as List).first[0], 0xabcdef);
          expect(
            retained['frame_count'],
            1,
            reason:
                'Recovery must not overwrite physical work with the 32-frame draft',
          );
        } else {
          await journey.until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.storeReplacementUnresolved &&
                sameLitchcraftDocument(
                  controller.documentSession.document,
                  intended,
                ),
            budgetMs: 25000,
            reason:
                'A failed RR transfer must automatically recover the same board and local animation',
          );
          expect(service.connectedId, _boardId);
          expect(ble.connections.length, greaterThan(1));
          expect(
            controller.frameStoreArtworkReadPending,
            isFalse,
            reason: 'Recovered Commit cannot retain a stale source chooser',
          );
          await _expectCleanStoreParity(journey, controller);
          await journey.finish(controller.setBrush(color: 0x123456));
          controller.beginStroke();
          controller.paintSquares([8]);
          controller.endStroke();
          await journey.until(
            () => !controller.pending,
            budgetMs: 10000,
            reason: 'A fresh ordinary app stroke drains after recovery',
          );
          await _expectCleanStoreParity(journey, controller);
          await journey.hall(BigInt.one);
          await ble.advance(tester, 120);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 120);
          await _expectCleanStoreParity(journey, controller);
        }
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.iOS,
        TargetPlatform.android,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: five distinct 256-frame uploads survive clear and Exit/re-entry',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      // A prior physical 895-write upload submitted in 4.9 s. Add bounded
      // per-operation delay so this journey also exercises a slower callback
      // schedule across all five nonresident full uploads.
      final ble = VirtualLitchBle(native, operationIntervalMs: 8);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final deliveryProbe = _StoreDeliveryProbe();
      final controller = LitchcraftController(
        service,
        appTrace: deliveryProbe,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      expect(controller.boardActive, isTrue, reason: controller.errorMessage);

      final balls = LitchcraftBouncingBallsPreset.decode().frames;
      for (var cycle = 1; cycle <= 5; cycle++) {
        await journey.finish(library.newArtwork());
        expect(controller.documentSession.document.frames, hasLength(1));
        final blankRoot = await _expectCleanStoreParity(journey, controller);
        final first = balls.first;
        final emitters = List<LitchcraftAnimatedEmitter>.of(first.emitters);
        final original = emitters.first;
        emitters[0] = LitchcraftAnimatedEmitter(
          color: 0x123400 + cycle,
          effect: original.effect,
          effectDurationMs: original.effectDurationMs,
        );
        final variant = List<LitchcraftAnimationFrame>.of(balls);
        variant[0] = LitchcraftAnimationFrame(
          durationMs: first.durationMs,
          emitters: emitters,
        );
        final beforeDelivery = deliveryProbe.deliveries.length;
        final now = DateTime.now().toUtc();
        final uploadStartedMs = journey.now;
        await journey.finish(
          library.open(
            LitchcraftArtwork.animated(
              id: 'distinct-balls-$cycle',
              name: 'Artwork',
              document: LitchcraftAnimationDocument(
                frames: variant,
                topology: controller.canvas.topology,
                favoriteColors: library.favoriteColors,
                playbackOnEntry: true,
              ),
              createdAt: now,
              modifiedAt: now,
            ),
          ),
        );
        final uploadElapsedMs = journey.now - uploadStartedMs;
        expect(
          uploadElapsedMs,
          lessThan(10000),
          reason: 'Cycle $cycle exceeded the 10 s simulated callback budget',
        );
        expect(deliveryProbe.deliveries.length, beforeDelivery + 1);
        final delivered = deliveryProbe.deliveries.last;
        expect(delivered['resident_hit'], isFalse);
        expect(delivered['frames'], 256);
        expect(
          delivered['body_bytes'],
          greaterThan(100000),
          reason:
              'Cycle $cycle must upload a distinct full body, not select a resident',
        );
        expect(
          delivered['chunks'],
          greaterThan(500),
          reason:
              'The capability-gated RGB body still crosses as a full upload',
        );
        expect(
          delivered['writes'],
          (delivered['chunks'] as int) + 2,
          reason:
              'Only Begin, all chunks, and final Commit cross the Object lane',
        );
        final ballsRoot = await _expectCleanStoreParity(journey, controller);
        expect(ballsRoot, isNot(blankRoot));
        expect(
          controller.documentSession.document.frames.first.emitters.first.color,
          0x123400 + cycle,
        );
        journey.checkpoint(
          'distinct-full-upload-$cycle',
          await journey.frame(),
          'bytes=${delivered['body_bytes']} '
              'chunks=${delivered['chunks']} root=$ballsRoot '
              'simulated_sync_ms=$uploadElapsedMs',
        );
        if (cycle == 3) {
          await journey.finish(controller.leave());
          final exited = await journey.frame();
          expect(exited['litchcraft_active'], isFalse);
          expect(
            exited['required_action'],
            9, // CorrectBoard: the empty board needs Play-side setup.
          );
          journey.checkpoint(
            'play-side-setup-after-exit',
            exited,
            'Exit reached correction/setup; a playable game is not asserted',
          );
          await journey.finish(controller.enter());
          expect(
            controller.boardActive,
            isTrue,
            reason: controller.errorMessage,
          );
          await _expectCleanStoreParity(journey, controller);
        }
      }
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: a fresh phone reads all 256 Store frames at MTU 512',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, negotiatedMtu: 512);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final controller = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      final library = LitchcraftLibrary(controller);
      var retired = false;
      addTearDown(() {
        if (!retired) {
          library.dispose();
          controller.dispose();
        }
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(library.loadBouncingBallsBenchmark());
      expect(controller.documentSession.document.frames, hasLength(256));
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(library.flush());
      library.dispose();
      controller.dispose();
      retired = true;
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final fresh = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
          localIntent: false,
        ),
      );
      addTearDown(fresh.dispose);
      await journey.finish(fresh.enter());
      await journey.until(
        () => fresh.documentSession.document.frames.length == 256,
        budgetMs: 6000,
        reason: 'MTU 512 readback must complete the entire resident artwork',
      );
      await _expectCleanStoreParity(journey, fresh);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: interrupted 256-frame read releases the old Store owner',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final seed = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      final library = LitchcraftLibrary(seed);
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(seed.enter());
      await journey.finish(library.loadBouncingBallsBenchmark());
      await journey.finish(library.flush());
      library.dispose();
      seed.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);

      await journey.finish(service.connect(_boardId));
      final interrupted = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
          localIntent: false,
        ),
      );
      final entering = interrupted.enter();
      var readActive = false;
      for (var elapsed = 0; !readActive && elapsed < 3000; elapsed += 10) {
        readActive =
            (await ble.command(tester, {
              'op': 'store_frame',
              'index': 0,
            }, flushOperations: false))['store_frame']['read_active'] ==
            true;
        if (!readActive) await journey.tick();
      }
      expect(
        readActive,
        isTrue,
        reason:
            'The fresh phone must start a Store Snapshot before interruption',
      );
      // Drop progress only after the actual native Snapshot is admitted.
      // Earlier loss would stall Inspect before there is a read to abandon.
      ble.reverseProgressDropsRemaining = 1000;
      await journey.finish(entering);
      await journey.until(
        () => ble.droppedReverseProgress >= 8,
        budgetMs: 3000,
        reason: 'A full reverse Object credit window must be in flight',
      );
      await ble.advance(tester, 100);
      expect(interrupted.documentSession.document.frames, hasLength(1));
      expect(
        (await ble.command(tester, {
          'op': 'store_frame',
          'index': 0,
        }))['store_frame']['read_active'],
        isTrue,
        reason: 'The interrupted owner must still hold a pending Store read',
      );
      interrupted.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      // The SDK queues the peripheral link event when the phone cancels its
      // subscription. Observe Store after the board receives that event.
      await ble.flush(tester);
      expect(ble.nativeSessionActivated, isFalse);
      expect(
        (await ble.command(tester, {
          'op': 'store_frame',
          'index': 0,
        }))['store_frame']['read_active'],
        isFalse,
        reason:
            'Disconnect must retire the abandoned read before any new Snapshot',
      );

      ble.reverseProgressDropsRemaining = 0;
      await journey.finish(service.connect(_boardId));
      final recovered = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
          localIntent: false,
        ),
      );
      addTearDown(recovered.dispose);
      await journey.finish(recovered.enter());
      await journey.until(
        () => recovered.documentSession.document.frames.length == 256,
        budgetMs: 6000,
        reason: 'New owner must read all 256 frames after interruption',
      );
      await _expectCleanStoreParity(journey, recovered);
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final selection in [
    'palette',
    'palette pre-ACK State',
    'hue',
    'hue teardown',
  ]) {
    testWidgets(
      'Litchcraft: Store $selection selection reaches the physical brush through Ordered Tool',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(
          native,
          negotiatedMtu: 23,
          operationIntervalMs: 20,
        );
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final document = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..addFrame();
        final seed = LitchcraftController(service, documentSession: document);
        addTearDown(() => ble.disposeJourney(tester, service));
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(seed.enter());
        await journey.finish(
          seed.transferFrameStoreForDiagnostics(
            frames: document.document.frames,
            playbackOnEntry: false,
          ),
        );
        seed.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
        await ble.flush(tester);
        await journey.finish(service.connect(_boardId));
        final controller = LitchcraftController(
          service,
          documentSession: LitchcraftDocumentSession(
            document.document,
            localIntent: false,
          ),
        );
        final library = LitchcraftLibrary(controller);
        var disposed = false;
        addTearDown(() {
          if (!disposed) {
            library.dispose();
            controller.dispose();
          }
        });
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        expect(controller.status, LitchcraftStatus.live);
        await ble.advance(tester, 300);
        // Establish a live confirmed brush before testing a second choice;
        // the initial entry State can precede this controller's mode binding.
        await journey.finish(controller.selectPaletteColor(0x445566));
        await ble.advance(tester, 300);
        expect(controller.color, 0x445566);
        final writesBefore = ble.writes.length;
        // Real Ordered receipts remain deliverable while the existing SDK
        // scheduler holds production State. This isolates receipt-before-State.
        ble.holdConnectionObservations = true;
        Future<void> awaitToolConfirmation(int color) => journey.until(
          () {
            final tools = ble.writes
                .skip(writesBefore)
                .map(
                  (write) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(write['bytes'] as List<int>),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .where(
                  (packet) =>
                      packet.kind == ConnectionSessionKind.ordered &&
                      packet.code == ConnectionSession.litchcraftToolOpcode,
                )
                .toList();
            if (tools.isEmpty) return false;
            final latest = tools.last;
            final confirmed = ble.writes.skip(writesBefore).any((write) {
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind == ConnectionSessionKind.control &&
                  packet?.code == ConnectionSessionControl.confirm &&
                  packet?.token == latest.token &&
                  packet!.sequence >= latest.sequence;
            });
            return confirmed &&
                ble.held.any((held) {
                  final packet = ConnectionSessionCodec.decode(
                    Uint8List.fromList(held.bytes),
                  );
                  return packet?.kind == ConnectionSessionKind.state &&
                      packet?.code == ConnectionSession.litchcraftStateCode &&
                      LitchcraftProtocol.readRgb(packet!.body, 6) == color;
                });
          },
          budgetMs: 500,
          reason: 'Tool is confirmed while its production State stays held',
        );
        var selectedColor = 0x339966;
        Future<void>? choice;
        if (selection.startsWith('palette')) {
          if (selection == 'palette pre-ACK State') {
            await journey.finish(controller.selectAnimationFrame(1));
            await ble.advance(tester, 300);
            expect(
              ble.held.any((held) {
                final packet = ConnectionSessionCodec.decode(
                  Uint8List.fromList(held.bytes),
                );
                return packet?.kind == ConnectionSessionKind.state &&
                    packet?.code == ConnectionSession.litchcraftStateCode &&
                    packet!.body[0] == 1 &&
                    LitchcraftProtocol.readRgb(packet.body, 6) == 0x445566;
              }),
              isTrue,
              reason: 'Retain genuine pre-Tool timeline State',
            );
            choice = controller.selectPaletteColor(selectedColor);
            for (var turn = 0; turn < 5; turn++) {
              await tester.pump();
            }
            // The native SDK write has begun but is not flushed yet. Deliver
            // its earlier State before Tool ACK, preserving characteristic order.
            expect(
              ble.writes.skip(writesBefore).any((write) {
                final packet = ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                );
                return packet?.kind == ConnectionSessionKind.ordered &&
                    packet?.code == ConnectionSession.litchcraftToolOpcode;
              }),
              isTrue,
            );
            ble.release('connection_session');
            await tester.pump();
          } else {
            choice = controller.selectPaletteColor(selectedColor);
            await journey.finish(choice);
          }
        } else {
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          );
          final hue = find.byKey(const ValueKey('craft-inspector-hue'));
          final rect = tester.getRect(hue);
          final pointer = await tester.startGesture(
            Offset(rect.left + 24, rect.center.dy),
          );
          await pointer.moveTo(
            Offset(rect.left + rect.width * .35, rect.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 16));
          final first = tester.widget<Slider>(hue).value;
          final firstColor = controller.color;
          await awaitToolConfirmation(firstColor);
          expect(
            controller.color,
            firstColor,
            reason: 'Receipt-before-State retains the finger-held brush',
          );
          // Inspect every rebuild while production State remains held.
          for (var turn = 0; turn < 4; ++turn) {
            await journey.tick();
            expect(tester.widget<Slider>(hue).value, first);
            expect(controller.color, firstColor);
          }
          if (selection == 'hue teardown') {
            await tester.pumpWidget(const SizedBox.shrink());
            await pointer.cancel();
            ble.holdConnectionObservations = false;
            ble.release('connection_session');
            await ble.flush(tester);
            // A different authoritative brush selection after view teardown
            // must reach this still-live controller. Use the same native Tool
            // owner, rather than manufacturing a passive State packet.
            ConnectionSession? session;
            await journey.finish(
              service.openConnectionSession(_boardId).then((value) {
                session = value;
              }),
            );
            await journey.finish(
              session!.sendOrdered(
                ConnectionSession.litchcraftToolOpcode,
                Uint8List.fromList([0, 15, 0x22, 0x55, 0x88]),
              ),
            );
            selectedColor = 0x225588;
            await journey.until(
              () => controller.color == selectedColor,
              budgetMs: 500,
              reason: 'Inspector teardown releases the held brush owner',
            );
          } else {
            await pointer.moveTo(
              Offset(rect.left + rect.width * .7, rect.center.dy),
            );
            await tester.pump(const Duration(milliseconds: 16));
            expect(tester.widget<Slider>(hue).value, greaterThan(first));
            selectedColor = controller.color;
            await pointer.up();
            await journey.until(
              () {
                final observed = ble.emitted
                    .where((packet) => packet.channel == 'connection_session')
                    .map(
                      (packet) => ConnectionSessionCodec.decode(
                        Uint8List.fromList(packet.bytes),
                      ),
                    )
                    .whereType<ConnectionSessionPacket>()
                    .where(
                      (packet) =>
                          packet.kind == ConnectionSessionKind.state &&
                          packet.code == ConnectionSession.litchcraftStateCode,
                    )
                    .last;
                return LitchcraftProtocol.readRgb(observed.body, 6) ==
                    selectedColor;
              },
              budgetMs: 500,
              reason: 'Latest hue reaches authoritative Tool State',
            );
          }
        }
        if (selection != 'hue teardown') {
          await awaitToolConfirmation(selectedColor);
        }
        if (selection == 'palette pre-ACK State') {
          var choiceCompleted = false;
          choice!.then<void>(
            (_) => choiceCompleted = true,
            onError: (Object _, StackTrace _) => choiceCompleted = true,
          );
          await journey.until(
            () => ble.writes.skip(writesBefore).any((write) {
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind == ConnectionSessionKind.control &&
                  packet?.code == ConnectionSessionControl.query &&
                  packet?.sequence == ConnectionSession.litchcraftStateCode;
            }),
            budgetMs: 500,
            reason: 'Ambiguous observation requests current board truth',
          );
          // Complete the actual Query SDK write while its State response stays
          // held. Write completion alone cannot release the brush selection.
          await ble.flush(tester);
          for (var turn = 0; turn < 4; ++turn) {
            await journey.tick();
            expect(controller.color, selectedColor);
            expect(choiceCompleted, isFalse);
          }
        }
        await tester.pump();
        expect(
          controller.color,
          selectedColor,
          reason: 'The selected color survives a receipt ahead of State',
        );
        ble.holdConnectionObservations = false;
        ble.release('connection_session');
        await ble.flush(tester);
        if (choice != null) await journey.finish(choice);
        expect(
          controller.color,
          selectedColor,
          reason: 'Authoritative State reconciles the acknowledged selection',
        );
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: 'A supported Store Tool selection cannot disconnect',
        );
        final tools = ble.writes.skip(writesBefore).where((write) {
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return packet?.kind == ConnectionSessionKind.ordered &&
              packet?.code == ConnectionSession.litchcraftToolOpcode;
        });
        expect(
          tools,
          isNotEmpty,
          reason: 'Every live Store color choice must reach the board',
        );
        expect(
          tools.every((write) => (write['bytes'] as List).length <= 20),
          isTrue,
          reason: 'Tool uses the minimum-MTU priority lane',
        );
        await journey.hall(BigInt.one);
        final physical = await journey.frame();
        debugPrint(
          'TOOL_TRACE choice=$selection selected=$selectedColor '
          'app=${controller.canvas.colors.take(4).toList()} '
          'board=${(physical['board'] as List)[0]} '
          'occupancy=${physical['occupancy']} status=${controller.status} '
          'tool_packets=${tools.map((w) => w['bytes']).toList()}',
        );
        await journey.until(
          () => controller.canvas.colors[0] == selectedColor,
          budgetMs: 500,
          reason: 'A new physical brush must use the app-selected color',
        );
        final painted = await journey.frame();
        expect(
          (painted['board'] as List)[0],
          everyElement([
            (selectedColor >> 16) & 255,
            (selectedColor >> 8) & 255,
            selectedColor & 255,
            0,
          ]),
        );
        journey.checkpoint(
          'Store-$selection-physical-brush',
          painted,
          'mtu=23 rgb=${selectedColor.toRadixString(16)}',
        );
        await journey.hall(BigInt.zero);
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(library.flush());
        library.dispose();
        controller.dispose();
        disposed = true;
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: unexpected known Store resident preserves the accepted artwork choice',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      addTearDown(() => ble.disposeJourney(tester, service));
      final first = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      )..addFrame();
      first.paintSelectedFrame(squares: [0], mask: 15, color: 0x123456);
      final firstDocument = first.document;
      final alternate = LitchcraftDocumentSession(first.document)
        ..paintSelectedFrame(squares: [1], mask: 15, color: 0xabcdef);
      final controller = LitchcraftController(service, documentSession: first);
      var controllerDisposed = false;
      addTearDown(() {
        if (!controllerDisposed) controller.dispose();
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      await journey.finish(controller.replaceDocument(firstDocument));
      await journey.finish(controller.replaceDocument(alternate.document));
      await journey.finish(controller.replaceDocument(firstDocument));
      final opening = service.openConnectionSession(_boardId);
      await journey.finish(opening);
      final session = (await opening)!;
      final client = createConnectionLitchcraftFrameStoreClient(
        session: session,
        monotonicMilliseconds: () => journey.now,
      );
      for (var direction = 0; direction < 2; direction++) {
        final accepted = direction == 0 ? firstDocument : alternate.document;
        if (direction != 0) {
          await journey.finish(controller.replaceDocument(accepted));
        }
        final listingRequest = client.residents(
          session: controller.sessionId,
          request: 60000 + direction * 2,
        );
        await journey.finish(listingRequest);
        final listing = await listingRequest;
        final other = listing.residents.singleWhere(
          (resident) => resident.rootSlot != listing.selectedRootSlot,
        );
        final start = ble.writes.length;
        await journey.finish(
          client.selectResident(
            session: controller.sessionId,
            request: 60001 + direction * 2,
            resident: other,
          ),
        );
        await journey.until(
          () => controller.status == LitchcraftStatus.conflict,
          budgetMs: 6000,
          reason:
              'A different known artwork requires the existing source choice',
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, accepted),
          isTrue,
        );
        expect(controller.animationPlaying, isFalse);
        expect(controller.frameStoreArtworkReadPending, isTrue);
        expect(
          _litchcraftRequests(ble, start).where(
            (packet) =>
                packet.length > 2 &&
                packet[2] == LitchcraftV2Protocol.frameStoreSelect,
          ),
          hasLength(1),
        );
        journey.checkpoint(
          'known-resident-source-choice',
          await journey.frame(),
          'direction=$direction local-artwork=retained playing=false',
        );
        if (direction == 1) {
          final beforeMark = jsonEncode((await journey.frame())['board']);
          await journey.hall(BigInt.one << 4);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 6400);
          expect(
            jsonEncode((await journey.frame())['board']),
            isNot(beforeMark),
            reason:
                'The real Hall mark changes the unrelated root rendered on the board',
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              accepted,
            ),
            isTrue,
            reason:
                'Painting the unrelated board root cannot edit the retained app artwork while choosing',
          );
          expect(controller.status, LitchcraftStatus.conflict);
        }
        await journey.finish(controller.resolveConflict(useBoard: false));
        expect(controller.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, accepted),
          isTrue,
        );
        await _expectCleanStoreParity(journey, controller);
      }
      controller.dispose();
      controllerDisposed = true;
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: minimum-MTU known Undo and Redo preserve the next Hall paint',
    (tester) async {
      for (final ordinaryAppPaint in [false, true]) {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(
          native,
          negotiatedMtu: 23,
          operationIntervalMs: 20,
        );
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final document = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        )..addFrame();
        final seed = LitchcraftController(service, documentSession: document);
        addTearDown(() => ble.disposeJourney(tester, service));
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(seed.enter());
        await journey.finish(
          seed.transferFrameStoreForDiagnostics(
            frames: document.document.frames,
            playbackOnEntry: false,
          ),
        );
        seed.dispose();
        await ble.flush(tester);
        await journey.finish(service.disconnect());
        await ble.flush(tester);
        await journey.finish(service.connect(_boardId));
        final controller = LitchcraftController(
          service,
          documentSession: LitchcraftDocumentSession(
            document.document,
            localIntent: false,
          ),
        );
        addTearDown(controller.dispose);
        await journey.finish(controller.enter());
        await journey.finish(controller.setBrush(color: 0x123456));
        await ble.advance(tester, 300);
        controller.beginStroke();
        controller.paintSquares([0]);
        controller.endStroke();
        await tester.pump();
        await journey.until(() => !controller.pending, budgetMs: 1500);
        expect(controller.documentSession.canUndo, isTrue);
        final undo = controller.undo();
        expect(controller.canvas.colors.take(4), everyElement(0));
        await journey.finish(undo);
        expect(controller.documentSession.canRedo, isTrue);
        expect((await journey.frame())['board'][0], everyElement([0, 0, 0, 0]));
        final redo = controller.redo();
        expect(controller.canvas.colors.take(4), everyElement(0x123456));
        await journey.finish(redo);
        expect(
          (await journey.frame())['board'][0],
          everyElement([0x12, 0x34, 0x56, 0]),
        );
        if (ordinaryAppPaint) {
          await journey.finish(controller.setBrush(color: 0xabcdef));
          controller.beginStroke();
          controller.paintSquares([0]);
          controller.endStroke();
          await tester.pump();
          await journey.until(() => !controller.pending, budgetMs: 1500);
          expect(
            (await journey.frame())['board'][0],
            everyElement([0xab, 0xcd, 0xef, 0]),
          );
          await journey.finish(controller.setBrush(color: 0x123456));
        } else {
          await journey.finish(controller.setBrush(eraser: true));
        }
        final physicalColor = ordinaryAppPaint ? 0x123456 : 0;
        await journey.hall(BigInt.one);
        await journey.until(
          () => controller.canvas.colors[0] == physicalColor,
          budgetMs: 500,
          reason: ordinaryAppPaint
              ? 'After ordinary app paint, a new Hall mark reusing the old history color must project'
              : 'Fresh Hall paint matching the predecessor inverse is not an echo',
        );
        expect(
          (await journey.frame())['board'][0],
          everyElement([
            physicalColor >> 16 & 255,
            physicalColor >> 8 & 255,
            physicalColor & 255,
            0,
          ]),
        );
        expect(controller.documentSession.canUndo, isFalse);
        expect(controller.documentSession.canRedo, isFalse);
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 400);
        expect(controller.canUndo, isTrue);
        journey.checkpoint(
          'minimum-MTU-history-next-Hall',
          await journey.frame(),
          'mtu=23 known-undo-redo=immediate ordinary-app-paint=$ordinaryAppPaint',
        );
        await journey.finish(service.disconnect());
      }
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final inverseDelivery in <String>[
    'before preview',
    'split Undo receipt',
    'during queued admission',
    'before Redo receipt',
    'after Redo receipt',
  ]) {
    testWidgets(
      'Litchcraft: RR long-stroke Undo receipt before inverse objects preserves first Redo '
      '$inverseDelivery',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchBle(native, operationIntervalMs: 20);
        final service = _BridgeBoard(ble);
        final journey = _Journey(tester, ble, service);
        final probe = _StoreHistoryProbe();
        final controller = LitchcraftController(service, appTrace: probe);
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await journey.finish(library.loadRickroll());
        await journey.finish(controller.toggleAnimationPlayback());
        await journey.finish(controller.selectAnimationFrame(5));
        await _expectCleanStoreParity(journey, controller);
        expect(controller.animationPlaying, isFalse);
        final before = controller.documentSession.document;
        expect(before.frames, hasLength(32));
        expect(before.frames[5].durationMs, 50);
        expect(
          before.frames[5].emitters.where(
            (emitter) => emitter.effect == LitchcraftEffectStyle.breathe,
          ),
          isNotEmpty,
        );
        expect(
          before.frames[5].emitters
              .where(
                (emitter) => emitter.effect == LitchcraftEffectStyle.breathe,
              )
              .map((emitter) => emitter.effectDurationMs),
          everyElement(1600),
        );
        await journey.finish(controller.setBrush(color: 0xff7430));
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        final start = journey.now;
        final writesBeforeStroke = ble.writes.length;
        // The compact Fill no longer creates a draining square stream. Keep
        // the escaped history fault on an actual mounted ordinary drag, whose
        // dense inverse still reaches the production reverse-buffer limit.
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        Offset at(int column, int row) =>
            rect.topLeft +
            Offset(
              rect.width * (column + .5) / 8,
              rect.height * (row + .5) / 8,
            );
        final pointer = await tester.startGesture(at(0, 0));
        for (var row = 0; row < 8; row++) {
          final first = row.isEven ? 0 : 7;
          final last = row.isEven ? 7 : 0;
          if (row != 0) await pointer.moveTo(at(first, row));
          await pointer.moveTo(at(last, row));
          await tester.pump();
        }
        await pointer.up();
        await tester.pump();
        final painted = controller.documentSession.document;
        expect(
          painted.frames[5].emitters.map((emitter) => emitter.color),
          everyElement(0xff7430),
        );
        // Keep the optimistic dense-stroke -> Undo overlap. Stop after the real
        // 23rd two-square Paint write, while the 64-square gesture still drains.
        int paintWrites() => ble.writes.skip(writesBeforeStroke).where((write) {
          if (write['channel'] != 'connection_session') return false;
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return packet?.kind == ConnectionSessionKind.ordered &&
              packet?.code == ConnectionSession.litchcraftPaintOpcode &&
              packet!.body.length > 8;
        }).length;
        await journey.until(() => paintWrites() == 23, budgetMs: 4000);
        expect(controller.pending, isTrue);
        debugPrint(
          'RR_HISTORY overlap paint_writes=${paintWrites()} '
          'elapsed_ms=${journey.now - start}',
        );
        ble.holdPhysicalStoreObjects = true;
        if (inverseDelivery == 'during queued admission') {
          // Supporting volatile-State-loss schedule only. The observed human
          // trigger above delays authentic inverse Objects without losing State.
          ble.holdConnectionObservations = true;
        }
        await tester.tap(find.byKey(const ValueKey('craft-undo')));
        expect(
          sameLitchcraftDocument(controller.documentSession.document, before),
          isTrue,
          reason: 'Known Undo previews the complete RR frame immediately',
        );
        if (inverseDelivery == 'split Undo receipt') {
          await journey.until(
            () => probe.events.any(
              (event) =>
                  event.event == 'litchcraft.store_history_admission' &&
                  event.fields['opcode'] == LitchcraftProtocol.undo,
            ),
            budgetMs: 4000,
          );
          ble.heldChannels.add('connection_session');
          await journey.until(
            () =>
                ble.held.where((packet) {
                  final decoded = ConnectionSessionCodec.decode(
                    Uint8List.fromList(packet.bytes),
                  );
                  return decoded?.kind == ConnectionSessionKind.object &&
                      decoded?.code == LitchcraftV2Protocol.physicalStoreBatch;
                }).length ==
                2,
            budgetMs: 1500,
          );
          final firstInverse = ble.held.firstWhere((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.object &&
                decoded?.code == LitchcraftV2Protocol.physicalStoreBatch;
          });
          ble.held.remove(firstInverse);
          ble.deliver(firstInverse);
          await tester.pump();
          final receiptPackets = ble.held.where((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind != ConnectionSessionKind.object;
          }).toList();
          for (final packet in receiptPackets) {
            ble.held.remove(packet);
            ble.deliver(packet);
          }
          ble.heldChannels.remove('connection_session');
          await tester.pump();
        }
        await journey.until(
          () => probe.events.any(
            (event) =>
                event.event == 'litchcraft.store_history_receipt' &&
                event.fields['opcode'] == LitchcraftProtocol.undo &&
                event.fields['history_applied'] == true,
          ),
          budgetMs: 4000,
        );
        await journey.until(() => !controller.pending, budgetMs: 500);
        final heldObjects = ble.held.where((packet) {
          final decoded = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return decoded?.kind == ConnectionSessionKind.object &&
              decoded?.code == LitchcraftV2Protocol.physicalStoreBatch;
        }).toList();
        expect(heldObjects, isNotEmpty);
        expect(controller.documentSession.canRedo, isTrue);
        expect(controller.canRedo, isTrue);
        debugPrint(
          'RR_HISTORY receipt_before_objects held=${heldObjects.length} '
          'elapsed_ms=${journey.now - start}',
        );
        Future<void>? selection;
        if (inverseDelivery == 'before preview' ||
            inverseDelivery == 'split Undo receipt') {
          ble.holdPhysicalStoreObjects = false;
          ble.release('connection_session');
          await tester.pump();
        } else if (inverseDelivery == 'during queued admission') {
          selection = controller.selectAnimationFrame(5);
        }
        final redo = find.byKey(const ValueKey('craft-redo'));
        expect(tester.widget<IconButton>(redo).onPressed, isNotNull);
        await tester.tap(redo);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, painted),
          isTrue,
          reason: 'The first mounted Redo previews the exact long stroke',
        );
        if (inverseDelivery == 'during queued admission') {
          final lostHints = ble.held.where((packet) {
            final decoded = ConnectionSessionCodec.decode(
              Uint8List.fromList(packet.bytes),
            );
            return decoded?.kind == ConnectionSessionKind.state &&
                decoded?.code == ConnectionSession.litchcraftStoreEditStateCode;
          }).toList();
          expect(lostHints, isNotEmpty);
          for (final packet in lostHints) {
            ble.held.remove(packet);
          }
          debugPrint(
            'RR_HISTORY supporting_volatile_hints_lost=${lostHints.length}',
          );
          ble.holdConnectionObservations = false;
        } else if (inverseDelivery == 'before Redo receipt') {
          // Hold this real Ordered receipt while the earlier inverse Objects
          // reach the unconfirmed Redo's bounded predecessor proof. Credits and
          // outcomes are delayed unchanged at the same existing SDK boundary.
          ble.heldChannels.add('connection_session');
          await journey.until(
            () => ble.writes.skip(writesBeforeStroke).any((write) {
              if (write['channel'] != 'connection_session') return false;
              final packet = ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              );
              return packet?.kind == ConnectionSessionKind.ordered &&
                  packet?.code == ConnectionSession.litchcraftRedoOpcode;
            }),
            budgetMs: 1500,
          );
          expect(
            probe.events.where(
              (event) =>
                  event.event == 'litchcraft.store_history_receipt' &&
                  event.fields['opcode'] == LitchcraftProtocol.redo,
            ),
            isEmpty,
          );
          for (final packet in heldObjects) {
            ble.held.remove(packet);
            ble.deliver(packet);
          }
          await tester.pump();
          ble.heldChannels.remove('connection_session');
        } else if (inverseDelivery == 'after Redo receipt') {
          await journey.until(
            () => probe.events.any(
              (event) =>
                  event.event == 'litchcraft.store_history_receipt' &&
                  event.fields['opcode'] == LitchcraftProtocol.redo,
            ),
            budgetMs: 1500,
          );
        }
        if (inverseDelivery != 'before preview') {
          ble.holdPhysicalStoreObjects = false;
          ble.release('connection_session');
          await tester.pump();
          if (selection != null) await journey.finish(selection);
        }
        await journey.until(() => !controller.pending, budgetMs: 1500);
        expect(
          probe.events.where(
            (event) =>
                event.event == 'litchcraft.store_history_requested' &&
                event.fields['opcode'] == LitchcraftProtocol.redo,
          ),
          hasLength(1),
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, painted),
          isTrue,
          reason: 'The first Redo must remain applied after admission',
        );
        expect(
          probe.events.any(
            (event) =>
                event.event == 'litchcraft.store_history_receipt' &&
                event.fields['opcode'] == LitchcraftProtocol.redo &&
                event.fields['history_applied'] == true,
          ),
          isTrue,
        );
        final admission = probe.events.lastWhere(
          (event) =>
              event.event == 'litchcraft.store_history_admission' &&
              event.fields['opcode'] == LitchcraftProtocol.redo,
        );
        expect(admission.fields['can_guard'], isTrue);
        if (inverseDelivery == 'during queued admission') {
          expect(
            admission.fields['physical_revision'],
            isNot(admission.fields['physical_before']),
            reason:
                'The authentic reliable inverse crosses the captured watermark',
          );
        }
        final requestsBeforeRepeat = probe.events.length;
        await tester.tap(find.byKey(const ValueKey('craft-undo')));
        await journey.until(() => !controller.pending, budgetMs: 1500);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, before),
          isTrue,
          reason: 'Repeated Undo remains ready after late history tails',
        );
        await tester.tap(redo);
        await journey.until(() => !controller.pending, budgetMs: 1500);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, painted),
          isTrue,
          reason: 'Repeated Redo remains ready after late history tails',
        );
        expect(
          probe.events
              .skip(requestsBeforeRepeat)
              .where(
                (event) =>
                    event.event == 'litchcraft.store_history_receipt' &&
                    event.fields['history_applied'] == true,
              ),
          hasLength(2),
        );
        await _expectCleanStoreParity(journey, controller);
        expect(
          _litchcraftRequests(ble, writesBeforeStroke).where(
            (packet) =>
                packet[0] == LitchcraftV2Protocol.magic &&
                (packet[2] == LitchcraftV2Protocol.frameStoreSnapshot ||
                    packet[2] == LitchcraftV2Protocol.frameStoreBegin),
          ),
          isEmpty,
          reason:
              'Known dense history needs no full Store read or body transfer',
        );
        expect(
          _litchcraftRequests(ble, writesBeforeStroke).where(_isLiveLayoutRead),
          isEmpty,
          reason: 'Known dense history must not fall into live-layout repair',
        );
        journey.checkpoint(
          'RR-dense-first-Redo',
          await journey.frame(),
          'frame=5 duration=50 all256-RGB/effect/duration=exact '
              'receipt-before-authentic-inverse-object',
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await journey.finish(library.flush());
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'Litchcraft: Hall Pick Color and physical paint retain shared Undo and Redo',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, operationIntervalMs: 20);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final initial = staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint([0], 15, 0x123456),
      );
      final seed = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(initial)..addFrame(),
      );
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(seed.enter());
      await journey.finish(
        seed.transferFrameStoreForDiagnostics(
          frames: seed.documentSession.document.frames,
          playbackOnEntry: false,
        ),
      );
      final document = seed.documentSession.document;
      seed.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final controller = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          document,
          localIntent: false,
        ),
      );
      final library = LitchcraftLibrary(controller);
      var disposed = false;
      addTearDown(() {
        if (!disposed) {
          library.dispose();
          controller.dispose();
        }
      });
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await journey.finish(controller.setBrush(color: 0xabcdef));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await ble.advance(tester, 300);
      expect(controller.documentSession.canUndo, isFalse);
      await journey.hall(BigInt.one);
      await ble.advance(tester, 2000);
      await journey.until(() => controller.color == 0x123456, budgetMs: 500);
      expect(controller.canvas.colors.take(4), everyElement(0x123456));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 400);
      await journey.hall(BigInt.one << 1);
      await journey.until(
        () => controller.canvas.colors[4] == 0x123456,
        budgetMs: 500,
      );
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 400);
      final historyStates = ble.emitted
          .where((packet) => packet.channel == 'connection_session')
          .map(
            (packet) =>
                ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes)),
          )
          .whereType<ConnectionSessionPacket>()
          .where(
            (packet) =>
                packet.kind == ConnectionSessionKind.state &&
                packet.code == ConnectionSession.litchcraftStateCode,
          );
      expect(
        historyStates.last.body[9] & 1,
        1,
        reason: 'The real Store journal advertises the completed Hall stroke',
      );
      expect(
        controller.documentSession.canUndo,
        isFalse,
        reason: 'A physical journal operation has no phone-local inverse',
      );
      expect(
        controller.canUndo,
        isTrue,
        reason: 'Physical Pick then paint must leave app Undo available',
      );
      final undo = find.byKey(const ValueKey('craft-undo'));
      expect(tester.widget<IconButton>(undo).onPressed, isNotNull);
      final writesBefore = ble.writes.length;
      await tester.tap(undo);
      await journey.until(
        () => controller.canvas.colors[4] == 0,
        budgetMs: 1500,
        reason: 'Unknown physical Undo returns the real inverse',
      );
      expect(controller.canvas.colors.take(4), everyElement(0x123456));
      await journey.until(
        () => controller.canRedo && !controller.pending,
        budgetMs: 500,
      );
      await tester.tap(find.byKey(const ValueKey('craft-redo')));
      await journey.until(
        () => controller.canvas.colors[4] == 0x123456,
        budgetMs: 1500,
      );
      final historyPackets = ble.writes
          .skip(writesBefore)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((packet) => packet.kind == ConnectionSessionKind.ordered);
      expect(
        historyPackets.map((packet) => packet.code),
        containsAllInOrder([
          ConnectionSession.litchcraftStoreGuardOpcode,
          ConnectionSession.litchcraftUndoOpcode,
          ConnectionSession.litchcraftStoreGuardOpcode,
          ConnectionSession.litchcraftRedoOpcode,
        ]),
      );
      await journey.until(() => !controller.pending, budgetMs: 500);
      await journey.finish(controller.selectAnimationFrame(1));
      await journey.finish(controller.undo());
      await journey.until(
        () =>
            controller.animationSelectedFrameIndex == 0 &&
            controller.canvas.colors[4] == 0,
        budgetMs: 1500,
        reason: 'The board journal selects its last edited frame before Undo',
      );
      await journey.finish(controller.selectAnimationFrame(1));
      await journey.finish(controller.redo());
      await journey.until(
        () =>
            controller.animationSelectedFrameIndex == 0 &&
            controller.canvas.colors[4] == 0x123456,
        budgetMs: 1500,
      );
      await journey.finish(controller.setBrush(color: 0x778899));
      controller.beginStroke();
      controller.paintSquares([2]);
      controller.endStroke();
      expect(controller.canvas.colors[8], 0x778899);
      final fastUndo = controller.undo();
      expect(
        controller.canvas.colors[8],
        0,
        reason: 'A locally known app inverse previews before its paint receipt',
      );
      await journey.finish(fastUndo);
      await journey.until(
        () => controller.canRedo && !controller.pending,
        budgetMs: 500,
      );
      expect(
        controller.documentSession.canRedo,
        isTrue,
        reason: 'The real reverse echo must preserve the known Redo inverse',
      );
      await journey.finish(controller.redo());
      await journey.until(
        () => controller.canvas.colors[8] == 0x778899,
        budgetMs: 1500,
      );
      await _expectCleanStoreParity(journey, controller);
      journey.checkpoint(
        'Hall-Pick-paint-shared-history',
        await journey.frame(),
        'Undo/Redo use the guarded board journal; Pick preserves its sample',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(library.flush());
      library.dispose();
      controller.dispose();
      disposed = true;
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final secondPhone = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          controller.documentSession.document,
          localIntent: false,
        ),
      );
      var secondPhoneDisposed = false;
      addTearDown(() {
        if (!secondPhoneDisposed) secondPhone.dispose();
      });
      await journey.finish(secondPhone.enter());
      await journey.until(() => secondPhone.canUndo, budgetMs: 500);
      expect(
        secondPhone.documentSession.canUndo,
        isFalse,
        reason:
            'Reconnect learns board availability without copying its journal',
      );
      await journey.finish(secondPhone.undo());
      await journey.until(
        () => secondPhone.canvas.colors[8] == 0,
        budgetMs: 1500,
      );
      await journey.finish(secondPhone.redo());
      await journey.until(
        () => secondPhone.canvas.colors[8] == 0x778899,
        budgetMs: 1500,
      );
      await _expectCleanStoreParity(journey, secondPhone);
      secondPhone.dispose();
      secondPhoneDisposed = true;
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'Litchcraft: magnet path and edge erase converge during delayed notifications',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native, operationIntervalMs: 20);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final document = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      )..addFrame();
      document.selectFrame(0);
      final seed = LitchcraftController(service, documentSession: document);
      addTearDown(() => ble.disposeJourney(tester, service));
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(seed.enter());
      await journey.finish(
        seed.transferFrameStoreForDiagnostics(
          frames: document.document.frames,
          playbackOnEntry: false,
        ),
      );
      seed.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      await ble.flush(tester);
      await journey.finish(service.connect(_boardId));
      final controller = LitchcraftController(
        service,
        documentSession: LitchcraftDocumentSession(
          document.document,
          localIntent: false,
        ),
      );
      var controllerDisposed = false;
      addTearDown(() {
        if (!controllerDisposed) controller.dispose();
      });
      await journey.finish(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.boardActive, isTrue, reason: controller.errorMessage);
      await ble.advance(
        tester,
        300,
      ); // Finish entry crossfade before optical checks.

      for (final square in [0, 1, 2, 3, 2, 3, 4, 5, 6, 7]) {
        final start = journey.now;
        await journey.hall(BigInt.one << square);
        await journey.until(
          () => controller.canvas.colors[square] != 0,
          budgetMs: 500,
          reason:
              'Magnet square $square did not reach the app canvas within 500 ms; '
              'status=${controller.status} boardActive=${controller.boardActive} '
              'error=${controller.errorMessage}',
        );
        final frame = await journey.frame();
        expect(
          (frame['physical'] as List)[square * 4],
          isNot(equals([0, 0, 0, 0])),
        );
        journey.checkpoint(
          'magnet-square-$square',
          frame,
          'input_to_app_ms=${journey.now - start}',
        );
      }
      final stationaryRevision = (await journey.frame())['litchcraft_revision'];
      await ble.advance(tester, 1000);
      expect(
        (await journey.frame())['litchcraft_revision'],
        stationaryRevision,
        reason: 'A magnet left resting emits no repeated paint mutations',
      );
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 400);
      await journey.finish(controller.setBrush(eraser: true));
      await tester.pumpWidget(
        MaterialApp(
          home: Center(
            child: SizedBox(
              width: 320,
              height: 320,
              child: AnimatedBuilder(
                animation: controller,
                builder: (context, _) => LitchcraftCanvasView(
                  canvas: controller.canvas,
                  onBegin: controller.beginStroke,
                  onPaint: controller.paintSquares,
                  onEnd: controller.endStroke,
                ),
              ),
            ),
          ),
        ),
      );
      final origin = tester.getTopLeft(find.byType(LitchcraftCanvasView));
      final liveRevisionBeforeErase =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']['live_revision']
              as int;
      final start = journey.now;
      final writesBefore = ble.writes.length;
      // Hold authority observations, not the Progress credit needed to carry
      // the rest of the bounded erase stroke to the board.
      ble.holdConnectionObservations = true;
      final pointer = await tester.startGesture(origin + const Offset(20, 20));
      await tester.pump(const Duration(milliseconds: 16));
      await pointer.moveTo(origin + const Offset(300, 20));
      await tester.pump(const Duration(milliseconds: 16));
      await pointer.moveTo(origin + const Offset(20, 20));
      await pointer.up();
      await tester.pump();
      final localMs = journey.now - start;
      expect(controller.canvas.colors.take(8), everyElement(0));
      expect(localMs, lessThan(250));
      final dispatchMs = await journey.until(
        () => ble.writes.length > writesBefore,
      );
      final paintedFrame = await journey.untilFrame((frame) {
        final board = frame['board'] as List;
        return List.generate(8, (square) => square).every(
          (square) => (board[square] as List).every(
            (slot) => (slot as List).every((channel) => channel == 0),
          ),
        );
      });
      final boardMs = journey.now - start;
      for (var square = 0; square < 8; square++) {
        expect(
          (paintedFrame['board'] as List)[square],
          everyElement([0, 0, 0, 0]),
          reason: 'Board renders erase before delayed notifications arrive',
        );
      }
      expect(boardMs, lessThan(250));
      await ble.advance(tester, 100);
      final held = List<VirtualPacket>.of(ble.held);
      expect(
        held,
        isNotEmpty,
        reason:
            'Actual production response is delayed; writes=${ble.writes.skip(writesBefore).map((write) => write['channel']).toList()} emitted=${ble.emitted.map((packet) => packet.channel).toList()}',
      );
      expect(
        controller.canvas.colors.take(8),
        everyElement(0),
        reason: 'Local erase remains visible while acknowledgement is delayed',
      );
      // Corrupt a real notification at the SDK boundary; no fake ACK or state.
      final malformed = held.first;
      malformed.pipe?.deliver(malformed.bytes.take(1).toList());
      await tester.pump();
      expect(controller.canvas.colors.take(8), everyElement(0));
      ble.holdConnectionObservations = false;
      ble.release('connection_session');
      for (
        var elapsed = 0;
        controller.pending && elapsed < 1000;
        elapsed += 10
      ) {
        await journey.tick();
      }
      expect(
        controller.pending,
        isFalse,
        reason:
            'Released session did not settle; status=${controller.status} error=${controller.errorMessage} pending_ops=${ble.hasPendingOperations} writes=${ble.writes.skip(writesBefore).map((write) {
              final bytes = write['bytes'] as List<int>;
              final packet = ConnectionSessionCodec.decode(Uint8List.fromList(bytes));
              return '${write['channel']}:${packet?.kind.name}/${packet?.code}/${packet?.sequence}';
            }).toList()} held=${ble.held.map((packet) => packet.channel).toList()} emitted=${ble.emitted.where((packet) => packet.channel == 'connection_session').map((packet) {
              final decoded = ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes));
              return '${decoded?.code}/${decoded?.sequence}';
            }).toList()}',
      );
      final frame = await journey.frame();
      final liveRevisionAfterErase =
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': 0,
              }))['store_frame']['live_revision']
              as int;
      expect(
        liveRevisionAfterErase - liveRevisionBeforeErase,
        8,
        reason:
            'Each of the eight crossed squares keeps its own Store edit revision even when two share one ordered packet',
      );
      for (var square = 0; square < 8; square++) {
        expect((frame['board'] as List)[square], everyElement([0, 0, 0, 0]));
        expect(
          (frame['physical'] as List).sublist(square * 4, square * 4 + 4),
          everyElement([0, 0, 0, 0]),
        );
      }
      final editWrites = ble.writes.skip(writesBefore).where((write) {
        if (write['channel'] != 'connection_session') return false;
        final packet = ConnectionSessionCodec.decode(
          Uint8List.fromList(write['bytes'] as List<int>),
        );
        return packet?.kind == ConnectionSessionKind.ordered;
      }).toList();
      expect(
        editWrites.length,
        lessThanOrEqualTo(12),
        reason:
            'A repeated edge path is bounded by changed squares, not pointer samples; writes=${editWrites.map((write) {
              final packet = ConnectionSessionCodec.decode(Uint8List.fromList(write['bytes'] as List<int>));
              return '${packet?.kind.name}/${packet?.code}/${packet?.sequence}';
            }).toList()}',
      );
      journey.checkpoint(
        'edge-erase',
        frame,
        'input_to_local_ms=$localMs '
            'input_to_first_dispatch_upper_ms=${localMs + dispatchMs} '
            'input_to_board_ms=$boardMs '
            'final_convergence_ms=${journey.now - start}',
      );
      expect(journey.now - start, lessThan(1000));

      await journey.finish(controller.setBrush(color: 0x336699, eraser: false));
      await journey.finish(controller.fillAll());
      expect(controller.canvas.colors, everyElement(0x336699));
      final historyWrites = ble.writes.length;
      await journey.finish(controller.undo());
      expect(controller.canvas.colors, everyElement(0));
      await journey.untilFrame((frame) {
        final board = frame['board'] as List;
        return board.every(
          (square) => (square as List).every(
            (slot) => (slot as List).every((channel) => channel == 0),
          ),
        );
      });
      await journey.finish(controller.redo());
      expect(controller.canvas.colors, everyElement(0x336699));
      final historyPackets = ble.writes
          .skip(historyWrites)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((packet) => packet.kind == ConnectionSessionKind.ordered)
          .toList();
      expect(
        historyPackets.map((packet) => packet.code),
        containsAllInOrder([
          ConnectionSession.litchcraftUndoOpcode,
          ConnectionSession.litchcraftRedoOpcode,
        ]),
        reason: 'known history leaves the per-feature V2 request scheduler',
      );

      final timelineWrites = ble.writes.length;
      await journey.finish(controller.selectAnimationFrame(1));
      await journey.until(() => controller.animationSelectedFrameIndex == 1);
      expect(controller.animationPlaying, isFalse);
      await journey.finish(controller.toggleAnimationPlayback());
      await journey.until(() => controller.animationPlaying);
      await ble.advance(tester, 75);
      final playingElapsed = controller.animationElapsedMilliseconds;
      expect(playingElapsed, greaterThan(0));
      await journey.finish(controller.toggleAnimationPlayback());
      await journey.until(() => !controller.animationPlaying);
      expect(
        controller.animationElapsedMilliseconds,
        greaterThanOrEqualTo(playingElapsed),
        reason: 'pause keeps the board-owned frame-local playback anchor',
      );
      final timelinePackets = ble.writes
          .skip(timelineWrites)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((packet) => packet.kind == ConnectionSessionKind.ordered)
          .toList();
      expect(
        timelinePackets.map((packet) => packet.code),
        containsAllInOrder([
          ConnectionSession.litchcraftTimelineOpcode,
          ConnectionSession.litchcraftTimelineOpcode,
          ConnectionSession.litchcraftTimelineOpcode,
        ]),
        reason: 'select, play and pause share the generic ordered owner',
      );

      final storeWrites = ble.writes.length;
      final stored = controller.transferFrameStoreForDiagnostics(
        frames: document.document.frames,
        playbackOnEntry: true,
      );
      await journey.finish(stored, budgetMs: 10000);
      final objectPackets = ble.writes
          .skip(storeWrites)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((packet) => packet.kind == ConnectionSessionKind.object)
          .toList();
      final storeAssembler = ConnectionLitchcraftAssembler();
      final storeOperations = <int>[];
      for (final packet in objectPackets) {
        expect(packet.code, ConnectionLitchcraftCodec.objectOpcode);
        final storePacket = storeAssembler.accept(packet.body);
        if (storePacket != null) storeOperations.add(storePacket[2]);
      }
      expect(
        storeOperations,
        containsAllInOrder([18, 19, 21]),
        reason:
            'Begin, compressed chunks and Commit share the negotiated Object stream',
      );
      expect(
        ble.writes
            .skip(storeWrites)
            .where((write) => write['channel'] == 'artwork_data'),
        isEmpty,
        reason: 'schema-4 Store no longer opens a second bulk characteristic',
      );
      final acceptedObjects = objectPackets.length;
      await journey.finish(controller.selectAnimationFrame(0));
      if (controller.animationPlaying) {
        await journey.finish(controller.toggleAnimationPlayback());
      }
      await journey.finish(controller.toggleAnimationPlayback());
      await journey.finish(controller.toggleAnimationPlayback());
      final objectsAfterRuntime = ble.writes
          .skip(storeWrites)
          .where((write) => write['channel'] == 'connection_session')
          .map(
            (write) => ConnectionSessionCodec.decode(
              Uint8List.fromList(write['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .where((packet) => packet.kind == ConnectionSessionKind.object)
          .toList();
      final postRuntimeAssembler = ConnectionLitchcraftAssembler();
      final postRuntimeOperations = <int>[];
      for (final packet in objectsAfterRuntime) {
        final storePacket = postRuntimeAssembler.accept(packet.body);
        if (storePacket != null) postRuntimeOperations.add(storePacket[2]);
      }
      bool isStoreTransfer(int operation) =>
          operation == 18 || operation == 19 || operation == 21;
      expect(
        postRuntimeOperations.where(isStoreTransfer).toList(),
        storeOperations.where(isStoreTransfer).toList(),
        reason:
            'select/play/pause reuse the committed generation without a second document transfer; operations=$postRuntimeOperations',
      );
      journey.checkpoint(
        'store-object-then-runtime',
        await journey.frame(),
        'objects=$acceptedObjects replay_objects=0',
      );

      await journey.finish(controller.leave());
      for (final packet in held) {
        ble.deliver(packet, lateCallback: true);
      }
      await tester.pump();
      expect(
        controller.boardActive,
        isFalse,
        reason: 'Old notifications cannot resurrect an exited canvas',
      );
      await journey.finish(controller.enter());
      expect(controller.boardActive, isTrue);
      expect(
        controller.canvas.colors,
        everyElement(0x336699),
        reason: 'the redone board authority survives exit and re-entry',
      );
      expect(controller.errorMessage, isNull);
      controller.dispose();
      controllerDisposed = true;
      await tester.pumpWidget(const SizedBox.shrink());
      await journey.finish(service.disconnect());
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );
  testWidgets(
    'native malformed Store read exhausts repairs and Connect restores artwork',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _MalformedStoreBodyBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final trace = _MalformedBodyTrace(() => journey.now);
      final controller = LitchcraftController(service, appTrace: trace);
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(controller.enter());
      final blank = staticLitchcraftDocument(LitchcraftCanvas.blank());
      final original = LitchcraftAnimationDocument(
        frames: [
          LitchcraftAnimationFrame(
            durationMs: 120,
            emitters: List.generate(
              256,
              (i) => LitchcraftAnimatedEmitter(
                color: 0x123456 + i,
                effect: LitchcraftEffectStyle.breathe,
                effectDurationMs: 1600,
              ),
            ),
          ),
          ...LitchcraftBouncingBallsPreset.decode().frames.skip(1),
        ],
        topology: blank.topology,
        favoriteColors: blank.favoriteColors,
        playbackOnEntry: false,
      );
      await journey.finish(controller.replaceDocument(original));
      await journey.finish(library.ready);
      await _expectCleanStoreParity(journey, controller);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      late final ConnectionSession old;
      await journey.finish(
        service.openConnectionSession(_boardId).then((value) {
          old = value!;
        }),
      );
      expect(old.isActivated, isTrue);
      await journey.finish(
        controller.setBrush(color: 0x55aa77, targetMask: 15),
      );
      ble.corruptStoreBody = true;
      await journey.hall((BigInt.one << 64) - BigInt.one);
      await ble.advance(tester, 400);
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 6400);
      await journey.until(
        () => ble.corruptedCopies.isNotEmpty,
        budgetMs: 20000,
        reason: 'actual dense Hall overflow must read full native Store',
      );
      final start = journey.now;
      while (journey.now - start < 30000 &&
          !controller.connectionRecoveryRequired) {
        await journey.tick();
      }
      final exhausted = controller.connectionRecoveryRequired;
      final recovery = trace.events
          .where((e) => e['event'] == 'litchcraft.recovery_scheduled')
          .toList();
      expect(ble.corruptedCopies, isNotEmpty);
      for (final entry in ble.corruptedCopies) {
        final before = entry['original'] as List<int>,
            after = entry['copy'] as List<int>;
        expect(after.length, before.length);
        expect(
          [
            for (var i = 0; i < before.length; i++)
              if (before[i] != after[i]) i,
          ],
          [entry['offset']],
        );
        final a = ConnectionSessionCodec.decode(Uint8List.fromList(before))!;
        final b = ConnectionSessionCodec.decode(Uint8List.fromList(after))!;
        expect(b.token, a.token);
        expect(b.sequence, a.sequence);
        expect(b.kind, a.kind);
        expect(b.code, a.code);
        final bodyStart = 4 + 13 + (entry['headerBytes'] as int);
        expect(
          b.body.sublist(0, bodyStart),
          a.body.sublist(0, bodyStart),
          reason: 'Manifest, generation, live revision and epoch stay exact',
        );
        final originalBody = Uint8List(entry['bodyBytes'] as int);
        var covered = 0;
        for (final part in ble.originalBodyChunks[entry['bodyKey']]!.entries) {
          originalBody.setRange(
            part.key,
            part.key + part.value.length,
            part.value,
          );
          covered += part.value.length;
        }
        expect(covered, originalBody.length);
        expect(litchcraftAnimationCrc32(originalBody), entry['bodyCrc32']);
        final corruptedBody = Uint8List.fromList(originalBody)..[2] ^= 1;
        expect(
          litchcraftAnimationCrc32(corruptedBody),
          isNot(entry['bodyCrc32']),
          reason: 'The controlled failure is body integrity, not authority',
        );
      }
      // Recover through the mounted action after all automatic attempts stop.
      // The fault is at SDK delivery; native authority and artwork stay intact.
      final copiesAtExhaustion = ble.corruptedCopies.length;
      if (exhausted) {
        final stoppedAt = journey.now;
        while (journey.now - stoppedAt < 2000) {
          await journey.tick();
        }
        expect(
          ble.corruptedCopies.length,
          copiesAtExhaustion,
          reason: 'automatic read attempts must stop after bounded exhaustion',
        );
        expect(
          old.isActivated,
          isTrue,
          reason: 'domain read exhaustion must preserve the healthy BLE owner',
        );
        final retry = find.byKey(const ValueKey('craft-reconnect'));
        expect(
          retry,
          findsOneWidget,
          reason: 'the existing Connect canvas action must be available',
        );
        expect(tester.widget<IconButton>(retry).onPressed, isNotNull);
        ble.corruptStoreBody = false;
        await tester.ensureVisible(retry);
        await tester.tap(retry);
        await tester.pump();
      } else {
        ble.corruptStoreBody = false;
      }
      await journey.until(
        () =>
            controller.status == LitchcraftStatus.live &&
            !controller.pending &&
            !controller.boardArtworkUnresolved &&
            controller.documentSession.document.frames[0].emitters.every(
              (e) => e.color == 0x55aa77,
            ),
        budgetMs: 20000,
        reason:
            'uncorrupted read must adopt actual physical paint and retain whole document',
      );
      await _expectCleanStoreParity(journey, controller);
      expect(await service.openConnectionSession(_boardId), same(old));
      expect(service.isConnected, isTrue);
      expect(controller.documentSession.document.frames, hasLength(256));
      expect(
        controller.documentSession.document.frames[0].emitters.every(
          (e) => e.color == 0x55aa77,
        ),
        isTrue,
      );
      expect(
        controller.documentSession.document.frames[0].emitters.every(
          (e) =>
              e.effect == LitchcraftEffectStyle.breathe &&
              e.effectDurationMs == 1600,
        ),
        isTrue,
      );
      debugPrint(
        'MALFORMED_STORE_READ recovered through mounted Connect with full256 RGB/effect parity',
      );
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
      await ble.flush(tester);
      await journey.finish(service.disconnect());
      expect(
        exhausted,
        isTrue,
        reason:
            'Persistent malformed Store read on connected BLE must exhaust four silent repairs and expose Connect canvas, rather than remain silently Live and retry forever',
      );
      expect(recovery.map((e) => e['attempt']).toList(), [1, 2, 3, 4]);
    },
    variant: TargetPlatformVariant(<TargetPlatform>{
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'live Store physical X mounted Syncing cue completes and cancels',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      try {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedLiveLayoutBle(native);
        final service = _BridgeBoard(ble, motionCapable: true);
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(service);
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(controller.enter());
        final metadata = controller.documentSession.document;
        final dense = LitchcraftAnimationFrame(
          durationMs: 2000,
          emitters: List.generate(
            256,
            (i) => LitchcraftAnimatedEmitter(
              color: 0x123400 + i,
              effect: LitchcraftEffectStyle.breathe,
              effectDurationMs: 1600,
            ),
          ),
        );
        final document = LitchcraftAnimationDocument(
          frames: List.filled(256, dense),
          topology: metadata.topology,
          favoriteColors: metadata.favoriteColors,
          sparkleSeed: metadata.sparkleSeed,
          playbackOnEntry: false,
        );
        await journey.finish(
          controller.replaceDocument(document),
          budgetMs: 60000,
        );
        await journey.finish(controller.selectAnimationFrame(250));
        await journey.finish(library.ready);
        await ble.advance(tester, 6000);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 300));
        for (final cancel in [false, true]) {
          await journey.finish(controller.eraseDisplayedFrameByPhone());
          await ble.advance(tester, 1100);
          int? readbackFrameAt;
          void recordReadbackFrame() {
            if (!controller.artworkReadbackPending || readbackFrameAt != null) {
              return;
            }
            WidgetsBinding.instance.addPostFrameCallback((_) {
              readbackFrameAt ??= journey.now;
            });
          }

          expect(controller.artworkReadbackPending, isFalse);
          controller.addListener(recordReadbackFrame);
          addTearDown(() => controller.removeListener(recordReadbackFrame));
          ble.holdLiveLayouts = true;
          ble.heldLiveLayouts.clear();
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 5,
            'down': true,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 5,
            'down': false,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          await journey.until(
            () => ble.heldLiveLayouts.isNotEmpty,
            budgetMs: 1000,
            reason: 'Physical X must reach the real LIVE layout read',
          );
          await tester.pump();
          controller.removeListener(recordReadbackFrame);
          expect(readbackFrameAt, isNotNull);
          // Anchor presentation time to the first mounted pending frame; the
          // real BLE/native journey can advance time before its layout is held.
          final readbackStartedAt = readbackFrameAt!;
          final beforeThreshold = readbackStartedAt + 499 - journey.now;
          expect(beforeThreshold, greaterThanOrEqualTo(0));
          await tester.pump(Duration(milliseconds: beforeThreshold));
          expect(find.text('Syncing…'), findsNothing);
          expect(
            find.byKey(const ValueKey('craft-canvas-toast-position')),
            findsNothing,
          );
          await tester.pump(const Duration(milliseconds: 1));
          expect(controller.pending, isTrue);
          expect(
            find.text('Syncing…'),
            findsOneWidget,
            reason: 'Board X shares the mounted history recovery cue',
          );
          expect(
            tester.getSemantics(find.text('Syncing…')).label,
            contains('Syncing'),
          );
          await tester.pump(const Duration(milliseconds: 180));
          if (cancel) {
            await journey.finish(service.disconnect());
            await tester.pump();
            expect(controller.historyRecoveryPending, isFalse);
            expect(find.text('Syncing…'), findsNothing);
          }
          final documentBeforeRelease = controller.documentSession.document;
          final statusBeforeRelease = controller.status;
          ble.holdLiveLayouts = false;
          for (final packet in ble.heldLiveLayouts) {
            ble.deliver(packet, lateCallback: cancel);
          }
          if (!cancel) {
            await journey.until(
              () =>
                  !controller.pending &&
                  sameLitchcraftDocument(
                    controller.documentSession.document,
                    document,
                  ),
              budgetMs: 2000,
            );
            expect(controller.historyRecoveryPending, isFalse);
            expect(
              find.text('Syncing…'),
              findsNothing,
              reason:
                  'The complete native read retires its activity in the same frame',
            );
            expect(
              find.byKey(const ValueKey('craft-canvas-toast-position')),
              findsNothing,
            );
          } else {
            await tester.pump();
            expect(service.isConnected, isFalse);
            expect(controller.status, statusBeforeRelease);
            expect(
              identical(
                controller.documentSession.document,
                documentBeforeRelease,
              ),
              isTrue,
              reason: 'Late old-session packets cannot adopt artwork',
            );
            expect(
              find.text('Syncing…'),
              findsNothing,
              reason: 'Late old-session packets cannot restore a pending cue',
            );
          }
        }
      } finally {
        semantics.dispose();
      }
    },
    variant: TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final origin in ['held Fill', 'phone shake', 'hardware shake']) {
    for (final history in ['app Undo', 'board X']) {
      testWidgets(
        'live Store dense256 clear origin=$origin history=$history',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final native = (await tester.runAsync(
            () => VirtualLitchProcess.start(1, false, hardwareV3: true),
          ))!;
          final ble = VirtualLitchBle(native);
          final service = _BridgeBoard(ble, motionCapable: true);
          final journey = _Journey(tester, ble, service);
          final trace = _StoreHistoryProbe(clock: () => journey.now);
          final controller = LitchcraftController(service, appTrace: trace);
          final library = LitchcraftLibrary(controller);
          addTearDown(() async {
            await tester.pumpWidget(const SizedBox.shrink());
            library.dispose();
            controller.dispose();
            await ble.disposeJourney(tester, service);
          });
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 1000);
          await journey.finish(service.connect(_boardId));
          await journey.finish(controller.enter());
          final metadata = controller.documentSession.document;
          final preset = LitchcraftBouncingBallsPreset.decode();
          final dense = LitchcraftAnimationFrame(
            durationMs: 2000,
            emitters: List.generate(
              256,
              (i) => LitchcraftAnimatedEmitter(
                color: 0x123400 + i,
                effect: LitchcraftEffectStyle.breathe,
                effectDurationMs: 1600,
              ),
            ),
          );
          final document = LitchcraftAnimationDocument(
            frames: List.generate(
              256,
              (i) => i == 250 ? dense : preset.frames[i],
            ),
            topology: metadata.topology,
            favoriteColors: metadata.favoriteColors,
            sparkleSeed: metadata.sparkleSeed,
            playbackOnEntry: false,
          );
          await journey.finish(
            controller.replaceDocument(document),
            budgetMs: 60000,
          );
          await journey.finish(controller.selectAnimationFrame(250));
          await journey.finish(library.ready);
          await ble.advance(tester, 6000);
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          );
          await tester.pump(const Duration(milliseconds: 300));
          expect(controller.canvas.colors, dense.emitters.map((e) => e.color));
          final startWrites = ble.writes.length;
          if (origin == 'held Fill') {
            final pointer = await tester.startGesture(
              tester.getCenter(find.byKey(const ValueKey('craft-quick-fill'))),
            );
            await tester.pump(const Duration(milliseconds: 600));
            await pointer.up();
            await tester.pump();
            await journey.until(() => !controller.pending, budgetMs: 1000);
          } else if (origin == 'phone shake') {
            expect(controller.shakeEraseSupported, isTrue);
            await journey.finish(controller.eraseDisplayedFrameByPhone());
          } else {
            // Same centered normalized waveform as the independent classifier
            // proof; the existing native motion owner performs qualification.
            var x = -1085, y = -542;
            await ble.command(tester, {
              'op': 'motion',
              'x': x,
              'y': y,
              'z': 1000,
            }, flushOperations: false);
            var sign = 1;
            for (final count in [7, 7, 7, 5]) {
              for (var i = 0; i < count; ++i) {
                await ble.advance(tester, 40);
                x += sign * 310;
                y += sign * 155;
                await ble.command(tester, {
                  'op': 'motion',
                  'x': x,
                  'y': y,
                  'z': 1000,
                }, flushOperations: false);
              }
              sign = -sign;
            }
          }
          await journey.until(
            () => controller.canvas.colors.every((c) => c == 0),
            budgetMs: 1000,
            reason: 'A real accepted clear updates all256 colors',
          );
          await ble.advance(tester, 1100);
          final beforeUndo = journey.now;
          final known = controller.documentSession.nextUndoDocument != null;
          Future<void>? undo;
          if (history == 'app Undo') {
            undo = controller.undo();
            unawaited(undo.catchError((Object _) {}));
          } else {
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': 5,
              'down': true,
            }, flushOperations: false);
            await ble.advance(tester, 20);
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': 5,
              'down': false,
            }, flushOperations: false);
            await ble.advance(tester, 20);
          }
          bool parity() {
            final frame = controller.documentSession.document.frames[250];
            if (frame.durationMs != dense.durationMs ||
                controller.documentSession.selectedFrameIndex != 250) {
              return false;
            }
            for (var i = 0; i < 256; ++i) {
              final a = frame.emitters[i], b = dense.emitters[i];
              if (a.color != b.color ||
                  a.effect != b.effect ||
                  a.effectDurationMs != b.effectDurationMs) {
                return false;
              }
            }
            return true;
          }

          await tester.pump();
          final immediate = parity();
          final pendingAfterUndo = controller.pending;
          int? partialColorsAfter250;
          while (!parity() && journey.now - beforeUndo < 25000) {
            if (journey.now - beforeUndo < 100) {
              await tester.pump();
              if (ble.hasPendingOperations) {
                await ble.flush(tester, maxOperations: 1);
              } else {
                await ble.command(tester, {
                  'op': 'advance',
                  'ms': 1,
                }, flushOperations: false);
                await tester.pump(const Duration(milliseconds: 1));
              }
            } else {
              await journey.tick();
            }
            if (partialColorsAfter250 == null &&
                journey.now - beforeUndo >= 250) {
              partialColorsAfter250 = controller.canvas.colors
                  .where((c) => c != 0)
                  .length;
            }
          }
          final elapsed = journey.now - beforeUndo;
          final reads = _litchcraftRequests(ble, startWrites)
              .where((p) => p[2] == LitchcraftV2Protocol.frameStoreSnapshot)
              .toList();
          if (undo != null) await journey.finish(undo, budgetMs: 1000);
          // The existing store_frame probe intentionally exposes a CLEAN
          // durable root. Query it only after completion and quiet persistence,
          // while retaining the already measured visible latency separately.
          await ble.advance(tester, 6000);
          final board =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 250,
                  }))['store_frame']
                  as Map<String, dynamic>;
          debugPrint(
            'CLEAR_UNDO_DIAG ${jsonEncode({
              "origin": origin,
              "history": history,
              "frame_count": 256,
              "frame": 250,
              "known_inverse": known,
              "immediate_full_parity": immediate,
              "parity_ms": elapsed,
              "pending_after_undo": pendingAfterUndo,
              "nonzero_colors_at_250ms": partialColorsAfter250,
              "pending_at_end": controller.pending,
              "snapshot_requests": reads.length,
              "app_parity": parity(),
              "native_first": (board["emitters"] as List).first,
              "native_last": (board["emitters"] as List).last,
              "history_events": trace.events.map((e) => {"event": e.event, ...e.fields}).toList(),
            })}',
          );
          final restored = parity();
          final fullDocumentParity = sameLitchcraftDocument(
            controller.documentSession.document,
            document,
          );
          final fullReads = reads.where((p) => p.length == 17).length;
          final liveFrames = reads
              .where((p) => p.length == 23 && (p[17] | p[18] << 8) != 0xfffe)
              .length;
          await tester.pumpWidget(const SizedBox.shrink());
          await journey.finish(service.disconnect());
          expect(restored, isTrue);
          expect(fullDocumentParity, isTrue);
          expect(
            fullReads,
            0,
            reason: 'Ordinary Undo never reads the full saved artwork',
          );
          expect(liveFrames, lessThanOrEqualTo(1));
          expect(
            pendingAfterUndo || immediate,
            isTrue,
            reason: 'An incomplete physical inverse remains visibly pending',
          );
          expect(
            elapsed,
            lessThan(50),
            reason:
                'Accepted clear→Undo restores complete visible selected frame within50ms',
          );
        },
        variant: TargetPlatformVariant(<TargetPlatform>{
          TargetPlatform.android,
          TargetPlatform.iOS,
        }),
        skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      );
    }
  }
}

// Replay the next gesture before a frame-layout receipt, using the mounted
// editor and native Store rather than waiting for a controller-only edit.
void _registerFrameEditPointerJourneys() {
  testWidgets(
    'Store frame ordering: completed AddCopy admits immediate mounted duration',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchBle(native);
      final service = _BridgeBoard(ble);
      final journey = _Journey(tester, ble, service);
      final controller = LitchcraftController(
        service,
        monotonicMilliseconds: () => journey.now,
      );
      final library = LitchcraftLibrary(controller);
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.disposeJourney(tester, service);
      });
      await journey.hall(BigInt.zero);
      await ble.advance(tester, 1000);
      await journey.finish(service.connect(_boardId));
      await journey.finish(library.ready);
      await journey.finish(controller.enter());
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      final original = LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        sparkleSeed: 7,
        frames: List.generate(
          32,
          (frame) => LitchcraftAnimationFrame(
            durationMs: 110,
            emitters: List.generate(
              256,
              (emitter) => LitchcraftAnimatedEmitter(
                color: ((frame + 1) << 16) | emitter,
              ),
            ),
          ),
        ),
      );
      await journey.finish(
        library.open(
          LitchcraftArtwork.animated(
            id: 'frame-frontier',
            name: 'Frame frontier',
            document: original,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          ),
        ),
      );
      await journey.finish(
        library.open(
          LitchcraftArtwork.fromJson(
            LitchcraftArtwork.animated(
              id: 'frame-frontier-import',
              name: 'Artwork',
              document: original,
              createdAt: DateTime.utc(2026),
              modifiedAt: DateTime.utc(2026),
            ).toJson(),
          ),
        ),
      );
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(controller.selectAnimationFrame(31));
      await journey.finish(controller.toggleAnimationPlayback());
      for (final delta in [109, 1, 110]) {
        await ble.advance(tester, delta);
        await journey.frame();
      }
      await journey.finish(controller.toggleAnimationPlayback());
      await ble.advance(tester, 1100);
      await journey.finish(controller.selectAnimationFrame(0));
      bool? added;
      await journey.finish(
        controller.addAnimationFrame().then((v) => added = v),
      );
      expect(added, isTrue);
      await ble.advance(tester, 16);
      expect(library.selectedFrameIndex, 1);
      expect(controller.animationSelectedFrameIndex, 1);
      expect(controller.captureAnimationFrameEditTarget()?.frame, 1);
      expect(controller.boardArtworkUnresolved, isFalse);
      expect(
        sameLitchcraftDocument(
          controller.documentSession.boardDocument!,
          library.animationDocument,
        ),
        isTrue,
      );
      bool? durationApplied;
      await journey.finish(
        controller
            .setAnimationFrameDuration(137)
            .then((value) => durationApplied = value),
      );
      expect(
        durationApplied,
        isTrue,
        reason: 'The first duration edit after accepted AddCopy must apply',
      );
      expect(library.animationDocument.frames[1].durationMs, 137);
      bool? effectApplied;
      await journey.finish(
        controller
            .applyAnimationEffect(
              square: 26,
              mask: 15,
              effect: LitchcraftEffectStyle.orbitCounterclockwise,
              effectDurationMs: 1700,
            )
            .then((value) => effectApplied = value),
      );
      expect(effectApplied, isTrue);
      await journey.finish(
        controller.setBrush(color: 0xff7430, targetMask: 15),
      );
      final rect = tester.getRect(find.byType(LitchcraftCanvasView));
      Offset at(int column) =>
          rect.topLeft +
          Offset(rect.width * (column + .5) / 8, rect.height * 3.5 / 8);
      final finger = await tester.startGesture(at(3));
      await tester.pump(const Duration(milliseconds: 16));
      await finger.moveTo(at(4));
      await tester.pump(const Duration(milliseconds: 16));
      await finger.up();
      await journey.until(
        () => !controller.pending && !ble.hasPendingOperations,
        budgetMs: 10000,
      );
      final edited = library.animationDocument.frames[1];
      expect(edited.durationMs, 137);
      expect(
        edited.emitters
            .skip(26 * 4)
            .take(4)
            .map((e) => [e.effect.id, e.effectDurationMs]),
        everyElement([6, 1700]),
      );
      for (final square in [27, 28]) {
        expect(
          edited.emitters.skip(square * 4).take(4).map((e) => e.color),
          everyElement(0xff7430),
        );
      }
      final neighbors = library.animationDocument.frames.toList()..removeAt(1);
      expect(
        sameLitchcraftDocument(
          LitchcraftAnimationDocument(
            frames: neighbors,
            topology: original.topology,
            favoriteColors: original.favoriteColors,
            sparkleSeed: original.sparkleSeed,
          ),
          original,
        ),
        isTrue,
        reason:
            'Duration, effect and pointer paint retain every neighboring frame',
      );
      await _expectCleanStoreParity(journey, controller);
      await journey.finish(service.disconnect());
    },
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );
  for (final scenario in [
    (action: 'add', source: 0, receiptHeld: false),
    (action: 'add', source: 1, receiptHeld: true),
    (action: 'add', source: 2, receiptHeld: true),
    (action: 'delete', source: 0, receiptHeld: true),
    (action: 'delete', source: 1, receiptHeld: false),
    (action: 'delete', source: 2, receiptHeld: true),
  ]) {
    final action = scenario.action;
    testWidgets(
      'Store frame ordering: $action at ${scenario.source} then immediate screen drag',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedFrameEditGattBle(native, operationIntervalMs: 20);
        final service = LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        final journey = _Journey(tester, ble, service);
        final controller = LitchcraftController(
          service,
          monotonicMilliseconds: () => journey.now,
        );
        final library = LitchcraftLibrary(controller);
        addTearDown(() async {
          ble.holdOpcode = null;
          for (final packet in ble.heldCheckpoints) {
            ble.deliver(packet);
          }
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.disposeJourney(tester, service);
        });
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        final seed =
            LitchcraftDocumentSession(
                staticLitchcraftDocument(LitchcraftCanvas.blank()),
              )
              ..addFrame()
              ..addFrame();
        await journey.finish(controller.replaceDocument(seed.document));
        seed.dispose();
        await journey.until(
          () =>
              !controller.boardArtworkUnresolved &&
              controller.documentSession.document.frames.length == 3 &&
              !controller.pending,
          budgetMs: 10000,
          reason: 'Start from a completely adopted three-frame Store document',
        );
        await journey.finish(controller.selectAnimationFrame(scenario.source));
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(
          controller.setBrush(color: 0xff7430, targetMask: 15),
        );
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pump(const Duration(milliseconds: 200));
        ble.writesFrom = ble.writes.length;
        ble.holdOpcode = ConnectionSession.litchcraftStoreFrameEditOpcode;
        if (action == 'add') {
          final add = find.byKey(const ValueKey('craft-add-frame'));
          await tester.ensureVisible(add);
          await tester.tap(add);
        } else {
          final frame = find.byKey(
            ValueKey('craft-select-frame-${scenario.source}'),
          );
          await tester.ensureVisible(frame);
          await tester.longPress(frame);
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(
            find.byKey(const ValueKey('craft-confirm-delete-frame')),
          );
          await tester.pump(const Duration(milliseconds: 300));
        }
        await tester.pump();
        final selected = action == 'add'
            ? scenario.source + 1
            : (scenario.source == 0 ? 0 : scenario.source - 1);
        expect(library.selectedFrameIndex, selected);
        expect(controller.pending, isTrue);
        if (scenario.receiptHeld) {
          await journey.until(
            () => ble.heldCheckpoints.isNotEmpty,
            budgetMs: 1000,
            reason: 'Native layout applied; only its genuine receipt is held',
          );
          expect(controller.pending, isTrue);
          expect(library.selectedFrameIndex, selected);
        }
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        Offset at(int column) =>
            rect.topLeft +
            Offset(rect.width * (column + .5) / 8, rect.height * 3.5 / 8);
        final finger = await tester.startGesture(at(3));
        await tester.pump(const Duration(milliseconds: 16));
        await finger.moveTo(at(4));
        await tester.pump(const Duration(milliseconds: 16));
        await finger.up();
        await tester.pump();
        expect(
          controller
              .documentSession
              .document
              .frames[selected]
              .emitters[27 * 4]
              .color,
          0xff7430,
          reason:
              'The first drag after $action must paint the locally selected '
              'frame before its receipt; frame=${controller.animationSelectedFrameIndex} '
              'elapsed=${controller.animationElapsedMilliseconds}',
        );
        final intended = controller.documentSession.document;
        ble.holdOpcode = null;
        for (final packet in ble.heldCheckpoints) {
          ble.deliver(packet);
        }
        ble.heldCheckpoints.clear();
        await journey.until(() => !controller.pending, budgetMs: 10000);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, intended),
          isTrue,
          reason: 'Layout publication preserves the following drag',
        );
        await _expectCleanStoreParity(journey, controller);
        for (var index = 0; index < intended.frames.length; ++index) {
          if (index == selected) continue;
          expect(
            intended.frames[index].emitters.every((e) => e.color == 0),
            isTrue,
            reason: 'The following drag never edits a neighbor',
          );
        }
        await journey.finish(controller.undo());
        expect(
          controller.documentSession.document.frames.length,
          intended.frames.length,
          reason: 'Undo removes the drag, not layout',
        );
        expect(
          controller.documentSession.document.frames[selected].emitters.every(
            (e) => e.color == 0,
          ),
          isTrue,
        );
        await journey.finish(controller.redo());
        expect(
          sameLitchcraftDocument(controller.documentSession.document, intended),
          isTrue,
          reason: 'Redo restores exactly the following drag',
        );
        await _expectCleanStoreParity(journey, controller);
        journey.checkpoint(
          '$action then drag',
          await journey.frame(),
          'selected=$selected local and native Store parity',
        );
        expect(ble.discoveryCalls, 1);
        expect(ble.exactBatches, isNotEmpty);
        expect(ble.qualifiedCalls, 0);
        await journey.finish(service.disconnect());
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}

class _InterruptedStrokeBle extends _DelayedPaintCheckpointBle {
  _InterruptedStrokeBle(super.native);
  bool holdAuthority = false;
  final heldAuthority = <VirtualPacket>[];
  bool holdNextPaint = false, paintHeld = false;
  final paintAdmission = Completer<void>();
  bool holdNextHistoryQuery = false, historyQueryHeld = false;
  final historyQueryAdmission = Completer<void>();

  Future<void> beforeWrite(List<int> value) async {
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value));
    if (holdNextHistoryQuery &&
        packet?.kind == ConnectionSessionKind.control &&
        packet?.code == ConnectionSessionControl.query &&
        packet?.sequence == ConnectionSession.litchcraftStateCode) {
      holdNextHistoryQuery = false;
      historyQueryHeld = true;
      await historyQueryAdmission.future;
      historyQueryHeld = false;
    }
    if (holdNextPaint &&
        packet?.kind == ConnectionSessionKind.ordered &&
        packet?.code == ConnectionSession.litchcraftPaintOpcode) {
      holdNextPaint = false;
      paintHeld = true;
      await paintAdmission.future;
      paintHeld = false;
    }
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    await beforeWrite(value);
    await super.writeCharacteristicWithResponse(c, value: value);
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    await beforeWrite(value);
    await super.writeCharacteristicWithoutResponse(c, value: value);
  }

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final decoded = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdAuthority &&
        decoded?.kind == ConnectionSessionKind.state &&
        decoded?.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      heldAuthority.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }

  void releaseAuthority() {
    holdAuthority = false;
    final packets = List<VirtualPacket>.of(heldAuthority);
    heldAuthority.clear();
    for (final packet in packets) {
      deliver(packet);
    }
  }
}

void _registerPhysicalUndoInterruptedStrokeJourneys() {
  for (final scenario in [
    'queued tail',
    'accepted tail',
    'later drag',
    'physical repaint',
    'color pick before admission',
    'new gesture during verification',
  ]) {
    for (final stateFirst
        in scenario == 'color pick before admission'
            ? [false]
            : [false, true]) {
      testWidgets(
        scenario == 'color pick before admission'
            ? 'Hall color-pick rollback preserves zero-admission pointer stroke with older Redo'
            : 'Physical X interrupts one unsynchronized pointer stroke $scenario stateFirst=$stateFirst',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final native = (await tester.runAsync(
            () => VirtualLitchProcess.start(1, false, hardwareV3: true),
          ))!;
          final ble = _InterruptedStrokeBle(native);
          final service = _BridgeBoard(ble);
          final journey = _Journey(tester, ble, service);
          final trace = _StoreHistoryProbe(clock: () => journey.now);
          final controller = LitchcraftController(service, appTrace: trace);
          final library = LitchcraftLibrary(controller);
          addTearDown(() async {
            await tester.pumpWidget(const SizedBox.shrink());
            library.dispose();
            controller.dispose();
            await ble.disposeJourney(tester, service);
          });
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 1000);
          await journey.finish(service.connect(_boardId));
          await journey.finish(library.ready);
          await journey.finish(controller.enter());
          if (scenario != 'physical repaint') {
            controller.documentSession.paintSelectedFrame(
              squares: [0],
              mask: 15,
              color: 0x22aa44,
            );
            controller.documentSession.paintSelectedFrame(
              squares: [20],
              mask: 15,
              color: 0x9933cc,
            );
          }
          await journey.finish(
            controller.transferFrameStoreForDiagnostics(
              frames: controller.documentSession.document.frames,
              playbackOnEntry: false,
            ),
          );
          await journey.finish(controller.enter());
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          await journey.finish(
            controller.setBrush(color: 0x3498db, targetMask: 15),
          );
          if (scenario == 'color pick before admission') {
            controller.beginStroke();
            controller.paintSquares([2]);
            controller.endStroke();
            await journey.until(() => !controller.pending, budgetMs: 3000);
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': 5,
              'down': true,
            });
            await ble.advance(tester, 20);
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': 5,
              'down': false,
            });
            await ble.advance(tester, 20);
            await _expectCleanStoreParity(journey, controller);
            await journey.until(
              () => controller.canRedo,
              budgetMs: 500,
              reason: 'Raw physical Undo establishes a board-only older Redo',
            );
            ble.holdNextPaint = true;
          }
          final before = controller.documentSession.document;
          final beforeRevision =
              ((await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']['live_revision'])
                  as int;
          expect(before.frames, hasLength(1));
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          );
          await tester.pump(const Duration(milliseconds: 300));
          final rect = tester.getRect(find.byType(LitchcraftCanvasView));
          Offset at(int x, int y) =>
              rect.topLeft +
              Offset(rect.width * (x + .5) / 8, rect.height * (y + .5) / 8);
          final traceBeforeGesture = trace.events.length;
          ble.holdPaintCheckpoints = true;
          final pointer = await tester.startGesture(at(0, 0));
          await pointer.moveTo(at(1, 0));
          await tester.pump(const Duration(milliseconds: 16));
          await journey.until(
            () => scenario == 'color pick before admission'
                ? ble.paintHeld
                : ble.heldPaintCheckpoints.isNotEmpty,
            budgetMs: 500,
            reason: 'The real board accepts a prefix before its receipt',
          );
          await pointer.moveTo(at(7, 0));
          await tester.pump(const Duration(milliseconds: 16));
          await pointer.moveTo(at(7, 1));
          await tester.pump(const Duration(milliseconds: 16));
          await pointer.moveTo(at(0, 1));
          await pointer.up();
          await tester.pump();
          final painted = controller.documentSession.document;
          expect(sameLitchcraftDocument(painted, before), isFalse);
          expect(controller.pending, isTrue);
          final prefix = (await ble.command(tester, {
            'op': 'store_frame',
            'index': 0,
          }, flushOperations: false))['store_frame'];
          debugPrint(
            'PHYSICAL_X_STROKE native checkpoint; pending=${controller.pending} '
            'stateFirst=$stateFirst live=${prefix['live_revision']}',
          );
          if (scenario == 'color pick before admission') {
            expect(
              prefix['live_revision'],
              beforeRevision,
              reason: 'The first App Paint has not reached native admission',
            );
            ble.holdConnectionObservations = true;
            ble.holdPhysicalStoreObjects = true;
            await journey.hall(BigInt.one);
            await ble.advance(tester, 2000);
            await ble.advance(tester, 6000);
            final picked = (await ble.command(tester, {
              'op': 'store_frame',
              'index': 0,
            }, flushOperations: false))['store_frame'];
            expect(
              picked['emitters'],
              equals(
                before.frames.single.emitters.map(
                  (e) => [e.color, e.effect.id, e.effectDurationMs],
                ),
              ),
              reason:
                  'Real Hall color pick restores its temporary mark to the pre-stroke artwork',
            );
            final tool = ble.emitted
                .where((p) => p.channel == 'connection_session')
                .map(
                  (p) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(p.bytes),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .lastWhere(
                  (p) =>
                      p.kind == ConnectionSessionKind.state &&
                      p.code == ConnectionSession.litchcraftStateCode,
                );
            expect(
              tool.body[9] & 2,
              2,
              reason:
                  'The net-zero color pick retains the older native Redo branch',
            );
            final restore = ble.held.lastWhere((p) {
              final value = ConnectionSessionCodec.decode(
                Uint8List.fromList(p.bytes),
              );
              return value?.kind == ConnectionSessionKind.state &&
                  value?.code == ConnectionSession.litchcraftStoreEditStateCode;
            });
            ble.held.remove(restore);
            ble.holdConnectionObservations = false;
            ble.deliver(restore);
            ble.holdPhysicalStoreObjects = false;
            ble.release('connection_session');
            ble.holdPaintCheckpoints = false;
            for (final packet in List<VirtualPacket>.of(
              ble.heldPaintCheckpoints,
            )) {
              ble.deliver(packet);
            }
            ble.heldPaintCheckpoints.clear();
            ble.paintAdmission.complete();
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 10000,
            );
            await ble.advance(tester, 300);
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 10000,
            );
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                painted,
              ),
              isTrue,
              reason:
                  'A color-pick rollback with old Redo cannot discard an unadmitted app gesture',
            );
            expect(controller.status, LitchcraftStatus.conflict);
            expect(
              trace.events
                  .skip(traceBeforeGesture)
                  .where(
                    (e) =>
                        e.event ==
                            'litchcraft.store_history_stroke_prefix_accepted' ||
                        e.event ==
                            'litchcraft.store_history_stroke_physical_undo',
                  ),
              isEmpty,
              reason:
                  'An unadmitted attempt cannot produce either acceptance or Undo proof',
            );
            ble.holdPhysicalStoreObjects = false;
            ble.release('connection_session');
            await journey.hall(BigInt.zero);
            await ble.advance(tester, 400);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                painted,
              ),
              isTrue,
            );
            await journey.finish(library.flush());
            await journey.finish(service.disconnect());
            return;
          }
          if (scenario == 'accepted tail') {
            ble.holdPaintCheckpoints = false;
            final receipts = List<VirtualPacket>.of(ble.heldPaintCheckpoints);
            ble.heldPaintCheckpoints.clear();
            for (final packet in receipts) {
              ble.deliver(packet);
            }
            await tester.pump();
            ble.holdPaintCheckpoints = true;
            var acceptedRevision = prefix['live_revision'] as int;
            final start = journey.now;
            while (acceptedRevision == prefix['live_revision'] &&
                journey.now - start < 500) {
              await journey.tick();
              acceptedRevision =
                  ((await ble.command(
                        tester,
                        {'op': 'store_frame', 'index': 0},
                        flushOperations: false,
                      ))['store_frame']['live_revision'])
                      as int;
            }
            expect(
              acceptedRevision,
              greaterThan(prefix['live_revision'] as int),
              reason:
                  'The native board accepts some released-pointer suffix before X',
            );
          }
          if (scenario == 'later drag') {
            final later = await tester.startGesture(at(4, 4));
            await later.moveTo(at(5, 4));
            await later.up();
            await tester.pump();
            expect(controller.canvas.colors[36 * 4], 0x3498db);
          }
          ble.holdAuthority = true;
          ble.holdPhysicalStoreObjects = true;
          if (scenario == 'physical repaint' && !stateFirst) {
            ble.holdConnectionObservations = true;
          }
          final button = scenario == 'physical repaint' ? 6 : 5;
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': button,
            'down': true,
          }, flushOperations: false);
          // Raw GPIO first needs the production 20 ms debounce. The
          // normalized O Press then owns a full one-second hold to select Erase;
          // repainting the already-blue prefix in Paint would be a no-op.
          await ble.advance(tester, 20);
          if (scenario == 'physical repaint') {
            await ble.advance(tester, 1000);
          }
          if (scenario == 'physical repaint') {
            final heldFrame = await journey.frame();
            expect(
              heldFrame['litchcraft_erasing'],
              isTrue,
              reason: 'The real held O selects Erase before Hall input',
            );
            // Store metadata omits dirty frame bodies. Observe the existing
            // produced semantic frame rather than waiting for a save.
            expect(
              (heldFrame['board'] as List)
                  .take(2)
                  .expand((square) => square as List)
                  .map(
                    (pixel) =>
                        (pixel[0] as int) << 16 |
                        (pixel[1] as int) << 8 |
                        (pixel[2] as int),
                  ),
              everyElement(0x3498db),
              reason: 'Both accepted prefix squares visibly contain paint',
            );
          }
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': button,
            'down': false,
          }, flushOperations: false);
          await ble.advance(tester, 20);
          if (scenario == 'physical repaint') {
            expect(
              (await journey.frame())['litchcraft_erasing'],
              isTrue,
              reason: 'A completed hold consumes release without Redo',
            );
            await journey.hall(BigInt.from(3));
            await ble.advance(tester, 300);
            await journey.hall(BigInt.zero);
            await ble.advance(tester, 400);
            final repainted = (await ble.command(tester, {
              'op': 'store_frame',
              'index': 0,
            }, flushOperations: false))['store_frame'];
            expect(
              repainted['live_revision'],
              greaterThan(prefix['live_revision'] as int),
              reason: 'Actual Hall erasure changes the accepted live pixels',
            );
            final repaintedFrame = await journey.frame();
            expect(
              (repaintedFrame['board'] as List)
                  .expand((square) => square as List)
                  .every(
                    (pixel) => (pixel as List)
                        .take(3)
                        .every((channel) => channel == 0),
                  ),
              isTrue,
              reason:
                  'Ordinary Hall erasure produces the blank pre-stroke RGB '
                  'projection before any dirty-frame save',
            );
          }
          final heldAtPhysicalCheckpoint = ble.held
              .where((p) => p.channel == 'connection_session')
              .toList();
          expect(
            heldAtPhysicalCheckpoint,
            isNotEmpty,
            reason: 'Real physical input publishes native reverse paint',
          );
          if (scenario == 'new gesture during verification') {
            ble.holdNextHistoryQuery = true;
          }
          if (scenario == 'physical repaint' && !stateFirst) {
            // An Applied prefix can advance common before physical feedback.
            // Equal local/common pixels still belong to this pending gesture.
            ble.holdPaintCheckpoints = false;
            for (final packet in List<VirtualPacket>.of(
              ble.heldPaintCheckpoints,
            )) {
              ble.deliver(packet);
            }
            ble.heldPaintCheckpoints.clear();
            await tester.pump();
            ble.holdConnectionObservations = false;
          }
          if (stateFirst) ble.releaseAuthority();
          ble.holdPhysicalStoreObjects = false;
          ble.release('connection_session');
          await tester.pump();
          if (!stateFirst) ble.releaseAuthority();
          ble.holdPaintCheckpoints = false;
          for (final packet in List<VirtualPacket>.of(
            ble.heldPaintCheckpoints,
          )) {
            ble.deliver(packet);
          }
          ble.heldPaintCheckpoints.clear();
          if (scenario == 'new gesture during verification') {
            await journey.until(
              () => ble.historyQueryHeld,
              budgetMs: 3000,
              reason: 'Actual Undo proof reaches the fresh history Query',
            );
            final later = await tester.startGesture(at(0, 0));
            await later.moveTo(at(1, 0));
            await tester.pump(const Duration(milliseconds: 16));
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                painted,
              ),
              isTrue,
              reason:
                  'The real later drag begins on already-blue cells without a document edit',
            );
            ble.historyQueryAdmission.complete();
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 10000,
            );
            await ble.advance(tester, 300);
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 10000,
            );
            await later.up();
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                painted,
              ),
              isTrue,
              reason:
                  'Starting a later no-op drag invalidates the pending older Undo proof',
            );
            expect(controller.status, LitchcraftStatus.conflict);
            await journey.finish(library.flush());
            await journey.finish(service.disconnect());
            return;
          }
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
            reason: 'The interrupted gesture and late receipts settle',
          );
          if (scenario == 'physical repaint') {
            expect(
              controller.status,
              LitchcraftStatus.conflict,
              reason:
                  'Ordinary board paint back to the same preimage is not Undo',
            );
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                painted,
              ),
              isTrue,
              reason:
                  'Without fresh canRedo, retain the complete local gesture for resolution',
            );
            await journey.finish(library.flush());
            await journey.finish(service.disconnect());
            return;
          }
          if (scenario == 'later drag') {
            await ble.advance(tester, 6000);
            await journey.until(
              () => !controller.pending && !ble.hasPendingOperations,
              budgetMs: 10000,
            );
            expect(
              controller.status,
              LitchcraftStatus.conflict,
              reason:
                  'A distinct later gesture retains the existing conservative choice',
            );
            expect(
              controller.canvas.colors[36 * 4],
              0x3498db,
              reason:
                  'An inverse for stroke A cannot silently discard the later queued stroke B',
            );
            expect(controller.canvas.colors[37 * 4], 0x3498db);
            expect(
              sameLitchcraftDocument(
                controller.documentSession.document,
                before,
              ),
              isFalse,
              reason: 'The pre-A inverse cannot be adopted as B’s preimage',
            );
            await journey.finish(library.flush());
            await journey.finish(service.disconnect());
            return;
          }
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason:
                'Physical Undo while syncing remains in the editor without choosing artwork',
          );
          expect(find.text('Choose artwork'), findsNothing);
          expect(
            sameLitchcraftDocument(controller.documentSession.document, before),
            isTrue,
            reason:
                'One physical X removes the entire finger gesture, including its unsent suffix',
          );
          expect(
            trace.events
                .skip(traceBeforeGesture)
                .any(
                  (e) =>
                      e.event ==
                      'litchcraft.store_history_stroke_physical_undo',
                ),
            isTrue,
            reason: 'Actual Applied changed-prefix evidence owns cancellation',
          );
          await _expectCleanStoreParity(journey, controller);
          await ble.advance(tester, 6000);
          expect(
            sameLitchcraftDocument(controller.documentSession.document, before),
            isTrue,
            reason: 'Late tail and save cannot resurrect the canceled gesture',
          );
          final next = await tester.startGesture(at(4, 4));
          await next.moveTo(at(5, 4));
          await next.up();
          await tester.pump();
          await journey.until(
            () => !controller.pending && !ble.hasPendingOperations,
            budgetMs: 10000,
          );
          expect(controller.status, LitchcraftStatus.live);
          await _expectCleanStoreParity(journey, controller);
          final now = controller.documentSession.document;
          for (var square = 0; square < 16; square++) {
            expect(
              now.frames.single.emitters.skip(square * 4).take(4),
              orderedEquals(
                before.frames.single.emitters.skip(square * 4).take(4),
              ),
              reason:
                  'The next drag must not revive any pixel from the undone drag',
            );
          }
          await journey.finish(library.flush());
          await journey.finish(service.disconnect());
        },
        variant: TargetPlatformVariant({
          TargetPlatform.android,
          TargetPlatform.iOS,
        }),
        skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
      );
    }
  }
}

// Hold genuine layout receipts after exact GATT admission. Native State and
// Progress still flow normally, so the drag crosses the real layout boundary.
class _DelayedFrameEditGattBle extends VirtualLitchGattBle {
  _DelayedFrameEditGattBle(super.native, {super.operationIntervalMs});

  int? holdOpcode;
  int writesFrom = 0;
  final heldCheckpoints = <VirtualPacket>[];

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final observed = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdOpcode != null &&
        observed?.kind == ConnectionSessionKind.control &&
        observed?.code == ConnectionSessionControl.checkpoint &&
        writes.skip(writesFrom).any((write) {
          if (write['channel'] != 'connection_session') return false;
          final request = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return request?.kind == ConnectionSessionKind.ordered &&
              request?.code == holdOpcode &&
              request?.token == observed?.token &&
              request!.sequence <= observed!.sequence;
        })) {
      heldCheckpoints.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Withhold genuine cumulative receipts for one predecessor opcode. State and
// Progress remain production notifications; released packets are unmodified.
class _DelayedNewBoundaryBle extends VirtualLitchBle {
  _DelayedNewBoundaryBle(super.native, {super.operationIntervalMs});
  int? holdOpcode;
  int writesFrom = 0;
  final heldCheckpoints = <VirtualPacket>[];
  bool holdStoreAuthority = false;
  final heldStoreAuthority = <VirtualPacket>[];
  bool holdNewAdmission = false;
  bool newAdmissionHeld = false;
  final newGate = Completer<void>();

  Future<void> beforeWrite(List<int> value) async {
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value));
    if (holdNewAdmission &&
        packet?.kind == ConnectionSessionKind.ordered &&
        packet?.code == ConnectionSession.litchcraftStoreNewCanvasOpcode) {
      holdNewAdmission = false;
      newAdmissionHeld = true;
      await newGate.future;
      newAdmissionHeld = false;
    }
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    await beforeWrite(value);
    await super.writeCharacteristicWithResponse(c, value: value);
  }

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic c, {
    required List<int> value,
  }) async {
    await beforeWrite(value);
    await super.writeCharacteristicWithoutResponse(c, value: value);
  }

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final observed = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdStoreAuthority &&
        observed?.kind == ConnectionSessionKind.state &&
        observed?.code == ConnectionSession.litchcraftStoreAuthorityStateCode) {
      heldStoreAuthority.add(packet);
      return;
    }
    if (holdOpcode != null &&
        observed?.kind == ConnectionSessionKind.control &&
        observed?.code == ConnectionSessionControl.checkpoint &&
        writes.skip(writesFrom).any((write) {
          if (write['channel'] != 'connection_session') return false;
          final request = ConnectionSessionCodec.decode(
            Uint8List.fromList(write['bytes'] as List<int>),
          );
          return request?.kind == ConnectionSessionKind.ordered &&
              request?.code == holdOpcode &&
              request?.token == observed?.token &&
              request!.sequence <= observed!.sequence;
        })) {
      heldCheckpoints.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Genuine SDK receipt delays exercise the existing app queue and native Store.
void _registerQueuedNewJourneys() {
  for (final scenario in [
    'idle',
    'queued-play',
    'queued-fill',
    'queued-stroke',
    'queued-structure',
    'physical-before-guard',
    'physical-after-guard',
    'physical-after-applied',
    'superseded',
    'pending-blank',
  ]) {
    testWidgets(
      'New artwork ordered boundary $scenario',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _DelayedNewBoundaryBle(native, operationIntervalMs: 20);
        final trace = _SyncRecoveryTrace(
          () => tester.binding.clock.now().millisecondsSinceEpoch,
        );
        final service = _BridgeBoard(ble, appTrace: trace);
        final journey = _Journey(tester, ble, service);
        final seed = LitchcraftDocumentSession(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        );
        seed.paintSelectedFrame(squares: [7], mask: 15, color: 0x11cc33);
        final controller = LitchcraftController(
          service,
          documentSession: seed,
          appTrace: trace,
          monotonicMilliseconds: () => journey.now,
        );
        final library = LitchcraftLibrary(controller);
        var cleaned = false;
        Future<void> cleanup() async {
          if (cleaned) return;
          cleaned = true;
          ble.holdOpcode = null;
          ble.holdStoreAuthority = false;
          for (final packet in ble.heldStoreAuthority) {
            ble.deliver(packet);
          }
          ble.heldStoreAuthority.clear();
          if (!ble.newGate.isCompleted) ble.newGate.complete();
          ble.heldChannels.clear();
          ble.release('connection_session');
          for (final packet in ble.heldCheckpoints) {
            ble.deliver(packet);
          }
          ble.heldCheckpoints.clear();
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await journey.finish(service.dispose(), budgetMs: 1000);
          await ble.flush(tester);
          await tester.runAsync(ble.close);
          await tester.runAsync(native.close);
          await tester.pump();
        }

        addTearDown(cleanup);
        await journey.hall(BigInt.zero);
        await ble.advance(tester, 1000);
        await journey.finish(service.connect(_boardId));
        await journey.finish(library.ready);
        await journey.finish(controller.enter());
        await journey.finish(
          controller.transferFrameStoreForDiagnostics(
            frames: seed.document.frames,
            playbackOnEntry: false,
          ),
        );
        await journey.finish(controller.enter());
        await _expectCleanStoreParity(journey, controller);
        await journey.finish(
          controller.setBrush(color: 0xff7430, targetMask: 15),
        );
        await ble.flush(tester);
        expect(controller.pending, isFalse);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        expect(find.byType(LitchcraftCanvasView), findsOneWidget);
        Future<dynamic>? previous;
        final heldPredecessor =
            scenario.startsWith('queued-') ||
            scenario == 'physical-before-guard' ||
            scenario == 'superseded';
        if (scenario == 'queued-structure') {
          await journey.finish(controller.addAnimationFrame());
          expect(controller.documentSession.selectedFrameIndex, 1);
          await _expectCleanStoreParity(journey, controller);
        }
        if (heldPredecessor) {
          ble.holdOpcode = scenario == 'queued-fill'
              ? 9
              : scenario == 'queued-stroke'
              ? 3
              : scenario == 'queued-structure'
              ? 10
              : 2;
          ble.writesFrom = ble.writes.length;
          if (scenario == 'queued-play' ||
              scenario == 'physical-before-guard' ||
              scenario == 'superseded') {
            previous = controller.toggleAnimationPlayback();
          } else if (scenario == 'queued-fill') {
            previous = controller.fillAllWithColor(0x2244cc);
          } else if (scenario == 'queued-structure') {
            previous = controller.addAnimationFrame();
          } else {
            controller.beginStroke();
            controller.paintSquares([12]);
          }
          previous?.ignore();
          await journey.until(
            () => ble.heldCheckpoints.isNotEmpty,
            budgetMs: 1000,
          );
          if (scenario == 'queued-stroke') controller.endStroke();
          if (scenario == 'queued-fill') {
            // Distinct earlier Fill output cannot masquerade as the new stroke.
            // Its unsent Tool is also retired with the old artwork source.
            controller.setBrush(color: 0xff7430, targetMask: 15).ignore();
          }
          expect(
            controller.pending,
            isTrue,
            reason: 'Genuine prior ordered receipt is held',
          );
        } else if (scenario == 'physical-after-applied') {
          ble.writesFrom = ble.writes.length;
          ble.holdOpcode = ConnectionSession.litchcraftStoreNewCanvasOpcode;
        } else if (scenario == 'physical-after-guard') {
          ble.holdNewAdmission = true;
        } else {
          ble.heldChannels.add('connection_session');
        }
        if (scenario == 'pending-blank') {
          await ble.command(tester, {'op': 'store_worker', 'enabled': false});
        }
        final writesFrom = ble.writes.length;
        final start = journey.now;
        Map<String, dynamic>? lastStore;
        final first = <String, int>{};
        var completed = false;
        Object? newError;
        final newWork = library.newArtwork();
        newWork.then<void>(
          (_) {
            completed = true;
          },
          onError: (Object error, StackTrace _) {
            newError = error;
            completed = true;
          },
        );
        await tester.pump();
        await journey.until(
          () =>
              controller.documentSession.document.frames.length == 1 &&
              !controller.canvas.hasMarks,
          budgetMs: 1000,
        );
        first['local_blank_ms'] = journey.now - start;
        await tester.pump();
        expect(
          tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas
              .hasMarks,
          isFalse,
          reason: 'Pointer starts after the actual blank widget publishes',
        );
        expect(
          completed,
          isFalse,
          reason: 'Pointer precedes New terminal delivery',
        );
        final rect = tester.getRect(find.byType(LitchcraftCanvasView));
        Offset at(int column) =>
            rect.topLeft +
            Offset(rect.width * (column + .5) / 8, rect.height * 3.5 / 8);
        final finger = await tester.startGesture(at(3));
        await finger.moveTo(at(4));
        await tester.pump(const Duration(milliseconds: 16));
        first['pointer_move_ms'] = journey.now - start;
        expect(
          controller
              .documentSession
              .document
              .frames
              .single
              .emitters[27 * 4]
              .color,
          0xff7430,
        );
        await finger.up();
        await tester.pump();
        first['pointer_up_ms'] = journey.now - start;
        final intended = controller.documentSession.document;
        Future<void> sample(String phase) async {
          final frame = await journey.frame();
          final store =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }, flushOperations: false))['store_frame']
                  as Map<String, dynamic>;
          final packets = ble.writes
              .skip(writesFrom)
              .where((w) => w['channel'] == 'connection_session')
              .map(
                (w) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(w['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .toList();
          final ordered = packets
              .where((p) => p.kind == ConnectionSessionKind.ordered)
              .toList();
          if (ordered.any((p) => p.code == 11)) {
            first.putIfAbsent(
              'ordered_new_enqueued_ms',
              () => journey.now - start,
            );
          }
          if (ordered.any((p) => p.code == 3)) {
            first.putIfAbsent(
              'ordered_paint_enqueued_ms',
              () => journey.now - start,
            );
          }
          final rendered =
              (frame['board'] as List)[27].any(
                    (p) => (p as List)[0] > p[1] && p[0] > p[2],
                  )
                  as bool;
          if (rendered) {
            first.putIfAbsent(
              'stroke_native_rendered_ms',
              () => journey.now - start,
            );
            first.putIfAbsent(
              'stroke_native_live_revision',
              () => store['live_revision'] as int,
            );
          }
          if (completed) {
            first.putIfAbsent(
              'new_future_terminal_ms',
              () => journey.now - start,
            );
          }
          lastStore = store;
        }

        await sample('pointer_before_receipt');
        // Advance only the existing transport/native session; no synthetic queue fields.
        for (var n = 0; n < 10; n++) {
          await journey.tick();
        }
        await sample('held_10_turns');
        if (scenario.startsWith('physical-')) {
          if (scenario == 'physical-after-guard') {
            await journey.until(() => ble.newAdmissionHeld, budgetMs: 1000);
          } else if (scenario == 'physical-after-applied') {
            await journey.until(
              () => ble.heldCheckpoints.isNotEmpty,
              budgetMs: 1000,
            );
          }
          if (scenario == 'physical-after-applied') {
            ble.holdStoreAuthority = true;
          }
          await journey.hall(BigInt.one << 20);
          await ble.advance(tester, 400);
          await journey.hall(BigInt.zero);
          await ble.advance(tester, 400);
          final physical =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          final physicalOutput = await journey.frame();
          expect(
            ((physicalOutput['board'] as List)[20] as List).any(
              (p) => (p as List).take(4).any((v) => v != 0),
            ),
            isTrue,
            reason:
                'Production Hall path rendered physical paint on the previously blank square',
          );
          expect(
            physical['live_revision'],
            greaterThan(lastStore!['live_revision'] as int),
          );
          if (scenario == 'physical-after-guard') {
            expect(ble.newAdmissionHeld, isTrue);
            ble.newGate.complete();
            await journey.until(() => completed, budgetMs: 5000);
            expect(
              newError,
              isNotNull,
              reason: 'Refused New must not report success',
            );
            expect(controller.status, LitchcraftStatus.conflict);
            expect(
              controller.canvas.colors[27 * 4],
              0xff7430,
              reason: 'The new local draft survives refusal',
            );
            final survived =
                (await ble.command(tester, {
                      'op': 'store_frame',
                      'index': 0,
                    }))['store_frame']
                    as Map<String, dynamic>;
            expect(survived['live_revision'], physical['live_revision']);
            final survivedOutput = await journey.frame();
            expect(
              ((survivedOutput['board'] as List)[20] as List).any(
                (p) => (p as List).take(4).any((v) => v != 0),
              ),
              isTrue,
            );
            final issued = ble.writes
                .skip(writesFrom)
                .where((w) => w['channel'] == 'connection_session')
                .map(
                  (w) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(w['bytes'] as List<int>),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .where(
                  (p) =>
                      p.kind == ConnectionSessionKind.ordered && p.code == 11,
                );
            expect(
              issued,
              hasLength(1),
              reason: 'Conflict must not rebase and replay New',
            );
            expect(
              _litchcraftRequests(ble, writesFrom).any(
                (p) =>
                    p.length > 2 &&
                    p[2] == LitchcraftV2Protocol.frameStoreBegin,
              ),
              isFalse,
            );
            expect(service.isConnected, isTrue);
            journey.checkpoint(
              'new-stale-guard-refused',
              await journey.frame(),
              'accepted=false draft=retained physical=retained next=choose-source',
            );
            await journey.finish(library.newArtwork());
            expect(controller.status, LitchcraftStatus.live);
            expect(controller.canvas.hasMarks, isFalse);
            final retried = ble.writes
                .skip(writesFrom)
                .where((w) => w['channel'] == 'connection_session')
                .map(
                  (w) => ConnectionSessionCodec.decode(
                    Uint8List.fromList(w['bytes'] as List<int>),
                  ),
                )
                .whereType<ConnectionSessionPacket>()
                .where(
                  (p) =>
                      p.kind == ConnectionSessionKind.ordered &&
                      p.code ==
                          ConnectionSession.litchcraftStoreNewCanvasOpcode,
                );
            expect(
              retried,
              hasLength(2),
              reason: 'Only the explicit second New retries, still compact',
            );
            expect(
              _litchcraftRequests(ble, writesFrom).any(
                (p) =>
                    p.length > 2 &&
                    p[2] == LitchcraftV2Protocol.frameStoreBegin,
              ),
              isFalse,
            );
            await _expectCleanStoreParity(journey, controller);
            await cleanup();
            return;
          }
        }
        if (scenario == 'superseded') {
          final replacement = staticLitchcraftDocument(
            LitchcraftCanvas.blank(topology: controller.canvas.topology),
          );
          final second = controller.replaceDocument(
            replacement,
            newCanvas: true,
          );
          ble.holdOpcode = null;
          for (final packet in ble.heldCheckpoints.toList()) {
            ble.deliver(packet);
          }
          ble.heldCheckpoints.clear();
          await journey.finish(second);
          await journey.until(() => completed);
          expect(newError, isA<LitchcraftTransferCancelled>());
          expect(
            controller.canvas.hasMarks,
            isFalse,
            reason: 'Superseded result cannot revive first draft paint',
          );
          final issued = ble.writes
              .skip(writesFrom)
              .where((w) => w['channel'] == 'connection_session')
              .map(
                (w) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(w['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where(
                (p) => p.kind == ConnectionSessionKind.ordered && p.code == 11,
              );
          expect(issued, hasLength(1), reason: 'Only current source sends New');
          await _expectCleanStoreParity(journey, controller);
          await cleanup();
          return;
        }
        first['controlled_release_ms'] = journey.now - start;
        if (heldPredecessor || scenario == 'physical-after-applied') {
          ble.holdOpcode = null;
          for (final packet in ble.heldCheckpoints.toList()) {
            ble.deliver(packet);
          }
          ble.heldCheckpoints.clear();
        } else {
          ble.heldChannels.remove('connection_session');
          ble.release('connection_session');
        }
        await tester.pump();
        if (scenario == 'physical-after-applied') {
          await journey.until(() => completed, budgetMs: 1000);
          // A reliable physical batch is a repair witness even when State37
          // still names the accepted blank. Inspect must start before its
          // newer State is released; cached equality cannot suppress repair.
          await journey.until(
            () => _litchcraftRequests(
              ble,
              writesFrom,
            ).any((p) => p.length > 2 && p[2] == LitchcraftV2Protocol.inspect),
            budgetMs: 1000,
          );
          expect(ble.heldStoreAuthority, isNotEmpty);
          ble.holdStoreAuthority = false;
          for (final packet in ble.heldStoreAuthority.toList()) {
            ble.deliver(packet);
          }
          ble.heldStoreAuthority.clear();
        }
        for (
          var n = 0;
          n < 180 &&
              (!completed ||
                  controller.pending ||
                  !first.containsKey('stroke_native_rendered_ms'));
          n++
        ) {
          await journey.tick();
          await sample('observe');
        }
        await sample('drained');
        expect(newError, isNull);
        expect(completed, isTrue);
        expect(
          controller.status,
          LitchcraftStatus.live,
          reason: controller.errorMessage,
        );
        expect(
          scenario == 'physical-after-applied' ||
              sameLitchcraftDocument(
                intended,
                controller.documentSession.document,
              ),
          isTrue,
          reason: 'New result cannot erase the immediate mounted stroke',
        );
        expect(
          first.containsKey('stroke_native_rendered_ms'),
          isTrue,
          reason:
              'The actual native semantic center must show paint before persistence release',
        );
        if (scenario == 'pending-blank') {
          final heldRoot = lastStore!;
          expect(heldRoot['dirty'], isTrue);
          first['blank_worker_release_ms'] = journey.now - start;
          await ble.command(tester, {
            'op': 'store_worker',
            'enabled': true,
          }, flushOperations: false);
        }
        await journey.finish(newWork);
        if (previous != null) {
          try {
            await journey.finish(previous);
          } on LitchcraftTransferCancelled {
            expect(
              scenario,
              anyOf('queued-fill', 'queued-structure'),
              reason:
                  'Retired source mutation must cancel without retiring New',
            );
          }
        }
        if (scenario == 'physical-after-applied') {
          expect(
            controller.canvas.colors[20 * 4],
            0xff7430,
            reason:
                'Accepted physical paint after New must survive delayed receipt',
          );
          expect(
            controller.canvas.colors[27 * 4],
            0xff7430,
            reason: 'Newer local stroke survives physical repair',
          );
        } else if (scenario == 'physical-before-guard') {
          expect(
            controller.canvas.colors[20 * 4],
            0,
            reason: 'Explicit New replaces work before its serialized guard',
          );
        }
        final saved = await _expectCleanStoreParity(journey, controller);
        await sample('clean_parity');
        final packets = ble.writes
            .skip(writesFrom)
            .where((w) => w['channel'] == 'connection_session')
            .map(
              (w) => ConnectionSessionCodec.decode(
                Uint8List.fromList(w['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .toList();
        final ordered = packets
            .where((p) => p.kind == ConnectionSessionKind.ordered)
            .toList();
        final storeRequests = _litchcraftRequests(
          ble,
          writesFrom,
        ).map((p) => p.length > 2 ? p[2] : -1).toList();
        debugPrint(
          'NEW_BOUNDARY $scenario first=${jsonEncode(first)} saved_generation=$saved',
        );
        expect(
          ordered.where(
            (p) => p.code == ConnectionSession.litchcraftStoreNewCanvasOpcode,
          ),
          hasLength(1),
          reason: 'New must remain compact even behind an issued command',
        );
        expect(
          storeRequests.contains(LitchcraftV2Protocol.frameStoreBegin),
          isFalse,
          reason: 'No hidden full-document replacement',
        );
        expect(
          ble.connections,
          hasLength(1),
          reason: 'Source retirement must not disconnect the current session',
        );
        journey.checkpoint(
          'new-$scenario-retained-stroke',
          await journey.frame(),
          'accepted=true local=blank-then-stroke saved=true next=draw',
        );
        await cleanup();
      },
      variant: TargetPlatformVariant(<TargetPlatform>{
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
