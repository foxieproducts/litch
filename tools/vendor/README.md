# Vendored-source comparison

[compare_vendor.py](compare_vendor.py) audits an in-tree vendor directory against an immutable
upstream baseline from [sources.json](sources.json). The caller supplies an existing upstream
Git checkout; the tool never clones or fetches.

The checkout must be at the pinned commit, have the pinned tag and tree, and
use the exact recorded `origin` URL. The audit reads upstream blobs and Git
entry modes from that commit, hashes local file content with Git's SHA-1 blob
format, maps local entries to Git modes (`100644`, `100755`, or `120000`),
excludes every `.git` path component, and prints sorted `exact`, `modified`,
`local-only`, and `upstream-only` inventories. Modified and local-only files
also have pinned local blob IDs and modes. A missing classification, changed
status, changed pinned local blob, chmod, or file/symlink type change makes the
command fail.

Run one audit from the repository root:

```sh
python3 tools/vendor/compare_vendor.py \
  --vendor mcu-max \
  --upstream /absolute/path/to/caller-supplied/mcu-max-checkout
```

The `mcu-max` key audits [the vendor tree inside Litch-Max](../../lib/litch-max/vendor/mcu-max)
against the pinned Gissio upstream. The other keys are `pca95x5` and
`sparkfun-veml6030`. Add `--semantic-diff` to print a whitespace- and
comment-insensitive token diff for modified C, C++, header, and Arduino source
files. This view supplements the complete blob inventory; it does not make a
changed file exact or permit an unclassified file.

Run the portable mcu-max C regression tests with the host compiler.

```sh
python3 tools/vendor/test_mcu_max.py
```

Exit status is `0` for a clean audit, `1` for classified-inventory or local-blob
drift, and `2` for an invalid manifest, checkout, or invocation. Run the
hermetic unit suite with:

```sh
python3 -m unittest discover -s tools/vendor/tests -v
```
