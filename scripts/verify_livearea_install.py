#!/usr/bin/env python3
"""Read-only comparison of VitaShell-copied installed artwork and shell cache."""
import argparse
import hashlib
import sys
from pathlib import Path
from verify_vpk import ASSETS, VerificationError, read_sfo


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sce_sys", type=Path,
                        help="Copied ux0:app/NINJA0001/sce_sys directory")
    parser.add_argument("--appmeta-contents", type=Path,
                        help="Copied ur0:appmeta/NINJA0001/livearea/contents")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    sfo = read_sfo((args.sce_sys / "param.sfo").read_bytes())
    print(f"Installed title/version: {sfo.get('TITLE_ID')} / {sfo.get('APP_VER')}")
    failures = int(sfo.get("TITLE_ID") != "NINJA0001")
    files = [(source, member.removeprefix("sce_sys/"))
             for source, member in ASSETS.values()]
    files.append(("extras/livearea/template.xml", "livearea/contents/template.xml"))
    for source, member in files:
        expected = hashlib.sha256((repo / source).read_bytes()).hexdigest()
        path = args.sce_sys / member
        actual = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "MISSING"
        equal = actual == expected
        failures += int(not equal)
        print(f"{'PASS' if equal else 'FAIL'} installed {member}: {actual}")
        if args.appmeta_contents and member.startswith("livearea/contents/"):
            cached = args.appmeta_contents / Path(member).name
            digest = hashlib.sha256(cached.read_bytes()).hexdigest() if cached.is_file() else "MISSING"
            equal = digest == actual and digest != "MISSING"
            failures += int(not equal)
            print(f"{'PASS' if equal else 'FAIL'} shell {cached.name}: {digest}")
    if not args.appmeta_contents:
        print("Shell-cache equality remains unknown; this check makes no shell writes.")
    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, VerificationError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
