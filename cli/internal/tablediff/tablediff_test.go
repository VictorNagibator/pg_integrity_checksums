package tablediff

import (
	"reflect"
	"testing"
)

func TestAutoBuckets(t *testing.T) {
	cases := map[int64]int{
		-1:            unknownRowsBuckets,
		0:             1,
		1:             1,
		64:            1,
		65:            2,
		1_000_000:     15625,
		1_000_000_000: MaxBuckets,
	}
	for rows, want := range cases {
		if got := AutoBuckets(rows); got != want {
			t.Errorf("AutoBuckets(%d) = %d, want %d", rows, got, want)
		}
	}
}

func TestDifferingBuckets(t *testing.T) {
	src := []Bucket{{0, 2, "aa"}, {1, 3, "bb"}, {2, 0, "00"}, {3, 5, "dd"}}
	tgt := []Bucket{{0, 2, "aa"}, {1, 4, "bc"}, {2, 1, "cc"}, {3, 5, "dd"}}
	got, rows := DifferingBuckets(src, tgt)
	if !reflect.DeepEqual(got, []int{1, 2}) || rows != 5 {
		t.Fatalf("got %v rows=%d", got, rows)
	}

	got, rows = DifferingBuckets(src, src)
	if len(got) != 0 || rows != 0 {
		t.Fatalf("identical: got %v rows=%d", got, rows)
	}

	// A bucket missing on one side counts as differing.
	got, rows = DifferingBuckets(src[:3], src)
	if !reflect.DeepEqual(got, []int{3}) || rows != 5 {
		t.Fatalf("missing bucket: got %v rows=%d", got, rows)
	}
}

func TestCompareRows(t *testing.T) {
	src := []Row{
		{0, "k1", "(1)", "c1"},
		{0, "k2", "(2)", "c2"},
		{1, "k3", "(3)", "c3"},
	}
	tgt := []Row{
		{0, "k1", "(1)", "c1"},
		{0, "k2", "(2)", "x2"},
		{1, "k4", "(4)", "c4"},
	}
	want := []RowDiff{
		{Kind: Changed, Key: "(2)", Bucket: 0, KeyHash: "k2", SourceChecksum: "c2", TargetChecksum: "x2"},
		{Kind: Missing, Key: "(3)", Bucket: 1, KeyHash: "k3", SourceChecksum: "c3"},
		{Kind: Extra, Key: "(4)", Bucket: 1, KeyHash: "k4", TargetChecksum: "c4"},
	}
	if got := CompareRows(src, tgt); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %+v\nwant %+v", got, want)
	}
	if got := CompareRows(src, src); len(got) != 0 {
		t.Fatalf("identical rows reported: %+v", got)
	}
}

func TestCompareRowsDuplicateKeys(t *testing.T) {
	// Whole-row key: duplicates share key hash and checksum.
	src := []Row{{0, "k", "(a)", "c"}, {0, "k", "(a)", "c"}, {0, "k", "(a)", "c"}}
	tgt := []Row{{0, "k", "(a)", "c"}}
	got := CompareRows(src, tgt)
	if len(got) != 2 || got[0].Kind != Missing || got[1].Kind != Missing {
		t.Fatalf("duplicates: %+v", got)
	}

	// Non-unique key: the identical row matches, the others pair up.
	src = []Row{{0, "k", "(a)", "c1"}, {0, "k", "(a)", "c2"}, {0, "k", "(a)", "c3"}}
	tgt = []Row{{0, "k", "(a)", "c2"}, {0, "k", "(a)", "x"}}
	got = CompareRows(src, tgt)
	if len(got) != 2 {
		t.Fatalf("non-unique key: %+v", got)
	}
	kinds := map[string]int{}
	for _, d := range got {
		kinds[d.Kind]++
	}
	if kinds[Changed] != 1 || kinds[Missing] != 1 {
		t.Fatalf("non-unique key kinds: %+v", got)
	}
}
