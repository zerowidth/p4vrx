#include "canvas.h"

static const font_glyph_t *glyph(const font_t *f, char ch)
{
    return ch >= 32 && ch < 127 ? &f->glyphs[ch - 32] : &f->glyphs[0];
}

void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    for (int j = y; j < y1; ++j)
        for (int i = x; i < x1; ++i) c->px[j * c->stride + i] = color;
}

static uint16_t blend(uint16_t bg, uint16_t fg, int a)
{
    int r = ((bg >> 11) * (15 - a) + (fg >> 11) * a) / 15;
    int g = (((bg >> 5) & 63) * (15 - a) + ((fg >> 5) & 63) * a) / 15;
    int b = ((bg & 31) * (15 - a) + (fg & 31) * a) / 15;
    return (uint16_t)(r << 11 | g << 5 | b);
}

int canvas_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint16_t color)
{
    for (; *s; ++s) {
        const font_glyph_t *g = glyph(f, *s);
        const uint8_t *bits = f->bits + g->offset;
        int stride = (g->w + 1) / 2, x0 = x + g->left, y0 = y - g->top;
        for (int j = 0; j < g->h; ++j) {
            int py = y0 + j;
            if (py < 0 || py >= c->h) continue;
            for (int i = 0; i < g->w; ++i) {
                int px = x0 + i;
                if (px < 0 || px >= c->w) continue;
                uint8_t b = bits[j * stride + i / 2];
                int a = i & 1 ? b & 15 : b >> 4;
                if (a) c->px[py * c->stride + px] = a == 15 ? color : blend(c->px[py * c->stride + px], color, a);
            }
        }
        x += g->advance;
    }
    return x;
}

int canvas_text_width(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; ++s) w += glyph(f, *s)->advance;
    return w;
}
