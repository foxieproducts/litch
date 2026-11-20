import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;

import '../constants/firmware_release.dart';
import '../models/firmware_manifest.dart';
import '../models/firmware_release_snapshot.dart';
import 'firmware_artifact_validator.dart';
import 'firmware_http_request_runner.dart';
import 'firmware_manifest_client.dart';

class FirmwareDownloadResult {
  const FirmwareDownloadResult({
    required this.file,
    required this.bytesReceived,
    required this.inspection,
  });

  final File file;
  final int bytesReceived;
  final FirmwareArtifactInspection inspection;
}

class FirmwarePreparedFile {
  const FirmwarePreparedFile({
    required this.file,
    required this.bytes,
    required this.downloaded,
    required this.inspection,
  });

  final File file;
  final int bytes;
  final bool downloaded;
  final FirmwareArtifactInspection inspection;
}

class FirmwareFetchException implements Exception {
  const FirmwareFetchException(this.message, {this.cause});

  final String message;
  final Object? cause;

  @override
  String toString() => message;
}

class FirmwareDownloadService {
  FirmwareDownloadService({
    http.Client? client,
    Directory? outputDirectory,
    FirmwareManifestClient? manifestClient,
    FirmwareRequestDeadlines deadlines = const FirmwareRequestDeadlines(),
  }) : _client = client ?? http.Client(),
       _outputDirectory = outputDirectory {
    _requestRunner = FirmwareHttpRequestRunner(_client, deadlines: deadlines);
    _manifestClient =
        manifestClient ?? FirmwareManifestClient(_client, deadlines: deadlines);
  }

  static const int _networkAttemptCount = 4;
  static const int _maximumNotesBytes = 256 * 1024;

  final http.Client _client;
  final Directory? _outputDirectory;
  late final FirmwareHttpRequestRunner _requestRunner;
  late final FirmwareManifestClient _manifestClient;

  Future<FirmwarePreparedFile> prepareFirmwareForUpdate(
    FirmwareReleaseSnapshot snapshot, {
    void Function(double progress)? onProgress,
  }) async {
    final FirmwareReleaseInfo release = snapshot.release;
    final FirmwareManifest manifest = snapshot.manifest;
    final Directory outputDir = _firmwareOutputDir();
    final File cachedFile = _immutableCacheFile(outputDir, snapshot);

    final cachedInspection = await _inspectArtifact(cachedFile, manifest);
    if (cachedInspection != null) {
      return _preparedCacheHit(cachedFile, cachedInspection);
    }

    final File partialFile = File('${cachedFile.path}.part');
    final partialInspection = await _inspectArtifact(partialFile, manifest);
    if (partialInspection != null) {
      await _promoteVerifiedPart(partialFile, cachedFile);
      return _preparedCacheHit(cachedFile, partialInspection);
    }
    if (await partialFile.exists()) {
      await partialFile.delete();
    }

    final List<File> legacyCandidates = <File>[
      File('${outputDir.path}/${release.fileName}'),
      File('${outputDir.path}/${release.fileName}.previous'),
    ];
    for (final File candidate in legacyCandidates) {
      if (await _inspectArtifact(candidate, manifest) == null) {
        continue;
      }
      await candidate.copy(partialFile.path);
      final inspection = await FirmwareArtifactValidator.inspectFile(
        partialFile,
        manifest,
      );
      FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
      await _promoteVerifiedPart(partialFile, cachedFile);
      return _preparedCacheHit(cachedFile, inspection);
    }

    final FirmwareDownloadResult fresh = await _downloadVerifiedArtifact(
      manifest: manifest,
      outFile: cachedFile,
      onProgress: onProgress,
    );
    return FirmwarePreparedFile(
      file: fresh.file,
      bytes: fresh.bytesReceived,
      downloaded: true,
      inspection: fresh.inspection,
    );
  }

  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    final FirmwareManifest manifest = await _fetchManifest(release);
    try {
      return FirmwareReleaseSnapshot(
        release: release,
        manifest: manifest,
        notesMarkdown: await _fetchVerifiedNotes(manifest),
      );
    } catch (error) {
      if (error is FirmwareFetchException) {
        rethrow;
      }
      throw FirmwareFetchException(
        'Unable to fetch firmware version notes: $error',
      );
    }
  }

  Future<FirmwareManifest> _fetchManifest(FirmwareReleaseInfo release) async {
    try {
      return await _manifestClient.fetch(release);
    } catch (error) {
      throw FirmwareFetchException(
        'Unable to fetch a trusted firmware manifest: $error',
      );
    }
  }

  Directory _firmwareOutputDir() {
    final Directory outputDir =
        _outputDirectory ??
        Directory('${Directory.systemTemp.path}/litch_firmware_downloads');
    if (!outputDir.existsSync()) {
      outputDir.createSync(recursive: true);
    }
    return outputDir;
  }

  File _immutableCacheFile(
    Directory outputDir,
    FirmwareReleaseSnapshot snapshot,
  ) {
    return File(
      '${outputDir.path}/firmware-${snapshot.release.channel.name}-'
      '${snapshot.manifest.artifact.sha256}.bin',
    );
  }

  Future<FirmwareArtifactInspection?> _inspectArtifact(
    File file,
    FirmwareManifest manifest,
  ) async {
    if (!await file.exists()) {
      return null;
    }
    final FirmwareArtifactInspection inspection;
    try {
      inspection = await FirmwareArtifactValidator.inspectFile(file, manifest);
    } catch (_) {
      return null;
    }
    // A complete selected hash cannot gain a different policy by refetching.
    // Keep this refusal outside the corrupt/missing-cache recovery path.
    FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
    return inspection;
  }

  FirmwarePreparedFile _preparedCacheHit(
    File file,
    FirmwareArtifactInspection inspection,
  ) {
    return FirmwarePreparedFile(
      file: file,
      bytes: inspection.sizeBytes,
      downloaded: false,
      inspection: inspection,
    );
  }

  Future<FirmwareDownloadResult> _downloadVerifiedArtifact({
    required FirmwareManifest manifest,
    required File outFile,
    void Function(double progress)? onProgress,
  }) async {
    Object? lastError;
    for (int attempt = 1; attempt <= _networkAttemptCount; ++attempt) {
      final File partialFile = File('${outFile.path}.part');
      bool partialVerified = false;
      try {
        if (await partialFile.exists()) {
          await partialFile.delete();
        }

        final http.StreamedResponse response = await _requestRunner.get(
          manifest.artifact.url,
          kind: FirmwareRequestKind.artifact,
          headers: const <String, String>{
            'Accept': 'application/octet-stream',
            'Cache-Control': 'no-cache',
          },
        );
        if (response.statusCode != HttpStatus.ok) {
          await response.stream.drain<void>();
          throw HttpException(
            'Firmware request failed (${response.statusCode}).',
            uri: manifest.artifact.url,
          );
        }
        // Content-Length may describe compressed wire bytes. Validate the
        // decoded stream against the manifest below.

        final IOSink sink = partialFile.openWrite();
        int bytesReceived = 0;
        try {
          await for (final List<int> chunk in response.stream) {
            bytesReceived += chunk.length;
            if (bytesReceived > manifest.artifact.sizeBytes) {
              throw const FirmwareArtifactValidationException(
                'Firmware response exceeds the manifest size.',
              );
            }
            sink.add(chunk);
            onProgress?.call(bytesReceived / manifest.artifact.sizeBytes);
          }
          await sink.flush();
        } finally {
          await sink.close();
        }
        if (bytesReceived != manifest.artifact.sizeBytes) {
          throw const FirmwareArtifactValidationException(
            'Firmware response is shorter than the manifest size.',
          );
        }

        final inspection = await FirmwareArtifactValidator.inspectFile(
          partialFile,
          manifest,
        );
        FirmwareArtifactValidator.requireSdkOnlyUpdate(inspection);
        partialVerified = true;
        await _promoteVerifiedPart(partialFile, outFile);
        onProgress?.call(1);
        return FirmwareDownloadResult(
          file: outFile,
          bytesReceived: bytesReceived,
          inspection: inspection,
        );
      } catch (error) {
        lastError = error;
        if (!partialVerified && await partialFile.exists()) {
          await partialFile.delete();
        }
        if (_isRetryableNetworkError(error) && attempt < _networkAttemptCount) {
          await Future<void>.delayed(Duration(milliseconds: 250 * attempt));
          continue;
        }
        break;
      }
    }

    throw FirmwareFetchException(
      'Unable to download trusted firmware from '
      '${manifest.artifact.url}. Last error: $lastError',
      cause: lastError,
    );
  }

  Future<void> _promoteVerifiedPart(File partialFile, File outFile) async {
    if (await outFile.exists()) {
      await outFile.delete();
    }
    await partialFile.rename(outFile.path);
  }

  Future<String> _fetchVerifiedNotes(FirmwareManifest manifest) async {
    final http.StreamedResponse response = await _requestRunner.get(
      manifest.notes.url,
      kind: FirmwareRequestKind.metadata,
      headers: const <String, String>{
        'Accept': 'text/markdown,text/plain;q=0.9',
        'Cache-Control': 'no-cache',
      },
    );
    if (response.statusCode != HttpStatus.ok) {
      await response.stream.drain<void>();
      throw FirmwareFetchException(
        'Version notes request failed (${response.statusCode}).',
      );
    }
    // IOClient can decode gzip while retaining its encoded Content-Length.
    // The manifest describes the decoded bytes, checked below.

    final BytesBuilder body = BytesBuilder(copy: false);
    await for (final List<int> chunk in response.stream) {
      body.add(chunk);
      if (body.length > manifest.notes.sizeBytes ||
          body.length > _maximumNotesBytes) {
        throw const FirmwareFetchException(
          'Firmware version notes are too large.',
        );
      }
    }
    final Uint8List bytes = body.takeBytes();
    if (bytes.length != manifest.notes.sizeBytes ||
        sha256.convert(bytes).toString() != manifest.notes.sha256) {
      throw const FirmwareFetchException(
        'Firmware version notes do not match the manifest.',
      );
    }
    try {
      return utf8.decode(bytes);
    } on FormatException catch (error) {
      throw FirmwareFetchException(
        'Firmware version notes are not UTF-8: $error',
      );
    }
  }

  bool _isRetryableNetworkError(Object error) {
    if (error is SocketException || error is HandshakeException) {
      return true;
    }
    if (error is http.ClientException) {
      return true;
    }
    if (error is HttpException) {
      final String text = error.message.toLowerCase();
      return text.contains('connection closed') || text.contains('timed out');
    }
    return false;
  }

  void dispose() {
    _client.close();
  }
}
