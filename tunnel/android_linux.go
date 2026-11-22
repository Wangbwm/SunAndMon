//go:build linux

package main

import (
	"net"
	"net/netip"
	"os"
	"runtime"
	"strings"
	"time"

	_ "tailscale.com/feature/androidbin"
	_ "tailscale.com/feature/androiddns"
	"tailscale.com/net/netmon"
)

const androidDefaultCertDirs = "/apex/com.android.conscrypt/cacerts:/system/etc/security/cacerts"

func init() {
	if !isRunningOnAndroid() {
		return
	}
	ensureAndroidCertDirs()
	netmon.RegisterInterfaceGetter(androidSafeInterfaces)
}

func ensureAndroidCertDirs() {
	cur := os.Getenv("SSL_CERT_DIR")
	if cur == "" || !strings.Contains(cur, "/apex/com.android.conscrypt/cacerts") {
		_ = os.Setenv("SSL_CERT_DIR", androidDefaultCertDirs)
	}
}

func isRunningOnAndroid() bool {
	if runtime.GOOS == "android" {
		return true
	}
	if _, err := os.Stat("/dev/__properties__"); err == nil {
		return true
	}
	if _, err := os.Stat("/system/build.prop"); err == nil {
		return true
	}
	return false
}

func androidSafeInterfaces() ([]netmon.Interface, error) {
	if ifs, err := net.Interfaces(); err == nil && len(ifs) > 0 {
		ret := make([]netmon.Interface, len(ifs))
		for i := range ifs {
			ret[i].Interface = &ifs[i]
		}
		return ret, nil
	}

	var addrs []net.Addr
	v4Targets := []string{
		"223.5.5.5:53",
		"119.29.29.29:53",
		"8.8.8.8:53",
		"192.168.1.1:53",
		"10.0.0.1:53",
	}
	for _, target := range v4Targets {
		if ip, ok := probeOutboundUDP("udp4", target); ok {
			addrs = append(addrs, &net.IPNet{IP: ip.AsSlice(), Mask: net.CIDRMask(32, 32)})
			break
		}
	}
	if ip, ok := probeOutboundUDP("udp6", "[2001:4860:4860::8888]:53"); ok {
		addrs = append(addrs, &net.IPNet{IP: ip.AsSlice(), Mask: net.CIDRMask(128, 128)})
	}
	if len(addrs) == 0 {
		// Last-resort synthetic IPv4 so netmon.New never fails during brief network handoffs.
		addrs = append(addrs, &net.IPNet{
			IP:   net.IPv4(10, 0, 2, 15),
			Mask: net.CIDRMask(32, 32),
		})
	}

	return []netmon.Interface{{
		Interface: &net.Interface{
			Index: 1,
			MTU:   1500,
			Name:  "wlan0",
			Flags: net.FlagUp | net.FlagRunning | net.FlagBroadcast | net.FlagMulticast,
		},
		AltAddrs: addrs,
	}}, nil
}

func probeOutboundUDP(network, addr string) (netip.Addr, bool) {
	d := net.Dialer{Timeout: 1500 * time.Millisecond}
	c, err := d.Dial(network, addr)
	if err != nil {
		return netip.Addr{}, false
	}
	defer c.Close()
	ua, ok := c.LocalAddr().(*net.UDPAddr)
	if !ok {
		return netip.Addr{}, false
	}
	ip, ok := netip.AddrFromSlice(ua.IP)
	if !ok {
		return netip.Addr{}, false
	}
	ip = ip.Unmap()
	if ip.IsLoopback() || ip.IsUnspecified() {
		return netip.Addr{}, false
	}
	return ip, true
}
