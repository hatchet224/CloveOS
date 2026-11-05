// SPDX-License-Identifier: GPL-3.0-or-later
// Onion 4.3.1-1 cookie/FTP transport. Only the selected ROM and pinned ARM32
// core are exported; no SD-card directory or personal configuration is served.
package main

import (
	"archive/zip"
	"bufio"
	"bytes"
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
	"strconv"
	"strings"
	"syscall"
	"time"
)

func rawContentCRC(filename string) (uint32, error) {
	f, e := os.Open(filename)
	if e != nil {
		return 0, e
	}
	defer f.Close()
	hash := crc32.NewIEEE()
	_, e = io.Copy(hash, f)
	return hash.Sum32(), e
}
func lanContentCRC(filename, core string) (uint32, error) {
	// Arcade cores keep their multi-ROM archive intact (block_extract=true).
	// RetroArch advertises that ZIP's CRC, rather than a cartridge member CRC.
	if strings.HasPrefix(core, "FB Alpha 2012") || core == "FinalBurn Neo" {
		return rawContentCRC(filename)
	}
	return contentCRC(filename)
}
func contentCRC(filename string) (uint32, error) {
	if strings.EqualFold(path.Ext(filename), ".zip") {
		archive, e := zip.OpenReader(filename)
		if e != nil {
			return 0, e
		}
		defer archive.Close()
		var selected *zip.File
		for _, f := range archive.File {
			if !f.FileInfo().IsDir() {
				if selected != nil {
					return 0, errors.New("LAN matching requires a single-cartridge ZIP")
				}
				selected = f
			}
		}
		if selected == nil || selected.UncompressedSize64 > 64*1024*1024 {
			return 0, errors.New("invalid ZIP cartridge")
		}
		data, e := selected.Open()
		if e != nil {
			return 0, e
		}
		defer data.Close()
		hash := crc32.NewIEEE()
		_, e = io.Copy(hash, io.LimitReader(data, 64*1024*1024+1))
		return hash.Sum32(), e
	}
	f, e := os.Open(filename)
	if e != nil {
		return 0, e
	}
	defer f.Close()
	hash := crc32.NewIEEE()
	_, e = io.Copy(hash, f)
	return hash.Sum32(), e
}
func payloadCRC(data []byte, extension string) (uint32, error) {
	if !strings.EqualFold(extension, ".zip") {
		return crc32.ChecksumIEEE(data), nil
	}
	archive, e := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if e != nil {
		return 0, e
	}
	var selected *zip.File
	for _, entry := range archive.File {
		if !entry.FileInfo().IsDir() {
			if selected != nil {
				return 0, errors.New("ambiguous cartridge ZIP")
			}
			selected = entry
		}
	}
	if selected == nil || selected.UncompressedSize64 > 64*1024*1024 {
		return 0, errors.New("invalid cartridge ZIP")
	}
	reader, e := selected.Open()
	if e != nil {
		return 0, e
	}
	defer reader.Close()
	digest := crc32.NewIEEE()
	count, e := io.Copy(digest, io.LimitReader(reader, 64*1024*1024+1))
	if e != nil || count > 64*1024*1024 {
		return 0, errors.New("ZIP cartridge exceeds bound")
	}
	return digest.Sum32(), nil
}
func advertisement(packet []byte, crc uint32, core, version string) (int, bool) {
	if len(packet) != 688 || binary.BigEndian.Uint32(packet[:4]) != 0x52414e53 || binary.BigEndian.Uint32(packet[4:8]) != crc || binary.BigEndian.Uint32(packet[12:16]) != 0 {
		return 0, false
	}
	text := func(data []byte) string {
		if at := bytesIndexZero(data); at >= 0 {
			return string(data[:at])
		}
		return ""
	}
	subsystem := text(packet[432:688])
	if text(packet[80:112]) != core || (version != "" && text(packet[112:144]) != version) || (subsystem != "" && subsystem != "N/A") {
		return 0, false
	}
	port := int(binary.BigEndian.Uint32(packet[8:12]))
	return port, port > 0 && port < 65536
}
func bytesIndexZero(data []byte) int {
	for i, b := range data {
		if b == 0 {
			return i
		}
	}
	return -1
}
func findLAN(filename, core, version, target string) {
	crc, e := lanContentCRC(filename, core)
	die(e)
	socket, e := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
	die(e)
	defer socket.Close()
	raw, e := socket.SyscallConn()
	die(e)
	var optionError error
	die(raw.Control(func(fd uintptr) {
		optionError = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_BROADCAST, 1)
	}))
	die(optionError)
	ip := net.ParseIP(target).To4()
	if ip == nil {
		die(errors.New("invalid LAN discovery address"))
	}
	for attempt := 0; attempt < 8; attempt++ {
		socket.SetDeadline(time.Now().Add(time.Second))
		_, e = socket.WriteToUDP([]byte{0x52, 0x41, 0x4e, 0x51}, &net.UDPAddr{IP: ip, Port: 55435})
		if e != nil {
			die(e)
		}
		for {
			buffer := make([]byte, 689)
			size, remote, e := socket.ReadFromUDP(buffer)
			if e != nil {
				break
			}
			if !remote.IP.IsPrivate() && !remote.IP.IsLoopback() {
				continue
			}
			if port, ok := advertisement(buffer[:size], crc, core, version); ok {
				fmt.Printf("%s|%d\n", remote.IP.String(), port)
				return
			}
		}
	}
	// Some shipped Onion RetroArch builds omit the optional UDP responder.
	// Their ordinary TCP handshake still provides exact core/version/content
	// metadata. Probe that metadata only, without entering a player session.
	if address, e := discoverTCP(target, crc, core, version); e == nil {
		fmt.Println(address)
		return
	}
	os.Exit(20)
}

func dhcpReply(request []byte, server net.IP) []byte {
	if len(request) < 240 || len(request) > 1500 || request[0] != 1 || request[1] != 1 || request[2] != 6 ||
		binary.BigEndian.Uint32(request[236:240]) != 0x63825363 {
		return nil
	}
	kind := byte(0)
	for i := 240; i < len(request); {
		code := request[i]
		i++
		if code == 255 {
			break
		}
		if code == 0 {
			continue
		}
		if i >= len(request) {
			return nil
		}
		size := int(request[i])
		i++
		if i+size > len(request) {
			return nil
		}
		if code == 53 && size == 1 {
			kind = request[i]
		}
		if code == 54 && (size != 4 || !net.IP(request[i:i+size]).Equal(server)) {
			return nil
		}
		i += size
	}
	if kind != 1 && kind != 3 {
		return nil
	}
	response := make([]byte, 240)
	copy(response, request[:240])
	response[0] = 2
	response[8] = 0
	response[9] = 0
	response[10] = 0x80
	response[11] = 0
	for i := 12; i < 28; i++ {
		response[i] = 0
	}
	host := server.To4()
	if host == nil {
		return nil
	}
	copy(response[16:20], host)
	response[19] = 101
	copy(response[20:24], host)
	message := byte(2)
	if kind == 3 {
		message = 5
	}
	response = append(response, 53, 1, message, 54, 4, host[0], host[1], host[2], host[3],
		51, 4, 0, 0, 14, 16, 1, 4, 255, 255, 255, 0, 3, 4, host[0], host[1], host[2], host[3], 255)
	for len(response) < 300 {
		response = append(response, 0)
	}
	return response
}
func dhcp(server, iface string) {
	ip := net.ParseIP(server).To4()
	if ip == nil || iface != "wlan0" {
		die(errors.New("invalid hotspot DHCP interface"))
	}
	connection, e := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero, Port: 67})
	die(e)
	defer connection.Close()
	raw, e := connection.SyscallConn()
	die(e)
	var socketError error
	die(raw.Control(func(fd uintptr) {
		socketError = syscall.SetsockoptString(int(fd), syscall.SOL_SOCKET, syscall.SO_BINDTODEVICE, iface)
	}))
	die(socketError)
	die(raw.Control(func(fd uintptr) {
		socketError = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_BROADCAST, 1)
	}))
	die(socketError)
	packet := make([]byte, 1501)
	for {
		size, _, e := connection.ReadFromUDP(packet)
		if e != nil {
			die(e)
		}
		reply := dhcpReply(packet[:size], ip)
		if reply != nil {
			_, e = connection.WriteToUDP(reply, &net.UDPAddr{IP: net.IPv4bcast, Port: 68})
			if e != nil {
				fmt.Fprintln(os.Stderr, "hotspot DHCP:", e)
			}
		}
	}
}

type Core struct{ Name, ARM32, ARM64, SHA32, SHA64, Version string }
type Export struct {
	Cookie                        string
	Files                         map[string]string
	LinkInbox, LinkRole, LinkSave string
}

func die(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(21)
	}
}
func readJSON(name string, value any) {
	data, e := os.ReadFile(name)
	die(e)
	die(json.Unmarshal(data, value))
}
func fileDigest(name string) (string, string, error) {
	f, e := os.Open(name)
	if e != nil {
		return "", "", e
	}
	defer f.Close()
	sha, crc := sha256.New(), crc32.NewIEEE()
	_, e = io.Copy(io.MultiWriter(sha, crc), f)
	// Stock Onion's xcrc prints lowercase hexadecimal and compares it as text.
	return hex.EncodeToString(sha.Sum(nil)), fmt.Sprintf("%08x", crc.Sum32()), e
}
func cores(name string) []Core { var rows []Core; readJSON(name, &rows); return rows }
func pinned(row Core) error {
	for _, v := range [][2]string{{row.ARM32, row.SHA32}, {row.ARM64, row.SHA64}} {
		actual, _, e := fileDigest(v[0])
		if e != nil {
			return e
		}
		if actual != v[1] {
			return errors.New("netplay core differs from pinned Onion revision")
		}
	}
	return nil
}
func prepare(registry, leaf, rom, folder string) {
	var core *Core
	for _, row := range cores(registry) {
		if row.Name == leaf {
			r := row
			core = &r
			break
		}
	}
	if core == nil {
		die(errors.New("this core has no verified Onion 4.3.1-1 peer build"))
	}
	die(pinned(*core))
	if !strings.HasPrefix(rom, "/mnt/SDCARD/Roms/") || strings.ContainsAny(rom, "\r\n") {
		die(errors.New("invalid selected game path"))
	}
	info, e := os.Stat(rom)
	die(e)
	if !info.Mode().IsRegular() {
		die(errors.New("selected game is not a regular file"))
	}
	_, romCRC, e := fileDigest(rom)
	die(e)
	if info.Size() > 26214400 {
		romCRC = "0"
	}
	_, coreCRC, e := fileDigest(core.ARM32)
	die(e)
	remoteCore := "/mnt/SDCARD/RetroArch/.retroarch/cores/" + core.Name
	cookie := fmt.Sprintf("[core]: %s\n[rom]: %s\n[corechksum]: %s\n[romchksum]: %s\n", remoteCore, rom, coreCRC, romCRC)
	cookieFile := folder + "/retroarch.cookie"
	die(os.WriteFile(cookieFile, []byte(cookie), 0600))
	mapping := Export{Cookie: cookieFile, Files: map[string]string{"/mnt/SDCARD/RetroArch/retroarch.cookie": cookieFile, remoteCore: core.ARM32, rom: rom}}
	data, e := json.Marshal(mapping)
	die(e)
	die(os.WriteFile(folder+"/export.json", data, 0600))
	fmt.Println(core.ARM64)
}
func reply(c net.Conn, code int, text string) error {
	_, e := fmt.Fprintf(c, "%d %s\r\n", code, text)
	return e
}
func exportedFile(mapping Export, full string) (string, bool) {
	if name, ok := mapping.Files[full]; ok {
		return name, true
	}
	name, _, ok := linkUpload(mapping, full)
	return name, ok
}
func servePeer(c net.Conn, mapping Export, ip net.IP) {
	defer c.Close()
	_ = reply(c, 220, "CloveOS Onion-compatible selected-game export")
	var passive net.Listener
	defer func() {
		if passive != nil {
			passive.Close()
		}
	}()
	cwd := "/"
	reader := bufio.NewReaderSize(c, 4096)
	for count := 0; count < 120; count++ {
		c.SetDeadline(time.Now().Add(30 * time.Second))
		line, e := reader.ReadString('\n')
		if e != nil || len(line) > 4096 {
			return
		}
		pieces := strings.SplitN(strings.TrimRight(line, "\r\n"), " ", 2)
		cmd := strings.ToUpper(pieces[0])
		arg := ""
		if len(pieces) == 2 {
			arg = pieces[1]
		}
		full := path.Clean(path.Join(cwd, arg))
		if strings.HasPrefix(arg, "/") {
			full = path.Clean(arg)
		}
		switch cmd {
		case "USER":
			_ = reply(c, 331, "Anonymous session")
		case "PASS":
			_ = reply(c, 230, "Read-only selected-game session")
		case "QUIT":
			_ = reply(c, 221, "Goodbye")
			return
		case "SYST":
			_ = reply(c, 215, "UNIX Type: L8")
		case "FEAT":
			io.WriteString(c, "211-Features\r\n EPSV\r\n SIZE\r\n211 End\r\n")
		case "TYPE", "OPTS", "NOOP":
			_ = reply(c, 200, "OK")
		case "PWD":
			_ = reply(c, 257, "\""+cwd+"\"")
		case "CWD":
			found := full == "/"
			for key := range mapping.Files {
				if strings.HasPrefix(key, strings.TrimSuffix(full, "/")+"/") {
					found = true
				}
			}
			if found {
				cwd = full
				_ = reply(c, 250, "Directory selected")
			} else {
				_ = reply(c, 550, "Unavailable")
			}
		case "SIZE", "MDTM":
			file, ok := exportedFile(mapping, full)
			if !ok {
				_ = reply(c, 550, "Unavailable")
				continue
			}
			info, e := os.Stat(file)
			if e != nil || !info.Mode().IsRegular() {
				_ = reply(c, 550, "Unavailable")
				continue
			}
			if cmd == "SIZE" {
				_ = reply(c, 213, strconv.FormatInt(info.Size(), 10))
			} else {
				_ = reply(c, 213, info.ModTime().UTC().Format("20060102150405"))
			}
		case "PASV", "EPSV":
			if passive != nil {
				passive.Close()
			}
			passive, e = net.Listen("tcp4", net.JoinHostPort(ip.String(), "0"))
			if e != nil {
				_ = reply(c, 425, "No passive socket")
				continue
			}
			port := passive.Addr().(*net.TCPAddr).Port
			if cmd == "EPSV" {
				_ = reply(c, 229, fmt.Sprintf("Entering Extended Passive Mode (|||%d|)", port))
			} else {
				v := ip.To4()
				_ = reply(c, 227, fmt.Sprintf("Entering Passive Mode (%d,%d,%d,%d,%d,%d)", v[0], v[1], v[2], v[3], port/256, port%256))
			}
		case "STOR":
			name, maximum, ok := linkUpload(mapping, full)
			if !ok || passive == nil {
				_ = reply(c, 502, "Upload unavailable")
				continue
			}
			_ = reply(c, 150, "Opening temporary link inbox")
			passive.(*net.TCPListener).SetDeadline(time.Now().Add(10 * time.Second))
			data, e := passive.Accept()
			passive.Close()
			passive = nil
			if e != nil {
				_ = reply(c, 425, "Data timeout")
				continue
			}
			if !data.RemoteAddr().(*net.TCPAddr).IP.Equal(c.RemoteAddr().(*net.TCPAddr).IP) {
				data.Close()
				return
			}
			data.SetDeadline(time.Now().Add(30 * time.Second))
			payload, e := io.ReadAll(io.LimitReader(data, maximum+1))
			data.Close()
			if e != nil || int64(len(payload)) > maximum {
				_ = reply(c, 552, "Link upload exceeds bound")
				continue
			}
			if e = atomicRAMFile(name, payload); e != nil {
				_ = reply(c, 451, "Temporary link inbox unavailable")
			} else {
				_ = reply(c, 226, "Link upload received")
			}
		case "RETR":
			name, ok := exportedFile(mapping, full)
			if !ok || passive == nil {
				_ = reply(c, 550, "Unavailable")
				continue
			}
			f, e := os.Open(name)
			if e != nil {
				_ = reply(c, 550, "Unavailable")
				continue
			}
			_ = reply(c, 150, "Opening selected file")
			passive.(*net.TCPListener).SetDeadline(time.Now().Add(10 * time.Second))
			data, e := passive.Accept()
			passive.Close()
			passive = nil
			if e != nil {
				f.Close()
				_ = reply(c, 425, "Data timeout")
				continue
			}
			if !data.RemoteAddr().(*net.TCPAddr).IP.Equal(c.RemoteAddr().(*net.TCPAddr).IP) {
				data.Close()
				f.Close()
				return
			}
			data.SetDeadline(time.Now().Add(120 * time.Second))
			_, e = io.Copy(data, f)
			data.Close()
			f.Close()
			if e != nil {
				_ = reply(c, 426, "Transfer interrupted")
			} else {
				_ = reply(c, 226, "Transfer complete")
			}
		default:
			_ = reply(c, 502, "Command unavailable in read-only game export")
		}
	}
}
func serve(address, filename string) {
	var mapping Export
	readJSON(filename, &mapping)
	if len(mapping.Files) != 3 {
		die(errors.New("export must contain exactly cookie, core and game"))
	}
	listener, e := net.Listen("tcp4", address)
	die(e)
	defer listener.Close()
	ip := listener.Addr().(*net.TCPAddr).IP
	if ip.IsUnspecified() {
		die(errors.New("export must bind to the temporary hotspot address"))
	}
	slots := make(chan struct{}, 4)
	for {
		c, e := listener.Accept()
		if e != nil {
			die(e)
		}
		select {
		case slots <- struct{}{}:
			go func() { defer func() { <-slots }(); servePeer(c, mapping, ip) }()
		default:
			c.Close()
		}
	}
}

type FTP struct {
	net.Conn
	reader *bufio.Reader
}

func (f *FTP) response() (int, string, error) {
	line, e := f.reader.ReadString('\n')
	if e != nil {
		return 0, "", e
	}
	if len(line) < 4 || len(line) > 4096 {
		return 0, "", errors.New("invalid FTP response")
	}
	code, e := strconv.Atoi(line[:3])
	if e != nil {
		return 0, "", e
	}
	if line[3] == '-' {
		for i := 0; i < 30; i++ {
			tail, e := f.reader.ReadString('\n')
			if e != nil {
				return 0, "", e
			}
			if strings.HasPrefix(tail, line[:3]+" ") {
				return code, tail, nil
			}
		}
		return 0, "", errors.New("FTP response too long")
	}
	return code, line, nil
}
func (f *FTP) command(cmd string) (int, string, error) {
	_, e := fmt.Fprintf(f.Conn, "%s\r\n", cmd)
	if e != nil {
		return 0, "", e
	}
	return f.response()
}
func connect(address string) (*FTP, error) {
	c, e := net.DialTimeout("tcp4", address, 3*time.Second)
	if e != nil {
		return nil, e
	}
	c.SetDeadline(time.Now().Add(20 * time.Second))
	f := &FTP{c, bufio.NewReaderSize(c, 4096)}
	code, _, e := f.response()
	if e != nil || code != 220 {
		c.Close()
		return nil, errors.New("Onion FTP greeting unavailable")
	}
	code, _, e = f.command("USER anonymous")
	if e == nil && code == 331 {
		code, _, e = f.command("PASS CloveOS@local")
	}
	if e != nil || code != 230 {
		c.Close()
		return nil, errors.New("Onion FTP login failed")
	}
	code, _, e = f.command("TYPE I")
	if e != nil || code != 200 {
		c.Close()
		return nil, errors.New("Onion FTP binary mode failed")
	}
	return f, nil
}
func fetch(address, remote string, maximum int64) ([]byte, error) {
	f, e := connect(address)
	if e != nil {
		return nil, e
	}
	defer f.Close()
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
	if e != nil {
		return nil, e
	}
	defer data.Close()
	data.SetDeadline(time.Now().Add(15 * time.Second))
	code, _, e = f.command("RETR " + remote)
	if e != nil || (code != 150 && code != 125) {
		return nil, errors.New("Onion selected-game cookie unavailable")
	}
	result, e := io.ReadAll(io.LimitReader(data, maximum+1))
	data.Close()
	if e != nil || int64(len(result)) > maximum {
		return nil, errors.New("FTP file exceeds bound or failed")
	}
	code, _, e = f.response()
	if e != nil || code != 226 {
		return nil, errors.New("FTP transfer did not complete")
	}
	return result, nil
}
func check(address, registry, rom, folder string) {
	data, e := fetch(address, "/mnt/SDCARD/RetroArch/retroarch.cookie", 4096)
	die(e)
	fields := map[string]string{}
	for _, line := range strings.Split(string(data), "\n") {
		if line == "" {
			continue
		}
		parts := strings.SplitN(line, ": ", 2)
		if len(parts) != 2 || strings.ContainsAny(parts[1], "\r\x00") {
			die(errors.New("invalid Onion cookie"))
		}
		if _, ok := fields[parts[0]]; ok {
			die(errors.New("duplicate cookie field"))
		}
		fields[parts[0]] = parts[1]
	}
	coreName := path.Base(fields["[core]"])
	var selected *Core
	for _, row := range cores(registry) {
		if row.Name == coreName {
			r := row
			selected = &r
			break
		}
	}
	if selected == nil {
		die(errors.New("host core has no matching ARM64 Onion build"))
	}
	die(pinned(*selected))
	_, crc, e := fileDigest(selected.ARM32)
	die(e)
	if !strings.EqualFold(crc, fields["[corechksum]"]) {
		die(errors.New("host core revision differs from Onion 4.3.1-1"))
	}
	_, crc, e = fileDigest(rom)
	die(e)
	if !strings.EqualFold(crc, fields["[romchksum]"]) {
		if coreName == "fbalpha2012_neogeo_libretro.so" {
			die(errors.New("select the exact Neo Geo ROM ZIP used by the Onion host"))
		}
		remote := fields["[rom]"]
		if !strings.HasPrefix(remote, "/mnt/SDCARD/Roms/") || fields["[romchksum]"] == "0" {
			die(errors.New("host game checksum is unavailable"))
		}
		payload, e := fetch(address, remote, 26214400)
		die(e)
		if !strings.EqualFold(fmt.Sprintf("%08x", crc32.ChecksumIEEE(payload)), fields["[romchksum]"]) {
			die(errors.New("host game changed during comparison"))
		}
		remoteCRC, e := payloadCRC(payload, path.Ext(remote))
		die(e)
		localCRC, e := contentCRC(rom)
		die(e)
		if localCRC != remoteCRC {
			die(errors.New("select the same game revision as the Onion host (cartridge CRC mismatch)"))
		}
	}
	die(os.WriteFile(folder+"/remote.cookie", data, 0600))
	fmt.Println(selected.ARM64)
}
func main() {
	if len(os.Args) < 2 {
		os.Exit(2)
	}
	switch os.Args[1] {
	case "link-init":
		if len(os.Args) != 6 {
			os.Exit(2)
		}
		linkInit(os.Args[2], os.Args[3], os.Args[4], os.Args[5])
	case "link-pair":
		if len(os.Args) != 4 {
			os.Exit(2)
		}
		linkPair(os.Args[2], os.Args[3])
	case "link-ready":
		if len(os.Args) != 3 {
			os.Exit(2)
		}
		linkReady(os.Args[2])
	case "link-finish":
		if len(os.Args) != 3 {
			os.Exit(2)
		}
		linkFinish(os.Args[2])
	case "find-lan":
		if len(os.Args) != 5 && len(os.Args) != 6 {
			os.Exit(2)
		}
		target := "255.255.255.255"
		if len(os.Args) == 6 {
			target = os.Args[5]
		}
		findLAN(os.Args[2], os.Args[3], os.Args[4], target)
	case "dhcp":
		if len(os.Args) != 4 {
			os.Exit(2)
		}
		dhcp(os.Args[2], os.Args[3])
	case "prepare":
		if len(os.Args) != 6 {
			os.Exit(2)
		}
		prepare(os.Args[2], os.Args[3], os.Args[4], os.Args[5])
	case "serve":
		if len(os.Args) != 4 {
			os.Exit(2)
		}
		serve(os.Args[2], os.Args[3])
	case "check":
		if len(os.Args) != 6 {
			os.Exit(2)
		}
		check(os.Args[2], os.Args[3], os.Args[4], os.Args[5])
	case "fetch":
		if len(os.Args) != 4 {
			os.Exit(2)
		}
		data, e := fetch(os.Args[2], os.Args[3], 4096)
		die(e)
		os.Stdout.Write(data)
	default:
		os.Exit(2)
	}
}
