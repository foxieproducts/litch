import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/local_player_profile.dart';

const String _alexId = 'lp_00000000000000000000000000000001';
const String _blairId = 'lp_00000000000000000000000000000002';
const String _oldAlexId = 'lp_00000000000000000000000000000003';

void main() {
  test('creates a bounded local profile and normalizes its name', () {
    final LocalPlayerDirectory directory = LocalPlayerDirectory.empty()
        .createProfile(id: _alexId, displayName: '  Alex   Fox  ');

    expect(directory.profiles.single.displayName, 'Alex Fox');
    expect(directory.selectedWhitePlayerId, isNull);
    expect(directory.selectedBlackPlayerId, isNull);
  });

  test('generates opaque bounded identities', () {
    final Random random = Random(4);
    final String first = generateLocalPlayerId(random);
    final String second = generateLocalPlayerId(random);

    expect(isValidLocalPlayerId(first), isTrue);
    expect(isValidLocalPlayerId(second), isTrue);
    expect(first, isNot(second));
  });

  test('selection keeps Guest explicit as a null local choice', () {
    LocalPlayerDirectory directory = _twoPlayers();
    directory = directory
        .selectPlayer(side: LocalPlayerChessSide.white, profileId: _alexId)
        .selectPlayer(side: LocalPlayerChessSide.black, profileId: _blairId);

    expect(directory.selectedWhitePlayerId, _alexId);
    expect(directory.selectedBlackPlayerId, _blairId);

    directory = directory.selectPlayer(side: LocalPlayerChessSide.white);
    expect(directory.selectedWhitePlayerId, isNull);
    expect(directory.selectedBlackPlayerId, _blairId);
  });

  test('rename preserves identity and archive-facing references', () {
    final LocalPlayerDirectory directory = _twoPlayers().renameProfile(
      id: _alexId,
      displayName: 'Alexandra',
    );

    expect(directory.profileById(_alexId)?.displayName, 'Alexandra');
    expect(directory.profileById(_alexId)?.id, _alexId);
  });

  test('merge removes the duplicate but resolves its historical identity', () {
    LocalPlayerDirectory directory = _twoPlayers()
        .createProfile(id: _oldAlexId, displayName: 'Old Alex')
        .selectPlayer(side: LocalPlayerChessSide.white, profileId: _oldAlexId);

    directory = directory.mergeProfiles(
      sourceId: _oldAlexId,
      targetId: _alexId,
    );

    expect(directory.profiles, hasLength(2));
    expect(directory.resolveProfileId(_oldAlexId), _alexId);
    expect(directory.profileById(_oldAlexId)?.displayName, 'Alex');
    expect(directory.selectedWhitePlayerId, _alexId);
  });

  test('delete unlinks merged history and returns selected sides to Guest', () {
    LocalPlayerDirectory directory = _twoPlayers()
        .createProfile(id: _oldAlexId, displayName: 'Old Alex')
        .mergeProfiles(sourceId: _oldAlexId, targetId: _alexId)
        .selectPlayer(side: LocalPlayerChessSide.white, profileId: _alexId);

    directory = directory.deleteProfile(_alexId);

    expect(directory.profileById(_alexId), isNull);
    expect(directory.resolveProfileId(_oldAlexId), isNull);
    expect(directory.selectedWhitePlayerId, isNull);
    expect(directory.profiles.single.id, _blairId);
  });

  test('round trip preserves profiles, aliases, and selection', () {
    final LocalPlayerDirectory directory = _twoPlayers()
        .createProfile(id: _oldAlexId, displayName: 'Old Alex')
        .mergeProfiles(sourceId: _oldAlexId, targetId: _alexId)
        .selectPlayer(side: LocalPlayerChessSide.white, profileId: _alexId)
        .selectPlayer(side: LocalPlayerChessSide.black, profileId: _blairId);

    final LocalPlayerDirectory decoded = LocalPlayerDirectory.fromJson(
      directory.toJson(),
    );

    expect(
      decoded.profiles.map((LocalPlayerProfile value) => value.id),
      <String>[_alexId, _blairId],
    );
    expect(decoded.resolveProfileId(_oldAlexId), _alexId);
    expect(decoded.selectedWhitePlayerId, _alexId);
    expect(decoded.selectedBlackPlayerId, _blairId);
  });

  test('rejects duplicate display names without silently merging people', () {
    expect(
      () => _twoPlayers().renameProfile(id: _blairId, displayName: ' alex '),
      throwsFormatException,
    );
  });

  test('rejects missing selections and malformed merge cycles', () {
    expect(
      () => _twoPlayers().selectPlayer(
        side: LocalPlayerChessSide.white,
        profileId: _oldAlexId,
      ),
      throwsStateError,
    );
    expect(
      () => LocalPlayerDirectory(
        profiles: <LocalPlayerProfile>[_profile(_alexId, 'Alex')],
        mergedPlayerIds: <String, String>{
          _oldAlexId: _blairId,
          _blairId: _oldAlexId,
        },
      ),
      throwsFormatException,
    );
  });

  test('enforces profile count and name bounds', () {
    LocalPlayerDirectory directory = LocalPlayerDirectory.empty();
    for (int index = 0; index < localPlayerProfileMaximumCount; index += 1) {
      directory = directory.createProfile(
        id: 'lp_${index.toRadixString(16).padLeft(32, '0')}',
        displayName: 'Player $index',
      );
    }

    expect(
      () => directory.createProfile(
        id: 'lp_${List<String>.filled(32, 'f').join()}',
        displayName: 'One too many',
      ),
      throwsStateError,
    );
    expect(
      () => validateLocalPlayerName(
        List<String>.filled(localPlayerNameMaximumRunes + 1, 'x').join(),
      ),
      throwsFormatException,
    );
  });
}

LocalPlayerDirectory _twoPlayers() {
  return LocalPlayerDirectory(
    profiles: <LocalPlayerProfile>[
      _profile(_alexId, 'Alex'),
      _profile(_blairId, 'Blair'),
    ],
  );
}

LocalPlayerProfile _profile(String id, String displayName) {
  return LocalPlayerProfile(id: id, displayName: displayName);
}
