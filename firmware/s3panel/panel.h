// What the other files of the sketch share with s3panel.ino: the canvas, the
// palette, the logos and the backlight.
#pragma once
#include <Arduino_GFX_Library.h>

#define SCREEN_W 320
#define SCREEN_H 480

#define CYAN_TXT 0x45BB
#define PINK_TXT 0xD98F
#define EYE_YELLOW 0xF645
#define BODY_DARK 0x1147
#define BODY_GLOW 0x2B5B
#define CYAN_DIM 0x22D7
#define PINK_DIM 0x6146

extern Arduino_Canvas *gfx;
extern bool displayOk;

void drawLogo();
void drawMiniLogo(const char *subtitle);
void setBacklightPct(uint8_t pct);  // 0..100, PWM
