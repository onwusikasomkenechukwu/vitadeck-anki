// Phase 2 boot test for the Anki-on-Vita project.
//
// Does exactly three things:
//   1. Clears the screen to a solid color.
//   2. Draws one line of static text using vita2d's PGF system font.
//   3. Reads ux0:data/vitadeck/test.txt and draws its first line below
//      the static text. If the file cannot be opened, draws
//      "no file found" instead -- which per Phase 2 spec is a failure
//      to investigate, not acceptable behavior.
//
// All vita2d / SceCtrl signatures verified against the installed SDK
// headers at $VITASDK/arm-vita-eabi/include/ (vita2d.h lines 162-172
// for PGF API, psp2/ctrl.h for input).

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr unsigned int CLEAR_COLOR  = RGBA8(0x10, 0x18, 0x40, 0xFF);  // dark navy
constexpr unsigned int WHITE        = RGBA8(0xFF, 0xFF, 0xFF, 0xFF);
constexpr unsigned int AMBER        = RGBA8(0xFF, 0xC8, 0x40, 0xFF);
constexpr unsigned int RED          = RGBA8(0xFF, 0x60, 0x60, 0xFF);

constexpr const char* TEST_FILE_PATH = "ux0:data/vitadeck/test.txt";
constexpr const char* STATIC_LINE    = "vitadeck phase 2 boot test";
constexpr const char* NO_FILE_LINE   = "no file found";

// Reads first line of TEST_FILE_PATH into out_buf (sized buf_size).
// Returns true on success. Trims trailing \r and \n.
bool read_first_line(const char* path, char* out_buf, size_t buf_size) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        return false;
    }
    if (std::fgets(out_buf, static_cast<int>(buf_size), f) == nullptr) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    size_t n = std::strlen(out_buf);
    while (n > 0 && (out_buf[n - 1] == '\n' || out_buf[n - 1] == '\r')) {
        out_buf[--n] = '\0';
    }
    return true;
}

}  // namespace

int main() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    vita2d_init();
    vita2d_set_clear_color(CLEAR_COLOR);

    vita2d_pgf* font = vita2d_load_default_pgf();

    char file_line[256] = {0};
    bool file_ok = read_first_line(TEST_FILE_PATH, file_line, sizeof(file_line));

    SceCtrlData pad;
    std::memset(&pad, 0, sizeof(pad));

    while (true) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT | SCE_CTRL_CROSS)) {
            break;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();

        // Static line (top, white).
        vita2d_pgf_draw_text(font, 40, 80, WHITE, 1.5f, STATIC_LINE);

        // File content (below, amber on success, red on failure).
        const char* line = file_ok ? file_line : NO_FILE_LINE;
        unsigned int color = file_ok ? AMBER : RED;
        vita2d_pgf_draw_text(font, 40, 160, color, 1.5f, line);

        // Footer hint.
        vita2d_pgf_draw_text(font, 40, 480, WHITE, 0.9f,
            "press X / START / SELECT to exit");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_fini();
    vita2d_free_pgf(font);

    sceKernelExitProcess(0);
    return 0;
}
