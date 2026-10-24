#!/usr/bin/env python3
"""Validate the supplied Outline X-Ray Bedrock resource pack structure.

Checks archive integrity, manifest requirements, alpha-enabled outline textures,
the RenderChunk material marker, and ore block-shape mappings. Does not prove
runtime shader compatibility with a specific Minecraft build.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path


def read_png_alpha(data: bytes) -> tuple[int, int, int, int]:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid PNG signature")
    pos = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        start = pos + 8
        end = start + length
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        chunk = data[start:end]
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])[:4]
        elif kind == b"IDAT":
            idat.extend(chunk)
        elif kind == b"IEND":
            break
        pos = end + 4
    if width is None or height is None or bit_depth != 8 or color_type != 6:
        raise ValueError(f"expected 8-bit RGBA PNG, got bit_depth={bit_depth}, color_type={color_type}")
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    if len(raw) != height * (stride + 1):
        raise ValueError("unexpected decompressed PNG size")
    prev = bytearray(stride)
    alpha_min, alpha_max = 255, 0
    transparent = 0
    offset = 0
    for _ in range(height):
        filt = raw[offset]
        offset += 1
        scan = bytearray(raw[offset:offset + stride])
        offset += stride
        for i in range(stride):
            left = scan[i - 4] if i >= 4 else 0
            up = prev[i]
            upper_left = prev[i - 4] if i >= 4 else 0
            if filt == 1:
                scan[i] = (scan[i] + left) & 255
            elif filt == 2:
                scan[i] = (scan[i] + up) & 255
            elif filt == 3:
                scan[i] = (scan[i] + ((left + up) // 2)) & 255
            elif filt == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                scan[i] = (scan[i] + predictor) & 255
            elif filt != 0:
                raise ValueError(f"unsupported PNG filter {filt}")
        for i in range(3, stride, 4):
            alpha = scan[i]
            alpha_min = min(alpha_min, alpha)
            alpha_max = max(alpha_max, alpha)
            if alpha < 255:
                transparent += 1
        prev = scan
    return width, height, alpha_min, alpha_max if transparent else 0


def validate(path: Path) -> list[str]:
    failures: list[str] = []
    with zipfile.ZipFile(path) as archive:
        bad = archive.testzip()
        if bad:
            failures.append(f"archive CRC failed for {bad}")
        names = set(archive.namelist())
        required = (
            "manifest.json",
            "blocks.json",
            "renderer/materials/RenderChunk.material.bin",
            "textures/blocks/stone.png",
            "textures/blocks/dirt.png",
            "textures/blocks/cobblestone.png",
        )
        for item in required:
            if item not in names:
                failures.append(f"missing {item}")
        if failures:
            return failures

        manifest = json.loads(archive.read("manifest.json"))
        header = manifest.get("header", {})
        min_engine = header.get("min_engine_version", [])
        if manifest.get("format_version") != 2:
            failures.append("manifest format_version is not 2")
        if min_engine < [1, 26, 51]:
            failures.append(f"min_engine_version is below [1, 26, 51]: {min_engine}")
        desc = str(header.get("description", "")).lower()
        if "patched minecraft" not in desc and "betterrenderdragon" not in desc:
            failures.append("manifest does not declare the patched Minecraft/BetterRenderDragon requirement")

        material = archive.read("renderer/materials/RenderChunk.material.bin")
        if b"RenderChunk" not in material or b"DepthOnlyOpaque" not in material:
            failures.append("RenderChunk material is missing expected DepthOnlyOpaque marker")

        blocks = json.loads(archive.read("blocks.json"))
        ore_names = [
            key for key in blocks
            if (key.startswith("minecraft:") and "ore" in key) or key == "minecraft:ancient_debris"
        ]
        if len(ore_names) < 15:
            failures.append(f"expected at least 15 ore/ancient-debris shape mappings; found {len(ore_names)}")
        wrong_shape = [key for key in ore_names if blocks[key].get("blockshape") != "cauldron"]
        if wrong_shape:
            failures.append("ore mappings without blockshape=cauldron: " + ", ".join(wrong_shape))

        texture_files = [
            name for name in names
            if name.startswith("textures/blocks/") and name.lower().endswith(".png")
        ]
        alpha_textures = 0
        partly_transparent = 0
        for name in texture_files:
            try:
                _, _, alpha_min, alpha_max = read_png_alpha(archive.read(name))
                alpha_textures += 1
                if alpha_min < 255 and alpha_max == 255:
                    partly_transparent += 1
            except Exception as exc:
                failures.append(f"invalid outline texture {name}: {exc}")
        if alpha_textures != len(texture_files):
            failures.append(f"only {alpha_textures}/{len(texture_files)} block textures verified as RGBA")
        if partly_transparent < min(3, len(texture_files)):
            failures.append("not enough textures contain both transparent and fully opaque pixels")

        print(f"Pack: {header.get('name', path.name)}")
        print(f"Version: {header.get('version')} | min engine: {min_engine}")
        print(f"Block textures: {len(texture_files)} | RGBA/alpha verified: {alpha_textures}")
        print(f"Ore mappings: {len(ore_names)} | compiled material bytes: {len(material)}")
        print("Scope: archive/content structure only; runtime shader support remains unverified")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", type=Path, help="path to the .mcpack file")
    args = parser.parse_args()
    try:
        failures = validate(args.pack)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if failures:
        print("VALIDATION FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print("Validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
