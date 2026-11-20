# Reactive_ble_mobile

Official Android and iOS implementation for the flutter_reactive_ble plugin.

## Local Apple connection lifecycle

The Apple adapter completes `disconnectFromDevice` only after the old native
terminal callback has retired its task, GATT state and connection publication.
Dart's owning subscription awaits that method result before replacing the link.
An overlapping same-device `connect` is rejected locally as still disconnecting;
it is not queued or retried by the adapter. Power-off and disposal retire pending
cancellation completions once. An already-retired disconnected device completes
immediately; a disconnected scalar with retained adapter ownership still waits
for its terminal callback.

`darwin/Tests/run.sh` exercises the connection manager and delegates using simulated CoreBluetooth callbacks to verify their order and cleanup.
