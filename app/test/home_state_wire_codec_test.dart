import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';

void main() {
  Uint8List fixture() {
    final row = File('../test/fixtures/home_state_wire_v1.csv')
        .readAsLinesSync()
        .firstWhere((line) => line.startsWith('representative,'));
    final hex = row.substring(row.indexOf(',') + 1);
    return Uint8List.fromList(<int>[
      for (var index = 0; index < hex.length; index += 2)
        int.parse(hex.substring(index, index + 2), radix: 16),
    ]);
  }

  test('decodes the shared full Home snapshot without JSON', () {
    final home = HomeStateWireCodec.tryDecode(fixture(), sequence: 41);

    expect(home, isNotNull);
    expect(home!.sequence, 41);
    expect(home.action, HomeStateAction.cpu);
    expect(
      home.position.fen,
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    );
    expect(home.gameStatus.variant, 'classic');
    expect(home.gameStatus.outcome, 'none');
    expect(
      home.physicalStatus.occupiedSquares,
      BigInt.parse('FFFF00000000FFFF', radix: 16),
    );
    expect(home.physicalStatus.rotated, isTrue);
    expect(home.freshSetup, isTrue);
    expect(home.undoPending, isTrue);
    expect(home.coachingMask, 2);
    expect(
      home.persistenceDegradation,
      HomePersistenceDegradation.quarantinedRecord,
    );
    expect(home.physicalMovePrompt?.fromSquare, 12);
    expect(home.physicalMovePrompt?.toSquare, 28);
    expect(home.lastMoves?.white?.fromSquare, 52);
    expect(home.lastMoves?.white?.toSquare, 36);
    expect(home.lastMoves?.mostRecent, HomeSide.white);
    expect(home.clockStatus?.activeSide, HomeSide.black);
    expect(home.clockStatus?.whiteRemainingSeconds, 298);
    expect(home.clockStatus?.blackRemainingSeconds, 300);
    expect(home.assistance?.presentation, HomeAssistancePresentation.ready);
    expect(home.assistance?.move, (from: 52, to: 36));
  });

  test('decodes the board-owned Classic result and its semantic rewind', () {
    final bytes = Uint8List(HomeStateWireCodec.payloadBytes);
    bytes[0] = HomeStateWireCodec.version;
    bytes[1] = HomeStateAction.idle.index;
    bytes[2] = 1; // Classic
    bytes.fillRange(62, 71, 0xff);
    int sequence = 0;
    HomeStateNotification? state(String fen, int classicResult) {
      bytes.setRange(
        11,
        54,
        GameHistoryCodec.encodePosition(
          ArchivedPosition(ply: 0, fen: fen),
          GameVariant.classic,
        ),
      );
      bytes[71] = classicResult;
      return HomeStateWireCodec.tryDecode(bytes, sequence: ++sequence);
    }

    const mate = 'k7/n1K5/1N6/8/8/8/8/8 b - - 0 1';
    final whiteWins = state(mate, 1)!.gameStatus;
    expect(
      (whiteWins.outcome, whiteWins.winner, whiteWins.isFinished),
      ('checkmate', 'white', true),
    );
    const foolsMate =
        'rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 0 1';
    expect(state(foolsMate, 1)!.gameStatus.winner, 'black');
    final stalemate = state('7k/5Q2/6K1/8/8/8/8/8 b - - 0 1', 2)!.gameStatus;
    expect((stalemate.outcome, stalemate.winner), ('stalemate', 'none'));
    final draw = state('k7/n1K5/8/4N3/8/8/8/8 b - - 0 1', 3)!.gameStatus;
    expect((draw.outcome, draw.winner), ('draw', 'none'));
    expect(
      state('k7/n1K5/8/8/2N5/8/8/8 w - - 0 1', 0)!.gameStatus.isFinished,
      isFalse,
    );
    // The released board omitted Classic results, even at the exact mate FEN.
    expect(state(mate, 0)!.gameStatus.isFinished, isFalse);
    bytes[8] |= 0x40; // CPU thinking owns bytes 71–76 while this flag is set.
    final thinking = state('k7/n1K5/8/8/2N5/8/8/8 w - - 0 1', 3)!;
    expect(thinking.gameStatus.isFinished, isFalse);
    expect(thinking.cpuThinking!.elapsedHalfSeconds, 3);
  });

  test('explicit resignation uses the existing Home outcome tuple', () {
    final bytes = fixture();
    bytes[1] = HomeStateAction.idle.index;
    bytes[3] = 3;
    bytes[4] = 2;
    bytes[5] = 0;
    final home = HomeStateWireCodec.tryDecode(bytes, sequence: 42);
    expect(home, isNotNull);
    expect(home!.gameStatus.outcome, 'resignation');
    expect(home.gameStatus.winner, 'black');
    bytes[3] = 4;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 43), isNull);
  });

  test('invalid complete Home remains incomplete until valid authority', () {
    final assembler = HomeStateWireAssembler();
    final bytes = fixture();
    void deliver(Uint8List payload, int revision) {
      for (var index = 0; index < HomeStateWireCodec.fragmentCount; ++index) {
        assembler.accept(
          code: HomeStateWireCodec.firstStateCode + index,
          revision: revision,
          sequence: revision,
          body: Uint8List.sublistView(payload, index * 10, (index + 1) * 10),
        );
      }
    }

    expect(assembler.isIncomplete, isFalse);
    deliver(bytes, 1);
    expect(assembler.isIncomplete, isFalse);
    final malformed = Uint8List.fromList(bytes)..[0] = 255;
    deliver(malformed, 2);
    expect(
      assembler.isIncomplete,
      isTrue,
      reason: 'all fragment slots is not proof of a valid snapshot',
    );
    deliver(bytes, 3);
    expect(
      assembler.isIncomplete,
      isFalse,
      reason: 'valid exact payload replay still establishes coherence',
    );
    assembler.clear();
    expect(assembler.isIncomplete, isFalse);
  });

  test('decodes only board-authoritative king and queen prompt state', () {
    final bytes = fixture();
    bytes[1] = HomeStateAction.correction.index;
    bytes[7] &= ~0x60; // No queued physical move or capture.
    bytes[62] = bytes[63] = bytes[64] = 0xff;
    expect(
      HomeStateWireCodec.tryDecode(
        bytes,
        sequence: 1,
      )!.kingQueenSwapPromptActive,
      isFalse,
    );
    bytes[6] |= 0x80;
    final home = HomeStateWireCodec.tryDecode(bytes, sequence: 2)!;
    expect(home.kingQueenSwapPromptActive, isTrue);
    expect(home.withoutAssistance().kingQueenSwapPromptActive, isTrue);
    expect(home.withCpuThinking(null).kingQueenSwapPromptActive, isTrue);
    bytes[6] |= 0x04; // Placement is a different board-owned flow.
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 3), isNull);
  });

  test('publishes only a complete revision and suppresses exact replay', () {
    final bytes = fixture();
    final assembler = HomeStateWireAssembler();
    HomeStateNotification? published;

    for (
      var index = HomeStateWireCodec.fragmentCount - 1;
      index >= 1;
      index -= 1
    ) {
      published = assembler.accept(
        code: HomeStateWireCodec.firstStateCode + index,
        revision: 0x1234,
        body: Uint8List.sublistView(
          bytes,
          index * HomeStateWireCodec.fragmentBytes,
          (index + 1) * HomeStateWireCodec.fragmentBytes,
        ),
        sequence: 8,
      );
      expect(published, isNull);
    }
    expect(assembler.isIncomplete, isTrue);
    published = assembler.accept(
      code: HomeStateWireCodec.firstStateCode,
      revision: 0x1234,
      body: Uint8List.sublistView(bytes, 0, HomeStateWireCodec.fragmentBytes),
      sequence: 8,
    );
    expect(published?.sequence, 8);
    expect(assembler.isIncomplete, isFalse);
    expect(
      assembler.accept(
        code: HomeStateWireCodec.firstStateCode,
        revision: 0x1234,
        body: Uint8List.sublistView(bytes, 0, HomeStateWireCodec.fragmentBytes),
        sequence: 9,
      ),
      isNull,
    );
  });

  test('decodes Coaching availability and matching Duck hint continuation', () {
    final bytes = fixture();
    bytes[91] = 0x07;
    final home = HomeStateWireCodec.tryDecode(bytes, sequence: 1);

    expect(home, isNotNull);
    expect(home!.assistance!.requested, isTrue);
    expect(home.assistance!.coachingOwned, isTrue);
    expect(home.assistance!.manualAvailable, isTrue);
    expect(home.assistance!.move, (from: 52, to: 36));

    bytes[1] = HomeStateAction.lift.index;
    bytes[2] = 2; // Duck variant.
    bytes[53] = 27; // The tracked Duck is on d5.
    bytes[6] &= ~0x08; // Ordinary play has left Fresh Setup.
    bytes[7] &= ~0x60;
    bytes[62] = bytes[63] = bytes[64] = 0xff;
    bytes[85] = HomeAssistancePresentation.legal.index;
    bytes[88] = 27;
    bytes[89] = 35;
    bytes[90] = 52;
    bytes[91] = 0x05; // Coaching owns this revealed, held compound hint.
    final data = ByteData.sublistView(bytes);
    data.setUint32(92, 0, Endian.little);
    data.setUint32(96, 0x1010, Endian.little);
    data.setUint32(54, 0x0800ffff, Endian.little);
    data.setUint32(58, 0xffef0000, Endian.little);
    HomeBoardSnapshot snapshot(HomeStateNotification state) =>
        HomeBoardSnapshot(
          position: state.position,
          gameStatus: state.gameStatus,
          physicalStatus: state.physicalStatus,
          homeStateAction: state.action,
          freshSetup: state.freshSetup,
          kingQueenSwapPromptActive: state.kingQueenSwapPromptActive,
          assistance: state.assistance,
        );
    final held = HomeStateWireCodec.tryDecode(bytes, sequence: 2)!;
    expect(held.assistance!.move, (from: 52, to: 36));
    expect(held.assistance!.duckMove, (from: 27, to: 35));
    expect(
      held.assistance!.legalDestinations,
      (BigInt.one << 36) | (BigInt.one << 44),
    );
    final heldSnapshot = snapshot(held);
    final visibleHeld = heldSnapshot.visibleAssistance!;
    expect(visibleHeld.presentation, HomeAssistancePresentation.legal);
    expect(visibleHeld.move, held.assistance!.move);
    expect(visibleHeld.legalSource, 52);
    expect(visibleHeld.legalDestinations, held.assistance!.legalDestinations);
    expect(visibleHeld.duckMove, isNull);
    expect(visibleHeld.requested, isTrue);
    expect(visibleHeld.coachingOwned, isTrue);
    expect(heldSnapshot.assistanceGuidance, 'Legal moves from e2 are marked.');

    // Current boards send only the chess step; older boards may retain the
    // full tuple above. Both expose the same chess-only guidance while held.
    final chessOnlyBytes = Uint8List.fromList(bytes)
      ..[88] = 0xff
      ..[89] = 0xff;
    final chessOnly = HomeStateWireCodec.tryDecode(
      chessOnlyBytes,
      sequence: 2,
    )!;
    expect(chessOnly.assistance!.duckMove, isNull);
    expect(snapshot(chessOnly).visibleAssistance, same(chessOnly.assistance));
    expect(snapshot(chessOnly).visibleAssistance!.move, visibleHeld.move);

    bytes[1] = HomeStateAction.settle.index;
    bytes[85] = HomeAssistancePresentation.duck.index;
    bytes[86] = bytes[87] = bytes[90] = 0xff;
    data.setUint32(96, 0, Endian.little);
    data.setUint32(58, 0xffef0010, Endian.little);
    final settling = HomeStateWireCodec.tryDecode(bytes, sequence: 3)!;
    final settlingSnapshot = snapshot(settling);
    expect(settling.assistance!.move, isNull);
    expect(settling.assistance!.duckMove, held.assistance!.duckMove);
    expect(settlingSnapshot.visibleAssistance, isNull);
    expect(settlingSnapshot.assistanceGuidance, isNull);
    expect(
      settlingSnapshot.copyWith(assistance: held.assistance).visibleAssistance,
      isNull,
      reason: 'Chess settling cannot invite the held chess move again.',
    );

    bytes[1] = HomeStateAction.duck.index;
    bytes[8] |= 0x01; // Human's selectable relocation has no fixed target.
    bytes[65] = 27;
    final relocating = HomeStateWireCodec.tryDecode(bytes, sequence: 4)!;
    expect(
      relocating.duckRelocationPrompt?.owner,
      HomeDuckRelocationOwner.human,
    );
    expect(relocating.duckRelocationPrompt?.toSquare, isNull);
    expect(relocating.assistance!.duckMove, settling.assistance!.duckMove);
    final relocatingSnapshot = snapshot(relocating);
    expect(relocatingSnapshot.visibleAssistance, same(relocating.assistance));
    expect(relocatingSnapshot.assistanceGuidance, 'Duck hint: d5 to d4.');
    final legacyRelocation = relocatingSnapshot
        .copyWith(assistance: held.assistance)
        .visibleAssistance!;
    expect(legacyRelocation.presentation, HomeAssistancePresentation.duck);
    expect(legacyRelocation.move, isNull);
    expect(legacyRelocation.legalDestinations, isNull);
    expect(legacyRelocation.duckMove, held.assistance!.duckMove);
  });

  test('rejects a semantically invalid assistance projection', () {
    final bytes = fixture();
    bytes[91] = 0x06;

    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 1), isNull);

    bytes[2] = 2;
    bytes[53] = 27;
    bytes[85] = HomeAssistancePresentation.legal.index;
    bytes[88] = 27;
    bytes[89] = 35;
    bytes[90] = 52;
    bytes[91] = 1;
    ByteData.sublistView(bytes).setUint32(96, 0x1010, Endian.little);
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 2), isNotNull);
    for (final invalid in [0xff, 64, 27]) {
      bytes[89] = invalid;
      expect(HomeStateWireCodec.tryDecode(bytes, sequence: 3), isNull);
    }
    bytes[89] = 35;
    bytes[86] = bytes[87] = 0xff;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 4), isNull);
    bytes[86] = 52;
    bytes[87] = 36;
    bytes[91] = 0;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 5), isNull);
    bytes[91] = 1;
    bytes[2] = 1;
    bytes[53] = 0xff;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 6), isNull);
    bytes[88] = bytes[89] = 0xff;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 7), isNotNull);

    bytes[1] = HomeStateAction.correction.index;
    bytes[2] = 2;
    bytes[53] = 27;
    bytes[7] &= ~0x60;
    bytes[62] = bytes[63] = bytes[64] = 0xff;
    bytes[85] = HomeAssistancePresentation.suppressed.index;
    bytes[86] = bytes[87] = bytes[90] = 0xff;
    final data = ByteData.sublistView(bytes);
    data.setUint32(58, 0xffff0400, Endian.little);
    data.setUint32(96, 0x00100400, Endian.little);
    final correction = HomeStateWireCodec.tryDecode(bytes, sequence: 8)!;
    final mask = (BigInt.one << 42) | (BigInt.one << 52);
    expect(correction.duckLandingRemovalMask, mask);
    expect(correction.withoutAssistance().duckLandingRemovalMask, mask);
    expect(correction.assistance?.legalDestinations, isNull);
    final snapshot = HomeBoardSnapshot(
      position: correction.position,
      gameStatus: correction.gameStatus,
      physicalStatus: correction.physicalStatus,
      homeStateAction: correction.action,
      duckLandingRemovalMask: correction.duckLandingRemovalMask,
    );
    expect(snapshot.guidedSquares, {42, 52});
    expect(snapshot.mismatchedSquares, isNot(contains(52)));
    expect(
      snapshot.copyWith(clearLiveState: true).duckLandingRemovalMask,
      isNull,
    );

    bytes[6] |= 0x04;
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 9), isNull);
    bytes[6] &= ~0x04;
    data.setUint32(96, 0x00100404, Endian.little);
    expect(HomeStateWireCodec.tryDecode(bytes, sequence: 10), isNull);
  });
}
