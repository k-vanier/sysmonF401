/* Généré par tools/mkassets.py : ne pas modifier à la main. */
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

#define ANIM_PIXELS 15120 /* width * height, pour le tampon de l'image affichée */

/* Caractères spéciaux des polices small et medium (et ° dans large). */
#define CH_UP     "\x01"
#define CH_DOWN   "\x02"
#define CH_DEGREE "\x7f"

extern const Font font_small, font_medium, font_large;
extern const Anim anim;

#endif
