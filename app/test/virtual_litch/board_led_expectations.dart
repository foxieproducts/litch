import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';

import 'sdk_ble.dart';

// Observe native publications before SDK holds/drops. The app deliberately
// retains an older Home in the report-loss journeys.
Map<String, dynamic> publishedVirtualHome(VirtualLitchBle ble, int sequence) {
  return _publishedVirtualHome(ble, sequence) ??
      (throw StateError(
        'No complete production Home packet for sequence $sequence',
      ));
}

Map<String, dynamic>? _publishedVirtualHome(VirtualLitchBle ble, int sequence) {
  final text = utf8.decode(
    ble.emitted
        .where((packet) => packet.channel == 'console')
        .expand((packet) => packet.bytes)
        .toList(),
  );
  // A notification may end halfway through the next newline-framed record.
  final lines = text.split('\n')..removeLast();
  for (final line in lines.reversed) {
    if (!line.startsWith('{')) continue;
    Map<String, dynamic> decoded;
    try {
      decoded = jsonDecode(line) as Map<String, dynamic>;
    } on FormatException {
      // A retired connection can leave a partial record before its successor.
      continue;
    }
    final home = decoded['event'] == 'board_state'
        ? decoded['home'] as Map<String, dynamic>
        : decoded;
    if (home['event'] == 'home_state' && home['seq'] == sequence) return home;
  }
  return null;
}

Future<Map<String, dynamic>> publishedBoardFrame(
  WidgetTester tester,
  VirtualLitchBle ble,
) async {
  // Publication is paced independently of rendering. Await its existing byte
  // stream, including when SDK delivery to the app is intentionally dropped.
  for (var elapsed = 0; elapsed <= 200; elapsed += 10) {
    final frame = await ble.frame(tester);
    if (_publishedVirtualHome(ble, frame['home_sequence'] as int) != null) {
      return frame;
    }
    if (elapsed < 200) await ble.advance(tester, 10);
  }
  throw StateError('Native frame has no complete matching Home after 200 ms');
}

void expectOrdinaryEmptyCenters(
  VirtualLitchBle ble,
  Map<String, dynamic> frame,
  String checkpoint, {
  bool cpuDirected = false,
  bool cpuDuckDirected = false,
}) {
  final home = HomeStateNotification.tryParse(
    jsonEncode(publishedVirtualHome(ble, frame['home_sequence'] as int)),
  );
  expect(
    home,
    isNotNull,
    reason: '$checkpoint current native Home must decode',
  );
  expectOrdinaryEmptyCentersForHome(
    home!,
    frame,
    checkpoint,
    cpuDirected: cpuDirected,
    cpuDuckDirected: cpuDuckDirected,
  );
}

// Supporting fault tests use the same assertion without manufacturing BLE
// publications. Production journeys above always decode the matching native Home.
void expectOrdinaryEmptyCentersForHome(
  HomeStateNotification authority,
  Map<String, dynamic> frame,
  String checkpoint, {
  bool cpuDirected = false,
  bool cpuDuckDirected = false,
}) {
  final occupancy = BigInt.parse(frame['occupancy'] as String, radix: 16);
  expect(authority.physicalStatus.occupiedSquares, occupancy);
  final snapshot = HomeBoardSnapshot(
    position: authority.position,
    gameStatus: authority.gameStatus,
    physicalStatus: authority.physicalStatus,
    homeStateAction: authority.action,
    freshSetup: authority.freshSetup,
    vexMoveFirst: authority.vexMoveFirst,
    kingQueenSwapPromptActive: authority.kingQueenSwapPromptActive,
    physicalMovePrompt: authority.physicalMovePrompt,
    duckRelocationPrompt: authority.duckRelocationPrompt,
    assistance: authority.assistance,
  );
  int? source;
  int? landing;
  var fixedCpuDuck = false;
  if (!authority.gameStatus.isFinished &&
      authority.physicalStatus.placementMode != true &&
      !authority.kingQueenSwapPromptActive &&
      authority.undoPending != true &&
      snapshot.captureRemovalSquare == null) {
    final cpu = authority.physicalMovePrompt;
    final duck = snapshot.visibleDuckRelocation;
    // CPU Home retains its plan during foreground overlays. Only checkpoints
    // explicitly exercising the unobscured directed move admit that exception.
    if (cpuDuckDirected &&
        duck != null &&
        duck.owner == HomeDuckRelocationOwner.cpu &&
        duck.toSquare != null &&
        duck.fromSquare != duck.toSquare) {
      source = duck.fromSquare;
      landing = duck.toSquare;
      fixedCpuDuck = true;
    } else if (!cpuDuckDirected &&
        cpuDirected &&
        authority.action == HomeStateAction.cpu &&
        cpu != null &&
        !cpu.isPromotionFor(authority.position)) {
      source = cpu.fromSquare;
      landing = cpu.toSquare;
    } else if (!cpuDuckDirected) {
      final hint = snapshot.visibleAssistance;
      final chessHint = switch (hint?.presentation) {
        HomeAssistancePresentation.ready ||
        HomeAssistancePresentation.refreshing ||
        HomeAssistancePresentation.compoundDuck ||
        HomeAssistancePresentation.legal => true,
        _ => false,
      };
      if (chessHint && hint!.requested) {
        source = hint.move?.from;
        landing = hint.move?.to;
      }
    }
  }
  if (cpuDuckDirected) {
    expect(
      fixedCpuDuck,
      isTrue,
      reason: '$checkpoint requires an unobscured current fixed CPU Duck plan',
    );
    expect(
      occupancy & (BigInt.one << landing!),
      BigInt.zero,
      reason: '$checkpoint fixed Duck landing must still be physically empty',
    );
  }
  bool duckTransitSquare(int square) {
    if (!fixedCpuDuck) return false;
    final rankDelta = landing! ~/ 8 - source! ~/ 8;
    final fileDelta = landing % 8 - source % 8;
    final rankOffset = square ~/ 8 - source ~/ 8;
    final fileOffset = square % 8 - source % 8;
    final withinRanks = rankDelta >= 0
        ? rankOffset >= 0 && rankOffset <= rankDelta
        : rankOffset <= 0 && rankOffset >= rankDelta;
    final withinFiles = fileDelta >= 0
        ? fileOffset >= 0 && fileOffset <= fileDelta
        : fileOffset <= 0 && fileOffset >= fileDelta;
    final distance = rankDelta.abs() > fileDelta.abs()
        ? rankDelta.abs()
        : fileDelta.abs();
    // Bound the direct line geometrically without replaying the renderer's
    // interpolation or choosing between equally close midpoint cells.
    return withinRanks &&
        withinFiles &&
        2 * (rankDelta * fileOffset - fileDelta * rankOffset).abs() <= distance;
  }

  void expectDuckYellow(List<int> center, int square) {
    expect(
      center[0] > 0 &&
          center[0] == center[1] &&
          center[2] == 0 &&
          center[3] == 0,
      isTrue,
      reason: '$checkpoint fixed Duck yellow cue ${algebraicSquare(square)}',
    );
  }

  final knightPieceTransit = <int>{};
  if (!fixedCpuDuck &&
      source != null &&
      landing != null &&
      authority.position.pieces[source]?.toLowerCase() == 'n' &&
      (occupancy & (BigInt.one << landing)) == BigInt.zero) {
    final dr = landing ~/ 8 - source ~/ 8;
    final df = landing % 8 - source % 8;
    if ((dr.abs() == 2 && df.abs() == 1) || (dr.abs() == 1 && df.abs() == 2)) {
      final verticalFirst = dr.abs() == 2;
      final longStep = verticalFirst ? dr.sign * 8 : df.sign;
      // The long leg owns its axis. At the elbow exactly one emitter uses
      // the vertical axis; horizontal-only cells never get a Piece exception.
      // This legacy center projects the physical Top/Bottom pair together;
      // the native journey checks their individual order and transient timing.
      for (var step = 0; step <= 2; step++) {
        final square = source + step * longStep;
        if ((verticalFirst || step == 2) &&
            (occupancy & (BigInt.one << square)) == BigInt.zero) {
          knightPieceTransit.add(square);
        }
      }
    }
  }
  final litKnightTransit = <int>[];
  final litDuckTransit = <int>[];
  final board = frame['board'] as List<dynamic>;
  for (var square = 0; square < 64; square++) {
    if ((occupancy & (BigInt.one << square)) != BigInt.zero) continue;
    final center = (board[square][1] as List<dynamic>).cast<int>();
    if (square == landing) {
      expect(
        center.any((channel) => channel > 0),
        isTrue,
        reason: '$checkpoint directed empty landing ${algebraicSquare(square)}',
      );
      if (fixedCpuDuck) expectDuckYellow(center, square);
    } else if (duckTransitSquare(square)) {
      if (center.any((channel) => channel > 0)) {
        expectDuckYellow(center, square);
        litDuckTransit.add(square);
      }
    } else if (knightPieceTransit.contains(square)) {
      if (center.any((channel) => channel > 0)) litKnightTransit.add(square);
    } else {
      expect(
        center,
        <int>[0, 0, 0, 0],
        reason: '$checkpoint ordinary empty center ${algebraicSquare(square)}',
      );
    }
  }
  expect(
    litDuckTransit.length,
    lessThanOrEqualTo(2),
    reason:
        '$checkpoint only a Duck head and its handoff may light transit centers',
  );
  if (litDuckTransit.length == 2) {
    expect(
      (litDuckTransit.first ~/ 8 - litDuckTransit.last ~/ 8).abs() <= 1 &&
          (litDuckTransit.first % 8 - litDuckTransit.last % 8).abs() <= 1,
      isTrue,
      reason:
          '$checkpoint Duck transit handoff stays on neighboring route cells',
    );
  }
  expect(
    litKnightTransit.length,
    lessThanOrEqualTo(2),
    reason: '$checkpoint only a head and its handoff may light transit centers',
  );
  if (litKnightTransit.length == 2) {
    expect(
      (litKnightTransit.first - litKnightTransit.last).abs(),
      8,
      reason: '$checkpoint Piece handoff stays on adjacent vertical leg cells',
    );
  }
}
