# Source-first release policy

This policy applies to the entire CloveOS repository and every future update.

## Source requirements

Every release must identify an immutable Git commit containing the corresponding
CloveOS source, local patches, build scripts, tests, and license notices. Changes
must be committed before or alongside the binary release. Preserve exact upstream
revisions, source archive hashes, toolchain versions, build flags, and a mapping
from each shipped executable/library to its source and license.

A reference to an upstream project, an alternative historical source tree, or a
successful binary test does not establish complete corresponding source for a
different shipped binary. Record incomplete mappings honestly and resolve them
before binary publication.

## Redistribution requirements

Verify redistribution rights for every attached payload, including kernel and
bootloader components, vendor firmware, emulator cores, statically linked
libraries, themes, fonts, icons, and splash artwork. Preserve notices and supply
source as required by each component's license. Do not invent a blanket GPL grant
for third-party work or treat source-available/noncommercial terms as an
unrestricted open-source license.

If a required component has unresolved rights or unavailable required source,
hold the complete binary image as a local/private draft. A source-only release
may proceed for independently cleared CloveOS code. A build recipe can require a
user's separately acquired input, but must clearly document that boundary and
must not claim a complete public-source-only rebuild.

## Privacy and validation requirements

Exclude ROMs, BIOS, saves, personal box art, account details, Wi-Fi credentials,
private keys, private logs, and local home paths. Test with generated fixtures or
permitted test content. Run the source checks and relevant regression tests.
Hash the final source and binary artifacts and verify uploaded hashes.

State physical device test results separately from simulated/fixture tests.
For R49, PSX launch/recovery, audio changes, charger/battery reporting, and
OnionOS 4.3.1-1 interoperability are release requirements. Do not describe
Netplay as fully verified without the supporting end-to-end evidence.

## Release checklist

- [ ] Matching source commit and all local modifications are public.
- [ ] Build instructions, exact dependency pins, and notices are included.
- [ ] Each shipped component has a source/license/redistribution record.
- [ ] Required corresponding source is available with the download.
- [ ] No unresolved vendor or artwork redistribution question remains.
- [ ] The privacy scan and relevant source/runtime tests pass.
- [ ] R49-specific functionality is verified and hardware limits are disclosed.
- [ ] Clean image contains no personal content, ROMs, or BIOS.
- [ ] Final uploaded file hashes match the local artifacts.

No checklist or automated scan is a legal certification. Unresolved questions
remain blockers rather than being marked complete by assumption.
