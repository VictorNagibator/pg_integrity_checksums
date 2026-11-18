#!/usr/bin/env python3
"""Record the README demo as an asciicast v2 file.

Every command shown is run for real with bash, and its real output is
recorded; only the typing rhythm and the pauses are synthetic.  Needs
pg_integrity_checksums and psql on PATH and the databases from setup.sh.

    PROD=postgres://localhost:5432/shop BACKUP=postgres://localhost:5433/shop \\
        demo/record.py demo/demo.cast
"""

import json
import os
import random
import re
import subprocess
import sys
import tempfile
import time

COLS, ROWS = 100, 24

PROMPT = "\x1b[1;32m$\x1b[0m "
DIM = "\x1b[90m"
RESET = "\x1b[0m"

# (command, pause after its output in seconds).  Comments are typed but not run.
SCENARIO = [
    ("# PROD is the source database, BACKUP a pg_dump of it restored on another server", 0.3),
    ("psql \"$PROD\" -c \"SELECT pg_table_logical_checksum('orders')\"", 0.6),
    ("psql \"$BACKUP\" -c \"SELECT pg_table_logical_checksum('orders')\"", 1.5),
    ("# One value silently changes in the copy...", 0.2),
    ('psql "$BACKUP" -c "UPDATE orders SET amount = amount + 0.01 WHERE id = 4242"', 0.6),
    ("psql \"$BACKUP\" -c \"SELECT pg_table_logical_checksum('orders')\"", 1.5),
    ("# Which row? Compare the row checksums of both copies", 0.2),
    ("Q=\"SELECT key, row_checksum FROM pg_table_row_checksums('orders') ORDER BY key_hash\"", 0.2),
    ('diff <(psql -At "$PROD" -c "$Q") <(psql -At "$BACKUP" -c "$Q")', 6.0),
]

# A command that only assigns a shell variable; later commands see it.
ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} OUTPUT.cast")
    try:
        dsns = {"PROD": os.environ["PROD"], "BACKUP": os.environ["BACKUP"]}
    except KeyError as e:
        sys.exit(f"{e.args[0]} is not set")

    rng = random.Random(1)
    env = dict(os.environ, **dsns)
    workdir = tempfile.mkdtemp(prefix="pgics-demo-")
    events = []
    assignments = []
    t = 0.5

    def emit(data, delay=0.0):
        nonlocal t
        t += delay
        events.append([round(t, 3), "o", data])

    emit(PROMPT)
    for cmd, pause in SCENARIO:
        comment = cmd.startswith("#")
        if cmd:
            if comment:
                emit(DIM)
            for ch in cmd:
                emit(ch, 0.022 + rng.uniform(-0.008, 0.012))
            if comment:
                emit(RESET)
            t += 0.15 if comment else 0.4
        emit("\r\n")
        if ASSIGNMENT.match(cmd):
            assignments.append(cmd)
        elif cmd and not comment:
            start = time.monotonic()
            script = "\n".join(assignments + [cmd])
            res = subprocess.run(["bash", "-c", script], cwd=workdir, env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True)
            elapsed = time.monotonic() - start
            out = res.stdout.replace("\r\n", "\n").replace("\n", "\r\n")
            if out:
                # Real time, but never so short that the output seems canned.
                emit(out, max(0.3, min(elapsed, 3.0)))
        t += pause
        emit(PROMPT)
    emit("", 1.0)

    header = {"version": 2, "width": COLS, "height": ROWS,
              "title": "pg_integrity_checksums: a database and its restored backup",
              "env": {"SHELL": "/bin/bash", "TERM": "xterm-256color"}}
    with open(sys.argv[1], "w") as f:
        f.write(json.dumps(header) + "\n")
        for ev in events:
            f.write(json.dumps(ev) + "\n")


if __name__ == "__main__":
    main()
