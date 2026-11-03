#!/usr/bin/env python3
"""Verify and stage the owned 1057 XAPK for the Vita loader."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

APK_SHA256 = "4350837c447218109e4a72a8d19a2d3699f9440f9a759aab7f3843d7b54f2d80"
SO_SHA256 = "0125c9766168a80d8ee84348d4028382f326d4a8ea5f6f1d8250a434fc1e69b3"
OBB_SHA256 = "a42a74b4a7a9c22c970cd504ce6463a070401d092605deb20fbf66f89b6dd1d4"
OBB_NAME = "main.1057.com.lego.ninjago.toe.obb"
SO_MEMBER = "lib/armeabi-v7a/libLEGO_M1.so"
EXPECTED_OBB_SIZE = 973_671_465


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_hash(path: Path, expected: str, label: str) -> None:
    actual = sha256(path)
    if actual != expected:
        raise ValueError(
            f"{label} SHA-256 mismatch\n"
            f"  expected: {expected}\n"
            f"  actual:   {actual}\n"
            "This loader only supports the original 1.05.2.970/1057 set."
        )
    print(f"verified {label}: {actual}")


def find_obb_member(archive: zipfile.ZipFile) -> str:
    matches = [
        name for name in archive.namelist()
        if name.replace("\\", "/").endswith("/" + OBB_NAME)
        or name == OBB_NAME
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one {OBB_NAME} in XAPK, found {len(matches)}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify and stage LEGO Ninjago Tournament 1057 data")
    parser.add_argument("xapk", type=Path, help="owned 1.05.2.970 XAPK")
    parser.add_argument("--output", type=Path, default=Path("stage"))
    args = parser.parse_args()

    source = args.xapk.resolve()
    output = args.output.resolve()
    if not source.is_file():
        parser.error(f"file does not exist: {source}")

    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ninjago-prepare-") as temporary:
        temporary_path = Path(temporary)
        apk_path = temporary_path / "base.apk"
        obb_path = output / OBB_NAME
        so_path = output / "libLEGO_M1.so"

        with zipfile.ZipFile(source) as xapk:
            if "base.apk" not in xapk.namelist():
                raise ValueError("XAPK has no base.apk")
            with xapk.open("base.apk") as incoming, apk_path.open("wb") as out:
                shutil.copyfileobj(incoming, out, 1024 * 1024)
            obb_member = find_obb_member(xapk)
            with xapk.open(obb_member) as incoming, obb_path.open("wb") as out:
                shutil.copyfileobj(incoming, out, 1024 * 1024)

        require_hash(apk_path, APK_SHA256, "base APK")
        if obb_path.stat().st_size != EXPECTED_OBB_SIZE:
            raise ValueError(
                f"OBB size mismatch: expected {EXPECTED_OBB_SIZE}, "
                f"got {obb_path.stat().st_size}")
        require_hash(obb_path, OBB_SHA256, "1057 OBB")

        with zipfile.ZipFile(apk_path) as apk:
            if SO_MEMBER not in apk.namelist():
                raise ValueError(f"APK has no {SO_MEMBER}")
            with apk.open(SO_MEMBER) as incoming, so_path.open("wb") as out:
                shutil.copyfileobj(incoming, out, 1024 * 1024)
            for member in apk.infolist():
                if member.is_dir() or not member.filename.startswith("assets/"):
                    continue
                destination = output / member.filename
                destination.parent.mkdir(parents=True, exist_ok=True)
                with apk.open(member) as incoming, destination.open("wb") as out:
                    shutil.copyfileobj(incoming, out, 1024 * 1024)
        require_hash(so_path, SO_SHA256, "ARMv7 libLEGO_M1.so")

    print(f"staged verified Vita data in {output}")
    print("copy the staged tree to ux0:data/ninjago/")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"prepare_data: {error}", file=sys.stderr)
        raise SystemExit(1)
