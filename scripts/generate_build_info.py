#!/usr/bin/env python3
"""Generate the provenance file embedded in a private-test VPK."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--renderer", required=True)
    parser.add_argument("--logging", choices=("enabled", "disabled"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--title-id", required=True)
    parser.add_argument("--eboot", type=Path, required=True)
    parser.add_argument(
        "--asset", action="append", default=[], metavar="NAME=PATH"
    )
    args = parser.parse_args()

    lines = [
        "format=1",
        f"revision={args.revision}",
        f"renderer={args.renderer}",
        f"logging={args.logging}",
        f"title_id={args.title_id}",
        f"version={args.version}",
        f"eboot_sha256={sha256(args.eboot)}",
    ]
    for item in args.asset:
        name, separator, value = item.partition("=")
        if not separator or not name or not value:
            parser.error(f"invalid --asset value: {item!r}")
        lines.append(f"asset_{name}_sha256={sha256(Path(value))}")

    content = "\n".join(lines) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
