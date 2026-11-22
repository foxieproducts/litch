import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/firmware_transfer_estimate.dart';

void main() {
  test('slow and fast measured transfers earn proportionate estimates', () {
    final slow = FirmwareTransferEstimate();
    final fast = FirmwareTransferEstimate();
    for (var second = 0; second <= 2; second++) {
      slow.add(.1 + .01 * second, Duration(seconds: second));
      fast.add(.1 + .1 * second, Duration(seconds: second));
      if (second < 2) {
        expect(slow.remainingAt(Duration(seconds: second)), isNull);
        expect(fast.remainingAt(Duration(seconds: second)), isNull);
      }
    }
    expect(
      slow.remainingAt(const Duration(seconds: 2))!.inSeconds,
      inInclusiveRange(87, 89),
    );
    expect(
      fast.remainingAt(const Duration(seconds: 2))!.inSeconds,
      inInclusiveRange(7, 8),
    );
  });

  test('recent window follows speed changes without reacting to one burst', () {
    final estimate = FirmwareTransferEstimate();
    for (var second = 0; second <= 8; second++) {
      estimate.add(.05 + .01 * second, Duration(seconds: second));
    }
    final before = estimate.remainingAt(const Duration(seconds: 8))!;
    estimate.add(.23, const Duration(milliseconds: 8100));
    final burst = estimate.remainingAt(const Duration(milliseconds: 8100))!;
    expect(burst.inSeconds, greaterThan(25));
    expect(burst, lessThan(before));
    for (var second = 9; second <= 18; second++) {
      estimate.add(.23 + .03 * (second - 8), Duration(seconds: second));
    }
    expect(
      estimate.remainingAt(const Duration(seconds: 18))!.inSeconds,
      inInclusiveRange(14, 17),
    );
  });

  test(
    'stall expires, resumed progress and rollback require fresh samples',
    () {
      final estimate = FirmwareTransferEstimate();
      void samples(int from, double progress) {
        for (var offset = 0; offset <= 2; offset++) {
          estimate.add(
            progress + .02 * offset,
            Duration(seconds: from + offset),
          );
        }
      }

      samples(0, .1);
      expect(estimate.remainingAt(const Duration(seconds: 2)), isNotNull);
      estimate.add(.14, const Duration(seconds: 5));
      expect(estimate.remainingAt(const Duration(seconds: 6)), isNull);
      estimate.add(.2, const Duration(seconds: 7));
      expect(estimate.remainingAt(const Duration(seconds: 7)), isNull);
      samples(8, .22);
      expect(estimate.remainingAt(const Duration(seconds: 10)), isNotNull);
      estimate.add(.1, const Duration(seconds: 11));
      expect(estimate.remainingAt(const Duration(seconds: 11)), isNull);
      estimate.add(.11, const Duration(seconds: 12));
      expect(estimate.remainingAt(const Duration(seconds: 12)), isNull);
    },
  );

  test(
    'new attempt, terminal progress and invalid clocks discard old rate',
    () {
      final estimate = FirmwareTransferEstimate();
      for (var second = 0; second <= 2; second++) {
        estimate.add(.2 + .1 * second, Duration(seconds: second));
      }
      expect(estimate.remainingAt(const Duration(seconds: 2)), isNotNull);
      for (final terminal in [0.0, 1.0, double.nan]) {
        estimate.add(terminal, const Duration(seconds: 3));
        expect(estimate.remainingAt(const Duration(seconds: 3)), isNull);
      }
      estimate.add(.5, const Duration(seconds: 10));
      estimate.add(.6, const Duration(seconds: 9));
      expect(estimate.remainingAt(const Duration(seconds: 9)), isNull);
      estimate.reset();
      expect(estimate.remainingAt(const Duration(seconds: 20)), isNull);
    },
  );
}
