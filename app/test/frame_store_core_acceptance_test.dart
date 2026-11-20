import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

import '../tool/frame_store_core_acceptance.dart' as diagnostic;
import '../tool/frame_store_core_backup.dart';
import '../tool/frame_store_core_runner.dart';

const digest =
    '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';
const target = 'AA:BB:CC:DD:EE:FF';

void main() {
  late Directory support;
  late CoreBackup backup;
  setUp(() async {
    support = await Directory.systemTemp.createTemp('core-protection-test-');
    await Directory(
      '${support.path}/litchcraft/artworks',
    ).create(recursive: true);
    await File(
      '${support.path}/litchcraft/current.json',
    ).writeAsString('{"preserve":"unmigrated"}');
    await File(
      '${support.path}/litchcraft/artworks/original.json',
    ).writeAsString('original payload');
    backup = await CoreBackup.create(
      backupArtwork: true,
      support: support,
      target: target,
      firmwareSha256: digest,
      readPreferences: () async => {
        'litchcraft_artwork_v1': 'legacy payload',
        'litchcraft_artwork_v1_previous': 'previous',
        'litchcraft_artwork_v1_rejected': 'rejected',
        'app_litchcraft_storage_limit_bytes': 1000000,
        'app_litchcraft_hold_duration_ms': 500,
        'unrelated': 'do not export',
      },
    );
  });
  tearDown(() async {
    await support.delete(recursive: true);
  });

  test('default evidence never reads phone artwork or preferences', () async {
    await Link(
      '${support.path}/litchcraft/opaque',
    ).create('${support.path}/litchcraft/current.json');
    final evidence = await CoreBackup.create(
      support: support,
      target: target,
      firmwareSha256: digest,
      readPreferences: () async =>
          throw StateError('Phone preferences accessed'),
    );
    expect(evidence.manifest['phone'], isEmpty);
    expect(evidence.manifest['originals'], isEmpty);
    expect(
      await Directory('${evidence.directory.path}/phone').exists(),
      isFalse,
    );
  });

  test(
    'default runs five uploads without original Snapshot or restoration',
    () async {
      final evidence = await CoreBackup.create(
        support: support,
        target: target,
        firmwareSha256: digest,
        readPreferences: () async => {},
      );
      final board = _FixtureBoard()..forbidOriginalSnapshot = true;
      final result = await CoreProtectedRunner(
        board,
        evidence,
        (_, _) {},
      ).run();
      expect(result.passed, isTrue, reason: '${result.testFailure}');
      expect(result.verifiedUploads, 5);
      expect(board.originalSnapshotCalls, 0);
      expect(board.snapshotCalls, 5);
      expect(board.selections, 0);
      expect(board.bodyWrites, 6);
      expect(board.corpusDigests.toSet().length, 5);
      expect(evidence.manifest['originals'], isEmpty);
      expect(evidence.manifest['phone'], isEmpty);
      expect((evidence.manifest['samples'] as List).length, 5);
      expect(
        (evidence.manifest['interruptionEvidence'] as Map)['commit_packets'],
        0,
      );
      final resumed = await CoreBackup.resume(evidence.directory);
      expect(resumed.preservesArtwork, isFalse);
      expect(resumed.manifest['stage'], 'complete');
      expect((resumed.manifest['documents'] as List).length, 6);
      final beforeRestore =
          board.bodyWrites + board.selections + board.reconnections;
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      expect(
        board.bodyWrites + board.selections + board.reconnections,
        beforeRestore,
      );
      final message = diagnostic.coreOutcomeMessage(
        passed: result.passed,
        runResult: result,
        identityOnly: false,
        resumed: false,
        restored: false,
        fullRestoration: false,
        hasOriginals: false,
        backupPath: evidence.directory.path,
      );
      expect(message, contains('five complete transfers verified'));
      expect(message, contains('Restoration: not requested.'));
      expect(message, contains('Test evidence:'));
      expect(message, isNot(contains('Saved backup:')));
    },
  );

  test(
    'default accepts one resident and existing phase/history without preservation gates',
    () async {
      final evidence = await CoreBackup.create(
        support: support,
        target: target,
        firmwareSha256: digest,
      );
      final board = _FixtureBoard()
        ..roots.remove(1)
        ..phaseMs = 25
        ..history = true
        ..playing = true
        ..forbidOriginalSnapshot = true;
      final result = await CoreProtectedRunner(
        board,
        evidence,
        (_, _) {},
      ).run();
      expect(result.passed, isTrue, reason: '${result.testFailure}');
      expect(board.originalSnapshotCalls, 0);
      expect(board.selections, 0);
      expect(board.bodyWrites, 6);
    },
  );

  test(
    'default uncertain Commit retains only proposals and authorizes no recovery writes',
    () async {
      final evidence = await CoreBackup.create(
        support: support,
        target: target,
        firmwareSha256: digest,
      );
      final board = _FixtureBoard()..fault = 'unknown';
      final result = await CoreProtectedRunner(
        board,
        evidence,
        (_, _) {},
      ).run();
      expect(result.passed, isFalse);
      expect(result.testFailure, isA<CoreSendFailure>());
      expect(result.restorationFailure, isNull);
      expect(board.bodyWrites, 2);
      expect(board.selections, 0);
      expect(board.snapshotCalls, 0);
      final resumed = await CoreBackup.resume(evidence.directory);
      expect(resumed.manifest['phone'], isEmpty);
      expect(resumed.manifest['originals'], isEmpty);
      expect((resumed.manifest['pending'] as Map)['outcome'], 'unknown');
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      final retried = await CoreProtectedRunner(
        board,
        resumed,
        (_, _) {},
      ).run();
      expect(retried.passed, isFalse);
      expect(board.bodyWrites, 2);
      expect(board.selections, 0);
      final message = diagnostic.coreOutcomeMessage(
        passed: false,
        runResult: result,
        identityOnly: false,
        resumed: false,
        restored: false,
        fullRestoration: false,
        hasOriginals: false,
      );
      expect(message, contains('uncertain Commit outcome'));
      expect(message, contains('Restoration: not requested.'));
    },
  );

  test(
    'default rejects dirty/foreign authority and incomplete test readback without restoration',
    () async {
      for (final fault in [
        'dirty_baseline',
        'foreign',
        'foreign_clean',
        'corrupt',
        'interruption_descriptor',
        'runtime_after_readback',
      ]) {
        final evidence = await CoreBackup.create(
          support: support,
          target: target,
          firmwareSha256: digest,
        );
        final board = _FixtureBoard()
          ..fault = fault
          ..dirty = fault == 'dirty_baseline';
        final result = await CoreProtectedRunner(
          board,
          evidence,
          (_, _) {},
        ).run();
        expect(result.passed, isFalse, reason: fault);
        expect(result.testFailure, isNotNull, reason: fault);
        expect(result.restorationFailure, isNull, reason: fault);
        expect(
          board.bodyWrites,
          fault == 'dirty_baseline'
              ? 0
              : fault == 'interruption_descriptor'
              ? 1
              : 2,
        );
        expect(board.originalSnapshotCalls, 0);
        expect(board.selections, 0);
      }
    },
  );

  test('explicit Android/iOS targets and strict production State37 decode', () {
    validateCoreTarget('android', target, digest);
    validateCoreTarget('ios', '00000000-0000-4000-8000-000000000011', digest);
    expect(
      () => validateCoreTarget('android', target.toLowerCase(), digest),
      throwsArgumentError,
    );
    expect(
      () => validateCoreTarget('ios', target, digest),
      throwsArgumentError,
    );
    expect(
      () =>
          validateCoreTarget('ios', '00000000-0000-4000-8000-000000000011', ''),
      throwsArgumentError,
    );
    final body = Uint8List(10)..[0] = 1;
    ByteData.sublistView(body)
      ..setUint32(2, 193, Endian.little)
      ..setUint32(6, 812, Endian.little);
    ConnectionSessionPacket packet() => ConnectionSessionPacket(
      kind: ConnectionSessionKind.state,
      code: ConnectionSession.litchcraftStoreAuthorityStateCode,
      token: 1,
      sequence: 2,
      body: body,
    );
    final authority = CoreAuthority.decode(packet(), 123);
    expect(authority.generation, 193);
    expect(authority.revision, 812); // Live revision is not root generation.
    body[1] = 2;
    expect(() => CoreAuthority.decode(packet(), 123), throwsFormatException);
  });

  test(
    'immutable phone backup resumes without product migration or preference writes',
    () async {
      expect(
        await File('${support.path}/litchcraft/migration.json').exists(),
        isFalse,
      );
      expect(
        await File('${support.path}/litchcraft/current.json').readAsString(),
        '{"preserve":"unmigrated"}',
      );
      final prefs =
          jsonDecode(
                await File(
                  '${backup.directory.path}/phone/preferences.json',
                ).readAsString(),
              )
              as Map;
      expect(prefs.length, 5);
      expect(prefs.containsKey('unrelated'), isFalse);
      final resumed = await CoreBackup.resume(backup.directory);
      expect(resumed.manifest['stage'], 'phone_saved');
      await File(
        '${backup.directory.path}/phone/litchcraft/current.json',
      ).writeAsString('corrupt');
      expect(() => CoreBackup.resume(backup.directory), throwsFormatException);
    },
  );

  test(
    'disk/copy/preferences instability fails before any board mutation',
    () async {
      final board = _FixtureBoard();
      var reads = 0;
      await expectLater(
        CoreBackup.create(
          backupArtwork: true,
          support: support,
          target: target,
          firmwareSha256: digest,
          readPreferences: () async => {
            'litchcraft_artwork_v1': 'changing-${reads++}',
          },
        ),
        throwsStateError,
      );
      expect(board.bodyWrites + board.selections, 0);
      final blocker = File('${support.path}/blocked')
        ..writeAsStringSync('disk fault equivalent');
      await expectLater(
        CoreBackup.create(
          backupArtwork: true,
          support: Directory(blocker.path),
          target: target,
          firmwareSha256: digest,
          readPreferences: () async => {},
        ),
        throwsA(isA<FileSystemException>()),
      );
      expect(board.bodyWrites + board.selections, 0);
    },
  );

  test(
    'unsupported dirty/history/runtime/one-root baselines have zero mutations',
    () async {
      for (final board in [
        _FixtureBoard()..dirty = true,
        _FixtureBoard()..history = true,
        _FixtureBoard()..phaseMs = 4,
        _FixtureBoard()..roots.remove(1),
      ]) {
        final result = await CoreProtectedRunner(
          board,
          backup,
          (_, _) {},
        ).run();
        expect(result.testFailure, isA<CoreUnsupportedBaseline>());
        expect(board.bodyWrites + board.selections, 0);
        expect(result.passed, isFalse);
      }
    },
  );

  test(
    'lost selection receipt preserves selected backup and leaves restoration pending',
    () async {
      final board = _FixtureBoard()..selectionFailureOnce = true;
      final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
      expect(result.passed, isFalse);
      expect(result.testFailure, isNotNull);
      expect(result.restorationFailure, isA<StateError>());
      expect(board.selected, 1);
      expect(board.bodyWrites, 0);
      expect(board.selections, 1);
      expect((backup.manifest['originals'] as List).length, 1);
      final resumed = await CoreBackup.resume(backup.directory);
      final path = (resumed.manifest['originals'] as List).first as String;
      verifyCoreDocument(board.originals[0]!, await resumed.document(path));
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      expect(board.selections, 1);
      expect(board.bodyWrites, 0);
    },
  );

  test(
    'restart pending selection rejects foreign same-target selection after pre-submit checkpoint',
    () async {
      final board = _FixtureBoard();
      await _saveSelectionCheckpoint(backup, board);
      await board.select(board.resident(1, board.roots[1]!));
      final selectionsBefore = board.selections;
      final resumed = await CoreBackup.resume(backup.directory);
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      expect(board.selections, selectionsBefore);
      expect(board.bodyWrites, 0);
      expect(board.selected, 1);
      verifyCoreDocument(board.originals[0]!, board.roots[0]!.document);
      verifyCoreDocument(board.originals[1]!, board.roots[1]!.document);
    },
  );

  test(
    'restart pending selection with unchanged exact fence permits selection-only recovery',
    () async {
      final board = _FixtureBoard();
      await _saveSelectionCheckpoint(backup, board);
      final resumed = await CoreBackup.resume(backup.directory);
      await CoreProtectedRunner(board, resumed, (_, _) {}).restore();
      expect(board.selections, 1);
      expect(board.bodyWrites, 0);
      expect(board.selected, 0);
      verifyCoreDocument(board.originals[0]!, board.roots[0]!.document);
      verifyCoreDocument(board.originals[1]!, board.roots[1]!.document);
    },
  );

  test(
    'restart after persisted owned selection terminal permits selection-only recovery',
    () async {
      final board = _FixtureBoard();
      await _saveSelectionCheckpoint(backup, board);
      final confirmed = await board.select(board.resident(1, board.roots[1]!));
      // The owned terminal is validated before _select persists this new fence.
      confirmed.requireBaseline();
      expect(confirmed.state.selectedRootSlot, 1);
      expect(confirmed.authority.revision, 1001);
      backup.manifest['fence'] = confirmed.toJson();
      await backup.checkpoint();
      // Simulate termination before the subsequent pending-intent removal.
      final resumed = await CoreBackup.resume(backup.directory);
      await CoreProtectedRunner(board, resumed, (_, _) {}).restore();
      expect(board.selections, 2);
      expect(board.bodyWrites, 0);
      expect(board.selected, 0);
      verifyCoreDocument(board.originals[0]!, board.roots[0]!.document);
      verifyCoreDocument(board.originals[1]!, board.roots[1]!.document);
    },
  );

  test(
    'five distinct full256 transfers plus first interrupted body restore both originals',
    () async {
      final board = _FixtureBoard();
      final events = <String>[];
      final result = await CoreProtectedRunner(
        board,
        backup,
        (event, _) => events.add(event),
      ).run();
      expect(
        result.passed,
        isTrue,
        reason: '${result.testFailure} / ${result.restorationFailure}',
      );
      expect(result.verifiedUploads, 5);
      expect(board.interruptionLostInactive, isTrue);
      expect(board.corpusDigests.toSet().length, 5);
      expect(events, contains('pre_commit_interruption_verified'));
      expect(board.reconnections, greaterThanOrEqualTo(8));
      expect(board.roots.length, 2);
      verifyCoreDocument(
        board.originals[0]!,
        board.roots[board.selected]!.document,
      );
      verifyCoreDocument(
        board.originals[1]!,
        board.roots[board.selected ^ 1]!.document,
      );
      final resumed = await CoreBackup.resume(backup.directory);
      expect(resumed.manifest['stage'], 'restored');
      expect((resumed.manifest['samples'] as List).length, 7);
      expect(
        (resumed.manifest['interruptionEvidence'] as Map)['commit_packets'],
        0,
      );
    },
  );

  test(
    'pre-Commit failure and corrupt readback cannot pass; both originals restored',
    () async {
      for (final fault in ['pre_commit', 'corrupt']) {
        final runBackup = await CoreBackup.create(
          backupArtwork: true,
          support: support,
          target: target,
          firmwareSha256: digest,
          readPreferences: () async => {},
        );
        final board = _FixtureBoard()..fault = fault;
        final result = await CoreProtectedRunner(
          board,
          runBackup,
          (_, _) {},
        ).run();
        expect(result.passed, isFalse);
        expect(result.testFailure, isNotNull);
        expect(
          result.restorationFailure,
          isNull,
          reason: '${result.restorationFailure}',
        );
        verifyCoreDocument(
          board.originals[0]!,
          board.roots[board.selected]!.document,
        );
        verifyCoreDocument(
          board.originals[1]!,
          board.roots[board.selected ^ 1]!.document,
        );
      }
    },
  );

  test(
    'lost Commit receipt settles known full authority and restores without false test pass',
    () async {
      final board = _FixtureBoard()..fault = 'unknown';
      final events = <String>[];
      final result = await CoreProtectedRunner(
        board,
        backup,
        (event, _) => events.add(event),
      ).run();
      expect(result.passed, isFalse);
      expect(result.testFailure, isA<CoreSendFailure>());
      expect(result.restorationFailure, isNull);
      expect(events, contains('unknown_commit_settled'));
      verifyCoreDocument(
        board.originals[0]!,
        board.roots[board.selected]!.document,
      );
      verifyCoreDocument(
        board.originals[1]!,
        board.roots[board.selected ^ 1]!.document,
      );
    },
  );

  test(
    'foreign or dirty live revision aborts restoration and keeps validated originals',
    () async {
      final board = _FixtureBoard()..fault = 'foreign';
      final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
      expect(result.passed, isFalse);
      expect(result.restorationFailure, isNotNull);
      expect(
        board.bodyWrites,
        2,
      ); // Interruption plus one corpus, never restore.
      final resumed = await CoreBackup.resume(backup.directory);
      expect((resumed.manifest['originals'] as List).length, 2);
      await resumed.document(
        (resumed.manifest['originals'] as List).first as String,
      );
    },
  );

  test(
    'corrupt saved phone copy stops runner before resident selection/body',
    () async {
      await File(
        '${backup.directory.path}/phone/litchcraft/current.json',
      ).writeAsString('corruption');
      final board = _FixtureBoard();
      final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
      expect(result.passed, isFalse);
      expect(result.testFailure, isA<FormatException>());
      expect(board.bodyWrites + board.selections, 0);
    },
  );

  test('clean foreign live revision prevents restoration overwrite', () async {
    final board = _FixtureBoard()..fault = 'foreign_clean';
    final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
    expect(result.passed, isFalse);
    expect(result.restorationFailure, isNotNull);
    expect(board.bodyWrites, 2);
    await CoreBackup.resume(backup.directory);
  });

  test(
    'restart unknown checkpoint rejects foreign switch to original-other before submission',
    () async {
      final board = _FixtureBoard();
      board.roots[1] = _Resident(board.originals[1]!, 101);
      board.generation = 101;
      await _saveUnknownCheckpoint(backup, board);
      final other = board.resident(1, board.roots[1]!);
      await board.select(
        other,
      ); // Another client intervenes after the checkpoint.
      final selectionsBeforeResume = board.selections;
      final resumed = await CoreBackup.resume(backup.directory);
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      expect(board.bodyWrites, 0);
      expect(board.selections, selectionsBeforeResume);
      expect(board.selected, 1);
      verifyCoreDocument(
        board.originals[1]!,
        board.roots[board.selected]!.document,
      );
    },
  );

  test(
    'restart unknown restoration proposal cannot adopt a foreign already-resident original',
    () async {
      final board = _FixtureBoard();
      board.roots[1] = _Resident(board.originals[1]!, 101);
      board.generation = 101;
      await _saveUnknownCheckpoint(
        backup,
        board,
        proposalDocument: board.originals[1]!,
      );
      await board.select(board.resident(1, board.roots[1]!));
      final selectionsBefore = board.selections;
      final resumed = await CoreBackup.resume(backup.directory);
      await expectLater(
        CoreProtectedRunner(board, resumed, (_, _) {}).restore(),
        throwsStateError,
      );
      expect(board.bodyWrites, 0);
      expect(board.selections, selectionsBefore);
      expect(board.selected, 1);
    },
  );

  test(
    'restart unknown checkpoint settles unchanged exact saved selection',
    () async {
      final board = _FixtureBoard();
      await _saveUnknownCheckpoint(backup, board);
      final resumed = await CoreBackup.resume(backup.directory);
      final events = <String>[];
      await CoreProtectedRunner(
        board,
        resumed,
        (event, _) => events.add(event),
      ).restore();
      expect(events, contains('unknown_commit_settled'));
      verifyCoreDocument(
        board.originals[0]!,
        board.roots[board.selected]!.document,
      );
      verifyCoreDocument(
        board.originals[1]!,
        board.roots[board.selected ^ 1]!.document,
      );
    },
  );

  test(
    'operator outcome shows test failure with independently successful restoration',
    () {
      final message = diagnostic.coreOutcomeMessage(
        backupArtwork: true,
        passed: false,
        identityOnly: false,
        resumed: false,
        restored: true,
        fullRestoration: true,
        hasOriginals: true,
        backupPath: '/saved/run',
        testFailure: StateError('Transfer rejected'),
      );
      expect(message, contains('Transfer test: failed.'));
      expect(message, contains('Transfer rejected'));
      expect(
        message,
        contains('Restoration: both original artworks restored and checked.'),
      );
      expect(message, contains('Saved backup: /saved/run'));
      expect(message, isNot(contains('Restoration: pending.')));
    },
  );

  test(
    'operator outcome shows both failures and accurate restoration frame counts',
    () {
      final message = diagnostic.coreOutcomeMessage(
        backupArtwork: true,
        passed: false,
        identityOnly: false,
        resumed: false,
        restored: false,
        fullRestoration: false,
        hasOriginals: true,
        backupPath: '/saved/run',
        testFailure: StateError('Transfer rejected'),
        restorationFailure: StateError('Foreign authority'),
      );
      expect(message, contains('Transfer test: failed.'));
      expect(message, contains('Transfer rejected'));
      expect(message, contains('Restoration: pending.'));
      expect(message, contains('Foreign authority'));
      expect(message, contains('Saved backup: /saved/run'));
      expect(
        diagnostic.coreTransferProgress(1, restoring: true),
        'Restoring original artwork (1 frame)…',
      );
      expect(
        diagnostic.coreTransferProgress(8, restoring: true),
        'Restoring original artwork (8 frames)…',
      );
      expect(
        diagnostic.coreTransferProgress(256),
        'Sending complete artwork (256 frames)…',
      );
    },
  );

  test(
    'second resident Snapshot failure restores selected original without body writes',
    () async {
      final board = _FixtureBoard()..secondSnapshotFailureOnce = true;
      final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
      expect(result.testFailure, isA<StateError>());
      expect(result.restorationFailure, isNull);
      expect(board.bodyWrites, 0);
      expect(board.selected, 0);
      expect((backup.manifest['originals'] as List).length, 1);
      verifyCoreDocument(board.originals[0]!, board.roots[0]!.document);
      verifyCoreDocument(board.originals[1]!, board.roots[1]!.document);
    },
  );

  test(
    'restart unknown checkpoint settles only the exact own committed proposal at revision+1',
    () async {
      final board = _FixtureBoard();
      await _saveUnknownCheckpoint(backup, board);
      await board.send(coreCorpus(0), await board.read(), nonresident: true);
      final resumed = await CoreBackup.resume(backup.directory);
      final events = <String>[];
      await CoreProtectedRunner(
        board,
        resumed,
        (event, _) => events.add(event),
      ).restore();
      expect(events, contains('unknown_commit_settled'));
      verifyCoreDocument(
        board.originals[0]!,
        board.roots[board.selected]!.document,
      );
      verifyCoreDocument(
        board.originals[1]!,
        board.roots[board.selected ^ 1]!.document,
      );
    },
  );

  test('restoration failure is separate and never yields pass', () async {
    final board = _FixtureBoard()..failRestore = true;
    final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
    expect(result.verifiedUploads, 5);
    expect(result.testFailure, isNull);
    expect(result.restorationFailure, isNotNull);
    expect(result.passed, isFalse);
    await CoreBackup.resume(backup.directory);
  });

  test(
    'operator shows five successful transfers when restoration alone fails',
    () async {
      final board = _FixtureBoard()..failRestore = true;
      final result = await CoreProtectedRunner(board, backup, (_, _) {}).run();
      expect(result.verifiedUploads, 5);
      expect(result.testFailure, isNull);
      expect(result.restorationFailure, isNotNull);
      expect(result.passed, isFalse);
      final message = diagnostic.coreOutcomeMessage(
        backupArtwork: true,
        passed: result.passed,
        runResult: result,
        identityOnly: false,
        resumed: false,
        restored: false,
        fullRestoration: false,
        hasOriginals: true,
        backupPath: backup.directory.path,
        testFailure: result.testFailure,
        restorationFailure: result.restorationFailure,
      );
      expect(
        message,
        contains('Transfer test: five complete transfers verified.'),
      );
      expect(message, contains('Restoration: pending.'));
      expect(message, contains('Controlled first-body fault'));
      expect(message, isNot(contains('Transfer verification incomplete.')));
    },
  );

  test(
    'operator separates verified transfers from connection cleanup failure',
    () {
      final result = CoreRunResult(5, null, null);
      final message = diagnostic.coreOutcomeMessage(
        backupArtwork: true,
        passed: false,
        runResult: result,
        identityOnly: false,
        resumed: false,
        restored: true,
        fullRestoration: true,
        hasOriginals: true,
        cleanupFailure: StateError('Cleanup deadline expired'),
      );
      expect(
        message,
        contains('Transfer test: five complete transfers verified.'),
      );
      expect(message, contains('Restoration: both original artworks restored'));
      expect(message, contains('Connection cleanup: failed.'));
      expect(message, contains('Cleanup deadline expired'));
    },
  );

  test(
    'production Store sender drains only its first54 window, omits Commit',
    () async {
      final document = coreCorpus(99);
      final transfer = prepare(document, 1, 2);
      final packets = StreamController<LitchcraftV2Packet>.broadcast();
      addTearDown(packets.close);
      var cancelled = false, bodies = 0, commits = 0;
      final client = LitchcraftFrameStoreClient(
        packets: packets.stream,
        writeControl: (_) async {},
        writeBulk: (_) async {},
        requestReceipt: (_) async {},
        writeBulkBatch: (batch) async {
          bodies += batch
              .where(
                (packet) =>
                    packet[2] == LitchcraftFrameStoreProtocol.storeChunk,
              )
              .length;
          commits += batch
              .where(
                (packet) =>
                    packet[2] == LitchcraftFrameStoreProtocol.storeCommit,
              )
              .length;
          cancelled = true;
        },
        cancelWriteIsAuthoritative: true,
        monotonicMilliseconds: () => 0,
      );
      expect(transfer.bodyBytes, greaterThan(54 * (226 - 13)));
      await expectLater(
        client.send(
          transfer,
          maximumPacketBytes: 226,
          isCancelled: () => cancelled,
        ),
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      expect(bodies, LitchcraftFrameStoreClient.highMtuWindowChunks);
      expect(commits, 0);
    },
  );
}

Future<void> _saveSelectionCheckpoint(
  CoreBackup backup,
  _FixtureBoard board,
) async {
  final view = await board.read();
  final selected = await backup.saveDocument(
    board.originals[0]!,
    'original-selected',
  );
  final initial = board.resident(0, board.roots[0]!);
  final target = board.resident(1, board.roots[1]!);
  Map<String, int> descriptor(LitchcraftFrameStoreResident resident) => {
    'rootSlot': resident.rootSlot,
    'generation': resident.generation,
    'bodyCrc32': resident.bodyCrc32,
    'bodyBytes': resident.bodyBytes,
  };
  backup.manifest.addAll({
    'stage': 'board_backup',
    'originals': [selected],
    'selectedDocument': selected,
    'fence': view.toJson(),
    'initialResident': descriptor(initial),
    'pending': {'kind': 'selection', 'resident': descriptor(target)},
  });
  await backup.checkpoint();
}

Future<void> _saveUnknownCheckpoint(
  CoreBackup backup,
  _FixtureBoard board, {
  LitchcraftAnimationDocument? proposalDocument,
}) async {
  final view = await board.read();
  final selected = await backup.saveDocument(
    board.originals[0]!,
    'original-selected',
  );
  final other = await backup.saveDocument(
    board.originals[1]!,
    'original-other',
  );
  final proposal = await backup.saveDocument(
    proposalDocument ?? coreCorpus(0),
    'pending',
  );
  backup.manifest.addAll({
    'stage': 'ready',
    'originals': [selected, other],
    'fence': view.toJson(),
    'selectedDocument': selected,
    'pending': {
      'kind': 'transfer',
      'document': proposal,
      'outcome': 'unknown',
      'priorDocument': selected,
      'fence': view.toJson(),
      'priorResidents': view.state.residents
          .map(
            (r) => {
              'rootSlot': r.rootSlot,
              'generation': r.generation,
              'bodyCrc32': r.bodyCrc32,
              'bodyBytes': r.bodyBytes,
            },
          )
          .toList(),
    },
  });
  await backup.checkpoint();
}

LitchcraftFrameStoreTransfer prepare(
  LitchcraftAnimationDocument document,
  int session,
  int generation,
) => LitchcraftFrameStoreProtocol.prepare(
  session: session,
  transfer: 9,
  expectedGeneration: generation,
  topology: document.topology.id,
  active: true,
  playbackOnEntry: document.playbackOnEntry,
  sparkleSeed: document.sparkleSeed,
  favoriteColors: document.favoriteColors,
  frames: document.frames,
);

class _Resident {
  _Resident(this.document, this.generation);
  final LitchcraftAnimationDocument document;
  final int generation;
}

/// Controlled faults model the real first-body inactive-root invalidation.
/// They prove local protection order, not BLE, physical phase or flash timing.
class _FixtureBoard implements CoreBoard {
  _FixtureBoard() {
    originals = {
      for (var i = 0; i < 2; i++)
        i: LitchcraftAnimationDocument(
          topology: LitchcraftTopology.v3,
          sparkleSeed: i + 1,
          favoriteColors: litchcraftDefaultFavoriteColors,
          frames: [
            LitchcraftAnimationFrame(
              emitters: List.generate(
                256,
                (index) =>
                    LitchcraftAnimatedEmitter(color: 0x123456 + i + index),
              ),
            ),
          ],
        ),
    };
    roots = {0: _Resident(originals[0]!, 100), 1: _Resident(originals[1]!, 99)};
  }
  late Map<int, LitchcraftAnimationDocument> originals;
  late Map<int, _Resident> roots;
  int selected = 0,
      revision = 1000,
      generation = 100,
      bodyWrites = 0,
      selections = 0,
      reconnections = 0,
      phaseMs = 0;
  bool dirty = false,
      playing = false,
      history = false,
      selectionFailureOnce = false,
      interruptionLostInactive = false,
      failRestore = false,
      secondSnapshotFailureOnce = false,
      forbidOriginalSnapshot = false;
  int snapshotCalls = 0, originalSnapshotCalls = 0;
  String? fault;
  bool _faultUsed = false, _corruptNext = false;
  final corpusDigests = <String>[];

  LitchcraftFrameStoreResident resident(int root, _Resident value) {
    final transfer = prepare(value.document, 7, value.generation);
    return LitchcraftFrameStoreResident(
      rootSlot: root,
      generation: value.generation,
      bodyCrc32: transfer.canonicalBodyCrc32,
      bodyBytes: transfer.canonicalBodyBytes,
      frameCount: value.document.frames.length,
      topology: 3,
      active: true,
      playbackOnEntry: value.document.playbackOnEntry,
      predecessorDeltas: true,
      sparkleSeed: value.document.sparkleSeed,
      favoriteColors: value.document.favoriteColors,
    );
  }

  @override
  Future<CoreBoardView> read() async => CoreBoardView(
    LitchcraftFrameStoreState(
      session: 7,
      generation: roots[selected]!.generation,
      requestId: 1,
      status: 0,
      active: true,
      playing: playing,
      hasArtwork: true,
      frameCount: roots[selected]!.document.frames.length,
      selectedFrame: 0,
      topology: 3,
      includesResidentListing: true,
      selectedRootSlot: selected,
      residents: roots.entries.map((e) => resident(e.key, e.value)).toList(),
    ),
    CoreAuthority(42, roots[selected]!.generation, revision, dirty),
    elapsedMs: phaseMs,
    history: history,
  );
  @override
  Future<LitchcraftFrameStoreSnapshot> snapshot(CoreBoardView view) async {
    snapshotCalls++;
    if (secondSnapshotFailureOnce && snapshotCalls == 2) {
      secondSnapshotFailureOnce = false;
      throw StateError('Second resident Snapshot failed');
    }
    final expected = roots[selected]!.document;
    if (expected.frames.length == 1) {
      originalSnapshotCalls++;
      if (forbidOriginalSnapshot) {
        throw StateError('Original Snapshot forbidden');
      }
    }
    final encoded = encodeLitchcraftAnimationDocument(
      expected,
      localStorage: true,
    );
    final document = _corruptNext
        ? originals[0]!
        : decodeLitchcraftAnimationDocument(
            encoded.bytes,
            encoded.crc32,
            localStorage: true,
          );
    _corruptNext = false;
    if (fault == 'runtime_after_readback') {
      playing = true;
      phaseMs = 5;
    }
    return LitchcraftFrameStoreSnapshot(
      document: document,
      session: 7,
      generation: view.state.generation,
      selectedFrame: 0,
      active: true,
      playing: false,
    );
  }

  @override
  Future<CoreBoardView> select(LitchcraftFrameStoreResident descriptor) async {
    selected = descriptor.rootSlot;
    revision++;
    selections++;
    if (selectionFailureOnce) {
      selectionFailureOnce = false;
      throw StateError('Selection applied; receipt lost');
    }
    return read();
  }

  @override
  Future<void> reconnect() async {
    reconnections++;
  }

  @override
  Future<CoreSendEvidence> send(
    LitchcraftAnimationDocument document,
    CoreBoardView view, {
    bool interrupt = false,
    bool nonresident = false,
  }) async {
    bodyWrites++;
    roots.remove(selected ^ 1); // The production seam the old tool overlooked.
    if (interrupt) {
      interruptionLostInactive = roots.length == 1;
      if (fault == 'interruption_descriptor') {
        roots[selected] = _Resident(originals[1]!, roots[selected]!.generation);
      }
      return CoreSendEvidence(null, {
        'body_packets': 48,
        'commit_packets': 0,
        'disconnected': true,
      });
    }
    if (failRestore && !nonresident || fault == 'pre_commit' && !_faultUsed) {
      _faultUsed = true;
      throw CoreSendFailure(
        StateError('Controlled first-body fault'),
        commitSubmitted: false,
      );
    }
    playing = false;
    phaseMs = 0;
    history = false;
    selected ^= 1;
    roots[selected] = _Resident(document, ++generation);
    revision++;
    if (nonresident) corpusDigests.add(coreDocumentSha256(document));
    if (!_faultUsed && fault != null) {
      _faultUsed = true;
      if (fault == 'unknown') {
        throw CoreSendFailure(
          StateError('Lost durable receipt'),
          commitSubmitted: true,
        );
      }
      if (fault == 'corrupt') _corruptNext = true;
      if (fault == 'foreign' || fault == 'foreign_clean') {
        revision++;
        dirty = fault == 'foreign';
      }
    }
    return CoreSendEvidence(
      LitchcraftFrameStoreTransferResult(
        state: (await read()).state,
        bodyBytes: prepare(document, 7, generation).bodyBytes,
        chunkPackets: 100,
        checkpoints: 0,
        writes: 102,
        beginWriteMilliseconds: 1,
        bulkWriteMilliseconds: 2,
        checkpointMilliseconds: 0,
        commitWriteMilliseconds: 1,
        receiptWaitMilliseconds: 3,
        elapsedMilliseconds: 7,
      ),
      {'commit_packets': 1, 'body_packets': 100},
    );
  }
}
