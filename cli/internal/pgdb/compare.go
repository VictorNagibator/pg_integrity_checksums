package pgdb

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/tablediff"
)

// compareHash is the algorithm of bucket and row checksums: 64 bits keeps
// accidental collisions between key hashes and row checksums negligible.
const compareHash = baseline.HashFNV64

// CompareOptions configures one side of a compare.
type CompareOptions struct {
	Workers int
	Timeout time.Duration
}

// CompareSide is one database of a compare: a repeatable-read, read-only
// transaction, so every table of that side is read from the same snapshot.
type CompareSide struct {
	tx               *sql.Tx
	extSchema        string
	Database         string
	ExtensionVersion string
	PostgresVersion  int
}

// CompareTable is a table found by CompareSide.Tables.
type CompareTable struct {
	Candidate
	// Columns lists the live columns as "name type" in column order; logical
	// row checksums depend on it, so two tables are only compared row by row
	// when it is the same on both sides.
	Columns []string
	// PrimaryKey lists the primary key columns, nil when there is none.
	PrimaryKey []string
	// EstimatedRows is the planner estimate (all leaf partitions of a
	// partitioned table), -1 when the table was never analyzed.
	EstimatedRows int64
}

// BeginCompare starts the snapshot transaction of one side and checks that
// its extension has the bucket functions (2.8.0 or later).
func BeginCompare(ctx context.Context, db *sql.DB, opts CompareOptions) (*CompareSide, error) {
	tx, err := db.BeginTx(ctx, &sql.TxOptions{
		Isolation: sql.LevelRepeatableRead,
		ReadOnly:  true,
	})
	if err != nil {
		return nil, fmt.Errorf("begin repeatable read: %w", err)
	}
	ok := false
	defer func() {
		if !ok {
			tx.Rollback()
		}
	}()

	if opts.Timeout > 0 {
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
		if _, err := tx.ExecContext(ctx,
			"SELECT set_config($1, $2, true)",
			workersGUC, strconv.Itoa(opts.Workers)); err != nil {
			return nil, fmt.Errorf("set %s: %w", workersGUC, err)
		}
	}

	side := &CompareSide{tx: tx}
	side.ExtensionVersion, side.PostgresVersion, side.Database, err = metadata(ctx, tx)
	if err != nil {
		return nil, err
	}
	side.extSchema, err = extensionSchema(ctx, tx)
	if err != nil {
		return nil, err
	}
	var hasBuckets bool
	if err := tx.QueryRowContext(ctx,
		"SELECT EXISTS (SELECT 1 FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "+
			"WHERE n.nspname = $1 AND p.proname = 'pg_table_bucket_checksums')",
		side.extSchema).Scan(&hasBuckets); err != nil {
		return nil, fmt.Errorf("check extension functions: %w", err)
	}
	if !hasBuckets {
		return nil, fmt.Errorf("extension %s %s has no pg_table_bucket_checksums (needs 2.8.0 or later; run ALTER EXTENSION %s UPDATE)",
			extensionName, side.ExtensionVersion, extensionName)
	}
	ok = true
	return side, nil
}

// Close ends the snapshot transaction.
func (s *CompareSide) Close() error {
	return s.tx.Rollback()
}

// Tables lists the tables in scope (ordinary and partitioned tables and
// materialized views; partitions are covered by their parent), sorted by
// schema and name.
func (s *CompareSide) Tables(ctx context.Context, scope baseline.Scope) ([]CompareTable, error) {
	scope.IncludeIndexes = false
	scope.IncludeSequences = false
	candidates, err := Discover(ctx, s.tx, scope)
	if err != nil {
		return nil, err
	}
	out := make([]CompareTable, 0, len(candidates))
	for _, c := range candidates {
		if c.Kind != baseline.ObjectTable {
			continue
		}
		t := CompareTable{Candidate: c}
		if t.Columns, err = s.columns(ctx, c.OID); err != nil {
			return nil, fmt.Errorf("read columns of %s.%s: %w", c.Schema, c.Name, err)
		}
		if t.PrimaryKey, err = s.primaryKey(ctx, c.OID); err != nil {
			return nil, fmt.Errorf("read primary key of %s.%s: %w", c.Schema, c.Name, err)
		}
		if t.EstimatedRows, err = s.estimatedRows(ctx, c.OID); err != nil {
			return nil, fmt.Errorf("read row estimate of %s.%s: %w", c.Schema, c.Name, err)
		}
		out = append(out, t)
	}
	return out, nil
}

func (s *CompareSide) columns(ctx context.Context, oid int64) ([]string, error) {
	rows, err := s.tx.QueryContext(ctx,
		"SELECT quote_ident(attname) || ' ' || format_type(atttypid, atttypmod) "+
			"FROM pg_attribute WHERE attrelid = $1::oid AND attnum > 0 AND NOT attisdropped "+
			"ORDER BY attnum", oid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var col string
		if err := rows.Scan(&col); err != nil {
			return nil, err
		}
		out = append(out, col)
	}
	return out, rows.Err()
}

func (s *CompareSide) primaryKey(ctx context.Context, oid int64) ([]string, error) {
	rows, err := s.tx.QueryContext(ctx,
		"SELECT a.attname FROM pg_index i "+
			"CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS k(attnum, ord) "+
			"JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum "+
			"WHERE i.indrelid = $1::oid AND i.indisprimary AND k.ord <= i.indnkeyatts "+
			"ORDER BY k.ord", oid)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var col string
		if err := rows.Scan(&col); err != nil {
			return nil, err
		}
		out = append(out, col)
	}
	return out, rows.Err()
}

func (s *CompareSide) estimatedRows(ctx context.Context, oid int64) (int64, error) {
	// pg_partition_tree() returns no rows for a table that is not
	// partitioned, so the table itself is added; a partitioned table has
	// no rows of its own and is filtered out by relkind.
	var n float64
	err := s.tx.QueryRowContext(ctx,
		"SELECT COALESCE(sum(c.reltuples) FILTER (WHERE c.reltuples >= 0), -1)::float8 "+
			"FROM (SELECT $1::oid AS relid UNION SELECT relid FROM pg_partition_tree($1::oid::regclass)) t "+
			"JOIN pg_class c ON c.oid = t.relid WHERE c.relkind IN ('r', 'm')", oid).Scan(&n)
	if err != nil {
		return 0, err
	}
	return int64(n), nil
}

// Guard runs fn in a savepoint, so an error in one table (missing privilege,
// row-level security, ...) does not abort the snapshot transaction.
func (s *CompareSide) Guard(ctx context.Context, fn func() error) error {
	if _, err := s.tx.ExecContext(ctx, "SAVEPOINT pgics_compare"); err != nil {
		return err
	}
	if err := fn(); err != nil {
		if _, rbErr := s.tx.ExecContext(ctx, "ROLLBACK TO SAVEPOINT pgics_compare"); rbErr != nil {
			return errors.Join(err, rbErr)
		}
		return err
	}
	_, err := s.tx.ExecContext(ctx, "RELEASE SAVEPOINT pgics_compare")
	return err
}

// keyArray formats key columns as a text[] literal. An empty slice selects
// the whole row as the key.
func keyArray(key []string) string {
	parts := make([]string, len(key))
	for i, k := range key {
		parts[i] = `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(k) + `"`
	}
	return "{" + strings.Join(parts, ",") + "}"
}

// Buckets returns the bucket checksums of a table, in bucket order.
func (s *CompareSide) Buckets(ctx context.Context, oid int64, buckets int, key []string) ([]tablediff.Bucket, error) {
	rows, err := s.tx.QueryContext(ctx,
		"SELECT bucket, row_count, encode(checksum, 'hex') FROM "+quoteIdent(s.extSchema)+
			".pg_table_bucket_checksums($1::oid::regclass, $2, $3::text[], $4) ORDER BY bucket",
		oid, buckets, keyArray(key), compareHash)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make([]tablediff.Bucket, 0, buckets)
	for rows.Next() {
		var b tablediff.Bucket
		if err := rows.Scan(&b.Bucket, &b.Count, &b.Checksum); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

// Rows returns the key, key hash and row checksum of the rows of a table in
// the given buckets.
func (s *CompareSide) Rows(ctx context.Context, oid int64, buckets int, only []int, key []string) ([]tablediff.Row, error) {
	sorted := append([]int(nil), only...)
	sort.Ints(sorted)
	parts := make([]string, len(sorted))
	for i, b := range sorted {
		parts[i] = strconv.Itoa(b)
	}
	rows, err := s.tx.QueryContext(ctx,
		"SELECT bucket, encode(key_hash, 'hex'), key, encode(row_checksum, 'hex') FROM "+
			quoteIdent(s.extSchema)+
			".pg_table_row_checksums($1::oid::regclass, $2, $3::int[], $4::text[], $5)",
		oid, buckets, "{"+strings.Join(parts, ",")+"}", keyArray(key), compareHash)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []tablediff.Row
	for rows.Next() {
		var r tablediff.Row
		if err := rows.Scan(&r.Bucket, &r.KeyHash, &r.Key, &r.Checksum); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}
