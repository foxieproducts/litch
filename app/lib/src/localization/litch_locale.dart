import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

@immutable
class LitchLocalePreference {
  const LitchLocalePreference.system() : locale = null;

  const LitchLocalePreference.fixed(Locale this.locale);

  final Locale? locale;

  bool get followsSystem => locale == null;

  String get storageValue => locale?.toLanguageTag() ?? 'system';

  static LitchLocalePreference fromStorageValue(
    String? value, {
    required Iterable<Locale> supportedLocales,
  }) {
    if (value == null || value == 'system') {
      return const LitchLocalePreference.system();
    }
    for (final Locale locale in supportedLocales) {
      if (locale.toLanguageTag() == value &&
          isUserSelectableLitchLocale(locale)) {
        return LitchLocalePreference.fixed(locale);
      }
    }
    return const LitchLocalePreference.system();
  }

  @override
  bool operator ==(Object other) =>
      other is LitchLocalePreference && other.locale == locale;

  @override
  int get hashCode => locale.hashCode;
}

const Locale litchEnglishLocale = Locale('en');
const Locale litchBritishEnglishLocale = Locale('en', 'GB');
const Locale litchSpanishLocale = Locale('es');
const Locale litchGermanLocale = Locale('de');
const Locale litchItalianLocale = Locale('it');
const Locale litchFrenchLocale = Locale('fr');
const Locale litchPseudoLocale = Locale('en', 'XA');

const List<Locale> litchUserSelectableLocales = <Locale>[
  litchEnglishLocale,
  litchBritishEnglishLocale,
  litchSpanishLocale,
  litchGermanLocale,
  litchItalianLocale,
  litchFrenchLocale,
];

const List<Locale> litchSupportedLocales = <Locale>[
  ...litchUserSelectableLocales,
];

bool isUserSelectableLitchLocale(Locale locale) =>
    litchUserSelectableLocales.contains(locale);

abstract final class LitchLocalePreferences {
  static const String preferenceKey = 'app_locale_mode';

  static Future<LitchLocalePreference> load({
    required Iterable<Locale> supportedLocales,
  }) async {
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    return LitchLocalePreference.fromStorageValue(
      preferences.getString(preferenceKey),
      supportedLocales: supportedLocales,
    );
  }

  static Future<void> save(LitchLocalePreference preference) async {
    final Locale? locale = preference.locale;
    if (locale != null && !isUserSelectableLitchLocale(locale)) {
      throw ArgumentError.value(
        locale,
        'preference',
        'Unsupported or test-only locales cannot be persisted.',
      );
    }
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    if (!await preferences.setString(preferenceKey, preference.storageValue)) {
      throw StateError('App language preference could not be saved.');
    }
  }
}

Locale resolveLitchLocale(
  List<Locale>? preferredLocales,
  Iterable<Locale> supportedLocales,
) {
  final List<Locale> supported = supportedLocales.toList(growable: false);
  final Locale fallback = supported.firstWhere(
    (Locale locale) => locale.languageCode == 'en',
    orElse: () => litchEnglishLocale,
  );
  if (preferredLocales == null || preferredLocales.isEmpty) {
    return fallback;
  }

  for (final Locale preferred in preferredLocales) {
    for (final Locale locale in supported) {
      if (locale.toLanguageTag() == preferred.toLanguageTag()) {
        return locale;
      }
    }
    if (preferred.scriptCode case final String preferredScript) {
      for (final Locale locale in supported) {
        if (locale.languageCode == preferred.languageCode &&
            locale.scriptCode == preferredScript) {
          return locale;
        }
      }
    }
    if (preferred.countryCode case final String preferredCountry) {
      for (final Locale locale in supported) {
        if (locale.languageCode == preferred.languageCode &&
            locale.countryCode == preferredCountry) {
          return locale;
        }
      }
    }
    for (final Locale locale in supported) {
      if (locale.languageCode == preferred.languageCode &&
          locale.scriptCode == null &&
          locale.countryCode == null) {
        return locale;
      }
    }
    for (final Locale locale in supported) {
      if (locale.languageCode == preferred.languageCode) {
        return locale;
      }
    }
  }
  return fallback;
}
