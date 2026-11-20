import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../localization/litch_strings.dart';
import '../models/litchcraft_animation.dart';
import '../models/litchcraft_state.dart';
import '../services/litchcraft_media.dart';
import 'litch_workspace_geometry.dart';
import 'litchcraft_canvas.dart';

enum LitchcraftShareFormat { still, editable, apng, gif }

class LitchcraftShareChoice {
  const LitchcraftShareChoice(this.format, this.settings);
  final LitchcraftShareFormat format;
  final LitchcraftMediaExportSettings settings;
}

Future<LitchcraftShareChoice?> showLitchcraftShareOptions(
  BuildContext context,
  LitchcraftAnimationDocument document, {
  bool flipped = false,
}) => showDialog<LitchcraftShareChoice>(
  context: context,
  builder: (_) => _ShareOptions(document: document, flipped: flipped),
);

class _ShareOptions extends StatefulWidget {
  const _ShareOptions({required this.document, required this.flipped});
  final LitchcraftAnimationDocument document;
  final bool flipped;
  @override
  State<_ShareOptions> createState() => _ShareOptionsState();
}

class _ShareOptionsState extends State<_ShareOptions> {
  late LitchcraftShareFormat _format =
      widget.document.frames.length > 1 ||
          widget.document.frames.any(
            (frame) => frame.emitters.any(
              (emitter) => emitter.effect != LitchcraftEffectStyle.off,
            ),
          )
      ? LitchcraftShareFormat.apng
      : LitchcraftShareFormat.still;
  late int _size = _format == LitchcraftShareFormat.still ? 1024 : 512;
  int _fps = 20;
  int? _duration;
  LitchcraftMediaLoop _loop = LitchcraftMediaLoop.automatic;
  late bool _flipped = widget.flipped;

  LitchcraftMediaExportSettings get _settings => LitchcraftMediaExportSettings(
    format: _format == LitchcraftShareFormat.gif
        ? LitchcraftMediaFormat.gif
        : LitchcraftMediaFormat.apng,
    size: _size,
    fps: _fps,
    durationMs: _duration,
    loop: _loop,
    flipped: _flipped,
  );

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final formats = <LitchcraftShareFormat, String>{
      LitchcraftShareFormat.still: strings.craftStillImage,
      LitchcraftShareFormat.editable: strings.craftExportEditableArtwork,
      LitchcraftShareFormat.apng: strings.craftAnimatedPng,
      LitchcraftShareFormat.gif: strings.craftGif,
    }.entries.toList()..sort((a, b) => compareLitchLabels(a.value, b.value));
    final animated =
        _format == LitchcraftShareFormat.apng ||
        _format == LitchcraftShareFormat.gif;
    final plan = animated
        ? planLitchcraftMediaExport(widget.document, _settings)
        : null;
    final wide = _hasMediaColumns(context);
    return AlertDialog(
      constraints: wide ? _wideDialogConstraints : null,
      title: Text(strings.craftExportOptions),
      content: _mediaColumns(
        wide: wide,
        stackedGap: _format == LitchcraftShareFormat.still ? 0 : 12,
        leading: [
          _choice<LitchcraftShareFormat>(
            'craft-export-format',
            strings.craftExportFormat,
            _format,
            Map.fromEntries(formats),
            (value) => setState(() {
              _format = value;
              if (value != LitchcraftShareFormat.still && _size > 512) {
                _size = 512;
              }
            }),
          ),
          if (_format != LitchcraftShareFormat.editable) ...[
            const SizedBox(height: 12),
            _choice<int>(
              'craft-export-size',
              strings.craftResolution,
              _size,
              {
                for (final size in [64, 128, 256, 512, if (!animated) 1024])
                  size: '$size × $size',
              },
              (value) => setState(() => _size = value),
            ),
          ],
          if (animated) ...[
            const SizedBox(height: 12),
            _choice<int>(
              'craft-export-fps',
              strings.craftFrameRate,
              _fps,
              {
                for (final fps in [5, 10, 20]) fps: '$fps fps',
              },
              (value) => setState(() => _fps = value),
            ),
          ],
        ],
        trailing: [
          if (_format == LitchcraftShareFormat.editable)
            Text(strings.craftEditableNotice)
          else ...[
            if (animated) ...[
              _choice<int>(
                'craft-export-duration',
                strings.craftExportDuration,
                _duration ?? 0,
                {
                  0: strings.craftLoopAuto,
                  for (final seconds in [1, 2, 5, 10])
                    seconds * 1000: '$seconds s',
                },
                (value) =>
                    setState(() => _duration = value == 0 ? null : value),
              ),
              const SizedBox(height: 12),
              _choice<LitchcraftMediaLoop>(
                'craft-export-loop',
                strings.craftLooping,
                _loop,
                {
                  LitchcraftMediaLoop.automatic: strings.craftLoopAuto,
                  LitchcraftMediaLoop.once: strings.craftPlayOnce,
                  LitchcraftMediaLoop.repeat: strings.craftRepeat,
                },
                (value) => setState(() => _loop = value),
              ),
              const SizedBox(height: 12),
              Text(
                strings.craftMediaSummary(
                  (plan!.durationMs / 1000).toString(),
                  plan.frameCount,
                ),
                key: const ValueKey('craft-export-summary'),
              ),
              if (plan.isPreview)
                Padding(
                  padding: const EdgeInsets.only(top: 8),
                  child: Text(strings.craftExportPreviewNotice),
                ),
              if (_format == LitchcraftShareFormat.gif)
                Padding(
                  padding: const EdgeInsets.only(top: 8),
                  child: Text(strings.craftGifPaletteNotice),
                ),
            ],
            SwitchListTile.adaptive(
              key: const ValueKey('craft-export-flipped'),
              contentPadding: EdgeInsets.zero,
              title: Text(strings.craftFlipExport),
              value: _flipped,
              onChanged: (value) => setState(() => _flipped = value),
            ),
          ],
        ],
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(strings.cancelAction),
        ),
        FilledButton(
          key: const ValueKey('craft-export-share'),
          onPressed: () => Navigator.of(
            context,
          ).pop(LitchcraftShareChoice(_format, _settings)),
          child: Text(strings.craftExportShare),
        ),
      ],
    );
  }
}

const _wideDialogConstraints = BoxConstraints(maxWidth: 832);

bool _hasMediaColumns(BuildContext context) {
  final window = MediaQuery.sizeOf(context);
  final width = window.width - MediaQuery.paddingOf(context).horizontal;
  // Leave room for two usable control columns and the dialog's side margins.
  return LitchWorkspaceGeometry.isTabletWindow(window) && width >= 800;
}

Widget _mediaColumns({
  required bool wide,
  required List<Widget> leading,
  required List<Widget> trailing,
  double stackedGap = 12,
}) {
  Widget scrollPane(List<Widget> children) => SingleChildScrollView(
    primary: wide ? false : null,
    child: Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: children,
    ),
  );
  return SizedBox(
    width: wide ? 784 : 380,
    child: wide
        ? Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Expanded(child: scrollPane(leading)),
              const SizedBox(width: 24),
              Expanded(child: scrollPane(trailing)),
            ],
          )
        : scrollPane([...leading, SizedBox(height: stackedGap), ...trailing]),
  );
}

Widget _choice<T extends Object>(
  String key,
  String label,
  T value,
  Map<T, String> choices,
  ValueChanged<T> onChanged,
) {
  if (choices.length <= 3) {
    return Column(
      key: ValueKey(key),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(label),
        RadioGroup<T>(
          groupValue: value,
          onChanged: (next) {
            if (next != null) onChanged(next);
          },
          child: Column(
            children: [
              for (final choice in choices.entries)
                RadioListTile<T>(
                  key: ValueKey('$key:${choice.key}'),
                  value: choice.key,
                  title: Text(choice.value),
                  contentPadding: EdgeInsets.zero,
                  visualDensity: VisualDensity.standard,
                ),
            ],
          ),
        ),
      ],
    );
  }
  return DropdownButtonFormField<T>(
    itemHeight: null,
    key: ValueKey('$key:$value'),
    initialValue: value,
    isDense: false,
    isExpanded: true,
    decoration: InputDecoration(label: Text(label)),
    items: [
      for (final choice in choices.entries)
        DropdownMenuItem(value: choice.key, child: Text(choice.value)),
    ],
    onChanged: (next) {
      if (next != null) onChanged(next);
    },
  );
}

/// A conversion owns only its temporary output. Dismissal cancels its worker;
/// an error offers retry without applying or discarding an artwork.
Future<T?> runLitchcraftMediaJob<T>(
  BuildContext context,
  Future<T> Function(LitchcraftMediaCancellation, void Function(double)) job,
) => showDialog<T>(
  context: context,
  barrierDismissible: false,
  builder: (_) => _MediaJob<T>(job: job),
);

class _MediaJob<T> extends StatefulWidget {
  const _MediaJob({required this.job});
  final Future<T> Function(LitchcraftMediaCancellation, void Function(double))
  job;
  @override
  State<_MediaJob<T>> createState() => _MediaJobState<T>();
}

class _MediaJobState<T> extends State<_MediaJob<T>> {
  LitchcraftMediaCancellation? _token;
  double _progress = 0;
  Object? _error;
  @override
  void initState() {
    super.initState();
    _run();
  }

  @override
  void dispose() {
    _token?.cancel();
    super.dispose();
  }

  Future<void> _run() async {
    final token = _token = LitchcraftMediaCancellation();
    setState(() {
      _error = null;
      _progress = 0;
    });
    try {
      final result = await widget.job(token, (value) {
        if (mounted && !token.isCancelled) setState(() => _progress = value);
      });
      if (mounted && !token.isCancelled) Navigator.of(context).pop(result);
    } catch (error) {
      if (mounted && !token.isCancelled) setState(() => _error = error);
    }
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return AlertDialog(
      title: Text(strings.craftMediaBusy),
      content: _error == null
          ? LinearProgressIndicator(value: _progress)
          : Text(strings.craftMediaFailed),
      actions: [
        TextButton(
          key: const ValueKey('craft-media-cancel'),
          onPressed: () {
            _token?.cancel();
            Navigator.of(context).pop();
          },
          child: Text(strings.cancelAction),
        ),
        if (_error != null)
          FilledButton(onPressed: _run, child: Text(strings.tryAgain)),
      ],
    );
  }
}

Future<LitchcraftMediaImportResult?> showLitchcraftImportOptions(
  BuildContext context,
  Uint8List bytes,
  LitchcraftMediaMetadata metadata, {
  required LitchcraftTopology topology,
  required List<int> favoriteColors,
  bool flipped = false,
}) => showDialog<LitchcraftMediaImportResult>(
  context: context,
  builder: (_) => _ImportOptions(
    bytes: bytes,
    metadata: metadata,
    topology: topology,
    favoriteColors: favoriteColors,
    flipped: flipped,
  ),
);

class _ImportOptions extends StatefulWidget {
  const _ImportOptions({
    required this.bytes,
    required this.metadata,
    required this.topology,
    required this.favoriteColors,
    required this.flipped,
  });
  final Uint8List bytes;
  final LitchcraftMediaMetadata metadata;
  final LitchcraftTopology topology;
  final List<int> favoriteColors;
  final bool flipped;
  @override
  State<_ImportOptions> createState() => _ImportOptionsState();
}

class _ImportOptionsState extends State<_ImportOptions> {
  LitchcraftMediaFit _fit = LitchcraftMediaFit.fit;
  LitchcraftMediaSampling _sampling = LitchcraftMediaSampling.smooth;
  LitchcraftMediaTiming _timing = LitchcraftMediaTiming.source;
  int _fps = 20;
  int _matte = 0;
  late final _first = TextEditingController(text: '1');
  late final _last = TextEditingController(
    text: '${widget.metadata.frameCount}',
  );
  LitchcraftMediaCancellation? _token;
  LitchcraftMediaImportResult? _result;
  Object? _error;
  bool _busy = true;
  bool _invalidRange = false;
  double _progress = 0;
  int _generation = 0;

  @override
  void initState() {
    super.initState();
    _convert();
  }

  @override
  void dispose() {
    _token?.cancel();
    _first.dispose();
    _last.dispose();
    super.dispose();
  }

  Future<void> _convert() async {
    _token?.cancel();
    final generation = ++_generation;
    final first = int.tryParse(_first.text);
    final last = int.tryParse(_last.text);
    final valid =
        first != null &&
        last != null &&
        first >= 1 &&
        last >= first &&
        last <= widget.metadata.frameCount;
    setState(() {
      _result = null;
      _error = null;
      _busy = valid;
      _invalidRange = !valid;
      _progress = 0;
    });
    if (!valid) return;
    final token = _token = LitchcraftMediaCancellation();
    try {
      final result = await importLitchcraftMedia(
        widget.bytes,
        topology: widget.topology,
        favoriteColors: widget.favoriteColors,
        token: token,
        settings: LitchcraftMediaImportSettings(
          fit: _fit,
          sampling: _sampling,
          timing: _timing,
          fps: _fps,
          matteColor: _matte,
          firstFrame: first - 1,
          lastFrame: last - 1,
          flipped: widget.flipped,
        ),
        onProgress: (value) {
          if (mounted && generation == _generation) {
            setState(() => _progress = value);
          }
        },
      );
      if (mounted && generation == _generation) {
        setState(() {
          _result = result;
          _busy = false;
        });
      }
    } catch (error) {
      if (mounted && generation == _generation && !token.isCancelled) {
        setState(() {
          _error = error;
          _busy = false;
        });
      }
    }
  }

  void _change(VoidCallback update) {
    update();
    _convert();
  }

  String _conversionError(BuildContext context) {
    final strings = litchStrings(context);
    if (_error is LitchcraftMediaException) {
      final code = (_error as LitchcraftMediaException).code;
      if (code == LitchcraftMediaErrorCode.frameLimit) {
        return strings.craftImportFrameLimitHelp;
      }
      if (const [
        LitchcraftMediaErrorCode.inputTooLarge,
        LitchcraftMediaErrorCode.dimensionsTooLarge,
        LitchcraftMediaErrorCode.outputTooLarge,
      ].contains(code)) {
        return strings.craftMediaTooLarge;
      }
    }
    return strings.craftMediaFailed;
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final result = _result;
    final wide = _hasMediaColumns(context);
    return AlertDialog(
      constraints: wide ? _wideDialogConstraints : null,
      title: Text(strings.craftImportOptions),
      content: _mediaColumns(
        wide: wide,
        leading: [
          Text('${widget.metadata.width} × ${widget.metadata.height}'),
          Text(
            strings.craftImportSourceSummary(
              (widget.metadata.durationMs / 1000).toString(),
              widget.metadata.frameCount,
            ),
          ),
          const SizedBox(height: 12),
          if (result != null) ...[
            Text(
              strings.craftImportConvertedSummary(
                (result.document.frames.fold<int>(
                          0,
                          (sum, frame) => sum + frame.durationMs,
                        ) /
                        1000)
                    .toString(),
                result.document.frames.length,
              ),
              key: const ValueKey('craft-import-summary'),
            ),
            Text(strings.craftImportFirstFramePreview),
            Center(
              child: SizedBox.square(
                dimension: wide ? 192 : 144,
                child: Semantics(
                  label: strings.craftImportPreview,
                  child: LitchcraftThumbnail(
                    blackBackground: true,
                    canvas: result.preview,
                    flipped: widget.flipped,
                  ),
                ),
              ),
            ),
          ],
          if (_busy) ...[
            LinearProgressIndicator(value: _progress),
            Text(strings.craftMediaBusy),
          ],
          const SizedBox(height: 12),
          Text(strings.craftRasterImportNotice),
        ],
        trailing: [
          _choice<LitchcraftMediaFit>(
            'craft-import-fit',
            strings.craftImportFit,
            _fit,
            {
              LitchcraftMediaFit.fit: strings.craftFit,
              LitchcraftMediaFit.crop: strings.craftCrop,
            },
            (value) => _change(() => _fit = value),
          ),
          const SizedBox(height: 12),
          _choice<LitchcraftMediaSampling>(
            'craft-import-sampling',
            strings.craftSampling,
            _sampling,
            {
              LitchcraftMediaSampling.smooth: strings.craftSmooth,
              LitchcraftMediaSampling.crisp: strings.craftCrisp,
            },
            (value) => _change(() => _sampling = value),
          ),
          const SizedBox(height: 12),
          _choice<int>(
            'craft-import-matte',
            strings.craftMatte,
            _matte,
            {0: strings.craftBlack, 0xffffff: strings.craftWhite},
            (value) => _change(() => _matte = value),
          ),
          if (widget.metadata.frameCount > 1) ...[
            const SizedBox(height: 12),
            LayoutBuilder(
              builder: (context, bounds) {
                final minimumWidth =
                    9 * MediaQuery.textScalerOf(context).scale(16);
                final tablet = LitchWorkspaceGeometry.isTabletWindow(
                  MediaQuery.sizeOf(context),
                );
                final stack = tablet && bounds.maxWidth < 2 * minimumWidth + 12;
                final fieldWidth = stack
                    ? bounds.maxWidth
                    : (bounds.maxWidth - 12) / 2;
                Widget frameField(
                  String id,
                  String label,
                  TextEditingController controller,
                ) => SizedBox(
                  width: fieldWidth,
                  child: TextField(
                    key: ValueKey('craft-import-$id'),
                    controller: controller,
                    keyboardType: TextInputType.number,
                    inputFormatters: [FilteringTextInputFormatter.digitsOnly],
                    decoration: InputDecoration(label: Text(label)),
                    onChanged: (_) => _convert(),
                  ),
                );
                return Wrap(
                  spacing: 12,
                  runSpacing: 12,
                  children: [
                    frameField('first', strings.craftImportFirstFrame, _first),
                    frameField('last', strings.craftImportLastFrame, _last),
                  ],
                );
              },
            ),
            if (_invalidRange)
              Semantics(
                liveRegion: true,
                child: Text(
                  strings.craftImportFrameRangeInvalid(
                    widget.metadata.frameCount,
                  ),
                  key: const ValueKey('craft-import-range-error'),
                  style: TextStyle(color: Theme.of(context).colorScheme.error),
                ),
              ),
            const SizedBox(height: 12),
            _choice<LitchcraftMediaTiming>(
              'craft-import-timing',
              strings.craftTiming,
              _timing,
              {
                LitchcraftMediaTiming.source: strings.craftSourceTiming,
                LitchcraftMediaTiming.chosenFps: strings.craftFrameRate,
              },
              (value) => _change(() => _timing = value),
            ),
            if (_timing == LitchcraftMediaTiming.chosenFps) ...[
              const SizedBox(height: 12),
              _choice<int>(
                'craft-import-fps',
                strings.craftFrameRate,
                _fps,
                {
                  for (final fps in [5, 10, 20]) fps: '$fps fps',
                },
                (value) => _change(() => _fps = value),
              ),
            ],
          ],
          if (result?.timingAdapted == true)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Text(strings.craftImportAdapted),
            ),
        ],
      ),
      actions: [
        if (_error != null)
          SizedBox(
            width: double.infinity,
            child: Semantics(
              liveRegion: true,
              child: Text(
                _conversionError(context),
                key: const ValueKey('craft-import-error'),
                style: TextStyle(color: Theme.of(context).colorScheme.error),
              ),
            ),
          ),
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(strings.cancelAction),
        ),
        FilledButton(
          key: const ValueKey('craft-import-accept'),
          onPressed: result == null || _busy
              ? null
              : () => Navigator.of(context).pop(result),
          child: Text(strings.craftImport),
        ),
      ],
    );
  }
}
