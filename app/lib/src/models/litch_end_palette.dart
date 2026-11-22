class LitchEndPalette {
  const LitchEndPalette({
    required this.p1ControlsWheel,
    required this.p2ControlsWheel,
    required this.usesDirectPhysicalKeys,
  });

  final int p1ControlsWheel;
  final int p2ControlsWheel;
  final bool usesDirectPhysicalKeys;

  /// Firmware keeps the two end hues at least 32 steps apart on the wheel.
  /// A named choice takes the closest allowed value, preferring the lower
  /// boundary on a tie. A finger drag continues in its current direction.
  static int resolveWheel(
    int proposed,
    int other, {
    int min = 0,
    int max = 255,
    int? dragFrom,
  }) {
    const separation = 32;
    final span = max - min + 1;
    int wrap(int value) => min + (value - min) % span;
    int distance(int a, int b) {
      final direct = (a - b).abs();
      return direct < span - direct ? direct : span - direct;
    }

    final wheel = wrap(proposed);
    final opposite = wrap(other);
    if (distance(wheel, opposite) >= separation) return wheel;
    final lower = wrap(opposite - separation);
    final upper = wrap(opposite + separation);
    if (dragFrom != null) return proposed >= dragFrom ? upper : lower;
    return distance(wheel, lower) <= distance(wheel, upper) ? lower : upper;
  }
}
