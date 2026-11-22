import 'dart:async';

import 'ota/ota_status_event.dart';

class FirmwareDigestReader {
  const FirmwareDigestReader({this.timeout = const Duration(seconds: 8)});

  final Duration timeout;

  Future<String?> read({
    required Stream<OtaStatusEvent> events,
    required Future<bool> Function() sendRequest,
    Future<void>? cancelled,
    // Close the sender's lifetime synchronously before publishing a reply.
    // The native send may still be pending when a reply or deadline arrives.
    void Function()? onTerminal,
    bool throwOnTimeout = false,
  }) async {
    final Map<int, String> fragments = <int, String>{};
    final Completer<String?> result = Completer<String?>();
    late final StreamSubscription<OtaStatusEvent> subscription;
    Timer? timer;

    void finish(String? value) {
      if (result.isCompleted) return;
      onTerminal?.call();
      result.complete(value);
    }

    subscription = events.listen(
      (OtaStatusEvent event) {
        if (result.isCompleted) {
          return;
        }
        if (event.kind == OtaStatusKind.error &&
            event.detail == 'unknown_cmd') {
          finish(null);
          return;
        }
        if (event.kind != OtaStatusKind.digest) {
          return;
        }
        for (int index = 0; index < 8; ++index) {
          final String fragment = (event.payload['h$index'] ?? '')
              .toString()
              .trim()
              .toLowerCase();
          if (RegExp(r'^[0-9a-f]{8}$').hasMatch(fragment)) {
            fragments[index] = fragment;
          }
        }

        final String token = (event.payload['h'] ?? '').toString().trim();
        if (token == 'e' || token == 'u') {
          finish(null);
        } else if (token == 'r') {
          // A concurrent read on this same immutable-image channel may have
          // started after earlier fragments but before their delayed ready.
          // Keep the original deadline; only a complete digest is terminal.
          if (fragments.length != 8) return;
          finish(
            List<String>.generate(8, (int index) => fragments[index]!).join(),
          );
        }
      },
      onError: (Object _) {
        finish(null);
      },
      onDone: () {
        finish(null);
      },
    );
    timer = Timer(timeout, () {
      if (!result.isCompleted) {
        onTerminal?.call();
        if (throwOnTimeout) {
          result.completeError(
            TimeoutException('Firmware digest reply', timeout),
          );
        } else {
          result.complete(null);
        }
      }
    });

    cancelled?.then((_) {
      finish(null);
    });
    unawaited(
      Future<bool>.sync(sendRequest).then(
        (bool sent) {
          if (!sent) finish(null);
        },
        onError: (Object _) {
          finish(null);
        },
      ),
    );
    try {
      return await result.future;
    } finally {
      timer.cancel();
      await subscription.cancel();
    }
  }
}
