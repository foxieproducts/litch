import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/support_email_launcher.dart';

void main() {
  test(
    'platform text truncation retains whole emoji and combining characters',
    () async {
      String? received;
      final launcher = SupportEmailLauncher(
        invoke: (_, subject, _) async {
          received = subject;
          return true;
        },
      );
      final prefix = List.filled(159, 'a').join();
      await launcher.open(
        recipient: SupportEmailLauncher.supportRecipient,
        subject: '$prefix👨‍👩‍👧 tail',
        body: '',
      );
      expect(received, prefix);
    },
  );
  test('opens once, bounds content, and applies a local cooldown', () async {
    DateTime now = DateTime.utc(2026, 9, 1, 12);
    final List<({String recipient, String subject, String body})> calls =
        <({String recipient, String subject, String body})>[];
    final SupportEmailLauncher launcher = SupportEmailLauncher(
      now: () => now,
      invoke: (String recipient, String subject, String body) async {
        calls.add((recipient: recipient, subject: subject, body: body));
        return true;
      },
    );

    expect(
      await launcher.open(
        recipient: 'support@foxieproducts.com',
        subject: List<String>.filled(200, 'S').join(),
        body: List<String>.filled(7000, 'B').join(),
      ),
      SupportEmailLaunchResult.opened,
    );
    expect(calls.single.subject.length, 160);
    expect(calls.single.body.length, 6000);

    expect(
      await launcher.open(
        recipient: 'support@foxieproducts.com',
        subject: 'again',
        body: 'again',
      ),
      SupportEmailLaunchResult.cooldown,
    );
    expect(calls, hasLength(1));

    now = now.add(const Duration(seconds: 8));
    expect(
      await launcher.open(
        recipient: 'support@foxieproducts.com',
        subject: 'again',
        body: 'again',
      ),
      SupportEmailLaunchResult.opened,
    );
    expect(calls, hasLength(2));
  });

  test('reports unavailable mail client without retrying', () async {
    int calls = 0;
    final SupportEmailLauncher launcher = SupportEmailLauncher(
      invoke: (String recipient, String subject, String body) async {
        ++calls;
        return false;
      },
    );

    expect(
      await launcher.open(
        recipient: 'support@foxieproducts.com',
        subject: 'Help',
        body: 'Report',
      ),
      SupportEmailLaunchResult.unavailable,
    );
    expect(calls, 1);
  });

  test('rejects any recipient other than Foxie support', () async {
    int calls = 0;
    final SupportEmailLauncher launcher = SupportEmailLauncher(
      invoke: (String recipient, String subject, String body) async {
        ++calls;
        return true;
      },
    );

    expect(
      await launcher.open(
        recipient: 'tester@example.org',
        subject: 'Help',
        body: 'Report',
      ),
      SupportEmailLaunchResult.failed,
    );
    expect(calls, 0);
  });
}
