/* Afficheur CPU / RAM / température / réseau pour serveur FreeBSD.
 *
 * Le PC envoie une ligne de texte par seconde sur le port série USB, ex. :
 *   cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 host=srv1
 * down / up en octets par seconde. Une clé absente = mesure indisponible (« -- »).
 * bri=0 éteint le rétroéclairage, toute autre valeur l'allume (peut être seul sur sa ligne).
 *
 * Écran 320x170 : à gauche une carte blanche avec l'animation (assets.c, généré
 * par tools/mkassets.py) et le nom du serveur ; à droite les jauges et le réseau. */

#include "sysmon.h"
#include "gfx.h"
#include "st7789.h"
#include "main.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLIP 0             /* 1 = écran monté à l'envers */
#define TIMEOUT_MS 5000    /* au-delà sans données : pastille rouge */
#define WAITING_MS 60000   /* au-delà : retour à l'écran d'attente */

#define W ST7789_W
#define H ST7789_H

/* Thème sombre. */
#define C_BG        RGB565(10, 12, 18)
#define C_TRACK     RGB565(34, 39, 52)
#define C_TEXT      RGB565(236, 238, 245)
#define C_DIM       RGB565(130, 138, 156)
#define C_CPU       RGB565(64, 170, 255)
#define C_RAM       RGB565(180, 110, 255)
#define C_TEMP      RGB565(255, 160, 40)
#define C_RED       RGB565(255, 70, 70)
#define C_GREEN     RGB565(60, 220, 120)
#define C_DOWN      RGB565(60, 220, 120)
#define C_UP        RGB565(190, 120, 255)
#define C_CARD      RGB565(255, 255, 255) /* fond du GIF */
#define C_CARD_TEXT RGB565(40, 40, 40)

/* Carte de gauche : animation puis nom du serveur. */
#define CARD_X 4
#define CARD_Y 4
#define CARD_W 130
#define CARD_H 162
#define HOST_Y (CARD_Y + CARD_H - 30)

/* Colonne de droite. */
#define RX 144
#define RW (W - 4 - RX)
#define ROW_H 36
#define ROW_Y0 4
#define BAR_H 6

/* Réseau : deux lignes de texte, graphe à droite. */
#define NET_Y 116
#define GW 70
#define GH 46
#define GX (W - 4 - GW)
#define GY (H - 4 - GH)

typedef struct {
  float cpu, ram, temp, down, up;
  char host[24];
} Stats;

static Stats st;
static uint32_t last_rx, next_frame;
static int online, layout_drawn, frame;

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

/* --- Carte et animation --- */

static void draw_card(void) {
  gfx_round_rect(CARD_X, CARD_Y, CARD_W, CARD_H, 12, C_CARD, C_BG);
}

static void draw_anim(void) {
  gfx_anim_frame(&anim, frame, CARD_X + (CARD_W - anim.width) / 2, CARD_Y + 6);
}

/* Pastille d'état puis nom du serveur, centrés sous l'animation. */
static void draw_host(void) {
  const char *name = st.host[0] ? st.host : "FreeBSD";
  char shown[24];
  strncpy(shown, name, sizeof shown - 1);
  shown[sizeof shown - 1] = 0;
  int maxw = CARD_W - 32;
  if (gfx_text_width(&font_medium, shown) > maxw) { /* trop long : coupé, suivi de « .. » */
    size_t n = strlen(shown);
    do {
      shown[--n] = 0;
      strcpy(shown + (n > 2 ? n - 2 : 0), "..");
    } while (n > 2 && gfx_text_width(&font_medium, shown) > maxw);
  }
  int tw = gfx_text_width(&font_medium, shown), dot = 10, gap = 6;
  int x = CARD_X + (CARD_W - (dot + gap + tw)) / 2;
  st7789_fill_rect(CARD_X + 8, HOST_Y, CARD_W - 16, font_medium.height, C_CARD);
  gfx_round_rect(x, HOST_Y + (font_medium.height - dot) / 2, dot, dot, dot / 2,
                 online ? C_GREEN : C_RED, C_CARD);
  gfx_text(x + dot + gap, HOST_Y, &font_medium, shown, C_CARD_TEXT, C_CARD);
}

/* --- Colonne de droite --- */

static void draw_right_bg(void) {
  st7789_fill_rect(CARD_X + CARD_W, 0, W - CARD_X - CARD_W, H, C_BG);
}

static void draw_layout(void) {
  static const char *labels[] = {"CPU", "RAM", "TEMP"};
  draw_right_bg();
  for (int i = 0; i < 3; i++) {
    int y = ROW_Y0 + i * ROW_H;
    gfx_text(RX, y + font_large.ascent - font_small.ascent, &font_small, labels[i], C_DIM, C_BG);
  }
  gfx_text(RX, NET_Y, &font_medium, CH_DOWN, C_DOWN, C_BG);
  gfx_text(RX, NET_Y + 24, &font_medium, CH_UP, C_UP, C_BG);
  layout_drawn = 1;
}

/* Valeur en grands chiffres alignée à droite + barre. pct dans 0..100, NAN = indisponible. */
static void draw_gauge(int row, float pct, uint16_t color, const char *text) {
  int y = ROW_Y0 + row * ROW_H;
  gfx_text_right(W - 4, y, &font_large, text, isnan(pct) ? C_DIM : C_TEXT, C_BG, RW - 50);
  int fill = 0;
  if (!isnan(pct)) {
    float p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    fill = (int)(p * RW / 100 + 0.5f);
  }
  gfx_bar(RX, y + font_large.height + 2, RW, BAR_H, fill, color, C_TRACK, C_BG);
}

static uint16_t alert(uint16_t c, float v, float limit) { return v >= limit ? C_RED : c; }

/* Entier arrondi, ou « -- », suivi de `unit`. */
static void fmt_int(char *out, size_t n, float v, const char *unit) {
  if (isnan(v)) snprintf(out, n, "--");
  else snprintf(out, n, "%d%s", (int)(v + 0.5f), unit);
}

/* Débit en octets/s : « 820 Ko/s », « 12.3 Mo/s ». Sans %f (newlib-nano). */
static void fmt_rate(char *out, size_t n, float b) {
  static const char *units[] = {"o/s", "Ko/s", "Mo/s", "Go/s"};
  if (isnan(b)) {
    snprintf(out, n, "--");
    return;
  }
  int u = 0;
  while (b >= 1000 && u < 3) {
    b /= 1024;
    u++;
  }
  if (b < 10 && u > 0) {
    unsigned t = (unsigned)(b * 10 + 0.5f); /* dixièmes */
    snprintf(out, n, "%u.%u %s", t / 10, t % 10, units[u]);
  } else {
    snprintf(out, n, "%u %s", (unsigned)(b + 0.5f), units[u]);
  }
}

/* Historique réseau : un point par ligne reçue, les plus récents à droite. */
static float hist_down[GW], hist_up[GW];
static int hist_len;

static void draw_graph(void) {
  static uint16_t col[GH];
  float top = 1024; /* échelle mini 1 Ko/s */
  for (int i = 0; i < hist_len; i++) {
    if (hist_down[i] > top) top = hist_down[i];
    if (hist_up[i] > top) top = hist_up[i];
  }
  /* Colonne par colonne : fond, aire de réception en dégradé, ligne d'envoi. */
  int off = GW - hist_len, prev_uy = -1;
  for (int x = 0; x < GW; x++) {
    int i = x - off;
    for (int y = 0; y < GH; y++) col[y] = (y == GH - 1) ? C_TRACK : C_BG;
    if (i >= 0) {
      int dh = (int)(hist_down[i] / top * (GH - 2) + 0.5f);
      for (int y = GH - dh; y < GH; y++) /* plus clair en haut de l'aire, sombre en bas */
        col[y] = gfx_blend(C_DOWN, C_BG, 150 - 110 * (y - (GH - dh)) / GH);
      if (dh > 0) col[GH - dh] = C_DOWN;
      int uy = GH - 2 - (int)(hist_up[i] / top * (GH - 2) + 0.5f);
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
  draw_gauge(0, st.cpu, alert(C_CPU, st.cpu, 80), buf);
  fmt_int(buf, sizeof buf, st.ram, "%");
  draw_gauge(1, st.ram, alert(C_RAM, st.ram, 80), buf);
  /* Température : barre de 0 à 100 °C, rouge à partir de 75 °C. */
  fmt_int(buf, sizeof buf, st.temp, CH_DEGREE "C");
  draw_gauge(2, st.temp, alert(C_TEMP, st.temp, 75), buf);

  int tx = RX + 18, tw = GX - 2 - tx; /* jusqu'au graphe : efface tout ancien texte */
  fmt_rate(buf, sizeof buf, st.down);
  st7789_fill_rect(gfx_text(tx, NET_Y, &font_medium, buf, C_TEXT, C_BG), NET_Y,
                   tw - gfx_text_width(&font_medium, buf), font_medium.height, C_BG);
  fmt_rate(buf, sizeof buf, st.up);
  st7789_fill_rect(gfx_text(tx, NET_Y + 24, &font_medium, buf, C_TEXT, C_BG), NET_Y + 24,
                   tw - gfx_text_width(&font_medium, buf), font_medium.height, C_BG);

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
  draw_right_bg();
  static const char *lines[] = {"En attente", "du serveur"};
  for (int i = 0; i < 2; i++) {
    int tw = gfx_text_width(&font_medium, lines[i]);
    gfx_text(RX + (RW - tw) / 2, H / 2 - font_medium.height + i * (font_medium.height + 2),
             &font_medium, lines[i], C_DIM, C_BG);
  }
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
  if (host_changed) draw_host();
  return 1;
}

void sysmon_init(void) {
  st7789_init(FLIP);
  st7789_backlight(1);
  /* Mire au démarrage : rouge, vert, bleu. Si elle n'apparaît pas, voir câblage / alimentation. */
  static const uint16_t test[] = {RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255)};
  for (int i = 0; i < 3; i++) {
    st7789_fill_rect(0, 0, W, H, test[i]);
    HAL_Delay(250);
  }
  st7789_fill_rect(0, 0, W, H, C_BG);
  draw_card();
  draw_anim();
  draw_host();
  draw_waiting();
  next_frame = HAL_GetTick() + anim.duration_ms;
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
      draw_host();
    }
    draw_stats();
  }

  uint32_t now = HAL_GetTick();
  if ((int32_t)(now - next_frame) >= 0) {
    frame = (frame + 1) % anim.frames;
    draw_anim();
    next_frame += anim.duration_ms;
    if ((int32_t)(now - next_frame) > 0) next_frame = now + anim.duration_ms; /* retard : on recale */
  }

  uint32_t idle = now - last_rx;
  if (online && idle > TIMEOUT_MS) {
    online = 0;
    draw_host();
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET); /* éteinte */
  }
  if (layout_drawn && !online && idle > WAITING_MS) {
    hist_len = 0;
    draw_waiting();
  }
}
