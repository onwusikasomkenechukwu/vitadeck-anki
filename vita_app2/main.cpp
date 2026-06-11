// Phase 7 (Part A) -- deck picker + per-deck reviewer.
//   - On launch, scan ux0:data/vitadeck/ for *.vitadeck folders, read each
//     manifest.json (deck_name, counts.cards, cloze_verified), and show a
//     picker. D-pad up/down moves the cursor, X launches the highlighted deck.
//   - One deck present -> picker is skipped, deck launches directly.
//   - No valid decks -> "No decks found" screen; any button rescans, START
//     exits.
//   - The Phase 5 reviewer is unchanged in behavior but now parameterized by
//     the selected deck folder (paths built at runtime, not hardcoded).
//   - Reviewer nav: START exits the app; SELECT returns to the picker.
//
// Phase 5 reviewer notes (retained):
//   - Word-wrapped front/back, vertical truncation marker.
//   - Card queue: ids loaded once, content fetched lazily by id.
//   - Cloze highlighting: [...] on the front and the revealed answer on
//     the back drawn in a contrast color.
//   - Grades appended to reviews.jsonl via a session-scoped FILE* kept
//     open across the whole session (fflush per write).
//
// API surfaces verified against installed SDK headers:
//   vita2d.h                       pgf draw/text_width; no _color variant exists
//   psp2/ctrl.h                    SCE_CTRL_* (UP/DOWN, CROSS/CIRCLE/SQUARE/
//                                  TRIANGLE, RTRIGGER, START, SELECT)
//   psp2/io/dirent.h               sceIoDopen/Dread/Dclose, SceIoDirent
//   psp2common/kernel/iofilemgr.h  SceIoStat.st_mode, SCE_S_ISDIR (via dirent.h)
//   psp2/sysmodule.h               SCE_SYSMODULE_SQLITE
//   psp2/sqlite.h                  SceSqliteMallocMethods + config
//   psp2/rtc.h                     sceRtcGetCurrentClockLocalTime + sceRtcGetTime_t
//   psp2/kernel/processmgr.h       sceKernelGetProcessTimeWide (uint64 us)
//   psp2/kernel/clib.h             sceClibPrintf
// sqlite3 prototypes in local sqlite3.h shim; ABI compat with SceSqlite
// system module is INFERRED, validated since Phase 3.
//
// Known newlib-nano quirk (proven in Phase 4): snprintf with %zu/%lld corrupts
// the vararg walk and crashes. We avoid %zu/%lld in snprintf entirely; integers
// destined for snprintf are pre-formatted with i64_to_dec and passed as %s.
// %s and %d in snprintf are fine (used since Phase 4). fprintf with %lld DOES
// work (reviews.jsonl proven), so the JSONL writer keeps it.

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>             // Phase 8: front touchscreen
#include <psp2/sysmodule.h>
#include <psp2/sqlite.h>
#include <psp2/rtc.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>   // sceKernelDelayThread (overlay pause)
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>           // sceIoRename/sceIoRemove (atomic save)

#include "sqlite3.h"
#include "font.h"                    // Phase 8: freetype Space Grotesk
#include "draw.h"                    // Phase 8: neo-brutalist primitives
#include "audio.h"                   // SFX mixer (scroll / select)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <vector>

// Vita default libc heap is small; SQLite + a 20k-id vector + buffers can
// exhaust it, and with -fno-exceptions bad_alloc becomes a silent abort.
unsigned int sceLibcHeapSize = 16 * 1024 * 1024;

namespace {

constexpr unsigned int BG_COLOR = RGBA8(0x10, 0x18, 0x28, 0xFF);
constexpr unsigned int WHITE    = RGBA8(0xFF, 0xFF, 0xFF, 0xFF);
constexpr unsigned int GREY     = RGBA8(0xA0, 0xA0, 0xA0, 0xFF);
constexpr unsigned int RED      = RGBA8(0xFF, 0x60, 0x60, 0xFF);
constexpr unsigned int AMBER    = RGBA8(0xFF, 0xC8, 0x40, 0xFF);
constexpr unsigned int GREEN    = RGBA8(0x80, 0xE0, 0x80, 0xFF);
constexpr unsigned int CYAN     = RGBA8(0x40, 0xE0, 0xFF, 0xFF);  // cloze / cursor

// All .vitadeck packages live here. Child paths are built at runtime.
constexpr const char* VITADECK_ROOT = "ux0:data/vitadeck";
constexpr int MAX_DECKS = 16;            // hard cap; no dynamic alloc for scan

// Image cards render a clean [IMAGE] placeholder instead of the decoded
// texture. Drawing loaded image textures under stress GPU-faults on this
// hardware, and every mitigation (small/aligned textures, near-1:1 scale,
// GPU-idle waits, never-free cache) still crashed -- a vita2d/GXM limit, not a
// fixable bug here. Placeholder keeps the app rock-stable; flip to false to
// re-attempt real image rendering.
constexpr bool VD_DIAG_NO_IMAGES = true;

// (Diagnostic confirmed the fault is in the texture DRAW, not load/decode. Fix:
// images are now re-encoded small + stride-aligned so vita2d_draw_texture_scale
// draws near-1:1 on a small texture.)
constexpr bool VD_DIAG_LOAD_ONLY = false;

// The .vitadeck format version this build understands (manifest.json
// "format_version"). See FORMAT.md "Format versioning contract". Increment the
// MAJOR (this integer) only when a change ALTERS or REMOVES an existing field,
// which makes older readers incompatible; additive-only changes keep this the
// same. A deck whose format_version exceeds this is refused at load time.
#define VITADECK_FORMAT_VERSION 1

constexpr int   SCREEN_W   = 960;
constexpr int   SCREEN_H   = 544;
constexpr int   BODY_LEFT  = 32;
constexpr int   BODY_RIGHT = 928;
constexpr int   BODY_TOP   = 110;
constexpr int   BODY_BOTTOM = SCREEN_H - 110;  // room for answer + footer + status
constexpr int   LINE_PX    = 32;
constexpr float TEXT_SCALE = 1.2f;
constexpr int   BODY_WIDTH = BODY_RIGHT - BODY_LEFT;

constexpr const char* CLOZE_MARK = "[...]";

// ---- Front touchscreen (Phase 8). Report coords are the panel's native
// 1920x1088; halve them to the 960x544 framebuffer. ----
struct TouchState { SceTouchData cur, prev; };

inline void touch_init(TouchState* t) {
    std::memset(t, 0, sizeof(*t));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t->cur, 1);   // prime: ignore held touch
}

// True + screen coords on a NEW finger-down this frame (tap edge-detect).
inline bool poll_tap(TouchState* t, int* tx, int* ty) {
    t->prev = t->cur;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t->cur, 1);
    if (t->cur.reportNum > 0 && t->prev.reportNum == 0) {
        *tx = (int)t->cur.report[0].x / 2;
        *ty = (int)t->cur.report[0].y / 2;
        return true;
    }
    return false;
}

inline bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// A single deck as shown in the picker.
struct DeckEntry {
    char folder[256];   // folder name, e.g. "eng_spa_clozes.vitadeck"
    char name[160];     // deck_name from manifest, or folder name on fallback
    int  card_count;    // -1 => unknown (render "?")
    bool cloze_verified;
    int  format_version; // manifest "format_version"; 0 => absent (treat as v1)
};

struct Card {
    sqlite3_int64 id;
    char          front[8192];
    char          back[8192];
    char          answer[512];  // extracted active-cloze answer (empty if none)
};

// Result of a reviewer session: how the user left it.
enum class RevResult { EXIT_APP, BACK_TO_PICKER };

// App-wide SFX, loaded in main(). scroll = list movement; select (utopia) =
// forward/continue/confirm; back (vinny) = back/no/cancel. Silent if absent.
audio::Sfx g_sfx_scroll, g_sfx_select, g_sfx_back;

// -----------------------------------------------------------------------
// int64 -> decimal string. Avoids the newlib-nano snprintf %lld/%zu path.
// -----------------------------------------------------------------------
void i64_to_dec(sqlite3_int64 v, char* out, size_t n) {
    if (n == 0) return;
    char tmp[24];
    int i = 0;
    unsigned long long u = (v < 0) ? (unsigned long long)(-v)
                                   : (unsigned long long)v;
    if (u == 0) tmp[i++] = '0';
    while (u > 0) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
    size_t w = 0;
    if (v < 0 && w < n - 1) out[w++] = '-';
    while (i > 0 && w < n - 1) out[w++] = tmp[--i];
    out[w] = '\0';
}

// -----------------------------------------------------------------------
// HTML strip + entity decode + [sound:] removal.
// -----------------------------------------------------------------------
void strip_html(const char* in, char* out, size_t out_size) {
    size_t w = 0;
    bool in_tag = false;
    for (size_t i = 0; in[i] != '\0' && w + 1 < out_size; ++i) {
        char c = in[i];
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        if (c == '<') { in_tag = true; continue; }

        // Drop [sound:...] audio markers (not real text).
        if (c == '[' && std::strncmp(in + i, "[sound:", 7) == 0) {
            const char* close = std::strchr(in + i, ']');
            if (close) { i = (size_t)(close - in); continue; }
        }

        if (c == '&') {
            if (std::strncmp(in + i, "&nbsp;", 6) == 0) { out[w++] = ' '; i += 5; continue; }
            if (std::strncmp(in + i, "&amp;",  5) == 0) { out[w++] = '&'; i += 4; continue; }
            if (std::strncmp(in + i, "&lt;",   4) == 0) { out[w++] = '<'; i += 3; continue; }
            if (std::strncmp(in + i, "&gt;",   4) == 0) { out[w++] = '>'; i += 3; continue; }
            if (std::strncmp(in + i, "&quot;", 6) == 0) { out[w++] = '"'; i += 5; continue; }
            if (std::strncmp(in + i, "&#39;",  5) == 0) { out[w++] = '\''; i += 4; continue; }
            // Numeric entity &#NNN; or &#xHH;
            if (in[i + 1] == '#') {
                int base = 10;
                size_t j = i + 2;
                if (in[j] == 'x' || in[j] == 'X') { base = 16; ++j; }
                long code = 0;
                size_t digits = 0;
                while (in[j] && in[j] != ';' && digits < 7) {
                    char d = in[j];
                    int val;
                    if (d >= '0' && d <= '9') val = d - '0';
                    else if (base == 16 && d >= 'a' && d <= 'f') val = 10 + d - 'a';
                    else if (base == 16 && d >= 'A' && d <= 'F') val = 10 + d - 'A';
                    else break;
                    code = code * base + val;
                    ++j; ++digits;
                }
                if (in[j] == ';' && digits > 0) {
                    if (code > 0 && code < 128) {
                        out[w++] = (char)code;     // ASCII
                    } else if (code >= 128 && code < 256) {
                        out[w++] = (char)code;     // Latin-1 byte, best effort
                    }
                    // codes >=256 dropped (would need UTF-8 encoding)
                    i = j;
                    continue;
                }
            }
        }
        out[w++] = c;
    }
    out[w] = '\0';
}

// -----------------------------------------------------------------------
// Extract the active cloze answer by diffing stripped front vs back.
// -----------------------------------------------------------------------
void extract_cloze_answer(const char* front, const char* back,
                          char* out, size_t n) {
    out[0] = '\0';
    if (std::strstr(front, CLOZE_MARK) == nullptr) return;  // not a cloze front
    size_t fp = std::strlen(front);
    size_t bp = std::strlen(back);
    size_t p = 0;
    while (p < fp && p < bp && front[p] == back[p]) ++p;
    size_t s = 0;
    while (s < (fp - p) && s < (bp - p) &&
           front[fp - 1 - s] == back[bp - 1 - s]) ++s;
    if (bp <= p + s) return;
    size_t start = p, end = bp - s;
    while (start < end && (back[start] == ' ' || back[start] == '\n' ||
                           back[start] == '\t')) ++start;
    while (end > start && (back[end - 1] == ' ' || back[end - 1] == '\n' ||
                           back[end - 1] == '\t')) --end;
    size_t len = end - start;
    if (len > n - 1) len = n - 1;
    std::memcpy(out, back + start, len);
    out[len] = '\0';
}

// Trim trailing/leading ASCII punctuation from a word for matching.
void trim_word(const char* in, char* out, size_t n) {
    size_t a = 0, b = std::strlen(in);
    auto is_punct = [](char c) {
        return c=='.'||c==','||c==';'||c==':'||c=='!'||c=='?'||
               c=='"'||c=='\''||c=='('||c==')'||c=='['||c==']';
    };
    while (a < b && is_punct(in[a])) ++a;
    while (b > a && is_punct(in[b - 1])) --b;
    size_t len = b - a;
    if (len > n - 1) len = n - 1;
    std::memcpy(out, in + a, len);
    out[len] = '\0';
}

// -----------------------------------------------------------------------
// Freetype word wrap with per-atom highlight (Phase 8). Same algorithm as the
// old pgf version, but measures/draws via Font. `y_start` is the cell top.
// Returns true if truncated at the bottom margin.
// -----------------------------------------------------------------------
// out_end_y (optional) receives the cursor y when it returns; measure_only
// computes layout without drawing (used to size the [PRE] box before drawing it).
bool draw_wrapped_ft(Font& f, const char* text,
                     int x_start, int y_start, int width, int y_max,
                     int size, unsigned int base_color,
                     const char* needle, bool contains, unsigned int hi_color,
                     int* out_end_y = nullptr, bool measure_only = false) {
    int line_height = f.line_height(size);
    if (line_height <= 0) line_height = size + 6;
    int x = x_start;
    int y = y_start;
    bool have_needle = needle && needle[0];
    int space_w = f.measure(" ", size);

    const char* p = text;
    while (*p) {
        if (*p == '\n') { x = x_start; y += line_height; if (y > y_max) { if (out_end_y) *out_end_y = y; return true; } ++p; continue; }
        if (*p == ' ' || *p == '\t') {
            while (*p == ' ' || *p == '\t') ++p;
            if (x != x_start) x += space_w;
            continue;
        }
        const char* a = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
        size_t alen = (size_t)(p - a);
        char atom[1024];
        if (alen > sizeof(atom) - 1) alen = sizeof(atom) - 1;
        std::memcpy(atom, a, alen);
        atom[alen] = '\0';
        int aw = f.measure(atom, size);

        if (x + aw > x_start + width && x != x_start) {
            x = x_start; y += line_height;
            if (y > y_max) { if (out_end_y) *out_end_y = y; return true; }
        }

        if (aw > width) {  // atom wider than a line: hard break by characters
            size_t off = 0;
            while (off < alen) {
                size_t take = 1, last_good = 1;
                while (off + take <= alen) {
                    char pre[1024];
                    std::memcpy(pre, atom + off, take);
                    pre[take] = '\0';
                    if (f.measure(pre, size) > width) break;
                    last_good = take; ++take;
                }
                char pre[1024];
                std::memcpy(pre, atom + off, last_good);
                pre[last_good] = '\0';
                if (!measure_only) f.draw(pre, x, y, size, base_color);
                off += last_good;
                if (off < alen) {
                    x = x_start; y += line_height;
                    if (y > y_max) { if (out_end_y) *out_end_y = y; return true; }
                } else {
                    x += f.measure(pre, size);
                }
            }
            continue;
        }

        unsigned int col = base_color;
        if (have_needle) {
            if (contains) {
                if (std::strstr(atom, needle)) col = hi_color;
            } else {
                char tw[1024];
                trim_word(atom, tw, sizeof(tw));
                if (tw[0] && std::strcmp(tw, needle) == 0) col = hi_color;
            }
        }
        if (!measure_only) f.draw(atom, x, y, size, col);
        x += aw;
    }
    if (out_end_y) *out_end_y = y;
    return false;
}

// -----------------------------------------------------------------------
// Card body renderer (Patch 2): like draw_wrapped_ft, but renders any
// [PRE]...[/PRE] segments as a bordered 18px box (inset 8px from the body
// region). Normal text uses `normal` at `size`; the box uses `pre_font` at 18.
// Returns true if truncated at y_max.
// -----------------------------------------------------------------------
// out_end_y (optional) receives the cursor y; measure_only computes the layout
// height without drawing (used to clamp the left-stick scroll).
bool draw_body(Font& normal, Font& pre_font, const char* text,
               int x, int y, int width, int y_max, int size,
               const char* needle, bool contains, unsigned int hi,
               int* out_end_y = nullptr, bool measure_only = false) {
    using namespace nb;
    const char* p = text;
    int cy = y;
    int normal_lh = normal.line_height(size);
    if (normal_lh <= 0) normal_lh = size + 6;
    while (*p) {
        const char* pre = std::strstr(p, "[PRE]");
        const char* seg_end = pre ? pre : (p + std::strlen(p));
        if (seg_end > p) {                          // normal run before the box
            // static: rendering is single-threaded and one call at a time, so
            // these big buffers must NOT sit on the stack -- run_reviewer's frame
            // plus freetype's rasterizer pool already run the main-thread stack
            // close to the edge, and two 8K stack buffers here overflow it
            // (crash lands inside freetype's gray_raster_render).
            static char seg[8192];
            size_t n = (size_t)(seg_end - p);
            if (n > sizeof(seg) - 1) n = sizeof(seg) - 1;
            std::memcpy(seg, p, n); seg[n] = '\0';
            int ey = cy;
            bool tr = draw_wrapped_ft(normal, seg, x, cy, width, y_max, size,
                                      BLACK, needle, contains, hi, &ey, measure_only);
            // Advance below the last text line before a [PRE] box, so a mid-line
            // trailing run (e.g. "...sync[PRE]") can't overlap the box top.
            cy = pre ? (ey + normal_lh) : ey;
            if (tr) { if (out_end_y) *out_end_y = cy; return true; }
        }
        if (!pre) break;
        const char* body = pre + 5;                 // after "[PRE]"
        const char* close = std::strstr(body, "[/PRE]");
        static char buf[8192];                      // static: keep off the stack
        size_t n = close ? (size_t)(close - body) : std::strlen(body);
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        std::memcpy(buf, body, n); buf[n] = '\0';
        const char* after = close ? close + 6 : body + std::strlen(body);
        // Trim leading/trailing blank lines + trailing spaces so the box hugs
        // its content (no empty first/last line inside the border).
        char* bt = buf;
        while (*bt == '\n' || *bt == '\r') ++bt;
        size_t bl = std::strlen(bt);
        while (bl > 0 && (bt[bl-1]=='\n' || bt[bl-1]=='\r' ||
                          bt[bl-1]==' '  || bt[bl-1]=='\t'))
            bt[--bl] = '\0';

        const int pad = 12;
        int box_x = x + 8, box_w = width - 16;       // inset 8px from body region
        int tx = box_x + 14, tw = box_w - 28;
        cy += 8;                                     // gap before the box
        int meas = cy + pad;
        draw_wrapped_ft(pre_font, bt, tx, cy + pad, tw, 1 << 20, 18,
                        BLACK, nullptr, false, 0, &meas, /*measure_only=*/true);
        // meas is the TOP of the last line; include a full line height so the
        // bottom border sits below the last line, not through it.
        int lh18 = pre_font.line_height(18); if (lh18 <= 0) lh18 = 24;
        int box_h = (meas - cy) + lh18 + pad;        // top pad + content + bottom pad
        if (!measure_only) {
            draw_rect(box_x, cy, box_w, box_h, WHITE);
            draw_rect_outline(box_x, cy, box_w, box_h, BLACK, BORDER);
            int ey = cy + pad;
            draw_wrapped_ft(pre_font, bt, tx, cy + pad, tw, y_max, 18,
                            BLACK, nullptr, false, 0, &ey);
        }
        cy = cy + box_h + 8;                          // gap after the box
        if (cy > y_max) { if (out_end_y) *out_end_y = cy; return true; }
        p = after;
    }
    if (out_end_y) *out_end_y = cy;
    return false;
}

// -----------------------------------------------------------------------
// Tiny manifest.json readers. The exporter emits flat, predictable JSON, so
// a key-substring scan is sufficient (and avoids pulling in a JSON library,
// per the "no new dependencies" constraint). Not a general JSON parser.
// -----------------------------------------------------------------------
int read_file_to_buf(const char* path, char* buf, size_t n) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    size_t r = std::fread(buf, 1, n - 1, f);
    std::fclose(f);
    buf[r] = '\0';
    return (int)r;
}

// Find `"key"`, then the following ':'; returns pointer just past ':' (value
// start, whitespace not skipped), or nullptr.
const char* json_value_after_key(const char* json, const char* quoted_key) {
    const char* p = std::strstr(json, quoted_key);
    if (!p) return nullptr;
    p += std::strlen(quoted_key);
    const char* colon = std::strchr(p, ':');
    if (!colon) return nullptr;
    return colon + 1;
}

bool json_find_string(const char* json, const char* quoted_key,
                      char* out, size_t n) {
    const char* v = json_value_after_key(json, quoted_key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') ++v;
    if (*v != '"') return false;
    ++v;
    size_t w = 0;
    while (*v && *v != '"' && w + 1 < n) {
        if (*v == '\\' && v[1]) ++v;  // skip simple escape
        out[w++] = *v++;
    }
    out[w] = '\0';
    return true;
}

bool json_find_int(const char* json, const char* quoted_key, int* out) {
    const char* v = json_value_after_key(json, quoted_key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') ++v;
    bool neg = false;
    if (*v == '-') { neg = true; ++v; }
    if (*v < '0' || *v > '9') return false;
    long val = 0;
    while (*v >= '0' && *v <= '9') { val = val * 10 + (*v - '0'); ++v; }
    *out = (int)(neg ? -val : val);
    return true;
}

bool json_find_bool(const char* json, const char* quoted_key, bool* out) {
    const char* v = json_value_after_key(json, quoted_key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') ++v;
    if (std::strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
    if (std::strncmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

bool ends_with(const char* s, const char* suf) {
    size_t ls = std::strlen(s), lf = std::strlen(suf);
    if (lf > ls) return false;
    return std::strcmp(s + (ls - lf), suf) == 0;
}

// -----------------------------------------------------------------------
// Scan VITADECK_ROOT for *.vitadeck folders. Fills up to `max` entries; sets
// *overflow if more than `max` exist. Returns the number filled. No dynamic
// allocation: caller passes a fixed DeckEntry[max].
// -----------------------------------------------------------------------
int scan_decks(DeckEntry* decks, int max, bool* overflow) {
    *overflow = false;
    SceUID dfd = sceIoDopen(VITADECK_ROOT);
    if (dfd < 0) return 0;  // dir missing/unreadable -> "no decks"

    int count = 0;
    SceIoDirent ent;
    while (true) {
        std::memset(&ent, 0, sizeof(ent));
        int rc = sceIoDread(dfd, &ent);
        if (rc <= 0) break;  // 0 = done, <0 = error
        if (!SCE_S_ISDIR(ent.d_stat.st_mode)) continue;
        if (!ends_with(ent.d_name, ".vitadeck")) continue;

        if (count >= max) { *overflow = true; continue; }

        DeckEntry* d = &decks[count];
        std::memset(d, 0, sizeof(*d));
        std::snprintf(d->folder, sizeof(d->folder), "%s", ent.d_name);
        d->card_count = -1;
        d->cloze_verified = false;
        d->format_version = 0;          // 0 = absent until parsed below

        // Read this deck's manifest.json (best effort).
        char manifest_path[512];
        std::snprintf(manifest_path, sizeof(manifest_path),
                      "%s/%s/manifest.json", VITADECK_ROOT, ent.d_name);
        char mbuf[4096];
        bool parsed_name = false;
        if (read_file_to_buf(manifest_path, mbuf, sizeof(mbuf)) > 0) {
            if (json_find_string(mbuf, "\"deck_name\"", d->name, sizeof(d->name))
                && d->name[0]) {
                parsed_name = true;
            }
            json_find_int(mbuf, "\"cards\"", &d->card_count);
            json_find_bool(mbuf, "\"cloze_verified\"", &d->cloze_verified);
            json_find_int(mbuf, "\"format_version\"", &d->format_version);
        }
        // Fallback: missing/malformed manifest -> folder name, "?" count.
        if (!parsed_name)
            std::snprintf(d->name, sizeof(d->name), "%s", ent.d_name);

        ++count;
    }
    sceIoDclose(dfd);
    return count;
}

// =======================================================================
// Gamification (Phase 7 Part B). All state is cosmetic and lives in
// player.json; it NEVER gates which cards are shown or when. On any read/IO
// failure we fall back to defaults silently and keep going.
// =======================================================================

constexpr const char* PLAYER_PATH = "ux0:data/vitadeck/player.json";
constexpr const char* PLAYER_TMP  = "ux0:data/vitadeck/player_tmp.json";

// The system pgf font has no emoji glyphs, so the flame (U+1F525) renders as
// a missing-glyph box on hardware. Per the spec we ship the ASCII fallback.
// To trial the emoji on real hardware, set this to "\xF0\x9F\x94\xA5".
constexpr const char* STREAK_PREFIX = "*";

// Achievement codes (stable; written verbatim into player.json's array).
constexpr const char* ACH_NAMES[5] = {
    "FIRST_REVIEW", "STREAK_7", "STREAK_30", "REVIEWS_100", "EASY_STREAK"
};

struct Player {
    int  xp;
    int  level;
    int  streak_days;
    char last_study_date[24];        // "YYYY-MM-DD", "" if never studied
    int  streak_freeze_tokens;
    int  total_reviews;
    bool ach[5];                     // indexed by ACH_NAMES
    // Daily missions (additive state kept in the same file).
    char missions_date[24];          // day the mission counters belong to
    int  m_reviewed;                 // "review 20 cards"
    bool m_easy;                     // "grade any card Easy"
    bool m_session_done;             // "complete a session without quitting"
};

void player_defaults(Player* p) {
    std::memset(p, 0, sizeof(*p));
    p->level = 1;
}

void player_load(Player* p) {
    player_defaults(p);
    char buf[2048];
    if (read_file_to_buf(PLAYER_PATH, buf, sizeof(buf)) <= 0) return;  // defaults
    json_find_int(buf, "\"xp\"", &p->xp);
    json_find_int(buf, "\"level\"", &p->level);
    if (p->level < 1) p->level = 1;
    json_find_int(buf, "\"streak_days\"", &p->streak_days);
    json_find_string(buf, "\"last_study_date\"", p->last_study_date,
                     sizeof(p->last_study_date));
    json_find_int(buf, "\"streak_freeze_tokens\"", &p->streak_freeze_tokens);
    json_find_int(buf, "\"total_reviews\"", &p->total_reviews);
    // achievements: scan the array region for each code as a substring.
    const char* arr = std::strstr(buf, "\"achievements\"");
    if (arr) {
        const char* end = std::strchr(arr, ']');
        for (int i = 0; i < 5; ++i) {
            const char* hit = std::strstr(arr, ACH_NAMES[i]);
            if (hit && (!end || hit < end)) p->ach[i] = true;
        }
    }
    json_find_string(buf, "\"missions_date\"", p->missions_date,
                     sizeof(p->missions_date));
    json_find_int(buf, "\"m_reviewed\"", &p->m_reviewed);
    json_find_bool(buf, "\"m_easy\"", &p->m_easy);
    json_find_bool(buf, "\"m_session_done\"", &p->m_session_done);
}

// Atomic-ish save: write the temp file fully, then rename over the target.
// fprintf %d/%s is safe (the newlib snprintf %lld/%zu quirk does not apply to
// fprintf -- proven by the reviews.jsonl writer).
void player_save(const Player* p) {
    std::FILE* f = std::fopen(PLAYER_TMP, "w");
    if (!f) return;  // never block gameplay on a write failure
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"xp\": %d,\n", p->xp);
    std::fprintf(f, "  \"level\": %d,\n", p->level);
    std::fprintf(f, "  \"streak_days\": %d,\n", p->streak_days);
    std::fprintf(f, "  \"last_study_date\": \"%s\",\n", p->last_study_date);
    std::fprintf(f, "  \"streak_freeze_tokens\": %d,\n", p->streak_freeze_tokens);
    std::fprintf(f, "  \"total_reviews\": %d,\n", p->total_reviews);
    std::fprintf(f, "  \"achievements\": [");
    bool first = true;
    for (int i = 0; i < 5; ++i) {
        if (!p->ach[i]) continue;
        std::fprintf(f, "%s\"%s\"", first ? "" : ", ", ACH_NAMES[i]);
        first = false;
    }
    std::fprintf(f, "],\n");
    std::fprintf(f, "  \"missions_date\": \"%s\",\n", p->missions_date);
    std::fprintf(f, "  \"m_reviewed\": %d,\n", p->m_reviewed);
    std::fprintf(f, "  \"m_easy\": %s,\n", p->m_easy ? "true" : "false");
    std::fprintf(f, "  \"m_session_done\": %s\n", p->m_session_done ? "true" : "false");
    std::fprintf(f, "}\n");
    std::fflush(f);
    std::fclose(f);
    // Rename over the target. On the first ever save the target is absent and
    // the rename succeeds outright; later it exists, so remove + rename.
    if (sceIoRename(PLAYER_TMP, PLAYER_PATH) < 0) {
        sceIoRemove(PLAYER_PATH);
        sceIoRename(PLAYER_TMP, PLAYER_PATH);
    }
}

// Today's local date as "YYYY-MM-DD".
void today_str(char* out, size_t n) {
    SceDateTime dt{};
    sceRtcGetCurrentClockLocalTime(&dt);
    std::snprintf(out, n, "%04d-%02d-%02d",
                  (int)dt.year, (int)dt.month, (int)dt.day);
}

// Serial day number for a "YYYY-MM-DD" string (days_from_civil, Hinnant).
// Difference of two serials is the exact day gap across month/year edges.
long date_serial(const char* s) {
    if (!s || std::strlen(s) < 10) return 0;
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int m = (s[5]-'0')*10 + (s[6]-'0');
    int d = (s[8]-'0')*10 + (s[9]-'0');
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Cumulative XP required to BE at level L. 1->2 costs 500, each next +200:
// reach(L) = 500*(L-1) + 100*(L-1)*(L-2).  reach(2)=500, reach(3)=1200, ...
long xp_to_reach_level(int L) {
    if (L <= 1) return 0;
    long n = L - 1;
    return 500 * n + 100 * n * (n - 1);
}

// Full-screen 2-second overlay (neo-brutalist card). Uses sceKernelDelayThread
// (a real sleep, not a busy-wait) per the spec.
void show_overlay(Font& bold, const char* line1, const char* line2) {
    using namespace nb;
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_dot_grid(DOTGRID);
    int w = 660, h = 200, x = (SCREEN_W - w) / 2, y = 170;
    draw_shadow_rect(x, y, w, h, HOTRED, BLACK, SHADOW);
    draw_text_centered(bold, line1, SCREEN_W / 2, y + 50, 30, WHITE);
    if (line2 && line2[0])
        draw_text_centered(bold, line2, SCREEN_W / 2, y + 110, 22, WHITE);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    sceKernelDelayThread(2u * 1000u * 1000u);  // 2 seconds
}

// 64-bit JSON integer reader (json_find_int is 32-bit; card ids overflow it).
bool json_find_i64(const char* json, const char* quoted_key, sqlite3_int64* out) {
    const char* v = json_value_after_key(json, quoted_key);
    if (!v) return false;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') ++v;
    bool neg = false;
    if (*v == '-') { neg = true; ++v; }
    if (*v < '0' || *v > '9') return false;
    sqlite3_int64 val = 0;
    while (*v >= '0' && *v <= '9') { val = val * 10 + (*v - '0'); ++v; }
    *out = neg ? -val : val;
    return true;
}

// Extract the filename from the first <img src="..."> in `html` (raw HTML,
// before strip_html removes the tag). Returns false if none.
bool parse_img_src(const char* html, char* out, size_t n) {
    const char* p = std::strstr(html, "<img");
    if (!p) return false;
    const char* s = std::strstr(p, "src=");
    if (!s) return false;
    s += 4;
    if (*s == '"' || *s == '\'') ++s;
    size_t w = 0;
    while (*s && *s != '"' && *s != '\'' && *s != '>' && w + 1 < n)
        out[w++] = *s++;
    out[w] = '\0';
    return w > 0;
}

// Case-insensitive suffix test (for image extensions).
bool ends_with_ci(const char* s, const char* suf) {
    size_t ls = std::strlen(s), lf = std::strlen(suf);
    if (lf > ls) return false;
    const char* a = s + (ls - lf);
    for (size_t i = 0; i < lf; ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return false;
    }
    return true;
}

// ---- Image texture LRU. Keyed by filename; evicts least-recently used. A
// "tried but missing/failed" load is cached as a null tex so we don't re-attempt
// every frame; the caller draws an [IMAGE] placeholder for null.
//
// Image textures are loaded once and NEVER freed during a session (only at
// teardown via freeall). Glyph textures use exactly this pattern and are
// stress-proof; the crash appeared only when images were freed+realloc'd on
// cache eviction while being drawn. So we never evict-free: once all CAP slots
// hold a texture, further new images show the [IMAGE] placeholder until the
// session ends. CAP bounds how many distinct images one session can show. ----
struct ImgCache {
    static const int CAP = 24;
    struct E { char name[128]; vita2d_texture* tex; bool used; unsigned long long t; };
    E e[CAP];
    unsigned long long clock;

    void init() { std::memset(e, 0, sizeof(e)); clock = 0; }

    vita2d_texture* get(const char* media_dir, const char* name) {
        if (!name || !name[0]) return nullptr;
        ++clock;
        for (int i = 0; i < CAP; ++i)
            if (e[i].used && std::strcmp(e[i].name, name) == 0) {
                e[i].t = clock;
                return e[i].tex;
            }
        // Cache miss. Use a FREE slot only -- never evict/free a used image
        // texture mid-session (that's the crash). When all slots are full, the
        // caller shows the [IMAGE] placeholder.
        int slot = -1;
        for (int i = 0; i < CAP; ++i) if (!e[i].used) { slot = i; break; }
        if (slot < 0) return nullptr;             // cache full -> placeholder

        // Allocate with the GPU idle (we're between frames; the last frame may
        // still be rendering, and allocating under it races the GPU).
        vita2d_wait_rendering_done();
        char path[320];
        std::snprintf(path, sizeof(path), "%s/%s", media_dir, name);  // %s only
        vita2d_texture* t = nullptr;
        if (ends_with_ci(name, ".png"))
            t = vita2d_load_PNG_file(path);
        else if (ends_with_ci(name, ".jpg") || ends_with_ci(name, ".jpeg"))
            t = vita2d_load_JPEG_file(path);
        // Reject corrupt/oversized decodes (would GPU-fault on draw). Don't
        // consume a cache slot for a failure -> placeholder, retry on re-view.
        if (t) {
            unsigned int w = vita2d_texture_get_width(t);
            unsigned int h = vita2d_texture_get_height(t);
            if (w == 0 || h == 0 || w > 4096 || h > 4096) {
                vita2d_free_texture(t);
                t = nullptr;
            }
        }
        if (!t) return nullptr;
        std::snprintf(e[slot].name, sizeof(e[slot].name), "%s", name);
        e[slot].tex = t;
        e[slot].used = true;
        e[slot].t = clock;
        return t;
    }

    void freeall() {
        for (int i = 0; i < CAP; ++i)
            if (e[i].used && e[i].tex) vita2d_free_texture(e[i].tex);
        init();
    }
};

// ---- progress.json (session resume). Atomic write like player.json. ----
bool progress_read(const char* path, sqlite3_int64* card_id, bool* seen_back,
                   char* deck_out, size_t dn) {
    char buf[512];
    if (read_file_to_buf(path, buf, sizeof(buf)) <= 0) return false;
    if (!json_find_i64(buf, "\"last_card_id\"", card_id)) return false;
    *seen_back = false;
    json_find_bool(buf, "\"last_seen_back\"", seen_back);
    deck_out[0] = '\0';
    json_find_string(buf, "\"deck_name\"", deck_out, dn);
    return true;
}

void progress_write(const char* tmp, const char* path, sqlite3_int64 card_id,
                    bool seen_back, const char* deck_name,
                    sqlite3_int64 saved_at) {
    std::FILE* f = std::fopen(tmp, "w");
    if (!f) return;
    std::fprintf(f,
        "{\n  \"last_card_id\": %lld,\n  \"last_seen_back\": %s,\n"
        "  \"deck_name\": \"%s\",\n  \"saved_at\": %lld\n}\n",
        (long long)card_id, seen_back ? "true" : "false",
        deck_name, (long long)saved_at);
    std::fflush(f);
    std::fclose(f);
    if (sceIoRename(tmp, path) < 0) { sceIoRemove(path); sceIoRename(tmp, path); }
}

// -----------------------------------------------------------------------
// SceSqlite allocator adapter (Phase 3 fix retained).
// -----------------------------------------------------------------------
extern "C" {
    static void* sql_malloc(int n)           { return std::malloc((size_t)n); }
    static void* sql_realloc(void* p, int n)  { return std::realloc(p, (size_t)n); }
    static void  sql_free(void* p)            { std::free(p); }
}

bool load_card_ids(sqlite3* db, std::vector<sqlite3_int64>* out,
                   char* err_buf, size_t err_size) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "SELECT id FROM cards ORDER BY id",
                                -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::snprintf(err_buf, err_size, "id query prepare rc=%d: %s",
                      rc, sqlite3_errmsg(db));
        return false;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out->push_back(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return true;
}

bool load_card_by_id(sqlite3_stmt* card_stmt, sqlite3_int64 id, Card* out) {
    sqlite3_reset(card_stmt);
    sqlite3_bind_int64(card_stmt, 1, id);
    if (sqlite3_step(card_stmt) != SQLITE_ROW) return false;
    out->id = id;
    const unsigned char* f = sqlite3_column_text(card_stmt, 0);
    const unsigned char* b = sqlite3_column_text(card_stmt, 1);
    std::snprintf(out->front, sizeof(out->front), "%s",
                  f ? reinterpret_cast<const char*>(f) : "");
    std::snprintf(out->back, sizeof(out->back), "%s",
                  b ? reinterpret_cast<const char*>(b) : "");
    return true;
}

sqlite3_int64 wall_clock_epoch_seconds() {
    SceDateTime now{};
    sceRtcGetCurrentClockLocalTime(&now);
    time_t t = 0;
    sceRtcGetTime_t(&now, &t);
    return (sqlite3_int64)t;
}

inline SceUInt64 now_us() { return sceKernelGetProcessTimeWide(); }

// (Phase 8 retired grade_for_button / grade_label: the reviewer now uses an
// on-screen D-pad-selected grade-button row confirmed with X, not the
// X/O/Square/Triangle mapping.)

// Yes/No confirmation dialog. X = yes (true), O = no (false).
bool confirm_dialog(Font& bold, Font& semibold, const char* title) {
    using namespace nb;
    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore buttons held on entry
    TouchState touch;
    touch_init(&touch);
    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed & SCE_CTRL_CROSS)  { audio::play(g_sfx_select); return true; }
        if (pressed & SCE_CTRL_CIRCLE) { audio::play(g_sfx_back);   return false; }
        int tx, ty;
        if (poll_tap(&touch, &tx, &ty)) {
            int w = 600, x = (SCREEN_W - w) / 2, y = 160;
            int bw = 220, bh = 64, by = y + 130;
            if (in_rect(tx, ty, x + 50, by, bw, bh)) return true;            // YES
            if (in_rect(tx, ty, x + w - 50 - bw, by, bw, bh)) return false;  // NO
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);
        int w = 600, h = 230, x = (SCREEN_W - w) / 2, y = 160;
        draw_shadow_rect(x, y, w, h, HOTRED, BLACK, SHADOW);
        draw_text_centered(bold, title, SCREEN_W / 2, y + 44, 26, WHITE);
        int bw = 220, bh = 64, by = y + 130;
        draw_shadow_rect(x + 50, by, bw, bh, WHITE, BLACK, 6, 4);
        draw_text_centered(bold, "YES", x + 50 + bw / 2, by + 14, 20, BLACK);
        draw_text_centered(semibold, "X", x + 50 + bw / 2, by + 42, 14, BLACK);
        draw_shadow_rect(x + w - 50 - bw, by, bw, bh, VIOLET, BLACK, 4);
        draw_text_centered(bold, "NO", x + w - 50 - bw / 2, by + 14, 20, BLACK);
        draw_text_centered(semibold, "O", x + w - 50 - bw / 2, by + 42, 14, BLACK);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

// -----------------------------------------------------------------------
// "No decks found" screen. Any button rescans; O exits the app.
// Returns true to rescan, false to exit.
// -----------------------------------------------------------------------
bool no_decks_screen(Font& bold, Font& semibold) {
    using namespace nb;
    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore buttons held on entry
    TouchState touch;
    touch_init(&touch);
    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed & SCE_CTRL_CIRCLE) return false;   // O = exit app
        if (pressed != 0) return true;                 // anything else = rescan
        int tx, ty;
        if (poll_tap(&touch, &tx, &ty)) return true;   // tap = rescan

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);
        // Centered bordered card with the message.
        int cw = 640, ch = 180, cx = (SCREEN_W - cw) / 2, cy = 180;
        draw_shadow_rect(cx, cy, cw, ch, YELLOW, BLACK, SHADOW);
        draw_text_centered(bold, "NO DECKS FOUND", SCREEN_W / 2, cy + 36, 28, BLACK);
        draw_text_centered(semibold, "TRANSFER A .VITADECK FOLDER VIA USB",
                           SCREEN_W / 2, cy + 86, 16, BLACK);
        draw_text_centered(semibold, "ANY BUTTON = RESCAN    O = EXIT",
                           SCREEN_W / 2, cy + 130, 14, BLACK);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

// =======================================================================
// Phase 9 -- per-deck statistics screen.
// =======================================================================

struct StatsData {
    int       day_counts[90];   // index 0 = oldest day in window, 89 = today
    long      oldest_serial;    // day-serial of index 0 (today_serial - 89)
    int       today_count;      // reviews whose local day == today
    long long today_ms;         // sum response_ms for today
    int       window_total;     // reviews inside the 90-day window
    int       window_good;      // grade 3+4 inside the window (for retention)
    bool      has_any;          // any valid review row at all
};

// Civil date from a day-serial (days since 1970-01-01). Inverse of the
// days_from_civil math inside date_serial(); used only for heatmap month labels.
void civil_from_days(long z, int* y, int* m, int* d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;                                  // [0, 146096]
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy  = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp  = (5 * doy + 2) / 153;
    long dd  = doy - (153 * mp + 2) / 5 + 1;
    long mm  = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

// Stream a deck's reviews.jsonl once, bucketing into the 90-day window.
void compute_stats(const char* folder, StatsData* sd) {
    std::memset(sd, 0, sizeof(*sd));
    char today[24];
    today_str(today, sizeof(today));                 // sceRtcGetCurrentClockLocalTime
    long today_serial = date_serial(today);
    sd->oldest_serial = today_serial - 89;

    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s/reviews.jsonl", VITADECK_ROOT, folder);
    std::FILE* f = std::fopen(path, "r");
    if (!f) return;                                  // absent -> has_any stays false
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        sqlite3_int64 ts = 0;
        if (!json_find_i64(line, "\"reviewed_at\"", &ts) || ts <= 0) continue;  // skip malformed
        int grade = 0, rms = 0;
        json_find_int(line, "\"grade\"", &grade);
        json_find_int(line, "\"response_ms\"", &rms);
        if (rms < 0) rms = 0;
        sd->has_any = true;
        // reviewed_at is local-as-if-UTC seconds (FORMAT.md), so ts/86400 is the
        // local day index since 1970-01-01 -- the SAME serial space date_serial()
        // uses for "today". [INFERRED from the local-as-if-UTC contract + the
        // days_from_civil math; the reverse mapping wasn't previously exercised
        // on-device, but matches the importer's gmtime-as-local convention.]
        long serial = (long)(ts / 86400);
        if (serial == today_serial) { sd->today_count++; sd->today_ms += rms; }
        long di = serial - sd->oldest_serial;
        if (di >= 0 && di <= 89) {
            sd->day_counts[di]++;
            sd->window_total++;
            if (grade == 3 || grade == 4) sd->window_good++;
        }
    }
    std::fclose(f);
}

// Case-insensitive substring test (needle must be lowercase).
bool name_has(const char* name, const char* needle) {
    char low[200];
    int i = 0;
    for (; name[i] && i < 199; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        low[i] = c;
    }
    low[i] = '\0';
    return std::strstr(low, needle) != nullptr;
}

// A rotating fun fact picked by the deck's subject (keyword-matched on its name).
const char* deck_fun_fact(const char* name, int rot) {
    static const char* CALC[] = {
        "THE INTEGRAL SIGN IS A STRETCHED 'S' FOR LATIN 'SUMMA', COINED BY LEIBNIZ IN 1675.",
        "A FUNCTION CAN BE CONTINUOUS EVERYWHERE YET DIFFERENTIABLE NOWHERE.",
        "NEWTON AND LEIBNIZ DEVELOPED CALCULUS INDEPENDENTLY IN THE LATE 1600S.",
    };
    static const char* GEN[] = {
        "HUMANS SHARE ABOUT 60% OF THEIR DNA WITH BANANAS.",
        "ONLY ABOUT 1.5% OF THE HUMAN GENOME ACTUALLY CODES FOR PROTEINS.",
        "IDENTICAL TWINS SHARE DNA BUT STILL HAVE DIFFERENT FINGERPRINTS.",
    };
    static const char* MED[] = {
        "YOUR HEART BEATS ROUGHLY 100,000 TIMES EVERY DAY.",
        "NERVE IMPULSES CAN TRAVEL UP TO 120 METERS PER SECOND.",
        "THE BODY HOLDS ENOUGH CARBON TO FILL ABOUT 9,000 PENCILS.",
    };
    static const char* LIN[] = {
        "THE LINUX KERNEL WAS FIRST RELEASED BY LINUS TORVALDS IN 1991.",
        "LINUX RUNS MOST OF THE INTERNET, PLUS PHONES, CARS, AND SUPERCOMPUTERS.",
        "'LINUX' IS JUST THE KERNEL; THE FULL SYSTEM IS OFTEN CALLED GNU/LINUX.",
    };
    static const char* SPA[] = {
        "SPANISH IS THE WORLD'S 2ND-MOST SPOKEN NATIVE LANGUAGE, AFTER MANDARIN.",
        "SPANISH HAS TWO VERBS FOR 'TO BE': SER AND ESTAR.",
        "INVERTED OPENING ! AND ? MARKS ARE A HALLMARK OF SPANISH PUNCTUATION.",
    };
    static const char* STUDY[] = {
        "SPACED REPETITION BEATS CRAMMING BY FIGHTING THE FORGETTING CURVE.",
        "REVIEWING RIGHT BEFORE YOU'D FORGET IS THE MOST EFFICIENT WAY TO LEARN.",
        "ACTIVE RECALL CEMENTS MEMORY FAR BETTER THAN RE-READING NOTES.",
    };
    const char** arr = STUDY;
    if      (name_has(name, "calc") || name_has(name, "multivariable")) arr = CALC;
    else if (name_has(name, "genetic"))                                 arr = GEN;
    else if (name_has(name, "mcat") || name_has(name, "mile"))          arr = MED;
    else if (name_has(name, "linux"))                                   arr = LIN;
    else if (name_has(name, "spa"))                                     arr = SPA;
    return arr[((rot % 3) + 3) % 3];
}

// Per-deck stats screen. L/R cycle decks (wrap); O/SELECT dismiss.
void show_stats(Font& bold, Font& semibold, const DeckEntry* decks, int count,
                int start) {
    using namespace nb;
    const unsigned int VIOLET_LO = RGBA8(220, 213, 251, 255);  // 1-4 reviews

    int idx = start;
    int day_sel = 89;                        // heatmap day cursor (89 = today)
    int stick_cd = 0;                        // left-stick repeat cooldown (frames)
    int frame = 0;                           // for rotating the fun fact
    StatsData sd;
    compute_stats(decks[idx].folder, &sd);

    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore the SELECT that opened us

    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) { audio::play(g_sfx_back); return; }
        if (pressed & SCE_CTRL_LTRIGGER) {
            idx = (idx - 1 + count) % count; compute_stats(decks[idx].folder, &sd);
            audio::play(g_sfx_scroll);
        }
        if (pressed & SCE_CTRL_RTRIGGER) {
            idx = (idx + 1) % count; compute_stats(decks[idx].folder, &sd);
            audio::play(g_sfx_scroll);
        }
        // D-pad AND left stick scrub the heatmap day cursor (left/right).
        if (pressed & SCE_CTRL_LEFT)  { if (day_sel > 0)  { --day_sel; audio::play(g_sfx_scroll); } }
        if (pressed & SCE_CTRL_RIGHT) { if (day_sel < 89) { ++day_sel; audio::play(g_sfx_scroll); } }
        bool sr = pad.lx > 170, sl = pad.lx < 86;   // lx: 0 left, 255 right
        if (sr || sl) {
            if (stick_cd == 0) {
                if (sr) { if (day_sel < 89) ++day_sel; }
                else    { if (day_sel > 0)  --day_sel; }
                audio::play(g_sfx_scroll);
                stick_cd = 6;
            } else --stick_cd;
        } else stick_cd = 0;
        ++frame;

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);

        // ---- Header: < PREV  DECKNAME  NEXT >  ........  STATS ----
        const int HEADER_H = 64;
        draw_rect(0, HEADER_H - BORDER, SCREEN_W, BORDER, BLACK);
        char dname[200]; upper(decks[idx].name, dname, sizeof(dname));
        draw_text(semibold, "< PREV", 20, 24, 16, VIOLET);
        int prevw = semibold.measure("< PREV", 16);
        int nx = 20 + prevw + 16;
        draw_text(bold, dname, nx, 16, 24, BLACK);
        int dnw = bold.measure(dname, 24);
        draw_text(semibold, "NEXT >", nx + dnw + 16, 24, 16, VIOLET);
        int sw = bold.measure("STATS", 20);
        draw_text(bold, "STATS", SCREEN_W - 20 - sw, 18, 20, BLACK);

        if (!sd.has_any) {
            int cw = 480, ch = 200, cx = (SCREEN_W - cw) / 2, cy = 180;
            draw_shadow_rect(cx, cy, cw, ch, WHITE, BLACK, SHADOW);
            draw_rect(cx, cy, cw, 12, HOTRED);                 // hot red top strip
            draw_rect(cx, cy + 12, cw, BORDER, BLACK);
            draw_text_centered(bold, "NO REVIEWS YET", cx + cw / 2, cy + 72, 28, BLACK);
            draw_text_centered(semibold, "REVIEW THIS DECK TO SEE STATS",
                               cx + cw / 2, cy + 122, 16, BLACK);
            draw_text_centered(semibold, "L / R = CHANGE DECK    O = BACK",
                               cx + cw / 2, cy + 168, 14, BLACK);
            vita2d_end_drawing();
            vita2d_swap_buffers();
            continue;
        }

        // ---- Summary row: four 240x80 cards tiled edge to edge. ----
        const int SUM_Y = HEADER_H, SUM_H = 80, CARDW = 240;
        char v0[24], v1[64], v2[48], v3[32], tmp[24], tmp2[24];
        i64_to_dec((sqlite3_int64)sd.today_count, v0, sizeof(v0));
        { long sec = (long)(sd.today_ms / 1000);
          i64_to_dec(sec / 60, tmp, sizeof(tmp)); i64_to_dec(sec % 60, tmp2, sizeof(tmp2));
          std::snprintf(v1, sizeof(v1), "%sm %ss", tmp, tmp2); }
        { long avg_ms = sd.today_count ? (long)(sd.today_ms / sd.today_count) : 0;
          i64_to_dec((avg_ms + 500) / 1000, tmp, sizeof(tmp));
          std::snprintf(v2, sizeof(v2), "%ss/card", tmp); }
        { int ret = sd.window_total ? (sd.window_good * 100) / sd.window_total : 0;
          i64_to_dec((sqlite3_int64)ret, tmp, sizeof(tmp));
          std::snprintf(v3, sizeof(v3), "%s%%", tmp); }
        const char* vals[4] = { v0, v1, v2, v3 };
        const char* labs[4] = { "STUDIED TODAY", "TIME TODAY", "AVG PACE", "RETENTION" };
        for (int i = 0; i < 4; ++i) {
            int x = i * CARDW;
            draw_shadow_rect(x, SUM_Y, CARDW, SUM_H, WHITE, BLACK, SHADOW);
            draw_text_centered(bold, vals[i], x + CARDW / 2, SUM_Y + 14, 28, BLACK);
            int strip_y = SUM_Y + SUM_H - 12;          // 12px label strip, flush bottom
            draw_rect(x, strip_y, CARDW, 12, VIOLET);
            draw_rect(x, strip_y, CARDW, BORDER, BLACK);
            draw_text_centered(semibold, labs[i], x + CARDW / 2, strip_y - 2, 14, BLACK);
        }

        // ---- 90-day activity heatmap (13 weeks x 7 days), centered. ----
        const int CELL = 28, PITCH = 32, GRID_W = 13 * PITCH;
        const int gx = (SCREEN_W - GRID_W) / 2;
        const int gy = 200;
        static const char* MON[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                      "JUL","AUG","SEP","OCT","NOV","DEC"};
        int prev_month = -1;
        for (int c = 0; c < 13; ++c) {                 // month labels at column starts
            int yy, mm, dd; civil_from_days(sd.oldest_serial + (long)c * 7, &yy, &mm, &dd);
            if (mm != prev_month) {
                draw_text(semibold, MON[mm - 1], gx + c * PITCH, gy - 22, 14, BLACK);
                prev_month = mm;
            }
        }
        { const char* dl[3] = {"M","W","F"}; int rows[3] = {0, 2, 4};   // day labels
          for (int k = 0; k < 3; ++k) {
              int w = semibold.measure(dl[k], 14);
              draw_text(semibold, dl[k], gx - 8 - w, gy + rows[k] * PITCH + 6, 14, BLACK);
          } }
        for (int i = 0; i < 90; ++i) {                 // every day gets a box
            int col = i / 7, row = i % 7;
            int x = gx + col * PITCH, y = gy + row * PITCH;
            int cnt = sd.day_counts[i];
            bool today = (i == 89);
            unsigned int fill = CREAM;                 // 0 reviews -> blank box
            if      (cnt == 0)   fill = CREAM;
            else if (cnt <= 4)   fill = VIOLET_LO;
            else if (cnt <= 9)   fill = VIOLET;
            else if (cnt <= 19)  fill = YELLOW;
            else                 fill = HOTRED;
            draw_rect(x, y, CELL, CELL, fill);
            draw_rect_outline(x, y, CELL, CELL, BLACK, today ? 5 : 2);  // today: 5px
        }
        // Day cursor: a cyan ring around the selected cell.
        {
            int col = day_sel / 7, row = day_sel % 7;
            int x = gx + col * PITCH, y = gy + row * PITCH;
            draw_rect_outline(x - 3, y - 3, CELL + 6, CELL + 6, CYAN, 3);
        }
        // Selected-day readout below the grid.
        {
            int yy, mm, dd;
            civil_from_days(sd.oldest_serial + day_sel, &yy, &mm, &dd);
            char db[16], cb[16], rd[64];
            i64_to_dec((sqlite3_int64)dd, db, sizeof(db));
            i64_to_dec((sqlite3_int64)sd.day_counts[day_sel], cb, sizeof(cb));
            std::snprintf(rd, sizeof(rd), "%s %s:  %s REVIEWS", MON[mm - 1], db, cb);
            draw_text_centered(semibold, rd, SCREEN_W / 2, gy + 7 * PITCH + 8, 16, BLACK);
        }

        // Fun fact card (fills the bottom space; subject picked from the deck name,
        // rotates every ~3 seconds).
        {
            int fx = 40, fy = 466, fw = 880, fh = 62;
            draw_shadow_rect(fx, fy, fw, fh, YELLOW, BLACK, SHADOW);
            draw_text(bold, "DID YOU KNOW?", fx + 16, fy + 8, 14, BLACK);
            const char* fact = deck_fun_fact(decks[idx].name, frame / 180);
            draw_text_centered(semibold, fact, fx + fw / 2, fy + 34, 14, BLACK);
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

// -----------------------------------------------------------------------
// Deck picker. D-pad up/down moves the cursor; X selects; START exits.
// SELECT opens the stats screen for the highlighted deck.
// Returns the selected deck index, or -1 to exit the app.
// -----------------------------------------------------------------------
int run_picker(Font& bold, Font& semibold, const DeckEntry* decks, int count,
               bool overflow, const Player* P) {
    using namespace nb;

    // Layout: 64px header, deck list on the left 60%, missions on right 40%.
    const int HEADER_H   = 64;
    const int LIST_TOP   = HEADER_H + 16;
    const int LIST_BOT   = SCREEN_H - 40;
    const int CARD_X     = 20;
    const int CARD_W     = 540;          // left ~60%
    const int CARD_H     = 60;
    const int CARD_GAP   = 12;
    const int MX = 596, MW = 344;        // missions panel (right ~40%)

    int visible = (LIST_BOT - LIST_TOP + CARD_GAP) / (CARD_H + CARD_GAP);
    if (visible < 1) visible = 1;

    int sel = 0, top = 0;
    int stick_cd = 0;          // left-stick repeat cooldown (frames)
    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore buttons held on entry
    TouchState touch;
    touch_init(&touch);

    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;

        if (pressed & SCE_CTRL_CIRCLE) {              // O = exit (with confirm)
            if (confirm_dialog(bold, semibold, "EXIT VITADECK?")) return -1;
        }
        if (pressed & SCE_CTRL_TRIANGLE) {            // Triangle = reset user data
            if (confirm_dialog(bold, semibold, "RESET USER DATA?")) return -2;
        }
        if (pressed & SCE_CTRL_SELECT) {              // SELECT = stats for highlighted deck
            show_stats(bold, semibold, decks, count, sel);
            sceCtrlPeekBufferPositive(0, &pad, 1);    // re-prime: don't read the dismiss as input
        }
        // L/R = skip menu music (back/next). Without input it keeps auto-advancing.
        if (pressed & SCE_CTRL_LTRIGGER) audio::bgm_skip(-1);
        if (pressed & SCE_CTRL_RTRIGGER) audio::bgm_skip(+1);
        // D-pad and stick both wrap around the list (top<->bottom).
        if (pressed & SCE_CTRL_UP)   { sel = (sel - 1 + count) % count; audio::play(g_sfx_scroll); }
        if (pressed & SCE_CTRL_DOWN) { sel = (sel + 1) % count;         audio::play(g_sfx_scroll); }
        // Left stick scrolls the deck list too (held = repeat). ly: 0 up, 255 down.
        bool sdn = pad.ly > 170, sup = pad.ly < 86;
        if (sdn || sup) {
            if (stick_cd == 0) {
                if (sdn) sel = (sel + 1) % count;            // wrap bottom -> top
                else     sel = (sel - 1 + count) % count;    // wrap top -> bottom
                audio::play(g_sfx_scroll);
                stick_cd = 7;                        // ~7 frames between repeats
            } else --stick_cd;
        } else stick_cd = 0;
        if (pressed & SCE_CTRL_CROSS) { audio::play(g_sfx_select); return sel; }
        // Touch: tap a deck card to launch it.
        int tx, ty;
        if (poll_tap(&touch, &tx, &ty)) {
            for (int i = top; i < count && i < top + visible; ++i) {
                int ry = LIST_TOP + (i - top) * (CARD_H + CARD_GAP);
                if (in_rect(tx, ty, CARD_X, ry, CARD_W, CARD_H)) {
                    audio::play(g_sfx_select);
                    return i;
                }
            }
        }

        // Keep the cursor inside the scroll window.
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);            // base layer (also shows behind header)

        // ---- Header bar: dots behind, 3px black bottom border. ----
        draw_rect(0, HEADER_H - BORDER, SCREEN_W, BORDER, BLACK);
        draw_text(bold, "VITADECK", 20, 16, 28, BLACK);
        {
            char nbuf[16], line[48], up[48];
            i64_to_dec((sqlite3_int64)P->streak_days, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "%s %s DAYS", STREAK_PREFIX, nbuf);
            upper(line, up, sizeof(up));
            int w = bold.measure(up, 20);
            draw_text(bold, up, SCREEN_W - 20 - w, 6, 20, BLACK);
            i64_to_dec((sqlite3_int64)P->level, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "LV.%s", nbuf);
            w = bold.measure(line, 20);
            draw_text(bold, line, SCREEN_W - 20 - w, 34, 20, BLACK);
        }

        // ---- Deck list (left). Each entry is a hard-shadow card. ----
        for (int i = top; i < count && i < top + visible; ++i) {
            int row = i - top;
            int y = LIST_TOP + row * (CARD_H + CARD_GAP);
            bool here = (i == sel);
            unsigned int fill   = here ? HOTRED : WHITE;
            unsigned int tcol   = here ? WHITE  : BLACK;
            int shadow          = here ? 8 : 6;
            draw_shadow_rect(CARD_X, y, CARD_W, CARD_H, fill, BLACK, shadow);

            char up[200];
            upper(decks[i].name, up, sizeof(up));
            draw_text(bold, up, CARD_X + 16, y + 8, 22, tcol);

            char cnt[24], line[64];
            if (decks[i].card_count >= 0)
                i64_to_dec((sqlite3_int64)decks[i].card_count, cnt, sizeof(cnt));
            else { cnt[0] = '?'; cnt[1] = '\0'; }
            std::snprintf(line, sizeof(line), "%s CARDS", cnt);
            draw_text(semibold, line, CARD_X + 16, y + 36, 16, tcol);

            if (decks[i].cloze_verified) {
                const char* badge = "[CLOZE]";
                int bw = bold.measure(badge, 16);
                draw_text(bold, badge, CARD_X + CARD_W - 16 - bw, y + 36, 16,
                          here ? WHITE : VIOLET);
            }
        }
        // Scroll affordances when the list overflows the window.
        if (top > 0)
            draw_text(bold, "^", CARD_X + CARD_W / 2, LIST_TOP - 14, 16, BLACK);
        if (top + visible < count || overflow)
            draw_text(bold, "v", CARD_X + CARD_W / 2, LIST_BOT - 4, 16, BLACK);

        // ---- Missions panel (right): yellow card. ----
        int MH = LIST_BOT - LIST_TOP;
        draw_shadow_rect(MX, LIST_TOP, MW, MH, YELLOW, BLACK, SHADOW);
        draw_text(bold, "DAILY MISSIONS", MX + 18, LIST_TOP + 16, 18, BLACK);
        {
            char today[24];
            today_str(today, sizeof(today));
            bool fresh = (std::strcmp(P->missions_date, today) == 0);
            int  mrev  = fresh ? P->m_reviewed     : 0;
            bool measy = fresh ? P->m_easy         : false;
            bool mdone = fresh ? P->m_session_done : false;

            int my = LIST_TOP + 60;
            char cnt[24], line[64];
            i64_to_dec((sqlite3_int64)(mrev > 20 ? 20 : mrev), cnt, sizeof(cnt));
            draw_text(semibold, "REVIEW 20 CARDS", MX + 18, my, 15, BLACK);
            if (mrev >= 20) draw_text(bold, "DONE", MX + MW - 80, my, 15, BLACK);
            else {
                std::snprintf(line, sizeof(line), "%s/20", cnt);
                int w = semibold.measure(line, 15);
                draw_text(semibold, line, MX + MW - 18 - w, my, 15, BLACK);
            }
            my += 44;
            draw_text(semibold, "GRADE A CARD EASY", MX + 18, my, 15, BLACK);
            draw_text(bold, measy ? "DONE" : "--", MX + MW - 80, my, 15, BLACK);
            my += 44;
            draw_text(semibold, "FINISH A SESSION", MX + 18, my, 15, BLACK);
            draw_text(bold, mdone ? "DONE" : "--", MX + MW - 80, my, 15, BLACK);
        }

        // Footer hint.
        {
            int fx = 20, fy = SCREEN_H - 28;
            const char* h = "UP/DOWN   X=OPEN   O=EXIT   SELECT=STATS   ";
            draw_text(semibold, h, fx, fy, 14, BLACK);
            fx += semibold.measure(h, 14);
            draw_triangle_icon(fx, fy, 13, BLACK, CREAM);   // the Triangle button glyph
            fx += 13 + 6;
            draw_text(semibold, "= RESET DATA", fx, fy, 14, BLACK);
            fx += semibold.measure("= RESET DATA", 14) + 18;
            draw_text(semibold, "L/R = PREV/NEXT TRACK", fx, fy, 14, VIOLET);
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

// -----------------------------------------------------------------------
// Reviewer session for a single deck folder. Paths are built from the folder
// name. Returns how the user left: EXIT_APP (START) or BACK_TO_PICKER (SELECT).
// -----------------------------------------------------------------------
// Resume prompt (Step 5). Returns 1 = continue (X), 0 = start over (O).
int resume_dialog(Font& bold, Font& semibold) {
    using namespace nb;
    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore buttons held on entry
    TouchState touch;
    touch_init(&touch);
    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed & SCE_CTRL_CROSS)  { audio::play(g_sfx_select); return 1; }
        if (pressed & SCE_CTRL_CIRCLE) { audio::play(g_sfx_back);   return 0; }
        int tx, ty;
        if (poll_tap(&touch, &tx, &ty)) {
            int w = 640, x = (SCREEN_W - w) / 2, y = 150;
            int bw = 250, bh = 70, by = y + 140;
            if (in_rect(tx, ty, x + 40, by, bw, bh)) return 1;           // CONTINUE
            if (in_rect(tx, ty, x + w - 40 - bw, by, bw, bh)) return 0;  // START OVER
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);
        int w = 640, h = 250, x = (SCREEN_W - w) / 2, y = 150;
        draw_shadow_rect(x, y, w, h, YELLOW, BLACK, SHADOW);
        draw_text_centered(bold, "RESUME FROM LAST CARD?",
                           SCREEN_W / 2, y + 36, 24, BLACK);
        int bw = 250, bh = 70, by = y + 140;
        draw_shadow_rect(x + 40, by, bw, bh, VIOLET, BLACK, 6, 4);   // highlighted
        draw_text_centered(bold, "CONTINUE", x + 40 + bw / 2, by + 14, 20, BLACK);
        draw_text_centered(semibold, "X", x + 40 + bw / 2, by + 44, 14, BLACK);
        draw_shadow_rect(x + w - 40 - bw, by, bw, bh, WHITE, BLACK, 4);
        draw_text_centered(bold, "START OVER", x + w - 40 - bw / 2, by + 14, 18, BLACK);
        draw_text_centered(semibold, "O", x + w - 40 - bw / 2, by + 44, 14, BLACK);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

RevResult run_reviewer(Font& bold, Font& semibold, const DeckEntry* deck,
                       Player* P) {
    using namespace nb;
    // Copy the folder into a bounded local first. Across inlining the compiler
    // can't prove deck->folder is NUL-terminated within its 256 bytes (it
    // assumes the string could span the whole decks[] array), which trips
    // -Wformat-truncation. A hand-bounded copy gives it a provable <=255 length.
    char folder[256];
    {
        size_t i = 0;
        for (; i + 1 < sizeof(folder) && deck->folder[i]; ++i)
            folder[i] = deck->folder[i];
        folder[i] = '\0';
    }
    char cards_db_path[512];
    char reviews_log_path[512];
    char media_dir[512];
    char progress_path[512];
    char progress_tmp[512];
    std::snprintf(cards_db_path, sizeof(cards_db_path),
                  "%s/%s/cards.sqlite", VITADECK_ROOT, folder);
    std::snprintf(reviews_log_path, sizeof(reviews_log_path),
                  "%s/%s/reviews.jsonl", VITADECK_ROOT, folder);
    std::snprintf(media_dir, sizeof(media_dir),
                  "%s/%s/media", VITADECK_ROOT, folder);
    std::snprintf(progress_path, sizeof(progress_path),
                  "%s/%s/progress.json", VITADECK_ROOT, folder);
    std::snprintf(progress_tmp, sizeof(progress_tmp),
                  "%s/%s/progress_tmp.json", VITADECK_ROOT, folder);

    // static: keep these big buffers OFF the main thread's (small) stack --
    // run_reviewer's frame plus freetype's glyph rasterizer overflow it
    // otherwise (crash in gray_convert_glyph). Single-threaded, one reviewer
    // session at a time, so a shared instance is safe.
    static ImgCache imgcache;
    imgcache.init();

    sqlite3* cards_db = nullptr;
    char err_buf[512] = {0};
    bool boot_ok = true;

    if (sqlite3_open(cards_db_path, &cards_db) != SQLITE_OK) {
        std::snprintf(err_buf, sizeof(err_buf), "cards open: %s",
                      cards_db ? sqlite3_errmsg(cards_db) : "null db");
        sceClibPrintf("[vitadeck] %s\n", err_buf);
        boot_ok = false;
    }

    std::vector<sqlite3_int64> card_ids;
    if (boot_ok) {
        if (!load_card_ids(cards_db, &card_ids, err_buf, sizeof(err_buf))) {
            sceClibPrintf("[vitadeck] %s\n", err_buf);
            boot_ok = false;
        } else if (card_ids.empty()) {
            std::snprintf(err_buf, sizeof(err_buf), "card queue empty");
            boot_ok = false;
        }
    }

    sqlite3_stmt* card_stmt = nullptr;
    if (boot_ok) {
        int rc = sqlite3_prepare_v2(cards_db,
            "SELECT front_html, back_html FROM cards WHERE id = ?",
            -1, &card_stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::snprintf(err_buf, sizeof(err_buf), "card_stmt prepare rc=%d: %s",
                          rc, sqlite3_errmsg(cards_db));
            sceClibPrintf("[vitadeck] %s\n", err_buf);
            boot_ok = false;
        }
    }

    std::FILE* rev_log = nullptr;
    if (boot_ok) {
        rev_log = std::fopen(reviews_log_path, "a");
        if (!rev_log) {
            sceClibPrintf("[vitadeck] WARNING fopen(reviews.jsonl) failed at "
                          "session start; grades will not persist\n");
        } else {
            sceClibPrintf("[vitadeck] reviews.jsonl opened for session\n");
        }
    }

    // static: Card (~17KB) + the two 8KB text buffers must stay off the stack
    // (see ImgCache note above). Re-zeroed each session/card below.
    static Card current;
    static char front_text[8192];
    static char back_text[8192];
    std::memset(&current, 0, sizeof(current));
    front_text[0] = '\0';
    back_text[0] = '\0';
    enum class View { FRONT, BACK, DONE, STATS };
    View view = View::FRONT;
    size_t queue_idx = 0;
    int reviewed_count = 0;
    SceUInt64 back_shown_us = 0;
    int scroll_y = 0;        // left-stick scroll offset for the current card view
    int scroll_max = 0;      // set each frame from measured content height

    // Per-card image state (Step 4) and layout input.
    char img_name[128] = "";
    vita2d_texture* cur_img = nullptr;
    bool has_image = false;
    int  front_len = 0;
    int  grade_sel = 2;             // grade buttons: 0..3 -> Again/Hard/Good/Easy

    auto fetch_current = [&]() -> bool {
        if (queue_idx >= card_ids.size()) return false;
        if (!load_card_by_id(card_stmt, card_ids[queue_idx], &current)) return false;
        strip_html(current.front, front_text, sizeof(front_text));
        strip_html(current.back,  back_text,  sizeof(back_text));
        extract_cloze_answer(front_text, back_text,
                             current.answer, sizeof(current.answer));
        front_len = (int)std::strlen(front_text);
        // Image: parse the RAW front_html (strip_html already removed the tag
        // from front_text), then load via the LRU. null tex -> placeholder.
        has_image = parse_img_src(current.front, img_name, sizeof(img_name));
        // Diagnostic: skip the texture load entirely (placeholder), to isolate
        // the image path from the GPU crash. Keeps the image LAYOUT either way.
        cur_img = (has_image && !VD_DIAG_NO_IMAGES)
                      ? imgcache.get(media_dir, img_name) : nullptr;
        return true;
    };

    if (boot_ok && !fetch_current()) {
        std::snprintf(err_buf, sizeof(err_buf), "fetch first card failed");
        boot_ok = false;
    }

    // ---- Gamification: session start (cosmetic; never gates cards). ----
    char today[24];
    today_str(today, sizeof(today));

    // Streak transition is computed now and applied to P in memory, but it is
    // only PERSISTED once the player grades a card (first player_save in the
    // loop). So merely opening and leaving a deck can't advance or break a
    // streak. last_study_date is set to today here for that eventual save.
    {
        long ts = date_serial(today);
        bool new_day;
        if (P->last_study_date[0] == '\0') {
            P->streak_days = 1;                  // first study ever
            new_day = true;
        } else {
            long diff = ts - date_serial(P->last_study_date);
            if (diff <= 0) {                     // already counted today
                new_day = false;
            } else if (diff == 1) {              // consecutive day
                P->streak_days += 1;
                new_day = true;
            } else {                             // missed one or more days
                if (P->streak_freeze_tokens > 0) P->streak_freeze_tokens -= 1;
                else                             P->streak_days = 1;
                new_day = true;
            }
        }
        std::snprintf(P->last_study_date, sizeof(P->last_study_date), "%s", today);
        if (new_day) {                           // reset daily missions
            std::snprintf(P->missions_date, sizeof(P->missions_date), "%s", today);
            P->m_reviewed = 0;
            P->m_easy = false;
            P->m_session_done = false;
        }
    }

    // Per-session tallies for the stats screen + EASY_STREAK achievement.
    int  sess_reviewed  = 0;
    int  sess_good_easy = 0;
    int  sess_xp        = 0;
    int  sess_cur_easy  = 0;
    int  sess_max_easy  = 0;
    bool sess_unlocked[5] = { false, false, false, false, false };

    // Award any newly-earned achievements (idempotent: guarded by P->ach[]).
    auto check_achievements = [&]() {
        if (!P->ach[0] && P->total_reviews >= 1)   { P->ach[0] = true; sess_unlocked[0] = true; }
        if (!P->ach[1] && P->streak_days   >= 7)   { P->ach[1] = true; sess_unlocked[1] = true; }
        if (!P->ach[2] && P->streak_days   >= 30)  { P->ach[2] = true; sess_unlocked[2] = true; }
        if (!P->ach[3] && P->total_reviews >= 100) { P->ach[3] = true; sess_unlocked[3] = true; }
        if (!P->ach[4] && sess_max_easy    >= 5)   { P->ach[4] = true; sess_unlocked[4] = true; }
    };

    // ---- Session resume (Step 5): if progress.json matches this deck, offer
    // to continue from the saved card; else start at card 1. ----
    if (boot_ok) {
        sqlite3_int64 last_id = 0;
        bool last_back = false;
        char pdeck[160];
        if (progress_read(progress_path, &last_id, &last_back, pdeck, sizeof(pdeck))
            && std::strcmp(pdeck, deck->name) == 0) {
            int found = -1;
            for (size_t i = 0; i < card_ids.size(); ++i)
                if (card_ids[i] == last_id) { found = (int)i; break; }
            if (found >= 0) {
                if (resume_dialog(bold, semibold) == 1) {        // CONTINUE
                    queue_idx = (size_t)found;
                    if (fetch_current()) {
                        view = last_back ? View::BACK : View::FRONT;
                        scroll_y = 0;
                        if (view == View::BACK) back_shown_us = now_us();
                    }
                } else {                                          // START OVER
                    sceIoRemove(progress_path);
                }
            }
        }
    }

    RevResult result = RevResult::EXIT_APP;
    SceCtrlData pad{};
    SceCtrlData prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore buttons held on entry
    TouchState touch;
    touch_init(&touch);

    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        int tx = 0, ty = 0;
        bool tapped = poll_tap(&touch, &tx, &ty);   // polled every frame

        // Left-stick vertical scroll for card text that runs off-screen.
        // ly: 0 = up, 255 = down. scroll_max comes from the previous frame's
        // measured content height (one-frame lag is invisible).
        if (view == View::FRONT || view == View::BACK) {
            if (pad.ly > 170)      scroll_y += 14;
            else if (pad.ly < 86)  scroll_y -= 14;
            if (scroll_y < 0) scroll_y = 0;
            if (scroll_y > scroll_max) scroll_y = scroll_max;
        }

        if (pressed & SCE_CTRL_CIRCLE) {
            audio::play(g_sfx_back);
            // O returns to the deck selector. App exit is the system PS button
            // (like other Vita apps), so there is no in-app EXIT_APP path now.
            // This is still a session end: run the gamification flush + the
            // achievement check (notably EASY_STREAK, only checked here). The
            // resume point (progress.json) is KEPT so re-entering this deck
            // continues where you left off; it is cleared only on deck-complete
            // or an explicit "start over".
            result = RevResult::BACK_TO_PICKER;
            if (sess_reviewed > 0) {
                check_achievements();
                player_save(P);
                for (int i = 0; i < 5; ++i)
                    if (sess_unlocked[i])
                        show_overlay(bold, "ACHIEVEMENT UNLOCKED", ACH_NAMES[i]);
            }
            break;
        }

        if (boot_ok) {
            // Tap anywhere on the card band also flips front->back.
            bool flip_tap = tapped && ty > 72 && ty < 460;
            if (view == View::FRONT &&
                ((pressed & SCE_CTRL_RTRIGGER) || flip_tap)) {
                view = View::BACK;
                back_shown_us = now_us();
                grade_sel = 2;                       // default selection = GOOD
                scroll_y = 0;                         // start the answer at the top
                // Resume point: persist on the front->back flip (Step 5).
                progress_write(progress_tmp, progress_path, current.id, true,
                               deck->name, wall_clock_epoch_seconds());
            } else if (view == View::BACK) {
                // Grade buttons: D-pad left/right selects, X confirms; or tap.
                if (pressed & SCE_CTRL_LEFT)  { if (grade_sel > 0) --grade_sel; }
                if (pressed & SCE_CTRL_RIGHT) { if (grade_sel < 3) ++grade_sel; }
                int grade = (pressed & SCE_CTRL_CROSS) ? grade_sel + 1 : 0;
                if (tapped) {                        // tap a grade button
                    int mg = 20, gp = 12, bh = 58, by = 466;
                    int bw = (SCREEN_W - 2 * mg - 3 * gp) / 4;
                    for (int i = 0; i < 4; ++i) {
                        int bx = mg + i * (bw + gp);
                        if (in_rect(tx, ty, bx, by, bw, bh)) {
                            grade_sel = i;
                            grade = i + 1;
                        }
                    }
                }
                if (grade != 0) {
                    SceUInt64 elapsed_us = now_us() - back_shown_us;
                    sqlite3_int64 ts = wall_clock_epoch_seconds();
                    int response_ms = (int)(elapsed_us / 1000ULL);

                    if (rev_log) {
                        int n = std::fprintf(rev_log,
                            "{\"card_id\":%lld,\"reviewed_at\":%lld,"
                            "\"grade\":%d,\"response_ms\":%d}\n",
                            (long long)current.id, (long long)ts,
                            grade, response_ms);
                        std::fflush(rev_log);
                        sceClibPrintf("[vitadeck] graded card_id=%lld grade=%d "
                                      "ts=%lld ms=%d (%d bytes)\n",
                                      (long long)current.id, grade,
                                      (long long)ts, response_ms, n);
                    } else {
                        sceClibPrintf("[vitadeck] grade card_id=%lld grade=%d "
                                      "NOT PERSISTED (no log handle)\n",
                                      (long long)current.id, grade);
                    }

                    ++reviewed_count;

                    // ---- Gamification: XP, level, missions, easy-streak. ----
                    int award = (grade == 1 ? 5 : grade == 2 ? 10 :
                                 grade == 3 ? 15 : 20);
                    P->xp += award;
                    sess_xp += award;
                    P->total_reviews += 1;
                    P->m_reviewed += 1;
                    ++sess_reviewed;
                    if (grade == 3 || grade == 4) ++sess_good_easy;
                    if (grade == 4) {
                        P->m_easy = true;
                        ++sess_cur_easy;
                        if (sess_cur_easy > sess_max_easy) sess_max_easy = sess_cur_easy;
                    } else {
                        sess_cur_easy = 0;
                    }
                    bool leveled = false;
                    while (P->xp >= xp_to_reach_level(P->level + 1)) {
                        P->level += 1;
                        leveled = true;
                    }
                    // Batched persist: flush player.json every 10 grades. The
                    // session-end paths (deck-complete below, START and SELECT
                    // handlers) always flush, so nothing graded is lost on a
                    // clean exit. XP / level / streak / total_reviews accumulate
                    // in memory between flushes; a hard power loss can drop up
                    // to 9 grades of XP -- an accepted cosmetic tradeoff,
                    // consistent with the non-atomic-rename note in player_save.
                    // Threshold achievements (STREAK_7/30, REVIEWS_100) and
                    // EASY_STREAK are all checked at session end, so the batch
                    // cadence never changes which achievements are awarded.
                    if (sess_reviewed % 10 == 0) player_save(P);
                    if (leveled) {
                        char lv[16], l2[48];
                        i64_to_dec((sqlite3_int64)P->level, lv, sizeof(lv));
                        std::snprintf(l2, sizeof(l2), "Level %s", lv);
                        show_overlay(bold, "LEVEL UP", l2);
                    }

                    ++queue_idx;
                    if (queue_idx >= card_ids.size()) {
                        view = View::DONE;
                        if (rev_log) { std::fclose(rev_log); rev_log = nullptr; }
                        sceIoRemove(progress_path);       // deck done -> no resume
                        // ---- Session end (deck completed). ----
                        P->m_session_done = true;        // "finish a session"
                        check_achievements();
                        player_save(P);
                        for (int i = 0; i < 5; ++i)
                            if (sess_unlocked[i])
                                show_overlay(bold, "ACHIEVEMENT UNLOCKED",
                                             ACH_NAMES[i]);
                    } else if (!fetch_current()) {
                        std::snprintf(err_buf, sizeof(err_buf),
                                      "fetch next card failed");
                        boot_ok = false;
                    } else {
                        view = View::FRONT;
                        scroll_y = 0;                 // next card starts at top
                    }
                }
            } else if (view == View::DONE) {
                if (pressed != 0 || tapped) view = View::STATS;   // -> summary
            } else if (view == View::STATS) {
                if (pressed != 0 || tapped) {                     // -> picker
                    result = RevResult::BACK_TO_PICKER;
                    break;
                }
            }
        }

        // ---- Draw (neo-brutalist). ----
        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);

        if (!boot_ok) {
            int cw = 720, ch = 200, cx = (SCREEN_W - cw) / 2, cy = 170;
            draw_shadow_rect(cx, cy, cw, ch, HOTRED, BLACK, SHADOW);
            draw_text_centered(bold, "BOOT FAILURE", SCREEN_W / 2, cy + 40, 26, WHITE);
            draw_text_centered(semibold, err_buf, SCREEN_W / 2, cy + 96, 14, WHITE);
            draw_text_centered(semibold, "O = SELECTOR",
                               SCREEN_W / 2, cy + 150, 14, WHITE);
        } else if (view == View::DONE) {
            int cw = 560, ch = 220, cx = (SCREEN_W - cw) / 2, cy = 160;
            draw_shadow_rect(cx, cy, cw, ch, VIOLET, BLACK, SHADOW);
            draw_text_centered(bold, "DECK COMPLETE", SCREEN_W / 2, cy + 44, 30, BLACK);
            char num[24], line[48];
            i64_to_dec((sqlite3_int64)reviewed_count, num, sizeof(num));
            std::snprintf(line, sizeof(line), "REVIEWED %s CARDS", num);
            draw_text_centered(bold, line, SCREEN_W / 2, cy + 112, 20, BLACK);
            draw_text_centered(semibold, "X = SUMMARY      O = SELECTOR",
                               SCREEN_W / 2, cy + 164, 14, BLACK);
        } else if (view == View::STATS) {
            draw_text(bold, "SESSION SUMMARY", 40, 28, 28, BLACK);
            int cx = 60, y = 104;
            char nbuf[24], line[64];
            i64_to_dec((sqlite3_int64)sess_reviewed, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "CARDS REVIEWED:  %s", nbuf);
            draw_text(semibold, line, cx, y, 20, BLACK); y += 42;
            int acc = sess_reviewed > 0 ? (sess_good_easy * 100) / sess_reviewed : 0;
            i64_to_dec((sqlite3_int64)acc, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "ACCURACY:  %s%%", nbuf);
            draw_text(semibold, line, cx, y, 20, BLACK); y += 42;
            i64_to_dec((sqlite3_int64)sess_xp, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "XP EARNED:  %s", nbuf);
            draw_text(semibold, line, cx, y, 20, BLACK); y += 42;
            i64_to_dec((sqlite3_int64)P->streak_days, nbuf, sizeof(nbuf));
            std::snprintf(line, sizeof(line), "STREAK:  %s %s", STREAK_PREFIX, nbuf);
            draw_text(semibold, line, cx, y, 20, BLACK); y += 50;
            draw_text(bold, "ACHIEVEMENTS THIS SESSION", cx, y, 18, HOTRED); y += 34;
            bool any = false;
            for (int i = 0; i < 5; ++i)
                if (sess_unlocked[i]) {
                    draw_text(semibold, ACH_NAMES[i], cx + 20, y, 16, BLACK);
                    y += 28; any = true;
                }
            if (!any) draw_text(semibold, "(NONE)", cx + 20, y, 16, BLACK);
            draw_text(semibold, "ANY BUTTON = BACK TO PICKER",
                      40, SCREEN_H - 28, 14, BLACK);
        } else {
            // ---- HUD: streak (left), level + XP bar (center), card # (right). ----
            {
                char nbuf[16], s[24];
                i64_to_dec((sqlite3_int64)P->streak_days, nbuf, sizeof(nbuf));
                std::snprintf(s, sizeof(s), "%s %s", STREAK_PREFIX, nbuf);
                draw_text(bold, s, 20, 12, 20, BLACK);

                i64_to_dec((sqlite3_int64)P->level, nbuf, sizeof(nbuf));
                char lv[24];
                std::snprintf(lv, sizeof(lv), "LV.%s", nbuf);
                int barw = 200, barh = 16, bx = (SCREEN_W - barw) / 2, by = 20;
                int lvw = bold.measure(lv, 18);
                draw_text(bold, lv, bx - lvw - 12, 14, 18, BLACK);
                long lo = xp_to_reach_level(P->level);
                long hi = xp_to_reach_level(P->level + 1);
                float frac = (hi > lo) ? (float)(P->xp - lo) / (float)(hi - lo) : 0.0f;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                draw_rect(bx, by, barw, barh, WHITE);
                draw_rect(bx, by, (int)(barw * frac), barh, VIOLET);
                draw_rect_outline(bx, by, barw, barh, BLACK, BORDER);

                char pos[24], tot[24], cn[64];
                i64_to_dec((sqlite3_int64)(queue_idx + 1), pos, sizeof(pos));
                i64_to_dec((sqlite3_int64)card_ids.size(), tot, sizeof(tot));
                std::snprintf(cn, sizeof(cn), "%s / %s", pos, tot);
                int cw = bold.measure(cn, 18);
                draw_text(bold, cn, SCREEN_W - 20 - cw, 14, 18, BLACK);
            }

            bool back = (view == View::BACK);
            const char* text   = back ? back_text : front_text;
            const char* needle = back ? (current.answer[0] ? current.answer : nullptr)
                                      : CLOZE_MARK;
            bool contains = !back;
            const int CY = 72, CBOT = 452;   // card band (room for button row)

            // Render the body clipped to the card and offset by scroll_y; sets
            // scroll_max from the measured content height so the left stick can't
            // overscroll, and draws up/down affordances at the region's edge.
            auto draw_scroll = [&](Font& nf, int bx, int by, int bw, int by_bot,
                                   int clx0, int cly0, int clx1, int cly1, int sz) {
                int visible = by_bot - by;
                int cend = by;
                draw_body(nf, semibold, text, bx, by, bw, 1 << 20, sz,
                          needle, contains, HOTRED, &cend, /*measure_only=*/true);
                // cend is the TOP of the last line; add a line + margin so the
                // bottom of the content can scroll fully into view.
                int full = (cend - by) + nf.line_height(sz) + 16;
                scroll_max = full > visible ? full - visible : 0;
                if (scroll_y > scroll_max) scroll_y = scroll_max;
                vita2d_enable_clipping();
                vita2d_set_clip_rectangle(clx0, cly0, clx1, cly1);
                draw_body(nf, semibold, text, bx, by - scroll_y, bw, 1 << 20, sz,
                          needle, contains, HOTRED);
                vita2d_disable_clipping();
                if (scroll_y > 0)          draw_text(bold, "^", clx1 - 28, cly0 + 2, 20, HOTRED);
                if (scroll_y < scroll_max) draw_text(bold, "v", clx1 - 28, cly1 - 26, 20, HOTRED);
            };

            if (has_image) {                 // 60/40 split: text left, image right
                int tx = 40, tw = 540, ix = 600, iw = 320, ih = CBOT - CY;
                draw_shadow_rect(tx, CY, tw, ih, WHITE, BLACK, 6);
                draw_scroll(semibold, tx + 16, CY + 16, tw - 32, CY + ih - 16,
                            tx + 6, CY + 6, tx + tw - 6, CY + ih - 6, 20);
                if (cur_img && !VD_DIAG_LOAD_ONLY) {
                    draw_rect(ix + 4, CY + 4, iw, ih, BLACK);   // 4px hard shadow
                    draw_rect(ix, CY, iw, ih, WHITE);           // region bg
                    draw_image_fit(cur_img, ix, CY, iw, ih);
                    draw_rect_outline(ix, CY, iw, ih, BLACK, BORDER);
                } else {                                        // placeholder
                    draw_shadow_rect(ix, CY, iw, ih, WHITE, BLACK, 4);
                    draw_text_centered(bold, "[IMAGE]", ix + iw / 2,
                                       CY + ih / 2 - 12, 20, BLACK);
                }
            } else if (!back && front_len < 120) {   // short front: centered, Bold 26
                int cw = 880, cx = 40, ch = CBOT - CY;
                scroll_max = 0;              // short fronts never overflow
                draw_shadow_rect(cx, CY, cw, ch, WHITE, BLACK, 6);
                draw_body(bold, semibold, text, cx + 40, CY + 60,
                    cw - 80, CY + ch - 20, 26, needle, contains, HOTRED);
            } else {                                  // long / back: full-width, 20
                int cw = 880, cx = 40, ch = CBOT - CY;
                draw_shadow_rect(cx, CY, cw, ch, WHITE, BLACK, 6);
                draw_scroll(semibold, cx + 24, CY + 20, cw - 48, CY + ch - 16,
                            cx + 6, CY + 6, cx + cw - 6, CY + ch - 6, 20);
            }

            // ---- Bottom: grade buttons (back) or flip hint (front). ----
            if (back) {
                const char* labels[4] = { "AGAIN", "HARD", "GOOD", "EASY" };
                unsigned int cols[4] = { HOTRED, WHITE, YELLOW, VIOLET };
                int margin = 20, gap = 12, n = 4;
                int bw = (SCREEN_W - 2 * margin - (n - 1) * gap) / n;
                int by = 466, bh = 58;
                for (int i = 0; i < n; ++i) {
                    int bx = margin + i * (bw + gap);
                    bool s = (i == grade_sel);
                    draw_shadow_rect(bx, by, bw, bh, cols[i], BLACK, s ? 6 : 4,
                                     s ? 5 : 3);
                    draw_text_centered(bold, labels[i], bx + bw / 2, by + 18,
                                       20, BLACK);
                }
            } else {
                draw_text_centered(semibold,
                    "R = SHOW ANSWER          O = SELECTOR",
                    SCREEN_W / 2, SCREEN_H - 28, 14, BLACK);
            }
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    imgcache.freeall();

    if (rev_log)   std::fclose(rev_log);
    if (card_stmt) sqlite3_finalize(card_stmt);
    if (cards_db)  sqlite3_close(cards_db);
    return result;
}

// Format-version gate, run after the user picks a deck (see FORMAT.md "Format
// versioning contract"). Returns true to proceed with the load; false if the
// deck is too new for this build (after showing a blocking error screen).
bool deck_format_ok(Font& bold, Font& semibold, const DeckEntry* deck) {
    using namespace nb;
    int fmt = deck->format_version;

    if (fmt == 0) {                                   // absent -> assume v1
        sceClibPrintf("[vitadeck] WARN: %s has no format_version; assuming v1\n",
                      deck->folder);
        return true;
    }
    if (fmt == VITADECK_FORMAT_VERSION) return true;  // exact match
    if (fmt < VITADECK_FORMAT_VERSION) {              // older, within tolerance
        sceClibPrintf("[vitadeck] WARN: %s format_version=%d < reader %d; "
                      "proceeding\n", deck->folder, fmt, VITADECK_FORMAT_VERSION);
        return true;
    }

    // fmt > reader: too new to open. Blocking error screen.
    sceClibPrintf("[vitadeck] BLOCK: %s format_version=%d > reader %d (too new)\n",
                  deck->folder, fmt, VITADECK_FORMAT_VERSION);
    SceCtrlData pad{}, prev{};
    sceCtrlPeekBufferPositive(0, &pad, 1);   // prime: ignore the still-held select press
    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed != 0) return false;      // any button -> back to picker

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_dot_grid(DOTGRID);
        int w = 720, h = 250, x = (SCREEN_W - w) / 2, y = 150;
        draw_shadow_rect(x, y, w, h, HOTRED, BLACK, SHADOW);   // Hot Red + hard shadow
        draw_text_centered(bold, "DECK FORMAT TOO NEW", SCREEN_W / 2, y + 54, 24, WHITE);
        draw_text_centered(semibold, "RE-EXPORT WITH A NEWER VITADECK",
                           SCREEN_W / 2, y + 116, 16, WHITE);
        draw_text_centered(semibold, "ANY BUTTON = BACK", SCREEN_W / 2, y + 184, 14, WHITE);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

}  // namespace

int main() {
    sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
    SceSqliteMallocMethods mm{};
    mm.xMalloc = sql_malloc; mm.xRealloc = sql_realloc; mm.xFree = sql_free;
    sceSqliteConfigMallocMethods(&mm);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    vita2d_init();
    vita2d_set_clear_color(BG_COLOR);

    // Phase 8 fonts (Space Grotesk) for the whole UI (pgf fully retired).
    Font bold, semibold;
    bold.init("app0:assets/fonts/SpaceGrotesk-Bold.ttf");
    semibold.init("app0:assets/fonts/SpaceGrotesk-SemiBold.ttf");

    // SFX + streaming main-menu BGM. The song lives on the SD card (it's ~37MB
    // raw, too big for the vpk); absent file = no music, handled gracefully.
    bool audio_ok = audio::init();
    if (audio_ok) {
        g_sfx_scroll = audio::load("app0:assets/audio/scroll.raw");
        g_sfx_select = audio::load("app0:assets/audio/select.raw");
        g_sfx_back   = audio::load("app0:assets/audio/back.raw");
        audio_ok = audio::bgm_start();   // producer idle until bgm_play sets a playlist
    }

    // Gamification state: read once on launch (defaults if absent/corrupt).
    // run_reviewer mutates it and persists to player.json itself.
    Player player;
    player_load(&player);

    // BGM playlists (stream from the SD card). Menu = full volume; reviewer =
    // reduced so it sits under the cards. bgm_mode avoids restarting the same
    // playlist when we stay in the same context.
    static const char* MENU_BGM[] = { "ux0:data/vitadeck/menu0.raw",
                                      "ux0:data/vitadeck/menu1.raw",
                                      "ux0:data/vitadeck/menu2.raw",
                                      "ux0:data/vitadeck/menu3.raw",
                                      "ux0:data/vitadeck/menu4.raw",
                                      "ux0:data/vitadeck/menu5.raw",
                                      "ux0:data/vitadeck/menu6.raw",
                                      "ux0:data/vitadeck/menu7.raw",
                                      "ux0:data/vitadeck/menu8.raw" };
    static const char* REV_BGM[]  = { "ux0:data/vitadeck/rev0.raw",
                                      "ux0:data/vitadeck/rev1.raw" };
    const int MENU_VOL = 256, REV_VOL = 96;
    int bgm_mode = -1;   // 0 = menu, 1 = reviewer

    // Outer shell: scan -> picker -> reviewer -> (back to picker | exit).
    while (true) {
        DeckEntry decks[MAX_DECKS];
        bool overflow = false;
        int count = scan_decks(decks, MAX_DECKS, &overflow);

        if (count == 0) {
            if (!no_decks_screen(bold, semibold)) break;  // START = exit app
            continue;                                      // any button = rescan
        }

        int sel;
        if (count == 1) {
            sel = 0;                             // single deck: skip picker
        } else {
            if (bgm_mode != 0) {                 // entering the menu
                if (audio_ok) audio::bgm_play(MENU_BGM, 9, MENU_VOL);
                bgm_mode = 0;
            }
            sel = run_picker(bold, semibold, decks, count, overflow, &player);
            if (sel == -1) break;                // O = exit app
            if (sel == -2) {                     // Triangle = reset user data
                // Resets the gamification profile (XP/level/streak/achievements/
                // missions). Review logs (reviews.jsonl) are deliberately NOT
                // touched -- those are the grades waiting to sync into Anki.
                sceIoRemove(PLAYER_PATH);
                sceIoRemove(PLAYER_TMP);
                player_defaults(&player);
                continue;                        // back to picker with fresh stats
            }
        }

        // Format-version gate (FORMAT.md "Format versioning contract"): refuse a
        // deck that is too new for this build instead of crashing/mis-rendering.
        if (!deck_format_ok(bold, semibold, &decks[sel]))
            continue;                            // too new -> back to picker

        if (bgm_mode != 1) {    // entering a deck -> reviewer playlist, quieter
            if (audio_ok) audio::bgm_play(REV_BGM, 2, REV_VOL);
            bgm_mode = 1;
        }
        RevResult r = run_reviewer(bold, semibold, &decks[sel], &player);
        if (r == RevResult::EXIT_APP) break;
        // BACK_TO_PICKER: loop -> rescan -> picker (or auto-relaunch if 1 deck)
    }

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
