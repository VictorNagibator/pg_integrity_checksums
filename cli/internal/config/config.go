// Package config loads optional .pgics.yaml profiles used by the CLI.
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"
)

// DefaultFileNames are searched in the current directory when no explicit
// --config path or PGICS_CONFIG is supplied.
var DefaultFileNames = []string{".pgics.yaml", ".pgics.yml", "pgics.yaml", "pgics.yml"}

// File is the top-level YAML document. A profile only supplies defaults;
// command-line flags always win.
type File struct {
	Default  string             `yaml:"default"`
	Profiles map[string]Profile `yaml:"profiles"`
}

// Profile contains defaults for one named connection/scope.
type Profile struct {
	DSN                  string   `yaml:"dsn"`
	Kind                 string   `yaml:"kind"`
	Hash                 string   `yaml:"hash"`
	Baseline             string   `yaml:"baseline"`
	Output               string   `yaml:"output"`
	Schemas              []string `yaml:"schema"`
	SchemasPlural        []string `yaml:"schemas"`
	ExcludeSchemas       []string `yaml:"exclude_schema"`
	ExcludeSchemasPlural []string `yaml:"exclude_schemas"`
	Tables               []string `yaml:"table"`
	TablesPlural         []string `yaml:"tables"`
	ExcludeTables        []string `yaml:"exclude_table"`
	ExcludeTablesPlural  []string `yaml:"exclude_tables"`
	IncludeSystem        *bool    `yaml:"include_system"`
	IncludeIndexes       *bool    `yaml:"include_indexes"`
	Workers              *int     `yaml:"workers"`
	Timeout              string   `yaml:"timeout"`
	Strict               *bool    `yaml:"strict"`
	Format               string   `yaml:"format"`
	FailOn               string   `yaml:"fail_on"`
	ShowOK               *bool    `yaml:"show_ok"`
	ChangedSince         *bool    `yaml:"changed_since"`
	Verbose              *bool    `yaml:"verbose"`
	ConnectTimeout       string   `yaml:"connect_timeout"`
	ApplicationName      string   `yaml:"application_name"`
}

// Load reads an explicit config file, PGICS_CONFIG, or the first default file
// found in the current directory. It returns nil when no file is present.
func Load(path string) (*File, string, error) {
	if path == "" {
		path = strings.TrimSpace(os.Getenv("PGICS_CONFIG"))
	}
	explicit := path != ""
	if path == "" {
		for _, name := range DefaultFileNames {
			if _, err := os.Stat(name); err == nil {
				path = name
				break
			}
		}
	}
	if path == "" {
		return nil, "", nil
	}

	data, err := os.ReadFile(path)
	if err != nil {
		if !explicit && os.IsNotExist(err) {
			return nil, "", nil
		}
		return nil, path, fmt.Errorf("read config %s: %w", path, err)
	}
	var f File
	if err := yaml.Unmarshal(data, &f); err != nil {
		return nil, path, fmt.Errorf("parse config %s: %w", path, err)
	}
	if len(f.Profiles) == 0 {
		return nil, path, fmt.Errorf("config %s has no profiles", path)
	}
	for name, profile := range f.Profiles {
		normalizeProfile(&profile)
		f.Profiles[name] = profile
		if err := validate(name, profile); err != nil {
			return nil, path, err
		}
	}
	return &f, path, nil
}

func normalizeProfile(p *Profile) {
	p.Schemas = append(p.Schemas, p.SchemasPlural...)
	p.ExcludeSchemas = append(p.ExcludeSchemas, p.ExcludeSchemasPlural...)
	p.Tables = append(p.Tables, p.TablesPlural...)
	p.ExcludeTables = append(p.ExcludeTables, p.ExcludeTablesPlural...)
	p.SchemasPlural = nil
	p.ExcludeSchemasPlural = nil
	p.TablesPlural = nil
	p.ExcludeTablesPlural = nil
}

func validate(name string, p Profile) error {
	if p.DSN != "" && !strings.HasPrefix(p.DSN, "env:") && !strings.Contains(p.DSN, "=") && !strings.Contains(p.DSN, "://") {
		return fmt.Errorf("profile %q: dsn must be a connection string", name)
	}
	if p.Workers != nil && *p.Workers < 0 {
		return fmt.Errorf("profile %q: workers must be >= 0", name)
	}
	if p.Hash != "" && p.Hash != "fnv32" && p.Hash != "fnv64" {
		return fmt.Errorf("profile %q: hash must be fnv32 or fnv64", name)
	}
	return nil
}

// Select resolves PGICS_PROFILE, the --profile flag, or the file's default.
func (f *File) Select(name string) (Profile, error) {
	if f == nil {
		if name == "" {
			name = strings.TrimSpace(os.Getenv("PGICS_PROFILE"))
		}
		if name != "" {
			return Profile{}, fmt.Errorf("profile %q requested but no config file was found", name)
		}
		return Profile{}, nil
	}
	if name == "" {
		name = strings.TrimSpace(os.Getenv("PGICS_PROFILE"))
	}
	if name == "" {
		name = f.Default
	}
	if name == "" {
		return Profile{}, nil
	}
	p, ok := f.Profiles[name]
	if !ok {
		names := make([]string, 0, len(f.Profiles))
		for n := range f.Profiles {
			names = append(names, n)
		}
		return Profile{}, fmt.Errorf("unknown profile %q (available: %s)", name, strings.Join(names, ", "))
	}
	return p, nil
}

// ResolveDSN supports dsn: env:DATABASE_URL so secrets can stay in the
// environment rather than in a committed config file.
func (p Profile) ResolveDSN() string {
	if strings.HasPrefix(p.DSN, "env:") {
		return strings.TrimSpace(os.Getenv(strings.TrimPrefix(p.DSN, "env:")))
	}
	return p.DSN
}

// BaseDir is the directory containing the config file. Relative baseline and
// output paths in a profile are resolved against it, not the process CWD.
func BaseDir(path string) string {
	if path == "" {
		return "."
	}
	dir := filepath.Dir(path)
	if dir == "" {
		return "."
	}
	return dir
}
