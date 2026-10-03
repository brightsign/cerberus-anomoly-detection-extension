// config-server: a tiny on-player HTTP server that serves the zone-picker page
// and reads/writes the live config file (/storage/sd/configs/config.json). It
// lets an operator edit the ROI zones in a browser pointed at the player, with a
// "Save & Restart" that applies them. Launched by bsext_init on its own port.
//
// The camera image for drawing comes from the image-stream-server (port 20200);
// this server only owns the page + config read/write + restart.
package main

import (
	_ "embed"
	"encoding/json"
	"errors"
	"flag"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
)

//go:embed web/zone-picker.html
var htmlPage []byte

//go:embed web/zone-picker.logic.js
var logicJS []byte

type server struct {
	configPath string
	restart    func() error
}

func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/zone-picker.logic.js", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/javascript")
		w.Write(logicJS)
	})
	mux.HandleFunc("/config", s.handleConfig)
	mux.HandleFunc("/restart", s.handleRestart)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" && r.URL.Path != "/zone-picker.html" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(htmlPage)
	})
	return mux
}

func (s *server) handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		b, err := os.ReadFile(s.configPath)
		if err != nil {
			http.Error(w, "config not found", http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write(b)
	case http.MethodPut, http.MethodPost:
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read error", http.StatusBadRequest)
			return
		}
		// Refuse to overwrite the live config with anything that is not valid JSON.
		if !json.Valid(body) {
			http.Error(w, "invalid JSON", http.StatusBadRequest)
			return
		}
		if err := atomicWrite(s.configPath, body); err != nil {
			http.Error(w, "write error: "+err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`))
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *server) handleRestart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if err := s.restart(); err != nil {
		http.Error(w, "restart failed: "+err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`))
}

// atomicWrite writes data to a temp file in the same dir then renames it over the
// target, so a crash mid-write never leaves a truncated config.
func atomicWrite(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func main() {
	port := flag.Int("port", 20300, "listen port")
	cfg := flag.String("config", "/storage/sd/configs/config.json", "config file to serve/edit")
	socHome := flag.String("soc-home", "", "extension dir containing bsext_init (for restart)")
	flag.Parse()

	s := &server{
		configPath: *cfg,
		restart: func() error {
			if *socHome == "" {
				return errors.New("soc-home not set; cannot restart")
			}
			// Detached (setsid) so we survive bsext_init restart stopping us, and
			// the HTTP response returns before the restart tears things down.
			return exec.Command("setsid", filepath.Join(*socHome, "bsext_init"), "restart").Start()
		},
	}
	addr := ":" + strconv.Itoa(*port)
	log.Printf("config-server listening on %s, editing %s", addr, *cfg)
	log.Fatal(http.ListenAndServe(addr, s.routes()))
}
