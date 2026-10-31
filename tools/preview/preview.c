/* Prévisualisation de l'affichage sur PC : compile le vrai sysmon.c / gfx.c /
 * assets.c du firmware avec un faux pilote ST7789 qui dessine dans une image en
 * mémoire, rejoue un scénario et écrit des captures PPM (320x170).
 *
 *   preview <dossier de sortie>
 *
 * Voir build.sh. */

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "st7789.h"
#include "sysmon.h"

static uint16_t fb[ST7789_H][ST7789_W];
static uint32_t tick;

uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t ms) { tick += ms; }
void HAL_GPIO_WritePin(int port, int pin, GPIO_PinState state) { (void)port, (void)pin, (void)state; }
void HAL_GPIO_TogglePin(int port, int pin) { (void)port, (void)pin; }

/* --- Faux pilote : même interface que Core/Src/st7789.c --- */

static int wx, wy, ww, wh, wpos;

void st7789_init(int flip) { (void)flip; memset(fb, 0, sizeof fb); }
void st7789_backlight(int on) { (void)on; }

void st7789_begin(int x, int y, int w, int h) { wx = x, wy = y, ww = w, wh = h, wpos = 0; }
void st7789_end(void) {}

void st7789_push(const uint16_t *px, uint32_t n) {
  while (n--) {
    int x = wx + wpos % ww, y = wy + wpos / ww;
    if (x >= 0 && x < ST7789_W && y >= 0 && y < ST7789_H && wpos < ww * wh) fb[y][x] = *px;
    px++;
    wpos++;
  }
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  st7789_begin(x, y, w, h);
  for (int i = 0; i < w * h; i++) st7789_push(&color, 1);
}

void st7789_blit(int x, int y, int w, int h, const uint16_t *px) {
  st7789_begin(x, y, w, h);
  st7789_push(px, (uint32_t)w * h);
}

/* --- Scénario --- */

static void shot(const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", ST7789_W, ST7789_H);
  for (int y = 0; y < ST7789_H; y++)
    for (int x = 0; x < ST7789_W; x++) {
      uint16_t c = fb[y][x];
      unsigned char rgb[3] = {(c >> 11) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    sysmon_loop();
    tick += 5;
  }
}

static void send(const char *line) {
  sysmon_rx((const uint8_t *)line, strlen(line));
  run(1000);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  sysmon_init();
  run(300);
  shot(dir, "1_attente");
  char line[160];
  for (int s = 0; s < 70; s++) {
    double k = s / 70.0;
    snprintf(line, sizeof line, "cpu=%.1f ram=%.1f temp=%.1f down=%.0f up=%.0f host=prodesk\n",
             20 + 60 * k * k, 41.5, 47 + 20 * k, 2e5 + 9e6 * (s % 17) / 17.0 * k, 3e4 + 4e5 * (s % 5) / 5.0);
    send(line);
  }
  shot(dir, "2_mesures");
  send("cpu=93 ram=88.4 temp=82 down=123456789 up=950 host=un-nom-de-serveur-tres-long\n");
  shot(dir, "3_alertes");
  send("cpu=4 host=srv1\n");
  shot(dir, "4_partiel");
  run(6000);
  shot(dir, "5_hors_ligne");
  return 0;
}
