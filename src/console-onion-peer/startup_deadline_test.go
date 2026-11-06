// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestStartupDeadlineHelper(t *testing.T) {
	if os.Getenv("CLOVE_DEADLINE_FIXTURE") != "1" {
		return
	}
	startupGuard(25 * time.Millisecond)
	time.Sleep(5 * time.Second)
	os.Exit(99)
}
func TestWholeStartupDeadline(t *testing.T) {
	start := time.Now()
	cmd := exec.Command(os.Args[0], "-test.run=^TestStartupDeadlineHelper$")
	cmd.Env = append(os.Environ(), "CLOVE_DEADLINE_FIXTURE=1")
	output, err := cmd.CombinedOutput()
	exit, ok := err.(*exec.ExitError)
	if !ok || exit.ExitCode() != 28 || !strings.Contains(string(output), "startup deadline exceeded") || time.Since(start) > 3*time.Second {
		t.Fatalf("unbounded startup: %q %v", output, err)
	}
	stop := startupGuard(25 * time.Millisecond)
	stop()
	time.Sleep(60 * time.Millisecond)
	t.Log("Overall startup budget returns28; cancelled guard cannot terminate successful startup; save publication is unguarded")
}

func TestStockGLOCookieCanonicalROMPath(t *testing.T) {
	raw := "/mnt/SDCARD/Emu/GBC/../../Roms/GBC/Pokemon - Blue.gbc"
	if strings.HasPrefix(raw, "/mnt/SDCARD/Roms/") {
		t.Fatal("old rejection counterexample invalid")
	}
	got, err := selectedROMPath(raw)
	if err != nil || got != "/mnt/SDCARD/Roms/GBC/Pokemon - Blue.gbc" {
		t.Fatalf("stock GLO path %q %v", got, err)
	}
	for _, raw := range []string{"/mnt/SDCARD/Roms/GB/../../../.console/accounts", "/mnt/SDCARD/Emu/../../RetroArch/retroarch.cfg", "/mnt/SDCARD/Roms", "Roms/GB/test.gb", "/mnt/SDCARD/Roms/GB/a\nRETR /etc/shadow", "/mnt/SDCARD/Roms/GB/a\\b.gb"} {
		if got, err := selectedROMPath(raw); err == nil {
			t.Fatalf("out of scope path accepted %q => %q", raw, got)
		}
	}
	t.Log("Captured stock GLO path accepted after normalization; traversal escaping Roms and FTP command injection rejected")
}
