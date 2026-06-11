# VitaDeck — Full Project Recap

A two-sided Anki-compatible flashcard system: a **desktop Python** side (export/import)
and a **PS Vita C++/vitaSDK** review client, meeting at the frozen `.vitadeck` package
format (`FORMAT.md`).

---

## Phase 0 / 1 — Package format + exporter (`FORMAT.md`, `exporter.py`)
Designed the frozen `.vitadeck` format from a real `.apkg` inspection, then built the
desktop exporter that produces it. Stdlib only — no Anki Python API on the export side.

**Investigation (`Linux+.apkg`):** inner collection is `collection.anki21` (schema ver 11,
SchedulerV2 legacy), **not** the `.anki2` stub (1 placeholder row) nor zstd `.anki21b`.
Note types/decks are JSON blobs in the `col` table; `notes.flds` joins fields with **0x1F**;
the `media` file is a JSON map of numbered archive entries → original filenames; cloze
models are `type:1` with `{{cN::answer::hint}}` syntax.

**Format delivered:** a `.vitadeck` folder — `manifest.json`, `cards.sqlite` (fully
**pre-resolved** front/back HTML so the Vita needs no template engine), `decks.json`,
`note_types.json`, `tags.sqlite` (separate card_tags index for filtering at med-school
scale), flat `media/` renamed to SHA-256 prefixes, `export_warnings.json`, `media_index.json`.

**Exporter delivered:** template resolution (`{{Field}}`, `{{FrontSide}}`, conditionals),
cloze rendering (`[...]` active / revealed inactive), HTML allowlist with per-card stripped-tag
logging, media hash-rename + reference rewrite, `data-font-size` span passthrough, `[sound:]`
sideload contract.

**Key findings & fixes:**
- None of the first three test decks had a cloze model → **verified cloze against four real
  decks** (`eng_{deu,fra,ita,spa}_clozes`, ~89k cloze cards) once they were added; the synthetic
  test was retired as load-bearing.
- `<table>` and `<pre>` are stripped (flagged in warnings) — surfaced as a real data-loss
  question for med-school comparison tables.

---

## Phase 2 — Vita environment proof
Proved the toolchain end-to-end before any review logic: one `.vpk` that builds, installs,
and runs on hardware (clears screen, draws PGF text, reads `ux0:data/vitadeck/test.txt`).

**Key findings & fixes:**
- The shell was **Git-for-Windows bash, not MSYS2** (no `pacman`). Installed MSYS2 via winget,
  then vitasdk via `vdpm` bootstrap + `install-all.sh` → `arm-vita-eabi-gcc 15.2.0`. All
  vitasdk commands since are driven through `C:\msys64\usr\bin\bash.exe -lc`.
- The PGF font API is **inlined into `vita2d.h`** (lines 162–172) — there is no separate
  `vita2d_pgf.h` in this SDK, contrary to older examples.

---

## Phase 3 — Vita reviewer, first card rendered
Brought the two sides together: open `cards.sqlite` from a `.vitadeck` on the card, query the
first card, render front/back, flip with R, log face-button grades.

**Key findings & fixes:**
- The Vita's **`SceSqlite` system module exports the full standard `sqlite3_*` API** but ships
  no public `sqlite3.h` — wrote a minimal shim header rather than bundle the amalgamation.
  Must `sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE)` first.
- **Fix 1 — `rc=7 SQLITE_NOMEM`:** SceSqlite has no default allocator; required
  `sceSqliteConfigMallocMethods` with a malloc/realloc/free adapter (struct takes `int`, not
  `size_t`).
- **Fix 2 — `rc=26 SQLITE_NOTADB`:** the exporter's WAL-mode header (write_ver=2) is rejected
  by the Vita's older SceSqlite → exporter now flips `PRAGMA journal_mode=DELETE` before close
  (write_ver=1). Still load-bearing for every deck.

---

## Phase 4 — Word wrap, card queue, review logging
Made it a functional reviewer: text that fits on screen, a queue that advances, grades that
persist.

**Delivered:** per-word wrap measured with `vita2d_pgf_text_width` (mid-word break for
over-long tokens, `v more` truncation marker); card queue (all ids loaded once, content
fetched lazily by id); deck-complete + restart screen; grade → review-log write with
`reviewed_at` (sceRtc) and `response_ms` (`sceKernelGetProcessTimeWide`).

**Key findings & fixes:**
- **Crash `C2-12828` between render steps:** `snprintf` with **`%zu` corrupts the vararg walk
  in newlib-nano** and aborts. (`%lld` works in `fprintf`/`sceClibPrintf` — that's why the log
  values were correct.) Card ids now formatted with a local `i64_to_dec` helper.
- **`reviews.sqlite` write hard-crashed** through SceSqlite's VFS → **format change to
  `reviews.jsonl`** (append-only via plain `fopen`, newlib→sceIo). Justified for med-school
  scale, documented in `FORMAT.md`; exporter no longer pre-creates the empty SQLite file.

---

## Phase 5 — Cloze rendering, session handle, cleanup
Made it usable for the real target workload (cloze-heavy decks), on `eng_spa_clozes` (20k cards).

**Delivered:**
- **Cloze color:** cyan `[...]` on the front; cyan answer on the back. The active answer is
  extracted **on-device by prefix/suffix-diffing stripped front vs back** (each card has exactly
  one active cloze, so the diff isolates it) — no schema change, plus an always-visible
  `answer:` line as a guaranteed fallback.
- **Persistent log handle:** `reviews.jsonl` opened once per session, `fflush` per grade,
  closed on exit/deck-complete (replaced the per-grade `fopen`/`fclose`).
- **Cleanup:** removed all `[STEP NN]` diagnostics (kept real runtime logs); decimal card ids;
  `strip_html` extended with `[sound:]` removal + numeric `&#NNN;` decode.

**Key findings & fixes:**
- **`vita2d_pgf_draw_text_color` does not exist** in this SDK; `vita2d_pgf_draw_text` already
  takes a color — multi-color is done by drawing segments at `text_width`-computed offsets.
- Vita clock corrected (Phase 4 had ~42 min skew); timestamps now match wall-clock within
  seconds. Confirmed **UTC-4** offset, consistent with the "local-as-if-UTC" `reviewed_at`
  contract. PGF font has **Latin-1 coverage** — Spanish accents render natively, no TTF needed
  (this later changed in Phase 8's font rewrite).

---

## Phase 6 — Desktop importer (`importer.py`)
Closes the loop: reads `reviews.jsonl` (grades recorded on the Vita) and replays them
into a live Anki collection through Anki's own scheduler.

**Delivered:** `importer.py` — safety checks (refuses to run if Anki is open), streaming
JSONL parse with validation/dedup, scheduler-driven grade application, `import_log.json`,
printed verification steps.

**Key findings & fixes:**
- The `anki` package isn't in the default **Python 3.14** — used Anki's own uv-managed
  venv (`%LOCALAPPDATA%\AnkiProgramFiles\.venv`, Python 3.13.5, anki 25.09.4).
- **API correction (confirmed from installed source):** the right call is
  `col.sched.answerCard(card, ease)` — the brief's `answer_card(card, ease)` actually
  takes a `CardAnswer` protobuf. Also had to add `card.start_timer()` (a bug the
  copy-test caught). SM-2 confirmed active (`fsrs` config = `None`).
- Tested end-to-end on a **throwaway copy** of the collection with synthetic + torture
  data (card …844 went New → Review, ivl 5d, ease 2500, +1 revlog row).

**State:** importer is ready; the real 6-row `reviews.jsonl` was located on the Vita and
pulled to `out\eng_spa_clozes.vitadeck\`. The actual live import against the real
`collection.anki2` is the one remaining optional step — it just needs Anki closed.

---

## Deck pipeline (exports + transfers)
- Vita memory card mounts as **D:** over USB; decks live at `D:\data\vitadeck\`
  (= `ux0:data/vitadeck/`).
- Exported and transferred via `exporter.py`: **MileDowns_MCAT_guide, Genetics_,
  _Multivariable_Calculus, Calculus_III_MAT273** (+ the original Linux+ and
  eng_spa_clozes). **6 decks** on the Vita now.
- **DAT Bootcamp** is Anki's newer `.anki21b` (zstd) format — initially unsupported, **now
  exported directly** by the exporter (see "Desktop patches" below): 377 cards.

---

## Phase 7 — Deck picker + gamification
**Part A — Deck picker:** scans for `*.vitadeck`, reads each `manifest.json`
(name / card count / cloze_verified), D-pad nav + scrolling, launches the selected deck.
Reviewer refactored from a hardcoded path to per-deck.

**Part B — Gamification** (`player.json`, atomic write):
- **Streak** with freeze tokens (exact day-serial math), **XP/levels** (5/10/15/20 per
  grade; 500 then +200/level), **3 daily missions**, **5 achievements** (FIRST_REVIEW,
  STREAK_7/30, REVIEWS_100, EASY_STREAK), and a **session-summary** screen.
- Flame emoji → `*` (pgf had no emoji glyph). Extended `player.json` with mission fields.
  Streak persists only after the first graded card (anti-farm).

**Patch pass:** fixed EASY_STREAK being lost on app-exit; switched `player.json` to a
**batched save every 10 grades** (session-ends always flush).

---

## Phase 8 — Neo-brutalist UI, images, resume
A full visual rewrite, retiring the pgf font entirely.

- **Fonts:** `font.h` — raw `FT_Face` + glyph cache (flat array + overflow table),
  Space Grotesk Bold/SemiBold packaged into the vpk. Recolors a cached white+alpha glyph
  via tint.
- **Primitives:** `draw.h` — cream + halftone dot grid, hard-shadow bordered rects, exact
  neo-brutalist palette. (Dot grid uses 2×2 **rects**, not `fill_circle`, which crashed by
  exhausting vita2d's vertex pool — diagnosed from a decoded coredump.)
- **Picker:** header bar, hard-shadow deck cards (Hot Red selection), Vivid Yellow
  missions panel.
- **Reviewer:** HUD (streak / level + Soft-Violet XP bar / card number), three card layouts
  (short-centered, long-wrapped, 60/40 image split), and an on-screen **grade-button row**
  (D-pad selects, X confirms) replacing the X/O/□/△ mapping.
- **Images:** parse `<img>` from front HTML → load `media/<hash>` into a **32-entry LRU** →
  `[IMAGE]` placeholder on miss.
- **Resume:** `progress.json` written on each card flip; resume dialog on relaunch; kept on
  back-out, cleared on deck-complete / start-over.

---

## Quality-of-life rounds
- **Navigation:** **O** returns to the selector, **O in the picker** opens an exit-confirm
  dialog, the **PS button** is the app exit (like stock apps). Fixed held-button "bleed"
  across screens by priming input on entry.
- **Special characters:** added **UTF-8 decoding** to the font renderer — accented letters
  (ñ/é/ü/ä…) were rendering as `?`-boxes because it walked bytes, not codepoints.
- **Branding:** app renamed **VitaDeck**; tile = `ankicovertile.png` (→ 128×128 icon0);
  LiveArea background = `ankibgsplash.png` (→ 840×500). **Install error 0x8010113D** solved
  — the Vita rejects 32-bit RGBA LiveArea PNGs; quantized all to **8-bit palette** with
  Pillow.
- **Reset:** **Triangle** on the main menu → confirm dialog → wipes the gamification profile
  (`player.json`); review logs are deliberately preserved.
- **Touchscreen:** tap a deck to launch, tap to flip, tap a grade button, tap dialog buttons
  / summary screens (coords scaled 1920×1088 → 960×544, tap edge-detected).

---

## Desktop patches — Anki's new `.anki21b` format
The exporter now reads the modern Anki ≥2.1.50 package directly (it previously errored and
told the user to re-export legacy). This was far more than "just decompress":
- **zstd decompression** of `collection.anki21b` via `zstandard` (added to a new
  `requirements.txt` for the user's *default* Python — explicitly **not** the importer's
  Anki venv).
- **New relational schema (`col.ver 18`):** `col.models`/`col.decks` are empty; data lives
  in `notetypes` / `fields` / `templates` / `decks` tables with **protobuf `config` blobs**.
  A tiny protobuf reader rebuilds the legacy `models`/`decks` dicts (template qfmt/afmt,
  notetype cloze-kind/css — field numbers read from the **real blobs**, not a compiled
  `.proto`). Registers Anki's custom **`unicase` collation** to query those tables; converts
  new-schema deck names (`\x1f` separators) back to `::`.
- The **media map** is itself a zstd-compressed protobuf `MediaEntries` (not JSON) — parsed
  so images still resolve. DAT Bootcamp exports cleanly: 377 cards, 316 media, clozes intact.

## Reviewer — tables, code blocks, scrolling
- **Tables → readable text (exporter side):** `<table>` was stripped by the exporter before
  the Vita ever saw it, so the fix lives in `sanitize_html` — it converts to a
  `TH-UPPERCASE | cell | cell` grid with a `---` divider, using literal `\n` (the Vita drops
  `<br>` as a tag). Forward-looking: real decks have almost no tables.
- **`<pre>` code blocks (both sides):** exporter wraps them in `[PRE]…[/PRE]` (newlines
  preserved); the reviewer renders each as a **bordered 18px box** inset in the card (box
  height includes the last line so the border doesn't clip it).
- **Left-stick scroll:** long answers now scroll, clipped to the card region
  (`vita2d_set_clip_rectangle`) with ▲/▼ affordances, clamped to the measured content height
  so it reaches the true end.
- **Picker scroll + wrap:** left stick scrolls the deck list (held = repeat); D-pad **and**
  stick wrap top↔bottom.
- **Crash fix:** the `[PRE]` renderer's two 8KB scratch buffers were moved off the stack
  (`static`) after a decoded coredump showed a **stack overflow surfacing inside freetype's
  rasterizer** (`run_reviewer`'s frame + freetype's pool left no room for them).

## Audio — SFX + streaming BGM (`audio.h`)
A small mixer on one `SceAudioOut` port (48kHz stereo) fed by a dedicated thread; 4 SFX
voices summed per grain. Fails safe to silence if a clip is missing.
- **SFX** (raw 48k stereo, packaged in the vpk): **NM Live** tick on deck-list movement,
  **Utopia** whoosh on forward/continue/confirm (deck select, resume-continue, dialog YES),
  **Vinny** on back/no/cancel (reviewer O, start-over, dialog NO).
- **BGM** streams from the SD card (too big for the vpk) as per-context **playlists** that
  loop and advance on track end: **menu** = Jeff Buckley → Childish Gambino *3005* at full
  volume; **reviewer** = *9-3-25* → *weeknd outro* at reduced volume. Switching context fades
  the volume and swaps the playlist.
- **Static fix:** the mixer + BGM-producer threads are **pinned to the same CPU core** — on
  different cores the Vita's weak memory ordering exposed half-written ring samples (audible
  static). Songs converted to mono 48k raw via a bundled ffmpeg (`imageio-ffmpeg`).
- **Audio hardening pass (2026-06-11):** `audio.h` now treats same-core pinning as a
  performance choice, not the correctness mechanism. The BGM ring uses acquire/release
  SPSC atomics, a power-of-two 65,536-sample mono buffer, batched producer publication,
  and mutex-serialized ring flushes on playlist switches so stale menu/reviewer audio
  does not leak across contexts.
- **Failure-path cleanup:** app startup now checks `audio::init()` before loading SFX,
  starting BGM, or calling `bgm_play`; failed port, mutex, or thread creation paths clean
  up instead of leaving half-initialized audio state.
- **Telemetry added:** counters now track underrun samples, producer waits, overflow
  samples, clipped samples, voice steals, playlist switches, open failures, empty reads,
  init failures, max SD read latency, and ring low/high watermarks.
- **Quality pass:** SFX and BGM have headroom constants, the mixer uses a lightweight
  fixed-point soft limiter instead of only hard clipping, and SFX voice stealing now
  cross-ramps the stolen voice out while fading the new voice in to reduce clicks.
- **Current deployed VPK:** rebuilt through MSYS2 on 2026-06-11 and copied to
  `D:\data\vitadeck\vitadeck_p3.vpk`; the sideloaded BGM files were already present as
  `menu0.raw`, `menu1.raw`, `rev0.raw`, and `rev1.raw`.

---

## Format-version gate
The exporter already wrote `"format_version": 1`; this added the reader side. `main.cpp`
defines `VITADECK_FORMAT_VERSION 1`, `scan_decks` parses each deck's `format_version`, and a
gate runs **after deck selection**: absent → assume v1 + log warning; equal / older-within-
tolerance → proceed; **newer → a blocking "DECK FORMAT TOO NEW" red screen** → back to the
picker (never crash/silently proceed). The additive-only (minor) vs. altering-or-removing
(major) contract is documented in `FORMAT.md`. Verified on hardware with a `format_version:99`
trap manifest, then restored.

## Phase 9 — Per-deck statistics screen
`SELECT` on the picker opens a per-deck stats screen (`show_stats`).
- **Data:** streams the deck's `reviews.jsonl` line-by-line (never fully loaded); maps each
  `reviewed_at` to a local day via `ts / 86400` — the same day-serial space as
  `date_serial()`/today, per the local-as-if-UTC contract.
- **Layout (neo-brutalist):** header (`< PREV` / `NEXT >` + deck name + `STATS`); four tiled
  summary cards — STUDIED TODAY, TIME TODAY, AVG PACE, RETENTION (good+easy over the 90-day
  window); a 13×7 **90-day activity heatmap** with a 5-tier color ramp (cream → faint violet
  → violet → yellow → red), month + M/W/F labels, and a thick border on today's cell.
- **Navigation:** L/R bumpers cycle decks (wrapping); **D-pad and left stick** scrub a cyan
  day-cursor through the grid with a `MON DD: N REVIEWS` readout; O/SELECT dismiss and the
  picker keeps its original highlight.
- **No-reviews state:** decks with an empty/absent `reviews.jsonl` show a "NO REVIEWS YET" card.
- **Polish:** the picker footer advertises `SELECT=STATS` and the Triangle-reset hint is now a
  drawn **△ icon** (scanline rectangles); every day in the window draws a visible box (blank
  for 0 reviews); a rotating **"DID YOU KNOW?"** fun-fact card fills the bottom, subject-matched
  to the deck (calculus / genetics / MCAT / Linux / Spanish / general study).
- **GPU crash fixed:** the first triangle-icon attempt used `vita2d_draw_array`, which
  **doesn't rebind the color shader** — drawn right after text, the textured glyph shader was
  still active and misread the color vertices (`GPUCRASH` coredump). Replaced with `draw_rect`
  scanlines.

---

## Media pipeline & the image-rendering saga
Stress-testing image-heavy med decks (MileDowns MCAT, Multivariable Calculus) surfaced a chain
of GPU faults, each decoded from `psp2dmp` coredumps (gunzip → ELF core → symbolicate the
faulting PC against the unstripped ELF):

- **Progressive JPEGs → baseline.** vita2d's JPEG loader yields a null/garbage texture for
  progressive JPEGs — a missing image or a **GPU crash on draw** (MileDowns alone had 288).
  `exporter.py` now re-encodes JPEGs to **baseline** via Pillow (added to `requirements.txt`),
  and all on-device media was re-encoded in place.
- **GIF/WebP → PNG.** vita2d can't decode GIF/WebP (they showed `[IMAGE]`). The exporter now
  converts them to PNG (first frame) and the `<img>` refs follow; on-device decks were patched
  in place (convert files + rewrite each `cards.sqlite`).
- **Size + stride caps.** Images are capped to **≤512px with dimensions aligned to ×16**
  (`_MAX_IMG_PX` / `_align16`) to bound GPU texture memory and keep the draw near-1:1.
- **The hard wall — image draws GPU-fault under stress.** A clean isolation chain (no-images →
  stable; load-but-don't-draw → stable; load+draw → crash) proved the fault is specifically
  **`vita2d_draw_texture_scale` of decoded image textures under rapid flipping**. It survived
  *every* mitigation tried at the time — small/aligned textures, near-1:1 scale, GPU-idle
  `wait_rendering_done`, and a never-free cache mirroring the stress-proof glyph path — so it was
  provisionally concluded to be a vita2d/GXM limit and a `[IMAGE]` placeholder shipped.
- **First hypothesis — texture *format* — TESTED ON HARDWARE AND DISPROVEN.** vita2d's JPEG
  loader returns 24-bit textures (`SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR` color / `U8_R111` gray,
  read from `xerpi/libvita2d` vs this SDK's `psp2/gxm.h`), so `img_to_rgba32` was added to
  re-wrap every decode into 32-bit `A8B8G8R8` — byte-identical in kind to the stress-proof glyph
  textures. It **still GPU-crashed** (fresh `GPUCRASH` coredump). This *eliminated* format (and,
  with the earlier never-free result, free-timing and size-alone) as the cause. The 32-bit
  conversion is kept anyway (cheap, correct hygiene, and gives us a texture we own to configure).
- **Actual root cause — texture WRAP mode on a scaled NPOT draw.** The only remaining difference
  between the stable glyph path and the crashing image path: glyphs draw **1:1**, images draw
  **scaled**. `sceGxmTextureInitLinear` defaults the U/V address mode to **REPEAT** (`gxm.h:901`,
  value 0) and vita2d **never** overrides it. A scaled, linear-filtered draw interpolates the
  last row/column against the *wrapped opposite edge* — invalid for **non-power-of-two** linear
  textures → GPU MMU fault. Glyphs are also NPOT but draw 1:1, so they never sample across the
  edge and never fault. The exporter's ×16 alignment does **not** make dimensions power-of-two,
  so it never helped, and "near-1:1 scale" still interpolates at the edge. **Wrap mode was never
  on the prior mitigation list.**
- **Fix — CONFIRMED WORKING on hardware.** `img_to_rgba32` sets `SCE_GXM_TEXTURE_ADDR_CLAMP`
  on both U/V axes (valid for NPOT; `gxm.h:903`) plus explicit LINEAR min/mag filters, via
  `sceGxmTextureSet*` on the public `tex->gxm_tex`. Images now render under the full 50-card
  MCAT flip stress test with no GPU fault. The retained 32-bit conversion and 32-entry LRU
  (evict-on-full while the GPU is idle) ride along. The `VD_DIAG_NO_IMAGES`/`VD_DIAG_LOAD_ONLY`
  diagnostic flags have been removed now that the path is proven; a null decode (missing file /
  unknown format) still falls back to the `[IMAGE]` placeholder.
- **Stack-overflow fix.** Raising the image cache for the never-free experiment pushed
  `run_reviewer`'s big locals (a `Card` + two 8 KB text buffers + the cache) past the small
  main-thread stack, crashing inside freetype's rasterizer (`gray_convert_glyph`). Fixed by
  moving those buffers off the stack (`static`).

## Exit-path GPU teardown + input-bleed fixes (2026-06-11)
Two bugs that only showed up when *leaving* an image-heavy deck and on the picker; both fixed
and confirmed on hardware.

- **Image-deck exit: crash → hang → fixed.** Pressing **O** to leave the MileDowns MCAT deck
  crashed instantly (text-only decks were unaffected). This is the *same* hazard as the draw
  saga — touching GPU memory the pipeline still references — but on the **teardown** path:
  `run_reviewer`'s loop exits and falls through to `imgcache.freeall()` (the 32-entry image
  LRU). A first fix added `vita2d_wait_rendering_done()` at the top of the O handler; that
  stopped the crash but then **hung**, frozen on the last reviewer card with the PS button
  dead — while the **menu BGM was already playing**. Because the audio producer can only
  advance *within* a playlist (never switch playlists on its own), that proved `run_reviewer`
  had already returned and the stall was the **picker's first `sceGxmBeginScene`** waiting on a
  display buffer that was never released. **Root cause:** `vita2d_wait_rendering_done()` is only
  `sceGxmFinish` — it drains *rendering* but **not the display flip queue**; the last
  `swap_buffers()` leaves a flip pending that still owns a display buffer, and freeing the
  texture CDRAM with it outstanding wedges the pipeline. **Fix (HW-confirmed):** also call
  `sceGxmDisplayQueueFinish()` — fully idling the GPU *and* draining the flip queue — right
  before `imgcache.freeall()`, at the single freeall site so it covers **every** exit path (O,
  deck-complete, session-summary), not just the O handler. GXM internals are inferred from
  `gxm.h` + `vita2d_fini`'s teardown order (no vita2d source in this SDK); the fix itself is
  hardware-verified.
- **Picker O-bleed.** On the picker, **Triangle** → reset-confirm dialog, then **O** to cancel
  correctly dismissed it but **immediately reopened the exit-confirm dialog** on the same frame.
  The picker's `pad` isn't updated while the dialog runs, so the next frame's edge-detect
  (`pad.buttons & ~prev.buttons`) read the still-held O as a fresh press. **Fix:** re-prime `pad`
  via `sceCtrlPeekBufferPositive` right after the reset dialog returns — the identical pattern
  already used for the SELECT/stats screen. The O/exit handler needs no fix (its entry `pad`
  already holds O, so the bleed is unique to Triangle, where the entry button differs from the
  cancel button). Same class as the earlier held-button "bleed" fixes.

## Audio expansion
- The **menu playlist grew to 9 tracks** (`menu0..menu8.raw`, looping; `BGM_MAX` raised to 12).
- **L / R skip previous / next track** in the main menu (`audio::bgm_skip`, handled in the BGM
  producer with a ring flush so the new song starts instantly); auto-advance on track end is
  unchanged, and the control is shown in the picker footer.

---

## Known limitation
- **Image rendering now works** (see the saga above): the GPU fault was a texture WRAP-mode bug
  on scaled NPOT draws, fixed by forcing `SCE_GXM_TEXTURE_ADDR_CLAMP`. Confirmed on hardware
  under the 50-card MCAT stress test. Text, cloze, gamification, stats, audio, and resume are all
  fully functional too.
- **LiveArea splash background doesn't display** (accepted). The packaging is byte-correct
  against the working SDK sample (valid `a1` template, 8-bit palette PNGs, `content-rev`
  bumped) — the most likely cause is the Vita's LiveArea cache not refreshing on
  install-over; a full delete+reinstall *might* fix it, but it's cosmetic. The bubble icon
  and "VitaDeck" name apply correctly.

---

## Files produced / evolved
- Desktop: `importer.py`, `exporter.py`, `FORMAT.md`, `requirements.txt` (zstandard)
- Vita app (`vita_app2/`): `main.cpp` (the app), `font.h`, `draw.h`, `audio.h`,
  `test_primitives.cpp` (Step-1 test vpk), `CMakeLists.txt`,
  `sce_sys/` (icon + LiveArea), `assets/fonts/`, `assets/audio/` (SFX in vpk)
- SD-card data (`ux0:data/vitadeck/`): the `.vitadeck` decks, `player.json`, per-deck
  `progress.json`/`reviews.jsonl`, and the BGM streams `menu0..menu8.raw` (menu) +
  `rev0/rev1.raw` (reviewer). Audio (copyrighted songs/SFX) and decks are gitignored.
- Build: MSYS2 + vitasdk (`C:\msys64\usr\local\vitasdk`) → `vitadeck_p3.vpk`
  (title **VDCKREV01**), staged to the Vita each round.

**Confirmed working on hardware**, including image cards (the GPU-draw fault is fixed — see the
image-rendering saga) and **leaving** image-heavy decks cleanly (the exit-teardown crash/hang is
fixed — see the exit-path section). One cosmetic limitation remains: the LiveArea splash doesn't
display. The one open thread outside the Vita app is the optional live `importer.py` run into the
real Anki collection.

---

## Build & deploy quick reference
```powershell
# Build by invoking MSYS2 bash; vitasdk is under C:\msys64\usr\local\vitasdk.
C:/msys64/usr/bin/bash.exe -lc "export VITASDK=/usr/local/vitasdk; \
  export PATH=\$VITASDK/bin:\$PATH; \
  cd '/c/Users/onwus/Downloads/anki psvita project/vita_app2/build'; \
  cmake --build ."
# Output: vita_app2/build/vitadeck_p3.vpk  (title VDCKREV01)
# If the Vita storage is mounted as D:, stage the installer here:
Copy-Item -LiteralPath 'C:\Users\onwus\Downloads\anki psvita project\vita_app2\build\vitadeck_p3.vpk' `
  -Destination 'D:\data\vitadeck\vitadeck_p3.vpk' -Force
```
Note: after editing `sce_sys` PNGs or fonts, run `cmake .` first; CMake stages those
assets to a space-free dir only at *configure* time (the project path has spaces, which
`vita_create_vpk`'s FILE directive doesn't escape).
