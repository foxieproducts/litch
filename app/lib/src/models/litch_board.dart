class LitchBoard {
  const LitchBoard({
    required this.id,
    required this.name,
    required this.rssi,
  });

  final String id;
  final String name;
  final int rssi;

  String get shortId {
    if (id.length <= 5) {
      return id;
    }
    return id.substring(id.length - 5);
  }

  String get displayName => '$name • $shortId';
}
