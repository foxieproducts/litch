import '../models/standard_chess_replay.dart';
import 'chess_engine_models.dart';

abstract interface class EnginePositionValidator {
  ChessEngineVariant get variant;

  bool isCanonicalPosition(String fen);

  bool hasLegalMoves(String fen);

  bool isLegalLine(String fen, List<String> moves);
}

class StandardChessEnginePositionValidator implements EnginePositionValidator {
  const StandardChessEnginePositionValidator();

  @override
  ChessEngineVariant get variant => ChessEngineVariant.classic;

  @override
  bool isCanonicalPosition(String fen) {
    try {
      validatedStandardChessInitialPosition(fen);
      return true;
    } on FormatException {
      return false;
    }
  }

  @override
  bool hasLegalMoves(String fen) {
    try {
      return validatedStandardChessInitialPosition(
        fen,
      ).generate_moves().isNotEmpty;
    } on FormatException {
      return false;
    }
  }

  @override
  bool isLegalLine(String fen, List<String> moves) {
    try {
      final chess = validatedStandardChessInitialPosition(fen);
      for (final uci in moves) {
        if (uci.length < 4 || uci.length > 5) return false;
        if (!chess.move(<String, String>{
          'from': uci.substring(0, 2),
          'to': uci.substring(2, 4),
          if (uci.length == 5) 'promotion': uci[4],
        })) {
          return false;
        }
      }
      return true;
    } on FormatException {
      return false;
    } on RangeError {
      return false;
    }
  }
}
