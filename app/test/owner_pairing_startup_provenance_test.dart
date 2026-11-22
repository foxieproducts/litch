import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart'
    show ConnectionStateUpdate, DeviceConnectionState;
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/services/connection_game_history_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_ota_status_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

class _PreOwnerBoard extends OwnerPairingBoard {
  static const _publicSupportObjectOpcode = 0x45;
  final forbiddenOpcodes = <int>[];
  final admittedObjectSequences = <int>[];
  final commands = <String>[];
  final assembler = ConnectionOtaStatusAssembler();
  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.object) {
      // Normal shipping firmware admits only public Support before owner proof.
      // Ordered consent and State bootstrap use separate admission rules.
      final ownerReady = flags & 2 != 0 && flags & 4 != 0;
      if (!ownerReady && packet.code != _publicSupportObjectOpcode) {
        forbiddenOpcodes.add(packet.code);
        if (packet.code == ConnectionOtaCodec.controlObjectOpcode) {
          final bytes = assembler.accept(packet.body);
          if (bytes != null) commands.add(utf8.decode(bytes));
        }
        return;
      }
      admittedObjectSequences.add(packet.sequence);
    }
    await super.sharedWrite(value);
  }
}

void main() {
  for (final outcome in ['ready', 'cancel', 'drop']) {
    testWidgets('startup provenance preserves consent then $outcome', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues({});
      PackageInfo.setMockInitialValues(
        appName: 'Litch',
        packageName: 'litch.test',
        version: '0.9.0',
        buildNumber: '13',
        buildSignature: 'test',
      );
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final board = _PreOwnerBoard()..capabilityReplyGate = Completer<void>();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      try {
        await tester.runAsync(() => connectBoard(service, board));
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        for (var i = 0; i < 30; i++) {
          await tester.pump(const Duration(milliseconds: 20));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        expect(
          find.byType(FirmwareScreen, skipOffstage: false),
          findsOneWidget,
          reason:
              'actual retained firmware screen must start before capability discovery',
        );
        board.capabilityReplyGate!.complete();
        for (var i = 0; i < 50; i++) {
          await tester.pump(const Duration(milliseconds: 20));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        expect(
          service.ownerPairingStatus?.state,
          OwnerPairingSessionState.awaitPhysical,
        );
        expect(
          find.byKey(const ValueKey('owner-pairing-gate')),
          findsOneWidget,
        );
        expect(find.text('X + O'), findsOneWidget);
        final token = service.connectionSessionToken;
        for (var i = 0; i < 24; i++) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 100)),
          );
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(
          board.forbiddenOpcodes,
          isEmpty,
          reason:
              'no private Object may interrupt consent; OTA commands: ${board.commands}',
        );
        expect(service.isConnected, isTrue);
        expect(service.connectionSessionToken, token);
        if (outcome != 'cancel') board.setState(3, nextFlags: 4);
        for (var i = 0; i < 24; i++) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 100)),
          );
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(
          board.forbiddenOpcodes,
          isEmpty,
          reason:
              'private Objects during consent; OTA commands: ${board.commands}',
        );
        expect(service.isConnected, isTrue);
        expect(service.connectionSessionToken, token);
        if (outcome == 'ready') {
          final session = await service.openConnectionSession('board-1');
          expect(session, isNotNull);
          expect(
            () => session!.sendObject(
              ConnectionGameHistoryCodec.objectOpcode,
              Uint8List.fromList([1]),
            ),
            throwsA(
              isA<StateError>().having(
                (error) => error.message,
                'message',
                'Connection-session object admission is unavailable',
              ),
            ),
            reason: 'common admission also protects the game-history domain',
          );
          board.setState(5, nextFlags: 7);
          for (var i = 0; i < 80 && !service.productReady; i++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 10)),
            );
          }
          expect(service.productReady, isTrue);
          for (var i = 0; i < 20 &&
              find.byKey(const ValueKey<String>('connected-board-status-action')).evaluate().isEmpty; i++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(() async {});
          }
          expect(board.digestRequests, 0,
              reason: 'the offstage Update card does not read private provenance');
          await tester.tap(
            find.byKey(const ValueKey<String>('connected-board-status-action')),
          );
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(
            find.descendant(
              of: find.byKey(const ValueKey('connected-board-popover')),
              matching: find.text('Check for updates'),
            ),
          );
          for (var i = 0; i < 80 && board.digestRequests == 0; i++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 10)),
            );
          }
          expect(
            board.admittedObjectSequences.first,
            1,
            reason: 'refused pre-owner submission must not consume a sequence',
          );
          expect(
            board.digestRequests,
            greaterThan(0),
            reason: 'opening Update reads provenance after owner readiness',
          );
        } else {
          if (outcome == 'cancel') {
            await tester.runAsync(() => service.cancelOwnerPairing());
          }
          await tester.pumpWidget(const SizedBox.shrink());
          if (outcome == 'drop') {
            board.connections.last.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.disconnected,
                failure: null,
              ),
            );
          } else {
            await tester.runAsync(() => service.disconnect());
          }
          for (var i = 0; i < 10; i++) {
            await tester.pump(const Duration(milliseconds: 20));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          expect(board.digestRequests, 0);
          expect(service.connectionSessionToken, isNot(token));
        }
        expect(
          board.forbiddenOpcodes,
          isEmpty,
          reason:
              'private Objects before owner proof; OTA commands: ${board.commands}',
        );
      } finally {
        if (!board.capabilityReplyGate!.isCompleted) {
          board.capabilityReplyGate!.complete();
        }
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
        debugDefaultTargetPlatformOverride = null;
      }
    });
  }
}
