import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

void main() {
  test(
    'reopening Setup reuses descriptors with current shared values',
    () async {
      final peer = SettingsSessionBoard();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: peer,
      );
      addTearDown(() => disposeBoard(service, peer));
      await connectBoard(service, peer, deviceId: 'board');
      expect(await service.ensureProductReady('board'), isTrue);
      await service.readSettings('board');
      final reads = peer.enumerationReads;
      peer.publishSettings();
      expect(service.hasBoardStateSnapshot, isTrue);
      final settings = await service.readSettings('board');
      expect(
        settings
            .firstWhere((setting) => setting.key == 'GAME_FIRST_PLAYER')
            .value,
        2,
      );
      expect(
        settings.firstWhere((setting) => setting.key == 'P1_PIECE_WHEEL').value,
        40,
      );
      expect(
        peer.enumerationReads,
        reads,
        reason: 'Setup reuses confirmed immutable descriptors.',
      );

      peer
        ..settingsRevision = 15
        ..firstPlayer = 1
        ..wheel = 88;
      peer.publishSettings();
      final refreshed = await service.readSettings('board');
      expect(
        refreshed
            .firstWhere((setting) => setting.key == 'P1_PIECE_WHEEL')
            .value,
        88,
      );
      expect(peer.enumerationReads, reads);
      await service.disconnect();
      await connectBoard(service, peer, deviceId: 'board');
      await service.readSettings('board');
      expect(
        peer.enumerationReads,
        reads + 1,
        reason: 'A fresh connection establishes descriptor authority again.',
      );
    },
  );
}
