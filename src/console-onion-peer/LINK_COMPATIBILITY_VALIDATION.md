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
  onion_peer_test.go startup_deadline_test.go onion_curl_test.go onion_address_test.go
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
