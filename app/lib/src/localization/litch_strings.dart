import 'package:flutter/widgets.dart';

import '../../l10n/generated/app_localizations.dart';

/// Standalone screen hosts may omit the app delegate; use the same English
/// catalog there rather than maintaining a second set of presentation strings.
AppLocalizations litchStrings(BuildContext context) =>
    Localizations.of<AppLocalizations>(context, AppLocalizations) ??
    lookupAppLocalizations(const Locale('en'));

/// Alphabetical order for the app's current Latin-script labels. Preserve the
/// displayed text as a deterministic tie-breaker after folding accents/ligatures.
int compareLitchLabels(String left, String right) {
  final comparison = _labelSortKey(left).compareTo(_labelSortKey(right));
  return comparison != 0
      ? comparison
      : left.toLowerCase().compareTo(right.toLowerCase());
}

String _labelSortKey(String label) =>
    label.toLowerCase().split('').map((letter) {
      return switch (letter) {
        'à' || 'á' || 'â' || 'ä' || 'ã' || 'å' => 'a',
        'æ' => 'ae',
        'ç' => 'c',
        'è' || 'é' || 'ê' || 'ë' => 'e',
        'ì' || 'í' || 'î' || 'ï' => 'i',
        'ñ' => 'n~',
        'ò' || 'ó' || 'ô' || 'ö' || 'õ' => 'o',
        'œ' => 'oe',
        'ß' => 'ss',
        'ù' || 'ú' || 'û' || 'ü' => 'u',
        'ý' || 'ÿ' => 'y',
        _ => letter,
      };
    }).join();
