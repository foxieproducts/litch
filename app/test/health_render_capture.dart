import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;
import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

// Optional review artifacts, never golden approval or a test expectation.
Future<void> captureHealthReview(
  WidgetTester tester,
  String name, {
  Finder? boundaryFinder,
}) async {
  final String? directory = Platform.environment['LITCH_HEALTH_CAPTURE_DIR'];
  if (directory == null) return;
  final RenderRepaintBoundary boundary = tester
      .renderObject<RenderRepaintBoundary>(
        boundaryFinder ?? find.byType(RepaintBoundary).first,
      );
  await tester.runAsync(() async {
    final ui.Image image = await boundary.toImage(pixelRatio: 1);
    final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
    final File file = File('$directory/$name.png');
    await file.parent.create(recursive: true);
    await file.writeAsBytes(bytes!.buffer.asUint8List());
    image.dispose();
  });
}

Future<void> loadHealthReviewFonts(
  WidgetTester tester, {
  bool useSdkFonts = false,
}) async {
  String? directory = Platform.environment['LITCH_HEALTH_FONT_DIR'];
  if (directory == null && !useSdkFonts) return;
  await tester.runAsync(() async {
    if (directory == null) {
      // Resolve the SDK used by this test, not a machine-specific installation.
      // Real glyph metrics are required for localized truncation assertions.
      final packageConfig = File('.dart_tool/package_config.json');
      final config =
          jsonDecode(await packageConfig.readAsString())
              as Map<String, dynamic>;
      final flutterPackage = (config['packages'] as List<dynamic>)
          .cast<Map<String, dynamic>>()
          .singleWhere((package) => package['name'] == 'flutter');
      final sdk = Directory.fromUri(
        packageConfig.absolute.uri.resolve(flutterPackage['rootUri'] as String),
      ).parent.parent;
      directory = '${sdk.path}/bin/cache/artifacts/material_fonts';
    }
    // Ordinary opted-in review tests use a portable layout proxy. Only an
    // explicit native font supplies native iOS metrics; never fall back to
    // Flutter's square glyphs for an unresolved Cupertino font family.
    final String cupertinoFont =
        Platform.environment['LITCH_HEALTH_CUPERTINO_FONT'] ??
        '$directory/Roboto-Regular.ttf';
    for (final family in <String>[
      'CupertinoSystemText',
      'CupertinoSystemDisplay',
    ]) {
      final loader = FontLoader(family)
        ..addFont(
          File(
            cupertinoFont,
          ).readAsBytes().then((bytes) => bytes.buffer.asByteData()),
        );
      await loader.load();
    }
    for (final String family in <String>['Roboto', 'Ahem', 'MaterialIcons']) {
      final String file = family == 'MaterialIcons'
          ? 'MaterialIcons-Regular.otf'
          : 'Roboto-Regular.ttf';
      final FontLoader loader = FontLoader(family)
        ..addFont(
          File(
            '$directory/$file',
          ).readAsBytes().then((bytes) => bytes.buffer.asByteData()),
        );
      await loader.load();
    }
  });
}
