import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:litch_mobile/src/services/ota/pending_ota_target_store.dart';

/// Store seam for tests that do not exercise filesystem/process recovery.
class MemoryOtaTargetStore implements OtaPendingTargetStore {
  OtaVerificationTarget? target;

  @override
  Future<OtaVerificationTarget?> load() async => target;

  @override
  Future<void> save(OtaVerificationTarget value) async {
    if (target != null && !identical(target, value)) {
      throw StateError('Another unresolved update owns the store.');
    }
    target = value;
  }

  @override
  Future<void> clear(OtaVerificationTarget value) async {
    if (!identical(target, value)) {
      throw StateError('Unexpected update target.');
    }
    target = null;
  }
}
