// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

func TestStockPokemonClientAdvertisedCore(t *testing.T) {
	payload := []byte("synthetic pinned TGB core fixture")
	file := filepath.Join(t.TempDir(), "tgbdual_libretro.so")
	if err := os.WriteFile(file, payload, 0600); err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(payload)
	row := Core{Name: "tgbdual_libretro.so", ARM32: file, SHA32: hex.EncodeToString(digest[:])}
	_, crc, err := fileDigest(file)
	if err != nil {
		t.Fatal(err)
	}
	server := curlFixture(t, Export{Files: map[string]string{"/mnt/SDCARD/RetroArch/.retroarch/cores/tgbdual_libretro.so": file}})
	fields := map[string]string{"[core]": "/mnt/SDCARD/RetroArch/.retroarch/cores/gambatte_libretro.so", "[corechksum]": "not-the-tgb-crc"}
	if err := checkLinkPeerCore("host", server, fields, row); err != nil {
		t.Fatalf("stock client with actual pinned TGB rejected: %v", err)
	}
	if err := checkLinkPeerCore("join", server, fields, row); err == nil {
		t.Fatal("joining a Gambatte-advertising host was accepted")
	}
	wrong := row
	wrong.SHA32 = hex.EncodeToString(make([]byte, 32))
	if err := checkLinkPeerCore("host", server, fields, wrong); err == nil {
		t.Fatal("different actual TGB file accepted")
	}
	fields["[core]"] = "/mnt/SDCARD/RetroArch/.retroarch/cores/untrusted_libretro.so"
	if err := checkLinkPeerCore("host", server, fields, row); err == nil {
		t.Fatal("unknown advertised core accepted")
	}
	fields["[core]"] = "/mnt/SDCARD/RetroArch/.retroarch/cores/tgbdual_libretro.so"
	fields["[corechksum]"] = crc
	for _, role := range []string{"host", "join"} {
		if err := checkLinkPeerCore(role, "invalid-no-network", fields, row); err != nil {
			t.Fatalf("matching TGB %s rejected: %v", role, err)
		}
	}
	fields["[corechksum]"] = "00000000"
	if err := checkLinkPeerCore("host", server, fields, row); err == nil {
		t.Fatal("explicit different TGB CRC accepted")
	}
	t.Log("Stock client's ordinary Gambatte cookie allowed only for hosting after read-only SHA256 proof of the actual pinned TGB; joining/unknown/mismatched cases rejected")
}
