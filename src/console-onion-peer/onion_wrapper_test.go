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

func TestProductionWrapperTemporaryLinkSettings(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("production POSIX shell required")
	}
	wrapper := os.Getenv("CLOVE_TEST_WRAPPER")
	if wrapper == "" {
		wrapper = filepath.Join("..", "console-supervisor", "init", "console-onion-easy-netplay")
	}
	raw, err := os.ReadFile(wrapper)
	if err != nil {
		t.Fatal(err)
	}
	text := string(raw)
	marker := "if [ \"$core_name\" = tgbdual_libretro.so ]; then"
	begin := strings.Index(text, marker)
	end := strings.Index(text, "# Check the backend binaries")
	if begin < 0 || end <= begin {
		t.Fatal("production configuration block not found")
	}
	flags := strings.Fields("savefiles_in_content_dir savestates_in_content_dir sort_savefiles_enable sort_savefiles_by_content_enable sort_savestates_enable sort_savestates_by_content_enable game_specific_options config_save_on_exit auto_overrides_enable history_list_enable savestate_auto_save savestate_auto_load netplay_public_announce netplay_use_mitm_server netplay_nat_traversal")
	for _, mode := range []string{"standard", "link"} {
		for _, role := range []string{"host", "join"} {
			t.Run(mode+"-"+role, func(t *testing.T) {
				root := t.TempDir()
				session := filepath.Join(root, "session")
				if err := os.Mkdir(session, 0700); err != nil {
					t.Fatal(err)
				}
				helper := filepath.Join(root, "helper")
				busybox := filepath.Join(root, "applets")
				// Only core preparation and POSIX applet dispatch are fixtures. The
				// production shell configuration block executes unchanged otherwise.
				if err := os.WriteFile(helper, []byte("#!/bin/sh\nprintf '%s\\n' \"$CLOVE_TEST_CORE\"\n"), 0700); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(busybox, []byte("#!/bin/sh\nexec \"$@\"\n"), 0700); err != nil {
					t.Fatal(err)
				}
				input := "core_options_path = \"/old/options.cfg\"\nsavefile_directory = \"/old/saves\"\nsavestate_directory = \"/old/states\"\nautosave_interval = \"15\"\nnetplay_ip_port = \"12345\"\ninput_enable_hotkey_btn = \"9\"\ninput_menu_toggle_btn = \"4\"\nlibretro_directory = \"/old/cores\"\ncore_info_cache_enable = \"true\"\nuntouched_setting = \"keep\"\n"
				for _, key := range flags {
					input += "  " + key + " = \"true\"\n"
				}
				base := filepath.Join(root, "base.cfg")
				if err := os.WriteFile(base, []byte(input), 0600); err != nil {
					t.Fatal(err)
				}
				core := filepath.Join(root, "cores", "tgbdual_libretro.so")
				body := strings.ReplaceAll(text[begin:end], "/system/bin/console-onion-peer", helper)
				body = strings.ReplaceAll(body, "/bin/busybox", busybox)
				prefix := "set -eu\nrole=$CLOVE_TEST_ROLE\nmode=$CLOVE_TEST_MODE\nsession=$CLOVE_TEST_SESSION\nconfig=$CLOVE_TEST_BASE\nbb=$CLOVE_TEST_APPLETS\ncore_name=tgbdual_libretro.so\nregistry=fixture\ncontent=fixture.gb\n"
				command := exec.Command("/bin/sh", "-c", prefix+body)
				command.Env = append(os.Environ(), "CLOVE_TEST_ROLE="+role, "CLOVE_TEST_MODE="+mode, "CLOVE_TEST_SESSION="+session, "CLOVE_TEST_BASE="+base, "CLOVE_TEST_APPLETS="+busybox, "CLOVE_TEST_CORE="+core)
				if output, err := command.CombinedOutput(); err != nil {
					t.Fatalf("production config generation: %v %s", err, output)
				}
				raw, err := os.ReadFile(filepath.Join(session, "retroarch.cfg"))
				if err != nil {
					t.Fatal(err)
				}
				values := configValues(string(raw))
				options, err := os.ReadFile(filepath.Join(session, "options.cfg"))
				if err != nil {
					t.Fatal(err)
				}
				optionValues := configValues(string(options))
				assertOne := func(values map[string][]string, key, want string) {
					t.Helper()
					if len(values[key]) != 1 || values[key][0] != want {
						t.Fatalf("%s = %v, want one %q", key, values[key], want)
					}
				}
				cable := "disabled"
				if mode == "link" {
					cable = "enabled"
				}
				assertOne(optionValues, "tgbdual_gblink_enable", cable)
				player := "1"
				if role == "join" {
					player = "2"
				}
				assertOne(optionValues, "tgbdual_single_screen_mp", "player "+player+" only")
				assertOne(values, "core_options_path", filepath.Join(session, "options.cfg"))
				assertOne(values, "untouched_setting", "keep")
				if mode == "link" {
					for _, key := range flags {
						assertOne(values, key, "false")
					}
					for key, want := range map[string]string{"savefile_directory": filepath.Join(session, "saves"), "savestate_directory": filepath.Join(session, "states"), "autosave_interval": "0", "netplay_ip_port": "55435", "input_enable_hotkey_btn": "19", "input_menu_toggle_btn": "8", "libretro_directory": filepath.Dir(core), "core_info_cache_enable": "false"} {
						assertOne(values, key, want)
					}
				} else {
					assertOne(values, "savefile_directory", "/old/saves")
					assertOne(values, "sort_savefiles_enable", "true")
				}
			})
		}
	}
}
