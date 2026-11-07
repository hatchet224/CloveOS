// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only legacy RetroArch room discovery. Stop before INFO acknowledgement,
// so probing does not join a player, load a state, or transfer SRAM.
package main

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Resolve only the sole associated hotspot station, never scan the subnet.
// Onion's client can fall back from DHCP to .101 through .250.
func associatedPeerAddress(iface, stations, table string) (string, error) {
	if iface != "wlan0" || len(stations) > 65536 || len(table) > 262144 {
		return "", errors.New("invalid hotspot discovery input")
	}
	station := ""
	authorized := false
	for _, line := range strings.Split(stations, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "flags=") && strings.Contains(line, "[ASSOC]") && strings.Contains(line, "[AUTHORIZED]") {
			authorized = true
		}
		if len(line) != 17 || strings.Count(line, ":") != 5 {
			continue
		}
		mac, err := net.ParseMAC(line)
		if err != nil || len(mac) != 6 || mac[0]&1 != 0 || mac.String() == "00:00:00:00:00:00" {
			continue
		}
		if station != "" {
			return "", errors.New("multiple associated stations")
		}
		station = mac.String()
	}
	if station == "" || !authorized {
		return "", errors.New("no authorized associated station")
	}
	selected := ""
	for _, line := range strings.Split(table, "\n") {
		fields := strings.Fields(line)
		if len(fields) != 6 || fields[5] != iface {
			continue
		}
		mac, err := net.ParseMAC(fields[3])
		if err != nil || mac.String() != station {
			continue
		}
		flags, err := strconv.ParseUint(fields[2], 0, 32)
		if err != nil || flags != 2 {
			continue
		}
		hardware, err := strconv.ParseUint(fields[1], 0, 32)
		if err != nil || hardware != 1 {
			continue
		}
		ip := net.ParseIP(fields[0]).To4()
		if ip == nil || ip[0] != 192 || ip[1] != 168 || ip[2] != 100 || ip[3] < 101 || ip[3] > 250 {
			continue
		}
		if selected != "" && selected != ip.String() {
			return "", errors.New("ambiguous station address")
		}
		selected = ip.String()
	}
	if selected == "" {
		return "", errors.New("associated station address not ready")
	}
	return selected, nil
}

func tcpRoom(ctx context.Context, address string, crc uint32, core, version string) (matched bool) {
	stage := "dial"
	defer func() {
		if os.Getenv("CLOVE_NETPLAY_DIAGNOSTICS") == "1" {
			fmt.Fprintln(os.Stderr, "Room lookup:", stage, "matched:", matched)
		}
	}()
	dialer := net.Dialer{Timeout: time.Second}
	connection, e := dialer.DialContext(ctx, "tcp4", address)
	if e != nil {
		stage = "dial: " + e.Error()
		return false
	}
	defer connection.Close()
	deadline := time.Now().Add(2500 * time.Millisecond)
	if end, ok := ctx.Deadline(); ok && end.Before(deadline) {
		deadline = end
	}
	connection.SetDeadline(deadline)
	header := make([]byte, 24)
	for i, value := range []uint32{0x52414e50, (8 << 15) | 8, 1, 6, 5, 0} {
		binary.BigEndian.PutUint32(header[i*4:], value)
	}
	if _, e = connection.Write(header); e != nil {
		return false
	}
	stage = "read header"
	if _, e = io.ReadFull(connection, header); e != nil || binary.BigEndian.Uint32(header[:4]) != 0x52414e50 || binary.BigEndian.Uint32(header[12:16]) != 0 {
		return false
	}
	protocol := binary.BigEndian.Uint32(header[16:20])
	if protocol < 5 || protocol > 6 {
		return false
	}
	stage = "nickname"
	nick := make([]byte, 40)
	binary.BigEndian.PutUint32(nick[:4], 0x20)
	binary.BigEndian.PutUint32(nick[4:8], 32)
	copy(nick[8:], "Clove room lookup")
	if _, e = connection.Write(nick); e != nil {
		return false
	}
	if _, e = io.ReadFull(connection, nick); e != nil || binary.BigEndian.Uint32(nick[:4]) != 0x20 || binary.BigEndian.Uint32(nick[4:8]) != 32 {
		return false
	}
	info := make([]byte, 76)
	stage = "read info"
	if _, e = io.ReadFull(connection, info); e != nil || binary.BigEndian.Uint32(info[:4]) != 0x22 || binary.BigEndian.Uint32(info[4:8]) != 68 {
		return false
	}
	text := func(data []byte) string {
		at := bytesIndexZero(data)
		if at < 0 {
			return ""
		}
		return string(data[:at])
	}
	stage = fmt.Sprintf("metadata crc=%08x expected=%08x core=%q version=%q", binary.BigEndian.Uint32(info[8:12]), crc, text(info[12:44]), text(info[44:76]))
	return binary.BigEndian.Uint32(info[8:12]) == crc && strings.EqualFold(text(info[12:44]), core) && (version == "" || text(info[44:76]) == version)
}
func roomTargets(target string) []string {
	if ip := net.ParseIP(target).To4(); ip != nil && target != "255.255.255.255" {
		if ip.IsPrivate() || ip.IsLoopback() {
			return []string{ip.String()}
		}
		return nil
	}
	if target != "255.255.255.255" {
		return nil
	}
	interfaces, e := net.Interfaces()
	if e != nil {
		return nil
	}
	seen := map[string]bool{}
	var result []string
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addresses, e := iface.Addrs()
		if e != nil {
			continue
		}
		for _, address := range addresses {
			local, ok := address.(*net.IPNet)
			if !ok {
				continue
			}
			ip := local.IP.To4()
			if ip == nil || !ip.IsPrivate() {
				continue
			}
			for last := 1; last < 255 && len(result) < 1024; last++ {
				candidate := net.IPv4(ip[0], ip[1], ip[2], byte(last))
				// Never probe outside the actual interface subnet; cap wide networks to
				// the device's own /24 rather than scanning arbitrary private ranges.
				if !local.Contains(candidate) || candidate.Equal(ip) {
					continue
				}
				text := candidate.String()
				if !seen[text] {
					seen[text] = true
					result = append(result, text)
				}
			}
		}
	}
	return result
}
func discoverTCP(target string, crc uint32, core, version string) (string, error) {
	addresses := roomTargets(target)
	if len(addresses) == 0 {
		return "", errors.New("no local Wi-Fi room addresses")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	jobs := make(chan string)
	matches := make(chan string, 1)
	var workers sync.WaitGroup
	for i := 0; i < 32; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for address := range jobs {
				if ctx.Err() != nil {
					return
				}
				if tcpRoom(ctx, net.JoinHostPort(address, strconv.Itoa(55435)), crc, core, version) {
					select {
					case matches <- address:
						cancel()
					default:
					}
					return
				}
			}
		}()
	}
	go func() {
		defer close(jobs)
		for _, address := range addresses {
			select {
			case jobs <- address:
			case <-ctx.Done():
				return
			}
		}
	}()
	finished := make(chan struct{})
	go func() { workers.Wait(); close(finished) }()
	select {
	case address := <-matches:
		return address + "|55435", nil
	case <-finished:
		select {
		case address := <-matches:
			return address + "|55435", nil
		default:
			return "", errors.New("no matching RetroArch room")
		}
	case <-ctx.Done():
		select {
		case address := <-matches:
			return address + "|55435", nil
		default:
			return "", errors.New("local room lookup timed out")
		}
	}
}
