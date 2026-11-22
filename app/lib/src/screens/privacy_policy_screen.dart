import 'package:flutter/cupertino.dart' show CupertinoPageRoute;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import '../localization/litch_strings.dart';
import '../services/external_link_launcher.dart';
import '../widgets/compact_controls.dart';
import '../widgets/litch_workspace_geometry.dart';
import '../widgets/reading_pane.dart';

class PrivacyPolicyScreen extends StatefulWidget {
  const PrivacyPolicyScreen({super.key});

  static CupertinoPageRoute<void> route() => CupertinoPageRoute<void>(
    settings: const RouteSettings(name: '/about/privacy'),
    builder: (_) => const PrivacyPolicyScreen(),
  );

  @override
  State<PrivacyPolicyScreen> createState() => _PrivacyPolicyScreenState();
}

class _PrivacyPolicyScreenState extends State<PrivacyPolicyScreen> {
  final Future<String> _policy = rootBundle.loadString(
    'assets/privacy/foxie_litch_privacy_policy.md',
  );

  Future<void> _openOnlinePolicy() async {
    final result = await ExternalLinkLauncher.shared.open(
      LitchExternalLinks.privacyPolicy,
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
    final strings = litchStrings(context);
    return Scaffold(
      appBar: LitchCompactAppBar(
        child: AppBar(
          toolbarHeight: LitchCompactControls.toolbarHeightOf(context),
          title: Text(strings.aboutPrivacyPolicy, maxLines: 2),
        ),
      ),
      body: SafeArea(
        top: false,
        child: FutureBuilder<String>(
          future: _policy,
          builder: (context, snapshot) {
            if (snapshot.hasError) {
              return Center(child: Text(strings.privacyPolicyOpenFailed));
            }
            final policy = snapshot.data;
            if (policy == null) {
              return const Center(child: CircularProgressIndicator());
            }
            return SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(20, 20, 20, 32),
              child: Center(
                child: ConstrainedBox(
                  constraints: BoxConstraints(
                    maxWidth:
                        LitchWorkspaceGeometry.isTabletWindow(
                          MediaQuery.sizeOf(context),
                        )
                        ? LitchReadingPane.proseWidth
                        : 760,
                  ),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Semantics(
                        localeForSubtree: const Locale('en'),
                        child: SelectionArea(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              for (final block in policy.trim().split(
                                RegExp(r'\n\s*\n'),
                              ))
                                _PolicyBlock(block),
                            ],
                          ),
                        ),
                      ),
                      Align(
                        alignment: AlignmentDirectional.centerStart,
                        child: TextButton(
                          isSemanticButton: false,
                          style: TextButton.styleFrom(
                            minimumSize: const Size(48, 48),
                          ),
                          onPressed: _openOnlinePolicy,
                          child: Semantics(
                            link: true,
                            linkUrl: LitchExternalLinks.privacyPolicy,
                            child: Text(strings.readPrivacyPolicyOnline),
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            );
          },
        ),
      ),
    );
  }
}

class _PolicyBlock extends StatelessWidget {
  const _PolicyBlock(this.block);

  final String block;

  @override
  Widget build(BuildContext context) {
    final styles = Theme.of(context).textTheme;
    final isTitle = block.startsWith('# ');
    if (isTitle || block.startsWith('## ')) {
      return Padding(
        padding: EdgeInsets.only(top: isTitle ? 0 : 8, bottom: 8),
        child: Semantics(
          header: true,
          headingLevel: isTitle ? 1 : 2,
          child: Text(
            block.substring(isTitle ? 2 : 3),
            style: isTitle ? styles.headlineSmall : styles.titleLarge,
          ),
        ),
      );
    }
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Text(
        block.replaceAll(RegExp(r'\r?\n'), ' '),
        style: styles.bodyLarge?.copyWith(height: 1.45),
      ),
    );
  }
}
