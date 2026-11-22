import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

import 'fixtures/litchcraft_v2_functional_peer.dart';

// Real service/link/document client, with storage revision and mode refusals
// simulated at GATT. This is not firmware, persistence or radio verification.
class _StoragePeer extends LitchcraftV2FunctionalPeer {
  _StoragePeer() {
    active = false;
    document = staticLitchcraftDocument(LitchcraftCanvas.blank());
  }
  Completer<void>? commitAck;
  bool _holdingCommitAck = false;
  int commits = 0;
  int? baseRevision;
  bool rejectNextBegin = false;
  bool dropCommitAck = false;
  bool staticWorkspace = false;

  @override
  void state(int request, {int status = 0}) {
    if (_holdingCommitAck && dropCommitAck) return;
    final gate = commitAck;
    if (_holdingCommitAck && gate != null) {
      unawaited(gate.future.then((_) => super.state(request, status: status)));
      return;
    }
    if (staticWorkspace) {
      event(LitchcraftV2Protocol.state, request, [
        status,
        active ? 1 : 0,
        0,
        0,
        0,
      ]);
    } else {
      super.state(request, status: status);
    }
  }

  void legacy(int kind, int request, List<int> payload) {
    final header = ByteData(12)
      ..setUint8(0, 0x4c)
      ..setUint8(1, kind)
      ..setUint32(2, 42, Endian.little)
      ..setUint32(6, revision, Endian.little)
      ..setUint16(10, request, Endian.little);
    emitDomainPacket([...header.buffer.asUint8List(), ...payload]);
  }

  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (staticWorkspace &&
        value.first == 0x4c &&
        (value[1] == LitchcraftProtocol.inspect ||
            value[1] == LitchcraftProtocol.snapshot)) {
      final request = value[6] | value[7] << 8;
      final metadata = [0, active ? 1 : 0, 3, 0, 0, 0, 15, 0];
      legacy(LitchcraftProtocol.state, request, metadata);
      if (value[1] == LitchcraftProtocol.snapshot) {
        legacy(LitchcraftProtocol.snapshotBegin, request, metadata);
        final colors = document.frames.first.emitters
            .map((e) => e.color)
            .toList();
        for (var offset = 0; offset < colors.length; offset += 2) {
          legacy(LitchcraftProtocol.snapshotChunk, request, [
            offset & 255,
            offset >> 8,
            ...colors.skip(offset).take(2).expand(LitchcraftProtocol.rgb),
          ]);
        }
        legacy(LitchcraftProtocol.snapshotEnd, request, []);
      }
      return;
    }
    if (value.first == LitchcraftV2Protocol.magic) {
      final data = ByteData.sublistView(Uint8List.fromList(value));
      final op = value[2];
      final request = data.getUint16(7, Endian.little);
      if ((op == LitchcraftV2Protocol.replaceBeginOp ||
              op == LitchcraftV2Protocol.streamBegin) &&
          rejectNextBegin) {
        rejectNextBegin = false;
        state(request, status: 1);
        return;
      }
      if ((op == LitchcraftV2Protocol.replaceBeginOp ||
          op == LitchcraftV2Protocol.streamBegin)) {
        baseRevision = data.getUint32(9, Endian.little);
      }
      if ((op == LitchcraftV2Protocol.replaceBeginOp ||
              op == LitchcraftV2Protocol.streamBegin) ||
          (op == LitchcraftV2Protocol.replaceCommitOp ||
              op == LitchcraftV2Protocol.streamCommit)) {
        expectSync(data.getUint32(3, Endian.little), 0);
        if (active || baseRevision != revision) {
          state(request, status: active ? 1 : 9);
          return;
        }
      }
      if ((op == LitchcraftV2Protocol.replaceCommitOp ||
          op == LitchcraftV2Protocol.streamCommit)) {
        commits++;
        staticWorkspace = false;
        _holdingCommitAck = true;
      }
    }
    try {
      await super.write(
        serviceId: serviceId,
        characteristicId: characteristicId,
        value: value,
        withResponse: withResponse,
      );
    } finally {
      _holdingCommitAck = false;
    }
  }
}

Future<void> _pump(WidgetTester tester, [int milliseconds = 30]) async {
  for (var i = 0; i < milliseconds; i++) {
    await tester.pump(const Duration(milliseconds: 1));
    // Native subscription cancellation also needs an actual event-loop turn.
    if (i % 100 == 0) {
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    }
  }
}

Future<T> _finish<T>(WidgetTester tester, Future<T> work) async {
  var done = false;
  work.whenComplete(() => done = true);
  for (var i = 0; i < 3000 && !done; i++) {
    await tester.pump(const Duration(milliseconds: 1));
    // Native subscription cancellation also needs an actual event-loop turn.
    if (i % 100 == 0) {
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    }
  }
  expect(done, isTrue, reason: 'bounded GATT operation');
  return work;
}

void main() {
  late _StoragePeer peer;
  late LitchcraftV2FunctionalService ble;
  late LitchcraftController owner;
  void storageTest(String name, Future<void> Function(WidgetTester) body) {
    testWidgets(name, (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      peer = _StoragePeer();
      ble = LitchcraftV2FunctionalService(peer);
      owner = LitchcraftController(ble);
      try {
        await body(tester);
      } finally {
        owner.dispose();
        await _finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      }
    });
  }

  Future<void> inspect(WidgetTester tester) async {
    final work = owner.adoptActiveBoard();
    expect(await _finish(tester, work), isFalse);
    expect(owner.canSaveArtworkToBoard, isTrue);
  }

  Future<void> paint(int square, int color) async {
    await owner.setBrush(color: color);
    owner.beginStroke();
    owner.paintSquares([square]);
    owner.endStroke();
  }

  storageTest(
    'inactive delivery is automatic and a delayed ACK keeps local edits responsive',
    (tester) async {
      await inspect(tester);
      await _pump(tester, 650);
      expect(peer.commits, 0);
      final library = LitchcraftLibrary(owner);
      await _finish(tester, library.ready);
      addTearDown(library.dispose);
      peer.commitAck = Completer<void>();
      await paint(1, 0xabcdef);
      expect(owner.artworkDeliveryPending, isTrue);
      await _pump(tester, 650);
      expect(owner.artworkDeliveryPending, isTrue);
      expect(owner.artworkDeliveryFinishing, isTrue);
      expect(owner.artworkDeliveryProgress, isNull);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      expect(owner.pending, isFalse);
      expect(peer.active, isFalse);
      await paint(2, 0x123456);
      await paint(3, 0x654321);
      expect(owner.canvas.colors[8], 0x123456);
      expect(owner.canvas.colors[12], 0x654321);
      expect(owner.canUndo, isTrue);
      await _finish(tester, library.save('Latest local artwork'));
      expect(library.artworks.single.canvas.colors[8], 0x123456);
      await _pump(tester, 100);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      peer.commitAck!.complete();
      peer.commitAck = null;
      await _pump(tester, 650);
      expect(peer.commits, 2);
      expect(owner.artworkDeliveryPending, isFalse);
      expect(owner.artworkDeliveryFinishing, isFalse);
      expect(
        sameLitchcraftDocument(peer.document, owner.documentSession.document),
        isTrue,
      );
      expect(owner.status, LitchcraftStatus.offline);
      expect(peer.active, isFalse);
      expect(peer.writes.where((p) => p.first == 0x4c), isEmpty);
    },
  );

  storageTest(
    'deliberate Open replaces inactive stored art without admission or conflict',
    (tester) async {
      peer.paint(4, 0x444444);
      await inspect(tester);
      final next = staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint([8], 15, 0xabcdef),
      );
      await owner.replaceDocument(next);
      await _pump(tester, 650);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      expect(sameLitchcraftDocument(peer.document, next), isTrue);
      expect(owner.status, isNot(LitchcraftStatus.conflict));
      expect(peer.active, isFalse);
    },
  );

  for (final physicalSquare in [1, 2]) {
    final overlapping = physicalSquare == 1;
    storageTest(
      overlapping
          ? 'reconnect preserves overlapping artwork until explicit choice'
          : 'reconnect silently converges disjoint physical and local artwork',
      (tester) async {
        await inspect(tester);
        await _pump(tester, 650);
        await paint(1, 0xabcdef);
        final connection = ble.connectionSessionToken;
        await _finish(tester, ble.disconnect());
        expect(ble.isConnected, isFalse);
        expect(ble.connectionSessionToken, isNot(connection));
        peer.paint(physicalSquare, 0x123456);
        peer.revision++;
        final local = owner.documentSession.document;
        final physical = peer.document;
        final statuses = <LitchcraftStatus>[];
        owner.addListener(() => statuses.add(owner.status));
        ble.debugSetConnectedDevice('board');
        await inspect(tester);
        await _pump(tester, 650);
        if (overlapping) {
          expect(owner.status, LitchcraftStatus.conflict);
          expect(peer.commits, 0);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, local),
            isTrue,
          );
          expect(sameLitchcraftDocument(peer.document, physical), isTrue);
          await _finish(tester, owner.resolveConflict(useBoard: false));
          expect(sameLitchcraftDocument(peer.document, local), isTrue);
        } else {
          final expected = staticLitchcraftDocument(
            LitchcraftCanvas.blank()
                .paint([1], 15, 0xabcdef)
                .paint([2], 15, 0x123456),
            favoriteColors: local.favoriteColors,
          );
          expect(statuses, isNot(contains(LitchcraftStatus.conflict)));
          expect(owner.status, LitchcraftStatus.offline);
          expect(peer.commits, 1);
          expect(owner.artworkDeliveryPending, isFalse);
          expect(owner.artworkDeliveryFinishing, isFalse);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, expected),
            isTrue,
            reason:
                'The local draft must retain both complete paint footprints.',
          );
          expect(
            sameLitchcraftDocument(peer.document, expected),
            isTrue,
            reason:
                'The board must receive the merged document, not just clear a status.',
          );
        }
        expect(peer.active, isFalse);
        expect(
          peer.writes.where(
            (p) =>
                p[0] == LitchcraftProtocol.magic &&
                p[1] == LitchcraftProtocol.enter,
          ),
          isEmpty,
        );
      },
    );
  }

  storageTest('physical entry during debounce prevents inactive replacement', (
    tester,
  ) async {
    await inspect(tester);
    await paint(1, 0xabcdef);
    peer.active = true;
    final physical = peer.document;
    await _pump(tester, 650);
    expect(peer.commits, 0);
    expect(peer.active, isTrue);
    expect(sameLitchcraftDocument(peer.document, physical), isTrue);
    expect(owner.canvas.colors[4], 0xabcdef);
  });

  storageTest(
    'opening a newer source retires delayed ACK and delivers only current intent next',
    (tester) async {
      await inspect(tester);
      await _pump(tester, 650);
      peer.commitAck = Completer<void>();
      await paint(1, 0xabcdef);
      await _pump(tester, 650);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      final next = staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint([8], 15, 0x123456),
      );
      await owner.replaceDocument(next);
      peer.commitAck!.complete();
      peer.commitAck = null;
      await _pump(tester, 700);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, next),
        isTrue,
      );
      expect(sameLitchcraftDocument(peer.document, next), isTrue);
      expect(peer.commits, 2);
      expect(owner.pending, isFalse);
    },
  );
  storageTest(
    'inactive rejected delivery retries the unchanged document after channel repair',
    (tester) async {
      await inspect(tester);
      await _pump(tester, 650);
      peer.rejectNextBegin = true;
      await paint(1, 0xabcdef);
      await _pump(tester, 2400);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      expect(
        sameLitchcraftDocument(peer.document, owner.documentSession.document),
        isTrue,
      );
      expect(peer.active, isFalse);
      expect(owner.status, isNot(LitchcraftStatus.error));
    },
  );

  storageTest(
    'unacknowledged accepted upload is a reconnect ancestor for newer local edits',
    (tester) async {
      await inspect(tester);
      await _pump(tester, 650);
      peer.dropCommitAck = true;
      await paint(1, 0xabcdef);
      await _pump(tester, 650);
      expect(
        peer.commits,
        1,
        reason:
            '${owner.status} ${owner.errorMessage} writes=${peer.writes.map((p) => p[2]).toList()}',
      );
      await paint(2, 0x123456);
      peer.dropCommitAck = false;
      await _pump(tester, 2700);
      expect(peer.commits, 2);
      expect(
        sameLitchcraftDocument(peer.document, owner.documentSession.document),
        isTrue,
      );
      expect(owner.status, isNot(LitchcraftStatus.conflict));
      expect(peer.active, isFalse);
    },
  );
  storageTest(
    'gameActive admission refusal still automatically stores locally painted artwork',
    (tester) async {
      peer.enterRejection = 1;
      await _finish(tester, owner.enter());
      expect(owner.status, LitchcraftStatus.gameActive);
      await paint(9, 0xabcdef);
      await _pump(tester, 650);
      expect(peer.commits, 1);
      expect(owner.status, LitchcraftStatus.gameActive);
      expect(peer.active, isFalse);
      expect(peer.selectedFrame, 0);
      expect(
        sameLitchcraftDocument(peer.document, owner.documentSession.document),
        isTrue,
      );
    },
  );
  for (final useBoard in [true, false]) {
    storageTest(
      'schema0 inactive conflict resolves safely: useBoard=$useBoard',
      (tester) async {
        peer.staticWorkspace = true;
        peer.paint(4, 0x444444);
        final board = peer.document;
        owner.restoreDocument(
          staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([8], 15, 0xabcdef),
          ),
        );
        final local = owner.documentSession.document;
        await inspect(tester);
        await _pump(tester, 650);
        expect(owner.status, LitchcraftStatus.conflict);
        expect(peer.commits, 0);
        await _finish(tester, owner.resolveConflict(useBoard: useBoard));
        await _pump(tester, 650);
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.commits, useBoard ? 0 : 1);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            useBoard ? board : local,
          ),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(peer.document, useBoard ? board : local),
          isTrue,
        );
        expect(peer.active, isFalse);
      },
    );
  }
}
