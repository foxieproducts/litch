import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/ota/ota_attempt_proof.dart';
import 'package:litch_mobile/src/services/ota/ota_status_event.dart';

Map<String, Object?> frame() => {
  'op': 1,
  'q': 0xffffffff,
  'n': 'fedcba9876543210',
  'v': 1,
  's': [1, 0xffffffff, 0, 0, 0, 23, 2, 23, 2, 1, 24, 4, 1],
  'a': [1, 1, 0, 1, 23, 24],
};

OtaStatusEvent decode(Map<String, Object?> payload) => OtaStatusEvent.decode(
  utf8.encode(jsonEncode(payload)),
  deviceId: 'board',
  generation: 7,
)!;

void main() {
  test(
    'exact proof owns the op envelope before version and terminal decoders',
    () {
      final payload = frame();
      final proof = OtaAttemptProof.tryParse(payload)!;
      expect(decode(payload).kind, OtaStatusKind.attemptProof);
      expect(proof.requestId, 0xffffffff);
      expect(proof.ageMs, 0xffffffff);
      expect(proof.bootNonce, 'fedcba9876543210');
      expect(proof.hasCoherentSelection, isTrue);
      expect(proof.selectors[1].state, 4);
      final unavailable = decode({'op': 1, 'q': 1, 'e': 'unavailable'});
      expect(unavailable.kind, OtaStatusKind.attemptProof);
      expect(unavailable.isTerminal, isFalse);
      expect(OtaAttemptProof.tryParse(unavailable.payload), isNull);
    },
  );

  test(
    'malformed keys, bounds, nonce, scalars and tuple lengths fail closed',
    () {
      final invalid = <Map<String, Object?>>[
        {...frame(), 'extra': 1},
        {...frame()}..remove('q'),
        {...frame(), 'op': 1.0},
        {...frame(), 'q': 0},
        {...frame(), 'q': 0x100000000},
        {...frame(), 'q': '1'},
        {...frame(), 'n': '0000000000000000'},
        {...frame(), 'n': 'FEDCBA9876543210'},
        {...frame(), 'n': 'x' * 1000},
        {...frame(), 'v': true},
        {...frame(), 'v': 2},
        {
          ...frame(),
          's': [1, 0, 0],
        },
        {
          ...frame(),
          'a': [1, 1, 0, 1, 23, 24, 0],
        },
        {
          ...frame(),
          's': [1, -1, 0, 0, 0, 23, 2, 23, 2, 1, 24, 4, 1],
        },
        {
          ...frame(),
          's': [1, 0, 2, 0, 0, 23, 2, 23, 2, 1, 24, 4, 1],
        },
        {
          ...frame(),
          's': [1, 0, 0, 0, 0, 23, 2, 23, 5, 1, 24, 4, 1],
        },
        {
          ...frame(),
          'a': [0, 0, 0, 1, 0, 0],
        },
        {
          ...frame(),
          'a': [1, 1, 0, 0, 23, 24],
        },
        {'op': 1.0, 'q': 1, 'e': 'unavailable'},
        {'op': 1, 'q': 1, 'e': 'other'},
      ];
      for (final payload in invalid) {
        expect(OtaAttemptProof.tryParse(payload), isNull, reason: '$payload');
        expect(
          decode(payload).kind,
          OtaStatusKind.attemptProofMalformed,
          reason: '$payload',
        );
      }
    },
  );

  test(
    'coherence rejects changed SDK choice and preserves admitted skipped sequence',
    () {
      final inconsistent = {
        ...frame(),
        's': [1, 0, 0, 0, 0, 23, 2, 23, 2, 1, 24, 2, 1],
      };
      expect(
        OtaAttemptProof.tryParse(inconsistent)!.hasCoherentSelection,
        isFalse,
      );
      final skipped = {
        ...frame(),
        'a': [1, 1, 0, 1, 45, 48],
      };
      expect(OtaAttemptProof.tryParse(skipped)!.admission.expectedSequence, 48);
    },
  );
}
