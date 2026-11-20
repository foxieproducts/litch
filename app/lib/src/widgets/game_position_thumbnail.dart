import 'package:flutter/material.dart';

import '../models/game_archive.dart';
import '../models/home_board_snapshot.dart';

/// A paint-only saved-position preview that keeps long game lists inexpensive.
class GamePositionThumbnail extends StatelessWidget {
  const GamePositionThumbnail({
    super.key,
    required this.archive,
    this.size = 88,
  });

  final GameArchive archive;
  final double size;

  @override
  Widget build(BuildContext context) {
    final ArchivedPosition position = archive.positions.last;
    final HomeChessPosition? parsed = HomeChessPosition.tryParse(position.fen);
    return ExcludeSemantics(
      child: SizedBox.square(
        dimension: size,
        child: ClipRRect(
          borderRadius: BorderRadius.circular(8),
          child: CustomPaint(
            key: ValueKey<String>('game-thumbnail-${archive.libraryId}'),
            painter: _PositionThumbnailPainter(
              fen: position.fen,
              pieces: parsed?.pieces ?? const <int, String>{},
              duckSquare: position.duckSquare,
            ),
          ),
        ),
      ),
    );
  }
}

class _PositionThumbnailPainter extends CustomPainter {
  const _PositionThumbnailPainter({
    required this.fen,
    required this.pieces,
    required this.duckSquare,
  });

  final String fen;
  final Map<int, String> pieces;
  final int? duckSquare;

  static const Color _lightSquare = Color(0xFFE8EEF3);
  static const Color _darkSquare = Color(0xFF526980);
  static const Color _p1Piece = Color(0xFFF7F1E4);
  static const Color _p2Piece = Color(0xFF203342);
  static const Color _duck = Color(0xFFF4B942);

  @override
  void paint(Canvas canvas, Size size) {
    final double square = size.shortestSide / 8;
    final Paint paint = Paint();
    for (int boardSquare = 0; boardSquare < 64; boardSquare += 1) {
      final int row = boardSquare ~/ 8;
      final int column = boardSquare % 8;
      final Offset origin = Offset(column * square, row * square);
      paint.color = (row + column).isEven ? _lightSquare : _darkSquare;
      canvas.drawRect(origin & Size.square(square), paint);

      final String? piece = pieces[boardSquare];
      if (piece != null) {
        final bool p1 = piece == piece.toUpperCase();
        paint.color = p1 ? _p1Piece : _p2Piece;
        canvas.drawCircle(
          origin + Offset(square / 2, square / 2),
          square * 0.37,
          paint,
        );
        final TextPainter label = TextPainter(
          text: TextSpan(
            text: piece.toUpperCase(),
            style: TextStyle(
              color: p1 ? _p2Piece : _p1Piece,
              fontSize: square * 0.53,
              fontWeight: FontWeight.w800,
              height: 1,
            ),
          ),
          textDirection: TextDirection.ltr,
        )..layout();
        label.paint(
          canvas,
          origin +
              Offset((square - label.width) / 2, (square - label.height) / 2),
        );
        label.dispose();
      }
      if (duckSquare == boardSquare) {
        paint.color = _duck;
        canvas.drawCircle(
          origin + Offset(square * 0.76, square * 0.24),
          square * 0.18,
          paint,
        );
      }
    }
  }

  @override
  bool shouldRepaint(_PositionThumbnailPainter oldDelegate) =>
      oldDelegate.fen != fen || oldDelegate.duckSquare != duckSquare;
}
