"""Génère les ressources du firmware : polices lissées et animation GIF.

    python mkassets.py [options] <animation.gif> <dossier du projet CubeIDE>

Écrit Core/Inc/assets.h et Core/Src/assets.c. Nécessite Pillow.

Polices : Go fonts (licence BSD, golang.org/x/image/font/gofont), cherchées dans
le cache des modules Go ou dans le dossier donné par GOFONTS. Chaque glyphe est
rendu dans une cellule de sa largeur d'avance, alpha sur 4 bits.

Animation : chaque image est recadrée (--crop, sinon union des zones non blanches
de toutes les images, légende séparée exclue), réduite pour tenir dans
--max-w x --max-h, éclaircie si besoin (--gamma < 1 : tons moyens plus clairs, blanc
et noir inchangés), et son fond presque blanc est ramené au blanc pur de l'écran.
Toutes les images partagent une palette. Les pixels sont rangés colonne par
colonne (de gauche à droite, chaque colonne de haut en bas : l'axe de balayage de
la dalle en paysage, ce qui limite le déchirement).

La première image est stockée entière (key), puis chaque image n'est stockée que
par ses différences avec la précédente (delta[i] : image i-1 -> i, delta[0] :
dernière -> première). Une couleur proche de celle déjà affichée est gardée telle
quelle, ce qui écarte le bruit de compression du GIF. Codage :
  octet 0x00-0x7F : n+1 pixels de l'indice qui suit
  octet 0x80-0xBF : (n & 0x3F)+1 indices littéraux qui suivent
  octet 0xC0-0xFF : (n & 0x3F)+1 pixels inchangés (deltas seulement)
"""

import argparse
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageSequence

# Caractères spéciaux : codes de contrôle du firmware -> caractère dessiné.
UP, DOWN, TRI_UP, TRI_DOWN, DEGREE = chr(1), chr(2), chr(3), chr(4), chr(0x7F)
SPECIAL = {UP: chr(0x2191), DOWN: chr(0x2193), TRI_UP: chr(0x25B2), TRI_DOWN: chr(0x25BC),
           DEGREE: chr(0xB0)}  # flèches, triangles, degré
EXTRA = UP + DOWN + TRI_UP + TRI_DOWN + DEGREE
# (nom C, fichier, taille en px, caractères, lignes gardées sous la ligne de base ou None)
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
FONTS = [
    ("font_small", "Go-Medium.ttf", 14, ASCII + EXTRA, None),
    ("font_medium", "Go-Bold.ttf", 18, ASCII + EXTRA, None),
    # chiffres et tailles (87G, 2/3, inf) : ni dessous ni vide au-dessus
    ("font_large", "Go-Bold.ttf", 28, "0123456789%.-/? CGKMTinf" + DEGREE, 1),
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
    """below : lignes gardées sous la ligne de base (None : toute la descente). Avec
    below, les lignes vides au-dessus de tous les glyphes sont retirées aussi : une
    police de chiffres n'occupe alors que la hauteur de ses chiffres."""
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    height = ascent + (descent if below is None else below)
    imgs = []
    for ch in chars:
        real = SPECIAL.get(ch, ch)
        w = max(1, round(font.getlength(real)))
        img = Image.new("L", (w, height), 0)
        ImageDraw.Draw(img).text((0, ascent), real, font=font, fill=255, anchor="ls")
        imgs.append(img)
    if below is not None:
        top = min((im.getbbox() or (0, height, 0, height))[1] for im in imgs)
        imgs = [im.crop((0, top, im.width, height)) for im in imgs]
        ascent, height = ascent - top, height - top
    glyph_map = [0xFF] * 128
    widths, offsets, data = [], [], bytearray()
    for i, (ch, img) in enumerate(zip(chars, imgs)):
        w = img.width
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


def auto_crop(frames):
    """Union des zones non blanches, réduite au plus haut bloc de lignes non vides
    (écarte une légende séparée du sujet par une bande blanche)."""
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
    return (b[0], best[0] + b[1], b[2], best[0] + b[3])


def whiten(f):
    """Fond presque blanc (gris très clair, peu saturé) -> blanc pur."""
    px = f.load()
    for y in range(f.height):
        for x in range(f.width):
            r, g, b = px[x, y]
            if min(r, g, b) > 232 and max(r, g, b) - min(r, g, b) < 14:
                px[x, y] = (255, 255, 255)
    return f


def encode(seq, prev=None):
    out = bytearray()
    lit = []

    def flush():
        while lit:
            chunk = lit[:64]
            del lit[:64]
            out.append(0x80 | (len(chunk) - 1))
            out.extend(chunk)

    i, n = 0, len(seq)
    while i < n:
        if prev is not None and seq[i] == prev[i]:
            flush()
            j = i
            while j < n and seq[j] == prev[j]:
                j += 1
            k = j - i
            while k:
                m = min(k, 64)
                out.append(0xC0 | (m - 1))
                k -= m
            i = j
            continue
        j = i
        while j < n and j - i < 128 and seq[j] == seq[i]:
            j += 1
        if j - i >= 3:
            flush()
            out.append(j - i - 1)
            out.append(seq[i])
            i = j
        else:
            lit.append(seq[i])
            i += 1
    flush()
    return out


def make_anim(path, step, crop, max_w, max_h, colors, noise, gamma):
    src = [f.convert("RGB") for f in ImageSequence.Iterator(Image.open(path))]
    durations = [f.info.get("duration", 100) for f in ImageSequence.Iterator(Image.open(path))]
    duration = round(sum(durations) / len(durations) * step)
    frames = src[::step]
    bbox = crop or auto_crop(frames)
    cw, ch = bbox[2] - bbox[0], bbox[3] - bbox[1]
    scale = min(max_w / cw, max_h / ch)
    w, h = round(cw * scale), round(ch * scale)
    lut = [round(255 * (v / 255) ** gamma) for v in range(256)] * 3
    out = [whiten(f.crop(bbox).resize((w, h), Image.LANCZOS).point(lut)) for f in frames]

    # Palette commune : quantification d'une planche de toutes les images. La
    # couleur la plus fréquente (le fond) est forcée au blanc pur s'il est clair.
    sheet = Image.new("RGB", (w, h * len(out)))
    for i, f in enumerate(out):
        sheet.paste(f, (0, i * h))
    q = sheet.quantize(colors=colors, method=Image.MEDIANCUT, dither=Image.NONE)
    pal = q.getpalette()[:3 * colors]
    idx = list(q.get_flattened_data())
    bg = max(set(idx), key=idx.count)
    if min(pal[3 * bg:3 * bg + 3]) > 230:
        pal[3 * bg:3 * bg + 3] = [255, 255, 255]
    ncol = len(pal) // 3
    palette = [rgb565(tuple(pal[3 * i:3 * i + 3])) for i in range(ncol)]

    def far(a, b):
        return sum((pal[3 * a + c] - pal[3 * b + c]) ** 2 for c in range(3)) >= noise

    n = w * h
    cols = []
    for i in range(len(out)):
        img = idx[i * n:(i + 1) * n]
        cols.append([img[y * w + x] for x in range(w) for y in range(h)])

    key = encode(cols[0])
    shown = list(cols[0])
    deltas = {}
    for i in list(range(1, len(cols))) + [0]:
        # Retour à la première image : exact, pour que chaque tour reparte de key.
        # Un pixel qui redevient fond est toujours réécrit : sinon des traces claires
        # (restes d'effets) proches du blanc resteraient affichées.
        new = list(cols[0]) if i == 0 else [v if v == bg or far(v, s) else s
                                            for v, s in zip(cols[i], shown)]
        deltas[i] = encode(new, shown)
        shown = new
    data, offsets = bytearray(), []
    for i in range(len(cols)):
        offsets.append(len(data))
        data.extend(deltas[i])

    c = [f"/* {os.path.basename(path)} : {len(cols)} images {w}x{h}, {duration} ms, "
         f"{ncol} couleurs, cadre {bbox} */"]
    c.append(f"static const uint16_t anim_palette[] = {{{', '.join(f'0x{p:04X}' for p in palette)}}};")
    c.append(f"static const uint8_t anim_key[{len(key)}] = {{\n{c_bytes(key)}\n}};")
    c.append(f"static const uint32_t anim_offset[] = {{{', '.join(map(str, offsets))}}};")
    c.append(f"static const uint8_t anim_delta[{len(data)}] = {{\n{c_bytes(data)}\n}};")
    c.append(f"const Anim anim = {{{w}, {h}, {len(cols)}, {duration}, anim_palette, anim_key, "
             f"anim_offset, anim_delta}};")
    size = len(key) + len(data) + 4 * len(offsets) + 2 * ncol
    return "\n".join(c), size, q.convert("RGB"), (w, h, len(cols), duration)


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

/* Animation : palette RGB565 commune, pixels rangés colonne par colonne, première
 * image entière (key) puis différences d'une image à l'autre (voir mkassets.py). */
typedef struct {
  uint16_t width, height, frames, duration_ms;
  const uint16_t *palette;
  const uint8_t *key;
  const uint32_t *offset; /* offset[i] : début dans delta du passage à l'image i */
  const uint8_t *delta;
} Anim;

#define ANIM_PIXELS %d /* width * height, pour le tampon de l'image affichée */

/* Caractères spéciaux des polices small et medium (et ° dans large). */
#define CH_UP     "\\x01"
#define CH_DOWN   "\\x02"
#define CH_TRI_UP   "\\x03" /* triangle plein vers le haut */
#define CH_TRI_DOWN "\\x04"
#define CH_DEGREE "\\x7f"

extern const Font font_small, font_medium, font_large;
extern const Anim anim;

#endif
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gif")
    ap.add_argument("project")
    ap.add_argument("--step", type=int, default=2, help="garde une image sur N (défaut 2)")
    ap.add_argument("--crop", help="cadre x0,y0,x1,y1 dans le GIF (défaut : automatique)")
    ap.add_argument("--max-w", type=int, default=136, help="largeur maximale à l'écran (défaut 136)")
    ap.add_argument("--max-h", type=int, default=120, help="hauteur maximale à l'écran (défaut 120)")
    ap.add_argument("--colors", type=int, default=64, help="taille de la palette (défaut 64)")
    ap.add_argument("--noise", type=int, default=300,
                    help="écart de couleur (somme des carrés RVB) sous lequel un pixel affiché est gardé (défaut 300)")
    ap.add_argument("--gamma", type=float, default=1.0,
                    help="correction gamma, < 1 éclaircit les tons moyens (défaut 1, aucune)")
    ap.add_argument("--preview", help="écrit la planche des images quantifiées dans ce PNG")
    a = ap.parse_args()
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None

    fdir = gofonts_dir()
    parts, total = [], 0
    for name, file, size, chars, below in FONTS:
        code, n = make_font(name, os.path.join(fdir, file), size, chars, below)
        parts.append(code)
        total += n
        print(f"{name}: {n} octets")
    code, n, preview, (w, h, nf, dur) = make_anim(a.gif, a.step, crop, a.max_w, a.max_h, a.colors, a.noise, a.gamma)
    parts.append(code)
    total += n
    print(f"anim: {nf} images {w}x{h}, {dur} ms, {n} octets")
    print(f"total: {total} octets")
    with open(os.path.join(a.project, "Core", "Inc", "assets.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (w * h))
    with open(os.path.join(a.project, "Core", "Src", "assets.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Généré par tools/mkassets.py : ne pas modifier à la main.\n"
                " * Polices : Go fonts, licence BSD (golang.org/x/image/font/gofont). */\n"
                '#include "assets.h"\n\n' + "\n\n".join(parts) + "\n")
    if a.preview:
        preview.save(a.preview)


if __name__ == "__main__":
    main()
