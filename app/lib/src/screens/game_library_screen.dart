import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';

import '../localization/litch_strings.dart';
import '../models/game_archive.dart';
import '../services/game_library_repository.dart';
import '../services/game_library_store.dart';
import '../services/post_game_review_analyzer.dart';
import '../widgets/game_position_thumbnail.dart';
import '../widgets/home_notice_presenter.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/post_game_review_analyzer_scope.dart';
import 'game_archive_labels.dart';
import 'fen_preview_screen.dart';
import 'post_game_review_screen.dart';

typedef GameLibraryOpener = Future<GameLibraryRepository> Function();

ScaffoldFeatureController<SnackBar, SnackBarClosedReason>
showGameLibraryDeleteNotice(
  BuildContext context,
  HomeNoticePresenter notices,
  SnackBar notice,
) {
  // A new delete replaces older feedback. Finish that replacement now so
  // navigation can retire this notice without removing an older queue head.
  final ScaffoldMessengerState messenger = ScaffoldMessenger.of(context);
  messenger.clearSnackBars();
  messenger.removeCurrentSnackBar();
  return notices.showNotice(context, notice);
}

Future<void> deleteGameArchiveWithUndo(
  BuildContext context,
  GameLibraryRepository repository,
  GameArchive archive, {
  required void Function(SnackBar) onNotice,
}) async {
  final strings = litchStrings(context);
  final bool confirmed =
      await showDialog<bool>(
        context: context,
        builder: (BuildContext dialogContext) => AlertDialog(
          title: Text(strings.gameLibraryDeleteQuestion),
          content: Text(strings.gameLibraryDeleteBody),
          actions: <Widget>[
            LitchCompactControls(
              child: TextButton(
                onPressed: () => Navigator.of(dialogContext).pop(false),
                child: Text(strings.cancelAction),
              ),
            ),
            LitchCompactControls(
              child: FilledButton(
                key: const ValueKey<String>('game-library-delete-confirm'),
                onPressed: () => Navigator.of(dialogContext).pop(true),
                child: Text(strings.gameLibraryDeleteGame),
              ),
            ),
          ],
        ),
      ) ??
      false;
  if (!confirmed || !context.mounted) return;

  final GameArchive? removed;
  try {
    removed = await repository.delete(archive.libraryId);
  } catch (_) {
    if (!context.mounted) return;
    onNotice(SnackBar(content: Text(strings.gameLibraryDeleteFailed)));
    return;
  }
  if (!context.mounted) return;
  Future<void> undo() async {
    if (removed == null) return;
    try {
      await repository.restoreDeleted(removed);
    } catch (_) {
      if (!context.mounted) return;
      onNotice(
        SnackBar(
          content: Text(strings.gameLibraryRestoreFailed),
          action: SnackBarAction(label: strings.undoAction, onPressed: undo),
        ),
      );
    }
  }

  onNotice(
    SnackBar(
      content: Text(strings.gameLibraryDeleted),
      action: removed == null
          ? null
          : SnackBarAction(label: strings.undoAction, onPressed: undo),
    ),
  );
}

/// Keeps the Games entry available while app-local storage is still opening or
/// needs a retry. The repository remains owned by the app root.
class GameLibraryRouteScreen extends StatefulWidget {
  const GameLibraryRouteScreen({
    super.key,
    this.repository,
    this.openRepository,
  }) : assert(repository != null || openRepository != null);

  final GameLibraryRepository? repository;
  final GameLibraryOpener? openRepository;

  @override
  State<GameLibraryRouteScreen> createState() => _GameLibraryRouteScreenState();
}

class _GameLibraryRouteScreenState extends State<GameLibraryRouteScreen> {
  GameLibraryRepository? _repository;
  bool _opening = false;
  Object? _openError;
  int _openGeneration = 0;

  @override
  void initState() {
    super.initState();
    _repository = widget.repository;
    if (_repository == null) {
      unawaited(_open());
    }
  }

  @override
  void didUpdateWidget(GameLibraryRouteScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.repository != null && widget.repository != _repository) {
      _openGeneration++;
      setState(() {
        _repository = widget.repository;
        _opening = false;
        _openError = null;
      });
    }
  }

  Future<void> _open() async {
    final GameLibraryOpener? opener = widget.openRepository;
    if (opener == null || _opening) {
      return;
    }
    final int generation = ++_openGeneration;
    setState(() {
      _opening = true;
      _openError = null;
    });
    try {
      final GameLibraryRepository repository = await opener();
      if (!mounted || generation != _openGeneration) {
        return;
      }
      setState(() {
        _repository = repository;
        _opening = false;
      });
    } catch (error) {
      if (!mounted || generation != _openGeneration) {
        return;
      }
      setState(() {
        _opening = false;
        _openError = error;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final GameLibraryRepository? repository = _repository;
    if (repository != null) {
      return GameLibraryScreen(repository: repository);
    }
    final strings = litchStrings(context);
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(title: Text(strings.gameLibraryTitle)),
      ),
      body: SafeArea(
        child: _opening && _openError == null
            ? Center(
                child: Semantics(
                  liveRegion: true,
                  label: strings.gameLibraryLoading,
                  child: const CircularProgressIndicator(),
                ),
              )
            : _LibraryUnavailable(onRetry: _open),
      ),
    );
  }
}

class GameLibraryScreen extends StatefulWidget {
  const GameLibraryScreen({super.key, required this.repository});

  final GameLibraryRepository repository;

  @override
  State<GameLibraryScreen> createState() => _GameLibraryScreenState();
}

class _GameLibraryScreenState extends State<GameLibraryScreen> {
  final ScrollController _scrollController = ScrollController();
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

  GameLibraryCatalog _catalog = const GameLibraryCatalog(
    totalCount: 0,
    recoveredFiles: <String>[],
    rejectedFiles: <String>[],
  );
  Map<int, GameArchive> _window = const <int, GameArchive>{};
  StreamSubscription<GameLibrarySnapshot>? _changes;
  bool _loading = true;
  Object? _loadError;
  int _windowRevision = 0;
  int? _pendingWindowCenter;
  bool _windowRequestScheduled = false;
  int _windowDrainGeneration = 0;
  int _lastWindowCenter = 0;

  @override
  void initState() {
    super.initState();
    _subscribe();
    unawaited(_load());
  }

  @override
  void didUpdateWidget(GameLibraryScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.repository == widget.repository) {
      return;
    }
    _retireDeleteNotice();
    unawaited(_changes?.cancel());
    _catalog = const GameLibraryCatalog(
      totalCount: 0,
      recoveredFiles: <String>[],
      rejectedFiles: <String>[],
    );
    _window = const <int, GameArchive>{};
    _loading = true;
    _loadError = null;
    _windowDrainGeneration += 1;
    _windowRequestScheduled = false;
    _pendingWindowCenter = null;
    _lastWindowCenter = 0;
    _subscribe();
    unawaited(_load());
  }

  void _subscribe() {
    final GameLibraryRepository repository = widget.repository;
    _changes = repository.changes.listen((GameLibrarySnapshot snapshot) {
      scheduleMicrotask(() {
        if (!mounted || repository != widget.repository) {
          return;
        }
        unawaited(_load(showProgress: false, preserveWindow: true));
      });
    });
  }

  Future<void> _load({
    bool showProgress = true,
    bool preserveWindow = false,
  }) async {
    final int revision = ++_windowRevision;
    if (mounted && showProgress) {
      setState(() {
        _loading = true;
        _loadError = null;
      });
    }
    try {
      GameLibraryCatalog catalog = await widget.repository.loadCatalog();
      int offset = preserveWindow
          ? (_lastWindowCenter - 24).clamp(0, catalog.totalCount)
          : 0;
      GameLibraryWindow window = catalog.totalCount == 0
          ? GameLibraryWindow(
              offset: 0,
              totalCount: 0,
              games: const [],
              generation: catalog.generation,
            )
          : await widget.repository.loadWindow(offset: offset, limit: 49);
      if (window.generation != catalog.generation) {
        catalog = await widget.repository.loadCatalog();
        offset = preserveWindow
            ? (_lastWindowCenter - 24).clamp(0, catalog.totalCount)
            : 0;
        window = catalog.totalCount == 0
            ? GameLibraryWindow(
                offset: 0,
                totalCount: 0,
                games: const [],
                generation: catalog.generation,
              )
            : await widget.repository.loadWindow(offset: offset, limit: 49);
        if (window.generation != catalog.generation) {
          throw StateError('Game library changed while loading.');
        }
      }
      if (!mounted || revision != _windowRevision) {
        return;
      }
      setState(() {
        _catalog = catalog;
        _window = <int, GameArchive>{
          for (int index = 0; index < window.games.length; index += 1)
            window.offset + index: window.games[index],
        };
        if (!preserveWindow) {
          _lastWindowCenter = window.offset + (window.games.length ~/ 2);
        }
        _loading = false;
        _loadError = null;
      });
    } catch (error) {
      if (!mounted || revision != _windowRevision) {
        return;
      }
      setState(() {
        _loading = false;
        _loadError = error;
      });
    }
  }

  void _needWindow(int index) {
    _lastWindowCenter = index;
    if (_window.containsKey(index)) return;
    _pendingWindowCenter = index;
    if (_windowRequestScheduled) return;
    _windowRequestScheduled = true;
    final int drainGeneration = _windowDrainGeneration;
    scheduleMicrotask(() => _drainWindowRequest(drainGeneration));
  }

  Future<void> _drainWindowRequest(int drainGeneration) async {
    try {
      while (mounted && drainGeneration == _windowDrainGeneration) {
        final int? center = _pendingWindowCenter;
        if (center == null) break;
        _pendingWindowCenter = null;
        final GameLibraryRepository repository = widget.repository;
        final int revision = ++_windowRevision;
        final int offset = (center - 24).clamp(0, _catalog.totalCount);
        final GameLibraryWindow window;
        try {
          window = await repository.loadWindow(offset: offset, limit: 49);
        } catch (error) {
          if (mounted && revision == _windowRevision) {
            setState(() => _loadError = error);
          }
          break;
        }
        if (!mounted ||
            drainGeneration != _windowDrainGeneration ||
            repository != widget.repository) {
          return;
        }
        if (window.generation != _catalog.generation) {
          unawaited(_load(showProgress: false, preserveWindow: true));
          return;
        }
        if (_pendingWindowCenter != null || revision != _windowRevision) {
          continue;
        }
        setState(() {
          final Map<int, GameArchive> merged = <int, GameArchive>{
            ..._window,
            for (int index = 0; index < window.games.length; index += 1)
              window.offset + index: window.games[index],
          };
          merged.removeWhere(
            (int index, GameArchive _) => (index - center).abs() > 96,
          );
          _window = merged;
        });
      }
    } finally {
      if (drainGeneration == _windowDrainGeneration) {
        _windowRequestScheduled = false;
        if (mounted && _pendingWindowCenter != null) {
          _needWindow(_pendingWindowCenter!);
        }
      }
    }
  }

  @override
  void dispose() {
    _windowDrainGeneration += 1;
    _retireDeleteNotice(afterUnmount: true);
    _scrollController.dispose();
    unawaited(_changes?.cancel());
    super.dispose();
  }

  Future<void> _open(GameArchive archive) async {
    // Undo feedback belongs to the list action, not the review route.
    _retireDeleteNotice();
    await Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        builder: (_) => GameLibraryDetailScreen(
          key: ValueKey<String>(
            'game-library-detail:${archive.libraryId}:${archive.boardGameId}',
          ),
          repository: widget.repository,
          initialArchive: archive,
          libraryId: archive.libraryId,
          gameId: archive.gameId,
          origin: archive.origin,
        ),
      ),
    );
    if (mounted && _scrollController.hasClients) {
      // Every review return starts at the newest games, independent of saves.
      _scrollController.jumpTo(0);
    }
  }

  void _openFenPreview() {
    _retireDeleteNotice();
    Navigator.of(context).push<void>(
      MaterialPageRoute<void>(builder: (_) => const FenPreviewScreen()),
    );
  }

  Future<void> _rename(GameArchive archive) async {
    String draft = archive.displayName ?? '';
    bool saving = false;
    String? errorText;
    await showDialog<void>(
      context: context,
      builder: (BuildContext dialogContext) {
        return StatefulBuilder(
          builder: (BuildContext dialogContext, StateSetter setDialogState) {
            final strings = litchStrings(dialogContext);
            Future<void> save() async {
              if (saving) return;
              final String submittedDraft = draft;
              final String? normalized = normalizeGameArchiveDisplayName(
                submittedDraft,
              );
              if (normalized != null &&
                  !isGameArchiveDisplayNameWithinBounds(normalized)) {
                setDialogState(
                  () => errorText = strings.gameLibraryNameTooLong,
                );
                return;
              }
              setDialogState(() {
                saving = true;
                errorText = null;
              });
              try {
                // Validate the same normalized grapheme and byte bounds before
                // entering storage; the repository validates again.
                archive.rename(submittedDraft);
                await widget.repository.rename(
                  archive.libraryId,
                  submittedDraft,
                );
                if (dialogContext.mounted &&
                    ModalRoute.of(dialogContext)?.isCurrent == true) {
                  Navigator.of(dialogContext).pop();
                }
              } on GameLibraryRecoveredRenameException {
                if (!dialogContext.mounted) return;
                setDialogState(() {
                  saving = false;
                  errorText = strings.gameLibraryNameRecoveryUnavailable;
                });
              } catch (_) {
                if (!dialogContext.mounted) return;
                setDialogState(() {
                  saving = false;
                  errorText = strings.gameLibraryRenameFailed;
                });
              }
            }

            final bool useAccessibleFullscreen =
                MediaQuery.textScalerOf(dialogContext).scale(1) >= 1.8;
            Widget nameField({required bool showDecorationHelp}) {
              return TextFormField(
                key: const ValueKey<String>('game-library-name-field'),
                initialValue: draft,
                autofocus: true,
                enabled: !saving,
                maxLength: gameArchiveDisplayNameMaximumCharacters,
                textCapitalization: TextCapitalization.sentences,
                textInputAction: TextInputAction.done,
                decoration: InputDecoration(
                  labelText: strings.gameLibraryGameNameLabel,
                  helper: showDecorationHelp
                      ? Text(
                          strings.gameLibraryGameNameHelp,
                          softWrap: true,
                          overflow: TextOverflow.visible,
                        )
                      : null,
                  error: errorText == null
                      ? null
                      : Text(
                          errorText!,
                          softWrap: true,
                          overflow: TextOverflow.visible,
                        ),
                ),
                onChanged: (String name) {
                  draft = name;
                  if (errorText != null) {
                    setDialogState(() => errorText = null);
                  }
                },
                onFieldSubmitted: (_) => unawaited(save()),
              );
            }

            Widget dismissButton() => LitchCompactControls(
              child: TextButton(
                onPressed: () => Navigator.of(dialogContext).pop(),
                child: Text(
                  saving ? strings.gameLibraryClose : strings.cancelAction,
                ),
              ),
            );
            Widget saveButton() => LitchCompactControls(
              child: FilledButton(
                key: const ValueKey<String>('game-library-name-save'),
                onPressed: saving ? null : () => unawaited(save()),
                child: Text(strings.gameLibrarySaveName),
              ),
            );
            final String title = archive.displayName == null
                ? strings.gameLibraryNameGame
                : strings.gameLibraryRenameGame;

            if (useAccessibleFullscreen) {
              return Dialog.fullscreen(
                child: Scaffold(
                  appBar: LitchCompactAppBar(
                    child: AppBar(
                      leading: IconButton(
                        onPressed: () => Navigator.of(dialogContext).pop(),
                        icon: const Icon(Icons.close_rounded),
                        tooltip: saving
                            ? strings.gameLibraryClose
                            : strings.cancelAction,
                      ),
                      title: Text(title),
                    ),
                  ),
                  body: SafeArea(
                    child: SingleChildScrollView(
                      padding: const EdgeInsets.fromLTRB(24, 20, 24, 24),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: <Widget>[
                          nameField(showDecorationHelp: false),
                          const SizedBox(height: 8),
                          Text(strings.gameLibraryGameNameHelp),
                          if (saving) ...<Widget>[
                            const SizedBox(height: 8),
                            Text(
                              strings.gameLibrarySavingContinues,
                              key: const ValueKey<String>(
                                'game-library-name-saving-continues',
                              ),
                            ),
                          ],
                          const SizedBox(height: 20),
                          Wrap(
                            alignment: WrapAlignment.end,
                            spacing: 8,
                            runSpacing: 8,
                            children: <Widget>[dismissButton(), saveButton()],
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              );
            }

            return AlertDialog(
              scrollable: true,
              title: Text(title),
              content: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  nameField(showDecorationHelp: true),
                  if (saving) ...<Widget>[
                    const SizedBox(height: 8),
                    Text(
                      strings.gameLibrarySavingContinues,
                      key: const ValueKey<String>(
                        'game-library-name-saving-continues',
                      ),
                    ),
                  ],
                ],
              ),
              actions: <Widget>[dismissButton(), saveButton()],
            );
          },
        );
      },
    );
  }

  Future<void> _delete(GameArchive archive) => deleteGameArchiveWithUndo(
    context,
    widget.repository,
    archive,
    onNotice: _showDeleteNotice,
  );

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final theme = Theme.of(context);
    double chromeWidth(String text, TextStyle? style) {
      final painter = TextPainter(
        text: TextSpan(text: text, style: style),
        textDirection: Directionality.of(context),
        textScaler: LitchCompactControls.textScalerOf(context),
        locale: Localizations.maybeLocaleOf(context),
      )..layout();
      final width = painter.width;
      painter.dispose();
      return width;
    }

    final availableWidth =
        MediaQuery.sizeOf(context).width -
        MediaQuery.paddingOf(context).horizontal;
    final bool compactLoadAction =
        chromeWidth(
              strings.gameLibraryTitle,
              theme.appBarTheme.titleTextStyle ?? theme.textTheme.titleLarge,
            ) +
            chromeWidth(
              strings.fenPreviewEntryAction,
              theme.textTheme.labelLarge,
            ) +
            56 +
            32 +
            18 +
            8 +
            24 >
        availableWidth;
    final Widget loadAction = compactLoadAction
        ? TextButton(
            key: const ValueKey<String>('game-library-fen-preview'),
            onPressed: _openFenPreview,
            child: Text(strings.fenPreviewEntryActionCompact),
          )
        : TextButton.icon(
            key: const ValueKey<String>('game-library-fen-preview'),
            onPressed: _openFenPreview,
            icon: const Icon(Icons.add_rounded),
            label: Text(strings.fenPreviewEntryAction),
          );
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(
          title: Text(strings.gameLibraryTitle),
          actions: <Widget>[
            Semantics(
              label: strings.fenPreviewEntryAction,
              button: true,
              onTap: _openFenPreview,
              excludeSemantics: true,
              child: Tooltip(
                message: strings.fenPreviewEntryAction,
                excludeFromSemantics: true,
                child: loadAction,
              ),
            ),
          ],
        ),
      ),
      body: SafeArea(
        child: _loading
            ? Center(
                child: Semantics(
                  liveRegion: true,
                  label: strings.gameLibraryLoading,
                  child: const CircularProgressIndicator(),
                ),
              )
            : _loadError != null
            ? _LibraryUnavailable(onRetry: _load)
            : Align(
                alignment: Alignment.topCenter,
                child: ConstrainedBox(
                  constraints: BoxConstraints(
                    maxWidth:
                        LitchWorkspaceGeometry.isTabletWindow(
                          MediaQuery.sizeOf(context),
                        )
                        ? LitchWorkspaceGeometry.readableContentWidth
                        : double.infinity,
                  ),
                  child: _LibraryContents(
                    scrollController: _scrollController,
                    catalog: _catalog,
                    window: _window,
                    onNeedWindow: _needWindow,
                    onOpen: _open,
                    onRename: _rename,
                    onDelete: _delete,
                  ),
                ),
              ),
      ),
    );
  }
}

class _LibraryUnavailable extends StatelessWidget {
  const _LibraryUnavailable({required this.onRetry});

  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(24),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 460),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              const Icon(Icons.error_outline_rounded, size: 40),
              const SizedBox(height: 12),
              Text(
                strings.gameLibraryUnavailableTitle,
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 8),
              Text(
                strings.gameLibraryUnavailableBody,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 18),
              LitchCompactControls(
                child: FilledButton.icon(
                  key: const ValueKey<String>('game-library-retry'),
                  style: FilledButton.styleFrom(minimumSize: const Size(0, 48)),
                  onPressed: onRetry,
                  icon: const Icon(Icons.refresh_rounded),
                  label: Text(strings.tryAgain),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _LibraryContents extends StatelessWidget {
  const _LibraryContents({
    required this.scrollController,
    required this.catalog,
    required this.window,
    required this.onNeedWindow,
    required this.onOpen,
    required this.onRename,
    required this.onDelete,
  });

  final ScrollController scrollController;
  final GameLibraryCatalog catalog;
  final Map<int, GameArchive> window;
  final ValueChanged<int> onNeedWindow;
  final ValueChanged<GameArchive> onOpen;
  final ValueChanged<GameArchive> onRename;
  final ValueChanged<GameArchive> onDelete;

  @override
  Widget build(BuildContext context) {
    final bool hasRecoveryNotice =
        catalog.recoveredFiles.isNotEmpty || catalog.rejectedFiles.isNotEmpty;
    if (catalog.totalCount == 0 && !hasRecoveryNotice) {
      return const Center(child: SingleChildScrollView(child: _EmptyLibrary()));
    }
    return LayoutBuilder(
      builder: (context, constraints) {
        final geometry = _GameLibraryRowGeometry(
          context,
          cardWidth: constraints.maxWidth - 32,
        );
        return CustomScrollView(
          key: const ValueKey<String>('game-library-list'),
          controller: scrollController,
          // Roughly twenty cards on either side of the viewport stay warm while
          // only visible/cache elements are mounted, even for very large lists.
          cacheExtent: geometry.extent(hasDisplayName: false) * 20,
          slivers: <Widget>[
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
              sliver: SliverToBoxAdapter(
                child: Semantics(
                  header: true,
                  child: Text(
                    litchStrings(
                      context,
                    ).gameLibraryStoredCount(catalog.totalCount),
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                ),
              ),
            ),
            if (hasRecoveryNotice)
              SliverPadding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
                sliver: SliverToBoxAdapter(
                  child: _RecoveryNotice(
                    snapshot: GameLibrarySnapshot(
                      recoveredFiles: catalog.recoveredFiles,
                      rejectedFiles: catalog.rejectedFiles,
                    ),
                  ),
                ),
              ),
            if (catalog.totalCount == 0)
              const SliverFillRemaining(
                hasScrollBody: false,
                child: _EmptyLibrary(),
              )
            else
              SliverPadding(
                padding: const EdgeInsets.fromLTRB(16, 0, 16, 28),
                sliver: SliverVariedExtentList.builder(
                  itemCount: catalog.totalCount,
                  itemExtentBuilder: (int index, _) => geometry.extent(
                    hasDisplayName: catalog.hasDisplayNameAt(index),
                  ),
                  itemBuilder: (BuildContext context, int index) {
                    onNeedWindow(index);
                    final GameArchive? game = window[index];
                    if (game == null) {
                      return _GameArchiveLoadingTile(
                        key: ValueKey<String>(
                          'game-library-placeholder-$index',
                        ),
                        hasDisplayName: catalog.hasDisplayNameAt(index),
                        geometry: geometry,
                      );
                    }
                    return SizedBox(
                      key: ValueKey<String>('game-library-row-$index'),
                      height: geometry.extent(
                        hasDisplayName: catalog.hasDisplayNameAt(index),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.only(bottom: 8),
                        child: _GameArchiveTile(
                          archive: game,
                          geometry: geometry,
                          onTap: () => onOpen(game),
                          onRename: () => onRename(game),
                          onDelete: () => onDelete(game),
                        ),
                      ),
                    );
                  },
                ),
              ),
          ],
        );
      },
    );
  }
}

class _EmptyLibrary extends StatelessWidget {
  const _EmptyLibrary();

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 420),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              const Icon(Icons.history_rounded, size: 40),
              const SizedBox(height: 12),
              Text(
                strings.gameLibraryEmptyTitle,
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 8),
              Text(strings.gameLibraryEmptyBody, textAlign: TextAlign.center),
            ],
          ),
        ),
      ),
    );
  }
}

class _RecoveryNotice extends StatelessWidget {
  const _RecoveryNotice({required this.snapshot});

  final GameLibrarySnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final int recovered = snapshot.recoveredFiles.length;
    final int rejected = snapshot.rejectedFiles.length;
    return Semantics(
      key: const ValueKey<String>('game-library-recovery-warning'),
      container: true,
      liveRegion: true,
      label: strings.gameLibraryRecoverySummary(recovered, rejected),
      child: Card(
        color: Theme.of(context).colorScheme.errorContainer,
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Icon(
                Icons.warning_amber_rounded,
                color: Theme.of(context).colorScheme.onErrorContainer,
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  strings.gameLibraryRecoverySummary(recovered, rejected),
                  style: TextStyle(
                    color: Theme.of(context).colorScheme.onErrorContainer,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _GameLibraryRowGeometry {
  _GameLibraryRowGeometry(BuildContext context, {required double cardWidth}) {
    final theme = Theme.of(context);
    final strings = litchStrings(context);
    final inheritedStyle = DefaultTextStyle.of(context).style;
    bodyStyle = inheritedStyle.merge(theme.textTheme.bodyMedium);
    titleStyle = inheritedStyle.merge(
      theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w700),
    );
    final scaler = MediaQuery.textScalerOf(context);
    final direction = Directionality.of(context);
    final locale = Localizations.maybeLocaleOf(context);
    Size measure(
      String text,
      TextStyle style, [
      double width = double.infinity,
    ]) {
      final painter = TextPainter(
        text: TextSpan(text: text, style: style),
        textDirection: direction,
        textScaler: scaler,
        locale: locale,
      )..layout(maxWidth: width);
      final size = painter.size;
      painter.dispose();
      return size;
    }

    final variants = <String>[
      for (final variant in GameVariant.values)
        gameArchiveVariantLabel(context, variant),
    ];
    final qualities = <String>[
      strings.gameLibraryCompleteReplay,
      strings.gameLibraryPartialReplay,
    ];
    final counts = <String>[
      for (final count in <int>[0, 1, gameArchiveMaximumPlies])
        strings.gameLibraryMoveCount(count),
    ];
    double widest(Iterable<String> values) =>
        values.map((value) => measure(value, bodyStyle).width).reduce(math.max);

    // Reserve the menu and InkWell padding before choosing the thumbnail layout.
    // This uses one bounded localized envelope, never the archive catalog.
    final contentWidth = math.max(1.0, cardWidth - 48 - 14);
    final inlineDetailsWidth = math.max(1.0, contentWidth - 88 - 12);
    stacked =
        widest(<String>[
          for (final variant in variants)
            '$variant · ${strings.gameLibraryMoveCount(1)}',
        ]) >
        inlineDetailsWidth;
    final detailsWidth = stacked ? contentWidth : inlineDetailsWidth;
    compactMetadata =
        !stacked &&
        widest(variants) + widest(qualities) + widest(counts) + 16 >
            detailsWidth;
    titleMaxLines = stacked ? 2 : 1;
    resultMaxLines = stacked ? 3 : 1;
    dateMaxLines = stacked ? 2 : 1;

    final bodyLineHeight = measure('Ag', bodyStyle).height;
    final titleLineHeight = measure('Ag', titleStyle).height;
    double tallest(Iterable<String> values) => values
        .map((value) => measure(value, bodyStyle, detailsWidth).height)
        .reduce(math.max);
    // The noncompact Wrap may put each item on its own run. Preserve that
    // envelope for unloaded rows, including partial replay and plural counts.
    final metadataHeight = compactMetadata
        ? bodyLineHeight * 2 + 2
        : tallest(variants) + tallest(qualities) + tallest(counts) + 12;
    double rowExtent(bool named) {
      final detailsHeight =
          titleLineHeight * titleMaxLines +
          4 +
          (named ? bodyLineHeight * resultMaxLines + 2 : 0) +
          bodyLineHeight * dateMaxLines +
          6 +
          metadataHeight;
      final contentHeight = stacked
          ? 72 + 10 + detailsHeight
          : math.max(88.0, detailsHeight);
      return math.max(112.0, contentHeight + 20) + 8;
    }

    unnamedExtent = rowExtent(false);
    namedExtent = rowExtent(true);
  }

  late final TextStyle titleStyle;
  late final TextStyle bodyStyle;
  late final bool stacked;
  late final bool compactMetadata;
  late final int titleMaxLines;
  late final int resultMaxLines;
  late final int dateMaxLines;
  late final double unnamedExtent;
  late final double namedExtent;

  double extent({required bool hasDisplayName}) =>
      hasDisplayName ? namedExtent : unnamedExtent;
}

class _GameArchiveLoadingTile extends StatelessWidget {
  const _GameArchiveLoadingTile({
    super.key,
    required this.hasDisplayName,
    required this.geometry,
  });

  final bool hasDisplayName;
  final _GameLibraryRowGeometry geometry;

  @override
  Widget build(BuildContext context) {
    final String loading = litchStrings(context).gameLibraryLoading;
    return SizedBox(
      height: geometry.extent(hasDisplayName: hasDisplayName),
      child: Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Semantics(
          label: loading,
          liveRegion: true,
          child: ExcludeSemantics(
            child: Card(
              margin: EdgeInsets.zero,
              child: Center(
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: <Widget>[
                      const Icon(Icons.hourglass_top_rounded),
                      const SizedBox(width: 12),
                      Flexible(child: Text(loading)),
                    ],
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _GameArchiveTile extends StatelessWidget {
  const _GameArchiveTile({
    required this.archive,
    required this.geometry,
    required this.onTap,
    required this.onRename,
    required this.onDelete,
  });

  final GameArchive archive;
  final _GameLibraryRowGeometry geometry;
  final VoidCallback onTap;
  final VoidCallback onRename;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final String result = gameArchiveResultLabel(context, archive);
    final String variant = gameArchiveVariantLabel(context, archive.variant);
    final String quality = archive.captureQuality == GameCaptureQuality.complete
        ? strings.gameLibraryCompleteReplay
        : strings.gameLibraryPartialReplay;
    final String moves = strings.gameLibraryMoveCount(
      archive.transitions.length,
    );
    final String date = MaterialLocalizations.of(
      context,
    ).formatMediumDate(archive.updatedAt.toLocal());
    final String title = archive.displayName ?? result;
    final String openLabel = strings.gameLibraryGameSemantics(
      date,
      variant,
      result,
      quality,
      moves,
    );
    Widget details() => DefaultTextStyle.merge(
      style: geometry.bodyStyle,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisAlignment: MainAxisAlignment.center,
        children: <Widget>[
          Text(
            title,
            maxLines: geometry.titleMaxLines,
            overflow: TextOverflow.ellipsis,
            style: geometry.titleStyle,
          ),
          const SizedBox(height: 4),
          if (archive.displayName != null) ...<Widget>[
            Text(
              result,
              maxLines: geometry.resultMaxLines,
              overflow: TextOverflow.ellipsis,
            ),
            const SizedBox(height: 2),
          ],
          Text(
            geometry.compactMetadata
                ? date
                : strings.gameLibrarySavedDate(date),
            maxLines: geometry.dateMaxLines,
            overflow: TextOverflow.ellipsis,
          ),
          const SizedBox(height: 6),
          if (geometry.compactMetadata) ...<Widget>[
            Text(
              '$variant · $moves',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
            if (archive.captureQuality ==
                GameCaptureQuality.partial) ...<Widget>[
              const SizedBox(height: 2),
              Text(quality, maxLines: 1, overflow: TextOverflow.ellipsis),
            ],
          ] else
            Wrap(
              spacing: 8,
              runSpacing: 6,
              children: <Widget>[Text(variant), Text(quality), Text(moves)],
            ),
        ],
      ),
    );
    return Card(
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: ConstrainedBox(
        constraints: const BoxConstraints(minHeight: 112),
        child: Row(
          crossAxisAlignment: geometry.stacked
              ? CrossAxisAlignment.start
              : CrossAxisAlignment.center,
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
                    key: ValueKey<String>(
                      'game-library-game-${archive.libraryId}',
                    ),
                    onTap: onTap,
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(10, 10, 4, 10),
                      child: geometry.stacked
                          ? Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: <Widget>[
                                GamePositionThumbnail(
                                  archive: archive,
                                  size: 72,
                                ),
                                const SizedBox(height: 10),
                                details(),
                              ],
                            )
                          : Row(
                              children: <Widget>[
                                GamePositionThumbnail(archive: archive),
                                const SizedBox(width: 12),
                                Expanded(child: details()),
                              ],
                            ),
                    ),
                  ),
                ),
              ),
            ),
            Padding(
              padding: EdgeInsets.only(top: geometry.stacked ? 8 : 0),
              child: SizedBox(
                width: 48,
                child: PopupMenuButton<String>(
                  key: ValueKey<String>(
                    'game-library-menu-${archive.libraryId}',
                  ),
                  tooltip: strings.gameLibraryGameActions,
                  onSelected: (String value) {
                    if (value == 'rename') {
                      onRename();
                    } else if (value == 'delete') {
                      onDelete();
                    }
                  },
                  itemBuilder: (BuildContext context) =>
                      <PopupMenuEntry<String>>[
                        PopupMenuItem<String>(
                          value: 'rename',
                          child: LitchCompactControls(
                            child: ListTile(
                              contentPadding: EdgeInsets.zero,
                              leading: const Icon(Icons.edit_outlined),
                              title: Text(
                                archive.displayName == null
                                    ? strings.gameLibraryNameGame
                                    : strings.gameLibraryRenameGame,
                              ),
                            ),
                          ),
                        ),
                        PopupMenuItem<String>(
                          value: 'delete',
                          child: LitchCompactControls(
                            child: ListTile(
                              contentPadding: EdgeInsets.zero,
                              leading: const Icon(Icons.delete_outline_rounded),
                              title: Text(strings.gameLibraryDeleteGame),
                            ),
                          ),
                        ),
                      ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class GameLibraryDetailScreen extends StatefulWidget {
  const GameLibraryDetailScreen({
    super.key,
    required this.repository,
    this.initialArchive,
    required this.libraryId,
    required this.gameId,
    required this.origin,
  });

  final GameLibraryRepository repository;
  final GameArchive? initialArchive;
  final String libraryId;
  final String gameId;
  final GameArchiveOrigin origin;

  @override
  State<GameLibraryDetailScreen> createState() =>
      _GameLibraryDetailScreenState();
}

class _GameLibraryDetailScreenState extends State<GameLibraryDetailScreen> {
  StreamSubscription<GameLibrarySnapshot>? _changes;
  final PostGameReviewJobOwner _fallbackReviewOwner = PostGameReviewJobOwner(
    analyzer: const BoundedLocalPostGameReviewAnalyzer(),
  );
  late GameArchive? _archive;
  int _lookupRevision = 0;

  @override
  void initState() {
    super.initState();
    _archive =
        widget.initialArchive ?? _matchingArchive(widget.repository.snapshot);
    _changes = widget.repository.changes.listen((_) {
      unawaited(_refreshExact());
    });
    unawaited(_refreshExact());
  }

  @override
  void dispose() {
    unawaited(_changes?.cancel());
    _fallbackReviewOwner.close();
    super.dispose();
  }

  GameArchive? _matchingArchive(GameLibrarySnapshot snapshot) {
    GameArchive? newest;
    for (final GameArchive game in snapshot.games) {
      if (game.libraryId != widget.libraryId ||
          game.gameId != widget.gameId ||
          game.origin != widget.origin) {
        continue;
      }
      if (newest == null || game.updatedAt.isAfter(newest.updatedAt)) {
        newest = game;
      }
    }
    return newest;
  }

  Future<void> _refreshExact() async {
    final int revision = ++_lookupRevision;
    final GameArchive? archive;
    try {
      archive = await widget.repository.findByIdentity(
        widget.libraryId,
        widget.gameId,
        widget.origin,
      );
    } catch (_) {
      return;
    }
    if (!mounted || revision != _lookupRevision) return;
    if (archive?.toJson().toString() == _archive?.toJson().toString()) return;
    setState(() => _archive = archive);
  }

  @override
  Widget build(BuildContext context) {
    final GameArchive? archive = _archive;
    if (archive != null) {
      return PostGameReviewScreen(
        archive: archive,
        reviewOwner:
            PostGameReviewJobOwnerScope.maybeOf(context) ??
            _fallbackReviewOwner,
      );
    }
    final strings = litchStrings(context);
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(title: Text(strings.gameLibraryTitle)),
      ),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: <Widget>[
              const Icon(Icons.event_busy_outlined, size: 40),
              const SizedBox(height: 12),
              Text(
                strings.gameLibraryGameUnavailableTitle,
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 8),
              Text(
                strings.gameLibraryGameUnavailableBody,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 18),
              LitchCompactControls(
                child: OutlinedButton.icon(
                  key: const ValueKey<String>('game-library-detail-back'),
                  style: OutlinedButton.styleFrom(
                    minimumSize: const Size(0, 48),
                  ),
                  onPressed: () => Navigator.of(context).pop(),
                  icon: const Icon(Icons.arrow_back_rounded),
                  label: Text(
                    MaterialLocalizations.of(context).backButtonTooltip,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
