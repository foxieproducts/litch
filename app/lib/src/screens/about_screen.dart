import 'dart:async';

import 'package:flutter/cupertino.dart' show CupertinoPageRoute;
import 'package:flutter/material.dart';

import '../widgets/compact_controls.dart';
import '../widgets/reading_pane.dart';
import '../widgets/litch_workspace_geometry.dart';
import 'package:package_info_plus/package_info_plus.dart';

import '../localization/litch_strings.dart';
import '../models/app_release_identity.dart';
import '../services/external_link_launcher.dart';
import 'manual_screen.dart';
import 'privacy_policy_screen.dart';

typedef AboutLinkOpener = Future<ExternalLinkLaunchResult> Function(Uri uri);
typedef AboutPackageInfoLoader = Future<PackageInfo> Function();

class LitchAcknowledgement {
  const LitchAcknowledgement({required this.name, required this.note});

  final String name;
  final String note;
}

// Keep personal thanks as content, separate from the layout, so this can grow
// without turning the About screen into a hand-maintained widget tree.
const List<LitchAcknowledgement> litchPersonalAcknowledgements =
    <LitchAcknowledgement>[];

class LitchAboutBrandButton extends StatelessWidget {
  const LitchAboutBrandButton({
    super.key,
    this.onPressed,
    this.onLoadVersionArtwork,
    this.versionTapClock,
    this.litchcraft = false,
    this.wordmarkHeight = 23,
    this.interactive = true,
    this.fitToWidth = true,
  });

  final VoidCallback? onPressed;
  final ValueChanged<int>? onLoadVersionArtwork;
  final DateTime Function()? versionTapClock;
  final bool litchcraft;
  final double wordmarkHeight;
  final bool interactive;
  final bool fitToWidth;

  // Budget for the wider wordmark so both page identities retain the same
  // lettering height beside the standard connected-board action.
  static double widthForHeight(double height) =>
      (267 / 28 * 23 + 5 + 674 / 644 * 26.3) * height / 23;

  @override
  Widget build(BuildContext context) {
    final VoidCallback open =
        onPressed ??
        () {
          Navigator.of(context).push<void>(
            AboutScreen.route(
              onLoadVersionArtwork: onLoadVersionArtwork,
              versionTapClock: versionTapClock,
            ),
          );
        };

    final artwork = Row(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Image.asset(
          litchcraft
              ? 'assets/branding/foxie-litchcraft-wordmark.png'
              : 'assets/branding/foxie-litch-wordmark.png',
          height: wordmarkHeight,
          excludeFromSemantics: true,
          filterQuality: FilterQuality.high,
        ),
        SizedBox(width: 5 * wordmarkHeight / 23),
        Image.asset(
          'assets/branding/foxie-fox.png',
          height: 26.3 * wordmarkHeight / 23,
          excludeFromSemantics: true,
          filterQuality: FilterQuality.high,
        ),
      ],
    );
    final Widget brand = ConstrainedBox(
      constraints: BoxConstraints(
        minWidth: interactive ? 48 : 0,
        minHeight: interactive ? 48 : 0,
      ),
      child: Align(
        alignment: Alignment.centerLeft,
        child: fitToWidth
            ? FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerLeft,
                child: artwork,
              )
            : artwork,
      ),
    );
    if (!interactive) {
      return Semantics(
        key: const ValueKey<String>('compact-brand'),
        header: true,
        label: litchStrings(context).aboutApp,
        excludeSemantics: true,
        child: brand,
      );
    }
    return Semantics(
      button: true,
      header: true,
      label: litchStrings(context).aboutApp,
      excludeSemantics: true,
      onTap: open,
      child: Tooltip(
        message: litchStrings(context).aboutApp,
        excludeFromSemantics: true,
        child: Material(
          color: Colors.transparent,
          child: InkWell(
            key: const ValueKey<String>('open-about'),
            onTap: open,
            borderRadius: BorderRadius.circular(8),
            child: brand,
          ),
        ),
      ),
    );
  }
}

class AboutScreen extends StatefulWidget {
  const AboutScreen({
    super.key,
    this.acknowledgements = litchPersonalAcknowledgements,
    this.openExternalLink,
    this.loadPackageInfo,
    this.onLoadVersionArtwork,
    this.versionTapClock,
  });

  final List<LitchAcknowledgement> acknowledgements;
  final AboutLinkOpener? openExternalLink;
  final AboutPackageInfoLoader? loadPackageInfo;
  final ValueChanged<int>? onLoadVersionArtwork;
  final DateTime Function()? versionTapClock;

  static CupertinoPageRoute<void> route({
    ValueChanged<int>? onLoadVersionArtwork,
    DateTime Function()? versionTapClock,
  }) {
    return CupertinoPageRoute<void>(
      settings: const RouteSettings(name: '/about'),
      builder: (_) => AboutScreen(
        onLoadVersionArtwork: onLoadVersionArtwork,
        versionTapClock: versionTapClock,
      ),
    );
  }

  @override
  State<AboutScreen> createState() => _AboutScreenState();
}

class _AboutScreenState extends State<AboutScreen> {
  PackageInfo? _packageInfo;
  int _versionTaps = 0;
  DateTime? _lastVersionTap;
  Timer? _versionTapTimer;

  static const _sequenceGap = Duration(milliseconds: 1300);

  void _cancelVersionSequence() {
    _versionTapTimer?.cancel();
    _versionTapTimer = null;
    _lastVersionTap = null;
    _versionTaps = 0;
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (ModalRoute.of(context)?.isCurrent != true) {
      _cancelVersionSequence();
    }
  }

  @override
  void didUpdateWidget(covariant AboutScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.onLoadVersionArtwork == null) {
      _cancelVersionSequence();
    }
  }

  void _tapVersion() {
    if (widget.onLoadVersionArtwork == null) return;
    final now = widget.versionTapClock?.call() ?? DateTime.now();
    final gap = _lastVersionTap == null
        ? null
        : now.difference(_lastVersionTap!);
    final continuous =
        gap != null && gap >= Duration.zero && gap <= _sequenceGap;
    _lastVersionTap = now;
    _versionTapTimer?.cancel();
    setState(() {
      _versionTaps = continuous ? (_versionTaps + 1).clamp(0, 8) : 1;
    });
    _versionTapTimer = Timer(_sequenceGap, () {
      if (!mounted) return;
      final action = widget.onLoadVersionArtwork;
      if (action == null || ModalRoute.of(context)?.isCurrent != true) {
        setState(_cancelVersionSequence);
        return;
      }
      final taps = _versionTaps;
      Navigator.of(context).pop();
      action(taps);
    });
  }

  @override
  void dispose() {
    _versionTapTimer?.cancel();
    super.dispose();
  }

  @override
  void initState() {
    super.initState();
    _loadPackageInfo();
  }

  Future<void> _loadPackageInfo() async {
    try {
      final AboutPackageInfoLoader loader =
          widget.loadPackageInfo ?? PackageInfo.fromPlatform;
      final PackageInfo packageInfo = await loader();
      if (!mounted) {
        return;
      }
      setState(() => _packageInfo = packageInfo);
    } catch (_) {}
  }

  Future<void> _openLink(Uri uri) async {
    final AboutLinkOpener opener =
        widget.openExternalLink ?? ExternalLinkLauncher.shared.open;
    final ExternalLinkLaunchResult result = await opener(uri);
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

  void _showLicenses() {
    final navigator = Navigator.of(context);
    final themes = InheritedTheme.capture(from: context, to: navigator.context);
    final notices = LicensePage(
      applicationName: 'Foxie Litch',
      applicationVersion: _versionText(context, _packageInfo),
      applicationIcon: const _LitchMark(size: 56),
      applicationLegalese: litchStrings(context).aboutLicenseNotice,
    );
    navigator.push<void>(
      MaterialPageRoute<void>(
        builder: (context) => themes.wrap(
          Builder(
            builder: (context) {
              if (!LitchWorkspaceGeometry.isTabletWindow(
                MediaQuery.sizeOf(context),
              )) {
                return notices;
              }
              // Leave room for the framework's package list beside a readable notice.
              return ColoredBox(
                color: Theme.of(context).scaffoldBackgroundColor,
                child: Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(
                      maxWidth:
                          320 + LitchWorkspaceGeometry.readableContentWidth,
                    ),
                    child: notices,
                  ),
                ),
              );
            },
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final TextScaler textScaler = LitchCompactControls.textScalerOf(context);
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(
          toolbarHeight: LitchCompactControls.toolbarHeightOf(context),
          title: Text(
            litchStrings(context).aboutTitle,
            textScaler: textScaler,
            softWrap: true,
            maxLines: 2,
          ),
        ),
      ),
      body: SafeArea(
        top: false,
        child: LitchReadingPane(
          child: CustomScrollView(
            key: const ValueKey<String>('about-scroll-view'),
            slivers: <Widget>[
              SliverPadding(
                padding: const EdgeInsets.fromLTRB(20, 20, 20, 0),
                sliver: SliverList.list(
                  children: <Widget>[
                    _AboutHero(
                      packageInfo: _packageInfo,
                      onVersionTap: widget.onLoadVersionArtwork == null
                          ? null
                          : _tapVersion,
                      versionTaps: _versionTaps,
                    ),
                    const SizedBox(height: 20),
                    Card(
                      child: _AboutLinkTile(
                        key: const ValueKey<String>('about-manual'),
                        icon: Icons.menu_book_rounded,
                        title: litchStrings(context).manualTitle,
                        subtitle: litchStrings(context).manualDescription,
                        trailingIcon: Icons.chevron_right_rounded,
                        onTap: () => Navigator.of(
                          context,
                        ).push<void>(ManualScreen.route()),
                      ),
                    ),
                    const SizedBox(height: 20),
                    _SectionHeading(litchStrings(context).aboutDiscover),
                    const SizedBox(height: 8),
                    Card(
                      child: Column(
                        children: <Widget>[
                          _AboutLinkTile(
                            key: const ValueKey<String>('about-product-link'),
                            icon: Icons.language_rounded,
                            title: litchStrings(context).aboutExplore,
                            subtitle: 'foxieproducts.com/litch',
                            onTap: () => _openLink(LitchExternalLinks.product),
                          ),
                          const Divider(height: 1),
                          _AboutLinkTile(
                            key: const ValueKey<String>('about-github-link'),
                            icon: Icons.code_rounded,
                            title: litchStrings(context).aboutGithub,
                            subtitle:
                                'github.com/foxieproducts\n${litchStrings(context).aboutGithubDescription}',
                            onTap: () => _openLink(LitchExternalLinks.github),
                          ),
                        ],
                      ),
                    ),
                    const SizedBox(height: 20),
                    _SectionHeading(litchStrings(context).aboutOpenSource),
                    const SizedBox(height: 8),
                    Text(
                      litchStrings(context).aboutOpenSourceBody,
                      style: theme.textTheme.bodyLarge?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                        height: 1.45,
                      ),
                    ),
                    const SizedBox(height: 12),
                    Card(
                      child: _AboutLinkTile(
                        key: const ValueKey<String>('about-licenses'),
                        icon: Icons.article_outlined,
                        title: litchStrings(context).aboutLicenses,
                        subtitle: litchStrings(
                          context,
                        ).aboutLicensesDescription,
                        trailingIcon: Icons.chevron_right_rounded,
                        onTap: _showLicenses,
                      ),
                    ),
                    const SizedBox(height: 12),
                    Card(
                      child: _AboutLinkTile(
                        key: const ValueKey<String>('about-privacy-policy'),
                        icon: Icons.privacy_tip_outlined,
                        title: litchStrings(context).aboutPrivacyPolicy,
                        subtitle: litchStrings(
                          context,
                        ).aboutPrivacyPolicyDescription,
                        trailingIcon: Icons.chevron_right_rounded,
                        onTap: () => Navigator.of(
                          context,
                        ).push<void>(PrivacyPolicyScreen.route()),
                      ),
                    ),
                    const SizedBox(height: 20),
                    _SectionHeading(litchStrings(context).aboutThanks),
                    const SizedBox(height: 8),
                    Text(
                      litchStrings(context).aboutThanksBody,
                      style: theme.textTheme.bodyLarge?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                        height: 1.45,
                      ),
                    ),
                    if (widget.acknowledgements.isNotEmpty)
                      const SizedBox(height: 12)
                    else
                      const SizedBox(height: 32),
                  ],
                ),
              ),
              if (widget.acknowledgements.isNotEmpty)
                SliverPadding(
                  padding: const EdgeInsets.fromLTRB(20, 0, 20, 32),
                  sliver: SliverList.separated(
                    itemCount: widget.acknowledgements.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 8),
                    itemBuilder: (BuildContext context, int index) {
                      final LitchAcknowledgement acknowledgement =
                          widget.acknowledgements[index];
                      return Card(
                        child: ListTile(
                          minTileHeight: 64,
                          leading: const Icon(Icons.favorite_outline_rounded),
                          title: Text(acknowledgement.name),
                          subtitle: Text(acknowledgement.note),
                        ),
                      );
                    },
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _AboutHero extends StatelessWidget {
  const _AboutHero({
    required this.packageInfo,
    required this.onVersionTap,
    required this.versionTaps,
  });

  final PackageInfo? packageInfo;
  final VoidCallback? onVersionTap;
  final int versionTaps;

  @override
  Widget build(BuildContext context) {
    final ThemeData theme = Theme.of(context);
    final strings = litchStrings(context);
    final String? version = _versionText(context, packageInfo);
    const textShadows = <Shadow>[
      Shadow(color: Colors.black, blurRadius: 4, offset: Offset(0, 1)),
    ];
    return Card(
      clipBehavior: Clip.antiAlias,
      child: Semantics(
        key: const ValueKey<String>('about-hero'),
        header: true,
        button: onVersionTap != null,
        label: <String>[
          strings.aboutApp,
          strings.aboutTagline,
          ?version,
        ].join('\n'),
        value: versionTaps == 0
            ? null
            : strings.aboutVersionTapsRecorded(versionTaps),
        onTap: onVersionTap,
        excludeSemantics: true,
        child: Stack(
          children: <Widget>[
            Positioned.fill(
              child: Image.asset(
                'assets/images/foxie_planet.jpeg',
                fit: BoxFit.cover,
                excludeFromSemantics: true,
              ),
            ),
            Positioned.fill(
              child: ColoredBox(color: Colors.black.withValues(alpha: 0.50)),
            ),
            Material(
              type: MaterialType.transparency,
              child: InkWell(
                excludeFromSemantics: true,
                onTap: onVersionTap,
                child: Padding(
                  padding: EdgeInsets.fromLTRB(
                    16,
                    16,
                    16,
                    version == null ? 16 : 8,
                  ),
                  child: Column(
                    children: <Widget>[
                      Row(
                        children: [
                          const _LitchMark(size: 48),
                          const SizedBox(width: 16),
                          Expanded(
                            child: Semantics(
                              header: true,
                              child: const LitchAboutBrandButton(
                                interactive: false,
                              ),
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 8),
                      Text(
                        strings.aboutTagline,
                        textAlign: TextAlign.start,
                        style: theme.textTheme.bodyLarge?.copyWith(
                          color: Colors.white,
                          height: 1.4,
                          shadows: textShadows,
                        ),
                      ),
                      if (version != null || versionTaps > 0)
                        ConstrainedBox(
                          constraints: const BoxConstraints(
                            minWidth: 48,
                            minHeight: 48,
                          ),
                          child: Column(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: <Widget>[
                              if (version != null)
                                Text(
                                  version,
                                  key: const ValueKey<String>('about-version'),
                                  style: theme.textTheme.labelLarge?.copyWith(
                                    color: Colors.white,
                                    shadows: textShadows,
                                  ),
                                ),
                              if (versionTaps > 0)
                                Text(
                                  List<String>.filled(
                                    versionTaps,
                                    '•',
                                  ).join(' '),
                                  key: const ValueKey(
                                    'about-version-tap-feedback',
                                  ),
                                  style: theme.textTheme.labelSmall?.copyWith(
                                    color: Colors.white,
                                    shadows: textShadows,
                                  ),
                                ),
                            ],
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _LitchMark extends StatelessWidget {
  const _LitchMark({required this.size});

  final double size;

  @override
  Widget build(BuildContext context) {
    final int cacheSize = (size * MediaQuery.devicePixelRatioOf(context))
        .ceil();
    return ExcludeSemantics(
      child: ClipRRect(
        borderRadius: BorderRadius.circular(size * 0.22),
        child: Image.asset(
          'assets/branding/foxie-app-icon.png',
          width: size,
          height: size,
          cacheWidth: cacheSize,
          cacheHeight: cacheSize,
          filterQuality: FilterQuality.high,
        ),
      ),
    );
  }
}

class _SectionHeading extends StatelessWidget {
  const _SectionHeading(this.text);

  final String text;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      header: true,
      child: Text(
        text,
        style: Theme.of(
          context,
        ).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700),
      ),
    );
  }
}

class _AboutLinkTile extends StatelessWidget {
  const _AboutLinkTile({
    super.key,
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.onTap,
    this.trailingIcon = Icons.open_in_new_rounded,
  });

  final IconData icon;
  final String title;
  final String subtitle;
  final VoidCallback onTap;
  final IconData trailingIcon;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      button: true,
      label: '$title. $subtitle',
      excludeSemantics: true,
      onTap: onTap,
      child: ListTile(
        minTileHeight: 72,
        leading: Icon(icon),
        title: Text(title),
        subtitle: Text(subtitle),
        trailing: Icon(trailingIcon),
        onTap: onTap,
      ),
    );
  }
}

String? _versionText(BuildContext context, PackageInfo? packageInfo) {
  if (packageInfo == null || packageInfo.version.trim().isEmpty) {
    return null;
  }
  final String version = litchAppReleaseIdentity.versionFor(packageInfo);
  final String build = packageInfo.buildNumber.trim();
  return build.isEmpty
      ? litchStrings(context).versionLabel(version)
      : litchStrings(context).versionLabel('$version ($build)');
}
