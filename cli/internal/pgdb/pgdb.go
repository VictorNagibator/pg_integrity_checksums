// Package pgdb connects the CLI to PostgreSQL and computes baselines using
// the SQL functions provided by the pg_integrity_checksums extension.
package pgdb

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path"
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

	// ChangedOnly enables the --changed-since heuristic. It is valid only for
	// logical checksums and requires Prior (the baseline being verified).
	ChangedOnly bool
	Prior       map[string]baseline.Object
	Reused      *int
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
// ConnectOptions controls connection setup. ApplicationName is visible in
// pg_stat_activity and is useful for identifying CLI sessions.
type ConnectOptions struct {
	Timeout         time.Duration
	ApplicationName string
}

func Open(ctx context.Context, dsn string) (*sql.DB, error) {
	return OpenWithOptions(ctx, dsn, ConnectOptions{})
}

// OpenWithOptions is Open with a configurable connect timeout and
// application_name. The pool is limited to one connection because the CLI
// intentionally keeps capture/verify on a single snapshot.
func OpenWithOptions(ctx context.Context, dsn string, opts ConnectOptions) (*sql.DB, error) {
	if dsn == "" {
		dsn = strings.TrimSpace(os.Getenv("DATABASE_URL"))
	}
	if opts.Timeout <= 0 {
		opts.Timeout = 15 * time.Second
	}
	db, err := sql.Open("pgx", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	pingCtx, cancel := context.WithTimeout(ctx, opts.Timeout)
	defer cancel()
	if err := db.PingContext(pingCtx); err != nil {
		db.Close()
		return nil, fmt.Errorf("connect (timeout %s): %w", opts.Timeout, err)
	}
	if opts.ApplicationName != "" {
		if _, err := db.ExecContext(ctx,
			"SELECT set_config('application_name', $1, false)",
			opts.ApplicationName); err != nil {
			db.Close()
			return nil, fmt.Errorf("set application_name: %w", err)
		}
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

	if opts.ChangedOnly && opts.Kind != baseline.KindLogical {
		return nil, errors.New("--changed-since is supported only for logical checksums")
	}

	// Schema hashes and table statistics are needed for every candidate
	// before checksums, because an index can be reused only if its table is
	// unchanged and index candidates are sorted before tables.
	schemaHashes := make(map[string]string, len(candidates))
	statsByKey := make(map[string]*baseline.TableStats)
	for _, c := range candidates {
		key := candidateKey(c)
		h, err := schemaHash(ctx, tx, c)
		if err != nil {
			return nil, fmt.Errorf("%s %s.%s schema: %w", c.Kind, c.Schema, c.Name, err)
		}
		schemaHashes[key] = h
		if c.Kind == baseline.ObjectTable {
			st, err := tableStats(ctx, tx, c)
			if err != nil {
				return nil, fmt.Errorf("%s %s.%s stats: %w", c.Kind, c.Schema, c.Name, err)
			}
			statsByKey[key] = st
		}
	}

	objects := make([]baseline.Object, 0, len(candidates))
	for _, c := range candidates {
		key := candidateKey(c)
		if opts.Progress != nil {
			opts.Progress(fmt.Sprintf("%s %s.%s", c.Kind, c.Schema, c.Name))
		}
		obj := baseline.Object{
			Kind:        c.Kind,
			Schema:      c.Schema,
			Name:        c.Name,
			TableSchema: c.TableSchema,
			TableName:   c.TableName,
			SchemaHash:  schemaHashes[key],
			Stats:       statsByKey[key],
		}
		if opts.ChangedOnly {
			if reused, ok := reusableObject(c, obj, opts.Prior, schemaHashes, statsByKey); ok {
				if opts.Reused != nil {
					*opts.Reused++
				}
				objects = append(objects, reused)
				continue
			}
		}

		checksum, reason, err := checksumObject(ctx, tx, c, opts.Kind)
		if err != nil {
			return nil, fmt.Errorf("%s %s.%s: %w", c.Kind, c.Schema, c.Name, err)
		}
		obj.Status = baseline.StatusOK
		if reason != "" {
			// NULL normally means the extension does not support this
			// object/kind (for example a physical checksum of an unusual
			// index AM). In strict mode that is a hard failure; otherwise
			// the object is recorded as skipped so the baseline is explicit.
			if opts.Strict {
				return nil, fmt.Errorf("%s %s.%s cannot be checksummed: %s",
					c.Kind, c.Schema, c.Name, reason)
			}
			obj.Status = baseline.StatusSkipped
			obj.Reason = reason
			obj.Stats = nil
		} else {
			obj.Checksum = checksum
		}
		objects = append(objects, obj)
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

// Inspection is a preflight result for doctor: whether capture can read the
// object and whether row-level security would make its data unavailable.
type Inspection struct {
	Candidate
	CanSelect bool
	RLS       bool
	Reason    string
}

// Inspect discovers objects in scope and checks SELECT privilege and RLS
// without computing any checksums. It is intentionally cheap.
func Inspect(ctx context.Context, q Querier, scope baseline.Scope) ([]Inspection, error) {
	query := "SELECT c.oid::bigint, n.nspname, c.relname, c.relkind, " +
		"COALESCE(tn.nspname, ''), COALESCE(tc.relname, ''), " +
		"CASE WHEN c.relkind IN ('i', 'I') THEN has_table_privilege(ix.indrelid, 'SELECT') " +
		"ELSE has_table_privilege(c.oid, 'SELECT') END, " +
		"CASE WHEN c.relkind IN ('r', 'p', 'm') THEN c.relrowsecurity ELSE false END " +
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
		return nil, fmt.Errorf("inspect relations: %w", err)
	}
	defer rows.Close()
	out := make([]Inspection, 0)
	for rows.Next() {
		var item Inspection
		var relkind string
		if err := rows.Scan(&item.OID, &item.Schema, &item.Name, &relkind,
			&item.TableSchema, &item.TableName, &item.CanSelect, &item.RLS); err != nil {
			return nil, err
		}
		switch relkind {
		case "r", "p", "m":
			item.Kind = baseline.ObjectTable
		case "i", "I":
			item.Kind = baseline.ObjectIndex
		default:
			continue
		}
		if !matchesScope(item.Candidate, scope) {
			continue
		}
		if !item.CanSelect {
			item.Reason = "no SELECT privilege"
		} else if item.RLS {
			item.Reason = "row-level security"
		}
		out = append(out, item)
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
	if len(scope.Schemas) > 0 && !matchesSchema(scope.Schemas, c.Schema) {
		return false
	}
	if matchesSchema(scope.ExcludeSchemas, c.Schema) {
		return false
	}

	tableSchema := c.Schema
	tableName := c.Name
	if c.Kind == baseline.ObjectIndex {
		tableSchema = c.TableSchema
		tableName = c.TableName
	}
	if len(scope.Tables) > 0 && !matchesTable(scope.Tables, tableSchema, tableName) {
		return false
	}
	if matchesTable(scope.ExcludeTables, tableSchema, tableName) {
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

// matchesSchema applies shell-style glob patterns such as app_*.
func matchesSchema(patterns []string, schema string) bool {
	for _, pattern := range patterns {
		if ok, _ := path.Match(pattern, schema); ok {
			return true
		}
	}
	return false
}

// matchesTable accepts both schema-qualified patterns (public.orders_*) and
// bare table patterns (orders_*), which then match in every schema.
func matchesTable(patterns []string, schema, table string) bool {
	identity := schema + "." + table
	for _, pattern := range patterns {
		target := identity
		if !strings.Contains(pattern, ".") {
			target = table
		}
		if ok, _ := path.Match(pattern, target); ok {
			return true
		}
	}
	return false
}

// candidateKey mirrors baseline.Object.Key for a discovered relation.
func candidateKey(c Candidate) string {
	return c.Kind + ":" + c.Schema + "." + c.Name
}

// tableStats reads the cumulative tuple counters and relfilenode for a table.
// A missing pg_stat_all_tables row (for example for a partitioned parent) is
// represented as nil so --changed-since falls back to a full checksum.
func tableStats(ctx context.Context, tx *sql.Tx, c Candidate) (*baseline.TableStats, error) {
	query := "SELECT c.relfilenode::bigint, s.n_tup_ins, s.n_tup_upd, s.n_tup_del " +
		"FROM pg_class c LEFT JOIN pg_stat_all_tables s ON s.relid = c.oid " +
		"WHERE c.oid = $1"
	var relfilenode, inserted, updated, deleted sql.NullInt64
	if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&relfilenode, &inserted, &updated, &deleted); err != nil {
		return nil, err
	}
	if !relfilenode.Valid || !inserted.Valid || !updated.Valid || !deleted.Valid {
		return nil, nil
	}
	return &baseline.TableStats{
		Relfilenode: relfilenode.Int64,
		Inserted:    inserted.Int64,
		Updated:     updated.Int64,
		Deleted:     deleted.Int64,
	}, nil
}

// statsEqual is intentionally strict: any counter regression (a stats reset)
// means we cannot prove the table unchanged, so the caller must checksum it.
func statsEqual(prior, current *baseline.TableStats) bool {
	if prior == nil || current == nil {
		return false
	}
	// With all-zero counters we cannot distinguish "never modified" from
	// "statistics were reset"; do not take the shortcut in that case.
	if prior.Inserted+prior.Updated+prior.Deleted == 0 {
		return false
	}
	if prior.Relfilenode != current.Relfilenode {
		return false
	}
	if current.Inserted < prior.Inserted || current.Updated < prior.Updated || current.Deleted < prior.Deleted {
		return false
	}
	return prior.Inserted == current.Inserted &&
		prior.Updated == current.Updated &&
		prior.Deleted == current.Deleted
}

// reusableObject decides whether --changed-since may copy a checksum from the
// previous baseline. It requires an unchanged schema, unchanged table counters
// and, for an index, an unchanged owning table. This is a performance hint;
// any uncertainty falls back to a real checksum.
func reusableObject(c Candidate, current baseline.Object, prior map[string]baseline.Object, hashes map[string]string, statsByKey map[string]*baseline.TableStats) (baseline.Object, bool) {
	if prior == nil {
		return baseline.Object{}, false
	}
	p, ok := prior[candidateKey(c)]
	if !ok || p.Status != baseline.StatusOK || p.Checksum == "" || p.SchemaHash == "" {
		return baseline.Object{}, false
	}
	if p.SchemaHash != current.SchemaHash {
		return baseline.Object{}, false
	}

	switch c.Kind {
	case baseline.ObjectTable:
		if !statsEqual(p.Stats, statsByKey[candidateKey(c)]) {
			return baseline.Object{}, false
		}
	case baseline.ObjectIndex:
		tableKey := "table:" + c.TableSchema + "." + c.TableName
		pt, ok := prior[tableKey]
		if !ok || pt.SchemaHash == "" || pt.SchemaHash != hashes[tableKey] {
			return baseline.Object{}, false
		}
		if !statsEqual(pt.Stats, statsByKey[tableKey]) {
			return baseline.Object{}, false
		}
	default:
		return baseline.Object{}, false
	}

	reused := p
	reused.SchemaHash = current.SchemaHash
	if c.Kind == baseline.ObjectTable {
		reused.Stats = current.Stats
	} else {
		reused.Stats = nil
	}
	return reused, true
}

// schemaHash returns a portable fingerprint of the object definition. It reads
// only catalogs and never includes OIDs, so a dump/restore with different OIDs
// still produces the same hash for the same schema.
func schemaHash(ctx context.Context, tx *sql.Tx, c Candidate) (string, error) {
	var spec string
	switch c.Kind {
	case baseline.ObjectTable:
		query := "SELECT c.relkind::text || '|' || c.relpersistence::text || '|' || " +
			"COALESCE(array_to_string(c.reloptions, ','), '') || '|' || " +
			"COALESCE(pg_get_partkeydef(c.oid), '') || '|' || " +
			"CASE WHEN c.relkind = 'm' THEN pg_get_viewdef(c.oid, true) ELSE '' END || '|' || " +
			"COALESCE((SELECT string_agg(a.attnum::text || ':' || quote_ident(a.attname) || ':' || " +
			"format_type(a.atttypid, a.atttypmod) || ':' || a.attnotnull::text || ':' || " +
			"a.attidentity::text || ':' || a.attgenerated::text || ':' || " +
			"COALESCE(coll.collname, '') || ':' || " +
			"COALESCE(pg_get_expr(d.adbin, d.adrelid), ''), ' | ' ORDER BY a.attnum) " +
			"FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum " +
			"LEFT JOIN pg_collation coll ON coll.oid = a.attcollation " +
			"WHERE a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped), '') || '|' || " +
			"COALESCE((SELECT string_agg(pg_get_constraintdef(con.oid, true), ' | ' ORDER BY con.oid) " +
			"FROM pg_constraint con WHERE con.conrelid = c.oid), '')" +
			" FROM pg_class c WHERE c.oid = $1"
		if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&spec); err != nil {
			return "", err
		}
	case baseline.ObjectIndex:
		query := "SELECT pg_get_indexdef(c.oid) || '|' || am.amname || '|' || " +
			"COALESCE(array_to_string(c.reloptions, ','), '') " +
			"FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid " +
			"JOIN pg_am am ON am.oid = c.relam WHERE c.oid = $1"
		if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&spec); err != nil {
			return "", err
		}
	default:
		return "", fmt.Errorf("unknown object kind %q", c.Kind)
	}
	sum := sha256.Sum256([]byte(spec))
	return "sha256:" + hex.EncodeToString(sum[:]), nil
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
