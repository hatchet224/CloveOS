# Native Wi-Fi, downloads and ScreenScraper

This build adds native AArch64 services to the existing supervisor; it does not
execute the Miyoo card's ARM32 binaries. All private IPC peers ship together.

## Kernel and boot chain

The Android 4.9.170 module is rejected. The image uses the Zero 28 Linux 4.9.191
kernel and all 19 corresponding modules from oakMOSS v0.6.0-beta.1. Its archive
SHA256, boot-partition hash, module hashes, vermagic and GPU checks are recorded
in `/etc/console/KERNEL_PROVENANCE.json`. This is an upstream prebuilt kernel,
not a locally compiled kernel. The stock Moss glibc 2.29 userland is retained;
oakMOSS's glibc 2.38 userland is not imported. The PowerVR firmware bytes match
stock Moss exactly. Raw boot0, U-Boot, environment and vendor partition entries
remain byte-identical; only the original boot partition is replaced explicitly.

`console-network-init.sh` runs in the background after the qualified card mount.
It loads 8189es normally, starts wpa_supplicant 2.12 with nl80211 and statically
linked pinned OpenWrt libnl-tiny, and starts DHCP
with the fixed `console-dhcp.sh` callback. DHCP validates IPv4 fields and never
evaluates lease text. DNS and clock synchronization support certificate checks.
Missing radio, failed association or DHCP cannot block MainUI or fabricate
checkpoint completion. Physical radio/driver/display compatibility remains a
board test, separate from the emulated service tests.

## Controller-only setup

Open **Settings > Wi-Fi**. X refreshes; A selects a WPA2-PSK/CCMP or open network.
Use D-pad/A on the QWERTY keyboard, X to delete, Y to reveal/hide, START to join,
and B to cancel. SELECT toggles letter case; L1/R1 switches between letters and
two number/symbol pages. The action row also provides case, pages, Space, Delete,
and Join. Every printable ASCII character is available, including spaces and
quotes. Long entries show their tail and a character count. WPA3-only, enterprise
and WEP networks are not offered as compatible. The idle Wi-Fi screen refreshes
status every two seconds and shows the assigned IP after DHCP succeeds.
Reconnect, disconnect and confirmed Forget are available. Credentials persist
only after SAVE_CONFIG, bounded validation, atomic rename and volume flushing.
The root-owned supplicant configuration lives in tmpfs; its durable copy is
`.console/wifi.conf` on the qualified card.

Open **Apps > ScreenScraper**, or **Y > ScreenScraper** on a selected game.
First-time setup includes your ScreenScraper username, password, region and
media selection. The GPL-3.0 Onion v4.3.1-1 scraper connector's application setup
is included in the native port; users do not request or enter developer keys.
The port identifies itself as `CloveOS`. Its source, modifications and
license are retained in `third_party/onion-scraper`. Existing personal account
records remain compatible and are not overwritten by application defaults.
Create a personal account at screenscraper.fr first if needed. Enter personal
credentials on-device, not in chat.
Passwords are hidden by default; the keyboard distinguishes letter case.
The application setup is checked against the official HTTPS API; physical Wi-Fi
and the user's account/quota remain separate checks. Server and quota failures
keep existing artwork and account settings intact.

Selected-ROM requests rehash the enrolled ROM and compute CRC32. GB/GBC lookup
uses the cartridge's CGB flag, even if a monochrome game was named `.gbc`.
Choose 2D/3D cover, screenshot, title screen, wheel, marquee or mix media and a
preferred region. The service parses bounded ScreenScraper JSON, decodes only
bounded PNG/JPEG, scales to at most 320x360, and writes a real PNG in the matching
`Roms/<system>/Imgs/<ROM stem>.png`. Missing-cover scraping is sequential; B
cancels. Existing covers survive network, validation and media-check failures.
This implementation fetches artwork; it does not implement every Onion scraper
provider, composite-art generator or rich metadata editing workflow.

## Native package downloads

**Apps > Package downloads** uses protected public HTTPS sources by default.
The six MinUI native Zero28 cores are selected from pinned base/extras ZIP
members; four CrossMix AArch64 GPU cores use pinned raw ELF downloads. Saturn's
unused libtmenu DT_NEEDED entry is removed before checking its installed hash.
No user-hosted server is required. All ten cores are bundled for offline use.
An optional direct-file HTTPS mirror is supported for the exact protected files.

Only fixed verified core IDs are downloadable. Architecture and SHA256 must
match the immutable `/etc/console/BINARY_MANIFEST.json`; a remote index cannot
authorize different executable bytes. Downloads are cached transactionally in
`.console/downloads`. An already-correct protected core stays in use. Repair of
a fixed existing core placeholder copies verified bytes into private tmpfs and
binds that immutable file over the placeholder; no executable runs from FAT.
Qualified cached files are checked on startup. Unknown packages, Miyoo binaries
and new unpinned versions are not installed. The complete Onion package catalog
is not yet ported. Source and licenses accompany the release source archive.

## Transport and persistence

curl 8.23.0 uses statically linked MbedTLS 3.6.7 and the protected CA bundle.
Peer/hostname checks stay enabled. HTTP downgrade, proxy environment, cookies,
unbounded bodies and credential-bearing log output are disabled. Connect/total
timeouts and cancellation are bounded. Network transfers run on the storage
worker; orderly shutdown cancels transfers and drains accepted settings writes
before final filesystem barriers. Network status is never checkpoint authority.
The matched kernel selects CFG80211 and disables CFG80211_WEXT; the final
supplicant and startup script therefore use nl80211.

`.console/online.json` and `online.previous.json` use checksummed canonical
records with atomic replacement and previous-record recovery. Hex fields are
encoding, not encryption; these and the Wi-Fi file contain private account data
on the user's writable card. They are not included in public source packages.

`ONLINE_TARGET_TESTS.json` records actual AArch64 code under Moss glibc 2.29,
verified local HTTPS, bad CA, size limits, cancellation, redirect downgrade
rejection, HTTP errors, FAT32 fsck, settings fallback, image validation, protected
package hashes and preservation of previous downloads. Its media checker is a
fault-injected adapter; it does not prove live radio, live ScreenScraper account
access, bind-mount repair or physical SD power-loss behavior.
