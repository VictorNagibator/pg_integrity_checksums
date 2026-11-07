package pgdb

import (
	"testing"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

func TestMatchesScopeDefaults(t *testing.T) {
	scope := baseline.NewScope()
	tests := []struct {
		name string
		c    Candidate
		want bool
	}{
		{"user-table", Candidate{Kind: baseline.ObjectTable, Schema: "public", Name: "t"}, true},
		{"user-index", Candidate{Kind: baseline.ObjectIndex, Schema: "public", Name: "i", TableSchema: "public", TableName: "t"}, true},
		{"system-table", Candidate{Kind: baseline.ObjectTable, Schema: "pg_catalog", Name: "pg_class"}, false},
		{"toast-table", Candidate{Kind: baseline.ObjectTable, Schema: "pg_toast", Name: "x"}, false},
		{"information", Candidate{Kind: baseline.ObjectTable, Schema: "information_schema", Name: "tables"}, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := matchesScope(tc.c, scope); got != tc.want {
				t.Fatalf("matchesScope = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestMatchesScopeFilters(t *testing.T) {
	scope := baseline.NewScope()
	scope.Schemas = []string{"app"}
	scope.ExcludeTables = []string{"app.secret"}
	scope.IncludeIndexes = false
	if !matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "app", Name: "t"}, scope) {
		t.Fatal("expected app.t")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "other", Name: "t"}, scope) {
		t.Fatal("unexpected other.t")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectIndex, Schema: "app", Name: "i", TableSchema: "app", TableName: "t"}, scope) {
		t.Fatal("indexes should be disabled")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "app", Name: "secret"}, scope) {
		t.Fatal("secret should be excluded")
	}
}

func TestMatchesScopeGlobs(t *testing.T) {
	scope := baseline.NewScope()
	scope.Schemas = []string{"app_*"}
	scope.Tables = []string{"orders_*"}
	scope.ExcludeTables = []string{"orders_tmp_*"}
	if !matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "app_prod", Name: "orders_2026"}, scope) {
		t.Fatal("glob should match")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "app_prod", Name: "customers"}, scope) {
		t.Fatal("table glob should not match")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "other", Name: "orders_2026"}, scope) {
		t.Fatal("schema glob should not match")
	}
	if matchesScope(Candidate{Kind: baseline.ObjectTable, Schema: "app_prod", Name: "orders_tmp_1"}, scope) {
		t.Fatal("excluded glob should not match")
	}
}

func TestMatchesScopeTableFilterAppliesToIndexes(t *testing.T) {
	scope := baseline.NewScope()
	scope.Tables = []string{"app.orders"}
	idx := Candidate{Kind: baseline.ObjectIndex, Schema: "app", Name: "orders_pkey", TableSchema: "app", TableName: "orders"}
	other := Candidate{Kind: baseline.ObjectIndex, Schema: "app", Name: "x", TableSchema: "app", TableName: "other"}
	if !matchesScope(idx, scope) {
		t.Fatal("index on selected table should match")
	}
	if matchesScope(other, scope) {
		t.Fatal("index on other table should not match")
	}
}

func TestStatsEqual(t *testing.T) {
	base := &baseline.TableStats{Relfilenode: 1, Inserted: 2, Updated: 3, Deleted: 4}
	if !statsEqual(base, &baseline.TableStats{Relfilenode: 1, Inserted: 2, Updated: 3, Deleted: 4}) {
		t.Fatal("equal stats should match")
	}
	if statsEqual(&baseline.TableStats{Relfilenode: 1}, &baseline.TableStats{Relfilenode: 1}) {
		t.Fatal("all-zero counters must not be used as proof")
	}
	if statsEqual(base, &baseline.TableStats{Relfilenode: 1, Inserted: 1}) {
		t.Fatal("counter regression must not match")
	}
	if statsEqual(base, &baseline.TableStats{Relfilenode: 2, Inserted: 2, Updated: 3, Deleted: 4}) {
		t.Fatal("relfilenode change must not match")
	}
}

func TestReusableObject(t *testing.T) {
	hash := "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	table := Candidate{Kind: baseline.ObjectTable, Schema: "app", Name: "t"}
	tableKey := candidateKey(table)
	stats := &baseline.TableStats{Relfilenode: 10, Inserted: 1}
	prior := map[string]baseline.Object{
		tableKey: {Kind: baseline.ObjectTable, Schema: "app", Name: "t", Status: baseline.StatusOK, Checksum: "7", SchemaHash: hash, Stats: stats},
	}
	hashes := map[string]string{tableKey: hash}
	statsByKey := map[string]*baseline.TableStats{tableKey: {Relfilenode: 10, Inserted: 1}}
	current := baseline.Object{Kind: baseline.ObjectTable, Schema: "app", Name: "t", SchemaHash: hash, Stats: statsByKey[tableKey]}
	reused, ok := reusableObject(table, current, prior, hashes, statsByKey)
	if !ok || reused.Checksum != "7" {
		t.Fatalf("table should be reusable: %#v %v", reused, ok)
	}

	current.SchemaHash = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	if _, ok := reusableObject(table, current, prior, hashes, statsByKey); ok {
		t.Fatal("schema change must prevent reuse")
	}

	idx := Candidate{Kind: baseline.ObjectIndex, Schema: "app", Name: "t_pkey", TableSchema: "app", TableName: "t"}
	idxKey := candidateKey(idx)
	prior[idxKey] = baseline.Object{Kind: baseline.ObjectIndex, Schema: "app", Name: "t_pkey", TableSchema: "app", TableName: "t", Status: baseline.StatusOK, Checksum: "8", SchemaHash: hash}
	hashes[idxKey] = hash
	idxCurrent := baseline.Object{Kind: baseline.ObjectIndex, Schema: "app", Name: "t_pkey", TableSchema: "app", TableName: "t", SchemaHash: hash}
	if reused, ok := reusableObject(idx, idxCurrent, prior, hashes, statsByKey); !ok || reused.Checksum != "8" {
		t.Fatalf("index should be reusable: %#v %v", reused, ok)
	}
}

func TestIsSystemSchema(t *testing.T) {
	for _, name := range []string{"pg_catalog", "information_schema", "pg_toast", "pg_temp_3", "pg_toast_temp_3"} {
		if !isSystemSchema(name) {
			t.Fatalf("%s should be system", name)
		}
	}
	for _, name := range []string{"public", "app", "pg_custom"} {
		if isSystemSchema(name) {
			t.Fatalf("%s should not be system", name)
		}
	}
}
