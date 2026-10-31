/* Afficheur CPU / RAM / température / réseau pour serveur FreeBSD.
 *
 * Le PC envoie une ligne de texte par seconde sur le port série USB, ex. :
 *   cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 host=srv1
 * down / up en octets par seconde. Une clé absente = mesure indisponible (« -- »).
 * bri=0 éteint le rétroéclairage, toute autre valeur l'allume (peut être seul sur sa ligne). */

#include "sysmon.h"
#include "st7789.h"
#include "main.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLIP 0             /* 1 = écran monté à l'envers */
#define TIMEOUT_MS 5000    /* au-delà sans données : point rouge */
#define WAITING_MS 60000   /* au-delà : retour à l'écran d'attente */

#define W ST7789_W
#define H ST7789_H

/* Thème sombre. */
#define C_BG        RGB565(10, 12, 18)
#define C_PANEL     RGB565(40, 46, 60)
#define C_TEXT      RGB565(230, 232, 240)
#define C_DIM       RGB565(120, 128, 145)
#define C_CPU       RGB565(64, 170, 255)
#define C_RAM       RGB565(180, 110, 255)
#define C_TEMP      RGB565(255, 160, 40)
#define C_RED       RGB565(255, 60, 60)
#define C_DOWN      RGB565(60, 220, 120)
#define C_DOWN_FILL RGB565(20, 90, 50)
#define C_UP        RGB565(190, 120, 255)

typedef struct {
  float cpu, ram, temp, down, up;
  char host[24];
} Stats;

static Stats st;
static uint32_t last_rx;
static int online, layout_drawn;

/* --- Réception : tampon circulaire rempli sous interruption USB --- */

static volatile uint8_t rx_ring[1024];
static volatile uint16_t rx_head, rx_tail;

void sysmon_rx(const uint8_t *buf, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    uint16_t next = (rx_head + 1) % sizeof rx_ring;
    if (next == rx_tail) return; /* plein : on perd la fin */
    rx_ring[rx_head] = buf[i];
    rx_head = next;
  }
}

static int rx_get(char *c) {
  if (rx_tail == rx_head) return 0;
  *c = rx_ring[rx_tail];
  rx_tail = (rx_tail + 1) % sizeof rx_ring;
  return 1;
}

/* --- Mise en page --- */

#define BAR_X 50
#define BAR_W 190
#define BAR_H 18
#define ROW_CPU 30
#define ROW_RAM 54
#define ROW_TEMP 78

/* Graphe réseau : un point par ligne reçue, les plus récents à droite. */
#define GX 158
#define GY 104
#define GW 158
#define GH 62
static float hist_down[GW], hist_up[GW];
static int hist_len;

/* Texte taille 2 aligné à droite sur `right`, sur fond plein : écrase l'ancienne valeur. */
static void text_right(int right, int y, const char *s, uint16_t fg, int width) {
  int tw = strlen(s) * 12;
  st7789_fill_rect(right - width, y, width - tw, 16, C_BG);
  st7789_text(right - tw, y, s, 2, fg, C_BG);
}

static void draw_header(void) {
  st7789_fill_rect(0, 0, W, 22, C_PANEL);
  st7789_text(6, 3, st.host[0] ? st.host : "FreeBSD", 2, C_TEXT, C_PANEL);
}

/* Pastille d'état en haut à droite : carré arrondi vert ou rouge. */
static void draw_status(void) {
  uint16_t c = online ? C_DOWN : C_RED;
  st7789_fill_rect(W - 16, 7, 8, 8, c);
  st7789_fill_rect(W - 17, 8, 10, 6, c);
  st7789_fill_rect(W - 15, 6, 6, 10, c);
}

static void draw_frame(int x, int y, int w, int h, uint16_t c) {
  st7789_fill_rect(x, y, w, 1, c);
  st7789_fill_rect(x, y + h - 1, w, 1, c);
  st7789_fill_rect(x, y, 1, h, c);
  st7789_fill_rect(x + w - 1, y, 1, h, c);
}

static void draw_layout(void) {
  static const char *labels[] = {"CPU", "RAM", "TMP"};
  static const int rows[] = {ROW_CPU, ROW_RAM, ROW_TEMP};
  st7789_fill_rect(0, 0, W, H, C_BG);
  draw_header();
  draw_status();
  for (int i = 0; i < 3; i++) {
    st7789_text(6, rows[i] + 1, labels[i], 2, C_DIM, C_BG);
    draw_frame(BAR_X, rows[i], BAR_W, BAR_H, C_PANEL);
  }
  st7789_fill_rect(0, 100, W, 1, C_PANEL);
  draw_frame(GX - 1, GY - 1, GW + 2, GH + 2, C_PANEL);
  st7789_text(6, 110, CH_DOWN, 2, C_DOWN, C_BG);
  st7789_text(6, 140, CH_UP, 2, C_UP, C_BG);
  layout_drawn = 1;
}

/* Barre + valeur. pct dans 0..100, NAN = indisponible. */
static void draw_gauge(int y, float pct, uint16_t color, const char *text) {
  int inner = BAR_W - 4;
  int fill = 0;
  if (!isnan(pct)) {
    float p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    fill = (int)(p * inner / 100 + 0.5f);
  }
  st7789_fill_rect(BAR_X + 2, y + 2, fill, BAR_H - 4, color);
  st7789_fill_rect(BAR_X + 2 + fill, y + 2, inner - fill, BAR_H - 4, C_BG);
  text_right(W - 4, y + 1, text, isnan(pct) ? C_DIM : C_TEXT, 72);
}

static uint16_t alert(uint16_t c, float v, float limit) { return v >= limit ? C_RED : c; }

/* Entier arrondi, ou « -- », suivi de `unit`. */
static void fmt_int(char *out, size_t n, float v, const char *unit) {
  if (isnan(v)) snprintf(out, n, "--");
  else snprintf(out, n, "%d%s", (int)(v + 0.5f), unit);
}

/* Débit en octets/s, au plus 10 caractères : « 123.4 Ko/s ». Sans %f (newlib-nano). */
static void fmt_rate(char *out, size_t n, float b) {
  static const char *units[] = {"Ko/s", "Mo/s", "Go/s"};
  if (isnan(b)) {
    snprintf(out, n, "   -- o/s");
    return;
  }
  if (b < 1024) {
    snprintf(out, n, "%5u  o/s", (unsigned)(b + 0.5f));
    return;
  }
  int u = 0;
  b /= 1024;
  while (b >= 1024 && u < 2) {
    b /= 1024;
    u++;
  }
  unsigned t = (unsigned)(b * 10 + 0.5f); /* dixièmes */
  snprintf(out, n, "%3u.%u %s", t / 10, t % 10, units[u]);
}

static void draw_graph(void) {
  static uint16_t col[GH];
  float top = 1024; /* échelle mini 1 Ko/s */
  for (int i = 0; i < hist_len; i++) {
    if (hist_down[i] > top) top = hist_down[i];
    if (hist_up[i] > top) top = hist_up[i];
  }
  /* Colonne par colonne, sans effacement préalable : pas de clignotement.
   * Réception = aire verte, envoi = ligne violette reliant les points. */
  int off = GW - hist_len, prev_uy = -1;
  for (int x = 0; x < GW; x++) {
    int i = x - off;
    for (int y = 0; y < GH; y++) col[y] = C_BG;
    if (i >= 0) {
      int dh = (int)(hist_down[i] / top * (GH - 1) + 0.5f);
      for (int y = GH - dh; y < GH; y++) col[y] = C_DOWN_FILL;
      col[GH - (dh > 0 ? dh : 1)] = C_DOWN;
      int uy = GH - 1 - (int)(hist_up[i] / top * (GH - 1) + 0.5f);
      int a = prev_uy < 0 ? uy : prev_uy, b = uy;
      if (a > b) { int t = a; a = b; b = t; }
      for (int y = a; y <= b; y++) col[y] = C_UP;
      prev_uy = uy;
    }
    st7789_blit(GX + x, GY, 1, GH, col);
  }
}

static void draw_stats(void) {
  char buf[16];
  if (!layout_drawn) draw_layout();

  fmt_int(buf, sizeof buf, st.cpu, "%");
  draw_gauge(ROW_CPU, st.cpu, alert(C_CPU, st.cpu, 80), buf);
  fmt_int(buf, sizeof buf, st.ram, "%");
  draw_gauge(ROW_RAM, st.ram, alert(C_RAM, st.ram, 80), buf);
  /* Température : barre de 0 à 100 °C, rouge à partir de 75 °C. */
  fmt_int(buf, sizeof buf, st.temp, CH_DEGREE "C");
  draw_gauge(ROW_TEMP, st.temp, alert(C_TEMP, st.temp, 75), buf);

  fmt_rate(buf, sizeof buf, st.down);
  text_right(GX - 6, 110, buf, C_TEXT, 120);
  fmt_rate(buf, sizeof buf, st.up);
  text_right(GX - 6, 140, buf, C_TEXT, 120);

  if (hist_len == GW) {
    memmove(hist_down, hist_down + 1, (GW - 1) * sizeof(float));
    memmove(hist_up, hist_up + 1, (GW - 1) * sizeof(float));
    hist_len--;
  }
  hist_down[hist_len] = isnan(st.down) ? 0 : st.down;
  hist_up[hist_len] = isnan(st.up) ? 0 : st.up;
  hist_len++;
  draw_graph();
}

static void draw_waiting(void) {
  static const char msg[] = "En attente du serveur";
  st7789_fill_rect(0, 0, W, H, C_BG);
  st7789_text((W - (int)(sizeof msg - 1) * 12) / 2, H / 2 - 8, msg, 2, C_DIM, C_BG);
  layout_drawn = 0;
}

/* Analyse une ligne « clé=valeur ... ». Renvoie 1 si elle contient des mesures. */
static int parse_line(char *line) {
  Stats s = {NAN, NAN, NAN, NAN, NAN, ""};
  int measures = 0;
  for (char *tok = strtok(line, " \t\r"); tok; tok = strtok(NULL, " \t\r")) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    if (!strcmp(k, "bri")) {
      st7789_backlight(atoi(v) > 0);
      continue;
    }
    measures = 1;
    if (!strcmp(k, "cpu")) s.cpu = strtof(v, NULL);
    else if (!strcmp(k, "ram")) s.ram = strtof(v, NULL);
    else if (!strcmp(k, "temp")) s.temp = strtof(v, NULL);
    else if (!strcmp(k, "down")) s.down = strtof(v, NULL);
    else if (!strcmp(k, "up")) s.up = strtof(v, NULL);
    else if (!strcmp(k, "host")) {
      strncpy(s.host, v, sizeof s.host - 1);
      s.host[sizeof s.host - 1] = 0;
    }
  }
  if (!measures) return 0;
  int host_changed = strcmp(s.host, st.host) != 0;
  st = s;
  if (layout_drawn && host_changed) {
    draw_header();
    draw_status();
  }
  return 1;
}

void sysmon_init(void) {
  st7789_init(FLIP);
  st7789_backlight(1);
  draw_waiting();
}

void sysmon_loop(void) {
  static char line[160];
  static size_t len;
  char c;

  while (rx_get(&c)) {
    if (c != '\n') {
      if (len < sizeof line - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    len = 0;
    if (!parse_line(line)) continue;
    last_rx = HAL_GetTick();
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); /* LED bleue : clignote à chaque ligne */
    if (!online) {
      online = 1;
      if (layout_drawn) draw_status();
    }
    draw_stats();
  }

  uint32_t idle = HAL_GetTick() - last_rx;
  if (online && idle > TIMEOUT_MS) {
    online = 0;
    draw_status();
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET); /* éteinte */
  }
  if (layout_drawn && !online && idle > WAITING_MS) {
    hist_len = 0;
    draw_waiting();
  }
}
