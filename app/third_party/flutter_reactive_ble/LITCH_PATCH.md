# Litch connection cancellation patch

Base: flutter_reactive_ble 5.4.0 from pub.dev, BSD-3-Clause (LICENSE retained).
DeviceConnectorImpl.connect uses single-subscription Repeater instead of
Repeater.broadcast so subscription cancellation awaits native disposal.
Litch gives each connection stream one owning subscription in LitchBleService.
The companion reactive_ble_mobile patch is required on Android: upstream replies
to disconnect before disposal, so the Dart patch alone is insufficient.
ConnectedDeviceOperationImpl.subscribeToCharacteristic also uses a
single-subscription Repeater. Awaiting its cancellation now drains native
notification disable before a replacement subscription can enable that same
characteristic. Broadcast cancellation previously returned early, letting stale
cleanup silence Litchcraft's repaired channel on the same healthy connection.
Each app characteristic subscription has one owning listener; consumers that
need to share it must fan out above that native owner. Scan/status broadcast
streams remain unchanged.

The regression tests cover Android connector cleanup and Litchcraft notification cancellation. An upstream replacement must wait for native disposal before reconnecting or enabling a replacement notification subscription.
