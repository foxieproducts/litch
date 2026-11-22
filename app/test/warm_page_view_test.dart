import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/widgets/warm_page_view.dart';

void main() {
  testWidgets(
    'all four bounded shells layout before first selection and stay warm',
    (tester) async {
      final controller = PageController(initialPage: 1);
      addTearDown(controller.dispose);
      final layouts = List.filled(4, 0);
      final builds = List.filled(4, 0);
      await tester.pumpWidget(
        MaterialApp(
          home: WarmPageView(
            controller: controller,
            children: [
              for (var index = 0; index < 4; index++)
                Builder(
                  builder: (_) {
                    builds[index]++;
                    return LayoutBuilder(
                      builder: (_, constraints) {
                        layouts[index]++;
                        return SizedBox.expand(key: ValueKey('page-$index'));
                      },
                    );
                  },
                ),
            ],
          ),
        ),
      );
      expect(builds, [1, 1, 1, 1]);
      expect(layouts, [1, 1, 1, 1]);
      final elements = [
        for (var i = 0; i < 4; i++)
          tester.element(find.byKey(ValueKey('page-$i'), skipOffstage: false)),
      ];
      for (final page in [2, 3, 2, 1, 0]) {
        controller.jumpToPage(page);
        await tester.pumpAndSettle();
        expect(controller.page, page);
        expect(builds, [1, 1, 1, 1]);
        expect(layouts, [1, 1, 1, 1]);
        for (var i = 0; i < 4; i++) {
          expect(
            tester.element(
              find.byKey(ValueKey('page-$i'), skipOffstage: false),
            ),
            same(elements[i]),
          );
        }
      }
      expect(tester.takeException(), isNull);
    },
  );
}
