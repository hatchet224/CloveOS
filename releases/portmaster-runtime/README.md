# PortMaster native runtime correction candidate

The official Bananaguy Classic Godot runtime fails to load with CloveOS's vendor
C++ library: it requires GLIBCXX_3.4.26, while that library exports through
GLIBCXX_3.4.24. The failure is reproduced with the actual ARM64 runtime and
official ready-to-run game package. This is a confirmed software blocker;
the tester's exact physical failure has not been captured.

An isolated C++ library under `/system/portmaster/lib` gets Godot through loading
to video initialization. Vendor and emulator libraries remain unchanged. The
dependency lock, extraction recipe, matching source references and notices are
included. Native-port logs now identify the script and its result, signal and
duration. A successful script exit is not reported as successful gameplay:
the upstream launcher can hide a failed engine behind its final shell command.

The official launcher passes the production child-cleanup, mount-cleanup and
clean-FAT software tests. QEMU lacks the handheld display and controls, and
Godot stops at its unavailable test video driver. Physical rendering, audio,
controls and gameplay remain unverified. The separate candidate is not flashed
or published as a binary. The accepted R56 installation remains intact.

No ROMs, BIOS, personal saves, screenshots, private card logs or game data are
included. See [BUILDING.md](BUILDING.md), [SOFTWARE_TESTS.json](SOFTWARE_TESTS.json)
and the existing dependency and release policies.
