import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import 'firmware_http_request_runner.dart';
import 'firmware_provenance_service.dart';

/// Recognition comes only from the protected publication origin, never from
/// an ordinary release catalog or a receipt supplied by the board or user.
class HttpFirmwareProvenanceVerifier implements FirmwareProvenanceVerifier {
  const HttpFirmwareProvenanceVerifier({http.Client? client})
    : _client = client;

  final http.Client? _client;
  static const int _maximumBytes = 1024;

  @override
  Future<bool?> isOfficialDigest(String sha256) async {
    if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(sha256)) return null;
    final client = _client ?? http.Client();
    try {
      final response = await FirmwareHttpRequestRunner(client).get(
        Uri.parse(
          'https://updates.foxieproducts.com/fw/provenance_$sha256.json',
        ),
        kind: FirmwareRequestKind.metadata,
        headers: const {
          'Accept': 'application/json',
          'Cache-Control': 'no-cache',
        },
      );
      if (response.statusCode != HttpStatus.ok ||
          (response.contentLength ?? 0) > _maximumBytes) {
        await response.stream.listen(null).cancel();
        return null;
      }
      final body = BytesBuilder(copy: false);
      await for (final chunk in response.stream) {
        if (body.length + chunk.length > _maximumBytes) return null;
        body.add(chunk);
      }
      final record = jsonDecode(utf8.decode(body.takeBytes()));
      if (record is! Map<String, dynamic> ||
          record['schema_version'] is! int ||
          record['schema_version'] != 1 ||
          record['image_sha256'] != sha256) {
        return null;
      }
      return switch (record['status']) {
        'official' => true,
        'custom' => false,
        'unknown' => null,
        _ => null,
      };
    } catch (_) {
      return null;
    } finally {
      if (_client == null) client.close();
    }
  }
}
