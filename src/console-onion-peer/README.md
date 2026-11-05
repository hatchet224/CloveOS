# Onion-compatible Netplay helper

Original CloveOS helper, GPL-3.0-or-later. The three production Go files use the
standard library only. They implement the legacy Onion cookie/file exchange,
GB/GBC battery exchange and validated room discovery used by the supervised
Host/Join scripts in `../console-supervisor/init`.

Build with the qualified Go toolchain (the recorded device build uses Go 1.27.3):

```sh
GO111MODULE=off go test onion_peer.go onion_link.go onion_lan.go onion_peer_test.go
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 GO111MODULE=off go build -trimpath -ldflags='-s -w' -o console-onion-peer onion_peer.go onion_link.go onion_lan.go
```

Tests use generated content and local test sockets. No commercial game, BIOS,
personal save, network profile, or stock executable is part of this source.
Cross-device use also requires the matching core variants and wrappers documented
under `../../releases/r49`. The helper alone does not establish compatibility.
