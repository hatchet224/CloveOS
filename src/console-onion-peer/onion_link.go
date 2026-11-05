// SPDX-License-Identifier: GPL-3.0-or-later
// Onion link-cable protocol files are scoped to this temporary session only.
package main

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"net"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type LinkSession struct {
	Role, Registry, ROM, Hash, OwnSave, PeerROM, PeerSave, PeerAddress string
	OwnSRAM, PeerSRAM                                                  int
}

func gbSRAM(rom []byte) (int, error) {
	if len(rom) < 32768 || len(rom) > 8*1024*1024 {
		return 0, errors.New("invalid GB cartridge size")
	}
	var checksum byte
	for i := 0x134; i < 0x14d; i++ {
		checksum = checksum - rom[i] - 1
	}
	banks := []int{0, 1, 1, 4, 16, 8}
	if rom[0x14d] != checksum || int(rom[0x149]) >= len(banks) {
		return 0, errors.New("invalid GB cartridge header")
	}
	size := banks[rom[0x149]] * 8192
	if size == 0 {
		return 0, errors.New("link cartridge has no battery RAM")
	}
	return size, nil
}
func saveLeaf(rom string) string { return strings.TrimSuffix(path.Base(rom), path.Ext(rom)) + ".srm" }
func linkRecord(hash string, sram, rtc []byte) []byte {
	data := append(append([]byte{}, sram...), rtc...)
	header := make([]byte, 208)
	copy(header[:8], "CLVLINK1")
	copy(header[72:136], hash)
	sum := sha256.Sum256(data)
	copy(header[136:200], hex.EncodeToString(sum[:]))
	binary.BigEndian.PutUint32(header[200:204], uint32(len(sram)))
	binary.BigEndian.PutUint32(header[204:208], uint32(len(rtc)))
	return append(header, data...)
}
func readLinkRecord(file, hash string, size int) ([]byte, []byte, error) {
	data, e := readBounded(file, 208+131072+4)
	if e != nil {
		return nil, nil, e
	}
	if len(data) != 208+size+4 || string(data[:8]) != "CLVLINK1" || string(data[72:136]) != hash ||
		binary.BigEndian.Uint32(data[200:204]) != uint32(size) || binary.BigEndian.Uint32(data[204:208]) != 4 {
		return nil, nil, errors.New("invalid saved link battery")
	}
	sum := sha256.Sum256(data[208:])
	if string(data[136:200]) != hex.EncodeToString(sum[:]) {
		return nil, nil, errors.New("link battery checksum mismatch")
	}
	return data[208 : 208+size], data[208+size:], nil
}
func seedBattery(card, hash string, size int) ([]byte, []byte, error) {
	sram, rtc, e := readLinkRecord(path.Join(card, "Saves/LinkCable", hash+".link"), hash, size)
	if e == nil {
		return sram, rtc, nil
	}
	if !os.IsNotExist(e) {
		return nil, nil, e
	}
	// Read an immutable, checksummed generation as a seed; never modify a normal
	// game's checkpoint or auto-state. Link-cable progress has its own save.
	names, e := filepath.Glob(path.Join(card, ".console/sessions/*/g-*/manifest.json"))
	if e != nil {
		return nil, nil, e
	}
	if len(names) > 32768 {
		return nil, nil, errors.New("too many saved generations to scan")
	}
	var newest uint64
	for _, name := range names {
		raw, e := readBounded(name, 16384)
		if e != nil {
			continue
		}
		raw = []byte(strings.TrimSpace(string(raw)))
		at := strings.LastIndex(string(raw), `,"checksum":`)
		if at < 0 {
			continue
		}
		var fields map[string]json.RawMessage
		if json.Unmarshal(raw, &fields) != nil {
			continue
		}
		text := func(key string) string { var value string; json.Unmarshal(fields[key], &value); return value }
		sum := sha256.Sum256(append(append([]byte{}, raw[:at]...), '}'))
		if text("checksum") != hex.EncodeToString(sum[:]) || text("content") != hash {
			continue
		}
		generation, e := strconv.ParseUint(text("generation"), 10, 64)
		if e != nil || generation <= newest {
			continue
		}
		data, e := readBounded(path.Join(path.Dir(name), "sram.bin"), int64(size))
		if e != nil || len(data) != size {
			continue
		}
		sum = sha256.Sum256(data)
		if hex.EncodeToString(sum[:]) != text("sram_hash") {
			continue
		}
		sram = data
		rtc = make([]byte, 4)
		newest = generation
	}
	if sram == nil {
		return nil, nil, errors.New("save this game normally before starting Pokemon trade/battle")
	}
	return sram, rtc, nil
}
func linkInit(registry, rom, folder, role string) {
	if role != "host" && role != "join" {
		die(errors.New("invalid link role"))
	}
	prepare(registry, "tgbdual_libretro.so", rom, folder)
	raw, e := readBounded(rom, 8*1024*1024)
	die(e)
	size, e := gbSRAM(raw)
	die(e)
	digest := sha256.Sum256(raw)
	hash := hex.EncodeToString(digest[:])
	sram, rtc, e := seedBattery("/mnt/SDCARD", hash, size)
	die(e)
	die(os.MkdirAll(path.Join(folder, "inbox"), 0700))
	die(os.MkdirAll(path.Join(folder, "saves/.netplay"), 0700))
	die(os.MkdirAll(path.Join(folder, "states"), 0700))
	die(atomicRAMFile(path.Join(folder, "own.srm"), sram))
	die(atomicRAMFile(path.Join(folder, "own.rtc"), rtc))
	var mapping Export
	readJSON(path.Join(folder, "export.json"), &mapping)
	mapping.LinkInbox = path.Join(folder, "inbox")
	mapping.LinkRole = role
	mapping.LinkSave = saveLeaf(rom)
	data, e := json.Marshal(mapping)
	die(e)
	die(atomicRAMFile(path.Join(folder, "export.json"), data))
	state := LinkSession{Role: role, Registry: registry, ROM: rom, Hash: hash, OwnSave: saveLeaf(rom), OwnSRAM: size}
	data, e = json.Marshal(state)
	die(e)
	die(atomicRAMFile(path.Join(folder, "link-session.json"), data))
}
func cookie(data []byte) (map[string]string, error) {
	fields := map[string]string{}
	for _, line := range strings.Split(string(data), "\n") {
		if line == "" {
			continue
		}
		pieces := strings.SplitN(line, ": ", 2)
		if len(pieces) != 2 || strings.ContainsAny(pieces[1], "\r\x00") {
			return nil, errors.New("invalid Onion cookie")
		}
		if _, exists := fields[pieces[0]]; exists {
			return nil, errors.New("duplicate Onion cookie field")
		}
		fields[pieces[0]] = pieces[1]
	}
	return fields, nil
}
func waitFile(name string, seconds int) ([]byte, error) {
	for attempt := 0; attempt < seconds*4; attempt++ {
		if _, e := os.Stat(path.Join(path.Dir(name), "stop_now")); e == nil {
			return nil, errors.New("Onion link peer cancelled")
		}
		data, e := readBounded(name, 131072)
		if e == nil {
			return data, nil
		}
		if !os.IsNotExist(e) {
			return nil, e
		}
		time.Sleep(250 * time.Millisecond)
	}
	return nil, errors.New("Onion link peer timed out")
}
func notify(address, marker string) error { return put(address, "/tmp/"+marker, nil) }
func remoteExists(address, remote string) (bool, error) {
	f, e := connect(address)
	if e != nil {
		return false, e
	}
	defer f.Close()
	code, _, e := f.command("SIZE " + remote)
	if e != nil {
		return false, e
	}
	if code == 213 {
		return true, nil
	}
	if code == 550 {
		return false, nil
	}
	return false, errors.New("Onion link marker check failed")
}
func linkPair(address, folder string) {
	var state LinkSession
	readJSON(path.Join(folder, "link-session.json"), &state)
	var data []byte
	var e error
	for attempt := 0; attempt < 80; attempt++ {
		data, e = fetch(address, "/mnt/SDCARD/RetroArch/retroarch.cookie", 4096)
		if e == nil {
			break
		}
		time.Sleep(250 * time.Millisecond)
	}
	die(e)
	fields, e := cookie(data)
	die(e)
	if path.Base(fields["[core]"]) != "tgbdual_libretro.so" {
		die(errors.New("Onion Pokemon link requires TGB Dual"))
	}
	var found bool
	for _, row := range cores(state.Registry) {
		if row.Name == "tgbdual_libretro.so" {
			die(pinned(row))
			_, crc, e := fileDigest(row.ARM32)
			die(e)
			if !strings.EqualFold(crc, fields["[corechksum]"]) {
				die(errors.New("Onion TGB Dual core revision differs"))
			}
			found = true
		}
	}
	if !found {
		die(errors.New("missing matching TGB Dual core"))
	}
	remote := fields["[rom]"]
	extension := strings.ToLower(path.Ext(remote))
	if !strings.HasPrefix(remote, "/mnt/SDCARD/Roms/") || (extension != ".gb" && extension != ".gbc") {
		die(errors.New("peer must select an unpacked GB/GBC cartridge"))
	}
	rom, e := fetch(address, remote, 8*1024*1024)
	die(e)
	crc := fmt.Sprintf("%08x", crc32IEEE(rom))
	if !strings.EqualFold(crc, fields["[romchksum]"]) {
		die(errors.New("peer cartridge checksum mismatch"))
	}
	size, e := gbSRAM(rom)
	die(e)
	state.PeerROM = remote
	state.PeerSave = saveLeaf(remote)
	state.PeerAddress = address
	state.PeerSRAM = size
	slot, other := 1, 2
	if state.Role == "join" {
		slot, other = 2, 1
	}
	own, e := readBounded(state.ROM, 8*1024*1024)
	die(e)
	die(atomicRAMFile(path.Join(folder, fmt.Sprintf("player%d.gb", slot)), own))
	die(atomicRAMFile(path.Join(folder, fmt.Sprintf("player%d.gb", other)), rom))
	savefolder := path.Join(folder, "saves")
	if state.Role == "join" {
		savefolder = path.Join(savefolder, ".netplay")
	}
	ownSave, e := readBounded(path.Join(folder, "own.srm"), 131072)
	die(e)
	if state.Role == "join" {
		die(put(address, "/tmp/"+state.OwnSave, ownSave))
	}
	peerSave := make([]byte, size)
	if state.Role == "host" {
		peerSave, e = waitFile(path.Join(folder, "inbox", state.PeerSave), 30)
		die(e)
		if len(peerSave) != size {
			die(errors.New("peer Pokemon save size differs from cartridge"))
		}
	}
	for number, battery := range map[int][]byte{slot: ownSave, other: peerSave} {
		die(atomicRAMFile(path.Join(savefolder, fmt.Sprintf("player%d.srm", number)), battery))
		die(atomicRAMFile(path.Join(savefolder, fmt.Sprintf("player%d.rtc", number)), make([]byte, 4)))
	}
	data, e = json.Marshal(state)
	die(e)
	die(atomicRAMFile(path.Join(folder, "link-session.json"), data))
	if state.Role == "host" {
		die(notify(address, "host_ready"))
		for attempt := 0; attempt < 120; attempt++ {
			exists, e := remoteExists(address, "/tmp/host_ready")
			die(e)
			if !exists {
				return
			}
			time.Sleep(250 * time.Millisecond)
		}
		die(errors.New("peer did not acknowledge first link-ready marker"))
	} else {
		_, e = waitFile(path.Join(folder, "inbox/host_ready"), 40)
		die(e)
		die(os.Remove(path.Join(folder, "inbox/host_ready")))
	}
}
func crc32IEEE(data []byte) uint32 { return crc32.ChecksumIEEE(data) }
func persistLink(state LinkSession, sram, rtc []byte) error {
	if len(sram) != state.OwnSRAM || len(rtc) != 4 {
		return errors.New("updated link battery has wrong size")
	}
	folder := "/mnt/SDCARD/Saves/LinkCable"
	if e := os.MkdirAll(folder, 0700); e != nil {
		return e
	}
	target := path.Join(folder, state.Hash+".link")
	if old, e := readBounded(target, 208+131072+4); e == nil {
		if e = atomicRAMFile(path.Join(folder, state.Hash+".previous"), old); e != nil {
			return e
		}
	} else if !os.IsNotExist(e) {
		return e
	}
	if e := atomicRAMFile(target, linkRecord(state.Hash, sram, rtc)); e != nil {
		return e
	}
	f, e := os.Open(target)
	if e != nil {
		return e
	}
	defer f.Close()
	if e = f.Sync(); e != nil {
		return e
	}
	// Linux UAPI: asm-generic (ARM64) 267; x86_64 306. The helper is built
	// only for these two architectures, without a libc dependency.
	number := uintptr(267)
	if runtime.GOARCH == "amd64" {
		number = 306
	} else if runtime.GOARCH != "arm64" {
		return errors.New("unsupported syncfs architecture")
	}
	_, _, errno := syscall.Syscall(number, f.Fd(), 0, 0)
	if errno != 0 {
		return errno
	}
	return nil
}
func linkReady(folder string) {
	var state LinkSession
	readJSON(path.Join(folder, "link-session.json"), &state)
	if state.Role == "host" {
		die(notify(state.PeerAddress, "host_ready"))
		return
	}
	_, e := waitFile(path.Join(folder, "inbox/host_ready"), 60)
	die(e)
	die(os.Remove(path.Join(folder, "inbox/host_ready")))
}
func linkFinish(folder string) {
	var state LinkSession
	readJSON(path.Join(folder, "link-session.json"), &state)
	if state.Role == "host" {
		own, e := readBounded(path.Join(folder, "saves/player1.srm"), 131072)
		die(e)
		rtc, e := readBounded(path.Join(folder, "saves/player1.rtc"), 4)
		die(e)
		die(persistLink(state, own, rtc))
		die(notify(state.PeerAddress, "ready_to_send"))
		_, e = waitFile(path.Join(folder, "inbox/ready_to_receive"), 40)
		die(e)
		other, e := readBounded(path.Join(folder, "saves/player2.srm"), 131072)
		die(e)
		if len(other) != state.PeerSRAM {
			die(errors.New("returned peer battery size differs"))
		}
		die(put(state.PeerAddress, "/tmp/"+state.PeerSave+"_rcvd", other))
	} else {
		die(notify(state.PeerAddress, "ready_to_receive"))
		_, e := waitFile(path.Join(folder, "inbox/ready_to_send"), 40)
		die(e)
		own, e := waitFile(path.Join(folder, "inbox", state.OwnSave+"_rcvd"), 30)
		die(e)
		rtc, e := readBounded(path.Join(folder, "saves/.netplay/player2.rtc"), 4)
		die(e)
		die(persistLink(state, own, rtc))
	}
}

func atomicRAMFile(name string, data []byte) error {
	file, e := os.CreateTemp(path.Dir(name), ".link-*")
	if e != nil {
		return e
	}
	temporary := file.Name()
	defer os.Remove(temporary)
	if _, e = file.Write(data); e != nil {
		file.Close()
		return e
	}
	if e = file.Sync(); e != nil {
		file.Close()
		return e
	}
	if e = file.Close(); e != nil {
		return e
	}
	return os.Rename(temporary, name)
}
func linkUpload(mapping Export, full string) (string, int64, bool) {
	if mapping.LinkInbox == "" || (mapping.LinkRole != "host" && mapping.LinkRole != "join") {
		return "", 0, false
	}
	if !strings.HasPrefix(full, "/tmp/") || path.Dir(full) != "/tmp" {
		return "", 0, false
	}
	leaf := path.Base(full)
	switch leaf {
	case "host_ready", "ready_to_receive", "ready_to_send", "stop_now":
		return path.Join(mapping.LinkInbox, leaf), 0, true
	}
	if mapping.LinkRole == "host" && strings.HasSuffix(leaf, ".srm") && len(leaf) <= 240 {
		entries, e := os.ReadDir(mapping.LinkInbox)
		if e != nil {
			return "", 0, false
		}
		count := 0
		exists := false
		for _, entry := range entries {
			if strings.HasSuffix(entry.Name(), ".srm") {
				count++
				if entry.Name() == leaf {
					exists = true
				}
			}
		}
		if count >= 2 && !exists {
			return "", 0, false
		}
		return path.Join(mapping.LinkInbox, leaf), 131072, true
	}
	if mapping.LinkRole == "join" && leaf == mapping.LinkSave+"_rcvd" {
		return path.Join(mapping.LinkInbox, leaf), 131072, true
	}
	return "", 0, false
}
func passiveSocket(f *FTP, address string) (net.Conn, error) {
	code, line, e := f.command("EPSV")
	if e != nil || code != 229 {
		return nil, errors.New("Onion FTP passive mode failed")
	}
	begin := strings.Index(line, "(|||")
	end := strings.LastIndex(line, "|)")
	if begin < 0 || end <= begin+4 {
		return nil, errors.New("invalid passive socket")
	}
	port, e := strconv.Atoi(line[begin+4 : end])
	if e != nil || port < 1 || port > 65535 {
		return nil, errors.New("invalid passive port")
	}
	host, _, e := net.SplitHostPort(address)
	if e != nil {
		return nil, e
	}
	data, e := net.DialTimeout("tcp4", net.JoinHostPort(host, strconv.Itoa(port)), 3*time.Second)
	if e == nil {
		data.SetDeadline(time.Now().Add(15 * time.Second))
	}
	return data, e
}
func put(address, remote string, payload []byte) error {
	if strings.ContainsAny(remote, "\r\n\x00") || len(payload) > 131072 {
		return errors.New("invalid link upload")
	}
	f, e := connect(address)
	if e != nil {
		return e
	}
	defer f.Close()
	data, e := passiveSocket(f, address)
	if e != nil {
		return e
	}
	defer data.Close()
	code, _, e := f.command("STOR " + remote)
	if e != nil || (code != 150 && code != 125) {
		return errors.New("Onion link upload rejected")
	}
	if _, e = data.Write(payload); e != nil {
		return e
	}
	data.Close()
	code, _, e = f.response()
	if e != nil || code != 226 {
		return errors.New("Onion link upload incomplete")
	}
	return nil
}
func readBounded(name string, maximum int64) ([]byte, error) {
	file, e := os.Open(name)
	if e != nil {
		return nil, e
	}
	defer file.Close()
	info, e := file.Stat()
	if e != nil || !info.Mode().IsRegular() || info.Size() > maximum {
		return nil, errors.New("invalid bounded file")
	}
	data, e := io.ReadAll(io.LimitReader(file, maximum+1))
	if e != nil || int64(len(data)) > maximum {
		return nil, errors.New("file exceeds bound")
	}
	return data, nil
}
