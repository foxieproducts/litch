# Firmware releases and corresponding source

The companion app checks the [Release catalog](https://updates.foxieproducts.com/fw/manifest.json)
or [Beta catalog](https://updates.foxieproducts.com/fw/manifest_beta.json).
Each published version identifies its firmware image and release notes.
The notes link to the corresponding source and relinking bundle for that image.

## Rebuild, relink and install

Download the bundle linked from the notes for the version you want to modify.
Its filename, `firmware_compliance_<sha256>.tar.gz`, includes the SHA-256 of the
compressed archive. The bundle includes Litch firmware source, the applicable
license notices, corresponding library source and relinking materials for
Arduino-ESP32 (LGPL-2.1-or-later) and NeoPixelBus (LGPL-3.0-or-later).

Follow the bundle's `README.md` to obtain the exact toolchain, reproduce the
original firmware image or rebuild it with a modified library. See `INSTALL.md`
in the same bundle for the ESP32-S3 ROM-loader procedure, partition layout
and OTA selector. Use the bundle matching the firmware you received. The
[relink source manifest](../../tools/release/firmware_relink_sources.json) and
[license manifest](../../tools/release/firmware_license_manifest.json)
identify the sources and notices used in the supplied release materials.

## Initial public firmware release

Beta and Release 1.0.1-0 use the same 1,726,352-byte ESP32-S3 image. Its file
SHA-256 is `1e441ef246477632dec86443c20c520b457bbd871ae92761c437af62b99b38af`.
The [Release notes](https://updates.foxieproducts.com/fw/version_1.0.1-0_665a0277fa4b6a401fe41f2a255831c8dceb924430bd27f1d4d77f05db3b3592.md)
and [Beta notes](https://updates.foxieproducts.com/fw/version_beta_1.0.1-0_665a0277fa4b6a401fe41f2a255831c8dceb924430bd27f1d4d77f05db3b3592.md)
contain the same feature list and link to the source and relinking materials
for that exact image.

The public Git history begins with the launch snapshot. To verify the firmware
built before that snapshot, use `verify` with the original
[build run](https://github.com/foxieproducts/litch/actions/runs/44684256095),
attempt `1`, source `4d2a261e64ed8822020dea260617cff94fbab016` and build
receipt `8f08a4f37f0d0cdd7f8edbb28c8219175427ad15c0ce8be7ccfa4dbb51f5423b`.
The original receipt records the digest of the build's unpublished draft
notes. The channel receipts verify the reviewed customer notes, firmware and
corresponding source that shipped. The two Git release tags mark the public
source snapshot used as the baseline for future release notes.

## Firmware 1.0.0-1

The 1.0.0-1 factory update limits memory used during wireless peer discovery,
requires responses from both light sensors before wireless verification, and
recovers discovery after the board misses a verifier advertisement. The Beta
and Release channels delivered the same 1,694,864-byte ESP32-S3 firmware image
with SHA-256 `b9da828979352fce9b26b9a9cfc0f4eadc14ec0f64ba88d06f6a5b0697f4730b`.


## Publishing firmware from GitHub

The [firmware workflow](../../.github/workflows/firmware_candidate.yml) runs from
`main` in `foxieproducts/litch`. Its `beta` channel serves the app's Beta
catalog and `stable` serves Release. Contributors can build the same source
locally. Publishing requires access to the repository's `firmware-publishing`
environment.

1. Commit the intended version and build inputs. Verify the source locally,
   including `./litch release check`. For a new version, check the live catalogs
   as well as the repository's release history.
2. In **Actions → Official firmware candidate → Run workflow**, select `main`,
   `build`, the channel, the full source commit and a reference to the local
   checks for that commit. The run builds the S3 image and its corresponding
   source and relinking bundle. Save the run ID, attempt and receipt SHA-256
   from the summary. GitHub retains this candidate artifact for 14 days. Complete
   any planned channel promotions that use it within that window. Building a
   candidate leaves both app catalogs as they were.
3. Run `prepare-notes` with those identifiers. Edit the draft release notes on
   GitHub. Run `prepare-notes` again to get the digest of the text you approved.
   Run the [firmware host check](../../.github/workflows/firmware_host_check.yml)
   to verify authenticated access to both catalogs and obtain their current
   digests.
4. For an authorized rollout, run `promote` with the candidate identifiers,
   the approved notes digest and the selected channel's current manifest digest.
   Promotion checks the retained files, publishes the immutable assets before
   replacing the manifest, and verifies what the companion app can download.
5. Use `verify` to check that this release is still the active channel delivery,
   including its catalog and compatibility aliases. After a later channel
   promotion, use `verify-archive` to check only the older release's immutable
   firmware, notes and compliance URLs; it does not check current channel health.
   Both operations are read-only. Supply the original source commit, channel,
   run ID, attempt and build receipt digest. They can recover the original
   candidate from the public release's archived build receipt, firmware and
   source history after the Actions artifact expires, and compare its frozen
   final receipt, notes and all GitHub assets. New GitHub build receipts bind
   their run and attempt. Older receipts prove the approved source and build
   digest but cannot distinguish successful executions of that same source;
   successful-run admission remains required. Promotion still requires its
   retained Actions candidate within the 14-day window.

Restrict the environment to the `main` branch and configure required reviewers
where supported. Set the AWS role, bucket and lock key as masked environment
secrets. Set the region and object prefix as environment variables. The workflow
obtains short-lived AWS credentials through GitHub OIDC.
Live promotion requires `FIRMWARE_PUBLISH_ENABLED=true`. The host check uses a
session restricted to reading the two manifests. For initial IAM setup, run it
with `identity_only` to see the GitHub-issued audience, subject and branch before
requesting AWS credentials. See the
[GitHub environment settings guide](https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments).

## USB installation and recovery

Use the app for normal firmware updates. To install your own ESP32-S3 build over USB on a V3.3 or V3.31 board, connect a data-capable cable, then run:

```sh
./litch devices
./litch flash s3 --device <usb-serial>
```

Choose the serial reported for that board. If it cannot enter the bootloader automatically, hold P1-X while reconnecting USB, release it, then retry. Factory erase clears saved data. The [factory programming guide](factory_programming.md) covers blank-board setup and the built-in functional test.
