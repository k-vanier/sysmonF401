/* Pilote minimal ST7789 1.9" (170x320) sur SPI1 (HAL), en paysage 320x170.
 * Le contrôleur a 240x320 pixels de mémoire : en paysage la dalle de 170 lignes
 * commence à la ligne 35. */

#include "st7789.h"
#include "main.h"

extern SPI_HandleTypeDef hspi1;

#define Y_OFFSET 35

#define CS_LOW()   HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_RESET)
#define CS_HIGH()  HAL_GPIO_WritePin(TFT_CS_GPIO_Port, TFT_CS_Pin, GPIO_PIN_SET)
#define DC_CMD()   HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_RESET)
#define DC_DATA()  HAL_GPIO_WritePin(TFT_DC_GPIO_Port, TFT_DC_Pin, GPIO_PIN_SET)

static void spi_send(const uint8_t *b, uint16_t n) {
  HAL_SPI_Transmit(&hspi1, (uint8_t *)b, n, HAL_MAX_DELAY);
}

static void cmd(uint8_t c, const uint8_t *data, uint16_t n) {
  CS_LOW();
  DC_CMD();
  spi_send(&c, 1);
  if (n) {
    DC_DATA();
    spi_send(data, n);
  }
  CS_HIGH();
}

void st7789_begin(int x, int y, int w, int h) {
  uint16_t x0 = x, x1 = x + w - 1, y0 = y + Y_OFFSET, y1 = y + h - 1 + Y_OFFSET;
  uint8_t ca[4] = {x0 >> 8, x0, x1 >> 8, x1};
  uint8_t ra[4] = {y0 >> 8, y0, y1 >> 8, y1};
  cmd(0x2A, ca, 4); /* CASET */
  cmd(0x2B, ra, 4); /* RASET */
  uint8_t c = 0x2C; /* RAMWR, puis les pixels en données */
  CS_LOW();
  DC_CMD();
  spi_send(&c, 1);
  DC_DATA();
}

void st7789_end(void) {
  CS_HIGH();
}

/* Pixels envoyés par paquets, en big-endian comme l'attend le contrôleur. */
static uint8_t txbuf[512];

void st7789_push(const uint16_t *px, uint32_t n) {
  while (n) {
    uint16_t k = 0;
    while (n && k < sizeof txbuf) {
      txbuf[k++] = *px >> 8;
      txbuf[k++] = *px++;
      n--;
    }
    spi_send(txbuf, k);
  }
}

void st7789_backlight(int on) {
  HAL_GPIO_WritePin(TFT_BLK_GPIO_Port, TFT_BLK_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void st7789_init(int flip) {
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(10);
  HAL_GPIO_WritePin(TFT_RST_GPIO_Port, TFT_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(120);

  cmd(0x01, NULL, 0); /* SWRESET */
  HAL_Delay(150);
  cmd(0x11, NULL, 0); /* SLPOUT */
  HAL_Delay(120);
  uint8_t colmod = 0x55; /* 16 bits par pixel */
  cmd(0x3A, &colmod, 1);
  uint8_t madctl = flip ? 0xA0 : 0x60; /* MV + MX (paysage) ou MV + MY (retourné) */
  cmd(0x36, &madctl, 1);
  cmd(0x21, NULL, 0); /* INVON : nécessaire sur les dalles IPS */
  cmd(0x13, NULL, 0); /* NORON */
  HAL_Delay(10);
  st7789_fill_rect(0, 0, ST7789_W, ST7789_H, 0);
  cmd(0x29, NULL, 0); /* DISPON */
  HAL_Delay(10);
}

void st7789_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  for (unsigned i = 0; i < sizeof txbuf; i += 2) {
    txbuf[i] = color >> 8;
    txbuf[i + 1] = color;
  }
  st7789_begin(x, y, w, h);
  uint32_t n = (uint32_t)w * h * 2;
  while (n) {
    uint16_t k = n > sizeof txbuf ? sizeof txbuf : n;
    spi_send(txbuf, k);
    n -= k;
  }
  st7789_end();
}

void st7789_blit(int x, int y, int w, int h, const uint16_t *px) {
  st7789_begin(x, y, w, h);
  st7789_push(px, (uint32_t)w * h);
  st7789_end();
}
