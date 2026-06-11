// audio.h -- tiny SFX mixer for VitaDeck.
//
// One stereo SceAudioOut port (48 kHz) fed by a dedicated thread. SFX are raw
// 48 kHz / stereo / signed-16 PCM (converted from .wav at build time). play()
// drops a clip onto one of a few mixer channels; the thread sums active
// channels into each output grain. No compression, no streaming -- clips are
// short and fully resident.
//
// API surfaces verified against installed headers:
//   psp2/audioout.h           sceAudioOutOpenPort/Output, SCE_AUDIO_OUT_*
//   psp2/kernel/threadmgr.h   sceKernelCreate/StartThread, mutex calls

#pragma once

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>

namespace audio {

constexpr int GRAIN = 1024;   // samples per output (multiple of 64; ~21ms @48k)
constexpr int FREQ  = 48000;
constexpr int CHANS = 4;      // simultaneous SFX voices
// Keep affinity as a performance choice, not as a synchronization primitive.
// The BGM ring below uses acquire/release publication for cross-core safety.
constexpr int AUDIO_CPU = 0x40000;   // SCE_KERNEL_CPU_MASK_USER_2
constexpr int SFX_GAIN = 224;        // 0..256, leaves headroom for BGM
constexpr int BGM_GAIN = 224;        // 0..256, leaves headroom for SFX
constexpr int STEAL_FADE = 64;       // short anti-click ramp for UI SFX

struct Sfx { int16_t* data = nullptr; int frames = 0; };

struct Channel {
    const int16_t* data;
    int frames;
    int pos;
    int fade_in;
    const int16_t* old_data;
    int old_frames;
    int old_pos;
    int old_fade;
    bool active;
};

static int      g_port  = -1;
static SceUID   g_mutex = -1;
static SceUID   g_mixer_thread = -1;
static SceUID   g_bgm_thread = -1;
static Channel  g_ch[CHANS];
static int16_t  g_mix[GRAIN * 2];
static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_mixer_active{false};

// ---- Streaming BGM (mono 48k s16, looped, with fade). A producer thread
// fills a ring buffer from disk; the mixer thread consumes it and sums it into
// every output sample at the current (ramped) volume. Streaming avoids loading
// the whole ~37MB song into RAM. ----
constexpr uint32_t BGM_RING = 65536;   // power-of-two SPSC ring, ~1.36s mono
constexpr uint32_t BGM_MASK = BGM_RING - 1;
constexpr int BGM_MAX  = 12;           // songs per playlist
alignas(64) static int16_t g_ring[BGM_RING];
alignas(64) static std::atomic<uint32_t> g_ring_w{0};
alignas(64) static std::atomic<uint32_t> g_ring_r{0};
static std::atomic<bool> g_bgm_active{false};
static int              g_bgm_vol = 0;     // 0..256 current, mixer-owned
static std::atomic<int>  g_bgm_target{0};  // 0..256 desired fade goal
static std::FILE*    g_bgm_fp = nullptr;
static char          g_pl[BGM_MAX][256];   // current playlist (file paths)
static int           g_pl_count  = 0;
static int           g_pl_idx    = 0;
static std::atomic<bool> g_pl_switch{false};  // request: restart at g_pl[0]
static std::atomic<int>  g_pl_skip{0};        // request: skip +N / -N tracks

struct Telemetry {
    uint32_t underrun_samples;
    uint32_t producer_waits;
    uint32_t overflow_samples;
    uint32_t clipped_samples;
    uint32_t voice_steals;
    uint32_t playlist_switches;
    uint32_t file_open_failures;
    uint32_t empty_reads;
    uint32_t init_failures;
    uint32_t max_read_us;
    uint32_t ring_low_water;
    uint32_t ring_high_water;
};

static std::atomic<uint32_t> g_stat_underruns{0};
static std::atomic<uint32_t> g_stat_waits{0};
static std::atomic<uint32_t> g_stat_overflows{0};
static std::atomic<uint32_t> g_stat_clips{0};
static std::atomic<uint32_t> g_stat_steals{0};
static std::atomic<uint32_t> g_stat_switches{0};
static std::atomic<uint32_t> g_stat_open_failures{0};
static std::atomic<uint32_t> g_stat_empty_reads{0};
static std::atomic<uint32_t> g_stat_init_failures{0};
static std::atomic<uint32_t> g_stat_max_read_us{0};
static std::atomic<uint32_t> g_stat_ring_low{BGM_RING};
static std::atomic<uint32_t> g_stat_ring_high{0};

inline void stat_max(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t cur = target.load(std::memory_order_relaxed);
    while (value > cur &&
           !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
}

inline void stat_min(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t cur = target.load(std::memory_order_relaxed);
    while (value < cur &&
           !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
}

inline uint32_t ring_fill(uint32_t w, uint32_t r) { return w - r; }

inline void ring_reset() {
    g_ring_r.store(0, std::memory_order_release);
    g_ring_w.store(0, std::memory_order_release);
    stat_min(g_stat_ring_low, 0);
}

inline int limit_sample(int v, uint32_t& clipped) {
    constexpr int KNEE = 30000;
    constexpr int HEAD = 32767 - KNEE;
    int sign = 1;
    if (v < 0) { sign = -1; v = -v; }
    if (v <= KNEE) return sign * v;
    ++clipped;
    int over = v - KNEE;
    int limited = KNEE + (HEAD * over) / (over + HEAD);
    if (limited > 32767) limited = 32767;
    return sign * limited;
}

inline Telemetry telemetry() {
    Telemetry t;
    t.underrun_samples = g_stat_underruns.load(std::memory_order_relaxed);
    t.producer_waits = g_stat_waits.load(std::memory_order_relaxed);
    t.overflow_samples = g_stat_overflows.load(std::memory_order_relaxed);
    t.clipped_samples = g_stat_clips.load(std::memory_order_relaxed);
    t.voice_steals = g_stat_steals.load(std::memory_order_relaxed);
    t.playlist_switches = g_stat_switches.load(std::memory_order_relaxed);
    t.file_open_failures = g_stat_open_failures.load(std::memory_order_relaxed);
    t.empty_reads = g_stat_empty_reads.load(std::memory_order_relaxed);
    t.init_failures = g_stat_init_failures.load(std::memory_order_relaxed);
    t.max_read_us = g_stat_max_read_us.load(std::memory_order_relaxed);
    t.ring_low_water = g_stat_ring_low.load(std::memory_order_relaxed);
    t.ring_high_water = g_stat_ring_high.load(std::memory_order_relaxed);
    return t;
}

inline int bgm_producer(SceSize, void*) {
    constexpr int CHUNK = 4096;
    static int16_t tmp[CHUNK];
    while (g_bgm_active.load(std::memory_order_acquire)) {
        // Playlist switch: publish the new path under the mutex, then flush the
        // old buffered tail so context changes cannot leak stale BGM.
        if (g_pl_switch.exchange(false, std::memory_order_acq_rel)) {
            char path[256] = "";
            sceKernelLockMutex(g_mutex, 1, nullptr);
            g_pl_idx = 0;
            if (g_pl_count > 0) std::snprintf(path, sizeof(path), "%s", g_pl[0]);
            sceKernelUnlockMutex(g_mutex, 1);
            if (g_bgm_fp) { std::fclose(g_bgm_fp); g_bgm_fp = nullptr; }
            sceKernelLockMutex(g_mutex, 1, nullptr);
            ring_reset();
            sceKernelUnlockMutex(g_mutex, 1);
            g_stat_switches.fetch_add(1, std::memory_order_relaxed);
            if (path[0]) {
                g_bgm_fp = std::fopen(path, "rb");
                if (!g_bgm_fp) g_stat_open_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Manual track skip (L/R in the menu): advance g_pl_idx by the requested
        // delta and flush the buffered tail so the new song starts immediately.
        int skip = g_pl_skip.exchange(0, std::memory_order_acq_rel);
        if (skip != 0 && g_pl_count > 0) {
            if (g_bgm_fp) { std::fclose(g_bgm_fp); g_bgm_fp = nullptr; }
            char path[256] = "";
            sceKernelLockMutex(g_mutex, 1, nullptr);
            g_pl_idx = ((g_pl_idx + skip) % g_pl_count + g_pl_count) % g_pl_count;
            std::snprintf(path, sizeof(path), "%s", g_pl[g_pl_idx]);
            ring_reset();
            sceKernelUnlockMutex(g_mutex, 1);
            g_stat_switches.fetch_add(1, std::memory_order_relaxed);
            if (path[0]) {
                g_bgm_fp = std::fopen(path, "rb");
                if (!g_bgm_fp) g_stat_open_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!g_bgm_fp) { sceKernelDelayThread(20000); continue; }
        uint32_t r = g_ring_r.load(std::memory_order_acquire);
        uint32_t w = g_ring_w.load(std::memory_order_relaxed);
        uint32_t fill = ring_fill(w, r);
        stat_max(g_stat_ring_high, fill);
        uint32_t freecnt = BGM_RING - fill;
        if (freecnt < CHUNK) {
            g_stat_waits.fetch_add(1, std::memory_order_relaxed);
            sceKernelDelayThread(3000);
            continue;
        }
        SceUInt64 start = sceKernelGetProcessTimeWide();
        size_t rd = std::fread(tmp, sizeof(int16_t), CHUNK, g_bgm_fp);
        SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
        if (elapsed > 0xffffffffu) elapsed = 0xffffffffu;
        stat_max(g_stat_max_read_us, (uint32_t)elapsed);
        if (rd == 0) {                                 // EOF -> next song in list
            g_stat_empty_reads.fetch_add(1, std::memory_order_relaxed);
            std::fclose(g_bgm_fp); g_bgm_fp = nullptr;
            char path[256] = "";
            sceKernelLockMutex(g_mutex, 1, nullptr);
            if (g_pl_count > 0) {
                g_pl_idx = (g_pl_idx + 1) % g_pl_count;
                std::snprintf(path, sizeof(path), "%s", g_pl[g_pl_idx]);
            }
            sceKernelUnlockMutex(g_mutex, 1);
            if (path[0]) {
                g_bgm_fp = std::fopen(path, "rb");
                if (!g_bgm_fp) g_stat_open_failures.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (rd > freecnt) {
            g_stat_overflows.fetch_add((uint32_t)(rd - freecnt), std::memory_order_relaxed);
            rd = freecnt;
        }
        for (size_t i = 0; i < rd; ++i) {
            g_ring[(w + (uint32_t)i) & BGM_MASK] = tmp[i];
        }
        g_ring_w.store(w + (uint32_t)rd, std::memory_order_release);
    }
    if (g_bgm_fp) { std::fclose(g_bgm_fp); g_bgm_fp = nullptr; }
    return 0;
}

// Start the BGM producer thread (idle until bgm_play sets a playlist).
inline bool bgm_start() {
    if (!g_initialized.load(std::memory_order_acquire) || g_mutex < 0) return false;
    if (g_bgm_active.load(std::memory_order_acquire)) return true;
    g_bgm_active.store(true, std::memory_order_release);
    g_bgm_thread = sceKernelCreateThread("vd_bgm_thr", bgm_producer,
                                         0x10000100, 0x2000, 0, AUDIO_CPU, nullptr);
    if (g_bgm_thread < 0 || sceKernelStartThread(g_bgm_thread, 0, nullptr) < 0) {
        if (g_bgm_thread >= 0) sceKernelDeleteThread(g_bgm_thread);
        g_bgm_thread = -1;
        g_bgm_active.store(false, std::memory_order_release);
        g_stat_init_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// Set the fade goal (0 = silent, 256 = full). The mixer ramps toward it.
inline void bgm_fade(int target) {
    if (target < 0)   target = 0;
    if (target > 256) target = 256;
    g_bgm_target.store(target, std::memory_order_release);
}

// Switch to a playlist (mono 48k s16 raw paths), looping, and fade to `vol`.
// The producer picks up the change at the next read; songs advance on EOF.
inline void bgm_play(const char* const* paths, int count, int vol) {
    if (!g_initialized.load(std::memory_order_acquire) || g_mutex < 0) return;
    if (count > BGM_MAX) count = BGM_MAX;
    if (count < 0) count = 0;
    sceKernelLockMutex(g_mutex, 1, nullptr);
    for (int i = 0; i < count; ++i)
        std::snprintf(g_pl[i], sizeof(g_pl[i]), "%s", paths[i]);
    g_pl_count = count;
    sceKernelUnlockMutex(g_mutex, 1);
    g_pl_switch.store(true, std::memory_order_release);
    bgm_fade(vol);
}

// Skip forward (+1) / back (-1) within the current playlist. Wraps. Multiple
// rapid presses accumulate. No-op if no playlist is set.
inline void bgm_skip(int dir) {
    g_pl_skip.fetch_add(dir, std::memory_order_release);
}

// Load a raw 48 kHz / stereo / s16le clip. Returns {nullptr,0} on failure
// (play() then silently no-ops, so audio never gates the UI).
inline Sfx load(const char* path) {
    Sfx s;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return s;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return s; }
    s.data = (int16_t*)std::malloc((size_t)n);
    if (!s.data) { std::fclose(f); return s; }
    size_t rd = std::fread(s.data, 1, (size_t)n, f);
    std::fclose(f);
    s.frames = (int)(rd / 4);   // 4 bytes/frame (stereo s16)
    return s;
}

inline void play(const Sfx& s) {
    if (!s.data || s.frames <= 0 || g_mutex < 0 ||
        !g_initialized.load(std::memory_order_acquire)) return;
    sceKernelLockMutex(g_mutex, 1, nullptr);
    int slot = -1, steal = 0;
    for (int i = 0; i < CHANS; ++i) {
        if (!g_ch[i].active) { slot = i; break; }
        if (g_ch[i].pos > g_ch[steal].pos) steal = i;   // most-advanced = steal
    }
    if (slot < 0) slot = steal;
    if (g_ch[slot].active) {
        g_ch[slot].old_data = g_ch[slot].data;
        g_ch[slot].old_frames = g_ch[slot].frames;
        g_ch[slot].old_pos = g_ch[slot].pos;
        g_ch[slot].old_fade = STEAL_FADE;
        g_stat_steals.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_ch[slot].old_data = nullptr;
        g_ch[slot].old_frames = 0;
        g_ch[slot].old_pos = 0;
        g_ch[slot].old_fade = 0;
    }
    g_ch[slot].data = s.data;
    g_ch[slot].frames = s.frames;
    g_ch[slot].pos = 0;
    g_ch[slot].fade_in = STEAL_FADE;
    g_ch[slot].active = true;
    sceKernelUnlockMutex(g_mutex, 1);
}

inline int thread_main(SceSize, void*) {
    while (g_mixer_active.load(std::memory_order_acquire)) {
        // Ramp BGM volume toward its fade goal once per grain (~0.7s full fade).
        int target = g_bgm_target.load(std::memory_order_acquire);
        if (g_bgm_vol < target) { g_bgm_vol += 8; if (g_bgm_vol > target) g_bgm_vol = target; }
        else if (g_bgm_vol > target) { g_bgm_vol -= 8; if (g_bgm_vol < target) g_bgm_vol = target; }

        sceKernelLockMutex(g_mutex, 1, nullptr);
        uint32_t local_underruns = 0;
        uint32_t local_clips = 0;
        uint32_t rpos = g_ring_r.load(std::memory_order_relaxed);
        uint32_t wpos = g_ring_w.load(std::memory_order_acquire);
        stat_min(g_stat_ring_low, ring_fill(wpos, rpos));
        for (int i = 0; i < GRAIN; ++i) {
            int l = 0, r = 0;
            for (int c = 0; c < CHANS; ++c) {
                Channel& ch = g_ch[c];
                if (ch.old_fade > 0 && ch.old_data && ch.old_pos < ch.old_frames) {
                    int gain = (SFX_GAIN * ch.old_fade) / STEAL_FADE;
                    l += (int)ch.old_data[ch.old_pos * 2] * gain / 256;
                    r += (int)ch.old_data[ch.old_pos * 2 + 1] * gain / 256;
                    ++ch.old_pos;
                    --ch.old_fade;
                    if (ch.old_fade <= 0 || ch.old_pos >= ch.old_frames) {
                        ch.old_data = nullptr;
                        ch.old_frames = 0;
                        ch.old_pos = 0;
                        ch.old_fade = 0;
                    }
                }
                if (!ch.active) continue;
                int gain = SFX_GAIN;
                if (ch.fade_in > 0) {
                    gain = (SFX_GAIN * (STEAL_FADE - ch.fade_in)) / STEAL_FADE;
                    --ch.fade_in;
                }
                l += (int)ch.data[ch.pos * 2] * gain / 256;
                r += (int)ch.data[ch.pos * 2 + 1] * gain / 256;
                if (++ch.pos >= ch.frames) ch.active = false;
            }
            // BGM: one mono ring sample per output sample, at faded volume.
            if (g_bgm_active.load(std::memory_order_relaxed) && rpos != wpos) {
                int v = (int)g_ring[rpos & BGM_MASK] * g_bgm_vol / 256;
                v = v * BGM_GAIN / 256;
                ++rpos;
                l += v; r += v;
            } else if (target > 0) {
                ++local_underruns;
            }
            l = limit_sample(l, local_clips);
            r = limit_sample(r, local_clips);
            g_mix[i * 2]     = (int16_t)l;
            g_mix[i * 2 + 1] = (int16_t)r;
        }
        g_ring_r.store(rpos, std::memory_order_release);
        if (local_underruns)
            g_stat_underruns.fetch_add(local_underruns, std::memory_order_relaxed);
        if (local_clips)
            g_stat_clips.fetch_add(local_clips, std::memory_order_relaxed);
        sceKernelUnlockMutex(g_mutex, 1);
        sceAudioOutOutput(g_port, g_mix);   // blocks ~GRAIN/FREQ until consumed
    }
    return 0;
}

// Open the port and start the mixer thread. Returns false on failure (the app
// then runs silently).
inline bool init() {
    if (g_initialized.load(std::memory_order_acquire)) return true;
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, FREQ,
                                 SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) {
        g_stat_init_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::memset(g_ch, 0, sizeof(g_ch));
    std::memset(g_mix, 0, sizeof(g_mix));
    ring_reset();
    g_mutex = sceKernelCreateMutex("vd_audio", 0, 0, nullptr);
    if (g_mutex < 0) {
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        g_stat_init_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_mixer_active.store(true, std::memory_order_release);
    g_mixer_thread = sceKernelCreateThread("vd_audio_thr", thread_main,
                                           0x10000100, 0x4000, 0, AUDIO_CPU, nullptr);
    if (g_mixer_thread < 0 || sceKernelStartThread(g_mixer_thread, 0, nullptr) < 0) {
        if (g_mixer_thread >= 0) sceKernelDeleteThread(g_mixer_thread);
        g_mixer_thread = -1;
        g_mixer_active.store(false, std::memory_order_release);
        sceKernelDeleteMutex(g_mutex);
        g_mutex = -1;
        sceAudioOutReleasePort(g_port);
        g_port = -1;
        g_stat_init_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_initialized.store(true, std::memory_order_release);
    return true;
}

}  // namespace audio
