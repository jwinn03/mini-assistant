#ifndef UI_H
#define UI_H

/* Creates the low-priority UI task that polls the touch panel, drives
   dsp_set_gain_db() in response, and redraws the gain bar + readout each
   frame. Call from StartDefaultTask after audio_init(). Depends on
   i2c3_bus_init(), lcd_init(), and ft5336 ready. */
void ui_init(void);

#endif
