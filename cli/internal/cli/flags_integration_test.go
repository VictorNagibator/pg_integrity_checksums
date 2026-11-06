package cli

import (
	"bytes"
	"compress/gzip"
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

func setupCLITestSchema(t *testing.T) (*sql.DB, string) {
	t.Helper()
	dsn := os.Getenv("PGICS_TEST_DSN")
	if dsn == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	ctx := context.Background()
	db, err := pgdb.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	schema := fmt.Sprintf("pgics_flags_%d", time.Now().UnixNano())
	if _, err := db.ExecContext(ctx, "CREATE SCHEMA "+schema); err != nil {
		db.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_, _ = db.ExecContext(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
		db.Close()
	})
	statements := []string{
		"CREATE TABLE " + schema + ".accounts (id int PRIMARY KEY, name text NOT NULL, amount numeric(12,2))",
		"INSERT INTO " + schema + ".accounts VALUES (1,'a',1.00),(2,'b',2.00)",
		"CREATE INDEX accounts_name_idx ON " + schema + ".accounts(name)",
		"CREATE TABLE " + schema + ".audit (id int PRIMARY KEY, note text)",
		"INSERT INTO " + schema + ".audit VALUES (1,'x')",
	}
	for _, stmt := range statements {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}
	return db, schema
}

func capturePath(t *testing.T, schema, path string, extra ...string) {
	t.Helper()
	args := []string{"capture", "--db", os.Getenv("PGICS_TEST_DSN"), "--schema", schema, "--output", path, "--quiet"}
	args = append(args, extra...)
	code, _, errOut := runCLI(args...)
	if code != 0 {
		t.Fatalf("capture: code=%d err=%s", code, errOut)
	}
}

func loadBaselineForTest(t *testing.T, path string) *baseline.Baseline {
	t.Helper()
	b, err := baseline.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func objectNames(b *baseline.Baseline) []string {
	names := make([]string, 0, len(b.Objects))
	for _, o := range b.Objects {
		names = append(names, o.Kind+":"+o.Name)
	}
	return names
}

func TestIntegrationFiltersAndStrict(t *testing.T) {
	_, schema := setupCLITestSchema(t)
	dir := t.TempDir()

	allPath := filepath.Join(dir, "all.json")
	capturePath(t, schema, allPath)
	all := loadBaselineForTest(t, allPath)
	if len(all.Objects) != 5 {
		t.Fatalf("expected 5 objects, got %v", objectNames(all))
	}

	noAccountsPath := filepath.Join(dir, "no-accounts.json")
	capturePath(t, schema, noAccountsPath, "--exclude-table", schema+".accounts")
	noAccounts := loadBaselineForTest(t, noAccountsPath)
	for _, o := range noAccounts.Objects {
		if o.Name == "accounts" || o.TableName == "accounts" {
			t.Fatalf("excluded table still present: %#v", o)
		}
	}

	onlyAccountsPath := filepath.Join(dir, "only-accounts.json")
	capturePath(t, schema, onlyAccountsPath, "--table", schema+".accounts")
	onlyAccounts := loadBaselineForTest(t, onlyAccountsPath)
	if len(onlyAccounts.Objects) != 3 {
		t.Fatalf("expected table and its two indexes, got %v", objectNames(onlyAccounts))
	}

	noIndexesPath := filepath.Join(dir, "no-indexes.json")
	capturePath(t, schema, noIndexesPath, "--table", schema+".accounts", "--no-indexes")
	noIndexes := loadBaselineForTest(t, noIndexesPath)
	if len(noIndexes.Objects) != 1 || noIndexes.Objects[0].Kind != baseline.ObjectTable {
		t.Fatalf("expected one table, got %v", objectNames(noIndexes))
	}

	excludedSchemaPath := filepath.Join(dir, "excluded-schema.json")
	capturePath(t, schema, excludedSchemaPath, "--exclude-schema", schema)
	excludedSchema := loadBaselineForTest(t, excludedSchemaPath)
	if len(excludedSchema.Objects) != 0 {
		t.Fatalf("exclude-schema should produce no objects, got %v", objectNames(excludedSchema))
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", excludedSchemaPath, "--quiet"); code != 0 {
		t.Fatalf("exclude-schema verify: code=%d err=%s", code, errOut)
	}

	strictPath := filepath.Join(dir, "strict.json")
	capturePath(t, schema, strictPath, "--strict", "--workers", "2", "--timeout", "30s")
	if code, out, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", strictPath, "--format", "json", "--quiet"); code != 0 || !strings.Contains(out, "\"summary\"") {
		t.Fatalf("strict/json verify: code=%d out=%q err=%s", code, out, errOut)
	}
	if code, out, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", strictPath, "--show-ok", "--quiet"); code != 0 || !strings.Contains(out, "OK") {
		t.Fatalf("show-ok verify: code=%d out=%q err=%s", code, out, errOut)
	}

	systemPath := filepath.Join(dir, "system.json")
	code, _, errOut := runCLI("capture", "--db", os.Getenv("PGICS_TEST_DSN"), "--include-system",
		"--table", "pg_catalog.pg_class", "--no-indexes", "--output", systemPath, "--quiet")
	if code != 0 {
		t.Fatalf("system capture: code=%d err=%s", code, errOut)
	}
	system := loadBaselineForTest(t, systemPath)
	if len(system.Objects) != 1 || system.Objects[0].Schema != "pg_catalog" {
		t.Fatalf("expected pg_catalog.pg_class, got %v", objectNames(system))
	}
}

func TestIntegrationSchemaGzipVersionAndPhysical(t *testing.T) {
	db, schema := setupCLITestSchema(t)
	ctx := context.Background()
	dir := t.TempDir()
	path := filepath.Join(dir, "baseline.json")
	capturePath(t, schema, path)

	if _, err := db.ExecContext(ctx, "ALTER TABLE "+schema+".accounts ADD COLUMN note text"); err != nil {
		t.Fatal(err)
	}
	code, out, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--quiet")
	if code != 1 || !strings.Contains(out, "SCHEMA_CHANGED") {
		t.Fatalf("schema verify: code=%d out=%q err=%q", code, out, errOut)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--fail-on", "none", "--quiet"); code != 0 {
		t.Fatalf("fail-on none: code=%d", code)
	}
	if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--yes", "--quiet"); code != 0 {
		t.Fatalf("update schema: code=%d err=%s", code, errOut)
	}

	// gzip is accepted by Load; verify and show must both work.
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	gzPath := filepath.Join(dir, "baseline.json.gz")
	gzFile, err := os.Create(gzPath)
	if err != nil {
		t.Fatal(err)
	}
	gzWriter := gzip.NewWriter(gzFile)
	if _, err := gzWriter.Write(raw); err != nil {
		t.Fatal(err)
	}
	if err := gzWriter.Close(); err != nil {
		t.Fatal(err)
	}
	if err := gzFile.Close(); err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("show", gzPath); code != 0 {
		t.Fatalf("show gzip: code=%d err=%s", code, errOut)
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", gzPath, "--quiet"); code != 0 {
		t.Fatalf("verify gzip: code=%d err=%s", code, errOut)
	}

	// Allow a different PostgreSQL major while keeping the same checksums.
	versionPath := filepath.Join(dir, "version.json")
	versionBaseline := loadBaselineForTest(t, path)
	versionBaseline.PostgresVersion = versionBaseline.PostgresVersion + 10000
	if err := baseline.Save(versionPath, versionBaseline, os.Stdout); err != nil {
		t.Fatal(err)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", versionPath, "--quiet"); code != 3 {
		t.Fatalf("version mismatch should be 3, got %d", code)
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", versionPath,
		"--allow-version-mismatch", "--quiet"); code != 0 {
		t.Fatalf("allow version: code=%d err=%s", code, errOut)
	}

	// changed-since and dry-run after the schema update.
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--dry-run", "--quiet"); code != 0 {
		t.Fatalf("dry run: code=%d err=%s", code, errOut)
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Fatal("dry-run modified baseline")
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--changed-since", "--quiet"); code != 0 {
		t.Fatalf("changed-since clean: code=%d err=%s", code, errOut)
	}
	if _, err := db.ExecContext(ctx, "UPDATE "+schema+".accounts SET amount = amount + 1 WHERE id = 1"); err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--changed-since", "--quiet"); code != 1 {
		t.Fatalf("changed-since changed: code=%d err=%s", code, errOut)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--fail-on", "changed", "--quiet"); code != 1 {
		t.Fatalf("fail-on changed: code=%d", code)
	}
	if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--yes", "--quiet"); code != 0 {
		t.Fatalf("update data: code=%d err=%s", code, errOut)
	}

	physicalPath := filepath.Join(dir, "physical.json")
	capturePath(t, schema, physicalPath, "--kind", "physical")
	physical := loadBaselineForTest(t, physicalPath)
	physical.SystemIdentifier = "1"
	if err := baseline.Save(physicalPath, physical, os.Stdout); err != nil {
		t.Fatal(err)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--quiet"); code != 3 {
		t.Fatalf("wrong physical cluster should be 3, got %d", code)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--changed-since", "--quiet"); code != 2 {
		t.Fatalf("physical changed-since should be 2, got %d", code)
	}
}
