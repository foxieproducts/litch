/// One local UCI process/session. Implementations may use an isolate-backed
/// native library or a platform channel, but must preserve line ordering.
abstract interface class UciTransport {
  /// Complete ASCII lines, framed before decoding and capped at 4096 bytes.
  /// Lists exclude CR/LF delimiters and remain immutable through delivery.
  /// Implementations must enforce the cap while assembling chunks, retiring
  /// their source before an oversized line is accumulated or decoded.
  Stream<List<int>> get lines;

  Future<void> start();

  Future<void> send(String command);

  Future<void> close();
}
