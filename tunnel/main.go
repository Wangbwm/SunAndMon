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
	"syscall"
	"time"
)

func main() {
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
