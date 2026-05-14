#include "i2c3_bus.h"

SemaphoreHandle_t i2c3_mutex;

void i2c3_bus_init(void)
{
    i2c3_mutex = xSemaphoreCreateMutex();
}
