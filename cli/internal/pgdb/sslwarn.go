package pgdb

import (
	"fmt"
	"net"
	"os"
	"strings"

	"github.com/jackc/pgx/v5"
)

// SuppressSSLWarningEnv disables the insecure-connection warning when set to
// a non-empty value other than "0".
const SuppressSSLWarningEnv = "PGICS_NO_SSL_WARNING"

// SSLWarning returns a warning when dsn describes a connection to a remote
// host that does not verify the server certificate, or "" when there is
// nothing to warn about. Unix sockets and loopback addresses are never
// reported. An empty dsn means the PG* environment variables.
//
// pgx does not expose sslmode, so the effective TLS settings are inspected:
// a connection verifies the server only when TLS is mandatory (no plaintext
// fallback) and the certificate chain is checked (sslmode=verify-ca or
// verify-full).
func SSLWarning(dsn string) string {
	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		return "" // the connection attempt reports the real error
	}
	configs := []*pgxFallback{{cfg.Host, cfg.TLSConfig != nil &&
		verifies(cfg.TLSConfig.InsecureSkipVerify, cfg.TLSConfig.VerifyPeerCertificate != nil)}}
	for _, fb := range cfg.Fallbacks {
		secure := fb.TLSConfig != nil && verifies(fb.TLSConfig.InsecureSkipVerify, fb.TLSConfig.VerifyPeerCertificate != nil)
		configs = append(configs, &pgxFallback{fb.Host, secure})
	}
	for _, c := range configs {
		if !c.secure && isRemote(c.host) {
			return fmt.Sprintf("warning: connection to %q does not verify the server certificate "+
				"(sslmode is not verify-ca or verify-full); traffic can be intercepted or "+
				"downgraded to plaintext. Set sslmode=verify-full (and sslrootcert) in the DSN, "+
				"or set %s=1 to silence this warning", c.host, SuppressSSLWarningEnv)
		}
	}
	return ""
}

type pgxFallback struct {
	host   string
	secure bool
}

func verifies(insecureSkipVerify, customVerify bool) bool {
	return !insecureSkipVerify || customVerify
}

func isRemote(host string) bool {
	if host == "" || strings.HasPrefix(host, "/") {
		return false
	}
	if strings.EqualFold(host, "localhost") {
		return false
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		return false
	}
	return true
}

func sslWarningSuppressed() bool {
	v := strings.TrimSpace(os.Getenv(SuppressSSLWarningEnv))
	return v != "" && v != "0"
}
