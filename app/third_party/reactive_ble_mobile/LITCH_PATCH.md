# Litch changes to reactive_ble_mobile

This copy of reactive_ble_mobile 5.4.0 carries the upstream BSD license. It works together with the single-subscription changes in flutter_reactive_ble.

## Connecting and disconnecting

On Android, a disconnect completes after subscription disposal and the SDK’s disconnect completion. An attempt already admitted to the connection queue finishes cleanup before the next connection starts. The 200 ms minimum connection age uses a monotonic clock. Expected GATT or disconnect errors after cancellation are handled inside the plugin. Unexpected errors still reach the thread’s exception handler.

On Apple platforms, an overlapping connection to the same peripheral is rejected while a disconnect is pending. A bounded journal stores the last 32 connection events and can be filtered by device. Exported events contain phase and lifecycle values. Discovery retains characteristic-discovery failures from any service when reporting the overall result.

## Writing without response

On Apple platforms, the plugin keeps one bounded batch per peripheral and sends whole packets when CoreBluetooth signals capacity. A reserved control write can take the next available slot between packets. The native admission deadline is five seconds. Disconnecting, replacing a connection or reaching the deadline retires pending writes.

On Android, a contiguous shared-session Object batch uses one low-priority SDK operation. Control writes can run between complete packets. A request for high-performance connection priority returns once the SDK accepts or refuses it. MTU negotiation and discovery can then proceed without waiting for a connection-parameter callback.

A completed native write reports local admission. The Dart session uses progress from the board to confirm receipt and recover missing packets.

## Apple privacy manifest

The pod includes `PrivacyInfo.xcprivacy`. Reason `35F9.1` covers its use of `ProcessInfo.systemUptime` for elapsed connection diagnostics and write deadlines. It does not export absolute uptime. [Apple’s required-reason API definitions](https://developer.apple.com/documentation/bundleresources/app-privacy-configuration/nsprivacyaccessedapitypes/nsprivacyaccessedapitype)
