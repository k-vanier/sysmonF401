/* Remplace Core/Inc/main.h pour compiler l'affichage sur PC (voir preview.c). */
#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;

#define LED_GPIO_Port 0
#define LED_Pin 0

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(int port, int pin, GPIO_PinState state);
void HAL_GPIO_TogglePin(int port, int pin);

#endif
