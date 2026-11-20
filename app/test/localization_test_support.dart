import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'health_render_capture.dart';
import 'support/pseudo_localizations.dart';

const localizationReviewLocales = <Locale>[
  ...litchSupportedLocales,
  litchPseudoLocale,
];

AppLocalizations reviewStrings(Locale locale) => locale == litchPseudoLocale
    ? PseudoLocalizations()
    : lookupAppLocalizations(locale);

Widget localizationHarness({
  required Locale locale,
  required Widget home,
  double scale = 1,
}) => MaterialApp(
  debugShowCheckedModeBanner: false,
  locale: locale,
  localizationsDelegates: const [
    PseudoLocalizationsDelegate(),
    ...AppLocalizations.localizationsDelegates,
  ],
  supportedLocales: localizationReviewLocales,
  theme: buildLitchTheme(),
  builder: (context, child) => RepaintBoundary(
    child: MediaQuery(
      data: MediaQuery.of(
        context,
      ).copyWith(textScaler: TextScaler.linear(scale)),
      child: child!,
    ),
  ),
  home: home,
);

Future<void> prepareLocalizationReview(WidgetTester tester) async {
  await loadHealthReviewFonts(tester, useSdkFonts: true);
  tester.view.physicalSize = const Size(393, 852);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

Future<void> decodeVisibleReviewImages(WidgetTester tester) async {
  final images = tester.widgetList<Image>(find.byType(Image)).toList();
  final context = tester.element(find.byType(MaterialApp));
  await tester.runAsync(() async {
    for (final image in images) {
      await precacheImage(image.image, context);
    }
  });
  await tester.pump();
  await tester.pumpAndSettle();
}

// Measure actual production text at its laid-out width, not merely its full
// semantic label: semantics preserved the words in the escaped clipping cases.
void expectReviewTextFits(WidgetTester tester, Finder text, String reason) {
  final richText = find.descendant(of: text, matching: find.byType(RichText));
  expect(richText, findsOneWidget, reason: reason);
  final paragraph = tester.renderObject<RenderParagraph>(richText);
  expect(paragraph.didExceedMaxLines, isFalse, reason: reason);
  final fullText = TextPainter(
    text: paragraph.text,
    textDirection: paragraph.textDirection,
    textScaler: paragraph.textScaler,
  )..layout(maxWidth: paragraph.size.width);
  expect(
    paragraph.size.height,
    greaterThanOrEqualTo(fullText.height - .1),
    reason: reason,
  );
  fullText.dispose();
}
