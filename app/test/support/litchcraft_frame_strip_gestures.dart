import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

Future<void> settleCraftFilmstripScroll(WidgetTester tester) async {
  final viewport = find.byKey(const ValueKey('craft-frame-scroll-viewport'));
  final position = tester
      .state<ScrollableState>(
        find.descendant(of: viewport, matching: find.byType(Scrollable)),
      )
      .position;
  // A long iOS strip can coast beyond the one-second short-strip bound.
  // Wait only for its ballistic scroll, with a strict three-second ceiling.
  for (
    var frame = 0;
    frame < 60 && position.isScrollingNotifier.value;
    frame++
  ) {
    await tester.pump(const Duration(milliseconds: 50));
  }
  expect(
    position.isScrollingNotifier.value,
    isFalse,
    reason:
        'Wait for this real scroll, not unrelated animated canvas ticks. '
        'offset=${position.pixels} range=${position.minScrollExtent}..${position.maxScrollExtent}',
  );
  await tester.pump();
}

Future<Finder> revealCraftFilmstripFrame(
  WidgetTester tester, {
  required int index,
  required int frameCount,
  required int Function() selectedFrame,
}) async {
  final viewport = find.byKey(const ValueKey('craft-frame-scroll-viewport'));
  final beforeScroll = selectedFrame();
  final target = find.byKey(ValueKey('craft-select-frame-$index'));
  // Gesture only the filmstrip. ensureVisible would also scroll ancestor
  // workspace pagers, which is not how a finger browses this control.
  for (
    var swipe = 0;
    swipe < frameCount + 2 && target.hitTestable().evaluate().isEmpty;
    swipe++
  ) {
    await tester.timedDrag(
      viewport,
      Offset(index < beforeScroll ? 53 : -53, 0),
      const Duration(milliseconds: 150),
    );
    await settleCraftFilmstripScroll(tester);
  }
  expect(
    selectedFrame(),
    beforeScroll,
    reason: 'Browsing the actual lazy filmstrip never selects a frame.',
  );
  expect(target.hitTestable(), findsOneWidget);
  return target;
}
