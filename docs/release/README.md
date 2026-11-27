# Release information

- [Firmware downloads and corresponding source](ci_foundation.md) lists the update channels and explains how to obtain the source, toolchain instructions and relinking materials for a firmware image.
- The [dependency inventory](generated/dependency_inventory.md), [software bill of materials](generated/release_sbom.json) and [third-party notices](../../THIRD_PARTY_NOTICES.md) identify components and their licenses. Mobile notices are available in **About → Licenses**.
- To check the inventory and notices locally, follow the [reproducibility guide](reproducibility_and_sbom.md).
- For editable V3.31 hardware and manufacturing exports, see the [PCB](../../pcb/README.md) and [mechanical assembly](../../manufacturing_models/README.md).

## Companion app source

Published iOS and Android builds use separate Git tags, `app-ios-<version>-build-<number>` and `app-android-<version>-build-<number>`. Each tag identifies the commit used to build that store package. See the [app publication instructions](../../app/README.md#publishing-the-companion-app).

For app version 1.0.2 (build 51), download the
[companion app source and licenses](https://github.com/foxieproducts/litch/archive/refs/tags/fw-stable-1.0.2.zip).
The [app directory](../../app/) contains the Flutter, iOS and Android projects,
dependency locks and vendored packages. Follow the [app build instructions](../../app/README.md)
to prepare a local build. This root source ZIP supplies the corresponding source
for that app build. GitHub's automatic archives omit Git submodule contents,
including Litch-Max. To rebuild a shipped firmware image, obtain its
[corresponding source and relinking bundle](ci_foundation.md#rebuild-relink-and-install),
which includes its exact engine source.

Original software uses [MPL 2.0](../../LICENSE). Original functional hardware uses [CERN-OHL-S-2.0](../../LICENSE-HARDWARE). Named documentation and selected images use [CC BY-SA 4.0](../../LICENSE-DOCUMENTATION). Keep the corresponding source and notices with distributions as required by the applicable license. Foxie reserves its trademarks.
