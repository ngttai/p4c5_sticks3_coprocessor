#!/usr/bin/env python3
"""Convert Noto Sans CJK SC to an LVGL font C file (4 bpp, no kerning), with PIL only.

Covers the 3755 level-1 GB2312 hanzi plus CJK and full-width punctuation. Latin and Vietnamese
text falls back to phone_font_<SIZE> (see gen_lv_font.py).

Usage: gen_lv_font_cjk.py FONT.ttc SIZE_PX NAME OUT.c
"""
import sys
from PIL import Image, ImageDraw, ImageFont

def gb2312_level1():
    cps = set()
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                cps.add(ord(bytes([hi, lo]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return cps


CPS = gb2312_level1() | set(range(0x3001, 0x3012)) | set(range(0xFF01, 0xFF5F))
CPS = sorted(CPS)
RANGES = []
for cp in CPS:
    if RANGES and RANGES[-1][1] == cp - 1:
        RANGES[-1] = (RANGES[-1][0], cp)
    else:
        RANGES.append((cp, cp))

ttf, size, name, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
font = ImageFont.truetype(ttf, size, index=2)   # Noto Sans CJK SC
ascent, descent = font.getmetrics()
notdef = bytes(font.getmask("￿", mode="L"))


def glyph(cp):
    ch = chr(cp)
    if cp != 0x20 and cp != 0xA0 and bytes(font.getmask(ch, mode="L")) == notdef:
        return None                                   # not in the font
    adv = round(font.getlength(ch) * 16)
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")    # relative to the baseline origin
    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return dict(adv=adv, w=0, h=0, ox=0, oy=0, px=[])
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
    px = [v >> 4 for v in img.getdata()]
    # trim empty rows and columns
    rows = [px[r * w:(r + 1) * w] for r in range(h)]
    top = 0
    while rows and not any(rows[0]):
        rows.pop(0)
        top += 1
    while rows and not any(rows[-1]):
        rows.pop()
    if not rows:
        return dict(adv=adv, w=0, h=0, ox=0, oy=0, px=[])
    left = min(next(i for i, v in enumerate(r) if v) if any(r) else w for r in rows)
    right = max(max(i for i, v in enumerate(r) if v) if any(r) else -1 for r in rows)
    rows = [r[left:right + 1] for r in rows]
    nh, nw = len(rows), right - left + 1
    return dict(adv=adv, w=nw, h=nh, ox=x0 + left, oy=-(y0 + top + nh), px=[v for r in rows for v in r])


glyphs = []        # (codepoint, glyph)
cmaps = []         # (range_start, range_length, glyph_id_start)
for a, b in RANGES:
    run = None
    for cp in range(a, b + 1):
        g = glyph(cp)
        if g is None:
            run = None
            continue
        gid = len(glyphs) + 1
        glyphs.append((cp, g))
        if run is None:
            run = [cp, 1, gid]
            cmaps.append(run)
        else:
            run[1] += 1

L = []
L.append(f"""/*******************************************************************************
 * Size: {size} px
 * Bpp: 4
 * Font: Noto Sans CJK SC Regular, SIL Open Font License 1.1 (see OFL.txt)
 * Generated with tools/gen_lv_font.py (no kerning)
 ******************************************************************************/

#include "lvgl.h"

extern const lv_font_t phone_font_{size};

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{""")
index = []
pos = 0
for cp, g in glyphs:
    index.append(pos)
    nib = g["px"] + ([0] if len(g["px"]) % 2 else [])
    data = [(nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2)]
    L.append(f"    /* U+{cp:04X} */")
    for i in range(0, len(data), 12):
        L.append("    " + " ".join(f"0x{v:02x}," for v in data[i:i + 12]))
    pos += len(data)
if pos == 0:
    L.append("    0x00")
L.append("};\n")
L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
L.append("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
for (cp, g), bi in zip(glyphs, index):
    L.append(f"    {{.bitmap_index = {bi}, .adv_w = {g['adv']}, .box_w = {g['w']}, .box_h = {g['h']}, "
             f".ofs_x = {g['ox']}, .ofs_y = {g['oy']}}},")
L.append("};\n")
# One sparse cmap: cmap_num is a 9-bit field in LVGL, so thousands of runs would overflow it.
first, last = glyphs[0][0], glyphs[-1][0]
assert last - first < 65536
L.append("static const uint16_t unicode_list[] = {")
offs = [cp - first for cp, _ in glyphs]
for i in range(0, len(offs), 12):
    L.append("    " + " ".join(f"{v}," for v in offs[i:i + 12]))
L.append("};\n")
L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
L.append(f"    {{\n        .range_start = {first}, .range_length = {last - first + 1}, .glyph_id_start = 1,\n"
         f"        .unicode_list = unicode_list, .glyph_id_ofs_list = NULL, .list_length = {len(offs)}, "
         ".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n    },")
L.append("};\n")
L.append(f"""static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {ascent + descent},
    .base_line = {descent},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = {-max(1, round(size * 0.12))},
    .underline_thickness = {max(1, round(size / 14))},
    .dsc = &font_dsc,
    .fallback = &phone_font_{size},     /* Latin, Vietnamese and LVGL symbols */
    .user_data = NULL,
}};
""")
open(out, "w").write("\n".join(L))
print(f"{name}: {len(glyphs)} glyphs, 1 sparse cmap, {pos} bytes, line_height {ascent + descent}, base_line {descent}")
