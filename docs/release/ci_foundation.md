# Firmware releases and corresponding source

<a id="initial-public-firmware-release"></a>

The companion app checks the [Release catalog](https://updates.foxieproducts.com/fw/manifest.json)
or [Beta catalog](https://updates.foxieproducts.com/fw/manifest_beta.json).
The [Release notes](https://updates.foxieproducts.com/fw/version.md) and
[Beta notes](https://updates.foxieproducts.com/fw/version_beta.md) describe
the firmware available on each channel.

## Rebuild, relink and install

Open the catalog for the channel you use and check its `version`. For 1.0.2
and later, follow `source.url` in that catalog to the notices and download link for
its corresponding source and relinking bundle. For earlier firmware, follow
the source link in the release notes for that version. These downloads are
hosted on the update server.
Its filename, `firmware_compliance_<sha256>.tar.gz`, includes the SHA-256 of the
compressed archive. The bundle includes Litch firmware source, the exact
Litch-Max engine source used by that firmware, the applicable license notices,
corresponding library source and relinking materials for Arduino-ESP32
(LGPL-2.1-or-later) and NeoPixelBus (LGPL-3.0-or-later).

In a Git checkout, Litch-Max is the pinned `lib/litch-max` submodule. Use
`git clone --recurse-submodules` for a fresh checkout, or
`git submodule update --init --recursive` after selecting a revision in an
existing checkout; the [build guide](../../README.md#building-or-repairing-litch)
shows the commands. GitHub's automatic source ZIP omits submodule contents.
The corresponding firmware bundle includes the engine source for the shipped
image; the [companion app source ZIP](README.md#companion-app-source) supplies
the mobile app source and licenses.

Follow the bundle's `README.md` to obtain the exact toolchain, reproduce the
original firmware image or rebuild it with a modified library. See `INSTALL.md`
in the same bundle for the ESP32-S3 ROM-loader procedure, partition layout
and OTA selector. Use the bundle matching the firmware you received. The
[relink source manifest](../../tools/release/firmware_relink_sources.json) and
[license manifest](../../tools/release/firmware_license_manifest.json)
identify the sources and notices used in the supplied release materials.

## Publishing firmware from GitHub

The [firmware workflow](../../.github/workflows/firmware_candidate.yml) runs from
`main` in `foxieproducts/litch`. Its `beta` channel serves the app's Beta
catalog and `stable` serves Release. Contributors can build the same source
locally. Publishing requires access to the repository's `firmware-publishing`
environment.

The first releases in the 1.0.2, 1.0.3 and 1.0.4 series use the bare version in app
and firmware source, native packages, release tags and manifests. Each occupies
sequence zero. For example, `1.0.3` and `1.0.3-0` identify the same slot and
the next Beta in that series is `1.0.3-1`. Earlier series keep their existing
spellings. Local retries and stable promotions
do not advance the sequence; native package codes remain independently checked
against prior store uploads. Prepare all source identities together with
`tools/release/release_identity.py` after reconciling complete release history
and the live Beta catalog.

The initial 1.0.2 build reads the [launch notes](initial_1.0.2.txt) from its
root commit for both channels. GitHub and the app show the same customer text.
The downloadable notes also include the version line used by older apps.
The update catalogs identify the separate source notice by URL, size and
checksum. For each public release starting with 1.0.2, keep its GitHub release,
tag and successful candidate build run so the verifier can check its archived files.

Starting with 1.0.3, maintain [firmware release notes](firmware_notes.md) in
source. Add each new version at the top, keep earlier sections unchanged, and
retain the complete 1.0.2 launch text. The draft uses this history for both
channels. The current section may be edited in the GitHub draft; earlier
sections must match the latest approved text for each version. Before preparing
a later release, carry any approved draft edits back into source. Version
headings use the exact firmware version. The builder still adds the leading
`Version:` line for older apps, and historical candidates
retain their original notes and verification behavior.

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
3. Run `prepare-notes` with those identifiers. Edit the current release section
   in the draft on GitHub if needed. Run `prepare-notes` again to get the digest
   of the text you approved.
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

## Official firmware recognition

Firmware checksums, digest-derived asset URLs and per-release proof tables stay
on the update server, not in source, documentation, workflow inputs or logs.
Git source identities and third-party dependency and license hashes serve
separate purposes and remain preserved.

The companion app's lookup uses an image-digest record on the protected update
origin. Only explicit recognition means Official; missing records and network
failures remain Unverified. The app sends no device identifier. Record contents
and digest-derived names stay with the published binaries on the server.

The protected [recognition workflow](../../.github/workflows/firmware_provenance.yml)
accepts only a channel and an optional version. With no version, it reads that
channel's fixed public catalog. It validates the canonical same-origin immutable
firmware URL, downloads the server binary, recomputes its whole-file and appended
image digests, and requires an exact match with the catalog before registering
recognition. There are no local receipt, image, URL or digest inputs and no
per-release seed table. Old Git history and expired Actions artifacts are not
needed for the current shipping image.

For an older version, only retained versions of the selected protected S3
channel catalog can establish publication authority. Reads are restricted to
the two catalog keys, bounded to 256 versions and 32 KiB per catalog. Conflicting
image identities or missing history leave recognition pending. Public binary
availability by itself does not establish that it was published. No historical
recognition is claimed until this authority and the exact server binary pass.

Normal promotion registers only after successful catalog readback, using the
same server-binary validation. Conditional creation and exact readback preserve
existing records and allow retries after interruption, including origins that
return HTTP 403 for missing objects. The workflow uses the existing publication
lock. Its OIDC session can read known catalog history, create recognition
records, and acquire/release the lock; it cannot modify catalogs, notes or
firmware. Actual AWS role permissions and retained history remain deployment
checks. The read-only host check can show the current protected OIDC identity.

Set `FIRMWARE_PUBLISH_ENABLED=false` in the protected environment to stop future
registration and promotion jobs before credential acquisition. This does not
remove valid records or change installed firmware. Pause publishers before a
focused tooling rollback and preserve valid server records. Server registration
and compatible app delivery remain separate gates.

## USB installation and recovery

Use the app for normal firmware updates. To install your own ESP32-S3 build over USB on a V3.3 or V3.31 board, connect a data-capable cable, then run:

```sh
./litch devices
./litch flash s3 --device <usb-serial>
```

Choose the serial reported for that board. If it cannot enter the bootloader automatically, hold P1-X while reconnecting USB, release it, then retry. Factory erase clears saved data. The [factory programming guide](factory_programming.md) covers blank-board setup and the built-in functional test.
