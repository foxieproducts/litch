import 'dart:convert';
import 'dart:math';

import 'user_text.dart';

const int localPlayerProfileSchemaVersion = 1;
const int localPlayerProfileMaximumCount = 16;
const int localPlayerMergeAliasMaximumCount = 256;
const int localPlayerNameMaximumRunes = 40;
const int localPlayerNameMaximumBytes = 120;

final RegExp _localPlayerIdPattern = RegExp(r'^lp_[0-9a-f]{32}$');

bool isValidLocalPlayerId(String value) {
  return _localPlayerIdPattern.hasMatch(value);
}

String generateLocalPlayerId([Random? random]) {
  final Random source = random ?? Random.secure();
  final StringBuffer encoded = StringBuffer('lp_');
  for (int index = 0; index < 16; index += 1) {
    encoded.write(source.nextInt(256).toRadixString(16).padLeft(2, '0'));
  }
  return encoded.toString();
}

class LocalPlayerProfileLimitException implements Exception {
  const LocalPlayerProfileLimitException();
}

enum LocalPlayerChessSide { white, black }

class LocalPlayerProfile {
  factory LocalPlayerProfile({
    required String id,
    required String displayName,
  }) {
    final LocalPlayerProfile profile = LocalPlayerProfile._(
      id: id,
      displayName: normalizeLocalPlayerName(displayName),
    );
    profile.validate();
    return profile;
  }

  const LocalPlayerProfile._({required this.id, required this.displayName});

  final String id;
  final String displayName;

  void validate() {
    if (!isValidLocalPlayerId(id)) {
      throw const FormatException('Invalid local player identity.');
    }
    if (displayName != normalizeLocalPlayerName(displayName)) {
      throw const FormatException('Local player name is not normalized.');
    }
    validateLocalPlayerName(displayName);
  }

  LocalPlayerProfile rename(String name) {
    return LocalPlayerProfile(id: id, displayName: name);
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{'id': id, 'displayName': displayName};
  }

  factory LocalPlayerProfile.fromJson(Map<String, Object?> json) {
    final LocalPlayerProfile profile = LocalPlayerProfile._(
      id: _requiredString(json, 'id'),
      displayName: _requiredString(json, 'displayName'),
    );
    profile.validate();
    return profile;
  }
}

class LocalPlayerDirectory {
  factory LocalPlayerDirectory({
    Iterable<LocalPlayerProfile> profiles = const <LocalPlayerProfile>[],
    Map<String, String> mergedPlayerIds = const <String, String>{},
    String? selectedWhitePlayerId,
    String? selectedBlackPlayerId,
  }) {
    final LocalPlayerDirectory directory = LocalPlayerDirectory._(
      profiles: List<LocalPlayerProfile>.unmodifiable(profiles),
      mergedPlayerIds: Map<String, String>.unmodifiable(mergedPlayerIds),
      selectedWhitePlayerId: selectedWhitePlayerId,
      selectedBlackPlayerId: selectedBlackPlayerId,
    );
    directory.validate();
    return directory;
  }

  const LocalPlayerDirectory._({
    required this.profiles,
    required this.mergedPlayerIds,
    required this.selectedWhitePlayerId,
    required this.selectedBlackPlayerId,
  });

  factory LocalPlayerDirectory.empty() => LocalPlayerDirectory();

  final List<LocalPlayerProfile> profiles;
  final Map<String, String> mergedPlayerIds;

  /// The person assigned to the current and future physical-board games.
  /// A null value deliberately means Guest; it is not an unknown attribution.
  final String? selectedWhitePlayerId;
  final String? selectedBlackPlayerId;

  void validate() {
    if (profiles.length > localPlayerProfileMaximumCount) {
      throw const FormatException('Too many local player profiles.');
    }
    if (mergedPlayerIds.length > localPlayerMergeAliasMaximumCount) {
      throw const FormatException('Too many merged local player identities.');
    }

    final Set<String> profileIds = <String>{};
    final Set<String> foldedNames = <String>{};
    for (final LocalPlayerProfile profile in profiles) {
      profile.validate();
      if (!profileIds.add(profile.id)) {
        throw const FormatException('Duplicate local player identity.');
      }
      if (!foldedNames.add(_foldPlayerName(profile.displayName))) {
        throw const FormatException('Duplicate local player name.');
      }
    }

    for (final MapEntry<String, String> alias in mergedPlayerIds.entries) {
      if (!isValidLocalPlayerId(alias.key) ||
          !isValidLocalPlayerId(alias.value) ||
          alias.key == alias.value ||
          profileIds.contains(alias.key)) {
        throw const FormatException('Invalid merged player identity.');
      }
      if (_resolveProfileId(alias.key, profileIds) == null) {
        throw const FormatException('Merged player identity is unresolved.');
      }
    }

    for (final String? selected in <String?>[
      selectedWhitePlayerId,
      selectedBlackPlayerId,
    ]) {
      if (selected != null && !profileIds.contains(selected)) {
        throw const FormatException('Selected local player does not exist.');
      }
    }
  }

  LocalPlayerProfile? profileById(String id) {
    final String? resolvedId = resolveProfileId(id);
    if (resolvedId == null) {
      return null;
    }
    for (final LocalPlayerProfile profile in profiles) {
      if (profile.id == resolvedId) {
        return profile;
      }
    }
    return null;
  }

  String? resolveProfileId(String id) {
    if (!isValidLocalPlayerId(id)) {
      return null;
    }
    return _resolveProfileId(
      id,
      profiles.map((LocalPlayerProfile profile) => profile.id).toSet(),
    );
  }

  LocalPlayerDirectory createProfile({
    required String id,
    required String displayName,
  }) {
    if (profiles.length >= localPlayerProfileMaximumCount) {
      throw StateError('The local player profile limit has been reached.');
    }
    final LocalPlayerProfile profile = LocalPlayerProfile(
      id: id,
      displayName: displayName,
    );
    _requireUniqueName(profile.displayName);
    if (resolveProfileId(profile.id) != null ||
        mergedPlayerIds.containsKey(profile.id)) {
      throw const FormatException('Local player identity is already in use.');
    }
    return _copyWith(profiles: <LocalPlayerProfile>[...profiles, profile]);
  }

  LocalPlayerDirectory renameProfile({
    required String id,
    required String displayName,
  }) {
    final int index = profiles.indexWhere(
      (LocalPlayerProfile profile) => profile.id == id,
    );
    if (index < 0) {
      throw StateError('The local player profile does not exist.');
    }
    _requireUniqueName(displayName, excludingId: id);
    final List<LocalPlayerProfile> renamed = List<LocalPlayerProfile>.of(
      profiles,
    );
    renamed[index] = profiles[index].rename(displayName);
    return _copyWith(profiles: renamed);
  }

  LocalPlayerDirectory selectPlayer({
    required LocalPlayerChessSide side,
    String? profileId,
  }) {
    if (profileId != null && profileById(profileId) == null) {
      throw StateError('The selected local player profile does not exist.');
    }
    final String? resolved = profileId == null
        ? null
        : resolveProfileId(profileId);
    return _copyWith(
      selectedWhitePlayerId: side == LocalPlayerChessSide.white
          ? resolved
          : selectedWhitePlayerId,
      selectedBlackPlayerId: side == LocalPlayerChessSide.black
          ? resolved
          : selectedBlackPlayerId,
    );
  }

  /// Home assigns a person to a slot; it never renames a person's history.
  LocalPlayerDirectory assignName({
    required LocalPlayerChessSide side,
    String? displayName,
    String Function()? nextId,
  }) {
    if (displayName == null) return selectPlayer(side: side);
    final name = normalizeLocalPlayerName(displayName);
    final matches = profiles
        .where(
          (profile) =>
              _foldPlayerName(profile.displayName) == _foldPlayerName(name),
        )
        .toList();
    if (matches.length > 1) {
      throw const FormatException('Ambiguous local player name.');
    }
    if (matches.isNotEmpty) {
      return selectPlayer(side: side, profileId: matches.single.id);
    }
    if (profiles.length >= localPlayerProfileMaximumCount) {
      throw const LocalPlayerProfileLimitException();
    }
    final id = (nextId ?? generateLocalPlayerId)();
    return createProfile(
      id: id,
      displayName: name,
    ).selectPlayer(side: side, profileId: id);
  }

  LocalPlayerProfile? selectedPlayer(LocalPlayerChessSide side) {
    final id = side == LocalPlayerChessSide.white
        ? selectedWhitePlayerId
        : selectedBlackPlayerId;
    return id == null ? null : profileById(id);
  }

  LocalPlayerDirectory mergeProfiles({
    required String sourceId,
    required String targetId,
  }) {
    if (sourceId == targetId) {
      throw StateError('A local player profile cannot merge into itself.');
    }
    final LocalPlayerProfile? source = profileById(sourceId);
    final LocalPlayerProfile? target = profileById(targetId);
    if (source == null || source.id != sourceId || target == null) {
      throw StateError('Both merged local player profiles must exist.');
    }
    if (!mergedPlayerIds.containsKey(sourceId) &&
        mergedPlayerIds.length >= localPlayerMergeAliasMaximumCount) {
      throw StateError('The merged local player identity limit was reached.');
    }

    final List<LocalPlayerProfile> merged = profiles
        .where((LocalPlayerProfile profile) => profile.id != sourceId)
        .map(
          (LocalPlayerProfile profile) => profile.id == target.id
              ? profile.rename(profile.displayName)
              : profile,
        )
        .toList(growable: false);
    final Map<String, String> aliases = Map<String, String>.of(mergedPlayerIds);
    for (final MapEntry<String, String> alias in mergedPlayerIds.entries.toList(
      growable: false,
    )) {
      if (resolveProfileId(alias.value) == sourceId) {
        aliases[alias.key] = target.id;
      }
    }
    aliases[sourceId] = target.id;

    return _copyWith(
      profiles: merged,
      mergedPlayerIds: aliases,
      selectedWhitePlayerId: selectedWhitePlayerId == sourceId
          ? target.id
          : selectedWhitePlayerId,
      selectedBlackPlayerId: selectedBlackPlayerId == sourceId
          ? target.id
          : selectedBlackPlayerId,
    );
  }

  LocalPlayerDirectory deleteProfile(String id) {
    final LocalPlayerProfile? profile = profileById(id);
    if (profile == null || profile.id != id) {
      throw StateError('The local player profile does not exist.');
    }
    final Set<String> removedIds = <String>{id};
    for (final String aliasId in mergedPlayerIds.keys) {
      if (resolveProfileId(aliasId) == id) {
        removedIds.add(aliasId);
      }
    }

    return _copyWith(
      profiles: profiles.where((LocalPlayerProfile value) => value.id != id),
      mergedPlayerIds: <String, String>{
        for (final MapEntry<String, String> alias in mergedPlayerIds.entries)
          if (!removedIds.contains(alias.key) &&
              !removedIds.contains(alias.value))
            alias.key: alias.value,
      },
      selectedWhitePlayerId: selectedWhitePlayerId == id
          ? null
          : selectedWhitePlayerId,
      selectedBlackPlayerId: selectedBlackPlayerId == id
          ? null
          : selectedBlackPlayerId,
    );
  }

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'schemaVersion': localPlayerProfileSchemaVersion,
      'profiles': profiles
          .map((LocalPlayerProfile profile) => profile.toJson())
          .toList(growable: false),
      'mergedPlayerIds': mergedPlayerIds,
      'selection': <String, Object?>{
        'whitePlayerId': selectedWhitePlayerId,
        'blackPlayerId': selectedBlackPlayerId,
      },
    };
  }

  factory LocalPlayerDirectory.fromJson(Map<String, Object?> json) {
    if (_requiredInt(json, 'schemaVersion') !=
        localPlayerProfileSchemaVersion) {
      throw const FormatException('Unsupported local player profile schema.');
    }
    final List<Object?> rawProfiles = _requiredList(json, 'profiles');
    final Map<String, Object?> rawAliases = _requiredMap(
      json['mergedPlayerIds'],
    );
    final Map<String, Object?> rawSelection = _requiredMap(json['selection']);
    return LocalPlayerDirectory(
      profiles: rawProfiles.map(
        (Object? value) => LocalPlayerProfile.fromJson(_requiredMap(value)),
      ),
      mergedPlayerIds: <String, String>{
        for (final MapEntry<String, Object?> alias in rawAliases.entries)
          alias.key: _stringValue(alias.value, field: 'merged player identity'),
      },
      selectedWhitePlayerId: _optionalString(rawSelection, 'whitePlayerId'),
      selectedBlackPlayerId: _optionalString(rawSelection, 'blackPlayerId'),
    );
  }

  String? _resolveProfileId(String id, Set<String> activeIds) {
    String current = id;
    final Set<String> visited = <String>{};
    while (visited.add(current)) {
      if (activeIds.contains(current)) {
        return current;
      }
      final String? next = mergedPlayerIds[current];
      if (next == null) {
        return null;
      }
      current = next;
    }
    return null;
  }

  void _requireUniqueName(String displayName, {String? excludingId}) {
    final String folded = _foldPlayerName(
      normalizeLocalPlayerName(displayName),
    );
    for (final LocalPlayerProfile profile in profiles) {
      if (profile.id != excludingId &&
          _foldPlayerName(profile.displayName) == folded) {
        throw const FormatException('A player with that name already exists.');
      }
    }
  }

  LocalPlayerDirectory _copyWith({
    Iterable<LocalPlayerProfile>? profiles,
    Map<String, String>? mergedPlayerIds,
    Object? selectedWhitePlayerId = _notProvided,
    Object? selectedBlackPlayerId = _notProvided,
  }) {
    return LocalPlayerDirectory(
      profiles: profiles ?? this.profiles,
      mergedPlayerIds: mergedPlayerIds ?? this.mergedPlayerIds,
      selectedWhitePlayerId: identical(selectedWhitePlayerId, _notProvided)
          ? this.selectedWhitePlayerId
          : selectedWhitePlayerId as String?,
      selectedBlackPlayerId: identical(selectedBlackPlayerId, _notProvided)
          ? this.selectedBlackPlayerId
          : selectedBlackPlayerId as String?,
    );
  }
}

const Object _notProvided = Object();

String normalizeLocalPlayerName(String value) {
  // Reject unsafe input before whitespace normalization can hide controls.
  if (!isSafeDisplayText(value)) {
    throw const FormatException('Local player name contains control text.');
  }
  return value.trim().replaceAll(RegExp(r'\s+'), ' ');
}

void validateLocalPlayerName(String value) {
  if (!isSafeDisplayText(value) ||
      value.isEmpty ||
      value.runes.length > localPlayerNameMaximumRunes ||
      utf8.encode(value).length > localPlayerNameMaximumBytes) {
    throw const FormatException('Local player name is out of range.');
  }
}

String _foldPlayerName(String value) => value.toLowerCase();

String _requiredString(Map<String, Object?> json, String key) {
  return _stringValue(json[key], field: key);
}

String _stringValue(Object? value, {required String field}) {
  if (value is! String || value.isEmpty) {
    throw FormatException('$field must be a non-empty string.');
  }
  return value;
}

String? _optionalString(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value == null) {
    return null;
  }
  return _stringValue(value, field: key);
}

int _requiredInt(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value is! int) {
    throw FormatException('$key must be an integer.');
  }
  return value;
}

List<Object?> _requiredList(Map<String, Object?> json, String key) {
  final Object? value = json[key];
  if (value is! List<Object?>) {
    throw FormatException('$key must be a list.');
  }
  return value;
}

Map<String, Object?> _requiredMap(Object? value) {
  if (value is! Map<String, Object?>) {
    throw const FormatException('Expected an object.');
  }
  return value;
}
