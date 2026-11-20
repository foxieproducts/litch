import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';

void main() {
  test(
    'all six locales include native Bluetooth and motion purpose strings',
    () {
      final project = File(
        'ios/Runner.xcodeproj/project.pbxproj',
      ).readAsStringSync();
      expect(project, contains('InfoPlist.strings in Resources'));
      expect(project, contains('isa = PBXVariantGroup;'));
      for (final locale in litchUserSelectableLocales) {
        final tag = locale.toLanguageTag();
        final path = '$tag.lproj/InfoPlist.strings';
        expect(project, contains(path));
        final source = File('ios/Runner/$path').readAsStringSync();
        for (final key in [
          'NSBluetoothAlwaysUsageDescription',
          'NSBluetoothPeripheralUsageDescription',
          'NSMotionUsageDescription',
        ]) {
          expect(
            RegExp('"$key" = "[^"\\n]+";').allMatches(source),
            hasLength(1),
          );
        }
      }
    },
  );

  test('iOS bundle registers exactly the selectable Flutter languages', () {
    final expected = litchUserSelectableLocales
        .map((locale) => locale.toLanguageTag())
        .toList();
    final plist = File('ios/Runner/Info.plist').readAsStringSync();
    final declaration = RegExp(
      r'<key>CFBundleLocalizations</key>\s*<array>([\s\S]*?)</array>',
    ).firstMatch(plist);
    expect(
      declaration,
      isNotNull,
      reason: 'Flutter locales need native registration',
    );
    final declared = RegExp(r'<string>([^<]+)</string>')
        .allMatches(declaration!.group(1)!)
        .map((match) => match.group(1)!)
        .toList();
    expect(declared, unorderedEquals(expected));
    expect(declared, isNot(contains(litchPseudoLocale.toLanguageTag())));

    final project = File(
      'ios/Runner.xcodeproj/project.pbxproj',
    ).readAsStringSync();
    final regions = RegExp(
      r'knownRegions = \(([\s\S]*?)\);',
    ).firstMatch(project);
    expect(regions, isNotNull);
    final regionNames = regions!
        .group(1)!
        .split(',')
        .map((value) => value.trim().replaceAll('"', ''))
        .where((value) => value.isNotEmpty)
        .toList();
    expect(regionNames, unorderedEquals(<String>['Base', ...expected]));
    expect(project, contains('developmentRegion = en;'));
  });

  test('iOS launch and Flutter host use an adaptive Litch background', () {
    for (final String storyboard in <String>[
      'ios/Runner/Base.lproj/LaunchScreen.storyboard',
      'ios/Runner/Base.lproj/Main.storyboard',
    ]) {
      final String source = File(storyboard).readAsStringSync();
      expect(source, contains('backgroundColor" name="LaunchBackground"'));
    }

    final Object? decoded = jsonDecode(
      File(
        'ios/Runner/Assets.xcassets/LaunchBackground.colorset/Contents.json',
      ).readAsStringSync(),
    );
    expect(decoded, isA<Map<String, Object?>>());
    final List<Object?> colors =
        (decoded! as Map<String, Object?>)['colors']! as List<Object?>;
    expect(colors, hasLength(2));
    final Map<String, Object?> dark = colors.last! as Map<String, Object?>;
    expect(dark['appearances'], isNotNull);
  });
}
