String simplifyFirmwareStatus(String message) {
  String out = message;
  out = out.replaceAll('OTA', 'Update');
  out = out.replaceAll('ota', 'update');
  out = out.replaceAll('BLE', '');
  out = out.replaceAll('ble', '');
  out = out.replaceAll('firmware', 'update');
  out = out.replaceAll('Firmware', 'Update');
  return out.replaceAll(RegExp(r'\s+'), ' ').trim();
}
