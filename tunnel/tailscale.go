package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"tailscale.com/ipn"
	"tailscale.com/tsnet"
)

type tsForwardEntry struct {
	mapping ForwardMapping
	cancel  context.CancelFunc
	closers []func()
}

// TailscaleProvider implements TunnelProvider using Tailscale's pure-userspace tsnet engine.
// Supports both official Tailscale and self-hosted Headscale (via ControlURL), with either
// interactive Web OAuth (AuthURL) or manual Pre-Auth Key authentication, completely isolated
// from any system Tailscale installation (zero TUN, zero OS routing changes).
type TailscaleProvider struct {
	mu          sync.Mutex
	srv         *tsnet.Server
	ctx         context.Context
	cancel      context.CancelFunc
	cfg         Config
	state       string
	virtualIP   string
	authURL     string
	lastErr     string
	peersOnline int
	hostExposed bool
	hostClosers []func()
	hostCancel  context.CancelFunc
	alloc       *LoopbackAllocator
	forwards    map[string]*tsForwardEntry
}

func NewTailscaleProvider(alloc *LoopbackAllocator) *TailscaleProvider {
	if alloc == nil {
		alloc = NewLoopbackAllocator()
	}
	return &TailscaleProvider{
		state:    "stopped",
		alloc:    alloc,
		forwards: make(map[string]*tsForwardEntry),
	}
}

func (t *TailscaleProvider) Type() ProviderType {
	return ProviderTailscale
}

func (t *TailscaleProvider) Start(ctx context.Context, cfg Config) error {
	_ = t.Stop()

	stateDir := cfg.StateDir
	if stateDir == "" {
		return errors.New("tailscale stateDir is required")
	}
	tsDir := filepath.Join(stateDir, "tailscale")
	if err := os.MkdirAll(tsDir, 0o700); err != nil {
		return fmt.Errorf("create tailscale state dir: %w", err)
	}

	hostname := strings.TrimSpace(cfg.Hostname)
	if hostname == "" {
		if h, err := os.Hostname(); err == nil && h != "" {
			hostname = "sunandmon-" + sanitizeHostname(h)
		} else {
			hostname = "sunandmon-node"
		}
	}

	var srv *tsnet.Server
	captureLog := func(format string, args ...any) {
		msg := fmt.Sprintf(format, args...)
		if idx := strings.Index(msg, "or go to: "); idx >= 0 {
			u := strings.TrimSpace(msg[idx+len("or go to: "):])
			if fields := strings.Fields(u); len(fields) > 0 {
				u = fields[0]
			}
			if validAuthURL(u) {
				t.mu.Lock()
				if t.srv == srv && t.state != "connected" {
					t.authURL = u
					t.state = "needs_auth"
					t.lastErr = ""
				}
				t.mu.Unlock()
			}
		}
	}

	srv = &tsnet.Server{
		Dir:        tsDir,
		Hostname:   hostname,
		ControlURL: strings.TrimSpace(cfg.ControlURL),
		AuthKey:    strings.TrimSpace(cfg.AuthKey),
		Logf:       captureLog,
		UserLogf:   captureLog,
	}
	runCtx, cancel := context.WithCancel(context.Background())

	t.mu.Lock()
	t.srv = srv
	t.ctx = runCtx
	t.cancel = cancel
	t.cfg = cfg
	t.cfg.Hostname = hostname
	t.state = "starting"
	t.virtualIP = ""
	t.authURL = ""
	t.lastErr = ""
	t.peersOnline = 0
	t.mu.Unlock()
	if err := srv.Start(); err != nil {
		_ = t.Stop()
		return fmt.Errorf("start tsnet: %w", err)
	}

	go t.pollStatus(runCtx, srv)
	return nil
}

func validAuthURL(value string) bool {
	u, err := url.Parse(value)
	return err == nil && u.Hostname() != "" && u.User == nil && (u.Scheme == "https" || u.Scheme == "http")
}

func (t *TailscaleProvider) pollStatus(ctx context.Context, srv *tsnet.Server) {
	lc, err := srv.LocalClient()
	if err != nil {
		t.mu.Lock()
		t.state = "error"
		t.lastErr = err.Error()
		t.mu.Unlock()
		return
	}

	// Trigger interactive login URL generation when using Web OAuth without an AuthKey.
	t.mu.Lock()
	needInteractive := t.cfg.AuthKey == ""
	t.mu.Unlock()

	// Subscribe to IPN bus notifications so BrowseToURL is captured immediately.
	go func() {
		watcher, werr := lc.WatchIPNBus(ctx, ipn.NotifyInitialState)
		if werr != nil {
			return
		}
		defer watcher.Close()
		for {
			n, nerr := watcher.Next()
			if nerr != nil {
				return
			}
			t.mu.Lock()
			if t.srv == srv && t.state != "connected" {
				if n.BrowseToURL != nil && validAuthURL(strings.TrimSpace(*n.BrowseToURL)) {
					u := strings.TrimSpace(*n.BrowseToURL)
					t.authURL = u
					t.state = "needs_auth"
					t.lastErr = ""
				} else if n.ErrMessage != nil && t.authURL == "" {
					t.lastErr = "Tailscale could not contact or authenticate with the control server"
				}
			}
			t.mu.Unlock()
		}
	}()

	ticker := time.NewTicker(800 * time.Millisecond)
	defer ticker.Stop()
	interactivelyRetried := false
	started := time.Now()

	for {
		st, err := lc.Status(ctx)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			t.mu.Lock()
			if t.state != "connected" {
				t.lastErr = err.Error()
			}
			t.mu.Unlock()
		} else if st != nil {
			var v4 string
			for _, ip := range st.TailscaleIPs {
				if ip.Is4() {
					v4 = ip.String()
					break
				}
			}
			if v4 == "" && len(st.TailscaleIPs) > 0 {
				v4 = st.TailscaleIPs[0].String()
			}
			onlinePeers := 0
			for _, peer := range st.Peer {
				if peer != nil && peer.Online {
					onlinePeers++
				}
			}

			t.mu.Lock()
			t.peersOnline = onlinePeers
			if t.srv != srv {
				t.mu.Unlock()
				return
			}
			if validAuthURL(st.AuthURL) && st.BackendState != "Running" {
				t.authURL = st.AuthURL
			}
			hasAuthURL := t.authURL != ""
			switch st.BackendState {
			case "Running":
				t.state = "connected"
				t.virtualIP = v4
				t.authURL = ""
				t.lastErr = ""
			case "NeedsLogin":
				t.state = "needs_auth"
				if hasAuthURL {
					t.lastErr = ""
				}
			default:
				if t.state != "connected" {
					if hasAuthURL {
						t.state = "needs_auth"
					} else {
						t.state = "starting"
					}
				}
			}
			if needInteractive && !hasAuthURL && t.state != "connected" && t.lastErr == "" && time.Since(started) >= 30*time.Second {
				t.lastErr = "Tailscale has not issued a sign-in link. Check access to the control server."
			}
			t.mu.Unlock()

			// tsnet starts the first login. Re-requesting it cancels the control
			// plane's in-flight login, so allow only one delayed recovery attempt.
			if st.BackendState == "NeedsLogin" && !hasAuthURL && needInteractive && !interactivelyRetried && time.Since(started) >= 25*time.Second {
				interactivelyRetried = true
				loginCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
				loginErr := lc.StartLoginInteractive(loginCtx)
				cancel()
				if loginErr != nil && ctx.Err() == nil {
					t.mu.Lock()
					if t.srv == srv && t.authURL == "" && t.state != "connected" {
						t.lastErr = "Tailscale could not request a browser sign-in link"
					}
					t.mu.Unlock()
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

func (t *TailscaleProvider) Stop() error {
	_ = t.UnexposeHost()

	t.mu.Lock()
	cancel := t.cancel
	srv := t.srv
	forwards := t.forwards
	t.forwards = make(map[string]*tsForwardEntry)
	t.cancel = nil
	t.srv = nil
	t.state = "stopped"
	t.virtualIP = ""
	t.authURL = ""
	t.lastErr = ""
	t.peersOnline = 0
	t.mu.Unlock()

	for _, f := range forwards {
		f.cancel()
		for _, c := range f.closers {
			c()
		}
	}
	if cancel != nil {
		cancel()
	}
	if srv != nil {
		return srv.Close()
	}
	return nil
}

func (t *TailscaleProvider) Status() Status {
	t.mu.Lock()
	defer t.mu.Unlock()
	forwards := make([]ForwardMapping, 0, len(t.forwards))
	for _, f := range t.forwards {
		forwards = append(forwards, f.mapping)
	}
	return Status{
		Provider:       ProviderTailscale,
		State:          t.state,
		VirtualIP:      t.virtualIP,
		ExposeIP:       t.virtualIP,
		Hostname:       t.cfg.Hostname,
		AuthURL:        t.authURL,
		Error:          t.lastErr,
		PeersOnline:    t.peersOnline,
		HostExposed:    t.hostExposed,
		ActiveForwards: forwards,
	}
}

func (t *TailscaleProvider) ExposeHost(ports HostPorts) error {
	_ = t.UnexposeHost()

	t.mu.Lock()
	srv := t.srv
	parentCtx := t.ctx
	t.mu.Unlock()
	if srv == nil || parentCtx == nil {
		return errors.New("tailscale tunnel is not running")
	}

	exposeCtx, cancel := context.WithCancel(parentCtx)
	var closers []func()
	cleanup := func() {
		cancel()
		for _, c := range closers {
			c()
		}
	}

	for _, port := range ports.TCPPorts() {
		ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", port))
		if err != nil {
			cleanup()
			return fmt.Errorf("tsnet listen tcp:%d: %w", port, err)
		}
		closers = append(closers, func() { _ = ln.Close() })
		target := fmt.Sprintf("127.0.0.1:%d", port)
		go func(l net.Listener, dst string) {
			for {
				conn, err := l.Accept()
				if err != nil {
					return
				}
				go func(in net.Conn) {
					out, err := net.DialTimeout("tcp", dst, 5*time.Second)
					if err != nil {
						_ = in.Close()
						return
					}
					pipeTCP(in, out)
				}(conn)
			}
		}(ln, target)
	}

	for _, port := range ports.UDPPorts() {
		pc, err := srv.ListenPacket("udp", fmt.Sprintf(":%d", port))
		if err != nil {
			cleanup()
			return fmt.Errorf("tsnet listen udp:%d: %w", port, err)
		}
		closers = append(closers, func() { _ = pc.Close() })
		target := fmt.Sprintf("127.0.0.1:%d", port)
		go serveUDPForward(exposeCtx, pc, func(ctx context.Context) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "udp", target)
		})
	}

	t.mu.Lock()
	t.hostExposed = true
	t.hostClosers = closers
	t.hostCancel = cancel
	t.mu.Unlock()
	return nil
}

func (t *TailscaleProvider) UnexposeHost() error {
	t.mu.Lock()
	cancel := t.hostCancel
	closers := t.hostClosers
	t.hostCancel = nil
	t.hostClosers = nil
	t.hostExposed = false
	t.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	for _, c := range closers {
		c()
	}
	return nil
}

func (t *TailscaleProvider) EnsureForward(remoteIP string, ports HostPorts) (ForwardMapping, error) {
	remoteIP = strings.TrimSpace(remoteIP)
	if net.ParseIP(remoteIP) == nil {
		return ForwardMapping{}, errors.New("invalid remoteIp")
	}
	norm := ports.Normalized()

	t.mu.Lock()
	if existing, ok := t.forwards[remoteIP]; ok && existing.mapping.Ports == norm {
		m := existing.mapping
		t.mu.Unlock()
		return m, nil
	}
	srv := t.srv
	parentCtx := t.ctx
	t.mu.Unlock()

	if srv == nil || parentCtx == nil {
		return ForwardMapping{}, errors.New("tailscale tunnel is not running")
	}
	_ = t.CloseForward(remoteIP)

	localHost, err := t.alloc.Allocate(string(ProviderTailscale) + ":" + remoteIP)
	if err != nil {
		return ForwardMapping{}, err
	}
	fwdCtx, cancel := context.WithCancel(parentCtx)
	var (
		closers      []func()
		chosenOffset int
		lastErr      error
	)
	type boundTCP struct {
		ln   net.Listener
		port uint16
	}
	type boundUDP struct {
		pc   net.PacketConn
		port uint16
	}
	var tcpListeners []boundTCP
	var udpListeners []boundUDP

	lc := exclusiveListenConfig()
	for _, offset := range candidateForwardOffsets(norm) {
		tcpListeners = nil
		udpListeners = nil
		var attemptClosers []func()
		closeAttempt := func() {
			for _, c := range attemptClosers {
				c()
			}
		}
		var bindErr error
		for _, port := range norm.TCPPorts() {
			lp := int(port) + offset
			ln, err := lc.Listen(fwdCtx, "tcp4", fmt.Sprintf("%s:%d", localHost, lp))
			if err != nil {
				bindErr = fmt.Errorf("listen %s:%d tcp: %w", localHost, lp, err)
				break
			}
			attemptClosers = append(attemptClosers, func() { _ = ln.Close() })
			tcpListeners = append(tcpListeners, boundTCP{ln: ln, port: port})
		}
		if bindErr == nil {
			for _, port := range norm.UDPPorts() {
				pc, err := lc.ListenPacket(fwdCtx, "udp4", fmt.Sprintf("%s:%d", localHost, port))
				if err != nil {
					bindErr = fmt.Errorf("listen %s:%d udp: %w", localHost, port, err)
					break
				}
				attemptClosers = append(attemptClosers, func() { _ = pc.Close() })
				udpListeners = append(udpListeners, boundUDP{pc: pc, port: port})
			}
		}
		if bindErr != nil {
			closeAttempt()
			lastErr = bindErr
			continue
		}
		closers = attemptClosers
		chosenOffset = offset
		lastErr = nil
		break
	}
	if lastErr != nil || len(closers) == 0 {
		cancel()
		if lastErr == nil {
			lastErr = fmt.Errorf("no free local port block available on %s", localHost)
		}
		return ForwardMapping{}, lastErr
	}

	for _, item := range tcpListeners {
		dst := net.JoinHostPort(remoteIP, fmt.Sprintf("%d", item.port))
		go func(l net.Listener, remote string) {
			for {
				conn, err := l.Accept()
				if err != nil {
					return
				}
				go func(in net.Conn) {
					dctx, dcancel := context.WithTimeout(fwdCtx, 8*time.Second)
					out, err := srv.Dial(dctx, "tcp", remote)
					dcancel()
					if err != nil {
						_ = in.Close()
						return
					}
					pipeTCP(in, out)
				}(conn)
			}
		}(item.ln, dst)
	}

	for _, item := range udpListeners {
		dst := net.JoinHostPort(remoteIP, fmt.Sprintf("%d", item.port))
		go serveUDPForward(fwdCtx, item.pc, func(ctx context.Context) (net.Conn, error) {
			return srv.Dial(ctx, "udp", dst)
		})
	}

	mapping := ForwardMapping{
		Provider:   ProviderTailscale,
		RemoteIP:   remoteIP,
		LocalHost:  localHost,
		PortOffset: chosenOffset,
		Ports:      norm,
	}
	t.mu.Lock()
	t.forwards[remoteIP] = &tsForwardEntry{
		mapping: mapping,
		cancel:  cancel,
		closers: closers,
	}
	t.mu.Unlock()
	return mapping, nil
}

func (t *TailscaleProvider) CloseForward(remoteIP string) error {
	t.mu.Lock()
	entry, ok := t.forwards[remoteIP]
	if ok {
		delete(t.forwards, remoteIP)
	}
	t.mu.Unlock()
	if ok {
		entry.cancel()
		for _, c := range entry.closers {
			c()
		}
	}
	return nil
}

func sanitizeHostname(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' {
			b.WriteRune(r)
		} else if r == '_' || r == ' ' || r == '.' {
			b.WriteByte('-')
		}
	}
	out := strings.Trim(b.String(), "-")
	if out == "" {
		return "node"
	}
	if len(out) > 40 {
		return out[:40]
	}
	return out
}
