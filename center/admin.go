package main

import (
	"embed"
	"io/fs"
	"log"
	"net/http"
	"slices"
	"strings"
	"time"
)

//go:embed admin/index.html admin/app.css admin/app.js
var adminFiles embed.FS

type nodeOverride struct {
	Name      string   `json:"name,omitempty"`
	Addresses []string `json:"addresses,omitempty"`
	Port      uint16   `json:"port,omitempty"`
	HTTPSPort uint16   `json:"httpsPort,omitempty"`
}

func (r registration) effectiveDevice() device {
	d := r.device
	if v := r.Override; v != nil {
		if v.Name != "" {
			d.Name = v.Name
		}
		if len(v.Addresses) != 0 {
			d.Addresses = v.Addresses
		}
		if v.Port != 0 {
			d.Port = v.Port
		}
		if v.HTTPSPort != 0 {
			d.HTTPSPort = v.HTTPSPort
		}
	}
	return d
}

type adminNode struct {
	ID       string        `json:"id"`
	Reported nodeOverride  `json:"reported"`
	Override *nodeOverride `json:"override"`
	Online   bool          `json:"online"`
	Deleted  bool          `json:"deleted"`
}

func (c *center) addAdminRoutes(mux *http.ServeMux) {
	static, err := fs.Sub(adminFiles, "admin")
	if err != nil {
		panic(err)
	}
	files := http.StripPrefix("/admin/", http.FileServerFS(static))
	serveAsset := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		files.ServeHTTP(w, r)
	})
	mux.Handle("GET /admin/{$}", serveAsset)
	mux.Handle("GET /admin/app.css", serveAsset)
	mux.Handle("GET /admin/app.js", serveAsset)
	api := http.NewServeMux()
	api.HandleFunc("GET /admin/api/nodes", c.adminList)
	api.HandleFunc("PATCH /admin/api/nodes/{id}", c.adminEdit)
	api.HandleFunc("DELETE /admin/api/nodes/{id}", c.adminDelete)
	api.HandleFunc("POST /admin/api/nodes/{id}/restore", c.adminRestore)
	api.HandleFunc("GET /admin/api/tunnel-config", c.adminGetTunnelConfig)
	api.HandleFunc("PUT /admin/api/tunnel-config", c.adminPutTunnelConfig)
	mux.Handle("/admin/api/", requireKey(c.adminHash, api))
}

func (c *center) adminList(w http.ResponseWriter, _ *http.Request) {
	now := time.Now()
	c.mu.Lock()
	nodes := make([]adminNode, 0, len(c.devices))
	for id, host := range c.devices {
		nodes = append(nodes, adminNode{
			ID: id, Reported: nodeOverride{host.Name, host.Addresses, host.Port, host.HTTPSPort},
			Override: host.Override, Online: !host.Deleted && now.Sub(host.LastSeen) < onlineFor,
			Deleted: host.Deleted,
		})
	}
	c.mu.Unlock()
	slices.SortFunc(nodes, func(a, b adminNode) int {
		if a.Deleted != b.Deleted {
			if a.Deleted {
				return 1
			}
			return -1
		}
		if a.Online != b.Online {
			if a.Online {
				return -1
			}
			return 1
		}
		return strings.Compare(a.ID, b.ID)
	})
	jsonResponse(w, nodes)
}

func (c *center) adminEdit(w http.ResponseWriter, r *http.Request) {
	var input nodeOverride
	if !readJSON(w, r, &input) {
		return
	}
	input.Name = strings.TrimSpace(input.Name)
	if (input.Name != "" && !validDeviceName(input.Name)) || len(input.Addresses) > 16 {
		http.Error(w, "invalid node configuration", http.StatusBadRequest)
		return
	}
	addresses, err := normalizeAddresses(input.Addresses)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	input.Addresses = addresses

	c.mu.Lock()
	defer c.mu.Unlock()
	id := r.PathValue("id")
	old, ok := c.devices[id]
	if !ok || old.Deleted {
		http.NotFound(w, r)
		return
	}
	updated := old
	updated.Override = nil
	if input.Name != "" || len(input.Addresses) != 0 || input.Port != 0 || input.HTTPSPort != 0 {
		updated.Override = &input
	}
	c.devices[id] = updated
	c.saveAdminChange(w, id, old)
}

func (c *center) adminDelete(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := r.PathValue("id")
	old, ok := c.devices[id]
	if !ok {
		http.NotFound(w, r)
		return
	}
	permanent := r.URL.Query().Get("permanent") == "1" || old.Deleted
	if permanent {
		delete(c.devices, id)
	} else {
		c.devices[id] = registration{
			device: device{ID: id, Name: old.effectiveDevice().Name}, Deleted: true,
		}
	}
	c.saveAdminChange(w, id, old)
}

func (c *center) adminRestore(w http.ResponseWriter, r *http.Request) {
	c.mu.Lock()
	defer c.mu.Unlock()
	id := r.PathValue("id")
	old, ok := c.devices[id]
	if !ok || !old.Deleted {
		http.NotFound(w, r)
		return
	}
	delete(c.devices, id)
	c.saveAdminChange(w, id, old)
}

// The caller holds c.mu for the entire write so a heartbeat cannot race an
// edit, deletion, restoration, or rollback.
func (c *center) saveAdminChange(w http.ResponseWriter, id string, old registration) {
	if err := c.save(); err != nil {
		c.devices[id] = old
		log.Print(err)
		http.Error(w, "could not save node registry", http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
