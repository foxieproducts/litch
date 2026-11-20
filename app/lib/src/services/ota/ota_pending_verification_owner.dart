import 'dart:io';

import 'package:path_provider/path_provider.dart';

import 'ota_post_reboot_verifier.dart';
import 'pending_ota_target_store.dart';

/// One pending update survives screens, native retirement and process death.
class OtaPendingVerificationOwner {
  OtaPendingVerificationOwner({OtaPendingTargetStore? store}) : _store = store;

  OtaPendingTargetStore? _store;
  Future<void>? _loading;
  bool _loaded = false;
  OtaVerificationTarget? _pending;
  bool _finalizationAcknowledged = false;
  ({OtaVerificationTarget target, OtaPostRebootDifferentImage result})? _failed;
  Object? storageError;

  OtaVerificationTarget? get pending => _pending;
  bool get finalizationAcknowledged => _finalizationAcknowledged;
  ({OtaVerificationTarget target, OtaPostRebootDifferentImage result})?
  get failed => _failed;

  Future<OtaPendingTargetStore> _resolveStore() async {
    if (_store case final store?) return store;
    final directory = await getApplicationSupportDirectory();
    return _store ??= FilePendingOtaTargetStore(
      File('${directory.path}/pending_firmware_update.json'),
    );
  }

  Future<void> restore() async {
    if (_loaded) return;
    if (_loading case final loading?) return loading;
    final work = _load();
    _loading = work;
    try {
      await work;
    } finally {
      _loading = null;
    }
  }

  Future<void> _load() async {
    try {
      final store = await _resolveStore();
      _pending = await store.load();
      _loaded = true;
      storageError = null;
    } catch (error) {
      storageError = error;
      rethrow;
    }
  }

  Future<void> remember(OtaVerificationTarget target) async {
    await restore();
    if (_pending != null) {
      throw StateError('An update is still awaiting verification.');
    }
    final store = await _resolveStore();
    try {
      await store.save(target);
    } catch (error) {
      // A failed return may follow publication. Reload the actual file before
      // permitting another attempt; absence is never inferred from an error.
      _loaded = false;
      storageError = error;
      rethrow;
    }
    _pending = target;
    _failed = null;
  }

  void acknowledgeFinalization(int uploadConnectionToken) {
    if (_pending?.uploadConnectionToken == uploadConnectionToken) {
      _finalizationAcknowledged = true;
    }
  }

  Future<bool> clear(OtaVerificationTarget target) async {
    await restore();
    if (!identical(_pending, target)) return false;
    final store = await _resolveStore();
    try {
      await store.clear(target);
    } catch (error) {
      storageError = error;
      rethrow;
    }
    _pending = null;
    _finalizationAcknowledged = false;
    storageError = null;
    return true;
  }

  Future<void> resolveFailure(
    OtaVerificationTarget target,
    OtaPostRebootDifferentImage result,
  ) async {
    if (!await clear(target)) {
      throw StateError('The pending update changed during verification.');
    }
    _failed = (target: target, result: result);
  }
}
