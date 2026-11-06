package cli

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

func writeBaseline(t *testing.T, path string, objects []baseline.Object) {
	t.Helper()
	b := baseline.New("test", baseline.KindLogical, "2.1.0", 160009, "app", baseline.NewScope(), objects)
	if err := baseline.Save(path, b, os.Stdout); err != nil {
		t.Fatal(err)
	}
}

func runCLI(args ...string) (int, string, string) {
	var out, errOut bytes.Buffer
	code := Run(context.Background(), args, &out, &errOut)
	return code, out.String(), errOut.String()
}

func TestVersionAndUsage(t *testing.T) {
	code, out, _ := runCLI("version")
	if code != 0 || !strings.Contains(out, "pg_integrity_checksums 0.1.0-dev") {
		t.Fatalf("version: code=%d out=%q", code, out)
	}
	code, out, _ = runCLI("help")
	if code != 0 || !strings.Contains(out, "Usage:") {
		t.Fatalf("help: code=%d out=%q", code, out)
	}
	code, _, _ = runCLI()
	if code != 2 {
		t.Fatalf("no args: code=%d", code)
	}
	code, _, _ = runCLI("nope")
	if code != 2 {
		t.Fatalf("unknown: code=%d", code)
	}
}

func TestShowAndDiff(t *testing.T) {
	dir := t.TempDir()
	oldPath := filepath.Join(dir, "old.json")
	newPath := filepath.Join(dir, "new.json")
	writeBaseline(t, oldPath, []baseline.Object{
		{Kind: baseline.ObjectTable, Schema: "public", Name: "t", Status: baseline.StatusOK, Checksum: "1"},
	})
	writeBaseline(t, newPath, []baseline.Object{
		{Kind: baseline.ObjectTable, Schema: "public", Name: "t", Status: baseline.StatusOK, Checksum: "2"},
	})

	code, out, errOut := runCLI("show", oldPath)
	if code != 0 || !strings.Contains(out, "objects: 1") {
		t.Fatalf("show: code=%d out=%q err=%q", code, out, errOut)
	}
	code, out, _ = runCLI("show", "--format", "json", oldPath)
	if code != 0 || !strings.Contains(out, "pg_integrity_checksums.baseline/v1") {
		t.Fatalf("show json: code=%d out=%q", code, out)
	}

	code, out, _ = runCLI("diff", oldPath, newPath)
	if code != 1 || !strings.Contains(out, "CHANGED") {
		t.Fatalf("diff: code=%d out=%q", code, out)
	}
	code, out, _ = runCLI("diff", "--format", "json", oldPath, newPath)
	if code != 1 || !strings.Contains(out, "changed") {
		t.Fatalf("diff json: code=%d out=%q", code, out)
	}
	code, _, _ = runCLI("diff", oldPath, newPath, "--no-fail")
	if code != 0 {
		t.Fatalf("diff no-fail: code=%d", code)
	}
	code, _, _ = runCLI("diff", oldPath, oldPath)
	if code != 0 {
		t.Fatalf("diff same: code=%d", code)
	}
}

func TestInvalidBaseline(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(bad, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	code, _, _ := runCLI("show", bad)
	if code != 3 {
		t.Fatalf("show bad: code=%d", code)
	}
	code, _, _ = runCLI("verify", "--baseline", bad)
	if code != 3 {
		t.Fatalf("verify bad: code=%d", code)
	}
	code, _, _ = runCLI("diff", bad, bad)
	if code != 3 {
		t.Fatalf("diff bad: code=%d", code)
	}
}

func TestUpdateRefusesWithoutYes(t *testing.T) {
	code, _, errOut := runCLI("update", "--baseline", "does-not-exist.json")
	if code != 4 {
		t.Fatalf("update refusal: code=%d err=%q", code, errOut)
	}
	if !strings.Contains(errOut, "without --yes") {
		t.Fatalf("unexpected error: %q", errOut)
	}
}

func TestShowHelp(t *testing.T) {
	code, _, errOut := runCLI("show", "--help")
	if code != 0 || !strings.Contains(errOut, "usage:") {
		t.Fatalf("show help: code=%d err=%q", code, errOut)
	}
	code, _, errOut = runCLI("diff", "--help")
	if code != 0 || !strings.Contains(errOut, "usage:") {
		t.Fatalf("diff help: code=%d err=%q", code, errOut)
	}
}
