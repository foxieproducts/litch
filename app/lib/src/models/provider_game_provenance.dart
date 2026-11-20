/// Archive provenance only. Unknown provider observations belong to the durable
/// reconciliation checkpoint, never to a confirmed archive result.
enum ProviderResultCategory {
  ongoing,
  checkmate,
  resignation,
  draw,
  timeout,
  aborted,
  providerDefined,
}

class ProviderGameProvenance {
  const ProviderGameProvenance({
    required this.provider,
    required this.gameId,
    required this.accountId,
    required this.localSide,
    required this.rawStatus,
    this.winner,
  });

  final String provider;
  final String gameId;
  final String accountId;
  final String localSide;
  final String rawStatus;
  final String? winner;

  ProviderResultCategory get category => switch (rawStatus) {
    'created' || 'started' => ProviderResultCategory.ongoing,
    'mate' => ProviderResultCategory.checkmate,
    'resign' => ProviderResultCategory.resignation,
    'stalemate' ||
    'draw' ||
    'insufficientMaterialClaim' => ProviderResultCategory.draw,
    'timeout' || 'outoftime' => ProviderResultCategory.timeout,
    'aborted' || 'noStart' => ProviderResultCategory.aborted,
    'cheat' ||
    'unknownFinish' ||
    'variantEnd' => ProviderResultCategory.providerDefined,
    _ => throw const FormatException(
      'Unconfirmed provider status requires recovery.',
    ),
  };

  void validate() {
    if (provider != 'lichess' ||
        !RegExp(r'^[A-Za-z0-9]{1,64}$').hasMatch(gameId) ||
        !_boundedAscii(accountId, 64) ||
        !_boundedAscii(rawStatus, 64) ||
        (localSide != 'white' && localSide != 'black') ||
        (winner != null && winner != 'white' && winner != 'black')) {
      throw const FormatException('Invalid provider archive provenance.');
    }
    final result = category;
    if ((result == ProviderResultCategory.ongoing ||
            result == ProviderResultCategory.draw) &&
        winner != null) {
      throw const FormatException(
        'Provider status contradicts its winning side.',
      );
    }
  }

  Map<String, Object?> toJson() => {
    'provider': provider,
    'gameId': gameId,
    'accountId': accountId,
    'localSide': localSide,
    'rawStatus': rawStatus,
    if (winner != null) 'winner': winner,
  };

  factory ProviderGameProvenance.fromJson(Map<String, Object?> json) {
    String requiredString(String key) {
      final value = json[key];
      if (value is! String) throw FormatException('Missing provider $key.');
      return value;
    }

    final winner = json['winner'];
    if (winner != null && winner is! String) {
      throw const FormatException('Invalid provider winner.');
    }
    final value = ProviderGameProvenance(
      provider: requiredString('provider'),
      gameId: requiredString('gameId'),
      accountId: requiredString('accountId'),
      localSide: requiredString('localSide'),
      rawStatus: requiredString('rawStatus'),
      winner: winner as String?,
    );
    value.validate();
    return value;
  }
}

bool _boundedAscii(String value, int maximum) =>
    value.isNotEmpty &&
    value.length <= maximum &&
    value.codeUnits.every((unit) => unit >= 0x20 && unit <= 0x7e);
