"""Structural check for the generated LVGL bitmap fonts (stdlib only).

LVGL's PLAIN 4bpp reader (lv_font_fmt_txt.c, `fdsc->bpp == 4`) only realigns the
nibble counter on a new row when `glyph_dsc.stride != 0`. Our generated fonts
leave `stride` at 0, so the bitmap must be one **continuous nibble stream** over
box_w * box_h: a glyph occupies exactly `(box_w * box_h + 1) // 2` bytes.

Packing per-row instead (`(box_w + 1) // 2` bytes per line) makes every row of an
odd-width glyph drift half a pixel to the right, which renders as a sheared
glyph. That shipped once; this check keeps it from coming back. It needs no
Pillow, so it can run in the normal validation pass.

Usage: check_font_bitmap.py [font.c ...]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# 只挑真正含字形的字体源（app_font_16.c / app_font_20.c），排除 app_font_coverage.c。
DEFAULT_FONTS = sorted((ROOT / "assets" / "fonts").glob("app_font_[0-9]*.c"))

DSC_RE = re.compile(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
    r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}")


def check(path: Path) -> int:
    src = path.read_text(encoding="utf-8")

    bitmap_text = re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", src, re.S)
    if not bitmap_text:
        raise SystemExit(f"{path.name}: glyph_bitmap array not found")
    bitmap_len = len(re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text.group(1)))

    dsc = [(int(a), int(b), int(c), int(d), int(e), int(f))
           for a, b, c, d, e, f in DSC_RE.findall(src)]
    if not dsc:
        raise SystemExit(f"{path.name}: glyph descriptor array not found")

    failures = 0
    # dsc[0] 是 LVGL 要求的哨兵（全 0），真正的字形从 dsc[1] 开始。
    for i in range(1, len(dsc)):
        index, _adv, box_w, box_h, _ox, _oy = dsc[i]
        want = (box_w * box_h + 1) // 2
        end = dsc[i + 1][0] if i + 1 < len(dsc) else bitmap_len
        got = end - index
        if got != want:
            print(f"FAIL {path.name}: glyph #{i} ({box_w}x{box_h}) occupies {got} bytes, "
                  f"continuous 4bpp needs {want}"
                  f"{' — looks like per-row padding' if got == ((box_w + 1) // 2) * box_h else ''}")
            failures += 1
            if failures > 5:
                print(f"FAIL {path.name}: too many mismatching glyphs, stopping")
                break

    if dsc[-1][0] > bitmap_len:
        print(f"FAIL {path.name}: last glyph starts at {dsc[-1][0]} "
              f"past the {bitmap_len}-byte bitmap")
        failures += 1

    if failures == 0:
        print(f"Font bitmap layout: PASS ({path.name}, "
              f"{len(dsc) - 1} glyphs, {bitmap_len} bytes)")
    return failures


def main() -> int:
    fonts = [Path(a) for a in sys.argv[1:]] or DEFAULT_FONTS
    if not fonts:
        print("no font sources found", file=sys.stderr)
        return 1
    total = 0
    for font in fonts:
        total += check(font)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
