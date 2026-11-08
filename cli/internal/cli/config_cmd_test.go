package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInitCreatesConfig(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "nested", ".pgics.yaml")
	code, out, errOut := runCLI("init",
		"--output", path,
		"--profile", "prod",
		"--dsn", "postgresql://user:pass@localhost/app",
		"--baseline", "baselines/prod.json",
		"--schema", "public",
		"--schema", "app",
	)
	if code != 0 {
		t.Fatalf("init: code=%d out=%q err=%q", code, out, errOut)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	for _, want := range []string{"default: prod", "dsn: postgresql://user:pass@localhost/app", "baseline: baselines/prod.json", "- public", "- app", "fail_on: any"} {
		if !strings.Contains(text, want) {
			t.Fatalf("missing %q in %q", want, text)
		}
	}

	if code, _, errOut := runCLI("init", "--output", path); code != 2 {
		t.Fatalf("overwrite without force: code=%d err=%q", code, errOut)
	}
	if code, _, errOut := runCLI("init", "--output", path, "--force", "--quiet"); code != 0 {
		t.Fatalf("force init: code=%d err=%q", code, errOut)
	}
	if info, err := os.Stat(filepath.Join(dir, "nested", "baselines")); err != nil || !info.IsDir() {
		t.Fatalf("baseline directory: %v %v", info, err)
	}
}

func TestInitStdout(t *testing.T) {
	code, out, errOut := runCLI("init", "--output", "-", "--profile", "demo", "--dsn", "env:DATABASE_URL", "--schema", "public")
	if code != 0 || !strings.Contains(out, "default: demo") || !strings.Contains(out, "dsn: env:DATABASE_URL") {
		t.Fatalf("init stdout: code=%d out=%q err=%q", code, out, errOut)
	}
}

func TestConfigValidateAndShow(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "pgics.yaml")
	text := `default: prod
profiles:
  prod:
    dsn: postgresql://user:secret@localhost/app
    baseline: baselines/prod.json
    output: baselines/prod.json
    schema: [public]
    fail_on: changed
    format: json
    workers: 2
    timeout: 5s
`
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}

	code, out, errOut := runCLI("config", "validate", "--config", path)
	if code != 0 || !strings.Contains(out, "config OK") {
		t.Fatalf("validate: code=%d out=%q err=%q", code, out, errOut)
	}

	code, out, errOut = runCLI("config", "show", "--config", path)
	if code != 0 {
		t.Fatalf("show: code=%d err=%q", code, errOut)
	}
	for _, want := range []string{"profile: prod", "workers: 2", "fail_on: changed", "***"} {
		if !strings.Contains(out, want) {
			t.Fatalf("show missing %q: %q", want, out)
		}
	}
	if strings.Contains(out, "secret") {
		t.Fatalf("show leaked DSN password: %q", out)
	}
}

func TestConfigValidateRejectsBadValues(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "pgics.yaml")
	text := `default: prod
profiles:
  prod:
    dsn: env:PGICS_MISSING_DSN
    timeout: nope
`
	if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PGICS_MISSING_DSN", "")
	code, _, errOut := runCLI("config", "validate", "--config", path)
	if code != 2 || !strings.Contains(errOut, "environment variable") {
		t.Fatalf("bad config: code=%d err=%q", code, errOut)
	}

	if code, _, _ := runCLI("config", "validate", "--config", filepath.Join(dir, "missing.yaml")); code != 2 {
		t.Fatalf("missing config: code=%d", code)
	}
}
