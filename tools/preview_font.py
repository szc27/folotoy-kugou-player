"""Render a text sample straight out of a generated LVGL font C file.

Used to eyeball glyph packing without flashing the device: it decodes the
bitmap exactly the way lv_font_fmt_txt.c does for LV_FONT_FMT_TXT_PLAIN at
4 bpp (continuous nibble stream, high nibble first), then paints the glyphs
on a baseline using ofs_x / ofs_y.

Usage: preview_font.py <font.c> <out.png> <text>
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

font_c, out_png, text = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]

src = font_c.read_text(encoding="utf-8")
bitmap = bytes(int(v, 16) for v in re.findall(
    r"0x([0-9a-fA-F]{2})", re.search(r"glyph_bitmap\[\] = \{(.*?)\n\};", src, re.S).group(1)))
dsc = [(int(a), int(b), int(c), int(d), int(e), int(f)) for a, b, c, d, e, f in re.findall(
    r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
    r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)]

ascii_list = re.search(r"range_start = (\d+), \.range_length = (\d+), "
                       r"\.glyph_id_start = (\d+),\s*\.unicode_list = NULL", src)
sparse_list = re.search(r"unicode_list_1\[\] = \{(.*?)\n\};", src, re.S)
sparse_cmap = re.search(r"range_start = (\d+), \.range_length = (\d+), "
                        r"\.glyph_id_start = (\d+),\s*\.unicode_list = unicode_list_1", src)
unicode_list = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{4})", sparse_list.group(1))]
ascii_start, ascii_len, ascii_gid = (int(g) for g in ascii_list.groups())
sparse_start, sparse_len, sparse_gid = (int(g) for g in sparse_cmap.groups())


def glyph_id(cp: int):
    if ascii_start <= cp < ascii_start + ascii_len:
        return ascii_gid + (cp - ascii_start)
    if sparse_start <= cp < sparse_start + sparse_len:
        try:
            return sparse_gid + unicode_list.index(cp - sparse_start)
        except ValueError:
            return None
    return None


def pixels(gid: int, box_w: int, box_h: int):
    """Decode exactly like lv_font_fmt_txt.c: no per-row realignment."""
    base = dsc[gid][0]
    out = [[0] * box_w for _ in range(box_h)]
    for k in range(box_w * box_h):
        byte = bitmap[base + k // 2]
        out[k // box_w][k % box_w] = (byte >> 4) if k % 2 == 0 else (byte & 0x0F)
    return out


line_height = int(re.search(r"\.line_height = (\d+)", src).group(1))
base_line = int(re.search(r"\.base_line = (\d+)", src).group(1))
scale = 6  # blow up so crooked rows are obvious

# 只看"字形本身有没有被装错"：每个字按 box 顶部对齐排列，不做基线定位
# （基线公式和 LVGL 内部一致性的问题由固件端负责，这里只验证位图打包）。
glyphs = []
total_w = 2
for ch in text:
    gid = glyph_id(ord(ch))
    if gid is None or gid >= len(dsc):
        continue
    _, adv_w, box_w, box_h, ofs_x, ofs_y = dsc[gid]
    glyphs.append((ch, gid, box_w, box_h))
    total_w += max(adv_w // 16, box_w) + 2

w = max(total_w + 8, 40)
h = max([g[3] for g in glyphs] + [line_height]) + 4

img = Image.new("L", (w, h), 255)
draw = ImageDraw.Draw(img)
pen = 2
for ch, gid, box_w, box_h in glyphs:
    if box_w and box_h:
        px = pixels(gid, box_w, box_h)
        for y in range(box_h):
            for x in range(box_w):
                if px[y][x]:
                    draw.point((pen + x, 2 + y), fill=255 - px[y][x] * 17)
    pen += box_w + 2

canvas = img.resize((w * scale, h * scale), Image.NEAREST)
canvas.save(out_png)
widths = " ".join(f"{ch}:{bw}x{bh}{'*' if bw % 2 else ''}" for ch, _, bw, bh in glyphs)
print(f"{out_png}: {w}x{h} px (x{scale}), line_height={line_height} base_line={base_line}")
print(f"  glyph boxes (* = odd width, the case that used to shear): {widths}")
