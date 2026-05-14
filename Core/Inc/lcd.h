#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_W       480
#define LCD_H       272
#define LCD_FB_ADDR 0xC0000000U
#define LCD_FB      ((uint16_t *)LCD_FB_ADDR)

/* RGB565 helpers */
#define LCD_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define LCD_BLACK   0x0000
#define LCD_WHITE   0xFFFF
#define LCD_RED     0xF800
#define LCD_GREEN   0x07E0
#define LCD_BLUE    0x001F
#define LCD_GRAY    0x8410
#define LCD_DKGRAY  0x4208
#define LCD_LTGRAY  0xC618
#define LCD_YELLOW  0xFFE0
#define LCD_CYAN    0x07FF
#define LCD_MAGENTA 0xF81F
#define LCD_ORANGE  0xFD20

#define LCD_FONT_W  8
#define LCD_FONT_H  16

/* Call once from StartDefaultTask, after sdram_init_sequence has run.
   Creates the VBLANK semaphore, arms the LTDC line-event, and clears
   the framebuffer to LCD_BLACK. */
void lcd_init(void);

void lcd_clear(uint16_t color);
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void lcd_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
void lcd_draw_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg);

/* Blocks the calling task until the next LTDC VBLANK boundary.
   Use before partial framebuffer redraws to avoid tearing. */
void lcd_wait_vblank(void);

#endif
