import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/local_player_profile.dart';
import 'package:litch_mobile/src/models/current_game_line_state.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/local_player_profile_store.dart';
import 'package:litch_mobile/src/services/physical_game_history_ingestion.dart';
import 'package:litch_mobile/src/screens/game_library_screen.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/recent_games_section.dart';

void main() {
  late Directory root;
  late GameLibraryRepository repository;
  late _HistoryBleService bleService;
  late PhysicalGameHistoryIngestion ingestion;

  setUp(() async {
    root = await Directory.systemTemp.createTemp('litch-history-ingestion-');
    repository = GameLibraryRepository(
      GameLibraryStore(Directory('${root.path}/library')),
    );
    bleService = _HistoryBleService();
    ingestion = PhysicalGameHistoryIngestion(
      bleService: bleService,
      repository: repository,
      receiveDirectory: Directory('${root.path}/receive'),
      clock: () => DateTime.utc(2026, 9, 14),
    );
    ingestion.start();
  });

  tearDown(() async {
    await ingestion.close();
    await bleService.dispose();
    await repository.close();
    if (await root.exists()) await root.delete(recursive: true);
  });

  testWidgets(
    'unpaired native connection and cold attachment never probe history; readiness starts ingestion',
    (tester) async {
      bleService.ownerReady = false;
      bleService.installLine(
        _completedClassic('000102030405060708090a0b0c0d0e0f'),
      );
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      expect(bleService.capabilityReads, 0);
      expect(bleService.archiveAcknowledgements, 0);
      await tester.runAsync(ingestion.close);
      await tester.runAsync(() async {
        ingestion = PhysicalGameHistoryIngestion(
          bleService: bleService,
          repository: repository,
          receiveDirectory: Directory('${root.path}/receive'),
        )..start();
        await ingestion.idle;
      });
      expect(bleService.capabilityReads, 0);
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.emitReadiness(true),
      );
      expect(bleService.capabilityReads, 1);
      expect(bleService.archiveAcknowledgements, 1);
    },
  );

  testWidgets(
    'connected Classic completion becomes durable and visible in Recent Games',
    (WidgetTester tester) async {
      const String gameId = '000102030405060708090a0b0c0d0e0f';
      bleService.installLine(_completedClassic(gameId));
      final Future<GameLibrarySnapshot> changed = repository.changes.first;
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      final GameLibrarySnapshot notification = (await tester.runAsync(
        () => changed,
      ))!;
      expect(notification.games.single.boardGameId, gameId);

      final GameLibraryRepository reopened = GameLibraryRepository(
        GameLibraryStore(Directory('${root.path}/library')),
      );
      addTearDown(reopened.close);
      final GameLibrarySnapshot durable = (await tester.runAsync(
        reopened.load,
      ))!;
      expect(durable.games.single.variant, GameVariant.classic);
      expect(durable.games.single.status, GameArchiveStatus.completed);
      expect(bleService.archiveAcknowledgements, 1);
      final GameLibraryWindow visibleWindow = (await tester.runAsync(
        () => repository.loadWindow(offset: 0, limit: 5),
      ))!;
      expect(visibleWindow.games.single.libraryId, 'board-$gameId');

      await tester.pumpWidget(_recentHarness(repository));
      await _settleRepositoryWidget(tester);
      expect(
        find.byKey(ValueKey<String>('recent-game-board-$gameId')),
        findsOneWidget,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await _settleRepositoryWidget(tester);
      await tester.runAsync(repository.close);
    },
  );

  testWidgets(
    'captured continuous physical play survives NewGame without reconnect or terminal outcome',
    (WidgetTester tester) async {
      const String previousId = '808182838485868788898a8b8c8d8e8f';
      const String nextId = '909192939495969798999a9b9c9d9e9f';
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);

      for (final int depth in <int>[1, 2]) {
        final line = _ongoingClassic(previousId).take(depth + 1).toList();
        bleService.installLine(line);
        await _emitAndDrain(tester, ingestion, bleService.emitCurrentHomeState);
      }

      // Explicit board NewGame immediately replaces the only board line. The
      // phone must already hold the old game; a pull after reset cannot recover it.
      bleService.clearLine();
      await _emitAndDrain(tester, ingestion, bleService.emitNewGameApplied);
      bleService.installLine(_ongoingClassic(nextId).take(2).toList());
      await _emitAndDrain(tester, ingestion, bleService.emitCurrentHomeState);

      final reopened = GameLibraryRepository(
        GameLibraryStore(Directory('${root.path}/library')),
      );
      addTearDown(reopened.close);
      final durable = (await tester.runAsync(reopened.load))!;
      expect(durable.games, hasLength(2));
      final previous = durable.games.singleWhere(
        (game) => game.boardGameId == previousId,
      );
      expect(previous.captureQuality, GameCaptureQuality.complete);
      expect(previous.currentPly, 2);
      expect(
        previous.positions.map((position) => position.fen),
        _ongoingClassic(previousId).map(
          (record) => switch (record) {
            GameHistoryLineBaseRecord record => record.position.fen,
            GameHistoryCommittedPlyRecord record => record.position.fen,
            _ => throw StateError('Ongoing fixture contains a seal.'),
          },
        ),
      );
      expect(bleService.archiveAcknowledgements, 3);
      expect(bleService.controlDispatches, 0);
    },
  );

  testWidgets(
    'deleted current game stays absent after reopen and automatic sync while a new game is captured',
    (tester) async {
      const deletedId = '505152535455565758595a5b5c5d5e5f';
      const nextId = '606162636465666768696a6b6c6d6e6f';
      bleService.installLine(_ongoingClassic(deletedId).take(2).toList());
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      expect(bleService.archiveAcknowledgements, 1);
      await tester.runAsync(() => repository.delete('board-$deletedId'));

      await tester.runAsync(() async {
        await ingestion.close();
        await repository.close();
        repository = GameLibraryRepository(
          GameLibraryStore(Directory('${root.path}/library')),
        );
        SharedPreferences.setMockInitialValues({});
        ingestion = PhysicalGameHistoryIngestion(
          bleService: bleService,
          repository: repository,
          receiveDirectory: Directory('${root.path}/receive'),
          playerProfiles: await LocalPlayerProfileStore.open(),
          clock: () => DateTime.utc(2026, 9, 14),
        )..start();
        await ingestion.idle;
      });
      expect((await tester.runAsync(repository.load))!.games, isEmpty);
      expect(bleService.archiveAcknowledgements, 2);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitDisconnect();
        bleService.emitConnect();
      });
      expect((await tester.runAsync(repository.load))!.games, isEmpty);
      expect(bleService.archiveAcknowledgements, 3);
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.installLine(_ongoingClassic(deletedId)),
      );
      expect((await tester.runAsync(repository.load))!.games, isEmpty);
      expect(bleService.archiveAcknowledgements, 4);

      final assigned = (await tester.runAsync(
        () => ingestion.assignPlayer(
          boardId: 'board-1',
          side: LocalPlayerChessSide.white,
          displayName: 'Alex',
        ),
      ))!;
      expect(
        assigned.selectedPlayer(LocalPlayerChessSide.white)?.displayName,
        'Alex',
      );
      expect((await tester.runAsync(repository.load))!.games, isEmpty);
      expect(bleService.archiveAcknowledgements, 5);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitNewGameApplied();
        bleService.installLine(_ongoingClassic(nextId).take(2).toList());
      });
      final captured = (await tester.runAsync(repository.load))!.games.single;
      expect(captured.boardGameId, nextId);
      expect(captured.currentPly, 1);
      expect(
        captured.whitePlayer.playerId,
        assigned.selectedPlayer(LocalPlayerChessSide.white)!.id,
      );
      expect(bleService.archiveAcknowledgements, 6);
      expect(bleService.controlDispatches, 0);
    },
  );

  testWidgets(
    'unreadable deletion intent cannot acknowledge or recreate the game',
    (tester) async {
      const gameId = '707172737475767778797a7b7c7d7e7f';
      bleService.installLine(_ongoingClassic(gameId).take(2).toList());
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      expect(bleService.archiveAcknowledgements, 1);
      final marker = (await tester.runAsync(() async {
        await repository.delete('board-$gameId');
        final files = await Directory(
          '${root.path}/library/.board-game-deletions-v1',
        ).list().toList();
        final file = files.single as File;
        await file.writeAsString('unreadable-intent');
        return file;
      }))!;

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitDisconnect();
        bleService.emitConnect();
      });

      expect(bleService.archiveAcknowledgements, 1);
      expect((await tester.runAsync(repository.load))!.games, isEmpty);
      expect(await tester.runAsync(marker.readAsString), 'unreadable-intent');
    },
  );

  testWidgets(
    'mounted library card marks a captured physical game unfinished when the same board starts another',
    (tester) async {
      const previousId = 'd0d1d2d3d4d5d6d7d8d9dadbdcdddedf';
      const nextId = 'e0e1e2e3e4e5e6e7e8e9eaebecedeeef';
      bleService.installLine(_ongoingClassic(previousId).take(2).toList());
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      final captured = (await tester.runAsync(repository.load))!;
      expect(captured.games.single.status, GameArchiveStatus.active);
      await tester.runAsync(repository.loadCatalog);
      await tester.runAsync(() => repository.loadWindow(offset: 0, limit: 49));
      await tester.runAsync(
        () => tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: GameLibraryScreen(repository: repository),
          ),
        ),
      );
      try {
        await _settleRepositoryWidget(tester);
        final previousCard = find.byKey(
          const ValueKey<String>('game-library-game-board-$previousId'),
        );
        expect(previousCard, findsOneWidget);
        expect(
          find.descendant(
            of: previousCard,
            matching: find.text('Game in progress'),
          ),
          findsOneWidget,
        );

        bleService.clearLine();
        await _emitAndDrain(tester, ingestion, bleService.emitNewGameApplied);
        bleService.installLine(_ongoingClassic(nextId).take(2).toList());
        await _emitAndDrain(tester, ingestion, bleService.emitCurrentHomeState);
        await _settleRepositoryWidget(tester);
        expect(
          find.byKey(const ValueKey<String>('game-library-game-board-$nextId')),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: previousCard,
            matching: find.text('Unfinished game'),
          ),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: previousCard,
            matching: find.text('Game in progress'),
          ),
          findsNothing,
        );
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await _settleRepositoryWidget(tester);
        await tester.runAsync(repository.close);
      }
    },
    timeout: const Timeout(Duration(seconds: 15)),
  );

  testWidgets(
    'midgame attach keeps the complete line and captures authoritative Undo',
    (WidgetTester tester) async {
      const String gameId = 'a0a1a2a3a4a5a6a7a8a9aaabacadaeaf';
      bleService.installLine(_ongoingClassic(gameId));
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);

      final beforeUndo = (await tester.runAsync(repository.load))!;
      expect(beforeUndo.games.single.currentPly, 2);
      expect(
        beforeUndo.games.single.captureQuality,
        GameCaptureQuality.complete,
      );
      final opening = _ongoingClassic(gameId);
      final beforeLast = opening[1] as GameHistoryCommittedPlyRecord;
      bleService.installLine(<GameHistoryRecord>[
        opening.first,
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: gameId,
          lineRevision: 4,
          position: beforeLast.position,
          transition: beforeLast.transition,
        ),
      ]);
      await _emitAndDrain(tester, ingestion, bleService.emitCurrentHomeState);

      final reopened = GameLibraryRepository(
        GameLibraryStore(Directory('${root.path}/library')),
      );
      addTearDown(reopened.close);
      final durable = (await tester.runAsync(reopened.load))!;
      expect(durable.games.single.currentPly, 1);
      expect(durable.games.single.lineRevision, 4);
      expect(durable.games.single.positions.first.ply, 0);
      expect(durable.games.single.captureQuality, GameCaptureQuality.complete);
      expect(bleService.archiveAcknowledgements, 2);
    },
  );

  testWidgets(
    'Home ahead of a deferred save waits for durable line publication',
    (WidgetTester tester) async {
      const String gameId = '606162636465666768696a6b6c6d6e6f';
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      bleService.installLine(_ongoingClassic(gameId).take(2).toList());
      await _emitAndDrain(tester, ingestion, bleService.emitCurrentHomeState);
      expect(bleService.capabilityReads, 2);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitCurrentHomeState(_ongoingClassic(gameId));
        bleService.emitCurrentHomeState(_ongoingClassic(gameId));
      });
      expect(bleService.capabilityReads, 2);
      final beforeSave = (await tester.runAsync(repository.load))!;
      expect(beforeSave.games.single.currentPly, 1);

      // Worker completion changes the durable offer without another Home.
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.installLine(_ongoingClassic(gameId)),
      );
      final afterSave = (await tester.runAsync(repository.load))!;
      expect(afterSave.games.single.currentPly, 2);
      expect(bleService.archiveAcknowledgements, 2);
    },
  );

  test(
    'line publications cannot pull outside the current foreground session',
    () async {
      Future<void> emitAndDrain(VoidCallback emit) async {
        emit();
        await ingestion.idle;
      }

      await emitAndDrain(bleService.emitConnect);
      bleService.installLine(
        _ongoingClassic('707172737475767778797a7b7c7d7e7f'),
        publish: false,
      );
      await emitAndDrain(() {
        bleService.emitLineState(present: false);
        bleService.emitCurrentHomeState();
        bleService.emitNewGameApplied();
        bleService.emitArchiveRequired();
      });
      expect(bleService.capabilityReads, 1);
      ingestion.setForegroundActive(false);
      await emitAndDrain(bleService.emitLineState);
      expect(bleService.capabilityReads, 1);
      expect(bleService.archiveAcknowledgements, 0);
      await emitAndDrain(() => ingestion.setForegroundActive(true));
      expect(bleService.capabilityReads, 2);
      bleService.advanceSessionWithoutConnectionEvent();
      await emitAndDrain(bleService.emitLineState);
      expect(bleService.capabilityReads, 2);
      await emitAndDrain(() {
        bleService.emitDisconnect();
        bleService.emitLineState();
      });
      expect(bleService.capabilityReads, 2);
      await ingestion.close();
      await emitAndDrain(() {
        bleService.emitConnect();
        bleService.emitLineState();
      });
      expect(bleService.capabilityReads, 2);
      expect(bleService.archiveAcknowledgements, 1);
      expect(bleService.controlDispatches, 0);
    },
  );

  testWidgets(
    'older firmware keeps terminal and refusal triggers until State45 proves support for each connection',
    (tester) async {
      const oldId = 'b0b1b2b3b4b5b6b7b8b9babbbcbdbebf';
      const nextId = 'c0c1c2c3c4c5c6c7c8c9cacbcccdcecf';
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      bleService.installLine(
        _ongoingClassic(oldId).take(2).toList(),
        publish: false,
      );
      await _emitAndDrain(tester, ingestion, bleService.emitArchiveRequired);
      expect(bleService.capabilityReads, 2);
      bleService.installLine(_completedClassic(oldId), publish: false);
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.emitTerminalHomeState(variant: 'c'),
      );
      expect(bleService.capabilityReads, 3);

      // A valid absent publication proves support too. It is not a lost game
      // or a request to probe the previous line.
      await _emitAndDrain(tester, ingestion, bleService.clearLine);
      bleService.installLine(
        _ongoingClassic(nextId).take(2).toList(),
        publish: false,
      );
      await _emitAndDrain(tester, ingestion, () {
        bleService.emitTerminalHomeState(variant: 'c');
        bleService.emitArchiveRequired();
      });
      expect(bleService.capabilityReads, 3);
      await _emitAndDrain(tester, ingestion, bleService.emitLineState);
      expect(bleService.capabilityReads, 4);
      expect(bleService.archiveAcknowledgements, 3);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitDisconnect();
        bleService.emitConnect();
      });
      bleService.installLine(_completedClassic(nextId), publish: false);
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.emitTerminalHomeState(variant: 'c'),
      );
      expect(bleService.capabilityReads, 6);
      expect(bleService.archiveAcknowledgements, 5);
    },
  );

  testWidgets(
    'durable publications coalesce while capability discovery is pending',
    (WidgetTester tester) async {
      const String gameId = '202122232425262728292a2b2c2d2e2f';
      bleService.installLine(_completedClassic(gameId));
      bleService.capabilityDelay = const Duration(milliseconds: 10);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitConnect();
        bleService.emitLineState();
        bleService.emitLineState();
      });
      expect(bleService.capabilityReads, 1);
      expect(bleService.archiveAcknowledgements, 1);

      bleService.capabilityDelay = Duration.zero;
      await _emitAndDrain(tester, ingestion, () {
        bleService.emitDisconnect();
        bleService.emitConnect();
      });
      expect(bleService.capabilityReads, 2);
      final snapshot = (await tester.runAsync(repository.load))!;
      expect(snapshot.games, hasLength(1));
    },
  );

  testWidgets(
    'reconnect during capability discovery drains the new session before idle',
    (WidgetTester tester) async {
      const String gameId = '303132333435363738393a3b3c3d3e3f';
      bleService.installLine(_completedClassic(gameId));
      final Completer<void> capabilityGate = Completer<void>();
      bleService.capabilityGate = capabilityGate;

      bleService.emitConnect();
      await tester.pump();
      expect(bleService.capabilityReads, 1);
      bleService.emitDisconnect();
      bleService.emitConnect();
      await tester.pump();
      await tester.runAsync(() async {
        capabilityGate.complete();
        await ingestion.idle.timeout(
          const Duration(seconds: 2),
          onTimeout: () => throw StateError(
            'history drain stalled: capabilities=${bleService.capabilityReads} '
            'acks=${bleService.archiveAcknowledgements}',
          ),
        );
      });

      expect(bleService.capabilityReads, 2);
      expect(bleService.archiveAcknowledgements, 1);
      final GameLibrarySnapshot snapshot = (await tester.runAsync(
        repository.load,
      ))!;
      expect(snapshot.games.single.boardGameId, gameId);
    },
  );

  testWidgets(
    'terminal Duck event while connected is pulled and reconnect stays idempotent',
    (WidgetTester tester) async {
      const String gameId = '101112131415161718191a1b1c1d1e1f';
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);

      bleService.installLine(_completedDuck(gameId));
      final Future<GameLibrarySnapshot> changed = repository.changes.first;
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.emitTerminalHomeState(variant: 'd'),
      );
      final GameLibrarySnapshot notification = (await tester.runAsync(
        () => changed,
      ))!;
      expect(notification.games.single.boardGameId, gameId);
      final GameLibrarySnapshot duckLibrary = (await tester.runAsync(
        repository.load,
      ))!;
      expect(duckLibrary.games.single.variant, GameVariant.duck);

      await _emitAndDrain(tester, ingestion, () {
        bleService.emitDisconnect();
        bleService.emitConnect();
      });
      final GameLibrarySnapshot afterReconnect = (await tester.runAsync(
        repository.load,
      ))!;
      expect(afterReconnect.games, hasLength(1));
      final GameLibraryWindow visibleWindow = (await tester.runAsync(
        () => repository.loadWindow(offset: 0, limit: 5),
      ))!;
      expect(visibleWindow.games.single.libraryId, 'board-$gameId');

      await tester.pumpWidget(_recentHarness(repository));
      await _settleRepositoryWidget(tester);
      expect(
        find.byKey(ValueKey<String>('recent-game-board-$gameId')),
        findsOneWidget,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await _settleRepositoryWidget(tester);
      await tester.runAsync(repository.close);
    },
  );

  testWidgets(
    'already-open empty Recent Games adds later Classic and Duck completions',
    (WidgetTester tester) async {
      final GameLibraryWindow initialWindow = (await tester.runAsync(
        () => repository.loadWindow(offset: 0, limit: 5),
      ))!;
      expect(initialWindow.games, isEmpty);
      await tester.pumpWidget(_recentHarness(repository));
      await _settleRepositoryWidget(tester);
      await tester.pumpAndSettle();
      await tester.runAsync(repository.loadCatalog);
      final State<StatefulWidget> mountedSection = tester.state(
        find.byType(RecentGamesSection),
      );
      expect(
        find.descendant(
          of: find.byType(RecentGamesSection),
          matching: find.byType(ListTile),
        ),
        findsNothing,
      );

      bleService.installLine(
        _completedClassic('404142434445464748494a4b4c4d4e4f'),
      );
      await _emitAndDrain(tester, ingestion, bleService.emitConnect);
      await _settleRepositoryWidget(tester);
      expect(
        tester.state(find.byType(RecentGamesSection)),
        same(mountedSection),
      );
      expect(
        find.descendant(
          of: find.byType(RecentGamesSection),
          matching: find.byType(ListTile),
        ),
        findsOneWidget,
      );

      bleService.installLine(
        _completedDuck('505152535455565758595a5b5c5d5e5f'),
      );
      await _emitAndDrain(
        tester,
        ingestion,
        () => bleService.emitTerminalHomeState(variant: 'd'),
      );
      final GameLibraryWindow updatedWindow = (await tester.runAsync(
        () => repository.loadWindow(offset: 0, limit: 5),
      ))!;
      expect(updatedWindow.games, hasLength(2));
      await _settleRepositoryWidget(tester);

      expect(
        tester.state(find.byType(RecentGamesSection)),
        same(mountedSection),
      );
      expect(
        find.descendant(
          of: find.byType(RecentGamesSection),
          matching: find.byType(ListTile),
        ),
        findsNWidgets(2),
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await _settleRepositoryWidget(tester);
    },
  );
}

Future<void> _emitAndDrain(
  WidgetTester tester,
  PhysicalGameHistoryIngestion ingestion,
  VoidCallback emit,
) async {
  emit();
  await tester.pump();
  await tester.runAsync(() => ingestion.idle);
}

Future<void> _settleRepositoryWidget(WidgetTester tester) async {
  for (int pass = 0; pass < 12; pass += 1) {
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 20)),
    );
    await tester.pump();
  }
}

Widget _recentHarness(GameLibraryRepository repository) => MaterialApp(
  theme: buildLitchTheme(),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Scaffold(
    body: RecentGamesSection(repository: repository, onOpenAll: () {}),
  ),
);

List<GameHistoryRecord> _completedClassic(String gameId) => <GameHistoryRecord>[
  GameHistoryLineBaseRecord(
    outboxSequence: 1,
    boardGameId: gameId,
    lineRevision: 1,
    variant: GameVariant.classic,
    position: const ArchivedPosition(
      ply: 0,
      fen:
          'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
          'RNBQKBNR w KQkq - 0 1',
    ),
  ),
  GameHistoryCommittedPlyRecord(
    outboxSequence: 2,
    boardGameId: gameId,
    lineRevision: 2,
    position: const ArchivedPosition(
      ply: 1,
      fen:
          'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
          'RNBQKBNR b KQkq e3 0 1',
    ),
    transition: const ArchivedTransition(
      side: GameSide.white,
      from: 52,
      to: 36,
    ),
  ),
  GameHistorySealRecord(
    outboxSequence: 3,
    boardGameId: gameId,
    lineRevision: 3,
    status: GameArchiveStatus.completed,
    outcome: GameOutcome.checkmate,
    winner: GameWinner.white,
  ),
];

List<GameHistoryRecord> _ongoingClassic(String gameId) => <GameHistoryRecord>[
  ..._completedClassic(gameId).take(2),
  GameHistoryCommittedPlyRecord(
    outboxSequence: 3,
    boardGameId: gameId,
    lineRevision: 3,
    position: const ArchivedPosition(
      ply: 2,
      fen:
          'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/'
          'RNBQKBNR w KQkq e6 0 2',
    ),
    transition: const ArchivedTransition(
      side: GameSide.black,
      from: 12,
      to: 28,
    ),
  ),
];

List<GameHistoryRecord> _completedDuck(String gameId) => <GameHistoryRecord>[
  GameHistoryLineBaseRecord(
    outboxSequence: 1,
    boardGameId: gameId,
    lineRevision: 1,
    variant: GameVariant.duck,
    position: const ArchivedPosition(
      ply: 0,
      fen:
          'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
          'RNBQKBNR w KQkq - 0 1',
      duckSquare: 27,
    ),
  ),
  GameHistoryCommittedPlyRecord(
    outboxSequence: 2,
    boardGameId: gameId,
    lineRevision: 2,
    position: const ArchivedPosition(
      ply: 1,
      fen:
          'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
          'RNBQKBNR b KQkq e3 0 1',
      duckSquare: 28,
    ),
    transition: const ArchivedTransition(
      side: GameSide.white,
      from: 52,
      to: 36,
      duckFrom: 27,
      duckTo: 28,
    ),
  ),
  GameHistorySealRecord(
    outboxSequence: 3,
    boardGameId: gameId,
    lineRevision: 3,
    status: GameArchiveStatus.completed,
    outcome: GameOutcome.fowled,
    winner: GameWinner.black,
  ),
];

class _HistoryBleService extends LitchBleService {
  _HistoryBleService()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _UnusedBle());

  static const LitchDeviceCapabilities supportedCapabilities =
      LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask: LitchDeviceCapabilities.gameHistoryBit,
        explicitlyReported: true,
      );

  final StreamController<ConnectionStateUpdate> _connections =
      StreamController<ConnectionStateUpdate>.broadcast();
  final StreamController<HomeStateNotification> _home =
      StreamController<HomeStateNotification>.broadcast();
  final StreamController<List<int>> _packets =
      StreamController<List<int>>.broadcast();
  final StreamController<CurrentGameLineState> _lineStates =
      StreamController<CurrentGameLineState>.broadcast();
  final StreamController<LitchControlEvent> _control =
      StreamController<LitchControlEvent>.broadcast();
  int controlDispatches = 0;
  bool _connected = false;
  bool ownerReady = true;
  final _readiness = StreamController<bool>.broadcast();
  @override
  bool get productReady => _connected && ownerReady;
  @override
  Stream<bool> get productReadinessStream => _readiness.stream;
  void emitReadiness(bool ready) {
    ownerReady = ready;
    _readiness.add(ready);
  }

  int _session = 0;
  _OfferedLine? _line;
  int archiveAcknowledgements = 0;
  int capabilityReads = 0;
  Duration capabilityDelay = Duration.zero;
  Completer<void>? capabilityGate;

  @override
  Stream<ConnectionStateUpdate> get connectionStream => _connections.stream;

  @override
  Stream<HomeStateNotification> get homeStateStream => _home.stream;

  @override
  Stream<LitchControlEvent> get controlActionEvents => _control.stream;

  @override
  Stream<CurrentGameLineState> get currentGameLineStream => _lineStates.stream;

  @override
  Future<LitchControlEvent> dispatchControlAction({
    required String deviceId,
    required LitchControlAction action,
    int undoCount = 1,
    LitchControlOwner? owner,
    bool? enabled,
  }) async {
    controlDispatches += 1;
    throw StateError('Ingestion must never dispatch or replay a board action.');
  }

  @override
  bool get isConnected => _connected;

  @override
  String? get connectedId => _connected ? 'board-1' : null;

  @override
  int get connectionSessionToken => _session;

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      supportedCapabilities;

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    capabilityReads += 1;
    await capabilityGate?.future;
    if (capabilityDelay > Duration.zero) {
      await Future<void>.delayed(capabilityDelay);
    }
    return supportedCapabilities;
  }

  @override
  Stream<List<int>> gameHistoryPackets(String deviceId) => _packets.stream;

  @override
  Future<void> writeGameHistoryPacket(String deviceId, List<int> bytes) async {
    final GameHistoryFrame request = GameHistoryFrame.decode(
      Uint8List.fromList(bytes),
    );
    final _OfferedLine? line = _line;
    switch (request.opcode) {
      case 0x20:
        if (line == null) {
          _emit(
            request,
            CurrentGameSyncOpcode.state,
            1,
            const CurrentGameDigest.absent().encode(allowAbsent: true),
          );
          _emit(
            request,
            CurrentGameSyncOpcode.bounds,
            2,
            const CurrentGameBounds.absent().encode(),
          );
          return;
        }
        _emit(
          request,
          CurrentGameSyncOpcode.state,
          1,
          line.digest.encode(allowAbsent: false),
        );
        _emit(request, CurrentGameSyncOpcode.bounds, 2, line.bounds.encode());
        final CurrentGameResumeCursor cursor = CurrentGameResumeCursor.decode(
          request.payload,
        );
        if (cursor.nextRecordSequence > 0 &&
            _sameBytes(
              cursor.prefixFingerprint,
              line.digest.fingerprint.take(8).toList(growable: false),
            )) {
          return;
        }
        _emit(
          request,
          CurrentGameSyncOpcode.segmentBegin,
          3,
          line.descriptor.encode(),
        );
        _emit(request, CurrentGameSyncOpcode.data, 4, line.bytes);
      case 0x25:
        _emit(
          request,
          CurrentGameSyncOpcode.segmentCommitted,
          request.frameSequence + 1,
          request.payload,
        );
      case 0x27:
        archiveAcknowledgements += 1;
        _emit(
          request,
          CurrentGameSyncOpcode.archived,
          request.frameSequence + 1,
          request.payload,
        );
      default:
        throw StateError('Unexpected current-game opcode ${request.opcode}.');
    }
  }

  void installLine(List<GameHistoryRecord> records, {bool publish = true}) {
    _line = _OfferedLine.fromRecords(records);
    if (publish) emitLineState();
  }

  int _lineRevision = 0;
  void emitLineState({bool present = true}) => _lineStates.add(
    CurrentGameLineState(revision: ++_lineRevision, present: present),
  );

  void emitConnect() {
    _connected = true;
    _session += 1;
    _connections.add(
      ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
  }

  void emitDisconnect() {
    _connected = false;
    _connections.add(
      ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
  }

  void advanceSessionWithoutConnectionEvent() {
    _session += 1;
  }

  void emitControl(LitchControlEvent event) => _control.add(event);

  void emitArchiveRequired() => emitControl(
    const LitchControlEvent(
      requestId: 19,
      action: LitchControlAction.newGame,
      phase: LitchControlPhase.rejected,
      reason: LitchMutationReason.archiveRequired,
    ),
  );

  void clearLine() {
    _line = null;
    emitLineState(present: false);
  }

  void emitNewGameApplied() => emitControl(
    const LitchControlEvent(
      requestId: 20,
      action: LitchControlAction.newGame,
      phase: LitchControlPhase.applied,
      reason: LitchMutationReason.none,
    ),
  );

  void emitCurrentHomeState([List<GameHistoryRecord>? liveRecords]) {
    final records =
        liveRecords ??
        GameHistoryCodec.decodeSegment(_line!.bytes, _line!.descriptor);
    final last = records.last;
    final position = switch (last) {
      GameHistoryLineBaseRecord record => record.position,
      GameHistoryCommittedPlyRecord record => record.position,
      _ => throw StateError('Ongoing fixture contains a seal.'),
    };
    final state = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":${++_homeSequence},"a":"idle",'
      '"fen":"${position.fen}","occ":"0000000000000000",'
      '"g":"c","duck":"--","out":"n","win":"n",'
      '"rot":0,"white":1,"pm":0,"fs":0}',
    )!;
    _home.add(state);
  }

  int _homeSequence = 1;

  void emitTerminalHomeState({required String variant}) {
    final HomeStateNotification state = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":1,"a":"idle",'
      '"fen":"8/8/8/8/8/8/8/8 w - - 0 1",'
      '"occ":"0000000000000000","g":"$variant","duck":'
      '${variant == 'd' ? '"e4"' : '"--"'},"out":"k","win":"w",'
      '"rot":0,"white":1,"pm":0,"fs":0}',
    )!;
    _home.add(state);
  }

  void _emit(
    GameHistoryFrame request,
    CurrentGameSyncOpcode opcode,
    int sequence,
    Uint8List payload,
  ) {
    scheduleMicrotask(() {
      _packets.add(
        GameHistoryFrame(
          opcode: opcode.wireValue,
          transactionId: request.transactionId,
          frameSequence: sequence,
          payload: payload,
        ).encode(),
      );
    });
  }

  @override
  Future<void> dispose() async {
    await _connections.close();
    await _readiness.close();
    await _home.close();
    await _control.close();
    await _lineStates.close();
    await _packets.close();
    await super.dispose();
  }
}

bool _sameBytes(List<int> left, List<int> right) {
  if (left.length != right.length) return false;
  for (int index = 0; index < left.length; index += 1) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

class _OfferedLine {
  const _OfferedLine({
    required this.bytes,
    required this.descriptor,
    required this.digest,
    required this.bounds,
  });

  factory _OfferedLine.fromRecords(List<GameHistoryRecord> records) {
    final BytesBuilder builder = BytesBuilder(copy: false);
    for (final GameHistoryRecord record in records) {
      builder.add(GameHistoryCodec.encodeRecord(record));
    }
    final Uint8List bytes = builder.takeBytes();
    return _OfferedLine(
      bytes: bytes,
      descriptor: GameHistorySegmentDescriptor(
        encodedBytes: bytes.length,
        recordCount: records.length,
        firstOutboxSequence: 1,
        crc32: GameHistoryCrc32.compute(bytes),
      ),
      digest: CurrentGameDigest(
        fingerprint: sha256.convert(bytes).bytes.take(12).toList(),
      ),
      bounds: CurrentGameBounds(
        lineRevision: records.last.lineRevision,
        recordCount: records.length,
        flags:
            currentGameSyncPresentFlag |
            currentGameSyncArchiveRequiredFlag |
            (records.last is GameHistorySealRecord
                ? currentGameSyncSealedFlag
                : 0),
      ),
    );
  }

  final Uint8List bytes;
  final GameHistorySegmentDescriptor descriptor;
  final CurrentGameDigest digest;
  final CurrentGameBounds bounds;
}

class _UnusedBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}
