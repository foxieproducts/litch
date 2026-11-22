import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:litch_mobile/src/online/lichess/lichess_board_api_client.dart';
import 'package:litch_mobile/src/online/lichess/lichess_prepared_session.dart';
import 'package:litch_mobile/src/online/lichess/lichess_prepared_file_store.dart';
import 'package:litch_mobile/src/online/lichess/lichess_request_coordinator.dart';

void main() {
  test(
    'local move is durable before POST and exact provider ply confirms one receipt',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      final hold = h.store.blockNext();
      final submitting = h.owner.submitLocalMove(
        uci: 'e2e4',
        localCommandId: 'local-1',
      );
      await _flush();
      expect(h.http.movePaths, isEmpty);
      expect(h.owner.state.inputEnabled, isFalse);
      hold.complete();
      expect(await submitting, LichessLocalMoveSubmissionResult.pending);
      expect(h.store.records.last.pendingLocalMove!.httpAcknowledged, isTrue);
      expect(h.http.movePaths, ['/api/board/game/game123/move/e2e4']);

      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _until(
        () => h.owner.state.record?.confirmedLocalMove?.targetPly == 1,
      );
      final record = h.owner.state.record!;
      expect(record.pendingLocalMove, isNull);
      expect(record.confirmedLocalMove!.localCommandId, 'local-1');
      expect(record.confirmed!.receipt.ply, 1);
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test(
    'ambiguous POST retries once after restart from exact durable pre-state',
    () async {
      final root = await Directory.systemTemp.createTemp('move-restart-');
      final disk = FileLichessPreparedSessionStore(
        Directory('${root.path}/prepared'),
      );
      final archives = Directory('${root.path}/archives');
      final first = _Harness(durableStore: disk, libraryDirectory: archives);
      await _bindAndReconcile(first);
      first.http.nextMoveError = StateError('lost response');
      expect(
        await first.owner.submitLocalMove(
          uci: 'e2e4',
          localCommandId: 'restart-1',
        ),
        LichessLocalMoveSubmissionResult.ambiguous,
      );
      expect(first.owner.state.recovery, LichessPreparedRecovery.moveAmbiguous);
      expect((await disk.load()).record!.pendingLocalMove!.retryUsed, isFalse);
      await first.close();

      final resumed = _Harness(
        durableStore: FileLichessPreparedSessionStore(disk.directory),
        libraryDirectory: archives,
      )..gate.armed = _identity;
      await resumed.owner.restore(const LichessAccountContext('account'));
      await _flush();
      resumed.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full())}\n'));
      await _until(
        () =>
            resumed.owner.state.record?.pendingLocalMove?.retryUsed == true &&
            resumed.owner.state.record?.pendingLocalMove?.httpAcknowledged ==
                true,
      );
      expect(resumed.http.movePaths, ['/api/board/game/game123/move/e2e4']);
      resumed.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await _flush();
      expect(resumed.http.movePaths, hasLength(1));
      resumed.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _until(
        () => resumed.owner.state.record?.confirmedLocalMove != null,
      );
      expect(
        resumed.owner.state.record!.confirmedLocalMove!.localCommandId,
        'restart-1',
      );
      expect(
        (await disk.load()).record!.confirmedLocalMove!.localCommandId,
        'restart-1',
      );
      await resumed.close();
      await root.delete(recursive: true);
    },
  );

  test(
    'duplicate pre-state cannot retry while initial POST is in flight',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      final response = h.http.holdMove();
      final submitting = h.owner.submitLocalMove(
        uci: 'e2e4',
        localCommandId: 'held-1',
      );
      await _until(() => h.http.movePaths.length == 1);

      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await _flush();
      expect(h.http.movePaths, hasLength(1));
      final pending = h.owner.state.record?.pendingLocalMove;
      expect(pending, isNotNull);
      expect(pending!.retryUsed, isFalse);

      response.complete(
        http.StreamedResponse(const Stream<List<int>>.empty(), 200),
      );
      expect(await submitting, LichessLocalMoveSubmissionResult.pending);
      expect(h.http.movePaths, hasLength(1));
    },
  );

  test(
    'duplicate pre-state defers while successful ACK write is held',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      final response = h.http.holdMove();
      final submitting = h.owner.submitLocalMove(
        uci: 'e2e4',
        localCommandId: 'held-ack-1',
      );
      await _until(() => h.http.movePaths.length == 1);
      final ackWrite = h.store.blockNext();
      response.complete(
        http.StreamedResponse(const Stream<List<int>>.empty(), 200),
      );
      await _until(() => h.store.nextWrite == null);

      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await _flush();
      expect(h.owner.state.recovery, isNot(LichessPreparedRecovery.storage));
      expect(h.http.movePaths, hasLength(1));

      ackWrite.complete();
      expect(await submitting, LichessLocalMoveSubmissionResult.pending);
      expect(h.owner.state.recovery, isNull);
      expect(h.owner.state.record!.pendingLocalMove!.httpAcknowledged, isTrue);
      expect(h.http.movePaths, hasLength(1));
    },
  );

  test('unreconciled provider snapshot blocks local move admission', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    final load = h.libraryStore.blockNextLoad();
    addTearDown(() {
      if (!load.release.isCompleted) load.release.complete();
    });
    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_state('e2e4 e7e5'))}\n'));
    await load.started.future;

    expect(
      await h.owner.submitLocalMove(
        uci: 'd2d4',
        localCommandId: 'unreconciled-1',
      ),
      LichessLocalMoveSubmissionResult.notReady,
    );
    expect(h.http.movePaths, isEmpty);
    expect(h.store.records.last.pendingLocalMove, isNull);

    load.release.complete();
    await _until(() => h.owner.state.record?.confirmed?.receipt.ply == 2);
  });

  test(
    'stale pre-state load cannot retry after initial POST acknowledgement',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      final response = h.http.holdMove();
      final submitting = h.owner.submitLocalMove(
        uci: 'e2e4',
        localCommandId: 'stale-load-1',
      );
      await _until(() => h.http.movePaths.length == 1);
      final load = h.libraryStore.blockNextLoad();
      addTearDown(() {
        if (!load.release.isCompleted) load.release.complete();
      });
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await load.started.future;

      response.complete(
        http.StreamedResponse(const Stream<List<int>>.empty(), 200),
      );
      expect(await submitting, LichessLocalMoveSubmissionResult.pending);
      expect(h.owner.state.record!.pendingLocalMove!.httpAcknowledged, isTrue);
      load.release.complete();
      await _flush();

      expect(h.http.movePaths, hasLength(1));
      expect(h.owner.state.record!.pendingLocalMove!.retryUsed, isFalse);
      expect(h.owner.state.record!.pendingLocalMove!.httpAcknowledged, isTrue);
    },
  );

  test('pre-state load rechecks a rejection settled while blocked', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    final response = h.http.holdMove();
    final submitting = h.owner.submitLocalMove(
      uci: 'e2e4',
      localCommandId: 'stale-rejection-1',
    );
    await _until(() => h.http.movePaths.length == 1);
    final load = h.libraryStore.blockNextLoad();
    addTearDown(() {
      if (!load.release.isCompleted) load.release.complete();
    });
    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_state(''))}\n'));
    await load.started.future;
    response.complete(
      http.StreamedResponse(const Stream<List<int>>.empty(), 400),
    );
    expect(await submitting, LichessLocalMoveSubmissionResult.rejected);
    load.release.complete();
    await _flush();
    expect(h.http.movePaths, hasLength(1));
    expect(h.owner.state.record!.pendingLocalMove!.rejected, isTrue);
    expect(h.owner.state.recovery, LichessPreparedRecovery.moveRejected);
  });

  for (final testCase in <(String, String, String, LichessPreparedRecovery)>[
    (
      'different target move',
      'd2d4',
      'started',
      LichessPreparedRecovery.moveConflict,
    ),
    (
      'skipped target ply',
      'd2d4 e2e4',
      'started',
      LichessPreparedRecovery.moveSkipped,
    ),
    ('terminal pre-state', '', 'aborted', LichessPreparedRecovery.moveTerminal),
  ]) {
    test(
      'pending local move keeps typed recovery for ${testCase.$1}',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await _bindAndReconcile(h);
        expect(
          await h.owner.submitLocalMove(
            uci: 'e2e4',
            localCommandId: 'conflict-1',
          ),
          LichessLocalMoveSubmissionResult.pending,
        );
        h.http
            .body(_gameStreamPath)
            .add(
              utf8.encode(
                '${jsonEncode(_state(testCase.$2, status: testCase.$3))}\n',
              ),
            );
        await _until(() => h.owner.state.recovery == testCase.$4);
        expect(h.owner.state.record!.pendingLocalMove, isNotNull);
        expect(h.owner.state.record!.confirmed!.receipt.ply, 0);
        expect(h.owner.state.inputEnabled, isFalse);
      },
    );
  }

  test('definitive rejection is durable and never retried', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    h.http.nextMoveStatus = 400;
    expect(
      await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'rejected-1'),
      LichessLocalMoveSubmissionResult.rejected,
    );
    expect(h.owner.state.record!.pendingLocalMove!.rejected, isTrue);
    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_state(''))}\n'));
    await _flush();
    expect(h.owner.state.recovery, LichessPreparedRecovery.moveRejected);
    expect(h.http.movePaths, hasLength(1));
  });

  test('exact provider move overrides a definitive HTTP rejection', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    h.http.nextMoveStatus = 400;
    expect(
      await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'rejected-2'),
      LichessLocalMoveSubmissionResult.rejected,
    );

    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
    await _until(
      () =>
          h.owner.state.record?.confirmedLocalMove?.localCommandId ==
          'rejected-2',
    );
    expect(h.owner.state.record!.pendingLocalMove, isNull);
    expect(h.owner.state.record!.confirmed!.receipt.ply, 1);
    expect(h.owner.state.recovery, isNull);
  });

  test(
    'confirmed command receipt blocks another command until consumed',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      expect(
        await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'receipt-1'),
        LichessLocalMoveSubmissionResult.pending,
      );
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _until(() => h.owner.state.record?.confirmedLocalMove != null);
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4 e7e5'))}\n'));
      await _until(() => h.owner.state.record?.confirmed?.receipt.ply == 2);

      expect(
        await h.owner.submitLocalMove(uci: 'd2d4', localCommandId: 'receipt-2'),
        LichessLocalMoveSubmissionResult.notReady,
      );
      expect(h.http.movePaths, hasLength(1));
      expect(
        h.owner.state.record!.confirmedLocalMove!.localCommandId,
        'receipt-1',
      );
    },
  );

  test(
    'new provider evidence fences HTTP admission after durable intent',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      final hold = h.store.blockNext();
      final submitting = h.owner.submitLocalMove(
        uci: 'e2e4',
        localCommandId: 'fenced-1',
      );
      await _flush();
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _flush();
      hold.complete();
      expect(await submitting, LichessLocalMoveSubmissionResult.notReady);
      expect(h.http.movePaths, isEmpty);
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test('provider advance while credentials wait prevents stale POST', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    final credentials = Completer<String>();
    h.token = (_) => credentials.future;
    final submitting = h.owner.submitLocalMove(
      uci: 'e2e4',
      localCommandId: 'credential-fence-1',
    );
    await _until(() => h.owner.state.record?.pendingLocalMove != null);
    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
    await _until(() => h.owner.state.record?.confirmedLocalMove != null);
    credentials.complete('fixture-token');
    expect(await submitting, LichessLocalMoveSubmissionResult.ambiguous);
    expect(h.http.movePaths, isEmpty);
  });

  test('HTTP 500 reconciles an authoritative applied move', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    h.http.nextMoveStatus = 500;
    expect(
      await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'server-1'),
      LichessLocalMoveSubmissionResult.ambiguous,
    );
    await _until(
      () =>
          h.http.bodyPaths.where((path) => path == _gameStreamPath).length >= 2,
    );
    h.http
        .body(_gameStreamPath)
        .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
    await _until(() => h.owner.state.record?.confirmedLocalMove != null);
    expect(h.owner.state.record!.pendingLocalMove, isNull);
    expect(h.owner.state.recovery, isNull);
  });

  test('HTTP 500 unchanged pre-state uses only the durable retry', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    h.http.nextMoveStatus = 500;
    expect(
      await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'server-2'),
      LichessLocalMoveSubmissionResult.ambiguous,
    );
    await _until(
      () =>
          h.http.bodyPaths.where((path) => path == _gameStreamPath).length >= 2,
    );
    h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(_full())}\n'));
    await _until(() => h.http.movePaths.length == 2);
    expect(h.owner.state.record!.pendingLocalMove!.retryUsed, isTrue);
    expect(h.owner.state.record!.pendingLocalMove!.httpAcknowledged, isTrue);
  });

  for (final statusCase in <(int, LichessPreparedRecovery)>[
    (429, LichessPreparedRecovery.moveRateLimited),
    (401, LichessPreparedRecovery.moveAuthentication),
    (403, LichessPreparedRecovery.movePermission),
  ]) {
    test(
      'HTTP ${statusCase.$1} preserves intent and exact provider authority',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await _bindAndReconcile(h);
        h.http.nextMoveStatus = statusCase.$1;
        expect(
          await h.owner.submitLocalMove(
            uci: 'e2e4',
            localCommandId: 'held-${statusCase.$1}',
          ),
          LichessLocalMoveSubmissionResult.ambiguous,
        );
        expect(h.owner.state.recovery, statusCase.$2);
        expect(
          h.owner.state.record!.pendingLocalMove!.recoveryHold,
          statusCase.$2,
        );
        expect(h.owner.state.record!.pendingLocalMove!.retryUsed, isFalse);
        h.http
            .body(_gameStreamPath)
            .add(utf8.encode('${jsonEncode(_state(''))}\n'));
        await _flush();
        expect(h.http.movePaths, hasLength(1));
        expect(h.owner.state.recovery, statusCase.$2);
        h.http
            .body(_gameStreamPath)
            .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
        await _until(() => h.owner.state.record?.confirmedLocalMove != null);
        expect(h.owner.state.record!.pendingLocalMove, isNull);
        expect(h.owner.state.recovery, isNull);
      },
    );
  }

  test(
    'unavailable credentials retain an authentication recovery hold',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      h.token = (_) async => throw StateError('credential store unavailable');
      expect(
        await h.owner.submitLocalMove(
          uci: 'e2e4',
          localCommandId: 'no-token-1',
        ),
        LichessLocalMoveSubmissionResult.ambiguous,
      );
      expect(
        h.owner.state.recovery,
        LichessPreparedRecovery.moveAuthentication,
      );
      expect(
        h.owner.state.record!.pendingLocalMove!.recoveryHold,
        LichessPreparedRecovery.moveAuthentication,
      );
      expect(h.http.movePaths, isEmpty);
    },
  );

  test('retirement detaches a retry waiting for credentials', () async {
    final h = _Harness();
    addTearDown(h.close);
    await _bindAndReconcile(h);
    h.http.nextMoveError = StateError('lost response');
    expect(
      await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'retire-1'),
      LichessLocalMoveSubmissionResult.ambiguous,
    );
    await _until(
      () =>
          h.http.bodyPaths.where((path) => path == _gameStreamPath).length >= 2,
    );
    final credentials = Completer<String>();
    h.token = (_) => credentials.future;
    h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(_full())}\n'));
    await _until(
      () => h.owner.state.record?.pendingLocalMove?.retryUsed == true,
    );
    await h.owner.background().timeout(const Duration(seconds: 1));
    expect(h.owner.state.stage, LichessPreparedSessionStage.suspended);
    expect(h.http.movePaths, hasLength(1));
  });

  test(
    'cooldown storage failure is typed and retirement stays drainable',
    () async {
      final cooldown = _Cooldown();
      final h = _Harness(
        coordinator: LichessRequestCoordinator(cooldownStore: cooldown),
      );
      addTearDown(h.close);
      await _bindAndReconcile(h);
      cooldown.failWrites = true;
      h.http.nextMoveStatus = 429;
      expect(
        await h.owner.submitLocalMove(uci: 'e2e4', localCommandId: 'storage-1'),
        LichessLocalMoveSubmissionResult.storageFailure,
      );
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      await h.owner.background().timeout(const Duration(seconds: 1));
      expect(h.owner.state.stage, LichessPreparedSessionStage.suspended);
    },
  );

  test(
    'failed retry acknowledgement keeps storage recovery on unchanged evidence',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindAndReconcile(h);
      h.http.nextMoveError = StateError('lost response');
      expect(
        await h.owner.submitLocalMove(
          uci: 'e2e4',
          localCommandId: 'retry-storage-1',
        ),
        LichessLocalMoveSubmissionResult.ambiguous,
      );
      await _until(
        () =>
            h.http.bodyPaths.where((path) => path == _gameStreamPath).length >=
            2,
      );
      final retryResponse = h.http.holdMove();
      h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(_full())}\n'));
      await _until(
        () => h.owner.state.record?.pendingLocalMove?.retryUsed == true,
      );
      h.store.failNextWrites = 1;
      retryResponse.complete(
        http.StreamedResponse(const Stream<List<int>>.empty(), 200),
      );
      await _until(
        () => h.owner.state.recovery == LichessPreparedRecovery.storage,
      );
      expect(h.owner.state.storageCertain, isFalse);
      expect(h.http.movePaths, hasLength(2));
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await _flush();
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      expect(h.owner.state.storageCertain, isFalse);
      expect(
        h.owner.state.stage,
        isNot(LichessPreparedSessionStage.boundProviderReconciled),
      );
      expect(h.http.movePaths, hasLength(2));
    },
  );

  test(
    'real checkpoint restart replays fresh full and preserves unknown recovery evidence',
    () async {
      final root = await Directory.systemTemp.createTemp('online-restart-');
      final disk = FileLichessPreparedSessionStore(
        Directory('${root.path}/prepared'),
      );
      final archives = Directory('${root.path}/archives');
      final h = _Harness(durableStore: disk, libraryDirectory: archives);
      await _bind(h);
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
      await _until(() => h.owner.state.record?.confirmed?.receipt.ply == 1);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundProviderReconciled,
      );
      expect((await disk.load()).record!.confirmed!.receipt.ply, 1);
      await h.close();
      final next = _Harness(
        durableStore: FileLichessPreparedSessionStore(disk.directory),
        libraryDirectory: archives,
      );
      next.gate.armed = _identity;
      await next.owner.restore(const LichessAccountContext('account'));
      await _flush();
      expect(next.owner.state.providerState, isNull);
      expect(next.owner.providerReceiptMonotonic, isNull);
      expect(next.owner.state.inputEnabled, isFalse);
      next.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4 e7e5'))}\n'));
      await _until(
        () =>
            next.owner.state.record?.confirmed?.receipt.ply == 2 &&
            next.owner.state.providerStreamConnected,
      );
      expect((await next.library.load()).games.single.transitions.length, 2);
      final archiveBytes = jsonEncode(
        (await next.library.load()).games.single.toJson(),
      );
      next.http
          .body(_gameStreamPath)
          .add(
            utf8.encode(
              '${jsonEncode(_state('e2e4 e7e5', status: 'futureResult'))}\n',
            ),
          );
      await _until(
        () =>
            next.owner.state.recovery == LichessPreparedRecovery.providerStatus,
      );
      final loaded = await disk.load();
      expect(loaded.record!.confirmed!.receipt.ply, 2);
      expect(loaded.record!.recoveryObservation!.rawStatus, 'futureResult');
      expect(
        jsonEncode((await next.library.load()).games.single.toJson()),
        archiveBytes,
      );
      await next.close();
      await root.delete(recursive: true);
    },
  );

  test(
    'archive ahead of interrupted checkpoint needs fresh matching full; shorter fresh freezes',
    () async {
      final root = await Directory.systemTemp.createTemp('online-interrupted-');
      final disk = FileLichessPreparedSessionStore(
        Directory('${root.path}/prepared'),
      );
      final fault = _FaultDisk(disk);
      final archives = Directory('${root.path}/archives');
      final h = _Harness(durableStore: fault, libraryDirectory: archives);
      await _bind(h);
      h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(_full())}\n'));
      await _until(() => h.owner.state.record?.confirmed != null);
      fault.failNextConfirmation = true;
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _until(
        () => h.owner.state.recovery == LichessPreparedRecovery.storage,
      );
      expect((await disk.load()).record!.confirmed!.receipt.ply, 0);
      expect((await h.library.load()).games.single.currentPly, 1);
      await h.close();
      final short = _Harness(durableStore: disk, libraryDirectory: archives)
        ..gate.armed = _identity;
      await short.owner.restore(const LichessAccountContext('account'));
      await _flush();
      short.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full())}\n'));
      await _until(
        () =>
            short.owner.state.recovery ==
            LichessPreparedRecovery.providerPosition,
      );
      expect((await disk.load()).record!.confirmed!.receipt.ply, 0);
      expect((await short.library.load()).games.single.currentPly, 1);
      await short.close();
      final resumed = _Harness(durableStore: disk, libraryDirectory: archives)
        ..gate.armed = _identity;
      await resumed.owner.restore(const LichessAccountContext('account'));
      await _flush();
      resumed.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4 e7e5'))}\n'));
      await _until(
        () =>
            resumed.owner.state.record?.confirmed?.receipt.ply == 2 &&
            resumed.owner.state.providerStreamConnected,
      );
      expect((await resumed.library.load()).games.single.transitions.length, 2);
      await resumed.close();
      await root.delete(recursive: true);
    },
  );

  test(
    'retirement drains actual checkpoint write before switching account authority',
    () async {
      final root = await Directory.systemTemp.createTemp('online-retire-');
      final disk = _FaultDisk(
        FileLichessPreparedSessionStore(Directory('${root.path}/prepared')),
      );
      final h = _Harness(
        durableStore: disk,
        libraryDirectory: Directory('${root.path}/archives'),
      );
      await _bind(h);
      final hold = disk.holdConfirmation();
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
      await disk.entered.future;
      var restored = false;
      final restoring = h.owner
          .restore(const LichessAccountContext('different-account'))
          .then((_) {
            restored = true;
          });
      await _flush();
      expect(restored, isFalse);
      expect(h.factoryAccounts, ['account']);
      hold.complete();
      await restoring;
      expect(h.owner.state.recovery, LichessPreparedRecovery.accountMismatch);
      expect(h.owner.state.providerStreamConnected, isFalse);
      expect(h.owner.state.inputEnabled, isFalse);
      expect((await disk.load()).record!.accountId, 'account');
      await h.close();
      await root.delete(recursive: true);
    },
  );

  test(
    'illegal cumulative suffix cannot replace durable confirmed archive',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
      await _until(() => h.owner.state.record?.confirmed?.receipt.ply == 1);
      final before = jsonEncode((await h.library.load()).games.single.toJson());
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('e2e4 e7e4'))}\n'));
      await _until(
        () =>
            h.owner.state.recovery == LichessPreparedRecovery.providerPosition,
      );
      expect(
        jsonEncode((await h.library.load()).games.single.toJson()),
        before,
      );
      expect(h.owner.state.record!.confirmed!.receipt.ply, 1);
    },
  );

  for (final accountLoss in [false, true]) {
    test(
      'loss during real checkpoint write cannot publish healthy account=$accountLoss',
      () async {
        final root = await Directory.systemTemp.createTemp('online-loss-');
        final disk = _FaultDisk(
          FileLichessPreparedSessionStore(Directory('${root.path}/prepared')),
        );
        final h = _Harness(
          durableStore: disk,
          libraryDirectory: Directory('${root.path}/archives'),
        );
        await _bind(h);
        final hold = disk.holdConfirmation();
        h.http
            .body(_gameStreamPath)
            .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
        await disk.entered.future;
        await h.http
            .body(accountLoss ? '/api/stream/event' : _gameStreamPath)
            .close();
        await _flush();
        hold.complete();
        await _flush();
        expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
        expect(h.owner.state.providerStreamConnected, isFalse);
        expect(h.owner.state.inputEnabled, isFalse);
        expect(h.owner.state.recovery, LichessPreparedRecovery.transport);
        await h.close();
        await root.delete(recursive: true);
      },
    );
  }

  for (final terminal in [false, true]) {
    test(
      'account EOF rejects late game callbacks terminal=$terminal',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        h.http.ignoreGameAbort = true;
        await _bind(h);
        final body = h.http.body(_gameStreamPath);
        final initial = _full();
        if (terminal) {
          initial['state'] = _state('', status: 'mate', winner: 'white');
        }
        body.add(utf8.encode('${jsonEncode(initial)}\n'));
        await _flush();
        final prior = h.owner.state.providerState;
        await h.http.body('/api/stream/event').close();
        await _flush();
        body.add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
        await _flush();
        await body.close();
        await _flush();
        expect(h.owner.state.providerState, same(prior));
        expect(h.owner.state.providerStreamConnected, isFalse);
        expect(h.owner.state.accountReady, isFalse);
        expect(h.owner.state.recovery, LichessPreparedRecovery.transport);
        expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      },
    );
  }

  test(
    'state before full cannot publish an unvalidated provider identity',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      final wrong = _full()..['id'] = 'other123';
      h.http
          .body(_gameStreamPath)
          .add(
            utf8.encode(
              '${jsonEncode(_state('e2e4'))}\n${jsonEncode(wrong)}\n',
            ),
          );
      await _flush();
      expect(h.owner.state.providerState, isNull);
      expect(h.owner.state.providerStreamConnected, isFalse);
      expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test(
    'bound provider stream publishes decoded state without enabling board input',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      expect(h.http.paths, contains(_gameStreamPath));
      expect(h.http.body('/api/stream/event').isClosed, isFalse);
      expect(h.http.body('/api/board/seek').isClosed, isTrue);
      expect(h.owner.state.providerState, isNull);
      expect(h.owner.state.providerStreamConnected, isFalse);
      final writes = h.store.records.length;
      final wire = utf8.encode('${jsonEncode(_full())}\n');
      final body = h.http.body(_gameStreamPath);
      body.add(wire.sublist(0, 19));
      await _flush();
      expect(h.owner.state.providerState, isNull);
      body.add(wire.sublist(19));
      await _ownerState(
        h,
        (state) =>
            state.providerStreamConnected &&
            state.record?.confirmed?.receipt.ply == 0,
      );
      expect(h.owner.state.providerState!.moves, isEmpty);
      expect(h.owner.state.providerStreamConnected, isTrue);
      body.add(
        utf8.encode(
          '${jsonEncode(_state('e2e4'))}\n${jsonEncode(_state('e2e4', whiteTime: 590000))}\n',
        ),
      );
      await _until(
        () =>
            h.owner.state.providerState?.moves.join(' ') == 'e2e4' &&
            h.owner.state.providerState?.whiteTimeMs == 590000,
      );
      expect(h.owner.state.providerState!.moves, ['e2e4']);
      expect(h.owner.state.providerState!.whiteTimeMs, 590000);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundProviderReconciled,
      );
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.store.records.length, greaterThan(writes));
      expect(h.gate.disarms, isEmpty);
      expect(h.awake.releases, 0);
      expect(
        h.http.paths.where((path) => path == _gameStreamPath),
        hasLength(1),
      );
    },
  );

  for (final invalid in [
    'game',
    'account',
    'side',
    'variant',
    'rated',
    'speed',
    'setup',
  ]) {
    test(
      'bound provider stream rejects conflicting $invalid context',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await _bind(h);
        final full = _full();
        switch (invalid) {
          case 'game':
            full['id'] = 'other123';
          case 'account':
            full['white'] = {'id': 'other', 'name': 'Other'};
          case 'side':
            full['black'] = full['white'];
            full['white'] = {'id': 'opponent', 'name': 'Opponent'};
          case 'variant':
            full['variant'] = {'key': 'atomic', 'name': 'Atomic'};
          case 'rated':
            full['rated'] = true;
          case 'speed':
            full['speed'] = 'blitz';
          case 'setup':
            full['initialFen'] = 'other position';
        }
        h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(full)}\n'));
        await _flush();
        expect(h.owner.state.providerState, isNull);
        expect(h.owner.state.providerStreamConnected, isFalse);
        expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
        expect(
          h.owner.state.recovery,
          ['game', 'account', 'side'].contains(invalid)
              ? LichessPreparedRecovery.providerIdentity
              : LichessPreparedRecovery.incompatibleGame,
        );
        expect(h.owner.state.inputEnabled, isFalse);
        expect(h.gate.disarms, isEmpty);
      },
    );
  }

  for (final nextMoves in ['e2e4', 'd2d4 d7d5']) {
    test(
      'unproven provider line $nextMoves retains previous evidence on resume',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await _bind(h);
        h.http
            .body(_gameStreamPath)
            .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4 e7e5'))}\n'));
        await _ownerState(
          h,
          (state) =>
              state.providerStreamConnected &&
              state.record?.confirmed?.receipt.ply == 2,
        );
        final original = h.owner.state.providerState;
        await h.owner.background();
        expect(h.owner.state.providerState, same(original));
        expect(h.owner.state.providerStreamConnected, isFalse);
        await h.owner.resume();
        await _flush();
        h.http
            .body(_gameStreamPath)
            .add(utf8.encode('${jsonEncode(_full(moves: nextMoves))}\n'));
        await _ownerState(
          h,
          (state) => state.recovery == LichessPreparedRecovery.providerPosition,
        );
        expect(
          h.owner.state.recovery,
          LichessPreparedRecovery.providerPosition,
        );
        expect(h.owner.state.providerState, same(original));
        expect(h.owner.state.providerState!.moves, ['e2e4', 'e7e5']);
        expect(h.owner.state.inputEnabled, isFalse);
        expect(h.gate.disarms, isEmpty);
        expect(
          h.store.records.where(
            (record) =>
                record.providerGameId != null && record.confirmed == null,
          ),
          hasLength(1),
        );
        await h.owner.refreshCandidates();
        expect(
          h.owner.state.recovery,
          LichessPreparedRecovery.providerPosition,
        );
      },
    );
  }

  test(
    'uncorrelated takeback in the current stream remains explicit recovery',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      final body = h.http.body(_gameStreamPath);
      body.add(utf8.encode('${jsonEncode(_full(moves: 'e2e4 e7e5'))}\n'));
      await _flush();
      final original = h.owner.state.providerState;
      body.add(utf8.encode('${jsonEncode(_state('e2e4'))}\n'));
      await _flush();
      expect(h.owner.state.providerState, same(original));
      expect(h.owner.state.recovery, LichessPreparedRecovery.providerPosition);
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.gate.disarms, isEmpty);
      expect(
        h.http.paths.where((path) => path.contains('/takeback/')),
        isEmpty,
      );
    },
  );

  test(
    'bound provider reconnect catches up once without reconfirming or submitting',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
      await _ownerState(
        h,
        (state) =>
            state.providerStreamConnected &&
            state.record?.confirmed?.receipt.ply == 1,
      );
      await h.http.body(_gameStreamPath).close();
      await _ownerState(
        h,
        (state) => state.recovery == LichessPreparedRecovery.transport,
      );
      expect(h.owner.state.recovery, LichessPreparedRecovery.transport);
      expect(h.owner.state.providerState!.moves, ['e2e4']);
      expect(h.owner.state.providerStreamConnected, isFalse);
      await h.owner.resume();
      await _flush();
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4 e7e5'))}\n'));
      await _ownerState(
        h,
        (state) =>
            state.providerStreamConnected &&
            state.record?.confirmed?.receipt.ply == 2,
      );
      expect(h.owner.state.providerState!.moves, ['e2e4', 'e7e5']);
      expect(h.owner.state.providerStreamConnected, isTrue);
      expect(h.owner.state.recovery, isNull);
      expect(h.owner.state.inputEnabled, isFalse);
      expect(
        h.store.records.where(
          (record) => record.providerGameId != null && record.confirmed == null,
        ),
        hasLength(1),
      );
      expect(h.http.paths.where((path) => path.contains('/move/')), isEmpty);
      expect(h.gate.arms, 1);
    },
  );

  test(
    'known terminal close retains result and gate; unknown status requests recovery',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      h.http
          .body(_gameStreamPath)
          .add(
            utf8.encode(
              '${jsonEncode(_full())}\n${jsonEncode(_state('e2e4', status: 'mate', winner: 'white'))}\n',
            ),
          );
      await _ownerState(
        h,
        (state) =>
            state.providerStreamConnected &&
            state.record?.confirmed?.receipt.rawStatus == 'mate',
      );
      await h.http.body(_gameStreamPath).close();
      await _ownerState(h, (state) => !state.providerStreamConnected);
      expect(h.owner.state.providerState!.status.wireValue, 'mate');
      expect(h.owner.state.recovery, isNull);
      expect(h.owner.state.providerStreamConnected, isFalse);
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.gate.disarms, isEmpty);
      await h.owner.resume();
      await _flush();
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_full(moves: 'e2e4'))}\n'));
      await _ownerState(
        h,
        (state) => state.recovery == LichessPreparedRecovery.providerPosition,
      );
      expect(h.owner.state.recovery, LichessPreparedRecovery.providerPosition);
      expect(h.owner.state.providerState!.status.wireValue, 'mate');
    },
  );

  test(
    'unknown provider status is retained without inventing a terminal result',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bind(h);
      h.http
          .body(_gameStreamPath)
          .add(
            utf8.encode(
              '${jsonEncode(_full())}\n${jsonEncode(_state('', status: 'futureStatus'))}\n',
            ),
          );
      await _flush();
      expect(h.owner.state.providerState!.status.wireValue, 'futureStatus');
      expect(h.owner.state.providerState!.winner, isNull);
      expect(h.owner.state.recovery, LichessPreparedRecovery.providerStatus);
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.gate.disarms, isEmpty);
    },
  );

  test(
    'account loss during inventory refresh retains transport recovery until resume',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.http.holdInventory();
      final refreshing = h.owner.refreshCandidates();
      await _flush();
      expect(h.owner.state.isRefreshingInventory, isTrue);
      expect(h.http.paths.last, '/api/account/playing');
      await h.http.body('/api/stream/event').close();
      await refreshing;
      expect(h.owner.state.accountReady, isFalse);
      expect(h.owner.state.recovery, LichessPreparedRecovery.transport);
      await h.owner.resume();
      expect(h.owner.state.accountReady, isTrue);
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      expect(h.owner.state.record!.seekMayHaveStarted, isTrue);
      expect(h.owner.state.record!.providerGameId, isNull);
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.gate.disarms, isEmpty);
      expect(
        h.http.paths.where((path) => path == '/api/board/seek'),
        hasLength(1),
      );
    },
  );

  test(
    'unacknowledged reset retains the record until an explicit fresh check succeeds',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      h.http.games = <Object?>[_game()];
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.http.games = <Object?>[];
      await h.owner.refreshCandidates();
      h.gate.confirmDisarm = false;
      await h.owner.cancel();
      expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
      expect(h.owner.state.gate, LichessPreparedGateState.unknown);
      expect(h.gate.armed, isNull);
      expect(h.owner.state.record!.released, isFalse);
      expect(h.store.records, hasLength(1));
      await h.owner.resume();
      h.gate.confirmDisarm = true;
      await h.owner.cancel();
      expect(h.owner.state.stage, LichessPreparedSessionStage.idle);
      expect(h.store.records.last.released, isTrue);
      expect(h.gate.disarms, <LichessPreparedIdentity>[_identity, _identity]);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
    },
  );

  for (final inventoryKind in <String>['nonzero', 'capped', 'malformed']) {
    test('$inventoryKind inventory prevents even a pre-seek reset', () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      h.gate.ready = false;
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      if (inventoryKind == 'malformed') {
        h.http.holdInventory().complete(
          http.StreamedResponse(
            Stream<List<int>>.value(utf8.encode('{}')),
            200,
          ),
        );
      } else {
        h.http.games = <Object?>[
          for (int i = 0; i < (inventoryKind == 'capped' ? 50 : 1); i++)
            _game(id: 'game$i'),
        ];
      }
      await h.owner.resume();
      await h.owner.cancel();
      expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      expect(h.owner.state.record!.released, isFalse);
      expect(h.owner.state.record!.seekMayHaveStarted, isFalse);
      expect(h.store.records, hasLength(1));
      expect(h.gate.disarms, isEmpty);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
    });
  }

  test(
    'pending local move keeps typed recovery for provider regression',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindBlackAndReconcile(h);
      expect(
        await h.owner.submitLocalMove(
          uci: 'e7e5',
          localCommandId: 'regression-1',
        ),
        LichessLocalMoveSubmissionResult.pending,
      );
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state(''))}\n'));
      await _until(
        () => h.owner.state.recovery == LichessPreparedRecovery.moveRegression,
      );
      expect(h.owner.state.record!.pendingLocalMove, isNotNull);
      expect(h.owner.state.record!.confirmed!.receipt.ply, 1);
    },
  );

  test(
    'recovery hold cannot accept a divergent same-ply provider line',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await _bindBlackAndReconcile(h);
      h.http.nextMoveStatus = 429;
      expect(
        await h.owner.submitLocalMove(uci: 'e7e5', localCommandId: 'diverge-1'),
        LichessLocalMoveSubmissionResult.ambiguous,
      );
      h.http
          .body(_gameStreamPath)
          .add(utf8.encode('${jsonEncode(_state('d2d4'))}\n'));
      await _until(
        () =>
            h.owner.state.recovery == LichessPreparedRecovery.providerPosition,
      );
      expect(h.owner.state.record!.confirmed!.receipt.ply, 1);
      expect(h.owner.state.record!.pendingLocalMove, isNotNull);
    },
  );

  test(
    'lost reset write acknowledgement cannot turn attempted release into durable idle',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      h.gate.ready = false;
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      await h.owner.resume();
      final write = h.store.blockNext();
      final cancelling = h.owner.cancel();
      await _flush();
      expect(h.gate.disarms, <LichessPreparedIdentity>[_identity]);
      write.completeError(StateError('Fixture release acknowledgement lost'));
      await cancelling;
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      expect(h.owner.state.storageCertain, isFalse);
      expect(h.owner.state.record!.identity, _identity);
      expect(h.store.records.last.released, isFalse);
      await h.owner.resume();
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      expect(h.owner.state.storageCertain, isFalse);
      expect(h.gate.disarms, hasLength(1));
      expect(h.http.paths, isNot(contains('/api/board/seek')));
    },
  );

  for (final changedBoard in <bool>[false, true]) {
    test(
      'reset cannot erase a different arm or stable board ($changedBoard) after readback',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await h.owner.restore(const LichessAccountContext('account'));
        h.gate.ready = false;
        await h.owner.prepare(boardContext: _identity, seek: _seek);
        await h.owner.resume();
        expect(h.owner.state.gate, LichessPreparedGateState.unarmed);
        if (changedBoard) {
          h.gate.boardIdentity = 'different-board';
        } else {
          h.gate.armed = _otherIdentity;
        }
        await h.owner.cancel();
        expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
        expect(h.owner.state.record!.released, isFalse);
        expect(h.store.records, hasLength(1));
        expect(h.gate.armed, changedBoard ? isNull : _otherIdentity);
      },
    );
  }

  test(
    'rejected setup explicitly resets a proven pre-seek preparation before trying again',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      h.gate.ready = false;
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
      await h.owner.resume();
      expect(h.owner.state.accountReady, isTrue);
      expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
      expect(h.owner.state.gate, LichessPreparedGateState.unarmed);
      expect(h.owner.state.record!.released, isFalse);
      expect(h.gate.arms, 1);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      await h.owner.cancel();
      expect(h.owner.state.stage, LichessPreparedSessionStage.idle);
      expect(h.store.records.last.released, isTrue);
      expect(h.gate.disarms, <LichessPreparedIdentity>[_identity]);
      await h.owner.restore(const LichessAccountContext('account'));
      expect(h.owner.state.stage, LichessPreparedSessionStage.idle);
      h.gate.ready = true;
      const nextIdentity = LichessPreparedIdentity(
        sessionId: 'prepared-2',
        boardIdentity: 'board',
        boardGameId: '000102030405060708090a0b0c0d0e0f',
        lineRevision: 7,
      );
      await h.owner.prepare(boardContext: nextIdentity, seek: _seek);
      await _flush();
      expect(h.owner.state.stage, LichessPreparedSessionStage.matching);
      expect(h.owner.state.gateIdentity, nextIdentity);
      expect(h.store.records.map((record) => record.revision), <int>[
        1,
        2,
        3,
        4,
      ]);
      expect(
        h.http.paths.where((path) => path == '/api/board/seek'),
        hasLength(1),
      );
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test(
    'explicit resume recovers after a failed keep-awake release succeeds',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.awake.releaseFailures = 1;
      await h.owner.background();
      expect(h.awake.releases, 1);
      await h.owner.resume();
      expect(h.awake.releases, 2);
      expect(h.awake.acquires, 2);
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      expect(h.owner.state.accountReady, isTrue);
      expect(h.owner.state.record!.identity, _identity);
      expect(h.gate.disarms, isEmpty);
    },
  );

  test(
    'unrelated gameStart only refreshes choices and explicit selection binds one side',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.http.games = <Object?>[
        <String, Object?>{..._game(id: 'chosen12'), 'color': 'black'},
        _game(id: 'second12'),
        <String, Object?>{..._game(id: 'rated123'), 'rated': true},
      ];
      h.http
          .body('/api/stream/event')
          .add(
            utf8.encode(
              '\n{"type":"gameStart","game":{"gameId":"other123","fullId":"other123abcd"}}\n',
            ),
          );
      await _flush();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.awaitingGameConfirmation,
      );
      expect(h.owner.state.candidates.map((game) => game.gameId), <String>[
        'chosen12',
        'second12',
      ]);
      expect(h.owner.state.inventory!.games, hasLength(3));
      expect(h.owner.state.record!.providerGameId, isNull);
      expect(() => h.owner.state.candidates.clear(), throwsUnsupportedError);
      final revision = h.owner.state.evidenceRevision;
      await h.owner.confirmGame('chosen12', revision);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
      expect(h.owner.state.record!.providerGameId, 'chosen12');
      expect(h.owner.state.record!.providerSide!.name, 'black');
      await h.owner.confirmGame('second12', revision);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
      expect(
        h.store.records.where(
          (record) => record.providerGameId != null && record.confirmed == null,
        ),
        hasLength(1),
      );
      expect(h.owner.state.inputEnabled, isFalse);
      expect(
        h.http.paths.every(
          (path) => <String>{
            '/api/stream/event',
            '/api/account/playing',
            '/api/board/seek',
          }.contains(path),
        ),
        isTrue,
      );
    },
  );

  test(
    'account stream loss keeps transport recovery even when closing the seek',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      await h.http.body('/api/stream/event').close();
      await _flush();
      expect(h.owner.state.accountReady, isFalse);
      expect(h.owner.state.recovery, LichessPreparedRecovery.transport);
      expect(h.owner.state.record!.seekMayHaveStarted, isTrue);
      expect(h.owner.state.gateIdentity, _identity);
      expect(h.gate.disarms, isEmpty);
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test('replacement clients share the same provider cooldown', () async {
    Duration elapsed = Duration.zero;
    final delays = <Duration>[];
    final h = _Harness(
      coordinator: LichessRequestCoordinator(
        cooldownStore: _Cooldown(),
        monotonicNow: () => elapsed,
        wallNow: () => DateTime.utc(2026, 9, 5).add(elapsed),
        delay: (duration) async {
          delays.add(duration);
          elapsed += duration;
        },
      ),
    );
    addTearDown(h.close);
    final response = h.http.holdAccount();
    final first = h.owner.restore(const LichessAccountContext('first'));
    response.complete(
      http.StreamedResponse(
        const Stream<List<int>>.empty(),
        429,
        headers: <String, String>{'retry-after': '75'},
      ),
    );
    await first;
    expect(h.owner.state.accountReady, isFalse);
    await h.owner.replaceAccount(const LichessAccountContext('second'));
    expect(h.owner.state.accountReady, isTrue);
    expect(delays, <Duration>[const Duration(seconds: 75)]);
    expect(h.factoryAccounts, <String>['first', 'second']);
  });

  test(
    'replacement aborts a held account handshake and ignores its late HTTP 200',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      final response = h.http.holdAccount();
      final first = h.owner.restore(const LichessAccountContext('first'));
      await _flush();
      expect(h.owner.state.accountReady, isFalse);
      expect(h.http.paths, <String>['/api/stream/event']);
      await h.owner.replaceAccount(const LichessAccountContext('second'));
      await first;
      final revision = h.owner.state.evidenceRevision;
      response.complete(_inventoryResponse(<Object?>[]));
      await _flush();
      expect(h.owner.state.accountReady, isTrue);
      expect(h.owner.state.evidenceRevision, revision);
      expect(
        h.http.paths.where((path) => path == '/api/account/playing'),
        hasLength(1),
      );
      expect(h.factoryAccounts, <String>['first', 'second']);
    },
  );

  test(
    'background during a binding write waits for durability and suppresses late success',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      h.http.games = <Object?>[_game()];
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      final write = h.store.blockNext();
      final binding = h.owner.confirmGame(
        'game123',
        h.owner.state.evidenceRevision,
      );
      await _flush();
      final suspending = h.owner.background();
      await _flush();
      expect(h.owner.state.stage, LichessPreparedSessionStage.retiring);
      expect(h.awake.releases, 0);
      write.complete();
      await binding;
      await suspending;
      expect(h.owner.state.stage, LichessPreparedSessionStage.suspended);
      expect(h.owner.state.storageCertain, isTrue);
      expect(h.owner.state.record!.providerGameId, 'game123');
      expect(h.awake.releases, 1);
      await h.owner.resume();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
      expect(h.store.records, hasLength(2));
      expect(h.gate.arms, 1);
    },
  );

  test(
    'lost binding acknowledgement preserves attempted evidence across refresh and restore',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      h.http.games = <Object?>[_game()];
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      final write = h.store.blockNext();
      final binding = h.owner.confirmGame(
        'game123',
        h.owner.state.evidenceRevision,
      );
      await _flush();
      write.completeError(StateError('Fixture durable acknowledgement lost'));
      await binding;
      await h.owner.refreshCandidates();
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      expect(h.owner.state.storageCertain, isFalse);
      expect(h.owner.state.record!.providerGameId, 'game123');
      expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      await h.owner.confirmGame('game123', h.owner.state.evidenceRevision);
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      await h.owner.background();
      await h.owner.resume();
      expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
      expect(h.owner.state.record!.providerGameId, 'game123');
      expect(h.gate.disarms, isEmpty);
      expect(h.store.records, hasLength(1));
    },
  );

  test(
    'retiring a pending arm blocks replacement until its effect is known',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      final arm = h.gate.blockArm();
      final preparing = h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      final replacing = h.owner.replaceAccount(
        const LichessAccountContext('other'),
      );
      await _flush();
      expect(h.factoryAccounts, <String>['account']);
      expect(h.owner.state.stage, LichessPreparedSessionStage.retiring);
      expect(h.awake.releases, 0);
      arm.complete(true);
      await preparing;
      await replacing;
      expect(h.owner.state.recovery, LichessPreparedRecovery.accountMismatch);
      expect(h.owner.state.gateIdentity, _identity);
      expect(h.owner.state.record!.accountId, 'account');
      expect(h.factoryAccounts, <String>['account']);
      expect(h.awake.releases, 1);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      expect(h.gate.disarms, isEmpty);
    },
  );

  test(
    'post-seek cancel and background preserve the arm without retry or repeated setup',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      await h.owner.cancel();
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      expect(h.owner.state.record!.seekMayHaveStarted, isTrue);
      expect(h.owner.state.gateIdentity, _identity);
      expect(h.gate.disarms, isEmpty);
      expect(h.awake.releases, 1);
      await h.owner.resume();
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      expect(h.awake.acquires, 2);
      await h.owner.background();
      expect(h.owner.state.stage, LichessPreparedSessionStage.suspended);
      expect(h.owner.state.record!.seekMayHaveStarted, isTrue);
      expect(h.owner.state.gateIdentity, _identity);
      expect(h.awake.releases, 2);
      h.http.games = <Object?>[_game()];
      await h.owner.resume();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.awaitingGameConfirmation,
      );
      expect(h.gate.arms, 1);
      expect(h.gate.disarms, isEmpty);
      expect(
        h.http.paths.where((path) => path == '/api/board/seek'),
        hasLength(1),
      );
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );

  test(
    'inventory failure during arm blocks seek admission without losing the arm',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      final arm = h.gate.blockArm();
      final preparing = h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      final response = h.http.holdInventory();
      final refreshing = h.owner.refreshCandidates();
      arm.complete(true);
      await _flush();
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      response.complete(
        http.StreamedResponse(Stream<List<int>>.value(utf8.encode('{}')), 200),
      );
      await refreshing;
      await preparing;
      expect(h.owner.state.recovery, LichessPreparedRecovery.inventory);
      expect(h.owner.state.gate, LichessPreparedGateState.armed);
      expect(h.owner.state.record!.seekMayHaveStarted, isFalse);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
    },
  );

  for (final kind in <LichessPreparedLoadKind>[
    LichessPreparedLoadKind.recovered,
    LichessPreparedLoadKind.corrupt,
    LichessPreparedLoadKind.unavailable,
  ]) {
    test(
      '$kind storage never becomes a safe empty or pre-seek cancellation',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        h.store.loadResult = LichessPreparedLoad(
          kind,
          record: LichessPreparedRecord(
            identity: _identity,
            accountId: 'account',
            seek: _seek,
            revision: 1,
            seekMayHaveStarted: false,
          ),
        );
        await h.owner.restore(const LichessAccountContext('account'));
        expect(h.owner.state.storageCertain, isFalse);
        expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
        await h.owner.cancel();
        expect(h.owner.state.recovery, LichessPreparedRecovery.storage);
        expect(h.gate.arms, 0);
        expect(h.gate.disarms, isEmpty);
        expect(h.http.paths, isEmpty);
        expect(h.store.records, isEmpty);
      },
    );
  }

  test(
    'capped and malformed inventories never authorize binding or erase evidence',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.http.games = <Object?>[
        for (int i = 0; i < 50; i++) _game(id: 'game$i'),
      ];
      await h.owner.refreshCandidates();
      expect(h.owner.state.inventory!.games, hasLength(50));
      expect(h.owner.state.inventory!.isUncertain, isTrue);
      await h.owner.confirmGame('game0', h.owner.state.evidenceRevision);
      expect(h.store.records.last.providerGameId, isNull);
      final held = h.http.holdInventory();
      final refreshing = h.owner.refreshCandidates();
      held.complete(
        http.StreamedResponse(
          Stream<List<int>>.value(
            utf8.encode('{"nowPlaying":[{}],"nbMyTurn":0}'),
          ),
          200,
        ),
      );
      await refreshing;
      expect(h.owner.state.recovery, LichessPreparedRecovery.inventory);
      expect(h.owner.state.inventory, isNull);
      expect(h.owner.state.record!.seekMayHaveStarted, isTrue);
      expect(h.gate.disarms, isEmpty);
    },
  );

  for (final conflict in <bool>[false, true]) {
    test(
      'unsupported setup/conflicting arm ($conflict) remains recoverable after refresh',
      () async {
        final h = _Harness();
        addTearDown(h.close);
        await h.owner.restore(const LichessAccountContext('account'));
        if (conflict) {
          h.gate.armed = _otherIdentity;
        } else {
          h.gate.ready = false;
        }
        await h.owner.prepare(boardContext: _identity, seek: _seek);
        expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
        h.http.games = <Object?>[_game()];
        await h.owner.refreshCandidates();
        expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
        expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
        expect(h.http.paths, isNot(contains('/api/board/seek')));
        expect(h.owner.state.record!.identity, _identity);
        expect(h.gate.armed, conflict ? _otherIdentity : isNull);
      },
    );
  }

  test(
    'account replacement closes unresolved credentials before a new client starts',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      final credentials = Completer<String>();
      h.token = (account) => account.accountId == 'first'
          ? credentials.future
          : Future<String>.value('second-fixture-token');
      final first = h.owner.restore(const LichessAccountContext('first'));
      await _flush();
      expect(h.http.paths, isEmpty);
      await h.owner.replaceAccount(const LichessAccountContext('second'));
      await first;
      expect(h.factoryAccounts, <String>['first', 'second']);
      expect(h.owner.state.accountReady, isTrue);
      final starts = h.http.paths.length;
      credentials.complete('late-fixture-token');
      await _flush();
      expect(h.http.paths, hasLength(starts));
      expect(h.owner.state.stage, LichessPreparedSessionStage.idle);
    },
  );

  test(
    'closed seek stays uncertain after empty inventory but can discover a game',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      await h.http.body('/api/board/seek').close();
      await _flush();
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      await h.owner.refreshCandidates();
      expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      expect(h.owner.state.recovery, LichessPreparedRecovery.matchingStopped);
      h.http.games = <Object?>[_game()];
      await h.owner.refreshCandidates();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.awaitingGameConfirmation,
      );
      expect(h.store.records.last.providerGameId, isNull);
      expect(h.gate.disarms, isEmpty);
      expect(
        h.http.paths.where((path) => path == '/api/board/seek'),
        hasLength(1),
      );
    },
  );

  test(
    'idle authorization has no keep-awake lease and lease failure blocks matching',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      expect(h.awake.acquires, 0);
      h.awake.allow = false;
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      expect(h.owner.state.recovery, LichessPreparedRecovery.keepAwake);
      expect(h.gate.arms, 0);
      expect(h.store.records, isEmpty);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
    },
  );

  test(
    'refresh invalidates the old choice before its response arrives',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      h.http.games = <Object?>[_game()];
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.awaitingGameConfirmation,
      );
      final revision = h.owner.state.evidenceRevision;
      final response = h.http.holdInventory();
      final refreshing = h.owner.refreshCandidates();
      await _flush();
      expect(h.owner.state.evidenceRevision, greaterThan(revision));
      expect(h.owner.state.isRefreshingInventory, isTrue);
      await h.owner.confirmGame('game123', revision);
      expect(h.store.records.last.providerGameId, isNull);
      response.complete(_inventoryResponse(<Object?>[_game(id: 'other123')]));
      await refreshing;
      expect(h.owner.state.candidates.single.gameId, 'other123');
      expect(h.owner.state.isRefreshingInventory, isFalse);
    },
  );

  test(
    'bound resume does not repeat confirmation or ignore a changed gate',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      h.http.games = <Object?>[_game()];
      await h.owner.refreshCandidates();
      await h.owner.confirmGame('game123', h.owner.state.evidenceRevision);
      await h.owner.background();
      await h.owner.resume();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
      expect(
        h.store.records.where(
          (record) => record.providerGameId != null && record.confirmed == null,
        ),
        hasLength(1),
      );
      await h.owner.background();
      h.gate.armed = const LichessPreparedIdentity(
        sessionId: 'other',
        boardIdentity: 'board',
        boardGameId: '000102030405060708090a0b0c0d0e0f',
        lineRevision: 7,
      );
      await h.owner.resume();
      expect(h.owner.state.stage, LichessPreparedSessionStage.uncertain);
      expect(h.owner.state.recovery, LichessPreparedRecovery.gate);
      expect(h.owner.state.record!.providerGameId, 'game123');
      expect(h.gate.disarms, isEmpty);
    },
  );

  test(
    'pre-seek cancellation waits for late arm then exactly disarms',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      final arm = h.gate.blockArm();
      final preparing = h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      final cancelling = h.owner.cancel();
      await _flush();
      expect(h.owner.state.stage, LichessPreparedSessionStage.retiring);
      expect(h.gate.disarms, isEmpty);
      arm.complete(true);
      await preparing;
      await cancelling;
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      expect(h.gate.disarms, <LichessPreparedIdentity>[_identity]);
      expect(h.owner.state.stage, LichessPreparedSessionStage.idle);
      expect(h.store.records.last.released, isTrue);
      expect(h.awake.releases, 1);
    },
  );

  test(
    'durable barriers prevent arm, seek, and premature bound publication',
    () async {
      final h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      final preparedWrite = h.store.blockNext();
      final preparing = h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      expect(h.gate.arms, 0);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      final seekWrite = h.store.blockNext();
      preparedWrite.complete();
      await _flush();
      expect(h.gate.arms, 1);
      expect(h.http.paths, isNot(contains('/api/board/seek')));
      seekWrite.complete();
      await preparing;
      h.http.games = <Object?>[_game()];
      await h.owner.refreshCandidates();
      final boundWrite = h.store.blockNext();
      final binding = h.owner.confirmGame(
        'game123',
        h.owner.state.evidenceRevision,
      );
      await _flush();
      await h.owner.refreshCandidates();
      expect(h.owner.state.stage, LichessPreparedSessionStage.binding);
      expect(h.owner.state.storageCertain, isFalse);
      boundWrite.complete();
      await binding;
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
    },
  );

  test(
    'matching requires explicit confirmation before durable binding',
    () async {
      final _Harness h = _Harness();
      addTearDown(h.close);
      await h.owner.restore(const LichessAccountContext('account'));
      await h.owner.prepare(boardContext: _identity, seek: _seek);
      await _flush();
      expect(h.http.paths, contains('/api/board/seek'));
      expect(h.store.records.first.seekMayHaveStarted, isFalse);
      expect(h.store.records.last.seekMayHaveStarted, isTrue);
      h.http.games = <Object?>[_game()];
      await h.owner.refreshCandidates();
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.awaitingGameConfirmation,
      );
      expect(h.owner.state.inputEnabled, isFalse);
      expect(h.store.records.last.providerGameId, isNull);
      await h.owner.confirmGame('game123', h.owner.state.evidenceRevision);
      expect(
        h.owner.state.stage,
        LichessPreparedSessionStage.boundAwaitingReconciliation,
      );
      expect(h.store.records.last.providerGameId, 'game123');
      expect(h.owner.state.inputEnabled, isFalse);
    },
  );
}

const _gameStreamPath = '/api/board/game/stream/game123';
Future<void> _bind(_Harness h) async {
  await h.owner.restore(const LichessAccountContext('account'));
  await h.owner.prepare(boardContext: _identity, seek: _seek);
  h.http.games = <Object?>[_game()];
  await h.owner.refreshCandidates();
  await h.owner.confirmGame('game123', h.owner.state.evidenceRevision);
  await _flush();
}

Future<void> _bindAndReconcile(_Harness h) async {
  await _bind(h);
  h.http.body(_gameStreamPath).add(utf8.encode('${jsonEncode(_full())}\n'));
  await _until(
    () =>
        h.owner.state.record?.confirmed?.receipt.ply == 0 &&
        h.owner.state.providerStreamConnected,
  );
}

Future<void> _bindBlackAndReconcile(_Harness h) async {
  await h.owner.restore(const LichessAccountContext('account'));
  await h.owner.prepare(boardContext: _identity, seek: _seek);
  h.http.games = <Object?>[_game(color: 'black')];
  await h.owner.refreshCandidates();
  await h.owner.confirmGame('game123', h.owner.state.evidenceRevision);
  await _flush();
  h.http
      .body(_gameStreamPath)
      .add(
        utf8.encode(
          '${jsonEncode(_full(moves: 'e2e4', accountIsBlack: true))}\n',
        ),
      );
  await _until(
    () =>
        h.owner.state.record?.confirmed?.receipt.ply == 1 &&
        h.owner.state.providerStreamConnected,
  );
}

Map<String, Object?> _state(
  String moves, {
  String status = 'started',
  String? winner,
  int whiteTime = 600000,
}) => {
  'type': 'gameState',
  'moves': moves,
  'wtime': whiteTime,
  'btime': 600000,
  'winc': 0,
  'binc': 0,
  'status': status,
  'winner': ?winner,
};
Map<String, Object?> _full({String moves = '', bool accountIsBlack = false}) =>
    {
      'type': 'gameFull',
      'id': 'game123',
      'variant': {'key': 'standard', 'name': 'Standard'},
      'speed': 'rapid',
      'rated': false,
      'createdAt': 0,
      'perf': {'name': 'Rapid'},
      'white': {
        'id': accountIsBlack ? 'opponent' : 'account',
        'name': accountIsBlack ? 'Opponent' : 'Account',
      },
      'black': {
        'id': accountIsBlack ? 'account' : 'opponent',
        'name': accountIsBlack ? 'Account' : 'Opponent',
      },
      'initialFen': 'startpos',
      'state': _state(moves),
    };

const _identity = LichessPreparedIdentity(
  sessionId: 'prepared-1',
  boardIdentity: 'board',
  boardGameId: '000102030405060708090a0b0c0d0e0f',
  lineRevision: 7,
);
const _otherIdentity = LichessPreparedIdentity(
  sessionId: 'other',
  boardIdentity: 'board',
  boardGameId: '000102030405060708090a0b0c0d0e0f',
  lineRevision: 7,
);
final _seek = LichessRealtimeSeek.casualRapid(minutes: 10, incrementSeconds: 0);
Map<String, Object?> _game({String id = 'game123', String color = 'white'}) =>
    <String, Object?>{
      'gameId': id,
      'color': color,
      'variant': <String, Object?>{'key': 'standard'},
      'speed': 'rapid',
      'rated': false,
      'isMyTurn': false,
      'opponent': <String, Object?>{'id': 'opponent', 'username': 'Opponent'},
    };
// Provider publication follows archive and checkpoint durability. Wait for the
// owner's receipt, rather than guessing how long filesystem work will take.
Future<void> _ownerState(
  _Harness h,
  bool Function(LichessPreparedSessionState) matches,
) async {
  if (!matches(h.owner.state)) await h.owner.states.firstWhere(matches);
}

Future<void> _flush() async {
  for (int i = 0; i < 30; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 2));
  }
}

final class _Harness {
  _Harness({
    LichessRequestCoordinator? coordinator,
    this.durableStore,
    Directory? libraryDirectory,
  }) : directory =
           libraryDirectory ??
           Directory.systemTemp.createTempSync('prepared-owner-'),
       ownsDirectory = libraryDirectory == null,
       coordinator =
           coordinator ?? LichessRequestCoordinator(cooldownStore: _Cooldown());
  final Directory directory;
  final bool ownsDirectory;
  final LichessPreparedSessionStore? durableStore;
  late final _GatedGameLibraryStore libraryStore = _GatedGameLibraryStore(
    directory,
  );
  late final GameLibraryRepository library = GameLibraryRepository(
    libraryStore,
  );
  final _Http http = _Http();
  final _Store store = _Store();
  final _Gate gate = _Gate();
  final _Awake awake = _Awake();
  final LichessRequestCoordinator coordinator;
  final factoryAccounts = <String>[];
  Future<String> Function(LichessAccountContext) token = (_) async =>
      'fixture-token';
  late final owner = LichessPreparedSessionOwner(
    clientFactory: (account) {
      factoryAccounts.add(account.accountId);
      return LichessBoardApiClient(
        httpClient: http,
        accessTokenProvider: () => token(account),
        requestCoordinator: coordinator,
      );
    },
    store: durableStore ?? store,
    gate: gate,
    keepAwake: awake,
    library: library,
  );
  Future<void> close() async {
    store.releaseHeld();
    gate.releaseHeld();
    await owner.dispose();
    await http.finish();
    await library.close();
    if (ownsDirectory) await directory.delete(recursive: true);
  }
}

final class _GatedLoad {
  final Completer<void> started = Completer<void>();
  final Completer<void> release = Completer<void>();
}

final class _GatedGameLibraryStore extends GameLibraryStore {
  _GatedGameLibraryStore(super.directory);

  _GatedLoad? _nextLoad;

  _GatedLoad blockNextLoad() {
    final gate = _GatedLoad();
    _nextLoad = gate;
    return gate;
  }

  @override
  Future<GameLibrarySnapshot> load() async {
    final gate = _nextLoad;
    _nextLoad = null;
    if (gate != null) {
      gate.started.complete();
      await gate.release.future;
    }
    return super.load();
  }
}

final class _Http extends http.BaseClient {
  final List<String> paths = <String>[];
  final List<StreamController<List<int>>> bodies =
      <StreamController<List<int>>>[];
  final bodyPaths = <String>[];
  StreamController<List<int>> body(String path) =>
      bodies[bodyPaths.lastIndexOf(path)];
  bool ignoreGameAbort = false;
  List<Object?> games = <Object?>[];
  Completer<http.StreamedResponse>? nextInventory;
  Completer<http.StreamedResponse>? nextAccount;
  Completer<http.StreamedResponse>? nextMove;
  int? nextMoveStatus;
  Object? nextMoveError;
  final List<String> movePaths = <String>[];
  final heldResponses = <Completer<http.StreamedResponse>>[];
  Completer<http.StreamedResponse> holdInventory() {
    final held = Completer<http.StreamedResponse>();
    heldResponses.add(held);
    return nextInventory = held;
  }

  Completer<http.StreamedResponse> holdAccount() {
    final held = Completer<http.StreamedResponse>();
    heldResponses.add(held);
    return nextAccount = held;
  }

  Completer<http.StreamedResponse> holdMove() {
    final held = Completer<http.StreamedResponse>();
    heldResponses.add(held);
    return nextMove = held;
  }

  @override
  Future<http.StreamedResponse> send(http.BaseRequest request) async {
    paths.add(request.url.path);
    final reply = _reply(request);
    if (request is http.AbortableRequest && request.abortTrigger != null) {
      return Future.any(<Future<http.StreamedResponse>>[
        reply,
        request.abortTrigger!.then(
          (_) => throw http.RequestAbortedException(request.url),
        ),
      ]);
    }
    return reply;
  }

  Future<http.StreamedResponse> _reply(http.BaseRequest request) async {
    if (request.url.path == '/api/stream/event' && nextAccount != null) {
      final held = nextAccount!;
      nextAccount = null;
      return held.future;
    }
    if (request.url.path == '/api/account/playing') {
      final held = nextInventory;
      nextInventory = null;
      return held == null ? _inventoryResponse(games) : held.future;
    }
    if (request.url.path.contains('/move/')) {
      movePaths.add(request.url.path);
      final held = nextMove;
      nextMove = null;
      if (held != null) return held.future;
      final Object? error = nextMoveError;
      nextMoveError = null;
      if (error != null) throw error;
      final int status = nextMoveStatus ?? 200;
      nextMoveStatus = null;
      return http.StreamedResponse(const Stream<List<int>>.empty(), status);
    }
    final body = StreamController<List<int>>();
    bodies.add(body);
    bodyPaths.add(request.url.path);
    if (request is http.AbortableRequest) {
      unawaited(
        request.abortTrigger?.then((_) {
          if (!body.isClosed &&
              !(ignoreGameAbort && request.url.path == _gameStreamPath)) {
            unawaited(body.close());
          }
        }),
      );
    }
    return http.StreamedResponse(body.stream, 200);
  }

  Future<void> finish() async {
    for (final held in heldResponses) {
      if (!held.isCompleted) held.complete(_inventoryResponse(<Object?>[]));
    }
    for (final body in bodies) {
      if (!body.isClosed) {
        await body.close();
      }
    }
  }
}

http.StreamedResponse _inventoryResponse(List<Object?> games) =>
    http.StreamedResponse(
      Stream<List<int>>.value(
        utf8.encode(
          jsonEncode(<String, Object?>{'nowPlaying': games, 'nbMyTurn': 0}),
        ),
      ),
      200,
    );

final class _Store implements LichessPreparedSessionStore {
  LichessPreparedLoad? loadResult;
  final records = <LichessPreparedRecord>[];
  Completer<void>? nextWrite;
  final heldWrites = <Completer<void>>[];
  int failNextWrites = 0;
  Completer<void> blockNext() {
    final held = Completer<void>();
    heldWrites.add(held);
    return nextWrite = held;
  }

  void releaseHeld() {
    for (final held in heldWrites) {
      if (!held.isCompleted) held.complete();
    }
  }

  @override
  Future<LichessPreparedLoad> load() async =>
      loadResult ??
      (records.isEmpty
          ? const LichessPreparedLoad(LichessPreparedLoadKind.absent)
          : LichessPreparedLoad(
              LichessPreparedLoadKind.current,
              record: records.last,
            ));
  @override
  Future<void> write(LichessPreparedRecord record) async {
    final held = nextWrite;
    nextWrite = null;
    if (held != null) await held.future;
    if (failNextWrites > 0) {
      failNextWrites--;
      throw StateError('prepared write unavailable');
    }
    records.add(record);
  }
}

final class _Gate implements LichessPreparedSessionGate {
  @override
  Future<LichessPreparedIdentity?> readback() async => armed;
  int arms = 0;
  String boardIdentity = 'board';
  bool ready = true;
  bool confirmDisarm = true;
  LichessPreparedIdentity? armed;
  final disarms = <LichessPreparedIdentity>[];
  Completer<bool>? armReply;
  final heldArms = <Completer<bool>>[];
  Completer<bool> blockArm() {
    final held = Completer<bool>();
    heldArms.add(held);
    return armReply = held;
  }

  void releaseHeld() {
    for (final held in heldArms) {
      if (!held.isCompleted) held.complete(true);
    }
  }

  @override
  Future<bool> armAndReadback(LichessPreparedIdentity identity) async {
    arms++;
    final held = armReply;
    armReply = null;
    final reply = held == null ? true : await held.future;
    final success =
        reply &&
        ready &&
        identity.boardIdentity == boardIdentity &&
        (armed == null || armed == identity);
    if (success) armed = identity;
    return success;
  }

  @override
  Future<bool> disarmPreSeek(LichessPreparedIdentity identity) async {
    disarms.add(identity);
    if (identity.boardIdentity != boardIdentity ||
        (armed != null && armed != identity)) {
      return false;
    }
    armed = null;
    return confirmDisarm;
  }
}

final class _Awake implements LichessPreparedKeepAwake {
  int acquires = 0, releases = 0;
  int releaseFailures = 0;
  bool allow = true;
  @override
  Future<bool> acquire() async {
    acquires++;
    return allow;
  }

  @override
  Future<void> release() async {
    releases++;
    if (releaseFailures > 0) {
      releaseFailures--;
      throw StateError('Fixture keep-awake release failure');
    }
  }
}

final class _Cooldown implements LichessCooldownStore {
  bool failWrites = false;

  @override
  Future<void> clear() async {}
  @override
  Future<LichessCooldownMarker?> read() async => null;
  @override
  Future<void> write(LichessCooldownMarker marker) async {
    if (failWrites) throw StateError('cooldown storage unavailable');
  }
}

Future<void> _until(bool Function() condition) async {
  for (var i = 0; i < 500 && !condition(); i++) {
    await Future<void>.delayed(const Duration(milliseconds: 2));
  }
  expect(
    condition(),
    isTrue,
    reason: 'bounded owner condition did not complete',
  );
}

final class _FaultDisk implements LichessPreparedSessionStore {
  _FaultDisk(this.inner);
  final FileLichessPreparedSessionStore inner;
  bool failNextConfirmation = false;
  Completer<void>? hold;
  Completer<void> entered = Completer<void>();
  Completer<void> holdConfirmation() {
    entered = Completer<void>();
    return hold = Completer<void>();
  }

  @override
  Future<LichessPreparedLoad> load() => inner.load();
  @override
  Future<void> write(LichessPreparedRecord record) async {
    if (record.confirmed != null) {
      if (failNextConfirmation) {
        failNextConfirmation = false;
        throw const FileSystemException('injected checkpoint failure');
      }
      final pending = hold;
      hold = null;
      if (pending != null) {
        entered.complete();
        await pending.future;
      }
    }
    await inner.write(record);
  }
}
