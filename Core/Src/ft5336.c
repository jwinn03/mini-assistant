#include "ft5336.h"
#include "i2c3_bus.h"

#define FT5336_I2C_ADDR      0x70   /* 7-bit 0x38 shifted -> 8-bit HAL form */
#define FT5336_REG_TD_STATUS 0x02
#define FT5336_REG_TOUCH1    0x03   /* XH, XL, YH, YL follow */
#define FT5336_REG_G_CIPHER  0xA3
#define FT5336_REG_G_MODE    0xA4   /* 0 = polling, 1 = interrupt trigger */
#define FT5336_I2C_TIMEOUT   100

static I2C_HandleTypeDef *ts_i2c;

static HAL_StatusTypeDef ft5336_read_byte(uint8_t reg, uint8_t *value)
{
    xSemaphoreTake(i2c3_mutex, portMAX_DELAY);
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(ts_i2c, FT5336_I2C_ADDR, reg,
                                                I2C_MEMADD_SIZE_8BIT, value, 1,
                                                FT5336_I2C_TIMEOUT);
    xSemaphoreGive(i2c3_mutex);
    return status;
}

static HAL_StatusTypeDef ft5336_write_byte(uint8_t reg, uint8_t value)
{
    xSemaphoreTake(i2c3_mutex, portMAX_DELAY);
    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(ts_i2c, FT5336_I2C_ADDR, reg,
                                                 I2C_MEMADD_SIZE_8BIT, &value, 1,
                                                 FT5336_I2C_TIMEOUT);
    xSemaphoreGive(i2c3_mutex);
    return status;
}

bool ft5336_init(I2C_HandleTypeDef *hi2c)
{
    ts_i2c = hi2c;

    /* Probe the device by reading the chip-ID register. A successful HAL_OK
       means the slave ACKed at FT5336_I2C_ADDR; the actual cipher value
       varies across FT53xx silicon revisions, so don't validate the byte. */
    uint8_t cipher = 0;
    if (ft5336_read_byte(FT5336_REG_G_CIPHER, &cipher) != HAL_OK) {
        return false;
    }

    /* Force polling mode (G_MODE = 0). The board has TS_INT wired to PI13
       (configured as EXTI in CubeMX) but Phase 3 polls and ignores it. */
    return ft5336_write_byte(FT5336_REG_G_MODE, 0x00) == HAL_OK;
}

bool ft5336_read(uint16_t *x, uint16_t *y)
{
    /* One burst read covers TD_STATUS + the first touch point:
         buf[0] = TD_STATUS  (number of active touches in bits[3:0])
         buf[1] = TOUCH1_XH  (bits[7:6] event, bits[3:0] X high)
         buf[2] = TOUCH1_XL  (X low)
         buf[3] = TOUCH1_YH  (bits[7:4] ID, bits[3:0] Y high)
         buf[4] = TOUCH1_YL  (Y low) */
    uint8_t buf[5] = {0};
    xSemaphoreTake(i2c3_mutex, portMAX_DELAY);
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(ts_i2c, FT5336_I2C_ADDR,
                                                FT5336_REG_TD_STATUS,
                                                I2C_MEMADD_SIZE_8BIT, buf, 5,
                                                FT5336_I2C_TIMEOUT);
    xSemaphoreGive(i2c3_mutex);
    if (status != HAL_OK) return false;

    uint8_t num_touches = buf[0] & 0x0F;
    if (num_touches == 0 || num_touches > 5) return false;

    uint16_t raw_x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
    uint16_t raw_y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];

    /* Direct passthrough — F746G-DISCO touch axes are nominally aligned with
       the LTDC pixel coordinate system. If touch reports inverted Y or
       swapped axes during bring-up, adjust here. */
    if (x) *x = raw_x;
    if (y) *y = raw_y;
    return true;
}
