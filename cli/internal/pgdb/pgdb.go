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
	Hash        string
	Scope       baseline.Scope
	Workers     int
	Timeout     time.Duration
	Strict      bool
	Progress    func(string)
	// Warning, when set, receives non-fatal notices about the capture.
	Warning func(string)

	// ChangedOnly enables the --changed-since heuristic. It is valid only for
	// logical checksums and requires Prior (the baseline being verified).
	ChangedOnly bool
	Prior       map[string]baseline.Object
	Reused      *int

	// SequencesOptional lets Snapshot drop sequences from the scope, with a
	// warning, when the extension predates pg_sequence_logical_checksum.
	// capture sets it when sequences were not asked for explicitly; verify
	// leaves it false, because the baseline's scope must be honoured.
	SequencesOptional bool

	// Precheck, when set, is called before any relation is checksummed with
	// a baseline that has only the identity fields filled in (kind, hash,
	// extension and PostgreSQL versions, database, cluster identity and
	// database OID).  An error stops the snapshot and is returned wrapped in
	// an IncompatibleError, so a baseline from the wrong server is refused
	// without scanning the whole database first.
	Precheck func(header *baseline.Baseline) error
}

// IncompatibleError is returned by Snapshot when Options.Precheck rejects
// the server.
type IncompatibleError struct {
	Err error
}

func (e *IncompatibleError) Error() string { return e.Err.Error() }
func (e *IncompatibleError) Unwrap() error { return e.Err }

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
// repeatable-read, read-only transaction. Sequences are the exception to
// the single snapshot: they are not transactional, so their state is read
// as it is when each one is checksummed. It returns a fully populated
// baseline; the caller decides whether and where to save it.
func Snapshot(ctx context.Context, db *sql.DB, opts Options) (*baseline.Baseline, error) {
	if opts.Kind == "" {
		opts.Kind = baseline.KindLogical
	}
	if opts.Kind != baseline.KindLogical && opts.Kind != baseline.KindPhysical {
		return nil, fmt.Errorf("invalid kind %q", opts.Kind)
	}
	if opts.Hash != "" && opts.Hash != baseline.HashFNV32 && opts.Hash != baseline.HashFNV64 {
		return nil, fmt.Errorf("invalid hash %q", opts.Hash)
	}
	// Sequences have a logical checksum only.
	if opts.Kind == baseline.KindPhysical {
		opts.Scope.IncludeSequences = false
	}

	// Table statistics are read before the snapshot transaction starts, on
	// purpose. --changed-since reuses a checksum when the counters have not
	// moved since the baseline, so the counters stored with a checksum must
	// never cover a change that the checksum's snapshot does not contain.
	// Read first, they can only lag behind the snapshot: a change that
	// commits in between is in the checksum but not yet in the counters, and
	// merely forces a recomputation on the next run. Read inside the
	// transaction (as before), a change committed after the snapshot was
	// taken could be counted but not checksummed, and the stale checksum would
	// then be reused forever.
	statsByOID, err := readTableStats(ctx, db)
	if err != nil {
		return nil, err
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

	if opts.Scope.IncludeSequences {
		ok, err := hasSequenceChecksum(ctx, tx)
		if err != nil {
			return nil, err
		}
		if !ok {
			if !opts.SequencesOptional {
				return nil, fmt.Errorf("extension %s %s has no pg_sequence_logical_checksum (needs 2.9.0 or later; run ALTER EXTENSION %s UPDATE)",
					extensionName, extVersion, extensionName)
			}
			opts.Scope.IncludeSequences = false
			if opts.Warning != nil {
				opts.Warning(fmt.Sprintf("extension %s %s cannot checksum sequences (needs 2.9.0 or later); capturing without sequences", extensionName, extVersion))
			}
		}
	}

	if opts.Precheck != nil {
		header := baseline.NewWithHash(opts.ToolVersion, opts.Kind, opts.Hash,
			extVersion, postgresVersion, database, opts.Scope, nil)
		header.SystemIdentifier = systemIdentifier
		header.DatabaseOID = databaseOID
		if err := opts.Precheck(header); err != nil {
			return nil, &IncompatibleError{Err: err}
		}
	}

	candidates, err := Discover(ctx, tx, opts.Scope)
	if err != nil {
		return nil, err
	}

	if opts.ChangedOnly && opts.Kind != baseline.KindLogical {
		return nil, errors.New("--changed-since is supported only for logical checksums")
	}
	if opts.ChangedOnly {
		// WAL replay does not update pg_stat_all_tables on a standby, so the
		// counters --changed-since relies on stay flat while the data
		// changes.  They are usually zero there, which already disables the
		// shortcut, but a standby can carry non-zero counters (for example a
		// data directory cloned from a cleanly stopped primary).  Never reuse
		// checksums on a standby; checksum every object instead.
		var inRecovery bool
		if err := tx.QueryRowContext(ctx, "SELECT pg_is_in_recovery()").Scan(&inRecovery); err != nil {
			return nil, fmt.Errorf("read recovery state: %w", err)
		}
		if inRecovery {
			opts.ChangedOnly = false
			if opts.Warning != nil {
				opts.Warning("--changed-since is ignored on a standby (WAL replay does not update table statistics); checksumming every object")
			}
		}
	}

	// Schema hashes and table statistics are needed for every candidate
	// before checksums, because an index can be reused only if its table is
	// unchanged and index candidates are sorted before tables.
	schemaHashes := make(map[string]string, len(candidates))
	statsByKey := make(map[string]*baseline.TableStats)
	restoreSettings, err := pinSchemaHashSettings(ctx, tx)
	if err != nil {
		return nil, err
	}
	for _, c := range candidates {
		key := candidateKey(c)
		h, err := schemaHash(ctx, tx, c)
		if err != nil {
			return nil, fmt.Errorf("%s %s.%s schema: %w", c.Kind, c.Schema, c.Name, err)
		}
		schemaHashes[key] = h
		if c.Kind == baseline.ObjectTable {
			// nil (no statistics, or a table created after they were read)
			// disables --changed-since for this table.
			statsByKey[key] = statsByOID[c.OID]
		}
	}
	if err := restoreSettings(); err != nil {
		return nil, err
	}

	// The extension is relocatable, so its schema may not be in search_path.
	extSchema, err := extensionSchema(ctx, tx)
	if err != nil {
		return nil, err
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

		checksum, reason, err := checksumObject(ctx, tx, extSchema, c, opts.Kind, opts.Hash)
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

	b := baseline.NewWithHash(
		opts.ToolVersion,
		opts.Kind,
		opts.Hash,
		extVersion,
		postgresVersion,
		database,
		opts.Scope,
		objects,
	)
	b.SystemIdentifier = systemIdentifier
	b.DatabaseOID = databaseOID
	b.SchemaHashVersion = baseline.SchemaHashVersion
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

// Discover enumerates top-level tables, indexes and sequences in pg_class. It does not
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
		ownerJoins +
		"WHERE c.relkind IN ('r', 'p', 'm', 'i', 'I', 'S') " +
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
		// i/I = ordinary/partitioned index; S = sequence.
		if c.Kind = objectKind(relkind); c.Kind == "" {
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
		ownerJoins +
		"WHERE c.relkind IN ('r', 'p', 'm', 'i', 'I', 'S') " +
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
		if item.Kind = objectKind(relkind); item.Kind == "" {
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

// ownerJoins resolves the table an object belongs to: the indexed table of
// an index, and the table owning a sequence through a serial or identity
// column or OWNED BY (an 'a' or 'i' dependency on a table column).
const ownerJoins = "LEFT JOIN pg_index ix ON ix.indexrelid = c.oid " +
	"LEFT JOIN pg_depend dep ON c.relkind = 'S' " +
	"AND dep.classid = 'pg_class'::regclass AND dep.objid = c.oid AND dep.objsubid = 0 " +
	"AND dep.refclassid = 'pg_class'::regclass AND dep.refobjsubid > 0 " +
	"AND dep.deptype IN ('a', 'i') " +
	"LEFT JOIN pg_class tc ON tc.oid = COALESCE(ix.indrelid, dep.refobjid) " +
	"LEFT JOIN pg_namespace tn ON tn.oid = tc.relnamespace "

// objectKind maps a pg_class relkind to a baseline object kind, or "" for a
// relation kind that baselines do not contain.
func objectKind(relkind string) string {
	switch relkind {
	case "r", "p", "m":
		return baseline.ObjectTable
	case "i", "I":
		return baseline.ObjectIndex
	case "S":
		return baseline.ObjectSequence
	}
	return ""
}

// hasSequenceChecksum reports whether the installed extension version has
// pg_sequence_logical_checksum (2.9.0 and later).
func hasSequenceChecksum(ctx context.Context, tx *sql.Tx) (bool, error) {
	var ok bool
	if err := tx.QueryRowContext(ctx,
		"SELECT EXISTS (SELECT 1 FROM pg_depend d "+
			"JOIN pg_extension e ON e.oid = d.refobjid "+
			"JOIN pg_proc p ON p.oid = d.objid "+
			"WHERE d.classid = 'pg_proc'::regclass AND d.deptype = 'e' "+
			"AND e.extname = $1 AND p.proname = 'pg_sequence_logical_checksum')",
		extensionName).Scan(&ok); err != nil {
		return false, fmt.Errorf("check extension functions: %w", err)
	}
	return ok, nil
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
	if c.Kind == baseline.ObjectSequence && !scope.IncludeSequences {
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

	// Indexes, and sequences owned by a table column, follow their table.
	tableSchema := c.Schema
	tableName := c.Name
	if c.TableName != "" {
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

// readTableStats reads the cumulative tuple counters and relfilenode of every
// table, keyed by OID. It runs as its own statement before the snapshot
// transaction; see Snapshot. A table without a pg_stat_all_tables row (for
// example a partitioned parent) is left out, so --changed-since falls back to
// a full checksum for it.
func readTableStats(ctx context.Context, q Querier) (map[int64]*baseline.TableStats, error) {
	query := "SELECT c.oid::bigint, c.relfilenode::bigint, s.n_tup_ins, s.n_tup_upd, s.n_tup_del " +
		"FROM pg_catalog.pg_class c " +
		"JOIN pg_catalog.pg_stat_all_tables s ON s.relid = c.oid " +
		"WHERE c.relkind IN ('r', 'm', 'p')"
	rows, err := q.QueryContext(ctx, query)
	if err != nil {
		return nil, fmt.Errorf("read table statistics: %w", err)
	}
	defer rows.Close()
	out := make(map[int64]*baseline.TableStats)
	for rows.Next() {
		var oid int64
		var relfilenode, inserted, updated, deleted sql.NullInt64
		if err := rows.Scan(&oid, &relfilenode, &inserted, &updated, &deleted); err != nil {
			return nil, err
		}
		if !relfilenode.Valid || !inserted.Valid || !updated.Valid || !deleted.Valid {
			continue
		}
		out[oid] = &baseline.TableStats{
			Relfilenode: relfilenode.Int64,
			Inserted:    inserted.Int64,
			Updated:     updated.Int64,
			Deleted:     deleted.Int64,
		}
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

// pinSchemaHashSettings fixes the session settings that change how
// format_type(), pg_get_expr(), pg_get_constraintdef() and pg_get_indexdef()
// spell names: those functions schema-qualify every name that is not visible
// in search_path, and quote every identifier when quote_all_identifiers is
// on. With search_path = pg_catalog every user object is schema-qualified,
// whatever the role or connection settings, so schema hashes are portable.
//
// The settings are transaction-local and the returned function restores
// them: the checksum functions run with the session's own search_path, which
// index expressions may depend on (before PostgreSQL 17 the server does not
// restrict it during maintenance).
func pinSchemaHashSettings(ctx context.Context, tx *sql.Tx) (func() error, error) {
	var searchPath, quoteAll string
	if err := tx.QueryRowContext(ctx,
		"SELECT pg_catalog.current_setting('search_path'), "+
			"pg_catalog.current_setting('quote_all_identifiers')").
		Scan(&searchPath, &quoteAll); err != nil {
		return nil, fmt.Errorf("read session settings: %w", err)
	}
	if _, err := tx.ExecContext(ctx,
		"SELECT pg_catalog.set_config('search_path', 'pg_catalog', true), "+
			"pg_catalog.set_config('quote_all_identifiers', 'off', true)"); err != nil {
		return nil, fmt.Errorf("pin schema hash settings: %w", err)
	}
	return func() error {
		if _, err := tx.ExecContext(ctx,
			"SELECT pg_catalog.set_config('search_path', $1, true), "+
				"pg_catalog.set_config('quote_all_identifiers', $2, true)",
			searchPath, quoteAll); err != nil {
			return fmt.Errorf("restore session settings: %w", err)
		}
		return nil
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
// still produces the same hash for the same schema: constraints are ordered by
// name (a restore creates them in another order than the original DDL did),
// and the caller pins the settings that change how names are spelled (see
// pinSchemaHashSettings). The result is baseline.SchemaHashVersion 3: columns are numbered by
// position among live columns, because attnum (which counts dropped columns)
// changes after a dump/restore.
func schemaHash(ctx context.Context, tx *sql.Tx, c Candidate) (string, error) {
	var spec string
	switch c.Kind {
	case baseline.ObjectTable:
		query := "SELECT c.relkind::text || '|' || c.relpersistence::text || '|' || " +
			"COALESCE(array_to_string(c.reloptions, ','), '') || '|' || " +
			"COALESCE(pg_get_partkeydef(c.oid), '') || '|' || " +
			"CASE WHEN c.relkind = 'm' THEN pg_get_viewdef(c.oid, true) ELSE '' END || '|' || " +
			"COALESCE((SELECT string_agg(a.ord::text || ':' || quote_ident(a.attname) || ':' || " +
			"format_type(a.atttypid, a.atttypmod) || ':' || a.attnotnull::text || ':' || " +
			"a.attidentity::text || ':' || a.attgenerated::text || ':' || " +
			"COALESCE(coll.collname, '') || ':' || " +
			"COALESCE(pg_get_expr(d.adbin, d.adrelid), ''), ' | ' ORDER BY a.ord) " +
			"FROM (SELECT x.*, row_number() OVER (ORDER BY x.attnum) AS ord FROM pg_attribute x " +
			"WHERE x.attrelid = c.oid AND x.attnum > 0 AND NOT x.attisdropped) a LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum " +
			"LEFT JOIN pg_collation coll ON coll.oid = a.attcollation " +
			"), '') || '|' || " +
			"COALESCE((SELECT string_agg(pg_get_constraintdef(con.oid, true), ' | ' ORDER BY con.conname) " +
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
	case baseline.ObjectSequence:
		// The parameters pg_dump restores with CREATE SEQUENCE; the owning
		// column is covered by the owning table's identity in the object.
		query := "SELECT c.relpersistence::text || '|' || format_type(s.seqtypid, NULL) || '|' || " +
			"s.seqstart::text || '|' || s.seqincrement::text || '|' || " +
			"s.seqmin::text || '|' || s.seqmax::text || '|' || " +
			"s.seqcache::text || '|' || s.seqcycle::text " +
			"FROM pg_class c JOIN pg_sequence s ON s.seqrelid = c.oid WHERE c.oid = $1"
		if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&spec); err != nil {
			return "", err
		}
	default:
		return "", fmt.Errorf("unknown object kind %q", c.Kind)
	}
	sum := sha256.Sum256([]byte(spec))
	return "sha256:" + hex.EncodeToString(sum[:]), nil
}

// extensionSchema returns the schema the extension is installed in.
func extensionSchema(ctx context.Context, tx *sql.Tx) (string, error) {
	var schema string
	if err := tx.QueryRowContext(ctx,
		"SELECT n.nspname FROM pg_extension e JOIN pg_namespace n ON n.oid = e.extnamespace WHERE e.extname = $1",
		extensionName).Scan(&schema); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return "", fmt.Errorf("extension %q is not installed", extensionName)
		}
		return "", fmt.Errorf("read extension schema: %w", err)
	}
	return schema, nil
}

// quoteIdent quotes an SQL identifier.
func quoteIdent(s string) string {
	return `"` + strings.ReplaceAll(s, `"`, `""`) + `"`
}

// checksumObject calls the extension function for one relation. A NULL result
// becomes a non-empty reason and is later recorded as skipped; SQL errors are
// returned to the caller and abort the snapshot.
func checksumObject(ctx context.Context, tx *sql.Tx, extSchema string, c Candidate, kind, hash string) (string, string, error) {
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
	case baseline.ObjectSequence:
		if kind == baseline.KindPhysical {
			return "", "", errors.New("sequences have no physical checksum")
		}
		function = "pg_sequence_logical_checksum"
	default:
		return "", "", fmt.Errorf("unknown object kind %q", c.Kind)
	}

	// Qualify with the extension's schema: it may not be in search_path.
	prefix := quoteIdent(extSchema) + "."

	if hash == "" {
		query := "SELECT " + prefix + function + "(" + args + ")::text"
		var checksum sql.NullString
		if err := tx.QueryRowContext(ctx, query, c.OID).Scan(&checksum); err != nil {
			return "", "", err
		}
		if !checksum.Valid {
			return "", "checksum function returned NULL", nil
		}
		return checksum.String, "", nil
	}

	// v2 captures call the generic bytea API and convert the digest to a
	// signed decimal bigint. Table logical fnv32 is unsigned because the
	// legacy table wrapper returned the unsigned 32-bit value in a bigint.
	unsigned := hash == baseline.HashFNV32 && c.Kind == baseline.ObjectTable && kind == baseline.KindLogical
	args = "$1::oid::regclass"
	switch c.Kind {
	case baseline.ObjectTable:
		if kind == baseline.KindPhysical {
			function = "pg_table_physical_checksum"
			args += ", false, $2"
		} else {
			function = "pg_table_logical_checksum"
			args += ", $2"
		}
	case baseline.ObjectIndex:
		if kind == baseline.KindPhysical {
			function = "pg_index_physical_checksum"
		} else {
			function = "pg_index_logical_checksum"
		}
		args += ", $2"
	case baseline.ObjectSequence:
		args += ", $2"
	default:
		return "", "", fmt.Errorf("unknown object kind %q", c.Kind)
	}
	unsignedSQL := "false"
	if unsigned {
		unsignedSQL = "true"
	}
	query := "SELECT " + prefix + "pg_checksum_bigint(" + prefix + function + "(" + args + "), " + unsignedSQL + ")::text"
	var checksum sql.NullString
	if err := tx.QueryRowContext(ctx, query, c.OID, hash).Scan(&checksum); err != nil {
		return "", "", err
	}
	if !checksum.Valid {
		return "", "checksum function returned NULL", nil
	}
	return checksum.String, "", nil
}
