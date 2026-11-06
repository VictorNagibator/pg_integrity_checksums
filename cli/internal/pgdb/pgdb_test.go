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
