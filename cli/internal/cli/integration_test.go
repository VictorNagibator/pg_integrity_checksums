package cli

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

func TestIntegrationCaptureVerifyUpdate(t *testing.T) {
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

	schema := fmt.Sprintf("pgics_it_%d", time.Now().UnixNano())
	if _, err := db.ExecContext(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer db.ExecContext(ctx, "DROP SCHEMA "+schema+" CASCADE")

	_, err = db.ExecContext(ctx, "CREATE TABLE "+schema+".accounts (id int PRIMARY KEY, name text, amount numeric(12,2))")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.ExecContext(ctx, "INSERT INTO "+schema+".accounts VALUES (1,'a',1.00),(2,'b',2.00)"); err != nil {
		t.Fatal(err)
	}
	if _, err := db.ExecContext(ctx, "CREATE INDEX accounts_name_idx ON "+schema+".accounts(name)"); err != nil {
		t.Fatal(err)
	}

	path := filepath.Join(t.TempDir(), "baseline.json")
	code, _, errOut := runCLI("capture", "--db", dsn, "--schema", schema, "--output", path, "--quiet")
	if code != 0 {
		t.Fatalf("capture: code=%d err=%s", code, errOut)
	}

	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 0 {
		t.Fatalf("verify clean: code=%d err=%s", code, errOut)
	}

	if _, err := db.ExecContext(ctx, "UPDATE "+schema+".accounts SET amount = amount + 1 WHERE id = 1"); err != nil {
		t.Fatal(err)
	}
	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 1 {
		t.Fatalf("verify changed: code=%d err=%s", code, errOut)
	}

	code, _, errOut = runCLI("update", "--db", dsn, "--baseline", path, "--yes", "--quiet")
	if code != 0 {
		t.Fatalf("update: code=%d err=%s", code, errOut)
	}
	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 0 {
		t.Fatalf("verify after update: code=%d err=%s", code, errOut)
	}

	physicalPath := filepath.Join(t.TempDir(), "physical.json")
	code, _, errOut = runCLI("capture", "--db", dsn, "--schema", schema, "--kind", "physical", "--output", physicalPath, "--quiet")
	if code != 0 {
		t.Fatalf("physical capture: code=%d err=%s", code, errOut)
	}
	code, _, errOut = runCLI("verify", "--db", dsn, "--baseline", physicalPath, "--quiet")
	if code != 0 {
		t.Fatalf("physical verify: code=%d err=%s", code, errOut)
	}
}
