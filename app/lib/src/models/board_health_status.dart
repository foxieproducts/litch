import 'dart:convert';
import 'dart:typed_data';
import 'home_board_snapshot.dart';
import 'runtime_health_snapshot.dart';

/// Presentation only: failed reads and past restart history are not proof of
/// a current fault. Firmware remains the sensor/power and recovery authority.
bool boardHealthNeedsAttention(
  RuntimeHealthResult? health, {
  HomePersistenceDegradation? persistence,
  bool sessionOnly = false,
}) =>
    persistence == HomePersistenceDegradation.storageIo ||
    switch (health) {
      RuntimeHealthAvailable(:final snapshot) => switch (snapshot.state) {
        RuntimeHealthState.degradedUsable ||
        RuntimeHealthState.inputPaused ||
        RuntimeHealthState.protectionLatched => true,
        RuntimeHealthState.healthy || RuntimeHealthState.unknown => false,
      },
      _ => false,
    };

class ConnectionRuntimeHealthState {
  const ConnectionRuntimeHealthState(this.snapshot);
  final RuntimeHealthSnapshot snapshot;

  static ConnectionRuntimeHealthState? tryParse(Uint8List body) {
    if (body.length != 6 || body[0] != 1 || body[3] > 3) return null;
    final data = ByteData.sublistView(body);
    final stateCode = body[3];
    return ConnectionRuntimeHealthState(
      RuntimeHealthSnapshot(
        revision: data.getUint16(1, Endian.little),
        stateCode: stateCode,
        state: switch (stateCode) {
          0 => RuntimeHealthState.healthy,
          1 => RuntimeHealthState.degradedUsable,
          2 => RuntimeHealthState.inputPaused,
          3 => RuntimeHealthState.protectionLatched,
          _ => RuntimeHealthState.unknown,
        },
        causeMask: data.getUint16(4, Endian.little),
      ),
    );
  }
}

class ConnectionBoardHealthContext {
  const ConnectionBoardHealthContext({
    required this.persistence,
    required this.sessionOnly,
    required this.resetReason,
    required this.bootCount,
    required this.watchdogRestarts,
  });
  final HomePersistenceDegradation? persistence;
  final bool sessionOnly;
  final int resetReason;
  final int bootCount;
  final int watchdogRestarts;

  static ConnectionBoardHealthContext? tryParse(Uint8List body) {
    if (body.length != 10 ||
        body[0] & 0xc0 != 0x40 ||
        body[0] & 0x38 != 0 ||
        body[0] & 0x03 > 2) {
      return null;
    }
    final data = ByteData.sublistView(body);
    return ConnectionBoardHealthContext(
      persistence: switch (body[0] & 0x03) {
        1 => HomePersistenceDegradation.quarantinedRecord,
        2 => HomePersistenceDegradation.storageIo,
        _ => null,
      },
      sessionOnly: body[0] & 0x04 != 0,
      resetReason: body[1],
      bootCount: data.getUint32(2, Endian.little),
      watchdogRestarts: data.getUint32(6, Endian.little),
    );
  }
}

class BoardHealthStatus {
  const BoardHealthStatus({
    required this.runtimeHealth,
    this.persistence,
    required this.resetReason,
    required this.bootCount,
    required this.watchdogRestarts,
    this.sessionOnly = false,
  });
  final RuntimeHealthSnapshot runtimeHealth;
  final HomePersistenceDegradation? persistence;
  final bool sessionOnly;
  final int resetReason;
  final int bootCount;
  final int watchdogRestarts;
  bool get watchdogRestart => resetReason >= 5 && resetReason <= 7;

  static BoardHealthStatus fromConnectionState(
    ConnectionRuntimeHealthState runtime,
    ConnectionBoardHealthContext context,
  ) {
    return BoardHealthStatus(
      runtimeHealth: runtime.snapshot,
      persistence: context.persistence,
      sessionOnly: context.sessionOnly,
      resetReason: context.resetReason,
      bootCount: context.bootCount,
      watchdogRestarts: context.watchdogRestarts,
    );
  }

  static BoardHealthStatus? tryParse(String line) {
    if (line.length > 191) return null;
    try {
      final dynamic data = jsonDecode(line);
      if (data is! Map<String, dynamic> ||
          data['event'] != 'board_health' ||
          data['v'] != 1) {
        return null;
      }
      final dynamic token = data['h'];
      final RuntimeHealthSnapshot? health = token is String
          ? RuntimeHealthSnapshot.fromToken(token)
          : null;
      if (health == null ||
          !const <String>['n', 'q', 'i'].contains(data['ps'])) {
        return null;
      }
      for (final String key in const <String>['rr', 'boot', 'wd']) {
        if (data[key] is! int || data[key] < 0 || data[key] > 0xffffffff) {
          return null;
        }
      }
      if (data.containsKey('sd') && data['sd'] != 0 && data['sd'] != 1) {
        return null;
      }
      return BoardHealthStatus(
        runtimeHealth: health,
        sessionOnly: data['sd'] == 1,
        persistence: data['ps'] == 'q'
            ? HomePersistenceDegradation.quarantinedRecord
            : data['ps'] == 'i'
            ? HomePersistenceDegradation.storageIo
            : null,
        resetReason: data['rr'] as int,
        bootCount: data['boot'] as int,
        watchdogRestarts: data['wd'] as int,
      );
    } catch (_) {
      return null;
    }
  }
}

String persistenceLossMessage(
  HomePersistenceDegradation persistence, {
  bool sessionOnly = false,
}) => sessionOnly && persistence == HomePersistenceDegradation.storageIo
    ? 'Saving is unavailable for this session. This session’s settings and game progress will be lost when the board turns off.'
    : switch (persistence) {
        HomePersistenceDegradation.quarantinedRecord =>
          'A saved game could not be restored. The board started a new game and is repairing its saved copy.',
        HomePersistenceDegradation.storageIo =>
          'The board could not confirm a saved copy. Recent changes may be lost when the board turns off.',
      };

class SupportIncident {
  SupportIncident._(this.fields);
  final Map<String, Object> fields;
  // Only structured board-owned numeric state and occupancy masks. Never copy
  // arbitrary console lines, names, credentials, FEN or move history.
  static const List<String> scalarKeys = <String>[
    'rr',
    'boot',
    'wd',
    'up',
    'prior',
    'prior_up',
    'phase',
    'variant',
    'hc',
    'se',
    'be',
    'flags',
    'saved',
    'failed',
    'prior_phase',
    'prior_variant',
    'prior_hc',
    'prior_se',
    'prior_flags',
  ];
  static const List<String> masks = <String>[
    'expected',
    'observed',
    'prior_expected',
    'prior_observed',
  ];
  static SupportIncident? tryParse(String line) {
    if (line.length > 1023) return null;
    try {
      final dynamic data = jsonDecode(line);
      if (data is! Map<String, dynamic> ||
          data['event'] != 'support_incident' ||
          data['v'] != 1) {
        return null;
      }
      final Map<String, Object> fields = <String, Object>{};
      for (final String key in scalarKeys) {
        final dynamic value = data[key];
        if (value is! int || value < 0 || value > 0xffffffff) return null;
        fields[key] = value;
      }
      for (final String key in masks) {
        final dynamic value = data[key];
        if (value is! String || !RegExp(r'^[0-9A-Fa-f]{16}$').hasMatch(value)) {
          return null;
        }
        fields[key] = value;
      }
      for (final String key in const <String>[
        'stages',
        'stage_ms',
        'now_stages',
        'now_ms',
        'recent',
      ]) {
        final dynamic value = data[key];
        final int length = key == 'recent' ? 8 : 4;
        if (value is! List ||
            value.length != length ||
            value.any(
              (dynamic item) =>
                  item is! int ||
                  item < 0 ||
                  item > (key.endsWith('ms') ? 0xffffffff : 14),
            )) {
          return null;
        }
        fields[key] = List<int>.from(value);
      }
      return SupportIncident._(Map<String, Object>.unmodifiable(fields));
    } catch (_) {
      return null;
    }
  }

  String get diagnosticText => jsonEncode(fields);
}
