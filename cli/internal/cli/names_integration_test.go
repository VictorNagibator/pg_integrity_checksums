package cli

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

// TestIntegrationDottedNames covers schema and table names that contain a
// dot: "x.y"."t" and "x"."y.t" read the same when joined with a dot, which
// made capture write a baseline that verify rejected as having a duplicate
// object, and compare match each table with the other one.
func TestIntegrationDottedNames(t *testing.T) {
	dsn := os.Getenv("PGICS_TEST_DSN")
	if dsn == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	ctx := context.Background()
	db, err := pgdb.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()

	x := fmt.Sprintf("pgics_dot_%d", time.Now().UnixNano())
	outer := `"` + x + `"`
	dotted := `"` + x + `.y"`
	for _, q := range []string{
		"CREATE SCHEMA " + outer,
		"CREATE SCHEMA " + dotted,
		"CREATE TABLE " + outer + `."y.t" (v int)`,
		"CREATE TABLE " + dotted + `."t" (v int)`,
		"INSERT INTO " + outer + `."y.t" VALUES (1)`,
		"INSERT INTO " + dotted + `."t" VALUES (2)`,
	} {
		if _, err := db.ExecContext(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	defer db.ExecContext(ctx, "DROP SCHEMA "+outer+" CASCADE")
	defer db.ExecContext(ctx, "DROP SCHEMA "+dotted+" CASCADE")

	path := filepath.Join(t.TempDir(), "baseline.json")
	code, _, errOut := runCLI("capture", "--db", dsn, "--schema", x+"*", "--output", path, "--quiet")
	if code != 0 {
		t.Fatalf("capture: code=%d err=%s", code, errOut)
	}
	b, err := baseline.Load(path)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if len(b.Objects) != 2 {
		t.Fatalf("want 2 objects, got %+v", b.Objects)
	}

	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 0 {
		t.Fatalf("verify clean: code=%d err=%s", code, errOut)
	}

	if _, err := db.ExecContext(ctx, "UPDATE "+dotted+`."t" SET v = 3`); err != nil {
		t.Fatal(err)
	}
	code, out, errOut := runCLI("verify", "--db", dsn, "--baseline", path, "--format", "json", "--quiet")
	if code != 1 {
		t.Fatalf("verify changed: code=%d out=%s err=%s", code, out, errOut)
	}
	var report struct {
		Summary baseline.Summary `json:"summary"`
	}
	if err := json.Unmarshal([]byte(out), &report); err != nil {
		t.Fatal(err)
	}
	if report.Summary.Changed != 1 || report.Summary.OK != 1 {
		t.Fatalf("summary: %+v\n%s", report.Summary, out)
	}

	// A database compared with itself has no differences.
	code, out, errOut = runCLI("compare", "--source", dsn, "--target", dsn, "--schema", x+"*", "--quiet")
	if code != 0 {
		t.Fatalf("compare: code=%d out=%s err=%s", code, out, errOut)
	}
}
