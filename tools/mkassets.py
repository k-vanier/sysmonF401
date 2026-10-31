"""Génère les ressources du firmware : polices lissées et animation GIF.

    python mkassets.py <animation.gif> <dossier du projet CubeIDE>

Écrit Core/Inc/assets.h et Core/Src/assets.c. Nécessite Pillow.

Polices : Go fonts (licence BSD, golang.org/x/image/font/gofont), cherchées dans
le cache des modules Go ou dans le dossier donné par GOFONTS. Chaque glyphe est
rendu dans une cellule de sa largeur d'avance, alpha sur 4 bits.

Animation : chaque image est recadrée sur le sujet (union des zones non blanches
de toutes les images, légende séparée exclue) et réduite à ANIM_H de haut. Le fond
blanc est gardé : le firmware affiche l'animation sur une carte blanche. Toutes
les images partagent une palette ; chaque image est compressée en RLE :
  octet n < 0x80 : n+1 pixels de l'indice qui suit
  octet n >= 0x80 : (n & 0x7F)+1 indices littéraux qui suivent
"""

import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageSequence

ANIM_H = 120              # hauteur de l'animation à l'écran
ANIM_FRAMES_STEP = 2      # 2 = une image sur deux (la flash ne loge pas les 32)
ANIM_COLORS = 64

# Caractères spéciaux : codes de contrôle du firmware -> caractère dessiné.
UP, DOWN, DEGREE = chr(1), chr(2), chr(0x7F)
SPECIAL = {UP: chr(0x2191), DOWN: chr(0x2193), DEGREE: chr(0xB0)}  # flèches, degré
# (nom C, fichier, taille en px, caractères, hauteur gardée sous la ligne de base ou None)
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
FONTS = [
    ("font_small", "Go-Medium.ttf", 14, ASCII + UP + DOWN + DEGREE, None),
    ("font_medium", "Go-Bold.ttf", 18, ASCII + UP + DOWN + DEGREE, None),
    ("font_large", "Go-Bold.ttf", 28, "0123456789%.-C " + DEGREE, 1),  # chiffres : rien dessous
]


def gofonts_dir():
    if os.environ.get("GOFONTS"):
        return os.environ["GOFONTS"]
    home = os.path.expanduser("~")
    hits = sorted(glob.glob(os.path.join(home, "go", "pkg", "mod", "golang.org", "x",
                                         "image@*", "font", "gofont", "ttfs")))
    if not hits:
        sys.exit("Go fonts introuvables : go mod download golang.org/x/image, ou GOFONTS=<dossier ttfs>")
    return hits[-1]


def c_bytes(data, indent="  "):
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def make_font(name, path, size, chars, below=None):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    height = ascent + (descent if below is None else below)
    glyph_map = [0xFF] * 128
    widths, offsets, data = [], [], bytearray()
    for i, ch in enumerate(chars):
        real = SPECIAL.get(ch, ch)
        w = max(1, round(font.getlength(real)))
        img = Image.new("L", (w, height), 0)
        ImageDraw.Draw(img).text((0, ascent), real, font=font, fill=255, anchor="ls")
        glyph_map[ord(ch)] = i
        widths.append(w)
        offsets.append(len(data))
        px = img.load()
        for y in range(height):
            row = [(px[x, y] + 8) // 17 for x in range(w)]  # 0..15
            if len(row) % 2:
                row.append(0)
            data.extend((row[k] << 4) | row[k + 1] for k in range(0, len(row), 2))
    c = [f"/* {os.path.basename(path)} {size} px, {len(chars)} glyphes */"]
    c.append(f"static const uint8_t {name}_data[{len(data)}] = {{\n{c_bytes(data)}\n}};")
    c.append(f"static const uint16_t {name}_offset[] = {{{', '.join(map(str, offsets))}}};")
    c.append(f"static const uint8_t {name}_width[] = {{{', '.join(map(str, widths))}}};")
    c.append(f"static const uint8_t {name}_map[128] = {{{', '.join(map(str, glyph_map))}}};")
    c.append(f"const Font {name} = {{{height}, {ascent}, {name}_map, {name}_width, "
             f"{name}_offset, {name}_data}};")
    return "\n".join(c), len(data) + 3 * len(chars) + 128


def make_anim(path):
    src = Image.open(path)
    frames = [f.convert("RGB") for f in ImageSequence.Iterator(src)][::ANIM_FRAMES_STEP]
    durations = [f.info.get("duration", 100) for f in ImageSequence.Iterator(Image.open(path))]
    duration = durations[0] * ANIM_FRAMES_STEP
    # Recadrage : union des zones non blanches de toutes les images, réduite au plus
    # haut bloc de lignes non vides (écarte une légende séparée par une bande blanche).
    ink = None
    for f in frames:
        m = f.convert("L").point(lambda v: 255 if v < 235 else 0)
        ink = m if ink is None else Image.composite(m, ink, m)
    rows = [ink.crop((0, y, ink.width, y + 1)).getbbox() is not None for y in range(ink.height)]
    best, start = (0, 0), None
    for y, r in enumerate(rows + [False]):
        if r and start is None:
            start = y
        elif not r and start is not None:
            if y - start > best[1] - best[0]:
                best = (start, y)
            start = None
    b = ink.crop((0, best[0], ink.width, best[1])).getbbox()
    bbox = (b[0], best[0] + b[1], b[2], best[0] + b[3])
    if os.environ.get("ANIM_CROP"):  # x0,y0,x1,y1 pour forcer le cadre
        bbox = tuple(int(v) for v in os.environ["ANIM_CROP"].split(","))
    w = round((bbox[2] - bbox[0]) * ANIM_H / (bbox[3] - bbox[1]))
    out = []
    for f in frames:
        out.append(f.crop(bbox).resize((w, ANIM_H), Image.LANCZOS))
    # Palette commune : quantification d'une planche de toutes les images.
    sheet = Image.new("RGB", (w, ANIM_H * len(out)))
    for i, f in enumerate(out):
        sheet.paste(f, (0, i * ANIM_H))
    sheet = sheet.quantize(colors=ANIM_COLORS, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = sheet.getpalette()[:3 * ANIM_COLORS]
    palette = [rgb565(tuple(pal[3 * i:3 * i + 3])) for i in range(len(pal) // 3)]
    idx = list(sheet.get_flattened_data())
    frames_rle, offsets = bytearray(), []
    for i in range(len(out)):
        offsets.append(len(frames_rle))
        frames_rle.extend(rle(idx[i * w * ANIM_H:(i + 1) * w * ANIM_H]))
    c = [f"/* {os.path.basename(path)} : {len(out)} images {w}x{ANIM_H}, {duration} ms, "
         f"{len(palette)} couleurs */"]
    c.append(f"static const uint16_t anim_palette[] = {{{', '.join(f'0x{p:04X}' for p in palette)}}};")
    c.append(f"static const uint32_t anim_offset[] = {{{', '.join(map(str, offsets))}}};")
    c.append(f"static const uint8_t anim_rle[{len(frames_rle)}] = {{\n{c_bytes(frames_rle)}\n}};")
    c.append(f"const Anim anim = {{{w}, {ANIM_H}, {len(out)}, {duration}, anim_palette, "
             f"anim_offset, anim_rle}};")
    preview = sheet.convert("RGB")
    return "\n".join(c), len(frames_rle) + 2 * len(palette), preview, (w, len(out))


def rle(idx):
    out = bytearray()
    i, n = 0, len(idx)
    lit = []

    def flush():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            out.append(0x80 | (len(chunk) - 1))
            out.extend(chunk)

    while i < n:
        j = i
        while j < n and j - i < 128 and idx[j] == idx[i]:
            j += 1
        if j - i >= 3:
            flush()
            out.append(j - i - 1)
            out.append(idx[i])
            i = j
        else:
            lit.append(idx[i])
            i += 1
    flush()
    return out


HEADER = """/* Généré par tools/mkassets.py : ne pas modifier à la main. */
#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>

/* Police lissée : alpha 4 bits, 2 pixels par octet, une cellule par glyphe. */
typedef struct {
  uint8_t height, ascent;
  const uint8_t *map;      /* code ASCII -> numéro de glyphe, 0xFF si absent */
  const uint8_t *width;    /* largeur (= avance) de chaque glyphe */
  const uint16_t *offset;  /* début de chaque glyphe dans data */
  const uint8_t *data;
} Font;

/* Animation : palette RGB565 commune, images compressées en RLE (voir mkassets.py). */
typedef struct {
  uint16_t width, height, frames, duration_ms;
  const uint16_t *palette;
  const uint32_t *offset;
  const uint8_t *rle;
} Anim;

/* Caractères spéciaux des polices small et medium (et ° dans large). */
#define CH_UP     "\\x01"
#define CH_DOWN   "\\x02"
#define CH_DEGREE "\\x7f"

extern const Font font_small, font_medium, font_large;
extern const Anim anim;

#endif
"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    gif, proj = sys.argv[1], sys.argv[2]
    fdir = gofonts_dir()
    parts, total = [], 0
    for name, file, size, chars, below in FONTS:
        code, n = make_font(name, os.path.join(fdir, file), size, chars, below)
        parts.append(code)
        total += n
        print(f"{name}: {n} octets")
    code, n, preview, (w, nf) = make_anim(gif)
    parts.append(code)
    total += n
    print(f"anim: {nf} images {w}x{ANIM_H}, {n} octets")
    print(f"total: {total} octets")
    with open(os.path.join(proj, "Core", "Inc", "assets.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
    with open(os.path.join(proj, "Core", "Src", "assets.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Généré par tools/mkassets.py : ne pas modifier à la main.\n"
                " * Polices : Go fonts, licence BSD (golang.org/x/image/font/gofont). */\n"
                '#include "assets.h"\n\n' + "\n\n".join(parts) + "\n")
    if os.environ.get("ANIM_PREVIEW"):
        preview.save(os.environ["ANIM_PREVIEW"])


if __name__ == "__main__":
    main()
