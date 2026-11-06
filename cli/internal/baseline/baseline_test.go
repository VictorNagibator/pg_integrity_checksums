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

func TestFailOn(t *testing.T) {
	s := Summary{Changed: 1, New: 1, Missing: 1}
	if FailOn(s, "none") {
		t.Fatal("none should not fail")
	}
	if !FailOn(s, "changed") || !FailOn(s, "new") || !FailOn(s, "missing") || !FailOn(s, "any") {
		t.Fatal("expected failure")
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
