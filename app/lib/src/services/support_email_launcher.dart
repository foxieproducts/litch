import 'package:flutter/services.dart';
import 'package:characters/characters.dart';

enum SupportEmailLaunchResult { opened, cooldown, unavailable, failed }

typedef SupportEmailInvoker =
    Future<bool> Function(String recipient, String subject, String body);

class SupportEmailLauncher {
  SupportEmailLauncher({
    SupportEmailInvoker? invoke,
    DateTime Function()? now,
    this.cooldown = const Duration(seconds: 8),
  }) : _invoke = invoke ?? _invokePlatform,
       _now = now ?? DateTime.now;

  static final SupportEmailLauncher shared = SupportEmailLauncher();
  static const String supportRecipient = 'support@foxieproducts.com';
  static const MethodChannel _channel = MethodChannel(
    'com.foxieproducts.litch/support',
  );

  final SupportEmailInvoker _invoke;
  final DateTime Function() _now;
  final Duration cooldown;
  DateTime? _lastOpenedAt;
  bool _opening = false;

  Future<SupportEmailLaunchResult> open({
    required String recipient,
    required String subject,
    required String body,
  }) async {
    if (recipient != supportRecipient) {
      return SupportEmailLaunchResult.failed;
    }
    final DateTime now = _now();
    final DateTime? lastOpenedAt = _lastOpenedAt;
    if (_opening ||
        (lastOpenedAt != null && now.difference(lastOpenedAt) < cooldown)) {
      return SupportEmailLaunchResult.cooldown;
    }
    _opening = true;
    try {
      final bool opened = await _invoke(
        recipient,
        _bounded(subject, 160),
        _bounded(body, 6000),
      );
      if (!opened) {
        return SupportEmailLaunchResult.unavailable;
      }
      _lastOpenedAt = now;
      return SupportEmailLaunchResult.opened;
    } on MissingPluginException {
      return SupportEmailLaunchResult.unavailable;
    } on PlatformException {
      return SupportEmailLaunchResult.failed;
    } catch (_) {
      return SupportEmailLaunchResult.failed;
    } finally {
      _opening = false;
    }
  }

  static Future<bool> _invokePlatform(
    String recipient,
    String subject,
    String body,
  ) async {
    return await _channel.invokeMethod<bool>('openEmail', <String, String>{
          'recipient': recipient,
          'subject': subject,
          'body': body,
        }) ??
        false;
  }

  static String _bounded(String value, int maximum) {
    if (value.length <= maximum) return value;
    // Keep the existing UTF-16 budget without splitting a user-perceived
    // character at the platform channel boundary.
    int end = 0;
    for (final character in value.characters) {
      if (end + character.length > maximum) break;
      end += character.length;
    }
    return value.substring(0, end);
  }
}
