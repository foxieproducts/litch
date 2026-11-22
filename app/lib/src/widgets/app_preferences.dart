import 'dart:async';

import 'package:flutter/material.dart';

import '../../l10n/generated/app_localizations.dart';
import '../localization/litch_locale.dart';
import '../localization/litch_strings.dart';
import '../services/app_litchcraft_preferences.dart';
import '../theme/litch_appearance.dart';
import 'settings_ui.dart';
import 'compact_controls.dart';
import 'reading_pane.dart';

/// The app root owns these preferences. Placing this above the Navigator keeps
/// pushed settings routes live without recreating the gate or BLE session.
class AppPreferencesScope extends InheritedWidget {
  const AppPreferencesScope({
    super.key,
    required this.appearanceMode,
    required this.onAppearanceModeChanged,
    required this.localePreference,
    required this.onLocalePreferenceChanged,
    this.showTips = true,
    this.onShowTipsChanged,
    this.litchcraftStorageLimitBytes =
        AppLitchcraftPreferences.defaultStorageLimitBytes,
    this.onLitchcraftStorageLimitChanged,
    required super.child,
  });

  final LitchAppearanceMode appearanceMode;
  final Future<void> Function(LitchAppearanceMode) onAppearanceModeChanged;
  final LitchLocalePreference localePreference;
  final Future<void> Function(LitchLocalePreference) onLocalePreferenceChanged;
  final bool showTips;
  final Future<void> Function(bool)? onShowTipsChanged;
  final int litchcraftStorageLimitBytes;
  final Future<void> Function(int)? onLitchcraftStorageLimitChanged;

  static AppPreferencesScope? maybeOf(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<AppPreferencesScope>();

  @override
  bool updateShouldNotify(AppPreferencesScope oldWidget) =>
      appearanceMode != oldWidget.appearanceMode ||
      localePreference != oldWidget.localePreference ||
      showTips != oldWidget.showTips ||
      litchcraftStorageLimitBytes != oldWidget.litchcraftStorageLimitBytes ||
      onAppearanceModeChanged != oldWidget.onAppearanceModeChanged ||
      onLocalePreferenceChanged != oldWidget.onLocalePreferenceChanged ||
      onShowTipsChanged != oldWidget.onShowTipsChanged ||
      onLitchcraftStorageLimitChanged !=
          oldWidget.onLitchcraftStorageLimitChanged;
}

class AppPreferences extends StatelessWidget {
  const AppPreferences({
    super.key,
    this.appearanceMode = LitchAppearanceMode.system,
    this.onAppearanceModeChanged,
    this.onNotice,
  });

  // Preserve the existing explicit Appearance boundary for standalone hosts.
  // Production settings hosts consume the same live root scope.
  final LitchAppearanceMode appearanceMode;
  final Future<void> Function(LitchAppearanceMode)? onAppearanceModeChanged;
  final ValueChanged<SnackBar>? onNotice;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final preferences = AppPreferencesScope.maybeOf(context);
    final effectiveAppearance = preferences?.appearanceMode ?? appearanceMode;
    final changeAppearance =
        preferences?.onAppearanceModeChanged ?? onAppearanceModeChanged;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: <Widget>[
        SettingsSectionHeading(
          title: strings.appearanceTitle,
          description: strings.appearanceDescription,
        ),
        const SizedBox(height: 8),
        Card(
          margin: EdgeInsets.zero,
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Semantics(
              container: true,
              label: strings.appAppearance,
              child: SettingsChoicePills<LitchAppearanceMode>(
                key: const ValueKey<String>('app-appearance-mode'),
                optionKey: (mode) =>
                    ValueKey('app-appearance-${mode.storageValue}'),
                segments: [
                  for (final mode in LitchAppearanceMode.values)
                    ButtonSegment(
                      value: mode,
                      label: Text(_appearanceLabel(strings, mode)),
                    ),
                ],
                selected: {effectiveAppearance},
                onSelectionChanged: changeAppearance == null
                    ? null
                    : (values) => unawaited(changeAppearance(values.single)),
              ),
            ),
          ),
        ),
        if (preferences != null) ...<Widget>[
          const SizedBox(height: 20),
          _LanguagePreference(preferences: preferences, onNotice: onNotice),
          const SizedBox(height: 20),
          _TipsPreference(preferences: preferences, onNotice: onNotice),
          const SizedBox(height: 20),
          _LitchcraftStoragePreference(preferences: preferences),
        ],
      ],
    );
  }
}

class _LitchcraftStoragePreference extends StatelessWidget {
  const _LitchcraftStoragePreference({required this.preferences});

  final AppPreferencesScope preferences;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final bytes = AppLitchcraftPreferences.normalizeStorageLimit(
      preferences.litchcraftStorageLimitBytes,
    );
    final megabytes = bytes / 1_000_000;
    final label = megabytes == megabytes.truncateToDouble()
        ? megabytes.toStringAsFixed(0)
        : megabytes.toStringAsFixed(6).replaceFirst(RegExp(r'0+$'), '');
    final change = preferences.onLitchcraftStorageLimitChanged;
    return Card(
      margin: EdgeInsets.zero,
      child: ListTile(
        key: const ValueKey<String>('app-litchcraft-storage-limit'),
        leading: const Icon(Icons.storage_outlined),
        title: Text(strings.litchcraftStorageLimit),
        subtitle: Text(strings.litchcraftStorageLimitValue(label)),
        trailing: const Icon(Icons.edit_outlined),
        enabled: change != null,
        onTap: change == null
            ? null
            : () => showDialog<void>(
                context: context,
                builder: (_) => _ArtworkStorageLimitDialog(
                  initialBytes: bytes,
                  onSave: change,
                ),
              ),
      ),
    );
  }
}

class _ArtworkStorageLimitDialog extends StatefulWidget {
  const _ArtworkStorageLimitDialog({
    required this.initialBytes,
    required this.onSave,
  });

  final int initialBytes;
  final Future<void> Function(int) onSave;

  @override
  State<_ArtworkStorageLimitDialog> createState() =>
      _ArtworkStorageLimitDialogState();
}

class _ArtworkStorageLimitDialogState
    extends State<_ArtworkStorageLimitDialog> {
  late final TextEditingController _controller = TextEditingController(
    text: (widget.initialBytes / 1_000_000).ceil().toString(),
  );
  String? _error;
  bool _saving = false;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    return PopScope(
      canPop: !_saving,
      child: AlertDialog(
        key: const ValueKey<String>('app-litchcraft-storage-dialog'),
        scrollable: true,
        title: Text(strings.litchcraftStorageLimit),
        content: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Text(strings.litchcraftStorageLimitDescription),
            const SizedBox(height: 16),
            TextField(
              key: const ValueKey<String>('app-litchcraft-storage-megabytes'),
              controller: _controller,
              enabled: !_saving,
              keyboardType: TextInputType.number,
              textInputAction: TextInputAction.done,
              decoration: InputDecoration(
                labelText: strings.litchcraftStorageLimitUnit,
              ),
              onSubmitted: (_) => unawaited(_save()),
            ),
            if (_error != null) ...<Widget>[
              const SizedBox(height: 12),
              Semantics(
                liveRegion: true,
                child: Text(
                  _error!,
                  style: TextStyle(color: Theme.of(context).colorScheme.error),
                ),
              ),
            ],
          ],
        ),
        actions: <Widget>[
          TextButton(
            onPressed: _saving ? null : () => Navigator.of(context).pop(),
            child: Text(strings.cancelAction),
          ),
          FilledButton(
            key: const ValueKey<String>('app-litchcraft-storage-save'),
            onPressed: _saving ? null : () => unawaited(_save()),
            child: Text(strings.saveAction),
          ),
        ],
      ),
    );
  }

  Future<void> _save() async {
    if (_saving) return;
    final strings = litchStrings(context);
    final text = _controller.text.trim();
    // Bound parsing before multiplication, including pasted/programmatic input.
    if (text.length > 10) {
      setState(() => _error = strings.litchcraftStorageLimitTooLarge);
      return;
    }
    final megabytes = RegExp(r'^[0-9]+$').hasMatch(text)
        ? int.tryParse(text)
        : null;
    if (megabytes == null || megabytes < 1) {
      setState(() => _error = strings.litchcraftStorageLimitInvalid);
      return;
    }
    if (megabytes >
        AppLitchcraftPreferences.maximumStorageLimitBytes ~/ 1_000_000) {
      setState(() => _error = strings.litchcraftStorageLimitTooLarge);
      return;
    }
    setState(() {
      _saving = true;
      _error = null;
    });
    try {
      await widget.onSave(megabytes * 1_000_000);
      if (mounted) Navigator.of(context).pop();
    } catch (_) {
      if (!mounted) return;
      setState(() {
        _saving = false;
        _error = strings.litchcraftStorageLimitSaveFailed;
      });
    }
  }
}

class _TipsPreference extends StatelessWidget {
  const _TipsPreference({required this.preferences, this.onNotice});

  final AppPreferencesScope preferences;
  final ValueChanged<SnackBar>? onNotice;

  @override
  Widget build(BuildContext context) => Card(
    margin: EdgeInsets.zero,
    child: Padding(
      padding: const EdgeInsets.all(16),
      child: MergeSemantics(
        child: SettingsToggleRow(
          leading: const Icon(Icons.tips_and_updates_outlined),
          title: Text(
            litchStrings(context).showTips,
            style: Theme.of(context).textTheme.bodyLarge,
          ),
          help: Text(
            litchStrings(context).showTipsDescription,
            style: Theme.of(context).textTheme.bodyMedium,
          ),
          control: Switch.adaptive(
            key: const ValueKey<String>('app-show-tips'),
            value: preferences.showTips,
            onChanged: preferences.onShowTipsChanged == null
                ? null
                : (enabled) => unawaited(_change(context, enabled)),
          ),
        ),
      ),
    ),
  );

  Future<void> _change(BuildContext context, bool enabled) async {
    try {
      await preferences.onShowTipsChanged!(enabled);
    } catch (_) {
      if (!context.mounted) return;
      final notice = SnackBar(
        content: Text(litchStrings(context).showTipsSaveFailed),
      );
      if (onNotice != null) {
        onNotice!(notice);
      } else {
        ScaffoldMessenger.of(context).showSnackBar(notice);
      }
    }
  }
}

class AppSettingsScreen extends StatelessWidget {
  const AppSettingsScreen({super.key});

  static MaterialPageRoute<void> route() => MaterialPageRoute<void>(
    settings: const RouteSettings(name: '/app-settings'),
    builder: (_) => const AppSettingsScreen(),
  );

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: LitchCompactAppBar(
      child: AppBar(title: Text(litchStrings(context).settingsTitle)),
    ),
    body: const SafeArea(
      top: false,
      child: SingleChildScrollView(
        padding: EdgeInsets.all(16),
        child: LitchReadingPane(child: AppPreferences()),
      ),
    ),
  );
}

class _LanguagePreference extends StatelessWidget {
  const _LanguagePreference({required this.preferences, this.onNotice});

  final AppPreferencesScope preferences;
  final ValueChanged<SnackBar>? onNotice;

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final label = strings.language == 'Language'
        ? strings.language
        : '${strings.language} · Language';
    final resolvedSystemLocale = resolveLitchLocale(
      WidgetsBinding.instance.platformDispatcher.locales,
      litchSupportedLocales,
    );
    final systemLabel =
        '${strings.languageSystem} (${_autonym(strings, resolvedSystemLocale)})';
    final choices = <LitchLocalePreference, String>{
      const LitchLocalePreference.system(): systemLabel,
      for (final locale in litchUserSelectableLocales)
        LitchLocalePreference.fixed(locale): _autonym(strings, locale),
    }.entries.toList()..sort((a, b) => compareLitchLabels(a.value, b.value));
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: <Widget>[
            Row(
              children: <Widget>[
                const Icon(Icons.translate_rounded),
                const SizedBox(width: 12),
                Expanded(child: Text(label)),
              ],
            ),
            const SizedBox(height: 8),
            Semantics(
              label: label,
              child: DropdownButton<LitchLocalePreference>(
                key: const ValueKey<String>('app-language'),
                value: preferences.localePreference,
                isExpanded: true,
                itemHeight: null,
                menuMaxHeight: 400,
                items: <DropdownMenuItem<LitchLocalePreference>>[
                  for (final choice in choices) _item(choice.key, choice.value),
                ],
                onChanged: (preference) async {
                  if (preference == null) return;
                  try {
                    await preferences.onLocalePreferenceChanged(preference);
                  } catch (_) {
                    if (!context.mounted) return;
                    final notice = SnackBar(
                      // A fast persistence failure can arrive before the
                      // root has rebuilt for the newly selected language.
                      content: Builder(
                        builder: (context) =>
                            Text(litchStrings(context).languageSaveFailed),
                      ),
                    );
                    if (onNotice != null) {
                      onNotice!(notice);
                    } else {
                      ScaffoldMessenger.of(context).showSnackBar(notice);
                    }
                  }
                },
              ),
            ),
          ],
        ),
      ),
    );
  }

  static DropdownMenuItem<LitchLocalePreference> _item(
    LitchLocalePreference preference,
    String label,
  ) => DropdownMenuItem<LitchLocalePreference>(
    value: preference,
    child: ConstrainedBox(
      constraints: const BoxConstraints(minHeight: 48),
      child: Align(
        alignment: Alignment.centerLeft,
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 8),
          child: Text(label),
        ),
      ),
    ),
  );

  static String _autonym(AppLocalizations strings, Locale locale) =>
      switch (locale.toLanguageTag()) {
        'en-GB' => strings.languageBritishEnglish,
        'es' => strings.languageSpanish,
        'de' => strings.languageGerman,
        'it' => strings.languageItalian,
        'fr' => strings.languageFrench,
        _ => strings.languageEnglish,
      };
}

String _appearanceLabel(AppLocalizations strings, LitchAppearanceMode mode) =>
    switch (mode) {
      LitchAppearanceMode.system => strings.languageSystem,
      LitchAppearanceMode.dark => strings.appearanceDark,
      LitchAppearanceMode.light => strings.appearanceLight,
    };
