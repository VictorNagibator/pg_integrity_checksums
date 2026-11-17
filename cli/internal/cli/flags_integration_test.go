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

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/stdlib"

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
	// The wrong cluster is refused before any relation is checksummed: no
	// "checking ..." progress line may appear.
	code, _, errOut = runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath)
	if code != 3 {
		t.Fatalf("wrong physical cluster should be 3, got %d: %s", code, errOut)
	}
	if !strings.Contains(errOut, "incompatible baseline") || strings.Contains(errOut, "checking ") {
		t.Fatalf("wrong physical cluster should be refused before the scan: %s", errOut)
	}
	if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--yes"); code != 3 ||
		strings.Contains(errOut, "checking ") {
		t.Fatalf("update with wrong physical cluster should be 3 before the scan, got %d: %s", code, errOut)
	}
	if code, _, _ := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--changed-since", "--quiet"); code != 2 {
		t.Fatalf("physical changed-since should be 2, got %d", code)
	}
}

// TestIntegrationFNV64Baseline covers the v2 baseline format and the
// generic fnv64 extension API. It requires an extension build that includes
// the version 2.2 generic overloads.
func TestIntegrationFNV64Baseline(t *testing.T) {
	if os.Getenv("PGICS_TEST_DSN") == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	db, schema := setupCLITestSchema(t)
	ctx := context.Background()
	dir := t.TempDir()

	v2Path := filepath.Join(dir, "baseline-v2.json")
	capturePath(t, schema, v2Path, "--hash", "fnv64")
	v2 := loadBaselineForTest(t, v2Path)
	if v2.Format != baseline.FormatV2 || v2.Hash != baseline.HashFNV64 {
		t.Fatalf("bad v2 metadata: format=%q hash=%q", v2.Format, v2.Hash)
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", v2Path, "--quiet"); code != 0 {
		t.Fatalf("v2 verify: code=%d err=%s", code, errOut)
	}

	v1Path := filepath.Join(dir, "baseline-v1.json")
	capturePath(t, schema, v1Path)
	if code, _, errOut := runCLI("diff", v1Path, v2Path); code != 3 {
		t.Fatalf("v1/v2 diff: code=%d err=%s", code, errOut)
	}

	if _, err := db.ExecContext(ctx, "UPDATE "+schema+".accounts SET amount = amount + 1 WHERE id = 1"); err != nil {
		t.Fatal(err)
	}
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", v2Path, "--quiet"); code != 1 {
		t.Fatalf("v2 changed verify: code=%d err=%s", code, errOut)
	}
	if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", v2Path, "--yes", "--quiet"); code != 0 {
		t.Fatalf("v2 update: code=%d err=%s", code, errOut)
	}
	v2After := loadBaselineForTest(t, v2Path)
	if v2After.Format != baseline.FormatV2 || v2After.Hash != baseline.HashFNV64 {
		t.Fatalf("update lost v2 metadata: format=%q hash=%q", v2After.Format, v2After.Hash)
	}

	physicalPath := filepath.Join(dir, "physical-v2.json")
	capturePath(t, schema, physicalPath, "--hash", "fnv64", "--kind", "physical")
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--quiet"); code != 0 {
		t.Fatalf("physical v2 verify: code=%d err=%s", code, errOut)
	}
}

// TestIntegrationCRC32CAndXXH3Baselines captures, verifies and updates v2
// baselines with the crc32c and xxh3 algorithms (extension 2.9.0).
func TestIntegrationCRC32CAndXXH3Baselines(t *testing.T) {
	if os.Getenv("PGICS_TEST_DSN") == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	db, schema := setupCLITestSchema(t)
	ctx := context.Background()
	dir := t.TempDir()

	paths := map[string]string{}
	for _, hash := range []string{baseline.HashCRC32C, baseline.HashXXH3} {
		path := filepath.Join(dir, hash+".json")
		capturePath(t, schema, path, "--hash", hash)
		b := loadBaselineForTest(t, path)
		if b.Format != baseline.FormatV2 || b.Hash != hash {
			t.Fatalf("%s: bad metadata: format=%q hash=%q", hash, b.Format, b.Hash)
		}
		if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--quiet"); code != 0 {
			t.Fatalf("%s verify: code=%d err=%s", hash, code, errOut)
		}
		paths[hash] = path
	}
	// Baselines of different algorithms cannot be compared.
	if code, _, errOut := runCLI("diff", paths[baseline.HashCRC32C], paths[baseline.HashXXH3]); code != 3 {
		t.Fatalf("crc32c/xxh3 diff: code=%d err=%s", code, errOut)
	}

	if _, err := db.ExecContext(ctx, "UPDATE "+schema+".accounts SET amount = amount + 1 WHERE id = 1"); err != nil {
		t.Fatal(err)
	}
	for hash, path := range paths {
		if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--quiet"); code != 1 {
			t.Fatalf("%s changed verify: code=%d err=%s", hash, code, errOut)
		}
		if code, _, errOut := runCLI("update", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", path, "--yes", "--quiet"); code != 0 {
			t.Fatalf("%s update: code=%d err=%s", hash, code, errOut)
		}
		if after := loadBaselineForTest(t, path); after.Hash != hash {
			t.Fatalf("%s: update changed the hash to %q", hash, after.Hash)
		}
	}

	physicalPath := filepath.Join(dir, "physical-xxh3.json")
	capturePath(t, schema, physicalPath, "--hash", "xxh3", "--kind", "physical")
	if code, _, errOut := runCLI("verify", "--db", os.Getenv("PGICS_TEST_DSN"), "--baseline", physicalPath, "--quiet"); code != 0 {
		t.Fatalf("physical xxh3 verify: code=%d err=%s", code, errOut)
	}
}

// TestIntegrationNewHashOldExtension checks that capture with crc32c or xxh3
// fails with a clear message on an extension older than 2.9.0.
func TestIntegrationNewHashOldExtension(t *testing.T) {
	dsn := os.Getenv("PGICS_TEST_DSN")
	if dsn == "" {
		t.Skip("PGICS_TEST_DSN is not set")
	}
	ctx := context.Background()
	admin, err := pgdb.Open(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close()

	dbname := fmt.Sprintf("pgics_oldhash_%d", time.Now().UnixNano())
	if _, err := admin.ExecContext(ctx, "CREATE DATABASE "+dbname); err != nil {
		t.Skipf("cannot create a second database: %v", err)
	}
	defer admin.ExecContext(ctx, "DROP DATABASE "+dbname)

	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Database = dbname
	oldDSN := stdlib.RegisterConnConfig(cfg)
	defer stdlib.UnregisterConnConfig(oldDSN)
	old, err := pgdb.Open(ctx, oldDSN)
	if err != nil {
		t.Fatal(err)
	}
	defer old.Close()
	for _, stmt := range []string{
		"CREATE EXTENSION pg_integrity_checksums VERSION '2.8.0'",
		"CREATE TABLE t (id int PRIMARY KEY)",
	} {
		if _, err := old.ExecContext(ctx, stmt); err != nil {
			t.Skipf("cannot set up extension 2.8.0: %v", err)
		}
	}

	for _, hash := range []string{baseline.HashCRC32C, baseline.HashXXH3} {
		code, _, errOut := runCLI("capture", "--db", oldDSN, "--hash", hash,
			"--output", filepath.Join(t.TempDir(), hash+".json"), "--quiet")
		if code == 0 || !strings.Contains(errOut, "needs extension pg_integrity_checksums 2.9.0 or later") ||
			!strings.Contains(errOut, "2.8.0") {
			t.Fatalf("%s capture on 2.8.0: code=%d err=%s", hash, code, errOut)
		}
	}
	// fnv64 still works there.
	if code, _, errOut := runCLI("capture", "--db", oldDSN, "--hash", "fnv64",
		"--output", filepath.Join(t.TempDir(), "fnv64.json"), "--quiet"); code != 0 {
		t.Fatalf("fnv64 capture on 2.8.0: code=%d err=%s", code, errOut)
	}
}

// TestIntegrationSchemaHashPortable checks that schema_hash does not depend on
// the order in which constraints were created (a restore creates them in
// another order than the original DDL) or on the session's search_path and
// quote_all_identifiers, which change how names are spelled.
func TestIntegrationSchemaHashPortable(t *testing.T) {
	db, schema := setupCLITestSchema(t)
	ctx := context.Background()
	statements := []string{
		"CREATE TYPE " + schema + ".mood AS ENUM ('ok', 'meh')",
		"CREATE TABLE " + schema + ".parent (id int PRIMARY KEY)",
		"INSERT INTO " + schema + ".parent VALUES (1)",
		// Constraint names deliberately not in creation order.
		"CREATE TABLE " + schema + ".child (id int, pid int, m " + schema + ".mood DEFAULT 'ok', " +
			"CONSTRAINT zz_pk PRIMARY KEY (id))",
		"ALTER TABLE " + schema + ".child ADD CONSTRAINT mm_fk FOREIGN KEY (pid) REFERENCES " + schema + ".parent (id)",
		"ALTER TABLE " + schema + ".child ADD CONSTRAINT aa_check CHECK (id > 0)",
		"INSERT INTO " + schema + ".child VALUES (1, 1, 'meh')",
		"CREATE INDEX child_expr_idx ON " + schema + ".child ((id * 2)) WHERE pid > 0",
	}
	for _, stmt := range statements {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}

	// Capture with the schema on the search_path and quote_all_identifiers
	// on: both used to change how names were spelled in the hashed text.
	dsn := os.Getenv("PGICS_TEST_DSN")
	custom := withRuntimeParams(t, dsn, "search_path="+schema+",public", "quote_all_identifiers=on")
	path := filepath.Join(t.TempDir(), "baseline.json")
	code, _, errOut := runCLI("capture", "--db", custom, "--schema", schema, "--output", path, "--quiet")
	if code != 0 {
		t.Fatalf("capture: code=%d err=%s", code, errOut)
	}
	if b := loadBaselineForTest(t, path); b.SchemaHashVersion != baseline.SchemaHashVersion {
		t.Fatalf("schema_hash_version %d, want %d", b.SchemaHashVersion, baseline.SchemaHashVersion)
	}

	// Recreate the constraints in another order, as a restore would.
	recreate := []string{
		"ALTER TABLE " + schema + ".child DROP CONSTRAINT aa_check",
		"ALTER TABLE " + schema + ".child DROP CONSTRAINT mm_fk",
		"ALTER TABLE " + schema + ".child DROP CONSTRAINT zz_pk",
		"ALTER TABLE " + schema + ".child ADD CONSTRAINT aa_check CHECK (id > 0)",
		"ALTER TABLE " + schema + ".child ADD CONSTRAINT mm_fk FOREIGN KEY (pid) REFERENCES " + schema + ".parent (id)",
		"ALTER TABLE " + schema + ".child ADD CONSTRAINT zz_pk PRIMARY KEY (id)",
	}
	for _, stmt := range recreate {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}

	// Verify with the default settings.
	code, out, errOut := runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 0 {
		t.Fatalf("verify: code=%d out=%s err=%s", code, out, errOut)
	}

	// A real schema change is still detected.
	if _, err := db.ExecContext(ctx, "ALTER TABLE "+schema+".child DROP CONSTRAINT aa_check"); err != nil {
		t.Fatal(err)
	}
	code, out, _ = runCLI("verify", "--db", dsn, "--baseline", path, "--quiet")
	if code != 1 || !strings.Contains(out, "schema_changed") {
		t.Fatalf("schema change not detected: code=%d out=%s", code, out)
	}
}

// TestIntegrationOldSchemaHashVersion checks that a baseline whose schema
// hashes were computed by an older CLI is verified without false schema
// changes, with a warning.
func TestIntegrationOldSchemaHashVersion(t *testing.T) {
	_, schema := setupCLITestSchema(t)
	dsn := os.Getenv("PGICS_TEST_DSN")
	path := filepath.Join(t.TempDir(), "baseline.json")
	capturePath(t, schema, path)

	b := loadBaselineForTest(t, path)
	b.SchemaHashVersion = 0
	for i := range b.Objects {
		b.Objects[i].SchemaHash = "sha256:" + strings.Repeat("0", 64)
	}
	if err := baseline.Save(path, b, nil); err != nil {
		t.Fatal(err)
	}

	code, out, errOut := runCLI("verify", "--db", dsn, "--baseline", path)
	if code != 0 {
		t.Fatalf("verify: code=%d out=%s err=%s", code, out, errOut)
	}
	if !strings.Contains(errOut, "schema_hash versions") {
		t.Fatalf("missing schema_hash version warning: %s", errOut)
	}
}

// withRuntimeParams appends server settings to a key/value or URL DSN.
func withRuntimeParams(t *testing.T, dsn string, params ...string) string {
	t.Helper()
	if strings.HasPrefix(dsn, "postgres://") || strings.HasPrefix(dsn, "postgresql://") {
		sep := "?"
		if strings.Contains(dsn, "?") {
			sep = "&"
		}
		return dsn + sep + strings.Join(params, "&")
	}
	return dsn + " " + strings.Join(params, " ")
}
