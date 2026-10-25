#include "drawing.h"
#include "sdl_renderer.h"
#include "../platform/platform_graphics.h"
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <math.h>

#define TEXT_CACHE_SIZE 192
#define TEXT_CACHE_BYTES (4u * 1024u * 1024u)
#define TEXT_MAX_LENGTH 256
#define FONT_SCALE_MAX 12

typedef struct {
    char text[TEXT_MAX_LENGTH];
    int scale, width, height;
    bool bold;
    size_t bytes;
    u64 used;
    SDL_Texture *texture;
} TextCacheEntry;

static TextCacheEntry text_cache[TEXT_CACHE_SIZE];
static TTF_Font *fonts[2][FONT_SCALE_MAX];
static void *bundled_font[2];
static size_t bundled_font_size[2];
static PlFontData shared_font;
static bool shared_font_ready, ttf_ready;
static size_t text_cache_bytes;
static u64 text_cache_clock;

void drawing_init(void) {
    if (ttf_ready) return;
    if (TTF_Init() != 0) return;
    ttf_ready = true;
    const char *paths[] = {"romfs:/fonts/Inter-Regular.otf", "romfs:/fonts/Inter-Bold.otf"};
    for (unsigned style = 0; style < 2; ++style) {
        bundled_font[style] = SDL_LoadFile(paths[style], &bundled_font_size[style]);
        if (bundled_font_size[style] > 8u * 1024u * 1024u) {
            SDL_free(bundled_font[style]);
            bundled_font[style] = NULL;
            bundled_font_size[style] = 0;
        }
    }
    if (bundled_font[0] && bundled_font[1]) return;
    Result result = plInitialize(PlServiceType_User);
    if (R_FAILED(result)) {
        SDL_Log("Shared font service unavailable: %08lX", (unsigned long)result);
        return;
    }
    shared_font_ready = true;
    result = plGetSharedFontByType(&shared_font, PlSharedFontType_Standard);
    if (R_FAILED(result)) {
        SDL_Log("Shared font unavailable: %08lX", (unsigned long)result);
        memset(&shared_font, 0, sizeof(shared_font));
    }
}

static void text_cache_free(TextCacheEntry *entry) {
    if (entry->texture) SDL_DestroyTexture(entry->texture);
    text_cache_bytes -= entry->bytes;
    memset(entry, 0, sizeof(*entry));
}

void drawing_shutdown(void) {
    for (unsigned i = 0; i < TEXT_CACHE_SIZE; ++i) text_cache_free(&text_cache[i]);
    for (unsigned style = 0; style < 2; ++style)
        for (unsigned i = 0; i < FONT_SCALE_MAX; ++i) {
            if (fonts[style][i]) TTF_CloseFont(fonts[style][i]);
            fonts[style][i] = NULL;
        }
    for (unsigned style = 0; style < 2; ++style) {
        SDL_free(bundled_font[style]);
        bundled_font[style] = NULL;
        bundled_font_size[style] = 0;
    }
    if (shared_font_ready) plExit();
    shared_font_ready = false;
    memset(&shared_font, 0, sizeof(shared_font));
    if (ttf_ready) TTF_Quit();
    ttf_ready = false;
    text_cache_clock = 0;
}

static TTF_Font *text_font(int scale, bool bold) {
    if (!ttf_ready || scale < 1 || scale > FONT_SCALE_MAX)
        return NULL;
    TTF_Font **font = &fonts[bold ? 1 : 0][scale - 1];
    if (!*font) {
        unsigned style = bold ? 1 : 0;
        bool native_bold = bold && bundled_font[1];
        const void *data = bundled_font[style];
        size_t size = bundled_font_size[style];
        if (!data && bundled_font[0]) {
            data = bundled_font[0];
            size = bundled_font_size[0];
        }
        if (!data) {
            data = shared_font.address;
            size = shared_font.size;
        }
        if (!data || !size) return NULL;
        SDL_RWops *source = SDL_RWFromConstMem(data, (int)size);
        if (source) *font = TTF_OpenFontRW(source, 1, scale == 1 ? 12 : scale * 8);
        if (*font) {
            TTF_SetFontStyle(*font, bold && !native_bold ? TTF_STYLE_BOLD : TTF_STYLE_NORMAL);
            TTF_SetFontHinting(*font, TTF_HINTING_LIGHT);
        }
    }
    return *font;
}

static TextCacheEntry *oldest_text(void) {
    TextCacheEntry *oldest = &text_cache[0];
    for (unsigned i = 0; i < TEXT_CACHE_SIZE; ++i) {
        if (!text_cache[i].texture) return &text_cache[i];
        if (text_cache[i].used < oldest->used) oldest = &text_cache[i];
    }
    return oldest;
}

static TextCacheEntry *cached_text(const char *text, int scale, bool bold) {
    TTF_Font *font = text_font(scale, bold);
    if (!font || !*text || strlen(text) >= TEXT_MAX_LENGTH) return NULL;
    for (unsigned i = 0; i < TEXT_CACHE_SIZE; ++i) {
        TextCacheEntry *entry = &text_cache[i];
        if (entry->texture && entry->scale == scale && entry->bold == bold &&
            strcmp(entry->text, text) == 0) {
            entry->used = ++text_cache_clock;
            return entry;
        }
    }
    int width, height;
    if (TTF_SizeUTF8(font, text, &width, &height) != 0 || width <= 0 || height <= 0 ||
        width > 4096 || height > 256) return NULL;
    size_t bytes = (size_t)width * (size_t)height * 4;
    if (bytes > TEXT_CACHE_BYTES) return NULL;
    while (text_cache_bytes + bytes > TEXT_CACHE_BYTES) {
        TextCacheEntry *victim = NULL;
        for (unsigned i = 0; i < TEXT_CACHE_SIZE; ++i)
            if (text_cache[i].texture && (!victim || text_cache[i].used < victim->used))
                victim = &text_cache[i];
        if (!victim) return NULL;
        text_cache_free(victim);
    }
    TextCacheEntry *entry = oldest_text();
    text_cache_free(entry);
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, white);
    if (!surface) return NULL;
    entry->texture = SDL_CreateTextureFromSurface(sdl_renderer_get(), surface);
    entry->width = surface->w;
    entry->height = surface->h;
    SDL_FreeSurface(surface);
    if (!entry->texture) return NULL;
    SDL_SetTextureBlendMode(entry->texture, SDL_BLENDMODE_BLEND);
    memcpy(entry->text, text, strlen(text) + 1);
    entry->scale = scale;
    entry->bold = bold;
    entry->bytes = bytes;
    entry->used = ++text_cache_clock;
    text_cache_bytes += bytes;
    return entry;
}

static bool modern_text(const char *text, int x, int y, int scale, u32 color, bool bold) {
    if (!platform_graphics_modern()) return false;
    if (color == 0xFF000000u) color = 0xFF121212u;
    else if (color == 0xFFFFFFFFu) color = 0xFFF8F8F8u;
    TextCacheEntry *entry = cached_text(text, scale, bold);
    if (!entry) return false;
    SDL_SetTextureColorMod(entry->texture, color & 255, (color >> 8) & 255, (color >> 16) & 255);
    SDL_SetTextureAlphaMod(entry->texture, (color >> 24) & 255);
    SDL_Rect destination = {x, y + (7 * scale - entry->height) / 2, entry->width, entry->height};
    SDL_RenderCopy(sdl_renderer_get(), entry->texture, NULL, &destination);
    return true;
}

static void drawing_set_color(u32 color) {
    if (platform_graphics_modern() && color == 0xFF000000u) color = 0xFF121212u;
    Uint8 a = (color >> 24) & 0xFF;
    Uint8 b = (color >> 16) & 0xFF;
    Uint8 g = (color >> 8) & 0xFF;
    Uint8 r = color & 0xFF;

    SDL_SetRenderDrawColor(sdl_renderer_get(), r, g, b, a);
}

void drawing_rect(int x, int y, int w, int h, u32 color) {
    SDL_Rect rect = { x, y, w, h };

    drawing_set_color(color);
    SDL_RenderFillRect(sdl_renderer_get(), &rect);
}

void drawing_rect_outline(int x, int y, int w, int h, int thickness, u32 color) {
    if (thickness <= 0)
        return;

    drawing_rect(x, y, w, thickness, color);
    drawing_rect(x, y + h - thickness, w, thickness, color);
    drawing_rect(x, y, thickness, h, color);
    drawing_rect(x + w - thickness, y, thickness, h, color);
}

static int rounded_inset(int row, int height, int radius) {
    int dy = row < radius ? radius - row - 1 : row >= height - radius ? row - (height - radius) : -1;
    if (dy < 0) return 0;
    return radius - (int)sqrtf((float)(radius * radius - dy * dy));
}

void drawing_round_rect(int x, int y, int w, int h, int radius, u32 color) {
    if (w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) { drawing_rect(x, y, w, h, color); return; }
    drawing_set_color(color);
    SDL_Rect center = {x, y + radius, w, h - 2 * radius};
    if (center.h > 0) SDL_RenderFillRect(sdl_renderer_get(), &center);
    for (int row = 0; row < radius; ++row) {
        int inset = rounded_inset(row, h, radius);
        SDL_RenderDrawLine(sdl_renderer_get(), x + inset, y + row, x + w - inset - 1, y + row);
        SDL_RenderDrawLine(sdl_renderer_get(), x + inset, y + h - row - 1, x + w - inset - 1, y + h - row - 1);
    }
}

void drawing_round_rect_outline(int x, int y, int w, int h, int radius, int thickness, u32 color) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 0) radius = 0;
    int inner_w = w - 2 * thickness, inner_h = h - 2 * thickness;
    int inner_radius = radius > thickness ? radius - thickness : 0;
    drawing_set_color(color);
    for (int row = 0; row < h; ++row) {
        int outer = rounded_inset(row, h, radius);
        int left = x + outer, right = x + w - outer - 1;
        if (row < thickness || row >= h - thickness || inner_w <= 0 || inner_h <= 0) {
            SDL_RenderDrawLine(sdl_renderer_get(), left, y + row, right, y + row);
        } else {
            int inner = rounded_inset(row - thickness, inner_h, inner_radius);
            SDL_RenderDrawLine(sdl_renderer_get(), left, y + row, x + thickness + inner - 1, y + row);
            SDL_RenderDrawLine(sdl_renderer_get(), x + w - thickness - inner, y + row, right, y + row);
        }
    }
}

void drawing_circle(int center_x, int center_y, int radius, u32 color) {
    if (radius <= 0)
        return;

    drawing_set_color(color);

    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                SDL_Rect pixel = { center_x + x, center_y + y, 1, 1 };
                SDL_RenderFillRect(sdl_renderer_get(), &pixel);
            }
        }
    }
}

void drawing_image(const Image *image, int x, int y) {
    if (!image || !image->texture)
        return;

    SDL_Rect dest = { x, y, image->width, image->height };
    SDL_RenderCopy(sdl_renderer_get(), image->texture, NULL, &dest);
}

void drawing_image_scaled(const Image *image, int x, int y, int w, int h) {
    if (!image || !image->texture)
        return;

    SDL_Rect dest = { x, y, w, h };
    SDL_RenderCopy(sdl_renderer_get(), image->texture, NULL, &dest);
}

static int decode_utf8_codepoint(const char **cursor) {
    const unsigned char *text = (const unsigned char*)*cursor;
    int codepoint = 0;

    if (text[0] < 0x80) {
        (*cursor)++;
        return text[0];
    }

    if ((text[0] & 0xE0) == 0xC0 && (text[1] & 0xC0) == 0x80) {
        codepoint = ((text[0] & 0x1F) << 6) | (text[1] & 0x3F);
        *cursor += 2;
        return codepoint;
    }

    if ((text[0] & 0xF0) == 0xE0 &&
        (text[1] & 0xC0) == 0x80 &&
        (text[2] & 0xC0) == 0x80) {
        codepoint = ((text[0] & 0x0F) << 12) |
                    ((text[1] & 0x3F) << 6) |
                    (text[2] & 0x3F);
        *cursor += 3;
        return codepoint;
    }

    if ((text[0] & 0xF8) == 0xF0 &&
        (text[1] & 0xC0) == 0x80 &&
        (text[2] & 0xC0) == 0x80 &&
        (text[3] & 0xC0) == 0x80) {
        codepoint = ((text[0] & 0x07) << 18) |
                    ((text[1] & 0x3F) << 12) |
                    ((text[2] & 0x3F) << 6) |
                    (text[3] & 0x3F);
        *cursor += 4;
        return codepoint;
    }

    (*cursor)++;
    return '?';
}

static char normalize_codepoint(int codepoint) {
    switch (codepoint) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
        case 0x00C4: case 0x00C5: case 0x00E0: case 0x00E1:
        case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        case 0x0100: case 0x0101: case 0x0102: case 0x0103:
        case 0x0104: case 0x0105:
            return 'A';

        case 0x00C7: case 0x00E7: case 0x0106: case 0x0107:
        case 0x0108: case 0x0109: case 0x010A: case 0x010B:
        case 0x010C: case 0x010D:
            return 'C';

        case 0x00D0: case 0x010E: case 0x010F: case 0x0110:
        case 0x0111:
            return 'D';

        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        case 0x0112: case 0x0113: case 0x0114: case 0x0115:
        case 0x0116: case 0x0117: case 0x0118: case 0x0119:
        case 0x011A: case 0x011B:
            return 'E';

        case 0x011C: case 0x011D: case 0x011E: case 0x011F:
        case 0x0120: case 0x0121: case 0x0122: case 0x0123:
            return 'G';

        case 0x0124: case 0x0125: case 0x0126: case 0x0127:
            return 'H';

        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
        case 0x0128: case 0x0129: case 0x012A: case 0x012B:
        case 0x012C: case 0x012D: case 0x012E: case 0x012F:
        case 0x0130: case 0x0131:
            return 'I';

        case 0x0134: case 0x0135:
            return 'J';

        case 0x0136: case 0x0137:
            return 'K';

        case 0x0139: case 0x013A: case 0x013B: case 0x013C:
        case 0x013D: case 0x013E: case 0x013F: case 0x0140:
        case 0x0141: case 0x0142:
            return 'L';

        case 0x00D1: case 0x00F1: case 0x0143: case 0x0144:
        case 0x0145: case 0x0146: case 0x0147: case 0x0148:
            return 'N';

        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
        case 0x00D6: case 0x00D8: case 0x00F2: case 0x00F3:
        case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
        case 0x014C: case 0x014D: case 0x014E: case 0x014F:
        case 0x0150: case 0x0151:
            return 'O';

        case 0x0154: case 0x0155: case 0x0156: case 0x0157:
        case 0x0158: case 0x0159:
            return 'R';

        case 0x015A: case 0x015B: case 0x015C: case 0x015D:
        case 0x015E: case 0x015F: case 0x0160: case 0x0161:
            return 'S';

        case 0x0162: case 0x0163: case 0x0164: case 0x0165:
        case 0x0166: case 0x0167:
            return 'T';

        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
        case 0x0168: case 0x0169: case 0x016A: case 0x016B:
        case 0x016C: case 0x016D: case 0x016E: case 0x016F:
        case 0x0170: case 0x0171: case 0x0172: case 0x0173:
            return 'U';

        case 0x0174: case 0x0175:
            return 'W';

        case 0x00DD: case 0x00FD: case 0x00FF: case 0x0176:
        case 0x0177: case 0x0178:
            return 'Y';

        case 0x0179: case 0x017A: case 0x017B: case 0x017C:
        case 0x017D: case 0x017E:
            return 'Z';

        case 0x00C6: case 0x00E6:
            return 'A';

        case 0x0152: case 0x0153:
            return 'O';

        case 0x00DE: case 0x00FE:
            return 'P';

        case 0x00DF:
            return 'S';

        case 0x2018: case 0x2019: case 0x201A: case 0x201B:
        case 0x2032: case 0x00B4:
            return '\'';

        case 0x201C: case 0x201D: case 0x201E: case 0x201F:
        case 0x2033:
            return '"';

        case 0x2010: case 0x2011: case 0x2012: case 0x2013:
        case 0x2014: case 0x2212:
            return '-';

        case 0x2026:
            return '.';

        case 0x00B0:
            return '\x01';

        case 0x2022:
            return '\x02';

        case 0x00A0:
            return ' ';

        default:
            if (codepoint >= 'a' && codepoint <= 'z')
                return (char)(codepoint - 'a' + 'A');

            if (codepoint >= 0 && codepoint <= 0x7F)
                return (char)codepoint;

            return '?';
    }
}

static const unsigned char *glyph_for_char(char c) {
    static const unsigned char unknown[7] = { 31, 17, 1, 2, 4, 0, 4 };
    static const unsigned char quote[7] = { 10, 10, 20, 0, 0, 0, 0 };
    static const unsigned char colon[7] = { 0, 4, 4, 0, 4, 4, 0 };
    static const unsigned char semicolon[7] = { 0, 4, 4, 0, 4, 4, 8 };
    static const unsigned char comma[7] = { 0, 0, 0, 0, 4, 4, 8 };
    static const unsigned char apostrophe[7] = { 4, 4, 8, 0, 0, 0, 0 };
    static const unsigned char hyphen[7] = { 0, 0, 0, 31, 0, 0, 0 };
    static const unsigned char period[7] = { 0, 0, 0, 0, 0, 12, 12 };
    static const unsigned char slash[7] = { 1, 2, 2, 4, 8, 8, 16 };
    static const unsigned char ampersand[7] = { 12, 18, 20, 8, 21, 18, 13 };
    static const unsigned char hash[7] = { 10, 10, 31, 10, 31, 10, 10 };
    static const unsigned char exclamation[7] = { 4, 4, 4, 4, 4, 0, 4 };
    static const unsigned char underscore[7] = { 0, 0, 0, 0, 0, 0, 31 };
    static const unsigned char plus[7] = { 0, 4, 4, 31, 4, 4, 0 };
    static const unsigned char left_paren[7] = { 2, 4, 8, 8, 8, 4, 2 };
    static const unsigned char right_paren[7] = { 8, 4, 2, 2, 2, 4, 8 };
    static const unsigned char left_bracket[7] = { 14, 8, 8, 8, 8, 8, 14 };
    static const unsigned char right_bracket[7] = { 14, 2, 2, 2, 2, 2, 14 };
    static const unsigned char degree[7] = { 14, 17, 17, 14, 0, 0, 0 };
    static const unsigned char bullet[7] = { 0, 0, 4, 14, 4, 0, 0 };
    static const unsigned char glyphs[][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, // A
        { 30, 17, 17, 30, 17, 17, 30 }, // B
        { 14, 17, 16, 16, 16, 17, 14 }, // C
        { 30, 17, 17, 17, 17, 17, 30 }, // D
        { 31, 16, 16, 30, 16, 16, 31 }, // E
        { 31, 16, 16, 30, 16, 16, 16 }, // F
        { 14, 17, 16, 23, 17, 17, 14 }, // G
        { 17, 17, 17, 31, 17, 17, 17 }, // H
        { 14, 4, 4, 4, 4, 4, 14 },      // I
        { 7, 2, 2, 2, 18, 18, 12 },     // J
        { 17, 18, 20, 24, 20, 18, 17 }, // K
        { 16, 16, 16, 16, 16, 16, 31 }, // L
        { 17, 27, 21, 21, 17, 17, 17 }, // M
        { 17, 25, 21, 19, 17, 17, 17 }, // N
        { 14, 17, 17, 17, 17, 17, 14 }, // O
        { 30, 17, 17, 30, 16, 16, 16 }, // P
        { 14, 17, 17, 17, 21, 18, 13 }, // Q
        { 30, 17, 17, 30, 20, 18, 17 }, // R
        { 15, 16, 16, 14, 1, 1, 30 },   // S
        { 31, 4, 4, 4, 4, 4, 4 },       // T
        { 17, 17, 17, 17, 17, 17, 14 }, // U
        { 17, 17, 17, 17, 17, 10, 4 },  // V
        { 17, 17, 17, 21, 21, 27, 17 }, // W
        { 17, 17, 10, 4, 10, 17, 17 },  // X
        { 17, 17, 10, 4, 4, 4, 4 },     // Y
        { 31, 1, 2, 4, 8, 16, 31 },     // Z
        { 14, 17, 19, 21, 25, 17, 14 }, // 0
        { 4, 12, 4, 4, 4, 4, 14 },      // 1
        { 14, 17, 1, 2, 4, 8, 31 },     // 2
        { 30, 1, 1, 14, 1, 1, 30 },     // 3
        { 2, 6, 10, 18, 31, 2, 2 },     // 4
        { 31, 16, 16, 30, 1, 1, 30 },   // 5
        { 14, 16, 16, 30, 17, 17, 14 }, // 6
        { 31, 1, 2, 4, 8, 8, 8 },       // 7
        { 14, 17, 17, 14, 17, 17, 14 }, // 8
        { 14, 17, 17, 15, 1, 1, 14 }    // 9
    };

    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';

    if (c >= 'A' && c <= 'Z')
        return glyphs[c - 'A'];

    if (c >= '0' && c <= '9')
        return glyphs[26 + c - '0'];

    if (c == ' ')
        return NULL;

    if (c == ':')
        return colon;

    if (c == ';')
        return semicolon;

    if (c == ',')
        return comma;

    if (c == '\'')
        return apostrophe;

    if (c == '"')
        return quote;

    if (c == '-')
        return hyphen;

    if (c == '.')
        return period;

    if (c == '/')
        return slash;

    if (c == '&')
        return ampersand;

    if (c == '#')
        return hash;

    if (c == '!')
        return exclamation;

    if (c == '?')
        return unknown;

    if (c == '_')
        return underscore;

    if (c == '+')
        return plus;

    if (c == '(')
        return left_paren;

    if (c == ')')
        return right_paren;

    if (c == '[')
        return left_bracket;

    if (c == ']')
        return right_bracket;

    if (c == '\x01')
        return degree;

    if (c == '\x02' || c == '*')
        return bullet;

    return unknown;
}

int drawing_text_width(const char *text, int scale) {
    int glyph_count = 0;
    const char *cursor = text;

    if (!text || scale <= 0)
        return 0;

    if (platform_graphics_modern()) {
        TextCacheEntry *entry = cached_text(text, scale, false);
        if (entry) return entry->width;
    }

    while (*cursor != '\0') {
        decode_utf8_codepoint(&cursor);
        glyph_count++;
    }

    return glyph_count * 6 * scale;
}

void drawing_text(const char *text, int x, int y, int scale, u32 color) {
    int glyph_index = 0;
    const char *cursor = text;

    if (!text || scale <= 0)
        return;

    if (modern_text(text, x, y, scale, color, false)) return;

    drawing_set_color(color);

    while (*cursor != '\0') {
        int codepoint = decode_utf8_codepoint(&cursor);
        char normalized = normalize_codepoint(codepoint);
        const unsigned char *glyph = glyph_for_char(normalized);

        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (glyph[row] & (1 << (4 - col))) {
                        SDL_Rect pixel = {
                            x + glyph_index * 6 * scale + col * scale,
                            y + row * scale,
                            scale,
                            scale
                        };
                        SDL_RenderFillRect(sdl_renderer_get(), &pixel);
                    }
                }
            }
        }

        glyph_index++;
    }
}

int drawing_text_height(int scale) {
    return scale > 0 ? 7 * scale : 0;
}

int drawing_text_bold_width(const char *text, int scale) {
    if (!text || scale <= 0) return 0;
    if (platform_graphics_modern()) {
        TextCacheEntry *entry = cached_text(text, scale, true);
        if (entry) return entry->width;
    }
    return drawing_text_width(text, scale);
}

void drawing_text_bold(const char *text, int x, int y, int scale, u32 color) {
    if (!text || scale <= 0) return;
    if (!modern_text(text, x, y, scale, color, true)) drawing_text(text, x, y, scale, color);
}
