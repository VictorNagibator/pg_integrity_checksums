package cli

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

func TestIntegrationConfigOutputsAndGlob(t *testing.T) {
	db, schema := setupCLITestSchema(t)
	dir := t.TempDir()
	dsn := os.Getenv("PGICS_TEST_DSN")
	configPath := filepath.Join(dir, "pgics.yaml")
	configText := strings.Join([]string{
		"default: test",
		"profiles:",
		"  test:",
		"    dsn: " + dsn,
		"    schemas: [" + schema + "]",
		"    output: baseline.json",
		"    baseline: baseline.json",
		"    workers: 1",
		"    timeout: 30s",
		"    connect_timeout: 10s",
		"    application_name: pgics_it",
	}, "\n")
	if err := os.WriteFile(configPath, []byte(configText), 0o600); err != nil {
		t.Fatal(err)
	}

	if code, _, errOut := runCLI("capture", "--config", configPath, "--profile", "test", "--quiet"); code != 0 {
		t.Fatalf("config capture: code=%d err=%s", code, errOut)
	}
	if code, _, errOut := runCLI("verify", "--config", configPath, "--profile", "test", "--quiet"); code != 0 {
		t.Fatalf("config verify: code=%d err=%s", code, errOut)
	}

	report := filepath.Join(dir, "report.json")
	if code, _, errOut := runCLI("verify", "--config", configPath, "--profile", "test", "--format", "json", "--output", report, "--quiet"); code != 0 {
		t.Fatalf("verify output: code=%d err=%s", code, errOut)
	}
	data, err := os.ReadFile(report)
	if err != nil || !strings.Contains(string(data), "summary") {
		t.Fatalf("report: %v %q", err, data)
	}

	junit := filepath.Join(dir, "junit.xml")
	baselinePath := filepath.Join(dir, "baseline.json")
	if code, _, errOut := runCLI("diff", baselinePath, baselinePath, "--format", "junit", "--output", junit); code != 0 {
		t.Fatalf("junit diff: code=%d err=%s", code, errOut)
	}
	junitData, err := os.ReadFile(junit)
	if err != nil || !strings.Contains(string(junitData), "<testsuite") {
		t.Fatalf("junit: %v %q", err, junitData)
	}

	globPath := filepath.Join(dir, "glob.json")
	if code, _, errOut := runCLI("capture", "--dsn", dsn, "--schema", schema, "--table", "*accounts*", "--output", globPath, "--quiet"); code != 0 {
		t.Fatalf("glob capture: code=%d err=%s", code, errOut)
	}
	glob := loadBaselineForTest(t, globPath)
	if len(glob.Objects) != 3 {
		t.Fatalf("glob expected table + 2 indexes, got %v", objectNames(glob))
	}

	if _, err := db.Exec("ALTER TABLE " + schema + ".accounts ADD COLUMN extra text"); err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("verify", "--config", configPath, "--profile", "test", "--fail-on", "schema-changed", "--quiet"); code != 1 {
		t.Fatalf("schema-changed fail-on: code=%d err=%s", code, errOut)
	}
}

func TestIntegrationDoctorVerbose(t *testing.T) {
	_, schema := setupCLITestSchema(t)
	code, out, errOut := runCLI("doctor", "--db", os.Getenv("PGICS_TEST_DSN"), "--verbose", "--quiet")
	if code != 0 {
		t.Fatalf("doctor: code=%d err=%s", code, errOut)
	}
	if !strings.Contains(out, "preflight_problems") || !strings.Contains(out, schema+".accounts") {
		t.Fatalf("doctor output: %q", out)
	}
	code, out, errOut = runCLI("doctor", "--db", os.Getenv("PGICS_TEST_DSN"), "--format", "json", "--quiet")
	if code != 0 || !strings.Contains(out, "preflight_problems") {
		t.Fatalf("doctor json: code=%d out=%q err=%s", code, out, errOut)
	}
}

func TestCompletionOutputs(t *testing.T) {
	for _, shell := range []string{"bash", "zsh", "fish", "powershell"} {
		code, out, errOut := runCLI("completion", shell)
		if code != 0 || strings.TrimSpace(out) == "" {
			t.Fatalf("completion %s: code=%d out=%q err=%s", shell, code, out, errOut)
		}
	}
}

func TestBaselineGzipRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "baseline.json.gz")
	b := baseline.New("0.2.0-dev", baseline.KindLogical, "2.1.0", 160009, "app", baseline.NewScope(), []baseline.Object{{
		Kind: baseline.ObjectTable, Schema: "public", Name: "t", Status: baseline.StatusOK, Checksum: "1",
	}})
	if err := baseline.Save(path, b, os.Stdout); err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("show", path); code != 0 {
		t.Fatalf("show gzip: code=%d err=%s", code, errOut)
	}
}
