import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';

import 'litchcraft_animation.dart';
import 'litchcraft_animation_codec.dart';
import 'litchcraft_state.dart';
import 'user_text.dart';

const int litchcraftArtworkMaximumEnvelopeBytes = 528 * 1024;

enum LitchcraftArtworkUnavailableReason {
  unsupportedSchema,
  corruptAdvancedDocument,
}

class LitchcraftUnsupportedArtwork implements Exception {
  const LitchcraftUnsupportedArtwork(this.reason);

  final LitchcraftArtworkUnavailableReason reason;
}

LitchcraftAnimationDocument _staticDocument(
  LitchcraftCanvas canvas,
  Iterable<int> favoriteColors,
) => LitchcraftAnimationDocument(
  frames: <LitchcraftAnimationFrame>[
    LitchcraftAnimationFrame(
      emitters: canvas.colors.map(
        (color) => LitchcraftAnimatedEmitter(color: color),
      ),
    ),
  ],
  topology: canvas.topology,
  favoriteColors: favoriteColors,
);

LitchcraftAnimationDocument _updatedDocument(
  LitchcraftAnimationDocument source, {
  LitchcraftCanvas? canvas,
  Iterable<int>? favoriteColors,
}) {
  final frames = source.frames.toList();
  if (canvas != null) {
    final first = frames.first;
    frames[0] = LitchcraftAnimationFrame(
      durationMs: first.durationMs,
      emitters: List<LitchcraftAnimatedEmitter>.generate(
        litchcraftAnimationEmitterCount,
        (index) => LitchcraftAnimatedEmitter(
          color: canvas.colors[index],
          effect: first.emitters[index].effect,
          effectDurationMs: first.emitters[index].effectDurationMs,
        ),
      ),
    );
  }
  return LitchcraftAnimationDocument(
    frames: frames,
    // Controller canvases may be adapted to a lower-detail board. Their RGB
    // values are an editable target view; source topology remains document
    // authority until the user creates a different artwork.
    topology: source.topology,
    favoriteColors: favoriteColors ?? source.favoriteColors,
    playbackOnEntry: source.playbackOnEntry,
    sparkleSeed: source.sparkleSeed,
  );
}

/// Editable intent colors and animation, before board presentation mapping.
class LitchcraftArtwork {
  factory LitchcraftArtwork({
    required String id,
    required String name,
    required LitchcraftCanvas canvas,
    required DateTime createdAt,
    required DateTime modifiedAt,
    List<int> favoriteColors = litchcraftDefaultFavoriteColors,
  }) => LitchcraftArtwork.animated(
    id: id,
    name: name,
    document: _staticDocument(canvas, favoriteColors),
    createdAt: createdAt,
    modifiedAt: modifiedAt,
  );

  LitchcraftArtwork.animated({
    required this.id,
    required this.name,
    required LitchcraftAnimationDocument document,
    required this.createdAt,
    required this.modifiedAt,
  }) : _document = document,
       _retainedSource = null,
       _preview = null,
       _storedFavorites = null,
       _storedFrameCount = null,
       storageReference = null,
       _resolver = null,
       unavailableReason = null {
    _validateMetadata();
  }

  LitchcraftArtwork._unavailable({
    required this.id,
    required this.name,
    required this.createdAt,
    required this.modifiedAt,
    required Map<String, Object?> retainedSource,
    required this.unavailableReason,
  }) : _document = null,
       _retainedSource = Map<String, Object?>.unmodifiable(retainedSource),
       _preview = null,
       _storedFavorites = null,
       _storedFrameCount = null,
       storageReference = null,
       _resolver = null {
    _validateMetadata();
  }

  /// A catalog entry keeps only the first-frame thumbnail and a bounded file
  /// reference. Resolving does not cache the decoded animation on this entry.
  LitchcraftArtwork.deferred({
    required this.id,
    required this.name,
    required this.createdAt,
    required this.modifiedAt,
    required int frameCount,
    required LitchcraftCanvas? preview,
    required List<int> favoriteColors,
    required this.storageReference,
    required Future<LitchcraftArtwork> Function() resolver,
    this.unavailableReason,
  }) : _document = null,
       _retainedSource = null,
       _preview = preview,
       _storedFavorites = List<int>.unmodifiable(favoriteColors),
       _storedFrameCount = frameCount,
       _resolver = resolver {
    _validateMetadata();
    if (unavailableReason == null &&
        (preview == null ||
            frameCount < 1 ||
            frameCount > litchcraftAnimationMaximumFrameCount ||
            favoriteColors.length != 8 ||
            favoriteColors.any((color) => color < 0 || color > 0xffffff))) {
      throw const FormatException('Invalid stored artwork preview.');
    }
    if (unavailableReason != null &&
        (preview != null || frameCount != 0 || favoriteColors.isNotEmpty)) {
      throw const FormatException('Invalid unavailable artwork preview.');
    }
  }

  void _validateMetadata() {
    if (!RegExp(r'^[a-zA-Z0-9_-]{1,80}$').hasMatch(id) ||
        name.trim().isEmpty ||
        name.length > 80 ||
        !isSafeDisplayText(name) ||
        !createdAt.isUtc ||
        !modifiedAt.isUtc ||
        modifiedAt.isBefore(createdAt)) {
      throw const FormatException('Invalid artwork document.');
    }
  }

  final String id;
  final String name;
  final LitchcraftAnimationDocument? _document;
  final Map<String, Object?>? _retainedSource;
  final LitchcraftCanvas? _preview;
  final List<int>? _storedFavorites;
  final int? _storedFrameCount;
  final Future<LitchcraftArtwork> Function()? _resolver;

  /// Opaque storage ownership; callers should use [resolve] or the store's
  /// compactArtwork method rather than inspecting this value.
  final Object? storageReference;
  final LitchcraftArtworkUnavailableReason? unavailableReason;
  final DateTime createdAt;
  final DateTime modifiedAt;

  bool get isSupported => unavailableReason == null;
  bool get isDeferred => _resolver != null;
  int get frameCount => _storedFrameCount ?? _document?.frames.length ?? 0;

  Future<LitchcraftArtwork> resolve() async {
    final resolver = _resolver;
    if (resolver == null) return this;
    final loaded = await resolver();
    return loaded.copyWith(
      id: id,
      name: name,
      createdAt: createdAt,
      modifiedAt: modifiedAt,
    );
  }

  LitchcraftAnimationDocument get document {
    if (isDeferred) {
      throw StateError('Resolve stored artwork before reading its document.');
    }
    final value = _document;
    if (value == null) {
      throw LitchcraftUnsupportedArtwork(unavailableReason!);
    }
    return value;
  }

  LitchcraftCanvas get canvas =>
      _preview ??
      LitchcraftCanvas(
        colors: document.frames.first.emitters.map((emitter) => emitter.color),
        topology: document.topology,
      );
  List<int> get favoriteColors => _storedFavorites ?? document.favoriteColors;

  bool get _usesStaticEnvelope {
    if (!isSupported ||
        document.frames.length != 1 ||
        document.frames.single.durationMs !=
            litchcraftAnimationDefaultFrameDurationMs ||
        document.playbackOnEntry ||
        document.sparkleSeed != 1) {
      return false;
    }
    return document.frames.single.emitters.every(
      (emitter) =>
          emitter.effect == LitchcraftEffectStyle.off &&
          emitter.effectDurationMs == 0,
    );
  }

  LitchcraftArtwork copyWith({
    String? id,
    String? name,
    LitchcraftAnimationDocument? document,
    LitchcraftCanvas? canvas,
    List<int>? favoriteColors,
    DateTime? createdAt,
    DateTime? modifiedAt,
  }) {
    if (document == null && canvas == null && favoriteColors == null) {
      if (isDeferred) {
        return LitchcraftArtwork.deferred(
          id: id ?? this.id,
          name: name ?? this.name,
          createdAt: createdAt ?? this.createdAt,
          modifiedAt: modifiedAt ?? this.modifiedAt,
          frameCount: frameCount,
          preview: _preview,
          favoriteColors: _storedFavorites!,
          storageReference: storageReference,
          resolver: _resolver!,
          unavailableReason: unavailableReason,
        );
      }
      if (!isSupported) {
        return LitchcraftArtwork._unavailable(
          id: id ?? this.id,
          name: name ?? this.name,
          createdAt: createdAt ?? this.createdAt,
          modifiedAt: modifiedAt ?? this.modifiedAt,
          retainedSource: <String, Object?>{
            ..._retainedSource!,
            'id': id ?? this.id,
            'name': name ?? this.name,
            'createdAt': (createdAt ?? this.createdAt).toIso8601String(),
            'modifiedAt': (modifiedAt ?? this.modifiedAt).toIso8601String(),
          },
          unavailableReason: unavailableReason,
        );
      }
    }
    if (!isSupported) {
      throw LitchcraftUnsupportedArtwork(unavailableReason!);
    }
    if (document != null && (canvas != null || favoriteColors != null)) {
      throw ArgumentError(
        'A replacement document cannot be combined with static fields.',
      );
    }
    final nextDocument =
        document ??
        _updatedDocument(
          this.document,
          canvas: canvas,
          favoriteColors: favoriteColors,
        );
    return LitchcraftArtwork.animated(
      id: id ?? this.id,
      name: name ?? this.name,
      document: nextDocument,
      createdAt: createdAt ?? this.createdAt,
      modifiedAt: modifiedAt ?? this.modifiedAt,
    );
  }

  Map<String, Object?> toJson() {
    if (isDeferred) {
      throw StateError(
        'Resolve stored artwork before serializing its payload.',
      );
    }
    if (!isSupported) {
      return jsonDecode(jsonEncode(_retainedSource)) as Map<String, Object?>;
    }
    if (_usesStaticEnvelope) {
      return <String, Object?>{
        'version': 2,
        'id': id,
        'name': name,
        'createdAt': createdAt.toIso8601String(),
        'modifiedAt': modifiedAt.toIso8601String(),
        'topology': document.topology.name,
        'mapping': 'a8-first-left-top-right-bottom',
        'colors': canvas.colors,
        'favoriteColors': favoriteColors,
      };
    }
    final encoded = encodeLitchcraftAnimationDocument(
      document,
      localStorage: true,
    );
    final envelope = <String, Object?>{
      'version': encoded.bytes.first,
      'id': id,
      'name': name,
      'createdAt': createdAt.toIso8601String(),
      'modifiedAt': modifiedAt.toIso8601String(),
      'mapping': 'a8-first-left-top-right-bottom',
      'document': base64Encode(encoded.bytes),
      'crc32': encoded.crc32,
    };
    if (utf8.encode(jsonEncode(envelope)).length >
        litchcraftArtworkMaximumEnvelopeBytes) {
      throw const FormatException('Artwork exceeds storage bound.');
    }
    return envelope;
  }

  factory LitchcraftArtwork.fromJson(Object? json) {
    if (json is! Map<String, dynamic> ||
        json['version'] is! int ||
        json['mapping'] != 'a8-first-left-top-right-bottom' ||
        json['id'] is! String ||
        json['name'] is! String ||
        json['createdAt'] is! String ||
        json['modifiedAt'] is! String) {
      throw const FormatException('Unsupported artwork document.');
    }
    if (utf8.encode(jsonEncode(json)).length >
        litchcraftArtworkMaximumEnvelopeBytes) {
      throw const FormatException('Artwork exceeds storage bound.');
    }
    final created = DateTime.tryParse(json['createdAt'] as String);
    final modified = DateTime.tryParse(json['modifiedAt'] as String);
    if (created == null || modified == null) {
      throw const FormatException('Invalid artwork metadata.');
    }
    final version = json['version'] as int;
    if (version > 4) {
      return LitchcraftArtwork._unavailable(
        id: json['id'] as String,
        name: json['name'] as String,
        createdAt: created,
        modifiedAt: modified,
        retainedSource: Map<String, Object?>.from(json),
        unavailableReason: LitchcraftArtworkUnavailableReason.unsupportedSchema,
      );
    }
    if (version == litchcraftAnimationSchema || version == 4) {
      try {
        if (json['document'] is! String || json['crc32'] is! int) {
          throw const FormatException('Invalid animated artwork envelope.');
        }
        return LitchcraftArtwork.animated(
          id: json['id'] as String,
          name: json['name'] as String,
          document: decodeLitchcraftAnimationDocument(
            base64Decode(json['document'] as String),
            json['crc32'] as int,
            localStorage: true,
          ),
          createdAt: created,
          modifiedAt: modified,
        );
      } on FormatException {
        return LitchcraftArtwork._unavailable(
          id: json['id'] as String,
          name: json['name'] as String,
          createdAt: created,
          modifiedAt: modified,
          retainedSource: Map<String, Object?>.from(json),
          unavailableReason:
              LitchcraftArtworkUnavailableReason.corruptAdvancedDocument,
        );
      }
    }
    if (version != 1 && version != 2 || json['colors'] is! List) {
      throw const FormatException('Unsupported artwork document.');
    }
    final topology = LitchcraftTopology.values
        .where((value) => value.name == json['topology'])
        .firstOrNull;
    final rawColors = json['colors'] as List;
    final rawFavorites = version == 2 ? json['favoriteColors'] : null;
    if (topology == null ||
        rawColors.length != litchcraftAnimationEmitterCount ||
        rawColors.any((color) => color is! int) ||
        (version == 2 &&
            (rawFavorites is! List ||
                rawFavorites.length != 8 ||
                rawFavorites.any((color) => color is! int)))) {
      throw const FormatException('Invalid artwork canvas.');
    }
    return LitchcraftArtwork(
      id: json['id'] as String,
      name: json['name'] as String,
      canvas: LitchcraftCanvas(
        colors: rawColors.cast<int>(),
        topology: topology,
      ),
      favoriteColors: version == 2
          ? (rawFavorites as List).cast<int>()
          : litchcraftDefaultFavoriteColors,
      createdAt: created,
      modifiedAt: modified,
    );
  }
}

bool sameLitchcraftArtwork(LitchcraftCanvas left, LitchcraftCanvas right) =>
    listEquals(left.colors, right.colors);
