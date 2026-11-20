import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';

String snapshot({
  Object? assistance,
  bool include = true,
  int sequence = 1,
  String variant = 'c',
}) => jsonEncode(<String, Object?>{
  'event': 'home_state',
  'v': 1,
  'seq': sequence,
  'a': 'idle',
  'fen': '8/8/8/8/8/8/4K3/7k w - - 0 1',
  'occ': '0000000000000000',
  'g': variant,
  'duck': '--',
  'out': 'n',
  'win': 'n',
  'rot': 0,
  'white': 1,
  'pm': 0,
  if (include) 'as': assistance,
});

void main() {
  test('older Home snapshots leave assistance unknown', () {
    final HomeStateNotification? state = HomeStateNotification.tryParse(
      snapshot(include: false),
    );
    expect(state, isNotNull);
    expect(state!.assistance, isNull);
  });

  final Map<String, HomeAssistancePresentation> presentations =
      <String, HomeAssistancePresentation>{
        '0x': HomeAssistancePresentation.suppressed,
        '2o': HomeAssistancePresentation.off,
        '3p': HomeAssistancePresentation.pending,
        '3t': HomeAssistancePresentation.thinking,
        '3he2e4': HomeAssistancePresentation.ready,
        '5re2e4': HomeAssistancePresentation.refreshing,
        '3be2e4a5d4': HomeAssistancePresentation.compoundDuck,
        '3da5d4': HomeAssistancePresentation.duck,
        '1le2A000000000000001': HomeAssistancePresentation.legal,
      };
  for (final MapEntry<String, HomeAssistancePresentation> entry
      in presentations.entries) {
    test('parses board assistance ${entry.key}', () {
      final HomeAssistanceState? state = HomeStateNotification.tryParse(
        snapshot(
          assistance: entry.key,
          variant: <String>['b', 'd'].contains(entry.key[1]) ? 'd' : 'c',
        ),
      )?.assistance;
      expect(state, isNotNull);
      expect(state!.presentation, entry.value);
      expect(state.requested, int.parse(entry.key[0]).isOdd);
    });
  }

  test('Coaching keeps Show Move available in pending and ready states', () {
    for (final value in ['6o', '7p', '7he2e4']) {
      final state = HomeStateNotification.tryParse(
        snapshot(assistance: value),
      )?.assistance;
      expect(state, isNotNull, reason: value);
      expect(state!.coachingOwned, isTrue);
      expect(state.manualAvailable, isTrue);
      expect(state.requested, value.startsWith('7'));
    }
  });

  test('Coaching ownership and retained request survive suppression', () {
    for (final String value in <String>['5x', '5le2A000000000000001']) {
      final HomeAssistanceState state = HomeStateNotification.tryParse(
        snapshot(assistance: value),
      )!.assistance!;
      expect(state.requested, isTrue);
      expect(state.coachingOwned, isTrue);
      expect(state.manualAvailable, isFalse);
      expect(state.isReady, isFalse);
    }
  });

  test('compound and remaining Duck retain the same logical destination', () {
    final HomeAssistanceState compound = HomeStateNotification.tryParse(
      snapshot(assistance: '3be2e4a5d4', variant: 'd'),
    )!.assistance!;
    final HomeAssistanceState remaining = HomeStateNotification.tryParse(
      snapshot(assistance: '3da5d4', sequence: 2, variant: 'd'),
    )!.assistance!;
    expect(compound.move, (from: 52, to: 36));
    expect(compound.duckMove, (from: 24, to: 35));
    expect(remaining.move, isNull);
    expect(remaining.duckMove, compound.duckMove);
    expect(compound.isReady, isTrue);
    expect(remaining.isReady, isTrue);
  });

  test('legal mask preserves both high and low logical square bits', () {
    final HomeAssistanceState state = HomeStateNotification.tryParse(
      snapshot(assistance: '1le2A000000000000001'),
    )!.assistance!;
    expect(state.legalSource, 52);
    expect(
      state.legalDestinations,
      BigInt.parse('A000000000000001', radix: 16),
    );
    expect(state.move, isNull);
    final held = HomeStateNotification.tryParse(
      snapshot(assistance: '1le210EF101000000000e4'),
    )!.assistance!;
    expect(held.legalSource, 52);
    expect(held.legalDestinations, BigInt.parse('10EF101000000000', radix: 16));
    expect(held.move, (from: 52, to: 36));
    expect(held.isReady, isFalse);
    for (final invalid in [
      '0le210EF101000000000e4', // no requested Hint
      '1le210EF101000000000e2', // selected source is not a destination
      '1le210EF101000000000d4', // target outside the full legal mask
      '1le210EF101000000000z9',
      '1le210EF101000000000e4a5', // no invented held Duck step
    ]) {
      expect(
        HomeStateNotification.tryParse(snapshot(assistance: invalid)),
        isNull,
        reason: invalid,
      );
    }
  });

  test(
    'Duck king-capture hint has only a chess step; refresh stays Classic',
    () {
      expect(
        HomeStateNotification.tryParse(
          snapshot(assistance: '3hf7e8', variant: 'd'),
        )?.assistance?.move,
        (from: 13, to: 4),
      );
      expect(
        HomeStateNotification.tryParse(
          snapshot(assistance: '5rf7e8', variant: 'd'),
        ),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(snapshot(assistance: '3be2e4a5d4')),
        isNull,
      );
      expect(
        HomeStateNotification.tryParse(snapshot(assistance: '3da5d4')),
        isNull,
      );
    },
  );

  for (final Object? value in <Object?>[
    null,
    3,
    '',
    '3',
    '8x',
    '3z',
    '3o',
    '2t',
    '2he2e4',
    '3he2e2',
    '3he9e4',
    '3hE2e4',
    '3he2e4x',
    '3be2e4a5a5',
    '3be2e4a5',
    '3da5--',
    '1le2FFFFFFFFFFFFFFF',
    '1le2FFFFFFFFFFFFFFFFF',
    '1le2FFFFFFFFFFFFFFFG',
    '1lz9FFFFFFFFFFFFFFFF',
    '1le20000000000000000',
    '1le20010000000000000',
  ]) {
    test('rejects malformed assistance atomically: $value', () {
      expect(
        HomeStateNotification.tryParse(snapshot(assistance: value)),
        isNull,
      );
    });
  }

  test(
    'omission and disconnect clear old assistance in the snapshot model',
    () {
      final HomeAssistanceState assistance = HomeStateNotification.tryParse(
        snapshot(assistance: '3he2e4'),
      )!.assistance!;
      final HomeBoardSnapshot current = HomeBoardSnapshot(
        assistance: assistance,
      );
      expect(current.copyWith(isRefreshing: true).assistance, same(assistance));
      expect(current.copyWith(clearAssistance: true).assistance, isNull);
      expect(current.copyWith(clearLiveState: true).assistance, isNull);
    },
  );
}
