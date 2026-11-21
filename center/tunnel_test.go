package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

func TestTunnelConfigAndEndpointRegistration(t *testing.T) {
	statePath := filepath.Join(t.TempDir(), "devices.json")
	loginKey := strings.Repeat("a", 64)
	adminKey := strings.Repeat("b", 64)
	c, err := newCenter(statePath, loginKey)
	if err != nil {
		t.Fatal(err)
	}
	adminHash := sha256.Sum256([]byte(adminKey))
	c.adminHash = &adminHash
	h := c.handler()

	// Default tunnel config is empty
	req := httptest.NewRequest("GET", "/v1/tunnel-config", nil)
	req.Header.Set("Authorization", "Bearer "+loginKey)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /v1/tunnel-config status = %d", rec.Code)
	}
	var cfg tunnelConfig
	if err := json.Unmarshal(rec.Body.Bytes(), &cfg); err != nil {
		t.Fatal(err)
	}
	if cfg.Provider != "" {
		t.Fatalf("expected empty default provider, got %q", cfg.Provider)
	}

	// Admin updates default tunnel config
	body, _ := json.Marshal(tunnelConfig{
		Provider:      "easytier",
		NetworkName:   "sunandmon-mesh",
		NetworkSecret: "mesh-secret",
		Peers:         []string{"tcp://public.easytier.top:11010"},
	})
	req = httptest.NewRequest("PUT", "/admin/api/tunnel-config", bytes.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+adminKey)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("PUT /admin/api/tunnel-config status = %d body=%s", rec.Code, rec.Body.String())
	}

	// Reload from disk and confirm persistence
	reloaded, err := newCenter(statePath, loginKey)
	if err != nil {
		t.Fatal(err)
	}
	if reloaded.tunnel.Provider != "easytier" || reloaded.tunnel.NetworkName != "sunandmon-mesh" || len(reloaded.tunnel.Peers) != 1 {
		t.Fatalf("reloaded tunnel config mismatch: %+v", reloaded.tunnel)
	}

	// Validate tunnel endpoint normalization
	p, ip, err := normalizeTunnelEndpoint("tailscale", "100.64.12.34")
	if err != nil || p != "tailscale" || ip != "100.64.12.34" {
		t.Fatalf("normalizeTunnelEndpoint tailscale failed: %q %q %v", p, ip, err)
	}
	if _, _, err := normalizeTunnelEndpoint("unknown", "100.64.12.34"); err == nil {
		t.Fatal("expected unsupported provider error")
	}
	if _, _, err := normalizeTunnelEndpoint("easytier", "127.0.0.1"); err == nil {
		t.Fatal("expected loopback tunnel address rejection")
	}

	// Validate concurrent multi-tunnel endpoints (Tailscale + EasyTier simultaneously)
	primaryP, primaryA, list, err := normalizeTunnelEndpoints("", "", []tunnelEndpoint{
		{Provider: "tailscale", Address: "100.64.12.34"},
		{Provider: "easytier", Address: "10.127.45.67"},
	})
	if err != nil || primaryP != "tailscale" || primaryA != "100.64.12.34" || len(list) != 2 {
		t.Fatalf("normalizeTunnelEndpoints multi failed: %q %q %+v %v", primaryP, primaryA, list, err)
	}
}
