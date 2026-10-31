/* Dessin lissé par-dessus le pilote ST7789 : texte, barres, rectangles arrondis,
 * animation. Chaque forme est calculée en mémoire contre une couleur de fond
 * connue puis envoyée d'un bloc : pas d'effacement préalable, pas de clignotement. */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "assets.h"

/* Mélange fg sur bg, a de 0 (bg) à 255 (fg). */
uint16_t gfx_blend(uint16_t fg, uint16_t bg, unsigned a);

int gfx_text_width(const Font *f, const char *s);
/* Texte dont le haut de ligne est en y ; renvoie le x après le dernier caractère. */
int gfx_text(int x, int y, const Font *f, const char *s, uint16_t fg, uint16_t bg);
/* Texte aligné à droite sur `right` ; efface en bg le reste de la zone de largeur `width`. */
void gfx_text_right(int right, int y, const Font *f, const char *s, uint16_t fg, uint16_t bg,
                    int width);

/* Rectangle aux coins arrondis de rayon r (un disque si r = w/2 = h/2). */
void gfx_round_rect(int x, int y, int w, int h, int r, uint16_t color, uint16_t bg);
/* Barre en gélule : les `fill` premiers pixels en color, le reste en track. */
void gfx_bar(int x, int y, int w, int h, int fill, uint16_t color, uint16_t track, uint16_t bg);

/* Image n de l'animation, coin haut gauche en (x, y). */
void gfx_anim_frame(const Anim *a, int n, int x, int y);

#endif
