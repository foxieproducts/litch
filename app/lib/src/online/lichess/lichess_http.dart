import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

typedef LichessRequestDeadline =
    Future<http.StreamedResponse?> Function(
      Future<http.StreamedResponse> response,
      Duration timeout,
    );

typedef LichessResponseBodyDeadline =
    Future<Uint8List?> Function(Future<Uint8List> body, Duration timeout);

enum LichessApiFailureKind {
  invalidRequest,
  invalidResponse,
  unavailableCredentials,
  unauthorized,
  forbidden,
  rateLimited,
  httpStatus,
  transport,
  secureStorage,
  disposed,
  streamOwnership,
}

final class LichessApiException implements Exception {
  const LichessApiException(this.kind, this.message, {this.statusCode});

  final LichessApiFailureKind kind;
  final String message;
  final int? statusCode;

  @override
  String toString() => message;
}

Future<http.StreamedResponse?> lichessDefaultRequestDeadline(
  Future<http.StreamedResponse> response,
  Duration timeout,
) async {
  try {
    return await response.timeout(timeout);
  } on TimeoutException {
    return null;
  }
}

Future<Uint8List?> lichessDefaultResponseBodyDeadline(
  Future<Uint8List> body,
  Duration timeout,
) async {
  try {
    return await body.timeout(timeout);
  } on TimeoutException {
    return null;
  }
}

Future<Uint8List> lichessReadBoundedResponseBody(
  Stream<List<int>> source, {
  required int maximumBytes,
  required Duration timeout,
  required LichessResponseBodyDeadline deadline,
  required bool Function() isClosed,
  required void Function() abort,
  required String oversizedMessage,
  required String deadlineMessage,
}) async {
  final Uint8List? body = await deadline(
    _collectBoundedResponseBody(
      source,
      maximumBytes: maximumBytes,
      isClosed: isClosed,
      oversizedMessage: oversizedMessage,
    ),
    timeout,
  );
  if (body != null) {
    return body;
  }
  abort();
  throw LichessApiException(LichessApiFailureKind.transport, deadlineMessage);
}

Future<Uint8List> _collectBoundedResponseBody(
  Stream<List<int>> source, {
  required int maximumBytes,
  required bool Function() isClosed,
  required String oversizedMessage,
}) async {
  final BytesBuilder bytes = BytesBuilder(copy: false);
  try {
    await for (final List<int> chunk in source) {
      if (bytes.length + chunk.length > maximumBytes) {
        throw LichessApiException(
          LichessApiFailureKind.invalidResponse,
          oversizedMessage,
        );
      }
      bytes.add(chunk);
    }
  } on LichessApiException {
    rethrow;
  } catch (_) {
    if (isClosed()) {
      throw const LichessApiException(
        LichessApiFailureKind.disposed,
        'The Lichess client is closed.',
      );
    }
    throw const LichessApiException(
      LichessApiFailureKind.transport,
      'The Lichess response stream was interrupted.',
    );
  }
  return bytes.takeBytes();
}

LichessApiFailureKind lichessFailureKindForStatus(int statusCode) {
  return switch (statusCode) {
    HttpStatus.unauthorized => LichessApiFailureKind.unauthorized,
    HttpStatus.forbidden => LichessApiFailureKind.forbidden,
    HttpStatus.tooManyRequests => LichessApiFailureKind.rateLimited,
    _ => LichessApiFailureKind.httpStatus,
  };
}

String lichessSafeStatusMessage(int statusCode) {
  return switch (statusCode) {
    HttpStatus.unauthorized =>
      'Lichess authorization has expired or was revoked.',
    HttpStatus.forbidden => 'Lichess denied this account or permission.',
    HttpStatus.tooManyRequests => 'Lichess is rate limiting requests.',
    _ => 'Lichess request failed with HTTP $statusCode.',
  };
}
