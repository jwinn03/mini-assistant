#ifndef UI_H
#define UI_H

/* Creates the low-priority UI task. Top of the screen is a tab strip across
   all effects (see EFFECT_COUNT in dsp_chain.h); the active tab's body shows
   that effect's slider widgets. Touch in the tab strip switches pages; touch
   in the slider body drags the corresponding parameter. Call from
   StartDefaultTask after audio_init(). Depends on i2c3_bus_init(),
   lcd_init(), and ft5336 ready. */
void ui_init(void);

#endif
