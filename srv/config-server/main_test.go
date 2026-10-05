package main

import (
	"bytes"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestGetConfigReturnsFile(t *testing.T) {
	cfg := filepath.Join(t.TempDir(), "config.json")
	os.WriteFile(cfg, []byte(`{"device":{"camera_device":"usb_camera"}}`), 0644)
	s := &server{configPath: cfg, restart: func() error { return nil }}
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("GET", "/config", nil))
	if w.Code != 200 {
		t.Fatalf("GET /config code=%d", w.Code)
	}
	if !bytes.Contains(w.Body.Bytes(), []byte("usb_camera")) {
		t.Fatalf("body=%s", w.Body.String())
	}
}

func TestGetConfigMissing404(t *testing.T) {
	s := &server{configPath: filepath.Join(t.TempDir(), "nope.json"), restart: func() error { return nil }}
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("GET", "/config", nil))
	if w.Code != 404 {
		t.Fatalf("expected 404, got %d", w.Code)
	}
}

func TestPutConfigWritesValidJSON(t *testing.T) {
	cfg := filepath.Join(t.TempDir(), "config.json")
	os.WriteFile(cfg, []byte(`{"old":true}`), 0644)
	s := &server{configPath: cfg, restart: func() error { return nil }}
	body := `{"device":{"camera_device":"usb_camera"},"roi":{"mode":"rect","tvs":[]}}`
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("PUT", "/config", bytes.NewBufferString(body)))
	if w.Code != 200 {
		t.Fatalf("PUT code=%d body=%s", w.Code, w.Body.String())
	}
	got, _ := os.ReadFile(cfg)
	if !bytes.Contains(got, []byte("usb_camera")) {
		t.Fatalf("file not updated: %s", got)
	}
}

func TestPutConfigRejectsInvalidJSON(t *testing.T) {
	cfg := filepath.Join(t.TempDir(), "config.json")
	os.WriteFile(cfg, []byte(`{"keep":1}`), 0644)
	s := &server{configPath: cfg, restart: func() error { return nil }}
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("PUT", "/config", bytes.NewBufferString(`{bad json`)))
	if w.Code != 400 {
		t.Fatalf("expected 400, got %d", w.Code)
	}
	got, _ := os.ReadFile(cfg)
	if !bytes.Contains(got, []byte("keep")) {
		t.Fatalf("file should be unchanged: %s", got)
	}
}

func TestRestartInvokesHook(t *testing.T) {
	called := false
	s := &server{configPath: "/tmp/x", restart: func() error { called = true; return nil }}
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("POST", "/restart", nil))
	if w.Code != 200 || !called {
		t.Fatalf("restart not invoked; code=%d called=%v", w.Code, called)
	}
}

func TestRootServesPage(t *testing.T) {
	s := &server{configPath: "/tmp/x", restart: func() error { return nil }}
	w := httptest.NewRecorder()
	s.routes().ServeHTTP(w, httptest.NewRequest("GET", "/", nil))
	if w.Code != 200 {
		t.Fatalf("GET / code=%d", w.Code)
	}
	if !bytes.Contains(w.Body.Bytes(), []byte("Zone Picker")) {
		t.Fatalf("embedded page missing")
	}
}
