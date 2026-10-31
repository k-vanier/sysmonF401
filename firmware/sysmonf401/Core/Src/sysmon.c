/* Afficheur CPU / RAM / température / réseau pour serveur FreeBSD.
 *
 * Le PC envoie une ligne de texte par seconde sur le port série USB, ex. :
 *   cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 os=15.0-RELEASE-p9
 * down / up en octets par seconde. Une clé absente = mesure indisponible (« -- »).
 * os = texte sous l'animation, _ pour les espaces (host= est accepté aussi).
 * bri=0 éteint le rétroéclairage, toute autre valeur l'allume (peut être seul sur sa ligne).
 *
 * Toutes les 5 s, une autre ligne peut porter le résumé des torrents (clés t...,
 * voir freebsd/torrents.go) : la colonne de droite alterne alors toutes les 10 s
 * entre la page système et la page torrents.
 *
 * Écran 320x170 sur fond blanc : à gauche l'animation (assets.c, généré par
 * tools/mkassets.py) et la version du système ; à droite la page affichée. */

#include "sysmon.h"
#include "gfx.h"
#include "st7789.h"
#include "main.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLIP 0             /* 1 = écran monté à l'envers */
#define TIMEOUT_MS 5000    /* au-delà sans données : hors ligne (LED éteinte) */
#define WAITING_MS 60000   /* au-delà : retour à l'écran d'attente */
#define PAGE_MS 10000      /* durée de chaque page */
#define TORRENT_MS 30000   /* résumé torrents plus vieux : page torrents retirée */

#define W ST7789_W
#define H ST7789_H

/* Thème clair : fond blanc, comme celui du GIF. */
#define C_BG        RGB565(255, 255, 255)
#define C_TRACK     RGB565(226, 229, 236)
#define C_TEXT      RGB565(30, 32, 40)
#define C_DIM       RGB565(118, 124, 138)
#define C_CPU       RGB565(30, 130, 230)
#define C_RAM       RGB565(140, 80, 220)
#define C_TEMP      RGB565(240, 140, 20)
#define C_RED       RGB565(220, 40, 40)
#define C_OK        RGB565(30, 160, 90)
#define C_DOWN      RGB565(236, 72, 153) /* rose */
#define C_UP        RGB565(150, 90, 230)

/* Colonne de gauche : animation puis version du système. */
#define LEFT_W 138
#define LABEL_W 142 /* le libellé peut déborder jusqu'à la colonne de droite */
#define LABEL_Y (H - 4 - font_medium.height) /* une ligne en bas, animation centrée au-dessus */
#define ANIM_X ((LEFT_W - anim.width) / 2)
#define ANIM_Y ((LABEL_Y - anim.height) / 2)

/* Colonne de droite : trois lignes « libellé, grand nombre, barre », puis une zone
 * basse (réseau ou top / flop des torrents). */
#define RX 144
#define RW (W - 4 - RX)
#define ROW_H 32
#define ROW_Y0 4
#define BAR_H 6
#define BAR_GAP 3 /* entre le bas des chiffres et la barre */
#define ROW_Y(r) (ROW_Y0 + (r) * ROW_H)
#define BAR_Y(r) (ROW_Y(r) + font_large.height + BAR_GAP)
#define LOW_Y (ROW_Y0 + 3 * ROW_H + 2)

/* Réseau : une ligne de débits (réception à gauche, envoi à droite), puis un graphe
 * en miroir sur toute la largeur : réception au-dessus de l'axe, envoi en dessous. */
#define NET_Y LOW_Y
#define GX RX
#define GW RW
#define GY (NET_Y + font_medium.height + 4)
#define GH (H - 3 - GY)
#define GH_MAX 64 /* pour le tampon d'une colonne */

typedef struct {
  float cpu, ram, temp, down, up;
  char label[40];
} Stats;

/* Résumé des torrents (c411_transmission), tel que reçu : tailles et ratios en texte. */
typedef struct {
  int dl, max, att, err, npct;
  int pct[8];
  char free[10], avail[10], need[10], up[10], down[10], ratio[10];
  char topr[10], flopr[10], topn[24], flopn[24], act[48], error[48];
  long age; /* secondes depuis le passage, à la réception */
} Torrents;

static Stats st;
static Torrents tor;
static uint32_t last_rx, tor_rx, next_frame, page_t;
static int online, layout_drawn, frame, page, have_tor, tor_changed;

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

/* --- Outils de texte --- */

static void copy(char *dst, size_t n, const char *src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
}

/* Coupe s pour qu'il tienne dans maxw pixels, en finissant par « .. ». */
static void fit(char *s, const Font *f, int maxw) {
  size_t n = strlen(s);
  while (n > 2 && gfx_text_width(f, s) > maxw) {
    s[--n] = 0;
    strcpy(s + n - 2, "..");
  }
}

/* Texte en petit, aligné sur la ligne de base d'un texte de la police `on` placé en y. */
static int text_small_on(int x, int y, const Font *on, const char *s, uint16_t fg) {
  return gfx_text(x, y + on->ascent - font_small.ascent, &font_small, s, fg, C_BG);
}

/* --- Colonne de gauche --- */

/* Sous l'animation : version du système (« 15.0-RELEASE-p9 »), centrée, en police
 * moyenne si elle tient, sinon en petite. Rien reçu : rien. */
static void draw_label(void) {
  char text[40];
  int maxw = LABEL_W - 4;
  strcpy(text, st.label);
  const Font *f = &font_medium;
  if (gfx_text_width(f, text) > maxw) f = &font_small;
  fit(text, f, maxw);

  st7789_fill_rect(0, LABEL_Y, LABEL_W, font_medium.height, C_BG);
  gfx_text((LABEL_W - gfx_text_width(f, text)) / 2, LABEL_Y + (font_medium.height - f->height) / 2,
           f, text, C_TEXT, C_BG);
}

/* --- Colonne de droite : éléments communs --- */

static void draw_right_bg(void) {
  st7789_fill_rect(LABEL_W, 0, W - LABEL_W, H, C_BG);
}

/* Libellé d'une ligne (en petit, aligné sur les grands chiffres), suivi d'un
 * complément optionnel dans sa couleur, coupé pour s'arrêter avant maxx. */
static void draw_row_label(int row, const char *label, const char *extra, uint16_t extra_fg,
                           int maxx) {
  int x = text_small_on(RX, ROW_Y(row), &font_large, label, C_DIM);
  if (extra && extra[0]) {
    char e[24];
    copy(e, sizeof e, extra);
    x = text_small_on(x, ROW_Y(row), &font_large, " ", C_DIM);
    fit(e, &font_small, maxx - 6 - x);
    text_small_on(x, ROW_Y(row), &font_large, e, extra_fg);
  }
}

/* Grand nombre aligné à droite d'une ligne ; clear : largeur effacée à sa gauche
 * (anciens chiffres plus larges), 0 sur une page fraîchement effacée. Renvoie le x
 * de son début. */
static int draw_row_value(int row, const char *text, uint16_t fg, int clear) {
  int tw = gfx_text_width(&font_large, text);
  if (clear) gfx_text_right(W - 4, ROW_Y(row), &font_large, text, fg, C_BG, clear);
  else gfx_text(W - 4 - tw, ROW_Y(row), &font_large, text, fg, C_BG);
  return W - 4 - tw;
}

/* Barre d'une ligne : fill sur 0..1 (NAN = vide). */
static void draw_row_bar(int row, float fill, uint16_t color, uint16_t track) {
  int px = isnan(fill) ? 0 : (int)((fill < 0 ? 0 : fill > 1 ? 1 : fill) * RW + 0.5f);
  gfx_bar(RX, BAR_Y(row), RW, BAR_H, px, color, track, C_BG);
}

static uint16_t alert(uint16_t c, float v, float limit) { return v >= limit ? C_RED : c; }

/* Entier arrondi, ou « -- », suivi de `unit`. */
static void fmt_int(char *out, size_t n, float v, const char *unit) {
  if (isnan(v)) snprintf(out, n, "--");
  else snprintf(out, n, "%d%s", (int)(v + 0.5f), unit);
}

/* --- Page système --- */

/* Débit en octets/s, en nombre et unité : « 820 » « Ko/s », « 12.3 » « Mo/s ».
 * Sans %f (newlib-nano). */
static void fmt_rate(char *num, size_t n, const char **unit, float b) {
  static const char *units[] = {"o/s", "Ko/s", "Mo/s", "Go/s"};
  *unit = "";
  if (isnan(b)) {
    snprintf(num, n, "--");
    return;
  }
  int u = 0;
  while (b >= 1000 && u < 3) {
    b /= 1024;
    u++;
  }
  *unit = units[u];
  if (b < 10 && u > 0) {
    unsigned t = (unsigned)(b * 10 + 0.5f); /* dixièmes */
    snprintf(num, n, "%u.%u", t / 10, t % 10);
  } else {
    snprintf(num, n, "%u", (unsigned)(b + 0.5f));
  }
}

/* Une colonne de débit : flèche et nombre en couleur, unité en petit sur la même
 * ligne de base ; le reste de la colonne (largeur w) est effacé. */
static void draw_rate(int x, int w, const char *arrow, float b, uint16_t color) {
  char num[12];
  const char *unit;
  fmt_rate(num, sizeof num, &unit, b);
  const Font *m = &font_medium, *sm = &font_small;
  int end = x + w;
  x = gfx_text(x, NET_Y, m, arrow, color, C_BG);
  x = gfx_text(x, NET_Y, m, " ", color, C_BG);
  x = gfx_text(x, NET_Y, m, num, color, C_BG);
  x = gfx_text(x, NET_Y, m, " ", C_DIM, C_BG);
  int uy = NET_Y + m->ascent - sm->ascent, uw = gfx_text_width(sm, unit);
  st7789_fill_rect(x, NET_Y, uw, uy - NET_Y, C_BG);
  x = gfx_text(x, uy, sm, unit, C_DIM, C_BG);
  st7789_fill_rect(x - uw, uy + sm->height, uw, NET_Y + m->height - uy - sm->height, C_BG);
  st7789_fill_rect(x, NET_Y, end - x, m->height, C_BG);
}

/* Historique réseau : un point par ligne reçue, les plus récents à droite. Il se
 * remplit aussi pendant que la page torrents est affichée. */
static float hist_down[GW], hist_up[GW];
static int hist_len;

static void push_history(void) {
  if (hist_len == GW) {
    memmove(hist_down, hist_down + 1, (GW - 1) * sizeof(float));
    memmove(hist_up, hist_up + 1, (GW - 1) * sizeof(float));
    hist_len--;
  }
  hist_down[hist_len] = isnan(st.down) ? 0 : st.down;
  hist_up[hist_len] = isnan(st.up) ? 0 : st.up;
  hist_len++;
}

/* Hauteur en pixels de v sur une demi-hauteur h, pour une échelle top. */
static int bar_px(float v, float top, int h) {
  int n = (int)(v / top * h + 0.5f);
  return n > h ? h : n;
}

static void draw_graph(void) {
  static uint16_t col[GH_MAX];
  int gh = GH > GH_MAX ? GH_MAX : GH, mid = gh / 2, hd = mid, hu = gh - mid - 1;
  /* Une échelle par sens : l'envoi, souvent bien plus faible, reste lisible. */
  float top_d = 1024, top_u = 1024; /* échelle mini 1 Ko/s */
  for (int i = 0; i < hist_len; i++) {
    if (hist_down[i] > top_d) top_d = hist_down[i];
    if (hist_up[i] > top_u) top_u = hist_up[i];
  }
  /* Colonne par colonne : réception au-dessus de l'axe, envoi en dessous, en aires
   * dégradées (soutenues au bord, pâles vers l'axe) avec un liseré au bord. */
  int off = GW - hist_len;
  for (int x = 0; x < GW; x++) {
    int i = x - off;
    for (int y = 0; y < gh; y++) col[y] = C_BG;
    col[mid] = C_TRACK;
    if (i >= 0) {
      int d = bar_px(hist_down[i], top_d, hd), u = bar_px(hist_up[i], top_u, hu);
      for (int k = 1; k <= d; k++) col[mid - k] = gfx_blend(C_DOWN, C_BG, 60 + 110 * k / hd);
      if (d) col[mid - d] = C_DOWN;
      for (int k = 1; k <= u; k++) col[mid + k] = gfx_blend(C_UP, C_BG, 60 + 110 * k / hu);
      if (u) col[mid + u] = C_UP;
    }
    st7789_blit(GX + x, GY, 1, gh, col);
  }
}

static void draw_system_values(void) {
  char buf[16];
  fmt_int(buf, sizeof buf, st.cpu, "%");
  draw_row_value(0, buf, isnan(st.cpu) ? C_DIM : C_TEXT, 70);
  draw_row_bar(0, st.cpu / 100, alert(C_CPU, st.cpu, 80), C_TRACK);
  fmt_int(buf, sizeof buf, st.ram, "%");
  draw_row_value(1, buf, isnan(st.ram) ? C_DIM : C_TEXT, 70);
  draw_row_bar(1, st.ram / 100, alert(C_RAM, st.ram, 80), C_TRACK);
  /* Température : barre de 0 à 100 °C, rouge à partir de 75 °C. */
  fmt_int(buf, sizeof buf, st.temp, CH_DEGREE "C");
  draw_row_value(2, buf, isnan(st.temp) ? C_DIM : C_TEXT, 70);
  draw_row_bar(2, st.temp / 100, alert(C_TEMP, st.temp, 75), C_TRACK);

  draw_rate(RX, RW / 2 - 4, CH_DOWN, st.down, C_DOWN);
  draw_rate(RX + RW / 2 + 4, RW / 2 - 4, CH_UP, st.up, C_UP);
  draw_graph();
}

static void draw_system_page(void) {
  draw_row_label(0, "CPU", NULL, 0, 0);
  draw_row_label(1, "RAM", NULL, 0, 0);
  draw_row_label(2, "TEMP", NULL, 0, 0);
  draw_system_values();
}

/* --- Page torrents --- */

/* « 44.1T », « 87G », « 1.5M » -> gigaoctets ; NAN si illisible. */
static float size_gb(const char *s) {
  char *end;
  float v = strtof(s, &end);
  if (end == s) return NAN;
  switch (*end) {
  case 'T': return v * 1024;
  case 'G': return v;
  case 'M': return v / 1024;
  case 'K': return v / (1024 * 1024);
  default: return v / (1024 * 1024 * 1024);
  }
}

/* Âge compact : « 40 s », « 12 min », « 3 h », « 2 j ». */
static void fmt_age(char *out, size_t n, long s) {
  if (s < 60) snprintf(out, n, "%ld s", s);
  else if (s < 3600) snprintf(out, n, "%ld min", s / 60);
  else if (s < 86400) snprintf(out, n, "%ld h", s / 3600);
  else snprintf(out, n, "%ld j", s / 86400);
}

/* La page torrents est faite de cartes : rectangles arrondis gris très clair. */
#define C_PANEL RGB565(243, 244, 248)
#define C_RESERVED RGB565(246, 196, 222) /* rose pâle : place déjà promise */
#define PAD 6
#define TOP_Y 2      /* cartes DL et ratio */
#define TOP_H 60
#define DL_W 76
#define DISK_Y 66    /* carte disque */
#define DISK_H 38
#define RANK_Y 107   /* top, flop, puis l'action */
#define TITLE_DY 3   /* titre d'une carte, puis grand nombre, puis barre */
#define BIG_DY 20

static void panel(int x, int y, int w, int h, uint16_t color) {
  gfx_round_rect(x, y, w, h, 8, color, C_BG);
}

/* Texte de la police f dont la ligne de base est en `base`. */
static int text_at_base(int x, int base, const Font *f, const char *s, uint16_t fg, uint16_t bg) {
  return gfx_text(x, base - f->ascent, f, s, fg, bg);
}

/* Remplace les _ par des espaces. */
static void spaces(char *s) {
  for (; *s; s++)
    if (*s == '_') *s = ' ';
}

/* Carte DL : titre et ajouts en attente, « 2/3 » en grand, une pastille par place
 * (rose remplie selon l'avancement, violette pour un ajout en attente). */
static void draw_dl_card(int x, int y, int w, int h) {
  char buf[16];
  panel(x, y, w, h, C_PANEL);
  int base = y + TITLE_DY + font_small.ascent;
  int tx = text_at_base(x + PAD, base, &font_small, "DL", C_DIM, C_PANEL);
  if (tor.att) {
    snprintf(buf, sizeof buf, " +%d att", tor.att);
    text_at_base(tx, base, &font_small, buf, C_UP, C_PANEL);
  }
  if (tor.max > 0) snprintf(buf, sizeof buf, "%d/%d", tor.dl, tor.max);
  else snprintf(buf, sizeof buf, "%d", tor.dl);
  gfx_text(x + PAD, y + BIG_DY, &font_large, buf, C_TEXT, C_PANEL);

  int n = tor.max > 0 ? tor.max : tor.dl + tor.att;
  if (n < 1) n = 1;
  if (n > 8) n = 8;
  int gap = 3, inner = w - 2 * PAD, sw = (inner - gap * (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    int fill = 0;
    uint16_t color = C_DOWN;
    if (i < tor.dl) fill = i < tor.npct ? sw * tor.pct[i] / 100 : sw;
    else if (i < tor.dl + tor.att) fill = sw, color = C_UP;
    gfx_bar(x + PAD + i * (sw + gap), y + h - PAD - BAR_H, sw, BAR_H, fill, color, C_TRACK, C_PANEL);
  }
}

/* Carte ratio : en grand (vert à partir de 1, orange en dessous) ; la barre montre
 * l'envoyé (violet) face au reçu (rose) depuis toujours. */
static void draw_ratio_card(int x, int y, int w, int h) {
  panel(x, y, w, h, C_PANEL);
  text_at_base(x + PAD, y + TITLE_DY + font_small.ascent, &font_small, "RATIO", C_DIM, C_PANEL);
  const char *r = tor.ratio[0] ? tor.ratio : "-";
  uint16_t fg = C_DIM;
  if (!strcmp(r, "inf")) fg = C_OK;
  else if (r[0] != '-' && r[0] != '?') fg = strtof(r, NULL) >= 1 ? C_OK : C_TEMP;
  gfx_text(x + PAD, y + BIG_DY, &font_large, r, fg, C_PANEL);
  float up = size_gb(tor.up), down = size_gb(tor.down), sum = up + down;
  int bw = w - 2 * PAD, fill = sum > 0 ? (int)(bw * up / sum + 0.5f) : 0;
  gfx_bar(x + PAD, y + h - PAD - BAR_H, bw, BAR_H, fill, C_UP, sum > 0 ? C_DOWN : C_TRACK, C_PANEL);
}

/* Carte disque : « dispo 29G » à gauche, « 87G libre » à droite ; la barre entière
 * est le libre, la part verte ce qui reste pour des ajouts, le rose pâle ce qui est
 * déjà promis (téléchargements en cours, marge minimale). En cas de manque : rouge. */
static void draw_disk_card(int x, int y, int w, int h) {
  panel(x, y, w, h, C_PANEL);
  int base = y + 4 + font_medium.ascent;
  const char *v = tor.need[0] ? tor.need : tor.avail[0] ? tor.avail : "?";
  const char *fr_txt = tor.free[0] ? tor.free : "?";
  int tx = text_at_base(x + PAD, base, &font_small, tor.need[0] ? "manque " : "dispo ", C_DIM, C_PANEL);
  tx = text_at_base(tx, base, &font_medium, v, tor.need[0] ? C_RED : C_OK, C_PANEL);
  int lw = gfx_text_width(&font_small, " libre"), vw = gfx_text_width(&font_medium, fr_txt);
  if (x + w - PAD - lw - vw < tx + 8) lw = 0; /* pas la place : le mot « libre » saute */
  int rx = text_at_base(x + w - PAD - lw - vw, base, &font_medium, fr_txt, C_TEXT, C_PANEL);
  if (lw) text_at_base(rx, base, &font_small, " libre", C_DIM, C_PANEL);

  int bw = w - 2 * PAD, by = y + h - 5 - 8;
  float fr = size_gb(tor.free), av = size_gb(tor.avail);
  if (tor.need[0]) gfx_bar(x + PAD, by, bw, 8, bw, C_RED, C_TRACK, C_PANEL);
  else if (fr > 0 && !isnan(av)) gfx_bar(x + PAD, by, bw, 8, (int)(bw * av / fr + 0.5f), C_OK, C_RESERVED, C_PANEL);
  else gfx_bar(x + PAD, by, bw, 8, 0, C_OK, C_TRACK, C_PANEL);
}

/* Ligne top / flop : triangle et ratio en couleur, puis le nom. */
static void draw_rank(int y, const char *tri, const char *ratio, const char *name, uint16_t color) {
  char n[24];
  if (!ratio[0]) return;
  int base = y + font_medium.ascent;
  int x = text_at_base(RX + 2, base, &font_medium, tri, color, C_BG);
  x = text_at_base(x, base, &font_medium, " ", color, C_BG);
  x = text_at_base(x, base, &font_medium, ratio, color, C_BG);
  copy(n, sizeof n, name);
  spaces(n);
  fit(n, &font_small, RX + RW - x - 8);
  text_at_base(x + 8, base, &font_small, n, C_TEXT, C_BG);
}

static void draw_torrent_page(void) {
  char buf[48], age[12];
  long since = tor.age + (long)((HAL_GetTick() - tor_rx) / 1000);
  fmt_age(age, sizeof age, since);

  if (tor.error[0]) { /* c411 n'a pas pu joindre Transmission : une carte rouge pâle */
    uint16_t bg = RGB565(253, 236, 236);
    panel(RX, TOP_Y, RW, H - 2 * TOP_Y, bg);
    text_at_base(RX + PAD, TOP_Y + 6 + font_small.ascent, &font_small, "TORRENTS", C_DIM, bg);
    gfx_text(RX + PAD, TOP_Y + 26, &font_medium, "Erreur", C_RED, bg);
    copy(buf, sizeof buf, tor.error);
    spaces(buf);
    fit(buf, &font_small, RW - 2 * PAD);
    gfx_text(RX + PAD, TOP_Y + 50, &font_small, buf, C_TEXT, bg);
    int fx = gfx_text(RX + PAD, TOP_Y + 84, &font_large, tor.free[0] ? tor.free : "?", C_TEXT, bg);
    text_at_base(fx, TOP_Y + 84 + font_large.ascent, &font_small, " libre", C_DIM, bg);
    snprintf(buf, sizeof buf, "il y a %s", age);
    gfx_text(RX + PAD, H - TOP_Y - 8 - font_small.height, &font_small, buf, C_DIM, bg);
    return;
  }

  draw_dl_card(RX, TOP_Y, DL_W, TOP_H);
  draw_ratio_card(RX + DL_W + 6, TOP_Y, RW - DL_W - 6, TOP_H);
  draw_disk_card(RX, DISK_Y, RW, DISK_H);

  /* Torrents terminés le plus et le moins rentables. */
  draw_rank(RANK_Y, CH_TRI_UP, tor.topr, tor.topn, C_OK);
  draw_rank(RANK_Y + font_medium.height, CH_TRI_DOWN, tor.flopr, tor.flopn, C_RED);

  /* Dernière ligne : torrents en erreur et ce que le dernier passage a fait (ou son
   * âge s'il est ancien : c411 ne tourne plus ?) ; à droite, les totaux envoyé / reçu
   * depuis toujours. */
  int y = H - 1 - font_small.height;
  int tw = gfx_text_width(&font_small, CH_UP) + gfx_text_width(&font_small, tor.up) +
           gfx_text_width(&font_small, " ") + gfx_text_width(&font_small, CH_DOWN) +
           gfx_text_width(&font_small, tor.down);
  int x = RX + RW - tw;
  x = gfx_text(x, y, &font_small, CH_UP, C_UP, C_BG);
  x = gfx_text(x, y, &font_small, tor.up, C_TEXT, C_BG);
  x = gfx_text(x, y, &font_small, " ", C_TEXT, C_BG);
  x = gfx_text(x, y, &font_small, CH_DOWN, C_DOWN, C_BG);
  gfx_text(x, y, &font_small, tor.down, C_TEXT, C_BG);

  int lx = RX, right = RX + RW - tw - 5;
  if (tor.err) {
    snprintf(buf, sizeof buf, "%d err ", tor.err);
    lx = gfx_text(lx, y, &font_small, buf, C_RED, C_BG);
  }
  char act[64];
  copy(act, sizeof act, tor.act);
  spaces(act);
  if (since > 3600) snprintf(act, sizeof act, "il y a %s", age);
  fit(act, &font_small, right - lx);
  gfx_text(lx, y, &font_small, act, since > 3600 ? C_TEMP : C_DIM, C_BG);
}

/* --- Pages --- */

static int torrents_valid(void) {
  return have_tor && HAL_GetTick() - tor_rx < TORRENT_MS;
}

/* Redessine la page affichée en entier. */
static void redraw_page(void) {
  draw_right_bg();
  if (page == 1) draw_torrent_page();
  else draw_system_page();
  layout_drawn = 1;
}

/* Passe à la page p et relance son minuteur. */
static void show_page(int p) {
  page = p;
  page_t = HAL_GetTick();
  redraw_page();
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

/* --- Réception des lignes --- */

/* Clé du résumé torrents ; renvoie 1 si k en est une. */
static int parse_torrent_key(Torrents *t, const char *k, const char *v) {
  if (k[0] != 't') return 0;
  k++;
  if (!strcmp(k, "dl")) t->dl = atoi(v);
  else if (!strcmp(k, "max")) t->max = atoi(v);
  else if (!strcmp(k, "att")) t->att = atoi(v);
  else if (!strcmp(k, "err")) t->err = atoi(v);
  else if (!strcmp(k, "age")) t->age = atol(v);
  else if (!strcmp(k, "pct")) { /* « 87,45,20 » */
    for (const char *p = v; *p && t->npct < 8; p++) {
      t->pct[t->npct++] = atoi(p);
      while (*p && *p != ',') p++;
      if (!*p) break;
    }
  }
  else if (!strcmp(k, "free")) copy(t->free, sizeof t->free, v);
  else if (!strcmp(k, "avail")) copy(t->avail, sizeof t->avail, v);
  else if (!strcmp(k, "need")) copy(t->need, sizeof t->need, v);
  else if (!strcmp(k, "up")) copy(t->up, sizeof t->up, v);
  else if (!strcmp(k, "down")) copy(t->down, sizeof t->down, v);
  else if (!strcmp(k, "ratio")) copy(t->ratio, sizeof t->ratio, v);
  else if (!strcmp(k, "topr")) copy(t->topr, sizeof t->topr, v);
  else if (!strcmp(k, "topn")) copy(t->topn, sizeof t->topn, v);
  else if (!strcmp(k, "flopr")) copy(t->flopr, sizeof t->flopr, v);
  else if (!strcmp(k, "flopn")) copy(t->flopn, sizeof t->flopn, v);
  else if (!strcmp(k, "act")) copy(t->act, sizeof t->act, v);
  else if (!strcmp(k, "error")) copy(t->error, sizeof t->error, v);
  else return 0;
  return 1;
}

/* Analyse une ligne « clé=valeur ... ». Renvoie 1 pour des mesures système,
 * 2 pour un résumé torrents, 0 sinon. */
static int parse_line(char *line) {
  Stats s = {NAN, NAN, NAN, NAN, NAN, ""};
  Torrents t;
  memset(&t, 0, sizeof t);
  int measures = 0, torrents = 0;
  for (char *tok = strtok(line, " \t\r"); tok; tok = strtok(NULL, " \t\r")) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    if (!strcmp(k, "bri")) {
      st7789_backlight(atoi(v) > 0);
      continue;
    }
    if (parse_torrent_key(&t, k, v)) {
      torrents = 1;
      continue;
    }
    measures = 1;
    if (!strcmp(k, "cpu")) s.cpu = strtof(v, NULL);
    else if (!strcmp(k, "ram")) s.ram = strtof(v, NULL);
    else if (!strcmp(k, "temp")) s.temp = strtof(v, NULL);
    else if (!strcmp(k, "down")) s.down = strtof(v, NULL);
    else if (!strcmp(k, "up")) s.up = strtof(v, NULL);
    else if (!strcmp(k, "os") || (!strcmp(k, "host") && !s.label[0])) {
      copy(s.label, sizeof s.label, v);
      for (char *p = s.label; *p; p++)
        if (*p == '_') *p = ' ';
    }
  }
  if (torrents) {
    Torrents a = t, b = tor; /* même résumé, à l'âge près ? (t et tor sont mis à zéro avant remplissage) */
    a.age = b.age = 0;
    tor_changed = !have_tor || memcmp(&a, &b, sizeof a) != 0;
    tor = t;
    tor_rx = HAL_GetTick();
    have_tor = 1;
    return 2;
  }
  if (!measures) return 0;
  int label_changed = strcmp(s.label, st.label) != 0;
  st = s;
  if (label_changed) draw_label();
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
  gfx_anim_reset(&anim, ANIM_X, ANIM_Y);
  draw_label();
  draw_waiting();
  next_frame = HAL_GetTick() + anim.duration_ms;
}

void sysmon_loop(void) {
  static char line[320];
  static size_t len;
  char c;

  while (rx_get(&c)) {
    if (c != '\n') {
      if (len < sizeof line - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    len = 0;
    int kind = parse_line(line);
    if (!kind) continue;
    last_rx = HAL_GetTick();
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin); /* LED bleue : clignote à chaque ligne */
    online = 1;
    if (kind == 1) push_history();
    if (!layout_drawn) show_page(0);
    else if (kind == 1 && page == 0) draw_system_values();
    else if (kind == 2 && page == 1 && tor_changed) redraw_page(); /* résumé changé */
  }

  uint32_t now = HAL_GetTick();
  if ((int32_t)(now - next_frame) >= 0) {
    frame = (frame + 1) % anim.frames;
    gfx_anim_next(&anim, frame, ANIM_X, ANIM_Y);
    next_frame += anim.duration_ms;
    if ((int32_t)(now - next_frame) > 0) next_frame = now + anim.duration_ms; /* retard : on recale */
  }

  /* Alternance des pages, seulement si un résumé torrents récent existe. */
  if (layout_drawn) {
    if (page == 1 && !torrents_valid()) show_page(0);
    else if (now - page_t >= PAGE_MS && (page == 1 || torrents_valid())) show_page(!page);
  }

  uint32_t idle = now - last_rx;
  if (online && idle > TIMEOUT_MS) {
    online = 0;
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET); /* éteinte */
  }
  if (layout_drawn && !online && idle > WAITING_MS) {
    hist_len = 0;
    draw_waiting();
  }
}
