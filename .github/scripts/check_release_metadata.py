#!/usr/bin/env python3
"""Check that the extension's release metadata is consistent.

Usage: check_release_metadata.py [repo-root]

For the version in pg_integrity_checksums.control this verifies that

  * META.json (version, provides.*.version and provides.*.file) agrees;
  * the install script pg_integrity_checksums--<version>.sql exists;
  * the newest section of Changes is for that version and is not empty;
  * an upgrade script from the previous Changes version exists
    (pg_integrity_checksums--<previous>--<version>.sql).

Between releases the newest section may be headed "Unreleased" instead: when
the control version has no section of its own yet, that section stands for
it.  The release PR renames it to "<version>   <date>"; the Release workflow
fails without that section.

It catches the usual mistakes of a release PR (a version bump without a
Changes entry or without an upgrade script) before the tag is pushed, instead
of in the Release workflow.  Exits with status 1 and prints every problem.
"""

import json
import re
import sys
from pathlib import Path

HEADER = re.compile(r"^([0-9][0-9A-Za-z.\-]*)(?:\s.*)?$")
UNRELEASED = "Unreleased"
EXT = "pg_integrity_checksums"


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    problems = []

    control = (root / f"{EXT}.control").read_text()
    match = re.search(
        r"^default_version\s*=\s*'([^']*)'", control, flags=re.MULTILINE
    )
    if not match:
        print(f"error: no default_version in {EXT}.control", file=sys.stderr)
        return 1
    version = match.group(1)

    meta = json.loads((root / "META.json").read_text())
    provides = meta["provides"][EXT]
    if meta["version"] != version:
        problems.append(
            f"META.json version {meta['version']} != control version {version}"
        )
    if provides["version"] != version:
        problems.append(
            f"META.json provides version {provides['version']} != {version}"
        )
    if provides["file"] != f"{EXT}--{version}.sql":
        problems.append(
            f"META.json provides file {provides['file']} != {EXT}--{version}.sql"
        )

    if not (root / f"{EXT}--{version}.sql").is_file():
        problems.append(f"missing install script {EXT}--{version}.sql")

    lines = (root / "Changes").read_text().splitlines()
    headers = [
        (index, m.group(1))
        for index, line in enumerate(lines)
        if (m := HEADER.match(line))
    ]

    # An "Unreleased" section above every version section stands for the
    # control version when that version has no section yet (a version bump
    # between releases).  Otherwise it collects changes for a later release
    # and is not checked, as before.
    unreleased = [
        index for index, line in enumerate(lines) if line.strip() == UNRELEASED
    ]
    if (
        unreleased
        and (not headers or unreleased[0] < headers[0][0])
        and not any(v == version for _, v in headers)
    ):
        headers.insert(0, (unreleased[0], version))
    if not headers:
        problems.append("Changes has no version sections")
    else:
        first_index, first_version = headers[0]
        if first_version != version:
            problems.append(
                f"newest Changes section is {first_version}, expected {version}"
            )
        else:
            end = headers[1][0] if len(headers) > 1 else len(lines)
            if not any(line.strip() for line in lines[first_index + 1 : end]):
                problems.append(f"Changes section {version} is empty")

            if len(headers) > 1:
                previous = headers[1][1]
                upgrade = f"{EXT}--{previous}--{version}.sql"
                if not (root / upgrade).is_file():
                    problems.append(f"missing upgrade script {upgrade}")

    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1

    print(f"release metadata OK for {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
