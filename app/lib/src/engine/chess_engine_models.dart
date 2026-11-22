import 'dart:convert';

import 'package:flutter/foundation.dart';

enum ChessEngineVariant { classic, duck }

enum EngineSearchPurpose { computerMove, postGameAnalysis, explicitAnalysis }

enum EnginePrivacyClass { onDevice, remote }

enum EngineScorePerspective { white }

enum EngineFailureReason {
  cancelled,
  unavailable,
  timedOut,
  busy,
  unsupported,
  invalidRequest,
  invalidResponse,
  rateLimited,
  transport,
  prohibited,
}

@immutable
class EngineCapabilities {
  EngineCapabilities({
    required this.providerId,
    required this.engineVersion,
    required this.privacyClass,
    required Set<ChessEngineVariant> variants,
    required Set<EngineSearchPurpose> purposes,
    this.supportsMoveTime = true,
    this.supportsNodes = false,
    this.supportsDepth = false,
    this.supportsScores = true,
    this.supportsCancellation = true,
    this.maximumMultiPv = 1,
    this.maximumPvPlies = 16,
    this.maximumRequestBytes = 4096,
  }) : variants = Set<ChessEngineVariant>.unmodifiable(variants),
       purposes = Set<EngineSearchPurpose>.unmodifiable(purposes) {
    if (!_boundedAscii(providerId, 64) || !_boundedAscii(engineVersion, 64)) {
      throw ArgumentError('Engine identity must not be empty.');
    }
    if (this.variants.isEmpty ||
        this.purposes.isEmpty ||
        maximumMultiPv < 1 ||
        maximumMultiPv > 3 ||
        maximumPvPlies < 1 ||
        maximumPvPlies > 16 ||
        maximumRequestBytes < 256 ||
        maximumRequestBytes > 65536) {
      throw ArgumentError('Engine capability bounds are invalid.');
    }
  }

  final String providerId;
  final String engineVersion;
  final EnginePrivacyClass privacyClass;
  final Set<ChessEngineVariant> variants;
  final Set<EngineSearchPurpose> purposes;
  final bool supportsMoveTime;
  final bool supportsNodes;
  final bool supportsDepth;
  final bool supportsScores;
  final bool supportsCancellation;
  final int maximumMultiPv;
  final int maximumPvPlies;
  final int maximumRequestBytes;
}

@immutable
class EngineTimeControl {
  EngineTimeControl({
    required this.whiteRemaining,
    required this.blackRemaining,
    required this.whiteIncrement,
    required this.blackIncrement,
    this.hardMoveBudget,
  }) {
    if (whiteRemaining.isNegative ||
        blackRemaining.isNegative ||
        whiteIncrement.isNegative ||
        blackIncrement.isNegative ||
        whiteRemaining > const Duration(days: 7) ||
        blackRemaining > const Duration(days: 7) ||
        whiteIncrement > const Duration(hours: 1) ||
        blackIncrement > const Duration(hours: 1)) {
      throw ArgumentError('Engine clock values must not be negative.');
    }
    if (hardMoveBudget != null &&
        (hardMoveBudget! <= Duration.zero ||
            hardMoveBudget! > const Duration(minutes: 10))) {
      throw ArgumentError('Engine deadline must be positive.');
    }
  }

  final Duration whiteRemaining;
  final Duration blackRemaining;
  final Duration whiteIncrement;
  final Duration blackIncrement;
  final Duration? hardMoveBudget;
}

@immutable
class EngineSearchLimits {
  EngineSearchLimits({
    this.moveTime,
    this.nodes,
    this.depth,
    this.multiPv = 1,
  }) {
    if ((moveTime != null && moveTime! <= Duration.zero) ||
        (nodes != null && nodes! <= 0) ||
        (depth != null && depth! <= 0)) {
      throw ArgumentError('Every supplied engine limit must be positive.');
    }
    if ((moveTime == null || moveTime! <= Duration.zero) &&
        (nodes == null || nodes! <= 0) &&
        (depth == null || depth! <= 0)) {
      throw ArgumentError('At least one positive engine limit is required.');
    }
    if (multiPv < 1 || multiPv > 3) {
      throw ArgumentError.value(multiPv, 'multiPv');
    }
    if ((moveTime != null && moveTime! > const Duration(minutes: 10)) ||
        (nodes != null && nodes! > 1000000000) ||
        (depth != null && depth! > 256)) {
      throw ArgumentError('Engine search limit is too large.');
    }
  }

  final Duration? moveTime;
  final int? nodes;
  final int? depth;
  final int multiPv;
}

@immutable
class EngineSearchRequest {
  EngineSearchRequest({
    required this.jobId,
    required this.sessionId,
    required this.positionRevision,
    required this.fen,
    required this.variant,
    required this.purpose,
    required this.limits,
    this.timeControl,
    this.seed = 0,
  }) {
    if (!_boundedAscii(jobId, 64) ||
        sessionId < 0 ||
        sessionId > 0x7fffffff ||
        positionRevision < 0 ||
        positionRevision > 0x7fffffff ||
        seed < 0 ||
        seed > 0xffffffff) {
      throw ArgumentError('Engine request identity is invalid.');
    }
    if (!_boundedAscii(fen, 159)) {
      throw ArgumentError('Engine FEN is empty or oversized.');
    }
  }

  final String jobId;
  final int sessionId;
  final int positionRevision;
  final String fen;
  final ChessEngineVariant variant;
  final EngineSearchPurpose purpose;
  final EngineSearchLimits limits;
  final EngineTimeControl? timeControl;
  final int seed;
}

class EngineCancellation {
  bool _isCancelled = false;

  bool get isCancelled => _isCancelled;

  void cancel() => _isCancelled = true;

  void throwIfCancelled() {
    if (_isCancelled) throw const EngineSearchCancelled();
  }
}

class EngineSearchCancelled implements Exception {
  const EngineSearchCancelled();
}

sealed class EngineSearchEvent {
  const EngineSearchEvent({
    required this.jobId,
    required this.sessionId,
    required this.positionRevision,
  });

  final String jobId;
  final int sessionId;
  final int positionRevision;
}

final class EngineSearchStarted extends EngineSearchEvent {
  const EngineSearchStarted({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
  });
}

@immutable
class EngineScore {
  EngineScore.centipawns(int value)
    : this._checked(value: value, maximumMagnitude: 100000);
  EngineScore.mate(int mateIn)
    : this._checked(mateIn: mateIn, maximumMagnitude: 512);
  EngineScore._checked({
    this.value,
    this.mateIn,
    required int maximumMagnitude,
  }) {
    final score = value ?? mateIn!;
    if (score.abs() > maximumMagnitude) {
      throw ArgumentError('Engine score is out of bounds.');
    }
  }

  final int? value;
  final int? mateIn;
  final EngineScorePerspective perspective = EngineScorePerspective.white;
}

final class EngineSearchInfo extends EngineSearchEvent {
  EngineSearchInfo({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
    required this.depth,
    required this.nodes,
    required this.elapsed,
    required this.score,
    required List<String> principalVariation,
    this.multiPv = 1,
  }) : principalVariation = List<String>.unmodifiable(principalVariation) {
    if (depth < 0 ||
        depth > 256 ||
        nodes < 0 ||
        nodes > 1000000000 ||
        elapsed.isNegative ||
        elapsed > const Duration(minutes: 10) ||
        multiPv < 1 ||
        multiPv > 3 ||
        this.principalVariation.length > 16) {
      throw ArgumentError('Engine search information is out of bounds.');
    }
  }

  final int depth;
  final int nodes;
  final Duration elapsed;
  final EngineScore score;
  final List<String> principalVariation;
  final int multiPv;
}

final class EngineBestMove extends EngineSearchEvent {
  const EngineBestMove({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
    required this.uci,
    this.ponder,
  });

  final String uci;
  final String? ponder;
}

/// A successful terminal-position search for which no legal move exists.
final class EngineNoLegalMove extends EngineSearchEvent {
  const EngineNoLegalMove({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
  });
}

final class EngineSearchCompleted extends EngineSearchEvent {
  const EngineSearchCompleted({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
  });
}

final class EngineSearchFailed extends EngineSearchEvent {
  EngineSearchFailed({
    required super.jobId,
    required super.sessionId,
    required super.positionRevision,
    required this.reason,
    this.retryAfter,
  }) {
    if ((reason == EngineFailureReason.rateLimited) != (retryAfter != null) ||
        (retryAfter != null &&
            (retryAfter! <= Duration.zero ||
                retryAfter! > const Duration(days: 1)))) {
      throw ArgumentError('Engine retry delay is invalid for this failure.');
    }
  }

  final EngineFailureReason reason;
  final Duration? retryAfter;
}

bool _boundedAscii(String value, int maximumBytes) {
  if (value.isEmpty || utf8.encode(value).length > maximumBytes) return false;
  return value.codeUnits.every((unit) => unit >= 0x20 && unit <= 0x7e);
}
