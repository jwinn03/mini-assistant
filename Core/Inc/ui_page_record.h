#ifndef UI_PAGE_RECORD_H
#define UI_PAGE_RECORD_H

#include <stdint.h>
#include <stdbool.h>

/* Record / playback UI page. The 8th tab in the strip, sitting alongside
   the seven effect pages. Self-contained: this module owns the button
   hit-rects, file-list state, and tap-source toggle; ui.c just calls into
   the lifecycle / event hooks. */

/* Called from ui_task once when the tab is opened (or on the first render
   after boot if Rec is the default page). Performs the full body repaint:
   page title, three transport buttons, status row, file list, TAP toggle.
   Subsequent frames go through ui_page_record_tick() for delta updates. */
void ui_page_record_redraw(void);

/* Called from ui_task each frame to repaint the bits that change (button
   colors, elapsed time, peak meter, file list selection highlight). Cheap
   when nothing's changed. */
void ui_page_record_tick(void);

/* Touch dispatcher. tx,ty are the FT5336 read coordinates; edge is true
   only on the initial press of a new touch (used by buttons; sliders
   don't exist here so it's mostly an edge-triggered surface). */
void ui_page_record_touch(uint16_t tx, uint16_t ty, bool edge);

#endif
