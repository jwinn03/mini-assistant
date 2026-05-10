#include "wm8994.h"
#include "cmsis_os.h"

static I2C_HandleTypeDef *codec_i2c;

volatile uint32_t wm8994_write_ok_count = 0;
volatile uint32_t wm8994_write_fail_count = 0;
volatile uint16_t wm8994_first_fail_reg = 0;
volatile HAL_StatusTypeDef wm8994_first_fail_status = HAL_OK;

HAL_StatusTypeDef wm8994_write_reg(uint16_t reg, uint16_t value)
{
    uint8_t data[2];
    data[0] = (value >> 8) & 0xFF;
    data[1] = value & 0xFF;
    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(codec_i2c, WM8994_I2C_ADDR, reg,
                             I2C_MEMADD_SIZE_16BIT, data, 2, 100);
    if (status == HAL_OK) {
        wm8994_write_ok_count++;
    } else {
        if (wm8994_write_fail_count == 0) {
            wm8994_first_fail_reg = reg;
            wm8994_first_fail_status = status;
        }
        wm8994_write_fail_count++;
    }
    return status;
}

HAL_StatusTypeDef wm8994_read_reg(uint16_t reg, uint16_t *value)
{
    uint8_t data[2];
    HAL_StatusTypeDef status;
    status = HAL_I2C_Mem_Read(codec_i2c, WM8994_I2C_ADDR, reg,
                              I2C_MEMADD_SIZE_16BIT, data, 2, 100);
    if (status == HAL_OK)
        *value = (data[0] << 8) | data[1];
    return status;
}

HAL_StatusTypeDef wm8994_init(I2C_HandleTypeDef *hi2c)
{
    HAL_StatusTypeDef status;
    codec_i2c = hi2c;

    /* Software reset */
    status = wm8994_write_reg(0x0000, 0x0000);
    if (status != HAL_OK) return status;
    osDelay(50);

    /* Errata workarounds */
    wm8994_write_reg(0x0102, 0x0003);
    wm8994_write_reg(0x0817, 0x0000);
    wm8994_write_reg(0x0102, 0x0000);

    /* Anti-pop */
    wm8994_write_reg(0x0039, 0x006C);

    /* Power Management 1: VMID_SEL=01 (2x50k), BIAS_ENA=1 */
    wm8994_write_reg(0x0001, 0x0003);
    osDelay(50);

    /* ---- Clocking: MCLK-direct mode ---- */

    wm8994_write_reg(0x0200, 0x0001);  /* AIF1CLK_SRC=MCLK1, AIF1CLK_ENA */
    wm8994_write_reg(0x0208, 0x000A);  /* AIF1DSPCLK_ENA, SYSDSPCLK_ENA */
    wm8994_write_reg(0x0210, 0x0083);  /* AIF1_SR=48kHz */

    /* ---- AIF1: I2S, 16-bit, codec is slave ---- */

    wm8994_write_reg(0x0300, 0x4010);  /* AIF1_FMT=I2S, AIF1_WL=16-bit */
    wm8994_write_reg(0x0302, 0x0000);  /* AIF1_MSTR=0 (slave) */

    /* ---- Output path: AIF1 -> DAC1 -> Output Mixer -> Headphones ---- */

    wm8994_write_reg(0x0005, 0x0303);  /* AIF1DAC1L/R enable, DAC1L/R enable */
    wm8994_write_reg(0x0601, 0x0001);  /* AIF1 timeslot 0 -> DAC1L */
    wm8994_write_reg(0x0602, 0x0001);  /* AIF1 timeslot 0 -> DAC1R */
    wm8994_write_reg(0x0604, 0x0000);  /* Clear DAC2L mixer */
    wm8994_write_reg(0x0605, 0x0000);  /* Clear DAC2R mixer */

    /* ---- Input path: Line in (IN1L/IN1R) -> ADC -> AIF1 ---- */

    wm8994_write_reg(0x0028, 0x0011);  /* Input mixer: IN1LN->IN1L, IN1RN->IN1R */
    wm8994_write_reg(0x0029, 0x0035);  /* Left input mixer: IN1L to MIXINL */
    wm8994_write_reg(0x002A, 0x0035);  /* Right input mixer: IN1R to MIXINR */
    wm8994_write_reg(0x0004, 0x0303);  /* AIF1ADC1L/R enable */
    wm8994_write_reg(0x0002, 0x6350);  /* IN1L/R enable, MIXINL/R enable, thermal */
    wm8994_write_reg(0x0606, 0x0002);  /* ADC1L -> AIF1 timeslot 0 left */
    wm8994_write_reg(0x0607, 0x0002);  /* ADC1R -> AIF1 timeslot 0 right */
    wm8994_write_reg(0x0700, 0x800D);  /* GPIO1 */

    /* ---- Output mixer & power ---- */

    wm8994_write_reg(0x0003, 0x0330);  /* MIXOUTL/R enable */
    wm8994_write_reg(0x0001, 0x0303);  /* VMID, BIAS, HPOUT1L/R enable */
    wm8994_write_reg(0x002D, 0x0001);  /* DAC1L -> output mixer left */
    wm8994_write_reg(0x002E, 0x0001);  /* DAC1R -> output mixer right */

    /* ---- Headphone output enable sequence ---- */

    wm8994_write_reg(0x0051, 0x0005);  /* Class W envelope tracking */
    wm8994_write_reg(0x004C, 0x9F25);  /* Charge pump enable */
    osDelay(15);

    wm8994_write_reg(0x0054, 0x0033);  /* DC servo enable */
    wm8994_write_reg(0x0060, 0x0022);  /* HP intermediate stage */
    osDelay(300);

    wm8994_write_reg(0x0060, 0x00EE);  /* HP output stage */
    wm8994_write_reg(0x0060, 0x00FF);  /* HP remove short */

    /* ---- Volume & unmute ---- */

    wm8994_write_reg(0x0610, 0x01C0);  /* DAC1L 0 dB */
    wm8994_write_reg(0x0611, 0x01C0);  /* DAC1R 0 dB */
    wm8994_write_reg(0x0402, 0x01C0);  /* AIF1 DAC1L 0 dB */
    wm8994_write_reg(0x0403, 0x01C0);  /* AIF1 DAC1R 0 dB */
    wm8994_write_reg(0x0420, 0x0000);  /* Unmute DAC1 */

    /* ---- Input volume & HPF ---- */

    wm8994_write_reg(0x0018, 0x000B);  /* IN1L volume 0 dB, unmute */
    wm8994_write_reg(0x001A, 0x000B);  /* IN1R volume 0 dB, unmute */
    wm8994_write_reg(0x0400, 0x01C0);  /* AIF1 ADC1L 0 dB */
    wm8994_write_reg(0x0401, 0x01C0);  /* AIF1 ADC1R 0 dB */
    wm8994_write_reg(0x0410, 0x1800);  /* ADC HPF */

    wm8994_write_reg(0x001C, 0x017F);  /* HP left volume max, VU, unmute */
    wm8994_write_reg(0x001D, 0x017F);  /* HP right volume max, VU, unmute */

    return HAL_OK;
}

void wm8994_set_headphone_volume(uint8_t volume)
{
    if (volume > 63)
        volume = 63;
    uint16_t val = 0x0100 | volume;
    wm8994_write_reg(0x001C, val);
    wm8994_write_reg(0x001D, val);
}
