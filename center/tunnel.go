package main

import (
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

type tunnelConfig struct {
	Provider      string   `json:"provider"`
	ControlURL    string   `json:"controlUrl,omitempty"`
	AuthKey       string   `json:"authKey,omitempty"`
	NetworkName   string   `json:"networkName,omitempty"`
	NetworkSecret string   `json:"networkSecret,omitempty"`
	Peers         []string `json:"peers,omitempty"`
}

func validTunnelProvider(p string) bool {
	return p == "" || p == "none" || p == "tailscale" || p == "easytier"
}

func normalizeTunnelEndpoint(provider, address string) (string, string, error) {
	p := strings.ToLower(strings.TrimSpace(provider))
	a := strings.TrimSpace(address)
	if p == "" || p == "none" {
		if a != "" {
			return "", "", errors.New("tunnelAddress requires tunnelProvider")
		}
		return "", "", nil
	}
	if p != "tailscale" && p != "easytier" {
		return "", "", errors.New("unsupported tunnelProvider")
	}
	ip, err := netip.ParseAddr(a)
	if err != nil || ip.Zone() != "" || !ip.Unmap().IsGlobalUnicast() {
		return "", "", errors.New("invalid tunnelAddress")
	}
	return p, ip.Unmap().String(), nil
}

func normalizeTunnelConfig(cfg tunnelConfig) (tunnelConfig, error) {
	out := tunnelConfig{
		Provider:      strings.ToLower(strings.TrimSpace(cfg.Provider)),
		ControlURL:    strings.TrimSpace(cfg.ControlURL),
		AuthKey:       strings.TrimSpace(cfg.AuthKey),
		NetworkName:   strings.TrimSpace(cfg.NetworkName),
		NetworkSecret: strings.TrimSpace(cfg.NetworkSecret),
	}
	if out.Provider == "none" {
		out.Provider = ""
	}
	if !validTunnelProvider(out.Provider) {
		return tunnelConfig{}, errors.New("unsupported tunnel provider")
	}
	if out.ControlURL != "" {
		u, err := url.Parse(out.ControlURL)
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			return tunnelConfig{}, errors.New("invalid controlUrl")
		}
	}
	if len(cfg.Peers) > 16 {
		return tunnelConfig{}, errors.New("too many tunnel peers")
	}
	for _, peer := range cfg.Peers {
		peer = strings.TrimSpace(peer)
		if peer == "" {
			continue
		}
		if len(peer) > 256 || strings.ContainsAny(peer, "\r\n\t ") {
			return tunnelConfig{}, errors.New("invalid tunnel peer")
		}
		out.Peers = append(out.Peers, peer)
	}
	if out.Provider == "" {
		return tunnelConfig{}, nil
	}
	return out, nil
}

func tunnelConfigFromEnv() (tunnelConfig, error) {
	rawPeers := strings.TrimSpace(os.Getenv("SUNANDMON_TUNNEL_PEERS"))
	var peers []string
	if rawPeers != "" {
		for _, item := range strings.Split(rawPeers, ",") {
			if s := strings.TrimSpace(item); s != "" {
				peers = append(peers, s)
			}
		}
	}
	return normalizeTunnelConfig(tunnelConfig{
		Provider:      os.Getenv("SUNANDMON_TUNNEL_PROVIDER"),
		ControlURL:    os.Getenv("SUNANDMON_TUNNEL_CONTROL_URL"),
		AuthKey:       os.Getenv("SUNANDMON_TUNNEL_AUTH_KEY"),
		NetworkName:   os.Getenv("SUNANDMON_TUNNEL_NETWORK_NAME"),
		NetworkSecret: os.Getenv("SUNANDMON_TUNNEL_NETWORK_SECRET"),
		Peers:         peers,
	})
}

func (c *center) tunnelConfigPath() string {
	return filepath.Join(filepath.Dir(c.path), "tunnel.json")
}

func (c *center) loadTunnelConfig() error {
	data, err := os.ReadFile(c.tunnelConfigPath())
	if errors.Is(err, os.ErrNotExist) {
		envCfg, envErr := tunnelConfigFromEnv()
		if envErr != nil {
			return envErr
		}
		c.tunnel = envCfg
		return nil
	}
	if err != nil {
		return err
	}
	var loaded tunnelConfig
	if err = json.Unmarshal(data, &loaded); err != nil {
		return err
	}
	normalized, err := normalizeTunnelConfig(loaded)
	if err != nil {
		return err
	}
	c.tunnel = normalized
	return nil
}

func (c *center) saveTunnelConfig() error {
	target := c.tunnelConfigPath()
	if err := os.MkdirAll(filepath.Dir(target), 0o700); err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(target), "tunnel-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if err = file.Chmod(0o600); err != nil {
		_ = file.Close()
		return err
	}
	err = json.NewEncoder(file).Encode(c.tunnel)
	if err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(file.Name(), target)
}

func (c *center) getTunnelConfig(w http.ResponseWriter, _ *http.Request) {
	c.mu.Lock()
	cfg := c.tunnel
	c.mu.Unlock()
	jsonResponse(w, cfg)
}

func (c *center) adminGetTunnelConfig(w http.ResponseWriter, _ *http.Request) {
	c.getTunnelConfig(w, nil)
}

func (c *center) adminPutTunnelConfig(w http.ResponseWriter, r *http.Request) {
	var input tunnelConfig
	if !readJSON(w, r, &input) {
		return
	}
	normalized, err := normalizeTunnelConfig(input)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	c.mu.Lock()
	old := c.tunnel
	c.tunnel = normalized
	if err := c.saveTunnelConfig(); err != nil {
		c.tunnel = old
		c.mu.Unlock()
		log.Print(err)
		http.Error(w, "could not save tunnel config", http.StatusInternalServerError)
		return
	}
	c.mu.Unlock()
	jsonResponse(w, normalized)
}
