#include "ui.h"
#include "lcd.h"
#include "ft5336.h"
#include "dsp.h"
#include "FreeRTOS.h"
#include "task.h"

extern I2C_HandleTypeDef hi2c3;

#define UI_TITLE_Y        30
#define UI_BAR_Y          130
#define UI_BAR_H          30
#define UI_TEXT_Y         180
#define UI_TOUCH_TOP      80       /* touch above this row is ignored */
#define UI_BAR_BG         LCD_DKGRAY
#define UI_BAR_FG         LCD_CYAN
#define UI_TEXT_FG        LCD_WHITE
#define UI_TEXT_BG        LCD_BLACK

static float    s_gain_db    = 0.0f;
static uint16_t s_last_bar_w = 0xFFFF;

static uint16_t gain_to_bar_width(float db)
{
    if (db < (float)DSP_GAIN_DB_MIN) db = (float)DSP_GAIN_DB_MIN;
    if (db > (float)DSP_GAIN_DB_MAX) db = (float)DSP_GAIN_DB_MAX;
    float t = (db - (float)DSP_GAIN_DB_MIN) /
              (float)(DSP_GAIN_DB_MAX - DSP_GAIN_DB_MIN);
    return (uint16_t)(t * LCD_W);
}

static int format_gain(float db, char *out)
{
    int db_int = (int)(db + (db >= 0.0f ? 0.5f : -0.5f));
    if (db_int < DSP_GAIN_DB_MIN) db_int = DSP_GAIN_DB_MIN;
    if (db_int > DSP_GAIN_DB_MAX) db_int = DSP_GAIN_DB_MAX;

    char *p = out;
    *p++ = 'G'; *p++ = 'a'; *p++ = 'i'; *p++ = 'n'; *p++ = ':'; *p++ = ' ';
    if (db_int >= 0) {
        *p++ = '+';
    } else {
        *p++ = '-';
        db_int = -db_int;
    }
    if (db_int >= 10) *p++ = '0' + (db_int / 10);
    *p++ = '0' + (db_int % 10);
    *p++ = ' '; *p++ = 'd'; *p++ = 'B';
    *p = '\0';
    return p - out;
}

static int strlen8(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void ui_task(void *arg)
{
    (void)arg;
    char text_buf[16];

    if (!ft5336_init(&hi2c3)) {
        /* Touch init failed: show error and stop the UI task. Audio keeps running. */
        lcd_draw_text(8, 8, "Touch init failed", LCD_RED, LCD_BLACK);
        vTaskDelete(NULL);
    }

    /* Static chrome */
    const char *title = "MINI ASSISTANT";
    int title_x = (LCD_W - strlen8(title) * LCD_FONT_W) / 2;
    lcd_draw_text(title_x, UI_TITLE_Y, title, LCD_WHITE, LCD_BLACK);

    /* Initial bar fill (background only — first frame's redraw paints the rest) */
    lcd_fill_rect(0, UI_BAR_Y, LCD_W, UI_BAR_H, UI_BAR_BG);
    s_last_bar_w = 0;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(33));

        uint16_t tx, ty;
        if (ft5336_read(&tx, &ty) && ty >= UI_TOUCH_TOP) {
            float new_db = ((float)tx / (float)LCD_W) *
                           (float)(DSP_GAIN_DB_MAX - DSP_GAIN_DB_MIN) +
                           (float)DSP_GAIN_DB_MIN;
            s_gain_db = new_db;
            dsp_set_gain_db(new_db);
        }

        uint16_t bar_w = gain_to_bar_width(s_gain_db);

        lcd_wait_vblank();

        /* Bar: only redraw the changed portion to keep VBLANK budget small. */
        if (bar_w != s_last_bar_w) {
            if (bar_w > s_last_bar_w) {
                /* Bar grew right -> paint new filled region */
                lcd_fill_rect(s_last_bar_w, UI_BAR_Y,
                              bar_w - s_last_bar_w, UI_BAR_H, UI_BAR_FG);
            } else {
                /* Bar shrunk -> paint exposed bg region */
                lcd_fill_rect(bar_w, UI_BAR_Y,
                              s_last_bar_w - bar_w, UI_BAR_H, UI_BAR_BG);
            }
            s_last_bar_w = bar_w;
        }

        /* Numeric readout: redraw text band (480 x 16 px ~ 15 KB / ~700us) */
        int len = format_gain(s_gain_db, text_buf);
        int text_x = (LCD_W - len * LCD_FONT_W) / 2;
        lcd_fill_rect(0, UI_TEXT_Y, LCD_W, LCD_FONT_H, UI_TEXT_BG);
        lcd_draw_text(text_x, UI_TEXT_Y, text_buf, UI_TEXT_FG, UI_TEXT_BG);
    }
}

void ui_init(void)
{
    xTaskCreate(ui_task, "UI", 512, NULL, tskIDLE_PRIORITY + 1, NULL);
}
