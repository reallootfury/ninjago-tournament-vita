"""Validate fixed Android-loader artwork (legacy script name).

These are original supplied assets, not crops of pic0.png. Do not regenerate
them from the Android wordmark splash: that changes the design and removes
the gate's transparency. No image-processing dependency is required.
"""
from pathlib import Path
import hashlib
import struct
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
LIVEAREA = ROOT / "extras" / "livearea"
ANDROID_ASSETS = {
    "bg0.png": ((840, 500), "7b6356674f0b0fe3989b66489d8e24188a1973d39c2d01676de40cef316dcd20"),
    "startup.png": ((280, 158), "c3a35453f467386d0a29e7d7268d23c5fdef3667791a5c09a972c637ecb3758e"),
}

def validate(livearea: Path = LIVEAREA) -> None:
    template = ET.parse(livearea / "template.xml").getroot()
    if template.findtext("livearea-background/image") != "bg0.png":
        raise ValueError("template does not reference Android-loader background")
    if template.findtext("gate/startup-image") != "startup.png":
        raise ValueError("template does not reference Android-loader gate")
    for name, (size, expected) in ANDROID_ASSETS.items():
        data = (livearea / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"{name}: original Android-loader artwork hash mismatch")
        if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
            raise ValueError(f"{name}: not a PNG")
        width, height, depth, colour = struct.unpack_from(">IIBB", data, 16)
        if (width, height) != size or depth != 8 or colour != 3:
            raise ValueError(f"{name}: expected 8-bit indexed PNG at {size}")

def main() -> None:
    validate()
    print("PASS: exact blue-striped background and transparent green Android gate")

if __name__ == "__main__":
    main()
