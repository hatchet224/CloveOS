# Stock Onion link startup compatibility

This change targets two startup failures before the emulator launches:

* Stock curl sends `CWD tmp` before relative `STOR`. The selected-game FTP
  server permits that virtual directory only for its bounded link inbox;
  existing role, filename and upload-size checks still apply.
* Stock Onion 4.3.1-1 can select a fallback client address from
  `192.168.100.101` through `.250`. The host resolves the sole authorized
  associated station through complete IPv4 ARP entries on `wlan0`, instead of
  assuming `.101`. It does not scan the subnet. Cookie, core revision and
  cartridge checks still follow discovery.

The exact upstream reference is tag `v4.3.1-1`, especially
`static/build/.tmp_update/script/network/hotspot_join.sh` and
`static/build/.tmp_update/script/netplay/easy-netplay_server_pokemon.sh`.

## Tests and build

Use the pinned Go toolchain from the main build instructions. The production
helper consists of `onion_peer.go`, `onion_link.go` and `onion_lan.go`:

```sh
GO111MODULE=off go test onion_peer.go onion_link.go onion_lan.go \
  onion_peer_test.go startup_deadline_test.go onion_curl_test.go onion_address_test.go \
  onion_stock_client_test.go onion_wrapper_test.go
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags='-s -w' \
  -o console-onion-peer onion_peer.go onion_link.go onion_lan.go
```

Run the curl tests with real curl installed. They use loopback only and skip
when curl is unavailable. Tests cover both roles, the double-slash marker URLs
used by stock scripts, relative marker upload/consumption, and forbidden writes.
Address tests cover DHCP, fallback, unrelated and incomplete ARP records,
unauthorized or absent stations, invalid interfaces/subnets, and ambiguity.
The disposable VM uses the production helper and wrapper with synthetic ROM
and save fixtures; radio association and RetroArch are mocked. That does not
establish successful wireless play or trading on physical handhelds.

## Deliberate remaining limitation

If the same station MAC has two distinct complete, in-range ARP addresses left
from repeated sessions, discovery refuses to guess between them. The setup
times out after its bounded wait and restores station Wi-Fi. This version does
not clear the board's neighbor cache or add a network scan. That ambiguity case
is tested, but physical repeated-session acceptance remains pending.

## Post-ready startup correction

Stock Onion's joining Pokemon wrapper writes its cookie before it reads the
host's forced TGB Dual selection. A cookie can therefore advertise the ordinary
GLO Gambatte core. The Clove host accepts that one stock case only after fetching
the canonical TGB Dual file read-only and checking its full SHA-256 against the
existing pinned ARM32 entry. It does not accept an arbitrary core name, accept a
Gambatte CRC as TGB proof, execute downloaded code, or widen its own FTP export.
Joining still requires a TGB host. Mismatching actual TGB SHA or advertised TGB
CRC is rejected.

The temporary RetroArch configuration now removes inherited assignments before
writing link-specific values. RetroArch retains the first duplicate key. The
earlier disabled-then-enabled TGB option kept the cable disabled while loading
two cartridges, leading to a null second-GB SRAM access in the pinned core.
Inherited save paths also overrode the appended RAM paths. Each replaced key is
now unique; standard single-player/ordinary Netplay configurations retain their
separate behavior. The frontend and cores themselves are unchanged.

Source tests execute the production shell configuration block for all four
standard/link and host/join combinations. Linux helper tests use real bounded
FTP transfers, including the stock Gambatte-cookie/TGB-SHA case. Separate
disposable QEMU checks reproduced exit 139 with the old configuration, then ran
both corrected roles for 120 actual ARM64 frontend/core frames and saved both
8192-byte SRAMs. Four further actual-core pairs ran 240 frames each: stock ARM32
RetroArch 1.15.0 versus installed Clove ARM64 RetroArch 1.19.1, both host
directions, GB and GBC synthetic link cartridges. Proprietary Miyoo display,
audio and input hardware were stubbed; Clove used headless SDL. This establishes
software startup/Netplay compatibility, not successful physical Wi-Fi trading.

## Primary references

* [Onion Pokemon Easy Netplay documentation](https://onionui.github.io/docs/multiplayer/easynetplay-pokemon)
* [Original merged Pokemon Netplay implementation, PR 1007](https://github.com/OnionUI/Onion/pull/1007)
* [Exact stock joining script, v4.3.1-1](https://github.com/OnionUI/Onion/blob/v4.3.1-1/static/build/.tmp_update/script/netplay/easy-netplay_client_pokemon.sh)
* [Exact stock host script, v4.3.1-1](https://github.com/OnionUI/Onion/blob/v4.3.1-1/static/build/.tmp_update/script/netplay/easy-netplay_server_pokemon.sh)
* [Stock themed information-panel implementation](https://github.com/OnionUI/Onion/blob/v4.3.1-1/src/infoPanel/infoPanel.c)
