#ifndef I2C3_BUS_H
#define I2C3_BUS_H

#include "FreeRTOS.h"
#include "semphr.h"

/* Shared mutex protecting the I2C3 bus.
   I2C3 is wired to two slaves on the F746G-DISCO board:
     - WM8994 audio codec (0x34)
     - FT5336 capacitive touch (0x70)
   Both drivers must take this mutex around any HAL_I2C call.
   FreeRTOS mutexes give priority inheritance, which prevents the low-priority
   UI task from blocking the audio task during contention. */
extern SemaphoreHandle_t i2c3_mutex;

void i2c3_bus_init(void);

#endif
