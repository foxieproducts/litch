enum SupportPhonePermission {
  bluetooth,
  bluetoothScan,
  bluetoothConnect,
  locationWhenInUse,
}

// These are source-qualified plugin observations, not inferred OS grants.
enum SupportPhonePermissionResult {
  granted,
  denied,
  permanentlyDenied,
  restricted,
  limited,
  provisional,
  timedOut,
  unavailable,
}

class SupportPhoneDiagnostics {
  SupportPhoneDiagnostics({
    required this.platform,
    required Map<SupportPhonePermission, SupportPhonePermissionResult>
    permissions,
    this.cachedBleStatus,
    this.cachedDiscoveryAvailability,
  }) : permissions =
           Map<
             SupportPhonePermission,
             SupportPhonePermissionResult
           >.unmodifiable(permissions);

  final String platform;
  final Map<SupportPhonePermission, SupportPhonePermissionResult> permissions;
  final String? cachedBleStatus;
  final String? cachedDiscoveryAvailability;
}
