package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"
	"sync"
)

type tunnelManager struct {
	mu       sync.Mutex
	active   TunnelProvider
	registry map[ProviderType]func() TunnelProvider
}

func newTunnelManager() *tunnelManager {
	return &tunnelManager{
		registry: map[ProviderType]func() TunnelProvider{
			ProviderTailscale: func() TunnelProvider { return NewTailscaleProvider() },
			ProviderEasyTier:  func() TunnelProvider { return NewEasyTierProvider() },
		},
	}
}

func (m *tunnelManager) status() Status {
	m.mu.Lock()
	p := m.active
	m.mu.Unlock()
	if p == nil {
		return Status{Provider: ProviderNone, State: "stopped"}
	}
	return p.Status()
}

func (m *tunnelManager) stop() error {
	m.mu.Lock()
	p := m.active
	m.active = nil
	m.mu.Unlock()
	if p != nil {
		return p.Stop()
	}
	return nil
}

func (m *tunnelManager) start(cfg Config) (Status, error) {
	if cfg.Provider == "" || cfg.Provider == ProviderNone {
		_ = m.stop()
		return m.status(), nil
	}
	factory, ok := m.registry[cfg.Provider]
	if !ok {
		return Status{}, errors.New("unsupported tunnel provider")
	}
	_ = m.stop()

	p := factory()
	if err := p.Start(nil, cfg); err != nil {
		return Status{Provider: cfg.Provider, State: "error", Error: err.Error()}, err
	}
	m.mu.Lock()
	m.active = p
	m.mu.Unlock()
	return p.Status(), nil
}

func (m *tunnelManager) expose(ports HostPorts) error {
	m.mu.Lock()
	p := m.active
	m.mu.Unlock()
	if p == nil {
		return errors.New("tunnel is not running")
	}
	return p.ExposeHost(ports)
}

func (m *tunnelManager) unexpose() error {
	m.mu.Lock()
	p := m.active
	m.mu.Unlock()
	if p == nil {
		return nil
	}
	return p.UnexposeHost()
}

func (m *tunnelManager) ensureForward(remoteIP string, ports HostPorts) (ForwardMapping, error) {
	m.mu.Lock()
	p := m.active
	m.mu.Unlock()
	if p == nil {
		return ForwardMapping{}, errors.New("tunnel is not running")
	}
	return p.EnsureForward(remoteIP, ports)
}

func (m *tunnelManager) closeForward(remoteIP string) error {
	m.mu.Lock()
	p := m.active
	m.mu.Unlock()
	if p == nil {
		return nil
	}
	return p.CloseForward(remoteIP)
}

func (m *tunnelManager) handler(token string) http.Handler {
	tokenHash := sha256.Sum256([]byte(token))
	mux := http.NewServeMux()

	writeJSON := func(w http.ResponseWriter, code int, v any) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(code)
		_ = json.NewEncoder(w).Encode(v)
	}
	readJSON := func(w http.ResponseWriter, r *http.Request, dst any) bool {
		dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
		if err := dec.Decode(dst); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return false
		}
		return true
	}

	mux.HandleFunc("GET /v1/capabilities", func(w http.ResponseWriter, r *http.Request) {
		core, cli := findEasyTierBinaries(r.URL.Query().Get("coreBinary"))
		writeJSON(w, http.StatusOK, map[string]any{
			"tailscale":        true,
			"easytier":         core != "" && cli != "",
			"easytierCorePath": core,
		})
	})

	mux.HandleFunc("GET /v1/status", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, m.status())
	})

	mux.HandleFunc("POST /v1/start", func(w http.ResponseWriter, r *http.Request) {
		var cfg Config
		if !readJSON(w, r, &cfg) {
			return
		}
		st, err := m.start(cfg)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, st)
			return
		}
		writeJSON(w, http.StatusOK, st)
	})

	mux.HandleFunc("POST /v1/stop", func(w http.ResponseWriter, _ *http.Request) {
		_ = m.stop()
		writeJSON(w, http.StatusOK, m.status())
	})

	mux.HandleFunc("POST /v1/expose", func(w http.ResponseWriter, r *http.Request) {
		var ports HostPorts
		if !readJSON(w, r, &ports) {
			return
		}
		if err := m.expose(ports); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, m.status())
	})

	mux.HandleFunc("POST /v1/unexpose", func(w http.ResponseWriter, _ *http.Request) {
		_ = m.unexpose()
		writeJSON(w, http.StatusOK, m.status())
	})

	mux.HandleFunc("POST /v1/forward", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			RemoteIP string    `json:"remoteIp"`
			Ports    HostPorts `json:"ports"`
		}
		if !readJSON(w, r, &req) {
			return
		}
		mapping, err := m.ensureForward(req.RemoteIP, req.Ports)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, mapping)
	})

	mux.HandleFunc("DELETE /v1/forward/{ip}", func(w http.ResponseWriter, r *http.Request) {
		_ = m.closeForward(r.PathValue("ip"))
		w.WriteHeader(http.StatusNoContent)
	})

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		if token != "" {
			got, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
			gotHash := sha256.Sum256([]byte(got))
			if !ok || !hmac.Equal(gotHash[:], tokenHash[:]) {
				writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
				return
			}
		}
		mux.ServeHTTP(w, r)
	})
}
