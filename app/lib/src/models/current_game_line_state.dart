/// Durable current-line availability, published by the board's existing
/// settings worker through the shared replaceable State lane.
///
/// The revision identifies a changed durable line, including Undo or a new
/// game with the same position. It is not a game identity or an archive receipt.
class CurrentGameLineState {
  const CurrentGameLineState({required this.revision, required this.present});

  static const int stateCode = 45;
  static const int version = 1;
  static const int bodyBytes = 2;

  final int revision;
  final bool present;

  static CurrentGameLineState? tryParse({
    required int revision,
    required List<int> body,
  }) {
    if (revision <= 0 ||
        revision >= 0xffffffff ||
        body.length != bodyBytes ||
        body[0] != version ||
        (body[1] != 0 && body[1] != 1)) {
      return null;
    }
    return CurrentGameLineState(revision: revision, present: body[1] == 1);
  }
}
