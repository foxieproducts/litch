# Release information

- [Firmware downloads and corresponding source](ci_foundation.md) lists the update channels and explains how to obtain the source, toolchain instructions and relinking materials for a firmware image.
- The [dependency inventory](generated/dependency_inventory.md), [software bill of materials](generated/release_sbom.json) and [third-party notices](../../THIRD_PARTY_NOTICES.md) identify components and their licenses. Mobile notices are available in **About → Licenses**.
- To check the inventory and notices locally, follow the [reproducibility guide](reproducibility_and_sbom.md).
- For editable V3.31 hardware and manufacturing exports, see the [PCB](../../pcb/README.md) and [mechanical assembly](../../manufacturing_models/README.md).

## Mobile source editions

The launch `main` commit contains the app source for 1.0.1 (49). The tags
`app-v1.0.0+46` and `app-v1.0.0+47` supply the matching Flutter, iOS and Android
sources and licenses for those mobile editions. In a clone of this repository,
extract a tagged tree into an empty directory. To open the sources for build 47,
run the following commands.

```sh
git fetch origin 'refs/tags/app-v1.0.0+47:refs/tags/app-v1.0.0+47'
mkdir litch-build-47
git archive 'app-v1.0.0+47^{tree}' | tar -x -C litch-build-47
```

The `app/` directory contains the original platform projects, dependency locks
and vendored packages. Run `flutter pub get` from that directory to prepare
the build. The root license and third-party notices accompany each edition.

Original software uses [MPL 2.0](../../LICENSE). Original functional hardware uses [CERN-OHL-S-2.0](../../LICENSE-HARDWARE). Named documentation and selected images use [CC BY-SA 4.0](../../LICENSE-DOCUMENTATION). Keep the corresponding source and notices with distributions as required by the applicable license. Foxie reserves its trademarks.
