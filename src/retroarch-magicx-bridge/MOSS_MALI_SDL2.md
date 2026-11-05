# Moss Mali SDL2 video adaptation

`patches/0003-moss-mali-sdl2.patch` applies to RetroArch v1.19.1 commit
`0792144fe3a7b59908b0afdb2c01722e79040360`. It modifies only
`gfx/drivers/sdl2_gfx.c`; no common structure or bridge protocol changes are required.

Apply alongside the checkpoint endpoint patches before compiling RetroArch:

```sh
git apply --check /path/to/retroarch-magicx-bridge/patches/0003-moss-mali-sdl2.patch
git apply /path/to/retroarch-magicx-bridge/patches/0003-moss-mali-sdl2.patch
```

The source detects the exact SDL video backend name `mali`. This backend uses
the Zero 28's demonstrated `SDL_WINDOW_SHOWN`-only 640x480 window policy, rather
than unsupported desktop fullscreen flags. Its physical renderer dimensions
establish the viewport; portrait output receives the clockwise 90 degree scanout
transform used by the Zero 28 MinUI platform, composed with core rotation.

Every Mali core and menu upload uses an explicit logical source rectangle and
validated source pitch. Queried format/access/backing bounds must be compatible
with that logical buffer. For example, a 160x144 RGB565 core frame is uploaded
with `{0,0,160,144}` and its 320-byte pitch even if the backing texture reports
256x256. Rendering crops to the same logical dimensions. It never samples the
uninitialized promoted region or uploads padded rows from the core's buffer.
Menu and text placement share the physical portrait transform. Renderer and
texture failures are reported instead of treating a blank renderer as success.

Other SDL video backends retain their existing fullscreen/windowed, viewport,
upload and rendering paths. A missing renderer now consistently fails video
initialization. This patch does not modify upstream `sdl2_gfx_read_viewport`;
that separate hardware-screenshot path requires its own bounded implementation
before relying on it with promoted/rotated vendor surfaces.

## Validation

`git apply --check` passed against the retained pinned upstream source.
`tests/test_moss_sdl2_helpers.py` extracts the actual production helper functions
and compiles them against a bounds-checking SDL stub. It covers SHOWN-only versus
normal flags; padded RGB565/ARGB/menu textures; explicit logical upload/crop;
short/overflow pitch and format/access/query errors; and portrait/core rotation.
The compiled assertions-enabled mock test passed. Its JSON report is retained
at `tests/MOSS_SDL2_MOCK_TESTS.json`.

Example on the Windows workspace:

```powershell
python tests/test_moss_sdl2_helpers.py --zig C:/path/to/zig.exe `
  --source C:/path/to/RetroArch/gfx/drivers/sdl2_gfx.c `
  --output-dir C:/path/to/work/sdl2-mock-tests
```

These are host-side behavioral tests with synthetic SDL replies. They do not
claim a physical GPU, handheld frame display, electrical vsync, or driver-level
memory correctness test. Rebuild the complete RetroArch executable, refresh
its pinned game-profile identities, and exercise actual gameplay on the board.
