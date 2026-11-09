// Package baseline defines the versioned baseline document shared by the
// pg_integrity_checksums CLI commands.
package baseline

import (
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"time"
)

const (
	// FormatV1 is the numeric FNV-1a 32-bit baseline format.
	FormatV1 = "pg_integrity_checksums.baseline/v1"
	// FormatV2 adds an explicit checksum algorithm field.
	FormatV2 = "pg_integrity_checksums.baseline/v2"

	// HashFNV32 and HashFNV64 are the supported baseline algorithms.
	HashFNV32 = "fnv32"
	HashFNV64 = "fnv64"

	// KindLogical is stable across physical reorganization and is the default.
	KindLogical = "logical"
	// KindPhysical describes bytes on one specific cluster.
	KindPhysical = "physical"

	// StatusOK means the object was checksummed successfully.
	StatusOK = "ok"
	// StatusSkipped means the SQL function returned NULL, normally because the
	// object or its access method is not supported by that checksum kind.
	StatusSkipped = "skipped"

	// ObjectTable covers ordinary, partitioned and materialized tables.
	ObjectTable = "table"
	// ObjectIndex covers indexes, including partitioned parent indexes.
	ObjectIndex = "index"
)

// checksumRE keeps checksums as strings in JSON (bigint values are not safe
// as JavaScript numbers) while still rejecting malformed values.
var checksumRE = regexp.MustCompile("^-?[0-9]+$")
var schemaHashRE = regexp.MustCompile("^sha256:[0-9a-f]{64}$")

// Scope is the discovery filter stored in a baseline. Verify reuses it so the
// same set of objects is compared instead of silently widening the scan.
type Scope struct {
	// Empty Schemas means all non-system schemas; non-empty is an allow-list.
	Schemas        []string `json:"schemas"`
	ExcludeSchemas []string `json:"exclude_schemas"`
	// Table filters use schema.table names. For indexes they match the table
	// the index belongs to, not the index name.
	Tables        []string `json:"tables"`
	ExcludeTables []string `json:"exclude_tables"`

	IncludeSystem  bool `json:"include_system"`
	IncludeIndexes bool `json:"include_indexes"`
}

// Object is one table or index in a baseline. Identity is name based rather
// than OID based so a baseline survives a dump/restore into another database.
type Object struct {
	Kind   string `json:"kind"`
	Schema string `json:"schema"`
	Name   string `json:"name"`

	// Indexes record their table so a diff can say what the index belongs to.
	TableSchema string `json:"table_schema,omitempty"`
	TableName   string `json:"table_name,omitempty"`

	Status   string `json:"status"`
	Checksum string `json:"checksum,omitempty"`
	// SchemaHash is a SHA-256 of the portable schema definition: columns,
	// constraints, partition key, reloptions and (for indexes) indexdef.
	SchemaHash string `json:"schema_hash,omitempty"`
	// Stats is table-only change-detection data used by --changed-since.
	Stats  *TableStats `json:"stats,omitempty"`
	Reason string      `json:"reason,omitempty"`
}

// TableStats is the coarse change counter set from pg_stat_all_tables plus the
// current relfilenode. It lets verify skip a table that has not been written
// since the baseline, but it is a heuristic, not a transactional guarantee.
type TableStats struct {
	Relfilenode int64 `json:"relfilenode"`
	Inserted    int64 `json:"inserted"`
	Updated     int64 `json:"updated"`
	Deleted     int64 `json:"deleted"`
}

// Baseline is the versioned JSON document captured by the CLI. It deliberately
// contains no OIDs and no data values, only object identities and checksums.
type Baseline struct {
	Format     string `json:"format"`
	BaselineID string `json:"baseline_id,omitempty"`
	CreatedAt  string `json:"created_at"`

	ToolVersion      string `json:"tool_version"`
	Kind             string `json:"kind"`
	Hash             string `json:"hash,omitempty"`
	ExtensionVersion string `json:"extension_version"`
	PostgresVersion  int    `json:"postgres_version"`
	Database         string `json:"database"`

	// Physical baselines are tied to one cluster and one database. Logical
	// baselines leave these fields empty.
	SystemIdentifier string `json:"system_identifier,omitempty"`
	DatabaseOID      int64  `json:"database_oid,omitempty"`

	Scope   Scope    `json:"scope"`
	Objects []Object `json:"objects"`
}

// NewScope returns the default discovery scope: every non-system schema,
// including indexes. Empty slices (not nil) keep the JSON schema happy and
// make captured baselines deterministic.
func NewScope() Scope {
	return Scope{
		Schemas:        []string{},
		ExcludeSchemas: []string{},
		Tables:         []string{},
		ExcludeTables:  []string{},
		IncludeIndexes: true,
	}
}

// Clone deep-copies the filter slices so callers cannot mutate a baseline's
// recorded scope through a shared backing array.
func (s Scope) Clone() Scope {
	clone := s
	clone.Schemas = append([]string{}, s.Schemas...)
	clone.ExcludeSchemas = append([]string{}, s.ExcludeSchemas...)
	clone.Tables = append([]string{}, s.Tables...)
	clone.ExcludeTables = append([]string{}, s.ExcludeTables...)
	return clone
}

// normalizeScope converts nil slices loaded from old baselines into empty
// arrays. This keeps Load tolerant while Save always emits schema-valid JSON.
func normalizeScope(s Scope) Scope {
	if s.Schemas == nil {
		s.Schemas = []string{}
	}
	if s.ExcludeSchemas == nil {
		s.ExcludeSchemas = []string{}
	}
	if s.Tables == nil {
		s.Tables = []string{}
	}
	if s.ExcludeTables == nil {
		s.ExcludeTables = []string{}
	}
	return s
}

// Key is the stable map key used by Compare. Kind is included because a table
// and an index can legally have the same schema-qualified name.
func (o Object) Key() string {
	return o.Kind + ":" + o.Schema + "." + o.Name
}

// Identity is the human-readable name used in reports.
func (o Object) Identity() string {
	return o.Schema + "." + o.Name
}

// SortObjects gives baselines deterministic order, which in turn makes
// BaselineID stable for equal content.
func SortObjects(objects []Object) {
	sort.Slice(objects, func(i, j int) bool {
		if objects[i].Kind != objects[j].Kind {
			return objects[i].Kind < objects[j].Kind
		}
		if objects[i].Schema != objects[j].Schema {
			return objects[i].Schema < objects[j].Schema
		}
		return objects[i].Name < objects[j].Name
	})
}

// New builds a complete baseline in memory. The caller still has to write it
// with Save; New never touches the filesystem.
func New(toolVersion, kind, extensionVersion string, postgresVersion int, database string, scope Scope, objects []Object) *Baseline {
	return NewWithHash(toolVersion, kind, "", extensionVersion, postgresVersion, database, scope, objects)
}

// NewWithHash builds a v2 baseline when hash is non-empty. An empty hash keeps
// the v1 behaviour for callers that still emit numeric FNV-1a 32-bit baselines.
func NewWithHash(toolVersion, kind, hash, extensionVersion string, postgresVersion int, database string, scope Scope, objects []Object) *Baseline {
	SortObjects(objects)
	format := FormatV1
	if hash != "" {
		format = FormatV2
	}
	b := &Baseline{
		Format:           format,
		CreatedAt:        time.Now().UTC().Format(time.RFC3339),
		ToolVersion:      toolVersion,
		Kind:             kind,
		Hash:             hash,
		ExtensionVersion: extensionVersion,
		PostgresVersion:  postgresVersion,
		Database:         database,
		Scope:            normalizeScope(scope.Clone()),
		Objects:          objects,
	}
	b.BaselineID = ComputeID(b)
	return b
}

// ComputeID hashes the semantic content of a baseline, not its formatting or
// creation timestamp. Two captures of the same database state therefore get the
// same ID even if their files were written at different times.
func ComputeID(b *Baseline) string {
	payload := struct {
		Kind             string   `json:"kind"`
		Hash             string   `json:"hash,omitempty"`
		ExtensionVersion string   `json:"extension_version"`
		PostgresVersion  int      `json:"postgres_version"`
		Database         string   `json:"database"`
		Objects          []Object `json:"objects"`
	}{
		Kind:             b.Kind,
		Hash:             b.Hash,
		ExtensionVersion: b.ExtensionVersion,
		PostgresVersion:  b.PostgresVersion,
		Database:         b.Database,
		Objects:          b.Objects,
	}
	data, err := json.Marshal(payload)
	if err != nil {
		return ""
	}
	sum := sha256.Sum256(data)
	return "sha256:" + hex.EncodeToString(sum[:])
}

// Finalize normalizes ordering and recomputes BaselineID after objects or
// metadata have been added by Snapshot.
func (b *Baseline) Finalize() {
	SortObjects(b.Objects)
	b.BaselineID = ComputeID(b)
}

// Validate rejects malformed or incompatible documents before the CLI tries to
// connect to PostgreSQL. It checks structure and required physical metadata; it
// intentionally does not reject unknown JSON fields so newer files stay readable.
func (b *Baseline) Validate() error {
	if b.Format != FormatV1 && b.Format != FormatV2 {
		return fmt.Errorf("unsupported baseline format %q", b.Format)
	}
	if b.Format == FormatV1 && b.Hash != "" {
		return errors.New("v1 baseline must not contain a hash algorithm")
	}
	if b.Format == FormatV2 && b.Hash != HashFNV32 && b.Hash != HashFNV64 {
		return fmt.Errorf("v2 baseline has invalid hash %q", b.Hash)
	}
	if b.Kind != KindLogical && b.Kind != KindPhysical {
		return fmt.Errorf("invalid baseline kind %q", b.Kind)
	}
	if strings.TrimSpace(b.ExtensionVersion) == "" {
		return errors.New("baseline has no extension_version")
	}
	if b.PostgresVersion < 120000 {
		return fmt.Errorf("invalid postgres_version %d", b.PostgresVersion)
	}
	if strings.TrimSpace(b.Database) == "" {
		return errors.New("baseline has no database")
	}
	if b.CreatedAt == "" {
		return errors.New("baseline has no created_at")
	}
	if _, err := time.Parse(time.RFC3339, b.CreatedAt); err != nil {
		return fmt.Errorf("invalid created_at: %w", err)
	}
	if b.Kind == KindPhysical && b.SystemIdentifier == "" {
		return errors.New("physical baseline has no system_identifier")
	}
	if b.Kind == KindPhysical && b.DatabaseOID <= 0 {
		return errors.New("physical baseline has no database_oid")
	}
	seen := make(map[string]bool, len(b.Objects))
	for i := range b.Objects {
		o := &b.Objects[i]
		if o.Kind != ObjectTable && o.Kind != ObjectIndex {
			return fmt.Errorf("object %d: invalid kind %q", i, o.Kind)
		}
		if strings.TrimSpace(o.Schema) == "" || strings.TrimSpace(o.Name) == "" {
			return fmt.Errorf("object %d: empty schema or name", i)
		}
		if o.Kind == ObjectIndex && (o.TableSchema == "" || o.TableName == "") {
			return fmt.Errorf("index %s: missing table identity", o.Identity())
		}
		if o.Status != StatusOK && o.Status != StatusSkipped {
			return fmt.Errorf("object %s: invalid status %q", o.Identity(), o.Status)
		}
		if o.Status == StatusOK && !checksumRE.MatchString(o.Checksum) {
			return fmt.Errorf("object %s: invalid checksum %q", o.Identity(), o.Checksum)
		}
		if o.SchemaHash != "" && !schemaHashRE.MatchString(o.SchemaHash) {
			return fmt.Errorf("object %s: invalid schema_hash %q", o.Identity(), o.SchemaHash)
		}
		if o.Stats != nil {
			if o.Kind != ObjectTable {
				return fmt.Errorf("object %s: stats are only valid for tables", o.Identity())
			}
			if o.Stats.Relfilenode < 0 || o.Stats.Inserted < 0 ||
				o.Stats.Updated < 0 || o.Stats.Deleted < 0 {
				return fmt.Errorf("object %s: negative table stats", o.Identity())
			}
		}
		if seen[o.Key()] {
			return fmt.Errorf("duplicate object %s", o.Key())
		}
		seen[o.Key()] = true
	}
	return nil
}

// Load reads a baseline from a file, gzip-compressed file or stdin ("-").
// The file is parsed first and validated before any database work starts.
func Load(path string) (*Baseline, error) {
	var r io.Reader
	if path == "-" {
		r = os.Stdin
	} else {
		f, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		defer f.Close()
		r = f
	}

	if strings.HasSuffix(path, ".gz") {
		gz, err := gzip.NewReader(r)
		if err != nil {
			return nil, err
		}
		defer gz.Close()
		r = gz
	}

	dec := json.NewDecoder(r)
	var b Baseline
	if err := dec.Decode(&b); err != nil {
		return nil, fmt.Errorf("parse baseline: %w", err)
	}
	b.Scope = normalizeScope(b.Scope)
	if err := b.Validate(); err != nil {
		return nil, err
	}
	return &b, nil
}

// Save writes a baseline atomically: data goes to a temporary file in the
// destination directory, is fsynced, and only then replaces the target. A
// failed capture or update therefore cannot leave a half-written baseline.
// Path "-" writes to stdout for pipelines.
func Save(path string, b *Baseline, stdout io.Writer) error {
	b.Scope = normalizeScope(b.Scope)
	b.Finalize()
	data, err := json.MarshalIndent(b, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')

	if path == "-" {
		_, err = stdout.Write(data)
		return err
	}

	dir := filepath.Dir(path)
	if dir == "." {
		dir = "."
	}
	tmp, err := os.CreateTemp(dir, ".pgics-baseline-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0o644); err != nil {
		tmp.Close()
		return err
	}
	if strings.HasSuffix(path, ".gz") {
		gz := gzip.NewWriter(tmp)
		if _, err := gz.Write(data); err != nil {
			gz.Close()
			tmp.Close()
			return err
		}
		if err := gz.Close(); err != nil {
			tmp.Close()
			return err
		}
	} else if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, path); err != nil {
		// Windows cannot rename over an existing file, so replace it first.
		// This is the only non-atomic step, and only on Windows.
		if runtime.GOOS != "windows" {
			return err
		}
		if removeErr := os.Remove(path); removeErr != nil && !os.IsNotExist(removeErr) {
			return err
		}
		return os.Rename(tmpName, path)
	}
	return nil
}
