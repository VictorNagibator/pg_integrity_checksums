// Package pgdb connects the CLI to PostgreSQL and computes baselines using
// the SQL functions provided by the pg_integrity_checksums extension.
package pgdb

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

const (
	extensionName = "pg_integrity_checksums"
	workersGUC    = "pg_integrity_checksums.workers"
)

// Options controls one snapshot. ToolVersion is copied into the baseline;
// Workers and Timeout map to the extension GUC and statement_timeout.
type Options struct {
	ToolVersion string
	Kind        string
	Scope       baseline.Scope
	Workers     int
	Timeout     time.Duration
	Strict      bool
	Progress    func(string)
}

// Candidate is a relation discovered in pg_class before its checksum is
// computed. OID is used only inside the transaction and never serialized.
type Candidate struct {
	OID         int64
	Kind        string
	Schema      string
	Name        string
	TableSchema string
	TableName   string
}

// Info is the small amount of server metadata shown by doctor.
type Info struct {
	ExtensionVersion string
	PostgresVersion  int
	Database         string
	CurrentUser      string
}

// Open creates a single-connection pool. One connection is intentional:
// capture and verify must run in one repeatable-read transaction and must not
// read different tables from different snapshots. If dsn is empty,
// DATABASE_URL and then the normal PG* variables are used.
func Open(ctx context.Context, dsn string) (*sql.DB, error) {
	if dsn == "" {
		dsn = strings.TrimSpace(os.Getenv("DATABASE_URL"))
	}
	db, err := sql.Open("pgx", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	pingCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	if err := db.PingContext(pingCtx); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

// Snapshot discovers objects and computes their checksums in one
// repeatable-read, read-only transaction. It returns a fully populated
// baseline; the caller decides whether and where to save it.
func Snapshot(ctx context.Context, db *sql.DB, opts Options) (*baseline.Baseline, error) {
	if opts.Kind == "" {
		opts.Kind = baseline.KindLogical
	}
	if opts.Kind != baseline.KindLogical && opts.Kind != baseline.KindPhysical {
		return nil, fmt.Errorf("invalid kind %q", opts.Kind)
	}

	// READ ONLY is a safety net: capture must never touch application data.
	tx, err := db.BeginTx(ctx, &sql.TxOptions{
		Isolation: sql.LevelRepeatableRead,
		ReadOnly:  true,
	})
	if err != nil {
		return nil, fmt.Errorf("begin repeatable read: %w", err)
	}
	defer tx.Rollback()

	if opts.Timeout > 0 {
		// PostgreSQL accepts "500ms" but not Go's "5m0s" format, so convert
		// to a plain millisecond value.
		timeoutMS := opts.Timeout.Milliseconds()
		if timeoutMS < 1 {
			timeoutMS = 1
		}
		if _, err := tx.ExecContext(ctx,
			"SELECT set_config('statement_timeout', $1, true)",
			strconv.FormatInt(timeoutMS, 10)+"ms"); err != nil {
			return nil, fmt.Errorf("set statement_timeout: %w", err)
		}
	}
	if opts.Workers > 0 {
		// The extension's parallel workers share this transaction snapshot,
		// so setting the GUC here cannot mix snapshots.
		if _, err := tx.ExecContext(ctx,
			"SELECT set_config($1, $2, true)",
			workersGUC, strconv.Itoa(opts.Workers)); err != nil {
			return nil, fmt.Errorf("set %s: %w", workersGUC, err)
		}
	}

	extVersion, postgresVersion, database, err := metadata(ctx, tx)
	if err != nil {
		return nil, err
	}

	var systemIdentifier string
	var databaseOID int64
	if opts.Kind == baseline.KindPhysical {
		// Physical checksums are meaningless on another cluster or database.
		// Record the cluster identity and database OID so verify can refuse
		// a baseline from the wrong place instead of reporting a false diff.
		if err := tx.QueryRowContext(ctx,
			"SELECT system_identifier::text FROM pg_control_system()").
			Scan(&systemIdentifier); err != nil {
			return nil, fmt.Errorf("read system_identifier: %w", err)
		}
		if err := tx.QueryRowContext(ctx,
			"SELECT oid::bigint FROM pg_database WHERE datname = current_database()").
			Scan(&databaseOID); err != nil {
			return nil, fmt.Errorf("read database oid: %w", err)
		}
	}

	candidates, err := Discover(ctx, tx, opts.Scope)
	if err != nil {
		return nil, err
	}

	objects := make([]baseline.Object, 0, len(candidates))
	for _, c := range candidates {
		if opts.Progress != nil {
			opts.Progress(fmt.Sprintf("%s %s.%s", c.Kind, c.Schema, c.Name))
		}
		checksum, reason, err := checksumObject(ctx, tx, c, opts.Kind)
		if err != nil {
			return nil, fmt.Errorf("%s %s.%s: %w", c.Kind, c.Schema, c.Name, err)
		}
		status := baseline.StatusOK
		if reason != "" {
			// NULL normally means the extension does not support this
			// object/kind (for example a physical checksum of an unusual
			// index AM). In strict mode that is a hard failure; otherwise
			// the object is recorded as skipped so the baseline is explicit.
			if opts.Strict {
				return nil, fmt.Errorf("%s %s.%s cannot be checksummed: %s",
					c.Kind, c.Schema, c.Name, reason)
			}
			status = baseline.StatusSkipped
		}
		objects = append(objects, baseline.Object{
			Kind:        c.Kind,
			Schema:      c.Schema,
			Name:        c.Name,
			TableSchema: c.TableSchema,
			TableName:   c.TableName,
			Status:      status,
			Checksum:    checksum,
			Reason:      reason,
		})
	}

	b := baseline.New(
		opts.ToolVersion,
		opts.Kind,
		extVersion,
		postgresVersion,
		database,
		opts.Scope,
		objects,
	)
	b.SystemIdentifier = systemIdentifier
	b.DatabaseOID = databaseOID
	b.Finalize()

	// Commit only after every checksum succeeded; on any earlier error the
	// deferred Rollback releases the read-only transaction.
	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("commit snapshot: %w", err)
	}
	return b, nil
}

// metadata reads the extension and server identity that will be recorded in
// every baseline. A missing extension is a user-facing error, not a fallback.
func metadata(ctx context.Context, tx *sql.Tx) (string, int, string, error) {
	var extVersion string
	if err := tx.QueryRowContext(ctx,
		"SELECT extversion FROM pg_extension WHERE extname = $1",
		extensionName).Scan(&extVersion); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return "", 0, "", fmt.Errorf("extension %q is not installed", extensionName)
		}
		return "", 0, "", fmt.Errorf("read extension version: %w", err)
	}

	var pgVersion int
	if err := tx.QueryRowContext(ctx,
		"SELECT current_setting('server_version_num')::int").Scan(&pgVersion); err != nil {
		return "", 0, "", fmt.Errorf("read PostgreSQL version: %w", err)
	}

	var database string
	if err := tx.QueryRowContext(ctx,
		"SELECT current_database()").Scan(&database); err != nil {
		return "", 0, "", fmt.Errorf("read database name: %w", err)
	}
	return extVersion, pgVersion, database, nil
}

// ReadInfo returns server and extension metadata without discovering or
// checksumming any relations. doctor uses it for a quick environment check.
func ReadInfo(ctx context.Context, db *sql.DB) (Info, error) {
	var i Info
	if err := db.QueryRowContext(ctx,
		"SELECT extversion FROM pg_extension WHERE extname = $1",
		extensionName).Scan(&i.ExtensionVersion); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return i, fmt.Errorf("extension %q is not installed", extensionName)
		}
		return i, err
	}
	if err := db.QueryRowContext(ctx,
		"SELECT current_setting('server_version_num')::int").Scan(&i.PostgresVersion); err != nil {
		return i, err
	}
	if err := db.QueryRowContext(ctx,
		"SELECT current_database()").Scan(&i.Database); err != nil {
		return i, err
	}
	if err := db.QueryRowContext(ctx,
		"SELECT current_user").Scan(&i.CurrentUser); err != nil {
		return i, err
	}
	return i, nil
}

// Discover enumerates top-level tables and indexes in pg_class. It does not
// checksum anything, which keeps doctor cheap and lets capture apply the scope
// filters before any expensive work starts.
//
// Partition leaves are deliberately excluded: the extension aggregates them
// through the partitioned parent, so including both would count the same data
// twice.
func Discover(ctx context.Context, q Querier, scope baseline.Scope) ([]Candidate, error) {
	query := "SELECT c.oid::bigint, n.nspname, c.relname, c.relkind, " +
		"COALESCE(tn.nspname, ''), COALESCE(tc.relname, '') " +
		"FROM pg_class c " +
		"JOIN pg_namespace n ON n.oid = c.relnamespace " +
		"LEFT JOIN pg_index ix ON ix.indexrelid = c.oid " +
		"LEFT JOIN pg_class tc ON tc.oid = ix.indrelid " +
		"LEFT JOIN pg_namespace tn ON tn.oid = tc.relnamespace " +
		"WHERE c.relkind IN ('r', 'p', 'm', 'i', 'I') " +
		"AND NOT c.relispartition " +
		"ORDER BY n.nspname, c.relname"
	rows, err := q.QueryContext(ctx, query)
	if err != nil {
		return nil, fmt.Errorf("discover relations: %w", err)
	}
	defer rows.Close()

	out := make([]Candidate, 0)
	for rows.Next() {
		var c Candidate
		var relkind string
		if err := rows.Scan(&c.OID, &c.Schema, &c.Name, &relkind,
			&c.TableSchema, &c.TableName); err != nil {
			return nil, err
		}
		// r = ordinary table, p = partitioned table, m = materialized view;
		// i/I = ordinary/partitioned index.
		switch relkind {
		case "r", "p", "m":
			c.Kind = baseline.ObjectTable
		case "i", "I":
			c.Kind = baseline.ObjectIndex
		default:
			continue
		}
		if matchesScope(c, scope) {
			out = append(out, c)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Kind != out[j].Kind {
			return out[i].Kind < out[j].Kind
		}
		if out[i].Schema != out[j].Schema {
			return out[i].Schema < out[j].Schema
		}
		return out[i].Name < out[j].Name
	})
	return out, nil
}

// Querier is implemented by both *sql.DB and *sql.Tx. Discover must work
// inside the snapshot transaction and from doctor's plain connection.
type Querier interface {
	QueryContext(ctx context.Context, query string, args ...any) (*sql.Rows, error)
}

// matchesScope applies the stored baseline scope in Go rather than embedding
// dynamic SQL filters. Table include/exclude filters apply to an index's owning
// table, so --table app.orders also captures its indexes.
func matchesScope(c Candidate, scope baseline.Scope) bool {
	if c.Kind == baseline.ObjectIndex && !scope.IncludeIndexes {
		return false
	}
	if isSystemSchema(c.Schema) {
		if !scope.IncludeSystem || isAlwaysExcludedSchema(c.Schema) {
			return false
		}
	}
	if len(scope.Schemas) > 0 && !contains(scope.Schemas, c.Schema) {
		return false
	}
	if contains(scope.ExcludeSchemas, c.Schema) {
		return false
	}

	tableIdentity := c.TableSchema + "." + c.TableName
	if c.Kind == baseline.ObjectTable {
		tableIdentity = c.Schema + "." + c.Name
	}
	if len(scope.Tables) > 0 && !contains(scope.Tables, tableIdentity) {
		return false
	}
	if contains(scope.ExcludeTables, tableIdentity) {
		return false
	}
	return true
}

// isSystemSchema identifies namespaces that are excluded by default.
// IncludeSystem can opt back into pg_catalog and information_schema.
func isSystemSchema(s string) bool {
	return s == "pg_catalog" ||
		s == "information_schema" ||
		s == "pg_toast" ||
		strings.HasPrefix(s, "pg_temp_") ||
		strings.HasPrefix(s, "pg_toast_temp_")
}

// isAlwaysExcludedSchema covers TOAST and temporary namespaces. They are not
// meaningful targets even with --include-system.
func isAlwaysExcludedSchema(s string) bool {
	return s == "pg_toast" ||
		strings.HasPrefix(s, "pg_temp_") ||
		strings.HasPrefix(s, "pg_toast_temp_")
}

func contains(list []string, value string) bool {
	for _, item := range list {
		if item == value {
			return true
		}
	}
	return false
}

// checksumObject calls the extension function for one relation. A NULL result
// becomes a non-empty reason and is later recorded as skipped; SQL errors are
// returned to the caller and abort the snapshot.
func checksumObject(ctx context.Context, tx *sql.Tx, c Candidate, kind string) (string, string, error) {
	var function string
	// The function accepts regclass; passing the OID avoids identifier quoting
	// issues for unusual schema/table names.
	args := "$1::oid::regclass"
	switch c.Kind {
	case baseline.ObjectTable:
		if kind == baseline.KindPhysical {
			function = "pg_table_physical_checksum"
			// false = do not include tuple headers; this is the compact
			// physical fingerprint used by capture.
			args += ", false"
		} else {
			function = "pg_table_logical_checksum"
		}
	case baseline.ObjectIndex:
		if kind == baseline.KindPhysical {
			function = "pg_index_physical_checksum"
		} else {
			function = "pg_index_logical_checksum"
		}
	default:
		return "", "", fmt.Errorf("unknown object kind %q", c.Kind)
	}

	query := "SELECT " + function + "(" + args + ")::text"
	var checksum sql.NullString
	if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&checksum); err != nil {
		return "", "", err
	}
	if !checksum.Valid {
		return "", "checksum function returned NULL", nil
	}
	return checksum.String, "", nil
}
