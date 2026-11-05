# Recorded R49 candidate recipes

These are the candidate's build, overlay, sealing and ordinary-file image
construction recipes. They are reference recipes for the original project layout,
not a standalone SDK or a command to flash physical storage. The public image
constructor writes a new local `.img` file and verifies its unchanged regions.

The working layout is `<project>/work/psx-switch-r49`, with the imported native
source under `source` and `public-source`, the compatibility inputs under
`onion-netplay`, and the qualified compilers/dependencies under the project's
`work` tree. Base-image inputs and exact hashes are recorded in the recipes and
the earlier `tools/reconstruct-r48` records. Those vendor/base images must be
obtained independently under their own terms. Private manifests, card contents
and build outputs are not supplied by this repository.

For the public pipeline, the overlay replaces only its listed files and removes
the listed old commander and uncleared theme resources. The clean default theme
uses original Clove assets and the separately licensed Atkinson font; the supplied
splash and existing expansion/boot chain are preserved. Fonts/artwork are
separate licensed inputs, not relicensed by these Python scripts.

The published package recipe obtains the build account from the local OS instead
of containing a developer's personal account name. Its optional legacy path scrub
uses a same-length generic replacement. This publication change affects host
privacy processing, not emulator behavior. Prefer compiler prefix maps and
stripped target builds, and still perform a final artifact privacy audit.

Scripts that intentionally use private game inputs for performance qualification,
and the private device identity record, are excluded. The reference layout's
input locations do not imply that their payloads are included or licensed here.
