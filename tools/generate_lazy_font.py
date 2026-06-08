#!/usr/bin/env python3
import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


MAGIC = b"LZYFNT1\0"
HEADER_FMT = "<8sHHHHHHIII"
ENTRY_FMT = "<IIHHHHhh"


def collect_symbols(text_path: Path) -> str:
    symbols = []
    seen = set()
    for code in range(0x20, 0x7F):
        ch = chr(code)
        seen.add(ch)
        symbols.append(ch)
    for ch in text_path.read_text(encoding="utf-8"):
        if ch.isspace() or ord(ch) < 0x20:
            continue
        if ch not in seen:
            seen.add(ch)
            symbols.append(ch)
    return "".join(symbols)


def find_lv_font_conv() -> list[str]:
    node = Path("e:/nodejs/node.exe")
    conv = Path("e:/nodejs/node_modules/lv_font_conv/lv_font_conv.js")
    if node.exists() and conv.exists():
        return [str(node), str(conv)]
    return ["lv_font_conv"]


def run_lv_font_conv(font: Path, size: int, bpp: int, symbols: str, dump_dir: Path) -> None:
    if dump_dir.exists():
        shutil.rmtree(dump_dir)
    cmd = [
        *find_lv_font_conv(),
        "--font",
        str(font),
        "--size",
        str(size),
        "--bpp",
        str(bpp),
        "--format",
        "dump",
        "--range",
        "0x20-0x7E",
        "--symbols",
        symbols,
        "--no-kerning",
        "-o",
        str(dump_dir),
    ]
    subprocess.run(cmd, check=True)


def glyph_bitmap_bits(png_path: Path, width: int, rows: int) -> bytes:
    if width <= 0 or rows <= 0:
        return b""

    image = Image.open(png_path).convert("L")
    pixels = image.load()
    img_w, img_h = image.size

    bbox = None
    for y in range(img_h):
        for x in range(img_w):
            if pixels[x, y] < 128:
                if bbox is None:
                    bbox = [x, y, x, y]
                else:
                    bbox[0] = min(bbox[0], x)
                    bbox[1] = min(bbox[1], y)
                    bbox[2] = max(bbox[2], x)
                    bbox[3] = max(bbox[3], y)

    if bbox is None:
        return b""

    x0, y0 = bbox[0], bbox[1]
    data = bytearray(math.ceil(width * rows / 8))
    bit_index = 0
    for y in range(rows):
        for x in range(width):
            sx = x0 + x
            sy = y0 + y
            if sx < img_w and sy < img_h and pixels[sx, sy] < 128:
                data[bit_index // 8] |= 0x80 >> (bit_index % 8)
            bit_index += 1
    return bytes(data)


def build_lazy_font(dump_dir: Path, output: Path, size: int, bpp: int) -> tuple[int, int]:
    info = json.loads((dump_dir / "font_info.json").read_text(encoding="utf-8"))
    line_height = int(info["ascent"] - info["descent"])
    base_line = int(-info["descent"])

    entries = []
    bitmaps = bytearray()
    seen = set()

    for glyph in sorted(info["glyphs"], key=lambda item: item["code"]):
        code = int(glyph["code"])
        if code in seen:
            continue
        seen.add(code)

        ft = glyph["freetype"]
        bitmap = ft["bitmap"]
        metrics = ft["metrics"]
        width = int(metrics["width"])
        rows = int(metrics["height"])
        adv_w = int(metrics["horiAdvance"])
        ofs_x = int(ft["bitmap_left"])
        ofs_y = int(ft["bitmap_top"]) - rows

        if width <= 0 or rows <= 0:
            width = 0
            rows = 0
            data = b""
            ofs_x = 0
            ofs_y = 0
        else:
            png_path = dump_dir / f"{code:x}.png"
            data = glyph_bitmap_bits(png_path, width, rows)

        bitmap_offset = len(bitmaps)
        bitmaps.extend(data)
        entries.append((code, bitmap_offset, len(data), adv_w, width, rows, ofs_x, ofs_y))

    header_size = struct.calcsize(HEADER_FMT)
    entry_size = struct.calcsize(ENTRY_FMT)
    index_offset = header_size
    bitmap_offset = index_offset + entry_size * len(entries)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as f:
        f.write(
            struct.pack(
                HEADER_FMT,
                MAGIC,
                1,
                size,
                line_height,
                base_line,
                bpp,
                0,
                len(entries),
                index_offset,
                bitmap_offset,
            )
        )
        for entry in entries:
            f.write(struct.pack(ENTRY_FMT, *entry))
        f.write(bitmaps)

    return len(entries), output.stat().st_size


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", required=True, type=Path)
    parser.add_argument("--text", required=True, type=Path)
    parser.add_argument("--size", required=True, type=int)
    parser.add_argument("--bpp", default=1, type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--keep-dump", type=Path)
    args = parser.parse_args()

    symbols = collect_symbols(args.text)
    dump_dir = args.keep_dump or Path(tempfile.mkdtemp(prefix="lazy_font_dump_"))
    try:
        run_lv_font_conv(args.font, args.size, args.bpp, symbols, dump_dir)
        glyph_count, byte_count = build_lazy_font(dump_dir, args.output, args.size, args.bpp)
        print(f"generated {args.output}: {byte_count} bytes, {glyph_count} glyphs")
    finally:
        if args.keep_dump is None:
            shutil.rmtree(dump_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
