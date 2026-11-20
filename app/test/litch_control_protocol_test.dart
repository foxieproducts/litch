import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';

void main() {
  test('encodes bounded Console records for minimum-MTU Ordered delivery', () {
    final records = LitchControlProtocol.encodeConnectionConsoleRecord(
      utf8.encode('settings status\n'),
    );

    expect(records, hasLength(3));
    expect(records[0], <int>[1, 1, 16, ...utf8.encode('setting')]);
    expect(records[1], <int>[1, 0, ...utf8.encode('s status')]);
    expect(records[2], <int>[1, 2, 10]);
  });

  test('rejects Console records that are empty, oversized, or contain NUL', () {
    expect(
      () => LitchControlProtocol.encodeConnectionConsoleRecord(const <int>[]),
      throwsRangeError,
    );
    expect(
      () => LitchControlProtocol.encodeConnectionConsoleRecord(
        List<int>.filled(256, 1),
      ),
      throwsRangeError,
    );
    expect(
      () => LitchControlProtocol.encodeConnectionConsoleRecord(<int>[65, 0]),
      throwsArgumentError,
    );
  });

  group('control request payloads', () {
    test(
      'Sleep has explicit Ordered action six beside storage reformat five',
      () {
        expect(LitchControlProtocol.encodeConnectionSleep(), <int>[
          1,
          6,
          0,
          0,
          1,
        ]);
        expect(LitchControlProtocol.encodeConnectionStorageReformat(), <int>[
          1,
          5,
          0,
          0,
          1,
        ]);
      },
    );

    test('encodes canonical connection-session action bodies', () {
      expect(
        LitchControlProtocol.encodeConnectionAction(
          action: LitchControlAction.hint,
        ),
        <int>[1, 1, 0, 0, 1],
      );
      expect(
        LitchControlProtocol.encodeConnectionAction(
          action: LitchControlAction.undo,
          undoCount: 3,
        ),
        <int>[1, 2, 0, 0, 3],
      );
      expect(
        LitchControlProtocol.encodeConnectionAction(
          action: LitchControlAction.coaching,
          owner: LitchControlOwner.p2,
          enabled: false,
        ),
        <int>[1, 4, 2, 1, 1],
      );
    });

    test('rejects malformed connection-session action fields', () {
      expect(
        () => LitchControlProtocol.encodeConnectionAction(
          action: LitchControlAction.undo,
          undoCount: 0,
        ),
        throwsArgumentError,
      );
      expect(
        () => LitchControlProtocol.encodeConnectionAction(
          action: LitchControlAction.coaching,
        ),
        throwsArgumentError,
      );
    });

    test('uses exact full action fields', () {
      expect(
        LitchControlProtocol.buildActionPayload(
          requestId: 17,
          action: LitchControlAction.hint,
        ),
        <String, Object>{'a': 'hint', 'i': 17},
      );
      expect(
        LitchControlProtocol.buildActionPayload(
          requestId: 18,
          action: LitchControlAction.undo,
        ),
        <String, Object>{'a': 'undo', 'i': 18, 'n': 1},
      );
      expect(
        LitchControlProtocol.buildActionPayload(
          requestId: 19,
          action: LitchControlAction.newGame,
        ),
        <String, Object>{'a': 'new_game', 'i': 19},
      );
      expect(
        LitchControlProtocol.buildActionPayload(
          requestId: 20,
          action: LitchControlAction.coaching,
          owner: LitchControlOwner.p1,
        ),
        <String, Object>{'a': 'coaching', 'i': 20, 'o': 'p1'},
      );
      expect(
        LitchControlProtocol.buildActionPayload(
          requestId: 21,
          action: LitchControlAction.coaching,
          owner: LitchControlOwner.p2,
          enabled: false,
        ),
        <String, Object>{'a': 'coaching', 'i': 21, 'o': 'p2', 'e': false},
      );
    });

    test('rejects zero IDs and malformed typed action fields', () {
      expect(
        () => LitchControlProtocol.buildActionPayload(
          requestId: 0,
          action: LitchControlAction.hint,
        ),
        throwsRangeError,
      );
      expect(
        () => LitchControlProtocol.buildActionPayload(
          requestId: 1,
          action: LitchControlAction.coaching,
        ),
        throwsArgumentError,
      );
      expect(
        () => LitchControlProtocol.buildActionPayload(
          requestId: 1,
          action: LitchControlAction.undo,
          undoCount: 0,
        ),
        throwsRangeError,
      );
    });
  });

  group('control outcome parsing', () {
    test('decodes full accepted and terminal outcomes', () {
      final LitchControlOutcome? accepted =
          LitchControlProtocol.decodeActionOutcome(<String, Object?>{
            'i': 19,
            'a': 'new_game',
            's': 'accepted',
            'r': 'none',
            'ok': true,
          }, expectedAction: LitchControlAction.newGame);
      final LitchControlOutcome? rejected =
          LitchControlProtocol.decodeActionOutcome(<String, Object?>{
            'i': 19,
            'a': 'new_game',
            's': 'rejected',
            'r': 'busy',
            'ok': false,
          }, expectedAction: LitchControlAction.newGame);

      expect(accepted?.status, LitchMutationStatus.accepted);
      expect(accepted?.actionState, LitchControlActionState.unknown);
      expect(accepted?.isTerminal, isFalse);
      expect(rejected?.status, LitchMutationStatus.rejected);
      expect(rejected?.reason, LitchMutationReason.busy);
      expect(rejected?.isTerminal, isTrue);

      final LitchControlOutcome? undoPending =
          LitchControlProtocol.decodeActionOutcome(<String, Object?>{
            'i': 20,
            'a': 'undo',
            's': 'applied',
            'r': 'none',
            'p': 'undo_pending',
            'ok': true,
          }, expectedAction: LitchControlAction.undo);
      expect(undoPending?.actionState, LitchControlActionState.undoPending);
    });

    test('archive-required New Game outcomes are terminal rejections', () {
      final payloads = <Map<String, Object?>>[
        <String, Object?>{
          'i': 19,
          'a': 'new_game',
          's': 'rejected',
          'r': 'archive_required',
          'ok': false,
        },
        <String, Object?>{
          'x': <Object?>[19, 2, 15],
        },
      ];
      for (final payload in payloads) {
        final outcome = LitchControlProtocol.decodeActionOutcome(
          payload,
          expectedAction: LitchControlAction.newGame,
        );
        expect(outcome?.requestId, 19);
        expect(outcome?.action, LitchControlAction.newGame);
        expect(outcome?.status, LitchMutationStatus.rejected);
        expect(outcome?.reason, LitchMutationReason.archiveRequired);
        expect(outcome?.isTerminal, isTrue);
      }
    });

    test('decodes every compact status and stable reason boundary', () {
      for (final LitchMutationStatus status in LitchMutationStatus.values) {
        final int reason = status == LitchMutationStatus.rejected ? 14 : 0;
        final LitchControlOutcome? outcome =
            LitchControlProtocol.decodeActionOutcome(<String, Object?>{
              'x': <Object?>[65535, status.wireCode, reason],
            }, expectedAction: LitchControlAction.undo);
        expect(outcome?.requestId, 65535);
        expect(outcome?.status, status);
        expect(outcome?.actionState, LitchControlActionState.unknown);
      }
      final LitchControlOutcome? rejected =
          LitchControlProtocol.decodeActionOutcome(<String, Object?>{
            'x': <Object?>[1, 2, 14],
          }, expectedAction: LitchControlAction.undo);
      expect(rejected?.reason, LitchMutationReason.storageUnavailable);
    });

    test('drops malformed, inconsistent, and mismatched outcomes', () {
      final List<Map<String, Object?>> malformed = <Map<String, Object?>>[
        <String, Object?>{
          'x': <Object?>[0, 1, 0],
        },
        <String, Object?>{
          'x': <Object?>[65536, 1, 0],
        },
        <String, Object?>{
          'x': <Object?>[1.0, 1, 0],
        },
        <String, Object?>{
          'x': <Object?>[1, 4, 0],
        },
        <String, Object?>{
          'x': <Object?>[1, 1, 15],
        },
        <String, Object?>{
          'x': <Object?>[1, 2, 0],
        },
        <String, Object?>{
          'x': <Object?>[1, 1],
        },
      ];
      for (final Map<String, Object?> payload in malformed) {
        expect(
          LitchControlProtocol.decodeActionOutcome(
            payload,
            expectedAction: LitchControlAction.hint,
          ),
          isNull,
        );
      }
      expect(
        LitchControlProtocol.decodeActionOutcome(<String, Object?>{
          'i': 7,
          'a': 'hint',
          's': 'applied',
          'r': 'none',
          'ok': false,
        }, expectedAction: LitchControlAction.hint),
        isNull,
      );
      expect(
        LitchControlProtocol.decodeActionOutcome(<String, Object?>{
          'i': 7,
          'a': 'hint',
          's': 'rejected',
          'r': 'response_too_large',
          'ok': false,
        }, expectedAction: LitchControlAction.hint),
        isNull,
      );
      expect(
        LitchControlProtocol.decodeActionOutcome(<String, Object?>{
          'i': 7,
          'a': 'undo',
          's': 'applied',
          'r': 'none',
          'ok': true,
        }, expectedAction: LitchControlAction.hint),
        isNull,
      );
      final LitchControlOutcome? futureState =
          LitchControlProtocol.decodeActionOutcome(<String, Object?>{
            'i': 8,
            'a': 'undo',
            's': 'applied',
            'r': 'none',
            'p': 'future_state',
            'ok': true,
          }, expectedAction: LitchControlAction.undo);
      expect(futureState?.actionState, LitchControlActionState.unknown);
    });
  });

  group('correlated setting outcomes', () {
    test('decodes full and compact y replies', () {
      final LitchSettingOutcome? full =
          LitchControlProtocol.decodeSettingOutcome(<String, Object?>{
            'k': 'BRIGHTNESS',
            'v': 6,
            'ok': true,
            'i': 42,
          });
      final LitchSettingOutcome? compactSuccess =
          LitchControlProtocol.decodeSettingOutcome(<String, Object?>{
            'y': <Object?>[43, 1, 0],
          });
      final LitchSettingOutcome? compactFailure =
          LitchControlProtocol.decodeSettingOutcome(<String, Object?>{
            'y': <Object?>[44, 0, 14],
          });

      expect(full?.requestId, 42);
      expect(full?.key, 'BRIGHTNESS');
      expect(full?.value, 6);
      expect(compactSuccess?.ok, isTrue);
      expect(compactSuccess?.hasValue, isFalse);
      expect(compactFailure?.reason, LitchMutationReason.storageUnavailable);
      expect(
        LitchControlProtocol.decodeSettingOutcome(<String, Object?>{
          'k': 'BRIGHTNESS',
          'e': 'response_too_large',
          'ok': false,
        })?.reason,
        LitchMutationReason.badRequest,
      );
    });

    test('does not coerce malformed y tuples', () {
      for (final Object tuple in <Object>[
        <Object?>[0, 1, 0],
        <Object?>[1, true, 0],
        <Object?>[1, 2, 0],
        <Object?>[1, 1, 1],
        <Object?>[1, 0, 0],
        <Object?>[1, 0],
      ]) {
        expect(
          LitchControlProtocol.decodeSettingOutcome(<String, Object?>{
            'y': tuple,
          }),
          isNull,
        );
      }
    });
  });

  test('request allocator wraps, skips reservations, and stays bounded', () {
    final LitchRequestIdAllocator allocator = LitchRequestIdAllocator(
      initialRequestId: 65535,
      maxReserved: 2,
    );
    expect(allocator.reserve(), 65535);
    expect(allocator.reserve(), 1);
    expect(allocator.reserve(), isNull);
    allocator.release(65535);
    expect(allocator.reserve(), 2);
    expect(allocator.isReserved(1), isTrue);
    expect(allocator.isReserved(2), isTrue);
    expect(allocator.reservedCount, 2);
  });
}
