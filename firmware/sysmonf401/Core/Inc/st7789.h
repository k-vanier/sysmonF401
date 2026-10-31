/* Pilote minimal ST7789 1.9" (170x320) sur SPI1, en paysage 320x170. */
#ifndef ST7789_DRV_H
#define ST7789_DRV_H

#include <stdint.h>

#define ST7789_W 320
#define ST7789_H 170

/* Couleur RGB565 à partir de composantes 8 bits. */
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Caractères spéciaux de la police, en plus de l'ASCII 0x20-0x7E. */
#define CH_UP     "\x01"
#define CH_DOWN   "\x02"
#define CH_DEGREE "\x03"

void st7789_init(int flip); /* flip = 1 : paysage retourné */
void st7789_backlight(int on);
void st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
/* Envoie w*h pixels (couleurs RGB565 natives) dans le rectangle. */
void st7789_blit(int x, int y, int w, int h, const uint16_t *px);
/* Texte en police 5x7, agrandie `scale` fois (1 ou 2), cellule de 6x8 * scale, fond plein. */
void st7789_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg);

#endif
