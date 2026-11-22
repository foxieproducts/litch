import 'package:flutter/foundation.dart';

/// Shared orientation mapping for drawing, hit testing and media conversion.
int litchcraftLogicalSquareForDisplay(
  int displaySquare, {
  bool flipped = false,
}) => flipped ? 63 - displaySquare : displaySquare;

int litchcraftLogicalRegionForDisplay(
  int displayRegion, {
  bool flipped = false,
}) => flipped ? (displayRegion + 2) % 4 : displayRegion;

/// Centroids of the editor's left, top, right and bottom triangular regions.
const List<(double, double)> litchcraftEmitterSamplePoints = [
  (1 / 6, 1 / 2),
  (1 / 2, 1 / 6),
  (5 / 6, 1 / 2),
  (1 / 2, 5 / 6),
];

enum LitchcraftStatus {
  offline,
  unsupported,
  gameActive,
  boardNotEmpty,
  connecting,
  live,
  conflict,
  error,
}

enum LitchcraftFullSyncDirection { sendingToBoard, readingFromBoard }

enum LitchcraftTopology {
  v1(1),
  v2(2),
  v3(3);

  const LitchcraftTopology(this.id);
  final int id;
  int get emittersPerSquare => this == v3 ? 4 : 3;
  List<int> get targets => this == v3 ? const [1, 2, 4, 8] : const [1, 10, 4];
  static LitchcraftTopology fromId(int id) => switch (id) {
    1 => v1,
    2 => v2,
    3 => v3,
    _ => throw const FormatException('Unknown Litchcraft topology'),
  };
}

const List<int> litchcraftDefaultFavoriteColors = <int>[
  0xff7430,
  0xff3452,
  0xffcf2d,
  0x37d769,
  0x22c3e2,
  0x3f6aff,
  0xae53ff,
  0xffffff,
];

/// Intended RGB, before ambient brightness/current limiting. The four semantic
/// regions are left/top/right/bottom. Square zero is a8, matching board state.
/// Legacy boards combine top/bottom at output; documents retain both originals.
@immutable
class LitchcraftCanvas {
  LitchcraftCanvas({
    required Iterable<int> colors,
    this.topology = LitchcraftTopology.v3,
  }) : colors = List<int>.unmodifiable(colors) {
    if (this.colors.length != 256 ||
        this.colors.any((c) => c < 0 || c > 0xffffff)) {
      throw const FormatException('Invalid Litchcraft canvas');
    }
  }
  factory LitchcraftCanvas.blank({
    LitchcraftTopology topology = LitchcraftTopology.v3,
  }) => LitchcraftCanvas(colors: List<int>.filled(256, 0), topology: topology);

  final List<int> colors;
  final LitchcraftTopology topology;
  bool get hasMarks => colors.any((c) => c != 0);
  LitchcraftCanvas withTopology(LitchcraftTopology value) =>
      LitchcraftCanvas(colors: colors, topology: value);
  LitchcraftCanvas paint(Iterable<int> squares, int mask, int color) {
    if (mask < 1 || mask > 15 || color < 0 || color > 0xffffff) {
      throw ArgumentError('Invalid brush');
    }
    final result = colors.toList();
    for (final square in squares) {
      if (square < 0 || square >= 64) throw RangeError.range(square, 0, 63);
      for (var region = 0; region < 4; region++) {
        if ((mask & (1 << region)) != 0) result[square * 4 + region] = color;
      }
    }
    return LitchcraftCanvas(colors: result, topology: topology);
  }

  LitchcraftCanvas rotated(int quarterTurns) {
    final turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0) return this;
    final result = List<int>.filled(256, 0);
    for (var square = 0; square < 64; square++) {
      var x = square % 8;
      var y = square ~/ 8;
      for (var turn = 0; turn < turns; turn++) {
        final nextX = 7 - y;
        y = x;
        x = nextX;
      }
      final destinationSquare = y * 8 + x;
      for (var region = 0; region < 4; region++) {
        final destinationRegion = (region + turns) % 4;
        result[destinationSquare * 4 + destinationRegion] =
            colors[square * 4 + region];
      }
    }
    return LitchcraftCanvas(colors: result, topology: topology);
  }

  @override
  bool operator ==(Object other) =>
      other is LitchcraftCanvas &&
      topology == other.topology &&
      listEquals(colors, other.colors);
  @override
  int get hashCode => Object.hash(topology, Object.hashAll(colors));
}
