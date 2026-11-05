# R49 implementation and compatibility source

This is the R49 software source supplement. A public full-card R49 download is
still pending release qualification and vendor redistribution/source evidence.
It must not be treated as a published image or physical handheld verification.

Native application changes are in `src/console-*`. The RetroArch installer in
`src/retroarch-magicx-bridge/install.py` now applies the R49 residual patch after
its eight baseline patches, and copies the required power/policy headers. It was
checked against a clean checkout of the exact upstream pin; all 14 integrated
RetroArch files matched the actual R49 source after line-ending normalization.

`compatibility` contains six core patch sets, exact upstream source archive
checksums, submodule receipts, metadata, license notices and a rebuild helper.
Fetch the source archives, then apply/build them on Linux using the qualified
target compiler wrappers:

```sh
python3 releases/r49/compatibility/fetch_sources.py
CC=/path/to/aarch64-cc CXX=/path/to/aarch64-cxx python3 releases/r49/compatibility/rebuild_compatibility.py
```

Read each upstream license in `compatibility/LICENSES`. Core patch sets retain
their upstream terms. In particular, Snes9x and FinalBurn components carry
restrictions separate from CloveOS's own GPL grant. The pinned Neo Geo candidate
has passed state and stock-frontend compatibility tests; its pin is not asserted
to be the unidentified exact historical compiler revision of Onion's binary.

The ARM32 stock reference executables, ROMs, BIOS and vendor firmware are not
copied here. The compatibility helper's protocol still uses the reference core
identities recorded in its registry; supplying those binary inputs has separate
source/provenance obligations. The manifest is not a license for them.

QEMU tests exercised both stock ARM32 and Clove ARM64 frontends, state exchange,
discovery, Host/Join and teardown. Physical hotspot/radio behavior, Miyoo/Zero28
pairing, PSX performance and charger transitions remain separate device checks.

The requested release format remains a full-card image, with the supplied splash,
`CLOVE_DATA`, full-card expansion and an empty public game/account library. Missing
vendor kernel/bootloader source and proprietary component grants are recorded in
the project's redistribution review. No claim that the entire image is GPL or
cleared for redistribution follows from this software source supplement.
