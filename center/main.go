package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const onlineFor = 45 * time.Second

type device struct {
	ID             string           `json:"id"`
	Name           string           `json:"name"`
	Addresses      []string         `json:"addresses"`
	Port           uint16           `json:"port"`
	HTTPSPort      uint16           `json:"httpsPort"`
	Certificate    string           `json:"certificate"`
	TunnelProvider string           `json:"tunnelProvider,omitempty"`
	TunnelAddress  string           `json:"tunnelAddress,omitempty"`
	Tunnels        []tunnelEndpoint `json:"tunnels,omitempty"`
}

type registration struct {
	device
	HostKey  string        `json:"hostKey,omitempty"`
	LastSeen time.Time     `json:"lastSeen"`
	Owner    string        `json:"-"`
	Override *nodeOverride `json:"override,omitempty"`
	Deleted  bool          `json:"deleted,omitempty"`
}

type center struct {
	loginHash [sha256.Size]byte
	adminHash *[sha256.Size]byte
	path      string
	mu        sync.Mutex
	devices   map[string]registration
	tunnel    tunnelConfig
}

func newCenter(path, login string) (*center, error) {
	c := &center{path: path, loginHash: sha256.Sum256([]byte(login)), devices: make(map[string]registration)}
	if err := c.loadTunnelConfig(); err != nil {
		return nil, err
	}
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return c, nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(data, &c.devices); err != nil || c.devices == nil {
		return nil, fmt.Errorf("invalid device registry %s", path)
	}
	for id, host := range c.devices {
		host.LastSeen = time.Time{}
		c.devices[id] = host
	}
	return c, nil
}

func (c *center) save() error {
	dir := filepath.Dir(c.path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	file, err := os.CreateTemp(dir, ".devices-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	err = json.NewEncoder(file).Encode(c.devices)
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
	return os.Rename(file.Name(), c.path)
}

func isHex(s string, size int) bool {
	if len(s) != size*2 || s != strings.ToLower(s) {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}

func validDeviceName(name string) bool {
	return name != "" && utf8.RuneCountInString(name) <= 100 && !strings.ContainsAny(name, "\r\n\t")
}

func normalizeAddresses(input []string) ([]string, error) {
	result := make([]string, 0, len(input))
	for _, value := range input {
		ip, err := netip.ParseAddr(strings.TrimSpace(value))
		if err != nil || ip.Zone() != "" || !ip.Unmap().IsGlobalUnicast() {
			return nil, errors.New("invalid device IP")
		}
		addr := ip.Unmap().String()
		if !slices.Contains(result, addr) {
			result = append(result, addr)
		}
	}
	return result, nil
}

func readJSON(w http.ResponseWriter, r *http.Request, dst any) bool {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 8192))
	decoder.DisallowUnknownFields()
	if decoder.Decode(dst) != nil || decoder.Decode(new(any)) != io.EOF {
		http.Error(w, "invalid JSON", http.StatusBadRequest)
		return false
	}
	return true
}

func jsonResponse(w http.ResponseWriter, body any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(body)
}

func (c *center) handler() http.Handler {
	devices := http.NewServeMux()
	devices.HandleFunc("GET /v1/devices", c.list)
	devices.HandleFunc("PUT /v1/devices/{id}", c.register)
	devices.HandleFunc("DELETE /v1/devices/{id}", c.remove)
	devices.HandleFunc("POST /v1/devices/{id}/pair", c.pair)
	devices.HandleFunc("GET /v1/tunnel-config", c.getTunnelConfig)
	mux := http.NewServeMux()
	mux.Handle("/v1/", requireKey(&c.loginHash, devices))
	c.addAdminRoutes(mux)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		mux.ServeHTTP(w, r)
	})
}

func requireKey(expected *[sha256.Size]byte, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if expected == nil {
			http.Error(w, "node management is disabled", http.StatusServiceUnavailable)
			return
		}
		key, ok := strings.CutPrefix(r.Header.Get("Authorization"), "Bearer ")
		hash := sha256.Sum256([]byte(key))
		if !ok || !hmac.Equal(hash[:], expected[:]) {
			http.Error(w, "invalid access key", http.StatusUnauthorized)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (c *center) list(w http.ResponseWriter, _ *http.Request) {
	type listedDevice struct {
		ID             string           `json:"id"`
		Name           string           `json:"name"`
		Online         bool             `json:"online"`
		TunnelProvider string           `json:"tunnelProvider,omitempty"`
		TunnelAddress  string           `json:"tunnelAddress,omitempty"`
		Tunnels        []tunnelEndpoint `json:"tunnels,omitempty"`
	}
	type endpointKey struct {
		name, address   string
		port, httpsPort uint16
	}
	type certificateKey struct {
		name, certificate string
	}
	c.mu.Lock()
	hosts := make([]registration, 0, len(c.devices))
	for _, host := range c.devices {
		if !host.Deleted {
			hosts = append(hosts, host)
		}
	}
	c.mu.Unlock()

	// Keep live IDs distinct. Prefer the newest record when old portable
	// copies report the same name and certificate or connection endpoint.
	slices.SortFunc(hosts, func(a, b registration) int {
		if byTime := b.LastSeen.Compare(a.LastSeen); byTime != 0 {
			return byTime
		}
		return strings.Compare(a.ID, b.ID)
	})
	now := time.Now()
	certificates := make(map[certificateKey]bool)
	endpoints := make(map[endpointKey]bool)
	devices := []listedDevice{}
	for _, host := range hosts {
		online := now.Sub(host.LastSeen) < onlineFor
		name := strings.ToLower(host.Name)
		cert := certificateKey{name, host.Certificate}
		duplicate := host.Certificate != "" && certificates[cert]
		for _, address := range host.Addresses {
			duplicate = duplicate || endpoints[endpointKey{name, address, host.Port, host.HTTPSPort}]
		}
		if !online && duplicate {
			continue
		}
		devices = append(devices, listedDevice{host.ID, host.effectiveDevice().Name, online, host.TunnelProvider, host.TunnelAddress, host.Tunnels})
		if host.Certificate != "" {
			certificates[cert] = true
		}
		for _, address := range host.Addresses {
			endpoints[endpointKey{name, address, host.Port, host.HTTPSPort}] = true
		}
	}
	slices.SortFunc(devices, func(a, b listedDevice) int {
		if byName := strings.Compare(a.Name, b.Name); byName != 0 {
			return byName
		}
		return strings.Compare(a.ID, b.ID)
	})
	w.Header().Set("X-SunAndMon-Deduplicated", "1")
	jsonResponse(w, devices)
}

func (c *center) register(w http.ResponseWriter, r *http.Request) {
	var input struct {
		device
		HostKey string `json:"hostKey"`
	}
	if !readJSON(w, r, &input) {
		return
	}
	input.ID = r.PathValue("id")
	owner := r.Header.Get("X-SunAndMon-Registration-Id")
	if owner != "" && !isHex(owner, 32) {
		http.Error(w, "invalid registration owner", http.StatusBadRequest)
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	block, rest := pem.Decode([]byte(input.Certificate))
	if !isHex(input.ID, 16) || !isHex(input.HostKey, 32) || input.HTTPSPort == 0 ||
		block == nil || block.Type != "CERTIFICATE" || len(strings.TrimSpace(string(rest))) != 0 ||
		!validDeviceName(input.Name) || input.Port == 0 || len(input.Addresses) == 0 || len(input.Addresses) > 16 {
		http.Error(w, "invalid device registration", http.StatusBadRequest)
		return
	}
	if _, err := x509.ParseCertificate(block.Bytes); err != nil {
		http.Error(w, "invalid host certificate", http.StatusBadRequest)
		return
	}
	addresses, err := normalizeAddresses(input.Addresses)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	tunnelProvider, tunnelAddress, tunnels, err := normalizeTunnelEndpoints(input.TunnelProvider, input.TunnelAddress, input.Tunnels)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	input.Addresses = addresses
	input.TunnelProvider = tunnelProvider
	input.TunnelAddress = tunnelAddress
	input.Tunnels = tunnels
	c.mu.Lock()
	defer c.mu.Unlock()
	old, exists := c.devices[input.ID]
	if old.Deleted {
		http.Error(w, "device was removed by a center administrator", http.StatusForbidden)
		return
	}
	if exists && !hmac.Equal([]byte(old.HostKey), []byte(input.HostKey)) {
		http.Error(w, "device key mismatch", http.StatusForbidden)
		return
	}
	if exists && old.Owner != "" && old.Owner != owner && time.Since(old.LastSeen) < onlineFor {
		http.Error(w, "device is registered by another running copy", http.StatusConflict)
		return
	}
	c.devices[input.ID] = registration{device: input.device, HostKey: input.HostKey, LastSeen: time.Now().UTC(), Owner: owner, Override: old.Override}
	if !exists || old.Name != input.Name || old.Port != input.Port ||
		old.HTTPSPort != input.HTTPSPort || old.Certificate != input.Certificate ||
		old.TunnelProvider != input.TunnelProvider || old.TunnelAddress != input.TunnelAddress ||
		!slices.Equal(old.Tunnels, tunnels) ||
		!slices.Equal(old.Addresses, addresses) {
		if err := c.save(); err != nil {
			if exists {
				c.devices[input.ID] = old
			} else {
				delete(c.devices, input.ID)
			}
			log.Print(err)
			http.Error(w, "could not save device", http.StatusInternalServerError)
			return
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

func (c *center) remove(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := r.PathValue("id")
	old, ok := c.devices[id]
	if ok && !old.Deleted {
		if !hmac.Equal([]byte(old.HostKey), []byte(r.Header.Get("X-SunAndMon-Host-Key"))) {
			http.Error(w, "device key mismatch", http.StatusForbidden)
			return
		}
		if old.Owner != "" && old.Owner != r.Header.Get("X-SunAndMon-Registration-Id") {
			http.Error(w, "device is registered by another running copy", http.StatusConflict)
			return
		}
		if old.Override != nil {
			offline := old
			offline.LastSeen, offline.Owner = time.Time{}, ""
			c.devices[id] = offline
		} else {
			delete(c.devices, id)
		}
		if err := c.save(); err != nil {
			c.devices[id] = old
			log.Print(err)
			http.Error(w, "could not remove device", http.StatusInternalServerError)
			return
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

func sign(key, purpose, payload string) string {
	mac := hmac.New(sha256.New, []byte(key))
	_, _ = io.WriteString(mac, purpose+":"+payload)
	return hex.EncodeToString(mac.Sum(nil))
}

func (c *center) pair(w http.ResponseWriter, r *http.Request) {
	var input struct {
		Fingerprint string `json:"fingerprint"`
	}
	if !readJSON(w, r, &input) {
		return
	}
	if !isHex(input.Fingerprint, 32) {
		http.Error(w, "invalid client certificate fingerprint", http.StatusBadRequest)
		return
	}
	c.mu.Lock()
	host, exists := c.devices[r.PathValue("id")]
	c.mu.Unlock()
	if !exists || host.Deleted || time.Since(host.LastSeen) >= onlineFor {
		http.Error(w, "device is offline", http.StatusNotFound)
		return
	}
	expiryNonce := fmt.Sprintf("%d.%s", time.Now().Add(time.Minute).Unix(), rand.Text())
	payload := expiryNonce + "." + input.Fingerprint
	jsonResponse(w, struct {
		device
		Ticket string `json:"ticket"`
	}{
		host.effectiveDevice(),
		expiryNonce + "." + sign(host.HostKey, "ticket", payload),
	})
}

func main() {
	addr := flag.String("listen", "127.0.0.1:8787", "listen address")
	path := flag.String("state", "state/devices.json", "private registry file")
	cert := flag.String("tls-cert", "", "PEM certificate chain")
	key := flag.String("tls-key", "", "PEM private key")
	flag.Parse()

	login := os.Getenv("SUNANDMON_CENTER_KEY")
	if !isHex(login, 32) {
		log.Fatal("SUNANDMON_CENTER_KEY must be a randomly generated 64-character lowercase hex value")
	}
	admin := os.Getenv("SUNANDMON_CENTER_ADMIN_KEY")
	if admin != "" && (!isHex(admin, 32) || admin == login) {
		log.Fatal("SUNANDMON_CENTER_ADMIN_KEY must be a separate randomly generated 64-character lowercase hex value")
	}
	host, _, err := net.SplitHostPort(*addr)
	ip, _ := netip.ParseAddr(host)
	if err != nil || (*cert == "") != (*key == "") || (*cert == "" && !ip.IsLoopback()) {
		log.Fatal("use a loopback listen address or configure both TLS flags")
	}
	c, err := newCenter(*path, login)
	if err != nil {
		log.Fatal(err)
	}
	if admin != "" {
		hash := sha256.Sum256([]byte(admin))
		c.adminHash = &hash
	}
	server := &http.Server{
		Addr: *addr, Handler: c.handler(), ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout: 10 * time.Second, WriteTimeout: 10 * time.Second, IdleTimeout: 30 * time.Second,
	}
	log.Printf("center listening on %s", *addr)
	if *cert != "" {
		log.Fatal(server.ListenAndServeTLS(*cert, *key))
	}
	log.Fatal(server.ListenAndServe())
}
