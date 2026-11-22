import 'dart:async';
import 'dart:convert';
import 'dart:isolate';
import 'dart:typed_data';

import '../models/litchcraft_animation.dart';
import '../models/litchcraft_animation_renderer.dart';
import '../models/litchcraft_artwork.dart';
import '../models/litchcraft_state.dart';
import 'litchcraft_media_codec.dart';

const int litchcraftMediaMaximumEncodedBytes = 32 * 1024 * 1024;
// Buffered output needs both its chunks and the final concatenated byte list.
// Six MiB leaves four MiB of the working budget for rasters and codec scratch.
const int litchcraftMediaMaximumBufferedBytes = 6 * 1024 * 1024;
const int litchcraftMediaMaximumWorkingBytes = 16 * 1024 * 1024;
const int litchcraftMediaMaximumExportDurationMs = 10000;
const int litchcraftMediaMaximumExportFrames = 200;

enum LitchcraftMediaFormat { gif, apng }

enum LitchcraftMediaLoop { automatic, once, repeat }

enum LitchcraftMediaFit { fit, crop }

enum LitchcraftMediaSampling { smooth, crisp }

enum LitchcraftMediaTiming { source, chosenFps }

enum LitchcraftMediaErrorCode {
  malformed,
  unsupported,
  inputTooLarge,
  dimensionsTooLarge,
  frameLimit,
  outputTooLarge,
  invalidSettings,
  cancelled,
}

class LitchcraftMediaException implements Exception {
  const LitchcraftMediaException(this.code, this.message);
  final LitchcraftMediaErrorCode code;
  final String message;
  @override
  String toString() => message;
}

class LitchcraftMediaCancellation {
  final _cancelled = Completer<void>();
  final _listeners = <void Function()>{};
  bool get isCancelled => _cancelled.isCompleted;
  Future<void> get whenCancelled => _cancelled.future;
  void cancel() {
    if (isCancelled) return;
    _cancelled.complete();
    for (final listener in _listeners.toList()) {
      listener();
    }
    _listeners.clear();
  }

  void Function() _listen(void Function() listener) {
    if (isCancelled) {
      listener();
    } else {
      _listeners.add(listener);
    }
    return () => _listeners.remove(listener);
  }

  void check() {
    if (isCancelled) {
      throw const LitchcraftMediaException(
        LitchcraftMediaErrorCode.cancelled,
        'Conversion cancelled.',
      );
    }
  }
}

class LitchcraftMediaExportSettings {
  const LitchcraftMediaExportSettings({
    this.format = LitchcraftMediaFormat.apng,
    this.size = 512,
    this.fps = 20,
    this.durationMs,
    this.loop = LitchcraftMediaLoop.automatic,
    this.flipped = false,
  });
  final LitchcraftMediaFormat format;
  final int size;
  final int fps;
  final int? durationMs;
  final LitchcraftMediaLoop loop;
  final bool flipped;
}

class LitchcraftMediaExportPlan {
  const LitchcraftMediaExportPlan({
    required this.durationMs,
    required this.frameCount,
    required this.completePeriod,
    required this.repeats,
    required this.sampleDurationMs,
  });
  final int durationMs;
  final int frameCount;
  final int sampleDurationMs;
  final bool completePeriod;
  final bool repeats;
  bool get isPreview => !completePeriod;
  int frameDurationMs(int index) => index == frameCount - 1
      ? durationMs - index * sampleDurationMs
      : sampleDurationMs;
}

int _gcd(int a, int b) {
  while (b != 0) {
    final remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

/// Effects use the global clock, so a frame cycle alone is not a loop period.
/// Sparkle changes its deterministic slot each cycle and is always a preview.
LitchcraftMediaExportPlan planLitchcraftMediaExport(
  LitchcraftAnimationDocument document,
  LitchcraftMediaExportSettings settings,
) {
  if (!const [64, 128, 256, 512].contains(settings.size) ||
      !const [5, 10, 20].contains(settings.fps) ||
      (settings.durationMs != null &&
          (settings.durationMs! < 50 ||
              settings.durationMs! > 10000 ||
              (settings.format == LitchcraftMediaFormat.gif &&
                  settings.durationMs! % 10 != 0)))) {
    throw const LitchcraftMediaException(
      LitchcraftMediaErrorCode.invalidSettings,
      'Choose a supported export size, speed and duration.',
    );
  }
  var period = document.frames.length == 1
      ? 1
      : document.frames.fold<int>(0, (sum, frame) => sum + frame.durationMs);
  var sparkle = false;
  var animated = document.frames.length > 1;
  for (final frame in document.frames) {
    for (final emitter in frame.emitters) {
      if (emitter.color == 0 || emitter.effect == LitchcraftEffectStyle.off) {
        continue;
      }
      animated = true;
      sparkle |= emitter.effect == LitchcraftEffectStyle.sparkle;
      if (period <= 10000) {
        period =
            period ~/
            _gcd(period, emitter.effectDurationMs) *
            emitter.effectDurationMs;
      }
    }
  }
  final step = 1000 ~/ settings.fps;
  if (!animated) period = step;
  final hasPeriod =
      !sparkle &&
      period <= 10000 &&
      (settings.format != LitchcraftMediaFormat.gif || period % 10 == 0);
  final duration = settings.durationMs ?? (hasPeriod ? period : 10000);
  final complete = hasPeriod && duration % period == 0;
  final frames = (duration + step - 1) ~/ step;
  if (frames > litchcraftMediaMaximumExportFrames) {
    throw const LitchcraftMediaException(
      LitchcraftMediaErrorCode.frameLimit,
      'Export supports at most 200 samples.',
    );
  }
  return LitchcraftMediaExportPlan(
    durationMs: duration,
    frameCount: frames,
    sampleDurationMs: step,
    completePeriod: complete,
    repeats:
        settings.loop == LitchcraftMediaLoop.repeat ||
        (settings.loop == LitchcraftMediaLoop.automatic && complete),
  );
}

typedef LitchcraftRasterRenderer =
    Future<Uint8List> Function(LitchcraftCanvas canvas, int size, bool flipped);

/// Write and release each chunk before completing. The caller owns its file
/// and removes partial output on cancellation or error.
typedef LitchcraftEncodedChunkSink = Future<void> Function(Uint8List chunk);

class LitchcraftMediaExportResult {
  const LitchcraftMediaExportResult({
    required this.bytes,
    required this.plan,
    required this.format,
    required this.streamed,
    required this.encodedByteLength,
    required this.peakBufferedByteLength,
  });

  /// Empty when [streamed] is true; the sink owns the encoded output instead.
  final Uint8List bytes;
  final LitchcraftMediaExportPlan plan;
  final LitchcraftMediaFormat format;
  final bool streamed;
  final int encodedByteLength;
  final int peakBufferedByteLength;
  String get extension => format == LitchcraftMediaFormat.gif ? 'gif' : 'png';
  String get mimeType =>
      format == LitchcraftMediaFormat.gif ? 'image/gif' : 'image/apng';
}

/// The renderer is the editor's existing painter. Only one raster is requested
/// at a time; compression runs off the UI isolate and retains no frame history.
Future<LitchcraftMediaExportResult> exportLitchcraftMedia(
  LitchcraftAnimationDocument document, {
  LitchcraftMediaExportSettings settings =
      const LitchcraftMediaExportSettings(),
  required LitchcraftRasterRenderer renderRgba,
  LitchcraftEncodedChunkSink? encodedSink,
  LitchcraftMediaCancellation? token,
  void Function(double)? onProgress,
}) async {
  final cancellation = token ?? LitchcraftMediaCancellation();
  cancellation.check();
  final plan = planLitchcraftMediaExport(document, settings);
  final output = LitchcraftMediaContainerEncoder(
    settings,
    plan,
    retainBytes: encodedSink == null,
  );
  Future<void> flushChunks() async {
    if (encodedSink == null) return;
    for (final chunk in output.takeChunks()) {
      cancellation.check();
      await encodedSink(chunk);
      cancellation.check();
    }
  }

  final colors = List<int>.filled(256, 0);
  onProgress?.call(0);
  for (var index = 0; index < plan.frameCount; index++) {
    cancellation.check();
    composeLitchcraftAnimationDocumentInto(
      document,
      index * plan.sampleDurationMs,
      colors,
    );
    var raster = await renderRgba(
      LitchcraftCanvas(colors: colors, topology: document.topology),
      settings.size,
      settings.flipped,
    );
    cancellation.check();
    if (raster.length != settings.size * settings.size * 4) {
      throw const LitchcraftMediaException(
        LitchcraftMediaErrorCode.malformed,
        'The artwork renderer returned an incomplete image.',
      );
    }
    final transferred = TransferableTypedData.fromList([raster]);
    // Relinquish the UI raster before the worker allocates its palette raster.
    raster = Uint8List(0);
    final encoded = await _runMediaWorker<Uint8List>(<String, Object?>{
      'kind': 'encode',
      'raster': transferred,
      'size': settings.size,
      'format': settings.format,
      'duration': plan.frameDurationMs(index),
    }, cancellation);
    cancellation.check();
    output.addFrame(encoded, index);
    await flushChunks();
    onProgress?.call((index + 1) / plan.frameCount);
  }
  cancellation.check();
  final bytes = output.finish();
  await flushChunks();
  cancellation.check();
  return LitchcraftMediaExportResult(
    bytes: bytes,
    plan: plan,
    format: settings.format,
    streamed: encodedSink != null,
    encodedByteLength: output.encodedByteLength,
    peakBufferedByteLength: output.peakBufferedByteLength,
  );
}

class LitchcraftMediaImportSettings {
  const LitchcraftMediaImportSettings({
    this.fit = LitchcraftMediaFit.fit,
    this.sampling = LitchcraftMediaSampling.smooth,
    this.timing = LitchcraftMediaTiming.source,
    this.fps = 20,
    this.firstFrame = 0,
    this.lastFrame,
    this.matteColor = 0,
    this.flipped = false,
  });
  final LitchcraftMediaFit fit;
  final LitchcraftMediaSampling sampling;
  final LitchcraftMediaTiming timing;
  final int fps;
  final int firstFrame;
  final int? lastFrame;
  final int matteColor;
  final bool flipped;
}

class LitchcraftMediaMetadata {
  LitchcraftMediaMetadata({
    required this.format,
    required this.width,
    required this.height,
    required Iterable<int> durationsMs,
    required this.loopCount,
  }) : durationsMs = List<int>.unmodifiable(durationsMs);
  final LitchcraftMediaFormat format;
  final int width;
  final int height;
  final List<int> durationsMs;

  /// Total plays; zero is indefinite. GIF repeat counts are normalized here.
  final int loopCount;
  int get frameCount => durationsMs.length;
  bool get isAnimated => frameCount > 1;
  int get durationMs => durationsMs.fold(0, (sum, duration) => sum + duration);
}

LitchcraftMediaMetadata inspectLitchcraftMedia(Uint8List bytes) =>
    inspectLitchcraftMediaSource(bytes);

Future<LitchcraftMediaMetadata> inspectLitchcraftMediaAsync(
  Uint8List bytes, {
  LitchcraftMediaCancellation? token,
}) => _runMediaWorker<LitchcraftMediaMetadata>(<String, Object?>{
  'kind': 'inspect',
  'bytes': bytes,
}, token ?? LitchcraftMediaCancellation());

class LitchcraftMediaImportResult {
  const LitchcraftMediaImportResult({
    required this.document,
    required this.metadata,
    required this.timingAdapted,
  });
  final LitchcraftAnimationDocument document;
  final LitchcraftMediaMetadata metadata;
  final bool timingAdapted;
  LitchcraftCanvas get preview => LitchcraftCanvas(
    colors: document.frames.first.emitters.map((emitter) => emitter.color),
    topology: document.topology,
  );
}

Future<LitchcraftMediaImportResult> importLitchcraftMedia(
  Uint8List bytes, {
  LitchcraftMediaImportSettings settings =
      const LitchcraftMediaImportSettings(),
  LitchcraftTopology topology = LitchcraftTopology.v3,
  List<int> favoriteColors = litchcraftDefaultFavoriteColors,
  LitchcraftMediaCancellation? token,
  void Function(double)? onProgress,
}) => _runMediaWorker<LitchcraftMediaImportResult>(
  <String, Object?>{
    'kind': 'import',
    'bytes': bytes,
    'settings': settings,
    'topology': topology,
    'favorites': favoriteColors,
  },
  token ?? LitchcraftMediaCancellation(),
  onProgress: onProgress,
);

Uint8List encodeLitchcraftEditableArtwork(LitchcraftArtwork artwork) =>
    Uint8List.fromList(utf8.encode(jsonEncode(artwork.toJson())));

LitchcraftArtwork decodeLitchcraftEditableArtwork(Uint8List bytes) {
  if (bytes.length > litchcraftArtworkMaximumEnvelopeBytes) {
    throw const LitchcraftMediaException(
      LitchcraftMediaErrorCode.inputTooLarge,
      'Editable artwork exceeds 528 KiB.',
    );
  }
  try {
    final artwork = LitchcraftArtwork.fromJson(jsonDecode(utf8.decode(bytes)));
    if (!artwork.isSupported) {
      if (artwork.unavailableReason ==
          LitchcraftArtworkUnavailableReason.corruptAdvancedDocument) {
        throw const LitchcraftMediaException(
          LitchcraftMediaErrorCode.malformed,
          'The editable artwork is damaged or incomplete.',
        );
      }
      throw const LitchcraftMediaException(
        LitchcraftMediaErrorCode.unsupported,
        'This editable artwork is unavailable or uses an unsupported version.',
      );
    }
    return artwork;
  } on FormatException {
    throw const LitchcraftMediaException(
      LitchcraftMediaErrorCode.malformed,
      'The editable artwork is damaged or incomplete.',
    );
  }
}

Future<T> _runMediaWorker<T>(
  Map<String, Object?> request,
  LitchcraftMediaCancellation token, {
  void Function(double)? onProgress,
}) async {
  token.check();
  final port = ReceivePort();
  Isolate? worker;
  final result = Completer<T>();
  // Cancellation can arrive while spawn is still awaiting initialization.
  // Observe that error immediately; the caller still awaits the same future.
  result.future.ignore();
  final stopListening = token._listen(() {
    if (!result.isCompleted) {
      result.completeError(
        const LitchcraftMediaException(
          LitchcraftMediaErrorCode.cancelled,
          'Conversion cancelled.',
        ),
      );
    }
  });
  final subscription = port.listen((dynamic message) {
    if (message is List && message.first == 'progress') {
      onProgress?.call(message[1] as double);
    } else if (message is List && message.first == 'result') {
      if (!result.isCompleted) result.complete(message[1] as T);
    } else if (message is List && message.first == 'error') {
      if (!result.isCompleted) result.completeError(message[1] as Object);
    } else if (!result.isCompleted) {
      result.completeError(
        const LitchcraftMediaException(
          LitchcraftMediaErrorCode.malformed,
          'Image conversion failed.',
        ),
      );
    }
  });
  try {
    final send = port.sendPort;
    worker = await Isolate.spawn(
      _mediaWorkerEntry,
      <Object>[send, request],
      onError: send,
      onExit: send,
    );
    return await result.future;
  } finally {
    stopListening();
    worker?.kill(priority: Isolate.immediate);
    await subscription.cancel();
    port.close();
  }
}

void _mediaWorkerEntry(List<Object> arguments) {
  final send = arguments[0] as SendPort;
  final request = arguments[1] as Map<String, Object?>;
  try {
    final Object result;
    if (request['kind'] == 'encode') {
      result = encodeLitchcraftMediaRaster(
        (request['raster'] as TransferableTypedData)
            .materialize()
            .asUint8List(),
        request['size'] as int,
        request['format'] as LitchcraftMediaFormat,
        request['duration'] as int,
      );
    } else if (request['kind'] == 'inspect') {
      result = inspectLitchcraftMediaSource(request['bytes'] as Uint8List);
    } else {
      result = convertLitchcraftMediaSource(
        request['bytes'] as Uint8List,
        request['settings'] as LitchcraftMediaImportSettings,
        request['topology'] as LitchcraftTopology,
        request['favorites'] as List<int>,
        onProgress: (value) => send.send(['progress', value]),
      );
    }
    send.send(['result', result]);
  } catch (error) {
    send.send([
      'error',
      error is LitchcraftMediaException
          ? error
          : const LitchcraftMediaException(
              LitchcraftMediaErrorCode.malformed,
              'The image is damaged or incomplete.',
            ),
    ]);
  }
}
