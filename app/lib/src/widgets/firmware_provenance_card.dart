import 'package:flutter/material.dart';

import '../localization/litch_strings.dart';
import '../models/firmware_provenance.dart';

class FirmwareProvenanceCard extends StatelessWidget {
  const FirmwareProvenanceCard({super.key, required this.provenance});

  final FirmwareProvenance provenance;

  @override
  Widget build(BuildContext context) {
    final (IconData icon, Color color) = switch (provenance.state) {
      FirmwareProvenanceState.official => (
        Icons.verified_outlined,
        Colors.green,
      ),
      FirmwareProvenanceState.custom => (Icons.build_outlined, Colors.blue),
      FirmwareProvenanceState.unverified => (Icons.help_outline, Colors.orange),
      FirmwareProvenanceState.offline => (
        Icons.bluetooth_disabled_outlined,
        Colors.grey,
      ),
    };

    final strings = litchStrings(context);
    final (String title, String message) = switch (provenance.state) {
      FirmwareProvenanceState.official => (
        strings.provenanceOfficialTitle,
        strings.provenanceOfficialHelp,
      ),
      FirmwareProvenanceState.custom => (
        strings.provenanceCustomTitle,
        strings.provenanceCustomHelp,
      ),
      FirmwareProvenanceState.unverified => (
        strings.provenanceUnverifiedTitle,
        strings.provenanceUnverifiedHelp,
      ),
      FirmwareProvenanceState.offline => (
        strings.provenanceOfflineTitle,
        strings.provenanceOfflineHelp,
      ),
    };
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Icon(icon, color: color),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: <Widget>[
                  Text(title, style: Theme.of(context).textTheme.titleMedium),
                  const SizedBox(height: 4),
                  Text(message),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
