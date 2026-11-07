package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeConfig(t *testing.T, text string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "pgics.yaml")
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoadAndSelect(t *testing.T) {
	path := writeConfig(t, strings.Join([]string{
		"default: prod",
		"profiles:",
		"  prod:",
		"    dsn: env:PGICS_TEST_DSN",
		"    schema: [public, app_*]",
		"    exclude_tables: []",
		"    workers: 4",
		"    include_indexes: true",
		"  staging:",
		"    dsn: host=/tmp dbname=staging",
	}, "\n"))
	f, gotPath, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if gotPath != path {
		t.Fatalf("path %q", gotPath)
	}
	p, err := f.Select("")
	if err != nil {
		t.Fatal(err)
	}
	if p.DSN != "env:PGICS_TEST_DSN" || len(p.Schemas) != 2 || p.Workers == nil || *p.Workers != 4 {
		t.Fatalf("bad profile: %#v", p)
	}
	t.Setenv("PGICS_TEST_DSN", "host=/tmp dbname=test")
	if p.ResolveDSN() != "host=/tmp dbname=test" {
		t.Fatalf("env dsn not resolved: %q", p.ResolveDSN())
	}
	staging, err := f.Select("staging")
	if err != nil || staging.DSN != "host=/tmp dbname=staging" {
		t.Fatalf("staging: %#v %v", staging, err)
	}
	if _, err := f.Select("missing"); err == nil {
		t.Fatal("expected unknown profile error")
	}
}

func TestLoadMissingDefaultIsOptional(t *testing.T) {
	old, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(old) })
	t.Setenv("PGICS_CONFIG", "")
	f, path, err := Load("")
	if err != nil || f != nil || path != "" {
		t.Fatalf("optional config: %#v %q %v", f, path, err)
	}
}

func TestLoadRejectsBadYAML(t *testing.T) {
	path := writeConfig(t, "profiles: [")
	if _, _, err := Load(path); err == nil {
		t.Fatal("expected parse error")
	}
}

func TestBaseDir(t *testing.T) {
	if got := BaseDir("/etc/pgics/config.yaml"); got != "/etc/pgics" {
		t.Fatalf("BaseDir: %q", got)
	}
	if got := BaseDir(""); got != "." {
		t.Fatalf("BaseDir empty: %q", got)
	}
}
