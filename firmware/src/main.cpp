// Afficheur CPU / RAM / température / réseau pour serveur FreeBSD.
// WeAct Black Pill STM32F401 + écran ST7789 1.9" (170x320), en paysage 320x170.
//
// Le PC envoie une ligne de texte par seconde sur le port série USB, ex. :
//   cpu=12.5 ram=43.1 temp=52.0 down=123456 up=7890 host=srv1
// down / up en octets par seconde. Une clé absente = mesure indisponible (« -- »).
// bri=0..100 règle le rétroéclairage (peut être seul sur sa ligne).

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// Câblage écran -> Black Pill (SPI1) :
//   GND -> GND, VCC -> 3V3, SCL -> PA5, SDA -> PA7, RES -> PA2, DC -> PA3, CS -> PA4, BLK -> PA1
#define TFT_CS  PA4
#define TFT_DC  PA3
#define TFT_RST PA2
#define TFT_BLK PA1 // PWM TIM2_CH2

#define ROTATION 1 // 1 = paysage, 3 = paysage retourné
#define TIMEOUT_MS 5000 // au-delà sans données : « hors ligne »

static const int W = 320, H = 170;

Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

// Thème sombre, couleurs proches de miniscreen.
static uint16_t C_BG, C_PANEL, C_TEXT, C_DIM, C_CPU, C_RAM, C_TEMP, C_RED, C_DOWN, C_DOWN_FILL, C_UP, C_OK;

struct Stats {
  float cpu = NAN, ram = NAN, temp = NAN;
  float down = NAN, up = NAN;
  char host[24] = "";
};

static Stats st;
static uint32_t lastRx = 0;
static bool online = false, layoutDrawn = false;

// Historique réseau pour le graphe (un point par ligne reçue).
static const int GX = 158, GY = 104, GW = 158, GH = 62;
static float histDown[GW], histUp[GW];
static int histLen = 0;

// --- Mise en page ---

static void setBacklight(int pct) {
  analogWrite(TFT_BLK, constrain(pct, 0, 100) * 255 / 100);
}

// Texte aligné à droite sur `right`, sur fond plein : écrase l'ancienne valeur sans clignoter.
static void textRight(int right, int y, const char *s, uint16_t fg, int width) {
  tft.setTextSize(2);
  tft.fillRect(right - width, y, width - (int)strlen(s) * 12, 16, C_BG);
  tft.setTextColor(fg, C_BG);
  tft.setCursor(right - (int)strlen(s) * 12, y);
  tft.print(s);
}

static void drawHeader() {
  tft.fillRect(0, 0, W, 22, C_PANEL);
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setCursor(6, 3);
  tft.print(st.host[0] ? st.host : "FreeBSD");
}

static void drawStatus() {
  tft.fillCircle(W - 12, 11, 5, online ? C_OK : C_RED);
}

static const int BAR_X = 50, BAR_W = 190, BAR_H = 18;
static const int ROW_CPU = 30, ROW_RAM = 54, ROW_TEMP = 78;

static void drawLayout() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatus();
  const char *labels[] = {"CPU", "RAM", "TMP"};
  const int rows[] = {ROW_CPU, ROW_RAM, ROW_TEMP};
  tft.setTextSize(2);
  for (int i = 0; i < 3; i++) {
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(6, rows[i] + 1);
    tft.print(labels[i]);
    tft.drawRoundRect(BAR_X, rows[i], BAR_W, BAR_H, 4, C_PANEL);
  }
  tft.drawFastHLine(0, 100, W, C_PANEL);
  tft.drawRect(GX - 1, GY - 1, GW + 2, GH + 2, C_PANEL);
  tft.setTextColor(C_DOWN, C_BG);
  tft.setCursor(6, 110);
  tft.write(0x19); // flèche bas (police cp437)
  tft.setTextColor(C_UP, C_BG);
  tft.setCursor(6, 140);
  tft.write(0x18); // flèche haut
  layoutDrawn = true;
}

// Barre + valeur. pct dans 0..100, NAN = indisponible.
static void drawGauge(int y, float pct, uint16_t color, const char *text) {
  int inner = BAR_W - 4;
  int fill = isnan(pct) ? 0 : (int)(constrain(pct, 0.0f, 100.0f) * inner / 100 + 0.5f);
  if (fill > 0) tft.fillRect(BAR_X + 2, y + 2, fill, BAR_H - 4, color);
  if (fill < inner) tft.fillRect(BAR_X + 2 + fill, y + 2, inner - fill, BAR_H - 4, C_BG);
  textRight(W - 4, y + 1, text, isnan(pct) ? C_DIM : C_TEXT, 72);
}

static uint16_t alert(uint16_t c, float pct, float limit) { return pct >= limit ? C_RED : c; }

// Débit en octets/s, au plus 10 caractères : « 123.4 Ko/s ».
static void fmtRate(float b, char *out, size_t n) {
  if (isnan(b)) snprintf(out, n, "   -- o/s");
  else if (b >= 1073741824.0f) snprintf(out, n, "%5.1f Go/s", b / 1073741824.0f);
  else if (b >= 1048576.0f) snprintf(out, n, "%5.1f Mo/s", b / 1048576.0f);
  else if (b >= 1024.0f) snprintf(out, n, "%5.1f Ko/s", b / 1024.0f);
  else snprintf(out, n, "%5.0f  o/s", b);
}

static void drawGraph() {
  float top = 1024; // échelle mini 1 Ko/s
  for (int i = 0; i < histLen; i++) top = max(top, max(histDown[i], histUp[i]));
  // Colonne par colonne (fond puis aire) pour éviter un effacement complet qui clignote.
  int off = GW - histLen; // les points les plus récents à droite
  int prevUy = -1;
  for (int x = 0; x < GW; x++) {
    int i = x - off;
    int dh = 0;
    if (i >= 0) dh = (int)(histDown[i] / top * (GH - 1) + 0.5f);
    if (dh < GH) tft.drawFastVLine(GX + x, GY, GH - dh, C_BG);
    if (dh > 0) tft.drawFastVLine(GX + x, GY + GH - dh, dh, C_DOWN_FILL);
    if (i >= 0) tft.drawPixel(GX + x, GY + GH - max(dh, 1), C_DOWN);
  }
  // Envoi : ligne par-dessus l'aire de réception.
  for (int x = off; x < GW; x++) {
    int uy = GY + GH - 1 - (int)(histUp[x - off] / top * (GH - 1) + 0.5f);
    if (prevUy >= 0) tft.drawLine(GX + x - 1, prevUy, GX + x, uy, C_UP);
    else tft.drawPixel(GX + x, uy, C_UP);
    prevUy = uy;
  }
}

static void drawStats() {
  if (!layoutDrawn) drawLayout();
  char buf[16];

  if (isnan(st.cpu)) strcpy(buf, "--");
  else snprintf(buf, sizeof buf, "%d%%", (int)(st.cpu + 0.5f));
  drawGauge(ROW_CPU, st.cpu, alert(C_CPU, st.cpu, 80), buf);

  if (isnan(st.ram)) strcpy(buf, "--");
  else snprintf(buf, sizeof buf, "%d%%", (int)(st.ram + 0.5f));
  drawGauge(ROW_RAM, st.ram, alert(C_RAM, st.ram, 80), buf);

  // Température : barre de 0 à 100 °C, rouge à partir de 75 °C.
  if (isnan(st.temp)) strcpy(buf, "--");
  else snprintf(buf, sizeof buf, "%d\xF8" "C", (int)(st.temp + 0.5f)); // 0xF8 = ° en cp437
  drawGauge(ROW_TEMP, st.temp, alert(C_TEMP, st.temp, 75), buf);

  fmtRate(st.down, buf, sizeof buf);
  textRight(GX - 6, 110, buf, C_TEXT, 120);
  fmtRate(st.up, buf, sizeof buf);
  textRight(GX - 6, 140, buf, C_TEXT, 120);

  if (histLen == GW) {
    memmove(histDown, histDown + 1, (GW - 1) * sizeof(float));
    memmove(histUp, histUp + 1, (GW - 1) * sizeof(float));
    histLen--;
  }
  histDown[histLen] = isnan(st.down) ? 0 : st.down;
  histUp[histLen] = isnan(st.up) ? 0 : st.up;
  histLen++;
  drawGraph();
}

static void drawWaiting() {
  tft.fillScreen(C_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_DIM, C_BG);
  const char *msg = "En attente du serveur";
  tft.setCursor((W - (int)strlen(msg) * 12) / 2, H / 2 - 8);
  tft.print(msg);
  layoutDrawn = false;
}

// --- Réception ---

// Analyse une ligne « clé=valeur clé=valeur ... ». Renvoie true si elle contient des mesures.
static bool parseLine(char *line) {
  Stats s;
  bool measures = false;
  for (char *tok = strtok(line, " \t\r"); tok; tok = strtok(nullptr, " \t\r")) {
    char *eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    if (!strcmp(k, "bri")) { setBacklight(atoi(v)); continue; }
    measures = true;
    if (!strcmp(k, "cpu")) s.cpu = atof(v);
    else if (!strcmp(k, "ram")) s.ram = atof(v);
    else if (!strcmp(k, "temp")) s.temp = atof(v);
    else if (!strcmp(k, "down")) s.down = atof(v);
    else if (!strcmp(k, "up")) s.up = atof(v);
    else if (!strcmp(k, "host")) strlcpy(s.host, v, sizeof s.host);
  }
  if (!measures) return false;
  bool hostChanged = strcmp(s.host, st.host) != 0;
  st = s;
  if (layoutDrawn && hostChanged) {
    drawHeader();
    drawStatus();
  }
  return true;
}

void setup() {
  C_BG = rgb(10, 12, 18);
  C_PANEL = rgb(40, 46, 60);
  C_TEXT = rgb(230, 232, 240);
  C_DIM = rgb(120, 128, 145);
  C_CPU = rgb(64, 170, 255);
  C_RAM = rgb(180, 110, 255);
  C_TEMP = rgb(255, 160, 40);
  C_RED = rgb(255, 60, 60);
  C_DOWN = rgb(60, 220, 120);
  C_DOWN_FILL = rgb(20, 90, 50);
  C_UP = rgb(190, 120, 255);
  C_OK = rgb(60, 220, 120);

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(100);
  tft.init(170, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(ROTATION);
  tft.cp437(true);
  drawWaiting();

  Serial.begin(115200); // débit ignoré en USB CDC
}

void loop() {
  static char line[160];
  static size_t len = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[len] = 0;
      len = 0;
      if (parseLine(line)) {
        lastRx = millis();
        if (!online) {
          online = true;
          if (layoutDrawn) drawStatus();
        }
        drawStats();
      }
    } else if (len < sizeof line - 1) {
      line[len++] = c;
    }
  }

  if (online && millis() - lastRx > TIMEOUT_MS) {
    online = false;
    drawStatus();
  }
  // Toujours rien après une minute : retour à l'écran d'attente.
  if (layoutDrawn && !online && millis() - lastRx > 60000) {
    histLen = 0;
    drawWaiting();
  }
}
