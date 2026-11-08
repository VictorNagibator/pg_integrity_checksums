package baseline

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// FuzzLoad checks that arbitrary baseline bytes never panic the JSON/validation path.
func FuzzLoad(f *testing.F) {
	b := New("0.3.0-dev", KindLogical, "2.1.0", 160009, "app", testScope(), []Object{{
		Kind: ObjectTable, Schema: "public", Name: "t", Status: StatusOK, Checksum: "1",
	}})
	seed, err := json.Marshal(b)
	if err != nil {
		f.Fatal(err)
	}
	f.Add(seed)
	f.Add([]byte("{}"))
	f.Add([]byte("{"))
	f.Fuzz(func(t *testing.T, data []byte) {
		path := filepath.Join(t.TempDir(), "baseline.json")
		if err := os.WriteFile(path, data, 0o600); err != nil {
			t.Fatal(err)
		}
		_, _ = Load(path)
	})
}
