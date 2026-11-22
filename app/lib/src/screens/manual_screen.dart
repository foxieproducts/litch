import 'package:flutter/cupertino.dart' show CupertinoPageRoute;
import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';

import '../../l10n/generated/app_localizations.dart';
import '../localization/litch_strings.dart';
import '../services/external_link_launcher.dart';
import '../widgets/board_controls_glyph.dart';
import '../widgets/home_chessboard.dart';
import '../widgets/litchcraft_tools.dart';
import '../widgets/reading_pane.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/manual_quick_setup_reference.dart';

class ManualScreen extends StatefulWidget {
  const ManualScreen({super.key, this.initialTopic});

  final String? initialTopic;

  static CupertinoPageRoute<void> route({String? initialTopic}) =>
      CupertinoPageRoute<void>(
        settings: const RouteSettings(name: '/manual'),
        builder: (_) => ManualScreen(initialTopic: initialTopic),
      );

  @override
  State<ManualScreen> createState() => _ManualScreenState();
}

class _ManualScreenState extends State<ManualScreen> {
  final TextEditingController _searchController = TextEditingController();
  final ScrollController _scrollController = ScrollController();
  String _query = '';

  @override
  void dispose() {
    _searchController.dispose();
    _scrollController.dispose();
    super.dispose();
  }

  void _clearSearch() {
    _searchController.clear();
    setState(() => _query = '');
  }

  void _openTopic(String id) {
    FocusScope.of(context).unfocus();
    Navigator.of(context).push<void>(_articleRoute(id));
  }

  @override
  Widget build(BuildContext context) {
    final strings = litchStrings(context);
    final theme = Theme.of(context);
    final topics = _buildTopics(strings);
    // Contextual help is already a reading page: Back returns to its caller.
    if (topics.any((topic) => topic.id == widget.initialTopic)) {
      return _ManualArticleScreen(topicId: widget.initialTopic!);
    }
    final normalizedQuery = _query.trim().toLowerCase();
    final visibleTopics = normalizedQuery.isEmpty
        ? topics
        : topics.where(
            (topic) =>
                topic.title.toLowerCase().contains(normalizedQuery) ||
                topic.body.toLowerCase().contains(normalizedQuery) ||
                (topic.recovery?.toLowerCase().contains(normalizedQuery) ??
                    false) ||
                (topic.attribution?.toLowerCase().contains(normalizedQuery) ??
                    false) ||
                (topic.id == 'controls' &&
                    quickSetupReference(strings).any(
                      (item) =>
                          item.label.toLowerCase().contains(normalizedQuery),
                    )),
          );

    return _ManualPage(
      child: _ManualContent(
        scrollKey: const PageStorageKey<String>('manual-scroll-view'),
        controller: _scrollController,
        children: [
          if (normalizedQuery.isEmpty) ...[
            Text(
              strings.manualIntroduction,
              style: theme.textTheme.bodyLarge?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
                height: 1.45,
              ),
            ),
            const SizedBox(height: 20),
          ],
          TextField(
            key: const ValueKey<String>('manual-search'),
            controller: _searchController,
            textInputAction: TextInputAction.search,
            decoration: InputDecoration(
              labelText: strings.manualSearchLabel,
              filled: true,
              fillColor: theme.colorScheme.surfaceContainerLow,
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(14),
                borderSide: BorderSide.none,
              ),
              prefixIcon: Icon(
                Icons.search_rounded,
                color: theme.colorScheme.onSurface,
              ),
              suffixIcon: _query.isEmpty
                  ? null
                  : IconButton(
                      key: const ValueKey<String>('manual-search-clear'),
                      tooltip: MaterialLocalizations.of(
                        context,
                      ).clearButtonTooltip,
                      onPressed: _clearSearch,
                      icon: Icon(
                        Icons.clear_rounded,
                        color: theme.colorScheme.onSurface,
                      ),
                    ),
            ),
            onChanged: (value) => setState(() => _query = value),
            onSubmitted: (_) => FocusScope.of(context).unfocus(),
          ),
          const SizedBox(height: 16),
          if (visibleTopics.isEmpty)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 24),
              child: Text(
                strings.manualSearchNoResults,
                key: const ValueKey<String>('manual-search-empty'),
                style: theme.textTheme.bodyLarge,
              ),
            )
          else
            ManualColumns(
              key: const ValueKey<String>('manual-topic-index'),
              children: [
                for (final topic in visibleTopics)
                  Column(
                    children: [
                      _ManualTopicRow(
                        key: normalizedQuery.isEmpty
                            ? null
                            : ValueKey<String>('manual-search-${topic.id}'),
                        data: topic,
                        onPressed: () => _openTopic(topic.id),
                      ),
                      const Divider(height: 1),
                    ],
                  ),
              ],
            ),
        ],
      ),
    );
  }
}

CupertinoPageRoute<void> _articleRoute(String id) => CupertinoPageRoute<void>(
  settings: RouteSettings(name: '/manual/$id'),
  builder: (_) => _ManualArticleScreen(topicId: id),
);

// Index and reader share the same system insets, heading and readable width.
class _ManualPage extends StatelessWidget {
  const _ManualPage({required this.child, this.navigation});

  final Widget child;
  final Widget? navigation;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final textScaler = LitchCompactControls.textScalerOf(context);
    return LayoutBuilder(
      builder: (context, constraints) => Scaffold(
        appBar: LitchCompactAppBar(
          child: AppBar(
            iconTheme: IconThemeData(color: theme.colorScheme.onSurface),
            toolbarHeight: LitchCompactControls.toolbarHeightOf(context),
            title: Text(
              litchStrings(context).manualTitle,
              textScaler: textScaler,
              softWrap: true,
              maxLines: 2,
            ),
          ),
        ),
        body: SafeArea(top: false, child: child),
        bottomNavigationBar: navigation,
      ),
    );
  }
}

class _ManualContent extends StatelessWidget {
  const _ManualContent({
    required this.scrollKey,
    required this.children,
    this.controller,
    this.maxWidth = 760,
  });

  final double maxWidth;
  final Key scrollKey;
  final List<Widget> children;
  final ScrollController? controller;

  @override
  Widget build(BuildContext context) => ListView(
    key: scrollKey,
    controller: controller,
    primary: false,
    padding: const EdgeInsets.fromLTRB(20, 20, 20, 32),
    children: [
      Center(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxWidth: maxWidth),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: children,
          ),
        ),
      ),
    ],
  );
}

List<_ManualTopicData> _buildTopics(AppLocalizations strings) =>
    <_ManualTopicData>[
      _ManualTopicData(
        id: 'start',
        title: strings.manualStartTitle,
        body: strings.manualStartBody,
      ),
      _ManualTopicData(
        id: 'controls',
        title: strings.manualControlsTitle,
        body: strings.manualControlsBody,
        recovery: strings.manualControlsRecovery,
      ),
      _ManualTopicData(
        id: 'moves',
        title: strings.manualMovesTitle,
        body: strings.manualMovesBody,
      ),
      _ManualTopicData(
        id: 'litchcraft',
        title: strings.manualLitchcraftTitle,
        body: strings.manualLitchcraftBody,
      ),
      _ManualTopicData(
        id: 'computer',
        title: strings.manualComputerTitle,
        body: strings.manualComputerBody,
      ),
      _ManualTopicData(
        id: 'duck',
        title: strings.manualDuckTitle,
        body: strings.manualDuckBody,
        attribution: strings.manualDuckAttribution,
      ),
      _ManualTopicData(
        id: 'assistance',
        title: strings.manualAssistanceTitle,
        body: strings.manualAssistanceBody,
      ),
      _ManualTopicData(
        id: 'undo',
        title: strings.manualUndoTitle,
        body: strings.manualUndoBody,
      ),
      _ManualTopicData(
        id: 'clock',
        title: strings.manualClockTitle,
        body: strings.manualClockBody,
      ),
      _ManualTopicData(
        id: 'lights',
        title: strings.boardLights,
        body: strings.manualLightsBody,
      ),
      _ManualTopicData(
        id: 'recovery',
        title: strings.manualRecoveryTitle,
        body: strings.manualRecoveryBody,
      ),
      _ManualTopicData(
        id: 'factory-reset',
        title: strings.manualFactoryResetTitle,
        body: strings.manualFactoryResetBody,
      ),
    ];

class _ManualTopicData {
  const _ManualTopicData({
    required this.id,
    required this.title,
    required this.body,
    this.recovery,
    this.attribution,
  });

  final String id;
  final String title;
  final String body;
  final String? recovery;
  final String? attribution;
}

// Keep the navigation identities intact while Vex alone retains full color.
const _manualMonochrome = ColorFilter.matrix(<double>[
  .2126,
  .7152,
  .0722,
  0,
  0,
  .2126,
  .7152,
  .0722,
  0,
  0,
  .2126,
  .7152,
  .0722,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
]);

Widget _topicIcon(String id, Color color) {
  final key = ValueKey<String>('manual-$id-icon');
  if (id == 'duck') {
    return Image.asset(
      HomeChessboard.duckAsset,
      key: key,
      width: 26,
      height: 26,
      cacheWidth: 104,
      fit: BoxFit.contain,
      excludeFromSemantics: true,
    );
  }
  if (id == 'start' || id == 'litchcraft') {
    return ColorFiltered(
      key: key,
      colorFilter: _manualMonochrome,
      child: IconTheme(
        data: IconThemeData(color: color),
        child: id == 'start'
            ? const LitchPlayGlyph(size: 26)
            : const LitchcraftIdentityGlyph(size: 26),
      ),
    );
  }
  if (id == 'controls') {
    return BoardControlsGlyph(key: key, color: color);
  }
  return Icon(
    switch (id) {
      'clock' => Icons.timer_outlined,
      'moves' => Icons.touch_app_outlined,
      'computer' => Icons.smart_toy_outlined,
      'assistance' => Icons.lightbulb_outline_rounded,
      'undo' => Icons.undo_rounded,
      'lights' => Icons.light_mode_outlined,
      'recovery' => Icons.health_and_safety_outlined,
      _ => Icons.restart_alt_rounded,
    },
    key: key,
    color: color,
    size: 26,
  );
}

class _ManualTopicRow extends StatelessWidget {
  const _ManualTopicRow({
    super.key,
    required this.data,
    required this.onPressed,
  });

  final _ManualTopicData data;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final color = theme.colorScheme.onSurface;
    return Semantics(
      key: PageStorageKey<String>('manual-${data.id}'),
      button: true,
      label: data.title,
      onTap: onPressed,
      excludeSemantics: true,
      child: ListTile(
        contentPadding: const EdgeInsets.symmetric(horizontal: 4, vertical: 8),
        minTileHeight: 64,
        minLeadingWidth: 44,
        leading: ExcludeSemantics(
          child: SizedBox(
            width: 44,
            height: 40,
            child: Center(child: _topicIcon(data.id, color)),
          ),
        ),
        title: Text(
          data.title,
          style: theme.textTheme.bodyLarge,
          softWrap: true,
        ),
        trailing: Icon(Icons.chevron_right_rounded, color: color),
        onTap: onPressed,
      ),
    );
  }
}

class _ManualArticleScreen extends StatefulWidget {
  const _ManualArticleScreen({required this.topicId});

  final String topicId;

  @override
  State<_ManualArticleScreen> createState() => _ManualArticleScreenState();
}

class _ManualArticleScreenState extends State<_ManualArticleScreen> {
  PageController? _pages;
  int _index = 0;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_pages != null) {
      return;
    }
    _index = _buildTopics(
      litchStrings(context),
    ).indexWhere((topic) => topic.id == widget.topicId);
    if (_index < 0) _index = 0;
    _pages = PageController(initialPage: _index);
  }

  @override
  void dispose() {
    _pages?.dispose();
    super.dispose();
  }

  void _goTo(int index) => _pages!.animateToPage(
    index,
    duration: const Duration(milliseconds: 240),
    curve: Curves.easeOutCubic,
  );

  Future<void> _openDuckChess() async {
    final result = await ExternalLinkLauncher.shared.open(
      LitchExternalLinks.duckChess,
    );
    if (!mounted ||
        result == ExternalLinkLaunchResult.opened ||
        result == ExternalLinkLaunchResult.busy) {
      return;
    }
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(content: Text(litchStrings(context).linkOpenFailed)),
      );
  }

  @override
  Widget build(BuildContext context) {
    final topics = _buildTopics(litchStrings(context));
    final theme = Theme.of(context);
    final strings = litchStrings(context);
    final tablet = LitchWorkspaceGeometry.isTabletWindow(
      MediaQuery.sizeOf(context),
    );
    final readingWidth = tablet ? LitchReadingPane.proseWidth : 760.0;
    final footer = LitchCompactControls(
      child: Material(
        color: theme.colorScheme.surface,
        child: SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
            child: Center(
              heightFactor: 1,
              child: ConstrainedBox(
                constraints: BoxConstraints(maxWidth: readingWidth),
                child: LayoutBuilder(
                  builder: (context, constraints) => IntrinsicHeight(
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        Expanded(
                          child: _ArticleLink(
                            data: _index > 0 ? topics[_index - 1] : null,
                            previous: true,
                            showArrow: _manualArrowFits(
                              context,
                              strings.manualPrevious,
                              (constraints.maxWidth - 12) / 2,
                            ),
                            directionLabel: strings.manualPrevious,
                            onPressed: _index > 0
                                ? () => _goTo(_index - 1)
                                : null,
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: _ArticleLink(
                            data: _index + 1 < topics.length
                                ? topics[_index + 1]
                                : null,
                            previous: false,
                            showArrow: _manualArrowFits(
                              context,
                              strings.manualNext,
                              (constraints.maxWidth - 12) / 2,
                            ),
                            directionLabel: strings.manualNext,
                            onPressed: _index + 1 < topics.length
                                ? () => _goTo(_index + 1)
                                : null,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    final articles = PageView.builder(
      key: const ValueKey('manual-article-pages'),
      controller: _pages,
      itemCount: topics.length,
      allowImplicitScrolling: true,
      onPageChanged: (index) => setState(() => _index = index),
      itemBuilder: (context, index) {
        final data = topics[index];
        // Put the reference between the Quick Setup directions and the
        // longer Bluetooth and Litchcraft instructions.
        final overviewEnd = data.id == 'controls'
            ? data.body.indexOf('\n\n')
            : -1;
        final bodyStyle = theme.textTheme.bodyLarge?.copyWith(height: 1.5);
        return ExcludeFocus(
          excluding: index != _index,
          child: _ManualContent(
            maxWidth: readingWidth,
            scrollKey: PageStorageKey<String>(
              'manual-${data.id}-article-scroll',
            ),
            children: [
              Semantics(
                header: true,
                child: Row(
                  children: [
                    ExcludeSemantics(
                      child: SizedBox(
                        width: 44,
                        child: Center(
                          child: _topicIcon(
                            data.id,
                            theme.colorScheme.onSurface,
                          ),
                        ),
                      ),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        data.title,
                        style: theme.textTheme.titleLarge,
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 16),
              Text(
                overviewEnd < 0
                    ? data.body
                    : data.body.substring(0, overviewEnd),
                key: ValueKey<String>('manual-${data.id}-body'),
                style: bodyStyle,
              ),
              if (overviewEnd >= 0) ...[
                const SizedBox(height: 20),
                ManualQuickSetupReference(strings: strings),
                const SizedBox(height: 20),
                Text(data.body.substring(overviewEnd + 2), style: bodyStyle),
              ],
              if (data.recovery case final recovery?) ...[
                const SizedBox(height: 16),
                Text(
                  recovery,
                  key: ValueKey<String>('manual-${data.id}-recovery'),
                  style: theme.textTheme.bodyLarge?.copyWith(height: 1.5),
                ),
              ],
              if (data.attribution case final attribution?) ...[
                const SizedBox(height: 24),
                Text(
                  attribution,
                  key: const ValueKey('manual-duck-attribution'),
                  style: theme.textTheme.bodyLarge?.copyWith(
                    height: 1.5,
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
                Align(
                  alignment: AlignmentDirectional.centerStart,
                  child: TextButton.icon(
                    key: const ValueKey('manual-duck-link'),
                    style: TextButton.styleFrom(
                      minimumSize: const Size(48, 48),
                    ),
                    onPressed: _openDuckChess,
                    icon: const Icon(Icons.open_in_new_rounded),
                    label: const Text('duckchess.com'),
                  ),
                ),
              ],
            ],
          ),
        );
      },
    );
    return LayoutBuilder(
      builder: (context, constraints) {
        final topicWidth = 104 + 9 * MediaQuery.textScalerOf(context).scale(16);
        final showContents =
            tablet &&
            constraints.maxWidth >=
                topicWidth + LitchReadingPane.proseWidth + 80;
        if (!showContents) {
          return _ManualPage(navigation: footer, child: articles);
        }
        return _ManualPage(
          child: Center(
            child: ConstrainedBox(
              constraints: BoxConstraints(
                maxWidth: topicWidth + LitchReadingPane.proseWidth + 80,
              ),
              child: Row(
                children: [
                  SizedBox(
                    width: topicWidth,
                    child: ListView(
                      primary: false,
                      padding: const EdgeInsets.symmetric(
                        vertical: 16,
                        horizontal: 8,
                      ),
                      children: [
                        for (int i = 0; i < topics.length; i++)
                          ListTile(
                            selected: i == _index,
                            selectedTileColor:
                                theme.colorScheme.secondaryContainer,
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(12),
                            ),
                            title: Text(topics[i].title),
                            onTap: () => _pages!.jumpToPage(i),
                          ),
                      ],
                    ),
                  ),
                  const VerticalDivider(width: 1),
                  Expanded(
                    child: Column(
                      children: [
                        Expanded(child: articles),
                        footer,
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}

bool _manualArrowFits(BuildContext context, String label, double width) {
  final text = TextPainter(
    text: TextSpan(text: label, style: Theme.of(context).textTheme.labelLarge),
    textDirection: Directionality.of(context),
    textScaler: LitchCompactControls.textScalerOf(context),
    locale: Localizations.localeOf(context),
  )..layout();
  final fits = text.width + 40 <= width;
  text.dispose();
  return fits;
}

class _ArticleLink extends StatelessWidget {
  const _ArticleLink({
    required this.data,
    required this.previous,
    required this.showArrow,
    required this.directionLabel,
    required this.onPressed,
  });

  final _ManualTopicData? data;
  final bool previous;
  final bool showArrow;
  final String directionLabel;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final label = data == null
        ? directionLabel
        : '$directionLabel: ${data!.title}';
    final style = TextButton.styleFrom(
      foregroundColor: Theme.of(context).colorScheme.onSurface,
      minimumSize: const Size(48, 48),
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 8),
    );
    final text = Text(
      directionLabel,
      semanticsLabel: label,
      textAlign: TextAlign.center,
    );
    final key = ValueKey<String>(
      'manual-${previous ? 'previous' : 'next'}-article',
    );
    return Tooltip(
      message: label,
      excludeFromSemantics: true,
      child: showArrow
          ? TextButton.icon(
              key: key,
              style: style,
              onPressed: onPressed,
              iconAlignment: previous ? IconAlignment.start : IconAlignment.end,
              icon: Icon(
                previous
                    ? Icons.arrow_back_rounded
                    : Icons.arrow_forward_rounded,
              ),
              label: text,
            )
          : TextButton(
              key: key,
              style: style,
              onPressed: onPressed,
              child: text,
            ),
    );
  }
}
