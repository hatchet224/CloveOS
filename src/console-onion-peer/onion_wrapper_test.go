// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func configValues(raw string) map[string][]string {
	result := map[string][]string{}
	for _, line := range strings.Split(raw, "\n") {
		parts := strings.SplitN(strings.TrimSpace(line), "=", 2)
		if len(parts) == 2 {
			key := strings.TrimSpace(parts[0])
			result[key] = append(result[key], strings.Trim(strings.TrimSpace(parts[1]), "\""))
		}
	}
	return result
}
func TestProductionNativeNetplayPreferencesAreSeededOnce(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("production POSIX shell required")
	}
	helper := filepath.Join("..", "console-supervisor", "init", "console-native-netplay-profile")
	raw, e := os.ReadFile(helper)
	if e != nil {
		t.Fatal(e)
	}
	for _, mode := range []string{"standard", "link"} {
		for _, role := range []string{"host", "join"} {
			t.Run(mode+"-"+role, func(t *testing.T) {
				root := t.TempDir()
				base := filepath.Join(root, "profile")
				session := filepath.Join(root, "session")
				if e := os.Mkdir(session, 0700); e != nil {
					t.Fatal(e)
				}
				core := filepath.Join(root, "core.so")
				factory := filepath.Join(root, "factory.cfg")
				applets := filepath.Join(root, "applets")
				script := filepath.Join(root, "profile.sh")
				for name, data := range map[string]string{core: "original fixture", factory: "config_save_on_exit = \"false\"\nrewind_granularity = \"3\"\n", applets: "#!/bin/sh\nexec \"$@\"\n", filepath.Join(session, "controls.cfg"): "input_menu_toggle_btn = \"4\"\n"} {
					if e := os.WriteFile(name, []byte(strings.ReplaceAll(data, "\\\"", "\"")), 0700); e != nil {
						t.Fatal(e)
					}
				}
				text := strings.ReplaceAll(string(raw), "bb=/bin/busybox", "bb="+applets)
				text = strings.ReplaceAll(text, "base=/mnt/SDCARD/.console/retroarch", "base="+base)
				text = strings.ReplaceAll(text, "/run/console/netplay-runtime|/run/console/easy-netplay|/run/console/easy-netplay/link", session)
				if e := os.WriteFile(script, []byte(text), 0700); e != nil {
					t.Fatal(e)
				}
				run := func() string {
					cmd := exec.Command("/bin/sh", script, mode, role, factory, core, session)
					out, e := cmd.CombinedOutput()
					if e != nil {
						t.Fatalf("native first-use profile: %v %s", e, out)
					}
					return strings.TrimSpace(string(out))
				}
				config := run()
				original, e := os.ReadFile(config)
				if e != nil {
					t.Fatal(e)
				}
				values := configValues(string(original))
				if len(values["config_save_on_exit"]) != 1 || values["config_save_on_exit"][0] != "true" {
					t.Fatal("missing native persistence default", values)
				}
				changed := []byte("config_save_on_exit = \"false\"\nauto_overrides_enable = \"true\"\nauto_remaps_enable = \"true\"\nautosave_interval = \"17\"\ninput_enable_hotkey_btn = \"12\"\ninput_menu_toggle_btn = \"3\"\ninput_exit_emulator_btn = \"11\"\nrewind_enable = \"true\"\nrewind_buffer_size = \"67108864\"\nrewind_granularity = \"7\"\nsavefile_directory = \"/custom/saves\"\nsavestate_directory = \"/custom/states\"\n")
				changed = []byte(strings.ReplaceAll(string(changed), "\\\"", "\""))
				if e := os.WriteFile(config, changed, 0600); e != nil {
					t.Fatal(e)
				}
				if got := run(); got != config {
					t.Fatal("native config moved", got, config)
				}
				after, e := os.ReadFile(config)
				if e != nil || string(after) != string(changed) {
					t.Fatal("native settings were rewritten", e, string(after))
				}
			})
		}
	}
}
func TestLinkCaptureDoesNotFallBackAfterIncompleteCapture(t *testing.T) {
	folder := t.TempDir()
	if e := os.WriteFile(filepath.Join(folder, "native-export-required"), nil, 0600); e != nil {
		t.Fatal(e)
	}
	if got := linkBatteryPath(folder, "saves/player1.srm", "player1.srm"); !strings.Contains(got, "native-battery-unavailable") {
		t.Fatal("partial native capture fell back to stale/wrong directory", got)
	}
	if e := os.Mkdir(filepath.Join(folder, "native-battery"), 0700); e != nil {
		t.Fatal(e)
	}
	if e := os.WriteFile(filepath.Join(folder, "native-battery", "complete"), []byte("ready\n"), 0600); e != nil {
		t.Fatal(e)
	}
	if got := linkBatteryPath(folder, "saves/player1.srm", "player1.srm"); !strings.HasSuffix(got, "native-battery/player1.srm") {
		t.Fatal("native custom directory capture not selected", got)
	}
}
