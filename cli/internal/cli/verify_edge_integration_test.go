package cli

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/stdlib"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

// newEmptyDatabase creates a database with nothing but the extension on the
// server of PGICS_TEST_DSN (needs CREATEDB and the extension available on the
// server) and returns a DSN for it and a connection to it.  Both are cleaned
// up when the test ends.
func newEmptyDatabase(t *testing.T) (string, *sql.DB) {
	t.Helper()
	dsn := os.Getenv("PGICS_TEST_DSN")
	if dsn == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	ctx := context.Background()
	admin, err := pgdb.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { admin.Close() })

	dbname := fmt.Sprintf("pgics_empty_%d", time.Now().UnixNano())
	if _, err := admin.ExecContext(ctx, "CREATE DATABASE "+dbname); err != nil {
		t.Skipf("cannot create a database: %v", err)
	}
	t.Cleanup(func() { admin.ExecContext(ctx, "DROP DATABASE "+dbname) })

	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Database = dbname
	emptyDSN := stdlib.RegisterConnConfig(cfg)
	t.Cleanup(func() { stdlib.UnregisterConnConfig(emptyDSN) })

	db, err := pgdb.Open(ctx, emptyDSN)
	if err != nil {
		t.Fatal(err)
	}
	// Registered after DROP DATABASE, so it runs first: the database cannot
	// be dropped while this connection is open.
	t.Cleanup(func() { db.Close() })
	if _, err := db.ExecContext(ctx, "CREATE EXTENSION pg_integrity_checksums"); err != nil {
		t.Fatal(err)
	}
	return emptyDSN, db
}

// verifyJSON runs verify with a JSON report and decodes it.
func verifyJSON(t *testing.T, dsn, path string, extra ...string) (int, baseline.Summary, []baseline.Change) {
	t.Helper()
	args := append([]string{"verify", "--db", dsn, "--baseline", path, "--format", "json", "--quiet"}, extra...)
	code, out, errOut := runCLI(args...)
	var report struct {
		Summary baseline.Summary  `json:"summary"`
		Changes []baseline.Change `json:"changes"`
	}
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatalf("verify %v: code=%d err=%s: %v\n%s", extra, code, errOut, err, out)
	}
	return code, report.Summary, report.Changes
}

func captureBaseline(t *testing.T, dsn string, extra ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "baseline.json")
	args := append([]string{"capture", "--db", dsn, "--output", path, "--quiet"}, extra...)
	if code, _, errOut := runCLI(args...); code != 0 {
		t.Fatalf("capture %v: code=%d err=%s", extra, code, errOut)
	}
	return path
}

func objectCount(t *testing.T, path string) int {
	t.Helper()
	b, err := baseline.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	return len(b.Objects)
}

// TestIntegrationVerifyEmptyDatabase: a database without user objects
// captures an empty baseline, which verifies clean in every report format.
// Objects that appear later are new, and objects of a baseline that are gone
// are missing; both fail verify by default.
func TestIntegrationVerifyEmptyDatabase(t *testing.T) {
	dsn, db := newEmptyDatabase(t)
	ctx := context.Background()

	empty := captureBaseline(t, dsn)
	if n := objectCount(t, empty); n != 0 {
		t.Fatalf("empty database captured %d objects", n)
	}

	code, out, errOut := runCLI("verify", "--db", dsn, "--baseline", empty)
	if code != 0 || !strings.Contains(out, "no changes") ||
		!strings.Contains(out, "checked=0 ok=0 changed=0 schema_changed=0 new=0 missing=0 skipped=0") {
		t.Fatalf("text: code=%d out=%s err=%s", code, out, errOut)
	}

	code, out, errOut = runCLI("verify", "--db", dsn, "--baseline", empty, "--format", "json", "--quiet")
	if code != 0 {
		t.Fatalf("json: code=%d out=%s err=%s", code, out, errOut)
	}
	// An empty list, not null, so that consumers can iterate it.
	if !strings.Contains(out, `"changes": []`) {
		t.Fatalf("json changes is not an empty list: %s", out)
	}
	_, summary, _ := verifyJSON(t, dsn, empty)
	if summary != (baseline.Summary{}) {
		t.Fatalf("json summary: %+v", summary)
	}

	code, out, errOut = runCLI("verify", "--db", dsn, "--baseline", empty, "--format", "junit", "--quiet")
	if code != 0 || !strings.Contains(out, `tests="0" failures="0"`) {
		t.Fatalf("junit: code=%d out=%s err=%s", code, out, errOut)
	}

	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", empty, "--changed-since", "--quiet")
	if code != 0 {
		t.Fatalf("--changed-since: code=%d err=%s", code, errOut)
	}

	// A table created after the capture, with its primary key index.
	if _, err := db.ExecContext(ctx, "CREATE TABLE t (id int PRIMARY KEY)"); err != nil {
		t.Fatal(err)
	}
	code, summary, changes := verifyJSON(t, dsn, empty)
	if code != 1 || summary.New != 2 || summary.Total() != 2 {
		t.Fatalf("new objects: code=%d summary=%+v changes=%+v", code, summary, changes)
	}
	for _, c := range changes {
		if c.Status != "new" || c.BaselineChecksum != "" {
			t.Fatalf("new object: %+v", c)
		}
	}
	if code, _, _ := verifyJSON(t, dsn, empty, "--fail-on", "changed"); code != 0 {
		t.Fatalf("new objects with --fail-on changed: code=%d", code)
	}

	// The other way round: the baseline has objects, the database none.
	withTable := captureBaseline(t, dsn)
	if _, err := db.ExecContext(ctx, "DROP TABLE t"); err != nil {
		t.Fatal(err)
	}
	code, summary, changes = verifyJSON(t, dsn, withTable)
	if code != 1 || summary.Missing != 2 || summary.Total() != 2 {
		t.Fatalf("missing objects: code=%d summary=%+v changes=%+v", code, summary, changes)
	}
	if code, _, _ := verifyJSON(t, dsn, withTable, "--fail-on", "new"); code != 0 {
		t.Fatalf("missing objects with --fail-on new: code=%d", code)
	}
}

// TestIntegrationVerifyNoUserTables: views, functions and types have no
// checksum, so a database whose schemas hold only those captures an empty
// baseline, like an empty database.  So does a --schema filter that matches
// no schema: capture does not warn, and the baseline verifies clean.
func TestIntegrationVerifyNoUserTables(t *testing.T) {
	dsn, db := newEmptyDatabase(t)
	ctx := context.Background()

	for _, stmt := range []string{
		"CREATE SCHEMA app",
		"CREATE VIEW app.v AS SELECT 1 AS x",
		"CREATE FUNCTION app.f() RETURNS int LANGUAGE sql AS 'SELECT 1'",
		"CREATE TYPE app.pair AS (a int, b int)",
		"CREATE TYPE app.mood AS ENUM ('ok', 'not ok')",
	} {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}

	whole := captureBaseline(t, dsn)
	scoped := captureBaseline(t, dsn, "--schema", "app")
	nomatch := captureBaseline(t, dsn, "--schema", "no_such_schema")
	for name, path := range map[string]string{"whole": whole, "--schema app": scoped, "no match": nomatch} {
		if n := objectCount(t, path); n != 0 {
			t.Fatalf("%s: captured %d objects", name, n)
		}
		if code, summary, _ := verifyJSON(t, dsn, path); code != 0 || summary != (baseline.Summary{}) {
			t.Fatalf("%s: code=%d summary=%+v", name, code, summary)
		}
	}

	// The system catalogs, left out above, are captured with
	// --include-system.
	system := captureBaseline(t, dsn, "--include-system", "--schema", "pg_catalog")
	if objectCount(t, system) == 0 {
		t.Fatal("--include-system captured no catalog")
	}

	// A table in the scope of the empty baseline is new; one outside it is
	// not seen.
	for _, stmt := range []string{
		"CREATE TABLE app.t (id int)",
		"CREATE TABLE public.other (id int)",
	} {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}
	code, summary, changes := verifyJSON(t, dsn, scoped)
	if code != 1 || summary.New != 1 || summary.Total() != 1 ||
		changes[0].Schema != "app" || changes[0].Name != "t" {
		t.Fatalf("scoped: code=%d summary=%+v changes=%+v", code, summary, changes)
	}
	code, summary, _ = verifyJSON(t, dsn, whole)
	if code != 1 || summary.New != 2 {
		t.Fatalf("whole: code=%d summary=%+v", code, summary)
	}
	if code, summary, _ := verifyJSON(t, dsn, nomatch); code != 0 || summary.Total() != 0 {
		t.Fatalf("no match: code=%d summary=%+v", code, summary)
	}
}
