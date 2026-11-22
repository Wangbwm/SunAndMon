package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"strings"
	"syscall"
	"time"
)

func configureAndroidCompatEnv() {
	// Android 14+ stores system CA certificates in /apex/com.android.conscrypt/cacerts
	// while older Android releases store them in /system/etc/security/cacerts.
	// Ensure both directories are present in SSL_CERT_DIR even if androidbin's init()
	// only set /system/etc/security/cacerts.
	if _, err := os.Stat("/etc/ssl/certs/ca-certificates.crt"); err != nil {
		cur := os.Getenv("SSL_CERT_DIR")
		if cur == "" || !strings.Contains(cur, "/apex/com.android.conscrypt/cacerts") {
			if _, err2 := os.Stat("/system/etc/security/cacerts"); err2 == nil {
				_ = os.Setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts:/system/etc/security/cacerts")
			}
		}
	}
	if runtime.GOOS == "android" {
		// Native Android builds use bionic's resolver, including per-network DNS.
		return
	}

	// Android does not have /etc/resolv.conf. Preserve androiddns's /dev/socket/dnsproxyd
	// dialer if installed, and fall back to SUNANDMON_DNS + public DNS servers if needed.
	dnsEnv := strings.TrimSpace(os.Getenv("SUNANDMON_DNS"))
	_, resolvErr := os.Stat("/etc/resolv.conf")
	if dnsEnv != "" || resolvErr != nil {
		var servers []string
		for _, raw := range strings.Split(dnsEnv, ",") {
			s := strings.TrimSpace(raw)
			if s == "" {
				continue
			}
			if !strings.Contains(s, ":") {
				s += ":53"
			}
			servers = append(servers, s)
		}
		servers = append(servers, "223.5.5.5:53", "119.29.29.29:53", "8.8.8.8:53", "1.1.1.1:53")
		prevDial := net.DefaultResolver.Dial
		dialer := &net.Dialer{Timeout: 2500 * time.Millisecond}
		net.DefaultResolver.PreferGo = true
		net.DefaultResolver.Dial = func(ctx context.Context, network, address string) (net.Conn, error) {
			if prevDial != nil {
				if conn, err := prevDial(ctx, network, address); err == nil {
					return conn, nil
				}
			}
			proto := "udp4"
			if strings.HasPrefix(network, "tcp") {
				proto = "tcp4"
			}
			var lastErr error
			for _, srv := range servers {
				conn, err := dialer.DialContext(ctx, proto, srv)
				if err == nil {
					return conn, nil
				}
				lastErr = err
			}
			return nil, lastErr
		}
	}
}

func main() {
	configureAndroidCompatEnv()
	listen := flag.String("listen", "127.0.0.1:0", "local loopback listen address")
	watchStdin := flag.Bool("watch-stdin", false, "exit cleanly when stdin reaches EOF")
	flag.Parse()

	token := os.Getenv("SUNANDMON_TUNNEL_TOKEN")

	ln, err := net.Listen("tcp4", *listen)
	if err != nil {
		log.Fatalf("listen %s: %v", *listen, err)
	}
	port := ln.Addr().(*net.TCPAddr).Port

	mgr := newTunnelManager()
	srv := &http.Server{
		Handler:           mgr.handler(token),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      15 * time.Second,
	}

	ctx, stopSignals := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stopSignals()

	if *watchStdin {
		go func() {
			_, _ = io.Copy(io.Discard, os.Stdin)
			stopSignals()
		}()
	}

	go func() {
		<-ctx.Done()
		_ = mgr.stop()
		shCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = srv.Shutdown(shCtx)
	}()

	// Signal readiness to parent process (SunAndMon.exe)
	fmt.Printf("READY %d\n", port)
	_ = os.Stdout.Sync()

	if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
		log.Fatalf("serve: %v", err)
	}
}
