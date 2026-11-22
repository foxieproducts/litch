import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';

void main() {
  test('Litchcraft displays LED drive values as sRGB', () {
    for (final (rgb, expected) in [
      (0x000000, 0xff000000),
      (0xff7430, 0xffffb478),
      (0x22c3e2, 0xff66e3f2),
      (0x808080, 0xffbcbcbc),
      (0xffffff, 0xffffffff),
    ]) {
      expect(litchcraftPreviewColor(rgb).toARGB32(), expected);
    }
    expect(
      litchSwatchMarkColor(litchcraftPreviewColor(0xff3b00)).toARGB32(),
      0xff000000,
    );
    expect(
      litchSwatchMarkColor(litchcraftPreviewColor(0xffffff)).toARGB32(),
      0xff000000,
    );
    expect(
      litchSwatchMarkColor(litchcraftPreviewColor(0x000000)).toARGB32(),
      0xffffffff,
    );
  });
}
