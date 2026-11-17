package cli

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/stdlib"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

// TestIntegrationCompare compares the database of PGICS_TEST_DSN with a
// second database it creates on the same server (needs CREATEDB and the
// extension available on the server).
func TestIntegrationCompare(t *testing.T) {
	dsn := os.Getenv("PGICS_TEST_DSN")
	if dsn == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	ctx := context.Background()
	src, err := pgdb.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer src.Close()

	dbname := fmt.Sprintf("pgics_cmp_%d", time.Now().UnixNano())
	if _, err := src.ExecContext(ctx, "CREATE DATABASE "+dbname); err != nil {
		t.Skipf("cannot create a second database: %v", err)
	}
	defer src.ExecContext(ctx, "DROP DATABASE "+dbname)

	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Database = dbname
	targetDSN := stdlib.RegisterConnConfig(cfg)
	defer stdlib.UnregisterConnConfig(targetDSN)
	tgt, err := pgdb.Open(ctx, targetDSN)
	if err != nil {
		t.Fatal(err)
	}
	defer tgt.Close()
	if _, err := tgt.ExecContext(ctx, "CREATE EXTENSION pg_integrity_checksums"); err != nil {
		t.Fatal(err)
	}

	schema := fmt.Sprintf("pgics_cmp_%d", time.Now().UnixNano())
	setup := []string{
		"CREATE SCHEMA " + schema,
		"CREATE TABLE " + schema + ".orders (id int PRIMARY KEY, note text)",
		"INSERT INTO " + schema + ".orders SELECT g, 'n' || g FROM generate_series(1, 5000) g",
		"CREATE TABLE " + schema + ".tags (tag text)",
		"INSERT INTO " + schema + ".tags VALUES ('a'), ('a'), ('b')",
	}
	for _, q := range setup {
		if _, err := src.ExecContext(ctx, q); err != nil {
			t.Fatal(err)
		}
		if _, err := tgt.ExecContext(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	defer src.ExecContext(ctx, "DROP SCHEMA "+schema+" CASCADE")

	args := []string{"compare", "--source", dsn, "--target", targetDSN, "--schema", schema, "--quiet"}
	code, out, errOut := runCLI(args...)
	if code != 0 || !strings.Contains(out, "no differences") {
		t.Fatalf("equal: code=%d out=%s err=%s", code, out, errOut)
	}

	for _, q := range []string{
		"UPDATE " + schema + ".orders SET note = 'x' WHERE id = 42",
		"DELETE FROM " + schema + ".orders WHERE id = 4242",
		"INSERT INTO " + schema + ".orders VALUES (9999, 'new')",
		"DELETE FROM " + schema + ".tags WHERE ctid = (SELECT min(ctid) FROM " + schema + ".tags WHERE tag = 'a')",
	} {
		if _, err := tgt.ExecContext(ctx, q); err != nil {
			t.Fatal(err)
		}
	}

	code, out, errOut = runCLI(append(args, "--format", "json", "--workers", "2")...)
	if code != 1 {
		t.Fatalf("different: code=%d out=%s err=%s", code, out, errOut)
	}
	var report compareReport
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatal(err)
	}
	s := report.Summary
	if s.Tables != 2 || s.Different != 2 || s.ChangedRows != 1 || s.MissingRows != 2 || s.ExtraRows != 1 {
		t.Fatalf("summary: %+v\n%s", s, out)
	}
	got := map[string]string{}
	for _, tbl := range report.Tables {
		for _, r := range tbl.Rows {
			got[tbl.Name+" "+r.Key] = r.Kind
		}
	}
	want := map[string]string{
		"orders (42)":   "changed",
		"orders (4242)": "missing",
		"orders (9999)": "extra",
		"tags (a)":      "missing",
	}
	for k, v := range want {
		if got[k] != v {
			t.Fatalf("row %s: got %q, want %q (all: %v)", k, got[k], v, got)
		}
	}

	code, _, errOut = runCLI(append(args, "--no-fail")...)
	if code != 0 {
		t.Fatalf("--no-fail: code=%d err=%s", code, errOut)
	}
}
