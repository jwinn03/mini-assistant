#ifndef WM8994_H
#define WM8994_H

#include "stm32f7xx_hal.h"

#define WM8994_I2C_ADDR  0x34

HAL_StatusTypeDef wm8994_init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef wm8994_write_reg(uint16_t reg, uint16_t value);
HAL_StatusTypeDef wm8994_read_reg(uint16_t reg, uint16_t *value);
void wm8994_set_headphone_volume(uint8_t volume);

#endif
