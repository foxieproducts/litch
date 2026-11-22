/// Board-owned assistance, separate from acknowledgement of a Hint toggle.
enum HomeAssistancePresentation {
  suppressed,
  off,
  pending,
  thinking,
  ready,
  refreshing,
  compoundDuck,
  duck,
  legal,
}

class HomeAssistanceState {
  const HomeAssistanceState({
    required this.requested,
    required this.manualAvailable,
    required this.coachingOwned,
    required this.presentation,
    this.move,
    this.duckMove,
    this.legalSource,
    this.legalDestinations,
  });

  final bool requested;
  // A useful app affordance, not a new gate on legacy firmware commands.
  final bool manualAvailable;
  final bool coachingOwned;
  final HomeAssistancePresentation presentation;
  final ({int from, int to})? move;
  final ({int from, int to})? duckMove;
  final int? legalSource;
  final BigInt? legalDestinations;

  bool get isReady => switch (presentation) {
    HomeAssistancePresentation.ready ||
    HomeAssistancePresentation.refreshing ||
    HomeAssistancePresentation.compoundDuck ||
    HomeAssistancePresentation.duck => true,
    _ => false,
  };
}
