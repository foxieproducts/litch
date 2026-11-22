import 'dart:typed_data';

import '../models/litch_control_action.dart';

class LitchRequestIdAllocator {
  LitchRequestIdAllocator({int initialRequestId = 1, this.maxReserved = 32})
    : assert(initialRequestId >= 1 && initialRequestId <= 0xffff),
      assert(maxReserved >= 1 && maxReserved <= 0xffff),
      _nextRequestId = initialRequestId;

  static const int maxRequestId = 0xffff;

  final int maxReserved;
  final Set<int> _reserved = <int>{};
  int _nextRequestId;

  int get reservedCount => _reserved.length;

  bool isReserved(int requestId) => _reserved.contains(requestId);

  int? reserve() {
    if (_reserved.length >= maxReserved) {
      return null;
    }

    for (int attempt = 0; attempt < maxRequestId; ++attempt) {
      final int candidate = _nextRequestId;
      _nextRequestId = candidate == maxRequestId ? 1 : candidate + 1;
      if (_reserved.add(candidate)) {
        return candidate;
      }
    }
    return null;
  }

  void release(int requestId) {
    _reserved.remove(requestId);
  }

  void clear() {
    _reserved.clear();
  }
}

class LitchSettingOutcome {
  const LitchSettingOutcome({
    required this.ok,
    required this.reason,
    required this.hasValue,
    required this.isCompact,
    this.requestId,
    this.key,
    this.value,
  });

  final int? requestId;
  final String? key;
  final bool ok;
  final LitchMutationReason reason;
  final bool hasValue;
  final Object? value;
  final bool isCompact;
}

class LitchControlProtocol {
  const LitchControlProtocol._();

  static const int connectionActionOpcode = 25;
  static const int connectionActionVersion = 1;
  static const int connectionConsoleOpcode = 26;
  static const int connectionConsoleBegin = 1;
  static const int connectionConsoleEnd = 2;
  static const int connectionConsoleMaximumBytes = 0xff;

  static List<Uint8List> encodeConnectionConsoleRecord(List<int> bytes) {
    if (bytes.isEmpty || bytes.length > connectionConsoleMaximumBytes) {
      throw RangeError.range(
        bytes.length,
        1,
        connectionConsoleMaximumBytes,
        'bytes.length',
      );
    }
    if (bytes.any((int value) => value < 0 || value > 0xff || value == 0)) {
      throw ArgumentError('Console records require non-NUL bytes.');
    }
    final records = <Uint8List>[];
    var offset = 0;
    while (offset < bytes.length) {
      final bool begins = offset == 0;
      final int headerBytes = begins ? 3 : 2;
      final int payloadBytes = (bytes.length - offset).clamp(
        1,
        10 - headerBytes,
      );
      final bool ends = offset + payloadBytes == bytes.length;
      records.add(
        Uint8List.fromList(<int>[
          connectionActionVersion,
          (begins ? connectionConsoleBegin : 0) |
              (ends ? connectionConsoleEnd : 0),
          if (begins) bytes.length,
          ...bytes.sublist(offset, offset + payloadBytes),
        ]),
      );
      offset += payloadBytes;
    }
    return records;
  }

  /// Explicit board-storage recovery, carried by the existing Ordered action
  /// owner. The app must obtain user confirmation before submitting it.
  static Uint8List encodeConnectionStorageReformat() =>
      Uint8List.fromList(<int>[connectionActionVersion, 5, 0, 0, 1]);

  /// Explicit app sleep uses the existing typed board-control action.
  /// Code 5 remains storage reformat; Sleep is never a legacy JSON action.
  static Uint8List encodeConnectionSleep() =>
      Uint8List.fromList(<int>[connectionActionVersion, 6, 0, 0, 1]);

  static Uint8List encodeConnectionAction({
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) {
    final int actionCode = action.index + 1;
    final int ownerCode = owner == null ? 0 : owner.index + 1;
    final int coachingCode = enabled == null
        ? 0
        : enabled
        ? 2
        : 1;
    final bool canonical = switch (action) {
      LitchControlAction.hint || LitchControlAction.newGame =>
        owner == null && enabled == null && undoCount == 1,
      LitchControlAction.undo =>
        owner == null && enabled == null && undoCount >= 1 && undoCount <= 0xff,
      LitchControlAction.coaching => owner != null && undoCount == 1,
    };
    if (!canonical) {
      throw ArgumentError('Invalid connection-session action fields.');
    }
    return Uint8List.fromList(<int>[
      connectionActionVersion,
      actionCode,
      ownerCode,
      coachingCode,
      undoCount,
    ]);
  }

  static Map<String, Object> buildActionPayload({
    required int requestId,
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) {
    if (!_isRequestId(requestId)) {
      throw RangeError.range(
        requestId,
        1,
        LitchRequestIdAllocator.maxRequestId,
        'requestId',
      );
    }

    final Map<String, Object> payload = <String, Object>{
      'a': action.wireToken,
      'i': requestId,
    };
    switch (action) {
      case LitchControlAction.hint:
      case LitchControlAction.newGame:
        if (owner != null || enabled != null || undoCount != 1) {
          throw ArgumentError('Unexpected fields for ${action.wireToken}.');
        }
        break;
      case LitchControlAction.undo:
        if (owner != null || enabled != null) {
          throw ArgumentError('Undo does not accept owner or enabled.');
        }
        if (undoCount < 1 || undoCount > 0xff) {
          throw RangeError.range(undoCount, 1, 0xff, 'undoCount');
        }
        payload['n'] = undoCount;
        break;
      case LitchControlAction.coaching:
        if (owner == null) {
          throw ArgumentError.notNull('owner');
        }
        if (undoCount != 1) {
          throw ArgumentError('Coaching does not accept undoCount.');
        }
        payload['o'] = owner.wireToken;
        if (enabled != null) {
          payload['e'] = enabled;
        }
        break;
    }
    return payload;
  }

  static LitchControlOutcome? decodeActionOutcome(
    Map<String, Object?> payload, {
    LitchControlAction? expectedAction,
  }) {
    if (payload.containsKey('x')) {
      if (expectedAction == null) {
        return null;
      }
      return _decodeCompactAction(payload['x'], expectedAction);
    }

    final int? requestId = _strictInt(payload['i']);
    final Object? rawAction = payload['a'];
    final Object? rawStatus = payload['s'];
    final Object? rawReason = payload['r'];
    final Object? rawActionState = payload['p'];
    final Object? rawOk = payload['ok'];
    if (!_isRequestId(requestId) ||
        rawAction is! String ||
        rawStatus is! String ||
        rawReason is! String ||
        rawOk is! bool) {
      return null;
    }

    final LitchControlAction? action = LitchControlAction.fromWireToken(
      rawAction,
    );
    final LitchMutationStatus? status = LitchMutationStatus.fromWireToken(
      rawStatus,
    );
    final LitchMutationReason? reason = LitchMutationReason.fromWireToken(
      rawReason,
    );
    final LitchControlActionState actionState = rawActionState is String
        ? LitchControlActionState.fromWireToken(rawActionState) ??
              LitchControlActionState.unknown
        : LitchControlActionState.unknown;
    if (action == null ||
        status == null ||
        reason == null ||
        (expectedAction != null && action != expectedAction) ||
        !_isConsistentOutcome(status: status, reason: reason, ok: rawOk)) {
      return null;
    }

    return LitchControlOutcome(
      requestId: requestId!,
      action: action,
      status: status,
      reason: reason,
      actionState: actionState,
    );
  }

  static LitchSettingOutcome? decodeSettingOutcome(
    Map<String, Object?> payload,
  ) {
    if (payload.containsKey('y')) {
      return _decodeCompactSetting(payload['y']);
    }

    final Object? rawKey = payload['k'];
    final Object? rawOk = payload['ok'];
    if (rawKey is! String || rawKey.isEmpty || rawOk is! bool) {
      return null;
    }

    int? requestId;
    if (payload.containsKey('i')) {
      requestId = _strictInt(payload['i']);
      if (!_isRequestId(requestId)) {
        return null;
      }
    }

    if (rawOk) {
      if (!payload.containsKey('v')) {
        return null;
      }
      return LitchSettingOutcome(
        requestId: requestId,
        key: rawKey,
        ok: true,
        reason: LitchMutationReason.none,
        hasValue: true,
        value: payload['v'],
        isCompact: false,
      );
    }

    final Object? rawError = payload['e'];
    if (rawError is! String) {
      return null;
    }
    final LitchMutationReason? reason = _settingReasonFromWireToken(rawError);
    if (reason == null || reason == LitchMutationReason.none) {
      return null;
    }
    return LitchSettingOutcome(
      requestId: requestId,
      key: rawKey,
      ok: false,
      reason: reason,
      hasValue: false,
      isCompact: false,
    );
  }

  static LitchControlOutcome? _decodeCompactAction(
    Object? rawTuple,
    LitchControlAction expectedAction,
  ) {
    if (rawTuple is! List<Object?> || rawTuple.length != 3) {
      return null;
    }
    final int? requestId = _strictInt(rawTuple[0]);
    final int? statusCode = _strictInt(rawTuple[1]);
    final int? reasonCode = _strictInt(rawTuple[2]);
    if (!_isRequestId(requestId) || statusCode == null || reasonCode == null) {
      return null;
    }
    final LitchMutationStatus? status = LitchMutationStatus.fromWireCode(
      statusCode,
    );
    final LitchMutationReason? reason = LitchMutationReason.fromWireCode(
      reasonCode,
    );
    if (status == null ||
        reason == null ||
        !_isConsistentOutcome(
          status: status,
          reason: reason,
          ok: status != LitchMutationStatus.rejected,
        )) {
      return null;
    }
    return LitchControlOutcome(
      requestId: requestId!,
      action: expectedAction,
      status: status,
      reason: reason,
      actionState: LitchControlActionState.unknown,
    );
  }

  static LitchSettingOutcome? _decodeCompactSetting(Object? rawTuple) {
    if (rawTuple is! List<Object?> || rawTuple.length != 3) {
      return null;
    }
    final int? requestId = _strictInt(rawTuple[0]);
    final int? okCode = _strictInt(rawTuple[1]);
    final int? reasonCode = _strictInt(rawTuple[2]);
    if (!_isRequestId(requestId) ||
        (okCode != 0 && okCode != 1) ||
        reasonCode == null) {
      return null;
    }
    final LitchMutationReason? reason = LitchMutationReason.fromWireCode(
      reasonCode,
    );
    final bool ok = okCode == 1;
    if (reason == null ||
        (ok && reason != LitchMutationReason.none) ||
        (!ok && reason == LitchMutationReason.none)) {
      return null;
    }
    return LitchSettingOutcome(
      requestId: requestId!,
      ok: ok,
      reason: reason,
      hasValue: false,
      isCompact: true,
    );
  }

  static bool _isConsistentOutcome({
    required LitchMutationStatus status,
    required LitchMutationReason reason,
    required bool ok,
  }) {
    final bool rejected = status == LitchMutationStatus.rejected;
    if (ok == rejected) {
      return false;
    }
    if (rejected) {
      return reason != LitchMutationReason.none;
    }
    return reason == LitchMutationReason.none;
  }

  static bool _isRequestId(int? value) =>
      value != null && value >= 1 && value <= 0xffff;

  static LitchMutationReason? _settingReasonFromWireToken(String token) {
    return LitchMutationReason.fromWireToken(token) ??
        switch (token) {
          'value_too_large' ||
          'response_too_large' => LitchMutationReason.badRequest,
          _ => null,
        };
  }

  static int? _strictInt(Object? value) => value is int ? value : null;
}
