package cli

import (
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/baseline"
)

// encodeJSON writes indented JSON plus a trailing newline. Stable field order
// comes from the structs in package baseline; map fields are only used for the
// small command envelopes.
func encodeJSON(w io.Writer, value any) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(value)
}

// writeChangesText prints one line per difference. Long checksums are kept
// verbatim rather than shortened: the output is meant to be copy-pasteable.
func writeChangesText(w io.Writer, changes []baseline.Change, showOK bool) {
	filtered := baseline.FilterChanges(changes, showOK)
	if len(filtered) == 0 {
		fmt.Fprintln(w, "no changes")
		return
	}
	for _, c := range filtered {
		line := fmt.Sprintf("%-14s %-5s %s.%s", strings.ToUpper(c.Status), c.Kind, c.Schema, c.Name)
		if c.Kind == baseline.ObjectIndex && c.TableName != "" {
			line += fmt.Sprintf(" (on %s.%s)", c.TableSchema, c.TableName)
		}
		if c.BaselineChecksum != "" || c.CurrentChecksum != "" {
			line += fmt.Sprintf(" expected=%s actual=%s", display(c.BaselineChecksum), display(c.CurrentChecksum))
		}
		if c.Reason != "" {
			line += fmt.Sprintf(" reason=%q", c.Reason)
		}
		fmt.Fprintln(w, line)
	}
}

// display substitutes a dash for an absent checksum (new/missing objects).
func display(value string) string {
	if value == "" {
		return "-"
	}
	return value
}

// writeSummary is the machine-friendly final line of text reports.
func writeSummary(w io.Writer, summary baseline.Summary) {
	fmt.Fprintf(w, "checked=%d ok=%d changed=%d schema_changed=%d new=%d missing=%d skipped=%d\n",
		summary.Total(), summary.OK, summary.Changed, summary.SchemaChanged, summary.New, summary.Missing, summary.Skipped)
}

// writeVerifyJSON is the CI-facing verify result. The baseline ID identifies
// which expected state was checked, while summary and changes describe the
// current database.
func writeVerifyJSON(w io.Writer, b *baseline.Baseline, changes []baseline.Change, summary baseline.Summary, showOK bool) error {
	return encodeJSON(w, map[string]any{
		"format":      "pg_integrity_checksums.verify/v1",
		"baseline_id": b.BaselineID,
		"kind":        b.Kind,
		"database":    b.Database,
		"summary":     summary,
		"changes":     baseline.FilterChanges(changes, showOK),
	})
}

// writeDiffJSON reports both baseline IDs so callers can prove which two
// snapshots were compared.
func writeDiffJSON(w io.Writer, oldB, newB *baseline.Baseline, changes []baseline.Change, summary baseline.Summary, showOK bool) error {
	return encodeJSON(w, map[string]any{
		"format":   "pg_integrity_checksums.diff/v1",
		"old_id":   oldB.BaselineID,
		"new_id":   newB.BaselineID,
		"old_kind": oldB.Kind,
		"new_kind": newB.Kind,
		"summary":  summary,
		"changes":  baseline.FilterChanges(changes, showOK),
	})
}
