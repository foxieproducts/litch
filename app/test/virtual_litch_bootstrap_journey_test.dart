import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/home_board_actions.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

extension _SettingWrite on ConnectionSessionPacket {
  String get settingKey => SettingsStateWireCodec.mutationKeys[body[1]];
  int get settingValue =>
      ByteData.sublistView(body).getUint16(2, Endian.little);
}

bool _settingsState(ConnectionSessionPacket packet) =>
    packet.kind == ConnectionSessionKind.state &&
    (SettingsStateWireCodec.acceptsCode(packet.code) ||
        packet.code == SettingsStateWireCodec.playerPaletteStateCode);

bool _appliedCheckpoint(
  ConnectionSessionPacket packet,
  ConnectionSessionPacket write,
) {
  if (packet.kind != ConnectionSessionKind.control ||
      packet.code != ConnectionSessionControl.checkpoint ||
      packet.token != write.token) {
    return false;
  }
  final offset = packet.sequence - write.sequence;
  final exceptions = ByteData.sublistView(
    packet.body,
  ).getUint16(8, Endian.little);
  return offset >= 0 && offset < 16 && (exceptions & (1 << offset)) == 0;
}

void main() {
  for (final mtu in [23, 247]) {
    testWidgets(
      'bulk bootstrap: production first Home frame at MTU $mtu',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(2, false),
        ))!;
        final ble = VirtualLitchGattBle(native, negotiatedMtu: mtu)
          ..dropConsole = true;
        final service = ble.createService();
        final writes = ble.sessionWrites.where(
          (packet) =>
              packet.kind == ConnectionSessionKind.ordered &&
              packet.code == SettingsStateWireCodec.mutationOpcode,
        );
        void releaseSettings() => ble.releaseWhere(
          (packet) =>
              packet.sessionPacket != null &&
              _settingsState(packet.sessionPacket!),
        );
        bool appliedSince(int start, ConnectionSessionPacket write) => ble
            .emitted
            .skip(start)
            .any(
              (p) =>
                  p.sessionPacket != null &&
                  _appliedCheckpoint(p.sessionPacket!, write),
            );
        Iterable<SettingsStateSnapshot> heldSettings({int skip = 0}) sync* {
          final assembler = SettingsStateWireAssembler();
          for (final record in ble.held.skip(skip)) {
            final packet = record.sessionPacket;
            if (packet == null || !_settingsState(packet)) continue;
            final state = assembler.accept(
              code: packet.code,
              revision: packet.sequence,
              body: packet.body,
            );
            if (state != null) yield state;
          }
        }

        bool heldSetting(String key, Object value) =>
            heldSettings().any((snapshot) => snapshot.values[key] == value);

        Future<void> waitForSheet(Finder control) =>
            ble.advanceUntil(tester, () {
              final element = control.evaluate().firstOrNull;
              return element != null &&
                  ModalRoute.of(element)?.animation?.isCompleted == true;
            }, 'the sheet containing $control opens');
        Future<void> waitForSheetClosed(Finder control) => ble.advanceUntil(
          tester,
          () => control.evaluate().isEmpty,
          'the sheet containing $control closes',
        );
        addTearDown(() => ble.disposeJourney(tester, service));
        try {
          const id = 'virtual-litch-production';
          expect(
            await ble.completeTransaction(tester, service.connect(id)),
            isTrue,
          );
          for (final entry in {
            'P1_END_PIECE_WHEEL': 40,
            'P2_END_PIECE_WHEEL': 200,
            'VS_CPU_LEVEL': 4,
            'VS_CPU_PLAYER': 2,
            'TIMER': 1,
          }.entries) {
            await ble.confirmSetting(tester, service, entry.key, entry.value);
          }
          await ble.command(tester, {'op': 'hall', 'mask': 'FFFF00000000FFFF'});
          await ble.advance(tester, 1000);
          final frame = await ble.frame(tester);
          final mapping = (frame['logical_to_physical'] as List).cast<int>();
          final occupied = BigInt.parse('FFFF00000000FFFF', radix: 16);
          const e2 = 52;
          await ble.command(tester, {
            'op': 'hall',
            'mask': (occupied & ~(BigInt.one << mapping[e2]))
                .toRadixString(16)
                .padLeft(16, '0'),
          });
          await ble.advance(tester, 100);
          await ble.completeTransaction(tester, service.disconnect());
          ble.holdSessionPacket = (packet) =>
              packet.kind == ConnectionSessionKind.state;
          expect(
            await ble.completeTransaction(tester, service.connect(id)),
            isTrue,
          );
          var usableFrames = 0;
          void checkRenderedPalette() {
            final boardFinder = find.byType(HomeChessboard);
            if (boardFinder.evaluate().isEmpty) {
              expect(
                find.byType(HomeScreen, skipOffstage: false),
                findsNothing,
                reason: 'Unknown mode must not mount a speculative Home frame',
              );
              return;
            }
            final board = tester.widget<HomeChessboard>(boardFinder);
            if (!service.hasBoardStateSnapshot) {
              // A palette received before the full snapshot must already match the board.
              if (board.snapshot.playerColorsConfirmed) {
                expect(board.snapshot.p1PieceWheel, 40);
                expect(board.snapshot.p2PieceWheel, 200);
              }
              return;
            }
            ++usableFrames;
            expect(board.snapshot.playerColorsConfirmed, isTrue);
            expect(board.snapshot.p1PieceWheel, 40);
            expect(board.snapshot.p2PieceWheel, 200);
            expect(board.snapshot.physicalStatus!.p1AtOppositeEnd, isTrue);
            expect(
              litchPieceAccentColor(board.snapshot, whitePiece: true),
              litchWheelPreviewColor(200),
            );
            expect(
              litchPieceAccentColor(board.snapshot, whitePiece: false),
              litchWheelPreviewColor(40),
            );
            expect(board.snapshot.clockStatus!.running, isTrue);
            final actions = tester.widget<HomeBoardActions>(
              find.byType(HomeBoardActions),
            );
            expect(
              actions.controller.coachingUnavailableReason(
                LitchControlOwner.p1,
              ),
              isNull,
            );
            expect(
              actions.controller.coachingUnavailableReason(
                LitchControlOwner.p2,
              ),
              isNotNull,
            );
          }

          Future<void> deliverAndCheck(List<VirtualPacket> packets) async {
            ble.held.removeWhere(packets.contains);
            for (final packet in packets) {
              ble.deliver(packet);
              await tester.pump();
              // Flush the actual service/feed callbacks, then render their frame.
              await tester.pump();
              checkRenderedPalette();
            }
          }

          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              home: RepaintBoundary(
                child: LitchMainScaffold(
                  bleService: service,
                  boardId: id,
                  boards: const [
                    LitchBoard(id: id, name: 'Foxie_Litch', rssi: -45),
                  ],
                  onDisconnect: service.disconnect,
                  loadAppConsolePreference: () async => false,
                ),
              ),
            ),
          );
          checkRenderedPalette();
          await ble.flush(tester);
          checkRenderedPalette();
          for (var step = 0; step < 8; ++step) {
            await ble.advance(tester, 100);
            checkRenderedPalette();
          }
          final packets = List<VirtualPacket>.of(ble.held);
          expect(packets, isNotEmpty);
          expect(packets.every((p) => p.bytes.length <= mtu - 3), isTrue);
          expect(packets.any((p) => p.sessionPacket!.isHomeState), isTrue);
          expect(packets.any((p) => _settingsState(p.sessionPacket!)), isTrue);
          await deliverAndCheck(packets);
          for (var step = 0; step < 60 && usableFrames == 0; ++step) {
            await ble.advance(tester, 100);
            checkRenderedPalette();
            await deliverAndCheck(List<VirtualPacket>.of(ble.held));
          }
          expect(usableFrames, greaterThan(0));
          final home = service.latestHomeState!;
          expect(home.position.sideToMove, HomeSide.white);
          expect(home.position.pieces[e2], 'P');
          expect(home.assistance!.legalSource, e2);
          // Seating rotates the view; speech and markers keep chess coordinates.
          expect(find.text('Legal moves from e2 are marked.'), findsOneWidget);
          for (final (square, role) in [
            ('e2', 'from'),
            ('e3', 'to'),
            ('e4', 'to'),
          ]) {
            expect(
              find.descendant(
                of: find.byKey(ValueKey('square-$square')),
                matching: find.byKey(ValueKey('assistance-$role-$square')),
              ),
              findsOneWidget,
            );
          }
          debugPrint(
            'BULK_BOOTSTRAP mtu=$mtu heldStateBytes=${packets.fold<int>(0, (total, p) => total + p.bytes.length)} '
            'packets=${packets.length} usableFrames=$usableFrames '
            'firstPlayer=2 endPalette=40/200 logicalPalette=200/40 clock=running next=move-lifted-piece',
          );
          await tester.runAsync(() async {
            final context = tester.element(find.byType(HomeChessboard));
            await Future.wait(
              tester
                  .widgetList<Image>(find.byType(Image))
                  .map((image) => precacheImage(image.image, context)),
            );
          });
          await tester.pump();
          await captureHealthReview(
            tester,
            'bulk-first-home-${Theme.of(tester.element(find.byType(HomeChessboard))).platform.name}-mtu-$mtu',
          );

          // The board applies slider writes while the app's State delivery is held.
          ble.holdSessionPacket = null;
          ble.releaseWhere(
            (p) => p.sessionPacket?.kind == ConnectionSessionKind.state,
          );
          await ble.advance(tester, 200);
          final connected = find.byKey(
            const ValueKey('connected-board-status-action'),
          );
          expect(connected.hitTestable(), findsOneWidget);
          await tester.tap(connected);
          final slider = find.descendant(
            of: find.byKey(const ValueKey('board-menu-brightness')),
            matching: find.byType(Slider),
          );
          await waitForSheet(slider);
          await tester.ensureVisible(slider);
          await tester.pump();
          final bounds = tester.getRect(slider);
          // The board-menu slider explicitly uses zero track padding.
          final track = bounds.width;
          ble.holdSessionPacket = _settingsState;
          final brightnessPackets = ble.emitted.length;
          final gesture = await tester.startGesture(
            Offset(bounds.left + track * .5, bounds.center.dy),
          );
          await gesture.moveTo(
            Offset(bounds.left + track * .8, bounds.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 16));
          final firstPreview = tester.widget<Slider>(slider).value;
          await ble.advanceUntil(
            tester,
            () => heldSetting('BRIGHTNESS', firstPreview.toInt()),
            'the board publishes the first drag value',
          );
          expect(tester.widget<Slider>(slider).value, firstPreview);
          await gesture.moveTo(
            Offset(bounds.left + track * .2, bounds.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 16));
          final secondPreview = tester.widget<Slider>(slider).value;
          expect(secondPreview, lessThan(firstPreview));
          // Deliver an earlier authoritative board result while the finger
          // still owns the preview, then keep dragging to a different value.
          releaseSettings();
          await tester.pump();
          await tester.pump();
          expect(
            service.latestSettingValue('BRIGHTNESS'),
            firstPreview.toInt(),
          );
          expect(tester.widget<Slider>(slider).value, secondPreview);
          await gesture.moveTo(Offset(bounds.right - 1, bounds.center.dy));
          await tester.pump(const Duration(milliseconds: 16));
          expect(tester.widget<Slider>(slider).value, 8);
          await gesture.up();
          await ble.flush(tester);
          for (var step = 0; step < 12; ++step) {
            await ble.advance(tester, 100);
          }
          final finalWrite = writes.lastWhere(
            (p) => p.settingKey == 'BRIGHTNESS' && p.settingValue == 8,
          );
          expect(appliedSince(brightnessPackets, finalWrite), isTrue);
          // The board sent its terminal Checkpoint while Settings State was held.
          expect(tester.widget<Slider>(slider).value, 8);
          expect(service.latestSettingValue('BRIGHTNESS'), isNot(8));
          final pendingSettings = List<VirtualPacket>.of(ble.held);
          expect(pendingSettings, isNotEmpty);
          ble.held.clear();
          for (final packet in pendingSettings) {
            ble.deliver(packet);
            await tester.pump();
            await tester.pump();
            expect(
              tester.widget<Slider>(slider).value,
              8,
              reason: 'Older queued records must not retire the final preview.',
            );
          }
          ble.holdSessionPacket = null;
          await ble.advance(tester, 1000);
          await tester.pump();
          expect(service.latestSettingValue('BRIGHTNESS'), 8);
          expect(tester.widget<Slider>(slider).value, 8);
          expect(find.textContaining('Couldn’t confirm'), findsNothing);
          debugPrint(
            'BULK_SLIDER mtu=$mtu held-preview=$secondPreview '
            'final=8 checkpoint-before-settings-state stable=true',
          );

          await tester.tapAt(const Offset(8, 780));
          await waitForSheetClosed(slider);
          expect(slider, findsNothing);
          final setup = find.byKey(const ValueKey('main-destination-setup'));
          expect(setup.hitTestable(), findsOneWidget);
          await tester.tap(setup);
          final settingsScroll = find.byKey(const ValueKey('settings-scroll'));
          await ble.advanceUntil(
            tester,
            () => settingsScroll.evaluate().isNotEmpty,
            'Setup opens',
          );

          // Player assignment changes logical ownership, not the fixed end
          // controlled by either Setup row. Exercise both actual pickers.
          final priorPalette = <VirtualPacket>[];
          for (final entry in {'P1': 85, 'P2': 0}.entries) {
            final row = find.byKey(ValueKey('quick:${entry.key}_PIECE_WHEEL'));
            await tester.scrollUntilVisible(
              row,
              200,
              scrollable: find
                  .descendant(
                    of: settingsScroll,
                    matching: find.byType(Scrollable),
                  )
                  .first,
            );
            await tester.pump();
            expect(row.hitTestable(), findsOneWidget);
            await tester.tap(row);
            final choice = find.byKey(
              ValueKey(
                'piece-color-choice:${entry.key}_PIECE_WHEEL:${entry.value}',
              ),
            );
            await waitForSheet(choice);
            await tester.ensureVisible(choice);
            await tester.pump();
            final writesBefore = writes.length;
            final emittedBefore = ble.emitted.length;
            final key = '${entry.key}_END_PIECE_WHEEL';
            ble.holdSessionPacket = _settingsState;
            await tester.tap(choice);
            await waitForSheetClosed(choice);
            await ble.advanceUntil(
              tester,
              () {
                final write = writes.skip(writesBefore).firstOrNull;
                return write != null &&
                    appliedSince(emittedBefore, write) &&
                    heldSetting(key, entry.value);
              },
              'firmware acknowledges $key with its new Settings State held',
            );
            final mutation = writes.skip(writesBefore).single;
            expect(mutation.settingKey, key);
            expect(mutation.settingValue, entry.value);
            expect(service.latestSettingValue(key), isNot(entry.value));
            for (final end in ['P1', 'P2']) {
              final control = find.byKey(ValueKey('quick:${end}_PIECE_WHEEL'));
              expect(
                tester
                    .widget<InkWell>(
                      find.descendant(
                        of: control,
                        matching: find.byType(InkWell),
                      ),
                    )
                    .onTap,
                isNotNull,
                reason:
                    'Independent player-color keys remain usable while one '
                    'pending value stays locally owned until confirmation.',
              );
            }
            debugPrint(
              'BULK_PALETTE_PENDING mtu=$mtu row=${entry.key} '
              'checkpoint=received settings-state=held independent-controls=enabled',
            );
            if (entry.key == 'P2') priorPalette.addAll(ble.held);
            ble.holdSessionPacket = null;
            releaseSettings();
            await ble.advanceUntil(
              tester,
              () => service.latestSettingValue(key) == entry.value,
              'the board confirms $key',
            );
            final color = litchWheelPreviewColor(entry.value);
            expect(
              find.descendant(
                of: row,
                matching: find.byKey(
                  ValueKey('piece-color-swatch:${color.toARGB32()}'),
                ),
              ),
              findsOneWidget,
            );
            debugPrint(
              'BULK_PALETTE_FIXED_END mtu=$mtu firstPlayer=2 '
              'row=${entry.key} write=${mutation.settingKey}=${mutation.settingValue} '
              'rendered=${entry.value}',
            );
          }
          expect(service.latestSettingValue('P1_END_PIECE_WHEEL'), 85);
          expect(service.latestSettingValue('P2_END_PIECE_WHEEL'), 0);

          // A held live hue preview must finish before a named preset can
          // commit. The picker remains usable while the write owner coalesces
          // the newest same-key intent, so choosing the opening color still
          // restores it after the drag.
          final p1HueRow = find.byKey(const ValueKey('quick:P1_PIECE_WHEEL'));
          await tester.ensureVisible(p1HueRow);
          await tester.pump();
          await tester.tap(p1HueRow);
          final hue = find.byKey(
            const ValueKey('piece-color-hue:P1_PIECE_WHEEL'),
          );
          await waitForSheet(hue);
          await tester.ensureVisible(hue);
          await tester.pump();
          expect(hue.hitTestable(), findsOneWidget);
          final hueBounds = tester.getRect(hue);
          final handoffWritesBefore = writes.length;
          final handoffPackets = ble.emitted.length;
          ble.holdSessionPacket = (packet) =>
              _settingsState(packet) ||
              (packet.kind == ConnectionSessionKind.control &&
                  packet.code == ConnectionSessionControl.checkpoint);
          final hueDrag = await tester.startGesture(
            Offset(
              hueBounds.left + 14 + (hueBounds.width - 28) / 3,
              hueBounds.center.dy,
            ),
          );
          await hueDrag.moveTo(
            Offset(hueBounds.left + 44, hueBounds.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 20));
          expect(tester.widget<Slider>(hue).value, greaterThan(300));
          await hueDrag.moveTo(
            Offset(hueBounds.left + 32, hueBounds.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 20));
          expect(tester.widget<Slider>(hue).value, greaterThan(300));
          await hueDrag.up();
          await ble.advanceUntil(tester, () {
            final preview = writes.skip(handoffWritesBefore).firstOrNull;
            return preview != null && appliedSince(handoffPackets, preview);
          }, 'firmware acknowledges the held hue preview');
          final green = find.byKey(
            const ValueKey('piece-color-choice:P1_PIECE_WHEEL:85'),
          );
          await tester.ensureVisible(green);
          await tester.pump();
          await tester.tap(green);
          await waitForSheetClosed(green);
          expect(writes.skip(handoffWritesBefore), hasLength(1));
          ble.holdSessionPacket = _settingsState;
          ble.releaseWhere(
            (p) =>
                p.sessionPacket?.code == ConnectionSessionControl.checkpoint &&
                p.sessionPacket?.kind == ConnectionSessionKind.control,
          );
          await ble.advance(tester, 100);
          expect(
            tester
                .widget<InkWell>(
                  find.descendant(of: p1HueRow, matching: find.byType(InkWell)),
                )
                .onTap,
            isNotNull,
            reason:
                'The same-key editor stays usable while its newest intent is '
                'coalesced behind the in-flight acknowledgement.',
          );
          ble.holdSessionPacket = null;
          releaseSettings();
          for (var step = 0; step < 20; ++step) {
            await ble.advance(tester, 100);
          }
          final handoffWrites = writes.skip(handoffWritesBefore).toList();
          expect(handoffWrites, hasLength(2));
          expect(
            handoffWrites.map((w) => w.settingKey),
            everyElement('P1_END_PIECE_WHEEL'),
          );
          expect(handoffWrites.first.settingValue, isNot(85));
          expect(handoffWrites.last.settingValue, 85);
          expect(
            handoffWrites.last.sequence,
            greaterThan(handoffWrites.first.sequence),
          );
          expect(service.latestSettingValue('P1_END_PIECE_WHEEL'), 85);
          expect(service.latestSettingValue('P2_END_PIECE_WHEEL'), 0);
          final greenSwatch = litchWheelPreviewColor(85);
          expect(
            find.descendant(
              of: p1HueRow,
              matching: find.byKey(
                ValueKey('piece-color-swatch:${greenSwatch.toARGB32()}'),
              ),
            ),
            findsOneWidget,
          );
          debugPrint(
            'BULK_PALETTE_HANDOFF mtu=$mtu held-multipoint-drag '
            'preview=${handoffWrites.first.settingValue} final-preset=85 '
            'named-write=once settings-state=confirmed final-ends=85/0',
          );
          await tester.ensureVisible(p1HueRow);
          await tester.pump();
          await captureHealthReview(
            tester,
            'bulk-fixed-end-setup-${Theme.of(tester.element(setup)).platform.name}-mtu-$mtu',
          );

          // The picker resolves named Red to the confirmed nearby custom
          // color, so choosing it should not emit a redundant mutation.
          for (final entry in {
            'P2_END_PIECE_WHEEL': 11,
            'P1_END_PIECE_WHEEL': 235,
          }.entries) {
            await ble.confirmSetting(tester, service, entry.key, entry.value);
          }
          expect(service.latestSettingValue('P1_END_PIECE_WHEEL'), 235);
          expect(service.latestSettingValue('P2_END_PIECE_WHEEL'), 11);
          final revision = service.boardStateSettingsRevision;
          final p1 = find.byKey(const ValueKey('quick:P1_PIECE_WHEEL'));
          await tester.ensureVisible(p1);
          await tester.pump();
          await tester.tap(p1);
          final red = find.byKey(
            const ValueKey('piece-color-choice:P1_PIECE_WHEEL:0'),
          );
          await waitForSheet(red);
          await tester.ensureVisible(red);
          await tester.pump();
          expect(red.hitTestable(), findsOneWidget);
          final writesBeforePicker = writes.length;
          ble.holdSessionPacket = _settingsState;
          await tester.tap(red);
          await waitForSheetClosed(red);
          expect(service.latestSettingValue('P1_END_PIECE_WHEEL'), 235);
          expect(
            writes.skip(writesBeforePicker),
            isEmpty,
            reason: 'A resolved unchanged choice must not write either end.',
          );
          final redWrites = writes
              .skip(writesBeforePicker)
              .where(
                (write) =>
                    write.settingKey == 'P1_END_PIECE_WHEEL' &&
                    write.settingValue == 0,
              );
          expect(
            redWrites,
            isEmpty,
            reason: 'The picker already knows Red resolves to current 235.',
          );
          expect(service.boardStateSettingsRevision, revision);
          debugPrint(
            'BULK_PALETTE_PICKER_NOOP mtu=$mtu physical=235/11 '
            'requested=0 resolved=235 mutation=none revision=$revision',
          );

          // Separately probe the firmware-adjusted Noop through the real
          // acknowledged service API; this is not an action from the picker.
          final heldBeforeNoop = ble.held.length;
          final noopWrite = service.writeSettingAcknowledged(
            deviceId: id,
            key: 'P1_END_PIECE_WHEEL',
            value: 0,
          );
          final noopAck =
              await ble.completeTransaction(tester, noopWrite)
                  as LitchSettingWriteAcknowledged;
          expect(noopAck.outcome.ok, isTrue);
          expect(noopAck.outcome.hasValue, isFalse);
          expect(noopAck.outcome.value, isNull);
          bool? noopConfirmed;
          noopAck.boardStateConfirmation!.then(
            (value) => noopConfirmed = value,
          );
          await tester.pump();
          expect(noopConfirmed, isNull);
          expect(redWrites, hasLength(1));
          expect(priorPalette, isNotEmpty);
          for (final packet in priorPalette) {
            ble.deliver(packet);
          }
          await tester.pump();
          expect(
            noopConfirmed,
            isNull,
            reason: 'The earlier 85/0 pair cannot confirm 235/11.',
          );
          await ble.advanceUntil(
            tester,
            () => heldSettings(skip: heldBeforeNoop).any(
              (state) =>
                  state.settingsRevision == revision &&
                  state.values['P1_END_PIECE_WHEEL'] == 235 &&
                  state.values['P2_END_PIECE_WHEEL'] == 11,
            ),
            'a fresh full Settings State reports the unchanged 235/11 pair',
          );
          expect(noopConfirmed, isNull);
          ble.holdSessionPacket = null;
          releaseSettings();
          // The read receipt settles an adjusted Noop without inventing a
          // revision change or replaying the mutation.
          for (var step = 0; step < 35 && noopConfirmed == null; ++step) {
            await ble.advance(tester, 100);
          }
          expect(noopConfirmed, isTrue);
          expect(redWrites, hasLength(1));
          expect(service.boardStateSettingsRevision, revision);
          final swatch = litchWheelPreviewColor(235);
          expect(
            find.descendant(
              of: p1,
              matching: find.byKey(
                ValueKey('piece-color-swatch:${swatch.toARGB32()}'),
              ),
            ),
            findsOneWidget,
          );
          expect(service.latestSettingValue('P1_END_PIECE_WHEEL'), 235);
          debugPrint(
            'BULK_PALETTE_PROTOCOL_NOOP mtu=$mtu physical=235/11 '
            'requested=0 final=235 receipt=confirmed '
            'revision=$revision unchanged=true',
          );
        } finally {
          ble.holdSessionPacket = null;
          await ble.completeTransaction(tester, service.disconnect());
          await tester.pumpWidget(const SizedBox.shrink());
        }
      },
      variant: TargetPlatformVariant({
        TargetPlatform.iOS,
        TargetPlatform.android,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
