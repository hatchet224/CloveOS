# Source packaging and card preparation

These source helpers create ordinary package directories and tar archives. They
do not create a complete bootable image, format storage, or write a raw device.
See [BUILDING.md](../BUILDING.md) for current source-build instructions and the
separate `tools/reconstruct-r48` historical image recipe.

The `console-packaging` helper expects the sibling `console-sessiond`,
`console-launcher`, `console-supervisor`, and `retroarch-magicx-bridge` sources.
Its SDK configuration templates require an explicit matching Linux toolchain,
sysroot, and build directory. Review them before running the build helper.

`package.py --layout-only` produces an intentionally non-installable directory
preview. A payload package also requires qualified binaries and matching build
metadata. Neither output is a flashable SD image or proof of board qualification.

Device boot firmware, kernel/modules, display stack, emulator components, BIOS,
and artwork remain separately licensed inputs. Do not publish a complete image
until the source, notices, privacy, and redistribution requirements in
[RELEASE_POLICY.md](../RELEASE_POLICY.md) are satisfied. No ROMs or BIOS are
included in this source repository.
