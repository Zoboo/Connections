#pragma once

#include <switch.h>
#include "image.h"

void drawing_init(void);
void drawing_shutdown(void);

void drawing_rect(int x, int y, int w, int h, u32 color);
void drawing_rect_outline(int x, int y, int w, int h, int thickness, u32 color);
void drawing_round_rect(int x, int y, int w, int h, int radius, u32 color);
void drawing_round_rect_outline(int x, int y, int w, int h, int radius, int thickness, u32 color);
void drawing_circle(int center_x, int center_y, int radius, u32 color);
void drawing_image(const Image *image, int x, int y);
void drawing_image_scaled(const Image *image, int x, int y, int w, int h);
int drawing_text_width(const char *text, int scale);
int drawing_text_bold_width(const char *text, int scale);
int drawing_text_height(int scale);
void drawing_text(const char *text, int x, int y, int scale, u32 color);
void drawing_text_bold(const char *text, int x, int y, int scale, u32 color);
