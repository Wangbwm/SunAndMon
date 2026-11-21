package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"math/big"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

func TestDeviceRegistrationAndPairing(t *testing.T) {
	const id = "0123456789abcdef0123456789abcdef"
	login := strings.Repeat("1", 64)
	hostKey := strings.Repeat("2", 64)
	clientFingerprint := strings.Repeat("3", 64)
	path := filepath.Join(t.TempDir(), "devices.json")
	c, err := newCenter(path, login)
	if err != nil {
		t.Fatal(err)
	}
	registrationOwner := ""
	call := func(method, uri, key, ownerKey, body string, status int) string {
		t.Helper()
		request := httptest.NewRequest(method, uri, strings.NewReader(body))
		request.Header.Set("Authorization", "Bearer "+key)
		request.Header.Set("X-SunAndMon-Host-Key", ownerKey)
		request.Header.Set("X-SunAndMon-Registration-Id", registrationOwner)
		response := httptest.NewRecorder()
		c.handler().ServeHTTP(response, request)
		if response.Code != status {
			t.Fatalf("%s %s returned %d, want %d: %s", method, uri, response.Code, status, response.Body)
		}
		return response.Body.String()
	}
	base := "/v1/devices"
	endpoint := base + "/" + id
	call("GET", base, "", "", "", http.StatusUnauthorized)
	call("GET", base, login, "", "", http.StatusOK)

	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	cert := &x509.Certificate{SerialNumber: big.NewInt(1), NotAfter: time.Now().Add(time.Hour)}
	der, err := x509.CreateCertificate(rand.Reader, cert, cert, publicKey, privateKey)
	if err != nil {
		t.Fatal(err)
	}
	input := registration{
		device: device{Name: "Workstation", Addresses: []string{"192.168.1.8", "192.168.1.8", "fd01::8"},
			Port: 47989, HTTPSPort: 47984, Certificate: string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}))},
		HostKey: hostKey,
	}
	encode := func(input registration) string {
		t.Helper()
		body, err := json.Marshal(struct {
			device
			HostKey string `json:"hostKey"`
		}{input.device, input.HostKey})
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)

	other := input
	other.HostKey = strings.Repeat("4", 64)
	call("PUT", endpoint, login, "", encode(other), http.StatusForbidden)
	other = input
	other.Addresses = []string{"127.0.0.1"}
	call("PUT", endpoint, login, "", encode(other), http.StatusBadRequest)
	list := call("GET", base, login, "", "", http.StatusOK)
	if !strings.Contains(list, `"online":true`) || strings.Contains(list, hostKey) || strings.Contains(list, "CERTIFICATE") {
		t.Fatalf("unexpected device list %s", list)
	}

	pairBody := `{"fingerprint":"` + clientFingerprint + `"}`
	grant := call("POST", endpoint+"/pair", login, "", pairBody, http.StatusOK)
	var result struct {
		device
		Ticket string
	}
	if err := json.Unmarshal([]byte(grant), &result); err != nil {
		t.Fatal(err)
	}
	prefix, signature, found := strings.Cut(result.Ticket, ".")
	nonce, signature, foundNonce := strings.Cut(signature, ".")
	if !found || !foundNonce || len(nonce) != 26 || signature != sign(hostKey, "ticket", prefix+"."+nonce+"."+clientFingerprint) {
		t.Fatalf("ticket was not bound to the certificate: %s", result.Ticket)
	}
	if len(result.Addresses) != 2 || result.Certificate != input.Certificate || strings.Contains(grant, hostKey) {
		t.Fatal("pairing response must contain the endpoint and public certificate only")
	}

	registrationOwner = strings.Repeat("6", 64)
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	registrationOwner = strings.Repeat("7", 64)
	call("PUT", endpoint, login, "", encode(input), http.StatusConflict)
	call("DELETE", endpoint, login, hostKey, "", http.StatusConflict)
	expired := c.devices[id]
	expired.LastSeen = time.Now().Add(-2 * onlineFor)
	c.devices[id] = expired
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	registrationOwner = strings.Repeat("6", 64)
	call("DELETE", endpoint, login, hostKey, "", http.StatusConflict)
	call("POST", endpoint+"/pair", login, "", pairBody, http.StatusOK)

	registrationOwner = ""
	c, err = newCenter(path, login)
	if err != nil {
		t.Fatal(err)
	}
	call("POST", endpoint+"/pair", login, "", pairBody, http.StatusNotFound)
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	call("DELETE", endpoint, login, strings.Repeat("5", 64), "", http.StatusForbidden)
	call("DELETE", endpoint, login, hostKey, "", http.StatusNoContent)
	call("POST", endpoint+"/pair", login, "", pairBody, http.StatusNotFound)

	adminKey := strings.Repeat("8", 64)
	adminHash := sha256.Sum256([]byte(adminKey))
	nodes := "/admin/api/nodes"
	adminEndpoint := nodes + "/" + id
	call("GET", nodes, adminKey, "", "", http.StatusServiceUnavailable)
	c.adminHash = &adminHash
	page := call("GET", "/admin/", "", "", "", http.StatusOK)
	if !strings.Contains(page, "节点管理") {
		t.Fatal("node management page not embedded")
	}
	call("GET", "/admin/app.js", "", "", "", http.StatusOK)
	call("GET", "/admin/app.css", "", "", "", http.StatusOK)
	call("GET", nodes, login, "", "", http.StatusUnauthorized)
	call("GET", base, adminKey, "", "", http.StatusUnauthorized)
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	call("PATCH", adminEndpoint, login, "", "{}", http.StatusUnauthorized)
	call("DELETE", adminEndpoint, login, "", "", http.StatusUnauthorized)
	call("PATCH", adminEndpoint, adminKey, "", `{"addresses":["127.0.0.1"]}`, http.StatusBadRequest)
	call("PATCH", adminEndpoint, adminKey, "", `{"httpsPort":65536}`, http.StatusBadRequest)
	call("PATCH", adminEndpoint, adminKey, "", `{"name":"客厅电脑","addresses":["::ffff:192.168.2.10","192.168.2.10"],"port":48089,"httpsPort":48084}`, http.StatusNoContent)
	if got := call("GET", nodes, adminKey, "", "", http.StatusOK); !strings.Contains(got, "Workstation") ||
		!strings.Contains(got, "客厅电脑") || strings.Contains(got, hostKey) || strings.Contains(got, "CERTIFICATE") {
		t.Fatal("admin list should contain reported and edited fields without cryptographic credentials")
	}
	input.Name = "Updated Workstation"
	input.Port = 48189
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	if got := call("GET", base, login, "", "", http.StatusOK); !strings.Contains(got, "客厅电脑") {
		t.Fatal("display name override was not applied")
	}
	if err := json.Unmarshal([]byte(call("POST", endpoint+"/pair", login, "", pairBody, http.StatusOK)), &result); err != nil {
		t.Fatal(err)
	}
	if result.Name != "客厅电脑" || !slices.Equal(result.Addresses, []string{"192.168.2.10"}) ||
		result.Port != 48089 || result.HTTPSPort != 48084 || result.Certificate != input.Certificate {
		t.Fatal("pairing should return normalized overrides and the original certificate")
	}
	call("DELETE", endpoint, login, hostKey, "", http.StatusNoContent)
	call("POST", endpoint+"/pair", login, "", pairBody, http.StatusNotFound)
	c, err = newCenter(path, login)
	if err != nil {
		t.Fatal(err)
	}
	c.adminHash = &adminHash
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
	if got := call("GET", base, login, "", "", http.StatusOK); !strings.Contains(got, "客厅电脑") {
		t.Fatal("administrator edits did not survive a host shutdown and server restart")
	}
	call("PATCH", adminEndpoint, adminKey, "", "{}", http.StatusNoContent)
	if got := call("GET", base, login, "", "", http.StatusOK); !strings.Contains(got, "Updated Workstation") {
		t.Fatal("clearing overrides should restore the latest reported values")
	}
	call("DELETE", adminEndpoint, adminKey, "", "", http.StatusNoContent)
	call("POST", endpoint+"/pair", login, "", pairBody, http.StatusNotFound)
	if got := call("GET", base, login, "", "", http.StatusOK); strings.TrimSpace(got) != "[]" {
		t.Fatal("deleted node still visible to clients")
	}
	call("DELETE", endpoint, login, hostKey, "", http.StatusNoContent)
	c, err = newCenter(path, login)
	if err != nil {
		t.Fatal(err)
	}
	c.adminHash = &adminHash
	call("PUT", endpoint, login, "", encode(input), http.StatusForbidden)
	if got := call("GET", nodes, adminKey, "", "", http.StatusOK); !strings.Contains(got, `"deleted":true`) {
		t.Fatal("deletion was not persisted")
	}
	call("POST", adminEndpoint+"/restore", adminKey, "", "", http.StatusNoContent)
	call("PUT", endpoint, login, "", encode(input), http.StatusNoContent)
}

func TestDirectoryDeduplicatesOldEndpoints(t *testing.T) {
	now := time.Now()
	workstation := device{
		ID: "current", Name: "Workstation", Certificate: "current cert",
		Addresses: []string{"192.168.1.8", "fd01::8"}, Port: 47989, HTTPSPort: 47984,
	}
	old := workstation
	old.ID, old.Name, old.Certificate = "old", "workstation", "old cert"
	old.Addresses = []string{"192.168.1.8"}

	sameCert := workstation
	sameCert.ID, sameCert.Port, sameCert.HTTPSPort = "same-cert", 48189, 48184
	sameCert.Addresses = []string{"192.168.1.9"}

	differentPort := workstation
	differentPort.ID, differentPort.Port, differentPort.HTTPSPort = "different-port", 48189, 48184
	differentPort.Certificate = "different port cert"

	differentHost := workstation
	differentHost.ID, differentHost.Name, differentHost.Certificate = "different-host", "Other PC", "other cert"

	otherOnline := old
	otherOnline.ID = "other-online"

	c := &center{devices: map[string]registration{
		workstation.ID:   {device: workstation, LastSeen: now},
		old.ID:           {device: old, LastSeen: now.Add(-2 * onlineFor)},
		sameCert.ID:      {device: sameCert, LastSeen: now.Add(-3 * onlineFor)},
		differentPort.ID: {device: differentPort, LastSeen: now.Add(-2 * onlineFor)},
		differentHost.ID: {device: differentHost, LastSeen: now.Add(-2 * onlineFor)},
		otherOnline.ID:   {device: otherOnline, LastSeen: now},
	}}
	response := httptest.NewRecorder()
	c.list(response, httptest.NewRequest(http.MethodGet, "/v1/devices", nil))
	var listing []struct{ ID string }
	if err := json.Unmarshal(response.Body.Bytes(), &listing); err != nil {
		t.Fatal(err)
	}
	var ids []string
	for _, row := range listing {
		ids = append(ids, row.ID)
	}
	slices.Sort(ids)
	if want := []string{"current", "different-host", "different-port", "other-online"}; !slices.Equal(ids, want) {
		t.Fatalf("visible device IDs = %v, want %v", ids, want)
	}
	if len(c.devices) != 6 || response.Header().Get("X-SunAndMon-Deduplicated") != "1" {
		t.Fatal("directory should filter the listing and keep the original registrations")
	}
}
