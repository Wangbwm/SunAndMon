//go:build windows

package main

import (
	"net"
	"os/exec"
	"syscall"
)

const soExclusiveAddrUse = ^syscall.SO_REUSEADDR // -5 (Winsock SO_EXCLUSIVEADDRUSE)

func hideWindow(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
}

// exclusiveListenConfig disables Go's default SO_REUSEADDR on Windows and sets
// SO_EXCLUSIVEADDRUSE so port probes and local tunnel listeners detect wildcard
// listeners (such as local Sunshine on 0.0.0.0:47989) instead of silently co-binding.
func exclusiveListenConfig() net.ListenConfig {
	return net.ListenConfig{
		Control: func(network, address string, c syscall.RawConn) error {
			var sockErr error
			if err := c.Control(func(fd uintptr) {
				_ = syscall.SetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 0)
				sockErr = syscall.SetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, soExclusiveAddrUse, 1)
			}); err != nil {
				return err
			}
			return sockErr
		},
	}
}
