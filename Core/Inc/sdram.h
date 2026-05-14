#ifndef SDRAM_H
#define SDRAM_H

#include "stm32f7xx_hal.h"

/* IS42S32400F-7TL SDRAM on F746G-DISCO, accessed as 16-bit by FMC.
   Sends the JEDEC-mandated power-up command sequence and programs the
   refresh-rate counter. MX_FMC_Init() configures the controller but does
   NOT send these commands — call this at end of MX_FMC_Init (USER CODE
   BEGIN FMC_Init 2) so the LTDC scanout that follows reads valid data. */
HAL_StatusTypeDef sdram_init_sequence(SDRAM_HandleTypeDef *hsdram);

#endif
