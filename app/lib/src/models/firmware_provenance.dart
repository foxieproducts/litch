enum FirmwareProvenanceState { official, custom, unverified, offline }

class FirmwareProvenance {
  const FirmwareProvenance({
    required this.state,
    required this.title,
    required this.message,
    this.sha256,
    this.awaitingBoardReadiness = false,
  });

  final FirmwareProvenanceState state;
  final String title;
  final String message;
  final String? sha256;

  /// The current board explicitly deferred its digest until boot acceptance.
  final bool awaitingBoardReadiness;

  static const FirmwareProvenance offline = FirmwareProvenance(
    state: FirmwareProvenanceState.offline,
    title: 'Firmware status offline',
    message:
        'Connect to a board to check its firmware. Updates remain available after you connect.',
  );

  static const FirmwareProvenance unverified = FirmwareProvenance(
    state: FirmwareProvenanceState.unverified,
    title: 'Firmware not verified',
    message:
        'This board or app can’t verify firmware provenance yet. That does not mean it is custom, and updates remain available.',
  );

  static final FirmwareProvenance pendingBoardReadiness = FirmwareProvenance(
    state: FirmwareProvenanceState.unverified,
    title: unverified.title,
    message: unverified.message,
    awaitingBoardReadiness: true,
  );

  factory FirmwareProvenance.official(String sha256) {
    return FirmwareProvenance(
      state: FirmwareProvenanceState.official,
      title: 'Official firmware',
      message: 'This board is running firmware recognized by Foxie Litch.',
      sha256: sha256,
    );
  }

  factory FirmwareProvenance.custom(String sha256) {
    return FirmwareProvenance(
      state: FirmwareProvenanceState.custom,
      title: 'Custom firmware',
      message:
          'Custom firmware detected. Thanks for making Litch your own. Please use official firmware for rated or online play—and otherwise, enjoy experimenting. Makers rule.',
      sha256: sha256,
    );
  }
}
