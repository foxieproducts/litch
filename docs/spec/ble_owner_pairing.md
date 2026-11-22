# Bluetooth pairing

Select the board in **Setup** and follow the physical confirmation shown on the board. Litch can remember up to three paired Bluetooth devices. If it already has three devices, use the pairing instructions in the in-app manual before adding another. Clearing saved pairings requires physical confirmation.

## Protocol

On supported ESP32-S3 boards, the app and firmware exchange pairing commands over the [shared connection protocol](connection_session_wire.md). Ordered opcode 27 carries a pairing action. State codes 40–42 report ownership, confirmation and the applicable outcome. Transport activation and discovery alone do not grant access to a paired board. The board validates the native bond and durable ownership state before admitting protected commands.

For recovery after an operating-system Bluetooth reset, open **About → Litch Manual** and follow the pairing recovery steps for your device.
