import 'package:flutter/services.dart';

enum ExternalLinkLaunchResult { opened, busy, disallowed, unavailable, failed }

typedef ExternalLinkInvoker = Future<bool> Function(String url);

abstract final class LitchExternalLinks {
  static final Uri product = Uri.parse('https://foxieproducts.com/litch');
  static final Uri privacyPolicy = Uri.parse(
    'https://foxieproducts.com/policies/privacy-policy',
  );
  static final Uri github = Uri.parse('https://github.com/foxieproducts');
  static final Uri duckChess = Uri.parse('https://duckchess.com');

  static final Set<String> _allowedUrls = <String>{
    product.toString(),
    privacyPolicy.toString(),
    github.toString(),
    duckChess.toString(),
  };

  static bool allows(Uri uri) => _allowedUrls.contains(uri.toString());
}

class ExternalLinkLauncher {
  ExternalLinkLauncher({ExternalLinkInvoker? invoke})
    : _invoke = invoke ?? _invokePlatform;

  static final ExternalLinkLauncher shared = ExternalLinkLauncher();
  static const MethodChannel _channel = MethodChannel(
    'com.foxieproducts.litch/external_links',
  );

  final ExternalLinkInvoker _invoke;
  bool _opening = false;

  Future<ExternalLinkLaunchResult> open(Uri uri) async {
    if (!LitchExternalLinks.allows(uri)) {
      return ExternalLinkLaunchResult.disallowed;
    }
    if (_opening) {
      return ExternalLinkLaunchResult.busy;
    }

    _opening = true;
    try {
      final bool opened = await _invoke(uri.toString());
      return opened
          ? ExternalLinkLaunchResult.opened
          : ExternalLinkLaunchResult.unavailable;
    } on MissingPluginException {
      return ExternalLinkLaunchResult.unavailable;
    } on PlatformException {
      return ExternalLinkLaunchResult.failed;
    } catch (_) {
      return ExternalLinkLaunchResult.failed;
    } finally {
      _opening = false;
    }
  }

  static Future<bool> _invokePlatform(String url) async {
    return await _channel.invokeMethod<bool>('openUrl', <String, String>{
          'url': url,
        }) ??
        false;
  }
}
