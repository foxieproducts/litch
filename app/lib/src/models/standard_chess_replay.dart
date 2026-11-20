import 'dart:convert';

import 'package:chess/chess.dart' as rules;

import 'game_archive.dart';

// Evaluation proposal: bound counters to signed 32-bit values, reserving room
// for a complete archive replay. This is not a new archive/wire constraint.
const int standardReplayMaximumInitialCounter =
    0x7fffffff - gameArchiveMaximumPlies;
final RegExp _uci = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');
final RegExp _counter = RegExp(r'^(?:0|[1-9][0-9]{0,9})$');

/// Reconstructs a forward standard-chess prefix into the existing archive value.
///
/// The rules engine is private and recreated on every call. All metadata and
/// positions pass GameArchive validation before a result can be returned.
/// Nothing is persisted, submitted, or enabled here. The caller owns provider
/// identity, clocks, lifecycle and durable publication. A shorter/divergent
/// provider line may be a legitimate takeback; it needs separate reconciliation.
/// This forward-only helper does not change the archive's completed-game Undo
/// semantics or permit mutation of an unfinished identity boundary.
GameArchive replayStandardChessExtension(
  GameArchive prior,
  List<String> cumulativeUci,
) {
  prior.validate();
  if (prior.variant != GameVariant.classic ||
      prior.captureQuality != GameCaptureQuality.complete ||
      prior.positions.first.ply != 0 ||
      cumulativeUci.length > gameArchiveMaximumPlies ||
      cumulativeUci.length < prior.transitions.length) {
    throw const FormatException(
      'Unsupported replay scope or cumulative bound.',
    );
  }
  if (cumulativeUci.length > prior.transitions.length &&
      prior.status != GameArchiveStatus.active) {
    throw const FormatException('Forward replay requires an active archive.');
  }
  for (final move in cumulativeUci) {
    if (move.length < 4 || move.length > 5 || !_uci.hasMatch(move)) {
      throw const FormatException('Invalid bounded UCI move.');
    }
  }
  final initialFen = prior.positions.first.fen;
  final chess = validatedStandardChessInitialPosition(initialFen);
  final positions = <ArchivedPosition>[prior.positions.first];
  final transitions = <ArchivedTransition>[];
  for (var index = 0; index < cumulativeUci.length; index++) {
    final uci = cumulativeUci[index];
    final fromName = uci.substring(0, 2);
    final toName = uci.substring(2, 4);
    final source = chess.get(fromName);
    if (source == null) {
      throw const FormatException('Replay origin is empty.');
    }
    final promotes =
        source.type == rules.Chess.PAWN && (uci[3] == '1' || uci[3] == '8');
    if (promotes != (uci.length == 5)) {
      throw const FormatException('Promotion must match the pawn move.');
    }
    final from = _square(fromName);
    final to = _square(toName);
    final isCastle = source.type == rules.Chess.KING && (from - to).abs() == 2;
    final isEp =
        source.type == rules.Chess.PAWN &&
        fromName[0] != toName[0] &&
        chess.get(toName) == null;
    final capture = chess.get(toName) != null
        ? to
        : isEp
        ? (from ~/ 8) * 8 + to % 8
        : null;
    final transition = ArchivedTransition(
      side: source.color == rules.Chess.WHITE ? GameSide.white : GameSide.black,
      from: from,
      to: to,
      captureSquare: capture,
      promotion: promotes
          ? switch (uci[4]) {
              'q' => PromotionPiece.queen,
              'r' => PromotionPiece.rook,
              'b' => PromotionPiece.bishop,
              'n' => PromotionPiece.knight,
              _ => throw const FormatException('Unsupported promotion.'),
            }
          : null,
      secondaryFrom: isCastle ? (from ~/ 8) * 8 + (to > from ? 7 : 0) : null,
      secondaryTo: isCastle ? from + (to > from ? 1 : -1) : null,
    );
    // Map input selects from legal generated moves. Never accept the package's
    // public Move object input, which bypasses its legal-list matching.
    if (!chess.move(<String, String>{
      'from': fromName,
      'to': toName,
      if (promotes) 'promotion': uci[4],
    })) {
      throw const FormatException('Illegal standard-chess move.');
    }
    final position = ArchivedPosition(ply: index + 1, fen: chess.fen);
    positions.add(position);
    transitions.add(transition);
    if (index < prior.transitions.length &&
        (position.fen != prior.positions[index + 1].fen ||
            jsonEncode(transition.toJson()) !=
                jsonEncode(prior.transitions[index].toJson()))) {
      throw const FormatException(
        'Cumulative replay conflicts with archive prefix.',
      );
    }
  }
  // This factory validates the complete actual board deltas, including capture,
  // promotion and secondary rook relocation, before the value is published.
  final candidate = GameArchive(
    libraryId: prior.libraryId,
    boardGameId: prior.boardGameId,
    gameId: prior.gameId,
    origin: prior.origin,
    createdAt: prior.createdAt,
    updatedAt: prior.updatedAt,
    status: prior.status,
    captureQuality: prior.captureQuality,
    variant: prior.variant,
    outcome: prior.outcome,
    winner: prior.winner,
    lineRevision: prior.lineRevision,
    positions: positions,
    transitions: transitions,
    displayName: prior.displayName,
    whitePlayer: prior.whitePlayer,
    blackPlayer: prior.blackPlayer,
    provenance: prior.provenance,
    provider: prior.provider,
  );
  return cumulativeUci.length == prior.transitions.length ? prior : candidate;
}

int _square(String name) =>
    (8 - int.parse(name[1])) * 8 + name.codeUnitAt(0) - 97;

/// Loads a canonical, internally consistent standard-chess position.
///
/// This is the shared legality boundary for replay and bounded local review.
rules.Chess validatedStandardChessInitialPosition(String fen) {
  // Reuse the archive's ASCII/159-byte/six-field syntax gate before the package.
  ArchivedPosition(ply: 0, fen: fen).validate();
  final fields = fen.split(' ');
  for (final field in fields.skip(4)) {
    if (!_counter.hasMatch(field) ||
        int.parse(field) > standardReplayMaximumInitialCounter) {
      throw const FormatException(
        'Replay FEN counter is outside the proposed bound.',
      );
    }
  }
  if (rules.Chess.validate_fen(fen)['valid'] != true) {
    throw const FormatException('Invalid standard-chess initial FEN.');
  }
  final chess = rules.Chess();
  if (!chess.load(fen) || chess.fen != fen) {
    throw const FormatException('Initial FEN is not canonical.');
  }
  bool has(String square, rules.PieceType type, rules.Color color) {
    final piece = chess.get(square);
    return piece?.type == type && piece?.color == color;
  }

  if (fields[0] == '8/8/8/8/8/8/8/8') {
    throw const FormatException(
      'An empty board is not a standard-chess position.',
    );
  }
  // Necessary material constraints, not a proof of historical reachability.
  for (final color in [rules.Chess.WHITE, rules.Chess.BLACK]) {
    final counts = <String, int>{};
    var total = 0;
    var lightBishops = 0;
    var darkBishops = 0;
    for (var rank = 1; rank <= 8; rank++) {
      for (var file = 0; file < 8; file++) {
        final piece = chess.get('${String.fromCharCode(97 + file)}$rank');
        if (piece == null || piece.color != color) continue;
        total++;
        counts.update(piece.type.name, (n) => n + 1, ifAbsent: () => 1);
        if (piece.type == rules.Chess.BISHOP) {
          if ((rank + file).isEven) {
            lightBishops++;
          } else {
            darkBishops++;
          }
        }
      }
    }
    int extra(int count, int originals) =>
        count > originals ? count - originals : 0;
    final pawns = counts['p'] ?? 0;
    final promotions =
        extra(counts['q'] ?? 0, 1) +
        extra(counts['r'] ?? 0, 2) +
        extra(counts['n'] ?? 0, 2) +
        extra(lightBishops, 1) +
        extra(darkBishops, 1);
    if (total > 16 || pawns > 8 || promotions > 8 - pawns) {
      throw const FormatException(
        'Initial material exceeds standard-chess bounds.',
      );
    }
  }
  // The package accepts a checked non-moving king in some mate positions.
  final nonMoving = chess.turn == rules.Chess.WHITE
      ? rules.Chess.BLACK
      : rules.Chess.WHITE;
  if (chess.king_attacked(nonMoving)) {
    throw const FormatException('The non-moving king cannot be in check.');
  }
  for (final right in ['K', 'Q', 'k', 'q']) {
    if (!fields[2].contains(right)) continue;
    final white = right == right.toUpperCase();
    final color = white ? rules.Chess.WHITE : rules.Chess.BLACK;
    final rank = white ? '1' : '8';
    final rookFile = right.toUpperCase() == 'K' ? 'h' : 'a';
    if (!has('e$rank', rules.Chess.KING, color) ||
        !has('$rookFile$rank', rules.Chess.ROOK, color)) {
      throw const FormatException(
        'Castling metadata lacks its home king or rook.',
      );
    }
  }
  final ep = fields[3];
  if (ep != '-') {
    final whiteToMove = fields[1] == 'w';
    final pawnRank = whiteToMove ? '5' : '4';
    final originRank = whiteToMove ? '7' : '2';
    final color = whiteToMove ? rules.Chess.BLACK : rules.Chess.WHITE;
    if (ep[1] != (whiteToMove ? '6' : '3') ||
        chess.get(ep) != null ||
        chess.get('${ep[0]}$originRank') != null ||
        !has('${ep[0]}$pawnRank', rules.Chess.PAWN, color) ||
        fields[4] != '0') {
      throw const FormatException('En-passant metadata is inconsistent.');
    }
  }
  return chess;
}
