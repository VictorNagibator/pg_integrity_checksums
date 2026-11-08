package cli

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"strings"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/config"
	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/pgdb"
)

// Version is overridden at release time with -ldflags -X. The default is
// used by local builds and tests.
var Version = "0.3.0-dev"

// Run dispatches one CLI invocation and returns the process exit code.
// stdout is reserved for data (JSON, diffs, reports); progress and errors go to
// stderr so shell pipelines stay clean.
func Run(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		usage(stderr)
		return 2
	}
	switch args[0] {
	case "help", "-h", "--help":
		usage(stdout)
		return 0
	case "version", "--version":
		fmt.Fprintf(stdout, "pg_integrity_checksums %s\n", Version)
		return 0
	case "init":
		return runInit(args[1:], stdout, stderr)
	case "config":
		return runConfig(args[1:], stdout, stderr)
	case "capture":
		return runCapture(ctx, args[1:], stdout, stderr)
	case "verify":
		return runVerify(ctx, args[1:], stdout, stderr)
	case "diff":
		return runDiff(ctx, args[1:], stdout, stderr)
	case "show":
		return runShow(ctx, args[1:], stdout, stderr)
	case "update":
		return runUpdate(ctx, args[1:], stdout, stderr)
	case "doctor":
		return runDoctor(ctx, args[1:], stdout, stderr)
	case "completion":
		return runCompletion(args[1:], stdout, stderr)
	default:
		fmt.Fprintf(stderr, "error: unknown command %q\n", args[0])
		usage(stderr)
		return 2
	}
}

// newFlagSet creates a flag set that reports errors to stderr instead of
// exiting, because Run must return an exit code to main.
func newFlagSet(name string, stderr io.Writer) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(stderr)
	fs.Usage = func() {
		fmt.Fprintf(stderr, "usage: pg_integrity_checksums %s [options]\n", name)
		fs.PrintDefaults()
	}
	return fs
}

// parseFlags parses a command's flags and rejects positional arguments.
// It returns -1 when parsing succeeded; any other value is the final exit code
// (0 for --help, 2 for a usage error).
func parseFlags(fs *flag.FlagSet, args []string, stderr io.Writer) int {
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintf(stderr, "error: unexpected arguments: %s\n", strings.Join(fs.Args(), " "))
		fs.Usage()
		return 2
	}
	return -1
}

// progress returns nil in quiet mode; Snapshot checks for nil before calling
// it, so the rest of the code does not need quiet-mode branches.
func progress(quiet bool, stderr io.Writer) func(string) {
	if quiet {
		return nil
	}
	return func(s string) {
		fmt.Fprintf(stderr, "checking %s\n", s)
	}
}

// runCapture discovers objects, computes checksums in one snapshot and writes
// a baseline. The scope recorded here is what verify and update reuse later.
func runCapture(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("capture", stderr)
	dsn := fs.String("db", "", "connection string")
	dsnAlias := fs.String("dsn", "", "connection string (alias for --db)")
	configPath := fs.String("config", "", "config file (default: .pgics.yaml)")
	profileName := fs.String("profile", "", "config profile")
	connectTimeout := fs.Duration("connect-timeout", 0, "connect timeout")
	applicationName := fs.String("application-name", "", "application_name for pg_stat_activity")
	output := fs.String("output", "baseline.json", "output file, or - for stdout")
	kindValue := fs.String("kind", baseline.KindLogical, "logical or physical")
	var schemas, excludeSchemas, tables, excludeTables stringList
	fs.Var(&schemas, "schema", "include schema (repeatable or comma-separated; globs allowed)")
	fs.Var(&excludeSchemas, "exclude-schema", "exclude schema (repeatable; globs allowed)")
	fs.Var(&tables, "table", "include schema.table (repeatable; globs allowed)")
	fs.Var(&excludeTables, "exclude-table", "exclude schema.table (repeatable; globs allowed)")
	includeSystem := fs.Bool("include-system", false, "include system schemas")
	noIndexes := fs.Bool("no-indexes", false, "do not capture indexes")
	workers := fs.Int("workers", 0, "pg_integrity_checksums.workers")
	timeout := fs.Duration("timeout", 0, "statement timeout, e.g. 5m")
	strict := fs.Bool("strict", false, "fail when an object cannot be checksummed")
	quiet := fs.Bool("quiet", false, "suppress progress")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}

	profile, cfgPath, err := loadProfile(*configPath, *profileName)
	if err != nil {
		return commandError(stderr, err)
	}
	set := flagSet(fs)
	base := config.BaseDir(cfgPath)

	if !set["db"] && !set["dsn"] {
		if value := profile.ResolveDSN(); value != "" {
			*dsn = value
		}
	}
	if effective, err := resolveDSN(*dsn, *dsnAlias); err != nil {
		return commandError(stderr, err)
	} else {
		*dsn = effective
	}
	if !set["kind"] && profile.Kind != "" {
		*kindValue = profile.Kind
	}
	if !set["output"] && profile.Output != "" {
		*output = profilePath(base, profile.Output)
	}
	if !set["schema"] {
		schemas = append(schemas, profile.Schemas...)
	}
	if !set["exclude-schema"] {
		excludeSchemas = append(excludeSchemas, profile.ExcludeSchemas...)
	}
	if !set["table"] {
		tables = append(tables, profile.Tables...)
	}
	if !set["exclude-table"] {
		excludeTables = append(excludeTables, profile.ExcludeTables...)
	}
	*includeSystem = boolValue(set, "include-system", *includeSystem, profile.IncludeSystem)
	if !set["no-indexes"] && profile.IncludeIndexes != nil {
		*noIndexes = !*profile.IncludeIndexes
	}
	*workers = intValue(set, "workers", *workers, profile.Workers)
	if !set["timeout"] && profile.Timeout != "" {
		parsed, err := profileDuration(profile.Timeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*timeout = parsed
	}
	*strict = boolValue(set, "strict", *strict, profile.Strict)
	if !set["connect-timeout"] && profile.ConnectTimeout != "" {
		parsed, err := profileDuration(profile.ConnectTimeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*connectTimeout = parsed
	}
	if !set["application-name"] && profile.ApplicationName != "" {
		*applicationName = profile.ApplicationName
	}
	if *connectTimeout < 0 {
		return commandError(stderr, errors.New("--connect-timeout must be >= 0"))
	}

	kind, err := parseKind(*kindValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if *workers < 0 {
		return commandError(stderr, errors.New("--workers must be >= 0"))
	}
	if *timeout < 0 {
		return commandError(stderr, errors.New("--timeout must be >= 0"))
	}

	scope := baseline.NewScope()
	scope.Schemas = append(scope.Schemas, schemas...)
	scope.ExcludeSchemas = append(scope.ExcludeSchemas, excludeSchemas...)
	scope.Tables = append(scope.Tables, tables...)
	scope.ExcludeTables = append(scope.ExcludeTables, excludeTables...)
	scope.IncludeSystem = *includeSystem
	scope.IncludeIndexes = !*noIndexes

	db, err := pgdb.OpenWithOptions(ctx, *dsn, pgdb.ConnectOptions{
		Timeout:         *connectTimeout,
		ApplicationName: *applicationName,
	})
	if err != nil {
		return commandError(stderr, err)
	}
	defer db.Close()

	b, err := pgdb.Snapshot(ctx, db, pgdb.Options{
		ToolVersion: Version,
		Kind:        kind,
		Scope:       scope,
		Workers:     *workers,
		Timeout:     *timeout,
		Strict:      *strict,
		Progress:    progress(*quiet, stderr),
	})
	if err != nil {
		return commandError(stderr, err)
	}
	if err := baseline.Save(*output, b, stdout); err != nil {
		return commandError(stderr, err)
	}
	if !*quiet {
		skipped := 0
		for _, o := range b.Objects {
			if o.Status == baseline.StatusSkipped {
				skipped++
			}
		}
		where := *output
		if where == "-" {
			where = "stdout"
		}
		fmt.Fprintf(stderr, "captured %d objects (%d skipped) to %s\n", len(b.Objects), skipped, where)
	}
	return 0
}

// runVerify recomputes the baseline scope in the current database, compares it
// with the stored objects and maps the chosen --fail-on policy to an exit code.
func runVerify(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("verify", stderr)
	dsn := fs.String("db", "", "connection string")
	dsnAlias := fs.String("dsn", "", "connection string (alias for --db)")
	configPath := fs.String("config", "", "config file (default: .pgics.yaml)")
	profileName := fs.String("profile", "", "config profile")
	connectTimeout := fs.Duration("connect-timeout", 0, "connect timeout")
	applicationName := fs.String("application-name", "", "application_name for pg_stat_activity")
	baselinePath := fs.String("baseline", "", "baseline file")
	formatValue := fs.String("format", "text", "text, json or junit")
	outputPath := fs.String("output", "-", "report file, or - for stdout")
	failOnValue := fs.String("fail-on", "any", "changed, schema-changed, new, missing, any or none")
	showOK := fs.Bool("show-ok", false, "include unchanged objects")
	allowVersion := fs.Bool("allow-version-mismatch", false, "allow a different PostgreSQL major")
	changedSince := fs.Bool("changed-since", false, "reuse unchanged objects from the baseline (logical only; heuristic)")
	workers := fs.Int("workers", 0, "pg_integrity_checksums.workers")
	timeout := fs.Duration("timeout", 0, "statement timeout")
	quiet := fs.Bool("quiet", false, "suppress progress")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}

	profile, cfgPath, err := loadProfile(*configPath, *profileName)
	if err != nil {
		return commandError(stderr, err)
	}
	set := flagSet(fs)
	base := config.BaseDir(cfgPath)
	if !set["db"] && !set["dsn"] {
		if value := profile.ResolveDSN(); value != "" {
			*dsn = value
		}
	}
	if effective, err := resolveDSN(*dsn, *dsnAlias); err != nil {
		return commandError(stderr, err)
	} else {
		*dsn = effective
	}
	if !set["baseline"] && profile.Baseline != "" {
		*baselinePath = profilePath(base, profile.Baseline)
	}
	if !set["format"] && profile.Format != "" {
		*formatValue = profile.Format
	}
	if !set["fail-on"] && profile.FailOn != "" {
		*failOnValue = profile.FailOn
	}
	*showOK = boolValue(set, "show-ok", *showOK, profile.ShowOK)
	*changedSince = boolValue(set, "changed-since", *changedSince, profile.ChangedSince)
	*workers = intValue(set, "workers", *workers, profile.Workers)
	if !set["timeout"] && profile.Timeout != "" {
		parsed, err := profileDuration(profile.Timeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*timeout = parsed
	}
	if !set["connect-timeout"] && profile.ConnectTimeout != "" {
		parsed, err := profileDuration(profile.ConnectTimeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*connectTimeout = parsed
	}
	if !set["application-name"] && profile.ApplicationName != "" {
		*applicationName = profile.ApplicationName
	}
	if *baselinePath == "" {
		return commandError(stderr, errors.New("--baseline is required (or set baseline in the config profile)"))
	}
	format, err := parseFormat(*formatValue)
	if err != nil {
		return commandError(stderr, err)
	}
	failOn, err := parseFailOn(*failOnValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if *workers < 0 || *timeout < 0 || *connectTimeout < 0 {
		return commandError(stderr, errors.New("--workers, --timeout and --connect-timeout must be >= 0"))
	}

	if sameFilePath(*outputPath, *baselinePath) {
		return commandError(stderr, errors.New("--output must not overwrite the baseline file"))
	}
	out, closeOutput, err := outputWriter(stdout, *outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()

	oldB, err := baseline.Load(*baselinePath)
	if err != nil {
		fmt.Fprintf(stderr, "error: invalid baseline: %v\n", err)
		return 3
	}
	if *changedSince && oldB.Kind != baseline.KindLogical {
		return commandError(stderr, errors.New("--changed-since is supported only for logical baselines"))
	}
	prior := make(map[string]baseline.Object, len(oldB.Objects))
	for _, o := range oldB.Objects {
		prior[o.Key()] = o
	}

	db, err := pgdb.OpenWithOptions(ctx, *dsn, pgdb.ConnectOptions{
		Timeout:         *connectTimeout,
		ApplicationName: *applicationName,
	})
	if err != nil {
		return commandError(stderr, err)
	}
	defer db.Close()
	if *changedSince && !*quiet {
		fmt.Fprintln(stderr, "warning: --changed-since uses table statistics as a heuristic; use a full verify when exactness is required")
	}

	reused := 0
	curB, err := pgdb.Snapshot(ctx, db, pgdb.Options{
		ToolVersion: Version,
		Kind:        oldB.Kind,
		Scope:       oldB.Scope,
		Workers:     *workers,
		Timeout:     *timeout,
		ChangedOnly: *changedSince,
		Prior:       prior,
		Reused:      &reused,
		Progress:    progress(*quiet, stderr),
	})
	if err != nil {
		return commandError(stderr, err)
	}

	if err := checkCompatibility(oldB, curB, *allowVersion); err != nil {
		fmt.Fprintf(stderr, "error: incompatible baseline: %v\n", err)
		return 3
	}
	if *changedSince && !*quiet {
		fmt.Fprintf(stderr, "changed-since: reused %d objects, checksummed %d\n", reused, len(curB.Objects)-reused)
	}
	if oldB.ExtensionVersion != curB.ExtensionVersion {
		fmt.Fprintf(stderr, "warning: baseline extension %s, current extension %s\n",
			oldB.ExtensionVersion, curB.ExtensionVersion)
	}
	if oldB.Database != curB.Database {
		fmt.Fprintf(stderr, "warning: baseline database %q, current database %q\n",
			oldB.Database, curB.Database)
	}

	changes, summary := baseline.Compare(oldB.Objects, curB.Objects)
	if format == "json" {
		if err := writeVerifyJSON(out, oldB, changes, summary, *showOK); err != nil {
			return commandError(stderr, err)
		}
	} else if format == "junit" {
		if err := writeVerifyJUnit(out, oldB, changes, summary, *showOK); err != nil {
			return commandError(stderr, err)
		}
	} else {
		writeChangesText(out, changes, *showOK)
		writeSummary(out, summary)
	}

	if baseline.FailOn(summary, failOn) {
		return 1
	}
	return 0
}

// checkCompatibility refuses comparisons that cannot be meaningful:
// different checksum kinds, a different PostgreSQL major by default, and a
// physical baseline from another cluster or database.
func checkCompatibility(oldB, curB *baseline.Baseline, allowVersion bool) error {
	if oldB.Kind != curB.Kind {
		return fmt.Errorf("baseline kind %q, current kind %q", oldB.Kind, curB.Kind)
	}
	if !allowVersion && oldB.PostgresVersion/10000 != curB.PostgresVersion/10000 {
		return fmt.Errorf("baseline PostgreSQL %d, current PostgreSQL %d",
			oldB.PostgresVersion, curB.PostgresVersion)
	}
	if oldB.Kind == baseline.KindPhysical {
		if oldB.SystemIdentifier != curB.SystemIdentifier {
			return errors.New("physical baseline belongs to another PostgreSQL cluster")
		}
		if oldB.DatabaseOID != curB.DatabaseOID {
			return errors.New("physical baseline belongs to another database")
		}
	}
	return nil
}

// runDiff compares two saved baselines without a database connection.
// Flags are parsed manually so they may appear before or after the file names
// (the standard flag package stops at the first positional argument).
func runDiff(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("diff", stderr)
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: pg_integrity_checksums diff [--format text|json] [--show-ok] [--no-fail] OLD NEW")
	}
	formatValue := "text"
	outputPath := "-"
	showOK := false
	noFail := false
	files := make([]string, 0, 2)

	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "-h" || a == "--help":
			fs.Usage()
			return 0
		case a == "--format":
			i++
			if i >= len(args) {
				return commandError(stderr, errors.New("--format requires a value"))
			}
			formatValue = args[i]
		case strings.HasPrefix(a, "--format="):
			formatValue = strings.TrimPrefix(a, "--format=")
		case a == "--output":
			i++
			if i >= len(args) {
				return commandError(stderr, errors.New("--output requires a value"))
			}
			outputPath = args[i]
		case strings.HasPrefix(a, "--output="):
			outputPath = strings.TrimPrefix(a, "--output=")
		case a == "--show-ok":
			showOK = true
		case a == "--no-fail":
			noFail = true
		case a == "--":
			files = append(files, args[i+1:]...)
			i = len(args)
		case strings.HasPrefix(a, "-"):
			return commandError(stderr, fmt.Errorf("unknown diff flag %q", a))
		default:
			files = append(files, a)
		}
	}
	if len(files) != 2 {
		fmt.Fprintln(stderr, "error: diff requires OLD and NEW baseline files")
		fs.Usage()
		return 2
	}
	format, err := parseFormat(formatValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if sameFilePath(outputPath, files[0]) || sameFilePath(outputPath, files[1]) {
		return commandError(stderr, errors.New("--output must not overwrite a baseline input"))
	}
	out, closeOutput, err := outputWriter(stdout, outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()
	oldB, err := baseline.Load(files[0])
	if err != nil {
		fmt.Fprintf(stderr, "error: invalid old baseline: %v\n", err)
		return 3
	}
	newB, err := baseline.Load(files[1])
	if err != nil {
		fmt.Fprintf(stderr, "error: invalid new baseline: %v\n", err)
		return 3
	}
	if oldB.Kind != newB.Kind {
		fmt.Fprintf(stderr, "error: baseline kinds differ: %s and %s\n", oldB.Kind, newB.Kind)
		return 3
	}
	changes, summary := baseline.Compare(oldB.Objects, newB.Objects)
	if format == "json" {
		if err := writeDiffJSON(out, oldB, newB, changes, summary, showOK); err != nil {
			return commandError(stderr, err)
		}
	} else if format == "junit" {
		if err := writeDiffJUnit(out, oldB, newB, changes, summary, showOK); err != nil {
			return commandError(stderr, err)
		}
	} else {
		writeChangesText(out, changes, showOK)
		writeSummary(out, summary)
	}
	if !noFail && summary.HasChanges() {
		return 1
	}
	return 0
}

// runShow prints baseline metadata or the full stored document. Like diff,
// it accepts flags on either side of the filename.
func runShow(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	_ = ctx
	fs := newFlagSet("show", stderr)
	fs.Usage = func() {
		fmt.Fprintln(stderr, "usage: pg_integrity_checksums show [--format text|json] [--objects] FILE")
	}
	formatValue := "text"
	outputPath := "-"
	showObjects := false
	files := make([]string, 0, 1)

	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "-h" || a == "--help":
			fs.Usage()
			return 0
		case a == "--format":
			i++
			if i >= len(args) {
				return commandError(stderr, errors.New("--format requires a value"))
			}
			formatValue = args[i]
		case strings.HasPrefix(a, "--format="):
			formatValue = strings.TrimPrefix(a, "--format=")
		case a == "--output":
			i++
			if i >= len(args) {
				return commandError(stderr, errors.New("--output requires a value"))
			}
			outputPath = args[i]
		case strings.HasPrefix(a, "--output="):
			outputPath = strings.TrimPrefix(a, "--output=")
		case a == "--objects":
			showObjects = true
		case a == "--":
			files = append(files, args[i+1:]...)
			i = len(args)
		case strings.HasPrefix(a, "-") && a != "-":
			return commandError(stderr, fmt.Errorf("unknown show flag %q", a))
		default:
			files = append(files, a)
		}
	}
	if len(files) != 1 {
		fmt.Fprintln(stderr, "error: show requires one baseline file")
		fs.Usage()
		return 2
	}
	format, err := parseFormat(formatValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if format == "junit" {
		return commandError(stderr, errors.New("show does not support --format junit"))
	}
	if sameFilePath(outputPath, files[0]) {
		return commandError(stderr, errors.New("--output must not overwrite the baseline input"))
	}
	out, closeOutput, err := outputWriter(stdout, outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()
	stdout = out
	b, err := baseline.Load(files[0])
	if err != nil {
		fmt.Fprintf(stderr, "error: invalid baseline: %v\n", err)
		return 3
	}
	if format == "json" {
		if err := encodeJSON(stdout, b); err != nil {
			return commandError(stderr, err)
		}
		return 0
	}
	ok, skipped := 0, 0
	for _, o := range b.Objects {
		if o.Status == baseline.StatusOK {
			ok++
		} else {
			skipped++
		}
	}
	fmt.Fprintf(stdout, "format: %s\n", b.Format)
	if b.BaselineID != "" {
		fmt.Fprintf(stdout, "baseline_id: %s\n", b.BaselineID)
	}
	fmt.Fprintf(stdout, "created_at: %s\n", b.CreatedAt)
	fmt.Fprintf(stdout, "kind: %s\n", b.Kind)
	fmt.Fprintf(stdout, "extension: %s\n", b.ExtensionVersion)
	fmt.Fprintf(stdout, "postgres: %d\n", b.PostgresVersion)
	fmt.Fprintf(stdout, "database: %s\n", b.Database)
	if b.Kind == baseline.KindPhysical {
		fmt.Fprintf(stdout, "system_identifier: %s\n", b.SystemIdentifier)
		fmt.Fprintf(stdout, "database_oid: %d\n", b.DatabaseOID)
	}
	fmt.Fprintf(stdout, "objects: %d (ok=%d skipped=%d)\n", len(b.Objects), ok, skipped)
	if showObjects {
		for _, o := range b.Objects {
			line := fmt.Sprintf("%-5s %s.%s", o.Kind, o.Schema, o.Name)
			if o.Kind == baseline.ObjectIndex && o.TableName != "" {
				line += fmt.Sprintf(" on %s.%s", o.TableSchema, o.TableName)
			}
			line += fmt.Sprintf(" %s", o.Status)
			if o.Checksum != "" {
				line += " " + o.Checksum
			}
			if o.Reason != "" {
				line += " reason=" + o.Reason
			}
			fmt.Fprintln(stdout, line)
		}
	}
	return 0
}

// runUpdate replaces a baseline after showing the diff. --yes is mandatory
// unless --dry-run is used: a corrupted database must never silently overwrite
// a known-good baseline.
func runUpdate(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("update", stderr)
	dsn := fs.String("db", "", "connection string")
	dsnAlias := fs.String("dsn", "", "connection string (alias for --db)")
	configPath := fs.String("config", "", "config file (default: .pgics.yaml)")
	profileName := fs.String("profile", "", "config profile")
	connectTimeout := fs.Duration("connect-timeout", 0, "connect timeout")
	applicationName := fs.String("application-name", "", "application_name for pg_stat_activity")
	baselinePath := fs.String("baseline", "", "baseline file")
	yes := fs.Bool("yes", false, "confirm replacing the baseline")
	dryRun := fs.Bool("dry-run", false, "print the diff without writing")
	formatValue := fs.String("format", "text", "text, json or junit")
	outputPath := fs.String("output", "-", "diff report file, or - for stdout")
	showOK := fs.Bool("show-ok", false, "include unchanged objects")
	workers := fs.Int("workers", 0, "pg_integrity_checksums.workers")
	timeout := fs.Duration("timeout", 0, "statement timeout")
	quiet := fs.Bool("quiet", false, "suppress progress")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}

	profile, cfgPath, err := loadProfile(*configPath, *profileName)
	if err != nil {
		return commandError(stderr, err)
	}
	set := flagSet(fs)
	base := config.BaseDir(cfgPath)
	if !set["db"] && !set["dsn"] {
		if value := profile.ResolveDSN(); value != "" {
			*dsn = value
		}
	}
	if effective, err := resolveDSN(*dsn, *dsnAlias); err != nil {
		return commandError(stderr, err)
	} else {
		*dsn = effective
	}
	if !set["baseline"] && profile.Baseline != "" {
		*baselinePath = profilePath(base, profile.Baseline)
	}
	if !set["format"] && profile.Format != "" {
		*formatValue = profile.Format
	}
	*showOK = boolValue(set, "show-ok", *showOK, profile.ShowOK)
	*workers = intValue(set, "workers", *workers, profile.Workers)
	if !set["timeout"] && profile.Timeout != "" {
		parsed, err := profileDuration(profile.Timeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*timeout = parsed
	}
	if !set["connect-timeout"] && profile.ConnectTimeout != "" {
		parsed, err := profileDuration(profile.ConnectTimeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*connectTimeout = parsed
	}
	if !set["application-name"] && profile.ApplicationName != "" {
		*applicationName = profile.ApplicationName
	}
	if *baselinePath == "" {
		return commandError(stderr, errors.New("--baseline is required (or set baseline in the config profile)"))
	}
	format, err := parseFormat(*formatValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if *workers < 0 || *timeout < 0 || *connectTimeout < 0 {
		return commandError(stderr, errors.New("--workers, --timeout and --connect-timeout must be >= 0"))
	}
	if !*yes && !*dryRun {
		fmt.Fprintln(stderr, "error: refusing to replace baseline without --yes (use --dry-run to preview)")
		return 4
	}
	if sameFilePath(*outputPath, *baselinePath) {
		return commandError(stderr, errors.New("--output must not overwrite the baseline file"))
	}
	out, closeOutput, err := outputWriter(stdout, *outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()

	oldB, err := baseline.Load(*baselinePath)
	if err != nil {
		fmt.Fprintf(stderr, "error: invalid baseline: %v\n", err)
		return 3
	}
	db, err := pgdb.OpenWithOptions(ctx, *dsn, pgdb.ConnectOptions{
		Timeout:         *connectTimeout,
		ApplicationName: *applicationName,
	})
	if err != nil {
		return commandError(stderr, err)
	}
	defer db.Close()

	newB, err := pgdb.Snapshot(ctx, db, pgdb.Options{
		ToolVersion: Version,
		Kind:        oldB.Kind,
		Scope:       oldB.Scope,
		Workers:     *workers,
		Timeout:     *timeout,
		Progress:    progress(*quiet, stderr),
	})
	if err != nil {
		return commandError(stderr, err)
	}
	if err := checkCompatibility(oldB, newB, false); err != nil {
		fmt.Fprintf(stderr, "error: incompatible baseline: %v\n", err)
		return 3
	}
	changes, summary := baseline.Compare(oldB.Objects, newB.Objects)
	if format == "json" {
		if err := writeDiffJSON(out, oldB, newB, changes, summary, *showOK); err != nil {
			return commandError(stderr, err)
		}
	} else if format == "junit" {
		if err := writeDiffJUnit(out, oldB, newB, changes, summary, *showOK); err != nil {
			return commandError(stderr, err)
		}
	} else {
		writeChangesText(out, changes, *showOK)
		writeSummary(out, summary)
	}
	if *dryRun {
		if !*quiet {
			fmt.Fprintln(stderr, "dry run: baseline not written")
		}
		return 0
	}
	if err := baseline.Save(*baselinePath, newB, stdout); err != nil {
		return commandError(stderr, err)
	}
	if !*quiet {
		fmt.Fprintf(stderr, "updated %s with %d objects\n", *baselinePath, len(newB.Objects))
	}
	return 0
}

// runDoctor performs a cheap environment check: extension version, server,
// current user and the number of relations that the default scope discovers.
// It deliberately does not compute checksums.
func runDoctor(ctx context.Context, args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("doctor", stderr)
	dsn := fs.String("db", "", "connection string")
	dsnAlias := fs.String("dsn", "", "connection string (alias for --db)")
	configPath := fs.String("config", "", "config file (default: .pgics.yaml)")
	profileName := fs.String("profile", "", "config profile")
	connectTimeout := fs.Duration("connect-timeout", 0, "connect timeout")
	applicationName := fs.String("application-name", "", "application_name for pg_stat_activity")
	timeout := fs.Duration("timeout", 0, "statement timeout")
	verbose := fs.Bool("verbose", false, "list every discovered object")
	strict := fs.Bool("strict", false, "exit 1 when a preflight problem is found")
	formatValue := fs.String("format", "text", "text or json")
	outputPath := fs.String("output", "-", "report file, or - for stdout")
	quiet := fs.Bool("quiet", false, "suppress progress")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}

	profile, _, err := loadProfile(*configPath, *profileName)
	if err != nil {
		return commandError(stderr, err)
	}
	set := flagSet(fs)
	if !set["db"] && !set["dsn"] {
		if value := profile.ResolveDSN(); value != "" {
			*dsn = value
		}
	}
	if effective, err := resolveDSN(*dsn, *dsnAlias); err != nil {
		return commandError(stderr, err)
	} else {
		*dsn = effective
	}
	if !set["timeout"] && profile.Timeout != "" {
		parsed, err := profileDuration(profile.Timeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*timeout = parsed
	}
	if !set["connect-timeout"] && profile.ConnectTimeout != "" {
		parsed, err := profileDuration(profile.ConnectTimeout)
		if err != nil {
			return commandError(stderr, err)
		}
		*connectTimeout = parsed
	}
	if !set["application-name"] && profile.ApplicationName != "" {
		*applicationName = profile.ApplicationName
	}
	*verbose = boolValue(set, "verbose", *verbose, profile.Verbose)
	if !set["format"] && profile.Format != "" {
		*formatValue = profile.Format
	}
	format, err := parseFormat(*formatValue)
	if err != nil {
		return commandError(stderr, err)
	}
	if format == "junit" {
		return commandError(stderr, errors.New("doctor does not support --format junit"))
	}
	if *timeout < 0 || *connectTimeout < 0 {
		return commandError(stderr, errors.New("--timeout and --connect-timeout must be >= 0"))
	}

	out, closeOutput, err := outputWriter(stdout, *outputPath)
	if err != nil {
		return commandError(stderr, err)
	}
	defer closeOutput()
	stdout = out

	db, err := pgdb.OpenWithOptions(ctx, *dsn, pgdb.ConnectOptions{
		Timeout:         *connectTimeout,
		ApplicationName: *applicationName,
	})
	if err != nil {
		return commandError(stderr, err)
	}
	defer db.Close()
	if *timeout > 0 {
		ms := timeout.Milliseconds()
		if ms < 1 {
			ms = 1
		}
		if _, err := db.ExecContext(ctx,
			"SELECT set_config('statement_timeout', $1, false)",
			fmt.Sprintf("%dms", ms)); err != nil {
			return commandError(stderr, err)
		}
	}
	info, err := pgdb.ReadInfo(ctx, db)
	if err != nil {
		return commandError(stderr, err)
	}
	scope := baseline.NewScope()
	scope.Schemas = append(scope.Schemas, profile.Schemas...)
	scope.ExcludeSchemas = append(scope.ExcludeSchemas, profile.ExcludeSchemas...)
	scope.Tables = append(scope.Tables, profile.Tables...)
	scope.ExcludeTables = append(scope.ExcludeTables, profile.ExcludeTables...)
	if profile.IncludeSystem != nil {
		scope.IncludeSystem = *profile.IncludeSystem
	}
	if profile.IncludeIndexes != nil {
		scope.IncludeIndexes = *profile.IncludeIndexes
	}
	items, err := pgdb.Inspect(ctx, db, scope)
	if err != nil {
		return commandError(stderr, err)
	}
	problems := 0
	for _, item := range items {
		if item.Reason != "" {
			problems++
		}
	}
	if !*quiet {
		fmt.Fprintln(stderr, "checking extension and scope")
	}
	if format == "json" {
		objects := make([]map[string]any, 0, len(items))
		for _, item := range items {
			objects = append(objects, map[string]any{
				"kind": item.Kind, "schema": item.Schema, "name": item.Name,
				"table_schema": item.TableSchema, "table_name": item.TableName,
				"can_select": item.CanSelect, "rls": item.RLS, "reason": item.Reason,
			})
		}
		if err := encodeJSON(stdout, map[string]any{
			"extension": info.ExtensionVersion, "postgres": info.PostgresVersion,
			"database": info.Database, "current_user": info.CurrentUser,
			"objects": objects, "preflight_problems": problems,
		}); err != nil {
			return commandError(stderr, err)
		}
	} else {
		fmt.Fprintf(stdout, "extension: %s\n", info.ExtensionVersion)
		fmt.Fprintf(stdout, "postgres: %d\n", info.PostgresVersion)
		fmt.Fprintf(stdout, "database: %s\n", info.Database)
		fmt.Fprintf(stdout, "current_user: %s\n", info.CurrentUser)
		fmt.Fprintf(stdout, "objects_in_default_scope: %d\n", len(items))
		fmt.Fprintf(stdout, "preflight_problems: %d\n", problems)
		if *verbose {
			for _, item := range items {
				line := fmt.Sprintf("%-5s %s.%s", item.Kind, item.Schema, item.Name)
				if item.Reason != "" {
					line += " WARN " + item.Reason
				} else {
					line += " OK"
				}
				fmt.Fprintln(stdout, line)
			}
		}
	}
	if *strict && problems > 0 {
		return 1
	}
	return 0
}
