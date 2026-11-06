package main

import (
	"context"
	"os"
	"os/signal"

	"github.com/VictorNagibator/pg_integrity_checksums/cli/internal/cli"
)

// main only wires signals and exits with the code returned by cli.Run.
// Ctrl-C cancels the context, which aborts long transactions and SQL calls.
func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	os.Exit(cli.Run(ctx, os.Args[1:], os.Stdout, os.Stderr))
}
