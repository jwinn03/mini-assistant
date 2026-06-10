#include "feature_dump.h"

#include <string.h>

/* Both buffers live in .sdram (NOLOAD — contents undefined at reset). That's
   fine without a memset: readers only consume entries below the counters,
   and the counters are .bss / reset by feature_dump_init. ~400 KB of the
   ~5.9 MB free SDRAM. Single writer (wake-word task); the debugger reads
   after feature_dump_done goes to 1, so no synchronization is needed. */
__attribute__((section(".sdram"), aligned(4)))
int16_t  feature_dump_pcm_buf[FEATURE_DUMP_PCM_CAPACITY];
__attribute__((section(".sdram"), aligned(4)))
uint16_t feature_dump_frame_buf[FEATURE_DUMP_FRAME_CAPACITY][40];

volatile uint32_t feature_dump_pcm_count   = 0;
volatile uint32_t feature_dump_frame_count = 0;
volatile uint32_t feature_dump_done        = 0;
volatile uint32_t feature_dump_invalid     = 0;
volatile uint32_t feature_dump_rearm       = 0;

void feature_dump_init(void)
{
    feature_dump_pcm_count   = 0;
    feature_dump_frame_count = 0;
    feature_dump_done        = 0;
    feature_dump_invalid     = 0;
    feature_dump_rearm       = 0;
}

int feature_dump_capturing(void)
{
    return feature_dump_done == 0u;
}

void feature_dump_pcm(const int16_t *samples, uint32_t n)
{
    if (feature_dump_done) {
        return;
    }

    uint32_t count = feature_dump_pcm_count;
    if (count + n > FEATURE_DUMP_PCM_CAPACITY) {
        /* A partial hop would desync sample/frame alignment — stop cleanly
           at the last whole-hop boundary instead. */
        feature_dump_done = 1;
        return;
    }

    memcpy(&feature_dump_pcm_buf[count], samples, n * sizeof(int16_t));
    feature_dump_pcm_count = count + n;

    if (feature_dump_pcm_count + n > FEATURE_DUMP_PCM_CAPACITY) {
        feature_dump_done = 1;   /* next hop wouldn't fit; finish now */
    }
}

void feature_dump_frame(const uint16_t *feat)
{
    if (feature_dump_done || feature_dump_frame_count >= FEATURE_DUMP_FRAME_CAPACITY) {
        return;
    }
    memcpy(feature_dump_frame_buf[feature_dump_frame_count], feat,
           40u * sizeof(uint16_t));
    feature_dump_frame_count = feature_dump_frame_count + 1u;
}

int feature_dump_take_rearm(void)
{
    if (!feature_dump_rearm) {
        return 0;
    }
    feature_dump_init();
    return 1;
}

void feature_dump_mark_invalid(void)
{
    if (!feature_dump_done) {
        feature_dump_invalid = 1;
    }
}
