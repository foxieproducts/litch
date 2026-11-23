import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

class _Permissions implements BlePermissionGateway {
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      PermissionStatus.granted;
  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async => {
    for (final permission in permissions) permission: PermissionStatus.granted,
  };
  @override
  Future<bool> openSettings() async => true;
}

// External SDK discovery only; pairing replies reuse the existing board seam.
// Durable Adding rollback/Active commit and physical X+O are board facts supplied
// here, not replacement persistence, security or physical-input implementations.
class _Board extends OwnerPairingBoard {
  final scan = StreamController<DiscoveredDevice>.broadcast(sync: true);
  @override
  Stream<BleStatus> get statusStream => Stream.value(BleStatus.ready);
  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) => scan.stream;
  void advertise() => scan.add(
    DiscoveredDevice(
      id: 'board-1',
      name: 'Foxie Litch',
      serviceUuids: [BleProtocol.baseService],
      serviceData: const {},
      manufacturerData: Uint8List(0),
      rssi: -40,
      connectable: Connectable.available,
    ),
  );
  void connection(int index, DeviceConnectionState state) =>
      connections[index].add(
        ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: state,
          failure: null,
        ),
      );
}

Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var turn = 0; turn < 100 && !done(); turn++) {
    await tester.pump(const Duration(milliseconds: 20));
    await tester.runAsync(() async {});
  }
  expect(done(), isTrue);
}

void main() {
  for (final activeCommitted in [false, true]) {
    testWidgets(
      'iOS second-phone reset ${activeCommitted ? 'after Active commit' : 'before Active commit'} keeps bounded retry and preserves claim fencing',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
        SharedPreferences.setMockInitialValues({
          'last_connected_board_id': 'board-1',
        });
        final board = _Board()
          ..state = 6
          ..flags = 13
          ..reason = 8;
        final service = LitchBleService(
          ble: board,
          permissionGateway: _Permissions(),
          pendingOtaStore: MemoryOtaTargetStore(),
          scanClock: tester.binding.clock.now,
        );
        try {
          await tester.pumpWidget(
            MaterialApp(home: ConnectionGateScreen(bleService: service)),
          );
          await _until(tester, () => board.scan.hasListener);
          board.advertise();
          await _until(tester, () => board.connections.isNotEmpty);
          board.connection(0, DeviceConnectionState.connected);
          await _until(tester, () => board.pairingCommands.length == 1);
          expect(
            board.pairingCommands.single.body[1],
            1,
            reason:
                'iOS uses board-initiated Begin, never Android BeginPhoneSecurity.',
          );
          expect(service.ownerPairingStatus?.ownerRetained, isTrue);
          // Existing external-board seam publishes physical-consent acceptance.
          board.setState(3, nextFlags: 5);
          await _until(
            tester,
            () =>
                service.ownerPairingStatus?.state ==
                OwnerPairingSessionState.securing,
          );
          expect(service.productReady, isFalse);
          final retiredToken = service.connectionSessionToken;
          final identity = service.durableBoardIdentity;
          expect(identity, isNotNull);
          ++board.bootNonce;
          board.connection(0, DeviceConnectionState.disconnected);
          // Reboot after journal rollback or after Active commit, before its Ready
          // publication reached the phone. This does not assert native key custody.
          board.state = activeCommitted ? 5 : 6;
          board.flags = activeCommitted ? 7 : 13;
          board.reason = activeCommitted ? 0 : 8;
          board.attempt = 0;
          board.physicalEnd = 0;
          ++board.revision;
          await _until(
            tester,
            () => board.scan.hasListener && !service.isConnected,
          );
          await tester.pump(const Duration(seconds: 2));
          expect(find.text('Connecting to Foxie Litch...'), findsNothing);
          expect(
            find.descendant(
              of: find.byKey(const ValueKey('connection-progress-overlay')),
              matching: find.textContaining(
                'Connection ended before setup finished.',
              ),
            ),
            findsOneWidget,
          );
          expect(service.productReady, isFalse);
          expect(service.connectionSessionToken, isNot(retiredToken));
          board.advertise();
          await tester.pump(const Duration(seconds: 1));
          expect(
            board.connections,
            hasLength(1),
            reason:
                'An advertisement does not bypass the selected-board retry interval.',
          );
          expect(board.pairingCommands, hasLength(1));
          expect(
            find.byKey(const ValueKey('chosen-board-cancel')).hitTestable(),
            findsOneWidget,
          );
          // The visible selected-board journey retries its link. It must not
          // grant a new pairing claim just because the native link returns.
          await _until(tester, () => board.connections.length == 2);
          board.connection(1, DeviceConnectionState.connected);
          await _until(tester, () => board.sessionActivated);
          if (activeCommitted) {
            await _until(tester, () => service.productReady);
            expect(
              board.pairingCommands,
              hasLength(1),
              reason:
                  'Durable Active admits quiet current-link proof without a new claim.',
            );
          } else {
            await _until(
              tester,
              () =>
                  !service.ownerPairingBusy &&
                  service.ownerPairingStatus?.canBegin == true,
            );
            expect(
              service.ownerPairingStatus?.ownerRetained,
              isTrue,
              reason:
                  'Board-authoritative rollback retains the prior owner. Physical key preservation is a native/HIL gate.',
            );
            expect(service.productReady, isFalse);
            expect(service.durableBoardIdentity, identity);
            expect(board.pairingCommands, hasLength(1));
            final retry = find.byKey(const ValueKey('owner-pairing-retry'));
            expect(retry, findsOneWidget);
            await tester.tap(retry);
            await _until(tester, () => board.pairingCommands.length == 2);
            expect(board.pairingCommands.last.body[1], 1);
            board.setState(3, nextFlags: 5);
            await tester.pump();
            expect(service.productReady, isFalse);
            board.setState(5, nextFlags: 7);
            await _until(tester, () => service.productReady);
          }
          await tester.pump();
          await tester.runAsync(() async {});
          await tester.pump();
          expect(service.durableBoardIdentity, identity);
          expect(find.text('Connecting to Foxie Litch...'), findsNothing);
          expect(service.connectedId, 'board-1');
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          var disposed = false;
          final disposal = service.dispose().whenComplete(
            () => disposed = true,
          );
          await _until(tester, () => disposed);
          await disposal;
          await tester.runAsync(() async {
            await disposeBoard(service, board);
            await board.scan.close();
          });
          debugDefaultTargetPlatformOverride = null;
        }
      },
      timeout: const Timeout(Duration(seconds: 20)),
    );
  }
}
