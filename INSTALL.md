# Installing CloveOS R52

**Release preparation:** these instructions apply once the R52 download is published. This source update does not itself publish a firmware image.

For the **MagicX Mini Zero 28**. Use a microSD card sold as **8 GB or larger**. The extracted raw image is 4 GiB. Flashing the whole-card image erases the selected card, including existing games and saves.

1. Download `CloveOS-r52.zip` and `SHA256SUMS.txt` from the [R52 release](https://github.com/hatchet224/CloveOS/releases/tag/r52).
2. In PowerShell, run `Get-FileHash .\CloveOS-r52.zip -Algorithm SHA256` and compare the result with the ZIP entry in the checksum file.
3. Extract `CloveOS-r52.img`. Allow at least 4 GiB of free space for extraction. Verify the image with the same command and its checksum entry if desired.
4. Back up the destination card. Select the extracted image and the correct card in **Rufus using DD mode** or **balenaEtcher**, then write it. PhoenixCard expects a different Allwinner firmware format and does not support this raw image.
5. Safely eject the card, insert it in TF1 and power on. Keep power connected during initial data expansion. Cancel any Windows prompt to format a Linux partition.
6. Complete startup setup. Fresh installs offer **English**, **Español** and **Русский** and save the selection. Existing installations retain their valid saved language. Settings → Language remains available.
7. Put games in `Roms/<system>` and required emulator BIOS files in `BIOS` on the Windows-visible `CLOVE_DATA` volume. Configure Wi-Fi and accounts on the handheld. Use the Ports Collection for PortMaster and its games.

First boot expands `CLOVE_DATA` to use available card capacity after system and filesystem overhead. Repeated boots at the same capacity do not resize it again. FAT32 limits individual files to 4 GiB minus one byte. Dirty or error-marked volumes need repair before expansion.

The shared image contains no ROMs, emulator BIOS, personal saves, downloaded box art, Wi-Fi credentials, account details, private keys or private device logs. Supply your own content and account information.

Read the release notes, `AUDIT.json` and [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for validation scope and limitations. Original CloveOS source is GPL-3.0-or-later; third-party components retain their licenses. Vendor corresponding-source and redistribution review remains incomplete; see [REDISTRIBUTION.md](REDISTRIBUTION.md).
