// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"bytes"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func curlFixture(t *testing.T, mapping Export) string {
	t.Helper()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			connection, err := listener.Accept()
			if err != nil {
				return
			}
			go servePeer(connection, mapping, net.ParseIP("127.0.0.1"))
		}
	}()
	return listener.Addr().String()
}
func curlUpload(t *testing.T, address, remote string, data []byte) error {
	t.Helper()
	name := "curl"
	if runtime.GOOS == "windows" {
		name = "curl.exe"
	}
	curl, err := exec.LookPath(name)
	if err != nil {
		t.Skip("curl executable required for stock upload compatibility test")
	}
	filename := filepath.Join(t.TempDir(), "upload")
	if err = os.WriteFile(filename, data, 0600); err != nil {
		t.Fatal(err)
	}
	command := exec.Command(curl, "-sS", "--connect-timeout", "2", "--max-time", "5", "-T", filename, "ftp://"+address+remote)
	for _, item := range os.Environ() {
		key := strings.ToLower(strings.SplitN(item, "=", 2)[0])
		if !strings.Contains(key, "proxy") {
			command.Env = append(command.Env, item)
		}
	}
	command.Env = append(command.Env, "NO_PROXY=127.0.0.1,localhost")
	output, err := command.CombinedOutput()
	if err != nil {
		return &curlFailure{err, string(output)}
	}
	return nil
}

type curlFailure struct {
	err    error
	output string
}

func (e *curlFailure) Error() string { return e.err.Error() + ": " + e.output }

func TestStockCurlLinkUploads(t *testing.T) {
	for _, role := range []string{"host", "join"} {
		t.Run(role, func(t *testing.T) {
			inbox := t.TempDir()
			mapping := Export{LinkInbox: inbox, LinkRole: role, LinkSave: "Player.srm"}
			address := curlFixture(t, mapping)
			leaf := "Player.srm"
			if role == "join" {
				leaf += "_rcvd"
			}
			battery := bytes.Repeat([]byte{0x5a}, 8192)
			if err := curlUpload(t, address, "/tmp/"+leaf, battery); err != nil {
				t.Fatalf("stock curl battery upload: %v", err)
			}
			actual, err := os.ReadFile(filepath.Join(inbox, leaf))
			if err != nil || !bytes.Equal(actual, battery) {
				t.Fatalf("battery mismatch: %v", err)
			}
			for _, marker := range []string{"host_ready", "ready_to_send", "ready_to_receive", "stop_now"} {
				// notify_peer expands ftp://IP/${notify_file}, whose leading
				// slash makes the real stock marker URL contain //tmp/.
				if err := curlUpload(t, address, "//tmp/"+marker, nil); err != nil {
					t.Fatalf("stock curl marker %s: %v", marker, err)
				}
				data, err := os.ReadFile(filepath.Join(inbox, marker))
				if err != nil || len(data) != 0 {
					t.Fatalf("marker mismatch %s: %v", marker, err)
				}
			}
			for _, remote := range []string{"/etc/shadow", "/tmp/unlisted.txt", "/tmp/subdir/host_ready", "/mnt/SDCARD/.console/accounts"} {
				if err := curlUpload(t, address, remote, nil); err == nil {
					t.Fatalf("out-of-scope curl upload accepted: %s", remote)
				}
			}
			if err := curlUpload(t, address, "/tmp/host_ready", []byte{1}); err == nil {
				t.Fatal("nonempty marker accepted")
			}
			if role == "join" {
				if err := curlUpload(t, address, "/tmp/Other.srm_rcvd", battery); err == nil {
					t.Fatal("unselected returned save accepted")
				}
			}
			t.Log("Real curl CWD /tmp + relative STOR succeeds for bounded link saves/markers; forbidden writes remain rejected")
		})
	}
}
func TestNoLinkCurlTemporaryDirectoryRejected(t *testing.T) {
	address := curlFixture(t, Export{Files: map[string]string{}})
	if err := curlUpload(t, address, "/tmp/host_ready", nil); err == nil {
		t.Fatal("ordinary selected-file export enabled a link inbox")
	}
}

func TestLinkTemporaryWorkingDirectorySequence(t *testing.T) {
	for _, role := range []string{"host", "join"} {
		t.Run(role, func(t *testing.T) {
			inbox := t.TempDir()
			address := curlFixture(t, Export{LinkInbox: inbox, LinkRole: role, LinkSave: "Player.srm"})
			ftp, err := connect(address)
			if err != nil {
				t.Fatal(err)
			}
			defer ftp.Close()
			for _, directory := range []string{"/", "tmp", "/tmp"} {
				if code, _, err := ftp.command("CWD " + directory); err != nil || code != 250 {
					t.Fatalf("stock CWD %s failed: %d %v", directory, code, err)
				}
			}
			if code, _, err := ftp.command("CWD /tmp/subdir"); err != nil || code != 550 {
				t.Fatalf("nested directory accepted: %d %v", code, err)
			}
			data, err := passiveSocket(ftp, address)
			if err != nil {
				t.Fatal(err)
			}
			code, _, err := ftp.command("STOR host_ready")
			if err != nil || code != 150 {
				data.Close()
				t.Fatalf("relative marker STOR: %d %v", code, err)
			}
			data.Close()
			if code, _, err := ftp.response(); err != nil || code != 226 {
				t.Fatalf("marker completion: %d %v", code, err)
			}
			if code, _, err := ftp.command("SIZE host_ready"); err != nil || code != 213 {
				t.Fatalf("relative marker SIZE: %d %v", code, err)
			}
			if err := os.Remove(filepath.Join(inbox, "host_ready")); err != nil {
				t.Fatal(err)
			}
			if code, _, err := ftp.command("SIZE host_ready"); err != nil || code != 550 {
				t.Fatalf("consumed marker remains visible: %d %v", code, err)
			}
			if code, _, err := ftp.command("STOR ../settings.srm"); err != nil || code != 502 {
				t.Fatalf("relative traversal write accepted: %d %v", code, err)
			}
			t.Log("Actual production helper accepts stock CWD/PWD/relative STOR/SIZE sequence only in bounded virtual link inbox")
		})
	}
}
