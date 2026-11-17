// Package tablediff compares the bucket and row checksums that the
// extension's pg_table_bucket_checksums and pg_table_row_checksums return
// for the same table on two databases.
package tablediff

import "sort"

// Row difference kinds.
const (
	Missing = "missing" // in the source only
	Extra   = "extra"   // in the target only
	Changed = "changed" // same key, different row
)

// MaxBuckets is the largest bucket count the extension accepts.
const MaxBuckets = 1 << 20

// rowsPerBucket is the bucket size AutoBuckets aims for: small enough that
// fetching the rows of a differing bucket is cheap, large enough that the
// bucket list stays a fraction of the table.
const rowsPerBucket = 64

// unknownRowsBuckets is used when the size of a table is unknown (never
// analyzed).
const unknownRowsBuckets = 1024

// Bucket is one row of pg_table_bucket_checksums.
type Bucket struct {
	Bucket   int
	Count    int64
	Checksum string
}

// Row is one row of pg_table_row_checksums.
type Row struct {
	Bucket   int
	KeyHash  string
	Key      string
	Checksum string
}

// RowDiff is one row that differs between source and target.
type RowDiff struct {
	Kind           string `json:"kind"`
	Key            string `json:"key"`
	Bucket         int    `json:"bucket"`
	KeyHash        string `json:"key_hash"`
	SourceChecksum string `json:"source_checksum,omitempty"`
	TargetChecksum string `json:"target_checksum,omitempty"`
}

// AutoBuckets picks a bucket count for a table of the given estimated size
// (negative when unknown): about rowsPerBucket rows per bucket.
func AutoBuckets(estimatedRows int64) int {
	if estimatedRows < 0 {
		return unknownRowsBuckets
	}
	n := (estimatedRows + rowsPerBucket - 1) / rowsPerBucket
	if n < 1 {
		return 1
	}
	if n > MaxBuckets {
		return MaxBuckets
	}
	return int(n)
}

// DifferingBuckets returns the bucket numbers whose checksum or row count
// differs, in order, and the number of rows they hold (per bucket the larger
// of the two counts). Both lists must come from the same bucket count.
func DifferingBuckets(source, target []Bucket) ([]int, int64) {
	byBucket := make(map[int]Bucket, len(target))
	for _, b := range target {
		byBucket[b.Bucket] = b
	}
	seen := make(map[int]bool, len(source))
	var out []int
	var rows int64
	for _, s := range source {
		seen[s.Bucket] = true
		t, ok := byBucket[s.Bucket]
		if ok && t.Checksum == s.Checksum && t.Count == s.Count {
			continue
		}
		out = append(out, s.Bucket)
		rows += max(s.Count, t.Count)
	}
	for _, t := range target {
		if !seen[t.Bucket] {
			out = append(out, t.Bucket)
			rows += t.Count
		}
	}
	sort.Ints(out)
	return out, rows
}

// CompareRows matches source and target rows by key hash and returns the
// rows that differ, ordered by bucket, key and kind.
//
// Keys need not be unique (a whole-row key repeats for duplicate rows, a
// chosen key may not be unique): for each key the rows that are identical
// on both sides are matched first, the remaining ones pair up as changed and
// whatever is left over is missing or extra.
func CompareRows(source, target []Row) []RowDiff {
	type group struct {
		bucket int
		key    string
		src    []string
		tgt    []string
	}
	groups := make(map[string]*group)
	add := func(r Row, fromSource bool) {
		g := groups[r.KeyHash]
		if g == nil {
			g = &group{bucket: r.Bucket, key: r.Key}
			groups[r.KeyHash] = g
		}
		if fromSource {
			g.src = append(g.src, r.Checksum)
		} else {
			g.tgt = append(g.tgt, r.Checksum)
		}
	}
	for _, r := range source {
		add(r, true)
	}
	for _, r := range target {
		add(r, false)
	}

	var out []RowDiff
	for keyHash, g := range groups {
		src, tgt := unmatched(g.src, g.tgt)
		n := min(len(src), len(tgt))
		for i := 0; i < n; i++ {
			out = append(out, RowDiff{Kind: Changed, Key: g.key, Bucket: g.bucket, KeyHash: keyHash,
				SourceChecksum: src[i], TargetChecksum: tgt[i]})
		}
		for _, c := range src[n:] {
			out = append(out, RowDiff{Kind: Missing, Key: g.key, Bucket: g.bucket, KeyHash: keyHash,
				SourceChecksum: c})
		}
		for _, c := range tgt[n:] {
			out = append(out, RowDiff{Kind: Extra, Key: g.key, Bucket: g.bucket, KeyHash: keyHash,
				TargetChecksum: c})
		}
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if a.Bucket != b.Bucket {
			return a.Bucket < b.Bucket
		}
		if a.Key != b.Key {
			return a.Key < b.Key
		}
		if a.KeyHash != b.KeyHash {
			return a.KeyHash < b.KeyHash
		}
		if a.Kind != b.Kind {
			return a.Kind < b.Kind
		}
		return a.SourceChecksum+a.TargetChecksum < b.SourceChecksum+b.TargetChecksum
	})
	return out
}

// unmatched removes the checksums present on both sides (as multisets) and
// returns what is left of each, sorted.
func unmatched(src, tgt []string) ([]string, []string) {
	counts := make(map[string]int, len(tgt))
	for _, c := range tgt {
		counts[c]++
	}
	var restSrc []string
	for _, c := range src {
		if counts[c] > 0 {
			counts[c]--
			continue
		}
		restSrc = append(restSrc, c)
	}
	var restTgt []string
	for _, c := range tgt {
		if counts[c] > 0 {
			counts[c]--
			restTgt = append(restTgt, c)
		}
	}
	sort.Strings(restSrc)
	sort.Strings(restTgt)
	return restSrc, restTgt
}
