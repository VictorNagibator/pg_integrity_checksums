package pgdb

import (
	"strings"
	"testing"
)

func TestSSLWarning(t *testing.T) {
	t.Setenv("PGSSLMODE", "")
	cases := []struct {
		name string
		dsn  string
		warn bool
	}{
		{"unix socket", "postgresql:///app?host=/tmp", false},
		{"loopback ip", "postgresql://u@127.0.0.1/app", false},
		{"loopback ipv6", "postgresql://u@[::1]/app", false},
		{"localhost", "postgresql://u@localhost/app", false},
		{"remote default prefer", "postgresql://u@db.example.com/app", true},
		{"remote disable", "postgresql://u@db.example.com/app?sslmode=disable", true},
		{"remote require", "postgresql://u@db.example.com/app?sslmode=require", true},
		{"remote verify-full", "postgresql://u@db.example.com/app?sslmode=verify-full", false},
		{"keyword dsn remote", "host=db.example.com dbname=app sslmode=prefer", true},
		{"keyword dsn verify-full", "host=db.example.com dbname=app sslmode=verify-full", false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := SSLWarning(c.dsn)
			if (got != "") != c.warn {
				t.Fatalf("SSLWarning(%q) = %q, want warning=%v", c.dsn, got, c.warn)
			}
			if got != "" && strings.Contains(got, "secret") {
				t.Fatalf("warning leaks DSN: %q", got)
			}
		})
	}
}

func TestSSLWarningInvalidDSN(t *testing.T) {
	if got := SSLWarning("postgresql://%zz"); got != "" {
		t.Fatalf("invalid DSN should not warn, got %q", got)
	}
}

func TestSSLWarningDoesNotLeakPassword(t *testing.T) {
	got := SSLWarning("postgresql://u:s3cret@db.example.com/app")
	if got == "" || strings.Contains(got, "s3cret") {
		t.Fatalf("unexpected warning %q", got)
	}
}
