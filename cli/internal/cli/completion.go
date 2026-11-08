package cli

import (
	"fmt"
	"io"
)

// runCompletion prints a shell completion script. The scripts are deliberately
// dependency-free and static; they complete commands and common flags.
func runCompletion(args []string, stdout, stderr io.Writer) int {
	if len(args) != 1 {
		return commandError(stderr, fmt.Errorf("usage: pg_integrity_checksums completion bash|zsh|fish|powershell"))
	}
	switch args[0] {
	case "bash":
		fmt.Fprint(stdout, bashCompletion())
	case "zsh":
		fmt.Fprint(stdout, zshCompletion())
	case "fish":
		fmt.Fprint(stdout, fishCompletion())
	case "powershell":
		fmt.Fprint(stdout, powershellCompletion())
	default:
		return commandError(stderr, fmt.Errorf("unsupported shell %q", args[0]))
	}
	return 0
}

func bashCompletion() string {
	return `# bash completion for pg_integrity_checksums
_pg_integrity_checksums() {
  local cur prev
  COMPREPLY=()
  cur="$2"
  prev="$3"
  if [ "$COMP_CWORD" -eq 1 ]; then
    COMPREPLY=( $(compgen -W "init config capture verify diff show update doctor completion version help" -- "$cur") )
    return 0
  fi
  case "$prev" in
    --db|--dsn|--config|--profile|--baseline|--output|--table|--exclude-table|--schema|--exclude-schema)
      _filedir
      return 0
      ;;
  esac
  COMPREPLY=( $(compgen -W "--db --dsn --config --profile --baseline --output --kind --format --fail-on --table --exclude-table --schema --exclude-schema --workers --timeout --connect-timeout --application-name --quiet --strict --show-ok --changed-since --allow-version-mismatch --yes --dry-run --objects --no-fail" -- "$cur") )
}
complete -F _pg_integrity_checksums pg_integrity_checksums
`
}

func zshCompletion() string {
	return `#compdef pg_integrity_checksums
_pg_integrity_checksums() {
  _arguments '1:command:(init config capture verify diff show update doctor completion version help)' '*::arg:->args'
}
compdef _pg_integrity_checksums pg_integrity_checksums
`
}

func fishCompletion() string {
	return `# fish completion for pg_integrity_checksums
complete -c pg_integrity_checksums -f -n '__fish_use_subcommand' -a 'init config capture verify diff show update doctor completion version help'
complete -c pg_integrity_checksums -l db -l dsn -l config -l profile -l baseline -l output -l table -l schema -r
`
}

func powershellCompletion() string {
	return `# PowerShell completion for pg_integrity_checksums
Register-ArgumentCompleter -Native -CommandName pg_integrity_checksums -ScriptBlock {
  param($wordToComplete, $commandAst, $cursorPosition)
  'capture','verify','diff','show','update','doctor','completion','version','help' |
    Where-Object { $_ -like "$wordToComplete*" } |
    ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }
}
`
}
