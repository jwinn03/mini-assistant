#include "audio.h"
#include "wm8994.h"
#include "dsp.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

extern SAI_HandleTypeDef hsai_BlockA2;
extern SAI_HandleTypeDef hsai_BlockB2;
extern I2C_HandleTypeDef hi2c3;

static int16_t audio_rx_buffer[AUDIO_DMA_SAMPLES] __attribute__((section(".audio_buffers")));
static int16_t audio_tx_buffer[AUDIO_DMA_SAMPLES] __attribute__((section(".audio_buffers")));

static TaskHandle_t audio_task_handle;

#define NOTIFY_FIRST_HALF  0
#define NOTIFY_SECOND_HALF 1

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

        uint32_t t0 = DWT->CYCCNT;
        process_audio(src, dst, AUDIO_HALF_SAMPLES);
        uint32_t cycles = DWT->CYCCNT - t0;
        dsp_cycles_last = cycles;
        if (cycles > dsp_cycles_max) dsp_cycles_max = cycles;
    }
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(audio_task_handle, NOTIFY_FIRST_HALF,
                       eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(audio_task_handle, NOTIFY_SECOND_HALF,
                       eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void audio_init(void)
{
    dsp_init();

    memset(audio_tx_buffer, 0, sizeof(audio_tx_buffer));
    memset(audio_rx_buffer, 0, sizeof(audio_rx_buffer));

    xTaskCreate(audio_task, "Audio", 256, NULL,
                configMAX_PRIORITIES - 1, &audio_task_handle);

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
    HAL_SAI_Receive_DMA(&hsai_BlockB2, (uint8_t *)audio_rx_buffer, AUDIO_DMA_SAMPLES);

    /* Now start Block A (master TX) — generates MCLK, SCK, FS.
       Block B syncs immediately. Codec needs MCLK for register writes. */
    HAL_SAI_Transmit_DMA(&hsai_BlockA2, (uint8_t *)audio_tx_buffer, AUDIO_DMA_SAMPLES);
    vTaskDelay(pdMS_TO_TICKS(10));

    wm8994_init(&hi2c3);
}
