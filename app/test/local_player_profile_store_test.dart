import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/local_player_profile.dart';
import 'package:litch_mobile/src/services/local_player_profile_store.dart';
import 'package:shared_preferences/shared_preferences.dart';

const String _alexId = 'lp_00000000000000000000000000000001';
const String _blairId = 'lp_00000000000000000000000000000002';

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues(<String, Object>{});
  });

  test('saves and loads a local directory', () async {
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    final LocalPlayerProfileStore store = LocalPlayerProfileStore(preferences);
    final LocalPlayerDirectory expected = _directory('Alex');

    await store.save(expected);
    final LocalPlayerProfileLoadResult loaded = await store.load();

    expect(loaded.recoveredFromBackup, isFalse);
    expect(loaded.rejectedStoredData, isFalse);
    expect(loaded.directory.profileById(_alexId)?.displayName, 'Alex');
    expect(loaded.directory.selectedWhitePlayerId, _alexId);
  });

  test(
    'recovers the last valid snapshot without overwriting bad data',
    () async {
      final SharedPreferences preferences =
          await SharedPreferences.getInstance();
      final LocalPlayerProfileStore store = LocalPlayerProfileStore(
        preferences,
      );
      await store.save(_directory('Alex'));
      await store.save(_directory('Alexandra'));
      await preferences.setString('local_player_profiles_v1', '{bad json');

      final LocalPlayerProfileLoadResult loaded = await store.load();

      expect(loaded.recoveredFromBackup, isTrue);
      expect(loaded.rejectedStoredData, isTrue);
      expect(loaded.directory.profileById(_alexId)?.displayName, 'Alex');
      expect(preferences.getString('local_player_profiles_v1'), '{bad json');
    },
  );

  test(
    'fails closed to Guest when both stored snapshots are invalid',
    () async {
      SharedPreferences.setMockInitialValues(<String, Object>{
        'local_player_profiles_v1': '{bad',
        'local_player_profiles_v1_previous': '[]',
      });
      final SharedPreferences preferences =
          await SharedPreferences.getInstance();
      final LocalPlayerProfileStore store = LocalPlayerProfileStore(
        preferences,
      );

      final LocalPlayerProfileLoadResult loaded = await store.load();

      expect(loaded.rejectedStoredData, isTrue);
      expect(loaded.directory.profiles, isEmpty);
      expect(loaded.directory.selectedWhitePlayerId, isNull);
      expect(loaded.directory.selectedBlackPlayerId, isNull);
    },
  );

  test('exports a versioned local-only profile document', () async {
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    final LocalPlayerProfileStore store = LocalPlayerProfileStore(preferences);
    final String exported = store.export(
      directory: _directory('Alex'),
      exportedAt: DateTime.utc(2026, 9, 3, 2, 1),
    );

    final Map<String, Object?> decoded =
        jsonDecode(exported) as Map<String, Object?>;
    expect(decoded['format'], localPlayerProfileExportFormat);
    expect(decoded['exportedAt'], '2026-09-03T02:01:00.000Z');
    expect(
      store.validateExport(exported).profileById(_alexId)?.displayName,
      'Alex',
    );
  });

  test('rejects export timestamps without explicit UTC', () async {
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    final LocalPlayerProfileStore store = LocalPlayerProfileStore(preferences);
    final Map<String, Object?> exported =
        jsonDecode(
              store.export(
                directory: _directory('Alex'),
                exportedAt: DateTime.utc(2026, 9, 3),
              ),
            )
            as Map<String, Object?>;
    exported['exportedAt'] = '2026-09-03T00:00:00';

    expect(
      () => store.validateExport(jsonEncode(exported)),
      throwsFormatException,
    );
  });

  test('clear removes names, aliases, and selections', () async {
    final SharedPreferences preferences = await SharedPreferences.getInstance();
    final LocalPlayerProfileStore store = LocalPlayerProfileStore(preferences);
    await store.save(_directory('Alex'));
    await store.save(_directory('Alexandra'));

    await store.clear();
    await store.clear();
    final LocalPlayerProfileLoadResult loaded = await store.load();

    expect(loaded.rejectedStoredData, isFalse);
    expect(loaded.directory.profiles, isEmpty);
  });
}

LocalPlayerDirectory _directory(String alexName) {
  return LocalPlayerDirectory(
    profiles: <LocalPlayerProfile>[
      LocalPlayerProfile(id: _alexId, displayName: alexName),
      LocalPlayerProfile(id: _blairId, displayName: 'Blair'),
    ],
    selectedWhitePlayerId: _alexId,
  );
}
