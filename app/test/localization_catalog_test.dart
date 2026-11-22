import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';

void main() {
  test(
    'every supported catalog owns every key and preserves ICU arguments',
    () {
      Map<String, dynamic> read(String locale) =>
          jsonDecode(File('lib/l10n/app_$locale.arb').readAsStringSync())
              as Map<String, dynamic>;
      final english = read('en');
      final keys = english.keys.where((key) => !key.startsWith('@')).toSet();
      Set<String> arguments(String text) {
        final result = <String>{};
        // ICU alternates argument braces and select/plural branch braces.
        // A one-word branch body, such as other{off}, is literal copy.
        final argumentScopes = <bool>[];
        for (var index = 0; index < text.length; index++) {
          if (text[index] == '{') {
            final isArgument = argumentScopes.isEmpty || !argumentScopes.last;
            if (isArgument) {
              final match = RegExp(
                r'\{(\w+)(?:\}|,)',
              ).matchAsPrefix(text, index);
              if (match != null) result.add(match.group(1)!);
            }
            argumentScopes.add(isArgument);
          } else if (text[index] == '}' && argumentScopes.isNotEmpty) {
            argumentScopes.removeLast();
          }
        }
        return result;
      }

      expect(arguments('{state, select, on{on} other{off}}'), {'state'});
      expect(
        arguments(
          'P{player}: {count, plural, one{{item}} other{{count} items}}',
        ),
        {'player', 'count', 'item'},
      );
      for (final locale in litchSupportedLocales) {
        final catalog = read(locale.toString());
        expect(
          catalog.keys.where((key) => !key.startsWith('@')).toSet(),
          keys,
          reason: '$locale must not silently inherit English',
        );
        for (final key in keys) {
          final value = catalog[key] as String;
          expect(value.trim(), isNotEmpty, reason: '$locale $key');
          expect(
            arguments(value),
            arguments(english[key] as String),
            reason: '$locale $key preserves whole-message arguments',
          );
          if ((english[key] as String).contains(', plural,')) {
            expect(value, contains(', plural,'), reason: '$locale $key');
            expect(value, contains('other{'), reason: '$locale $key');
          }
        }
      }
    },
  );

  test(
    'Manual catalogs give current product instructions without bring-up copy',
    () {
      final technicalNames = RegExp(
        r'\b(?:S3|ESP32[\w-]*|V[123](?:\.\d+)?)\b',
        caseSensitive: false,
      );
      for (final locale in litchSupportedLocales) {
        final catalog =
            jsonDecode(File('lib/l10n/app_$locale.arb').readAsStringSync())
                as Map<String, dynamic>;
        for (final key in <String>[
          'manualStartBody',
          'manualControlsBody',
          'manualLitchcraftBody',
          'manualClockBody',
          'manualRecoveryBody',
          'manualFactoryResetBody',
        ]) {
          expect(
            catalog[key],
            isNot(contains(technicalNames)),
            reason: '$locale $key',
          );
        }
        expect(catalog.containsKey('manualFactoryTestBody'), isFalse);
        expect(
          catalog['manualLitchcraftBody'],
          contains('$litchcraftAnimationMaximumFrameCount'),
          reason: '$locale must state the supported document frame limit',
        );
        expect(
          catalog['craftFrameLimit'],
          contains('$litchcraftAnimationMaximumFrameCount'),
        );
        expect(catalog['manualClockBody'], contains('3+2'));
        expect(catalog['manualClockBody'], contains('15+10'));
        expect(catalog['craftSquareSemantics'], '{square}, {state}');
      }
      final english = lookupAppLocalizations(const Locale('en'));
      expect(
        english.manualControlsBody,
        contains('Hold X for about one second to open Quick Setup'),
      );
      expect(
        english.manualStartBody,
        contains('hold X for about one second to open Quick Setup'),
      );
      for (final body in [
        english.manualControlsBody,
        english.manualStartBody,
      ]) {
        expect(
          body,
          isNot(contains(RegExp(r'\bcent(?:er|re)\b', caseSensitive: false))),
        );
      }
      expect(english.craftControlsHelp, contains('tap Fill to fill'));
      expect(english.craftControlsHelp, contains('hold the same action'));
      expect(english.craftControlsHelp, contains('to clear it'));
      expect(english.manualLitchcraftBody, contains('Tap Fill to fill'));
      expect(english.manualLitchcraftBody, contains('half a second'));
      expect(
        english.manualLitchcraftBody,
        contains('to clear the frame instead'),
      );
      expect(
        english.manualLitchcraftBody,
        contains('Hold a color swatch for half a second'),
      );
      expect(english.craftControlsHelp, isNot(contains('Hold Paint or Erase')));
    },
  );

  test(
    'About prose is translated while names and license bodies remain content',
    () {
      final english = lookupAppLocalizations(const Locale('en'));
      for (final locale in litchSupportedLocales) {
        final strings = lookupAppLocalizations(locale);
        expect(strings.aboutOpenSourceBody, contains('Flutter'));
        expect(strings.aboutOpenSourceBody, contains('Reactive BLE'));
        if (locale.languageCode != 'en') {
          expect(
            strings.aboutOpenSourceBody,
            isNot(english.aboutOpenSourceBody),
          );
          expect(strings.aboutThanksBody, isNot(english.aboutThanksBody));
          expect(strings.aboutLicenseNotice, isNot(english.aboutLicenseNotice));
        }
      }
    },
  );

  test('material event copy is grammatical for rook, queen, and promotion', () {
    final expected = <Locale, (String, String, String)>{
      const Locale('en'): (
        'P1 (W) captured a queen on d8.',
        'P1 (W) captured a rook on a1.',
        'P1 (W) promoted a pawn to a queen.',
      ),
      const Locale('en', 'GB'): (
        'P1 (W) captured a queen on d8.',
        'P1 (W) captured a rook on a1.',
        'P1 (W) promoted a pawn to a queen.',
      ),
      const Locale('de'): (
        'P1 (W): Dame auf d8 geschlagen.',
        'P1 (W): Turm auf a1 geschlagen.',
        'P1 (W): Bauernumwandlung zu Dame.',
      ),
      const Locale('es'): (
        'P1 (W): captura de dama en d8.',
        'P1 (W): captura de torre en a1.',
        'P1 (W): promoción de peón a dama.',
      ),
      const Locale('fr'): (
        'P1 (W) : capture de dame en d8.',
        'P1 (W) : capture de tour en a1.',
        'P1 (W) : promotion d’un pion en dame.',
      ),
      const Locale('it'): (
        'P1 (W): cattura di donna in d8.',
        'P1 (W): cattura di torre in a1.',
        'P1 (W): promozione di un pedone a donna.',
      ),
    };

    for (final MapEntry(:key, :value) in expected.entries) {
      final strings = lookupAppLocalizations(key);
      expect(
        strings.reviewMaterialCapture(
          strings.reviewWhite,
          strings.reviewPieceQueen,
          'd8',
        ),
        value.$1,
      );
      expect(
        strings.reviewMaterialCapture(
          strings.reviewWhite,
          strings.reviewPieceRook,
          'a1',
        ),
        value.$2,
      );
      expect(
        strings.reviewMaterialPromotion(
          strings.reviewWhite,
          strings.reviewPieceQueen,
        ),
        value.$3,
      );
    }
  });

  test('Spanish and French half-move copy names both reset events', () {
    expect(
      lookupAppLocalizations(const Locale('es')).fenPreviewHalfmoveClock(7),
      'Medios movimientos: 7 desde un movimiento de peón o una captura',
    );
    expect(
      lookupAppLocalizations(const Locale('fr')).fenPreviewHalfmoveClock(7),
      'Demi-coups : 7 depuis un mouvement de pion ou une prise',
    );
  });
}
