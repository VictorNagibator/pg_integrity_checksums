package cli

import (
	"errors"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"strings"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/config"
)

func runConfig(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: pg_integrity_checksums config validate|show [options]")
		return 2
	}
	switch args[0] {
	case "validate":
		return runConfigValidate(args[1:], stdout, stderr)
	case "show":
		return runConfigShow(args[1:], stdout, stderr)
	case "help", "-h", "--help":
		fmt.Fprintln(stdout, "usage: pg_integrity_checksums config validate|show [options]")
		return 0
	default:
		return commandError(stderr, fmt.Errorf("unknown config command %q", args[0]))
	}
}

func runConfigValidate(args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("config validate", stderr)
	configPath := fs.String("config", "", "config file")
	profileName := fs.String("profile", "", "profile name")
	quiet := fs.Bool("quiet", false, "suppress success output")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}
	f, path, err := config.Load(*configPath)
	if err != nil {
		return commandError(stderr, err)
	}
	if f == nil {
		return commandError(stderr, errors.New("no config file found"))
	}
	if *profileName != "" {
		p, err := f.Select(*profileName)
		if err != nil {
			return commandError(stderr, err)
		}
		if err := validateProfileRuntime(*profileName, p); err != nil {
			return commandError(stderr, err)
		}
	} else {
		for name, p := range f.Profiles {
			if err := validateProfileRuntime(name, p); err != nil {
				return commandError(stderr, err)
			}
		}
	}
	if !*quiet {
		if *profileName != "" {
			fmt.Fprintf(stdout, "config OK: %s (profile %s)\n", path, *profileName)
		} else {
			label := "profiles"
			if len(f.Profiles) == 1 {
				label = "profile"
			}
			fmt.Fprintf(stdout, "config OK: %s (%d %s)\n", path, len(f.Profiles), label)
		}
	}
	return 0
}

func runConfigShow(args []string, stdout, stderr io.Writer) int {
	fs := newFlagSet("config show", stderr)
	configPath := fs.String("config", "", "config file")
	profileName := fs.String("profile", "", "profile name")
	if code := parseFlags(fs, args, stderr); code >= 0 {
		return code
	}
	f, path, err := config.Load(*configPath)
	if err != nil {
		return commandError(stderr, err)
	}
	if f == nil {
		return commandError(stderr, errors.New("no config file found"))
	}
	name := *profileName
	if name == "" {
		name = f.Default
	}
	if name == "" && len(f.Profiles) == 1 {
		for only := range f.Profiles {
			name = only
		}
	}
	if name == "" {
		return commandError(stderr, errors.New("no profile selected; use --profile"))
	}
	p, err := f.Select(name)
	if err != nil {
		return commandError(stderr, err)
	}
	fmt.Fprintf(stdout, "config: %s\n", path)
	fmt.Fprintf(stdout, "profile: %s\n", name)
	fmt.Fprintf(stdout, "dsn: %s\n", maskDSN(p.ResolveDSN()))
	if p.DSN != "" && strings.HasPrefix(p.DSN, "env:") {
		fmt.Fprintf(stdout, "dsn_source: %s\n", p.DSN)
	}
	fmt.Fprintf(stdout, "baseline: %s\n", valueOrDash(p.Baseline))
	fmt.Fprintf(stdout, "output: %s\n", valueOrDash(p.Output))
	fmt.Fprintf(stdout, "schema: %s\n", joinOrDash(p.Schemas))
	fmt.Fprintf(stdout, "exclude_schema: %s\n", joinOrDash(p.ExcludeSchemas))
	fmt.Fprintf(stdout, "table: %s\n", joinOrDash(p.Tables))
	fmt.Fprintf(stdout, "exclude_table: %s\n", joinOrDash(p.ExcludeTables))
	fmt.Fprintf(stdout, "fail_on: %s\n", valueOrDash(p.FailOn))
	if p.Timeout != "" {
		fmt.Fprintf(stdout, "timeout: %s\n", p.Timeout)
	}
	if p.ConnectTimeout != "" {
		fmt.Fprintf(stdout, "connect_timeout: %s\n", p.ConnectTimeout)
	}
	if p.Workers != nil {
		fmt.Fprintf(stdout, "workers: %d\n", *p.Workers)
	}
	return 0
}

func validateProfileRuntime(name string, p config.Profile) error {
	if strings.HasPrefix(p.DSN, "env:") && p.ResolveDSN() == "" {
		return fmt.Errorf("profile %q: environment variable %s is empty", name, strings.TrimPrefix(p.DSN, "env:"))
	}
	if p.Timeout != "" {
		if _, err := profileDuration(p.Timeout); err != nil {
			return fmt.Errorf("profile %q: %w", name, err)
		}
	}
	if p.ConnectTimeout != "" {
		if _, err := profileDuration(p.ConnectTimeout); err != nil {
			return fmt.Errorf("profile %q: %w", name, err)
		}
	}
	if p.Format != "" {
		if _, err := parseFormat(p.Format); err != nil {
			return fmt.Errorf("profile %q: %w", name, err)
		}
	}
	if p.FailOn != "" {
		if _, err := parseFailOn(p.FailOn); err != nil {
			return fmt.Errorf("profile %q: %w", name, err)
		}
	}
	if p.Kind != "" {
		if _, err := parseKind(p.Kind); err != nil {
			return fmt.Errorf("profile %q: %w", name, err)
		}
	}
	return nil
}

func maskDSN(value string) string {
	if value == "" {
		return "-"
	}
	if strings.HasPrefix(value, "env:") {
		return value
	}
	if strings.Contains(value, "://") {
		if u, err := url.Parse(value); err == nil && u.User != nil {
			user := u.User.Username()
			if _, hasPassword := u.User.Password(); hasPassword {
				u.User = url.UserPassword(user, "***")
				return strings.ReplaceAll(strings.ReplaceAll(u.String(), "%2A%2A%2A", "***"), "%2a%2a%2a", "***")
			}
			return u.String()
		}
	}
	re := regexp.MustCompile("(?i)(password\\s*=\\s*)\\S+")
	return re.ReplaceAllString(value, "${1}***")
}

func valueOrDash(value string) string {
	if value == "" {
		return "-"
	}
	return value
}

func joinOrDash(values []string) string {
	if len(values) == 0 {
		return "-"
	}
	return strings.Join(values, ",")
}
