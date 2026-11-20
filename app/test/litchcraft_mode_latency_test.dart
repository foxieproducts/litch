import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'fixtures/litchcraft_v2_functional_peer.dart';

class _ScheduledPeer extends LitchcraftV2FunctionalPeer {
  int? holdReceiptForOperation, heldRequest;
  final receiptGate = Completer<void>();
  bool receiptInFlight = false;
  _ScheduledPeer() {
    beforeDomainReceipt = (bytes) {
      if (bytes.first != LitchcraftV2Protocol.magic ||
          bytes[2] != LitchcraftV2Protocol.state ||
          (bytes[11] | bytes[12] << 8) != heldRequest) {
        return null;
      }
      receiptInFlight = true;
      return receiptGate.future;
    };
  }

  @override
  void respond(List<int> bytes) {
    if (bytes.first == LitchcraftV2Protocol.magic &&
        bytes[2] == holdReceiptForOperation) {
      heldRequest = bytes[7] | bytes[8] << 8;
      holdReceiptForOperation = null;
    }
    super.respond(bytes);
  }

  DateTime Function()? now;
  DateTime? modeReceived;
  Completer<void>? toolGate;
  bool toolInFlight = false;
  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (value[0] == 0x4c &&
        (value[1] == LitchcraftProtocol.enter ||
            value[1] == LitchcraftProtocol.exit)) {
      modeReceived = now!();
    }
    if (value[0] == 0x4c &&
        value[1] == LitchcraftProtocol.tool &&
        toolGate != null) {
      toolInFlight = true;
      await toolGate!.future;
    }
    await super.write(
      serviceId: serviceId,
      characteristicId: characteristicId,
      value: value,
      withResponse: withResponse,
    );
  }
}

Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var step = 0; step < 1000 && !done(); step++) {
    await tester.pump(const Duration(milliseconds: 10));
  }
  expect(
    done(),
    isTrue,
    reason: 'operation must finish within 10s virtual time',
  );
}

class _UnknownBoardService extends LitchcraftV2FunctionalService {
  _UnknownBoardService(super.peer);
  final capabilitiesReady = Completer<LitchDeviceCapabilities>();
  @override
  LitchDeviceCapabilities deviceCapabilities(String id) =>
      capabilitiesReady.isCompleted
      ? super.deviceCapabilities(id)
      : LitchDeviceCapabilities.unavailable;
  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) => capabilitiesReady.future;
  void finishCapabilities() =>
      capabilitiesReady.complete(super.deviceCapabilities('board'));
}

void main() {
  testWidgets(
    'scripted domain Enter can precede its delayed feature projection',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      final peer = _ScheduledPeer()
        ..active = false
        ..now = tester.binding.clock.now;
      final service = _UnknownBoardService(peer);
      final controller = LitchcraftController(service);
      final start = tester.binding.clock.now();
      var entered = false;
      unawaited(controller.enter().then((_) => entered = true));
      await tester.pump(const Duration(milliseconds: 20));
      final prompt = peer.active;
      service.finishCapabilities();
      await _until(tester, () => entered);
      controller.dispose();
      await service.dispose();
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
      expect(
        prompt,
        isTrue,
        reason: 'resolved control must admit mode before OTA metadata',
      );
      expect(peer.modeReceived!.difference(start).inMilliseconds, lessThan(50));
    },
  );

  testWidgets('post-ACK Exit does not wait for held artwork preparation', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    SharedPreferences.setMockInitialValues({});
    final gate = Completer<void>();
    final peer = _ScheduledPeer()
      ..active = false
      ..now = tester.binding.clock.now;
    final service = LitchcraftV2FunctionalService(peer);
    var ready = false;
    unawaited(
      service.litchcraftControlAvailable('board').then((ok) => ready = ok),
    );
    await _until(tester, () => ready);
    var preparingArtwork = false;
    service.beforeArtworkPacketSize = () {
      preparingArtwork = true;
      return gate.future;
    };
    final controller = LitchcraftController(service);
    var entered = false;
    unawaited(controller.enter().then((_) => entered = true));
    await _until(tester, () => preparingArtwork);
    expect(peer.active, isTrue);
    peer.modeReceived = null;
    final start = tester.binding.clock.now();
    var left = false;
    unawaited(controller.leave().then((_) => left = true));
    await tester.pump(const Duration(milliseconds: 20));
    final prompt = !peer.active;
    gate.complete();
    await _until(tester, () => entered && left);
    controller.dispose();
    await service.dispose();
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
    expect(prompt, isTrue);
    expect(peer.modeReceived!.difference(start).inMilliseconds, lessThan(50));
  });

  testWidgets(
    'latest brush intent survives document replacement during old native completion',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      final peer = _ScheduledPeer()..now = tester.binding.clock.now;
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      var entered = false;
      unawaited(controller.enter().then((_) => entered = true));
      await _until(tester, () => entered);
      peer.toolGate = Completer<void>();
      var first = false, latest = false;
      unawaited(controller.setBrush(eraser: true).then((_) => first = true));
      await _until(tester, () => peer.toolInFlight);
      final source = LitchcraftDocumentSession(
        controller.documentSession.document,
      )..paintSelectedFrame(squares: [9], mask: 15, color: 0xabcdef);
      controller.restoreDocument(source.document);
      unawaited(
        controller
            .setBrush(eraser: false, color: 0x123456)
            .then((_) => latest = true),
      );
      peer.toolGate!.complete();
      await _until(tester, () => first && latest);
      expect(peer.brushErase, isFalse);
      expect(peer.brushColor, 0x123456);
      expect(controller.eraser, isFalse);
      expect(controller.color, 0x123456);
      controller.dispose();
      await service.dispose();
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'rapid color swaps remain two-color optimistic under a slow ACK',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      final peer = _ScheduledPeer()..now = tester.binding.clock.now;
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      var entered = false;
      unawaited(controller.enter().then((_) => entered = true));
      await _until(tester, () => entered);
      await controller.setBrush(color: 0x123456);
      await controller.setBrush(color: 0xabcdef);

      peer.toolGate = Completer<void>();
      var first = false, second = false;
      unawaited(controller.swapBrushColor().then((_) => first = true));
      await _until(tester, () => peer.toolInFlight);
      unawaited(controller.swapBrushColor().then((_) => second = true));
      expect(controller.color, 0xabcdef);
      expect(controller.previousColor, 0x123456);
      expect(controller.color, isNot(controller.previousColor));

      peer.toolGate!.complete();
      await _until(tester, () => first && second);
      expect(peer.brushColor, 0xabcdef);
      expect(controller.color, 0xabcdef);
      expect(controller.previousColor, 0x123456);
      controller.dispose();
      await service.dispose();
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'late local Tool echoes never overwrite a newer optimistic color swap',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      final peer = _ScheduledPeer()..now = tester.binding.clock.now;
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      var entered = false;
      unawaited(controller.enter().then((_) => entered = true));
      await _until(tester, () => entered);
      await controller.setBrush(color: 0x123456);
      await controller.setBrush(color: 0xabcdef);

      final gate = Completer<void>();
      peer.brushEventGate = gate.future;
      var sawIdenticalSlots = false;
      void observe() {
        if (controller.previousColor == controller.color) {
          sawIdenticalSlots = true;
        }
      }

      controller.addListener(observe);
      await controller.swapBrushColor();
      await controller.swapBrushColor();
      expect(controller.color, 0xabcdef);
      expect(controller.previousColor, 0x123456);
      gate.complete();
      await tester.pump();
      expect(sawIdenticalSlots, isFalse);
      expect(controller.color, 0xabcdef);
      expect(controller.previousColor, 0x123456);
      controller.removeListener(observe);
      controller.dispose();
      await service.dispose();
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'Erase immediately followed by Play retains the local tool choice',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues({});
      final peer = _ScheduledPeer()..now = tester.binding.clock.now;
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      var entered = false;
      unawaited(controller.enter().then((_) => entered = true));
      await _until(tester, () => entered);
      var selected = false, left = false;
      unawaited(controller.setBrush(eraser: true).then((_) => selected = true));
      unawaited(controller.leave().then((_) => left = true));
      await _until(tester, () => selected && left);
      expect(controller.boardActive, isFalse);
      expect(controller.eraser, isTrue);
      controller.dispose();
      await service.dispose();
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final bulk in [true, false]) {
    testWidgets(
      'Exit latency while ${bulk ? "bulk" : "small"} replacement is pending',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.android;
        SharedPreferences.setMockInitialValues({});
        final peer = _ScheduledPeer()
          ..supportsBulkDocument = bulk
          ..now = tester.binding.clock.now;
        final service = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(service);
        var entered = false;
        unawaited(controller.enter().then((_) => entered = true));
        await _until(tester, () => entered);
        final source = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
        peer.holdReceiptForOperation = bulk
            ? LitchcraftV2Protocol.streamCommit
            : LitchcraftV2Protocol.replaceChunkOp;
        var delivered = false;
        unawaited(
          controller
              .replaceDocument(source.document)
              .then((_) => delivered = true),
        );
        await _until(tester, () => peer.receiptInFlight);
        expect(delivered, isFalse);
        peer.modeReceived = null;
        final start = tester.binding.clock.now();
        var left = false;
        unawaited(controller.leave().then((_) => left = true));
        await _until(tester, () => peer.modeReceived != null);
        final latency = peer.modeReceived!.difference(start).inMilliseconds;
        expect(peer.active, isFalse);
        peer.receiptGate.complete();
        await _until(tester, () => left && delivered);
        debugPrint(
          'MODE_LATENCY transport=${bulk ? "bulk" : "small"} exit_wire_ms=$latency',
        );
        controller.dispose();
        await service.dispose();
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
        expect(
          latency,
          lessThan(50),
          reason: 'mode control must not wait for artwork transfer',
        );
      },
    );
  }
}
