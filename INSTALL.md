# Installing CloveOS r48

This test image targets the **MagicX Mini Zero 28**. Use a microSD card sold as 8 GB or larger. Tested virtual capacities extend through 1 TB. The fresh image is 4 GiB.

1. Download `CloveOS-r48.zip` and `SHA256SUMS.txt` from the same release.
2. Verify the ZIP's SHA-256 against `SHA256SUMS.txt`. In PowerShell, run `Get-FileHash .\CloveOS-r48.zip -Algorithm SHA256`.
3. Extract `CloveOS-r48.img`. Allow at least 4 GiB of free disk space for extraction. You can verify the extracted image with `Get-FileHash .\CloveOS-r48.img -Algorithm SHA256` and the image entry in the same checksum file.
4. Back up anything you need from the destination card. Use a whole-card image writer such as balenaEtcher or Rufus in DD mode to write the `.img`. Confirm the correct target card before starting. Flashing erases that card.
5. Safely eject the card, put it in the Mini Zero 28's TF1 slot, and power on. Keep power connected while CloveOS prepares the data volume. It automatically expands before normal launcher services start. If Windows offers to format a Linux partition, cancel that prompt.
6. Add your own games to `Roms/<system>` and any required emulator BIOS files to `BIOS` on the Windows-visible `CLOVE_DATA` volume. Use the folders supplied by the image. Configure Wi-Fi and other preferences on the handheld.

The expansion path preserves file data, retries interrupted metadata updates, and makes no expansion metadata writes on repeated boots at the same capacity. An unclean FAT32 volume is refused before new expansion. If that happens, power down, repair the data filesystem with an appropriate tool, and retry. Reformatting the data volume removes the expansion layout.

System partitions and filesystem structures consume part of the card. This build reserves two 128 MiB FAT tables and about 1 GiB for the system area. The remaining data capacity is available for games and other files. FAT32 limits each individual file to 4 GiB minus one byte.

The image starts without game content, emulator BIOS files, saves, downloaded box art, Wi-Fi profiles, or account credentials. ScreenScraper setup requires the user's own account and API client credentials. SFTP creates its host key locally, and enabled transfer services create their own credentials.

VM tests covered the filesystem, original Zero 28 kernel, clean launcher starts, capacity expansion, and recovery. Fresh-card first boot on the physical handheld is still pending. See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) before testing.
