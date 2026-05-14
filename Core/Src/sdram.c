#include "sdram.h"

/* SDRAM mode register fields (JEDEC SDR):
     bits[2:0]   burst length     = 000  -> 1
     bit[3]      burst type       = 0    -> sequential
     bits[6:4]   CAS latency      = 011  -> 3
     bits[8:7]   operating mode   = 00   -> standard
     bit[9]      write burst mode = 1    -> single-location
   -> 0x230 */
#define SDRAM_MODEREG_VALUE  0x0230U

/* Refresh count: (SDCLK * tREF / num_rows) - 20.
   For SDCLK = 108 MHz, tREF = 64 ms, num_rows = 4096:
     (108e6 * 64e-3 / 4096) - 20 = 1667.
   Use 0x0603 = 1539 -> 14.4 us per row interval -> ~59 ms full refresh,
   which adds a comfortable margin under the 64 ms spec. This matches ST's
   stm32746g_discovery_sdram.c BSP reference value. */
#define SDRAM_REFRESH_COUNT  0x0603U

HAL_StatusTypeDef sdram_init_sequence(SDRAM_HandleTypeDef *hsdram)
{
    FMC_SDRAM_CommandTypeDef cmd = {0};
    HAL_StatusTypeDef status;

    /* Step 1: enable SDRAM clock to wake the device. */
    cmd.CommandMode            = FMC_SDRAM_CMD_CLK_ENABLE;
    cmd.CommandTarget          = FMC_SDRAM_CMD_TARGET_BANK1;
    cmd.AutoRefreshNumber      = 1;
    cmd.ModeRegisterDefinition = 0;
    status = HAL_SDRAM_SendCommand(hsdram, &cmd, 0x1000);
    if (status != HAL_OK) return status;

    /* Step 2: wait at least 100 us for the device to stabilize.
       SysTick is up from HAL_Init, so HAL_Delay works here. */
    HAL_Delay(1);

    /* Step 3: precharge all banks. */
    cmd.CommandMode = FMC_SDRAM_CMD_PALL;
    status = HAL_SDRAM_SendCommand(hsdram, &cmd, 0x1000);
    if (status != HAL_OK) return status;

    /* Step 4: issue 8 auto-refresh cycles. */
    cmd.CommandMode       = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
    cmd.AutoRefreshNumber = 8;
    status = HAL_SDRAM_SendCommand(hsdram, &cmd, 0x1000);
    if (status != HAL_OK) return status;

    /* Step 5: load mode register. */
    cmd.CommandMode            = FMC_SDRAM_CMD_LOAD_MODE;
    cmd.AutoRefreshNumber      = 1;
    cmd.ModeRegisterDefinition = SDRAM_MODEREG_VALUE;
    status = HAL_SDRAM_SendCommand(hsdram, &cmd, 0x1000);
    if (status != HAL_OK) return status;

    /* Step 6: program the auto-refresh interval. */
    return HAL_SDRAM_ProgramRefreshRate(hsdram, SDRAM_REFRESH_COUNT);
}
