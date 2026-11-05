# Historical r48 reconstruction recipe

These scripts and metadata reconstruct the exact clean 4 GiB r48 image only when
all required matching inputs are independently available. They do not provide
those binary or artwork inputs or grant rights to redistribute them.

`MANIFEST.json` records required file hashes, upstream URLs, SquashFS layout, and
other inputs. `MISSING-COMPONENTS.json` is the historical list of files needing
matching local build outputs. `PROVENANCE.md` records the remaining input/source
and redistribution questions. The native sources now published in this repository
address part of that inventory; they do not prove every binary is reproducible.

The custom splash bitmap is intentionally not supplied in this source import.
Supply the correctly licensed image independently if reconstructing this exact
historical image. This omission does not change the personal-card splash.

Use `python3 rebuild.py --help` for the `stage` and `assemble` subcommands, with
Python 3.11+, Linux/WSL, squashfs-tools 4.6.1 and dosfstools 4.2. Build and test
only regular image files. These source publication steps never write an SD card.
