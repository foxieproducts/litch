# BLE Updater Tool

[tools/ble_update_firmware.py](ble_update_firmware.py) is a developer maintenance updater and bandwidth probe. It uses the current board's pre-session OTA control and raw data characteristics. The companion app uses the shared connection session for product updates; a transfer cannot mix these paths on one connection.

It supports:

- scanning for current `Foxie_Litch` boards
- preferring live service-advertising boards over macOS/CoreBluetooth cached name-only entries
- reusing the last successful board address to skip repeat scan delays on later runs when possible
- auto-selecting when exactly one candidate is visible
- prompting when multiple boards are visible
- fetching `fw_version` when available
- uploading a real firmware binary with `ota_begin` / `ota_end`
- choosing fast or reliable writes on the current maintenance protocol
- benchmarking the OTA upload path without flashing via `ota_sim_begin`
- benchmarking board-to-host raw notify throughput via a structured `notify_test` control command
- running a quick `test` command that summarizes uplink and downlink bandwidth in about 10 seconds total

For stripped transport measurements without the updater's mixed OTA/progress plumbing, use [tools/ble_raw_bandwidth.py](ble_raw_bandwidth.py).

## Dependency

The script uses `bleak`.

Connection setup owns the BLE client until service discovery and both
notification subscriptions succeed. If setup fails or is canceled, it attempts
one disconnect with a two-second cancellation timeout before returning the
original error.
A cleanup failure is retained as an exception note; it does not imply the link
was released. This relies on the backend honoring cancellation; the current
CoreBluetooth backend does. Successful setup transfers cleanup ownership to the
command.

From the repo root:

```bash
python -m pip install bleak
```

## Common usage

List discoverable boards:

```bash
python tools/ble_update_firmware.py scan
```

If you specifically need to inspect cached name-only matches too:

```bash
python tools/ble_update_firmware.py scan --include-name-match
```

For a developer update on a board that permits this maintenance connection, name the board using the established command:

```bash
./litch flash s3 --ble-device <board-selector>
```

This builds the image and binds the selected board. After a confirmed End, it checks the board's running version, image digest and OTA acceptance. See `./litch flash --help` for selection details.

The direct maintenance client does not establish owner admission. On an owner-protected board its probe may be refused unless this connection is already admitted; use the companion app for a product update or USB recovery. Recovery-only and owner-disabled maintenance profiles can use this path when their current capability and transfer profiles are available.

For commissioning or transport diagnosis, the standalone client can upload an explicit image:

```bash
python tools/ble_update_firmware.py update path/to/firmware.bin --device <board-selector>
```

The standalone command leaves **post-reboot verification pending**. It does not perform the wrapper's exact-board checks. If End delivery or completion is uncertain, it sends neither another End nor an Abort. The wrapper attempts to inspect the same board but reports the update as pending even when version, digest and acceptance match: those facts alone do not prove this attempt ran. Use `--mode reliable` to select writes with response on the same current protocol.

Benchmark the upload path without flashing:

```bash
python tools/ble_update_firmware.py bench-uplink --bytes 524288
```

Run the existing firmware notify benchmark from the host side:

```bash
python tools/ble_update_firmware.py bench-downlink --sizes 20,64,100,180,244 --packet-count 200 --delay-ms 0
```

Run both directions and sweep OTA sender knobs:

```bash
python tools/ble_update_firmware.py bench --bytes 524288 --chunk-sizes 96,128,180,244 --bursts 1,4,8,12,16
```

To measure Bluetooth throughput, run

```bash
python tools/ble_update_firmware.py test
```

Run the stripped raw throughput probes:

```bash
python tools/ble_raw_bandwidth.py downlink --size 244 --count 4000
python tools/ble_raw_bandwidth.py uplink --bytes 524288 --chunk-size 244 --burst 12
python tools/ble_raw_bandwidth.py both --size 244 --count 4000 --bytes 524288 --chunk-size 244 --burst 12
```

## Tuning knobs

Firmware-side knobs already in the current stack:

- preferred MTU: `BLE_PREFERRED_MTU`
- preferred data length: `BLE_FAST_DATA_LEN_OCTETS`
- fast connection interval range: `BLE_FAST_CONN_INTERVAL_MIN_UNITS` / `BLE_FAST_CONN_INTERVAL_MAX_UNITS`
- OTA status cadence: `MIN_OTA_STATUS_INTERVAL_MS`
- notify benchmark pacing: `notify_test` packet size, packet count, and inter-packet delay

Host-side knobs exposed by the tool:

- `--chunk-size`
- `--mode auto|fast|reliable`
- `--burst`
- `--begin-attempts`
- `--retry-delay-seconds`
- `--pre-begin-delay-seconds`
- `--final-settle-seconds`

Every upload and simulated upload requires a current contract-3 capability report, firmware-update support, a complete OTA-v2 probe and a matching Begin profile before sending data. The board's minimum-MTU compact replies and truncated replies do not contain the full window profile, so this tool stops without sending image bytes if it cannot verify those values. `auto` selects fast writes; `reliable` selects writes with response without changing protocol. Both obey the negotiated receive window and wait for final byte acknowledgement before End.

For official releases, the companion app verifies the downloaded firmware and confirms the running image after reboot. See the [firmware guide](../docs/release/ci_foundation.md).
