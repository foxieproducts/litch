class LitchSetting {
  const LitchSetting({
    required this.key,
    required this.type,
    required this.category,
    required this.value,
    required this.defaultValue,
    this.min,
    this.max,
    this.options = const <String>[],
    this.description = '',
  });

  final String key;
  final String type;
  final String category;
  final Object value;
  final Object defaultValue;
  final int? min;
  final int? max;
  final List<String> options;
  final String description;
}
