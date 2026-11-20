import 'dart:async';

import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../models/game_archive.dart';
import '../screens/game_archive_labels.dart';
import '../screens/game_library_screen.dart';
import '../services/game_library_repository.dart';
import '../services/game_library_store.dart';
import 'game_position_thumbnail.dart';
import 'home_notice_presenter.dart';

class RecentGamesSection extends StatefulWidget {
  const RecentGamesSection({
    super.key,
    this.repository,
    this.openRepository,
    required this.onOpenAll,
  }) : assert(repository != null || openRepository != null);

  final GameLibraryRepository? repository;
  final GameLibraryOpener? openRepository;
  final VoidCallback onOpenAll;

  @override
  State<RecentGamesSection> createState() => _RecentGamesSectionState();
}

class _RecentGamesSectionState extends State<RecentGamesSection> {
  final HomeNoticePresenter _notices = HomeNoticePresenter();
  ScaffoldFeatureController<SnackBar, SnackBarClosedReason>? _deleteNotice;

  void _showDeleteNotice(SnackBar notice) {
    if (!mounted || ModalRoute.of(context)?.isCurrent == false) return;
    _deleteNotice = showGameLibraryDeleteNotice(context, _notices, notice);
  }

  void _retireDeleteNotice({bool afterUnmount = false}) {
    final notice = _deleteNotice;
    _deleteNotice = null;
    if (afterUnmount) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _notices.removeNotice(notice);
      });
    } else {
      _notices.removeNotice(notice);
    }
  }

  void _openAllGames() {
    _retireDeleteNotice();
    widget.onOpenAll();
  }

  GameLibraryRepository? _repository;
  GameLibrarySnapshot _snapshot = GameLibrarySnapshot();
  StreamSubscription<GameLibrarySnapshot>? _changes;
  bool _loading = true;
  Object? _error;
  int _generation = 0;

  @override
  void initState() {
    super.initState();
    _repository = widget.repository;
    if (_repository case final GameLibraryRepository repository) {
      _attach(repository);
    } else {
      unawaited(_open());
    }
  }

  @override
  void didUpdateWidget(RecentGamesSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.repository != null && widget.repository != _repository) {
      _retireDeleteNotice();
      _generation += 1;
      unawaited(_changes?.cancel());
      _repository = widget.repository;
      _attach(widget.repository!);
    } else if (widget.repository == null &&
        oldWidget.openRepository != widget.openRepository &&
        _repository == null) {
      unawaited(_open());
    }
  }

  void _attach(GameLibraryRepository repository) {
    _snapshot = GameLibrarySnapshot();
    _error = null;
    _loading = true;
    _changes = repository.changes.listen((GameLibrarySnapshot change) {
      scheduleMicrotask(() {
        if (!mounted || repository != _repository) return;
        if (change.games.isEmpty) {
          unawaited(_load(repository));
          return;
        }
        final Map<String, GameArchive> merged = <String, GameArchive>{
          for (final GameArchive game in _snapshot.games) game.libraryId: game,
          for (final GameArchive game in change.games) game.libraryId: game,
        };
        final List<GameArchive> recent = merged.values.toList()
          ..sort((GameArchive left, GameArchive right) {
            final int byUpdatedAt = right.updatedAt.compareTo(left.updatedAt);
            return byUpdatedAt != 0
                ? byUpdatedAt
                : left.libraryId.compareTo(right.libraryId);
          });
        _onSnapshot(
          repository,
          GameLibrarySnapshot(
            games: recent.take(5),
            recoveredFiles: _snapshot.recoveredFiles,
            rejectedFiles: _snapshot.rejectedFiles,
          ),
        );
      });
    });
    unawaited(_load(repository));
  }

  void _onSnapshot(
    GameLibraryRepository repository,
    GameLibrarySnapshot snapshot,
  ) {
    if (!mounted || repository != _repository) return;
    setState(() {
      _snapshot = snapshot;
      _loading = false;
      _error = null;
    });
  }

  Future<void> _open() async {
    final GameLibraryOpener? opener = widget.openRepository;
    if (opener == null) return;
    final int generation = ++_generation;
    if (mounted) {
      setState(() {
        _loading = true;
        _error = null;
      });
    }
    try {
      final GameLibraryRepository repository = await opener();
      if (!mounted || generation != _generation) return;
      _repository = repository;
      _attach(repository);
    } catch (error) {
      if (!mounted || generation != _generation) return;
      setState(() {
        _loading = false;
        _error = error;
      });
    }
  }

  Future<void> _load(GameLibraryRepository repository) async {
    final int generation = ++_generation;
    try {
      GameLibraryCatalog catalog = await repository.loadCatalog();
      GameLibraryWindow window = catalog.totalCount == 0
          ? GameLibraryWindow(
              offset: 0,
              totalCount: 0,
              games: const [],
              generation: catalog.generation,
            )
          : await repository.loadWindow(offset: 0, limit: 5);
      if (window.generation != catalog.generation) {
        catalog = await repository.loadCatalog();
        window = catalog.totalCount == 0
            ? GameLibraryWindow(
                offset: 0,
                totalCount: 0,
                games: const [],
                generation: catalog.generation,
              )
            : await repository.loadWindow(offset: 0, limit: 5);
        if (window.generation != catalog.generation) {
          throw StateError('Game library changed while loading.');
        }
      }
      if (!mounted || generation != _generation || repository != _repository) {
        return;
      }
      _onSnapshot(
        repository,
        GameLibrarySnapshot(
          games: window.games,
          recoveredFiles: catalog.recoveredFiles,
          rejectedFiles: catalog.rejectedFiles,
        ),
      );
    } catch (error) {
      if (!mounted || generation != _generation || repository != _repository) {
        return;
      }
      setState(() {
        _loading = false;
        _error = error;
      });
    }
  }

  void _retry() {
    final GameLibraryRepository? repository = _repository;
    if (repository != null) {
      setState(() {
        _loading = true;
        _error = null;
      });
      unawaited(_load(repository));
    } else {
      unawaited(_open());
    }
  }

  void _openGame(GameArchive archive) {
    final GameLibraryRepository? repository = _repository;
    if (repository == null) return;
    _retireDeleteNotice();
    Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        builder: (_) => GameLibraryDetailScreen(
          key: ValueKey<String>(
            'game-library-detail:${archive.libraryId}:${archive.boardGameId}',
          ),
          repository: repository,
          initialArchive: archive,
          libraryId: archive.libraryId,
          gameId: archive.gameId,
          origin: archive.origin,
        ),
      ),
    );
  }

  Future<void> _deleteGame(GameArchive archive) async {
    final GameLibraryRepository? repository = _repository;
    if (repository == null) return;
    await deleteGameArchiveWithUndo(
      context,
      repository,
      archive,
      onNotice: _showDeleteNotice,
    );
  }

  @override
  void dispose() {
    _retireDeleteNotice(afterUnmount: true);
    _generation += 1;
    unawaited(_changes?.cancel());
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final List<GameArchive> recent = _snapshot.games
        .take(3)
        .toList(growable: false);
    final bool hasRecoveryNotice =
        _snapshot.recoveredFiles.isNotEmpty ||
        _snapshot.rejectedFiles.isNotEmpty;

    return Card(
      key: const ValueKey<String>('recent-games-section'),
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 16, 14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Wrap(
              alignment: WrapAlignment.spaceBetween,
              crossAxisAlignment: WrapCrossAlignment.center,
              spacing: 12,
              children: <Widget>[
                Text(
                  strings.recentGamesTitle,
                  style: Theme.of(context).textTheme.titleLarge,
                ),
                TextButton(
                  key: const ValueKey<String>('game-library-action'),
                  onPressed: _openAllGames,
                  style: TextButton.styleFrom(minimumSize: const Size(0, 48)),
                  child: Text(strings.seeAllGamesAction),
                ),
              ],
            ),
            if (!_loading && _error == null && hasRecoveryNotice) ...<Widget>[
              const SizedBox(height: 4),
              Semantics(
                button: true,
                onTap: _openAllGames,
                label: strings.gameLibraryRecoverySummary(
                  _snapshot.recoveredFiles.length,
                  _snapshot.rejectedFiles.length,
                ),
                child: ExcludeSemantics(
                  child: InkWell(
                    key: const ValueKey<String>(
                      'recent-games-recovery-warning',
                    ),
                    borderRadius: BorderRadius.circular(12),
                    onTap: _openAllGames,
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(minHeight: 48),
                      child: Ink(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 12,
                          vertical: 10,
                        ),
                        decoration: BoxDecoration(
                          color: Theme.of(context).colorScheme.errorContainer,
                          borderRadius: BorderRadius.circular(12),
                        ),
                        child: Row(
                          children: <Widget>[
                            Icon(
                              Icons.warning_amber_rounded,
                              color: Theme.of(
                                context,
                              ).colorScheme.onErrorContainer,
                            ),
                            const SizedBox(width: 10),
                            Expanded(
                              child: Text(
                                strings.recentGamesRecoveryNotice,
                                style: TextStyle(
                                  color: Theme.of(
                                    context,
                                  ).colorScheme.onErrorContainer,
                                ),
                              ),
                            ),
                            const SizedBox(width: 8),
                            Icon(
                              Icons.chevron_right_rounded,
                              color: Theme.of(
                                context,
                              ).colorScheme.onErrorContainer,
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 8),
            ],
            if (_loading) ...<Widget>[
              const SizedBox(height: 8),
              Semantics(
                liveRegion: true,
                label: strings.gameLibraryLoading,
                child: ExcludeSemantics(
                  child: Text(strings.gameLibraryLoading),
                ),
              ),
            ] else if (_error != null) ...<Widget>[
              Text(strings.gameLibraryUnavailableBody),
              const SizedBox(height: 4),
              Align(
                alignment: AlignmentDirectional.centerStart,
                child: TextButton.icon(
                  key: const ValueKey<String>('recent-games-retry'),
                  onPressed: _retry,
                  style: TextButton.styleFrom(minimumSize: const Size(0, 48)),
                  icon: const Icon(Icons.refresh_rounded),
                  label: Text(strings.tryAgain),
                ),
              ),
            ] else if (recent.isEmpty) ...<Widget>[
              Text(
                _snapshot.rejectedFiles.isNotEmpty
                    ? strings.gameLibraryUnavailableTitle
                    : strings.gameLibraryEmptyBody,
              ),
            ] else ...<Widget>[
              for (int index = 0; index < recent.length; index++) ...<Widget>[
                if (index > 0) const Divider(height: 1),
                _RecentGameTile(
                  archive: recent[index],
                  onTap: () => _openGame(recent[index]),
                  onDelete: () => _deleteGame(recent[index]),
                ),
              ],
            ],
          ],
        ),
      ),
    );
  }
}

class _RecentGameTile extends StatelessWidget {
  const _RecentGameTile({
    required this.archive,
    required this.onTap,
    required this.onDelete,
  });

  final GameArchive archive;
  final VoidCallback onTap;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final String result = gameArchiveResultLabel(context, archive);
    final String date = MaterialLocalizations.of(
      context,
    ).formatMediumDate(archive.updatedAt.toLocal());
    final String moves = strings.gameLibraryMoveCount(
      archive.transitions.length,
    );
    final String title = archive.displayName ?? result;
    final String variant = gameArchiveVariantLabel(context, archive.variant);
    final String openLabel = strings.gameLibraryGameSemantics(
      date,
      variant,
      result,
      archive.captureQuality == GameCaptureQuality.complete
          ? strings.gameLibraryCompleteReplay
          : strings.gameLibraryPartialReplay,
      moves,
    );
    final details = ListTile(
      contentPadding: EdgeInsets.zero,
      minTileHeight: 64,
      title: Text(title),
      subtitle: Text(
        archive.displayName == null
            ? '${strings.gameLibrarySavedDate(date)} · $moves'
            : '$result · ${strings.gameLibrarySavedDate(date)} · $moves',
      ),
    );
    return Row(
      children: <Widget>[
        Expanded(
          child: Semantics(
            button: true,
            onTap: onTap,
            label: archive.displayName == null
                ? openLabel
                : '$title. $openLabel',
            child: ExcludeSemantics(
              child: InkWell(
                key: ValueKey<String>('recent-game-${archive.libraryId}'),
                onTap: onTap,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  child: MediaQuery.textScalerOf(context).scale(14) >= 21
                      ? Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: <Widget>[
                            GamePositionThumbnail(archive: archive, size: 72),
                            const SizedBox(height: 10),
                            details,
                          ],
                        )
                      : Row(
                          children: <Widget>[
                            GamePositionThumbnail(archive: archive),
                            const SizedBox(width: 12),
                            Expanded(child: details),
                          ],
                        ),
                ),
              ),
            ),
          ),
        ),
        PopupMenuButton<String>(
          key: ValueKey<String>('recent-game-menu-${archive.libraryId}'),
          tooltip: strings.gameLibraryGameActions,
          onSelected: (String value) {
            if (value == 'delete') onDelete();
          },
          itemBuilder: (BuildContext context) => <PopupMenuEntry<String>>[
            PopupMenuItem<String>(
              value: 'delete',
              child: ListTile(
                contentPadding: EdgeInsets.zero,
                leading: const Icon(Icons.delete_outline_rounded),
                title: Text(strings.gameLibraryDeleteGame),
              ),
            ),
          ],
        ),
      ],
    );
  }
}
