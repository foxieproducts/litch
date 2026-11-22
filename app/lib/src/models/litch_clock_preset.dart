abstract final class LitchClockPresets {
  static const Map<int, String> labels = <int, String>{
    0: 'Off',
    1: '1 min',
    2: '3 + 2',
    3: '5 min',
    4: '10 min',
    5: '15 + 10',
    6: '30 min',
  };

  static const Map<int, int> baseSeconds = <int, int>{
    0: 0,
    1: 60,
    2: 180,
    3: 300,
    4: 600,
    5: 900,
    6: 1800,
  };

  static bool contains(int setting) => labels.containsKey(setting);
}
