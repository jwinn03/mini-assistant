#include "audio.h"
#include "wm8994.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

extern SAI_HandleTypeDef hsai_BlockA2;
extern SAI_HandleTypeDef hsai_BlockB2;
extern I2C_HandleTypeDef hi2c3;

volatile uint32_t audio_debug_stage = 0;
volatile HAL_StatusTypeDef audio_debug_codec_status = HAL_ERROR;
volatile HAL_StatusTypeDef audio_debug_tx_status = HAL_ERROR;
volatile HAL_StatusTypeDef audio_debug_rx_status = HAL_ERROR;
volatile uint32_t audio_debug_callback_count = 0;
volatile uint16_t audio_debug_device_id = 0;
volatile HAL_StatusTypeDef audio_debug_id_status = HAL_ERROR;
volatile uint16_t audio_debug_reg_0001 = 0;
volatile HAL_StatusTypeDef audio_debug_reg_0001_status = HAL_ERROR;
volatile uint16_t audio_debug_reg_0002 = 0;
volatile uint16_t audio_debug_reg_0003 = 0;
volatile uint16_t audio_debug_reg_0005 = 0;
volatile uint16_t audio_debug_reg_004C = 0;
volatile uint16_t audio_debug_reg_0200 = 0;
volatile uint16_t audio_debug_reg_0610 = 0;
volatile uint32_t audio_debug_sai_a_cr1 = 0;
volatile uint32_t audio_debug_sai_a_sr = 0;
volatile uint32_t audio_debug_sai_b_cr1 = 0;
volatile uint32_t audio_debug_sai_b_sr = 0;
volatile int16_t audio_debug_rx_pattern = 0;

static int16_t audio_rx_buffer[AUDIO_DMA_SAMPLES] __attribute__((section(".audio_buffers")));
static int16_t audio_tx_buffer[AUDIO_DMA_SAMPLES] __attribute__((section(".audio_buffers")));

static TaskHandle_t audio_task_handle;

#define NOTIFY_FIRST_HALF  0
#define NOTIFY_SECOND_HALF 1

volatile int16_t audio_debug_rx_max = 0;
volatile int16_t audio_debug_rx_min = 0;

static void audio_task(void *argument)
{
    uint32_t notification;
    (void)argument;

    for (;;)
    {
        xTaskNotifyWait(0, 0xFFFFFFFF, &notification, portMAX_DELAY);

        int16_t *src, *dst;
        if (notification == NOTIFY_FIRST_HALF) {
            src = audio_rx_buffer;
            dst = audio_tx_buffer;
        } else {
            src = &audio_rx_buffer[AUDIO_HALF_SAMPLES];
            dst = &audio_tx_buffer[AUDIO_HALF_SAMPLES];
        }

        int16_t max = 0, min = 0;
        for (uint32_t i = 0; i < AUDIO_HALF_SAMPLES; i++) {
            if (src[i] > max) max = src[i];
            if (src[i] < min) min = src[i];
        }
        audio_debug_rx_max = max;
        audio_debug_rx_min = min;

        memcpy(dst, src, AUDIO_HALF_SAMPLES * sizeof(int16_t));
    }
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    audio_debug_callback_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(audio_task_handle, NOTIFY_FIRST_HALF,
                       eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    audio_debug_callback_count++;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(audio_task_handle, NOTIFY_SECOND_HALF,
                       eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void audio_init(void)
{
    memset(audio_tx_buffer, 0, sizeof(audio_tx_buffer));
    memset(audio_rx_buffer, 0, sizeof(audio_rx_buffer));

    /* Create task first so the notification handle is valid before DMA callbacks fire */
    xTaskCreate(audio_task, "Audio", 256, NULL,
                configMAX_PRIORITIES - 1, &audio_task_handle);
    audio_debug_stage = 1;

    /* WM8994 DSP mode triggers on LRCLK rising edge (datasheet Fig.60).
       With FS_ACTIVE_LOW the rising edge falls at the SAI frame midpoint,
       rotating the slot mapping by 2:
         WM8994 timeslot 0 (DAC) = SAI slots 2,3
         WM8994 timeslot 1 (DMIC2) = SAI slots 0,1
       Swap the CubeMX-configured masks before DMA starts. */
    SAI2_Block_A->SLOTR = (SAI2_Block_A->SLOTR & ~SAI_xSLOTR_SLOTEN_Msk)
                          | (0xCU << SAI_xSLOTR_SLOTEN_Pos);
    SAI2_Block_B->SLOTR = (SAI2_Block_B->SLOTR & ~SAI_xSLOTR_SLOTEN_Msk)
                          | (0x3U << SAI_xSLOTR_SLOTEN_Pos);

    /* RM0385 §30.4.7: synchronous slave must be enabled BEFORE the master.
       Block B (sync slave RX) waits idle until Block A starts generating clocks. */
    audio_debug_rx_status = HAL_SAI_Receive_DMA(
        &hsai_BlockB2, (uint8_t *)audio_rx_buffer, AUDIO_DMA_SAMPLES);
    audio_debug_stage = 2;

    /* Now start Block A (master TX) — generates MCLK, SCK, FS.
       Block B syncs immediately. Codec needs MCLK for register writes. */
    audio_debug_tx_status = HAL_SAI_Transmit_DMA(
        &hsai_BlockA2, (uint8_t *)audio_tx_buffer, AUDIO_DMA_SAMPLES);
    vTaskDelay(pdMS_TO_TICKS(10));
    audio_debug_stage = 3;

    audio_debug_codec_status = wm8994_init(&hi2c3);
    audio_debug_stage = 4;

    uint16_t tmp;
    if (wm8994_read_reg(0x0000, &tmp) == HAL_OK) audio_debug_device_id = tmp;
    audio_debug_id_status = HAL_OK;
    if (wm8994_read_reg(0x0001, &tmp) == HAL_OK) audio_debug_reg_0001 = tmp;
    audio_debug_reg_0001_status = HAL_OK;
    if (wm8994_read_reg(0x0002, &tmp) == HAL_OK) audio_debug_reg_0002 = tmp;
    if (wm8994_read_reg(0x0003, &tmp) == HAL_OK) audio_debug_reg_0003 = tmp;
    if (wm8994_read_reg(0x0005, &tmp) == HAL_OK) audio_debug_reg_0005 = tmp;
    if (wm8994_read_reg(0x004C, &tmp) == HAL_OK) audio_debug_reg_004C = tmp;
    if (wm8994_read_reg(0x0200, &tmp) == HAL_OK) audio_debug_reg_0200 = tmp;
    if (wm8994_read_reg(0x0610, &tmp) == HAL_OK) audio_debug_reg_0610 = tmp;

    /* Write test pattern to RX buffer, then wait for DMA to overwrite it */
    for (int i = 0; i < AUDIO_DMA_SAMPLES; i++)
        audio_rx_buffer[i] = 0x5A5A;
    vTaskDelay(pdMS_TO_TICKS(100));
    audio_debug_rx_pattern = audio_rx_buffer[0];

    audio_debug_sai_a_cr1 = SAI2_Block_A->CR1;
    audio_debug_sai_a_sr  = SAI2_Block_A->SR;
    audio_debug_sai_b_cr1 = SAI2_Block_B->CR1;
    audio_debug_sai_b_sr  = SAI2_Block_B->SR;

    audio_debug_stage = 5;
}
