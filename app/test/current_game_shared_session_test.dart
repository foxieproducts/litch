import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/connection_game_history_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/current_game_pull_client.dart';
import 'package:litch_mobile/src/services/current_game_receive_store.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_history_pull_client.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const _deviceId = 'virtual-current-game';

// Only SDK delivery is delayed/lost. C++ owns admission and retained reverse
// packets. Its history fixture emits CurrentState only; cancel at that frontier.
// current_game_pull_client_test covers the complete durable archive transaction.
class _DelayedHistoryBle extends VirtualLitchGattBle {
  _DelayedHistoryBle(super.native);
  int objectToken = 0;
  bool holdAdmission = true, holdState = true;
  final heldState = <VirtualPacket>[];

  List<ConnectionSessionPacket> get sentPackets => writes
      .where((write) => write['channel'] == 'connection_session')
      .map(
        (write) => ConnectionSessionCodec.decode(
          Uint8List.fromList(write['bytes'] as List<int>),
        ),
      )
      .whereType<ConnectionSessionPacket>()
      .toList();

  List<ConnectionSessionPacket> get begins => sentPackets
      .where(
        (packet) =>
            packet.kind == ConnectionSessionKind.object &&
            packet.code == ConnectionGameHistoryCodec.objectOpcode,
      )
      .toList();

  bool get queriedAdmission => sentPackets.any(
    (packet) =>
        packet.kind == ConnectionSessionKind.control &&
        packet.code == ConnectionSessionControl.query &&
        packet.token == objectToken &&
        packet.sequence == 0,
  );

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final value = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (value?.kind == ConnectionSessionKind.control &&
        value?.code == ConnectionSessionControl.progress &&
        value?.token == objectToken &&
        holdAdmission) {
      if (!queriedAdmission) return;
      holdAdmission = false;
    }
    if (holdState &&
        value?.kind == ConnectionSessionKind.object &&
        value?.code == ConnectionGameHistoryCodec.receiptCode) {
      heldState.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }

  void releaseOriginalState() {
    holdState = false;
    final originals = List<VirtualPacket>.of(heldState);
    heldState.clear();
    for (final packet in originals) {
      deliver(packet);
    }
  }
}

// The public production service endpoints used by PhysicalGameHistoryIngestion.
class _HistoryTransport implements GameHistoryTransport {
  _HistoryTransport(this.service);
  final LitchBleService service;
  final _packets = StreamController<Uint8List>.broadcast(sync: true);
  StreamSubscription<List<int>>? _subscription;
  int statesReceived = 0;
  @override
  Stream<Uint8List> get notifications => _packets.stream;
  @override
  Future<void> subscribe() async {
    _subscription = service.gameHistoryPackets(_deviceId).listen((bytes) {
      final frame = GameHistoryFrame.decode(Uint8List.fromList(bytes));
      if (frame.opcode == CurrentGameSyncOpcode.state.wireValue) {
        ++statesReceived;
      }
      _packets.add(Uint8List.fromList(bytes));
    }, onError: _packets.addError);
  }

  @override
  Future<void> write(Uint8List bytes) =>
      service.writeGameHistoryPacket(_deviceId, bytes);
  @override
  Future<void> unsubscribe() async {
    await _subscription?.cancel();
    _subscription = null;
    if (!_packets.isClosed) await _packets.close();
  }
}

// Storage is deliberately not reached by this State-only peer. Real durable
// receive/cursor/archive behavior is covered by current_game_pull_client_test.
class _NoPrefixReceiveStore implements CurrentGameReceiveStore {
  int loadCount = 0;
  @override
  Future<CurrentGameReceiveCursor?> loadCursor(String boardIdentity) async {
    ++loadCount;
    return null;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => throw StateError(
    'State-only shared test unexpectedly reached durable storage',
  );
}

Future<void> _until(VirtualLitchBle ble, bool Function() done) async {
  final clock = Stopwatch()..start();
  while (!done() && clock.elapsed < const Duration(seconds: 5)) {
    if (ble.hasPendingOperations) {
      await ble.flush(null, maxOperations: 1);
    } else {
      await ble.command(null, {
        'op': 'advance',
        'ms': 10,
      }, flushOperations: false);
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
  }
  expect(done(), isTrue, reason: 'Current-game shared frontier did not settle');
}

Future<T> _finish<T>(VirtualLitchBle ble, Future<T> work) async {
  var done = false;
  T? result;
  Object? failure;
  work.then<void>(
    (value) {
      result = value;
      done = true;
    },
    onError: (Object error, StackTrace _) {
      failure = error;
      done = true;
    },
  );
  await _until(ble, () => done);
  if (failure != null) throw failure!;
  return result as T;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final loseFirstState in [false, true]) {
    test('shared current-game Begin stays unique after admission with '
        '${loseFirstState ? 'lost' : 'delayed'} State', () async {
      SharedPreferences.setMockInitialValues({});
      final native = await VirtualLitchProcess.start(
        1,
        false,
        hardwareV3: true,
      );
      final ble = _DelayedHistoryBle(native);
      final service = LitchBleService(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      final receive = _NoPrefixReceiveStore();
      final transport = _HistoryTransport(service);
      CurrentGamePullClient? client;
      Future<void>? outcome;
      var pullDone = false;
      Object? pullFailure;
      var beginCountAfterDelay = 0;
      try {
        expect(await _finish(ble, service.connect(_deviceId)), isTrue);
        final session = await _finish(
          ble,
          service.openConnectionSession(_deviceId),
        );
        expect(session?.isActivated, isTrue);
        ble.objectToken = session!.objectStreamToken;
        client = CurrentGamePullClient(
          boardIdentity: _deviceId,
          transport: transport,
          receiveStore: receive,
          isOwnerReady: () => service.isConnected,
          nextTransactionId: () => 41,
        );
        outcome = client.pull().then<void>(
          (_) {
            pullDone = true;
          },
          onError: (Object error, StackTrace _) {
            pullFailure = error;
            pullDone = true;
          },
        );
        await _until(
          ble,
          () =>
              ble.queriedAdmission &&
              !ble.holdAdmission &&
              ble.heldState.isNotEmpty,
        );
        expect(ble.begins, hasLength(1));
        await Future<void>.delayed(const Duration(milliseconds: 350));
        await ble.flush(null);
        beginCountAfterDelay = ble.begins.length;
        final original = ble.heldState.first;
        final originalEnvelope = ConnectionSessionCodec.decode(
          Uint8List.fromList(original.bytes),
        )!;
        if (loseFirstState) {
          // Drop only this packet. The native reverse owner must offer the
          // retained same sequence again, without another domain Begin.
          ble.heldState.clear();
          ble.holdState = false;
        } else {
          ble.releaseOriginalState();
        }
        await _until(ble, () => transport.statesReceived >= 1);
        expect(service.isConnected, isTrue);
        if (loseFirstState) {
          expect(
            ble.emitted.where((packet) {
              final value = ConnectionSessionCodec.decode(
                Uint8List.fromList(packet.bytes),
              );
              return packet.channel == 'connection_session' &&
                  value?.kind == ConnectionSessionKind.object &&
                  value?.token == originalEnvelope.token &&
                  value?.sequence == originalEnvelope.sequence;
            }).length,
            greaterThan(1),
          );
        }
        expect(receive.loadCount, 1);
      } finally {
        if (client != null) await _finish(ble, client.cancel());
        if (outcome != null) {
          await _until(ble, () => pullDone);
          await outcome;
          expect(pullFailure, isA<StateError>());
        }
        await _finish(ble, service.disconnect());
        await _finish(ble, service.dispose());
        await ble.flush(null);
        await ble.close();
        await native.close();
      }
      expect(
        beginCountAfterDelay,
        1,
        reason: 'An admitted Begin must wait for its semantic receipt',
      );
    }, skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null);
  }
}
