import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/device_landscape_side.dart';
import 'package:litch_mobile/src/widgets/orientation_workspace.dart';

void main() {
  testWidgets(
    'iOS landscape keeps only the camera edge and matches portrait on the clear edge',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(852, 393);
      addTearDown(tester.view.reset);
      const channel = MethodChannel(
        'com.foxieproducts.litch/interface_orientation',
      );
      var side = 'right';
      var compensate = false;
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'orientationState'
            ? {'side': side, 'compensateHalfTurn': compensate}
            : null,
      );
      addTearDown(
        () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          channel,
          null,
        ),
      );
      MediaQueryData? observed;
      await tester.pumpWidget(
        MaterialApp(
          theme: ThemeData(platform: TargetPlatform.iOS),
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              padding: const EdgeInsets.fromLTRB(59, 0, 59, 21),
              viewPadding: const EdgeInsets.fromLTRB(59, 0, 59, 21),
            ),
            child: OrientationWorkspace(child: child!),
          ),
          home: Builder(
            builder: (context) {
              observed = MediaQuery.of(context);
              return const SafeArea(
                child: Center(
                  child: SizedBox.square(
                    dimension: 200,
                    key: ValueKey('board'),
                  ),
                ),
              );
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(observed!.viewPadding, const EdgeInsets.fromLTRB(59, 21, 0, 21));
      expect(tester.getCenter(find.byKey(const ValueKey('board'))).dy, 393 / 2);
      side = 'left';
      compensate = true;
      tester.binding.handleMetricsChanged();
      await tester.pumpAndSettle();
      expect(observed!.viewPadding, const EdgeInsets.fromLTRB(0, 21, 59, 21));
      expect(tester.getCenter(find.byKey(const ValueKey('board'))).dy, 393 / 2);
      compensate = false;
      side = '';
      tester.view.physicalSize = const Size(393, 852);
      await tester.pump();
      tester.view.physicalSize = const Size(852, 393);
      await tester.pump();
      expect(observed!.viewPadding, const EdgeInsets.fromLTRB(59, 21, 59, 21));
      side = 'right';
      tester.binding.handleMetricsChanged();
      await tester.pump();
      expect(observed!.viewPadding, const EdgeInsets.fromLTRB(59, 21, 0, 21));
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'uses the live viewport without scaling and only settles inset jitter',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(832, 384);
      addTearDown(tester.view.reset);
      var media = const MediaQueryData(
        size: Size(832, 384),
        padding: EdgeInsets.fromLTRB(34, 0, 48, 0),
        viewPadding: EdgeInsets.fromLTRB(34, 0, 48, 0),
      );
      final mediaNotifier = ValueNotifier(media);
      addTearDown(mediaNotifier.dispose);
      MediaQueryData? observed;
      await tester.pumpWidget(
        MaterialApp(
          home: ValueListenableBuilder<MediaQueryData>(
            valueListenable: mediaNotifier,
            builder: (context, data, _) => MediaQuery(
              data: data,
              child: OrientationWorkspace(
                sideReader: () async => DeviceLandscapeSide.right,
                child: Builder(
                  builder: (context) {
                    observed = MediaQuery.of(context);
                    return const Center(
                      child: SizedBox.square(
                        key: ValueKey('board'),
                        dimension: 300,
                        child: ColoredBox(color: Colors.amber),
                      ),
                    );
                  },
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pump();
      const intermediateLandscape = Size(650, 500);
      mediaNotifier.value = media.copyWith(size: intermediateLandscape);
      tester.view.physicalSize = intermediateLandscape;
      await tester.pump(const Duration(milliseconds: 40));
      expect(observed!.size, intermediateLandscape);
      final landscapeBoard = tester.getRect(
        find.byKey(const ValueKey('board')),
      );
      expect(landscapeBoard.size, const Size.square(300));
      expect(landscapeBoard.center, intermediateLandscape.center(Offset.zero));

      const firstPortrait = Size(450, 700);
      mediaNotifier.value = media.copyWith(size: firstPortrait);
      tester.view.physicalSize = firstPortrait;
      await tester.pump();
      expect(observed!.size, firstPortrait);
      final board = tester.getRect(find.byKey(const ValueKey('board')));
      expect(board.width, closeTo(board.height, 0.001));
      expect(tester.takeException(), isNull);
      media = media.copyWith(
        size: firstPortrait,
        padding: const EdgeInsets.fromLTRB(0, 28, 0, 24),
        viewPadding: const EdgeInsets.fromLTRB(0, 28, 0, 24),
      );
      mediaNotifier.value = media;
      tester.view.physicalSize = media.size;
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 99));
      expect(observed!.viewPadding, isNot(media.viewPadding));
      await tester.pump(const Duration(milliseconds: 1));
      expect(observed!.size, media.size);
      expect(observed!.viewPadding, media.viewPadding);
      await tester.pumpAndSettle();
      expect(tester.hasRunningAnimations, isFalse);
    },
  );

  testWidgets('one finite content turn starts in final shell geometry', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(832, 384);
    addTearDown(tester.view.reset);
    var side = DeviceLandscapeSide.right;
    OrientationWorkspaceScope? scope;
    EdgeInsets? resolvedInsets;
    final reduceMotion = ValueNotifier(false);
    addTearDown(reduceMotion.dispose);
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => ValueListenableBuilder(
          valueListenable: reduceMotion,
          builder: (context, disabled, _) => MediaQuery(
            data: MediaQuery.of(context).copyWith(disableAnimations: disabled),
            child: child!,
          ),
        ),
        home: OrientationWorkspace(
          sideReader: () async => side,
          child: Builder(
            builder: (context) {
              scope = OrientationWorkspaceScope.maybeOf(context);
              resolvedInsets = MediaQuery.viewPaddingOf(context);
              return const Row(
                children: [
                  SizedBox(width: 200, height: 300, key: ValueKey('shell')),
                  OrientationContentTurn(
                    child: SizedBox.square(
                      dimension: 300,
                      key: ValueKey('canvas'),
                    ),
                  ),
                ],
              );
            },
          ),
        ),
      ),
    );
    await tester.pump();
    final shell = tester.getRect(find.byKey(const ValueKey('shell')));
    side = DeviceLandscapeSide.left;
    tester.view.viewPadding = const FakeViewPadding(right: 24);
    tester.binding.handleMetricsChanged();
    await tester.pump();
    expect(scope!.side, DeviceLandscapeSide.left);
    expect(scope!.turn.value, 0);
    expect(resolvedInsets, const EdgeInsets.only(right: 24));
    await tester.pump(const Duration(milliseconds: 110));
    expect(scope!.turn.value, closeTo(0.5, 0.01));
    expect(tester.getRect(find.byKey(const ValueKey('shell'))), shell);
    tester.binding.handleMetricsChanged();
    await tester.pump();
    expect(scope!.turn.value, closeTo(0.5, 0.01));
    await tester.pumpAndSettle();
    expect(scope!.side, DeviceLandscapeSide.left);
    expect(scope!.turn.value, 1);
    expect(tester.hasRunningAnimations, isFalse);
    final matrix = tester
        .widget<Transform>(
          find.descendant(
            of: find.byType(OrientationContentTurn),
            matching: find.byType(Transform),
          ),
        )
        .transform;
    expect(matrix.isIdentity(), isTrue);

    reduceMotion.value = true;
    await tester.pump();
    side = DeviceLandscapeSide.right;
    tester.view.viewPadding = const FakeViewPadding(left: 24);
    await tester.pump();
    expect(scope!.side, DeviceLandscapeSide.right);
    expect(scope!.turn.value, 1);
    expect(resolvedInsets, const EdgeInsets.only(left: 24));
  });

  testWidgets(
    'rapid side reversal cancels obsolete completion and stale reads',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(832, 384);
      addTearDown(tester.view.reset);
      var side = DeviceLandscapeSide.right;
      Completer<DeviceLandscapeSide?>? delayed;
      OrientationWorkspaceScope? scope;
      await tester.pumpWidget(
        MaterialApp(
          home: OrientationWorkspace(
            sideReader: () => delayed?.future ?? Future.value(side),
            child: Builder(
              builder: (context) {
                scope = OrientationWorkspaceScope.maybeOf(context);
                return const SizedBox.expand();
              },
            ),
          ),
        ),
      );
      await tester.pump();
      side = DeviceLandscapeSide.left;
      tester.binding.handleMetricsChanged();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      side = DeviceLandscapeSide.right;
      tester.binding.handleMetricsChanged();
      await tester.pumpAndSettle();
      expect(scope!.side, DeviceLandscapeSide.right);
      expect(scope!.turn.value, 1);
      delayed = Completer<DeviceLandscapeSide?>();
      tester.binding.handleMetricsChanged();
      final stale = delayed;
      delayed = null;
      tester.binding.handleMetricsChanged();
      await tester.pump();
      stale.complete(DeviceLandscapeSide.left);
      await tester.pumpAndSettle();
      expect(scope!.side, DeviceLandscapeSide.right);
      expect(tester.hasRunningAnimations, isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets('portrait interrupts a landscape half-turn immediately', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(896, 414);
    addTearDown(tester.view.reset);
    String? side = 'right';
    var compensate = false;
    const channel = MethodChannel(
      'com.foxieproducts.litch/interface_orientation',
    );
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'orientationState'
          ? {'side': side, 'compensateHalfTurn': compensate}
          : null,
    );
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        channel,
        null,
      ),
    );
    final media = ValueNotifier(const MediaQueryData(size: Size(896, 414)));
    addTearDown(media.dispose);
    OrientationWorkspaceScope? scope;
    MediaQueryData? observed;
    await tester.pumpWidget(
      MaterialApp(
        home: ValueListenableBuilder<MediaQueryData>(
          valueListenable: media,
          builder: (context, data, _) => MediaQuery(
            data: data,
            child: OrientationWorkspace(
              child: Builder(
                builder: (context) {
                  scope = OrientationWorkspaceScope.maybeOf(context);
                  observed = MediaQuery.of(context);
                  return const OrientationContentTurn(child: SizedBox.expand());
                },
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pump();
    side = 'left';
    compensate = true;
    tester.binding.handleMetricsChanged();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 60));
    expect(scope!.turn.value, inExclusiveRange(0, 1));

    side = null;
    compensate = false;
    media.value = const MediaQueryData(
      size: Size(414, 896),
      padding: EdgeInsets.fromLTRB(0, 44, 0, 34),
      viewPadding: EdgeInsets.fromLTRB(0, 44, 0, 34),
    );
    tester.view.physicalSize = media.value.size;
    await tester.pump();
    expect(observed!.size, const Size(414, 896));
    expect(
      scope!.turn.value,
      1,
      reason: 'Portrait must not inherit the unfinished landscape turn.',
    );
    final matrix = tester
        .widget<Transform>(
          find.descendant(
            of: find.byType(OrientationContentTurn),
            matching: find.byType(Transform),
          ),
        )
        .transform;
    expect(matrix.isIdentity(), isTrue);
    // A later native callback can still describe the previous landscape scene.
    // Portrait geometry remains authoritative even for a fresh async reply.
    side = 'left';
    compensate = true;
    tester.binding.handleMetricsChanged();
    await tester.pump();
    expect(
      tester.widget<RotatedBox>(find.byType(RotatedBox)).quarterTurns,
      0,
      reason:
          'Stale native landscape compensation cannot turn portrait upside down.',
    );
    await tester.pump(const Duration(milliseconds: 220));
    expect(scope!.turn.value, 1);
    expect(tester.hasRunningAnimations, isFalse);
    // The first valid pose after portrait should not animate another
    // half-turn from a side remembered before portrait.
    compensate = false;
    side = null;
    media.value = const MediaQueryData(size: Size(896, 414));
    tester.view.physicalSize = media.value.size;
    await tester.pump();
    side = 'right';
    tester.binding.handleMetricsChanged();
    await tester.pump();
    expect(scope!.side, DeviceLandscapeSide.right);
    expect(scope!.turn.value, 1);
    expect(tester.takeException(), isNull);
  });
}
