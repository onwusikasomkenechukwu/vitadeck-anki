// draw.h -- neo-brutalist drawing primitives for Phase 8.
//
// Everything is sharp-cornered filled rectangles + freetype text. No 1px
// lines: borders are filled rects >= 3px. Every bordered element gets a hard
// (un-blurred) black shadow offset down-right -- the neo-brutalist signature.
//
// Colors are RGBA8 (the macro packs R in the low byte -> matches
// SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, verified in vita2d.h:14).

#pragma once

#include <vita2d.h>
#include "font.h"

namespace nb {

// ---- Palette (exact values from the Phase 8 design system). ----
constexpr unsigned int CREAM    = RGBA8(255, 253, 245, 255);
constexpr unsigned int DOTGRID  = RGBA8(217, 215, 208, 255);  // ~15% black on cream
constexpr unsigned int BLACK    = RGBA8(0,   0,   0,   255);
constexpr unsigned int WHITE    = RGBA8(255, 255, 255, 255);
constexpr unsigned int HOTRED   = RGBA8(255, 107, 107, 255);
constexpr unsigned int YELLOW   = RGBA8(255, 217, 61,  255);
constexpr unsigned int VIOLET   = RGBA8(196, 181, 253, 255);

constexpr int SCREEN_W = 960;
constexpr int SCREEN_H = 544;
constexpr int BORDER   = 3;    // minimum border thickness
constexpr int SHADOW   = 6;    // default hard-shadow offset

// ---- Uppercase an ASCII/Latin-1 string into `out` (UI labels are uppercase;
// card content is NOT passed through here). ----
inline void upper(const char* in, char* out, size_t n) {
    size_t w = 0;
    if (n == 0) return;
    for (size_t i = 0; in && in[i] && w + 1 < n; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        out[w++] = (char)c;
    }
    out[w] = '\0';
}

// ---- Filled rectangle. ----
inline void draw_rect(int x, int y, int w, int h, unsigned int color) {
    vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

// ---- Full-screen cream canvas + halftone dot grid (drawn once per frame as
// the base layer). 2px dots on a 20px lattice.
//
// NOTE: the dots are 2x2 filled RECTANGLES, not circles. The spec said "2px
// circles", but vita2d_draw_fill_circle builds a many-vertex triangle fan, and
// ~1344 of them per frame exhausts vita2d's internal vertex pool -> NULL vertex
// pointer -> hard fault (confirmed via coredump: crash PC in
// vita2d_draw_fill_circle). At 2px a square is visually identical to a circle,
// and draw_rectangle is a cheap 4-vertex quad like the rest of the UI. ----
inline void draw_dot_grid(unsigned int dot_color) {
    draw_rect(0, 0, SCREEN_W, SCREEN_H, CREAM);
    for (int y = 10; y < SCREEN_H; y += 20)
        for (int x = 10; x < SCREEN_W; x += 20)
            draw_rect(x - 1, y - 1, 2, 2, dot_color);
}

// ---- Border drawn as four filled rects (no 1px lines). ----
inline void draw_rect_outline(int x, int y, int w, int h,
                              unsigned int color, int t) {
    if (t < 1) t = 1;
    draw_rect(x,         y,         w, t, color);   // top
    draw_rect(x,         y + h - t, w, t, color);   // bottom
    draw_rect(x,         y,         t, h, color);   // left
    draw_rect(x + w - t, y,         t, h, color);   // right
}

// ---- Hard-shadow bordered element: black shadow first, then fill + border. ----
inline void draw_shadow_rect(int x, int y, int w, int h,
                             unsigned int fill, unsigned int border,
                             int offset, int thickness = BORDER) {
    draw_rect(x + offset, y + offset, w, h, BLACK);   // hard shadow (no blur)
    draw_rect(x, y, w, h, fill);                       // fill
    draw_rect_outline(x, y, w, h, border, thickness);  // border
}

// ---- Neo-brutalist "Triangle button" icon: an upward outline triangle drawn
// as horizontal scanline rectangles (a solid outer triangle with a smaller
// inner one cut out in the fill color).
//
// NOTE: built ONLY from draw_rect. An earlier version used vita2d_draw_array
// with the GXM triangle primitive, which GPU-faults (psp2core "GPUCRASH"):
// vita2d_draw_array does not rebind the color shader, so right after drawing
// text the textured glyph shader is still active and the 16-byte color vertices
// are misread by a shader expecting 20-byte textured vertices. ----
inline void draw_tri_fill(int x, int y, int size, unsigned int color) {
    for (int r = 1; r <= size; ++r) {                 // apex (r=1) down to base
        int half = r / 2; if (half < 1) half = 1;
        draw_rect(x + size / 2 - half, y + r - 1, half * 2, 1, color);
    }
}
inline void draw_triangle_icon(int x, int y, int size, unsigned int color,
                               unsigned int inner) {
    draw_tri_fill(x, y, size, color);                 // solid outer
    int t = 3, isize = size - 2 * t;                  // ~3px outline -> inner cutout
    if (isize > 1) draw_tri_fill(x + t, y + t + 1, isize, inner);
}

// ---- Text (cell-top positioning, matches Font::draw). ----
inline void draw_text(Font& f, const char* text, int x, int y,
                      int size, unsigned int color) {
    f.draw(text, x, y, size, color);
}
inline void draw_text_centered(Font& f, const char* text, int cx, int y,
                               int size, unsigned int color) {
    int w = f.measure(text, size);
    f.draw(text, cx - w / 2, y, size, color);
}

// ---- Scale an image to fit within max_w x max_h, preserving aspect ratio,
// centered in the region. (The spec wrote SceUID; loaded images are actually
// vita2d_texture* from vita2d_load_PNG/JPEG_file, so that's the type used.) ----
inline void draw_image_fit(vita2d_texture* tex, int x, int y,
                           int max_w, int max_h) {
    if (!tex) return;
    float iw = (float)vita2d_texture_get_width(tex);
    float ih = (float)vita2d_texture_get_height(tex);
    if (iw <= 0.0f || ih <= 0.0f) return;
    float sx = (float)max_w / iw;
    float sy = (float)max_h / ih;
    float s  = sx < sy ? sx : sy;
    float dw = iw * s, dh = ih * s;
    float dx = (float)x + ((float)max_w - dw) / 2.0f;
    float dy = (float)y + ((float)max_h - dh) / 2.0f;
    vita2d_draw_texture_scale(tex, dx, dy, s, s);
}

}  // namespace nb
