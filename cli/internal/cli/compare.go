package cli

import (
	"context"
	"errors"
	"fmt"
	"io"
	"slices"
	"strings"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/tablediff"
)

// Table statuses of a compare.
const (
	compareEqual         = "equal"
	compareDifferent     = "different"
	compareOnlyInSource  = "only_in_source"
	compareOnlyInTarget  = "only_in_target"
	compareSchemaDiffers = "schema_differs"
	compareError         = "error"
)

// keyMap implements flag.Value for --key schema.table=col1,col2.
type keyMap map[string][]string

func (k keyMap) String() string { return "" }

func (k keyMap) Set(value string) error {
	table, cols, ok := strings.Cut(value, "=")
	table = strings.TrimSpace(table)
	if !ok || table == "" || !strings.Contains(table, ".") {
		return fmt.Errorf("want schema.table=col1,col2 (an empty column list selects the whole row), got %q", value)
	}
	list := []string{}
	for _, c := range strings.Split(cols, ",") {
		if c = strings.TrimSpace(c); c != "" {
			list = append(list, c)
		}
	}
	k[table] = list
	return nil
}

// compareTable is the result for one table.
type compareTable struct {
	Schema           string              `json:"schema"`
	Name             string              `json:"name"`
	Status           string              `json:"status"`
	Reason           string              `json:"reason,omitempty"`
	Key              []string            `json:"key"`
	Buckets          int                 `json:"buckets,omitempty"`
	SourceRows       int64               `json:"source_rows"`
	TargetRows       int64               `json:"target_rows"`
	DifferingBuckets []int               `json:"differing_buckets,omitempty"`
	RowsInBuckets    int64               `json:"rows_in_differing_buckets,omitempty"`
	RowsListed       bool                `json:"rows_listed"`
	Rows             []tablediff.RowDiff `json:"rows,omitempty"`
}

type compareSummary struct {
	Tables        int `json:"tables"`
	Equal         int `json:"equal"`
	Different     int `json:"different"`
	OnlyInSource  int `json:"only_in_source"`
	OnlyInTarget  int `json:"only_in_target"`
	SchemaDiffers int `json:"schema_differs"`
	Errors        int `json:"errors"`
	MissingRows   int `json:"missing_rows"`
	ExtraRows     int `json:"extra_rows"`
	ChangedRows   int `json:"changed_rows"`
}

func (s compareSummary) hasDifferences() bool {
	return s.Different+s.OnlyInSource+s.OnlyInTarget+s.SchemaDiffers+s.Errors > 0
}

type compareSideInfo struct {
	Database         string `json:"database"`
	ExtensionVersion string `json:"extension_version"`
	PostgresVersion  int    `json:"postgres_version"`
}

type compareReport struct {
	Source  compareSideInfo `json:"source"`
	Target  compareSideInfo `json:"target"`
	Tables  []compareTable  `json:"tables"`
	Summary compareSummary  `json:"summary"`
}

// runCompare compares the tables of two live databases row by row through
// the extension's bucket and row checksum functions, without moving the
// table data to the client.
func runCompare(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("compare", stderr)
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: pg_integrity_checksums compare --source DSN --target DSN [options]")
		fs.PrintDefaults()
	}
	sourceDSN := fs.String("source", "", "connection string of the source database")
	targetDSN := fs.String("target", "", "connection string of the target database")
	connectTimeout := fs.Duration("connect-timeout", 0, "connect timeout")
	applicationName := fs.String("application-name", "", "application_name for pg_stat_activity")
	var schemas, excludeSchemas, tables, excludeTables stringList
	fs.Var(&schemas, "schema", "include schema (repeatable or comma-separated; globs allowed)")
	fs.Var(&excludeSchemas, "exclude-schema", "exclude schema (repeatable; globs allowed)")
	fs.Var(&tables, "table", "include schema.table (repeatable; globs allowed)")
	fs.Var(&excludeTables, "exclude-table", "exclude schema.table (repeatable; globs allowed)")
	includeSystem := fs.Bool("include-system", false, "include system schemas")
	keys := keyMap{}
	fs.Var(keys, "key", "row key of a table: schema.table=col1,col2 (repeatable; default: primary key, else the whole row)")
	buckets := fs.Int("buckets", 0, "buckets per table (0: about 64 rows per bucket)")
	maxRows := fs.Int64("max-rows", 10000, "list rows only when the differing buckets of a table hold at most this many rows (0: buckets only)")
	workers := fs.Int("workers", 0, "pg_integrity_checksums.workers on both servers")
	timeout := fs.Duration("timeout", 0, "statement timeout")
	formatValue := fs.String("format", "text", "text or json")
	outputPath := fs.String("output", "-", "report file, or - for stdout")
	showOK := fs.Bool("show-ok", false, "include equal tables in the text report")
	noFail := fs.Bool("no-fail", false, "exit 0 even when the databases differ")
	quiet := fs.Bool("quiet", false, "suppress progress")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}

	if *sourceDSN == "" || *targetDSN == "" {
		return commandError(stderr, errors.New("--source and --target are required"))
	}
	if *formatValue != "text" && *formatValue != "json" {
		return commandError(stderr, fmt.Errorf("invalid --format %q (want text or json)", *formatValue))
	}
	if *buckets < 0 || *buckets > tablediff.MaxBuckets {
		return commandError(stderr, fmt.Errorf("--buckets must be between 0 and %d", tablediff.MaxBuckets))
	}
	if *maxRows < 0 || *workers < 0 || *timeout < 0 || *connectTimeout < 0 {
		return commandError(stderr, errors.New("--max-rows, --workers, --timeout and --connect-timeout must be >= 0"))
	}

	scope := baseline.Scope{
		Schemas:        schemas,
		ExcludeSchemas: excludeSchemas,
		Tables:         tables,
		ExcludeTables:  excludeTables,
		IncludeSystem:  *includeSystem,
	}
	connect := pgdb.ConnectOptions{Timeout: *connectTimeout, ApplicationName: *applicationName}
	sideOpts := pgdb.CompareOptions{Workers: *workers, Timeout: *timeout}

	sourceDB, err := pgdb.OpenWithOptions(ctx, *sourceDSN, connect)
	if err != nil {
		return commandError(stderr, fmt.Errorf("source: %w", err))
	}
	defer sourceDB.Close()
	targetDB, err := pgdb.OpenWithOptions(ctx, *targetDSN, connect)
	if err != nil {
		return commandError(stderr, fmt.Errorf("target: %w", err))
	}
	defer targetDB.Close()

	source, err := pgdb.BeginCompare(ctx, sourceDB, sideOpts)
	if err != nil {
		return commandError(stderr, fmt.Errorf("source: %w", err))
	}
	defer source.Close()
	target, err := pgdb.BeginCompare(ctx, targetDB, sideOpts)
	if err != nil {
		return commandError(stderr, fmt.Errorf("target: %w", err))
	}
	defer target.Close()

	sourceTables, err := source.Tables(ctx, scope)
	if err != nil {
		return commandError(stderr, fmt.Errorf("source: %w", err))
	}
	targetTables, err := target.Tables(ctx, scope)
	if err != nil {
		return commandError(stderr, fmt.Errorf("target: %w", err))
	}
	for name := range keys {
		if !slices.ContainsFunc(sourceTables, func(t pgdb.CompareTable) bool {
			return t.Schema+"."+t.Name == name
		}) {
			return commandError(stderr, fmt.Errorf("--key: table %s is not in the source scope", name))
		}
	}

	report := compareReport{
		Source: compareSideInfo{source.Database, source.ExtensionVersion, source.PostgresVersion},
		Target: compareSideInfo{target.Database, target.ExtensionVersion, target.PostgresVersion},
	}
	if source.ExtensionVersion != target.ExtensionVersion && !*quiet {
		fmt.Fprintf(stderr, "warning: source extension %s, target extension %s; a release that changed checksum values makes equal rows differ\n",
			source.ExtensionVersion, target.ExtensionVersion)
	}

	targetByName := make(map[string]pgdb.CompareTable, len(targetTables))
	for _, t := range targetTables {
		targetByName[t.Schema+"."+t.Name] = t
	}
	sourceNames := make(map[string]bool, len(sourceTables))
	for _, s := range sourceTables {
		name := s.Schema + "." + s.Name
		sourceNames[name] = true
		t, ok := targetByName[name]
		if !ok {
			report.Tables = append(report.Tables, compareTable{Schema: s.Schema, Name: s.Name, Status: compareOnlyInSource})
			continue
		}
		if !*quiet {
			fmt.Fprintf(stderr, "comparing %s\n", name)
		}
		key, explicit := keys[name]
		report.Tables = append(report.Tables, compareOne(ctx, source, target, s, t, key, explicit, *buckets, *maxRows, stderr, *quiet))
	}
	for _, t := range targetTables {
		if !sourceNames[t.Schema+"."+t.Name] {
			report.Tables = append(report.Tables, compareTable{Schema: t.Schema, Name: t.Name, Status: compareOnlyInTarget})
		}
	}
	slices.SortFunc(report.Tables, func(a, b compareTable) int {
		if c := strings.Compare(a.Schema, b.Schema); c != 0 {
			return c
		}
		return strings.Compare(a.Name, b.Name)
	})
	report.Summary = summarizeCompare(report.Tables)

	out, closeOutput, err := outputWriter(stdout, *outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()
	if *formatValue == "json" {
		if err := encodeJSON(out, report); err != nil {
			return commandError(stderr, err)
		}
	} else {
		writeCompareText(out, report, *showOK)
	}
	if !*noFail && report.Summary.hasDifferences() {
		return 1
	}
	return 0
}

// compareOne compares one table present on both sides.
func compareOne(ctx context.Context, source, target *pgdb.CompareSide, s, t pgdb.CompareTable,
	key []string, explicitKey bool, buckets int, maxRows int64, stderr io.Writer, quiet bool) compareTable {
	res := compareTable{Schema: s.Schema, Name: s.Name}
	if !slices.Equal(s.Columns, t.Columns) {
		res.Status = compareSchemaDiffers
		res.Reason = fmt.Sprintf("columns differ: source (%s), target (%s)",
			strings.Join(s.Columns, ", "), strings.Join(t.Columns, ", "))
		return res
	}
	if !explicitKey {
		// The same key on both sides, or the key hashes do not match.
		key = s.PrimaryKey
		if key == nil {
			key = []string{}
		}
		if !slices.Equal(s.PrimaryKey, t.PrimaryKey) && !quiet {
			fmt.Fprintf(stderr, "warning: %s.%s: primary keys differ (source %v, target %v); using the source key\n",
				s.Schema, s.Name, s.PrimaryKey, t.PrimaryKey)
		}
	}
	res.Key = key
	if buckets == 0 {
		buckets = tablediff.AutoBuckets(max(s.EstimatedRows, t.EstimatedRows))
	}
	res.Buckets = buckets

	var srcBuckets, tgtBuckets []tablediff.Bucket
	fail := func(side string, err error) compareTable {
		res.Status = compareError
		res.Reason = side + ": " + err.Error()
		return res
	}
	if err := source.Guard(ctx, func() (err error) {
		srcBuckets, err = source.Buckets(ctx, s.OID, buckets, key)
		return err
	}); err != nil {
		return fail("source", err)
	}
	if err := target.Guard(ctx, func() (err error) {
		tgtBuckets, err = target.Buckets(ctx, t.OID, buckets, key)
		return err
	}); err != nil {
		return fail("target", err)
	}
	for _, b := range srcBuckets {
		res.SourceRows += b.Count
	}
	for _, b := range tgtBuckets {
		res.TargetRows += b.Count
	}

	differing, rowsIn := tablediff.DifferingBuckets(srcBuckets, tgtBuckets)
	if len(differing) == 0 {
		res.Status = compareEqual
		return res
	}
	res.Status = compareDifferent
	res.DifferingBuckets = differing
	res.RowsInBuckets = rowsIn
	if rowsIn > maxRows {
		return res
	}

	var srcRows, tgtRows []tablediff.Row
	if err := source.Guard(ctx, func() (err error) {
		srcRows, err = source.Rows(ctx, s.OID, buckets, differing, key)
		return err
	}); err != nil {
		return fail("source", err)
	}
	if err := target.Guard(ctx, func() (err error) {
		tgtRows, err = target.Rows(ctx, t.OID, buckets, differing, key)
		return err
	}); err != nil {
		return fail("target", err)
	}
	res.Rows = tablediff.CompareRows(srcRows, tgtRows)
	res.RowsListed = true
	return res
}

func summarizeCompare(tables []compareTable) compareSummary {
	var s compareSummary
	s.Tables = len(tables)
	for _, t := range tables {
		switch t.Status {
		case compareEqual:
			s.Equal++
		case compareDifferent:
			s.Different++
		case compareOnlyInSource:
			s.OnlyInSource++
		case compareOnlyInTarget:
			s.OnlyInTarget++
		case compareSchemaDiffers:
			s.SchemaDiffers++
		case compareError:
			s.Errors++
		}
		for _, r := range t.Rows {
			switch r.Kind {
			case tablediff.Missing:
				s.MissingRows++
			case tablediff.Extra:
				s.ExtraRows++
			case tablediff.Changed:
				s.ChangedRows++
			}
		}
	}
	return s
}

func writeCompareText(w io.Writer, r compareReport, showOK bool) {
	printed := false
	for _, t := range r.Tables {
		if t.Status == compareEqual && !showOK {
			continue
		}
		printed = true
		status := strings.ToUpper(t.Status)
		name := t.Schema + "." + t.Name
		switch t.Status {
		case compareEqual:
			fmt.Fprintf(w, "%-14s %s rows=%d\n", status, name, t.SourceRows)
		case compareDifferent:
			fmt.Fprintf(w, "%-14s %s source_rows=%d target_rows=%d differing_buckets=%d/%d\n",
				status, name, t.SourceRows, t.TargetRows, len(t.DifferingBuckets), t.Buckets)
			if !t.RowsListed {
				fmt.Fprintf(w, "  %d rows in the differing buckets; raise --max-rows to list them\n",
					t.RowsInBuckets)
				continue
			}
			for _, d := range t.Rows {
				fmt.Fprintf(w, "  %-8s %s\n", strings.ToUpper(d.Kind), d.Key)
			}
		case compareSchemaDiffers, compareError:
			fmt.Fprintf(w, "%-14s %s reason=%q\n", status, name, t.Reason)
		default:
			fmt.Fprintf(w, "%-14s %s\n", status, name)
		}
	}
	if !printed {
		fmt.Fprintln(w, "no differences")
	}
	s := r.Summary
	fmt.Fprintf(w, "tables=%d equal=%d different=%d only_in_source=%d only_in_target=%d schema_differs=%d errors=%d missing_rows=%d extra_rows=%d changed_rows=%d\n",
		s.Tables, s.Equal, s.Different, s.OnlyInSource, s.OnlyInTarget, s.SchemaDiffers, s.Errors,
		s.MissingRows, s.ExtraRows, s.ChangedRows)
}
