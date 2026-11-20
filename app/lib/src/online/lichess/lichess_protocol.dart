enum LichessProtocolFailureKind {
  malformedUtf8,
  malformedJson,
  nonObjectJson,
  oversizedLine,
  missingField,
  invalidField,
  unexpectedFirstEvent,
}

final class LichessProtocolException implements Exception {
  const LichessProtocolException(this.kind, this.message);

  final LichessProtocolFailureKind kind;
  final String message;

  @override
  String toString() => message;
}
