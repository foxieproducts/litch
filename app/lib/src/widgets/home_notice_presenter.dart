import 'package:flutter/material.dart';

/// The connected board tabs share one notice owner so guidance does not queue.
class HomeNoticePresenter {
  ScaffoldFeatureController<SnackBar, SnackBarClosedReason>? _current;
  ScaffoldMessengerState? _messenger;

  void removeNotice(
    ScaffoldFeatureController<SnackBar, SnackBarClosedReason>? controller,
  ) {
    if (controller != null && identical(_current, controller)) {
      _removeCurrent();
    }
  }

  void _removeCurrent() {
    final ScaffoldMessengerState? messenger = _messenger;
    _current = null;
    _messenger = null;
    if (messenger?.mounted == true) {
      messenger!.removeCurrentSnackBar();
    }
  }

  ScaffoldFeatureController<SnackBar, SnackBarClosedReason> showNotice(
    BuildContext context,
    SnackBar notice,
  ) {
    final ScaffoldMessengerState messenger = ScaffoldMessenger.of(context);
    // Finish the old Home notice immediately so the next one cannot queue
    // behind its exit animation. Never retain a controller after it closes.
    _removeCurrent();
    final controller = messenger.showSnackBar(notice);
    _current = controller;
    _messenger = messenger;
    controller.closed.then((_) {
      if (identical(_current, controller)) {
        _current = null;
        _messenger = null;
      }
    });
    return controller;
  }
}
