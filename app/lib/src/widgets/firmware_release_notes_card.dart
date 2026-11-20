import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../localization/presentation_strings.dart';

class FirmwareReleaseNotesCard extends StatelessWidget {
  const FirmwareReleaseNotesCard({
    super.key,
    required this.beta,
    required this.details,
    this.originalContent = true,
  });

  final bool beta;
  final String details;
  final bool originalContent;

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(
              beta
                  ? litchStrings(context).betaReleaseNotes
                  : litchStrings(context).releaseNotes,
              style: Theme.of(context).textTheme.titleMedium,
            ),
            const SizedBox(height: 8),
            if (originalContent &&
                !localizedUsesEnglish(litchStrings(context))) ...<Widget>[
              Text(litchStrings(context).originalNotes),
              const SizedBox(height: 8),
            ],
            SelectableText(details),
          ],
        ),
      ),
    );
  }
}
