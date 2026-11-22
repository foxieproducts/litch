import 'dart:io';
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/external_link_launcher.dart';

void main() {
  test('opens only the exact approved destinations', () async {
    final List<String> calls = <String>[];
    final ExternalLinkLauncher launcher = ExternalLinkLauncher(
      invoke: (String url) async {
        calls.add(url);
        return true;
      },
    );

    expect(
      await launcher.open(LitchExternalLinks.product),
      ExternalLinkLaunchResult.opened,
    );
    expect(
      await launcher.open(LitchExternalLinks.privacyPolicy),
      ExternalLinkLaunchResult.opened,
    );
    expect(
      await launcher.open(LitchExternalLinks.github),
      ExternalLinkLaunchResult.opened,
    );
    expect(
      await launcher.open(LitchExternalLinks.duckChess),
      ExternalLinkLaunchResult.opened,
    );
    expect(calls, <String>[
      'https://foxieproducts.com/litch',
      'https://foxieproducts.com/policies/privacy-policy',
      'https://github.com/foxieproducts',
      'https://duckchess.com',
    ]);

    expect(
      await launcher.open(Uri.parse('http://foxieproducts.com/litch')),
      ExternalLinkLaunchResult.disallowed,
    );
    expect(
      await launcher.open(
        Uri.parse('https://foxieproducts.com/litch?redirect=elsewhere'),
      ),
      ExternalLinkLaunchResult.disallowed,
    );
    for (final url in [
      'https://foxieproducts.com/pages/litch-app-privacy',
      'https://foxieproducts.com/policies/privacy-policy?redirect=x',
      'http://duckchess.com',
      'https://duckchess.com/',
      'https://duckchess.com?redirect=x',
      'https://duckchess.com/#x',
      'https://duckchess.com.evil.test',
      'https://user@duckchess.com',
    ]) {
      expect(
        await launcher.open(Uri.parse(url)),
        ExternalLinkLaunchResult.disallowed,
      );
    }
    expect(calls, hasLength(4));
  });

  test('both native adapters pin the same exact external destinations', () {
    final paths = [
      'android/app/src/main/kotlin/com/foxieproducts/litch_mobile/MainActivity.kt',
      'ios/Runner/AppDelegate.swift',
    ];
    for (final path in paths) {
      final source = File(path).readAsStringSync();
      final urls = RegExp(
        r'"(https://(?:foxieproducts|github|duckchess)[^"]*)"',
      ).allMatches(source).map((match) => match.group(1)).toSet();
      expect(urls, {
        'https://foxieproducts.com/litch',
        'https://foxieproducts.com/policies/privacy-policy',
        'https://github.com/foxieproducts',
        'https://duckchess.com',
      }, reason: path);
    }
  });

  test('coalesces a second tap while a browser launch is pending', () async {
    final Completer<bool> pending = Completer<bool>();
    int calls = 0;
    final ExternalLinkLauncher launcher = ExternalLinkLauncher(
      invoke: (String url) {
        ++calls;
        return pending.future;
      },
    );

    final Future<ExternalLinkLaunchResult> first = launcher.open(
      LitchExternalLinks.product,
    );
    expect(
      await launcher.open(LitchExternalLinks.github),
      ExternalLinkLaunchResult.busy,
    );
    expect(calls, 1);

    pending.complete(true);
    expect(await first, ExternalLinkLaunchResult.opened);
  });

  test('turns platform launch failures into bounded results', () async {
    final ExternalLinkLauncher unavailable = ExternalLinkLauncher(
      invoke: (String url) async => false,
    );
    expect(
      await unavailable.open(LitchExternalLinks.product),
      ExternalLinkLaunchResult.unavailable,
    );

    final ExternalLinkLauncher missing = ExternalLinkLauncher(
      invoke: (String url) => throw MissingPluginException(),
    );
    expect(
      await missing.open(LitchExternalLinks.product),
      ExternalLinkLaunchResult.unavailable,
    );

    final ExternalLinkLauncher failed = ExternalLinkLauncher(
      invoke: (String url) => throw PlatformException(code: 'launch_failed'),
    );
    expect(
      await failed.open(LitchExternalLinks.product),
      ExternalLinkLaunchResult.failed,
    );
  });
}
