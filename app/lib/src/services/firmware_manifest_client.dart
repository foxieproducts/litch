import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;

import '../constants/firmware_release.dart';
import '../models/firmware_manifest.dart';
import 'firmware_http_request_runner.dart';

class FirmwareManifestFetchException implements Exception {
  const FirmwareManifestFetchException(this.message);

  final String message;

  @override
  String toString() => message;
}

class FirmwareManifestClient {
  FirmwareManifestClient(
    http.Client client, {
    FirmwareRequestDeadlines deadlines = const FirmwareRequestDeadlines(),
  }) : _requestRunner = FirmwareHttpRequestRunner(client, deadlines: deadlines);

  static const int _maximumManifestBytes = 32 * 1024;

  final FirmwareHttpRequestRunner _requestRunner;

  Future<FirmwareManifest> fetch(FirmwareReleaseInfo release) async {
    final http.StreamedResponse response;
    try {
      response = await _requestRunner.get(
        release.manifestUrl,
        kind: FirmwareRequestKind.metadata,
        headers: const <String, String>{
          'Accept': 'application/json',
          'Cache-Control': 'no-cache',
        },
      );
    } catch (error) {
      throw FirmwareManifestFetchException(
        'Unable to fetch firmware manifest: $error',
      );
    }
    if (response.statusCode != HttpStatus.ok) {
      await response.stream.drain<void>();
      throw FirmwareManifestFetchException(
        'Firmware manifest request failed (${response.statusCode}).',
      );
    }
    final int? declaredLength = response.contentLength;
    if (declaredLength != null && declaredLength > _maximumManifestBytes) {
      await response.stream.drain<void>();
      throw const FirmwareManifestFetchException(
        'Firmware manifest is too large.',
      );
    }

    final BytesBuilder body = BytesBuilder(copy: false);
    await for (final List<int> chunk in response.stream) {
      body.add(chunk);
      if (body.length > _maximumManifestBytes) {
        throw const FirmwareManifestFetchException(
          'Firmware manifest is too large.',
        );
      }
    }
    final Uint8List bytes = body.takeBytes();
    final String source;
    try {
      source = utf8.decode(bytes);
    } on FormatException catch (error) {
      throw FirmwareManifestFetchException(
        'Firmware manifest is not UTF-8: $error',
      );
    }
    return FirmwareManifest.parse(
      source,
      manifestUrl: release.manifestUrl,
      expectedChannel: release.channel.name,
    );
  }
}
