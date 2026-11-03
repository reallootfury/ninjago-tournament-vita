#!/usr/bin/env python3
"""Reject stale or mismatched Ninjago Vita packages before device testing."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path
from render_livearea import validate as validate_livearea_artwork


REVISION = "1.00"
DEBUG_REVISION = "1.00 Logging"
RENDERER = "vitaGL"
TITLE_ID = "NINJA0001"
VERSION = "01.00"
ASSETS = {
    "icon0": ("extras/livearea/icon0.png", "sce_sys/icon0.png"),
    "pic0": ("extras/livearea/pic0.png", "sce_sys/pic0.png"),
    "startup": (
        "extras/livearea/startup.png",
        "sce_sys/livearea/contents/startup.png",
    ),
    "bg0": (
        "extras/livearea/bg0.png",
        "sce_sys/livearea/contents/bg0.png",
    ),
}


class VerificationError(RuntimeError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_sfo(data: bytes) -> dict[str, object]:
    if len(data) < 20 or data[:4] != b"\x00PSF":
        raise VerificationError("sce_sys/param.sfo is not a valid PSF")
    _, key_offset, data_offset, count = struct.unpack_from("<IIII", data, 4)
    result: dict[str, object] = {}
    for index in range(count):
        entry_offset = 20 + index * 16
        if entry_offset + 16 > len(data):
            raise VerificationError("param.sfo entry table is truncated")
        key_rel, value_format, length, _, value_rel = struct.unpack_from(
            "<HHIII", data, entry_offset
        )
        key_start = key_offset + key_rel
        key_end = data.find(b"\0", key_start)
        value_start = data_offset + value_rel
        value_end = value_start + length
        if key_end < 0 or value_end > len(data):
            raise VerificationError("param.sfo contains an invalid offset")
        key = data[key_start:key_end].decode("utf-8", "strict")
        value = data[value_start:value_end]
        if value_format == 0x0404 and length == 4:
            result[key] = struct.unpack("<I", value)[0]
        else:
            result[key] = value.rstrip(b"\0").decode("utf-8", "replace")
    return result


def read_info(data: bytes) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in data.decode("utf-8", "strict").splitlines():
        key, separator, value = line.partition("=")
        if not separator or not key:
            raise VerificationError(f"invalid build-info.txt line: {line!r}")
        result[key] = value
    return result


def require_equal(label: str, actual: object, expected: object) -> None:
    if actual != expected:
        raise VerificationError(
            f"{label} mismatch: got {actual!r}, expected {expected!r}"
        )


def verify_elf(path: Path, revision: str) -> None:
    data = path.read_bytes()
    if len(data) < 0x28 or data[:4] != b"\x7fELF" or data[4] != 1:
        raise VerificationError(f"{path} is not an ELF32 executable")
    endian = "<" if data[5] == 1 else ">"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    flags = struct.unpack_from(endian + "I", data, 0x24)[0]
    require_equal("ELF machine", machine, 40)
    if not (flags & 0x200) or (flags & 0x400):
        raise VerificationError(
            f"ELF is not ARM soft-float ABI (e_flags=0x{flags:08x})"
        )
    identity = f"Ninjago Tournament {revision}; renderer {RENDERER}.\n".encode()
    if identity not in data:
        raise VerificationError(f"ELF does not match {revision} / {RENDERER}")


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Verify eboot, SFO, revision, ABI and LiveArea assets"
    )
    parser.add_argument("vpk", type=Path)
    parser.add_argument("--debug", action="store_true", help="Verify the logging-enabled variant")
    parser.add_argument("--eboot", type=Path)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    build = repo / ("build-log" if args.debug else "build")
    revision = DEBUG_REVISION if args.debug else REVISION
    logging_mode = "enabled" if args.debug else "disabled"
    elf = args.elf or build / "ninjago_vita"
    eboot = args.eboot or build / "eboot.bin"

    validate_livearea_artwork(repo / "extras/livearea")
    verify_elf(elf, revision)
    source_eboot = eboot.read_bytes()

    with zipfile.ZipFile(args.vpk) as archive:
        broken = archive.testzip()
        if broken:
            raise VerificationError(f"VPK has a corrupt member: {broken}")
        names = set(archive.namelist())
        required = {"eboot.bin", "sce_sys/param.sfo", "build-info.txt"}
        required.update(member for _, member in ASSETS.values())
        required.add("sce_sys/livearea/contents/template.xml")
        missing = sorted(required - names)
        if missing:
            raise VerificationError(f"VPK is missing: {', '.join(missing)}")

        license_sources = [repo / "LICENSE", repo / "COPYING", repo / "THIRD_PARTY.md"]
        license_sources.extend(sorted((repo / "licenses").glob("*.txt")))
        for source in license_sources:
            member = f"licenses/{source.name}"
            require_equal(f"{member} license hash",
                          sha256(archive.read(member)), sha256(source.read_bytes()))

        embedded_eboot = archive.read("eboot.bin")
        require_equal("embedded eboot", sha256(embedded_eboot), sha256(source_eboot))

        sfo = read_sfo(archive.read("sce_sys/param.sfo"))
        require_equal("SFO TITLE_ID", sfo.get("TITLE_ID"), TITLE_ID)
        require_equal("SFO APP_VER", sfo.get("APP_VER"), VERSION)

        info = read_info(archive.read("build-info.txt"))
        require_equal("build revision", info.get("revision"), revision)
        require_equal("logging mode", info.get("logging"), logging_mode)
        require_equal("renderer revision", info.get("renderer"), RENDERER)
        require_equal("build title ID", info.get("title_id"), TITLE_ID)
        require_equal("build version", info.get("version"), VERSION)
        require_equal("build eboot hash", info.get("eboot_sha256"), sha256(source_eboot))
        for name, (source_name, member) in ASSETS.items():
            source_data = (repo / source_name).read_bytes()
            digest = sha256(source_data)
            require_equal(f"{name} VPK hash", sha256(archive.read(member)), digest)
            require_equal(f"{name} build-info hash", info.get(f"asset_{name}_sha256"), digest)

        template = archive.read("sce_sys/livearea/contents/template.xml")
        require_equal(
            "LiveArea template hash",
            sha256(template),
            sha256((repo / "extras/livearea/template.xml").read_bytes()),
        )
        if b'content-rev="6"' not in template:
            raise VerificationError("LiveArea template does not use content-rev 6")

    print(f"PASS: {args.vpk}")
    print(f"  revision: {revision}; logging: {logging_mode}")
    print(f"  title/version: {TITLE_ID} / {VERSION}")
    print(f"  eboot SHA-256: {sha256(source_eboot)}")
    print("  ARM ABI: EABI5 soft-float")
    print("  LiveArea: four source hashes match; content-rev=6")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, ET.ParseError, zipfile.BadZipFile,
            VerificationError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
