package baseline

import "sort"

// Change is one entry in a diff. Status is one of ok, changed, new, missing
// or skipped; missing/new are relative to the current database in verify and
// relative to the second file in diff.
type Change struct {
	Kind        string `json:"kind"`
	Schema      string `json:"schema"`
	Name        string `json:"name"`
	TableSchema string `json:"table_schema,omitempty"`
	TableName   string `json:"table_name,omitempty"`

	Status           string `json:"status"`
	BaselineChecksum string `json:"baseline_checksum,omitempty"`
	CurrentChecksum  string `json:"current_checksum,omitempty"`
	Reason           string `json:"reason,omitempty"`
}

// Summary counts changes by category. Skipped is reported separately because
// an unsupported object is not evidence that data changed.
type Summary struct {
	OK      int `json:"ok"`
	Changed int `json:"changed"`
	New     int `json:"new"`
	Missing int `json:"missing"`
	Skipped int `json:"skipped"`
}

// Total is the number of objects considered by the comparison.
func (s Summary) Total() int {
	return s.OK + s.Changed + s.New + s.Missing + s.Skipped
}

// HasChanges reports whether data or object membership changed. Skipped
// objects alone do not make the result fail.
func (s Summary) HasChanges() bool {
	return s.Changed > 0 || s.New > 0 || s.Missing > 0
}

// Compare matches objects by their stable key and produces a deterministic,
// sorted change list. It is used both by verify (baseline vs database) and by
// diff (old baseline vs new baseline).
func Compare(oldObjects, currentObjects []Object) ([]Change, Summary) {
	oldMap := make(map[string]Object, len(oldObjects))
	for _, o := range oldObjects {
		oldMap[o.Key()] = o
	}
	currentMap := make(map[string]Object, len(currentObjects))
	for _, o := range currentObjects {
		currentMap[o.Key()] = o
	}

	// Walk the union once so missing and new objects are reported from either
	// side of the comparison.
	keys := make(map[string]bool, len(oldMap)+len(currentMap))
	for k := range oldMap {
		keys[k] = true
	}
	for k := range currentMap {
		keys[k] = true
	}

	changes := make([]Change, 0, len(keys))
	summary := Summary{}

	for key := range keys {
		oldObj, hasOld := oldMap[key]
		curObj, hasCur := currentMap[key]

		base := Change{}
		if hasOld {
			base.Kind = oldObj.Kind
			base.Schema = oldObj.Schema
			base.Name = oldObj.Name
			base.TableSchema = oldObj.TableSchema
			base.TableName = oldObj.TableName
		} else {
			base.Kind = curObj.Kind
			base.Schema = curObj.Schema
			base.Name = curObj.Name
			base.TableSchema = curObj.TableSchema
			base.TableName = curObj.TableName
		}

		switch {
		case !hasCur:
			base.Status = "missing"
			base.BaselineChecksum = oldObj.Checksum
			base.Reason = oldObj.Reason
			summary.Missing++
		case !hasOld:
			base.Status = "new"
			base.CurrentChecksum = curObj.Checksum
			base.Reason = curObj.Reason
			summary.New++
		case oldObj.Status == StatusSkipped || curObj.Status == StatusSkipped:
			base.Status = "skipped"
			base.BaselineChecksum = oldObj.Checksum
			base.CurrentChecksum = curObj.Checksum
			if curObj.Reason != "" {
				base.Reason = curObj.Reason
			} else {
				base.Reason = oldObj.Reason
			}
			summary.Skipped++
		case oldObj.Checksum != curObj.Checksum:
			base.Status = "changed"
			base.BaselineChecksum = oldObj.Checksum
			base.CurrentChecksum = curObj.Checksum
			summary.Changed++
		default:
			base.Status = "ok"
			base.BaselineChecksum = oldObj.Checksum
			base.CurrentChecksum = curObj.Checksum
			summary.OK++
		}
		changes = append(changes, base)
	}

	sort.Slice(changes, func(i, j int) bool {
		if changes[i].Kind != changes[j].Kind {
			return changes[i].Kind < changes[j].Kind
		}
		if changes[i].Schema != changes[j].Schema {
			return changes[i].Schema < changes[j].Schema
		}
		return changes[i].Name < changes[j].Name
	})
	return changes, summary
}

// FilterChanges returns only non-OK entries unless showOK is true. JSON
// callers use the same filter, so text and JSON reports list the same set.
func FilterChanges(changes []Change, showOK bool) []Change {
	if showOK {
		return changes
	}
	out := make([]Change, 0, len(changes))
	for _, c := range changes {
		if c.Status != "ok" {
			out = append(out, c)
		}
	}
	return out
}

// FailOn maps the --fail-on policy to an exit decision. An empty or unknown
// mode is treated as "any" by callers that do not validate it first.
func FailOn(summary Summary, mode string) bool {
	switch mode {
	case "none":
		return false
	case "changed":
		return summary.Changed > 0
	case "new":
		return summary.New > 0
	case "missing":
		return summary.Missing > 0
	case "any", "":
		return summary.HasChanges()
	default:
		return summary.HasChanges()
	}
}
