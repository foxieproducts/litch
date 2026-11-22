// Typed allowlist for the reviewable support draft. Never include free text,
// artwork content/names/colors, setting values, exact squares or chess moves.
enum SupportActionKind {
  piecesChanged,
  paint,
  erase,
  artworkLoad,
  artworkSave,
  settingChange,
  hint,
  undo,
  newGame,
  coaching,
  sleep,
  storageRecovery,
  enterLitchcraft,
  exitLitchcraft,
  selectTool,
  selectFrame,
  playback,
  fill,
  clear,
  redo,
  effect,
  editFrames,
}

enum SupportActionOutcome {
  requested,
  accepted,
  applied,
  rejected,
  failed,
  canceled,
  unconfirmed,
  observed,
  noChange,
  appliedOnPhone,
}

class SupportAction {
  const SupportAction({
    required this.sequence,
    required this.uptimeMs,
    required this.kind,
    required this.outcome,
    this.durationMs,
    this.count = 0,
    this.secondCount = 0,
  });
  final int sequence, uptimeMs, count, secondCount;
  final int? durationMs;
  final SupportActionKind kind;
  final SupportActionOutcome outcome;
  SupportAction settled(
    SupportActionOutcome result,
    int now,
    int count,
    int secondCount,
  ) => SupportAction(
    sequence: sequence,
    uptimeMs: uptimeMs,
    kind: kind,
    outcome: result,
    durationMs: now >= uptimeMs ? now - uptimeMs : null,
    count: count == 0 && secondCount == 0 ? this.count : count,
    secondCount: count == 0 && secondCount == 0
        ? this.secondCount
        : secondCount,
  );
  String get description => switch (kind) {
    SupportActionKind.piecesChanged =>
      'Pieces: $count lifted, $secondCount placed',
    SupportActionKind.paint =>
      'Paint gesture${count > 0 ? ' ($count squares)' : ''}',
    SupportActionKind.erase =>
      'Erase gesture${count > 0 ? ' ($count squares)' : ''}',
    SupportActionKind.artworkLoad => 'Open artwork',
    SupportActionKind.artworkSave => 'Save artwork',
    SupportActionKind.settingChange => 'Change board setting',
    SupportActionKind.hint => 'Hint',
    SupportActionKind.undo => 'Undo',
    SupportActionKind.newGame => 'New game',
    SupportActionKind.coaching => 'Coaching',
    SupportActionKind.sleep => 'Sleep',
    SupportActionKind.storageRecovery => 'Storage recovery',
    SupportActionKind.enterLitchcraft => 'Enter Litchcraft',
    SupportActionKind.exitLitchcraft => 'Exit Litchcraft',
    SupportActionKind.selectTool => 'Select paint tool',
    SupportActionKind.selectFrame => 'Select frame',
    SupportActionKind.playback => 'Change playback',
    SupportActionKind.fill => 'Fill canvas',
    SupportActionKind.clear => 'Clear canvas',
    SupportActionKind.redo => 'Redo',
    SupportActionKind.effect => 'Apply effect',
    SupportActionKind.editFrames => 'Edit frames',
  };
  String get outcomeLabel => switch (outcome) {
    SupportActionOutcome.appliedOnPhone => 'applied in the app, board unconfirmed',
    SupportActionOutcome.noChange => 'no change',
    _ => outcome.name,
  };
}

class SupportActionHistory {
  SupportActionHistory(this.clock);
  static const capacity = 20;
  final int Function() clock;
  final List<SupportAction> _actions = [];
  int _sequence = 0, omitted = 0;
  List<SupportAction> get actions => List.unmodifiable(_actions);
  int begin(SupportActionKind kind, {int count = 0, int secondCount = 0}) {
    if (_actions.length == capacity) {
      _actions.removeAt(0);
      omitted++;
    }
    final sequence = ++_sequence;
    _actions.add(
      SupportAction(
        sequence: sequence,
        uptimeMs: clock(),
        kind: kind,
        outcome: SupportActionOutcome.requested,
        count: count,
        secondCount: secondCount,
      ),
    );
    return sequence;
  }

  void finish(
    int sequence,
    SupportActionOutcome outcome, {
    int count = 0,
    int secondCount = 0,
  }) {
    final index = _actions.indexWhere((a) => a.sequence == sequence);
    if (index < 0) return;
    if (!{
      SupportActionOutcome.requested,
      SupportActionOutcome.accepted,
      SupportActionOutcome.unconfirmed,
    }.contains(_actions[index].outcome)) {
      return;
    }
    _actions[index] = _actions[index].settled(
      outcome,
      clock(),
      count,
      secondCount,
    );
  }
}

abstract interface class AppUserActionSink {
  SupportActionHistory get userActions;
  int get supportMonotonicMs;
}

class SupportAppActionSnapshot {
  const SupportAppActionSnapshot({
    required this.actions,
    required this.capturedMs,
    required this.omitted,
  });
  final List<SupportAction> actions;
  final int capturedMs, omitted;
}
