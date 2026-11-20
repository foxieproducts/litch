import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart'
    show debugDefaultTargetPlatformOverride;
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/device_landscape_side.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:litch_mobile/src/widgets/litch_workspace_geometry.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:litch_mobile/src/widgets/litchcraft_motion_editor.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';
import 'fixtures/litchcraft_v2_functional_peer.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ConnectedScreenBle extends LitchBleService {
  _ConnectedScreenBle()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _NoopBle());
  @override
  bool get isConnected => true;
}

/// Holds the board-authorized Craft state for shell presentation checks.
/// Radio, mode negotiation and transfer cause/recovery are not simulated here.
class _ActiveArtworkDeliveryController extends _ArtworkDeliveryController {
  _ActiveArtworkDeliveryController(super.bleService);
  @override
  bool get selected => true;
  @override
  bool get boardActive => true;
}

/// Only discovery is suppressed; production disconnected shell chrome and
/// editor navigation/layout are exercised without a native radio.
class _OfflineShellBle extends LitchBleService {
  _OfflineShellBle()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _NoopBle());
  @override
  BleAvailability get bleAvailability => BleAvailability.unsupported;
  @override
  Future<void> startScan() async {}
}

class _NoticeConnectionBle extends LitchBleService {
  _NoticeConnectionBle()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _NoopBle());
  int epoch = 1;
  String board = 'first-board';
  @override
  int get connectionSessionToken => epoch;
  @override
  String? get connectedId => board;
}

class _NoticeConnectionController extends _ArtworkDeliveryController {
  _NoticeConnectionController(super.bleService);
  @override
  bool get usesFrameStoreDocument => true;
}

bool _primaryFocusIsWithin(Finder finder) {
  final context = FocusManager.instance.primaryFocus?.context;
  if (context is! Element) return false;
  final target = finder.evaluate().single;
  if (identical(context, target)) return true;
  var found = false;
  context.visitAncestorElements((ancestor) {
    if (identical(ancestor, target)) {
      found = true;
      return false;
    }
    return true;
  });
  return found;
}

Axis _toolbarAxis(WidgetTester tester) {
  final widget = tester.widget(find.byKey(const ValueKey('craft-tool-grid')));
  return switch (widget) {
    Flex(:final direction) => direction,
    Wrap(:final direction) => direction,
    _ => throw TestFailure('Unexpected toolbar layout: ${widget.runtimeType}'),
  };
}

class _PendingCraftController extends LitchcraftController {
  _PendingCraftController(super.bleService);
  @override
  bool get pending => true;
  @override
  LitchcraftStatus get status => LitchcraftStatus.live;
}

class _ArtworkDeliveryController extends _PendingCraftController {
  _ArtworkDeliveryController(super.bleService);

  LitchcraftFullSyncDirection? direction;
  double? syncProgress;
  bool rootPending = false;
  bool readbackPending = false;
  bool boardReadPending = false;
  bool boardUnavailable = false;
  bool queuedPlay = false;
  LitchcraftStatus connectionStatus = LitchcraftStatus.live;

  @override
  LitchcraftStatus get status => connectionStatus;
  @override
  bool get artworkReadbackPending => readbackPending;
  @override
  bool get boardArtworkLoading => boardReadPending;
  @override
  bool get boardArtworkUnresolved => boardReadPending;
  @override
  bool get storeArtworkUnavailable => boardUnavailable;
  @override
  bool get animationPlaybackPending => queuedPlay;

  void showBoardRead(bool value) {
    boardReadPending = value;
    notifyListeners();
  }

  void showReadback(bool value) {
    readbackPending = value;
    notifyListeners();
  }

  void showQueuedPlay(bool value) {
    queuedPlay = value;
    notifyListeners();
  }

  void setStatus(LitchcraftStatus value) {
    connectionStatus = value;
    notifyListeners();
  }

  @override
  bool get storeRootPublicationPending => rootPending;

  void showRootPending(bool value) {
    rootPending = value;
    notifyListeners();
  }

  @override
  LitchcraftFullSyncDirection? get fullAnimationSyncDirection => direction;
  @override
  double? get fullAnimationSyncProgress => syncProgress;

  void showFullSync(LitchcraftFullSyncDirection? value, double? progress) {
    direction = value;
    syncProgress = progress;
    notifyListeners();
  }

  @override
  bool artworkDeliveryPending = true;
  @override
  double? artworkDeliveryProgress;
  @override
  bool artworkDeliveryFinishing = false;

  void showDelivery({
    bool pending = true,
    double? progress,
    bool finishing = false,
  }) {
    artworkDeliveryPending = pending;
    artworkDeliveryProgress = progress;
    artworkDeliveryFinishing = finishing;
    notifyListeners();
  }
}

class _SlowArtworkOpenController extends LitchcraftController {
  _SlowArtworkOpenController(super.bleService);

  Completer<void>? replacement;

  @override
  Future<void> replaceDocument(
    LitchcraftAnimationDocument document, {
    bool newCanvas = false,
  }) {
    restoreDocument(document);
    replacement = Completer<void>();
    return replacement!.future;
  }
}

class _FailingArtworkSaveLibrary extends LitchcraftLibrary {
  _FailingArtworkSaveLibrary(super.controller);

  @override
  Future<void> save(String name, {bool saveAs = false}) async {
    throw StateError('Controlled phone storage failure');
  }
}

class _SlowArtworkSaveController extends LitchcraftController {
  _SlowArtworkSaveController(super.bleService);

  Completer<void>? coherentRelease;

  @override
  Future<LitchcraftAnimationDocument> coherentDocument() async {
    await coherentRelease?.future;
    return super.coherentDocument();
  }
}

class _ConflictCraftController extends LitchcraftController {
  _ConflictCraftController(super.bleService);
  final release = Completer<void>();
  bool? resolvedWithBoard;
  LitchcraftStatus currentStatus = LitchcraftStatus.conflict;
  LitchcraftCanvas appArtwork = LitchcraftCanvas.blank().paint(
    const <int>[0],
    15,
    0xff7430,
  );
  LitchcraftCanvas physicalArtwork = LitchcraftCanvas.blank().paint(
    const <int>[63],
    15,
    0x0070ff,
  );
  @override
  LitchcraftStatus get status => currentStatus;
  @override
  LitchcraftCanvas get canvas => appArtwork;
  @override
  LitchcraftCanvas get boardCanvas => physicalArtwork;

  void setStatus(LitchcraftStatus value) {
    currentStatus = value;
    notifyListeners();
  }

  void replacePhysicalArtwork(LitchcraftCanvas value) {
    physicalArtwork = value;
    notifyListeners();
  }

  @override
  Future<void> resolveConflict({required bool useBoard}) {
    resolvedWithBoard = useBoard;
    return release.future;
  }
}

class _PendingConflictPreviewController extends _ConflictCraftController {
  _PendingConflictPreviewController(super.bleService) {
    // The unresolved fallback is deliberately stale. It is never a preview.
    physicalArtwork = LitchcraftCanvas.blank();
  }

  int previewStarts = 0;
  int previewRetirements = 0;
  LitchcraftCanvas? verifiedPreview;
  bool previewLoading = false;
  bool previewFailed = false;

  @override
  bool get frameStoreArtworkReadPending => true;
  @override
  LitchcraftCanvas? get conflictBoardPreview => verifiedPreview;
  @override
  bool get conflictBoardPreviewLoading => previewLoading;
  @override
  bool get conflictBoardPreviewFailed => previewFailed;

  @override
  Future<void> prepareConflictBoardPreview() async {
    ++previewStarts;
    previewLoading = true;
    previewFailed = false;
    notifyListeners();
  }

  void failPreview() {
    previewLoading = false;
    previewFailed = true;
    notifyListeners();
  }

  void finishPreview() {
    verifiedPreview = LitchcraftCanvas.blank().paint(
      const <int>[63],
      15,
      0x0070ff,
    );
    previewLoading = false;
    previewFailed = false;
    notifyListeners();
  }

  @override
  void retireConflictBoardPreview() {
    ++previewRetirements;
    previewLoading = false;
    notifyListeners();
  }
}

class _HistoryRecoveryCraftController extends _ConflictCraftController {
  _HistoryRecoveryCraftController(super.bleService) {
    currentStatus = LitchcraftStatus.live;
  }

  bool recoveringHistory = false;
  @override
  bool get historyRecoveryPending => recoveringHistory;

  bool readingArtwork = false;
  @override
  bool get artworkReadbackPending => recoveringHistory && readingArtwork;

  void setArtworkReadback(bool value) {
    readingArtwork = value;
    notifyListeners();
  }

  @override
  bool get connectionRecoveryRequired =>
      currentStatus == LitchcraftStatus.error;

  void setHistoryRecovery(bool value) {
    recoveringHistory = value;
    notifyListeners();
  }
}

class _ConflictPlayingCraftController extends _ConflictCraftController {
  _ConflictPlayingCraftController(super.bleService);

  int boardElapsed = 0;

  @override
  bool get animationBoardAuthorityLive => true;
  @override
  bool get animationPlaying => true;
  @override
  int get animationElapsedMilliseconds => boardElapsed;
}

class _LoadingBoardCraftController extends LitchcraftController {
  _LoadingBoardCraftController(super.bleService);

  bool loading = true;
  LitchcraftStatus currentStatus = LitchcraftStatus.live;
  int paintAttempts = 0;

  @override
  LitchcraftStatus get status => currentStatus;
  @override
  bool get boardArtworkUnresolved => loading;
  @override
  bool get boardArtworkLoading =>
      loading && currentStatus == LitchcraftStatus.live;
  @override
  void paintSquares(Iterable<int> squares) {
    paintAttempts++;
  }

  void finishLoading() {
    loading = false;
    notifyListeners();
  }

  void failLoading() {
    currentStatus = LitchcraftStatus.error;
    notifyListeners();
  }
}

class _FailingConflictCraftController extends _ConflictCraftController {
  _FailingConflictCraftController(super.bleService);

  @override
  Future<void> resolveConflict({required bool useBoard}) async {
    throw StateError('Controlled board artwork read failure');
  }
}

class _PhysicalPickCraftController extends LitchcraftController {
  _PhysicalPickCraftController(super.bleService);
  bool picking = false;
  bool picked = false;
  bool erase = false;

  @override
  LitchcraftStatus get status => LitchcraftStatus.live;
  @override
  bool get physicalColorPicking => picking;
  @override
  bool get physicalColorPicked => picked;
  @override
  bool get eraser => erase;

  void showPhysicalPick({required bool inProgress, bool erasing = false}) {
    picking = inProgress;
    picked = !inProgress;
    erase = erasing;
    notifyListeners();
  }
}

class _MutableStatusCraftController extends LitchcraftController {
  _MutableStatusCraftController(
    super.bleService, {
    required LitchcraftStatus initialStatus,
  }) : currentStatus = initialStatus;

  LitchcraftStatus currentStatus;

  @override
  LitchcraftStatus get status => currentStatus;

  void setStatus(LitchcraftStatus value) {
    currentStatus = value;
    notifyListeners();
  }

  void repeatStatusNotification() => notifyListeners();
}

class _FlippableCraftController extends LitchcraftController {
  _FlippableCraftController(super.bleService);

  bool testFlipped = false;

  @override
  bool get flipped => testFlipped;

  void flipPresentation() {
    testFlipped = !testFlipped;
    notifyListeners();
  }
}

class _RefusedRotationCraftController extends LitchcraftController {
  _RefusedRotationCraftController(super.bleService)
    : super(
        documentSession: LitchcraftDocumentSession(
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: <LitchcraftAnimationFrame>[
              LitchcraftAnimationFrame(
                emitters: List<LitchcraftAnimatedEmitter>.filled(
                  256,
                  const LitchcraftAnimatedEmitter(color: 0),
                ),
              ),
              LitchcraftAnimationFrame(
                emitters: List<LitchcraftAnimatedEmitter>.filled(
                  256,
                  const LitchcraftAnimatedEmitter(color: 0),
                ),
              ),
            ],
          ),
        ),
      );

  int rotationAttempts = 0;

  @override
  LitchcraftStatus get status => LitchcraftStatus.live;

  @override
  Future<bool> rotateArtwork(int quarterTurns) async {
    rotationAttempts++;
    return false;
  }
}

class _UnsupportedCraftController extends LitchcraftController {
  _UnsupportedCraftController(super.bleService)
    : super(
        documentSession: LitchcraftDocumentSession(
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: List.generate(
              2,
              (frame) => LitchcraftAnimationFrame(
                emitters: List.generate(
                  256,
                  (_) => LitchcraftAnimatedEmitter(
                    color: frame == 0 ? 0xff0000 : 0x0000ff,
                  ),
                ),
              ),
            ),
          ),
        ),
      );

  @override
  LitchcraftStatus get status => LitchcraftStatus.unsupported;
}

class _UnavailableStoreCraftController extends LitchcraftController {
  _UnavailableStoreCraftController(
    super.bleService, {
    required this.currentStatus,
    this.missingRoot = false,
  }) : super(
         documentSession: LitchcraftDocumentSession(
           LitchcraftAnimationDocument(
             topology: LitchcraftTopology.v3,
             favoriteColors: litchcraftDefaultFavoriteColors,
             frames: List.generate(
               9,
               (frame) => LitchcraftAnimationFrame(
                 emitters: List.generate(
                   256,
                   (_) => LitchcraftAnimatedEmitter(
                     color: frame.isEven ? 0xff0000 : 0x0000ff,
                   ),
                 ),
               ),
             ),
           ),
         ),
       );

  final LitchcraftStatus currentStatus;
  final bool missingRoot;
  int boardToggleCalls = 0;

  @override
  LitchcraftStatus get status => currentStatus;
  @override
  bool get storeArtworkUnavailable => missingRoot;
  @override
  Future<void> toggleAnimationPlayback() async => boardToggleCalls++;
}

class _BoardTimelineCraftController extends LitchcraftController {
  _BoardTimelineCraftController(super.bleService);
  bool playing = true;
  bool queuedPlay = false;
  bool failedPlay = false;
  bool busy = false;
  bool authoritative = true;
  int selectedFrame = 0;
  int toggleCalls = 0, selectCalls = 0;
  int effectCalls = 0, addFrameCalls = 0, deleteFrameCalls = 0;
  bool acceptEffects = true;
  bool captureEnabled = true;
  int durationCalls = 0;
  LitchcraftFrameEditTarget? lastDurationTarget;
  Completer<void>? toggleRelease;
  LitchcraftFrameEditTarget? lastDeleteTarget;

  @override
  LitchcraftStatus get status => LitchcraftStatus.live;
  @override
  bool get animationBoardAuthorityLive => authoritative;
  @override
  bool get animationPlaying => playing;
  @override
  bool get animationPlaybackPending => queuedPlay;
  @override
  bool get animationPlaybackFailed => failedPlay;
  @override
  bool get waitingForLift => busy;
  @override
  int get animationSelectedFrameIndex => selectedFrame;
  @override
  int get animationElapsedMilliseconds => 250;

  @override
  Future<void> toggleAnimationPlayback() async {
    toggleCalls++;
    if (queuedPlay) {
      queuedPlay = false;
    } else if (failedPlay) {
      failedPlay = false;
      playing = true;
    } else {
      playing = !playing;
    }
    notifyListeners();
    await toggleRelease?.future;
  }

  @override
  Future<void> selectAnimationFrame(int index) async {
    selectCalls++;
    selectedFrame = index;
    documentSession.selectFrame(index);
    notifyListeners();
  }

  @override
  Future<bool> applyAnimationEffect({
    required int square,
    required int mask,
    required LitchcraftEffectStyle effect,
    int effectDurationMs = litchcraftAnimationDefaultEffectDurationMs,
    LitchcraftFrameEditTarget? target,
  }) async {
    effectCalls++;
    if (!acceptEffects) return false;
    documentSession.applyEffect(
      square: square,
      mask: mask,
      effect: effect,
      effectDurationMs: effectDurationMs,
    );
    return true;
  }

  @override
  LitchcraftFrameEditTarget? captureAnimationFrameEditTarget({int? frame}) =>
      captureEnabled
      ? LitchcraftFrameEditTarget(
          frame: frame ?? selectedFrame,
          session: 42,
          revision: 1,
          generation: 1,
        )
      : null;

  @override
  Future<bool> setAnimationFrameDuration(
    int durationMs, {
    LitchcraftFrameEditTarget? target,
  }) async {
    durationCalls++;
    lastDurationTarget = target;
    if (target != null) documentSession.selectFrame(target.frame);
    documentSession.setSelectedFrameDuration(durationMs);
    selectedFrame = documentSession.selectedFrameIndex;
    return true;
  }

  @override
  Future<LitchcraftFrameEditTarget?> prepareAnimationFrameEdit() async {
    playing = false;
    documentSession.selectFrame(selectedFrame);
    return LitchcraftFrameEditTarget(
      frame: selectedFrame,
      session: 42,
      revision: 1,
      generation: 1,
    );
  }

  @override
  Future<bool> addAnimationFrame({LitchcraftFrameEditTarget? target}) async {
    addFrameCalls++;
    documentSession.addFrame();
    selectedFrame = documentSession.selectedFrameIndex;
    return true;
  }

  @override
  Future<bool> deleteAnimationFrame({LitchcraftFrameEditTarget? target}) async {
    deleteFrameCalls++;
    lastDeleteTarget = target;
    if (target != null) documentSession.selectFrame(target.frame);
    documentSession.deleteSelectedFrame();
    selectedFrame = documentSession.selectedFrameIndex;
    return true;
  }
}

class _StoreTimelineCraftController extends _BoardTimelineCraftController {
  _StoreTimelineCraftController(super.bleService);

  @override
  bool get boardActive => authoritative;
  @override
  bool get boardArtworkUnresolved => false;
  @override
  bool get animationBoardAuthorityLive => false;
  @override
  bool get usesFrameStoreDocument => true;
  @override
  bool get storePlaybackFollowsLocal => true;
  @override
  int get animationElapsedMilliseconds =>
      (litchcraftAnimationFrameStartOrNull(
            documentSession.document,
            selectedFrame,
          ) ??
          0) +
      250;
}

Future<void> _openToolTray(WidgetTester tester) async {
  if (find.byKey(const ValueKey('craft-paint')).evaluate().isNotEmpty) {
    return;
  }
  await tester.tap(find.byKey(const ValueKey('craft-palette')));
  await tester.pumpAndSettle();
}

Future<void> _openAnimationTray(WidgetTester tester) async {
  final tablet = find.byKey(const ValueKey('craft-tablet-workspace'));
  if (tablet.evaluate().isNotEmpty) {
    final portraitFrames = find.byKey(
      const ValueKey('craft-portrait-select-frames'),
    );
    if (portraitFrames.evaluate().isNotEmpty) {
      await tester.tap(portraitFrames);
      await tester.pump();
      return;
    }
    final showTools = find.byKey(const ValueKey('craft-tablet-open-tools'));
    if (showTools.hitTestable().evaluate().isNotEmpty) {
      await tester.tap(showTools);
      await tester.pump();
    }
    final play = find.byKey(const ValueKey('craft-preview'));
    final jump = find.byKey(const ValueKey('craft-jump-to-tool-section'));
    if (play.hitTestable().evaluate().isEmpty && jump.evaluate().isNotEmpty) {
      await tester.tap(jump);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const ValueKey('craft-jump-frames')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
    }
    return;
  }
  if (find
          .byKey(const ValueKey('craft-animation-quick-tray'))
          .evaluate()
          .isNotEmpty ||
      find.byKey(const ValueKey('craft-frame-panel')).evaluate().isNotEmpty) {
    return;
  }
  final launcher = find.byKey(const ValueKey('craft-animate'));
  await tester.ensureVisible(launcher);
  await tester.pump();
  await tester.tap(launcher);
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

Future<void> _revealInspectorControl(
  WidgetTester tester,
  Finder target, {
  double dragDy = -120,
  int maxDrags = 80,
}) async {
  if (target.hitTestable().evaluate().isNotEmpty) return;
  if (target.evaluate().isEmpty) {
    throw TestFailure('Inspector control is absent: $target');
  }

  // The current inspector is intentionally fixed and non-scrolling. Only the
  // exceptional short-portrait workspace owns a page-level fallback scroll.
  // ensureVisible follows that production ancestor when it exists and is a
  // no-op for ordinary portrait and landscape layouts.
  await tester.ensureVisible(target);
  await tester.pump(const Duration(milliseconds: 120));
  if (target.hitTestable().evaluate().isNotEmpty) return;

  final fallback = find.byKey(const ValueKey('craft-short-portrait-scroll'));
  if (fallback.hitTestable().evaluate().isNotEmpty) {
    for (var attempt = 0; attempt < maxDrags; attempt++) {
      await tester.drag(fallback, Offset(0, dragDy.sign * 24));
      await tester.pump(const Duration(milliseconds: 120));
      if (target.hitTestable().evaluate().isNotEmpty) return;
    }
  }
  throw TestFailure(
    'Inspector control never became hittable: $target; target=${tester.getRect(target)}',
  );
}

List<String> _recordHoldHaptics(WidgetTester tester) {
  final haptics = <String>[];
  tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
    SystemChannels.platform,
    (call) async {
      if (call.method == 'HapticFeedback.vibrate') {
        haptics.add(call.arguments as String);
      }
      return null;
    },
  );
  addTearDown(
    () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      null,
    ),
  );
  return haptics;
}

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    // The phone inspector and tablet workbench expose different controls.
    // Set the real view so MediaQuery and layout see the same window on rotation.
    final view =
        TestWidgetsFlutterBinding.instance.platformDispatcher.implicitView!;
    view.devicePixelRatio = 1;
    view.physicalSize = const Size(390, 844);
    addTearDown(view.reset);
  });

  for (final side in [Alignment.centerLeft, Alignment.centerRight]) {
    testWidgets('SFNS timing captions keep whole words at 200% $side', (
      tester,
    ) async {
      final previousPlatform = debugDefaultTargetPlatformOverride;
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      tester.view.physicalSize = const Size(852, 393);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues({});
      final ble = _OfflineShellBle();
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await library.dismissWelcome();
      final insets = EdgeInsets.fromLTRB(
        side.x < 0 ? 59 : 0,
        0,
        side.x < 0 ? 0 : 59,
        21,
      );
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      try {
        await tester.pumpWidget(
          RepaintBoundary(
            key: const ValueKey('timing-word-review'),
            child: MaterialApp(
              debugShowCheckedModeBanner: false,
              theme: buildLitchTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  padding: insets,
                  viewPadding: insets,
                  textScaler: const TextScaler.linear(2),
                  disableAnimations: true,
                ),
                child: child!,
              ),
              home: Scaffold(
                body: SafeArea(
                  child: LitchcraftScreen(
                    controller: controller,
                    library: library,
                    landscapeRibbonSide: side,
                    rootSafeInsets: insets,
                  ),
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        void expectWholeWords() {
          for (final label in ['Slower', 'Faster']) {
            final paragraph = tester.renderObject<RenderParagraph>(
              find.descendant(
                of: find.text(label),
                matching: find.byType(RichText),
              ),
            );
            final words = RegExp(
              r'\S+',
            ).allMatches(paragraph.text.toPlainText()).toList();
            expect(words, isNotEmpty);
            for (final word in words) {
              final boxes = paragraph.getBoxesForSelection(
                TextSelection(baseOffset: word.start, extentOffset: word.end),
              );
              expect(boxes, isNotEmpty);
              expect(
                boxes.map((box) => box.top).toSet(),
                hasLength(1),
                reason: '$label must not split within a word at200% SFNS $side',
              );
              final control = tester.getRect(
                find.byKey(const ValueKey('craft-timing-control')),
              );
              for (final box in boxes) {
                final left = paragraph.localToGlobal(Offset(box.left, box.top));
                final right = paragraph.localToGlobal(Offset(box.right, box.bottom));
                expect(left.dx, greaterThanOrEqualTo(control.left));
                expect(right.dx, lessThanOrEqualTo(control.right));
              }
            }
          }
          expect(
            tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
            canvas,
          );
          expect(tester.takeException(), isNull);
        }

        Future<void> dragWithRebuild(String key, VoidCallback rebuild) async {
          final target = find.byKey(ValueKey(key));
          final rect = tester.getRect(target);
          expect(rect.height, greaterThanOrEqualTo(48));
          final gesture = await tester.startGesture(
            Offset(rect.left + rect.width * .25, rect.center.dy),
          );
          await tester.pump(const Duration(milliseconds: 40));
          final first = tester.widget<Slider>(target).value;
          await gesture.moveTo(
            Offset(rect.left + rect.width * .5, rect.center.dy),
            timeStamp: const Duration(milliseconds: 80),
          );
          await tester.pump(const Duration(milliseconds: 40));
          final middle = tester.widget<Slider>(target).value;
          expect(middle, isNot(first));
          rebuild();
          await tester.pump(const Duration(milliseconds: 50));
          expect(tester.widget<Slider>(target).value, middle);
          expect(tester.getRect(target), rect);
          expectWholeWords();
          await gesture.moveTo(
            Offset(rect.left + rect.width * .75, rect.center.dy),
            timeStamp: const Duration(milliseconds: 160),
          );
          await tester.pump(const Duration(milliseconds: 40));
          expect(tester.widget<Slider>(target).value, isNot(middle));
          expect(tester.getRect(target), rect);
          expectWholeWords();
          await gesture.up();
          await tester.pump(const Duration(milliseconds: 40));
          expect(tester.getRect(target), rect);
          expectWholeWords();
        }

        await tester.tap(find.byKey(const ValueKey('craft-motion')));
        await tester.pumpAndSettle();
        expectWholeWords();
        await captureHealthReview(
          tester,
          'timing-fixed-${side.x < 0 ? 'left' : 'right'}-effects',
          boundaryFinder: find.byKey(const ValueKey('timing-word-review')),
        );
        await tester.tap(find.byKey(const ValueKey('craft-effect-selector')));
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey('craft-effect-breathe')).hitTestable(),
        );
        await tester.pumpAndSettle();
        await dragWithRebuild(
          'craft-effect-duration',
          controller.notifyListeners,
        );
        await tester.tap(find.byKey(const ValueKey('craft-animate')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
        await tester.pumpAndSettle();
        expect(library.animationDocument.frames, hasLength(2));
        library.setSelectedFrameDuration(1250);
        await tester.pump();
        expectWholeWords();
        await dragWithRebuild(
          'craft-frame-duration',
          () => library.selectFrame(0),
        );
        await captureHealthReview(
          tester,
          'timing-fixed-${side.x < 0 ? 'left' : 'right'}-animation',
          boundaryFinder: find.byKey(const ValueKey('timing-word-review')),
        );
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        debugDefaultTargetPlatformOverride = previousPlatform;
        library.dispose();
        controller.dispose();
        await ble.dispose();
      }
    });
  }


  testWidgets('native portrait timing fits without moving the canvas', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPadding);
    addTearDown(tester.view.resetViewPadding);
    for (final sample in [
      (const Size(393, 852), 3.0, 59.0, 34.0, TargetPlatform.iOS),
      (const Size(384, 832), 2.8125, 24.0, 24.0, TargetPlatform.android),
    ]) {
      for (final scale in [1.0, 2.0]) {
        tester.view.devicePixelRatio = sample.$2;
        tester.view.physicalSize = sample.$1 * sample.$2;
        tester.view.padding = tester.view.viewPadding = FakeViewPadding(
          top: sample.$3 * sample.$2,
          bottom: sample.$4 * sample.$2,
        );
        SharedPreferences.setMockInitialValues({});
        final ble = _OfflineShellBle();
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await library.dismissWelcome();
        try {
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme().copyWith(platform: sample.$5),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: TextScaler.linear(scale),
                  disableAnimations: true,
                ),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: ble,
                craftController: controller,
                craftLibrary: library,
              ),
            ),
          );
          await tester.pumpAndSettle();
          await tester.tap(find.byType(LitchcraftIdentityGlyph).last);
          await tester.pumpAndSettle();
          final canvas = find.byKey(const ValueKey('craft-ribbon-board'));
          final canvasRect = tester.getRect(canvas);
          await tester.tap(find.byKey(const ValueKey('craft-motion')));
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          expect(tester.getRect(canvas), canvasRect);
          final selector = find.byKey(const ValueKey('craft-effect-selector'));
          final timing = find.byKey(
            const ValueKey('craft-effect-duration-context'),
          );
          for (final target in [selector, timing]) {
            expect(target.hitTestable(), findsOneWidget);
            expect(tester.getSize(target).height, greaterThanOrEqualTo(48));
          }
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pumpAndSettle();
          final animation = find.byKey(const ValueKey('craft-animation-panel'));
          final content = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-content')),
          );
          for (final label in ['Slower', 'Faster']) {
            final caption = find.descendant(
              of: animation,
              matching: find.text(label),
            );
            expect(
              tester.getRect(caption).bottom,
              lessThanOrEqualTo(content.bottom),
            );
          }
          expect(tester.getRect(canvas), canvasRect);
          await tester.tap(find.byKey(const ValueKey('craft-leds')));
          await tester.pumpAndSettle();
          final ledContent = tester.getRect(
            find.byKey(const ValueKey('craft-inspector-content')),
          );
          final ledPanel = find.byKey(const ValueKey('craft-led-panel'));
          for (final mask in [1, 2, 4, 8, 15]) {
            final target = find.byKey(ValueKey('craft-target-$mask'));
            expect(target.hitTestable(), findsOneWidget);
            final rect = tester.getRect(target);
            expect(rect.size.shortestSide, greaterThanOrEqualTo(48));
            expect(rect.left, greaterThanOrEqualTo(ledContent.left));
            expect(rect.right, lessThanOrEqualTo(ledContent.right));
            expect(rect.top, greaterThanOrEqualTo(ledContent.top));
            expect(rect.bottom, lessThanOrEqualTo(ledContent.bottom));
            expect(
              find.descendant(of: target, matching: find.byType(Text)),
              findsNothing,
            );
          }
          expect(
            find.descendant(of: ledPanel, matching: find.byType(Text)),
            findsOneWidget,
            reason:
                'Only the shared prompt is visible; '
                'target names remain accessible.',
          );
          expect(tester.takeException(), isNull);
          expect(tester.getRect(canvas), canvasRect);
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          library.dispose();
          controller.dispose();
          await ble.dispose();
        }
      }
    }
  });

  testWidgets(
    'one tap opens frame inspector during a real held Store read',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      try {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final radio = VirtualLitchGattBle(native);
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: radio,
          gattSessionFactory: ReactiveBleGattSessionFactory(radio),
        );
        // Production disposal is pumped below; OS resources close in runner
        // teardown without reawaiting the cached fake-clock disposal future.
        addTearDown(() async {
          try {
            await radio.close();
          } finally {
            await tester.runAsync(native.close);
          }
        });
        final seed = LitchcraftController(service);
        LitchcraftController? owner;
        LitchcraftLibrary? activeLibrary;
        Future<void> step() async {
          await tester.pump();
          if (radio.hasPendingOperations) {
            await radio.flush(tester, maxOperations: 1);
          } else {
            await radio.command(tester, {
              'op': 'advance',
              'ms': 10,
            }, flushOperations: false);
            await tester.pump(const Duration(milliseconds: 10));
          }
        }

        Future<void> until(bool Function() done) async {
          for (var turn = 0; turn < 1000 && !done(); turn++) {
            await step();
          }
          expect(done(), isTrue, reason: 'production Store work must settle');
        }

        Future<void> finish(Future<dynamic> work) async {
          var done = false;
          Object? failure;
          work.then<void>(
            (_) => done = true,
            onError: (Object error, StackTrace _) {
              failure = error;
              done = true;
            },
          );
          await until(() => done);
          if (failure != null) throw failure!;
        }

        Future<void> cleanup() async {
          radio.heldChannels.clear();
          radio.release('connection_session');
          await tester.pumpWidget(const SizedBox.shrink());
          activeLibrary?.dispose();
          owner?.dispose();
          seed.dispose();
          // Exact GATT cleanup can submit native work; drain it before dispose.
          await finish(service.disconnect());
          await finish(service.dispose());
        }

        try {
          await radio.command(tester, {
            'op': 'hall',
            'mask': '0000000000000000',
          });
          await radio.advance(tester, 1000);
          await finish(service.connect('inspector-board'));
          await finish(seed.enter());
          final initial = seed.documentSession.document;
          await finish(
            seed.replaceDocument(
              LitchcraftAnimationDocument(
                frames: List.filled(33, initial.frames.first),
                topology: initial.topology,
                favoriteColors: initial.favoriteColors,
              ),
            ),
          );
          expect(seed.usesFrameStoreDocument, isTrue);
          expect(seed.documentSession.document.frames, hasLength(33));
          seed.dispose();
          await tester.pump();
          await finish(service.disconnect());
          await finish(service.connect('inspector-board'));

          final controller = owner = LitchcraftController(service);
          final library = activeLibrary = LitchcraftLibrary(controller);
          await library.ready;
          library.dismissedWelcome = true;
          final entry = controller.enter();
          entry.ignore();
          await until(() => controller.boardArtworkUnresolved);
          radio.heldChannels.add('connection_session');
          await radio.flush(tester);
          expect(controller.boardArtworkUnresolved, isTrue);
          expect(radio.held, isNotEmpty);
          await tester.pumpWidget(
            MaterialApp(
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          );
          final launcher = find.byKey(const ValueKey('craft-animate'));
          final writesBeforeOpening = radio.writes.length;
          await tester.tap(launcher);
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-animation-bar')),
            findsOneWidget,
            reason:
                'The first tap opens local controls while artwork is loading',
          );
          final launcherSemantics = tester.getSemantics(launcher);
          expect(
            launcherSemantics.flagsCollection.isEnabled,
            ui.Tristate.isTrue,
          );
          expect(
            launcherSemantics.getSemanticsData().hasAction(
              ui.SemanticsAction.tap,
            ),
            isTrue,
          );
          expect(
            launcherSemantics.getSemanticsData().hasAction(
              ui.SemanticsAction.longPress,
            ),
            isFalse,
            reason:
                'Opening Frames does not enable held playback during the read',
          );
          expect(tester.getSize(launcher).width, greaterThanOrEqualTo(48));
          expect(tester.getSize(launcher).height, greaterThanOrEqualTo(48));
          for (final key in ['craft-add-frame', 'craft-preview']) {
            expect(
              tester.widget<IconButton>(find.byKey(ValueKey(key))).onPressed,
              isNull,
            );
          }
          await tester.longPress(launcher);
          await tester.pump();
          expect(
            radio.writes.skip(writesBeforeOpening).where((write) {
              if (write['channel'] != 'connection_session') return false;
              return ConnectionSessionCodec.decode(
                    Uint8List.fromList(write['bytes'] as List<int>),
                  )?.kind ==
                  ConnectionSessionKind.ordered;
            }),
            isEmpty,
            reason: 'Local tabs and a guarded hold must send no board action',
          );
          expect(controller.boardArtworkUnresolved, isTrue);
          for (final key in [
            'craft-leds',
            'craft-palette',
            'craft-artwork-menu',
            'craft-animate',
          ]) {
            final tab = find.byKey(ValueKey(key));
            await tester.tap(tab);
            await tester.pump();
            expect(
              tester.getSemantics(tab).flagsCollection.isSelected,
              ui.Tristate.isTrue,
            );
            expect(controller.boardArtworkUnresolved, isTrue);
          }

          radio.heldChannels.clear();
          radio.release('connection_session');
          await finish(entry);
          await until(() => !controller.boardArtworkUnresolved);
          await tester.pump(const Duration(milliseconds: 300));
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: controller.errorMessage,
          );
          expect(controller.boardArtworkUnresolved, isFalse);
          expect(controller.documentSession.document.frames, hasLength(33));
          expect(
            find.byKey(const ValueKey('craft-animation-bar')),
            findsOneWidget,
          );
          expect(
            tester.getSemantics(launcher).flagsCollection.isSelected,
            ui.Tristate.isTrue,
          );
          await finish(service.disconnect());
          await tester.tap(find.byKey(const ValueKey('craft-palette')));
          await tester.pump();
          await tester.tap(launcher);
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-animation-bar')),
            findsOneWidget,
          );
          expect(controller.documentSession.document.frames, hasLength(33));
          expect(tester.takeException(), isNull);
        } finally {
          await cleanup();
        }
      } finally {
        semantics.dispose();
      }
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'sync activity animates while measured progress stalls and stops with work',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ArtworkDeliveryController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => RepaintBoundary(
            key: const ValueKey('sync-review-root'),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final canvasRect = tester.getRect(canvas);
      final selectedFrame = library.selectedFrameIndex;
      final document = library.animationDocument;
      final activity = find.byKey(const ValueKey('craft-sync-activity'));
      final circle = find.descendant(
        of: activity,
        matching: find.byType(CircularProgressIndicator),
      );
      final bar = find.byKey(const ValueKey('craft-full-animation-sync'));
      controller.showFullSync(LitchcraftFullSyncDirection.sendingToBoard, .4);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 499));
      expect(activity, findsNothing);
      await tester.pump(const Duration(milliseconds: 1));
      await tester.pump(const Duration(milliseconds: 180));
      expect(circle, findsOneWidget);
      expect(tester.widget<CircularProgressIndicator>(circle).value, isNull);
      Future<List<int>> pixels() async => (await tester.runAsync(() async {
        final boundary = tester.renderObject<RenderRepaintBoundary>(
          find.byKey(const ValueKey('sync-review-root')),
        );
        final image = await boundary.toImage(pixelRatio: 1);
        final bytes = await image.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        );
        final region = tester
            .getRect(activity)
            .shift(
              -tester.getTopLeft(
                find.byKey(const ValueKey('sync-review-root')),
              ),
            );
        final rgba = bytes!.buffer.asUint8List();
        final pixels = <int>[];
        for (var y = region.top.ceil(); y < region.bottom.floor(); y++) {
          final from = (y * image.width + region.left.ceil()) * 4;
          final to = (y * image.width + region.right.floor()) * 4;
          pixels.addAll(rgba.sublist(from, to));
        }
        image.dispose();
        return pixels;
      }))!;
      await tester.pump(const Duration(seconds: 2));
      expect(tester.widget<LinearProgressIndicator>(bar).value, .4);
      final before = await pixels();
      await tester.pump(const Duration(milliseconds: 160));
      final after = await pixels();
      expect(
        after,
        isNot(orderedEquals(before)),
        reason:
            'The loading circle must actually animate, even with unchanged transfer coverage.',
      );
      await tester.pump(const Duration(seconds: 2));
      expect(tester.widget<LinearProgressIndicator>(bar).value, .4);
      expect(circle, findsOneWidget);
      controller.setStatus(LitchcraftStatus.connecting);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(
        circle,
        findsOneWidget,
        reason:
            'An active reconnect/retry must keep activity visible without inventing bytes.',
      );
      expect(tester.widget<LinearProgressIndicator>(bar).value, .4);
      controller.setStatus(LitchcraftStatus.live);
      controller.showFullSync(LitchcraftFullSyncDirection.sendingToBoard, .995);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(tester.widget<LinearProgressIndicator>(bar).value, .995);
      expect(circle, findsOneWidget);
      controller.showFullSync(null, null);
      await tester.pump();
      expect(activity, findsNothing);
      controller.showRootPending(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(circle, findsOneWidget);
      controller.showRootPending(false);
      await tester.pump();
      expect(
        find.text('Sending artwork…'),
        findsNothing,
        reason: 'Completion retires the entire activity surface immediately.',
      );
      expect(
        activity,
        findsNothing,
        reason:
            'Completed work cannot retain an activity surface.',
      );
      await tester.pump(const Duration(milliseconds: 700));
      expect(find.text('Sending artwork…'), findsNothing);
      for (final outcome in [
        LitchcraftStatus.error,
        LitchcraftStatus.offline,
        LitchcraftStatus.live,
      ]) {
        controller.showFullSync(
          LitchcraftFullSyncDirection.readingFromBoard,
          .25,
        );
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        controller.showFullSync(null, null);
        controller.setStatus(outcome);
        await tester.pump();
        expect(activity, findsNothing);
      }
      expect(tester.getRect(canvas), canvasRect);
      expect(library.selectedFrameIndex, selectedFrame);
      expect(library.animationDocument, same(document));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final config in <({Size size, double scale, Locale locale})>[
    (size: Size(393, 852), scale: 1, locale: Locale('en')),
    (size: Size(320, 844), scale: 2, locale: Locale('de')),
    (size: Size(844, 390), scale: 2, locale: Locale('fr')),
  ]) {
    testWidgets(
      'Litchcraft feedback whole shell ${config.size} ${config.scale}x ${config.locale}',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = config.size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _ConnectedScreenBle();
        final controller = _ActiveArtworkDeliveryController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        await tester.pumpWidget(
          MaterialApp(
            locale: config.locale,
            theme: buildLitchTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => RepaintBoundary(
              key: const ValueKey('feedback-shell'),
              child: MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(config.scale)),
                child: child!,
              ),
            ),
            home: LitchMainScaffold(
              bleService: ble,
              boardId: 'board-1',
              boards: const [],
              onDisconnect: () async {},
              initialPageIndex: 0,
              initialCraftModeResolved: true,
              craftController: controller,
              craftLibrary: library,
              loadAppConsolePreference: () async => false,
            ),
          ),
        );
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        await tester.pump(const Duration(milliseconds: 300));
        final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
        final canvasRect = tester.getRect(canvas);
        final name =
            'feedback-${debugDefaultTargetPlatformOverride!.name}-${config.size.width.toInt()}-${config.scale.toInt()}x-${config.locale}';
        controller.showFullSync(LitchcraftFullSyncDirection.sendingToBoard, .4);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        await tester.pump(const Duration(milliseconds: 300));
        expect(
          find.byKey(const ValueKey('craft-sync-activity')),
          findsOneWidget,
        );
        await captureHealthReview(
          tester,
          '$name-transfer',
          boundaryFinder: find.byKey(const ValueKey('feedback-shell')),
        );
        controller.setStatus(LitchcraftStatus.connecting);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        await tester.pump(const Duration(milliseconds: 300));
        expect(
          tester
              .widget<LinearProgressIndicator>(
                find.byKey(const ValueKey('craft-full-animation-sync')),
              )
              .value,
          .4,
        );
        await captureHealthReview(
          tester,
          '$name-retry',
          boundaryFinder: find.byKey(const ValueKey('feedback-shell')),
        );
        controller.showFullSync(null, null);
        controller.setStatus(LitchcraftStatus.live);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);
        final hex = find.byKey(const ValueKey('craft-inspector-hex'));
        await _revealInspectorControl(tester, hex);
        await tester.pump();
        await tester.tap(hex);
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byKey(const ValueKey('craft-hex-pad')), findsOneWidget);
        await captureHealthReview(
          tester,
          '$name-keypad',
          boundaryFinder: find.byKey(const ValueKey('feedback-shell')),
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pump();
        expect(tester.testTextInput.isVisible, isFalse);
        expect(tester.getRect(canvas), canvasRect);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
      variant: const TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets(
    'persistent shared document failure becomes actionable after quiet repair',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      Future<void> drain() async {
        for (var i = 0; i < 30; i++) {
          await tester.pump(const Duration(milliseconds: 1));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
      }

      final entry = controller.enter();
      await drain();
      await entry;
      expect(controller.status, LitchcraftStatus.live);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      var failedWrites = 0;
      peer.deliverNotifications = false;
      peer.beforeDomainWork = () async {
        failedWrites++;
      };
      final edit = controller.fillAllWithColor(0x123456);
      var editFinished = false;
      edit.whenComplete(() => editFinished = true);
      for (var second = 0; second < 90 && !editFinished; ++second) {
        await drain();
        await tester.pump(const Duration(seconds: 1));
      }
      expect(editFinished, isTrue);
      await edit;
      expect(controller.status, LitchcraftStatus.error);
      expect(find.byKey(const ValueKey('craft-reconnect')), findsNothing);
      expect(find.textContaining('Board controls unavailable'), findsNothing);
      for (
        var second = 0;
        second < 90 && !controller.connectionRecoveryRequired;
        ++second
      ) {
        await tester.pump(const Duration(seconds: 1));
        await drain();
        if (!controller.connectionRecoveryRequired) {
          expect(find.byKey(const ValueKey('craft-reconnect')), findsNothing);
          expect(
            find.textContaining('Board controls unavailable'),
            findsNothing,
          );
        }
      }
      await drain();
      expect(controller.connectionRecoveryRequired, isTrue);
      expect(find.textContaining('Board controls unavailable'), findsOneWidget);
      final reconnect = find.byKey(const ValueKey('craft-reconnect'));
      expect(reconnect.hitTestable(), findsOneWidget);
      final failuresAtRest = failedWrites;
      await tester.pump(const Duration(seconds: 30));
      await drain();
      expect(
        failedWrites,
        failuresAtRest,
        reason: 'exhausted repair does not spin forever',
      );
      expect(controller.canvas.hasMarks, isTrue);
      peer.beforeDomainWork = null;
      peer.deliverNotifications = true;
      await tester.tap(reconnect);
      await drain();
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.connectionRecoveryRequired, isFalse);
      expect(reconnect, findsNothing);
      expect(find.textContaining('Board controls unavailable'), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await tester.runAsync(ble.dispose);
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'occupied toast wraps completely inside notched landscape at scale $scale',
      (tester) async {
        tester.view.physicalSize = const Size(852, 393);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = _MutableStatusCraftController(
          ble,
          initialStatus: LitchcraftStatus.boardNotEmpty,
        );
        final library = LitchcraftLibrary(controller);
        await library.ready;
        const insets = EdgeInsets.fromLTRB(59, 0, 59, 21);
        await tester.pumpWidget(
          MaterialApp(
            home: MediaQuery(
              data: MediaQueryData(
                size: const Size(852, 393),
                padding: insets,
                viewPadding: insets,
                textScaler: TextScaler.linear(scale),
              ),
              child: Scaffold(
                body: SafeArea(
                  child: LitchcraftScreen(
                    controller: controller,
                    library: library,
                    rootSafeInsets: insets,
                  ),
                ),
              ),
            ),
          ),
        );
        await tester.pump();
        final message = find.text(
          'Remove all pieces to connect automatically. You can keep painting here.',
        );
        expect(message, findsOneWidget);
        expect(
          tester.renderObject<RenderParagraph>(message).didExceedMaxLines,
          isFalse,
        );
        final toast = tester.getRect(
          find.byKey(const ValueKey('craft-status-card')),
        );
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        expect(toast.left, greaterThanOrEqualTo(canvas.left));
        expect(toast.right, lessThanOrEqualTo(canvas.right));
        expect(toast.top, greaterThanOrEqualTo(canvas.top));
        expect(toast.bottom, lessThanOrEqualTo(canvas.bottom));
        await tester.pump(const Duration(seconds: 4));
        expect(message, findsNothing);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }
  testWidgets(
    'occupied board notice expires once across automatic recovery status changes',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _MutableStatusCraftController(
        ble,
        initialStatus: LitchcraftStatus.boardNotEmpty,
      );
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      const message =
          'Remove all pieces to connect automatically. You can keep painting here.';
      expect(find.text(message), findsOneWidget);
      await tester.pump(const Duration(seconds: 4));
      expect(find.text(message), findsNothing);
      for (var attempt = 0; attempt < 3; attempt++) {
        controller.setStatus(LitchcraftStatus.connecting);
        await tester.pump();
        expect(find.text('Connecting your canvas…'), findsNothing);
        expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        controller.setStatus(LitchcraftStatus.boardNotEmpty);
        await tester.pump(const Duration(seconds: 2));
        expect(find.text(message), findsNothing);
        expect(find.byKey(const ValueKey('craft-connect')), findsNothing);
      }
      controller.setStatus(LitchcraftStatus.live);
      await tester.pump();
      controller.setStatus(LitchcraftStatus.boardNotEmpty);
      await tester.pump();
      await tester.pump();
      expect(
        find.text(message),
        findsOneWidget,
        reason:
            'A new occupied episode after successful admission can explain the next action once.',
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final authoritative in [false, true]) {
    testWidgets(
      'applying a square effect animates immediately while frame playback remains stopped (board=$authoritative)',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = authoritative
            ? (_BoardTimelineCraftController(ble)..playing = false)
            : LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        controller.restoreDocument(
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: [
              for (final color in [0xff0000, 0x0000ff])
                LitchcraftAnimationFrame(
                  emitters: List.generate(
                    256,
                    (_) => LitchcraftAnimatedEmitter(color: color),
                  ),
                ),
            ],
          ),
        );
        var now = 0;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                monotonicMilliseconds: () => now,
              ),
            ),
          ),
        );
        await tester.pump();
        final welcome = find.byKey(const ValueKey('craft-welcome-dismiss'));
        if (welcome.evaluate().isNotEmpty) {
          await tester.tap(welcome);
          await tester.pump();
        }
        await tester.tap(find.byKey(const ValueKey('craft-motion')));
        await tester.pump(const Duration(milliseconds: 300));
        final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
        await _revealInspectorControl(tester, breathe);
        await tester.tap(breathe.hitTestable());
        await tester.pump();
        final rect = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        await tester.tapAt(
          rect.topLeft + Offset(rect.width / 16, rect.height / 16),
        );
        await tester.pump();
        LitchcraftCanvasView canvas() => tester.widget<LitchcraftCanvasView>(
          find.byType(LitchcraftCanvasView),
        );
        final first = canvas().canvas.colors.first;
        now = 500;
        await tester.pump(const Duration(milliseconds: 500));
        expect(
          canvas().canvas.colors.first,
          isNot(first),
          reason: 'An applied effect must advance without ever pressing Play.',
        );
        expect(library.selectedFrameIndex, 0);
        expect(
          canvas().canvas.colors.first & 0xff,
          0,
          reason: 'Effects must not start the blue second frame.',
        );
        expect(
          find.byKey(const ValueKey('craft-animation-playing-indicator')),
          findsNothing,
        );
        if (controller is _BoardTimelineCraftController) {
          expect(controller.effectCalls, 1);
          expect(controller.toggleCalls, 0);
          expect(controller.playing, isFalse);
        }
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }
  testWidgets(
    'compact Fill tap and Clear hold preserve mode and undo atomically',
    (tester) async {
      tester.view.physicalSize = const Size(390, 760);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      Future<void> clearWithHold() async {
        final fill = find.byKey(const ValueKey('craft-quick-fill'));
        final gesture = await tester.startGesture(tester.getCenter(fill));
        await tester.pump(const Duration(milliseconds: 600));
        await gesture.up();
        await tester.pumpAndSettle();
      }

      await controller.setBrush(color: 0x3ab799, eraser: true);
      await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
      await tester.pumpAndSettle();
      expect(
        controller.canvas.colors.every((color) => color == 0x3ab799),
        isTrue,
      );
      expect(
        controller.eraser,
        isTrue,
        reason: 'Fill never toggles the brush mode.',
      );
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pumpAndSettle();
      expect(controller.hasMarks, isFalse);
      expect(
        controller.canUndo,
        isFalse,
        reason: 'Fill creates one undo boundary.',
      );
      await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
      await tester.pumpAndSettle();
      library.applyEffect(
        square: 0,
        mask: 15,
        effect: LitchcraftEffectStyle.breathe,
        effectDurationMs: 1000,
      );
      final beforeClear = library.animationDocument;
      await clearWithHold();
      expect(controller.hasMarks, isFalse);
      expect(
        library.animationDocument.frames.single.emitters.every(
          (emitter) => emitter.effect == LitchcraftEffectStyle.off,
        ),
        isTrue,
      );
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pumpAndSettle();
      expect(
        encodeLitchcraftAnimationDocument(library.animationDocument).bytes,
        encodeLitchcraftAnimationDocument(beforeClear).bytes,
        reason: 'One Clear undo restores paint and effects.',
      );
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'unsupported animated artwork stays editable with board-update guidance',
    (tester) async {
      final semantics = tester.ensureSemantics();
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = _OfflineShellBle();
      final controller = _UnsupportedCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();

      expect(find.textContaining('ausgewählten Frame'), findsNothing);
      expect(
        find.text(
          'Male hier jederzeit. Aktualisiere die Board-Software für Live-Malen.',
        ),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Semantics>(find.byKey(const ValueKey('craft-status-card')))
            .properties
            .liveRegion,
        isTrue,
      );
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .enabled,
        isTrue,
      );
      final action = find.byKey(const ValueKey('craft-send-still-frame'));
      expect(action, findsNothing);

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
      semantics.dispose();
    },
  );

  for (final (status, missingRoot) in <(LitchcraftStatus, bool)>[
    (LitchcraftStatus.unsupported, false),
    (LitchcraftStatus.error, true),
  ]) {
    testWidgets(
      'connected $status with a nine-frame draft previews on the phone',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _ConnectedScreenBle();
        final controller = _UnavailableStoreCraftController(
          ble,
          currentStatus: status,
          missingRoot: missingRoot,
        );
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await _openAnimationTray(tester);
        expect(find.byTooltip('Preview on this device'), findsOneWidget);
        expect(find.byTooltip('Play board'), findsNothing);
        await tester.tap(find.byKey(const ValueKey('craft-preview')));
        await tester.pump();
        expect(controller.boardToggleCalls, 0);
        expect(find.byTooltip('Pause preview on this device'), findsOneWidget);
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  for (final effect in [false, true]) {
    testWidgets(
      'production compact shell at 200 percent keeps compact frame controls and board help reachable',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _OfflineShellBle();
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        expect(library.animationDocument.frames, hasLength(1));
        if (effect) {
          await controller.setBrush(color: 0xff0000);
          controller.beginStroke();
          controller.paintSquares([0]);
          controller.endStroke();
          library.applyEffect(
            square: 0,
            mask: 15,
            effect: LitchcraftEffectStyle.breathe,
            effectDurationMs: 1000,
          );
          expect(
            library.animationDocument.frames.single.emitters.first.effect,
            LitchcraftEffectStyle.breathe,
          );
        }
        tester.view.physicalSize = const Size(640, 360);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(
          MaterialApp(
            locale: const Locale('de'),
            theme: buildLitchTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: const TextScaler.linear(2),
                padding: const EdgeInsets.fromLTRB(20, 0, 12, 16),
                viewPadding: const EdgeInsets.fromLTRB(20, 0, 12, 16),
              ),
              child: child!,
            ),
            home: ConnectionGateScreen(
              bleService: ble,
              craftController: controller,
              craftLibrary: library,
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find
              .descendant(
                of: find.byKey(const ValueKey('disconnected-navigation')),
                matching: find.byType(LitchcraftIdentityGlyph),
              )
              .first,
        );
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('craft-landscape-shell')),
          findsOneWidget,
        );
        expect(library.animationDocument.frames, hasLength(1));
        expect(
          tester
              .widget<LitchcraftScreen>(find.byType(LitchcraftScreen))
              .landscapeHeader,
          isNull,
          reason: 'Board options stay outside the creative workspace',
        );
        expect(
          find.byKey(const ValueKey('craft-landscape-navigation-surface')),
          findsOneWidget,
        );
        final launcher = find.byKey(const ValueKey('craft-animate'));
        expect(launcher.hitTestable(), findsOneWidget);
        await tester.tap(launcher);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        if (effect) {
          final play = find.byKey(const ValueKey('craft-preview'));
          expect(play.hitTestable(), findsOneWidget);
          expect(tester.getSize(play).shortestSide, greaterThanOrEqualTo(48));
          await tester.tap(play);
          await tester.pump();
          expect(find.byIcon(Icons.pause), findsOneWidget);
          await tester.tap(play);
          await tester.pumpAndSettle();
          expect(find.byIcon(Icons.play_arrow), findsOneWidget);
        }
        expect(
          find.byKey(const ValueKey('craft-board-controls')),
          findsNothing,
        );
        expect(
          find.byKey(const ValueKey<String>('connected-board-status-action')),
          findsOneWidget,
        );
        library.addFrame();
        library.addFrame();
        library.selectFrame(1);
        for (final key in <String>[
          'craft-frame-previous',
          'craft-preview',
          'craft-frame-next',
          'craft-add-frame',
          'craft-frame-filmstrip',
        ]) {
          final control = find.byKey(ValueKey(key));
          expect(control, findsOneWidget);
          await tester.pump(const Duration(milliseconds: 200));
          expect(
            tester.getSize(control).shortestSide,
            greaterThanOrEqualTo(48),
          );
          expect(control.hitTestable(), findsOneWidget);
        }
        expect(
          find.byKey(const ValueKey('craft-delete-current-frame')),
          findsNothing,
          reason: 'Frame deletion is a confirmed filmstrip long-press.',
        );
        final filmstrip = tester.getRect(
          find.byKey(const ValueKey('craft-frame-filmstrip')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        expect(inspector.contains(filmstrip.topLeft), isTrue);
        expect(inspector.contains(filmstrip.bottomRight), isTrue);
        expect(
          tester.widget<PageView>(
            find.byKey(const ValueKey('main-pages')),
          ).controller!.page,
          1,
          reason: 'Inspector controls do not scroll the app navigation.',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  for (final stop in ['Pause', 'Inspector switch', 'preferences', 'Drawing']) {
    testWidgets(
      'detached distinct frames follow playback and $stop adopts the displayed paint target',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        controller.restoreDocument(
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: [
              for (final color in [0xff0000, 0x0000ff])
                LitchcraftAnimationFrame(
                  emitters: List.generate(
                    256,
                    (_) => LitchcraftAnimatedEmitter(color: color),
                  ),
                ),
            ],
          ),
        );
        Widget app(bool showTips) => MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => AppPreferencesScope(
            appearanceMode: LitchAppearanceMode.system,
            onAppearanceModeChanged: (_) async {},
            localePreference: const LitchLocalePreference.system(),
            onLocalePreferenceChanged: (_) async {},
            showTips: showTips,
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        );
        await tester.pumpWidget(app(true));
        await tester.pumpAndSettle();
        await _openAnimationTray(tester);
        LitchcraftCanvasView canvas() => tester.widget<LitchcraftCanvasView>(
          find.byType(LitchcraftCanvasView),
        );
        expect(canvas().canvas.colors.first, 0xff0000);
        final initialThumbnailCenters = [
          for (var index = 0; index < 2; index++)
            tester
                .getCenter(find.byKey(ValueKey('craft-select-frame-$index')))
                .dx,
        ];
        void expectFitAllThumbnailsStayVisible() {
          final viewport = tester.getRect(
            find.byKey(const ValueKey('craft-frame-scroll-viewport')),
          );
          for (var index = 0; index < 2; index++) {
            final rect = tester.getRect(
              find.byKey(ValueKey('craft-select-frame-$index')),
            );
            expect(rect.left, greaterThanOrEqualTo(viewport.left));
            expect(rect.right, lessThanOrEqualTo(viewport.right));
            expect(
              rect.center.dx,
              closeTo(initialThumbnailCenters[index], .01),
              reason: 'When all frames fit, playback must not scroll the list',
            );
          }
        }

        await tester.tap(find.byKey(const ValueKey('craft-preview')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 1200));
        expect(
          canvas().canvas.colors.first,
          0x0000ff,
          reason: 'production compositor crossed the frame boundary',
        );
        expect(
          tester
              .widget<IconButton>(
                find.byKey(const ValueKey('craft-select-frame-1')),
              )
              .isSelected,
          isFalse,
          reason:
              'The playing frame has a playback marker, not edit-selection styling.',
        );
        expect(
          tester
              .getSemantics(find.byKey(const ValueKey('craft-select-frame-1')))
              .label,
          'Playing frame 2',
        );
        expectFitAllThumbnailsStayVisible();
        await tester.pump(const Duration(milliseconds: 1000));
        expect(canvas().canvas.colors.first, 0xff0000);
        expect(
          tester
              .widget<IconButton>(
                find.byKey(const ValueKey('craft-select-frame-0')),
              )
              .isSelected,
          isFalse,
          reason:
              'Playback wraps with a playing marker instead of edit selection.',
        );
        expect(
          tester
              .getSemantics(find.byKey(const ValueKey('craft-select-frame-0')))
              .label,
          'Playing frame 1',
        );
        expectFitAllThumbnailsStayVisible();
        await tester.pump(const Duration(milliseconds: 1000));
        expect(canvas().canvas.colors.first, 0x0000ff);
        if (stop == 'Drawing') {
          await controller.setBrush(color: 0x00ff00);
          await tester.pump();
          final rect = tester.getRect(
            find.byKey(const ValueKey('litchcraft-canvas')),
          );
          final stroke = await tester.startGesture(
            rect.topLeft + Offset(rect.width / 16, rect.height / 16),
          );
          await stroke.moveBy(Offset(rect.width / 8, 0));
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 1200));
          expect(library.selectedFrameIndex, 1);
          expect(canvas().canvas.colors.first, 0x00ff00);
          expect(find.byIcon(Icons.pause), findsNothing);
          await stroke.up();
        } else if (stop == 'preferences') {
          await tester.pumpWidget(app(false));
        } else if (stop == 'Pause') {
          await tester.tap(find.byKey(const ValueKey('craft-preview')));
        } else if (stop == 'Inspector switch') {
          await tester.tap(find.byKey(const ValueKey('craft-palette')));
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await _openAnimationTray(tester);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
        }
        await tester.pump();
        expect(library.selectedFrameIndex, 1);
        if (stop != 'preferences') {
          expect(find.bySemanticsLabel('Frame 2 of 2'), findsWidgets);
        }
        expect(find.byIcon(Icons.pause), findsNothing);
        expect(
          canvas().canvas.colors.first,
          stop == 'Drawing' ? 0x00ff00 : 0x0000ff,
        );
        await controller.setBrush(color: 0x00ff00);
        await tester.pump();
        final rect = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        await tester.tapAt(
          rect.topLeft + Offset(rect.width / 16, rect.height / 16),
        );
        await tester.pump();
        expect(
          library.animationDocument.frames[0].emitters.first.color,
          0xff0000,
        );
        expect(
          library.animationDocument.frames[1].emitters.first.color,
          0x00ff00,
        );
        expect(
          canvas().canvas.colors.first,
          0x00ff00,
          reason: 'rendered canvas and newly painted frame agree',
        );
        if (stop == 'Pause') {
          final preview = find.byKey(const ValueKey('craft-preview'));
          final first = find.byKey(const ValueKey('craft-select-frame-0'));
          await tester.tap(preview);
          await tester.pump();
          expect(canvas().canvas.colors.first, 0x00ff00);
          await tester.tap(first);
          await tester.pump();
          expect(find.byIcon(Icons.pause), findsNothing);
          expect(library.selectedFrameIndex, 0);
          expect(tester.widget<IconButton>(first).isSelected, isTrue);
          expect(canvas().canvas.colors.first, 0xff0000);

          await tester.tap(preview);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 1200));
          expect(canvas().canvas.colors.first, 0x00ff00);
          await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
          await tester.pump();
          final frames = library.animationDocument.frames;
          expect(frames, hasLength(3));
          expect(
            frames[2].emitters.map((emitter) => emitter.color),
            frames[1].emitters.map((emitter) => emitter.color),
          );
          expect(library.selectedFrameIndex, 2);
          expect(
            tester
                .widget<IconButton>(
                  find.byKey(const ValueKey('craft-select-frame-2')),
                )
                .isSelected,
            isTrue,
          );
          expect(find.byIcon(Icons.pause), findsNothing);
          expect(canvas().canvas.colors.first, 0x00ff00);
        }
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  for (final size in [const Size(390, 844), const Size(1000, 472.5)]) {
    testWidgets('full animation sync uses the canvas message at $size', (
      tester,
    ) async {
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ArtworkDeliveryController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      for (final (direction, label) in <(LitchcraftFullSyncDirection, String)>[
        (
          LitchcraftFullSyncDirection.sendingToBoard,
          'Sending animation to your Litch…',
        ),
        (
          LitchcraftFullSyncDirection.readingFromBoard,
          'Reading animation from your Litch…',
        ),
      ]) {
        controller.showFullSync(direction, .4);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        expect(find.text(label), findsOneWidget);
        final card = find.byKey(const ValueKey('craft-status-card'));
        final bar = find.byKey(const ValueKey('craft-full-animation-sync'));
        expect(card, findsOneWidget);
        expect(bar, findsOneWidget);
        expect(tester.widget<LinearProgressIndicator>(bar).value, .4);
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final cardRect = tester.getRect(card);
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        expect(cardRect.top, closeTo(canvas.top + 12, 1));
        expect(canvas.overlaps(cardRect), isTrue);
        expect(inspector.overlaps(cardRect), isFalse);
      }
      controller.showFullSync(null, null);
      await tester.pump(const Duration(milliseconds: 200));
      expect(
        find.byKey(const ValueKey('craft-full-animation-sync')),
        findsNothing,
      );
      controller.showRootPending(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('Sending artwork…'), findsOneWidget);
      final pendingRoot = find.byKey(
        const ValueKey('craft-store-root-pending'),
      );
      expect(pendingRoot, findsOneWidget);
      expect(tester.widget<LinearProgressIndicator>(pendingRoot).value, isNull);
      expect(
        find.byKey(const ValueKey('craft-full-animation-sync')),
        findsNothing,
      );
      controller.showRootPending(false);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      expect(pendingRoot, findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    });
  }

  testWidgets(
    'distinct Fade filmstrip thumbnails preserve intended artwork at the dark trough',
    (tester) async {
      final document = LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: [
          for (final color in [0xff0000, 0x0000ff])
            LitchcraftAnimationFrame(
              emitters: List.generate(
                256,
                (index) => LitchcraftAnimatedEmitter(
                  color: index == 0 ? 0xffffff : color,
                  effect: LitchcraftEffectStyle.fadeToBlack,
                  effectDurationMs: 1000,
                ),
              ),
            ),
        ],
      );
      expect(
        composeLitchcraftAnimationDocument(
          document,
          0,
        ).colors.every((color) => color == 0),
        isTrue,
      );
      for (var index = 0; index < 2; index++) {
        await tester.pumpWidget(
          MaterialApp(
            home: Center(
              child: LitchcraftFrameThumbnail(
                document: document,
                frameIndex: index,
              ),
            ),
          ),
        );
        final painter = tester
            .widget<CustomPaint>(
              find.descendant(
                of: find.byType(LitchcraftFrameThumbnail),
                matching: find.byType(CustomPaint),
              ),
            )
            .painter!;
        final recorder = ui.PictureRecorder();
        painter.paint(ui.Canvas(recorder), const Size(28, 28));
        final picture = recorder.endRecording();
        final image = picture.toImageSync(28, 28);
        final pixels = await tester.runAsync(() => image.toByteData());
        final offset = (20 * 28 + 20) * 4;
        expect(pixels!.getUint8(offset), index == 0 ? 255 : 0);
        expect(pixels.getUint8(offset + 2), index == 0 ? 0 : 255);
        image.dispose();
        picture.dispose();
      }
      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'connected Store frame tap selects the board and displayed canvas',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = _ConnectedScreenBle();
      final controller = _StoreTimelineCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      controller.restoreDocument(
        LitchcraftAnimationDocument(
          topology: LitchcraftTopology.v3,
          favoriteColors: litchcraftDefaultFavoriteColors,
          frames: [
            for (final color in [0xff0000, 0x0000ff])
              LitchcraftAnimationFrame(
                emitters: List.generate(
                  256,
                  (_) => LitchcraftAnimatedEmitter(color: color),
                ),
              ),
          ],
        ),
      );
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-animate')).hitTestable(),
        findsOneWidget,
      );
      await _openAnimationTray(tester);
      LitchcraftCanvasView canvas() => tester.widget<LitchcraftCanvasView>(
        find.byType(LitchcraftCanvasView),
      );
      expect(canvas().canvas.colors.first, 0xff0000);
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-1')));
      await tester.pump();
      expect(controller.selectCalls, 1);
      expect(controller.selectedFrame, 1);
      expect(canvas().canvas.colors.first, 0x0000ff);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final captureEnabled in [true, false]) {
    testWidgets(
      'Store timing drag keeps its captured target: $captureEnabled',
      (tester) async {
        tester.view.physicalSize = const Size(844, 390);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _ConnectedScreenBle();
        final controller = _StoreTimelineCraftController(ble)
          ..playing = false
          ..captureEnabled = captureEnabled;
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.addFrame();
        library.selectFrame(1);
        controller.selectedFrame = 1;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await _openAnimationTray(tester);
        final slider = find.byKey(const ValueKey('craft-frame-duration'));
        final original = library.animationDocument;
        final rect = tester.getRect(slider);
        final gesture = await tester.startGesture(
          Offset(rect.left + 24, rect.center.dy),
        );
        await tester.pump();
        await gesture.moveTo(
          rect.center,
          timeStamp: const Duration(milliseconds: 80),
        );
        await tester.pump();
        final heldValue = tester.widget<Slider>(slider).value;
        controller.selectedFrame = 0;
        library.selectFrame(0);
        controller.notifyListeners();
        await tester.pump(const Duration(milliseconds: 50));
        expect(tester.widget<Slider>(slider).value, heldValue);
        expect(library.animationDocument, original);
        await gesture.moveTo(
          Offset(rect.right - 24, rect.center.dy),
          timeStamp: const Duration(milliseconds: 160),
        );
        await tester.pump();
        expect(library.animationDocument, original);
        await gesture.up();
        await tester.pump();
        expect(controller.durationCalls, captureEnabled ? 1 : 0);
        if (captureEnabled) {
          expect(controller.lastDurationTarget?.frame, 1);
          expect(
            library.animationDocument.frames[0].durationMs,
            original.frames[0].durationMs,
          );
          expect(
            library.animationDocument.frames[1].durationMs,
            isNot(original.frames[1].durationMs),
          );
          await tester.longPress(
            find.byKey(const ValueKey('craft-select-frame-0')),
          );
          await tester.pumpAndSettle();
          expect(find.text('Delete frame 1?'), findsOneWidget);
          expect(library.selectedFrameIndex, 1);
          expect(controller.selectedFrame, 1);
          await tester.tap(find.text('Cancel'));
          await tester.pumpAndSettle();
          expect(library.selectedFrameIndex, 1);
          expect(controller.selectedFrame, 1);
          expect(controller.deleteFrameCalls, 0);
        } else {
          expect(library.animationDocument, original);
          final retiredPreview = find.byKey(
            const ValueKey('craft-status-card'),
          );
          expect(retiredPreview, findsOneWidget);
          expect(
            tester.getSemantics(retiredPreview).flagsCollection.isLiveRegion,
            isFalse,
          );
          await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
          await tester.pump();
          expect(controller.addFrameCalls, 0);
          final deleteTarget = find.byKey(
            const ValueKey('craft-select-frame-1'),
          );
          await tester.ensureVisible(deleteTarget);
          await tester.pumpAndSettle();
          expect(deleteTarget.hitTestable(), findsOneWidget);
          await tester.longPress(deleteTarget);
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-confirm-delete-frame')),
            findsNothing,
          );
          expect(controller.deleteFrameCalls, 0);
          expect(library.animationDocument, original);
        }
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  testWidgets('Play starts from the selected live board frame', (tester) async {
    tester.view.physicalSize = const Size(640, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = _ConnectedScreenBle();
    final controller = _BoardTimelineCraftController(ble)..playing = false;
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    library.selectFrame(1);
    controller.selectedFrame = 1;
    controller.documentSession.selectFrame(1);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final tray = tester.getRect(
      find.byKey(const ValueKey('craft-portrait-tools-tray')),
    );
    final board = tester.getRect(
      find.byKey(const ValueKey('craft-ribbon-board')),
    );
    expect(tray.top, greaterThan(board.bottom));
    expect(tray.width, 640 - 2 * LitchWorkspaceGeometry.contentEdgeInset);
    final colorField = tester.getSize(
      find.byKey(const ValueKey('craft-inspector-color-field')),
    );
    expect(colorField.shortestSide, greaterThanOrEqualTo(48));
    await _openAnimationTray(tester);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    expect(controller.toggleCalls, 1);
    expect(controller.selectCalls, 0);
    expect(controller.selectedFrame, 1);
    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'live board playback and frame selection never start phone preview',
    (tester) async {
      final semantics = tester.ensureSemantics();
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _BoardTimelineCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.addFrame();
      library.selectFrame(0);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _openAnimationTray(tester);
      expect(find.byIcon(Icons.pause), findsOneWidget);
      expect(find.byTooltip('Pause board'), findsOneWidget);
      expect(find.bySemanticsLabel('Pause board'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-animation-playing-indicator')),
        findsOneWidget,
      );
      expect(
        find.bySemanticsLabel('Play board'),
        findsNothing,
        reason: 'the visual playing badge cannot announce a second Play action',
      );
      controller.toggleRelease = Completer<void>();
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump();
      expect(controller.toggleCalls, 1);
      expect(find.byIcon(Icons.play_arrow), findsOneWidget);
      expect(find.byTooltip('Play board'), findsOneWidget);
      expect(find.bySemanticsLabel('Play board'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-animation-playing-indicator')),
        findsNothing,
      );
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .enabled,
        isTrue,
        reason:
            'A pending play/pause acknowledgement must not disable the editor.',
      );
      expect(
        tester
            .widget<InkWell>(
              find.descendant(
                of: find.byKey(const ValueKey('craft-palette')),
                matching: find.byType(InkWell),
              ),
            )
            .onTap,
        isNotNull,
      );
      controller.toggleRelease!.complete();
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-1')));
      await tester.pump();
      expect(controller.selectCalls, 1);
      expect(controller.selectedFrame, 1);
      await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
      await tester.pumpAndSettle();
      expect(controller.addFrameCalls, 1);
      expect(library.animationDocument.frames, hasLength(3));
      await tester.tap(find.byKey(const ValueKey('craft-motion')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('craft-board-controls')), findsNothing);
      final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
      for (
        var attempt = 0;
        attempt < 6 && breathe.hitTestable().evaluate().isEmpty;
        attempt++
      ) {
        await tester.drag(
          find.byKey(const ValueKey('craft-inspector-scroll')),
          const Offset(0, -120),
        );
        await tester.pump(const Duration(milliseconds: 200));
      }
      expect(breathe.hitTestable(), findsOneWidget);
      await tester.tap(breathe);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-active-effect')), findsOneWidget);
      expect(
        tester.getSemantics(breathe),
        matchesSemantics(
          isButton: true,
          isEnabled: true,
          hasEnabledState: true,
          isSelected: true,
          hasSelectedState: true,
          isFocusable: true,
          hasFocusAction: true,
          hasTapAction: true,
        ),
      );
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      await tester.tapAt(
        canvas.topLeft + Offset(canvas.width * 5 / 16, canvas.height * 5 / 16),
      );
      await tester.pump();
      expect(controller.effectCalls, 1);

      await _openAnimationTray(tester);
      await tester.ensureVisible(
        find.byKey(const ValueKey('craft-select-frame-2')),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-select-frame-2')).hitTestable(),
        findsOneWidget,
      );
      await tester.longPress(
        find.byKey(const ValueKey('craft-select-frame-2')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Delete frame 3?'), findsOneWidget);
      controller.selectedFrame = 0;
      controller.documentSession.selectFrame(0);
      controller.notifyListeners();
      await tester.pump();
      await tester.tap(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
      );
      await tester.pumpAndSettle();
      expect(controller.deleteFrameCalls, 1);
      expect(controller.lastDeleteTarget?.frame, 2);
      expect(library.animationDocument.frames, hasLength(2));
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
      semantics.dispose();
    },
  );

  testWidgets(
    'queued board Play and rejection keep actionable status visible',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _BoardTimelineCraftController(ble)..playing = false;
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.addFrame();
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _openAnimationTray(tester);
      controller.queuedPlay = true;
      controller.notifyListeners();
      await tester.pump();
      expect(find.byTooltip('Cancel Play board'), findsWidgets);
      expect(
        find.byKey(const ValueKey('craft-cancel-queued-play')),
        findsOneWidget,
      );
      expect(find.text('Play when artwork is ready'), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('craft-cancel-queued-play')));
      await tester.pump();
      expect(controller.queuedPlay, isFalse);

      controller.failedPlay = true;
      controller.notifyListeners();
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-retry-queued-play')),
        findsOneWidget,
      );
      expect(
        find.text(
          'That action could not finish. Your canvas is still here. Try again.',
        ),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-retry-queued-play')));
      await tester.pump();
      expect(controller.failedPlay, isFalse);
      expect(controller.playing, isTrue);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('Board controls stay outside Litchcraft at 200 percent', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _BoardTimelineCraftController(ble)..playing = false;
    final library = LitchcraftLibrary(controller);
    await library.ready;
    for (final size in <Size>[const Size(320, 520), const Size(844, 390)]) {
      tester.view.physicalSize = size;
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: MediaQuery(
            data: MediaQueryData(
              size: size,
              textScaler: const TextScaler.linear(2),
            ),
            child: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-board-controls')), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-board-controls-scroll')),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('connected-board-status-action')),
        findsNothing,
      );
      // Detailed physical controls and Busy recovery remain covered in the
      // Manual's real-font 200% tests at these same two viewport sizes.
      expect(tester.takeException(), isNull);
    }
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'landscape busy recovery floats without blocking creative controls at 200 percent',
    (tester) async {
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _BoardTimelineCraftController(ble)..busy = false;
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: MediaQuery(
            data: const MediaQueryData(
              size: Size(844, 390),
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));

      final canvasBefore = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      controller.busy = true;
      controller.notifyListeners();
      await tester.pump(const Duration(milliseconds: 300));

      final status = tester.getRect(
        find.byKey(const ValueKey('craft-status-card')),
      );
      final inspector = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      for (final key in <String>['craft-toolbar', 'craft-quick-actions']) {
        final control = find.byKey(ValueKey(key));
        if (control.evaluate().isNotEmpty) {
          expect(
            control.hitTestable(),
            findsOneWidget,
            reason: 'non-interactive recovery must not block $key',
          );
        }
      }
      expect(inspector.overlaps(status), isFalse);
      expect(
        status.overlaps(
          tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        ),
        isTrue,
        reason: 'recovery uses the same top-of-canvas message as other hints',
      );
      expect(status.top, closeTo(canvasBefore.top + 12, 1));
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        canvasBefore,
        reason: 'a live recovery transition must not resize the canvas',
      );
      expect(
        tester.getSize(find.byKey(const ValueKey('litchcraft-canvas'))).width,
        greaterThanOrEqualTo(220),
      );
      final details = find.byKey(const ValueKey('craft-status-details'));
      expect(details, findsNothing);
      expect(find.byType(BottomSheet), findsNothing);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'narrow landscape transport recovery stays automatic in both rotations',
    (tester) async {
      tester.view.physicalSize = const Size(540, 360);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ConflictCraftController(ble)
        ..currentStatus = LitchcraftStatus.error;
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;

      for (final side in <Alignment>[
        Alignment.centerLeft,
        Alignment.centerRight,
      ]) {
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: MediaQuery(
              data: const MediaQueryData(
                size: Size(540, 360),
                textScaler: TextScaler.linear(2),
                padding: EdgeInsets.fromLTRB(12, 8, 16, 12),
                viewPadding: EdgeInsets.fromLTRB(12, 8, 16, 12),
              ),
              child: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                  landscapeRibbonSide: side,
                ),
              ),
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 300));

        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final safeRect = Rect.fromLTRB(12, 8, 540 - 16, 360 - 12);
        expect(safeRect.overlaps(canvas), isTrue, reason: 'side=$side');
        expect(find.textContaining('Board controls unavailable'), findsNothing);
        final retry = find.byKey(const ValueKey('craft-connect'));
        expect(retry, findsNothing, reason: 'side=$side');
        expect(tester.takeException(), isNull);
      }

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'native landscape direction keeps the rail thumb-side without remounting the editor',
    (tester) async {
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _BoardTimelineCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      var side = DeviceLandscapeSide.left;
      var reads = 0;
      Future<DeviceLandscapeSide?> readSide() async {
        reads++;
        return side;
      }

      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              padding: const EdgeInsets.fromLTRB(30, 0, 24, 18),
              viewPadding: const EdgeInsets.fromLTRB(30, 0, 24, 18),
            ),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              deviceLandscapeSideReader: readSide,
            ),
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      final state = tester.state(find.byType(LitchcraftScreen));
      Future<void> expectSafeMenu() async {
        await tester.tap(find.byKey(const ValueKey('craft-leds')));
        await tester.pumpAndSettle();
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        expect(inspector.left, greaterThanOrEqualTo(30));
        expect(inspector.right, lessThanOrEqualTo(820));
        expect(inspector.top, greaterThanOrEqualTo(0));
        expect(inspector.bottom, lessThanOrEqualTo(372));
        expect(inspector.overlaps(canvas), isFalse);
        final target = find.byKey(const ValueKey('craft-target-15'));
        await _revealInspectorControl(tester, target);
        await tester.tap(target.hitTestable());
        await tester.pumpAndSettle();
      }

      expect(
        tester.getRect(find.byKey(const ValueKey('craft-toolbar'))).left,
        closeTo(
          tester
              .getRect(find.byKey(const ValueKey('craft-persistent-inspector')))
              .left,
          .01,
        ),
      );
      await expectSafeMenu();

      side = DeviceLandscapeSide.right;
      tester.binding.handleMetricsChanged();
      await tester.pump();
      await tester.pump();
      expect(tester.state(find.byType(LitchcraftScreen)), same(state));
      expect(reads, greaterThanOrEqualTo(2));
      expect(
        tester.getRect(find.byKey(const ValueKey('craft-toolbar'))).right,
        closeTo(
          tester
              .getRect(find.byKey(const ValueKey('craft-persistent-inspector')))
              .right,
          .01,
        ),
      );
      await expectSafeMenu();

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'landscape gives one third to tools and opposite hand three open wells',
    (tester) async {
      final haptics = _recordHoldHaptics(tester);
      final semantics = tester.ensureSemantics();
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              landscapeRibbonSide: Alignment.centerLeft,
            ),
          ),
        ),
      );
      await tester.pump();

      final inspector = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      final board = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final quick = tester.getRect(
        find.byKey(const ValueKey('craft-quick-actions')),
      );
      expect(inspector.right, lessThanOrEqualTo(board.left));
      // The safe shell now consumes each edge once; keep the explicit 12px
      // inter-group gutter instead of depending on duplicated outside padding.
      expect(quick.left - board.right, greaterThanOrEqualTo(12));
      for (final key in <String>[
        'craft-quick-brush',
        'craft-quick-previous-color',
        'craft-quick-fill',
      ]) {
        expect(
          tester.getSize(find.byKey(ValueKey(key))).shortestSide,
          greaterThanOrEqualTo(72),
        );
      }
      final currentColorDot = tester.getSize(
        find.byKey(const ValueKey('craft-current-color-dot')),
      );
      final previousColorDot = tester.getSize(
        find.byKey(const ValueKey('craft-previous-color-dot')),
      );
      expect(
        currentColorDot.shortestSide,
        greaterThan(previousColorDot.shortestSide),
      );
      expect(currentColorDot.shortestSide, greaterThanOrEqualTo(30));
      final inspectorMaterial = tester.widget<Material>(
        find.byKey(const ValueKey('craft-inspector')),
      );
      expect(inspectorMaterial.shape, isA<RoundedRectangleBorder>());
      expect(find.byKey(const ValueKey('craft-paint')), findsNothing);
      expect(find.byKey(const ValueKey('craft-erase')), findsNothing);
      expect(find.byKey(const ValueKey('craft-pick-color')), findsNothing);
      expect(find.byKey(const ValueKey('craft-fill')), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-inspector-previous-color')),
        findsNothing,
      );
      final brushSemantics = tester.getSemantics(
        find.byKey(const ValueKey('craft-quick-brush-semantics')),
      );
      expect(brushSemantics.label, 'Paint');
      expect(brushSemantics.hint, 'Erase mode');
      expect(
        brushSemantics.getSemanticsData().hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      final previousSemantics = tester.getSemantics(
        find.byKey(const ValueKey('craft-quick-previous-color-semantics')),
      );
      expect(previousSemantics.label, 'Swap current and previous colors');
      expect(previousSemantics.hint, 'Pick color from canvas');
      expect(
        previousSemantics.getSemanticsData().hasAction(
          ui.SemanticsAction.longPress,
        ),
        isTrue,
      );
      expect(
        previousSemantics.flagsCollection.isEnabled,
        ui.Tristate.isTrue,
        reason: 'eyedropper hold remains enabled before a prior color exists',
      );
      final fillSemantics = tester.getSemantics(
        find.byKey(const ValueKey('craft-quick-fill-semantics')),
      );
      expect(fillSemantics.label, 'Fill canvas');
      expect(fillSemantics.hint, 'Clear all');
      expect(
        fillSemantics.getSemanticsData().hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      expect(
        fillSemantics.getSemanticsData().hasAction(
          ui.SemanticsAction.longPress,
        ),
        isTrue,
      );

      await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
      await tester.pump();
      expect(controller.eraser, isTrue);
      previousSemantics.owner!.performAction(
        previousSemantics.id,
        ui.SemanticsAction.longPress,
      );
      await tester.pump();
      expect(haptics, ['HapticFeedbackType.mediumImpact']);
      haptics.clear();
      expect(
        find.byKey(const ValueKey('craft-quick-eyedropper-active')),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Icon>(
              find.byKey(const ValueKey('craft-quick-eyedropper-active')),
            )
            .size,
        closeTo(25.6, .01),
      );
      await tester.tap(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-quick-eyedropper-active')),
        findsNothing,
        reason: 'Tapping the active eyedropper cancels color pickup.',
      );
      await tester.longPress(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pump();
      expect(haptics, [
        'HapticFeedbackType.lightImpact',
        'HapticFeedbackType.mediumImpact',
      ]);
      haptics.clear();
      await tester.tapAt(
        tester.getCenter(find.byKey(const ValueKey('litchcraft-canvas'))),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-quick-eyedropper-active')),
        findsNothing,
      );

      await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
      await tester.pump();
      expect(controller.canvas.colors, everyElement(isNot(0)));
      final clearNode = tester.getSemantics(
        find.byKey(const ValueKey('craft-quick-fill-semantics')),
      );
      clearNode.owner!.performAction(
        clearNode.id,
        ui.SemanticsAction.longPress,
      );
      await tester.pump();
      expect(controller.canvas.colors, everyElement(0));
      expect(haptics, ['HapticFeedbackType.mediumImpact']);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
      semantics.dispose();
    },
  );

  testWidgets(
    'presentation flip animates canvas and icons without rotating toolbar chrome',
    (tester) async {
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _FlippableCraftController(ble);
      final library = LitchcraftLibrary(controller);
      final paneWidth = ValueNotifier<double>(844);
      addTearDown(paneWidth.dispose);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: ValueListenableBuilder<double>(
              valueListenable: paneWidth,
              builder: (_, width, child) => Align(
                alignment: Alignment.centerLeft,
                child: SizedBox(width: width, child: child),
              ),
              child: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pump();
      final toolbar = find.byKey(const ValueKey('craft-toolbar'));
      final toolbarBefore = tester.getRect(toolbar);
      final boardBefore = tester.getRect(
        find.byKey(const ValueKey('craft-ribbon-board')),
      );

      controller.flipPresentation();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      Transform transitionFor(Key key) => tester.widget<Transform>(
        find
            .ancestor(of: find.byKey(key), matching: find.byType(Transform))
            .first,
      );
      expect(
        transitionFor(
          const ValueKey('craft-canvas-orientation-transition'),
        ).transform.storage[0].abs(),
        lessThan(.99),
      );
      expect(
        transitionFor(
          const ValueKey('craft-toolbar-icon-transition-palette'),
        ).transform.storage[0].abs(),
        lessThan(.99),
      );
      expect(tester.getRect(toolbar), toolbarBefore);
      expect(
        tester.getRect(find.byKey(const ValueKey('craft-ribbon-board'))),
        boardBefore,
      );
      await tester.pump(const Duration(milliseconds: 160));
      expect(
        transitionFor(
          const ValueKey('craft-canvas-orientation-transition'),
        ).transform.storage[0],
        moreOrLessEquals(1, epsilon: .001),
      );
      expect(tester.getRect(toolbar), toolbarBefore);
      expect(tester.takeException(), isNull);

      // A pane reflow is not a second host orientation event. Moving the
      // retained editor across its layout breakpoint must not replay rotation.
      for (final width in [479.0, 844.0, 479.0, 844.0]) {
        paneWidth.value = width;
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        expect(
          transitionFor(
            const ValueKey('craft-canvas-orientation-transition'),
          ).transform.storage[0],
          moreOrLessEquals(1, epsilon: .001),
        );
        await controller.setBrush(color: 0x123456);
        await tester.pump(const Duration(milliseconds: 40));
        expect(
          transitionFor(
            const ValueKey('craft-canvas-orientation-transition'),
          ).transform.storage[0],
          moreOrLessEquals(1, epsilon: .001),
        );
      }

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('rejected live effect never announces success', (tester) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _BoardTimelineCraftController(ble)
      ..playing = false
      ..acceptEffects = false;
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.tap(find.byKey(const ValueKey('craft-motion')));
    await tester.pump(const Duration(milliseconds: 300));
    final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
    await _revealInspectorControl(tester, breathe);
    await tester.tap(breathe.hitTestable());
    await tester.pump(const Duration(milliseconds: 100));
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    await tester.tapAt(canvas.center);
    await tester.pump(const Duration(milliseconds: 300));

    expect(controller.effectCalls, 1);
    final strings = AppLocalizations.of(
      tester.element(find.byType(LitchcraftScreen)),
    );
    expect(find.text(strings.craftEffectApplied), findsNothing);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'landscape persistent inspector keeps every creative control at 200 percent',
    (tester) async {
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: MediaQuery(
            data: const MediaQueryData(
              size: Size(844, 390),
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );
      final openCanvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pump(const Duration(milliseconds: 300));
      final detail = find.byKey(const ValueKey('craft-target-4'));
      expect(
        detail.hitTestable(),
        findsOneWidget,
        reason:
            'the persistent inspector exposes LED targets without scrolling',
      );
      await tester.tap(detail.hitTestable());
      await tester.pump();
      expect(controller.targetMask, 4);
      await tester.tapAt(openCanvas.center);
      await tester.pump(const Duration(milliseconds: 120));
      expect(find.byKey(const ValueKey('craft-target-4')), findsOneWidget);
      expect(controller.targetMask, 4);
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        openCanvas,
      );
      expect(find.byKey(const ValueKey('craft-ribbon-scroll')), findsNothing);
      expect(find.byType(Scrollbar), findsNothing);

      await tester.tap(find.byKey(const ValueKey('craft-palette')));
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        find.byKey(const ValueKey('craft-target-4')).hitTestable(),
        findsNothing,
      );
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );

      expect(find.byKey(const ValueKey('craft-ribbon-scroll')), findsNothing);
      expect(find.byType(Scrollbar), findsNothing);
      final inspectorRect = tester.getRect(
        find.byKey(const ValueKey('craft-landscape-shell')),
      );
      for (final key in <String>['craft-toolbar']) {
        final rect = tester.getRect(find.byKey(ValueKey(key)));
        expect(rect.left, greaterThanOrEqualTo(inspectorRect.left));
        expect(rect.top, greaterThanOrEqualTo(inspectorRect.top));
        expect(rect.right, lessThanOrEqualTo(inspectorRect.right + .5));
        expect(
          rect.bottom,
          lessThanOrEqualTo(inspectorRect.bottom + .5),
          reason: '$key remains fully visible after tray dismissal',
        );
      }
      for (final key in <String>[
        'craft-palette',
        'craft-undo',
        'craft-redo',
        'craft-leds',
        'craft-motion',
        'craft-animate',
        'craft-artwork-menu',
      ]) {
        expect(
          find.byKey(ValueKey(key)).hitTestable(),
          findsOneWidget,
          reason: '$key remains visible around the persistent inspector',
        );
      }
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'compact landscape target row stays in the safe pane and announces selection',
    (tester) async {
      final semantics = tester.ensureSemantics();
      tester.view.physicalSize = const Size(667, 375);
      for (final scale in <double>[1, 2]) {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: MediaQuery(
              data: MediaQueryData(
                size: const Size(667, 375),
                padding: const EdgeInsets.fromLTRB(12, 8, 20, 10),
                viewPadding: const EdgeInsets.fromLTRB(12, 8, 20, 10),
                textScaler: TextScaler.linear(scale),
              ),
              child: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          tester
              .getSemantics(
                find.byKey(const ValueKey('craft-quick-brush-semantics')),
              )
              .flagsCollection
              .isSelected,
          ui.Tristate.isTrue,
        );
        await tester.tap(find.byKey(const ValueKey('craft-leds')));
        await tester.pumpAndSettle();

        const safePane = Rect.fromLTRB(12, 8, 647, 365);
        final canvasFinder = find.byKey(const ValueKey('litchcraft-canvas'));
        final canvas = tester.getRect(canvasFinder);
        final toolbarFinder = find.byKey(const ValueKey('craft-toolbar'));
        final toolbar = tester.getRect(toolbarFinder);
        final board = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        expect(inspector.left, greaterThanOrEqualTo(safePane.left));
        expect(inspector.top, greaterThanOrEqualTo(safePane.top));
        expect(inspector.right, lessThanOrEqualTo(safePane.right));
        expect(inspector.bottom, lessThanOrEqualTo(safePane.bottom));
        expect(inspector.overlaps(canvas), isFalse);
        for (final mask in <int>[1, 2, 4, 8, 15]) {
          final choice = find.byKey(ValueKey('craft-target-$mask'));
          final rect = tester.getRect(choice);
          expect(
            choice.hitTestable(),
            findsOneWidget,
            reason: 'target $mask at $rect inside inspector $inspector',
          );
          expect(rect.width, greaterThanOrEqualTo(47.9));
          expect(rect.height, greaterThanOrEqualTo(47.9));
          expect(rect.left, greaterThanOrEqualTo(safePane.left));
          expect(rect.top, greaterThanOrEqualTo(safePane.top));
          expect(rect.right, lessThanOrEqualTo(safePane.right));
          expect(rect.bottom, lessThanOrEqualTo(safePane.bottom));
          expect(rect.overlaps(canvas), isFalse);
        }
        final strings = AppLocalizations.of(
          tester.element(find.byType(LitchcraftScreen)),
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('craft-target-15'))),
          matchesSemantics(
            tooltip: strings.craftWholeSquare,
            isButton: true,
            isEnabled: true,
            hasEnabledState: true,
            isSelected: true,
            hasSelectedState: true,
            isFocusable: true,
            hasFocusAction: true,
            hasTapAction: true,
          ),
        );
        final firstTarget = find.byKey(const ValueKey('craft-target-1'));
        expect(firstTarget.hitTestable(), findsOneWidget);
        expect(
          tester.getSemantics(firstTarget),
          matchesSemantics(
            tooltip: strings.craftLeftLight,
            isButton: true,
            isEnabled: true,
            hasEnabledState: true,
            isSelected: false,
            hasSelectedState: true,
            isFocusable: true,
            hasFocusAction: true,
            hasTapAction: true,
          ),
        );
        await tester.tap(firstTarget);
        await tester.pumpAndSettle();
        expect(controller.targetMask, 1);
        expect(find.byKey(const ValueKey('craft-target-15')), findsOneWidget);
        expect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
          findsOneWidget,
        );
        expect(tester.getRect(canvasFinder), canvas);
        expect(tester.getRect(toolbarFinder), toolbar);
        expect(
          tester.getRect(find.byKey(const ValueKey('craft-ribbon-board'))),
          board,
        );

        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      }
      semantics.dispose();
    },
  );

  testWidgets(
    'frame rail edits, previews, and applies every selected-light effect locally',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await controller.setBrush(targetMask: 5);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();

      await _openAnimationTray(tester);
      expect(find.byKey(const ValueKey('craft-motion')), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
      await tester.pump();
      expect(library.animationDocument.frames, hasLength(2));
      expect(library.selectedFrameIndex, 1);
      expect(tester.takeException(), isNull, reason: 'two-frame controls');

      expect(find.byKey(const ValueKey('craft-frame-details')), findsNothing);
      await tester.tap(find.byKey(const ValueKey('craft-motion')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
      expect(tester.takeException(), isNull, reason: 'effects tray');
      final rainbow = find.byKey(const ValueKey('craft-effect-rainbow'));
      expect(rainbow.hitTestable(), findsOneWidget);
      await tester.tap(rainbow);
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        tester.getSemantics(rainbow),
        matchesSemantics(
          isButton: true,
          isEnabled: true,
          hasEnabledState: true,
          isSelected: true,
          hasSelectedState: true,
          isFocusable: true,
          hasFocusAction: true,
          hasTapAction: true,
        ),
      );
      expect(find.byKey(const ValueKey('craft-active-effect')), findsOneWidget);
      expect(
        tester
            .getRect(find.byKey(const ValueKey('craft-active-effect')))
            .overlaps(
              tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
            ),
        isFalse,
      );

      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      final rect = tester.getRect(canvas);
      await tester.tapAt(
        rect.topLeft + Offset(rect.width / 16, rect.height / 16),
      );
      await tester.pump();
      final emitters = library.selectedFrame.emitters;
      expect(emitters[0].effect, LitchcraftEffectStyle.rainbow);
      expect(emitters[1].effect, LitchcraftEffectStyle.off);
      expect(emitters[2].effect, LitchcraftEffectStyle.rainbow);
      expect(emitters[3].effect, LitchcraftEffectStyle.off);

      await _openAnimationTray(tester);
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump(const Duration(milliseconds: 150));
      expect(find.byIcon(Icons.pause), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump();
      expect(find.byIcon(Icons.play_arrow), findsOneWidget);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('direct frame controls reach frame 8 and preserve access', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    tester.view.physicalSize = const Size(360, 780);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await _openAnimationTray(tester);
    final play = find.byKey(const ValueKey('craft-preview'));
    final filmstrip = find.byKey(const ValueKey('craft-frame-filmstrip'));
    expect(tester.getSize(play).shortestSide, greaterThanOrEqualTo(48));
    expect(tester.getCenter(play).dy, tester.getCenter(filmstrip).dy);
    for (final removed in <String>[
      'craft-animation-effect',
      'craft-frame-previous',
      'craft-frame-next',
      'craft-delete-current-frame',
      'craft-frame-details',
    ]) {
      expect(find.byKey(ValueKey(removed)), findsNothing);
    }
    expect(find.byKey(const ValueKey('craft-inspector-scroll')), findsNothing);
    final turtle = find.text('Slower');
    final rabbit = find.text('Faster');
    expect(turtle, findsOneWidget);
    expect(rabbit, findsOneWidget);
    expect(tester.getCenter(turtle).dx, lessThan(tester.getCenter(rabbit).dx));
    expect(
      tester
          .widget<Slider>(find.byKey(const ValueKey('craft-frame-duration')))
          .divisions,
      99,
    );
    for (var frame = 2; frame <= 8; frame++) {
      await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
      await tester.pump();
      expect(library.selectedFrameIndex, frame - 1);
    }
    expect(
      tester
          .widget<IconButton>(find.byKey(const ValueKey('craft-add-frame')))
          .onPressed,
      isNotNull,
      reason: 'The normal editor can continue beyond the legacy eight frames.',
    );
    controller.restoreDraft(controller.canvas.paint(<int>[0], 1, 0x123456));
    library.selectFrame(6);
    await tester.pumpAndSettle();
    expect(
      library.selectedFrameIndex,
      6,
      reason: 'programmatic filmstrip reveal must preserve frame authority',
    );
    final lastFrame = find.byKey(const ValueKey('craft-select-frame-7'));
    final lastFrameNode = tester.getSemantics(lastFrame);
    expect(
      lastFrameNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    expect(
      lastFrameNode.getSemanticsData().hasAction(ui.SemanticsAction.longPress),
      isTrue,
    );
    lastFrameNode.owner!.performAction(
      lastFrameNode.id,
      ui.SemanticsAction.tap,
    );
    await tester.pump();
    expect(library.selectedFrameIndex, 7);
    expect(controller.canvas.colors.first, 0x123456);
    expect(find.bySemanticsLabel('Frame 8 of 8'), findsWidgets);
    await tester.ensureVisible(lastFrame);
    await tester.pumpAndSettle();
    await tester.longPress(lastFrame);
    await tester.pumpAndSettle();
    expect(find.text('Delete frame 8?'), findsOneWidget);
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    expect(library.animationDocument.frames, hasLength(8));
    expect(tester.takeException(), isNull);

    semantics.dispose();
    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('adding equal-color frames keeps the landscape canvas painted', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    controller.restoreDraft(
      LitchcraftCanvas.blank().paint(<int>[0], 15, 0xff3b30),
    );
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: RepaintBoundary(
            key: const ValueKey('craft-pixel-boundary'),
            child: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await _openAnimationTray(tester);
    Future<List<int>> samplePaintedSquare() async {
      final boundaryFinder = find.byKey(const ValueKey('craft-pixel-boundary'));
      final boundary = tester.renderObject<RenderRepaintBoundary>(
        boundaryFinder,
      );
      final canvasRect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final boundaryRect = tester.getRect(boundaryFinder);
      final point =
          canvasRect.topLeft -
          boundaryRect.topLeft +
          Offset(canvasRect.width / 16, canvasRect.height / 16);
      final image = (await tester.runAsync(
        () => boundary.toImage(pixelRatio: 1),
      ))!;
      final bytes = (await tester.runAsync(
        () => image.toByteData(format: ui.ImageByteFormat.rawRgba),
      ))!;
      final offset = ((point.dy.floor() * image.width) + point.dx.floor()) * 4;
      final pixel = bytes.buffer.asUint8List(offset, 4).toList();
      image.dispose();
      return pixel;
    }

    final before = await samplePaintedSquare();
    expect(before.take(3), isNot(<int>[255, 255, 255]));

    await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
    await tester.pumpAndSettle();
    expect(await samplePaintedSquare(), before);

    controller.documentSession.addFrame();
    await tester.pumpAndSettle();
    expect(await samplePaintedSquare(), before);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'reconnect stops preview, disables editing, and leaves help reachable',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _MutableStatusCraftController(
        ble,
        initialStatus: LitchcraftStatus.offline,
      );
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.addFrame();
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _openAnimationTray(tester);
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await tester.pump(const Duration(milliseconds: 150));
      expect(find.byIcon(Icons.pause), findsOneWidget);
      controller.setStatus(LitchcraftStatus.live);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byIcon(Icons.pause), findsNothing);
      expect(find.byKey(const ValueKey('craft-frame-panel')), findsOneWidget);

      controller.setStatus(LitchcraftStatus.offline);
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('craft-motion')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
      controller.setStatus(LitchcraftStatus.live);
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
      final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
      expect(
        tester
            .widget<InkWell>(
              find.descendant(of: breathe, matching: find.byType(InkWell)),
            )
            .onTap,
        isNull,
        reason: 'a legacy live board cannot accept schema-3 effects',
      );
      controller.setStatus(LitchcraftStatus.offline);
      await tester.pump();
      await _openAnimationTray(tester);
      await tester.longPress(
        find.byKey(const ValueKey('craft-select-frame-1')),
      );
      await tester.pumpAndSettle();
      expect(
        library.selectedFrameIndex,
        1,
        reason:
            'Add selected its copy; opening and stopping preview must retain that displayed frame',
      );
      expect(find.text('Delete frame 2?'), findsOneWidget);
      controller.setStatus(LitchcraftStatus.live);
      controller.repeatStatusNotification();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('Delete frame 2?'), findsNothing);
      expect(library.animationDocument.frames, hasLength(2));
      expect(
        tester
            .getSemantics(find.byKey(const ValueKey('craft-select-frame-1')))
            .getSemanticsData()
            .hasAction(ui.SemanticsAction.longPress),
        isFalse,
      );

      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(milliseconds: 200));
      expect(tester.takeException(), isNull);
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('disposing a playing editor cancels its preview timer', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await _openAnimationTray(tester);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump(const Duration(milliseconds: 120));
    expect(find.byIcon(Icons.pause), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump(const Duration(milliseconds: 250));
    expect(tester.takeException(), isNull);
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('changing tips preference fully stops animation preview', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    Widget app(bool showTips) => MaterialApp(
      builder: (context, child) => AppPreferencesScope(
        appearanceMode: LitchAppearanceMode.system,
        onAppearanceModeChanged: (_) async {},
        localePreference: const LitchLocalePreference.system(),
        onLocalePreferenceChanged: (_) async {},
        showTips: showTips,
        child: child!,
      ),
      home: Scaffold(
        body: LitchcraftScreen(controller: controller, library: library),
      ),
    );

    await tester.pumpWidget(app(true));
    await tester.pumpAndSettle();
    await _openAnimationTray(tester);
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump(const Duration(milliseconds: 120));
    expect(find.byIcon(Icons.pause), findsOneWidget);
    await tester.pumpWidget(app(false));
    await tester.pump();
    expect(find.byIcon(Icons.pause), findsNothing);
    expect(find.byKey(const ValueKey('craft-motion')), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('frame deletion confirms and reports the resulting selection', (
    tester,
  ) async {
    final haptics = _recordHoldHaptics(tester);
    final semantics = tester.ensureSemantics();
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library
      ..addFrame()
      ..addFrame();
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await _openAnimationTray(tester);
    expect(find.byKey(const ValueKey('craft-frame-details')), findsNothing);
    expect(
      find.byKey(const ValueKey('craft-delete-current-frame')),
      findsNothing,
    );
    // Stopped selection preserves browsing; reveal the target explicitly.
    await tester.ensureVisible(
      find.byKey(const ValueKey('craft-select-frame-2')),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey('craft-select-frame-2')).hitTestable(),
      findsOneWidget,
    );
    final pointer = await tester.startGesture(
      tester.getCenter(find.byKey(const ValueKey('craft-select-frame-2'))),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 399));
    expect(find.byType(AlertDialog), findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    await pointer.up();
    await tester.pumpAndSettle();
    expect(haptics, [
      'HapticFeedbackType.lightImpact',
      'HapticFeedbackType.mediumImpact',
    ]);
    expect(find.text('Delete frame 3?'), findsOneWidget);
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    expect(library.animationDocument.frames, hasLength(3));

    // Stopped selection preserves browsing; reveal the target explicitly.
    await tester.ensureVisible(
      find.byKey(const ValueKey('craft-select-frame-2')),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey('craft-select-frame-2')).hitTestable(),
      findsOneWidget,
    );
    haptics.clear();
    final node = tester.getSemantics(
      find.byKey(const ValueKey('craft-select-frame-2')),
    );
    node.owner!.performAction(node.id, ui.SemanticsAction.longPress);
    await tester.pumpAndSettle();
    expect(haptics, ['HapticFeedbackType.mediumImpact']);
    await tester.tap(find.byKey(const ValueKey('craft-confirm-delete-frame')));
    await tester.pumpAndSettle();
    expect(library.animationDocument.frames, hasLength(2));
    expect(library.selectedFrameIndex, 1);
    expect(find.bySemanticsLabel('Frame 2 of 2'), findsWidgets);
    expect(
      find.text('Deleted frame 3. Frame 2 of 2 is now selected.'),
      findsOneWidget,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
    semantics.dispose();
  });

  testWidgets('200 percent Effects inspector reaches every one-step effect', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(360, 780);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-motion')));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const ValueKey('craft-effect-panel')), findsOneWidget);
    final selector = find.byKey(const ValueKey('craft-effect-selector'));
    expect(selector.hitTestable(), findsOneWidget);
    await tester.tap(selector);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    for (final effect in LitchcraftEffectStyle.values) {
      final choice = find.byKey(ValueKey('craft-effect-${effect.name}'));
      expect(tester.getSize(choice).height, greaterThanOrEqualTo(48));
      expect(tester.getRect(choice).bottom, lessThanOrEqualTo(780));
    }
    final breathe = find.byKey(const ValueKey('craft-effect-breathe'));
    expect(breathe, findsOneWidget);
    expect(tester.widget<MenuItemButton>(breathe).onPressed, isNotNull);
    await tester.tap(breathe.hitTestable());
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(
      find.descendant(of: selector, matching: find.text('Breathe')),
      findsOneWidget,
    );
    expect(
      tester
          .widget<Slider>(find.byKey(const ValueKey('craft-effect-duration')))
          .onChanged,
      isNotNull,
    );
    expect(find.text('Faster'), findsOneWidget);
    expect(find.text('Slower'), findsOneWidget);
    final canvasFinder = find.byKey(const ValueKey('litchcraft-canvas'));
    expect(canvasFinder.hitTestable(), findsOneWidget);
    final canvas = tester.getRect(canvasFinder);
    await tester.tapAt(canvas.center);
    await tester.pump();
    expect(
      library.animationDocument.frames.single.emitters.where(
        (emitter) => emitter.effect == LitchcraftEffectStyle.breathe,
      ),
      isNotEmpty,
    );
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('status explains physical hold-to-pick progress and result', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _PhysicalPickCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );

    controller.showPhysicalPick(inProgress: true);
    await tester.pump();
    expect(
      find.text('Hold the piece still to pick this square’s color.'),
      findsNothing,
      reason: 'Ordinary Hall contact has a one-second qualification period',
    );
    await tester.pump(const Duration(seconds: 1));
    expect(
      find.text('Hold the piece still to pick this square’s color.'),
      findsOneWidget,
    );
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    await tester.pump(const Duration(seconds: 1));
    // Paused Flutter frames remain latched behind the OS. Check the first
    // resumed render rather than expecting an invisible background rebuild.
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump(const Duration(milliseconds: 500));
    expect(
      find.text('Hold the piece still to pick this square’s color.'),
      findsNothing,
    );
    await tester.pump(const Duration(milliseconds: 500));
    expect(
      find.text('Hold the piece still to pick this square’s color.'),
      findsOneWidget,
    );
    controller.showPhysicalPick(inProgress: false);
    await tester.pump();
    expect(
      find.text('Color picked. Move the piece to paint somewhere else.'),
      findsOneWidget,
    );
    controller.showPhysicalPick(inProgress: false, erasing: true);
    await tester.pump();
    expect(
      find.text('Erase selected. Move the piece to erase somewhere else.'),
      findsOneWidget,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'physical pick outranks app picker and remains a live region at 200 percent',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PhysicalPickCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) {
            final media = MediaQuery.of(context);
            final landscape = media.size.width > media.size.height;
            final insets = landscape
                ? const EdgeInsets.fromLTRB(20, 0, 12, 16)
                : EdgeInsets.zero;
            return MediaQuery(
              data: media.copyWith(
                textScaler: const TextScaler.linear(2),
                padding: insets,
                viewPadding: insets,
              ),
              child: child!,
            );
          },
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await _openToolTray(tester);
      await tester.longPress(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pumpAndSettle();
      controller.showPhysicalPick(inProgress: true);
      await tester.pump(const Duration(milliseconds: 1200));

      const progress =
          'Halte die Figur still, um die Farbe dieses Feldes aufzunehmen.';
      expect(find.text(progress), findsOneWidget);
      expect(
        find.text(
          'Tippe auf ein Feld, um seine Farbe zu übernehmen. Dein Bild bleibt unverändert.',
        ),
        findsNothing,
      );
      expect(find.bySemanticsLabel(progress), findsOneWidget);
      expect(
        tester.getSemantics(find.bySemanticsLabel(progress)),
        isSemantics(label: progress, isLiveRegion: true),
      );
      expect(tester.takeException(), isNull);

      controller.showPhysicalPick(inProgress: false);
      await tester.pump();
      expect(
        find.text('Farbe aufgenommen. Bewege die Figur, um woanders zu malen.'),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'share is full-size and an idle tip uses a transient nonblocking toast',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PendingCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox(
              width: 390,
              height: 760,
              child: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
      await tester.pumpAndSettle();
      final share = find.byKey(const ValueKey('craft-share'));
      expect(share, findsOneWidget);
      expect(tester.getSize(share).width, greaterThanOrEqualTo(48));
      expect(tester.getSize(share).height, greaterThanOrEqualTo(48));
      expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
      await tester.pump(const Duration(seconds: 10));
      final card = find.byKey(const ValueKey('craft-status-card'));
      expect(card, findsOneWidget);
      final material = tester.widget<Material>(
        find.descendant(of: card, matching: find.byType(Material)).first,
      );
      expect(material.color, isNot(Colors.transparent));
      expect(material.elevation, 0);
      final row = find.descendant(of: card, matching: find.byType(Row));
      expect(
        tester.widget<Row>(row).crossAxisAlignment,
        CrossAxisAlignment.center,
      );
      final toolbar = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      expect(
        toolbar.contains(
          tester.getCenter(find.byKey(const ValueKey('craft-leds'))),
        ),
        isTrue,
        reason: 'the LED selector belongs to the persistent tool surface',
      );
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      expect(canvas.overlaps(tester.getRect(card)), isTrue);
      final copy = tester.widget<Text>(
        find.descendant(of: card, matching: find.byType(Text)).first,
      );
      expect(copy.maxLines, isNull);
      expect(copy.overflow, TextOverflow.visible);
      expect(find.byKey(const ValueKey('craft-status-details')), findsNothing);
      expect(find.byType(BottomSheet), findsNothing);
      await tester.pump(const Duration(seconds: 5));
      expect(card, findsNothing);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'portrait connection error leaves the canvas geometry unchanged',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _MutableStatusCraftController(
        ble,
        initialStatus: LitchcraftStatus.live,
      );
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();

      final before = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      controller.setStatus(LitchcraftStatus.error);
      await tester.pump(const Duration(milliseconds: 200));
      final card = find.byKey(const ValueKey('craft-status-card'));
      final after = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      expect(card, findsNothing);
      expect(after, before);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'LED selector uses diagrams with accessible names and full tap targets',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      final panel = find.byKey(const ValueKey('craft-led-panel'));
      expect(
        find.descendant(
          of: panel,
          matching: find.text('Select LEDs to paint:'),
        ),
        findsOneWidget,
      );
      final strings = AppLocalizations.of(tester.element(panel));
      for (final entry in <int, String>{
        1: strings.craftLeftLight,
        2: strings.craftTopLight,
        4: strings.craftRightLight,
        8: strings.craftBottomLight,
        15: strings.craftWholeSquare,
      }.entries) {
        final control = find.byKey(ValueKey('craft-target-${entry.key}'));
        expect(control.hitTestable(), findsOneWidget);
        expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
        expect(tester.getSemantics(control).tooltip, entry.value);
        expect(
          find.descendant(of: control, matching: find.byType(Text)),
          findsNothing,
          reason: 'Target directions are conveyed by the diagram, not captions.',
        );
        expect(
          find.descendant(of: panel, matching: find.text(entry.value)),
          findsNothing,
        );
      }
      expect(
        find.descendant(of: panel, matching: find.byType(Scrollable)),
        findsNothing,
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('frame timing drag owns its preview and commits once on release', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await _openAnimationTray(tester);
    final slider = find.byKey(const ValueKey('craft-frame-duration'));
    expect(slider.hitTestable(), findsOneWidget);
    final initialDuration =
        library.animationDocument.frames[library.selectedFrameIndex].durationMs;
    final rect = tester.getRect(slider);
    final gesture = await tester.startGesture(
      Offset(rect.left + 24, rect.center.dy),
    );
    await tester.pump();
    final firstPreview = tester.widget<Slider>(slider).value;
    await gesture.moveTo(
      Offset(rect.center.dx, rect.center.dy),
      timeStamp: const Duration(milliseconds: 80),
    );
    await tester.pump();
    final middlePreview = tester.widget<Slider>(slider).value;
    expect(middlePreview, isNot(firstPreview));
    expect(
      library.animationDocument.frames[library.selectedFrameIndex].durationMs,
      initialDuration,
      reason: 'A held drag previews locally without changing the document.',
    );
    await gesture.moveTo(
      Offset(rect.right - 24, rect.center.dy),
      timeStamp: const Duration(milliseconds: 160),
    );
    await tester.pump();
    final finalPreview = tester.widget<Slider>(slider).value;
    expect(finalPreview, greaterThan(middlePreview));
    expect(
      library.animationDocument.frames[library.selectedFrameIndex].durationMs,
      initialDuration,
      reason:
          'Asynchronous rebuilds while held cannot steal the finger-owned preview.',
    );
    final heldToast = find.byKey(const ValueKey('craft-status-card'));
    expect(heldToast, findsOneWidget);
    expect(
      tester.getSemantics(heldToast).flagsCollection.isLiveRegion,
      isFalse,
    );
    await gesture.up();
    await tester.pump();
    final finalDuration =
        library.animationDocument.frames[library.selectedFrameIndex].durationMs;
    expect(
      finalDuration,
      lessThan(initialDuration),
      reason: 'Moving toward the rabbit makes the frame faster.',
    );
    final toast = find.byKey(const ValueKey('craft-status-card'));
    expect(toast, findsOneWidget);
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final inspector = tester.getRect(
      find.byKey(const ValueKey('craft-persistent-inspector')),
    );
    final toastRect = tester.getRect(toast);
    expect(canvas.overlaps(toastRect), isTrue);
    expect(inspector.overlaps(toastRect), isFalse);
    expect(toastRect.top, closeTo(canvas.top + 12, 1));
    expect(
      find.descendant(of: toast, matching: find.byType(BackdropFilter)),
      findsNothing,
    );
    expect(tester.getSemantics(toast).flagsCollection.isLiveRegion, isTrue);
    expect(
      find.text(
        'Frame duration: ${litchcraftDurationSeconds(finalDuration)} s',
      ),
      findsOneWidget,
    );
    await tester.pump(const Duration(milliseconds: 1401));
    expect(toast, findsNothing);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('Color and LEDs switch persistent inspector sections', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(390, 844);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final palette = find.byKey(const ValueKey('craft-palette'));
    expect(palette, findsOneWidget);
    expect(
      find.byKey(const ValueKey('craft-inspector-color-field')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('craft-target-15')), findsNothing);
    final color = find.byKey(const ValueKey('craft-inspector-hex'));
    expect(color, findsOneWidget);
    await tester.tap(color);
    await tester.pumpAndSettle();
    for (final digit in '37D769'.split('')) {
      await tester.tap(find.byKey(ValueKey('craft-hex-key-$digit')));
    }
    await tester.tap(find.byKey(const ValueKey('craft-hex-apply')));
    await tester.pumpAndSettle();
    expect(controller.color, 0x37d769);
    expect(
      find.byKey(const ValueKey('craft-persistent-inspector')),
      findsOneWidget,
    );

    await tester.tap(find.byKey(const ValueKey('craft-leds')));
    await tester.pumpAndSettle();
    final rightLight = find.byKey(const ValueKey('craft-target-4'));
    expect(rightLight, findsOneWidget);
    if (rightLight.hitTestable().evaluate().isEmpty) {
      await _revealInspectorControl(tester, rightLight);
    }
    await tester.tap(rightLight.hitTestable());
    await tester.pump();
    expect(controller.targetMask, 4);

    final motion = find.byKey(const ValueKey('craft-motion'));
    expect(motion.hitTestable(), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-animate')), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('color commit and repeated swap keep exactly two colors', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );

    final previous = find.byKey(const ValueKey('craft-quick-previous-color'));
    expect(previous, findsOneWidget);
    expect(tester.getSize(previous).shortestSide, greaterThanOrEqualTo(48));
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey('craft-quick-previous-color-semantics')),
          )
          .flagsCollection
          .isEnabled,
      ui.Tristate.isTrue,
      reason: 'the same control always retains its hold-to-pick action',
    );

    await controller.setBrush(color: 0x123456);
    await tester.pump();
    await tester.tap(previous);
    await tester.pump();
    expect(controller.color, 0xff7430);
    expect(controller.previousColor, 0x123456);

    await tester.tap(previous);
    await tester.pump();
    expect(controller.color, 0x123456);
    expect(controller.previousColor, 0xff7430);

    await controller.setBrush(color: 0xabcdef);
    await tester.pump();
    expect(controller.color, 0xabcdef);
    expect(controller.previousColor, 0x123456);
    await controller.setBrush(color: 0xabcdef);
    await tester.pump();
    expect(controller.previousColor, 0x123456);

    await controller.setBrush(eraser: true);
    await tester.pump();
    await tester.tap(previous);
    await tester.pump();
    expect(controller.eraser, isTrue);
    expect(
      find.byKey(const ValueKey('craft-persistent-inspector')),
      findsOneWidget,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  for (final platform in <TargetPlatform>[
    TargetPlatform.android,
    TargetPlatform.iOS,
  ]) {
    testWidgets(
      '${platform.name} phone rotation keeps a centered canvas and reachable ribbon',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        controller.restoreDraft(
          LitchcraftCanvas.blank().paint(<int>[0], 15, 0x22c3e2),
        );
        await tester.pumpWidget(
          MaterialApp(
            theme: ThemeData(platform: platform),
            home: Scaffold(
              appBar: AppBar(title: const Text('LITCHCRAFT')),
              body: LitchcraftScreen(controller: controller, library: library),
              bottomNavigationBar: NavigationBar(
                selectedIndex: 0,
                destinations: <NavigationDestination>[
                  NavigationDestination(
                    icon: Icon(Icons.brush_outlined),
                    label: 'Litchcraft',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.sports_esports_outlined),
                    label: 'Play',
                  ),
                  NavigationDestination(
                    icon: Icon(Icons.settings_outlined),
                    label: 'Setup',
                  ),
                ],
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('craft-portrait-shell')),
          findsOneWidget,
        );
        final beforeStroke = controller.canvas;
        final portraitCanvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final stroke = await tester.startGesture(
          portraitCanvas.topLeft +
              Offset(portraitCanvas.width / 16, portraitCanvas.height / 16),
        );
        await stroke.moveTo(
          portraitCanvas.topLeft +
              Offset(portraitCanvas.width * 3 / 16, portraitCanvas.height / 16),
        );
        await tester.pump();
        expect(controller.canvas, isNot(beforeStroke));

        tester.view.physicalSize = const Size(844, 390);
        await tester.pumpAndSettle();
        final afterRotation = controller.canvas;
        await stroke.moveTo(const Offset(8, 8));
        await tester.pump();
        expect(
          controller.canvas,
          afterRotation,
          reason: 'rotation cancels the canvas-owned pointer before reflow',
        );
        await stroke.up();
        await tester.pump();
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final shell = tester.getRect(
          find.byKey(const ValueKey('craft-landscape-shell')),
        );
        final board = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        expect(
          canvas.center.dy,
          moreOrLessEquals(board.center.dy, epsilon: 1),
          reason: 'the board stays vertically centered in the safe stage',
        );
        expect(
          inspector.center.dy,
          moreOrLessEquals(canvas.center.dy, epsilon: 1),
        );
        expect(inspector.right, lessThanOrEqualTo(shell.right));
        expect(_toolbarAxis(tester), Axis.vertical);
        final undo = tester.getRect(find.byKey(const ValueKey('craft-undo')));
        final redo = tester.getRect(find.byKey(const ValueKey('craft-redo')));
        final history = tester.getRect(
          find.byKey(const ValueKey('craft-history-footer')),
        );
        expect(redo.center.dy, moreOrLessEquals(undo.center.dy, epsilon: 1));
        expect(history.contains(undo.center), isTrue);
        expect(history.contains(redo.center), isTrue);
        expect(
          find.byKey(const ValueKey('craft-animate')),
          findsOneWidget,
          reason: 'one-frame work keeps Animate available contextually',
        );
        final navigation = tester.getRect(find.byType(NavigationBar));
        expect(canvas.width, greaterThanOrEqualTo(220));
        expect(canvas.height, moreOrLessEquals(canvas.width, epsilon: 0.5));
        expect(canvas.right, lessThan(inspector.left));
        expect(navigation.top - canvas.bottom, greaterThanOrEqualTo(6));
        final focusTarget = find.byKey(const ValueKey('craft-palette'));
        var reachedFocusTarget = false;
        for (var step = 0; step < 32; step++) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pumpAndSettle();
          if (_primaryFocusIsWithin(focusTarget)) {
            reachedFocusTarget = true;
            expect(
              tester.getRect(focusTarget).bottom,
              lessThanOrEqualTo(inspector.bottom),
            );
            break;
          }
        }
        expect(reachedFocusTarget, isTrue);
        await controller.undo();
        expect(
          controller.canvas,
          beforeStroke,
          reason: 'rotation finishes one coherent stroke and Undo restores it',
        );
        expect(
          controller.canUndo,
          isFalse,
          reason: 'the held rotation gesture creates exactly one undo frame',
        );
        expect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
          findsOneWidget,
        );
        expect(find.byKey(const ValueKey('craft-leds')), findsOneWidget);
        expect(
          find.byKey(const ValueKey('craft-status-card')),
          findsNothing,
          reason: 'ordinary offline state uses the shell connection status',
        );
        expect(tester.takeException(), isNull);

        tester.view.physicalSize = const Size(667, 375);
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('craft-landscape-shell')),
          findsOneWidget,
        );
        expect(
          tester.getSize(find.byKey(const ValueKey('litchcraft-canvas'))).width,
          greaterThan(160),
        );

        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  testWidgets(
    'Android 384 by 832 viewport keeps the whole editor visible without page scrolling',
    (tester) async {
      tester.view.physicalSize = const Size(384, 832);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PendingCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            appBar: AppBar(title: const Text('LITCHCRAFT')),
            body: LitchcraftScreen(controller: controller, library: library),
            bottomNavigationBar: NavigationBar(
              selectedIndex: 0,
              destinations: const <NavigationDestination>[
                NavigationDestination(
                  icon: Icon(Icons.brush_outlined),
                  label: 'Litchcraft',
                ),
                NavigationDestination(
                  icon: Icon(Icons.sports_esports_outlined),
                  label: 'Play',
                ),
                NavigationDestination(
                  icon: Icon(Icons.settings_outlined),
                  label: 'Setup',
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-portrait-shell')),
        findsOneWidget,
      );
      final palette = find.byKey(const ValueKey('craft-palette'));
      final leds = find.byKey(const ValueKey('craft-leds'));
      final effects = find.byKey(const ValueKey('craft-motion'));
      final animation = find.byKey(const ValueKey('craft-animate'));
      final undo = find.byKey(const ValueKey('craft-undo'));
      final redo = find.byKey(const ValueKey('craft-redo'));
      for (final action in <Finder>[
        palette,
        leds,
        effects,
        animation,
        undo,
        redo,
      ]) {
        expect(tester.getSize(action).shortestSide, greaterThanOrEqualTo(48));
      }
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
      await tester.pumpAndSettle();
      final status = tester.getRect(
        find.byKey(const ValueKey('craft-status-card')),
      );
      final navigation = tester.getRect(find.byType(NavigationBar));
      expect(canvas.height, greaterThanOrEqualTo(220));
      expect(
        canvas.overlaps(status),
        isTrue,
        reason: 'canvas=$canvas status=$status',
      );
      expect(status.bottom, lessThanOrEqualTo(navigation.top - 8));
      expect(find.byKey(const ValueKey('craft-help')), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'short portrait at 200 percent keeps compact tools and canvas reachable',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = const Size(320, 640);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          builder: (context, child) {
            final media = MediaQuery.of(context);
            final landscape = media.size.width > media.size.height;
            final insets = landscape
                ? const EdgeInsets.fromLTRB(20, 0, 12, 16)
                : EdgeInsets.zero;
            return MediaQuery(
              data: media.copyWith(
                textScaler: const TextScaler.linear(2),
                padding: insets,
                viewPadding: insets,
              ),
              child: child!,
            );
          },
          home: Scaffold(
            appBar: AppBar(title: const Text('LITCHCRAFT')),
            body: LitchcraftScreen(controller: controller, library: library),
            bottomNavigationBar: NavigationBar(
              selectedIndex: 0,
              destinations: const <NavigationDestination>[
                NavigationDestination(
                  icon: Icon(Icons.brush_outlined),
                  label: 'Litchcraft',
                ),
                NavigationDestination(
                  icon: Icon(Icons.sports_esports_outlined),
                  label: 'Play',
                ),
                NavigationDestination(
                  icon: Icon(Icons.settings_outlined),
                  label: 'Setup',
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-portrait-shell')),
        findsOneWidget,
      );
      final compactToolSize = tester.getSize(
        find.byKey(const ValueKey('craft-inspector-tab-palette')),
      );
      expect(compactToolSize.width, greaterThanOrEqualTo(48));
      expect(compactToolSize.height, greaterThanOrEqualTo(48));
      final canvasRect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      const viewport = Rect.fromLTWH(0, 0, 320, 640);
      expect(viewport.intersect(canvasRect), canvasRect);
      final lastSquare = find.byKey(const ValueKey('craft-square-63'));
      await tester.tapAt(tester.getCenter(lastSquare));
      await tester.pump();
      expect(
        controller.canvas.colors.skip(63 * 4).take(4),
        everyElement(isNot(0)),
        reason: 'the bottom-right square remains paintable at 200% text',
      );
      expect(
        tester.getSize(find.byKey(const ValueKey('litchcraft-canvas'))).width,
        tester.getSize(find.byKey(const ValueKey('litchcraft-canvas'))).height,
      );
      expect(tester.takeException(), isNull);
      await captureHealthReview(tester, 'compact-portrait-320x640-200');

      tester.view.physicalSize = const Size(390, 300);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-portrait-shell')),
        findsOneWidget,
        reason: 'a narrow portrait accessibility viewport is not two-pane',
      );
      expect(_toolbarAxis(tester), Axis.horizontal);
      expect(tester.takeException(), isNull);

      tester.view.physicalSize = const Size(667, 375);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('craft-landscape-shell')),
        findsOneWidget,
      );
      final landscapeCanvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final landscapeNavigation = tester.getRect(find.byType(NavigationBar));
      expect(landscapeCanvas.width, greaterThan(150));
      expect(
        landscapeNavigation.top - landscapeCanvas.bottom,
        greaterThanOrEqualTo(8),
      );
      final color = find.byKey(const ValueKey('craft-wheel-color-1'));
      await tester.tap(color);
      await tester.pumpAndSettle();
      expect(controller.color, litchcraftPaletteRailColors[1]);
      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pump(const Duration(milliseconds: 300));
      final target = find.byKey(const ValueKey('craft-target-4'));
      await _revealInspectorControl(tester, target);
      expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        landscapeCanvas,
        reason: 'changing inspector tools must not move or resize the artwork',
      );
      expect(tester.getSize(target).shortestSide, greaterThanOrEqualTo(48));
      await tester.tap(target.hitTestable());
      await tester.pump();
      expect(controller.targetMask, 4);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets(
    'tips wait for inactivity, dismiss on touch, and tools explain themselves',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PendingCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
      await tester.pump(const Duration(seconds: 9));
      expect(find.text('Paint some happy little squares!'), findsNothing);
      await tester.pump(const Duration(seconds: 1));
      await tester.pump(const Duration(milliseconds: 200));
      expect(find.text('Paint some happy little squares!'), findsOneWidget);
      expect(
        find.bySemanticsLabel('Paint some happy little squares!'),
        findsOneWidget,
      );
      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      await tester.tapAt(tester.getCenter(canvas));
      await tester.pump();
      await tester.pump();
      expect(find.text('Paint some happy little squares!'), findsNothing);
      await tester.pump(const Duration(seconds: 9));
      expect(find.text('Paint some happy little squares!'), findsNothing);
      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pump(const Duration(milliseconds: 300));
      final leftTarget = find.byKey(const ValueKey('craft-target-1'));
      await _revealInspectorControl(tester, leftTarget);
      await tester.tap(leftTarget.hitTestable());
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 200));
      expect(find.textContaining('Left outward.'), findsOneWidget);
      expect(find.textContaining('Paint on the canvas'), findsOneWidget);
      expect(
        tester
            .widget<Semantics>(find.byKey(const ValueKey('craft-status-card')))
            .properties
            .liveRegion,
        isTrue,
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final boardPlayback in [false, true]) {
    testWidgets(
      'periodic tips stay hidden during ${boardPlayback ? "board" : "phone"} playback',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = boardPlayback
            ? (_BoardTimelineCraftController(ble)..playing = false)
            : LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        controller.restoreDocument(
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: [
              for (final color in [0xff0000, 0x0000ff])
                LitchcraftAnimationFrame(
                  emitters: List.generate(
                    256,
                    (_) => LitchcraftAnimatedEmitter(color: color),
                  ),
                ),
            ],
          ),
        );
        library.dismissedWelcome = true;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await _openAnimationTray(tester);
        await tester.pump(const Duration(seconds: 10));
        await tester.pump(const Duration(milliseconds: 200));
        expect(find.text('Paint some happy little squares!'), findsOneWidget);
        await tester.tap(find.byKey(const ValueKey('craft-preview')));
        await tester.pump();
        expect(find.text('Paint some happy little squares!'), findsNothing);
        for (var index = 0; index < 3; index++) {
          await tester.pump(const Duration(seconds: 10));
          expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        }
        await tester.tap(find.byKey(const ValueKey('craft-preview')));
        await tester.pump();
        await tester.pump(const Duration(seconds: 10));
        await tester.pump(const Duration(milliseconds: 200));
        expect(find.text('Paint some happy little squares!'), findsOneWidget);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  testWidgets('disabled tips stay hidden but Paint and Erase still announce', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => AppPreferencesScope(
          appearanceMode: LitchAppearanceMode.system,
          onAppearanceModeChanged: (_) async {},
          localePreference: const LitchLocalePreference.system(),
          onLocalePreferenceChanged: (_) async {},
          showTips: false,
          onShowTipsChanged: (_) async {},
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(seconds: 60));
    expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);

    await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
    await tester.pumpAndSettle();
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.text('Erase mode'), findsOneWidget);
    expect(
      tester
          .widget<Semantics>(find.byKey(const ValueKey('craft-status-card')))
          .properties
          .liveRegion,
      isTrue,
    );
    await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
    await tester.pumpAndSettle();
    expect(find.text('Paint mode'), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  for (final size in [const Size(390, 844), const Size(1000, 472.5)]) {
    testWidgets(
      'artwork delivery stays silent and keeps editing available at $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final semantics = tester.ensureSemantics();
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = _ArtworkDeliveryController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        expect(find.textContaining('Sending artwork'), findsNothing);
        expect(find.bySemanticsLabel(RegExp('Sending artwork')), findsNothing);
        expect(
          find.byKey(const ValueKey('craft-full-animation-sync')),
          findsNothing,
        );
        final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
        final rect = tester.getRect(canvas);
        final stroke = await tester.startGesture(
          rect.topLeft + Offset(rect.width / 16, rect.height / 16),
        );
        for (final progress in [.25, .50, .995]) {
          controller.showDelivery(progress: progress);
          await stroke.moveBy(Offset(rect.width / 8, 0));
          await tester.pump(const Duration(milliseconds: 200));
          expect(find.textContaining('Sending artwork'), findsNothing);
          expect(
            find.bySemanticsLabel(RegExp('Sending artwork')),
            findsNothing,
          );
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isTrue,
          );
          expect(tester.getRect(canvas), rect);
          expect(controller.hasMarks, isTrue);
        }
        await stroke.up();
        controller.showDelivery(progress: 1, finishing: true);
        await tester.pump(const Duration(milliseconds: 200));
        expect(find.text('Finishing transfer…'), findsNothing);
        expect(find.textContaining('100%'), findsNothing);
        expect(find.text('Saved on this device'), findsNothing);
        await tester.tap(find.byKey(const ValueKey('craft-motion')));
        await tester.pump(const Duration(milliseconds: 300));
        expect(
          find.byKey(const ValueKey('craft-effect-panel')),
          findsOneWidget,
        );
        expect(find.text('Finishing transfer…'), findsNothing);

        controller.showDelivery(pending: false);
        await tester.pump(const Duration(milliseconds: 200));
        expect(find.text('Finishing transfer…'), findsNothing);
        expect(find.textContaining('Sending artwork'), findsNothing);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
        semantics.dispose();
      },
    );
  }

  testWidgets('a held canvas stroke never becomes idle', (tester) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _PendingCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump();
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    final stroke = await tester.startGesture(tester.getCenter(canvas));
    await tester.pump(const Duration(seconds: 12));
    expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);

    await stroke.up();
    await tester.pump(const Duration(seconds: 9));
    expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.text('Paint some happy little squares!'), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('semantic canvas use restarts the inactivity interval', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _PendingCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(seconds: 9));
    final square = find.byKey(const ValueKey('craft-square-7'));
    final node = tester.getSemantics(square);
    node.owner!.performAction(node.id, ui.SemanticsAction.tap);
    await tester.pump();
    expect(controller.hasMarks, isTrue);

    await tester.pump(const Duration(seconds: 9));
    expect(find.text('Paint some happy little squares!'), findsNothing);
    final undo = find.byKey(const ValueKey('craft-undo'));
    final undoNode = tester.getSemantics(undo);
    undoNode.owner!.performAction(undoNode.id, ui.SemanticsAction.tap);
    await tester.pump();
    expect(controller.hasMarks, isFalse);

    await tester.pump(const Duration(seconds: 9));
    expect(find.text('Paint some happy little squares!'), findsNothing);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.text('Paint some happy little squares!'), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    semantics.dispose();
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('all 40 localized tips participate in the idle cycle', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _PendingCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump(const Duration(seconds: 10));
    for (var tip = 2; tip <= 40; tip++) {
      await tester.pump(const Duration(seconds: 4));
      await tester.pump(const Duration(seconds: 10));
    }
    await tester.pump(const Duration(milliseconds: 200));
    expect(
      find.text('Pause, then change the one square you notice.'),
      findsOneWidget,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  for (final initialStatus in <LitchcraftStatus>[
    LitchcraftStatus.live,
    LitchcraftStatus.offline,
  ]) {
    testWidgets(
      'a ${initialStatus.name} status transition cannot move a held stroke',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = _MutableStatusCraftController(
          ble,
          initialStatus: initialStatus,
        );
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        final canvasFinder = find.byKey(const ValueKey('litchcraft-canvas'));
        final before = tester.getRect(canvasFinder);
        final stroke = await tester.startGesture(before.center);
        await stroke.moveBy(const Offset(24, 0));
        controller.setStatus(LitchcraftStatus.error);
        await tester.pump();
        expect(tester.getRect(canvasFinder), before);
        await stroke.moveBy(const Offset(24, 0));
        await tester.pump();
        expect(tester.getRect(canvasFinder), before);
        await stroke.up();
        await tester.pump();

        final after = tester.getRect(canvasFinder);
        expect(after, before);
        expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        final bottomSquare = find.byKey(const ValueKey('craft-square-63'));
        await tester.ensureVisible(bottomSquare);
        await tester.pumpAndSettle();
        await tester.tapAt(tester.getCenter(bottomSquare));
        await tester.pump();
        expect(
          controller.canvas.colors.skip(63 * 4).take(4),
          everyElement(isNot(0)),
          reason: 'recovery must leave the bottom edge paintable',
        );
        expect(controller.canUndo, isTrue);

        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  testWidgets(
    'landscape picker guidance leaves artwork and controls anchored',
    (tester) async {
      tester.view.physicalSize = const Size(844, 390);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PhysicalPickCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      final canvasBefore = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final toolsBefore = tester.getRect(
        find.byKey(const ValueKey('craft-toolbar')),
      );
      final inspectorBefore = tester.getRect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
      );
      controller.showPhysicalPick(inProgress: true);
      await tester.pump(const Duration(seconds: 1));
      final status = tester.getRect(
        find.byKey(const ValueKey('craft-status-card')),
      );
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      expect(canvas.overlaps(status), isTrue);
      expect(status.top, closeTo(canvas.top + 12, 1));
      expect(
        status.overlaps(
          tester.getRect(
            find.byKey(const ValueKey('craft-persistent-inspector')),
          ),
        ),
        isFalse,
        reason: 'guidance must leave creative controls unobscured',
      );
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        canvasBefore,
      );
      expect(
        tester.getRect(find.byKey(const ValueKey('craft-toolbar'))),
        toolsBefore,
      );
      expect(
        tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        ),
        inspectorBefore,
      );
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('landscape idle tips use a bounded toast at 200 percent text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _PendingCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(seconds: 10));
    await tester.pump(const Duration(milliseconds: 200));

    final card = find.byKey(const ValueKey('craft-status-card'));
    final inspector = find.byKey(const ValueKey('craft-toolbar'));
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    expect(card, findsOneWidget);
    expect(tester.getRect(inspector).contains(tester.getCenter(card)), isFalse);
    expect(
      tester.getRect(canvas).overlaps(tester.getRect(card)),
      isTrue,
      reason: 'optional guidance belongs to the artwork, not the tool surface',
    );
    final copy = tester.widget<Text>(
      find.descendant(of: card, matching: find.byType(Text)).first,
    );
    expect(copy.maxLines, isNull);
    expect(copy.overflow, TextOverflow.visible);
    expect(find.byKey(const ValueKey('craft-status-details')), findsNothing);
    expect(find.byType(BottomSheet), findsNothing);
    await tester.pump(const Duration(seconds: 5));
    expect(card, findsNothing);
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('picker guidance floats over controls and leaves canvas clear', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    tester.view.physicalSize = const Size(320, 640);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    const safeInsets = EdgeInsets.fromLTRB(0, 24, 0, 34);
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context).copyWith(
            padding: safeInsets,
            viewPadding: safeInsets,
            textScaler: const TextScaler.linear(2),
          ),
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    for (final key in <String>[
      'craft-quick-brush',
      'craft-quick-previous-color',
      'craft-quick-fill',
    ]) {
      final action = find.byKey(ValueKey(key));
      expect(action, findsOneWidget);
      await _revealInspectorControl(tester, action);
      final rect = tester.getRect(action);
      expect(rect.left, greaterThanOrEqualTo(0), reason: key);
      expect(rect.right, lessThanOrEqualTo(320), reason: key);
      expect(rect.top, greaterThanOrEqualTo(safeInsets.top), reason: key);
      expect(
        rect.bottom,
        lessThanOrEqualTo(640 - safeInsets.bottom),
        reason: key,
      );
      expect(rect.shortestSide, greaterThanOrEqualTo(47.999), reason: key);
      expect(action.hitTestable(), findsOneWidget, reason: key);
    }
    await tester.tap(find.byKey(const ValueKey('craft-leds')));
    await tester.pumpAndSettle();
    for (final key in <String>[
      'craft-target-1',
      'craft-target-2',
      'craft-target-4',
      'craft-target-8',
    ]) {
      final action = find.byKey(ValueKey(key));
      await _revealInspectorControl(tester, action);
      final rect = tester.getRect(action);
      expect(rect.left, greaterThanOrEqualTo(0), reason: key);
      expect(rect.right, lessThanOrEqualTo(320), reason: key);
      expect(rect.top, greaterThanOrEqualTo(safeInsets.top), reason: key);
      expect(
        rect.bottom,
        lessThanOrEqualTo(640 - safeInsets.bottom),
        reason: key,
      );
      expect(rect.shortestSide, greaterThanOrEqualTo(47.999), reason: key);
    }
    final allTargets = find.byKey(const ValueKey('craft-target-15'));
    expect(allTargets, findsOneWidget);
    await _revealInspectorControl(tester, allTargets);
    final allTargetsRect = tester.getRect(allTargets);
    expect(allTargetsRect.left, greaterThanOrEqualTo(0));
    expect(allTargetsRect.right, lessThanOrEqualTo(320));
    expect(allTargetsRect.top, greaterThanOrEqualTo(safeInsets.top));
    expect(allTargetsRect.bottom, lessThanOrEqualTo(640 - safeInsets.bottom));
    expect(allTargetsRect.shortestSide, greaterThanOrEqualTo(47.999));
    expect(allTargets.hitTestable(), findsOneWidget);
    final canvasBeforePicker = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final pickColor = find.byKey(const ValueKey('craft-quick-previous-color'));
    await tester.longPress(pickColor.hitTestable());
    await tester.pump(const Duration(milliseconds: 200));
    final canvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final status = tester.getRect(
      find.byKey(const ValueKey('craft-status-card')),
    );
    expect(
      canvas,
      canvasBeforePicker,
      reason: 'persistent picker guidance must not move or resize artwork',
    );
    expect(canvas.overlaps(status), isTrue);
    expect(status.top, closeTo(canvas.top + 12, 1));
    expect(
      status.overlaps(
        tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        ),
      ),
      isFalse,
    );
    final copy = tester.widget<Text>(
      find.text(
        'Tap a square to pick its color. Your artwork stays unchanged.',
      ),
    );
    expect(copy.maxLines, isNull);
    expect(copy.overflow, TextOverflow.visible);
    expect(find.byKey(const ValueKey('craft-status-details')), findsNothing);
    expect(find.byType(BottomSheet), findsNothing);
    await tester.pump(const Duration(seconds: 4));
    expect(
      find.byKey(const ValueKey('craft-status-card')),
      findsOneWidget,
      reason: 'picker guidance remains available until color picking ends',
    );

    final visibleCanvas = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    await tester.tapAt(
      visibleCanvas.bottomRight -
          Offset(visibleCanvas.width / 16, visibleCanvas.height / 16),
    );
    await tester.pump();
    expect(controller.eraser, isTrue);
    expect(controller.hasMarks, isFalse);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    '750 ms Paint Erase toggle holds apply once, cancel outside and undo in one step',
    (tester) async {
      tester.view.physicalSize = const Size(667, 375);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final brush = find.byKey(const ValueKey('craft-quick-brush'));
      await tester.tap(brush);
      await tester.pumpAndSettle();
      expect(controller.eraser, isTrue);
      await tester.tap(brush);
      await tester.pumpAndSettle();
      expect(controller.eraser, isFalse);

      final fill = find.byKey(const ValueKey('craft-quick-fill'));
      await tester.tap(fill);
      await tester.pumpAndSettle();
      expect(
        controller.canvas.colors.every((c) => c == controller.color),
        isTrue,
      );
      await controller.undo();
      expect(controller.hasMarks, isFalse);
      await tester.longPress(fill);
      await tester.pumpAndSettle();
      expect(controller.hasMarks, isFalse);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('palette tray selects a swatch and custom picker fills canvas', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => AppPreferencesScope(
          appearanceMode: LitchAppearanceMode.system,
          onAppearanceModeChanged: (_) async {},
          localePreference: const LitchLocalePreference.system(),
          onLocalePreferenceChanged: (_) async {},
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final fillColor = litchcraftPaletteRailColors[4];
    final swatch = find.byKey(const ValueKey('craft-wheel-color-4'));
    expect(fillColor, 0xff9a00);
    final swatchIcon = tester.widget<IconButton>(
      find.descendant(of: swatch, matching: find.byType(IconButton)),
    );
    final swatchDecoration =
        (swatchIcon.icon as Container).decoration! as BoxDecoration;
    expect(swatchDecoration.color!.toARGB32(), 0xffffcc00);
    await tester.tap(swatch);
    await tester.pumpAndSettle();
    expect(controller.color, fillColor);
    expect(controller.hasMarks, isFalse);
    final field = find.byKey(const ValueKey('craft-inspector-color-field'));
    final fieldPaint = tester.widget<CustomPaint>(
      find.descendant(of: field, matching: find.byType(CustomPaint)),
    );
    final fieldPixel = (await tester.runAsync(() async {
      final recorder = ui.PictureRecorder();
      fieldPaint.painter!.paint(ui.Canvas(recorder), const Size(128, 128));
      final picture = recorder.endRecording();
      final image = await picture.toImage(128, 128);
      try {
        final bytes = (await image.toByteData(format: ui.ImageByteFormat.rawRgba))!;
        const offset = (124 * 128 + 126) * 4;
        return bytes.buffer.asUint8List(bytes.offsetInBytes + offset, 4).toList();
      } finally {
        image.dispose();
        picture.dispose();
      }
    }))!;
    // The picker position is near RGB #070400 before the display transfer.
    expect(fieldPixel[0], closeTo(46, 4));
    expect(fieldPixel[1], closeTo(34, 4));
    expect(fieldPixel[3], 255);
    await tester.tap(find.byKey(const ValueKey('craft-quick-fill')));
    await tester.pumpAndSettle();
    expect(
      controller.canvas.colors.every((color) => color == fillColor),
      isTrue,
    );
    await controller.undo();
    expect(controller.hasMarks, isFalse);
    expect(controller.canUndo, isFalse);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('palette remains open through canvas paint and erase strokes', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final boardBefore = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final swatch = find.byKey(const ValueKey('craft-wheel-color-3'));
    await _revealInspectorControl(tester, swatch);
    await tester.tap(swatch.hitTestable());
    await tester.pump();
    expect(
      find.byKey(const ValueKey('craft-inspector-color-field')),
      findsOneWidget,
    );
    expect(
      tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
      boardBefore,
    );

    await tester.tapAt(
      tester.getCenter(find.byKey(const ValueKey('craft-square-0'))),
    );
    await tester.pump(const Duration(milliseconds: 120));
    expect(controller.canvas.colors.take(4), isNot(everyElement(0)));
    expect(
      find.byKey(const ValueKey('craft-inspector-color-field')),
      findsOneWidget,
    );
    expect(
      tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
      boardBefore,
    );

    final erase = find.byKey(const ValueKey('craft-quick-brush'));
    await tester.tap(erase.hitTestable());
    await tester.pump(const Duration(milliseconds: 200));
    await tester.tapAt(
      tester.getCenter(find.byKey(const ValueKey('craft-square-0'))),
    );
    await tester.pump(const Duration(milliseconds: 120));
    expect(controller.canvas.colors.take(4), everyElement(0));
    expect(
      find.byKey(const ValueKey('craft-inspector-color-field')),
      findsOneWidget,
    );

    await tester.tap(find.byKey(const ValueKey('craft-animate')));
    await tester.pump(const Duration(milliseconds: 300));
    expect(
      find.byKey(const ValueKey('craft-inspector-color-field')).hitTestable(),
      findsNothing,
    );
    expect(find.byKey(const ValueKey('craft-frame-filmstrip')), findsOneWidget);
    expect(
      tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
      boardBefore,
    );

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('animation filmstrip has no nested frame-details detour', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    await _openAnimationTray(tester);
    expect(find.byKey(const ValueKey('craft-frame-details')), findsNothing);
    expect(find.byKey(const ValueKey('craft-frame-filmstrip')), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-motion-editor')), findsNothing);
    expect(find.byKey(const ValueKey('litchcraft-canvas')), findsOneWidget);

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'constrained landscape keeps palette endpoints and canvas hittable',
    (tester) async {
      tester.view.physicalSize = const Size(667, 375);
      const safeInsets = EdgeInsets.fromLTRB(44, 0, 44, 21);
      for (final side in <Alignment>[
        Alignment.centerLeft,
        Alignment.centerRight,
      ]) {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                padding: safeInsets,
                viewPadding: safeInsets,
                textScaler: const TextScaler.linear(2),
              ),
              child: child!,
            ),
            home: Scaffold(
              body: SafeArea(
                child: LitchcraftScreen(
                  controller: controller,
                  library: library,
                  landscapeRibbonSide: side,
                  rootSafeInsets: safeInsets,
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
        final before = tester.getRect(canvas);

        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        final safe = Rect.fromLTRB(
          safeInsets.left,
          safeInsets.top,
          667 - safeInsets.right,
          375 - safeInsets.bottom,
        );
        if (side == Alignment.centerLeft) {
          expect(inspector.right, lessThanOrEqualTo(safe.right));
        } else {
          expect(inspector.left, greaterThanOrEqualTo(safe.left));
        }
        expect(inspector.top, greaterThanOrEqualTo(safe.top));
        expect(inspector.bottom, lessThanOrEqualTo(safe.bottom));
        expect(inspector.overlaps(before), isFalse);
        final tab = tester.getRect(
          find.byKey(const ValueKey('craft-inspector-tab-palette')),
        );
        if (side == Alignment.centerLeft) {
          expect(tab.left, moreOrLessEquals(inspector.left, epsilon: .01));
        } else {
          expect(tab.right, moreOrLessEquals(inspector.right, epsilon: .01));
        }
        final field = find.byKey(const ValueKey('craft-inspector-color-field'));
        expect(field.hitTestable(), findsOneWidget);
        final fieldRect = tester.getRect(field);
        final colorGesture = await tester.startGesture(fieldRect.topLeft);
        await colorGesture.moveTo(fieldRect.center);
        await tester.pump();
        await colorGesture.moveTo(fieldRect.bottomRight);
        await tester.pump();
        expect(controller.color, 0x000000);
        await colorGesture.moveTo(fieldRect.topLeft);
        await colorGesture.up();
        await tester.pump();
        expect(controller.color, 0xffffff);
        expect(tester.getRect(canvas), before);
        await tester.tapAt(before.center);
        await tester.pump(const Duration(milliseconds: 120));
        expect(controller.hasMarks, isTrue);
        expect(
          find.byKey(const ValueKey('craft-inspector-color-field')),
          findsOneWidget,
        );

        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      }
    },
  );

  testWidgets('Frames toolbar icon follows play and pause state', (
    tester,
  ) async {
    final haptics = _recordHoldHaptics(tester);
    final semantics = tester.ensureSemantics();
    tester.view.physicalSize = const Size(844, 390);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.addFrame();
    library.addFrame();
    library.selectFrame(1);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: RepaintBoundary(
            key: const ValueKey('animation-action-render'),
            child: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final launcher = find.byKey(const ValueKey('craft-animate'));
    Future<void> expectPlaybackAction({required bool playing}) async {
      final root = find.byKey(const ValueKey('animation-action-render'));
      final glyph = find.descendant(
        of: launcher,
        matching: find.byKey(const ValueKey('craft-animation-glyph')),
      );
      final bounds = tester.getRect(glyph);
      final offset = bounds.topLeft - tester.getTopLeft(root);
      final boundary = tester.renderObject<RenderRepaintBoundary>(root);
      final raster = (await tester.runAsync(() async {
        final image = await boundary.toImage(pixelRatio: 1);
        final bytes = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
        final width = image.width;
        image.dispose();
        return (bytes!, width);
      }))!;
      List<int> pixel(double x) {
        final column = (offset.dx + bounds.width * x).floor();
        final row = (offset.dy + bounds.height * .5).floor();
        final start = (row * raster.$2 + column) * 4;
        return [
          for (var channel = 0; channel < 3; channel++)
            raster.$1.getUint8(start + channel),
        ];
      }

      // Read the painted mark at the real 28px section glyph: the triangle
      // fills its center; pause has a black center gap and a white left bar.
      expect(pixel(.52), playing ? [0, 0, 0] : [0x41, 0xc8, 0x95]);
      if (playing) expect(pixel(.44), [255, 255, 255]);
      final preview = find.byKey(const ValueKey('craft-preview'));
      final action = playing ? 'Pause preview on this device' : 'Preview on this device';
      expect(tester.getSemantics(preview).label, action);
      expect(find.byTooltip(action), findsOneWidget);
      expect(tester.getSemantics(launcher).label, 'Animation');
      expect(tester.getSemantics(launcher).hint, 'Hold to play or pause.');
    }
    expect(
      tester
          .widget<LitchcraftAnimationGlyph>(
            find.descendant(
              of: launcher,
              matching: find.byType(LitchcraftAnimationGlyph),
            ),
          )
          .playing,
      isFalse,
    );
    expect(
      find.descendant(
        of: launcher,
        matching: find.byType(LitchcraftFrameThumbnail),
      ),
      findsNothing,
    );
    await tester.tap(launcher);
    await tester.pump(const Duration(milliseconds: 300));
    expect(tester.takeException(), isNull);
    final panel = find.byKey(const ValueKey('craft-animation-panel'));
    expect(panel, findsOneWidget);
    await expectPlaybackAction(playing: false);
    expect(
      find.byKey(const ValueKey('craft-frame-filmstrip')),
      findsOneWidget,
      reason: 'the persistent studio has no vertical reveal step',
    );
    expect(library.animationDocument.frames, hasLength(3));
    expect(library.selectedFrameIndex, 1);
    final viewport = find.byKey(const ValueKey('craft-frame-scroll-viewport'));
    final position = tester
        .state<ScrollableState>(
          find.descendant(of: viewport, matching: find.byType(Scrollable)),
        )
        .position;
    for (var index = 0; index < 3; index++) {
      expect(
        find.byKey(ValueKey('craft-select-frame-$index')),
        findsOneWidget,
        reason:
            'three-frame geometry: viewport=${tester.getRect(viewport)} '
            'offset=${position.pixels} '
            'range=${position.minScrollExtent}..${position.maxScrollExtent}',
      );
    }
    expect(
      tester.getSemantics(find.byKey(const ValueKey('craft-select-frame-1'))),
      matchesSemantics(
        isButton: true,
        isSelected: true,
        hasSelectedState: true,
        hasTapAction: true,
        hasLongPressAction: true,
        hasEnabledState: true,
        isEnabled: true,
      ),
    );
    await tester.tap(find.byKey(const ValueKey('craft-preview')));
    await tester.pump();
    expect(
      tester
          .widget<LitchcraftAnimationGlyph>(
            find.descendant(
              of: launcher,
              matching: find.byType(LitchcraftAnimationGlyph),
            ),
          )
          .playing,
      isTrue,
    );
    await expectPlaybackAction(playing: true);
    expect(
      tester.getSize(find.byKey(const ValueKey('craft-preview'))).shortestSide,
      greaterThanOrEqualTo(48),
    );
    expect(find.byKey(const ValueKey('craft-frame-previous')), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-frame-next')), findsOneWidget);
    expect(
      tester
          .getSize(find.byKey(const ValueKey('craft-frame-next')))
          .shortestSide,
      greaterThanOrEqualTo(48),
    );

    final pointer = await tester.startGesture(tester.getCenter(launcher));
    await tester.pump(const Duration(milliseconds: 100));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 399));
    expect(find.byIcon(Icons.pause), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 1));
    await pointer.up();
    await tester.pump();
    expect(find.byIcon(Icons.pause), findsNothing);
    await expectPlaybackAction(playing: false);
    expect(haptics, [
      'HapticFeedbackType.lightImpact',
      'HapticFeedbackType.mediumImpact',
    ]);
    haptics.clear();
    final node = tester.getSemantics(launcher);
    node.owner!.performAction(node.id, ui.SemanticsAction.longPress);
    await tester.pump();
    expect(find.byIcon(Icons.pause), findsOneWidget);
    await expectPlaybackAction(playing: true);
    expect(haptics, ['HapticFeedbackType.mediumImpact']);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
    semantics.dispose();
  });

  testWidgets(
    'persistent inspector honors reduced motion and 200 percent text',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              disableAnimations: true,
              textScaler: const TextScaler.linear(2),
            ),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.longPress(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pump();
      final switcher = tester.widget<AnimatedSwitcher>(
        find.descendant(
          of: find.byKey(const ValueKey('craft-canvas-toast-position')),
          matching: find.byType(AnimatedSwitcher),
        ),
      );
      expect(switcher.duration, Duration.zero);
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('craft-persistent-inspector')),
        findsOneWidget,
      );
      expect(find.byType(Scrollbar), findsNothing);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('ordinary tool guidance is transient and has no detail sheet', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final before = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.byKey(const ValueKey('craft-status-card')), findsOneWidget);
    expect(find.byKey(const ValueKey('craft-status-details')), findsNothing);
    expect(find.byType(BottomSheet), findsNothing);
    expect(
      tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
      before,
    );
    await tester.pump(const Duration(seconds: 3));
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('two-finger twist rotates artwork in an undoable 90 degree step', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final colors = List<int>.filled(256, 0)
      ..setRange(0, 4, const <int>[0x110000, 0x002200, 0x000033, 0x444444]);
    final controller = LitchcraftController(ble)
      ..restoreDraft(LitchcraftCanvas(colors: colors));
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final Finder canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    final Rect logicalZero = tester.getRect(
      find.byKey(const ValueKey('craft-square-0')),
    );
    expect(logicalZero.center.dx, lessThan(tester.getCenter(canvas).dx));
    expect(logicalZero.center.dy, lessThan(tester.getCenter(canvas).dy));

    expect(find.byKey(const ValueKey('craft-rotate-view')), findsNothing);

    final Rect bounds = tester.getRect(canvas);
    final TestGesture first = await tester.startGesture(
      bounds.center,
      pointer: 1,
    );
    // Natural placement jitter must not turn a two-finger artwork gesture into
    // an artwork edit before the second contact arrives.
    await first.moveBy(const Offset(2, 1));
    await tester.pump();
    final TestGesture second = await tester.startGesture(
      bounds.center + const Offset(60, 0),
      pointer: 2,
    );
    await tester.pump();
    await second.moveTo(bounds.center + const Offset(42, 42));
    await tester.pump();
    await second.moveTo(bounds.center + const Offset(0, 60));
    await tester.pump();
    await second.up();
    await first.up();
    await tester.pumpAndSettle();

    expect(controller.canvas.colors.skip(7 * 4).take(4), <int>[
      0x444444,
      0x110000,
      0x002200,
      0x000033,
    ]);
    expect(controller.canvas.colors.take(7 * 4), everyElement(0));
    expect(
      tester.getRect(find.byKey(const ValueKey('craft-square-0'))),
      logicalZero,
      reason: 'artwork rotates inside the stable board coordinate frame',
    );
    expect(controller.canUndo, isTrue);
    expect(find.text('Artwork rotated. To undo, press Undo.'), findsOneWidget);
    expect(tester.takeException(), isNull);

    final TestGesture third = await tester.startGesture(
      bounds.center,
      pointer: 3,
    );
    final TestGesture fourth = await tester.startGesture(
      bounds.center + const Offset(60, 0),
      pointer: 4,
    );
    await tester.pump();
    await tester.pumpWidget(const SizedBox());
    expect(tester.takeException(), isNull);
    await fourth.cancel();
    await third.cancel();
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'two-finger twist rotates effect metadata with its painted emitter',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final emitters = List<LitchcraftAnimatedEmitter>.generate(
        256,
        (index) => LitchcraftAnimatedEmitter(
          color: index == 0 ? 0x112233 : 0,
          effect: index == 0
              ? LitchcraftEffectStyle.flash
              : LitchcraftEffectStyle.off,
          effectDurationMs: index == 0 ? 1700 : 0,
        ),
      );
      final source = LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: <LitchcraftAnimationFrame>[
          LitchcraftAnimationFrame(emitters: emitters),
        ],
      );
      final session = LitchcraftDocumentSession(source);
      final controller = LitchcraftController(ble, documentSession: session);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();

      final rotateAction = find.byKey(const ValueKey('craft-rotate-artwork'));
      expect(rotateAction, findsOneWidget);
      expect(
        tester.getSize(rotateAction).shortestSide,
        greaterThanOrEqualTo(48),
      );
      expect(
        find.descendant(of: rotateAction, matching: find.byType(Text)),
        findsNothing,
        reason: 'icon-only controls do not restore removed button labels',
      );

      final bounds = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final first = await tester.startGesture(bounds.center, pointer: 1);
      final second = await tester.startGesture(
        bounds.center + const Offset(60, 0),
        pointer: 2,
      );
      await tester.pump();
      await second.moveTo(bounds.center + const Offset(42, 42));
      await tester.pump();
      await second.moveTo(bounds.center + const Offset(0, 60));
      await tester.pump();
      await second.up();
      await first.up();
      await tester.pumpAndSettle();

      // Clockwise rotation maps square 0/region 0 to square 7/region 1.
      const destinationEmitter = 7 * 4 + 1;
      final rotated = session.document.frames.single.emitters;
      expect(rotated[destinationEmitter].color, 0x112233);
      expect(rotated[destinationEmitter].effect, LitchcraftEffectStyle.flash);
      expect(rotated[destinationEmitter].effectDurationMs, 1700);
      expect(rotated[0].effect, LitchcraftEffectStyle.off);

      final reverseFirst = await tester.startGesture(bounds.center, pointer: 3);
      final reverseSecond = await tester.startGesture(
        bounds.center + const Offset(60, 0),
        pointer: 4,
      );
      await tester.pump();
      await reverseSecond.moveTo(bounds.center + const Offset(42, -42));
      await tester.pump();
      await reverseSecond.moveTo(bounds.center + const Offset(0, -60));
      await tester.pump();
      await reverseSecond.up();
      await reverseFirst.up();
      await tester.pumpAndSettle();

      final restored = session.document.frames.single.emitters;
      expect(restored[0].color, 0x112233);
      expect(restored[0].effect, LitchcraftEffectStyle.flash);
      expect(restored[0].effectDurationMs, 1700);
      expect(restored[destinationEmitter].effect, LitchcraftEffectStyle.off);
      expect(tester.takeException(), isNull);

      await tester.tap(rotateAction);
      await tester.pumpAndSettle();
      expect(
        session.document.frames.single.emitters[0].effect,
        LitchcraftEffectStyle.off,
      );
      expect(
        session.document.frames.single.emitters[destinationEmitter].effect,
        LitchcraftEffectStyle.flash,
      );
      await controller.undo();
      await tester.pumpAndSettle();
      expect(
        session.document.frames.single.emitters[0].effect,
        LitchcraftEffectStyle.flash,
      );
      expect(
        session.document.frames.single.emitters[destinationEmitter].effect,
        LitchcraftEffectStyle.off,
      );

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('refused two-finger rotation does not announce success', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _RefusedRotationCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final bounds = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final first = await tester.startGesture(bounds.center, pointer: 1);
    final second = await tester.startGesture(
      bounds.center + const Offset(60, 0),
      pointer: 2,
    );
    await tester.pump();
    await second.moveTo(bounds.center + const Offset(42, 42));
    await tester.pump();
    await second.moveTo(bounds.center + const Offset(0, 60));
    await tester.pump();
    await second.up();
    await first.up();
    await tester.pumpAndSettle();

    expect(controller.rotationAttempts, 1);
    expect(find.text('Artwork rotated. To undo, press Undo.'), findsNothing);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'compact inspector exposes one-step Paint and Erase mode control',
    (tester) async {
      tester.view.physicalSize = const Size(390, 760);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _PendingCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox(
              width: 390,
              height: 760,
              child: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final brush = find.byKey(const ValueKey('craft-quick-brush'));
      expect(tester.getSize(brush).shortestSide, greaterThanOrEqualTo(48));
      expect(
        tester.getSemantics(
          find.byKey(const ValueKey('craft-quick-brush-semantics')),
        ),
        matchesSemantics(
          label: 'Paint',
          hint: 'Erase mode',
          isButton: true,
          hasEnabledState: true,
          isEnabled: true,
          hasSelectedState: true,
          isSelected: true,
          hasTapAction: true,
        ),
      );
      await tester.tap(brush);
      await tester.pumpAndSettle();
      expect(controller.eraser, isTrue);
      expect(
        tester.getSemantics(
          find.byKey(const ValueKey('craft-quick-brush-semantics')),
        ),
        matchesSemantics(
          label: 'Erase',
          hint: 'Paint mode',
          isButton: true,
          hasEnabledState: true,
          isEnabled: true,
          hasSelectedState: true,
          isSelected: true,
          hasTapAction: true,
        ),
      );
      final toolKeys = <String>[
        'craft-palette',
        'craft-leds',
        'craft-motion',
        'craft-animate',
        'craft-artwork-menu',
      ];
      final rowCenters = <int>{};
      for (final key in toolKeys) {
        final control = find.byKey(ValueKey(key));
        expect(tester.getSize(control).shortestSide, greaterThanOrEqualTo(48));
        rowCenters.add(tester.getCenter(control).dy.round());
      }
      expect(rowCenters.length, lessThanOrEqualTo(2));
      expect(
        tester.getSize(find.byKey(const ValueKey('craft-tool-grid'))).width,
        lessThanOrEqualTo(
          tester.getSize(find.byKey(const ValueKey('litchcraft-canvas'))).width,
        ),
      );
      expect(find.byType(Scrollbar), findsNothing);
      expect(find.byKey(const ValueKey('craft-target-15')), findsNothing);
      for (final key in <String>['craft-motion']) {
        expect(
          tester.getSize(find.byKey(ValueKey(key))).shortestSide,
          greaterThanOrEqualTo(48),
        );
      }
      expect(find.byKey(const ValueKey('craft-help')), findsNothing);
      expect(find.byIcon(Icons.sensors_rounded), findsNothing);

      final brushAction = find.ancestor(
        of: brush,
        matching: find.byType(LitchcraftHoldAction),
      );
      var reachedBrush = false;
      for (var step = 0; step < 32; step++) {
        await tester.sendKeyEvent(LogicalKeyboardKey.tab);
        await tester.pump();
        if (_primaryFocusIsWithin(brushAction)) {
          reachedBrush = true;
          break;
        }
      }
      expect(reachedBrush, isTrue);
      await tester.sendKeyEvent(LogicalKeyboardKey.space);
      await tester.pump(const Duration(milliseconds: 200));
      expect(controller.eraser, isFalse);
      await tester.tap(brush.hitTestable());
      await tester.pump(const Duration(milliseconds: 200));
      expect(controller.eraser, isTrue);
      await tester.tap(brush.hitTestable());
      await tester.pump(const Duration(milliseconds: 200));
      expect(controller.eraser, isFalse);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );
  testWidgets('welcome guidance stays put until the first stroke ends', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    await tester.ensureVisible(canvas);
    await tester.pumpAndSettle();
    final before = tester.getRect(canvas);
    final gesture = await tester.startGesture(before.center);
    await gesture.moveBy(const Offset(24, 0));
    await tester.pump(const Duration(milliseconds: 650));

    expect(controller.hasMarks, isTrue);
    expect(library.dismissedWelcome, isFalse);
    expect(tester.getRect(canvas).top, before.top);

    // A connection lifecycle event can end the logical controller stroke, but
    // must not move the canvas while the physical pointer still owns it.
    controller.endStroke();
    await tester.pump();
    expect(library.dismissedWelcome, isFalse);
    expect(tester.getRect(canvas).top, before.top);

    await gesture.up();
    await tester.pump(const Duration(milliseconds: 650));
    expect(library.dismissedWelcome, isTrue);
    final persisted = await LitchcraftArtworkStore(
      await SharedPreferences.getInstance(),
    ).load();
    expect(persisted.dismissedWelcome, isTrue);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'a second tap still paints while the prior live stroke is awaiting acknowledgement',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      // Controlled transport-pending presentation; protocol/controller tests own
      // the real ACK queue. This catches the screen disabling the next pointer.
      final controller = _PendingCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await library.dismissWelcome();
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
      await tester.pumpAndSettle();
      expect(controller.eraser, isTrue);
      await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
      await tester.pumpAndSettle();
      expect(controller.eraser, isFalse);
      final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
      await tester.ensureVisible(canvas);
      await tester.pumpAndSettle();
      final rect = tester.getRect(canvas);
      await tester.tapAt(
        rect.topLeft + Offset(rect.width / 16, rect.height / 16),
      );
      await tester.pump();
      await tester.tapAt(
        rect.topLeft + Offset(rect.width * 3 / 16, rect.height / 16),
      );
      await tester.pump();
      expect(controller.canvas.colors[0], litchcraftPalette.first);
      expect(controller.canvas.colors[4], litchcraftPalette.first);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final phase in [
    'entry',
    'board read',
    'sustained handoff',
    'completed transfer',
  ]) {
    testWidgets(
      'Store preparation feedback stays truthful through $phase',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _NoticeConnectionBle();
        final controller = _NoticeConnectionController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await library.dismissWelcome();
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        if (phase == 'sustained handoff') {
          controller.setStatus(LitchcraftStatus.connecting);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 250));
          expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
          controller.showBoardRead(true);
          controller.setStatus(LitchcraftStatus.live);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 249));
          expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
          await tester.pump(const Duration(milliseconds: 1));
          expect(find.text('Reading artwork from your Litch…'), findsOneWidget);
          expect(
            find.byKey(const ValueKey('craft-sync-activity')),
            findsOneWidget,
          );
          controller.showBoardRead(false);
          await tester.pump();
        } else if (phase == 'completed transfer') {
          controller.showFullSync(
            LitchcraftFullSyncDirection.sendingToBoard,
            .4,
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 500));
          expect(find.text('Sending animation to your Litch…'), findsOneWidget);
          controller.showFullSync(null, null);
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-sync-activity')),
            findsNothing,
          );
          expect(
            find.byType(LinearProgressIndicator),
            findsNothing,
            reason: 'Completion cannot retain cached progress',
          );
          expect(find.text('Sending animation to your Litch…'), findsNothing);
          await tester.pump(const Duration(milliseconds: 680));
        } else {
          if (phase == 'entry') {
            controller.setStatus(LitchcraftStatus.connecting);
          } else {
            controller.showBoardRead(true);
          }
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-status-card')),
            findsNothing,
            reason: 'Routine preparation must pass the existing notice delay',
          );
          await tester.pump(const Duration(milliseconds: 200));
          controller.setStatus(LitchcraftStatus.live);
          controller.showBoardRead(true);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 200));
          controller.showBoardRead(false);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 700));
        }
        expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        expect(
          find.byKey(const ValueKey('craft-canvas-toast-position')),
          findsNothing,
        );
        expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
    );
  }

  testWidgets('short artwork readback never flashes Syncing', (tester) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _HistoryRecoveryCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    controller.setHistoryRecovery(true);
    controller.setArtworkReadback(true);
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.text('Syncing…'), findsNothing);
    expect(
      find.byKey(const ValueKey('craft-canvas-toast-position')),
      findsNothing,
    );
    expect(controller.artworkReadbackPending, isTrue);
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .enabled,
      isTrue,
    );
    controller.setHistoryRecovery(false);
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text('Syncing…'), findsNothing);
    expect(
      find.byKey(const ValueKey('craft-canvas-toast-position')),
      findsNothing,
    );
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'readback activity retires with completion and errors bypass its delay',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _HistoryRecoveryCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      controller.setHistoryRecovery(true);
      controller.setArtworkReadback(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 499));
      expect(find.text('Syncing…'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-canvas-toast-position')),
        findsNothing,
      );
      await tester.pump(const Duration(milliseconds: 1));
      expect(find.text('Syncing…'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-canvas-toast-position')),
        findsOneWidget,
      );
      await tester.pump(const Duration(milliseconds: 180));
      controller.setHistoryRecovery(false);
      await tester.pump();
      expect(controller.artworkReadbackPending, isFalse);
      expect(find.text('Syncing…'), findsNothing);
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .enabled,
        isTrue,
      );
      await tester.pump(const Duration(milliseconds: 499));
      expect(find.text('Syncing…'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-canvas-toast-position')),
        findsNothing,
      );
      await tester.pump(const Duration(milliseconds: 1));
      expect(find.text('Syncing…'), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-canvas-toast-position')),
        findsNothing,
      );
      controller.setHistoryRecovery(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('Syncing…'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-canvas-toast-position')),
        findsOneWidget,
      );
      controller.setStatus(LitchcraftStatus.error);
      await tester.pump();
      expect(find.text('Syncing…'), findsNothing);
      expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('sync phases share one notice without restarting its timer', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ArtworkDeliveryController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    controller.showFullSync(LitchcraftFullSyncDirection.sendingToBoard, .4);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 499));
    expect(
      find.byKey(const ValueKey('craft-full-animation-sync')),
      findsNothing,
    );
    await tester.pump(const Duration(milliseconds: 1));
    expect(
      find.byKey(const ValueKey('craft-full-animation-sync')),
      findsOneWidget,
    );
    await tester.pump(const Duration(milliseconds: 180));
    final status = find.byKey(const ValueKey('craft-status-card'));
    final position = tester.getRect(status);
    controller.showFullSync(LitchcraftFullSyncDirection.readingFromBoard, .6);
    await tester.pump();
    expect(find.text('Reading animation from your Litch…'), findsOneWidget);
    expect(tester.getRect(status), position);
    controller.showFullSync(null, null);
    controller.showRootPending(true);
    await tester.pump();
    expect(find.text('Sending artwork…'), findsOneWidget);
    controller.showRootPending(false);
    controller.showReadback(true);
    await tester.pump();
    expect(find.text('Syncing…'), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 500));
    controller.showReadback(false);
    await tester.pump();
    expect(
      find.text('Syncing…'),
      findsNothing,
      reason: 'Completion retires the continuous phase notice',
    );
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'sync notice yields to failures and queued play without retaining actions',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ArtworkDeliveryController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      for (final status in [
        LitchcraftStatus.conflict,
        LitchcraftStatus.error,
        LitchcraftStatus.offline,
        LitchcraftStatus.gameActive,
      ]) {
        controller.setStatus(LitchcraftStatus.live);
        controller.showRootPending(true);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        expect(find.text('Sending artwork…'), findsOneWidget);
        controller.setStatus(status);
        await tester.pump();
        expect(
          find.text('Sending artwork…'),
          findsNothing,
          reason: 'current recovery or departure immediately replaces sync',
        );
      }
      controller.setStatus(LitchcraftStatus.live);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('Sending artwork…'), findsOneWidget);
      library.saveFailed = true;
      controller.showRootPending(true);
      await tester.pump();
      expect(find.text('Sending artwork…'), findsNothing);
      expect(
        find.text('Couldn’t save. Your canvas is still here. Try again.'),
        findsOneWidget,
      );
      library.saveFailed = false;
      controller.showQueuedPlay(true);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-cancel-queued-play')),
        findsOneWidget,
      );
      expect(find.text('Sending artwork…'), findsNothing);
      controller.showQueuedPlay(false);
      controller.showRootPending(false);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('craft-cancel-queued-play')),
        findsNothing,
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('source and route lifecycle cancel the active sync notice', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ArtworkDeliveryController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    bool active = true;
    late StateSetter updateRoute;
    await tester.pumpWidget(
      MaterialApp(
        home: StatefulBuilder(
          builder: (context, setState) {
            updateRoute = setState;
            return Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                active: active,
              ),
            );
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
    Future<void> show() async {
      controller.showRootPending(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('Sending artwork…'), findsOneWidget);
    }

    await show();
    controller.showRootPending(false);
    controller.documentSession.open(library.animationDocument);
    await tester.pump();
    expect(find.text('Sending artwork…'), findsNothing);
    await show();
    updateRoute(() => active = false);
    await tester.pump();
    expect(find.text('Sending artwork…'), findsNothing);
    updateRoute(() => active = true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 499));
    expect(find.text('Sending artwork…'), findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    expect(find.text('Sending artwork…'), findsOneWidget);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    await tester.pump();
    expect(find.text('Sending artwork…'), findsNothing);
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 499));
    expect(find.text('Sending artwork…'), findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    expect(find.text('Sending artwork…'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('readback notice never moves or blocks a held drawing gesture', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ArtworkDeliveryController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await library.dismissWelcome();
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    final position = tester.getRect(canvas);
    controller.showReadback(true);
    await tester.pump();
    final pointer = await tester.startGesture(
      position.topLeft + Offset(position.width / 16, position.height / 16),
    );
    await tester.pump(const Duration(milliseconds: 250));
    await pointer.moveTo(
      position.topLeft + Offset(position.width * 3 / 16, position.height / 16),
    );
    await tester.pump(const Duration(milliseconds: 250));
    expect(find.text('Syncing…'), findsOneWidget);
    expect(tester.getRect(canvas), position);
    await pointer.moveTo(
      position.topLeft + Offset(position.width * 5 / 16, position.height / 16),
    );
    controller.showReadback(false);
    await tester.pump();
    expect(tester.getRect(canvas), position);
    await pointer.up();
    await tester.pump();
    expect(controller.canvas.colors[0], litchcraftPalette.first);
    expect(controller.canvas.colors[4], litchcraftPalette.first);
    expect(controller.canvas.colors[8], litchcraftPalette.first);
    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'reduced motion retires completed activity immediately',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ArtworkDeliveryController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => MediaQuery(
              data: MediaQuery.of(context).copyWith(disableAnimations: true),
              child: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      controller.showReadback(true);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('Syncing…'), findsOneWidget);
      controller.showReadback(false);
      await tester.pump();
      expect(controller.artworkReadbackPending, isFalse);
      expect(find.text('Syncing…'), findsNothing);
      await tester.pump(const Duration(milliseconds: 499));
      expect(find.text('Syncing…'), findsNothing);
      await tester.pump(const Duration(milliseconds: 1));
      expect(find.text('Syncing…'), findsNothing);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('separate short sync operations each start a fresh delay', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ArtworkDeliveryController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    controller.showReadback(true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    controller.showReadback(false);
    await tester.pump();
    controller.showReadback(true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 499));
    expect(find.text('Syncing…'), findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    expect(find.text('Syncing…'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'a new connection and board binding retire the previous sync notice',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = _NoticeConnectionBle();
      final controller = _NoticeConnectionController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      Future<void> show() async {
        controller.showReadback(true);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        expect(find.text('Syncing…'), findsOneWidget);
        controller.showReadback(false);
        await tester.pump();
      }

      await show();
      ble.epoch++;
      controller.setStatus(LitchcraftStatus.connecting);
      await tester.pump();
      expect(find.text('Syncing…'), findsNothing);
      expect(find.text('Connecting your canvas…'), findsNothing);
      controller.setStatus(LitchcraftStatus.live);
      await tester.pump();
      await show();
      ble.epoch++;
      ble.board = 'second-board';
      controller.showReadback(false);
      await tester.pump();
      expect(
        find.text('Syncing…'),
        findsNothing,
        reason:
            'same controller and document cannot retain another board connection notice',
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('replacing the controller cancels its active sync notice', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final first = _ArtworkDeliveryController(ble);
    final second = _ArtworkDeliveryController(ble);
    final firstLibrary = LitchcraftLibrary(first);
    final secondLibrary = LitchcraftLibrary(second);
    await Future.wait([firstLibrary.ready, secondLibrary.ready]);
    Widget screen(
      _ArtworkDeliveryController controller,
      LitchcraftLibrary library,
    ) => MaterialApp(
      home: Scaffold(
        body: LitchcraftScreen(controller: controller, library: library),
      ),
    );
    await tester.pumpWidget(screen(first, firstLibrary));
    await tester.pumpAndSettle();
    first.showReadback(true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.text('Syncing…'), findsOneWidget);
    await tester.pumpWidget(screen(second, secondLibrary));
    expect(find.text('Syncing…'), findsNothing);
    await tester.pumpWidget(const SizedBox());
    firstLibrary.dispose();
    secondLibrary.dispose();
    first.dispose();
    second.dispose();
    await ble.dispose();
  });

  testWidgets('blocking read cannot mask current recovery and library failures', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ArtworkDeliveryController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    controller.showBoardRead(true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 700));
    expect(find.text('Reading artwork from your Litch…'), findsOneWidget);
    // Controlled overlap models failure notification before an async read's
    // finally block retires its loading flag; no transport timing is faked.
    for (final status in [LitchcraftStatus.error, LitchcraftStatus.offline]) {
      controller.setStatus(status);
      await tester.pump();
      expect(find.text('Reading artwork from your Litch…'), findsNothing);
    }
    controller.setStatus(LitchcraftStatus.live);
    library.saveFailed = true;
    controller.showBoardRead(true);
    await tester.pump();
    expect(find.text('Reading artwork from your Litch…'), findsNothing);
    expect(
      find.text('Couldn’t save. Your canvas is still here. Try again.'),
      findsOneWidget,
    );
    library.saveFailed = false;
    controller.boardUnavailable = true;
    controller.showBoardRead(true);
    await tester.pump();
    expect(find.text('Reading artwork from your Litch…'), findsNothing);
    expect(
      find.text(
        'Could not read artwork from your Litch. Your app artwork is safe. Try again.',
      ),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('native artwork readback has a truthful scoped pending cue', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final semantics = tester.ensureSemantics();
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _HistoryRecoveryCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Syncing…'), findsNothing);
    controller.setHistoryRecovery(true);
    await tester.pump(const Duration(milliseconds: 200));
    expect(
      find.text('Syncing…'),
      findsNothing,
      reason: 'History acknowledgement alone does not transfer artwork',
    );
    controller.setArtworkReadback(true);
    // Only sustained artwork readback presents the scoped pending notice.
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.text('Syncing…'), findsOneWidget);
    expect(
      tester
          .widget<LinearProgressIndicator>(
            find.byKey(const ValueKey('craft-interactive-sync-pending')),
          )
          .value,
      isNull,
    );
    final status = find.byKey(const ValueKey('craft-status-card'));
    expect(
      tester
          .getSemantics(status)
          .getSemanticsData()
          .flagsCollection
          .isLiveRegion,
      isTrue,
    );
    expect(find.byKey(const ValueKey('craft-sync-activity')), findsOneWidget);
    controller.setHistoryRecovery(false);
    await tester.pump();
    expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);
    await tester.pump(const Duration(milliseconds: 700));
    expect(find.text('Syncing…'), findsNothing);
    controller.setHistoryRecovery(true);
    controller.setStatus(LitchcraftStatus.conflict);
    await tester.pumpAndSettle();
    expect(find.text('Syncing…'), findsNothing);
    expect(
      find.byKey(const ValueKey('craft-resolve-conflict')),
      findsOneWidget,
    );
    controller.setStatus(LitchcraftStatus.error);
    await tester.pumpAndSettle();
    expect(find.text('Syncing…'), findsNothing);
    expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);
    controller.setStatus(LitchcraftStatus.offline);
    await tester.pumpAndSettle();
    expect(find.text('Syncing…'), findsNothing);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
    semantics.dispose();
  });

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'pending artwork chooser clears Android bottom inset at scale $scale',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final previousPlatform = debugDefaultTargetPlatformOverride;
        debugDefaultTargetPlatformOverride = TargetPlatform.android;
        await loadHealthReviewFonts(tester);
        final semantics = tester.ensureSemantics();
        try {
          final ble = LitchBleService(
            pendingOtaStore: MemoryOtaTargetStore(),
            ble: _NoopBle(),
          );
          final controller = _PendingConflictPreviewController(ble);
          final library = LitchcraftLibrary(controller);
          await library.ready;
          library.recovered = true;
          library.dismissedWelcome = true;
          addTearDown(() async {
            await tester.pumpWidget(const SizedBox.shrink());
            library.dispose();
            controller.dispose();
            await ble.dispose();
          });
          final draft = controller.canvas;
          const reviewKey = ValueKey('craft-chooser-inset-review');
          await tester.pumpWidget(
            RepaintBoundary(
              key: reviewKey,
              child: MaterialApp(
                theme: buildLitchTheme(),
                localizationsDelegates: AppLocalizations.localizationsDelegates,
                supportedLocales: AppLocalizations.supportedLocales,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context).copyWith(
                    padding: const EdgeInsets.only(bottom: 48),
                    viewPadding: const EdgeInsets.only(bottom: 48),
                    textScaler: TextScaler.linear(scale),
                  ),
                  child: child!,
                ),
                home: Scaffold(
                  body: LitchcraftScreen(
                    controller: controller,
                    library: library,
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          expect(controller.previewStarts, 0);
          await tester.tap(
            find.byKey(const ValueKey('craft-resolve-conflict')),
          );
          await tester.pump(const Duration(milliseconds: 300));
          final sheet = find.byType(BottomSheet);
          final sheetMedia = MediaQuery.of(tester.element(sheet));
          expect(sheetMedia.padding.bottom, 48);
          expect(sheetMedia.viewPadding.bottom, 48);
          expect(controller.previewStarts, 1);
          final appChoice = find.byKey(const ValueKey('craft-use-app-artwork'));
          expect(tester.widget<InkWell>(appChoice).onTap, isNotNull);
          expect(find.text('Reading artwork from your Litch…'), findsOneWidget);
          expect(controller.canvas, draft);
          expect(controller.resolvedWithBoard, isNull);

          controller.failPreview();
          await tester.pump();
          expect(find.text('Try again'), findsOneWidget);
          expect(tester.widget<InkWell>(appChoice).onTap, isNotNull);
          expect(controller.canvas, draft);
          controller.finishPreview();
          await tester.pumpAndSettle();
          final scrollable = find.descendant(
            of: sheet,
            matching: find.byType(Scrollable),
          );
          expect(scrollable, findsOneWidget);
          // Real sheet gestures must expose the whole last choice at maximum
          // scroll, including its consequence text above system navigation.
          await tester.drag(scrollable, const Offset(0, -1200));
          await tester.pumpAndSettle();
          final position = tester.state<ScrollableState>(scrollable).position;
          expect(position.pixels, closeTo(position.maxScrollExtent, 0.5));
          final card = tester.getRect(appChoice);
          final consequence = find.descendant(
            of: appChoice,
            matching: find.text(
              'The physical board changes to match this app canvas.',
            ),
          );
          final consequenceRect = tester.getRect(consequence);
          expect(
            tester.renderObject<RenderParagraph>(consequence).didExceedMaxLines,
            isFalse,
          );
          expect(card.width, greaterThanOrEqualTo(48));
          expect(card.height, greaterThanOrEqualTo(48));
          final appSemantics = tester.getSemantics(appChoice);
          expect(appSemantics.rect.width, greaterThanOrEqualTo(48));
          expect(appSemantics.rect.height, greaterThanOrEqualTo(48));
          expect(
            appSemantics.getSemanticsData().hasAction(ui.SemanticsAction.tap),
            isTrue,
          );
          expect(appChoice.hitTestable(), findsOneWidget);
          debugPrint(
            'CHOOSER_INSET scale=$scale viewport=390x844 bottomInset=48 '
            'scroll=${position.pixels}/${position.maxScrollExtent} '
            'card=$card consequence=$consequenceRect safeBottom=796 '
            'enabled=true tapSemantics=true draftPreserved=true',
          );
          await captureHealthReview(
            tester,
            'chooser-bottom-inset-390x844-scale-$scale',
            boundaryFinder: find.byKey(reviewKey),
          );
          expect(
            card.bottom,
            lessThanOrEqualTo(796),
            reason:
                'The whole app choice must clear the 48px system navigation',
          );
          expect(consequenceRect.bottom, lessThanOrEqualTo(796));
          await tester.tap(appChoice);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          expect(find.byType(BottomSheet), findsNothing);
          expect(controller.resolvedWithBoard, isFalse);
          expect(controller.previewRetirements, 1);
          expect(controller.canvas, draft);
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isFalse,
            reason: 'The existing source choice stays pending until resolution',
          );
          controller.release.complete();
          await tester.pumpAndSettle();
          expect(controller.canvas, draft);
          expect(tester.takeException(), isNull);
        } finally {
          debugDefaultTargetPlatformOverride = previousPlatform;
          semantics.dispose();
        }
      },
    );
  }

  for (final useBoard in <bool?>[false, true, null]) {
    testWidgets(
      'pending board chooser previews before choice and retires for $useBoard',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(320, 844);
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = _PendingConflictPreviewController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.recovered = true;
        final draft = controller.canvas;
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(controller.previewStarts, 0);
        await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
        await tester.pump(const Duration(milliseconds: 300));
        expect(controller.previewStarts, 1);
        expect(find.byIcon(Icons.download_for_offline), findsNothing);
        final boardChoice = find.byKey(
          const ValueKey('craft-use-board-artwork'),
        );
        final appChoice = find.byKey(const ValueKey('craft-use-app-artwork'));
        expect(
          find.descendant(
            of: boardChoice,
            matching: find.byType(LitchcraftThumbnail),
          ),
          findsNothing,
        );
        expect(find.text('Reading artwork from your Litch…'), findsOneWidget);
        expect(tester.widget<InkWell>(appChoice).onTap, isNotNull);
        expect(controller.canvas, draft);
        expect(controller.resolvedWithBoard, isNull);

        controller.failPreview();
        await tester.pumpAndSettle();
        expect(find.text('Try again'), findsOneWidget);
        await tester.ensureVisible(find.text('Try again'));
        await tester.pumpAndSettle();
        await tester.tap(find.text('Try again'));
        await tester.pump();
        expect(controller.previewStarts, 2);
        expect(find.byType(BottomSheet), findsOneWidget);
        expect(controller.resolvedWithBoard, isNull);

        controller.finishPreview();
        await tester.pumpAndSettle();
        final boardPreview = tester.widget<LitchcraftThumbnail>(
          find.descendant(
            of: boardChoice,
            matching: find.byType(LitchcraftThumbnail),
          ),
        );
        final appPreview = tester.widget<LitchcraftThumbnail>(
          find.descendant(
            of: appChoice,
            matching: find.byType(LitchcraftThumbnail),
          ),
        );
        expect(
          boardPreview.canvas.colors.sublist(252, 256),
          everyElement(0x0070ff),
        );
        expect(appPreview.canvas.colors[0], 0xff7430);
        expect(
          controller.boardCanvas.colors.sublist(252, 256),
          everyElement(0),
        );
        expect(controller.canvas, draft);
        expect(controller.resolvedWithBoard, isNull);
        if (useBoard == null) {
          final sheetContext = tester.element(find.byType(BottomSheet));
          Navigator.of(sheetContext).pop();
        } else {
          final choice = useBoard ? boardChoice : appChoice;
          await tester.ensureVisible(choice);
          await tester.pumpAndSettle();
          await tester.tap(choice);
        }
        await tester.pump(const Duration(milliseconds: 300));
        expect(controller.previewRetirements, useBoard == true ? 0 : 1);
        expect(controller.resolvedWithBoard, useBoard);
        controller.release.complete();
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      },
    );
  }

  testWidgets('conflict stays passive at 200% until its choice is opened', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    tester.view.physicalSize = const Size(320, 844);
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.recovered = true;
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(
            context,
          ).copyWith(textScaler: const TextScaler.linear(2)),
          child: child!,
        ),
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey('craft-resolve-conflict')),
      findsOneWidget,
    );
    expect(find.text('Choose artwork'), findsOneWidget);
    expect(find.byType(BottomSheet), findsNothing);
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.byType(BottomSheet),
        matching: find.byType(LitchcraftThumbnail),
      ),
      findsNWidgets(2),
    );
    expect(find.text('Use the artwork on your Litch'), findsOneWidget);
    expect(
      find.text('The app changes to match the physical board.'),
      findsOneWidget,
    );
    expect(find.text('Put the app artwork on your Litch'), findsOneWidget);
    expect(
      find.text('The physical board changes to match this app canvas.'),
      findsOneWidget,
    );
    await tester.ensureVisible(
      find.byKey(const ValueKey('craft-use-app-artwork')),
    );
    await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
    await tester.pump();
    expect(controller.resolvedWithBoard, isFalse);
    final before = controller.canvas;
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    await tester.tapAt(tester.getRect(canvas).center);
    await tester.pump();
    expect(controller.canvas, before);
    controller.release.complete();
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'retained Litchcraft conflict UI never escapes onto another page',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ConflictCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              active: false,
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byType(BottomSheet), findsNothing);
      expect(find.text('Choose artwork'), findsNothing);

      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              active: true,
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byType(BottomSheet), findsNothing);
      expect(find.text('Choose artwork'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('craft-resolve-conflict')),
        findsOneWidget,
      );

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('resident read explains why canvas drawing waits', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _LoadingBoardCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 700));
    expect(find.text('Reading artwork from your Litch…'), findsOneWidget);
    final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .enabled,
      isFalse,
    );
    await tester.tapAt(tester.getRect(canvas).center);
    await tester.pump();
    expect(controller.paintAttempts, 0);

    controller.failLoading();
    await tester.pumpAndSettle();
    expect(
      find.text(
        'Board controls unavailable. Your app artwork is safe. Try again.',
      ),
      findsOneWidget,
    );
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .enabled,
      isFalse,
    );

    controller.currentStatus = LitchcraftStatus.live;
    controller.finishLoading();
    await tester.pumpAndSettle();
    expect(find.text('Reading artwork from your Litch…'), findsNothing);
    expect(
      tester
          .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .enabled,
      isTrue,
    );
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('board artwork choice explains the read while controls wait', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-use-board-artwork')));
    await tester.pump(const Duration(milliseconds: 300));
    expect(controller.resolvedWithBoard, isTrue);
    final statusCard = find.byKey(const ValueKey('craft-status-card'));
    expect(
      find.descendant(
        of: statusCard,
        matching: find.text('Reading artwork from your Litch…'),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(of: statusCard, matching: find.text('Choose artwork')),
      findsNothing,
    );
    controller.release.complete();
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('unresolved Store artwork does not animate the app draft', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictPlayingCraftController(ble);
    controller.restoreDocument(
      LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: <LitchcraftAnimationFrame>[
          for (final color in <int>[0xff0000, 0x0000ff])
            LitchcraftAnimationFrame(
              durationMs: 100,
              emitters: List<LitchcraftAnimatedEmitter>.filled(
                256,
                LitchcraftAnimatedEmitter(color: color),
              ),
            ),
        ],
      ),
    );
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    LitchcraftCanvasView appCanvas() => tester.widget<LitchcraftCanvasView>(
      find.ancestor(
        of: find.byKey(const ValueKey('litchcraft-canvas')),
        matching: find.byType(LitchcraftCanvasView),
      ),
    );
    final initial = appCanvas().canvas;
    controller.boardElapsed = 150;
    controller.notifyListeners();
    await tester.pump();
    expect(appCanvas().canvas, initial);
    expect(find.text('Play board'), findsNothing);
    expect(find.text('Pause board'), findsNothing);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'failed board read keeps a named retry after transient feedback',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _FailingConflictCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-use-board-artwork')));
      await tester.pumpAndSettle();
      await tester.pump(const Duration(seconds: 4));
      expect(
        find.text(
          'Could not read the artwork on your Litch. Your app artwork is safe. '
          'Choose again to retry.',
        ),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
      await tester.pumpAndSettle();
      expect(find.byType(BottomSheet), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('a new conflict remains passive after an old choice closes', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    final firstSheet = tester.widget<BottomSheet>(find.byType(BottomSheet));
    controller.setStatus(LitchcraftStatus.live);
    await tester.pump();
    controller.replacePhysicalArtwork(
      controller.physicalArtwork.paint(const <int>[7], 15, 0x2eff00),
    );
    controller.setStatus(LitchcraftStatus.conflict);
    await tester.pump();
    await tester.tapAt(const Offset(4, 4));
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsNothing);
    expect(
      find.byKey(const ValueKey('craft-resolve-conflict')),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsOneWidget);
    expect(
      tester.widget<BottomSheet>(find.byType(BottomSheet)),
      isNot(same(firstSheet)),
    );
    controller.setStatus(LitchcraftStatus.live);
    Navigator.of(tester.element(find.byType(LitchcraftScreen))).pop();
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'reconnect does not repeatedly reopen the same artwork conflict',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _ConflictCraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Choose artwork'), findsOneWidget);
      expect(find.byType(BottomSheet), findsNothing);

      controller.setStatus(LitchcraftStatus.offline);
      await tester.pump();
      controller.setStatus(LitchcraftStatus.conflict);
      await tester.pumpAndSettle();

      expect(find.byType(BottomSheet), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-resolve-conflict')),
        findsOneWidget,
        reason: 'the unresolved choice stays available without nagging again',
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('disconnect closes the artwork choice', (tester) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Choose artwork'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsOneWidget);
    controller.setStatus(LitchcraftStatus.offline);
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsNothing);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('choosing an artwork never opens a stale second conflict sheet', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ConflictCraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    final obsoleteSheet = tester.widget<BottomSheet>(find.byType(BottomSheet));
    controller.setStatus(LitchcraftStatus.live);
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsNothing);
    controller.setStatus(LitchcraftStatus.conflict);
    await tester.pumpAndSettle();
    expect(
      find.byType(BottomSheet),
      findsNothing,
      reason: 'a later conflict requires a fresh deliberate source choice',
    );
    await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
    await tester.pumpAndSettle();
    expect(
      tester.widget<BottomSheet>(find.byType(BottomSheet)),
      isNot(same(obsoleteSheet)),
    );
    await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
    await tester.pumpAndSettle();
    expect(controller.resolvedWithBoard, isFalse);
    expect(find.byType(BottomSheet), findsNothing);
    controller.setStatus(LitchcraftStatus.live);
    controller.release.complete();
    await tester.pumpAndSettle();
    expect(find.byType(BottomSheet), findsNothing);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  test('blank canvas uses chessboard light-square parity', () {
    expect(litchcraftDisplaySquareIsDark(0), isFalse, reason: 'a8 is light');
    expect(litchcraftDisplaySquareIsDark(7), isTrue, reason: 'h8 is dark');
    expect(litchcraftDisplaySquareIsDark(56), isTrue, reason: 'a1 is dark');
    expect(litchcraftDisplaySquareIsDark(63), isFalse, reason: 'h1 is light');
  });

  test('detail targets follow the physical player-end flip', () {
    const flippedMasks = <int, int>{1: 4, 2: 8, 4: 1, 8: 2, 10: 10, 15: 15};
    for (final flipped in <bool>[false, true]) {
      for (final mask in <int>[1, 2, 4, 8, 10, 15]) {
        expect(
          litchcraftDisplayTargetMask(mask, flipped: flipped),
          flipped ? flippedMasks[mask] : mask,
          reason: 'mask=$mask flipped=$flipped',
        );
      }
    }
  });

  testWidgets('saving disables only Save while painting remains interactive', (
    tester,
  ) async {
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _SlowArtworkSaveController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    controller.beginStroke();
    controller.paintSquares(<int>[0]);
    controller.endStroke();
    await library.save('Working artwork');
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    controller.coherentRelease = Completer<void>();
    await tester.tap(find.byKey(const ValueKey('craft-save')));
    await tester.pump();

    expect(
      tester
          .widget<TextButton>(find.byKey(const ValueKey('craft-save')))
          .onPressed,
      isNull,
    );
    final canvas = tester.widget<LitchcraftCanvasView>(
      find
          .ancestor(
            of: find.byKey(const ValueKey('litchcraft-canvas')),
            matching: find.byType(LitchcraftCanvasView),
          )
          .first,
    );
    expect(canvas.enabled, isTrue);
    final beforeRotation = List<int>.of(controller.canvas.colors);
    await tester.tap(find.byKey(const ValueKey('craft-rotate-artwork')));
    await tester.pump();
    expect(controller.canvas.colors, isNot(equals(beforeRotation)));

    controller.coherentRelease!.complete();
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'offline artwork saves, renames, duplicates, reopens and restores after restart',
    (tester) async {
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      controller.beginStroke();
      controller.paintSquares(<int>[0, 7, 63]);
      controller.endStroke();
      await library.save('First painting');
      expect(library.dirty, isFalse);
      final source = library.artworks.single;
      await library.rename(source, 'Corners');
      await library.duplicate(library.artworks.single);
      expect(library.artworks, hasLength(2));
      expect(library.artworks.map((art) => art.id).toSet(), hasLength(2));
      await controller.clear();
      expect(library.dirty, isTrue);
      await library.open(source);
      expect(controller.canvas.colors[0], litchcraftPalette.first);
      expect(controller.canvas.colors[63 * 4], litchcraftPalette.first);
      await library.flush();
      library.dispose();
      controller.dispose();
      final restoredController = LitchcraftController(ble);
      final restoredLibrary = LitchcraftLibrary(restoredController);
      await restoredLibrary.ready;
      expect(restoredController.canvas, source.canvas);
      expect(restoredLibrary.artworks, hasLength(2));
      await restoredLibrary.delete(restoredLibrary.artworks.first);
      expect(restoredLibrary.artworks, hasLength(1));
      expect(restoredController.canvas, source.canvas);
      await restoredLibrary.flush();
      restoredLibrary.dispose();
      restoredController.dispose();
      await ble.dispose();
    },
  );

  testWidgets('artwork opens as a searchable full-screen library', (
    tester,
  ) async {
    final haptics = _recordHoldHaptics(tester);
    final semantics = tester.ensureSemantics();
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _SlowArtworkOpenController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await library.save('Sunset', saveAs: true);
    controller.beginStroke();
    controller.paintSquares(<int>[0]);
    controller.endStroke();
    await library.save('Forest', saveAs: true);
    final sunset = library.artworks.singleWhere((art) => art.name == 'Sunset');
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-library')));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('craft-library-search')), findsOneWidget);
    expect(find.byType(BottomSheet), findsNothing);
    expect(find.text('Sunset'), findsOneWidget);
    expect(find.text('Forest'), findsOneWidget);
    final shareSunset = find.byKey(
      ValueKey('craft-share-artwork-${sunset.id}'),
    );
    expect(shareSunset, findsOneWidget);
    expect(shareSunset.hitTestable(), findsOneWidget);
    expect(tester.getSize(shareSunset).shortestSide, greaterThanOrEqualTo(48));
    final deleteSunset = find.byKey(
      ValueKey('craft-delete-artwork-${sunset.id}'),
    );
    final renameSunset = find.byKey(
      ValueKey('craft-rename-artwork-${sunset.id}'),
    );
    expect(renameSunset.hitTestable(), findsOneWidget);
    expect(deleteSunset.hitTestable(), findsOneWidget);
    expect(tester.getSize(deleteSunset).shortestSide, greaterThanOrEqualTo(48));
    expect(
      tester.getTopLeft(deleteSunset).dx,
      lessThan(tester.getTopLeft(shareSunset).dx),
    );
    expect(find.text('Open'), findsNothing);
    expect(find.text('Export / Share'), findsNothing);

    final sunsetRow = find.byKey(ValueKey('craft-artwork-${sunset.id}'));
    final partial = await tester.startGesture(tester.getCenter(sunsetRow));
    await tester.pump(const Duration(milliseconds: 100));
    await partial.cancel();
    await tester.pump(const Duration(seconds: 1));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    expect(find.byKey(const ValueKey('craft-library-search')), findsOneWidget);
    expect(find.text('Rename'), findsNothing);
    haptics.clear();

    final pointer = await tester.startGesture(tester.getCenter(sunsetRow));
    await tester.pump(const Duration(milliseconds: 100));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 399));
    expect(find.text('Rename'), findsNothing);
    await tester.pump(const Duration(milliseconds: 1));
    await pointer.up();
    await tester.pumpAndSettle();
    expect(haptics, [
      'HapticFeedbackType.lightImpact',
      'HapticFeedbackType.mediumImpact',
    ]);
    expect(find.text('Rename'), findsOneWidget);
    expect(find.widgetWithText(TextFormField, 'Sunset'), findsOneWidget);
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-library')));
    await tester.pumpAndSettle();
    haptics.clear();
    final node = tester.getSemantics(sunsetRow);
    node.owner!.performAction(node.id, ui.SemanticsAction.longPress);
    await tester.pumpAndSettle();
    expect(haptics, ['HapticFeedbackType.mediumImpact']);
    expect(find.widgetWithText(TextFormField, 'Sunset'), findsOneWidget);
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-library')));
    await tester.pumpAndSettle();
    haptics.clear();
    await tester.tap(find.byKey(ValueKey('craft-artwork-${sunset.id}')));
    await tester.pumpAndSettle();
    expect(haptics, isEmpty);
    expect(find.byKey(const ValueKey('craft-library-search')), findsNothing);
    expect(controller.canvas, sunset.canvas);
    expect(
      tester
          .widget<LitchcraftCanvasView>(
            find
                .ancestor(
                  of: find.byKey(const ValueKey('litchcraft-canvas')),
                  matching: find.byType(LitchcraftCanvasView),
                )
                .first,
          )
          .enabled,
      isTrue,
      reason: 'board acknowledgement must not disable the reopened editor',
    );
    controller.replacement!.complete();
    await tester.pumpAndSettle();
    semantics.dispose();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('library holds preserve unavailable and recovery row actions', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    final haptics = _recordHoldHaptics(tester);
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    final date = DateTime.utc(2026, 10, 22);
    library.previousDraft = LitchcraftArtwork(
      id: 'draft',
      name: 'Draft',
      canvas: LitchcraftCanvas.blank(),
      createdAt: date,
      modifiedAt: date,
    );
    library.artworks = [
      LitchcraftArtwork.fromJson({
        'version': 99,
        'id': 'future',
        'name': 'Future',
        'createdAt': date.toIso8601String(),
        'modifiedAt': date.toIso8601String(),
        'mapping': 'a8-first-left-top-right-bottom',
      }),
    ];
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-library')));
    await tester.pumpAndSettle();
    final recovery = find.byKey(const ValueKey('craft-artwork-previous-draft'));
    final recoveryNode = tester.getSemantics(recovery).getSemanticsData();
    expect(recoveryNode.hasAction(ui.SemanticsAction.tap), isTrue);
    expect(recoveryNode.hasAction(ui.SemanticsAction.longPress), isFalse);
    final pointer = await tester.startGesture(tester.getCenter(recovery));
    await tester.pump(const Duration(milliseconds: 600));
    await pointer.cancel();
    await tester.pump();
    expect(haptics, isEmpty);

    final unavailable = find.byKey(const ValueKey('craft-artwork-future'));
    final node = tester.getSemantics(unavailable);
    expect(node.getSemanticsData().hasAction(ui.SemanticsAction.tap), isFalse);
    expect(
      node.getSemanticsData().hasAction(ui.SemanticsAction.longPress),
      isTrue,
    );
    expect(
      tester
          .getSemantics(
            find.byKey(const ValueKey('craft-delete-artwork-future')),
          )
          .getSemanticsData()
          .hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    await tester.tap(unavailable);
    await tester.pump();
    expect(find.byKey(const ValueKey('craft-library-search')), findsOneWidget);
    expect(haptics, isEmpty);
    node.owner!.performAction(node.id, ui.SemanticsAction.longPress);
    await tester.pumpAndSettle();
    expect(haptics, ['HapticFeedbackType.mediumImpact']);
    expect(find.widgetWithText(TextFormField, 'Future'), findsOneWidget);
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    semantics.dispose();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'artwork hold saves changes before New and cancellation preserves them',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final artwork = find.byKey(const ValueKey('craft-artwork-menu'));
      Future<void> hold() async {
        final gesture = await tester.startGesture(tester.getCenter(artwork));
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.byType(AlertDialog), findsNothing);
        await tester.pump(const Duration(milliseconds: 400));
        await gesture.up();
        await tester.pumpAndSettle();
      }

      // A pristine document needs no save decision.
      await hold();
      expect(find.byType(AlertDialog), findsNothing);
      controller.beginStroke();
      controller.paintSquares(<int>[0]);
      controller.endStroke();
      await tester.pumpAndSettle();
      final original = library.animationDocument;
      expect(controller.hasMarks, isTrue);
      // A cancelled partial hold never opens the New flow.
      final partial = await tester.startGesture(tester.getCenter(artwork));
      await tester.pump(const Duration(milliseconds: 300));
      await partial.cancel();
      await tester.pumpAndSettle();
      expect(library.animationDocument, original);
      expect(find.byType(AlertDialog), findsNothing);
      await hold();
      expect(
        find.text('Save changes before creating new artwork?'),
        findsOneWidget,
      );
      await tester.tap(find.text('Cancel'));
      await tester.pumpAndSettle();
      expect(library.animationDocument, original);
      // Canceling Save As also aborts New.
      await hold();
      await tester.tap(find.widgetWithText(FilledButton, 'Save'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Cancel'));
      await tester.pumpAndSettle();
      expect(library.animationDocument, original);
      await hold();
      await tester.tap(find.widgetWithText(FilledButton, 'Save'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextFormField), 'Before new');
      await tester.tap(find.widgetWithText(FilledButton, 'Save'));
      await tester.pumpAndSettle();
      expect(controller.hasMarks, isFalse);
      expect(library.artworks.single.name, 'Before new');
      expect(
        sameLitchcraftDocument(library.artworks.single.document, original),
        isTrue,
      );
      expect(library.previousDraft?.canvas.hasMarks, isTrue);
      // Reopening the unchanged saved source skips the prompt.
      await library.open(library.artworks.single);
      await tester.pumpAndSettle();
      await hold();
      expect(find.byType(AlertDialog), findsNothing);
      expect(controller.hasMarks, isFalse);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('artwork hold keeps document when saving fails', (tester) async {
    final haptics = _recordHoldHaptics(tester);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = _FailingArtworkSaveLibrary(controller);
    await library.ready;
    controller.beginStroke();
    controller.paintSquares(<int>[0]);
    controller.endStroke();
    final original = library.animationDocument;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final semantics = tester.ensureSemantics();
    final node = tester.getSemantics(
      find.byKey(const ValueKey('craft-artwork-menu')),
    );
    expect(
      node.getSemanticsData().hasAction(ui.SemanticsAction.longPress),
      isTrue,
    );
    tester.binding.renderViews.single.owner!.semanticsOwner!.performAction(
      node.id,
      ui.SemanticsAction.longPress,
    );
    await tester.pumpAndSettle();
    expect(haptics, ['HapticFeedbackType.mediumImpact']);
    await tester.tap(find.widgetWithText(FilledButton, 'Save'));
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, 'Save'));
    await tester.pumpAndSettle();
    expect(library.animationDocument, original);
    expect(library.artworks, isEmpty);
    semantics.dispose();
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'artwork hold publishes unnamed editable New before board receipt',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = _SlowArtworkOpenController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      controller.beginStroke();
      controller.paintSquares(<int>[0]);
      controller.endStroke();
      await library.save('Original');
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final gesture = await tester.startGesture(
        tester.getCenter(find.byKey(const ValueKey('craft-artwork-menu'))),
      );
      await tester.pump(const Duration(milliseconds: 800));
      await gesture.up();
      await tester.pumpAndSettle();
      expect(controller.replacement, isNotNull);
      expect(controller.replacement!.isCompleted, isFalse);
      expect(library.activeName, isNull);
      expect(controller.hasMarks, isFalse);
      final canvas = tester
          .widgetList<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
          .firstWhere((view) => view.enabled);
      expect(canvas.enabled, isTrue);
      await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-save')));
      await tester.pumpAndSettle();
      expect(find.byType(TextFormField), findsOneWidget);
      await tester.enterText(find.byType(TextFormField), 'New blank');
      await tester.tap(find.widgetWithText(FilledButton, 'Save'));
      await tester.pumpAndSettle();
      expect(library.artworks.length, 2);
      expect(
        library.artworks
            .firstWhere((art) => art.name == 'Original')
            .canvas
            .hasMarks,
        isTrue,
      );
      controller.replacement!.complete();
      await tester.pumpAndSettle();
      expect(library.activeName, 'New blank');
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('artwork plus menu starts New and preserves the prior draft', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    controller.beginStroke();
    controller.paintSquares(<int>[0]);
    controller.endStroke();
    expect(controller.hasMarks, isTrue);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-library')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('craft-add-artwork')));
    await tester.pumpAndSettle();
    expect(find.text('New artwork'), findsOneWidget);
    await tester.tap(find.text('New artwork'));
    await tester.pumpAndSettle();
    expect(
      find.text('Save changes before creating new artwork?'),
      findsOneWidget,
    );
    await tester.tap(find.text('Don’t save'));
    await tester.pumpAndSettle();

    expect(controller.hasMarks, isFalse);
    expect(library.previousDraft?.canvas.hasMarks, isTrue);
    expect(library.activeName, isNull);

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'opening preserves previous canvas and saving unchanged adaptation preserves source topology',
    (tester) async {
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      controller.restoreDraft(
        LitchcraftCanvas.blank().paint(<int>[0], 2, 0x123456),
      );
      await library.save('Detailed source');
      final source = library.artworks.single;
      controller.restoreDraft(
        source.canvas.withTopology(LitchcraftTopology.v1),
      );
      await library.save('Detailed source');
      expect(library.artworks.single.canvas.topology, LitchcraftTopology.v3);
      controller.beginStroke();
      controller.paintSquares(<int>[7]);
      controller.endStroke();
      final beforeOpen = controller.canvas;
      await library.open(source);
      expect(library.previousDraft!.canvas.colors, beforeOpen.colors);
      expect(library.previousDraft!.canvas.topology, LitchcraftTopology.v3);
      final stored = await LitchcraftArtworkStore(
        await SharedPreferences.getInstance(),
      ).load();
      expect(stored.previousDraft!.canvas.colors, beforeOpen.colors);
      expect(stored.previousDraft!.canvas.topology, LitchcraftTopology.v3);
      await library.flush();
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final width in <double>[320, 390]) {
    for (final scale in <double>[1, 2]) {
      testWidgets(
        'painting screen fits $width at ${scale * 100}% text and palette exits eraser',
        (tester) async {
          tester.view.physicalSize = Size(width, 844);
          final ble = LitchBleService(
            pendingOtaStore: MemoryOtaTargetStore(),
            ble: _NoopBle(),
          );
          final controller = LitchcraftController(ble);
          final library = LitchcraftLibrary(controller);
          await library.ready;
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(scale)),
                child: child!,
              ),
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          final canvas = find.byKey(const ValueKey('litchcraft-canvas'));
          final rect = tester.getRect(canvas);
          await tester.tapAt(
            rect.topLeft + Offset(rect.width / 16, rect.height / 16),
          );
          await tester.pumpAndSettle();
          expect(controller.canvas.colors[0], litchcraftPalette.first);
          expect(library.dismissedWelcome, isTrue);
          if (width == 320 && scale == 1) {
            final undo = tester.getRect(
              find.byKey(const ValueKey('craft-undo')),
            );
            final redo = tester.getRect(
              find.byKey(const ValueKey('craft-redo')),
            );
            expect(
              redo.top,
              undo.top,
              reason: 'Undo and Redo remain a pair; Redo is never orphaned',
            );
            final history = tester.getRect(
              find.byKey(const ValueKey('craft-history-footer')),
            );
            expect(history.contains(undo.center), isTrue);
            expect(history.contains(redo.center), isTrue);
            expect(
              find.descendant(
                of: find.byKey(const ValueKey('craft-history-footer')),
                matching: find.byKey(const ValueKey('craft-undo')),
              ),
              findsOneWidget,
            );
            expect(
              find.descendant(
                of: find.byKey(const ValueKey('craft-history-footer')),
                matching: find.byKey(const ValueKey('craft-redo')),
              ),
              findsOneWidget,
            );
            await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
            await tester.pumpAndSettle();
            expect(
              tester.getTopLeft(find.byKey(const ValueKey('craft-library'))).dy,
              lessThan(undo.top),
            );
            await tester.tap(find.byKey(const ValueKey('craft-palette')));
            await tester.pumpAndSettle();
          }
          await controller.setBrush(eraser: true);
          final teal = find.byKey(const ValueKey('craft-wheel-color-3'));
          await _revealInspectorControl(tester, teal);
          await tester.tap(teal.hitTestable());
          await tester.pump();
          expect(controller.eraser, isFalse);
          expect(controller.color, litchcraftPaletteRailColors[3]);
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox());
          await library.flush();
          library.dispose();
          controller.dispose();
          await ble.dispose();
        },
      );
    }
  }

  testWidgets(
    'one-shot color pick samples the selected emitter without painting and preserves paint color on off squares',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      controller.restoreDraft(
        LitchcraftCanvas.blank()
            .paint(<int>[0], 1, 0xff0000)
            .paint(<int>[0], 2, 0x0000ff),
      );
      await controller.setBrush(targetMask: 2);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final before = controller.canvas;
      library.recovered = true;
      await tester.longPress(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pumpAndSettle();
      expect(
        find.text(
          'Recovered the last saved draft. Some newer changes could not be read.',
        ),
        findsOneWidget,
        reason: 'essential recovery remains visible while color pick is armed',
      );
      expect(
        find.text(
          'Tap a square to pick its color. Your artwork stays unchanged.',
        ),
        findsNothing,
      );
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final first =
          canvas.topLeft + Offset(canvas.width / 16, canvas.height / 16);
      final second = first + Offset(canvas.width / 8, 0);
      final gesture = await tester.startGesture(first);
      await tester.pump();
      await gesture.moveTo(second);
      await tester.pump();
      await gesture.up();
      expect(controller.canvas, before);
      expect(controller.color, 0x0000ff);
      expect(controller.eraser, isFalse);
      await tester.longPress(
        find.byKey(const ValueKey('craft-quick-previous-color')),
      );
      await tester.pumpAndSettle();
      await tester.tapAt(second);
      await tester.pump();
      expect(controller.canvas, before);
      expect(controller.eraser, isTrue);
      expect(controller.color, 0x0000ff);
      await tester.tap(find.byKey(const ValueKey('craft-quick-brush')));
      await tester.pumpAndSettle();
      expect(controller.eraser, isFalse);
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .enabled,
        isTrue,
      );
      final currentCanvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final paintSecond =
          currentCanvas.topLeft +
          Offset(currentCanvas.width * 3 / 16, currentCanvas.height / 16);
      await tester.tapAt(paintSecond);
      await tester.pumpAndSettle();
      expect(controller.canvas.colors[5], 0x0000ff);
      expect(controller.canvas.colors[4], 0);
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  for (final topology in <LitchcraftTopology>[
    LitchcraftTopology.v1,
    LitchcraftTopology.v3,
  ]) {
    for (final flipped in <bool>[false, true]) {
      testWidgets(
        'spatial target picker respects ${topology.name} orientation flipped=$flipped',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          int? selected;
          await tester.pumpWidget(
            MaterialApp(
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              home: Builder(
                builder: (context) => Scaffold(
                  body: TextButton(
                    onPressed: () async =>
                        selected = await showLitchcraftDetailPicker(
                          context,
                          topology,
                          15,
                          flipped: flipped,
                        ),
                    child: const Text('Detail'),
                  ),
                ),
              ),
            ),
          );
          await tester.tap(find.text('Detail'));
          await tester.pumpAndSettle();
          expect(find.byType(BottomSheet), findsOneWidget);
          final left = tester.getCenter(
            find.byKey(const ValueKey('craft-target-1')),
          );
          final right = tester.getCenter(
            find.byKey(const ValueKey('craft-target-4')),
          );
          expect(left.dx < right.dx, !flipped);
          expect(
            find.descendant(
              of: find.byTooltip(flipped ? 'Right outward' : 'Left outward'),
              matching: find.byKey(const ValueKey('craft-target-1')),
            ),
            findsOneWidget,
          );
          for (final mask in topology.targets) {
            final rect = tester.getRect(
              find.byKey(ValueKey('craft-target-$mask')),
            );
            expect(rect.width, greaterThanOrEqualTo(48));
            expect(rect.height, greaterThanOrEqualTo(48));
          }
          if (topology == LitchcraftTopology.v3) {
            final top = tester.getCenter(
              find.byKey(const ValueKey('craft-target-2')),
            );
            final bottom = tester.getCenter(
              find.byKey(const ValueKey('craft-target-8')),
            );
            expect(top.dy < bottom.dy, !flipped);
          } else {
            expect(
              tester
                  .getCenter(find.byKey(const ValueKey('craft-target-10')))
                  .dx,
              closeTo((left.dx + right.dx) / 2, .01),
            );
          }
          await tester.tap(find.byKey(const ValueKey('craft-target-1')));
          await tester.pumpAndSettle();
          expect(selected, 1);
        },
      );
    }
  }

  testWidgets(
    'Color and LEDs switch exclusively while canvas painting stays direct',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final before = controller.canvas;
      final canvas = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );

      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-leds')));
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('craft-target-15')), findsOneWidget);
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        canvas,
      );
      await tester.tap(find.byKey(const ValueKey('craft-palette')));
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        find.byKey(const ValueKey('craft-target-15')).hitTestable(),
        findsNothing,
      );
      final colorField = find.byKey(
        const ValueKey('craft-inspector-color-field'),
      );
      expect(colorField, findsOneWidget);
      expect(
        tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
        canvas,
      );
      for (final key in <String>[
        'craft-palette',
        'craft-leds',
        'craft-motion',
        'craft-animate',
        'craft-undo',
        'craft-redo',
      ]) {
        final size = tester.getSize(find.byKey(ValueKey(key)));
        expect(size.width, greaterThanOrEqualTo(48), reason: key);
        expect(size.height, greaterThanOrEqualTo(48), reason: key);
      }

      for (final key in <String>[
        'craft-paint',
        'craft-erase',
        'craft-pick-color',
        'craft-fill',
      ]) {
        expect(find.byKey(ValueKey(key)), findsNothing);
      }
      await tester.tapAt(canvas.center);
      await tester.pump(const Duration(milliseconds: 120));
      expect(controller.canvas, isNot(before));
      expect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
        findsOneWidget,
      );

      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('ribbon safe-pane geometry and palette endpoints at both text scales', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    for (final sample in <(String, Size, EdgeInsets, Alignment)>[
      (
        'portrait',
        const Size(390, 844),
        const EdgeInsets.fromLTRB(0, 47, 0, 34),
        Alignment.centerRight,
      ),
      (
        'narrow-portrait',
        const Size(320, 640),
        const EdgeInsets.fromLTRB(0, 24, 0, 24),
        Alignment.centerRight,
      ),
      (
        'landscape-left',
        const Size(844, 390),
        const EdgeInsets.fromLTRB(47, 0, 0, 21),
        Alignment.centerLeft,
      ),
      (
        'landscape-right',
        const Size(844, 390),
        const EdgeInsets.fromLTRB(0, 0, 47, 21),
        Alignment.centerRight,
      ),
      (
        'narrow-left',
        const Size(667, 375),
        const EdgeInsets.fromLTRB(44, 0, 44, 21),
        Alignment.centerLeft,
      ),
      (
        'narrow-right',
        const Size(667, 375),
        const EdgeInsets.fromLTRB(44, 0, 44, 21),
        Alignment.centerRight,
      ),
    ]) {
      for (final scale in <double>[1, 2]) {
        tester.view.physicalSize = sample.$2;
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _NoopBle(),
        );
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        controller.restoreDraft(
          LitchcraftCanvas.blank().paint(<int>[0, 9, 18, 27], 15, 0x22c3e2),
        );
        await tester.pumpWidget(
          RepaintBoundary(
            key: const ValueKey('ribbon-review-boundary'),
            child: MaterialApp(
              theme: buildLitchTheme(),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  padding: sample.$3,
                  viewPadding: sample.$3,
                  textScaler: TextScaler.linear(scale),
                ),
                child: child!,
              ),
              home: Scaffold(
                appBar: sample.$2.width <= sample.$2.height
                    ? AppBar(title: const Text('LITCHCRAFT'))
                    : null,
                body: SafeArea(
                  top: sample.$2.width > sample.$2.height,
                  bottom: sample.$2.width > sample.$2.height,
                  child: LitchcraftScreen(
                    controller: controller,
                    library: library,
                    landscapeRibbonSide: sample.$4,
                    rootSafeInsets: sample.$3,
                  ),
                ),
                bottomNavigationBar: sample.$2.width <= sample.$2.height
                    ? NavigationBar(
                        selectedIndex: 0,
                        destinations: const <NavigationDestination>[
                          NavigationDestination(
                            icon: Icon(Icons.brush_outlined),
                            label: 'Litchcraft',
                          ),
                          NavigationDestination(
                            icon: Icon(Icons.sports_esports_outlined),
                            label: 'Play',
                          ),
                          NavigationDestination(
                            icon: Icon(Icons.settings_outlined),
                            label: 'Setup',
                          ),
                        ],
                      )
                    : null,
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final canvasFinder = find.byKey(const ValueKey('litchcraft-canvas'));
        expect(
          tester.takeException(),
          isNull,
          reason: '${sample.$1} at ${scale}x initial layout',
        );
        final canvas = tester.getRect(canvasFinder);
        final ribbonBoard = tester.getRect(
          find.byKey(const ValueKey('craft-ribbon-board')),
        );
        final safe = Rect.fromLTRB(
          sample.$3.left,
          sample.$3.top,
          sample.$2.width - sample.$3.right,
          sample.$2.height - sample.$3.bottom,
        );
        final vertical = sample.$2.width > sample.$2.height;
        if (!vertical) {
          expect(
            canvas.center.dx,
            closeTo(safe.center.dx, .01),
            reason:
                '${sample.$1} scale $scale canvas=$canvas board=$ribbonBoard safe=$safe',
          );
        }
        if (vertical) {
          expect(
            canvas.center.dy,
            closeTo(safe.center.dy, .01),
            reason: 'landscape centers within the complete OS-safe viewport',
          );
        }
        expect(canvas.left, greaterThanOrEqualTo(safe.left));
        expect(
          canvas.top,
          greaterThanOrEqualTo(vertical ? safe.top : 0),
          reason:
              'portrait receives app-bar-adjusted local bounds; landscape owns its top inset',
        );
        expect(canvas.right, lessThanOrEqualTo(safe.right));
        expect(canvas.bottom, lessThanOrEqualTo(safe.bottom));
        final toolbar = tester.getRect(
          find.byKey(const ValueKey('craft-toolbar')),
        );
        final inspector = tester.getRect(
          find.byKey(const ValueKey('craft-inspector')),
        );
        expect(
          _toolbarAxis(tester),
          vertical ? Axis.vertical : Axis.horizontal,
        );
        expect(toolbar.overlaps(canvas), isFalse);
        expect(toolbar.left, greaterThanOrEqualTo(0));
        expect(toolbar.top, greaterThanOrEqualTo(0));
        expect(toolbar.right, lessThanOrEqualTo(sample.$2.width));
        expect(toolbar.bottom, lessThanOrEqualTo(sample.$2.height));
        if (!vertical) {
          expect(safe.contains(toolbar.topLeft), isTrue);
          expect(toolbar.right, lessThanOrEqualTo(safe.right));
          expect(toolbar.bottom, lessThanOrEqualTo(safe.bottom));
        }
        if (vertical) {
          expect(
            sample.$4.x < 0 ? toolbar.left : toolbar.right,
            closeTo(sample.$4.x < 0 ? inspector.left : inspector.right, .01),
            reason:
                'landscape tool buttons stay on the outside edge of the tool area',
          );
        } else {
          expect(
            toolbar.bottom,
            closeTo(inspector.bottom, .01),
            reason: 'portrait tool buttons stay at the bottom of the inspector',
          );
        }
        for (final key in <String>[
          'craft-quick-brush',
          'craft-quick-previous-color',
          'craft-quick-fill',
          'craft-palette',
          'craft-leds',
          'craft-motion',
          'craft-animate',
          'craft-artwork-menu',
          'craft-undo',
          'craft-redo',
        ]) {
          final action = find.byKey(ValueKey(key));
          await tester.ensureVisible(action);
          await tester.pumpAndSettle();
          expect(
            action.hitTestable(),
            findsOneWidget,
            reason: '$key ${sample.$1} $scale',
          );
          expect(tester.getSize(action).shortestSide, greaterThanOrEqualTo(48));
        }
        final palette = find.byKey(const ValueKey('craft-palette'));
        if (!vertical) {
          expect(
            tester
                .getSize(find.byKey(const ValueKey('craft-color-wheel-rail')))
                .height,
            greaterThanOrEqualTo(64),
            reason:
                'Portrait color browsing needs a forgiving drag target so a swatch drag does not become page navigation.',
          );
        }
        await tester.ensureVisible(palette);
        await tester.pumpAndSettle();
        final boundary = find.byKey(const ValueKey('ribbon-review-boundary'));
        await captureHealthReview(
          tester,
          'ribbon-${sample.$1}-$scale-closed',
          boundaryFinder: boundary,
        );
        await tester.tap(palette);
        await tester.pumpAndSettle();
        expect(
          tester.takeException(),
          isNull,
          reason: '${sample.$1} at ${scale}x color inspector',
        );
        final swap = find.byKey(const ValueKey('craft-quick-previous-color'));
        expect(swap, findsOneWidget);
        expect(tester.getSize(swap).shortestSide, greaterThanOrEqualTo(48));
        expect(swap.hitTestable(), findsOneWidget);
        final tray = tester.getRect(
          find.byKey(const ValueKey('craft-persistent-inspector')),
        );
        expect(tray.left, greaterThanOrEqualTo(0));
        expect(tray.top, greaterThanOrEqualTo(0));
        expect(tray.right, lessThanOrEqualTo(sample.$2.width));
        expect(tray.bottom, lessThanOrEqualTo(sample.$2.height));
        if (!vertical) {
          expect(safe.contains(tray.topLeft), isTrue);
          expect(tray.right, lessThanOrEqualTo(safe.right));
          expect(tray.bottom, lessThanOrEqualTo(safe.bottom));
        }
        expect(tray.overlaps(canvas), isFalse);
        final visibleSwatches = find
            .byWidgetPredicate(
              (widget) =>
                  widget.key is ValueKey<String> &&
                  (widget.key! as ValueKey<String>).value.startsWith(
                    'craft-wheel-color-',
                  ),
            )
            .hitTestable();
        expect(visibleSwatches, findsWidgets);
        final visibleElements = visibleSwatches.evaluate().toList();
        final swatchCenters = visibleElements
            .map(
              (element) => tester.getCenter(
                find.byElementPredicate(
                  (candidate) => identical(candidate, element),
                ),
              ),
            )
            .toList(growable: false);
        for (var index = 1; index < swatchCenters.length; index++) {
          final previous = swatchCenters[index - 1];
          final current = swatchCenters[index];
          expect(
            vertical ? current.dy > previous.dy : current.dx > previous.dx,
            isTrue,
            reason:
                'palette order is stable at ${sample.$1} scale $scale: '
                '${litchcraftPaletteRailColors[index - 1]} before '
                '${litchcraftPaletteRailColors[index]}',
          );
        }
        for (final element in visibleElements) {
          final swatch = find.byElementPredicate(
            (candidate) => identical(candidate, element),
          );
          expect(tester.getSize(swatch).shortestSide, greaterThanOrEqualTo(48));
        }
        expect(tester.getSize(canvasFinder), canvas.size);
        if (vertical) expect(tester.getRect(canvasFinder), canvas);
        await captureHealthReview(
          tester,
          'ribbon-${sample.$1}-$scale-color',
          boundaryFinder: boundary,
        );
        expect(
          find.descendant(
            of: find.byKey(const ValueKey('craft-persistent-inspector')),
            matching: find.byType(Divider),
          ),
          findsNothing,
          reason:
              'The Color panel has no line below the saturation/value field '
              'in ${sample.$1} at ${scale}x.',
        );
        final selectedSwatchKey =
            visibleElements.first.widget.key! as ValueKey<String>;
        final selectedSwatchIndex = int.parse(
          selectedSwatchKey.value.substring('craft-wheel-color-'.length),
        );
        await tester.tap(find.byKey(selectedSwatchKey));
        await tester.pumpAndSettle();
        expect(
          controller.color,
          litchcraftPaletteRailColors[selectedSwatchIndex],
        );
        expect(
          find.byKey(const ValueKey('craft-inspector-color-field')),
          findsOneWidget,
        );
        await tester.tap(find.byKey(const ValueKey('craft-leds')));
        await tester.pumpAndSettle();
        expect(
          tester.takeException(),
          isNull,
          reason: '${sample.$1} at ${scale}x LED inspector',
        );
        expect(find.byKey(const ValueKey('craft-led-panel')), findsOneWidget);
        expect(tester.getSize(canvasFinder), canvas.size);
        if (vertical) expect(tester.getRect(canvasFinder), canvas);
        for (final mask in <int>[1, 2, 4, 8, 15]) {
          final target = find.byKey(ValueKey('craft-target-$mask'));
          final rect = tester.getRect(target);
          expect(target.hitTestable(), findsOneWidget);
          expect(rect.size.shortestSide, greaterThanOrEqualTo(47.9));
          expect(rect.top, greaterThanOrEqualTo(safe.top));
          expect(rect.bottom, lessThanOrEqualTo(safe.bottom));
          final menuViewport = tester.getRect(
            find.byKey(const ValueKey('craft-persistent-inspector')),
          );
          expect(rect.top, greaterThanOrEqualTo(menuViewport.top));
          expect(rect.bottom, lessThanOrEqualTo(menuViewport.bottom));
          if (vertical) expect(rect.overlaps(canvas), isFalse);
        }
        await captureHealthReview(
          tester,
          'ribbon-${sample.$1}-$scale-tool',
          boundaryFinder: boundary,
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(const ValueKey('craft-tool-tray')), findsNothing);
        expect(
          find.byKey(const ValueKey('craft-led-panel')),
          findsOneWidget,
          reason: 'the fixed inspector is not a dismissible overlay',
        );
        expect(tester.getSize(canvasFinder), canvas.size);
        if (vertical) expect(tester.getRect(canvasFinder), canvas);
        expect(
          tester.takeException(),
          isNull,
          reason: '${sample.$1} at ${scale}x text',
        );
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
      }
    }
  });

  testWidgets('dock state survives portrait landscape recomposition', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(390, 844);
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await controller.setBrush(color: litchcraftPalette[4], targetMask: 4);
    library.addFrame();
    final document = library.animationDocument;
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    tester.view.physicalSize = const Size(844, 390);
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('craft-landscape-shell')), findsOneWidget);
    expect(controller.color, litchcraftPalette[4]);
    expect(controller.targetMask, 4);
    expect(library.animationDocument, same(document));
    expect(library.animationDocument.frames, hasLength(2));
    tester.view.physicalSize = const Size(390, 844);
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('craft-portrait-shell')), findsOneWidget);
    expect(controller.color, litchcraftPalette[4]);
    expect(controller.targetMask, 4);
    expect(library.animationDocument.frames, hasLength(2));

    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets(
    'color field follows a held two-dimensional drag and preset applies in one tap',
    (tester) async {
      LitchcraftColorPickerResult? selected;
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () async => selected =
                    await showLitchcraftColorPicker(context, 0xff7430),
                child: const Text('Pick'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Pick'));
      await tester.pumpAndSettle();
      final field = find.byKey(const ValueKey('craft-color-field'));
      final rect = tester.getRect(field);
      final gesture = await tester.startGesture(
        rect.topLeft + Offset(rect.width * .2, rect.height * .2),
      );
      await gesture.moveTo(
        rect.topLeft + Offset(rect.width * .4, rect.height * .4),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final hex = find.byWidgetPredicate(
        (widget) => widget is Text && (widget.data?.startsWith('#') ?? false),
      );
      final first = tester.widget<Text>(hex).data;
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.widget<Text>(hex).data, first);
      expect(tester.getRect(field), rect);
      await gesture.moveTo(
        rect.topLeft + Offset(rect.width * .8, rect.height * .7),
      );
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Text>(hex).data, isNot(first));
      await gesture.up();
      await tester.tap(find.text('Use color'));
      await tester.pumpAndSettle();
      expect(selected?.color, isNot(0xff7430));
      expect(selected?.fillCanvas, isFalse);
      await tester.tap(find.text('Pick'));
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(ValueKey('craft-picker-color-${litchcraftPalette[3]}')),
      );
      await tester.pumpAndSettle();
      expect(selected?.color, litchcraftPalette[3]);
      expect(selected?.fillCanvas, isFalse);
      expect(find.byType(BottomSheet), findsNothing);
    },
  );

  testWidgets(
    'custom picker real held drag remains stable across intermediate pumps',
    (tester) async {
      LitchcraftColorPickerResult? selected;
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () async {
                  selected = await showLitchcraftColorPicker(context, 0xff7430);
                },
                child: const Text('Pick'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Pick'));
      await tester.pumpAndSettle();
      final hue = find.byType(Slider).first;
      final rect = tester.getRect(hue);
      final gesture = await tester.startGesture(
        Offset(rect.left + 24, rect.center.dy),
      );
      await gesture.moveTo(
        Offset(rect.left + rect.width * .35, rect.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final first = tester.widget<Slider>(hue).value;
      await tester.pump(const Duration(milliseconds: 30));
      expect(tester.widget<Slider>(hue).value, first);
      await gesture.moveTo(Offset(rect.left + rect.width * .7, rect.center.dy));
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(hue).value, greaterThan(first));
      await gesture.up();
      await tester.pumpAndSettle();
      await tester.tap(find.text('Use color'));
      await tester.pumpAndSettle();
      expect(selected, isNotNull);
      expect(selected?.color, isNot(0xff7430));
      expect(selected?.fillCanvas, isFalse);
    },
  );
}
