import 'package:flutter_test/flutter_test.dart' as flutter_test;

import 'support/standard_chess_replay_cases.dart';

void main() {
  flutter_test.test(
    'all standard special moves validate through replay and archive',
    runSpecialMoveCases,
  );
  flutter_test.test(
    'strict cumulative replay validates FEN, bounds and atomicity',
    runCumulativeReplayCases,
  );
  flutter_test.test(
    'forward replay preserves completed and unfinished archives',
    runArchiveLifecycleCases,
  );
}
