#ifndef FT5336_H
#define FT5336_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f7xx_hal.h"

/* FocalTech FT5336 capacitive touch controller on STM32F746G-DISCO.
   Wired to I2C3 (same bus as WM8994 codec, mutex-protected). The driver
   takes the i2c3_mutex around each transaction. */

bool ft5336_init(I2C_HandleTypeDef *hi2c);

/* Polls the controller and returns true if at least one touch is active.
   *x and *y are filled with the first touch point in LCD pixel space
   (0..LCD_W-1, 0..LCD_H-1). Pass NULL to ignore either coordinate. */
bool ft5336_read(uint16_t *x, uint16_t *y);

#endif
