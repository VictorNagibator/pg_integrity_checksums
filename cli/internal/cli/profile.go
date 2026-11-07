package cli

import (
	"flag"
	"fmt"
	"path/filepath"
	"time"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/config"
)

func flagSet(fs *flag.FlagSet) map[string]bool {
	set := make(map[string]bool)
	fs.Visit(func(f *flag.Flag) {
		set[f.Name] = true
	})
	return set
}

func loadProfile(configPath, profileName string) (config.Profile, string, error) {
	f, path, err := config.Load(configPath)
	if err != nil {
		return config.Profile{}, path, err
	}
	p, err := f.Select(profileName)
	if err != nil {
		return config.Profile{}, path, err
	}
	return p, path, nil
}

func resolveDSN(db, dsn string) (string, error) {
	if db != "" && dsn != "" && db != dsn {
		return "", fmt.Errorf("--db and --dsn specify different connection strings")
	}
	if dsn != "" {
		return dsn, nil
	}
	return db, nil
}

func profilePath(base, value string) string {
	if value == "" || value == "-" || filepath.IsAbs(value) {
		return value
	}
	return filepath.Join(base, value)
}

func profileDuration(value string) (time.Duration, error) {
	if value == "" {
		return 0, nil
	}
	d, err := time.ParseDuration(value)
	if err != nil {
		return 0, fmt.Errorf("invalid profile duration %q: %w", value, err)
	}
	if d < 0 {
		return 0, fmt.Errorf("profile duration must be >= 0")
	}
	return d, nil
}

func boolValue(set map[string]bool, name string, current bool, profileValue *bool) bool {
	if set[name] || profileValue == nil {
		return current
	}
	return *profileValue
}

func intValue(set map[string]bool, name string, current int, profileValue *int) int {
	if set[name] || profileValue == nil {
		return current
	}
	return *profileValue
}
