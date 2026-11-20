import 'dart:convert';

const int _uint32Max = 0xffffffff;

bool _uint32(Object? value) =>
    value is int && value >= 0 && value <= _uint32Max;
bool _slot(int value) => value == 0 || value == 1;
bool _sequence(int value) => value > 0 && value < _uint32Max;
bool _nonce(String value) =>
    RegExp(r'^[0-9a-f]{16}$').hasMatch(value) && value != '0000000000000000';

class OtaAttemptSelector {
  const OtaAttemptSelector({
    required this.sequence,
    required this.state,
    required this.crcValid,
  });

  final int sequence;
  final int state;
  final bool crcValid;
}

class OtaAttemptAdmission {
  const OtaAttemptAdmission({
    required this.attempt,
    required this.phase,
    required this.sourceSlot,
    required this.targetSlot,
    required this.sourceSequence,
    required this.expectedSequence,
  });

  final int attempt;
  final int phase;
  final int sourceSlot;
  final int targetSlot;
  final int sourceSequence;
  final int expectedSequence;
}

/// Immutable identity captured from this boot's admitted Receiving transaction.
class OtaAttemptIdentity {
  const OtaAttemptIdentity({
    required this.preBootNonce,
    required this.attempt,
    required this.sourceSlot,
    required this.sourceSector,
    required this.sourceSequence,
    required this.targetSlot,
    required this.targetSector,
    required this.expectedSequence,
  });

  final String preBootNonce;
  final int attempt;
  final int sourceSlot;
  final int sourceSector;
  final int sourceSequence;
  final int targetSlot;
  final int targetSector;
  final int expectedSequence;

  bool get isValid =>
      _nonce(preBootNonce) &&
      attempt > 0 &&
      _uint32(attempt) &&
      _slot(sourceSlot) &&
      _slot(targetSlot) &&
      sourceSlot != targetSlot &&
      _slot(sourceSector) &&
      _slot(targetSector) &&
      sourceSector != targetSector &&
      _sequence(sourceSequence) &&
      _sequence(expectedSequence) &&
      expectedSequence > sourceSequence &&
      (sourceSequence - 1) % 2 == sourceSlot &&
      (expectedSequence - 1) % 2 == targetSlot;
}

/// Exact version-1 proof on the existing OTA status owner. Request/session/Hello
/// correlation belongs to that owner; parsing never grants freshness itself.
class OtaAttemptProof {
  const OtaAttemptProof({
    required this.requestId,
    required this.bootNonce,
    required this.runtimeAccepted,
    required this.sample,
    required this.ageMs,
    required this.runningSlot,
    required this.selectedSlot,
    required this.selectedSector,
    required this.selectedSequence,
    required this.runningState,
    required this.selectors,
    required this.admission,
  });

  final int requestId;
  final String bootNonce;
  final bool runtimeAccepted;
  final int sample;
  final int ageMs;
  final int runningSlot;
  final int selectedSlot;
  final int selectedSector;
  final int selectedSequence;
  final int runningState;
  final List<OtaAttemptSelector> selectors;
  final OtaAttemptAdmission admission;

  bool get hasCoherentSelection {
    if (sample == 0 ||
        selectors.length != 2 ||
        !_slot(selectedSector) ||
        !_sequence(selectedSequence) ||
        runningSlot != selectedSlot ||
        (selectedSequence - 1) % 2 != selectedSlot) {
      return false;
    }
    final selected = selectors[selectedSector];
    if (!selected.crcValid ||
        selected.sequence != selectedSequence ||
        (selected.state != 1 && selected.state != 2) ||
        runningState != selected.state) {
      return false;
    }
    // Mirror the pinned SDK's selection, including its tie preference for 0.
    int? selectedBySdk;
    for (int index = 0; index < 2; ++index) {
      final entry = selectors[index];
      if (!entry.crcValid ||
          entry.sequence == _uint32Max ||
          entry.state == 3 ||
          entry.state == 4) {
        continue;
      }
      if (selectedBySdk == null ||
          entry.sequence > selectors[selectedBySdk].sequence) {
        selectedBySdk = index;
      }
    }
    return selectedBySdk == selectedSector;
  }

  static bool isUnavailable(Map<String, Object?> payload) =>
      payload.length == 3 &&
      payload['op'] is int &&
      payload['op'] == 1 &&
      _uint32(payload['q']) &&
      (payload['q']! as int) > 0 &&
      payload['e'] == 'unavailable';

  static OtaAttemptProof? tryParse(Map<String, Object?> payload) {
    if (payload.length != 6 ||
        !payload.keys.toSet().containsAll(['op', 'q', 'n', 'v', 's', 'a']) ||
        payload['op'] is! int ||
        payload['op'] != 1 ||
        !_uint32(payload['q']) ||
        payload['q'] == 0 ||
        payload['n'] is! String ||
        !_nonce(payload['n']! as String) ||
        payload['v'] is! int ||
        (payload['v'] != 0 && payload['v'] != 1)) {
      return null;
    }
    final s = payload['s'];
    final a = payload['a'];
    if (s is! List ||
        s.length != 13 ||
        !s.every(_uint32) ||
        a is! List ||
        a.length != 6 ||
        !a.every(_uint32)) {
      return null;
    }
    final values = s.cast<int>();
    final admission = a.cast<int>();
    if (!_slot(values[2]) ||
        !_slot(values[3]) ||
        !_slot(values[4]) ||
        values[6] > 2 ||
        !_rawState(values[8]) ||
        !_rawState(values[11]) ||
        values[9] > 1 ||
        values[12] > 1 ||
        admission[1] > 4) {
      return null;
    }
    if (admission[1] == 0) {
      if (admission[0] != 0 ||
          admission[2] != 255 ||
          admission[3] != 255 ||
          admission[4] != 0 ||
          admission[5] != 0) {
        return null;
      }
    } else if (admission[0] == 0 ||
        !_slot(admission[2]) ||
        !_slot(admission[3]) ||
        admission[2] == admission[3] ||
        !_sequence(admission[4]) ||
        !_sequence(admission[5]) ||
        admission[5] <= admission[4] ||
        (admission[4] - 1) % 2 != admission[2] ||
        (admission[5] - 1) % 2 != admission[3]) {
      return null;
    }
    // Bound the actual encoded status, including keys and punctuation.
    if (utf8.encode(jsonEncode(payload)).length > 223) {
      return null;
    }
    return OtaAttemptProof(
      requestId: payload['q']! as int,
      bootNonce: payload['n']! as String,
      runtimeAccepted: payload['v'] == 1,
      sample: values[0],
      ageMs: values[1],
      runningSlot: values[2],
      selectedSlot: values[3],
      selectedSector: values[4],
      selectedSequence: values[5],
      runningState: values[6],
      selectors: List.unmodifiable([
        for (final offset in [7, 10])
          OtaAttemptSelector(
            sequence: values[offset],
            state: values[offset + 1],
            crcValid: values[offset + 2] == 1,
          ),
      ]),
      admission: OtaAttemptAdmission(
        attempt: admission[0],
        phase: admission[1],
        sourceSlot: admission[2],
        targetSlot: admission[3],
        sourceSequence: admission[4],
        expectedSequence: admission[5],
      ),
    );
  }

  static bool _rawState(int value) => value <= 4 || value == _uint32Max;
}
