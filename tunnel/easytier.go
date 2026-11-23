package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

type easyTierIdentity struct {
	MachineID string `json:"machineId"`
	ProxyIP   string `json:"proxyIp"`
}

type etForwardRule struct {
	Proto string
	Bind  string
	Dst   string
}

type etForwardEntry struct {
	mapping ForwardMapping
	rules   []etForwardRule
}

// EasyTierProvider implements TunnelProvider by supervising easytier-core in pure userspace
// (--no-tun --use-smoltcp --multi-thread) mode, completely isolated from OS routes and
// existing Tailscale/VPN setups.
type EasyTierProvider struct {
	mu          sync.Mutex
	cmd         *exec.Cmd
	ctx         context.Context
	cancel      context.CancelFunc
	cfg         Config
	corePath    string
	cliPath     string
	rpcAddr     string
	identity    easyTierIdentity
	state       string
	virtualIP   string
	lastErr     string
	peersOnline int
	hostExposed bool
	hostPorts   HostPorts
	alloc       *LoopbackAllocator
	forwards    map[string]*etForwardEntry
}

func NewEasyTierProvider(alloc *LoopbackAllocator) *EasyTierProvider {
	if alloc == nil {
		alloc = NewLoopbackAllocator()
	}
	return &EasyTierProvider{
		state:    "stopped",
		alloc:    alloc,
		forwards: make(map[string]*etForwardEntry),
	}
}

func (e *EasyTierProvider) Type() ProviderType {
	return ProviderEasyTier
}

func findEasyTierBinaries(overrideCore string) (string, string) {
	coreName := "easytier-core"
	cliName := "easytier-cli"
	if runtime.GOOS == "windows" {
		coreName += ".exe"
		cliName += ".exe"
	}
	candidates := []string{}
	if strings.TrimSpace(overrideCore) != "" {
		candidates = append(candidates, strings.TrimSpace(overrideCore))
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		candidates = append(candidates,
			filepath.Join(dir, coreName),
			filepath.Join(dir, "easytier", coreName),
			filepath.Join(dir, "tunnel", coreName),
		)
	}
	if p, err := exec.LookPath(coreName); err == nil {
		candidates = append(candidates, p)
	}
	for _, c := range candidates {
		if st, err := os.Stat(c); err == nil && !st.IsDir() {
			cli := filepath.Join(filepath.Dir(c), cliName)
			if st2, err2 := os.Stat(cli); err2 == nil && !st2.IsDir() {
				return c, cli
			}
			if p, err2 := exec.LookPath(cliName); err2 == nil {
				return c, p
			}
		}
	}
	return "", ""
}

func loadOrCreateEasyTierIdentity(dir string) (easyTierIdentity, error) {
	path := filepath.Join(dir, "identity.json")
	if data, err := os.ReadFile(path); err == nil {
		var id easyTierIdentity
		if json.Unmarshal(data, &id) == nil && id.MachineID != "" && net.ParseIP(id.ProxyIP) != nil {
			return id, nil
		}
	}
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return easyTierIdentity{}, err
	}
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	mid := fmt.Sprintf("%08x-%04x-%04x-%04x-%012x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
	h := sha256.Sum256([]byte(mid))
	// Deterministic unique host subnet-proxy address in 10.127.1..254.1..254
	oct3 := int(h[0])%250 + 2
	oct4 := int(h[1])%250 + 2
	id := easyTierIdentity{
		MachineID: mid,
		ProxyIP:   fmt.Sprintf("10.127.%d.%d", oct3, oct4),
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return easyTierIdentity{}, err
	}
	raw, _ := json.MarshalIndent(id, "", "  ")
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		return easyTierIdentity{}, err
	}
	return id, nil
}

func pickFreeLoopbackPort() (int, error) {
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	port := ln.Addr().(*net.TCPAddr).Port
	_ = ln.Close()
	return port, nil
}

func (e *EasyTierProvider) Start(ctx context.Context, cfg Config) error {
	_ = e.Stop()

	if strings.TrimSpace(cfg.NetworkName) == "" {
		return errors.New("easytier networkName is required")
	}
	if cfg.StateDir == "" {
		return errors.New("easytier stateDir is required")
	}
	corePath, cliPath := findEasyTierBinaries(cfg.CoreBinary)
	if corePath == "" || cliPath == "" {
		return errors.New("easytier-core / easytier-cli binaries not found")
	}
	etDir := filepath.Join(cfg.StateDir, "easytier")
	id, err := loadOrCreateEasyTierIdentity(etDir)
	if err != nil {
		return fmt.Errorf("load easytier identity: %w", err)
	}
	rpcPort, err := pickFreeLoopbackPort()
	if err != nil {
		return err
	}
	rpcAddr := fmt.Sprintf("127.0.0.1:%d", rpcPort)

	hostname := strings.TrimSpace(cfg.Hostname)
	if hostname == "" {
		if h, err := os.Hostname(); err == nil && h != "" {
			hostname = "sunandmon-" + sanitizeHostname(h)
		} else {
			hostname = "sunandmon-node"
		}
	}

	args := []string{
		"--no-tun",
		"--use-smoltcp",
		"--multi-thread",
		"--machine-id", id.MachineID,
		"--hostname", hostname,
		"--network-name", strings.TrimSpace(cfg.NetworkName),
		"--rpc-portal", rpcAddr,
		"--rpc-portal-whitelist", "127.0.0.1/32",
	}
	if strings.TrimSpace(cfg.NetworkSecret) != "" {
		args = append(args, "--network-secret", strings.TrimSpace(cfg.NetworkSecret))
	}
	if strings.TrimSpace(cfg.VirtualIP) != "" {
		args = append(args, "-i", strings.TrimSpace(cfg.VirtualIP))
	} else {
		args = append(args, "-d")
	}
	if lanIP := primaryLANIPv4(); lanIP != "" {
		// Expose this host's non-loopback LAN IP (where Sunshine listens on 0.0.0.0)
		// through EasyTier's userspace smoltcp subnet proxy for both TCP and UDP streaming.
		args = append(args, "-n", fmt.Sprintf("%s/32->%s/32", lanIP, id.ProxyIP))
	}
	peers := cfg.Peers
	if len(peers) == 0 {
		peers = []string{"tcp://public.easytier.top:11010"}
	}
	for _, peer := range peers {
		if p := strings.TrimSpace(peer); p != "" {
			args = append(args, "-p", p)
		}
	}

	runCtx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(runCtx, corePath, args...)
	cmd.Dir = etDir
	hideWindow(cmd)

	if err := cmd.Start(); err != nil {
		cancel()
		return fmt.Errorf("start easytier-core: %w", err)
	}

	e.mu.Lock()
	e.cmd = cmd
	e.ctx = runCtx
	e.cancel = cancel
	e.cfg = cfg
	e.cfg.Hostname = hostname
	e.corePath = corePath
	e.cliPath = cliPath
	e.rpcAddr = rpcAddr
	e.identity = id
	e.state = "starting"
	e.virtualIP = ""
	e.lastErr = ""
	e.peersOnline = 0
	e.hostExposed = true
	e.mu.Unlock()

	go func() {
		err := cmd.Wait()
		e.mu.Lock()
		if e.cmd == cmd && e.state != "stopped" {
			e.state = "error"
			if err != nil {
				e.lastErr = fmt.Sprintf("easytier-core exited: %v", err)
			} else {
				e.lastErr = "easytier-core exited unexpectedly"
			}
		}
		e.mu.Unlock()
	}()

	go e.pollStatus(runCtx, cliPath, rpcAddr)
	return nil
}

func diagnoseEasyTierPeers(peers []string) string {
	var failures []string
	for _, raw := range peers {
		raw = strings.TrimSpace(raw)
		if raw == "" {
			continue
		}
		u, err := url.Parse(raw)
		if err != nil || u.Host == "" {
			failures = append(failures, fmt.Sprintf("%s (invalid URL)", raw))
			continue
		}
		host := u.Hostname()
		port := u.Port()
		if port == "" {
			port = "11010"
		}
		if net.ParseIP(host) == nil {
			if addrs, derr := net.LookupHost(host); derr != nil || len(addrs) == 0 {
				failures = append(failures, fmt.Sprintf("%s (DNS lookup failed)", raw))
				continue
			}
		}
		if strings.EqualFold(u.Scheme, "tcp") || strings.EqualFold(u.Scheme, "ws") || strings.EqualFold(u.Scheme, "wss") {
			c, cerr := net.DialTimeout("tcp", net.JoinHostPort(host, port), 2*time.Second)
			if cerr != nil {
				failures = append(failures, fmt.Sprintf("%s (unreachable)", raw))
				continue
			}
			_ = c.Close()
			return ""
		}
		return ""
	}
	if len(failures) == 0 {
		return ""
	}
	return "Peer unreachable: " + strings.Join(failures, ", ") + ". Enter a reachable EasyTier relay or self-hosted node."
}

func (e *EasyTierProvider) pollStatus(ctx context.Context, cliPath, rpcAddr string) {
	startedAt := time.Now()
	diagnosed := false
	ticker := time.NewTicker(1200 * time.Millisecond)
	defer ticker.Stop()

	for {
		cctx, ccancel := context.WithTimeout(ctx, 2*time.Second)
		cmd := exec.CommandContext(cctx, cliPath, "-p", rpcAddr, "-o", "json", "node")
		hideWindow(cmd)
		out, err := cmd.Output()
		ccancel()

		if err == nil && len(out) > 0 {
			var node struct {
				IPv4Addr string `json:"ipv4_addr"`
			}
			if json.Unmarshal(out, &node) == nil {
				ip := strings.Split(strings.TrimSpace(node.IPv4Addr), "/")[0]
				if net.ParseIP(ip) != nil {
					peersOnline := e.countPeers(ctx, cliPath, rpcAddr)
					e.mu.Lock()
					if e.state != "stopped" {
						e.state = "connected"
						e.virtualIP = ip
						e.peersOnline = peersOnline
						e.lastErr = ""
					}
					e.mu.Unlock()
				} else if !diagnosed && time.Since(startedAt) >= 4*time.Second {
					diagnosed = true
					e.mu.Lock()
					peersCopy := append([]string(nil), e.cfg.Peers...)
					e.mu.Unlock()
					if len(peersCopy) == 0 {
						peersCopy = []string{"tcp://public.easytier.top:11010"}
					}
					if msg := diagnoseEasyTierPeers(peersCopy); msg != "" {
						e.mu.Lock()
						if e.state == "starting" {
							e.state = "error"
							e.lastErr = msg
						}
						e.mu.Unlock()
					}
				}
			}
		}

		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (e *EasyTierProvider) countPeers(ctx context.Context, cliPath, rpcAddr string) int {
	cctx, ccancel := context.WithTimeout(ctx, 2*time.Second)
	defer ccancel()
	cmd := exec.CommandContext(cctx, cliPath, "-p", rpcAddr, "-o", "json", "peer")
	hideWindow(cmd)
	out, err := cmd.Output()
	if err != nil {
		return 0
	}
	var peers []map[string]any
	if json.Unmarshal(out, &peers) != nil {
		return 0
	}
	count := 0
	for _, p := range peers {
		if cost, ok := p["cost"].(string); ok && strings.EqualFold(cost, "Local") {
			continue
		}
		count++
	}
	return count
}

func (e *EasyTierProvider) Stop() error {
	e.mu.Lock()
	cancel := e.cancel
	cmd := e.cmd
	e.cancel = nil
	e.cmd = nil
	e.state = "stopped"
	e.virtualIP = ""
	e.lastErr = ""
	e.peersOnline = 0
	e.hostExposed = false
	e.forwards = make(map[string]*etForwardEntry)
	e.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	if cmd != nil && cmd.Process != nil {
		_ = cmd.Process.Kill()
	}
	return nil
}

func (e *EasyTierProvider) Status() Status {
	e.mu.Lock()
	defer e.mu.Unlock()
	forwards := make([]ForwardMapping, 0, len(e.forwards))
	for _, f := range e.forwards {
		forwards = append(forwards, f.mapping)
	}
	exposeIP := e.identity.ProxyIP
	if e.state != "connected" {
		exposeIP = ""
	}
	return Status{
		Provider:       ProviderEasyTier,
		State:          e.state,
		VirtualIP:      e.virtualIP,
		ExposeIP:       exposeIP,
		Hostname:       e.cfg.Hostname,
		Error:          e.lastErr,
		PeersOnline:    e.peersOnline,
		HostExposed:    e.hostExposed && e.state == "connected",
		ActiveForwards: forwards,
	}
}

func (e *EasyTierProvider) ExposeHost(ports HostPorts) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.cmd == nil {
		return errors.New("easytier tunnel is not running")
	}
	e.hostPorts = ports.Normalized()
	e.hostExposed = true
	return nil
}

func (e *EasyTierProvider) UnexposeHost() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.hostExposed = false
	return nil
}

func (e *EasyTierProvider) EnsureForward(remoteIP string, ports HostPorts) (ForwardMapping, error) {
	remoteIP = strings.TrimSpace(remoteIP)
	if net.ParseIP(remoteIP) == nil {
		return ForwardMapping{}, errors.New("invalid remoteIp")
	}
	norm := ports.Normalized()

	e.mu.Lock()
	if existing, ok := e.forwards[remoteIP]; ok && existing.mapping.Ports == norm {
		m := existing.mapping
		e.mu.Unlock()
		return m, nil
	}
	cliPath := e.cliPath
	rpcAddr := e.rpcAddr
	parentCtx := e.ctx
	e.mu.Unlock()

	if cliPath == "" || rpcAddr == "" || parentCtx == nil {
		return ForwardMapping{}, errors.New("easytier tunnel is not running")
	}
	_ = e.CloseForward(remoteIP)

	localHost, err := e.alloc.Allocate(string(ProviderEasyTier) + ":" + remoteIP)
	if err != nil {
		return ForwardMapping{}, err
	}

	var (
		added        []etForwardRule
		chosenOffset int
		lastErr      error
		attempts     int
	)
	for _, offset := range candidateForwardOffsets(norm) {
		if !canBindForwardPorts(localHost, norm, offset) {
			continue
		}
		attempts++
		var rules []etForwardRule
		for _, port := range norm.TCPPorts() {
			rules = append(rules, etForwardRule{
				Proto: "tcp",
				Bind:  fmt.Sprintf("%s:%d", localHost, int(port)+offset),
				Dst:   net.JoinHostPort(remoteIP, fmt.Sprintf("%d", port)),
			})
		}
		for _, port := range norm.UDPPorts() {
			rules = append(rules, etForwardRule{
				Proto: "udp",
				Bind:  fmt.Sprintf("%s:%d", localHost, port),
				Dst:   net.JoinHostPort(remoteIP, fmt.Sprintf("%d", port)),
			})
		}

		added = make([]etForwardRule, 0, len(rules))
		failed := false
		for _, r := range rules {
			cctx, ccancel := context.WithTimeout(parentCtx, 5*time.Second)
			cmd := exec.CommandContext(cctx, cliPath, "-p", rpcAddr, "port-forward", "add", r.Proto, r.Bind, r.Dst)
			hideWindow(cmd)
			out, err := cmd.CombinedOutput()
			ccancel()
			if err != nil {
				for _, a := range added {
					rctx, rcancel := context.WithTimeout(context.Background(), 3*time.Second)
					rcmd := exec.CommandContext(rctx, cliPath, "-p", rpcAddr, "port-forward", "remove", a.Proto, a.Bind)
					hideWindow(rcmd)
					_ = rcmd.Run()
					rcancel()
				}
				added = nil
				lastErr = fmt.Errorf("easytier port-forward %s %s->%s: %v (%s)", r.Proto, r.Bind, r.Dst, err, strings.TrimSpace(string(out)))
				failed = true
				break
			}
			added = append(added, r)
		}
		if !failed {
			chosenOffset = offset
			lastErr = nil
			break
		}
		if attempts >= 8 {
			break
		}
	}
	if lastErr != nil {
		return ForwardMapping{}, lastErr
	}
	if len(added) == 0 {
		return ForwardMapping{}, fmt.Errorf("no free local port block available on %s", localHost)
	}

	mapping := ForwardMapping{
		Provider:   ProviderEasyTier,
		RemoteIP:   remoteIP,
		LocalHost:  localHost,
		PortOffset: chosenOffset,
		Ports:      norm,
	}
	e.mu.Lock()
	e.forwards[remoteIP] = &etForwardEntry{
		mapping: mapping,
		rules:   added,
	}
	e.mu.Unlock()
	return mapping, nil
}

func (e *EasyTierProvider) CloseForward(remoteIP string) error {
	e.mu.Lock()
	entry, ok := e.forwards[remoteIP]
	cliPath := e.cliPath
	rpcAddr := e.rpcAddr
	if ok {
		delete(e.forwards, remoteIP)
	}
	e.mu.Unlock()

	if ok && cliPath != "" && rpcAddr != "" {
		for _, r := range entry.rules {
			cctx, ccancel := context.WithTimeout(context.Background(), 3*time.Second)
			cmd := exec.CommandContext(cctx, cliPath, "-p", rpcAddr, "port-forward", "remove", r.Proto, r.Bind)
			hideWindow(cmd)
			_ = cmd.Run()
			ccancel()
		}
	}
	return nil
}
