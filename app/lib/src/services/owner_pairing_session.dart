import 'dart:async';

import '../models/owner_pairing_session.dart';
import 'connection_session.dart';
import 'connection_session_codec.dart';

/// Product access was refused by the current board ownership boundary.
final class OwnerPairingNotReady implements Exception {
  const OwnerPairingNotReady();
  @override
  String toString() => 'Confirm this board before using it.';
}

/// State40 arrived, but the board has not proven canonical identity storage.
/// This is a durable-readiness failure, not a missing transport response.
final class OwnerIdentityNotDurable implements Exception {
  const OwnerIdentityNotDurable();
  @override
  String toString() => 'Board identity has not been durably restored.';
}

/// Typed pairing adapter. Delivery, repair and lifetime belong to the shared
/// session; transport acknowledgement never establishes ownership.
final class OwnerPairingSession {
  OwnerPairingSession(
    this.session, {
    Duration readinessTimeout = defaultReadinessTimeout,
  }) : _readinessTimeout = readinessTimeout {
    _subscription = session.states.listen(
      _accept,
      onDone: () {
        if (!_updates.isClosed) unawaited(_updates.close());
      },
    );
  }

  final ConnectionSession session;
  static const defaultReadinessTimeout = Duration(seconds: 20);
  final Duration _readinessTimeout;
  late final StreamSubscription<ConnectionSessionPacket> _subscription;
  final _updates = StreamController<OwnerPairingSessionStatus>.broadcast(
    sync: true,
  );
  OwnerPairingSessionStatus? _status;
  int _statusRevision = 0;
  final Map<int, ({int revision, List<int> bytes})> _identityHalves = {};
  bool _identityConflict = false;
  int _nextAttempt = 1;
  int _claimStatusFloor = 0;
  String? _provenIdentity;
  bool _disposed = false;
  int? _phoneBondAttempt;
  bool _phoneBondTaken = false;

  bool get phoneBondRequested => _phoneBondAttempt != null;
  void retireNativeBondAttempt() => _phoneBondAttempt = null;
  int? takeNativeBondAttempt() {
    final current = _status;
    if (_disposed ||
        _phoneBondTaken ||
        _phoneBondAttempt == null ||
        !session.isActivated ||
        durableIdentity == null ||
        current?.nativeBondAllowed != true ||
        current!.attempt != _phoneBondAttempt ||
        _statusRevision <= _claimStatusFloor) {
      return null;
    }
    _phoneBondTaken = true;
    return _phoneBondAttempt;
  }

  OwnerPairingSessionStatus? get status => _status;
  Stream<OwnerPairingSessionStatus> get statuses => _updates.stream;
  String? get durableIdentity {
    if (_identityConflict ||
        !session.isActivated ||
        _status?.identityDurable != true) {
      return null;
    }
    return _provenIdentity;
  }

  String? get _coherentIdentity {
    if (_status?.identityDurable != true || _identityHalves.length != 2) {
      return null;
    }
    final low = _identityHalves[41]!;
    final high = _identityHalves[42]!;
    if (low.revision != _statusRevision || high.revision != _statusRevision) {
      return null;
    }
    final bytes = [...low.bytes, ...high.bytes];
    if (!bytes.any((byte) => byte != 0)) return null;
    return bytes.map((byte) => byte.toRadixString(16).padLeft(2, '0')).join();
  }

  bool get ready =>
      session.isActivated &&
      _status?.connectionOwnerReady == true &&
      durableIdentity != null;

  void _accept(ConnectionSessionPacket packet) {
    if (_disposed || _status?.foreignOwnerRefused == true) return;
    if (packet.code == OwnerPairingSessionStatus.stateCode) {
      final parsed = OwnerPairingSessionStatus.tryParse(packet.body);
      if (parsed == null || packet.sequence <= _statusRevision) return;
      _statusRevision = packet.sequence;
      _status = parsed;
    } else if (packet.code == 41 || packet.code == 42) {
      if (packet.body.length != 8 || packet.sequence < _statusRevision) return;
      final prior = _identityHalves[packet.code];
      if (prior != null && packet.sequence < prior.revision) return;
      _identityHalves[packet.code] = (
        revision: packet.sequence,
        bytes: List<int>.of(packet.body),
      );
    } else {
      return;
    }
    // State replay can interleave revisions. Pin and compare complete snapshots,
    // never a newly durable half combined with an older unavailable zero half.
    // An immutable identity already proven in this session survives assembly of
    // newer status revisions. Status flags still revoke product access at once.
    final identity = _coherentIdentity;
    if (identity != null) {
      if (_provenIdentity != null && identity != _provenIdentity) {
        _identityConflict = true;
      } else {
        _provenIdentity = identity;
      }
    }
    if (_status case final value?) _updates.add(value);
  }

  Future<OwnerPairingSessionStatus> readStatus() async {
    await session.readStateSnapshot<OwnerPairingSessionStatus>(
      code: 40,
      accept: (packet) {
        _accept(packet);
        if (packet.code != 40 ||
            OwnerPairingSessionStatus.tryParse(packet.body) == null ||
            packet.sequence < _statusRevision ||
            _status == null) {
          return null;
        }
        return _status;
      },
    );
    await _repairIdentity();
    return _status!;
  }

  /// A new board can connect before its first durable identity promotion.
  /// The common State reader owns this bounded wait and any Query repair.
  Future<OwnerPairingSessionStatus> waitForInitialIdentity() async {
    try {
      await session.readStateSnapshot<OwnerPairingSessionStatus>(
        code: 40,
        timeout: _readinessTimeout,
        mode: ConnectionSessionStateReadMode.observeFirst,
        accept: (packet) {
          _accept(packet);
          final current = _status;
          if (current == null || _awaitingInitialIdentity(current)) return null;
          return current;
        },
      );
    } on TimeoutException {
      if (!_disposed && session.isActivated && awaitingInitialIdentity) {
        throw const OwnerIdentityNotDurable();
      }
      rethrow;
    }
    await _repairIdentity();
    return _status!;
  }

  static bool _awaitingInitialIdentity(OwnerPairingSessionStatus status) =>
      status.state == OwnerPairingSessionState.unavailable &&
      status.reason == OwnerPairingSessionReason.none &&
      !status.identityDurable;

  bool get awaitingInitialIdentity =>
      _status != null && _awaitingInitialIdentity(_status!);

  Future<void> _repairIdentity() async {
    if (_status!.foreignOwnerRefused) return;
    if (_status!.identityDurable && durableIdentity == null) {
      await session.readStateSnapshot<bool>(
        code: 40,
        accept: (packet) {
          _accept(packet);
          return !_status!.identityDurable ||
                  durableIdentity != null ||
                  _identityConflict
              ? true
              : null;
        },
      );
    }
  }

  /// Begin only admits the board's physical prompt. Final State is authority.
  Future<ConnectionSessionOutcome> begin({bool phoneInitiated = false}) {
    _claimStatusFloor = _statusRevision;
    final attempt = _nextAttempt++;
    if (_nextAttempt > 0xffff) _nextAttempt = 1;
    _phoneBondAttempt = phoneInitiated ? attempt : null;
    _phoneBondTaken = false;
    return session.sendOrdered(
      OwnerPairingSessionStatus.orderedOpcode,
      OwnerPairingSessionStatus.command(
        cancel: false,
        attempt: attempt,
        phoneInitiated: phoneInitiated,
      ),
    );
  }

  Future<ConnectionSessionOutcome> cancel() {
    final current = _status;
    if (current == null ||
        current.state != OwnerPairingSessionState.awaitPhysical) {
      throw StateError('Physical confirmation has already ended.');
    }
    retireNativeBondAttempt();
    return session.sendOrdered(
      OwnerPairingSessionStatus.orderedOpcode,
      OwnerPairingSessionStatus.command(
        cancel: true,
        attempt: current.attempt,
        physicalEnd: current.physicalEnd,
      ),
    );
  }

  Future<void> waitUntilReady() async {
    if (_status?.foreignOwnerRefused == true) {
      throw StateError('Board belongs to another device.');
    }
    if (ready) return;
    try {
      await session.readStateSnapshot<bool>(
        code: 40,
        timeout: _readinessTimeout,
        mode: ConnectionSessionStateReadMode.observeFirst,
        accept: (packet) {
          _accept(packet);
          if (ready) return true;
          final current = _status;
          if (current != null &&
              _statusRevision > _claimStatusFloor &&
              !current.claimPending &&
              current.state != OwnerPairingSessionState.clearPrompt &&
              current.state != OwnerPairingSessionState.ownerReady &&
              !(current.state == OwnerPairingSessionState.ownedOther &&
                  current.reason == OwnerPairingSessionReason.none)) {
            throw StateError('Board pairing ended: ${current.reason.name}.');
          }
          if (_identityConflict) {
            throw StateError('Board identity changed during pairing.');
          }
          return null;
        },
      );
    } on TimeoutException {
      if (_status != null) {
        throw TimeoutException(
          'Board owner verification did not finish',
          _readinessTimeout,
        );
      }
      rethrow;
    }
  }

  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _subscription.cancel();
    await _updates.close();
  }
}
