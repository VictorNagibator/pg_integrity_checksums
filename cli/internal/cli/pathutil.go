package cli

import "path/filepath"

// sameFilePath reports whether two output/input paths resolve to the same
// file. It prevents opening a report path that would truncate a baseline.
func sameFilePath(a, b string) bool {
	if a == "" || a == "-" || b == "" || b == "-" {
		return false
	}
	aa, errA := filepath.Abs(a)
	bb, errB := filepath.Abs(b)
	if errA != nil || errB != nil {
		return false
	}
	return filepath.Clean(aa) == filepath.Clean(bb)
}
