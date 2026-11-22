import 'support/memory_ota_target_store.dart';
import 'package:flutter/services.dart';
import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';

class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// Only discovery is suppressed; production disconnected shell chrome and
/// editor navigation/layout are exercised without a native radio.
class _OfflineShellBle extends LitchBleService {
  _OfflineShellBle()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _NoopBle());
  @override
  BleAvailability get bleAvailability => BleAvailability.unsupported;
  @override
  Future<void> startScan() async {}
}

void main() {
  for (final sample in <(String, Size, EdgeInsets, TargetPlatform)>[
    (
      'ios-portrait',
      Size(390, 844),
      EdgeInsets.fromLTRB(0, 47, 0, 34),
      TargetPlatform.iOS,
    ),
    (
      'ios-landscape',
      Size(844, 390),
      EdgeInsets.fromLTRB(47, 0, 0, 21),
      TargetPlatform.iOS,
    ),
    (
      'ios-landscape-clear-left',
      Size(844, 390),
      EdgeInsets.fromLTRB(0, 0, 47, 21),
      TargetPlatform.iOS,
    ),
    (
      'android-landscape-camera-left',
      Size(915, 412),
      EdgeInsets.fromLTRB(24, 24, 0, 0),
      TargetPlatform.android,
    ),
    (
      'android-portrait',
      Size(412, 915),
      EdgeInsets.fromLTRB(0, 24, 0, 24),
      TargetPlatform.android,
    ),
    (
      'android-landscape',
      Size(915, 412),
      EdgeInsets.fromLTRB(0, 24, 24, 0),
      TargetPlatform.android,
    ),
    (
      'compact-landscape',
      Size(540, 360),
      EdgeInsets.fromLTRB(20, 24, 12, 16),
      TargetPlatform.android,
    ),
  ]) {
    for (final scale in <double>[1, 2]) {
      testWidgets('Artwork inspector ${sample.$1} text $scale', (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final ble = _OfflineShellBle();
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await library.dismissWelcome();
        await controller.setBrush(color: 0x66d2bd);
        controller.beginStroke();
        controller.paintSquares([0, 9, 18, 27, 36, 45, 54, 63]);
        controller.endStroke();
        await library.save('Evening light');
        await library.duplicate(library.artworks.single);
        await library.rename(library.artworks.first, 'Corner study');
        await tester.binding.setSurfaceSize(sample.$2);
        tester.view.physicalSize = sample.$2;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(() => tester.binding.setSurfaceSize(null));
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        await tester.pumpWidget(
          RepaintBoundary(
            key: const ValueKey('artwork-review'),
            child: MaterialApp(
              debugShowCheckedModeBanner: false,
              theme: buildLitchTheme().copyWith(platform: sample.$4),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: TextScaler.linear(scale),
                  padding: sample.$3,
                  viewPadding: sample.$3,
                  disableAnimations: true,
                ),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: ble,
                craftController: controller,
                craftLibrary: library,
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(
          find
              .descendant(
                of: find.byKey(const ValueKey('disconnected-navigation')),
                matching: find.byType(LitchcraftIdentityGlyph),
              )
              .first,
        );
        await tester.pumpAndSettle();
        final canvas = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        await tester.tap(find.byKey(const ValueKey('craft-artwork-menu')));
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        expect(
          tester.getRect(find.byKey(const ValueKey('litchcraft-canvas'))),
          canvas,
        );
        await captureHealthReview(
          tester,
          '${sample.$1}-${scale.toInt()}00',
          boundaryFinder: find.byKey(const ValueKey('artwork-review')),
        );
        expect(find.byKey(const ValueKey('craft-send-to-litch')), findsNothing);
        expect(find.text('Send to Litch'), findsNothing);
        final panel = find.byKey(const ValueKey('craft-artwork-panel'));
        expect(
          find.descendant(of: panel, matching: find.text('Artwork')),
          findsNothing,
        );
        for (final key in ['craft-library', 'craft-save', 'craft-share']) {
          final button = find.byKey(ValueKey(key));
          expect(button.hitTestable(), findsOneWidget);
          final rect = tester.getRect(button);
          expect(rect.shortestSide, greaterThanOrEqualTo(48 - .001));
          expect(canvas.overlaps(rect), isFalse);
        }
        final semantics = tester.ensureSemantics();
        expect(
          tester
              .getSemantics(find.byKey(const ValueKey('craft-library')))
              .label,
          contains('Evening light, Open saved artwork'),
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('craft-save'))).label,
          contains('Save'),
        );
        expect(
          tester.getSemantics(find.byKey(const ValueKey('craft-share'))).label,
          contains('Export / Share'),
        );
        FocusManager.instance.primaryFocus?.unfocus();
        await tester.pump();
        // Real focus traversal must reach the selector before local Save/Share.
        final reached = <String>[];
        for (var i = 0; i < 40 && reached.length < 3; i++) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pump();
          final focused = FocusManager.instance.primaryFocus?.context;
          if (focused is Element) {
            for (final key in ['craft-library', 'craft-save', 'craft-share']) {
              final button = find.byKey(ValueKey(key)).evaluate().single;
              var within = identical(focused, button);
              focused.visitAncestorElements((ancestor) {
                if (identical(ancestor, button)) within = true;
                return !within;
              });
              if (within && !reached.contains(key)) reached.add(key);
            }
          }
        }
        expect(reached, ['craft-library', 'craft-save', 'craft-share']);
        await tester.tap(find.byKey(const ValueKey('craft-save')));
        await tester.pumpAndSettle();
        expect(find.text('Saved on this device'), findsOneWidget);
        expect(tester.takeException(), isNull);
        final notice = tester.getRect(
          find.byKey(const ValueKey('craft-status-card')),
        );
        expect(canvas.intersect(notice), notice);
        expect(
          tester.getSemantics(find.text('Saved on this device')).label,
          contains('Saved on this device'),
        );
        expect(
          find.descendant(of: panel, matching: find.byType(Scrollable)),
          findsNothing,
        );
        await captureHealthReview(
          tester,
          '${sample.$1}-${scale.toInt()}00-saved',
          boundaryFinder: find.byKey(const ValueKey('artwork-review')),
        );
        await tester.tap(find.byKey(const ValueKey('craft-library')));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const ValueKey('craft-library-search')),
          findsOneWidget,
        );
        final corner = library.artworks.singleWhere(
          (artwork) => artwork.name == 'Corner study',
        );
        final row = find.byKey(ValueKey('craft-artwork-${corner.id}'));
        final rename = find.byKey(
          ValueKey('craft-rename-artwork-${corner.id}'),
        );
        final share = find.byKey(ValueKey('craft-share-artwork-${corner.id}'));
        final delete = find.byKey(
          ValueKey('craft-delete-artwork-${corner.id}'),
        );
        for (final target in <Finder>[row, rename, share, delete]) {
          expect(target, findsOneWidget);
          await tester.ensureVisible(target);
          await tester.pump();
          expect(
            target.hitTestable(),
            findsOneWidget,
            reason:
                'Every artwork row action must remain reachable in compact landscape and at 200% text.',
          );
        }
        expect(
          find.descendant(
            of: rename,
            matching: find.byIcon(Icons.edit_outlined),
          ),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: delete,
            matching: find.byIcon(Icons.delete_outline_rounded),
          ),
          findsOneWidget,
        );
        expect(
          find.descendant(of: share, matching: find.byType(Text)),
          findsNothing,
        );
        expect(find.text('Open'), findsNothing);
        expect(find.text('Export / Share'), findsNothing);
        final rowRect = tester.getRect(row);
        for (final action in <Finder>[rename, delete, share]) {
          final rect = tester.getRect(action);
          expect(rect.shortestSide, greaterThanOrEqualTo(48));
          expect(rowRect.intersect(rect), rect);
        }
        final deleteRect = tester.getRect(delete);
        final shareRect = tester.getRect(share);
        expect(
          deleteRect.top < shareRect.top ||
              (deleteRect.top == shareRect.top &&
                  deleteRect.left < shareRect.left),
          isTrue,
          reason: 'Delete must precede Share in reading order.',
        );
        expect(find.byType(Divider), findsNothing);
        expect(tester.takeException(), isNull);
        await captureHealthReview(
          tester,
          '${sample.$1}-${scale.toInt()}00-library-entry',
          boundaryFinder: find.byKey(const ValueKey('artwork-review')),
        );
        await tester.tap(delete);
        await tester.pumpAndSettle();
        expect(
          find.text(
            'Corner study\n\nThis removes the saved copy from this device. Your current canvas stays.',
          ),
          findsOneWidget,
        );
        await tester.tap(find.text('Cancel'));
        await tester.pumpAndSettle();
        expect(library.artworks, hasLength(2));
        semantics.dispose();
        await tester.pumpWidget(const SizedBox());
        await library.flush();
        library.dispose();
        controller.dispose();
        await ble.dispose();
      });
    }
  }
}
