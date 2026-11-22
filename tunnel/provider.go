package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"
)

type ProviderType string

const (
	ProviderNone      ProviderType = "none"
	ProviderTailscale ProviderType = "tailscale"
	ProviderEasyTier  ProviderType = "easytier"
)

type Config struct {
	Provider ProviderType `json:"provider"`
	StateDir string       `json:"stateDir"`
	Hostname string       `json:"hostname"`

	// Tailscale / Headscale options
	ControlURL  string `json:"controlUrl,omitempty"`
	AuthKey     string `json:"authKey,omitempty"`
	UseWebOAuth bool   `json:"useWebOAuth,omitempty"`

	// EasyTier options
	NetworkName   string   `json:"networkName,omitempty"`
	NetworkSecret string   `json:"networkSecret,omitempty"`
	Peers         []string `json:"peers,omitempty"`
	CoreBinary    string   `json:"coreBinary,omitempty"`
	VirtualIP     string   `json:"virtualIp,omitempty"`
}

// HostPorts describes the Sunshine/SunAndMon TCP and UDP ports exposed on a host.
// Standard Sunshine offsets relative to base HTTP port (default 47989):
//   HTTPS     = http - 5  (47984)
//   RTSP      = http + 21 (48010)
//   Video UDP = http + 9  (47998)
//   Ctrl  UDP = http + 10 (47999)
//   Audio UDP = http + 11 (48000)
//   Mic   UDP = http + 13 (48002)
//   Clip  TCP = http + 1  (47990)
type HostPorts struct {
	HTTPPort  uint16 `json:"httpPort"`
	HTTPSPort uint16 `json:"httpsPort"`
	RTSPPort  uint16 `json:"rtspPort,omitempty"`
	VideoPort uint16 `json:"videoPort,omitempty"`
	CtrlPort  uint16 `json:"ctrlPort,omitempty"`
	AudioPort uint16 `json:"audioPort,omitempty"`
	MicPort   uint16 `json:"micPort,omitempty"`
	ClipPort  uint16 `json:"clipPort,omitempty"`
}

func (p HostPorts) Normalized() HostPorts {
	out := p
	if out.HTTPPort == 0 {
		out.HTTPPort = 47989
	}
	if out.HTTPSPort == 0 {
		if out.HTTPPort > 5 {
			out.HTTPSPort = out.HTTPPort - 5
		} else {
			out.HTTPSPort = 47984
		}
	}
	if out.RTSPPort == 0 {
		out.RTSPPort = out.HTTPPort + 21
	}
	if out.VideoPort == 0 {
		out.VideoPort = out.HTTPPort + 9
	}
	if out.CtrlPort == 0 {
		out.CtrlPort = out.HTTPPort + 10
	}
	if out.AudioPort == 0 {
		out.AudioPort = out.HTTPPort + 11
	}
	if out.MicPort == 0 {
		out.MicPort = out.HTTPPort + 13
	}
	if out.ClipPort == 0 {
		out.ClipPort = out.HTTPPort + 1
	}
	return out
}

func (p HostPorts) TCPPorts() []uint16 {
	n := p.Normalized()
	return uniquePorts(n.HTTPPort, n.HTTPSPort, n.RTSPPort, n.ClipPort)
}

func (p HostPorts) UDPPorts() []uint16 {
	n := p.Normalized()
	return uniquePorts(n.VideoPort, n.CtrlPort, n.AudioPort, n.MicPort)
}

func uniquePorts(vals ...uint16) []uint16 {
	seen := make(map[uint16]bool, len(vals))
	out := make([]uint16, 0, len(vals))
	for _, v := range vals {
		if v != 0 && !seen[v] {
			seen[v] = true
			out = append(out, v)
		}
	}
	return out
}

type ForwardMapping struct {
	Provider  ProviderType `json:"provider,omitempty"`
	RemoteIP  string       `json:"remoteIp"`
	LocalHost string       `json:"localHost"`
	Ports     HostPorts    `json:"ports"`
}

type Status struct {
	Provider       ProviderType     `json:"provider"`
	State          string           `json:"state"` // stopped | starting | needs_auth | connected | error
	VirtualIP      string           `json:"virtualIp,omitempty"`
	ExposeIP       string           `json:"exposeIp,omitempty"`
	Hostname       string           `json:"hostname,omitempty"`
	AuthURL        string           `json:"authUrl,omitempty"`
	Error          string           `json:"error,omitempty"`
	PeersOnline    int              `json:"peersOnline,omitempty"`
	HostExposed    bool             `json:"hostExposed,omitempty"`
	ActiveForwards []ForwardMapping `json:"activeForwards,omitempty"`
}

type MultiStatus struct {
	Status
	Providers map[ProviderType]Status `json:"providers"`
}

// TunnelProvider is the unified abstraction implemented by both Tailscale/Headscale (tsnet)
// and EasyTier (--no-tun userspace) backends.
type TunnelProvider interface {
	Type() ProviderType
	Start(ctx context.Context, cfg Config) error
	Stop() error
	Status() Status
	ExposeHost(ports HostPorts) error
	UnexposeHost() error
	EnsureForward(remoteIP string, ports HostPorts) (ForwardMapping, error)
	CloseForward(remoteIP string) error
}

// LoopbackAllocator assigns distinct 127.18.x.y addresses per remote tunnel peer so each
// peer keeps Sunshine's exact native TCP/UDP ports without colliding with local Sunshine.
type LoopbackAllocator struct {
	mu      sync.Mutex
	byPeer  map[string]string
	nextOct uint16
}

func NewLoopbackAllocator() *LoopbackAllocator {
	return &LoopbackAllocator{
		byPeer:  make(map[string]string),
		nextOct: 10,
	}
}

func (a *LoopbackAllocator) Allocate(remoteIP string) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if ip, ok := a.byPeer[remoteIP]; ok {
		return ip, nil
	}
	if a.nextOct > 60000 {
		return "", errors.New("exhausted loopback forwarding slots")
	}
	hi := byte(a.nextOct / 250)
	lo := byte(a.nextOct%250) + 1
	a.nextOct++
	ip := fmt.Sprintf("127.18.%d.%d", hi, lo)
	if ln, err := net.Listen("tcp4", ip+":0"); err == nil {
		_ = ln.Close()
	} else {
		ip = "127.0.0.1"
	}
	a.byPeer[remoteIP] = ip
	return ip, nil
}

func (a *LoopbackAllocator) Release(remoteIP string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	delete(a.byPeer, remoteIP)
}

func pipeTCP(a, b net.Conn) {
	var wg sync.WaitGroup
	wg.Add(2)
	copyHalf := func(dst, src net.Conn) {
		defer wg.Done()
		_, _ = io.Copy(dst, src)
		if cw, ok := dst.(interface{ CloseWrite() error }); ok {
			_ = cw.CloseWrite()
		}
	}
	go copyHalf(a, b)
	go copyHalf(b, a)
	wg.Wait()
	_ = a.Close()
	_ = b.Close()
}

// udpRelaySession tracks one client-side UDP flow mapped to an upstream net.Conn.
type udpRelaySession struct {
	upstream net.Conn
	lastSeen time.Time
}

// serveUDPForward bridges a listening net.PacketConn to an upstream dialer (per remote peer address).
func serveUDPForward(ctx context.Context, pc net.PacketConn, dialUpstream func(context.Context) (net.Conn, error)) {
	var mu sync.Mutex
	sessions := make(map[string]*udpRelaySession)
	defer func() {
		mu.Lock()
		for _, s := range sessions {
			_ = s.upstream.Close()
		}
		mu.Unlock()
	}()

	// Reap idle UDP flows
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				cutoff := time.Now().Add(-90 * time.Second)
				mu.Lock()
				for key, s := range sessions {
					if s.lastSeen.Before(cutoff) {
						_ = s.upstream.Close()
						delete(sessions, key)
					}
				}
				mu.Unlock()
			}
		}
	}()

	buf := make([]byte, 65535)
	for {
		n, srcAddr, err := pc.ReadFrom(buf)
		if err != nil {
			return
		}
		key := srcAddr.String()
		mu.Lock()
		sess := sessions[key]
		if sess != nil {
			sess.lastSeen = time.Now()
		}
		mu.Unlock()

		if sess == nil {
			up, err := dialUpstream(ctx)
			if err != nil {
				continue
			}
			sess = &udpRelaySession{upstream: up, lastSeen: time.Now()}
			mu.Lock()
			if existing := sessions[key]; existing != nil {
				_ = up.Close()
				sess = existing
				sess.lastSeen = time.Now()
			} else {
				sessions[key] = sess
				go func(peer net.Addr, s *udpRelaySession, k string) {
					rbuf := make([]byte, 65535)
					for {
						rn, rerr := s.upstream.Read(rbuf)
						if rerr != nil {
							break
						}
						mu.Lock()
						s.lastSeen = time.Now()
						mu.Unlock()
						if _, werr := pc.WriteTo(rbuf[:rn], peer); werr != nil {
							break
						}
					}
					mu.Lock()
					if sessions[k] == s {
						delete(sessions, k)
					}
					mu.Unlock()
					_ = s.upstream.Close()
				}(srcAddr, sess, key)
			}
			mu.Unlock()
		}
		_, _ = sess.upstream.Write(buf[:n])
	}
}

func primaryLANIPv4() string {
	conn, err := net.DialTimeout("udp4", "8.8.8.8:80", 2*time.Second)
	if err == nil {
		defer conn.Close()
		if addr, ok := conn.LocalAddr().(*net.UDPAddr); ok && addr.IP != nil && !addr.IP.IsLoopback() {
			return addr.IP.String()
		}
	}
	ifaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipNet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			v4 := ipNet.IP.To4()
			if v4 != nil && !v4.IsLoopback() && !v4.IsLinkLocalUnicast() {
				return v4.String()
			}
		}
	}
	return ""
}
