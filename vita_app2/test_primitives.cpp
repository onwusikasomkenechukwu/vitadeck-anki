// test_primitives.cpp -- Phase 8 Step 1 isolation test.
//
// A single screen that exercises every drawing primitive and Space Grotesk at
// several sizes, so the font packaging + freetype glyph cache + neo-brutalist
// primitives can be verified on hardware BEFORE any real screen is built on
// them. Builds as its own vpk (title VDCKTEST1); the Phase 7 app is untouched.
//
// Prints time-to-first-draw to the debug console (PSM/udcd console).

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "font.h"
#include "draw.h"

unsigned int sceLibcHeapSize = 16 * 1024 * 1024;

using namespace nb;

int main() {
    SceUInt64 t_boot = sceKernelGetProcessTimeWide();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita2d_init();
    vita2d_set_clear_color(CREAM);

    Font bold, semibold;
    bool fb = bold.init("app0:assets/fonts/SpaceGrotesk-Bold.ttf");
    bool fs = semibold.init("app0:assets/fonts/SpaceGrotesk-SemiBold.ttf");
    sceClibPrintf("[test] font bold=%d semibold=%d\n", (int)fb, (int)fs);

    bool first_frame = true;
    SceCtrlData pad{}, prev{};

    while (true) {
        prev = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~prev.buttons;
        if (pressed & SCE_CTRL_START) break;

        vita2d_start_drawing();
        vita2d_clear_screen();

        // Base layer: cream + halftone dots.
        draw_dot_grid(DOTGRID);

        // Title (Bold, uppercase, multiple sizes down the left).
        draw_text(bold, "VITADECK", 32, 24, 28, BLACK);
        draw_text(bold, "PRIMITIVES TEST", 32, 64, 20, BLACK);

        // Shadow-rect swatches in each palette color, with labels.
        struct Sw { unsigned int fill; const char* name; };
        Sw sws[5] = {
            { HOTRED, "HOT RED" }, { YELLOW, "YELLOW" }, { VIOLET, "VIOLET" },
            { WHITE,  "WHITE"   }, { BLACK,  "BLACK"  },
        };
        for (int i = 0; i < 5; ++i) {
            int x = 32 + i * 120;
            int y = 110;
            draw_shadow_rect(x, y, 100, 70, sws[i].fill, BLACK, SHADOW);
            unsigned int tc = (sws[i].fill == BLACK) ? WHITE : BLACK;
            draw_text_centered(bold, sws[i].name, x + 50, y + 26, 14, tc);
        }

        // Outline-only rectangle (3px) + a thick 4px "selected" variant.
        draw_rect_outline(32, 210, 200, 50, BLACK, 3);
        draw_text(semibold, "3PX OUTLINE", 48, 224, 18, BLACK);
        draw_shadow_rect(252, 210, 200, 50, YELLOW, BLACK, 8, 4);
        draw_text(bold, "8PX SHADOW 4PX", 264, 222, 16, BLACK);

        // Type specimen: Bold headings vs SemiBold body, several sizes.
        draw_text(bold,     "BOLD 26: THE QUICK BROWN FOX", 32, 290, 26, BLACK);
        draw_text(bold,     "Bold 20: Jumps Over 1234567890", 32, 326, 20, BLACK);
        draw_text(semibold, "SemiBold 18 body: the lazy dog. naive cafe.",
                  32, 356, 18, BLACK);
        draw_text(semibold, "SemiBold 16: measure/draw width check |||",
                  32, 382, 16, BLACK);

        // Centered text across the screen.
        draw_text_centered(bold, "CENTERED HEADING", SCREEN_W / 2, 410, 22, HOTRED);

        // Image placeholder box (what a missing image renders as).
        int ix = 720, iy = 110, iw = 200, ih = 150;
        draw_shadow_rect(ix, iy, iw, ih, WHITE, BLACK, 4);
        draw_text_centered(bold, "[IMAGE]", ix + iw / 2, iy + ih / 2 - 10, 18, BLACK);

        // Footer hint.
        draw_text(semibold, "START = exit", 32, SCREEN_H - 30, 16, BLACK);

        vita2d_end_drawing();
        vita2d_swap_buffers();

        if (first_frame) {
            first_frame = false;
            SceUInt64 t_first = sceKernelGetProcessTimeWide();
            sceClibPrintf("[test] time-to-first-draw: %u us\n",
                          (unsigned int)(t_first - t_boot));
        }
    }

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
