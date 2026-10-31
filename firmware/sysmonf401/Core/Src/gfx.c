/* Dessin lissé par-dessus le pilote ST7789 (voir gfx.h). */

#include "gfx.h"
#include "st7789.h"

#include <math.h>

static uint16_t buf[2048]; /* tampon partagé : un glyphe, une barre ou une ligne */

uint16_t gfx_blend(uint16_t fg, uint16_t bg, unsigned a) {
  if (a >= 255) return fg;
  if (a == 0) return bg;
  unsigned r = ((fg >> 11) * a + (bg >> 11) * (255 - a)) / 255;
  unsigned g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a)) / 255;
  unsigned b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a)) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static int glyph(const Font *f, char c) {
  int g = f->map[(uint8_t)c & 0x7F];
  return g == 0xFF ? f->map[' '] : g;
}

int gfx_text_width(const Font *f, const char *s) {
  int w = 0;
  for (; *s; s++) {
    int g = glyph(f, *s);
    if (g != 0xFF) w += f->width[g];
  }
  return w;
}

int gfx_text(int x, int y, const Font *f, const char *s, uint16_t fg, uint16_t bg) {
  uint16_t ramp[16]; /* les 16 niveaux d'alpha, calculés une fois */
  for (int a = 0; a < 16; a++) ramp[a] = gfx_blend(fg, bg, a * 17);
  for (; *s; s++) {
    int g = glyph(f, *s);
    if (g == 0xFF) continue;
    int w = f->width[g], h = f->height, rb = (w + 1) / 2;
    if (w * h > (int)(sizeof buf / sizeof buf[0])) continue;
    const uint8_t *d = f->data + f->offset[g];
    for (int r = 0; r < h; r++)
      for (int c = 0; c < w; c++) {
        uint8_t v = d[r * rb + c / 2];
        buf[r * w + c] = ramp[(c & 1) ? v & 15 : v >> 4];
      }
    st7789_blit(x, y, w, h, buf);
    x += w;
  }
  return x;
}

void gfx_text_right(int right, int y, const Font *f, const char *s, uint16_t fg, uint16_t bg,
                    int width) {
  int tw = gfx_text_width(f, s);
  if (tw < width) st7789_fill_rect(right - width, y, width - tw, f->height, bg);
  gfx_text(right - tw, y, f, s, fg, bg);
}

/* Couverture (0-255) du pixel (px, py) par une gélule horizontale de hauteur h,
 * de x = 0 à w, coins de rayon r. */
static unsigned coverage(int px, int py, int w, int h, float r) {
  float cx = px + 0.5f, cy = py + 0.5f;
  float qx = cx < r ? r : cx > w - r ? w - r : cx;
  float qy = cy < r ? r : cy > h - r ? h - r : cy;
  float dx = cx - qx, dy = cy - qy;
  float d = sqrtf(dx * dx + dy * dy) - r; /* < 0 : dedans */
  float cov = 0.5f - d;
  return cov <= 0 ? 0 : cov >= 1 ? 255 : (unsigned)(cov * 255);
}

void gfx_round_rect(int x, int y, int w, int h, int r, uint16_t color, uint16_t bg) {
  if (w > (int)(sizeof buf / sizeof buf[0])) return;
  for (int py = 0; py < h; py++) {
    /* Hors des coins la ligne est pleine : envoi direct. */
    if (py >= r && py < h - r) {
      st7789_fill_rect(x, y + py, w, 1, color);
      continue;
    }
    for (int px = 0; px < w; px++) buf[px] = gfx_blend(color, bg, coverage(px, py, w, h, r));
    st7789_blit(x, y + py, w, 1, buf);
  }
}

void gfx_bar(int x, int y, int w, int h, int fill, uint16_t color, uint16_t track, uint16_t bg) {
  if (w * h > (int)(sizeof buf / sizeof buf[0])) return;
  float r = h / 2.0f;
  for (int py = 0; py < h; py++)
    for (int px = 0; px < w; px++)
      buf[py * w + px] = gfx_blend(px < fill ? color : track, bg, coverage(px, py, w, h, r));
  st7789_blit(x, y, w, h, buf);
}

/* Les images sont stockées colonne par colonne et envoyées de même, une fenêtre
 * d'une colonne à la fois : en paysage la dalle se rafraîchit selon cet axe, la
 * limite entre ancienne et nouvelle image reste donc une seule verticale au lieu
 * d'une diagonale sur toute l'animation (scintillement). */
void gfx_anim_frame(const Anim *a, int n, int x, int y) {
  const uint8_t *p = a->rle + a->offset[n];
  int h = a->height, k = 0, col = 0, run = 0, literal = 0;
  uint16_t c = 0;
  while (col < a->width) {
    if (!run) {
      uint8_t t = *p++;
      run = (t & 0x7F) + 1;
      literal = t >= 0x80;
      if (!literal) c = a->palette[*p++];
    }
    buf[k++] = literal ? a->palette[*p++] : c;
    run--;
    if (k == h) { /* colonne complète */
      st7789_blit(x + col, y, 1, h, buf);
      col++;
      k = 0;
    }
  }
}
