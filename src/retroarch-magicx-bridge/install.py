#!/usr/bin/env python3
"""Install the endpoint and integration diff into a clean, exact RetroArch pin."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile

PIN = "0792144fe3a7b59908b0afdb2c01722e79040360"
TRACKED = ["Makefile.common", "retroarch.c", "runloop.c", "input/input_driver.c", "tasks/task_save.c",
           "input/drivers_joypad/sdl_joypad.c", "menu/menu_driver.c", "menu/menu_displaylist.c",
           "gfx/drivers/gl2.c", "gfx/drivers/sdl2_gfx.c", "gfx/drivers_context/sdl_gl_ctx.c", "audio/drivers/sdl_audio.c", "gfx/video_driver.c"]
MODULE = ["magicx_bridge.c", "magicx_bridge.h", "magicx_asset_cache.c", "magicx_asset_cache.h", "magicx_io.c", "magicx_retroarch.c", "magicx_retroarch.h",
          "magicx_controls.c", "magicx_controls.h", "magicx_screenshot.c", "magicx_screenshot.h",
          "magicx_png.c", "vendor/lodepng.c", "vendor/lodepng.h", "support/sessiond/control_protocol.h",
          "support/sessiond/bios_policy.h", "magicx_input_layout.h", "support/json.c", "support/sha256.c", "support/sessiond/durability.h", "support/sessiond/json.h", "support/sessiond/sha256.h", "LICENSE"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retroarch", type=pathlib.Path, required=True)
    args = parser.parse_args()
    checkout = args.retroarch.resolve(strict=True)
    module = pathlib.Path(__file__).resolve().parent
    patch = module / "patches" / "0001-magicx-checkpoint-hooks.patch"
    def git(*arguments):
        return subprocess.run(["git", "-c", "core.longpaths=true", *arguments], cwd=checkout,
                              check=True, capture_output=True, text=True, encoding="utf-8")
    if git("rev-parse", "HEAD").stdout.strip() != PIN:
        raise SystemExit(f"Refusing a different RetroArch revision; expected {PIN}")
    if git("status", "--porcelain", "--", *TRACKED).stdout:
        raise SystemExit("Refusing to overwrite changes in the integration files; use a clean checkout")
    destination = checkout / "magicx"
    if destination.exists() or destination.is_symlink():
        raise SystemExit("Refusing to overwrite an existing magicx directory; use a fresh checkout")
    video_patch = module / "patches" / "0002-magicx-portrait-gl-handoff.patch"
    texture_patch = module / "patches" / "0004-magicx-gles-texture-init.patch"
    diagnostic_patch = module / "patches" / "0005-magicx-first-frame-diagnostics.patch"
    audio_state_patch = module / "patches" / "0006-magicx-audio-state-overhead.patch"
    psp_sampling_patch = module / "patches" / "0007-magicx-psp-output-sampling.patch"
    sdl_patch = module / "patches" / "0003-moss-mali-sdl2.patch"
    meter_patch = module / "patches" / "0008-magicx-level-meter.patch"
    patches = [patch, video_patch, sdl_patch, texture_patch, diagnostic_patch, audio_state_patch,
               psp_sampling_patch, meter_patch]
    # Later patches depend on the preceding hooks. Validate the complete
    # sequence on disposable copies before changing the user's checkout.
    with tempfile.TemporaryDirectory(prefix="magicx-patch-check-") as scratch:
        staging = pathlib.Path(scratch)
        for relative in TRACKED:
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(checkout / relative, target)
        for item in patches:
            for operation in ("--check", None):
                command = ["git", "-c", "core.longpaths=true", "apply"]
                if operation:
                    command.append(operation)
                subprocess.run([*command, str(item)], cwd=staging, check=True,
                               capture_output=True, text=True, encoding="utf-8")
    for relative in MODULE:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(module / relative, target)
    compatibility = module / "support/sessiond/retroarch_compat.h"
    shutil.copyfile(compatibility, destination / "support/sessiond/retroarch_compat.h")
    for item in patches:
        git("apply", str(item))
    print(f"Installed {len(MODULE)} module files and the integration patch at RetroArch {PIN}")

if __name__ == "__main__":
    main()
