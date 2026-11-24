package baseline

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func testScope() Scope {
	s := NewScope()
	s.Schemas = []string{"public"}
	return s
}

func TestNewSortsAndComputesID(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{
		{Kind: ObjectTable, Schema: "public", Name: "z", Status: StatusOK, Checksum: "2"},
		{Kind: ObjectTable, Schema: "public", Name: "a", Status: StatusOK, Checksum: "1"},
	})
	if b.Objects[0].Name != "a" || b.Objects[1].Name != "z" {
		t.Fatalf("objects not sorted: %#v", b.Objects)
	}
	if !strings.HasPrefix(b.BaselineID, "sha256:") || len(b.BaselineID) != len("sha256:")+64 {
		t.Fatalf("bad baseline id %q", b.BaselineID)
	}
	if b.Format != FormatV1 {
		t.Fatalf("bad format %q", b.Format)
	}
}

func TestScopeEmptySlicesJSON(t *testing.T) {
	data, err := json.Marshal(NewScope())
	if err != nil {
		t.Fatal(err)
	}
	text := string(data)
	if strings.Contains(text, "null") {
		t.Fatalf("empty scope contains null: %s", text)
	}
	for _, key := range []string{"schemas", "exclude_schemas", "tables", "exclude_tables"} {
		if !strings.Contains(text, "\""+key+"\":[]") {
			t.Fatalf("scope key %s is not an empty array: %s", key, text)
		}
	}
}

func TestSaveLoadRoundTrip(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{
		{Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "-42"},
		{Kind: ObjectIndex, Schema: "public", Name: "t_pkey", TableSchema: "public", TableName: "t", Status: StatusOK, Checksum: "42"},
	})
	path := filepath.Join(t.TempDir(), "baseline.json")
	if err := Save(path, b, os.Stdout); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.BaselineID != b.BaselineID {
		t.Fatalf("id mismatch: %q %q", loaded.BaselineID, b.BaselineID)
	}
	if len(loaded.Objects) != 2 {
		t.Fatalf("objects mismatch: %#v", loaded.Objects)
	}
	byName := map[string]Object{}
	for _, o := range loaded.Objects {
		byName[o.Name] = o
	}
	if byName["t_pkey"].TableName != "t" || byName["t"].Checksum != "-42" {
		t.Fatalf("objects mismatch: %#v", loaded.Objects)
	}
}

func TestLoadNormalizesNullScope(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), nil)
	b.Scope.Schemas = nil
	b.Scope.ExcludeSchemas = nil
	b.Scope.Tables = nil
	b.Scope.ExcludeTables = nil
	data, err := json.Marshal(b)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "null-scope.json")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	loaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Scope.Schemas == nil || loaded.Scope.ExcludeTables == nil {
		t.Fatalf("scope was not normalized: %#v", loaded.Scope)
	}
}

func TestSaveGzip(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "1",
	}})
	path := filepath.Join(t.TempDir(), "baseline.json.gz")
	if err := Save(path, b, os.Stdout); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) < 2 || data[0] != 0x1f || data[1] != 0x8b {
		t.Fatalf("not gzip: %x", data[:2])
	}
	loaded, err := Load(path)
	if err != nil || len(loaded.Objects) != 1 {
		t.Fatalf("load gzip: %v %#v", err, loaded)
	}
}

func TestValidateRejectsBadDocuments(t *testing.T) {
	tests := []struct {
		name string
		edit func(*Baseline)
	}{
		{"format", func(b *Baseline) { b.Format = "v0" }},
		{"kind", func(b *Baseline) { b.Kind = "other" }},
		{"checksum", func(b *Baseline) { b.Objects[0].Checksum = "x" }},
		{"index-table", func(b *Baseline) { b.Objects[0].TableName = "" }},
		{"physical-system", func(b *Baseline) { b.Kind = KindPhysical }},
		{"schema-hash-version-future", func(b *Baseline) { b.SchemaHashVersion = SchemaHashVersion + 1 }},
		{"schema-hash-version-negative", func(b *Baseline) { b.SchemaHashVersion = -1 }},
		{"physical-sequence", func(b *Baseline) {
			b.Kind = KindPhysical
			b.SystemIdentifier = "1"
			b.DatabaseOID = 1
			b.Objects[0] = Object{Kind: ObjectSequence, Schema: "public", Name: "s", Status: StatusOK, Checksum: "1"}
		}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{
				{Kind: ObjectIndex, Schema: "public", Name: "i", TableSchema: "public", TableName: "t", Status: StatusOK, Checksum: "1"},
			})
			tc.edit(b)
			if err := b.Validate(); err == nil {
				t.Fatal("expected validation error")
			}
		})
	}
}

func TestCompareStatuses(t *testing.T) {
	oldObjects := []Object{
		{Kind: ObjectTable, Schema: "public", Name: "same", Status: StatusOK, Checksum: "1"},
		{Kind: ObjectTable, Schema: "public", Name: "changed", Status: StatusOK, Checksum: "1"},
		{Kind: ObjectTable, Schema: "public", Name: "missing", Status: StatusOK, Checksum: "1"},
	}
	currentObjects := []Object{
		{Kind: ObjectTable, Schema: "public", Name: "same", Status: StatusOK, Checksum: "1"},
		{Kind: ObjectTable, Schema: "public", Name: "changed", Status: StatusOK, Checksum: "2"},
		{Kind: ObjectTable, Schema: "public", Name: "new", Status: StatusOK, Checksum: "3"},
	}
	changes, summary := Compare(oldObjects, currentObjects)
	if summary.OK != 1 || summary.Changed != 1 || summary.Missing != 1 || summary.New != 1 {
		t.Fatalf("bad summary: %#v", summary)
	}
	want := map[string]string{"same": "ok", "changed": "changed", "missing": "missing", "new": "new"}
	for _, c := range changes {
		if want[c.Name] != c.Status {
			t.Fatalf("object %s: got %s want %s", c.Name, c.Status, want[c.Name])
		}
	}
}

func TestCompareOKToSkippedIsChange(t *testing.T) {
	old := []Object{{Kind: ObjectIndex, Schema: "public", Name: "i", Status: StatusOK, Checksum: "1"}}
	cur := []Object{{Kind: ObjectIndex, Schema: "public", Name: "i", Status: StatusSkipped, Reason: "invalid index"}}
	changes, summary := Compare(old, cur)
	if summary.Changed != 1 || summary.Skipped != 0 || !summary.HasChanges() {
		t.Fatalf("bad summary: %#v", summary)
	}
	if changes[0].Status != "changed" {
		t.Fatalf("status %q, want changed", changes[0].Status)
	}
	// skipped -> skipped and skipped -> ok stay skipped.
	old[0].Status = StatusSkipped
	if _, s2 := Compare(old, cur); s2.Skipped != 1 || s2.HasChanges() {
		t.Fatalf("bad summary: %#v", s2)
	}
}

func TestCompareSchemaChanged(t *testing.T) {
	oldObjects := []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK,
		Checksum: "1", SchemaHash: "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	}}
	currentObjects := []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK,
		Checksum: "2", SchemaHash: "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	}}
	changes, summary := Compare(oldObjects, currentObjects)
	if summary.SchemaChanged != 1 || summary.Changed != 0 || summary.HasChanges() != true {
		t.Fatalf("bad schema summary: %#v", summary)
	}
	if changes[0].Status != "schema_changed" {
		t.Fatalf("bad status: %s", changes[0].Status)
	}
	if !FailOn(summary, "changed") || !FailOn(summary, "any") {
		t.Fatal("schema change should fail changed/any")
	}
}

func TestStatsValidation(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "1",
		Stats: &TableStats{Relfilenode: 1, Inserted: 2},
	}})
	if err := b.Validate(); err != nil {
		t.Fatal(err)
	}
	b.Objects[0].Stats.Deleted = -1
	if err := b.Validate(); err == nil {
		t.Fatal("negative stats should fail")
	}
}

func TestFailOn(t *testing.T) {
	s := Summary{Changed: 1, New: 1, Missing: 1}
	if FailOn(s, "none") {
		t.Fatal("none should not fail")
	}
	if !FailOn(s, "changed") || !FailOn(s, "new") || !FailOn(s, "missing") || !FailOn(s, "any") {
		t.Fatal("expected failure")
	}
	if FailOn(Summary{Changed: 1}, "schema-changed") {
		t.Fatal("data change must not match schema-changed")
	}
	if !FailOn(Summary{SchemaChanged: 1}, "schema-changed") {
		t.Fatal("schema change must match schema-changed")
	}
	if FailOn(Summary{OK: 1}, "any") {
		t.Fatal("unchanged should not fail")
	}
}

func TestLoadRejectsBadJSON(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bad.json")
	if err := os.WriteFile(path, []byte("{"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("expected parse error")
	}
}

func TestNewWithHashV2(t *testing.T) {
	b := NewWithHash("0.4.0", KindLogical, HashFNV64, "2.2.0", 160009, "app", testScope(), []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "-1",
	}})
	if b.Format != FormatV2 || b.Hash != HashFNV64 {
		t.Fatalf("bad v2 metadata: format=%q hash=%q", b.Format, b.Hash)
	}
	if err := b.Validate(); err != nil {
		t.Fatalf("v2 validation: %v", err)
	}
	data, err := json.Marshal(b)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"hash":"fnv64"`) {
		t.Fatalf("v2 JSON has no hash: %s", data)
	}
}

func TestValidateRejectsBadV2Hash(t *testing.T) {
	b := NewWithHash("0.4.0", KindLogical, HashFNV64, "2.2.0", 160009, "app", testScope(), nil)
	b.Hash = "sha256"
	if err := b.Validate(); err == nil {
		t.Fatal("expected invalid hash error")
	}
}

func TestCompareBaselinesSchemaHashVersions(t *testing.T) {
	hashA := "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	hashB := "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	mk := func(version int, hash, checksum string) *Baseline {
		b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{{
			Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK,
			Checksum: checksum, SchemaHash: hash,
		}})
		b.SchemaHashVersion = version
		return b
	}

	// Same version: a different hash is a schema change.
	_, summary, comparable := CompareBaselines(mk(2, hashA, "1"), mk(2, hashB, "1"))
	if !comparable || summary.SchemaChanged != 1 {
		t.Fatalf("same version: comparable=%v summary=%#v", comparable, summary)
	}

	// An old baseline (absent field = version 1) against a version 2 one:
	// the hashes are not comparable and must not be reported as a schema
	// change, but data changes still are.
	changes, summary, comparable := CompareBaselines(mk(0, hashA, "1"), mk(2, hashB, "1"))
	if comparable || summary.SchemaChanged != 0 || summary.OK != 1 {
		t.Fatalf("old vs new: comparable=%v summary=%#v", comparable, summary)
	}
	if changes[0].Status != "ok" {
		t.Fatalf("bad status %q", changes[0].Status)
	}
	_, summary, _ = CompareBaselines(mk(0, hashA, "1"), mk(2, hashB, "2"))
	if summary.Changed != 1 {
		t.Fatalf("data change not reported across versions: %#v", summary)
	}

	// CompareBaselines must not modify the baselines it was given.
	old := mk(0, hashA, "1")
	CompareBaselines(old, mk(2, hashB, "1"))
	if old.Objects[0].SchemaHash != hashA {
		t.Fatal("CompareBaselines cleared the caller's schema hash")
	}

	if mk(0, hashA, "1").EffectiveSchemaHashVersion() != 1 {
		t.Fatal("absent schema_hash_version must mean 1")
	}
}

func TestSequenceObjects(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.9.0", 160009, "app", testScope(), []Object{
		{Kind: ObjectSequence, Schema: "public", Name: "free_seq", Status: StatusOK, Checksum: "-7"},
		{Kind: ObjectSequence, Schema: "public", Name: "t_id_seq", TableSchema: "public", TableName: "t", Status: StatusOK, Checksum: "3"},
		{Kind: ObjectTable, Schema: "public", Name: "t_id_seq", Status: StatusOK, Checksum: "1"},
	})
	if err := b.Validate(); err != nil {
		t.Fatal(err)
	}
	if !b.Scope.IncludeSequences {
		t.Fatal("NewScope should include sequences")
	}

	// A table and a sequence with the same name are distinct objects.
	cur := append([]Object{}, b.Objects...)
	for i := range cur {
		if cur[i].Kind == ObjectSequence && cur[i].Name == "t_id_seq" {
			cur[i].Checksum = "4"
		}
	}
	changes, summary := Compare(b.Objects, cur)
	if summary.Changed != 1 || summary.OK != 2 {
		t.Fatalf("summary = %+v", summary)
	}
	for _, c := range changes {
		if c.Status == "changed" && (c.Kind != ObjectSequence || c.TableName != "t") {
			t.Fatalf("unexpected change %+v", c)
		}
	}
}

func TestOldScopeHasNoSequences(t *testing.T) {
	var s Scope
	if err := json.Unmarshal([]byte(`{"schemas":[],"exclude_schemas":[],"tables":[],"exclude_tables":[],"include_system":false,"include_indexes":true}`), &s); err != nil {
		t.Fatal(err)
	}
	if s.IncludeSequences {
		t.Fatal("a scope written before sequences were supported must not include them")
	}
}

func TestNewWithHashCRC32CAndXXH3(t *testing.T) {
	for _, hash := range []string{HashCRC32C, HashXXH3} {
		b := NewWithHash("0.7.0", KindLogical, hash, "2.9.0", 160009, "app", testScope(), []Object{{
			Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "-1",
		}})
		if b.Format != FormatV2 || b.Hash != hash {
			t.Fatalf("%s: bad v2 metadata: format=%q hash=%q", hash, b.Format, b.Hash)
		}
		if err := b.Validate(); err != nil {
			t.Fatalf("%s: v2 validation: %v", hash, err)
		}
	}
}

func TestValidHash(t *testing.T) {
	for _, h := range []string{"fnv32", "fnv64", "crc32c", "xxh3"} {
		if !ValidHash(h) {
			t.Fatalf("ValidHash(%q) = false", h)
		}
	}
	for _, h := range []string{"", "FNV32", "crc32", "xxh64", "sha256"} {
		if ValidHash(h) {
			t.Fatalf("ValidHash(%q) = true", h)
		}
	}
}

func TestHashMinExtension(t *testing.T) {
	for hash, want := range map[string]string{
		HashFNV32:  "",
		HashFNV64:  "",
		HashCRC32C: "2.9.0",
		HashXXH3:   "2.9.0",
	} {
		if got := HashMinExtension(hash); got != want {
			t.Fatalf("HashMinExtension(%q) = %q, want %q", hash, got, want)
		}
	}
}

// Names joined with a dot are ambiguous: table "a.b"."c" and table "a"."b.c"
// must stay two objects, in a baseline and in a comparison.
func TestDottedNamesAreDistinct(t *testing.T) {
	objects := []Object{
		{Kind: ObjectTable, Schema: "a.b", Name: "c", Status: StatusOK, Checksum: "1"},
		{Kind: ObjectTable, Schema: "a", Name: "b.c", Status: StatusOK, Checksum: "2"},
	}
	if objects[0].Key() == objects[1].Key() {
		t.Fatalf("keys collide: %q", objects[0].Key())
	}
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), objects)
	if err := b.Validate(); err != nil {
		t.Fatalf("validate: %v", err)
	}

	changed := []Object{objects[0], objects[1]}
	changed[1].Checksum = "3"
	changes, summary := Compare(objects, changed)
	if len(changes) != 2 || summary.OK != 1 || summary.Changed != 1 {
		t.Fatalf("compare: %+v %+v", summary, changes)
	}
}

func TestValidateRejectsDuplicateObject(t *testing.T) {
	b := New("0.1.0", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{
		{Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "1"},
		{Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "2"},
	})
	err := b.Validate()
	if err == nil || !strings.Contains(err.Error(), `duplicate table "public"."t"`) {
		t.Fatalf("got %v", err)
	}
}
