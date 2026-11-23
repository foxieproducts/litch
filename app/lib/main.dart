import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import 'l10n/generated/app_localizations.dart';
import 'src/engine/engine_runtime.dart';
import 'src/localization/litch_locale.dart';
import 'src/screens/connection_gate_screen.dart';
import 'src/services/game_library_repository.dart';
import 'src/services/game_library_store.dart';
import 'src/services/app_litchcraft_preferences.dart';
import 'src/services/app_trace_recorder.dart';
import 'src/services/app_tips_preferences.dart';
import 'src/services/app_upgrade_notice_service.dart';
import 'src/services/litch_ble_service.dart';
import 'src/services/litchcraft_artwork_store.dart';
import 'src/services/physical_game_history_ingestion.dart';
import 'src/services/local_player_profile_store.dart';
import 'src/models/local_player_profile.dart';
import 'src/services/post_game_review_analyzer.dart';
import 'src/theme/litch_appearance.dart';
import 'src/theme/litch_theme.dart';
import 'src/widgets/app_preferences.dart';
import 'src/widgets/orientation_workspace.dart';
import 'src/widgets/post_game_review_analyzer_scope.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final trace = AppTraceRecorder.start(
    directory: () async {
      final supportDirectory = await getApplicationSupportDirectory();
      return Directory('${supportDirectory.path}/app_trace');
    },
  );
  trace.record(
    'app.started',
    fields: <String, Object?>{
      'app_generation': 2,
      'build_mode': kDebugMode
          ? 'debug'
          : kProfileMode
          ? 'profile'
          : 'release',
    },
  );
  registerAppTraceServiceExtension(trace);
  runApp(LitchMobileApp(appTrace: trace));
}

typedef GameLibraryLoader = Future<GameLibraryRepository> Function();

class LitchMobileApp extends StatefulWidget {
  const LitchMobileApp({
    super.key,
    this.bleService,
    this.firmwarePageBuilder,
    this.gameLibrary,
    this.gameLibraryLoader,
    this.appTrace,
    this.engineRuntime,
    this.appUpgradeNoticeService,
  });

  final LitchBleService? bleService;
  final FirmwarePageBuilder? firmwarePageBuilder;
  final GameLibraryRepository? gameLibrary;
  final GameLibraryLoader? gameLibraryLoader;
  final AppTraceSink? appTrace;
  final EngineRuntime? engineRuntime;
  final AppUpgradeNoticeService? appUpgradeNoticeService;

  @override
  State<LitchMobileApp> createState() => _LitchMobileAppState();
}

class _LitchMobileAppState extends State<LitchMobileApp> {
  late final LitchBleService _bleService;
  late final bool _ownsBleService;
  LitchAppearanceMode _appearanceMode = LitchAppearanceMode.system;
  LitchLocalePreference _localePreference =
      const LitchLocalePreference.system();
  bool _showTips = true;
  int _litchcraftStorageLimitBytes =
      AppLitchcraftPreferences.defaultStorageLimitBytes;
  int _tipsRevision = 0;
  int _litchcraftStorageRevision = 0;
  int _localeRevision = 0;
  GameLibraryRepository? _gameLibrary;
  Future<GameLibraryRepository>? _gameLibraryOpen;
  bool _ownsGameLibrary = false;
  Directory? _applicationSupportDirectory;
  PhysicalGameHistoryIngestion? _gameHistoryIngestion;
  Future<void>? _gameHistoryStart;
  PostGameReviewJobOwner? _postGameReviewOwner;

  @override
  void initState() {
    super.initState();
    _ownsBleService = widget.bleService == null;
    _bleService =
        widget.bleService ?? LitchBleService(appTrace: widget.appTrace);
    _bindEngineRuntime(widget.engineRuntime);
    _gameLibrary = widget.gameLibrary;
    if (_gameLibrary case final GameLibraryRepository repository) {
      unawaited(_startGameHistoryIngestion(repository));
    }
    if (_gameLibrary == null) {
      unawaited(_warmGameLibrary());
    }
    unawaited(_loadAppearanceMode());
    unawaited(_loadLocalePreference());
    unawaited(_loadShowTips());
    unawaited(_loadLitchcraftStorageLimit());
  }

  void _bindEngineRuntime(EngineRuntime? runtime) {
    _postGameReviewOwner?.close();
    _postGameReviewOwner = PostGameReviewJobOwner(
      analyzer: runtime == null
          ? const BoundedLocalPostGameReviewAnalyzer()
          : EngineBackedPostGameReviewAnalyzer(runtime: runtime),
    );
  }

  @override
  void didUpdateWidget(LitchMobileApp oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.engineRuntime, widget.engineRuntime)) {
      _bindEngineRuntime(widget.engineRuntime);
    }
  }

  Future<void> _warmGameLibrary() async {
    try {
      await _openGameLibrary();
    } catch (_) {
      // Games remains visible and retries this opener from its own route.
    }
  }

  Future<GameLibraryRepository> _openGameLibrary() {
    final GameLibraryRepository? current = _gameLibrary;
    if (current != null) {
      return Future<GameLibraryRepository>.value(current);
    }
    return _gameLibraryOpen ??= _createGameLibrary();
  }

  Future<GameLibraryRepository> _createGameLibrary() async {
    GameLibraryRepository? repository;
    try {
      repository = await (widget.gameLibraryLoader ?? _loadGameLibrary)();
      if (!mounted) {
        await repository.close();
        throw StateError('The app closed while opening saved games.');
      }
      setState(() {
        _gameLibrary = repository;
        _ownsGameLibrary = true;
      });
      unawaited(_startGameHistoryIngestion(repository));
      return repository;
    } catch (error) {
      _gameLibraryOpen = null;
      await repository?.close();
      rethrow;
    }
  }

  Future<GameLibraryRepository> _loadGameLibrary() async {
    final Directory supportDirectory = await getApplicationSupportDirectory();
    _applicationSupportDirectory = supportDirectory;
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(Directory('${supportDirectory.path}/game_library')),
    );
    return repository;
  }

  Future<void> _startGameHistoryIngestion(GameLibraryRepository repository) {
    if (_gameHistoryIngestion != null) return Future<void>.value();
    return _gameHistoryStart ??= () async {
      try {
        final Directory supportDirectory =
            _applicationSupportDirectory ??
            await getApplicationSupportDirectory();
        final playerProfiles = await LocalPlayerProfileStore.open();
        if (!mounted || repository != _gameLibrary) return;
        final PhysicalGameHistoryIngestion ingestion =
            PhysicalGameHistoryIngestion(
              bleService: _bleService,
              repository: repository,
              receiveDirectory: Directory(
                '${supportDirectory.path}/current_game_receive',
              ),
              playerProfiles: playerProfiles,
              appTrace: widget.appTrace,
            );
        _gameHistoryIngestion = ingestion;
        ingestion.start();
      } catch (_) {
        // Saved games remain usable if the receive staging directory is not
        // available. A later app launch retries activation from scratch.
      }
    }();
  }

  Future<LocalPlayerDirectory> _assignPlayer({
    required String boardId,
    required LocalPlayerChessSide side,
    String? displayName,
    Future<void>? cancellation,
    bool Function()? beginCommit,
  }) async {
    final repository = await _openGameLibrary();
    await _startGameHistoryIngestion(repository);
    final owner = _gameHistoryIngestion;
    if (owner == null) throw StateError('Current-game storage is unavailable.');
    return owner.assignPlayer(
      boardId: boardId,
      side: side,
      displayName: displayName,
      cancellation: cancellation,
      beginCommit: beginCommit,
    );
  }

  @override
  void dispose() {
    final GameLibraryRepository? repository = _gameLibrary;
    final PhysicalGameHistoryIngestion? ingestion = _gameHistoryIngestion;
    _postGameReviewOwner?.close();
    unawaited(() async {
      await ingestion?.close();
      if (_ownsGameLibrary && repository != null) await repository.close();
      if (_ownsBleService) await _bleService.dispose();
    }());
    super.dispose();
  }

  Future<void> _loadAppearanceMode() async {
    try {
      final LitchAppearanceMode mode = await LitchAppearancePreferences.load();
      if (mounted) {
        setState(() => _appearanceMode = mode);
      }
    } catch (_) {
      // The system theme already rendered a usable first frame. An unavailable
      // preference store must not turn startup into an unhandled async error.
    }
  }

  Future<void> _setAppearanceMode(LitchAppearanceMode mode) async {
    if (_appearanceMode == mode) {
      return;
    }
    setState(() => _appearanceMode = mode);
    await LitchAppearancePreferences.save(mode);
  }

  Future<void> _loadLocalePreference() async {
    final revision = _localeRevision;
    try {
      final preference = await LitchLocalePreferences.load(
        supportedLocales: litchSupportedLocales,
      );
      if (mounted && revision == _localeRevision) {
        setState(() => _localePreference = preference);
      }
    } catch (_) {
      // System language remains usable when preferences cannot be loaded.
    }
  }

  Future<void> _setLocalePreference(LitchLocalePreference preference) async {
    // Even selecting the current value is explicit intent: supersede a pending
    // startup read and allow retrying a previous failed persistence write.
    _localeRevision++;
    setState(() => _localePreference = preference);
    await LitchLocalePreferences.save(preference);
  }

  Future<void> _loadShowTips() async {
    final revision = _tipsRevision;
    try {
      final enabled = await AppTipsPreferences.load();
      if (mounted && revision == _tipsRevision) {
        setState(() => _showTips = enabled);
      }
    } catch (_) {
      // Tips default on; unavailable preferences must not delay startup.
    }
  }

  Future<void> _setShowTips(bool enabled) async {
    // Selecting the current value retries a prior persistence failure and
    // supersedes a startup read that has not completed yet.
    _tipsRevision++;
    setState(() => _showTips = enabled);
    await AppTipsPreferences.save(enabled);
  }

  Future<void> _loadLitchcraftStorageLimit() async {
    final revision = _litchcraftStorageRevision;
    try {
      final bytes = await AppLitchcraftPreferences.loadStorageLimit();
      if (mounted && revision == _litchcraftStorageRevision) {
        setState(() => _litchcraftStorageLimitBytes = bytes);
      }
    } catch (_) {
      // The 1 GB default remains usable when preferences cannot load.
    }
  }

  Future<void> _setLitchcraftStorageLimit(int bytes) async {
    final revision = ++_litchcraftStorageRevision;
    await AppLitchcraftPreferences.saveStorageLimit(bytes);
    if (mounted && revision == _litchcraftStorageRevision) {
      setState(() => _litchcraftStorageLimitBytes = bytes);
    }
  }

  @override
  Widget build(BuildContext context) {
    Widget app = MaterialApp(
      title: 'Foxie Litch',
      debugShowCheckedModeBanner: false,
      theme: buildLitchTheme(),
      darkTheme: buildLitchDarkTheme(),
      themeMode: _appearanceMode.themeMode,
      locale: _localePreference.locale,
      supportedLocales: litchSupportedLocales,
      localeListResolutionCallback: resolveLitchLocale,
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      builder: (context, child) => AppPreferencesScope(
        appearanceMode: _appearanceMode,
        onAppearanceModeChanged: _setAppearanceMode,
        localePreference: _localePreference,
        onLocalePreferenceChanged: _setLocalePreference,
        showTips: _showTips,
        onShowTipsChanged: _setShowTips,
        litchcraftStorageLimitBytes: _litchcraftStorageLimitBytes,
        onLitchcraftStorageLimitChanged: _setLitchcraftStorageLimit,
        child: OrientationWorkspace(child: child!),
      ),
      home: ConnectionGateScreen(
        bleService: _bleService,
        craftStoreLoader: () => LitchcraftArtworkStore.open(
          storageLimitProvider: () => _litchcraftStorageLimitBytes,
        ),
        appTrace: widget.appTrace,
        firmwarePageBuilder: widget.firmwarePageBuilder,
        gameLibrary: _gameLibrary,
        gameLibraryOpener: _openGameLibrary,
        assignPlayer: _assignPlayer,
        appearanceMode: _appearanceMode,
        onAppearanceModeChanged: _setAppearanceMode,
        appUpgradeNoticeService: widget.appUpgradeNoticeService,
      ),
    );
    final reviewOwner = _postGameReviewOwner;
    final runtime = widget.engineRuntime;
    if (reviewOwner != null) {
      app = PostGameReviewJobOwnerScope(owner: reviewOwner, child: app);
    }
    if (runtime != null) {
      app = EngineRuntimeScope(runtime: runtime, child: app);
    }
    return app;
  }
}
