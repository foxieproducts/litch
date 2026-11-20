import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

import 'connection_session_codec.dart';

typedef ConnectionSessionSend = Future<void> Function(Uint8List packet);
typedef ConnectionSessionBatchSend =
    Future<void> Function(List<Uint8List> packets);
typedef ConnectionSessionObject = ({int opcode, Uint8List body});

enum ConnectionSessionDisposition { applied, rejected, cancelled }

enum ConnectionSessionObjectPriority { interactive, bulk }

/// Submission releases the producer after native issue; the session still owns
/// every peer admission and repair obligation. It is never object verification.
enum ConnectionSessionObjectCompletion { admitted, submitted }

enum ConnectionSessionStateReadMode { query, observeFirst }

/// An intent retired only the unissued suffix; the shared session stays valid.
final class ConnectionSessionObjectCancelled implements Exception {
  const ConnectionSessionObjectCancelled();
}

final class _ObjectSubmission {
  _ObjectSubmission({
    required this.objects,
    required this.priority,
    required this.atomicMessage,
    required this.messageEnds,
    this.observation,
    this.onAdmissionProgress,
  });

  final List<ConnectionSessionObject> objects;
  final ConnectionSessionObjectPriority priority;
  final bool atomicMessage;
  final List<int> messageEnds;
  final _ObjectSubmissionObservation? observation;
  void Function(int admittedObjects, int totalObjects)? onAdmissionProgress;
  int admittedPackets = 0;
  final Completer<void> result = Completer<void>();
  final Completer<void> submitted = Completer<void>();
  int nextIndex = 0;
  int pendingPackets = 0;
  bool allIssued = false;
  int? cancelledEnd;
  int get issueEnd => cancelledEnd ?? objects.length;

  void cancelUnissued() {
    onAdmissionProgress = null;
    if (allIssued || cancelledEnd != null) return;
    // An issued logical prefix must reach its End before another producer can
    // reuse the receiver's reassembly owner. Unstarted messages may be dropped.
    final end = nextIndex == 0
        ? 0
        : messageEnds.firstWhere((end) => end >= nextIndex);
    if (end < objects.length) cancelledEnd = end;
  }

  void track(Future<void> packet) {
    pendingPackets += 1;
    packet.then(
      (_) {
        pendingPackets -= 1;
        admittedPackets += 1;
        try {
          onAdmissionProgress?.call(admittedPackets, objects.length);
        } catch (_) {
          // Presentation cannot change admission or transport completion.
        }
        if (pendingPackets == 0 && nextIndex == objects.length) {
          observation?.accepted();
        }
        _tryComplete();
      },
      onError: (Object error, StackTrace stack) {
        fail(error, stack);
      },
    );
  }

  void finishIssuing() {
    allIssued = true;
    if (!submitted.isCompleted) {
      if (cancelledEnd == null) {
        submitted.complete();
      } else {
        submitted.completeError(const ConnectionSessionObjectCancelled());
      }
    }
    _tryComplete();
  }

  void fail(Object error, [StackTrace? stack]) {
    onAdmissionProgress = null;
    if (!submitted.isCompleted) submitted.completeError(error, stack);
    if (!result.isCompleted) {
      observation?.complete('failed');
      result.completeError(error, stack);
    }
  }

  void _tryComplete() {
    if (allIssued && pendingPackets == 0 && !result.isCompleted) {
      observation?.complete(cancelledEnd == null ? 'accepted' : 'cancelled');
      if (cancelledEnd == null) {
        result.complete();
      } else {
        result.completeError(const ConnectionSessionObjectCancelled());
      }
    }
  }
}

// One optional bounded observation per producer submission, never per packet.
// This clock has no role in grants, deadlines, retries or transport scheduling.
final class _ObjectSubmissionObservation {
  _ObjectSubmissionObservation(this.clock, this.report, this.life, this.count)
    : queued = clock();
  final int Function() clock;
  final void Function(Map<String, Object?>) report;
  final int life, count, queued;
  int? first, last, firstSend, lastReturn, acceptedAt;
  int native = 0, grantWait = 0;
  bool reported = false;

  void accepted() => acceptedAt = clock();

  void complete(String outcome) {
    if (reported) return;
    reported = true;
    try {
      report({
        'life': life,
        'first': first,
        'last': last,
        'count': count,
        'queued_us': queued,
        'first_send_us': firstSend,
        'last_return_us': lastReturn,
        'accepted_us': acceptedAt,
        'done_us': clock(),
        'grant_wait_us': grantWait,
        'native_us': native,
        'outcome': outcome,
      });
    } catch (_) {
      // Diagnostics cannot change the submission's result or recovery owner.
    }
  }
}

final class ConnectionSessionOutcome {
  const ConnectionSessionOutcome({
    required this.sequence,
    required this.disposition,
    required this.reason,
    required this.domainRevision,
    required this.domainDigest,
  });

  final int sequence;
  final ConnectionSessionDisposition disposition;
  final int reason;
  final int domainRevision;
  final int domainDigest;
}

final class ConnectionSession {
  ConnectionSession({
    required Stream<List<int>> packets,
    required ConnectionSessionSend send,
    ConnectionSessionBatchSend? sendBatch,
    required this.clientNonce,
    this.packetMaximum = ConnectionSessionCodec.minimumPacketBytes,
    this.orderedTopic = 1,
    this.orderedCapacity = 8,
    this.recoveryProbeDelay = const Duration(milliseconds: 121),
    this.recoveryFailureTimeout = const Duration(seconds: 2),
    this.receivedHeadCompletionTimeout = const Duration(seconds: 20),
    this.onRetired,
    this.onStateReadCompleted,
    bool Function(int opcode)? canSubmitObject,
    int Function()? observationMicroseconds,
  }) : _canSubmitObject = canSubmitObject,
       _observationMicroseconds = observationMicroseconds,
       _send = send,
       _sendBatch = sendBatch {
    if (clientNonce <= 0 || clientNonce > 0x7fffffffffffffff) {
      throw ArgumentError.value(clientNonce, 'clientNonce');
    }
    if (packetMaximum < ConnectionSessionCodec.minimumPacketBytes ||
        packetMaximum > ConnectionSessionCodec.maximumPacketBytes ||
        orderedTopic <= 0 ||
        orderedTopic > 0xffff ||
        orderedCapacity <= 0 ||
        orderedCapacity > ConnectionSessionCodec.maximumOrderedCapacity ||
        recoveryProbeDelay <= Duration.zero ||
        recoveryFailureTimeout <= recoveryProbeDelay ||
        receivedHeadCompletionTimeout < recoveryFailureTimeout) {
      throw ArgumentError('Invalid connection-session bounds');
    }
    // Retirement may precede later handshake awaits or a native write return.
    // Observe every startup reply now; its original future still fails callers.
    for (final reply in [
      _welcome,
      _identity,
      _activated,
      _opened,
      _objectOpened,
      _stateOpened,
      _reverseObjectOpened,
    ]) {
      reply.future.ignore();
    }
    _subscription = packets.listen(
      _onBytes,
      onError: _onError,
      onDone: _onDone,
    );
  }

  static const orderedFeature = 1;
  static const stateFeature = 2;
  static const objectFeature = 8;
  static const reverseObjectFeature = 16;
  static const litchcraftOrderedTopic = 1;
  static const litchcraftStateTopic = 1;
  static const litchcraftReverseObjectTopic = 2;
  static const reverseObjectCapacity = 8;
  static const litchcraftStateCode = 1;
  static const runtimeHealthStateCode = 2;
  static const boardHealthContextStateCode = 3;
  static const litchcraftStoreEditStateCode = 22;
  static const litchcraftStoreAuthorityStateCode = 37;
  static const stateCapacity = 37;
  static const stateMaximumBody = 10;
  static const litchcraftTimelineOpcode = 2;
  static const litchcraftModeOpcode = 1;
  static const litchcraftPaintOpcode = 3;
  static const litchcraftEffectOpcode = 4;
  static const litchcraftToolOpcode = 5;
  static const litchcraftUndoOpcode = 6;
  static const litchcraftRedoOpcode = 7;
  static const litchcraftStoreGuardOpcode = 8;
  static const litchcraftStoreCanvasOpcode = 9;
  static const litchcraftStoreFrameEditOpcode = 10;
  static const litchcraftStoreNewCanvasOpcode = 11;
  static const storeGuardEpochFeature = 0x0020;
  static const orderedModeFeature = 0x0040;
  static const otaStatusReceiptCode = 0x32;
  static const expectedFeedbackMs = 40;
  static const objectSubmissionQueueCapacity = 16;
  static const maximumConsecutiveInteractiveObjectBursts = 3;

  final int clientNonce;
  final int packetMaximum;
  final int orderedTopic;
  final int orderedCapacity;
  final Duration recoveryProbeDelay;
  final Duration recoveryFailureTimeout;
  final Duration receivedHeadCompletionTimeout;
  final void Function(Map<String, Object?> fields)? onRetired;
  final void Function()? onStateReadCompleted;
  final bool Function(int opcode)? _canSubmitObject;
  final int Function()? _observationMicroseconds;
  final Stopwatch _observationClock = Stopwatch()..start();
  int _observeNow() =>
      _observationMicroseconds?.call() ?? _observationClock.elapsedMicroseconds;
  final ConnectionSessionSend _send;
  final ConnectionSessionBatchSend? _sendBatch;
  late final StreamSubscription<List<int>> _subscription;
  final Completer<ConnectionSessionPacket> _welcome = Completer();
  final Completer<ConnectionSessionPacket> _identity = Completer();
  final Completer<ConnectionSessionPacket> _activated = Completer();
  final Completer<ConnectionSessionPacket> _opened = Completer();
  final Completer<ConnectionSessionPacket> _objectOpened = Completer();
  final Completer<void> _stateOpened = Completer();
  final Completer<void> _reverseObjectOpened = Completer();
  final StreamController<ConnectionSessionPacket> _states =
      StreamController<ConnectionSessionPacket>.broadcast(sync: true);
  final StreamController<ConnectionSessionPacket> _objectReceipts =
      StreamController<ConnectionSessionPacket>.broadcast(sync: true);
  Uint8List? _helloBytes;
  Uint8List? _welcomeBytes;
  Uint8List? _identityBytes;
  int _connectionToken = 0;
  int _boardBootNonce = 0;
  int get boardBootNonce => _boardBootNonce;
  bool _storeGuardEpochSupported = false;
  bool get storeGuardEpochSupported => _storeGuardEpochSupported;
  bool _orderedModeSupported = false;
  bool get orderedModeSupported => _orderedModeSupported;
  int _streamToken = 0;
  int _orderedSelectedCapacity = 0;
  int _orderedMaximumBody = 0;
  int _stateSelectedMaximumBody = 0;
  int _stateStreamToken = 0;
  int _objectStreamToken = 0;
  int _reverseObjectStreamToken = 0;
  int _reverseObjectMaximumBody = 0;
  int _reverseObjectConfirmedThrough = 0;
  int _reverseObjectGrantThrough = 0;
  int _reverseObjectSelectedCapacity = 0;
  int _objectMaximumBody = 0;
  int _nextObjectSequence = 1;
  int _objectAcceptedThrough = 0;
  int _objectGrantThrough = 0;
  int _objectSelectedCapacity = 0;
  bool _objectQueryOutstanding = false;
  int _nextCentralToken = 0x80000001;
  Uint8List? _stateOpenRequest;
  Uint8List? _stateOpenedResponse;
  Uint8List? _reverseObjectOpenRequest;
  Uint8List? _reverseObjectOpenedResponse;
  int _selectedPacketMaximum = 0;
  int _grantThrough = 0;
  int _nextSequence = 1;
  int _confirmedThrough = 0;
  int _receiverConfirmedThrough = 0;
  int _progressTerminal = 0;
  int _progressReceivedAhead = 0;
  int _checkpointThrough = 0;
  int _checkpointRevision = 0;
  int _checkpointDigest = 0;
  int _checkpointExceptions = 0;
  bool _confirmInFlight = false;
  bool _confirmReplayInFlight = false;
  bool _queryOutstanding = false;
  bool _recoveryWriteInFlight = false;
  bool _objectRecoveryWriteInFlight = false;
  Future<void> _issueTail = Future<void>.value();
  Future<void> _recoveryTail = Future<void>.value();
  Future<void> _objectRecoveryTail = Future<void>.value();
  bool _reverseObjectProgressSending = false;
  bool _reverseObjectProgressDirty = false;
  Completer<void>? _grantWaiter;
  Completer<void>? _objectGrantWaiter;
  int _activeStateReads = 0;
  Timer? _recoveryTimer;
  Timer? _objectRecoveryTimer;
  Stopwatch? _recoveryDeadline;
  int _receivedHeadSequence = 0;
  Stopwatch? _receivedHeadDeadline;
  Stopwatch? _objectRecoveryDeadline;
  int _recoveryBackoff = 1;
  int _objectRecoveryBackoff = 1;
  final Map<int, Completer<ConnectionSessionOutcome>> _pending = {};
  final Map<int, Uint8List> _issued = {};
  final Set<int> _retransmissionQueued = {};
  final Map<int, Completer<void>> _objectPending = {};
  final Map<int, Uint8List> _objectIssued = {};
  final Queue<_ObjectSubmission> _interactiveObjectSubmissions = Queue();
  final Queue<_ObjectSubmission> _bulkObjectSubmissions = Queue();
  _ObjectSubmission? _activeObjectSubmission;
  bool _objectPumpRunning = false;
  int _consecutiveInteractiveObjectBursts = 0;
  final Map<int, Uint8List> _reverseObjectReceived = {};
  final Map<int, (ConnectionSessionDisposition, int)> _exceptionOutcomes = {};
  final Map<int, ConnectionSessionOutcome> _complete = {};
  bool _disposed = false;
  final Completer<void> _ended = Completer<void>();
  Object _retirementError = StateError('Connection session ended');

  int get connectionToken => _connectionToken;
  int get streamToken => _streamToken;
  int get stateStreamToken => _stateStreamToken;
  int get objectStreamToken => _objectStreamToken;
  int get reverseObjectStreamToken => _reverseObjectStreamToken;
  int get reverseObjectConfirmedThrough => _reverseObjectConfirmedThrough;
  int get reverseObjectGrantThrough => _reverseObjectGrantThrough;
  int get reverseObjectSelectedCapacity => _reverseObjectSelectedCapacity;
  int get objectMaximumBody => _objectMaximumBody;
  int get objectSelectedCapacity => _objectSelectedCapacity;
  int get orderedMaximumBody => _orderedMaximumBody;
  int get orderedSelectedCapacity => _orderedSelectedCapacity;
  Stream<ConnectionSessionPacket> get states => _states.stream;
  Stream<ConnectionSessionPacket> get objectReceipts => _objectReceipts.stream;
  int get selectedPacketMaximum => _selectedPacketMaximum;
  int get grantThrough => _grantThrough;
  bool get isActivated =>
      !_disposed &&
      _connectionToken != 0 &&
      _streamToken != 0 &&
      _stateStreamToken != 0 &&
      _objectStreamToken != 0 &&
      _reverseObjectStreamToken != 0;

  Future<void> activate({
    Duration timeout = const Duration(seconds: 10),
  }) async {
    if (_disposed || _connectionToken != 0) {
      throw StateError('Connection session is not available for activation');
    }
    final helloBody = Uint8List(10);
    final helloData = ByteData.sublistView(helloBody);
    helloData.setUint64(0, clientNonce, Endian.little);
    helloData.setUint16(8, packetMaximum, Endian.little);
    _helloBytes = ConnectionSessionCodec.encode(
      ConnectionSessionPacket(
        kind: ConnectionSessionKind.control,
        code: ConnectionSessionControl.hello,
        token: 0,
        sequence: 0,
        body: helloBody,
      ),
    );
    // Welcome and Identity are one frozen transcript. Either may be lost;
    // keep replaying this same Hello until both have arrived under one deadline.
    final transcriptReplies = await _sendUntilReply(
      request: _helloBytes!,
      reply: Future.wait([_welcome.future, _identity.future]),
      timeout: timeout,
      description: 'Hello transcript',
    );
    final welcome = transcriptReplies[0];
    final identity = transcriptReplies[1];
    if (welcome.token == 0 ||
        identity.token != welcome.token ||
        _u64(welcome.body) != clientNonce ||
        _u64(identity.body) == 0 ||
        (_u16(identity.body, 8) &
                (orderedFeature |
                    stateFeature |
                    objectFeature |
                    reverseObjectFeature)) !=
            (orderedFeature |
                stateFeature |
                objectFeature |
                reverseObjectFeature)) {
      throw const FormatException('Invalid connection-session transcript');
    }
    final selectedMaximum = _u16(welcome.body, 8);
    if (selectedMaximum < ConnectionSessionCodec.minimumPacketBytes ||
        selectedMaximum > packetMaximum) {
      throw const FormatException('Invalid negotiated packet maximum');
    }
    _connectionToken = welcome.token;
    _boardBootNonce = _u64(identity.body);
    _storeGuardEpochSupported =
        (_u16(identity.body, 8) & storeGuardEpochFeature) != 0;
    _orderedModeSupported = (_u16(identity.body, 8) & orderedModeFeature) != 0;
    _selectedPacketMaximum = selectedMaximum;
    final transcript = Uint8List.fromList([
      ..._helloBytes!,
      ..._welcomeBytes!,
      ..._identityBytes!,
    ]);
    final crc = _crc32(transcript);
    final readyBody = Uint8List(4);
    ByteData.sublistView(readyBody).setUint32(0, crc, Endian.little);
    final readyRequest = _encodeControl(
      ConnectionSessionControl.ready,
      body: readyBody,
    );
    final activated = await _sendUntilReply(
      request: readyRequest,
      reply: _activated.future,
      timeout: timeout,
      description: 'Ready',
    );
    if (_u32(activated.body) != crc) {
      throw const FormatException('Connection-session activation mismatch');
    }

    final openBody = Uint8List(8);
    final openData = ByteData.sublistView(openBody);
    openData.setUint16(0, orderedTopic, Endian.little);
    openBody[2] = 0;
    openBody[3] = orderedCapacity;
    openData.setUint16(4, 10, Endian.little);
    openData.setUint16(6, expectedFeedbackMs, Endian.little);
    final orderedOpenRequest = _encodeControl(
      ConnectionSessionControl.open,
      sequence: 1,
      body: openBody,
    );
    final opened = await _sendUntilReply(
      request: orderedOpenRequest,
      reply: _opened.future,
      timeout: timeout,
      description: 'Ordered Open',
    );
    final status = _u16(opened.body, 8);
    final streamToken = _u32(opened.body);
    if (status != 0 ||
        streamToken == 0 ||
        streamToken == _connectionToken ||
        opened.body[7] != 0 ||
        opened.body[6] == 0 ||
        opened.body[6] > orderedCapacity ||
        _u16(opened.body, 4) == 0 ||
        _u16(opened.body, 4) > 10) {
      throw StateError('Connection-session ordered topic was refused');
    }
    _streamToken = streamToken;
    _orderedMaximumBody = _u16(opened.body, 4);
    _orderedSelectedCapacity = opened.body[6];
    _grantThrough = _orderedSelectedCapacity;

    openData.setUint16(0, litchcraftOrderedTopic, Endian.little);
    openBody[2] = 1;
    openBody[3] = ConnectionSessionCodec.maximumWindow;
    openData.setUint16(
      4,
      _selectedPacketMaximum - ConnectionSessionCodec.headerBytes,
      Endian.little,
    );
    openData.setUint16(6, expectedFeedbackMs, Endian.little);
    final objectOpenRequest = _encodeControl(
      ConnectionSessionControl.open,
      sequence: 2,
      body: openBody,
    );
    final objectOpened = await _sendUntilReply(
      request: objectOpenRequest,
      reply: _objectOpened.future,
      timeout: timeout,
      description: 'Object Open',
    );
    final objectStatus = _u16(objectOpened.body, 8);
    final objectToken = _u32(objectOpened.body);
    final objectMaximum = _u16(objectOpened.body, 4);
    final objectCapacity = objectOpened.body[6];
    if (objectStatus != 0 ||
        objectToken == 0 ||
        objectToken == _connectionToken ||
        objectToken == _streamToken ||
        objectOpened.body[7] != 1 ||
        objectCapacity == 0 ||
        objectCapacity > ConnectionSessionCodec.maximumWindow ||
        objectMaximum == 0 ||
        objectMaximum >
            _selectedPacketMaximum - ConnectionSessionCodec.headerBytes) {
      throw StateError('Connection-session object topic was refused');
    }
    _objectStreamToken = objectToken;
    _objectMaximumBody = objectMaximum;
    _objectSelectedCapacity = objectCapacity;
    _objectGrantThrough = objectCapacity;
    if (!_stateOpened.isCompleted) {
      // Ready is idempotent on the board and replays both Activated and its
      // retained reverse-State Open. Reuse it when that board-initiated Open
      // was the lifecycle record lost over the air.
      await _sendUntilReply(
        request: readyRequest,
        reply: _stateOpened.future,
        timeout: timeout,
        description: 'State Open',
      );
    }
    if (!_reverseObjectOpened.isCompleted) {
      await _sendUntilReply(
        request: readyRequest,
        reply: _reverseObjectOpened.future,
        timeout: timeout,
        description: 'Reverse Object Open',
      );
    }
  }

  /// Joins only work already owned at this boundary. Submission results include
  /// native completion and peer admission; submitted callers may have returned
  /// while their issued packet futures remain pending here.
  Future<void> drainObjects() async {
    if (_disposed) throw _retirementError;
    final work = <Future<void>>[
      for (final pending in _objectPending.values) pending.future,
      if (_activeObjectSubmission case final active?) active.result.future,
      for (final queued in _interactiveObjectSubmissions) queued.result.future,
      for (final queued in _bulkObjectSubmissions) queued.result.future,
    ];
    await _untilEnded(
      Future.wait<void>([
        for (final future in work)
          future.onError<ConnectionSessionObjectCancelled>((_, _) {}),
      ], eagerError: true),
    );
  }

  Future<void> sendObject(int opcode, Uint8List body) => sendObjectBatch([
    (opcode: opcode, body: body),
  ], priority: ConnectionSessionObjectPriority.interactive);

  /// Progress covers the peer-admitted prefix, including before the native
  /// producer finishes. Cancellation retires this observer with the submission.
  Future<void> sendObjectBatch(
    List<ConnectionSessionObject> objects, {
    ConnectionSessionObjectPriority priority =
        ConnectionSessionObjectPriority.bulk,
    bool atomicMessage = false,
    Future<void>? cancellationFuture,
    List<int>? messageEnds,
    void Function(Map<String, Object?> fields)? onObservation,
    void Function(int admittedObjects, int totalObjects)? onAdmissionProgress,
    ConnectionSessionObjectCompletion completion =
        ConnectionSessionObjectCompletion.admitted,
  }) {
    if (_disposed || !isActivated || objects.isEmpty) {
      throw StateError('Connection-session object stream is unavailable');
    }
    if (_queuedObjectSubmissionCount >= objectSubmissionQueueCapacity) {
      throw StateError('Connection-session object submission queue is full');
    }
    final immutable = objects
        .map(
          (object) =>
              (opcode: object.opcode, body: Uint8List.fromList(object.body)),
        )
        .toList(growable: false);
    if (immutable.any(
      (object) =>
          object.opcode < 0 ||
          object.opcode > 255 ||
          object.body.length > _objectMaximumBody,
    )) {
      throw StateError('Connection-session object exceeds negotiated bounds');
    }
    final canSubmit = _canSubmitObject;
    if (canSubmit != null &&
        immutable.any((object) => !canSubmit(object.opcode))) {
      throw StateError('Connection-session object admission is unavailable');
    }
    final ends = List<int>.unmodifiable(messageEnds ?? [immutable.length]);
    var previousEnd = 0;
    for (final end in ends) {
      if (end <= previousEnd || end > immutable.length) {
        throw ArgumentError.value(messageEnds, 'messageEnds');
      }
      previousEnd = end;
    }
    if (previousEnd != immutable.length) {
      throw ArgumentError.value(messageEnds, 'messageEnds');
    }
    final submission = _ObjectSubmission(
      objects: immutable,
      priority: priority,
      atomicMessage: atomicMessage,
      messageEnds: ends,
      onAdmissionProgress: onAdmissionProgress,
      observation: onObservation == null
          ? null
          : _ObjectSubmissionObservation(
              _observeNow,
              onObservation,
              clientNonce,
              immutable.length,
            ),
    );
    // Exactly one completion belongs to the caller. The session retains the
    // other lifetime and reports later admission failures through retirement.
    final future = completion == ConnectionSessionObjectCompletion.submitted
        ? submission.submitted.future
        : submission.result.future;
    (completion == ConnectionSessionObjectCompletion.submitted
            ? submission.result.future
            : submission.submitted.future)
        .ignore();
    _objectQueueFor(priority).addLast(submission);
    cancellationFuture?.then<void>((_) {
      submission.cancelUnissued();
      if (submission.cancelledEnd != null &&
          submission.nextIndex == submission.issueEnd &&
          !identical(_activeObjectSubmission, submission)) {
        _objectQueueFor(priority).remove(submission);
        submission.finishIssuing();
      }
      final waiter = _objectGrantWaiter;
      _objectGrantWaiter = null;
      if (waiter != null && !waiter.isCompleted) waiter.complete();
      _startObjectPump();
    }, onError: (Object _, StackTrace _) {});
    _startObjectPump();
    return future;
  }

  int get _queuedObjectSubmissionCount =>
      _interactiveObjectSubmissions.length +
      _bulkObjectSubmissions.length +
      (_activeObjectSubmission == null ? 0 : 1);

  Queue<_ObjectSubmission> _objectQueueFor(
    ConnectionSessionObjectPriority priority,
  ) => priority == ConnectionSessionObjectPriority.interactive
      ? _interactiveObjectSubmissions
      : _bulkObjectSubmissions;

  void _startObjectPump() {
    if (_objectPumpRunning || _disposed) return;
    _objectPumpRunning = true;
    unawaited(_pumpObjectSubmissions());
  }

  Future<void> _pumpObjectSubmissions() async {
    _ObjectSubmission? atomicContinuation;
    try {
      // Observe a retirement which already completed before this submission
      // before allocating any sequence or entering the native writer.
      await Future<void>.value();
      while (!_disposed) {
        int? grantWaitStarted;
        while (!_disposed && _nextObjectSequence > _objectGrantThrough) {
          if (atomicContinuation != null &&
              atomicContinuation.nextIndex >= atomicContinuation.issueEnd) {
            break;
          }
          if (atomicContinuation == null &&
              _interactiveObjectSubmissions.isEmpty &&
              _bulkObjectSubmissions.isEmpty) {
            return;
          }
          grantWaitStarted ??= _observeNow();
          _objectGrantWaiter ??= Completer<void>();
          _armObjectRecovery();
          await _objectGrantWaiter!.future;
        }
        if (_disposed) return;
        // Wholly unstarted work keeps competing for priority while credit is
        // closed. Only an already-started atomic message retains its boundary;
        // fairness counters advance when a burst can actually be issued.
        final submission = atomicContinuation ?? _takeObjectSubmission();
        if (submission == null) return;
        _activeObjectSubmission = submission;
        final observation = submission.observation;
        if (grantWaitStarted != null && observation != null) {
          observation.grantWait +=
              _observeNow() -
              (grantWaitStarted < observation.queued
                  ? observation.queued
                  : grantWaitStarted);
        }
        if (submission.nextIndex < submission.issueEnd) {
          await _issueObjectBurst(submission);
        }
        if (submission.nextIndex < submission.issueEnd) {
          if (submission.atomicMessage) {
            atomicContinuation = submission;
            continue;
          }
          _objectQueueFor(submission.priority).addLast(submission);
        } else {
          submission.finishIssuing();
        }
        atomicContinuation = null;
        _activeObjectSubmission = null;
      }
    } catch (error, stack) {
      // A native batch failure means the platform writer and this negotiated
      // Object stream no longer have a trustworthy ordering relationship. Do
      // not leave the session selectable as "connected" with pending Object
      // watermarks that can never advance; force the owning BLE service to
      // establish a fresh session before another Litchcraft command runs.
      // The sequential fallback still needs to surface an individual packet
      // failure to its caller so higher-level staging can send its bounded
      // Cancel operation (and then decide whether recovery is needed).
      if (_sendBatch != null) {
        _retireForRecovery(error, stack);
      } else {
        _activeObjectSubmission?.fail(error, stack);
      }
    } finally {
      _activeObjectSubmission = null;
      _armObjectRecovery();
      _objectPumpRunning = false;
      if (!_disposed && _queuedObjectSubmissionCount > 0) {
        _startObjectPump();
      }
    }
  }

  _ObjectSubmission? _takeObjectSubmission() {
    if (_interactiveObjectSubmissions.isNotEmpty &&
        (_bulkObjectSubmissions.isEmpty ||
            _consecutiveInteractiveObjectBursts <
                maximumConsecutiveInteractiveObjectBursts)) {
      _consecutiveInteractiveObjectBursts += 1;
      return _interactiveObjectSubmissions.removeFirst();
    }
    if (_bulkObjectSubmissions.isNotEmpty) {
      _consecutiveInteractiveObjectBursts = 0;
      return _bulkObjectSubmissions.removeFirst();
    }
    if (_interactiveObjectSubmissions.isNotEmpty) {
      _consecutiveInteractiveObjectBursts += 1;
      return _interactiveObjectSubmissions.removeFirst();
    }
    return null;
  }

  Future<void> _issueObjectBurst(_ObjectSubmission submission) async {
    final end = (submission.nextIndex + _objectSelectedCapacity).clamp(
      0,
      submission.issueEnd,
    );
    final encoded = <Uint8List>[];
    while (submission.nextIndex < end &&
        _nextObjectSequence <= _objectGrantThrough) {
      final object = submission.objects[submission.nextIndex++];
      final sequence = _nextObjectSequence++;
      submission.observation?.first ??= sequence;
      submission.observation?.last = sequence;
      final packet = ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.object,
          code: object.opcode,
          token: _objectStreamToken,
          sequence: sequence,
          body: object.body,
        ),
      );
      final completion = Completer<void>();
      _objectPending[sequence] = completion;
      _objectIssued[sequence] = packet;
      submission.track(completion.future);
      encoded.add(packet);
    }
    _armObjectRecovery();
    final batch = _sendBatch;
    final observation = submission.observation;
    final started = observation == null ? 0 : _observeNow();
    observation?.firstSend ??= started;
    try {
      if (batch == null) {
        for (final packet in encoded) {
          await _send(packet);
        }
      } else {
        await batch(encoded);
      }
    } finally {
      if (observation != null) {
        final returned = _observeNow();
        observation.native += returned - started;
        observation.lastReturn = returned;
      }
    }
  }

  Future<void> requestState({int code = 0}) {
    if (_disposed || _stateStreamToken == 0 || code < 0 || code > 0xff) {
      throw StateError('Connection-session state stream is unavailable');
    }
    return _sendControl(
      ConnectionSessionControl.query,
      token: _stateStreamToken,
      sequence: code,
    );
  }

  Future<ConnectionSessionPacket> readState({
    required int code,
    int earliestSequence = 0,
    bool Function(ConnectionSessionPacket packet)? accept,
  }) => readStateSnapshot<ConnectionSessionPacket>(
    code: code,
    accept: (packet) =>
        packet.code == code &&
            packet.sequence >= earliestSequence &&
            (accept == null || accept(packet))
        ? packet
        : null,
  );

  /// Replays replaceable State until its typed adapter has one coherent value.
  /// Zero packets and partial snapshots share the same bounded recovery owner.
  Future<T> readStateSnapshot<T>({
    int code = 0,
    ConnectionSessionStateReadMode mode = ConnectionSessionStateReadMode.query,
    required T? Function(ConnectionSessionPacket packet) accept,
    Duration? timeout,
  }) async {
    if (timeout != null && timeout <= recoveryProbeDelay) {
      throw ArgumentError.value(timeout, 'timeout');
    }
    if (_disposed || _stateStreamToken == 0 || code < 0 || code > 0xff) {
      throw StateError('Connection-session state stream is unavailable');
    }
    final observed = Completer<T>();
    observed.future.ignore();
    final subscription = states.listen(
      (packet) {
        if (observed.isCompleted) return;
        try {
          final value = accept(packet);
          if (value != null) observed.complete(value);
        } catch (error, stack) {
          observed.completeError(error, stack);
        }
      },
      onError: (Object error, StackTrace stack) {
        if (!observed.isCompleted) observed.completeError(error, stack);
      },
      onDone: () {
        if (!observed.isCompleted) {
          observed.completeError(StateError('Connection session ended'));
        }
      },
    );
    if (mode == ConnectionSessionStateReadMode.query) ++_activeStateReads;
    var completed = false;
    final elapsed = Stopwatch()..start();
    final deadline = timeout ?? recoveryFailureTimeout;
    try {
      if (mode == ConnectionSessionStateReadMode.observeFirst) {
        // Ordinary publication normally completes before a repair is needed.
        // The same lifetime and total deadline own this initial observation.
        try {
          return await _untilEnded(observed.future).timeout(recoveryProbeDelay);
        } on TimeoutException {
          // Continue through the existing bounded Query/replay owner.
        }
      }
      final value = await _sendUntilReply(
        request: _encodeControl(
          ConnectionSessionControl.query,
          token: _stateStreamToken,
          sequence: code,
        ),
        reply: observed.future,
        timeout: mode == ConnectionSessionStateReadMode.observeFirst
            ? deadline - elapsed.elapsed
            : deadline,
        description: 'state $code',
        canSend: mode == ConnectionSessionStateReadMode.observeFirst
            ? () => _activeStateReads == 0
            : null,
      );
      completed = true;
      return value;
    } finally {
      try {
        await subscription.cancel();
      } finally {
        if (mode == ConnectionSessionStateReadMode.query) {
          --_activeStateReads;
          if (completed && !_disposed) onStateReadCompleted?.call();
        }
      }
    }
  }

  Future<void> requestReverseObjectRecovery() {
    if (_disposed || _reverseObjectStreamToken == 0) {
      throw StateError(
        'Connection-session reverse Object stream is unavailable',
      );
    }
    return _sendControl(
      ConnectionSessionControl.query,
      token: _reverseObjectStreamToken,
    );
  }

  Future<void> requestObjectReceipt(int request) {
    if (_disposed ||
        _objectStreamToken == 0 ||
        request <= 0 ||
        request > 0xffff) {
      throw StateError('Connection-session Object receipt is unavailable');
    }
    return _sendControl(
      ConnectionSessionControl.query,
      token: _objectStreamToken,
      sequence: request,
    );
  }

  Future<ConnectionSessionOutcome> sendOrdered(
    int opcode,
    Uint8List body,
  ) async {
    if (_disposed ||
        !isActivated ||
        opcode < 0 ||
        opcode > 255 ||
        body.length > _orderedMaximumBody) {
      throw StateError('Connection-session ordered capacity is unavailable');
    }
    final completer = Completer<ConnectionSessionOutcome>();
    // Retirement can fail the outcome before the native write returns. Observe
    // that early error now; the returned future still reports it to the caller.
    completer.future.ignore();
    final immutableBody = Uint8List.fromList(body);
    final issue = _issueTail.then((_) async {
      while (!_disposed && _nextSequence > _grantThrough) {
        final waiter = _grantWaiter ??= Completer<void>();
        _armRecovery();
        await waiter.future.timeout(const Duration(seconds: 10));
      }
      if (_disposed) throw StateError('Connection session ended');
      final sequence = _nextSequence++;
      _pending[sequence] = completer;
      final encoded = ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.ordered,
          code: opcode,
          token: _streamToken,
          sequence: sequence,
          body: immutableBody,
        ),
      );
      _issued[sequence] = encoded;
      _armRecovery();
      try {
        await _send(encoded);
      } catch (error, stack) {
        // Issued bytes may already have reached the board. Retire their shared
        // lifetime before queued actions or repair can reuse this uncertain
        // stream; pre-issue failures still affect only their own caller.
        _retireForRecovery(error, stack);
        rethrow;
      }
    });
    _issueTail = issue.catchError((Object _, StackTrace _) {});
    unawaited(
      issue.catchError((Object error, StackTrace stack) {
        if (!completer.isCompleted) completer.completeError(error, stack);
      }),
    );
    return _untilEnded(completer.future);
  }

  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    _retirementError = StateError('Connection session disposed');
    if (!_ended.isCompleted) _ended.complete();
    _completePendingError(StateError('Connection session disposed'));
    _recoveryTimer?.cancel();
    _objectRecoveryTimer?.cancel();
    _failOrdered(StateError('Connection session disposed'));
    _failObject(StateError('Connection session disposed'));
    await _subscription.cancel();
    await _states.close();
    await _objectReceipts.close();
  }

  Future<void> heartbeat() {
    if (_disposed || !isActivated) {
      throw StateError('Connection session is not active');
    }
    return _sendControl(ConnectionSessionControl.heartbeat);
  }

  Uint8List _encodeControl(
    int code, {
    int sequence = 0,
    Uint8List? body,
    int? token,
  }) {
    final streamControl =
        code == ConnectionSessionControl.confirm ||
        code == ConnectionSessionControl.query;
    return ConnectionSessionCodec.encode(
      ConnectionSessionPacket(
        kind: ConnectionSessionKind.control,
        code: code,
        token: token ?? (streamControl ? _streamToken : _connectionToken),
        sequence: sequence,
        body: body ?? Uint8List(0),
      ),
    );
  }

  Future<void> _sendControl(
    int code, {
    int sequence = 0,
    Uint8List? body,
    int? token,
  }) =>
      _send(_encodeControl(code, sequence: sequence, body: body, token: token));

  Future<T> _untilEnded<T>(Future<T> work) => Future.any<T>([
    work,
    _ended.future.then<T>((_) => throw _retirementError),
  ]);

  Future<T> _sendUntilReply<T>({
    required Uint8List request,
    required Future<T> reply,
    required Duration timeout,
    required String description,
    bool Function()? canSend,
  }) async {
    // A response can fail while its SDK write is still pending. Observe that
    // early error without changing the failure returned to the caller.
    reply.ignore();
    final clock = Stopwatch()..start();
    while (true) {
      if (_disposed) throw _retirementError;
      var remaining = timeout - clock.elapsed;
      if (remaining <= Duration.zero) {
        throw TimeoutException(
          'Connection-session $description was not acknowledged',
          timeout,
        );
      }
      if (canSend?.call() ?? true) {
        final sent = _send(request);
        await _untilEnded(
          Future.any<Object?>([
            sent.then<Object?>((_) => null),
            reply.then<Object?>((value) => value),
          ]),
        ).timeout(remaining);
      }
      remaining = timeout - clock.elapsed;
      if (remaining <= Duration.zero) {
        throw TimeoutException(
          'Connection-session $description was not acknowledged',
          timeout,
        );
      }
      final retryWindow = remaining < recoveryProbeDelay
          ? remaining
          : recoveryProbeDelay;
      try {
        return await _untilEnded(reply).timeout(retryWindow);
      } on TimeoutException {
        if (clock.elapsed >= timeout) {
          throw TimeoutException(
            'Connection-session $description was not acknowledged',
            timeout,
          );
        }
      }
    }
  }

  void _onBytes(List<int> raw) {
    if (_disposed) return;
    final bytes = Uint8List.fromList(raw);
    final packet = ConnectionSessionCodec.decode(
      bytes,
      packetLimit: _selectedPacketMaximum == 0
          ? packetMaximum
          : _selectedPacketMaximum,
    );
    if (packet == null) return;
    if (packet.kind == ConnectionSessionKind.object) {
      if (packet.token == _reverseObjectStreamToken &&
          _reverseObjectStreamToken != 0) {
        _acceptReverseObject(packet, bytes);
      }
      return;
    }
    if (packet.kind == ConnectionSessionKind.state) {
      if (_objectStreamToken != 0 &&
          packet.token == _objectStreamToken &&
          packet.body.length <= _objectMaximumBody &&
          packet.sequence > 0) {
        _objectReceipts.add(
          ConnectionSessionPacket(
            kind: packet.kind,
            code: packet.code,
            token: packet.token,
            sequence: packet.sequence,
            body: Uint8List.fromList(packet.body),
          ),
        );
      } else if (_stateStreamToken != 0 &&
          packet.token == _stateStreamToken &&
          packet.body.length <= _stateSelectedMaximumBody &&
          packet.code > 0 &&
          packet.sequence > 0) {
        _states.add(
          ConnectionSessionPacket(
            kind: packet.kind,
            code: packet.code,
            token: packet.token,
            sequence: packet.sequence,
            body: Uint8List.fromList(packet.body),
          ),
        );
      }
      return;
    }
    if (packet.kind != ConnectionSessionKind.control) return;
    switch (packet.code) {
      case ConnectionSessionControl.welcome:
        _welcomeBytes ??= bytes;
        if (!_welcome.isCompleted) _welcome.complete(packet);
        break;
      case ConnectionSessionControl.identity:
        _identityBytes ??= bytes;
        if (!_identity.isCompleted) _identity.complete(packet);
        break;
      case ConnectionSessionControl.activated:
        if (packet.token == _connectionToken && !_activated.isCompleted) {
          _activated.complete(packet);
        }
        break;
      case ConnectionSessionControl.opened:
        if (packet.token == _connectionToken) {
          if (packet.sequence == 1 && !_opened.isCompleted) {
            _opened.complete(packet);
          } else if (packet.sequence == 2 && !_objectOpened.isCompleted) {
            _objectOpened.complete(packet);
          }
        }
        break;
      case ConnectionSessionControl.open:
        if (packet.sequence == 1) {
          unawaited(_acceptStateOpen(packet, bytes));
        } else if (packet.sequence == 2) {
          unawaited(_acceptReverseObjectOpen(packet, bytes));
        }
        break;
      case ConnectionSessionControl.progress:
        if (packet.token == _objectStreamToken && _objectStreamToken != 0) {
          _acceptObjectProgress(packet);
        } else if (_streamToken != 0 && packet.token == _streamToken) {
          final terminal = packet.sequence;
          final confirmed = _u32(packet.body);
          final grant = _u32(packet.body, 4);
          final receivedAhead = _u16(packet.body, 8);
          final advanced =
              terminal > _progressTerminal ||
              confirmed > _receiverConfirmedThrough ||
              grant > _grantThrough ||
              (terminal == _progressTerminal &&
                  receivedAhead != _progressReceivedAhead);
          if (terminal < _progressTerminal ||
              confirmed < _receiverConfirmedThrough ||
              confirmed > terminal ||
              terminal > grant ||
              grant - confirmed > _orderedSelectedCapacity) {
            break;
          }
          _progressTerminal = terminal;
          _progressReceivedAhead = receivedAhead;
          if (confirmed > _receiverConfirmedThrough) {
            _receiverConfirmedThrough = confirmed;
            _issued.removeWhere(
              (sequence, _) => sequence <= _receiverConfirmedThrough,
            );
            _retransmissionQueued.removeWhere(
              (sequence) => sequence <= _receiverConfirmedThrough,
            );
          }
          if (grant >= _grantThrough) {
            _grantThrough = grant;
            final waiter = _grantWaiter;
            _grantWaiter = null;
            if (waiter != null && !waiter.isCompleted) waiter.complete();
          }
          // Advancing snapshots can have been queued before our Query and may
          // truthfully omit packets submitted later. Only an unchanged Query
          // response establishes a persistent hole worth retransmitting.
          if (_queryOutstanding && !advanced) {
            _queueMissingRetransmissions(terminal, grant, receivedAhead);
          }
          _replayConfirmIfNeeded();
          // Without wire correlation, only a stable snapshot can establish
          // Query liveness; an unsolicited advanced tail is not that proof.
          final reply = _queryOutstanding && !advanced;
          if (advanced) {
            _recoveryAdvanced(reply: reply);
          } else if (reply) {
            _queryOutstanding = false;
            // An unchanged reply proves liveness only for retained head work.
            // Missing positions keep their original no-progress deadline.
            _armRecovery(reply: true);
          }
        }
        break;
      case ConnectionSessionControl.checkpoint:
        if (packet.token == _streamToken) _acceptCheckpoint(packet);
        break;
      case ConnectionSessionControl.outcome:
        if (packet.token == _streamToken) _acceptOutcome(packet);
        break;
    }
  }

  Future<void> _acceptStateOpen(
    ConnectionSessionPacket packet,
    Uint8List encoded,
  ) async {
    try {
      if (packet.token != _connectionToken ||
          packet.sequence != 1 ||
          _u16(packet.body) != litchcraftStateTopic ||
          packet.body[2] != 1 ||
          packet.body[3] == 0 ||
          packet.body[3] > stateCapacity ||
          _u16(packet.body, 4) == 0 ||
          _u16(packet.body, 4) > stateMaximumBody) {
        throw const FormatException('Invalid connection state stream');
      }
      if (_stateOpenRequest != null) {
        if (!_bytesEqual(_stateOpenRequest!, encoded)) {
          throw const FormatException('Changed duplicate state Open');
        }
        await _send(_stateOpenedResponse!);
        return;
      }
      final streamToken = _nextCentralToken++;
      if ((streamToken & 0x80000000) == 0) {
        throw StateError('Central stream-token space exhausted');
      }
      final body = Uint8List(10);
      final data = ByteData.sublistView(body);
      data.setUint32(0, streamToken, Endian.little);
      data.setUint16(4, _u16(packet.body, 4), Endian.little);
      body[6] = packet.body[3];
      body[7] = 1;
      final response = ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.control,
          code: ConnectionSessionControl.opened,
          token: _connectionToken,
          sequence: packet.sequence,
          body: body,
        ),
      );
      _stateOpenRequest = Uint8List.fromList(encoded);
      _stateOpenedResponse = response;
      _stateStreamToken = streamToken;
      _stateSelectedMaximumBody = _u16(packet.body, 4);
      await _send(response);
      if (!_stateOpened.isCompleted) _stateOpened.complete();
    } catch (error, stack) {
      if (!_stateOpened.isCompleted) _stateOpened.completeError(error, stack);
      _retireForRecovery(error, stack);
    }
  }

  Future<void> _acceptReverseObjectOpen(
    ConnectionSessionPacket packet,
    Uint8List encoded,
  ) async {
    try {
      if (packet.token != _connectionToken ||
          packet.sequence != 2 ||
          _u16(packet.body) != litchcraftReverseObjectTopic ||
          packet.body[2] != 1 ||
          packet.body[3] == 0 ||
          packet.body[3] > reverseObjectCapacity ||
          _u16(packet.body, 4) == 0 ||
          _u16(packet.body, 4) >
              _selectedPacketMaximum - ConnectionSessionCodec.headerBytes) {
        throw const FormatException('Invalid reverse Object stream');
      }
      if (_reverseObjectOpenRequest != null) {
        if (!_bytesEqual(_reverseObjectOpenRequest!, encoded)) {
          throw const FormatException('Changed duplicate reverse Object Open');
        }
        await _send(_reverseObjectOpenedResponse!);
        return;
      }
      final streamToken = _nextCentralToken++;
      if ((streamToken & 0x80000000) == 0) {
        throw StateError('Central stream-token space exhausted');
      }
      final body = Uint8List(10);
      final data = ByteData.sublistView(body);
      data.setUint32(0, streamToken, Endian.little);
      data.setUint16(4, _u16(packet.body, 4), Endian.little);
      body[6] = packet.body[3];
      body[7] = 1;
      final response = ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.control,
          code: ConnectionSessionControl.opened,
          token: _connectionToken,
          sequence: packet.sequence,
          body: body,
        ),
      );
      _reverseObjectOpenRequest = Uint8List.fromList(encoded);
      _reverseObjectOpenedResponse = response;
      _reverseObjectStreamToken = streamToken;
      _reverseObjectMaximumBody = _u16(packet.body, 4);
      _reverseObjectSelectedCapacity = packet.body[3];
      _reverseObjectGrantThrough = _reverseObjectSelectedCapacity;
      await _send(response);
      if (!_reverseObjectOpened.isCompleted) {
        _reverseObjectOpened.complete();
      }
    } catch (error, stack) {
      if (!_reverseObjectOpened.isCompleted) {
        _reverseObjectOpened.completeError(error, stack);
      }
      _retireForRecovery(error, stack);
    }
  }

  void _acceptReverseObject(ConnectionSessionPacket packet, Uint8List encoded) {
    final sequence = packet.sequence;
    if (packet.body.length > _reverseObjectMaximumBody ||
        sequence > _reverseObjectGrantThrough) {
      return;
    }
    if (sequence <= _reverseObjectConfirmedThrough) {
      _publishReverseObjectProgress();
      return;
    }
    final prior = _reverseObjectReceived[sequence];
    if (prior != null) {
      if (!_bytesEqual(prior, encoded)) {
        _retireForRecovery(
          const FormatException('Changed duplicate reverse Object'),
        );
      }
      _publishReverseObjectProgress();
      return;
    }
    _reverseObjectReceived[sequence] = Uint8List.fromList(encoded);
    while (true) {
      final nextBytes = _reverseObjectReceived.remove(
        _reverseObjectConfirmedThrough + 1,
      );
      if (nextBytes == null) break;
      final nextPacket = ConnectionSessionCodec.decode(
        nextBytes,
        packetLimit: _selectedPacketMaximum == 0
            ? packetMaximum
            : _selectedPacketMaximum,
      );
      if (nextPacket == null) {
        _retireForRecovery(
          const FormatException('Retained reverse Object did not decode'),
        );
        return;
      }
      _reverseObjectConfirmedThrough += 1;
      _objectReceipts.add(nextPacket);
    }
    _reverseObjectGrantThrough =
        _reverseObjectConfirmedThrough + _reverseObjectSelectedCapacity;
    _publishReverseObjectProgress();
  }

  void _publishReverseObjectProgress() {
    if (_disposed || _reverseObjectStreamToken == 0) return;
    // Retained packets may be resent while one SDK write is pending. Keep the
    // newest cumulative cursor instead of queuing one write per duplicate.
    _reverseObjectProgressDirty = true;
    if (_reverseObjectProgressSending) return;
    _reverseObjectProgressSending = true;
    unawaited(_drainReverseObjectProgress());
  }

  Future<void> _drainReverseObjectProgress() async {
    try {
      while (_reverseObjectProgressDirty && !_disposed) {
        _reverseObjectProgressDirty = false;
        var receivedAhead = 0;
        for (final sequence in _reverseObjectReceived.keys) {
          final offset = sequence - _reverseObjectConfirmedThrough - 1;
          if (offset >= 0 && offset < 16) receivedAhead |= 1 << offset;
        }
        final body = Uint8List(10);
        final data = ByteData.sublistView(body);
        data.setUint32(0, _reverseObjectConfirmedThrough, Endian.little);
        data.setUint32(4, _reverseObjectGrantThrough, Endian.little);
        data.setUint16(8, receivedAhead, Endian.little);
        final packet = _encodeControl(
          ConnectionSessionControl.progress,
          token: _reverseObjectStreamToken,
          sequence: _reverseObjectConfirmedThrough,
          body: body,
        );
        await _send(packet);
      }
    } catch (error, stack) {
      _retireForRecovery(error, stack);
    } finally {
      _reverseObjectProgressSending = false;
    }
  }

  void _acceptCheckpoint(ConnectionSessionPacket packet) {
    final through = packet.sequence;
    if (through < _checkpointThrough || through > _grantThrough) return;
    final advanced = through > _checkpointThrough;
    _checkpointThrough = through;
    _checkpointRevision = _u32(packet.body);
    _checkpointDigest = _u32(packet.body, 4);
    final exceptions = _u16(packet.body, 8);
    _checkpointExceptions = exceptions;
    for (
      var sequence = _confirmedThrough + 1;
      sequence <= through;
      sequence++
    ) {
      final offset = through - sequence;
      if (offset >= 16 || (exceptions & (1 << offset)) != 0) continue;
      _complete[sequence] = ConnectionSessionOutcome(
        sequence: sequence,
        disposition: ConnectionSessionDisposition.applied,
        reason: 0,
        domainRevision: _checkpointRevision,
        domainDigest: _checkpointDigest,
      );
    }
    for (final entry in _exceptionOutcomes.entries.toList()) {
      if (entry.key > through) continue;
      _complete[entry.key] = ConnectionSessionOutcome(
        sequence: entry.key,
        disposition: entry.value.$1,
        reason: entry.value.$2,
        domainRevision: _checkpointRevision,
        domainDigest: _checkpointDigest,
      );
      _exceptionOutcomes.remove(entry.key);
    }
    if (advanced) _recoveryAdvanced();
    _settleAndConfirm();
  }

  void _acceptOutcome(ConnectionSessionPacket packet) {
    if (packet.sequence <= _confirmedThrough || packet.body.length != 3) return;
    final raw = packet.body[0];
    final reason = _u16(packet.body, 1);
    if (raw == 0 || raw > 2 || reason == 0) return;
    final disposition = raw == 1
        ? ConnectionSessionDisposition.rejected
        : ConnectionSessionDisposition.cancelled;
    final previous = _exceptionOutcomes[packet.sequence];
    _exceptionOutcomes[packet.sequence] = (disposition, reason);
    if (previous == null) _recoveryAdvanced();
    if (packet.sequence <= _checkpointThrough) {
      _complete[packet.sequence] = ConnectionSessionOutcome(
        sequence: packet.sequence,
        disposition: disposition,
        reason: reason,
        domainRevision: _checkpointRevision,
        domainDigest: _checkpointDigest,
      );
      _exceptionOutcomes.remove(packet.sequence);
      _settleAndConfirm();
    }
  }

  void _settleAndConfirm() {
    // Keep terminal positions intact until the preceding cumulative Confirm is
    // accepted by the transport. Otherwise checkpoints arriving during that
    // write can complete futures but lose the watermark needed to free grant.
    if (_disposed || _confirmInFlight) return;
    var through = _confirmedThrough;
    while (_complete.containsKey(through + 1)) {
      through++;
      final result = _complete.remove(through)!;
      _pending.remove(through)?.complete(result);
    }
    if (through == _confirmedThrough) return;
    _confirmInFlight = true;
    // Terminal success ends the head allowance. A held Confirm SDK write
    // still needs ordinary bounded recovery, even with no pending actions.
    _armRecovery(progress: true);
    final confirming = through;
    _sendControl(ConnectionSessionControl.confirm, sequence: confirming).then(
      (_) {
        if (_disposed) return;
        if (confirming > _confirmedThrough) _confirmedThrough = confirming;
        _confirmInFlight = false;
        _armRecovery();
        _settleAndConfirm();
      },
      onError: (Object error, StackTrace stack) {
        _confirmInFlight = false;
        _retireForRecovery(error, stack);
      },
    );
  }

  void _onError(Object error, StackTrace stack) {
    _retireForRecovery(error, stack);
  }

  void _onDone() {
    _retireForRecovery(StateError('Connection session ended'));
  }

  void _acceptObjectProgress(ConnectionSessionPacket packet) {
    if (packet.body.length != 10) return;
    final accepted = _u32(packet.body);
    final grant = _u32(packet.body, 4);
    final receivedAhead = _u16(packet.body, 8);
    if (packet.sequence != accepted ||
        accepted < _objectAcceptedThrough ||
        accepted > grant ||
        grant < _objectGrantThrough ||
        grant - accepted > _objectSelectedCapacity ||
        receivedAhead != 0) {
      return;
    }
    final advanced =
        accepted > _objectAcceptedThrough || grant > _objectGrantThrough;
    _objectAcceptedThrough = accepted;
    _objectGrantThrough = grant;
    for (final sequence in _objectPending.keys.toList()) {
      if (sequence > accepted) continue;
      _objectPending.remove(sequence)?.complete();
      _objectIssued.remove(sequence);
    }
    if (_nextObjectSequence <= grant) {
      final waiter = _objectGrantWaiter;
      _objectGrantWaiter = null;
      if (waiter != null && !waiter.isCompleted) waiter.complete();
    }
    if (_objectQueryOutstanding && !advanced) {
      final repaired = _retransmitNextObject();
      _objectRecoveryBackoff = repaired ? 1 : _objectRecoveryBackoff;
    }
    _objectQueryOutstanding = false;
    _armObjectRecovery(progress: advanced);
  }

  bool _retransmitNextObject() {
    // Firmware admits Object envelopes sequentially. Later retained packets
    // cannot advance its watermark while the first missing sequence remains a
    // hole, and bursting the whole suffix only adds pressure to the BLE queue.
    // Retry that exact earliest envelope on every unchanged progress reply;
    // the fixed recovery deadline still bounds a genuinely dead connection.
    final packet = _objectIssued[_objectAcceptedThrough + 1];
    if (packet == null) return false;
    final resend = _objectRecoveryTail.then((_) => _send(packet));
    _objectRecoveryTail = resend.then<void>(
      (_) {},
      onError: (Object error, StackTrace stack) {
        _retireForRecovery(error, stack);
      },
    );
    return true;
  }

  void _armObjectRecovery({bool progress = false}) {
    _objectRecoveryTimer?.cancel();
    if (_disposed || !_hasOutstandingObjectRecovery) {
      _objectRecoveryDeadline = null;
      _objectRecoveryBackoff = 1;
      return;
    }
    if (progress || _objectRecoveryDeadline == null) {
      _objectRecoveryDeadline = Stopwatch()..start();
      _objectRecoveryBackoff = 1;
    }
    _objectRecoveryTimer = Timer(
      _nextRecoveryDelay(_objectRecoveryDeadline!, _objectRecoveryBackoff),
      _recoverObject,
    );
  }

  void _recoverObject() {
    if (_disposed || !_hasOutstandingObjectRecovery) return;
    if (_recoveryExpired(_objectRecoveryDeadline)) {
      _retireForRecovery(
        TimeoutException(
          'Connection-session object stream exceeded its recovery deadline',
          recoveryFailureTimeout,
        ),
      );
      return;
    }
    if (_objectRecoveryWriteInFlight) {
      _armObjectRecovery();
      return;
    }
    _objectQueryOutstanding = true;
    _objectRecoveryWriteInFlight = true;
    _objectRecoveryBackoff = _nextRecoveryBackoff(_objectRecoveryBackoff);
    _sendControl(
      ConnectionSessionControl.query,
      token: _objectStreamToken,
    ).then((_) {
      _objectRecoveryWriteInFlight = false;
      _armObjectRecovery();
    }, onError: _retireForRecovery);
    _armObjectRecovery();
  }

  void _failObject(Object error, [StackTrace? stack]) {
    _objectRecoveryTimer?.cancel();
    _objectRecoveryDeadline = null;
    _objectRecoveryBackoff = 1;
    final waiter = _objectGrantWaiter;
    _objectGrantWaiter = null;
    if (waiter != null && !waiter.isCompleted) {
      waiter.completeError(error, stack);
    }
    for (final pending in _objectPending.values) {
      if (!pending.isCompleted) pending.completeError(error, stack);
    }
    _objectPending.clear();
    _objectIssued.clear();
    _activeObjectSubmission?.fail(error, stack);
    for (final queue in [
      _interactiveObjectSubmissions,
      _bulkObjectSubmissions,
    ]) {
      while (queue.isNotEmpty) {
        queue.removeFirst().fail(error, stack);
      }
    }
  }

  void _failOrdered(Object error, [StackTrace? stack]) {
    _recoveryTimer?.cancel();
    _recoveryDeadline = null;
    _receivedHeadSequence = 0;
    _receivedHeadDeadline = null;
    _recoveryBackoff = 1;
    final waiter = _grantWaiter;
    _grantWaiter = null;
    if (waiter != null && !waiter.isCompleted) {
      waiter.completeError(error, stack);
    }
    for (final pending in _pending.values) {
      if (!pending.isCompleted) pending.completeError(error, stack);
    }
    _pending.clear();
    _issued.clear();
    _retransmissionQueued.clear();
  }

  bool _queueMissingRetransmissions(
    int terminal,
    int grant,
    int receivedAhead,
  ) {
    var queued = false;
    final issuedThrough = _nextSequence - 1;
    final through = issuedThrough < grant ? issuedThrough : grant;
    for (var sequence = terminal + 1; sequence <= through; sequence++) {
      final bit = sequence - terminal - 1;
      if (bit >= ConnectionSessionCodec.maximumWindow ||
          (receivedAhead & (1 << bit)) != 0 ||
          !_issued.containsKey(sequence) ||
          _retransmissionQueued.contains(sequence)) {
        continue;
      }
      _retransmissionQueued.add(sequence);
      queued = true;
      final exact = _issued[sequence]!;
      final resend = _recoveryTail.then((_) async {
        // Earlier native work may retire the session or confirm this record
        // while its repair waits. Neither can leave a queued replay live.
        if (_disposed || !identical(_issued[sequence], exact)) return;
        await _send(exact);
      });
      _recoveryTail = resend
          .whenComplete(() {
            _retransmissionQueued.remove(sequence);
          })
          .catchError((Object error, StackTrace stack) {
            _retireForRecovery(error, stack);
          });
    }
    return queued;
  }

  bool _replayConfirmIfNeeded() {
    if (_confirmedThrough <= _receiverConfirmedThrough ||
        _confirmInFlight ||
        _confirmReplayInFlight) {
      return false;
    }
    _confirmReplayInFlight = true;
    _sendControl(
      ConnectionSessionControl.confirm,
      sequence: _confirmedThrough,
    ).then((_) {
      _confirmReplayInFlight = false;
      _armRecovery();
    }, onError: _retireForRecovery);
    return true;
  }

  void _recoveryAdvanced({bool reply = false}) {
    _queryOutstanding = false;
    _armRecovery(progress: true, reply: reply);
  }

  bool get _hasReceivedPendingHead =>
      !_confirmInFlight &&
      !_confirmReplayInFlight &&
      _receivedHeadSequence == _progressTerminal + 1 &&
      (_progressReceivedAhead & 1) != 0 &&
      _pending.containsKey(_receivedHeadSequence) &&
      !_pending.keys.any((sequence) => sequence <= _progressTerminal);

  bool _updateReceivedHeadRecovery() {
    if (_receivedHeadSequence != 0 &&
        (_progressTerminal >= _receivedHeadSequence ||
            !_pending.containsKey(_receivedHeadSequence))) {
      _receivedHeadSequence = 0;
      _receivedHeadDeadline = null;
    }
    final head = _progressTerminal + 1;
    if (_receivedHeadSequence == 0 &&
        !_confirmInFlight &&
        !_confirmReplayInFlight &&
        (_progressReceivedAhead & 1) != 0 &&
        _pending.containsKey(head) &&
        !_pending.keys.any((sequence) => sequence <= _progressTerminal)) {
      // Receipt proves retention, not success. Only this nonterminal head
      // earns a fixed completion allowance; tail traffic cannot renew it.
      _receivedHeadSequence = head;
      _receivedHeadDeadline = Stopwatch()..start();
      return true;
    }
    return false;
  }

  void _armRecovery({bool progress = false, bool reply = false}) {
    if (_disposed || !_hasOutstandingRecovery) {
      _recoveryTimer?.cancel();
      _recoveryDeadline = null;
      _receivedHeadSequence = 0;
      _receivedHeadDeadline = null;
      _recoveryBackoff = 1;
      return;
    }
    final headStarted = _updateReceivedHeadRecovery();
    if (_hasReceivedPendingHead &&
        !headStarted &&
        !reply &&
        _recoveryTimer?.isActive == true) {
      // New tail submissions/notifications cannot postpone the next head probe.
      return;
    }
    _recoveryTimer?.cancel();
    if (_recoveryDeadline == null ||
        headStarted ||
        (progress && !_hasReceivedPendingHead)) {
      _recoveryDeadline = Stopwatch()..start();
      _recoveryBackoff = 1;
    } else if (reply && _hasReceivedPendingHead) {
      // Query replies refresh silence detection, never completion. Keep the
      // probe backoff so a responsive slow worker does not cause a repair storm.
      _recoveryDeadline = Stopwatch()..start();
    }
    var delay = _nextRecoveryDelay(_recoveryDeadline!, _recoveryBackoff);
    final completion = _receivedHeadDeadline;
    if (completion != null) {
      final remaining = receivedHeadCompletionTimeout - completion.elapsed;
      if (remaining < delay) {
        delay = remaining > Duration.zero ? remaining : Duration.zero;
      }
    }
    _recoveryTimer = Timer(delay, _recover);
  }

  void _recover() {
    if (_disposed || !_hasOutstandingRecovery) return;
    final completion = _receivedHeadDeadline;
    if (completion != null &&
        completion.elapsed >= receivedHeadCompletionTimeout) {
      _retireForRecovery(
        TimeoutException(
          'Connection session exceeded its received-head completion deadline',
          receivedHeadCompletionTimeout,
        ),
      );
      return;
    }
    if (_recoveryExpired(_recoveryDeadline)) {
      _retireForRecovery(
        TimeoutException(
          'Connection session exceeded its recovery deadline',
          recoveryFailureTimeout,
        ),
      );
      return;
    }
    if (_recoveryWriteInFlight) {
      _armRecovery();
      return;
    }
    _queryOutstanding = true;
    _recoveryWriteInFlight = true;
    _recoveryBackoff = _nextRecoveryBackoff(_recoveryBackoff);
    var requestedOutcome = 0;
    for (
      var sequence = _confirmedThrough + 1;
      sequence <= _checkpointThrough;
      sequence++
    ) {
      final bit = _checkpointThrough - sequence;
      if (bit < 16 &&
          (_checkpointExceptions & (1 << bit)) != 0 &&
          !_exceptionOutcomes.containsKey(sequence) &&
          !_complete.containsKey(sequence)) {
        requestedOutcome = sequence;
        break;
      }
    }
    _sendControl(
      ConnectionSessionControl.query,
      sequence: requestedOutcome,
    ).then((_) {
      _recoveryWriteInFlight = false;
      _armRecovery();
    }, onError: _retireForRecovery);
    // The SDK may hold a write. Keep deadlines live without queuing more Queries.
    _armRecovery();
  }

  Duration _nextRecoveryDelay(Stopwatch deadline, int backoff) {
    final remaining = recoveryFailureTimeout - deadline.elapsed;
    if (remaining <= Duration.zero) return Duration.zero;
    final proposed = Duration(
      microseconds: recoveryProbeDelay.inMicroseconds * backoff,
    );
    return proposed < remaining ? proposed : remaining;
  }

  bool _recoveryExpired(Stopwatch? deadline) =>
      deadline == null || deadline.elapsed >= recoveryFailureTimeout;

  static int _nextRecoveryBackoff(int current) =>
      current >= 8 ? 8 : current * 2;

  bool get _hasOutstandingRecovery =>
      _pending.isNotEmpty ||
      _grantWaiter != null ||
      _confirmInFlight ||
      _confirmReplayInFlight ||
      _confirmedThrough > _receiverConfirmedThrough;

  bool get _hasOutstandingObjectRecovery =>
      _objectPending.isNotEmpty || _objectGrantWaiter != null;

  void _retireForRecovery(Object error, [StackTrace? stack]) {
    if (_disposed) return;
    final cause = switch (error) {
      TimeoutException(:final message)
          when message?.startsWith('Connection-session object stream') ==
              true =>
        'object_deadline',
      TimeoutException(:final message)
          when message?.startsWith(
                'Connection session exceeded its received-head',
              ) ==
              true =>
        'ordered_completion_deadline',
      TimeoutException(:final message)
          when message?.startsWith('Connection session exceeded') == true =>
        'ordered_deadline',
      TimeoutException() => 'timeout',
      FormatException() => 'protocol',
      StateError(:final message) when message == 'Connection session ended' =>
        'notification_done',
      StateError() => 'state',
      _ => 'transport',
    };
    try {
      final orderedSequence = _pending.isEmpty
          ? null
          : _pending.keys.reduce((a, b) => a < b ? a : b);
      final orderedHead = orderedSequence == null
          ? null
          : _issued[orderedSequence];
      final objectHead = _objectIssued[_objectAcceptedThrough + 1];
      onRetired?.call(<String, Object?>{
        'cause': cause,
        'error_kind': error.runtimeType.toString(),
        'ordered_pending': _pending.length,
        'ord_head_seq': orderedSequence,
        'ord_head_op': orderedHead?[1],
        'ord_terminal': _progressTerminal,
        'ord_ahead': _progressReceivedAhead,
        'object_pending': _objectPending.length,
        'reverse_confirmed': _reverseObjectConfirmedThrough,
        'reverse_grant': _reverseObjectGrantThrough,
        'reverse_buffered': _reverseObjectReceived.length,
        'head_op': objectHead?[1],
        'head_seq': objectHead == null ? null : _objectAcceptedThrough + 1,
        'obj_accepted': _objectAcceptedThrough,
        'obj_grant': _objectGrantThrough,
        'obj_query': _objectQueryOutstanding,
        'query_write': _objectRecoveryWriteInFlight,
      });
    } catch (_) {
      // Diagnostics cannot change session retirement or recovery.
    }
    _disposed = true;
    _retirementError = error;
    if (!_ended.isCompleted) _ended.complete();
    _failOrdered(error, stack);
    _failObject(error, stack);
    _completePendingError(error, stack);
    unawaited(_subscription.cancel());
  }

  void _completePendingError(Object error, [StackTrace? stack]) {
    for (final completer in [
      _welcome,
      _identity,
      _activated,
      _opened,
      _objectOpened,
      _stateOpened,
      _reverseObjectOpened,
    ]) {
      if (!completer.isCompleted) completer.completeError(error, stack);
    }
    if (!_states.isClosed) unawaited(_states.close());
    if (!_objectReceipts.isClosed) unawaited(_objectReceipts.close());
  }

  static bool _bytesEqual(Uint8List left, Uint8List right) {
    if (left.length != right.length) return false;
    for (var index = 0; index < left.length; index++) {
      if (left[index] != right[index]) return false;
    }
    return true;
  }

  static int _u16(Uint8List bytes, [int offset = 0]) =>
      ByteData.sublistView(bytes).getUint16(offset, Endian.little);
  static int _u32(Uint8List bytes, [int offset = 0]) =>
      ByteData.sublistView(bytes).getUint32(offset, Endian.little);
  static int _u64(Uint8List bytes, [int offset = 0]) =>
      ByteData.sublistView(bytes).getUint64(offset, Endian.little);

  static int _crc32(Uint8List bytes) {
    var crc = 0xffffffff;
    for (final byte in bytes) {
      crc ^= byte;
      for (var bit = 0; bit < 8; bit++) {
        crc = (crc >>> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
      }
    }
    return (~crc) & 0xffffffff;
  }
}
