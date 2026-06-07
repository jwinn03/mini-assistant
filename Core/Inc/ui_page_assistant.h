#ifndef UI_PAGE_ASSISTANT_H
#define UI_PAGE_ASSISTANT_H

#include <stdint.h>
#include <stdbool.h>

/* Assistant page — Phase 6 step 7 stub.
   The 9th tab. Shows a wake-fire indicator that flashes for 500 ms whenever
   wake_word_total_fires increments, plus three diagnostic readouts:
   inference count (proof the wake-word task is alive), last dequantised
   confidence, and total wake-fires. Phase 7 will replace this body with
   the VAD utterance-capture UI; this page is the place to hang that work. */

void ui_page_assistant_redraw(void);
void ui_page_assistant_tick(void);
void ui_page_assistant_touch(uint16_t tx, uint16_t ty, bool edge);

#endif
