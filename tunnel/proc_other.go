//go:build !windows

package main

import (
	"net"
	"os/exec"
)

func hideWindow(*exec.Cmd) {}

func exclusiveListenConfig() net.ListenConfig {
	return net.ListenConfig{}
}
