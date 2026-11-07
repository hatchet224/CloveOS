// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"fmt"
	"strings"
	"testing"
)

func TestAssociatedPeerAddress(t *testing.T) {
	const mac = "02:11:22:33:44:55"
	station := "Selected interface 'wlan0'\n" + mac + "\nflags=[AUTH][ASSOC][AUTHORIZED]\n"
	row := func(ip, m, flags, iface string) string {
		return fmt.Sprintf("%s 0x1 %s %s * %s\n", ip, flags, m, iface)
	}
	valid := row("192.168.100.101", mac, "0x2", "wlan0")
	cases := []struct{ name, iface, stations, arp, want string }{
		{"normal-DHCP", "wlan0", station, valid, "192.168.100.101"},
		{"stock-random-fallback", "wlan0", station, row("192.168.100.177", mac, "0x2", "wlan0"), "192.168.100.177"},
		{"fallback-upper-bound", "wlan0", strings.ReplaceAll(station, mac, strings.ToUpper(mac)), row("192.168.100.250", mac, "0x2", "wlan0"), "192.168.100.250"},
		{"ignore-unrelated-stale", "wlan0", station, row("192.168.100.101", "02:aa:bb:cc:dd:ee", "0x2", "wlan0") + row("192.168.100.177", mac, "0x2", "wlan0"), "192.168.100.177"},
		{"no-station", "wlan0", "FAIL\n", valid, ""},
		{"unauthorized-station", "wlan0", mac + "\nflags=[AUTH][ASSOC]\n", valid, ""},
		{"no-ARP", "wlan0", station, "IP address HW type Flags HW address Mask Device\n", ""},
		{"stale-incomplete-ARP", "wlan0", station, row("192.168.100.101", mac, "0x0", "wlan0"), ""},
		{"wrong-interface", "wlan0", station, row("192.168.100.101", mac, "0x2", "eth0"), ""},
		{"wrong-subnet", "wlan0", station, row("192.168.0.101", mac, "0x2", "wlan0"), ""},
		{"host-address", "wlan0", station, row("192.168.100.100", mac, "0x2", "wlan0"), ""},
		{"outside-client-range", "wlan0", station, row("192.168.100.251", mac, "0x2", "wlan0"), ""},
		{"ambiguous-old-address", "wlan0", station, valid + row("192.168.100.177", mac, "0x2", "wlan0"), ""},
		{"two-associated-stations", "wlan0", station + "02:aa:bb:cc:dd:ee\n", valid, ""},
		{"multicast-MAC", "wlan0", "01:11:22:33:44:55", valid, ""},
		{"invalid-interface", "eth0", station, valid, ""},
		{"malformed-table", "wlan0", station, "not an arp table", ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := associatedPeerAddress(c.iface, c.stations, c.arp)
			if got != c.want || (err == nil) != (c.want != "") {
				t.Fatalf("got %q, %v; want %q", got, err, c.want)
			}
		})
	}
}
