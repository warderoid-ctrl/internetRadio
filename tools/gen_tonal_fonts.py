#!/usr/bin/env python3
"""Build the Tonal UI's LVGL fonts: Inter / Inter Tight text fonts and Lucide icon fonts.

Text fonts come from the pinned Fontsource npm packages (static weights, latin subset = Basic Latin +
Latin-1 + common punctuation), icons from the pinned lucide-static package. LVGL's own converter
(lv_font_conv) turns them into C, 4 bpp, uncompressed.

- Text fonts fall back to the nearest built-in Montserrat, so LV_SYMBOL_* (keyboard, msgbox...) still draw.
- Icon fonts hold only the icons in ICONS and fall back to a text font, so a label in I_20 draws
  LUCIDE_PLUS " Add": icon at 20 px, text in Inter 16. Their line box is the text font's (grown to fit
  the icon) and icons are centred in it, so they sit level with the text.
- Digits are made tabular (equal advance), so ticking clocks and times don't shuffle sideways.

The generated .c files are committed: a firmware build needs neither Node nor network, only regenerating does.

Usage:  python tools/gen_tonal_fonts.py
Needs:  node + npx (fetches the packages and lv_font_conv on first run)
Writes: fonts/tonal_*.c, fonts/LICENSE-*.txt, tonal_fonts.h

To add an icon: find its name on lucide.dev, add it to ICONS, rerun.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

INTER = "@fontsource/inter@5.3.0"
INTER_TIGHT = "@fontsource/inter-tight@5.3.0"
LUCIDE = "lucide-static@1.61.0"
LV_FONT_CONV = "lv_font_conv@1.5.3"

LATIN = "0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x20AC,0x2212"
CLOCK = "0x2D,0x30-0x3A"                     # - 0-9 :
HERO = "0x2D,0x30-0x39,0xB0,0x2212"          # - 0-9 ° −

# name, family, weight, size, glyphs, Montserrat fallback
TEXT = [
    ("tonal_clock_128", "inter-tight", 600, 128, CLOCK, None),
    ("tonal_night_236", "inter-tight", 300, 236, CLOCK, None),
    ("tonal_hero_112", "inter-tight", 600, 112, HERO, None),
    ("tonal_hero_72", "inter-tight", 600, 72, HERO, None),
    ("tonal_t34", "inter-tight", 600, 34, LATIN, 40),
    ("tonal_t26", "inter-tight", 700, 26, LATIN, 28),
    ("tonal_t22", "inter-tight", 700, 22, LATIN, 24),
    ("tonal_l20", "inter", 600, 20, LATIN, 20),
    ("tonal_m16", "inter", 600, 16, LATIN, 16),
    ("tonal_s14", "inter", 500, 14, LATIN, 14),
    ("tonal_xs12", "inter", 500, 12, LATIN, 14),
]
WEATHER_ICONS = ["sun", "moon", "cloud", "cloud-sun", "cloud-moon", "cloud-rain", "cloud-drizzle", "cloud-snow",
                 "cloud-fog", "cloud-lightning"]
# name, size, text font it falls back to, icons (None = all of ICONS)
ICON_FONTS = [
    ("tonal_i20", 20, "tonal_m16", None),
    ("tonal_i24", 24, "tonal_l20", None),
    ("tonal_i28", 28, "tonal_l20", None),
    ("tonal_i56", 56, "tonal_t34", WEATHER_ICONS),   # the big weather glyphs only
]

ICONS = [
    # navigation
    "grid-2x2", "radio", "podcast", "cloud-sun", "house", "timer", "image", "settings", "mic", "calendar",
    "chevron-left", "chevron-right", "chevron-up", "chevron-down", "arrow-left", "x", "check", "ellipsis",
    # player
    "play", "pause", "skip-back", "skip-forward", "rotate-ccw", "rotate-cw", "volume-1", "volume-2", "volume-x",
    "shuffle", "repeat", "list", "list-music", "music", "heart", "star", "history", "rss", "globe",
    # weather
    "sun", "moon", "cloud", "cloud-moon", "cloud-rain", "cloud-drizzle", "cloud-snow", "cloud-fog",
    "cloud-lightning", "wind", "umbrella", "droplets", "sunrise", "sunset", "thermometer",
    # home assistant
    "lightbulb", "plug", "lock", "lock-open", "tv", "fan", "power", "zap", "door-open", "toggle-left", "toggle-right",
    # general
    "wifi", "wifi-off", "search", "plus", "minus", "pencil", "trash-2", "map-pin", "refresh-cw", "triangle-alert",
    "info", "upload", "eye", "eye-off", "bell", "alarm-clock", "hourglass", "smartphone", "qr-code", "sun-moon",
    "palette", "sun-dim", "clock", "delete", "message-square", "send", "satellite-dish",
]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(HERE, "fonts")
HEADER = os.path.join(HERE, "tonal_fonts.h")


def run(cmd, cwd=None):
    cmd = [shutil.which(cmd[0]) or cmd[0]] + cmd[1:]   # npm / npx are .cmd scripts on Windows
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("failed: " + " ".join(cmd))
    return r.stdout


def fetch(pkg, workdir):
    """npm package unpacked into workdir/<dirname>; returns its root."""
    tgz = run(["npm", "pack", pkg, "--silent"], cwd=workdir).strip().splitlines()[-1]
    dest = os.path.join(workdir, re.sub(r"\W", "_", pkg))
    with tarfile.open(os.path.join(workdir, tgz)) as tar:
        tar.extractall(dest)
    return os.path.join(dest, "package")


def metrics(c_src):
    return (int(re.search(r"\.line_height = (\d+)", c_src).group(1)),
            int(re.search(r"\.base_line = (-?\d+)", c_src).group(1)))


GLYPH = re.compile(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), "
                   r"\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}")


CMAP = re.compile(r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*"
                  r"\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), \.list_length = (\d+), "
                  r"\.type = LV_FONT_FMT_TXT_CMAP_(\w+)")


def glyph_ids(c_src):
    """{codepoint: glyph id} from the font's cmaps (all four lv_font_conv formats)."""
    def array(name):
        body = re.search(r"%s\[\] = \{([^}]*)\}" % name, c_src).group(1)
        return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", re.sub(r"/\*.*?\*/", "", body))]
    ids = {}
    for m in CMAP.finditer(c_src):
        start, length, gid, ulist, ofs, n, kind = m.groups()
        start, length, gid = int(start), int(length), int(gid)
        ofs = array(ofs) if ofs != "NULL" else None
        if kind.startswith("FORMAT0"):
            for i in range(length):
                ids[start + i] = gid + (ofs[i] if ofs else i)
        else:                                     # SPARSE_TINY / SPARSE_FULL
            for i, d in enumerate(array(ulist)):
                ids[start + d] = gid + (ofs[i] if ofs else i)
    return ids


def tabular_digits(c_src):
    """Give 0-9 the widest digit's advance, each glyph centred in it."""
    cmap = glyph_ids(c_src)
    ids = [cmap[cp] for cp in range(0x30, 0x3A)]
    glyphs = list(GLYPH.finditer(c_src))          # glyph_dsc[] in order: index = glyph id
    widest = max(int(glyphs[i].group(2)) for i in ids)
    out, last = [], 0
    for i in ids:
        g = glyphs[i]
        bi, adv, bw, bh, ox, oy = (int(x) for x in g.groups())
        ox += round((widest - adv) / 16 / 2)      # adv_w is in 1/16 px
        out.append(c_src[last:g.start()])
        out.append("{.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}"
                   % (bi, widest, bw, bh, ox, oy))
        last = g.end()
    out.append(c_src[last:])
    return "".join(out)


def fit_icons(c_src, size, fb_metrics):
    """Line box = the fallback text font's, grown to fit the icon; icons centred in it."""
    lh_f, bl_f = fb_metrics
    line_h = max(lh_f, size + 2)
    base = bl_f + (line_h - lh_f) // 2
    shift = math.floor(line_h / 2.0 - base - size / 2.0)
    c_src = re.sub(r"\.line_height = \d+,", ".line_height = %d," % line_h, c_src)
    c_src = re.sub(r"\.base_line = -?\d+,", ".base_line = %d," % base, c_src)
    return re.sub(r"\.ofs_y = (-?\d+)\}", lambda m: ".ofs_y = %d}" % (int(m.group(1)) + shift), c_src)


NOTICE = """/*
 * %s -- GENERATED by tools/gen_tonal_fonts.py, do not edit.
 * %s
 * Licence: %s (alongside this file).
 */

"""


def convert(work, name, font_file, ranges, size, fallback):
    cmd = ["npx", "--yes", LV_FONT_CONV, "--font", font_file, "-r", ranges, "--size", str(size),
           "--bpp", "4", "--format", "lvgl", "--no-compress", "--no-kerning",
           "--lv-include", "lvgl.h", "--lv-font-name", name, "-o", name + ".c"]
    if fallback:
        cmd += ["--lv-fallback", fallback]
    run(cmd, cwd=work)
    return open(os.path.join(work, name + ".c"), encoding="utf-8").read()


def write(name, c_src, what, licence):
    open(os.path.join(FONT_DIR, name + ".c"), "w", encoding="utf-8", newline="\n").write(
        NOTICE % (name, what, licence) + c_src)
    print("wrote fonts/%s.c" % name)


def macro(icon):
    return "LUCIDE_" + icon.upper().replace("-", "_")


def utf8(cp):
    return "".join("\\x%02X" % b for b in chr(cp).encode("utf-8"))


def main():
    os.makedirs(FONT_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        pkgs = {"inter": fetch(INTER, work), "inter-tight": fetch(INTER_TIGHT, work)}
        lucide = fetch(LUCIDE, work)
        shutil.copy(os.path.join(pkgs["inter"], "LICENSE"), os.path.join(FONT_DIR, "LICENSE-inter.txt"))
        shutil.copy(os.path.join(pkgs["inter-tight"], "LICENSE"), os.path.join(FONT_DIR, "LICENSE-inter-tight.txt"))
        shutil.copy(os.path.join(lucide, "LICENSE"), os.path.join(FONT_DIR, "LICENSE-lucide.txt"))

        # bare filenames inside the temp dir: lv_font_conv records its command line in the output
        text_metrics = {}
        for name, fam, wght, size, ranges, mont in TEXT:
            fn = "%s-%d.woff" % (fam, wght)
            shutil.copy(os.path.join(pkgs[fam], "files", "%s-latin-%d-normal.woff" % (fam, wght)), os.path.join(work, fn))
            c_src = convert(work, name, fn, ranges, size, "lv_font_montserrat_%d" % mont if mont else None)
            c_src = tabular_digits(c_src)
            text_metrics[name] = metrics(c_src)
            title = {"inter": "Inter", "inter-tight": "Inter Tight"}[fam]
            write(name, c_src, "%s %d, %d px, from %s" % (title, wght, size, INTER if fam == "inter" else INTER_TIGHT),
                  "SIL Open Font License 1.1, LICENSE-%s.txt" % fam)

        codepoints = json.load(open(os.path.join(lucide, "font", "codepoints.json"), encoding="utf-8"))
        missing = [n for n in ICONS if n not in codepoints]
        if missing:
            sys.exit("not in %s: %s" % (LUCIDE, ", ".join(missing)))
        shutil.copy(os.path.join(lucide, "font", "lucide.ttf"), os.path.join(work, "lucide.ttf"))
        for name, size, fb, icons in ICON_FONTS:
            ranges = ",".join("0x%X" % codepoints[n] for n in (icons or ICONS))
            c_src = fit_icons(convert(work, name, "lucide.ttf", ranges, size, fb), size, text_metrics[fb])
            write(name, c_src, "Lucide icons (lucide.dev), %d px, from %s; text falls back to %s" % (size, LUCIDE, fb),
                  "ISC (some icons MIT, from Feather), LICENSE-lucide.txt")

    width = max(len(macro(n)) for n in ICONS)
    lines = [
        "#pragma once",
        "// Tonal UI fonts: Inter / Inter Tight (SIL OFL 1.1) and Lucide icons (ISC). Licences in fonts/.",
        "// GENERATED by tools/gen_tonal_fonts.py -- edit the lists there and rerun rather than editing this file.",
        "//",
        "// Icons only draw in a tonal_i* font; those fall back to a text font, so LUCIDE_PLUS \" Add\" works in one label.",
        "// tonal_i56 holds only the weather icons.",
        "",
        'extern "C" {',
    ]
    lines += ["extern const lv_font_t %s;" % n[0] for n in TEXT + ICON_FONTS]
    lines += ["}", ""]
    for n in ICONS:
        lines.append('#define %-*s "%s" // %s' % (width, macro(n), utf8(codepoints[n]), n))
    open(HEADER, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("wrote tonal_fonts.h (%d icons)" % len(ICONS))


if __name__ == "__main__":
    main()
