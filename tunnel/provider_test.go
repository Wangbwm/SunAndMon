package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestLoopbackAllocator(t *testing.T) {
	a := NewLoopbackAllocator()
	ip1, err := a.Allocate("100.64.0.1")
	if err != nil {
		t.Fatal(err)
	}
	ip1Again, err := a.Allocate("100.64.0.1")
	if err != nil || ip1Again != ip1 {
		t.Fatalf("expected stable IP, got %q vs %q (%v)", ip1, ip1Again, err)
	}
	ip2, err := a.Allocate("100.64.0.2")
	if err != nil || ip2 == ip1 {
		t.Fatalf("expected distinct IP, got %q vs %q (%v)", ip1, ip2, err)
	}
}

func TestHostPortsNormalization(t *testing.T) {
	p := HostPorts{HTTPPort: 47989}.Normalized()
	if p.HTTPSPort != 47984 || p.RTSPPort != 48010 || p.VideoPort != 47998 || p.CtrlPort != 47999 || p.AudioPort != 48000 {
		t.Fatalf("unexpected normalized ports: %+v", p)
	}
}

func TestServeUDPForward(t *testing.T) {
	// Echo UDP upstream server
	up, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer up.Close()
	go func() {
		buf := make([]byte, 1024)
		for {
			n, addr, err := up.ReadFrom(buf)
			if err != nil {
				return
			}
			_, _ = up.WriteTo(append([]byte("ECHO:"), buf[:n]...), addr)
		}
	}()

	// Local listening forwarder socket
	front, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer front.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go serveUDPForward(ctx, front, func(c context.Context) (net.Conn, error) {
		var d net.Dialer
		return d.DialContext(c, "udp4", up.LocalAddr().String())
	})

	client, err := net.Dial("udp4", front.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	_ = client.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := client.Write([]byte("frame-1")); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 128)
	n, err := client.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if string(buf[:n]) != "ECHO:frame-1" {
		t.Fatalf("unexpected reply: %q", string(buf[:n]))
	}
}

func TestManagerHTTPAuthAndCapabilities(t *testing.T) {
	mgr := newTunnelManager()
	h := mgr.handler("secret-token")

	// Unauthorized
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest("GET", "/v1/status", nil))
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}

	// Authorized capabilities
	req := httptest.NewRequest("GET", "/v1/capabilities", nil)
	req.Header.Set("Authorization", "Bearer secret-token")
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
	var caps map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &caps); err != nil {
		t.Fatal(err)
	}
	if caps["tailscale"] != true {
		t.Fatalf("expected tailscale capability true, got %+v", caps)
	}

	// Stop when stopped succeeds cleanly
	req = httptest.NewRequest("POST", "/v1/stop", bytes.NewReader(nil))
	req.Header.Set("Authorization", "Bearer secret-token")
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200 on stop, got %d", rec.Code)
	}
	var ms MultiStatus
	if err := json.Unmarshal(rec.Body.Bytes(), &ms); err != nil {
		t.Fatal(err)
	}
	if ms.Providers[ProviderTailscale].State != "stopped" || ms.Providers[ProviderEasyTier].State != "stopped" {
		t.Fatalf("expected both providers in MultiStatus: %+v", ms)
	}
}

func TestSharedLoopbackAllocatorSeparatesProviders(t *testing.T) {
	a := NewLoopbackAllocator()
	tsIP, err := a.Allocate("tailscale:100.64.0.1")
	if err != nil {
		t.Fatal(err)
	}
	etIP, err := a.Allocate("easytier:100.64.0.1")
	if err != nil {
		t.Fatal(err)
	}
	if tsIP == etIP {
		t.Fatalf("expected distinct loopback IPs per provider, got %s", tsIP)
	}
}

func TestEasyTierIdentityPersistence(t *testing.T) {
	dir := t.TempDir()
	id1, err := loadOrCreateEasyTierIdentity(dir)
	if err != nil {
		t.Fatal(err)
	}
	id2, err := loadOrCreateEasyTierIdentity(dir)
	if err != nil {
		t.Fatal(err)
	}
	if id1 != id2 {
		t.Fatalf("identity did not persist: %+v vs %+v", id1, id2)
	}
}

func TestSelectForwardPortOffsetAvoidsConflict(t *testing.T) {
	a := NewLoopbackAllocator()
	localHost, err := a.Allocate("test:conflict")
	if err != nil {
		t.Fatal(err)
	}
	// Pick a base port block whose native TCP and UDP ports are currently free.
	var basePort uint16
	for candidate := uint16(45100); candidate < 47000; candidate += 100 {
		if canBindForwardPorts(localHost, HostPorts{HTTPPort: candidate}.Normalized(), 0) {
			basePort = candidate
			break
		}
	}
	if basePort == 0 {
		t.Fatal("could not find a free test base port block")
	}

	// Occupy the HTTP TCP port to simulate local Sunshine holding 0.0.0.0:47989.
	blocker, err := net.Listen("tcp4", net.JoinHostPort(localHost, fmt.Sprintf("%d", basePort)))
	if err != nil {
		t.Fatal(err)
	}
	defer blocker.Close()

	ports := HostPorts{HTTPPort: basePort}.Normalized()
	offset, err := selectForwardPortOffset(localHost, ports)
	if err != nil {
		t.Fatalf("selectForwardPortOffset failed: %v", err)
	}
	if offset == 0 {
		t.Fatalf("expected non-zero port offset when %s:%d is occupied", localHost, basePort)
	}
	if !canBindForwardPorts(localHost, ports, offset) {
		t.Fatalf("returned offset %d is not bindable on %s", offset, localHost)
	}
}
