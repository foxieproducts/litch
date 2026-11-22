# Verifying the dependency inventory and notices

Run `./litch release check` from the repository root to regenerate and compare
the dependency inventory, software bill of materials and both notice files.
The check runs offline. It uses the [license source index](../../tools/release/dependency_license_evidence.json)
and the license texts and package metadata included in the repository.

See the [release guide](README.md) for the files distributed with Litch.
