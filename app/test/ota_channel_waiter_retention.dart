// Run from app/: dart --enable-vm-service=0 --packages=.dart_tool/package_config.json test/ota_channel_waiter_retention.dart
// A separate VM allows a black-box retention check against real full GC,
// without a public debug API or a counter that mirrors channel internals.
import 'dart:async';
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:isolate';
import 'dart:io';

// This explicit VM harness reuses the client pinned by flutter_test.
// ignore: depend_on_referenced_packages
import 'package:vm_service/vm_service_io.dart';
import 'package:litch_mobile/src/services/ota/litch_ota_control_channel.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';

class _WaitOwner {
  void notificationError(Object error) =>
      throw StateError('Settled waiter received a later native error: $error');
}

Future<void> _refresh(
  LitchOtaControlChannel channel,
  List<WeakReference<_WaitOwner>> references,
  int mode,
) async {
  final owner = _WaitOwner();
  references.add(WeakReference(owner));
  final cancelled = Completer<void>();
  final waiting = channel.waitFor(
    (event) => event.kind == OtaStatusKind.deviceInfo,
    timeout: mode == 1
        ? const Duration(milliseconds: 1)
        : const Duration(seconds: 1),
    cancelled: mode == 2 ? cancelled.future : null,
    onNotificationError: owner.notificationError,
  );
  if (mode == 0) {
    Timer.run(() => channel.acceptPacket(utf8.encode('{"i":[3,3,671]}')));
  } else if (mode == 2) {
    Timer.run(cancelled.complete);
  }
  final result = await waiting;
  if ((mode == 0) != (result != null)) {
    throw StateError(
      'Expected ${mode == 0 ? 'reply' : 'settled empty result'}',
    );
  }
}

Future<void> main() async {
  final info = await developer.Service.getInfo();
  final uri = info.serverUri;
  if (uri == null) throw StateError('Run with --enable-vm-service=0');
  final vm = await vmServiceConnectUri(
    uri.replace(scheme: 'ws', path: '${uri.path}ws').toString(),
  );
  final isolate = developer.Service.getIsolateId(Isolate.current)!;

  final packets = StreamController<List<int>>.broadcast(sync: true);
  final channel = LitchOtaControlChannel(
    deviceId: 'board-1',
    connectionToken: 1,
    generation: 1,
    subscribe: () => packets.stream,
  );
  final references = <WeakReference<_WaitOwner>>[];
  try {
    await channel.start();
    Future<void> collect(String phase) async {
      for (var pass = 0; pass < 3; pass++) {
        await Future<void>.delayed(Duration.zero);
        await vm.getAllocationProfile(isolate, gc: true);
      }
      final retained = references.where((ref) => ref.target != null).length;
      stdout.writeln(
        '$phase: retained=$retained/${references.length} settled wait owners',
      );
      if (retained != 0) throw StateError('Completed waiters remain retained');
    }

    for (var refresh = 0; refresh < 180; refresh++) {
      await _refresh(channel, references, refresh % 3);
    }
    await collect('healthy direct notification');
    await channel.retireDirectPackets();
    for (var refresh = 0; refresh < 180; refresh++) {
      await _refresh(channel, references, refresh % 3);
    }
    await collect('shared-session takeover');
  } finally {
    await channel.dispose();
    await packets.close();
    await vm.dispose();
    await developer.Service.controlWebServer(enable: false);
  }
}
