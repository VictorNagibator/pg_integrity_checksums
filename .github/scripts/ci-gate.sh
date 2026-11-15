#!/usr/bin/env bash
# Wait until every other check run on a commit has finished, then fail if any
# of them failed.
#
# The workflows use path filters, so the set of checks differs between pull
# requests and a branch ruleset cannot require them directly.  This script is
# the single required check ("CI Gate"): it passes when everything that did
# run passed.
#
# Environment:
#   REPO           owner/name                         (required)
#   SHA            commit to watch                    (required)
#   GH_TOKEN       token with checks:read             (required by gh)
#   SELF_NAME      name of this check, ignored        (default: CI Gate)
#   INITIAL_DELAY  seconds before the first poll      (default: 60)
#   INTERVAL       seconds between polls              (default: 30)
#   STABLE_POLLS   consecutive all-done polls needed  (default: 2)
set -euo pipefail

: "${REPO:?REPO is required}"
: "${SHA:?SHA is required}"
SELF_NAME="${SELF_NAME:-CI Gate}"
INITIAL_DELAY="${INITIAL_DELAY:-60}"
INTERVAL="${INTERVAL:-30}"
STABLE_POLLS="${STABLE_POLLS:-2}"

# Latest run of each check (a re-run creates a new check run with the same
# name), excluding this gate.
latest_runs() {
	gh api --paginate --slurp "repos/$REPO/commits/$SHA/check-runs?per_page=100" |
		jq --arg self "$SELF_NAME" '
			[.[].check_runs[] | select(.name != $self)]
			| group_by([.app.slug, .name])
			| map(max_by(.id))'
}

# Workflows started by the same event register their checks within seconds;
# give them time so the first poll does not see an empty list.
sleep "$INITIAL_DELAY"

stable=0
while :; do
	runs="$(latest_runs)"
	total="$(jq 'length' <<<"$runs")"
	pending="$(jq '[.[] | select(.status != "completed")] | length' <<<"$runs")"

	if [ "$pending" -eq 0 ]; then
		stable=$((stable + 1))
		# Jobs with "needs" appear only after their dependencies finish, so
		# require the all-done state to hold across several polls.
		if [ "$stable" -ge "$STABLE_POLLS" ]; then
			break
		fi
	else
		stable=0
		echo "$(date -u +%H:%M:%S) waiting for $pending of $total checks:"
		jq -r '.[] | select(.status != "completed") | "  - \(.name) (\(.status))"' <<<"$runs"
	fi
	sleep "$INTERVAL"
done

echo "All $total checks completed:"
jq -r '.[] | "  \(.conclusion // "none" | ascii_upcase)  \(.name)"' <<<"$runs" | sort

failed="$(jq -r '
	.[]
	| select((.conclusion // "") as $c | ["success", "skipped", "neutral"] | index($c) | not)
	| "\(.name): \(.conclusion) \(.html_url)"' <<<"$runs")"

if [ -n "$failed" ]; then
	echo
	echo "Failed checks:"
	while IFS= read -r line; do
		echo "::error::$line"
	done <<<"$failed"
	exit 1
fi

echo "CI Gate passed."
