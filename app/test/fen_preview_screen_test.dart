import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/fen_preview_screen.dart';

const _fen = 'r3k2r/ppp2ppp/8/3pP3/8/8/PPP2PPP/R2QKQ1R w KQkq d6 0 23';

Widget _harness({TextScaler scaler = TextScaler.noScaling}) => MaterialApp(
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(context).copyWith(textScaler: scaler),
    child: child!,
  ),
  home: const FenPreviewScreen(),
);

Future<void> _enter(WidgetTester tester, String fen) async {
  await tester.enterText(
    find.byKey(const ValueKey<String>('fen-preview-field')),
    fen,
  );
  await tester.ensureVisible(
    find.byKey(const ValueKey<String>('fen-preview-validate')),
  );
  await tester.tap(find.byKey(const ValueKey<String>('fen-preview-validate')));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('validates and previews every FEN field and promoted pieces', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    await tester.pumpWidget(_harness());
    expect(find.bySemanticsLabel(RegExp(r'FEN')), findsWidgets);
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('fen-preview-validate')))
          .height,
      greaterThanOrEqualTo(48),
    );
    await _enter(tester, _fen);

    expect(
      find.text('Preview on this device. Your board is unchanged.'),
      findsOneWidget,
    );
    expect(find.text(_fen), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-d1')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-f1')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e1')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e8')), findsOneWidget);
    expect(find.bySemanticsLabel('d1, P1 queen'), findsOneWidget);
    expect(find.bySemanticsLabel('f1, P1 queen'), findsOneWidget);
    expect(find.bySemanticsLabel('e1, P1 king'), findsOneWidget);
    expect(find.bySemanticsLabel('e8, P2 king'), findsOneWidget);
    expect(find.text('Side to move: P1 (W)'), findsOneWidget);
    expect(
      find.text(
        'Castling rights: White king-side, White queen-side, Black king-side, Black queen-side',
      ),
      findsOneWidget,
    );
    expect(find.text('En-passant target: d6'), findsOneWidget);
    expect(
      find.text('Half-move count: 0 since a pawn move or capture'),
      findsOneWidget,
    );
    expect(find.text('Move number: 23'), findsOneWidget);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('fen-preview-board')))
          .label,
      contains('P1 (W) to move'),
    );
    semantics.dispose();
  });

  for (final invalid in <String>[
    'not a fen',
    '4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1',
    '4k3/8/8/8/8/8/8/4K3 w K - 0 1',
    '8/8/8/8/8/8/4k3/4K3 w - - 0 1',
    '4k3/8/8/8/8/8/4r3/4K3 b - - 0 1',
  ]) {
    testWidgets('rejects malformed or impossible FEN: $invalid', (
      tester,
    ) async {
      await tester.pumpWidget(_harness());
      await _enter(tester, invalid);

      expect(
        find.text(
          'Enter a valid, internally consistent standard-chess FEN with all six fields.',
        ),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('fen-preview-board')),
        findsNothing,
      );
    });
  }

  testWidgets('edit again retains the draft and clears the prior preview', (
    tester,
  ) async {
    await tester.pumpWidget(_harness());
    await _enter(tester, _fen);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('fen-preview-edit-again')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('fen-preview-edit-again')),
    );
    await tester.pump();

    final field = tester.widget<TextField>(
      find.byKey(const ValueKey<String>('fen-preview-field')),
    );
    expect(field.controller!.text, _fen);
    expect(
      find.byKey(const ValueKey<String>('fen-preview-board')),
      findsNothing,
    );
  });

  testWidgets('entry and preview remain usable at 200 percent on 320px', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_harness(scaler: const TextScaler.linear(2)));

    expect(tester.takeException(), isNull);
    final validate = find.byKey(const ValueKey<String>('fen-preview-validate'));
    expect(tester.getSize(validate).height, greaterThanOrEqualTo(48));
    await _enter(tester, _fen);
    expect(tester.takeException(), isNull);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('fen-preview-edit-again')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('fen-preview-edit-again')))
          .height,
      greaterThanOrEqualTo(48),
    );
  });

  testWidgets('validation guidance wraps with keyboard at 200 percent', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_harness(scaler: const TextScaler.linear(2)));

    await _enter(tester, 'not a fen');
    tester.view.viewInsets = const FakeViewPadding(bottom: 280);
    addTearDown(() => tester.view.resetViewInsets());
    await tester.pump();

    const message =
        'Enter a valid, internally consistent standard-chess FEN with all six fields.';
    final Finder error = find.text(message);
    expect(error, findsOneWidget);
    final Text rendered = tester.widget<Text>(error);
    expect(rendered.overflow, isNot(TextOverflow.ellipsis));
    expect(tester.takeException(), isNull);
  });
}
