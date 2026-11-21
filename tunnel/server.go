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
	mu               sync.Mutex
	alloc            *LoopbackAllocator
	providers        map[ProviderType]TunnelProvider
	registry         map[ProviderType]func(*LoopbackAllocator) TunnelProvider
	desiredHostPorts *HostPorts
}

func newTunnelManager() *tunnelManager {
	alloc := NewLoopbackAllocator()
	return &tunnelManager{
		alloc:     alloc,
		providers: make(map[ProviderType]TunnelProvider),
		registry: map[ProviderType]func(*LoopbackAllocator) TunnelProvider{
			ProviderTailscale: func(a *LoopbackAllocator) TunnelProvider { return NewTailscaleProvider(a) },
			ProviderEasyTier:  func(a *LoopbackAllocator) TunnelProvider { return NewEasyTierProvider(a) },
		},
	}
}

func (m *tunnelManager) status() MultiStatus {
	m.mu.Lock()
	activeCopy := make(map[ProviderType]TunnelProvider, len(m.providers))
	for k, v := range m.providers {
		activeCopy[k] = v
	}
	desiredPorts := m.desiredHostPorts
	m.mu.Unlock()

	perProvider := map[ProviderType]Status{
		ProviderTailscale: {Provider: ProviderTailscale, State: "stopped"},
		ProviderEasyTier:  {Provider: ProviderEasyTier, State: "stopped"},
	}

	var allForwards []ForwardMapping
	for pt, p := range activeCopy {
		st := p.Status()
		// Auto-expose host ports on any provider that just finished connecting.
		if desiredPorts != nil && st.State == "connected" && !st.HostExposed {
			_ = p.ExposeHost(*desiredPorts)
			st = p.Status()
		}
		perProvider[pt] = st
		allForwards = append(allForwards, st.ActiveForwards...)
	}

	// Build summary Status for backward compatibility:
	// prefer connected > needs_auth > starting > error > stopped.
	summary := Status{Provider: ProviderNone, State: "stopped", ActiveForwards: allForwards}
	order := []ProviderType{ProviderTailscale, ProviderEasyTier}
	rank := func(s string) int {
		switch s {
		case "connected":
			return 4
		case "needs_auth":
			return 3
		case "starting":
			return 2
		case "error":
			return 1
		default:
			return 0
		}
	}
	bestRank := 0
	totalPeers := 0
	anyHostExposed := false
	for _, pt := range order {
		st := perProvider[pt]
		totalPeers += st.PeersOnline
		if st.HostExposed {
			anyHostExposed = true
		}
		if r := rank(st.State); r > bestRank {
			bestRank = r
			summary = st
		}
	}
	summary.PeersOnline = totalPeers
	summary.HostExposed = anyHostExposed
	summary.ActiveForwards = allForwards

	return MultiStatus{
		Status:    summary,
		Providers: perProvider,
	}
}

func (m *tunnelManager) stopProvider(provider ProviderType) error {
	m.mu.Lock()
	if provider == "" || provider == ProviderNone {
		all := make([]TunnelProvider, 0, len(m.providers))
		for _, p := range m.providers {
			all = append(all, p)
		}
		m.providers = make(map[ProviderType]TunnelProvider)
		m.mu.Unlock()
		var firstErr error
		for _, p := range all {
			if err := p.Stop(); err != nil && firstErr == nil {
				firstErr = err
			}
		}
		return firstErr
	}
	p, ok := m.providers[provider]
	if ok {
		delete(m.providers, provider)
	}
	m.mu.Unlock()
	if ok {
		return p.Stop()
	}
	return nil
}

func (m *tunnelManager) stop() error {
	return m.stopProvider("")
}

func (m *tunnelManager) start(cfg Config) (MultiStatus, error) {
	if cfg.Provider == "" || cfg.Provider == ProviderNone {
		_ = m.stop()
		return m.status(), nil
	}
	factory, ok := m.registry[cfg.Provider]
	if !ok {
		return MultiStatus{}, errors.New("unsupported tunnel provider")
	}
	_ = m.stopProvider(cfg.Provider)

	p := factory(m.alloc)
	if err := p.Start(nil, cfg); err != nil {
		ms := m.status()
		ms.Providers[cfg.Provider] = Status{Provider: cfg.Provider, State: "error", Error: err.Error()}
		ms.Error = err.Error()
		return ms, err
	}
	m.mu.Lock()
	m.providers[cfg.Provider] = p
	m.mu.Unlock()
	return m.status(), nil
}

func (m *tunnelManager) expose(ports HostPorts) error {
	norm := ports.Normalized()
	m.mu.Lock()
	m.desiredHostPorts = &norm
	active := make([]TunnelProvider, 0, len(m.providers))
	for _, p := range m.providers {
		active = append(active, p)
	}
	m.mu.Unlock()
	if len(active) == 0 {
		return errors.New("no tunnel provider is running")
	}
	var lastErr error
	anyOK := false
	for _, p := range active {
		if err := p.ExposeHost(norm); err != nil {
			lastErr = err
		} else {
			anyOK = true
		}
	}
	if !anyOK && lastErr != nil {
		return lastErr
	}
	return nil
}

func (m *tunnelManager) unexpose() error {
	m.mu.Lock()
	m.desiredHostPorts = nil
	active := make([]TunnelProvider, 0, len(m.providers))
	for _, p := range m.providers {
		active = append(active, p)
	}
	m.mu.Unlock()
	for _, p := range active {
		_ = p.UnexposeHost()
	}
	return nil
}

func (m *tunnelManager) ensureForward(provider ProviderType, remoteIP string, ports HostPorts) (ForwardMapping, error) {
	m.mu.Lock()
	var target TunnelProvider
	if provider != "" && provider != ProviderNone {
		target = m.providers[provider]
	} else {
		// Auto-pick first connected provider if caller didn't specify one.
		for _, pt := range []ProviderType{ProviderEasyTier, ProviderTailscale} {
			if p, ok := m.providers[pt]; ok && p.Status().State == "connected" {
				target = p
				break
			}
		}
	}
	m.mu.Unlock()
	if target == nil {
		return ForwardMapping{}, errors.New("requested tunnel provider is not running")
	}
	return target.EnsureForward(remoteIP, ports)
}

func (m *tunnelManager) closeForward(provider ProviderType, remoteIP string) error {
	m.mu.Lock()
	targets := make([]TunnelProvider, 0, len(m.providers))
	if provider != "" && provider != ProviderNone {
		if p, ok := m.providers[provider]; ok {
			targets = append(targets, p)
		}
	} else {
		for _, p := range m.providers {
			targets = append(targets, p)
		}
	}
	m.mu.Unlock()
	for _, p := range targets {
		_ = p.CloseForward(remoteIP)
	}
	return nil
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

	mux.HandleFunc("POST /v1/stop", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Provider ProviderType `json:"provider,omitempty"`
		}
		if r.Body != nil {
			_ = json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&req)
		}
		_ = m.stopProvider(req.Provider)
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
			Provider ProviderType `json:"provider,omitempty"`
			RemoteIP string       `json:"remoteIp"`
			Ports    HostPorts    `json:"ports"`
		}
		if !readJSON(w, r, &req) {
			return
		}
		mapping, err := m.ensureForward(req.Provider, req.RemoteIP, req.Ports)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, mapping)
	})

	mux.HandleFunc("DELETE /v1/forward/{ip}", func(w http.ResponseWriter, r *http.Request) {
		provider := ProviderType(r.URL.Query().Get("provider"))
		_ = m.closeForward(provider, r.PathValue("ip"))
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
