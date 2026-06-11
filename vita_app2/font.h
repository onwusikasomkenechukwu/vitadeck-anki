// font.h -- Freetype-backed text rendering for Phase 8 (neo-brutalist UI).
//
// Replaces the pgf font stack with Space Grotesk rendered through freetype.
//
// CACHE STRATEGY (reported per the phase spec): a flat array indexed by
// [size-slot][char 0..255].
//   * Up to FONT_MAX_SIZES distinct pixel sizes are registered on first use.
//   * Each (slot, char) glyph is rasterized exactly once -- FT_Load_Char with
//     FT_LOAD_RENDER -- into a small RGBA texture that is WHITE with per-pixel
//     alpha = coverage. The same cached glyph is then recolored at draw time
//     via vita2d_draw_texture_tint, so one cache entry serves every color.
//   * No LRU / eviction: the UI uses a tiny fixed set of sizes over the
//     ASCII+Latin-1 range, so the working set is small and fully resident
//     (~8 sizes x ~95 printable glyphs). A 256-entry LRU was the alternative
//     but eviction would only add churn here for no memory saving.
//
// API provenance: every freetype call below is VERIFIED against the installed
// headers (freetype 2.14.3 in this vitasdk), not inferred from training:
//   FT_Init_FreeType      freetype.h:2351
//   FT_New_Memory_Face    freetype.h:2614
//   FT_Set_Pixel_Sizes    freetype.h:3176
//   FT_Load_Char          freetype.h:3281   FT_LOAD_RENDER freetype.h:3503
//   FT_GlyphSlotRec.{advance,bitmap,bitmap_left,bitmap_top}  freetype.h:2275-2281
//   FT_Size_Metrics.{ascender,height}                        freetype.h:1982-1984
//   FT_Bitmap.{rows,width,pitch,buffer} are the standard freetype 2.x fields.

#pragma once

#include <vita2d.h>
#include <psp2/gxm.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

constexpr int FONT_MAX_SIZES = 12;

struct Glyph {
    vita2d_texture* tex;     // null = no pixels (e.g. space) or load failed
    short left, top;         // bitmap_left / bitmap_top
    short w, h;
    short advance;           // pen advance in pixels (advance.x >> 6)
    bool  loaded;
};

// Codepoints 0..255 index glyphs[] directly; rarer codepoints (Latin Extended,
// curly quotes, dashes, etc.) go in a small linear-probe overflow table.
constexpr int FONT_EXT_CAP = 192;

struct FontSize {
    int          px;         // pixel size for this slot (0 = unused slot)
    int          ascent;     // baseline offset from cell top (px)
    int          line_h;     // line height (px)
    Glyph        glyphs[256];
    Glyph        ext[FONT_EXT_CAP];   // glyphs for codepoints >= 256
    unsigned int ext_cp[FONT_EXT_CAP];
    int          ext_count;
};

// Decode one UTF-8 codepoint at *pp and advance *pp past it. Invalid/truncated
// sequences fall back to a single Latin-1 byte (so we never read past the NUL,
// which terminates any incomplete trailing sequence).
inline unsigned int utf8_next(const unsigned char** pp) {
    const unsigned char* p = *pp;
    unsigned int c = p[0];
    if (c < 0x80) { *pp = p + 1; return c; }
    if ((c >> 5) == 0x6 && (p[1] & 0xC0) == 0x80) {
        *pp = p + 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c >> 4) == 0xE && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp = p + 3;
        return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((c >> 3) == 0x1E && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *pp = p + 4;
        return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
               ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *pp = p + 1; return c;   // invalid lead byte -> Latin-1 fallback
}

struct Font {
    FT_Library     lib;
    FT_Face        face;
    unsigned char* data;     // file bytes kept alive for the FT_Face lifetime
    bool           ok;
    FontSize       sizes[FONT_MAX_SIZES];

    // Load a .ttf from `path` (e.g. "app0:assets/fonts/SpaceGrotesk-Bold.ttf").
    bool init(const char* path) {
        std::memset(this, 0, sizeof(*this));
        if (FT_Init_FreeType(&lib) != 0) return false;

        std::FILE* fp = std::fopen(path, "rb");
        if (!fp) return false;
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(fp); return false; }
        data = (unsigned char*)std::malloc((size_t)sz);
        if (!data) { std::fclose(fp); return false; }
        size_t rd = std::fread(data, 1, (size_t)sz, fp);
        std::fclose(fp);
        if (rd != (size_t)sz) return false;

        if (FT_New_Memory_Face(lib, data, (FT_Long)sz, 0, &face) != 0) return false;
        ok = true;
        return true;
    }

    FontSize* slot(int px) {
        for (int i = 0; i < FONT_MAX_SIZES; ++i)
            if (sizes[i].px == px) return &sizes[i];
        for (int i = 0; i < FONT_MAX_SIZES; ++i) {
            if (sizes[i].px != 0) continue;
            FontSize* s = &sizes[i];
            std::memset(s, 0, sizeof(*s));
            s->px = px;
            FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
            s->ascent = (int)(face->size->metrics.ascender >> 6);
            s->line_h = (int)(face->size->metrics.height   >> 6);
            return s;
        }
        return &sizes[0];  // size table exhausted: reuse slot 0 (UI uses < 12)
    }

    Glyph* glyph(FontSize* s, unsigned int cp) {
        Glyph* g = nullptr;
        if (cp < 256) {
            g = &s->glyphs[cp];
        } else {
            for (int i = 0; i < s->ext_count; ++i)
                if (s->ext_cp[i] == cp) { g = &s->ext[i]; break; }
            if (!g) {
                if (s->ext_count >= FONT_EXT_CAP)
                    g = &s->glyphs['?'];          // table full: fall back to '?'
                else {
                    g = &s->ext[s->ext_count];
                    s->ext_cp[s->ext_count] = cp;
                    ++s->ext_count;
                }
            }
        }
        if (g->loaded) return g;
        g->loaded = true;
        g->tex = nullptr;

        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)s->px);  // face is shared across slots
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) return g;

        FT_GlyphSlot sl = face->glyph;
        g->advance = (short)(sl->advance.x >> 6);
        g->left    = (short)sl->bitmap_left;
        g->top     = (short)sl->bitmap_top;
        int w = (int)sl->bitmap.width;
        int h = (int)sl->bitmap.rows;
        g->w = (short)w;
        g->h = (short)h;
        if (w <= 0 || h <= 0) return g;   // space etc.: advance only, no texture

        vita2d_texture* tex = vita2d_create_empty_texture_format(
            (unsigned)w, (unsigned)h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!tex) return g;
        unsigned int* dst = (unsigned int*)vita2d_texture_get_datap(tex);
        int stride = (int)vita2d_texture_get_stride(tex) / 4;
        const unsigned char* src = sl->bitmap.buffer;
        int pitch = sl->bitmap.pitch;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char cov = src[y * pitch + x];
                dst[y * stride + x] = RGBA8(255, 255, 255, cov);  // white + alpha
            }
        g->tex = tex;
        return g;
    }

    // Pixel width of `text` at size `px` (UTF-8 aware).
    int measure(const char* text, int px) {
        if (!ok || !text) return 0;
        FontSize* s = slot(px);
        int w = 0;
        const unsigned char* p = (const unsigned char*)text;
        while (*p) w += glyph(s, utf8_next(&p))->advance;
        return w;
    }

    // Draw `text` with its cell TOP at (x, y). Recolors cached glyphs via tint.
    // UTF-8 aware: accented Latin etc. render as their real glyphs.
    void draw(const char* text, int x, int y, int px, unsigned int color) {
        if (!ok || !text) return;
        FontSize* s = slot(px);
        int baseline = y + s->ascent;
        int pen = x;
        const unsigned char* p = (const unsigned char*)text;
        while (*p) {
            Glyph* g = glyph(s, utf8_next(&p));
            if (g->tex)
                vita2d_draw_texture_tint(g->tex, (float)(pen + g->left),
                                         (float)(baseline - g->top), color);
            pen += g->advance;
        }
    }

    int line_height(int px) { return slot(px)->line_h; }
};
